/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_STORE_BINARY_REPLAY_H
#define SNAJPAGENT_STORE_BINARY_REPLAY_H

#include "store.h"
#include "store_binary.h"
#include "store_binary_checkpoint.h"

struct snag_binary_recovery {
    struct snag_binary_anchor verified;
    uint64_t batches;
    uint64_t incomplete_tail_bytes;
    uint64_t problem_seq; /* Zero on success. */
    uint64_t problem_start, problem_end; /* Half-open physical byte range. */
};

/* Core-state replay from a stopped native journal, not application resume.
 * Caller holds source's exclusive writer lock throughout replay/publication.
 * Source descriptors/bytes/position are unchanged; restored must be initialized
 * and state-only. Only full success replaces restored and the optional, distinct
 * owning payload-origin output. Origins must be initialized (normally zeroed);
 * success releases their replaced storage, and failure leaves it untouched.
 * Release the final output with checkpoint_sources_free. Origins
 * are provisional until then, tracking fixed text, graph-time directories and
 * unsettled process origins separately from original literal fields.
 * Recovery is diagnostic on failure and identifies the last fully interpreted
 * batch and failing range.
 * Optional fn receives borrowed, resolved data and post-event core state after
 * strict reduction. Unknown optional metadata has no semantic callback. Callback
 * effects are provisional until full replay succeeds; fn failure aborts replay.
 * next_seq/last_time_ms identify the logical callback position. Native offsets
 * and digests remain batch-level and cannot be used as a per-event checkpoint.
 * Incomplete uncommitted tails are reported, never truncated. Semantic errors,
 * unsupported references and allocation/read failures never authorize repair.
 * Uses the literal projection domain and the shared strict reducer. Receipt-backed
 * turn text/content references bind to authoritative receipts: current pending
 * direct/timer input (also inside IRC admission), or latest queued text/original
 * content. Direct/timer instruction references also bind to the pending receipt;
 * literal turn instructions remain independently validated. Queued turns declare
 * instruction originals directly because queue receipts have no such list;
 * current queue identity and text/content receipt bindings still apply. Direct,
 * queue and steering/reminder receipts (including IRC input/steering) may declare earlier
 * canonical fields without borrowing source metadata. Steering retains its own
 * ID/turn and reminder-state/prompt checks in the existing strict reducer.
 * Receipt-backed fields must reuse those exact declarations. Goal turns declare
 * original text/instruction fields directly; current active-goal state supplies
 * authority and the reducer checks the fixed prompt, no content and read-only.
 * Whole literal fields only, no chains; source-role and causal checks precede
 * reduction. Queued voice transcript/request refs preserve the accepting queue's
 * connection/input/response/call IDs and provider/model metadata; the adapter and
 * reducer still validate queue identity and active-turn scope. Interruption,
 * failure and correction snapshot spans bind to the current response-start
 * sequence as well as original scope/item metadata and contiguous fragments.
 * Literal snapshot entries retain their independent reducer validation.
 * Completed-response graphs use the same span bindings while retaining tool
 * calls, graph ordering, usage and continuation placement. Result log references
 * require native half-open sequence ranges ending no later than their owning
 * record. Their original presentation coordinates stay unchanged; native readers
 * must use the logical range, process/stream identity and byte window instead.
 * Voice starts use native sequence references. Other payload refs remain ENOTSUP.
 * Provider-view reconstruction, checkpoints, indexes and cutover remain separate.
 * Linked only by tests while native backend integration is unfinished. */
int snag_store_reconcile_binary(struct snag_session *source, struct snag_session *restored,
    snag_session_event_fn fn, void *opaque, struct snag_binary_recovery *recovery,
    struct snag_binary_checkpoint_sources *, char *error, size_t error_size);

/* Reconstruct exactly through a committed batch anchor, still verifying from
 * byte zero. All anchor members must match the independently replayed prefix.
 * Bytes after prefix->end are UNINSPECTED: success never supplies suffix/tail
 * repair authority, and incomplete_tail_bytes is zero. A partial batch boundary
 * fails instead of being adopted as a shorter prefix. Source locking, provisional
 * callbacks and atomic state/origin ownership follow the full-journal API.
 * The anchor may alias recovery->verified. This is not checkpoint/suffix resume. */
int snag_store_reconcile_binary_prefix(struct snag_session *source, struct snag_session *restored,
    const struct snag_binary_anchor *prefix, snag_session_event_fn fn, void *opaque,
    struct snag_binary_recovery *recovery, struct snag_binary_checkpoint_sources *sources,
    char *error, size_t error_size);

/* Reduce a suffix into DISPOSABLE state/origins already semantically verified
 * at start in this locked immutable journal. This does not authenticate that
 * prefix or permit arbitrary-offset/index authority. State must be unbound and
 * state-only. It may be partially advanced on failure; caller must discard it.
 * Sources retain owned storage on every return. Callbacks are provisional; only
 * successful source recheck supplies tail diagnostics, never truncation itself.
 * recovery.batches counts suffix batches only; start may alias recovery.verified. */
int snag_store_reduce_binary_suffix(struct snag_session *source, struct snag_session *state,
    const struct snag_binary_anchor *start, snag_session_event_fn fn, void *opaque,
    struct snag_binary_recovery *recovery, struct snag_binary_checkpoint_sources *sources,
    char *error, size_t error_size);

#endif
