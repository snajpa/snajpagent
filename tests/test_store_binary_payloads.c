/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_checkpoint.h"
#include "fs.h"
#include "store_binary_legacy.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char golden[] = {
    0x01, 0x00, 0x01, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x02, 0x07,
    0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x03, 0x07, 0x06, 0x05, 0x04, 0x03,
    0x02, 0x01, 0x04, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x05, 0x07,
    0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x06, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x07,
    0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
};

static bool
equal_json(const json_t *a, const json_t *b)
{
    return a == b || (a && b && json_equal(a, b));
}

static void
bad_decode(const void *data, size_t size)
{
    struct snag_binary_checkpoint_payloads before, after;
    memset(&before, 0x6d, sizeof(before));
    after = before;
    assert(snag_binary_checkpoint_payloads_decode(data, size, &after) < 0);
    assert(!memcmp(&before, &after, sizeof(before)));
}

static void
bad_read(int fd, const struct snag_binary_anchor *anchor,
    const struct snag_binary_checkpoint_payloads *view, const struct snag_session *state)
{
    struct snag_binary_checkpoint_payloads_state before, after;
    memset(&before, 0x6d, sizeof(before));
    after = before;
    assert(snag_binary_checkpoint_payloads_read(fd, anchor, view, state, &after) < 0);
    assert(!memcmp(&before, &after, sizeof(before)));
}

void
test_store_binary_payloads_state(int fd, const struct snag_binary_anchor *anchor,
    const struct snag_binary_checkpoint_sources *sources, const struct snag_session *state)
{
    struct snag_buf bytes = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_payloads_encode(&bytes, sources, state));
    struct snag_binary_checkpoint_payloads view;
    assert(!snag_binary_checkpoint_payloads_decode(bytes.data, bytes.len, &view));
    struct snag_binary_checkpoint_payloads_state restored = {0};
    int64_t position = snag_seek(fd, 0, SEEK_CUR);
    assert(!snag_binary_checkpoint_payloads_read(fd, anchor, &view, state, &restored));
    assert(snag_seek(fd, 0, SEEK_CUR) == position);
    assert(equal_json(restored.instructions, state->active_instructions));
    assert(equal_json(restored.compact_output, state->compact_output));
    assert(equal_json(restored.response_public, state->response_public));
    assert(equal_json(restored.downloads, state->download_queue));
    assert(restored.response_public_bytes == state->response_public_bytes);
    snag_binary_checkpoint_payloads_free(&restored);
    assert(!restored.instructions && !restored.compact_output && !restored.response_public &&
        !restored.downloads && !restored.response_public_bytes);
    snag_binary_checkpoint_payloads_free(&restored);
    snag_binary_checkpoint_payloads_free(NULL);
    bad_read(-1, anchor, &view, state);
    bad_read(fd, NULL, &view, state);
    bad_read(fd, anchor, NULL, state);
    bad_read(fd, anchor, &view, NULL);
    struct snag_binary_anchor early = *anchor;
    early.next_seq = 1u;
    bad_read(fd, &early, &view, state);
    struct snag_binary_checkpoint_payloads bad = view;
    bad.download_count = SIZE_MAX;
    bad_read(fd, anchor, &bad, state);
    struct snag_session changed = *state;
    changed.active_turn = !state->active_turn;
    bad_read(fd, anchor, &view, &changed);
    if (view.turn) {
        bad = view;
        bad.turn = 1u;
        bad_read(fd, anchor, &bad, state);
        changed = *state;
        changed.active_turn_id[0] = state->active_turn_id[0] == 'a' ? 'b' : 'a';
        bad_read(fd, anchor, &view, &changed);
        changed = *state;
        changed.turn_count ^= 1u;
        bad_read(fd, anchor, &view, &changed);
    }
    if (view.compact_end) {
        bad = view;
        bad.compact_start = 1u;
        bad_read(fd, anchor, &bad, state);
        if (view.compact_start > 2u) {
            bad.compact_start = 2u; /* Earlier valid start, even with a reused ID. */
            bad_read(fd, anchor, &bad, state);
        }
        bad = view;
        bad.compact_end = anchor->next_seq;
        bad_read(fd, anchor, &bad, state);
        changed = *state;
        changed.compact_seq ^= 1u;
        bad_read(fd, anchor, &view, &changed);
        changed = *state;
        changed.compact_id[0] = state->compact_id[0] == 'a' ? 'b' : 'a';
        bad_read(fd, anchor, &view, &changed);
        changed = *state;
        memcpy(changed.compact_scope,
            "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd", 65u);
        if (strcmp(changed.compact_scope, state->compact_scope)) {
            bad_read(fd, anchor, &view, &changed);
        }
    }
    if (view.response_end) {
        bad = view;
        bad.response_start = view.turn;
        bad_read(fd, anchor, &bad, state);
        bad = view;
        bad.response_end = anchor->next_seq;
        bad_read(fd, anchor, &bad, state);
        changed = *state;
        changed.response_public_bytes ^= 1u;
        bad_read(fd, anchor, &view, &changed);
        changed = *state;
        changed.active_response_id[0] = '\0';
        bad_read(fd, anchor, &view, &changed);
        changed = *state;
        changed.active_cycle ^= 1u;
        bad_read(fd, anchor, &view, &changed);
        changed = *state;
        changed.active_response_id[0] = state->active_response_id[0] == 'a' ? 'b' : 'a';
        bad_read(fd, anchor, &view, &changed);
    }
    if (view.download_count) {
        const unsigned char wrong[8] = {1u};
        bad = view;
        bad.downloads = wrong;
        bad.download_count = 1u;
        bad_read(fd, anchor, &bad, state);
    }
    assert(snag_seek(fd, 0, SEEK_CUR) == position);
    snag_buf_free(&bytes);
}

void
test_store_binary_payloads(void)
{
    uint64_t base = UINT64_C(0x0102030405060700);
    struct snag_binary_checkpoint_download_source downloads[2] = {
        {.receipt = base + 6u}, {.receipt = base + 7u}};
    struct snag_binary_checkpoint_sources sources = {
        .compact_start = base + 2u, .compact_end = base + 3u,
        .response_start = base + 4u, .response_end = base + 5u,
        .downloads = downloads, .download_count = 2u};
    sources.texts.slots[SNAG_BINARY_TEXT_ACTIVE_PROMPT].declaration = base + 1u;
    struct snag_session state;
    snag_session_init(&state);
    state.active_turn = true;
    state.active_instructions = json_array();
    state.response_public = json_array();
    state.compact_output = json_array();
    state.download_queue = json_pack("[{},{}]");
    assert(state.active_instructions && state.response_public && state.compact_output &&
        state.download_queue);
    struct snag_buf encoded = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_payloads_encode(&encoded, &sources, &state));
    assert(encoded.len == sizeof(golden) && !memcmp(encoded.data, golden, sizeof(golden)));
    struct snag_binary_checkpoint_payloads view;
    assert(!snag_binary_checkpoint_payloads_decode(golden, sizeof(golden), &view));
    assert(view.turn == base + 1u && view.compact_start == base + 2u &&
        view.compact_end == base + 3u && view.response_start == base + 4u &&
        view.response_end == base + 5u && view.download_count == 2u && view.downloads_present);
    for (size_t i = 0u; i < sizeof(golden); ++i) bad_decode(golden, i);
    bad_decode(NULL, sizeof(golden));
    assert(snag_binary_checkpoint_payloads_decode(golden, sizeof(golden), NULL) < 0);
    unsigned char changed[sizeof(golden) + 1u];
    memcpy(changed, golden, sizeof(golden));
    changed[sizeof(golden)] = 0u;
    bad_decode(changed, sizeof(changed));
    const size_t offsets[] = {0u, 1u, 2u, 10u, 18u, 26u, 34u, 42u, 50u, 51u, 59u};
    for (size_t i = 0u; i < sizeof(offsets) / sizeof(*offsets); ++i) {
        memcpy(changed, golden, sizeof(golden));
        size_t length = offsets[i] < 2u || offsets[i] == 50u ? 1u : 8u;
        memset(changed + offsets[i], 255, length);
        bad_decode(changed, sizeof(golden));
    }
    const size_t zeroes[] = {2u, 10u, 18u, 26u, 34u, 42u, 50u, 51u, 59u};
    for (size_t i = 0u; i < sizeof(zeroes) / sizeof(*zeroes); ++i) {
        memcpy(changed, golden, sizeof(golden));
        memset(changed + zeroes[i], 0, zeroes[i] == 50u ? 1u : 8u);
        bad_decode(changed, sizeof(golden));
    }
    memcpy(changed, golden, sizeof(golden));
    memcpy(changed + 59u, changed + 51u, 8u);
    bad_decode(changed, sizeof(golden));
    struct snag_buf limited = {.max = sizeof(golden) + 3u};
    assert(!snag_buf_append(&limited, "keep", 4u));
    assert(snag_binary_checkpoint_payloads_encode(&limited, &sources, &state) < 0);
    assert(limited.len == 4u && !memcmp(limited.data, "keep", 4u));
    limited.max++;
    assert(!snag_binary_checkpoint_payloads_encode(&limited, &sources, &state));
    assert(!memcmp(limited.data + 4u, golden, sizeof(golden)));
    struct snag_buf alias = {.max = SIZE_MAX};
    assert(!snag_buf_append(&alias, downloads, sizeof(downloads)));
    sources.downloads = (struct snag_binary_checkpoint_download_source *)alias.data;
    assert(!snag_binary_checkpoint_payloads_encode(&alias, &sources, &state));
    assert(alias.len == sizeof(downloads) + sizeof(golden));
    assert(!memcmp(alias.data + sizeof(downloads), golden, sizeof(golden)));
    assert(snag_binary_checkpoint_payloads_encode(NULL, &sources, &state) < 0);
    assert(snag_binary_checkpoint_payloads_encode(&encoded, NULL, &state) < 0);
    assert(snag_binary_checkpoint_payloads_encode(&encoded, &sources, NULL) < 0);
    snag_session_close(&state);
    snag_session_init(&state);
    sources = (struct snag_binary_checkpoint_sources){0};
    encoded.len = 0u;
    assert(!snag_binary_checkpoint_payloads_encode(&encoded, &sources, &state));
    assert(encoded.len == 51u);
    unsigned char empty[51] = {1u};
    assert(!memcmp(encoded.data, empty, sizeof(empty)));
    state.download_queue = json_array();
    assert(state.download_queue);
    encoded.len = 0u;
    assert(!snag_binary_checkpoint_payloads_encode(&encoded, &sources, &state));
    empty[50] = 1u;
    assert(!memcmp(encoded.data, empty, sizeof(empty)));
    json_t *canary = (json_t *)&view;
    assert(snag_binary_instructions_legacy(NULL, true, &canary) < 0);
    assert(canary == (json_t *)&view);
    const unsigned char list[] = {0u, 0u, 0u, 0u};
    struct snag_binary_instructions instructions = {list, sizeof(list)};
    assert(snag_binary_instructions_legacy(&instructions, true, NULL) < 0);
    snag_session_close(&state);
    snag_buf_free(&encoded);
    snag_buf_free(&limited);
    snag_buf_free(&alias);
}
