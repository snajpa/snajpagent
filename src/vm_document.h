/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_VM_DOCUMENT_H
#define SNAJPAGENT_VM_DOCUMENT_H

#include "json.h"

struct snag_vm_document;
struct snag_vm_document_row {
    size_t block, begin, end, column;
    bool heading;
};

/* Immutable projected blocks, with a sparse row index. Building/reflow belongs
 * on the reader thread. Looking up a visible row scans at most 128 rows, without
 * allocating an entry for every cell-wide row of a large output record. */
struct snag_vm_document *snag_vm_document_open(json_t *blocks, unsigned int columns,
    bool (*cancel)(void *), void *);
struct snag_vm_document *snag_vm_document_ref(struct snag_vm_document *);
void snag_vm_document_free(struct snag_vm_document *);
size_t snag_vm_document_rows(const struct snag_vm_document *);
unsigned int snag_vm_document_columns(const struct snag_vm_document *);
int snag_vm_document_row(const struct snag_vm_document *, size_t, struct snag_vm_document_row *);
const json_t *snag_vm_document_block(const struct snag_vm_document *, size_t);
const char *snag_vm_document_text(const struct snag_vm_document *,
    const struct snag_vm_document_row *);
/* Locate an anchor after reflow/reprojection; a hidden block resolves to the
 * nearest later source event, or the final row when it is beyond the page. */
size_t snag_vm_document_locate(const struct snag_vm_document *, const char *key,
    uint64_t seq, size_t byte, bool heading);

#endif
