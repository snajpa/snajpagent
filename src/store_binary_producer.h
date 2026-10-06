/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_STORE_BINARY_PRODUCER_H
#define SNAJPAGENT_STORE_BINARY_PRODUCER_H

#include "store.h"
#include "store_binary_event.h"

struct snag_binary_output_source {
    struct snag_binary_public_value value;
    size_t index;
};

struct snag_binary_input_source {
    uint64_t creation;
    unsigned char id[16];
    struct snag_binary_input_reference text, content, instructions;
    json_t *paths;
};

/* Working provenance, not a session-wide source table. Only the pending input,
 * queue and open response own entries. Stage a clone before admission; adopt it
 * with the private reducer candidate after the canonical durable ACK. */
struct snag_binary_producer {
    struct snag_buf field;
    struct snag_binary_output_source *outputs;
    size_t output_count, output_capacity;
    json_t *public;
    struct snag_binary_input_source input, *queue;
    size_t queue_count, queue_capacity;
};

/* Initialized owning destinations. A failed clone leaves the destination intact.
 * Prepared field bytes are scratch, so cloning copies working provenance only. */
int snag_binary_producer_clone(struct snag_binary_producer *destination,
    const struct snag_binary_producer *source);
void snag_binary_producer_free(struct snag_binary_producer *producer);

struct snag_binary_checkpoint_sources;
struct snag_binary_checkpoint_index;
/* Rebuild working writer provenance at an independently admitted state/clock.
 * Caller pins same-journal ancestry, immutable bytes and complete old closure
 * plus bounded suffix in non-NULL access. Exact input/queue declarations and
 * open-response fragments supply canonical offsets; no lifetime replay, writer
 * binding or semantic adoption occurs. out is initialized/owning and changes
 * only on complete success, including cancellation. Descriptor position and
 * state/origins stay unchanged; zero provenance belongs to closed scopes. */
int snag_binary_producer_restore(struct snag_binary_producer *out, int fd,
    const struct snag_binary_anchor *, const struct snag_binary_checkpoint_index *,
    const struct snag_binary_checkpoint_sources *, const struct snag_session *,
    bool (*cancelled)(void *), void *opaque);

/* The caller supplies native, engine-owned process and durable-boundary cursors.
 * Legacy/import presentation offsets alone cannot establish these sequences.
 * Literal field bytes are prepared in producer.field. On error discard the
 * staged producer; on success record borrows the rewritten field. */
int snag_binary_producer_live_result(struct snag_binary_producer *producer,
    const struct snag_session *committed, struct snag_binary_record *record);

/* The caller has strictly reduced the event into state and encoded its literal
 * body into producer.field. Replace repeated input/public-output bytes with typed
 * canonical references, then retain/prune provenance for that candidate. On error
 * discard the staged producer. record is a borrowed view of the resulting field. */
int snag_binary_producer_reference(struct snag_binary_producer *producer,
    const struct snag_session *state, uint64_t sequence, struct snag_binary_record *record,
    const json_t *data);

#endif
