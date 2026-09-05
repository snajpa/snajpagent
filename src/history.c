/* SPDX-License-Identifier: GPL-2.0-only */
#include "history.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
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
    free(history->path);
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
history_memory_add(struct snag_history *term, const char *text, bool *dropped)
{
    size_t len = strlen(text);
    char *copy;

    if (!len || len > SNAG_HISTORY_BYTES)
        return 0;
    copy = snag_strdup_checked(text, SNAG_HISTORY_BYTES);
    if (!copy)
        return -1;
    while (term->snapshot.count == SNAG_HISTORY_COUNT ||
           term->snapshot.bytes > SNAG_HISTORY_BYTES - len) {
        size_t old = strlen(term->snapshot.items[0]);
        if (dropped)
            *dropped = true;
        free(term->snapshot.items[0]);
        memmove(term->snapshot.items, term->snapshot.items + 1u,
                (term->snapshot.count - 1u) * sizeof(term->snapshot.items[0]));
        --term->snapshot.count;
        term->snapshot.bytes -= old;
    }
    term->snapshot.items[term->snapshot.count++] = copy;
    term->snapshot.bytes += len;
    return 0;
}

static int
history_lock(int fd)
{
    struct flock lock;

    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    while (fcntl(fd, F_SETLKW, &lock) < 0)
        if (errno != EINTR)
            return -1;
    return 0;
}

static int
history_file_open(struct snag_history *term)
{
    struct stat st;
    int fd = open(term->path,
                  O_RDWR | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    int flags;

    if (fd < 0)
        return -1;
    if (fstat(fd, &st) < 0) {
        int saved = errno;
        (void)close(fd);
        errno = saved;
        return -1;
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid()) {
        (void)close(fd);
        errno = EACCES;
        return -1;
    }
    flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0 ||
        fchmod(fd, 0600) < 0 || history_lock(fd) < 0) {
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
history_rewrite(struct snag_history *term, int fd)
{
    struct snag_buf encoded;
    int rc = -1;

    snag_buf_init(&encoded, HISTORY_FILE_BYTES);
    if (ftruncate(fd, 0) < 0 || lseek(fd, 0, SEEK_SET) < 0)
        goto out;
    for (size_t i = 0u; i < term->snapshot.count; ++i)
        if (history_encode(&encoded, term->snapshot.items[i]) < 0 ||
            snag_write_full(fd, encoded.data, encoded.len) < 0 ||
            snag_write_full(fd, "\n", 1u) < 0)
            goto out;
    rc = snag_sync_file(fd);
out:
    snag_buf_free(&encoded);
    return rc;
}

static int
history_load_locked(struct snag_history *term, int fd, bool *damaged)
{
    struct stat st;
    struct snag_buf file, decoded;
    size_t pos = 0u;
    bool dirty = false;
    int rc = -1;

    if (fstat(fd, &st) < 0 || st.st_size < 0 ||
        (uintmax_t)st.st_size > HISTORY_FILE_BYTES)
        return -1;
    snag_buf_init(&file, HISTORY_FILE_BYTES + 1u);
    snag_buf_init(&decoded, SNAG_HISTORY_BYTES + 1u);
    if (lseek(fd, 0, SEEK_SET) < 0)
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
    snag_history_snapshot_free(&term->snapshot);
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
        } else if (history_memory_add(term, (char *)decoded.data, &dropped) < 0) {
            goto out;
        } else if (dropped) {
            dirty = true;
        }
        pos += len + 1u;
    }
    rc = dirty ? history_rewrite(term, fd) : 0;
out:
    snag_buf_free(&decoded);
    snag_buf_free(&file);
    return rc;
}

int
snag_history_refresh(struct snag_history *term)
{
    bool damaged = false;
    int fd, rc, saved;

    if (!term->path)
        return 0;
    fd = history_file_open(term);
    if (fd < 0)
        goto fail;
    rc = history_load_locked(term, fd, &damaged);
    saved = errno;
    (void)close(fd);
    errno = saved;
    if (rc < 0)
        goto fail;
    if (damaged)
        history_note_warning(term);
    return 0;
fail:
    history_note_warning(term);
    return -1;
}

int
snag_history_open(struct snag_history *term, const char *dotdir)
{
    struct snag_buf path;

    if (!term || !dotdir || dotdir[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    snag_buf_init(&path, SNAG_PATH_MAX_BYTES);
    if (snag_buf_printf(&path, "%s/prompt_history", dotdir) < 0 ||
        snag_buf_terminate(&path) < 0) {
        snag_buf_free(&path);
        history_note_warning(term);
        return -1;
    }
    free(term->path);
    term->path = (char *)path.data;
    path.data = NULL;
    snag_buf_free(&path);
    return snag_history_refresh(term);
}

int
snag_history_add(struct snag_history *term, const char *text)
{
    struct snag_buf encoded;
    bool damaged = false, dropped = false, retained = false;
    int fd, rc = -1, saved;

    if (!term || !text || !*text)
        return 0;
    if (!term->path)
        return history_memory_add(term, text, NULL);
    fd = history_file_open(term);
    if (fd < 0)
        goto memory;
    if (history_load_locked(term, fd, &damaged) < 0)
        goto close_memory;
    snag_buf_init(&encoded, HISTORY_FILE_BYTES);
    if (history_encode(&encoded, text) < 0 ||
        snag_write_full(fd, encoded.data, encoded.len) < 0 ||
        snag_write_full(fd, "\n", 1u) < 0 ||
        history_memory_add(term, text, &dropped) < 0)
        goto encoded_out;
    retained = true;
    rc = dropped ? history_rewrite(term, fd) : snag_sync_file(fd);
encoded_out:
    snag_buf_free(&encoded);
    saved = errno;
    (void)close(fd);
    errno = saved;
    if (rc == 0) {
        if (damaged)
            history_note_warning(term);
        return 0;
    }
    history_note_warning(term);
    if (!retained)
        (void)history_memory_add(term, text, NULL);
    return -1;
close_memory:
    saved = errno;
    (void)close(fd);
    errno = saved;
memory:
    history_note_warning(term);
    if (history_memory_add(term, text, NULL) < 0)
        return -1;
    return -1;
}
