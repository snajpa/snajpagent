/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_STORE_BINARY_INDEX_H
#define SNAJPAGENT_STORE_BINARY_INDEX_H

#include "store_binary.h"

#define SNAG_BINARY_INDEX_HEADER_SIZE 112u
#define SNAG_BINARY_INDEX_ENTRY_SIZE 96u
/* At most one entry per minimum-sized record in a permitted multi-record batch. */
#define SNAG_BINARY_INDEX_BATCH_MAX ((SNAG_BINARY_BATCH_TARGET / \
    SNAG_BINARY_RECORD_HEADER_SIZE) * SNAG_BINARY_INDEX_ENTRY_SIZE)

struct snag_binary_index_entry {
    uint64_t sequence;
    uint64_t batch_offset;
    uint64_t turn;
    uint32_t record_offset;
    uint16_t kind;
    unsigned char batch_digest[32];
};

/* Draft derived index. Header identity is supplied independently from the
 * canonical journal. Entry checksums bind that identity and their sequence slot;
 * neither checksum grants semantic state or canonical-batch authority. */
void snag_binary_index_header_encode(unsigned char out[SNAG_BINARY_INDEX_HEADER_SIZE],
    const struct snag_binary_identity *identity);
/* 0 exact header, 1 incomplete, -1 malformed/wrong identity. */
int snag_binary_index_header_decode(const void *data, size_t size,
    const struct snag_binary_identity *identity);
int snag_binary_index_entry_encode(unsigned char out[SNAG_BINARY_INDEX_ENTRY_SIZE],
    const struct snag_binary_identity *identity, const struct snag_binary_index_entry *entry);
int snag_binary_index_entry_decode(const void *data, size_t size,
    const struct snag_binary_identity *identity, uint64_t sequence,
    struct snag_binary_index_entry *out);
int snag_binary_index_offset(uint64_t sequence, int64_t *out);

/* Build one bounded batch's entries, appended atomically. Both anchors must be
 * independently authenticated in this immutable journal. No lifetime map, index
 * publication, durability barrier or application cutover is supplied here. */
int snag_binary_index_append_batch(struct snag_buf *out,
    const struct snag_binary_identity *identity, const struct snag_binary_anchor *before,
    const struct snag_binary_anchor *after, const void *data, size_t size);
/* Check a hint against an independently authenticated containing batch, including
 * actual record boundaries and surrounding turn ordinal. Failure preserves out.
 * Never construct the trusted anchors from the index hint itself. */
int snag_binary_index_resolve(const struct snag_binary_index_entry *entry,
    const struct snag_binary_anchor *before, const struct snag_binary_anchor *after,
    const void *data, size_t size, struct snag_binary_record *out);

/* Positional reads preserve fd position and caller output. Return 1 for missing
 * or torn index data, -1 for errors/corruption. A successful hint still requires
 * canonical resolution. The index may be incomplete without losing history. */
int snag_binary_index_read_hint(int fd, const struct snag_binary_identity *identity,
    uint64_t sequence, struct snag_binary_index_entry *out);
/* Binary search over a caller-bounded prefix of index slots. 1 is unavailable,
 * never proof that a turn does not exist. Resolve the returned turn-start hint
 * canonically before use; no lifetime index scan or rebuild is done here. */
int snag_binary_index_turn_hint(int fd, const struct snag_binary_identity *identity,
    uint64_t indexed_count, uint64_t turn, struct snag_binary_index_entry *out);

#endif
