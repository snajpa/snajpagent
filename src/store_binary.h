/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_STORE_BINARY_H
#define SNAJPAGENT_STORE_BINARY_H

#include "base.h"

#define SNAG_BINARY_HEADER_SIZE 96u
#define SNAG_BINARY_BATCH_HEADER_SIZE 112u
#define SNAG_BINARY_BATCH_FOOTER_SIZE 80u
#define SNAG_BINARY_RECORD_HEADER_SIZE 32u
#define SNAG_BINARY_BATCH_TARGET (1024u * 1024u)
/* Default recovery-work budget, plus one final permitted physical batch. */
#define SNAG_BINARY_SUFFIX_TARGET (32u * 1024u * 1024u)
#define SNAG_BINARY_BATCH_MAX (SNAG_MAX_EVENT_LINE + SNAG_BINARY_RECORD_HEADER_SIZE + \
    SNAG_BINARY_BATCH_HEADER_SIZE + SNAG_BINARY_BATCH_FOOTER_SIZE)
#define SNAG_BINARY_RECORD_OPTIONAL 1u
#define SNAG_BINARY_REF_SIZE 16u
#define SNAG_BINARY_LEGACY_CHECKPOINT 0x8000u
#define SNAG_BINARY_LEGACY_CHECKPOINT_SIZE 48u
#define SNAG_BINARY_CHECKPOINT_RECEIPT 0x8001u
#define SNAG_BINARY_CHECKPOINT_RECEIPT_VERSION 2u
#define SNAG_BINARY_CHECKPOINT_RECEIPT_SIZE 152u
#define SNAG_BINARY_CHECKPOINT_HEADER_SIZE 160u
#define SNAG_BINARY_CHECKPOINT_FOOTER_SIZE 48u

/* Imported derived checkpoint: original JSONL coordinates and chain hash.
 * Metadata only; it supplies neither state nor a native seek boundary. */
struct snag_binary_legacy_checkpoint {
    uint64_t start, end;
    unsigned char digest[32];
};

/* Draft 0.2: zero-free physical batch envelope. Runtime storage remains JSONL
 * until native backend and conversion integration are complete. */
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

/* Optional canonical metadata pins one immutable checkpoint image and the
 * index-format-0.2 tree root at its captured boundary. This decoded value alone
 * does not establish membership in the journal or snapshot semantic validity. */
struct snag_binary_checkpoint_receipt {
    uint64_t generation, image_size;
    struct snag_binary_anchor boundary;
    unsigned char image_digest[32];
    unsigned char index_root[32];
};

struct snag_binary_checkpoint_section {
    uint16_t version;
    const unsigned char *data;
    size_t size;
};

/* Draft frame 0.2: sections require their own field-shaped semantic codecs.
 * A frame checksum does not validate those fields or authenticate its anchor. */
struct snag_binary_checkpoint_frame {
    struct snag_binary_identity identity;
    struct snag_binary_anchor boundary;
    uint64_t generation;
    /* Decoders retain the verified footer digest for later receipt joins.
     * Encoders derive it from sections; it is not an additional wire field. */
    unsigned char image_digest[32];
    struct snag_binary_checkpoint_section core, provider, access;
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
 * supplies a nonzero generation and nonempty core/provider sections. Access
 * metadata is separately versioned; zero version/size means absent and cannot
 * support bounded resume. Full-prefix verification can reconstruct from journal.
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
    size_t size; /* Decoded image length; record positions address this view. */
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
/* Encode version 2 atomically. The image size bounds cache loading independently
 * of cache-supplied fields. Decode returns 0 for supported metadata, 1 for
 * an unknown optional version/index format, -1 for malformed supported fields.
 * Every nonzero result preserves out. A caller using the receipt must separately
 * establish its canonical membership and earlier committed capture boundary. */
int snag_binary_checkpoint_receipt_encode(struct snag_buf *out,
    const struct snag_binary_checkpoint_receipt *receipt);
int snag_binary_checkpoint_receipt_decode(const struct snag_binary_record *record,
    struct snag_binary_checkpoint_receipt *out);
/* Match the frame's identity, generation, exact size/boundary and complete image
 * checksum to an independently authenticated receipt. Same 0/1/-1 framing and
 * borrowed-view rules as frame_decode. The section codecs and the index
 * frontier still require validation; this is not a journal bootstrap routine. */
int snag_binary_checkpoint_frame_from_receipt(const void *data, size_t size,
    const struct snag_binary_identity *identity,
    const struct snag_binary_checkpoint_receipt *receipt,
    struct snag_binary_checkpoint_frame *out);
/* Probe only the final digest of a regular cache file, without reading its body.
 * This is an untrusted lookup key, never a checkpoint pin. Returns 0/-1 and
 * preserves digest on failure. Neither probe nor read seeks the descriptor. */
int snag_binary_checkpoint_image_probe(int fd, unsigned char digest[32]);
/* Read an image bounded by an independently authenticated canonical receipt.
 * Reject nonregular/extent-mismatched files before allocation, then read in 64KiB
 * chunks and verify framing, identity, receipt binding and unchanged file stamps.
 * Cancellation is checked before allocation, between chunks and after validation.
 * Success replaces image with owned bytes and out with borrowed section views;
 * failure preserves both outputs. Their storage must be independent. No adoption,
 * section semantics or journal ancestry checks; no implicit history fallback. */
int snag_binary_checkpoint_image_read(int fd, const struct snag_binary_identity *identity,
    const struct snag_binary_checkpoint_receipt *receipt, struct snag_buf *image,
    struct snag_binary_checkpoint_frame *out, bool (*cancelled)(void *), void *opaque);
/* Find the latest receipt for each supplied image digest (NULL means absent)
 * by walking backward from an independently trusted immutable committed tail.
 * Follow each selected capture boundary to its exact ancestor before returning
 * its bit (1/2) in the result. Zero means neither image is pinned in this window;
 * -1 means corruption, read/allocation failure or cancellation (ECANCELED).
 * floor is the oldest eligible physical boundary, not a cache-supplied anchor.
 * Work stays between through and floor plus one predecessor batch. Cancellation
 * is checked between batches. Output slots without a returned bit, and both
 * slots on failure, remain unchanged. Optional sequences receives the verified
 * canonical receipt ordinal for each returned slot under the same atomic rule;
 * use it for cross-slot recency, never an image's claimed generation. No descriptor
 * seeks, writes or adoption. Image/section/frontier and locking remain caller duties. */
int snag_binary_checkpoint_receipts_find(int fd, const struct snag_binary_anchor *through,
    uint64_t floor, const unsigned char *const images[2], struct snag_buf *scratch,
    struct snag_binary_checkpoint_receipt out[2], uint64_t sequences[2],
    bool (*cancelled)(void *), void *opaque);
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
/* Encode a decoded batch image; wire_encode supplies its physical envelope.
 * Optionally return the physical anchor computed from those same bytes.
 * Both bytes and next remain unchanged on failure. next may alias anchor, but
 * its storage must be independent of out's buffer. */
int snag_binary_batch_encode(struct snag_buf *out, const struct snag_binary_anchor *anchor,
    const struct snag_binary_record *records, uint32_t count, uint64_t turns,
    struct snag_binary_anchor *next);
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

/* Discover the physical committed end from this descriptor's captured EOF, never
 * from an index/checkpoint hint. Caller owns identity checks, an immutable prefix
 * and exclusive writer/repair ownership. Scans bounded envelope delimiters and
 * verifies the last batch plus its immediate predecessor, then checks any open
 * tail. Corrupt closed frames are errors, never skipped. Returns 0/-1, preserving
 * all outputs on error and never seeking/writing. Scratch views always expire.
 * A header-only file returns its root (next_seq=1); session creation and all other
 * record semantics remain unverified. Earlier-prefix integrity, receipt ancestry
 * and snapshot admission are separate checks; this is not a full replay. */
int snag_binary_journal_tail(int fd, uint64_t boundary, struct snag_buf *scratch,
    struct snag_binary_identity *identity, struct snag_binary_anchor *out, uint64_t *incomplete);

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

/* Read a batch at an independently authenticated offset/digest beneath an
 * immutable committed boundary. A bare index hint or a digest read from the
 * target file is insufficient. Reads this batch and its immediate predecessor,
 * without walking the lifetime chain; returns fully checked anchors on either
 * side. Identity/immutability and prefix membership remain caller obligations.
 * Return 0/-1; missing committed bytes are errors. No seeks/writes. Outputs stay
 * unchanged on failure, but all earlier scratch views expire on every call. */
int snag_binary_batch_at(int fd, uint64_t boundary, uint64_t offset,
    const unsigned char digest[32], struct snag_buf *scratch, struct snag_binary_batch *batch,
    struct snag_binary_anchor *before, struct snag_binary_anchor *after);

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
