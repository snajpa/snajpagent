/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_STORE_BINARY_IO_H
#define SNAJPAGENT_STORE_BINARY_IO_H

#include "store_binary_publish.h"
#include "wake.h"

struct snag_binary_io;

/* Syscall seam for deterministic journal and checkpoint publication failures.
 * NULL callbacks use the platform functions. Callback state stays alive until
 * close; callbacks run only on the I/O thread and follow those syscall contracts.
 * Creation is private/exclusive; sync_dir follows snag_sync_dir's 0/1/-1 result. */
struct snag_binary_io_ops {
    int (*write_full)(void *, int, const void *, size_t);
    int (*sync_file)(void *, int);
    void *opaque;
    int (*create_private)(void *, int, const char *);
    int (*rename_at)(void *, int, const char *, int, const char *);
    int (*sync_dir)(void *, int);
};

struct snag_binary_io_result {
    struct snag_binary_anchor written;
    struct snag_binary_anchor durable;
    int error;
    bool retryable;
    bool checkpoint_receipt;
};

/* Caller owns a private regular read/write journal and its exclusive lock.
 * Boundary is independently recovered, durable and exactly at EOF; the caller
 * has already handled incomplete tails. From start until close, this worker has
 * exclusive descriptor/file access. Neither operation closes the descriptor.
 * Semantic staging stays on the engine thread. This worker never reduces state.
 * Session commits stage reducer/provenance outside this worker. Backend
 * selection, independently verified recovery and creation publication remain
 * caller prerequisites; this worker supplies no semantic authority. */
struct snag_binary_io *snag_binary_io_start(int fd,
    const struct snag_binary_anchor *boundary, const struct snag_binary_io_ops *ops);

/* Copy one explicitly grouped transaction into immutable worker-owned inputs.
 * The framing limits bound this copy. Only one unacknowledged engine transaction
 * may branch from the current durable anchor; EBUSY preserves the pending one.
 * Success means queued, never durable or permission to adopt/release effects. */
int snag_binary_io_submit(struct snag_binary_io *io,
    const struct snag_binary_record *records, uint32_t count, uint64_t turns);
snag_wake_fd snag_binary_io_wake(const struct snag_binary_io *io);

/* Return 1 while pending, 0 only for a durable acknowledgement, -1 for error.
 * A completed error fills out and sets errno; incomplete/invalid calls preserve
 * out. Consume each completion once. Written and durable anchors stay distinct
 * after sync failure. Such a failure retains the exact batch and bars new work. */
int snag_binary_io_take(struct snag_binary_io *io, struct snag_binary_io_result *out);

/* The same acknowledgement, additionally transferring ownership of its exact
 * decoded batch on durable success. batch is an initialized owning buffer;
 * success frees/replaces its old storage without allocation or re-encoding.
 * Pending, invalid and failed calls preserve it. Caller releases it separately;
 * subsequent submissions, retries and close cannot invalidate transferred bytes.
 * This includes internally constructed checkpoint receipts. The engine advances
 * its logical frontier from these bytes and its independently known before anchor;
 * this worker still neither reduces state nor confers authority on index hints. */
int snag_binary_io_take_batch(struct snag_binary_io *, struct snag_binary_io_result *,
    struct snag_buf *batch);

/* One explicit reconciliation attempt after consuming an I/O failure. Check
 * existing tail bytes against the retained batch; append only a matching missing
 * suffix, then sync again. Never truncate, resequence or re-encode the batch.
 * A second failure requires fresh recovery before a replacement owner can start. */
int snag_binary_io_retry(struct snag_binary_io *io);

/* Configure once, while idle. Caller owns the private session directory and has
 * independently authenticated both usable generation/receipt ordinal pairs
 * (both zero means unusable). Replace the earlier canonical receipt or an absent
 * slot, keeping its peer. Generation claims do not establish recency. Descriptor
 * lifetime/exclusive writer ownership continue through I/O-owner close. */
int snag_binary_io_checkpoint_setup(struct snag_binary_io *, int directory,
    const uint64_t generations[2], const uint64_t sequences[2]);
/* Move all owned section buffers on success only. The snapshot must describe
 * the owner's current durable boundary. Journal commits take priority between
 * bounded checkpoint chunks/stages. Newer commits may follow this snapshot. */
int snag_binary_io_checkpoint_submit(struct snag_binary_io *, struct snag_binary_io_snapshot *);
/* 1 pending, 0 durably published, -1 error. Only completed calls fill the result.
 * Unsupported directory sync is an error, not a durable-publication claim.
 * Consumed failures retain phase/bytes for a caller-paced retry, without blocking
 * journal acknowledgements. The caller enforces the recovery-suffix budget. */
int snag_binary_io_checkpoint_take(struct snag_binary_io *,
    struct snag_binary_publication_result *);
/* Same result stream, additionally transferring owning access bytes on success.
 * Pending/failed calls preserve access. Publication does not establish usable
 * checkpoint custody: retain these provisionally until canonical receipt ACK. */
int snag_binary_io_checkpoint_take_access(struct snag_binary_io *,
    struct snag_binary_publication_result *, struct snag_buf *access);
int snag_binary_io_checkpoint_retry(struct snag_binary_io *);
/* After consuming successful file publication, queue its exact canonical receipt
 * through the ordinary journal owner. index_root belongs to that captured snapshot,
 * not a newer frontier; timestamp comes from the engine. No engine state is adopted
 * here. Its journal ACK alone marks checkpoint_receipt and makes the slot usable.
 * Failed writes/sync retain the exact request for the existing paced retry. Other
 * journal work may precede this receipt; another checkpoint remains EBUSY until
 * its receipt is durable. Closing idle preserves an unreceipted image. */
int snag_binary_io_checkpoint_receipt_submit(struct snag_binary_io *,
    const unsigned char index_root[32], uint64_t timestamp);

/* EBUSY while a request or unconsumed completion exists. Closing a consumed
 * failure preserves journal bytes and provisional checkpoint files for recovery.
 * It never removes a published slot or closes the caller's directory/journal. */
int snag_binary_io_close(struct snag_binary_io *io);

#endif
