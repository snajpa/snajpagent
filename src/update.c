/* SPDX-License-Identifier: GPL-2.0-only */
#include "update.h"

#ifndef SNAJPAGENT_UPDATE_URL
struct snag_update *
snag_update_start(const char *program, const char *url, snag_wake_fd wake)
{
    (void)program; (void)url; (void)wake;
    return 0;
}
const char *snag_update_take(struct snag_update *update) { (void)update; return 0; }
char *snag_update_stop(struct snag_update *update) { (void)update; return 0; }
#else
#include "base.h"
#include "fs.h"
#include "http.h"
#include "json.h"
#include "snajpagent.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UPDATE_INFO_MAX (16u * 1024u)
#define UPDATE_BINARY_MAX (256u * 1024u * 1024u)
#define UPDATE_MARKER "\nsnajpagent-update-v1\n" SNAJPAGENT_NAME "\n" SNAJPAGENT_UPDATE_TARGET "\n" SNAJPAGENT_UPDATE_BASE "\n"
/* Retained in the executable; candidates must retain this publisher/target. */
static const volatile char identity[] = UPDATE_MARKER SNAJPAGENT_VERSION "\n";

struct snag_update {
    pthread_t thread;
    atomic_bool cancel, done;
    bool reported;
    snag_wake_fd wake;
    char *program, *url;
    char banner[4096];
};

struct download {
    struct snag_update *update;
    struct snag_buf *body;
    struct snag_sha256 hash;
    int fd;
    uint64_t size, limit;
};

static bool
secure_url(const char *url, bool fragment)
{
    if (!url || strlen(url) >= 2048u) return false;
#ifdef SNAJPAGENT_TEST_UPDATE
    if (strncmp(url, "http://127.0.0.1:", 17u) == 0)
        return true;
#endif
    if (strncmp(url, "https://", 8u) != 0 || !url[8])
        return false;
    for (const char *p = url; *p; ++p)
        if ((unsigned char)*p <= 32u || (unsigned char)*p >= 127u ||
            *p == '@' || (!fragment && *p == '#') || *p == '\\') return false;
    return true;
}

static size_t
receive(char *data, size_t size, size_t count, void *opaque)
{
    struct download *d = opaque;
    if (size && count > SIZE_MAX / size) return 0;
    size *= count;
    if (atomic_load(&d->update->cancel) || size > d->limit - d->size) return 0;
    if (d->body ? snag_buf_append(d->body, data, size) < 0 : snag_write_full(d->fd, data, size) < 0) return 0;
    snag_sha256_update(&d->hash, data, size);
    d->size += size;
    return size;
}

static int
fetch(struct snag_update *update, const char *url, struct snag_buf *body,
      int fd, uint64_t limit, char hash[65], uint64_t *size)
{
    CURL *curl = NULL;
    CURLM *multi = NULL;
    int rc = -1, running = 0, pending;
    bool attached = false;
    struct download d = {.update = update, .body = body, .fd = fd, .limit = limit};
    long status = 0;
    if (!secure_url(url, false) || snag_http_init() != CURLE_OK) return -1;
    /* Synchronous resolvers cannot meet cancellation/exit latency guarantees. */
    if (!(curl_version_info(CURLVERSION_NOW)->features & CURL_VERSION_ASYNCHDNS)) return -1;
    curl = curl_easy_init();
    multi = curl_multi_init();
    snag_sha256_init(&d.hash);
    if (!curl || !multi || snag_http_trust(curl) != CURLE_OK) goto out;
#define SET(option, value) do { if (curl_easy_setopt(curl, option, value) != CURLE_OK) goto out; } while (0)
    SET(CURLOPT_URL, url);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_FOLLOWLOCATION, 1L);
    SET(CURLOPT_MAXREDIRS, 5L);
#if LIBCURL_VERSION_NUM >= 0x075500
#ifdef SNAJPAGENT_TEST_UPDATE
    SET(CURLOPT_PROTOCOLS_STR, "http,https");
    SET(CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    SET(CURLOPT_PROTOCOLS_STR, "https");
    SET(CURLOPT_REDIR_PROTOCOLS_STR, "https");
#endif
#else
    SET(CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    SET(CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
    SET(CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    SET(CURLOPT_TIMEOUT_MS, body ? 15000L : 120000L);
    SET(CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)limit);
    SET(CURLOPT_WRITEFUNCTION, receive);
    SET(CURLOPT_WRITEDATA, &d);
    SET(CURLOPT_USERAGENT, SNAJPAGENT_NAME "/" SNAJPAGENT_VERSION);
#undef SET
    if (curl_multi_add_handle(multi, curl) != CURLM_OK) goto out;
    attached = true;
    do {
        if (atomic_load(&update->cancel) || curl_multi_perform(multi, &running) != CURLM_OK) goto out;
        if (running && curl_multi_poll(multi, NULL, 0, 100, NULL) != CURLM_OK) goto out;
    } while (running);
    CURLMsg *message = curl_multi_info_read(multi, &pending);
    if (!message || message->msg != CURLMSG_DONE || message->data.result != CURLE_OK ||
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK || status != 200) goto out;
    unsigned char digest[32];
    snag_sha256_final(&d.hash, digest);
    if (hash) {
        for (size_t i = 0; i < sizeof(digest); ++i) (void)snprintf(hash + 2u * i, 3u, "%02x", digest[i]);
    }
    if (size) *size = d.size;
    rc = 0;
out:
    if (attached) (void)curl_multi_remove_handle(multi, curl);
    if (multi) (void)curl_multi_cleanup(multi);
    if (curl) curl_easy_cleanup(curl);
    return rc;
}

static bool
same_file(const snag_file_info *a, const snag_file_info *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_size == b->st_size && a->st_mtime == b->st_mtime;
}

static bool
has_identity(int fd, const char *version, struct snag_update *update)
{
    char expected[4096];
    unsigned char buffer[16384];
    size_t keep = 0;
    int n = snprintf(expected, sizeof(expected), "%s%s\n", UPDATE_MARKER, version);
    if (n <= 0 || (size_t)n >= sizeof(expected) || snag_seek(fd, 0, SEEK_SET) < 0) return false;
    for (;;) {
        if (atomic_load(&update->cancel)) return false;
        ssize_t got = read(fd, buffer + keep, sizeof(buffer) - keep);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        size_t len = keep + (size_t)got;
        for (size_t i = 0; i + (size_t)n <= len; ++i)
            if (memcmp(buffer + i, expected, (size_t)n) == 0) return true;
        keep = len < (size_t)n - 1u ? len : (size_t)n - 1u;
        memmove(buffer, buffer + len - keep, keep);
    }
}

static const char *
version_parts(const char *text, uint32_t parts[3])
{
    if (!text || strlen(text) > 100u) return NULL;
    for (size_t i = 0; i < 3u; ++i) {
        const char *start = text;
        parts[i] = 0;
        while (*text >= '0' && *text <= '9') {
            unsigned int digit = (unsigned int)(*text++ - '0');
            if (parts[i] > (UINT32_MAX - digit) / 10u) return NULL;
            parts[i] = parts[i] * 10u + digit;
        }
        if (text == start || (text - start > 1 && *start == '0') || (i < 2u && *text++ != '.')) return NULL;
    }
    return text;
}

static bool
newer_version(const char *candidate)
{
    uint32_t a[3], b[3];
    const char *suffix = version_parts(candidate, a);
    const char *current = version_parts(SNAJPAGENT_VERSION, b);
    if (!suffix || !current) return false;
    if (*suffix && (*suffix != '-' || strlen(suffix + 1u) < 7u || strlen(suffix + 1u) > 40u ||
                    !snag_hex_is_lower(suffix + 1u, strlen(suffix + 1u)))) return false;
    if (!*current && *suffix) return false;
    for (size_t i = 0; i < 3u; ++i)
        if (a[i] != b[i]) return a[i] > b[i];
    /* Git hashes have no chronological ordering: the publisher chooses the
     * development tip. Numeric releases still never move backwards. */
    return *current == '-' && strcmp(candidate, SNAJPAGENT_VERSION) != 0;
}

static void
install_update(struct snag_update *update)
{
    char *path = NULL, *base = NULL;
    char meta_url[2060], lockname[300], stage[300], backup[300], hash[65];
    int dir = -1, original = -1, lock = -1, output = -1;
    bool staged = false;
    const char *restore = NULL;
#if defined(_WIN32) || defined(SNAJPAGENT_TEST_RENAME_ASIDE)
    char canonical[300];
#endif
    struct snag_buf body;
    json_t *meta = NULL;
    snag_file_info before, current;
    struct snag_permissions permissions = {0};
    struct snag_file_privacy privacy;
    uint64_t expected_size = 0, downloaded = 0;
    char error[128];
    snag_buf_init(&body, UPDATE_INFO_MAX);
    /* Make the embedded marker observable even under LTO. */
    if (!identity[0] || !secure_url(update->url, false)) goto out;
#if defined(__linux__) && !defined(SNAJPAGENT_TEST_UPDATE)
    char self[SNAG_PATH_MAX_BYTES + 1u];
    ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1u);
    if (len <= 0 || (size_t)len >= sizeof(self) - 1u) goto out;
    self[len] = '\0';
    path = strdup(self);
#else
    path = snag_program_path(update->program);
#endif
    if (!path || !(base = strrchr(path, '/')) || !base[1]) goto out;
    *base++ = '\0';
#if defined(_WIN32) || defined(SNAJPAGENT_TEST_RENAME_ASIDE)
    /* Recovery and installation reserve the same canonical name. */
    static const char suffix[] = ".update-old.exe";
    size_t base_len = strlen(base), suffix_len = sizeof(suffix) - 1u;
    if (base[0] == '.' && base_len > suffix_len + 1u && strcmp(base + base_len - suffix_len, suffix) == 0) {
        size_t len = base_len - suffix_len - 1u;
        if (len >= sizeof(canonical)) goto out;
        memcpy(canonical, base + 1u, len); canonical[len] = '\0';
        restore = base;
        base = canonical;
    }
#endif
    if (snprintf(lockname, sizeof(lockname), ".%s.update-lock", base) >= (int)sizeof(lockname) ||
        snprintf(stage, sizeof(stage), ".%s.update-new", base) >= (int)sizeof(stage) ||
        snprintf(backup, sizeof(backup), ".%s.update-old.exe", base) >= (int)sizeof(backup)) goto out;
#ifdef _WIN32
    /* Absolute NT paths ignore the directory descriptor. */
    dir = snag_open_read_security_at(-1, path, true);
#else
    dir = snag_open_read_security_at(AT_FDCWD, *path ? path : "/", true);
#endif
    if (dir < 0 || snag_fstat(dir, &current) < 0 ||
        snag_fd_privacy(dir, &privacy) < 0 || !privacy.effective_owner) goto out;
#ifndef _WIN32
    if (current.st_mode & (S_IWGRP | S_IWOTH)) goto out;
#endif
    lock = snag_open_private_append_at(dir, lockname, true);
    if (lock < 0 && errno == EEXIST) lock = snag_open_private_append_at(dir, lockname, false);
    if (lock < 0 || snag_lock_file(lock, false) < 0) goto out;
    original = snag_open_read_security_at(dir, restore ? restore : base, false);
    if (original < 0 || snag_fstat(original, &before) < 0 || !S_ISREG(before.st_mode) || before.st_size < 0 ||
        (uint64_t)before.st_size > UPDATE_BINARY_MAX ||
        before.st_nlink != 1 || snag_fd_privacy(original, &privacy) < 0 ||
        !privacy.effective_owner || !has_identity(original, SNAJPAGENT_VERSION, update) ||
        snag_permissions_capture(original, &permissions) < 0) goto out;
#ifndef _WIN32
    if (before.st_mode & (S_ISUID | S_ISGID | S_IWGRP | S_IWOTH)) goto out;
#endif
    if (restore) {
        if (snag_lstat_at(dir, base, &current) < 0 && errno == ENOENT &&
            snag_rename_at(dir, restore, dir, base) == 0) (void)snag_sync_dir(dir);
        goto out;
    }
    /* The owner-controlled directory and lock reserve these staging names. */
    if (snag_lstat_at(dir, stage, &current) == 0) {
        if (!S_ISREG(current.st_mode) || current.st_nlink != 1 || snag_unlink_at(dir, stage, false) < 0)
            goto out;
    } else if (errno != ENOENT) {
        goto out;
    }
    if (snprintf(meta_url, sizeof(meta_url), "%s.json", update->url) >= (int)sizeof(meta_url) ||
        fetch(update, meta_url, &body, -1, UPDATE_INFO_MAX, NULL, NULL) < 0) goto out;
    meta = snag_json_load_strict(body.data, body.len, UPDATE_INFO_MAX, error, sizeof(error));
    if (!meta) goto out;
    const char *name = snag_json_string(meta, "name");
    const char *target = snag_json_string(meta, "target");
    const char *version = snag_json_string(meta, "version");
    const char *url = snag_json_string(meta, "url");
    const char *digest = snag_json_string(meta, "sha256");
    const char *changelog = snag_json_string(meta, "changelog");
    if (!name || strcmp(name, SNAJPAGENT_NAME) || !target || strcmp(target, SNAJPAGENT_UPDATE_TARGET) ||
        !newer_version(version) || !secure_url(url, false) || !secure_url(changelog, true) ||
        !digest || strlen(digest) != 64u || !snag_hex_is_lower(digest, 64u) ||
        snag_json_integer_u64(meta, "size", &expected_size) < 0 ||
        !expected_size || expected_size > UPDATE_BINARY_MAX || atomic_load(&update->cancel)) goto out;
    output = snag_create_private_at(dir, stage, true);
    if (output < 0) goto out;
    staged = true;
    if (fetch(update, url, NULL, output, expected_size, hash, &downloaded) < 0 ||
        downloaded != expected_size || strcmp(hash, digest) || !has_identity(output, version, update) ||
        snag_permissions_apply(output, &permissions) < 0 || snag_sync_file(output) < 0 ||
        snag_lstat_at(dir, base, &current) < 0 || !same_file(&before, &current) ||
        atomic_load(&update->cancel)) goto out;
    close(output); output = -1;
#ifdef SNAJPAGENT_TEST_RENAME_ASIDE
    if (true) {
#else
    if (snag_rename_at(dir, stage, dir, base) < 0) {
#endif
#if defined(_WIN32) || defined(SNAJPAGENT_TEST_RENAME_ASIDE)
        /* Classic Windows cannot replace a mapped executable. Preserve it at
         * a fixed recovery name until no process maps it. No restart/helper. */
        if (snag_lstat_at(dir, backup, &current) == 0) {
            if (!S_ISREG(current.st_mode) || snag_unlink_at(dir, backup, false) < 0) goto out;
        } else if (errno != ENOENT) {
            goto out;
        }
        if (snag_rename_at(dir, base, dir, backup) < 0) goto out;
        (void)snag_sync_dir(dir);
#ifdef SNAJPAGENT_TEST_RENAME_ASIDE
        if (getenv("SNAJPAGENT_TEST_RENAME_CRASH")) _exit(79);
#endif
        if (snag_rename_at(dir, stage, dir, base) < 0) {
            (void)snag_rename_at(dir, backup, dir, base);
            goto out;
        }
#else
        goto out;
#endif
    }
    staged = false;
    (void)snag_sync_dir(dir);
    (void)snprintf(update->banner, sizeof(update->banner),
        "=== " SNAJPAGENT_NAME " updated ===\nInstalled %s. Restart when convenient to use it.\nChangelog: %s\n",
        version, changelog);
out:
    if (output >= 0) close(output);
    if (staged) (void)snag_unlink_at(dir, stage, false);
    if (original >= 0) close(original);
    if (lock >= 0) close(lock);
    if (dir >= 0) close(dir);
    snag_permissions_free(&permissions);
    json_decref(meta);
    snag_buf_free(&body);
    free(path);
}

static void *
worker(void *opaque)
{
    struct snag_update *update = opaque;
    install_update(update);
    atomic_store_explicit(&update->done, true, memory_order_release);
    snag_wakeup_send(update->wake);
    return NULL;
}

struct snag_update *
snag_update_start(const char *program, const char *url, snag_wake_fd wake)
{
    struct snag_update *update = calloc(1u, sizeof(*update));
    if (!update) return NULL;
    atomic_init(&update->cancel, false);
    atomic_init(&update->done, false);
    update->wake = wake;
    update->program = program ? strdup(program) : NULL;
    update->url = url ? strdup(url) : NULL;
    if (!update->program || !update->url || pthread_create(&update->thread, NULL, worker, update) != 0) {
        free(update->program); free(update->url); free(update);
        return NULL;
    }
    return update;
}

const char *
snag_update_take(struct snag_update *update)
{
    if (!update || update->reported || !atomic_load_explicit(&update->done, memory_order_acquire))
        return NULL;
    update->reported = true;
    return update->banner[0] ? update->banner : NULL;
}

char *
snag_update_stop(struct snag_update *update)
{
    if (!update) return NULL;
    atomic_store(&update->cancel, true);
    (void)pthread_join(update->thread, NULL);
    const char *pending = snag_update_take(update);
    char *banner = pending ? strdup(pending) : NULL;
    free(update->program); free(update->url); free(update);
    return banner;
}
#endif
