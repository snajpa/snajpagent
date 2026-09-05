/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"
#include "provider.h"
#include "context.h"
#include "json.h"
#include "tools.h"
#include "wire.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*fixture_emit_fn)(void *opaque, size_t item_index,
                               enum snj_item_kind kind,
                               enum snj_item_phase phase,
                               const char *text, size_t len);
typedef int (*fixture_pump_fn)(void *opaque, unsigned int timeout_ms);
#ifdef SNAJPAGENT_TEST_FIXTURE
static bool fixture_capacity_rejected_once;

static json_t *
fixture_model_limits(size_t index)
{
    json_t *limits = json_object();

    if (!limits ||
        snj_json_set_new(limits, "auto_compact_input_tokens", json_null()) < 0 ||
        snj_json_set_new(limits, "context_window_tokens",
                         index < 2u ? json_integer(272000) : json_null()) < 0 ||
        snj_json_set_new(limits, "effective_context_window_percent",
                         json_null()) < 0 ||
        snj_json_set_new(limits, "input_context_window_tokens", json_null()) < 0 ||
        snj_json_set_new(limits, "max_context_window_tokens",
                         index < 2u ? json_integer(872000) : json_null()) < 0 ||
        snj_json_set_new(limits, "max_input_tokens", json_null()) < 0 ||
        snj_json_set_new(limits, "max_output_tokens", json_null()) < 0) {
        if (limits)
            json_decref(limits);
        return NULL;
    }
    return limits;
}

int snj_fixture_response(const char *prompt, const json_t *steering,
                         const char *workspace, unsigned int cycle,
                         const char *goal_prompt, uint64_t goal_turn_count,
                         fixture_emit_fn emit, fixture_pump_fn pump, void *opaque,
                         struct snj_response_graph *graph,
                         struct snj_provider_failure *failure,
                         char *error, size_t error_size);
int snj_fixture_tool(const struct snj_response_item *call,
                     fixture_pump_fn pump, void *pump_opaque,
                     json_t **result, char *error, size_t error_size);
#endif

enum snj_managed_continuation
snj_app_managed_continuation_classify(const struct app_state *app,
                                      const struct snj_response_graph *graph,
                                      const struct snj_graph_decision *decision)
{
    const char *handle = app->session.active_process_handle;
    bool continuation = false;
    bool mismatch = false;

    if (!handle[0])
        return SNJ_MANAGED_CONTINUATION_NONE;
    if (decision->outcome != SNJ_GRAPH_CALLS)
        return SNJ_MANAGED_CONTINUATION_ORDERING_VIOLATION;
    for (size_t i = 0; i < graph->count; ++i) {
        const struct snj_response_item *call = &graph->items[i];
        const char *arg_handle;

        if (call->kind != SNJ_ITEM_TOOL_CALL)
            continue;
        if (call->name && strcmp(call->name, "write_stdin") == 0) {
            if (continuation)
                return SNJ_MANAGED_CONTINUATION_ORDERING_VIOLATION;
            continuation = true;
            arg_handle = snj_json_string(call->arguments, "handle");
            mismatch = !arg_handle || strcmp(arg_handle, handle) != 0;
            continue;
        }
        if (continuation || !app->networked || !call->name ||
            (strcmp(call->name, "irc_send") != 0 &&
             strcmp(call->name, "irc_state") != 0 &&
             strcmp(call->name, "irc_topic") != 0))
            return SNJ_MANAGED_CONTINUATION_ORDERING_VIOLATION;
    }
    if (!continuation)
        return SNJ_MANAGED_CONTINUATION_ORDERING_VIOLATION;
    return mismatch ? SNJ_MANAGED_CONTINUATION_HANDLE_MISMATCH :
                      SNJ_MANAGED_CONTINUATION_MATCHED;
}

int
snj_app_provider_models(struct app_state *app,
                        const struct snj_provider_config *provider,
                        json_t **models, char *error, size_t error_size)
{
#ifdef SNAJPAGENT_TEST_FIXTURE
    static const char *const ids[] = {
        "gpt-5.6-luna", "gpt-5.6-terra", "vendor/future-model"
    };
    static const char *const efforts[] = {
        "low", "medium", "high", "xhigh", "max", "ultra"
    };
    const char *failure = getenv("SNAJPAGENT_FIXTURE_MODEL_FAILURE");
    json_t *out = NULL;

    (void)app;
    if (models)
        *models = NULL;
    if (failure && strcmp(failure, provider->name) == 0) {
        if (error_size)
            (void)snprintf(error, error_size,
                           "fixture model discovery failed");
        errno = EIO;
        return -1;
    }
    out = json_array();
    if (!models || !out)
        goto fail;
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        json_t *entry = json_object();
        json_t *variants = json_array();
        if (!entry || !variants)
            goto fail_entry;
        if (i == 0u) {
            if (json_array_append_new(variants, json_string("high")) < 0)
                goto fail_entry;
        } else if (i == 1u) {
            for (size_t j = 0; j < sizeof(efforts) / sizeof(efforts[0]); ++j)
                if (json_array_append_new(variants,
                                          json_string(efforts[j])) < 0)
                    goto fail_entry;
        }
        if (snj_json_set_new(entry, "default_effort",
                             i == 0u ? json_string("high") :
                             i == 1u ? json_string("medium") : json_null()) < 0)
            goto fail_entry;
        if (snj_json_set_new(entry, "efforts", variants) < 0) {
            variants = NULL;
            goto fail_entry;
        }
        variants = NULL;
        if (snj_json_set_new(entry, "id", json_string(ids[i])) < 0 ||
            snj_json_set_new(entry, "limits", fixture_model_limits(i)) < 0)
            goto fail_entry;
        if (json_array_append_new(out, entry) < 0) {
            entry = NULL;
            goto fail_entry;
        }
        entry = NULL;
        continue;
fail_entry:
        if (variants)
            json_decref(variants);
        if (entry)
            json_decref(entry);
        goto fail;
    }
    *models = out;
    return 0;
fail:
    if (out)
        json_decref(out);
    errno = ENOMEM;
    return -1;
#else
    struct snj_credential credential;
    int rc;
    snj_credential_clear(&credential);
    if (snj_credential_read(&credential, provider->api_key_env,
                            error, error_size) < 0)
        return -1;
    rc = snj_provider_models_list(app->config, provider, &credential,
                                  &app->ui, snj_app_active_input_pump, app,
                                  models, error, error_size);
    snj_credential_clear(&credential);
    return rc;
#endif
}

bool
snj_app_exact_count_enabled(enum snj_token_count_mode mode,
                            enum snj_count_capability capability)
{
    return mode == SNJ_TOKEN_COUNT_STRICT ||
        (mode == SNJ_TOKEN_COUNT_AUTO &&
         capability != SNJ_COUNT_UNSUPPORTED);
}

int
snj_app_provider_count(struct app_state *app, const json_t *count_request,
                       const struct snj_credential *credential,
                       uint64_t model_input_bytes, uint64_t *input_tokens,
                       const char **count_method,
                       char *error, size_t error_size)
{
#ifdef SNAJPAGENT_TEST_FIXTURE
    (void)credential;
    (void)input_tokens;
    (void)model_input_bytes;
    if (app->turn_provider->exact_token_count == SNJ_TOKEN_COUNT_STRICT)
        *count_method = "exact";
    if (app->session.last_user &&
        strcmp(app->session.last_user, "compact_budget") == 0) {
        *input_tokens = 90000u;
        *count_method = "exact";
    }
    {
        struct snj_buf encoded;
        bool wait_for_mention;

        snj_buf_init(&encoded, SNJ_WIRE_BODY_MAX);
        if (snj_json_canonical(count_request, &encoded) < 0 ||
            snj_buf_terminate(&encoded) < 0) {
            if (error_size)
                (void)snprintf(error, error_size,
                               "fixture count request could not be encoded");
            snj_buf_free(&encoded);
            return -1;
        }
        wait_for_mention = strstr((const char *)encoded.data,
                                  "network_count_wait") != NULL &&
            strstr((const char *)encoded.data, "network count mention") == NULL;
        snj_buf_free(&encoded);
        if (wait_for_mention)
            for (unsigned int i = 0u; i < 100u; ++i) {
                int pump_rc = snj_app_active_input_pump(app, 20u);

                if (pump_rc != 0)
                    return pump_rc;
            }
    }
    return 0;
#else
    bool endpoint_unsupported = false;
    uint64_t sample_bytes;
    int cancel_code = 0;
    int rc;

    if (strcmp(*count_method, "anchored_upper_bound") == 0 ||
        !snj_app_exact_count_enabled(
            app->turn_provider->exact_token_count,
            app->turn_capacity.count_capability))
        return SNJ_APP_COUNT_SKIPPED;
    rc = snj_provider_responses_count(count_request, app->config,
                                      app->turn_provider, credential,
                                      &app->ui,
                                      snj_app_active_input_pump, app,
                                      input_tokens, &endpoint_unsupported,
                                      error, error_size,
                                      &cancel_code, NULL);
    if (rc == 0) {
        *count_method = "exact";
        sample_bytes = *input_tokens ? model_input_bytes : 0u;
        snj_app_record_model_accounting(app, SNJ_COUNT_SUPPORTED,
                                        sample_bytes,
                                        sample_bytes ? *input_tokens : 0u, 0u);
        return 0;
    }
    if (!endpoint_unsupported)
        return rc;
    snj_app_record_model_accounting(app, SNJ_COUNT_UNSUPPORTED, 0u, 0u, 0u);
    if (app->turn_provider->exact_token_count == SNJ_TOKEN_COUNT_STRICT)
        return rc;
    if (error_size)
        error[0] = '\0';
    return SNJ_APP_COUNT_SKIPPED;
#endif
}

int
snj_app_provider_compact(struct app_state *app, const json_t *compact_request,
                         const struct snj_credential *credential,
                         json_t **output, uint64_t *output_tokens_bound,
                         char *error, size_t error_size)
{
#ifdef SNAJPAGENT_TEST_FIXTURE
    json_t *fixture_output = json_array();
    json_t *item = json_object();
    char hash[SNJ_SHA256_HEX_LEN + 1u];
    size_t bytes = 0u;

    (void)compact_request;
    (void)credential;
    if (app && app->session.last_user &&
        (strcmp(app->session.last_user, "compaction_steer") == 0 ||
         strcmp(app->session.last_user, "capacity_recovery_steer") == 0))
        for (unsigned int i = 0u; i < 100u; ++i) {
            int pump_rc = snj_app_active_input_pump(app, 20u);

            if (pump_rc != 0) {
                json_decref(item);
                json_decref(fixture_output);
                return pump_rc;
            }
        }
    if (output)
        *output = NULL;
    if (output_tokens_bound)
        *output_tokens_bound = 0u;
    if (!output || !output_tokens_bound || !fixture_output || !item ||
        snj_json_set_new(item, "encrypted_content",
                         json_string("fixture-native-compact")) < 0 ||
        snj_json_set_new(item, "type", json_string("compaction")) < 0 ||
        json_array_append_new(fixture_output, item) < 0 ||
        snj_context_compact_output_valid(fixture_output, hash, &bytes,
                                         error, error_size) < 0) {
        if (item)
            json_decref(item);
        if (fixture_output)
            json_decref(fixture_output);
        return -1;
    }
    item = NULL;
    *output = fixture_output;
    *output_tokens_bound = (uint64_t)bytes;
    return 0;
#else
    int cancel_code = 0;
    int rc = snj_provider_responses_compact(compact_request, app->config,
                                            app->turn_provider,
                                            credential, &app->ui,
                                            snj_app_active_input_pump, app,
                                            output, output_tokens_bound,
                                            error, error_size, &cancel_code, NULL);
    if (rc == 1 || rc == 2)
        return rc;
    return rc;
#endif
}

int
snj_app_provider_run(struct app_state *app, const char *prompt,
                     const json_t *steering, unsigned int cycle,
                     const json_t *create_request,
                     const struct snj_credential *credential,
                     struct snj_response_graph *graph,
                     struct snj_provider_failure *failure,
                     char *error, size_t error_size,
                     unsigned int *retry_count)
{
#ifdef SNAJPAGENT_TEST_FIXTURE
    (void)credential;
    if (failure)
        memset(failure, 0, sizeof(*failure));
    if (retry_count)
        *retry_count = 0u;
    if (((strcmp(prompt, "capacity_recovery") == 0 ||
          strcmp(prompt, "capacity_recovery_steer") == 0) &&
         !fixture_capacity_rejected_once) ||
        strcmp(prompt, "capacity_recovery_twice") == 0) {
        fixture_capacity_rejected_once = true;
        if (failure) {
            memcpy(failure->code, "context_length_exceeded", 24u);
            memcpy(failure->message, "fixture context rejected", 25u);
            failure->context_limit_tokens = 100000u;
            failure->requested_input_tokens = 90000u;
            failure->context_limit_known = true;
            failure->requested_input_known = true;
        }
        if (error_size)
            (void)snprintf(error, error_size,
                           "fixture context rejected");
        errno = EOVERFLOW;
        return -1;
    }
    {
        json_t *ts = json_object_get(create_request, "tools");
        json_t *input = json_object_get(create_request, "input");
        bool read_only = app->session.active_read_only;

        if (read_only && json_array_size(ts) != 4u)
            return -1;
        for (size_t i = 0; read_only && i < json_array_size(ts); ++i) {
            json_t *tool = json_array_get(ts, i);
            const char *type = snj_json_string(tool, "type");

            if (type && strcmp(type, "web_search") == 0 && json_object_size(tool) == 1u)
                continue;
            if (!type || strcmp(type, "function") != 0 ||
                !snj_read_only_tool(snj_json_string(tool, "name")))
                return -1;
        }
        for (size_t i = 0; i < json_array_size(input); ++i) {
            json_t *message = json_array_get(input, i);
            const char *role = snj_json_string(message, "role");
            json_t *content = json_object_get(message, "content");

            if (!role || strcmp(role, "developer") != 0)
                continue;
            for (size_t j = 0; j < json_array_size(content); ++j) {
                const char *text = snj_json_string(json_array_get(content, j), "text");
                if ((read_only || app->session.active_queued ||
                     app->session.pending_queue_count) && text &&
                    strncmp(text, "Persistent goal ", 16u) == 0) {
                    snj_errorf(error, error_size, "fixture: goal reminder bypassed queued work");
                    return -1;
                }
            }
        }
    }
    return snj_fixture_response(prompt, steering, app->session.workspace, cycle,
                                app->session.goal_prompt,
                                app->session.goal_turn_count,
                                snj_app_stream_public, snj_app_active_input_pump,
                                app, graph, failure, error, error_size);
#else
    int cancel_code = 0;
    (void)prompt;
    (void)steering;
    (void)cycle;
    if (retry_count)
        *retry_count = 0u;
    int rc = snj_provider_responses_create(create_request, app->config,
                                           app->turn_provider, credential,
                                           &app->ui,
                                           snj_app_stream_public_response, app,
                                           snj_app_active_input_pump, app, graph,
                                           failure, error, error_size, &cancel_code,
                                           retry_count);
    if (rc == 1 || rc == 2)
        return rc;
    return rc;
#endif
}

#ifndef SNAJPAGENT_TEST_FIXTURE
static int
tool_input_pump(void *opaque, unsigned int timeout_ms)
{
    struct app_state *app = opaque;
    int rc = snj_app_active_input_pump(opaque, timeout_ms);

    if (rc == 0 && app->networked && app->irc_urgent.len)
        return 1;
    return rc;
}
#endif

int
snj_app_tool_run(struct app_state *app, const struct snj_response_item *call,
                 const struct snj_credential *credential, json_t **result,
                 char *error, size_t error_size)
{
    static const char *const send_keys[] = {"notice", "text"};
    static const char *const topic_keys[] = {"topic"};

    if (app->session.active_read_only) {
        if (call && snj_read_only_tool(call->name))
            return snj_tools_read_only(call, app->session.workspace,
                                       snj_app_active_input_pump, app, result);
        *result = snj_tool_result_terminal(false,
            "Tool unavailable: this turn is read-only; use list_files, read_file or grep.");
        return *result ? 0 : -1;
    }
    if (call && snj_read_only_tool(call->name)) {
        *result = snj_tool_result_terminal(false,
            "Native inspection tools are available only in /ro queries.");
        return *result ? 0 : -1;
    }

    if (call && call->name &&
        (strcmp(call->name, "create_goal") == 0 ||
         strcmp(call->name, "update_goal") == 0))
        return snj_app_goal_tool(app, call, result, error, error_size);
    if (call && call->name && strcmp(call->name, "irc_state") == 0) {
        struct snj_buf state;
        int rc;

        *result = NULL;
        if (!app->irc || !snj_json_exact_keys(call->arguments, NULL, 0u)) {
            *result = snj_tool_result_terminal(false,
                                                "irc_state arguments are invalid");
            return *result ? 0 : -1;
        }
        snj_buf_init(&state, SNJ_MAX_IRC_SNAPSHOT);
        rc = snj_irc_snapshot(app->irc, &state, error, error_size);
        if (rc == 0)
            rc = snj_buf_terminate(&state);
        if (rc == 0)
            *result = snj_tool_result_terminal(true,
                                                (const char *)state.data);
        snj_buf_free(&state);
        return rc < 0 || !*result ? -1 : 0;
    }
    if (call && call->name && strcmp(call->name, "irc_send") == 0) {
        const char *text = snj_json_string(call->arguments, "text");
        json_t *notice_value = json_object_get(call->arguments, "notice");
        bool notice;
        int rc;

        *result = NULL;
        if (!app->irc ||
            !snj_json_exact_keys(call->arguments, send_keys, 2u) ||
            !text || !*text || strlen(text) > SNJ_MAX_PUBLIC_ITEM ||
            !snj_utf8_valid((const unsigned char *)text, strlen(text), true) ||
            (!json_is_null(notice_value) &&
             !json_is_true(notice_value) && !json_is_false(notice_value))) {
            *result = snj_tool_result_terminal(false,
                                                "irc_send arguments are invalid");
            return *result ? 0 : -1;
        }
        notice = json_is_true(notice_value);
        rc = notice ? snj_irc_send_agent_notice(app->irc, text, error, error_size) :
                      snj_irc_send_agent(app->irc, text, error, error_size);
        *result = snj_tool_result_terminal(rc == 0,
            rc == 0 ? (notice ? "IRC notice sent" : "IRC message sent") :
            (error[0] ? error : "IRC message could not be sent"));
        return *result ? 0 : -1;
    }
    if (call && call->name && strcmp(call->name, "irc_topic") == 0) {
        const char *topic = snj_json_string(call->arguments, "topic");
        int rc;

        *result = NULL;
        if (!app->irc ||
            !snj_json_exact_keys(call->arguments, topic_keys, 1u) || !topic) {
            *result = snj_tool_result_terminal(false,
                                                "irc_topic arguments are invalid");
            return *result ? 0 : -1;
        }
        rc = snj_irc_set_agent_topic(app->irc, topic, error, error_size);
        *result = snj_tool_result_terminal(rc == 0,
            rc == 0 ? "IRC topic changed" :
            (error[0] ? error : "IRC topic could not be changed"));
        return *result ? 0 : -1;
    }
#ifdef SNAJPAGENT_TEST_FIXTURE
    (void)credential;
    return snj_fixture_tool(call, snj_app_active_input_pump, app,
                            result, error, error_size);
#else
    return snj_tools_run(call, app->config, credential,
                         app->session.workspace,
                         tool_input_pump, app, snj_ui_wake_fd(&app->ui),
                         result, error, error_size);
#endif
}
