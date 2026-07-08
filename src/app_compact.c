/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"

#include "context.h"
#include "json.h"
#include "provider.h"
#include "snajpagent.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static int
hash_json_bounded(const json_t *value, size_t max,
                  char hash[SNJ_SHA256_HEX_LEN + 1u], size_t *bytes,
                  char *error, size_t error_size)
{
    struct snj_buf encoded;
    int rc = -1;

    if (hash)
        hash[0] = '\0';
    if (bytes)
        *bytes = 0u;
    snj_buf_init(&encoded, max);
    if (snj_json_canonical(value, &encoded) < 0) {
        snprintf(error, error_size, "canonical compaction JSON exceeds bound");
        goto out;
    }
    if (hash)
        snj_sha256_hex(encoded.data, encoded.len, hash);
    if (bytes)
        *bytes = encoded.len;
    rc = 0;
out:
    snj_buf_free(&encoded);
    return rc;
}

static bool
count_method_valid(const char *method)
{
    return method && (strcmp(method, "exact") == 0 ||
                      strcmp(method, "qualified_upper_bound") == 0);
}

static json_t *
compaction_started_data(const struct snj_session *session,
                        const char *model, const char *compact_id,
                        const char *reason, const char *count_method,
                        uint64_t source_seq, const char *source_hash,
                        const char *request_hash,
                        const char *count_request_hash,
                        uint64_t input_tokens_bound)
{
    json_t *data = json_object();

    if (!data ||
        snj_json_set_new(data, "capability_version",
                         json_string(SNAJPAGENT_CAPABILITY_VERSION)) < 0 ||
        snj_json_set_new(data, "compact_id", json_string(compact_id)) < 0 ||
        snj_json_set_new(data, "count_method", json_string(count_method)) < 0 ||
        snj_json_set_new(data, "count_request_sha256",
                         json_string(count_request_hash)) < 0 ||
        snj_json_set_new(data, "input_tokens_bound",
                         json_integer((json_int_t)input_tokens_bound)) < 0 ||
        snj_json_set_new(data, "model", json_string(model)) < 0 ||
        snj_json_set_new(data, "predecessor_compact_id",
                         session->compact_id[0] ?
                         json_string(session->compact_id) : json_null()) < 0 ||
        snj_json_set_new(data, "profile_id",
                         json_string(SNAJPAGENT_PROFILE_ID)) < 0 ||
        snj_json_set_new(data, "reason", json_string(reason)) < 0 ||
        snj_json_set_new(data, "request_sha256",
                         json_string(request_hash)) < 0 ||
        snj_json_set_new(data, "source_seq",
                         json_integer((json_int_t)source_seq)) < 0 ||
        snj_json_set_new(data, "source_sha256",
                         json_string(source_hash)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
}

static json_t *
compaction_completed_data(const char *compact_id,
                          const char *source_hash,
                          const char *output_hash,
                          const char *count_method,
                          const char *output_count_method,
                          const char *output_count_request_hash,
                          uint64_t input_tokens_bound,
                          uint64_t output_tokens_bound,
                          const json_t *output)
{
    json_t *data = json_object();

    if (!data ||
        snj_json_set_new(data, "compact_id", json_string(compact_id)) < 0 ||
        snj_json_set_new(data, "count_method", json_string(count_method)) < 0 ||
        snj_json_set_new(data, "input_tokens_bound",
                         json_integer((json_int_t)input_tokens_bound)) < 0 ||
        snj_json_set_new(data, "output", json_deep_copy(output)) < 0 ||
        snj_json_set_new(data, "output_count_method",
                         json_string(output_count_method)) < 0 ||
        snj_json_set_new(data, "output_count_request_sha256",
                         json_string(output_count_request_hash)) < 0 ||
        snj_json_set_new(data, "output_sha256", json_string(output_hash)) < 0 ||
        snj_json_set_new(data, "output_tokens_bound",
                         json_integer((json_int_t)output_tokens_bound)) < 0 ||
        snj_json_set_new(data, "source_sha256",
                         json_string(source_hash)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
}

static int
commit_rendered(struct app_state *app, const char *type, json_t *data,
                char *error, size_t error_size)
{
    uint64_t seq;

    if (!data) {
        snprintf(error, error_size, "cannot allocate %s event", type);
        errno = ENOMEM;
        return -1;
    }
    if (snj_session_commit(&app->session, type, data, &seq,
                           error, error_size) < 0)
        return -1;
    if (snj_render_event(&app->render, seq, type) < 0) {
        snprintf(error, error_size, "durable compaction event output failed");
        return -1;
    }
    return 0;
}

static void
clear_active_compaction(struct snj_session *session)
{
    session->active_compact_id[0] = '\0';
    session->active_compact_source_sha256[0] = '\0';
    session->active_compact_source_seq = 0u;
}

static int
compaction_state_valid(const struct app_state *app, const char *reason,
                       bool active_prefix, char *error, size_t error_size)
{
    if (!app || !app->config || !reason ||
        (strcmp(reason, "manual") != 0 && strcmp(reason, "automatic") != 0)) {
        snprintf(error, error_size, "invalid compaction reason");
        errno = EINVAL;
        return -1;
    }
    if (active_prefix) {
        if (strcmp(reason, "automatic") != 0 || !app->session.active_turn ||
            app->session.response_open ||
            app->session.active_process_handle[0] != '\0' ||
            app->session.active_compact_id[0] != '\0') {
            snprintf(error, error_size,
                     "pre-response compaction requires an active turn before response");
            errno = EINVAL;
            return -1;
        }
        return 0;
    }
    if (app->session.active_turn || app->session.response_open ||
        app->session.active_process_handle[0] != '\0' ||
        app->session.active_compact_id[0] != '\0') {
        snprintf(error, error_size, "compaction requires an idle session");
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int
build_compaction_request(struct app_state *app, bool active_prefix,
                         const char *model, const char *effort,
                         json_t **request, json_t **count_request,
                         char source_hash[SNJ_SHA256_HEX_LEN + 1u],
                         size_t *source_bytes,
                         char request_hash[SNJ_SHA256_HEX_LEN + 1u],
                         size_t *request_bytes, uint64_t *source_seq,
                         char *error, size_t error_size)
{
    if (active_prefix)
        return snj_context_compact_active_prefix_request_build(&app->session,
            model, effort, request, count_request, source_hash, source_bytes,
            request_hash, request_bytes, source_seq, error, error_size);
    return snj_context_compact_request_build(&app->session, model, effort,
        request, count_request, source_hash, source_bytes, request_hash,
        request_bytes, source_seq, error, error_size);
}

#ifndef SNAJPAGENT_TEST_FIXTURE
static json_t *
reasoning_settings(const char *effort)
{
    json_t *settings = json_object();

    if (!settings ||
        snj_json_set_new(settings, "effort", json_string(effort)) < 0) {
        if (settings)
            json_decref(settings);
        return NULL;
    }
    return settings;
}

static json_t *
responses_compact_create_request(const json_t *compact_request,
                                 const char *model, const char *effort)
{
    static const char instruction[] =
        "Compact the prior conversation for future Responses turns. Return "
        "exactly this JSON shape and nothing else: [{\"type\":\"message\","
        "\"role\":\"developer\",\"content\":\"<compact summary>\"}]. "
        "Write the compact summary so it preserves the user's goals, decisions, "
        "constraints, repository state, active blockers, and next steps. Do not "
        "use markdown fences, prose outside JSON, or tool calls.";
    json_t *request = json_object();
    json_t *input = compact_request ? json_object_get(compact_request, "input") : NULL;
    json_t *input_copy = NULL;
    json_t *compact_instruction = json_object();

    if (!request || !json_is_array(input) || !compact_instruction ||
        !model || !effort)
        goto fail;
    input_copy = json_deep_copy(input);
    if (!input_copy ||
        snj_json_set_new(compact_instruction, "content",
                         json_string(instruction)) < 0 ||
        snj_json_set_new(compact_instruction, "role",
                         json_string("developer")) < 0 ||
        json_array_append_new(input_copy, compact_instruction) < 0)
        goto fail;
    compact_instruction = NULL;
    if (snj_json_set_new(request, "input", input_copy) < 0)
        goto fail;
    input_copy = NULL;
    if (snj_json_set_new(request, "max_output_tokens",
                         json_integer(SNAJPAGENT_MAX_OUTPUT_TOKENS)) < 0 ||
        snj_json_set_new(request, "model", json_string(model)) < 0 ||
        snj_json_set_new(request, "parallel_tool_calls", json_false()) < 0 ||
        snj_json_set_new(request, "reasoning", reasoning_settings(effort)) < 0 ||
        snj_json_set_new(request, "store", json_false()) < 0 ||
        snj_json_set_new(request, "stream", json_true()) < 0 ||
        snj_json_set_new(request, "tool_choice", json_string("none")) < 0 ||
        snj_json_set_new(request, "tools", json_array()) < 0 ||
        snj_json_set_new(request, "truncation", json_string("disabled")) < 0)
        goto fail;
    return request;

fail:
    if (compact_instruction)
        json_decref(compact_instruction);
    if (input_copy)
        json_decref(input_copy);
    if (request)
        json_decref(request);
    return NULL;
}
#endif

static int
run_responses_compaction(struct app_state *app, const json_t *compact_request,
                         const struct snj_credential *credential,
                         const char *model, const char *effort,
                         json_t **output, uint64_t *output_tokens_bound,
                         char *error, size_t error_size)
{
    char output_hash[SNJ_SHA256_HEX_LEN + 1u];
    size_t output_bytes = 0u;

    if (output)
        *output = NULL;
    if (output_tokens_bound)
        *output_tokens_bound = 0u;
    if (!output || !output_tokens_bound) {
        snprintf(error, error_size, "invalid Responses compaction output");
        errno = EINVAL;
        return -1;
    }
#ifdef SNAJPAGENT_TEST_FIXTURE
    json_t *fixture_output = json_array();
    json_t *item = json_object();

    (void)app;
    (void)compact_request;
    (void)credential;
    (void)model;
    (void)effort;
    if (!fixture_output || !item ||
        snj_json_set_new(item, "content",
                         json_string("fixture responses compact summary")) < 0 ||
        snj_json_set_new(item, "role", json_string("developer")) < 0 ||
        snj_json_set_new(item, "type", json_string("message")) < 0 ||
        json_array_append_new(fixture_output, item) < 0) {
        if (item)
            json_decref(item);
        if (fixture_output)
            json_decref(fixture_output);
        return -1;
    }
    item = NULL;
    if (snj_context_compact_output_valid(fixture_output, output_hash,
                                         &output_bytes, error, error_size) < 0) {
        json_decref(fixture_output);
        return -1;
    }
    *output = fixture_output;
    *output_tokens_bound = (uint64_t)output_bytes;
    return 0;
#else
    struct snj_response_graph graph;
    struct snj_graph_decision decision;
    json_t *create_request = NULL;
    int cancel_code = 0;
    int rc = -1;

    create_request = responses_compact_create_request(compact_request,
                                                      model, effort);
    if (!create_request) {
        snprintf(error, error_size, "cannot build Responses compact request");
        errno = ENOMEM;
        return -1;
    }
    snj_response_graph_init(&graph);
    if (snj_provider_responses_create(create_request, app->config, credential,
                                      &app->render, NULL, NULL,
                                      snj_app_active_input_pump, app, &graph,
                                      error, error_size, &cancel_code, NULL) != 0)
        goto out;
    if (snj_response_graph_classify(&graph, &decision,
                                    error, error_size) < 0)
        goto out;
    if (decision.outcome != SNJ_GRAPH_FINAL ||
        decision.final_index >= graph.count ||
        !graph.items[decision.final_index].text) {
        snprintf(error, error_size,
                 "Responses compaction did not return a final JSON answer");
        errno = EPROTO;
        goto out;
    }
    *output = snj_json_load_strict(
        (const unsigned char *)graph.items[decision.final_index].text,
        strlen(graph.items[decision.final_index].text),
        SNJ_CONTEXT_MAX_COMPACT, error, error_size);
    if (!*output)
        goto out;
    if (snj_context_compact_output_valid(*output, output_hash, &output_bytes,
                                         error, error_size) < 0) {
        json_decref(*output);
        *output = NULL;
        goto out;
    }
    *output_tokens_bound = (uint64_t)output_bytes;
    rc = 0;
out:
    snj_response_graph_free(&graph);
    json_decref(create_request);
    return rc;
#endif
}

static int
run_compaction(struct app_state *app, const char *reason, bool active_prefix,
               const struct snj_credential *provided_credential,
               bool *compacted, char *error, size_t error_size)
{
    struct snj_credential owned_credential;
    const struct snj_credential *credential = provided_credential;
    json_t *request = NULL;
    json_t *count_request = NULL;
    json_t *output_count_request = NULL;
    json_t *output = NULL;
    char compact_id[SNJ_ID_HEX_LEN + 1u];
    char source_hash[SNJ_SHA256_HEX_LEN + 1u];
    char request_hash[SNJ_SHA256_HEX_LEN + 1u];
    char count_request_hash[SNJ_SHA256_HEX_LEN + 1u];
    char output_hash[SNJ_SHA256_HEX_LEN + 1u];
    char output_count_request_hash[SNJ_SHA256_HEX_LEN + 1u];
    const char *count_method = "qualified_upper_bound";
    const char *output_count_method = "qualified_upper_bound";
    const char *model;
    const char *effort;
    size_t source_bytes = 0u;
    size_t request_bytes = 0u;
    size_t count_request_bytes = 0u;
    size_t output_count_request_bytes = 0u;
    size_t output_bytes = 0u;
    uint64_t input_tokens_bound = 0u;
    uint64_t output_tokens_bound = 0u;
    uint64_t source_seq;
    bool started = false;
    int build_rc;
    int rc = -1;

    snj_credential_clear(&owned_credential);
    if (compacted)
        *compacted = false;
    if (compaction_state_valid(app, reason, active_prefix,
                               error, error_size) < 0)
        return -1;
    model = active_prefix && app->turn_model ? app->turn_model :
                                               app->session.default_model;
    effort = active_prefix && app->turn_effort ? app->turn_effort :
                                                 app->session.default_effort;
    build_rc = build_compaction_request(app, active_prefix, model, effort,
                                        &request, &count_request, source_hash,
                                        &source_bytes, request_hash,
                                        &request_bytes, &source_seq,
                                        error, error_size);
    if (build_rc == 1) {
        if (!active_prefix && strcmp(reason, "manual") == 0 &&
            snj_render_host(&app->render,
                            "compaction skipped; no new context since the previous compact output") < 0)
            return -1;
        return 0;
    }
    if (build_rc < 0)
        goto out;
    if (source_bytes == 0u || source_bytes > (size_t)INT64_MAX ||
        request_bytes == 0u) {
        snprintf(error, error_size, "compact request has invalid bounds");
        errno = EINVAL;
        goto out;
    }
    input_tokens_bound = (uint64_t)source_bytes;
    if (hash_json_bounded(count_request, SNJ_CONTEXT_MAX_COMPACT,
                          count_request_hash, &count_request_bytes,
                          error, error_size) < 0 || count_request_bytes == 0u) {
        snprintf(error, error_size, "compact count request exceeds 12 MiB");
        goto out;
    }
#ifndef SNAJPAGENT_TEST_FIXTURE
    if (!credential) {
        if (snj_credential_read(&owned_credential,
                                app->config->provider_api_key_env,
                                error, error_size) < 0)
            goto out;
        credential = &owned_credential;
    }
    if (app->config->provider_exact_token_count) {
        if (snj_app_provider_count(app, count_request, credential,
                                   &input_tokens_bound, error, error_size) != 0)
            goto out;
        count_method = "exact";
    }
#endif
    if (input_tokens_bound == 0u || input_tokens_bound > (uint64_t)INT64_MAX) {
        snprintf(error, error_size, "compact input-token bound is invalid");
        errno = EOVERFLOW;
        goto out;
    }
    if (!active_prefix && strcmp(reason, "automatic") == 0 &&
        app->config->auto_compact_input_tokens != 0u &&
        input_tokens_bound < app->config->auto_compact_input_tokens) {
        rc = 0;
        goto out;
    }
    if (snj_random_id(compact_id) < 0) {
        snprintf(error, error_size, "cryptographic compact id generation failed");
        goto out;
    }
    if (commit_rendered(app, "compaction_started",
            compaction_started_data(&app->session, model, compact_id, reason,
                                    count_method, source_seq, source_hash,
                                    request_hash, count_request_hash,
                                    input_tokens_bound),
            error, error_size) < 0)
        goto out;
    started = true;
    if (app->config->provider_native_compaction) {
        if (snj_app_provider_compact(app, request, credential, &output,
                                     &output_tokens_bound,
                                     error, error_size) != 0)
            goto out;
    } else {
        if (run_responses_compaction(app, request, credential, model, effort,
                                     &output, &output_tokens_bound,
                                     error, error_size) != 0)
            goto out;
    }
    if (snj_context_compact_output_valid(output, output_hash, &output_bytes,
                                         error, error_size) < 0)
        goto out;
    output_tokens_bound = (uint64_t)output_bytes;
    if (snj_context_compact_output_count_request_build(output, model,
            &output_count_request, output_count_request_hash,
            &output_count_request_bytes, error, error_size) < 0 ||
        output_count_request_bytes == 0u)
        goto out;
#ifndef SNAJPAGENT_TEST_FIXTURE
    if (app->config->provider_exact_token_count) {
        if (snj_app_provider_count(app, output_count_request, credential,
                                   &output_tokens_bound, error, error_size) != 0)
            goto out;
        output_count_method = "exact";
    }
#endif
    if (output_tokens_bound > (uint64_t)INT64_MAX) {
        snprintf(error, error_size, "compact output bound is too large");
        errno = EOVERFLOW;
        goto out;
    }
    if (commit_rendered(app, "compaction_completed",
            compaction_completed_data(compact_id, source_hash, output_hash,
                                      count_method, output_count_method,
                                      output_count_request_hash,
                                      input_tokens_bound, output_tokens_bound,
                                      output),
            error, error_size) < 0)
        goto out;
    started = false;
    if (!active_prefix && strcmp(reason, "manual") == 0 &&
        snj_render_host(&app->render,
                        "compaction completed and installed for future turns") < 0)
        goto out;
    if (compacted)
        *compacted = true;
    rc = 0;
out:
    if (rc < 0 && started)
        clear_active_compaction(&app->session);
    if (output)
        json_decref(output);
    if (output_count_request)
        json_decref(output_count_request);
    if (count_request)
        json_decref(count_request);
    if (request)
        json_decref(request);
    snj_credential_clear(&owned_credential);
    return rc;
}

int
snj_app_compact_idle_command(struct app_state *app, const char *reason,
                             char *error, size_t error_size)
{
    return run_compaction(app, reason, false, NULL, NULL, error, error_size);
}

int
snj_app_compact_after_turn(struct app_state *app, uint64_t input_tokens_bound,
                           const char *count_method,
                           char *error, size_t error_size)
{
    if (!app || !app->config || !count_method_valid(count_method)) {
        snprintf(error, error_size, "invalid automatic compaction state");
        errno = EINVAL;
        return -1;
    }
    if (app->config->auto_compact_input_tokens == 0u)
        return 0;
    if (input_tokens_bound < app->config->auto_compact_input_tokens)
        return 0;
    return snj_app_compact_idle_command(app, "automatic", error, error_size);
}

int
snj_app_compact_before_response(struct app_state *app,
                                const struct snj_credential *credential,
                                uint64_t input_tokens_bound,
                                const char *count_method, bool *compacted,
                                char *error, size_t error_size)
{
    if (compacted)
        *compacted = false;
    if (!app || !app->config || !compacted || !count_method_valid(count_method)) {
        snprintf(error, error_size, "invalid pre-response compaction state");
        errno = EINVAL;
        return -1;
    }
    if (app->config->auto_compact_input_tokens == 0u)
        return 0;
    if (input_tokens_bound < app->config->auto_compact_input_tokens)
        return 0;
    return run_compaction(app, "automatic", true, credential, compacted,
                          error, error_size);
}
