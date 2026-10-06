/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_VM_REPORT_H
#define SNAJPAGENT_VM_REPORT_H

#include "store.h"
#include "vm_document.h"
#include "wire.h"

/* Redact known secrets and replace bytes that JSON text cannot represent. */
int snag_vm_report_text(const unsigned char *, size_t, const struct snag_wire_secrets *,
    bool (*cancel)(void *), void *, struct snag_buf *);
bool snag_vm_report_valid(const json_t *);
/* Reads a private immutable report without a writer lock. Verification,
 * redaction and reflow run on the caller's background worker. */
struct snag_vm_document *snag_vm_report_read(struct snag_store *, const char *session,
    const json_t *report, unsigned int columns, const struct snag_wire_secrets *,
    bool (*cancel)(void *), void *, char *error, size_t size);

#endif
