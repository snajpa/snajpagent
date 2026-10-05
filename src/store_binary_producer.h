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

/* The caller has strictly reduced the event into state and encoded its literal
 * body into producer.field. Replace repeated input/public-output bytes with typed
 * canonical references, then retain/prune provenance for that candidate. On error
 * discard the staged producer. record is a borrowed view of the resulting field. */
int snag_binary_producer_reference(struct snag_binary_producer *producer,
    const struct snag_session *state, uint64_t sequence, struct snag_binary_record *record,
    const json_t *data);

#endif
