/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_APP_INTERNAL_H
#define SNAJPAGENT_APP_INTERNAL_H

#include "app.h"
#include "base.h"
#include "cli.h"
#include "config.h"
#include "credential.h"
#include "auth.h"
#include "instructions.h"
#include "irc.h"
#include "model_cache.h"
#include "render.h"
#include "responses.h"
#include "store.h"
#include "term.h"
#include "ui.h"
#include "turn.h"

#include "snj_jansson.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct partial_public_item {
    size_t graph_index;
    enum snj_item_kind kind;
    enum snj_item_phase phase;
    char local_item_id[SNJ_ID_HEX_LEN + 1u];
    char provider_item_id[SNJ_MAX_PROVIDER_ID + 1u];
    struct snj_buf text;
};

struct app_state {
    struct snj_store store;
    struct snj_session session;
    struct snj_ui ui;
    struct snj_irc *irc;
    struct snj_buf irc_urgent;
    struct snj_buf irc_background;
    struct snj_instruction_set turn_instructions;
    struct snj_model_cache model_cache;
    struct snj_model_capacity turn_capacity;
    const struct snj_cli *cli;
    struct snj_config *config;
    const char *config_path;
    bool config_allow_create;
    const char *turn_model;
    const char *turn_effort;
    const struct snj_provider_config *turn_provider;
    const struct snj_provider_config *staged_provider;
    const char *staged_model;
    const char *staged_effort;
    const struct snj_response_graph *stream_graph;
    struct partial_public_item partial[SNJ_MAX_RESPONSE_ITEMS];
    size_t partial_count;
    size_t partial_bytes;
    size_t stream_item_index;
    enum snj_item_kind stream_kind;
    enum snj_item_phase stream_phase;
    bool stream_item_active;
    bool stream_item_seen;
    bool stream_item_hidden;
    bool stream_failed;
    int stream_errno;
    char stream_error[256];
    bool steering_requested;
    bool interrupt_requested;
    bool queue_armed;
    bool goal_armed;
    bool last_turn_refused;
    bool queue_edit_was_armed;
    bool input_closed;
    bool execute;
    bool networked;
    bool tool_active;
    bool irc_urgent_local_operator;
    bool irc_turn_local_operator;
    bool irc_turn_replied;
    int shutdown_signal;
    uint64_t irc_background_since_ms;
    char queue_edit_id[SNJ_ID_HEX_LEN + 1u];
    size_t queue_edit_number;
    char capacity_cache_error[256];
};

enum {
    /* Provider pump results already use 1 and 2. */
    SNJ_APP_COUNT_SKIPPED = 3
};

int snj_app_commit_event(struct app_state *app, const char *type, json_t *data,
                         char *error, size_t error_size);
int snj_app_capacity_resolve(struct app_state *app,
                             const struct snj_provider_config *provider,
                             const char *model,
                             struct snj_model_capacity *capacity,
                             char *error, size_t error_size);
void snj_app_record_model_accounting(struct app_state *app,
                                     enum snj_count_capability capability,
                                     uint64_t model_input_bytes,
                                     uint64_t input_tokens,
                                     uint64_t hard_input_tokens);
int snj_app_goal_command(struct app_state *app, const char *line, bool active);
int snj_app_goal_tool(struct app_state *app,
                      const struct snj_response_item *call,
                      json_t **result, char *error, size_t error_size);
int snj_app_goal_pause(struct app_state *app, const char *reason,
                       char *error, size_t error_size);

enum snj_managed_continuation {
    SNJ_MANAGED_CONTINUATION_NONE,
    SNJ_MANAGED_CONTINUATION_MATCHED,
    SNJ_MANAGED_CONTINUATION_HANDLE_MISMATCH,
    SNJ_MANAGED_CONTINUATION_ORDERING_VIOLATION
};

enum queue_command_kind {
    QUEUE_COMMAND_LIST,
    QUEUE_COMMAND_ADD,
    QUEUE_COMMAND_DELETE,
    QUEUE_COMMAND_EDIT,
    QUEUE_COMMAND_CLEAR,
    QUEUE_COMMAND_POP
};

json_t *snj_app_preference_changed_data(const char *old_key,
                                        const char *old_value,
                                        const char *new_key,
                                        const char *new_value);
json_t *snj_app_model_selection_changed_data(
    const char *old_provider, const char *new_provider,
    const char *old_model, const char *new_model,
    const char *old_effort, const char *new_effort);
json_t *snj_app_turn_started_data(const struct app_state *app,
                                  const char *prompt,
                                  const char *turn_id,
                                  const struct snj_queued_turn *queued,
                                  bool goal_turn, bool read_only);
json_t *snj_app_steering_snapshot(const struct snj_session *session);
int snj_app_request_digests(struct app_state *app, const char *prompt,
                            const json_t *steering, unsigned int cycle,
                            const struct snj_credential *credential,
                            char input_hash[SNJ_SHA256_HEX_LEN + 1u],
                            char request_hash[SNJ_SHA256_HEX_LEN + 1u],
                            char count_request_hash[SNJ_SHA256_HEX_LEN + 1u],
                            uint64_t *input_tokens_bound,
                            uint64_t *model_input_bytes,
                            uint64_t *request_input_bytes,
                            uint64_t *request_input_count,
                            char request_input_hash[SNJ_SHA256_HEX_LEN + 1u],
                            const char *provider_source_sha256,
                            const char **count_method,
                            struct snj_buf *request_body,
                            json_t **create_request,
                            json_t **count_request,
                            char *error, size_t error_size);
json_t *snj_app_response_started_data(const char *turn_id,
                                      const char *response_id,
                                      unsigned int cycle,
                                      const char *compact_id,
                                      const char *model,
                                      const char *input_hash,
                                      const char *request_hash,
                                      const char *count_request_hash,
                                      const char *count_method,
                                      uint64_t input_tokens_bound,
                                      uint64_t model_input_bytes,
                                      uint64_t request_input_bytes,
                                      uint64_t request_input_count,
                                      const char *request_input_hash,
                                      const char *baseline_hash,
                                      const char *provider,
                                      const char *effort,
                                      const char *provider_source_sha256,
                                      const struct snj_model_capacity *capacity,
                                      const json_t *steering);
json_t *snj_app_response_capacity_rejected_data(
                                      const char *turn_id,
                                      const char *response_id,
                                      unsigned int cycle,
                                      const char *request_hash,
                                      const struct snj_provider_failure *failure,
                                      const struct snj_model_capacity *capacity,
                                      const char *provider_source_sha256);
json_t *snj_app_response_completed_data(const char *turn_id,
                                        const char *response_id,
                                        unsigned int cycle,
                                        const struct snj_response_graph *graph);
json_t *snj_app_response_output_correction_data(
                                        const char *turn_id,
                                        const char *response_id,
                                        unsigned int cycle,
                                        const char *correction_id,
                                        const char *text,
                                        json_t *partial_public);
json_t *snj_app_turn_completed_data(const char *turn_id,
                                    const char *response_id,
                                    const char *item_id);
json_t *snj_app_steering_added_data(const char *turn_id,
                                    const char *steering_id,
                                    const char *text);
json_t *snj_app_future_turn_queued_data(const char *turn_id,
                                        const char *queue_id,
                                        const char *text, bool read_only);
json_t *snj_app_future_turn_edited_data(const char *queue_id,
                                        const char *text, bool read_only);
json_t *snj_app_future_turn_cancelled_data(const struct snj_session *session,
                                           const bool remove[SNJ_MAX_PENDING_TURNS]);
json_t *snj_app_response_interrupted_data(const char *turn_id,
                                          const char *response_id,
                                          unsigned int cycle,
                                          const char *origin,
                                          const char *reason,
                                          json_t *partial_public);
json_t *snj_app_turn_interrupted_data(const char *turn_id,
                                      const char *origin,
                                      const char *reason);
json_t *snj_app_response_failed_data(const char *turn_id,
                                     const char *response_id,
                                     unsigned int cycle,
                                     const char *class_name,
                                     const char *message,
                                     json_t *partial_public,
                                     unsigned int retry_count);
json_t *snj_app_turn_failed_data(const char *turn_id,
                                 const char *class_name,
                                 const char *message);
json_t *snj_app_tool_started_data(const char *turn_id,
                                  const char *call_id,
                                  const char *action_sha256,
                                  const char *workspace);
json_t *snj_app_tool_finished_data(const char *turn_id,
                                   const char *call_id,
                                   json_t *result);
json_t *snj_app_process_closed_data(const char *turn_id,
                                    const char *handle,
                                    const char *cause,
                                    json_t *result);
int snj_app_compact_idle_command(struct app_state *app, const char *reason,
                                 char *error, size_t error_size);
int snj_app_compact_after_turn(struct app_state *app, uint64_t input_tokens_bound,
                               const char *count_method,
                               char *error, size_t error_size);
int snj_app_compact_before_response(struct app_state *app,
                                    const struct snj_credential *credential,
                                    uint64_t input_tokens_bound,
                                    const char *count_method, bool *compacted,
                                    char *error, size_t error_size);
int snj_app_compact_after_capacity_rejection(
                                    struct app_state *app,
                                    const struct snj_credential *credential,
                                    bool *compacted,
                                    char *error, size_t error_size);
void snj_app_response_cycle_release(struct app_state *app,
                                    struct snj_response_graph *graph,
                                    json_t **steering, json_t **create_request,
                                    json_t **count_request,
                                    struct snj_buf *request_body);
enum snj_managed_continuation snj_app_managed_continuation_classify(
    const struct app_state *app, const struct snj_response_graph *graph,
    const struct snj_graph_decision *decision);
int snj_app_lifecycle_command(struct app_state *app, const char *line,
                              bool *handled, bool *exit_now);
int snj_app_parse_queue_argument(const char *argument,
                                 enum queue_command_kind *kind,
                                 size_t *number);

int snj_app_active_input_pump(void *opaque, unsigned int timeout_ms);
int snj_app_irc_event(void *opaque, const struct snj_irc_event *event);
int snj_app_irc_trace(void *opaque, unsigned int level, char direction,
                      const char *endpoint, const char *text, size_t len);
int snj_app_irc_restore(struct app_state *app, char *error, size_t error_size);
int snj_app_irc_flush_urgent(struct app_state *app,
                             char *error, size_t error_size);
char *snj_app_irc_take_pending(struct app_state *app,
                               bool *local_operator, bool force_background);
int snj_app_irc_snapshot(struct app_state *app, const char *reason,
                         char *error, size_t error_size);
bool snj_app_exact_count_enabled(enum snj_token_count_mode mode,
                                 enum snj_count_capability capability);
int snj_app_provider_count(struct app_state *app, const json_t *count_request,
                           const struct snj_credential *credential,
                           uint64_t model_input_bytes, uint64_t *input_tokens,
                           const char **count_method,
                           char *error, size_t error_size);
int snj_app_provider_models(struct app_state *app,
                            const struct snj_provider_config *provider,
                            json_t **models,
                            char *error, size_t error_size);
int snj_app_provider_compact(struct app_state *app, const json_t *compact_request,
                             const struct snj_credential *credential,
                             json_t **output, uint64_t *output_tokens_bound,
                             char *error, size_t error_size);
int snj_app_provider_run(struct app_state *app, const char *prompt,
                         const json_t *steering, unsigned int cycle,
                         const json_t *create_request,
                         const struct snj_credential *credential,
                         struct snj_response_graph *graph,
                         struct snj_provider_failure *failure,
                         char *error, size_t error_size,
                         unsigned int *retry_count);
int snj_app_tool_run(struct app_state *app,
                     const struct snj_response_item *call,
                     const struct snj_credential *credential,
                     json_t **result, char *error, size_t error_size);

void snj_app_clear_partial_public(struct app_state *app);
json_t *snj_app_partial_public_json(const struct app_state *app);
int snj_app_finish_stream_item(struct app_state *app);
int snj_app_abort_stream_item(struct app_state *app);
int snj_app_stream_public(void *opaque, size_t item_index,
                          enum snj_item_kind kind,
                          enum snj_item_phase phase,
                          const char *text, size_t len);
int snj_app_stream_public_response(void *opaque, size_t item_index,
                                   enum snj_item_kind kind,
                                   enum snj_item_phase phase,
                                   const char *provider_item_id,
                                   const char *text, size_t len);
void snj_app_reset_stream(struct app_state *app);

#endif
