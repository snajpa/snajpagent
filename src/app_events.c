/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"

#include "context.h"
#include "media.h"
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
    if (snag_buf_reserve(pending, len + 1u) < 0 || snag_buf_append(pending, text, len) < 0) return -1;
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

        if (len > SNAG_MAX_STEERING_TEXT - batch.len) break;
        if (snag_buf_append(&batch, text, len) < 0) goto fail;
        *used += len + 1u;
    }
    if (*used && snag_buf_terminate(&batch) == 0) return (char *)batch.data;
fail: snag_buf_free(&batch);
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
        for (size_t i = 0u; i < refs->len / sizeof(*items); ++i) items[i].end -= used;
    }
    return 0;
}

bool
snag_app_irc_prompt(const char *text)
{
    /* The admission batch is the prompt of an IRC-triggered turn: runtime
     * plumbing rather than text the operator submitted. Callers must not echo
     * it as a submission, and it carries no conversation-level formatting. */
    return text && (!strncmp(text, "[IRC update id=", 15u) ||
                    !strncmp(text, "[IRC endpoint=", 14u));
}

static int
append_irc_projection(struct snag_buf *pending, const struct snag_irc_event *event)
{
    struct snag_buf line;
    char when[32u];
    time_t seconds = (time_t)(event->timestamp_ms / 1000u);
    struct tm tm;
    int rc = -1;

    if (!snag_gmtime(&seconds, &tm) || strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0)
        memcpy(when, "1970-01-01T00:00:00Z", 21u);
    snag_buf_init(&line, SNAG_IRC_TEXT_MAX + SNAG_CONFIG_IRC_ENDPOINT_MAX +
                         SNAG_CONFIG_IRC_ROOM_MAX + SNAG_CONFIG_IRC_NICK_MAX + 256u);
    if (event->stream[0] || event->historical) {
        /* This batch text becomes the turn prompt: the operator reads it during
         * replay and the model receives it as the turn's user message. It has
         * to stand on its own, so name the update instead of pointing at a room
         * event the reader may not have; the durable id still ties it back. */
        rc = snag_buf_printf(&line, "[IRC update id=%s:%llu endpoint=%s room=%s event=%s sender=%s]\n",
            event->stream, (unsigned long long)event->sequence, event->endpoint, event->room,
            snag_irc_kind_name(event->kind), event->nick[0] ? event->nick : "server");
        if (rc == 0) rc = append_pending(pending, (const char *)line.data, line.len);
        snag_buf_free(&line);
        return rc;
    }
    if (snag_buf_printf(&line, "[IRC endpoint=%s room=%s time=%s event=%s sender=%s operator=%s]\n%s\n",
            event->endpoint, event->room, when, snag_irc_kind_name(event->kind),
            event->nick[0] ? event->nick : "server", event->op ? "true" : "false", event->text) < 0 ||
        append_pending(pending, (const char *)line.data, line.len) < 0) goto out;
    rc = 0;
out: snag_buf_free(&line);
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
    if (add && route->count < SNAG_IRC_DESTINATIONS_MAX) route->targets[route->count++] = target;
}

static void
prune_replies(struct snag_irc_route *route, const struct snag_irc_destinations *destinations, size_t *offsets)
{
    size_t kept = 0u;
    for (size_t i = 0u; i < route->count; ++i)
        for (size_t j = 0u; j < destinations->count; ++j)
            if (route->targets[i].id == destinations->items[j].target.id &&
                route->targets[i].revision == destinations->items[j].target.revision) {
                if (offsets) offsets[kept] = offsets[i];
                route->targets[kept++] = route->targets[i];
            }
    route->count = kept;
}

int
snag_app_sync_destinations(struct app_state *app)
{
    struct snag_irc_destinations current;
    uint64_t generation = snag_irc_destinations_generation(app->irc);

    /* Rebuilding and comparing the whole destination set only means something
     * after the runtime mutated one; the pump runs inside read-tool walk
     * checkpoints, where the rebuild costs more than the walk it serves. */
    if (app->irc_destinations_ready &&
        generation == app->irc_destinations_generation)
        return 0;
    snag_irc_destinations(app->irc, &current);
    if (app->irc_destinations_ready &&
        memcmp(&current, &app->irc_destinations, sizeof(current)) == 0) {
        app->irc_destinations_generation = generation;
        return 0;
    }
    if (snag_ui_send(&app->ui, (struct snag_ui_command){
        .kind = SNAG_UI_DESTINATIONS, .data.destinations = &current}) < 0) return -1;
    prune_replies(&app->irc_urgent_replies, &current, app->irc_urgent_reply_offsets);
    prune_replies(&app->irc_turn_replies, &current, NULL);
    app->irc_destinations = current;
    app->irc_destinations_generation = generation;
    app->irc_destinations_ready = true;
    return 0;
}

int
snag_app_irc_snapshot(struct app_state *app, const char *reason, char *error, size_t error_size)
{
    int rc = -1;

    if (!app || !app->irc || !reason) return snag_errno(EINVAL);
    if (snag_app_sync_destinations(app) < 0) return -1;
    struct snag_buf snapshot = {.max = SNAG_MAX_IRC_SNAPSHOT};
    rc = strcmp(reason, "compaction") != 0 ? snag_irc_state(app->irc, &snapshot, error, error_size) :
        snag_irc_snapshot(app->irc, &snapshot, error, error_size);
    if (rc < 0) goto out;
    rc = -1;
    if (snag_buf_terminate(&snapshot) < 0) goto out;
    rc = snag_app_commit_event(app, "irc_snapshot", json_pack("{s:s,s:s,s:I}", "reason", reason,
                  "text", (const char *)snapshot.data,
                  "timestamp_ms", (json_int_t)snag_time_ms()), error, error_size);
out: snag_buf_free(&snapshot);
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

    if (!app || !event) return -1;
    if (snag_app_sync_destinations(app) < 0) return -1;
    struct snag_irc_event accepted = *event;
    accepted.input = !snag_irc_local_identity(app->irc, event, true) &&
        event->kind != SNAG_IRC_HISTORY_READY && (event->stream[0] || event->historical);
    accepted.classified = true;
    accepted.urgent = (event->kind == SNAG_IRC_MESSAGE || event->kind == SNAG_IRC_NOTICE) &&
        !event->historical && snag_irc_mentions_agent(app->irc, event->endpoint, event->text);
    accepted.reply = accepted.urgent && snag_irc_local_identity(app->irc, event, false);
    uint64_t accepted_seq = app->session.next_seq;
    if (snag_app_commit_event(app, "irc_event", snag_irc_event_data(&accepted), error, sizeof(error)) < 0)
        return -1;
    if (snag_ui_send(&app->ui, (struct snag_ui_command){
        .kind = SNAG_UI_IRC, .data.irc = event}) < 0) return -1;
    if (event->kind == SNAG_IRC_DISCONNECTED &&
        strstr(event->text, "endpoint removed; discarded ") == event->text &&
        snag_ui_text(&app->ui, SNAG_UI_WARNING, event->text) < 0) return -1;
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
        if (snag_app_irc_snapshot(app, "join", error, sizeof(error)) < 0) return -1;
    }

    if (chat) ++app->input_generation;
    urgent = chat && !event->historical && snag_irc_mentions_agent(app->irc, event->endpoint, event->text);
    reply_offset = app->irc_urgent.len;
    if (append_irc_projection(urgent ? &app->irc_urgent : &app->irc_background, event) < 0) return -1;
    if (accepted.input && event->historical) {
        /* Catch-up is background context, but unlike newly arriving ordinary
         * chat it is available at the existing join-history response boundary. */
        if (snag_app_commit_event(app, "irc_admitted", json_pack("{s:[I]}",
                "sequences", (json_int_t)accepted_seq), error, sizeof(error)) < 0) return -1;
    } else if (accepted.input) {
        struct irc_input_ref ref = {accepted_seq, urgent ? app->irc_urgent.len : app->irc_background.len};
        if (snag_buf_append(urgent ? &app->irc_urgent_refs : &app->irc_background_refs,
                           &ref, sizeof(ref)) < 0) return -1;
    }
    if (urgent && local_operator && snag_irc_event_target(app->irc, event, &target)) {
        size_t before = app->irc_urgent_replies.count;
        reply_target(&app->irc_urgent_replies, target, true);
        if (app->irc_urgent_replies.count > before) app->irc_urgent_reply_offsets[before] = reply_offset;
    }
    if (!urgent && !app->irc_background_since_ms) app->irc_background_since_ms = snag_time_ms();
    return 0;
}

int
snag_app_irc_trace(void *opaque, unsigned int level, char direction,
                  const char *endpoint, const char *text, size_t len)
{
    struct app_state *app = opaque;
    char label[384u];
    int rc = -1;

    if (!app || !endpoint || !text || (level != 5u && level != 6u) || (direction != '<' && direction != '>'))
        return snag_errno(EINVAL);
    if (!snag_ui_enabled(&app->ui, level == 6u ? SNAG_PRESENT_WIRE : SNAG_PRESENT_PROTOCOL)) return 0;
    struct snag_buf safe = {.max = 4u * SNAG_IRC_LINE_MAX};
    if (level == 6u) {
        safe.max += SNAG_CONFIG_IRC_ENDPOINT_MAX + 8u;
        if (snag_buf_printf(&safe, "IRC [%s] ", endpoint) < 0) goto out;
        if (safe.max > safe.len + 4u * SNAG_IRC_LINE_MAX) safe.max = safe.len + 4u * SNAG_IRC_LINE_MAX;
    }
    for (size_t i = 0u; i < len; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20u || c == 0x7fu) {
            if (snag_buf_printf(&safe, "\\x%02X", (unsigned int)c) < 0) goto out;
        } else if (snag_buf_putc(&safe, c) < 0) {
            goto out;
        }
    }
    if (level == 6u) {
        rc = snag_ui_send(&app->ui, (struct snag_ui_command){
            .kind = SNAG_UI_TRANSPORT, .data.value = direction, .text = (const char *)safe.data, .len = safe.len});
    } else {
        int n = snprintf(label, sizeof(label), "irc.command %c %s", direction, endpoint);
        if (n < 0 || (size_t)n >= sizeof(label)) {
            errno = EOVERFLOW;
            goto out;
        }
        rc = snag_ui_send(&app->ui, (struct snag_ui_command){
            .kind = SNAG_UI_PROTOCOL, .label = label, .text = (const char *)safe.data, .len = safe.len});
    }
out: snag_buf_free(&safe);
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
snag_app_irc_flush_urgent(struct app_state *app, char *error, size_t error_size)
{
    char steering_id[SNAG_ID_HEX_LEN + 1u];
    size_t used;
    char *text;
    int rc;
    const struct snag_model_limit_config *limit;
    bool admit_all;

    if (!app || !app->session.active_turn) return 0;
    limit = snag_config_model_limit_exact(app->config, app->session.active_turn_provider,
        app->config->model);
    admit_all = (app->session.steering_override && *app->session.steering_override) ?
        strcmp(app->session.steering_override, "all") == 0 :
        (limit && strcmp(limit->steering, "all") == 0);
    if (!app->irc_urgent.len && !(admit_all && app->irc_background.len)) return 0;
    if (app->irc_urgent.len) {
        if (snag_random_id(steering_id) < 0 || !(text = pending_batch(&app->irc_urgent, &used)))
            return -1;
        rc = admit_irc_input(app, &app->irc_urgent_refs, used, "steering_added",
                snag_app_steering_added_data(app->session.active_turn_id, steering_id, text),
                error, error_size);
        free(text);
        if (rc < 0) return -1;
        consume_pending(&app->irc_urgent, used);
        admit_replies(app, used);
    }
    if (admit_all && app->irc_background.len) {
        if (snag_random_id(steering_id) < 0 ||
            !(text = pending_batch(&app->irc_background, &used))) return -1;
        rc = admit_irc_input(app, &app->irc_background_refs, used, "steering_added",
                snag_app_steering_added_data(app->session.active_turn_id, steering_id, text),
                error, error_size);
        free(text);
        if (rc < 0) return -1;
        consume_pending(&app->irc_background, used);
        if (!app->irc_background.len) app->irc_background_since_ms = 0u;
    }
    return 0;
}

char *
snag_app_irc_take_pending(struct app_state *app, bool *local_operator, bool force_background)
{
    struct snag_buf *source;
    char *copy;
    size_t used;

    if (local_operator) *local_operator = false;
    /* Background room traffic waits for the current turn. Admitting a second
       input_received while one is active is an invalid store transition, which
       used to take the whole session down as soon as a peer joined the room. */
    if (!app || app->session.active_turn || app->session.pending_input) return NULL;
    if (app->irc_urgent.len) {
        source = &app->irc_urgent;
        if (local_operator) *local_operator = app->irc_urgent_replies.count != 0u;
    /* A paused or blocked persistent goal is an explicit idle boundary. Keep
     * ordinary room traffic pending until the operator resumes or submits new
     * work; direct mentions remain urgent and may still start a turn. */
    } else if (app->session.goal_status == SNAG_GOAL_PAUSED ||
               app->session.goal_status == SNAG_GOAL_BLOCKED) {
        return NULL;
    /* Startup/history alone must not turn an unused session into saved work. */
    } else if (app->session.log_fd >= 0 && app->irc_background.len && (force_background ||
                snag_time_ms() - app->irc_background_since_ms >= 100u)) {
        source = &app->irc_background;
    } else {
        return NULL;
    }
    copy = pending_batch(source, &used);
    if (!copy) return NULL;
    char error[256] = {0};
    if (admit_irc_input(app, source == &app->irc_urgent ? &app->irc_urgent_refs :
                        &app->irc_background_refs, used, "input_received",
                        snag_app_input_received_data(app, copy, false, false),
                        error, sizeof(error)) < 0) {
        free(copy);
        (void)snag_ui_text(&app->ui, SNAG_UI_ERROR, error);
        return NULL;
    }
    consume_pending(source, used);
    app->irc_turn_replies.count = 0u;
    if (source == &app->irc_urgent) {
        admit_replies(app, used);
        if (local_operator) *local_operator = app->irc_turn_replies.count != 0u;
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
                  uint64_t seq, const char *type, const json_t *data, char *error, size_t error_size)
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

    if (!array) return NULL;
    for (size_t i = 0; i < session->pending_steering_count; ++i) {
        if (!session->pending_steering[i].first_context_ms) continue;
        json_t *item = json_pack("{s:s,s:s}", "id", session->pending_steering[i].steering_id,
            "text", session->pending_steering[i].text);
        json_t *content = session->pending_steering[i].content;
        if (content && snag_json_set_new(item, "content", json_incref((json_t *)content)) < 0) {
            json_decref(item); json_decref(array); return NULL;
        }
        if (!item || json_array_append_new(array, item) < 0) {
            json_decref(array);
            return NULL;
        }
    }
    return array;
}

static void
visible_detail(char text[80], enum snag_presentation kind, unsigned int level, enum snag_render_view view)
{
    size_t limit = snag_presentation_limit(kind, level);
    if (!snag_presentation_enabled(kind, level, view)) (void)snprintf(text, 80u, "hidden");
    else if (limit == SIZE_MAX) (void)snprintf(text, 80u, "full (subject to capture/display limits)");
    else (void)snprintf(text, 80u, "preview (up to %zu characters)", limit);
}

static int
operator_visibility(const struct app_state *app, char *text, size_t size)
{
    unsigned int level = snag_ui_verbosity(&app->ui);
    enum snag_render_view view = snag_ui_view(&app->ui);
    bool rows = snag_presentation_enabled(SNAG_PRESENT_TOOL, level, view);
    bool full = snag_presentation_limit(SNAG_PRESENT_ARGUMENTS, level) == SIZE_MAX &&
                snag_presentation_limit(SNAG_PRESENT_OUTPUT, level) == SIZE_MAX;
    char arguments[80], output[80];
    visible_detail(arguments, SNAG_PRESENT_ARGUMENTS, level, view);
    visible_detail(output, SNAG_PRESENT_OUTPUT, level, view);
    const char *guidance = view == SNAG_RENDER_CHAT ?
        "Chat view hides private rollout even at high verbosity. Keep useful private progress in rollout; "
        "a view setting never authorizes sending local details to IRC or changing destinations." :
        !rows ? "Tool details are hidden: give concise commentary before meaningful work and at progress/outcome checkpoints, "
                "so the conversation carries the work. Do not narrate every small call or fill waits with generic status." :
        !full ? "Brief rows/previews can omit important details. Explain the purpose and significance of work and results "
                     "without repeating visible tool rows." :
        "Detailed tool traces are available: reduce redundant per-call narration, while retaining useful milestones and explanations.";
    int n = snprintf(text, size,
        "Local operator display snapshot: verbosity=%u (%s); view=%s; rollout text=%s; "
        "tool rows=%s; arguments=%s; output=%s; terminal tool-output cap=%u bytes (0=unlimited). "
        "%s This is current presentation state, superseding older display snapshots; it is not proof the operator read every detail. "
        "%s Always communicate important decisions, blockers, risks and final outcomes. "
        "This affects progress narration, not diligence, permissions, requested answer length or private reasoning disclosure.",
        level, snag_verbosity_name(level), view == SNAG_RENDER_CHAT ? "chat" : "rollout",
        snag_presentation_enabled(SNAG_PRESENT_CONVERSATION, level, view) ? "visible" : "hidden",
        rows ? "visible (brief start/outcome rows)" : "hidden", arguments, output, app->config->max_output_bytes,
        app->execute ? "In one-shot mode final answers go to stdout; commentary and diagnostics use stderr, which may be redirected." :
                       "Interactive mode uses the selected local view; off-screen or truncated details may not have been seen.",
        guidance);
    return n >= 0 && (size_t)n < size ? 0 : snag_errno(EOVERFLOW);
}

bool
snag_app_context_cancelled(void *opaque)
{
    struct app_state *app = opaque;
    return app->interrupt_requested || snag_app_shutdown(app) || snag_ui_leaving(&app->ui) ||
        (!app->queue_edit_id[0] && snag_ui_interrupt_pending(&app->ui));
}

int
snag_app_request_build(struct app_state *app, const json_t *steering, unsigned int cycle,
                       const struct snag_credential *credential, struct snag_context_projection *projection,
                       const char **count_method, struct snag_buf *request_body,
                       char *error, size_t error_size)
{
    int rc;
    const struct snag_context_control control = {
        .cancelled = snag_app_context_cancelled,
        .opaque = app,
        .history_orientation = app->history_orientation,
        .goal_recovery_rebase = app->history_recovery_rebase
    };

    app->request_networked = app->networked && !app->session.active_read_only;
    snag_irc_capture_route(app->irc, &app->irc_request_route);
    char visibility[2048];
    if (operator_visibility(app, visibility, sizeof(visibility)) < 0)
        return snag_errorf(error, error_size, "cannot describe operator visibility");
    char continuation_scope[SNAG_SHA256_HEX_LEN + 1u];
    if (snag_context_continuation_scope(app->turn_provider, app->turn_model,
                                       credential, continuation_scope) < 0)
        return snag_errorf(error, error_size, "cannot bind provider continuation");
    if (snag_app_provider_activity(app, true) < 0) return -1;
    rc = snag_context_build(&app->session, app->turn_model, app->turn_effort,
        cycle, steering, app->turn_capacity.max_output_tokens,
        app->turn_capacity.max_output_tokens, app->config, continuation_scope,
        &app->turn_instructions, visibility, projection, error, error_size, &control);
    int context_errno = errno;
    bool cancelled = rc < 0 && errno == ECANCELED;
    if (snag_app_provider_activity(app, false) < 0) return -1;
    if (cancelled) (void)snag_app_active_input_pump(app, 0u);
    if (rc < 0) {
        errno = context_errno;
        return -1;
    }
    memcpy(projection->continuation_scope, continuation_scope, sizeof(projection->continuation_scope));
    *count_method = "unknown";
    rc = 0;
    if (snag_ui_enabled(&app->ui, SNAG_PRESENT_PROTOCOL)) {
        struct snag_secret_set secrets = {0};

        snag_buf_init(request_body, SNAG_WIRE_BODY_MAX);
        struct snag_buf encoded = {.max = SNAG_WIRE_BODY_MAX};
        if (snag_secret_set_build(&secrets, app->config, credential, error, error_size) < 0 ||
            snag_media_request_has_images(projection->create_request.value) || projection->create_request.bytes > SNAG_WIRE_BODY_MAX ||
            snag_json_canonical(projection->create_request.value, &encoded) < 0 ||
            snag_wire_json_redact(encoded.data, encoded.len, &secrets.wire,
                                 request_body, error, error_size) < 0) {
            snag_buf_reset(request_body);
            rc = snag_buf_printf(request_body, "<request body omitted; bytes=%zu; sha256=%s>\n",
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
snag_app_response_started_data(const struct app_state *app, const char *turn_id, const char *response_id,
                               unsigned int cycle, const struct snag_context_projection *projection,
                               const char *count_method, const char *provider_source_sha256,
                               const json_t *steering)
{
    const struct snag_model_capacity *capacity = &app->turn_capacity;
    const char *compact_id = app->session.compact_id;
    const char *baseline = NULL;
    json_t *ids = json_array();
    json_t *data = NULL;

    if (!ids || !json_is_array(steering) || !snag_hex_is_lower(provider_source_sha256, SNAG_SHA256_HEX_LEN))
        goto out;
    for (size_t i = 0; i < json_array_size(steering); ++i) {
        const char *id = snag_json_string(json_array_get(steering, i), "id");
        if (!id || json_array_append_new(ids, json_string(id)) < 0) goto out;
    }
    data = json_pack( "{s:I,s:s?,s:s,s:s?,s:s,s:s,s:s,s:I,s:s,s:o,s:I,s:s,s:I,s:s,"
        "s:s,s:s,s:s,s:I,s:I,s:s,s:o,s:s,s:s,s:b,s:O,s:s}",
        "irc_seq", (json_int_t)projection->irc_seq, "baseline_sha256", baseline,
        "capability_version", SNAJPAGENT_CAPABILITY_VERSION, "compact_id", *compact_id ? compact_id : NULL,
        "count_method", count_method, "count_request_sha256", projection->count_request.sha256,
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
    if (data && projection->host_context &&
        json_object_set(data, "host_context", projection->host_context) < 0) {
        json_decref(data);
        data = NULL;
    }
out: json_decref(ids);
    return data;
}

json_t *
snag_app_response_capacity_rejected_data( const char *turn_id, const char *response_id, unsigned int cycle,
    const char *request_hash, const struct snag_provider_failure *failure,
    const struct snag_model_capacity *capacity, const char *provider_source_sha256)
{
    if (!turn_id || !response_id || !request_hash || !failure || !capacity || !provider_source_sha256 ||
        !snag_hex_is_lower(provider_source_sha256, SNAG_SHA256_HEX_LEN)) return NULL;
    uint64_t safety_ceiling = snag_capacity_safety_ceiling(
        failure->context_limit_tokens, failure->requested_input_tokens, capacity->max_output_tokens);
    return json_pack("{s:s,s:o,s:I,s:s,s:o,s:s,s:s,s:o,s:s,s:s}",
        "code", failure->code, "context_limit_tokens", failure->context_limit_tokens ?
            json_integer((json_int_t)failure->context_limit_tokens) : json_null(),
        "cycle", (json_int_t)cycle, "message", failure->message,
        "observed_hard_input_tokens", safety_ceiling ? json_integer((json_int_t)safety_ceiling) : json_null(),
        "provider_source_sha256", provider_source_sha256, "request_sha256", request_hash,
        "requested_input_tokens", failure->requested_input_tokens ?
            json_integer((json_int_t)failure->requested_input_tokens) : json_null(),
        "response_id", response_id, "turn_id", turn_id);
}

json_t *
snag_app_turn_completed_data(const char *turn_id, const char *response_id, const char *item_id)
{
    return json_pack("{s:s,s:s,s:s}", "final_item_id", item_id,
        "final_response_id", response_id, "turn_id", turn_id);
}

json_t *
snag_app_steering_added_data(const char *turn_id, const char *steering_id, const char *text)
{
    return json_pack("{s:s,s:s,s:s}", "steering_id", steering_id, "text", text, "turn_id", turn_id);
}

json_t *
snag_app_response_interrupted_data(const char *turn_id, const char *response_id,
                          unsigned int cycle, const char *origin, const char *reason, json_t *partial_public)
{
    return json_pack("{s:I,s:s,s:o,s:s,s:s,s:s}", "cycle", (json_int_t)cycle, "origin", origin,
        "partial_public", partial_public ? partial_public : json_array(),
        "reason", reason, "response_id", response_id, "turn_id", turn_id);
}

json_t *
snag_app_turn_failed_data(const char *turn_id, const char *class_name, const char *message)
{
    return json_pack("{s:s,s:s,s:s}", "class", class_name, "message", message, "turn_id", turn_id);
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
    if (!utf8 && snag_base64_append(&encoded, bytes, len) < 0) goto out;
    if (app->output_cache.valid && !strcmp(app->output_cache.handle, handle) &&
        app->output_cache.stream == stream) app->output_cache.valid = false;
    event = json_pack("{s:s,s:s,s:i,s:I,s:s,s:s%}", "turn_id", app->session.active_turn_id, "handle", handle,
        "stream", (int)stream, "offset", (json_int_t)offset, "encoding", utf8 ? "utf8" : "base64",
        "data", (const char *)(utf8 ? bytes : (const void *)encoded.data), utf8 ? len : encoded.len);
    if (event) rc = snag_app_commit_event(app, "process_output", event, error, sizeof(error));
out: snag_buf_free(&encoded);
    return rc;
}

struct process_read_range {
    const char *handle;
    unsigned int stream;
    uint64_t from, to, seen;
    struct snag_buf *out;
};

static int
read_process_chunk(void *opaque, const struct snag_session *state, uint64_t seq, const char *type,
                    const json_t *data, char *error, size_t error_size)
{
    struct process_read_range *read = opaque;
    uint64_t stream, offset;
    int rc = -1;
    (void)state;
    (void)seq;
    (void)error;
    (void)error_size;
    if (strcmp(type, "process_output") || !snag_json_string(data, "handle") ||
        strcmp(snag_json_string(data, "handle"), read->handle)) return 0;
    if (snag_json_integer_u64(data, "stream", &stream) < 0 ||
        snag_json_integer_u64(data, "offset", &offset) < 0) return -1;
    if (stream != read->stream) return 0;
    struct snag_buf bytes = {.max = 16384u};
    if (snag_process_output_decode(data, &bytes) < 0 || offset > UINT64_MAX - bytes.len) goto out;
    uint64_t end = offset + bytes.len;
    uint64_t from = offset > read->from ? offset : read->from;
    uint64_t to = end < read->to ? end : read->to;
    if (from < to) {
        if (from != read->from + read->seen) goto out;
        read->seen += to - from;
        uint64_t ranges[4] = {read->from, read->to, read->to, read->to};
        if (read->to - read->from > read->out->max) {
            ranges[1] = read->from + read->out->max / 2u;
            ranges[2] = read->to - (read->out->max - read->out->max / 2u);
        }
        for (unsigned int i = 0u; i < 4u; i += 2u) {
            uint64_t a = from > ranges[i] ? from : ranges[i];
            uint64_t b = to < ranges[i + 1u] ? to : ranges[i + 1u];
            if (a < b && snag_buf_append(read->out, bytes.data + (size_t)(a - offset), (size_t)(b - a)) < 0)
                goto out;
        }
    }
    rc = 0;
out: snag_buf_free(&bytes);
    return rc;
}

int
snag_app_tool_read(void *opaque, const char *handle, unsigned int stream,
                   uint64_t from, uint64_t to, struct snag_buf *out)
{
    struct app_state *app = opaque;
    struct snag_process_state *process = snag_session_process(&app->session, handle);
    struct process_read_range read = {.handle = handle, .stream = stream, .from = from, .to = to, .out = out};
    char error[256] = {0};
    if (!process || from > to || snag_session_each_event_since(&app->session, process, read_process_chunk,
                                      &read, error, sizeof(error)) < 0) return -1;
    return read.seen == to - from ? 0 : -1;
}

struct process_output_scan {
    const char *handle;
    unsigned int stream;
    uint64_t from, retain, total;
    bool known;
    struct snag_buf *out;
};

static int
scan_process_output(void *opaque, const struct snag_session *state, uint64_t seq, const char *type,
                    const json_t *data, char *error, size_t error_size)
{
    struct process_output_scan *scan = opaque;
    const json_t *result = NULL, *ref = NULL;
    const char *handle;
    uint64_t stream, offset;
    (void)state;
    (void)seq;
    (void)error;
    (void)error_size;

    if (!strcmp(type, "tool_finished") || !strcmp(type, "process_closed"))
        result = json_object_get(data, "result");
    if (result) ref = json_object_get(result, "output_ref");
    if (ref && (handle = snag_json_string(ref, "handle")) && !strcmp(handle, scan->handle)) {
        uint64_t end;
        scan->known = true;
        if (snag_json_integer_u64(ref, scan->stream ? "stderr_end" : "stdout_end", &end) < 0)
            return -1;
        if (end > scan->total) scan->total = end;
    }
    if (strcmp(type, "process_output") || !(handle = snag_json_string(data, "handle")) ||
        strcmp(handle, scan->handle)) return 0;
    if (snag_json_integer_u64(data, "stream", &stream) < 0 ||
        snag_json_integer_u64(data, "offset", &offset) < 0) return -1;
    if (stream != scan->stream) return 0;
    struct snag_buf bytes = {.max = 16384u};
    int rc = -1;
    if (snag_process_output_decode(data, &bytes) < 0 || offset > UINT64_MAX - bytes.len) goto out;
    uint64_t end = offset + bytes.len;
    scan->known = true;
    if (end > scan->total) scan->total = end;
    uint64_t window_end = scan->retain > UINT64_MAX - scan->from ?
        UINT64_MAX : scan->from + scan->retain;
    if (scan->retain && scan->from < end && offset < window_end) {
        uint64_t from = offset > scan->from ? offset : scan->from;
        uint64_t to = end < window_end ? end : window_end;
        if (from < to && snag_buf_append(scan->out, bytes.data + (size_t)(from - offset),
                                         (size_t)(to - from)) < 0) goto out;
    }
    rc = 0;
out:
    snag_buf_free(&bytes);
    return rc;
}

#define APP_HISTORY_LIMIT_MAX 50u
#define APP_HISTORY_DETAIL_MAX 2048u

struct app_history_record {
    uint64_t seq;
    char *type;
    char *data;
};

struct app_history_scan {
    struct app_history_record records[APP_HISTORY_LIMIT_MAX];
    uint64_t before_seq;
    size_t limit, detail_bytes, count, total;
};

static char *
history_excerpt(const char *text, size_t limit)
{
    size_t len = text ? strlen(text) : 0u;
    bool clipped = len > limit;
    size_t use = clipped ? limit : len;
    while (use && !snag_utf8_valid((const unsigned char *)text, use, true)) --use;
    char *copy = malloc(use + (clipped ? 4u : 1u));
    if (!copy) return NULL;
    if (use) memcpy(copy, text, use);
    if (clipped) memcpy(copy + use, "...", 4u);
    else copy[use] = '\0';
    return copy;
}

static int
history_event(void *opaque, const struct snag_session *state, uint64_t seq,
              const char *type, const json_t *data, char *error, size_t error_size)
{
    struct app_history_scan *scan = opaque;
    char *encoded, *detail, *name;
    (void)state;
    (void)error;
    (void)error_size;
    if (scan->before_seq && seq >= scan->before_seq) return 0;
    encoded = json_dumps(data, JSON_COMPACT | JSON_SORT_KEYS | JSON_ENCODE_ANY);
    if (!encoded) return -1;
    detail = history_excerpt(encoded, scan->detail_bytes);
    free(encoded);
    name = snag_strdup_checked(type, 128u);
    if (!detail || !name) {
        free(detail);
        free(name);
        return -1;
    }
    ++scan->total;
    if (scan->count == scan->limit) {
        free(scan->records[0].type);
        free(scan->records[0].data);
        memmove(scan->records, scan->records + 1u,
                (scan->limit - 1u) * sizeof(scan->records[0]));
        --scan->count;
    }
    scan->records[scan->count++] = (struct app_history_record){
        .seq = seq, .type = name, .data = detail};
    return 0;
}

static void
history_scan_free(struct app_history_scan *scan)
{
    for (size_t i = 0u; i < scan->count; ++i) {
        free(scan->records[i].type);
        free(scan->records[i].data);
    }
}

int
snag_app_history_page(struct app_state *app, const struct snag_response_item *call, json_t **result,
                      char *error, size_t error_size)
{
    uint64_t before = 0u, limit = 20u, detail = 512u;
    struct app_history_scan scan = {0};
    struct snag_buf text = {.max = app->session.tool_output_bytes + 1u};
    struct snag_buf body = {.max = app->session.tool_output_bytes};
    int rc = -1;

    *result = NULL;
    if (!snag_json_arg_keys(call->arguments, "", "before_seq limit detail_bytes", error, error_size) ||
        !snag_json_arg_uint(call->arguments, "before_seq", 0u, 1u, UINT64_MAX,
                            &before, error, error_size) ||
        !snag_json_arg_uint(call->arguments, "limit", 20u, 1u, APP_HISTORY_LIMIT_MAX,
                            &limit, error, error_size) ||
        !snag_json_arg_uint(call->arguments, "detail_bytes", 512u, 128u, APP_HISTORY_DETAIL_MAX,
                            &detail, error, error_size))
        goto invalid;
    scan.before_seq = before;
    scan.limit = (size_t)limit;
    scan.detail_bytes = (size_t)detail;
    if (snag_session_each_event(&app->session, history_event, &scan, error, error_size) < 0) goto out;
    uint64_t latest = app->session.next_seq ? app->session.next_seq - 1u : 0u;
    size_t returned = 0u;
    uint64_t oldest_returned = 0u;
    for (size_t i = scan.count; i > 0u; --i) {
        struct app_history_record *record = &scan.records[i - 1u];
        struct snag_buf line = {.max = strlen(record->data) + strlen(record->type) + 64u};
        int line_rc = snag_buf_printf(&line, "%llu %s %s\n", (unsigned long long)record->seq,
                                      record->type, record->data);
        if (line_rc < 0 || body.len > app->session.tool_output_bytes ||
            line.len > app->session.tool_output_bytes - body.len ||
            body.len + line.len + 512u > app->session.tool_output_bytes) {
            snag_buf_free(&line);
            break;
        }
        if (snag_buf_append(&body, line.data, line.len) < 0) {
            snag_buf_free(&line);
            goto out;
        }
        snag_buf_free(&line);
        ++returned;
        oldest_returned = record->seq;
    }
    uint64_t next_before = scan.total > returned ? oldest_returned : 0u;
    if (snag_buf_printf(&text,
            "[session history id=%s latest_seq=%llu before_seq=%llu matched=%zu returned=%zu order=newest-first next_before_seq=%llu]\n",
            app->session.id, (unsigned long long)latest, (unsigned long long)before,
            scan.total, returned, (unsigned long long)next_before) < 0 ||
        snag_buf_append(&text, body.data, body.len) < 0) goto out;
    if (snag_buf_terminate(&text) < 0) goto out;
    *result = snag_tool_result_terminal(true, (const char *)text.data);
    rc = *result ? 0 : -1;
    goto out;
invalid:
    *result = snag_tool_result_terminal(false, error[0] ? error : "Invalid read_session_history arguments.");
    rc = *result ? 0 : -1;
out:
    history_scan_free(&scan);
    snag_buf_free(&body);
    snag_buf_free(&text);
    return rc;
}

struct app_goal_record {
    uint64_t created_seq, last_seq;
    char id[SNAG_ID_HEX_LEN + 1u];
    char parent[SNAG_ID_HEX_LEN + 1u];
    char replaced_by[SNAG_ID_HEX_LEN + 1u];
    char status[16];
    char *prompt;
    bool locked;
};

struct app_goal_scan {
    struct app_goal_record records[APP_HISTORY_LIMIT_MAX];
    uint64_t before_seq;
    size_t limit, count, total;
};

static struct app_goal_record *
goal_record_find(struct app_goal_scan *scan, const char *id)
{
    if (!id) return NULL;
    for (size_t i = 0u; i < scan->count; ++i)
        if (!strcmp(scan->records[i].id, id)) return &scan->records[i];
    return NULL;
}

static int
goal_record_add(struct app_goal_scan *scan, uint64_t seq, const char *id,
                const char *parent, const char *prompt, const char *status, bool locked)
{
    char *copy = history_excerpt(prompt, 512u);
    if (!copy) return -1;
    ++scan->total;
    if (scan->count == scan->limit) {
        free(scan->records[0].prompt);
        memmove(scan->records, scan->records + 1u,
                (scan->limit - 1u) * sizeof(scan->records[0]));
        --scan->count;
    }
    struct app_goal_record *record = &scan->records[scan->count++];
    memset(record, 0, sizeof(*record));
    record->created_seq = record->last_seq = seq;
    record->locked = locked;
    if (!snag_strcpy(record->id, sizeof(record->id), id) ||
        (parent && !snag_strcpy(record->parent, sizeof(record->parent), parent)) ||
        !snag_strcpy(record->status, sizeof(record->status), status)) {
        free(copy);
        --scan->count;
        return -1;
    }
    record->prompt = copy;
    return 0;
}

static int
goal_list_event(void *opaque, const struct snag_session *state, uint64_t seq,
                const char *type, const json_t *data, char *error, size_t error_size)
{
    struct app_goal_scan *scan = opaque;
    const char *id = snag_json_string(data, "goal_id");
    struct app_goal_record *record;
    (void)error;
    (void)error_size;

    if (!strcmp(type, "goal_started")) {
        if (!scan->before_seq || seq < scan->before_seq)
            return goal_record_add(scan, seq, id, NULL, snag_json_string(data, "prompt"),
                                   "active", false);
        return 0;
    }
    if (!strcmp(type, "goal_replaced")) {
        const char *new_id = snag_json_string(data, "new_goal_id");
        record = goal_record_find(scan, id);
        if (record) {
            (void)snag_strcpy(record->status, sizeof(record->status), "replaced");
            (void)snag_strcpy(record->replaced_by, sizeof(record->replaced_by), new_id);
            record->last_seq = seq;
        }
        if (!scan->before_seq || seq < scan->before_seq)
            return goal_record_add(scan, seq, new_id, id, snag_json_string(data, "prompt"),
                                   snag_goal_status_name(state->goal_status), state->goal_locked);
        return 0;
    }
    if (strncmp(type, "goal_", 5u) || !(record = goal_record_find(scan, id))) return 0;
    record->last_seq = seq;
    if (!strcmp(type, "goal_reworded")) {
        char *copy = history_excerpt(snag_json_string(data, "prompt"), 512u);
        if (!copy) return -1;
        free(record->prompt);
        record->prompt = copy;
    } else if (!strcmp(type, "goal_lock_changed")) {
        record->locked = json_is_true(json_object_get(data, "locked"));
    } else if (!strcmp(type, "goal_paused")) {
        (void)snag_strcpy(record->status, sizeof(record->status), "paused");
    } else if (!strcmp(type, "goal_blocked")) {
        (void)snag_strcpy(record->status, sizeof(record->status), "blocked");
    } else if (!strcmp(type, "goal_resumed")) {
        (void)snag_strcpy(record->status, sizeof(record->status), "active");
    } else if (!strcmp(type, "goal_completed")) {
        (void)snag_strcpy(record->status, sizeof(record->status), "completed");
    } else if (!strcmp(type, "goal_cancelled")) {
        (void)snag_strcpy(record->status, sizeof(record->status), "cancelled");
    }
    return 0;
}

int
snag_app_goal_list(struct app_state *app, const struct snag_response_item *call, json_t **result,
                   char *error, size_t error_size)
{
    uint64_t before = 0u, limit = 20u;
    struct app_goal_scan scan = {0};
    struct snag_buf text = {.max = app->session.tool_output_bytes + 1u};
    struct snag_buf body = {.max = app->session.tool_output_bytes};
    int rc = -1;

    *result = NULL;
    if (!snag_json_arg_keys(call->arguments, "", "before_seq limit", error, error_size) ||
        !snag_json_arg_uint(call->arguments, "before_seq", 0u, 1u, UINT64_MAX,
                            &before, error, error_size) ||
        !snag_json_arg_uint(call->arguments, "limit", 20u, 1u, APP_HISTORY_LIMIT_MAX,
                            &limit, error, error_size))
        goto invalid;
    scan.before_seq = before;
    scan.limit = (size_t)limit;
    if (snag_session_each_event(&app->session, goal_list_event, &scan, error, error_size) < 0) goto out;
    size_t returned = 0u;
    uint64_t oldest_returned = 0u;
    for (size_t i = scan.count; i > 0u; --i) {
        struct app_goal_record *record = &scan.records[i - 1u];
        struct snag_buf entry = {.max = (record->prompt ? strlen(record->prompt) : 0u) + 384u};
        int entry_rc = snag_buf_printf(&entry,
            "created=%llu last=%llu id=%s status=%s locked=%s%s%s%s%s\nprompt: %s\n",
            (unsigned long long)record->created_seq, (unsigned long long)record->last_seq,
            record->id, record->status, record->locked ? "true" : "false",
            record->parent[0] ? " parent=" : "", record->parent,
            record->replaced_by[0] ? " replaced_by=" : "", record->replaced_by,
            record->prompt ? record->prompt : "");
        if (entry_rc < 0 || body.len > app->session.tool_output_bytes ||
            entry.len > app->session.tool_output_bytes - body.len ||
            body.len + entry.len + 512u > app->session.tool_output_bytes) {
            snag_buf_free(&entry);
            break;
        }
        if (snag_buf_append(&body, entry.data, entry.len) < 0) {
            snag_buf_free(&entry);
            goto out;
        }
        snag_buf_free(&entry);
        ++returned;
        oldest_returned = record->created_seq;
    }
    uint64_t next_before = scan.total > returned ? oldest_returned : 0u;
    if (snag_buf_printf(&text,
            "[goals session=%s current=%s status=%s matched=%zu returned=%zu order=newest-first next_before_seq=%llu]\n",
            app->session.id, app->session.goal_id[0] ? app->session.goal_id : "none",
            snag_goal_status_name(app->session.goal_status), scan.total, returned,
            (unsigned long long)next_before) < 0 ||
        snag_buf_append(&text, body.data, body.len) < 0) goto out;
    if (snag_buf_terminate(&text) < 0) goto out;
    *result = snag_tool_result_terminal(true, (const char *)text.data);
    rc = *result ? 0 : -1;
    goto out;
invalid:
    *result = snag_tool_result_terminal(false, error[0] ? error : "Invalid list_goals arguments.");
    rc = *result ? 0 : -1;
out:
    for (size_t i = 0u; i < scan.count; ++i) free(scan.records[i].prompt);
    snag_buf_free(&body);
    snag_buf_free(&text);
    return rc;
}

static int
load_output_window(struct app_state *app, const char *handle, unsigned int stream,
                   uint64_t offset, size_t retain, struct snag_buf *out, uint64_t *total)
{
    struct process_output_scan scan = {
        .handle = handle, .stream = stream, .from = offset, .retain = retain, .out = out};
    char error[256] = {0};
    if (snag_session_each_event(&app->session, scan_process_output, &scan, error, sizeof(error)) < 0)
        return -1;
    if (!scan.known || offset > scan.total) return snag_errno(ENOENT);
    *total = scan.total;
    return 0;
}

int
snag_app_output_page(struct app_state *app, const struct snag_response_item *call, json_t **result,
                     char *error, size_t error_size)
{
    const char *handle = NULL, *stream_name = NULL;
    uint64_t offset = 0u, requested = app->session.tool_output_bytes;
    uint32_t applied;
    unsigned int stream;
    size_t retain, raw_limit, available, use;
    const unsigned char *bytes;
    uint64_t total;
    bool cached = false;
    struct snag_buf direct = {0}, encoded = {.max = SNAG_CONFIG_TOKEN_LIMIT_MAX};
    char header[256];

    *result = NULL;
    if (!snag_json_arg_keys(call->arguments, "handle stream", "offset max_output_bytes", error, error_size) ||
        !snag_json_arg_text(call->arguments, "handle", SNAG_ID_HEX_LEN, SNAG_ID_HEX_LEN,
                            false, &handle, error, error_size) ||
        !snag_json_arg_text(call->arguments, "stream", 6u, 6u, false,
                            &stream_name, error, error_size) ||
        !snag_json_arg_uint(call->arguments, "offset", 0u, 0u, UINT64_MAX,
                            &offset, error, error_size) ||
        !snag_json_arg_uint(call->arguments, "max_output_bytes", app->session.tool_output_bytes,
                            512u, SNAG_CONFIG_TOKEN_LIMIT_MAX, &requested, error, error_size))
        goto invalid;
    if (!snag_hex_is_lower(handle, SNAG_ID_HEX_LEN) ||
        (strcmp(stream_name, "stdout") && strcmp(stream_name, "stderr"))) {
        (void)snag_errorf(error, error_size,
            "handle must be 32 lowercase hex characters and stream must be stdout or stderr");
        goto invalid;
    }
    stream = strcmp(stream_name, "stderr") == 0;
    applied = requested > app->session.tool_output_bytes ?
        app->session.tool_output_bytes : (uint32_t)requested;
    raw_limit = applied > sizeof(header) ? (applied - sizeof(header)) * 3u / 4u : 1u;
    retain = app->session.output_cache_bytes ? app->session.output_cache_bytes : raw_limit;

    uint64_t cache_end = app->output_cache.data.len > UINT64_MAX - app->output_cache.offset ?
        UINT64_MAX : app->output_cache.offset + app->output_cache.data.len;
    bool cache_contains = app->output_cache.valid &&
        app->output_cache.stream == stream && !strcmp(app->output_cache.handle, handle) &&
        offset >= app->output_cache.offset &&
        (offset < cache_end || (offset == cache_end && offset == app->output_cache.total));
    if (!snag_session_process(&app->session, handle) && cache_contains) {
        size_t at = (size_t)(offset - app->output_cache.offset);
        bytes = app->output_cache.data.data ? app->output_cache.data.data + at :
            (const unsigned char *)"";
        available = app->output_cache.data.len - at;
        total = app->output_cache.total;
        cached = true;
    } else if (app->session.output_cache_bytes) {
        snag_buf_free(&app->output_cache.data);
        snag_buf_init(&app->output_cache.data, retain);
        if (load_output_window(app, handle, stream, offset, retain,
                               &app->output_cache.data, &total) < 0) goto unavailable;
        memcpy(app->output_cache.handle, handle, SNAG_ID_HEX_LEN + 1u);
        app->output_cache.stream = stream;
        app->output_cache.offset = offset;
        app->output_cache.total = total;
        app->output_cache.valid = true;
        bytes = app->output_cache.data.data ? app->output_cache.data.data :
            (const unsigned char *)"";
        available = app->output_cache.data.len;
    } else {
        snag_buf_init(&direct, retain);
        if (load_output_window(app, handle, stream, offset, retain, &direct, &total) < 0)
            goto unavailable;
        bytes = direct.data ? direct.data : (const unsigned char *)"";
        available = direct.len;
    }
    use = available < raw_limit ? available : raw_limit;
    while (use && use > raw_limit - 4u && !snag_utf8_valid(bytes, use, true)) --use;
    bool utf8 = snag_utf8_valid(bytes, use, true);
    if (!utf8 && snag_base64_append(&encoded, bytes, use) < 0) goto fail;
    const char *payload = utf8 ? (const char *)bytes : (const char *)encoded.data;
    size_t payload_len = utf8 ? use : encoded.len;
    uint64_t next = offset + use;
    int n = snprintf(header, sizeof(header),
        "[tool output handle=%s stream=%s offset=%llu next_offset=%llu total_bytes=%llu eof=%s encoding=%s cache=%s]\n",
        handle, stream_name, (unsigned long long)offset, (unsigned long long)next,
        (unsigned long long)total, next >= total ? "true" : "false", utf8 ? "utf8" : "base64",
        cached ? "hit" : app->session.output_cache_bytes ? "fill" : "disabled");
    if (n < 0 || (size_t)n >= sizeof(header)) goto fail;
    struct snag_buf text = {.max = (size_t)applied + 1u};
    int rc = snag_buf_append(&text, header, (size_t)n) < 0 ||
             snag_buf_append(&text, payload, payload_len) < 0 || snag_buf_terminate(&text) < 0 ? -1 : 0;
    if (rc == 0) *result = snag_tool_result_terminal(true, (const char *)text.data);
    if (rc == 0 && *result)
        rc = snag_json_set_new(*result, "max_output_tokens", json_integer(applied));
    snag_buf_free(&text);
    snag_buf_free(&direct);
    snag_buf_free(&encoded);
    if (rc < 0 || !*result) return -1;
    return 0;

unavailable:
    (void)snag_errorf(error, error_size,
        "No durable %s output exists for handle %s at offset %llu in this session.",
        stream_name ? stream_name : "tool", handle ? handle : "(invalid)",
        (unsigned long long)offset);
invalid:
    *result = snag_tool_result_terminal(false, error[0] ? error : "Invalid read_tool_output arguments.");
    snag_buf_free(&direct);
    snag_buf_free(&encoded);
    return *result ? 0 : -1;
fail:
    snag_buf_free(&direct);
    snag_buf_free(&encoded);
    return -1;
}

/* Owner loss invalidates the OS handle, not the bytes already in the journal. */
int
snag_app_recovered_output(struct app_state *app, const char *handle, json_t *result)
{
    struct snag_process_state *process = snag_session_process(&app->session, handle);
    if (!process || json_object_get(result, "output_ref") ||
        strcmp(snag_json_string(result, "status"), "outcome_unknown")) return 0;
    struct snag_buf message = {.max = 32768u};
    int rc = -1;
    if (snag_buf_printf(&message, "Tool outcome is unknown: owner_lost. The old handle is invalid. "
            "Do not repeat ambiguous effects without inspecting current state. "
            "Captured output remains in %s/events.jsonl.\n", app->session.dir_path) < 0) goto out;
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
        if (erc == 0) erc = snag_json_set_new(result, names[stream], json_pack("{s:I,s:s,s:I,s:s%,s:I}",
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
            "log_start", (json_int_t)process->log_offset, "log_end", (json_int_t)app->session.log_end)) < 0)
        goto out;
    rc = 0;
out: snag_buf_free(&message);
    return rc;
}
