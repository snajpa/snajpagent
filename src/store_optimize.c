/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_optimize.h"
#include "context.h"
#include "fs.h"
#include "store_binary_import.h"
#include "store_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zstd.h>

static const char *cache_names[] = {"history.idx", "checkpoint.0", "checkpoint.1"};

static int
sync_directory(int fd)
{
    int rc = snag_sync_dir(fd);
    return rc > 0 ? snag_errno(ENOTSUP) : rc;
}

static int
private_file(int directory, const char *name, snag_file_info *info)
{
    int fd = snag_open_read_security_at(directory, name, false);
    if (fd < 0) return -1;
    char error[128];
    int rc = snag_store_verify_private_fd(fd, false, name, error, sizeof(error));
    if (!rc) rc = snag_fstat(fd, info);
    int saved = errno;
    (void)close(fd);
    errno = saved;
    return rc;
}

static int
remove_private_file(int directory, const char *name)
{
    snag_file_info info;
    if (private_file(directory, name, &info) < 0) return errno == ENOENT ? 0 : -1;
    return snag_unlink_at(directory, name, false);
}

/* Keep the stage's hard link until the complete chosen bundle is synchronized. */
static int
install_cache(int stage, const char *from, int directory, const char *to)
{
    snag_file_info info;
    if (private_file(stage, from, &info) < 0) {
        if (errno != ENOENT) return -1;
        return remove_private_file(directory, to);
    }
    if (remove_private_file(directory, ".optimize-install") < 0 ||
        snag_link_at(stage, from, directory, ".optimize-install") < 0 ||
        snag_rename_at(directory, ".optimize-install", directory, to) < 0) return -1;
    return 0;
}

static int
clear_stage(struct snag_session *session, int stage)
{
    const char *names[] = {"journal.bin", "new-journal.bin", "old-journal.bin", "lock"};
    if (remove_private_file(stage, "ready") < 0 || sync_directory(stage) < 0) return -1;
    for (size_t i = 0u; i < sizeof(names) / sizeof(names[0]); ++i)
        if (remove_private_file(stage, names[i]) < 0) return -1;
    for (size_t i = 0u; i < 3u; ++i) {
        char old[32];
        (void)snprintf(old, sizeof(old), "old-%s", cache_names[i]);
        if (remove_private_file(stage, cache_names[i]) < 0 ||
            remove_private_file(stage, old) < 0) return -1;
    }
    int fd = dup(stage);
    if (fd < 0) return -1;
    struct snag_directory *directory = snag_directory_open(fd);
    if (!directory) { (void)close(fd); return -1; }
    const char *name;
    int failed = 0;
    errno = 0;
    while ((name = snag_directory_next(directory))) {
        if (strlen(name) == sizeof(".checkpoint..tmp") - 1u + SNAG_ID_HEX_LEN &&
            !strncmp(name, ".checkpoint.", 12u) &&
            snag_hex_is_lower(name + 12u, SNAG_ID_HEX_LEN) &&
            !strcmp(name + 12u + SNAG_ID_HEX_LEN, ".tmp") &&
            remove_private_file(stage, name) < 0) {
            failed = errno;
            break;
        }
        errno = 0;
    }
    if (!failed) failed = errno;
    if (snag_directory_close(directory) < 0 && !failed) failed = errno;
    if (failed) return snag_errno(failed);
    if (sync_directory(stage) < 0 ||
        snag_unlink_at(session->dir_fd, ".optimize", true) < 0 ||
        sync_directory(session->dir_fd) < 0) return -1;
    return 0;
}

int
snag_store_recover_optimization(struct snag_session *session, char *error, size_t error_size)
{
    if (!session || session->dir_fd < 0 || session->lock_fd < 0)
        return snag_fail(error, error_size, EINVAL, "optimization recovery requires session lock");
    int stage = snag_open_read_security_at(session->dir_fd, ".optimize", true);
    if (stage < 0) return errno == ENOENT ? 0 : -1;
    int rc = -1;
    if (snag_store_verify_private_fd(stage, true, "optimization stage", error, error_size) < 0)
        goto done;
    snag_file_info ready, selected, old, fresh;
    if (private_file(stage, "ready", &ready) < 0) {
        if (errno == ENOENT) rc = clear_stage(session, stage);
        goto done;
    }
    if (private_file(session->dir_fd, "journal.bin", &selected) < 0 ||
        private_file(stage, "old-journal.bin", &old) < 0 ||
        private_file(stage, "new-journal.bin", &fresh) < 0) goto done;
    bool committed = selected.st_dev == fresh.st_dev && selected.st_ino == fresh.st_ino;
    if (!committed && (selected.st_dev != old.st_dev || selected.st_ino != old.st_ino)) {
        snag_fail(error, error_size, ESTALE, "optimization journal selection changed");
        goto done;
    }
    for (size_t i = 0u; i < 3u; ++i) {
        char name[32];
        (void)snprintf(name, sizeof(name), "%s%s", committed ? "" : "old-", cache_names[i]);
        if (install_cache(stage, name, session->dir_fd, cache_names[i]) < 0) goto done;
    }
    if (sync_directory(session->dir_fd) < 0 || clear_stage(session, stage) < 0) goto done;
    rc = 0;
done:
    {
        int saved = errno;
        (void)close(stage);
        if (rc < 0 && error_size && !error[0])
            snag_errorf(error, error_size, "cannot recover optimization: %s", strerror(saved));
        errno = saved;
    }
    return rc;
}

/* The retained JSONL includes its exact spelling and any unsealed tail. Keep
 * it as an ordinary Zstandard archive, independently of the native journal. */
static int
verify_archive(int archive, int source, int64_t length, unsigned char *input,
    unsigned char *output, const struct snag_context_control *control)
{
    ZSTD_DCtx *codec = ZSTD_createDCtx();
    if (!codec) return snag_errno(ENOMEM);
    const size_t chunk = 1024u * 1024u;
    unsigned char expected[8192];
    int64_t position = 0, packed = 0;
    size_t remaining = 1u;
    int rc = -1;
    for (;;) {
        if (control && control->cancelled && control->cancelled(control->opaque)) {
            errno = ECANCELED;
            break;
        }
        ssize_t got = snag_pread(archive, input, chunk, packed);
        if (got < 0) { if (errno == EINTR) continue; break; }
        if (!got) {
            if (!remaining && position == length) rc = 0;
            else errno = EBADMSG;
            break;
        }
        if (!remaining) { errno = EBADMSG; break; }
        packed += got;
        ZSTD_inBuffer in = {input, (size_t)got, 0u};
        while (in.pos < in.size) {
            ZSTD_outBuffer out = {output, chunk, 0u};
            remaining = ZSTD_decompressStream(codec, &out, &in);
            if (ZSTD_isError(remaining) || out.pos > (uint64_t)(length - position)) {
                errno = EBADMSG;
                goto done;
            }
            size_t checked = 0u;
            while (checked < out.pos) {
                size_t size = out.pos - checked;
                if (size > sizeof(expected)) size = sizeof(expected);
                ssize_t read = snag_pread(source, expected, size, position);
                if (read < 0 && errno == EINTR) continue;
                if (read != (ssize_t)size || memcmp(expected, output + checked, size)) {
                    errno = EBADMSG;
                    goto done;
                }
                checked += size;
                position += (int64_t)size;
            }
            if (!remaining && in.pos != in.size) { errno = EBADMSG; goto done; }
        }
    }
done:
    ZSTD_freeDCtx(codec);
    return rc;
}

static int
compress_legacy(struct snag_session *session, const struct snag_context_control *control,
    char *error, size_t error_size)
{
    int directory = snag_open_read_security_at(session->dir_fd, ".legacy-source", true);
    if (directory < 0) return errno == ENOENT ? 0 : -1;
    int source = -1, archive = -1, rc = -1;
    bool temporary = false;
    unsigned char *buffers = NULL;
    ZSTD_CCtx *codec = NULL;
    snag_file_info before, after;
    const size_t chunk = 1024u * 1024u;
    if (snag_store_verify_private_fd(directory, true, "retained legacy directory",
            error, error_size) < 0) goto done;
    source = snag_open_read_security_at(directory, "events.jsonl", false);
    if (source < 0) { if (errno == ENOENT) rc = 0; goto done; }
    if (snag_store_verify_private_fd(source, false, "retained legacy journal",
            error, error_size) < 0 || snag_fstat(source, &before) < 0 || before.st_size < 0)
        goto done;
    buffers = malloc(2u * chunk);
    if (!buffers) goto done;
    archive = snag_open_read_security_at(directory, "events.jsonl.zst", false);
    if (archive < 0) {
        if (errno != ENOENT || remove_private_file(directory, "events.jsonl.zst.tmp") < 0)
            goto done;
        archive = snag_create_private_at(directory, "events.jsonl.zst.tmp", true);
        if (archive < 0) goto done;
        temporary = true;
        codec = ZSTD_createCCtx();
        if (!codec) { errno = ENOMEM; goto done; }
        if (ZSTD_isError(ZSTD_CCtx_setParameter(codec, ZSTD_c_compressionLevel, 3))) {
            errno = EINVAL;
            goto done;
        }
        int64_t position = 0;
        size_t pending = 1u;
        while (pending) {
            if (control && control->cancelled && control->cancelled(control->opaque)) {
                errno = ECANCELED;
                goto done;
            }
            ssize_t got = snag_pread(source, buffers, chunk, position);
            if (got < 0) { if (errno == EINTR) continue; goto done; }
            position += got;
            ZSTD_inBuffer in = {buffers, (size_t)got, 0u};
            do {
                ZSTD_outBuffer out = {buffers + chunk, chunk, 0u};
                pending = ZSTD_compressStream2(codec, &out, &in,
                    got ? ZSTD_e_continue : ZSTD_e_end);
                if (ZSTD_isError(pending)) { errno = EIO; goto done; }
                if (snag_write_full(archive, out.dst, out.pos) < 0) goto done;
            } while (in.pos < in.size || (!got && pending));
            if (got) pending = 1u;
        }
        if (snag_sync_file(archive) < 0) goto done;
    }
    if (snag_store_verify_private_fd(archive, false, "retained legacy archive",
            error, error_size) < 0 ||
        verify_archive(archive, source, before.st_size, buffers, buffers + chunk, control) < 0 ||
        private_file(directory, "events.jsonl", &after) < 0) goto done;
    if (!snag_file_unchanged(&before, &after)) {
        errno = ESTALE;
        goto done;
    }
    if (temporary && snag_rename_at(directory, "events.jsonl.zst.tmp",
            directory, "events.jsonl.zst") < 0) goto done;
    temporary = false;
    if (sync_directory(directory) < 0 ||
        snag_unlink_at(directory, "events.jsonl", false) < 0 || sync_directory(directory) < 0)
        goto done;
    rc = 0;
done:
    {
        int saved = errno;
        ZSTD_freeCCtx(codec);
        free(buffers);
        if (source >= 0) (void)close(source);
        if (archive >= 0) (void)close(archive);
        if (temporary) (void)remove_private_file(directory, "events.jsonl.zst.tmp");
        (void)close(directory);
        if (rc < 0 && error_size && !error[0])
            snag_errorf(error, error_size, "native optimized; legacy archival failed: %s",
                strerror(saved));
        errno = saved;
    }
    return rc;
}

int
snag_store_optimize_directory(struct snag_session *source,
    const struct snag_context_control *control, char *error, size_t error_size)
{
    if (!source || source->lock_fd < 0 || source->log_fd < 0 || source->binary ||
        source->dir_fd < 0 || source->snapshot_read_only)
        return snag_fail(error, error_size, EINVAL, "invalid stopped optimization source");
    if (snag_store_recover_optimization(source, error, error_size) < 0) return -1;
    struct snag_session *target = malloc(sizeof(*target));
    if (!target) return -1;
    snag_session_init(target);
    int index = -1, stage = -1, rc = -1;
    bool ready = false, published = false;
    snag_file_info original, current;
    if (snag_fstat(source->log_fd, &original) < 0 ||
        snag_mkdir_private_at(source->dir_fd, ".optimize") < 0) goto done;
    stage = snag_open_read_security_at(source->dir_fd, ".optimize", true);
    if (stage < 0 || snag_store_verify_private_fd(stage, true,
            "optimization stage", error, error_size) < 0) goto done;
    target->dir_fd = dup(stage);
    target->dir_path = snag_path_join(source->dir_path, ".optimize");
    target->lock_fd = snag_create_private_at(stage, "lock", true);
    target->log_fd = snag_create_private_at(stage, "journal.bin", true);
    index = snag_create_private_at(stage, "history.idx", true);
    if (target->dir_fd < 0 || !target->dir_path || target->lock_fd < 0 ||
        target->log_fd < 0 || index < 0 || snag_lock_file(target->lock_fd, false) < 0) goto done;
    for (size_t i = 1u; i < 3u; ++i) {
        int fd = snag_create_private_at(stage, cache_names[i], true);
        if (fd < 0) goto done;
        int synced = snag_sync_file(fd);
        (void)close(fd);
        if (synced < 0) goto done;
    }
    if (snag_store_stage_optimized_session(source, target, index, control,
            error, error_size) < 0) goto done;
    snag_session_unbind_binary(target);
    if (snag_sync_file(index) < 0 || snag_sync_file(target->log_fd) < 0 ||
        sync_directory(stage) < 0) goto done;
    if (control && control->cancelled && control->cancelled(control->opaque)) {
        snag_fail(error, error_size, ECANCELED, "optimization cancelled before publication");
        goto done;
    }
    if (private_file(source->dir_fd, "journal.bin", &current) < 0 ||
        !snag_file_unchanged(&original, &current)) {
        snag_fail(error, error_size, ESTALE, "optimization source changed before publication");
        goto done;
    }
    if (snag_link_at(source->dir_fd, "journal.bin", stage, "old-journal.bin") < 0 ||
        snag_link_at(stage, "journal.bin", stage, "new-journal.bin") < 0) goto done;
    for (size_t i = 0u; i < 3u; ++i) {
        char name[32];
        (void)snprintf(name, sizeof(name), "old-%s", cache_names[i]);
        if (private_file(source->dir_fd, cache_names[i], &current) < 0) {
            if (errno == ENOENT) continue;
            goto done;
        }
        if (snag_link_at(source->dir_fd, cache_names[i], stage, name) < 0) goto done;
    }
    if (sync_directory(stage) < 0 || sync_directory(source->dir_fd) < 0) goto done;
    int marker = snag_create_private_at(stage, "ready", true);
    if (marker < 0) goto done;
    int synced = snag_sync_file(marker);
    (void)close(marker);
    ready = true;
    if (synced < 0 || sync_directory(stage) < 0) goto done;
    for (size_t i = 0u; i < 3u; ++i)
        if (install_cache(stage, cache_names[i], source->dir_fd, cache_names[i]) < 0) goto done;
    if (sync_directory(source->dir_fd) < 0 ||
        snag_rename_at(stage, "journal.bin", source->dir_fd, "journal.bin") < 0) goto done;
    published = true;
    if (sync_directory(stage) < 0 || sync_directory(source->dir_fd) < 0 ||
        clear_stage(source, stage) < 0 ||
        compress_legacy(source, control, error, error_size) < 0) goto done;
    rc = 0;
done:
    {
        int saved = errno ? errno : EIO;
        snag_session_close(target);
        free(target);
        if (index >= 0) (void)close(index);
        if (stage >= 0) {
            if (rc < 0 && !ready && clear_stage(source, stage) < 0) {
                size_t used = error_size ? strlen(error) : 0u;
                if (used < error_size)
                    (void)snag_errorf(error + used, error_size - used,
                        "%sprovisional cleanup failed: %s", used ? "; " : "", strerror(errno));
            }
            (void)close(stage);
        }
        if (rc < 0 && error_size && !error[0])
            snag_errorf(error, error_size, "optimization %s: %s",
                published ? "selected; durability/cleanup uncertain" :
                ready ? "interrupted; recover on next locked open" : "not published",
                strerror(saved));
        errno = saved;
    }
    return rc;
}
