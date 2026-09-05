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
    json_t *data = json_object();

    if (!data ||
        snag_json_set_new(data, new_key, json_string(new_value)) < 0 ||
        snag_json_set_new(data, old_key, json_string(old_value)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
}

json_t *
snag_app_model_selection_changed_data(
    const char *old_provider, const char *new_provider,
    const char *old_model, const char *new_model,
    const char *old_effort, const char *new_effort)
{
    json_t *data = json_object();

    if (!data ||
        snag_json_set_new(data, "new_effort", json_string(new_effort)) < 0 ||
        snag_json_set_new(data, "new_model", json_string(new_model)) < 0 ||
        snag_json_set_new(data, "new_provider", json_string(new_provider)) < 0 ||
        snag_json_set_new(data, "old_effort", json_string(old_effort)) < 0 ||
        snag_json_set_new(data, "old_model", json_string(old_model)) < 0 ||
        snag_json_set_new(data, "old_provider", json_string(old_provider)) < 0) {
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
        snag_json_set_new(config, "capability_version",
                         json_string(SNAJPAGENT_CAPABILITY_VERSION)) < 0 ||
        snag_json_set_new(config, "effort", json_string(app->turn_effort)) < 0 ||
        snag_json_set_new(config, "max_output_tokens",
            app->turn_capacity.max_output_known ?
                json_integer((json_int_t)app->turn_capacity.max_output_tokens) :
                json_null()) < 0 ||
        snag_json_set_new(config, "model", json_string(app->turn_model)) < 0 ||
        snag_json_set_new(config, "provider",
                         json_string(app->turn_provider->name)) < 0 ||
        snag_json_set_new(config, "profile_id",
                         json_string(SNAJPAGENT_PROFILE_ID)) < 0 ||
        snag_json_set_new(config, "prompt_schema", json_integer(1)) < 0 ||
        snag_json_set_new(config, "replay_schema", json_integer(1)) < 0 ||
        snag_json_set_new(config, "tool_schema", json_integer(1)) < 0) {
        if (config)
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
    json_t *data = json_object();
    json_t *instructions = snag_instructions_metadata_json(&app->turn_instructions);

    if (!data || !instructions ||
        snag_json_set_new(data, "config", turn_config(app)) < 0 ||
        snag_json_set_new(data, "input_kind",
                         json_string(goal_turn ? "goal" :
                                     queued ? "queued" : "direct")) < 0 ||
        snag_json_set_new(data, "instructions", instructions) < 0)
        goto fail;
    instructions = NULL;
    if (snag_json_set_new(data, "read_only", json_boolean(read_only)) < 0 ||
        snag_json_set_new(data, "queue_id",
                         queued ? json_string(queued->queue_id) : json_null()) < 0 ||
        snag_json_set_new(data, "queue_seq",
                         queued ? json_integer((json_int_t)queued->seq) :
                                  json_null()) < 0 ||
        snag_json_set_new(data, "text", json_string(prompt)) < 0 ||
        snag_json_set_new(data, "turn_id", json_string(turn_id)) < 0 ||
        snag_json_set_new(data, "turn_number",
                         json_integer((json_int_t)(app->session.turn_count + 1u))) < 0 ||
        snag_json_set_new(data, "workspace",
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
    json_t *data = json_object();

    if (!data ||
        snag_json_set_new(data, "endpoint", json_string(event->endpoint)) < 0 ||
        snag_json_set_new(data, "historical",
                         json_boolean(event->historical)) < 0 ||
        snag_json_set_new(data, "kind",
                         json_string(irc_kind_name(event->kind))) < 0 ||
        snag_json_set_new(data, "local", json_boolean(event->local)) < 0 ||
        snag_json_set_new(data, "nick", json_string(event->nick)) < 0 ||
        snag_json_set_new(data, "op", json_boolean(event->op)) < 0 ||
        snag_json_set_new(data, "room", json_string(event->room)) < 0 ||
        snag_json_set_new(data, "text", json_string(event->text)) < 0 ||
        snag_json_set_new(data, "timestamp_ms",
                         json_integer((json_int_t)event->timestamp_ms)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
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
snag_app_request_digests(struct app_state *app, const char *prompt,
                const json_t *steering, unsigned int cycle,
                const struct snag_credential *credential,
                char input_hash[SNAG_SHA256_HEX_LEN + 1u],
                char request_hash[SNAG_SHA256_HEX_LEN + 1u],
                char count_request_hash[SNAG_SHA256_HEX_LEN + 1u],
                uint64_t *input_tokens_bound, uint64_t *model_input_bytes,
                uint64_t *request_input_bytes, uint64_t *request_input_count,
                char request_input_hash[SNAG_SHA256_HEX_LEN + 1u],
                const char *provider_source_sha256,
                const char **count_method, struct snag_buf *request_body,
                json_t **create_request, json_t **count_request,
                char *error, size_t error_size)
{
    struct snag_context_projection projection;
    int rc;

    (void)prompt;
    if (!input_tokens_bound || !model_input_bytes || !request_input_bytes ||
        !request_input_count || !request_input_hash ||
        !provider_source_sha256 || !count_method) {
        errno = EINVAL;
        return -1;
    }
    *count_method = "qualified_upper_bound";
    if (create_request)
        *create_request = NULL;
    if (count_request)
        *count_request = NULL;
    snag_context_projection_init(&projection);
    rc = snag_context_build(&app->session, app->turn_model, app->turn_effort,
                           cycle, steering,
                           app->turn_capacity.max_output_tokens,
                           app->turn_capacity.max_output_known,
                           app->config,
                           &app->turn_instructions, &projection,
                           error, error_size);
    if (rc == 0) {
        memcpy(input_hash, projection.model_input_sha256,
               SNAG_SHA256_HEX_LEN + 1u);
        memcpy(request_hash, projection.request_sha256,
               SNAG_SHA256_HEX_LEN + 1u);
        memcpy(count_request_hash, projection.count_request_sha256,
               SNAG_SHA256_HEX_LEN + 1u);
        *input_tokens_bound = projection.input_tokens_bound;
        *model_input_bytes = (uint64_t)projection.model_input_bytes;
        *request_input_bytes = (uint64_t)projection.request_input_bytes;
        *request_input_count = (uint64_t)projection.request_input_count;
        memcpy(request_input_hash, projection.request_input_sha256,
               SNAG_SHA256_HEX_LEN + 1u);
        {
            uint64_t anchored_bound = 0u;
            int anchor_rc = snag_context_usage_anchor_bound(
                &app->session, app->turn_provider->name, app->turn_model,
                app->turn_effort, provider_source_sha256, &projection,
                &anchored_bound);

            if (anchor_rc < 0) {
                if (error_size)
                    (void)snprintf(error, error_size,
                                   "cannot evaluate provider-usage anchor");
                rc = -1;
            } else if (anchor_rc == 1) {
                *input_tokens_bound = anchored_bound;
                *count_method = "anchored_upper_bound";
            } else if (app->turn_capacity.observed_tokens_per_million_bytes) {
                *input_tokens_bound = snag_context_input_estimate(
                    *model_input_bytes,
                    app->turn_capacity.observed_tokens_per_million_bytes);
                *count_method = "statistical_upper_estimate";
            }
        }
        if (create_request)
            *create_request = json_incref(projection.create_request);
        if (count_request)
            *count_request = json_incref(projection.count_request);
        if (snag_ui_enabled(&app->ui, SNAG_PRESENT_PROTOCOL) && request_body) {
            struct snag_buf encoded;
            struct snag_secret_set secrets;

            snag_buf_init(&encoded, SNAG_WIRE_BODY_MAX);
            snag_secret_set_build(&secrets, app->config, credential);
            if (projection.create_request_bytes <= SNAG_WIRE_BODY_MAX &&
                snag_json_canonical(projection.create_request, &encoded) == 0 &&
                snag_wire_json_redact(encoded.data, encoded.len, &secrets.wire,
                                     request_body, error, error_size) == 0) {
                /* sanitized canonical request captured for durable-fenced rendering */
            } else {
                snag_buf_reset(request_body);
                if (snag_buf_printf(request_body,
                        "<request body omitted; bytes=%zu; sha256=%s>\n",
                        projection.create_request_bytes,
                        projection.request_sha256) < 0)
                    rc = -1;
            }
            snag_buf_free(&encoded);
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
    snag_context_projection_free(&projection);
    return rc;
}
json_t *
snag_app_response_started_data(const char *turn_id, const char *response_id,
                      unsigned int cycle, const char *compact_id,
                      const char *model,
                      const char *input_hash,
                      const char *request_hash, const char *count_request_hash,
                      const char *count_method, uint64_t input_tokens_bound,
                      uint64_t model_input_bytes, uint64_t request_input_bytes,
                      uint64_t request_input_count, const char *request_input_hash,
                      const char *baseline_hash,
                      const char *provider, const char *effort,
                      const char *provider_source_sha256,
                      const struct snag_model_capacity *capacity,
                      const json_t *steering)
{
    json_t *data = json_object();
    json_t *ids = json_array();

    if (!data || !ids || !steering || !provider || !effort ||
        !provider_source_sha256 ||
        !snag_hex_is_lower(provider_source_sha256, SNAG_SHA256_HEX_LEN) ||
        !capacity)
        goto fail;
    for (size_t i = 0; i < json_array_size(steering); ++i) {
        json_t *item = json_array_get(steering, i);
        const char *id = snag_json_string(item, "id");
        if (!id || json_array_append_new(ids, json_string(id)) < 0)
            goto fail;
    }
    if (!model || !count_method || !count_request_hash)
        goto fail;
    if (snag_json_set_new(data, "baseline_sha256",
                         baseline_hash && *baseline_hash ?
                         json_string(baseline_hash) : json_null()) < 0 ||
        snag_json_set_new(data, "capability_version",
                         json_string(SNAJPAGENT_CAPABILITY_VERSION)) < 0 ||
        snag_json_set_new(data, "compact_id",
                         compact_id && *compact_id ?
                         json_string(compact_id) : json_null()) < 0 ||
        snag_json_set_new(data, "count_method", json_string(count_method)) < 0 ||
        snag_json_set_new(data, "count_request_sha256",
                         json_string(count_request_hash)) < 0 ||
        snag_json_set_new(data, "capacity_source",
                         json_string(snag_capacity_source_name(
                             capacity->source))) < 0 ||
        snag_json_set_new(data, "cycle", json_integer((json_int_t)cycle)) < 0 ||
        snag_json_set_new(data, "effort", json_string(effort)) < 0 ||
        snag_json_set_new(data, "hard_input_tokens",
            capacity->hard_input_known ?
                json_integer((json_int_t)capacity->hard_input_tokens) :
                json_null()) < 0 ||
        snag_json_set_new(data, "input_tokens_bound",
                         json_integer((json_int_t)input_tokens_bound)) < 0 ||
        snag_json_set_new(data, "model", json_string(model)) < 0 ||
        snag_json_set_new(data, "model_input_bytes",
                         json_integer((json_int_t)model_input_bytes)) < 0 ||
        snag_json_set_new(data, "model_input_sha256", json_string(input_hash)) < 0 ||
        snag_json_set_new(data, "profile_id",
                         json_string(SNAJPAGENT_PROFILE_ID)) < 0 ||
        snag_json_set_new(data, "provider", json_string(provider)) < 0 ||
        snag_json_set_new(data, "provider_source_sha256",
                         json_string(provider_source_sha256)) < 0 ||
        snag_json_set_new(data, "request_input_bytes",
                         json_integer((json_int_t)request_input_bytes)) < 0 ||
        snag_json_set_new(data, "request_input_count",
                         json_integer((json_int_t)request_input_count)) < 0 ||
        snag_json_set_new(data, "request_input_sha256",
                         json_string(request_input_hash)) < 0 ||
        snag_json_set_new(data, "requested_output_tokens",
            capacity->max_output_known ?
                json_integer((json_int_t)capacity->max_output_tokens) :
                json_null()) < 0 ||
        snag_json_set_new(data, "request_sha256", json_string(request_hash)) < 0 ||
        snag_json_set_new(data, "response_id", json_string(response_id)) < 0 ||
        snag_json_set_new(data, "source_bound",
                         json_boolean(capacity->source_bound)) < 0)
        goto fail;
    {
        int rc = snag_json_set_new(data, "steering_ids", ids);
        ids = NULL;
        if (rc < 0)
            goto fail;
    }
    if (snag_json_set_new(data, "turn_id", json_string(turn_id)) < 0)
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
snag_app_response_capacity_rejected_data(
    const char *turn_id, const char *response_id, unsigned int cycle,
    const char *request_hash, const struct snag_provider_failure *failure,
    const struct snag_model_capacity *capacity,
    const char *provider_source_sha256)
{
    json_t *data = json_object();
    uint64_t safety_ceiling = 0u;
    bool safety_ceiling_known;

    safety_ceiling_known = capacity &&
        snag_provider_failure_safety_ceiling(failure,
            capacity->max_output_known, capacity->max_output_tokens,
            &safety_ceiling);

    if (!data || !turn_id || !response_id || !request_hash || !failure ||
        !capacity || !provider_source_sha256 ||
        !snag_hex_is_lower(provider_source_sha256, SNAG_SHA256_HEX_LEN) ||
        snag_json_set_new(data, "code", json_string(failure->code)) < 0 ||
        snag_json_set_new(data, "context_limit_tokens",
            failure->context_limit_known ?
                json_integer((json_int_t)failure->context_limit_tokens) :
                json_null()) < 0 ||
        snag_json_set_new(data, "cycle", json_integer((json_int_t)cycle)) < 0 ||
        snag_json_set_new(data, "message", json_string(failure->message)) < 0 ||
        snag_json_set_new(data, "observed_hard_input_tokens",
            safety_ceiling_known ?
                json_integer((json_int_t)safety_ceiling) : json_null()) < 0 ||
        snag_json_set_new(data, "provider_source_sha256",
                         json_string(provider_source_sha256)) < 0 ||
        snag_json_set_new(data, "request_sha256",
                         json_string(request_hash)) < 0 ||
        snag_json_set_new(data, "requested_input_tokens",
            failure->requested_input_known ?
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
    json_t *data = json_object();
    json_t *items = snag_response_graph_json(graph);
    json_t *value;

    if (!data || !items)
        goto fail;
    if (snag_json_set_new(data, "cycle", json_integer((json_int_t)cycle)) < 0)
        goto fail;
    value = items;
    items = NULL;
    if (snag_json_set_new(data, "items", value) < 0)
        goto fail;
    if (snag_json_set_new(data, "provider_response_id",
                         json_string(graph->provider_response_id)) < 0 ||
        snag_json_set_new(data, "response_id", json_string(response_id)) < 0 ||
        snag_json_set_new(data, "status", json_string("completed")) < 0 ||
        snag_json_set_new(data, "turn_id", json_string(turn_id)) < 0 ||
        snag_json_set_new(data, "usage",
                         snag_response_usage_json(&graph->usage)) < 0)
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
snag_app_response_output_correction_data(const char *turn_id,
                                        const char *response_id,
                                        unsigned int cycle,
                                        const char *correction_id,
                                        const char *text,
                                        json_t *partial_public)
{
    json_t *data = json_object();
    json_t *partial = partial_public;

    if (!data || !partial ||
        snag_json_set_new(data, "correction_id",
                         json_string(correction_id)) < 0 ||
        snag_json_set_new(data, "cycle",
                         json_integer((json_int_t)cycle)) < 0)
        goto fail;
    {
        int rc = snag_json_set_new(data, "partial_public", partial);
        partial = NULL;
        if (rc < 0)
            goto fail;
    }
    if (
        snag_json_set_new(data, "response_id", json_string(response_id)) < 0 ||
        snag_json_set_new(data, "text", json_string(text)) < 0 ||
        snag_json_set_new(data, "turn_id", json_string(turn_id)) < 0)
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
snag_app_turn_completed_data(const char *turn_id, const char *response_id,
                    const char *item_id)
{
    json_t *data = json_object();
    if (!data ||
        snag_json_set_new(data, "final_item_id", json_string(item_id)) < 0 ||
        snag_json_set_new(data, "final_response_id", json_string(response_id)) < 0 ||
        snag_json_set_new(data, "turn_id", json_string(turn_id)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
}

json_t *
snag_app_steering_added_data(const char *turn_id, const char *steering_id,
                    const char *text)
{
    json_t *data = json_object();
    if (!data ||
        snag_json_set_new(data, "steering_id", json_string(steering_id)) < 0 ||
        snag_json_set_new(data, "text", json_string(text)) < 0 ||
        snag_json_set_new(data, "turn_id", json_string(turn_id)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
}

json_t *
snag_app_future_turn_queued_data(const char *turn_id, const char *queue_id,
                        const char *text, bool read_only)
{
    json_t *data = json_object();
    if (!data || snag_json_set_new(data, "read_only", json_boolean(read_only)) < 0 ||
        snag_json_set_new(data, "queue_id", json_string(queue_id)) < 0 ||
        snag_json_set_new(data, "text", json_string(text)) < 0 ||
        snag_json_set_new(data, "while_turn_id", json_string(turn_id)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
}

json_t *
snag_app_future_turn_edited_data(const char *queue_id, const char *text,
                               bool read_only)
{
    json_t *data = json_object();

    if (!data || snag_json_set_new(data, "read_only", json_boolean(read_only)) < 0 ||
        snag_json_set_new(data, "queue_id", json_string(queue_id)) < 0 ||
        snag_json_set_new(data, "text", json_string(text)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
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
    json_t *data = json_object();
    json_t *partial = partial_public ? partial_public : json_array();

    if (!data || !partial)
        goto fail;
    if (
        snag_json_set_new(data, "cycle", json_integer((json_int_t)cycle)) < 0 ||
        snag_json_set_new(data, "origin", json_string(origin)) < 0)
        goto fail;
    {
        int rc = snag_json_set_new(data, "partial_public", partial);
        partial = NULL;
        if (rc < 0)
            goto fail;
    }
    if (snag_json_set_new(data, "reason", json_string(reason)) < 0 ||
        snag_json_set_new(data, "response_id", json_string(response_id)) < 0 ||
        snag_json_set_new(data, "turn_id", json_string(turn_id)) < 0)
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
snag_app_turn_interrupted_data(const char *turn_id, const char *origin, const char *reason)
{
    json_t *data = json_object();
    if (!data || snag_json_set_new(data, "origin", json_string(origin)) < 0 ||
        snag_json_set_new(data, "reason", json_string(reason)) < 0 ||
        snag_json_set_new(data, "turn_id", json_string(turn_id)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
}

json_t *
snag_app_response_failed_data(const char *turn_id, const char *response_id,
                     unsigned int cycle, const char *class_name,
                     const char *message, json_t *partial_public,
                     unsigned int retry_count)
{
    json_t *data = json_object();
    json_t *partial = partial_public ? partial_public : json_array();

    if (!data || !partial ||
        snag_json_set_new(data, "class", json_string(class_name)) < 0 ||
        snag_json_set_new(data, "cycle", json_integer((json_int_t)cycle)) < 0 ||
        snag_json_set_new(data, "message", json_string(message)) < 0)
        goto fail;
    {
        int rc = snag_json_set_new(data, "partial_public", partial);
        partial = NULL;
        if (rc < 0)
            goto fail;
    }
    if (
        snag_json_set_new(data, "response_id", json_string(response_id)) < 0 ||
        snag_json_set_new(data, "retry_count",
                         json_integer((json_int_t)retry_count)) < 0 ||
        snag_json_set_new(data, "turn_id", json_string(turn_id)) < 0)
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
snag_app_turn_failed_data(const char *turn_id, const char *class_name, const char *message)
{
    json_t *data = json_object();
    if (!data || snag_json_set_new(data, "class", json_string(class_name)) < 0 ||
        snag_json_set_new(data, "message", json_string(message)) < 0 ||
        snag_json_set_new(data, "turn_id", json_string(turn_id)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
}

json_t *
snag_app_tool_started_data(const char *turn_id, const char *call_id,
                  const char *action_sha256, const char *workspace)
{
    json_t *data = json_object();
    if (!data ||
        snag_json_set_new(data, "action_sha256", json_string(action_sha256)) < 0 ||
        snag_json_set_new(data, "call_id", json_string(call_id)) < 0 ||
        snag_json_set_new(data, "resolved_workdir", json_string(workspace)) < 0 ||
        snag_json_set_new(data, "turn_id", json_string(turn_id)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
}

/* Takes ownership of result, including on failure. */
json_t *
snag_app_tool_finished_data(const char *turn_id, const char *call_id, json_t *result)
{
    json_t *data = json_object();
    json_t *value;
    if (!data) {
        if (result)
            json_decref(result);
        return NULL;
    }
    if (snag_json_set_new(data, "call_id", json_string(call_id)) < 0)
        goto fail;
    value = result;
    result = NULL;
    if (snag_json_set_new(data, "result", value) < 0 ||
        snag_json_set_new(data, "turn_id", json_string(turn_id)) < 0)
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
snag_app_process_closed_data(const char *turn_id, const char *handle,
                            const char *cause, json_t *result)
{
    json_t *data = json_object();
    json_t *value;

    if (!data) {
        if (result)
            json_decref(result);
        return NULL;
    }
    if (snag_json_set_new(data, "cause", json_string(cause)) < 0 ||
        snag_json_set_new(data, "handle", json_string(handle)) < 0)
        goto fail;
    value = result;
    result = NULL;
    if (snag_json_set_new(data, "result", value) < 0 ||
        snag_json_set_new(data, "turn_id", json_string(turn_id)) < 0)
        goto fail;
    return data;

fail:
    if (result)
        json_decref(result);
    if (data)
        json_decref(data);
    return NULL;
}
