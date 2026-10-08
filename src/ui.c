/* SPDX-License-Identifier: GPL-2.0-only */
#include "ui.h"
#include "irc.h"
#include "irc_address.h"
#include "presentation.h"
#include "session_relay.h"
#include "session_view.h"
#include "turn.h"
#include "update.h"
#include "wake.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

struct ui_snapshot {
    enum snag_render_view view;
    bool opened, prompt_wanted, active, native_continuing;
    struct snag_terminal_profile profile;
    char label[SNAG_TERM_LABEL_BYTES];
    uint64_t turn_generation;
    struct snag_irc_target selection;
    struct snag_irc_conversation_target conversation, address_conversation;
    struct snag_irc_scopes scopes;
};

struct ui_message {
    struct snag_ui_command command;
    struct ui_snapshot snapshot;
    struct snag_buf delivered;
    char error[256];
    int result, saved_errno;
    bool prompt_changed;
    size_t input_tail;
    atomic_bool done;
};

#define UI_QUEUE_CAPACITY 32u
#define UI_RENDER_BATCH 8u
#define UI_INPUT_CAPACITY (SNAG_MAX_DIRECT_PROMPT + 4096u)
#define UI_HARD_EXIT_GRACE_MS 500u

struct ui_queue {
    _Atomic size_t head, tail;
    void *items[UI_QUEUE_CAPACITY];
    snag_wake_fd wake[2];
};

struct ui_input {
    pthread_mutex_t lock;
    pthread_t thread;
    snag_wake_fd control[2];
    struct snag_term_host *host;
    unsigned char *bytes;
    size_t head, len;
    uint64_t ctrl_c_since_ms, hard_exit_deadline_ms;
    unsigned int ctrl_c_count;
    bool running, stop, ended, overflow;
};

struct ui_hard_exit_watchdog {
    struct snag_term_host host;
    uint64_t deadline_ms;
};

struct ui_action {
    uint64_t received_ms;
    uint64_t attachment;
    bool interface_input, terminal_command;
    int verbosity;
    char view_request[SNAG_ID_HEX_LEN + 1u];
    enum snag_term_action action;
    char *text;
    int error;
    char feedback[192];
    bool submission_echoed, view_applied, input_error;
    bool history_refresh, history_warning, steering, local;
    struct ui_snapshot snapshot;
    struct snag_irc_route route;
};

struct snag_ui_runtime {
    struct ui_queue actions;
    struct ui_input input;
    snag_wake_fd commands[2];
    _Atomic(struct ui_message *) request; /* One synchronous engine call. */
    pthread_t thread, engine;
    struct snag_signal_mask saved_mask;
    atomic_int fatal;
    atomic_bool exit_requested, cancel, hard_exit_acknowledged;
    atomic_uint yield_requested; /* 1: pending echo, 2: already echoed. */
    atomic_bool hard_exit_requested;
    atomic_uint steering_pending, dictation_control;
    atomic_uint level, view;
    _Atomic uint64_t interrupt;
    _Atomic uint64_t pause_until;
    _Atomic uint64_t session_pending, session_attachment;
    _Atomic uint64_t session_releasing, session_failures;
};

struct ui_conversation_tab {
    struct ui_conversation_tab *next;
    struct snag_irc_conversation_target target;
    char endpoint[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    struct snag_buf draft;
    size_t cursor;
    bool opened;
};

struct ui_channel_draft {
    struct ui_channel_draft *next;
    uint32_t destination;
    struct snag_buf text;
    size_t cursor;
};

struct snag_ui_display {
    struct snag_render render;
    struct snag_presentation_writer *presentation;
    char presentation_error[256];
    struct snag_term term;
    struct ui_conversation_tab *conversations, *conversation;
    struct ui_channel_draft rollout_draft;
    struct ui_channel_draft *channel_drafts, *main_draft;
    struct snag_term_prompt prompt;
    struct snag_update *update;
    struct snag_ui_runtime *runtime;
    uint64_t turn_generation;
    bool suspended;
    bool input_closed, backlog_warned, view_repainting;
    bool native, native_barrier, native_continuing, native_quitting;
    bool direct, direct_opened;
    struct snag_view_channel direct_channel;
    struct snag_terminal_profile profile;
    struct snag_session_process native_process;
    struct snag_session_relay relay;
    uint64_t voice_bound_generation;
    unsigned char voice_bound_offer[SNAG_SESSION_VOICE_BYTES];
    struct snag_session_listener listener;
    struct snag_view_server *view;
    json_t *view_state;
    char native_notice[256];
    char feedback[192];
    struct ui_action *local;
    bool local_acknowledged, painting_feedback;
};

static int
retain_output(struct snag_ui_display *display, const struct snag_ui_command *command)
{
    if (display->presentation_error[0]) return 0;
    bool failed = snag_presentation_append(display->presentation, command) < 0;
    if (failed) {
        (void)snprintf(display->presentation_error, sizeof(display->presentation_error),
            "Session output retention stopped: %s", strerror(errno));
    }
    if (display->view_state) {
        json_t *position = snag_presentation_snapshot(display->presentation);
        if (!position) return -1;
        if (!failed && json_equal(position, json_object_get(display->view_state, "presentation"))) {
            json_decref(position);
            return 0;
        }
        /* Nested fields already belong to this immutable display snapshot. */
        json_t *state = json_copy(display->view_state);
        if (!state) {
            json_decref(position);
            return -1;
        }
        if (json_object_set_new(state, "presentation", position) < 0 ||
            (failed && json_object_set_new(state, "presentation_error",
                           json_string(display->presentation_error)) < 0)) {
            json_decref(state);
            return -1;
        }
        snag_view_server_state(display->view, state);
        json_decref(display->view_state);
        display->view_state = state;
    }
    if (!failed || display->direct || snag_view_server_attached(display->view)) return 0;
    return snag_render_warning_ctx(&display->render, display->presentation_error);
}

static int view_state(struct snag_ui_display *, const json_t *);

static int
view_feedback(void *opaque, const json_t *route, enum snag_term_feedback kind,
    const char *label, const char *text)
{
    struct snag_ui_display *display = opaque;
    struct snag_ui_command command = {.kind = kind == SNAG_TERM_CHOICES ?
        SNAG_UI_CHOICES : SNAG_UI_SUBMITTED, .label = label, .text = text,
        .data.value = true, .route = route};
    if (retain_output(display, &command) < 0) return -1;
    if (kind == SNAG_TERM_CHOICES)
        return display->term.input_only ? 0 :
            snag_presentation_apply(&display->render, &command, NULL);
    if (!display->term.input_only) return 0;
    display->term.prompt_clock.captured = false;
    snag_term_capture_prompt_clock(&display->term, time(NULL));
    return view_state(display, display->view_state);
}

static int
editor_feedback(void *opaque, enum snag_term_feedback kind, const char *label, const char *text)
{
    struct snag_ui_display *display = opaque;
    json_t *route = display->term.chat ?
        snag_irc_conversation_route(&display->term.conversation) : json_null();
    if (!route) return -1;
    int rc = view_feedback(display, route, kind, label, text);
    json_decref(route);
    return rc;
}

static int
present_local(struct snag_ui_display *display, enum snag_ui_operation kind, const char *label,
    const char *text, bool input, const struct ui_snapshot *origin)
{
    json_t *route = origin && origin->view == SNAG_RENDER_CHAT ?
        snag_irc_conversation_route(&origin->conversation) : json_null();
    if (!route) return -1;
    struct snag_ui_command command = {
        .kind = kind, .label = label, .text = text, .data.value = input, .route = route};
    int rc = retain_output(display, &command);
    if (!rc) rc = snag_presentation_apply(&display->render, &command, NULL);
    json_decref(route);
    return rc;
}

static int
set_level(struct snag_ui_display *display, unsigned int level)
{
    if (!snag_verbosity_name(level)) return snag_errno(EINVAL);
    display->render.verbosity = level;
    atomic_store(&display->runtime->level, level);
    return 0;
}

static void
verbosity_command(struct snag_ui_display *display, const char *text)
{
    unsigned int level = display->render.verbosity;
    if (snag_verbosity_apply(text, &level, display->render.view,
        display->feedback, sizeof(display->feedback))) (void)set_level(display, level);
}

static int
queue_open(struct ui_queue *queue)
{
    atomic_init(&queue->head, 0u);
    atomic_init(&queue->tail, 0u);
    return snag_wakeup_create(queue->wake);
}

static bool
queue_full(struct ui_queue *queue)
{
    return atomic_load_explicit(&queue->head, memory_order_relaxed) -
               atomic_load_explicit(&queue->tail, memory_order_acquire) ==
           UI_QUEUE_CAPACITY;
}

static bool
queue_push(struct ui_queue *queue, void *item)
{
    size_t head = atomic_load_explicit(&queue->head, memory_order_relaxed);
    if (head - atomic_load_explicit(&queue->tail, memory_order_acquire) == UI_QUEUE_CAPACITY)
        return false;
    queue->items[head % UI_QUEUE_CAPACITY] = item;
    atomic_store_explicit(&queue->head, head + 1u, memory_order_release);
    snag_wakeup_send(queue->wake[1]);
    return true;
}

static void *
queue_pop(struct ui_queue *queue)
{
    size_t tail = atomic_load_explicit(&queue->tail, memory_order_relaxed);
    void *item;
    if (tail == atomic_load_explicit(&queue->head, memory_order_acquire)) return NULL;
    item = queue->items[tail % UI_QUEUE_CAPACITY];
    atomic_store_explicit(&queue->tail, tail + 1u, memory_order_release);
    return item;
}

static int
input_status(void *opaque)
{
    struct ui_input *input = opaque;
    int status = 0;
    (void)pthread_mutex_lock(&input->lock);
    if (input->overflow || input->len) status |= SNAG_TERM_WAIT_INPUT;
    if (input->ended && !input->len && !input->overflow) status |= SNAG_TERM_WAIT_END;
    (void)pthread_mutex_unlock(&input->lock);
    return status;
}

static ssize_t
input_read(void *opaque, void *buffer, size_t size)
{
    struct ui_input *input = opaque;
    size_t first, take;
    (void)pthread_mutex_lock(&input->lock);
    if (input->overflow) {
        input->overflow = false;
        (void)pthread_mutex_unlock(&input->lock);
        return snag_errno(EOVERFLOW);
    }
    take = input->len < size ? input->len : size;
    first = UI_INPUT_CAPACITY - input->head;
    if (first > take) first = take;
    memcpy(buffer, input->bytes + input->head, first);
    memcpy((unsigned char *)buffer + first, input->bytes, take - first);
    input->head = (input->head + take) % UI_INPUT_CAPACITY;
    input->len -= take;
    (void)pthread_mutex_unlock(&input->lock);
    return (ssize_t)take;
}

static void
input_hard_exit_host(struct snag_term_host *host)
{
    /* The emergency path must not wait for a full terminal output queue: on
     * POSIX TCSAFLUSH has drain semantics and can deadlock behind the stalled
     * presentation writer this fallback exists to escape. Restore immediately;
     * process exit discards any unread input. */
    (void)snag_term_input_restore(host, false);
#ifdef _WIN32
    ExitProcess(0u);
#else
    _exit(0);
#endif
}

static void
input_hard_exit(struct snag_ui_runtime *runtime)
{
    input_hard_exit_host(runtime->input.host);
}

static void *
input_hard_exit_watchdog(void *opaque)
{
    struct ui_hard_exit_watchdog *watchdog = opaque;
    for (;;) {
        uint64_t now = snag_monotonic_ms();
        if (now >= watchdog->deadline_ms) break;
        uint64_t remaining = watchdog->deadline_ms - now;
        (void)snag_sleep_ms((unsigned int)remaining);
    }
    input_hard_exit_host(&watchdog->host);
    return NULL;
}

static void
input_note_controls(struct snag_ui_runtime *runtime, const unsigned char *bytes, size_t len)
{
    struct ui_input *input = &runtime->input;
    struct snag_term_host emergency_host;
    uint64_t deadline_ms = 0u;
    bool exit = false;

    (void)pthread_mutex_lock(&input->lock);
    for (size_t i = 0u; i < len; ++i) {
        uint64_t now = snag_monotonic_ms();
        if (bytes[i] != 0x03u) {
            input->ctrl_c_count = 0u;
            continue;
        }
        if (!input->ctrl_c_count || now - input->ctrl_c_since_ms > 2000u) {
            input->ctrl_c_since_ms = now;
            input->ctrl_c_count = 0u;
        }
        if (++input->ctrl_c_count == 5u) {
            deadline_ms = input->hard_exit_deadline_ms = now + UI_HARD_EXIT_GRACE_MS;
            emergency_host = *input->host;
            exit = true;
        }
    }
    (void)pthread_mutex_unlock(&input->lock);
    if (exit) {
        atomic_store(&runtime->hard_exit_requested, true);
        struct ui_hard_exit_watchdog *watchdog = malloc(sizeof(*watchdog));
        pthread_t thread;
        int rc = watchdog ? 0 : ENOMEM;
        if (watchdog) {
            watchdog->host = emergency_host;
            watchdog->deadline_ms = deadline_ms;
            rc = pthread_create(&thread, NULL, input_hard_exit_watchdog, watchdog);
            if (rc == 0)
                (void)pthread_detach(thread);
            else
                free(watchdog);
        }
        atomic_store(&runtime->exit_requested, true);
        snag_wakeup_send(runtime->actions.wake[1]);
        snag_wakeup_send(runtime->commands[1]);
        /* Resource exhaustion must not turn the hard escape into an ordinary
         * drain-dependent shutdown. */
        if (rc != 0) input_hard_exit_host(&emergency_host);
    }
}

static void
input_enqueue(struct snag_ui_runtime *runtime, const unsigned char *bytes, size_t len)
{
    struct ui_input *input = &runtime->input;
    size_t at, first;
    (void)pthread_mutex_lock(&input->lock);
    if (len > UI_INPUT_CAPACITY - input->len) {
        input->head = input->len = 0u;
        input->overflow = true;
    } else {
        at = (input->head + input->len) % UI_INPUT_CAPACITY;
        first = UI_INPUT_CAPACITY - at;
        if (first > len) first = len;
        memcpy(input->bytes + at, bytes, first);
        memcpy(input->bytes, bytes + first, len - first);
        input->len += len;
    }
    (void)pthread_mutex_unlock(&input->lock);
    snag_wakeup_send(runtime->commands[1]);
}

enum held_control { HELD_NONE, HELD_INTERRUPT, HELD_EXIT, HELD_YIELD };

static enum held_control
input_take_held_control(struct ui_input *input, bool allow_yield)
{
    static const char yield_line[] = "/yield\r";
    enum held_control action = HELD_NONE;

    (void)pthread_mutex_lock(&input->lock);
    for (size_t i = 0u; i < input->len; ++i) {
        unsigned char byte = input->bytes[(input->head + i) % UI_INPUT_CAPACITY];
        size_t used = 0u;

        if (byte == 0x03u || byte == 0x04u) {
            action = byte == 0x03u ? HELD_INTERRUPT : HELD_EXIT;
            used = 1u;
        } else if (allow_yield && byte == '/' &&
                   (i == 0u || input->bytes[(input->head + i - 1u) % UI_INPUT_CAPACITY] == '\r') &&
                   input->len - i >= sizeof(yield_line) - 1u) {
            bool matches = true;

            for (size_t j = 0u; j < sizeof(yield_line) - 1u; ++j)
                if (input->bytes[(input->head + i + j) % UI_INPUT_CAPACITY] !=
                    (unsigned char)yield_line[j])
                    matches = false;
            if (matches) {
                action = HELD_YIELD;
                used = sizeof(yield_line) - 1u;
            }
        }
        if (!used) continue;
        /* Cancel hidden typeahead before this priority control. Bytes typed
         * afterward remain available at the next safe prompt. */
        input->head = (input->head + i + used) % UI_INPUT_CAPACITY;
        input->len -= i + used;
        input->overflow = false;
        break;
    }
    (void)pthread_mutex_unlock(&input->lock);
    return action;
}

static void *
input_main(void *opaque)
{
    struct snag_ui_runtime *runtime = opaque;
    struct ui_input *input = &runtime->input;
    unsigned char bytes[4096];
    for (;;) {
        int timeout = -1;
        (void)pthread_mutex_lock(&input->lock);
        bool stop = input->stop;
        uint64_t deadline = input->hard_exit_deadline_ms;
        (void)pthread_mutex_unlock(&input->lock);
        if (stop) break;
        if (deadline && !atomic_load(&runtime->hard_exit_acknowledged)) {
            uint64_t now = snag_monotonic_ms();
            if (now >= deadline) input_hard_exit(runtime);
            timeout = (int)(deadline - now);
        }
        int rc = snag_term_input_native_wait(input->host, input->control[0], timeout);
        if (rc < 0) {
            if (errno == EINTR) continue;
            atomic_store(&runtime->fatal, errno ? errno : EIO);
            snag_wakeup_send(runtime->actions.wake[1]);
            snag_wakeup_send(runtime->commands[1]);
            break;
        }
        if (rc == 0) continue;
        if (rc & SNAG_TERM_WAIT_WAKE) snag_wakeup_drain(input->control[0]);
        (void)pthread_mutex_lock(&input->lock);
        stop = input->stop;
        (void)pthread_mutex_unlock(&input->lock);
        if (stop) break;
        if (rc & SNAG_TERM_WAIT_END) {
            (void)pthread_mutex_lock(&input->lock);
            input->ended = true;
            (void)pthread_mutex_unlock(&input->lock);
            snag_wakeup_send(runtime->commands[1]);
            break;
        }
        if (!(rc & SNAG_TERM_WAIT_INPUT)) continue;
        ssize_t count = snag_term_input_native_read(input->host, bytes, sizeof(bytes));
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        if (count < 0) {
            atomic_store(&runtime->fatal, errno ? errno : EIO);
            snag_wakeup_send(runtime->actions.wake[1]);
            snag_wakeup_send(runtime->commands[1]);
            break;
        }
        if (!count) {
            (void)pthread_mutex_lock(&input->lock);
            input->ended = true;
            (void)pthread_mutex_unlock(&input->lock);
            snag_wakeup_send(runtime->commands[1]);
            break;
        }
        input_note_controls(runtime, bytes, (size_t)count);
        input_enqueue(runtime, bytes, (size_t)count);
    }
    return NULL;
}

static int
input_start(struct snag_ui_display *display, const unsigned char *first, size_t first_len)
{
    struct snag_ui_runtime *runtime = display->runtime;
    struct ui_input *input = &runtime->input;
    int rc;
    (void)pthread_mutex_lock(&input->lock);
    input->host = &display->term.host;
    input->head = input->len = 0u;
    input->ctrl_c_since_ms = input->hard_exit_deadline_ms = 0u;
    input->ctrl_c_count = 0u;
    input->stop = input->ended = input->overflow = false;
    (void)pthread_mutex_unlock(&input->lock);
    atomic_store(&runtime->hard_exit_acknowledged, false);
    /* Seed the empty ring before the worker can read newer terminal bytes. */
    if (first_len) input_enqueue(runtime, first, first_len);
    snag_term_input_redirect(input->host, input_status, input_read, input);
    rc = pthread_create(&input->thread, NULL, input_main, runtime);
    if (rc != 0) {
        snag_term_input_redirect(input->host, NULL, NULL, NULL);
        input->host = NULL;
        return snag_errno(rc);
    }
    input->running = true;
    return 0;
}

static void
input_stop(struct snag_ui_display *display)
{
    struct ui_input *input = &display->runtime->input;
    if (!input->running) return;
    atomic_store(&display->runtime->hard_exit_acknowledged, true);
    (void)pthread_mutex_lock(&input->lock);
    input->stop = true;
    (void)pthread_mutex_unlock(&input->lock);
    snag_wakeup_send(input->control[1]);
    (void)pthread_join(input->thread, NULL);
    snag_term_input_redirect(input->host, NULL, NULL, NULL);
    input->host = NULL;
    input->running = false;
}

static void
display_capture_conversation(
    struct snag_ui_display *display, const char *text, struct ui_snapshot *snapshot)
{
    memset(&snapshot->address_conversation, 0, sizeof(snapshot->address_conversation));
    if (!text) return;
    size_t verb = strcspn(text, " \t\r\n");
    if (!((verb == 6u && !strncmp(text, "/query", verb)) ||
            (verb == 4u && !strncmp(text, "/msg", verb)) ||
            (verb == 5u && !strncmp(text, "/part", verb)) ||
            (verb == 7u && !strncmp(text, "/notice", verb))))
        return;
    char *operand = NULL;
    const char *rest;
    char error[128u];
    if (snag_irc_address_operand(text + verb, &operand, &rest, error, sizeof(error)) < 0) return;
    (void)rest;
    bool channel_id = !strncmp(operand, "channel:", 8u);
    const char *id = channel_id                        ? operand + 8u
                     : !strncmp(operand, "query:", 6u) ? operand + 6u
                                                       : NULL;
    const struct snag_irc_scope *scope = NULL;
    struct snag_irc_address address;
    if (!id) {
        if (snag_irc_address_parse(
                &address, operand, SNAG_IRC_MESSAGE_ADDRESS, error, sizeof(error)) < 0)
            goto done;
        scope = snag_irc_scope_resolve(&snapshot->scopes,
            snapshot->view == SNAG_RENDER_CHAT ? snapshot->selection.id : 0u, address.endpoint,
            error, sizeof(error));
        if (!scope) goto done;
    }
    for (struct ui_conversation_tab *tab = display->conversations; tab; tab = tab->next) {
        if (tab->target.identity != SNAG_IRC_OPERATOR) continue;
        if (id && (tab->target.kind == SNAG_IRC_CHANNEL) != channel_id) continue;
        if (id ? strcmp(id, tab->target.conversation)
               : (strcmp(scope->target.connection, tab->target.connection) ||
                     scope->target.generation != tab->target.generation ||
                     !snag_irc_name_equal(scope->casemapping[SNAG_IRC_OPERATOR], address.target,
                         tab->target.kind == SNAG_IRC_CHANNEL ? tab->target.room
                                                              : tab->target.peer)))
            continue;
        snapshot->address_conversation = tab->target;
        break;
    }
done:
    free(operand);
}

static void
take_snapshot(struct snag_ui_display *display, struct ui_snapshot *snapshot, const char *text)
{
    snapshot->view = snag_render_view(&display->render);
    snapshot->opened = display->term.opened || display->direct_opened;
    snapshot->prompt_wanted = display->term.prompt_wanted;
    snapshot->active = display->term.active;
    snapshot->native_continuing = display->native_continuing;
    snapshot->profile = display->profile;
    snapshot->turn_generation = display->turn_generation;
    snapshot->selection = display->term.destination;
    snapshot->conversation = display->term.conversation;
    snag_irc_capture_scopes(display->term.destinations, &snapshot->scopes);
    memcpy(snapshot->label, display->term.label, sizeof(snapshot->label));
    display_capture_conversation(display, text, snapshot);
}

static void
adopt_snapshot(struct snag_ui *ui, const struct ui_snapshot *snapshot)
{
    ui->view = snapshot->view;
    ui->opened = snapshot->opened;
    ui->prompt_wanted = snapshot->prompt_wanted;
    ui->active = snapshot->active;
    ui->native_continuing = snapshot->native_continuing;
    ui->profile = snapshot->profile;
    ui->turn_generation = snapshot->turn_generation;
    ui->selection = snapshot->selection;
    memcpy(ui->label, snapshot->label, sizeof(ui->label));
}

static void
prompt_free(struct snag_term_prompt *prompt)
{
    free((void *)prompt->source);
    for (size_t i = 0u; i < SNAG_PROMPT_HOUR; ++i) free((void *)prompt->values[i]);
    memset(prompt, 0, sizeof(*prompt));
}

static void
message_free(struct ui_message *message)
{
    if (message->command.kind == SNAG_UI_HISTORY_SNAPSHOT)
        snag_history_snapshot_free(&message->command.data.history.entries);
    if (message->command.kind == SNAG_UI_PROMPT || message->command.kind == SNAG_UI_VALIDATE)
        prompt_free(&message->command.data.prompt);
}

static bool
same_conversation_view(const struct ui_conversation_tab *a, const struct ui_conversation_tab *b)
{
    if (!a || !b) return false;
    if (a == b) return true;
    if (a->target.kind == SNAG_IRC_CONNECTION_EVENTS &&
        b->target.kind == SNAG_IRC_CONNECTION_EVENTS)
        return !strcmp(a->target.connection, b->target.connection);
    return a->target.kind == SNAG_IRC_CHANNEL && b->target.kind == SNAG_IRC_CHANNEL &&
           !strcmp(a->target.connection, b->target.connection) &&
           snag_irc_name_equal(a->target.casemapping, a->target.room, b->target.room);
}

static bool
extra_tab(const struct snag_ui_display *display, const struct ui_conversation_tab *tab)
{
    if (tab->target.kind == SNAG_IRC_CONNECTION_EVENTS)
        return tab->opened && tab->target.identity == SNAG_IRC_OPERATOR;
    if (tab->target.kind != SNAG_IRC_CHANNEL) return true;
    if (tab->target.identity == SNAG_IRC_AGENT) {
        for (const struct ui_conversation_tab *other = display->conversations; other;
            other = other->next) {
            if (other->target.identity == SNAG_IRC_OPERATOR && same_conversation_view(tab, other))
                return false;
        }
        return true;
    }
    const struct snag_term *term = &display->term;
    for (size_t i = 0u; term->destinations && i < term->destinations->count; ++i) {
        const struct snag_irc_destination *destination = &term->destinations->items[i];
        if (!strcmp(destination->connection, tab->target.connection) &&
            snag_irc_name_equal(
                destination->casemapping[SNAG_IRC_OPERATOR], destination->room, tab->target.room))
            return false;
    }
    return true;
}

static int view_state(struct snag_ui_display *, const json_t *);

static int
apply_prompt(struct snag_ui_display *display)
{
    struct snag_term *term = &display->term;

    term->conversation_labels = term->destinations && term->destinations->count > 1u;
    for (const struct ui_conversation_tab *tab = display->conversations;
        tab && !term->conversation_labels; tab = tab->next) {
        if (tab->target.kind != SNAG_IRC_CONNECTION_EVENTS && extra_tab(display, tab)) {
            term->conversation_labels = true;
        }
    }
    term->blank_local = display->prompt.values[0] != NULL;
    if (snag_term_configure_prompt(&display->term, &display->prompt, &display->term) < 0) return -1;
    return view_state(display, display->view_state);
}

static const struct snag_irc_destination *
selected_destination(const struct snag_term *term)
{
    if (!term->destinations) return NULL;
    for (size_t i = 0u; i < term->destinations->count; ++i)
        if (term->destinations->items[i].target.id == term->destination.id)
            return &term->destinations->items[i];
    return NULL;
}

static struct ui_channel_draft *
display_channel_draft(struct snag_ui_display *display, uint32_t destination)
{
    struct ui_channel_draft **slot = &display->channel_drafts;
    while (*slot && (*slot)->destination != destination) slot = &(*slot)->next;
    if (!*slot) {
        *slot = calloc(1u, sizeof(**slot));
        if (!*slot) return NULL;
        (*slot)->destination = destination;
        snag_buf_init(&(*slot)->text, SNAG_MAX_DIRECT_PROMPT + 1u);
    }
    return *slot;
}

static int
display_select_draft(struct snag_ui_display *display, struct ui_conversation_tab *next,
    struct ui_channel_draft *main)
{
    if (!next && !main) return -1;
    if (display->conversation == next && (next || display->main_draft == main)) return 0;
    struct snag_term *term = &display->term;
    struct ui_conversation_tab *old = display->conversation;
    term->defer_redraw = true;
    if (snag_term_swap_draft(term, old ? &old->draft : &display->main_draft->text,
            old ? &old->cursor : &display->main_draft->cursor) < 0 ||
        snag_term_swap_draft(
            term, next ? &next->draft : &main->text, next ? &next->cursor : &main->cursor) < 0)
        return -1;
    display->conversation = next;
    if (!next) display->main_draft = main;
    term->conversation = next ? next->target : (struct snag_irc_conversation_target){0};
    if (next) {
        next->opened = true;
        for (size_t i = 0u; term->destinations && i < term->destinations->count; ++i)
            if (term->destinations->items[i].target.id == next->target.destination)
                term->destination = term->destinations->items[i].target;
    }
    return 0;
}

static int
display_restore_input(struct snag_ui_display *display, const struct snag_ui_command *command)
{
    struct snag_buf *draft;
    size_t *cursor;
    bool focused;
    if (command->data.draft.view == SNAG_RENDER_CHAT && command->data.draft.conversation[0]) {
        struct ui_conversation_tab *tab = display->conversations;
        while (tab && strcmp(tab->target.conversation, command->data.draft.conversation))
            tab = tab->next;
        if (!tab) return snag_errno(ENOENT);
        draft = &tab->draft;
        cursor = &tab->cursor;
        focused = display->conversation == tab;
    } else {
        struct ui_channel_draft *main =
            command->data.draft.view == SNAG_RENDER_ROLLOUT
                ? &display->rollout_draft
                : display_channel_draft(display, command->data.draft.destination);
        if (!main) return -1;
        draft = &main->text;
        cursor = &main->cursor;
        focused = !display->conversation && display->main_draft == main;
    }
    if (focused) return snag_term_restore_draft(&display->term, command->text);
    if (command->len > SNAG_MAX_DIRECT_PROMPT) return snag_errno(EOVERFLOW);
    if (command->len > draft->len && snag_buf_reserve(draft, command->len - draft->len) < 0)
        return -1;
    snag_buf_reset(draft);
    if (snag_buf_append(draft, command->text, command->len) < 0) return -1;
    *cursor = command->len;
    return 0;
}

static int
display_conversation_event(struct snag_ui_display *display, const struct snag_irc_event *event)
{
    if (!event->routed || (event->route.kind == SNAG_IRC_CHANNEL && !event->route.membership[0]))
        return 0;
    struct snag_term *term = &display->term;
    struct ui_conversation_tab **slot = &display->conversations;
    while (*slot && strcmp((*slot)->target.conversation, event->route.conversation))
        slot = &(*slot)->next;
    bool created = !*slot;
    if (created) {
        *slot = calloc(1u, sizeof(**slot));
        if (!*slot) return -1;
        snag_buf_init(&(*slot)->draft, SNAG_MAX_DIRECT_PROMPT + 1u);
        memcpy((*slot)->target.conversation, event->route.conversation,
            sizeof((*slot)->target.conversation));
        memcpy((*slot)->endpoint, event->endpoint, sizeof((*slot)->endpoint));
        term->conversation_tabs = true;
    }
    struct ui_conversation_tab *tab = *slot;
    bool focused = display->conversation == tab && display->render.view == SNAG_RENDER_CHAT;
    /* A draft keeps its recipient until explicitly discarded/reopened. */
    if (!(focused ? term->draft.len : tab->draft.len)) {
        tab->target.kind = event->route.kind;
        tab->target.identity = event->route.identity;
        tab->target.generation = event->route.generation;
        memcpy(tab->target.connection, event->route.connection, sizeof(tab->target.connection));
        memcpy(tab->target.peer, event->route.peer, sizeof(tab->target.peer));
        memcpy(tab->target.room, event->room, sizeof(tab->target.room));
        memcpy(tab->target.membership, event->route.membership, sizeof(tab->target.membership));
        memcpy(tab->target.endpoint, event->endpoint, sizeof(tab->target.endpoint));
        for (size_t i = 0u; term->destinations && i < term->destinations->count; ++i) {
            const struct snag_irc_destination *destination = &term->destinations->items[i];
            if (!strcmp(destination->connection, event->route.connection)) {
                tab->target.destination = destination->target.id;
                tab->target.casemapping = destination->casemapping[event->route.identity];
            }
        }
        if (focused) term->conversation = tab->target;
    }
    if (created && tab->target.kind == SNAG_IRC_CHANNEL &&
        tab->target.identity == SNAG_IRC_OPERATOR) {
        for (size_t i = 0u; term->destinations && i < term->destinations->count; ++i) {
            const struct snag_irc_destination *destination = &term->destinations->items[i];
            if (strcmp(destination->connection, tab->target.connection) ||
                !snag_irc_name_equal(destination->casemapping[SNAG_IRC_OPERATOR], destination->room,
                    tab->target.room))
                continue;
            struct ui_channel_draft *main = display_channel_draft(display, destination->target.id);
            if (!main) return -1;
            if (!display->conversation && display->main_draft == main && term->chat) {
                /* The live default-room draft gains its first exact route. */
                display->conversation = tab;
                term->conversation = tab->target;
                focused = true;
            } else {
                struct snag_buf empty = tab->draft;
                tab->draft = main->text;
                main->text = empty;
                tab->cursor = main->cursor;
                main->cursor = 0u;
            }
            break;
        }
    }
    return display->prompt.source && !display->view_repainting ? apply_prompt(display) : 0;
}

static struct ui_conversation_tab *
display_room_tab(struct snag_ui_display *display, const struct snag_irc_destination *destination)
{
    for (struct ui_conversation_tab *tab = display->conversations; tab; tab = tab->next)
        if (destination && tab->target.kind == SNAG_IRC_CHANNEL &&
            tab->target.identity == SNAG_IRC_OPERATOR &&
            !strcmp(tab->target.connection, destination->connection) &&
            snag_irc_name_equal(destination->casemapping[SNAG_IRC_OPERATOR], tab->target.room,
                destination->room))
            return tab;
    return NULL;
}

static int
display_set_chat_room(
    struct snag_ui_display *display, const struct snag_irc_destination *destination, bool announce)
{
    const char *endpoint = destination ? destination->endpoint : "";
    const char *room = destination ? destination->room : "";
    struct ui_conversation_tab *tab = display_room_tab(display, destination);
    bool leaving_conversation = display->conversation != tab;
    if (display_select_draft(display, tab,
            display_channel_draft(display, destination ? destination->target.id : 0u)) < 0)
        return -1;
    if (snag_render_set_chat_room(&display->render, endpoint, room, announce) < 0) return -1;
    display->view_repainting =
        display->render.view == SNAG_RENDER_CHAT && snag_render_view_pending(&display->render);
    if (display->view_repainting)
        display->term.defer_redraw = true;
    else if (leaving_conversation && !display->native_barrier)
        display->term.defer_redraw = false;
    return 0;
}

static int
display_set_view(
    struct snag_ui_display *display, enum snag_render_view view, bool announce, bool prompt_ready)
{
    struct snag_term *term = &display->term;
    bool changed = display->render.view != view;

    if (view != SNAG_RENDER_CHAT && view != SNAG_RENDER_ROLLOUT) return snag_errno(EINVAL);
    term->defer_redraw = true;
    if (announce && changed &&
        snag_render_host(&display->render,
            view == SNAG_RENDER_CHAT ? "switching to chat" : "switching to rollout") < 0)
        return -1;
    if (view == SNAG_RENDER_ROLLOUT && display->conversation) {
        if (display_set_chat_room(display, selected_destination(term), false) < 0) return -1;
    }
    if (view == SNAG_RENDER_ROLLOUT) {
        if (display_select_draft(display, NULL, &display->rollout_draft) < 0) return -1;
    } else if (!display->conversation &&
               display_select_draft(
                   display, NULL, display_channel_draft(display, term->destination.id)) < 0)
        return -1;
    term->defer_redraw = true;
    term->chat = view == SNAG_RENDER_CHAT;
    if (snag_render_set_view(&display->render, view) < 0) return -1;
    display->view_repainting = snag_render_view_pending(&display->render);
    if (display->prompt.source) {
        display->prompt.mode = view == SNAG_RENDER_CHAT ? 0u : display->prompt.active ? 2u : 1u;
        if (!display->view_repainting && prompt_ready) {
            term->defer_redraw = false;
            if (apply_prompt(display) < 0) return -1;
        }
    } else if (!display->view_repainting && prompt_ready) {
        term->defer_redraw = false;
    }
    atomic_store(&display->runtime->view, (unsigned int)view);
    return 0;
}

/* The presenter owns tab order for every view. Connection logs remain in the
 * directory so a workspace can include a log explicitly opened in a pane. */
static json_t *
display_tabs(struct snag_ui_display *display)
{
    const struct snag_term *term = &display->term;
    size_t channels =
        term->destinations && term->destinations->count ? term->destinations->count : 1u;
    json_t *tabs = json_pack("[[s,b]]", "rollout", true);
    if (!tabs) return NULL;
    for (size_t i = 0u; i < channels; ++i) {
        const struct snag_irc_destination *destination =
            term->destinations && term->destinations->count ? &term->destinations->items[i] : NULL;
        const struct ui_conversation_tab *tab = display_room_tab(display, destination);
        json_t *entry = tab ? json_pack("[s,b]", tab->target.conversation, true) :
            json_pack("[I,b]", (json_int_t)(destination ? destination->target.id : 0u), true);
        if (!entry || json_array_append_new(tabs, entry) < 0) goto fail;
    }
    for (struct ui_conversation_tab *tab = display->conversations; tab; tab = tab->next) {
        bool enabled = extra_tab(display, tab);
        if (!enabled && !(tab->target.kind == SNAG_IRC_CONNECTION_EVENTS &&
            tab->target.identity == SNAG_IRC_OPERATOR)) continue;
        json_t *entry = json_pack("[s,b]", tab->target.conversation, enabled);
        if (!entry || json_array_append_new(tabs, entry) < 0) goto fail;
    }
    return tabs;
fail:
    json_decref(tabs);
    return NULL;
}

static int
display_cycle_view(struct snag_ui_display *display)
{
    struct snag_term *term = &display->term;
    size_t channels =
        term->destinations && term->destinations->count ? term->destinations->count : 1u;
    json_t *tabs = display_tabs(display);
    if (!tabs) return -1;
    size_t count = json_array_size(tabs);
    size_t current = 0u;
    if (display->render.view == SNAG_RENDER_CHAT) {
        if (display->conversation && extra_tab(display, display->conversation)) {
            for (size_t i = 1u; i < count; ++i) {
                const char *id = json_string_value(json_array_get(json_array_get(tabs, i), 0u));
                if (id && !strcmp(id, display->conversation->target.conversation)) current = i;
            }
        } else {
            current = 1u;
            for (size_t i = 0u; term->destinations && i < term->destinations->count; ++i)
                if (term->destinations->items[i].target.id == term->destination.id)
                    current = i + 1u;
        }
    }
    size_t next = current;
    do {
        next = term->view_reverse ? (next ? next - 1u : count - 1u) : (next + 1u) % count;
    } while (!json_is_true(json_array_get(json_array_get(tabs, next), 1u)));
    const char *id = json_string_value(json_array_get(json_array_get(tabs, next), 0u));
    struct ui_conversation_tab *tab = display->conversations;
    while (tab && (!id || strcmp(id, tab->target.conversation))) tab = tab->next;
    json_decref(tabs);
    if (!next) return display_set_view(display, SNAG_RENDER_ROLLOUT, true, true);
    if (next <= channels) {
        const struct snag_irc_destination *destination =
            term->destinations && term->destinations->count ? &term->destinations->items[next - 1u]
                                                            : NULL;
        if (destination && snag_term_select_destination(term, destination->target.id) < 0)
            return -1;
        if (display_set_chat_room(display, destination, true) < 0) return -1;
    } else {
        if (!tab) return snag_errno(EINVAL);
        if (display_select_draft(display, tab, NULL) < 0 ||
            snag_render_set_chat_conversation(
                &display->render, tab->endpoint, &term->conversation, true) < 0)
            return -1;
    }
    return display_set_view(display, SNAG_RENDER_CHAT, true, true);
}

#if SNAJPAGENT_VM
static void
view_bound(void *opaque, uint64_t generation)
{
    struct snag_ui_display *display = opaque;
    struct snag_ui_runtime *runtime = display->runtime;
    /* A semantic controller retains the editor but paints through its own
     * renderer. Classic reattachment rebuilds the presentation boundary. */
    display->term.input_only = display->direct || generation != 0u;
    display->term.defer_redraw = display->direct || generation != 0u;
    display->native_barrier = false;
    display->view_repainting = false;
    atomic_store(&runtime->session_attachment, generation);
    atomic_store(&runtime->session_pending, 0u);
    atomic_store(&runtime->session_releasing, 0u);
    snag_wakeup_send(runtime->actions.wake[1]);
}

static bool
view_selects_conversation(const char *text)
{
    size_t length = strcspn(text, " \t\r\n");
    char verb[16];
    if (length >= sizeof(verb)) return false;
    memcpy(verb, text, length);
    verb[length] = '\0';
    if (!snag_string_in(verb, "/query /chat /join /connections")) return false;
    const char *argument = text + length;
    while (isspace((unsigned char)*argument)) ++argument;
    if (!*argument) return true;
    char *operand = NULL;
    const char *rest;
    char error[128];
    if (snag_irc_address_operand(argument, &operand, &rest, error, sizeof(error)) < 0) {
        return false;
    }
    free(operand);
    return !*rest;
}

static int
view_submit(void *opaque, const char *id, const char *text, const json_t *route,
    uint64_t generation, bool terminal, bool queued, int verbosity)
{
    struct snag_ui_display *display = opaque;
    struct snag_ui_runtime *runtime = display->runtime;
    if (display->suspended || display->input_closed ||
        atomic_load(&runtime->session_attachment) != generation)
        return snag_errno(ESTALE);
    if (queue_full(&runtime->actions) || display->local) return snag_errno(EAGAIN);
    /* Semantic controllers paint independently; refresh their submission snapshot
     * from the shared animation state instead of the owner's last terminal frame. */
    if (!terminal && display->term.animation.source[0] &&
        snag_term_animation_render(&display->term.animation, snag_monotonic_ms(),
            display->term.label) < 0) return -1;
    struct ui_action *item = calloc(1u, sizeof(*item));
    if (!item) return -1;
    item->text = strdup(text);
    if (!item->text) {
        free(item);
        return -1;
    }
    memcpy(item->view_request, id, sizeof(item->view_request));
    item->received_ms = snag_time_ms();
    item->action = queued ? SNAG_TERM_QUEUE : SNAG_TERM_SUBMIT;
    item->attachment = generation;
    item->interface_input = true;
    item->terminal_command = terminal;
    item->verbosity = verbosity;
    if (terminal) {
        if (snag_term_hide(&display->term) < 0) {
            free(item->text);
            free(item);
            return -1;
        }
        display->term.prompt_wanted = false;
        snag_term_destination_route(&display->term, text, &item->route);
    }
    take_snapshot(display, &item->snapshot, NULL);
    item->snapshot.view = SNAG_RENDER_ROLLOUT;
    item->snapshot.selection = (struct snag_irc_target){0};
    item->snapshot.conversation = (struct snag_irc_conversation_target){0};
    if (json_is_object(route)) {
        struct snag_irc_conversation_target target;
        if (terminal || snag_view_conversation_read(route, &target) < 0 ||
            target.identity != SNAG_IRC_OPERATOR)
            goto stale;
        struct ui_conversation_tab *tab;
        for (tab = display->conversations; tab; tab = tab->next)
            if (!strcmp(tab->target.connection, target.connection) &&
                !strcmp(tab->target.conversation, target.conversation) &&
                tab->target.identity == target.identity && tab->target.kind == target.kind)
                break;
        if (!tab) goto stale;
        struct snag_irc_scope *scope = NULL;
        for (size_t i = 0u; i < item->snapshot.scopes.count; ++i)
            if (!strcmp(item->snapshot.scopes.items[i].target.connection, target.connection)) {
                scope = &item->snapshot.scopes.items[i];
                break;
            }
        if (!scope) goto stale;
        target.destination = scope->target.destination;
        /* Explicit selection can reopen a disconnected pane. Message-bearing
         * commands and drafts retain the pane's original connection epoch. */
        if (!view_selects_conversation(text)) scope->target.generation = target.generation;
        item->snapshot.selection.id = target.destination;
        item->snapshot.view = SNAG_RENDER_CHAT;
        item->snapshot.conversation = target;
    }
    display_capture_conversation(display, text, &item->snapshot);
    item->steering = !queued && item->snapshot.view == SNAG_RENDER_ROLLOUT &&
                     item->snapshot.active && !snag_prompt_command(text);
    if (item->steering) atomic_fetch_add(&runtime->steering_pending, 1u);
    if (queue_push(&runtime->actions, item)) {
        display->term.prompt_clock.captured = false;
        return 0;
    }
    if (item->steering) atomic_fetch_sub(&runtime->steering_pending, 1u);
    free(item->text);
    free(item);
    return snag_errno(EAGAIN);
stale:
    free(item->text);
    free(item);
    return snag_errno(ESTALE);
}

static int
view_control(void *opaque, bool quit)
{
    struct snag_ui_display *display = opaque;
    if (display->input_closed) return snag_errno(ESTALE);
    if (quit) {
        atomic_store(&display->runtime->exit_requested, true);
        display->input_closed = true;
    } else {
        atomic_store(&display->runtime->interrupt,
            display->turn_generation ? display->turn_generation : UINT64_MAX);
    }
    snag_wakeup_send(display->runtime->actions.wake[1]);
    return 0;
}

#endif /* SNAJPAGENT_VM */

static int
session_service(struct snag_ui_display *display, int timeout_ms)
{
    enum snag_session_message event;
    snag_view_server_step(display->view);
    if (!display->native) return 0;
    if (!display->native_quitting && atomic_load(&display->runtime->hard_exit_requested) &&
        display->relay.peer >= 0 && display->relay.phase == SNAG_SESSION_ATTACHED) {
        /* The sole relay writer announces the watchdog's accepted hard escape.
         * Never have the input/watchdog thread interleave transport frames. */
        if (snag_session_relay_control(&display->relay, SNAG_SESSION_QUITTING, NULL, 0u) == 0)
            display->native_quitting = true;
        else if (errno != EAGAIN && errno != EALREADY)
            return -1;
    }
    int rc = snag_session_relay_step(&display->relay, &display->listener, timeout_ms, &event);
    if (rc) return rc < 0 ? -1 : snag_errno(EPIPE);
    if (event == SNAG_SESSION_COMMIT) {
        display->native_quitting = false;
        display->term.input_only = false;
        atomic_store(&display->runtime->session_releasing, 0u);
        atomic_store(&display->runtime->session_attachment, 0u);
        atomic_store(&display->runtime->session_pending, display->relay.generation);
        snag_wakeup_send(display->runtime->actions.wake[1]);
    } else if (event == SNAG_SESSION_BOUND) {
        if (display->relay.voice_offered) {
            display->voice_bound_generation = display->relay.generation;
            memcpy(display->voice_bound_offer, display->relay.voice_offer,
                sizeof(display->voice_bound_offer));
        }
        atomic_store(&display->runtime->session_attachment, display->relay.generation);
        snag_wakeup_send(display->runtime->actions.wake[1]);
    } else if (event == SNAG_SESSION_COMMAND) {
        display->relay.command_admitted =
            snag_view_server_terminal(display->view, display->relay.command_reference) == 0;
    } else if (event == SNAG_SESSION_RELEASE) {
        atomic_store(&display->runtime->session_attachment, 0u);
        atomic_store(&display->runtime->session_releasing, display->relay.generation);
        snag_wakeup_send(display->runtime->actions.wake[1]);
    } else if (event == SNAG_SESSION_RESIZE) {
        snag_term_notify_resize();
        if (display->suspended) (void)snag_session_process_redraw(&display->native_process);
    } else if (event == SNAG_SESSION_DETACH) {
        display->native_continuing = false;
        display->native_quitting = false;
        atomic_store(&display->runtime->session_attachment, 0u);
        atomic_store(&display->runtime->session_pending, 0u);
        atomic_store(&display->runtime->session_releasing, 0u);
    } else if (event == SNAG_SESSION_ERROR) {
        (void)atomic_fetch_add(&display->runtime->session_failures, 1u);
        snag_wakeup_send(display->runtime->actions.wake[1]);
        (void)snprintf(display->native_notice, sizeof(display->native_notice), "%.*s",
            (int)display->relay.event_length, display->relay.event_data);
    }
    return 0;
}

static int
session_control_generation(struct snag_ui_display *display, enum snag_session_message type,
    const void *data, size_t length, uint64_t generation)
{
    if (display->direct) {
        if (type != SNAG_SESSION_EXIT || length != 1u) return snag_errno(ENOTSUP);
        snag_view_server_exit(display->view, *(const unsigned char *)data);
        uint64_t deadline = snag_monotonic_ms() + 5000u;
        while (snag_view_server_busy(display->view)) {
            snag_view_server_step(display->view);
            if (snag_monotonic_ms() >= deadline) return snag_errno(ETIMEDOUT);
            (void)snag_sleep_ms(1u);
        }
        return 0;
    }
    if (!display->native) return snag_errno(ENOTSUP);
    if (type == SNAG_SESSION_EXIT && length == 1u)
        snag_view_server_exit(display->view, *(const unsigned char *)data);
    uint64_t deadline = snag_monotonic_ms() + 5000u;
    for (;;) {
        if (session_service(display, 0) < 0) return -1;
        if (generation &&
            (display->relay.generation != generation ||
                display->relay.phase != SNAG_SESSION_ATTACHED || display->relay.peer < 0))
            return 0;
        if (snag_session_relay_control(&display->relay, type, data, length) == 0) break;
        if (type == SNAG_SESSION_EXIT && errno == ENOTCONN) break;
        if (errno != EAGAIN) return -1;
        if (snag_monotonic_ms() >= deadline) return snag_errno(ETIMEDOUT);
        if (session_service(display, 1) < 0) return -1;
    }
    if (type == SNAG_SESSION_SUSPEND) {
        display->native_continuing = true;
        atomic_store(&display->runtime->session_attachment, 0u);
    }
    /* Synchronous UI completion includes the control frame, not only its
     * placement behind output. The engine can then resume producing output. */
    while (display->relay.output.used ||
           (type == SNAG_SESSION_EXIT && snag_view_server_busy(display->view))) {
        if (snag_monotonic_ms() >= deadline) return snag_errno(ETIMEDOUT);
        if (session_service(display, 1) < 0) return -1;
    }
    return 0;
}

static int
session_control(struct snag_ui_display *display, enum snag_session_message type, const void *data,
    size_t length)
{
    return session_control_generation(display, type, data, length, 0u);
}

static int
session_suspend(void *opaque)
{
    struct snag_ui_display *display = opaque;
    if (snag_term_hide(&display->term) < 0 || snag_term_attachment_modes(&display->term, false) < 0)
        return -1;
    if (session_control(display, SNAG_SESSION_SUSPEND, NULL, 0u) < 0)
        (void)snprintf(display->native_notice, sizeof(display->native_notice),
            "cannot suspend terminal client: %s", strerror(errno));
    return 0;
}

static int
apply_session(struct snag_ui_display *display, const struct snag_ui_command *command)
{
    struct snag_ui_runtime *runtime = display->runtime;
#if SNAJPAGENT_VM
    if (command->kind == SNAG_UI_SESSION_DIRECT) {
        struct snag_view_channel *channel = command->data.view_channel;
        if (display->native || display->direct) return snag_errno(EALREADY);
        if (!channel || !channel->local) return snag_errno(EINVAL);
        display->direct_channel = *channel;
        snag_view_channel_init(channel, -1);
        display->direct = true;
        display->term.input_only = display->term.defer_redraw = true;
        return 0;
    }
#endif /* SNAJPAGENT_VM */
    if (command->kind == SNAG_UI_SESSION_START) {
        struct snag_session_process *process = command->data.session_process;
        if (display->native || display->direct) return snag_errno(EALREADY);
        if (snag_session_relay_init(&display->relay, process->master, process->peer) < 0) return -1;
        display->listener = (struct snag_session_listener){.fd = -1, .dir_fd = -1};
        display->native_process = (struct snag_session_process){
            .master = -1, .slave = process->slave, .peer = -1, .profile = process->profile};
        process->master = process->slave = process->peer = -1;
        display->native = true;
        display->profile = display->relay.profile = display->native_process.profile;
        display->term.screen = display->profile.sty[0] != '\0';
        display->term.backend = display->profile;
        atomic_store(&runtime->session_attachment, display->relay.generation);
        display->term.suspend = session_suspend;
        display->term.suspend_opaque = display;
        return snag_term_output_prepare(&display->term);
    }
    if (!display->native && !display->direct) return snag_errno(ENOTSUP);
    if (command->kind == SNAG_UI_SESSION_LISTEN) {
        const struct snag_session *session = command->data.session;
        if (!session) {
            snag_presentation_writer_close(display->presentation);
            display->presentation = NULL;
            display->presentation_error[0] = '\0';
            snag_view_server_stop(display->view);
            if (display->native) snag_session_listener_close(&display->listener);
            return 0;
        }
        if (!display->presentation) {
            display->presentation = snag_presentation_writer_open(session->dir_fd, session->id);
            if (display->presentation &&
                snag_presentation_start(display->presentation, session->next_seq) < 0) {
                snag_presentation_writer_close(display->presentation);
                display->presentation = NULL;
            }
            if (!display->presentation) {
                (void)snprintf(display->presentation_error, sizeof(display->presentation_error),
                    "Session output retention unavailable: %s", strerror(errno));
                if (!display->direct &&
                    snag_render_warning_ctx(&display->render, display->presentation_error) < 0)
                    return -1;
            }
        }
        if (display->native && snag_session_listener_open(&display->listener, session->dir_fd,
                                   session->dir_path, session->lock_fd) < 0)
            return -1;
#if SNAJPAGENT_VM
        struct snag_view_callbacks callbacks = {
            view_bound, view_submit, view_control, display, view_feedback};
        display->view =
            display->direct
                ? snag_view_server_direct(&display->direct_channel, session->id, callbacks)
                : snag_view_server_open(session->dir_fd, session->dir_path, session->lock_fd,
                      session->id, &display->relay, callbacks);
        if (!display->view) {
            if (display->native) snag_session_listener_close(&display->listener);
            return -1;
        }
#endif /* SNAJPAGENT_VM */
        return 0;
    }
    if (command->kind == SNAG_UI_SESSION_CONTROL)
        return session_control(
            display, (enum snag_session_message)command->data.value, command->text, command->len);
    if (display->direct) return snag_errno(ENOTSUP);
    if (command->kind == SNAG_UI_SESSION_BOUND) {
        if (!command->data.session_voice.bytes || !command->data.session_voice.present)
            return snag_errno(EINVAL);
        bool present = display->voice_bound_generation &&
                       display->voice_bound_generation == command->data.session_voice.generation;
        *command->data.session_voice.present = present;
        if (present) {
            memcpy(command->data.session_voice.bytes, display->voice_bound_offer,
                sizeof(display->voice_bound_offer));
            display->voice_bound_generation = 0u;
        }
        return 0;
    }
    if (command->kind == SNAG_UI_SESSION_OFFER || command->kind == SNAG_UI_SESSION_RELEASED) {
        uint64_t generation = command->data.session_voice.generation;
        if (display->relay.generation != generation || display->relay.peer < 0)
            return snag_errno(ESTALE);
        if (command->kind == SNAG_UI_SESSION_OFFER) {
            if (!command->data.session_voice.bytes || !command->data.session_voice.present)
                return snag_errno(EINVAL);
            *command->data.session_voice.present = display->relay.voice_offered;
            if (display->relay.voice_offered) {
                memcpy(command->data.session_voice.bytes, display->relay.voice_offer,
                    SNAG_SESSION_VOICE_BYTES);
            }
            return 0;
        }
        if (atomic_load(&runtime->session_releasing) != generation ||
            command->data.session_voice.mode > SNAG_SESSION_VOICE_MUTED)
            return snag_errno(ESTALE);
        unsigned char receipt[17];
        memcpy(receipt, display->relay.voice_offer, 16u);
        receipt[16] = (unsigned char)command->data.session_voice.mode;
        int rc = snag_session_relay_control(
            &display->relay, SNAG_SESSION_RELEASED, receipt, sizeof(receipt));
        if (!rc) atomic_store(&runtime->session_releasing, 0u);
        return rc;
    }
    if (display->relay.generation != command->data.seq || display->relay.peer < 0)
        return snag_errno(ESTALE);
    if (command->kind == SNAG_UI_SESSION_PROGRESS) {
        int rc = snag_session_relay_control(&display->relay, SNAG_SESSION_PROGRESS, NULL, 0u);
        return rc < 0 && errno == EAGAIN ? 0 : rc;
    }
    if (command->kind == SNAG_UI_SESSION_REFUSE) {
        int rc = session_control(display, SNAG_SESSION_ERROR, command->text, command->len);
        if (!rc) atomic_store(&runtime->session_pending, 0u);
        return rc;
    }
    if (command->kind == SNAG_UI_SESSION_REBIND) {
        if (display->relay.phase != SNAG_SESSION_REPAINT) return snag_errno(ESTALE);
        display->native_barrier = true;
        display->term.defer_redraw = true;
        if (snag_render_rebind(&display->render) < 0 ||
            snag_session_relay_activate(
                &display->relay, command->data.seq, display->native_process.slave) < 0)
            return -1;
        display->profile = display->relay.profile;
        display->term.screen = display->profile.sty[0] != '\0';
        display->term.backend = display->profile;
        atomic_store(&runtime->session_pending, 0u);
        if (display->suspended) (void)snag_session_process_redraw(&display->native_process);
        if (!display->suspended && display->term.opened &&
            snag_term_attachment_modes(&display->term, true) < 0)
            return -1;
        return 0;
    }
    if (display->relay.phase != SNAG_SESSION_ATTACHED &&
        display->relay.phase != SNAG_SESSION_ACCEPTED)
        return snag_errno(ESTALE);
    display->native_barrier = false;
    display->term.defer_redraw = display->view_repainting;
    if (!display->suspended && display->prompt.source && !display->view_repainting)
        return apply_prompt(display);
    return 0;
}

static bool
session_command(enum snag_ui_operation kind)
{
    return kind >= SNAG_UI_SESSION_START && kind <= SNAG_UI_SESSION_READY;
}

static int admit_input(struct snag_ui_display *, const char *, uint64_t);

static json_t *
view_prompt(const struct snag_ui_display *display)
{
    const struct snag_term_prompt *prompt = &display->prompt;
    if (!prompt->source || !prompt->values[0]) return json_null();
    json_t *values = json_array();
    json_t *frames = json_array();
    if (!values || !frames) goto fail;
    const char *resolved[SNAG_PROMPT_FIELD_COUNT];
    char clock[3][12];
    snag_term_prompt_values(prompt, &display->term, &display->term, resolved, clock);
    for (size_t i = 0u; i < SNAG_PROMPT_FIELD_COUNT; ++i)
        if (json_array_append_new(values, json_string(resolved[i])) < 0) goto fail;
    for (size_t i = 0u; i < SNAG_TERM_SPINNER_COUNT; ++i)
        if (json_array_append_new(frames, json_string(prompt->frames[i])) < 0) goto fail;
    return json_pack("{s:s,s:o,s:o,s:i,s:i,s:b,s:I,s:I,s:b,s:b}", "template", prompt->source,
        "values", values, "frames", frames, "states", (int)prompt->states, "rate",
        (int)prompt->rate, "active", prompt->active, "epoch",
        (json_int_t)display->term.animation.epoch, "tool_until",
        (json_int_t)display->term.animation.tool_until, "interrupted",
        display->term.interrupt_pending, "conversation_labels", display->term.conversation_labels);
fail:
    json_decref(values);
    json_decref(frames);
    return NULL;
}

static int
view_state(struct snag_ui_display *display, const json_t *state)
{
    if (!state) return 0;
    json_t *copy = json_deep_copy(state);
    if (!copy) return -1;
    if (display->term.irc_names &&
        json_object_set(copy, "irc_names", display->term.irc_names) < 0) {
        json_decref(copy);
        return -1;
    }
    if (json_object_set_new(copy, "tabs", display_tabs(display)) < 0 ||
        json_object_set_new(copy, "prompt", view_prompt(display)) < 0) {
        json_decref(copy);
        return -1;
    }
    if (json_object_set_new(
            copy, "presentation", snag_presentation_snapshot(display->presentation)) < 0) {
        json_decref(copy);
        return -1;
    }
    if (display->presentation_error[0] && json_object_set_new(copy, "presentation_error",
                                              json_string(display->presentation_error)) < 0) {
        json_decref(copy);
        return -1;
    }
    snag_view_server_state(display->view, copy);
    json_decref(display->view_state);
    display->view_state = copy;
    return 0;
}

static int
apply_message(struct snag_ui_display *display, struct snag_ui_command *command, char *error,
    size_t error_size)
{
    struct snag_render *render = &display->render;
    struct snag_term *term = &display->term;

    if (command->kind == SNAG_UI_IRC && display_conversation_event(display, command->data.irc) < 0)
        return -1;
    int presented = snag_presentation_apply(render, command, NULL);
    if (presented != 1) return presented;

    switch (command->kind) {
    case SNAG_UI_VIEW_STATE: {
        /* Routes leave the display worker with its current server case rules. */
        json_t *channels = json_object_get(command->data.voice, "channels");
        for (size_t i = 0u; i < json_array_size(channels); ++i) {
            json_t *route = json_object_get(json_array_get(channels, i), "route");
            const char *connection = snag_json_string(route, "connection");
            const char *identity = snag_json_string(route, "identity");
            for (size_t j = 0u; term->destinations && j < term->destinations->count; ++j) {
                const struct snag_irc_destination *destination = &term->destinations->items[j];
                if (strcmp(destination->connection, connection)) continue;
                enum snag_irc_identity role =
                    !strcmp(identity, "operator") ? SNAG_IRC_OPERATOR : SNAG_IRC_AGENT;
                if (json_object_set_new(
                        route, "casemapping", json_integer(destination->casemapping[role])) < 0)
                    return -1;
                break;
            }
        }
        return view_state(display, command->data.voice);
    }
    case SNAG_UI_COMMAND_REPORT:
        return snag_view_server_report(display->view, command->data.voice, command->text);
    case SNAG_UI_COMMAND_RESULT: {
        const json_t *result = command->data.voice;
        uint64_t generation = 0u;
        if (json_is_true(json_object_get(result, "return_terminal")) &&
            snag_view_server_terminal_generation(
                display->view, snag_json_string(result, "id"), &generation) < 0)
            return -1;
        int rc = snag_view_server_command_result(display->view, result);
        if (!rc && generation)
            rc = session_control_generation(display, SNAG_SESSION_DETACH, NULL, 0u, generation);
        return rc;
    }
    case SNAG_UI_VIEW_RESULT: {
        const json_t *r = command->data.voice;
        uint64_t seq;
        if (snag_json_integer_u64(r, "seq", &seq) < 0) return -1;
        return snag_view_server_result(display->view, snag_json_string(r, "id"),
            snag_json_string(r, "status"), seq, snag_json_string(r, "event"));
    }
    case SNAG_UI_SESSION_START:
    case SNAG_UI_SESSION_DIRECT:
    case SNAG_UI_SESSION_LISTEN:
    case SNAG_UI_SESSION_CONTROL:
    case SNAG_UI_SESSION_REBIND:
    case SNAG_UI_SESSION_READY:
    case SNAG_UI_SESSION_OFFER:
    case SNAG_UI_SESSION_PROGRESS:
    case SNAG_UI_SESSION_REFUSE:
    case SNAG_UI_SESSION_RELEASED:
    case SNAG_UI_SESSION_BOUND:
        return apply_session(display, command);
    case SNAG_UI_LEVEL:
        return set_level(display, command->data.value);
    case SNAG_UI_CLOSE:
        snag_render_attach_term(render, NULL);
        display->direct_opened = false;
        input_stop(display);
        snag_term_close(term);
        return 0;
    case SNAG_UI_COLOR: {
        bool previous = render->color_stderr;
        snag_render_set_color(render, (enum snag_color_mode)command->data.value);
        return previous != render->color_stderr && display->prompt.source ? apply_prompt(display)
                                                                          : 0;
    }
    case SNAG_UI_MARKDOWN:
        snag_render_set_markdown(render, command->data.value != 0u);
        return 0;
    case SNAG_UI_IRC_NAMES:
        if (json_equal(term->irc_names, command->data.voice)) return 0;
        json_decref(term->irc_names);
        term->irc_names = json_incref((json_t *)command->data.voice);
        term->completion_armed = false;
        return view_state(display, display->view_state);
    case SNAG_UI_DESTINATIONS:
        if (snag_term_set_destinations(term, command->data.destinations) < 0) return -1;
        if (render->view == SNAG_RENDER_CHAT && !display->conversation) {
            const struct snag_irc_destination *destination = selected_destination(term);
            if (destination && display_set_chat_room(display, destination, false) < 0) return -1;
        }
        return display->prompt.source && !display->view_repainting ? apply_prompt(display) : 0;
    case SNAG_UI_SELECT:
        if (snag_term_select_destination(term, command->data.value) < 0) return -1;
        if (render->view == SNAG_RENDER_CHAT &&
            display_set_chat_room(display, selected_destination(term), true) < 0)
            return -1;
        return display->prompt.source && !display->view_repainting ? apply_prompt(display) : 0;
    case SNAG_UI_ROUTE:
        snag_term_destination_route(term, command->text, command->data.route);
        return 0;
    case SNAG_UI_CONVERSATION:
        return display_conversation_event(display, command->data.irc);
    case SNAG_UI_CONVERSATION_SELECT:
        for (struct ui_conversation_tab *tab = display->conversations; tab; tab = tab->next) {
            if (strcmp(tab->target.conversation, command->data.conversation->conversation))
                continue;
            tab->target = *command->data.conversation;
            if (display_select_draft(display, tab, NULL) < 0) return -1;
            term->conversation = tab->target;
            if (snag_render_set_chat_conversation(
                    render, tab->endpoint, &term->conversation, true) < 0)
                return -1;
            return display_set_view(display, SNAG_RENDER_CHAT, true, true);
        }
        return snag_errno(ENOENT);
    case SNAG_UI_COMMANDS:
        snag_term_set_commands(term, command->data.commands.items, command->data.commands.count);
        return 0;
    case SNAG_UI_PAUSE:
        snag_term_set_typing_pause(term, command->data.timing.typing_pause_ms);
        term->animation.tool_delay_ms = command->data.timing.tool_spinner_off_delay_ms;
        if (!term->animation.tool_delay_ms) term->animation.tool_until = 0u;
        return 0;
    case SNAG_UI_OPEN:
        if (display->direct) {
            display->direct_opened = true;
            return 0;
        }
        if (snag_term_open(term, error, error_size) < 0) return -1;
        if (input_start(display, NULL, 0u) < 0) {
            (void)snprintf(
                error, error_size, "cannot start terminal input worker: %s", strerror(errno));
            snag_term_close(term);
            return -1;
        }
        snag_render_attach_term(render, term);
        return 0;
    case SNAG_UI_EXTERNAL: {
        if (display->direct) return snag_errno(ENOTSUP);
        int rc;
        if (command->data.value) {
            if (snag_render_suspend(render, true) < 0) return -1;
            input_stop(display);
            rc = snag_term_external_begin(term, error, error_size);
            if (rc < 0) {
                (void)snag_render_suspend(render, false);
                (void)input_start(display, NULL, 0u);
            }
        } else {
            rc = snag_term_external_end(term, error, error_size);
            if (rc == 0) {
                (void)snag_render_suspend(render, false);
                /* Transfers can return keyboard typeahead before the engine
                 * finishes admitting the file. Only an acknowledged prompt
                 * may release that input; restoring terminal modes cannot. */
                display->view_repainting = term->prompt_wanted;
                term->defer_redraw = display->native_barrier || display->view_repainting;
            }
            if (rc == 0 && command->len && (!command->text || command->len > UI_INPUT_CAPACITY))
                rc =
                    snag_errorf(error, error_size, "transfer input tail exceeds terminal capacity");
            /* input_start resets the ring; seed the tail after that reset but
             * before the restarted worker can read newer terminal bytes. */
            if (rc == 0 &&
                input_start(display, (const unsigned char *)command->text, command->len) < 0)
                rc = snag_errorf(
                    error, error_size, "cannot restart terminal input worker: %s", strerror(errno));
        }
        if (rc == 0) display->suspended = command->data.value != 0u;
        return rc;
    }
    case SNAG_UI_HOLD:
        if (snag_term_hide(term) < 0) return -1;
        if (command->data.value && !term->active) ++display->turn_generation;
        term->active = command->data.value != 0u;
        term->prompt_wanted = false;
        return 0;
    case SNAG_UI_VALIDATE: {
        const char *frames[SNAG_TERM_SPINNER_COUNT];
        struct snag_term probe;
        int rc;
        for (size_t i = 0u; i < SNAG_TERM_SPINNER_COUNT; ++i)
            frames[i] = command->data.prompt.frames[i];
        snag_term_init(&probe);
        rc = snag_term_set_prompt_template(&probe, false, command->text, frames,
            command->data.prompt.rate, (1u << SNAG_TERM_SPINNER_COUNT) - 1u);
        snag_term_close(&probe);
        return rc;
    }
    case SNAG_UI_SPINNERS:
        display->prompt.states = command->data.value;
        if (snag_term_set_spinner_states(term, command->data.value) < 0) return -1;
        return view_state(display, display->view_state);
    case SNAG_UI_DRAFT:
        return snag_term_restore_draft(term, command->text);
    case SNAG_UI_INPUT_DRAFT:
        return display_restore_input(display, command);
    case SNAG_UI_INSERT: {
        int rc = snag_term_insert_draft(term, command->text);
        return rc < 0 && (errno == EOVERFLOW || errno == EILSEQ) ? 1 : rc;
    }
    case SNAG_UI_AUDIO: {
        bool voice = command->data.value == 2u;
        if (command->text[0] && display->native &&
            !atomic_load(&display->runtime->session_attachment))
            return 1;
        if (voice && (!term->opened || !term->raw || !term->capable || term->input_only ||
                         term->output_depth))
            return 1;
        int rc = snag_term_audio(term, command->text, command->data.value == 1u);
        return rc < 0 && errno == ENOTTY ? 1 : rc;
    }
    case SNAG_UI_CAPTION:
        return snag_term_caption(term, command->data.value, command->text);
    case SNAG_UI_VIEW:
        if (!term->opened) {
            struct ui_channel_draft *draft =
                command->data.value == SNAG_RENDER_CHAT
                    ? display_channel_draft(display, term->destination.id)
                    : &display->rollout_draft;
            if (display_select_draft(display, NULL, draft) < 0) return -1;
            render->view = (enum snag_render_view)command->data.value;
            term->chat = render->view == SNAG_RENDER_CHAT;
            atomic_store(&display->runtime->view, command->data.value);
            return 0;
        }
        return display_set_view(display, (enum snag_render_view)command->data.value, false, true);
    case SNAG_UI_HISTORY_SNAPSHOT:
        return snag_term_history_set(
            term, &command->data.history.entries, command->data.history.refresh);
    case SNAG_UI_UPDATE:
        if (display->direct) return 0;
        if (!display->update)
            display->update =
                snag_update_start(command->label, command->text, display->runtime->commands[1]);
        return 0;
    case SNAG_UI_INPUT:
        return admit_input(display, command->text, command->data.seq);
    case SNAG_UI_STOP:
        return 0;
    default:
        break; /* Output was applied above; raw bytes are sliced by apply_display. */
    }
    return snag_errno(EINVAL);
}

static int
finish_input(struct snag_ui_display *display, struct ui_action *item, int rc)
{
    struct snag_ui_runtime *runtime = display->runtime;
    struct snag_term *term = &display->term;

    item->history_warning = term->history_reader.warning;
    term->history_reader.warning = false;
    if (item->text) item->received_ms = snag_time_ms();
    if (item->action == SNAG_TERM_VIEW) {
        if (display_cycle_view(display) < 0) goto fail;
        item->action = SNAG_TERM_NONE;
    }
    take_snapshot(display, &item->snapshot, item->text);
    /* The presentation owner retains commands before engine work can stall.
     * Their later acknowledgements use this flag to avoid a second echo. */
    if (rc > 0 && item->action == SNAG_TERM_SUBMIT && !term->input_only &&
        snag_prompt_command(item->text)) {
        if (present_local(display, SNAG_UI_SUBMITTED, item->snapshot.label, item->text, true,
                &item->snapshot) < 0)
            goto fail;
        item->submission_echoed = true;
    }
    /* A /yield completed as the prompt becomes held uses the same priority
     * path as a line found in the held input ring. */
    if (rc > 0 && item->action == SNAG_TERM_SUBMIT && item->text && !strcmp(item->text, "/yield") &&
        (term->animation.states & (1u << SNAG_TERM_SPINNER_TOOL))) {
        atomic_store(&runtime->yield_requested, item->submission_echoed ? 2u : 1u);
        snag_wakeup_send(runtime->actions.wake[1]);
        free(item->text);
        free(item);
        return 0;
    }
    if (item->text && item->action != SNAG_TERM_UPLOAD)
        snag_term_destination_route(term, item->text, &item->route);
    if (item->action == SNAG_TERM_INTERRUPT) term->interrupt_pending = true;
    if (item->action == SNAG_TERM_CANCEL || item->action == SNAG_TERM_INTERRUPT) {
        bool deferred = term->defer_redraw;
        term->defer_redraw =
            deferred || item->action == SNAG_TERM_SUBMIT || item->action == SNAG_TERM_QUEUE;
        if (!term->input_only && display->prompt.source && !display->view_repainting &&
            apply_prompt(display) < 0)
            goto fail;
        term->defer_redraw = deferred;
    }
    atomic_store(&runtime->pause_until,
        term->typing_active ? term->last_input_ms + term->typing_pause_ms : 0u);
    if (rc < 0 && errno != EINTR) {
        item->error = errno;
        item->input_error = true;
    }
    if (term->history_refresh_requested && !term->input_backlog) {
        term->history_refresh_requested = false;
        item->history_refresh = true;
    }
    if (item->action == SNAG_TERM_SUBMIT && item->text) {
        uint32_t id;
        size_t body;
        enum snag_irc_target_command command =
            snag_irc_target_parse(item->text, strlen(item->text), &id, &body);
        if (term->blank_local && snag_text_blank(item->text)) {
            display->feedback[0] = '\0';
            if (!term->input_only && display->prompt.source && !display->view_repainting &&
                apply_prompt(display) < 0)
                goto fail;
            item->local = true;
        } else if (command == SNAG_IRC_TARGET_SELECT) {
            if (snag_term_select_destination(term, id) == 0) {
                (void)snprintf(display->feedback, sizeof(display->feedback), "destination: %u", id);
                if (display->render.view == SNAG_RENDER_CHAT &&
                    display_set_chat_room(display, selected_destination(term), true) < 0)
                    goto fail;
            } else
                (void)snprintf(display->feedback, sizeof(display->feedback),
                    "destination %u is unavailable; use /names", id);
            if (!term->input_only && display->prompt.source && !display->view_repainting &&
                apply_prompt(display) < 0)
                goto fail;
            take_snapshot(display, &item->snapshot, item->text);
            item->local = true;
        } else if (!strcmp(item->text, "/chat") || !strcmp(item->text, "/rollout")) {
            enum snag_render_view view =
                !strcmp(item->text, "/chat") ? SNAG_RENDER_CHAT : SNAG_RENDER_ROLLOUT;
            if (view == SNAG_RENDER_CHAT) {
                const struct snag_irc_destination *destination = selected_destination(term);
                if (!destination && term->destinations && term->destinations->count) {
                    destination = &term->destinations->items[0];
                    if (snag_term_select_destination(term, destination->target.id) < 0) goto fail;
                }
                if (display_set_chat_room(display, destination, false) < 0) goto fail;
            }
            /* Switch immediately in the presentation owner, but leave the
             * submitted command for the engine. It owns command semantics
             * such as the offline status and will acknowledge readiness with
             * SNAG_UI_VIEW plus a fresh prompt. */
            if (display_set_view(display, view, true, false) < 0) goto fail;
            item->view_applied = true;
            take_snapshot(display, &item->snapshot, item->text);
        } else if (snag_verbosity_command(item->text, strlen(item->text)) &&
            !snag_command_chained(item->text, strlen(item->text))) {
            const char *value = item->text + 8u;
            while (isspace((unsigned char)*value)) ++value;
            /* Active queries remain responsive while the engine is held. */
            if (*value || item->snapshot.active) {
                verbosity_command(display, item->text);
                item->local = true;
            }
        }
    }
    if (item->local) {
        item->action = SNAG_TERM_NONE;
        term->prompt_wanted = true;
        snag_term_trace(term, "want-true", "read_input-local");
        display->local = item;
        display->local_acknowledged = false;
        return 0;
    }
    if (item->action == SNAG_TERM_DICTATE_DONE || item->action == SNAG_TERM_DICTATE_CANCEL) {
        if (item->action == SNAG_TERM_DICTATE_CANCEL)
            atomic_store(&runtime->dictation_control, (unsigned int)item->action);
        else {
            unsigned int empty = 0u;
            (void)atomic_compare_exchange_strong(
                &runtime->dictation_control, &empty, (unsigned int)item->action);
        }
    } else if (item->action == SNAG_TERM_INTERRUPT) {
        atomic_store(&runtime->interrupt, display->turn_generation);
    } else if (item->action == SNAG_TERM_EXIT) {
        atomic_store(&runtime->exit_requested, true);
        display->input_closed = true;
    } else if (item->action == SNAG_TERM_CANCEL && term->input_backlog) {
        atomic_store(&runtime->cancel, true);
    } else if (item->action != SNAG_TERM_NONE || item->local || item->history_refresh ||
               item->history_warning || item->error) {
        if (item->action == SNAG_TERM_SUBMIT || item->action == SNAG_TERM_QUEUE) {
            /* The action is admitted to the engine, but it has not finished.
             * Keep subsequent typeahead in the input ring until the engine
             * sends an explicit readiness prompt. */
            if (item->action == SNAG_TERM_SUBMIT && item->snapshot.active &&
                item->snapshot.view == SNAG_RENDER_ROLLOUT && item->text &&
                (item->text[0] != '/' || item->text[1] == '/')) {
                item->steering = true;
                atomic_fetch_add(&runtime->steering_pending, 1u);
            }
        }
        /* Keep an active rollout steer visible even when the engine cannot
         * accept another composer yet. */
        if (item->action == SNAG_TERM_SUBMIT && item->text && item->snapshot.active &&
            item->snapshot.view == SNAG_RENDER_ROLLOUT && !item->submission_echoed &&
            (item->text[0] != '/' || item->text[1] == '/') && !queue_full(&runtime->actions)) {
            if (snag_render_input_submitted(&display->render, item->snapshot.label, item->text) < 0)
                goto fail;
            item->submission_echoed = true;
        }
        if (queue_push(&runtime->actions, item)) return 0;
        atomic_store(&runtime->fatal, item->error ? item->error : EOVERFLOW);
    }
    {
        bool notify = item->action != SNAG_TERM_NONE || item->error;
        free(item->text);
        free(item);
        if (notify) snag_wakeup_send(runtime->actions.wake[1]);
    }
    return 0;
fail:
    free(item->text);
    free(item);
    return -1;
}

static int
admit_input(struct snag_ui_display *display, const char *text, uint64_t attachment)
{
    struct snag_ui_runtime *runtime = display->runtime;
    if (!text || !snag_text_valid(text, 0u, SNAG_MAX_DIRECT_PROMPT)) return snag_errno(EINVAL);
    if (!display->term.opened || display->suspended || display->input_closed ||
        display->native_barrier || atomic_load(&runtime->session_attachment) != attachment ||
        (display->native && !attachment)) {
        return snag_errno(ESTALE);
    }
    if (queue_full(&runtime->actions) || display->local) return snag_errno(EAGAIN);
    struct ui_action *item = calloc(1u, sizeof(*item));
    if (!item) return -1;
    item->text = strdup(text);
    if (!item->text) {
        free(item);
        return -1;
    }
    item->action = SNAG_TERM_SUBMIT;
    item->attachment = attachment;
    item->interface_input = true;
    return finish_input(display, item, 1);
}

static int local_feedback(struct snag_ui_display *display);

static int
read_input(struct snag_ui_display *display, int timeout_ms)
{
    if (session_service(display, 0) < 0) return -1;
    if ((display->native || display->direct) && (timeout_ms < 0 || timeout_ms > 16))
        timeout_ms = 16;
    struct snag_ui_runtime *runtime = display->runtime;
    struct snag_term *term = &display->term;
    struct ui_action *item;
    int rc;

    /* Output checkpoints can leave a local submission waiting to be painted.
     * Settle it before reading the next Enter from the same input burst. */
    if (!term->input_only && local_feedback(display) < 0) return -1;
    /* A presentation refresh can repaint the label before the engine finishes
     * an action. Only its readiness prompt releases subsequent input. */
    bool held = display->painting_feedback ||
                ((!term->prompt_wanted || term->submit_awaiting_activity) &&
                    !term->dictating && !term->input_only);
    if (term->opened && !display->suspended && !display->input_closed && held) {
        enum held_control control = input_take_held_control(
            &runtime->input, (term->animation.states & (1u << SNAG_TERM_SPINNER_TOOL)) != 0u);

        if (control == HELD_EXIT) atomic_store(&runtime->exit_requested, true);
        if (control == HELD_INTERRUPT)
            atomic_store(&runtime->interrupt,
                display->turn_generation ? display->turn_generation : UINT64_MAX);
        if (control == HELD_YIELD) {
            if (!term->input_only &&
                present_local(display, SNAG_UI_SUBMITTED, term->label, "/yield", true, NULL) < 0) {
                return -1;
            }
            atomic_store(&runtime->yield_requested, term->input_only ? 1u : 2u);
        }
        if (control != HELD_NONE) {
            snag_wakeup_send(runtime->actions.wake[1]);
            return 0;
        }
    }
    if (!term->opened || display->suspended || display->input_closed || held ||
        display->native_barrier) {
        rc = snag_wakeup_wait(runtime->commands[0], timeout_ms);
        return rc < 0 && errno != EINTR ? -1 : 0;
    }
    term->input_backlog = queue_full(&runtime->actions);
    term->local_backlog = display->local != NULL;
    if (!term->input_backlog)
        display->backlog_warned = false;
    else if (!display->backlog_warned && !term->input_only) {
        display->backlog_warned = true;
        if (snag_render_warning_ctx(
                &display->render, "input backlog is full; draft retained, retry Enter shortly") < 0)
            return -1;
    }
    item = calloc(1u, sizeof(*item));
    if (!item) return -1;
    rc = snag_term_poll(term, timeout_ms, runtime->commands[0], &item->action, &item->text);
    return finish_input(display, item, rc);
}

static int
local_feedback(struct snag_ui_display *display)
{
    struct ui_action *item = display->local;
    int rc = 0;

    if (!item || display->suspended || display->painting_feedback) return 0;
    if (!display->local_acknowledged) {
        display->painting_feedback = true;
        if (!item->submission_echoed) {
            rc = present_local(display, SNAG_UI_SUBMITTED, item->snapshot.label, item->text, false,
                &item->snapshot);
        }
        if (rc == 0 && display->feedback[0])
            rc = present_local(
                display, SNAG_UI_HOST, NULL, display->feedback, false, &item->snapshot);
        if (rc == 0) memcpy(item->feedback, display->feedback, sizeof(item->feedback));
        display->painting_feedback = false;
        display->local_acknowledged = true;
    }
    if (snag_text_blank(item->text)) {
        free(item->text);
        free(item);
        display->local = NULL;
    } else if (queue_push(&display->runtime->actions, item))
        display->local = NULL;
    return rc;
}

static int
output_input_checkpoint(void *opaque)
{
    struct snag_ui_display *display = opaque;
    int rc = read_input(display, 0);
    if (rc < 0) return -1;
    return atomic_load(&display->runtime->exit_requested) ? snag_errno(ECANCELED) : 0;
}

static int
render_input_checkpoint(void *opaque)
{
    struct snag_ui_display *display = opaque;
    int rc = read_input(display, 0);
    display->render.suppress_optional = atomic_load(&display->runtime->exit_requested) ||
                                        atomic_load(&display->runtime->interrupt) ||
                                        atomic_load(&display->runtime->steering_pending);
    if (rc < 0) return -1;
    if (atomic_load(&display->runtime->exit_requested)) return snag_errno(ECANCELED);
    return local_feedback(display);
}

static bool
public_stopped(struct snag_ui_runtime *runtime)
{
    return atomic_load(&runtime->exit_requested) || atomic_load(&runtime->interrupt) ||
           atomic_load(&runtime->steering_pending);
}

static int
display_prompt(struct snag_ui_display *display, struct ui_message *message)
{
    struct snag_term *term = &display->term;
    struct snag_ui_command *command = &message->command;
    if (!display->direct && !snag_view_server_attached(display->view)) {
        term->defer_redraw = true;
        struct snag_ui_command boundary = {.kind = SNAG_UI_BEFORE_PROMPT};
        if (snag_presentation_apply(&display->render, &boundary, NULL) < 0) return -1;
        term->defer_redraw = false;
    }
    if (command->data.prompt.active && !term->active) ++display->turn_generation;
    /* After consuming Ctrl-C the owner can acknowledge an editor-only
     * cancellation with another active prompt; the model turn continues. */
    if (!command->data.prompt.active ||
        atomic_load(&display->runtime->interrupt) != display->turn_generation)
        term->interrupt_pending = false;
    prompt_free(&display->prompt);
    display->prompt = command->data.prompt;
    memset(&command->data.prompt, 0, sizeof(command->data.prompt));
    /* A queued readiness prompt cannot acknowledge input received after
     * the engine sent it. The action queue owns this ordering. */
    if (message->input_tail == atomic_load(&display->runtime->actions.head))
        term->submit_awaiting_activity = false;
    if (display->view_repainting) {
        term->defer_redraw = true;
        return 0;
    }
    return apply_prompt(display);
}

static int
apply_display(struct snag_ui_display *display, struct ui_message *message)
{
    struct snag_ui_runtime *runtime = display->runtime;
    size_t offset = 0u;
    bool raw = message->command.kind == SNAG_UI_RAW;
    if (message->command.kind == SNAG_UI_PROMPT && message->command.label) {
        /* Dispatch gets a fresh label without replacing a live draft/clock. */
        struct snag_term submitted;
        snag_term_init(&submitted);
        submitted.defer_redraw = true;
        int rc =
            snag_term_configure_prompt(&submitted, &message->command.data.prompt, &display->term);
        struct snag_ui_command echo = {
            .kind = SNAG_UI_SUBMITTED, .label = submitted.label, .text = message->command.label};
        if (!rc) rc = retain_output(display, &echo);
        if (!rc && !display->direct && !snag_view_server_attached(display->view))
            rc = snag_presentation_apply(&display->render, &echo, NULL);
        snag_term_close(&submitted);
        return rc;
    }
    if (message->command.kind == SNAG_UI_PROMPT && !message->command.label) {
        struct snag_ui_command boundary = {.kind = SNAG_UI_BEFORE_PROMPT};
        if (retain_output(display, &boundary) < 0) return -1;
        const struct snag_term_prompt *old = &display->prompt;
        const struct snag_term_prompt *next = &message->command.data.prompt;
        message->prompt_changed =
            !display->term.prompt_wanted || old->active != next->active ||
            old->mode != next->mode ||
            strcmp(old->source ? old->source : "", next->source ? next->source : "");
        for (size_t i = 0u; i < SNAG_PROMPT_HOUR && !message->prompt_changed; ++i) {
            message->prompt_changed = strcmp(old->values[i] ? old->values[i] : "",
                                          next->values[i] ? next->values[i] : "") != 0;
        }
    } else if (message->command.kind == SNAG_UI_HOLD || message->command.kind == SNAG_UI_CLOSE) {
        message->prompt_changed = display->term.prompt_wanted;
    }

    if (retain_output(display, &message->command) < 0) return -1;
    if (message->command.retain_only) return 0;
    if (message->command.kind == SNAG_UI_PROMPT) return display_prompt(display, message);

    if (display->direct || snag_view_server_attached(display->view)) {
        switch (message->command.kind) {
        case SNAG_UI_IRC:
            return display_conversation_event(display, message->command.data.irc);
        case SNAG_UI_HOST:
        case SNAG_UI_HELP:
        case SNAG_UI_RUNTIME:
        case SNAG_UI_ERROR:
        case SNAG_UI_WARNING:
        case SNAG_UI_ROLLOUT_END:
        case SNAG_UI_ROLLOUT_ABORT:
        case SNAG_UI_SUBMITTED:
        case SNAG_UI_CHOICES:
        case SNAG_UI_BEFORE_PROMPT:
        case SNAG_UI_PUBLIC_BEGIN:
        case SNAG_UI_PUBLIC:
        case SNAG_UI_ORIENTATION:
        case SNAG_UI_HISTORY:
        case SNAG_UI_DURABLE:
        case SNAG_UI_EVENT:
        case SNAG_UI_RESUME:
        case SNAG_UI_PROTOCOL:
        case SNAG_UI_TRANSPORT:
        case SNAG_UI_RAW:
        case SNAG_UI_VOICE_EVENT:
        case SNAG_UI_CAPTION:
            return 0;
        default:
            break;
        }
    }

    display->render.suppress_optional = public_stopped(runtime);
    if (!raw && message->command.kind != SNAG_UI_PUBLIC)
        return apply_message(display, &message->command, message->error, sizeof(message->error));
    while (offset < message->command.len) {
        size_t amount = message->command.len - offset;
        if (amount > 1024u) amount = 1024u;
        if (raw && amount < message->command.len - offset) {
            size_t boundary = amount;
            while (boundary &&
                   ((unsigned char)message->command.text[offset + boundary] & 0xc0u) == 0x80u)
                --boundary;
            if (boundary) amount = boundary;
        }
        if (!raw && public_stopped(runtime)) return 0;
        if (render_input_checkpoint(display) < 0) return -1;
        while (!raw && !public_stopped(runtime) &&
               snag_term_typing_pause_remaining(&display->term, snag_monotonic_ms()))
            if (read_input(display, 16) < 0) return -1;
        if (!raw && public_stopped(runtime)) return 0;
        if (raw ? snag_term_write(
                      (int)message->command.data.value, message->command.text + offset, amount) < 0
                : snag_render_rollout(&display->render, message->command.text + offset, amount,
                      &message->delivered) < 0)
            return -1;
        offset += amount;
    }
    return 0;
}

/* An input-shaped error describes the data that just arrived, not a broken
 * terminal. The engine reports it once and keeps reading keystrokes, so the
 * presenter must not close terminal input for it: a latched close left the
 * composer echoing nothing and consuming no keystroke, including Ctrl-C, while
 * the engine kept running. Only a real terminal failure closes input. */
static bool
input_condition(int error)
{
    return error == EOVERFLOW || error == EILSEQ;
}

static void *
presentation_main(void *opaque)
{
    struct snag_ui_display *display = opaque;
    struct snag_ui_runtime *runtime = display->runtime;

    snag_term_init(&display->term);
    snag_buf_init(&display->rollout_draft.text, SNAG_MAX_DIRECT_PROMPT + 1u);
    display->main_draft = &display->rollout_draft;
    display->term.input_checkpoint = output_input_checkpoint;
    display->term.input_opaque = display;
    display->term.feedback = editor_feedback;
    display->term.feedback_opaque = display;
    snag_render_init(&display->render, 0u);
    display->render.checkpoint = render_input_checkpoint;
    display->render.checkpoint_opaque = display;
    (void)snag_render_backfill_start(&display->render, runtime->commands[1]);
    (void)snag_term_signals_unblock();
    for (;;) {
        struct ui_message *message;
        snag_wakeup_drain(runtime->commands[0]);
        if (snag_render_backfill_collect(&display->render) < 0)
            atomic_store(&runtime->fatal, errno ? errno : EIO);
        message = atomic_exchange_explicit(&runtime->request, NULL, memory_order_acquire);
        const char *banner = display->suspended ? NULL : snag_update_take(display->update);
        bool runnable = !display->direct && !snag_view_server_attached(display->view) &&
                        snag_render_view_runnable(&display->render);
        if (banner) (void)snag_render_update(&display->render, banner);
        if (read_input(display, message || banner || runnable ? 0 : -1) < 0) {
            int error = errno ? errno : EIO;
            atomic_store(&runtime->fatal, error);
            if (!input_condition(error)) display->input_closed = true;
            snag_wakeup_send(runtime->actions.wake[1]);
        }
        if (local_feedback(display) < 0) atomic_store(&runtime->fatal, errno ? errno : EIO);
        if (display->native_notice[0] && !display->suspended && !display->native_barrier) {
            char notice[sizeof(display->native_notice)];
            memcpy(notice, display->native_notice, sizeof(notice));
            display->native_notice[0] = '\0';
            if (snag_render_warning_ctx(&display->render, notice) < 0)
                atomic_store(&runtime->fatal, errno ? errno : EIO);
        }
        if (message) {
            /* A short delivery may release a citation held by the renderer. */
            snag_buf_init(&message->delivered, message->command.len + 4u + SNAG_CITE_BLOCK_MAX);
            message->result = apply_display(display, message);
            message->saved_errno = errno;
            if (message->result < 0 && message->command.kind != SNAG_UI_VALIDATE &&
                !(message->command.kind == SNAG_UI_INPUT &&
                    (message->saved_errno == ESTALE || message->saved_errno == EAGAIN ||
                        message->saved_errno == EINVAL)) &&
                !session_command(message->command.kind) &&
                !(message->saved_errno == ECANCELED && atomic_load(&runtime->exit_requested))) {
                int error = errno ? errno : EIO;
                atomic_store(&runtime->fatal, error);
                if (!input_condition(error)) display->input_closed = true;
            }
            take_snapshot(display, &message->snapshot, message->command.text);
            atomic_store(&runtime->view, (unsigned int)display->render.view);
            {
                bool stop = message->command.kind == SNAG_UI_STOP;
                atomic_store_explicit(&message->done, true, memory_order_release);
                snag_wakeup_send(runtime->actions.wake[1]);
                if (stop) break;
            }
        }
        if (!display->direct && !display->suspended && !display->native_barrier &&
            !snag_view_server_attached(display->view) &&
            snag_render_view_pending(&display->render) &&
            snag_render_flush_pending(&display->render, UI_RENDER_BATCH) < 0) {
            atomic_store(&runtime->fatal, errno ? errno : EIO);
            display->input_closed = true;
            snag_wakeup_send(runtime->actions.wake[1]);
        }
        if (!display->suspended && display->view_repainting &&
            !snag_render_view_pending(&display->render)) {
            display->view_repainting = false;
            display->term.defer_redraw = display->native_barrier;
            if (!display->native_barrier && display->prompt.source && apply_prompt(display) < 0) {
                atomic_store(&runtime->fatal, errno ? errno : EIO);
                display->input_closed = true;
                snag_wakeup_send(runtime->actions.wake[1]);
            }
        }
    }
    bool hard_exit;
    (void)pthread_mutex_lock(&runtime->input.lock);
    hard_exit = runtime->input.hard_exit_deadline_ms != 0u;
    (void)pthread_mutex_unlock(&runtime->input.lock);
    char *banner = snag_update_stop(display->update);
    if (banner) {
        (void)snag_render_update(&display->render, banner);
        free(banner);
    }
    snag_presentation_writer_close(display->presentation);
    snag_render_free(&display->render);
    if (display->local) {
        free(display->local->text);
        free(display->local);
    }
    input_stop(display);
    if (hard_exit)
        snag_term_abort(&display->term);
    else
        snag_term_close(&display->term);
    snag_view_server_close(display->view);
#if SNAJPAGENT_VM
    if (display->direct) snag_view_channel_close(&display->direct_channel);
#endif
    if (display->native) {
        snag_session_listener_close(&display->listener);
        snag_session_relay_close(&display->relay);
        snag_session_process_close(&display->native_process);
    }
    while (display->conversations) {
        struct ui_conversation_tab *tab = display->conversations;
        display->conversations = tab->next;
        snag_buf_free(&tab->draft);
        free(tab);
    }
    while (display->channel_drafts) {
        struct ui_channel_draft *draft = display->channel_drafts;
        display->channel_drafts = draft->next;
        snag_buf_free(&draft->text);
        free(draft);
    }
    snag_buf_free(&display->rollout_draft.text);
    json_decref(display->view_state);
    prompt_free(&display->prompt);
    free(display);
    return NULL;
}

static int
request(struct snag_ui *ui, struct ui_message *message, struct snag_buf *delivered, char *error,
    size_t error_size)
{
    int rc = -1, saved;
    struct snag_ui_runtime *runtime = ui->runtime;
    assert(pthread_equal(pthread_self(), runtime->engine));
    atomic_init(&message->done, false);
    if (message->command.len > SIZE_MAX - 4u - SNAG_CITE_BLOCK_MAX) {
        errno = EOVERFLOW;
        goto out;
    }
    message->input_tail = atomic_load(&runtime->actions.tail);
    atomic_store_explicit(&runtime->request, message, memory_order_release);
    snag_wakeup_send(runtime->commands[1]);
    for (;;) {
        snag_wakeup_drain(runtime->actions.wake[0]);
        if (atomic_load_explicit(&message->done, memory_order_acquire)) break;
        (void)snag_wakeup_wait(runtime->actions.wake[0], -1);
    }
    rc = message->result;
    adopt_snapshot(ui, &message->snapshot);
    if (error && error_size) (void)snprintf(error, error_size, "%s", message->error);
    if (delivered && message->delivered.len &&
        snag_buf_append(delivered, message->delivered.data, message->delivered.len) < 0)
        rc = -1;
    if (rc == 0 && ui->observe && !message->command.retain_only) {
        const char *kind = NULL;
        switch (message->command.kind) {
        case SNAG_UI_HOST:
            kind = "host";
            break;
        case SNAG_UI_HELP:
            kind = "help";
            break;
        case SNAG_UI_ERROR:
            kind = "error";
            break;
        case SNAG_UI_WARNING:
            kind = "warning";
            break;
        default:
            break;
        }
        if (kind && message->command.text)
            ui->observe(ui->observe_opaque, kind, message->command.text, NULL);
        if (message->prompt_changed)
            ui->observe(ui->observe_opaque, ui->prompt_wanted ? "prompt" : "prompt_closed",
                ui->label, NULL);
    }
    snag_buf_free(&message->delivered);
    if (rc < 0 && message->saved_errno) errno = message->saved_errno;
out:
    saved = errno;
    message_free(message);
    errno = saved;
    return rc;
}

static int
send_message(struct snag_ui *ui, struct ui_message *message, const char *text)
{
    message->command.text = text;
    message->command.len = text ? strlen(text) : 0u;
    return request(ui, message, NULL, NULL, 0u);
}

static int
send_input_message(struct snag_ui *ui, struct ui_message *message, const char *text)
{
    json_t *route = ui->input_view == SNAG_RENDER_CHAT ?
        snag_irc_conversation_route(&ui->input_conversation) : json_null();
    if (!route) return -1;
    message->command.route = route;
    int rc = send_message(ui, message, text);
    json_decref(route);
    return rc;
}

int
snag_ui_send(struct snag_ui *ui, struct snag_ui_command command)
{
    struct ui_message message = {.command = command};
    switch (command.kind) {
    case SNAG_UI_DRAFT:
    case SNAG_UI_INPUT_DRAFT:
    case SNAG_UI_EVENT:
    case SNAG_UI_DURABLE:
        return send_message(ui, &message, command.text);
    default:
        return request(ui, &message, NULL, NULL, 0u);
    }
}

int
snag_ui_restore_input(struct snag_ui *ui, const char *text)
{
    return snag_ui_send(ui, (struct snag_ui_command){.kind = SNAG_UI_INPUT_DRAFT,
                                .text = text,
                                .data.draft = {.view = ui->input_view,
                                    .destination = ui->input_destination,
                                    .conversation = ui->input_conversation.conversation}});
}

int
snag_ui_session_start(struct snag_ui *ui, struct snag_session_process *process)
{
    int rc = snag_ui_send(ui,
        (struct snag_ui_command){.kind = SNAG_UI_SESSION_START, .data.session_process = process});
    if (rc == 0) ui->native = true;
    return rc;
}

#if SNAJPAGENT_VM
int
snag_ui_session_direct(struct snag_ui *ui, struct snag_view_channel *channel)
{
    int rc = snag_ui_send(
        ui, (struct snag_ui_command){.kind = SNAG_UI_SESSION_DIRECT, .data.view_channel = channel});
    if (!rc) ui->direct = true;
    return rc;
}
#endif

int
snag_ui_session_listen(struct snag_ui *ui, const struct snag_session *session)
{
    int rc = snag_ui_send(
        ui, (struct snag_ui_command){.kind = SNAG_UI_SESSION_LISTEN, .data.session = session});
    if (!rc) {
        ui->view_listening = SNAJPAGENT_VM && session != NULL;
        ui->view_state_seq = UINT64_MAX;
        if (session) rc = snag_ui_view_state(ui, session);
    }
    return rc;
}

#if SNAJPAGENT_VM
static json_t *
connection_activity(const struct snag_session *session, const json_t *connection, uint64_t *seq)
{
    uint64_t latest = 0u, time = 0u, received = 0u, incoming = 0u;
    const char *id;
    const json_t *item;
    json_object_foreach(json_object_get(connection, "conversations"), id, item)
    {
        const json_t *routing = json_object_get(json_object_get(item, "data"), "routing");
        if (strcmp(snag_json_string(routing, "conversation_kind"), "connection")) continue;
        uint64_t at = (uint64_t)json_integer_value(json_object_get(item, "seq"));
        if (at > *seq) *seq = at;
        const json_t *activity =
            json_object_get(json_object_get(session->irc_activity, "items"), id);
        at = (uint64_t)json_integer_value(json_object_get(activity, "seq"));
        if (at > latest) {
            latest = at;
            time = (uint64_t)json_integer_value(json_object_get(activity, "time"));
        }
        received += (uint64_t)json_integer_value(json_object_get(activity, "received"));
        at = (uint64_t)json_integer_value(json_object_get(activity, "incoming"));
        if (at > incoming) incoming = at;
    }
    return latest
               ? json_pack("{s:I,s:I,s:I,s:I}", "seq", (json_int_t)latest, "time", (json_int_t)time,
                     "received", (json_int_t)received, "incoming", (json_int_t)incoming)
               : json_null();
}

static json_t *
view_conversations(const struct snag_session *session, enum snag_irc_conversation_kind wanted)
{
    json_t *queries = json_array();
    if (!queries) return NULL;
    const char *connection;
    json_t *entry;
    json_object_foreach(session->irc_conversations, connection, entry)
    {
        const char *conversation;
        json_t *item;
        json_object_foreach(json_object_get(entry, "conversations"), conversation, item)
        {
            const json_t *data = json_object_get(item, "data");
            const json_t *routing = json_object_get(data, "routing");
            const char *kind = snag_json_string(routing, "conversation_kind");
            if (!kind || strcmp(kind, wanted == SNAG_IRC_CHANNEL ? "channel"
                                      : wanted == SNAG_IRC_QUERY ? "query"
                                                                 : "connection"))
                continue;
            struct snag_irc_conversation_target target = {0};
            target.kind = wanted;
            target.casemapping = SNAG_IRC_CASE_UNKNOWN;
            (void)snag_strcpy(target.connection, sizeof(target.connection), connection);
            (void)snag_strcpy(target.conversation, sizeof(target.conversation), conversation);
            (void)snag_strcpy(target.peer, sizeof(target.peer), snag_json_string(routing, "peer"));
            (void)snag_strcpy(
                target.endpoint, sizeof(target.endpoint), snag_json_string(entry, "endpoint"));
            if (wanted == SNAG_IRC_CHANNEL) {
                const char *membership = snag_json_string(routing, "membership");
                if (!membership || !*membership) continue;
                (void)snag_strcpy(target.membership, sizeof(target.membership), membership);
                (void)snag_strcpy(target.room, sizeof(target.room), snag_json_string(data, "room"));
                (void)snag_strcpy(
                    target.endpoint, sizeof(target.endpoint), snag_json_string(entry, "endpoint"));
            }
            target.identity = !strcmp(snag_json_string(routing, "identity"), "operator")
                                  ? SNAG_IRC_OPERATOR
                                  : SNAG_IRC_AGENT;
            if (wanted == SNAG_IRC_CONNECTION_EVENTS && target.identity == SNAG_IRC_AGENT) continue;
            (void)snag_json_integer_u64(routing, "generation", &target.generation);
            json_t *route = snag_irc_conversation_route(&target);
            uint64_t seq = (uint64_t)json_integer_value(json_object_get(item, "seq"));
            json_t *activity =
                wanted == SNAG_IRC_CONNECTION_EVENTS
                    ? connection_activity(session, entry, &seq)
                    : json_incref(json_object_get(
                          json_object_get(session->irc_activity, "items"), conversation));
            if (!activity && wanted != SNAG_IRC_CONNECTION_EVENTS) activity = json_null();
            json_t *query =
                route && activity
                    ? json_pack("{s:O,s:O,s:O,s:I,s:O,s:O}", "route", route, "endpoint",
                          json_object_get(entry, "endpoint"), "connected",
                          json_object_get(entry, "connected"), "seq", (json_int_t)seq, "activity",
                          activity, "joined",
                          json_object_get(routing, "joined") ? json_object_get(routing, "joined")
                                                             : json_null())
                    : NULL;
            json_decref(activity);
            json_decref(route);
            if (!query || json_array_append_new(queries, query) < 0) {
                json_decref(queries);
                return NULL;
            }
        }
    }
    return queries;
}
#endif /* SNAJPAGENT_VM */

int
snag_ui_view_state(struct snag_ui *ui, const struct snag_session *session)
{
    if (!ui->view_listening || ui->view_state_seq == session->next_seq) return 0;
    json_t *state = json_pack("{s:I,s:I,s:s,s:i,s:b,s:s,s:s,s:s,s:s,s:s}", "seq",
        (json_int_t)(session->next_seq - 1u), "end", (json_int_t)session->log_end, "sha256",
        session->prev_sha256, "schema", (int)session->format_version, "active",
        session->active_turn, "provider", session->default_provider, "model",
        session->default_model, "effort", session->default_effort, "service_tier",
        session->service_tier ? session->service_tier : "", "name",
        session->name ? session->name : "");
    if (!state) return -1;
#if SNAJPAGENT_VM
    if (session->irc_activity && json_object_set(state, "irc_activity_after",
                                     json_object_get(session->irc_activity, "after")) < 0) {
        json_decref(state);
        return -1;
    }
    json_t *queries = view_conversations(session, SNAG_IRC_QUERY);
    if (!queries || json_object_set_new(state, "queries", queries) < 0) {
        json_decref(state);
        return -1;
    }
    json_t *channels = view_conversations(session, SNAG_IRC_CHANNEL);
    if (!channels || json_object_set_new(state, "channels", channels) < 0) {
        json_decref(state);
        return -1;
    }
    json_t *connections = view_conversations(session, SNAG_IRC_CONNECTION_EVENTS);
    if (!connections || json_object_set_new(state, "connections", connections) < 0) {
        json_decref(state);
        return -1;
    }
#endif
    int rc =
        snag_ui_send(ui, (struct snag_ui_command){.kind = SNAG_UI_VIEW_STATE, .data.voice = state});
    json_decref(state);
    if (!rc) ui->view_state_seq = session->next_seq;
    return rc;
}

int
snag_ui_view_result(
    struct snag_ui *ui, const char *id, const char *status, uint64_t seq, const char *event)
{
    if (!ui->view_listening || !id || !*id) return 0;
    json_t *result = json_pack("{s:s,s:s,s:I,s:s}", "id", id, "status", status, "seq",
        (json_int_t)seq, "event", event ? event : "");
    if (!result) return -1;
    int rc = snag_ui_send(
        ui, (struct snag_ui_command){.kind = SNAG_UI_VIEW_RESULT, .data.voice = result});
    json_decref(result);
    if (!strcmp(ui->view_request, id)) ui->view_request[0] = '\0';
    return rc;
}

int
snag_ui_command_result(struct snag_ui *ui, const json_t *result)
{
    if (!ui->view_listening) return 0;
    return snag_ui_send(
        ui, (struct snag_ui_command){.kind = SNAG_UI_COMMAND_RESULT, .data.voice = result});
}

int
snag_ui_command_report(struct snag_ui *ui, const json_t *report, const char *error)
{
    if (!ui->view_listening) return 0;
    return snag_ui_send(
        ui, (struct snag_ui_command){
                .kind = SNAG_UI_COMMAND_REPORT, .data.voice = report, .text = error});
}

int
snag_ui_session_control(
    struct snag_ui *ui, enum snag_session_message type, const void *data, size_t length)
{
    return snag_ui_send(ui, (struct snag_ui_command){.kind = SNAG_UI_SESSION_CONTROL,
                                .data.value = (unsigned int)type,
                                .text = data,
                                .len = length});
}

uint64_t
snag_ui_session_pending(const struct snag_ui *ui)
{
    return ui->native ? atomic_load(&ui->runtime->session_pending) : 0u;
}

uint64_t
snag_ui_session_attachment(const struct snag_ui *ui)
{
    return ui->native ? atomic_load(&ui->runtime->session_attachment) : 0u;
}

uint64_t
snag_ui_session_releasing(const struct snag_ui *ui)
{
    return ui->native ? atomic_load(&ui->runtime->session_releasing) : 0u;
}

uint64_t
snag_ui_session_failures(const struct snag_ui *ui)
{
    return ui->native ? atomic_load(&ui->runtime->session_failures) : 0u;
}

int
snag_ui_session_rebind(struct snag_ui *ui, uint64_t generation)
{
    return snag_ui_send(
        ui, (struct snag_ui_command){.kind = SNAG_UI_SESSION_REBIND, .data.seq = generation});
}

int
snag_ui_session_ready(struct snag_ui *ui, uint64_t generation)
{
    return snag_ui_send(
        ui, (struct snag_ui_command){.kind = SNAG_UI_SESSION_READY, .data.seq = generation});
}

int
snag_ui_init(struct snag_ui *ui)
{
    struct snag_ui_runtime *runtime;
    struct snag_ui_display *display;
    int rc;
    memset(ui, 0, sizeof(*ui));
    runtime = calloc(1u, sizeof(*runtime));
    if (!runtime) return -1;
    display = calloc(1u, sizeof(*display));
    if (!display) goto fail;
    display->runtime = runtime;
    atomic_init(&runtime->fatal, 0);
    atomic_init(&runtime->session_pending, 0u);
    atomic_init(&runtime->session_attachment, 0u);
    atomic_init(&runtime->session_releasing, 0u);
    atomic_init(&runtime->session_failures, 0u);
    runtime->engine = pthread_self();
    atomic_init(&runtime->interrupt, 0u);
    atomic_init(&runtime->exit_requested, false);
    atomic_init(&runtime->cancel, false);
    atomic_init(&runtime->hard_exit_acknowledged, false);
    atomic_init(&runtime->hard_exit_requested, false);
    atomic_init(&runtime->steering_pending, 0u);
    atomic_init(&runtime->dictation_control, 0u);
    atomic_init(&runtime->pause_until, 0u);
    atomic_init(&runtime->level, 0u);
    atomic_init(&runtime->view, SNAG_RENDER_ROLLOUT);
    atomic_init(&runtime->request, NULL);
    runtime->input.bytes = malloc(UI_INPUT_CAPACITY);
    if (!runtime->input.bytes) goto fail;
    rc = pthread_mutex_init(&runtime->input.lock, NULL);
    if (rc != 0) {
        errno = rc;
        goto input_buffer;
    }
    if (snag_wakeup_create(runtime->input.control) < 0) goto input_mutex;
    if (snag_wakeup_create(runtime->commands) < 0) goto input_control;
    if (queue_open(&runtime->actions) < 0) goto commands;
    if (snag_term_signals_block(&runtime->saved_mask) < 0) goto actions;
    rc = pthread_create(&runtime->thread, NULL, presentation_main, display);
    if (rc != 0) {
        (void)snag_term_signals_restore(&runtime->saved_mask);
        errno = rc;
        goto actions;
    }
    ui->runtime = runtime;
    ui->view = SNAG_RENDER_ROLLOUT;
    return 0;
actions:
    snag_wakeup_close(runtime->actions.wake);
commands:
    snag_wakeup_close(runtime->commands);
input_control:
    snag_wakeup_close(runtime->input.control);
input_mutex:
    (void)pthread_mutex_destroy(&runtime->input.lock);
input_buffer:
    free(runtime->input.bytes);
fail:
    free(display);
    free(runtime);
    return -1;
}

void
snag_ui_free(struct snag_ui *ui)
{
    struct snag_ui_runtime *runtime = ui->runtime;
    struct ui_message message = {.command = {.kind = SNAG_UI_STOP}};
    struct ui_action *action;
    if (!runtime) return;
    (void)send_message(ui, &message, NULL);
    (void)pthread_join(runtime->thread, NULL);
    while ((action = queue_pop(&runtime->actions))) {
        free(action->text);
        free(action);
    }
    snag_wakeup_close(runtime->actions.wake);
    snag_wakeup_close(runtime->commands);
    snag_wakeup_close(runtime->input.control);
    (void)pthread_mutex_destroy(&runtime->input.lock);
    free(runtime->input.bytes);
    (void)snag_term_signals_restore(&runtime->saved_mask);
    free(runtime);
    ui->runtime = NULL;
    snag_history_free(&ui->history);
}

int
snag_ui_update(struct snag_ui *ui, const char *program, const char *url)
{
    struct ui_message message = {.command = {.kind = SNAG_UI_UPDATE, .label = program}};
    return send_message(ui, &message, url);
}

int
snag_ui_text(struct snag_ui *ui, enum snag_ui_operation op, const char *text)
{
    if (op == SNAG_UI_ERROR) ui->command_error = true;
    if (ui->command_report && (op == SNAG_UI_HOST || op == SNAG_UI_HELP || op == SNAG_UI_ERROR ||
                                  op == SNAG_UI_WARNING || op == SNAG_UI_RUNTIME)) {
        size_t length = strlen(text);
        if (snag_term_append_safe(ui->command_report, text, length) < 0) return -1;
        int rc = length && text[length - 1u] == '\n' ? 0 : snag_buf_putc(ui->command_report, '\n');
        if (rc < 0) return rc;
        if (!ui->command_report_passthrough) {
            struct ui_message message = {.command = {.kind = op, .retain_only = true}};
            return send_input_message(ui, &message, text);
        }
    }
    struct ui_message message = {.command = {.kind = op}};
    return ui->command_report ? send_input_message(ui, &message, text) :
        send_message(ui, &message, text);
}

int
snag_ui_set_verbosity(struct snag_ui *ui, unsigned int level)
{
    struct ui_message message = {.command = {.kind = SNAG_UI_LEVEL, .data.value = level}};
    if (!snag_verbosity_name(level)) return snag_errno(EINVAL);
    return send_message(ui, &message, NULL);
}

unsigned int
snag_ui_verbosity(const struct snag_ui *ui)
{
    return ui && ui->runtime ? atomic_load(&ui->runtime->level) : 0u;
}

enum snag_render_view
snag_ui_view(const struct snag_ui *ui)
{
    return ui && ui->runtime ? (enum snag_render_view)atomic_load(&ui->runtime->view)
                             : SNAG_RENDER_ROLLOUT;
}

bool
snag_ui_enabled(const struct snag_ui *ui, enum snag_presentation kind)
{
    return ui && ui->runtime &&
           snag_presentation_enabled(kind, snag_ui_verbosity(ui), snag_ui_view(ui));
}

int
snag_ui_capture_route(struct snag_ui *ui, const char *text)
{
    struct ui_message message = {
        .command = {.kind = SNAG_UI_ROUTE, .data.route = &ui->input_route}};
    int rc = send_message(ui, &message, text);
    if (!rc) {
        ui->input_conversation = message.snapshot.conversation;
        ui->input_address_conversation = message.snapshot.address_conversation;
        ui->input_scopes = message.snapshot.scopes;
        ui->input_destination = message.snapshot.selection.id;
    }
    return rc;
}

uint32_t
snag_ui_pause_remaining(struct snag_ui *ui)
{
    uint64_t until = atomic_load(&ui->runtime->pause_until);
    uint64_t now = snag_monotonic_ms();
    if (public_stopped(ui->runtime)) return 0u;
    return until > now ? (uint32_t)(until - now) : 0u;
}

int
snag_ui_open(struct snag_ui *ui, char *error, size_t error_size)
{
    struct ui_message message = {.command = {.kind = SNAG_UI_OPEN}};
    if (ui->opened) return 0;
    return request(ui, &message, NULL, error, error_size);
}

int
snag_ui_external(struct snag_ui *ui, bool begin, char *error, size_t error_size)
{
    struct ui_message message = {.command = {.kind = SNAG_UI_EXTERNAL, .data.value = begin}};
    return request(ui, &message, NULL, error, error_size);
}

int
snag_ui_external_replay(
    struct snag_ui *ui, const unsigned char *tail, size_t length, char *error, size_t error_size)
{
    struct ui_message message = {.command = {.kind = SNAG_UI_EXTERNAL,
                                     .data.value = false,
                                     .text = (const char *)tail,
                                     .len = length}};
    return request(ui, &message, NULL, error, error_size);
}

int
snag_ui_hold(struct snag_ui *ui, bool active)
{
    return snag_ui_send(ui, (struct snag_ui_command){.kind = SNAG_UI_HOLD, .data.value = active});
}

static int
send_prompt(struct snag_ui *ui, enum snag_ui_operation kind, bool active, const char *label,
    const char *const spinners[SNAG_TERM_SPINNER_COUNT], uint32_t per_second, unsigned int states,
    const char *const values[SNAG_PROMPT_HOUR], unsigned int mode, const char *submitted)
{
    struct ui_message message = {
        .command = {.kind = kind,
            .label = submitted,
            .data.prompt = {.active = active, .rate = per_second, .states = states, .mode = mode}}};
    for (size_t i = 0u; i < SNAG_TERM_SPINNER_COUNT; ++i) {
        if (strlen(spinners[i]) >= sizeof(message.command.data.prompt.frames[i]))
            return snag_errno(EOVERFLOW);
        memcpy(message.command.data.prompt.frames[i], spinners[i], strlen(spinners[i]) + 1u);
    }
    for (size_t i = 0u; values && i < SNAG_PROMPT_HOUR; ++i) {
        size_t limit = i == SNAG_PROMPT_SESSION_NAME ? SNAG_PATH_MAX_BYTES : SNAG_TERM_LABEL_BYTES;
        message.command.data.prompt.values[i] = snag_strdup_checked(values[i], limit);
        if (!message.command.data.prompt.values[i]) {
            message_free(&message);
            return -1;
        }
    }
    if (kind == SNAG_UI_PROMPT &&
        !(message.command.data.prompt.source = snag_strdup_checked(label, SIZE_MAX))) {
        message_free(&message);
        return -1;
    }
    return send_message(ui, &message, label);
}

int
snag_ui_prompt(struct snag_ui *ui, bool active, const char *label,
    const char *const spinners[SNAG_TERM_SPINNER_COUNT], uint32_t per_second, unsigned int states)
{
    return send_prompt(
        ui, SNAG_UI_PROMPT, active, label, spinners, per_second, states, NULL, 0u, NULL);
}

int
snag_ui_composer(struct snag_ui *ui, bool active, const char *format,
    const char *const values[SNAG_PROMPT_HOUR], unsigned int mode,
    const char *const spinners[SNAG_TERM_SPINNER_COUNT], uint32_t per_second, unsigned int states,
    const char *submitted)
{
    return send_prompt(
        ui, SNAG_UI_PROMPT, active, format, spinners, per_second, states, values, mode, submitted);
}

int
snag_ui_validate_prompt(struct snag_ui *ui, const char *label,
    const char *const spinners[SNAG_TERM_SPINNER_COUNT], uint32_t per_second)
{
    return send_prompt(
        ui, SNAG_UI_VALIDATE, false, label, spinners, per_second, 0u, NULL, 0u, NULL);
}

int
snag_ui_simple_prompt(struct snag_ui *ui, bool active)
{
    const char *frames[SNAG_TERM_SPINNER_COUNT] = {" ", " ", " "};
    return snag_ui_prompt(ui, active, active ? "» " : "› ", frames, 1u, 0u);
}

int
snag_ui_insert_draft(struct snag_ui *ui, const char *text)
{
    return snag_ui_send(ui, (struct snag_ui_command){.kind = SNAG_UI_INSERT, .text = text});
}
int
snag_ui_audio(struct snag_ui *ui, const char *label, bool dictating)
{
    struct snag_ui_command message = {.kind = SNAG_UI_AUDIO, .data.value = dictating};
    int rc = snag_ui_send(
        ui, (struct snag_ui_command){.kind = message.kind, .data = message.data, .text = label});
    if (!dictating) atomic_store(&ui->runtime->dictation_control, 0u);
    return rc;
}

int
snag_ui_voice(struct snag_ui *ui, const char *label)
{
    struct snag_ui_command message = {.kind = SNAG_UI_AUDIO, .data.value = 2u};
    return snag_ui_send(
        ui, (struct snag_ui_command){.kind = message.kind, .data = message.data, .text = label});
}

int
snag_ui_caption(struct snag_ui *ui, unsigned int speaker, const char *text)
{
    struct snag_ui_command message = {.kind = SNAG_UI_CAPTION, .data.value = speaker};
    return snag_ui_send(
        ui, (struct snag_ui_command){.kind = message.kind, .data = message.data, .text = text});
}

int
snag_ui_input(struct snag_ui *ui, const char *text, uint64_t attachment)
{
    if (!ui || !ui->runtime || !text) return snag_errno(EINVAL);
    return snag_ui_send(
        ui, (struct snag_ui_command){
                .kind = SNAG_UI_INPUT, .text = text, .len = strlen(text), .data.seq = attachment});
}

static int history_snapshot(struct snag_ui *ui, bool refresh);

bool
snag_ui_leaving(const struct snag_ui *ui)
{
    return ui && ui->runtime && atomic_load(&ui->runtime->exit_requested);
}

void
snag_ui_request_exit(struct snag_ui *ui)
{
    atomic_store(&ui->runtime->exit_requested, true);
    snag_wakeup_send(ui->runtime->actions.wake[1]);
    snag_wakeup_send(ui->runtime->commands[1]);
}

bool
snag_ui_interrupt_pending(const struct snag_ui *ui)
{
    uint64_t pending = ui && ui->runtime ? atomic_load(&ui->runtime->interrupt) : 0u;
    return pending && (pending == ui->turn_generation || pending == UINT64_MAX);
}

bool
snag_ui_yield_pending(const struct snag_ui *ui)
{
    return ui && ui->runtime && atomic_load(&ui->runtime->yield_requested);
}

int
snag_ui_poll(struct snag_ui *ui, int timeout_ms, enum snag_term_action *action, char **text)
{
    struct snag_ui_runtime *runtime = ui->runtime;
    struct ui_action *item;
    uint64_t deadline = snag_monotonic_ms() + (timeout_ms > 0 ? (uint32_t)timeout_ms : 0u);

    assert(pthread_equal(pthread_self(), runtime->engine));
    *action = SNAG_TERM_NONE;
    *text = NULL;
    ui->input_echoed = false;
    ui->input_error = false;
    ui->input_interface = ui->input_terminal_command = false;
    ui->input_verbosity = -1;
    if (ui->view_request[0] &&
        snag_ui_view_result(ui, ui->view_request, "rejected", 0u, "input was not admitted") < 0)
        return -1;
    for (;;) {
        snag_wakeup_drain(runtime->actions.wake[0]);
        int fatal = atomic_load(&runtime->fatal);
        if (fatal) {
            errno = fatal;
            /* A buffer/encoding failure can originate in presentation too.
             * Report it once without closing input or blaming the draft. */
            if (input_condition(fatal)) atomic_store(&runtime->fatal, 0);
            return -1;
        }
        if (atomic_load(&runtime->exit_requested)) {
            /* Stop reading immediately, but let the engine retain submissions
             * already received before delivering the exit control. */
            item = queue_pop(&runtime->actions);
            if (item) break;
            atomic_store(&runtime->exit_requested, false);
            *action = SNAG_TERM_EXIT;
            return 1;
        }
        unsigned int dictation = atomic_exchange(&runtime->dictation_control, 0u);
        if (dictation) {
            *action = (enum snag_term_action)dictation;
            return 1;
        }
        uint64_t interrupted = atomic_exchange(&runtime->interrupt, 0u);
        if (interrupted && (interrupted == ui->turn_generation || interrupted == UINT64_MAX)) {
            *action = SNAG_TERM_INTERRUPT;
            return 1;
        }
        if (atomic_exchange(&runtime->cancel, false)) {
            *action = SNAG_TERM_CANCEL;
            return 1;
        }
        unsigned int yield = atomic_exchange(&runtime->yield_requested, 0u);
        if (yield) {
            *text = snag_strdup_checked("/yield", 16u);
            if (!*text) return -1;
            ui->input_echoed = yield == 2u;
            *action = SNAG_TERM_SUBMIT;
            return 1;
        }
        /* COMMIT is engine work even when no input action was submitted. */
        if (atomic_load(&runtime->session_pending)) return 0;
        item = queue_pop(&runtime->actions);
        if (item) {
            snag_wakeup_send(runtime->commands[1]);
            break;
        }
        uint64_t now = snag_monotonic_ms();
        if (timeout_ms >= 0 && now >= deadline) return 0;
        int remaining = timeout_ms < 0 ? -1 : (int)(deadline - now);
        if (snag_wakeup_wait(runtime->actions.wake[0], remaining) < 0) {
            if (errno == EINTR) return 0;
            return -1;
        }
    }
    if (item->interface_input && (item->attachment != atomic_load(&runtime->session_attachment) ||
                                     (ui->native && !item->attachment))) {
        if (item->steering) atomic_fetch_sub(&runtime->steering_pending, 1u);
        (void)snag_ui_view_result(
            ui, item->view_request, "rejected", 0u, "originating controller detached");
        free(item->text);
        free(item);
        return snag_ui_text(
            ui, SNAG_UI_WARNING, "UI input discarded: its originating attachment ended.");
    }
    memcpy(ui->view_request, item->view_request, sizeof(ui->view_request));
    ui->input_received_ms = item->received_ms;
    ui->input_error = item->input_error;
    ui->input_terminal_command = item->terminal_command;
    ui->input_interface = item->interface_input && !item->terminal_command;
    ui->input_verbosity = ui->input_interface ? item->verbosity : -1;
    ui->input_view = item->snapshot.view;
    ui->input_active = item->snapshot.active;
    ui->input_route = item->route;
    ui->input_conversation = item->snapshot.conversation;
    ui->input_address_conversation = item->snapshot.address_conversation;
    ui->input_scopes = item->snapshot.scopes;
    ui->input_destination = item->snapshot.selection.id;
    ui->input_echoed = item->submission_echoed;
    ui->input_view_applied = item->view_applied;
    ui->selection = item->snapshot.selection;
    memcpy(ui->submitted_label, item->snapshot.label, sizeof(ui->submitted_label));
    if (item->feedback[0] && ui->observe)
        ui->observe(ui->observe_opaque, "host", item->feedback, item->text);
    if (item->local) {
        (void)snag_ui_history_add(ui, item->text);
        free(item->text);
        item->text = NULL;
    }
    *action = item->action;
    *text = item->text;
    if (item->steering) atomic_fetch_sub(&runtime->steering_pending, 1u);
    if (item->history_warning && !ui->history.warned)
        ui->history.warning = ui->history.warned = true;
    if (item->history_refresh) {
        if (history_snapshot(ui, true) < 0) {
            item->error = errno;
            ui->input_error = false;
        }
    }
    {
        int error = item->error;
        free(item);
        if (error) {
            errno = error;
            return -1;
        }
    }
    return *action != SNAG_TERM_NONE;
}

snag_wake_fd
snag_ui_wake_fd(const struct snag_ui *ui)
{
    return ui && ui->runtime ? ui->runtime->actions.wake[0] : SNAG_WAKE_INVALID;
}

void
snag_ui_signal(struct snag_ui *ui)
{
    if (ui) {
        atomic_store(&ui->runtime->exit_requested, true);
        snag_wakeup_send(ui->runtime->actions.wake[1]);
    }
}

int
snag_ui_submitted(struct snag_ui *ui, const char *label, const char *text, bool input)
{
    struct ui_message message = {.command = {.kind = SNAG_UI_SUBMITTED, .data.value = input}};
    if (label == ui->label && ui->submitted_label[0]) label = ui->submitted_label;
    message.command.label = label;
    return input ? send_input_message(ui, &message, text) : send_message(ui, &message, text);
}

int
snag_ui_public(struct snag_ui *ui, const char *text, size_t len, struct snag_buf *delivered)
{
    struct ui_message message = {.command = {.kind = SNAG_UI_PUBLIC, .text = text, .len = len}};
    return request(ui, &message, delivered, NULL, 0u);
}

int
snag_ui_orientation(struct snag_ui *ui, const struct snag_session *session, bool resumed)
{
    struct ui_message message = {.command = {.kind = SNAG_UI_ORIENTATION,
                                     .data.orientation = {.turns = session->turn_count,
                                         .queued = session->pending_queue_count,
                                         .resumed = resumed,
                                         .queue_armed = session->queue_armed}}};
    message.command.label = session->id;
    return send_message(ui, &message, session->cwd);
}

struct history_replay {
    struct snag_ui *ui;
    struct snag_render *report;
    struct snag_history_turn turn;
    struct snag_buf response;
    struct history_irc_payload *payloads;
    size_t payload_count;
    uint64_t completed, shown, total;
};

/* Turn prompts name IRC updates by durable id and keep the room event itself
 * aside; replay resolves the references so the operator reads the room content
 * rather than an internal pointer. The ring bounds memory for long sessions. */
#define HISTORY_IRC_PAYLOADS 256u

struct history_irc_payload {
    char id[SNAG_ID_HEX_LEN + 24u];
    char *text;
};

static int
history_note_irc_event(struct history_replay *history, const char *type, const json_t *data)
{
    struct snag_irc_event event;
    struct snag_buf rendered;
    snag_buf_init(&rendered, sizeof(event) + 1024u);
    if (snag_irc_event_record_read(type, data, &event) < 0) return 0;
    if (!event.stream[0] || snag_irc_event_projection(&rendered, &event) < 0 || !rendered.len ||
        !rendered.data) {
        snag_buf_free(&rendered);
        return 0;
    }
    if (!history->payloads) {
        history->payloads = calloc(HISTORY_IRC_PAYLOADS, sizeof(*history->payloads));
        if (!history->payloads) {
            snag_buf_free(&rendered);
            return -1;
        }
    }
    struct history_irc_payload *slot =
        &history->payloads[history->payload_count % HISTORY_IRC_PAYLOADS];
    ++history->payload_count;
    while (rendered.len && ((char *)rendered.data)[rendered.len - 1u] == '\n') --rendered.len;
    ((char *)rendered.data)[rendered.len] = '\0';
    (void)snprintf(
        slot->id, sizeof(slot->id), "%s:%llu", event.stream, (unsigned long long)event.sequence);
    free(slot->text);
    slot->text = (char *)rendered.data;
    return 0;
}

static const char *
history_irc_payload(struct history_replay *history, const char *id, size_t len)
{
    size_t count;
    if (!history->payloads) return NULL;
    count = history->payload_count < HISTORY_IRC_PAYLOADS ? history->payload_count
                                                          : HISTORY_IRC_PAYLOADS;
    for (size_t i = 0u; i < count; ++i)
        if (history->payloads[i].text && strlen(history->payloads[i].id) == len &&
            !strncmp(history->payloads[i].id, id, len))
            return history->payloads[i].text;
    return NULL;
}

static char *
history_resolve_irc(struct history_replay *history, const char *text)
{
    static const char marker[] = "[IRC update id=";
    const char *cursor = text;
    size_t refs = 0u;
    struct snag_buf out;
    if (!text || !strstr(text, marker)) return NULL;
    for (const char *p = text; (p = strstr(p, marker)) != NULL; p += sizeof(marker) - 1u) ++refs;
    snag_buf_init(&out, strlen(text) + refs * (SNAG_IRC_TEXT_MAX + 128u) + 1u);
    while (*cursor) {
        const char *end = strchr(cursor, '\n');
        size_t line = end ? (size_t)(end - cursor) : strlen(cursor);
        const char *payload = NULL;
        if (line > sizeof(marker) - 1u && !strncmp(cursor, marker, sizeof(marker) - 1u)) {
            const char *id = cursor + sizeof(marker) - 1u;
            const char *space = memchr(id, ' ', line - (sizeof(marker) - 1u));
            if (space) payload = history_irc_payload(history, id, (size_t)(space - id));
        }
        if (snag_buf_append(&out, payload ? payload : cursor, payload ? strlen(payload) : line) <
                0 ||
            (end && snag_buf_putc(&out, '\n') < 0))
            goto fail;
        cursor = end ? end + 1 : cursor + line;
    }
    if (snag_buf_terminate(&out) < 0) goto fail;
    return (char *)out.data;
fail:
    snag_buf_free(&out);
    return NULL;
}

static int
history_display(struct history_replay *history, const struct snag_history_turn *turn)
{
    if (history->report) {
        return snag_render_history(
            history->report, turn, history->shown, history->completed, history->total);
    }
    return snag_ui_send(history->ui, (struct snag_ui_command){.kind = SNAG_UI_HISTORY,
                                         .data.replay = {.turn = turn,
                                             .shown = history->shown,
                                             .completed = history->completed,
                                             .total = history->total}});
}

static int
history_append(char **target, const char *text, const char *separator)
{
    if (!text) return snag_errno(EINVAL);
    size_t old = *target ? strlen(*target) : 0u;
    size_t sep = old ? strlen(separator) : 0u, len = strlen(text);
    size_t size;
    if (!snag_size_add(old, sep, &size) || !snag_size_add(size, len + 1u, &size)) return -1;
    char *joined = realloc(*target, size);
    if (!joined) return -1;
    memcpy(joined + old, separator, sep);
    memcpy(joined + old + sep, text, len + 1u);
    *target = joined;
    return 0;
}

static int
history_items(struct history_replay *history, const json_t *items)
{
    for (size_t i = 0u; i < json_array_size(items); ++i) {
        const json_t *item = json_array_get(items, i);
        const char *kind = snag_json_string(item, "kind");
        if (snag_string_in(kind, "assistant refusal") &&
            history_append(&history->turn.assistant, snag_json_string(item, "text"), "\n\n") < 0)
            return -1;
    }
    return 0;
}

static int
history_flush(struct history_replay *history, bool finish)
{
    char *resolved;
    if (!finish && !history->turn.user && !history->turn.assistant && !history->turn.partial) {
        return 0;
    }
    resolved = history_resolve_irc(history, history->turn.user);
    if (resolved) {
        free(history->turn.user);
        history->turn.user = resolved;
    }
    if (finish && history->response.len) {
        if (snag_buf_terminate(&history->response) < 0 ||
            history_append(&history->turn.assistant, (char *)history->response.data, "\n\n") < 0)
            return -1;
        snag_buf_reset(&history->response);
    }
    if (!history->turn.continuation) ++history->shown;
    struct snag_history_turn displayed = history->turn;
    if (!finish) displayed.status = NULL;
    int rc = history_display(history, &displayed);
    free(history->turn.user);
    free(history->turn.assistant);
    history->turn.user = NULL;
    history->turn.assistant = NULL;
    history->turn.partial = false;
    history->turn.continuation = true;
    return rc;
}

static int
history_finish(struct history_replay *history)
{
    if (!history->turn.user && !history->turn.partial && !history->turn.continuation) return 0;
    int rc = history_flush(history, true);
    if (!rc) history->turn = (struct snag_history_turn){0};
    return rc;
}

static int
history_event(void *opaque, const struct snag_session *state, uint64_t seq, const char *type,
    const json_t *data, char *error, size_t error_size)
{
    struct history_replay *history = opaque;
    struct snag_history_turn *turn = &history->turn;
    bool completed = snag_string_in(type, "turn_completed turn_completed_silent");
    (void)state;
    (void)seq;
    (void)error;
    (void)error_size;
    if (history->ui && snag_ui_leaving(history->ui)) return snag_errno(ECANCELED);
    if (history->ui && snag_string_in(type, "irc_event irc_event_v2") &&
        history_note_irc_event(history, type, data) < 0)
        return -1;
    if (!strcmp(type, "voice_event")) {
        if (history_flush(history, false) < 0) return -1;
        if (history->report) {
            return snag_render_voice_event(history->report, json_object_get(data, "event"), 0u, 0u);
        }
        return snag_ui_send(history->ui, (struct snag_ui_command){.kind = SNAG_UI_VOICE_EVENT,
                                             .data.voice = json_object_get(data, "event")});
    }
    if (!strcmp(type, "turn_started")) {
        if (history_finish(history) < 0) return -1;
        const char *text = snag_json_string(data, "text");
        const char *kind = snag_json_string(data, "input_kind");
        if (!text) return snag_errno(EINVAL);
        turn->user = strdup(text);
        turn->timer = kind && !strcmp(kind, "timer");
        turn->status = "unfinished";
        return turn->user ? 0 : -1;
    }
    if (!turn->user && !turn->partial && !turn->continuation) return 0;
    if (!strcmp(type, "irc_admitted") && json_object_get(data, "steering"))
        return history_append(&turn->user,
            snag_json_string(json_object_get(data, "steering"), "text"), "\nsteering: ");
    if (!strcmp(type, "steering_added"))
        return history_append(&turn->user, snag_json_string(data, "text"), "\nsteering: ");
    if (!strcmp(type, "response_started")) {
        snag_buf_reset(&history->response);
    } else if (!strcmp(type, "response_output")) {
        const char *text = snag_json_string(json_object_get(data, "item"), "text");
        uint64_t offset;
        if (!text || snag_json_integer_u64(data, "offset", &offset) < 0) return snag_errno(EINVAL);
        if (!offset && history->response.len && snag_buf_append(&history->response, "\n\n", 2u) < 0)
            return -1;
        return snag_buf_append(&history->response, text, strlen(text));
    } else if (snag_string_in(type, "response_completed response_failed response_interrupted "
                                    "response_output_correction")) {
        snag_buf_reset(&history->response);
        json_t *items = json_object_get(data, "items");
        if (!items) items = json_object_get(data, "partial_public");
        return history_items(history, items);
    } else if (completed || snag_string_in(type, "turn_failed turn_interrupted")) {
        history->completed += completed;
        turn->status = completed                      ? "completed"
                       : !strcmp(type, "turn_failed") ? "failed"
                                                      : "interrupted";
        return history_finish(history);
    }
    return 0;
}

struct history_window {
    struct snag_ui *ui;
    json_t *events;
    uint64_t remaining;
    size_t prefix_irc;
    bool wants_irc;
};

static int
history_collect(void *opaque, const struct snag_session *state, uint64_t seq, const char *type,
    const json_t *data, char *error, size_t error_size)
{
    struct history_window *window = opaque;
    (void)state;
    (void)seq;
    (void)error;
    (void)error_size;
    if (snag_ui_leaving(window->ui)) return snag_errno(ECANCELED);
    bool irc = snag_string_in(type, "irc_event irc_event_v2");
    if (!strcmp(type, "irc_admitted") && !json_object_get(data, "steering")) return 0;
    if (!window->remaining) {
        if (!window->wants_irc || window->prefix_irc == HISTORY_IRC_PAYLOADS) return 1;
        if (!irc) return 0;
        ++window->prefix_irc;
    } else if (
        !irc &&
        !snag_string_in(type,
            "voice_event turn_started steering_added irc_admitted response_started response_output "
            "response_completed response_failed response_interrupted response_output_correction "
            "turn_completed turn_completed_silent turn_failed turn_interrupted"))
        return 0;
    const json_t *text_data =
        !strcmp(type, "irc_admitted") ? json_object_get(data, "steering") : data;
    const char *text = !irc ? snag_json_string(text_data, "text") : NULL;
    if (text && strstr(text, "[IRC update id=")) window->wants_irc = true;
    if (!strcmp(type, "turn_started")) --window->remaining;
    if (json_array_append_new(
            window->events, json_pack("{s:s,s:O}", "type", type, "data", (json_t *)data)) < 0)
        return -1;
    bool done =
        !window->remaining && (!window->wants_irc || window->prefix_irc == HISTORY_IRC_PAYLOADS);
    return done ? SNAG_JOURNAL_STOP_AFTER : 0;
}

static int
history_show(
    struct snag_ui *ui, struct snag_session *session, uint64_t count, struct snag_render *report)
{
    /* One response's public text plus one separator per fragment, under the
     * response-public byte bound. */
    struct history_replay history = {.ui = ui,
        .report = report,
        .total = session->turn_count,
        .response = {.max = 3u * SNAG_MAX_RESPONSE_GRAPH}};
    struct history_window window = {.ui = ui, .remaining = count, .events = json_array()};
    uint64_t before = 0u;
    int rc = window.events ? 0 : -1;
    if (!rc && count) {
        char error[256] = {0};
        rc = snag_session_each_event_reverse(
            session, 0u, SNAG_JOURNAL_PAGE_BYTES, history_collect, &window, &before,
            error, sizeof(error));
        if (rc < 0 && !snag_ui_leaving(ui)) {
            int saved = errno;
            char message[512];
            (void)snprintf(message, sizeof(message), "cannot replay session history: %s",
                error[0] ? error : strerror(saved));
            (void)snag_ui_text(ui, SNAG_UI_ERROR, message);
            errno = saved;
        }
        bool first = true;
        for (size_t i = json_array_size(window.events); !rc && i; --i) {
            const json_t *entry = json_array_get(window.events, i - 1u);
            const char *type = snag_json_string(entry, "type");
            if (first && !snag_string_in(type, "irc_event irc_event_v2 voice_event")) {
                history.turn.partial = before && strcmp(type, "turn_started");
                history.turn.status = "unfinished";
                first = false;
            }
            rc = history_event(&history, NULL, 0u, type, json_object_get(entry, "data"), NULL, 0u);
        }
        if (rc == 0) rc = history_finish(&history);
        if (!rc && before && window.remaining) {
            char notice[192];
            (void)snprintf(notice, sizeof(notice),
                "History scan window ended; earlier events: read_session_history before_seq=%llu.",
                (unsigned long long)before);
            rc = report ? snag_render_host(report, notice) : snag_ui_text(ui, SNAG_UI_HOST, notice);
        }
    }
    if (rc == 0 && count && session->pending_input) {
        const char *text = snag_json_string(session->pending_input, "text");
        rc = report ? snag_render_submitted(report, "pending input › ", text)
                    : snag_ui_submitted(ui, "pending input › ", text, false);
    }
    if (rc == 0) rc = history_display(&history, NULL);
    free(history.turn.user);
    free(history.turn.assistant);
    snag_buf_free(&history.response);
    if (history.payloads)
        for (size_t i = 0u; i < HISTORY_IRC_PAYLOADS; ++i) free(history.payloads[i].text);
    free(history.payloads);
    json_decref(window.events);
    return rc;
}

int
snag_ui_history(struct snag_ui *ui, struct snag_session *session, uint64_t count)
{
    return history_show(ui, session, count, NULL);
}

int
snag_ui_history_report(
    struct snag_ui *ui, struct snag_session *session, uint64_t count, struct snag_buf *text)
{
    struct snag_render render;
    struct snag_term target = {.capture = text, .output_fd = {-1, -1}};
    struct snag_term *previous = snag_term_output_owner();

    snag_render_init(&render, snag_ui_verbosity(ui));
    render.stdout_terminal = render.stderr_terminal = true;
    render.markdown = false;
    /* Only this engine thread renders into the report; the UI keeps its terminal. */
    snag_term_output_bind(&target);
    int rc = history_show(ui, session, count, &render);
    snag_term_output_bind(previous);
    snag_render_free(&render);
    return rc;
}

static int
history_snapshot(struct snag_ui *ui, bool refresh)
{
    struct ui_message message = {
        .command = {.kind = SNAG_UI_HISTORY_SNAPSHOT, .data.history.refresh = refresh}};
    if (snag_history_snapshot_copy(&message.command.data.history.entries, &ui->history.snapshot) <
        0)
        return -1;
    return send_message(ui, &message, NULL);
}

int
snag_ui_history_open(struct snag_ui *ui, const char *dotdir, const char *session_dir)
{
    int rc = snag_history_open(&ui->history, dotdir);
    if (snag_history_bind(&ui->history, session_dir) < 0) rc = -1;
    return history_snapshot(ui, false) < 0 ? -1 : rc;
}

int
snag_ui_history_add(struct snag_ui *ui, const char *text)
{
    int rc = snag_history_add(&ui->history, text);
    return history_snapshot(ui, false) < 0 ? -1 : rc;
}

bool
snag_ui_history_warning(struct snag_ui *ui)
{
    return snag_history_take_warning(&ui->history);
}
