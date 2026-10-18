/* SPDX-License-Identifier: GPL-2.0-only */
#include "ui.h"
#include "irc.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static size_t allocations, snapshots, freed_text;
static bool allocation_fails;

static void *
input_calloc(size_t count, size_t size)
{
    ++allocations;
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
#define free input_free
#define snag_irc_capture_scopes input_scopes
#include "../src/ui.c"
#undef snag_irc_capture_scopes
#undef free
#undef calloc

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

int
main(void)
{
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
    return 0;
}
