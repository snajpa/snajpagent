/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_STORE_BINARY_H
#define SNAJPAGENT_STORE_BINARY_H

#include "base.h"

#define SNAG_BINARY_HEADER_SIZE 96u
#define SNAG_BINARY_BATCH_HEADER_SIZE 112u
#define SNAG_BINARY_BATCH_FOOTER_SIZE 80u
#define SNAG_BINARY_RECORD_HEADER_SIZE 32u
#define SNAG_BINARY_BATCH_TARGET (1024u * 1024u)
#define SNAG_BINARY_BATCH_MAX (SNAG_MAX_EVENT_LINE + SNAG_BINARY_RECORD_HEADER_SIZE + \
    SNAG_BINARY_BATCH_HEADER_SIZE + SNAG_BINARY_BATCH_FOOTER_SIZE)
#define SNAG_BINARY_RECORD_OPTIONAL 1u

/* Draft 0.1: framing only. Runtime storage remains JSONL until the typed event,
 * full-state checkpoint and conversion implementations are complete. */
struct snag_binary_identity {
    unsigned char id[16];
    uint64_t created_ms;
};

/* A verified boundary. The first predecessor digest binds the immutable header;
 * later digests bind the entire previous batch. These are file byte offsets. */
struct snag_binary_anchor {
    uint64_t end, next_seq, turns, previous;
    unsigned char digest[32];
};

struct snag_binary_record {
    uint16_t kind, version;
    uint32_t flags;
    uint64_t timestamp_ms;
    const unsigned char *payload;
    size_t size;
};

struct snag_binary_batch {
    const unsigned char *data;
    size_t size;
    uint32_t count;
    uint64_t first_seq;
};

void snag_binary_header_encode(unsigned char out[SNAG_BINARY_HEADER_SIZE],
    const struct snag_binary_identity *identity);
/* Decode returns 0 for complete verified framing, 1 for an incomplete prefix,
 * or -1 for invalid bytes. A failure leaves all output arguments unchanged.
 * Batch decode adopts no semantic state: typed payload codecs must additionally
 * reject unknown required record kinds/versions before applying transitions. */
int snag_binary_header_decode(const void *data, size_t size,
    struct snag_binary_identity *identity, struct snag_binary_anchor *anchor);
int snag_binary_batch_encode(struct snag_buf *out, const struct snag_binary_anchor *anchor,
    const struct snag_binary_record *records, uint32_t count, uint64_t turns);
int snag_binary_batch_decode(const void *data, size_t size,
    const struct snag_binary_anchor *anchor, struct snag_binary_batch *batch,
    struct snag_binary_anchor *next);
/* Read one bounded batch beneath a caller-verified immutable file boundary.
 * Return 1 for EOF/incomplete tail, -1 for corruption/read failure, 0 for a
 * verified batch. Never changes the fd offset or file. Reuses scratch storage;
 * each call invalidates earlier payload views, even when it fails. The caller
 * owns the writer lock, source identity checks and any subsequent tail repair. */
int snag_binary_batch_read(int fd, uint64_t boundary,
    const struct snag_binary_anchor *anchor, struct snag_buf *scratch,
    struct snag_binary_batch *batch, struct snag_binary_anchor *next);
/* Iterate only an immutable batch returned by successful decode. The cursor is
 * initially SNAG_BINARY_BATCH_HEADER_SIZE. Payload views borrow its bytes. */
int snag_binary_record_next(const struct snag_binary_batch *batch, size_t *cursor,
    struct snag_binary_record *record, uint64_t *sequence);

#endif
