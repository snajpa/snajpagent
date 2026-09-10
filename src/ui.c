/* SPDX-License-Identifier: GPL-2.0-only */
#include "ui.h"
#include "wake.h"
#include "update.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ui_snapshot {
    enum snag_render_view view;
    bool opened, prompt_wanted, active;
    char label[SNAG_TERM_LABEL_BYTES];
    uint64_t turn_generation;
    struct snag_irc_target selection;
};

struct ui_message {
    struct snag_ui_command command;
    struct ui_snapshot snapshot;
    struct snag_buf delivered;
    char error[256];
    int result, saved_errno;
    atomic_bool done;
};

#define UI_QUEUE_CAPACITY 32u

struct ui_queue {
    _Atomic size_t head, tail;
    void *items[UI_QUEUE_CAPACITY];
    snag_wake_fd wake[2];
};

struct ui_action {
    uint64_t received_ms;
    enum snag_term_action action;
    char *text;
    int error;
    bool history_refresh, history_warning, steering, local;
    struct ui_snapshot snapshot;
    struct snag_irc_route route;
};

struct snag_ui_runtime {
    struct ui_queue actions;
    snag_wake_fd commands[2];
    _Atomic(struct ui_message *) request; /* One synchronous engine call. */
    pthread_t thread, engine;
    struct snag_signal_mask saved_mask;
    atomic_int fatal;
    atomic_bool exit_requested, cancel;
    atomic_uint steering_pending;
    atomic_uint level, view;
    _Atomic uint64_t interrupt;
    _Atomic uint64_t pause_until;
};

struct snag_ui_display {
    struct snag_render render;
    struct snag_term term;
    struct snag_ui_prompt prompt;
    struct snag_update *update;
    struct snag_ui_runtime *runtime;
    uint64_t turn_generation;
    bool suspended;
    bool input_closed, backlog_warned;
    char feedback[192];
    struct ui_action *local;
    bool local_acknowledged, painting_feedback;
};

static int
set_level(struct snag_ui_display *display, unsigned int level)
{
    if (!snag_verbosity_name(level))
        return snag_errno(EINVAL);
    display->render.verbosity = level;
    atomic_store(&display->runtime->level, level);
    return 0;
}

static void
verbosity_command(struct snag_ui_display *display, const char *text)
{
    const char *value = text + 8u;
    while (isspace((unsigned char)*value))
        ++value;
    if (*value) {
        const char *end = value + 1u;
        while (isspace((unsigned char)*end))
            ++end;
        if (*value < '0' || *value > '6' || *end) {
            (void)snprintf(display->feedback, sizeof(display->feedback),
                           "/verbose expects one integer from 0 through 6");
            return;
        }
        (void)set_level(display, (unsigned int)(*value - '0'));
    }
    (void)snprintf(display->feedback, sizeof(display->feedback),
        "verbosity: %u (%s)%s", display->render.verbosity,
        snag_verbosity_name(display->render.verbosity),
        display->render.view == SNAG_RENDER_CHAT ? " · work detail is in /rollout" : "");
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
    if (head - atomic_load_explicit(&queue->tail, memory_order_acquire) ==
        UI_QUEUE_CAPACITY)
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
    if (tail == atomic_load_explicit(&queue->head, memory_order_acquire))
        return NULL;
    item = queue->items[tail % UI_QUEUE_CAPACITY];
    atomic_store_explicit(&queue->tail, tail + 1u, memory_order_release);
    return item;
}

static void
take_snapshot(struct snag_ui_display *display, struct ui_snapshot *snapshot)
{
    snapshot->view = snag_render_view(&display->render);
    snapshot->opened = display->term.opened;
    snapshot->prompt_wanted = display->term.prompt_wanted;
    snapshot->active = display->term.active;
    snapshot->turn_generation = display->turn_generation;
    snapshot->selection = display->term.destination;
    memcpy(snapshot->label, display->term.label, sizeof(snapshot->label));
}

static void
adopt_snapshot(struct snag_ui *ui, const struct ui_snapshot *snapshot)
{
    ui->view = snapshot->view;
    ui->opened = snapshot->opened;
    ui->prompt_wanted = snapshot->prompt_wanted;
    ui->active = snapshot->active;
    ui->turn_generation = snapshot->turn_generation;
    ui->selection = snapshot->selection;
    memcpy(ui->label, snapshot->label, sizeof(ui->label));
}

static void
prompt_free(struct snag_ui_prompt *prompt)
{
    free(prompt->source);
    for (size_t i = 0u; i < SNAG_PROMPT_HOUR; ++i)
        free(prompt->values[i]);
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

static int
configure_prompt(struct snag_ui_display *display, const struct snag_ui_prompt *prompt,
                 struct snag_term *term)
{
    const char *frames[SNAG_TERM_SPINNER_COUNT];
    const char *values[SNAG_PROMPT_FIELD_COUNT];
    const struct snag_prompt_clock *clock = &term->prompt_clock;
    char hour[12], minute[12], second[12], label[SNAG_TERM_LABEL_BYTES];
    const char *text = prompt->source;

    snag_term_capture_prompt_clock(term, time(NULL));
    if (prompt->values[0]) {
        (void)snprintf(hour, sizeof(hour), clock->valid ? "%u" : "--", clock->hour);
        (void)snprintf(minute, sizeof(minute), clock->valid ? "%u" : "--", clock->minute);
        (void)snprintf(second, sizeof(second), clock->valid ? "%u" : "--", clock->second);
        for (size_t i = 0u; i < SNAG_PROMPT_HOUR; ++i)
            values[i] = prompt->values[i];
        if (prompt->mode == 0u && display->term.destinations)
            for (size_t i = 0u; i < display->term.destinations->count; ++i) {
                const struct snag_irc_destination *destination =
                    &display->term.destinations->items[i];
                if (destination->target.id == display->term.destination.id &&
                    destination->operator[0])
                    values[SNAG_PROMPT_OPERATOR] = destination->operator;
            }
        values[SNAG_PROMPT_HOUR] = hour;
        values[SNAG_PROMPT_MINUTE] = minute;
        values[SNAG_PROMPT_SECOND] = second;
        if (snag_config_prompt_expand(text, prompt->mode, values,
                SNAG_TERM_SPINNER_MARKER_BASE, label, sizeof(label)) < 0)
            return -1;
        text = label;
    }
    for (size_t i = 0u; i < SNAG_TERM_SPINNER_COUNT; ++i)
        frames[i] = prompt->frames[i];
    return snag_term_set_prompt_template(term, prompt->active, text,
                                        frames, prompt->rate, prompt->states);
}

static int
apply_prompt(struct snag_ui_display *display)
{
    display->term.blank_local = display->prompt.values[0] != NULL;
    return configure_prompt(display, &display->prompt, &display->term);
}

static int
apply_message(struct snag_ui_display *display, struct snag_ui_command *command,
              char *error, size_t error_size)
{
    struct snag_render *render = &display->render;
    struct snag_term *term = &display->term;

    switch (command->kind) {
    case SNAG_UI_LEVEL: return set_level(display, command->data.value);
    case SNAG_UI_HOST: return snag_render_host(render, command->text);
    case SNAG_UI_HELP: return snag_render_help(render, command->text);
    case SNAG_UI_RUNTIME: return snag_render_runtime(render, command->text);
    case SNAG_UI_ERROR: return snag_render_error_ctx(render, command->text);
    case SNAG_UI_WARNING: return snag_render_warning_ctx(render, command->text);
    case SNAG_UI_ROLLOUT_END: return snag_render_rollout_end(render);
    case SNAG_UI_ROLLOUT_ABORT: return snag_render_rollout_abort(render);
    case SNAG_UI_CLOSE:
        snag_render_attach_term(render, NULL);
        snag_term_close(term);
        return 0;
    case SNAG_UI_COLOR: {
        bool previous = render->color_stderr;
        snag_render_set_color(render, (enum snag_color_mode)command->data.value);
        return previous != render->color_stderr && display->prompt.source ?
            apply_prompt(display) : 0;
    }
    case SNAG_UI_MARKDOWN:
        snag_render_set_markdown(render, command->data.value != 0u);
        return 0;
    case SNAG_UI_DESTINATIONS:
        if (snag_term_set_destinations(term, command->data.destinations) < 0)
            return -1;
        return display->prompt.source ? apply_prompt(display) : 0;
    case SNAG_UI_SELECT:
        if (snag_term_select_destination(term, command->data.value) < 0)
            return -1;
        return display->prompt.source ? apply_prompt(display) : 0;
    case SNAG_UI_ROUTE:
        snag_term_destination_route(term, command->text, command->data.route);
        return 0;
    case SNAG_UI_COMMANDS:
        snag_term_set_commands(term, command->data.commands.items,
                             command->data.commands.count);
        return 0;
    case SNAG_UI_PAUSE:
        snag_term_set_typing_pause(term, command->data.timing.typing_pause_ms);
        term->tool_spinner_off_delay_ms = command->data.timing.tool_spinner_off_delay_ms;
        return 0;
    case SNAG_UI_OPEN:
        if (snag_term_open(term, error, error_size) < 0)
            return -1;
        snag_render_attach_term(render, term);
        return 0;
    case SNAG_UI_EXTERNAL:
        display->suspended = command->data.value != 0u;
        return command->data.value ?
            snag_term_external_begin(term, error, error_size) :
            snag_term_external_end(term, error, error_size);
    case SNAG_UI_PROMPT: {
        if (command->label) {
            /* Dispatch gets a fresh label without replacing a live draft/clock. */
            struct snag_term submitted;
            snag_term_init(&submitted);
            submitted.defer_redraw = true;
            int rc = configure_prompt(display, &command->data.prompt, &submitted);
            if (rc == 0)
                rc = snag_render_submitted(render, submitted.label, command->label);
            snag_term_close(&submitted);
            return rc;
        }
        term->defer_redraw = true;
        if (snag_render_before_prompt(render) < 0)
            return -1;
        term->defer_redraw = false;
        if (command->data.prompt.active && !term->active)
            ++display->turn_generation;
        prompt_free(&display->prompt);
        display->prompt = command->data.prompt;
        memset(&command->data.prompt, 0, sizeof(command->data.prompt));
        return apply_prompt(display);
    }
    case SNAG_UI_VALIDATE: {
        const char *frames[SNAG_TERM_SPINNER_COUNT];
        struct snag_term probe;
        int rc;
        for (size_t i = 0u; i < SNAG_TERM_SPINNER_COUNT; ++i)
            frames[i] = command->data.prompt.frames[i];
        snag_term_init(&probe);
        rc = snag_term_set_prompt_template(&probe, false, command->text,
                    frames, command->data.prompt.rate,
                    (1u << SNAG_TERM_SPINNER_COUNT) - 1u);
        snag_term_close(&probe);
        return rc;
    }
    case SNAG_UI_SPINNERS:
        display->prompt.states = command->data.value;
        return snag_term_set_spinner_states(term, command->data.value);
    case SNAG_UI_DRAFT: return snag_term_restore_draft(term, command->text);
    case SNAG_UI_VIEW:
        term->defer_redraw = true;
        term->chat = command->data.value == SNAG_RENDER_CHAT;
        if (!term->opened) {
            render->view = (enum snag_render_view)command->data.value;
            return 0;
        }
        return snag_render_set_view(render,
                                  (enum snag_render_view)command->data.value);
    case SNAG_UI_SUBMITTED:
        return command->data.value ?
            snag_render_input_submitted(render, command->label, command->text) :
            snag_render_submitted(render, command->label, command->text);
    case SNAG_UI_PUBLIC_BEGIN:
        return snag_render_rollout_begin(render, command->data.public.fd,
                                        command->label, command->data.public.kind);
    case SNAG_UI_ORIENTATION:
        return snag_render_orientation(render, command->text, command->label,
                    command->data.orientation.turns,
                    command->data.orientation.queued,
                    command->data.orientation.resumed, command->data.orientation.queue_armed);
    case SNAG_UI_HISTORY:
        return snag_render_history(render, command->data.replay.turn,
            command->data.replay.shown, command->data.replay.completed, command->data.replay.total);
    case SNAG_UI_IRC: return snag_render_irc_event(render, command->data.irc);
    case SNAG_UI_DURABLE:
        return snag_render_durable(render, command->data.durable.fd,
            command->data.durable.source, command->text,
            command->data.durable.timeout_ms, command->data.durable.max_output_bytes);
    case SNAG_UI_EVENT:
        return snag_render_event(render, command->data.seq, command->text);
    case SNAG_UI_RESUME:
        return snag_render_resume_hint(render, command->text, command->len);
    case SNAG_UI_PROTOCOL:
        return snag_render_protocol(render, command->label, command->text,
                                   command->len);
    case SNAG_UI_TRANSPORT:
        return snag_render_transport(render, (char)command->data.value,
                                    command->text, command->len);
    case SNAG_UI_HISTORY_SNAPSHOT:
        return snag_term_history_set(term, &command->data.history.entries,
                                    command->data.history.refresh);
    case SNAG_UI_UPDATE:
        if (!display->update)
            display->update = snag_update_start(command->label, command->text,
                                                display->runtime->commands[1]);
        return 0;
    case SNAG_UI_STOP: return 0;
    case SNAG_UI_PUBLIC: case SNAG_UI_RAW: break; /* Sliced by apply_display. */
    }
    return snag_errno(EINVAL);
}

static int
read_input(struct snag_ui_display *display, int timeout_ms)
{
    struct snag_ui_runtime *runtime = display->runtime;
    struct snag_term *term = &display->term;
    struct ui_action *item;
    int rc;

    if (!term->opened || display->suspended ||
        display->input_closed) {
        rc = snag_wakeup_wait(runtime->commands[0], timeout_ms);
        return rc < 0 && errno != EINTR ? -1 : 0;
    }
    term->input_backlog = queue_full(&runtime->actions);
    term->local_backlog = display->local != NULL;
    if (!term->input_backlog)
        display->backlog_warned = false;
    else if (!display->backlog_warned && !term->input_only) {
        display->backlog_warned = true;
        if (snag_render_warning_ctx(&display->render,
                "input backlog is full; draft retained, retry Enter shortly") < 0)
            return -1;
    }
    item = calloc(1u, sizeof(*item));
    if (!item)
        return -1;
    rc = snag_term_poll(term, timeout_ms, runtime->commands[0],
                       &item->action, &item->text);
    item->history_warning = term->history_reader.warning;
    term->history_reader.warning = false;
    if (item->text) item->received_ms = snag_time_ms();
    take_snapshot(display, &item->snapshot);
    if (item->text)
        snag_term_destination_route(term, item->text, &item->route);
    if (item->action == SNAG_TERM_CANCEL || item->action == SNAG_TERM_INTERRUPT ||
        item->action == SNAG_TERM_SUBMIT || item->action == SNAG_TERM_QUEUE) {
        bool deferred = term->defer_redraw;
        term->defer_redraw = deferred || item->action == SNAG_TERM_SUBMIT ||
                             item->action == SNAG_TERM_QUEUE;
        if (!term->input_only && display->prompt.source && apply_prompt(display) < 0)
            goto fail;
        term->defer_redraw = deferred;
    }
    atomic_store(&runtime->pause_until,
        term->typing_active ? term->last_input_ms + term->typing_pause_ms : 0u);
    if (rc < 0 && errno != EINTR)
        item->error = errno;
    if (term->history_refresh_requested && !term->input_backlog) {
        term->history_refresh_requested = false;
        item->history_refresh = true;
    }
    if (item->action == SNAG_TERM_SUBMIT && item->text) {
        uint32_t id;
        size_t body;
        enum snag_irc_target_command command = snag_irc_target_parse(
            item->text, strlen(item->text), &id, &body);
        if (term->blank_local && snag_text_blank(item->text)) {
            display->feedback[0] = '\0';
            item->local = true;
        } else if (command == SNAG_IRC_TARGET_SELECT) {
            if (snag_term_select_destination(term, id) == 0)
                (void)snprintf(display->feedback, sizeof(display->feedback),
                               "destination: %u", id);
            else
                (void)snprintf(display->feedback, sizeof(display->feedback),
                               "destination %u is unavailable; use /names", id);
            if (!term->input_only && display->prompt.source && apply_prompt(display) < 0)
                goto fail;
            take_snapshot(display, &item->snapshot);
            item->local = true;
        } else if (snag_verbosity_command(item->text, strlen(item->text))) {
            verbosity_command(display, item->text);
            item->local = true;
        }
    }
    if (item->local) {
        item->action = SNAG_TERM_NONE;
        term->prompt_wanted = true;
        display->local = item;
        display->local_acknowledged = false;
        return 0;
    }
    if (item->action == SNAG_TERM_INTERRUPT) {
        atomic_store(&runtime->interrupt, display->turn_generation);
    } else if (item->action == SNAG_TERM_EXIT) {
        atomic_store(&runtime->exit_requested, true);
        display->input_closed = true;
    } else if (item->action == SNAG_TERM_CANCEL && term->input_backlog) {
        atomic_store(&runtime->cancel, true);
    } else if (item->action != SNAG_TERM_NONE || item->local ||
               item->history_refresh || item->history_warning || item->error) {
        if (item->action == SNAG_TERM_SUBMIT || item->action == SNAG_TERM_QUEUE) {
            term->prompt_wanted = true;
            if (item->action == SNAG_TERM_SUBMIT && item->snapshot.active &&
                item->snapshot.view == SNAG_RENDER_ROLLOUT && item->text &&
                (item->text[0] != '/' || item->text[1] == '/')) {
                item->steering = true;
                atomic_fetch_add(&runtime->steering_pending, 1u);
            }
        }
        if (queue_push(&runtime->actions, item))
            return 0;
        atomic_store(&runtime->fatal, item->error ? item->error : EOVERFLOW);
    }
    {
        bool notify = item->action != SNAG_TERM_NONE || item->error;
        free(item->text);
        free(item);
        if (notify)
            snag_wakeup_send(runtime->actions.wake[1]);
    }
    return 0;
fail:
    free(item->text);
    free(item);
    return -1;
}

static int
local_feedback(struct snag_ui_display *display)
{
    struct ui_action *item = display->local;
    int rc = 0;

    if (!item || display->painting_feedback)
        return 0;
    if (!display->local_acknowledged) {
        display->painting_feedback = true;
        rc = snag_render_submitted(&display->render, item->snapshot.label, item->text);
        if (rc == 0 && display->feedback[0])
            rc = snag_render_host(&display->render, display->feedback);
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
    return read_input(opaque, 0);
}

static int
render_input_checkpoint(void *opaque)
{
    struct snag_ui_display *display = opaque;
    int rc = read_input(display, 0);
    display->render.suppress_optional = atomic_load(&display->runtime->exit_requested) ||
        atomic_load(&display->runtime->interrupt) ||
        atomic_load(&display->runtime->steering_pending);
    return rc < 0 ? -1 : local_feedback(display);
}

static bool
public_stopped(struct snag_ui_runtime *runtime)
{
    return atomic_load(&runtime->exit_requested) ||
           atomic_load(&runtime->interrupt) ||
           atomic_load(&runtime->steering_pending);
}

static int
apply_display(struct snag_ui_display *display, struct ui_message *message)
{
    struct snag_ui_runtime *runtime = display->runtime;
    size_t offset = 0u;
    bool raw = message->command.kind == SNAG_UI_RAW;

    display->render.suppress_optional = public_stopped(runtime);
    if (!raw && message->command.kind != SNAG_UI_PUBLIC)
        return apply_message(display, &message->command,
                             message->error, sizeof(message->error));
    while (offset < message->command.len) {
        size_t amount = message->command.len - offset;
        if (amount > 1024u)
            amount = 1024u;
        if (raw && amount < message->command.len - offset) {
            size_t boundary = amount;
            while (boundary && ((unsigned char)message->command.text[offset + boundary] & 0xc0u) == 0x80u)
                --boundary;
            if (boundary)
                amount = boundary;
        }
        if (render_input_checkpoint(display) < 0)
            return -1;
        while (!raw && !public_stopped(runtime) &&
               snag_term_typing_pause_remaining(&display->term, snag_monotonic_ms()))
            if (read_input(display, 16) < 0)
                return -1;
        if (!raw && public_stopped(runtime))
            return 0;
        if (raw ? snag_term_write((int)message->command.data.value,
                                 message->command.text + offset, amount) < 0 :
            snag_render_rollout(&display->render, message->command.text + offset, amount,
                                &message->delivered) < 0)
            return -1;
        offset += amount;
    }
    return 0;
}

static void *
presentation_main(void *opaque)
{
    struct snag_ui_runtime *runtime = opaque;
    struct snag_ui_display display = {.runtime = runtime};

    snag_term_init(&display.term);
    display.term.input_checkpoint = output_input_checkpoint;
    display.term.input_opaque = &display;
    snag_render_init(&display.render, 0u);
    display.render.checkpoint = render_input_checkpoint;
    display.render.checkpoint_opaque = &display;
    (void)snag_term_signals_unblock();
    for (;;) {
        struct ui_message *message;
        snag_wakeup_drain(runtime->commands[0]);
        message = atomic_exchange_explicit(&runtime->request, NULL, memory_order_acquire);
        const char *banner = display.suspended ? NULL : snag_update_take(display.update);
        if (banner)
            (void)snag_render_update(&display.render, banner);
        if (read_input(&display, message || banner ? 0 : -1) < 0) {
            atomic_store(&runtime->fatal, errno ? errno : EIO);
            display.input_closed = true;
            snag_wakeup_send(runtime->actions.wake[1]);
        }
        if (local_feedback(&display) < 0)
            atomic_store(&runtime->fatal, errno ? errno : EIO);
        if (!message)
            continue;
        snag_buf_init(&message->delivered, message->command.len + 4u);
        message->result = apply_display(&display, message);
        message->saved_errno = errno;
        if (message->result < 0 && message->command.kind != SNAG_UI_VALIDATE) {
            atomic_store(&runtime->fatal, errno ? errno : EIO);
            display.input_closed = true;
        }
        take_snapshot(&display, &message->snapshot);
        atomic_store(&runtime->view, (unsigned int)display.render.view);
        {
            bool stop = message->command.kind == SNAG_UI_STOP;
            atomic_store_explicit(&message->done, true, memory_order_release);
            snag_wakeup_send(runtime->actions.wake[1]);
            if (stop)
                break;
        }
    }
    char *banner = snag_update_stop(display.update);
    if (banner) {
        (void)snag_render_update(&display.render, banner);
        free(banner);
    }
    snag_render_free(&display.render);
    if (display.local) {
        free(display.local->text);
        free(display.local);
    }
    snag_term_close(&display.term);
    prompt_free(&display.prompt);
    return NULL;
}

static int
request(struct snag_ui *ui, struct ui_message *message, struct snag_buf *delivered,
        char *error, size_t error_size)
{
    int rc = -1, saved;
    struct snag_ui_runtime *runtime = ui->runtime;
    assert(pthread_equal(pthread_self(), runtime->engine));
    atomic_init(&message->done, false);
    if (message->command.len > SIZE_MAX - 4u) {
        errno = EOVERFLOW;
        goto out;
    }
    atomic_store_explicit(&runtime->request, message, memory_order_release);
    snag_wakeup_send(runtime->commands[1]);
    for (;;) {
        snag_wakeup_drain(runtime->actions.wake[0]);
        if (atomic_load_explicit(&message->done, memory_order_acquire))
            break;
        (void)snag_wakeup_wait(runtime->actions.wake[0], -1);
    }
    rc = message->result;
    adopt_snapshot(ui, &message->snapshot);
    if (error && error_size)
        (void)snprintf(error, error_size, "%s", message->error);
    if (delivered && message->delivered.len &&
        snag_buf_append(delivered, message->delivered.data,
                       message->delivered.len) < 0)
        rc = -1;
    snag_buf_free(&message->delivered);
    if (rc < 0 && message->saved_errno)
        errno = message->saved_errno;
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

int
snag_ui_send(struct snag_ui *ui, struct snag_ui_command command)
{
    struct ui_message message = {.command = command};
    switch (command.kind) {
    case SNAG_UI_DRAFT: case SNAG_UI_EVENT: case SNAG_UI_DURABLE:
        return send_message(ui, &message, command.text);
    default:
        return request(ui, &message, NULL, NULL, 0u);
    }
}

int
snag_ui_init(struct snag_ui *ui)
{
    struct snag_ui_runtime *runtime;
    int rc;
    memset(ui, 0, sizeof(*ui));
    runtime = calloc(1u, sizeof(*runtime));
    if (!runtime)
        return -1;
    atomic_init(&runtime->fatal, 0);
    runtime->engine = pthread_self();
    atomic_init(&runtime->interrupt, 0u);
    atomic_init(&runtime->exit_requested, false);
    atomic_init(&runtime->cancel, false);
    atomic_init(&runtime->steering_pending, 0u);
    atomic_init(&runtime->pause_until, 0u);
    atomic_init(&runtime->level, 0u);
    atomic_init(&runtime->view, SNAG_RENDER_ROLLOUT);
    atomic_init(&runtime->request, NULL);
    if (snag_wakeup_create(runtime->commands) < 0)
        goto fail;
    if (queue_open(&runtime->actions) < 0)
        goto commands;
    if (snag_term_signals_block(&runtime->saved_mask) < 0) {
        goto actions;
    }
    rc = pthread_create(&runtime->thread, NULL, presentation_main, runtime);
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
fail:
    free(runtime);
    return -1;
}

void
snag_ui_free(struct snag_ui *ui)
{
    struct snag_ui_runtime *runtime = ui->runtime;
    struct ui_message message = {.command = {.kind = SNAG_UI_STOP}};
    struct ui_action *action;
    if (!runtime)
        return;
    (void)send_message(ui, &message, NULL);
    (void)pthread_join(runtime->thread, NULL);
    while ((action = queue_pop(&runtime->actions))) {
        free(action->text);
        free(action);
    }
    snag_wakeup_close(runtime->actions.wake);
    snag_wakeup_close(runtime->commands);
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
    struct ui_message message = {.command = {.kind = op}};
    return send_message(ui, &message, text);
}

int
snag_ui_set_verbosity(struct snag_ui *ui, unsigned int level)
{
    struct ui_message message = {.command = {.kind = SNAG_UI_LEVEL, .data.value = level}};
    if (!snag_verbosity_name(level))
        return snag_errno(EINVAL);
    return send_message(ui, &message, NULL);
}

unsigned int
snag_ui_verbosity(const struct snag_ui *ui)
{
    return ui && ui->runtime ? atomic_load(&ui->runtime->level) : 0u;
}

bool
snag_ui_enabled(const struct snag_ui *ui, enum snag_presentation kind)
{
    return ui && ui->runtime && snag_presentation_enabled(kind,
        snag_ui_verbosity(ui), (enum snag_render_view)atomic_load(&ui->runtime->view));
}

int
snag_ui_capture_route(struct snag_ui *ui, const char *text)
{
    struct ui_message message = {.command = {.kind = SNAG_UI_ROUTE, .data.route = &ui->input_route}};
    return send_message(ui, &message, text);
}

uint32_t
snag_ui_pause_remaining(struct snag_ui *ui)
{
    uint64_t until = atomic_load(&ui->runtime->pause_until);
    uint64_t now = snag_monotonic_ms();
    if (public_stopped(ui->runtime))
        return 0u;
    return until > now ? (uint32_t)(until - now) : 0u;
}

int
snag_ui_open(struct snag_ui *ui, char *error, size_t error_size)
{
    struct ui_message message = {.command = {.kind = SNAG_UI_OPEN}};
    if (ui->opened)
        return 0;
    return request(ui, &message, NULL, error, error_size);
}

int
snag_ui_external(struct snag_ui *ui, bool begin, char *error, size_t error_size)
{
    struct ui_message message = {.command = {.kind = SNAG_UI_EXTERNAL, .data.value = begin}};
    return request(ui, &message, NULL, error, error_size);
}

static int
send_prompt(struct snag_ui *ui, enum snag_ui_operation kind, bool active, const char *label,
              const char *const spinners[SNAG_TERM_SPINNER_COUNT],
              uint32_t per_second, unsigned int states,
              const char *const values[SNAG_PROMPT_HOUR], unsigned int mode,
              const char *submitted)
{
    struct ui_message message = {.command = {
        .kind = kind, .label = submitted,
        .data.prompt = {.active = active, .rate = per_second, .states = states,
                        .mode = mode}
    }};
    for (size_t i = 0u; i < SNAG_TERM_SPINNER_COUNT; ++i) {
        if (strlen(spinners[i]) >= sizeof(message.command.data.prompt.frames[i]))
            return snag_errno(EOVERFLOW);
        memcpy(message.command.data.prompt.frames[i], spinners[i],
               strlen(spinners[i]) + 1u);
    }
    for (size_t i = 0u; values && i < SNAG_PROMPT_HOUR; ++i) {
        message.command.data.prompt.values[i] = snag_strdup_checked(values[i], SNAG_TERM_LABEL_BYTES);
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
              const char *const spinners[SNAG_TERM_SPINNER_COUNT],
              uint32_t per_second, unsigned int states)
{
    return send_prompt(ui, SNAG_UI_PROMPT, active, label, spinners, per_second, states, NULL, 0u, NULL);
}

int
snag_ui_composer(struct snag_ui *ui, bool active, const char *format,
                 const char *const values[SNAG_PROMPT_HOUR], unsigned int mode,
                 const char *const spinners[SNAG_TERM_SPINNER_COUNT],
                 uint32_t per_second, unsigned int states, const char *submitted)
{
    return send_prompt(ui, SNAG_UI_PROMPT, active, format, spinners, per_second,
                        states, values, mode, submitted);
}

int
snag_ui_validate_prompt(struct snag_ui *ui, const char *label,
                       const char *const spinners[SNAG_TERM_SPINNER_COUNT],
                       uint32_t per_second)
{
    return send_prompt(ui, SNAG_UI_VALIDATE, false, label, spinners, per_second, 0u, NULL, 0u, NULL);
}

int
snag_ui_simple_prompt(struct snag_ui *ui, bool active)
{
    const char *frames[SNAG_TERM_SPINNER_COUNT] = {" ", " ", " "};
    return snag_ui_prompt(ui, active, active ? "» " : "› ", frames, 1u, 0u);
}

static int history_snapshot(struct snag_ui *ui, bool refresh);

bool
snag_ui_leaving(const struct snag_ui *ui)
{
    return ui && ui->runtime && atomic_load(&ui->runtime->exit_requested);
}

int
snag_ui_poll(struct snag_ui *ui, int timeout_ms,
            enum snag_term_action *action, char **text)
{
    struct snag_ui_runtime *runtime = ui->runtime;
    struct ui_action *item;
    uint64_t deadline = snag_monotonic_ms() + (timeout_ms > 0 ? (uint32_t)timeout_ms : 0u);

    assert(pthread_equal(pthread_self(), runtime->engine));
    *action = SNAG_TERM_NONE;
    *text = NULL;
    for (;;) {
        snag_wakeup_drain(runtime->actions.wake[0]);
        int fatal = atomic_load(&runtime->fatal);
        if (fatal) {
            errno = fatal;
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
        uint64_t interrupted = atomic_exchange(&runtime->interrupt, 0u);
        if (interrupted && interrupted == ui->turn_generation) {
            *action = SNAG_TERM_INTERRUPT;
            return 1;
        }
        if (atomic_exchange(&runtime->cancel, false)) {
            *action = SNAG_TERM_CANCEL;
            return 1;
        }
        item = queue_pop(&runtime->actions);
        if (item) {
            snag_wakeup_send(runtime->commands[1]);
            break;
        }
        uint64_t now = snag_monotonic_ms();
        if (timeout_ms >= 0 && now >= deadline)
            return 0;
        int remaining = timeout_ms < 0 ? -1 : (int)(deadline - now);
        if (snag_wakeup_wait(runtime->actions.wake[0], remaining) < 0) {
            if (errno == EINTR)
                return 0;
            return -1;
        }
    }
    ui->input_received_ms = item->received_ms;
    ui->input_view = item->snapshot.view;
    ui->input_active = item->snapshot.active;
    ui->input_route = item->route;
    ui->selection = item->snapshot.selection;
    memcpy(ui->submitted_label, item->snapshot.label,
           sizeof(ui->submitted_label));
    if (item->local) {
        (void)snag_ui_history_add(ui, item->text);
        free(item->text);
        item->text = NULL;
    }
    *action = item->action;
    *text = item->text;
    if (item->steering)
        atomic_fetch_sub(&runtime->steering_pending, 1u);
    if (item->history_warning && !ui->history.warned) {
        ui->history.warning = ui->history.warned = true;
    }
    if (item->history_refresh) {
        if (history_snapshot(ui, true) < 0)
            item->error = errno;
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
    if (label == ui->label && ui->submitted_label[0])
        label = ui->submitted_label;
    message.command.label = label;
    return send_message(ui, &message, text);
}

int
snag_ui_public(struct snag_ui *ui, const char *text, size_t len,
              struct snag_buf *delivered)
{
    struct ui_message message = {.command = {
        .kind = SNAG_UI_PUBLIC, .text = text, .len = len}};
    return request(ui, &message, delivered, NULL, 0u);
}

int
snag_ui_orientation(struct snag_ui *ui, const struct snag_session *session,
                   bool resumed)
{
    struct ui_message message = {.command = {
        .kind = SNAG_UI_ORIENTATION,
        .data.orientation = {.turns = session->turn_count,
            .queued = session->pending_queue_count, .resumed = resumed,
            .queue_armed = session->queue_armed}
    }};
    message.command.label = session->id;
    return send_message(ui, &message, session->workspace);
}

struct history_replay {
    struct snag_ui *ui;
    struct snag_history_turn turn;
    struct snag_buf response;
    uint64_t completed, skip, shown, total;
};

static int
history_display(struct history_replay *history, const struct snag_history_turn *turn)
{
    return snag_ui_send(history->ui, (struct snag_ui_command){.kind = SNAG_UI_HISTORY,
        .data.replay = {.turn = turn, .shown = history->shown,
            .completed = history->completed, .total = history->total}});
}

static int
history_append(char **target, const char *text, const char *separator)
{
    size_t old = *target ? strlen(*target) : 0u;
    size_t sep = old ? strlen(separator) : 0u, len = strlen(text);
    size_t size;
    if (!snag_size_add(old, sep, &size) || !snag_size_add(size, len + 1u, &size))
        return -1;
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
history_finish(struct history_replay *history)
{
    if (!history->turn.user) return 0;
    if (history->response.len) {
        if (snag_buf_terminate(&history->response) < 0 ||
            history_append(&history->turn.assistant, (char *)history->response.data, "\n\n") < 0)
            return -1;
        snag_buf_reset(&history->response);
    }
    ++history->shown;
    int rc = history_display(history, &history->turn);
    free(history->turn.user);
    free(history->turn.assistant);
    history->turn = (struct snag_history_turn){0};
    return rc;
}

static int
history_event(void *opaque, const struct snag_session *state, uint64_t seq,
              const char *type, const json_t *data, char *error, size_t error_size)
{
    struct history_replay *history = opaque;
    struct snag_history_turn *turn = &history->turn;
    bool completed = snag_string_in(type, "turn_completed turn_completed_silent");
    (void)state; (void)seq; (void)error; (void)error_size;
    if (!history->ui) {
        history->completed += completed;
        return 0;
    }
    if (!strcmp(type, "turn_started")) {
        if (history_finish(history) < 0) return -1;
        if (history->skip) { --history->skip; return 0; }
        turn->user = strdup(snag_json_string(data, "text"));
        turn->status = "unfinished";
        return turn->user ? 0 : -1;
    }
    if (!turn->user) return 0;
    if (!strcmp(type, "irc_admitted") && json_object_get(data, "steering"))
        return history_append(&turn->user, snag_json_string(json_object_get(data, "steering"), "text"), "\nsteering: ");
    if (!strcmp(type, "steering_added"))
        return history_append(&turn->user, snag_json_string(data, "text"), "\nsteering: ");
    if (!strcmp(type, "response_started")) {
        snag_buf_reset(&history->response);
    } else if (!strcmp(type, "response_output")) {
        const char *text = snag_json_string(json_object_get(data, "item"), "text");
        uint64_t offset;
        if (snag_json_integer_u64(data, "offset", &offset) < 0) return -1;
        if (!offset && history->response.len && snag_buf_append(&history->response, "\n\n", 2u) < 0)
            return -1;
        return snag_buf_append(&history->response, text, strlen(text));
    } else if (snag_string_in(type, "response_completed response_failed response_interrupted response_output_correction")) {
        snag_buf_reset(&history->response);
        json_t *items = json_object_get(data, "items");
        if (!items) items = json_object_get(data, "partial_public");
        return history_items(history, items);
    } else if (completed || snag_string_in(type, "turn_failed turn_interrupted")) {
        turn->status = completed ? "completed" : !strcmp(type, "turn_failed") ? "failed" : "interrupted";
        return history_finish(history);
    }
    return 0;
}

int
snag_ui_history(struct snag_ui *ui, struct snag_session *session, uint64_t count)
{
    struct history_replay history = {.total = session->turn_count,
        .response = {.max = SNAG_MAX_RESPONSE_GRAPH + 2u * SNAG_MAX_RESPONSE_ITEMS}};
    int rc = snag_session_each_event(session, history_event, &history, NULL, 0u);
    history.ui = ui;
    if (rc == 0 && count && history.total) {
        history.skip = history.total > count ? history.total - count : 0u;
        rc = snag_session_each_event(session, history_event, &history, NULL, 0u);
        if (rc == 0) rc = history_finish(&history);
    }
    if (rc == 0 && count && session->pending_input)
        rc = snag_ui_submitted(ui, "pending input › ", snag_json_string(session->pending_input, "text"), false);
    if (rc == 0) rc = history_display(&history, NULL);
    free(history.turn.user);
    free(history.turn.assistant);
    snag_buf_free(&history.response);
    return rc;
}

static int
history_snapshot(struct snag_ui *ui, bool refresh)
{
    struct ui_message message = {.command = {
        .kind = SNAG_UI_HISTORY_SNAPSHOT, .data.history.refresh = refresh
    }};
    if (snag_history_snapshot_copy(&message.command.data.history.entries,
                                 &ui->history.snapshot) < 0)
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
