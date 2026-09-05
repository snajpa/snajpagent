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
};

struct snag_history {
    struct snag_history_snapshot snapshot;
    char *path;
    bool warning;
    bool warned;
};

void snag_history_free(struct snag_history *history);
void snag_history_snapshot_free(struct snag_history_snapshot *snapshot);
int snag_history_snapshot_copy(struct snag_history_snapshot *out,
                              const struct snag_history_snapshot *source);
int snag_history_open(struct snag_history *history, const char *dotdir);
int snag_history_refresh(struct snag_history *history);
int snag_history_add(struct snag_history *history, const char *text);
bool snag_history_take_warning(struct snag_history *history);

#endif
