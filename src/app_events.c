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

static int
append_pending(struct snag_buf *pending, const char *text, size_t len)
{
    /* NUL separates complete projections, including multiline messages. */
    if (snag_buf_reserve(pending, len + 1u) < 0 ||
        snag_buf_append(pending, text, len) < 0)
        return -1;
    return snag_buf_putc(pending, 0u);
}

static char *
pending_batch(const struct snag_buf *pending, size_t *used)
{

    *used = 0u;
    struct snag_buf batch = {.max = SNAG_MAX_STEERING_TEXT + 1u};
    while (*used < pending->len) {
        const char *text = (const char *)pending->data + *used;
        size_t len = strlen(text);

        if (len > SNAG_MAX_STEERING_TEXT - batch.len)
            break;
        if (snag_buf_append(&batch, text, len) < 0)
            goto fail;
        *used += len + 1u;
    }
    if (*used && snag_buf_terminate(&batch) == 0)
        return (char *)batch.data;
fail:
    snag_buf_free(&batch);
    return NULL;
}

static void
consume_pending(struct snag_buf *pending, size_t used)
{
    pending->len -= used;
    memmove(pending->data, pending->data + used, pending->len);
}

struct irc_input_ref { uint64_t seq; size_t end; };

static int
admit_irc_input(struct app_state *app, struct snag_buf *refs, size_t used,
                const char *kind, json_t *intent, char *error, size_t error_size)
{
    if (!intent) {
        snag_errorf(error, error_size, "cannot construct network input intent");
        app->input_closed = true;
        return -1;
    }
    struct irc_input_ref *items = (struct irc_input_ref *)refs->data;
    size_t count = 0u, total = refs->len / sizeof(*items);
    json_t *sequences = json_array();
    if (!sequences) { json_decref(intent); return -1; }
    while (count < total && items[count].end <= used) {
        if (json_array_append_new(sequences, json_integer((json_int_t)items[count].seq)) < 0) {
            json_decref(sequences); json_decref(intent); return -1;
        }
        ++count;
    }
    json_t *data = count ? json_pack("{s:O}", "sequences", sequences) : NULL;
    if (count && snag_json_set_new(data,
            !strcmp(kind, "input_received") ? "input" : "steering", json_incref(intent)) < 0) {
        json_decref(data); data = NULL;
    }
    int rc = count ? snag_app_commit_event(app, "irc_admitted", data, error, error_size) :
        snag_app_commit_event(app, kind, json_incref(intent), error, error_size);
    json_decref(sequences);
    json_decref(intent);
    if (rc < 0) { app->input_closed = true; return -1; }
    if (refs->len) {
        memmove(items, items + count, refs->len - count * sizeof(*items));
        refs->len -= count * sizeof(*items);
        for (size_t i = 0u; i < refs->len / sizeof(*items); ++i)
            items[i].end -= used;
    }
    return 0;
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

    if (!snag_gmtime(&seconds, &tm) ||
        strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0)
        memcpy(when, "1970-01-01T00:00:00Z", 21u);
    snag_buf_init(&line, SNAG_IRC_TEXT_MAX + SNAG_CONFIG_IRC_ENDPOINT_MAX +
                         SNAG_CONFIG_IRC_ROOM_MAX + SNAG_CONFIG_IRC_NICK_MAX + 256u);
    if (event->stream[0] || event->historical) {
        rc = snag_buf_printf(&line, "[IRC update id=%s:%llu endpoint=%s room=%s; received content is in the preceding room event]\n",
            event->stream, (unsigned long long)event->sequence, event->endpoint, event->room);
        if (rc == 0)
            rc = append_pending(pending, (const char *)line.data, line.len);
        snag_buf_free(&line);
        return rc;
    }
    if (snag_buf_printf(&line,
            "[IRC endpoint=%s room=%s time=%s event=%s sender=%s operator=%s]\n%s\n",
            event->endpoint, event->room, when, snag_irc_kind_name(event->kind),
            event->nick[0] ? event->nick : "server",
            event->op ? "true" : "false", event->text) < 0 ||
        append_pending(pending, (const char *)line.data, line.len) < 0)
        goto out;
    rc = 0;
out:
    snag_buf_free(&line);
    return rc;
}

static void
reply_target(struct snag_irc_route *route, struct snag_irc_target target, bool add)
{
    for (size_t i = 0u; i < route->count; ++i)
        if (route->targets[i].id == target.id && route->targets[i].revision == target.revision) {
            if (!add) {
                memmove(route->targets + i, route->targets + i + 1u,
                         (--route->count - i) * sizeof(route->targets[0]));
            }
            return;
        }
    if (add && route->count < SNAG_IRC_DESTINATIONS_MAX)
        route->targets[route->count++] = target;
}

static void
prune_replies(struct snag_irc_route *route, const struct snag_irc_destinations *destinations,
                size_t *offsets)
{
    size_t kept = 0u;
    for (size_t i = 0u; i < route->count; ++i)
        for (size_t j = 0u; j < destinations->count; ++j)
            if (route->targets[i].id == destinations->items[j].target.id &&
                route->targets[i].revision == destinations->items[j].target.revision) {
                if (offsets)
                    offsets[kept] = offsets[i];
                route->targets[kept++] = route->targets[i];
            }
    route->count = kept;
}

int
snag_app_sync_destinations(struct app_state *app)
{
    struct snag_irc_destinations current;

    snag_irc_destinations(app->irc, &current);
    if (app->irc_destinations_ready &&
        memcmp(&current, &app->irc_destinations, sizeof(current)) == 0)
        return 0;
    if (snag_ui_send(&app->ui, (struct snag_ui_command){
        .kind = SNAG_UI_DESTINATIONS, .data.destinations = &current}) < 0)
        return -1;
    prune_replies(&app->irc_urgent_replies, &current, app->irc_urgent_reply_offsets);
    prune_replies(&app->irc_turn_replies, &current, NULL);
    app->irc_destinations = current;
    app->irc_destinations_ready = true;
    return 0;
}

int
snag_app_irc_snapshot(struct app_state *app, const char *reason,
                     char *error, size_t error_size)
{
    int rc = -1;

    if (!app || !app->irc || !reason)
        return snag_errno(EINVAL);
    if (snag_app_sync_destinations(app) < 0)
        return -1;
    struct snag_buf snapshot = {.max = SNAG_MAX_IRC_SNAPSHOT};
    rc = strcmp(reason, "compaction") != 0 ?
        snag_irc_state(app->irc, &snapshot, error, error_size) :
        snag_irc_snapshot(app->irc, &snapshot, error, error_size);
    if (rc < 0)
        goto out;
    rc = -1;
    if (snag_buf_terminate(&snapshot) < 0)
        goto out;
    rc = snag_app_commit_event(app, "irc_snapshot",
        json_pack("{s:s,s:s,s:I}", "reason", reason,
                  "text", (const char *)snapshot.data,
                  "timestamp_ms", (json_int_t)snag_time_ms()), error, error_size);
out:
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
    struct snag_irc_target target;
    size_t reply_offset;

    if (!app || !event)
        return -1;
    if (snag_app_sync_destinations(app) < 0)
        return -1;
    struct snag_irc_event accepted = *event;
    accepted.input = !snag_irc_local_identity(app->irc, event, true) &&
        event->kind != SNAG_IRC_HISTORY_READY && (event->stream[0] || event->historical);
    accepted.classified = true;
    accepted.urgent = (event->kind == SNAG_IRC_MESSAGE || event->kind == SNAG_IRC_NOTICE) &&
        !event->historical && snag_irc_mentions_agent(app->irc, event->endpoint, event->text);
    accepted.reply = accepted.urgent && snag_irc_local_identity(app->irc, event, false);
    uint64_t accepted_seq = app->session.next_seq;
    if (snag_app_commit_event(app, "irc_event", snag_irc_event_data(&accepted),
                             error, sizeof(error)) < 0)
        return -1;
    if (snag_ui_send(&app->ui, (struct snag_ui_command){
        .kind = SNAG_UI_IRC, .data.irc = event}) < 0)
        return -1;
    if (event->kind == SNAG_IRC_DISCONNECTED &&
        strstr(event->text, "endpoint removed; discarded ") == event->text &&
        snag_ui_text(&app->ui, SNAG_UI_WARNING, event->text) < 0)
        return -1;
    chat = event->kind == SNAG_IRC_MESSAGE || event->kind == SNAG_IRC_NOTICE;
    own_agent = snag_irc_local_identity(app->irc, event, true);
    local_operator = snag_irc_local_identity(app->irc, event, false);
    if (own_agent) {
        if (app->session.active_turn && event->kind == SNAG_IRC_MESSAGE &&
            snag_irc_event_target(app->irc, event, &target))
            reply_target(&app->irc_turn_replies, target, false);
        return 0;
    }
    if (event->kind == SNAG_IRC_HISTORY_READY) {
        if (snag_app_irc_snapshot(app, "join", error, sizeof(error)) < 0)
            return -1;
    }

    if (chat)
        ++app->input_generation;
    urgent = chat && !event->historical && snag_irc_mentions_agent(app->irc, event->endpoint, event->text);
    reply_offset = app->irc_urgent.len;
    if (append_irc_projection(urgent ? &app->irc_urgent :
                                      &app->irc_background, event) < 0)
        return -1;
    if (accepted.input && event->historical) {
        /* Catch-up is background context, but unlike newly arriving ordinary
         * chat it is available at the existing join-history response boundary. */
        if (snag_app_commit_event(app, "irc_admitted", json_pack("{s:[I]}",
                "sequences", (json_int_t)accepted_seq), error, sizeof(error)) < 0)
            return -1;
    } else if (accepted.input) {
        struct irc_input_ref ref = {accepted_seq, urgent ? app->irc_urgent.len : app->irc_background.len};
        if (snag_buf_append(urgent ? &app->irc_urgent_refs : &app->irc_background_refs,
                           &ref, sizeof(ref)) < 0) return -1;
    }
    if (urgent && local_operator && snag_irc_event_target(app->irc, event, &target)) {
        size_t before = app->irc_urgent_replies.count;
        reply_target(&app->irc_urgent_replies, target, true);
        if (app->irc_urgent_replies.count > before)
            app->irc_urgent_reply_offsets[before] = reply_offset;
    }
    if (!urgent && !app->irc_background_since_ms)
        app->irc_background_since_ms = snag_time_ms();
    return 0;
}

int
snag_app_irc_trace(void *opaque, unsigned int level, char direction,
                  const char *endpoint, const char *text, size_t len)
{
    struct app_state *app = opaque;
    char label[384u];
    int rc = -1;

    if (!app || !endpoint || !text || (level != 5u && level != 6u) ||
        (direction != '<' && direction != '>'))
        return snag_errno(EINVAL);
    if (!snag_ui_enabled(&app->ui, level == 6u ? SNAG_PRESENT_WIRE : SNAG_PRESENT_PROTOCOL))
        return 0;
    struct snag_buf safe = {.max = 4u * SNAG_IRC_LINE_MAX};
    if (level == 6u) {
        safe.max += SNAG_CONFIG_IRC_ENDPOINT_MAX + 8u;
        if (snag_buf_printf(&safe, "IRC [%s] ", endpoint) < 0)
            goto out;
        if (safe.max > safe.len + 4u * SNAG_IRC_LINE_MAX)
            safe.max = safe.len + 4u * SNAG_IRC_LINE_MAX;
    }
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
        rc = snag_ui_send(&app->ui, (struct snag_ui_command){
            .kind = SNAG_UI_TRANSPORT, .data.value = direction, .text = (const char *)safe.data, .len = safe.len});
    } else {
        int n = snprintf(label, sizeof(label), "irc.command %c %s",
                         direction, endpoint);
        if (n < 0 || (size_t)n >= sizeof(label)) {
            errno = EOVERFLOW;
            goto out;
        }
        rc = snag_ui_send(&app->ui, (struct snag_ui_command){
            .kind = SNAG_UI_PROTOCOL, .label = label, .text = (const char *)safe.data, .len = safe.len});
    }
out:
    snag_buf_free(&safe);
    return rc;
}

static void
admit_replies(struct app_state *app, size_t used)
{
    size_t kept = 0u;
    for (size_t i = 0u; i < app->irc_urgent_replies.count; ++i) {
        if (app->irc_urgent_reply_offsets[i] < used) {
            reply_target(&app->irc_turn_replies, app->irc_urgent_replies.targets[i], true);
        } else {
            app->irc_urgent_replies.targets[kept] = app->irc_urgent_replies.targets[i];
            app->irc_urgent_reply_offsets[kept++] = app->irc_urgent_reply_offsets[i] - used;
        }
    }
    app->irc_urgent_replies.count = kept;
}

int
snag_app_irc_flush_urgent(struct app_state *app,
                         char *error, size_t error_size)
{
    char steering_id[SNAG_ID_HEX_LEN + 1u];
    size_t used;
    char *text;
    int rc;

    if (!app || !app->session.active_turn || !app->irc_urgent.len)
        return 0;
    if (snag_random_id(steering_id) < 0 ||
        !(text = pending_batch(&app->irc_urgent, &used)))
        return -1;
    rc = admit_irc_input(app, &app->irc_urgent_refs, used, "steering_added",
            snag_app_steering_added_data(app->session.active_turn_id, steering_id, text), error, error_size);
    free(text);
    if (rc < 0) return -1;
    consume_pending(&app->irc_urgent, used);
    admit_replies(app, used);
    return 0;
}

char *
snag_app_irc_take_pending(struct app_state *app,
                         bool *local_operator, bool force_background)
{
    struct snag_buf *source;
    char *copy;
    size_t used;

    if (local_operator)
        *local_operator = false;
    if (!app || app->session.pending_input)
        return NULL;
    if (app->irc_urgent.len) {
        source = &app->irc_urgent;
        if (local_operator)
            *local_operator = app->irc_urgent_replies.count != 0u;
    /* Startup/history alone must not turn an unused session into saved work. */
    } else if (app->session.log_fd >= 0 && app->irc_background.len &&
               (force_background ||
                snag_time_ms() - app->irc_background_since_ms >= 100u)) {
        source = &app->irc_background;
    } else {
        return NULL;
    }
    copy = pending_batch(source, &used);
    if (!copy)
        return NULL;
    char error[256] = {0};
    if (admit_irc_input(app, source == &app->irc_urgent ? &app->irc_urgent_refs :
                        &app->irc_background_refs, used, "input_received",
                        snag_app_input_received_data(app, copy, false), error, sizeof(error)) < 0) {
        free(copy);
        (void)snag_ui_text(&app->ui, SNAG_UI_ERROR, error);
        return NULL;
    }
    consume_pending(source, used);
    app->irc_turn_replies.count = 0u;
    if (source == &app->irc_urgent) {
        admit_replies(app, used);
        if (local_operator)
            *local_operator = app->irc_turn_replies.count != 0u;
    } else if (!source->len) {
        app->irc_background_since_ms = 0u;
    }
    return copy;
}

struct irc_restore {
    struct app_state *app;
    json_t *pending;
};

static int
restore_irc_event(void *opaque, const struct snag_session *state,
                  uint64_t seq, const char *type, const json_t *data,
                  char *error, size_t error_size)
{
    struct irc_restore *restore = opaque;
    struct app_state *app = restore->app;
    struct snag_irc_event event;
    (void)error; (void)error_size;
    if (!strcmp(type, "irc_admitted")) {
        const json_t *seqs = json_object_get(data, "sequences");
        for (size_t j = 0u; j < json_array_size(seqs); ++j) {
            for (size_t i = 0u; i < json_array_size(restore->pending); ++i) {
                const json_t *entry = json_array_get(restore->pending, i);
                if (json_integer_value(json_object_get(entry, "seq")) !=
                    json_integer_value(json_array_get(seqs, j))) continue;
                if (snag_irc_event_read(json_object_get(entry, "data"), &event) < 0) return -1;
                bool current = (state->active_turn && app->session.active_turn &&
                    !strcmp(state->active_turn_id, app->session.active_turn_id)) ||
                    (state->pending_input && (app->session.pending_input || app->session.active_turn));
                struct snag_irc_target target;
                if (current && event.reply && snag_irc_event_target(app->irc, &event, &target))
                    reply_target(&app->irc_turn_replies, target, true);
                if (json_array_remove(restore->pending, i) < 0) return -1;
                break;
            }
        }
        return 0;
    }
    if (strcmp(type, "irc_event")) return 0;
    if (snag_irc_event_read(data, &event) < 0 || snag_irc_restore_event(app->irc, &event) < 0) return -1;
    if (event.input && json_array_append_new(restore->pending,
            json_pack("{s:I,s:O}", "seq", (json_int_t)seq, "data", (json_t *)data)) < 0) return -1;
    return 0;
}

int
snag_app_irc_restore(struct app_state *app, char *error, size_t error_size)
{
    if (!app || !app->irc) return snag_errno(EINVAL);
    struct irc_restore restore = {app, json_array()};
    if (!restore.pending) return -1;
    int rc = snag_session_each_event(&app->session, restore_irc_event, &restore, error, error_size);
    for (size_t i = 0u; !rc && i < json_array_size(restore.pending); ++i) {
        const json_t *entry = json_array_get(restore.pending, i);
        struct snag_irc_event event;
        if (snag_irc_event_read(json_object_get(entry, "data"), &event) < 0) { rc = -1; break; }
        bool urgent = event.classified ? event.urgent : !event.historical &&
            snag_irc_mentions_agent(app->irc, event.endpoint, event.text);
        struct snag_buf *buffer = urgent ? &app->irc_urgent : &app->irc_background;
        size_t offset = buffer->len;
        rc = append_irc_projection(buffer, &event);
        struct irc_input_ref ref = {(uint64_t)json_integer_value(json_object_get(entry, "seq")), buffer->len};
        if (!rc) rc = snag_buf_append(urgent ? &app->irc_urgent_refs : &app->irc_background_refs, &ref, sizeof(ref));
        struct snag_irc_target target;
        if (urgent && event.reply && snag_irc_event_target(app->irc, &event, &target)) {
            size_t before = app->irc_urgent_replies.count;
            reply_target(&app->irc_urgent_replies, target, true);
            if (before != app->irc_urgent_replies.count) app->irc_urgent_reply_offsets[before] = offset;
        }
    }
    if (app->irc_background.len) app->irc_background_since_ms = snag_time_ms();
    json_decref(restore.pending);
    return rc;
}

json_t *
snag_app_steering_snapshot(const struct snag_session *session)
{
    json_t *array = json_array();

    if (!array)
        return NULL;
    for (size_t i = 0; i < session->pending_steering_count; ++i) {
        json_t *item = json_pack("{s:s,s:s}",
            "id", session->pending_steering[i].steering_id,
            "text", session->pending_steering[i].text);
        if (!item || json_array_append_new(array, item) < 0) {
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
                       struct snag_context_projection *projection,
                       const char **count_method, struct snag_buf *request_body,
                       char *error, size_t error_size)
{
    int rc;

    app->request_networked = snag_irc_enabled(app->config) &&
                             !app->session.active_read_only;
    snag_irc_capture_route(app->irc, &app->irc_request_route);
    rc = snag_context_build(&app->session, app->turn_model, app->turn_effort,
        cycle, steering, app->turn_capacity.max_output_tokens,
        app->turn_capacity.max_output_tokens, app->config,
        &app->turn_instructions, projection, error, error_size);

    if (rc < 0)
        return -1;
    *count_method = "unknown";
    rc = 0;
    if (snag_ui_enabled(&app->ui, SNAG_PRESENT_PROTOCOL)) {
        struct snag_secret_set secrets = {0};

        snag_buf_init(request_body, SNAG_WIRE_BODY_MAX);
        struct snag_buf encoded = {.max = SNAG_WIRE_BODY_MAX};
        if (snag_secret_set_build(&secrets, app->config, credential, error, error_size) < 0 ||
            projection->create_request.bytes > SNAG_WIRE_BODY_MAX ||
            snag_json_canonical(projection->create_request.value, &encoded) < 0 ||
            snag_wire_json_redact(encoded.data, encoded.len, &secrets.wire,
                                 request_body, error, error_size) < 0) {
            snag_buf_reset(request_body);
            rc = snag_buf_printf(request_body,
                "<request body omitted; bytes=%zu; sha256=%s>\n",
                projection->create_request.bytes, projection->create_request.sha256);
        }
        snag_buf_free(&encoded);
        snag_secret_set_free(&secrets);
    }
    /* Only the request views and accounting facts survive into the cycle. */
    json_decref(projection->model_input.value);
    projection->model_input.value = NULL;
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
    const char *baseline = NULL;
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
        "{s:I,s:s?,s:s,s:s?,s:s,s:s,s:s,s:I,s:s,s:o,s:I,s:s,s:I,s:s,"
        "s:s,s:s,s:s,s:I,s:I,s:s,s:o,s:s,s:s,s:b,s:O,s:s}",
        "irc_seq", (json_int_t)projection->irc_seq, "baseline_sha256", baseline,
        "capability_version", SNAJPAGENT_CAPABILITY_VERSION,
        "compact_id", *compact_id ? compact_id : NULL,
        "count_method", count_method,
        "count_request_sha256", projection->count_request.sha256,
        "capacity_source", snag_capacity_source_name(capacity->source),
        "cycle", (json_int_t)cycle, "effort", app->turn_effort,
        "hard_input_tokens", capacity->hard_input_known ?
            json_integer((json_int_t)capacity->hard_input_tokens) : json_null(),
        "input_tokens_bound", (json_int_t)projection->input_tokens_bound,
        "model", app->turn_model, "model_input_bytes", (json_int_t)projection->model_input.bytes,
        "model_input_sha256", projection->model_input.sha256,
        "profile_id", SNAJPAGENT_PROFILE_ID, "provider", app->turn_provider->name,
        "provider_source_sha256", provider_source_sha256,
        "request_input_bytes", (json_int_t)projection->request_input_bytes,
        "request_input_count", (json_int_t)projection->request_input_count,
        "request_input_sha256", projection->request_input_sha256,
        "requested_output_tokens", capacity->max_output_tokens ?
            json_integer((json_int_t)capacity->max_output_tokens) : json_null(),
        "request_sha256", projection->create_request.sha256,
        "response_id", response_id, "source_bound", capacity->source_bound,
        "steering_ids", ids, "turn_id", turn_id);
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
    if (!turn_id || !response_id || !request_hash || !failure ||
        !capacity || !provider_source_sha256 ||
        !snag_hex_is_lower(provider_source_sha256, SNAG_SHA256_HEX_LEN))
        return NULL;
    uint64_t safety_ceiling = snag_capacity_safety_ceiling(
        failure->context_limit_tokens, failure->requested_input_tokens,
        capacity->max_output_tokens);
    return json_pack("{s:s,s:o,s:I,s:s,s:o,s:s,s:s,s:o,s:s,s:s}",
        "code", failure->code, "context_limit_tokens", failure->context_limit_tokens ?
            json_integer((json_int_t)failure->context_limit_tokens) : json_null(),
        "cycle", (json_int_t)cycle, "message", failure->message,
        "observed_hard_input_tokens", safety_ceiling ?
            json_integer((json_int_t)safety_ceiling) : json_null(),
        "provider_source_sha256", provider_source_sha256, "request_sha256", request_hash,
        "requested_input_tokens", failure->requested_input_tokens ?
            json_integer((json_int_t)failure->requested_input_tokens) : json_null(),
        "response_id", response_id, "turn_id", turn_id);
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
snag_app_response_interrupted_data(const char *turn_id, const char *response_id,
                          unsigned int cycle, const char *origin,
                          const char *reason, json_t *partial_public)
{
    return json_pack("{s:I,s:s,s:o,s:s,s:s,s:s}",
        "cycle", (json_int_t)cycle, "origin", origin,
        "partial_public", partial_public ? partial_public : json_array(),
        "reason", reason, "response_id", response_id, "turn_id", turn_id);
}

json_t *
snag_app_turn_failed_data(const char *turn_id, const char *class_name, const char *message)
{
    return json_pack("{s:s,s:s,s:s}", "class", class_name,
        "message", message, "turn_id", turn_id);
}

int
snag_app_tool_output(void *opaque, const char *handle, unsigned int stream,
                     uint64_t offset, const void *bytes, size_t len)
{
    struct app_state *app = opaque;
    char error[256] = {0};
    bool utf8 = snag_utf8_valid(bytes, len, true);
    json_t *event;
    int rc = -1;
    struct snag_buf encoded = {.max = 32768u};
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
read_process_chunk(void *opaque, const struct snag_session *state,
                    uint64_t seq, const char *type,
                    const json_t *data, char *error, size_t error_size)
{
    struct process_read_range *read = opaque;
    uint64_t stream, offset;
    int rc = -1;
    (void)state;
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
    struct snag_buf bytes = {.max = 16384u};
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

/* Owner loss invalidates the OS handle, not the bytes already in the journal. */
int
snag_app_recovered_output(struct app_state *app, const char *handle, json_t *result)
{
    struct snag_process_state *process = snag_session_process(&app->session, handle);
    if (!process || json_object_get(result, "output_ref") ||
        strcmp(snag_json_string(result, "status"), "outcome_unknown"))
        return 0;
    struct snag_buf message = {.max = 32768u};
    int rc = -1;
    if (snag_buf_printf(&message, "Tool outcome is unknown: owner_lost. The old handle is invalid. "
            "Do not repeat ambiguous effects without inspecting current state. "
            "Captured output remains in %s/events.jsonl.\n", app->session.dir_path) < 0)
        goto out;
    const char *names[] = {"stdout", "stderr"};
    for (unsigned int stream = 0u; stream < 2u; ++stream) {
        struct snag_buf bytes = {.max = 6000u}, encoded = {.max = 8000u};
        uint64_t from = process->collected_bytes[stream], to = process->output_bytes[stream];
        if (snag_app_tool_read(app, handle, stream, from, to, &bytes) < 0) {
            snag_buf_free(&bytes); goto out;
        }
        bool utf8 = snag_utf8_valid(bytes.data, bytes.len, true);
        int erc = utf8 ? snag_buf_append(&encoded, bytes.data, bytes.len) :
            snag_base64_append(&encoded, bytes.data, bytes.len);
        if (erc == 0) erc = snag_json_set_new(result, names[stream],
            json_pack("{s:I,s:s,s:I,s:s%,s:I}",
                "discarded_bytes", (json_int_t)(to - from - bytes.len),
                "encoding", utf8 ? "utf8" : "base64", "original_bytes", (json_int_t)(to - from),
                "retained", encoded.len ? (char *)encoded.data : "", encoded.len,
                "retained_bytes", (json_int_t)bytes.len));
        if (erc == 0 && encoded.len) erc = snag_buf_printf(&message, "\n%s%s:\n%.*s\n",
            names[stream], utf8 ? "" : " (base64)", (int)encoded.len, (char *)encoded.data);
        if (erc == 0 && to - from > bytes.len)
            erc = snag_buf_printf(&message, "[%llu bytes omitted between the retained head and tail]\n",
                                 (unsigned long long)(to - from - bytes.len));
        snag_buf_free(&bytes); snag_buf_free(&encoded);
        if (erc < 0) goto out;
    }
    if (snag_json_set_new(result, "model_text", json_stringn((char *)message.data, message.len)) < 0 ||
        snag_json_set_new(result, "max_output_tokens", json_integer(16000)) < 0 ||
        snag_json_set_new(result, "output_ref", json_pack("{s:s,s:I,s:I,s:I,s:I,s:I,s:I,s:I,s:b,s:I,s:I}",
            "handle", handle, "stdout_start", (json_int_t)process->collected_bytes[0],
            "stdout_end", (json_int_t)process->output_bytes[0],
            "stderr_start", (json_int_t)process->collected_bytes[1],
            "stderr_end", (json_int_t)process->output_bytes[1],
            "stdin_accepted", (json_int_t)process->input_accepted,
            "stdin_written", (json_int_t)process->input_written,
            "stdin_pending", (json_int_t)process->input_pending, "stdin_open", 0,
            "log_start", (json_int_t)process->log_offset,
            "log_end", (json_int_t)app->session.log_end)) < 0)
        goto out;
    rc = 0;
out:
    snag_buf_free(&message);
    return rc;
}
