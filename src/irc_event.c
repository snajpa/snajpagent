/* SPDX-License-Identifier: GPL-2.0-only */
#include "irc.h"
#include "json.h"

#include <errno.h>
#include <string.h>

const char *
snag_irc_kind_name(enum snag_irc_event_kind kind)
{
    static const char *const names[] = {
        "connected", "disconnected", "join", "part", "quit", "nick",
        "message", "notice", "topic", "mode", "history_ready" };

    return (unsigned int)kind < sizeof(names) / sizeof(names[0]) ? names[kind] : "unknown";
}

static const char *const identities[] = {"operator", "agent"};
static const char *const conversations[] = {"connection", "channel", "query"};
static const char *const directions[] = {"incoming", "outgoing"};
static const char *const deliveries[] = {"none", "pending", "written", "acknowledged",
    "failed", "uncertain"};

static json_t *
route_data(const struct snag_irc_event_route *route)
{
    if ((unsigned int)route->identity > SNAG_IRC_AGENT ||
        (unsigned int)route->kind > SNAG_IRC_QUERY ||
        (unsigned int)route->direction > SNAG_IRC_OUTGOING ||
        (unsigned int)route->delivery > SNAG_IRC_UNCERTAIN || route->generation > INT64_MAX) {
        errno = EINVAL;
        return NULL;
    }
    return json_pack("{s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:I,s:b}",
        "connection_id", route->connection, "conversation_id", route->conversation,
        "identity", identities[route->identity], "conversation_kind", conversations[route->kind],
        "peer", route->peer, "target", route->target, "direction", directions[route->direction],
        "send_id", route->send, "state", deliveries[route->delivery],
        "source_message_id", route->source, "generation", (json_int_t)route->generation,
        "action", route->action);
}

json_t *
snag_irc_event_data(const struct snag_irc_event *event)
{
    json_t *data = json_pack("{s:s,s:b,s:s,s:b,s:s,s:b,s:s,s:s,s:I,s:s,s:I,s:b}",
        "endpoint", event->endpoint, "historical", event->historical,
        "kind", snag_irc_kind_name(event->kind), "local", event->local,
        "nick", event->nick, "op", event->op, "room", event->room,
        "text", event->text, "timestamp_ms", (json_int_t)event->timestamp_ms,
        "stream", event->stream, "sequence", (json_int_t)event->sequence, "input", event->input);
    if (event->classified && (snag_json_set_new(data, "urgent", json_boolean(event->urgent)) < 0 ||
                             snag_json_set_new(data, "reply", json_boolean(event->reply)) < 0)) {
        json_decref(data); return NULL;
    }
    if (event->routed && snag_json_set_new(data, "routing", route_data(&event->route)) < 0) {
        json_decref(data);
        return NULL;
    }
    return data;
}

static bool
event_field(const json_t *data, const char *key, char *out, size_t size)
{
    const char *value = snag_json_string(data, key);
    size_t length = json_string_length(json_object_get(data, key));

    if (!value || strlen(value) != length || length >= size ||
        !snag_utf8_valid((const unsigned char *)value, length, true)) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (*p < 0x20u || *p == 0x7fu) return false;
    return snag_strcpy(out, size, value);
}

static int
named_value(const json_t *data, const char *key, const char *const *names, size_t count)
{
    const char *value = snag_json_string(data, key);
    if (!value || strlen(value) != json_string_length(json_object_get(data, key))) return -1;
    for (size_t i = 0u; i < count; ++i)
        if (!strcmp(value, names[i])) return (int)i;
    return -1;
}

bool
snag_irc_event_model_visible(const struct snag_irc_event *event)
{
    return !event->routed || event->route.kind == SNAG_IRC_CHANNEL ||
        (event->route.kind == SNAG_IRC_QUERY && event->route.identity == SNAG_IRC_AGENT);
}

static int
route_read(const json_t *data, struct snag_irc_event *event)
{
    struct snag_irc_event_route *route = &event->route;
    int identity = named_value(data, "identity", identities, 2u);
    int kind = named_value(data, "conversation_kind", conversations, 3u);
    int direction = named_value(data, "direction", directions, 2u);
    int delivery = named_value(data, "state", deliveries, 6u);
    if (!snag_json_exact_keys(data, "connection_id conversation_id generation identity "
        "conversation_kind peer target direction send_id state source_message_id action") ||
        identity < 0 || kind < 0 || direction < 0 || delivery < 0 ||
        !event_field(data, "connection_id", route->connection, sizeof(route->connection)) ||
        !snag_hex_is_lower(route->connection, SNAG_ID_HEX_LEN) ||
        !event_field(data, "conversation_id", route->conversation, sizeof(route->conversation)) ||
        !snag_hex_is_lower(route->conversation, SNAG_ID_HEX_LEN) ||
        snag_json_integer_u64(data, "generation", &route->generation) < 0 || !route->generation ||
        !event_field(data, "peer", route->peer, sizeof(route->peer)) ||
        !event_field(data, "target", route->target, sizeof(route->target)) ||
        !event_field(data, "source_message_id", route->source, sizeof(route->source)) ||
        !event_field(data, "send_id", route->send, sizeof(route->send)) ||
        !json_is_boolean(json_object_get(data, "action"))) return -1;
    route->identity = (enum snag_irc_identity)identity;
    route->kind = (enum snag_irc_conversation_kind)kind;
    route->direction = (enum snag_irc_direction)direction;
    route->delivery = (enum snag_irc_delivery)delivery;
    route->action = json_is_true(json_object_get(data, "action"));
    bool chat = event->kind == SNAG_IRC_MESSAGE || event->kind == SNAG_IRC_NOTICE;
    if ((route->kind == SNAG_IRC_QUERY && (!route->peer[0] || event->room[0])) ||
        (route->kind != SNAG_IRC_QUERY && route->peer[0]) ||
        (route->kind == SNAG_IRC_CHANNEL && (!event->room[0] ||
            strcmp(route->target, event->room))) ||
        (route->kind == SNAG_IRC_CONNECTION_EVENTS && (event->room[0] || chat)) ||
        (chat && !route->target[0]) || (route->action && event->kind != SNAG_IRC_MESSAGE) ||
        (route->direction == SNAG_IRC_INCOMING &&
            (route->delivery != SNAG_IRC_DELIVERY_NONE || route->send[0])) ||
        (route->direction == SNAG_IRC_OUTGOING &&
            (route->delivery == SNAG_IRC_DELIVERY_NONE ||
                !snag_hex_is_lower(route->send, SNAG_ID_HEX_LEN) || event->input)) ||
        (!snag_irc_event_model_visible(event) && (event->input || event->urgent || event->reply)) ||
        ((event->historical || event->kind != SNAG_IRC_MESSAGE ||
            route->direction != SNAG_IRC_INCOMING) && (event->urgent || event->reply)) ||
        (event->reply && !event->urgent)) return -1;
    return 0;
}

static int
event_read(const json_t *data, struct snag_irc_event *event, bool routed)
{
    const char *kind = snag_json_string(data, "kind");
    json_t *filled = NULL;

    memset(event, 0, sizeof(*event));
    event->routed = routed;
    /* Workspace-era journals recorded the room event without the stream
     * watermark, its sequence and the input flag; fill those defaults before
     * the current field validation runs. */
    if (!routed && !json_object_get(data, "stream") && !json_object_get(data, "sequence") &&
        !json_object_get(data, "input")) {
        filled = json_deep_copy(data);
        if (!filled ||
            json_object_set_new(filled, "stream", json_string("")) < 0 ||
            json_object_set_new(filled, "sequence", json_integer(0)) < 0 ||
            json_object_set_new(filled, "input", json_false()) < 0) {
            json_decref(filled);
            return snag_errno(ENOMEM);
        }
        data = filled;
    }
    event->classified = json_object_get(data, "urgent") != NULL;
    event->urgent = json_is_true(json_object_get(data, "urgent"));
    event->reply = json_is_true(json_object_get(data, "reply"));
    const char *keys = routed ? event->classified ?
        "endpoint historical kind local nick op room text timestamp_ms stream sequence "
            "input urgent reply routing" :
        "endpoint historical kind local nick op room text timestamp_ms stream sequence "
            "input routing" :
        event->classified ?
        "endpoint historical kind local nick op room text timestamp_ms stream sequence "
            "input urgent reply" :
        "endpoint historical kind local nick op room text timestamp_ms stream sequence input";
    if (!snag_json_exact_keys(data, keys) || !kind ||
        strlen(kind) != json_string_length(json_object_get(data, "kind")) ||
        (event->classified && (!json_is_boolean(json_object_get(data, "urgent")) ||
                               !json_is_boolean(json_object_get(data, "reply")))) ||
        !event_field(data, "endpoint", event->endpoint, sizeof(event->endpoint)) || !event->endpoint[0] ||
        !event_field(data, "room", event->room, sizeof(event->room)) ||
        !event_field(data, "nick", event->nick, sizeof(event->nick)) ||
        !event_field(data, "text", event->text, sizeof(event->text)) ||
        !json_is_boolean(json_object_get(data, "historical")) ||
        !json_is_boolean(json_object_get(data, "local")) || !json_is_boolean(json_object_get(data, "op")) ||
        snag_json_integer_u64(data, "timestamp_ms", &event->timestamp_ms) < 0 || !event->timestamp_ms ||
        !event_field(data, "stream", event->stream, sizeof(event->stream)) ||
        (event->stream[0] && !snag_hex_is_lower(event->stream, SNAG_ID_HEX_LEN)) ||
        snag_json_integer_u64(data, "sequence", &event->sequence) < 0 ||
        (!!event->sequence != !!event->stream[0]) || !json_is_boolean(json_object_get(data, "input")))
        goto invalid;
    for (unsigned int i = 0u; i <= (unsigned int)SNAG_IRC_HISTORY_READY; ++i) {
        if (strcmp(kind, snag_irc_kind_name((enum snag_irc_event_kind)i)) != 0) continue;
        event->kind = (enum snag_irc_event_kind)i;
        event->historical = json_is_true(json_object_get(data, "historical"));
        event->input = json_is_true(json_object_get(data, "input"));
        event->local = json_is_true(json_object_get(data, "local"));
        event->op = json_is_true(json_object_get(data, "op"));
        if (routed && route_read(json_object_get(data, "routing"), event) < 0) goto invalid;
        json_decref(filled);
        return 0;
    }
invalid:
    json_decref(filled);
    return snag_errno(EINVAL);
}

int
snag_irc_event_read(const json_t *data, struct snag_irc_event *event)
{
    return event_read(data, event, false);
}

const char *
snag_irc_event_record_type(const struct snag_irc_event *event)
{
    return event->routed ? "irc_event_v2" : "irc_event";
}

int
snag_irc_event_record_read(const char *type, const json_t *data, struct snag_irc_event *event)
{
    if (!type) return snag_errno(EINVAL);
    if (!strcmp(type, "irc_event")) return event_read(data, event, false);
    if (!strcmp(type, "irc_event_v2")) return event_read(data, event, true);
    return snag_errno(EINVAL);
}

int
snag_irc_event_projection(struct snag_buf *out, const struct snag_irc_event *event)
{
    if (!snag_irc_event_model_visible(event)) return 0;
    if (event->routed) {
        const struct snag_irc_event_route *route = &event->route;
        if ((unsigned int)route->identity > SNAG_IRC_AGENT ||
            (unsigned int)route->kind > SNAG_IRC_QUERY ||
            (unsigned int)route->direction > SNAG_IRC_OUTGOING) return snag_errno(EINVAL);
        return snag_buf_printf(out, "[IRC endpoint=%s connection=%s generation=%llu "
            "identity=%s conversation=%s kind=%s peer=%s target=%s direction=%s "
            "event=%s sender=%s action=%s historical=%s id=%s:%llu]\n%s\n",
            event->endpoint, route->connection, (unsigned long long)route->generation,
            identities[route->identity], route->conversation, conversations[route->kind],
            route->peer, route->target, directions[route->direction],
            snag_irc_kind_name(event->kind), event->nick[0] ? event->nick : "server",
            route->action ? "true" : "false", event->historical ? "true" : "false",
            event->stream, (unsigned long long)event->sequence, event->text);
    }
    return snag_buf_printf(out,
        "[IRC endpoint=%s room=%s event=%s sender=%s operator=%s historical=%s id=%s:%llu]\n%s\n",
        event->endpoint, event->room, snag_irc_kind_name(event->kind),
        event->nick[0] ? event->nick : "server", event->op ? "true" : "false",
        event->historical ? "true" : "false", event->stream,
        (unsigned long long)event->sequence, event->text);
}
