/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_checkpoint.h"
#include "fs.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
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

struct access_fixture {
    struct snag_buf needed, entries, bytes;
    struct snag_binary_checkpoint_index index;
};

static uint64_t
number(const unsigned char *bytes)
{
    uint64_t result = 0u;
    for (size_t i = 0u; i < 8u; ++i) result |= (uint64_t)bytes[i] << (8u * i);
    return result;
}

static bool
needed(const struct access_fixture *access, uint64_t sequence)
{
    const uint64_t *sequences = (const uint64_t *)access->needed.data;
    for (size_t i = 0u; i < access->needed.len / sizeof(*sequences); ++i) {
        if (sequences[i] == sequence) return true;
    }
    return false;
}

static void
need_sequence(struct access_fixture *access, uint64_t sequence)
{
    if (sequence && !needed(access, sequence)) {
        assert(!snag_buf_append(&access->needed, &sequence, sizeof(sequence)));
    }
}

/* Collect one-level original literal references independently of the reader.
 * The fixture oracle authenticates the prefix; a production consumer must pin
 * the checkpoint before using these derived locations. */
static void
need_receipt(int fd, const struct snag_binary_anchor *through, uint64_t wanted,
    struct access_fixture *access)
{
    if (!wanted) return;
    need_sequence(access, wanted);
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_batch batch;
    struct snag_binary_anchor before;
    assert(!snag_binary_batch_find(fd, through, wanted, &scratch, &batch, &before));
    size_t offset = SNAG_BINARY_BATCH_HEADER_SIZE;
    struct snag_binary_record record;
    uint64_t sequence;
    int rc;
    while ((rc = snag_binary_record_next(&batch, &offset, &record, &sequence)) == 0) {
        if (sequence != wanted) continue;
        struct snag_binary_event event;
        assert(!snag_binary_event_decode(&record, &event));
        if (event.kind == SNAG_BINARY_IRC_ADMITTED) {
            struct snag_binary_event embedded;
            assert(!snag_binary_event_decode(&event.data.irc_admitted.input, &embedded));
            event = embedded;
        }
        switch (event.kind) {
        case SNAG_BINARY_INPUT_RECEIVED:
            need_sequence(access, event.data.input.text_ref.target.sequence);
            need_sequence(access, event.data.input.content_ref.target.sequence);
            need_sequence(access, event.data.input.instructions_ref.target.sequence);
            break;
        case SNAG_BINARY_FUTURE_TURN_QUEUED:
        case SNAG_BINARY_FUTURE_TURN_EDITED:
            need_sequence(access, event.data.queued.text_ref.target.sequence);
            need_sequence(access, event.data.queued.content_ref.target.sequence);
            if (event.kind == SNAG_BINARY_FUTURE_TURN_QUEUED && event.data.queued.has_voice) {
                const struct snag_binary_voice_source *voice = &event.data.queued.voice;
                need_sequence(access, voice->transcript_ref.target.sequence);
                need_sequence(access, voice->request_ref.target.sequence);
            }
            break;
        case SNAG_BINARY_STEERING_ADDED:
        case SNAG_BINARY_IRC_REPLY_REMINDER:
            need_sequence(access, event.data.steering_input.text_ref.target.sequence);
            need_sequence(access, event.data.steering_input.content_ref.target.sequence);
            break;
        default:
            assert(event.kind == SNAG_BINARY_RESPONSE_OUTPUT_CORRECTION);
            break;
        }
        break;
    }
    assert(!rc);
    snag_buf_free(&scratch);
}

static void
make_access(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_anchor *boundary, const struct snag_binary_checkpoint_inputs *view,
    struct access_fixture *access)
{
    access->needed.max = access->entries.max = access->bytes.max = SIZE_MAX;
    need_receipt(fd, through, view->input, access);
    for (size_t i = 0u; i < view->queue_count; ++i) {
        need_receipt(fd, through, number(view->queue + 24u * i), access);
        need_receipt(fd, through, number(view->queue + 24u * i + 8u), access);
    }
    for (size_t i = 0u; i < view->steering_count; ++i) {
        need_receipt(fd, through, number(view->steering + 16u * i), access);
    }
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    assert(snag_pread(fd, header, sizeof(header), 0) == (ssize_t)sizeof(header));
    struct snag_binary_identity identity;
    struct snag_binary_anchor cursor;
    assert(!snag_binary_header_decode(header, sizeof(header), &identity, &cursor));
    struct snag_binary_index_tree tree = {0};
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_buf flat = {.max = SNAG_BINARY_INDEX_BATCH_MAX};
    while (cursor.end < boundary->end) {
        struct snag_binary_batch batch;
        struct snag_binary_anchor after;
        assert(!snag_binary_batch_read(fd, boundary->end, &cursor, &scratch, &batch, &after));
        assert(!snag_binary_index_tree_append_batch(NULL, &tree, &identity,
            &cursor, &after, batch.data, batch.size));
        snag_buf_reset(&flat);
        assert(!snag_binary_index_append_batch(&flat, &identity, &cursor, &after,
            batch.data, batch.size));
        for (uint64_t sequence = cursor.next_seq; sequence < after.next_seq; ++sequence) {
            if (!needed(access, sequence)) continue;
            struct snag_binary_index_entry entry;
            size_t offset = (size_t)(sequence - cursor.next_seq) * SNAG_BINARY_INDEX_ENTRY_SIZE;
            assert(!snag_binary_index_entry_decode(flat.data + offset,
                SNAG_BINARY_INDEX_ENTRY_SIZE, &identity, sequence, &entry));
            assert(!snag_buf_append(&access->entries, &entry, sizeof(entry)));
        }
        cursor = after;
    }
    assert(cursor.end == boundary->end && cursor.next_seq == boundary->next_seq &&
        cursor.turns == boundary->turns && cursor.previous == boundary->previous &&
        !memcmp(cursor.digest, boundary->digest, 32u));
    assert(!snag_binary_checkpoint_index_encode(&access->bytes, &identity,
        boundary, &tree, (const struct snag_binary_index_entry *)access->entries.data,
        access->entries.len / sizeof(struct snag_binary_index_entry)));
    unsigned char root[32];
    assert(!snag_binary_index_tree_root(&tree, root));
    assert(!snag_binary_checkpoint_index_decode(access->bytes.data, access->bytes.len,
        &identity, boundary, root, &access->index));
    snag_buf_free(&scratch);
    snag_buf_free(&flat);
}

static void
free_access(struct access_fixture *access)
{
    snag_buf_free(&access->bytes);
    snag_buf_free(&access->entries);
    snag_buf_free(&access->needed);
}

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
    const struct snag_binary_checkpoint_inputs *view, const struct snag_session *state,
    const struct snag_binary_checkpoint_index *access)
{
    for (unsigned mode = 0u; mode < 2u; ++mode) {
        struct snag_binary_checkpoint_inputs_state out;
        memset(&out, 0xa5, sizeof(out));
        struct snag_binary_checkpoint_inputs_state saved = out;
        assert(snag_binary_checkpoint_inputs_read(fd, anchor, mode ? access : NULL,
            view, state, &out) < 0);
        assert(!memcmp(&out, &saved, sizeof(out)));
    }
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
    int64_t position = snag_seek(fd, 0, SEEK_CUR);
    assert(position >= 0);
    struct access_fixture access = {0}, suffix_access = {0};
    make_access(fd, anchor, anchor, &view, &access);
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_batch batch;
    struct snag_binary_anchor before;
    assert(!snag_binary_batch_previous(fd, anchor, &scratch, &batch, &before));
    make_access(fd, anchor, &before, &view, &suffix_access);
    snag_buf_free(&scratch);
    struct snag_binary_checkpoint_inputs_state restored = {0};
    for (unsigned mode = 0u; mode < 3u; ++mode) {
        const struct snag_binary_checkpoint_index *selected = mode == 0u ? NULL :
            mode == 1u ? &access.index : &suffix_access.index;
        int rc = snag_binary_checkpoint_inputs_read(fd, anchor, selected, &view, state, &restored);
        if (rc < 0) {
            fprintf(stderr, "checkpoint inputs mode=%u errno=%d input=%" PRIu64
                " queue=%zu steering=%zu locations=%zu\n", mode, errno, view.input,
                view.queue_count, view.steering_count, selected ? selected->entry_count : 0u);
        }
        assert(!rc);
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
    }
    assert(!restored.input && !restored.strings && !restored.queue && !restored.steering);
    snag_binary_checkpoint_inputs_free(&restored);
    snag_binary_checkpoint_inputs_free(NULL);
    bad_read(-1, anchor, &view, state, &access.index);
    bad_read(fd, NULL, &view, state, &access.index);
    bad_read(fd, anchor, NULL, state, &access.index);
    bad_read(fd, anchor, &view, NULL, &access.index);
    struct snag_binary_checkpoint_inputs bad = view;
    bad.queue_count = SIZE_MAX;
    bad_read(fd, anchor, &bad, state, &access.index);
    bad = view;
    bad.steering_count = SIZE_MAX;
    bad_read(fd, anchor, &bad, state, &access.index);
    bad = view;
    bad.input = 1u; /* Session creation cannot be used as an input receipt. */
    bad_read(fd, anchor, &bad, state, &access.index);
    bad.input = anchor->next_seq;
    bad_read(fd, anchor, &bad, state, &access.index);
    if (view.queue_count || view.steering_count || view.input) {
        struct snag_binary_anchor early = *anchor;
        early.next_seq = 1u;
        bad_read(fd, &early, &view, state, &access.index);
    }
    if (view.input) {
        struct snag_session active = *state;
        active.active_turn = true;
        bad_read(fd, anchor, &view, &active, &access.index);
    }
    bool admitted = false;
    for (size_t i = 0u; i < view.steering_count; ++i)
        admitted |= state->pending_steering[i].first_context_ms != 0u;
    if (admitted) {
        struct snag_session wrong = *state;
        wrong.active_turn = false;
        bad_read(fd, anchor, &view, &wrong, &access.index);
    }
    json_t *receipt = (json_t *)&restored;
    uint64_t timestamp = UINT64_MAX;
    assert(snag_binary_checkpoint_receipt_read(fd, anchor, NULL, 1u,
        SNAG_BINARY_RESPONSE_COMPLETED, &receipt, &timestamp) < 0);
    assert(receipt == (json_t *)&restored && timestamp == UINT64_MAX);
    const struct snag_binary_index_entry *entries =
        (const struct snag_binary_index_entry *)access.entries.data;
    size_t count = access.entries.len / sizeof(*entries);
    for (size_t omitted = 0u; omitted < count; ++omitted) {
        struct snag_buf kept = {.max = SIZE_MAX}, bytes = {.max = SIZE_MAX};
        for (size_t i = 0u; i < count; ++i) {
            if (i != omitted) assert(!snag_buf_append(&kept, &entries[i], sizeof(*entries)));
        }
        assert(!snag_binary_checkpoint_index_encode(&bytes, &access.index.identity,
            anchor, &access.index.tree, (const struct snag_binary_index_entry *)kept.data,
            count - 1u));
        unsigned char root[32];
        assert(!snag_binary_index_tree_root(&access.index.tree, root));
        struct snag_binary_checkpoint_index incomplete;
        assert(!snag_binary_checkpoint_index_decode(bytes.data, bytes.len,
            &access.index.identity, anchor, root, &incomplete));
        struct snag_binary_checkpoint_inputs_state saved;
        memset(&restored, 0xa5, sizeof(restored));
        memcpy(&saved, &restored, sizeof(saved));
        assert(snag_binary_checkpoint_inputs_read(fd, anchor, &incomplete,
            &view, state, &restored) < 0);
        assert(errno == ENOENT && !memcmp(&restored, &saved, sizeof(saved)));
        snag_buf_free(&kept);
        snag_buf_free(&bytes);
    }
    assert(snag_seek(fd, 0, SEEK_CUR) == position);
    free_access(&access);
    free_access(&suffix_access);
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
