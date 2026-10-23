/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_EDIT_HISTORY_H
#define SNAJPAGENT_EDIT_HISTORY_H

#include "json.h"

struct snag_edit;
struct snag_edit_history {
    struct snag_edit *undo, *redo;
    struct snag_buf original;
    size_t cursor;
    bool grouping;
};

void snag_edit_history_free(struct snag_edit_history *);
int snag_edit_begin(struct snag_edit_history *, const struct snag_buf *, size_t cursor);
int snag_edit_end(struct snag_edit_history *, const struct snag_buf *, size_t cursor);
int snag_edit_undo(struct snag_edit_history *, struct snag_buf *, size_t *cursor,
    bool redo, size_t count);
json_t *snag_edit_history_save(const struct snag_edit_history *);
int snag_edit_history_load(struct snag_edit_history *, const json_t *);

#endif
