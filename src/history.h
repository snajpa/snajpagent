/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_HISTORY_H
#define SNAJPAGENT_HISTORY_H

#include "base.h"

#define SNAG_HISTORY_COUNT 100u
#define SNAG_HISTORY_BYTES (4u * 1024u * 1024u)

struct snag_history_snapshot {
    char *items[SNAG_HISTORY_COUNT];
    size_t count;
    size_t bytes;
    char *local_path, *global_path;
    int64_t local_end;
};

/* Newest to oldest: unsaved input, session file, global file. */
struct snag_history_cursor {
    unsigned int source;
    int64_t offset;
};

struct snag_history_reader {
    int fd[2];
    int64_t size[2], cache_start;
    size_t cache_len;
    unsigned int cache_source;
    unsigned char cache[8192];
    struct snag_buf encoded, decoded;
    size_t budget;
    int64_t scan_pos, scan_first, scan_last;
    unsigned int scan_source;
    bool scanning, scan_complete, scan_oversized;
    bool warning;
};

struct snag_history {
    struct snag_history_snapshot snapshot;
    char *path;
    char *global_path;
    int64_t merged;
    int local_fd;
    bool warning;
    bool warned;
};

void snag_history_free(struct snag_history *history);
void snag_history_snapshot_free(struct snag_history_snapshot *snapshot);
int snag_history_snapshot_copy(struct snag_history_snapshot *out, const struct snag_history_snapshot *source);
int snag_history_open(struct snag_history *history, const char *dotdir);
int snag_history_bind(struct snag_history *history, const char *session_dir);
int snag_history_merge(struct snag_history *history);
int snag_history_add(struct snag_history *history, const char *text);
bool snag_history_take_warning(struct snag_history *history);
void snag_history_reader_close(struct snag_history_reader *reader);
void snag_history_reader_open(struct snag_history_reader *reader,
                              const struct snag_history_snapshot *snapshot);
struct snag_history_cursor snag_history_end(const struct snag_history_snapshot *snapshot);
/* 1: record; 0: boundary; 2: yield (replenish budget); -1: error.
 * Text is borrowed until the next read. Clear scanning to cancel a partial read. */
int snag_history_read(struct snag_history_reader *reader,
                     const struct snag_history_snapshot *snapshot, bool newer,
                     struct snag_history_cursor from, struct snag_history_cursor *start,
                     struct snag_history_cursor *end, const char **text);

#endif
