/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_VM_NAVIGATION_H
#define SNAJPAGENT_VM_NAVIGATION_H

#include "vm_search.h"

enum snag_vm_navigation_kind {
    SNAG_VM_NAV_NONE, SNAG_VM_NAV_LINE, SNAG_VM_NAV_LAST,
    SNAG_VM_NAV_UP, SNAG_VM_NAV_DOWN, SNAG_VM_NAV_LEFT, SNAG_VM_NAV_RIGHT,
    SNAG_VM_NAV_WORD_NEXT, SNAG_VM_NAV_WORD_PREVIOUS, SNAG_VM_NAV_WORD_END,
    SNAG_VM_NAV_LINE_START, SNAG_VM_NAV_LINE_FIRST, SNAG_VM_NAV_LINE_END,
    SNAG_VM_NAV_ROW_UP, SNAG_VM_NAV_ROW_DOWN
};
struct snag_vm_navigation_request {
    enum snag_vm_navigation_kind kind;
    struct snag_vm_anchor start;
    uint64_t count;
    size_t column;
    bool operate;
};
struct snag_vm_navigation;
struct snag_vm_navigation *snag_vm_navigation_open(const struct snag_vm_navigation_request *);
void snag_vm_navigation_close(struct snag_vm_navigation *);
/* Chronological projected blocks, joining contiguous fragments. 1 stops this
 * pass; 0 needs more source; -1 fails. No history-sized index is retained. */
int snag_vm_navigation_block(struct snag_vm_navigation *, const json_t *,
    bool (*cancel)(void *), void *);
/* 0 returns a source anchor, 1 requests another pass over the same pinned source
 * (state already rewound), -1 fails without a usable result. */
int snag_vm_navigation_finish(struct snag_vm_navigation *, struct snag_vm_anchor *);

#endif
