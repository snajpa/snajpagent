/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_VM_LAYOUT_H
#define SNAJPAGENT_VM_LAYOUT_H

#include "json.h"

enum snag_vm_split { SNAG_VM_HORIZONTAL, SNAG_VM_VERTICAL };

struct snag_vm_layout;
struct snag_vm_rectangle {
    uint64_t window;
    unsigned int row, column, rows, columns;
    bool visible;
};

struct snag_vm_layout *snag_vm_layout_new(uint64_t window);
void snag_vm_layout_free(struct snag_vm_layout *);
bool snag_vm_layout_contains(const struct snag_vm_layout *, uint64_t window);
size_t snag_vm_layout_count(const struct snag_vm_layout *);
/* New window becomes the second child. Failure preserves the previous tree. */
int snag_vm_layout_split(struct snag_vm_layout *, uint64_t existing, uint64_t added,
    enum snag_vm_split);
/* The final leaf stays present; the frontend decides the last-window action. */
int snag_vm_layout_close(struct snag_vm_layout *, uint64_t window);
void snag_vm_layout_equalize(struct snag_vm_layout *);
/* Adjust the nearest containing split on this axis, in percentage points. */
int snag_vm_layout_resize(struct snag_vm_layout *, uint64_t window, enum snag_vm_split,
    int percentage_points);
/* Emit every leaf, including hidden ones. Shrink preserves the tree and gives
 * the focused branch priority; one-cell windows remain inspectable. */
int snag_vm_layout_place(const struct snag_vm_layout *, uint64_t focus,
    unsigned int rows, unsigned int columns,
    int (*emit)(void *, const struct snag_vm_rectangle *), void *);
json_t *snag_vm_layout_json(const struct snag_vm_layout *);
struct snag_vm_layout *snag_vm_layout_load(const json_t *, char *, size_t);

#endif
