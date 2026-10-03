/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_STORE_BINARY_IMPORT_H
#define SNAJPAGENT_STORE_BINARY_IMPORT_H

#include "store_binary_replay.h"
#include "store_internal.h"

struct snag_binary_import_result {
    struct snag_legacy_recovery legacy;
    struct snag_binary_recovery native;
    /* Owning, valid only after whole-stage success. */
    struct snag_binary_checkpoint_sources sources;
    char source_sha256[SNAG_SHA256_HEX_LEN + 1u];
    unsigned char semantic_digest[32];
};

/* Stage a literal native journal from a stopped, exclusively locked JSONL source.
 * The caller owns an empty, readable/writable, private regular destination and
 * excludes all other access to it. Descriptors remain caller-owned. The source,
 * including its descriptor position, is unchanged; destination position advances.
 * Failure leaves provisional destination bytes for the caller to discard, never
 * a publishable session. Restored must be initialized and state-only; only full
 * success adopts it. Release previous result.sources before reusing result.
 * Result contains diagnostics on failure; sources are owned only on success.
 *
 * Replays the source strictly, batches bounded field-shaped records, replaces
 * derived checkpoints with explicit metadata, then verifies native semantic
 * event digests and core state against legacy replay. Matching current-response
 * snapshots reference complete streams or their first fragments; other snapshots
 * remain literal. Receipt-backed turns reuse matching fields from the current
 * input or queue head, preserving creation/content and latest-edit text ownership.
 * Source identity is rechecked after verification. Reports and retains an
 * incomplete source tail without repair. Adoption cursors are verified in the
 * legacy prefix and replaced by native logical starts; physical cursors are
 * independently reconstructed for comparison. Coordinate-bearing process results
 * still fail until their relocation is implemented.
 * No other reference construction, provider checkpoint, index, fsync, format selection,
 * old-writer exclusion or publication is provided by this test-linked stage. */
int snag_store_import_binary_journal(struct snag_session *source, int destination,
    struct snag_session *restored, struct snag_binary_import_result *result,
    char *error, size_t error_size);

#endif
