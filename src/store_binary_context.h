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

/* Slow independent checkpoint verifier. Strict replay establishes prefix authority;
 * both decoded frame sections must equal the canonical core and provider recipe
 * at that boundary. All source-recheck/atomic adoption rules above apply. Success
 * adopts core state read from the frame and a provider seam materialized from
 * its recipe, using the replay-verified canonical payload pool. Unsupported core
 * fields (currently nonempty voice roots) retain ENOTSUP. This still reads the
 * complete prefix: no efficient loader, suffix cursor or publication is supplied. */
int snag_store_verify_binary_context_checkpoint(struct snag_session *source,
    struct snag_session *restored, const struct snag_binary_anchor *prefix,
    const void *checkpoint, size_t checkpoint_size, struct snag_binary_recovery *recovery,
    struct snag_binary_checkpoint_sources *sources, const struct snag_context_control *control,
    char *error, size_t error_size);

#endif
