/* SPDX-License-Identifier: GPL-2.0-only */
#include "media.h"
#include "base64.h"
#include "config.h"
#include "fs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool
mime_valid(const char *mime)
{
    size_t slash = 0, len = mime ? strlen(mime) : 0;
    if (!len || len > 96u || mime[0] == '/' || mime[len - 1u] == '/')
        return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)mime[i];
        if (c == '/') ++slash;
        else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '-' || c == '+' || c == '.')) return false;
    }
    return slash == 1u;
}

const char *
snag_media_mime(const char *path)
{
    static const struct { const char *suffix, *mime; } formats[] = {
        {"pdf", "application/pdf"}, {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
        {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {"odt", "application/vnd.oasis.opendocument.text"}, {"odp", "application/vnd.oasis.opendocument.presentation"},
        {"ods", "application/vnd.oasis.opendocument.spreadsheet"},
        {"wav", "audio/wav"}, {"mp3", "audio/mpeg"}, {"m4a", "audio/mp4"}, {"ogg", "audio/ogg"}, {"flac", "audio/flac"},
        {"mp4", "video/mp4"}, {"mov", "video/quicktime"}, {"mkv", "video/x-matroska"}, {"webm", "video/webm"},
        {"csv", "text/csv"}, {"tsv", "text/tab-separated-values"}, {"txt", "text/plain"}, {"md", "text/markdown"}
    };
    const char *ext = path ? strrchr(path, '.') : NULL;
    if (!ext) return NULL;
    char lower[16]; size_t n = strlen(++ext);
    if (n >= sizeof(lower)) return NULL;
    for (size_t i = 0; i <= n; ++i) lower[i] = ext[i] >= 'A' && ext[i] <= 'Z' ? ext[i] + ('a' - 'A') : ext[i];
    for (size_t i = 0; i < sizeof(formats)/sizeof(formats[0]); ++i)
        if (!strcmp(lower, formats[i].suffix)) return formats[i].mime;
    return NULL;
}

bool
snag_media_valid(const json_t *asset)
{
    const char *id = snag_json_string(asset, "id");
    const char *hash = snag_json_string(asset, "sha256");
    uint64_t bytes;
    return snag_json_exact_keys(asset,"id mime_type bytes sha256") && id && hash &&
        snag_hex_is_lower(id, SNAG_ID_HEX_LEN) &&
        snag_hex_is_lower(hash, SNAG_SHA256_HEX_LEN) &&
        mime_valid(snag_json_string(asset, "mime_type")) &&
        snag_json_integer_u64(asset, "bytes", &bytes) == 0 &&
        bytes > 0u && bytes <= SNAG_MEDIA_FILE_MAX;
}

static int
private_directory(int session_fd)
{
    struct snag_file_privacy privacy;
    int fd = snag_open_read_at(session_fd, "media", true);
    if (fd < 0) return -1;
    if (snag_fd_privacy(fd, &privacy) < 0 ||
        !privacy.effective_owner || !privacy.private_access) {
        close(fd);
        errno = EACCES;
        return -1;
    }
    return fd;
}

/* Only the named private Office scratch tree is traversed. Never follow links.
 * During import, files may disappear between enumeration and stat/open. */
static int
work_tree(int fd,unsigned int depth,unsigned int *entries,uint64_t *bytes,bool remove)
{
    if(depth>32u) {errno=EFBIG;return -1;}
    int scan=snag_open_read_at(fd,".",true);
    struct snag_directory *dir=scan>=0?snag_directory_open(scan):NULL;
    if(!dir) {if(scan>=0)close(scan);return -1;}
    int rc=-1;const char *name;
    for(;;) {
        errno=0;name=snag_directory_next(dir);
        if(!name) {if(!errno)rc=0;break;}
        if(!strcmp(name,".") || !strcmp(name,".."))continue;
        if(++*entries>(remove?16384u:4096u)) {errno=EFBIG;break;}
        snag_file_info st;
        if(snag_lstat_at(fd,name,&st)<0) {if(errno==ENOENT)continue;break;}
        bool directory=S_ISDIR(st.st_mode);
        if(directory) {
            int child=snag_open_read_at(fd,name,true);
            if(child<0) {if(errno==ENOENT)continue;break;}
            int nested=work_tree(child,depth+1u,entries,bytes,remove);
            close(child);if(nested)break;
        } else if(!remove) {
            if(!S_ISREG(st.st_mode) || st.st_nlink!=1u || st.st_size<0) {errno=EINVAL;break;}
            if((uint64_t)st.st_size>SNAG_MEDIA_WORK_MAX-*bytes) {errno=EFBIG;break;}
            *bytes+=(uint64_t)st.st_size;
        }
        if(remove && snag_unlink_at(fd,name,directory)<0 && errno!=ENOENT)break;
    }
    int saved=errno;
    if(snag_directory_close(dir)<0 && !rc)return -1;
    errno=saved;return rc;
}

int
snag_media_work_check(int fd)
{
    unsigned int entries=0;uint64_t bytes=0;
    return work_tree(fd,0u,&entries,&bytes,false);
}

int
snag_media_work_remove(int session_fd,char *error,size_t size)
{
    int fd=snag_open_read_at(session_fd,SNAG_MEDIA_WORK_NAME,true),rc=-1;
    if(fd<0) {if(errno==ENOENT)return 0;goto failed;}
    struct snag_file_privacy privacy;
    struct snag_directory_lock lock={.fd=-1};
    if(snag_fd_privacy(fd,&privacy)<0 || !privacy.effective_owner || !privacy.private_access) {
        errno=EACCES;goto closed;
    }
    if(snag_directory_lock_acquire(fd,&lock)<0) {
        if(errno==EAGAIN || errno==EWOULDBLOCK)rc=1;
        goto closed;
    }
    unsigned int entries=0;uint64_t bytes=0;
    rc=work_tree(fd,0u,&entries,&bytes,true);
    if(!rc)rc=snag_unlink_at(session_fd,SNAG_MEDIA_WORK_NAME,true);
    if(snag_directory_lock_release(&lock)<0 && !rc)rc=-1;
closed:
    close(fd);
    if(!rc)return snag_sync_dir(session_fd);
failed:
    snag_errorf(error,size,rc==1?"Office worker still owns its temporary profile; retry after it exits":
        "Cannot remove Office temporary profile: %s",strerror(errno));
    return rc;
}

int
snag_media_work_open(int session_fd,char *error,size_t size)
{
    if(snag_media_work_remove(session_fd,error,size)!=0)return -1;
    if(snag_mkdir_private_at(session_fd,SNAG_MEDIA_WORK_NAME)<0)goto failed;
    int fd=snag_open_read_at(session_fd,SNAG_MEDIA_WORK_NAME,true);
    if(fd>=0)return fd;
failed:
    snag_errorf(error,size,"Cannot create Office temporary directory: %s",strerror(errno));
    return -1;
}

static bool
unchanged(const snag_file_info *a, const snag_file_info *b)
{
    if (a->st_dev != b->st_dev || a->st_ino != b->st_ino ||
        a->st_size != b->st_size || a->st_mtime != b->st_mtime ||
        !S_ISREG(b->st_mode)) return false;
#ifndef _WIN32
    if (a->st_ctime != b->st_ctime) return false;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    if (a->st_mtimespec.tv_nsec != b->st_mtimespec.tv_nsec ||
        a->st_ctimespec.tv_nsec != b->st_ctimespec.tv_nsec) return false;
#else
    if (a->st_mtim.tv_nsec != b->st_mtim.tv_nsec ||
        a->st_ctim.tv_nsec != b->st_ctim.tv_nsec) return false;
#endif
#endif
    return true;
}

int
snag_media_snapshot(int session_fd, const char *workspace, const char *path,
                    const char *mime, size_t max_bytes,
                    int (*pump)(void *, unsigned int), void *opaque,
                    json_t **asset, char *error, size_t error_size)
{
    char id[SNAG_ID_HEX_LEN + 1u], hash[SNAG_SHA256_HEX_LEN + 1u];
    unsigned char block[64u * 1024u];
    struct snag_sha256 digest;
    snag_file_info before, after;
    int input = -1, output = -1, dir = -1, rc = -1, saved;
    uint64_t copied = 0;
    bool created = false;
    *asset = NULL;
    if (!workspace || !path || (mime && !mime_valid(mime)) || !max_bytes ||
        max_bytes > SNAG_MEDIA_FILE_MAX) { errno = EINVAL; goto out; }
    if (pump && pump(opaque, 0u)) { errno = ECANCELED; goto out; }
    input = snag_open_inspect_path(workspace, path);
    if (input < 0 || snag_fstat(input, &before) < 0) goto out;
    if (!S_ISREG(before.st_mode) || before.st_size <= 0) { errno = EINVAL; goto out; }
    if ((uint64_t)before.st_size > max_bytes) { errno = EFBIG; goto out; }
    if (!mime) {
        unsigned char header[12];
        ssize_t n = snag_pread(input, header, sizeof(header), 0);
        if (n >= 8 && !memcmp(header, "\211PNG\r\n\032\n", 8u)) mime = "image/png";
        else if (n >= 3 && header[0] == 0xffu && header[1] == 0xd8u && header[2] == 0xffu)
            mime = "image/jpeg";
        else if (n >= 6 && (!memcmp(header, "GIF87a", 6u) || !memcmp(header, "GIF89a", 6u))) mime = "image/gif";
        else if (n == 12 && !memcmp(header, "RIFF", 4u) && !memcmp(header + 8u, "WEBP", 4u)) mime = "image/webp";
        else if (n >= 2 && header[0] == 'B' && header[1] == 'M') mime = "image/bmp";
        else if (n >= 4 && (!memcmp(header, "II\052\0", 4u) || !memcmp(header, "MM\0\052", 4u))) mime = "image/tiff";
        else {
            errno = EINVAL;
            snag_errorf(error, error_size, "Expected PNG, JPEG, GIF, WebP, BMP or TIFF image bytes.");
            close(input);
            return -1;
        }
    }
    if (snag_mkdir_private_at(session_fd, "media") < 0 && errno != EEXIST) goto out;
    dir = private_directory(session_fd);
    if (dir < 0 || snag_random_id(id) < 0) goto out;
    /* Exclusive random name is private staging until fsync and journal acceptance.
     * No rename-overwrite race, mutable hard link, or second staging namespace. */
    output = snag_create_private_at(dir, id, true);
    if (output < 0) goto out;
    created = true;
    snag_sha256_init(&digest);
    for (;;) {
        ssize_t n;
        if (pump && pump(opaque, 0u)) { errno = ECANCELED; goto out; }
        n = read(input, block, sizeof(block));
        if (n < 0) { if (errno == EINTR) continue; goto out; }
        if (!n) break;
        if ((uint64_t)n > (uint64_t)before.st_size - copied) { errno = ESTALE; goto out; }
        if (snag_write_full(output, block, (size_t)n) < 0) goto out;
        snag_sha256_update(&digest, block, (size_t)n);
        copied += (uint64_t)n;
    }
    if (snag_fstat(input, &after) < 0) goto out;
    if (copied != (uint64_t)before.st_size || !unchanged(&before, &after)) {
        errno = ESTALE;
        goto out;
    }
    snag_sha256_final_hex(&digest, hash);
    if (snag_fsync(output) < 0) goto out;
    saved = close(output); output = -1;
    if (saved < 0 || snag_sync_dir(dir) < 0 || snag_sync_dir(session_fd) < 0) goto out;
    *asset = json_pack("{s:s,s:s,s:I,s:s}", "id", id, "mime_type", mime,
                       "bytes", (json_int_t)copied, "sha256", hash);
    if (!*asset) { errno = ENOMEM; goto out; }
    rc = 0;
out:
    saved = errno;
    if (input >= 0) close(input);
    if (output >= 0) close(output);
    if (rc < 0 && created) (void)snag_unlink_at(dir, id, false);
    if (dir >= 0) close(dir);
    if (rc < 0 && error_size)
        (void)snprintf(error, error_size, "Cannot snapshot media: %s", strerror(saved));
    errno = saved;
    return rc;
}

static int
media_read(int session_fd, const json_t *asset, snag_bytes_sink sink, void *sink_opaque,
            int (*pump)(void *, unsigned int), void *opaque,
            char *error, size_t error_size)
{
    struct snag_file_privacy privacy;
    snag_file_info before, after;
    struct snag_sha256 digest;
    char hash[SNAG_SHA256_HEX_LEN + 1u];
    unsigned char block[3072];
    uint64_t size, copied = 0;
    int dir = -1, fd = -1, rc = -1, saved;
    if (!snag_media_valid(asset)) { errno = EINVAL; goto out; }
    (void)snag_json_integer_u64(asset, "bytes", &size);
    dir = private_directory(session_fd);
    if (dir < 0) goto out;
    fd = snag_open_inspect_at(dir, snag_json_string(asset, "id"));
    if (fd < 0 || snag_fstat(fd, &before) < 0 || snag_fd_privacy(fd, &privacy) < 0) goto out;
    if (!S_ISREG(before.st_mode) || before.st_nlink != 1u ||
        !privacy.effective_owner || !privacy.private_access || before.st_size < 0 ||
        (uint64_t)before.st_size != size) { errno = EINVAL; goto out; }
    snag_sha256_init(&digest);
    for (;;) {
        if (pump && pump(opaque, 0u)) { errno = ECANCELED; goto out; }
        ssize_t n = read(fd, block, sizeof(block));
        if (n < 0) { if (errno == EINTR) continue; goto out; }
        if (!n) break;
        if ((uint64_t)n > size - copied) { errno = ESTALE; goto out; }
        if (sink && sink(sink_opaque, block, (size_t)n) != 0) goto out;
        snag_sha256_update(&digest, block, (size_t)n);
        copied += (uint64_t)n;
    }
    if (snag_fstat(fd, &after) < 0) goto out;
    snag_sha256_final_hex(&digest, hash);
    if (copied != size || !unchanged(&before, &after) ||
        strcmp(hash, snag_json_string(asset, "sha256"))) { errno = ESTALE; goto out; }
    rc = 0;
out:
    saved = errno;
    if (fd >= 0) close(fd);
    if (dir >= 0) close(dir);
    if (rc < 0) {
        if (error_size) (void)snprintf(error, error_size, "Cannot read retained media: %s", strerror(saved));
    }
    errno = saved;
    return rc;
}

static int
media_buffer(void *opaque, const unsigned char *data, size_t len)
{
    return snag_buf_append(opaque, data, len);
}

int
snag_media_read(int session_fd, const json_t *asset, struct snag_buf *bytes,
                char *error, size_t error_size)
{
    size_t original = bytes->len;
    int rc = media_read(session_fd, asset, media_buffer, bytes, NULL, NULL, error, error_size);
    if (rc < 0) bytes->len = original;
    return rc;
}

int
snag_media_verify(int session_fd, const json_t *asset,
                  int (*pump)(void *, unsigned int), void *opaque, char *error, size_t error_size)
{
    return media_read(session_fd, asset, NULL, NULL, pump, opaque, error, error_size);
}

struct media_encoder { struct snag_base64_stream stream; struct snag_buf *output; };

static int
media_encode(void *opaque, const unsigned char *data, size_t len)
{
    struct media_encoder *encoder = opaque;
    return snag_base64_write(&encoder->stream, data, len, media_buffer, encoder->output);
}

static int
media_base64(int session_fd, const json_t *asset, struct snag_buf *out,
             char *error, size_t error_size)
{
    struct media_encoder encoder = {.output = out};
    size_t original = out->len;
    int rc = media_read(session_fd, asset, media_encode, &encoder, NULL, NULL, error, error_size);
    if (!rc) rc = snag_base64_finish(&encoder.stream, media_buffer, out);
    if (rc < 0) out->len = original;
    return rc;
}

bool
snag_media_content_valid(const json_t *content)
{
    if (!json_is_array(content) || !json_array_size(content))
        return false;
    uint64_t bytes = 0;
    for (size_t i = 0; i < json_array_size(content); ++i) {
        json_t *part = json_array_get(content, i);
        const char *type = snag_json_string(part, "type");
        if (!type) return false;
        if (!strcmp(type, "input_text")) {
            if (!snag_json_exact_keys(part,"type text") || !snag_json_string(part, "text")) return false;
        } else if (!strcmp(type, "file")) {
            if (!snag_json_exact_keys(part,"type asset") || !snag_media_valid(json_object_get(part, "asset"))) return false;
        } else if (!strcmp(type, "input_image")) {
            json_t *source = json_object_get(part, "source");
            if (source && (!snag_media_valid(source) || !snag_json_string(part, "note") ||
                strlen(snag_json_string(part, "note")) > 1024u)) return false;
            json_t *asset = json_object_get(part, "asset");
            uint64_t size;
            const char *mime;
            if (!snag_json_exact_keys(part,source?"type asset source note":"type asset") || !snag_media_valid(asset)) return false;
            mime = snag_json_string(asset, "mime_type");
            if (strcmp(mime, "image/png") && strcmp(mime, "image/jpeg")) return false;
            (void)snag_json_integer_u64(asset, "bytes", &size);
            if (size > SNAG_MEDIA_REQUEST_MAX - bytes) return false;
            bytes += size;
        } else return false;
    }
    return true;
}

json_t *
snag_media_content_resolve(int session_fd, const json_t *content, char *error, size_t error_size)
{
    json_t *out = NULL;
    if (!snag_media_content_valid(content)) {
        snag_errorf(error, error_size, "invalid retained media content");
        return NULL;
    }
    out = json_array();
    if (!out) return NULL;
    for (size_t i = 0; i < json_array_size(content); ++i) {
        json_t *part = json_array_get(content, i), *asset = json_object_get(part, "asset");
        if (!asset) {
            if (json_array_append(out, part) < 0) goto fail;
            continue;
        }
        if (!strcmp(snag_json_string(part, "type"), "file")) {
            char label[320];
            (void)snprintf(label, sizeof(label), "Retained file asset:%s (%s, %lld bytes). Reference only; contents are not included in this label. "
                "Use an appropriate inspection tool with path=asset:%s.",
                snag_json_string(asset, "id"), snag_json_string(asset, "mime_type"),
                (long long)json_integer_value(json_object_get(asset, "bytes")), snag_json_string(asset, "id"));
            if (json_array_append_new(out, json_pack("{s:s,s:s}", "type", "input_text", "text", label)) < 0) goto fail;
            continue;
        }
        json_t *source = json_object_get(part, "source");
        if (source) {
            if (snag_media_verify(session_fd, source, NULL, NULL, error, error_size) < 0) goto fail;
            char note[1280];
            (void)snprintf(note, sizeof(note), "Image source asset:%s; normalized asset:%s. %s",
                snag_json_string(source, "id"), snag_json_string(asset, "id"), snag_json_string(part, "note"));
            if (json_array_append_new(out, json_pack("{s:s,s:s}", "type", "input_text", "text", note)) < 0) goto fail;
        }
        struct snag_buf url;
        snag_buf_init(&url, SNAG_MEDIA_REQUEST_MAX / 3u * 4u + 256u);
        int rc = snag_buf_printf(&url, "data:%s;base64,", snag_json_string(asset, "mime_type"));
        if (!rc) rc = media_base64(session_fd, asset, &url, error, error_size);
        if (!rc) rc = snag_buf_terminate(&url);
        if (rc == 0)
            rc = json_array_append_new(out, json_pack("{s:s,s:s,s:s}",
                "type", "input_image", "image_url", (const char *)url.data,
                "detail", "high"));
        snag_buf_free(&url);
        if (rc < 0) goto fail;
    }
    return out;
fail:
    json_decref(out);
    return NULL;
}

json_t *
snag_media_message_content(int session_fd, const char *text, const json_t *content,
                           char *error, size_t error_size)
{
    if (!content) return json_string(text);
    json_t *parts = snag_media_content_resolve(session_fd, content, error, error_size);
    if (!parts) return NULL;
    json_t *out = json_pack("[{s:s,s:s}]", "type", "input_text", "text", text);
    int rc = out ? json_array_extend(out, parts) : -1;
    json_decref(parts);
    if (rc < 0) { json_decref(out); return NULL; }
    return out;
}

/* Inspect only message/tool content positions, not arbitrary tool arguments. */
bool
snag_media_request_has_images(const json_t *request)
{
    const json_t *input = json_is_array(request) ? request : json_object_get(request, "input");
    for (size_t i = 0; i < json_array_size(input); ++i) {
        json_t *item = json_array_get(input, i);
        json_t *content = json_object_get(item, "content");
        if (!content) content = json_object_get(item, "output");
        for (size_t j = 0; j < json_array_size(content); ++j) {
            const char *type = snag_json_string(json_array_get(content, j), "type");
            if (type && !strcmp(type, "input_image")) return true;
        }
    }
    return false;
}

/* Provider-generic image input budget for locally bounded media requests.
 *
 * This client prepares every image at high detail, so the provider resizes it
 * to its own processed-image bound before tokenizing it. The first-party
 * client budgets such an image with one nominal resized-image size instead of
 * a per-model table; it derives a size from the image itself only for
 * original detail, which this client never sends. Apply the same generic
 * budget on every route, so an image-capable provider, endpoint or model
 * needs no operator rule and no per-model constant. A configured
 * image_tokens model-limit still declares a provider-documented per-image
 * ceiling and wins. The provider's own reported input count remains
 * authoritative after each response; see snag_app_measured_input(). */
#define SNAG_MEDIA_RESIZED_IMAGE_BUDGET 7373u

int
snag_media_token_bound(const json_t *request, uint64_t configured_image_tokens,
                       uint64_t *tokens, char *error, size_t size)
{
    uint64_t ceiling = configured_image_tokens ? configured_image_tokens : SNAG_MEDIA_RESIZED_IMAGE_BUDGET;
    json_t *copy = NULL, *input = NULL;
    uint64_t images = 0;
    size_t bytes = 0;
    int rc = -1;
    if (snag_media_request_check(request, error, size) < 0) return -1;
    copy = json_copy((json_t *)request); input = json_array();
    if (!copy || !input || json_object_set(copy, "input", input) < 0) goto out;
    json_t *original = json_object_get(request, "input");
    for (size_t i = 0; i < json_array_size(original); ++i) {
        json_t *item = json_array_get(original, i);
        const char *key = json_object_get(item, "content") ? "content" : "output";
        json_t *parts = json_object_get(item, key);
        if (!json_is_array(parts)) {
            if (json_array_append(input, item) < 0) goto out;
            continue;
        }
        json_t *next = json_copy(item), *text = json_array();
        if (!next || !text || json_object_set(next, key, text) < 0 || json_array_append(input, next) < 0) {
            json_decref(next); json_decref(text); goto out;
        }
        json_decref(next);
        for (size_t j = 0; j < json_array_size(parts); ++j) {
            json_t *part = json_array_get(parts, j);
            const char *type = snag_json_string(part, "type");
            if (type && !strcmp(type, "input_image")) {
                const char *detail = snag_json_string(part, "detail");
                if (!detail || strcmp(detail, "high")) {
                    json_decref(text);
                    snag_errorf(error, size, "Local image budget requires explicit high detail"); goto out;
                }
                ++images;
                /* Shallow copy only metadata; Jansson shares the original data
                 * URL until this reference is removed. Never copy base64. */
                json_t *meta = json_copy(part);
                int added = meta && json_object_del(meta, "image_url") == 0 ? json_array_append(text, meta) : -1;
                json_decref(meta);
                if (added < 0) { json_decref(text); goto out; }
            } else if (json_array_append(text, part) < 0) { json_decref(text); goto out; }
        }
        json_decref(text);
    }
    if (!images || snag_json_digest_bounded(copy, 32u * 1024u * 1024u, NULL, &bytes) < 0) goto out;
    /* Non-image canonical bytes use the existing conservative text bound,
     * with request/item framing reserve. Image tokens are counted separately. */
    *tokens = (uint64_t)bytes + images * ceiling + 1024u + 32u * json_array_size(input);
    rc = 0;
out:
    json_decref(input); json_decref(copy);
    if (rc && error && size && !*error) snag_errorf(error, size, "Cannot budget image request safely");
    return rc;
}

int
snag_media_compaction_prepare(json_t *request, uint64_t *omitted,
                              char *error, size_t error_size)
{
    static const char marker[] =
        "Historical image bytes omitted from the compaction request. The image was already "
        "presented in its original response cycle and remains in durable session media.";
    json_t *input = json_is_array(request) ? request : json_object_get(request, "input");
    uint64_t count = 0;

    if (!json_is_array(input)) goto invalid;
    for (size_t i = 0; i < json_array_size(input); ++i) {
        json_t *item = json_array_get(input, i);
        json_t *parts = json_object_get(item, "content");
        if (!parts) parts = json_object_get(item, "output");
        for (size_t j = 0; j < json_array_size(parts); ++j) {
            json_t *part = json_array_get(parts, j);
            const char *type = snag_json_string(part, "type");
            if (!type || strcmp(type, "input_image")) continue;
            const char *url = snag_json_string(part, "image_url");
            const char *encoded = url ? strstr(url, ";base64,") : NULL;
            if (!encoded || strncmp(url, "data:image/", 11u)) goto invalid;
            encoded += 8u;
            size_t len = strlen(encoded);
            if (!len || len % 4u) goto invalid;
            if (json_array_set_new(parts, j, json_pack("{s:s,s:s}",
                    "type", "input_text", "text", marker)) < 0) goto failed;
            ++count;
        }
    }
    if (omitted) *omitted = count;
    return 0;
invalid:
    snag_errorf(error, error_size, "Compaction input contains invalid image data");
    errno = EINVAL;
    return -1;
failed:
    snag_errorf(error, error_size, "Cannot prepare image history for compaction");
    return -1;
}

int
snag_media_request_check(const json_t *request, char *error, size_t error_size)
{
    json_t *input = json_object_get(request, "input");
    size_t bytes = 0;
    for (size_t i = 0; i < json_array_size(input); ++i) {
        json_t *item = json_array_get(input, i);
        json_t *parts = json_object_get(item, "content");
        if (!parts) parts = json_object_get(item, "output");
        for (size_t j = 0; j < json_array_size(parts); ++j) {
            json_t *part = json_array_get(parts, j);
            const char *type = snag_json_string(part, "type");
            if (!type || strcmp(type, "input_image")) continue;
            const char *url = snag_json_string(part, "image_url");
            const char *encoded = url ? strstr(url, ";base64,") : NULL;
            if (!encoded || strncmp(url, "data:image/", 11u)) goto invalid;
            encoded += 8u;
            size_t len = strlen(encoded);
            if (!len || len % 4u) goto invalid;
            size_t size = len / 4u * 3u - (encoded[len - 1u] == '=') -
                          (encoded[len - 2u] == '=');
            if (size > SNAG_MEDIA_REQUEST_MAX - bytes) goto invalid;
            bytes += size;
        }
    }
    return 0;
invalid:
    snag_errorf(error, error_size, "Image request exceeds 12 MiB, or contains invalid image data.");
    errno = EFBIG;
    return -1;
}

/* Called only by explicit session deletion, before removing its journal. Do not
 * traverse subdirectories or symlinks, and leave unexpected entries for diagnosis. */
int
snag_media_remove(int session_fd, char *error, size_t error_size)
{
    int fd = private_directory(session_fd), saved, rc = -1;
    struct snag_directory *dir;
    const char *name;
    if (fd < 0) {
        if (errno == ENOENT) return 0;
        goto failed;
    }
    dir = snag_directory_open(fd);
    if (!dir) { close(fd); goto failed; }
    for (;;) {
        snag_file_info info;
        errno = 0;
        name = snag_directory_next(dir);
        if (!name) { if (!errno) rc = 0; break; }
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (!snag_hex_is_lower(name, SNAG_ID_HEX_LEN)) {
            errno = EINVAL;
            break;
        }
        if (snag_lstat_at(fd, name, &info) < 0) break;
        if (!S_ISREG(info.st_mode) || info.st_nlink != 1u) { errno = EINVAL; break; }
        if (snag_unlink_at(fd, name, false) < 0) break;
    }
    saved = errno;
    if (snag_directory_close(dir) < 0 && rc == 0) goto failed;
    errno = saved;
    if (rc < 0) goto failed;
    if (snag_unlink_at(session_fd, "media", true) == 0) return snag_sync_dir(session_fd);
failed:
    snag_errorf(error, error_size, "Cannot remove deleted-session media: %s", strerror(errno));
    return -1;
}

int
snag_media_save(int session_fd, const void *bytes, size_t len, const char *mime,
                 json_t **asset, char *error, size_t error_size)
{
    char id[SNAG_ID_HEX_LEN + 1u], hash[SNAG_SHA256_HEX_LEN + 1u];
    int dir = -1, fd = -1, rc = -1, saved;
    bool created = false;
    *asset = NULL;
    if (!len || len > SNAG_MEDIA_REQUEST_MAX || !mime_valid(mime)) { errno = EINVAL; goto out; }
    if (snag_mkdir_private_at(session_fd, "media") < 0 && errno != EEXIST) goto out;
    dir = private_directory(session_fd);
    if (dir < 0 || snag_random_id(id) < 0) goto out;
    fd = snag_create_private_at(dir, id, true);
    if (fd < 0) goto out;
    created = true;
    if (snag_write_full(fd, bytes, len) < 0 || snag_fsync(fd) < 0) goto out;
    saved = close(fd); fd = -1;
    if (saved < 0 || snag_sync_dir(dir) < 0 || snag_sync_dir(session_fd) < 0) goto out;
    snag_sha256_hex(bytes, len, hash);
    *asset = json_pack("{s:s,s:s,s:I,s:s}", "id", id, "mime_type", mime, "bytes", (json_int_t)len, "sha256", hash);
    if (!*asset) { errno = ENOMEM; goto out; }
    rc = 0;
out:
    saved = errno;
    if (fd >= 0) close(fd);
    if (rc < 0 && created) (void)snag_unlink_at(dir, id, false);
    if (dir >= 0) close(dir);
    if (rc < 0) snag_errorf(error, error_size, "Cannot retain derived media: %s", strerror(saved));
    errno = saved;
    return rc;
}
