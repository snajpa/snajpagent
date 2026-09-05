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
                  char hash[SNAG_SHA256_HEX_LEN + 1u], size_t *bytes,
                  char *error, size_t error_size)
{
    struct snag_buf encoded;
    int rc = -1;

    if (hash)
        hash[0] = '\0';
    if (bytes)
        *bytes = 0u;
    snag_buf_init(&encoded, max);
    if (snag_json_canonical(value, &encoded) < 0) {
        snprintf(error, error_size, "canonical compaction JSON exceeds bound");
        goto out;
    }
    if (hash)
        snag_sha256_hex(encoded.data, encoded.len, hash);
    if (bytes)
        *bytes = encoded.len;
    rc = 0;
out:
    snag_buf_free(&encoded);
    return rc;
}

static bool
count_method_valid(const char *method)
{
    return method && (strcmp(method, "exact") == 0 ||
                      strcmp(method, "anchored_upper_bound") == 0 ||
                      strcmp(method, "statistical_upper_estimate") == 0 ||
                      strcmp(method, "qualified_upper_bound") == 0);
}

static bool
active_reason(const char *reason)
{
    return reason && (strcmp(reason, "proactive") == 0 ||
                      strcmp(reason, "hard_budget") == 0 ||
                      strcmp(reason, "provider_rejection") == 0);
}

static json_t *
compaction_started_data(const struct snag_session *session,
                        const char *model, const char *compact_id,
                        const char *reason, const char *count_method,
                        uint64_t source_seq, const char *source_hash,
                        const char *request_hash,
                        const char *count_request_hash,
                        uint64_t input_tokens_bound)
{
    json_t *data = json_object();

    if (!data ||
        snag_json_set_new(data, "capability_version",
                         json_string(SNAJPAGENT_CAPABILITY_VERSION)) < 0 ||
        snag_json_set_new(data, "compact_id", json_string(compact_id)) < 0 ||
        snag_json_set_new(data, "count_method", json_string(count_method)) < 0 ||
        snag_json_set_new(data, "count_request_sha256",
                         json_string(count_request_hash)) < 0 ||
        snag_json_set_new(data, "input_tokens_bound",
                         json_integer((json_int_t)input_tokens_bound)) < 0 ||
        snag_json_set_new(data, "model", json_string(model)) < 0 ||
        snag_json_set_new(data, "predecessor_compact_id",
                         session->compact_id[0] ?
                         json_string(session->compact_id) : json_null()) < 0 ||
        snag_json_set_new(data, "profile_id",
                         json_string(SNAJPAGENT_PROFILE_ID)) < 0 ||
        snag_json_set_new(data, "reason", json_string(reason)) < 0 ||
        snag_json_set_new(data, "request_sha256",
                         json_string(request_hash)) < 0 ||
        snag_json_set_new(data, "source_seq",
                         json_integer((json_int_t)source_seq)) < 0 ||
        snag_json_set_new(data, "source_sha256",
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
        snag_json_set_new(data, "compact_id", json_string(compact_id)) < 0 ||
        snag_json_set_new(data, "count_method", json_string(count_method)) < 0 ||
        snag_json_set_new(data, "input_tokens_bound",
                         json_integer((json_int_t)input_tokens_bound)) < 0 ||
        snag_json_set_new(data, "output", json_deep_copy(output)) < 0 ||
        snag_json_set_new(data, "output_count_method",
                         json_string(output_count_method)) < 0 ||
        snag_json_set_new(data, "output_count_request_sha256",
                         json_string(output_count_request_hash)) < 0 ||
        snag_json_set_new(data, "output_sha256", json_string(output_hash)) < 0 ||
        snag_json_set_new(data, "output_tokens_bound",
                         json_integer((json_int_t)output_tokens_bound)) < 0 ||
        snag_json_set_new(data, "source_sha256",
                         json_string(source_hash)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
}

static json_t *
compaction_interrupted_data(const char *compact_id, const char *reason)
{
    json_t *data = json_object();

    if (!data ||
        snag_json_set_new(data, "compact_id", json_string(compact_id)) < 0 ||
        snag_json_set_new(data, "reason", json_string(reason)) < 0) {
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
    if (snag_session_commit(&app->session, type, data, &seq,
                           error, error_size) < 0)
        return -1;
    if (snag_ui_event(&app->ui, seq, type) < 0) {
        snprintf(error, error_size, "durable compaction event output failed");
        return -1;
    }
    return 0;
}

static void
clear_active_compaction(struct snag_session *session)
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
        (strcmp(reason, "manual") != 0 && !active_reason(reason))) {
        snprintf(error, error_size, "invalid compaction reason");
        errno = EINVAL;
        return -1;
    }
    if (active_prefix) {
        if (!active_reason(reason) || !app->session.active_turn ||
            app->session.response_open ||
            app->session.pending_call_count ||
            app->session.active_compact_id[0] != '\0') {
            snprintf(error, error_size,
                     "pre-response compaction requires an active turn before response");
            errno = EINVAL;
            return -1;
        }
        return 0;
    }
    if (app->session.active_turn || app->session.response_open ||
        app->session.process_count ||
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
                         uint64_t source_budget, bool allow_oversized_first,
                         json_t **request, json_t **count_request,
                         char source_hash[SNAG_SHA256_HEX_LEN + 1u],
                         size_t *source_bytes,
                         char request_hash[SNAG_SHA256_HEX_LEN + 1u],
                         size_t *request_bytes, uint64_t *source_seq,
                         char *error, size_t error_size)
{
    if (active_prefix)
        return snag_context_compact_active_prefix_request_build(&app->session,
            model, effort, source_budget, allow_oversized_first,
            request, count_request,
            source_hash, source_bytes,
            request_hash, request_bytes, source_seq, error, error_size);
    return snag_context_compact_request_build(&app->session, model, effort,
        source_budget, allow_oversized_first, request, count_request,
        source_hash, source_bytes,
        request_hash,
        request_bytes, source_seq, error, error_size);
}

static json_t *
responses_compact_create_request(const json_t *compact_request,
                                 const char *model, const char *effort,
                                 const struct snag_model_capacity *capacity)
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
        !model || !effort || !capacity)
        goto fail;
    input_copy = json_deep_copy(input);
    if (!input_copy ||
        snag_json_set_new(compact_instruction, "content",
                         json_string(instruction)) < 0 ||
        snag_json_set_new(compact_instruction, "role",
                         json_string("developer")) < 0 ||
        json_array_append_new(input_copy, compact_instruction) < 0)
        goto fail;
    compact_instruction = NULL;
    if (snag_json_set_new(request, "input", input_copy) < 0)
        goto fail;
    input_copy = NULL;
    if (snag_json_set_new(request, "model", json_string(model)) < 0 ||
        snag_json_set_new(request, "parallel_tool_calls", json_false()) < 0 ||
        snag_json_set_new(request, "reasoning", snag_context_reasoning_settings(effort)) < 0 ||
        snag_json_set_new(request, "store", json_false()) < 0 ||
        snag_json_set_new(request, "stream", json_true()) < 0 ||
        snag_json_set_new(request, "tool_choice", json_string("none")) < 0 ||
        snag_json_set_new(request, "tools", json_array()) < 0 ||
        snag_json_set_new(request, "truncation", json_string("disabled")) < 0)
        goto fail;
    if (capacity->max_output_known &&
        snag_json_set_new(request, "max_output_tokens",
            json_integer((json_int_t)capacity->max_output_tokens)) < 0)
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

static json_t *
responses_compact_count_request(const json_t *compact_request,
                                const char *model, const char *effort,
                                const struct snag_model_capacity *capacity)
{
    json_t *request = responses_compact_create_request(compact_request,
                                                        model, effort,
                                                        capacity);

    if (!request)
        return NULL;
    if (json_object_del(request, "store") < 0 ||
        json_object_del(request, "stream") < 0 ||
        (json_object_get(request, "max_output_tokens") &&
         json_object_del(request, "max_output_tokens") < 0)) {
        json_decref(request);
        return NULL;
    }
    return request;
}

static int
run_responses_compaction(struct app_state *app, const json_t *create_request,
                         const struct snag_credential *credential,
                         json_t **output, uint64_t *output_tokens_bound,
                         char *error, size_t error_size)
{
    char output_hash[SNAG_SHA256_HEX_LEN + 1u];
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
    (void)create_request;
    (void)credential;
    if (!fixture_output || !item ||
        snag_json_set_new(item, "content",
                         json_string("fixture responses compact summary")) < 0 ||
        snag_json_set_new(item, "role", json_string("developer")) < 0 ||
        snag_json_set_new(item, "type", json_string("message")) < 0 ||
        json_array_append_new(fixture_output, item) < 0) {
        if (item)
            json_decref(item);
        if (fixture_output)
            json_decref(fixture_output);
        return -1;
    }
    item = NULL;
    if (snag_context_compact_output_valid(fixture_output, output_hash,
                                         &output_bytes, error, error_size) < 0) {
        json_decref(fixture_output);
        return -1;
    }
    *output = fixture_output;
    *output_tokens_bound = (uint64_t)output_bytes;
    return 0;
#else
    struct snag_response_graph graph;
    struct snag_graph_decision decision;
    int cancel_code = 0;
    int provider_rc;
    int rc = -1;

    snag_response_graph_init(&graph);
    provider_rc = snag_provider_responses_create(
        create_request, app->config, app->turn_provider, credential,
        &app->ui, NULL, NULL, snag_app_active_input_pump, app, &graph,
        NULL, error, error_size, &cancel_code, NULL);
    if (provider_rc != 0) {
        rc = provider_rc;
        goto out;
    }
    if (snag_response_graph_classify(&graph, &decision,
                                    error, error_size) < 0)
        goto out;
    if (decision.outcome != SNAG_GRAPH_FINAL ||
        decision.final_index >= graph.count ||
        !graph.items[decision.final_index].text) {
        snprintf(error, error_size,
                 "Responses compaction did not return a final JSON answer");
        errno = EPROTO;
        goto out;
    }
    *output = snag_json_load_strict(
        (const unsigned char *)graph.items[decision.final_index].text,
        strlen(graph.items[decision.final_index].text),
        SNAG_CONTEXT_MAX_COMPACT, error, error_size);
    if (!*output)
        goto out;
    if (snag_context_compact_output_valid(*output, output_hash, &output_bytes,
                                         error, error_size) < 0) {
        json_decref(*output);
        *output = NULL;
        goto out;
    }
    *output_tokens_bound = (uint64_t)output_bytes;
    rc = 0;
out:
    snag_response_graph_free(&graph);
    return rc;
#endif
}

static int
run_compaction_attempt(struct app_state *app, const char *reason, bool active_prefix,
               bool allow_native,
               const struct snag_credential *provided_credential,
               bool *compacted, char *error, size_t error_size)
{
    struct snag_credential owned_credential;
    const struct snag_credential *credential = provided_credential;
    json_t *request = NULL;
    json_t *provider_request = NULL;
    json_t *count_request = NULL;
    json_t *output_count_request = NULL;
    json_t *output = NULL;
    char compact_id[SNAG_ID_HEX_LEN + 1u];
    char source_hash[SNAG_SHA256_HEX_LEN + 1u];
    char request_hash[SNAG_SHA256_HEX_LEN + 1u];
    char count_request_hash[SNAG_SHA256_HEX_LEN + 1u];
    char output_hash[SNAG_SHA256_HEX_LEN + 1u];
    char output_count_request_hash[SNAG_SHA256_HEX_LEN + 1u];
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
    uint64_t source_budget;
    uint64_t threshold;
    bool use_exact;
    bool started = false;
    bool native;
    int build_rc;
    int stage_rc;
    int rc = -1;

    snag_credential_clear(&owned_credential);
    if (compacted)
        *compacted = false;
    if (compaction_state_valid(app, reason, active_prefix,
                               error, error_size) < 0)
        return -1;
    model = active_prefix && app->turn_model ? app->turn_model :
                                               app->session.default_model;
    effort = active_prefix && app->turn_effort ? app->turn_effort :
                                                 app->session.default_effort;
    if (!active_prefix)
        app->turn_provider = snag_config_provider(
            app->config, app->session.default_provider[0] ?
                         app->session.default_provider : NULL);
    if (!app->turn_provider) {
        snprintf(error, error_size,
                 "selected provider is not present in the current configuration");
        errno = ENOENT;
        goto out;
    }
    native = allow_native && app->turn_provider->native_compaction;
    if (!active_prefix &&
        snag_app_capacity_resolve(app, app->turn_provider, model,
                                 &app->turn_capacity,
                                 error, error_size) < 0)
        goto out;
    threshold = snag_model_compact_threshold(app->turn_provider,
                                           &app->turn_capacity);
    if (strcmp(reason, "proactive") == 0 && !threshold) {
        rc = 0;
        goto out;
    }
#ifndef SNAJPAGENT_TEST_FIXTURE
    if (!credential) {
        if (snag_auth_read(app->store.root_fd, app->turn_provider, false, NULL,
                          &owned_credential, snag_app_active_input_pump, app,
                          error, error_size) < 0)
            goto out;
        credential = &owned_credential;
    }
#endif
    use_exact = snag_app_exact_count_enabled(
        app->turn_provider->exact_token_count,
        app->turn_capacity.count_capability);
    source_budget = app->turn_capacity.hard_input_known && !use_exact &&
        !app->turn_capacity.observed_tokens_per_million_bytes ?
        app->turn_capacity.hard_input_tokens : SNAG_CONTEXT_MAX_COMPACT - 4096u;
    if (source_budget > SNAG_CONTEXT_MAX_COMPACT - 4096u)
        source_budget = SNAG_CONTEXT_MAX_COMPACT - 4096u;
    for (unsigned int selection = 0u; selection < 8u; ++selection) {
        build_rc = build_compaction_request(app, active_prefix, model, effort,
                                            source_budget,
                                            use_exact,
                                            &request, &count_request,
                                            source_hash, &source_bytes,
                                            request_hash, &request_bytes,
                                            &source_seq, error, error_size);
        if (build_rc == 1) {
            if (selection != 0u) {
                snprintf(error, error_size,
                         "no complete history prefix fits the hard context budget");
                errno = EOVERFLOW;
                goto out;
            }
            if (!active_prefix && strcmp(reason, "manual") == 0 &&
                snag_ui_text(&app->ui, SNAG_UI_HOST,
                    "compaction skipped; no new context since the previous compact output") < 0)
                goto out;
            rc = 0;
            goto out;
        }
        if (build_rc < 0)
            goto out;
        if (source_bytes == 0u || source_bytes > (size_t)INT64_MAX) {
            snprintf(error, error_size, "compact source has invalid bounds");
            errno = EINVAL;
            goto out;
        }
        if (native) {
            provider_request = json_incref(request);
        } else {
            provider_request = responses_compact_create_request(
                request, model, effort, &app->turn_capacity);
            json_decref(count_request);
            count_request = responses_compact_count_request(
                request, model, effort, &app->turn_capacity);
        }
        if (provider_request && app->turn_provider->auth == SNAG_AUTH_CHATGPT &&
            native &&
            snag_json_set_new(provider_request, "instructions", json_string("")) < 0)
            goto out;
        if (provider_request && !native && app->turn_provider->auth == SNAG_AUTH_CHATGPT &&
            snag_context_codex_request(provider_request) < 0)
            goto out;
        if (!provider_request || !count_request) {
            snprintf(error, error_size,
                     "cannot build bounded compaction provider request");
            errno = ENOMEM;
            goto out;
        }
        if (hash_json_bounded(provider_request, SNAG_CONTEXT_MAX_COMPACT,
                              request_hash, &request_bytes,
                              error, error_size) < 0 || request_bytes == 0u ||
            hash_json_bounded(count_request, SNAG_CONTEXT_MAX_COMPACT,
                              count_request_hash, &count_request_bytes,
                              error, error_size) < 0 ||
            count_request_bytes == 0u) {
            snprintf(error, error_size,
                     "compaction provider request exceeds 12 MiB");
            goto out;
        }
        input_tokens_bound = snag_context_input_estimate((uint64_t)count_request_bytes,
            app->turn_capacity.observed_tokens_per_million_bytes);
        count_method = app->turn_capacity.observed_tokens_per_million_bytes ?
            "statistical_upper_estimate" : "qualified_upper_bound";
        if (use_exact) {
            stage_rc = snag_app_provider_count(app, count_request, credential, 0u,
                &input_tokens_bound, &count_method, error, error_size);
            if (stage_rc != 0 && stage_rc != SNAG_APP_COUNT_SKIPPED) {
                rc = stage_rc;
                goto out;
            }
            if (stage_rc == SNAG_APP_COUNT_SKIPPED) {
                use_exact = false;
            }
        }
        if (input_tokens_bound == 0u ||
            input_tokens_bound > (uint64_t)INT64_MAX) {
            snprintf(error, error_size,
                     "compact input-token bound is invalid");
            errno = EOVERFLOW;
            goto out;
        }
        if (!app->turn_capacity.hard_input_known ||
            input_tokens_bound <= app->turn_capacity.hard_input_tokens)
            break;
        if (selection == 7u || source_bytes <= 1u) {
            snprintf(error, error_size,
                     "compaction input estimate %llu (%s) cannot fit hard budget %llu",
                     (unsigned long long)input_tokens_bound, count_method,
                     (unsigned long long)app->turn_capacity.hard_input_tokens);
            errno = EOVERFLOW;
            goto out;
        }
        if (strcmp(count_method, "qualified_upper_bound") != 0) {
            uint64_t scaled;
            if ((uint64_t)source_bytes > UINT64_MAX /
                    app->turn_capacity.hard_input_tokens)
                scaled = (uint64_t)source_bytes / 2u;
            else
                scaled = (uint64_t)source_bytes *
                    app->turn_capacity.hard_input_tokens /
                    input_tokens_bound;
            source_budget = scaled - scaled / 10u;
        } else {
            uint64_t envelope = count_request_bytes > source_bytes ?
                (uint64_t)(count_request_bytes - source_bytes) : 0u;
            source_budget = app->turn_capacity.hard_input_tokens > envelope ?
                app->turn_capacity.hard_input_tokens - envelope : 0u;
        }
        if (source_budget >= (uint64_t)source_bytes)
            source_budget = (uint64_t)source_bytes - 1u;
        if (!source_budget)
            source_budget = 1u; /* Zero means unbounded to the source walker. */
        json_decref(provider_request);
        provider_request = NULL;
        json_decref(count_request);
        count_request = NULL;
        json_decref(request);
        request = NULL;
    }
    if (!active_prefix && strcmp(reason, "proactive") == 0 &&
        input_tokens_bound < threshold) {
        rc = 0;
        goto out;
    }
    if (snag_random_id(compact_id) < 0) {
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
    if (native) {
        stage_rc = snag_app_provider_compact(app, provider_request, credential,
                                            &output,
                                            &output_tokens_bound,
                                            error, error_size);
        if (stage_rc == SNAG_PROVIDER_UNSUPPORTED) {
            if (commit_rendered(app, "compaction_interrupted",
                    compaction_interrupted_data(compact_id, "endpoint_unavailable"),
                    error, error_size) < 0)
                goto out;
            started = false;
        }
        if (stage_rc != 0) {
            rc = stage_rc;
            goto out;
        }
    } else {
        stage_rc = run_responses_compaction(app, provider_request, credential,
                                            &output,
                                            &output_tokens_bound,
                                            error, error_size);
        if (stage_rc != 0) {
            rc = stage_rc;
            goto out;
        }
    }
    if (snag_context_compact_output_valid(output, output_hash, &output_bytes,
                                         error, error_size) < 0)
        goto out;
    output_tokens_bound = (uint64_t)output_bytes;
    if (snag_context_compact_output_count_request_build(output, model,
            &output_count_request, output_count_request_hash,
            &output_count_request_bytes, error, error_size) < 0 ||
        output_count_request_bytes == 0u)
        goto out;
    if (use_exact) {
        stage_rc = snag_app_provider_count(app, output_count_request, credential,
            0u, &output_tokens_bound, &output_count_method, error, error_size);
        if (stage_rc != 0 && stage_rc != SNAG_APP_COUNT_SKIPPED) {
            rc = stage_rc;
            goto out;
        }
        if (stage_rc == SNAG_APP_COUNT_SKIPPED)
            output_tokens_bound = (uint64_t)output_bytes;
    }
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
    if (app->networked &&
        snag_app_irc_snapshot(app, "compaction", error, error_size) < 0)
        goto out;
    started = false;
    if (compacted)
        *compacted = true;
    rc = 0;
out:
    if ((rc == 1 || rc == 2) && started) {
        if (commit_rendered(app, "compaction_interrupted",
                compaction_interrupted_data(compact_id,
                    rc == 1 ? "steering" : "user"),
                error, error_size) < 0)
            rc = -1;
        else
            started = false;
    }
    if (rc < 0 && started)
        clear_active_compaction(&app->session);
    if (output)
        json_decref(output);
    if (output_count_request)
        json_decref(output_count_request);
    if (count_request)
        json_decref(count_request);
    if (provider_request)
        json_decref(provider_request);
    if (request)
        json_decref(request);
    snag_credential_clear(&owned_credential);
    return rc;
}

static int
run_compaction(struct app_state *app, const char *reason, bool active_prefix,
               const struct snag_credential *credential, bool *compacted,
               char *error, size_t error_size)
{
    int rc = run_compaction_attempt(app, reason, active_prefix, true,
                                    credential, compacted, error, error_size);
    if (rc != SNAG_PROVIDER_UNSUPPORTED)
        return rc;
    if (snag_ui_text(&app->ui, SNAG_UI_WARNING,
        "native compaction unavailable; compacting through Responses") < 0)
        return -1;
    if (error_size)
        error[0] = '\0';
    return run_compaction_attempt(app, reason, active_prefix, false,
                                  credential, compacted, error, error_size);
}

int
snag_app_compact_idle_command(struct app_state *app, const char *reason,
                             char *error, size_t error_size)
{
    return run_compaction(app, reason, false, NULL, NULL, error, error_size);
}

int
snag_app_compact_after_turn(struct app_state *app, uint64_t input_tokens_bound,
                           const char *count_method,
                           char *error, size_t error_size)
{
    uint64_t threshold;

    if (!app || !app->config || !app->turn_provider ||
        !count_method_valid(count_method)) {
        snprintf(error, error_size, "invalid proactive compaction state");
        errno = EINVAL;
        return -1;
    }
    threshold = snag_model_compact_threshold(app->turn_provider,
                                           &app->turn_capacity);
    if (!threshold || input_tokens_bound < threshold)
        return 0;
    return snag_app_compact_idle_command(app, "proactive", error, error_size);
}

int
snag_app_compact_before_response(struct app_state *app,
                                const struct snag_credential *credential,
                                uint64_t input_tokens_bound,
                                const char *count_method, bool *compacted,
                                char *error, size_t error_size)
{
    if (compacted)
        *compacted = false;
    if (!app || !app->config || !app->turn_provider || !compacted ||
        !count_method_valid(count_method)) {
        snprintf(error, error_size, "invalid pre-response compaction state");
        errno = EINVAL;
        return -1;
    }
    {
        uint64_t threshold = snag_model_compact_threshold(app->turn_provider,
                                                        &app->turn_capacity);
        bool over_hard = app->turn_capacity.hard_input_known &&
            input_tokens_bound > app->turn_capacity.hard_input_tokens;
        bool over_proactive = threshold && input_tokens_bound >= threshold;
        int rc;

        if (!over_hard && !over_proactive)
            return 0;
        rc = run_compaction(app, over_hard ? "hard_budget" : "proactive",
                            true, credential, compacted, error, error_size);
        if (rc != 0)
            return rc;
        if (over_hard && !*compacted &&
            strcmp(count_method, "statistical_upper_estimate") != 0 &&
            strcmp(count_method, "qualified_upper_bound") != 0) {
            snprintf(error, error_size,
                     "context input estimate %llu (%s) exceeds hard budget %llu; no complete older turn can be compacted",
                     (unsigned long long)input_tokens_bound, count_method,
                     (unsigned long long)app->turn_capacity.hard_input_tokens);
            errno = EOVERFLOW;
            return -1;
        }
        return 0;
    }
}

int
snag_app_compact_after_capacity_rejection(
    struct app_state *app, const struct snag_credential *credential,
    bool *compacted, char *error, size_t error_size)
{
    if (compacted)
        *compacted = false;
    if (!app || !credential || !compacted) {
        snprintf(error, error_size,
                 "invalid provider-rejection compaction state");
        errno = EINVAL;
        return -1;
    }
    return run_compaction(app, "provider_rejection", true, credential,
                          compacted, error, error_size);
}
