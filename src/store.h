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
#define SNAG_MAX_STEERING_PER_TURN 32u
#define SNAG_MAX_QUEUED_TEXT (256u * 1024u)
#define SNAG_MAX_PENDING_TURNS 128u
#define SNAG_MAX_PENDING_QUEUE_TEXT (16u * 1024u * 1024u)
#define SNAG_MAX_IRC_SNAPSHOT (8u * 1024u * 1024u)
#define SNAG_IRC_REPLY_REMINDER_TEXT \
    "Use irc_send to reply to the local operator in the IRC room before " \
    "ending this turn."
#define SNAG_MAX_GOAL_PROMPT (1024u * 1024u)
#define SNAG_MAX_GOAL_BLOCKER (64u * 1024u)
#define SNAG_GOAL_CONTINUATION_TEXT "Continue the active goal from its durable state."

enum snag_goal_status {
    SNAG_GOAL_NONE,
    SNAG_GOAL_ACTIVE,
    SNAG_GOAL_PAUSED,
    SNAG_GOAL_BLOCKED,
    SNAG_GOAL_COMPLETED,
    SNAG_GOAL_CANCELLED
};

enum snag_response_terminal {
    SNAG_RESPONSE_TERMINAL_NONE,
    SNAG_RESPONSE_TERMINAL_STEERED,
    SNAG_RESPONSE_TERMINAL_INTERRUPTED,
    SNAG_RESPONSE_TERMINAL_FAILED
};

struct snag_pending_call {
    char call_id[SNAG_ID_HEX_LEN + 1u];
    char action_sha256[SNAG_SHA256_HEX_LEN + 1u];
    char tool_name[16];
    char process_handle[SNAG_ID_HEX_LEN + 1u];
    char command[257], workdir[257];
    bool started;
    bool finished;
};

struct snag_pending_steering {
    char steering_id[SNAG_ID_HEX_LEN + 1u];
    uint64_t seq;
    uint64_t received_ms, first_context_ms;
    char *text;
};

struct snag_queued_turn {
    char queue_id[SNAG_ID_HEX_LEN + 1u];
    uint64_t seq;
    uint64_t received_ms, first_context_ms;
    char *text;
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

struct snag_session {
    char id[SNAG_ID_HEX_LEN + 1u];
    char prev_sha256[SNAG_SHA256_HEX_LEN + 1u];
    char active_turn_id[SNAG_ID_HEX_LEN + 1u];
    char active_response_id[SNAG_ID_HEX_LEN + 1u];
    char final_item_id[SNAG_ID_HEX_LEN + 1u];
    char final_response_id[SNAG_ID_HEX_LEN + 1u];
    struct snag_input_observation active_accounting, usage_anchor, context_meter;
    struct snag_process_state processes[SNAG_MAX_PROCESSES];
    size_t process_count;
    uint64_t irc_received_seq, irc_consumed_seq, response_irc_seq;
    uint32_t max_parallel_commands;
    bool parallel_tool_calls;
    char compact_id[SNAG_ID_HEX_LEN + 1u];
    char active_compact_id[SNAG_ID_HEX_LEN + 1u];
    char active_compact_source_sha256[SNAG_SHA256_HEX_LEN + 1u];
    char default_provider[SNAG_CONFIG_PROVIDER_NAME_MAX + 1u];
    char goal_id[SNAG_ID_HEX_LEN + 1u];
    char default_model[SNAG_MODEL_MAX_BYTES];
    char active_turn_model[SNAG_MODEL_MAX_BYTES];
    char active_turn_provider[SNAG_CONFIG_PROVIDER_NAME_MAX + 1u];
    char active_turn_effort[SNAG_EFFORT_MAX_BYTES];
    char capacity_ceiling_provider[SNAG_CONFIG_PROVIDER_NAME_MAX + 1u];
    char capacity_ceiling_model[SNAG_MODEL_MAX_BYTES];
    char capacity_ceiling_source_sha256[SNAG_SHA256_HEX_LEN + 1u];
    char default_effort[SNAG_EFFORT_MAX_BYTES];
    char *workspace;
    char trash_name[SNAG_ID_HEX_LEN + 1u + SNAG_ID_HEX_LEN + 1u];
    char *dir_path;
    char *first_user;
    char *last_user;
    char *last_assistant;
    char *goal_prompt;
    char *goal_blocker;
    json_t *compact_output;
    int dir_fd;
    int log_fd;
    int lock_fd;
    struct snag_buf *pending_log; /* New sessions stay in memory until input. */
    int64_t log_end;
    uint64_t next_seq;
    uint64_t turn_count;
    uint64_t last_time_ms;
    uint64_t compact_seq;
    uint64_t active_compact_source_seq;
    uint64_t capacity_ceiling_input_tokens;
    uint64_t input_received_ms, input_first_context_ms;
    uint64_t recovery_count;
    uint64_t goal_revision;
    uint64_t goal_turn_count;
    size_t pending_steering_bytes;
    size_t pending_queue_bytes;
    unsigned int active_cycle;
    enum snag_graph_outcome response_outcome;
    struct snag_pending_call pending_calls[SNAG_MAX_CALLS_PER_RESPONSE];
    struct snag_pending_steering pending_steering[SNAG_MAX_STEERING_PER_TURN];
    struct snag_queued_turn pending_queue[SNAG_MAX_PENDING_TURNS];
    size_t pending_call_count;
    size_t pending_steering_count;
    size_t pending_queue_count;
    bool append_rollback_pending;
    int64_t append_rollback_end;
    bool active_turn;
    bool last_turn_failed;
    bool retry_read_only;
    bool active_read_only;
    bool active_queued;
    bool archived;
    bool delete_requested;
    bool response_open;
    bool response_complete;
    bool irc_reply_reminded;
    bool output_correction_used;
    unsigned int cyber_clarifications;
    enum snag_response_terminal response_terminal;
    enum snag_goal_status goal_status;
    bool goal_locked;
    bool capacity_ceiling_valid;
};

const char *snag_goal_status_name(enum snag_goal_status status);
bool snag_goal_unfinished(enum snag_goal_status status);

void snag_store_init(struct snag_store *store);
void snag_store_close(struct snag_store *store);
int snag_store_open(struct snag_store *store, const char *dotdir,
                   char *error, size_t error_size);

void snag_session_init(struct snag_session *session);
void snag_session_close(struct snag_session *session);
int snag_session_prepare(struct snag_session *session, const char *workspace,
                         const char *provider, const char *model, const char *effort,
                         char *error, size_t error_size);
int snag_session_persist(struct snag_store *store, struct snag_session *session,
                         char *error, size_t error_size);
int snag_session_create(struct snag_store *store, struct snag_session *session,
                       const char *workspace, const char *provider,
                       const char *model,
                       const char *effort, char *error, size_t error_size);
int snag_session_open(struct snag_store *store, struct snag_session *session,
                     const char *prefix, char *error, size_t error_size);
int snag_session_open_last(struct snag_store *store, struct snag_session *session,
                          const char *workspace, bool all,
                          char *error, size_t error_size);
typedef int (*snag_store_emit_fn)(void *, const char *, size_t);
int snag_store_list(struct snag_store *store, const char *workspace, bool all,
                    bool include_archived, snag_store_emit_fn emit, void *opaque,
                    char *error, size_t error_size);
int snag_session_archive(struct snag_session *session, uint64_t *written_seq,
                        char *error, size_t error_size);
int snag_session_unarchive(struct snag_session *session, uint64_t *written_seq,
                          char *error, size_t error_size);
int snag_session_delete(struct snag_store *store, struct snag_session *session,
                       const char *confirmed_prefix, uint64_t *written_seq,
                       char *error, size_t error_size);
int snag_session_complete_delete(struct snag_store *store,
                                struct snag_session *session,
                                char *error, size_t error_size);

/* Full replay supplies validated post-event state; cursor scans supply NULL. */
typedef int (*snag_session_event_fn)(void *opaque, const struct snag_session *state,
                                   uint64_t seq,
                                    const char *type, const json_t *data,
                                    char *error, size_t error_size);
int snag_session_each_event(struct snag_session *session,
                           snag_session_event_fn fn, void *opaque,
                           char *error, size_t error_size);
struct snag_process_state *snag_session_process(struct snag_session *, const char *handle);
int snag_process_output_decode(const json_t *data, struct snag_buf *bytes);
int snag_session_each_event_since(struct snag_session *, const struct snag_process_state *,
                                  snag_session_event_fn, void *, char *, size_t);

int snag_session_commit(struct snag_session *session, const char *type,
                       json_t *data, uint64_t *written_seq,
                       char *error, size_t error_size);

#endif
