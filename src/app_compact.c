/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"
#include "media.h"

#include "context.h"
#include "json.h"
#include "provider.h"
#include "snajpagent.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static bool
count_method_valid(const char *method)
{
    /* The local image bound produces the legitimate conservative
     * media_upper_bound method when exact counting is unavailable; the
     * non-exact branches below already treat it conservatively. */
    return snag_string_in(method, "exact unknown media_upper_bound");
}

static bool
active_reason(const char *reason)
{
    return snag_string_in(reason, "proactive hard_budget provider_rejection model_switch");
}

static json_t *
compaction_interrupted_data(const char *compact_id, const char *reason)
{
    return json_pack("{s:s,s:s}", "compact_id", compact_id, "reason", reason);
}

static int
commit_rendered(struct app_state *app, const char *type, json_t *data, char *error, size_t error_size)
{
    uint64_t seq;

    if (!data) return snag_fail(error, error_size, ENOMEM, "cannot allocate %s event", type);
    if (snag_session_commit(&app->session, type, data, &seq, error, error_size) < 0) return -1;
    if (snag_ui_send(&app->ui, (struct snag_ui_command){
        .kind = SNAG_UI_EVENT, .text = type, .data.seq = seq}) < 0) {
        return snag_errorf(error, error_size, "durable compaction event output failed");
    }
    return 0;
}

static int
compaction_state_valid(const struct app_state *app, const char *reason,
                       bool active_prefix, char *error, size_t error_size)
{
    if (!app || !app->config || !reason || (strcmp(reason, "manual") != 0 && !active_reason(reason)))
        return snag_fail(error, error_size, EINVAL, "invalid compaction reason");
    if (active_prefix) {
        if (!app->session.active_turn || app->session.response_open ||
            app->session.pending_call_count || app->session.active_compact_id[0] != '\0')
            return snag_fail(error, error_size, EINVAL,
                "pre-response compaction requires an active turn before response");
        return 0;
    }
    if (app->session.active_turn || app->session.response_open || app->session.process_count ||
        app->session.active_compact_id[0] != '\0')
        return snag_fail(error, error_size, EINVAL, "compaction requires an idle session");
    return 0;
}

static json_t *
responses_compact_create_request(const json_t *compact_request, const char *model, const char *effort,
                                 const struct snag_model_capacity *capacity)
{
    static const char instruction[] =
        "Compact the prior conversation for future Responses turns. Return only "
        "the summary text, preserving the user's goals, decisions, "
        "constraints, repository state, active blockers, and next steps. Preserve "
        "working-document locations and distinguish requirements, observations, "
        "unapproved proposals and corrected assumptions. The summary is a recovery "
        "aid, not new authority. Do not use a JSON wrapper or tool calls.";
    json_t *input = json_object_get(compact_request, "input");
    json_t *copy;
    json_t *request = NULL;

    if (!json_is_array(input) || !model || !effort || !capacity) return NULL;
    copy = json_copy(input);
    if (!copy || json_array_append_new(copy, json_pack("{s:s,s:s}", "role", "developer",
                      "content", instruction)) < 0) goto out;
    /* No tool_choice: this request declares no tools, so the choice is inert,
     * and a provider that accepts only "auto" rejects every other value. */
    request = json_pack("{s:O,s:s,s:b,s:{s:s},s:b,s:b,s:[],s:s}",
        "input", copy, "model", model, "parallel_tool_calls", 0,
        "reasoning", "effort", effort, "store", 0, "stream", 1,
        "tools", "truncation", "disabled");
    if (request && capacity->max_output_tokens && snag_json_set_new(request, "max_output_tokens",
            json_integer((json_int_t)capacity->max_output_tokens)) < 0) {
        json_decref(request);
        request = NULL;
    }
out: json_decref(copy);
    return request;
}

static json_t *
responses_compact_count_request(const json_t *create_request)
{
    json_t *request = json_copy((json_t *)create_request);

    if (!request) return NULL;
    if (json_object_del(request, "store") < 0 || json_object_del(request, "stream") < 0 ||
        (json_object_get(request, "max_output_tokens") &&
         json_object_del(request, "max_output_tokens") < 0)) {
        json_decref(request);
        return NULL;
    }
    return request;
}

static int
run_responses_compaction(struct app_state *app, const json_t *create_request,
                         const struct snag_credential *credential, struct snag_json_document *output,
                         char *error, size_t error_size)
{
    snag_json_document_free(output);
#ifdef SNAJPAGENT_TEST_FIXTURE
    (void)app;
    (void)create_request;
    (void)credential;
    return snag_context_compact_output_set(output, json_pack("[{s:s,s:s,s:s}]",
        "content", "fixture responses compact summary",
        "role", "user", "type", "message"), error, error_size);
#else
    struct snag_graph_decision decision;
    struct snag_provider_failure failure = {0};
    int rc;

    struct snag_response_graph graph = {0};
    rc = snag_provider_responses_create((struct snag_provider_connection){
        app->config, app->turn_provider, credential, &app->ui, snag_app_provider_input_pump, app,
        app->session.id, app->turn_provider->request_timeout_ms},
        create_request, NULL, NULL, NULL, NULL, &graph, &failure, error, error_size, NULL);
    if (rc != 0 && snag_provider_failure_is_capacity(&failure)) rc = SNAG_PROVIDER_CONTEXT_OVERFLOW;
    /* A provider that answered with an error status rejected this request; only
     * a lost body (no provider facts) is worth retrying with a smaller source. */
    if (rc != 0 && !snag_provider_failure_is_capacity(&failure) &&
        (failure.code[0] || failure.type[0] || failure.message[0]))
        rc = SNAG_PROVIDER_REJECTED;
    if (rc != 0 && !failure.new_input && snag_provider_failure_is_policy(&failure))
        app->turn_policy_stopped = SNAG_POLICY_STOP_PROVIDER;
    if (rc != 0) goto out;
    rc = -1;
    if (snag_response_graph_classify(&graph, &decision, error, error_size) < 0) {
        /* The provider answered: a summary we cannot use is a rejection for this
         * command, not a lost body worth retrying with a smaller source. */
        rc = SNAG_PROVIDER_REJECTED;
        goto out;
    }
    if (decision.outcome == SNAG_GRAPH_REFUSAL) app->turn_policy_stopped = SNAG_POLICY_STOP_REFUSAL;
    struct snag_response_item final = snag_response_graph_item(&graph, decision.final_index);
    if (decision.outcome != SNAG_GRAPH_FINAL || decision.final_index >= graph.count || !final.text) {
        (void)snag_fail(error, error_size, EPROTO, "Responses compaction did not return a final summary");
        rc = SNAG_PROVIDER_REJECTED;
        goto out;
    }
    rc = snag_context_compact_output_set(output, json_pack("[{s:s,s:s,s:s}]",
        "type", "message", "role", "user", "content", final.text), error, error_size);
out: snag_response_graph_free(&graph);
    return rc;
#endif
}

/* Nothing uncovered: the history is already summarized in full, so no chunk can
 * be compacted and a capacity rejection can only be reduced by condensing the
 * merged summary itself (the merge step) under the same binding. Returns 1 when
 * a reduce ran and committed, 0 when there was nothing to condense, <0 on a
 * real failure. */
static int
run_reduce_attempt(struct app_state *app, const char *reason, const struct snag_credential *credential,
                   bool native, const char *model, const char *continuation_scope,
                   char *error, size_t error_size)
{
    static const char instruction[] =
        "merge these summaries of overlapping chunks, deduplicating anything that appears twice";
    struct snag_json_document request = {0}, output = {0};
    char compact_id[SNAG_ID_HEX_LEN + 1u];
    char source_hash[SNAG_SHA256_HEX_LEN + 1u];
    bool started = false;
    int rc = 0, stage_rc;

    if (!app->session.compact_output || !app->session.compact_id[0]) return 0;
    if (snag_context_compact_reduce_request_build(&app->session, app->turn_provider, model,
            app->turn_effort, app->session.compact_output, instruction, &request, error, error_size) != 0)
        return 0;   /* no portable text: nothing to condense */
    rc = -1;
    if (snag_context_compact_output_valid(app->session.compact_output, source_hash, NULL, error, error_size) < 0 ||
        snag_random_id(compact_id) < 0) goto out;
    if (strcmp(reason, "manual") && snag_ui_text(&app->ui, SNAG_UI_HOST,
            "Condensing the merged summary; Ctrl-C interrupts") < 0) goto out;
    if (commit_rendered(app, "compaction_started",
            json_pack("{s:s,s:s,s:s,s:s,s:I,s:s,s:s,s:s,s:s,s:s,s:s,s:I,s:s,s:s}",
                "capability_version", SNAJPAGENT_CAPABILITY_VERSION,
                "compact_id", compact_id, "count_method", "unknown",
                "count_request_sha256", request.sha256,
                "input_tokens_bound", (json_int_t)0,
                "model", app->session.active_turn_model[0] ? app->session.active_turn_model : model,
                "compaction_model", model,
                "predecessor_compact_id", app->session.compact_id,
                /* The store accepts an unchanged boundary only for this
                 * reason; emitting the caller's reason here made the commit
                 * fail and the reduce retry until the turn gave up. */
                "profile_id", SNAJPAGENT_PROFILE_ID, "reason", "reduce",
                "request_sha256", request.sha256,
                "source_seq", (json_int_t)app->session.compact_seq,
                "source_sha256", source_hash,
                "continuation_scope", continuation_scope), error, error_size) < 0) goto out;
    started = true;
    if (snag_app_provider_activity(app, true) < 0) goto out;
    if (native) {
        stage_rc = snag_app_provider_compact(app, request.value, credential, &output, error, error_size);
    } else {
        /* Every other compaction request goes through this wrapper: an empty
         * tools array, no tool_choice, the compaction instruction and the
         * capacity output limit. The reduce must not diverge from it. */
        json_t *wire = responses_compact_create_request(request.value, model, app->turn_effort,
                                                        &app->turn_capacity);
        if (!wire) { snag_errorf(error, error_size, "cannot build the reduce wire request"); goto out; }
        stage_rc = run_responses_compaction(app, wire, credential, &output, error, error_size);
        json_decref(wire);
    }
    if (snag_app_provider_activity(app, false) < 0) goto out;
    if (stage_rc != 0) goto out;
    if (commit_rendered(app, "compaction_completed",
            json_pack("{s:s,s:s,s:I,s:O,s:s,s:s,s:s,s:I,s:s,s:s}",
                "compact_id", compact_id, "count_method", "unknown",
                "input_tokens_bound", (json_int_t)0, "output", json_incref(output.value),
                "output_count_method", "unknown",
                "output_count_request_sha256", request.sha256, "output_sha256", output.sha256,
                "output_tokens_bound", (json_int_t)0,
                "source_sha256", source_hash, "continuation_scope", continuation_scope),
            error, error_size) < 0) goto out;
    if (app->networked && snag_app_irc_snapshot(app, "compaction", error, error_size) < 0) goto out;
    started = false;
    rc = 1;
out:
    if (rc < 0 && started) {
        if (commit_rendered(app, "compaction_interrupted",
                compaction_interrupted_data(compact_id, "error"), error, error_size) < 0) {
            /* the interruption failure is the one worth reporting */
        }
    }
    snag_json_document_free(&request);
    snag_json_document_free(&output);
    return rc;
}

static int
run_compaction_attempt(struct app_state *app, const char *reason, bool active_prefix, bool allow_native,
               const struct snag_credential *provided_credential,
               bool *compacted, char *error, size_t error_size)
{
    struct snag_credential owned_credential;
    const struct snag_credential *credential = provided_credential;
    struct snag_context_projection projection = {0};
    struct snag_json_document output_count = {0};
    struct snag_json_document output = {0};
    char compact_id[SNAG_ID_HEX_LEN + 1u];
    const char *count_method = "unknown";
    const char *output_count_method = "unknown";
    const char *model;
    const char *effort;
    uint64_t input_tokens_bound = 0u;
    uint64_t output_tokens_bound = 0u;
    uint64_t source_budget;
    uint64_t threshold;
    bool use_exact;
    bool reduced = false;
    bool generated = false;
    char prior_request[SNAG_SHA256_HEX_LEN + 1u] = {0};
    bool started = false;
    bool native;
    int build_rc;
    int stage_rc;
    int rc = -1;

    snag_credential_clear(&owned_credential);
    if (compacted) *compacted = false;
    if (compaction_state_valid(app, reason, active_prefix, error, error_size) < 0) return -1;
    model = active_prefix && app->turn_model ? app->turn_model : app->session.default_model;
    effort = active_prefix && app->turn_effort ? app->turn_effort : app->session.default_effort;
    if (!active_prefix) {
        app->turn_model = model;
        app->turn_provider = snag_config_provider( app->config, app->session.default_provider[0] ?
                         app->session.default_provider : NULL);
    }
    if (!app->turn_provider) {
        (void)snag_fail(error, error_size, ENOENT,
                 "selected provider is not present in the current configuration");
        goto out;
    }
    native = allow_native && app->turn_provider->native_compaction;
    if (!active_prefix && snag_app_capacity_resolve(app, app->turn_provider, model,
                                 &app->turn_capacity, error, error_size) < 0) goto out;
    threshold = snag_model_compact_threshold(app->turn_provider, &app->turn_capacity);
    if (strcmp(reason, "proactive") == 0 && !threshold) {
        rc = 0;
        goto out;
    }
#ifndef SNAJPAGENT_TEST_FIXTURE
    if (!credential) {
        if (snag_auth_read(app->store.root_fd, app->turn_provider, false, NULL,
                          &owned_credential, snag_app_active_input_pump, app, error, error_size) < 0)
            goto out;
        credential = &owned_credential;
    }
#endif
    use_exact = snag_app_exact_count_enabled( app->turn_provider->exact_token_count,
        app->turn_capacity.count_capability);
    char continuation_scope[SNAG_SHA256_HEX_LEN + 1u];
    if (snag_context_continuation_scope(app->turn_provider, model,
            credential ? credential : &owned_credential, continuation_scope) < 0) goto out;
    if (strcmp(reason, "manual") && snag_ui_text(&app->ui, SNAG_UI_HOST,
            "Compacting context; Ctrl-C interrupts") < 0) goto out;
    source_budget = SNAG_CONTEXT_MAX_COMPACT - 4096u;
    /* The compact source must fit the model that summarises it. When the route
     * cannot count exactly, derive a byte bound from this session's own last
     * observation (bytes per input token) instead of a constant, so a large
     * context does not send a request far over the model window only to stall
     * there. No usable observation keeps the protocol maximum. */
    /* Prefer the provider-reported usage anchor: it is a real count, while the
     * context meter may hold a media upper bound that would collapse the ratio. */
    const struct snag_input_observation *ratio_source = NULL;
    if (app->session.usage_anchor.valid && app->session.usage_anchor.input_tokens &&
        app->session.usage_anchor.model_input_bytes)
        ratio_source = &app->session.usage_anchor;
    else if (app->session.context_meter.valid && app->session.context_meter.input_tokens &&
             app->session.context_meter.model_input_bytes)
        ratio_source = &app->session.context_meter;
    if (app->turn_capacity.hard_input_known && ratio_source) {
        uint64_t per_token = ratio_source->model_input_bytes / ratio_source->input_tokens;
        if (per_token) {
            uint64_t window_bytes = (uint64_t)app->turn_capacity.hard_input_tokens * per_token;
            window_bytes -= window_bytes / 8u; /* summary instruction and JSON framing */
            if (window_bytes && window_bytes < source_budget) source_budget = window_bytes;
            /* A distorted observation must not shrink the budget below the
             * smallest meaningful source. */
            if (source_budget < SNAG_CONTEXT_COMPACT_FLOOR) source_budget = SNAG_CONTEXT_COMPACT_FLOOR;
        }
    }
    const struct snag_context_control control = {
        .cancelled = snag_app_context_cancelled,
        .opaque = app
    };
    for (unsigned int selection = 0u; selection < 8u; ++selection) {
        if (snag_app_provider_activity(app, true) < 0) goto out;
        /* The oversized-first exception belongs to the first attempt only: once
         * a rejection has forced a smaller budget, an oversized group must be
         * cut (the builder marks the omission) instead of being re-sent
         * unchanged, which the shrink loop reported as irreducible. */
        build_rc = snag_context_compact_request_build(&app->session, model, effort, active_prefix,
                                            source_budget, !reduced, continuation_scope,
                                            &projection, error, error_size, &control);
        bool cancelled = build_rc < 0 && errno == ECANCELED;
        if (snag_app_provider_activity(app, false) < 0) goto out;
        if (cancelled) {
            rc = snag_app_active_input_pump(app, 0u);
            if (rc == 0) rc = -1;
            goto out;
        }
        if (build_rc == 1) {
            if (selection != 0u) {
                (void)snag_fail(error, error_size, EOVERFLOW,
                         "no complete history prefix fits the hard context budget");
                goto out;
            }
            /* The history is summarized in full, so no chunk is left to compact:
             * a capacity rejection can then only be reduced by condensing the
             * merged summary itself under this same binding. */
            /* Only a capacity rejection justifies condensing an already
             * complete summary; routine "nothing new" checks must stay quiet. */
            int reduce_rc = strcmp(reason, "provider_rejection") == 0 ?
                run_reduce_attempt(app, reason, credential, native, model, continuation_scope,
                                   error, error_size) : 0;
            if (reduce_rc < 0) goto out;
            if (reduce_rc == 1) {
                if (compacted) *compacted = true;
                rc = 0;
                goto out;
            }
            if (strcmp(reason, "manual") == 0 && snag_ui_text(&app->ui, SNAG_UI_HOST, active_prefix ?
                    "compaction waiting for a complete context boundary" :
                    "compaction skipped; no new context since the previous compact output") < 0) goto out;
            rc = 0;
            goto out;
        }
        if (build_rc < 0) goto out;
        if (projection.model_input.bytes == 0u || projection.model_input.bytes > (size_t)INT64_MAX) {
            (void)snag_fail(error, error_size, EINVAL, "compact source has invalid bounds");
            goto out;
        }
        if (!native) {
            json_t *wire = responses_compact_create_request(
                projection.create_request.value, model, effort, &app->turn_capacity);
            json_decref(projection.create_request.value);
            projection.create_request.value = wire;
            json_decref(projection.count_request.value);
            projection.count_request.value = responses_compact_count_request(projection.create_request.value);
        }
        /* Codex-style compact endpoints require the field even when the caller
         * supplies no summary instruction; omission is rejected as invalid. */
        if (projection.create_request.value && native &&
            snag_json_set_new(projection.create_request.value, "instructions", json_string("")) < 0) goto out;
        if (projection.create_request.value && !native && app->turn_provider->auth == SNAG_AUTH_CHATGPT &&
            snag_context_codex_request(projection.create_request.value) < 0) goto out;
          /* The compaction request is the largest request a session sends, so it carries the same
           * session cache key as an ordinary request; a key derived per request path would place the
           * compacted history in a different cache space from the turn that produced it. */
          char compact_cache_key[SNAG_CACHE_KEY_LEN + 1u];
          snag_context_cache_key(&app->session, app->turn_provider ? app->turn_provider->name : NULL,
                                 snag_config_model_upstream(app->turn_provider, model), compact_cache_key);
          if (compact_cache_key[0] &&
              ((projection.create_request.value && snag_json_set_new(projection.create_request.value,
                    "prompt_cache_key", json_string(compact_cache_key)) < 0) ||
               (projection.count_request.value && snag_json_set_new(projection.count_request.value,
                    "prompt_cache_key", json_string(compact_cache_key)) < 0))) goto out;
        if (!projection.create_request.value || !projection.count_request.value ||
            snag_context_provider_model(app->turn_provider, model, projection.create_request.value) < 0 ||
            snag_context_provider_model(app->turn_provider, model, projection.count_request.value) < 0) {
            (void)snag_fail(error, error_size, ENOMEM, "cannot build bounded compaction provider request");
            goto out;
        }
        if (snag_json_document_measure(&projection.create_request, SNAG_CONTEXT_MAX_COMPACT) < 0 || projection.create_request.bytes == 0u ||
            snag_json_document_measure(&projection.count_request, SNAG_CONTEXT_MAX_COMPACT) < 0 ||
            projection.count_request.bytes == 0u) {
            snprintf(error, error_size, "compaction provider request exceeds 12 MiB");
            goto out;
        }
        /* A repeated request is only irreducible when it still exceeds the
         * budget: once the cut source fits, it is sendable and the loop must
         * try it instead of declaring the group irreducible. */
        if (reduced && !strcmp(prior_request, projection.create_request.sha256) &&
            projection.model_input.bytes > source_budget) {
            snprintf(error, error_size, "irreducible complete history group exceeds provider context");
            goto out;
        }
        input_tokens_bound = 0u;
        count_method = "unknown";
        stage_rc = SNAG_APP_COUNT_SKIPPED;
        if (use_exact || snag_media_request_has_images(projection.count_request.value)) {
            if (snag_app_provider_activity(app, true) < 0) goto out;
            stage_rc = snag_app_provider_count(app, projection.count_request.value, credential,
                &input_tokens_bound, &count_method, error, error_size);
            if (snag_app_provider_activity(app, false) < 0) goto out;
            if (stage_rc != 0 && stage_rc != SNAG_APP_COUNT_SKIPPED &&
                stage_rc != SNAG_PROVIDER_CONTEXT_OVERFLOW) {
                rc = stage_rc;
                goto out;
            }
            if (stage_rc == SNAG_APP_COUNT_SKIPPED) use_exact = false;
        }
        /* A known bound over the model's hard input means this request cannot
         * be sent: shrink the source first. Only an unknown count leaves the
         * attempt as built, and the provider answer decides from there.
         * A media upper bound counts as known here: the turn guard already
         * treats it that way, and sending an 11M-token compaction request to a
         * 258k-token route stalls the provider and re-runs on every resume. */
        if (stage_rc != SNAG_PROVIDER_CONTEXT_OVERFLOW &&
            !(strcmp(count_method, "unknown") != 0 &&
              app->turn_capacity.hard_input_known &&
              input_tokens_bound > app->turn_capacity.hard_input_tokens)) {
            if (snag_random_id(compact_id) < 0) {
                snprintf(error, error_size, "cryptographic compact id generation failed");
                goto out;
            }
            if (commit_rendered(app, "compaction_started",
                    json_pack("{s:s,s:s,s:s,s:s,s:I,s:s,s:s,s:s?,s:s,s:s,s:s,s:I,s:s,s:s}",
                        "capability_version", SNAJPAGENT_CAPABILITY_VERSION,
                        "compact_id", compact_id, "count_method", count_method,
                        "count_request_sha256", projection.count_request.sha256,
                        "input_tokens_bound", (json_int_t)input_tokens_bound,
                        /* The bound model keeps the durable replay contract; the
                         * compaction model names the target binding that actually
                         * produced this summary. */
                        "model", app->session.active_turn_model[0] ?
                            app->session.active_turn_model : model,
                        "compaction_model", model,
                        "predecessor_compact_id", app->session.compact_id[0] ? app->session.compact_id : NULL,
                        "profile_id", SNAJPAGENT_PROFILE_ID, "reason", reason,
                        "request_sha256", projection.create_request.sha256,
                        "source_seq", (json_int_t)projection.source_seq,
                        "source_sha256", projection.model_input.sha256,
                        "continuation_scope", continuation_scope), error, error_size) < 0) goto out;
            started = true;
            if (snag_app_provider_activity(app, true) < 0) goto out;
            stage_rc = native ? snag_app_provider_compact(app, projection.create_request.value, credential,
                    &output, error, error_size) :
                run_responses_compaction(app, projection.create_request.value, credential,
                    &output, error, error_size);
            if (snag_app_provider_activity(app, false) < 0) goto out;
            if (stage_rc == 0) {
                generated = true;
                break;
            }
            if (stage_rc == SNAG_PROVIDER_UNSUPPORTED || stage_rc == SNAG_PROVIDER_CONTEXT_OVERFLOW ||
                stage_rc == SNAG_PROVIDER_REJECTED) {
                if (commit_rendered(app, "compaction_interrupted", compaction_interrupted_data(compact_id,
                            stage_rc == SNAG_PROVIDER_UNSUPPORTED ?
                            "endpoint_unavailable" : stage_rc == SNAG_PROVIDER_CONTEXT_OVERFLOW ?
                            "context_rejected" : "error"), error, error_size) < 0) goto out;
                started = false;
            }
            /* A transport or provider failure on a large source (a gateway
             * dropping the body as "Failure when receiving data from the peer")
             * shrinks the source and retries inside this bounded loop instead of
             * failing the turn; the floor stops the budget collapsing. Only the
             * native compact route needs this: a responses-style compaction that
             * answered at all is a rejection, not a lost body. */
            bool retry_smaller = native && stage_rc < 0 && stage_rc != SNAG_PROVIDER_UNSUPPORTED &&
                source_budget > SNAG_CONTEXT_COMPACT_FLOOR;
            if (stage_rc != SNAG_PROVIDER_CONTEXT_OVERFLOW && !retry_smaller) {
                /* A rejected request fails the command: the interruption is already
                 * recorded, and the caller must report the failure (not a silent
                 * success) with the provider's own message. */
                rc = stage_rc == SNAG_PROVIDER_REJECTED ? -1 : stage_rc;
                goto out;
            }
            if (retry_smaller) {
                if (started) {
                    if (commit_rendered(app, "compaction_interrupted",
                            compaction_interrupted_data(compact_id, "error"), error, error_size) < 0) goto out;
                    started = false;
                }
                if (snag_ui_text(&app->ui, SNAG_UI_HOST,
                        "compaction request failed; retrying with a smaller source (Ctrl-C interrupts)") < 0)
                    goto out;
            }
        }
        /* Shrink bytes only after a measured rejection/count: no token conversion.
         * The walker preserves complete groups; identical irreducible input stops. */
        memcpy(prior_request, projection.create_request.sha256, sizeof(prior_request));
        reduced = true;
        source_budget = projection.model_input.bytes / 2u;
        /* Floor the halving: below this the builder cannot fit any group and
         * reports the source as irreducible instead of cutting it. The floor
         * must never raise the budget above the source that was just rejected:
         * a source already below the floor still has to shrink, or every retry
         * repeats the identical request until the loop gives up and the turn
         * ends without a compaction. */
        if (source_budget < SNAG_CONTEXT_COMPACT_FLOOR &&
            projection.model_input.bytes > SNAG_CONTEXT_COMPACT_FLOOR)
            source_budget = SNAG_CONTEXT_COMPACT_FLOOR;
        snag_context_projection_free(&projection);
    }
    if (!generated) {
        snprintf(error, error_size, "compaction input still exceeds context after eight attempts");
        goto out;
    }
    /* Design step 3: a merged summary that has grown past a quarter of the window
     * is condensed once, under the same binding, before it is measured and
     * committed. The reduce carries only the merged text and the dedupe
     * instruction, so no event re-enters the source and the covered boundary
     * never moves; a failed reduce keeps the un-reduced merge. */
    if (app->turn_capacity.hard_input_known) {
        const struct snag_input_observation *ratio = NULL;
        if (app->session.usage_anchor.valid && app->session.usage_anchor.input_tokens &&
            app->session.usage_anchor.model_input_bytes)
            ratio = &app->session.usage_anchor;
        else if (app->session.context_meter.valid && app->session.context_meter.input_tokens &&
                 app->session.context_meter.model_input_bytes)
            ratio = &app->session.context_meter;
        uint64_t per_token = ratio ? ratio->model_input_bytes / ratio->input_tokens : 0u;
        uint64_t window_bytes = per_token && app->turn_capacity.hard_input_tokens <= UINT64_MAX / per_token ?
            (uint64_t)app->turn_capacity.hard_input_tokens * per_token : 0u;
        if (window_bytes && output.bytes > window_bytes / 4u) {
            struct snag_json_document reduce_request = {0};
            struct snag_json_document reduced = {0};
            char reduce_error[256] = {0};
            int reduce_rc = snag_context_compact_reduce_request_build(&app->session, app->turn_provider,
                model, effort, output.value,
                "merge these summaries of overlapping chunks, deduplicating anything that appears twice",
                &reduce_request, reduce_error, sizeof(reduce_error));
            if (reduce_rc == 0) {
                if (snag_app_provider_activity(app, true) < 0) goto out;
                reduce_rc = native ? snag_app_provider_compact(app, reduce_request.value, credential,
                        &reduced, reduce_error, sizeof(reduce_error)) :
                    run_responses_compaction(app, reduce_request.value, credential,
                        &reduced, reduce_error, sizeof(reduce_error));
                if (snag_app_provider_activity(app, false) < 0) goto out;
                if (reduce_rc == 0 && reduced.value) {
                    snag_json_document_free(&output);
                    output = reduced;
                    memset(&reduced, 0, sizeof(reduced));
                }
            }
            snag_json_document_free(&reduce_request);
            snag_json_document_free(&reduced);
        }
    }
    output_tokens_bound = 0u;
    if (snag_context_compact_output_count_request_build(output.value,
            snag_config_model_upstream(app->turn_provider, model), &output_count, error, error_size) < 0 ||
        output_count.bytes == 0u) goto out;
    if (use_exact) {
        if (snag_app_provider_activity(app, true) < 0) goto out;
        stage_rc = snag_app_provider_count(app, output_count.value, credential,
            &output_tokens_bound, &output_count_method, error, error_size);
        if (snag_app_provider_activity(app, false) < 0) goto out;
        if (stage_rc != 0 && stage_rc != SNAG_APP_COUNT_SKIPPED &&
            stage_rc != SNAG_PROVIDER_CONTEXT_OVERFLOW) {
            rc = stage_rc;
            goto out;
        }
        if (stage_rc != 0) {
            output_tokens_bound = 0u;
            output_count_method = "unknown";
        }
    }
    if (output_tokens_bound > (uint64_t)INT64_MAX) {
        (void)snag_fail(error, error_size, EOVERFLOW, "compact output bound is too large");
        goto out;
    }
    if (commit_rendered(app, "compaction_completed", json_pack("{s:s,s:s,s:I,s:O,s:s,s:s,s:s,s:I,s:s,s:s}",
                "compact_id", compact_id, "count_method", count_method,
                "input_tokens_bound", (json_int_t)input_tokens_bound, "output", output.value,
                "output_count_method", output_count_method,
                "output_count_request_sha256", output_count.sha256, "output_sha256", output.sha256,
                "output_tokens_bound", (json_int_t)output_tokens_bound,
                "source_sha256", projection.model_input.sha256, "continuation_scope", continuation_scope),
            error, error_size) < 0) goto out;
    if (app->networked && snag_app_irc_snapshot(app, "compaction", error, error_size) < 0) goto out;
    started = false;
    if (compacted) *compacted = true;
    rc = 0;
out:
    if ((rc == 1 || rc == 2 || rc == SNAG_PROVIDER_NEW_INPUT) && started) {
        if (commit_rendered(app, "compaction_interrupted", compaction_interrupted_data(compact_id,
                    rc == 1 ? "steering" : rc == 2 ? "user" : "error"), error, error_size) < 0) rc = -1;
        else started = false;
    }
    if (rc < 0 && started && app->session.active_compact_id[0]) {
        char cleanup_error[256] = {0};
        if (commit_rendered(app, "compaction_interrupted", compaction_interrupted_data(compact_id, "error"),
                cleanup_error, sizeof(cleanup_error)) < 0) snprintf(error, error_size, "%s", cleanup_error);
    }
    snag_json_document_free(&output);
    snag_json_document_free(&output_count);
    snag_context_projection_free(&projection);
    snag_credential_clear(&owned_credential);
    return rc;
}

static int
run_compaction(struct app_state *app, const char *reason, bool active_prefix,
               const struct snag_credential *credential, bool *compacted, char *error, size_t error_size)
{
    /* A provider that keeps aborting the summary request used to hold the
     * session: every turn retry re-ran the same compaction (13 attempts over
     * 1.5 hours in one report). Bound consecutive failures; a completed
     * compaction, new operator input or a manual /compact resets the count. */
    if (app->history_orientation < SNAG_HISTORY_ORIENTATION_COMPACT)
        app->history_orientation = SNAG_HISTORY_ORIENTATION_COMPACT;
    if (app->compaction_failures >= 8u) {
        app->compaction_bounded = true;
        return snag_fail(error, error_size, EPROTO,
            "compaction failed %u times in a row; previous context retained; "
            "new input or /compact can retry", app->compaction_failures);
    }
    int rc = run_compaction_attempt(app, reason, active_prefix, true,
                                    credential, compacted, error, error_size);
    if (rc == SNAG_PROVIDER_UNSUPPORTED) {
        if (snag_ui_text(&app->ui, SNAG_UI_WARNING,
            "native compaction unavailable; compacting through Responses") < 0) return -1;
        if (error_size) error[0] = '\0';
        rc = run_compaction_attempt(app, reason, active_prefix, false,
                                    credential, compacted, error, error_size);
    }
    if (rc == 0 && compacted && *compacted) {
        app->compaction_failures = 0u;
        app->compaction_bounded = false;
    } else if (rc < 0) {
        ++app->compaction_failures;
    }
    return rc;
}

int
snag_app_compact_requested(struct app_state *app, char *error, size_t error_size)
{
    bool active = app->session.active_turn, compacted = false;
    enum snag_policy_stop policy = app->turn_policy_stopped;
    int rc;

    /* A new idle operation must not inherit cancellation of the previous turn. */
    if (!active && !app->input_closed) {
        app->interrupt_requested = false;
        app->steering_requested = false;
    }
    if (snag_ui_text(&app->ui, SNAG_UI_HOST, "Compacting context; Ctrl-C interrupts") < 0) return -1;
    /* An explicit /compact is a fresh operator decision: clear the consecutive
     * failure bound so the attempt runs. */
    app->compaction_failures = 0u;
    app->compaction_bounded = false;
    rc = run_compaction(app, "manual", active, NULL, &compacted, error, error_size);
    app->turn_policy_stopped = policy;
    if (rc < 0 && snag_ui_text(&app->ui, SNAG_UI_WARNING,
            "compaction failed; previous context retained; /compact retries") < 0) return -1;
    if (rc > 0 && snag_ui_text(&app->ui, SNAG_UI_WARNING,
            "compaction interrupted; previous context retained") < 0) return -1;
    if (rc == 0 && active && !compacted) return SNAG_APP_COMPACT_DEFERRED;
    return rc;
}

int
snag_app_compact_after_turn(struct app_state *app, uint64_t input_tokens_bound, const char *count_method,
                           char *error, size_t error_size)
{
    uint64_t threshold;

    if (!app || !app->config || !app->turn_provider || !count_method_valid(count_method))
        return snag_fail(error, error_size, EINVAL, "invalid proactive compaction state");
    threshold = snag_model_compact_threshold(app->turn_provider, &app->turn_capacity);
    if (!snag_app_measured_input(app, &input_tokens_bound) || !threshold || input_tokens_bound < threshold)
        return 0;
    return run_compaction(app, "proactive", false, NULL, NULL, error, error_size);
}

int
snag_app_compact_before_response(struct app_state *app, const struct snag_credential *credential,
                                uint64_t input_tokens_bound, const char *count_method, bool *compacted,
                                char *error, size_t error_size)
{
    if (compacted) *compacted = false;
    if (!app || !app->config || !app->turn_provider || !compacted || !count_method_valid(count_method))
        return snag_fail(error, error_size, EINVAL, "invalid pre-response compaction state");
    {
        uint64_t threshold = snag_model_compact_threshold(app->turn_provider, &app->turn_capacity);
        uint64_t measured = input_tokens_bound;
        /* A local media upper bound is a known figure for the hard guard: the
         * request either fits the model window or the provider rejects it, so
         * compacting before sending is the only safe order. */
        bool known_bound = strcmp(count_method, "exact") == 0 ||
            strcmp(count_method, "media_upper_bound") == 0;
        bool measured_known = strcmp(count_method, "exact") == 0 || snag_app_measured_input(app, &measured);
        /* A session that is already over its window never produces a successful
         * usage figure, so the proactive path could never fire for it. Let a
         * known bound (exact or media) drive the same threshold check. */
        if (!measured_known && known_bound) {
            measured = input_tokens_bound;
            measured_known = true;
        }
        /* The window can shrink when the model or provider changes, and a route
         * without exact counting reports no bound at all: an input the session
         * already measured above the hard window must compact in stages rather
         * than be sent whole (the resume failure fed 8.3 MB to a 258k window). */
        bool over_hard = app->turn_capacity.hard_input_known &&
            ((known_bound && input_tokens_bound > app->turn_capacity.hard_input_tokens) ||
             (measured_known && measured > app->turn_capacity.hard_input_tokens));
        bool over_proactive = measured_known && threshold && measured >= threshold;
        int rc;

        if (!over_hard && !over_proactive) return 0;
        rc = run_compaction(app, over_hard ? "hard_budget" : "proactive",
                            true, credential, compacted, error, error_size);
        if (rc != 0) return rc;
        if (over_hard && !*compacted) return snag_fail(error, error_size, EOVERFLOW,
                "context input count %llu (%s) exceeds hard budget %llu; no complete older turn can be compacted",
                (unsigned long long)input_tokens_bound, count_method,
                (unsigned long long)app->turn_capacity.hard_input_tokens);
        return 0;
    }
}

int
snag_app_compact_after_capacity_rejection( struct app_state *app, const struct snag_credential *credential,
    bool *compacted, char *error, size_t error_size)
{
    if (compacted) *compacted = false;
    if (!app || !credential || !compacted)
        return snag_fail(error, error_size, EINVAL, "invalid provider-rejection compaction state");
    return run_compaction(app, "provider_rejection", true, credential, compacted, error, error_size);
}
