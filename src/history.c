/* SPDX-License-Identifier: GPL-2.0-only */
#include "history.h"
#include "fs.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HISTORY_FILE_BYTES (SNAG_HISTORY_BYTES * 4u + SNAG_HISTORY_COUNT)

static void
history_memory_clear(struct snag_history_snapshot *snapshot)
{
    for (size_t i = 0u; i < snapshot->count; ++i) free(snapshot->items[i]);
    snapshot->count = snapshot->bytes = 0u;
}

void
snag_history_snapshot_free(struct snag_history_snapshot *snapshot)
{
    history_memory_clear(snapshot);
    free(snapshot->local_path);
    free(snapshot->global_path);
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
    out->local_end = source->local_end;
    out->local_path = source->local_path ? strdup(source->local_path) : NULL;
    out->global_path = source->global_path ? strdup(source->global_path) : NULL;
    if ((source->local_path && !out->local_path) || (source->global_path && !out->global_path)) {
        snag_history_snapshot_free(out);
        return -1;
    }
    return 0;
}

void
snag_history_free(struct snag_history *history)
{
    snag_history_snapshot_free(&history->snapshot);
    if (history->path) (void)close(history->local_fd);
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

/* Readers never take the writer lock. Each navigation/search holds fixed file
 * ends; append-only writers cannot move a selected entry beneath the editor. */
void
snag_history_reader_close(struct snag_history_reader *reader)
{
    for (unsigned int i = 0u; i < 2u; ++i)
        if (reader->fd[i] >= 0) (void)close(reader->fd[i]);
    snag_buf_free(&reader->encoded);
    snag_buf_free(&reader->decoded);
    *reader = (struct snag_history_reader){.fd = {-1, -1}};
}

void
snag_history_reader_open(struct snag_history_reader *reader,
                         const struct snag_history_snapshot *snapshot)
{
    const char *paths[] = {snapshot->local_path, snapshot->global_path};
    snag_history_reader_close(reader);
    reader->budget = SIZE_MAX;
    reader->encoded.max = HISTORY_FILE_BYTES;
    reader->decoded.max = SNAG_HISTORY_BYTES + 1u;
    for (unsigned int i = 0u; i < 2u; ++i) {
        if (!paths[i]) continue;
        int fd = snag_open_read(paths[i], false);
        snag_file_info st;
        struct snag_file_privacy privacy;
        if (fd < 0 || snag_fstat(fd, &st) < 0 || st.st_size < 0 ||
            !S_ISREG(st.st_mode) || st.st_nlink != 1u ||
            snag_fd_privacy(fd, &privacy) < 0 || !privacy.effective_owner || !privacy.private_access) {
            if (fd >= 0) (void)close(fd);
            reader->warning = true;
            continue;
        }
        reader->fd[i] = fd;
        reader->size[i] = i == 0u && snapshot->local_end < st.st_size ?
                          snapshot->local_end : st.st_size;
    }
}

struct snag_history_cursor
snag_history_end(const struct snag_history_snapshot *snapshot)
{
    return (struct snag_history_cursor){0u, (int64_t)snapshot->count};
}

static int
history_byte(struct snag_history_reader *reader, unsigned int source, int64_t offset,
             unsigned char *byte)
{
    if (reader->cache_source != source || offset < reader->cache_start ||
        offset - reader->cache_start >= (int64_t)reader->cache_len) {
        reader->cache_source = source;
        reader->cache_start = offset - offset % (int64_t)sizeof(reader->cache);
        reader->cache_len = 0u;
        size_t amount = sizeof(reader->cache);
        if (reader->size[source - 1u] - reader->cache_start < (int64_t)amount)
            amount = (size_t)(reader->size[source - 1u] - reader->cache_start);
        ssize_t got;
        do got = snag_pread(reader->fd[source - 1u], reader->cache, amount, reader->cache_start);
        while (got < 0 && errno == EINTR);
        if (got <= 0) return snag_errno(EIO);
        reader->cache_len = (size_t)got;
    }
    if (offset - reader->cache_start >= (int64_t)reader->cache_len)
        return snag_errno(EIO);
    *byte = reader->cache[(size_t)(offset - reader->cache_start)];
    return 0;
}

int
snag_history_read(struct snag_history_reader *reader,
                  const struct snag_history_snapshot *snapshot, bool newer,
                  struct snag_history_cursor from, struct snag_history_cursor *start,
                  struct snag_history_cursor *end, const char **text)
{
    if (reader->scanning) {
        from.source = reader->scan_source;
        from.offset = newer ? reader->scan_first : reader->scan_last;
    }
    for (;;) {
        if (from.source == 0u) {
            if ((!newer && from.offset > 0) ||
                (newer && from.offset < (int64_t)snapshot->count)) {
                int64_t i = newer ? from.offset : from.offset - 1;
                *start = (struct snag_history_cursor){0u, i};
                *end = (struct snag_history_cursor){0u, i + 1};
                *text = snapshot->items[i];
                return 1;
            }
            if (newer) return 0;
            from = (struct snag_history_cursor){1u, reader->size[0]};
        }
        if (from.source > 2u) return 0;
        int64_t size = reader->size[from.source - 1u];
        if ((!newer && from.offset <= 0) || (newer && from.offset >= size)) {
            if (newer) {
                --from.source;
                from.offset = 0;
            } else {
                if (++from.source > 2u) return 0;
                from.offset = reader->size[from.source - 1u];
            }
            continue;
        }
        if (from.offset > size) return snag_errno(EIO);
        unsigned char c;
        if (!reader->scanning) {
            reader->scan_source = from.source;
            reader->scan_pos = from.offset;
            reader->scan_first = reader->scan_last = from.offset;
            reader->scan_complete = reader->scan_oversized = false;
            snag_buf_reset(&reader->encoded);
            if (!newer) {
                if (history_byte(reader, from.source, from.offset - 1, &c) < 0) return -1;
                reader->scan_complete = c == '\n';
                if (reader->scan_complete) --reader->scan_pos;
            }
            reader->scanning = true;
        }
        from.source = reader->scan_source;
        while (newer ? reader->scan_pos < size : reader->scan_pos > 0) {
            if (!reader->budget) return 2;
            --reader->budget;
            int64_t pos = newer ? reader->scan_pos : reader->scan_pos - 1;
            if (history_byte(reader, from.source, pos, &c) < 0) {
                reader->scanning = false;
                return -1;
            }
            if (newer) ++reader->scan_pos;
            if (c == '\n') {
                if (newer) reader->scan_complete = true;
                break;
            }
            if (!newer) --reader->scan_pos;
            if (reader->encoded.len == reader->encoded.max) reader->scan_oversized = true;
            if (!reader->scan_oversized && snag_buf_putc(&reader->encoded, c) < 0) return -1;
        }
        if (newer) reader->scan_last = reader->scan_pos;
        else {
            reader->scan_first = reader->scan_pos;
            for (size_t i = 0u, n = reader->encoded.len; i < n / 2u; ++i) {
                c = reader->encoded.data[i];
                reader->encoded.data[i] = reader->encoded.data[n - 1u - i];
                reader->encoded.data[n - 1u - i] = c;
            }
        }
        from.offset = reader->scan_pos;
        reader->scanning = false;
        if (!reader->scan_complete || reader->scan_oversized || history_decode(reader->encoded.data,
            reader->encoded.len, &reader->decoded) < 0 || reader->decoded.len > SNAG_MAX_DIRECT_PROMPT) {
            reader->warning = true;
            continue;
        }
        *start = (struct snag_history_cursor){from.source, reader->scan_first};
        *end = (struct snag_history_cursor){from.source, reader->scan_last};
        *text = (char *)reader->decoded.data;
        return 1;
    }
}

/* Only discard an incomplete final record while holding the writer lock.
 * Valid old records are never rewritten or pruned. */
static int64_t
history_complete_end(int fd)
{
    int64_t end = snag_seek(fd, 0, SEEK_END), pos = end;
    unsigned char block[8192];
    if (end < 0) return -1;
    while (pos > 0) {
        size_t n = pos < (int64_t)sizeof(block) ? (size_t)pos : sizeof(block);
        ssize_t got;
        if (snag_seek(fd, pos - (int64_t)n, SEEK_SET) < 0) return -1;
        do got = read(fd, block, n);
        while (got < 0 && errno == EINTR);
        if (got != (ssize_t)n) return -1;
        for (size_t i = n; i; --i)
            if (block[i - 1u] == '\n')
                return pos - (int64_t)n + (int64_t)i;
        pos -= (int64_t)n;
    }
    return 0;
}

static int
history_append(int fd, const struct snag_history_snapshot *snapshot, int64_t *end)
{
    struct snag_buf encoded = {.max = HISTORY_FILE_BYTES};
    int64_t original = history_complete_end(fd);
    int rc = -1;
    if (original < 0 || snag_truncate(fd, original) < 0) return -1;
    for (size_t i = 0u; i < snapshot->count; ++i)
        if (history_encode(&encoded, snapshot->items[i]) < 0 ||
            snag_buf_putc(&encoded, '\n') < 0 ||
            snag_write_full(fd, encoded.data, encoded.len) < 0) goto out;
    if (snag_sync_file(fd) < 0 || (*end = snag_seek(fd, 0, SEEK_END)) < 0) goto out;
    rc = 0;
out:
    if (rc < 0) (void)snag_truncate(fd, original);
    snag_buf_free(&encoded);
    return rc;
}

int
snag_history_open(struct snag_history *history, const char *dotdir)
{
    if (!history || !snag_path_root_len(dotdir)) return snag_errno(EINVAL);
    history->global_path = snag_path_join(dotdir, "prompt_history");
    history->snapshot.global_path = history->global_path ? strdup(history->global_path) : NULL;
    int fd = history->snapshot.global_path ? snag_open_history(history->global_path) : -1;
    if (fd < 0) { history_note_warning(history); return -1; }
    (void)close(fd);
    return 0;
}

int
snag_history_bind(struct snag_history *history, const char *session_dir)
{
    if (!history || !history->global_path || !session_dir || history->path) return 0;
    char *path = snag_path_join(session_dir, "prompt_history");
    char *copy = path ? strdup(path) : NULL;
    int fd = copy ? snag_open_history(path) : -1;
    int rc = -1;
    if (fd >= 0) {
        int64_t original = history_complete_end(fd), end = 0;
        if (original >= 0 && history_append(fd, &history->snapshot, &end) == 0) {
            history->merged = original;
            history->path = path;
            history->local_fd = fd;
            history->snapshot.local_path = copy;
            history->snapshot.local_end = end;
            path = copy = NULL;
            history_memory_clear(&history->snapshot);
            rc = 0;
        }
        if (rc < 0) (void)close(fd);
    }
    free(path); free(copy);
    if (rc < 0) history_note_warning(history);
    return rc;
}

int
snag_history_merge(struct snag_history *history)
{
    if (!history || !history->global_path ||
        (history->merged == history->snapshot.local_end && !history->snapshot.count)) return 0;
    int fd = history_file_open(history->global_path), rc = -1;
    int64_t original = -1;
    if (fd < 0 || (original = history_complete_end(fd)) < 0 || snag_truncate(fd, original) < 0) goto out;
    if (history->merged < history->snapshot.local_end) {
        int local = history->local_fd;
        if (snag_seek(local, history->merged, SEEK_SET) < 0) goto out;
        unsigned char block[8192];
        int64_t pos = history->merged;
        while (pos < history->snapshot.local_end) {
            size_t n = sizeof(block);
            if (history->snapshot.local_end - pos < (int64_t)n) n = (size_t)(history->snapshot.local_end - pos);
            ssize_t got = read(local, block, n);
            if (got < 0 && errno == EINTR) continue;
            if (got != (ssize_t)n || snag_write_full(fd, block, n) < 0) goto out;
            pos += (int64_t)n;
        }
    }
    int64_t end;
    if (history_append(fd, &history->snapshot, &end) < 0) goto out;
    history->merged = history->snapshot.local_end;
    history_memory_clear(&history->snapshot);
    rc = 0;
out:
    if (rc < 0 && original >= 0) (void)snag_truncate(fd, original);
    if (fd >= 0) (void)close(fd);
    if (rc < 0) history_note_warning(history);
    return rc;
}

int
snag_history_add(struct snag_history *history, const char *text)
{
    if (!history || !text || !*text) return 0;
    bool dropped = false;
    if (history_memory_add(&history->snapshot, text, &dropped) < 0) {
        history_note_warning(history);
        return -1;
    }
    if (dropped) history_note_warning(history);
    if (!history->path) return 0;
    int rc = history_append(history->local_fd, &history->snapshot, &history->snapshot.local_end);
    if (rc == 0) {
        history_memory_clear(&history->snapshot);
    } else history_note_warning(history);
    return rc;
}
