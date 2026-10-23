/* SPDX-License-Identifier: GPL-2.0-only */
#include "ui.h"
#include "irc.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

static size_t allocations, snapshots, freed_text;
static bool allocation_fails;
static _Thread_local bool input_malloc_fails;
static _Thread_local size_t input_malloc_size, input_calloc_bytes;
static _Thread_local size_t input_allocations;
static _Atomic int pending_render_error;
static _Atomic bool pending_render_exit, pending_render_seen;

static bool render_pending_fixture(const struct snag_render *);
static int render_flush_fixture(struct snag_render *, size_t);

static void *
input_malloc(size_t size)
{
    ++input_allocations;
    input_malloc_size = size;
    if (input_malloc_fails) {
        errno = ENOMEM;
        return NULL;
    }
    return malloc(size);
}

static char *
input_strdup_checked(const char *text, size_t size)
{
    ++input_allocations;
    return snag_strdup_checked(text, size);
}

static void *
input_calloc(size_t count, size_t size)
{
    ++allocations;
    input_calloc_bytes += count * size;
    if (allocation_fails) {
        errno = ENOMEM;
        return NULL;
    }
    void *result = calloc(count, size);
    /* Successful allocation need not preserve the input operation's errno. */
    if (result) errno = EDOM;
    return result;
}

static void
input_free(void *pointer)
{
    if (pointer) ++freed_text;
    free(pointer);
}

static void
input_scopes(const struct snag_irc_destinations *destinations, struct snag_irc_scopes *scopes)
{
    ++snapshots;
    snag_irc_capture_scopes(destinations, scopes);
}

/* Exercise the private presentation-owner poll without adding a product hook. */
#define calloc input_calloc
#define malloc input_malloc
#define free input_free
#define snag_strdup_checked input_strdup_checked
#define snag_render_view_pending render_pending_fixture
#define snag_render_flush_pending render_flush_fixture
#define snag_irc_capture_scopes input_scopes
#include "../src/ui.c"
#undef snag_irc_capture_scopes
#undef free
#undef snag_strdup_checked
#undef malloc
#undef calloc
#undef snag_render_view_pending
#undef snag_render_flush_pending

static bool
render_pending_fixture(const struct snag_render *render)
{
    return atomic_load(&pending_render_error) || snag_render_view_pending(render);
}

static int
render_flush_fixture(struct snag_render *render, size_t records)
{
    int error = atomic_exchange(&pending_render_error, 0);
    if (!error) return snag_render_flush_pending(render, records);
    struct snag_ui_display *display = render->checkpoint_opaque;
    atomic_store(&display->runtime->exit_requested, atomic_load(&pending_render_exit));
    atomic_store(&pending_render_seen, true);
    return snag_errno(error);
}

static void
test_pending_render_cancellation_preserves_orderly_exit(void)
{
    const int errors[] = {ECANCELED, ECANCELED, EIO};
    for (size_t i = 0u; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        struct snag_ui ui;
        bool exiting = i != 1u;
        atomic_store(&pending_render_error, errors[i]);
        atomic_store(&pending_render_exit, exiting);
        atomic_store(&pending_render_seen, false);
        assert(snag_ui_init(&ui) == 0);
        /* Two acknowledged messages bracket the worker's pending-render pass. */
        assert(snag_ui_send(&ui, (struct snag_ui_command){.kind = SNAG_UI_SPINNERS}) == 0);
        assert(snag_ui_send(&ui, (struct snag_ui_command){.kind = SNAG_UI_SPINNERS}) == 0);
        assert(atomic_load(&pending_render_seen));
        bool orderly = exiting && errors[i] == ECANCELED;
        assert(atomic_load(&ui.runtime->fatal) == (orderly ? 0 : errors[i]));
        enum snag_term_action action;
        char *text = NULL;
        int rc = snag_ui_poll(&ui, 0, &action, &text);
        if (orderly) {
            assert(rc == 1 && action == SNAG_TERM_EXIT && !text);
        } else {
            assert(rc < 0 && errno == errors[i] && !text);
        }
        atomic_store(&ui.runtime->fatal, 0);
        snag_ui_free(&ui);
    }
}

static void
test_ui_initialization_does_not_reserve_owner_transports(void)
{
    struct snag_ui ui;
    allocation_fails = input_malloc_fails = false;
    input_calloc_bytes = 0u;
    assert(snag_ui_init(&ui) == 0);
    (void)fprintf(stderr, "UI initialization: %zu bytes; relay: %zu; channel: %zu\n",
        input_calloc_bytes, sizeof(struct snag_session_relay), sizeof(struct snag_view_channel));
    /* The ordinary frontend needs neither owner transport. Its complete
     * constructor should cost less than the native relay alone. */
    assert(input_calloc_bytes < sizeof(struct snag_session_relay));
    snag_ui_free(&ui);
}

static void
test_queued_prompt_keeps_the_presenter_view(void)
{
    for (unsigned int frontend = 0u; frontend < 2u; ++frontend) {
        for (unsigned int chat = 0u; chat < 2u; ++chat) {
            for (unsigned int active = 0u; active < 2u; ++active) {
                struct snag_ui_runtime runtime = {0};
                struct snag_ui_display display = {.runtime = &runtime, .frontend = frontend};
                snag_term_init(&display.term);
                snag_render_init(&display.render, 0u);
                display.term.input_only = true;
                display.term.chat = chat;
                display.render.view = chat ? SNAG_RENDER_CHAT : SNAG_RENDER_ROLLOUT;
                for (unsigned int attempt = 0u; attempt < 2u; ++attempt) {
                    struct ui_message message = {.command.kind = SNAG_UI_PROMPT};
                    struct snag_term_prompt *prompt = &message.command.data.prompt;
                    static const char format[] =
                        "{chat:chat}{rollout-idle:idle}{rollout-active:active}";
                    char *storage = malloc(sizeof(format) + sizeof("context"));
                    assert(storage);
                    memcpy(storage, format, sizeof(format));
                    memcpy(storage + sizeof(format), "context", sizeof("context"));
                    prompt->source = storage;
                    prompt->values[0] = storage + sizeof(format);
                    prompt->active = active;
                    prompt->rate = attempt + 1u;
                    prompt->states = attempt;
                    /* The engine queued this before the presenter consumed Tab. */
                    prompt->mode = chat ? 1u : 0u;
                    for (size_t i = 0u; i < SNAG_TERM_SPINNER_COUNT; ++i) {
                        assert(snag_strcpy(prompt->frames[i], sizeof(prompt->frames[i]), " "));
                    }
                    assert(apply_display(&display, &message) == 0);
                    assert(message.prompt_changed == !attempt);
                    assert(!strcmp(display.term.label,
                        chat ? "chat " : active ? "active " : "idle "));
                    assert(display.prompt.mode == (chat ? 0u : active ? 2u : 1u));
                }
                prompt_free(&display.prompt);
                snag_term_close(&display.term);
                snag_render_free(&display.render);
            }
        }
    }
}

static void
test_prompt_fields_share_owned_storage(void)
{
    struct snag_ui ui;
    assert(snag_ui_init(&ui) == 0);
    char format[] =
        "prefix {model}{chat: suffix}{rollout-idle: suffix}{rollout-active: suffix}";
    char model[] = "owned-model";
    const char *values[SNAG_PROMPT_HOUR];
    const char *spinners[SNAG_TERM_SPINNER_COUNT] = {" ", " ", " "};
    for (size_t i = 0u; i < SNAG_PROMPT_HOUR; ++i) values[i] = "";
    values[SNAG_PROMPT_MODEL] = model;
    input_allocations = 0u;
    assert(snag_ui_composer(&ui, false, format, values, 1u, spinners, 1u, 0u, NULL) == 0);
    assert(input_allocations == 1u);
    assert(strstr(ui.label, "prefix owned-model suffix"));
    memset(format, 'x', sizeof(format) - 1u);
    memset(model, 'y', sizeof(model) - 1u);
    assert(snag_ui_send(&ui, (struct snag_ui_command){.kind = SNAG_UI_SPINNERS}) == 0);
    assert(strstr(ui.label, "prefix owned-model suffix"));
    input_malloc_fails = true;
    assert(snag_ui_composer(&ui, false, "replacement", values, 1u, spinners, 1u, 0u, NULL) < 0);
    assert(errno == ENOMEM);
    input_malloc_fails = false;
    char oversized[SNAG_TERM_LABEL_BYTES + 2u];
    memset(oversized, 'z', sizeof(oversized) - 1u);
    oversized[sizeof(oversized) - 1u] = '\0';
    values[SNAG_PROMPT_MODEL] = oversized;
    assert(snag_ui_composer(&ui, false, "replacement", values, 1u, spinners, 1u, 0u, NULL) < 0);
    assert(errno == EOVERFLOW);
    assert(snag_ui_send(&ui, (struct snag_ui_command){.kind = SNAG_UI_SPINNERS}) == 0);
    assert(strstr(ui.label, "prefix owned-model suffix"));
    snag_ui_free(&ui);
}

static void
test_owner_relay_failure_preserves_caller_ownership(void)
{
    struct snag_ui_runtime runtime = {0};
    struct snag_ui_display display = {.runtime = &runtime};
    struct snag_session_process process = {.master = -1, .slave = -1, .peer = -1};
    struct snag_ui_command start = {.kind = SNAG_UI_SESSION_START,
        .data.session_process = &process};
    size_t before = freed_text;
    input_malloc_fails = true;
    assert(apply_session(&display, &start) < 0 && errno == ENOMEM);
    input_malloc_fails = false;
    assert(!display.native && freed_text == before);
    for (unsigned int i = 0u; i < 2u; ++i) {
        assert(apply_session(&display, &start) < 0 && errno == EBADF);
        assert(input_malloc_size == sizeof(struct snag_session_relay));
        assert(!display.native && freed_text == before + i + 1u);
        assert(process.master == -1 && process.slave == -1 && process.peer == -1);
    }
}

#if SNAJPAGENT_VM
static void
test_direct_channel_allocation_preserves_transfer_ownership(void)
{
    struct snag_ui_runtime runtime = {0};
    struct snag_ui_display display = {.runtime = &runtime};
    struct snag_view_channel pair[2];
    assert(snag_view_channel_pair(pair) == 0);
    struct snag_view_local *local = pair[0].local;
    struct snag_ui_command start = {.kind = SNAG_UI_SESSION_DIRECT,
        .data.view_channel = &pair[0]};
    input_malloc_fails = true;
    assert(apply_session(&display, &start) < 0 && errno == ENOMEM);
    input_malloc_fails = false;
    assert(!display.direct && pair[0].local == local);
    struct snag_ui ui;
    assert(snag_ui_init(&ui) == 0);
    assert(snag_ui_send(&ui, start) == 0);
    assert(!pair[0].local);
    assert(snag_ui_send(&ui, start) < 0 && errno == EALREADY);
    snag_ui_free(&ui);
    json_t *message = NULL;
    assert(snag_view_channel_read(&pair[1], &message) < 0 && errno == ECONNRESET);
    assert(!message);
    snag_view_channel_close(&pair[1]);
}

static json_t *
shutdown_exchange(struct snag_view_server *server, struct snag_view_channel *peer,
    json_t *request)
{
    assert(request && snag_view_channel_send(peer, request) == 0);
    json_decref(request);
    uint64_t deadline = snag_monotonic_ms() + 5000u;
    while (snag_monotonic_ms() < deadline) {
        if (peer->output) assert(snag_view_channel_write(peer) >= 0);
        snag_view_server_step(server);
        json_t *reply = NULL;
        int rc = snag_view_channel_read(peer, &reply);
        assert(rc >= 0);
        if (rc) return reply;
        assert(snag_sleep_ms(1u) == 0);
    }
    assert(!"shutdown reply timed out");
    return NULL;
}

static void
test_editor_close_stops_drafts_before_releasing_storage(void)
{
    for (unsigned int queued = 0u; queued < 2u; ++queued) {
        struct snag_ui_runtime runtime = {0};
        runtime.actions.wake[0] = runtime.actions.wake[1] = SNAG_WAKE_INVALID;
        struct snag_ui_display display = {.runtime = &runtime, .direct = true};
        snag_term_init(&display.term);
        snag_render_init(&display.render, 0u);
        display.term.input_only = true;
        display.main_draft = &display.rollout_draft;
        struct snag_view_channel pair[2];
        assert(snag_view_channel_pair(pair) == 0);
        struct snag_view_callbacks callbacks = {.bound = view_bound, .submit = view_submit,
            .control = view_control, .opaque = &display, .draft = view_draft};
        display.view = snag_view_server_direct(&pair[1],
            "11111111111111111111111111111111", callbacks);
        assert(display.view);
        json_t *reply = shutdown_exchange(display.view, &pair[0],
            json_pack("{s:s,s:i}", "type", "hello", "version", 1));
        assert(!strcmp(snag_json_string(reply, "type"), "capabilities"));
        json_decref(reply);
        reply = shutdown_exchange(display.view, &pair[0],
            json_pack("{s:s}", "type", "reserve"));
        uint64_t generation = (uint64_t)json_integer_value(json_object_get(reply, "generation"));
        assert(generation);
        json_decref(reply);
        reply = shutdown_exchange(display.view, &pair[0], json_pack("{s:s,s:I}",
            "type", "commit", "generation", (json_int_t)generation));
        assert(!strcmp(snag_json_string(reply, "type"), "bound"));
        json_decref(reply);
        json_t *edit = json_pack("{s:s,s:I,s:s,s:i,s:i,s:s,s:i}", "type", "draft",
            "generation", (json_int_t)generation, "route", "rollout", "revision", 1,
            "edit", 1, "text", "late unsent draft", "cursor", 17);
        assert(edit);
        if (queued) {
            assert(snag_view_channel_send(&pair[0], edit) == 0);
            assert(snag_view_channel_write(&pair[0]) == 1);
        }
        struct ui_message close = {.command.kind = SNAG_UI_CLOSE};
        assert(apply_display(&display, &close) == 0);
        if (!queued) {
            assert(snag_view_channel_send(&pair[0], edit) == 0);
            assert(snag_view_channel_write(&pair[0]) == 1);
        }
        json_decref(edit);
        /* Service the close/EXIT gap. A late edit must not call the dead editor
         * or send a refusal that disconnects its controller before EXIT. */
        snag_view_server_step(display.view);
        snag_view_server_exit(display.view, 0u);
        reply = shutdown_exchange(display.view, &pair[0],
            json_pack("{s:s,s:I}", "type", "cancel", "generation", (json_int_t)generation));
        assert(!strcmp(snag_json_string(reply, "type"), "exit"));
        json_decref(reply);
        snag_view_channel_close(&pair[0]);
        snag_view_server_close(display.view);
        snag_render_free(&display.render);
    }
}
#endif

static void
test_input_storage_follows_actual_input(void)
{
    struct snag_ui ui;
    assert(snag_ui_init(&ui) == 0);
    struct ui_input *input = &ui.runtime->input;
    assert(!input->bytes);
    unsigned char bytes[4096];
    assert(input_read(input, bytes, sizeof(bytes)) == 0);
    input_enqueue(ui.runtime, bytes, 0u);
    assert(!input->bytes);
    input_enqueue(ui.runtime, (const unsigned char *)"abc", 3u);
    assert(input->bytes && input->len == 3u);
    assert(input_malloc_size <= sizeof(bytes));
    size_t capacity = input_malloc_size;
    unsigned char *storage = input->bytes;
    assert(input_read(input, bytes, sizeof(bytes)) == 3 && !memcmp(bytes, "abc", 3u));

    memset(storage, 'x', capacity);
    input->head = capacity - 2u;
    input->len = 2u;
    input_enqueue(ui.runtime, (const unsigned char *)"ab", 2u);
    assert(input_read(input, bytes, sizeof(bytes)) == 4 && !memcmp(bytes, "xxab", 4u));
    input->head = capacity - 2u;
    input->len = 2u;
    input_enqueue(ui.runtime, (const unsigned char *)"\003z", 2u);
    assert(input_take_held_control(input, false) == HELD_INTERRUPT);
    assert(input_read(input, bytes, sizeof(bytes)) == 1 && bytes[0] == 'z');

    /* Failed growth leaves wrapped typeahead intact and retryable. */
    input->head = capacity - 2u;
    input->len = 2u;
    memset(bytes, 'y', sizeof(bytes));
    input_malloc_fails = true;
    input_enqueue(ui.runtime, bytes, sizeof(bytes));
    input_malloc_fails = false;
    assert(input->bytes == storage && input->head == capacity - 2u && input->len == 2u);
    assert(atomic_load(&ui.runtime->fatal) == ENOMEM);
    atomic_store(&ui.runtime->fatal, 0);
    input_enqueue(ui.runtime, bytes, sizeof(bytes));
    assert(input_malloc_size == 2u * capacity && input->len == sizeof(bytes) + 2u);
    assert(input_read(input, bytes, sizeof(bytes)) == sizeof(bytes));
    assert(bytes[0] == 'x' && bytes[1] == 'x');
    for (size_t i = 2u; i < sizeof(bytes); ++i) assert(bytes[i] == 'y');
    assert(input_read(input, bytes, sizeof(bytes)) == 2 && bytes[0] == 'y' && bytes[1] == 'y');

    /* Keep the full paste capacity and reuse it after draining. */
    unsigned char *paste = malloc(UI_INPUT_CAPACITY);
    assert(paste);
    memset(paste, 'x', UI_INPUT_CAPACITY);
    input_enqueue(ui.runtime, paste, UI_INPUT_CAPACITY);
    free(paste);
    assert(input_malloc_size == UI_INPUT_CAPACITY && input->len == UI_INPUT_CAPACITY);
    storage = input->bytes;
    size_t total = 0u;
    input_malloc_fails = true;
    while (input->len) {
        ssize_t count = input_read(input, bytes, sizeof(bytes));
        assert(count > 0);
        for (size_t i = 0u; i < (size_t)count; ++i) assert(bytes[i] == 'x');
        total += (size_t)count;
    }
    input_malloc_fails = false;
    assert(total == UI_INPUT_CAPACITY && input->bytes == storage);
    input->len = UI_INPUT_CAPACITY;
    input_enqueue(ui.runtime, (const unsigned char *)"z", 1u);
    assert(input_read(input, bytes, sizeof(bytes)) < 0 && errno == EOVERFLOW);
    assert(!input->len && !input->overflow && input->bytes == storage);
    snag_ui_free(&ui);

    assert(snag_ui_init(&ui) == 0);
    input_malloc_fails = true;
    input_enqueue(ui.runtime, (const unsigned char *)"x", 1u);
    input_malloc_fails = false;
    assert(!ui.runtime->input.bytes && !ui.runtime->input.len);
    assert(atomic_load(&ui.runtime->fatal) == ENOMEM);
    snag_ui_free(&ui);
}

static void
test_frontend_keeps_only_the_consumed_state_catalog(void)
{
    assert(snag_workspace_search(NULL, 0u) == NULL);
    assert(!strcmp(snag_workspace_search(" /search\tprivate\nsecond", 23u), "private\nsecond"));
    assert(!*snag_workspace_search("/search", 7u));
    assert(!snag_workspace_search("//search private", 16u));
    assert(!snag_workspace_search("/searching private", 18u));
    assert(!snag_workspace_search("/search private; /status", 24u));
    assert(!strcmp(snag_workspace_search("/search 'a;b'", 13u), "'a;b'"));
    struct snag_ui_display display = {.frontend = true};
    snag_term_init(&display.term);
    json_t *state = json_pack("{s:[{s:s}],s:{s:s}}", "skills_catalog", "name", "first",
        "unconsumed", "body", "owner state must not be copied or republished");
    assert(state);
    json_t *catalog = json_object_get(state, "skills_catalog");
    assert(view_state(&display, state) == 0);
    assert(!display.view_state && display.term.skills_catalog == catalog);
    json_decref(state);
    assert(!strcmp(snag_json_string(json_array_get(display.term.skills_catalog, 0u),
        "name"), "first"));
    /* Reapplying a catalog must retain it before releasing the previous reference. */
    state = json_object();
    assert(state && json_object_set(state, "skills_catalog", catalog) == 0);
    assert(view_state(&display, state) == 0 && display.term.skills_catalog == catalog);
    json_decref(state);
    state = json_pack("{s:[{s:s}]}", "skills_catalog", "name", "replacement");
    assert(state && view_state(&display, state) == 0);
    json_decref(state);
    assert(!strcmp(snag_json_string(json_array_get(display.term.skills_catalog, 0u),
        "name"), "replacement"));
    state = json_object();
    assert(state && view_state(&display, state) == 0);
    assert(!display.view_state && !display.term.skills_catalog);
    json_decref(state);
    snag_term_close(&display.term);
}

static json_malloc_t state_malloc;
static size_t state_allocations, state_bytes;
static bool state_malloc_fails;

static void *
count_state_malloc(size_t size)
{
    ++state_allocations;
    state_bytes += size;
    if (state_malloc_fails) {
        errno = ENOMEM;
        return NULL;
    }
    return state_malloc(size);
}

static void
test_prompt_snapshot_reuses_only_unchanged_arrays(void)
{
    struct snag_ui_display display = {0};
    snag_term_init(&display.term);
    display.term.prompt_clock = (struct snag_prompt_clock){
        .captured = true, .valid = true, .hour = 1, .minute = 2, .second = 3};
    display.prompt.source = "{model} {hour}";
    for (size_t i = 0u; i < SNAG_PROMPT_HOUR; ++i) display.prompt.values[i] = "field";
    json_free_t release;
    json_get_alloc_funcs(&state_malloc, &release);
    state_allocations = state_bytes = 0u;
    json_set_alloc_funcs(count_state_malloc, release);
    json_t *first = view_prompt(&display);
    json_set_alloc_funcs(state_malloc, release);
    size_t first_allocations = state_allocations, first_bytes = state_bytes;
    assert(first);
    display.view_state = json_pack("{s:o}", "prompt", first);
    assert(display.view_state);
    state_allocations = state_bytes = 0u;
    json_set_alloc_funcs(count_state_malloc, release);
    json_t *same = view_prompt(&display);
    json_set_alloc_funcs(state_malloc, release);
    assert(state_allocations < first_allocations && state_bytes < first_bytes);
    assert(same && json_equal(first, same));
    const char *fields[] = {"values", "frames", "raw_values"};
    for (size_t i = 0u; i < sizeof(fields) / sizeof(fields[0]); ++i)
        assert(json_object_get(same, fields[i]) == json_object_get(first, fields[i]));
    json_decref(same);

    display.prompt.values[0] = "changed";
    json_t *changed = view_prompt(&display);
    assert(changed && !strcmp(json_string_value(json_array_get(
        json_object_get(changed, "raw_values"), 0u)), "changed"));
    assert(json_object_get(changed, "raw_values") != json_object_get(first, "raw_values"));
    assert(json_object_get(changed, "values") != json_object_get(first, "values"));
    assert(json_object_get(changed, "frames") == json_object_get(first, "frames"));
    json_decref(changed);
    display.prompt.values[0] = "field";

    display.term.destinations = calloc(1u, sizeof(*display.term.destinations));
    assert(display.term.destinations);
    display.term.destinations->count = 1u;
    struct snag_irc_destination *route = &display.term.destinations->items[0];
    route->target.id = display.term.destination.id = 7u;
    assert(snag_strcpy(route->operator, sizeof(route->operator), "alice"));
    assert(snag_strcpy(route->model, sizeof(route->model), "agent"));
    changed = view_prompt(&display);
    assert(changed && !strcmp(json_string_value(json_array_get(
        json_object_get(changed, "values"), SNAG_PROMPT_OPERATOR)), "alice"));
    assert(!strcmp(json_string_value(json_array_get(
        json_object_get(changed, "values"), SNAG_PROMPT_MODEL_NICK)), "agent"));
    assert(json_object_get(changed, "values") != json_object_get(first, "values"));
    assert(json_object_get(changed, "raw_values") == json_object_get(first, "raw_values"));
    assert(json_object_get(changed, "frames") == json_object_get(first, "frames"));
    json_decref(changed);
    free(display.term.destinations);
    display.term.destinations = NULL;
    display.term.destination.id = 0u;

    ++display.term.prompt_clock.hour;
    changed = view_prompt(&display);
    assert(changed && !strcmp(json_string_value(json_array_get(
        json_object_get(changed, "values"), SNAG_PROMPT_HOUR)), "2"));
    assert(json_object_get(changed, "values") != json_object_get(first, "values"));
    assert(json_object_get(changed, "raw_values") == json_object_get(first, "raw_values"));
    json_decref(changed);
    --display.term.prompt_clock.hour;

    display.prompt.frames[0][0] = '*';
    display.prompt.mode = 2u;
    changed = view_prompt(&display);
    assert(changed && !strcmp(json_string_value(json_array_get(
        json_object_get(changed, "frames"), 0u)), "*"));
    assert(!strcmp(json_string_value(json_array_get(
        json_object_get(changed, "values"), SNAG_PROMPT_MODE)), "rollout-active"));
    assert(json_object_get(changed, "frames") != json_object_get(first, "frames"));
    assert(json_object_get(changed, "values") != json_object_get(first, "values"));
    assert(json_object_get(changed, "raw_values") == json_object_get(first, "raw_values"));
    assert(json_object_set_new(display.view_state, "prompt", changed) == 0);
    same = view_prompt(&display);
    assert(same && json_equal(changed, same));
    for (size_t i = 0u; i < sizeof(fields) / sizeof(fields[0]); ++i)
        assert(json_object_get(same, fields[i]) == json_object_get(changed, fields[i]));
    json_decref(same);

    display.prompt.values[0] = "retry after allocation failure";
    state_malloc_fails = true;
    json_set_alloc_funcs(count_state_malloc, release);
    json_t *failed = view_prompt(&display);
    json_set_alloc_funcs(state_malloc, release);
    state_malloc_fails = false;
    assert(!failed && json_object_get(display.view_state, "prompt") == changed);
    same = view_prompt(&display);
    assert(same && !strcmp(json_string_value(json_array_get(
        json_object_get(same, "raw_values"), 0u)), display.prompt.values[0]));
    assert(!strcmp(json_string_value(json_array_get(
        json_object_get(changed, "raw_values"), 0u)), "field"));
    json_decref(same);
    json_decref(display.view_state);
    snag_term_close(&display.term);
}

static void
test_audio_updates_preserve_unrelated_display_state(void)
{
    struct snag_ui_display display = {0};
    snag_term_init(&display.term);
    display.term.input_only = true;
    json_t *state = json_pack("{s:[],s:{s:s}}", "skills_catalog",
        "work_context", "directory", "original");
    assert(state && view_state(&display, state) == 0);
    json_decref(state);
    json_free_t release;
    json_get_alloc_funcs(&state_malloc, &release);
    const char *unchanged[] = {"skills_catalog", "work_context", "destinations", "tabs",
        "prompt", "frontend", "presentation"};
    for (unsigned int pass = 0u; pass < 2u; ++pass) {
        json_t *previous = json_incref(display.view_state);
        struct snag_ui_command command = {.kind = pass ? SNAG_UI_CAPTION : SNAG_UI_AUDIO,
            .text = pass ? "caption text" : "", .data.value = pass};
        display.audio_epoch = 7u + pass;
        state_allocations = state_bytes = 0u;
        json_set_alloc_funcs(count_state_malloc, release);
        char error[128];
        int rc = apply_message(&display, &command, error, sizeof(error));
        json_set_alloc_funcs(state_malloc, release);
        assert(rc == 0 && state_allocations <= 32u);
        for (size_t i = 0u; i < sizeof(unchanged) / sizeof(unchanged[0]); ++i)
            assert(json_object_get(display.view_state, unchanged[i]) ==
                json_object_get(previous, unchanged[i]));
        const json_t *audio = json_object_get(display.view_state, "audio");
        assert(json_integer_value(json_object_get(audio, "epoch")) == 7u + pass);
        assert(!strcmp(snag_json_string(audio, "label"), display.term.audio_label));
        assert(json_is_false(json_object_get(audio, "dictating")));
        assert(!strcmp(json_string_value(json_array_get(
            json_object_get(audio, "captions"), 1u)), pass ? "caption text" : ""));
        assert(json_integer_value(json_object_get(json_object_get(previous, "audio"),
            "epoch")) == (pass ? 7 : 0));
        json_decref(previous);
    }
    json_t *previous = display.view_state;
    state_malloc_fails = true;
    json_set_alloc_funcs(count_state_malloc, release);
    char error[128];
    struct snag_ui_command command = {.kind = SNAG_UI_CAPTION, .text = "retry caption"};
    int rc = apply_message(&display, &command, error, sizeof(error));
    json_set_alloc_funcs(state_malloc, release);
    state_malloc_fails = false;
    assert(rc < 0 && display.view_state == previous);
    assert(apply_message(&display, &command, error, sizeof(error)) == 0);
    assert(!strcmp(json_string_value(json_array_get(json_object_get(
        json_object_get(display.view_state, "audio"), "captions"), 0u)), "retry caption"));
    json_decref(display.view_state);
    json_decref(display.term.skills_catalog);
    display.term.skills_catalog = NULL;
    snag_term_close(&display.term);
}

static void
test_semantic_channel_uses_bounded_span_encoding(void)
{
#if SNAJPAGENT_VM
    struct snag_view_channel pair[2];
    assert(snag_view_channel_pair(pair) == 0);
    json_t *value = json_pack("{s:s,s:[i,f,f,b,n]}", "text", "café 🐱\n\"\\",
        "scalars", 1, 1.0, -0.0, 1);
    assert(value);
    json_free_t release;
    json_get_alloc_funcs(&state_malloc, &release);
    state_allocations = state_bytes = 0u;
    state_malloc_fails = true;
    json_set_alloc_funcs(count_state_malloc, release);
    int rc = snag_view_channel_send(&pair[0], value);
    json_set_alloc_funcs(state_malloc, release);
    state_malloc_fails = false;
    assert(rc == 0 && state_allocations == 0u);
    assert(snag_view_channel_send(&pair[0], value) < 0 && errno == EAGAIN);
    assert(snag_view_channel_write(&pair[0]) == 1);
    json_t *received = NULL;
    assert(snag_view_channel_read(&pair[1], &received) == 1);
    assert(json_equal(value, received));
    const json_t *scalars = json_object_get(received, "scalars");
    assert(json_is_integer(json_array_get(scalars, 0u)));
    assert(json_is_real(json_array_get(scalars, 1u)));
    assert(json_is_real(json_array_get(scalars, 2u)));
    json_decref(received);

    char *large = malloc(SNAG_VIEW_MESSAGE_MAX);
    assert(large);
    memset(large, 'x', SNAG_VIEW_MESSAGE_MAX);
    json_t *oversized = json_pack("{s:s%}", "text", large, (size_t)SNAG_VIEW_MESSAGE_MAX);
    free(large);
    assert(oversized);
    assert(snag_view_channel_send(&pair[0], oversized) < 0 && errno == EOVERFLOW);
    assert(!pair[0].output && !pair[0].write_deadline);
    json_decref(oversized);
    json_t *nul = json_pack("{s:s%}", "text", "a\0b", (size_t)3u);
    assert(nul && snag_view_channel_send(&pair[0], nul) < 0 && errno == EILSEQ);
    assert(!pair[0].output && !pair[0].write_deadline);
    json_decref(nul);
    assert(snag_view_channel_send(&pair[0], value) == 0);
    assert(snag_view_channel_write(&pair[0]) == 1);
    assert(snag_view_channel_read(&pair[1], &received) == 1);
    assert(json_equal(value, received));
    json_decref(received);
    json_decref(value);
    snag_view_channel_close(&pair[0]);
    snag_view_channel_close(&pair[1]);
#endif /* SNAJPAGENT_VM */
}

static void
test_display_refresh_reuses_its_immutable_session_snapshot(void)
{
    size_t counts[2], bytes[2];
    for (size_t pass = 0u; pass < 2u; ++pass) {
        struct snag_ui_display display = {0};
        snag_term_init(&display.term);
        display.term.input_only = true;
        json_t *state = json_pack("{s:[],s:{s:s}}", "skills_catalog",
            "work_context", "directory", "original");
        assert(state);
        json_t *catalog = json_object_get(state, "skills_catalog");
        for (size_t i = 0u; i < (pass ? 128u : 1u); ++i)
            assert(json_array_append_new(catalog,
                json_pack("{s:s}", "name", "retained skill")) == 0);
        assert(view_state(&display, state) == 0);
        json_t *previous = json_incref(display.view_state);
        /* Engine-owned input remains mutable; the display must first isolate it. */
        assert(json_object_set_new(json_object_get(state, "work_context"),
            "directory", json_string("changed")) == 0);
        assert(json_array_append_new(catalog, json_string("later skill")) == 0);
        assert(!strcmp(snag_json_string(json_object_get(previous, "work_context"),
            "directory"), "original"));
        assert(json_array_size(json_object_get(previous, "skills_catalog")) ==
            (pass ? 128u : 1u));
        json_decref(state);

        json_free_t release;
        json_get_alloc_funcs(&state_malloc, &release);
        state_allocations = state_bytes = 0u;
        display.color = SNAG_COLOR_ALWAYS;
        json_set_alloc_funcs(count_state_malloc, release);
        int rc = view_state(&display, display.view_state);
        json_set_alloc_funcs(state_malloc, release);
        assert(rc == 0);
        counts[pass] = state_allocations;
        bytes[pass] = state_bytes;
        /* Replacing display fields must not mutate an earlier published root. */
        assert(display.view_state != previous);
        assert(json_integer_value(json_object_get(json_object_get(previous,
            "frontend"), "color")) == SNAG_COLOR_AUTO);
        assert(json_integer_value(json_object_get(json_object_get(display.view_state,
            "frontend"), "color")) == SNAG_COLOR_ALWAYS);
        json_t *published = display.view_state;
        display.color = SNAG_COLOR_NEVER;
        state_malloc_fails = true;
        json_set_alloc_funcs(count_state_malloc, release);
        rc = view_state(&display, display.view_state);
        json_set_alloc_funcs(state_malloc, release);
        state_malloc_fails = false;
        assert(rc < 0 && display.view_state == published);
        assert(json_integer_value(json_object_get(json_object_get(published,
            "frontend"), "color")) == SNAG_COLOR_ALWAYS);
        json_decref(previous);
        json_decref(display.view_state);
        snag_term_close(&display.term);
    }
    /* Refreshing presentation fields must not recopy the unchanged catalog. */
    fprintf(stderr, "display refresh: %zu/%zu allocations, %zu/%zu bytes\n",
        counts[0], counts[1], bytes[0], bytes[1]);
    assert(counts[0] == counts[1] && bytes[0] == bytes[1]);
}

static void
test_restore_inactive_draft_clears_its_history(void)
{
    struct snag_ui_display display = {0};
    snag_term_init(&display.term);
    display.term.input_only = true;
    struct ui_channel_draft channel = {.destination = 7u};
    struct ui_conversation_tab tab = {0};
    assert(snag_strcpy(tab.target.conversation, sizeof(tab.target.conversation),
        "11111111111111111111111111111111"));
    display.channel_drafts = &channel;
    display.conversations = &tab;
    struct snag_buf *drafts[] = {&display.rollout_draft.text, &channel.text, &tab.draft};
    struct snag_edit_history *histories[] = {&display.rollout_draft.edits,
        &channel.edits, &tab.edits};
    size_t *cursors[] = {&display.rollout_draft.cursor, &channel.cursor, &tab.cursor};
    for (size_t i = 0u; i < 3u; ++i) {
        struct snag_buf *draft = drafts[i];
        snag_buf_init(draft, SNAG_MAX_DIRECT_PROMPT + 1u);
        assert(snag_edit_begin(histories[i], draft, 0u) == 0);
        assert(snag_buf_append(draft, "old", 3u) == 0);
        assert(snag_edit_end(histories[i], draft, 3u) == 0 && histories[i]->undo);
        /* Adopt equal bytes too: they belong to a new input lifetime. */
        struct snag_ui_command command = {.text = "old", .len = 3u};
        command.data.draft.view = i == 0u ? SNAG_RENDER_ROLLOUT : SNAG_RENDER_CHAT;
        command.data.draft.destination = 7u;
        command.data.draft.conversation = i == 2u ? tab.target.conversation : "";
        assert(display_restore_input(&display, &command) == 0);
        assert(!histories[i]->undo && !histories[i]->redo && !histories[i]->grouping);
        assert(snag_edit_undo(histories[i], draft, cursors[i], false, 1u) == 0);
        assert(*cursors[i] == 3u && draft->len == 3u && !memcmp(draft->data, "old", 3u));
        snag_buf_free(draft);
        snag_edit_history_free(histories[i]);
    }
    snag_term_close(&display.term);
}

static void
test_first_exact_room_route_retains_draft_history(void)
{
    struct snag_ui_display display = {0};
    snag_term_init(&display.term);
    display.term.input_only = true;
    struct snag_irc_destinations destinations = {.count = 1u};
    struct snag_irc_destination *destination = &destinations.items[0];
    destination->target.id = 7u;
    assert(snag_strcpy(destination->connection, sizeof(destination->connection),
        "11111111111111111111111111111111"));
    assert(snag_strcpy(destination->room, sizeof(destination->room), "#room"));
    display.term.destinations = &destinations;
    display.main_draft = &display.rollout_draft;
    struct ui_channel_draft *channel = display_channel_draft(&display, 7u);
    assert(channel && snag_edit_begin(&channel->edits, &channel->text, 0u) == 0);
    assert(snag_buf_append(&channel->text, "draft é", strlen("draft é")) == 0);
    channel->cursor = channel->text.len;
    assert(snag_edit_end(&channel->edits, &channel->text, channel->cursor) == 0);
    struct snag_irc_event event = {.routed = true};
    event.route.kind = SNAG_IRC_CHANNEL;
    event.route.identity = SNAG_IRC_OPERATOR;
    assert(snag_strcpy(event.route.conversation, sizeof(event.route.conversation),
        "22222222222222222222222222222222"));
    assert(snag_strcpy(event.route.connection, sizeof(event.route.connection),
        destination->connection));
    assert(snag_strcpy(event.route.membership, sizeof(event.route.membership),
        "33333333333333333333333333333333"));
    assert(snag_strcpy(event.room, sizeof(event.room), "#room"));
    assert(display_conversation_event(&display, &event) == 0);
    struct ui_conversation_tab *tab = display.conversations;
    assert(tab && tab->draft.len == strlen("draft é") && tab->edits.undo);
    assert(!channel->text.len && !channel->edits.undo && !channel->cursor);
    assert(snag_edit_undo(&tab->edits, &tab->draft, &tab->cursor, false, 1u) == 0);
    assert(!tab->draft.len && !tab->cursor);
    snag_edit_history_free(&tab->edits);
    snag_buf_free(&tab->draft);
    free(tab);
    snag_edit_history_free(&channel->edits);
    snag_buf_free(&channel->text);
    free(channel);
    display.term.destinations = NULL;
    snag_term_close(&display.term);
}

static void
test_frontend_capture_preserves_the_actual_action(void)
{
#if SNAJPAGENT_VM
    struct snag_ui input = {.input_received_ms = 1234u, .input_turn_generation = 17u,
        .input_view = SNAG_RENDER_ROLLOUT, .input_verbosity = 4, .input_active = true,
        .input_view_applied = true, .selection = {.id = 7u, .revision = 29u}};
    (void)snag_strcpy(input.submitted_label, sizeof(input.submitted_label), "captured > ");
    input.input_route.count = 1u;
    input.input_route.targets[0] = input.selection;
    input.input_scopes.count = 1u;
    struct snag_irc_scope *scope = &input.input_scopes.items[0];
    scope->target.destination = 7u;
    scope->target.generation = 23u;
    memset(scope->target.connection, 'a', SNAG_ID_HEX_LEN);
    (void)snag_strcpy(scope->endpoint, sizeof(scope->endpoint), "captured.invalid:6667");
    (void)snag_strcpy(scope->room, sizeof(scope->room), "#captured");
    scope->casemapping[0] = SNAG_IRC_ASCII;
    scope->casemapping[1] = SNAG_IRC_RFC1459_STRICT;
    (void)snag_strcpy(scope->chantypes[0], sizeof(scope->chantypes[0]), "#&");
    (void)snag_strcpy(scope->chantypes[1], sizeof(scope->chantypes[1]), "#");
    json_t *capture = snag_ui_input_capture(&input);
    assert(capture);
    memset(&input, 0, sizeof(input));
    struct ui_action action = {0};
    assert(input_capture_read(capture, &action) == 0);
    assert(action.received_ms == 1234u && action.snapshot.turn_generation == 17u);
    assert(action.snapshot.view == SNAG_RENDER_ROLLOUT && action.snapshot.active);
    assert(action.snapshot.verbosity == 4u && action.view_applied);
    assert(!strcmp(action.snapshot.label, "captured > "));
    assert(action.snapshot.selection.id == 7u && action.snapshot.selection.revision == 29u);
    assert(action.route.count == 1u && action.route.targets[0].id == 7u);
    assert(action.snapshot.scopes.count == 1u);
    scope = &action.snapshot.scopes.items[0];
    assert(scope->target.generation == 23u && scope->target.destination == 7u);
    assert(!strcmp(scope->endpoint, "captured.invalid:6667") &&
        !strcmp(scope->room, "#captured") && scope->casemapping[0] == SNAG_IRC_ASCII &&
        scope->casemapping[1] == SNAG_IRC_RFC1459_STRICT && !strcmp(scope->chantypes[0], "#&"));
    struct snag_ui_runtime runtime = {0};
    struct snag_ui_display display = {.runtime = &runtime};
    atomic_store(&runtime.session_attachment, 31u);
    assert(queue_open(&runtime.actions) == 0);
    assert(snag_wakeup_create(runtime.commands) == 0);
    snag_term_init(&display.term);
    display.render.view = SNAG_RENDER_CHAT;
    (void)snag_strcpy(display.term.label, sizeof(display.term.label), "later owner > ");
    json_t *route = json_string("rollout");
    assert(route && view_submit(&display, "11111111111111111111111111111111", "captured text",
        route, 31u, false, false, -1, true, false, capture) == 0);
    json_decref(route);
    struct ui_action *admitted = queue_pop(&runtime.actions);
    assert(admitted && !strcmp(admitted->text, "captured text"));
    assert(admitted->snapshot.view == SNAG_RENDER_ROLLOUT && admitted->snapshot.active);
    assert(admitted->received_ms == 1234u && admitted->snapshot.turn_generation == 17u);
    assert(!strcmp(admitted->snapshot.label, "captured > "));
    assert(admitted->snapshot.scopes.items[0].target.generation == 23u);
    assert(admitted->route.targets[0].revision == 29u && admitted->view_applied);
    free(admitted->text);
    free(admitted);
    snag_term_close(&display.term);
    snag_wakeup_close(runtime.actions.wake);
    snag_wakeup_close(runtime.commands);
    assert(json_object_set_new(capture, "active", json_string("true")) == 0);
    assert(input_capture_read(capture, &action) < 0);
    json_decref(capture);
#endif
}

static void
test_frontend_destination_context(void)
{
    struct snag_irc_destinations source = {.count = 1u}, decoded;
    struct snag_irc_destination *item = &source.items[0];
    item->target = (struct snag_irc_target){.id = 7u, .revision = 19u};
    item->generation = 23u;
    item->names_revision = 29u;
    item->joined = true;
    assert(snag_strcpy(item->connection, sizeof(item->connection),
        "11111111111111111111111111111111"));
    assert(snag_strcpy(item->endpoint, sizeof(item->endpoint), "test:6667"));
    assert(snag_strcpy(item->room, sizeof(item->room), "#room"));
    assert(snag_strcpy(item->operator, sizeof(item->operator), "operator"));
    assert(snag_strcpy(item->model, sizeof(item->model), "agent"));
    assert(snag_strcpy(item->nicks, sizeof(item->nicks), "alice bob"));
    for (size_t i = 0u; i < 2u; ++i) {
        item->casemapping[i] = SNAG_IRC_RFC1459;
        assert(snag_strcpy(item->chantypes[i], sizeof(item->chantypes[i]), "#&"));
    }
    json_t *rows = frontend_destinations(&source);
    assert(rows && frontend_destinations_read(rows, &decoded) == 0);
    assert(decoded.count == 1u);
    json_t *roundtrip = frontend_destinations(&decoded);
    assert(roundtrip && json_equal(rows, roundtrip));
    json_decref(roundtrip);
    /* Disconnected destinations can lose names before their route retires. */
    const char *empty_fields[] = {"room", "model", "operator", "nicks"};
    json_t *row = json_array_get(rows, 0u);
    for (size_t i = 0u; i < sizeof(empty_fields) / sizeof(empty_fields[0]); ++i) {
        json_t *original = json_incref(json_object_get(row, empty_fields[i]));
        assert(json_object_set_new(row, empty_fields[i], json_string("")) == 0);
        assert(frontend_destinations_read(rows, &decoded) == 0);
        roundtrip = frontend_destinations(&decoded);
        assert(roundtrip && json_equal(rows, roundtrip));
        json_decref(roundtrip);
        assert(json_object_set_new(row, empty_fields[i], json_integer(0)) == 0);
        assert(frontend_destinations_read(rows, &decoded) < 0);
        assert(json_object_set_new(row, empty_fields[i], original) == 0);
    }
    struct snag_ui_runtime runtime = {0};
    struct snag_ui_display display = {.runtime = &runtime, .frontend = true};
    snag_term_init(&display.term);
    json_t *state = json_pack("{s:O,s:{s:[s]}}", "destinations", rows,
        "irc_names", "sentinel", "Alice");
#if SNAJPAGENT_VM
    struct snag_ui input = {.input_received_ms = 456u, .input_turn_generation = 11u,
        .input_active = true, .input_view_applied = true, .input_verbosity = 3,
        .selection = {.id = 99u, .revision = 101u}};
    assert(snag_strcpy(input.submitted_label, sizeof(input.submitted_label), "source > "));
    json_t *capture = snag_ui_input_capture(&input);
    assert(capture && json_object_set_new(state, "prompt",
        json_pack("{s:b,s:i}", "active", false, "generation", 41)) == 0);
    json_t *forwarded = snag_ui_input_capture_forward(capture, state, "/fast off");
    struct ui_action action = {0};
    assert(forwarded && input_capture_read(forwarded, &action) == 0);
    assert(action.received_ms == 456u && !strcmp(action.snapshot.label, "source > "));
    assert(action.snapshot.verbosity == 3u && action.snapshot.turn_generation == 41u);
    assert(!action.snapshot.active && !action.view_applied && !action.snapshot.selection.id);
    assert(action.snapshot.view == SNAG_RENDER_ROLLOUT && !action.route.count);
    assert(action.snapshot.scopes.count == 1u);
    assert(action.snapshot.scopes.items[0].target.destination == 7u &&
        action.snapshot.scopes.items[0].target.generation == 23u &&
        !strcmp(action.snapshot.scopes.items[0].target.connection, item->connection));
    assert(!strcmp(action.snapshot.scopes.items[0].endpoint, "test:6667"));
    assert(json_object_set_new(json_array_get(rows, 0u), "generation", json_integer(24)) == 0);
    assert(input_capture_read(forwarded, &action) == 0);
    assert(action.snapshot.scopes.items[0].target.generation == 23u);
    assert(input_capture_read(capture, &action) == 0 && action.snapshot.selection.id == 99u &&
        action.snapshot.turn_generation == 11u && action.snapshot.active);
    assert(json_object_set_new(json_array_get(rows, 0u), "generation", json_integer(23)) == 0);
    json_decref(forwarded);
    json_decref(capture);
#endif
    struct ui_snapshot frozen = {.view = SNAG_RENDER_CHAT, .selection = item->target,
        .conversation = {.kind = SNAG_IRC_QUERY, .identity = SNAG_IRC_OPERATOR,
            .generation = 22u, .destination = 7u}};
    snag_irc_capture_scopes(&source, &frozen.scopes);
    (void)snag_strcpy(frozen.conversation.connection, sizeof(frozen.conversation.connection),
        item->connection);
    (void)snag_strcpy(frozen.conversation.conversation, sizeof(frozen.conversation.conversation),
        "55555555555555555555555555555555");
    (void)snag_strcpy(frozen.conversation.peer, sizeof(frozen.conversation.peer), "peer");
    display_capture_conversation(&display, "/query 7/peer retained body", &frozen);
    assert(frozen.address_conversation.generation == 22u);
    assert(!strcmp(frozen.address_conversation.conversation, frozen.conversation.conversation));
    display_capture_conversation(&display, "/query 7/peer", &frozen);
    assert(!frozen.address_conversation.connection[0]);
    display_capture_conversation(&display, "/query 7/other unrelated body", &frozen);
    assert(!frozen.address_conversation.connection[0]);
    json_t *frontend_state = snag_ui_frontend_state(state);
    struct snag_ui_command command = {.kind = SNAG_UI_VIEW_STATE, .data.voice = frontend_state};
    char error[128];
    json_t *empty_state = json_copy(frontend_state);
    assert(empty_state && json_object_set_new(empty_state, "destinations", json_array()) == 0);
    command.data.voice = empty_state;
    input_malloc_fails = true;
    assert(apply_message(&display, &command, error, sizeof(error)) == 0);
    assert(!display.term.destinations);
    assert(apply_message(&display, &command, error, sizeof(error)) == 0);
    assert(!display.term.destinations);
    assert(json_object_set_new(empty_state, "destinations", json_integer(0)) == 0);
    errno = 0;
    assert(apply_message(&display, &command, error, sizeof(error)) < 0 && errno == EINVAL);
    assert(!display.term.destinations);
    assert(json_object_set_new(empty_state, "destinations", json_array()) == 0);
    command.data.voice = frontend_state;
    errno = 0;
    assert(apply_message(&display, &command, error, sizeof(error)) < 0 && errno == ENOMEM);
    assert(!display.term.destinations);
    input_malloc_fails = false;
    assert(frontend_state && apply_message(&display, &command, error, sizeof(error)) == 0);
    assert(display.term.destinations && display.term.destinations->count == 1u);
    assert(display.term.destination.id == 7u && display.term.destination.revision == 19u);
    struct snag_irc_destinations *owned = display.term.destinations;
    command.data.voice = empty_state;
    input_malloc_fails = true;
    assert(apply_message(&display, &command, error, sizeof(error)) == 0);
    assert(display.term.destinations == owned && !owned->count);
    assert(display.term.destination.id == 7u && display.term.destination.revision == 19u);
    command.data.voice = frontend_state;
    assert(apply_message(&display, &command, error, sizeof(error)) == 0);
    assert(display.term.destinations == owned && owned->count == 1u);
    input_malloc_fails = false;
    json_decref(empty_state);
    assert(apply_message(&display, &command, error, sizeof(error)) == 0);
    assert(display.term.destinations == owned);
    assert(!display.view_state);
    assert(json_equal(display.term.irc_names, json_object_get(state, "irc_names")));
    struct snag_irc_scopes scopes;
    snag_irc_capture_scopes(display.term.destinations, &scopes);
    assert(scopes.count == 1u && scopes.items[0].target.generation == 23u);
    assert(!strcmp(scopes.items[0].room, "#room"));
#if SNAJPAGENT_VM
    struct snag_irc_conversation_target target = {.kind = SNAG_IRC_CHANNEL,
        .identity = SNAG_IRC_OPERATOR, .generation = 23u, .casemapping = SNAG_IRC_RFC1459};
    (void)snag_strcpy(target.connection, sizeof(target.connection), item->connection);
    (void)snag_strcpy(target.conversation, sizeof(target.conversation),
        "22222222222222222222222222222222");
    (void)snag_strcpy(target.membership, sizeof(target.membership),
        "33333333333333333333333333333333");
    (void)snag_strcpy(target.endpoint, sizeof(target.endpoint), item->endpoint);
    (void)snag_strcpy(target.room, sizeof(target.room), item->room);
    json_t *route = snag_irc_conversation_route(&target);
    assert(json_object_set_new(json_array_get(rows, 0u), "generation", json_integer(23)) == 0);
    assert(route && json_object_set_new(state, "channels",
        json_pack("[{s:O,s:s}]", "route", route, "endpoint", item->endpoint)) == 0);
    json_t *captured = snag_ui_input_capture(&input);
    json_t *addressed = snag_ui_input_capture_forward(captured, state, "/part test:6667/#room");
    assert(addressed && input_capture_read(addressed, &action) == 0);
    assert(action.snapshot.address_conversation.kind == SNAG_IRC_CHANNEL);
    assert(action.snapshot.address_conversation.destination == 7u);
    assert(!strcmp(action.snapshot.address_conversation.membership, target.membership));
    assert(!strcmp(action.snapshot.address_conversation.connection, item->connection));
    assert(json_is_null(json_object_get(captured, "address")));
    assert(json_object_set_new(route, "membership",
        json_string("44444444444444444444444444444444")) == 0);
    assert(input_capture_read(addressed, &action) == 0);
    assert(!strcmp(action.snapshot.address_conversation.membership, target.membership));
    json_decref(addressed);
    json_decref(captured);
    json_decref(frontend_state);
    frontend_state = snag_ui_frontend_state(state);
    command.data.voice = frontend_state;
    assert(frontend_state);
    assert(apply_message(&display, &command, error, sizeof(error)) == 0);
    assert(display.conversations && !display.conversations->next);
    assert(!strcmp(display.conversations->target.conversation, target.conversation));
    assert(display.conversations->target.destination == 7u);
    assert(display.conversations->target.generation == 23u);
    assert(apply_message(&display, &command, error, sizeof(error)) == 0);
    assert(!display.conversations->next);
    snag_buf_free(&display.conversations->draft);
    free(display.conversations);
    while (display.channel_drafts) {
        struct ui_channel_draft *draft = display.channel_drafts;
        display.channel_drafts = draft->next;
        snag_buf_free(&draft->text);
        free(draft);
    }
    json_decref(route);
#endif
    assert(json_array_set_new(json_object_get(json_array_get(rows, 0u), "target"),
        0u, json_integer(-1)) == 0);
    assert(frontend_destinations_read(rows, &decoded) < 0);
    snag_term_close(&display.term);
    json_decref(display.view_state);
    json_decref(rows);
    json_decref(state);
    json_decref(frontend_state);
}

static int
input_wait(void *opaque)
{
    int error = *(int *)opaque;
    if (!error) return 0;
    errno = error;
    return -1;
}

static ssize_t
input_empty(void *opaque, void *buffer, size_t size)
{
    (void)opaque;
    (void)buffer;
    (void)size;
    assert(!"idle input must not be read");
    return -1;
}

static void
input_bytes(struct snag_term *term, const char *text)
{
    size_t length = strlen(text);
    assert(length <= sizeof(term->input));
    memcpy(term->input, text, length);
    term->input_pos = 0u;
    term->input_len = length;
}

static void
test_checkpoint_during_output_retains_its_snapshot_and_reply(void)
{
#if SNAJPAGENT_VM
    allocation_fails = false;
    struct snag_ui_runtime runtime = {0};
    struct snag_ui_display display = {.runtime = &runtime};
    struct snag_term *term = &display.term;
    struct snag_view_channel pair[2];
    assert(snag_view_channel_pair(pair) == 0);
    display.view = snag_view_server_direct(&pair[1],
        "11111111111111111111111111111111", (struct snag_view_callbacks){
            view_bound, view_submit, view_control, &display, view_feedback, view_draft,
            view_input_control, view_presented});
    assert(display.view);
    assert(queue_open(&runtime.actions) == 0);
    assert(snag_wakeup_create(runtime.commands) == 0);
    snag_term_init(term);
    snag_render_init(&display.render, 0u);
    display.main_draft = &display.rollout_draft;
    term->opened = term->input_only = true;
    struct snag_buf output = {.max = SIZE_MAX};
    term->capture = &output;
    snag_term_output_bind(term);
    int input_error = 0;
    snag_term_input_redirect(&term->host, input_wait, input_empty, &input_error);
    assert(snag_buf_append(&term->draft, "checkpoint draft", 16u) == 0);
    term->cursor = 16u;
    input_bytes(term, "\033[9004;1~");
    while (term->input_pos < term->input_len) assert(read_input(&display, 0) == 0);
    assert(output.len == 0u);
    /* Later input must not replace the snapshot at the request boundary. */
    assert(snag_buf_append(&term->draft, " later", 6u) == 0);
    term->cursor += 6u;
    term->input_only = false;
    assert(read_input(&display, 0) == 0);
    assert(snag_buf_terminate(&output) == 0);
    assert(strstr((char *)output.data, "SNAJPAGENT-SCREEN/1:DRAFT:1:"));
    assert(strstr((char *)output.data, "\"text\":\"checkpoint draft\""));
    assert(!strstr((char *)output.data, " later"));
    snag_term_output_bind(NULL);
    term->capture = NULL;
    term->opened = false;
    snag_term_close(term);
    snag_buf_free(&output);
    snag_buf_free(&display.checkpoint_reply);
    snag_view_server_close(display.view);
    snag_view_channel_close(&pair[0]);
    snag_wakeup_close(runtime.actions.wake);
    snag_wakeup_close(runtime.commands);
#endif
}

static void
test_frontend_events_transfer_objects_in_queue_order(void)
{
    allocation_fails = false;
    struct snag_ui_runtime runtime = {.engine = pthread_self()};
    struct snag_ui_display display = {.runtime = &runtime, .frontend = true};
    assert(queue_open(&runtime.actions) == 0);
    assert(snag_wakeup_create(runtime.commands) == 0);
    struct snag_ui ui = {.runtime = &runtime, .frontend = true};
    snag_term_init(&display.term);
    snag_render_init(&display.render, 0u);
    display.main_draft = &display.rollout_draft;
    display.term.opened = display.term.input_only = true;
    int input_error = 0;
    snag_term_input_redirect(&display.term.host, input_wait, input_empty, &input_error);
    assert(snag_buf_append(&display.term.draft, "unchanged draft", 15u) == 0);
    display.term.cursor = 15u;
    ++display.term.input_revision;
    assert(frontend_feedback(&display, json_null(), SNAG_TERM_ECHO, "label", "echo\n\033") == 0);
    assert(read_input(&display, 0) == 0);
    enum snag_term_action action;
    char *text = NULL;
    assert(snag_ui_poll(&ui, 0, &action, &text) == 1);
    assert(action == SNAG_TERM_FEEDBACK && !text);
    assert(!strcmp(snag_json_string(ui.frontend_message, "type"), "feedback"));
    assert(!strcmp(snag_json_string(ui.frontend_message, "text"), "echo\n\033"));
    assert(json_integer_value(json_object_get(json_object_get(ui.frontend_message,
        "origin"), "event")) == 1);
    json_t *feedback = json_incref(ui.frontend_message);
    assert(snag_ui_poll(&ui, 0, &action, &text) == 1);
    assert(action == SNAG_TERM_EDITORS && !text);
    assert(!strcmp(snag_json_string(ui.frontend_message, "type"), "editors"));
    const json_t *snapshot = json_object_get(ui.frontend_message, "snapshot");
    const json_t *draft = json_array_get(json_object_get(snapshot, "drafts"), 0u);
    assert(!strcmp(snag_json_string(draft, "text"), "unchanged draft"));
    assert(json_integer_value(json_object_get(draft, "cursor")) == 15);
    assert(json_integer_value(json_object_get(json_object_get(snapshot, "feedback"),
        "event")) == 1);
    assert(snag_ui_poll(&ui, 0, &action, &text) == 0 && !ui.frontend_message);
    assert(!strcmp(snag_json_string(feedback, "text"), "echo\n\033"));
    json_decref(feedback);
    allocation_fails = true;
    assert(frontend_feedback(&display, json_null(), SNAG_TERM_ECHO, "label", "failed") < 0);
    assert(display.frontend_sequence == 1u && !queue_pop(&runtime.actions));
    allocation_fails = false;
    display.term.opened = false;
    snag_term_close(&display.term);
    snag_buf_free(&display.rollout_draft.text);
    snag_wakeup_close(runtime.actions.wake);
    snag_wakeup_close(runtime.commands);

    /* Shutdown releases both a polled event and one still in the queue. */
    assert(snag_ui_init(&ui) == 0);
    json_t *held = json_string("retained event");
    struct ui_action *queued = calloc(1u, sizeof(*queued));
    assert(held && queued);
    queued->action = SNAG_TERM_FEEDBACK;
    queued->frontend_message = json_incref(held);
    ui.frontend_message = json_incref(held);
    assert(queue_push(&ui.runtime->actions, queued) && held->refcount == 3u);
    snag_ui_free(&ui);
    assert(!ui.frontend_message && held->refcount == 1u);
    json_decref(held);
}

static void
test_priority_yield_retains_its_captured_input(void)
{
    struct snag_ui_runtime runtime = {.engine = pthread_self()};
    runtime.actions.wake[0] = runtime.actions.wake[1] = SNAG_WAKE_INVALID;
    runtime.commands[0] = runtime.commands[1] = SNAG_WAKE_INVALID;
    struct snag_ui_display display = {.runtime = &runtime, .turn_generation = 17u};
    snag_term_init(&display.term);
    display.term.input_only = true;
    display.term.active = true;
    display.term.animation.states = 1u << SNAG_TERM_SPINNER_TOOL;
    display.render.verbosity = 3u;
    assert(snag_strcpy(display.term.label, sizeof(display.term.label), "yield-source > "));
    struct ui_action *item = calloc(1u, sizeof(*item));
    assert(item);
    item->action = SNAG_TERM_SUBMIT;
    item->text = strdup("/yield");
    assert(item->text && finish_input(&display, item, 1) == 0);
    assert(!queue_pop(&runtime.actions));
    display.turn_generation = 18u;
    display.render.verbosity = 1u;
    struct snag_ui ui = {.runtime = &runtime, .frontend = true};
    enum snag_term_action action;
    char *text = NULL;
    assert(snag_ui_poll(&ui, 0, &action, &text) == 1);
    assert(action == SNAG_TERM_SUBMIT && text && !strcmp(text, "/yield"));
    assert(ui.input_received_ms && ui.input_turn_generation == 17u);
    assert(ui.input_active && ui.input_verbosity == 3);
    assert(!strcmp(ui.submitted_label, "yield-source > "));
    free(text);
    snag_term_close(&display.term);
}

static void
test_dictation_control_retains_its_captured_input(void)
{
    struct snag_ui_runtime runtime = {.engine = pthread_self()};
    runtime.actions.wake[0] = runtime.actions.wake[1] = SNAG_WAKE_INVALID;
    runtime.commands[0] = runtime.commands[1] = SNAG_WAKE_INVALID;
    struct snag_ui_display display = {.runtime = &runtime};
    snag_term_init(&display.term);
    display.term.input_only = true;
    display.render.verbosity = 3u;
    assert(snag_strcpy(display.term.label, sizeof(display.term.label), "dictation-source > "));
    const enum snag_term_action controls[] = {SNAG_TERM_DICTATE_DONE, SNAG_TERM_DICTATE_CANCEL};
    for (size_t i = 0u; i < 2u; ++i) {
        struct ui_action *item = calloc(1u, sizeof(*item));
        assert(item);
        item->action = controls[i];
        assert(finish_input(&display, item, 1) == 0);
        assert(!queue_pop(&runtime.actions));
        struct snag_ui ui = {.runtime = &runtime, .frontend = true};
        enum snag_term_action action;
        char *text = NULL;
        assert(snag_ui_poll(&ui, 0, &action, &text) == 1);
        assert(action == controls[i] && ui.input_received_ms && ui.input_verbosity == 3);
        assert(!strcmp(ui.submitted_label, "dictation-source > "));
        free(text);
    }
    snag_term_close(&display.term);
}

static void
test_owner_dictation_control_preserves_capture_and_attachment(void)
{
#if SNAJPAGENT_VM
    struct snag_ui_runtime runtime = {.engine = pthread_self()};
    runtime.actions.wake[0] = runtime.actions.wake[1] = SNAG_WAKE_INVALID;
    runtime.commands[0] = runtime.commands[1] = SNAG_WAKE_INVALID;
    struct snag_ui_display display = {.runtime = &runtime};
    struct snag_ui source = {.input_received_ms = 1234u, .input_verbosity = 3};
    assert(snag_strcpy(source.submitted_label, sizeof(source.submitted_label), "captured mic > "));
    json_t *capture = snag_ui_input_capture(&source);
    assert(capture);
    assert(view_input_control(&display, SNAG_TERM_DICTATE_DONE, capture) < 0);
    atomic_store(&runtime.session_attachment, 7u);
    assert(view_input_control(&display, SNAG_TERM_DICTATE_DONE, capture) == 0);
    /* Esc supersedes a pending Enter, without losing its capture context. */
    assert(view_input_control(&display, SNAG_TERM_DICTATE_CANCEL, capture) == 0);
    struct snag_ui ui = {.runtime = &runtime};
    enum snag_term_action action;
    char *text = NULL;
    assert(snag_ui_poll(&ui, 0, &action, &text) == 1);
    assert(action == SNAG_TERM_DICTATE_CANCEL && !text);
    assert(ui.input_received_ms == 1234u && ui.input_verbosity == 3);
    assert(!strcmp(ui.submitted_label, "captured mic > "));
    json_object_set_new(capture, "received", json_string("invalid"));
    assert(view_input_control(&display, SNAG_TERM_DICTATE_CANCEL, capture) < 0);
    assert(!atomic_load(&runtime.dictation_control));
    json_decref(capture);
#endif
}

#if SNAJPAGENT_VM
struct audio_ack_fixture {
    struct snag_ui_display *display;
    atomic_bool done;
    int result;
};

static void *
audio_ack_request(void *opaque)
{
    struct audio_ack_fixture *fixture = opaque;
    char error[128];
    fixture->result = apply_message(fixture->display,
        &(struct snag_ui_command){.kind = SNAG_UI_AUDIO, .text = "[MIC ON] ", .data.value = 1u},
        error, sizeof(error));
    snag_view_server_step(fixture->display->view);
    atomic_store(&fixture->done, true);
    return NULL;
}

static json_t *
audio_ack_exchange(struct snag_view_server *server, struct snag_view_channel *peer,
    json_t *request, const char *type)
{
    if (request) {
        assert(snag_view_channel_send(peer, request) == 0);
        json_decref(request);
    }
    uint64_t deadline = snag_monotonic_ms() + 3000u;
    while (snag_monotonic_ms() < deadline) {
        if (peer->output) assert(snag_view_channel_write(peer) >= 0);
        if (server) snag_view_server_step(server);
        json_t *reply = NULL;
        int rc = snag_view_channel_read(peer, &reply);
        assert(rc >= 0);
        if (rc && !strcmp(snag_json_string(reply, "type"), type)) return reply;
        json_decref(reply);
        assert(snag_sleep_ms(1u) == 0);
    }
    assert(!"audio presentation receipt timed out");
    return NULL;
}

static void
test_capture_waits_for_the_matching_presented_epoch(void)
{
    struct snag_ui_runtime runtime = {0};
    runtime.actions.wake[0] = runtime.actions.wake[1] = SNAG_WAKE_INVALID;
    runtime.commands[0] = runtime.commands[1] = SNAG_WAKE_INVALID;
    struct snag_ui_display display = {.runtime = &runtime, .direct = true};
    snag_term_init(&display.term);
    display.term.input_only = true;
    int input_error = 0;
    snag_term_input_redirect(&display.term.host, input_wait, input_empty, &input_error);
    display.main_draft = &display.rollout_draft;
    display.view_state = json_object();
    struct snag_view_channel pair[2];
    assert(display.view_state && snag_view_channel_pair(pair) == 0);
    display.view = snag_view_server_direct(&pair[1], "11111111111111111111111111111111",
        (struct snag_view_callbacks){view_bound, view_submit, view_control, &display,
            view_feedback, view_draft, view_input_control, view_presented});
    assert(display.view);
    json_t *reply = audio_ack_exchange(display.view, &pair[0],
        json_pack("{s:s,s:i}", "type", "hello", "version", 1), "capabilities");
    json_decref(reply);
    reply = audio_ack_exchange(display.view, &pair[0], json_pack("{s:s}", "type", "reserve"),
        "reserved");
    uint64_t generation = (uint64_t)json_integer_value(json_object_get(reply, "generation"));
    json_decref(reply);
    reply = audio_ack_exchange(display.view, &pair[0], json_pack("{s:s,s:I}", "type", "commit",
        "generation", (json_int_t)generation), "bound");
    json_decref(reply);
    assert(display.audio_epoch == 1u && !display.audio_presented);
    /* A legacy semantic client has not established a visible frontend. */
    struct audio_ack_fixture fixture = {.display = &display};
    assert(audio_ack_request(&fixture) == NULL && fixture.result == 1);
    reply = audio_ack_exchange(display.view, &pair[0], json_pack("{s:s,s:I,s:i}", "type",
        "presented", "generation", (json_int_t)generation, "audio", 1), "control");
    json_decref(reply);
    atomic_store(&fixture.done, false);
    pthread_t worker;
    assert(pthread_create(&worker, NULL, audio_ack_request, &fixture) == 0);
    do {
        reply = audio_ack_exchange(NULL, &pair[0], NULL, "state");
        const json_t *audio = json_object_get(json_object_get(reply, "state"), "audio");
        bool pending = json_integer_value(json_object_get(audio, "epoch")) == 2;
        json_decref(reply);
        if (pending) break;
    } while (true);
    assert(!atomic_load(&fixture.done));
    reply = audio_ack_exchange(NULL, &pair[0], json_pack("{s:s,s:I,s:i}", "type", "presented",
        "generation", (json_int_t)generation, "audio", 1), "error");
    json_decref(reply);
    assert(!atomic_load(&fixture.done));
    reply = audio_ack_exchange(NULL, &pair[0], json_pack("{s:s,s:I,s:i}", "type", "presented",
        "generation", (json_int_t)generation, "audio", 2), "control");
    json_decref(reply);
    assert(pthread_join(worker, NULL) == 0);
    assert(atomic_load(&fixture.done) && fixture.result == 0 && display.term.dictating);
    atomic_store(&fixture.done, false);
    assert(pthread_create(&worker, NULL, audio_ack_request, &fixture) == 0);
    do {
        reply = audio_ack_exchange(NULL, &pair[0], NULL, "state");
        const json_t *audio = json_object_get(json_object_get(reply, "state"), "audio");
        bool pending = json_integer_value(json_object_get(audio, "epoch")) == 3;
        json_decref(reply);
        if (pending) break;
    } while (true);
    assert(!atomic_load(&fixture.done));
    snag_view_channel_close(&pair[0]);
    assert(pthread_join(worker, NULL) == 0);
    assert(fixture.result == 1 && !display.term.dictating && !display.term.audio_label[0]);
    assert(!display.audio_presented && view_presented(&display, 3u) < 0);
    snag_view_server_close(display.view);
    json_decref(display.view_state);
    snag_term_close(&display.term);
}
#endif /* SNAJPAGENT_VM */

static void
test_audio_state_uses_the_ordinary_terminal_and_preserves_capture_guard(void)
{
    struct snag_ui_runtime runtime = {0};
    struct snag_ui_display display = {.runtime = &runtime};
    snag_term_init(&display.term);
    display.term.input_only = true;
    display.view_state = json_object();
    assert(display.view_state);
    char error[128];
    struct snag_ui_command audio = {.kind = SNAG_UI_AUDIO, .text = "[mic] ", .data.value = 1u};
    /* A bound semantic controller alone cannot make microphone capture visible. */
    atomic_store(&runtime.session_attachment, 1u);
    display.native = true;
    assert(apply_message(&display, &audio, error, sizeof(error)) == 1);
    assert(!display.term.dictating && !display.term.audio_label[0]);
    audio.data.value = 0u;
    assert(apply_message(&display, &audio, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(json_object_get(display.view_state, "audio"), "label"),
        "[mic] "));
    assert(apply_message(&display, &(struct snag_ui_command){.kind = SNAG_UI_CAPTION,
        .text = "retained partial", .data.value = 0u}, error, sizeof(error)) == 0);
    const json_t *captions = json_object_get(json_object_get(display.view_state, "audio"),
        "captions");
    assert(!strcmp(json_string_value(json_array_get(captions, 0u)), "retained partial"));
    audio.text = "";
    assert(apply_message(&display, &audio, error, sizeof(error)) == 0);
    assert(!display.term.caption[0][0] && !display.term.caption[1][0]);
    json_decref(display.view_state);
    snag_term_close(&display.term);
}

int
main(void)
{
    test_prompt_snapshot_reuses_only_unchanged_arrays();
    struct snag_ui_runtime runtime = {0};
    struct snag_ui_display display = {.runtime = &runtime};
    struct snag_term *term = &display.term;
    assert(queue_open(&runtime.actions) == 0);
    assert(snag_wakeup_create(runtime.commands) == 0);
    snag_term_init(term);
    snag_render_init(&display.render, 0u);
    term->opened = term->input_only = true;
    int input_error = 0;
    snag_term_input_redirect(&term->host, input_wait, input_empty, &input_error);

    /* Idle, typing-only and interrupted polls need neither an action nor its
     * large routing snapshot. Typing still publishes the engine pause. */
    for (unsigned int i = 0u; i < 128u; ++i) assert(read_input(&display, 0) == 0);
    assert(allocations == 0u && snapshots == 0u && !queue_pop(&runtime.actions));
    allocation_fails = true;
    assert(read_input(&display, 0) == 0);
    allocation_fails = false;
    input_bytes(term, "x");
    assert(read_input(&display, 0) == 0);
    assert(allocations == 0u && snapshots == 0u);
    term->typing_active = true;
    term->last_input_ms = 10u;
    term->typing_pause_ms = 25u;
    assert(read_input(&display, 0) == 0 && atomic_load(&runtime.pause_until) == 35u);
    term->typing_active = false;
    input_error = EINTR;
    assert(read_input(&display, 0) == 0 && atomic_load(&runtime.pause_until) == 0u);
    assert(allocations == 0u && snapshots == 0u);
    input_error = 0;

    /* A refresh waits behind admitted actions, then is delivered exactly once. */
    int occupied;
    for (size_t i = 0u; i < UI_QUEUE_CAPACITY; ++i)
        assert(queue_push(&runtime.actions, &occupied));
    term->history_refresh_requested = true;
    assert(read_input(&display, 0) == 0 && term->history_refresh_requested);
    assert(allocations == 0u && snapshots == 0u);
    for (size_t i = 0u; i < UI_QUEUE_CAPACITY; ++i)
        assert(queue_pop(&runtime.actions) == &occupied);
    assert(read_input(&display, 0) == 0 && !term->history_refresh_requested);
    struct ui_action *item = queue_pop(&runtime.actions);
    assert(item && item->history_refresh && !item->history_warning);
    free(item);
    assert(read_input(&display, 0) == 0 && !queue_pop(&runtime.actions));

    term->history_reader.warning = true;
    assert(read_input(&display, 0) == 0 && !term->history_reader.warning);
    item = queue_pop(&runtime.actions);
    assert(item && item->history_warning && !item->history_refresh);
    free(item);
    input_error = EIO;
    assert(read_input(&display, 0) == 0);
    item = queue_pop(&runtime.actions);
    assert(item && item->input_error && item->error == EIO);
    free(item);
    input_error = 0;

    snag_buf_reset(&term->draft);
    term->cursor = 0u;
    input_bytes(term, "submitted\r");
    assert(read_input(&display, 0) == 0);
    item = queue_pop(&runtime.actions);
    assert(item && item->action == SNAG_TERM_SUBMIT && !strcmp(item->text, "submitted"));
    assert(item->received_ms && item->snapshot.opened);
    free(item->text);
    free(item);

    /* A line returned by the editor is released if action allocation fails. */
    input_bytes(term, "failed\r");
    allocation_fails = true;
    size_t before_free = freed_text;
    assert(read_input(&display, 0) < 0 && errno == ENOMEM);
    assert(freed_text == before_free + 1u && !queue_pop(&runtime.actions));
    assert(allocations == 5u && snapshots == 4u);

    term->opened = false;
    snag_term_close(term);
    snag_wakeup_close(runtime.actions.wake);
    snag_wakeup_close(runtime.commands);
    test_checkpoint_during_output_retains_its_snapshot_and_reply();
    test_frontend_capture_preserves_the_actual_action();
    test_frontend_keeps_only_the_consumed_state_catalog();
    test_audio_updates_preserve_unrelated_display_state();
    test_semantic_channel_uses_bounded_span_encoding();
    test_display_refresh_reuses_its_immutable_session_snapshot();
    test_restore_inactive_draft_clears_its_history();
    test_first_exact_room_route_retains_draft_history();
    test_frontend_destination_context();
    test_frontend_events_transfer_objects_in_queue_order();
    test_priority_yield_retains_its_captured_input();
    test_dictation_control_retains_its_captured_input();
    test_owner_dictation_control_preserves_capture_and_attachment();
#if SNAJPAGENT_VM
    test_capture_waits_for_the_matching_presented_epoch();
#endif /* SNAJPAGENT_VM */
    test_audio_state_uses_the_ordinary_terminal_and_preserves_capture_guard();
    test_input_storage_follows_actual_input();
    test_ui_initialization_does_not_reserve_owner_transports();
    test_prompt_fields_share_owned_storage();
    test_pending_render_cancellation_preserves_orderly_exit();
    test_queued_prompt_keeps_the_presenter_view();
    test_owner_relay_failure_preserves_caller_ownership();
#if SNAJPAGENT_VM
    test_direct_channel_allocation_preserves_transfer_ownership();
    test_editor_close_stops_drafts_before_releasing_storage();
#endif
    return 0;
}
