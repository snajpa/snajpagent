/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_CONTEXT_H
#define SNAJPAGENT_CONTEXT_H

#include "store.h"
#include "instructions.h"

#include <stddef.h>
#include <stdint.h>

#define SNAG_UNSETTLED_COMMANDS_MESSAGE                                                            \
    "Unsettled commands remain; collect their terminal results before a final answer."

#define SNAG_CONTEXT_MAX_REQUEST (32u * 1024u * 1024u)
#define SNAG_CONTEXT_MAX_COMPACT (12u * 1024u * 1024u)

int snag_context_codex_request(json_t *request);
int snag_context_codex_compact_request(json_t *request);
/* Shared native file/history-reading schemas; NULL outside this capability. */
json_t *snag_context_read_tool_schema(const char *name);
struct snag_credential;
int snag_context_continuation_scope(const struct snag_provider_config *provider, const char *model,
    const struct snag_credential *credential, char digest[SNAG_SHA256_HEX_LEN + 1u]);
/* Bind the ordinary local model once when constructing a provider wire request. */
int snag_context_provider_model(
    const struct snag_provider_config *provider, const char *model, json_t *request);
json_t *snag_context_interface_request(const struct snag_session *,
    const struct snag_provider_config *, const char *model, const char *effort, const json_t *input,
    const json_t *tools);
/* Select input-token endpoint fields without mutating generation controls. */
json_t *snag_context_count_request(const json_t *create_request);

struct snag_context_control {
    bool (*cancelled)(void *opaque);
    void *opaque;
    enum snag_history_orientation {
        SNAG_HISTORY_ORIENTATION_NONE = 0,
        SNAG_HISTORY_ORIENTATION_COMPACT,
        SNAG_HISTORY_ORIENTATION_RECOVERY
    } history_orientation;
    /* True only when the current active automatic-goal turn itself crossed a
     * process/capacity recovery boundary. An idle resume followed by a new
     * goal may still need orientation, but must retain ordinary history. */
    bool goal_recovery_rebase;
    const char *irc_replies;
    const json_t *mcp_tools; /* Borrowed, immutable active-turn catalog. */
    /* Permit an idle display projection and keep checkpoint writes disabled.
     * The derived request cache may advance; no turn or request is admitted. */
    bool preview;
};

struct snag_context_projection {
    struct snag_json_document model_input, create_request, count_request;
    json_t *host_context; /* Newly emitted snapshot; NULL when unchanged. */
    char continuation_scope[SNAG_SHA256_HEX_LEN + 1u];
    size_t request_input_bytes;
    size_t request_input_count;
    size_t request_controller_count;
    uint64_t input_tokens_bound;
    uint64_t irc_seq;
    uint64_t irc_boundary, irc_count;
    char request_input_sha256[SNAG_SHA256_HEX_LEN + 1u];
    uint64_t source_seq; /* Selected complete group for compaction. */
};

/* Staged provider capture shares the live cache's event seam and renderer.
 * Feed every resolved semantic event and its post-reduction state, in order.
 * Effects remain provisional: bind only after complete replay/source verification
 * against the matching core state. The state-only target must have no consumer or
 * embedded checkpoint. Binding transfers ownership and clears *capture on success;
 * failure changes neither target nor ownership. Event capture/binding do no I/O.
 * The borrowed control lives until bind/free. This is an in-memory reconstruction
 * interface, not a serialized provider checkpoint or a journal format selector. */
struct snag_context_capture;
struct snag_context_capture *snag_context_capture_new(const struct snag_context_control *control);
int snag_context_capture_event(void *opaque, const struct snag_session *state, uint64_t seq,
    const char *type, const json_t *data, char *error, size_t error_size);
int snag_context_capture_bind(struct snag_context_capture **capture, struct snag_session *session,
    char *error, size_t error_size);
void snag_context_capture_free(struct snag_context_capture *capture);
/* Populate only historical IRC sources referenced by the captured seam/current
 * input. wanted maps canonical sequence strings to their first retained admission
 * sequence; prompt may name IRC stream tuples, which are not canonical ordinals.
 * The ordered read-only walker establishes the complete requested closure under
 * immutable source ownership; it emits IRC events and checkpoint metadata
 * (type session_checkpoint).
 * Those metadata rows are lookup-only, never reducer/provider event admission.
 * Resolve before bind. Failure preserves the capture's previous source table. */
typedef int (*snag_context_source_walk_fn)(void *source, const json_t *wanted, const char *prompt,
    snag_session_event_fn fn, void *opaque, char *error, size_t error_size);
int snag_context_capture_sources(struct snag_context_capture *capture,
    const struct snag_session *state, snag_context_source_walk_fn walk, void *source, char *error,
    size_t error_size);

/* Temporarily transfer a disposable state-only session's capture back to its
 * caller for further verified replay. Requires an empty output; failure is
 * atomic. The supplied control is borrowed until free or rebind. Core reduction
 * then has no live callbacks; explicit capture_event reports cancellation/OOM. */
int snag_context_capture_take(struct snag_session *session,
    const struct snag_context_control *control, struct snag_context_capture **capture, char *error,
    size_t error_size);
/* Borrow the retained event seam and its historical IRC closure from a bound
 * capture. They remain owned by the session and may change on the next commit.
 * The materialized request cache is not exposed. Canonical event data remains
 * internal, including any provider-private fields retained by those events. */
int snag_context_capture_seam(
    const struct snag_session *session, const json_t **recent, const json_t **history);
/* Seed an empty rebuild capture from validated, matching checkpoint entries.
 * Shallow copies own the arrays; immutable entries/payloads are shared. Pending
 * initially covers the entire seam because the first projection rebuilds it. */
int snag_context_capture_seed(
    struct snag_context_capture *capture, const json_t *recent, const json_t *history);

void snag_context_projection_free(struct snag_context_projection *projection);
/* Start the live view after creating a new session; resumed sessions derive it
 * from the journal once on their first projection. */
void snag_context_start_new(struct snag_session *session);
int snag_context_build(struct snag_session *session, const char *model, const char *effort,
    unsigned int cycle, const json_t *steering, uint64_t max_output_tokens, bool max_output_known,
    const struct snag_config *config, const char *continuation_scope,
    const struct snag_instruction_set *instructions, const char *operator_visibility,
    struct snag_context_projection *projection, char *error, size_t error_size,
    const struct snag_context_control *control);
/* Model-work identity: stable across resume and independent of turn, cycle or
 * timing. The interface request derives its own namespace from this identity. */
#define SNAG_CACHE_KEY_LEN 32u
void snag_context_cache_key(const struct snag_session *session, const char *provider,
    const char *model, char out[SNAG_CACHE_KEY_LEN + 1u]);
int snag_context_compact_request_build(struct snag_session *session, const char *model,
    const char *effort, bool active_prefix, uint64_t source_budget, bool allow_oversized_first,
    const char *continuation_scope, struct snag_context_projection *projection, char *error,
    size_t error_size, const struct snag_context_control *control);
int snag_context_compact_output_count_request_build(const json_t *output, const char *model,
    struct snag_json_document *count_request, char *error, size_t error_size);
/* Smallest compaction source considered meaningful: a budget below this
 * cannot carry even one small history group, so the shrink loop floors
 * here and the builder cuts the group instead of collapsing further. */
#define SNAG_CONTEXT_COMPACT_FLOOR (64u * 1024u)

/* Seam overlap: a compaction source re-reads this many already-covered events so
 * a chunk boundary cannot lose the joint between summaries. */
/* Seam overlap: 0 disables the re-read. Otherwise that many already-covered
 * events are re-read so a chunk boundary cannot lose the joint between
 * summaries; the pruner in context.c keeps the request valid by dropping any
 * call or output whose counterpart fell outside the re-read.
 *
 * Back at 0 on purpose: with 16 the tmux terminal matrix's count-overflow mode
 * stopped producing compaction_completed ("AssertionError: count-overflow"),
 * so the overlap is not yet safe to carry. Next step is to check whether the
 * covered boundary is advanced to the overlap floor instead of the covered end;
 * the boundary must advance past everything the chunk actually covered while
 * only the request source re-reads behind it. */
#define SNAG_CONTEXT_COMPACT_OVERLAP_EVENTS 16u

int snag_context_compact_output_valid(const json_t *output,
    char output_hash[SNAG_SHA256_HEX_LEN + 1u], size_t *output_bytes, char *error,
    size_t error_size);
/* Consumes a compact result and retains its validated canonical measurement. */
int snag_context_compact_output_set(
    struct snag_json_document *document, json_t *value, char *error, size_t error_size);
/* Builds the one-shot condense request for a merged summary that has grown past
 * the window fraction: its input is the merged text plus the dedupe
 * instruction and nothing else, so no event re-enters the source. */
int snag_context_compact_reduce_request_build(struct snag_session *session,
    const struct snag_provider_config *provider, const char *model, const char *effort,
    const json_t *output, const char *instruction, struct snag_json_document *create_request,
    char *error, size_t error_size);

#endif
