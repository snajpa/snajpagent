/* SPDX-License-Identifier: GPL-2.0-only */
#include "irc.h"
#include "json.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct snag_irc_event
query(void)
{
    struct snag_irc_event event = {.routed = true, .kind = SNAG_IRC_MESSAGE,
        .timestamp_ms = 1000u, .classified = true, .sequence = 5u,
        .endpoint = "localhost:6667", .nick = "peer", .text = "private é界 payload",
        .stream = "11111111111111111111111111111111", .route = {
            .connection = "22222222222222222222222222222222",
            .conversation = "33333333333333333333333333333333",
            .generation = 1u, .identity = SNAG_IRC_OPERATOR, .kind = SNAG_IRC_QUERY,
            .direction = SNAG_IRC_INCOMING, .peer = "peer", .target = "my-operator",
            .source = "server/message-id"}};
    return event;
}

static void
roundtrip(const struct snag_irc_event *event)
{
    json_t *data = snag_irc_event_data(event);
    struct snag_irc_event decoded;
    const char *type = snag_irc_event_record_type(event);
    assert(data && snag_irc_event_record_read(type, data, &decoded) == 0);
    json_t *again = snag_irc_event_data(&decoded);
    assert(again && json_equal(data, again));
    assert(decoded.routed == event->routed && !strcmp(decoded.text, event->text));
    if (event->routed) {
        assert(snag_irc_event_read(data, &decoded) < 0);
        assert(snag_irc_event_record_read("irc_event", data, &decoded) < 0);
    } else assert(snag_irc_event_record_read("irc_event_v2", data, &decoded) < 0);
    json_decref(again);
    json_decref(data);
}

static void
privacy_and_provenance(void)
{
    struct snag_irc_event event = query();
    roundtrip(&event);
    struct snag_buf out = {.max = 32768u};
    assert(snag_buf_append(&out, "unchanged", strlen("unchanged")) == 0);
    assert(!snag_irc_event_model_visible(&event));
    assert(snag_irc_event_projection(&out, &event) == 0 && out.len == strlen("unchanged"));
    snag_buf_free(&out);
    out.max = 32768u;
    event.route.identity = SNAG_IRC_AGENT;
    strcpy(event.route.target, "my-agent");
    event.input = event.urgent = event.reply = true;
    roundtrip(&event);
    assert(snag_irc_event_model_visible(&event));
    assert(snag_irc_event_projection(&out, &event) == 0 && snag_buf_terminate(&out) == 0);
    const char *text = (const char *)out.data;
    assert(strstr(text, "identity=agent") && strstr(text, "kind=query") &&
        strstr(text, "peer=peer target=my-agent") && strstr(text, "direction=incoming") &&
        strstr(text, event.route.connection) && strstr(text, event.route.conversation) &&
        strstr(text, event.text));
    snag_buf_free(&out);

    event.route.kind = SNAG_IRC_CHANNEL;
    event.route.peer[0] = 0;
    strcpy(event.room, "#room");
    strcpy(event.route.target, "#room");
    event.route.identity = SNAG_IRC_OPERATOR;
    roundtrip(&event);
    assert(snag_irc_event_model_visible(&event));

    event.routed = false;
    roundtrip(&event);
    json_t *legacy = snag_irc_event_data(&event);
    assert(legacy && json_object_del(legacy, "stream") == 0 &&
        json_object_del(legacy, "sequence") == 0 && json_object_del(legacy, "input") == 0);
    struct snag_irc_event decoded;
    assert(snag_irc_event_read(legacy, &decoded) == 0 && !decoded.input && !decoded.routed);
    json_decref(legacy);
}

static void
delivery_states(void)
{
    struct snag_irc_event event = query();
    event.route.direction = SNAG_IRC_OUTGOING;
    strcpy(event.route.send, "44444444444444444444444444444444");
    strcpy(event.route.target, "peer");
    strcpy(event.nick, "my-operator");
    for (unsigned int state = SNAG_IRC_PENDING; state <= SNAG_IRC_UNCERTAIN; ++state) {
        event.route.delivery = (enum snag_irc_delivery)state;
        roundtrip(&event);
    }
    event.route.action = true;
    roundtrip(&event);
    event = query();
    event.kind = SNAG_IRC_NOTICE;
    roundtrip(&event);
    event.kind = SNAG_IRC_DISCONNECTED;
    event.route.kind = SNAG_IRC_CONNECTION_EVENTS;
    event.route.peer[0] = event.route.target[0] = 0;
    roundtrip(&event);
    assert(!snag_irc_event_model_visible(&event));
}

static void
invalid_fields(void)
{
    struct snag_irc_event event = query(), decoded;
    json_t *original = snag_irc_event_data(&event);
    assert(original);
    const char *changes[] = {
        "{\"connection_id\":\"bad\"}", "{\"conversation_id\":\"\"}",
        "{\"generation\":0}", "{\"generation\":-1}", "{\"identity\":\"other\"}",
        "{\"conversation_kind\":\"unknown\"}", "{\"conversation_kind\":\"connection\"}",
        "{\"conversation_kind\":\"channel\"}", "{\"peer\":\"\"}", "{\"target\":\"\"}",
        "{\"target\":\"wrong\\nrecipient\"}", "{\"direction\":\"unknown\"}",
        "{\"direction\":\"outgoing\"}", "{\"state\":\"written\"}", "{\"state\":\"other\"}",
        "{\"send_id\":\"44444444444444444444444444444444\"}", "{\"action\":1}",
        "{\"source_message_id\":\"bad\\nsource\"}", "{\"extra\":true}"
    };
    for (size_t i = 0u; i < sizeof(changes) / sizeof(changes[0]); ++i) {
        json_t *data = json_deep_copy(original);
        json_t *change = json_loads(changes[i], 0u, NULL);
        assert(data && change && json_object_update(json_object_get(data, "routing"), change) == 0);
        assert(snag_irc_event_record_read("irc_event_v2", data, &decoded) < 0);
        json_decref(change);
        json_decref(data);
    }
    const char *flags[] = {"input", "urgent", "reply"};
    for (size_t i = 0u; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        json_t *data = json_deep_copy(original);
        assert(data && json_object_set_new(data, flags[i], json_true()) == 0);
        assert(snag_irc_event_record_read("irc_event_v2", data, &decoded) < 0);
        json_decref(data);
    }
    const char embedded[] = "peer\0different";
    json_t *data = json_deep_copy(original);
    assert(data && json_object_set_new(json_object_get(data, "routing"), "peer",
        json_stringn(embedded, sizeof(embedded) - 1u)) == 0);
    assert(snag_irc_event_record_read("irc_event_v2", data, &decoded) < 0);
    json_decref(data);
    const char identity[] = "operator\0other";
    data = json_deep_copy(original);
    assert(data && json_object_set_new(json_object_get(data, "routing"), "identity",
        json_stringn(identity, sizeof(identity) - 1u)) == 0);
    assert(snag_irc_event_record_read("irc_event_v2", data, &decoded) < 0);
    json_decref(data);
    event.routed = false;
    data = snag_irc_event_data(&event);
    const char kind[] = "message\0other";
    assert(data && json_object_set_new(data, "kind", json_stringn(kind, sizeof(kind) - 1u)) == 0);
    assert(snag_irc_event_read(data, &decoded) < 0);
    json_decref(data);
    json_decref(original);
}

int
main(void)
{
    privacy_and_provenance();
    delivery_states();
    invalid_fields();
    puts("test_irc_event: ok");
    return 0;
}
