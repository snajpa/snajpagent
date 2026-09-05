/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"

#include "context.h"
#include "json.h"
#include "secret.h"
#include "snajpagent.h"
#include "wire.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

json_t *
snag_app_preference_changed_data(const char *old_key, const char *old_value,
                        const char *new_key, const char *new_value)
{
    return json_pack("{s:s,s:s}", new_key, new_value, old_key, old_value);
}

json_t *
snag_app_model_selection_changed_data(
    const char *old_provider, const char *new_provider,
    const char *old_model, const char *new_model,
    const char *old_effort, const char *new_effort)
{
    return json_pack("{s:s,s:s,s:s,s:s,s:s,s:s}",
        "new_effort", new_effort, "new_model", new_model,
        "new_provider", new_provider, "old_effort", old_effort,
        "old_model", old_model, "old_provider", old_provider);
}

static json_t *
turn_config(const struct app_state *app)
{
    json_t *config = json_pack("{s:s,s:s,s:n,s:s,s:s,s:s,s:i,s:i,s:i,s:i,s:b}",
        "capability_version", SNAJPAGENT_CAPABILITY_VERSION,
        "effort", app->turn_effort, "max_output_tokens", "model", app->turn_model,
        "provider", app->turn_provider->name, "profile_id", SNAJPAGENT_PROFILE_ID,
        "prompt_schema", 1, "replay_schema", 1, "tool_schema", 1,
        "max_parallel_commands", (int)app->config->max_parallel_commands,
        "parallel_tool_calls", app->turn_provider->parallel_tool_calls);

    if (config && app->turn_capacity.max_output_tokens &&
        snag_json_set_new(config, "max_output_tokens",
            json_integer((json_int_t)app->turn_capacity.max_output_tokens)) < 0) {
        json_decref(config);
        return NULL;
    }
    return config;
}

json_t *
snag_app_turn_started_data(const struct app_state *app, const char *prompt,
                  const char *turn_id, const struct snag_queued_turn *queued,
                  bool goal_turn, bool read_only)
{
    json_t *instructions = snag_instructions_metadata_json(&app->turn_instructions);
    json_t *config = turn_config(app);
    json_t *data = json_pack("{s:O,s:s,s:O,s:b,s:s?,s:n,s:s,s:s,s:I,s:s}",
        "config", config, "input_kind", goal_turn ? "goal" : queued ? "queued" : "direct",
        "instructions", instructions, "read_only", read_only,
        "queue_id", queued ? queued->queue_id : NULL, "queue_seq",
        "text", prompt, "turn_id", turn_id,
        "turn_number", (json_int_t)(app->session.turn_count + 1u),
        "workspace", app->session.workspace);

    if (data && queued &&
        snag_json_set_new(data, "queue_seq", json_integer((json_int_t)queued->seq)) < 0) {
        json_decref(data);
        data = NULL;
    }
    json_decref(config);
    json_decref(instructions);
    return data;
}

static const char *
irc_kind_name(enum snag_irc_event_kind kind)
{
    static const char *const names[] = {
        "connected", "disconnected", "join", "part", "quit", "nick",
        "message", "notice", "topic", "mode", "history_ready"
    };

    return (unsigned int)kind < sizeof(names) / sizeof(names[0]) ?
           names[kind] : "unknown";
}

static int
irc_kind_parse(const char *name, enum snag_irc_event_kind *kind)
{
    for (unsigned int i = 0u; i <= (unsigned int)SNAG_IRC_HISTORY_READY; ++i)
        if (strcmp(name, irc_kind_name((enum snag_irc_event_kind)i)) == 0) {
            *kind = (enum snag_irc_event_kind)i;
            return 0;
        }
    errno = EINVAL;
    return -1;
}

static json_t *
irc_event_data(const struct snag_irc_event *event)
{
    return json_pack("{s:s,s:b,s:s,s:b,s:s,s:b,s:s,s:s,s:I}",
        "endpoint", event->endpoint, "historical", event->historical,
        "kind", irc_kind_name(event->kind), "local", event->local,
        "nick", event->nick, "op", event->op, "room", event->room,
        "text", event->text, "timestamp_ms", (json_int_t)event->timestamp_ms);
}

static int
append_pending(struct snag_buf *pending, const char *text, size_t len)
{
    static const char omitted[] =
        "[older coalesced IRC entries omitted at the input bound]\n";

    if (len > pending->max - sizeof(omitted)) {
        errno = EOVERFLOW;
        return -1;
    }
    if (pending->len > pending->max - len) {
        snag_buf_reset(pending);
        if (snag_buf_append(pending, omitted, sizeof(omitted) - 1u) < 0)
            return -1;
    }
    return snag_buf_append(pending, text, len);
}

static int
append_irc_projection(struct snag_buf *pending,
                      const struct snag_irc_event *event)
{
    struct snag_buf line;
    char when[32u];
    time_t seconds = (time_t)(event->timestamp_ms / 1000u);
    struct tm tm;
    int rc = -1;

    if (!gmtime_r(&seconds, &tm) ||
        strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0)
        memcpy(when, "1970-01-01T00:00:00Z", 21u);
    snag_buf_init(&line, 2048u);
    if (snag_buf_printf(&line,
            "[IRC endpoint=%s room=%s time=%s event=%s sender=%s operator=%s]\n%s\n",
            event->endpoint, event->room, when, irc_kind_name(event->kind),
            event->nick[0] ? event->nick : "server",
            event->op ? "true" : "false", event->text) < 0 ||
        append_pending(pending, (const char *)line.data, line.len) < 0)
        goto out;
    rc = 0;
out:
    snag_buf_free(&line);
    return rc;
}

int
snag_app_irc_snapshot(struct app_state *app, const char *reason,
                     char *error, size_t error_size)
{
    struct snag_buf snapshot;
    json_t *data = NULL;
    int rc = -1;

    if (!app || !app->irc || !reason) {
        errno = EINVAL;
        return -1;
    }
    snag_buf_init(&snapshot, SNAG_MAX_IRC_SNAPSHOT);
    if (snag_irc_snapshot(app->irc, &snapshot, error, error_size) < 0 ||
        snag_buf_terminate(&snapshot) < 0 || !(data = json_object()) ||
        snag_json_set_new(data, "reason", json_string(reason)) < 0 ||
        snag_json_set_new(data, "text",
                         json_string((const char *)snapshot.data)) < 0 ||
        snag_json_set_new(data, "timestamp_ms",
                         json_integer((json_int_t)snag_time_ms())) < 0)
        goto out;
    if (snag_app_commit_event(app, "irc_snapshot", data,
                             error, error_size) < 0) {
        data = NULL;
        goto out;
    }
    data = NULL;
    rc = 0;
out:
    if (data)
        json_decref(data);
    snag_buf_free(&snapshot);
    if (rc < 0 && error_size && !error[0])
        (void)snprintf(error, error_size, "cannot retain IRC room snapshot");
    return rc;
}

int
snag_app_irc_event(void *opaque, const struct snag_irc_event *event)
{
    struct app_state *app = opaque;
    char error[256] = {0};
    bool chat;
    bool own_agent;
    bool local_operator;
    bool urgent;

    if (!app || !event)
        return -1;
    if (snag_app_commit_event(app, "irc_event", irc_event_data(event),
                             error, sizeof(error)) < 0)
        return -1;
    if (snag_ui_networked(&app->ui, app->networked,
                         snag_irc_model_nick(app->irc)) < 0)
        return -1;
    if (snag_ui_irc_event(&app->ui, event) < 0)
        return -1;
    chat = event->kind == SNAG_IRC_MESSAGE || event->kind == SNAG_IRC_NOTICE;
    own_agent = event->local &&
        strcmp(event->nick, snag_irc_model_nick(app->irc)) == 0;
    local_operator = event->local &&
        strcmp(event->nick, snag_irc_operator_nick(app->irc)) == 0;
    if (own_agent) {
        if (app->session.active_turn && event->kind == SNAG_IRC_MESSAGE)
            app->irc_turn_replied = true;
        return 0;
    }
    if (event->kind == SNAG_IRC_HISTORY_READY) {
        if (snag_app_irc_snapshot(app, "join", error, sizeof(error)) < 0)
            return -1;
    }
    if (event->historical)
        return 0;
    urgent = chat && snag_irc_mentions_agent(app->irc, event->endpoint, event->text);
    if (append_irc_projection(urgent ? &app->irc_urgent :
                                      &app->irc_background, event) < 0)
        return -1;
    if (urgent)
        app->irc_urgent_local_operator |= local_operator;
    else if (!app->irc_background_since_ms)
        app->irc_background_since_ms = snag_time_ms();
    return 0;
}

int
snag_app_irc_trace(void *opaque, unsigned int level, char direction,
                  const char *endpoint, const char *text, size_t len)
{
    struct app_state *app = opaque;
    struct snag_buf safe;
    char label[384u];
    int rc = -1;

    if (!app || !endpoint || !text || (level != 5u && level != 6u) ||
        (direction != '<' && direction != '>')) {
        errno = EINVAL;
        return -1;
    }
    if (!snag_ui_enabled(&app->ui, level == 6u ? SNAG_PRESENT_WIRE : SNAG_PRESENT_PROTOCOL))
        return 0;
    snag_buf_init(&safe, 4u * SNAG_IRC_LINE_MAX);
    for (size_t i = 0u; i < len; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20u || c == 0x7fu) {
            if (snag_buf_printf(&safe, "\\x%02X", (unsigned int)c) < 0)
                goto out;
        } else if (snag_buf_putc(&safe, c) < 0) {
            goto out;
        }
    }
    if (level == 6u) {
        struct snag_buf line;

        snag_buf_init(&line, 4u * SNAG_IRC_LINE_MAX +
                            SNAG_CONFIG_IRC_ENDPOINT_MAX + 8u);
        if (snag_buf_printf(&line, "IRC [%s] ", endpoint) < 0 ||
            snag_buf_append(&line, safe.data, safe.len) < 0) {
            snag_buf_free(&line);
            goto out;
        }
        rc = snag_ui_transport(&app->ui, direction,
                                  (const char *)line.data, line.len);
        snag_buf_free(&line);
    } else {
        int n = snprintf(label, sizeof(label), "irc.command %c %s",
                         direction, endpoint);
        if (n < 0 || (size_t)n >= sizeof(label)) {
            errno = EOVERFLOW;
            goto out;
        }
        rc = snag_ui_protocol(&app->ui, label,
                                 (const char *)safe.data, safe.len);
    }
out:
    snag_buf_free(&safe);
    return rc;
}

int
snag_app_irc_flush_urgent(struct app_state *app,
                         char *error, size_t error_size)
{
    char steering_id[SNAG_ID_HEX_LEN + 1u];
    bool local_operator;

    if (!app || !app->session.active_turn || !app->irc_urgent.len)
        return 0;
    if (snag_buf_terminate(&app->irc_urgent) < 0 ||
        snag_random_id(steering_id) < 0)
        return -1;
    local_operator = app->irc_urgent_local_operator;
    if (snag_app_commit_event(app, "steering_added",
            snag_app_steering_added_data(app->session.active_turn_id,
                steering_id, (const char *)app->irc_urgent.data),
            error, error_size) < 0)
        return -1;
    snag_buf_reset(&app->irc_urgent);
    app->irc_urgent_local_operator = false;
    if (local_operator) {
        app->irc_turn_local_operator = true;
        app->irc_turn_replied = false;
    }
    return 0;
}

char *
snag_app_irc_take_pending(struct app_state *app,
                         bool *local_operator, bool force_background)
{
    struct snag_buf *source;
    char *copy;

    if (local_operator)
        *local_operator = false;
    if (!app)
        return NULL;
    if (app->irc_urgent.len) {
        source = &app->irc_urgent;
        if (local_operator)
            *local_operator = app->irc_urgent_local_operator;
    } else if (app->irc_background.len &&
               (force_background ||
                snag_time_ms() - app->irc_background_since_ms >= 100u)) {
        source = &app->irc_background;
    } else {
        return NULL;
    }
    if (snag_buf_terminate(source) < 0)
        return NULL;
    copy = snag_strdup_checked((const char *)source->data,
                              SNAG_MAX_STEERING_TEXT);
    if (!copy)
        return NULL;
    snag_buf_reset(source);
    if (source == &app->irc_urgent)
        app->irc_urgent_local_operator = false;
    else
        app->irc_background_since_ms = 0u;
    return copy;
}

static int
restore_irc_event(void *opaque, uint64_t seq, const char *type,
                  const json_t *data, char *error, size_t error_size)
{
    struct app_state *app = opaque;
    struct snag_irc_event event;
    const char *value;
    uint64_t timestamp_ms;

    (void)seq;
    (void)error;
    (void)error_size;
    if (strcmp(type, "irc_event") != 0)
        return 0;
    memset(&event, 0, sizeof(event));
    value = snag_json_string(data, "kind");
    if (!value || irc_kind_parse(value, &event.kind) < 0 ||
        snag_json_integer_u64(data, "timestamp_ms", &timestamp_ms) < 0)
        return -1;
    event.timestamp_ms = timestamp_ms;
#define RESTORE_FIELD(member, key) do { \
    value = snag_json_string(data, key); \
    if (!value || snprintf(event.member, sizeof(event.member), "%s", value) < 0 || \
        strlen(value) >= sizeof(event.member)) return -1; \
} while (0)
    RESTORE_FIELD(endpoint, "endpoint");
    RESTORE_FIELD(room, "room");
    RESTORE_FIELD(nick, "nick");
    RESTORE_FIELD(text, "text");
#undef RESTORE_FIELD
    event.historical = json_is_true(json_object_get(data, "historical"));
    event.local = json_is_true(json_object_get(data, "local"));
    event.op = json_is_true(json_object_get(data, "op"));
    return snag_irc_restore_event(app->irc, &event);
}

int
snag_app_irc_restore(struct app_state *app, char *error, size_t error_size)
{
    if (!app || !app->irc) {
        errno = EINVAL;
        return -1;
    }
    return snag_session_each_event(&app->session, restore_irc_event, app,
                                  error, error_size);
}

json_t *
snag_app_steering_snapshot(const struct snag_session *session)
{
    json_t *array = json_array();

    if (!array)
        return NULL;
    for (size_t i = 0; i < session->pending_steering_count; ++i) {
        json_t *item = json_object();
        if (!item ||
            snag_json_set_new(item, "id",
                             json_string(session->pending_steering[i].steering_id)) < 0 ||
            snag_json_set_new(item, "text",
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
snag_app_request_build(struct app_state *app, const json_t *steering,
                       unsigned int cycle,
                       const struct snag_credential *credential,
                       const char *provider_source_sha256,
                       struct snag_context_projection *projection,
                       const char **count_method, struct snag_buf *request_body,
                       char *error, size_t error_size)
{
    uint64_t anchored_bound = 0u;
    int rc = snag_context_build(&app->session, app->turn_model, app->turn_effort,
        cycle, steering, app->turn_capacity.max_output_tokens,
        app->turn_capacity.max_output_tokens, app->config,
        &app->turn_instructions, projection, error, error_size);

    if (rc < 0)
        return -1;
    *count_method = "qualified_upper_bound";
    rc = snag_context_usage_anchor_bound(&app->session, app->turn_provider->name,
        app->turn_model, app->turn_effort, provider_source_sha256, projection,
        &anchored_bound);
    if (rc < 0) {
        snag_errorf(error, error_size, "cannot evaluate provider-usage anchor");
        return -1;
    }
    if (rc == 1) {
        projection->input_tokens_bound = anchored_bound;
        *count_method = "anchored_upper_bound";
    } else if (app->turn_capacity.observed_tokens_per_million_bytes) {
        projection->input_tokens_bound = snag_context_input_estimate(
            projection->model_input_bytes,
            app->turn_capacity.observed_tokens_per_million_bytes);
        *count_method = "statistical_upper_estimate";
    }
    rc = 0;
    if (snag_ui_enabled(&app->ui, SNAG_PRESENT_PROTOCOL)) {
        struct snag_buf encoded;
        struct snag_secret_set secrets;

        snag_buf_init(request_body, SNAG_WIRE_BODY_MAX);
        snag_buf_init(&encoded, SNAG_WIRE_BODY_MAX);
        snag_secret_set_build(&secrets, app->config, credential);
        if (projection->create_request_bytes > SNAG_WIRE_BODY_MAX ||
            snag_json_canonical(projection->create_request, &encoded) < 0 ||
            snag_wire_json_redact(encoded.data, encoded.len, &secrets.wire,
                                 request_body, error, error_size) < 0) {
            snag_buf_reset(request_body);
            rc = snag_buf_printf(request_body,
                "<request body omitted; bytes=%zu; sha256=%s>\n",
                projection->create_request_bytes, projection->request_sha256);
        }
        snag_buf_free(&encoded);
    }
    /* Only the request views and accounting facts survive into the cycle. */
    json_decref(projection->model_input);
    projection->model_input = NULL;
    return rc;
}
json_t *
snag_app_response_started_data(const struct app_state *app,
                               const char *turn_id, const char *response_id,
                               unsigned int cycle,
                               const struct snag_context_projection *projection,
                               const char *count_method,
                               const char *provider_source_sha256,
                               const json_t *steering)
{
    const struct snag_model_capacity *capacity = &app->turn_capacity;
    const char *compact_id = app->session.compact_id;
    const char *baseline = strcmp(count_method, "anchored_upper_bound") == 0 ?
        app->session.usage_anchor_model_input_sha256 : NULL;
    json_t *ids = json_array();
    json_t *data = NULL;

    if (!ids || !json_is_array(steering) ||
        !snag_hex_is_lower(provider_source_sha256, SNAG_SHA256_HEX_LEN))
        goto out;
    for (size_t i = 0; i < json_array_size(steering); ++i) {
        const char *id = snag_json_string(json_array_get(steering, i), "id");
        if (!id || json_array_append_new(ids, json_string(id)) < 0)
            goto out;
    }
    data = json_pack(
        "{s:s?,s:s,s:s?,s:s,s:s,s:s,s:I,s:s,s:n,s:I,s:s,s:I,s:s,"
        "s:s,s:s,s:s,s:I,s:I,s:s,s:n,s:s,s:s,s:b,s:O,s:s}",
        "baseline_sha256", baseline,
        "capability_version", SNAJPAGENT_CAPABILITY_VERSION,
        "compact_id", *compact_id ? compact_id : NULL,
        "count_method", count_method,
        "count_request_sha256", projection->count_request_sha256,
        "capacity_source", snag_capacity_source_name(capacity->source),
        "cycle", (json_int_t)cycle, "effort", app->turn_effort,
        "hard_input_tokens", "input_tokens_bound", (json_int_t)projection->input_tokens_bound,
        "model", app->turn_model, "model_input_bytes", (json_int_t)projection->model_input_bytes,
        "model_input_sha256", projection->model_input_sha256,
        "profile_id", SNAJPAGENT_PROFILE_ID, "provider", app->turn_provider->name,
        "provider_source_sha256", provider_source_sha256,
        "request_input_bytes", (json_int_t)projection->request_input_bytes,
        "request_input_count", (json_int_t)projection->request_input_count,
        "request_input_sha256", projection->request_input_sha256,
        "requested_output_tokens", "request_sha256", projection->request_sha256,
        "response_id", response_id, "source_bound", capacity->source_bound,
        "steering_ids", ids, "turn_id", turn_id);
    if (data &&
        ((capacity->hard_input_known &&
          snag_json_set_new(data, "hard_input_tokens",
              json_integer((json_int_t)capacity->hard_input_tokens)) < 0) ||
         (capacity->max_output_tokens &&
          snag_json_set_new(data, "requested_output_tokens",
              json_integer((json_int_t)capacity->max_output_tokens)) < 0))) {
        json_decref(data);
        data = NULL;
    }
out:
    json_decref(ids);
    return data;
}

json_t *
snag_app_response_capacity_rejected_data(
    const char *turn_id, const char *response_id, unsigned int cycle,
    const char *request_hash, const struct snag_provider_failure *failure,
    const struct snag_model_capacity *capacity,
    const char *provider_source_sha256)
{
    json_t *data = json_object();
    uint64_t safety_ceiling = 0u;

    if (failure && capacity)
        safety_ceiling = snag_capacity_safety_ceiling(
            failure->context_limit_tokens, failure->requested_input_tokens,
            capacity->max_output_tokens);

    if (!data || !turn_id || !response_id || !request_hash || !failure ||
        !capacity || !provider_source_sha256 ||
        !snag_hex_is_lower(provider_source_sha256, SNAG_SHA256_HEX_LEN) ||
        snag_json_set_new(data, "code", json_string(failure->code)) < 0 ||
        snag_json_set_new(data, "context_limit_tokens",
            failure->context_limit_tokens ?
                json_integer((json_int_t)failure->context_limit_tokens) :
                json_null()) < 0 ||
        snag_json_set_new(data, "cycle", json_integer((json_int_t)cycle)) < 0 ||
        snag_json_set_new(data, "message", json_string(failure->message)) < 0 ||
        snag_json_set_new(data, "observed_hard_input_tokens",
            safety_ceiling ?
                json_integer((json_int_t)safety_ceiling) : json_null()) < 0 ||
        snag_json_set_new(data, "provider_source_sha256",
                         json_string(provider_source_sha256)) < 0 ||
        snag_json_set_new(data, "request_sha256",
                         json_string(request_hash)) < 0 ||
        snag_json_set_new(data, "requested_input_tokens",
            failure->requested_input_tokens ?
                json_integer((json_int_t)failure->requested_input_tokens) :
                json_null()) < 0 ||
        snag_json_set_new(data, "response_id", json_string(response_id)) < 0 ||
        snag_json_set_new(data, "turn_id", json_string(turn_id)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
}

json_t *
snag_app_response_completed_data(const char *turn_id, const char *response_id,
                        unsigned int cycle,
                        const struct snag_response_graph *graph)
{
    json_t *items = snag_response_graph_json(graph);
    json_t *usage = snag_response_usage_json(&graph->usage);
    json_t *data = json_pack("{s:I,s:O,s:s,s:s,s:s,s:s,s:O}",
        "cycle", (json_int_t)cycle, "items", items,
        "provider_response_id", graph->provider_response_id,
        "response_id", response_id, "status", "completed",
        "turn_id", turn_id, "usage", usage);
    json_decref(items);
    json_decref(usage);
    return data;
}

json_t *
snag_app_response_output_correction_data(const char *turn_id,
                                        const char *response_id,
                                        unsigned int cycle,
                                        const char *correction_id,
                                        const char *text,
                                        json_t *partial_public)
{
    json_t *data = json_pack("{s:s,s:I,s:O,s:s,s:s,s:s}",
        "correction_id", correction_id, "cycle", (json_int_t)cycle,
        "partial_public", partial_public, "response_id", response_id,
        "text", text, "turn_id", turn_id);
    json_decref(partial_public);
    return data;
}

json_t *
snag_app_turn_completed_data(const char *turn_id, const char *response_id,
                    const char *item_id)
{
    return json_pack("{s:s,s:s,s:s}", "final_item_id", item_id,
        "final_response_id", response_id, "turn_id", turn_id);
}

json_t *
snag_app_steering_added_data(const char *turn_id, const char *steering_id,
                    const char *text)
{
    return json_pack("{s:s,s:s,s:s}", "steering_id", steering_id,
        "text", text, "turn_id", turn_id);
}

json_t *
snag_app_future_turn_queued_data(const char *turn_id, const char *queue_id,
                        const char *text, bool read_only)
{
    return json_pack("{s:b,s:s,s:s,s:s}", "read_only", read_only,
        "queue_id", queue_id, "text", text, "while_turn_id", turn_id);
}

json_t *
snag_app_future_turn_edited_data(const char *queue_id, const char *text,
                               bool read_only)
{
    return json_pack("{s:b,s:s,s:s}", "read_only", read_only,
        "queue_id", queue_id, "text", text);
}

json_t *
snag_app_future_turn_cancelled_data(const struct snag_session *session,
                           const bool remove[SNAG_MAX_PENDING_TURNS])
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
        int rc = snag_json_set_new(data, "queue_ids", ids);
        ids = NULL;
        if (rc < 0)
            goto fail;
    }
    if (snag_json_set_new(data, "reason", json_string("user")) < 0)
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
snag_app_response_interrupted_data(const char *turn_id, const char *response_id,
                          unsigned int cycle, const char *origin,
                          const char *reason, json_t *partial_public)
{
    json_t *partial = partial_public ? partial_public : json_array();
    json_t *data = json_pack("{s:I,s:s,s:O,s:s,s:s,s:s}",
        "cycle", (json_int_t)cycle, "origin", origin, "partial_public", partial,
        "reason", reason, "response_id", response_id, "turn_id", turn_id);
    json_decref(partial);
    return data;
}

json_t *
snag_app_turn_interrupted_data(const char *turn_id, const char *origin, const char *reason)
{
    return json_pack("{s:s,s:s,s:s}", "origin", origin,
        "reason", reason, "turn_id", turn_id);
}

json_t *
snag_app_response_failed_data(const char *turn_id, const char *response_id,
                     unsigned int cycle, const char *class_name,
                     const char *message, json_t *partial_public,
                     unsigned int retry_count)
{
    json_t *partial = partial_public ? partial_public : json_array();
    json_t *data = json_pack("{s:s,s:I,s:s,s:O,s:s,s:I,s:s}",
        "class", class_name, "cycle", (json_int_t)cycle, "message", message,
        "partial_public", partial, "response_id", response_id,
        "retry_count", (json_int_t)retry_count, "turn_id", turn_id);
    json_decref(partial);
    return data;
}

json_t *
snag_app_turn_failed_data(const char *turn_id, const char *class_name, const char *message)
{
    return json_pack("{s:s,s:s,s:s}", "class", class_name,
        "message", message, "turn_id", turn_id);
}

json_t *
snag_app_tool_started_data(const char *turn_id, const char *call_id,
                  const char *action_sha256, const char *workspace)
{
    return json_pack("{s:s,s:s,s:s,s:s}", "action_sha256", action_sha256,
        "call_id", call_id, "resolved_workdir", workspace, "turn_id", turn_id);
}

int
snag_app_tool_output(void *opaque, const char *handle, unsigned int stream,
                     uint64_t offset, const void *bytes, size_t len)
{
    struct app_state *app = opaque;
    struct snag_buf encoded;
    char error[256] = {0};
    bool utf8 = snag_utf8_valid(bytes, len, true);
    json_t *event;
    int rc = -1;
    snag_buf_init(&encoded, 32768u);
    if (!utf8 && snag_base64_append(&encoded, bytes, len) < 0)
        goto out;
    event = json_pack("{s:s,s:s,s:i,s:I,s:s,s:s%}",
        "turn_id", app->session.active_turn_id, "handle", handle,
        "stream", (int)stream, "offset", (json_int_t)offset,
        "encoding", utf8 ? "utf8" : "base64",
        "data", (const char *)(utf8 ? bytes : (const void *)encoded.data),
        utf8 ? len : encoded.len);
    if (event)
        rc = snag_app_commit_event(app, "process_output", event, error, sizeof(error));
out:
    snag_buf_free(&encoded);
    return rc;
}

struct process_read_range {
    const char *handle;
    unsigned int stream;
    uint64_t from, to, seen;
    struct snag_buf *out;
};

static int
read_process_chunk(void *opaque, uint64_t seq, const char *type,
                    const json_t *data, char *error, size_t error_size)
{
    struct process_read_range *read = opaque;
    struct snag_buf bytes;
    uint64_t stream, offset;
    int rc = -1;
    (void)seq;
    (void)error;
    (void)error_size;
    if (strcmp(type, "process_output") ||
        !snag_json_string(data, "handle") ||
        strcmp(snag_json_string(data, "handle"), read->handle))
        return 0;
    if (snag_json_integer_u64(data, "stream", &stream) < 0 ||
        snag_json_integer_u64(data, "offset", &offset) < 0)
        return -1;
    if (stream != read->stream)
        return 0;
    snag_buf_init(&bytes, 16384u);
    if (snag_process_output_decode(data, &bytes) < 0 || offset > UINT64_MAX - bytes.len)
        goto out;
    uint64_t end = offset + bytes.len;
    uint64_t from = offset > read->from ? offset : read->from;
    uint64_t to = end < read->to ? end : read->to;
    if (from < to) {
        if (from != read->from + read->seen)
            goto out;
        read->seen += to - from;
        uint64_t ranges[4] = {read->from, read->to, read->to, read->to};
        if (read->to - read->from > read->out->max) {
            ranges[1] = read->from + read->out->max / 2u;
            ranges[2] = read->to - (read->out->max - read->out->max / 2u);
        }
        for (unsigned int i = 0u; i < 4u; i += 2u) {
            uint64_t a = from > ranges[i] ? from : ranges[i];
            uint64_t b = to < ranges[i + 1u] ? to : ranges[i + 1u];
            if (a < b && snag_buf_append(read->out, bytes.data + (size_t)(a - offset),
                                        (size_t)(b - a)) < 0)
                goto out;
        }
    }
    rc = 0;
out:
    snag_buf_free(&bytes);
    return rc;
}

int
snag_app_tool_read(void *opaque, const char *handle, unsigned int stream,
                   uint64_t from, uint64_t to, struct snag_buf *out)
{
    struct app_state *app = opaque;
    struct snag_process_state *process = snag_session_process(&app->session, handle);
    struct process_read_range read = {.handle = handle, .stream = stream,
        .from = from, .to = to, .out = out};
    char error[256] = {0};
    if (!process || from > to ||
        snag_session_each_event_since(&app->session, process, read_process_chunk,
                                      &read, error, sizeof(error)) < 0)
        return -1;
    return read.seen == to - from ? 0 : -1;
}

/* Takes ownership of result, including on failure. */
json_t *
snag_app_tool_finished_data(const char *turn_id, const char *call_id, json_t *result)
{
    json_t *data = json_pack("{s:s,s:O,s:s}",
        "call_id", call_id, "result", result, "turn_id", turn_id);
    json_decref(result);
    return data;
}
/* Takes ownership of result, including on failure. */
json_t *
snag_app_process_closed_data(const char *turn_id, const char *handle,
                            const char *cause, json_t *result)
{
    json_t *data = json_pack("{s:s,s:s,s:O,s:s}",
        "cause", cause, "handle", handle, "result", result, "turn_id", turn_id);
    json_decref(result);
    return data;
}
