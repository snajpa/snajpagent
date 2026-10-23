/* SPDX-License-Identifier: GPL-2.0-only */
#include "store.h"
#include "vm_reader.h"
#include "vm_terminal.h"

#include <assert.h>
#include <string.h>
#ifndef _WIN32
#include <poll.h>
#include <stdlib.h>
#endif

/* Session discovery is unrelated to the handoff's mutation boundary. */
static int
locate_fixture(struct snag_store *store, struct snag_session *session, const char *prefix,
    snag_store_emit_fn emit, void *opaque, char *error, size_t error_size)
{
    (void)store;
    (void)emit;
    (void)opaque;
    (void)error;
    (void)error_size;
    memcpy(session->id, prefix, sizeof(session->id));
    return 0;
}

static int terminal_step_fixture(struct snag_vm_terminal *);
static size_t terminal_rows_fixture(const struct snag_vm_terminal *);
#ifndef _WIN32
static int poll_fixture(struct pollfd *, nfds_t, int);
static void *poll_calloc_fixture(size_t, size_t);
static void *poll_realloc_fixture(void *, size_t);
static snag_wake_fd reader_fd_fixture(const struct snag_vm_reader *);
#define poll poll_fixture
#define calloc poll_calloc_fixture
#define realloc poll_realloc_fixture
#define snag_vm_reader_fd reader_fd_fixture
#endif

#define snag_session_locate locate_fixture
#define snag_vm_terminal_step terminal_step_fixture
#define snag_vm_terminal_history_rows terminal_rows_fixture
#include "../src/vm.c"
#undef snag_session_locate
#undef snag_vm_terminal_step
#undef snag_vm_terminal_history_rows
#ifndef _WIN32
#undef poll
#undef calloc
#undef realloc
#undef snag_vm_reader_fd

static bool polling_fixture, fail_poll_allocation;
static size_t poll_allocations, expected_poll_count;
static void *poll_storage;
static int poll_error;

static snag_wake_fd
reader_fd_fixture(const struct snag_vm_reader *reader)
{
    return polling_fixture ? SNAG_WAKE_INVALID : snag_vm_reader_fd(reader);
}

static void *
poll_calloc_fixture(size_t count, size_t size)
{
    if (!polling_fixture) return calloc(count, size);
    ++poll_allocations;
    if (fail_poll_allocation) { errno = ENOMEM; return NULL; }
    poll_storage = calloc(count, size);
    return poll_storage;
}

static void *
poll_realloc_fixture(void *storage, size_t size)
{
    if (!polling_fixture) return realloc(storage, size);
    ++poll_allocations;
    if (fail_poll_allocation) { errno = ENOMEM; return NULL; }
    poll_storage = realloc(storage, size);
    return poll_storage;
}

static int
poll_fixture(struct pollfd *fds, nfds_t count, int timeout)
{
    if (!polling_fixture) return poll(fds, count, timeout);
    assert(count == expected_poll_count && fds[0].fd == STDIN_FILENO);
    for (nfds_t i = 0u; i < count; ++i) fds[i].revents = 0;
    if (poll_error) { errno = poll_error; return -1; }
    return 0;
}

static void
input_wait_reuses_poll_storage_after_growth_and_failure(void)
{
    struct vm vm = {0};
    struct vm_launch job = {.fd = 123};
    polling_fixture = true;
    expected_poll_count = 5u;
    poll_allocations = 0u;
    fail_poll_allocation = true;
    assert(input_ready(&vm, 0) < 0 && errno == ENOMEM);
    assert(poll_allocations == 1u && !poll_storage);
    fail_poll_allocation = false;
    for (size_t i = 0u; i < 32u; ++i) assert(input_ready(&vm, 0) == 0);
    assert(poll_allocations == 2u && poll_storage);
    void *initial = poll_storage;
    vm.launches = &job;
    fail_poll_allocation = true;
    assert(input_ready(&vm, 0) < 0 && errno == ENOMEM);
    assert(poll_allocations == 3u && poll_storage == initial);
    vm.launches = NULL;
    fail_poll_allocation = false;
    assert(input_ready(&vm, 0) == 0 && poll_allocations == 3u);
    vm.launches = &job;
    expected_poll_count = 6u;
    for (size_t i = 0u; i < 32u; ++i) assert(input_ready(&vm, 0) == 0);
    assert(poll_allocations == 4u);
    vm.launches = NULL;
    expected_poll_count = 5u;
    poll_error = EINTR;
    assert(input_ready(&vm, 0) == 0);
    poll_error = EIO;
    assert(input_ready(&vm, 0) < 0 && errno == EIO);
    poll_error = 0;
    assert(input_ready(&vm, 0) == 0 && poll_allocations == 4u);
    polling_fixture = false;
    free(poll_storage);
    poll_storage = NULL;
}
#endif

static struct vm *stepping_vm;
static int step_result;

static int
terminal_step_fixture(struct snag_vm_terminal *terminal)
{
    if (!stepping_vm) return snag_vm_terminal_step(terminal);
    assert(stepping_vm->reading_input);
    assert(output_checkpoint(stepping_vm) == 0);
    return step_result;
}

static size_t
terminal_rows_fixture(const struct snag_vm_terminal *terminal)
{
    return stepping_vm ? 0u : snag_vm_terminal_history_rows(terminal);
}

static void
frontend_output_defers_input_until_parser_returns(void)
{
    struct vm vm = {0};
    struct vm_native native = {0};
    for (size_t i = 0u; i < 4u; ++i) {
        bool reading = i >= 2u;
        vm.reading_input = reading;
        step_result = i % 2u ? -1 : 1;
        stepping_vm = &vm;
        assert(native_step(&vm, &native) == step_result);
        stepping_vm = NULL;
        assert(vm.reading_input == reading);
    }
}

static json_malloc_t frame_malloc;
static size_t frame_allocations, frame_bytes;

static void *
count_frame_malloc(size_t size)
{
    ++frame_allocations;
    frame_bytes += size;
    return frame_malloc(size);
}

static void
frontend_frame_omits_unconsumed_owner_state(void)
{
    size_t counts[2], bytes[2], lengths[2];
    for (size_t pass = 0u; pass < 2u; ++pass) {
        struct snag_view_channel pair[2];
        assert(snag_view_channel_pair(pair) == 0);
        struct vm_window window = {.id = 1u};
        struct vm_native native = {.window = 1u, .channel = pair[0]};
        struct snag_vm_connection connection = {.bound = true};
        memcpy(native.session, "0123456789abcdef0123456789abcdef", sizeof(native.session));
        memcpy(connection.session, native.session, sizeof(connection.session));
        connection.state = json_pack("{s:i,s:i,s:s,s:{},s:{},s:{},s:[],s:{},"
            "s:[],s:{},s:[],s:[],s:[]}",
            "seq", 0, "end", 0, "sha256", "digest", "presentation", "frontend",
            "prompt", "skills_catalog", "audio", "destinations", "irc_names",
            "channels", "queries", "connections");
        assert(connection.state);
        size_t length = pass ? 65536u : 1u;
        char *text = malloc(length);
        assert(text);
        memset(text, 'x', length);
        assert(json_object_set_new(connection.state, "work_context",
            json_stringn(text, length)) == 0);
        free(text);
        struct vm vm = {.windows = &window, .count = 1u, .connections = &connection};
        vm.store.root_path = "/fixture";
        json_free_t release;
        json_get_alloc_funcs(&frame_malloc, &release);
        frame_allocations = frame_bytes = 0u;
        json_set_alloc_funcs(count_frame_malloc, release);
        int rc = native_broker_step(&vm, &native);
        json_set_alloc_funcs(frame_malloc, release);
        assert(rc == 0 && native.channel.output);
        counts[pass] = frame_allocations;
        bytes[pass] = frame_bytes;
        lengths[pass] = native.channel.output_len;
        json_t *frame = json_loads(native.channel.output, JSON_REJECT_DUPLICATES, NULL);
        assert(frame);
        const json_t *state = json_object_get(frame, "state");
        assert(json_is_object(state) && json_object_size(state) == 13u);
        const char *key;
        json_t *value;
        json_object_foreach((json_t *)state, key, value)
            assert(json_equal(value, json_object_get(connection.state, key)));
        json_decref(frame);
        assert(snag_view_channel_write(&native.channel) >= 0 && !native.channel.output);
        frame = NULL;
        assert(snag_view_channel_read(&pair[1], &frame) == 1 && frame);
        json_decref(frame);
        json_t *ack = json_pack("{s:s,s:I}", "type", "frame", "serial",
            (json_int_t)native.serial);
        assert(ack && snag_view_channel_send(&pair[1], ack) == 0);
        json_decref(ack);
        assert(snag_view_channel_write(&pair[1]) >= 0);
        native.authoritative = true;
        assert(native_broker_step(&vm, &native) == 0 && native.ready);
        ++connection.revision;
        frame_allocations = frame_bytes = 0u;
        json_set_alloc_funcs(count_frame_malloc, release);
        rc = native_broker_step(&vm, &native);
        json_set_alloc_funcs(frame_malloc, release);
        /* A receipt must release input readiness without republishing state. */
        assert(rc == 0 && native.channel.output);
        assert(frame_allocations < counts[pass] && frame_bytes < bytes[pass]);
        assert(native.channel.output_len < lengths[pass]);
        frame = json_loads(native.channel.output, JSON_REJECT_DUPLICATES, NULL);
        assert(frame && !strcmp(snag_json_string(frame, "type"), "ready"));
        assert(json_object_size(frame) == 1u);
        assert(!json_object_get(json_object_get(connection.state, "prompt"), "type"));
        json_decref(frame);
        assert(snag_view_channel_write(&native.channel) >= 0 && !native.channel.output);
        frame = NULL;
        assert(snag_view_channel_read(&pair[1], &frame) == 1 && frame);
        json_decref(frame);
        assert(native_broker_step(&vm, &native) == 0 && !native.channel.output);
        for (unsigned int refresh = 0u; refresh < 5u; ++refresh) {
            if (!refresh) {
                native.authoritative = false;
                native.audio_sent = native.audio_ready = native.audio_forwarded = 7u;
                ++connection.revision;
            }
            else if (refresh == 1u) {
                native.authoritative = true;
                native.revision = UINT64_MAX;
            } else if (refresh == 2u) ++connection.state_revision;
            else if (refresh == 3u) {
                native.authoritative = false;
                native.audio_sent = native.audio_ready = 7u;
                native.audio_forwarded = 0u;
                ++connection.revision;
            } else {
                ++connection.generation;
                ++connection.revision;
            }
            frame_allocations = frame_bytes = 0u;
            json_set_alloc_funcs(count_frame_malloc, release);
            rc = native_broker_step(&vm, &native);
            json_set_alloc_funcs(frame_malloc, release);
            assert(rc == 0 && native.channel.output);
            size_t frame_length = native.channel.output_len;
            assert(snag_view_channel_write(&native.channel) >= 0);
            frame = NULL;
            assert(snag_view_channel_read(&pair[1], &frame) == 1 && frame);
            assert(!strcmp(snag_json_string(frame, "type"), "frame"));
            if (!refresh) {
                /* A mirror draft refresh reuses its acknowledged owner state. */
                assert(!json_object_get(frame, "state"));
                assert(json_is_array(json_object_get(frame, "drafts")));
                assert(!json_object_get(frame, "prompt"));
                assert(json_object_size(frame) == 3u);
                assert(frame_allocations < counts[pass] && frame_bytes < bytes[pass]);
                assert(frame_length < lengths[pass]);
                assert(native.audio_sent == 7u && native.audio_ready == 7u &&
                    native.audio_forwarded == 7u);
            } else {
                assert(json_is_object(json_object_get(frame, "state")));
                assert(!native.audio_ready);
            }
            json_decref(frame);
            ack = json_pack("{s:s,s:I}", "type", "frame", "serial",
                (json_int_t)native.serial);
            assert(ack && snag_view_channel_send(&pair[1], ack) == 0);
            json_decref(ack);
            assert(snag_view_channel_write(&pair[1]) >= 0);
            assert(native_broker_step(&vm, &native) == 0 && !native.channel.output);
        }
        json_decref(connection.state);
        snag_view_channel_close(&native.channel);
        snag_view_channel_close(&pair[1]);
    }
    /* Unconsumed owner metadata must not multiply serialization or wire work. */
    assert(counts[0] == counts[1] && bytes[0] == bytes[1] && lengths[0] == lengths[1]);
}

static void
retired_frontend_paint(void)
{
    struct vm_window window = {.id = 1u, .kind = VIEW_HELP};
    struct vm_native native = {.serial = 7u, .acknowledged = 7u, .audio_ready = 1u};
    struct vm vm = {.windows = &window, .count = 1u, .native = &native};
    vm.layout = snag_vm_layout_new(window.id);
    vm.output = open("/dev/null", O_WRONLY);
    assert(vm.layout && vm.output >= 0);

    assert(draw(&vm) == 0);
    assert(native.audio_forwarded == 0u);

    close(vm.output);
    snag_vm_grid_free(&vm.grid);
    snag_vm_layout_free(vm.layout);
}

static void
captured_input_snapshot(void)
{
    struct vm_window window = {.id = 1u, .kind = VIEW_HELP};
    struct vm_native *native = calloc(1u, sizeof(*native));
    assert(native);
    snag_view_channel_init(&native->channel, -1);
    native->window = window.id;
    memcpy(native->session, "0123456789abcdef0123456789abcdef", sizeof(native->session));
    native->pending_actions = json_pack("[{s:i,s:s}]", "action", SNAG_TERM_INTERRUPT, "text", "");
    assert(native->pending_actions);
    struct vm vm = {.windows = &window, .count = 1u, .native = native};
    vm.layout = snag_vm_layout_new(window.id);
    assert(vm.layout);
    json_t *state = state_snapshot(&vm);
    assert(state && !json_object_get(state, "native_input"));
    json_decref(state);

    assert(json_array_append_new(native->pending_actions, json_pack("{s:i,s:s}",
        "action", SNAG_TERM_SUBMIT, "text", "queued 中")) == 0);
    native->pending_input.max = SIZE_MAX;
    assert(snag_buf_append(&native->pending_input, "tail\x1b", 5u) == 0);
    state = state_snapshot(&vm);
    const json_t *row = json_array_get(json_object_get(state, "native_input"), 0u);
    assert(state && row);
    struct snag_buf input = {.max = SIZE_MAX};
    assert(snag_base64_decode(&input, snag_json_string(row, "input")) == 0);
    const char expected[] = "queued 中\rtail\x1b";
    assert(input.len == sizeof(expected) - 1u && !memcmp(input.data, expected, input.len));
    snag_buf_free(&input);
    json_decref(state);
    snag_buf_reset(&native->pending_input);
    vm.count = 0u;
    native_unused(&vm);
    assert(vm.native == native);
    json_decref(native->pending_actions);
    native->pending_actions = NULL;
    native_unused(&vm);
    assert(!vm.native);
    snag_vm_layout_free(vm.layout);
}

int
main(void)
{
#ifndef _WIN32
    input_wait_reuses_poll_storage_after_growth_and_failure();
#endif
    frontend_output_defers_input_until_parser_returns();
    frontend_frame_omits_unconsumed_owner_state();
    retired_frontend_paint();
    captured_input_snapshot();
    const char *session = "0123456789abcdef0123456789abcdef";
    struct snag_vm_connection source = {0};
    memcpy(source.session, session, sizeof(source.session));
    memset(source.instance, 'a', SNAG_ID_HEX_LEN);
    source.terminal_commands = true;
    struct snag_vm_buffer rollout = {.terminal_result = true, .terminal_auto = true};
    rollout.pending = json_pack("{s:s}", "id", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    assert(rollout.pending);
    source.rollout = &rollout;
    struct vm_window window = {0};
    memcpy(window.session_id, session, sizeof(window.session_id));
    struct vm_native native = {.pending_input = {.len = 1u}};
    struct vm vm = {.windows = &window, .connections = &source, .native = &native};
    unsigned char original[sizeof(vm.classic.command)];
    memset(original, 'c', sizeof(original));
    memcpy(vm.classic.command, original, sizeof(original));

    classic_request(&vm, session);

    assert(vm.command_failed && strstr(vm.message, "Retained frontend input"));
    assert(!memcmp(vm.classic.command, original, sizeof(original)));
    assert(rollout.terminal_auto);
    assert(!vm.classic_pending && vm.native == &native);
    assert(!vm.classic.session[0]);
    native.pending_input.len = 0u;
    native.pending_actions = json_pack("[{s:i,s:s}]", "action", SNAG_TERM_SUBMIT,
        "text", "captured-before-receipt");
    assert(native.pending_actions);
    vm.command_failed = false;
    classic_request(&vm, session);
    assert(vm.command_failed && strstr(vm.message, "Retained frontend input"));
    assert(!memcmp(vm.classic.command, original, sizeof(original)));
    assert(!vm.classic_pending && vm.native == &native && rollout.terminal_auto);
    vm.command_failed = false;
    quit_sessions(&vm, true, false);
    assert(vm.command_failed && strstr(vm.message, "Retained frontend input"));
    assert(vm.native == &native && json_array_size(native.pending_actions) == 1u);
    json_decref(native.pending_actions);
    json_decref(rollout.pending);
    puts("classic refused handoff: state preserved");
    return 0;
}
