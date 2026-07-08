/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"

#include "context.h"
#include "json.h"
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
    if (!app->config->provider_native_compaction) {
        if (!active_prefix && strcmp(reason, "manual") == 0 &&
            snj_render_host(&app->render,
                            "native compaction is disabled by provider configuration") < 0)
            return -1;
        return 0;
    }
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
                            "native compaction skipped; no new context since the previous compact output") < 0)
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
    if (snj_app_provider_compact(app, request, credential, &output,
                                 &output_tokens_bound,
                                 error, error_size) != 0)
        goto out;
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
                        "native compaction completed and installed for future turns") < 0)
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
    if (app->config->auto_compact_input_tokens == 0u ||
        !app->config->provider_native_compaction)
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
    if (app->config->auto_compact_input_tokens == 0u ||
        !app->config->provider_native_compaction)
        return 0;
    if (input_tokens_bound < app->config->auto_compact_input_tokens)
        return 0;
    return run_compaction(app, "automatic", true, credential, compacted,
                          error, error_size);
}
