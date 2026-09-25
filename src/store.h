/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_STORE_H
#define SNAJPAGENT_STORE_H

#include "config.h"
#include "json.h"
#include "turn.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#define SNAG_MODEL_MAX_BYTES 256u
#define SNAG_EFFORT_MAX_BYTES 64u
#define SNAG_MAX_STEERING_TEXT (256u * 1024u)
#define SNAG_MAX_PENDING_STEERING_BYTES (8u * 1024u * 1024u)
#define SNAG_MAX_QUEUED_TEXT (256u * 1024u)
#define SNAG_MAX_PENDING_QUEUE_TEXT (16u * 1024u * 1024u)
#define SNAG_MAX_IRC_SNAPSHOT (8u * 1024u * 1024u)
#define SNAG_MAX_TIMER_TEXT (256u * 1024u)
#define SNAG_IRC_REPLY_REMINDER_TEXT \
    "Use irc_send to reply to the local operator in the IRC room before " \
    "ending this turn."
#define SNAG_MAX_GOAL_PROMPT (1024u * 1024u)
#define SNAG_BANNER_MAX (4u * 1024u)
#define SNAG_MAX_GOAL_BLOCKER (64u * 1024u)
#define SNAG_GOAL_CONTINUATION_TEXT "Continue the active goal from its durable state."
#define SNAG_HOST_CONTEXT_BEGIN \
    "[snajpagent host continuation — not a new user message]\n" \
    "Host state snapshot: the following facts replace earlier snapshots."
#define SNAG_HOST_CONTEXT_END \
    "[snajpagent host continuation — not a new user message]\n" \
    "End host state snapshot."

enum snag_policy_stop {
    SNAG_POLICY_STOP_NONE, SNAG_POLICY_STOP_PROVIDER, SNAG_POLICY_STOP_REFUSAL };

enum snag_goal_status {
    SNAG_GOAL_NONE, SNAG_GOAL_ACTIVE, SNAG_GOAL_PAUSED, SNAG_GOAL_BLOCKED,
    SNAG_GOAL_COMPLETED, SNAG_GOAL_CANCELLED };

enum snag_response_terminal {
    SNAG_RESPONSE_TERMINAL_NONE, SNAG_RESPONSE_TERMINAL_STEERED,
    SNAG_RESPONSE_TERMINAL_INTERRUPTED, SNAG_RESPONSE_TERMINAL_FAILED };

enum snag_session_control {
    SNAG_CONTROL_CONFIG = 1u, SNAG_CONTROL_CACHE = 2u, SNAG_CONTROL_COMPACT = 4u, SNAG_CONTROL_ARCHIVE = 8u,
    SNAG_CONTROL_DELETE = 16u, SNAG_CONTROL_RETRY = 32u };

struct snag_pending_call {
    char call_id[SNAG_ID_HEX_LEN + 1u];
    char action_sha256[SNAG_SHA256_HEX_LEN + 1u];
    char tool_name[64];
    char process_handle[SNAG_ID_HEX_LEN + 1u];
    char command[257], workdir[257];
    bool started;
    bool finished;
};

struct snag_pending_steering {
    char steering_id[SNAG_ID_HEX_LEN + 1u];
    uint64_t seq;
    uint64_t received_ms, first_context_ms;
    const char *text;
    json_t *content;
};

struct snag_queued_turn {
    char queue_id[SNAG_ID_HEX_LEN + 1u];
    uint64_t seq;
    uint64_t received_ms, first_context_ms;
    const char *text;
    json_t *content;
    bool read_only;
};

struct snag_store {
    char *root_path;
    int root_fd;
    int sessions_fd;
    int trash_fd;
};

/* Independent request-time and completed-usage observations share a value
 * representation, not a lifetime. A valid observation may contain zero tokens. */
struct snag_input_observation {
    char provider[SNAG_CONFIG_PROVIDER_NAME_MAX + 1u];
    char model[SNAG_MODEL_MAX_BYTES];
    char effort[SNAG_EFFORT_MAX_BYTES];
    char compact_id[SNAG_ID_HEX_LEN + 1u];
    char provider_source_sha256[SNAG_SHA256_HEX_LEN + 1u];
    char model_input_sha256[SNAG_SHA256_HEX_LEN + 1u];
    char request_input_sha256[SNAG_SHA256_HEX_LEN + 1u];
    char request_sha256[SNAG_SHA256_HEX_LEN + 1u];
    uint64_t model_input_bytes;
    uint64_t request_input_bytes;
    uint64_t request_input_count;
    uint64_t input_tokens;
    uint64_t requested_output_tokens;
    bool valid;
};

bool snag_input_observation_matches(const struct snag_input_observation *,
    const char *provider, const char *model, const char *effort,
    const char *source_sha256, const char *compact_id);

/* Per-session usage totals, accumulated where a completed response is applied, so a
 * resumed session derives them from its journal rather than from memory. */
struct snag_usage_totals {
    uint64_t responses;
    uint64_t input_tokens;
    uint64_t cached_input_tokens;
    uint64_t uncached_input_tokens;
    uint64_t output_tokens;
    uint64_t reasoning_tokens;
    uint64_t total_tokens;
    bool cached_seen;
};

struct snag_session {
    char id[SNAG_ID_HEX_LEN + 1u];
    char prev_sha256[SNAG_SHA256_HEX_LEN + 1u];
    char active_turn_id[SNAG_ID_HEX_LEN + 1u];
    char active_response_id[SNAG_ID_HEX_LEN + 1u];
    char final_item_id[SNAG_ID_HEX_LEN + 1u];
    char final_response_id[SNAG_ID_HEX_LEN + 1u];
    struct snag_input_observation active_accounting, usage_anchor, context_meter, capacity_rejection;
    struct snag_usage_totals usage_totals;
    struct snag_process_state *processes;
    size_t process_count, process_capacity;
    uint64_t irc_received_seq, irc_consumed_seq, response_irc_seq;
    uint32_t max_parallel_commands;
    uint32_t default_yield_ms, max_wait_ms;
    uint32_t default_timeout_ms, max_timeout_ms;
    uint32_t tool_output_bytes, output_cache_bytes;
    bool parallel_tool_calls;
    char compact_id[SNAG_ID_HEX_LEN + 1u];
    char active_compact_id[SNAG_ID_HEX_LEN + 1u];
    char active_compact_source_sha256[SNAG_SHA256_HEX_LEN + 1u];
    char active_compact_scope[SNAG_SHA256_HEX_LEN + 1u];
    char default_provider[SNAG_CONFIG_PROVIDER_NAME_MAX + 1u];
    char goal_id[SNAG_ID_HEX_LEN + 1u];
    char goal_parent_id[SNAG_ID_HEX_LEN + 1u];
    char timer_id[SNAG_ID_HEX_LEN + 1u];
    char default_model[SNAG_MODEL_MAX_BYTES];
    char active_turn_model[SNAG_MODEL_MAX_BYTES];
    char active_turn_provider[SNAG_CONFIG_PROVIDER_NAME_MAX + 1u];
    char active_turn_effort[SNAG_EFFORT_MAX_BYTES];
    char capacity_ceiling_provider[SNAG_CONFIG_PROVIDER_NAME_MAX + 1u];
    char capacity_ceiling_model[SNAG_MODEL_MAX_BYTES];
    char capacity_ceiling_source_sha256[SNAG_SHA256_HEX_LEN + 1u];
    char default_effort[SNAG_EFFORT_MAX_BYTES];
    /* Context-window selection for the selected model; durable through
     * context_selection_changed and restored on resume. */
    enum snag_context_mode context_mode;
    uint64_t context_tokens;
    /* Empty means the process configuration's validated shell. A model-selected
     * absolute shell path is replayed from command_shell_changed. */
    char command_shell[SNAG_CONFIG_PATH_MAX + 1u];
    const char *cwd;
    char trash_name[SNAG_ID_HEX_LEN + 1u + SNAG_ID_HEX_LEN + 1u];
    char *dir_path;
    const char *first_user;
    const char *last_user;
    const char *active_prompt;
    const char *goal_prompt;
    const char *goal_blocker;
    const char *timer_text;
    const char *banner_text;
    const char *steering_override;
    /* Private immutable string owners; text fields above and in pending inputs borrow. */
    json_t *strings;
    json_t *compact_output;
    char compact_scope[SNAG_SHA256_HEX_LEN + 1u];
    json_t *pending_input; /* Accepted direct input awaiting turn preparation. */
    json_t *active_instructions; /* Original path metadata for same-turn recovery. */
    json_t *response_public; /* Reconstructed public prefix of the current response. */
    size_t response_public_bytes;
    int dir_fd;
    int log_fd;
    int lock_fd;
    struct snag_buf *pending_log; /* New sessions stay in memory until input. */
    int64_t log_end;
    uint64_t next_seq;
    /* An optional in-process consumer of newly committed events. The durable
     * state remains authoritative; a failed consumer must invalidate itself,
     * not turn a successfully synced commit into a failed write. */
    void (*on_commit)(void *, const struct snag_session *, uint64_t, const char *, const json_t *);
    void (*on_commit_free)(void *);
    void *on_commit_opaque;
    uint64_t turn_count;
    uint64_t last_time_ms;
    uint64_t compact_seq;
    /* Events at or below this durable recovery boundary are available through
     * read_session_history but never re-enter automatic provider context. */
    uint64_t context_rebase_seq;
    char context_rebase_turn_id[SNAG_ID_HEX_LEN + 1u];
    bool context_rebase_has_new_results;
    /* Set while replaying a format-2 journal (the workspace-era schema): such
     * records use the small compatibility rules in the store and carry no
     * workspace semantics. New journals never set it. */
    bool legacy_journal;
    uint64_t active_compact_source_seq;
    uint64_t capacity_ceiling_input_tokens;
    uint64_t input_received_ms, input_first_context_ms;
    uint64_t recovery_count;
    uint64_t goal_revision;
    uint64_t goal_turn_count;
    uint64_t timer_due_ms;
    size_t pending_steering_bytes;
    size_t pending_queue_bytes;
    unsigned int active_cycle;
    uint64_t turn_retry_attempts;
    uint32_t turn_retry_limit;
    enum snag_graph_outcome response_outcome;
    struct snag_pending_call *pending_calls;
    struct snag_pending_steering *pending_steering;
    struct snag_queued_turn *pending_queue;
    size_t pending_call_count, pending_call_capacity;
    size_t pending_steering_count, pending_steering_capacity;
    size_t pending_queue_count, pending_queue_capacity;
    uint64_t write_failures; /* Process-local, includes every event writer. */
    bool append_rollback_pending;
    int64_t append_rollback_end;
    unsigned int pending_controls, started_controls;
    bool compact_control_image_boundary;
    uint64_t compact_control_source_seq;
    uint64_t control_seq[6];
    bool queue_armed;
    bool active_turn;
    bool last_turn_failed;
    bool retry_read_only;
    bool active_read_only;
    bool active_queued;
    bool active_goal;
    bool cancel_requested;
    enum snag_policy_stop policy_stopped;
    bool response_handoff;
    bool archived;
    bool delete_requested;
    bool response_open;
    bool response_complete;
    bool irc_reply_reminded;
    bool output_correction_used;
    /* A model-requested per-turn input boundary. Pending steers remain
     * journaled but do not acquire first_context_ms until a later turn. */
    bool steering_deferred;
    enum snag_response_terminal response_terminal;
    enum snag_goal_status goal_status;
    bool goal_locked;
    bool capacity_ceiling_valid;
};

const char *snag_goal_status_name(enum snag_goal_status status);
bool snag_goal_unfinished(enum snag_goal_status status);

void snag_store_init(struct snag_store *store);
void snag_store_close(struct snag_store *store);
int snag_store_open(struct snag_store *store, const char *dotdir, char *error, size_t error_size);

void snag_session_init(struct snag_session *session);
void snag_session_close(struct snag_session *session);
/* Resolve a cwd path and require an existing UTF-8 directory. label
 * names the cwd in diagnostics; NULL uses the bare "cwd" wording. */
char *snag_cwd_resolve(const char *cwd, const char *label, char *error, size_t error_size);
int snag_session_prepare(struct snag_session *session, const char *cwd,
                         const char *provider, const char *model, const char *effort,
                         char *error, size_t error_size);
int snag_session_persist(struct snag_store *store, struct snag_session *session,
                         char *error, size_t error_size);
int snag_session_create(struct snag_store *store, struct snag_session *session,
                       const char *cwd, const char *provider, const char *model,
                       const char *effort, char *error, size_t error_size);
int snag_session_open(struct snag_store *store, struct snag_session *session,
                     const char *prefix, char *error, size_t error_size);
int snag_session_open_last(struct snag_store *store, struct snag_session *session,
                          char *error, size_t error_size);
typedef int (*snag_store_emit_fn)(void *, const char *, size_t);
int snag_store_list(struct snag_store *store,
                    bool include_archived, snag_store_emit_fn emit, void *opaque,
                    char *error, size_t error_size);
int snag_session_archive(struct snag_session *session, uint64_t *written_seq, char *error, size_t error_size);
int snag_session_unarchive(struct snag_session *session, uint64_t *written_seq,
                          char *error, size_t error_size);
int snag_session_delete(struct snag_store *store, struct snag_session *session,
                       const char *confirmed_prefix, uint64_t *written_seq, char *error, size_t error_size);
int snag_session_complete_delete(struct snag_store *store, struct snag_session *session,
                                char *error, size_t error_size);

/* Called by the sole session owner after finalized voice input. Acceptance is
 * one queued-input event; repeated connection/input IDs never enqueue twice,
 * including after queue consumption, deletion, or session replay. */
int snag_session_voice_queue(struct snag_session *,const json_t *,char id[SNAG_ID_HEX_LEN+1u],
                             bool *duplicate,char *,size_t);
/* Read the original queue/turn state, including after disconnect or replay.
 * Caller owns *result: status, turn_id (empty while queued), and text. */
int snag_session_voice_status(struct snag_session *,const char *queue_id,json_t **result,char *,size_t);
/* Bounded textual seed for an explicitly opened connection; never actions or
 * orphaned function outputs. Includes recent captions and latest handoff state. */
int snag_session_voice_context(struct snag_session *,json_t **result,char *,size_t);

/* Full replay supplies validated post-event state; cursor scans supply NULL. */
typedef int (*snag_session_event_fn)(void *opaque, const struct snag_session *state, uint64_t seq,
                                    const char *type, const json_t *data, char *error, size_t error_size);
int snag_session_each_event(struct snag_session *session, snag_session_event_fn fn, void *opaque,
                           char *error, size_t error_size);
struct snag_process_state *snag_session_process(struct snag_session *, const char *handle);
bool snag_session_pending_steering_unadmitted(const struct snag_session *);
int snag_process_output_decode(const json_t *data, struct snag_buf *bytes);
int snag_session_each_event_since(struct snag_session *, const struct snag_process_state *,
                                  snag_session_event_fn, void *, char *, size_t);

int snag_session_commit(struct snag_session *session, const char *type, json_t *data, uint64_t *written_seq,
                       char *error, size_t error_size);

int snag_session_media(struct snag_session *session, const char *path, const char *mime,
                       int (*pump)(void *, unsigned int), void *opaque,
                       json_t **asset, char **retained_path, char *error, size_t error_size);

/* Context-selection mode names are durable session-log values; the session
 * store owns their parsing so replay and the application agree. */
const char *snag_context_mode_name(enum snag_context_mode mode);
/* 0 for a known mode name, -1 otherwise. */
int snag_context_mode_parse(const char *name, enum snag_context_mode *mode);
bool snag_context_choice_valid(enum snag_context_mode mode, uint64_t tokens);

#endif
