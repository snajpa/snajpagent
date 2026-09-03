/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"
#include "provider.h"
#include "context.h"
#include "json.h"
#include "tools.h"

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
int snj_fixture_response(const char *prompt, const json_t *steering,
                         const char *workspace, unsigned int cycle,
                         fixture_emit_fn emit, fixture_pump_fn pump, void *opaque,
                         struct snj_response_graph *graph,
                         char *error, size_t error_size);
int snj_fixture_tool(const struct snj_response_item *call, json_t **result,
                     char *error, size_t error_size);
#endif

int
snj_app_provider_models(struct app_state *app,
                        const struct snj_provider_config *provider,
                        json_t **models, char *error, size_t error_size)
{
#ifdef SNAJPAGENT_TEST_FIXTURE
    static const char *const ids[] = {
        "gpt-5.6-sol", "gpt-5.6-terra", "vendor/future-model"
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
        if (i < 2u)
            for (size_t j = 0; j < sizeof(efforts) / sizeof(efforts[0]); ++j)
                if (json_array_append_new(variants,
                                          json_string(efforts[j])) < 0)
                    goto fail_entry;
        if (snj_json_set_new(entry, "default_effort",
                             i < 2u ? json_string("medium") : json_null()) < 0)
            goto fail_entry;
        if (snj_json_set_new(entry, "efforts", variants) < 0) {
            variants = NULL;
            goto fail_entry;
        }
        variants = NULL;
        if (snj_json_set_new(entry, "id", json_string(ids[i])) < 0)
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
                                  &app->render, models, error, error_size);
    snj_credential_clear(&credential);
    return rc;
#endif
}

int
snj_app_provider_count(struct app_state *app, const json_t *count_request,
                       const struct snj_credential *credential,
                       uint64_t *input_tokens, char *error, size_t error_size)
{
#ifdef SNAJPAGENT_TEST_FIXTURE
    (void)app;
    (void)count_request;
    (void)credential;
    (void)input_tokens;
    (void)error;
    (void)error_size;
    return 0;
#else
    int cancel_code = 0;
    int rc = snj_provider_responses_count(count_request, app->config,
                                          app->turn_provider, credential,
                                          &app->render, NULL, NULL,
                                          input_tokens, error, error_size,
                                          &cancel_code, NULL);
    if (rc == 1 || rc == 2)
        return rc;
    return rc;
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

    (void)app;
    (void)compact_request;
    (void)credential;
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
                                            credential, &app->render,
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
                     char *error, size_t error_size,
                     unsigned int *retry_count)
{
#ifdef SNAJPAGENT_TEST_FIXTURE
    (void)create_request;
    (void)credential;
    if (retry_count)
        *retry_count = 0u;
    return snj_fixture_response(prompt, steering, app->session.workspace, cycle,
                                snj_app_stream_public, snj_app_active_input_pump,
                                app, graph, error, error_size);
#else
    int cancel_code = 0;
    (void)prompt;
    (void)steering;
    (void)cycle;
    if (retry_count)
        *retry_count = 0u;
    int rc = snj_provider_responses_create(create_request, app->config,
                                           app->turn_provider, credential,
                                           &app->render,
                                           snj_app_stream_public_response, app,
                                           snj_app_active_input_pump, app, graph,
                                           error, error_size, &cancel_code,
                                           retry_count);
    if (rc == 1 || rc == 2)
        return rc;
    return rc;
#endif
}

int
snj_app_tool_run(struct app_state *app, const struct snj_response_item *call,
                 const struct snj_credential *credential, json_t **result,
                 char *error, size_t error_size)
{
#ifdef SNAJPAGENT_TEST_FIXTURE
    (void)app;
    (void)credential;
    return snj_fixture_tool(call, result, error, error_size);
#else
    return snj_tools_run(call, app->config, credential,
                         app->session.workspace,
                         snj_app_active_input_pump, app,
                         result, error, error_size);
#endif
}
