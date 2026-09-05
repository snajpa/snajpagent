/* SPDX-License-Identifier: GPL-2.0-only */
#include "ui.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum ui_kind {
    UI_TEXT, UI_LEVEL, UI_COLOR, UI_MARKDOWN, UI_NETWORKED, UI_NICKS, UI_COMMANDS, UI_PAUSE,
    UI_OPEN, UI_EXTERNAL, UI_PROMPT, UI_SPINNERS, UI_DRAFT,
    UI_VIEW, UI_SUBMITTED, UI_PUBLIC_BEGIN, UI_PUBLIC, UI_VALIDATE,
    UI_ORIENTATION, UI_HISTORY, UI_IRC, UI_DURABLE, UI_EVENT,
    UI_RESUME, UI_PROTOCOL, UI_TRANSPORT, UI_RAW, UI_HISTORY_SNAPSHOT, UI_STOP
};

struct ui_snapshot {
    enum snag_render_view view;
    bool opened, prompt_wanted, active;
    char label[SNAG_TERM_LABEL_BYTES];
    uint64_t turn_generation;
};

struct ui_prompt {
    bool active;
    uint32_t rate;
    unsigned int states, mode;
    char frames[SNAG_TERM_SPINNER_COUNT][80];
    char *values[SNAG_PROMPT_HOUR];
};

struct ui_message {
    enum ui_kind kind;
    char *text;
    char *label;
    size_t len;
    uint64_t sequence;
    struct ui_snapshot snapshot;
    struct snag_buf delivered;
    char error[256];
    int result, saved_errno;
    atomic_bool done;
    union {
        enum snag_ui_operation operation;
        unsigned int value;
        struct ui_prompt prompt;
        struct { int fd; enum snag_presentation kind; } public;
        struct { uint64_t turns; size_t queued; bool resumed; } orientation;
        struct snag_irc_event irc;
        struct { int fd; struct snag_render_source source;
                 uint32_t timeout_ms, max_output_bytes; } durable;
        uint64_t seq;
        struct { const struct snag_term_command *items; size_t count; } commands;
        struct { struct snag_history_snapshot entries; bool refresh; } history;
    } data;
};

#define UI_QUEUE_CAPACITY 32u

struct ui_queue {
    _Atomic size_t head, tail;
    void *items[UI_QUEUE_CAPACITY];
    int wake[2];
};

struct ui_action {
    enum snag_term_action action;
    char *text;
    int error;
    bool history_refresh, steering, local;
    struct ui_snapshot snapshot;
};

struct snag_ui_runtime {
    struct ui_queue commands, actions;
    pthread_t thread, engine;
    sigset_t saved_mask;
    atomic_int fatal;
    atomic_bool exit_requested, cancel;
    atomic_uint steering_pending;
    atomic_uint level, view;
    _Atomic uint64_t interrupt;
    _Atomic uint64_t pause_until;
    uint64_t sequence;
};

struct snag_ui_display {
    struct snag_render render;
    struct snag_term term;
    struct ui_prompt prompt;
    char *prompt_source;
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
    if (!snag_verbosity_name(level)) {
        errno = EINVAL;
        return -1;
    }
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

static void
wake_owner(struct ui_queue *queue)
{
    int saved = errno;
    char byte = 0;
    ssize_t rc;
    do {
        rc = write(queue->wake[1], &byte, 1u);
    } while (rc < 0 && errno == EINTR);
    errno = saved;
}

static void
drain_wake(struct ui_queue *queue)
{
    char bytes[64];
    while (read(queue->wake[0], bytes, sizeof(bytes)) > 0)
        ;
}

static int
queue_open(struct ui_queue *queue)
{
    atomic_init(&queue->head, 0u);
    atomic_init(&queue->tail, 0u);
    if (pipe(queue->wake) < 0)
        return -1;
    for (size_t i = 0u; i < 2u; ++i) {
        int flags = fcntl(queue->wake[i], F_GETFL);
        if (flags < 0 ||
            fcntl(queue->wake[i], F_SETFL, flags | O_NONBLOCK) < 0 ||
            fcntl(queue->wake[i], F_SETFD, FD_CLOEXEC) < 0) {
            int saved = errno;
            (void)close(queue->wake[0]);
            (void)close(queue->wake[1]);
            errno = saved;
            return -1;
        }
    }
    return 0;
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
    wake_owner(queue);
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
    memcpy(ui->label, snapshot->label, sizeof(ui->label));
}

static void
message_free(struct ui_message *message)
{
    if (message->kind == UI_HISTORY_SNAPSHOT)
        snag_history_snapshot_free(&message->data.history.entries);
    free(message->text);
    free(message->label);
    if (message->kind == UI_PROMPT)
        for (size_t i = 0u; i < SNAG_PROMPT_HOUR; ++i)
            free(message->data.prompt.values[i]);
}

static int
apply_prompt(struct snag_ui_display *display)
{
    struct ui_prompt *prompt = &display->prompt;
    const char *frames[SNAG_TERM_SPINNER_COUNT];
    const char *values[SNAG_PROMPT_FIELD_COUNT];
    const struct snag_prompt_clock *clock = &display->term.prompt_clock;
    char hour[12], minute[12], second[12], label[SNAG_TERM_LABEL_BYTES];
    const char *text = display->prompt_source;

    snag_term_capture_prompt_clock(&display->term, time(NULL));
    if (prompt->values[0]) {
        (void)snprintf(hour, sizeof(hour), clock->valid ? "%u" : "--", clock->hour);
        (void)snprintf(minute, sizeof(minute), clock->valid ? "%u" : "--", clock->minute);
        (void)snprintf(second, sizeof(second), clock->valid ? "%u" : "--", clock->second);
        for (size_t i = 0u; i < SNAG_PROMPT_HOUR; ++i)
            values[i] = prompt->values[i];
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
    return snag_term_set_prompt_template(&display->term, prompt->active, text,
                                        frames, prompt->rate, prompt->states);
}

static int
apply_text(struct snag_ui_display *display, const struct ui_message *message)
{
    struct snag_render *render = &display->render;
    struct snag_term *term = &display->term;

    switch (message->data.operation) {
    case SNAG_UI_HOST: return snag_render_host(render, message->text);
    case SNAG_UI_RUNTIME: return snag_render_runtime(render, message->text);
    case SNAG_UI_ERROR: return snag_render_error_ctx(render, message->text);
    case SNAG_UI_WARNING: return snag_render_warning_ctx(render, message->text);
    case SNAG_UI_ROLLOUT_END: return snag_render_rollout_end(render);
    case SNAG_UI_ROLLOUT_ABORT: return snag_render_rollout_abort(render);
    case SNAG_UI_CLOSE:
        snag_render_attach_term(render, NULL);
        snag_term_close(term);
        return 0;
    }
    errno = EINVAL;
    return -1;
}

static int
apply_message(struct snag_ui_display *display, struct ui_message *message,
              char *error, size_t error_size)
{
    struct snag_render *render = &display->render;
    struct snag_term *term = &display->term;

    switch (message->kind) {
    case UI_LEVEL: return set_level(display, message->data.value);
    case UI_TEXT: return apply_text(display, message);
    case UI_COLOR: {
        bool previous = render->color_stderr;
        snag_render_set_color(render, (enum snag_color_mode)message->data.value);
        return previous != render->color_stderr && display->prompt_source ?
            apply_prompt(display) : 0;
    }
    case UI_MARKDOWN:
        snag_render_set_markdown(render, message->data.value != 0u);
        return 0;
    case UI_NETWORKED:
        snag_render_set_networked(render, message->data.value != 0u,
                                message->text);
        term->chat = render->view == SNAG_RENDER_CHAT;
        return 0;
    case UI_NICKS:
        free(term->nicks);
        term->nicks = message->text;
        message->text = NULL;
        return 0;
    case UI_COMMANDS:
        snag_term_set_commands(term, message->data.commands.items,
                             message->data.commands.count);
        return 0;
    case UI_PAUSE:
        snag_term_set_typing_pause(term, message->data.value);
        return 0;
    case UI_OPEN:
        if (snag_term_open(term, error, error_size) < 0)
            return -1;
        snag_render_attach_term(render, term);
        return 0;
    case UI_EXTERNAL:
        display->suspended = message->data.value != 0u;
        return message->data.value ?
            snag_term_external_begin(term, error, error_size) :
            snag_term_external_end(term, error, error_size);
    case UI_PROMPT: {
        term->defer_redraw = true;
        if (snag_render_before_prompt(render) < 0)
            return -1;
        term->defer_redraw = false;
        if (message->data.prompt.active && !term->active)
            ++display->turn_generation;
        free(display->prompt_source);
        for (size_t i = 0u; i < SNAG_PROMPT_HOUR; ++i)
            free(display->prompt.values[i]);
        display->prompt = message->data.prompt;
        display->prompt_source = message->text;
        message->text = NULL;
        memset(message->data.prompt.values, 0, sizeof(message->data.prompt.values));
        return apply_prompt(display);
    }
    case UI_VALIDATE: {
        const char *frames[SNAG_TERM_SPINNER_COUNT];
        struct snag_term probe;
        int rc;
        for (size_t i = 0u; i < SNAG_TERM_SPINNER_COUNT; ++i)
            frames[i] = message->data.prompt.frames[i];
        snag_term_init(&probe);
        rc = snag_term_set_prompt_template(&probe, false, message->text,
                    frames, message->data.prompt.rate,
                    (1u << SNAG_TERM_SPINNER_COUNT) - 1u);
        snag_term_close(&probe);
        return rc;
    }
    case UI_SPINNERS:
        display->prompt.states = message->data.value;
        return snag_term_set_spinner_states(term, message->data.value);
    case UI_DRAFT: return snag_term_restore_draft(term, message->text);
    case UI_VIEW:
        term->defer_redraw = true;
        term->chat = message->data.value == SNAG_RENDER_CHAT;
        if (!term->opened) {
            render->view = (enum snag_render_view)message->data.value;
            return 0;
        }
        return snag_render_set_view(render,
                                  (enum snag_render_view)message->data.value);
    case UI_SUBMITTED:
        return message->data.value ?
            snag_render_input_submitted(render, message->label, message->text) :
            snag_render_submitted(render, message->label, message->text);
    case UI_PUBLIC_BEGIN:
        return snag_render_rollout_begin(render, message->data.public.fd,
                                        message->label, message->data.public.kind);
    case UI_ORIENTATION:
        return snag_render_orientation(render, message->text, message->label,
                    message->data.orientation.turns,
                    message->data.orientation.queued,
                    message->data.orientation.resumed);
    case UI_HISTORY:
        return snag_render_history(render, message->label, message->text);
    case UI_IRC: return snag_render_irc_event(render, &message->data.irc);
    case UI_DURABLE:
        return snag_render_durable(render, message->data.durable.fd,
            message->data.durable.source, message->text,
            message->data.durable.timeout_ms, message->data.durable.max_output_bytes);
    case UI_EVENT:
        return snag_render_event(render, message->data.seq, message->text);
    case UI_RESUME:
        return snag_render_resume_hint(render, message->text, message->len);
    case UI_PROTOCOL:
        return snag_render_protocol(render, message->label, message->text,
                                   message->len);
    case UI_TRANSPORT:
        return snag_render_transport(render, (char)message->data.value,
                                    message->text, message->len);
    case UI_HISTORY_SNAPSHOT:
        return snag_term_history_set(term, &message->data.history.entries,
                                    message->data.history.refresh);
    case UI_STOP: return 0;
    case UI_PUBLIC: case UI_RAW: break; /* Sliced by apply_display. */
    }
    errno = EINVAL;
    return -1;
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
        struct pollfd pfd = {runtime->commands.wake[0], POLLIN, 0};
        rc = poll(&pfd, 1u, timeout_ms);
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
    rc = snag_term_poll(term, timeout_ms, runtime->commands.wake[0],
                       &item->action, &item->text);
    take_snapshot(display, &item->snapshot);
    if (item->action == SNAG_TERM_CANCEL || item->action == SNAG_TERM_INTERRUPT ||
        item->action == SNAG_TERM_SUBMIT || item->action == SNAG_TERM_QUEUE) {
        bool deferred = term->defer_redraw;
        term->defer_redraw = deferred || item->action == SNAG_TERM_SUBMIT ||
                             item->action == SNAG_TERM_QUEUE;
        if (!term->input_only && display->prompt_source && apply_prompt(display) < 0) {
            free(item->text);
            free(item);
            return -1;
        }
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
    if (item->action == SNAG_TERM_SUBMIT && item->text &&
        snag_verbosity_command(item->text, strlen(item->text))) {
        verbosity_command(display, item->text);
        item->local = true;
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
               item->history_refresh || item->error) {
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
            wake_owner(&runtime->actions);
    }
    return 0;
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
        if (rc == 0)
            rc = snag_render_host(&display->render, display->feedback);
        display->painting_feedback = false;
        display->local_acknowledged = true;
    }
    if (queue_push(&display->runtime->actions, item))
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
    bool raw = message->kind == UI_RAW;

    display->render.suppress_optional = public_stopped(runtime);
    if (!raw && message->kind != UI_PUBLIC)
        return apply_message(display, message,
                             message->error, sizeof(message->error));
    while (offset < message->len) {
        size_t amount = message->len - offset;
        if (amount > 1024u)
            amount = 1024u;
        if (render_input_checkpoint(display) < 0)
            return -1;
        while (!raw && !public_stopped(runtime) &&
               snag_term_typing_pause_remaining(&display->term, snag_monotonic_ms()))
            if (read_input(display, 16) < 0)
                return -1;
        if (!raw && public_stopped(runtime))
            return 0;
        if (raw ? snag_term_write((int)message->data.value,
                                 message->text + offset, amount) < 0 :
            snag_render_rollout(&display->render, message->text + offset, amount,
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
    sigset_t signals;
    uint64_t sequence = 0u;

    snag_term_init(&display.term);
    display.term.input_checkpoint = output_input_checkpoint;
    display.term.input_opaque = &display;
    snag_render_init(&display.render, 0u);
    display.render.checkpoint = render_input_checkpoint;
    display.render.checkpoint_opaque = &display;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGWINCH);
    (void)pthread_sigmask(SIG_UNBLOCK, &signals, NULL);
    for (;;) {
        struct ui_message *message;
        drain_wake(&runtime->commands);
        message = queue_pop(&runtime->commands);
        if (read_input(&display, message ? 0 : -1) < 0) {
            atomic_store(&runtime->fatal, errno ? errno : EIO);
            display.input_closed = true;
            wake_owner(&runtime->actions);
        }
        if (local_feedback(&display) < 0)
            atomic_store(&runtime->fatal, errno ? errno : EIO);
        if (!message)
            continue;
        snag_buf_init(&message->delivered, message->len + 4u);
        message->result = message->sequence == ++sequence ?
            apply_display(&display, message) : -1;
        message->saved_errno = errno;
        if (message->result < 0 && message->kind != UI_VALIDATE) {
            atomic_store(&runtime->fatal, errno ? errno : EIO);
            display.input_closed = true;
        }
        take_snapshot(&display, &message->snapshot);
        atomic_store(&runtime->view, (unsigned int)display.render.view);
        {
            bool stop = message->kind == UI_STOP;
            atomic_store_explicit(&message->done, true, memory_order_release);
            wake_owner(&runtime->actions);
            if (stop)
                break;
        }
    }
    snag_render_free(&display.render);
    if (display.local) {
        free(display.local->text);
        free(display.local);
    }
    snag_term_close(&display.term);
    free(display.prompt_source);
    for (size_t i = 0u; i < SNAG_PROMPT_HOUR; ++i)
        free(display.prompt.values[i]);
    return NULL;
}

static int
request(struct snag_ui *ui, struct ui_message *message, const char *label,
        const char *text, size_t len, struct snag_buf *delivered,
        char *error, size_t error_size)
{
    int rc = -1, saved;
    struct snag_ui_runtime *runtime = ui->runtime;
    struct pollfd pfd = {runtime->actions.wake[0], POLLIN, 0};
    assert(pthread_equal(pthread_self(), runtime->engine));
    message->len = len;
    atomic_init(&message->done, false);
    if (label && !(message->label = snag_strdup_checked(label, SIZE_MAX)))
        goto out;
    if (text) {
        if (len == SIZE_MAX || !(message->text = malloc(len + 1u)))
            goto out;
        memcpy(message->text, text, len);
        message->text[len] = '\0';
    }
    message->sequence = ++runtime->sequence;
    if (!queue_push(&runtime->commands, message)) {
        errno = EOVERFLOW;
        goto out;
    }
    for (;;) {
        drain_wake(&runtime->actions);
        if (atomic_load_explicit(&message->done, memory_order_acquire))
            break;
        (void)poll(&pfd, 1u, -1);
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
    return request(ui, message, NULL, text, text ? strlen(text) : 0u,
                   NULL, NULL, 0u);
}

int
snag_ui_init(struct snag_ui *ui)
{
    struct snag_ui_runtime *runtime;
    sigset_t signals;
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
    if (queue_open(&runtime->commands) < 0)
        goto fail;
    if (queue_open(&runtime->actions) < 0)
        goto commands;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGWINCH);
    rc = pthread_sigmask(SIG_BLOCK, &signals, &runtime->saved_mask);
    if (rc != 0) {
        errno = rc;
        goto actions;
    }
    rc = pthread_create(&runtime->thread, NULL, presentation_main, runtime);
    if (rc != 0) {
        (void)pthread_sigmask(SIG_SETMASK, &runtime->saved_mask, NULL);
        errno = rc;
        goto actions;
    }
    ui->runtime = runtime;
    ui->view = SNAG_RENDER_ROLLOUT;
    return 0;
actions:
    (void)close(runtime->actions.wake[0]);
    (void)close(runtime->actions.wake[1]);
commands:
    (void)close(runtime->commands.wake[0]);
    (void)close(runtime->commands.wake[1]);
fail:
    free(runtime);
    return -1;
}

void
snag_ui_free(struct snag_ui *ui)
{
    struct snag_ui_runtime *runtime = ui->runtime;
    struct ui_message message = {.kind = UI_STOP};
    struct ui_action *action;
    if (!runtime)
        return;
    (void)send_message(ui, &message, NULL);
    (void)pthread_join(runtime->thread, NULL);
    while ((action = queue_pop(&runtime->actions))) {
        free(action->text);
        free(action);
    }
    (void)close(runtime->actions.wake[0]);
    (void)close(runtime->actions.wake[1]);
    (void)close(runtime->commands.wake[0]);
    (void)close(runtime->commands.wake[1]);
    (void)pthread_sigmask(SIG_SETMASK, &runtime->saved_mask, NULL);
    free(runtime);
    ui->runtime = NULL;
    snag_history_free(&ui->history);
}

int
snag_ui_text(struct snag_ui *ui, enum snag_ui_operation op, const char *text)
{
    struct ui_message message = {.kind = UI_TEXT, .data.operation = op};
    return send_message(ui, &message, text);
}

int
snag_ui_set_verbosity(struct snag_ui *ui, unsigned int level)
{
    struct ui_message message = {.kind = UI_LEVEL, .data.value = level};
    if (!snag_verbosity_name(level)) {
        errno = EINVAL;
        return -1;
    }
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
snag_ui_color(struct snag_ui *ui, enum snag_color_mode mode)
{
    struct ui_message message = {.kind = UI_COLOR, .data.value = mode};
    return send_message(ui, &message, NULL);
}

int
snag_ui_markdown(struct snag_ui *ui, bool enabled)
{
    struct ui_message message = {.kind = UI_MARKDOWN, .data.value = enabled};
    return send_message(ui, &message, NULL);
}

int
snag_ui_networked(struct snag_ui *ui, bool enabled, const char *nick)
{
    struct ui_message message = {
        .kind = UI_NETWORKED, .data.value = enabled
    };
    return send_message(ui, &message, nick);
}

int
snag_ui_nicks(struct snag_ui *ui, const char *nicks)
{
    struct ui_message message = {.kind = UI_NICKS};
    return send_message(ui, &message, nicks);
}

int
snag_ui_commands(struct snag_ui *ui, const struct snag_term_command *commands,
                size_t count)
{
    struct ui_message message = {
        .kind = UI_COMMANDS, .data.commands = {.items = commands, .count = count}
    };
    return send_message(ui, &message, NULL);
}

int
snag_ui_typing_pause(struct snag_ui *ui, uint32_t ms)
{
    struct ui_message message = {.kind = UI_PAUSE, .data.value = ms};
    return send_message(ui, &message, NULL);
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
    struct ui_message message = {.kind = UI_OPEN};
    if (ui->opened)
        return 0;
    return request(ui, &message, NULL, NULL, 0u, NULL, error, error_size);
}

int
snag_ui_external(struct snag_ui *ui, bool begin, char *error, size_t error_size)
{
    struct ui_message message = {.kind = UI_EXTERNAL, .data.value = begin};
    return request(ui, &message, NULL, NULL, 0u, NULL, error, error_size);
}

static int
send_prompt(struct snag_ui *ui, enum ui_kind kind, bool active, const char *label,
              const char *const spinners[SNAG_TERM_SPINNER_COUNT],
              uint32_t per_second, unsigned int states,
              const char *const values[SNAG_PROMPT_HOUR], unsigned int mode)
{
    struct ui_message message = {
        .kind = kind,
        .data.prompt = {.active = active, .rate = per_second, .states = states,
                        .mode = mode}
    };
    for (size_t i = 0u; i < SNAG_TERM_SPINNER_COUNT; ++i) {
        if (strlen(spinners[i]) >= sizeof(message.data.prompt.frames[i])) {
            errno = EOVERFLOW;
            return -1;
        }
        memcpy(message.data.prompt.frames[i], spinners[i],
               strlen(spinners[i]) + 1u);
    }
    for (size_t i = 0u; values && i < SNAG_PROMPT_HOUR; ++i) {
        message.data.prompt.values[i] = snag_strdup_checked(values[i], SNAG_TERM_LABEL_BYTES);
        if (!message.data.prompt.values[i]) {
            message_free(&message);
            return -1;
        }
    }
    return send_message(ui, &message, label);
}

int
snag_ui_prompt(struct snag_ui *ui, bool active, const char *label,
              const char *const spinners[SNAG_TERM_SPINNER_COUNT],
              uint32_t per_second, unsigned int states)
{
    return send_prompt(ui, UI_PROMPT, active, label, spinners, per_second, states, NULL, 0u);
}

int
snag_ui_composer(struct snag_ui *ui, bool active, const char *format,
                 const char *const values[SNAG_PROMPT_HOUR], unsigned int mode,
                 const char *const spinners[SNAG_TERM_SPINNER_COUNT],
                 uint32_t per_second, unsigned int states)
{
    return send_prompt(ui, UI_PROMPT, active, format, spinners, per_second,
                        states, values, mode);
}

int
snag_ui_validate_prompt(struct snag_ui *ui, const char *label,
                       const char *const spinners[SNAG_TERM_SPINNER_COUNT],
                       uint32_t per_second)
{
    return send_prompt(ui, UI_VALIDATE, false, label, spinners, per_second, 0u, NULL, 0u);
}

int
snag_ui_simple_prompt(struct snag_ui *ui, bool active)
{
    const char *frames[SNAG_TERM_SPINNER_COUNT] = {" ", " ", " "};
    return snag_ui_prompt(ui, active, active ? "» " : "› ", frames, 1u, 0u);
}

int
snag_ui_spinner_states(struct snag_ui *ui, unsigned int states)
{
    struct ui_message message = {.kind = UI_SPINNERS, .data.value = states};
    return send_message(ui, &message, NULL);
}

int
snag_ui_restore_draft(struct snag_ui *ui, const char *text)
{
    struct ui_message message = {.kind = UI_DRAFT};
    return send_message(ui, &message, text);
}

static int history_snapshot(struct snag_ui *ui, bool refresh);

int
snag_ui_poll(struct snag_ui *ui, int timeout_ms,
            bool active, enum snag_term_action *action, char **text)
{
    struct snag_ui_runtime *runtime = ui->runtime;
    struct pollfd pfd = {runtime->actions.wake[0], POLLIN, 0};
    struct ui_action *item;
    uint64_t deadline = snag_monotonic_ms() + (timeout_ms > 0 ? (uint32_t)timeout_ms : 0u);

    assert(pthread_equal(pthread_self(), runtime->engine));
    *action = SNAG_TERM_NONE;
    *text = NULL;
    for (;;) {
        drain_wake(&runtime->actions);
        int fatal = atomic_load(&runtime->fatal);
        if (fatal) {
            errno = fatal;
            return -1;
        }
        if (atomic_exchange(&runtime->exit_requested, false)) {
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
        size_t tail = atomic_load_explicit(&runtime->actions.tail, memory_order_relaxed);
        item = tail == atomic_load_explicit(&runtime->actions.head, memory_order_acquire) ?
            NULL : runtime->actions.items[tail % UI_QUEUE_CAPACITY];
        /* Defer idle submissions, not view/edit controls or the wait deadline. */
        if (item && active && !item->snapshot.active &&
            (item->action == SNAG_TERM_SUBMIT || item->action == SNAG_TERM_QUEUE))
            item = NULL;
        else
            item = queue_pop(&runtime->actions);
        if (item) {
            wake_owner(&runtime->commands);
            break;
        }
        uint64_t now = snag_monotonic_ms();
        if (timeout_ms >= 0 && now >= deadline)
            return 0;
        int remaining = timeout_ms < 0 ? -1 : (int)(deadline - now);
        if (poll(&pfd, 1u, remaining) < 0) {
            if (errno == EINTR)
                return 0;
            return -1;
        }
    }
    ui->input_view = item->snapshot.view;
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
    if (item->history_refresh) {
        (void)snag_history_refresh(&ui->history);
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

int
snag_ui_wake_fd(const struct snag_ui *ui)
{
    return ui && ui->runtime ? ui->runtime->actions.wake[0] : -1;
}

void
snag_ui_signal(struct snag_ui *ui)
{
    if (ui) {
        atomic_store(&ui->runtime->exit_requested, true);
        wake_owner(&ui->runtime->actions);
    }
}

int
snag_ui_set_view(struct snag_ui *ui, enum snag_render_view view)
{
    struct ui_message message = {.kind = UI_VIEW, .data.value = view};
    return send_message(ui, &message, NULL);
}

int
snag_ui_submitted(struct snag_ui *ui, const char *label, const char *text, bool input)
{
    struct ui_message message = {.kind = UI_SUBMITTED, .data.value = input};
    if (label == ui->label && ui->submitted_label[0])
        label = ui->submitted_label;
    return request(ui, &message, label, text, strlen(text), NULL, NULL, 0u);
}

int
snag_ui_public_begin(struct snag_ui *ui, int fd, const char *label,
                     enum snag_presentation kind)
{
    struct ui_message message = {
        .kind = UI_PUBLIC_BEGIN, .data.public = {.fd = fd, .kind = kind}
    };
    return request(ui, &message, label, NULL, 0u, NULL, NULL, 0u);
}

int
snag_ui_public(struct snag_ui *ui, const char *text, size_t len,
              struct snag_buf *delivered)
{
    struct ui_message message = {.kind = UI_PUBLIC};
    return request(ui, &message, NULL, text, len, delivered, NULL, 0u);
}

int
snag_ui_orientation(struct snag_ui *ui, const struct snag_session *session,
                   bool resumed)
{
    struct ui_message message = {
        .kind = UI_ORIENTATION,
        .data.orientation = {.turns = session->turn_count,
            .queued = session->pending_queue_count, .resumed = resumed}
    };
    return request(ui, &message, session->id, session->workspace,
                   strlen(session->workspace), NULL, NULL, 0u);
}

int
snag_ui_history(struct snag_ui *ui, const struct snag_session *session)
{
    struct ui_message message = {.kind = UI_HISTORY};
    return request(ui, &message, session->last_user, session->last_assistant,
                   session->last_assistant ? strlen(session->last_assistant) : 0u,
                   NULL, NULL, 0u);
}

int
snag_ui_irc_event(struct snag_ui *ui, const struct snag_irc_event *event)
{
    struct ui_message message = {.kind = UI_IRC, .data.irc = *event};
    return send_message(ui, &message, NULL);
}

int
snag_ui_durable(struct snag_ui *ui, int fd, struct snag_render_source source,
                 const char *type, uint32_t timeout_ms, uint32_t max_output_bytes)
{
    struct ui_message message = {.kind = UI_DURABLE,
        .data.durable = {fd, source, timeout_ms, max_output_bytes}};
    return send_message(ui, &message, type);
}

int
snag_ui_event(struct snag_ui *ui, uint64_t seq, const char *type)
{
    struct ui_message message = {.kind = UI_EVENT, .data.seq = seq};
    return send_message(ui, &message, type);
}

int
snag_ui_resume_hint(struct snag_ui *ui, const char *text, size_t len)
{
    struct ui_message message = {.kind = UI_RESUME};
    return request(ui, &message, NULL, text, len, NULL, NULL, 0u);
}

int
snag_ui_protocol(struct snag_ui *ui, const char *label, const char *text, size_t len)
{
    struct ui_message message = {.kind = UI_PROTOCOL};
    return request(ui, &message, label, text, len, NULL, NULL, 0u);
}

int
snag_ui_transport(struct snag_ui *ui, char direction, const char *text, size_t len)
{
    struct ui_message message = {.kind = UI_TRANSPORT, .data.value = direction};
    return request(ui, &message, NULL, text, len, NULL, NULL, 0u);
}

int
snag_ui_raw(struct snag_ui *ui, int fd, const char *text, size_t len)
{
    struct ui_message message = {.kind = UI_RAW, .data.value = (unsigned int)fd};
    return request(ui, &message, NULL, text, len, NULL, NULL, 0u);
}

static int
history_snapshot(struct snag_ui *ui, bool refresh)
{
    struct ui_message message = {
        .kind = UI_HISTORY_SNAPSHOT, .data.history.refresh = refresh
    };
    if (snag_history_snapshot_copy(&message.data.history.entries,
                                 &ui->history.snapshot) < 0)
        return -1;
    return send_message(ui, &message, NULL);
}

int
snag_ui_history_open(struct snag_ui *ui, const char *dotdir)
{
    int rc = snag_history_open(&ui->history, dotdir);
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
