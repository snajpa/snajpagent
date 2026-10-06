/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_STORE_BINARY_INDEX_H
#define SNAJPAGENT_STORE_BINARY_INDEX_H

#include "store_binary.h"

#define SNAG_BINARY_INDEX_HEADER_SIZE 112u
#define SNAG_BINARY_INDEX_ENTRY_SIZE 96u
#define SNAG_BINARY_INDEX_HASH_SIZE 32u
/* At most one entry per minimum-sized record in a permitted multi-record batch. */
#define SNAG_BINARY_INDEX_BATCH_MAX ((SNAG_BINARY_BATCH_TARGET / \
    SNAG_BINARY_RECORD_HEADER_SIZE) * SNAG_BINARY_INDEX_ENTRY_SIZE)
/* One parent per added leaf plus the carry through at most 64 existing peaks. */
#define SNAG_BINARY_INDEX_TREE_BATCH_MAX (SNAG_BINARY_INDEX_BATCH_MAX + \
    (SNAG_BINARY_INDEX_BATCH_MAX / SNAG_BINARY_INDEX_ENTRY_SIZE + 64u) * \
    SNAG_BINARY_INDEX_HASH_SIZE)

struct snag_binary_index_tree {
    uint64_t count;
    unsigned char peaks[64][SNAG_BINARY_INDEX_HASH_SIZE];
};

struct snag_binary_index_entry {
    uint64_t sequence;
    uint64_t batch_offset;
    uint64_t turn;
    uint32_t record_offset;
    uint16_t kind;
    unsigned char batch_digest[32];
};

#define SNAG_BINARY_CHECKPOINT_INDEX_HEADER_SIZE (24u + 64u * SNAG_BINARY_INDEX_HASH_SIZE)

/* Version-1 checkpoint access metadata. The frontier covers every canonical
 * record; the sorted entry table holds only the caller's working-set locations.
 * Decoded entries borrow immutable bytes. The entire enclosing image must be
 * pinned by a canonical receipt for checkpoint source custody. Query-built views
 * instead prove each row independently beneath an admitted frontier. Merely
 * matching the frontier root cannot authenticate a self-supplied location table. */
struct snag_binary_checkpoint_index {
    struct snag_binary_identity identity;
    struct snag_binary_anchor boundary;
    struct snag_binary_index_tree tree;
    const unsigned char *entries;
    size_t entry_count;
};

/* Atomic append, permitting inputs to borrow out. The frontier count must match
 * the captured boundary; unused peaks are zero and entries strictly ordered.
 * No cache-file geometry check or lifetime record table is required. */
int snag_binary_checkpoint_index_encode(struct snag_buf *out,
    const struct snag_binary_identity *, const struct snag_binary_anchor *,
    const struct snag_binary_index_tree *, const struct snag_binary_index_entry *, size_t count);
/* Exact structural decoding against an independently established boundary/root.
 * Validate every location and frontier slot before replacing out. This does not
 * establish whole-image authority, working-set completeness or source semantics. */
int snag_binary_checkpoint_index_decode(const void *data, size_t size,
    const struct snag_binary_identity *, const struct snag_binary_anchor *,
    const unsigned char root[32], struct snag_binary_checkpoint_index *out);
/* Append an owning encoding of an immutable decoded working-set view. Recheck
 * its structural shape before changing out; inputs may borrow out. Canonical
 * custody, ancestry and complete working-set semantics remain caller obligations. */
int snag_binary_checkpoint_index_copy(struct snag_buf *,
    const struct snag_binary_checkpoint_index *);
/* Binary search in successfully decoded, unchanged metadata: 0 found, 1 absent,
 * -1 invalid. Every nonzero result preserves out. A required absent location is
 * an incomplete checkpoint, never permission to invent content or skip history. */
int snag_binary_checkpoint_index_find(const struct snag_binary_checkpoint_index *,
    uint64_t sequence, struct snag_binary_index_entry *out);

/* Locate a canonical batch using already pinned, immutable checkpoint access
 * metadata. Old sequences must be in that table; missing entries fail ENOENT.
 * Newer sequences use backward lookup through the caller-bounded suffix. The
 * caller establishes the common journal/ancestry and suffix budget separately.
 * NULL access retains full backward lookup for independent repair/oracles.
 * Canonical batch/record checks still apply; no index file is read. Return0/-1,
 * preserving batch/before and fd position on failure; scratch views expire. */
int snag_binary_checkpoint_batch_find(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_index *access, uint64_t sequence,
    struct snag_buf *scratch, struct snag_binary_batch *batch, struct snag_binary_anchor *before);

/* Native next-record cut. before is the independently authenticated anchor
 * before its containing batch; record_offset addresses the decoded image, never
 * the descriptor. Batch boundaries normalize to HEADER_SIZE/anchor.next_seq. */
struct snag_binary_cursor {
    struct snag_binary_anchor before;
    uint64_t next_seq;
    uint32_t record_offset;
};

/* CPU-only capture at [before.next_seq,after.next_seq], from an authenticated
 * immutable decoded batch and its full anchors. Exact record boundary/turn
 * ordinal checks precede atomic replacement; no source authority is granted. */
int snag_binary_cursor_capture(const struct snag_binary_anchor *before,
    const struct snag_binary_anchor *after, const struct snag_binary_batch *, uint64_t next_seq,
    struct snag_binary_cursor *out);

/* Read [cursor.next_seq,end) beneath independently admitted immutable through.
 * Caller establishes common identity/ancestry and cursor membership separately.
 * Visit borrows record and its exact after-record cut: 0 continues,1 accepts and
 * pauses,-1 fails; other values fail EINVAL. Return0 complete,1 paused,-1 error.
 * Cursor changes only on0/1; callbacks must stage effects until then. Batches are
 * read once per traversal, including validation of a partial starting cut.
 * Cancellation before reads/visits/adoption; pread only, no index or tail repair. */
int snag_binary_cursor_read(int fd, const struct snag_binary_anchor *through, uint64_t end,
    struct snag_binary_cursor *,
    int (*visit)(void *, const struct snag_binary_record *, uint64_t,
        const struct snag_binary_cursor *after),
    bool (*cancelled)(void *), void *opaque);

/* Same traversal with non-NULL independently admitted working-set custody.
 * Each visited old record must have an exact canonical entry, otherwise ENOENT.
 * Only the bounded newer suffix is contiguous; missing old entries never grant
 * prefix access. Other cursor/callback/source authority contracts are unchanged. */
int snag_binary_checkpoint_cursor_read(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_index *, uint64_t end, struct snag_binary_cursor *,
    int (*visit)(void *, const struct snag_binary_record *, uint64_t,
        const struct snag_binary_cursor *after),
    bool (*cancelled)(void *), void *opaque);

/* Same guarded traversal for explicitly requested history. Missing old rows
 * require individual verified cache membership under the independently admitted
 * exact through frontier, never a root obtained from the cache. index_fd=-1
 * leaves them unavailable; installed custody works independently of cache health.
 * Every visited old tuple is checked against the canonical containing batch.
 * Identity/ancestry/immutability and query work remain caller obligations. */
int snag_binary_checkpoint_query_cursor_read(int fd, int index_fd,
    const struct snag_binary_anchor *through, const struct snag_binary_checkpoint_index *,
    const struct snag_binary_index_tree *, uint64_t end, struct snag_binary_cursor *,
    int (*visit)(void *, const struct snag_binary_record *, uint64_t,
        const struct snag_binary_cursor *after),
    bool (*cancelled)(void *), void *opaque);

/* Enumerate [first,end) in sequence order. Pinned access selects only listed
 * old working-set records, grouping physical batches; the newer suffix remains
 * contiguous. NULL access enumerates the contiguous independent oracle prefix.
 * The producer/consumer must establish complete closure, common identity,
 * ancestry, immutable bytes and the suffix work bound before using this API.
 * Visit borrows a record until return: 0 continues, -1 aborts with its errno;
 * positive returns are invalid. Callbacks must stage any state until complete
 * success; this iterator does not roll back their outputs. Cancellation checks
 * precede reads and record visits. Uses pread, never seeks/writes. Return0/-1. */
int snag_binary_checkpoint_records_read(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_index *access, uint64_t first, uint64_t end,
    int (*visit)(void *, const struct snag_binary_record *, uint64_t),
    bool (*cancelled)(void *), void *opaque);

/* Draft derived index. Header identity is supplied independently from the
 * canonical journal. Entry checksums bind that identity and their sequence slot;
 * neither checksum grants semantic state or canonical-batch authority. */
void snag_binary_index_header_encode(unsigned char out[SNAG_BINARY_INDEX_HEADER_SIZE],
    const struct snag_binary_identity *identity);
/* 0 exact header, 1 incomplete, -1 malformed/wrong identity. */
int snag_binary_index_header_decode(const void *data, size_t size,
    const struct snag_binary_identity *identity);
int snag_binary_index_header_read(int fd, const struct snag_binary_identity *identity);
int snag_binary_index_entry_encode(unsigned char out[SNAG_BINARY_INDEX_ENTRY_SIZE],
    const struct snag_binary_identity *identity, const struct snag_binary_index_entry *entry);
int snag_binary_index_entry_decode(const void *data, size_t size,
    const struct snag_binary_identity *identity, uint64_t sequence,
    struct snag_binary_index_entry *out);
int snag_binary_index_offset(uint64_t sequence, int64_t *out);
/* Complete append-only forest prefix: fixed-width entries followed by the
 * 32-byte parents completed by each leaf. All older prefix nodes stay in place. */
int snag_binary_index_end(uint64_t count, int64_t *out);

/* RFC 9162 tree hash over encoded entries: H(0 || entry), H(1 || left || right).
 * Logical root calculation is independent of persisted cache addressability.
 * A root supplied from this cache itself grants no canonical membership. */
int snag_binary_index_tree_root(const struct snag_binary_index_tree *, unsigned char out[32]);
/* Append advances a staged frontier atomically. A non-NULL output additionally
 * emits cache bytes and checks their physical file geometry. NULL output advances
 * only the logical frontier, so unavailable/unrepresentable derived storage does
 * not limit canonical journaling. Logical-only entry and batch appends use no
 * allocation; entry/anchor validation still applies. */
int snag_binary_index_tree_append(struct snag_buf *, struct snag_binary_index_tree *,
    const struct snag_binary_identity *, const struct snag_binary_index_entry *);
int snag_binary_index_tree_append_batch(struct snag_buf *, struct snag_binary_index_tree *,
    const struct snag_binary_identity *, const struct snag_binary_anchor *before,
    const struct snag_binary_anchor *after, const void *data, size_t size);
/* Load only the logarithmic forest frontier; compare against an independently
 * established root for this journal prefix before replacing the caller's tree. */
int snag_binary_index_tree_load(int fd, const struct snag_binary_identity *, uint64_t count,
    const unsigned char root[32], struct snag_binary_index_tree *);
/* Verify membership beneath that independently established root. Cache absence
 * returns 1, corruption/errors -1; neither changes out or proves history absent.
 * The resulting location still requires canonical batch/record decoding. */
int snag_binary_index_read_verified(int fd, const struct snag_binary_identity *, uint64_t count,
    const unsigned char root[32], uint64_t sequence, struct snag_binary_index_entry *out);
int snag_binary_index_turn_verified(int fd, const struct snag_binary_identity *, uint64_t count,
    const unsigned char root[32], uint64_t turn, struct snag_binary_index_entry *out);

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
/* Load a previously authenticated entry's canonical record directly. The entry
 * must be established independently as a member of through in this immutable
 * journal, e.g. by a verified tree proof, never by read_hint alone. Validates the
 * batch digest, sequence/record position/kind and actual surrounding turn. Reads
 * at most the containing batch and its predecessor; no lifetime scan. Return
 * 0/-1, preserve out and fd position on error; all older scratch views expire. */
int snag_binary_index_load_record(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_index_entry *entry, struct snag_buf *scratch,
    struct snag_binary_record *out);

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
