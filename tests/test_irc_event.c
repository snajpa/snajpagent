/* SPDX-License-Identifier: GPL-2.0-only */
#include "irc.h"
#include "json.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void
conversation_routes(void)
{
    struct snag_irc_conversation_target target = {
        .connection = "22222222222222222222222222222222",
        .conversation = "33333333333333333333333333333333",
        .generation = 42u, .identity = SNAG_IRC_OPERATOR, .kind = SNAG_IRC_QUERY,
        .peer = "peer", .endpoint = "localhost:6667", .room = "#work",
        .membership = "44444444444444444444444444444444", .casemapping = SNAG_IRC_RFC1459};
    json_t *expected = json_pack("{s:s,s:s,s:i,s:s,s:s}",
        "connection", "22222222222222222222222222222222",
        "conversation", "33333333333333333333333333333333", "generation", 42,
        "identity", "operator", "peer", "peer");
    assert(expected);
    for (unsigned int variant = 0u; variant < 3u; ++variant) {
        if (variant == 1u) {
            target.kind = SNAG_IRC_CONNECTION_EVENTS;
            target.identity = SNAG_IRC_AGENT;
            assert(!json_object_del(expected, "peer"));
            assert(!json_object_set_new(expected, "identity", json_string("agent")));
            assert(!json_object_set_new(expected, "endpoint", json_string("localhost:6667")));
        } else if (variant == 2u) {
            target.kind = SNAG_IRC_CHANNEL;
            assert(!json_object_set_new(expected, "room", json_string("#work")));
            assert(!json_object_set_new(expected, "membership",
                json_string("44444444444444444444444444444444")));
            assert(!json_object_set_new(expected, "casemapping", json_integer(SNAG_IRC_RFC1459)));
        }
        json_t *route = snag_irc_conversation_route(&target);
        assert(route && json_equal(route, expected));
        json_decref(route);
    }
    json_decref(expected);
}

static void
nickname_mappings(void)
{
    assert(snag_irc_name_equal(SNAG_IRC_ASCII, "Peer", "peer"));
    assert(!snag_irc_name_equal(SNAG_IRC_ASCII, "peer[", "peer{"));
    assert(snag_irc_name_equal(SNAG_IRC_RFC1459, "Peer[\\]^", "peer{|}~"));
    assert(snag_irc_name_equal(SNAG_IRC_RFC1459_STRICT, "Peer[\\]", "peer{|}"));
    assert(!snag_irc_name_equal(SNAG_IRC_RFC1459_STRICT, "Peer^", "peer~"));
    assert(!snag_irc_name_equal(SNAG_IRC_CASE_UNKNOWN, "Peer", "peer"));
    assert(snag_irc_name_equal(SNAG_IRC_CASE_UNKNOWN, "Peer", "Peer"));
}

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
channel_membership(void)
{
    struct snag_irc_event event = query();
    event.kind = SNAG_IRC_CONNECTED;
    event.local = true;
    event.route.kind = SNAG_IRC_CHANNEL;
    event.route.peer[0] = '\0';
    strcpy(event.room, "#room");
    strcpy(event.route.target, event.room);
    strcpy(event.route.membership, "44444444444444444444444444444444");
    event.route.joined = event.route.rejoin = true;
    roundtrip(&event);
    assert(!snag_irc_event_model_visible(&event));
    json_t *data = snag_irc_event_data(&event);
    json_t *directory = snag_irc_conversations_update(NULL, data, 1u);
    assert(directory && snag_irc_conversations_valid(directory, 2u));
    const char *fields[] = {"membership", "joined", "rejoin"};
    struct snag_irc_event decoded;
    for (size_t i = 0u; i < 3u; ++i) {
        json_t *bad = json_deep_copy(data);
        assert(bad && json_object_del(json_object_get(bad, "routing"), fields[i]) == 0);
        assert(snag_irc_event_record_read("irc_event_v2", bad, &decoded) < 0);
        json_decref(bad);
    }
    json_decref(data);
    event.input = true;
    data = snag_irc_event_data(&event);
    assert(snag_irc_event_record_read("irc_event_v2", data, &decoded) < 0);
    json_decref(data);
    event.input = false;
    event.kind = SNAG_IRC_DISCONNECTED;
    event.route.joined = event.route.rejoin = false;
    strcpy(event.route.membership, "55555555555555555555555555555555");
    data = snag_irc_event_data(&event);
    json_t *parted = snag_irc_conversations_update(directory, data, 2u);
    assert(parted && snag_irc_conversations_valid(parted, 3u));
    json_decref(parted);
    json_decref(data);
    json_decref(directory);
    roundtrip(&event);
    event.kind = SNAG_IRC_MESSAGE;
    assert(snag_irc_event_model_visible(&event));
    roundtrip(&event);
}

static void
channel_reply_capture(void)
{
    struct snag_irc_event member = query();
    member.kind = SNAG_IRC_CONNECTED;
    member.local = true;
    member.route.kind = SNAG_IRC_CHANNEL;
    member.route.identity = SNAG_IRC_AGENT;
    member.route.peer[0] = '\0';
    strcpy(member.room, "#Room[");
    strcpy(member.route.target, member.room);
    strcpy(member.route.membership, "44444444444444444444444444444444");
    member.route.joined = member.route.rejoin = true;
    json_t *data = snag_irc_event_data(&member);
    json_t *directory = snag_irc_conversations_update(NULL, data, 1u);
    json_decref(data);
    assert(directory);
    struct snag_irc_event event = member;
    event.kind = SNAG_IRC_MESSAGE;
    event.route.identity = SNAG_IRC_OPERATOR;
    strcpy(event.route.conversation, "55555555555555555555555555555555");
    strcpy(event.route.membership, "66666666666666666666666666666666");
    strcpy(event.room, "#room{");
    strcpy(event.route.target, event.room);
    event.input = event.urgent = event.reply = true;
    assert(snag_irc_event_capture_reply(&event, directory, SNAG_IRC_RFC1459) == 0);
    assert(event.reply_captured &&
        !strcmp(event.reply_conversation, member.route.conversation) &&
        !strcmp(event.reply_membership, member.route.membership));
    roundtrip(&event);
    data = snag_irc_event_data(&event);
    struct snag_irc_event decoded;
    const char *invalid[] = {"false", "{}", "{\"conversation_id\":\"invalid\"}",
        "{\"conversation_id\":\"33333333333333333333333333333333\",\"membership\":\"bad\"}",
        "{\"conversation_id\":\"33333333333333333333333333333333\",\"membership\":\"44444444444444444444444444444444\",\"extra\":true}"};
    for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        json_t *bad = json_deep_copy(data);
        assert(bad && json_object_set_new(bad, "reply_to",
            json_loads(invalid[i], JSON_DECODE_ANY, NULL)) == 0);
        assert(snag_irc_event_record_read("irc_event_v2", bad, &decoded) < 0);
        json_decref(bad);
    }
    assert(json_object_set_new(data, "reply", json_false()) == 0);
    assert(snag_irc_event_record_read("irc_event_v2", data, &decoded) < 0);
    json_decref(data);
    assert(snag_irc_event_capture_reply(&event, directory, SNAG_IRC_ASCII) == 0);
    assert(event.reply_captured && !event.reply_conversation[0] && !event.reply_membership[0]);
    roundtrip(&event);
    ++event.route.generation;
    assert(snag_irc_event_capture_reply(&event, directory, SNAG_IRC_RFC1459) == 0);
    assert(event.reply_captured && !event.reply_conversation[0]);
    roundtrip(&event);
    --event.route.generation;
    member.kind = SNAG_IRC_DISCONNECTED;
    member.route.joined = false;
    data = snag_irc_event_data(&member);
    json_t *parted = snag_irc_conversations_update(directory, data, 2u);
    assert(parted);
    assert(snag_irc_event_capture_reply(&event, parted, SNAG_IRC_RFC1459) == 0);
    assert(event.reply_captured && !event.reply_conversation[0]);
    roundtrip(&event);
    json_decref(data);
    json_decref(parted);
    json_decref(directory);
}

static void
channel_send_input(void)
{
    struct snag_irc_event event = query();
    event.local = event.input = event.urgent = event.reply = true;
    event.route.kind = SNAG_IRC_CHANNEL;
    event.route.peer[0] = '\0';
    event.route.direction = SNAG_IRC_OUTGOING;
    event.route.delivery = SNAG_IRC_ACKNOWLEDGED;
    strcpy(event.route.send, "44444444444444444444444444444444");
    strcpy(event.room, "#room");
    strcpy(event.route.target, event.room);
    roundtrip(&event);
    event.route.delivery = SNAG_IRC_WRITTEN;
    roundtrip(&event);
    struct snag_irc_event decoded;
    for (unsigned int state = SNAG_IRC_PENDING; state <= SNAG_IRC_UNCERTAIN; ++state) {
        if (state == SNAG_IRC_WRITTEN || state == SNAG_IRC_ACKNOWLEDGED) continue;
        event.route.delivery = (enum snag_irc_delivery)state;
        json_t *data = snag_irc_event_data(&event);
        assert(data && snag_irc_event_record_read("irc_event_v2", data, &decoded) < 0);
        json_decref(data);
    }
    event.route.delivery = SNAG_IRC_ACKNOWLEDGED;
    event.route.identity = SNAG_IRC_AGENT;
    json_t *data = snag_irc_event_data(&event);
    assert(data && snag_irc_event_record_read("irc_event_v2", data, &decoded) < 0);
    json_decref(data);
    event.route.identity = SNAG_IRC_OPERATOR;
    event.local = false;
    data = snag_irc_event_data(&event);
    assert(data && snag_irc_event_record_read("irc_event_v2", data, &decoded) < 0);
    json_decref(data);
    event.local = true;
    event.input = false;
    data = snag_irc_event_data(&event);
    assert(data && snag_irc_event_record_read("irc_event_v2", data, &decoded) < 0);
    json_decref(data);
    event.input = true;
    event.urgent = event.reply = false;
    event.kind = SNAG_IRC_NOTICE;
    roundtrip(&event);
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
    event.route.revised = true;
    for (unsigned int state = SNAG_IRC_ACKNOWLEDGED; state <= SNAG_IRC_UNCERTAIN; ++state) {
        event.route.delivery = (enum snag_irc_delivery)state;
        roundtrip(&event);
    }
    struct snag_irc_event decoded;
    for (unsigned int state = SNAG_IRC_PENDING; state <= SNAG_IRC_WRITTEN; ++state) {
        event.route.delivery = (enum snag_irc_delivery)state;
        json_t *data = snag_irc_event_data(&event);
        assert(data && snag_irc_event_record_read("irc_event_v2", data, &decoded) < 0);
        json_decref(data);
    }
    event.route.delivery = SNAG_IRC_ACKNOWLEDGED;
    json_t *data = snag_irc_event_data(&event);
    assert(json_object_set_new(json_object_get(data, "routing"), "revised", json_false()) == 0);
    assert(snag_irc_event_record_read("irc_event_v2", data, &decoded) < 0);
    json_decref(data);
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

static void
directory_update_test(void)
{
    struct snag_irc_event event = query();
    json_t *data = snag_irc_event_data(&event);
    json_t *first = snag_irc_conversations_update(NULL, data, 2u);
    json_decref(data);
    assert(first && snag_irc_conversations_valid(first, 3u));
    assert(!snag_irc_conversations_valid(first, 2u));
    json_t *saved = json_deep_copy(first);
    strcpy(event.route.peer, "renamed");
    data = snag_irc_event_data(&event);
    assert(!snag_irc_conversations_update(first, data, 3u));
    json_decref(data);
    event.kind = SNAG_IRC_NICK;
    strcpy(event.text, "renamed");
    data = snag_irc_event_data(&event);
    json_t *renamed = snag_irc_conversations_update(first, data, 3u);
    assert(renamed && json_equal(first, saved));
    assert(!snag_irc_conversations_update(first, data, 2u));
    json_decref(data);

    event = query();
    event.route.identity = SNAG_IRC_AGENT;
    data = snag_irc_event_data(&event);
    assert(!snag_irc_conversations_update(first, data, 3u));
    json_decref(data);
    event = query();
    strcpy(event.endpoint, "another:6667");
    data = snag_irc_event_data(&event);
    assert(!snag_irc_conversations_update(first, data, 3u));
    json_decref(data);
    strcpy(event.route.connection, "55555555555555555555555555555555");
    data = snag_irc_event_data(&event);
    assert(!snag_irc_conversations_update(first, data, 3u));
    json_decref(data);

    event = query();
    event.kind = SNAG_IRC_CONNECTED;
    event.route.kind = SNAG_IRC_CONNECTION_EVENTS;
    event.route.peer[0] = event.route.target[0] = 0;
    strcpy(event.route.conversation, "66666666666666666666666666666666");
    data = snag_irc_event_data(&event);
    json_t *connected = snag_irc_conversations_update(first, data, 4u);
    assert(connected && snag_irc_conversations_valid(connected, 5u));
    const json_t *status = json_object_get(json_object_get(connected, event.route.connection),
        "connected");
    assert(json_is_true(json_object_get(status, "operator")) &&
        json_is_null(json_object_get(status, "agent")));
    json_decref(data);
    event.route.identity = SNAG_IRC_AGENT;
    strcpy(event.route.conversation, "77777777777777777777777777777777");
    data = snag_irc_event_data(&event);
    json_t *both = snag_irc_conversations_update(connected, data, 5u);
    json_decref(data);
    event.kind = SNAG_IRC_DISCONNECTED;
    data = snag_irc_event_data(&event);
    json_t *disconnected = snag_irc_conversations_update(both, data, 6u);
    json_decref(data);
    assert(disconnected && snag_irc_conversations_valid(disconnected, 7u));
    status = json_object_get(json_object_get(disconnected, event.route.connection), "connected");
    assert(json_is_true(json_object_get(status, "operator")) &&
        json_is_false(json_object_get(status, "agent")));
    event.kind = SNAG_IRC_CONNECTED;
    event.route.generation = 2u;
    data = snag_irc_event_data(&event);
    json_t *reconnected = snag_irc_conversations_update(disconnected, data, 7u);
    json_decref(data);
    assert(reconnected && snag_irc_conversations_valid(reconnected, 8u));
    event.route.generation = 1u;
    data = snag_irc_event_data(&event);
    assert(!snag_irc_conversations_update(reconnected, data, 8u));
    json_decref(data);
    assert(json_equal(first, saved));
    json_decref(reconnected);
    json_decref(disconnected);
    json_decref(both);
    json_decref(connected);
    json_decref(renamed);
    json_decref(saved);
    json_decref(first);
}

int
main(void)
{
    conversation_routes();
    nickname_mappings();
    privacy_and_provenance();
    channel_membership();
    channel_reply_capture();
    channel_send_input();
    delivery_states();
    invalid_fields();
    directory_update_test();
    puts("test_irc_event: ok");
    return 0;
}
