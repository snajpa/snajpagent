/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_event.h"
#include "store.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

void test_store_binary_event(void);

static void
assert_rejected(struct snag_binary_record record)
{
    struct snag_binary_event sentinel = {.kind = SNAG_BINARY_GOAL_CANCELLED};
    struct snag_binary_event out = sentinel;
    assert(snag_binary_event_decode(&record, &out) < 0);
    assert(!memcmp(&out, &sentinel, sizeof(out)));
}

static void
roundtrip(const struct snag_binary_event *event)
{
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, event));
    struct snag_binary_record record = {
        .kind = (uint16_t)event->kind, .version = 1u,
        .payload = payload.data, .size = payload.len
    };
    struct snag_binary_event decoded;
    assert(!snag_binary_event_decode(&record, &decoded));
    assert(decoded.kind == event->kind);
    if (event->kind >= SNAG_BINARY_TIMER_SCHEDULED &&
        event->kind <= SNAG_BINARY_TIMER_CANCELLED) {
        assert(!memcmp(decoded.data.timer.id, event->data.timer.id, 16u));
        if (event->kind == SNAG_BINARY_TIMER_SCHEDULED) {
            assert(decoded.data.timer.due_ms == event->data.timer.due_ms);
            assert(decoded.data.timer.text.size == event->data.timer.text.size);
            assert(!memcmp(decoded.data.timer.text.data, event->data.timer.text.data,
                event->data.timer.text.size));
        }
    } else if (event->kind >= SNAG_BINARY_GOAL_STARTED &&
        event->kind <= SNAG_BINARY_GOAL_CANCELLED) {
        assert(!memcmp(decoded.data.goal.id, event->data.goal.id, 16u));
        if (event->kind == SNAG_BINARY_GOAL_REPLACED)
            assert(!memcmp(decoded.data.goal.replacement, event->data.goal.replacement, 16u));
        if (event->kind == SNAG_BINARY_GOAL_REPLACED ||
            event->kind == SNAG_BINARY_GOAL_REWORDED ||
            event->kind == SNAG_BINARY_GOAL_COMPLETED ||
            event->kind == SNAG_BINARY_GOAL_BLOCKED)
            assert(decoded.data.goal.actor == event->data.goal.actor);
        if (event->kind == SNAG_BINARY_GOAL_LOCK_CHANGED)
            assert(decoded.data.goal.locked == event->data.goal.locked);
        if (event->kind == SNAG_BINARY_GOAL_PAUSED)
            assert(decoded.data.goal.pause == event->data.goal.pause);
        if (event->kind == SNAG_BINARY_GOAL_STARTED ||
            event->kind == SNAG_BINARY_GOAL_REPLACED ||
            event->kind == SNAG_BINARY_GOAL_REWORDED ||
            event->kind == SNAG_BINARY_GOAL_BLOCKED) {
            assert(decoded.data.goal.text.size == event->data.goal.text.size);
            assert(!memcmp(decoded.data.goal.text.data, event->data.goal.text.data,
                event->data.goal.text.size));
        }
    }
    struct snag_buf encoded_again = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&encoded_again, &decoded));
    assert(encoded_again.len == payload.len);
    assert(!payload.len || !memcmp(encoded_again.data, payload.data, payload.len));
    snag_buf_free(&encoded_again);
    for (size_t i = 0; i < payload.len; ++i) {
        record.size = i;
        assert_rejected(record);
    }
    record.size = payload.len;
    record.version = 2u;
    assert_rejected(record);
    record.version = 1u;
    record.flags = SNAG_BINARY_RECORD_OPTIONAL;
    assert_rejected(record); /* Semantic records cannot claim optional status. */
    record.flags = 0u;
    assert(!snag_buf_putc(&payload, 0));
    record.payload = payload.data;
    record.size = payload.len;
    assert_rejected(record); /* Unknown trailing fields need a new payload version. */
    snag_buf_free(&payload);
}

static struct snag_binary_text
text(const char *value)
{
    return (struct snag_binary_text){(const unsigned char *)value, strlen(value)};
}

static void
test_input_lists(void)
{
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_binary_part parts[3] = {{.kind = SNAG_BINARY_PART_TEXT}};
    parts[0].text = text("caf\xc3\xa9");
    assert(!snag_binary_content_encode(&bytes, parts, 1u));
    static const unsigned char golden[] = "\x01\0\0\0\x01\x05\0\0\0caf\xc3\xa9";
    assert(bytes.len == sizeof(golden) - 1u && !memcmp(bytes.data, golden, bytes.len));
    struct snag_binary_content content = {0}, sentinel = {0};
    assert(!snag_binary_content_decode(bytes.data, bytes.len, &content));
    assert(content.data == bytes.data && content.size == bytes.len);
    for (size_t i = 0; i < bytes.len; ++i) {
        assert(snag_binary_content_decode(bytes.data, i, &sentinel) < 0);
        assert(!sentinel.data && !sentinel.size);
    }
    size_t offset = 0;
    struct snag_binary_part part;
    assert(!snag_binary_content_next(&content, &offset, &part));
    assert(part.kind == SNAG_BINARY_PART_TEXT && part.text.size == 5u);
    assert(!memcmp(part.text.data, parts[0].text.data, part.text.size));
    assert(snag_binary_content_next(&content, &offset, &part) == 1);
    assert(offset == content.size && part.text.size == 5u);
    offset = 1u;
    assert(snag_binary_content_next(&content, &offset, &part) < 0 && offset == 1u);
    assert(part.text.size == 5u);
    bytes.data[4] = 4u;
    assert(snag_binary_content_decode(bytes.data, bytes.len, &sentinel) < 0);
    bytes.data[4] = 1u;
    bytes.data[9] = 0u;
    assert(snag_binary_content_decode(bytes.data, bytes.len, &sentinel) < 0);
    bytes.data[9] = 'c';
    memset(bytes.data, 0xff, 4u);
    assert(snag_binary_content_decode(bytes.data, bytes.len, &sentinel) < 0);
    snag_buf_reset(&bytes);
    parts[1].kind = SNAG_BINARY_PART_FILE;
    parts[1].asset.bytes = 7u;
    parts[1].asset.mime = text("text/plain");
    memset(parts[1].asset.id, 0xa5, 16u);
    memset(parts[1].asset.sha256, 0x5a, 32u);
    parts[2].kind = SNAG_BINARY_PART_IMAGE;
    parts[2].asset = parts[1].asset;
    parts[2].asset.mime = text("image/png");
    parts[2].source = parts[1].asset;
    parts[2].has_source = true;
    parts[2].text = text("derived thumbnail");
    assert(!snag_binary_content_encode(&bytes, parts, 3u));
    assert(!snag_binary_content_decode(bytes.data, bytes.len, &content));
    offset = 0;
    struct snag_binary_part decoded[3];
    for (size_t i = 0; i < 3u; ++i)
        assert(!snag_binary_content_next(&content, &offset, &decoded[i]));
    assert(offset == bytes.len && snag_binary_content_next(&content, &offset, &part) == 1);
    assert(decoded[1].asset.bytes == 7u && !memcmp(decoded[1].asset.id, parts[1].asset.id, 16u));
    assert(!memcmp(decoded[2].source.sha256, parts[2].source.sha256, 32u));
    assert(decoded[2].has_source && decoded[2].text.size == parts[2].text.size);
    struct snag_buf again = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_content_encode(&again, decoded, 3u));
    assert(again.len == bytes.len && !memcmp(again.data, bytes.data, bytes.len));
    for (size_t i = 0; i < bytes.len; ++i)
        assert(snag_binary_content_decode(bytes.data, i, &sentinel) < 0);
    size_t saved = bytes.len;
    parts[1].asset.bytes = 0;
    assert(snag_binary_content_encode(&bytes, parts, 3u) < 0 && bytes.len == saved);
    assert(snag_binary_content_encode(&bytes, parts, SIZE_MAX) < 0 && bytes.len == saved);
    assert(snag_binary_content_encode(&bytes, NULL, 0) < 0 && bytes.len == saved);
    snag_buf_reset(&bytes);
    assert(!snag_binary_content_encode(&bytes, &parts[2], 1u));
    bytes.data[74] = 2u; /* Image source-presence byte after the primary asset. */
    assert(snag_binary_content_decode(bytes.data, bytes.len, &sentinel) < 0);
    bytes.data[74] = 1u;
    memset(bytes.data + 53, 0, 8u); /* Required positive asset byte count. */
    assert(snag_binary_content_decode(bytes.data, bytes.len, &sentinel) < 0);
    snag_buf_reset(&bytes);
    parts[2].has_source = false;
    assert(!snag_binary_content_encode(&bytes, &parts[2], 1u));
    assert(!snag_binary_content_decode(bytes.data, bytes.len, &content));
    offset = 0;
    assert(!snag_binary_content_next(&content, &offset, &part) && !part.has_source);
    assert(!snag_buf_putc(&bytes, 0));
    assert(snag_binary_content_decode(bytes.data, bytes.len, &sentinel) < 0);

    snag_buf_reset(&bytes);
    struct snag_binary_instruction instructions[2] = {{.path = text("/a")}, {.path = text("/b")}};
    assert(!snag_binary_instructions_encode(&bytes, instructions, 1u));
    static const unsigned char instruction_bytes[] = "\x01\0\0\0\0\x02\0\0\0/a";
    assert(bytes.len == sizeof(instruction_bytes) - 1u);
    assert(!memcmp(bytes.data, instruction_bytes, bytes.len));
    struct snag_binary_instructions paths = {0};
    for (size_t i = 0; i < bytes.len; ++i) {
        assert(snag_binary_instructions_decode(bytes.data, i, &paths) < 0);
        assert(!paths.data && !paths.size);
    }
    bytes.data[4] = 2u;
    assert(snag_binary_instructions_decode(bytes.data, bytes.len, &paths) < 0);
    snag_buf_reset(&bytes);
    instructions[1].has_snapshot = true;
    instructions[1].bytes = UINT64_C(0x01020304);
    memset(instructions[1].sha256, 0x3c, 32u);
    assert(!snag_binary_instructions_encode(&bytes, instructions, 2u));
    assert(!snag_binary_instructions_decode(bytes.data, bytes.len, &paths));
    struct snag_binary_instruction decoded_paths[2];
    offset = 0;
    assert(!snag_binary_instructions_next(&paths, &offset, &decoded_paths[0]));
    assert(!snag_binary_instructions_next(&paths, &offset, &decoded_paths[1]));
    assert(!decoded_paths[0].has_snapshot && decoded_paths[1].has_snapshot);
    assert(decoded_paths[1].bytes == UINT64_C(0x01020304));
    assert(!memcmp(decoded_paths[1].sha256, instructions[1].sha256, 32u));
    assert(snag_binary_instructions_next(&paths, &offset, &decoded_paths[0]) == 1);
    snag_buf_reset(&again);
    assert(!snag_binary_instructions_encode(&again, decoded_paths, 2u));
    assert(again.len == bytes.len && !memcmp(again.data, bytes.data, bytes.len));
    saved = bytes.len;
    instructions[0].path = text("");
    assert(snag_binary_instructions_encode(&bytes, instructions, 2u) < 0 && bytes.len == saved);
    assert(snag_binary_instructions_encode(&bytes, instructions, SIZE_MAX) < 0 &&
           bytes.len == saved);
    snag_buf_reset(&bytes);
    assert(!snag_binary_instructions_encode(&bytes, NULL, 0));
    assert(bytes.len == 4u && !memcmp(bytes.data, "\0\0\0\0", 4u));
    assert(!snag_binary_instructions_decode(bytes.data, bytes.len, &paths));
    offset = 0;
    assert(snag_binary_instructions_next(&paths, &offset, &decoded_paths[0]) == 1 && !offset);
    snag_buf_free(&again); snag_buf_free(&bytes);
}

static void
test_input_events(void)
{
    struct snag_buf instructions = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf parts = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_instructions_encode(&instructions, NULL, 0));
    struct snag_binary_part part = {.kind = SNAG_BINARY_PART_TEXT, .text = text("original")};
    assert(!snag_binary_content_encode(&parts, &part, 1u));
    struct snag_binary_event input = {.kind = SNAG_BINARY_INPUT_RECEIVED};
    input.data.input.selection = (struct snag_binary_selection){text("p"), text("m"), text("e")};
    input.data.input.instructions = (struct snag_binary_instructions){
        instructions.data, instructions.len
    };
    input.data.input.text = text("x");
    input.data.input.received_ms = UINT64_C(0x0102030405060708);
    input.data.input.read_only = true;
    input.data.input.origin = SNAG_BINARY_INPUT_TIMER;
    roundtrip(&input);
    assert(!snag_binary_event_encode(&payload, &input));
    static const unsigned char golden[] =
        "\x01\x01\x08\x07\x06\x05\x04\x03\x02\x01"
        "\x01\0\0\0p\x01\0\0\0m\x01\0\0\0e\0\0\0\0\x01\0\0\0x";
    assert(payload.len == sizeof(golden) - 1u && !memcmp(payload.data, golden, payload.len));
    struct snag_binary_record record = {
        .kind = SNAG_BINARY_INPUT_RECEIVED, .version = 1u,
        .payload = payload.data, .size = payload.len
    };
    struct snag_binary_event decoded;
    assert(!snag_binary_event_decode(&record, &decoded));
    assert(decoded.data.input.received_ms == input.data.input.received_ms);
    assert(decoded.data.input.origin == SNAG_BINARY_INPUT_TIMER && decoded.data.input.read_only);
    assert(!decoded.data.input.content.data && decoded.data.input.text.data[0] == 'x');
    payload.data[0] = 2u;
    assert_rejected(record);
    payload.data[0] = 1u;
    payload.data[1] = 4u;
    assert_rejected(record);
    input.data.input.content = (struct snag_binary_content){parts.data, parts.len};
    input.data.input.origin = SNAG_BINARY_INPUT_DEFAULT;
    input.data.input.read_only = false;
    roundtrip(&input);
    struct snag_binary_event event = {.kind = SNAG_BINARY_INPUT_CANCELLED};
    roundtrip(&event);
    event.kind = SNAG_BINARY_STEERING_ADDED;
    memset(event.data.steering_input.id, 0x11, 16u);
    memset(event.data.steering_input.turn, 0x22, 16u);
    event.data.steering_input.text = text("original steering");
    roundtrip(&event);
    event.data.steering_input.has_received_ms = true;
    event.data.steering_input.content = input.data.input.content;
    roundtrip(&event); /* A present zero timestamp must not become absent. */
    event.data.steering_input.received_ms = UINT64_C(0x1020304050607080);
    roundtrip(&event);
    event.kind = SNAG_BINARY_IRC_REPLY_REMINDER;
    roundtrip(&event);
    event = (struct snag_binary_event){.kind = SNAG_BINARY_STEERING_DEFERRED};
    memset(event.data.turn, 0x22, 16u);
    roundtrip(&event);
    event = (struct snag_binary_event){.kind = SNAG_BINARY_INPUT_ADMITTED};
    memset(event.data.admission.turn, 0x22, 16u);
    event.data.admission.time_ms = 1u;
    roundtrip(&event);
    static const unsigned char ids[2][16] = {{1}, {2}};
    event.data.admission.ids = (struct snag_binary_ids){ids, 2u};
    roundtrip(&event);
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    assert(payload.len == 60u && payload.data[16] == 1u && payload.data[24] == 2u);
    assert(payload.data[28] == 1u && payload.data[44] == 2u);
    record = (struct snag_binary_record){
        .kind = SNAG_BINARY_INPUT_ADMITTED, .version = 1u,
        .payload = payload.data, .size = payload.len
    };
    memset(payload.data + 24, 0xff, 4u);
    assert_rejected(record);
    size_t saved = payload.len;
    event.data.admission.time_ms = 0;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event = (struct snag_binary_event){.kind = SNAG_BINARY_FUTURE_QUEUE_STATE};
    roundtrip(&event);
    event.data.queue_armed = true;
    roundtrip(&event);
    event = (struct snag_binary_event){.kind = SNAG_BINARY_FUTURE_TURN_CANCELLED};
    event.data.queue_cancel.actor = SNAG_BINARY_USER;
    event.data.queue_cancel.ids = (struct snag_binary_ids){ids, 2u};
    roundtrip(&event);
    event.data.queue_cancel.actor = SNAG_BINARY_MODEL;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event.data.queue_cancel.actor = SNAG_BINARY_USER;
    event.data.queue_cancel.ids.count = 0;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    char *large = malloc(SNAG_MAX_DIRECT_PROMPT);
    assert(large);
    memset(large, 'x', SNAG_MAX_DIRECT_PROMPT);
    input.data.input.text = (struct snag_binary_text){
        (unsigned char *)large, SNAG_MAX_DIRECT_PROMPT
    };
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &input));
    record = (struct snag_binary_record){
        .kind = SNAG_BINARY_INPUT_RECEIVED, .version = 1u,
        .payload = payload.data, .size = payload.len
    };
    assert(!snag_binary_event_decode(&record, &decoded));
    assert(decoded.data.input.text.size == SNAG_MAX_DIRECT_PROMPT);
    ++input.data.input.text.size;
    saved = payload.len;
    assert(snag_binary_event_encode(&payload, &input) < 0 && payload.len == saved);
    free(large);
    input.data.input.text = text("x");
    snag_buf_reset(&instructions);
    struct snag_binary_instruction legacy = {.path = text("/a"), .has_snapshot = true};
    assert(!snag_binary_instructions_encode(&instructions, &legacy, 1u));
    input.data.input.instructions = (struct snag_binary_instructions){
        instructions.data, instructions.len
    };
    assert(snag_binary_event_encode(&payload, &input) < 0 && payload.len == saved);
    assert(snag_binary_event_encode(&payload, NULL) < 0 && payload.len == saved);
    assert(snag_binary_event_decode(NULL, &decoded) < 0);
    snag_buf_free(&instructions); snag_buf_free(&parts); snag_buf_free(&payload);
}

static void
test_queued_input(void)
{
    struct snag_binary_event event = {.kind = SNAG_BINARY_FUTURE_TURN_QUEUED};
    event.data.queued.id[0] = 10u;
    event.data.queued.text = text("x");
    roundtrip(&event);
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    static const unsigned char golden[23] = {[0] = 10, [18] = 1, [22] = 'x'};
    assert(payload.len == sizeof(golden) && !memcmp(payload.data, golden, sizeof(golden)));
    struct snag_binary_record record = {
        .kind = SNAG_BINARY_FUTURE_TURN_QUEUED, .version = 1u,
        .payload = payload.data, .size = payload.len
    };
    payload.data[16] = 4u; /* Armed value cannot appear without presence. */
    assert_rejected(record);
    payload.data[16] = 64u;
    assert_rejected(record);
    payload.data[16] = 0u;
    payload.data[17] = 2u; /* Null while-turn belongs to voice, not typed input. */
    assert_rejected(record);
    payload.data[17] = 3u;
    assert_rejected(record);
    event.data.queued.while_kind = SNAG_BINARY_WHILE_ID;
    memset(event.data.queued.while_id, 0x11, 16u);
    event.data.queued.read_only = true;
    event.data.queued.has_armed = true;
    event.data.queued.has_received_ms = true;
    roundtrip(&event); /* Explicit disarm and zero receipt time remain present. */
    event.data.queued.armed = true;
    event.data.queued.received_ms = UINT64_C(0x0102030405060708);
    roundtrip(&event);
    struct snag_buf content = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_binary_part part = {.kind = SNAG_BINARY_PART_TEXT, .text = text("queue original")};
    assert(!snag_binary_content_encode(&content, &part, 1u));
    event.data.queued.content = (struct snag_binary_content){content.data, content.len};
    roundtrip(&event);
    event.kind = SNAG_BINARY_FUTURE_TURN_EDITED;
    roundtrip(&event);
    event.data.queued.has_armed = false;
    event.data.queued.has_received_ms = false;
    roundtrip(&event);

    event = (struct snag_binary_event){.kind = SNAG_BINARY_FUTURE_TURN_QUEUED};
    event.data.queued.id[0] = 10u;
    event.data.queued.text = text("x");
    event.data.queued.has_voice = true;
    event.data.queued.while_kind = SNAG_BINARY_WHILE_NULL;
    memset(event.data.queued.voice.connection_id, 0x33, 16u);
    event.data.queued.voice.input_id = text("i");
    event.data.queued.voice.response_id = text("r");
    event.data.queued.voice.call_id = text("c");
    event.data.queued.voice.provider = text("p");
    event.data.queued.voice.model = text("m");
    event.data.queued.voice.transcript = text("t");
    event.data.queued.voice.request = text("q");
    roundtrip(&event);
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    static const unsigned char voice_tail[] =
        "\x01\0\0\0i\x01\0\0\0r\x01\0\0\0c\x01\0\0\0p"
        "\x01\0\0\0m\x01\0\0\0t\x01\0\0\0q";
    assert(payload.len == 74u && payload.data[16] == 32u && payload.data[17] == 2u);
    assert(!memcmp(payload.data + 23, event.data.queued.voice.connection_id, 16u));
    assert(!memcmp(payload.data + 39, voice_tail, sizeof(voice_tail) - 1u));
    record.payload = payload.data;
    record.size = payload.len;
    struct snag_binary_event decoded;
    assert(!snag_binary_event_decode(&record, &decoded));
    assert(decoded.data.queued.has_voice &&
           decoded.data.queued.while_kind == SNAG_BINARY_WHILE_NULL);
    assert(decoded.data.queued.voice.transcript.size == 1u);
    assert(decoded.data.queued.voice.transcript.data[0] == 't');
    assert(decoded.data.queued.voice.request.data[0] == 'q');
    payload.data[16] = 33u;
    assert_rejected(record);
    payload.data[16] = 48u;
    assert_rejected(record);
    payload.data[16] = 32u;
    payload.data[17] = 0u;
    assert_rejected(record);
    payload.data[17] = 2u;
    record.kind = SNAG_BINARY_FUTURE_TURN_EDITED;
    assert_rejected(record);
    size_t saved = payload.len;
    event.kind = SNAG_BINARY_FUTURE_TURN_EDITED;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event.kind = SNAG_BINARY_FUTURE_TURN_QUEUED;
    event.data.queued.read_only = true;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event.data.queued.read_only = false;
    event.data.queued.content = (struct snag_binary_content){content.data, content.len};
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event.data.queued.content = (struct snag_binary_content){0};
    event.data.queued.while_kind = SNAG_BINARY_WHILE_EMPTY;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event.data.queued.while_kind = SNAG_BINARY_WHILE_ID;
    memset(event.data.queued.while_id, 0x44, 16u);
    event.data.queued.has_armed = true;
    event.data.queued.armed = true;
    event.data.queued.has_received_ms = true;
    event.data.queued.received_ms = 123u;
    roundtrip(&event);
    event.data.queued.voice.input_id = text("");
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event.data.queued.voice.input_id = text("i");
    event.data.queued.voice.transcript.size = SNAG_MAX_QUEUED_TEXT;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    snag_buf_free(&payload); snag_buf_free(&content);
}

static void
test_metadata(void)
{
    struct snag_binary_selection before = {text("p"), text("m"), text("e")};
    struct snag_binary_selection after = {text("q"), text("n"), text("f")};
    struct snag_binary_event events[12] = {0};
    for (size_t i = 0; i < 12u; ++i) events[i].kind = (enum snag_binary_kind)(i + 1u);
    events[0].data.created.source_format = 4u;
    events[0].data.created.protocol = SNAG_BINARY_RESPONSES;
    events[0].data.created.selection = before;
    events[0].data.created.cwd = text("/");
    events[1].data.cwd.before = text("/old");
    events[1].data.cwd.after = text("/new");
    events[2].data.archive.origin = SNAG_BINARY_USER;
    events[3].data.archive.origin = SNAG_BINARY_USER;
    for (size_t i = 0; i < 16u; ++i) {
        events[4].data.deletion.session[i] = (unsigned char)i;
        events[4].data.deletion.nonce[i] = (unsigned char)(i + 16u);
        events[8].data.turn_model.id[i] = (unsigned char)(31u - i);
    }
    memcpy(events[4].data.deletion.confirmed_prefix, events[4].data.deletion.session, 4u);
    events[5].data.banner = text("banner caf\xc3\xa9");
    events[6].data.steering = SNAG_BINARY_STEERING_MENTIONS;
    events[7].data.model.before = before;
    events[7].data.model.after = after;
    events[8].data.turn_model.before = before;
    events[8].data.turn_model.effort = text("high");
    events[9].data.effort.before = text("medium");
    events[9].data.effort.after = text("high");
    events[10].data.context.before = (struct snag_binary_context_choice){
        SNAG_BINARY_CONTEXT_DEFAULT, 0u
    };
    events[10].data.context.after = (struct snag_binary_context_choice){
        SNAG_BINARY_CONTEXT_TOKENS, UINT64_C(0x01020304)
    };
    events[11].data.shell = text("/bin/sh");
    for (size_t i = 0; i < 12u; ++i) roundtrip(&events[i]);
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &events[0]));
    static const unsigned char created_bytes[] =
        "\x04\0\x01\x01\0\0\0p\x01\0\0\0m\x01\0\0\0e\x01\0\0\0/";
    assert(payload.len == sizeof(created_bytes) - 1u);
    assert(!memcmp(payload.data, created_bytes, payload.len));
    struct snag_binary_record record = {
        .kind = SNAG_BINARY_SESSION_CREATED, .version = 1u,
        .payload = payload.data, .size = payload.len
    };
    struct snag_binary_event decoded;
    assert(!snag_binary_event_decode(&record, &decoded));
    assert(decoded.data.created.source_format == 4u);
    assert(decoded.data.created.protocol == SNAG_BINARY_RESPONSES);
    assert(decoded.data.created.cwd.size == 1u && decoded.data.created.cwd.data[0] == '/');
    assert(decoded.data.created.selection.provider.data[0] == 'p');
    assert(decoded.data.created.selection.model.data[0] == 'm');
    assert(decoded.data.created.selection.effort.data[0] == 'e');
    payload.data[0] = 5u;
    assert_rejected(record);
    payload.data[0] = 4u;
    payload.data[2] = 2u;
    assert_rejected(record);
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &events[10]));
    static const unsigned char context_bytes[] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 4, 3, 2, 1, 0, 0, 0, 0
    };
    assert(payload.len == sizeof(context_bytes));
    assert(!memcmp(payload.data, context_bytes, payload.len));
    record = (struct snag_binary_record){
        .kind = SNAG_BINARY_CONTEXT_SELECTION_CHANGED, .version = 1u,
        .payload = payload.data, .size = payload.len
    };
    assert(!snag_binary_event_decode(&record, &decoded));
    assert(decoded.data.context.before.mode == SNAG_BINARY_CONTEXT_DEFAULT);
    assert(decoded.data.context.after.mode == SNAG_BINARY_CONTEXT_TOKENS);
    assert(decoded.data.context.after.tokens == UINT64_C(0x01020304));
    payload.data[0] = 3u;
    assert_rejected(record);
    payload.data[0] = 0u;
    payload.data[1] = 1u;
    assert_rejected(record);
    payload.data[1] = 0u;
    memset(payload.data + 10, 0, 8u);
    assert_rejected(record);
    for (uint16_t format = 2u; format <= 4u; ++format) {
        events[0].data.created.source_format = format;
        roundtrip(&events[0]);
    }
    events[5].data.banner = text("");
    roundtrip(&events[5]);
    for (int mode = SNAG_BINARY_STEERING_DEFAULT; mode <= SNAG_BINARY_STEERING_ALL; ++mode) {
        events[6].data.steering = (enum snag_binary_steering)mode;
        roundtrip(&events[6]);
    }
    events[10].data.context.before.mode = SNAG_BINARY_CONTEXT_MAX;
    events[10].data.context.after.tokens = SNAG_CONFIG_TOKEN_LIMIT_MAX;
    roundtrip(&events[10]);
    size_t saved = payload.len;
    events[10].data.context.after.tokens++;
    assert(snag_binary_event_encode(&payload, &events[10]) < 0 && payload.len == saved);
    events[2].data.archive.origin = SNAG_BINARY_MODEL;
    assert(snag_binary_event_encode(&payload, &events[2]) < 0 && payload.len == saved);
    events[6].data.steering = (enum snag_binary_steering)3;
    assert(snag_binary_event_encode(&payload, &events[6]) < 0 && payload.len == saved);
    events[0].data.created.source_format = 1u;
    assert(snag_binary_event_encode(&payload, &events[0]) < 0 && payload.len == saved);
    events[0].data.created.source_format = 4u;
    events[0].data.created.selection.provider = text("");
    assert(snag_binary_event_encode(&payload, &events[0]) < 0 && payload.len == saved);
    events[11].data.shell = text("");
    assert(snag_binary_event_encode(&payload, &events[11]) < 0 && payload.len == saved);
    snag_buf_free(&payload);
}

static void
input_reference_batch(const struct snag_binary_record *source, struct snag_buf *bytes,
    struct snag_binary_batch *batch)
{
    struct snag_binary_identity identity = {.id = {1}, .created_ms = 1u};
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    snag_binary_header_encode(header, &identity);
    struct snag_binary_anchor anchor, next;
    assert(!snag_binary_header_decode(header, sizeof(header), &identity, &anchor));
    struct snag_binary_record records[] = {
        {.kind = SNAG_BINARY_INPUT_CANCELLED, .version = 1u}, *source
    };
    snag_buf_reset(bytes);
    assert(!snag_binary_batch_encode(bytes, &anchor, records, 2u, 0u));
    assert(!snag_binary_batch_decode(bytes->data, bytes->len, &anchor, batch, &next));
}

static void
assert_input_reference_rejected(const struct snag_binary_ref *reference,
    const struct snag_binary_batch *batch, enum snag_binary_input_leaf field)
{
    const unsigned char sentinel = 0;
    const unsigned char *view = &sentinel;
    assert(snag_binary_input_ref_resolve(reference, batch, field, &view) < 0);
    assert(view == &sentinel);
}

static void
assert_input_leaf_missing(const struct snag_binary_batch *batch, uint64_t sequence,
    enum snag_binary_input_leaf field)
{
    struct snag_binary_ref reference = {99u, 98u, 97u};
    assert(snag_binary_input_ref_create(batch, sequence, field, &reference) < 0);
    assert(reference.sequence == 99u && reference.offset == 98u && reference.size == 97u);
    reference.sequence = sequence;
    assert_input_reference_rejected(&reference, batch, field);
}

static void
test_input_references(void)
{
    struct snag_buf instructions = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf parts = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf bytes = {.max = SNAG_BINARY_BATCH_MAX};
    assert(!snag_binary_instructions_encode(&instructions, NULL, 0));
    struct snag_binary_part part = {.kind = SNAG_BINARY_PART_TEXT, .text = text("original")};
    assert(!snag_binary_content_encode(&parts, &part, 1u));
    struct snag_binary_event event = {.kind = SNAG_BINARY_INPUT_RECEIVED};
    event.data.input.selection = (struct snag_binary_selection){
        text("original"), text("m"), text("e")
    };
    event.data.input.instructions = (struct snag_binary_instructions){
        instructions.data, instructions.len
    };
    event.data.input.content = (struct snag_binary_content){parts.data, parts.len};
    event.data.input.text = text("original");
    assert(!snag_binary_event_encode(&payload, &event));
    struct snag_binary_record record = {
        .kind = event.kind, .version = 1u, .payload = payload.data, .size = payload.len
    };
    struct snag_binary_batch batch;
    input_reference_batch(&record, &bytes, &batch);
    const struct snag_binary_text expected[] = {
        text("original"), {parts.data, parts.len}, {instructions.data, instructions.len}
    };
    const uint32_t offsets[] = {40u, 48u, 32u};
    struct snag_binary_ref reference;
    const unsigned char *view;
    for (int field = SNAG_BINARY_INPUT_TEXT; field <= SNAG_BINARY_INPUT_INSTRUCTIONS; ++field) {
        assert(!snag_binary_input_ref_create(&batch, 2u, field, &reference));
        assert(reference.sequence == 2u && reference.offset == offsets[field - 1]);
        assert(reference.size == expected[field - 1].size);
        assert(!snag_binary_input_ref_resolve(&reference, &batch, field, &view));
        assert(!memcmp(view, expected[field - 1].data, reference.size));
    }
    assert(!snag_binary_input_ref_create(&batch, 2u, SNAG_BINARY_INPUT_TEXT, &reference));
    assert_input_reference_rejected(&reference, &batch, SNAG_BINARY_INPUT_CONTENT);
    assert_input_reference_rejected(&reference, &batch, SNAG_BINARY_INPUT_INSTRUCTIONS);
    /* Identical bytes in provider metadata are a valid raw slice, not the input leaf. */
    struct snag_binary_ref wrong = {2u, 14u, 8u};
    assert(!snag_binary_ref_resolve(&wrong, &batch, record.kind, 1u, &view));
    assert(!memcmp(view, "original", 8u));
    assert_input_reference_rejected(&wrong, &batch, SNAG_BINARY_INPUT_TEXT);
    const struct snag_binary_ref invalid[] = {
        {2u, 41u, 7u}, {2u, 36u, 12u}, {2u, 40u, 9u}, {2u, 40u, 0u}, {2u, 57u, 8u},
        {2u, UINT32_MAX, 8u}, {2u, 40u, UINT32_MAX},
        {0u, 40u, 8u}, {1u, 40u, 8u}, {3u, 40u, 8u}, {UINT64_MAX, 40u, 8u}
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i)
        assert_input_reference_rejected(&invalid[i], &batch, SNAG_BINARY_INPUT_TEXT);
    assert_input_leaf_missing(&batch, 0u, SNAG_BINARY_INPUT_TEXT);
    assert_input_leaf_missing(&batch, 1u, SNAG_BINARY_INPUT_TEXT);
    assert_input_leaf_missing(&batch, 3u, SNAG_BINARY_INPUT_TEXT);
    assert_input_leaf_missing(&batch, UINT64_MAX, SNAG_BINARY_INPUT_TEXT);
    assert_input_leaf_missing(&batch, 2u, 0);
    assert_input_leaf_missing(&batch, 2u, 6);
    assert_input_leaf_missing(&batch, 2u, SNAG_BINARY_INPUT_VOICE_TRANSCRIPT);
    assert_input_leaf_missing(NULL, 2u, SNAG_BINARY_INPUT_TEXT);
    assert(snag_binary_input_ref_create(&batch, 2u, SNAG_BINARY_INPUT_TEXT, NULL) < 0);
    assert_input_reference_rejected(NULL, &batch, SNAG_BINARY_INPUT_TEXT);
    assert(snag_binary_input_ref_resolve(&reference, &batch, SNAG_BINARY_INPUT_TEXT, NULL) < 0);

    /* Framing integrity alone cannot establish supported or valid typed semantics. */
    for (int variant = 0; variant < 3; ++variant) {
        struct snag_binary_record changed = record;
        if (variant == 0) changed.version = 2u;
        if (variant == 1) changed.flags = SNAG_BINARY_RECORD_OPTIONAL;
        if (variant == 2) {
            changed.kind = 0x8001u;
            changed.flags = SNAG_BINARY_RECORD_OPTIONAL;
        }
        input_reference_batch(&changed, &bytes, &batch);
        assert_input_leaf_missing(&batch, 2u, SNAG_BINARY_INPUT_TEXT);
        assert_input_reference_rejected(&reference, &batch, SNAG_BINARY_INPUT_TEXT);
    }
    payload.data[40] = 0;
    input_reference_batch(&record, &bytes, &batch);
    assert_input_leaf_missing(&batch, 2u, SNAG_BINARY_INPUT_TEXT);
    payload.data[40] = 'o';
    payload.data[48] = 0; /* Reject bad content even when the requested text is well formed. */
    input_reference_batch(&record, &bytes, &batch);
    assert_input_reference_rejected(&reference, &batch, SNAG_BINARY_INPUT_TEXT);
    payload.data[48] = 1u;

    for (size_t size = 0; size < record.size; ++size) {
        struct snag_binary_record short_record = record;
        short_record.size = size;
        input_reference_batch(&short_record, &bytes, &batch);
        assert_input_leaf_missing(&batch, 2u, SNAG_BINARY_INPUT_TEXT);
    }
    assert(!snag_buf_append(&payload, "x", 1u));
    record.payload = payload.data;
    record.size = payload.len;
    input_reference_batch(&record, &bytes, &batch);
    assert_input_leaf_missing(&batch, 2u, SNAG_BINARY_INPUT_TEXT);

    const enum snag_binary_kind kinds[] = {
        SNAG_BINARY_STEERING_ADDED, SNAG_BINARY_IRC_REPLY_REMINDER,
        SNAG_BINARY_FUTURE_TURN_QUEUED, SNAG_BINARY_FUTURE_TURN_EDITED
    };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(*kinds); ++i) {
        event = (struct snag_binary_event){.kind = kinds[i]};
        if (i < 2u) {
            event.data.steering_input.text = text("original");
            event.data.steering_input.content = (struct snag_binary_content){parts.data, parts.len};
        } else {
            event.data.queued.text = text("original");
            event.data.queued.content = (struct snag_binary_content){parts.data, parts.len};
        }
        snag_buf_reset(&payload);
        assert(!snag_binary_event_encode(&payload, &event));
        record = (struct snag_binary_record){
            .kind = event.kind, .version = 1u, .payload = payload.data, .size = payload.len
        };
        input_reference_batch(&record, &bytes, &batch);
        for (int field = SNAG_BINARY_INPUT_TEXT; field <= SNAG_BINARY_INPUT_CONTENT; ++field) {
            assert(!snag_binary_input_ref_create(&batch, 2u, field, &reference));
            assert(!snag_binary_input_ref_resolve(&reference, &batch, field, &view));
            assert(reference.size == expected[field - 1].size);
            assert(!memcmp(view, expected[field - 1].data, reference.size));
        }
        assert_input_leaf_missing(&batch, 2u, SNAG_BINARY_INPUT_INSTRUCTIONS);
    }

    event = (struct snag_binary_event){.kind = SNAG_BINARY_FUTURE_TURN_QUEUED};
    event.data.queued.text = text("original");
    event.data.queued.has_voice = true;
    event.data.queued.while_kind = SNAG_BINARY_WHILE_NULL;
    event.data.queued.voice = (struct snag_binary_voice_source){
        .input_id = text("i"), .response_id = text("r"), .call_id = text("c"),
        .provider = text("p"), .model = text("m"),
        .transcript = text("original"), .request = text("original")
    };
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    record = (struct snag_binary_record){
        .kind = event.kind, .version = 1u, .payload = payload.data, .size = payload.len
    };
    input_reference_batch(&record, &bytes, &batch);
    assert_input_leaf_missing(&batch, 2u, SNAG_BINARY_INPUT_CONTENT);
    for (int field = SNAG_BINARY_INPUT_VOICE_TRANSCRIPT;
        field <= SNAG_BINARY_INPUT_VOICE_REQUEST; ++field) {
        assert(!snag_binary_input_ref_create(&batch, 2u, field, &reference));
        assert(!snag_binary_input_ref_resolve(&reference, &batch, field, &view));
        assert(reference.size == 8u && !memcmp(view, "original", 8u));
        assert_input_reference_rejected(&reference, &batch, SNAG_BINARY_INPUT_TEXT);
        int other = field == SNAG_BINARY_INPUT_VOICE_TRANSCRIPT ?
            SNAG_BINARY_INPUT_VOICE_REQUEST : SNAG_BINARY_INPUT_VOICE_TRANSCRIPT;
        assert_input_reference_rejected(&reference, &batch, other);
    }
    snag_buf_free(&instructions);
    snag_buf_free(&parts);
    snag_buf_free(&payload);
    snag_buf_free(&bytes);
}

void
test_store_binary_event(void)
{
    test_input_lists();
    test_input_events();
    test_queued_input();
    test_metadata();
    test_input_references();
    static const unsigned char text[] = "goal caf\xc3\xa9";
    struct snag_binary_event event = {.kind = SNAG_BINARY_TIMER_SCHEDULED};
    for (size_t i = 0; i < 16u; ++i) event.data.timer.id[i] = (unsigned char)i;
    event.data.timer.due_ms = UINT64_C(0x0807060504030201);
    event.data.timer.text = (struct snag_binary_text){(const unsigned char *)"hi", 2u};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    static const unsigned char timer_bytes[] =
        "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f"
        "\x01\x02\x03\x04\x05\x06\x07\x08\x02\0\0\0hi";
    assert(payload.len == sizeof(timer_bytes) - 1u);
    assert(!memcmp(payload.data, timer_bytes, payload.len));
    for (int kind = SNAG_BINARY_TIMER_SCHEDULED; kind <= SNAG_BINARY_TIMER_CANCELLED; ++kind) {
        event.kind = (enum snag_binary_kind)kind;
        roundtrip(&event);
    }
    event = (struct snag_binary_event){.kind = SNAG_BINARY_GOAL_REPLACED};
    for (size_t i = 0; i < 16u; ++i) {
        event.data.goal.id[i] = (unsigned char)i;
        event.data.goal.replacement[i] = (unsigned char)(i + 16u);
    }
    event.data.goal.actor = SNAG_BINARY_MODEL;
    event.data.goal.text = (struct snag_binary_text){(const unsigned char *)"work", 4u};
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    static const unsigned char goal_bytes[] =
        "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f"
        "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f"
        "\x02\x04\0\0\0work";
    assert(payload.len == sizeof(goal_bytes) - 1u);
    assert(!memcmp(payload.data, goal_bytes, payload.len));
    event.data.goal.text = (struct snag_binary_text){text, sizeof(text) - 1u};
    event.data.goal.pause = SNAG_BINARY_PAUSE_USER;
    for (int kind = SNAG_BINARY_GOAL_STARTED; kind <= SNAG_BINARY_GOAL_CANCELLED; ++kind) {
        event.kind = (enum snag_binary_kind)kind;
        roundtrip(&event);
    }
    event.kind = SNAG_BINARY_GOAL_PAUSED;
    for (int pause = SNAG_BINARY_PAUSE_INPUT_CLOSED; pause <= SNAG_BINARY_PAUSE_USER; ++pause) {
        event.data.goal.pause = (enum snag_binary_pause)pause;
        roundtrip(&event);
    }
    event.kind = SNAG_BINARY_GOAL_LOCK_CHANGED;
    event.data.goal.locked = true;
    roundtrip(&event);
    event.kind = SNAG_BINARY_GOAL_REPLACED;
    event.data.goal.actor = SNAG_BINARY_USER;
    roundtrip(&event);
    event.kind = SNAG_BINARY_GOAL_REWORDED;
    roundtrip(&event);
    event.kind = SNAG_BINARY_GOAL_COMPLETED;
    roundtrip(&event);
    /* Decode compatibility is explicit; it never silently drops an unknown
     * required event, an unknown semantic version or an unrecognized flag. */
    struct snag_binary_record record = {.kind = 31u, .version = 1u};
    assert_rejected(record);
    record.flags = SNAG_BINARY_RECORD_OPTIONAL;
    assert_rejected(record);
    record.kind = 0x8000u;
    struct snag_binary_event kept = event;
    assert(snag_binary_event_decode(&record, &kept) == 1);
    assert(!memcmp(&event, &kept, sizeof(kept)));
    record.flags = 0u;
    assert_rejected(record);
    record.flags = 3u;
    assert_rejected(record);
    record.flags = SNAG_BINARY_RECORD_OPTIONAL;
    record.version = 0u;
    assert_rejected(record);
    /* Invalid UTF-8, NUL, oversized strings and missing actors fail without
     * appending an incomplete payload to the caller's pending transaction. */
    size_t saved = payload.len;
    event.kind = SNAG_BINARY_GOAL_STARTED;
    static const unsigned char bad_utf8[] = {0xc3u, 0x28u};
    event.data.goal.text = (struct snag_binary_text){bad_utf8, sizeof(bad_utf8)};
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    static const unsigned char with_nul[] = {'a', 0, 'b'};
    event.data.goal.text = (struct snag_binary_text){with_nul, sizeof(with_nul)};
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event.data.goal.text = (struct snag_binary_text){text, 0u};
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event.data.goal.text = (struct snag_binary_text){NULL, 1u};
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event.data.goal.text = (struct snag_binary_text){text, sizeof(text) - 1u};
    event.kind = SNAG_BINARY_GOAL_REWORDED;
    event.data.goal.actor = 0;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event.kind = SNAG_BINARY_GOAL_BLOCKED;
    event.data.goal.actor = SNAG_BINARY_USER;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event.kind = SNAG_BINARY_GOAL_PAUSED;
    event.data.goal.pause = 0;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event.kind = (enum snag_binary_kind)31;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event = (struct snag_binary_event){.kind = SNAG_BINARY_TIMER_SCHEDULED};
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    /* Retain the existing maximum goal text, independently of batch target. */
    unsigned char *large = malloc(SNAG_MAX_GOAL_PROMPT + 1u);
    assert(large);
    memset(large, 'x', SNAG_MAX_GOAL_PROMPT + 1u);
    event = (struct snag_binary_event){.kind = SNAG_BINARY_GOAL_STARTED};
    event.data.goal.text = (struct snag_binary_text){large, SNAG_MAX_GOAL_PROMPT};
    roundtrip(&event);
    event.data.goal.text.size++;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    free(large);
    snag_buf_free(&payload);
    /* Field-level corruption is rejected even without depending on the outer
     * batch checksum; semantic enums and lengths have their own validation. */
    static const enum snag_binary_kind malformed[] = {
        SNAG_BINARY_GOAL_LOCK_CHANGED, SNAG_BINARY_GOAL_PAUSED,
        SNAG_BINARY_GOAL_COMPLETED, SNAG_BINARY_GOAL_BLOCKED,
        SNAG_BINARY_TIMER_SCHEDULED, SNAG_BINARY_GOAL_STARTED
    };
    for (size_t i = 0; i < sizeof(malformed) / sizeof(*malformed); ++i) {
        payload.max = SNAG_MAX_EVENT_LINE;
        event = (struct snag_binary_event){.kind = malformed[i]};
        if (event.kind == SNAG_BINARY_TIMER_SCHEDULED) {
            event.data.timer.due_ms = 1u;
            event.data.timer.text = (struct snag_binary_text){text, sizeof(text) - 1u};
        } else {
            event.data.goal.actor = SNAG_BINARY_MODEL;
            event.data.goal.pause = SNAG_BINARY_PAUSE_USER;
            event.data.goal.text = (struct snag_binary_text){text, sizeof(text) - 1u};
        }
        assert(!snag_binary_event_encode(&payload, &event));
        record = (struct snag_binary_record){
            .kind = (uint16_t)event.kind, .version = 1u,
            .payload = payload.data, .size = payload.len
        };
        if (event.kind == SNAG_BINARY_TIMER_SCHEDULED) memset(payload.data + 16, 0, 8u);
        else payload.data[16] = 0xffu;
        assert_rejected(record);
        if (event.kind == SNAG_BINARY_GOAL_STARTED) {
            payload.data[16] = (unsigned char)(sizeof(text) - 1u);
            payload.data[20] = 0u;
            assert_rejected(record);
            payload.data[20] = 0xffu;
            assert_rejected(record);
        }
        snag_buf_free(&payload);
    }
}
