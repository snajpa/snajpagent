/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_VM_SEARCH_H
#define SNAJPAGENT_VM_SEARCH_H

#include "json.h"

struct snag_vm_search;
struct snag_vm_anchor {
    char key[160];
    uint64_t seq, byte, order;
    bool heading;
};

/* Feed chronological projected blocks. Adjacent pieces of one public item
 * or process stream retain overlap; unrelated bodies are separate text fields.
 * Memory scales with the query, independently of the retained history size. */
struct snag_vm_search *snag_vm_search_open(const char *, bool ignorecase,
    bool reverse, const struct snag_vm_anchor *);
void snag_vm_search_close(struct snag_vm_search *);
/* Stable order within one event: public ordinal, then diagnostic metadata. */
uint64_t snag_vm_search_order(const json_t *block);
int snag_vm_search_block(struct snag_vm_search *, const json_t *, bool (*cancel)(void *), void *);
bool snag_vm_search_result(const struct snag_vm_search *, struct snag_vm_anchor *, bool *wrapped);

#endif
