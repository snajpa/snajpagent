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
    assert(!memcmp(encoded_again.data, payload.data, payload.len));
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

void
test_store_binary_event(void)
{
    test_metadata();
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
