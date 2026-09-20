/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"
#include "media.h"
#include "provider.h"
#include "context.h"
#include "json.h"
#include "tools.h"
#include "tools_write.h"
#include "wire.h"
#include "secret.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
snag_app_provider_input_pump(void *opaque, unsigned int timeout_ms)
{
    struct app_state *app = opaque;
    uint64_t before = app->input_generation;
    int rc = snag_app_active_input_pump(app, timeout_ms);

    return rc == 0 && before != app->input_generation ? SNAG_PROVIDER_NEW_INPUT : rc;
}

#ifdef SNAJPAGENT_TEST_FIXTURE
static bool fixture_capacity_rejected_once;

static json_t *
fixture_model_limits(size_t index)
{
    return json_pack("{s:n,s:o,s:n,s:n,s:o,s:n,s:n}", "auto_compact_input_tokens", "context_window_tokens",
        index < 2u ? json_integer(272000) : json_null(),
        "effective_context_window_percent", "input_context_window_tokens",
        "max_context_window_tokens", index < 2u ? json_integer(872000) : json_null(),
        "max_input_tokens", "max_output_tokens");
}

int snag_fixture_response(const char *prompt, const json_t *steering, const json_t *request,
                         const char *workspace, unsigned int cycle,
                         const char *goal_prompt, uint64_t goal_turn_count,
                         snag_responses_emit_fn emit, snag_provider_pump_fn pump, void *opaque,
                         snag_responses_hosted_fn hosted, void *hosted_opaque,
                         struct snag_response_graph *graph, struct snag_provider_failure *failure,
                         char *error, size_t error_size);
int snag_fixture_tool(const struct snag_response_item *call, snag_provider_pump_fn pump, void *pump_opaque,
                     json_t **result, char *error, size_t error_size);
#endif

int
snag_app_provider_models(struct app_state *app, const struct snag_provider_config *provider,
                        json_t **models, char *error, size_t error_size)
{
#ifdef SNAJPAGENT_TEST_FIXTURE
    static const char *const ids[] = {
        "gpt-5.6-luna", "gpt-5.6-terra", "vendor/future-model" };
    const char *failure = getenv("SNAJPAGENT_FIXTURE_MODEL_FAILURE");
    json_t *out = NULL;

    (void)app;
    if (models) *models = NULL;
    if (failure && strcmp(failure, provider->name) == 0) {
        if (error_size) (void)snprintf(error, error_size, "fixture model discovery failed");
        return snag_errno(EIO);
    }
    out = json_array();
    if (!models || !out) goto fail;
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        json_t *variants = i == 0u ? json_pack("[s]", "high") :
            i == 1u ? json_pack("[s,s,s,s,s,s]", "low", "medium", "high",
                                "xhigh", "max", "ultra") : json_array();
        json_t *entry = json_pack("{s:s?,s:o,s:s,s:o}", "default_effort",
            i == 0u ? "high" : i == 1u ? "medium" : NULL,
            "efforts", variants, "id", ids[i], "limits", fixture_model_limits(i));
        if (!entry || json_array_append_new(out, entry) < 0) goto fail;
    }
    *models = out;
    return 0;
fail: json_decref(out);
    return snag_errno(ENOMEM);
#else
    struct snag_credential credential;
    int rc;
    snag_credential_clear(&credential);
    if (snag_auth_read(app->store.root_fd, provider, false, NULL, &credential,
                      snag_app_active_input_pump, app, error, error_size) < 0) return -1;
    rc = snag_provider_models_list((struct snag_provider_connection){
        app->config, provider, &credential, &app->ui, snag_app_provider_input_pump, app, app->session.id},
        models, error, error_size);
    snag_credential_clear(&credential);
    return rc;
#endif
}

bool
snag_app_exact_count_enabled(enum snag_token_count_mode mode, enum snag_count_capability capability)
{
    return mode == SNAG_TOKEN_COUNT_STRICT || (mode == SNAG_TOKEN_COUNT_AUTO &&
         capability != SNAG_COUNT_UNSUPPORTED);
}

/* Hosted search is executed by the provider; journal and render it as a tool
 * row without ever treating it as a local call the dispatcher could run. */
static int
hosted_search_activity(void *opaque, bool started, const char *item_id, const char *status,
                       const json_t *action, const json_t *sources)
{
    struct app_state *app = opaque;
    char error[256] = {0};
    const char *turn_id = app->session.active_turn_id;
    json_t *data;
    int rc;

    if (!turn_id[0]) return snag_errno(EPROTO);
    if (!(data = json_object()) ||
        json_object_set_new(data, "item_id", json_string(item_id)) < 0 ||
        json_object_set_new(data, "turn_id", json_string(turn_id)) < 0) {
        json_decref(data);
        return snag_errno(errno ? errno : ENOMEM);
    }
    if (started) {
        /* The callback lends its values; the event owns its own copies. */
        if (action && json_object_set_new(data, "action", json_deep_copy(action)) < 0) {
            json_decref(data);
            return snag_errno(errno ? errno : ENOMEM);
        }
    } else {
        if (json_object_set_new(data, "status",
                                json_string(status && status[0] ? status : "unknown")) < 0 ||
            (sources && json_object_set_new(data, "sources", json_deep_copy(sources)) < 0)) {
            json_decref(data);
            return snag_errno(errno ? errno : ENOMEM);
        }
    }
    rc = snag_app_commit_event(app, started ? "hosted_search_started" : "hosted_search_finished",
                               data, error, sizeof(error));
    return rc < 0 ? snag_errno(errno ? errno : EIO) : 0;
}

#ifndef SNAJPAGENT_TEST_FIXTURE
static int
media_count(struct app_state *app, const json_t *request, uint64_t *tokens,
             const char **method, char *error, size_t size)
{
    struct snag_model_limit_config limits;
    (void)snag_config_resolve_limits(app->config, app->turn_provider->name, app->turn_model, &limits, NULL);
    if (snag_media_token_bound(request, limits.image_tokens, tokens, error, size) < 0) return -1;
    *method = "media_upper_bound";
    if (size) *error = '\0';
    return SNAG_APP_COUNT_SKIPPED;
}
#endif

int
snag_app_provider_count(struct app_state *app, const json_t *count_request,
                       const struct snag_credential *credential, uint64_t *input_tokens,
                       const char **count_method, char *error, size_t error_size)
{
#ifdef SNAJPAGENT_TEST_FIXTURE
    (void)credential;
    if (strcmp(snag_json_string(count_request, "model"),
               snag_config_model_upstream(app->turn_provider, app->turn_model)) != 0)
        return snag_errorf(error, error_size, "fixture count request did not resolve the provider model");
    (void)input_tokens;
    if (app->turn_provider->exact_token_count == SNAG_TOKEN_COUNT_STRICT) {
        *count_method = "exact";
        *input_tokens = 1000u;
    }
    if (app->session.last_user && strcmp(app->session.last_user, "compact_budget") == 0) {
        *input_tokens = 90000u;
        *count_method = "exact";
    }
    {
        bool wait_for_mention;

        struct snag_buf encoded = {.max = SNAG_WIRE_BODY_MAX};
        if (snag_json_canonical(count_request, &encoded) < 0 || snag_buf_terminate(&encoded) < 0) {
            if (error_size) (void)snprintf(error, error_size, "fixture count request could not be encoded");
            snag_buf_free(&encoded);
            return -1;
        }
        wait_for_mention = strstr((const char *)encoded.data, "network_count_wait") != NULL &&
            strstr((const char *)encoded.data, "network count mention") == NULL;
        snag_buf_free(&encoded);
        if (wait_for_mention)
            for (unsigned int i = 0u; i < 100u; ++i) {
                int pump_rc = snag_app_active_input_pump(app, 20u);

                if (pump_rc != 0) return pump_rc;
            }
    }
    return 0;
#else
    bool endpoint_unsupported = false;
    uint64_t exact_tokens = 0u;
    int rc;

    bool images = snag_media_request_has_images(count_request);
    if (!snag_app_exact_count_enabled(
            app->turn_provider->exact_token_count,
            app->turn_capacity.count_capability))
 {
        if (images) return media_count(app,count_request,input_tokens,count_method,error,error_size);
        return SNAG_APP_COUNT_SKIPPED;
    }
    rc = snag_provider_responses_count((struct snag_provider_connection){
        app->config, app->turn_provider, credential, &app->ui, snag_app_provider_input_pump, app, app->session.id},
        count_request, &exact_tokens, &endpoint_unsupported, error, error_size, NULL);
    if (rc == 0) {
        *input_tokens = exact_tokens;
        *count_method = "exact";
        snag_app_record_model_accounting(app, SNAG_COUNT_SUPPORTED, 0u);
        return 0;
    }
    if (!endpoint_unsupported) return rc;
    snag_app_record_model_accounting(app, SNAG_COUNT_UNSUPPORTED, 0u);
    if (app->turn_provider->exact_token_count == SNAG_TOKEN_COUNT_STRICT)
        return rc;
    if (images) return media_count(app, count_request, input_tokens, count_method, error, error_size);
    if (error_size)
        error[0] = '\0';
    return SNAG_APP_COUNT_SKIPPED;
#endif
}

int
snag_app_provider_compact(struct app_state *app, const json_t *compact_request,
                         const struct snag_credential *credential, struct snag_json_document *output,
                         char *error, size_t error_size)
{
#ifdef SNAJPAGENT_TEST_FIXTURE
    json_t *fixture_output = json_pack("[{s:s,s:s}]",
        "encrypted_content", "fixture-native-compact", "type", "compaction");

    (void)compact_request;
    (void)credential;
    if (app->turn_provider->auth == SNAG_AUTH_CHATGPT &&
        app->session.last_user && strcmp(app->session.last_user, "native_compact_unavailable") == 0) {
        json_decref(fixture_output);
        return SNAG_PROVIDER_UNSUPPORTED;
    }
    if (app && app->session.last_user &&
        (snag_string_in(app->session.last_user, "compaction_steer capacity_recovery_steer")))
        for (unsigned int i = 0u; i < 100u; ++i) {
            int pump_rc = snag_app_active_input_pump(app, 20u);

            if (pump_rc != 0) {
                json_decref(fixture_output);
                return pump_rc;
            }
        }
    return snag_context_compact_output_set(output, fixture_output, error, error_size);
#else
    return snag_provider_responses_compact((struct snag_provider_connection){
        app->config, app->turn_provider, credential, &app->ui, snag_app_provider_input_pump, app, app->session.id},
        compact_request, output, error, error_size, NULL);
#endif
}

int
snag_app_provider_run(struct app_state *app, const char *prompt, const json_t *steering, unsigned int cycle,
                     const json_t *create_request, const struct snag_credential *credential,
                     struct snag_response_graph *graph, struct snag_provider_failure *failure,
                     char *error, size_t error_size, unsigned int *retry_count)
{
#ifdef SNAJPAGENT_TEST_FIXTURE
    (void)credential;
    if (failure) memset(failure, 0, sizeof(*failure));
    if (retry_count) *retry_count = 0u;
    if (((snag_string_in(prompt, "capacity_recovery capacity_recovery_steer")) &&
         !fixture_capacity_rejected_once) || strcmp(prompt, "capacity_recovery_twice") == 0) {
        fixture_capacity_rejected_once = true;
        if (failure) {
            memcpy(failure->code, "context_length_exceeded", 24u);
            memcpy(failure->message, "fixture context rejected", 25u);
            failure->context_limit_tokens = 100000u;
            failure->requested_input_tokens = 90000u;
        }
        if (error_size) (void)snprintf(error, error_size, "fixture context rejected");
        return snag_errno(EOVERFLOW);
    }
    {
        json_t *input = json_object_get(create_request, "input");
        bool read_only = app->session.active_read_only;

        for (size_t i = 0; i < json_array_size(input); ++i) {
            json_t *message = json_array_get(input, i);
            const char *role = snag_json_string(message, "role");
            json_t *content = json_object_get(message, "content");

            if (!role || strcmp(role, "developer") != 0) continue;
            for (size_t j = 0; j < json_array_size(content); ++j) {
                const char *text = snag_json_string(json_array_get(content, j), "text");
                if ((read_only || app->session.active_queued || app->session.pending_queue_count) && text &&
                    strncmp(text, "Persistent goal ", 16u) == 0)
                    return snag_errorf(error, error_size, "fixture: goal reminder bypassed queued work");
            }
        }
    }
    return snag_fixture_response(prompt, steering, create_request, app->session.workspace, cycle,
                                app->session.goal_prompt, app->session.goal_turn_count,
                                snag_app_stream_public, snag_app_active_input_pump,
                                app, hosted_search_activity, app, graph, failure, error, error_size);
#else
    (void)prompt;
    (void)steering;
    (void)cycle;
    if (retry_count) *retry_count = 0u;
    return snag_provider_responses_create((struct snag_provider_connection){
        app->config, app->turn_provider, credential, &app->ui, snag_app_provider_input_pump, app, app->session.id},
        create_request, snag_app_stream_public, app, hosted_search_activity, app,
        graph, failure, error, error_size, retry_count);
#endif
}

#ifndef SNAJPAGENT_TEST_FIXTURE
static int
tool_input_pump(void *opaque, unsigned int timeout_ms)
{
    struct app_state *app = opaque;
    int rc = snag_app_active_input_pump(opaque, timeout_ms);

    if (rc == 0 && app->irc_urgent.len) return 1;
    return rc;
}
#endif

static bool
irc_tool_route(const struct app_state *app, const json_t *destination, struct snag_irc_route *route)
{
    const char *text = json_is_string(destination) ? json_string_value(destination) : NULL;
    uint32_t id;
    size_t body;
    char selector[16u];

    memset(route, 0, sizeof(*route));
    if (((!destination || json_is_null(destination)) && app->irc_request_route.count == 1u) ||
        (text && strcmp(text, "all") == 0)) {
        *route = app->irc_request_route;
        return route->count != 0u;
    }
    if (!text || strlen(text) > 10u) return false;
    (void)snprintf(selector, sizeof(selector), "/%s", text);
    if (snag_irc_target_parse(selector, strlen(selector), &id, &body) != SNAG_IRC_TARGET_SELECT) return false;
    for (size_t i = 0u; i < app->irc_request_route.count; ++i)
        if (app->irc_request_route.targets[i].id == id) {
            route->targets[route->count++] = app->irc_request_route.targets[i];
            return true;
        }
    return false;
}

static int
video_transcribe(void *opaque, const json_t *source, uint64_t start, uint64_t end, json_t **result)
{
    struct app_state *app = opaque;
    return snag_tools_transcribe_asset(&app->session, source, start, end, app->store.root_fd,
        app->config, snag_app_active_input_pump, app, snag_ui_wake_fd(&app->ui), result);
}

int
snag_app_tool_run(struct app_state *app, const struct snag_response_item *call,
                 const struct snag_credential *credential, json_t **result, char *error, size_t error_size)
{
    if (call && call->name && (!strcmp(call->name, "read_document") ||
        !strcmp(call->name, "view_video") || !strcmp(call->name, "view_image") ||
        !strcmp(call->name, "listen_audio") || !strcmp(call->name, "transcribe_audio") ||
        (!app->session.active_read_only && !strcmp(call->name, "speak_text")))) {
        struct snag_secret_set secrets = {0};
        int rc = snag_secret_set_build(&secrets, app->config, credential, error, error_size);
        if (!rc) {
            if (!strcmp(call->name, "read_document"))
                rc = snag_tools_document(call, &app->session, snag_app_active_input_pump, app,
                                         snag_ui_wake_fd(&app->ui), result);
            else if (!strcmp(call->name, "view_video"))
                rc = snag_tools_video(call, &app->session, snag_app_active_input_pump, app,
                                      snag_ui_wake_fd(&app->ui), video_transcribe, result);
            else if (!strcmp(call->name, "view_image"))
                rc = snag_tools_image(call, &app->session,
                                      snag_app_active_input_pump, app, result);
            else
                rc = snag_tools_audio(call, &app->session, app->store.root_fd, app->config,
                                      snag_app_active_input_pump, app, snag_ui_wake_fd(&app->ui), result);
        }
        if (!rc && *result) rc = snag_secret_result(&secrets, *result, error, error_size);
        snag_secret_set_free(&secrets);
        return rc;
    }
    /* Exploration tools are available in every turn. */
    if (call && call->name && snag_read_only_tool(call->name)) {
        struct snag_secret_set secrets = {0};
        int rc = snag_secret_set_build(&secrets, app->config, credential, error, error_size);
        if (rc == 0) rc = snag_tools_read_only(call, app->session.workspace,
                                         snag_app_active_input_pump, app, result);
        if (rc == 0 && *result) rc = snag_secret_result(&secrets, *result, error, error_size);
        snag_secret_set_free(&secrets);
        return rc;
    }
    if (app->session.active_read_only) {
        *result = snag_tool_result_terminal(false,
            "Tool unavailable: this turn is read-only; use list_files, read_file, grep or view_image.");
        return *result ? 0 : -1;
    }
    if (call && call->name && strcmp(call->name, "write_file") == 0)
        return snag_tools_write_file(call, app->session.workspace, result, error, error_size);
    if (call && call->name && strcmp(call->name, "edit_file") == 0)
        return snag_tools_edit_file(call, app->session.workspace, result, error, error_size);

    if (call && call->name && strcmp(call->name, "timer") == 0)
        return snag_app_timer_tool(app, call, result, error, error_size);
    if (call && call->name && strcmp(call->name, "defer_steering") == 0) {
        if (!call->arguments || !snag_json_exact_keys(call->arguments, ""))
            return (*result = snag_tool_result_terminal(false,
                "defer_steering takes no arguments: {}.")) ? 0 : -1;
        app->steering_deferred = true;
        return (*result = snag_tool_result_terminal(true,
            "steering deferred for the remainder of the turn")) ? 0 : -1;
    }
    if (call && call->name && (snag_string_in(call->name, "create_goal update_goal")))
        return snag_app_goal_tool(app, call, result, error, error_size);
    if (call && call->name && snag_string_in(call->name, "irc_connect irc_host irc_disconnect")) {
        const char *endpoint = NULL;
        bool hosting = strcmp(call->name, "irc_host") == 0;
        char message[256];
        int rc;

        *result = NULL;
        if (!app->irc)
            return (*result = snag_tool_result_terminal(false,
                "IRC runtime is not available in this one-shot process.")) ? 0 : -1;
        if (!strcmp(call->name, "irc_disconnect")) {
            if (!snag_json_arg_keys(call->arguments, "endpoint", "hosting", error, error_size) ||
                !snag_json_arg_text(call->arguments, "endpoint", 1u, SNAG_CONFIG_IRC_ENDPOINT_MAX,
                                    false, &endpoint, error, error_size) ||
                !snag_json_arg_bool(call->arguments, "hosting", false, &hosting, error, error_size))
                return (*result = snag_tool_result_terminal(false, error)) ? 0 : -1;
            rc = snag_irc_remove(app->irc, hosting, endpoint, error, error_size);
        } else {
            if (!snag_json_arg_keys(call->arguments, "endpoint", "", error, error_size) ||
                !snag_json_arg_text(call->arguments, "endpoint", 1u, SNAG_CONFIG_IRC_ENDPOINT_MAX,
                                    false, &endpoint, error, error_size))
                return (*result = snag_tool_result_terminal(false, error)) ? 0 : -1;
            rc = snag_irc_add(app->irc, app->config, app->session.workspace, hosting,
                              endpoint, error, error_size);
        }
        if (rc < 0)
            return (*result = snag_tool_result_terminal(false, error[0] ? error :
                "IRC endpoint transition failed.")) ? 0 : -1;
        app->networked = true;
        if (snag_app_sync_destinations(app) < 0)
            return snag_errorf(error, error_size, "IRC destination state could not be refreshed");
        if (!strcmp(call->name, "irc_disconnect") && !app->irc_destinations.count)
            app->networked = false;
        (void)snprintf(message, sizeof(message), "IRC %s succeeded for %s",
                       !strcmp(call->name, "irc_connect") ? "connect" :
                       !strcmp(call->name, "irc_host") ? "host" : "disconnect", endpoint);
        return (*result = snag_tool_result_terminal(true, message)) ? 0 : -1;
    }
    if (call && call->name && strcmp(call->name, "irc_state") == 0) {
        int rc;

        *result = NULL;
        if (!snag_json_arg_keys(call->arguments, "", "", error, error_size)) {
            *result = snag_tool_result_terminal(false, "irc_state takes an empty JSON object: {}.");
            return *result ? 0 : -1;
        }
        struct snag_buf state = {.max = SNAG_MAX_IRC_SNAPSHOT};
        rc = app->irc ? snag_irc_state(app->irc, &state, error, error_size) :
            snag_buf_printf(&state, "no active endpoints\n");
        if (rc == 0) rc = snag_buf_terminate(&state);
        if (rc == 0) *result = snag_tool_result_terminal(true, (const char *)state.data);
        snag_buf_free(&state);
        return rc < 0 || !*result ? -1 : 0;
    }
    if (call && call->name && (snag_string_in(call->name, "irc_send irc_topic"))) {
        bool topic = strcmp(call->name, "irc_topic") == 0;
        const char *text = snag_json_string(call->arguments, topic ? "topic" : "text");
        struct snag_irc_route route;
        int rc;

        *result = NULL;
        bool notice = false;
        if (!snag_json_arg_keys(call->arguments,
                                 topic ? "topic" : "text", topic ? "destination" : "destination notice", error, error_size) ||
            !snag_json_arg_text(call->arguments, topic ? "topic" : "text", topic ? 0u : 1u,
                                SNAG_MAX_PUBLIC_ITEM, false, &text, error, error_size) ||
            (!topic && !snag_json_arg_bool(call->arguments, "notice", false, &notice, error, error_size))) {
            *result = snag_tool_result_terminal(false, error);
            return *result ? 0 : -1;
        }
        if (!app->irc) {
            *result = snag_tool_result_terminal(false,
                "IRC runtime is not connected or hosted; use irc_connect or irc_host first.");
            return *result ? 0 : -1;
        }
        if (!irc_tool_route(app, json_object_get(call->arguments, "destination"), &route)) {
            *result = snag_tool_result_terminal(false,
                "Select a destination number string from irc_state, or all to broadcast. "
                "Null is valid only for a sole destination. No message was sent.");
            return *result ? 0 : -1;
        }
        struct snag_buf report = {.max = 8192u};
        rc = snag_irc_send_route(app->irc, &route, true,
            topic ? SNAG_IRC_TOPIC : notice ? SNAG_IRC_NOTICE : SNAG_IRC_MESSAGE,
            text, &report, error, error_size);
        if (rc >= 0 && snag_buf_terminate(&report) == 0)
            *result = snag_tool_result_terminal(rc == 0, report.len > 1u ? (const char *)report.data : error);
        snag_buf_free(&report);
        return rc < 0 || !*result ? -1 : 0;
    }
#ifdef SNAJPAGENT_TEST_FIXTURE
    (void)credential;
    return snag_fixture_tool(call, snag_app_active_input_pump, app, result, error, error_size);
#else
    return snag_tools_run(call, app->config, credential, app->session.workspace,
                         tool_input_pump, app, snag_ui_wake_fd(&app->ui), result, error, error_size);
#endif
}
