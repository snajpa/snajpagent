/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_checkpoint.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char golden[] = {
    0x01, 0x00, 0x01, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x07, 0x06, 0x05, 0x04, 0x03,
    0x02, 0x01, 0x04, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x03, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x03, 0x07, 0x06, 0x05, 0x04, 0x03,
    0x02, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x05, 0x07, 0x06, 0x05, 0x04, 0x03,
    0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x06, 0x07, 0x06, 0x05, 0x04, 0x03,
    0x02, 0x01, 0x2a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static void
bad_decode(const void *data, size_t size)
{
    struct snag_binary_checkpoint_inputs out;
    memset(&out, 0xa5, sizeof(out));
    struct snag_binary_checkpoint_inputs saved = out;
    assert(snag_binary_checkpoint_inputs_decode(data, size, &out) < 0);
    assert(!memcmp(&out, &saved, sizeof(out)));
}

static void
bad_read(int fd, const struct snag_binary_anchor *anchor,
    const struct snag_binary_checkpoint_inputs *view, const struct snag_session *state)
{
    struct snag_binary_checkpoint_inputs_state out;
    memset(&out, 0xa5, sizeof(out));
    struct snag_binary_checkpoint_inputs_state saved = out;
    assert(snag_binary_checkpoint_inputs_read(fd, anchor, view, state, &out) < 0);
    assert(!memcmp(&out, &saved, sizeof(out)));
}

static bool
same_json(const json_t *a, const json_t *b)
{
    return (!a && !b) || (a && b && json_equal(a, b));
}

void
test_store_binary_inputs_state(int fd, const struct snag_binary_anchor *anchor,
    const struct snag_binary_checkpoint_sources *sources, const struct snag_session *state)
{
    struct snag_buf wire = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_inputs_encode(&wire, sources, state));
    struct snag_binary_checkpoint_inputs view;
    assert(!snag_binary_checkpoint_inputs_decode(wire.data, wire.len, &view));
    struct snag_binary_checkpoint_inputs_state restored = {0};
    assert(!snag_binary_checkpoint_inputs_read(fd, anchor, &view, state, &restored));
    assert(same_json(restored.input, state->pending_input));
    assert(restored.queue_count == state->pending_queue_count);
    assert(restored.steering_count == state->pending_steering_count);
    assert(restored.queue_bytes == state->pending_queue_bytes);
    assert(restored.steering_bytes == state->pending_steering_bytes);
    for (size_t i = 0u; i < restored.queue_count; ++i) {
        const struct snag_queued_turn *a = &restored.queue[i], *b = &state->pending_queue[i];
        assert(!strcmp(a->queue_id, b->queue_id) && !strcmp(a->text, b->text));
        assert(a->seq == b->seq && a->received_ms == b->received_ms);
        assert(a->first_context_ms == b->first_context_ms && a->read_only == b->read_only);
        assert(same_json(a->content, b->content));
    }
    for (size_t i = 0u; i < restored.steering_count; ++i) {
        const struct snag_pending_steering *a = &restored.steering[i];
        const struct snag_pending_steering *b = &state->pending_steering[i];
        assert(!strcmp(a->steering_id, b->steering_id) && !strcmp(a->text, b->text));
        assert(a->seq == b->seq && a->received_ms == b->received_ms);
        assert(a->first_context_ms == b->first_context_ms && same_json(a->content, b->content));
    }
    snag_binary_checkpoint_inputs_free(&restored);
    assert(!restored.input && !restored.strings && !restored.queue && !restored.steering);
    snag_binary_checkpoint_inputs_free(&restored);
    snag_binary_checkpoint_inputs_free(NULL);
    bad_read(-1, anchor, &view, state);
    bad_read(fd, NULL, &view, state);
    bad_read(fd, anchor, NULL, state);
    bad_read(fd, anchor, &view, NULL);
    struct snag_binary_checkpoint_inputs bad = view;
    bad.queue_count = SIZE_MAX;
    bad_read(fd, anchor, &bad, state);
    bad = view;
    bad.steering_count = SIZE_MAX;
    bad_read(fd, anchor, &bad, state);
    bad = view;
    bad.input = 1u; /* Session creation cannot be used as an input receipt. */
    bad_read(fd, anchor, &bad, state);
    bad.input = anchor->next_seq;
    bad_read(fd, anchor, &bad, state);
    if (view.queue_count || view.steering_count || view.input) {
        struct snag_binary_anchor early = *anchor;
        early.next_seq = 1u;
        bad_read(fd, &early, &view, state);
    }
    if (view.input) {
        struct snag_session active = *state;
        active.active_turn = true;
        bad_read(fd, anchor, &view, &active);
    }
    if (view.steering_count) {
        struct snag_session wrong = *state;
        wrong.active_turn = false;
        bad_read(fd, anchor, &view, &wrong);
        wrong = *state;
        memset(wrong.active_turn_id, 'f', SNAG_ID_HEX_LEN);
        bad_read(fd, anchor, &view, &wrong);
    }
    json_t *receipt = (json_t *)&restored;
    uint64_t timestamp = UINT64_MAX;
    assert(snag_binary_checkpoint_receipt_read(fd, anchor, 1u,
        SNAG_BINARY_RESPONSE_COMPLETED, &receipt, &timestamp) < 0);
    assert(receipt == (json_t *)&restored && timestamp == UINT64_MAX);
    snag_buf_free(&wire);
}

void
test_store_binary_inputs(void)
{
    const uint64_t base = UINT64_C(0x0102030405060700);
    struct snag_binary_checkpoint_queue_source queue[2] = {
        {base + 2u, base + 4u}, {base + 3u, base + 3u}};
    struct snag_queued_turn queued[2] = {{.seq = base + 2u},
        {.seq = base + 3u, .first_context_ms = UINT64_MAX}};
    struct snag_pending_steering steering[2] = {
        {.seq = base + 5u, .first_context_ms = UINT64_C(1) << 63u},
        {.seq = base + 6u, .first_context_ms = 42u}};
    struct snag_binary_checkpoint_sources sources = {
        .input = base + 1u, .queue = queue, .queue_count = 2u};
    struct snag_session state = {.pending_input = json_object(),
        .pending_queue = queued, .pending_queue_count = 2u,
        .pending_steering = steering, .pending_steering_count = 2u};
    assert(state.pending_input);
    struct snag_buf wire = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_inputs_encode(&wire, &sources, &state));
    assert(wire.len == sizeof(golden) && !memcmp(wire.data, golden, wire.len));
    struct snag_binary_checkpoint_inputs view;
    assert(!snag_binary_checkpoint_inputs_decode(golden, sizeof(golden), &view));
    assert(view.input == sources.input && view.queue_count == 2u && view.steering_count == 2u);
    for (size_t i = 0u; i < sizeof(golden); ++i) bad_decode(golden, i);
    bad_decode(NULL, sizeof(golden));
    assert(snag_binary_checkpoint_inputs_decode(golden, sizeof(golden), NULL) < 0);
    unsigned char changed[sizeof(golden) + 1u];
    memcpy(changed, golden, sizeof(golden));
    changed[sizeof(golden)] = 0u;
    bad_decode(changed, sizeof(changed));
    const size_t offsets[] = {0u, 1u, 2u, 10u, 18u, 26u, 34u, 50u, 58u, 74u, 90u};
    for (size_t i = 0u; i < sizeof(offsets) / sizeof(*offsets); ++i) {
        memcpy(changed, golden, sizeof(golden));
        size_t size = offsets[i] < 2u ? 1u : 8u;
        memset(changed + offsets[i], 255, size);
        bad_decode(changed, sizeof(golden));
    }
    const size_t zeroes[] = {26u, 34u, 50u, 58u, 74u, 90u};
    for (size_t i = 0u; i < sizeof(zeroes) / sizeof(*zeroes); ++i) {
        memcpy(changed, golden, sizeof(golden));
        memset(changed + zeroes[i], 0, 8u);
        bad_decode(changed, sizeof(golden));
    }
    memcpy(changed, golden, sizeof(golden));
    memcpy(changed + 50u, changed + 26u, 8u);
    bad_decode(changed, sizeof(golden));
    memcpy(changed, golden, sizeof(golden));
    memcpy(changed + 90u, changed + 74u, 8u);
    bad_decode(changed, sizeof(golden));
    struct snag_buf limited = {.max = sizeof(golden) + 3u};
    assert(!snag_buf_append(&limited, "keep", 4u));
    assert(snag_binary_checkpoint_inputs_encode(&limited, &sources, &state) < 0);
    assert(limited.len == 4u && !memcmp(limited.data, "keep", 4u));
    ++limited.max;
    assert(!snag_binary_checkpoint_inputs_encode(&limited, &sources, &state));
    assert(!memcmp(limited.data + 4u, golden, sizeof(golden)));
    snag_buf_free(&limited);
    struct snag_buf alias = {.max = SIZE_MAX};
    assert(!snag_buf_append(&alias, queue, sizeof(queue)));
    sources.queue = (void *)alias.data;
    assert(!snag_binary_checkpoint_inputs_encode(&alias, &sources, &state));
    assert(!memcmp(alias.data + sizeof(queue), golden, sizeof(golden)));
    snag_buf_free(&alias);
    sources.queue = queue;
    ++sources.queue[0].creation;
    assert(snag_binary_checkpoint_inputs_encode(&wire, &sources, &state) < 0);
    --sources.queue[0].creation;
    --sources.queue_count;
    assert(snag_binary_checkpoint_inputs_encode(&wire, &sources, &state) < 0);
    ++sources.queue_count;
    sources.input = 0u;
    assert(snag_binary_checkpoint_inputs_encode(&wire, &sources, &state) < 0);
    assert(wire.len == sizeof(golden));
    json_decref(state.pending_input);
    state = (struct snag_session){0};
    sources = (struct snag_binary_checkpoint_sources){0};
    snag_buf_reset(&wire);
    assert(!snag_binary_checkpoint_inputs_encode(&wire, &sources, &state));
    assert(wire.len == 26u && wire.data[0] == 1u);
    for (size_t i = 1u; i < wire.len; ++i) assert(!wire.data[i]);
    assert(!snag_binary_checkpoint_inputs_decode(wire.data, wire.len, &view));
    assert(!view.input && !view.queue_count && !view.steering_count);
    snag_buf_free(&wire);
}
