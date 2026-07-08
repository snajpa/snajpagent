/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_APP_INTERNAL_H
#define SNAJPAGENT_APP_INTERNAL_H

#include "app.h"
#include "base.h"
#include "cli.h"
#include "config.h"
#include "credential.h"
#include "instructions.h"
#include "render.h"
#include "store.h"
#include "term.h"
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
    struct snj_render render;
    struct snj_term term;
    struct snj_instruction_set turn_instructions;
    const struct snj_cli *cli;
    const struct snj_config *config;
    const char *turn_model;
    const char *turn_effort;
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
    bool steering_requested;
    bool interrupt_requested;
    bool queue_armed;
    bool input_closed;
    bool execute;
    uint64_t active_since_ms;
    bool activity_shown;
};

json_t *snj_app_preference_changed_data(const char *old_key,
                                        const char *old_value,
                                        const char *new_key,
                                        const char *new_value);
json_t *snj_app_turn_started_data(const struct app_state *app,
                                  const char *prompt,
                                  const char *turn_id,
                                  const struct snj_queued_turn *queued);
json_t *snj_app_steering_snapshot(const struct snj_session *session);
int snj_app_request_digests(struct app_state *app, const char *prompt,
                            const json_t *steering, unsigned int cycle,
                            const struct snj_credential *credential,
                            char input_hash[SNJ_SHA256_HEX_LEN + 1u],
                            char request_hash[SNJ_SHA256_HEX_LEN + 1u],
                            char count_request_hash[SNJ_SHA256_HEX_LEN + 1u],
                            uint64_t *input_tokens_bound,
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
                                      const json_t *steering);
json_t *snj_app_response_completed_data(const char *turn_id,
                                        const char *response_id,
                                        unsigned int cycle,
                                        const struct snj_response_graph *graph);
json_t *snj_app_turn_completed_data(const char *turn_id,
                                    const char *response_id,
                                    const char *item_id);
json_t *snj_app_steering_added_data(const char *turn_id,
                                    const char *steering_id,
                                    const char *text);
json_t *snj_app_future_turn_queued_data(const char *turn_id,
                                        const char *queue_id,
                                        const char *text);
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
void snj_app_response_cycle_release(struct app_state *app,
                                    struct snj_response_graph *graph,
                                    json_t **steering, json_t **create_request,
                                    json_t **count_request,
                                    struct snj_buf *request_body);
bool snj_app_managed_continuation_graph_matches(const struct app_state *app,
                                               const struct snj_response_graph *graph,
                                               const struct snj_graph_decision *decision);
int snj_app_lifecycle_command(struct app_state *app, const char *line,
                              bool *handled, bool *exit_now);

int snj_app_active_input_pump(void *opaque, unsigned int timeout_ms);
int snj_app_provider_count(struct app_state *app, const json_t *count_request,
                           const struct snj_credential *credential,
                           uint64_t *input_tokens,
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
