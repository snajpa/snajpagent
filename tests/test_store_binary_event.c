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
    if (event->kind <= SNAG_BINARY_TIMER_CANCELLED) {
        assert(!memcmp(decoded.data.timer.id, event->data.timer.id, 16u));
        if (event->kind == SNAG_BINARY_TIMER_SCHEDULED) {
            assert(decoded.data.timer.due_ms == event->data.timer.due_ms);
            assert(decoded.data.timer.text.size == event->data.timer.text.size);
            assert(!memcmp(decoded.data.timer.text.data, event->data.timer.text.data,
                event->data.timer.text.size));
        }
    } else {
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

void
test_store_binary_event(void)
{
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
    struct snag_binary_record record = {.kind = 1u, .version = 1u};
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
    event.kind = (enum snag_binary_kind)1;
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
