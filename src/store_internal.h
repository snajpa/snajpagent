/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_STORE_INTERNAL_H
#define SNAJPAGENT_STORE_INTERNAL_H

#include "store.h"
#include "store_binary_io.h"
#include "store_binary_index.h"
#include "store_binary_producer.h"

/* Attach the live commit path to an independently verified native state and
 * exact EOF boundary under its exclusive lock. The caller proves identity,
 * frontier membership and complete working provenance. Clone producer metadata
 * on success; caller retains all input owners. No file conversion or creation. */
int snag_session_bind_binary(struct snag_session *, const struct snag_binary_identity *,
    const struct snag_binary_anchor *, const struct snag_binary_index_tree *,
    const struct snag_binary_producer *, const struct snag_binary_io_ops *, char *, size_t);

#include <stdbool.h>
#include <stddef.h>

#define SNAG_TRASH_SUFFIX_HEX_LEN SNAG_ID_HEX_LEN
#define SNAG_TRASH_NAME_LEN (SNAG_ID_HEX_LEN + 1u + SNAG_TRASH_SUFFIX_HEX_LEN)

enum snag_tail_policy {
    SNAG_TAIL_REJECT, SNAG_TAIL_TRUNCATE, SNAG_TAIL_IGNORE };

struct snag_legacy_recovery {
    uint64_t verified_records, discarded_checkpoints, repaired_pointers;
    int64_t verified_end;
    uint64_t incomplete_tail_bytes;
    uint64_t problem_seq; /* Zero on success; byte range is [start, end). */
    int64_t problem_start, problem_end;
};

/* Resolve one line boundary inside an independently verified, immutable legacy
 * prefix. No prefix scan or file-position change; failure preserves out. */
int snag_store_legacy_cursor_at(struct snag_session *session, int64_t offset,
    struct snag_journal_cursor *out, char *error, size_t error_size);

/* Explicit offline import only. Caller holds the original source writer lock
 * throughout replay/publication. No source/cwd writes or descriptor seeks.
 * Replays canonical records from byte zero, ignoring derived checkpoint bodies
 * but validating their envelopes and chain. Known legacy defaults still apply;
 * unresolved transitions fail instead of silently contributing no state.
 * Initialized state-only restored is replaced only on success. Callback state
 * is borrowed/provisional until success; checkpoint context must be rebuilt.
 * Recovery reports the verified prefix and exact failing record range, or the
 * unresolved suffix when a whole record cannot be read. It is not a converter. */
int snag_store_reconcile_legacy(struct snag_session *source, struct snag_session *restored,
    snag_session_event_fn fn, void *opaque, struct snag_legacy_recovery *recovery,
    char *error, size_t error_size);

/* Shared strict state transition for verified history. The caller supplies a
 * provisional state/clock and validated record identity and discards it on error.
 * No live filesystem checks or legacy invalid-transition downgrade. */
int snag_store_reduce_event(struct snag_session *state, const char *type, const json_t *data,
    uint64_t sequence, char *error, size_t error_size);

/* Derive pending-call metadata from a validated graph item and its graph-time
 * directory. Failure preserves out; lifecycle flags start cleared. */
int snag_pending_call_from_item(const struct snag_response_item *, const char *cwd,
    struct snag_pending_call *out);

json_t *snag_checkpoint_state_encode(const struct snag_session *session);
int snag_checkpoint_state_decode(const json_t *data, struct snag_session *state);

bool snag_store_trash_id(const char *name, char id[SNAG_ID_HEX_LEN + 1u]);
int snag_store_verify_private_fd(int fd, bool directory, const char *name, char *error, size_t error_size);
int snag_store_open_session_files(struct snag_session *session, bool create, char *error, size_t error_size);
int snag_store_remove_upload_staging(int session_fd, char *error, size_t error_size);
int snag_store_scan_log(struct snag_session *session, enum snag_tail_policy tail_policy,
                       char *error, size_t error_size);
int snag_store_complete_trash_delete(struct snag_store *store, const char *trash_name,
                                    char *error, size_t error_size);

#endif
