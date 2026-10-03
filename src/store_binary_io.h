/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_STORE_BINARY_IO_H
#define SNAJPAGENT_STORE_BINARY_IO_H

#include "store_binary.h"
#include "wake.h"

struct snag_binary_io;

/* Narrow syscall seam for deterministic partial-write and ambiguous-sync tests.
 * NULL callbacks use the platform functions. Callback state stays alive until
 * close; callbacks run only on the I/O thread and follow those syscall contracts. */
struct snag_binary_io_ops {
    int (*write_full)(void *, int, const void *, size_t);
    int (*sync_file)(void *, int);
    void *opaque;
};

struct snag_binary_io_result {
    struct snag_binary_anchor written;
    struct snag_binary_anchor durable;
    int error;
    bool retryable;
};

/* Caller owns a private regular read/write journal and its exclusive lock.
 * Boundary is independently recovered, durable and exactly at EOF; the caller
 * has already handled incomplete tails. From start until close, this worker has
 * exclusive descriptor/file access. Neither operation closes the descriptor.
 * Semantic staging stays on the engine thread. This worker never reduces state.
 * No application cutover, checkpoint/index maintenance or creation publication
 * is provided by this test-linked journal owner yet. */
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

/* One explicit reconciliation attempt after consuming an I/O failure. Check
 * existing tail bytes against the retained batch; append only a matching missing
 * suffix, then sync again. Never truncate, resequence or re-encode the batch.
 * A second failure requires fresh recovery before a replacement owner can start. */
int snag_binary_io_retry(struct snag_binary_io *io);

/* EBUSY while a request or unconsumed completion exists. Closing a consumed
 * failure preserves all journal bytes and requires recovery before reuse. */
int snag_binary_io_close(struct snag_binary_io *io);

#endif
