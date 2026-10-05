/* SPDX-License-Identifier: GPL-2.0-only */
#include "fixture_store_binary.h"
#include "fs.h"
#include "store_binary_checkpoint.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
same_process(const struct snag_process_state *a, const struct snag_process_state *b)
{
    assert(!strcmp(a->handle, b->handle));
    assert(!strcmp(a->command, b->command));
    assert(!strcmp(a->workdir, b->workdir));
    assert(!memcmp(a->output_bytes, b->output_bytes, sizeof(a->output_bytes)));
    assert(!memcmp(a->collected_bytes, b->collected_bytes, sizeof(a->collected_bytes)));
    assert(a->input_accepted == b->input_accepted && a->input_written == b->input_written);
    assert(a->input_pending == b->input_pending);
    assert(a->ready == b->ready && a->draining == b->draining);
    assert(!a->log_offset && !a->log_seq && !*a->log_hash);
}

static void
bad_decode(const void *data, size_t size)
{
    struct snag_binary_checkpoint_processes out;
    unsigned char saved[sizeof(out)];
    memset(saved, 0xa5, sizeof(saved));
    memcpy(&out, saved, sizeof(out));
    assert(snag_binary_checkpoint_processes_decode(data, size, &out) < 0);
    assert(!memcmp(&out, saved, sizeof(out)));
}

static void
bad_read(int fd, const struct snag_binary_anchor *anchor,
    const struct snag_binary_checkpoint_index *access,
    const struct snag_binary_checkpoint_processes *view, const struct snag_session *state)
{
    for (unsigned int mode = 0u; mode < 2u; ++mode) {
        struct snag_process_state canary;
        struct snag_process_state *out = &canary;
        assert(snag_binary_checkpoint_processes_read(fd, anchor, mode ? access : NULL, view,
            state, &out) < 0);
        assert(out == &canary);
    }
}

void
test_store_binary_processes_state(int fd, const struct snag_binary_anchor *anchor,
    const struct snag_binary_checkpoint_sources *sources, const struct snag_session *state)
{
    struct snag_buf access_bytes = {.max = SIZE_MAX};
    struct snag_binary_checkpoint_index access;
    binary_fixture_access(fd, anchor, &access_bytes, &access);
    assert(sources->process_count == state->process_count);
    struct snag_buf wire = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_processes_encode(&wire, sources, state));
    assert(wire.len == 10u + 97u * state->process_count);
    struct snag_binary_checkpoint_processes view;
    assert(!snag_binary_checkpoint_processes_decode(wire.data, wire.len, &view));
    struct snag_process_state *restored = NULL;
    int64_t position = snag_seek(fd, 0, SEEK_CUR);
    for (unsigned int mode = 0u; mode < 2u; ++mode) {
        assert(!snag_binary_checkpoint_processes_read(fd, anchor, mode ? &access : NULL, &view,
            state, &restored));
        for (size_t i = 0u; i < view.count; ++i) same_process(&restored[i], &state->processes[i]);
        if (!view.count) assert(!restored);
        free(restored);
        restored = NULL;
    }
    bad_read(-1, anchor, &access, &view, state);
    bad_read(fd, NULL, &access, &view, state);
    bad_read(fd, anchor, &access, NULL, state);
    bad_read(fd, anchor, &access, &view, NULL);
    assert(snag_binary_checkpoint_processes_read(fd, anchor, NULL, &view, state, NULL) < 0);
    if (view.count) {
        struct snag_session wrong = *state;
        wrong.active_turn_id[0] = wrong.active_turn_id[0] == 'a' ? 'b' : 'a';
        bad_read(fd, anchor, &access, &view, &wrong);
        wrong = *state;
        wrong.active_turn = false;
        bad_read(fd, anchor, &access, &view, &wrong);
        struct snag_binary_anchor early = *anchor;
        early.next_seq = sources->processes[0].started;
        bad_read(fd, &early, &access, &view, state);
        struct snag_binary_checkpoint_processes bad = view;
        bad.data = NULL;
        bad_read(fd, anchor, &access, &bad, state);
        bad = view;
        bad.count = SIZE_MAX;
        bad_read(fd, anchor, &access, &bad, state);
        unsigned char *changed = malloc(wire.len);
        assert(changed);
        memcpy(changed, wire.data, wire.len);
        bad = view;
        bad.data = changed + 10u;
        changed[10u + 32u] ^= 1u; /* Canonical cwd field offset. */
        bad_read(fd, anchor, &access, &bad, state);
        memcpy(changed, wire.data, wire.len);
        memset(changed + 10u + 40u, 255, 56u);
        for (unsigned int flags = 0u; flags < 4u; ++flags) {
            changed[106] = (unsigned char)flags;
            assert(!snag_binary_checkpoint_processes_decode(changed, wire.len, &bad));
            assert(!snag_binary_checkpoint_processes_read(fd, anchor, NULL, &bad,
                state, &restored));
            assert(restored[0].output_bytes[0] == UINT64_MAX);
            assert(restored[0].output_bytes[1] == UINT64_MAX);
            assert(restored[0].collected_bytes[0] == UINT64_MAX);
            assert(restored[0].collected_bytes[1] == UINT64_MAX);
            assert(restored[0].input_accepted == UINT64_MAX);
            assert(restored[0].input_written == UINT64_MAX);
            assert(restored[0].input_pending == UINT64_MAX);
            assert(restored[0].ready == ((flags & 1u) != 0u));
            assert(restored[0].draining == ((flags & 2u) != 0u));
            free(restored);
        }
        free(changed);
        struct snag_binary_checkpoint_process_source bad_source = sources->processes[0];
        struct snag_process_state canary, saved;
        memset(&canary, 0xa5, sizeof(canary));
        saved = canary;
        bad_source.handle[0] = bad_source.handle[0] == 'a' ? 'b' : 'a';
        assert(snag_binary_checkpoint_process_source_read(fd, anchor, NULL, &bad_source, state,
            &canary) < 0);
        assert(!memcmp(&canary, &saved, sizeof(canary)));
    }
    assert(snag_seek(fd, 0, SEEK_CUR) == position);
    for (size_t i = 0u; i < sources->process_count; ++i) {
        uint64_t required[] = {sources->processes[i].started, sources->processes[i].call.graph,
            sources->processes[i].call.cwd.declaration};
        for (size_t j = 0u; j < sizeof(required) / sizeof(required[0]); ++j) {
            struct snag_buf omitted = {.max = SIZE_MAX};
            struct snag_binary_checkpoint_index missing;
            binary_fixture_access_omit(&access, required[j], &omitted, &missing);
            struct snag_process_state canary;
            struct snag_process_state *output = &canary;
            errno = 0;
            assert(snag_binary_checkpoint_processes_read(fd, anchor, &missing,
                &view, state, &output) < 0 && errno == ENOENT && output == &canary);
            snag_buf_free(&omitted);
        }
    }
    snag_buf_free(&access_bytes);
    snag_buf_free(&wire);
}

void
test_store_binary_processes(void)
{
    uint64_t base = UINT64_C(0x0102030405060700);
    struct snag_process_state processes[2];
    memset(processes, 0, sizeof(processes));
    struct snag_binary_checkpoint_sources sources = {.process_count = 2u};
    sources.processes = calloc(2u, sizeof(*sources.processes));
    assert(sources.processes);
    for (size_t i = 0u; i < 2u; ++i) {
        struct snag_process_state *p = &processes[i];
        memset(p->handle, i ? 'b' : 'a', SNAG_ID_HEX_LEN);
        p->output_bytes[1] = 1u;
        p->collected_bytes[0] = INT64_MAX;
        p->collected_bytes[1] = (uint64_t)INT64_MAX + 1u;
        p->input_accepted = UINT64_MAX;
        p->input_written = 42u;
        p->input_pending = base;
        p->ready = p->draining = i == 0u;
        /* Physical scan caches are deliberately excluded from the block. */
        p->log_seq = 99u;
        p->log_offset = 111u;
        memset(p->log_hash, 'c', SNAG_SHA256_HEX_LEN);
        struct snag_binary_checkpoint_process_source *source = &sources.processes[i];
        memcpy(source->handle, p->handle, sizeof(source->handle));
        source->started = base + 5u + i;
        source->call.graph = base + 3u;
        source->call.cwd.declaration = base + 1u;
        source->call.cwd.original.target = (struct snag_binary_ref){
            .sequence = base + 1u, .offset = 20u, .size = 7u};
    }
    struct snag_session state = {.processes = processes, .process_count = 2u};
    struct snag_buf wire = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_processes_encode(&wire, &sources, &state));
    char hash[65];
    snag_sha256_hex(wire.data, wire.len, hash);
    assert(wire.len == 204u);
    assert(!strcmp(hash, "b38cee6f739dc7d7b01864ebafed256369ae4acd10b33f4219db778821704995"));
    struct snag_binary_checkpoint_processes view;
    assert(!snag_binary_checkpoint_processes_decode(wire.data, wire.len, &view));
    assert(view.count == 2u && view.data == wire.data + 10u);
    for (size_t size = 0u; size < wire.len; ++size) bad_decode(wire.data, size);
    bad_decode(NULL, wire.len);
    assert(snag_binary_checkpoint_processes_decode(wire.data, wire.len, NULL) < 0);
    unsigned char changed[205];
    memcpy(changed, wire.data, wire.len);
    changed[204] = 0u;
    bad_decode(changed, sizeof(changed));
    for (size_t i = 0u; i < 2u; ++i) {
        memcpy(changed, wire.data, wire.len);
        changed[i] ^= 1u;
        bad_decode(changed, wire.len);
        for (unsigned int bit = 2u; bit < 8u; ++bit) {
            memcpy(changed, wire.data, wire.len);
            changed[10u + i * 97u + 96u] |= (unsigned char)(1u << bit);
            bad_decode(changed, wire.len);
        }
    }
    memcpy(changed, wire.data, wire.len);
    memset(changed + 2u, 255, 8u);
    bad_decode(changed, wire.len);
    for (size_t offset = 10u; offset <= 34u; offset += 8u) {
        memcpy(changed, wire.data, wire.len);
        memset(changed + offset, 0, 8u);
        bad_decode(changed, wire.len);
    }
    memcpy(changed, wire.data, wire.len);
    memcpy(changed + 107u, changed + 10u, 8u);
    bad_decode(changed, wire.len);
    struct snag_buf limited = {.max = wire.len + 3u};
    assert(!snag_buf_append(&limited, "keep", 4u));
    assert(snag_binary_checkpoint_processes_encode(&limited, &sources, &state) < 0);
    assert(limited.len == 4u && !memcmp(limited.data, "keep", 4u));
    ++limited.max;
    assert(!snag_binary_checkpoint_processes_encode(&limited, &sources, &state));
    assert(!memcmp(limited.data + 4u, wire.data, wire.len));
    snag_buf_free(&limited);
    struct snag_buf alias = {.max = SIZE_MAX};
    assert(!snag_buf_append(&alias, processes, sizeof(processes)));
    state.processes = (void *)alias.data;
    assert(!snag_binary_checkpoint_processes_encode(&alias, &sources, &state));
    assert(!memcmp(alias.data + sizeof(processes), wire.data, wire.len));
    snag_buf_free(&alias);
    state.processes = processes;
    sources.processes[1].started = sources.processes[0].started;
    assert(snag_binary_checkpoint_processes_encode(&wire, &sources, &state) < 0);
    ++sources.processes[1].started;
    sources.processes[0].handle[0] = 'f';
    assert(snag_binary_checkpoint_processes_encode(&wire, &sources, &state) < 0);
    sources.processes[0].handle[0] = 'a';
    --sources.process_count;
    assert(snag_binary_checkpoint_processes_encode(&wire, &sources, &state) < 0);
    assert(wire.len == 204u);
    snag_binary_checkpoint_sources_free(&sources);
    assert(!sources.processes && !sources.process_count && !sources.calls.graph);
    snag_binary_checkpoint_sources_free(&sources);
    snag_binary_checkpoint_sources_free(NULL);
    state.process_count = 0u;
    snag_buf_reset(&wire);
    assert(!snag_binary_checkpoint_processes_encode(&wire, &sources, &state));
    assert(wire.len == 10u && wire.data[0] == 1u);
    for (size_t i = 1u; i < wire.len; ++i) assert(!wire.data[i]);
    assert(!snag_binary_checkpoint_processes_decode(wire.data, wire.len, &view));
    assert(!view.count);
    snag_buf_free(&wire);
}
