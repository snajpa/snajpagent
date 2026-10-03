/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_STORE_BINARY_LEGACY_H
#define SNAJPAGENT_STORE_BINARY_LEGACY_H

#include "store_binary_event.h"

/* Convert legacy event data objects to/from field-shaped native payloads.
 * Currently covers session metadata, controls, goals, timers, downloads, rules,
 * dictation usage, voice observations, compactions, rebases, capacity rejections
 * and turn outcomes (including recovery and cancellation/yield requests).
 * Input controls cover cancellation, admission, deferral, queue state/cancellation.
 * Inline input receipts, steering and IRC reminders retain typed media/paths.
 * Queued/edited inputs retain option presence, while-turn form and voice provenance.
 * Turn starts retain open config fields, mixed legacy instruction metadata and media.
 * Response starts retain legacy/full accounting shapes and typed host snapshots.
 * Stream output, interruptions, failures and corrections retain typed public items.
 * Unresolved public output spans return ENOTSUP, never substituted text.
 * Completed responses retain typed graphs, nullable usage and provider continuation.
 * Tool starts and process-output chunks preserve binding fields and original encoding.
 * Results and process closure preserve typed values, excerpts, options and log coordinates.
 * IRC retains watermark/classification presence, snapshots and typed admitted inputs.
 * Voice sealing/adoption retains identities, counters and original begin coordinates.
 * Voice archives use the enclosed public field profile, including checkpoint views
 * and unassigned source names. Literal ordinary typed snapshots also decode.
 * Native input references return ENOTSUP here; journal-aware resolution is separate.
 * Encode uses the current payload version returned by snag_binary_event_version.
 * All 73 assigned source kinds have adapters; unresolved references return ENOTSUP.
 * Archive observations never fall back to an opaque known record or mutate source state.
 * The importer must separately verify the complete legacy envelope, chain,
 * source-platform admission rules and reducer transitions before publication.
 * Failure leaves output bytes, kind, type and owned data outputs unchanged.
 * Decode validates the record view and returns an owned data object. */
int snag_binary_legacy_encode(struct snag_buf *out, const char *type, const json_t *data,
                              enum snag_binary_kind *kind);
int snag_binary_legacy_decode(const struct snag_binary_record *record, const char **type,
                              json_t **data);

/* Project a resolved whole instruction list, preserving path-only entries and
 * optional original snapshot metadata. New ownership only on success. */
int snag_binary_instructions_legacy(const struct snag_binary_instructions *, bool snapshots,
    json_t **out);

#endif
