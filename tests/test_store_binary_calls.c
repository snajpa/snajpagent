/* SPDX-License-Identifier: GPL-2.0-only */
#include "fs.h"
#include "store_binary_checkpoint.h"
#include "store_internal.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
same_call(const struct snag_pending_call *a, const struct snag_pending_call *b)
{
    assert(!strcmp(a->call_id, b->call_id));
    assert(!strcmp(a->action_sha256, b->action_sha256));
    assert(!strcmp(a->tool_name, b->tool_name));
    assert(!strcmp(a->process_handle, b->process_handle));
    assert(!strcmp(a->command, b->command));
    assert(!strcmp(a->workdir, b->workdir));
    assert(a->started == b->started && a->finished == b->finished);
}

static void
bad_decode(const void *data, size_t size)
{
    struct snag_binary_checkpoint_calls value;
    unsigned char saved[sizeof(value)];
    memset(saved, 0xa5, sizeof(saved));
    memcpy(&value, saved, sizeof(value));
    assert(snag_binary_checkpoint_calls_decode(data, size, &value) < 0);
    assert(!memcmp(&value, saved, sizeof(value)));
}

static void
bad_read(int fd, const struct snag_binary_anchor *anchor,
    const struct snag_binary_checkpoint_calls *calls, const struct snag_session *state)
{
    struct snag_pending_call canary;
    struct snag_pending_call *out = &canary;
    assert(snag_binary_checkpoint_calls_read(fd, anchor, calls, state, &out) < 0);
    assert(out == &canary);
}

void
test_store_binary_calls_state(int fd, const struct snag_binary_anchor *anchor,
    const struct snag_binary_checkpoint_call_source *source, const struct snag_session *state)
{
    struct snag_buf encoded = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_calls_encode(&encoded, source, state));
    assert(encoded.len == 42u + state->pending_call_count);
    struct snag_binary_checkpoint_calls view;
    assert(!snag_binary_checkpoint_calls_decode(encoded.data, encoded.len, &view));
    assert(view.count == state->pending_call_count);
    struct snag_pending_call *calls = NULL;
    int64_t position = snag_seek(fd, 0, SEEK_CUR);
    assert(!snag_binary_checkpoint_calls_read(fd, anchor, &view, state, &calls));
    for (size_t i = 0u; i < view.count; ++i) same_call(&calls[i], &state->pending_calls[i]);
    assert(snag_seek(fd, 0, SEEK_CUR) == position);
    bad_read(-1, anchor, &view, state);
    bad_read(fd, NULL, &view, state);
    bad_read(fd, anchor, NULL, state);
    bad_read(fd, anchor, &view, NULL);
    assert(snag_binary_checkpoint_calls_read(fd, anchor, &view, state, NULL) < 0);
    if (view.count) {
        struct snag_session wrong = *state;
        wrong.active_cycle = state->active_cycle == UINT32_MAX ? 0u : state->active_cycle + 1u;
        bad_read(fd, anchor, &view, &wrong);
        wrong = *state;
        wrong.active_turn_id[0] = wrong.active_turn_id[0] == 'a' ? 'b' : 'a';
        bad_read(fd, anchor, &view, &wrong);
        wrong = *state;
        wrong.active_response_id[0] = wrong.active_response_id[0] == 'a' ? 'b' : 'a';
        bad_read(fd, anchor, &view, &wrong);
        struct snag_binary_checkpoint_calls bad = view;
        ++bad.source.cwd.original.target.offset;
        bad_read(fd, anchor, &bad, state);
        bad = view;
        --bad.source.cwd.original.target.size;
        bad_read(fd, anchor, &bad, state);
        bad = view;
        bad.source.graph = anchor->next_seq;
        bad_read(fd, anchor, &bad, state);
        bad = view;
        bad.source.graph = bad.source.cwd.declaration;
        bad_read(fd, anchor, &bad, state);
        bad = view;
        bad.flags = NULL;
        bad_read(fd, anchor, &bad, state);
        unsigned char *flags = malloc(view.count + 1u);
        assert(flags);
        memcpy(flags, view.flags, view.count);
        flags[view.count] = 0u;
        bad = view;
        bad.flags = flags;
        ++bad.count;
        bad_read(fd, anchor, &bad, state);
        bad.count = view.count - 1u;
        bad_read(fd, anchor, &bad, state);
        bad.count = view.count;
        flags[0] = 4u;
        bad_read(fd, anchor, &bad, state);
        free(flags);
    } else {
        assert(!calls && !source->graph);
    }
    free(calls);
    snag_buf_free(&encoded);
}

static void
call_derivation(void)
{
    const char *id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    struct snag_response_item item = {.kind = SNAG_ITEM_TOOL_CALL, .call_id = id,
        .name = "exec_command"};
    char command[270];
    memset(command, 'x', 255u);
    memcpy(command + 255u, "\xc3\xa9 tail", 8u);
    item.arguments = json_pack("{s:s,s:s}", "command", command, "workdir", "./somewhere");
    assert(item.arguments);
    struct snag_pending_call a, b;
    assert(!snag_pending_call_from_item(&item, "/original", &a));
    assert(!snag_pending_call_from_item(&item, "/changed", &b));
    assert(strcmp(a.action_sha256, b.action_sha256));
    assert(!strcmp(a.call_id, id) && !strcmp(a.process_handle, id));
    assert(strlen(a.command) == 255u && !strcmp(a.workdir, "./somewhere"));
    assert(!a.started && !a.finished);
    item.name = "write_stdin";
    assert(!json_object_set_new(item.arguments, "handle", json_string(id)));
    assert(!snag_pending_call_from_item(&item, "/original", &a));
    assert(!strcmp(a.process_handle, id) && !*a.command && !*a.workdir);
    assert(!json_object_set_new(item.arguments, "handle", json_string("bad")));
    assert(!snag_pending_call_from_item(&item, "/original", &a));
    assert(!*a.process_handle);
    item.name = "read_file";
    assert(!snag_pending_call_from_item(&item, "/original", &a));
    b = a;
    assert(snag_pending_call_from_item(NULL, "/original", &a) < 0);
    assert(snag_pending_call_from_item(&item, NULL, &a) < 0);
    assert(snag_pending_call_from_item(&item, "/original", NULL) < 0);
    item.call_id = "bad";
    assert(snag_pending_call_from_item(&item, "/original", &a) < 0);
    same_call(&a, &b);
    json_decref(item.arguments);
}

void
test_store_binary_calls(void)
{
    call_derivation();
    struct snag_pending_call calls[4];
    memset(calls, 0, sizeof(calls));
    for (unsigned int i = 0u; i < 4u; ++i) {
        calls[i].started = (i & 1u) != 0u;
        calls[i].finished = (i & 2u) != 0u;
    }
    struct snag_session state = {.pending_calls = calls, .pending_call_count = 4u};
    struct snag_binary_checkpoint_call_source source = {
        .graph = UINT64_C(0x0102030405060708),
        .cwd = {.declaration = UINT64_C(0x0102030405060701),
            .original = {.target = {.sequence = UINT64_C(0x0102030405060701),
                .offset = 0x112233u, .size = 0x4455u}}}};
    struct snag_buf encoded = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_calls_encode(&encoded, &source, &state));
    assert(encoded.len == 46u);
    char digest[65];
    snag_sha256_hex(encoded.data, encoded.len, digest);
    assert(!strcmp(digest, "d8b4196121e804db55ac03c9c1b3e53f66490a24e4223d002fecb8674916fff2"));
    struct snag_binary_checkpoint_calls view;
    assert(!snag_binary_checkpoint_calls_decode(encoded.data, encoded.len, &view));
    assert(view.count == 4u && view.source.graph == source.graph);
    assert(!memcmp(view.flags, "\0\1\2\3", 4u));
    for (size_t size = 0u; size < encoded.len; ++size) bad_decode(encoded.data, size);
    bad_decode(NULL, encoded.len);
    assert(snag_binary_checkpoint_calls_decode(encoded.data, encoded.len, NULL) < 0);
    unsigned char broken[47];
    for (size_t i = 0u; i < 2u; ++i) {
        memcpy(broken, encoded.data, encoded.len);
        broken[i] ^= 2u;
        bad_decode(broken, encoded.len);
    }
    memcpy(broken, encoded.data, encoded.len);
    broken[46] = 0u;
    bad_decode(broken, sizeof(broken));
    for (size_t i = 42u; i < 46u; ++i) {
        for (unsigned int bit = 2u; bit < 8u; ++bit) {
            memcpy(broken, encoded.data, encoded.len);
            broken[i] |= (unsigned char)(1u << bit);
            bad_decode(broken, encoded.len);
        }
    }
    memcpy(broken, encoded.data, encoded.len);
    memset(broken + 34u, 255, 8u);
    bad_decode(broken, encoded.len);
    struct snag_buf budget = {.max = 49u};
    assert(!snag_buf_append(&budget, "keep", 4u));
    assert(snag_binary_checkpoint_calls_encode(&budget, &source, &state) < 0);
    assert(budget.len == 4u && !memcmp(budget.data, "keep", 4u));
    budget.max = 50u;
    assert(!snag_binary_checkpoint_calls_encode(&budget, &source, &state));
    assert(budget.len == 50u && !memcmp(budget.data + 4u, encoded.data, encoded.len));
    snag_buf_free(&budget);
    struct snag_buf alias = {.max = SIZE_MAX};
    assert(!snag_buf_append(&alias, calls, sizeof(calls)));
    state.pending_calls = (void *)alias.data;
    assert(!snag_binary_checkpoint_calls_encode(&alias, &source, &state));
    assert(!memcmp(alias.data + sizeof(calls), encoded.data, encoded.len));
    snag_buf_free(&alias);
    state.pending_calls = calls;
    struct snag_binary_checkpoint_call_source bad = source;
    bad.graph = 0u;
    assert(snag_binary_checkpoint_calls_encode(&encoded, &bad, &state) < 0);
    bad = source;
    bad.cwd.original.field = SNAG_BINARY_INPUT_TEXT;
    assert(snag_binary_checkpoint_calls_encode(&encoded, &bad, &state) < 0);
    bad = source;
    ++bad.cwd.original.target.sequence;
    assert(snag_binary_checkpoint_calls_encode(&encoded, &bad, &state) < 0);
    assert(encoded.len == 46u);
    state.pending_call_count = 0u;
    source = (struct snag_binary_checkpoint_call_source){0};
    snag_buf_reset(&encoded);
    assert(!snag_binary_checkpoint_calls_encode(&encoded, &source, &state));
    assert(encoded.len == 42u && encoded.data[0] == 1u);
    for (size_t i = 1u; i < encoded.len; ++i) assert(!encoded.data[i]);
    for (size_t i = 2u; i < 34u; ++i) {
        memcpy(broken, encoded.data, encoded.len);
        broken[i] = 1u;
        bad_decode(broken, encoded.len);
    }
    assert(!snag_binary_checkpoint_calls_decode(encoded.data, encoded.len, &view));
    assert(!view.count && !view.source.graph);
    snag_buf_free(&encoded);
}
