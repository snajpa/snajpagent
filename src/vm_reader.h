/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_VM_READER_H
#define SNAJPAGENT_VM_READER_H

#include "store.h"
#include "wake.h"
#include "wire.h"

struct snag_vm_reader;

struct snag_vm_read_request {
    char session_id[SNAG_ID_HEX_LEN + 1u];
    bool trusted_tail, refresh, reverse;
    struct snag_journal_cursor tail, cursor;
    uint64_t before_seq;
};

struct snag_vm_read_result {
    uint64_t generation;
    struct snag_vm_read_request request;
    struct snag_journal_cursor tail, cursor;
    /* In scan order; each entry is {seq, type, data}. Checkpoints are metadata.
     * Private provider payloads and the reader's secret snapshot are filtered. */
    json_t *events;
    bool best_effort, incomplete, more;
    int error_number;
    char error[256];
};

/* Store remains open and immutable until close. Secrets are copied; rebuilding
 * the reader installs a new configuration/redaction snapshot. One background
 * worker owns journal descriptors and never acquires a session writer lock. */
struct snag_vm_reader *snag_vm_reader_open(struct snag_store *,
    const struct snag_wire_secrets *, char *, size_t);
void snag_vm_reader_close(struct snag_vm_reader *);
/* New visible work cancels the previous request and discards obsolete results.
 * The returned generation identifies the exact viewport request. */
uint64_t snag_vm_reader_request(struct snag_vm_reader *, const struct snag_vm_read_request *);
void snag_vm_reader_cancel(struct snag_vm_reader *);
snag_wake_fd snag_vm_reader_fd(const struct snag_vm_reader *);
/* Nonblocking; ownership transfers to the caller. NULL means no completed work. */
struct snag_vm_read_result *snag_vm_reader_take(struct snag_vm_reader *);
void snag_vm_read_result_free(struct snag_vm_read_result *);

#endif
