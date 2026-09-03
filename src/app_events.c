/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"

#include "context.h"
#include "json.h"
#include "secret.h"
#include "snajpagent.h"
#include "wire.h"

#include <string.h>

json_t *
snj_app_preference_changed_data(const char *old_key, const char *old_value,
                        const char *new_key, const char *new_value)
{
    json_t *data = json_object();

    if (!data ||
        snj_json_set_new(data, new_key, json_string(new_value)) < 0 ||
        snj_json_set_new(data, old_key, json_string(old_value)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
}

static json_t *
turn_config(const struct app_state *app)
{
    json_t *config = json_object();
    if (!config ||
        snj_json_set_new(config, "capability_version",
                         json_string(SNAJPAGENT_CAPABILITY_VERSION)) < 0 ||
        snj_json_set_new(config, "effort", json_string(app->turn_effort)) < 0 ||
        snj_json_set_new(config, "max_output_tokens",
                         json_integer(SNAJPAGENT_MAX_OUTPUT_TOKENS)) < 0 ||
        snj_json_set_new(config, "model", json_string(app->turn_model)) < 0 ||
        snj_json_set_new(config, "profile_id",
                         json_string(SNAJPAGENT_PROFILE_ID)) < 0 ||
        snj_json_set_new(config, "prompt_schema", json_integer(1)) < 0 ||
        snj_json_set_new(config, "replay_schema", json_integer(1)) < 0 ||
        snj_json_set_new(config, "tool_schema", json_integer(1)) < 0) {
        if (config)
            json_decref(config);
        return NULL;
    }
    return config;
}

json_t *
snj_app_turn_started_data(const struct app_state *app, const char *prompt,
                  const char *turn_id, const struct snj_queued_turn *queued)
{
    json_t *data = json_object();
    json_t *instructions = snj_instructions_metadata_json(&app->turn_instructions);

    if (!data || !instructions ||
        snj_json_set_new(data, "config", turn_config(app)) < 0 ||
        snj_json_set_new(data, "input_kind",
                         json_string(queued ? "queued" : "direct")) < 0 ||
        snj_json_set_new(data, "instructions", instructions) < 0)
        goto fail;
    instructions = NULL;
    if (snj_json_set_new(data, "queue_id",
                         queued ? json_string(queued->queue_id) : json_null()) < 0 ||
        snj_json_set_new(data, "queue_seq",
                         queued ? json_integer((json_int_t)queued->seq) :
                                  json_null()) < 0 ||
        snj_json_set_new(data, "text", json_string(prompt)) < 0 ||
        snj_json_set_new(data, "turn_id", json_string(turn_id)) < 0 ||
        snj_json_set_new(data, "turn_number",
                         json_integer((json_int_t)(app->session.turn_count + 1u))) < 0 ||
        snj_json_set_new(data, "workspace",
                         json_string(app->session.workspace)) < 0)
        goto fail;
    return data;
fail:
    if (instructions)
        json_decref(instructions);
    if (data)
        json_decref(data);
    return NULL;
}

json_t *
snj_app_steering_snapshot(const struct snj_session *session)
{
    json_t *array = json_array();

    if (!array)
        return NULL;
    for (size_t i = 0; i < session->pending_steering_count; ++i) {
        json_t *item = json_object();
        if (!item ||
            snj_json_set_new(item, "id",
                             json_string(session->pending_steering[i].steering_id)) < 0 ||
            snj_json_set_new(item, "text",
                             json_string(session->pending_steering[i].text)) < 0) {
            if (item)
                json_decref(item);
            json_decref(array);
            return NULL;
        }
        if (json_array_append_new(array, item) < 0) {
            json_decref(array);
            return NULL;
        }
    }
    return array;
}

int
snj_app_request_digests(struct app_state *app, const char *prompt,
                const json_t *steering, unsigned int cycle,
                const struct snj_credential *credential,
                char input_hash[SNJ_SHA256_HEX_LEN + 1u],
                char request_hash[SNJ_SHA256_HEX_LEN + 1u],
                char count_request_hash[SNJ_SHA256_HEX_LEN + 1u],
                uint64_t *input_tokens_bound, struct snj_buf *request_body,
                json_t **create_request, json_t **count_request,
                char *error, size_t error_size)
{
    struct snj_context_projection projection;
    int rc;

    (void)prompt;
    if (create_request)
        *create_request = NULL;
    if (count_request)
        *count_request = NULL;
    snj_context_projection_init(&projection);
    rc = snj_context_build(&app->session, app->turn_model, app->turn_effort,
                           cycle, steering,
                           &app->turn_instructions, &projection,
                           error, error_size);
    if (rc == 0) {
        memcpy(input_hash, projection.model_input_sha256,
               SNJ_SHA256_HEX_LEN + 1u);
        memcpy(request_hash, projection.request_sha256,
               SNJ_SHA256_HEX_LEN + 1u);
        memcpy(count_request_hash, projection.count_request_sha256,
               SNJ_SHA256_HEX_LEN + 1u);
        *input_tokens_bound = projection.input_tokens_bound;
        if (create_request)
            *create_request = json_incref(projection.create_request);
        if (count_request)
            *count_request = json_incref(projection.count_request);
        if (app->render.verbosity >= 5u && request_body) {
            struct snj_buf encoded;
            struct snj_secret_set secrets;

            snj_buf_init(&encoded, SNJ_WIRE_BODY_MAX);
            snj_secret_set_build(&secrets, app->config, credential);
            if (projection.create_request_bytes <= SNJ_WIRE_BODY_MAX &&
                snj_json_canonical(projection.create_request, &encoded) == 0 &&
                snj_wire_json_redact(encoded.data, encoded.len, &secrets.wire,
                                     request_body, error, error_size) == 0) {
                /* sanitized canonical request captured for durable-fenced rendering */
            } else {
                snj_buf_reset(request_body);
                if (snj_buf_printf(request_body,
                        "<request body omitted; bytes=%zu; sha256=%s>\n",
                        projection.create_request_bytes,
                        projection.request_sha256) < 0)
                    rc = -1;
            }
            snj_buf_free(&encoded);
        }
    }
    if (rc < 0) {
        if (create_request && *create_request) {
            json_decref(*create_request);
            *create_request = NULL;
        }
        if (count_request && *count_request) {
            json_decref(*count_request);
            *count_request = NULL;
        }
    }
    snj_context_projection_free(&projection);
    return rc;
}


json_t *
snj_app_response_started_data(const char *turn_id, const char *response_id,
                      unsigned int cycle, const char *compact_id,
                      const char *model,
                      const char *input_hash,
                      const char *request_hash, const char *count_request_hash,
                      const char *count_method, uint64_t input_tokens_bound,
                      const json_t *steering)
{
    json_t *data = json_object();
    json_t *ids = json_array();

    if (!data || !ids || !steering)
        goto fail;
    for (size_t i = 0; i < json_array_size(steering); ++i) {
        json_t *item = json_array_get(steering, i);
        const char *id = snj_json_string(item, "id");
        if (!id || json_array_append_new(ids, json_string(id)) < 0)
            goto fail;
    }
    if (!model || !count_method || !count_request_hash)
        goto fail;
    if (snj_json_set_new(data, "baseline_sha256", json_null()) < 0 ||
        snj_json_set_new(data, "capability_version",
                         json_string(SNAJPAGENT_CAPABILITY_VERSION)) < 0 ||
        snj_json_set_new(data, "compact_id",
                         compact_id && *compact_id ?
                         json_string(compact_id) : json_null()) < 0 ||
        snj_json_set_new(data, "count_method", json_string(count_method)) < 0 ||
        snj_json_set_new(data, "count_request_sha256",
                         json_string(count_request_hash)) < 0 ||
        snj_json_set_new(data, "cycle", json_integer((json_int_t)cycle)) < 0 ||
        snj_json_set_new(data, "input_tokens_bound",
                         json_integer((json_int_t)input_tokens_bound)) < 0 ||
        snj_json_set_new(data, "model", json_string(model)) < 0 ||
        snj_json_set_new(data, "model_input_sha256", json_string(input_hash)) < 0 ||
        snj_json_set_new(data, "profile_id",
                         json_string(SNAJPAGENT_PROFILE_ID)) < 0 ||
        snj_json_set_new(data, "request_sha256", json_string(request_hash)) < 0 ||
        snj_json_set_new(data, "response_id", json_string(response_id)) < 0)
        goto fail;
    {
        int rc = snj_json_set_new(data, "steering_ids", ids);
        ids = NULL;
        if (rc < 0)
            goto fail;
    }
    if (snj_json_set_new(data, "turn_id", json_string(turn_id)) < 0)
        goto fail;
    return data;
fail:
    if (ids)
        json_decref(ids);
    if (data)
        json_decref(data);
    return NULL;
}

json_t *
snj_app_response_completed_data(const char *turn_id, const char *response_id,
                        unsigned int cycle,
                        const struct snj_response_graph *graph)
{
    json_t *data = json_object();
    json_t *items = snj_response_graph_json(graph);
    json_t *value;

    if (!data || !items)
        goto fail;
    if (snj_json_set_new(data, "cycle", json_integer((json_int_t)cycle)) < 0)
        goto fail;
    value = items;
    items = NULL;
    if (snj_json_set_new(data, "items", value) < 0)
        goto fail;
    if (snj_json_set_new(data, "provider_response_id",
                         json_string(graph->provider_response_id)) < 0 ||
        snj_json_set_new(data, "response_id", json_string(response_id)) < 0 ||
        snj_json_set_new(data, "status", json_string("completed")) < 0 ||
        snj_json_set_new(data, "turn_id", json_string(turn_id)) < 0 ||
        snj_json_set_new(data, "usage",
                         snj_response_usage_json(&graph->usage)) < 0)
        goto fail;
    return data;
fail:
    if (items)
        json_decref(items);
    if (data)
        json_decref(data);
    return NULL;
}

json_t *
snj_app_turn_completed_data(const char *turn_id, const char *response_id,
                    const char *item_id)
{
    json_t *data = json_object();
    if (!data ||
        snj_json_set_new(data, "final_item_id", json_string(item_id)) < 0 ||
        snj_json_set_new(data, "final_response_id", json_string(response_id)) < 0 ||
        snj_json_set_new(data, "turn_id", json_string(turn_id)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
}

json_t *
snj_app_steering_added_data(const char *turn_id, const char *steering_id,
                    const char *text)
{
    json_t *data = json_object();
    if (!data ||
        snj_json_set_new(data, "steering_id", json_string(steering_id)) < 0 ||
        snj_json_set_new(data, "text", json_string(text)) < 0 ||
        snj_json_set_new(data, "turn_id", json_string(turn_id)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
}

json_t *
snj_app_future_turn_queued_data(const char *turn_id, const char *queue_id,
                        const char *text)
{
    json_t *data = json_object();
    if (!data || snj_json_set_new(data, "queue_id", json_string(queue_id)) < 0 ||
        snj_json_set_new(data, "text", json_string(text)) < 0 ||
        snj_json_set_new(data, "while_turn_id", json_string(turn_id)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
}

json_t *
snj_app_future_turn_cancelled_data(const struct snj_session *session,
                           const bool remove[SNJ_MAX_PENDING_TURNS])
{
    json_t *data = json_object();
    json_t *ids = json_array();

    if (!data || !ids)
        goto fail;
    for (size_t i = 0; i < session->pending_queue_count; ++i) {
        if (remove[i] && json_array_append_new(ids,
                json_string(session->pending_queue[i].queue_id)) < 0)
            goto fail;
    }
    if (json_array_size(ids) == 0u)
        goto fail;
    {
        int rc = snj_json_set_new(data, "queue_ids", ids);
        ids = NULL;
        if (rc < 0)
            goto fail;
    }
    if (snj_json_set_new(data, "reason", json_string("user")) < 0)
        goto fail;
    return data;
fail:
    if (ids)
        json_decref(ids);
    if (data)
        json_decref(data);
    return NULL;
}

json_t *
snj_app_response_interrupted_data(const char *turn_id, const char *response_id,
                          unsigned int cycle, const char *origin,
                          const char *reason, json_t *partial_public)
{
    json_t *data = json_object();
    json_t *partial = partial_public ? partial_public : json_array();

    if (!data || !partial)
        goto fail;
    if (
        snj_json_set_new(data, "cycle", json_integer((json_int_t)cycle)) < 0 ||
        snj_json_set_new(data, "origin", json_string(origin)) < 0)
        goto fail;
    {
        int rc = snj_json_set_new(data, "partial_public", partial);
        partial = NULL;
        if (rc < 0)
            goto fail;
    }
    if (snj_json_set_new(data, "reason", json_string(reason)) < 0 ||
        snj_json_set_new(data, "response_id", json_string(response_id)) < 0 ||
        snj_json_set_new(data, "turn_id", json_string(turn_id)) < 0)
        goto fail;
    return data;
fail:
    if (partial)
        json_decref(partial);
    if (data)
        json_decref(data);
    return NULL;
}

json_t *
snj_app_turn_interrupted_data(const char *turn_id, const char *origin, const char *reason)
{
    json_t *data = json_object();
    if (!data || snj_json_set_new(data, "origin", json_string(origin)) < 0 ||
        snj_json_set_new(data, "reason", json_string(reason)) < 0 ||
        snj_json_set_new(data, "turn_id", json_string(turn_id)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
}

json_t *
snj_app_response_failed_data(const char *turn_id, const char *response_id,
                     unsigned int cycle, const char *class_name,
                     const char *message, json_t *partial_public,
                     unsigned int retry_count)
{
    json_t *data = json_object();
    json_t *partial = partial_public ? partial_public : json_array();

    if (!data || !partial ||
        snj_json_set_new(data, "class", json_string(class_name)) < 0 ||
        snj_json_set_new(data, "cycle", json_integer((json_int_t)cycle)) < 0 ||
        snj_json_set_new(data, "message", json_string(message)) < 0)
        goto fail;
    {
        int rc = snj_json_set_new(data, "partial_public", partial);
        partial = NULL;
        if (rc < 0)
            goto fail;
    }
    if (
        snj_json_set_new(data, "response_id", json_string(response_id)) < 0 ||
        snj_json_set_new(data, "retry_count",
                         json_integer((json_int_t)retry_count)) < 0 ||
        snj_json_set_new(data, "turn_id", json_string(turn_id)) < 0)
        goto fail;
    return data;
fail:
    if (partial)
        json_decref(partial);
    if (data)
        json_decref(data);
    return NULL;
}

json_t *
snj_app_turn_failed_data(const char *turn_id, const char *class_name, const char *message)
{
    json_t *data = json_object();
    if (!data || snj_json_set_new(data, "class", json_string(class_name)) < 0 ||
        snj_json_set_new(data, "message", json_string(message)) < 0 ||
        snj_json_set_new(data, "turn_id", json_string(turn_id)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
}

json_t *
snj_app_tool_started_data(const char *turn_id, const char *call_id,
                  const char *action_sha256, const char *workspace)
{
    json_t *data = json_object();
    if (!data ||
        snj_json_set_new(data, "action_sha256", json_string(action_sha256)) < 0 ||
        snj_json_set_new(data, "call_id", json_string(call_id)) < 0 ||
        snj_json_set_new(data, "resolved_workdir", json_string(workspace)) < 0 ||
        snj_json_set_new(data, "turn_id", json_string(turn_id)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
}

/* Takes ownership of result, including on failure. */
json_t *
snj_app_tool_finished_data(const char *turn_id, const char *call_id, json_t *result)
{
    json_t *data = json_object();
    json_t *value;
    if (!data) {
        if (result)
            json_decref(result);
        return NULL;
    }
    if (snj_json_set_new(data, "call_id", json_string(call_id)) < 0)
        goto fail;
    value = result;
    result = NULL;
    if (snj_json_set_new(data, "result", value) < 0 ||
        snj_json_set_new(data, "turn_id", json_string(turn_id)) < 0)
        goto fail;
    return data;
fail:
    if (result)
        json_decref(result);
    json_decref(data);
    return NULL;
}


/* Takes ownership of result, including on failure. */
json_t *
snj_app_process_closed_data(const char *turn_id, const char *handle,
                            const char *cause, json_t *result)
{
    json_t *data = json_object();
    json_t *value;

    if (!data) {
        if (result)
            json_decref(result);
        return NULL;
    }
    if (snj_json_set_new(data, "cause", json_string(cause)) < 0 ||
        snj_json_set_new(data, "handle", json_string(handle)) < 0)
        goto fail;
    value = result;
    result = NULL;
    if (snj_json_set_new(data, "result", value) < 0 ||
        snj_json_set_new(data, "turn_id", json_string(turn_id)) < 0)
        goto fail;
    return data;

fail:
    if (result)
        json_decref(result);
    if (data)
        json_decref(data);
    return NULL;
}
