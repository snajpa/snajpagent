/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_VM_READER_H
#define SNAJPAGENT_VM_READER_H

#include "store.h"
#include "vm_document.h"
#include "wake.h"
#include "wire.h"

struct snag_vm_reader;
enum snag_vm_read_kind { SNAG_VM_READ_HISTORY, SNAG_VM_READ_SESSIONS,
    SNAG_VM_READ_REPORT, SNAG_VM_READ_REPORTS };

struct snag_vm_read_request {
    enum snag_vm_read_kind kind;
    uint64_t stored_limit;
    json_t *report, *known_reports;
    /* Immutable full IDs of other open buffers whose verified descriptors stay
     * cached. Request copies the array. Omit to retain only this request's view. */
    json_t *retained_sessions;
    /* Required if this process holds a writer: never probe/close its lock.
     * The caller preserves ownership through this request's completion. */
    char owned_session_id[SNAG_ID_HEX_LEN + 1u];
    char session_id[SNAG_ID_HEX_LEN + 1u];
    bool trusted_tail, refresh, reverse;
    bool project, if_changed, tail_only;
    unsigned int verbosity, columns;
    /* tail is the owner's bound when trusted; previous is the already
     * displayed bound, independently used by if_changed and tail_only. */
    struct snag_journal_cursor tail, previous, cursor;
    uint64_t before_seq;
};

struct snag_vm_read_result {
    uint64_t generation;
    struct snag_vm_read_request request;
    struct snag_journal_cursor tail, cursor;
    /* In scan order; entries have seq, type, data and original public-text byte
     * counts for offsets through redaction. Checkpoints are metadata.
     * Private provider payloads and the reader's secret snapshot are filtered. */
    json_t *events;
    json_t *catalog;
    /* Projected history owns chronological display blocks instead of events.
     * Raw encoded output and provider payloads remain private to the worker. */
    json_t *blocks;
    struct snag_vm_document *document;
    bool best_effort, incomplete, more, unchanged;
    int error_number;
    char error[256];
};

/* Store remains open and immutable until close. Secrets are copied; rebuilding
 * the reader installs a new configuration/redaction snapshot. One background
 * worker owns read-only journal descriptors and performs catalogue/status reads.
 * The request identifies any writer already owned by the calling process. */
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
