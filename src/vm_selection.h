/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_VM_SELECTION_H
#define SNAJPAGENT_VM_SELECTION_H

#include "vm_search.h"

#include <stdio.h>

/* Zero initialized, workspace-local and never part of the model context.
 * The store directory stays open until all registers have been released. */
struct snag_vm_register {
    struct snag_buf text;
    FILE *file;
    uint64_t length;
    int directory;
    char path[64];
    bool lines, block;
};

void snag_vm_register_free(struct snag_vm_register *);
int snag_vm_register_append(struct snag_vm_register *, int directory, const void *, size_t);
int snag_vm_register_read(struct snag_vm_register *, uint64_t offset, void *, size_t);

/* Endpoints name whole graphemes, inclusively. Rectangles use logical display
 * columns [left,right); soft wraps never introduce line breaks in a register. */
enum snag_vm_selection_kind { SNAG_VM_SELECT_NONE, SNAG_VM_SELECT_CHAR,
    SNAG_VM_SELECT_LINE, SNAG_VM_SELECT_BLOCK };
struct snag_vm_selection {
    enum snag_vm_selection_kind kind;
    struct snag_vm_anchor first, last;
    size_t left, right;
};
struct snag_vm_copy;
int snag_vm_anchor_compare(const struct snag_vm_anchor *, const struct snag_vm_anchor *);
struct snag_vm_copy *snag_vm_copy_open(const struct snag_vm_selection *, int directory);
void snag_vm_copy_close(struct snag_vm_copy *);
/* Feed projected blocks in chronological order. 1 means the complete selection
 * was found; 0 needs more source; -1 is failure. Copies only projected bytes. */
int snag_vm_copy_block(struct snag_vm_copy *, const json_t *, bool (*cancel)(void *), void *);
int snag_vm_copy_finish(struct snag_vm_copy *, struct snag_vm_register *,
    bool (*cancel)(void *), void *);

#endif
