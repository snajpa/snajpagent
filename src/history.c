/* SPDX-License-Identifier: GPL-2.0-only */
#include "history.h"
#include "fs.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HISTORY_FILE_BYTES (SNAG_HISTORY_BYTES * 4u + SNAG_HISTORY_COUNT)

void
snag_history_snapshot_free(struct snag_history_snapshot *snapshot)
{
    for (size_t i = 0u; i < snapshot->count; ++i)
        free(snapshot->items[i]);
    memset(snapshot, 0, sizeof(*snapshot));
}

int
snag_history_snapshot_copy(struct snag_history_snapshot *out,
                          const struct snag_history_snapshot *source)
{
    memset(out, 0, sizeof(*out));
    for (size_t i = 0u; i < source->count; ++i) {
        out->items[i] = snag_strdup_checked(source->items[i], SNAG_HISTORY_BYTES);
        if (!out->items[i]) {
            snag_history_snapshot_free(out);
            return -1;
        }
        ++out->count;
    }
    out->bytes = source->bytes;
    return 0;
}

void
snag_history_free(struct snag_history *history)
{
    snag_history_snapshot_free(&history->snapshot);
    snag_history_snapshot_free(&history->pending);
    free(history->path);
    free(history->global_path);
    memset(history, 0, sizeof(*history));
}

static void
history_note_warning(struct snag_history *term)
{
    if (!term->warned)
        term->warning = true;
    term->warned = true;
}

bool
snag_history_take_warning(struct snag_history *term)
{
    bool pending = term && term->warning;

    if (term)
        term->warning = false;
    return pending;
}

static int
history_memory_add(struct snag_history_snapshot *snapshot, const char *text, bool *dropped)
{
    size_t len = strlen(text);
    char *copy;

    if (!len || len > SNAG_HISTORY_BYTES)
        return 0;
    copy = snag_strdup_checked(text, SNAG_HISTORY_BYTES);
    if (!copy)
        return -1;
    while (snapshot->count == SNAG_HISTORY_COUNT ||
           snapshot->bytes > SNAG_HISTORY_BYTES - len) {
        size_t old = strlen(snapshot->items[0]);
        if (dropped)
            *dropped = true;
        free(snapshot->items[0]);
        memmove(snapshot->items, snapshot->items + 1u,
                (snapshot->count - 1u) * sizeof(snapshot->items[0]));
        --snapshot->count;
        snapshot->bytes -= old;
    }
    snapshot->items[snapshot->count++] = copy;
    snapshot->bytes += len;
    return 0;
}

static int
history_lock(int fd)
{
    while (snag_lock_file(fd, true) < 0)
        if (errno != EINTR)
            return -1;
    return 0;
}

static int
history_file_open(const char *path)
{
    int fd = snag_open_history(path);

    if (fd < 0)
        return -1;
    if (history_lock(fd) < 0) {
        int saved = errno;
        (void)close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static int
history_decode(const unsigned char *line, size_t len, struct snag_buf *out)
{
    static const char hex[] = "0123456789ABCDEF";

    snag_buf_reset(out);
    for (size_t i = 0u; i < len; ++i) {
        unsigned char c = line[i];

        if (c != '\\') {
            if (c < 0x20u || c == 0x7fu || snag_buf_putc(out, c) < 0)
                return -1;
            continue;
        }
        if (++i >= len)
            return -1;
        c = line[i];
        if (c == '\\') c = '\\';
        else if (c == 'n') c = '\n';
        else if (c == 'r') c = '\r';
        else if (c == 't') c = '\t';
        else if (c == 'x' && i + 2u < len) {
            const char *hi = strchr(hex, line[++i]);
            const char *lo = strchr(hex, line[++i]);
            if (!hi || !*hi || !lo || !*lo)
                return -1;
            c = (unsigned char)(((hi - hex) << 4) | (lo - hex));
        } else {
            return -1;
        }
        if (!c || snag_buf_putc(out, c) < 0)
            return -1;
    }
    if (!out->len || !snag_utf8_valid(out->data, out->len, true) ||
        snag_buf_terminate(out) < 0)
        return -1;
    return 0;
}

static int
history_encode(struct snag_buf *out, const char *text)
{
    static const char hex[] = "0123456789ABCDEF";
    const unsigned char *p = (const unsigned char *)text;

    snag_buf_reset(out);
    for (; *p; ++p) {
        unsigned char c = *p;
        const char *escape = c == '\\' ? "\\\\" : c == '\n' ? "\\n" :
                             c == '\r' ? "\\r" : c == '\t' ? "\\t" : NULL;
        if (escape) {
            if (snag_buf_append(out, escape, 2u) < 0)
                return -1;
        } else if (c < 0x20u || c == 0x7fu) {
            unsigned char encoded[4] = {'\\', 'x', hex[c >> 4], hex[c & 15u]};
            if (snag_buf_append(out, encoded, sizeof(encoded)) < 0)
                return -1;
        } else if (snag_buf_putc(out, c) < 0) {
            return -1;
        }
    }
    return 0;
}

static int
history_rewrite(const struct snag_history_snapshot *snapshot, int fd)
{
    int rc = -1;

    struct snag_buf encoded = {.max = HISTORY_FILE_BYTES};
    struct snag_buf file = {.max = HISTORY_FILE_BYTES};
    for (size_t i = 0u; i < snapshot->count; ++i)
        if (history_encode(&encoded, snapshot->items[i]) < 0 ||
            snag_buf_append(&file, encoded.data, encoded.len) < 0 ||
            snag_buf_putc(&file, '\n') < 0)
            goto out;
    if (snag_truncate(fd, 0) == 0 && snag_seek(fd, 0, SEEK_SET) == 0 &&
        snag_write_full(fd, file.data, file.len) == 0)
        rc = snag_sync_file(fd);
out:
    snag_buf_free(&encoded);
    snag_buf_free(&file);
    return rc;
}

static int
history_load_locked(struct snag_history_snapshot *snapshot, int fd, bool *damaged)
{
    snag_file_info st;
    size_t pos = 0u;
    bool dirty = false;
    int rc = -1;

    if (snag_fstat(fd, &st) < 0 || st.st_size < 0 ||
        (uintmax_t)st.st_size > HISTORY_FILE_BYTES)
        return -1;
    struct snag_buf file = {.max = HISTORY_FILE_BYTES + 1u};
    struct snag_buf decoded = {.max = SNAG_HISTORY_BYTES + 1u};
    if (snag_seek(fd, 0, SEEK_SET) < 0)
        goto out;
    while (file.len < (size_t)st.st_size) {
        unsigned char chunk[4096];
        ssize_t got = read(fd, chunk, sizeof(chunk));
        if (got < 0) {
            if (errno == EINTR) continue;
            goto out;
        }
        if (!got) break;
        if (snag_buf_append(&file, chunk, (size_t)got) < 0)
            goto out;
    }
    snag_history_snapshot_free(snapshot);
    while (pos < file.len) {
        unsigned char *lf = memchr(file.data + pos, '\n', file.len - pos);
        bool dropped = false;
        size_t len;

        if (!lf) {
            dirty = *damaged = true;
            break;
        }
        len = (size_t)(lf - file.data - pos);
        if (history_decode(file.data + pos, len, &decoded) < 0) {
            dirty = *damaged = true;
        } else if (history_memory_add(snapshot, (char *)decoded.data, &dropped) < 0) {
            goto out;
        } else if (dropped) {
            dirty = true;
        }
        pos += len + 1u;
    }
    rc = dirty ? history_rewrite(snapshot, fd) : 0;
out:
    snag_buf_free(&decoded);
    snag_buf_free(&file);
    return rc;
}

static char *
history_path(const char *directory)
{
    if (!snag_path_root_len(directory)) {
        errno = EINVAL;
        return NULL;
    }
    return snag_path_join(directory, "prompt_history");
}

int
snag_history_open(struct snag_history *history, const char *dotdir)
{
    bool damaged = false;
    int rc = -1;
    if (!history) return snag_errno(EINVAL);
    history->global_path = history_path(dotdir);
    int fd = history->global_path ? history_file_open(history->global_path) : -1;
    if (fd >= 0) {
        rc = history_load_locked(&history->snapshot, fd, &damaged);
        (void)close(fd);
    }
    if (rc < 0 || damaged) history_note_warning(history);
    return rc;
}

int
snag_history_bind(struct snag_history *history, const char *session_dir)
{
    snag_file_info st;
    bool damaged = false;
    int rc = -1;
    if (!history || !history->global_path || !session_dir || history->path)
        return 0;
    char *path = history_path(session_dir);
    if (!path) goto out;
    bool existed = snag_lstat(path, &st) == 0;
    if (!existed && errno != ENOENT) goto out;
    int fd = history_file_open(path);
    if (fd < 0) goto out;
    if (existed) {
        struct snag_history_snapshot saved = {0};
        rc = history_load_locked(&saved, fd, &damaged);
        for (size_t i = 0u; rc == 0 && i < history->pending.count; ++i)
            rc = history_memory_add(&saved, history->pending.items[i], NULL);
        if (rc == 0 && history->pending.count) rc = history_rewrite(&saved, fd);
        if (rc == 0) {
            snag_history_snapshot_free(&history->snapshot);
            history->snapshot = saved;
        } else snag_history_snapshot_free(&saved);
    } else rc = history_rewrite(&history->snapshot, fd);
    (void)close(fd);
    if (rc == 0) {
        history->path = path;
        path = NULL;
    }
out:
    free(path);
    if (rc < 0 || damaged) history_note_warning(history);
    return rc;
}

int
snag_history_merge(struct snag_history *history)
{
    struct snag_history_snapshot global = {0};
    bool damaged = false;
    int rc = -1;
    if (!history || !history->global_path || !history->pending.count) return 0;
    int fd = history_file_open(history->global_path);
    if (fd < 0) goto out;
    if (history_load_locked(&global, fd, &damaged) < 0) goto close_file;
    for (size_t i = 0u; i < history->pending.count; ++i)
        if (history_memory_add(&global, history->pending.items[i], NULL) < 0)
            goto close_file;
    rc = history_rewrite(&global, fd);
    if (rc == 0) snag_history_snapshot_free(&history->pending);
close_file:
    (void)close(fd);
out:
    snag_history_snapshot_free(&global);
    if (rc < 0 || damaged) history_note_warning(history);
    return rc;
}

int
snag_history_add(struct snag_history *history, const char *text)
{
    bool dropped = false;
    int rc = -1;
    struct snag_buf encoded = {.max = HISTORY_FILE_BYTES};
    if (!history || !text || !*text) return 0;
    if (history_memory_add(&history->snapshot, text, &dropped) < 0 ||
        history_memory_add(&history->pending, text, NULL) < 0) goto out;
    if (!history->path) return 0;
    int fd = history_file_open(history->path);
    if (fd < 0) goto out;
    if (dropped || history->rewrite) rc = history_rewrite(&history->snapshot, fd);
    else if (snag_seek(fd, 0, SEEK_END) >= 0 && history_encode(&encoded, text) == 0 &&
             snag_write_full(fd, encoded.data, encoded.len) == 0 &&
             snag_write_full(fd, "\n", 1u) == 0)
        rc = snag_sync_file(fd);
    (void)close(fd);
out:
    snag_buf_free(&encoded);
    history->rewrite = rc < 0;
    if (rc < 0) history_note_warning(history);
    return rc;
}
