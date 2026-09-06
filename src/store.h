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
    char *text;
};

struct snag_queued_turn {
    char queue_id[SNAG_ID_HEX_LEN + 1u];
    uint64_t seq;
    char *text;
    bool read_only;
};

struct snag_store {
    char *root_path;
    int root_fd;
    int sessions_fd;
    int trash_fd;
};

struct snag_session {
    char id[SNAG_ID_HEX_LEN + 1u];
    char prev_sha256[SNAG_SHA256_HEX_LEN + 1u];
    char active_turn_id[SNAG_ID_HEX_LEN + 1u];
    char active_response_id[SNAG_ID_HEX_LEN + 1u];
    char final_item_id[SNAG_ID_HEX_LEN + 1u];
    char final_response_id[SNAG_ID_HEX_LEN + 1u];
    struct snag_process_state processes[SNAG_MAX_PROCESSES];
    size_t process_count;
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
    char active_response_model_input_sha256[SNAG_SHA256_HEX_LEN + 1u];
    char active_response_request_input_sha256[SNAG_SHA256_HEX_LEN + 1u];
    char active_response_request_sha256[SNAG_SHA256_HEX_LEN + 1u];
    char active_response_provider_source_sha256[SNAG_SHA256_HEX_LEN + 1u];
    char usage_anchor_provider[SNAG_CONFIG_PROVIDER_NAME_MAX + 1u];
    char usage_anchor_model[SNAG_MODEL_MAX_BYTES];
    char usage_anchor_effort[SNAG_EFFORT_MAX_BYTES];
    char usage_anchor_compact_id[SNAG_ID_HEX_LEN + 1u];
    char usage_anchor_provider_source_sha256[SNAG_SHA256_HEX_LEN + 1u];
    char usage_anchor_model_input_sha256[SNAG_SHA256_HEX_LEN + 1u];
    char usage_anchor_request_input_sha256[SNAG_SHA256_HEX_LEN + 1u];
    char context_meter_provider[SNAG_CONFIG_PROVIDER_NAME_MAX + 1u];
    char context_meter_model[SNAG_MODEL_MAX_BYTES];
    char context_meter_effort[SNAG_EFFORT_MAX_BYTES];
    char context_meter_compact_id[SNAG_ID_HEX_LEN + 1u];
    char context_meter_provider_source_sha256[SNAG_SHA256_HEX_LEN + 1u];
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
    int64_t log_end;
    uint64_t next_seq;
    uint64_t turn_count;
    uint64_t last_time_ms;
    uint64_t compact_seq;
    uint64_t active_compact_source_seq;
    uint64_t active_response_model_input_bytes;
    uint64_t active_response_request_input_bytes;
    uint64_t active_response_request_input_count;
    uint64_t usage_anchor_model_input_bytes;
    uint64_t usage_anchor_request_input_bytes;
    uint64_t usage_anchor_request_input_count;
    uint64_t usage_anchor_input_tokens;
    uint64_t context_meter_input_tokens;
    uint64_t capacity_ceiling_input_tokens;
    uint64_t active_response_requested_output_tokens;
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
    bool usage_anchor_valid;
    bool context_meter_valid;
    bool capacity_ceiling_valid;
    bool active_response_requested_output_known;
};

const char *snag_goal_status_name(enum snag_goal_status status);
bool snag_goal_unfinished(enum snag_goal_status status);

void snag_store_init(struct snag_store *store);
void snag_store_close(struct snag_store *store);
int snag_store_open(struct snag_store *store, const char *dotdir,
                   char *error, size_t error_size);

void snag_session_init(struct snag_session *session);
void snag_session_close(struct snag_session *session);
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

typedef int (*snag_session_event_fn)(void *opaque, uint64_t seq,
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
