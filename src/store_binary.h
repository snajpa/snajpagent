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
#define SNAG_BINARY_REF_SIZE 16u
#define SNAG_BINARY_LEGACY_CHECKPOINT 0x8000u
#define SNAG_BINARY_LEGACY_CHECKPOINT_SIZE 48u
#define SNAG_BINARY_CHECKPOINT_HEADER_SIZE 160u
#define SNAG_BINARY_CHECKPOINT_FOOTER_SIZE 48u

/* Imported derived checkpoint: original JSONL coordinates and chain hash.
 * Metadata only; it supplies neither state nor a native seek boundary. */
struct snag_binary_legacy_checkpoint {
    uint64_t start, end;
    unsigned char digest[32];
};

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

struct snag_binary_checkpoint_section {
    uint16_t version;
    const unsigned char *data;
    size_t size;
};

/* Frame only: both sections require their own field-shaped semantic codecs.
 * A frame checksum does not validate those fields or authenticate its anchor. */
struct snag_binary_checkpoint_frame {
    struct snag_binary_identity identity;
    struct snag_binary_anchor boundary;
    uint64_t generation;
    struct snag_binary_checkpoint_section core, provider;
};

/* Fixed-size framing state. Sections stay immutable/alive until completion and
 * cannot borrow this encoder's storage. Initialize before requesting chunks. */
struct snag_binary_checkpoint_encoder {
    struct snag_binary_checkpoint_frame frame;
    struct snag_sha256 hash;
    unsigned char header[SNAG_BINARY_CHECKPOINT_HEADER_SIZE];
    unsigned char footer[SNAG_BINARY_CHECKPOINT_FOOTER_SIZE];
    size_t position;
    size_t total;
};

/* No snapshot-sized allocation or per-event cap. Init preserves out on failure.
 * Next advances by at most budget bytes, returning a borrowed chunk (0), complete
 * (1), or error (-1). EOF/error preserve chunk outputs; invalid arguments also
 * preserve encoder state. Callers can yield between chunks. This supplies framing
 * only, without section semantics, publication, durability or resume authority. */
int snag_binary_checkpoint_encoder_init(struct snag_binary_checkpoint_encoder *out,
    const struct snag_binary_checkpoint_frame *frame);
int snag_binary_checkpoint_encoder_next(struct snag_binary_checkpoint_encoder *encoder,
    size_t budget, const unsigned char **data, size_t *size);

/* Append atomically. Section bytes may borrow the destination buffer. The caller
 * supplies a nonzero generation and two nonempty, separately versioned sections.
 * No per-event limit applies to the complete active-state snapshot. */
int snag_binary_checkpoint_frame_encode(struct snag_buf *out,
    const struct snag_binary_checkpoint_frame *frame);
/* Match a caller-authenticated identity/boundary from the same immutable journal.
 * Never promote the frame's own boundary to trusted state. Returns 0 for exactly
 * one verified frame, 1 for an incomplete prefix, -1 for invalid/mismatched bytes.
 * Success borrows section views, retaining their versions for mandatory semantic
 * decoding before adoption. Failure/incompleteness leaves frame unchanged. */
int snag_binary_checkpoint_frame_decode(const void *data, size_t size,
    const struct snag_binary_identity *identity, const struct snag_binary_anchor *boundary,
    struct snag_binary_checkpoint_frame *frame);

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

/* A slice of one canonical record's payload in the same journal. The
 * containing typed field determines the expected record kind/version. */
struct snag_binary_ref {
    uint64_t sequence;
    uint32_t offset, size;
};

int snag_binary_ref_encode(unsigned char out[SNAG_BINARY_REF_SIZE],
    const struct snag_binary_ref *reference);
int snag_binary_ref_decode(const void *data, size_t size, struct snag_binary_ref *reference);
int snag_binary_legacy_checkpoint_encode(struct snag_buf *out,
    const struct snag_binary_legacy_checkpoint *checkpoint);
int snag_binary_legacy_checkpoint_decode(const struct snag_binary_record *record,
    struct snag_binary_legacy_checkpoint *checkpoint);
/* Resolve only against a verified immutable batch from this journal. Validate
 * sequence, required record status, expected type/version and slice bounds
 * before exposing a view. Required state cannot depend on optional metadata. */
int snag_binary_ref_resolve(const struct snag_binary_ref *reference,
    const struct snag_binary_batch *batch, uint16_t kind, uint16_t version,
    const unsigned char **view);

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

/* Walk backward from an already trusted committed anchor in the same immutable
 * journal prefix. The caller owns identity/immutability; an untrusted index entry is
 * not such an anchor. Verify the current batch against that digest and verify
 * its predecessor to recover the predecessor's turn count. Return 0 for a batch,
 * 1 only at a verified file-header boundary, -1 on corruption/read/allocation
 * failure. Missing committed bytes are errors, never an incomplete-tail result.
 * No fd seeks or writes. Each call invalidates older scratch views; batch/before
 * remain unchanged on EOF/error. Success borrows scratch for the current batch
 * and returns a full predecessor anchor usable by either direction of reading. */
int snag_binary_batch_previous(int fd, const struct snag_binary_anchor *after,
    struct snag_buf *scratch, struct snag_binary_batch *batch, struct snag_binary_anchor *before);

/* Find an existing sequence by walking backward from a trusted snapshot anchor.
 * Uses bounded scratch, not an untrusted seek hint or a scan for magic bytes.
 * The caller still validates field role, causal ordering and owner identity.
 * Return 0/-1; batch/before stay unchanged on error, older scratch views expire.
 * Work is proportional to distance from through; this is not the durable index. */
int snag_binary_batch_find(int fd, const struct snag_binary_anchor *through, uint64_t sequence,
    struct snag_buf *scratch, struct snag_binary_batch *batch, struct snag_binary_anchor *before);
/* Iterate only an immutable batch returned by successful decode. The cursor is
 * initially SNAG_BINARY_BATCH_HEADER_SIZE. Payload views borrow its bytes. */
int snag_binary_record_next(const struct snag_binary_batch *batch, size_t *cursor,
    struct snag_binary_record *record, uint64_t *sequence);

#endif
