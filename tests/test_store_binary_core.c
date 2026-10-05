/* SPDX-License-Identifier: GPL-2.0-only */
#include "fixture_store_binary.h"
#include "fs.h"
#include "json.h"
#include "store_binary_checkpoint.h"
#include "store_internal.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define CORE_HEADER 84u

static const struct slot { const char *key; size_t offset; } slots[] = {
#define SLOT(f) {#f, offsetof(struct snag_session, f)}
    SLOT(cwd), SLOT(first_user), SLOT(last_user), SLOT(active_prompt), SLOT(goal_prompt),
    SLOT(goal_blocker), SLOT(timer_text), SLOT(banner_text), SLOT(steering_override), SLOT(name)
#undef SLOT
};

static uint64_t
number(const unsigned char *bytes)
{
    uint64_t result = 0u;
    for (size_t i = 0u; i < 8u; ++i) result |= (uint64_t)bytes[i] << (i * 8u);
    return result;
}

static void
put_number(unsigned char *bytes, uint64_t value)
{
    for (size_t i = 0u; i < 8u; ++i) bytes[i] = (unsigned char)(value >> (i * 8u));
}

static void
keep_text(json_t *strings, const char *key, const char *text)
{
    if (text) assert(!json_object_set_new(strings, key, json_string(text)));
}

static json_t *
semantic_state(const struct snag_session *state)
{
    json_t *value = snag_checkpoint_state_encode(state), *owners = json_object();
    assert(value && owners);
    /* Unreachable dictionary entries and physical scan caches are private
     * implementation storage, not semantic state or native seek authority. */
    for (size_t i = 0u; i < COUNT(slots); ++i) {
        const char *text;
        memcpy(&text, (const unsigned char *)state + slots[i].offset, sizeof(text));
        keep_text(owners, slots[i].key, text);
    }
    keep_text(owners, "irc_snapshot", snag_json_string(state->strings, "irc_snapshot"));
    json_t *options = json_object_get(state->strings, "resume_options");
    if (options) assert(!json_object_set(owners, "resume_options", options));
    for (size_t i = 0u; i < state->pending_queue_count; ++i)
        keep_text(owners, state->pending_queue[i].queue_id, state->pending_queue[i].text);
    for (size_t i = 0u; i < state->pending_steering_count; ++i)
        keep_text(owners, state->pending_steering[i].steering_id, state->pending_steering[i].text);
    if (!json_object_size(owners)) { json_decref(owners); owners = json_null(); }
    assert(!json_object_set_new(value, "strings", owners));
    json_t *processes = json_object_get(value, "processes");
    for (size_t i = 0u; i < json_array_size(processes); ++i) {
        json_t *process = json_array_get(processes, i);
        json_object_del(process, "log_offset");
        json_object_del(process, "log_seq");
        json_object_del(process, "log_hash");
    }
    return value;
}

static void
same(const struct snag_session *expected, const struct snag_session *actual)
{
    json_t *left = semantic_state(expected), *right = semantic_state(actual);
    const char *key;
    json_t *value;
    json_object_foreach(left, key, value) {
        if (!json_equal(value, json_object_get(right, key)))
            fprintf(stderr, "native core mismatch: %s\n", key);
    }
    assert(json_equal(left, right));
    const json_t *owners = json_object_get(right, "strings");
    assert(actual->strings ? json_equal(actual->strings, owners) : json_is_null(owners));
    json_decref(left);
    json_decref(right);
    for (size_t i = 0u; i < COUNT(slots); ++i) {
        const char *text;
        memcpy(&text, (const unsigned char *)actual + slots[i].offset, sizeof(text));
        assert(text == snag_json_string(actual->strings, slots[i].key));
    }
    for (size_t i = 0u; i < actual->pending_queue_count; ++i) {
        const struct snag_queued_turn *item = &actual->pending_queue[i];
        assert(item->text == snag_json_string(actual->strings, item->queue_id));
    }
    for (size_t i = 0u; i < actual->pending_steering_count; ++i) {
        const struct snag_pending_steering *item = &actual->pending_steering[i];
        assert(item->text == snag_json_string(actual->strings, item->steering_id));
    }
    for (size_t i = 0u; i < actual->process_count; ++i) {
        assert(!actual->processes[i].log_offset && !actual->processes[i].log_seq &&
            !actual->processes[i].log_hash[0]);
    }
    assert(actual->dir_fd == -1 && actual->log_fd == -1 && actual->lock_fd == -1);
    assert(!actual->pending_log && !actual->on_commit && !actual->on_checkpoint &&
        !actual->on_commit_free && !actual->on_commit_opaque && !actual->voice_projection &&
        !actual->checkpoint_context && !actual->checkpoint_state && !actual->dir_path &&
        !actual->history_cursor.offset && !actual->history_cursor.next_seq);
}

static void
reject(int fd, const struct snag_binary_checkpoint_frame *frame)
{
    struct snag_session state, before;
    struct snag_binary_checkpoint_sources sources, saved;
    memset(&state, 0xa5, sizeof(state));
    memcpy(&before, &state, sizeof(before));
    memset(&sources, 0x5a, sizeof(sources));
    memcpy(&saved, &sources, sizeof(saved));
    int64_t position = snag_seek(fd, 0, SEEK_CUR);
    assert(snag_binary_checkpoint_core_read(fd, frame, NULL, &state, &sources) < 0);
    assert(!memcmp(&before, &state, sizeof(state)));
    assert(!memcmp(&saved, &sources, sizeof(sources)));
    assert(position == snag_seek(fd, 0, SEEK_CUR));
}

static void
encode_checks(const struct snag_buf *core, const struct snag_binary_checkpoint_sources *sources,
    const struct snag_session *state)
{
    struct snag_buf output = {.max = core->len + 6u};
    assert(!snag_buf_append(&output, "prefix", 6u));
    --output.max;
    assert(snag_binary_checkpoint_core_encode(&output, sources, state) < 0);
    assert(output.len == 6u && !memcmp(output.data, "prefix", 6u));
    ++output.max;
    assert(!snag_binary_checkpoint_core_encode(&output, sources, state));
    assert(output.len == core->len + 6u && !memcmp(output.data + 6u, core->data, core->len));
    output.len = 6u;
    struct snag_binary_checkpoint_sources bad = *sources;
    bad.download_count = sources->download_count + 1u;
    assert(snag_binary_checkpoint_core_encode(&output, &bad, state) < 0);
    assert(output.len == 6u && !memcmp(output.data, "prefix", 6u));
    assert(snag_binary_checkpoint_core_encode(NULL, sources, state) < 0);
    assert(snag_binary_checkpoint_core_encode(&output, NULL, state) < 0);
    assert(snag_binary_checkpoint_core_encode(&output, sources, NULL) < 0);
    for (size_t i = 0u; i < 5u; ++i) {
        struct snag_session unsupported = *state;
        unsupported.voice_history = (struct snag_voice_history_root){0};
        switch (i) {
        case 0u: unsupported.voice_history.adopted_seq = 1u; break;
        case 1u: unsupported.voice_history.transfer_id[0] = '1'; break;
        case 2u: unsupported.voice_history.begin.offset = 1; break;
        case 3u: unsupported.voice_history.begin.next_seq = 1u; break;
        default: unsupported.voice_history.begin.prev_sha256[0] = '1'; break;
        }
        assert(snag_binary_checkpoint_core_encode(&output, sources, &unsupported) < 0 &&
            errno == EINVAL);
        assert(output.len == 6u && !memcmp(output.data, "prefix", 6u));
    }
    snag_buf_free(&output);
    /* These native structs are borrowed test inputs in an append prefix,
     * not any part of the checkpoint wire representation. */
    size_t prefix = sizeof(*state) + sizeof(*sources);
    struct snag_buf alias = {.max = SIZE_MAX};
    assert(!snag_buf_append(&alias, state, sizeof(*state)));
    assert(!snag_buf_append(&alias, sources, sizeof(*sources)));
    assert(!snag_binary_checkpoint_core_encode(&alias,
        (const struct snag_binary_checkpoint_sources *)(alias.data + sizeof(*state)),
        (const struct snag_session *)alias.data));
    assert(alias.len == prefix + core->len &&
        !memcmp(alias.data + prefix, core->data, core->len));
    assert(!memcmp(alias.data, state, sizeof(*state)) &&
        !memcmp(alias.data + sizeof(*state), sources, sizeof(*sources)));
    snag_buf_free(&alias);
}

static void
golden_header(const struct snag_binary_checkpoint_sources *original,
    const struct snag_session *snapshot)
{
    /* Structural prefix only: these epochs do not claim a valid journal state. */
    const unsigned char golden[] = {2u, 0u, 7u, 0u, 8u, 7u, 6u, 5u, 4u, 3u, 2u, 1u,
        0x18u, 0x17u, 0x16u, 0x15u, 0x14u, 0x13u, 0x12u, 0x11u};
    struct snag_binary_checkpoint_sources sources = *original;
    sources.active_compact = 0x0102030405060708ULL;
    sources.response_start = 0x1112131415161718ULL;
    struct snag_session state = *snapshot;
    state.response_public = NULL;
    state.response_public_bytes = 0u;
    struct snag_buf wire = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_core_encode(&wire, &sources, &state));
    assert(wire.len > CORE_HEADER && !memcmp(wire.data, golden, sizeof(golden)));
    snag_buf_free(&wire);
}

static void
bad_wire(int fd, struct snag_binary_checkpoint_frame frame)
{
    size_t size = frame.core.size;
    unsigned char *copy = malloc(size + 1u);
    assert(copy);
    memcpy(copy, frame.core.data, size);
    for (size_t i = 0u; i < size; ++i) { frame.core.size = i; reject(fd, &frame); }
    frame.core.data = copy;
    frame.core.size = size + 1u;
    copy[size] = 0u;
    reject(fd, &frame);
    frame.core.size = size;
    frame.core.version = 1u;
    reject(fd, &frame);
    frame.core.version = SNAG_BINARY_CORE_VERSION;
    size_t bad[] = {0u, 1u, 2u, 3u};
    for (size_t i = 0u; i < COUNT(bad); ++i) {
        copy[bad[i]] ^= 0x80u;
        reject(fd, &frame);
        copy[bad[i]] ^= 0x80u;
    }
    uint64_t voice = number(copy + 20u);
    uint64_t bad_voice[] = {1u, 2u, frame.boundary.next_seq, UINT64_MAX};
    for (size_t i = 0u; i < COUNT(bad_voice); ++i) {
        put_number(copy + 20u, bad_voice[i]);
        reject(fd, &frame);
    }
    put_number(copy + 20u, voice);
    size_t offset = CORE_HEADER;
    for (size_t i = 0u; i < 7u; ++i) {
        unsigned char saved[8];
        memcpy(saved, copy + 28u + i * 8u, sizeof(saved));
        memset(copy + 28u + i * 8u, 0xff, sizeof(saved));
        reject(fd, &frame);
        memset(copy + 28u + i * 8u, 0, sizeof(saved));
        reject(fd, &frame);
        memcpy(copy + 28u + i * 8u, saved, sizeof(saved));
        copy[offset] = 2u;
        reject(fd, &frame);
        copy[offset] = 1u;
        offset += (size_t)number(saved);
    }
    assert(offset == size);
    frame.core.data = NULL;
    reject(fd, &frame);
    free(copy);
}

static void
bad_totals(int fd, struct snag_binary_checkpoint_frame frame,
    const struct snag_binary_checkpoint_sources *sources, const struct snag_session *state)
{
    struct snag_buf core = {.max = SIZE_MAX};
    for (size_t i = 0u; i < 3u; ++i) {
        struct snag_session changed = *state;
        if (i == 0u) changed.pending_queue_bytes ^= 1u;
        if (i == 1u) changed.pending_steering_bytes ^= 1u;
        if (i == 2u) changed.response_public_bytes ^= 1u;
        snag_buf_reset(&core);
        assert(!snag_binary_checkpoint_core_encode(&core, sources, &changed));
        frame.core.data = (const unsigned char *)core.data;
        frame.core.size = core.len;
        reject(fd, &frame);
    }
    snag_buf_free(&core);
}

struct closure_cancel {
    size_t calls, fail_at;
};

static bool
cancel_closure(void *opaque)
{
    struct closure_cancel *cancel = opaque;
    return ++cancel->calls == cancel->fail_at;
}

static void
closure_label_precision(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_sources *sources, const struct snag_session *state,
    const struct snag_binary_checkpoint_index *available,
    const struct snag_binary_checkpoint_index *selected)
{
    if (!sources->process_count || sources->response_end || state->pending_steering_count) return;
    for (size_t i = 0u; i < SNAG_BINARY_CHECKPOINT_TEXT_COUNT; ++i) {
        uint64_t sequence = sources->texts.slots[i].declaration;
        struct snag_binary_index_entry entry;
        if (sequence && !snag_binary_checkpoint_index_find(available, sequence, &entry) &&
            entry.kind == SNAG_BINARY_RESPONSE_OUTPUT_CORRECTION) return;
    }
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    for (size_t i = 0u; i < sources->process_count; ++i) {
        uint64_t wanted = sources->processes[i].call.graph;
        struct snag_binary_batch batch;
        struct snag_binary_anchor before;
        assert(!snag_binary_checkpoint_batch_find(fd, through, available, wanted,
            &scratch, &batch, &before));
        size_t cursor = SNAG_BINARY_BATCH_HEADER_SIZE;
        struct snag_binary_record record;
        uint64_t sequence;
        int rc;
        while ((rc = snag_binary_record_next(&batch, &cursor, &record, &sequence)) == 0) {
            if (sequence != wanted) continue;
            struct snag_binary_event event;
            assert(!snag_binary_event_decode(&record, &event));
            assert(event.kind == SNAG_BINARY_RESPONSE_COMPLETED);
            size_t offset = 0u;
            struct snag_binary_graph_item item;
            while ((rc = snag_binary_graph_items_next(&event.data.response_completed.items,
                    &offset, &item)) == 0) {
                if (item.kind == SNAG_BINARY_ITEM_TOOL_CALL ||
                    !item.data.output.source.first.sequence) continue;
                struct snag_binary_index_entry entry;
                assert(snag_binary_checkpoint_index_find(selected,
                    item.data.output.source.first.sequence, &entry) == 1);
                assert(snag_binary_checkpoint_index_find(selected,
                    item.data.output.source.last_sequence, &entry) == 1);
            }
            assert(rc == 1);
            break;
        }
    }
    snag_buf_free(&scratch);
}

static void
closure_core_checks(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_sources *sources, const struct snag_session *state,
    const struct snag_binary_checkpoint_frame *frame,
    const struct snag_binary_checkpoint_index *available)
{
    /* Core-only fixture recipe names creation. Paired context fixtures exercise
     * their actual recent/history recipe roots independently. */
    json_t *recent = json_pack("[{s:I,s:I,s:s,s:b,s:b,s:b}]", "seq", (json_int_t)1u,
        "time", (json_int_t)available->identity.created_ms, "turn", "", "active", false,
        "unfinished", false, "processes", false);
    json_t *history = json_array();
    assert(recent && history);
    struct snag_buf provider = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_provider_encode(&provider, state, recent, history));
    struct snag_buf selected = {.max = SIZE_MAX};
    struct closure_cancel cancel = {0};
    int64_t position = snag_seek(fd, 0, SEEK_CUR);
    int rc = snag_binary_checkpoint_access_capture(fd, through, available, &available->tree,
        sources, state, provider.data, provider.len, cancel_closure, &cancel, &selected);
    if (rc < 0) fprintf(stderr, "core closure sequence %llu: %d\n",
        (unsigned long long)through->next_seq, errno);
    assert(!rc && selected.len <= SNAG_BINARY_CHECKPOINT_INDEX_HEADER_SIZE +
        available->entry_count * SNAG_BINARY_INDEX_ENTRY_SIZE);
    unsigned char root[32];
    assert(!snag_binary_index_tree_root(&available->tree, root));
    struct snag_binary_checkpoint_index access;
    assert(!snag_binary_checkpoint_index_decode(selected.data, selected.len,
        &available->identity, through, root, &access));
    struct snag_buf repeated = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_access_capture(fd, through, &access, &access.tree,
        sources, state, provider.data, provider.len, NULL, NULL, &repeated));
    assert(selected.len == repeated.len && !memcmp(selected.data, repeated.data, selected.len));
    struct snag_session restored;
    snag_session_init(&restored);
    struct snag_binary_checkpoint_sources origins = {0};
    assert(!snag_binary_checkpoint_core_read(fd, frame, &access, &restored, &origins));
    closure_label_precision(fd, through, sources, state, available, &access);
    snag_buf_free(&selected);
    same(state, &restored);
    snag_session_close(&restored);
    snag_binary_checkpoint_sources_free(&origins);
    unsigned int class_mode = (!!state->pending_call_count) | (!!state->process_count << 1u) |
        (!!state->voice_history.adopted_seq << 2u) | (!!sources->response_end << 3u) |
        (!!state->pending_queue_count << 4u);
    static bool checked[32];
    if (!checked[class_mode]) {
        checked[class_mode] = true;
        struct snag_buf output = {.max = SIZE_MAX};
        assert(!snag_buf_append(&output, "keep", 4u));
        for (size_t i = 1u; i <= cancel.calls; ++i) {
            struct closure_cancel stopped = {.fail_at = i};
            assert(snag_binary_checkpoint_access_capture(fd, through, available, &available->tree,
                sources, state, provider.data, provider.len, cancel_closure, &stopped,
                &output) < 0 && errno == ECANCELED);
            assert(stopped.calls == i && output.len == 4u && !memcmp(output.data, "keep", 4u));
        }
        struct snag_buf omitted = {.max = SIZE_MAX};
        struct snag_binary_checkpoint_index missing;
        binary_fixture_access_omit(available, 1u, &omitted, &missing);
        assert(snag_binary_checkpoint_access_capture(fd, through, &missing, &missing.tree,
            sources, state, provider.data, provider.len, NULL, NULL, &output) < 0 &&
            errno == ENOENT);
        assert(output.len == 4u && !memcmp(output.data, "keep", 4u));
        snag_buf_free(&omitted);
        struct snag_binary_index_tree bad = available->tree;
        --bad.count;
        assert(snag_binary_checkpoint_access_capture(fd, through, available, &bad,
            sources, state, provider.data, provider.len, NULL, NULL, &output) < 0 &&
            errno == EINVAL);
        struct snag_binary_checkpoint_index wrong = *available;
        wrong.identity.id[0] ^= 1u;
        assert(snag_binary_checkpoint_access_capture(fd, through, &wrong, &available->tree,
            sources, state, provider.data, provider.len, NULL, NULL, &output) < 0 &&
            errno == EINVAL);
        assert(snag_binary_checkpoint_access_capture(-1, through, available, &available->tree,
            sources, state, provider.data, provider.len, NULL, NULL, &output) < 0 &&
            errno == EINVAL);
        assert(output.len == 4u && !memcmp(output.data, "keep", 4u));
        snag_buf_free(&output);
    }
    assert(snag_seek(fd, 0, SEEK_CUR) == position);
    snag_buf_free(&repeated);
    snag_buf_free(&provider);
    json_decref(recent);
    json_decref(history);
}

void
test_store_binary_core_state(int fd, const struct snag_binary_anchor *anchor,
    const struct snag_binary_checkpoint_sources *sources, const struct snag_session *state)
{
    struct snag_buf core = {.max = SIZE_MAX}, encoded = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_core_encode(&core, sources, state));
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    assert(snag_pread(fd, header, sizeof(header), 0) == sizeof(header));
    struct snag_binary_checkpoint_frame frame = {.boundary = *anchor, .generation = 1u,
        .core = {.version = SNAG_BINARY_CORE_VERSION,
            .data = (const unsigned char *)core.data, .size = core.len},
        /* Opaque framing fixture only; this consumer does not decode a provider view. */
        .provider = {.version = 1u, .data = (const unsigned char *)"x", .size = 1u}};
    struct snag_binary_anchor root;
    assert(!snag_binary_header_decode(header, sizeof(header), &frame.identity, &root));
    assert(!snag_binary_checkpoint_frame_encode(&encoded, &frame));
    assert(!snag_binary_checkpoint_frame_decode(encoded.data, encoded.len,
        &frame.identity, anchor, &frame));
    struct snag_buf access_bytes = {.max = SIZE_MAX};
    struct snag_binary_checkpoint_index access;
    binary_fixture_access(fd, anchor, &access_bytes, &access);
    closure_core_checks(fd, anchor, sources, state, &frame, &access);
    struct snag_session indexed;
    snag_session_init(&indexed);
    struct snag_binary_checkpoint_sources indexed_sources = {0};
    int64_t indexed_position = snag_seek(fd, 0, SEEK_CUR);
    assert(!snag_binary_checkpoint_core_read(fd, &frame, &access, &indexed, &indexed_sources));
    assert(indexed_position == snag_seek(fd, 0, SEEK_CUR));
    snag_buf_free(&access_bytes);
    same(state, &indexed);
    snag_binary_checkpoint_sources_free(&indexed_sources);
    snag_session_close(&indexed);
    struct snag_session restored, old;
    snag_session_init(&restored);
    restored.strings = json_pack("{s:s}", "keep", "old owner");
    assert(restored.strings);
    old = restored;
    struct snag_binary_checkpoint_sources recovered = {0};
    int64_t position = snag_seek(fd, 0, SEEK_CUR);
    assert(!snag_binary_checkpoint_core_read(fd, &frame, NULL, &restored, &recovered));
    assert(!strcmp(snag_json_string(old.strings, "keep"), "old owner"));
    snag_session_close(&old);
    assert(position == snag_seek(fd, 0, SEEK_CUR));
    /* The assembled objects own everything they need after frame bytes expire. */
    snag_buf_free(&encoded);
    frame.core.data = (const unsigned char *)core.data;
    frame.provider.data = (const unsigned char *)"x";
    same(state, &restored);
    assert(recovered.response_start == sources->response_start &&
        recovered.active_compact == sources->active_compact &&
        recovered.resume_options == sources->resume_options);
    struct snag_buf again = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_core_encode(&again, &recovered, &restored));
    assert(core.len == again.len && !memcmp(core.data, again.data, core.len));
    snag_binary_checkpoint_sources_free(&recovered);
    snag_session_close(&restored);
    encode_checks(&core, sources, state);
    bad_totals(fd, frame, sources, state);
    static bool checked_wire;
    if (!checked_wire) {
        bad_wire(fd, frame);
        golden_header(sources, state);
        reject(fd, NULL);
        struct snag_binary_checkpoint_frame bad = frame;
        bad.boundary.next_seq = 0u;
        reject(fd, &bad);
        bad = frame;
        bad.boundary.turns = bad.boundary.next_seq;
        reject(fd, &bad);
        bad = frame;
        bad.boundary.end = UINT64_MAX;
        reject(fd, &bad);
        bad.boundary.end = SNAG_BINARY_HEADER_SIZE - 1u;
        reject(fd, &bad);
        checked_wire = true;
    }
    if (sources->texts.through) {
        int closed = dup(fd);
        assert(closed >= 0 && !close(closed));
        reject(closed, &frame);
    }
    frame.core.data = (const unsigned char *)core.data;
    for (size_t i = 4u; i <= 12u; i += 8u) {
        unsigned char saved[8];
        memcpy(saved, core.data + i, sizeof(saved));
        put_number((unsigned char *)core.data + i, anchor->next_seq);
        reject(fd, &frame);
        if (number(saved)) {
            put_number((unsigned char *)core.data + i, 0u);
            reject(fd, &frame);
            put_number((unsigned char *)core.data + i, 1u);
            reject(fd, &frame);
        }
        memcpy(core.data + i, saved, sizeof(saved));
    }
    snag_buf_free(&again);
    snag_buf_free(&encoded);
    snag_buf_free(&core);
}
