/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_event.h"
#include "fs.h"
#include "instructions.h"
#include "irc.h"
#include "json.h"
#include "media.h"
#include "store.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
        .kind = (uint16_t)event->kind, .version = snag_binary_event_version(event->kind),
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
    record.version = snag_binary_event_version(event->kind) + 1u;
    assert_rejected(record);
    record.version = snag_binary_event_version(event->kind);
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
    static const unsigned char instruction_bytes[] = "\x01\0\0\0\0/a\0";
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
    instructions[0].path = (struct snag_binary_text){(const unsigned char *)"/a\0x", 4u};
    assert(snag_binary_instructions_encode(&bytes, instructions, 2u) < 0 && bytes.len == saved);
    instructions[0].path = text("/\xff");
    assert(snag_binary_instructions_encode(&bytes, instructions, 2u) < 0 && bytes.len == saved);
    assert(snag_binary_instructions_encode(&bytes, instructions, SIZE_MAX) < 0 &&
           bytes.len == saved);
    snag_buf_reset(&bytes);
    assert(!snag_binary_instructions_encode(&bytes, NULL, 0));
    assert(bytes.len == 4u && !memcmp(bytes.data, "\0\0\0\0", 4u));
    assert(!snag_binary_instructions_decode(bytes.data, bytes.len, &paths));
    offset = 0;
    assert(snag_binary_instructions_next(&paths, &offset, &decoded_paths[0]) == 1 && !offset);
    unsigned char *maximum = malloc(SNAG_PATH_MAX_BYTES + 1u);
    assert(maximum);
    memset(maximum, 'x', SNAG_PATH_MAX_BYTES + 1u);
    maximum[0] = '/';
    instructions[0].path = (struct snag_binary_text){maximum, SNAG_PATH_MAX_BYTES};
    snag_buf_reset(&bytes);
    assert(!snag_binary_instructions_encode(&bytes, instructions, 1u));
    assert(bytes.len == SNAG_PATH_MAX_BYTES + 6u);
    assert(!snag_binary_instructions_decode(bytes.data, bytes.len, &paths));
    offset = 0u;
    assert(!snag_binary_instructions_next(&paths, &offset, &decoded_paths[0]));
    assert(decoded_paths[0].path.size == SNAG_PATH_MAX_BYTES);
    assert(!memcmp(decoded_paths[0].path.data, maximum, SNAG_PATH_MAX_BYTES));
    saved = bytes.len;
    instructions[0].path.size++;
    assert(snag_binary_instructions_encode(&bytes, instructions, 1u) < 0 && bytes.len == saved);
    bytes.data[bytes.len - 1u] = 'x';
    assert(!snag_buf_putc(&bytes, 0));
    assert(snag_binary_instructions_decode(bytes.data, bytes.len, &paths) < 0);
    free(maximum);
    snag_buf_free(&again); snag_buf_free(&bytes);
}

static void
test_instruction_legacy_budget(void)
{
    const size_t count = 1024u;
    const size_t path_size = 16379u;
    struct snag_binary_instruction *items = calloc(count, sizeof(*items));
    char *path = malloc(path_size + 1u);
    json_t *paths = json_array();
    assert(items && path && paths && path_size <= SNAG_PATH_MAX_BYTES);
    memset(path, 'x', path_size);
#ifdef _WIN32
    memcpy(path, "C:/", 3u);
#else
    path[0] = '/';
#endif
    for (size_t i = 0u; i < count; ++i) {
        assert(snprintf(path + path_size - 8u, 9u, "%08u", (unsigned int)i) == 8);
        assert(!json_array_append_new(paths, json_stringn(path, path_size)));
        items[i].path = text(json_string_value(json_array_get(paths, i)));
    }
    char error[128];
    assert(!snag_instructions_metadata_valid(paths, error, sizeof(error)));
    json_t *data = json_pack("{s:s,s:O,s:s,s:s,s:b,s:I,s:s}", "effort", "e",
        "instructions", paths, "model", "m", "provider", "p", "read_only", 0,
        "received_at_ms", (json_int_t)0, "text", "t");
    char hash[65];
    memset(hash, '0', 64u);
    hash[64] = '\0';
    /* Exact envelope shape; source-chain binding is independent of this size check. */
    json_t *record = json_pack("{s:O,s:s,s:I,s:s,s:I,s:s,s:i,s:I}", "data", data,
        "prev_sha256", hash, "seq", (json_int_t)2,
        "session_id", "0123456789abcdef0011223344556677", "time_ms", (json_int_t)1,
        "type", "input_received", "v", 2, "checkpoint_offset", (json_int_t)0);
    assert(data && record && !snag_json_digest(record, hash));
    assert(!snag_json_set_new(record, "event_sha256", json_string(hash)));
    struct snag_buf legacy = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_json_canonical(record, &legacy) && !snag_buf_putc(&legacy, '\n'));
    assert(legacy.len < SNAG_MAX_EVENT_LINE);
    /* Length4 plus flag1 per path exceeds the native limit by four bytes. */
    assert(4u + count * (5u + path_size) == SNAG_MAX_EVENT_LINE + 4u);
    struct snag_buf encoded = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_instructions_encode(&encoded, items, count));
    assert(encoded.len == 4u + count * (2u + path_size));
    struct snag_binary_event event = {.kind = SNAG_BINARY_INPUT_RECEIVED};
    event.data.input.selection = (struct snag_binary_selection){text("p"), text("m"), text("e")};
    event.data.input.text = text("t");
    event.data.input.instructions = (struct snag_binary_instructions){encoded.data, encoded.len};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event) && payload.len < legacy.len);
    struct snag_binary_record native = {
        .kind = 96u, .version = 2u, .payload = payload.data, .size = payload.len
    };
    struct snag_binary_event decoded;
    assert(!snag_binary_event_decode(&native, &decoded));
    size_t offset = 0u;
    struct snag_binary_instruction item;
    for (size_t i = 0u; i < count; ++i) {
        assert(!snag_binary_instructions_next(&decoded.data.input.instructions, &offset, &item));
        assert(!item.has_snapshot && item.path.size == path_size);
        assert(!memcmp(item.path.data, items[i].path.data, path_size));
    }
    assert(snag_binary_instructions_next(&decoded.data.input.instructions, &offset, &item) == 1);
    snag_buf_free(&payload);
    snag_buf_free(&encoded);
    snag_buf_free(&legacy);
    json_decref(record);
    json_decref(data);
    json_decref(paths);
    free(path);
    free(items);
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
    assert(!snag_binary_batch_encode(bytes, &anchor, records, 2u, 0u, NULL));
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
control_reference_rejected(const struct snag_binary_ref *reference,
    const struct snag_binary_batch *batch, enum snag_binary_kind kind)
{
    struct snag_binary_control_text_source source = {
        .event.kind = SNAG_BINARY_GOAL_CANCELLED, .text = text("kept")};
    unsigned char saved[sizeof(source)];
    memcpy(saved, &source, sizeof(source));
    assert(snag_binary_control_text_ref_resolve(reference, batch, kind, &source) < 0);
    assert(!memcmp(saved, &source, sizeof(source)));
}

static void
control_source_missing(const struct snag_binary_batch *batch, uint64_t sequence,
    enum snag_binary_kind kind)
{
    struct snag_binary_ref reference = {99u, 98u, 97u};
    assert(snag_binary_control_text_ref_create(batch, sequence, kind, &reference) < 0);
    assert(reference.sequence == 99u && reference.offset == 98u && reference.size == 97u);
    reference.sequence = sequence;
    control_reference_rejected(&reference, batch, kind);
}

static void
test_control_text_references(void)
{
    static const enum snag_binary_kind kinds[] = {SNAG_BINARY_SESSION_CREATED,
        SNAG_BINARY_CWD_CHANGED, SNAG_BINARY_BANNER_UPDATED, SNAG_BINARY_TIMER_SCHEDULED,
        SNAG_BINARY_GOAL_STARTED, SNAG_BINARY_GOAL_REPLACED, SNAG_BINARY_GOAL_REWORDED,
        SNAG_BINARY_GOAL_BLOCKED, SNAG_BINARY_BANNER_UPDATED, SNAG_BINARY_SESSION_NAMED};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf bytes = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_buf again = {.max = SNAG_MAX_EVENT_LINE};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(*kinds); ++i) {
        struct snag_binary_text expected = text(i == 8u ? "" : "/tmp/caf\xc3\xa9");
        struct snag_binary_event event = {.kind = kinds[i]};
        if (i == 0u) {
            event.data.created.source_format = 4u;
            event.data.created.protocol = SNAG_BINARY_RESPONSES;
            event.data.created.selection = (struct snag_binary_selection){
                .provider = expected, .model = text("m"), .effort = text("e")};
            event.data.created.cwd = expected;
        } else if (i == 1u) {
            event.data.cwd.before = event.data.cwd.after = expected;
        } else if (event.kind == SNAG_BINARY_BANNER_UPDATED) {
            event.data.banner = expected;
        } else if (event.kind == SNAG_BINARY_SESSION_NAMED) {
            event.data.name = expected;
        } else if (i == 3u) {
            memset(event.data.timer.id, 0x11, 16u);
            event.data.timer.due_ms = UINT64_C(0x1122334455667788);
            event.data.timer.text = expected;
        } else {
            memset(event.data.goal.id, 0x22, 16u);
            memset(event.data.goal.replacement, 0x33, 16u);
            event.data.goal.actor = SNAG_BINARY_MODEL;
            event.data.goal.text = expected;
        }
        snag_buf_reset(&payload);
        assert(!snag_binary_event_encode(&payload, &event));
        struct snag_binary_record record = {.kind = event.kind, .version = 1u,
            .timestamp_ms = 17u, .payload = payload.data, .size = payload.len};
        struct snag_binary_batch batch;
        input_reference_batch(&record, &bytes, &batch);
        struct snag_binary_ref reference;
        assert(!snag_binary_control_text_ref_create(&batch, 2u, event.kind, &reference));
        /* Each selected control text is the last field, unlike equal metadata. */
        assert(reference.sequence == 2u && reference.size == expected.size &&
            reference.offset == payload.len - expected.size);
        struct snag_binary_control_text_source source;
        assert(!snag_binary_control_text_ref_resolve(&reference, &batch, event.kind, &source));
        assert(source.event.kind == event.kind && source.text.size == expected.size &&
            !memcmp(source.text.data, expected.data, expected.size));
        snag_buf_reset(&again);
        assert(!snag_binary_event_encode(&again, &source.event));
        assert(again.len == payload.len && !memcmp(again.data, payload.data, payload.len));

        if (i < 2u) {
            struct snag_binary_ref metadata = reference;
            metadata.offset = i == 0u ? 7u : 4u;
            const unsigned char *view;
            assert(!snag_binary_ref_resolve(&metadata, &batch, event.kind, 1u, &view));
            assert(!memcmp(view, expected.data, expected.size));
            control_reference_rejected(&metadata, &batch, event.kind);
        }
        for (size_t other = 0; other < sizeof(kinds) / sizeof(*kinds); ++other) {
            if (kinds[other] == event.kind) continue;
            control_source_missing(&batch, 2u, kinds[other]);
            control_reference_rejected(&reference, &batch, kinds[other]);
        }
        control_source_missing(&batch, 2u, 0);
        control_source_missing(&batch, 2u, (enum snag_binary_kind)0x10001u);
        for (unsigned int fault = 0u; fault < 10u; ++fault) {
            struct snag_binary_ref bad = reference;
            if (fault == 0u) bad.sequence = 0u;
            if (fault == 1u) bad.sequence = 1u;
            if (fault == 2u) bad.sequence = 3u;
            if (fault == 3u) bad.sequence = UINT64_MAX;
            if (fault == 4u) bad.offset++;
            if (fault == 5u) bad.offset--;
            if (fault == 6u) bad.size++;
            if (fault == 7u) bad.size--;
            if (fault == 8u) bad.offset = UINT32_MAX;
            if (fault == 9u) bad.size = UINT32_MAX;
            control_reference_rejected(&bad, &batch, event.kind);
            if (fault < 4u) control_source_missing(&batch, bad.sequence, event.kind);
        }
        control_source_missing(NULL, 2u, event.kind);
        control_reference_rejected(NULL, &batch, event.kind);
        assert(snag_binary_control_text_ref_create(&batch, 2u, event.kind, NULL) < 0);
        assert(snag_binary_control_text_ref_resolve(&reference, &batch, event.kind, NULL) < 0);

        for (unsigned int fault = 0u; fault < 3u; ++fault) {
            struct snag_binary_record bad = record;
            if (fault == 0u) bad.version++;
            if (fault == 1u) bad.flags = SNAG_BINARY_RECORD_OPTIONAL;
            if (fault == 2u) {
                bad.kind = 0x8001u;
                bad.flags = SNAG_BINARY_RECORD_OPTIONAL;
            }
            input_reference_batch(&bad, &bytes, &batch);
            control_source_missing(&batch, 2u, (enum snag_binary_kind)bad.kind);
            control_reference_rejected(&reference, &batch, event.kind);
        }
        for (size_t size = 0; size < record.size; ++size) {
            struct snag_binary_record bad = record;
            bad.size = size;
            input_reference_batch(&bad, &bytes, &batch);
            control_source_missing(&batch, 2u, event.kind);
        }

        /* Invalid unselected metadata must reject an otherwise intact leaf. */
        size_t offset = reference.offset;
        if (i == 0u) offset = 2u; /* Protocol. */
        if (i == 1u) offset = 4u; /* NUL in previous cwd. */
        if (i == 3u) offset = 16u; /* Nonzero deadline. */
        if (i == 5u) offset = 32u; /* Replacement actor. */
        if (i == 6u || i == 7u) offset = 16u; /* Reword/block actor. */
        if (i != 8u) {
            unsigned char saved[8];
            size_t width = i == 3u ? 8u : 1u;
            memcpy(saved, payload.data + offset, width);
            memset(payload.data + offset, i == 7u ? SNAG_BINARY_USER : 0, width);
            input_reference_batch(&record, &bytes, &batch);
            control_source_missing(&batch, 2u, event.kind);
            memcpy(payload.data + offset, saved, width);
        }
        assert(!snag_buf_putc(&payload, 0u));
        record.payload = payload.data;
        record.size = payload.len;
        input_reference_batch(&record, &bytes, &batch);
        control_source_missing(&batch, 2u, event.kind);
    }

    /* State changes without text cannot become canonical text sources. */
    static const enum snag_binary_kind absent[] = {SNAG_BINARY_TIMER_FIRED,
        SNAG_BINARY_TIMER_CANCELLED, SNAG_BINARY_GOAL_LOCK_CHANGED, SNAG_BINARY_GOAL_PAUSED,
        SNAG_BINARY_GOAL_COMPLETED, SNAG_BINARY_GOAL_RESUMED, SNAG_BINARY_GOAL_CANCELLED,
        SNAG_BINARY_STEERING_UPDATED};
    for (size_t i = 0; i < sizeof(absent) / sizeof(*absent); ++i) {
        struct snag_binary_event event = {.kind = absent[i]};
        if (i < 2u) memset(event.data.timer.id, 0x11, 16u);
        else if (event.kind != SNAG_BINARY_STEERING_UPDATED) {
            memset(event.data.goal.id, 0x22, 16u);
            event.data.goal.actor = SNAG_BINARY_MODEL;
            event.data.goal.pause = SNAG_BINARY_PAUSE_USER;
        }
        snag_buf_reset(&payload);
        assert(!snag_binary_event_encode(&payload, &event));
        struct snag_binary_record record = {.kind = event.kind, .version = 1u,
            .payload = payload.data, .size = payload.len};
        struct snag_binary_batch batch;
        input_reference_batch(&record, &bytes, &batch);
        control_source_missing(&batch, 2u, event.kind);
    }
    snag_buf_free(&again);
    snag_buf_free(&bytes);
    snag_buf_free(&payload);
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
        if (variant == 0) changed.version = 3u;
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

static void
test_input_reference_variants(void)
{
    struct snag_buf instructions = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf parts = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf source = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf bytes = {.max = SNAG_BINARY_BATCH_MAX};
    assert(!snag_binary_instructions_encode(&instructions, NULL, 0));
    struct snag_binary_part part = {.kind = SNAG_BINARY_PART_TEXT, .text = text("attachment")};
    assert(!snag_binary_content_encode(&parts, &part, 1u));
    struct snag_binary_event event = {.kind = SNAG_BINARY_INPUT_RECEIVED};
    event.data.input.selection = (struct snag_binary_selection){text("p"), text("m"), text("e")};
    event.data.input.text = text("original");
    event.data.input.content = (struct snag_binary_content){parts.data, parts.len};
    event.data.input.instructions = (struct snag_binary_instructions){
        instructions.data, instructions.len
    };
    assert(!snag_binary_event_encode(&source, &event));
    struct snag_binary_record original = {
        .kind = event.kind, .version = 2u, .payload = source.data, .size = source.len
    };
    struct snag_binary_batch batch;
    input_reference_batch(&original, &bytes, &batch);
    struct snag_binary_input_reference references[3] = {0};
    for (size_t i = 0; i < 3u; ++i) {
        references[i].field = (enum snag_binary_input_leaf)(i + 1u);
        assert(!snag_binary_input_ref_create(&batch, 2u, references[i].field,
            &references[i].target));
    }
    event = (struct snag_binary_event){.kind = SNAG_BINARY_FUTURE_TURN_EDITED};
    event.data.queued.id[0] = 7u;
    event.data.queued.text_ref = references[0];
    event.data.queued.content_ref = references[1];
    roundtrip(&event);
    assert(!snag_binary_event_encode(&payload, &event));
    static const unsigned char golden[59] = {
        [0] = 7u, [16] = 16u,
        [17] = 255u, [18] = 255u, [19] = 255u, [20] = 255u, [21] = 1u,
        [22] = 2u, [30] = 33u, [34] = 8u,
        [38] = 255u, [39] = 255u, [40] = 255u, [41] = 255u, [42] = 2u,
        [43] = 2u, [51] = 41u, [55] = 19u
    };
    assert(payload.len == sizeof(golden) && !memcmp(payload.data, golden, sizeof(golden)));
    struct snag_binary_record record = {
        .kind = event.kind, .version = 1u, .payload = payload.data, .size = payload.len
    };
    assert_rejected(record); /* Reference bytes cannot masquerade as a v1 literal. */
    record.version = 2u;
    struct snag_binary_event decoded;
    assert(!snag_binary_event_decode(&record, &decoded));
    assert(!decoded.data.queued.text.data && !decoded.data.queued.content.data);
    const unsigned char *view;
    assert(!snag_binary_input_ref_resolve(&decoded.data.queued.text_ref.target, &batch,
        decoded.data.queued.text_ref.field, &view));
    assert(!memcmp(view, "original", 8u));

    /* Two metadata edits retain the same canonical input and attachment once. */
    struct snag_buf edited = {.max = SNAG_MAX_EVENT_LINE};
    event.data.queued.read_only = true;
    assert(!snag_binary_event_encode(&edited, &event));
    event.data.queued.read_only = false;
    struct snag_binary_record records[] = {
        {.kind = SNAG_BINARY_INPUT_CANCELLED, .version = 1u}, original, record,
        {.kind = event.kind, .version = 2u, .payload = edited.data, .size = edited.len}
    };
    struct snag_binary_identity identity = {.id = {1}, .created_ms = 1u};
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    snag_binary_header_encode(header, &identity);
    struct snag_binary_anchor anchor, next;
    assert(!snag_binary_header_decode(header, sizeof(header), &identity, &anchor));
    snag_buf_reset(&bytes);
    assert(!snag_binary_batch_encode(&bytes, &anchor, records, 4u, 0u, NULL));
    assert(!snag_binary_batch_decode(bytes.data, bytes.len, &anchor, &batch, &next));
    size_t cursor = SNAG_BINARY_BATCH_HEADER_SIZE;
    unsigned int originals = 0u;
    unsigned int attachments = 0u;
    struct snag_binary_record item;
    uint64_t sequence;
    for (size_t i = 0; i < 4u; ++i) {
        assert(!snag_binary_record_next(&batch, &cursor, &item, &sequence));
        for (size_t j = 0; j + 8u <= item.size; ++j) {
            if (!memcmp(item.payload + j, "original", 8u)) ++originals;
            if (j + 10u <= item.size && !memcmp(item.payload + j, "attachment", 10u))
                ++attachments;
        }
        if (sequence < 3u) continue;
        assert(!snag_binary_event_decode(&item, &decoded));
        assert(decoded.data.queued.read_only == (sequence == 4u));
        assert(!snag_binary_input_ref_resolve(&decoded.data.queued.text_ref.target, &batch,
            decoded.data.queued.text_ref.field, &view));
        assert(!memcmp(view, "original", 8u));
        assert(!snag_binary_input_ref_resolve(&decoded.data.queued.content_ref.target, &batch,
            decoded.data.queued.content_ref.field, &view));
        assert(!memcmp(view, parts.data, parts.len));
        assert_input_leaf_missing(&batch, sequence, SNAG_BINARY_INPUT_TEXT);
    }
    assert(originals == 1u && attachments == 1u);
    snag_buf_free(&edited);
    /* A reference-valued field cannot become a new canonical leaf. */
    input_reference_batch(&record, &bytes, &batch);
    assert_input_leaf_missing(&batch, 2u, SNAG_BINARY_INPUT_TEXT);
    assert_input_leaf_missing(&batch, 2u, SNAG_BINARY_INPUT_CONTENT);
    for (unsigned int field = 0; field <= 6u; ++field) {
        payload.data[21] = (unsigned char)field;
        if (field == SNAG_BINARY_INPUT_TEXT || field == SNAG_BINARY_INPUT_VOICE_TRANSCRIPT ||
            field == SNAG_BINARY_INPUT_VOICE_REQUEST) {
            assert(!snag_binary_event_decode(&record, &decoded));
        } else {
            assert_rejected(record);
        }
    }
    payload.data[21] = 1u;
    payload.data[22] = 0;
    assert_rejected(record);
    payload.data[22] = 2u;
    payload.data[34] = 0;
    assert_rejected(record);
    payload.data[34] = 8u;
    payload.data[55] = 8u;
    assert_rejected(record);
    payload.data[55] = 19u;
    size_t saved = payload.len;
    event.data.queued.text = text("ambiguous");
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event.data.queued.text = (struct snag_binary_text){0};
    const struct snag_binary_ref invalid[] = {
        {0u, 33u, 8u}, {UINT64_MAX, 33u, 8u}, {2u, UINT32_MAX, 8u},
        {2u, 33u, 0u}, {2u, 33u, SNAG_MAX_QUEUED_TEXT + 1u}
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        event.data.queued.text_ref.target = invalid[i];
        assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    }
    event.data.queued.text_ref = references[1];
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event.data.queued.text_ref = references[0];
    event.data.queued.text_ref.field = 0;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);

    const enum snag_binary_kind kinds[] = {
        SNAG_BINARY_INPUT_RECEIVED, SNAG_BINARY_STEERING_ADDED, SNAG_BINARY_IRC_REPLY_REMINDER,
        SNAG_BINARY_FUTURE_TURN_QUEUED
    };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(*kinds); ++i) {
        event = (struct snag_binary_event){.kind = kinds[i]};
        if (i == 0u) {
            event.data.input.selection = (struct snag_binary_selection){
                text("p"), text("m"), text("e")
            };
            event.data.input.text_ref = references[0];
            event.data.input.content_ref = references[1];
            event.data.input.instructions_ref = references[2];
        } else if (i < 3u) {
            event.data.steering_input.text_ref = references[0];
            event.data.steering_input.content_ref = references[1];
        } else {
            event.data.queued.text_ref = references[0];
            event.data.queued.content_ref = references[1];
        }
        roundtrip(&event);
    }
    event.data.queued.content_ref = (struct snag_binary_input_reference){0};
    event.data.queued.content = (struct snag_binary_content){parts.data, parts.len};
    roundtrip(&event); /* Literal attachments beside referenced text. */
    event.data.queued.content = (struct snag_binary_content){0};
    event.data.queued.has_voice = true;
    event.data.queued.while_kind = SNAG_BINARY_WHILE_NULL;
    event.data.queued.voice = (struct snag_binary_voice_source){
        .input_id = text("i"), .response_id = text("r"), .call_id = text("c"),
        .provider = text("p"), .model = text("m"),
        .transcript_ref = references[0], .request_ref = references[0]
    };
    roundtrip(&event);
    event.data.queued.voice.transcript = text("ambiguous");
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    assert(snag_binary_event_version(SNAG_BINARY_INPUT_RECEIVED) == 2u);
    assert(snag_binary_event_version(SNAG_BINARY_GOAL_STARTED) == 1u);
    assert(!snag_binary_event_version(31));
    snag_buf_free(&instructions);
    snag_buf_free(&parts);
    snag_buf_free(&source);
    snag_buf_free(&payload);
    snag_buf_free(&bytes);
}

static struct snag_binary_event
turn_start_event(void)
{
    static const unsigned char empty_instructions[4];
    struct snag_binary_event event = {.kind = SNAG_BINARY_TURN_STARTED};
    event.data.started.number = 1u;
    event.data.started.cwd = text("/w");
    event.data.started.text = text("x");
    event.data.started.instructions = (struct snag_binary_instructions){empty_instructions, 4u};
    event.data.started.config.selection = (struct snag_binary_selection){
        text("p"), text("m"), text("e")
    };
    return event;
}

static void
test_turn_starts(void)
{
    struct snag_binary_event event = turn_start_event();
    event.data.started.workspace = true;
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    static const unsigned char golden[] =
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\x01\0\0\0\0\0\0\0\0\x10"
        "\x02\0\0\0/w\x01\0\0\0p\x01\0\0\0m\x01\0\0\0e"
        "\0\0\x06\x07\0\0\0\0\x01\0\0\0x";
    assert(payload.len == sizeof(golden) - 1u && !memcmp(payload.data, golden, payload.len));
    assert(snag_binary_event_version(event.kind) == 1u);
    roundtrip(&event);
    struct snag_binary_record record = {
        .kind = event.kind, .version = 1u, .payload = payload.data, .size = payload.len
    };
    struct snag_binary_event decoded;
    assert(!snag_binary_event_decode(&record, &decoded));
    assert(decoded.data.started.workspace && !decoded.data.started.has_read_only);
    assert(!decoded.data.started.config.present && !decoded.data.started.has_received_ms);
    payload.data[24] = 4u;
    assert_rejected(record);
    payload.data[24] = 0u;
    payload.data[25] = 32u;
    assert_rejected(record);
    payload.data[25] = 2u;
    /* A value without its presence bit is invalid. */
    assert_rejected(record);
    payload.data[25] = 16u;
    payload.data[16] = 0u;
    assert_rejected(record);

    struct snag_binary_turn_config *config = &event.data.started.config;
    config->present = 0x7fffu;
    for (size_t i = 0; i < SNAG_BINARY_TURN_NUMBER_COUNT; ++i) {
        config->numbers[i] = UINT64_C(0x0807060504030201) + i;
    }
    static const unsigned char values[] = {0u, 1u, 2u, 3u, 1u, 4u, 'z', 0u, 5u, 7u};
    static const size_t widths[] = {1u, 1u, 1u, 2u, 3u, 2u};
    size_t cursor = 0u;
    for (size_t i = 0u; i < SNAG_BINARY_TURN_VALUE_COUNT; ++i) {
        config->values[i] = (struct snag_binary_result_value){
            .data = values + cursor, .size = widths[i]
        };
        cursor += widths[i];
    }
    config->parallel_calls = true;
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    record.payload = payload.data;
    record.size = payload.len;
    assert(!snag_binary_event_decode(&record, &decoded));
    assert(decoded.data.started.config.present == 0x7fffu);
    for (size_t i = 0; i < SNAG_BINARY_TURN_NUMBER_COUNT; ++i) {
        assert(decoded.data.started.config.numbers[i] == config->numbers[i]);
        for (size_t j = 0; j < 8u; ++j) {
            assert(payload.data[49u + 8u * i + j] == (j ? j + 1u : i + 1u));
        }
    }
    size_t tag = 49u + 8u * SNAG_BINARY_TURN_NUMBER_COUNT;
    assert(!memcmp(payload.data + tag, values, sizeof(values)));
    assert(decoded.data.started.config.values[SNAG_BINARY_TURN_MAX_OUTPUT].kind ==
        SNAG_BINARY_RESULT_ARRAY);
    assert(decoded.data.started.config.parallel_calls);
    roundtrip(&event);
    payload.data[48] |= 0x80u;
    assert_rejected(record);
    payload.data[48] &= 0x7fu;
    payload.data[tag] = 8u;
    assert_rejected(record);
    payload.data[tag] = 0u;
    payload.data[tag + sizeof(values)] = 2u;
    assert_rejected(record);
    payload.data[tag + sizeof(values)] = 1u;
    payload.data[tag + sizeof(values) + 1u] = 0u;
    assert_rejected(record);

    for (unsigned int i = 0; i < 15u; ++i) {
        config->present = (uint16_t)(1u << i);
        roundtrip(&event);
    }
    config->present = 1u << (8u + SNAG_BINARY_TURN_MAX_OUTPUT);
    static const unsigned char zero[] = {128u};
    config->values[SNAG_BINARY_TURN_MAX_OUTPUT] = (struct snag_binary_result_value){
        .data = values, .size = 1u
    };
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    size_t null_size = payload.len;
    record.payload = payload.data;
    record.size = payload.len;
    assert(!snag_binary_event_decode(&record, &decoded));
    assert(decoded.data.started.config.values[SNAG_BINARY_TURN_MAX_OUTPUT].kind ==
        SNAG_BINARY_RESULT_NULL);
    roundtrip(&event);
    config->values[SNAG_BINARY_TURN_MAX_OUTPUT] = (struct snag_binary_result_value){
        .data = zero, .size = 1u
    };
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    assert(payload.len == null_size && payload.data[49] == 128u);
    record.payload = payload.data;
    record.size = payload.len;
    assert(!snag_binary_event_decode(&record, &decoded));
    assert(decoded.data.started.config.values[SNAG_BINARY_TURN_MAX_OUTPUT].kind ==
        SNAG_BINARY_RESULT_INTEGER);
    assert(!decoded.data.started.config.values[SNAG_BINARY_TURN_MAX_OUTPUT].integer);
    roundtrip(&event);

    config->present = SNAG_BINARY_TURN_PARALLEL_CALLS;
    config->parallel_calls = false;
    for (unsigned int origin = 0; origin <= SNAG_BINARY_TURN_TIMER; ++origin) {
        event.data.started.origin = (enum snag_binary_turn_origin)origin;
        event.data.started.queue_seq = 17u;
        memset(event.data.started.queue_id, 0x5a, 16u);
        for (unsigned int flags = 0; flags < 16u; ++flags) {
            if ((flags & 2u) && !(flags & 1u)) continue;
            event.data.started.has_read_only = (flags & 1u) != 0;
            event.data.started.read_only = (flags & 2u) != 0;
            event.data.started.has_received_ms = (flags & 4u) != 0;
            event.data.started.workspace = (flags & 8u) != 0;
            event.data.started.received_ms = UINT64_C(0x1234567890);
            roundtrip(&event);
        }
    }

    snag_buf_reset(&payload);
    assert(!snag_buf_append(&payload, "keep", 4u));
    for (unsigned int variant = 0; variant < 8u; ++variant) {
        event = turn_start_event();
        if (variant == 0u) event.data.started.number = 0;
        if (variant == 1u) {
            event.data.started.origin = (enum snag_binary_turn_origin)-1;
        }
        if (variant == 2u) event.data.started.read_only = true;
        if (variant == 3u) event.data.started.config.present = 0x8000u;
        if (variant == 4u || variant == 5u) {
            event.data.started.origin = SNAG_BINARY_TURN_QUEUED;
            event.data.started.queue_seq = variant == 4u ? 0u : UINT64_MAX;
        }
        if (variant == 6u) event.data.started.cwd = text("");
        if (variant == 7u) event.data.started.text = text("");
        assert(snag_binary_event_encode(&payload, &event) < 0);
        assert(payload.len == 4u && !memcmp(payload.data, "keep", 4u));
    }
    event = turn_start_event();
    event.data.started.origin = SNAG_BINARY_TURN_QUEUED;
    event.data.started.queue_seq = 17u;
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    record.payload = payload.data;
    record.size = payload.len;
    assert(!snag_binary_event_decode(&record, &decoded));
    assert(decoded.data.started.queue_seq == 17u);
    memset(payload.data + 42u, 0, 8u);
    assert_rejected(record);
    memset(payload.data + 42u, 0xff, 8u);
    assert_rejected(record);
    event.data.started.has_received_ms = true;
    event.data.started.received_ms = 0u;
    roundtrip(&event);
    snag_buf_free(&payload);
}

static void
test_turn_input_references(void)
{
    struct snag_buf instructions = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf parts = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf source = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf bytes = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_event event = turn_start_event();
    struct snag_binary_part part = {.kind = SNAG_BINARY_PART_TEXT, .text = text("attachment")};
    assert(!snag_binary_content_encode(&parts, &part, 1u));
    struct snag_binary_event receipt = {.kind = SNAG_BINARY_INPUT_RECEIVED};
    receipt.data.input.selection = event.data.started.config.selection;
    receipt.data.input.instructions = event.data.started.instructions;
    receipt.data.input.text = text("original");
    receipt.data.input.content = (struct snag_binary_content){parts.data, parts.len};
    assert(!snag_binary_event_encode(&source, &receipt));
    struct snag_binary_record original = {
        .kind = receipt.kind, .version = snag_binary_event_version(receipt.kind),
        .payload = source.data, .size = source.len
    };
    struct snag_binary_batch batch;
    input_reference_batch(&original, &bytes, &batch);
    event.data.started.text = (struct snag_binary_text){0};
    event.data.started.text_ref.field = SNAG_BINARY_INPUT_TEXT;
    event.data.started.content_ref.field = SNAG_BINARY_INPUT_CONTENT;
    assert(!snag_binary_input_ref_create(&batch, 2u, SNAG_BINARY_INPUT_TEXT,
        &event.data.started.text_ref.target));
    assert(!snag_binary_input_ref_create(&batch, 2u, SNAG_BINARY_INPUT_CONTENT,
        &event.data.started.content_ref.target));
    /* Discovered turn instructions are not the receipt's empty explicit list. */
    struct snag_binary_instruction instruction = {.path = text("/w/AGENTS.md")};
    assert(!snag_binary_instructions_encode(&instructions, &instruction, 1u));
    event.data.started.instructions = (struct snag_binary_instructions){
        instructions.data, instructions.len
    };
    assert(!snag_binary_event_encode(&payload, &event));
    roundtrip(&event);
    struct snag_binary_record records[] = {
        {.kind = SNAG_BINARY_INPUT_CANCELLED, .version = 1u}, original,
        {.kind = event.kind, .version = 1u, .payload = payload.data, .size = payload.len}
    };
    struct snag_binary_identity identity = {.id = {1}, .created_ms = 1u};
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    snag_binary_header_encode(header, &identity);
    struct snag_binary_anchor anchor, next;
    assert(!snag_binary_header_decode(header, sizeof(header), &identity, &anchor));
    snag_buf_reset(&bytes);
    assert(!snag_binary_batch_encode(&bytes, &anchor, records, 3u, 1u, NULL));
    assert(!snag_binary_batch_decode(bytes.data, bytes.len, &anchor, &batch, &next));
    struct snag_binary_event decoded;
    assert(!snag_binary_event_decode(&records[2], &decoded));
    const unsigned char *view;
    assert(!snag_binary_input_ref_resolve(&decoded.data.started.text_ref.target, &batch,
        decoded.data.started.text_ref.field, &view));
    assert(!memcmp(view, "original", 8u));
    assert(!snag_binary_input_ref_resolve(&decoded.data.started.content_ref.target, &batch,
        decoded.data.started.content_ref.field, &view));
    assert(!memcmp(view, parts.data, parts.len));
    assert_input_leaf_missing(&batch, 3u, SNAG_BINARY_INPUT_TEXT);
    assert_input_leaf_missing(&batch, 3u, SNAG_BINARY_INPUT_CONTENT);
    struct snag_binary_ref reference;
    assert(!snag_binary_input_ref_create(&batch, 3u, SNAG_BINARY_INPUT_INSTRUCTIONS, &reference));
    assert(!snag_binary_input_ref_resolve(&reference, &batch,
        SNAG_BINARY_INPUT_INSTRUCTIONS, &view));
    assert(reference.size == instructions.len &&
        !memcmp(view, instructions.data, instructions.len));
    unsigned int originals = 0u;
    unsigned int attachments = 0u;
    for (size_t i = 0; i < 3u; ++i) {
        for (size_t j = 0; j + 8u <= records[i].size; ++j) {
            if (!memcmp(records[i].payload + j, "original", 8u)) ++originals;
            if (j + 10u <= records[i].size &&
                !memcmp(records[i].payload + j, "attachment", 10u)) {
                ++attachments;
            }
        }
    }
    assert(originals == 1u && attachments == 1u);

    /* A receipt-less legacy turn supplies its own original leaves and snapshot. */
    event = turn_start_event();
    event.data.started.workspace = true;
    event.data.started.text = text("original");
    event.data.started.content = receipt.data.input.content;
    instruction.has_snapshot = true;
    instruction.bytes = 16298u;
    memset(instruction.sha256, 0xa5, sizeof(instruction.sha256));
    snag_buf_reset(&instructions);
    assert(!snag_binary_instructions_encode(&instructions, &instruction, 1u));
    event.data.started.instructions = (struct snag_binary_instructions){
        instructions.data, instructions.len
    };
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    roundtrip(&event);
    original = (struct snag_binary_record){
        .kind = event.kind, .version = 1u, .payload = payload.data, .size = payload.len
    };
    input_reference_batch(&original, &bytes, &batch);
    struct snag_binary_input_reference references[3];
    for (unsigned int i = 0; i < 3u; ++i) {
        references[i].field = (enum snag_binary_input_leaf)(SNAG_BINARY_INPUT_TEXT + i);
        assert(!snag_binary_input_ref_create(&batch, 2u, references[i].field,
            &references[i].target));
        assert(!snag_binary_input_ref_resolve(&references[i].target, &batch,
            references[i].field, &view));
    }
    struct snag_binary_instructions snapshot;
    assert(!snag_binary_instructions_decode(view, references[2].target.size, &snapshot));
    struct snag_binary_instruction decoded_instruction;
    size_t cursor = 0;
    assert(!snag_binary_instructions_next(&snapshot, &cursor, &decoded_instruction));
    assert(decoded_instruction.has_snapshot && decoded_instruction.bytes == instruction.bytes);
    assert(!memcmp(decoded_instruction.sha256, instruction.sha256, sizeof(instruction.sha256)));
    event.data.started.number = 2u;
    event.data.started.text = (struct snag_binary_text){0};
    event.data.started.content = (struct snag_binary_content){0};
    event.data.started.instructions = (struct snag_binary_instructions){0};
    event.data.started.text_ref = references[0];
    event.data.started.content_ref = references[1];
    event.data.started.instructions_ref = references[2];
    roundtrip(&event);
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    original.payload = payload.data;
    original.size = payload.len;
    input_reference_batch(&original, &bytes, &batch);
    for (unsigned int i = 0; i < 3u; ++i) {
        assert_input_leaf_missing(&batch, 2u, references[i].field);
    }
    size_t saved = payload.len;
    event.data.started.text = text("ambiguous");
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    snag_buf_free(&instructions);
    snag_buf_free(&parts);
    snag_buf_free(&source);
    snag_buf_free(&payload);
    snag_buf_free(&bytes);
}

static void
test_turn_outcomes(void)
{
    static const unsigned char ids[] =
        "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f"
        "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f"
        "\x20\x21\x22\x23\x24\x25\x26\x27\x28\x29\x2a\x2b\x2c\x2d\x2e\x2f";
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_binary_event event = {.kind = SNAG_BINARY_TURN_YIELD_REQUESTED}, decoded;
    assert(SNAG_BINARY_TURN_YIELD_REQUESTED == 129 && SNAG_BINARY_TURN_CANCEL_REQUESTED == 130 &&
        SNAG_BINARY_TURN_RECOVERY == 131 && SNAG_BINARY_TURN_COMPLETED == 132 &&
        SNAG_BINARY_TURN_COMPLETED_SILENT == 133 && SNAG_BINARY_TURN_INTERRUPTED == 134 &&
        SNAG_BINARY_TURN_FAILED == 135);
    memcpy(event.data.turn, ids, 16u);
    for (int kind = SNAG_BINARY_TURN_YIELD_REQUESTED;
         kind <= SNAG_BINARY_TURN_CANCEL_REQUESTED; ++kind) {
        event.kind = (enum snag_binary_kind)kind;
        snag_buf_reset(&payload);
        assert(!snag_binary_event_encode(&payload, &event));
        assert(payload.len == 16u && !memcmp(payload.data, ids, 16u));
        assert(snag_binary_event_version(event.kind) == 1u);
        roundtrip(&event);
    }
    event = (struct snag_binary_event){.kind = SNAG_BINARY_TURN_COMPLETED};
    memcpy(event.data.completed.turn, ids, 16u);
    memcpy(event.data.completed.response, ids + 16u, 16u);
    memcpy(event.data.completed.item, ids + 32u, 16u);
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    assert(payload.len == 48u && !memcmp(payload.data, ids, 48u));
    roundtrip(&event);

    event = (struct snag_binary_event){.kind = SNAG_BINARY_TURN_COMPLETED_SILENT};
    memcpy(event.data.silent.turn, ids, 16u);
    memcpy(event.data.silent.response, ids + 16u, 16u);
    for (int reason = SNAG_BINARY_QUIET_ROOM_UPDATE;
         reason <= SNAG_BINARY_QUIET_REPLY_EXHAUSTED; ++reason) {
        event.data.silent.reason = (enum snag_binary_quiet_reason)reason;
        snag_buf_reset(&payload);
        assert(!snag_binary_event_encode(&payload, &event));
        assert(payload.len == 33u && !memcmp(payload.data, ids, 32u));
        assert(payload.data[32] == (unsigned char)reason);
        roundtrip(&event);
    }
    struct snag_binary_record record = {
        .kind = (uint16_t)event.kind, .version = 1u, .payload = payload.data, .size = payload.len
    };
    payload.data[32] = 0;
    assert_rejected(record);
    payload.data[32] = 3;
    assert_rejected(record);
    event.data.silent.reason = 0;
    size_t saved = payload.len;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event.data.silent.reason = (enum snag_binary_quiet_reason)3;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);

    event = (struct snag_binary_event){.kind = SNAG_BINARY_TURN_INTERRUPTED};
    memcpy(event.data.interrupted.turn, ids, 16u);
    for (int origin = SNAG_BINARY_INTERRUPT_USER;
         origin <= SNAG_BINARY_INTERRUPT_OUTPUT; ++origin) {
        event.data.interrupted.origin = (enum snag_binary_interrupt_origin)origin;
        for (int reason = SNAG_BINARY_INTERRUPT_CANCELLED;
             reason <= SNAG_BINARY_INTERRUPT_SESSION_RECOVERED; ++reason) {
            event.data.interrupted.reason = (enum snag_binary_interrupt_reason)reason;
            snag_buf_reset(&payload);
            assert(!snag_binary_event_encode(&payload, &event));
            assert(payload.len == 18u && !memcmp(payload.data, ids, 16u));
            assert(payload.data[16] == (unsigned char)origin &&
                payload.data[17] == (unsigned char)reason);
            roundtrip(&event);
        }
    }
    record = (struct snag_binary_record){
        .kind = (uint16_t)event.kind, .version = 1u, .payload = payload.data, .size = payload.len
    };
    for (size_t field = 16u; field <= 17u; ++field) {
        unsigned char value = payload.data[field];
        payload.data[field] = 0;
        assert_rejected(record);
        payload.data[field] = field == 16u ? 4u : 5u;
        assert_rejected(record);
        payload.data[field] = value;
    }
    saved = payload.len;
    event.data.interrupted.origin = (enum snag_binary_interrupt_origin)4;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event.data.interrupted.origin = SNAG_BINARY_INTERRUPT_USER;
    event.data.interrupted.reason = (enum snag_binary_interrupt_reason)5;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);

    event = (struct snag_binary_event){.kind = SNAG_BINARY_TURN_FAILED};
    memcpy(event.data.failed.turn, ids, 16u);
    event.data.failed.message = text("bad");
    for (int class_id = SNAG_BINARY_FAILURE_CONTEXT;
         class_id <= SNAG_BINARY_FAILURE_INTERNAL; ++class_id) {
        event.data.failed.class_id = (enum snag_binary_failure_class)class_id;
        snag_buf_reset(&payload);
        assert(!snag_binary_event_encode(&payload, &event));
        assert(payload.len == 24u && !memcmp(payload.data, ids, 16u));
        assert(payload.data[16] == (unsigned char)class_id &&
            !memcmp(payload.data + 17u, "\x03\0\0\0bad", 7u));
        roundtrip(&event);
    }
    record = (struct snag_binary_record){
        .kind = (uint16_t)event.kind, .version = 1u, .payload = payload.data, .size = payload.len
    };
    payload.data[16] = 0;
    assert_rejected(record);
    payload.data[16] = 9;
    assert_rejected(record);
    payload.data[16] = SNAG_BINARY_FAILURE_INTERNAL;
    payload.data[21] = 0;
    assert_rejected(record);
    payload.data[21] = 0xff;
    assert_rejected(record);
    saved = payload.len;
    event.data.failed.class_id = 0;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event.data.failed.class_id = (enum snag_binary_failure_class)9;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event.data.failed.class_id = SNAG_BINARY_FAILURE_OUTPUT;
    event.data.failed.message = text("");
    roundtrip(&event);
    event.data.failed.message = text("diagnostic caf\xc3\xa9");
    roundtrip(&event);
    unsigned char message[8193];
    memset(message, 'x', sizeof(message));
    event.data.failed.message = (struct snag_binary_text){message, 8192u};
    roundtrip(&event);
    event.data.failed.message.size = sizeof(message);
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event.data.failed.message = (struct snag_binary_text){(const unsigned char *)"a\0b", 3u};
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event.data.failed.message = text("\xc0\x80");
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    /* A complete oversized diagnostic must fail, not only a truncated field. */
    snag_buf_reset(&payload);
    assert(!snag_buf_append(&payload, ids, 16u));
    assert(!snag_buf_append(&payload, "\x01\x01\x20\0\0", 5u));
    assert(!snag_buf_append(&payload, message, sizeof(message)));
    record.payload = payload.data;
    record.size = payload.len;
    assert_rejected(record);

    event = (struct snag_binary_event){.kind = SNAG_BINARY_TURN_RECOVERY};
    memcpy(event.data.recovery.turn, ids, 16u);
    event.data.recovery.class_name = text("old");
    event.data.recovery.message = text("bad");
    event.data.recovery.has_retry_attempts = true;
    event.data.recovery.retry_attempts = (uint64_t)UINT32_MAX + 1u;
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    static const unsigned char recovery_tail[] =
        "\x01\0\0\0\0\x01\0\0\0\x03\0\0\0old\x03\0\0\0bad";
    assert(payload.len == 16u + sizeof(recovery_tail) - 1u &&
        !memcmp(payload.data, ids, 16u) &&
        !memcmp(payload.data + 16u, recovery_tail, sizeof(recovery_tail) - 1u));
    roundtrip(&event);
    record = (struct snag_binary_record){
        .kind = (uint16_t)event.kind, .version = 1u, .payload = payload.data, .size = payload.len
    };
    assert(!snag_binary_event_decode(&record, &decoded));
    assert(decoded.data.recovery.has_retry_attempts &&
        decoded.data.recovery.retry_attempts == (uint64_t)UINT32_MAX + 1u);
    payload.data[16] = 2;
    assert_rejected(record);
    payload.data[16] = 1;
    payload.data[17] = 1; /* One beyond the valid 33-bit maximum. */
    assert_rejected(record);
    payload.data[17] = 0;
    payload.data[29] = 0; /* Embedded NUL in the open-vocabulary class. */
    assert_rejected(record);
    payload.data[29] = 0xff;
    assert_rejected(record);
    saved = payload.len;
    ++event.data.recovery.retry_attempts;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event.data.recovery.retry_attempts = 0u;
    event.data.recovery.class_name = text("");
    event.data.recovery.message = text("");
    for (unsigned int present = 0u; present <= 1u; ++present) {
        event.data.recovery.has_retry_attempts = present != 0u;
        snag_buf_reset(&payload);
        assert(!snag_binary_event_encode(&payload, &event));
        assert(payload.len == 25u + 8u * present);
        record.payload = payload.data;
        record.size = payload.len;
        assert(!snag_binary_event_decode(&record, &decoded));
        assert(decoded.data.recovery.has_retry_attempts == (present != 0u) &&
            !decoded.data.recovery.retry_attempts && !decoded.data.recovery.class_name.size &&
            !decoded.data.recovery.message.size);
        roundtrip(&event);
    }
    event.data.recovery.class_name = text("legacy caf\xc3\xa9");
    event.data.recovery.message = (struct snag_binary_text){message, 8192u};
    roundtrip(&event);
    saved = payload.len;
    event.data.recovery.message.size = sizeof(message);
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event.data.recovery.message = text("");
    event.data.recovery.class_name = (struct snag_binary_text){message, sizeof(message)};
    roundtrip(&event); /* Recovery classes do not share the diagnostic-size bound. */
    event.data.recovery.class_name = (struct snag_binary_text){(const unsigned char *)"a\0b", 3u};
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    snag_buf_reset(&payload);
    assert(!snag_buf_append(&payload, ids, 16u));
    /* Absent retry count, empty class, complete 8193-byte message. */
    assert(!snag_buf_append(&payload, "\0\0\0\0\0\x01\x20\0\0", 9u));
    assert(!snag_buf_append(&payload, message, sizeof(message)));
    record.payload = payload.data;
    record.size = payload.len;
    assert_rejected(record);
    snag_buf_free(&payload);
}

static void
test_response_starts(void)
{
    struct snag_binary_event event = {.kind = SNAG_BINARY_RESPONSE_STARTED}, decoded;
    struct snag_binary_response_start *start = &event.data.response_started;
    for (size_t i = 0u; i < 16u; ++i) {
        start->turn[i] = (unsigned char)i;
        start->response[i] = (unsigned char)(i + 16u);
    }
    memset(start->request_sha256, 0x11, 32u);
    memset(start->count_request_sha256, 0x22, 32u);
    memset(start->model_input_sha256, 0x33, 32u);
    start->cycle = UINT32_C(0x01020304);
    start->count_method = SNAG_BINARY_COUNT_UNKNOWN;
    start->selection.model = text("m");
    start->capability = text("cap");
    start->profile = text("p");
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    static const unsigned char legacy_bytes[] =
        "\0\0\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f"
        "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f"
        "\x04\x03\x02\x01\x03\0\0\0\0\0\0\0\0"
        "\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11"
        "\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11"
        "\x22\x22\x22\x22\x22\x22\x22\x22\x22\x22\x22\x22\x22\x22\x22\x22"
        "\x22\x22\x22\x22\x22\x22\x22\x22\x22\x22\x22\x22\x22\x22\x22\x22"
        "\x33\x33\x33\x33\x33\x33\x33\x33\x33\x33\x33\x33\x33\x33\x33\x33"
        "\x33\x33\x33\x33\x33\x33\x33\x33\x33\x33\x33\x33\x33\x33\x33\x33"
        "\x01\0\0\0m\x03\0\0\0cap\x01\0\0\0p\0\0\0\0";
    assert(SNAG_BINARY_RESPONSE_STARTED == 160 && snag_binary_event_version(event.kind) == 1u);
    assert(payload.len == sizeof(legacy_bytes) - 1u &&
        !memcmp(payload.data, legacy_bytes, payload.len));
    roundtrip(&event);
    struct snag_binary_record record = {
        .kind = (uint16_t)event.kind, .version = 1u, .payload = payload.data, .size = payload.len
    };
    assert(!snag_binary_event_decode(&record, &decoded));
    const struct snag_binary_response_start *result = &decoded.data.response_started;
    assert(!result->full_accounting && !result->has_irc_seq && !result->host_context.size &&
        !result->selection.provider.size && !result->selection.effort.size &&
        !result->model_input_bytes && !result->request_input_bytes && !result->request_input_count);
    assert(result->cycle == start->cycle && result->count_method == SNAG_BINARY_COUNT_UNKNOWN);
    payload.data[1] = 1; /* Reserved mask bit. */
    assert_rejected(record);
    payload.data[1] = 0;
    const unsigned char full_only[] = {4u, 32u, 64u, 128u};
    for (size_t i = 0u; i < sizeof(full_only); ++i) {
        payload.data[0] = full_only[i];
        assert_rejected(record);
    }
    payload.data[0] = 0;
    payload.data[38] = 0;
    assert_rejected(record);
    payload.data[38] = 7;
    assert_rejected(record);
    payload.data[38] = SNAG_BINARY_COUNT_UNKNOWN;
    payload.data[39] = 1; /* Unknown is not a nonzero measurement. */
    assert_rejected(record);
    payload.data[39] = 0;
    memset(payload.data + 34u, 0, 4u);
    assert_rejected(record);

    start->has_irc_seq = true;
    for (unsigned int nonzero = 0u; nonzero <= 1u; ++nonzero) {
        start->irc_seq = nonzero ? UINT64_MAX : 0u;
        snag_buf_reset(&payload);
        assert(!snag_binary_event_encode(&payload, &event));
        record.payload = payload.data;
        record.size = payload.len;
        assert(!snag_binary_event_decode(&record, &decoded));
        assert(!result->full_accounting && result->has_irc_seq &&
            result->irc_seq == start->irc_seq);
        roundtrip(&event);
    }
    for (int method = SNAG_BINARY_COUNT_EXACT;
         method <= SNAG_BINARY_COUNT_QUALIFIED_UPPER_BOUND; ++method) {
        start->count_method = (enum snag_binary_count_method)method;
        start->has_baseline = method == SNAG_BINARY_COUNT_ANCHORED_UPPER_BOUND;
        start->input_tokens_bound = method == SNAG_BINARY_COUNT_UNKNOWN ? 0u : UINT64_MAX;
        roundtrip(&event);
    }

    struct snag_binary_part parts[] = {
        {.kind = SNAG_BINARY_PART_TEXT, .text = text("begin")},
        {.kind = SNAG_BINARY_PART_TEXT, .text = text("host caf\xc3\xa9")},
        {.kind = SNAG_BINARY_PART_TEXT, .text = text("end")}
    };
    struct snag_buf context = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_content_encode(&context, parts, 3u));
    start->host_context = (struct snag_binary_content){context.data, context.len};
    start->full_accounting = true;
    start->has_baseline = start->has_compact = start->source_bound = true;
    start->has_hard_input = start->has_requested_output = true;
    start->count_method = SNAG_BINARY_COUNT_ANCHORED_UPPER_BOUND;
    start->capacity_source = SNAG_BINARY_CAPACITY_STALE_CATALOG_IGNORED;
    start->selection.provider = text("p");
    start->selection.effort = text("e");
    start->input_tokens_bound = UINT64_C(0x100000001);
    memset(start->baseline_sha256, 0x44, 32u);
    memset(start->compact, 0x55, 16u);
    memset(start->provider_source_sha256, 0x66, 32u);
    memset(start->request_input_sha256, 0x77, 32u);
    static const unsigned char steering[][16] = {{1u}, {2u}};
    start->steering = (struct snag_binary_ids){steering, 2u};
    start->model_input_bytes = UINT64_C(0x0807060504030201);
    start->request_input_bytes = UINT64_C(0x100000008);
    start->request_input_count = UINT64_C(0x0203040506070809);
    start->hard_input_tokens = 0u; /* This field, unlike output limits, permits zero. */
    start->requested_output_tokens = SNAG_CONFIG_TOKEN_LIMIT_MAX;
    start->irc_seq = UINT64_C(0x0807060504030201);
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    assert(payload.data[0] == 255u && !payload.data[1]);
    static const unsigned char numbers[] =
        "\x01\x02\x03\x04\x05\x06\x07\x08"
        "\x08\0\0\0\x01\0\0\0"
        "\x09\x08\x07\x06\x05\x04\x03\x02"
        "\0\0\0\0\0\0\0\0"
        "\x00\x28\x6b\xee\0\0\0\0"
        "\x01\x02\x03\x04\x05\x06\x07\x08";
    size_t context_offset = payload.len - context.len;
    size_t numbers_offset = context_offset - (sizeof(numbers) - 1u);
    assert(!memcmp(payload.data + numbers_offset, numbers, sizeof(numbers) - 1u));
    assert(!memcmp(payload.data + context_offset, context.data, context.len));
    record.payload = payload.data;
    record.size = payload.len;
    assert(!snag_binary_event_decode(&record, &decoded));
    assert(result->full_accounting && result->has_baseline && result->has_compact &&
        result->source_bound && result->has_hard_input && result->has_requested_output);
    assert(result->capacity_source == SNAG_BINARY_CAPACITY_STALE_CATALOG_IGNORED &&
        result->input_tokens_bound == start->input_tokens_bound &&
        result->model_input_bytes == start->model_input_bytes &&
        result->request_input_bytes == start->request_input_bytes &&
        result->request_input_count == start->request_input_count &&
        !result->hard_input_tokens &&
        result->requested_output_tokens == SNAG_CONFIG_TOKEN_LIMIT_MAX);
    assert(result->selection.provider.size == 1u && *result->selection.provider.data == 'p' &&
        result->selection.effort.size == 1u && *result->selection.effort.data == 'e');
    assert(result->steering.count == 2u && !memcmp(result->steering.values, steering, 32u));
    assert(result->host_context.size == context.len &&
        !memcmp(result->host_context.data, context.data, context.len));
    assert(!memcmp(result->baseline_sha256, start->baseline_sha256, 32u) &&
        !memcmp(result->compact, start->compact, 16u) &&
        !memcmp(result->provider_source_sha256, start->provider_source_sha256, 32u) &&
        !memcmp(result->request_input_sha256, start->request_input_sha256, 32u));
    roundtrip(&event);
    struct snag_buf full = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_buf_append(&full, payload.data, payload.len));
    struct snag_binary_event complete = event;

    payload.data[38] = SNAG_BINARY_COUNT_UNKNOWN;
    assert_rejected(record);
    payload.data[38] = SNAG_BINARY_COUNT_ANCHORED_UPPER_BOUND;
    payload.data[numbers_offset - 65u] = 0;
    assert_rejected(record);
    payload.data[numbers_offset - 65u] = 6;
    assert_rejected(record);
    payload.data[numbers_offset - 65u] = SNAG_BINARY_CAPACITY_STALE_CATALOG_IGNORED;
    memset(payload.data + numbers_offset, 0, 8u);
    assert_rejected(record);
    memcpy(payload.data + numbers_offset, numbers, 8u);
    memset(payload.data + numbers_offset + 8u, 0, 8u);
    assert_rejected(record);
    memcpy(payload.data + numbers_offset + 8u, numbers + 8u, 8u);
    memset(payload.data + numbers_offset + 32u, 0, 8u);
    assert_rejected(record);
    memcpy(payload.data + numbers_offset + 32u, numbers + 32u, 8u);
    payload.data[numbers_offset + 36u] = 1; /* Beyond the configured output-limit domain. */
    assert_rejected(record);
    payload.data[numbers_offset + 36u] = 0;
    payload.data[context_offset] = 1;
    record.size = context_offset + 14u; /* A complete single text part. */
    assert_rejected(record);
    record.size = payload.len;
    payload.data[context_offset] = 3;
    payload.data[context_offset + 9u] = 0;
    assert_rejected(record);
    payload.data[context_offset + 9u] = 0xff;
    assert_rejected(record);

    for (int source = SNAG_BINARY_CAPACITY_UNKNOWN;
         source <= SNAG_BINARY_CAPACITY_STALE_CATALOG_IGNORED; ++source) {
        start->capacity_source = (enum snag_binary_capacity_source)source;
        roundtrip(&event);
    }
    for (int method = SNAG_BINARY_COUNT_EXACT;
         method <= SNAG_BINARY_COUNT_QUALIFIED_UPPER_BOUND; ++method) {
        start->count_method = (enum snag_binary_count_method)method;
        start->has_baseline = method == SNAG_BINARY_COUNT_ANCHORED_UPPER_BOUND;
        start->input_tokens_bound = method == SNAG_BINARY_COUNT_UNKNOWN ? 0u : 1u;
        roundtrip(&event);
    }
    start->has_irc_seq = start->has_compact = start->has_hard_input = false;
    start->has_requested_output = start->source_bound = false;
    start->host_context = (struct snag_binary_content){0};
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    record.payload = payload.data;
    record.size = payload.len;
    assert(payload.data[0] == 1u);
    assert(!snag_binary_event_decode(&record, &decoded));
    assert(result->full_accounting && !result->has_irc_seq && !result->has_baseline &&
        !result->has_compact && !result->has_hard_input && !result->has_requested_output &&
        !result->host_context.size && !result->source_bound);
    roundtrip(&event);

    size_t saved = payload.len;
    event = complete;
    start->full_accounting = false;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event = complete;
    start->cycle = 0u;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event = complete;
    start->count_method = (enum snag_binary_count_method)7;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event = complete;
    start->has_baseline = false;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event = complete;
    start->count_method = SNAG_BINARY_COUNT_EXACT;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event = complete;
    start->capacity_source = (enum snag_binary_capacity_source)6;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event = complete;
    start->model_input_bytes = 0u;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event = complete;
    start->request_input_bytes = 0u;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    event = complete;
    start->requested_output_tokens = 0u;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    start->requested_output_tokens = SNAG_CONFIG_TOKEN_LIMIT_MAX + 1u;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);

    for (unsigned int mode = 0u; mode < 3u; ++mode) {
        event = complete;
        snag_buf_reset(&context);
        if (mode == 1u) parts[1].text = text("");
        if (mode == 2u) {
            parts[1].kind = SNAG_BINARY_PART_FILE;
            parts[1].asset.bytes = 1u;
            parts[1].asset.mime = text("application/octet-stream");
        }
        assert(!snag_binary_content_encode(&context, parts, mode ? 3u : 1u));
        start->host_context = (struct snag_binary_content){context.data, context.len};
        saved = payload.len;
        assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
        /* Construct otherwise-complete wire bytes with the invalid host list. */
        snag_buf_reset(&payload);
        assert(!snag_buf_append(&payload, full.data, context_offset));
        assert(!snag_buf_append(&payload, context.data, context.len));
        record.payload = payload.data;
        record.size = payload.len;
        assert_rejected(record);
    }
    snag_buf_free(&context);
    snag_buf_free(&full);
    snag_buf_free(&payload);
}

static void
test_output_span_codec(void)
{
    struct snag_binary_output_span span = {
        .first = {UINT64_C(0x0807060504030201), 82u, 1u},
        .last_sequence = UINT64_C(0x0807060504030204), .bytes = 4u
    };
    unsigned char bytes[SNAG_BINARY_OUTPUT_SPAN_SIZE + 1u] = {0};
    assert(!snag_binary_output_span_encode(bytes, &span));
    static const unsigned char expected[] =
        "\x01\x02\x03\x04\x05\x06\x07\x08\x52\0\0\0\x01\0\0\0"
        "\x04\x02\x03\x04\x05\x06\x07\x08\x04\0\0\0";
    assert(sizeof(expected) - 1u == SNAG_BINARY_OUTPUT_SPAN_SIZE);
    assert(!memcmp(bytes, expected, sizeof(expected) - 1u));
    struct snag_binary_output_span decoded;
    assert(!snag_binary_output_span_decode(bytes, SNAG_BINARY_OUTPUT_SPAN_SIZE, &decoded));
    assert(decoded.first.sequence == span.first.sequence && decoded.first.offset == 82u &&
        decoded.first.size == 1u && decoded.last_sequence == span.last_sequence &&
        decoded.bytes == 4u);
    unsigned char saved[sizeof(decoded)];
    memset(&decoded, 0x5a, sizeof(decoded));
    memcpy(saved, &decoded, sizeof(decoded));
    for (size_t i = 0u; i < sizeof(bytes); ++i) {
        if (i == SNAG_BINARY_OUTPUT_SPAN_SIZE) continue;
        assert(snag_binary_output_span_decode(bytes, i, &decoded) < 0);
        assert(!memcmp(saved, &decoded, sizeof(decoded)));
    }
    assert(snag_binary_output_span_decode(bytes, sizeof(bytes), &decoded) < 0);
    static const struct { size_t offset; unsigned char value; } mutations[] = {
        {12u, 0u}, {12u, 5u}, {16u, 0u}, {16u, 1u}, {24u, 0u}, {24u, 1u},
        {26u, 0x21u}, {11u, 0xffu}
    };
    for (size_t i = 0u; i < sizeof(mutations) / sizeof(mutations[0]); ++i) {
        unsigned char old = bytes[mutations[i].offset];
        bytes[mutations[i].offset] = mutations[i].value;
        assert(snag_binary_output_span_decode(bytes, SNAG_BINARY_OUTPUT_SPAN_SIZE, &decoded) < 0);
        assert(!memcmp(saved, &decoded, sizeof(decoded)));
        bytes[mutations[i].offset] = old;
    }
    for (unsigned int i = 0u; i < 11u; ++i) {
        struct snag_binary_output_span bad = span;
        switch (i) {
        case 0: bad.first.sequence = 0u; break;
        case 1: bad.first.sequence = UINT64_MAX; break;
        case 2: bad.first.size = 0u; break;
        case 3: bad.last_sequence = bad.first.sequence - 1u; break;
        case 4: bad.last_sequence = UINT64_MAX; break;
        case 5: bad.bytes = 0u; break;
        case 6: bad.bytes = SNAG_MAX_PUBLIC_ITEM + 1u; break;
        case 7: bad.first.size = 5u; break;
        case 8: bad.last_sequence = bad.first.sequence; break;
        case 9: bad.bytes = 1u; break;
        case 10: bad.first.offset = UINT32_MAX; break;
        }
        assert(snag_binary_output_span_encode(bytes, &bad) < 0);
        assert(!memcmp(bytes, expected, sizeof(expected) - 1u));
    }
    assert(snag_binary_output_span_encode(NULL, &span) < 0);
    assert(snag_binary_output_span_encode(bytes, NULL) < 0);
    assert(snag_binary_output_span_decode(NULL, SNAG_BINARY_OUTPUT_SPAN_SIZE, &decoded) < 0);
    assert(snag_binary_output_span_decode(bytes, SNAG_BINARY_OUTPUT_SPAN_SIZE, NULL) < 0);
    span.last_sequence = UINT64_MAX - 1u;
    span.bytes = SNAG_MAX_PUBLIC_ITEM;
    assert(!snag_binary_output_span_encode(bytes, &span));
    assert(!snag_binary_output_span_decode(bytes, SNAG_BINARY_OUTPUT_SPAN_SIZE, &decoded));
    assert(decoded.bytes == SNAG_MAX_PUBLIC_ITEM && decoded.last_sequence == UINT64_MAX - 1u);
    span.last_sequence = span.first.sequence;
    span.bytes = span.first.size;
    assert(!snag_binary_output_span_encode(bytes, &span));
    assert(!snag_binary_output_span_decode(bytes, SNAG_BINARY_OUTPUT_SPAN_SIZE, &decoded));
}

static uint64_t
output_span_fixture(int fd, const struct snag_binary_event events[6], unsigned int variant,
    struct snag_binary_anchor *anchor, struct snag_binary_anchor *middle,
    struct snag_binary_output_span *span)
{
    struct snag_buf payloads[6] = {0};
    struct snag_binary_record records[6];
    for (size_t i = 0u; i < 6u; ++i) {
        payloads[i].max = SNAG_MAX_EVENT_LINE;
        assert(!snag_binary_event_encode(&payloads[i], &events[i]));
        records[i] = (struct snag_binary_record){.kind = (uint16_t)events[i].kind, .version = 1u,
            .payload = payloads[i].data, .size = payloads[i].len};
    }
    if (variant == 11u) records[3].version = 2u;
    if (variant == 12u) records[3].flags = SNAG_BINARY_RECORD_OPTIONAL;
    if (variant == 13u) payloads[3].data[payloads[3].len - 2u] = 0xffu;
    if (variant == 14u) payloads[3].data[52u] = SNAG_BINARY_ITEM_TOOL_CALL;
    struct snag_binary_identity identity = {.id = {1}, .created_ms = 1u};
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    snag_binary_header_encode(header, &identity);
    assert(!snag_binary_header_decode(header, sizeof(header), &identity, anchor));
    struct snag_buf bytes = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_batch batch;
    struct snag_binary_anchor next;
    assert(!snag_binary_batch_encode(&bytes, anchor, records, 3u, 0u, NULL));
    assert(!snag_binary_batch_decode(bytes.data, bytes.len, anchor, &batch, middle));
    assert(!snag_binary_output_ref_create(&batch, 2u, &span->first));
    span->last_sequence = 5u;
    span->bytes = 4u;
    assert(!snag_truncate(fd, 0));
    assert(snag_seek(fd, 0, SEEK_SET) == 0);
    assert(!snag_write_full(fd, header, sizeof(header)));
    assert(!snag_write_full(fd, bytes.data, bytes.len));
    snag_buf_reset(&bytes);
    assert(!snag_binary_batch_encode(&bytes, middle, records + 3u, 3u, 0u, NULL));
    assert(!snag_binary_batch_decode(bytes.data, bytes.len, middle, &batch, &next));
    assert(!snag_write_full(fd, bytes.data, bytes.len));
    snag_buf_free(&bytes);
    for (size_t i = 0u; i < 6u; ++i) snag_buf_free(&payloads[i]);
    return next.end;
}

static void
assert_output_span_unresolved(int fd, uint64_t boundary, const struct snag_binary_anchor *anchor,
    const struct snag_binary_output_span *span, const struct snag_binary_response_output *expected)
{
    struct snag_buf out = {.max = 64u};
    assert(!snag_buf_append(&out, "keep", 4u));
    assert(snag_seek(fd, 7, SEEK_SET) == 7);
    assert(snag_binary_output_span_resolve(fd, boundary, anchor, span,
        expected->turn, expected->response, expected->cycle, &expected->item, &out) < 0);
    assert(out.len == 4u && !memcmp(out.data, "keep", 4u));
    assert(snag_seek(fd, 0, SEEK_CUR) == 7);
    snag_buf_free(&out);
}

static void
test_public_snapshot_source(int fd, uint64_t boundary, const struct snag_binary_anchor *anchor,
    const struct snag_binary_output_span *span, const struct snag_binary_response_output *source)
{
    struct snag_binary_public_value value = {.item = source->item, .source = *span};
    struct snag_buf items = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_public_items_encode(&items, &value, 1u));
    /* Metadata, span marker and 28B span; no copied text. */
    assert(items.len == 59u);
    struct snag_binary_event event = {.kind = SNAG_BINARY_RESPONSE_INTERRUPTED};
    struct snag_binary_event decoded;
    struct snag_binary_response_interruption *interrupted = &event.data.response_interrupted;
    memcpy(interrupted->turn, source->turn, 16u);
    memcpy(interrupted->response, source->response, 16u);
    interrupted->cycle = source->cycle;
    interrupted->origin = SNAG_BINARY_INTERRUPT_USER;
    interrupted->reason = SNAG_BINARY_INTERRUPT_CANCELLED;
    assert(!snag_binary_public_items_decode(items.data, items.len, &interrupted->partial));
    roundtrip(&event);
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    struct snag_binary_record record = {
        .kind = (uint16_t)event.kind, .version = 1u, .payload = payload.data, .size = payload.len
    };
    assert(!snag_binary_event_decode(&record, &decoded));
    interrupted = &decoded.data.response_interrupted;
    size_t cursor = 0u;
    assert(!snag_binary_public_items_next(&interrupted->partial, &cursor, &value));
    assert(!value.item.text.data && !value.item.text.size);
    struct snag_buf joined = {.max = SNAG_MAX_PUBLIC_ITEM};
    assert(!snag_binary_output_span_resolve(fd, boundary, anchor, &value.source,
        interrupted->turn, interrupted->response, interrupted->cycle, &value.item, &joined));
    assert(joined.len == 4u && !memcmp(joined.data, "A\xc3\xa9Z", 4u));
    assert(snag_binary_public_items_next(&interrupted->partial, &cursor, &value) == 1);
    struct snag_binary_graph_item graph[2] = {0};
    graph[0].kind = SNAG_BINARY_ITEM_TOOL_CALL;
    graph[0].data.call.id[0] = 9u;
    graph[0].data.call.provider_id = text("tool");
    graph[0].data.call.provider_call_id = text("call");
    graph[0].data.call.name = text("cd");
    graph[0].data.call.arguments = text("{}");
    graph[1].kind = source->item.kind;
    graph[1].data.output = (struct snag_binary_public_value){source->item, *span};
    struct snag_buf graph_bytes = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_graph_items_encode(&graph_bytes, graph, 2u));
    event = (struct snag_binary_event){.kind = SNAG_BINARY_RESPONSE_COMPLETED};
    struct snag_binary_response_complete *complete = &event.data.response_completed;
    memcpy(complete->turn, source->turn, 16u);
    memcpy(complete->response, source->response, 16u);
    complete->cycle = source->cycle;
    complete->provider_id = text("r");
    assert(!snag_binary_graph_items_decode(graph_bytes.data, graph_bytes.len, &complete->items));
    roundtrip(&event);
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    record = (struct snag_binary_record){.kind = (uint16_t)event.kind, .version = 1u,
        .payload = payload.data, .size = payload.len};
    assert(!snag_binary_event_decode(&record, &decoded));
    complete = &decoded.data.response_completed;
    cursor = 0u;
    struct snag_binary_graph_item graph_item;
    assert(!snag_binary_graph_items_next(&complete->items, &cursor, &graph_item) &&
        graph_item.kind == SNAG_BINARY_ITEM_TOOL_CALL);
    assert(!snag_binary_graph_items_next(&complete->items, &cursor, &graph_item) &&
        graph_item.kind == source->item.kind);
    snag_buf_reset(&joined);
    assert(!snag_binary_output_span_resolve(fd, boundary, anchor, &graph_item.data.output.source,
        complete->turn, complete->response, complete->cycle,
        &graph_item.data.output.item, &joined));
    assert(joined.len == 4u && !memcmp(joined.data, "A\xc3\xa9Z", 4u));
    snag_buf_free(&graph_bytes);
    snag_buf_free(&joined);
    snag_buf_free(&payload);
    snag_buf_free(&items);
}

static void
test_output_span_file(void)
{
    char *path = snag_path_join(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
        "snajpagent-output-span-XXXXXX");
    assert(path);
    int fd = mkstemp(path);
    assert(fd >= 0);
    struct snag_binary_event events[6] = {0};
    events[0].kind = SNAG_BINARY_TIMER_SCHEDULED;
    events[0].data.timer.due_ms = 1u;
    events[0].data.timer.text = text("gap");
    events[2] = events[0];
    events[1].kind = SNAG_BINARY_RESPONSE_OUTPUT;
    struct snag_binary_response_output *first = &events[1].data.response_output;
    first->turn[0] = 1u;
    first->response[0] = 2u;
    first->cycle = 1u;
    first->index = 7u;
    first->item.id[0] = 3u;
    first->item.kind = SNAG_BINARY_ITEM_ASSISTANT;
    first->item.phase = SNAG_BINARY_PHASE_COMMENTARY;
    first->item.provider_id = text("A");
    first->item.text = text("A");
    events[3] = events[1];
    events[3].data.response_output.offset = 1u;
    events[3].data.response_output.item.text = text("\xc3\xa9");
    events[4] = events[1];
    events[4].data.response_output.offset = 3u;
    events[4].data.response_output.item.text = text("Z");
    events[5] = events[1];
    events[5].data.response_output.index++;
    events[5].data.response_output.item.id[0]++;
    struct snag_binary_response_output expected = *first;
    expected.item.text = (struct snag_binary_text){0};
    struct snag_binary_anchor anchor, middle;
    struct snag_binary_output_span span;
    uint64_t boundary = output_span_fixture(fd, events, 0u, &anchor, &middle, &span);
    test_public_snapshot_source(fd, boundary, &anchor, &span, &expected);
    struct snag_buf out = {.max = 64u};
    assert(!snag_buf_append(&out, "keep", 4u));
    assert(snag_seek(fd, 7, SEEK_SET) == 7);
    assert(!snag_binary_output_span_resolve(fd, boundary, &anchor, &span, expected.turn,
        expected.response, expected.cycle, &expected.item, &out));
    assert(out.len == 8u && !memcmp(out.data, "keepA\xc3\xa9Z", 8u));
    assert(snag_seek(fd, 0, SEEK_CUR) == 7);
    snag_buf_reset(&out);
    struct snag_binary_output_span single = span;
    single.last_sequence = single.first.sequence;
    single.bytes = single.first.size;
    assert(!snag_binary_output_span_resolve(fd, boundary, &anchor, &single, expected.turn,
        expected.response, expected.cycle, &expected.item, &out));
    assert(out.len == 1u && out.data[0] == 'A');
    struct snag_binary_output_span bad = span;
    bad.first.offset -= 5u; /* Same bytes in provider metadata, not original text. */
    assert_output_span_unresolved(fd, boundary, &anchor, &bad, &expected);
    bad = span;
    bad.first.size++;
    assert_output_span_unresolved(fd, boundary, &anchor, &bad, &expected);
    bad = span;
    bad.bytes++;
    assert_output_span_unresolved(fd, boundary, &anchor, &bad, &expected);
    bad = span;
    bad.last_sequence = 3u; /* A non-output endpoint. */
    assert_output_span_unresolved(fd, boundary, &anchor, &bad, &expected);
    bad = span;
    bad.last_sequence = 6u; /* The following item belongs outside the span. */
    bad.bytes++;
    assert_output_span_unresolved(fd, boundary, &anchor, &bad, &expected);
    bad = span;
    bad.first.sequence = 4u;
    bad.first.size = 2u;
    bad.bytes = 3u; /* A suffix cannot masquerade as a complete original item. */
    assert_output_span_unresolved(fd, boundary, &anchor, &bad, &expected);
    assert_output_span_unresolved(fd, boundary, &middle, &span, &expected);
    assert_output_span_unresolved(fd, boundary - 1u, &anchor, &span, &expected);
    assert_output_span_unresolved(fd, middle.end, &anchor, &span, &expected);
    struct snag_binary_response_output wrong = expected;
    wrong.response[0]++;
    assert_output_span_unresolved(fd, boundary, &anchor, &span, &wrong);
    wrong = expected;
    wrong.item.text = text("");
    assert_output_span_unresolved(fd, boundary, &anchor, &span, &wrong);
    for (unsigned int i = 0u; i < 15u; ++i) {
        struct snag_binary_event changed[6];
        memcpy(changed, events, sizeof(changed));
        struct snag_binary_response_output *source = &changed[3].data.response_output;
        switch (i) {
        case 0: source->turn[0]++; break;
        case 1: source->response[0]++; break;
        case 2: source->cycle++; break;
        case 3: source->item.id[0]++; break;
        case 4: source->item.provider_id = text("different"); break;
        case 5: source->item.phase = SNAG_BINARY_PHASE_FINAL_ANSWER; break;
        case 6:
            source->item.kind = SNAG_BINARY_ITEM_REFUSAL;
            source->item.phase = SNAG_BINARY_PHASE_FINAL_ANSWER;
            break;
        case 7: source->index++; break;
        case 8: source->offset++; break;
        case 9: source->offset = 0u; break;
        case 10: changed[1].data.response_output.offset = 1u; break;
        default: break; /* Framed invalid version/flag/payload in the fixture. */
        }
        boundary = output_span_fixture(fd, changed, i, &anchor, &middle, &span);
        assert_output_span_unresolved(fd, boundary, &anchor, &span, &expected);
    }
    boundary = output_span_fixture(fd, events, 0u, &anchor, &middle, &span);
    snag_buf_reset(&out);
    out.max = 4u;
    assert(!snag_buf_append(&out, "keep", 4u));
    assert(snag_binary_output_span_resolve(fd, boundary, &anchor, &span, expected.turn,
        expected.response, expected.cycle, &expected.item, &out) < 0);
    assert(out.len == 4u && !memcmp(out.data, "keep", 4u));
    out.max = 64u;
    assert(snag_binary_output_span_resolve(-1, boundary, &anchor, &span, expected.turn,
        expected.response, expected.cycle, &expected.item, &out) < 0);
    assert(out.len == 4u && !memcmp(out.data, "keep", 4u));
    unsigned char byte;
    assert(snag_seek(fd, (int64_t)boundary - 1, SEEK_SET) == (int64_t)boundary - 1);
    assert(read(fd, &byte, 1u) == 1);
    byte ^= 1u;
    assert(snag_seek(fd, (int64_t)boundary - 1, SEEK_SET) == (int64_t)boundary - 1);
    assert(!snag_write_full(fd, &byte, 1u));
    assert_output_span_unresolved(fd, boundary, &anchor, &span, &expected);
    byte ^= 1u;
    assert(snag_seek(fd, (int64_t)boundary - 1, SEEK_SET) == (int64_t)boundary - 1);
    assert(!snag_write_full(fd, &byte, 1u));
    assert(!snag_truncate(fd, (int64_t)boundary - 1));
    assert_output_span_unresolved(fd, boundary, &anchor, &span, &expected);
    snag_buf_free(&out);
    assert(!close(fd));
    assert(!unlink(path));
    free(path);
}

static void
assert_output_reference_rejected(const struct snag_binary_ref *reference,
    const struct snag_binary_batch *batch)
{
    struct snag_binary_response_output out;
    memset(&out, 0x5a, sizeof(out));
    unsigned char saved[sizeof(out)];
    memcpy(saved, &out, sizeof(out));
    assert(snag_binary_output_ref_resolve(reference, batch, &out) < 0);
    assert(!memcmp(saved, &out, sizeof(out)));
}

static void
assert_output_text_missing(const struct snag_binary_batch *batch, uint64_t sequence)
{
    struct snag_binary_ref reference = {99u, 98u, 97u};
    assert(snag_binary_output_ref_create(batch, sequence, &reference) < 0);
    assert(reference.sequence == 99u && reference.offset == 98u && reference.size == 97u);
    reference.sequence = sequence;
    assert_output_reference_rejected(&reference, batch);
}

static void
test_output_references(const struct snag_binary_event *event)
{
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, event));
    struct snag_binary_record record = {
        .kind = (uint16_t)event->kind, .version = 1u, .payload = payload.data, .size = payload.len
    };
    struct snag_binary_batch batch;
    input_reference_batch(&record, &bytes, &batch);
    struct snag_binary_ref reference;
    assert(!snag_binary_output_ref_create(&batch, 2u, &reference));
    assert(reference.sequence == 2u && reference.offset == 82u && reference.size == 4u);
    struct snag_binary_response_output out;
    assert(!snag_binary_output_ref_resolve(&reference, &batch, &out));
    const struct snag_binary_response_output *original = &event->data.response_output;
    assert(out.cycle == original->cycle && out.index == original->index &&
        out.offset == original->offset && out.item.kind == original->item.kind &&
        out.item.phase == original->item.phase);
    assert(!memcmp(out.turn, original->turn, 16u) &&
        !memcmp(out.response, original->response, 16u) &&
        !memcmp(out.item.id, original->item.id, 16u));
    assert(out.item.text.size == 4u && !memcmp(out.item.text.data, "same", 4u));
    assert(out.item.provider_id.size == 4u && !memcmp(out.item.provider_id.data, "same", 4u));
    assert(out.item.text.data == out.item.provider_id.data + 8u);
    assert_input_leaf_missing(&batch, 2u, SNAG_BINARY_INPUT_TEXT);
    struct snag_binary_ref invalid[] = {
        {2u, 74u, 4u}, /* Equal provider-ID bytes have no public-text authority. */
        {2u, 82u, 0u}, {2u, 82u, 3u}, {2u, 83u, 3u}, {2u, 81u, 5u},
        {2u, 82u, 5u}, {2u, UINT32_MAX, 4u}, {2u, 82u, UINT32_MAX},
        {1u, 82u, 4u}, {3u, 82u, 4u}, {UINT64_MAX, 82u, 4u}
    };
    for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert_output_reference_rejected(&invalid[i], &batch);
    assert_output_reference_rejected(NULL, &batch);
    assert_output_reference_rejected(&reference, NULL);
    assert(snag_binary_output_ref_resolve(&reference, &batch, NULL) < 0);
    assert(snag_binary_output_ref_create(&batch, 2u, NULL) < 0);
    assert_output_text_missing(NULL, 2u);
    assert_output_text_missing(&batch, 0u);
    assert_output_text_missing(&batch, 1u);
    assert_output_text_missing(&batch, 3u);
    assert_output_text_missing(&batch, UINT64_MAX);
    for (size_t i = 0u; i < payload.len; ++i) {
        record.size = i;
        input_reference_batch(&record, &bytes, &batch);
        assert_output_text_missing(&batch, 2u);
        assert_output_reference_rejected(&reference, &batch);
    }
    record.size = payload.len;
    record.version = 2u;
    input_reference_batch(&record, &bytes, &batch);
    assert_output_text_missing(&batch, 2u);
    assert_output_reference_rejected(&reference, &batch);
    record.version = 1u;
    record.flags = SNAG_BINARY_RECORD_OPTIONAL;
    input_reference_batch(&record, &bytes, &batch);
    assert_output_text_missing(&batch, 2u);
    assert_output_reference_rejected(&reference, &batch);
    record.kind = 0x8000u;
    input_reference_batch(&record, &bytes, &batch);
    assert_output_text_missing(&batch, 2u);
    assert_output_reference_rejected(&reference, &batch);
    record.kind = SNAG_BINARY_RESPONSE_OUTPUT;
    record.flags = 0u;
    payload.data[74u] = '\n'; /* Validate metadata even when selecting only text. */
    input_reference_batch(&record, &bytes, &batch);
    assert_output_text_missing(&batch, 2u);
    assert_output_reference_rejected(&reference, &batch);
    payload.data[74u] = 's';
    assert(!snag_buf_append(&payload, "x", 1u));
    record.payload = payload.data;
    record.size = payload.len;
    input_reference_batch(&record, &bytes, &batch);
    assert_output_text_missing(&batch, 2u);
    assert_output_reference_rejected(&reference, &batch);
    struct snag_binary_event timer = {.kind = SNAG_BINARY_TIMER_SCHEDULED};
    timer.data.timer.due_ms = 1u;
    timer.data.timer.text = text("same");
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &timer));
    record.kind = SNAG_BINARY_TIMER_SCHEDULED;
    record.payload = payload.data;
    record.size = payload.len;
    input_reference_batch(&record, &bytes, &batch);
    assert_output_text_missing(&batch, 2u);
    assert_output_reference_rejected(&reference, &batch);
    snag_buf_free(&bytes);
    snag_buf_free(&payload);
}

static void
test_public_literal_limits(const struct snag_binary_public_item *item)
{
    struct snag_binary_public_value value = {.item = *item};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_public_items_encode(&payload, &value, 1u));
    struct snag_binary_public_items items;
    assert(!snag_binary_public_items_decode(payload.data, payload.len, &items));
    size_t offset = 0u;
    struct snag_binary_public_value decoded;
    assert(!snag_binary_public_items_next(&items, &offset, &decoded));
    assert(decoded.item.text.size == SNAG_MAX_PUBLIC_ITEM &&
        !memcmp(decoded.item.text.data, item->text.data, SNAG_MAX_PUBLIC_ITEM));
    struct snag_binary_public_items kept = items;
    assert(snag_binary_public_items_decode(payload.data, payload.len - 1u, &items) < 0);
    assert(items.data == kept.data && items.size == kept.size);
    size_t saved = payload.len;
    value.item.text.size++;
    assert(snag_binary_public_items_encode(&payload, &value, 1u) < 0 && payload.len == saved);
    size_t length_offset = (size_t)(decoded.item.text.data - payload.data) - 4u;
    payload.data[length_offset] = 1u;
    assert(!snag_buf_append(&payload, "!", 1u));
    items = (struct snag_binary_public_items){(const unsigned char *)"keep", 4u};
    kept = items;
    assert(snag_binary_public_items_decode(payload.data, payload.len, &items) < 0);
    assert(items.data == kept.data && items.size == kept.size);
    snag_buf_free(&payload);
}

static void
test_response_output_limits(struct snag_binary_event event)
{
    struct snag_binary_response_output *output = &event.data.response_output;
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    char provider[SNAG_MAX_PROVIDER_ID + 1u];
    memset(provider, 'p', sizeof(provider));
    output->item.provider_id = (struct snag_binary_text){(unsigned char *)provider,
        SNAG_MAX_PROVIDER_ID};
    roundtrip(&event);
    output->item.provider_id.size++;
    assert(snag_binary_event_encode(&payload, &event) < 0 && !payload.len);
    output->item.provider_id = text("id-\xc3\xa9");
    unsigned char *large = malloc(SNAG_MAX_PUBLIC_ITEM + 1u);
    assert(large);
    for (size_t i = 0u; i < SNAG_MAX_PUBLIC_ITEM; i += 2u) {
        large[i] = 0xc3u;
        large[i + 1u] = 0xa9u;
    }
    large[SNAG_MAX_PUBLIC_ITEM] = '!';
    output->offset = 0u;
    output->item.text = (struct snag_binary_text){large, SNAG_MAX_PUBLIC_ITEM};
    test_public_literal_limits(&output->item);
    assert(!snag_binary_event_encode(&payload, &event));
    struct snag_binary_record record = {
        .kind = (uint16_t)event.kind, .version = 1u, .payload = payload.data, .size = payload.len
    };
    struct snag_binary_event decoded;
    assert(!snag_binary_event_decode(&record, &decoded));
    struct snag_binary_text result = decoded.data.response_output.item.text;
    assert(result.size == SNAG_MAX_PUBLIC_ITEM && !memcmp(result.data, large, result.size));
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_binary_batch batch;
    /* A record above the grouping target occupies its own batch. */
    struct snag_binary_identity identity = {.id = {1}, .created_ms = 1u};
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    snag_binary_header_encode(header, &identity);
    struct snag_binary_anchor anchor, next;
    assert(!snag_binary_header_decode(header, sizeof(header), &identity, &anchor));
    assert(!snag_binary_batch_encode(&bytes, &anchor, &record, 1u, 0u, NULL));
    assert(!snag_binary_batch_decode(bytes.data, bytes.len, &anchor, &batch, &next));
    struct snag_binary_ref reference;
    struct snag_binary_response_output source;
    assert(!snag_binary_output_ref_create(&batch, 1u, &reference));
    assert(!snag_binary_output_ref_resolve(&reference, &batch, &source));
    assert(source.item.text.size == result.size &&
        !memcmp(source.item.text.data, large, result.size));
    char *path = snag_path_join(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
        "snajpagent-output-span-large-XXXXXX");
    assert(path);
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(!snag_write_full(fd, header, sizeof(header)));
    assert(!snag_write_full(fd, bytes.data, bytes.len));
    struct snag_binary_output_span span = {
        .first = reference, .last_sequence = 1u, .bytes = SNAG_MAX_PUBLIC_ITEM
    };
    struct snag_binary_public_item item = source.item;
    item.text = (struct snag_binary_text){0};
    struct snag_buf joined = {.max = SNAG_MAX_PUBLIC_ITEM};
    assert(!snag_binary_output_span_resolve(fd, next.end, &anchor, &span,
        source.turn, source.response, source.cycle, &item, &joined));
    assert(joined.len == result.size && !memcmp(joined.data, large, joined.len));
    snag_buf_free(&joined);
    assert(!close(fd));
    assert(!unlink(path));
    free(path);
    snag_buf_free(&bytes);
    record.size--;
    assert_rejected(record);
    size_t saved = payload.len;
    output->offset = 1u;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    output->offset = 0u;
    output->item.text.size++;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    /* Include the entire oversized text, so rejection is not mere truncation. */
    size_t length_offset = (size_t)(result.data - payload.data) - 4u;
    payload.data[length_offset] = 1u; /* 0x00200000 -> 0x00200001. */
    assert(!snag_buf_append(&payload, "!", 1u));
    record.payload = payload.data;
    record.size = payload.len;
    assert_rejected(record);
    free(large);
    snag_buf_free(&payload);
}

static void
test_response_output(void)
{
    struct snag_binary_event event = {.kind = SNAG_BINARY_RESPONSE_OUTPUT}, decoded;
    struct snag_binary_response_output *output = &event.data.response_output;
    for (size_t i = 0u; i < 16u; ++i) {
        output->turn[i] = (unsigned char)i;
        output->response[i] = (unsigned char)(i + 16u);
        output->item.id[i] = (unsigned char)(i + 32u);
    }
    output->cycle = UINT32_C(0x01020304);
    output->index = UINT64_C(0x0807060504030201);
    output->offset = 2u;
    output->item.kind = SNAG_BINARY_ITEM_ASSISTANT;
    output->item.phase = SNAG_BINARY_PHASE_COMMENTARY;
    output->item.provider_id = text("same");
    output->item.text = text("same");
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    static const unsigned char bytes[] =
        "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f"
        "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f"
        "\x04\x03\x02\x01\x01\x02\x03\x04\x05\x06\x07\x08\x02\0\0\0\0\0\0\0"
        "\x01\x01\x20\x21\x22\x23\x24\x25\x26\x27\x28\x29\x2a\x2b\x2c\x2d\x2e\x2f"
        "\x04\0\0\0same\x04\0\0\0same";
    assert(SNAG_BINARY_RESPONSE_OUTPUT == 161 && snag_binary_event_version(event.kind) == 1u);
    assert(payload.len == sizeof(bytes) - 1u && !memcmp(payload.data, bytes, payload.len));
    roundtrip(&event);
    struct snag_binary_record record = {
        .kind = (uint16_t)event.kind, .version = 1u, .payload = payload.data, .size = payload.len
    };
    assert(!snag_binary_event_decode(&record, &decoded));
    const struct snag_binary_response_output *result = &decoded.data.response_output;
    assert(result->cycle == output->cycle && result->index == output->index &&
        result->offset == output->offset && result->item.kind == SNAG_BINARY_ITEM_ASSISTANT &&
        result->item.phase == SNAG_BINARY_PHASE_COMMENTARY);
    assert(!memcmp(result->turn, output->turn, 16u) &&
        !memcmp(result->response, output->response, 16u) &&
        !memcmp(result->item.id, output->item.id, 16u));
    assert(result->item.text.size == 4u && !memcmp(result->item.text.data, "same", 4u));
    test_output_references(&event);
    test_response_output_limits(event);
    static const struct { size_t offset; unsigned char value; } mutations[] = {
        {52u, 0u}, {52u, 3u}, {52u, 255u}, {53u, 0u}, {53u, 3u}, {53u, 255u},
        {51u, 1u}, {70u, 0u}, {71u, 3u}, {78u, 0u}, {74u, 0u}, {74u, '\n'},
        {74u, 0x7fu}, {74u, 0xffu}, {82u, 0u}, {82u, 0xffu}
    };
    for (size_t i = 0u; i < sizeof(mutations) / sizeof(mutations[0]); ++i) {
        unsigned char old = payload.data[mutations[i].offset];
        payload.data[mutations[i].offset] = mutations[i].value;
        assert_rejected(record);
        payload.data[mutations[i].offset] = old;
    }
    memset(payload.data + 32u, 0, 4u);
    assert_rejected(record);
    memcpy(payload.data + 32u, bytes + 32u, 4u);
    payload.data[52u] = SNAG_BINARY_ITEM_REFUSAL;
    assert_rejected(record); /* Refusal cannot be commentary. */
    payload.data[52u] = SNAG_BINARY_ITEM_ASSISTANT;
    payload.data[74u] = 0xc2u;
    payload.data[75u] = 0x85u;
    assert_rejected(record); /* Valid UTF-8 C1 control in provider identity. */
    payload.data[74u] = 's';
    payload.data[75u] = 'a';
    size_t saved = payload.len;
    output->cycle = 0u;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    output->cycle = UINT32_MAX;
    output->index = UINT64_MAX;
    output->offset = SNAG_MAX_PUBLIC_ITEM - 4u;
    roundtrip(&event);
    output->offset++;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    output->offset = UINT64_MAX;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    output->offset = 0u;
    output->item.phase = SNAG_BINARY_PHASE_FINAL_ANSWER;
    roundtrip(&event);
    output->item.kind = SNAG_BINARY_ITEM_REFUSAL;
    roundtrip(&event);
    output->item.phase = SNAG_BINARY_PHASE_COMMENTARY;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    output->item.phase = SNAG_BINARY_PHASE_FINAL_ANSWER;
    output->item.kind = SNAG_BINARY_ITEM_TOOL_CALL;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    output->item.kind = SNAG_BINARY_ITEM_ASSISTANT;
    output->item.phase = (enum snag_binary_item_phase)3;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    output->item.phase = SNAG_BINARY_PHASE_FINAL_ANSWER;
    const char *bad_provider_ids[] = {"", "a\nb", "a\x7f", "a\xc2\x85", "\xff"};
    for (size_t i = 0u; i < sizeof(bad_provider_ids) / sizeof(bad_provider_ids[0]); ++i) {
        output->item.provider_id = text(bad_provider_ids[i]);
        assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    }
    output->item.provider_id = (struct snag_binary_text){(unsigned char *)"a\0b", 3u};
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    output->item.provider_id = text("same");
    output->item.text = text("");
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    output->item.text = (struct snag_binary_text){(unsigned char *)"a\0b", 3u};
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    output->item.text = text("\xff");
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    output->item.text = text("line 1\nline 2 \xc3\xa9");
    roundtrip(&event);
    assert(payload.len == sizeof(bytes) - 1u && !memcmp(payload.data, bytes, payload.len));
    snag_buf_free(&payload);
}

static void
assert_public_items_rejected(const void *data, size_t size)
{
    const struct snag_binary_public_items sentinel = {(const unsigned char *)"keep", 4u};
    struct snag_binary_public_items out = sentinel;
    assert(snag_binary_public_items_decode(data, size, &out) < 0);
    assert(out.data == sentinel.data && out.size == sentinel.size);
}

static void
test_public_items(void)
{
    struct snag_binary_public_value values[2] = {0};
    struct snag_binary_public_value decoded[2];
    values[0].item.kind = SNAG_BINARY_ITEM_ASSISTANT;
    values[0].item.phase = SNAG_BINARY_PHASE_COMMENTARY;
    values[0].item.id[0] = 3u;
    values[0].item.provider_id = text("id");
    values[0].item.text = text("hi");
    values[1].item.kind = SNAG_BINARY_ITEM_REFUSAL;
    values[1].item.phase = SNAG_BINARY_PHASE_FINAL_ANSWER;
    values[1].item.id[0] = 4u;
    values[1].item.provider_id = text("ref");
    values[1].source = (struct snag_binary_output_span){{7u, 79u, 1u}, 9u, 3u};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_public_items_encode(&payload, values, 2u));
    static const unsigned char expected[] =
        "\x02\0\0\0\x01\x01\x03\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\x02\0\0\0id\x02\0\0\0hi"
        "\x02\x02\x04\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\x03\0\0\0ref\xff\xff\xff\xff"
        "\x07\0\0\0\0\0\0\0\x4f\0\0\0\x01\0\0\0"
        "\x09\0\0\0\0\0\0\0\x03\0\0\0";
    assert(payload.len == sizeof(expected) - 1u &&
        !memcmp(payload.data, expected, payload.len));
    struct snag_binary_public_items items;
    assert(!snag_binary_public_items_decode(payload.data, payload.len, &items));
    size_t cursor = 0u;
    assert(!snag_binary_public_items_next(&items, &cursor, &decoded[0]));
    assert(decoded[0].item.kind == SNAG_BINARY_ITEM_ASSISTANT &&
        decoded[0].item.phase == SNAG_BINARY_PHASE_COMMENTARY && decoded[0].item.id[0] == 3u &&
        decoded[0].item.text.size == 2u && !memcmp(decoded[0].item.text.data, "hi", 2u));
    assert(!decoded[0].source.first.sequence);
    assert(!snag_binary_public_items_next(&items, &cursor, &decoded[1]));
    assert(decoded[1].item.kind == SNAG_BINARY_ITEM_REFUSAL &&
        decoded[1].item.phase == SNAG_BINARY_PHASE_FINAL_ANSWER && decoded[1].item.id[0] == 4u &&
        !decoded[1].item.text.data && !decoded[1].item.text.size);
    assert(decoded[1].source.first.sequence == 7u && decoded[1].source.first.offset == 79u &&
        decoded[1].source.first.size == 1u && decoded[1].source.last_sequence == 9u &&
        decoded[1].source.bytes == 3u);
    struct snag_binary_public_value kept = decoded[1];
    assert(snag_binary_public_items_next(&items, &cursor, &decoded[1]) == 1);
    assert(!memcmp(&kept, &decoded[1], sizeof(kept)) && cursor == payload.len);
    cursor = 1u;
    assert(snag_binary_public_items_next(&items, &cursor, &decoded[1]) < 0 && cursor == 1u);
    assert(!memcmp(&kept, &decoded[1], sizeof(kept)));
    struct snag_buf again = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_public_items_encode(&again, decoded, 2u));
    assert(again.len == payload.len && !memcmp(again.data, payload.data, payload.len));
    for (size_t i = 0u; i < payload.len; ++i) {
        assert_public_items_rejected(payload.data, i);
    }
    static const struct { size_t offset; unsigned char value; } mutations[] = {
        {0u, 0u}, {0u, 1u}, {0u, 3u}, {4u, 3u}, {5u, 0u}, {22u, 0u}, {26u, 0u},
        {28u, 0u}, {32u, 0u}, {34u, 3u}, {35u, 1u}, {59u, 0u}, {75u, 0u}, {87u, 0u}
    };
    for (size_t i = 0u; i < sizeof(mutations) / sizeof(mutations[0]); ++i) {
        unsigned char old = payload.data[mutations[i].offset];
        payload.data[mutations[i].offset] = mutations[i].value;
        assert_public_items_rejected(payload.data, payload.len);
        payload.data[mutations[i].offset] = old;
    }
    size_t saved = payload.len;
    values[1].item.text = text("conflict");
    assert(snag_binary_public_items_encode(&payload, values, 2u) < 0 && payload.len == saved);
    values[1].item.text = (struct snag_binary_text){0};
    values[1].source.first.sequence = 0u;
    assert(snag_binary_public_items_encode(&payload, values, 2u) < 0 && payload.len == saved);
    values[1].source.first.sequence = 7u;
    values[0].item.kind = SNAG_BINARY_ITEM_TOOL_CALL;
    assert(snag_binary_public_items_encode(&payload, values, 2u) < 0 && payload.len == saved);
    values[0].item.kind = SNAG_BINARY_ITEM_ASSISTANT;
    values[0].item.text = text("");
    assert(snag_binary_public_items_encode(&payload, values, 2u) < 0 && payload.len == saved);
    values[0].item.text = text("hi");
    assert(snag_binary_public_items_encode(&payload, NULL, 1u) < 0 && payload.len == saved);
    assert(snag_binary_public_items_encode(&payload, values, SIZE_MAX) < 0 && payload.len == saved);
    payload.max = saved;
    assert(snag_binary_public_items_encode(&payload, values, 2u) < 0 && payload.len == saved);
    assert(!memcmp(payload.data, expected, saved));
    payload.max = SNAG_MAX_EVENT_LINE;
    assert(!snag_buf_append(&payload, "x", 1u));
    assert_public_items_rejected(payload.data, payload.len);
    snag_buf_reset(&payload);
    assert(!snag_binary_public_items_encode(&payload, NULL, 0u));
    assert(payload.len == 4u && !memcmp(payload.data, "\0\0\0\0", 4u));
    assert(!snag_binary_public_items_decode(payload.data, payload.len, &items));
    cursor = 0u;
    assert(snag_binary_public_items_next(&items, &cursor, &decoded[1]) == 1 && !cursor);
    assert(!memcmp(&kept, &decoded[1], sizeof(kept)));
    snag_buf_free(&again);
    snag_buf_free(&payload);
}

static void
test_response_interruption(void)
{
    static const unsigned char empty[] = {0u, 0u, 0u, 0u};
    struct snag_binary_event event = {.kind = SNAG_BINARY_RESPONSE_INTERRUPTED};
    struct snag_binary_response_interruption *interrupted = &event.data.response_interrupted;
    interrupted->turn[0] = 1u;
    interrupted->response[0] = 2u;
    interrupted->cycle = 1u;
    interrupted->origin = SNAG_BINARY_INTERRUPT_USER;
    interrupted->reason = SNAG_BINARY_INTERRUPT_CANCELLED;
    interrupted->partial = (struct snag_binary_public_items){empty, sizeof(empty)};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    static const unsigned char expected[] =
        "\x01\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\x02\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\x01\0\0\0\x01\x01\0\0\0\0";
    assert(payload.len == sizeof(expected) - 1u &&
        !memcmp(payload.data, expected, payload.len));
    assert(SNAG_BINARY_RESPONSE_INTERRUPTED == 162 && snag_binary_event_version(event.kind) == 1u);
    roundtrip(&event);
    struct snag_binary_record record = {
        .kind = (uint16_t)event.kind, .version = 1u, .payload = payload.data, .size = payload.len
    };
    const enum snag_binary_interrupt_origin origins[] = {
        SNAG_BINARY_INTERRUPT_USER, SNAG_BINARY_INTERRUPT_RECOVERY,
        SNAG_BINARY_INTERRUPT_OUTPUT, SNAG_BINARY_INTERRUPT_STEERING
    };
    const enum snag_binary_interrupt_reason reasons[] = {
        SNAG_BINARY_INTERRUPT_CANCELLED, SNAG_BINARY_INTERRUPT_PROCESS_LOST,
        SNAG_BINARY_INTERRUPT_OUTPUT_LOST, SNAG_BINARY_INTERRUPT_STEERED,
        SNAG_BINARY_INTERRUPT_CONTROL
    };
    for (size_t i = 0u; i < sizeof(origins) / sizeof(origins[0]); ++i) {
        for (size_t j = 0u; j < sizeof(reasons) / sizeof(reasons[0]); ++j) {
            interrupted->origin = origins[i];
            interrupted->reason = reasons[j];
            payload.data[36u] = (unsigned char)origins[i];
            payload.data[37u] = (unsigned char)reasons[j];
            if ((i == 3u) != (j == 3u)) {
                size_t saved = payload.len;
                assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
                assert_rejected(record);
            } else {
                roundtrip(&event);
            }
        }
    }
    payload.data[36u] = SNAG_BINARY_INTERRUPT_USER;
    payload.data[37u] = SNAG_BINARY_INTERRUPT_CANCELLED;
    const unsigned char bad_origins[] = {0u, 5u, 255u};
    for (size_t i = 0u; i < sizeof(bad_origins); ++i) {
        payload.data[36u] = bad_origins[i];
        assert_rejected(record);
    }
    payload.data[36u] = SNAG_BINARY_INTERRUPT_USER;
    const unsigned char bad_reasons[] = {0u, SNAG_BINARY_INTERRUPT_SESSION_RECOVERED, 7u, 255u};
    for (size_t i = 0u; i < sizeof(bad_reasons); ++i) {
        payload.data[37u] = bad_reasons[i];
        assert_rejected(record);
    }
    payload.data[37u] = SNAG_BINARY_INTERRUPT_CANCELLED;
    payload.data[32u] = 0u;
    assert_rejected(record);
    payload.data[32u] = 1u;
    size_t saved = payload.len;
    interrupted->origin = SNAG_BINARY_INTERRUPT_USER;
    interrupted->reason = SNAG_BINARY_INTERRUPT_CANCELLED;
    interrupted->cycle = 0u;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    interrupted->cycle = UINT32_MAX;
    roundtrip(&event);
    interrupted->partial = (struct snag_binary_public_items){0};
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    interrupted->partial = (struct snag_binary_public_items){empty, 3u};
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    /* Extending the shared tag namespace must not widen the turn profile. */
    struct snag_binary_event turn = {.kind = SNAG_BINARY_TURN_INTERRUPTED};
    turn.data.interrupted.origin = SNAG_BINARY_INTERRUPT_STEERING;
    turn.data.interrupted.reason = SNAG_BINARY_INTERRUPT_CANCELLED;
    assert(snag_binary_event_encode(&payload, &turn) < 0 && payload.len == saved);
    turn.data.interrupted.origin = SNAG_BINARY_INTERRUPT_USER;
    turn.data.interrupted.reason = SNAG_BINARY_INTERRUPT_STEERED;
    assert(snag_binary_event_encode(&payload, &turn) < 0 && payload.len == saved);
    turn.data.interrupted.reason = SNAG_BINARY_INTERRUPT_CONTROL;
    assert(snag_binary_event_encode(&payload, &turn) < 0 && payload.len == saved);
    turn.data.interrupted.origin = SNAG_BINARY_INTERRUPT_RECOVERY;
    turn.data.interrupted.reason = SNAG_BINARY_INTERRUPT_SESSION_RECOVERED;
    roundtrip(&turn);
    snag_buf_free(&payload);
}

static void
test_response_failure_limits(struct snag_binary_event event)
{
    struct snag_binary_response_failure *failure = &event.data.response_failed;
    unsigned char message[8193u];
    unsigned char policy[128u];
    memset(message, 'm', sizeof(message));
    memset(policy, 'p', sizeof(policy));
    failure->present = 15u;
    failure->message = (struct snag_binary_text){message, 8192u};
    failure->policy_code = (struct snag_binary_text){policy, 63u};
    failure->policy_type = (struct snag_binary_text){policy, 63u};
    failure->clarification_skipped = (struct snag_binary_text){policy, 127u};
    roundtrip(&event);
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_buf_append(&payload, "keep", 4u));
    struct snag_binary_text *fields[] = {&failure->message, &failure->policy_code,
        &failure->policy_type, &failure->clarification_skipped};
    static const unsigned char nul[] = {'a', 0u, 'b'};
    for (size_t i = 0u; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        struct snag_binary_text saved = *fields[i];
        fields[i]->size++;
        assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == 4u);
        *fields[i] = text("\xff");
        assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == 4u);
        *fields[i] = (struct snag_binary_text){nul, sizeof(nul)};
        assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == 4u);
        *fields[i] = saved;
    }
    failure->clarification_skipped = text("");
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == 4u);
    failure->clarification_skipped = text("s");
    failure->policy_code = text("");
    failure->policy_type = text("");
    failure->message = text("\xc3\xa9");
    roundtrip(&event);
    assert(!memcmp(payload.data, "keep", 4u));
    snag_buf_free(&payload);
}

static void
test_response_failure(void)
{
    static const unsigned char empty[] = {0u, 0u, 0u, 0u};
    struct snag_binary_event event = {.kind = SNAG_BINARY_RESPONSE_FAILED};
    struct snag_binary_response_failure *failure = &event.data.response_failed;
    failure->turn[0] = 1u;
    failure->response[0] = 2u;
    failure->cycle = 1u;
    failure->class_name = SNAG_BINARY_FAILURE_PROVIDER;
    failure->retry_count = 2u;
    failure->message = text("");
    failure->partial = (struct snag_binary_public_items){empty, sizeof(empty)};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    static const unsigned char expected[] =
        "\x01\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\x02\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\x01\0\0\0\x02\x02\0\0\0\0\0\0\0\0\0";
    assert(payload.len == sizeof(expected) - 1u &&
        !memcmp(payload.data, expected, payload.len));
    assert(SNAG_BINARY_RESPONSE_FAILED == 163 && snag_binary_event_version(event.kind) == 1u);
    roundtrip(&event);
    struct snag_binary_record record = {
        .kind = (uint16_t)event.kind, .version = 1u, .payload = payload.data, .size = payload.len
    };
    size_t saved = payload.len;
    for (unsigned i = 0u; i <= 9u; ++i) {
        failure->class_name = (enum snag_binary_failure_class)i;
        payload.data[36u] = (unsigned char)i;
        if (i == 0u || i == 4u || i == 5u || i == 9u) {
            assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
            assert_rejected(record);
        } else {
            roundtrip(&event);
        }
    }
    failure->class_name = SNAG_BINARY_FAILURE_PROVIDER;
    payload.data[36u] = SNAG_BINARY_FAILURE_PROVIDER;
    for (unsigned i = 0u; i <= 3u; ++i) {
        failure->retry_count = (uint8_t)i;
        if (i == 3u) {
            assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
            payload.data[37u] = 3u;
            assert_rejected(record);
        } else {
            roundtrip(&event);
        }
    }
    failure->retry_count = 2u;
    payload.data[37u] = 2u;
    failure->cycle = 0u;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    payload.data[32u] = 0u;
    assert_rejected(record);
    payload.data[32u] = 1u;
    failure->cycle = UINT32_MAX;
    roundtrip(&event);
    failure->cycle = 1u;
    for (unsigned i = 0u; i <= UINT8_MAX; ++i) {
        if (i == 0u || i == 1u || i == 3u || i == 7u || i == 15u) continue;
        failure->present = (uint8_t)i;
        assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
        payload.data[38u] = (unsigned char)i;
        assert_rejected(record);
    }
    payload.data[38u] = 0u;
    const unsigned char masks[] = {0u, 1u, 3u, 7u, 15u};
    const size_t tail_sizes[] = {0u, 1u, 9u, 10u, 25u};
    static const unsigned char tail[] =
        "\x01\0\0\0\0\x01\0\0\0\x01"
        "\x01\0\0\0c\x01\0\0\0t\x01\0\0\0s";
    struct snag_buf full = {.max = SNAG_MAX_EVENT_LINE};
    for (size_t i = 0u; i < sizeof(masks); ++i) {
        failure->present = masks[i];
        failure->policy_stopped = true;
        failure->turn_retry_attempts = (uint64_t)UINT32_MAX + 1u;
        failure->new_input = true;
        failure->policy_code = text("c");
        failure->policy_type = text("t");
        failure->clarification_skipped = text("s");
        snag_buf_reset(&full);
        assert(!snag_binary_event_encode(&full, &event));
        payload.data[38u] = masks[i];
        assert(full.len == saved + tail_sizes[i] && !memcmp(full.data, payload.data, saved));
        assert(!memcmp(full.data + saved, tail, tail_sizes[i]));
        roundtrip(&event);
        struct snag_binary_record complete = {
            .kind = (uint16_t)event.kind, .version = 1u, .payload = full.data, .size = full.len
        };
        struct snag_binary_event decoded;
        assert(!snag_binary_event_decode(&complete, &decoded));
        const struct snag_binary_response_failure *result = &decoded.data.response_failed;
        assert(result->present == masks[i] && result->policy_stopped == (i >= 1u) &&
            result->new_input == (i >= 3u) && result->turn_retry_attempts ==
            (i >= 2u ? (uint64_t)UINT32_MAX + 1u : 0u));
        assert(result->policy_code.size == (i == 4u ? 1u : 0u) &&
            result->policy_type.size == result->policy_code.size &&
            result->clarification_skipped.size == result->policy_code.size);
        failure->policy_stopped = false;
        failure->turn_retry_attempts = 0u;
        failure->new_input = false;
        roundtrip(&event);
    }
    /* The full encoded form above retains true booleans and the 64-bit counter. */
    struct snag_binary_record complete = {
        .kind = (uint16_t)event.kind, .version = 1u, .payload = full.data, .size = full.len
    };
    const size_t boolean_offsets[] = {47u, 56u};
    for (size_t i = 0u; i < sizeof(boolean_offsets) / sizeof(boolean_offsets[0]); ++i) {
        full.data[boolean_offsets[i]] = 2u;
        assert_rejected(complete);
        full.data[boolean_offsets[i]] = 1u;
    }
    full.data[48u] = 1u;
    assert_rejected(complete);
    full.data[48u] = 0u;
    failure->turn_retry_attempts = (uint64_t)UINT32_MAX + 2u;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    failure->turn_retry_attempts = (uint64_t)UINT32_MAX + 1u;
    test_response_failure_limits(event);
    struct snag_binary_public_value values[2] = {0};
    values[0].item.kind = SNAG_BINARY_ITEM_ASSISTANT;
    values[0].item.phase = SNAG_BINARY_PHASE_COMMENTARY;
    values[0].item.id[0] = 3u;
    values[0].item.provider_id = text("p");
    values[0].item.text = text("literal");
    values[1] = values[0];
    values[1].item.id[0] = 4u;
    values[1].item.text = (struct snag_binary_text){0};
    values[1].source = (struct snag_binary_output_span){{7u, 79u, 1u}, 9u, 3u};
    struct snag_buf items = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_public_items_encode(&items, values, 2u));
    failure->partial = (struct snag_binary_public_items){items.data, items.len};
    roundtrip(&event);
    failure->partial.size--;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    failure->partial = (struct snag_binary_public_items){0};
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    snag_buf_free(&items);
    snag_buf_free(&full);
    snag_buf_free(&payload);
}

static void
test_correction_snapshots(struct snag_binary_event event)
{
    const char *prompts[] = {SNAG_EMPTY_OUTPUT_CORRECTION,
        SNAG_OVERSIZED_OUTPUT_CORRECTION, SNAG_CYBER_CLARIFICATION};
    struct snag_binary_response_correction *correction = &event.data.response_correction;
    struct snag_binary_public_value value = {0};
    value.item.phase = SNAG_BINARY_PHASE_FINAL_ANSWER;
    value.item.id[0] = 4u;
    value.item.provider_id = text("p");
    struct snag_buf items = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    for (size_t i = 0u; i < sizeof(prompts) / sizeof(prompts[0]); ++i) {
        correction->text = text(prompts[i]);
        value.item.kind = SNAG_BINARY_ITEM_ASSISTANT;
        value.item.text = correction->text;
        snag_buf_reset(&items);
        assert(!snag_binary_public_items_encode(&items, &value, 1u));
        correction->partial = (struct snag_binary_public_items){items.data, items.len};
        roundtrip(&event);
        snag_buf_reset(&payload);
        assert(!snag_binary_event_encode(&payload, &event));
        struct snag_binary_record record = {
            .kind = (uint16_t)event.kind, .version = 1u,
            .payload = payload.data, .size = payload.len
        };
        struct snag_binary_event decoded;
        assert(!snag_binary_event_decode(&record, &decoded));
        struct snag_binary_public_value public;
        size_t offset = 0u;
        assert(!snag_binary_public_items_next(&decoded.data.response_correction.partial,
            &offset, &public));
        size_t public_offset = (size_t)(public.item.text.data - payload.data);
        size_t kind_offset =
            (size_t)(decoded.data.response_correction.partial.data - payload.data) + 4u;
        struct snag_binary_batch batch;
        input_reference_batch(&record, &bytes, &batch);
        struct snag_binary_ref reference;
        assert(!snag_binary_input_ref_create(&batch, 2u, SNAG_BINARY_INPUT_TEXT, &reference));
        assert(reference.offset == 56u && reference.size == correction->text.size);
        const unsigned char *view;
        assert(!snag_binary_input_ref_resolve(&reference, &batch, SNAG_BINARY_INPUT_TEXT, &view));
        assert(!memcmp(view, correction->text.data, reference.size));
        struct snag_binary_ref wrong = reference;
        /* Equal public-output bytes cannot stand in for the host correction. */
        wrong.offset = (uint32_t)public_offset;
        assert_input_reference_rejected(&wrong, &batch, SNAG_BINARY_INPUT_TEXT);
        wrong = reference;
        wrong.size--;
        assert_input_reference_rejected(&wrong, &batch, SNAG_BINARY_INPUT_TEXT);
        const enum snag_binary_input_leaf others[] = {SNAG_BINARY_INPUT_CONTENT,
            SNAG_BINARY_INPUT_INSTRUCTIONS, SNAG_BINARY_INPUT_VOICE_TRANSCRIPT,
            SNAG_BINARY_INPUT_VOICE_REQUEST};
        for (size_t j = 0u; j < sizeof(others) / sizeof(others[0]); ++j) {
            assert(snag_binary_input_ref_create(&batch, 2u, others[j], &wrong) < 0);
            assert_input_reference_rejected(&reference, &batch, others[j]);
        }
        assert(snag_binary_output_ref_create(&batch, 2u, &wrong) < 0);
        value.item.kind = SNAG_BINARY_ITEM_REFUSAL;
        snag_buf_reset(&items);
        assert(!snag_binary_public_items_encode(&items, &value, 1u));
        correction->partial = (struct snag_binary_public_items){items.data, items.len};
        if (i < 2u) {
            roundtrip(&event);
        } else {
            size_t saved = payload.len;
            assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
            payload.data[kind_offset] = SNAG_BINARY_ITEM_REFUSAL;
            assert_rejected(record);
            input_reference_batch(&record, &bytes, &batch);
            assert(snag_binary_input_ref_create(&batch, 2u, SNAG_BINARY_INPUT_TEXT, &wrong) < 0);
            assert_input_reference_rejected(&reference, &batch, SNAG_BINARY_INPUT_TEXT);
        }
    }
    value.item.kind = SNAG_BINARY_ITEM_ASSISTANT;
    value.item.text = (struct snag_binary_text){0};
    value.source = (struct snag_binary_output_span){{7u, 79u, 1u}, 7u, 1u};
    snag_buf_reset(&items);
    assert(!snag_binary_public_items_encode(&items, &value, 1u));
    correction->partial = (struct snag_binary_public_items){items.data, items.len};
    roundtrip(&event);
    snag_buf_free(&bytes);
    snag_buf_free(&payload);
    snag_buf_free(&items);
}

static void
test_response_correction(void)
{
    static const unsigned char empty[] = {0u, 0u, 0u, 0u};
    struct snag_binary_event event = {.kind = SNAG_BINARY_RESPONSE_OUTPUT_CORRECTION};
    struct snag_binary_response_correction *correction = &event.data.response_correction;
    correction->turn[0] = 1u;
    correction->response[0] = 2u;
    correction->correction[0] = 3u;
    correction->cycle = 1u;
    correction->text = text(SNAG_EMPTY_OUTPUT_CORRECTION);
    correction->partial = (struct snag_binary_public_items){empty, sizeof(empty)};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    static const char original[] = "You tried to send an empty assistant message. "
        "Send nonempty text or take another action.";
    unsigned char expected[sizeof(original) + 59u] = {0};
    expected[0] = 1u;
    expected[16u] = 2u;
    expected[32u] = 1u;
    expected[36u] = 3u;
    expected[52u] = sizeof(original) - 1u;
    memcpy(expected + 56u, original, sizeof(original) - 1u);
    assert(payload.len == sizeof(expected) && !memcmp(payload.data, expected, payload.len));
    assert(SNAG_BINARY_RESPONSE_OUTPUT_CORRECTION == 164 &&
        snag_binary_event_version(event.kind) == 1u);
    roundtrip(&event);
    struct snag_binary_record record = {
        .kind = (uint16_t)event.kind, .version = 1u, .payload = payload.data, .size = payload.len
    };
    const unsigned char invalid_text[] = {'x', 0u, 0xffu};
    for (size_t i = 0u; i < sizeof(invalid_text); ++i) {
        payload.data[56u] = invalid_text[i];
        assert_rejected(record);
    }
    payload.data[56u] = 'Y';
    payload.data[32u] = 0u;
    assert_rejected(record);
    payload.data[32u] = 1u;
    payload.data[payload.len - 4u] = 1u;
    assert_rejected(record);
    payload.data[payload.len - 4u] = 0u;
    size_t saved = payload.len;
    correction->cycle = 0u;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    correction->cycle = UINT32_MAX;
    roundtrip(&event);
    correction->cycle = 1u;
    const struct snag_binary_text bad[] = {
        {NULL, 1u}, {NULL, 0u}, {(const unsigned char *)original, SIZE_MAX},
        {(const unsigned char *)"arbitrary instruction", 21u}
    };
    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        correction->text = bad[i];
        assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    }
    correction->text = text(SNAG_EMPTY_OUTPUT_CORRECTION);
    correction->partial.size--;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    correction->partial.size++;
    test_correction_snapshots(event);
    snag_buf_free(&payload);
}

static void
assert_graph_items_rejected(const void *data, size_t size)
{
    struct snag_binary_graph_items out = {(const unsigned char *)"keep", 4u, 9u};
    struct snag_binary_graph_items saved = out;
    assert(snag_binary_graph_items_decode(data, size, &out) < 0);
    assert(out.data == saved.data && out.size == saved.size && out.count == saved.count);
}

static void
test_large_graph_reference(const struct snag_binary_graph_items *items,
    const struct snag_binary_text *arguments)
{
    struct snag_binary_event event = {.kind = SNAG_BINARY_RESPONSE_COMPLETED};
    event.data.response_completed.cycle = 1u;
    event.data.response_completed.provider_id = text("r");
    event.data.response_completed.items = *items;
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    struct snag_binary_record record = {.kind = (uint16_t)event.kind, .version = 1u,
        .payload = payload.data, .size = payload.len};
    struct snag_binary_identity identity = {.id = {1}, .created_ms = 1u};
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    snag_binary_header_encode(header, &identity);
    struct snag_binary_anchor anchor, next;
    assert(!snag_binary_header_decode(header, sizeof(header), &identity, &anchor));
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_binary_batch batch;
    assert(!snag_binary_batch_encode(&bytes, &anchor, &record, 1u, 0u, NULL));
    assert(!snag_binary_batch_decode(bytes.data, bytes.len, &anchor, &batch, &next));
    struct snag_binary_ref reference;
    assert(!snag_binary_graph_ref_create(&batch, 1u, 0u, SNAG_BINARY_ITEM_TOOL_CALL, &reference));
    assert(reference.offset == 45u && reference.size == SNAG_MAX_TOOL_ARGUMENTS + 37u);
    struct snag_binary_graph_source source;
    assert(!snag_binary_graph_ref_resolve(&reference, &batch, SNAG_BINARY_ITEM_TOOL_CALL, &source));
    assert(source.item.data.call.arguments.size == arguments->size &&
        !memcmp(source.item.data.call.arguments.data, arguments->data, arguments->size));
    snag_buf_free(&bytes);
    snag_buf_free(&payload);
}

static void
test_graph_argument_limit(struct snag_binary_graph_item item)
{
    unsigned char *large = malloc(SNAG_MAX_TOOL_ARGUMENTS + 1u);
    assert(large);
    memcpy(large, "{\"x\":\"", 6u);
    memset(large + 6u, 'a', SNAG_MAX_TOOL_ARGUMENTS - 8u);
    memcpy(large + SNAG_MAX_TOOL_ARGUMENTS - 2u, "\"}", 2u);
    item.data.call.arguments = (struct snag_binary_text){large, SNAG_MAX_TOOL_ARGUMENTS};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_graph_items_encode(&payload, &item, 1u));
    struct snag_binary_graph_items items;
    assert(!snag_binary_graph_items_decode(payload.data, payload.len, &items));
    struct snag_binary_graph_item decoded;
    size_t cursor = 0u;
    assert(!snag_binary_graph_items_next(&items, &cursor, &decoded));
    assert(decoded.data.call.arguments.size == SNAG_MAX_TOOL_ARGUMENTS &&
        !memcmp(decoded.data.call.arguments.data, large, SNAG_MAX_TOOL_ARGUMENTS));
    test_large_graph_reference(&items, &decoded.data.call.arguments);
    assert_graph_items_rejected(payload.data, payload.len - 1u);
    size_t length_offset = (size_t)(decoded.data.call.arguments.data - payload.data) - 4u;
    size_t saved = payload.len;
    memmove(large + SNAG_MAX_TOOL_ARGUMENTS - 1u, large + SNAG_MAX_TOOL_ARGUMENTS - 2u, 2u);
    large[SNAG_MAX_TOOL_ARGUMENTS - 2u] = 'a';
    item.data.call.arguments.size++;
    assert(snag_binary_graph_items_encode(&payload, &item, 1u) < 0 && payload.len == saved);
    assert(!snag_buf_append(&payload, "x", 1u));
    memmove(payload.data + saved - 1u, payload.data + saved - 2u, 2u);
    payload.data[saved - 2u] = 'a';
    payload.data[length_offset] = 1u;
    assert_graph_items_rejected(payload.data, payload.len);
    snag_buf_free(&payload);
    free(large);
}

static void
test_graph_items(void)
{
    struct snag_binary_graph_item values[3] = {0};
    values[0].kind = SNAG_BINARY_ITEM_TOOL_CALL;
    values[0].data.call.id[0] = 7u;
    values[0].data.call.provider_id = text("i");
    values[0].data.call.provider_call_id = text("c");
    values[0].data.call.name = text("cd");
    values[0].data.call.arguments = text("{}");
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_graph_items_encode(&payload, values, 1u));
    static const unsigned char expected[] =
        "\x01\0\0\0\x03\x07\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\x01\0\0\0i\x01\0\0\0c\x02\0\0\0cd\x02\0\0\0{}";
    assert(payload.len == sizeof(expected) - 1u &&
        !memcmp(payload.data, expected, payload.len));
    struct snag_binary_graph_items items;
    assert(!snag_binary_graph_items_decode(payload.data, payload.len, &items) && items.count == 1u);
    struct snag_binary_graph_item decoded[3];
    size_t cursor = 0u;
    assert(!snag_binary_graph_items_next(&items, &cursor, &decoded[0]));
    assert(decoded[0].kind == SNAG_BINARY_ITEM_TOOL_CALL && decoded[0].data.call.id[0] == 7u &&
        decoded[0].data.call.arguments.size == 2u &&
        !memcmp(decoded[0].data.call.arguments.data, "{}", 2u));
    for (size_t i = 0u; i < payload.len; ++i) {
        assert_graph_items_rejected(payload.data, i);
    }
    const size_t mutations[] = {0u, 4u, 25u, 30u, 35u, 41u};
    for (size_t i = 0u; i < sizeof(mutations) / sizeof(mutations[0]); ++i) {
        unsigned char saved = payload.data[mutations[i]];
        payload.data[mutations[i]] = 0u;
        assert_graph_items_rejected(payload.data, payload.len);
        payload.data[mutations[i]] = saved;
    }
    size_t saved = payload.len;
    const char *invalid_arguments[] = {"[]", "null", "{\"a\":1,\"a\":2}", "{ \"a\":1}",
        "{\"b\":1,\"a\":2}", "{\"x\":0.5}", "{\"x\":\"\\u0000\"}"};
    for (size_t i = 0u; i < sizeof(invalid_arguments) / sizeof(invalid_arguments[0]); ++i) {
        values[0].data.call.arguments = text(invalid_arguments[i]);
        assert(snag_binary_graph_items_encode(&payload, values, 1u) < 0 && payload.len == saved);
    }
    values[0].data.call.arguments = text("{}");
    values[0].data.call.name = text("unknown_tool");
    assert(snag_binary_graph_items_encode(&payload, values, 1u) < 0 && payload.len == saved);
    values[0].data.call.name = text("cd");
    values[0].data.call.provider_call_id = text("\x1f");
    assert(snag_binary_graph_items_encode(&payload, values, 1u) < 0 && payload.len == saved);
    values[0].data.call.provider_call_id = text("c");
    test_graph_argument_limit(values[0]);
    values[1].kind = SNAG_BINARY_ITEM_ASSISTANT;
    values[1].data.output.item.kind = SNAG_BINARY_ITEM_ASSISTANT;
    values[1].data.output.item.phase = SNAG_BINARY_PHASE_COMMENTARY;
    values[1].data.output.item.id[0] = 9u;
    values[1].data.output.item.provider_id = text("p");
    values[1].data.output.item.text = text("speech");
    values[2] = values[1];
    values[2].data.output.item.id[0] = 10u;
    values[2].data.output.item.text = (struct snag_binary_text){0};
    values[2].data.output.source = (struct snag_binary_output_span){{2u, 79u, 1u}, 2u, 1u};
    snag_buf_reset(&payload);
    assert(!snag_binary_graph_items_encode(&payload, values, 3u));
    assert(!snag_binary_graph_items_decode(payload.data, payload.len, &items) && items.count == 3u);
    cursor = 0u;
    for (size_t i = 0u; i < 3u; ++i) {
        assert(!snag_binary_graph_items_next(&items, &cursor, &decoded[i]));
        assert(decoded[i].kind == values[i].kind);
    }
    assert(decoded[1].data.output.item.text.size == 6u &&
        decoded[2].data.output.source.first.sequence == 2u &&
        !decoded[2].data.output.item.text.data);
    struct snag_binary_graph_item kept = decoded[2];
    assert(snag_binary_graph_items_next(&items, &cursor, &decoded[2]) == 1);
    assert(!memcmp(&kept, &decoded[2], sizeof(kept)) && cursor == payload.len);
    cursor = 1u;
    assert(snag_binary_graph_items_next(&items, &cursor, &decoded[2]) < 0 && cursor == 1u);
    assert(!memcmp(&kept, &decoded[2], sizeof(kept)));
    struct snag_buf again = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_graph_items_encode(&again, decoded, 3u));
    assert(again.len == payload.len && !memcmp(again.data, payload.data, payload.len));
    saved = payload.len;
    values[1].data.output.item.kind = SNAG_BINARY_ITEM_REFUSAL;
    assert(snag_binary_graph_items_encode(&payload, values, 3u) < 0 && payload.len == saved);
    assert(snag_binary_graph_items_encode(&payload, NULL, 1u) < 0 && payload.len == saved);
    assert(snag_binary_graph_items_encode(&payload, values, SIZE_MAX) < 0 && payload.len == saved);
    assert(!snag_buf_append(&payload, "x", 1u));
    assert_graph_items_rejected(payload.data, payload.len);
    snag_buf_free(&again);
    snag_buf_free(&payload);
}

static void
assert_continuation_rejected(const void *data, size_t size, uint32_t count)
{
    struct snag_binary_continuation out = {(const unsigned char *)"keep", 4u};
    struct snag_binary_continuation saved = out;
    assert(snag_binary_continuation_decode(data, size, count, &out) < 0);
    assert(out.data == saved.data && out.size == saved.size);
}

static void
test_binary_continuation(void)
{
    struct snag_binary_continuation_item values[2] = {
        {1u, text("{\"type\":\"reasoning\"}")}, {1u, text("{\"type\":\"reasoning\"}")}
    };
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_continuation_encode(&payload, values, 1u, 3u));
    static const unsigned char expected[] =
        "\x01\0\0\0\x01\0\0\0\0\0\0\0\x14\0\0\0{\"type\":\"reasoning\"}";
    assert(payload.len == sizeof(expected) - 1u &&
        !memcmp(payload.data, expected, payload.len));
    struct snag_binary_continuation items;
    assert(!snag_binary_continuation_decode(payload.data, payload.len, 3u, &items));
    size_t cursor = 0u;
    struct snag_binary_continuation_item decoded;
    assert(!snag_binary_continuation_next(&items, &cursor, &decoded));
    assert(decoded.before == 1u && decoded.item.size == 20u &&
        !memcmp(decoded.item.data, values[0].item.data, 20u));
    struct snag_binary_continuation_item kept = decoded;
    assert(snag_binary_continuation_next(&items, &cursor, &decoded) == 1);
    assert(!memcmp(&kept, &decoded, sizeof(kept)) && cursor == payload.len);
    for (size_t i = 0u; i < payload.len; ++i) {
        assert_continuation_rejected(payload.data, i, 3u);
    }
    assert_continuation_rejected(payload.data, payload.len, 0u);
    size_t saved = payload.len;
    const char *bad[] = {"{}", "[]", "{\"type\":\"assistant\"}",
        "{\"id\":\"\",\"type\":\"reasoning\"}", "{\"status\":\"running\",\"type\":\"reasoning\"}",
        "{\"content\":[{\"text\":\"x\",\"type\":\"bad\"}],\"type\":\"reasoning\"}"};
    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        values[0].item = text(bad[i]);
        assert(snag_binary_continuation_encode(&payload, values, 1u, 3u) < 0 &&
            payload.len == saved);
    }
    values[0].item = text("{\"custom\":{\"k\":1},\"status\":null,"
        "\"summary\":[{\"text\":\"x\",\"type\":\"summary_text\"}],\"type\":\"reasoning\"}");
    snag_buf_reset(&payload);
    assert(!snag_binary_continuation_encode(&payload, values, 2u, 3u));
    assert(!snag_binary_continuation_decode(payload.data, payload.len, 3u, &items));
    cursor = 0u;
    assert(!snag_binary_continuation_next(&items, &cursor, &decoded));
    assert(decoded.item.size == values[0].item.size &&
        !memcmp(decoded.item.data, values[0].item.data, decoded.item.size));
    size_t second_offset = cursor;
    assert(!snag_binary_continuation_next(&items, &cursor, &decoded));
    payload.data[second_offset] = 0u;
    assert_continuation_rejected(payload.data, payload.len, 3u);
    payload.data[second_offset] = 1u;
    saved = payload.len;
    values[1].before = 0u;
    assert(snag_binary_continuation_encode(&payload, values, 2u, 3u) < 0 && payload.len == saved);
    values[1].before = UINT64_MAX;
    assert(snag_binary_continuation_encode(&payload, values, 2u, UINT32_MAX) < 0 &&
        payload.len == saved);
    assert(!snag_buf_append(&payload, "x", 1u));
    assert_continuation_rejected(payload.data, payload.len, 3u);
    snag_buf_reset(&payload);
    assert(!snag_binary_continuation_encode(&payload, NULL, 0u, 0u));
    assert(!snag_binary_continuation_decode(payload.data, payload.len, 0u, &items));
    cursor = 0u;
    assert(snag_binary_continuation_next(&items, &cursor, &decoded) == 1 && !cursor);
    snag_buf_free(&payload);
}

static void
assert_graph_reference_rejected(const struct snag_binary_ref *reference,
    const struct snag_binary_batch *batch, enum snag_binary_item_kind kind)
{
    struct snag_binary_graph_source source;
    memset(&source, 0x5a, sizeof(source));
    unsigned char saved[sizeof(source)];
    memcpy(saved, &source, sizeof(source));
    assert(snag_binary_graph_ref_resolve(reference, batch, kind, &source) < 0);
    assert(!memcmp(saved, &source, sizeof(source)));
}

static void
assert_continuation_reference_rejected(const struct snag_binary_ref *reference,
    const struct snag_binary_batch *batch)
{
    struct snag_binary_continuation_source source;
    memset(&source, 0x5a, sizeof(source));
    unsigned char saved[sizeof(source)];
    memcpy(saved, &source, sizeof(source));
    assert(snag_binary_continuation_ref_resolve(reference, batch, &source) < 0);
    assert(!memcmp(saved, &source, sizeof(source)));
}

static void
assert_complete_originals_missing(const struct snag_binary_batch *batch, uint64_t sequence)
{
    struct snag_binary_ref reference = {99u, 98u, 97u};
    assert(snag_binary_graph_ref_create(batch, sequence, 0u,
        SNAG_BINARY_ITEM_TOOL_CALL, &reference) < 0);
    assert(reference.sequence == 99u && reference.offset == 98u && reference.size == 97u);
    assert(snag_binary_continuation_ref_create(batch, sequence, &reference) < 0);
    assert(reference.sequence == 99u && reference.offset == 98u && reference.size == 97u);
    reference = (struct snag_binary_ref){sequence, 45u, 39u};
    assert_graph_reference_rejected(&reference, batch, SNAG_BINARY_ITEM_TOOL_CALL);
    reference = (struct snag_binary_ref){sequence, 204u, 36u};
    assert_continuation_reference_rejected(&reference, batch);
}

static void
test_completed_references(void)
{
    struct snag_binary_graph_item values[3] = {0};
    values[0].kind = SNAG_BINARY_ITEM_TOOL_CALL;
    values[0].data.call.id[0] = 7u;
    values[0].data.call.provider_id = text("i");
    values[0].data.call.provider_call_id = text("c");
    values[0].data.call.name = text("cd");
    values[0].data.call.arguments = text("{}");
    values[1].kind = SNAG_BINARY_ITEM_ASSISTANT;
    values[1].data.output.item.kind = SNAG_BINARY_ITEM_ASSISTANT;
    values[1].data.output.item.phase = SNAG_BINARY_PHASE_COMMENTARY;
    values[1].data.output.item.id[0] = 8u;
    values[1].data.output.item.provider_id = text("{}");
    values[1].data.output.item.text = text("{}");
    values[2] = values[1];
    values[2].data.output.item.id[0] = 9u;
    values[2].data.output.item.text = (struct snag_binary_text){0};
    values[2].data.output.source = (struct snag_binary_output_span){{1u, 79u, 1u}, 1u, 1u};
    struct snag_buf graph = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_graph_items_encode(&graph, values, 3u));
    struct snag_binary_continuation_item reasoning = {2u, text("{\"type\":\"reasoning\"}")};
    struct snag_buf continuation = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_continuation_encode(&continuation, &reasoning, 1u, 3u));
    struct snag_binary_event event = {.kind = SNAG_BINARY_RESPONSE_COMPLETED};
    struct snag_binary_response_complete *complete = &event.data.response_completed;
    complete->turn[0] = 1u;
    complete->response[0] = 2u;
    complete->cycle = 1u;
    complete->provider_id = text("r");
    complete->items = (struct snag_binary_graph_items){graph.data, graph.len, 3u};
    complete->continuation_form = SNAG_BINARY_CONTINUATION_ITEMS;
    complete->continuation_scope[0] = 9u;
    complete->continuation = (struct snag_binary_continuation){continuation.data, continuation.len};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event) && payload.len == 240u);
    struct snag_binary_record record = {.kind = (uint16_t)event.kind, .version = 1u,
        .payload = payload.data, .size = payload.len};
    struct snag_binary_batch batch;
    input_reference_batch(&record, &bytes, &batch);
    struct snag_binary_ref call_ref, text_ref, continuation_ref;
    assert(!snag_binary_graph_ref_create(&batch, 2u, 0u, SNAG_BINARY_ITEM_TOOL_CALL, &call_ref));
    assert(call_ref.sequence == 2u && call_ref.offset == 45u && call_ref.size == 39u);
    assert(!snag_binary_graph_ref_create(&batch, 2u, 1u, SNAG_BINARY_ITEM_ASSISTANT, &text_ref));
    assert(text_ref.sequence == 2u && text_ref.offset == 84u && text_ref.size == 30u);
    struct snag_binary_graph_source source;
    assert(!snag_binary_graph_ref_resolve(&call_ref, &batch, SNAG_BINARY_ITEM_TOOL_CALL, &source));
    assert(source.cycle == 1u && source.index == 0u && source.item.data.call.id[0] == 7u &&
        !memcmp(source.turn, complete->turn, 16u) &&
        !memcmp(source.response, complete->response, 16u));
    assert(source.item.data.call.arguments.size == 2u &&
        !memcmp(source.item.data.call.arguments.data, "{}", 2u));
    assert(!snag_binary_graph_ref_resolve(&text_ref, &batch, SNAG_BINARY_ITEM_ASSISTANT, &source));
    assert(source.index == 1u && source.item.data.output.item.id[0] == 8u &&
        source.item.data.output.item.text.size == 2u &&
        !memcmp(source.item.data.output.item.text.data, "{}", 2u));
    assert(!snag_binary_continuation_ref_create(&batch, 2u, &continuation_ref));
    assert(continuation_ref.sequence == 2u && continuation_ref.offset == 204u &&
        continuation_ref.size == 36u);
    struct snag_binary_continuation_source retained;
    assert(!snag_binary_continuation_ref_resolve(&continuation_ref, &batch, &retained));
    assert(retained.cycle == 1u && !memcmp(retained.turn, complete->turn, 16u) &&
        !memcmp(retained.response, complete->response, 16u) &&
        !memcmp(retained.scope, complete->continuation_scope, 32u) &&
        retained.items.size == continuation.len &&
        !memcmp(retained.items.data, continuation.data, continuation.len));
    const struct snag_binary_ref bad[] = {
        {2u, 0u, 39u}, {2u, 45u, 0u}, {2u, 45u, 38u}, {2u, 46u, 38u}, {2u, 44u, 40u},
        {2u, 45u, 40u}, {2u, 82u, 2u}, {2u, 106u, 2u}, {2u, 112u, 2u}, {2u, 114u, 56u},
        {2u, UINT32_MAX, 39u}, {2u, 45u, UINT32_MAX}, {0u, 45u, 39u}, {1u, 45u, 39u},
        {3u, 45u, 39u}, {UINT64_MAX, 45u, 39u}, {2u, 204u, 35u}, {2u, 205u, 35u},
        {2u, 220u, 20u}, {2u, 204u, 0u}, {2u, 204u, UINT32_MAX}
    };
    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        assert_graph_reference_rejected(&bad[i], &batch, SNAG_BINARY_ITEM_TOOL_CALL);
        assert_graph_reference_rejected(&bad[i], &batch, SNAG_BINARY_ITEM_ASSISTANT);
        assert_continuation_reference_rejected(&bad[i], &batch);
    }
    assert_graph_reference_rejected(&text_ref, &batch, SNAG_BINARY_ITEM_TOOL_CALL);
    assert_graph_reference_rejected(&text_ref, &batch, SNAG_BINARY_ITEM_REFUSAL);
    assert_graph_reference_rejected(&call_ref, &batch, (enum snag_binary_item_kind)0);
    assert_graph_reference_rejected(&continuation_ref, &batch, SNAG_BINARY_ITEM_TOOL_CALL);
    assert_continuation_reference_rejected(&call_ref, &batch);
    assert_continuation_reference_rejected(&text_ref, &batch);
    assert_output_reference_rejected(&text_ref, &batch);
    assert_input_leaf_missing(&batch, 2u, SNAG_BINARY_INPUT_TEXT);
    assert_input_leaf_missing(&batch, 2u, SNAG_BINARY_INPUT_CONTENT);
    struct snag_binary_ref kept = {99u, 98u, 97u};
    const uint32_t indexes[] = {0u, 2u, 3u, UINT32_MAX};
    for (size_t i = 0u; i < sizeof(indexes) / sizeof(indexes[0]); ++i) {
        assert(snag_binary_graph_ref_create(&batch, 2u, indexes[i],
            SNAG_BINARY_ITEM_ASSISTANT, &kept) < 0);
        assert(kept.sequence == 99u && kept.offset == 98u && kept.size == 97u);
    }
    assert_graph_reference_rejected(NULL, &batch, SNAG_BINARY_ITEM_TOOL_CALL);
    assert_continuation_reference_rejected(NULL, &batch);
    assert(snag_binary_graph_ref_create(&batch, 2u, 0u, SNAG_BINARY_ITEM_TOOL_CALL, NULL) < 0);
    assert(snag_binary_graph_ref_resolve(&call_ref, &batch, SNAG_BINARY_ITEM_TOOL_CALL, NULL) < 0);
    assert(snag_binary_continuation_ref_create(&batch, 2u, NULL) < 0);
    assert(snag_binary_continuation_ref_resolve(&continuation_ref, &batch, NULL) < 0);
    assert_complete_originals_missing(NULL, 2u);
    assert_complete_originals_missing(&batch, 0u);
    assert_complete_originals_missing(&batch, 1u);
    assert_complete_originals_missing(&batch, 3u);
    assert_complete_originals_missing(&batch, UINT64_MAX);
    for (size_t i = 0u; i < payload.len; ++i) {
        record.size = i;
        input_reference_batch(&record, &bytes, &batch);
        assert_complete_originals_missing(&batch, 2u);
    }
    record.size = payload.len;
    const size_t positions[] = {32u, 40u, 45u, 66u, 82u, 170u, 208u, 239u};
    const unsigned char corrupt[] = {0u, '\n', 0u, '\n', '[', 64u, 4u, 'x'};
    for (size_t i = 0u; i < sizeof(positions) / sizeof(positions[0]); ++i) {
        unsigned char saved = payload.data[positions[i]];
        payload.data[positions[i]] = corrupt[i];
        input_reference_batch(&record, &bytes, &batch);
        assert_complete_originals_missing(&batch, 2u);
        payload.data[positions[i]] = saved;
    }
    record.version = 2u;
    input_reference_batch(&record, &bytes, &batch);
    assert_complete_originals_missing(&batch, 2u);
    record.version = 1u;
    record.flags = SNAG_BINARY_RECORD_OPTIONAL;
    input_reference_batch(&record, &bytes, &batch);
    assert_complete_originals_missing(&batch, 2u);
    record.flags = 0u;
    record.kind = SNAG_BINARY_RESPONSE_OUTPUT;
    input_reference_batch(&record, &bytes, &batch);
    assert_complete_originals_missing(&batch, 2u);
    record.kind = (uint16_t)event.kind;
    for (unsigned i = 0u; i < 2u; ++i) {
        complete->continuation_form = (enum snag_binary_continuation_form)i;
        snag_buf_reset(&payload);
        assert(!snag_binary_event_encode(&payload, &event));
        record.payload = payload.data;
        record.size = payload.len;
        input_reference_batch(&record, &bytes, &batch);
        assert(snag_binary_continuation_ref_create(&batch, 2u, &kept) < 0);
        assert(kept.sequence == 99u && kept.offset == 98u && kept.size == 97u);
        assert_continuation_reference_rejected(&continuation_ref, &batch);
        assert(!snag_binary_graph_ref_resolve(&call_ref, &batch,
            SNAG_BINARY_ITEM_TOOL_CALL, &source));
    }
    unsigned char empty[4] = {0};
    complete->continuation_form = SNAG_BINARY_CONTINUATION_ITEMS;
    complete->continuation = (struct snag_binary_continuation){empty, sizeof(empty)};
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    record.payload = payload.data;
    record.size = payload.len;
    input_reference_batch(&record, &bytes, &batch);
    assert(!snag_binary_continuation_ref_create(&batch, 2u, &continuation_ref));
    assert(continuation_ref.offset == 204u && continuation_ref.size == 4u);
    assert(!snag_binary_continuation_ref_resolve(&continuation_ref, &batch, &retained));
    assert(retained.items.size == 4u && !memcmp(retained.items.data, empty, 4u));
    continuation_ref.offset = 180u;
    assert_continuation_reference_rejected(&continuation_ref, &batch);
    payload.data[84u] = SNAG_BINARY_ITEM_REFUSAL;
    payload.data[85u] = SNAG_BINARY_PHASE_FINAL_ANSWER;
    input_reference_batch(&record, &bytes, &batch);
    assert(!snag_binary_graph_ref_create(&batch, 2u, 1u, SNAG_BINARY_ITEM_REFUSAL, &text_ref));
    assert(!snag_binary_graph_ref_resolve(&text_ref, &batch, SNAG_BINARY_ITEM_REFUSAL, &source));
    assert(source.index == 1u &&
        source.item.data.output.item.phase == SNAG_BINARY_PHASE_FINAL_ANSWER);
    assert_graph_reference_rejected(&text_ref, &batch, SNAG_BINARY_ITEM_ASSISTANT);
    snag_buf_free(&bytes);
    snag_buf_free(&payload);
    snag_buf_free(&continuation);
    snag_buf_free(&graph);
}

static void
assert_irc_encode_rejected(const struct snag_binary_event *event)
{
    struct snag_buf out = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_buf_append(&out, "keep", 4u));
    assert(snag_binary_event_encode(&out, event) < 0);
    assert(out.len == 4u && !memcmp(out.data, "keep", 4u));
    snag_buf_free(&out);
}

static void
copy_irc_text(char *out, size_t capacity, struct snag_binary_text value)
{
    assert(value.size < capacity);
    if (value.size) memcpy(out, value.data, value.size);
    out[value.size] = '\0';
}

static void
assert_irc_legacy(const struct snag_binary_irc_event *value)
{
    struct snag_irc_event old = {
        .kind = (enum snag_irc_event_kind)(value->kind - 1),
        .timestamp_ms = value->timestamp_ms, .sequence = value->sequence,
        .historical = value->historical, .local = value->is_local, .op = value->op,
        .input = value->input, .classified = value->classified,
        .urgent = value->urgent, .reply = value->reply
    };
    copy_irc_text(old.endpoint, sizeof(old.endpoint), value->endpoint);
    copy_irc_text(old.room, sizeof(old.room), value->room);
    copy_irc_text(old.nick, sizeof(old.nick), value->nick);
    copy_irc_text(old.text, sizeof(old.text), value->text);
    if (value->has_stream) {
        static const char hex[] = "0123456789abcdef";
        for (size_t i = 0u; i < 16u; ++i) {
            old.stream[i * 2u] = hex[value->stream[i] >> 4u];
            old.stream[i * 2u + 1u] = hex[value->stream[i] & 15u];
        }
    }
    json_t *data = snag_irc_event_data(&old);
    assert(data);
    if (!value->has_watermark) {
        assert(!json_object_del(data, "stream"));
        assert(!json_object_del(data, "sequence"));
        assert(!json_object_del(data, "input"));
    }
    struct snag_irc_event parsed;
    assert(!snag_irc_event_read(data, &parsed));
    assert(parsed.kind == old.kind && parsed.timestamp_ms == old.timestamp_ms &&
        parsed.sequence == old.sequence && !strcmp(parsed.stream, old.stream));
    assert(parsed.historical == old.historical && parsed.local == old.local && parsed.op == old.op);
    assert(parsed.input == old.input && parsed.classified == old.classified &&
        parsed.urgent == old.urgent && parsed.reply == old.reply);
    assert(!strcmp(parsed.endpoint, old.endpoint) && !strcmp(parsed.room, old.room) &&
        !strcmp(parsed.nick, old.nick) && !strcmp(parsed.text, old.text));
    json_decref(data);
}

static void
test_irc_event_limits(void)
{
    struct snag_binary_event event = {.kind = SNAG_BINARY_IRC_EVENT};
    struct snag_binary_irc_event *irc = &event.data.irc_event;
    irc->kind = SNAG_BINARY_IRC_MESSAGE;
    irc->timestamp_ms = 1u;
    irc->endpoint = text("h");
    struct snag_binary_text *fields[] = {&irc->endpoint, &irc->room, &irc->nick, &irc->text};
    const size_t maximum[] = {255u, 51u, 30u, 4096u};
    unsigned char bytes[4097];
    memset(bytes, 'x', sizeof(bytes));
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    for (size_t i = 0u; i < 4u; ++i) {
        struct snag_binary_text saved = *fields[i];
        *fields[i] = (struct snag_binary_text){bytes, maximum[i]};
        snag_buf_reset(&payload);
        assert(!snag_binary_event_encode(&payload, &event));
        struct snag_binary_record record = {.kind = 208u, .version = 1u,
            .payload = payload.data, .size = payload.len};
        struct snag_binary_event decoded;
        assert(!snag_binary_event_decode(&record, &decoded));
        struct snag_binary_irc_event *parsed = &decoded.data.irc_event;
        struct snag_binary_text views[] = {
            parsed->endpoint, parsed->room, parsed->nick, parsed->text
        };
        assert(views[i].size == maximum[i] && !memcmp(views[i].data, bytes, maximum[i]));
        assert_irc_legacy(parsed);
        size_t length_offset = (size_t)(views[i].data - payload.data) - 4u;
        size_t end = length_offset + 4u + maximum[i];
        assert(!snag_buf_putc(&payload, 'x'));
        memmove(payload.data + end + 1u, payload.data + end, payload.len - end - 1u);
        payload.data[end] = 'x';
        for (size_t j = 0u; j < 4u; ++j) {
            payload.data[length_offset + j] = (unsigned char)((maximum[i] + 1u) >> (8u * j));
        }
        record.payload = payload.data;
        record.size = payload.len;
        assert_rejected(record);
        fields[i]->size++;
        assert_irc_encode_rejected(&event);
        const char *bad[] = {"\n", "\r", "\t", "\x7f", "\xff", "\xc0\x80"};
        for (size_t j = 0u; j < sizeof(bad) / sizeof(bad[0]); ++j) {
            *fields[i] = text(bad[j]);
            assert_irc_encode_rejected(&event);
        }
        *fields[i] = (struct snag_binary_text){(const unsigned char *)"a\0b", 3u};
        assert_irc_encode_rejected(&event);
        *fields[i] = text("\xc2\x80"); /* Historical admission permits non-ASCII controls. */
        roundtrip(&event);
        assert_irc_legacy(irc);
        *fields[i] = saved;
    }
    irc->endpoint = text("");
    assert_irc_encode_rejected(&event);
    snag_buf_free(&payload);
}

static void
test_irc_event(void)
{
    struct snag_binary_event event = {.kind = SNAG_BINARY_IRC_EVENT};
    struct snag_binary_irc_event *irc = &event.data.irc_event;
    *irc = (struct snag_binary_irc_event){.kind = SNAG_BINARY_IRC_MESSAGE, .timestamp_ms = 1u,
        .sequence = 7u, .stream = {2u}, .endpoint = text("h:1"), .room = text("#r"),
        .nick = text("n"), .text = text("hi"), .historical = true, .is_local = true, .op = true,
        .has_watermark = true, .has_stream = true, .input = true, .classified = true,
        .urgent = true, .reply = true};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    static const unsigned char expected[59] = {
        [0] = 7u, [1] = 255u, [2] = 1u, [3] = 1u, [11] = 2u, [27] = 7u,
        [35] = 3u, [39] = 'h', [40] = ':', [41] = '1', [42] = 2u, [46] = '#', [47] = 'r',
        [48] = 1u, [52] = 'n', [53] = 2u, [57] = 'h', [58] = 'i'
    };
    assert(payload.len == sizeof(expected) && !memcmp(payload.data, expected, sizeof(expected)));
    assert(SNAG_BINARY_IRC_EVENT == 208 && snag_binary_event_version(event.kind) == 1u);
    roundtrip(&event);
    struct snag_binary_record record = {.kind = 208u, .version = 1u,
        .payload = payload.data, .size = payload.len};
    const struct { size_t offset; unsigned char value; } bad[] = {
        {0u, 0u}, {0u, 12u}, {2u, 2u}, {3u, 0u}, {10u, 128u}, {27u, 0u}, {34u, 128u},
        {1u, 247u}, {1u, 191u}, {39u, '\n'}, {46u, '\t'}, {52u, 127u}, {58u, 255u}
    };
    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        unsigned char saved = payload.data[bad[i].offset];
        payload.data[bad[i].offset] = bad[i].value;
        assert_rejected(record);
        payload.data[bad[i].offset] = saved;
    }
    for (unsigned int flags = 0u; flags < 512u; ++flags) {
        irc->historical = (flags & 1u) != 0u;
        irc->is_local = (flags & 2u) != 0u;
        irc->op = (flags & 4u) != 0u;
        irc->has_watermark = (flags & 8u) != 0u;
        irc->has_stream = (flags & 16u) != 0u;
        irc->input = (flags & 32u) != 0u;
        irc->classified = (flags & 64u) != 0u;
        irc->urgent = (flags & 128u) != 0u;
        irc->reply = (flags & 256u) != 0u;
        irc->sequence = irc->has_stream ? 7u : 0u;
        bool valid = ((flags & 8u) || !(flags & 48u)) && ((flags & 64u) || !(flags & 384u));
        if (!valid) {
            assert_irc_encode_rejected(&event);
            continue;
        }
        roundtrip(&event);
        assert_irc_legacy(irc);
    }
    static const char *const names[] = {"connected", "disconnected", "join", "part", "quit",
        "nick", "message", "notice", "topic", "mode", "history_ready"};
    for (size_t i = 0u; i < sizeof(names) / sizeof(names[0]); ++i) {
        irc->kind = (enum snag_binary_irc_kind)(i + 1u);
        assert(!strcmp(snag_irc_kind_name((enum snag_irc_event_kind)i), names[i]));
        roundtrip(&event);
        assert_irc_legacy(irc);
    }
    irc->kind = 0;
    assert_irc_encode_rejected(&event);
    irc->kind = SNAG_BINARY_IRC_MESSAGE;
    irc->timestamp_ms = 0u;
    assert_irc_encode_rejected(&event);
    irc->timestamp_ms = (uint64_t)INT64_MAX + 1u;
    assert_irc_encode_rejected(&event);
    irc->timestamp_ms = INT64_MAX;
    irc->sequence = INT64_MAX;
    roundtrip(&event);
    assert_irc_legacy(irc);
    irc->sequence++;
    assert_irc_encode_rejected(&event);
    irc->sequence = 0u;
    assert_irc_encode_rejected(&event);
    irc->has_stream = false;
    irc->sequence = 1u;
    assert_irc_encode_rejected(&event);
    snag_buf_free(&payload);
    test_irc_event_limits();
}

static void
test_irc_snapshot(void)
{
    struct snag_binary_event event = {.kind = SNAG_BINARY_IRC_SNAPSHOT};
    struct snag_binary_irc_snapshot *snapshot = &event.data.irc_snapshot;
    *snapshot = (struct snag_binary_irc_snapshot){.reason = SNAG_BINARY_IRC_SNAPSHOT_JOIN,
        .timestamp_ms = 513u, .text = text("hi\n")};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    static const unsigned char expected[16] = {
        [0] = 1u, [1] = 1u, [2] = 2u, [9] = 3u, [13] = 'h', [14] = 'i', [15] = '\n'
    };
    assert(payload.len == sizeof(expected) && !memcmp(payload.data, expected, sizeof(expected)));
    assert(SNAG_BINARY_IRC_SNAPSHOT == 209 && snag_binary_event_version(event.kind) == 1u);
    for (unsigned int reason = 1u; reason <= 4u; ++reason) {
        snapshot->reason = (enum snag_binary_irc_snapshot_reason)reason;
        roundtrip(&event);
    }
    struct snag_binary_record record = {.kind = 209u, .version = 1u,
        .payload = payload.data, .size = payload.len};
    const struct { size_t offset; unsigned char value; } bad[] = {
        {0u, 0u}, {0u, 5u}, {8u, 128u}, {9u, 0u}, {13u, 0u}, {13u, 255u}
    };
    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        unsigned char saved = payload.data[bad[i].offset];
        payload.data[bad[i].offset] = bad[i].value;
        assert_rejected(record);
        payload.data[bad[i].offset] = saved;
    }
    snapshot->reason = 0;
    assert_irc_encode_rejected(&event);
    snapshot->reason = SNAG_BINARY_IRC_SNAPSHOT_NICK;
    snapshot->timestamp_ms = 0u;
    assert_irc_encode_rejected(&event);
    snapshot->timestamp_ms = (uint64_t)INT64_MAX + 1u;
    assert_irc_encode_rejected(&event);
    snapshot->timestamp_ms = INT64_MAX;
    roundtrip(&event);
    const char *invalid[] = {"", "\xff", "\xc0\x80"};
    for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        snapshot->text = text(invalid[i]);
        assert_irc_encode_rejected(&event);
    }
    snapshot->text = (struct snag_binary_text){(const unsigned char *)"a\0b", 3u};
    assert_irc_encode_rejected(&event);
    unsigned char *large = malloc(SNAG_MAX_IRC_SNAPSHOT + 1u);
    assert(large);
    memset(large, 'x', SNAG_MAX_IRC_SNAPSHOT + 1u);
    snapshot->text = (struct snag_binary_text){large, SNAG_MAX_IRC_SNAPSHOT};
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    record.payload = payload.data;
    record.size = payload.len;
    struct snag_binary_event decoded;
    assert(!snag_binary_event_decode(&record, &decoded));
    assert(decoded.data.irc_snapshot.text.size == SNAG_MAX_IRC_SNAPSHOT &&
        !memcmp(decoded.data.irc_snapshot.text.data, large, SNAG_MAX_IRC_SNAPSHOT));
    snapshot->text.size++;
    assert_irc_encode_rejected(&event);
    assert(!snag_buf_putc(&payload, 'x'));
    payload.data[9] = 1u; /* Complete text, one byte beyond the existing 8 MiB bound. */
    record.payload = payload.data;
    record.size = payload.len;
    assert_rejected(record);
    free(large);
    snag_buf_free(&payload);
}

static void
assert_sequences_rejected(const void *data, size_t size)
{
    struct snag_binary_sequences sentinel = {(const unsigned char *)"keep", 4u};
    struct snag_binary_sequences out = sentinel;
    assert(snag_binary_sequences_decode(data, size, &out) < 0);
    assert(out.data == sentinel.data && out.size == sentinel.size);
}

static void
test_sequences_large(void)
{
    /* This accepted JSON list exceeds the frame bound if widened to fixed64. */
    const size_t count = 2100000u;
    uint64_t *values = malloc(count * sizeof(*values));
    assert(values);
    size_t json_bytes = 2u;
    for (size_t i = 0u; i < count; ++i) {
        values[i] = i + 1u;
        json_bytes += i != 0u;
        for (uint64_t value = values[i]; value; value /= 10u) json_bytes++;
    }
    assert(json_bytes == 15688897u && json_bytes < SNAG_MAX_EVENT_LINE);
    assert(count * 8u + 4u > SNAG_MAX_EVENT_LINE);
    struct snag_buf encoded = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_sequences_encode(&encoded, values, count));
    assert(encoded.len == 6286343u);
    struct snag_binary_event event = {.kind = SNAG_BINARY_IRC_ADMITTED};
    assert(!snag_binary_sequences_decode(encoded.data, encoded.len,
        &event.data.irc_admitted.sequences));
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    struct snag_binary_record record = {.kind = 210u, .version = 1u,
        .payload = payload.data, .size = payload.len};
    struct snag_binary_event decoded;
    assert(!snag_binary_event_decode(&record, &decoded));
    size_t offset = 0u;
    uint64_t value = 0u;
    for (size_t i = 0u; i < count; ++i) {
        assert(!snag_binary_sequences_next(&decoded.data.irc_admitted.sequences, &offset, &value));
        assert(value == values[i]);
    }
    assert(snag_binary_sequences_next(&decoded.data.irc_admitted.sequences, &offset, &value) == 1);
    assert(value == count && offset == encoded.len);
    free(values);
    snag_buf_free(&payload);
    snag_buf_free(&encoded);
}

static void
test_sequences(void)
{
    uint64_t values[] = {1u, 128u, INT64_MAX};
    struct snag_buf encoded = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_sequences_encode(&encoded, values, 3u));
    static const unsigned char expected[] = {
        3u, 0u, 0u, 0u, 1u, 128u, 1u,
        255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u, 127u
    };
    assert(encoded.len == sizeof(expected) && !memcmp(encoded.data, expected, sizeof(expected)));
    struct snag_binary_sequences sequences;
    assert(!snag_binary_sequences_decode(encoded.data, encoded.len, &sequences));
    size_t offset = 0u;
    uint64_t value = 77u;
    for (size_t i = 0u; i < 3u; ++i) {
        assert(!snag_binary_sequences_next(&sequences, &offset, &value));
        assert(value == values[i]);
    }
    assert(snag_binary_sequences_next(&sequences, &offset, &value) == 1);
    assert(offset == encoded.len && value == INT64_MAX);
    offset = 2u;
    assert(snag_binary_sequences_next(&sequences, &offset, &value) < 0);
    assert(offset == 2u && value == INT64_MAX);
    offset = sequences.size + 1u;
    assert(snag_binary_sequences_next(&sequences, &offset, &value) < 0);
    assert(offset == sequences.size + 1u && value == INT64_MAX);
    assert(snag_binary_sequences_next(NULL, &offset, &value) < 0);
    assert(snag_binary_sequences_next(&sequences, NULL, &value) < 0);
    assert(snag_binary_sequences_next(&sequences, &offset, NULL) < 0);
    for (size_t i = 0u; i < encoded.len; ++i) {
        assert_sequences_rejected(encoded.data, i);
    }
    const unsigned char bad[][14] = {
        {0u, 0u, 0u, 0u}, {1u, 0u, 0u, 0u, 0u}, {1u, 0u, 0u, 0u, 129u, 0u},
        {1u, 0u, 0u, 0u, 128u}, {2u, 0u, 0u, 0u, 2u, 1u}, {2u, 0u, 0u, 0u, 1u, 1u},
        {255u, 255u, 255u, 255u, 1u}, {1u, 0u, 0u, 0u, 1u, 2u},
        {1u, 0u, 0u, 0u, 255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u, 1u}
    };
    const size_t sizes[] = {4u, 5u, 6u, 5u, 6u, 6u, 5u, 6u, 14u};
    for (size_t i = 0u; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        assert_sequences_rejected(bad[i], sizes[i]);
    }
    struct snag_binary_sequences truncated = {bad[3], sizes[3]};
    offset = 0u;
    assert(snag_binary_sequences_next(&truncated, &offset, &value) < 0);
    assert(!offset && value == INT64_MAX);
    assert_sequences_rejected(NULL, 1u);
    assert(snag_binary_sequences_decode(encoded.data, encoded.len, NULL) < 0);
    assert(snag_binary_sequences_encode(NULL, values, 3u) < 0);
    assert(snag_binary_sequences_encode(&encoded, NULL, 1u) < 0);
    assert(snag_binary_sequences_encode(&encoded, values, 0u) < 0);
    assert(snag_binary_sequences_encode(&encoded, values, SIZE_MAX) < 0);
    values[1] = 0u;
    assert(snag_binary_sequences_encode(&encoded, values, 3u) < 0);
    values[1] = 1u;
    assert(snag_binary_sequences_encode(&encoded, values, 3u) < 0);
    values[1] = (uint64_t)INT64_MAX + 1u;
    assert(snag_binary_sequences_encode(&encoded, values, 3u) < 0);
    assert(encoded.len == sizeof(expected) && !memcmp(encoded.data, expected, sizeof(expected)));
    snag_buf_free(&encoded);
    test_sequences_large();
}

static void
test_irc_admission(void)
{
    struct snag_buf sequences = {.max = SNAG_MAX_EVENT_LINE};
    uint64_t sequence = 1u;
    assert(!snag_binary_sequences_encode(&sequences, &sequence, 1u));
    struct snag_binary_event event = {.kind = SNAG_BINARY_IRC_ADMITTED};
    struct snag_binary_irc_admission *admission = &event.data.irc_admitted;
    admission->sequences = (struct snag_binary_sequences){sequences.data, sequences.len};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    static const unsigned char empty[7] = {[0] = 1u, [4] = 1u};
    assert(payload.len == sizeof(empty) && !memcmp(payload.data, empty, sizeof(empty)));
    assert(SNAG_BINARY_IRC_ADMITTED == 210 && snag_binary_event_version(event.kind) == 1u);
    roundtrip(&event);
    admission->input.size = 1u;
    assert_irc_encode_rejected(&event);
    admission->input.size = 0u;
    struct snag_binary_event steering = {.kind = SNAG_BINARY_STEERING_ADDED};
    steering.data.steering_input.id[0] = 'h';
    steering.data.steering_input.id[1] = 'i';
    steering.data.steering_input.turn[0] = 5u;
    steering.data.steering_input.text = text("hi");
    struct snag_buf nested = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&nested, &steering));
    admission->input = (struct snag_binary_record){.kind = SNAG_BINARY_STEERING_ADDED,
        .version = 2u, .payload = nested.data, .size = nested.len};
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    static const unsigned char expected[52] = {
        [0] = 1u, [4] = 1u, [5] = 98u, [7] = 2u, [9] = 39u,
        [13] = 'h', [14] = 'i', [29] = 5u, [46] = 2u, [50] = 'h', [51] = 'i'
    };
    assert(payload.len == sizeof(expected) && !memcmp(payload.data, expected, sizeof(expected)));
    roundtrip(&event);
    struct snag_binary_record record = {.kind = 210u, .version = 1u,
        .payload = payload.data, .size = payload.len};
    struct snag_buf batch_bytes = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_batch batch;
    input_reference_batch(&record, &batch_bytes, &batch);
    struct snag_binary_ref reference;
    assert(!snag_binary_input_ref_create(&batch, 2u, SNAG_BINARY_INPUT_TEXT, &reference));
    assert(reference.sequence == 2u && reference.offset == 50u && reference.size == 2u);
    const unsigned char *view;
    assert(!snag_binary_input_ref_resolve(&reference, &batch, SNAG_BINARY_INPUT_TEXT, &view));
    assert(!memcmp(view, "hi", 2u));
    struct snag_binary_ref wrong = {2u, 13u, 2u}; /* Equal bytes in the embedded ID. */
    assert_input_reference_rejected(&wrong, &batch, SNAG_BINARY_INPUT_TEXT);
    wrong = (struct snag_binary_ref){2u, 51u, 1u};
    assert_input_reference_rejected(&wrong, &batch, SNAG_BINARY_INPUT_TEXT);
    assert_input_leaf_missing(&batch, 2u, SNAG_BINARY_INPUT_CONTENT);
    assert_input_leaf_missing(&batch, 2u, SNAG_BINARY_INPUT_INSTRUCTIONS);
    const struct { size_t offset; unsigned char value; } bad[] = {
        {0u, 0u}, {0u, 255u}, {4u, 0u}, {5u, 99u}, {5u, 210u}, {7u, 0u}, {7u, 3u},
        {9u, 38u}, {9u, 40u}, {45u, 4u}, {51u, 255u}
    };
    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        unsigned char saved = payload.data[bad[i].offset];
        payload.data[bad[i].offset] = bad[i].value;
        assert_rejected(record);
        input_reference_batch(&record, &batch_bytes, &batch);
        assert_input_leaf_missing(&batch, 2u, SNAG_BINARY_INPUT_TEXT);
        payload.data[bad[i].offset] = saved;
    }
    const uint16_t wrong_kinds[] = {
        SNAG_BINARY_IRC_REPLY_REMINDER, SNAG_BINARY_IRC_ADMITTED, 0x8000u
    };
    for (size_t i = 0u; i < sizeof(wrong_kinds) / sizeof(wrong_kinds[0]); ++i) {
        admission->input.kind = wrong_kinds[i];
        assert_irc_encode_rejected(&event);
    }
    admission->input.kind = SNAG_BINARY_STEERING_ADDED;
    admission->input.flags = SNAG_BINARY_RECORD_OPTIONAL;
    assert_irc_encode_rejected(&event);
    admission->input.flags = 0u;
    admission->input.version = 1u;
    roundtrip(&event);
    admission->input.version = 0u;
    assert_irc_encode_rejected(&event);
    admission->input.version = 3u;
    assert_irc_encode_rejected(&event);
    admission->input.version = 2u;
    admission->input.size--;
    assert_irc_encode_rejected(&event);
    admission->input.size++;
    admission->input.payload = NULL;
    assert_irc_encode_rejected(&event);

    /* A reference inside the wrapper remains a reference, not another original. */
    steering.data.steering_input.text = (struct snag_binary_text){0};
    steering.data.steering_input.text_ref = (struct snag_binary_input_reference){
        .field = SNAG_BINARY_INPUT_TEXT, .target = {1u, 20u, 2u}};
    snag_buf_reset(&nested);
    assert(!snag_binary_event_encode(&nested, &steering));
    admission->input.payload = nested.data;
    admission->input.size = nested.len;
    roundtrip(&event);
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    record.payload = payload.data;
    record.size = payload.len;
    input_reference_batch(&record, &batch_bytes, &batch);
    assert_input_leaf_missing(&batch, 2u, SNAG_BINARY_INPUT_TEXT);
    admission->input.version = 1u;
    assert_irc_encode_rejected(&event);

    struct snag_buf instructions = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf content = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_instructions_encode(&instructions, NULL, 0u));
    struct snag_binary_part part = {.kind = SNAG_BINARY_PART_TEXT, .text = text("part")};
    assert(!snag_binary_content_encode(&content, &part, 1u));
    struct snag_binary_event input = {.kind = SNAG_BINARY_INPUT_RECEIVED};
    input.data.input.selection = (struct snag_binary_selection){text("p"), text("m"), text("e")};
    input.data.input.text = text("x");
    input.data.input.instructions = (struct snag_binary_instructions){instructions.data,
        instructions.len};
    input.data.input.content = (struct snag_binary_content){content.data, content.len};
    snag_buf_reset(&nested);
    assert(!snag_binary_event_encode(&nested, &input));
    admission->input = (struct snag_binary_record){.kind = SNAG_BINARY_INPUT_RECEIVED,
        .version = 2u, .payload = nested.data, .size = nested.len};
    roundtrip(&event);
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    record.payload = payload.data;
    record.size = payload.len;
    input_reference_batch(&record, &batch_bytes, &batch);
    const uint32_t offsets[] = {46u, 47u, 38u};
    const struct snag_binary_text leaves[] = {text("x"), {content.data, content.len},
        {instructions.data, instructions.len}};
    for (unsigned int i = 0u; i < 3u; ++i) {
        enum snag_binary_input_leaf field = (enum snag_binary_input_leaf)(i + 1u);
        assert(!snag_binary_input_ref_create(&batch, 2u, field, &reference));
        assert(reference.offset == offsets[i] && reference.size == leaves[i].size);
        assert(!snag_binary_input_ref_resolve(&reference, &batch, field, &view));
        assert(!memcmp(view, leaves[i].data, leaves[i].size));
    }
    admission->input = (struct snag_binary_record){0};
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    record.payload = payload.data;
    record.size = payload.len;
    input_reference_batch(&record, &batch_bytes, &batch);
    assert_input_leaf_missing(&batch, 2u, SNAG_BINARY_INPUT_TEXT);
    sequences.data[4] = 0u;
    assert_irc_encode_rejected(&event);
    snag_buf_free(&instructions);
    snag_buf_free(&content);
    snag_buf_free(&batch_bytes);
    snag_buf_free(&nested);
    snag_buf_free(&payload);
    snag_buf_free(&sequences);
}

static json_t *
legacy_result_value_event(json_t *value)
{
    json_t *result = snag_tool_result("io_failed", NULL, "legacy", -1, 0u);
    assert(result && !json_object_set(result, "exit_code", value));
    assert(!snag_tool_result_valid(result));
    json_t *event = json_pack("{s:{s:o}}", "data", "result", result);
    assert(event);
    return event;
}

static void
assert_result_value_rejected(const void *data, size_t size)
{
    struct snag_binary_result_value out;
    unsigned char saved[sizeof(out)];
    memset(&out, 0xa5, sizeof(out));
    memcpy(saved, &out, sizeof(out));
    assert(snag_binary_result_value_decode(data, size, &out) < 0);
    assert(!memcmp(saved, &out, sizeof(out)));
    struct snag_binary_result_value value = {.data = data, .size = size};
    json_t *json = json_true();
    assert(snag_binary_result_value_json(&value, &json) < 0 && json == json_true());
}

static void
assert_result_value_encode_rejected(json_t *value)
{
    struct snag_buf out = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_buf_append(&out, "keep", 4u));
    assert(snag_binary_result_value_encode(&out, value) < 0);
    assert(out.len == 4u && !memcmp(out.data, "keep", 4u));
    snag_buf_free(&out);
}

static void
assert_result_value_roundtrip(json_t *value)
{
    assert(value);
    struct snag_buf encoded = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf canonical = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_json_canonical(value, &canonical));
    assert(!snag_binary_result_value_encode(&encoded, value));
    assert(encoded.len <= canonical.len);
    struct snag_binary_result_value decoded;
    assert(!snag_binary_result_value_decode(encoded.data, encoded.len, &decoded));
    assert(decoded.data == encoded.data && decoded.size == encoded.len &&
        decoded.canonical_size == canonical.len);
    if (json_is_integer(value)) {
        assert(decoded.kind == SNAG_BINARY_RESULT_INTEGER &&
            decoded.integer == json_integer_value(value));
    }
    json_t *reconstructed = NULL;
    assert(!snag_binary_result_value_json(&decoded, &reconstructed));
    assert(json_equal(reconstructed, value));
    struct snag_buf again = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_result_value_encode(&again, reconstructed));
    assert(again.len == encoded.len && !memcmp(again.data, encoded.data, encoded.len));
    for (size_t i = 0u; encoded.len <= 128u && i < encoded.len; ++i) {
        assert_result_value_rejected(encoded.data, i);
    }
    json_t *event = legacy_result_value_event(value);
    assert(!snag_json_canonical(event, &canonical));
    json_decref(event);
    json_decref(reconstructed);
    snag_buf_free(&again);
    snag_buf_free(&canonical);
    snag_buf_free(&encoded);
}

static void
test_result_value_depth(void)
{
    json_t *value = json_null();
    for (size_t i = 0u; i < 45u; ++i) {
        json_t *array = json_array();
        assert(array && !json_array_append_new(array, value));
        value = array;
    }
    assert_result_value_roundtrip(value);
    json_t *array = json_array();
    assert(array && !json_array_append_new(array, value));
    assert_result_value_encode_rejected(array);
    json_t *event = legacy_result_value_event(array);
    struct snag_buf canonical = {.max = SNAG_MAX_EVENT_LINE};
    assert(snag_json_canonical(event, &canonical) < 0);
    json_decref(event);
    json_decref(array);
    unsigned char deep[93];
    memset(deep, 5, 46u);
    deep[46] = 0u;
    memset(deep + 47u, 7, 46u);
    assert_result_value_rejected(deep, sizeof(deep));
    snag_buf_free(&canonical);
}

static void
test_result_value_limits(void)
{
    unsigned char *bytes = malloc(28384u);
    assert(bytes);
    memset(bytes, 'x', 28384u);
    json_t *part = json_stringn((const char *)bytes, 16384u);
    json_t *array = json_array();
    assert(part && array);
    for (size_t i = 0u; i < 1022u; ++i) assert(!json_array_append(array, part));
    assert(!json_array_append_new(array, json_stringn((const char *)bytes, 28384u)));
    json_decref(part);
    free(bytes);
    struct snag_buf canonical = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf encoded = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_json_canonical(array, &canonical));
    assert(canonical.len == 16775902u);
    assert(!snag_binary_result_value_encode(&encoded, array));
    assert(encoded.len == 16774880u);
    /* Type+length4 per string and type+count4 per array would not fit. */
    assert(encoded.len + 1023u * 3u + 3u > SNAG_MAX_EVENT_LINE);
    assert_result_value_roundtrip(array);
    json_decref(array);
    snag_buf_free(&encoded);
    snag_buf_free(&canonical);

    /* A short native control string can still exceed the canonical JSON bound. */
    size_t count = SNAG_MAX_EVENT_LINE / 6u + 1u;
    bytes = malloc(count + 2u);
    assert(bytes);
    bytes[0] = 4u;
    memset(bytes + 1u, 1, count);
    bytes[count + 1u] = 0u;
    assert_result_value_rejected(bytes, count + 2u);
    json_t *value = json_stringn((const char *)bytes + 1u, count);
    assert(value);
    assert_result_value_encode_rejected(value);
    json_decref(value);
    free(bytes);
}

static void
test_result_value_statuses(void)
{
    const char *statuses[] = {"not_run", "outcome_unknown", "denied", "cancelled", "running",
        "timed_out", "patch_rejected", "io_failed"};
    const char *reasons[] = {"protocol_conflict", "owner_lost", "user_denied", "turn_cancelled",
        "wait_timeout", NULL, NULL, NULL};
    json_t *exit_value = json_pack("{s:b}", "legacy", 1);
    json_t *signal_value = json_pack("[n,i]", 123);
    assert(exit_value && signal_value);
    for (size_t i = 0u; i < sizeof(statuses) / sizeof(statuses[0]); ++i) {
        json_t *result = snag_tool_result(statuses[i], reasons[i], "legacy", -1, 0u);
        assert(result && !json_object_set(result, "exit_code", exit_value) &&
            !json_object_set(result, "signal", signal_value));
        if (i == 4u) {
            assert(!json_object_set_new(result, "handle",
                json_string("0123456789abcdef0123456789abcdef")));
        }
        assert(!snag_tool_result_valid(result));
        json_decref(result);
    }
    assert_result_value_roundtrip(exit_value);
    assert_result_value_roundtrip(signal_value);
    json_decref(exit_value);
    json_decref(signal_value);
}

static void
test_result_values(void)
{
    json_t *value = json_pack("{s:[i,i,n],s:s}", "z", -1, 128, "a", "\xc3\xa9\n");
    assert(value);
    struct snag_buf encoded = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_result_value_encode(&encoded, value));
    static const unsigned char expected[] = {
        6u, 4u, 'a', 0u, 4u, 195u, 169u, '\n', 0u,
        4u, 'z', 0u, 5u, 3u, 1u, 3u, 128u, 2u, 0u, 7u, 7u
    };
    assert(encoded.len == sizeof(expected) && !memcmp(encoded.data, expected, sizeof(expected)));
    assert_result_value_roundtrip(value);
    json_decref(value);
    json_t *values[] = {json_null(), json_false(), json_true(), json_integer(-1),
        json_integer(0), json_integer(127), json_integer(128), json_integer(-64),
        json_integer(-65), json_integer(INT64_MIN), json_integer(INT64_MAX),
        json_string(""), json_string("\\\"\n\x7f\xc3\xa9"), json_array(), json_object(),
        json_pack("{s:n,s:b,s:[b]}", "", "a", 1, "\x07", 0)};
    for (size_t i = 0u; i < sizeof(values) / sizeof(values[0]); ++i) {
        assert_result_value_roundtrip(values[i]);
        json_decref(values[i]);
    }
    snag_buf_reset(&encoded);
    value = json_integer(INT64_MIN);
    assert(value && !snag_binary_result_value_encode(&encoded, value));
    static const unsigned char minimum[] = {
        3u, 255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u, 1u
    };
    assert(encoded.len == sizeof(minimum) && !memcmp(encoded.data, minimum, sizeof(minimum)));
    json_decref(value);
    const unsigned char bad[][11] = {
        {7u}, {8u}, {127u}, {3u, 0u}, {3u, 2u}, {3u, 129u, 0u},
        {3u, 255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u, 2u},
        {4u, 'a'}, {4u, 255u, 0u}, {5u, 0u}, {5u, 7u, 0u},
        {6u, 4u, 'b', 0u, 0u, 4u, 'a', 0u, 0u, 7u},
        {6u, 4u, 'a', 0u, 0u, 4u, 'a', 0u, 0u, 7u},
        {6u, 128u, 0u, 7u}, {6u, 4u, 'a', 0u, 7u},
        {6u, 4u, 255u, 0u, 0u, 7u}, {6u, 4u, 0u, 0u, 4u, 0u, 0u, 7u}
    };
    const size_t sizes[] = {
        1u, 1u, 1u, 2u, 2u, 3u, 11u, 2u, 3u, 2u, 3u, 10u, 10u, 4u, 5u, 6u, 8u
    };
    for (size_t i = 0u; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        assert_result_value_rejected(bad[i], sizes[i]);
    }
    assert_result_value_rejected(NULL, 0u);
    assert_result_value_rejected(expected, SIZE_MAX);
    struct snag_buf small = {.max = 3u};
    assert(!snag_buf_append(&small, "ok", 2u));
    value = json_integer(128);
    assert(value && snag_binary_result_value_encode(&small, value) < 0);
    assert(small.len == 2u && !memcmp(small.data, "ok", 2u));
    json_decref(value);
    snag_buf_free(&small);
    struct snag_binary_result_value decoded;
    assert(!snag_binary_result_value_decode(encoded.data, encoded.len, &decoded));
    /* Metadata is a decoded convenience, never authority over the borrowed bytes. */
    decoded.kind = SNAG_BINARY_RESULT_OBJECT;
    decoded.integer = 0;
    value = NULL;
    assert(!snag_binary_result_value_json(&decoded, &value));
    assert(json_is_integer(value) && json_integer_value(value) == INT64_MIN);
    json_decref(value);
    assert(snag_binary_result_value_decode(encoded.data, encoded.len, NULL) < 0);
    assert(snag_binary_result_value_encode(NULL, json_null()) < 0);
    assert(snag_binary_result_value_json(&decoded, NULL) < 0);
    value = json_true();
    assert(snag_binary_result_value_json(NULL, &value) < 0 && value == json_true());
    assert_result_value_encode_rejected(NULL);
    value = json_real(0.5);
    assert(value);
    json_t *event = legacy_result_value_event(value);
    struct snag_buf canonical = {.max = SNAG_MAX_EVENT_LINE};
    assert(snag_json_canonical(event, &canonical) < 0);
    assert_result_value_encode_rejected(value);
    json_decref(event);
    json_decref(value);
    value = json_stringn("a\0b", 3u);
    assert(value);
    assert_result_value_encode_rejected(value);
    json_decref(value);
    value = json_stringn_nocheck("\xff", 1u);
    assert(value);
    assert_result_value_encode_rejected(value);
    json_decref(value);
    value = json_object();
    assert(value && !json_object_set_new_nocheck(value, "\xff", json_null()));
    assert_result_value_encode_rejected(value);
    json_decref(value);
    snag_buf_free(&canonical);
    snag_buf_free(&encoded);
    test_result_value_depth();
    test_result_value_limits();
    test_result_value_statuses();
}

static struct snag_binary_tool_result
binary_tool_result(void)
{
    static const unsigned char zero[] = {128u};
    static const unsigned char null[] = {0u};
    return (struct snag_binary_tool_result){.status = SNAG_BINARY_TOOL_SUCCEEDED,
        .duration_ms = 7u, .exit_code = {.data = zero, .size = sizeof(zero)},
        .signal = {.data = null, .size = sizeof(null)},
        .model_text = {(const unsigned char *)"ok", 2u},
        .streams = {{.encoding = SNAG_BINARY_EXCERPT_UTF8},
            {.encoding = SNAG_BINARY_EXCERPT_UTF8}}};
}

static void
assert_event_encode_rejected(const struct snag_binary_event *event)
{
    struct snag_buf out = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_buf_append(&out, "keep", 4u));
    assert(snag_binary_event_encode(&out, event) < 0);
    assert(out.len == 4u && !memcmp(out.data, "keep", 4u));
    snag_buf_free(&out);
}

static void
test_turn_config_values(void)
{
    struct snag_binary_event event = turn_start_event();
    struct snag_binary_turn_config *config = &event.data.started.config;
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf extensions = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    json_t *values = json_loads(
        "[null,false,true,-9223372036854775808,\"text\",[1,null],{\"nested\":false}]", 0u, NULL);
    assert(values);
    for (size_t i = 0u; i < json_array_size(values); ++i) {
        json_t *value = json_array_get(values, i);
        snag_buf_reset(&bytes);
        assert(!snag_binary_result_value_encode(&bytes, value));
        json_t *extra = json_pack("{s:O,s:O}", "", value, "extension", value);
        assert(extra);
        snag_buf_reset(&extensions);
        assert(!snag_binary_rule_value_encode(&extensions, extra));
        config->extensions = (struct snag_binary_result_value){
            .data = extensions.data, .size = extensions.len
        };
        for (size_t j = 0u; j < SNAG_BINARY_TURN_VALUE_COUNT; ++j) {
            config->present = (uint16_t)(1u << (8u + j));
            config->values[j] = (struct snag_binary_result_value){
                .data = bytes.data, .size = bytes.len, .canonical_size = SIZE_MAX,
                .kind = SNAG_BINARY_RESULT_INTEGER, .integer = 42
            };
            roundtrip(&event);
            snag_buf_reset(&payload);
            assert(!snag_binary_event_encode(&payload, &event));
            struct snag_binary_record record = {
                .kind = event.kind, .version = 1u, .payload = payload.data, .size = payload.len
            };
            struct snag_binary_event decoded;
            assert(!snag_binary_event_decode(&record, &decoded));
            json_t *projected = NULL;
            assert(!snag_binary_result_value_json(&decoded.data.started.config.values[j],
                &projected));
            assert(json_equal(projected, value));
            json_decref(projected);
            assert(!snag_binary_rule_value_json(&decoded.data.started.config.extensions,
                &projected));
            assert(json_equal(projected, extra));
            json_decref(projected);
        }
        json_decref(extra);
    }
    json_decref(values);

    event = turn_start_event();
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    size_t empty_size = payload.len;
    static const unsigned char empty[] = {6u, 7u};
    config->extensions = (struct snag_binary_result_value){
        .data = empty, .size = sizeof(empty)
    };
    struct snag_buf explicit_empty = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&explicit_empty, &event));
    assert(explicit_empty.len == empty_size &&
        !memcmp(explicit_empty.data, payload.data, empty_size));
    snag_buf_free(&explicit_empty);
    static const char *const named[] = {
        "provider", "model", "effort", "max_parallel_commands", "default_yield_ms",
        "max_wait_ms", "default_timeout_ms", "max_timeout_ms", "tool_output_bytes",
        "output_cache_bytes", "max_turn_retries", "prompt_schema", "replay_schema",
        "tool_schema", "capability_version", "profile_id", "max_output_tokens",
        "parallel_tool_calls"
    };
    for (size_t i = 0u; i < sizeof(named) / sizeof(named[0]); ++i) {
        json_t *extra = json_pack("{s:n}", named[i]);
        assert(extra);
        snag_buf_reset(&extensions);
        assert(!snag_binary_rule_value_encode(&extensions, extra));
        json_decref(extra);
        config->extensions = (struct snag_binary_result_value){
            .data = extensions.data, .size = extensions.len
        };
        assert_event_encode_rejected(&event);
        /* Check the decoder independently of the writer's named-key rejection. */
        snag_buf_reset(&bytes);
        assert(!snag_buf_append(&bytes, payload.data, 49u));
        assert(!snag_buf_append(&bytes, extensions.data, extensions.len));
        assert(!snag_buf_append(&bytes, payload.data + 51u, payload.len - 51u));
        assert_rejected((struct snag_binary_record){
            .kind = event.kind, .version = 1u, .payload = bytes.data, .size = bytes.len
        });
    }
    json_t *near_names = json_loads(
        "{\"Provider\":null,\"max_wait_m\":0,\"nested\":{\"provider\":false}}", 0u, NULL);
    assert(near_names);
    snag_buf_reset(&extensions);
    assert(!snag_binary_rule_value_encode(&extensions, near_names));
    json_decref(near_names);
    config->extensions = (struct snag_binary_result_value){
        .data = extensions.data, .size = extensions.len
    };
    roundtrip(&event);
    for (unsigned int i = 0u; i < 4u; ++i) {
        const unsigned char invalid[] = {0u, 7u};
        config->extensions = (struct snag_binary_result_value){
            .data = invalid, .size = sizeof(invalid)
        };
        if (i == 1u) config->extensions.size = 0u;
        if (i == 2u) config->extensions.data = NULL;
        if (i == 3u) config->extensions.size = SNAG_MAX_EVENT_LINE + 1u;
        assert_event_encode_rejected(&event);
    }

    event = turn_start_event();
    config->present = 1u << 8u;
    unsigned char deep[93];
    memset(deep, 5u, 45u);
    deep[45] = 0u;
    memset(deep + 46u, 7u, 45u);
    config->values[0] = (struct snag_binary_result_value){
        .data = deep, .size = 91u
    };
    roundtrip(&event);
    memset(deep, 5u, 46u);
    deep[46] = 0u;
    memset(deep + 47u, 7u, 46u);
    config->values[0].size = sizeof(deep);
    assert_event_encode_rejected(&event);
    config->present = 0u;
    snag_buf_reset(&extensions);
    assert(!snag_buf_append(&extensions, "\6\4x\0", 4u));
    assert(!snag_buf_append(&extensions, deep + 1u, sizeof(deep) - 2u));
    assert(!snag_buf_append(&extensions, "\7", 1u));
    config->extensions = (struct snag_binary_result_value){
        .data = extensions.data, .size = extensions.len
    };
    roundtrip(&event);
    extensions.data[4] = 8u;
    assert_event_encode_rejected(&event);
    snag_buf_reset(&extensions);
    assert(!snag_buf_append(&extensions, "\6\4x\0", 4u));
    assert(!snag_buf_append(&extensions, deep, sizeof(deep)));
    assert(!snag_buf_append(&extensions, "\7", 1u));
    config->extensions = (struct snag_binary_result_value){
        .data = extensions.data, .size = extensions.len
    };
    assert_event_encode_rejected(&event);
    snag_buf_free(&bytes);
    snag_buf_free(&extensions);
    snag_buf_free(&payload);
}

static void
test_tool_result_statuses(void)
{
    const char *statuses[] = {"invalid", "not_run", "outcome_unknown", "denied", "cancelled",
        "running", "succeeded", "failed", "signaled", "timed_out", "patch_rejected", "io_failed",
        "invalid"};
    const char *reasons[] = {NULL, "protocol_conflict", "read_only", "process_limit", "batch_yield",
        "operator_yield", "process_busy", "stdin_busy", "stdin_closed", "invalid_arguments",
        "managed_process_conflict", "managed_process_handle_mismatch", "recovery_unstarted",
        "superseded_by_steering", "turn_cancelled", "process_interaction_required", "rule_rejected",
        "owner_lost", "unreaped_after_sigkill", "user_denied", "timeout_handoff", "wait_timeout",
        "steering_handoff", "output_drain_timeout", "invalid"};
    json_t *values[] = {json_null(), json_integer(0), json_integer(INT64_MIN),
        json_pack("[n]"), json_pack("{s:b}", "x", 1)};
    struct snag_buf bytes[5] = {{.max = 128u}, {.max = 128u}, {.max = 128u},
        {.max = 128u}, {.max = 128u}};
    for (size_t i = 0u; i < 5u; ++i) {
        assert(values[i] && !snag_binary_result_value_encode(&bytes[i], values[i]));
    }
    const size_t pairs[][2] = {{0u, 0u}, {1u, 0u}, {0u, 1u}, {3u, 4u}, {2u, 0u}, {0u, 2u}};
    for (size_t s = 0u; s < sizeof(statuses) / sizeof(statuses[0]); ++s) {
        for (size_t r = 0u; r < sizeof(reasons) / sizeof(reasons[0]); ++r) {
            for (unsigned int h = 0u; h < 2u; ++h) {
                for (size_t p = 0u; p < sizeof(pairs) / sizeof(pairs[0]); ++p) {
                    struct snag_binary_event event = {.kind = SNAG_BINARY_TOOL_FINISHED};
                    struct snag_binary_tool_result *result = &event.data.tool_finished.result;
                    *result = binary_tool_result();
                    result->status = (enum snag_binary_tool_status)s;
                    result->reason = (enum snag_binary_tool_reason)r;
                    result->has_handle = h != 0u;
                    size_t e = pairs[p][0];
                    size_t g = pairs[p][1];
                    result->exit_code = (struct snag_binary_result_value){
                        .data = bytes[e].data, .size = bytes[e].len};
                    result->signal = (struct snag_binary_result_value){
                        .data = bytes[g].data, .size = bytes[g].len};
                    json_t *legacy = snag_tool_result(statuses[s], reasons[r], "ok", 0, 7u);
                    assert(legacy && !json_object_set(legacy, "exit_code", values[e]) &&
                        !json_object_set(legacy, "signal", values[g]));
                    if (h) {
                        assert(!json_object_set_new(legacy, "handle",
                            json_string("00000000000000000000000000000000")));
                    }
                    if (!snag_tool_result_valid(legacy)) roundtrip(&event);
                    else assert_event_encode_rejected(&event);
                    json_decref(legacy);
                }
            }
        }
    }
    for (size_t i = 0u; i < 5u; ++i) {
        json_decref(values[i]);
        snag_buf_free(&bytes[i]);
    }
}

static void
test_tool_result_options(void)
{
    struct snag_binary_event event = {.kind = SNAG_BINARY_TOOL_FINISHED};
    struct snag_binary_tool_result *result = &event.data.tool_finished.result;
    struct snag_binary_part part = {.kind = SNAG_BINARY_PART_TEXT,
        .text = {(const unsigned char *)"part", 4u}};
    struct snag_buf content = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_content_encode(&content, &part, 1u));
    for (unsigned int flags = 0u; flags < 8u; ++flags) {
        *result = binary_tool_result();
        result->max_output_tokens = flags & 1u ? SNAG_CONFIG_TOKEN_LIMIT_MAX : 0u;
        result->has_output_ref = (flags & 2u) != 0u;
        result->output_ref = (struct snag_binary_tool_output_ref){.stdin_accepted = INT64_MAX,
            .stdin_written = 1u, .stdin_pending = INT64_MAX - 1u, .stdin_open = true,
            .from = {INT64_MAX, 0u}, .to = {INT64_MAX, 0u},
            .log_start = INT64_MAX - 1u, .log_end = INT64_MAX};
        if (flags & 4u) {
            result->content = (struct snag_binary_content){content.data, content.len};
        }
        if ((flags & 2u) && !(flags & 1u)) {
            assert_event_encode_rejected(&event);
        } else {
            roundtrip(&event);
        }
    }
    struct snag_binary_tool_result valid = *result;
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event) && payload.len == 219u);
    struct snag_binary_record record = {.kind = 177u, .version = 1u,
        .payload = payload.data, .size = payload.len};
    const size_t offsets[] = {34u, 189u, 205u, 180u, 141u, 218u};
    const unsigned char corrupt[] = {8u, 2u, 128u, 128u, 254u, 0u};
    for (size_t i = 0u; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        unsigned char saved = payload.data[offsets[i]];
        payload.data[offsets[i]] = corrupt[i];
        assert_rejected(record);
        payload.data[offsets[i]] = saved;
    }
    memset(payload.data + 109u, 0, 8u);
    assert_rejected(record);
    /* Remove the optional limit completely, leaving a well-formed output ref. */
    memmove(payload.data + 109u, payload.data + 117u, payload.len - 117u);
    payload.data[34] = 6u;
    record.size -= 8u;
    assert_rejected(record);
    snag_buf_free(&payload);
    result->max_output_tokens++;
    assert_event_encode_rejected(&event);
    *result = valid;
    result->output_ref.stdin_pending++;
    assert_event_encode_rejected(&event);
    *result = valid;
    result->output_ref.stdin_written = UINT64_MAX;
    assert_event_encode_rejected(&event);
    *result = valid;
    result->output_ref.stdin_accepted = UINT64_MAX;
    assert_event_encode_rejected(&event);
    *result = valid;
    result->output_ref.from[0]--;
    assert_event_encode_rejected(&event);
    *result = valid;
    result->output_ref.to[0]++;
    assert_event_encode_rejected(&event);
    *result = valid;
    result->output_ref.log_start = UINT64_MAX;
    assert_event_encode_rejected(&event);
    *result = valid;
    result->content.size--;
    assert_event_encode_rejected(&event);
    *result = valid;
    result->content.data = NULL;
    assert_event_encode_rejected(&event);
    snag_buf_reset(&content);
    struct snag_binary_part images[2] = {{.kind = SNAG_BINARY_PART_IMAGE,
        .asset = {.bytes = SNAG_MEDIA_REQUEST_MAX / 2u,
            .mime = {(const unsigned char *)"image/png", 9u}}}};
    images[1] = images[0];
    assert(!snag_binary_content_encode(&content, images, 2u));
    *result = binary_tool_result();
    result->content = (struct snag_binary_content){content.data, content.len};
    roundtrip(&event);
    images[1].asset.bytes++;
    snag_buf_reset(&content);
    assert(!snag_binary_content_encode(&content, images, 2u));
    result->content = (struct snag_binary_content){content.data, content.len};
    assert_event_encode_rejected(&event);
    snag_buf_free(&content);
}

static void
test_native_tool_output_ref(void)
{
    struct snag_binary_event event = {.kind = SNAG_BINARY_TOOL_FINISHED};
    struct snag_binary_tool_result *result = &event.data.tool_finished.result;
    *result = binary_tool_result();
    result->max_output_tokens = 16000u;
    result->has_output_ref = true;
    result->output_ref = (struct snag_binary_tool_output_ref){.native = true,
        .log_start = 100u, .log_end = 200u, .first_sequence = 2u, .end_sequence = 7u};
    struct snag_binary_tool_result valid = *result;
    roundtrip(&event);
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&bytes, &event));
    assert(bytes.len == 222u && bytes.data[34] == 11u);
    const unsigned char expected[16] = {2u, 0, 0, 0, 0, 0, 0, 0, 7u};
    assert(!memcmp(bytes.data + 206u, expected, sizeof(expected)));
    struct snag_binary_record record = {.kind = 177u, .version = 1u,
        .payload = bytes.data, .size = bytes.len};
    assert_rejected(record);
    record.version = 2u;
    bytes.data[34] = 9u; /* Native range without an output reference. */
    assert_rejected(record);
    bytes.data[34] = 27u; /* Unknown flag. */
    assert_rejected(record);
    bytes.data[34] = 11u;
    memset(bytes.data + 206u, 0, 8u); /* Only one zero endpoint. */
    assert_rejected(record);
    snag_buf_free(&bytes);
    for (unsigned int bad = 0u; bad < 7u; ++bad) {
        *result = valid;
        struct snag_binary_tool_output_ref *ref = &result->output_ref;
        if (bad == 0u) ref->end_sequence = 0u;
        if (bad == 1u) ref->first_sequence = 8u;
        if (bad == 2u) ref->end_sequence = UINT64_MAX;
        if (bad == 3u) ref->native = false;
        if (bad == 4u) result->has_output_ref = false;
        if (bad == 5u) ref->log_start = ref->log_end = 0u;
        if (bad == 6u) ref->first_sequence = ref->end_sequence;
        assert_event_encode_rejected(&event);
    }
    *result = valid;
    result->output_ref.log_start = result->output_ref.log_end;
    result->output_ref.first_sequence = result->output_ref.end_sequence;
    roundtrip(&event); /* A recorded empty interval is distinct from no hint. */
    result->output_ref.log_start = result->output_ref.log_end = 0u;
    result->output_ref.first_sequence = result->output_ref.end_sequence = 0u;
    roundtrip(&event);
    *result = valid;
    result->output_ref.log_start = 0u;
    result->output_ref.first_sequence = 1u;
    roundtrip(&event);
    result->output_ref.first_sequence = INT64_MAX - 1u;
    result->output_ref.end_sequence = INT64_MAX;
    roundtrip(&event); /* Range membership is checked by the journal consumer. */
    struct snag_binary_event close = {.kind = SNAG_BINARY_PROCESS_CLOSED};
    close.data.process_closed.cause = SNAG_BINARY_PROCESS_USER_INTERRUPT;
    close.data.process_closed.result = valid;
    roundtrip(&close);
}

static void
test_tool_result_excerpts(void)
{
    struct snag_binary_event event = {.kind = SNAG_BINARY_TOOL_FINISHED};
    struct snag_binary_tool_result *result = &event.data.tool_finished.result;
    *result = binary_tool_result();
    result->streams[0] = (struct snag_binary_tool_excerpt){.encoding = SNAG_BINARY_EXCERPT_BASE64,
        .retained = {(const unsigned char *)"!!!!", 4u}, .retained_bytes = INT64_MAX};
    json_t *legacy = snag_tool_result("succeeded", NULL, "ok", 0, 7u);
    assert(legacy);
    json_t *excerpt = json_pack("{s:i,s:s,s:i,s:s,s:I}", "discarded_bytes", 0,
        "encoding", "base64", "original_bytes", 0, "retained", "!!!!",
        "retained_bytes", (json_int_t)INT64_MAX);
    assert(excerpt && !json_object_set_new(legacy, "stdout", excerpt));
    assert(!snag_tool_result_valid(legacy));
    roundtrip(&event);
    result->streams[0].retained.size--;
    assert_event_encode_rejected(&event);
    result->streams[0].retained.size++;
    result->streams[0].encoding = SNAG_BINARY_EXCERPT_UTF8;
    assert_event_encode_rejected(&event);
    result->streams[0].retained_bytes = 4u;
    assert(!json_object_set_new(excerpt, "encoding", json_string("utf8")) &&
        !json_object_set_new(excerpt, "retained_bytes", json_integer(4)));
    /* Legacy admission does not require retained_bytes <= original_bytes. */
    assert(!snag_tool_result_valid(legacy));
    roundtrip(&event);
    result->streams[0].discarded_bytes = 1u;
    assert_event_encode_rejected(&event);
    result->streams[0].discarded_bytes = 0u;
    result->streams[0].original_bytes = (uint64_t)INT64_MAX + 1u;
    assert_event_encode_rejected(&event);
    *result = binary_tool_result();
    result->duration_ms = (uint64_t)INT64_MAX + 1u;
    assert_event_encode_rejected(&event);
    *result = binary_tool_result();
    result->model_text = (struct snag_binary_text){(const unsigned char *)"a\0b", 3u};
    assert_event_encode_rejected(&event);
    json_decref(legacy);
}

static void
test_tool_result_records(void)
{
    struct snag_binary_event event = {.kind = SNAG_BINARY_TOOL_FINISHED};
    struct snag_binary_tool_finish *finish = &event.data.tool_finished;
    finish->turn[0] = 1u;
    finish->call[0] = 2u;
    finish->result = binary_tool_result();
    finish->result.streams[0] = (struct snag_binary_tool_excerpt){
        .encoding = SNAG_BINARY_EXCERPT_UTF8, .original_bytes = 1u, .retained_bytes = 1u,
        .retained = {(const unsigned char *)"x", 1u}};
    unsigned char expected[110] = {[0] = 1u, [16] = 2u, [32] = 6u, [35] = 7u, [43] = 128u,
        [45] = 2u, [49] = 'o', [50] = 'k', [51] = 1u, [60] = 1u, [68] = 1u, [76] = 1u,
        [80] = 'x', [81] = 1u};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    assert(payload.len == sizeof(expected) && !memcmp(payload.data, expected, sizeof(expected)));
    roundtrip(&event);
    struct snag_binary_record record = {.kind = 177u, .version = 1u,
        .payload = payload.data, .size = payload.len};
    const size_t offsets[] = {32u, 33u, 34u, 42u, 43u, 44u, 51u, 67u, 75u, 80u, 81u};
    const unsigned char corrupt[] = {0u, 24u, 8u, 128u, 0u, 128u, 3u, 128u, 128u, 0u, 0u};
    for (size_t i = 0u; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        unsigned char saved = payload.data[offsets[i]];
        payload.data[offsets[i]] = corrupt[i];
        assert_rejected(record);
        payload.data[offsets[i]] = saved;
    }
    struct snag_binary_event close = {.kind = SNAG_BINARY_PROCESS_CLOSED};
    close.data.process_closed.turn[0] = 1u;
    close.data.process_closed.handle[0] = 2u;
    close.data.process_closed.cause = SNAG_BINARY_PROCESS_USER_INTERRUPT;
    close.data.process_closed.result = finish->result;
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &close));
    assert(payload.len == sizeof(expected) + 1u && !memcmp(payload.data, expected, 32u) &&
        payload.data[32] == 1u &&
        !memcmp(payload.data + 33u, expected + 32u, sizeof(expected) - 32u));
    for (unsigned int cause = 0u; cause <= 7u; ++cause) {
        close.data.process_closed.cause = (enum snag_binary_process_cause)cause;
        if (cause && cause < 7u) roundtrip(&close);
        else assert_event_encode_rejected(&close);
    }
    close.data.process_closed.cause = SNAG_BINARY_PROCESS_TOOL_FAILURE;
    const enum snag_binary_tool_reason reasons[] = {SNAG_BINARY_TOOL_REASON_NONE,
        SNAG_BINARY_TOOL_PROTOCOL_CONFLICT, SNAG_BINARY_TOOL_OWNER_LOST,
        SNAG_BINARY_TOOL_USER_DENIED, SNAG_BINARY_TOOL_TURN_CANCELLED};
    for (unsigned int status = 1u; status <= 11u; ++status) {
        struct snag_binary_tool_result *result = &close.data.process_closed.result;
        *result = binary_tool_result();
        result->status = (enum snag_binary_tool_status)status;
        result->has_handle = status == 5u;
        result->reason = status < 5u ? reasons[status] : SNAG_BINARY_TOOL_REASON_NONE;
        if (status == 8u) {
            struct snag_binary_result_value signal = result->signal;
            result->signal = result->exit_code;
            result->exit_code = signal;
        }
        if (status == 1u || status == 3u || status == 5u || status == 10u) {
            assert_event_encode_rejected(&close);
        } else {
            roundtrip(&close);
        }
    }
    size_t length = 2u * 1024u * 1024u + 1u;
    unsigned char *large = malloc(SNAG_MAX_EVENT_LINE);
    assert(large);
    memset(large, 'x', SNAG_MAX_EVENT_LINE);
    finish->result.model_text = (struct snag_binary_text){large, length};
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    record.payload = payload.data;
    record.size = payload.len;
    struct snag_binary_event decoded;
    assert(!snag_binary_event_decode(&record, &decoded));
    assert(decoded.data.tool_finished.result.model_text.size == length);
    finish->result.model_text.size = SNAG_MAX_EVENT_LINE - sizeof(expected) + 2u;
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event) && payload.len == SNAG_MAX_EVENT_LINE);
    record.payload = payload.data;
    record.size = payload.len;
    assert(!snag_binary_event_decode(&record, &decoded));
    finish->result.model_text.size++;
    assert_event_encode_rejected(&event);
    free(large);
    snag_buf_free(&payload);
    test_tool_result_statuses();
    test_tool_result_options();
    test_native_tool_output_ref();
    test_tool_result_excerpts();
}

static void
test_tool_start(void)
{
    struct snag_binary_event event = {.kind = SNAG_BINARY_TOOL_STARTED};
    struct snag_binary_tool_start *start = &event.data.tool_started;
    start->turn[0] = 1u;
    start->call[0] = 2u;
    start->action_sha256[0] = 3u;
    start->cwd = text("/");
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    static const unsigned char expected[69] = {
        [0] = 1u, [16] = 2u, [32] = 3u, [64] = 1u, [68] = '/'
    };
    assert(payload.len == sizeof(expected) && !memcmp(payload.data, expected, sizeof(expected)));
    assert(SNAG_BINARY_TOOL_STARTED == 176 && snag_binary_event_version(event.kind) == 1u);
    roundtrip(&event);
    size_t saved = payload.len;
    const char *bad[] = {"", "\xc0\x80", "\xff"};
    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        start->cwd = text(bad[i]);
        assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    }
    start->cwd = (struct snag_binary_text){(const unsigned char *)"a\0b", 3u};
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    unsigned char path[SNAG_PATH_MAX_BYTES + 1u];
    memset(path, '/', sizeof(path));
    start->cwd = (struct snag_binary_text){path, SNAG_PATH_MAX_BYTES};
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    struct snag_binary_record record = {.kind = (uint16_t)event.kind, .version = 1u,
        .payload = payload.data, .size = payload.len};
    struct snag_binary_event decoded;
    assert(!snag_binary_event_decode(&record, &decoded));
    assert(decoded.data.tool_started.cwd.size == SNAG_PATH_MAX_BYTES &&
        !memcmp(decoded.data.tool_started.cwd.data, path, SNAG_PATH_MAX_BYTES));
    saved = payload.len;
    start->cwd.size++;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    assert(!snag_buf_append(&payload, "/", 1u));
    payload.data[64u] = 1u;
    record.payload = payload.data;
    record.size = payload.len;
    assert_rejected(record);
    snag_buf_free(&payload);
}

static void
test_process_output_legacy(const struct snag_binary_process_output *output,
    const char *encoding, const char *data)
{
    json_t *legacy = json_pack("{s:s,s:s,s:i,s:I,s:s,s:s}",
        "turn_id", "01000000000000000000000000000000",
        "handle", "02000000000000000000000000000000", "stream", (int)output->stream,
        "offset", (json_int_t)output->offset, "encoding", encoding, "data", data);
    assert(legacy);
    struct snag_buf decoded = {.max = SNAG_BINARY_PROCESS_CHUNK_MAX};
    assert(!snag_process_output_decode(legacy, &decoded));
    assert(decoded.len == output->size && !memcmp(decoded.data, output->data, output->size));
    if (output->encoding == SNAG_BINARY_BYTES_BASE64) {
        struct snag_buf encoded = {.max = 32768u};
        assert(!snag_base64_append(&encoded, output->data, output->size));
        assert(encoded.len == strlen(data) && !memcmp(encoded.data, data, encoded.len));
        snag_buf_free(&encoded);
    }
    snag_buf_free(&decoded);
    json_decref(legacy);
}

static void
test_process_output(void)
{
    struct snag_binary_event event = {.kind = SNAG_BINARY_PROCESS_OUTPUT};
    struct snag_binary_process_output *output = &event.data.process_output;
    output->turn[0] = 1u;
    output->handle[0] = 2u;
    output->offset = (UINT64_C(1) << 32u) + 3u;
    output->stream = 1u;
    output->encoding = SNAG_BINARY_BYTES_UTF8;
    output->data = (const unsigned char *)"\xc3\xa9";
    output->size = 2u;
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    static const unsigned char expected[48] = {
        [0] = 1u, [16] = 2u, [32] = 3u, [36] = 1u, [40] = 1u, [41] = 1u,
        [42] = 2u, [46] = 0xc3u, [47] = 0xa9u
    };
    assert(payload.len == sizeof(expected) && !memcmp(payload.data, expected, sizeof(expected)));
    assert(SNAG_BINARY_PROCESS_OUTPUT == 192 && snag_binary_event_version(event.kind) == 1u);
    roundtrip(&event);
    test_process_output_legacy(output, "utf8", "\xc3\xa9");
    struct snag_binary_record record = {.kind = (uint16_t)event.kind, .version = 1u,
        .payload = payload.data, .size = payload.len};
    struct snag_binary_event decoded;
    assert(!snag_binary_event_decode(&record, &decoded));
    assert(decoded.data.process_output.offset == output->offset &&
        decoded.data.process_output.stream == 1u && decoded.data.process_output.size == 2u &&
        decoded.data.process_output.data == payload.data + 46u);
    const size_t positions[] = {39u, 40u, 41u, 42u, 46u};
    const unsigned char bad_values[] = {0x80u, 2u, 3u, 0u, 0u};
    for (size_t i = 0u; i < sizeof(positions) / sizeof(positions[0]); ++i) {
        unsigned char saved = payload.data[positions[i]];
        payload.data[positions[i]] = bad_values[i];
        assert_rejected(record);
        payload.data[positions[i]] = saved;
    }
    size_t saved = payload.len;
    output->stream = 2u;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    output->stream = 0u;
    roundtrip(&event);
    output->encoding = (enum snag_binary_byte_encoding)0;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    output->encoding = SNAG_BINARY_BYTES_UTF8;
    output->offset = (uint64_t)INT64_MAX - output->size;
    roundtrip(&event);
    output->offset++;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    output->offset = 0u;
    static const unsigned char raw[] = {0u, 0xffu, 0xfeu, 'a'};
    output->data = raw;
    output->size = sizeof(raw);
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    output->encoding = SNAG_BINARY_BYTES_BASE64;
    roundtrip(&event);
    test_process_output_legacy(output, "base64", "AP/+YQ==");
    output->size = 0u;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    output->size = 1u;
    output->data = NULL;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    unsigned char large[SNAG_BINARY_PROCESS_CHUNK_MAX + 1u];
    memset(large, 0xff, sizeof(large));
    output->data = large;
    output->size = SNAG_BINARY_PROCESS_CHUNK_MAX;
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    record.payload = payload.data;
    record.size = payload.len;
    assert(!snag_binary_event_decode(&record, &decoded));
    assert(decoded.data.process_output.size == SNAG_BINARY_PROCESS_CHUNK_MAX &&
        !memcmp(decoded.data.process_output.data, large, SNAG_BINARY_PROCESS_CHUNK_MAX));
    saved = payload.len;
    output->size++;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    payload.data[42u] = 1u;
    assert(!snag_buf_append(&payload, large, 1u));
    record.payload = payload.data;
    record.size = payload.len;
    assert_rejected(record);
    snag_buf_free(&payload);
}

static void
assert_process_reference_rejected(const struct snag_binary_ref *reference,
    const struct snag_binary_batch *batch)
{
    struct snag_binary_process_output source;
    memset(&source, 0x5a, sizeof(source));
    unsigned char saved[sizeof(source)];
    memcpy(saved, &source, sizeof(source));
    assert(snag_binary_process_ref_resolve(reference, batch, &source) < 0);
    assert(!memcmp(saved, &source, sizeof(source)));
}

static void
assert_process_original_missing(const struct snag_binary_batch *batch, uint64_t sequence)
{
    struct snag_binary_ref reference = {99u, 98u, 97u};
    assert(snag_binary_process_ref_create(batch, sequence, &reference) < 0);
    assert(reference.sequence == 99u && reference.offset == 98u && reference.size == 97u);
    reference = (struct snag_binary_ref){sequence, 46u, 4u};
    assert_process_reference_rejected(&reference, batch);
}

static void
test_process_references(void)
{
    static const unsigned char data[] = {'a', 0u, 0xffu, 'b'};
    struct snag_binary_event event = {.kind = SNAG_BINARY_PROCESS_OUTPUT};
    struct snag_binary_process_output *output = &event.data.process_output;
    memcpy(output->turn, data, sizeof(data));
    output->handle[0] = 3u;
    output->offset = (UINT64_C(1) << 32u) + 7u;
    output->stream = 1u;
    output->encoding = SNAG_BINARY_BYTES_BASE64;
    output->data = data;
    output->size = sizeof(data);
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    struct snag_binary_record record = {.kind = (uint16_t)event.kind, .version = 1u,
        .payload = payload.data, .size = payload.len};
    struct snag_binary_batch batch;
    input_reference_batch(&record, &bytes, &batch);
    struct snag_binary_ref reference;
    assert(!snag_binary_process_ref_create(&batch, 2u, &reference));
    assert(reference.sequence == 2u && reference.offset == 46u && reference.size == 4u);
    struct snag_binary_process_output source;
    assert(!snag_binary_process_ref_resolve(&reference, &batch, &source));
    assert(!memcmp(source.turn, output->turn, 16u) &&
        !memcmp(source.handle, output->handle, 16u) && source.offset == output->offset &&
        source.stream == 1u && source.encoding == SNAG_BINARY_BYTES_BASE64 &&
        source.size == sizeof(data) && !memcmp(source.data, data, sizeof(data)));
    const struct snag_binary_ref bad[] = {
        {2u, 0u, 4u}, {2u, 46u, 0u}, {2u, 46u, 3u}, {2u, 47u, 3u}, {2u, 45u, 5u},
        {2u, 46u, 5u}, {2u, UINT32_MAX, 4u}, {2u, 46u, UINT32_MAX},
        {0u, 46u, 4u}, {1u, 46u, 4u}, {3u, 46u, 4u}, {UINT64_MAX, 46u, 4u}
    };
    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        assert_process_reference_rejected(&bad[i], &batch);
    }
    assert_input_leaf_missing(&batch, 2u, SNAG_BINARY_INPUT_TEXT);
    assert_output_reference_rejected(&reference, &batch);
    assert_graph_reference_rejected(&reference, &batch, SNAG_BINARY_ITEM_ASSISTANT);
    assert_continuation_reference_rejected(&reference, &batch);
    assert_process_reference_rejected(NULL, &batch);
    assert_process_reference_rejected(&reference, NULL);
    assert(snag_binary_process_ref_create(&batch, 2u, NULL) < 0);
    assert(snag_binary_process_ref_resolve(&reference, &batch, NULL) < 0);
    assert_process_original_missing(NULL, 2u);
    assert_process_original_missing(&batch, 0u);
    assert_process_original_missing(&batch, 1u);
    assert_process_original_missing(&batch, 3u);
    assert_process_original_missing(&batch, UINT64_MAX);
    for (size_t i = 0u; i < payload.len; ++i) {
        record.size = i;
        input_reference_batch(&record, &bytes, &batch);
        assert_process_original_missing(&batch, 2u);
    }
    record.size = payload.len;
    const size_t positions[] = {39u, 40u, 41u, 42u};
    const unsigned char corrupt[] = {0x80u, 2u, 1u, 0u};
    for (size_t i = 0u; i < sizeof(positions) / sizeof(positions[0]); ++i) {
        unsigned char saved = payload.data[positions[i]];
        payload.data[positions[i]] = corrupt[i];
        input_reference_batch(&record, &bytes, &batch);
        assert_process_original_missing(&batch, 2u);
        payload.data[positions[i]] = saved;
    }
    record.version = 2u;
    input_reference_batch(&record, &bytes, &batch);
    assert_process_original_missing(&batch, 2u);
    record.version = 1u;
    record.flags = SNAG_BINARY_RECORD_OPTIONAL;
    input_reference_batch(&record, &bytes, &batch);
    assert_process_original_missing(&batch, 2u);
    record.flags = 0u;
    record.kind = SNAG_BINARY_RESPONSE_OUTPUT;
    input_reference_batch(&record, &bytes, &batch);
    assert_process_original_missing(&batch, 2u);
    record.kind = (uint16_t)event.kind;
    unsigned char large[SNAG_BINARY_PROCESS_CHUNK_MAX];
    memset(large, 'x', sizeof(large));
    output->encoding = SNAG_BINARY_BYTES_UTF8;
    output->data = large;
    output->size = sizeof(large);
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    record.payload = payload.data;
    record.size = payload.len;
    input_reference_batch(&record, &bytes, &batch);
    assert(!snag_binary_process_ref_create(&batch, 2u, &reference));
    assert(reference.size == SNAG_BINARY_PROCESS_CHUNK_MAX);
    assert(!snag_binary_process_ref_resolve(&reference, &batch, &source));
    assert(source.encoding == SNAG_BINARY_BYTES_UTF8 && source.size == sizeof(large) &&
        !memcmp(source.data, large, sizeof(large)));
    snag_buf_free(&bytes);
    snag_buf_free(&payload);
}

static void
assert_result_reference_rejected(const struct snag_binary_ref *reference,
    const struct snag_binary_batch *batch, enum snag_binary_kind kind)
{
    struct snag_binary_result_source source;
    memset(&source, 0x5a, sizeof(source));
    unsigned char saved[sizeof(source)];
    memcpy(saved, &source, sizeof(source));
    assert(snag_binary_result_ref_resolve(reference, batch, kind, &source) < 0);
    assert(!memcmp(saved, &source, sizeof(source)));
}

static void
assert_result_original_missing(const struct snag_binary_batch *batch, uint64_t sequence,
    enum snag_binary_kind kind)
{
    struct snag_binary_ref reference = {99u, 98u, 97u};
    assert(snag_binary_result_ref_create(batch, sequence, kind, &reference) < 0);
    assert(reference.sequence == 99u && reference.offset == 98u && reference.size == 97u);
    reference = (struct snag_binary_ref){sequence,
        kind == SNAG_BINARY_TOOL_FINISHED ? 32u : 33u, 77u};
    assert_result_reference_rejected(&reference, batch, kind);
}

static void
test_result_references(void)
{
    const enum snag_binary_kind kinds[] = {SNAG_BINARY_TOOL_FINISHED, SNAG_BINARY_PROCESS_CLOSED};
    for (size_t k = 0u; k < 2u; ++k) {
        struct snag_binary_event event = {.kind = kinds[k]};
        if (!k) {
            event.data.tool_finished.turn[0] = 1u;
            event.data.tool_finished.call[0] = 128u;
            event.data.tool_finished.result = binary_tool_result();
        } else {
            event.data.process_closed.turn[0] = 1u;
            event.data.process_closed.handle[0] = 128u;
            event.data.process_closed.cause = SNAG_BINARY_PROCESS_OUTPUT_FAILURE;
            event.data.process_closed.result = binary_tool_result();
        }
        struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
        struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
        assert(!snag_binary_event_encode(&payload, &event));
        struct snag_binary_record record = {.kind = (uint16_t)event.kind, .version = 1u,
            .payload = payload.data, .size = payload.len};
        struct snag_binary_batch batch;
        input_reference_batch(&record, &bytes, &batch);
        struct snag_binary_ref reference;
        assert(!snag_binary_result_ref_create(&batch, 2u, kinds[k], &reference));
        uint32_t offset = k ? 33u : 32u;
        assert(reference.sequence == 2u && reference.offset == offset && reference.size == 77u);
        struct snag_binary_result_source source;
        assert(!snag_binary_result_ref_resolve(&reference, &batch, kinds[k], &source));
        assert(source.kind == kinds[k] && source.turn[0] == 1u && source.owner[0] == 128u &&
            source.cause == (k ? SNAG_BINARY_PROCESS_OUTPUT_FAILURE : 0));
        assert(source.result.status == SNAG_BINARY_TOOL_SUCCEEDED &&
            source.result.exit_code.kind == SNAG_BINARY_RESULT_INTEGER &&
            source.result.exit_code.integer == 0 && source.result.model_text.size == 2u &&
            !memcmp(source.result.model_text.data, "ok", 2u));
        const struct snag_binary_ref bad[] = {
            {2u, offset, 76u}, {2u, offset + 1u, 76u}, {2u, offset, 78u}, {2u, offset, 0u},
            {2u, 0u, 77u}, {2u, 16u, 1u}, {2u, offset + 11u, 1u},
            {2u, UINT32_MAX, 77u}, {2u, offset, UINT32_MAX},
            {0u, offset, 77u}, {1u, offset, 77u}, {3u, offset, 77u},
            {UINT64_MAX, offset, 77u}
        };
        for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
            assert_result_reference_rejected(&bad[i], &batch, kinds[k]);
        }
        assert_input_leaf_missing(&batch, 2u, SNAG_BINARY_INPUT_TEXT);
        assert_output_reference_rejected(&reference, &batch);
        assert_graph_reference_rejected(&reference, &batch, SNAG_BINARY_ITEM_ASSISTANT);
        assert_continuation_reference_rejected(&reference, &batch);
        assert_process_reference_rejected(&reference, &batch);
        assert_result_original_missing(&batch, 2u, kinds[1u - k]);
        assert_result_original_missing(&batch, 2u, SNAG_BINARY_TOOL_STARTED);
        assert_result_original_missing(NULL, 2u, kinds[k]);
        assert_result_original_missing(&batch, 0u, kinds[k]);
        assert_result_original_missing(&batch, 1u, kinds[k]);
        assert_result_original_missing(&batch, 3u, kinds[k]);
        assert_result_original_missing(&batch, UINT64_MAX, kinds[k]);
        assert_result_reference_rejected(NULL, &batch, kinds[k]);
        assert(snag_binary_result_ref_create(&batch, 2u, kinds[k], NULL) < 0);
        assert(snag_binary_result_ref_resolve(&reference, &batch, kinds[k], NULL) < 0);
        for (size_t i = 0u; i < payload.len; ++i) {
            record.size = i;
            input_reference_batch(&record, &bytes, &batch);
            assert_result_original_missing(&batch, 2u, kinds[k]);
        }
        record.size = payload.len;
        payload.data[payload.len - 1u] = 128u;
        input_reference_batch(&record, &bytes, &batch);
        assert_result_original_missing(&batch, 2u, kinds[k]);
        payload.data[payload.len - 1u] = 0u;
        record.version = 3u;
        input_reference_batch(&record, &bytes, &batch);
        assert_result_original_missing(&batch, 2u, kinds[k]);
        record.version = 1u;
        record.flags = SNAG_BINARY_RECORD_OPTIONAL;
        input_reference_batch(&record, &bytes, &batch);
        assert_result_original_missing(&batch, 2u, kinds[k]);
        record.flags = 0u;
        record.kind = SNAG_BINARY_PROCESS_OUTPUT;
        input_reference_batch(&record, &bytes, &batch);
        assert_result_original_missing(&batch, 2u, kinds[k]);
        struct snag_binary_tool_result *rich = k ? &event.data.process_closed.result :
            &event.data.tool_finished.result;
        rich->max_output_tokens = SNAG_CONFIG_TOKEN_LIMIT_MAX;
        rich->has_output_ref = true;
        rich->output_ref.handle[0] = 3u;
        rich->output_ref.stdin_open = true;
        struct snag_binary_part part = {.kind = SNAG_BINARY_PART_TEXT, .text = text("part")};
        struct snag_buf content = {.max = SNAG_MAX_EVENT_LINE};
        assert(!snag_binary_content_encode(&content, &part, 1u));
        rich->content = (struct snag_binary_content){content.data, content.len};
        snag_buf_reset(&payload);
        assert(!snag_binary_event_encode(&payload, &event));
        record.kind = (uint16_t)event.kind;
        record.payload = payload.data;
        record.size = payload.len;
        input_reference_batch(&record, &bytes, &batch);
        assert(!snag_binary_result_ref_create(&batch, 2u, kinds[k], &reference));
        assert(reference.offset == offset && reference.size == 187u);
        assert(!snag_binary_result_ref_resolve(&reference, &batch, kinds[k], &source));
        assert(source.result.max_output_tokens == SNAG_CONFIG_TOKEN_LIMIT_MAX &&
            source.result.has_output_ref && source.result.output_ref.handle[0] == 3u &&
            source.result.output_ref.stdin_open && source.result.content.size == content.len &&
            !memcmp(source.result.content.data, content.data, content.len));
        reference.size -= (uint32_t)content.len;
        assert_result_reference_rejected(&reference, &batch, kinds[k]);
        payload.data[payload.len - 1u] = 0u;
        input_reference_batch(&record, &bytes, &batch);
        assert_result_original_missing(&batch, 2u, kinds[k]);
        snag_buf_free(&content);
        snag_buf_free(&bytes);
        snag_buf_free(&payload);
    }
}

static void
test_compact_start_and_interrupt(void)
{
    struct snag_binary_event event = {.kind = SNAG_BINARY_COMPACTION_STARTED};
    struct snag_binary_compact_start *value = &event.data.compaction_started;
    value->id[0] = 1u;
    value->reason = SNAG_BINARY_COMPACT_MANUAL;
    value->count_method = SNAG_BINARY_COUNT_EXACT;
    value->source_seq = 2u;
    value->input_tokens = 3u;
    value->source_sha256[0] = 4u;
    value->request_sha256[0] = 5u;
    value->count_request_sha256[0] = 6u;
    value->model = text("m");
    value->capability = text("c");
    value->profile = text("p");
    value->predecessor[0] = 7u;
    value->scope[0] = 8u;
    value->compaction_model = text("s");
    unsigned char expected[146] = {[0] = 1u, [17] = 1u, [18] = 1u, [19] = 2u, [27] = 3u,
        [35] = 4u, [67] = 5u, [99] = 6u, [131] = 1u, [135] = 'm', [136] = 1u, [140] = 'c',
        [141] = 1u, [145] = 'p'};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    assert(payload.len == sizeof(expected) && !memcmp(payload.data, expected, sizeof(expected)));
    struct snag_binary_record record = {.kind = 224u, .version = 1u,
        .payload = payload.data, .size = payload.len};
    const size_t positions[] = {16u, 17u, 18u, 19u, 26u, 34u, 131u, 135u};
    const unsigned char corrupt[] = {8u, 0u, 0u, 0u, 128u, 128u, 0u, 0u};
    for (size_t i = 0u; i < sizeof(positions) / sizeof(positions[0]); ++i) {
        unsigned char saved = payload.data[positions[i]];
        payload.data[positions[i]] = corrupt[i];
        assert_rejected(record);
        payload.data[positions[i]] = saved;
    }
    for (unsigned int flags = 0u; flags < 8u; ++flags) {
        value->has_predecessor = (flags & 1u) != 0u;
        value->has_scope = (flags & 2u) != 0u;
        value->has_compaction_model = (flags & 4u) != 0u;
        roundtrip(&event);
    }
    payload.len = 0u;
    assert(!snag_binary_event_encode(&payload, &event) && payload.len == 199u);
    assert(payload.data[16] == 7u && payload.data[146] == 7u && payload.data[162] == 8u &&
        payload.data[194] == 1u && payload.data[198] == 's');
    for (unsigned int reason = 0u; reason <= 8u; ++reason) {
        value->reason = (enum snag_binary_compact_reason)reason;
        if (reason >= 1u && reason <= 7u) roundtrip(&event);
        else assert_event_encode_rejected(&event);
    }
    value->reason = SNAG_BINARY_COMPACT_MANUAL;
    const uint64_t counts[] = {0u, 1u, INT64_MAX, (uint64_t)INT64_MAX + 1u};
    for (unsigned int method = 0u; method <= 7u; ++method) {
        value->count_method = (enum snag_binary_count_method)method;
        for (size_t i = 0u; i < sizeof(counts) / sizeof(counts[0]); ++i) {
            value->input_tokens = counts[i];
            bool valid = method >= 1u && method <= 6u && counts[i] <= INT64_MAX &&
                (method == 3u ? counts[i] == 0u : counts[i] != 0u);
            if (valid) roundtrip(&event);
            else assert_event_encode_rejected(&event);
        }
    }
    value->count_method = SNAG_BINARY_COUNT_EXACT;
    value->input_tokens = 1u;
    value->source_seq = 0u;
    assert_event_encode_rejected(&event);
    value->source_seq = INT64_MAX;
    roundtrip(&event);
    value->source_seq++;
    assert_event_encode_rejected(&event);
    value->source_seq = 1u;
    unsigned char model[SNAG_MODEL_MAX_BYTES];
    memset(model, 'x', sizeof(model));
    value->compaction_model = (struct snag_binary_text){model, sizeof(model)};
    roundtrip(&event); /* The historical optional compaction model has no name-size cap. */
    value->model = value->compaction_model;
    assert_event_encode_rejected(&event);
    value->model = text("m");
    value->compaction_model = text("");
    assert_event_encode_rejected(&event);
    snag_buf_free(&payload);

    event = (struct snag_binary_event){.kind = SNAG_BINARY_COMPACTION_INTERRUPTED};
    event.data.compaction_interrupted.id[0] = 9u;
    for (unsigned int reason = 0u; reason <= 6u; ++reason) {
        event.data.compaction_interrupted.reason = (enum snag_binary_compact_stop_reason)reason;
        if (reason >= 1u && reason <= 5u) roundtrip(&event);
        else assert_event_encode_rejected(&event);
    }
    event.data.compaction_interrupted.reason = SNAG_BINARY_COMPACT_STOP_STEERING;
    payload.max = 64u;
    assert(!snag_binary_event_encode(&payload, &event) && payload.len == 17u);
    unsigned char interrupted[17] = {[0] = 9u, [16] = 1u};
    assert(!memcmp(payload.data, interrupted, sizeof(interrupted)));
    payload.data[16] = 6u;
    record = (struct snag_binary_record){.kind = 225u, .version = 1u,
        .payload = payload.data, .size = payload.len};
    assert_rejected(record);
    snag_buf_free(&payload);
}

static void
compact_output(struct snag_binary_compact_complete *value, struct snag_binary_text output)
{
    struct snag_sha256 hash;
    value->output = output;
    snag_sha256_init(&hash);
    snag_sha256_update(&hash, output.data, output.size);
    snag_sha256_final(&hash, value->output_sha256);
}

static void
test_compact_complete(void)
{
    struct snag_binary_event event = {.kind = SNAG_BINARY_COMPACTION_COMPLETED};
    struct snag_binary_compact_complete *value = &event.data.compaction_completed;
    value->id[0] = 1u;
    value->count_method = SNAG_BINARY_COUNT_EXACT;
    value->output_count_method = SNAG_BINARY_COUNT_MEDIA_UPPER_BOUND;
    value->input_tokens = 3u;
    value->output_tokens = 2u;
    value->output_count_request_sha256[0] = 4u;
    value->source_sha256[0] = 5u;
    value->scope[0] = 6u;
    compact_output(value, text("[{\"type\":\"\"}]"));
    const unsigned char digest[32] = {0x53, 0x05, 0xc2, 0x91, 0x79, 0xe9, 0xf8, 0x6f,
        0xb1, 0x6d, 0x5c, 0xfe, 0x1e, 0x48, 0x61, 0x85, 0xfe, 0x8c, 0xa0, 0xe5, 0x21, 0x23,
        0x2c, 0x8a, 0xc6, 0x7c, 0x9d, 0xc8, 0xc7, 0xfe, 0xb3, 0xbc};
    unsigned char expected[148] = {[0] = 1u, [17] = 1u, [18] = 2u, [19] = 3u, [27] = 2u,
        [35] = 4u, [67] = 5u, [131] = 13u};
    memcpy(expected + 99u, digest, sizeof(digest));
    memcpy(expected + 135u, "[{\"type\":\"\"}]", 13u);
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    assert(payload.len == sizeof(expected) && !memcmp(payload.data, expected, sizeof(expected)));
    struct snag_binary_record record = {.kind = 226u, .version = 1u,
        .payload = payload.data, .size = payload.len};
    const size_t positions[] = {16u, 17u, 18u, 19u, 26u, 34u, 99u, 134u, 143u};
    const unsigned char corrupt[] = {2u, 4u, 4u, 0u, 128u, 128u, 0u, 1u, 0u};
    for (size_t i = 0u; i < sizeof(positions) / sizeof(positions[0]); ++i) {
        unsigned char saved = payload.data[positions[i]];
        payload.data[positions[i]] = corrupt[i];
        assert_rejected(record);
        payload.data[positions[i]] = saved;
    }
    roundtrip(&event);
    value->has_scope = true;
    roundtrip(&event);
    payload.len = 0u;
    assert(!snag_binary_event_encode(&payload, &event) && payload.len == 180u);
    assert(payload.data[16] == 1u && payload.data[131] == 6u && payload.data[163] == 13u);
    for (unsigned int method = 0u; method <= 7u; ++method) {
        for (unsigned int output_method = 0u; output_method <= 7u; ++output_method) {
            value->count_method = (enum snag_binary_count_method)method;
            value->output_count_method = (enum snag_binary_count_method)output_method;
            for (unsigned int counts = 0u; counts < 4u; ++counts) {
                value->input_tokens = counts & 1u ? 3u : 0u;
                value->output_tokens = counts & 2u ? 2u : 0u;
                bool valid = method >= 1u && method <= 6u && method != 4u &&
                    output_method >= 1u && output_method <= 6u && output_method != 4u &&
                    (method == 3u ? !value->input_tokens : !!value->input_tokens) &&
                    (output_method == 3u ? !value->output_tokens : !!value->output_tokens);
                if (valid) roundtrip(&event);
                else assert_event_encode_rejected(&event);
            }
        }
    }
    value->count_method = value->output_count_method = SNAG_BINARY_COUNT_EXACT;
    value->input_tokens = value->output_tokens = INT64_MAX;
    roundtrip(&event);
    value->input_tokens++;
    assert_event_encode_rejected(&event);
    value->input_tokens = 1u;
    value->output_tokens++;
    assert_event_encode_rejected(&event);
    value->output_tokens = 1u;
    const char *invalid[] = {"[]", "{}", "[{}]", "[1]", "[{\"type\":1}]",
        "[{\"type\":null}]", " [{\"type\":\"\"}]", "[{\"z\":0,\"type\":\"x\"}]",
        "[{\"type\":\"x\",\"type\":\"y\"}]", "[{\"type\":\"x\",\"z\":0.1}]",
        "[{\"type\":\"\\u0000\"}]"};
    for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        compact_output(value, text(invalid[i]));
        assert_event_encode_rejected(&event);
    }
    compact_output(value, text("[{\"encrypted_content\":\"kept\","
        "\"new\":[false,{\"n\":-1}],\"type\":\"future\"}]"));
    roundtrip(&event);
    value->output_sha256[0] ^= 1u;
    assert_event_encode_rejected(&event);
    struct snag_buf array = {.max = 4096u};
    for (size_t count = 128u; count <= 129u; ++count) {
        array.len = 0u;
        assert(!snag_buf_putc(&array, '['));
        for (size_t i = 0u; i < count; ++i) {
            if (i) assert(!snag_buf_putc(&array, ','));
            assert(!snag_buf_append(&array, "{\"type\":\"\"}", 11u));
        }
        assert(!snag_buf_putc(&array, ']'));
        compact_output(value, (struct snag_binary_text){array.data, array.len});
        if (count == 128u) roundtrip(&event);
        else assert_event_encode_rejected(&event);
    }
    snag_buf_free(&array);
    const size_t maximum = 12u * 1024u * 1024u;
    unsigned char *large = malloc(maximum + 1u);
    assert(large);
    memset(large, 'x', maximum + 1u);
    memcpy(large, "[{\"type\":\"", 10u);
    memcpy(large + maximum - 3u, "\"}]", 3u);
    compact_output(value, (struct snag_binary_text){large, maximum});
    payload.len = 0u;
    assert(!snag_binary_event_encode(&payload, &event) && payload.len == maximum + 167u);
    struct snag_binary_event decoded;
    record = (struct snag_binary_record){.kind = 226u, .version = 1u,
        .payload = payload.data, .size = payload.len};
    assert(!snag_binary_event_decode(&record, &decoded));
    assert(decoded.data.compaction_completed.output.size == maximum);
    assert(!memcmp(decoded.data.compaction_completed.output.data, large, maximum));
    large[maximum - 3u] = 'x';
    memcpy(large + maximum - 2u, "\"}]", 3u);
    compact_output(value, (struct snag_binary_text){large, maximum + 1u});
    assert_event_encode_rejected(&event);
    free(large);
    snag_buf_free(&payload);
}

static void
assert_compact_reference_rejected(const struct snag_binary_ref *reference,
    const struct snag_binary_batch *batch)
{
    struct snag_binary_compact_complete source;
    memset(&source, 0x5a, sizeof(source));
    unsigned char saved[sizeof(source)];
    memcpy(saved, &source, sizeof(source));
    assert(snag_binary_compact_ref_resolve(reference, batch, &source) < 0);
    assert(!memcmp(saved, &source, sizeof(source)));
}

static void
assert_compact_original_missing(const struct snag_binary_batch *batch, uint64_t sequence)
{
    struct snag_binary_ref reference = {99u, 98u, 97u};
    assert(snag_binary_compact_ref_create(batch, sequence, &reference) < 0);
    assert(reference.sequence == 99u && reference.offset == 98u && reference.size == 97u);
    reference = (struct snag_binary_ref){sequence, 135u, 13u};
    assert_compact_reference_rejected(&reference, batch);
}

static void
test_compact_references(void)
{
    struct snag_binary_event event = {.kind = SNAG_BINARY_COMPACTION_COMPLETED};
    struct snag_binary_compact_complete *value = &event.data.compaction_completed;
    value->id[0] = 1u;
    value->count_method = value->output_count_method = SNAG_BINARY_COUNT_EXACT;
    value->input_tokens = 3u;
    value->output_tokens = 2u;
    value->source_sha256[0] = 4u;
    value->output_count_request_sha256[0] = 5u;
    value->scope[0] = 6u;
    compact_output(value, text("[{\"type\":\"\"}]"));
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_binary_batch batch;
    for (unsigned int scope = 0u; scope < 2u; ++scope) {
        value->has_scope = scope != 0u;
        payload.len = 0u;
        assert(!snag_binary_event_encode(&payload, &event));
        struct snag_binary_record record = {.kind = 226u, .version = 1u,
            .payload = payload.data, .size = payload.len};
        input_reference_batch(&record, &bytes, &batch);
        struct snag_binary_ref reference;
        assert(!snag_binary_compact_ref_create(&batch, 2u, &reference));
        uint32_t offset = scope ? 167u : 135u;
        assert(reference.sequence == 2u && reference.offset == offset && reference.size == 13u);
        struct snag_binary_compact_complete source;
        assert(!snag_binary_compact_ref_resolve(&reference, &batch, &source));
        assert(source.id[0] == 1u && source.input_tokens == 3u && source.output_tokens == 2u &&
            source.source_sha256[0] == 4u && source.output_count_request_sha256[0] == 5u &&
            source.count_method == SNAG_BINARY_COUNT_EXACT &&
            source.output_count_method == SNAG_BINARY_COUNT_EXACT && source.has_scope == !!scope);
        assert(!scope || source.scope[0] == 6u);
        assert(source.output.size == 13u && !memcmp(source.output.data, value->output.data, 13u));
        assert(!memcmp(source.output_sha256, value->output_sha256, 32u));
        const struct snag_binary_ref bad[] = {
            {2u, offset + 1u, 12u}, {2u, offset, 12u}, {2u, offset, 14u}, {2u, offset, 0u},
            {2u, offset - 4u, 17u}, {2u, 0u, 13u}, {2u, offset + 1u, 11u},
            {2u, UINT32_MAX, 13u}, {2u, offset, UINT32_MAX},
            {0u, offset, 13u}, {1u, offset, 13u}, {3u, offset, 13u}, {UINT64_MAX, offset, 13u}
        };
        for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
            assert_compact_reference_rejected(&bad[i], &batch);
        }
        assert_input_leaf_missing(&batch, 2u, SNAG_BINARY_INPUT_TEXT);
        assert_output_reference_rejected(&reference, &batch);
        assert_graph_reference_rejected(&reference, &batch, SNAG_BINARY_ITEM_ASSISTANT);
        assert_continuation_reference_rejected(&reference, &batch);
        assert_process_reference_rejected(&reference, &batch);
        assert_result_original_missing(&batch, 2u, SNAG_BINARY_TOOL_FINISHED);
        assert_compact_original_missing(NULL, 2u);
        assert_compact_original_missing(&batch, 0u);
        assert_compact_original_missing(&batch, 1u);
        assert_compact_original_missing(&batch, 3u);
        assert_compact_original_missing(&batch, UINT64_MAX);
        assert_compact_reference_rejected(NULL, &batch);
        assert(snag_binary_compact_ref_create(&batch, 2u, NULL) < 0);
        assert(snag_binary_compact_ref_resolve(&reference, &batch, NULL) < 0);
        for (size_t i = 0u; i < payload.len; ++i) {
            record.size = i;
            input_reference_batch(&record, &bytes, &batch);
            assert_compact_original_missing(&batch, 2u);
        }
        record.size = payload.len;
        payload.data[99] ^= 1u;
        input_reference_batch(&record, &bytes, &batch);
        assert_compact_original_missing(&batch, 2u);
        payload.data[99] ^= 1u;
        record.version = 2u;
        input_reference_batch(&record, &bytes, &batch);
        assert_compact_original_missing(&batch, 2u);
        record.version = 1u;
        record.flags = SNAG_BINARY_RECORD_OPTIONAL;
        input_reference_batch(&record, &bytes, &batch);
        assert_compact_original_missing(&batch, 2u);
        record.flags = 0u;
        record.kind = 224u;
        input_reference_batch(&record, &bytes, &batch);
        assert_compact_original_missing(&batch, 2u);
    }
    event = (struct snag_binary_event){.kind = SNAG_BINARY_INPUT_RECEIVED};
    event.data.input.origin = SNAG_BINARY_INPUT_DEFAULT;
    event.data.input.selection = (struct snag_binary_selection){text("p"), text("m"), text("e")};
    event.data.input.text = text("[{\"type\":\"\"}]");
    struct snag_buf instructions = {.max = 16u};
    assert(!snag_binary_instructions_encode(&instructions, NULL, 0u));
    event.data.input.instructions = (struct snag_binary_instructions){
        instructions.data, instructions.len};
    payload.len = 0u;
    assert(!snag_binary_event_encode(&payload, &event));
    struct snag_binary_record record = {.kind = 96u, .version = 2u,
        .payload = payload.data, .size = payload.len};
    input_reference_batch(&record, &bytes, &batch);
    assert_compact_original_missing(&batch, 2u);
    snag_buf_free(&instructions);
    snag_buf_free(&bytes);
    snag_buf_free(&payload);
}

static void
test_event_names(void)
{
    static const struct {
        uint16_t kind;
        const char *name;
    } expected[] = {
        {1u, "session_created"}, {2u, "cwd_changed"}, {3u, "session_archived"},
        {4u, "session_unarchived"}, {5u, "session_delete_requested"}, {6u, "banner_updated"},
        {7u, "steering_updated"}, {8u, "model_selection_changed"}, {9u, "turn_model_changed"},
        {10u, "effort_changed"}, {11u, "context_selection_changed"}, {12u, "command_shell_changed"},
        {13u, "session_named"}, {14u, "session_options"},
        {16u, "control_requested"}, {17u, "control_started"}, {18u, "control_finished"},
        {32u, "timer_scheduled"}, {33u, "timer_fired"}, {34u, "timer_cancelled"},
        {64u, "goal_started"}, {65u, "goal_replaced"}, {66u, "goal_reworded"},
        {67u, "goal_lock_changed"}, {68u, "goal_paused"}, {69u, "goal_blocked"},
        {70u, "goal_completed"}, {71u, "goal_resumed"}, {72u, "goal_cancelled"},
        {96u, "input_received"}, {97u, "input_cancelled"}, {98u, "steering_added"},
        {99u, "irc_reply_reminder"}, {100u, "steering_deferred"}, {101u, "input_admitted"},
        {102u, "future_queue_state"}, {103u, "future_turn_cancelled"},
        {104u, "future_turn_queued"}, {105u, "future_turn_edited"},
        {128u, "turn_started"}, {129u, "turn_yield_requested"}, {130u, "turn_cancel_requested"},
        {131u, "turn_recovery"}, {132u, "turn_completed"}, {133u, "turn_completed_silent"},
        {134u, "turn_interrupted"}, {135u, "turn_failed"}, {160u, "response_started"},
        {161u, "response_output"}, {162u, "response_interrupted"}, {163u, "response_failed"},
        {164u, "response_output_correction"}, {165u, "response_completed"},
        {166u, "response_capacity_rejected"}, {167u, "hosted_search_started"},
        {168u, "hosted_search_finished"}, {176u, "tool_started"}, {177u, "tool_finished"},
        {192u, "process_output"}, {193u, "process_closed"}, {208u, "irc_event"},
        {209u, "irc_snapshot"}, {210u, "irc_admitted"}, {224u, "compaction_started"},
        {225u, "compaction_interrupted"}, {226u, "compaction_completed"}, {227u, "context_rebased"},
        {240u, "download_queued"}, {241u, "download_removed"}, {242u, "downloads_cleared"},
        {248u, "rule_log"}, {249u, "rule_transform"}, {256u, "audio_usage"}, {257u, "voice_event"},
        {264u, "voice_transfer_record"}, {265u, "voice_transfer_sealed"},
        {266u, "voice_transfer_adopted"}
    };
    for (size_t i = 0u; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        const char *name = snag_binary_event_name((enum snag_binary_kind)expected[i].kind);
        assert(name && !strcmp(name, expected[i].name));
        enum snag_binary_kind kind = SNAG_BINARY_GOAL_CANCELLED;
        assert(!snag_binary_event_kind(expected[i].name, &kind) && kind == expected[i].kind);
    }
    size_t count = 0u;
    for (uint32_t i = 0u; i <= UINT16_MAX; ++i) {
        enum snag_binary_kind kind = (enum snag_binary_kind)i;
        const char *name = snag_binary_event_name(kind);
        assert(!!name == !!snag_binary_event_version(kind));
        if (name) count++;
    }
    assert(count == sizeof(expected) / sizeof(expected[0]));
    assert(!snag_binary_event_name((enum snag_binary_kind)-1));
    const char *invalid[] = {"", "goal_changed", "model_selected", "voice_caption",
        "timer_fired ", "session_created\n", "GOAL_STARTED", "unknown"};
    for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        enum snag_binary_kind kind = SNAG_BINARY_GOAL_CANCELLED;
        assert(snag_binary_event_kind(invalid[i], &kind) < 0 && kind == SNAG_BINARY_GOAL_CANCELLED);
    }
    enum snag_binary_kind kind = SNAG_BINARY_GOAL_CANCELLED;
    assert(snag_binary_event_kind(NULL, &kind) < 0 && kind == SNAG_BINARY_GOAL_CANCELLED);
    assert(snag_binary_event_kind("goal_started", NULL) < 0);
}

static void
assert_voice_body_rejected(const void *data, size_t size)
{
    struct snag_binary_voice_body out;
    memset(&out, 0xa5, sizeof(out));
    unsigned char saved[sizeof(out)];
    memcpy(saved, &out, sizeof(out));
    assert(snag_binary_voice_body_decode(data, size, &out) < 0);
    assert(!memcmp(saved, &out, sizeof(out)));
}

static void
assert_voice_json_rejected(const json_t *body)
{
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_buf_append(&bytes, "keep", 4u));
    assert(snag_binary_voice_body_encode(&bytes, body) < 0);
    assert(bytes.len == 4u && !memcmp(bytes.data, "keep", 4u));
    snag_buf_free(&bytes);
}

static void
assert_voice_roundtrip(const json_t *body)
{
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_voice_body_encode(&bytes, body));
    struct snag_binary_voice_body decoded;
    assert(!snag_binary_voice_body_decode(bytes.data, bytes.len, &decoded));
    json_t *copy = NULL;
    assert(!snag_binary_voice_body_json(&decoded, &copy) && json_equal(body, copy));
    struct snag_buf encoded = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_voice_body_encode(&encoded, copy));
    assert(encoded.len == bytes.len && !memcmp(encoded.data, bytes.data, bytes.len));
    if (bytes.len <= 256u) {
        for (size_t n = 0u; n < bytes.len; ++n) {
            assert_voice_body_rejected(bytes.data, n);
        }
    }
    json_t *legacy = json_pack("{s:{s:s,s:s,s:s,s:O}}", "data", "connection_id",
        "00000000000000000000000000000001", "provider", "p", "model", "m", "event", body);
    struct snag_buf canonical = {.max = SNAG_MAX_EVENT_LINE};
    assert(legacy && !snag_json_canonical(legacy, &canonical));
    struct snag_binary_event event = {.kind = SNAG_BINARY_VOICE_EVENT};
    event.data.voice_event = (struct snag_binary_voice_event){.connection = {1u},
        .provider = text("p"), .model = text("m"), .body = decoded};
    if (bytes.len <= 256u) roundtrip(&event);
    encoded.len = 0u;
    assert(!snag_binary_event_encode(&encoded, &event));
    struct snag_binary_record record = {.kind = 257u, .version = 1u,
        .payload = encoded.data, .size = encoded.len};
    struct snag_binary_event again;
    assert(!snag_binary_event_decode(&record, &again));
    json_t *inner = NULL;
    assert(!snag_binary_voice_body_json(&again.data.voice_event.body, &inner));
    assert(json_equal(body, inner));
    json_decref(inner);
    json_decref(legacy);
    json_decref(copy);
    snag_buf_free(&canonical);
    snag_buf_free(&encoded);
    snag_buf_free(&bytes);
}

static void
test_voice_fields(void)
{
    const char *const names[] = {"arguments", "call_id", "covered_next_seq", "covered_offset",
        "covered_sha256", "delay_ms", "disposition", "effort", "error", "http_status", "input",
        "input_id", "interrupted", "item_id", "item_index", "kind", "metrics", "model", "muted",
        "operation", "output", "output_id", "played_ms", "presentation", "provider", "queue_id",
        "reason", "reply", "report", "response_id", "result", "retry_after_ms", "source",
        "source_as_of_seq", "source_call_id", "source_through_seq", "speaker", "status", "stream",
        "submitted_id", "summary", "target_session_id", "text", "tool", "tool_call_id", "turn_id"};
    const char *const kinds[] = {
        "voice_started", "voice_stopped", "voice_transcript", "voice_usage",
        "voice_asr_failed", "voice_interrupted", "voice_response", "voice_result", "voice_muted"};
    json_t *all = json_pack("{s:s}", "type", "voice_response");
    assert(all && sizeof(names) / sizeof(names[0]) == 46u);
    for (size_t k = 0u; k < sizeof(kinds) / sizeof(kinds[0]); ++k) {
        json_t *body = json_pack("{s:s}", "type", kinds[k]);
        assert(body);
        assert_voice_roundtrip(body);
        for (size_t i = 0u; i < sizeof(names) / sizeof(names[0]); ++i) {
            assert(!json_object_set_new(body, names[i], json_integer((json_int_t)i)));
            struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
            assert(!snag_binary_voice_body_encode(&bytes, body));
            unsigned char expected[12] = {[9] = (unsigned char)(128u + i), [10] = 6u, [11] = 7u};
            expected[0] = (unsigned char)(k + 1u);
            expected[1u + i / 8u] = (unsigned char)(1u << (i % 8u));
            assert(bytes.len == sizeof(expected) &&
                !memcmp(bytes.data, expected, sizeof(expected)));
            assert_voice_roundtrip(body);
            assert(!json_object_del(body, names[i]));
            snag_buf_free(&bytes);
        }
        json_decref(body);
    }
    for (size_t i = 0u; i < sizeof(names) / sizeof(names[0]); ++i) {
        assert(!json_object_set_new(all, names[i], json_integer((json_int_t)i)));
    }
    assert_voice_roundtrip(all);
    json_decref(all);
    json_t *body = json_pack("{s:s,s:n,s:b,s:[n,{s:s,s:i}]}", "type", "voice_response",
        "", "x", 1, "extra", "type", "nested", "text", 9);
    assert(body);
    assert_voice_roundtrip(body);
    json_t *values[] = {json_null(), json_true(), json_false(), json_integer(INT64_MIN),
        json_string("\xce\xbb\n"), json_array(), json_pack("{s:i}", "value", 7)};
    for (size_t i = 0u; i < sizeof(values) / sizeof(values[0]); ++i) {
        assert(values[i] && !json_object_set(body, "text", values[i]));
        assert(!json_object_set(body, "extra", values[i]));
        assert_voice_roundtrip(body);
        json_decref(values[i]);
    }
    json_decref(body);
    body = json_pack("{s:s}", "type", "voice_started");
    assert(body);
    for (size_t i = 0u; i <= sizeof(names) / sizeof(names[0]); ++i) {
        const char *name = i == sizeof(names) / sizeof(names[0]) ? "type" : names[i];
        json_t *shadow = json_pack("{s:n}", name);
        struct snag_buf extensions = {.max = SNAG_MAX_EVENT_LINE};
        assert(shadow && !snag_binary_rule_value_encode(&extensions, shadow));
        struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
        const unsigned char header[9] = {1u};
        assert(!snag_buf_append(&bytes, header, sizeof(header)));
        assert(!snag_buf_append(&bytes, extensions.data, extensions.len));
        assert_voice_body_rejected(bytes.data, bytes.len);
        struct snag_binary_event event = {.kind = SNAG_BINARY_VOICE_EVENT};
        event.data.voice_event.provider = text("p");
        event.data.voice_event.model = text("m");
        event.data.voice_event.body.kind = SNAG_BINARY_VOICE_STARTED;
        event.data.voice_event.body.extensions = (struct snag_binary_result_value){
            .data = extensions.data, .size = extensions.len};
        assert_event_encode_rejected(&event);
        json_t *out = json_true();
        assert(snag_binary_voice_body_json(&event.data.voice_event.body, &out) < 0);
        assert(out == json_true());
        snag_buf_free(&bytes);
        snag_buf_free(&extensions);
        json_decref(shadow);
    }
    json_decref(body);
}

static void
test_voice_event_boundaries(void)
{
    struct snag_binary_event event = {.kind = SNAG_BINARY_VOICE_EVENT};
    event.data.voice_event = (struct snag_binary_voice_event){.connection = {1u},
        .provider = text("p"), .model = text("m"), .body.kind = SNAG_BINARY_VOICE_MUTED};
    const unsigned char truth = 2u;
    event.data.voice_event.body.fields.muted = (struct snag_binary_result_value){
        .data = &truth, .size = 1u};
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&bytes, &event));
    const unsigned char expected[38] = {[0] = 1u, [16] = 1u, [20] = 'p', [21] = 1u, [25] = 'm',
        [26] = 9u, [29] = 4u, [35] = 2u, [36] = 6u, [37] = 7u};
    assert(bytes.len == sizeof(expected) && !memcmp(bytes.data, expected, sizeof(expected)));
    roundtrip(&event);
    event.data.voice_event.body.fields.muted.kind = SNAG_BINARY_RESULT_FALSE;
    json_t *copy = NULL;
    assert(!snag_binary_voice_body_json(&event.data.voice_event.body, &copy));
    assert(json_is_true(json_object_get(copy, "muted")));
    json_decref(copy);
    unsigned char invalid[11] = {[0] = 1u, [9] = 6u, [10] = 7u};
    for (unsigned int i = 46u; i < 64u; ++i) {
        invalid[1u + i / 8u] = (unsigned char)(1u << (i % 8u));
        assert_voice_body_rejected(invalid, sizeof(invalid));
        invalid[1u + i / 8u] = 0u;
    }
    invalid[0] = 0u;
    assert_voice_body_rejected(invalid, sizeof(invalid));
    invalid[0] = 10u;
    assert_voice_body_rejected(invalid, sizeof(invalid));
    assert_voice_body_rejected(NULL, 0u);
    assert(snag_binary_voice_body_decode(expected + 26u, 12u, NULL) < 0);
    assert(snag_binary_voice_body_encode(NULL, json_null()) < 0);
    assert(snag_binary_voice_body_json(&event.data.voice_event.body, NULL) < 0);
    copy = json_true();
    assert(snag_binary_voice_body_json(NULL, &copy) < 0 && copy == json_true());
    assert_voice_json_rejected(NULL);
    assert_voice_json_rejected(json_true());
    json_t *body = json_pack("{s:s}", "type", "voice_unknown");
    assert(body);
    assert_voice_json_rejected(body);
    assert(!json_object_set_new(body, "type", json_stringn("voice_muted\0x", 13u)));
    assert_voice_json_rejected(body);
    assert(!json_object_set_new(body, "type", json_string("voice_muted")));
    assert(!json_object_set_new(body, "muted", json_real(1.5)));
    assert_voice_json_rejected(body);
    assert(!json_object_del(body, "muted"));
    assert(!json_object_set_new_nocheck(body, "\xff", json_null()));
    assert_voice_json_rejected(body);
    json_decref(body);

    json_t *deep = json_null();
    for (size_t i = 0u; i < 45u; ++i) {
        json_t *array = json_array();
        assert(array && !json_array_append_new(array, deep));
        deep = array;
    }
    body = json_pack("{s:s,s:O,s:O}", "type", "voice_transcript", "text", deep, "extra", deep);
    assert(body);
    assert_voice_roundtrip(body);
    json_t *array = json_array();
    assert(array && !json_array_append_new(array, deep));
    assert(!json_object_set(body, "text", array));
    assert_voice_json_rejected(body);
    assert(!json_object_del(body, "text") && !json_object_set(body, "extra", array));
    assert_voice_json_rejected(body);
    json_decref(array);
    json_decref(body);

    unsigned char *large = malloc(SNAG_MAX_EVENT_LINE);
    assert(large);
    memset(large, 'x', SNAG_MAX_EVENT_LINE);
    body = json_pack("{s:s,s:o}", "type", "voice_transcript", "text",
        json_stringn((const char *)large, 2u * 1024u * 1024u + 1u));
    assert(body);
    assert_voice_roundtrip(body);
    json_decref(body);
    event.data.voice_event.body = (struct snag_binary_voice_body){
        .kind = SNAG_BINARY_VOICE_TRANSCRIPT};
    large[0] = 4u;
    size_t size = SNAG_MAX_EVENT_LINE - 37u;
    large[size - 1u] = 0u;
    event.data.voice_event.body.fields.text = (struct snag_binary_result_value){
        .data = large, .size = size};
    bytes.len = 0u;
    assert(!snag_binary_event_encode(&bytes, &event) && bytes.len == SNAG_MAX_EVENT_LINE);
    struct snag_binary_record record = {.kind = 257u, .version = 1u,
        .payload = bytes.data, .size = bytes.len};
    struct snag_binary_event decoded;
    assert(!snag_binary_event_decode(&record, &decoded));
    large[size - 1u] = 'x';
    large[size] = 0u;
    event.data.voice_event.body.fields.text.size++;
    assert_event_encode_rejected(&event);
    free(large);
    snag_buf_free(&bytes);
}

static void
reject_archive_fields(uint16_t kind, const unsigned char *data, size_t size)
{
    struct snag_binary_archive_fields decoded;
    memset(&decoded, 0xa5, sizeof(decoded));
    struct snag_binary_archive_fields before = decoded;
    assert(snag_binary_archive_fields_decode(kind, data, size, &decoded) < 0);
    assert(!memcmp(&decoded, &before, sizeof(decoded)));
    struct snag_binary_archive_fields fields = {.kind = kind, .data = data, .size = size};
    json_t *value = json_true();
    assert(snag_binary_archive_fields_json(&fields, &value) < 0 && value == json_true());
}

static void
test_public_archive_fields(void)
{
    static const unsigned char empty[] = {6u, 7u};
    static const unsigned char checkpoint[] = {7u, 3u, 0x82u, 4u, 2u, 0x82u, 6u, 7u};
    static const unsigned char response[] = {0x18u, 2u, 5u, 7u, 2u, 0u, 6u, 7u};
    static const unsigned char adopted[] = {0xf0u, 0u, 2u, 3u, 0x82u, 4u,
        4u, 'a', 0u, 4u, 'b', 0u, 6u, 7u};
    static const unsigned char foreign[] = {4u, 0u, 0u, 0u, 'o', 'd', 'd', 1u,
        6u, 4u, 0u, 1u, 7u};
    static const struct {
        const char *type;
        uint16_t kind;
        const char *json;
        const unsigned char *bytes;
        size_t size;
    } cases[] = {
        {"input_cancelled", 97u, "{}", empty, sizeof(empty)},
        {"session_checkpoint", 0x7fffu, "{\"covers_through_seq\":257,\"provider_view\":true,"
            "\"snapshot_v\":2}", checkpoint, sizeof(checkpoint)},
        {"response_completed", 165u, "{\"items\":[],\"provider_payload_omitted\":true,"
            "\"usage\":null}", response, sizeof(response)},
        {"voice_transfer_adopted", 266u, "{\"session_boundary\":true,\"source_as_of_seq\":257,"
            "\"source_session_id\":\"a\",\"target_session_id\":\"b\"}", adopted, sizeof(adopted)},
        {"odd\001", 0u, "{\"\":false}", foreign, sizeof(foreign)}
    };
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        json_t *source = json_loads(cases[i].json, 0u, NULL);
        struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
        uint16_t kind = UINT16_MAX;
        assert(source && !snag_binary_archive_fields_encode(&bytes, cases[i].type, source, &kind));
        assert(kind == cases[i].kind && bytes.len == cases[i].size &&
            !memcmp(bytes.data, cases[i].bytes, bytes.len));
        struct snag_binary_archive_fields fields;
        assert(!snag_binary_archive_fields_decode(kind, bytes.data, bytes.len, &fields));
        assert(fields.type.size == strlen(cases[i].type) &&
            !memcmp(fields.type.data, cases[i].type, fields.type.size));
        fields.type = (struct snag_binary_text){0};
        fields.present = UINT64_MAX;
        memset(fields.values, 0xa5, sizeof(fields.values));
        memset(&fields.extensions, 0xa5, sizeof(fields.extensions));
        json_t *projected = NULL;
        assert(!snag_binary_archive_fields_json(&fields, &projected));
        assert(json_equal(source, projected));
        json_decref(projected);
        for (size_t n = 0u; n < bytes.len; ++n) reject_archive_fields(kind, bytes.data, n);
        struct snag_buf small = {.max = 1u};
        assert(!snag_buf_putc(&small, 'K'));
        kind = UINT16_MAX;
        assert(snag_binary_archive_fields_encode(&small, cases[i].type, source, &kind) < 0);
        assert(kind == UINT16_MAX && small.len == 1u && small.data[0] == 'K');
        snag_buf_free(&small);
        json_decref(source);
        snag_buf_free(&bytes);
    }
    const unsigned char reserved[] = {8u, 6u, 7u};
    const unsigned char shadow[] = {0u, 6u, 4u,
        's', 'n', 'a', 'p', 's', 'h', 'o', 't', '_', 'v', 0u, 0u, 7u};
    const unsigned char duplicate[] = {6u, 4u, 'a', 0u, 0u, 4u, 'a', 0u, 1u, 7u};
    const unsigned char array[] = {5u, 7u};
    reject_archive_fields(0x7fffu, reserved, sizeof(reserved));
    reject_archive_fields(0x7fffu, shadow, sizeof(shadow));
    reject_archive_fields(97u, duplicate, sizeof(duplicate));
    reject_archive_fields(97u, array, sizeof(array));
    reject_archive_fields(264u, empty, sizeof(empty));
    reject_archive_fields(265u, empty, sizeof(empty));
    reject_archive_fields(0x8000u, empty, sizeof(empty));
    const char *known[] = {"session_checkpoint", "response_completed", "input_cancelled",
        "voice_transfer_record", "voice_transfer_sealed"};
    for (size_t i = 0u; i < sizeof(known) / sizeof(known[0]); ++i) {
        unsigned char forged[64] = {0};
        size_t length = strlen(known[i]);
        forged[0] = (unsigned char)length;
        memcpy(forged + 4u, known[i], length);
        memcpy(forged + 4u + length, empty, sizeof(empty));
        reject_archive_fields(0u, forged, 4u + length + sizeof(empty));
    }
    unsigned char malformed[sizeof(foreign)];
    memcpy(malformed, foreign, sizeof(malformed));
    malformed[4] = 0u;
    reject_archive_fields(0u, malformed, sizeof(malformed));
    malformed[4] = 0xffu;
    reject_archive_fields(0u, malformed, sizeof(malformed));
    memcpy(malformed, foreign, sizeof(malformed));
    malformed[0] = 0u;
    reject_archive_fields(0u, malformed, sizeof(malformed));
    assert(!snag_binary_archive_field_name(0u, 0u));
    assert(!strcmp(snag_binary_archive_field_name(0x7fffu, 2u), "snapshot_v"));
    assert(!snag_binary_archive_field_name(0x7fffu, 3u));

    struct snag_binary_event event = {.kind = SNAG_BINARY_VOICE_TRANSFER_RECORD};
    event.data.voice_transfer_record = (struct snag_binary_voice_archive){
        .id = {1u}, .target = {2u}, .source = {3u}, .source_seq = 4u,
        .source_kind = 0x7fffu, .source_version = 0x8001u,
        .data = checkpoint, .size = sizeof(checkpoint)};
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&bytes, &event));
    unsigned char expected[72] = {[0] = 1u, [16] = 2u, [32] = 3u, [48] = 4u,
        [56] = 0xffu, [57] = 0x7fu, [58] = 1u, [59] = 0x80u, [60] = 8u};
    memcpy(expected + 64u, checkpoint, sizeof(checkpoint));
    assert(bytes.len == sizeof(expected) && !memcmp(bytes.data, expected, sizeof(expected)));
    roundtrip(&event);
    event.data.voice_transfer_record.source_version = 0x8002u;
    assert_event_encode_rejected(&event);
    struct snag_binary_record not_executable = {.kind = 165u, .version = 0x8001u,
        .payload = response, .size = sizeof(response)};
    struct snag_binary_event decoded;
    assert(snag_binary_event_decode(&not_executable, &decoded) < 0);
    snag_buf_free(&bytes);
}

static void
test_voice_archives(void)
{
    const unsigned char source[15] = {1u, 0u, 0u, 0u, 'p', 1u, 0u, 0u, 0u, 'm',
        1u, 0u, 0u, 0u, 'r'};
    struct snag_binary_event event = {.kind = SNAG_BINARY_VOICE_TRANSFER_RECORD};
    event.data.voice_transfer_record = (struct snag_binary_voice_archive){
        .id = {1u}, .target = {2u}, .source = {3u}, .source_seq = 4u,
        .source_kind = 256u, .source_version = 1u, .data = source, .size = sizeof(source)};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    unsigned char expected[79] = {[0] = 1u, [16] = 2u, [32] = 3u, [48] = 4u,
        [57] = 1u, [58] = 1u, [60] = 15u};
    memcpy(expected + 64u, source, sizeof(source));
    assert(payload.len == sizeof(expected) && !memcmp(payload.data, expected, sizeof(expected)));
    roundtrip(&event);
    struct snag_binary_record record = {.kind = 264u, .version = 1u,
        .payload = payload.data, .size = payload.len};
    struct snag_binary_event decoded;
    assert(!snag_binary_event_decode(&record, &decoded));
    assert(decoded.data.voice_transfer_record.data == payload.data + 64u &&
        decoded.data.voice_transfer_record.size == sizeof(source));
    const size_t corrupt[] = {48u, 57u, 58u, 60u, 64u, 78u};
    for (size_t i = 0u; i < sizeof(corrupt) / sizeof(corrupt[0]); ++i) {
        payload.data[corrupt[i]] = 0u;
        assert_rejected(record);
        memcpy(payload.data, expected, sizeof(expected));
    }
    payload.data[60] = 16u;
    assert_rejected(record);
    memcpy(payload.data, expected, sizeof(expected));
    payload.data[55] = 0x80u;
    assert_rejected(record);
    memcpy(payload.data, expected, sizeof(expected));
    const uint16_t invalid_kinds[] = {0u, 264u, 265u, 0x7fffu, 0x8000u, 0xffffu};
    for (size_t i = 0u; i < sizeof(invalid_kinds) / sizeof(invalid_kinds[0]); ++i) {
        event.data.voice_transfer_record.source_kind = invalid_kinds[i];
        assert_event_encode_rejected(&event);
        payload.data[56] = (unsigned char)invalid_kinds[i];
        payload.data[57] = (unsigned char)(invalid_kinds[i] >> 8u);
        assert_rejected(record);
    }
    event.data.voice_transfer_record.source_kind = 256u;
    event.data.voice_transfer_record.source_version = 0u;
    assert_event_encode_rejected(&event);
    event.data.voice_transfer_record.source_version = 2u;
    assert_event_encode_rejected(&event);
    event.data.voice_transfer_record.source_version = 1u;
    event.data.voice_transfer_record.source_seq = 0u;
    assert_event_encode_rejected(&event);
    event.data.voice_transfer_record.source_seq = INT64_MAX;
    roundtrip(&event);
    event.data.voice_transfer_record.source_seq++;
    assert_event_encode_rejected(&event);
    event.data.voice_transfer_record.source_seq = 4u;
    event.data.voice_transfer_record.source_kind = 264u;
    event.data.voice_transfer_record.data = expected;
    event.data.voice_transfer_record.size = sizeof(expected);
    assert_event_encode_rejected(&event);

    struct snag_binary_event child = {.kind = SNAG_BINARY_VOICE_TRANSFER_SEALED};
    child.data.voice_transfer_sealed = (struct snag_binary_voice_transfer){
        .source_as_of = 1u, .count = 1u};
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&bytes, &child));
    event.data.voice_transfer_record.source_kind = (uint16_t)child.kind;
    event.data.voice_transfer_record.data = bytes.data;
    event.data.voice_transfer_record.size = bytes.len;
    assert_event_encode_rejected(&event);
    child = (struct snag_binary_event){.kind = SNAG_BINARY_VOICE_TRANSFER_ADOPTED};
    child.data.voice_transfer_adopted = (struct snag_binary_voice_adopted){
        .transfer = {.source_as_of = 1u, .count = 1u}, .begin_offset = 1u, .begin_seq = 2u};
    bytes.len = 0u;
    assert(!snag_binary_event_encode(&bytes, &child));
    event.data.voice_transfer_record.source_kind = (uint16_t)child.kind;
    event.data.voice_transfer_record.data = bytes.data;
    event.data.voice_transfer_record.size = bytes.len;
    event.data.voice_transfer_record.source_version = 2u;
    roundtrip(&event);
    event.data.voice_transfer_record.source_version = 1u;
    child = (struct snag_binary_event){.kind = SNAG_BINARY_STEERING_DEFERRED};
    bytes.len = 0u;
    assert(!snag_binary_event_encode(&bytes, &child));
    event.data.voice_transfer_record.source_kind = (uint16_t)child.kind;
    event.data.voice_transfer_record.data = bytes.data;
    event.data.voice_transfer_record.size = bytes.len;
    roundtrip(&event);
    event.data.voice_transfer_record.source_version = 2u;
    roundtrip(&event);
    event.data.voice_transfer_record.source_version = 3u;
    assert_event_encode_rejected(&event);
    event.data.voice_transfer_record.source_version = 1u;
    child = (struct snag_binary_event){.kind = SNAG_BINARY_SESSION_ARCHIVED};
    child.data.archive.origin = SNAG_BINARY_USER;
    bytes.len = 0u;
    assert(!snag_binary_event_encode(&bytes, &child));
    event.data.voice_transfer_record.source_kind = SNAG_BINARY_SESSION_ARCHIVED;
    event.data.voice_transfer_record.data = bytes.data;
    event.data.voice_transfer_record.size = bytes.len;
    roundtrip(&event);
    /* Ordinary archive metadata requires its user-origin byte. */
    event.data.voice_transfer_record.data = NULL;
    event.data.voice_transfer_record.size = 0u;
    assert_event_encode_rejected(&event);

    unsigned char *large = calloc(1u, SNAG_MAX_EVENT_LINE);
    assert(large);
    size_t size = SNAG_MAX_EVENT_LINE - 84u;
    for (size_t i = 0u; i < 4u; ++i) {
        large[80u + i] = (unsigned char)(size >> (i * 8u));
    }
    memset(large + 84u, 'x', size);
    record = (struct snag_binary_record){.kind = 249u, .version = 1u,
        .payload = large, .size = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_decode(&record, &decoded));
    event.data.voice_transfer_record.source_kind = 249u;
    event.data.voice_transfer_record.data = large;
    event.data.voice_transfer_record.size = SNAG_MAX_EVENT_LINE;
    assert_event_encode_rejected(&event);
    event.data.voice_transfer_record.size++;
    assert_event_encode_rejected(&event);
    free(large);
    snag_buf_free(&bytes);
    snag_buf_free(&payload);
}

static void
test_audio_usage(void)
{
    struct snag_binary_event event = {.kind = SNAG_BINARY_AUDIO_USAGE};
    event.data.audio_usage = (struct snag_binary_audio_usage){text("p"), text("m"), text("r")};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    const unsigned char expected[15] = {1u, 0u, 0u, 0u, 'p', 1u, 0u, 0u, 0u, 'm',
        1u, 0u, 0u, 0u, 'r'};
    assert(payload.len == sizeof(expected) && !memcmp(payload.data, expected, sizeof(expected)));
    roundtrip(&event);
    struct snag_binary_record record = {.kind = 256u, .version = 1u,
        .payload = payload.data, .size = payload.len};
    for (size_t i = 0u; i < 3u; ++i) {
        payload.data[i * 5u] = 0u;
        assert_rejected(record);
        memcpy(payload.data, expected, sizeof(expected));
        payload.data[i * 5u + 4u] = 0u;
        assert_rejected(record);
        payload.data[i * 5u + 4u] = 0xffu;
        assert_rejected(record);
        memcpy(payload.data, expected, sizeof(expected));
    }
    unsigned char *large = malloc(256u * 1024u);
    assert(large);
    memset(large, 'x', 256u * 1024u);
    struct snag_binary_text *fields[] = {&event.data.audio_usage.provider,
        &event.data.audio_usage.model, &event.data.audio_usage.report};
    const size_t limits[] = {SNAG_CONFIG_PROVIDER_NAME_MAX, SNAG_MODEL_MAX_BYTES - 1u,
        256u * 1024u - 1u};
    for (size_t i = 0u; i < 3u; ++i) {
        struct snag_binary_text saved = *fields[i];
        *fields[i] = (struct snag_binary_text){large, limits[i]};
        payload.len = 0u;
        assert(!snag_binary_event_encode(&payload, &event));
        record.payload = payload.data;
        record.size = payload.len;
        struct snag_binary_event decoded;
        assert(!snag_binary_event_decode(&record, &decoded));
        struct snag_buf copy = {.max = SNAG_MAX_EVENT_LINE};
        assert(!snag_binary_event_encode(&copy, &decoded));
        assert(copy.len == payload.len && !memcmp(copy.data, payload.data, copy.len));
        snag_buf_free(&copy);
        fields[i]->size++;
        assert_event_encode_rejected(&event);
        fields[i]->size = 0u;
        assert_event_encode_rejected(&event);
        *fields[i] = saved;
    }
    event.data.audio_usage.report = text(" \n");
    roundtrip(&event);
    free(large);
    snag_buf_free(&payload);
}

static void
test_voice_transfer_anchors(void)
{
    struct snag_binary_event event = {.kind = SNAG_BINARY_VOICE_TRANSFER_SEALED};
    struct snag_binary_voice_transfer transfer = {
        .id = {1u}, .target = {2u}, .source = {3u}, .source_as_of = 4u, .count = 5u};
    event.data.voice_transfer_sealed = transfer;
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    unsigned char expected[112] = {[0] = 1u, [16] = 2u, [32] = 3u, [48] = 4u, [56] = 5u,
        [64] = 6u, [72] = 7u, [80] = 8u};
    assert(payload.len == 64u && !memcmp(payload.data, expected, 64u));
    roundtrip(&event);
    event.kind = SNAG_BINARY_VOICE_TRANSFER_ADOPTED;
    event.data.voice_transfer_adopted = (struct snag_binary_voice_adopted){
        .transfer = transfer, .begin_offset = 6u, .begin_seq = 7u, .begin_sha256 = {8u}};
    payload.len = 0u;
    assert(!snag_binary_event_encode(&payload, &event));
    assert(payload.len == sizeof(expected) + 1u && !payload.data[0] &&
        !memcmp(payload.data + 1u, expected, sizeof(expected)));
    roundtrip(&event);
    uint64_t *fields[] = {&event.data.voice_transfer_adopted.transfer.source_as_of,
        &event.data.voice_transfer_adopted.transfer.count,
        &event.data.voice_transfer_adopted.begin_offset,
        &event.data.voice_transfer_adopted.begin_seq};
    const uint64_t numbers[] = {0u, 1u, 2u, INT64_MAX, (uint64_t)INT64_MAX + 1u, UINT64_MAX};
    for (size_t i = 0u; i < 4u; ++i) {
        uint64_t saved = *fields[i];
        for (size_t j = 0u; j < sizeof(numbers) / sizeof(numbers[0]); ++j) {
            *fields[i] = numbers[j];
            bool valid = numbers[j] >= (i == 3u ? 2u : 1u) && numbers[j] <= INT64_MAX;
            if (valid) roundtrip(&event);
            else assert_event_encode_rejected(&event);
            memcpy(payload.data, expected, sizeof(expected));
            for (size_t k = 0u; k < 8u; ++k) {
                payload.data[48u + i * 8u + k] = (unsigned char)(numbers[j] >> (k * 8u));
            }
            struct snag_binary_record record = {.kind = 266u, .version = 1u,
                .payload = payload.data, .size = sizeof(expected)};
            if (valid) {
                struct snag_binary_event decoded;
                assert(!snag_binary_event_decode(&record, &decoded));
            } else {
                assert_rejected(record);
            }
            if (i < 2u) {
                struct snag_binary_event sealed = {.kind = SNAG_BINARY_VOICE_TRANSFER_SEALED};
                sealed.data.voice_transfer_sealed = event.data.voice_transfer_adopted.transfer;
                record.kind = 265u;
                record.size = 64u;
                if (valid) {
                    roundtrip(&sealed);
                    struct snag_binary_event decoded;
                    assert(!snag_binary_event_decode(&record, &decoded));
                } else {
                    assert_event_encode_rejected(&sealed);
                    assert_rejected(record);
                }
            }
        }
        *fields[i] = saved;
    }
    event.data.voice_transfer_adopted = (struct snag_binary_voice_adopted){
        .transfer = transfer, .begin_seq = 7u, .native = true};
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    const unsigned char native[73] = {[0] = 1u, [1] = 1u, [17] = 2u, [33] = 3u,
        [49] = 4u, [57] = 5u, [65] = 7u};
    assert(payload.len == sizeof(native) && !memcmp(payload.data, native, sizeof(native)));
    roundtrip(&event);
    event.data.voice_transfer_adopted.begin_offset = 1u;
    assert_event_encode_rejected(&event);
    event.data.voice_transfer_adopted.begin_offset = 0u;
    event.data.voice_transfer_adopted.begin_sha256[0] = 1u;
    assert_event_encode_rejected(&event);
    struct snag_binary_record record = {.kind = 266u, .version = 2u,
        .payload = payload.data, .size = payload.len};
    payload.data[0] = 2u;
    assert_rejected(record);
    payload.data[0] = 1u;
    payload.data[65] = 1u;
    assert_rejected(record);
    snag_buf_free(&payload);
}

static void
assert_rule_value_rejected(const void *data, size_t size)
{
    struct snag_binary_result_value out;
    memset(&out, 0xa5, sizeof(out));
    unsigned char saved[sizeof(out)];
    memcpy(saved, &out, sizeof(out));
    assert(snag_binary_rule_value_decode(data, size, &out) < 0);
    assert(!memcmp(saved, &out, sizeof(out)));
    struct snag_binary_result_value value = {.data = data, .size = size};
    json_t *json = json_true();
    assert(snag_binary_rule_value_json(&value, &json) < 0 && json == json_true());
}

static void
test_rule_values(void)
{
    json_t *values[] = {json_null(), json_false(), json_true(), json_integer(INT64_MIN),
        json_integer(INT64_MAX), json_string("\xce\xbb\n"), json_array(),
        json_pack("[i,s]", -1, "x"), json_pack("{s:i,s:s}", "z", 7, "a", "value")};
    for (size_t i = 0u; i < sizeof(values) / sizeof(values[0]); ++i) {
        json_t *value = values[i];
        struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
        struct snag_buf result_bytes = {.max = SNAG_MAX_EVENT_LINE};
        struct snag_buf canonical = {.max = SNAG_MAX_EVENT_LINE};
        assert(value && !snag_json_canonical(value, &canonical));
        assert(!snag_binary_rule_value_encode(&bytes, value));
        assert(!snag_binary_result_value_encode(&result_bytes, value));
        assert(bytes.len == result_bytes.len && !memcmp(bytes.data, result_bytes.data, bytes.len));
        struct snag_binary_result_value field;
        assert(!snag_binary_rule_value_decode(bytes.data, bytes.len, &field));
        assert(field.data == bytes.data && field.size == bytes.len &&
            field.canonical_size == canonical.len);
        field.kind = SNAG_BINARY_RESULT_FALSE;
        field.integer = 123;
        field.canonical_size = 1u;
        json_t *json = NULL;
        assert(!snag_binary_rule_value_json(&field, &json) && json_equal(value, json));
        struct snag_binary_event event = {.kind = SNAG_BINARY_RULE_LOG};
        event.data.rule_log = (struct snag_binary_rule_log){field, field, field};
        roundtrip(&event);
        for (size_t n = 0u; n < bytes.len; ++n) {
            assert_rule_value_rejected(bytes.data, n);
        }
        json_decref(json);
        json_decref(value);
        snag_buf_free(&canonical);
        snag_buf_free(&result_bytes);
        snag_buf_free(&bytes);
    }
    /* Noncanonical encoding of a small integer. */
    unsigned char invalid[] = {3u, 0u};
    assert_rule_value_rejected(invalid, sizeof(invalid));
    invalid[0] = 7u;
    assert_rule_value_rejected(invalid, 1u);
    assert_rule_value_rejected(NULL, 0u);
    unsigned char null_value = 0u;
    struct snag_binary_result_value field = {.data = &null_value, .size = 1u};
    assert(snag_binary_rule_value_decode(&null_value, 1u, NULL) < 0);
    assert(snag_binary_rule_value_encode(NULL, json_null()) < 0);
    assert(snag_binary_rule_value_json(&field, NULL) < 0);
    json_t *json = json_true();
    assert(snag_binary_rule_value_json(NULL, &json) < 0 && json == json_true());
    struct snag_buf bytes = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_buf_append(&bytes, "keep", 4u));
    json_t *real = json_real(1.5);
    assert(real && snag_binary_rule_value_encode(&bytes, real) < 0);
    assert(bytes.len == 4u && !memcmp(bytes.data, "keep", 4u));
    json_decref(real);
    bytes.max = 4u;
    assert(snag_binary_rule_value_encode(&bytes, json_null()) < 0 && bytes.len == 4u);
    snag_buf_free(&bytes);

    json_t *deep = json_null();
    for (size_t i = 0u; i < 46u; ++i) {
        json_t *array = json_array();
        assert(array && !json_array_append_new(array, deep));
        deep = array;
    }
    bytes.max = SNAG_MAX_EVENT_LINE;
    assert(!snag_binary_rule_value_encode(&bytes, deep) && bytes.len == 93u);
    assert_result_value_rejected(bytes.data, bytes.len);
    assert(!snag_binary_rule_value_decode(bytes.data, bytes.len, &field));
    assert(!snag_binary_rule_value_json(&field, &json) && json_equal(deep, json));
    json_decref(json);
    struct snag_binary_event event = {.kind = SNAG_BINARY_RULE_LOG};
    event.data.rule_log.chain = field;
    event.data.rule_log.message = event.data.rule_log.rule =
        (struct snag_binary_result_value){.data = &null_value, .size = 1u};
    roundtrip(&event);
    json_t *legacy = json_pack("{s:{s:O,s:n,s:n}}", "data", "chain", deep, "message", "rule");
    struct snag_buf canonical = {.max = SNAG_MAX_EVENT_LINE};
    assert(legacy && !snag_json_canonical(legacy, &canonical));
    json_t *array = json_array();
    assert(array && !json_array_append_new(array, deep));
    size_t saved = bytes.len;
    assert(snag_binary_rule_value_encode(&bytes, array) < 0 && bytes.len == saved);
    assert(!json_object_set(json_object_get(legacy, "data"), "chain", array));
    canonical.len = 0u;
    assert(snag_json_canonical(legacy, &canonical) < 0);
    json_decref(legacy);
    json_decref(array);
    unsigned char too_deep[95];
    memset(too_deep, 5u, 47u);
    too_deep[47] = 0u;
    memset(too_deep + 48u, 7u, 47u);
    assert_rule_value_rejected(too_deep, sizeof(too_deep));
    event.data.rule_log.chain = (struct snag_binary_result_value){
        .data = too_deep, .size = sizeof(too_deep)};
    assert_event_encode_rejected(&event);
    snag_buf_free(&canonical);
    snag_buf_free(&bytes);
}

static void
test_rule_events(void)
{
    const unsigned char values[] = {0u, 1u, 4u, 'r', 0u};
    struct snag_binary_event event = {.kind = SNAG_BINARY_RULE_LOG};
    event.data.rule_log.chain = (struct snag_binary_result_value){.data = values, .size = 1u};
    event.data.rule_log.message = (struct snag_binary_result_value){
        .data = values + 1u, .size = 1u};
    event.data.rule_log.rule = (struct snag_binary_result_value){.data = values + 2u, .size = 3u};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    assert(payload.len == sizeof(values) && !memcmp(payload.data, values, sizeof(values)));
    roundtrip(&event);
    struct snag_binary_record record = {.kind = 248u, .version = 1u,
        .payload = payload.data, .size = payload.len};
    /* Unterminated final field, after two valid fields. */
    payload.data[4] = 7u;
    assert_rejected(record);
    unsigned char *large = malloc(SNAG_MAX_EVENT_LINE);
    assert(large);
    memset(large, 'x', SNAG_MAX_EVENT_LINE);
    large[0] = 4u;
    large[SNAG_MAX_EVENT_LINE - 3u] = 0u;
    event.data.rule_log.rule = (struct snag_binary_result_value){
        .data = large, .size = SNAG_MAX_EVENT_LINE - 2u};
    payload.len = 0u;
    assert(!snag_binary_event_encode(&payload, &event) && payload.len == SNAG_MAX_EVENT_LINE);
    struct snag_binary_event decoded;
    record.payload = payload.data;
    record.size = payload.len;
    assert(!snag_binary_event_decode(&record, &decoded));
    assert(decoded.data.rule_log.rule.size == SNAG_MAX_EVENT_LINE - 2u);
    large[SNAG_MAX_EVENT_LINE - 3u] = 'x';
    large[SNAG_MAX_EVENT_LINE - 2u] = 0u;
    event.data.rule_log.rule.size++;
    assert_event_encode_rejected(&event);
    free(large);
    snag_buf_free(&payload);

    event = (struct snag_binary_event){.kind = SNAG_BINARY_RULE_TRANSFORM};
    event.data.rule_transform.call[0] = 1u;
    event.data.rule_transform.original_sha256[0] = 2u;
    event.data.rule_transform.effective_sha256[0] = 3u;
    event.data.rule_transform.rule = text("r");
    payload.max = SNAG_MAX_EVENT_LINE;
    assert(!snag_binary_event_encode(&payload, &event));
    unsigned char expected[85] = {[0] = 1u, [16] = 2u, [48] = 3u, [80] = 1u, [84] = 'r'};
    assert(payload.len == sizeof(expected) && !memcmp(payload.data, expected, sizeof(expected)));
    roundtrip(&event);
    record = (struct snag_binary_record){.kind = 249u, .version = 1u,
        .payload = payload.data, .size = payload.len};
    payload.data[84] = 0u;
    assert_rejected(record);
    event.data.rule_transform.rule = text("");
    roundtrip(&event);
    unsigned char rule[1025];
    memset(rule, 'r', sizeof(rule));
    event.data.rule_transform.rule = (struct snag_binary_text){rule, sizeof(rule)};
    roundtrip(&event);
    event.data.rule_transform.rule = (struct snag_binary_text){(const unsigned char *)"\xff", 1u};
    assert_event_encode_rejected(&event);
    snag_buf_free(&payload);
}

static void
test_download_events(void)
{
    struct snag_binary_event event = {.kind = SNAG_BINARY_DOWNLOAD_QUEUED};
    struct snag_binary_download *value = &event.data.download_queued;
    value->id[0] = 1u;
    value->sha256[0] = 2u;
    value->bytes = 3u;
    value->mtime = 4u;
    value->queued_ms = 5u;
    value->path = text("/p");
    value->name = text("n");
    unsigned char expected[83] = {[0] = 1u, [16] = 2u, [48] = 3u, [56] = 4u, [64] = 5u,
        [72] = 2u, [76] = '/', [77] = 'p', [78] = 1u, [82] = 'n'};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    assert(payload.len == sizeof(expected) && !memcmp(payload.data, expected, sizeof(expected)));
    roundtrip(&event);
    struct snag_binary_record record = {.kind = 240u, .version = 1u,
        .payload = payload.data, .size = payload.len};
    const size_t positions[] = {55u, 63u, 71u, 72u, 75u, 76u, 78u, 82u};
    const unsigned char corrupt[] = {128u, 128u, 128u, 0u, 1u, 0u, 0u, 255u};
    for (size_t i = 0u; i < sizeof(positions) / sizeof(positions[0]); ++i) {
        unsigned char saved = payload.data[positions[i]];
        payload.data[positions[i]] = corrupt[i];
        assert_rejected(record);
        payload.data[positions[i]] = saved;
    }
    value->bytes = value->mtime = value->queued_ms = 0u;
    roundtrip(&event);
    value->bytes = value->mtime = value->queued_ms = INT64_MAX;
    roundtrip(&event);
    value->bytes++;
    assert_event_encode_rejected(&event);
    value->bytes = 0u;
    value->mtime++;
    assert_event_encode_rejected(&event);
    value->mtime = 0u;
    value->queued_ms++;
    assert_event_encode_rejected(&event);
    value->queued_ms = 0u;
    unsigned char name[1020]; /* Existing Windows UTF-8 admission bound exceeds Unix NAME_MAX. */
    memset(name, 'x', sizeof(name));
    value->name = (struct snag_binary_text){name, sizeof(name)};
    value->path = text("C:\\out\\file");
    roundtrip(&event);
    value->path = text("\\\\server\\share\\file");
    roundtrip(&event);
    unsigned char path[SNAG_PATH_MAX_BYTES + 1u];
    memset(path, 'x', sizeof(path));
    path[0] = '/';
    value->path = (struct snag_binary_text){path, SNAG_PATH_MAX_BYTES};
    value->name = text("n");
    roundtrip(&event);
    value->path.size++;
    assert_event_encode_rejected(&event);
    value->path = text("");
    assert_event_encode_rejected(&event);
    value->path = text("/p");
    value->name = text("");
    assert_event_encode_rejected(&event);
    value->name = (struct snag_binary_text){(const unsigned char *)"a\0b", 3u};
    assert_event_encode_rejected(&event);
    snag_buf_free(&payload);

    unsigned char reason[1025];
    memset(reason, 'r', sizeof(reason));
    for (unsigned int kind = 241u; kind <= 242u; ++kind) {
        event = (struct snag_binary_event){.kind = (enum snag_binary_kind)kind};
        struct snag_binary_text *message = kind == 241u ? &event.data.download_removed.reason :
            &event.data.downloads_cleared;
        if (kind == 241u) event.data.download_removed.id[0] = 1u;
        *message = text("x");
        payload.max = 2048u;
        assert(!snag_binary_event_encode(&payload, &event));
        unsigned char removed[21] = {[0] = 1u, [16] = 1u, [20] = 'x'};
        unsigned char cleared[5] = {1u, 0u, 0u, 0u, 'x'};
        assert(payload.len == (kind == 241u ? 21u : 5u));
        assert(!memcmp(payload.data, kind == 241u ? removed : cleared, payload.len));
        roundtrip(&event);
        *message = text("");
        roundtrip(&event);
        *message = (struct snag_binary_text){reason, 1024u};
        roundtrip(&event);
        message->size++;
        assert_event_encode_rejected(&event);
        *message = (struct snag_binary_text){(const unsigned char *)"\xff", 1u};
        assert_event_encode_rejected(&event);
        snag_buf_free(&payload);
    }
}

static void
test_binary_controls(void)
{
    for (unsigned int kind = 16u; kind <= 18u; ++kind) {
        struct snag_binary_event event = {.kind = (enum snag_binary_kind)kind};
        for (unsigned int control = 0u; control < 256u; ++control) {
            event.data.control = (struct snag_binary_control){.control = control};
            bool valid = control && control <= 32u && !(control & (control - 1u));
            if (valid) roundtrip(&event);
            else assert_event_encode_rejected(&event);
            event.data.control.image_boundary = true;
            event.data.control.source_seq = 7u;
            if (kind == 16u && control == 4u) roundtrip(&event);
            else assert_event_encode_rejected(&event);
        }
    }
    struct snag_binary_event event = {.kind = SNAG_BINARY_CONTROL_REQUESTED};
    event.data.control = (struct snag_binary_control){.control = SNAG_CONTROL_COMPACT,
        .image_boundary = true, .source_seq = (UINT64_C(1) << 32u) + 7u};
    const unsigned char expected[] = {4u, 1u, 7u, 0u, 0u, 0u, 1u, 0u, 0u, 0u};
    struct snag_buf payload = {.max = 32u};
    assert(!snag_binary_event_encode(&payload, &event));
    assert(payload.len == sizeof(expected) && !memcmp(payload.data, expected, sizeof(expected)));
    roundtrip(&event);
    event.data.control.source_seq = INT64_MAX;
    roundtrip(&event);
    event.data.control.source_seq++;
    assert_event_encode_rejected(&event);
    event.data.control.source_seq = 0u;
    assert_event_encode_rejected(&event);
    event.data.control.source_seq = 1u;
    event.data.control.image_boundary = false;
    assert_event_encode_rejected(&event);
    event.data.control.source_seq = 0u;
    event.data.control.control = UINT32_MAX;
    assert_event_encode_rejected(&event);
    struct snag_binary_record record = {.kind = 16u, .version = 1u,
        .payload = payload.data, .size = payload.len};
    payload.data[1] = 2u;
    assert_rejected(record);
    payload.data[1] = 1u;
    payload.data[9] = 128u;
    assert_rejected(record);
    payload.data[9] = 0u;
    record.kind = 17u;
    assert_rejected(record);
    record.kind = 18u;
    assert_rejected(record);
    snag_buf_free(&payload);
}

static void
test_context_rebase(void)
{
    struct snag_binary_event event = {.kind = SNAG_BINARY_CONTEXT_REBASED};
    event.data.context_rebased.turn[0] = 1u;
    event.data.context_rebased.turn[15] = 2u;
    event.data.context_rebased.reason = SNAG_BINARY_REBASE_GOAL_RECOVERY;
    unsigned char expected[17] = {[0] = 1u, [15] = 2u, [16] = 1u};
    struct snag_buf payload = {.max = 64u};
    assert(!snag_binary_event_encode(&payload, &event));
    assert(payload.len == sizeof(expected) && !memcmp(payload.data, expected, sizeof(expected)));
    for (unsigned int reason = 0u; reason < 4u; ++reason) {
        event.data.context_rebased.reason = (enum snag_binary_rebase_reason)reason;
        if (reason == 1u || reason == 2u) roundtrip(&event);
        else assert_event_encode_rejected(&event);
    }
    struct snag_binary_record record = {.kind = 227u, .version = 1u,
        .payload = payload.data, .size = payload.len};
    payload.data[16] = 3u;
    assert_rejected(record);
    snag_buf_free(&payload);
}

static void
test_capacity_rejection(void)
{
    struct snag_binary_event event = {.kind = SNAG_BINARY_RESPONSE_CAPACITY_REJECTED};
    struct snag_binary_capacity_rejection *value = &event.data.capacity_rejected;
    value->turn[0] = 1u;
    value->response[0] = 2u;
    value->cycle = 3u;
    value->provider_source_sha256[0] = 4u;
    value->request_sha256[0] = 5u;
    value->context_limit = 1024u;
    value->requested_input = 100u;
    value->observed_input = 64u;
    value->message = text("no");
    unsigned char expected[130] = {[0] = 1u, [16] = 2u, [32] = 3u, [36] = 4u, [68] = 5u,
        [101] = 4u, [108] = 100u, [116] = 64u, [124] = 2u, [128] = 'n', [129] = 'o'};
    struct snag_buf payload = {.max = 512u};
    assert(!snag_binary_event_encode(&payload, &event));
    assert(payload.len == sizeof(expected) && !memcmp(payload.data, expected, sizeof(expected)));
    roundtrip(&event);
    struct snag_binary_record record = {.kind = 166u, .version = 1u,
        .payload = payload.data, .size = payload.len};
    const size_t positions[] = {32u, 107u, 115u, 123u, 127u, 128u};
    const unsigned char corrupt[] = {0u, 1u, 1u, 1u, 1u, 0u};
    for (size_t i = 0u; i < sizeof(positions) / sizeof(positions[0]); ++i) {
        unsigned char saved = payload.data[positions[i]];
        payload.data[positions[i]] = corrupt[i];
        assert_rejected(record);
        payload.data[positions[i]] = saved;
    }
    for (unsigned int present = 0u; present < 8u; ++present) {
        value->context_limit = present & 1u ? SNAG_CONFIG_TOKEN_LIMIT_MAX : 0u;
        value->requested_input = present & 2u ? SNAG_CONFIG_TOKEN_LIMIT_MAX : 0u;
        value->observed_input = present & 4u ? SNAG_CONFIG_TOKEN_LIMIT_MAX : 0u;
        roundtrip(&event);
    }
    struct snag_binary_capacity_rejection valid = *value;
    value->context_limit++;
    assert_event_encode_rejected(&event);
    *value = valid;
    value->requested_input++;
    assert_event_encode_rejected(&event);
    *value = valid;
    value->observed_input++;
    assert_event_encode_rejected(&event);
    *value = valid;
    value->cycle = 0u;
    assert_event_encode_rejected(&event);
    value->cycle = UINT32_MAX;
    unsigned char message[256];
    memset(message, 'x', sizeof(message));
    value->message = (struct snag_binary_text){message, 255u};
    roundtrip(&event);
    value->message.size = 256u;
    assert_event_encode_rejected(&event);
    value->message = text("");
    roundtrip(&event);
    value->message = (struct snag_binary_text){(const unsigned char *)"\xff", 1u};
    assert_event_encode_rejected(&event);
    value->message = (struct snag_binary_text){(const unsigned char *)"a\0b", 3u};
    assert_event_encode_rejected(&event);
    json_t *legacy = json_pack("{s:n}", "limit");
    uint64_t limit = 99u;
    assert(legacy &&
        snag_json_nullable_limit(legacy, "limit", SNAG_CONFIG_TOKEN_LIMIT_MAX, &limit));
    assert(limit == 0u);
    assert(!json_object_set_new(legacy, "limit", json_integer(0)));
    assert(!snag_json_nullable_limit(legacy, "limit", SNAG_CONFIG_TOKEN_LIMIT_MAX, &limit));
    json_decref(legacy);
    snag_buf_free(&payload);
}

static void
test_response_complete(void)
{
    static const unsigned char empty[] = {0u, 0u, 0u, 0u};
    struct snag_binary_event event = {.kind = SNAG_BINARY_RESPONSE_COMPLETED};
    struct snag_binary_response_complete *complete = &event.data.response_completed;
    complete->turn[0] = 1u;
    complete->response[0] = 2u;
    complete->cycle = 1u;
    complete->provider_id = text("r");
    complete->items = (struct snag_binary_graph_items){empty, sizeof(empty), 0u};
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&payload, &event));
    /* IDs, cycle, provider ID, empty graph, unknown legacy usage, absent continuation. */
    static const unsigned char expected[] =
        "\x01\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\x02\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\x01\0\0\0\x01\0\0\0r\0\0\0\0\0\0";
    assert(payload.len == sizeof(expected) - 1u &&
        !memcmp(payload.data, expected, payload.len));
    assert(SNAG_BINARY_RESPONSE_COMPLETED == 165 && snag_binary_event_version(event.kind) == 1u);
    roundtrip(&event);
    complete->cached_present = true;
    roundtrip(&event);
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event) && payload.len == 47u);
    assert(payload.data[45u] == 32u);
    struct snag_binary_record record = {.kind = (uint16_t)event.kind, .version = 1u,
        .payload = payload.data, .size = payload.len};
    struct snag_binary_event decoded;
    assert(!snag_binary_event_decode(&record, &decoded));
    assert(decoded.data.response_completed.cached_present &&
        !decoded.data.response_completed.usage.cached_known);
    complete->usage = (struct snag_response_usage){.input_known = true, .input_tokens = 10u,
        .output_known = true, .output_tokens = 5u, .reasoning_known = true, .reasoning_tokens = 3u,
        .total_known = true, .total_tokens = 15u, .cached_known = true, .cached_input_tokens = 7u};
    roundtrip(&event);
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    static const unsigned char usage[] =
        "\x3f\x0a\0\0\0\0\0\0\0\x05\0\0\0\0\0\0\0"
        "\x03\0\0\0\0\0\0\0\x0f\0\0\0\0\0\0\0\x07\0\0\0\0\0\0\0\0";
    assert(payload.len == 45u + sizeof(usage) - 1u &&
        !memcmp(payload.data + 45u, usage, sizeof(usage) - 1u));
    record.payload = payload.data;
    record.size = payload.len;
    payload.data[62u] = 6u;
    assert_rejected(record);
    payload.data[62u] = 3u;
    payload.data[70u] = 16u;
    assert_rejected(record);
    payload.data[70u] = 15u;
    payload.data[45u] = 31u;
    assert_rejected(record);
    payload.data[45u] = 64u;
    assert_rejected(record);
    payload.data[45u] = 63u;
    size_t saved = payload.len;
    complete->cached_present = false;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    complete->cached_present = true;
    complete->usage.total_tokens = 16u;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    complete->usage.total_tokens = 15u;
    complete->usage.reasoning_tokens = 6u;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    complete->usage.reasoning_tokens = 3u;
    /* Preserve inconsistent reported cache counts; totals decide whether to use them. */
    complete->usage.cached_input_tokens = 20u;
    roundtrip(&event);
    for (unsigned i = 0u; i < 5u; ++i) {
        complete->usage = (struct snag_response_usage){0};
        uint64_t *tokens[] = {&complete->usage.input_tokens, &complete->usage.output_tokens,
            &complete->usage.reasoning_tokens, &complete->usage.total_tokens,
            &complete->usage.cached_input_tokens};
        bool *known[] = {&complete->usage.input_known, &complete->usage.output_known,
            &complete->usage.reasoning_known, &complete->usage.total_known,
            &complete->usage.cached_known};
        *known[i] = true;
        *tokens[i] = (uint64_t)INT64_MAX;
        roundtrip(&event);
        *tokens[i] += 1u;
        assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    }
    complete->usage = (struct snag_response_usage){0};
    complete->cached_present = false;
    for (size_t i = 0u; i < 32u; ++i) {
        complete->continuation_scope[i] = (unsigned char)i;
    }
    complete->continuation_form = SNAG_BINARY_CONTINUATION_NULL;
    roundtrip(&event);
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event) && payload.len == 79u);
    assert(payload.data[46u] == 1u &&
        !memcmp(payload.data + 47u, complete->continuation_scope, 32u));
    record.payload = payload.data;
    record.size = payload.len;
    assert(!snag_binary_event_decode(&record, &decoded));
    assert(decoded.data.response_completed.continuation_form == SNAG_BINARY_CONTINUATION_NULL &&
        !decoded.data.response_completed.continuation.data);
    payload.data[46u] = 3u;
    assert_rejected(record);
    payload.data[46u] = 1u;
    saved = payload.len;
    complete->continuation_form = (enum snag_binary_continuation_form)3;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    complete->continuation_form = SNAG_BINARY_CONTINUATION_ITEMS;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    complete->continuation = (struct snag_binary_continuation){empty, sizeof(empty)};
    roundtrip(&event);
    struct snag_binary_continuation_item reasoning = {0u, text("{\"type\":\"reasoning\"}")};
    struct snag_buf continuation = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_continuation_encode(&continuation, &reasoning, 1u, 0u));
    complete->continuation = (struct snag_binary_continuation){continuation.data, continuation.len};
    roundtrip(&event);
    continuation.data[4u] = 1u;
    complete->items.count = 100u;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    continuation.data[4u] = 0u;
    complete->items.count = 0u;
    complete->cycle = 0u;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    complete->cycle = 1u;
    complete->provider_id = text("\n");
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    complete->provider_id = text("r");
    complete->items.size = 3u;
    assert(snag_binary_event_encode(&payload, &event) < 0 && payload.len == saved);
    snag_buf_free(&continuation);
    snag_buf_free(&payload);
}

void
test_store_binary_event(void)
{
    test_input_lists();
    test_instruction_legacy_budget();
    test_input_events();
    test_queued_input();
    test_metadata();
    test_control_text_references();
    test_input_references();
    test_input_reference_variants();
    test_turn_starts();
    test_turn_config_values();
    test_turn_input_references();
    test_turn_outcomes();
    test_response_starts();
    test_response_output();
    test_output_span_codec();
    test_output_span_file();
    test_public_items();
    test_response_interruption();
    test_response_failure();
    test_response_correction();
    test_graph_items();
    test_binary_continuation();
    test_response_complete();
    test_completed_references();
    test_tool_start();
    test_process_output();
    test_process_references();
    test_result_references();
    test_binary_controls();
    test_compact_start_and_interrupt();
    test_compact_complete();
    test_compact_references();
    test_download_events();
    test_rule_values();
    test_rule_events();
    test_audio_usage();
    test_voice_transfer_anchors();
    test_voice_fields();
    test_voice_event_boundaries();
    test_voice_archives();
    test_public_archive_fields();
    test_event_names();
    test_context_rebase();
    test_capacity_rejection();
    test_irc_event();
    test_irc_snapshot();
    test_sequences();
    test_irc_admission();
    test_result_values();
    test_tool_result_records();
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
