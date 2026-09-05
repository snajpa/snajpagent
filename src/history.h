/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_HISTORY_H
#define SNAJPAGENT_HISTORY_H

#include "base.h"

#define SNJ_HISTORY_COUNT 100u
#define SNJ_HISTORY_BYTES (4u * 1024u * 1024u)

struct snj_history_snapshot {
    char *items[SNJ_HISTORY_COUNT];
    size_t count;
    size_t bytes;
};

struct snj_history {
    struct snj_history_snapshot snapshot;
    char *path;
    bool warning;
    bool warned;
};

void snj_history_free(struct snj_history *history);
void snj_history_snapshot_free(struct snj_history_snapshot *snapshot);
int snj_history_snapshot_copy(struct snj_history_snapshot *out,
                              const struct snj_history_snapshot *source);
int snj_history_open(struct snj_history *history, const char *dotdir);
int snj_history_refresh(struct snj_history *history);
int snj_history_add(struct snj_history *history, const char *text);
bool snj_history_take_warning(struct snj_history *history);

#endif
