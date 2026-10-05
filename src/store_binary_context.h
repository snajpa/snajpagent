/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_STORE_BINARY_CONTEXT_H
#define SNAJPAGENT_STORE_BINARY_CONTEXT_H

#include "context.h"
#include "store_binary_replay.h"

/* Full stopped-journal replay, with a provider event seam attached to the
 * resulting state-only session. Source locking, immutable-source verification,
 * diagnostics and origin ownership follow snag_store_reconcile_binary.
 * Only complete success replaces restored and optional sources. No provider
 * request, process, native checkpoint, suffix cursor or writer is created.
 * The first projection rebuilds through the existing context renderer, without
 * reparsing JSONL. Later projections use its ordinary incremental cache.
 * These native adapters remain test-linked, not application resume. */
int snag_store_reconcile_binary_context(struct snag_session *source,
    struct snag_session *restored, struct snag_binary_recovery *recovery,
    struct snag_binary_checkpoint_sources *sources, const struct snag_context_control *control,
    char *error, size_t error_size);

/* The same staged adoption at an exact committed prefix. Both semantic capture
 * and historical IRC lookup stop there. Later journal bytes stay uninspected;
 * this API supplies no tail-repair or checkpoint/suffix-resume authority. */
int snag_store_reconcile_binary_context_prefix(struct snag_session *source,
    struct snag_session *restored, const struct snag_binary_anchor *prefix,
    struct snag_binary_recovery *recovery, struct snag_binary_checkpoint_sources *sources,
    const struct snag_context_control *control, char *error, size_t error_size);

/* Joint state-only core/provider materialization from a frame already decoded
 * and pinned against this independently authenticated canonical receipt.
 * The caller establishes latest membership, capture ancestry, complete producer
 * closure and immutable frame/source ownership under the exclusive writer lock.
 * Compare common frame/receipt/index/section boundaries, resolve all required
 * sources through pinned access and recheck source stamps before joint adoption.
 * Work uses the working-set table and canonical batches; no prefix replay or
 * index file is read. Missing/unsupported access is unavailable (ENOTSUP), never
 * an implicit lifetime fallback. Cancellation is between materialization phases
 * and provider rows; both initialized outputs remain unchanged on failure.
 * Bytes after capture stay uninspected. No suffix/tail-repair, live resources,
 * provider configuration, process or voice-device ownership is installed. */
int snag_store_materialize_binary_context_checkpoint(struct snag_session *source,
    struct snag_session *restored, const struct snag_binary_checkpoint_frame *frame,
    const struct snag_binary_checkpoint_receipt *receipt,
    struct snag_binary_checkpoint_sources *sources, const struct snag_context_control *control,
    char *error, size_t error_size);

/* Joint materialization followed by exact bounded suffix reduction and current
 * historical lookup. stop is independently established, within the caller's
 * byte-work budget; its later bytes remain uninspected with no tail authority.
 * available may supplement the embedded old table with independently proved
 * required locations at the same capture/frontier. NULL uses the embedded table,
 * never a lifetime fallback. Caller supplies complete old dependencies needed
 * by actual suffix fields and current IRC lookup, including the canonical next-row
 * discriminator retained by legacy admission lookup, not guessed stream ordinals.
 * Missing required locations/structured labels fail unavailable. All frame/receipt/locking
 * prerequisites above apply. Failure preserves both initialized output owners;
 * success installs state-only core/origins/provider capture and no live resources. */
int snag_store_resume_pinned_binary_context_checkpoint(struct snag_session *source,
    struct snag_session *restored, const struct snag_binary_checkpoint_frame *frame,
    const struct snag_binary_checkpoint_receipt *receipt, const struct snag_binary_anchor *stop,
    const struct snag_binary_checkpoint_index *available,
    struct snag_binary_checkpoint_sources *sources, const struct snag_context_control *control,
    char *error, size_t error_size);

/* Read-only admission from the actual physical tail and two caller-owned image
 * descriptors (-1 absent). floor is the independently set oldest eligible capture
 * offset, not cache-supplied; receipt/ancestry work stays in that window plus one
 * predecessor. Probe footer keys, authenticate both canonical receipts and select
 * their latest ordinal, then read exactly its pinned image and jointly restore
 * through the discovered tail. A damaged/unsupported selected image fails; no
 * implicit lifetime scan, older-state adoption, rewrite or truncation follows.
 * Optional available[slot] supplements that capture under the independent proof
 * contract above. Caller owns immutable source/images and exclusive writer lock.
 * Success atomically installs state-only core/origins/provider; recovery counts
 * only suffix batches and reports physical open-tail bytes without repair authority.
 * Missing image/pin is ENOENT. Failure preserves all initialized output owners. */
int snag_store_admit_binary_context_checkpoint(struct snag_session *source,
    struct snag_session *restored, const int images[2], uint64_t floor,
    const struct snag_binary_checkpoint_index *const available[2],
    struct snag_binary_recovery *recovery, struct snag_binary_checkpoint_sources *sources,
    const struct snag_context_control *control, char *error, size_t error_size);

/* Slow independent checkpoint verifier. Strict replay establishes prefix authority;
 * both decoded frame sections must equal the canonical core and provider recipe
 * at that boundary. All source-recheck/atomic adoption rules above apply. Success
 * adopts core state read from the frame and a provider seam materialized from
 * its recipe, using the replay-verified canonical payload pool. Voice roots are
 * reconstructed from native adoption references. This still reads the complete
 * prefix: no efficient loader, suffix cursor or publication is supplied. Access
 * metadata is unused here; this verifier grants no location-table authority. */
int snag_store_verify_binary_context_checkpoint(struct snag_session *source,
    struct snag_session *restored, const struct snag_binary_anchor *prefix,
    const void *checkpoint, size_t checkpoint_size, struct snag_binary_recovery *recovery,
    struct snag_binary_checkpoint_sources *sources, const struct snag_context_control *control,
    char *error, size_t error_size);

/* Verify/materialize the checkpoint as above, then strictly reduce its suffix
 * and rebuild the current historical lookup closure before joint adoption.
 * The entire operation rechecks the locked source. No failure replaces restored
 * or optional sources, or supplies tail-repair authority. Successful recovery
 * describes the full verified journal (prefix plus suffix batches) and any
 * incomplete tail, which this read-only consumer never truncates. This still
 * replays the prefix and is test-linked, not efficient application resume. */
int snag_store_resume_binary_context_checkpoint(struct snag_session *source,
    struct snag_session *restored, const struct snag_binary_anchor *prefix,
    const void *checkpoint, size_t checkpoint_size, struct snag_binary_recovery *recovery,
    struct snag_binary_checkpoint_sources *sources, const struct snag_context_control *control,
    char *error, size_t error_size);

#endif
