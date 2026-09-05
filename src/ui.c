/* SPDX-License-Identifier: GPL-2.0-only */
#include "ui.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum ui_kind {
    UI_TEXT, UI_COLOR, UI_MARKDOWN, UI_NETWORKED, UI_COMMANDS, UI_PAUSE,
    UI_OPEN, UI_EXTERNAL, UI_PROMPT, UI_SPINNERS, UI_DRAFT,
    UI_VIEW, UI_SUBMITTED, UI_PUBLIC_BEGIN, UI_PUBLIC,
    UI_ORIENTATION, UI_HISTORY, UI_IRC, UI_TOOL, UI_EVENT,
    UI_RESUME, UI_PROTOCOL, UI_TRANSPORT, UI_RAW
};

struct ui_message {
    enum ui_kind kind;
    unsigned int verbosity;
    char *text;
    char *label;
    size_t len;
    union {
        enum snj_ui_operation operation;
        unsigned int value;
        struct { bool enabled; } network;
        struct { bool active; uint32_t rate; unsigned int states;
                 char frames[SNJ_TERM_SPINNER_COUNT][80]; } prompt;
        struct { int fd; bool rollout; } public;
        struct { uint64_t turns; size_t queued; bool resumed; } orientation;
        struct snj_irc_event irc;
        struct { enum snj_render_role role; size_t colored_len; } tool;
        uint64_t seq;
        struct { struct snj_term_command *items; size_t count; } commands;
    } data;
};

struct snj_ui_display {
    struct snj_render render;
    struct snj_term term;
    char frames[SNJ_TERM_SPINNER_COUNT][80];
    struct ui_message commands;
};

static void
message_free(struct ui_message *message)
{
    if (message->kind == UI_COMMANDS) {
        for (size_t i = 0u; i < message->data.commands.count; ++i) {
            free((char *)message->data.commands.items[i].syntax);
            free((char *)message->data.commands.items[i].description);
        }
        free(message->data.commands.items);
    }
    free(message->text);
    free(message->label);
}

static int
apply_text(struct snj_ui_display *display, const struct ui_message *message)
{
    struct snj_render *render = &display->render;
    struct snj_term *term = &display->term;

    switch (message->data.operation) {
    case SNJ_UI_HOST: return snj_render_host(render, message->text);
    case SNJ_UI_RUNTIME: return snj_render_runtime(render, message->text);
    case SNJ_UI_ERROR: return snj_render_error_ctx(render, message->text);
    case SNJ_UI_WARNING: return snj_render_warning_ctx(render, message->text);
    case SNJ_UI_BEFORE_PROMPT: return snj_render_before_prompt(render);
    case SNJ_UI_OUTPUT_BEGIN: return snj_term_output_begin(term, true);
    case SNJ_UI_OUTPUT_END: return snj_term_output_end(term);
    case SNJ_UI_PUBLIC_END: return snj_render_public_end(render);
    case SNJ_UI_PUBLIC_ABORT: return snj_render_public_abort(render);
    case SNJ_UI_ROLLOUT_END: return snj_render_rollout_end(render);
    case SNJ_UI_ROLLOUT_ABORT: return snj_render_rollout_abort(render);
    case SNJ_UI_CLOSE:
        snj_render_attach_term(render, NULL);
        snj_term_close(term);
        return 0;
    }
    errno = EINVAL;
    return -1;
}

static int
apply_message(struct snj_ui_display *display, struct ui_message *message,
              struct snj_buf *delivered, char *error, size_t error_size)
{
    struct snj_render *render = &display->render;
    struct snj_term *term = &display->term;

    render->verbosity = message->verbosity;
    switch (message->kind) {
    case UI_TEXT: return apply_text(display, message);
    case UI_COLOR:
        snj_render_set_color(render, (enum snj_color_mode)message->data.value);
        return 0;
    case UI_MARKDOWN:
        snj_render_set_markdown(render, message->data.value != 0u);
        return 0;
    case UI_NETWORKED:
        snj_render_set_networked(render, message->data.network.enabled,
                                message->text);
        return 0;
    case UI_COMMANDS:
        message_free(&display->commands);
        display->commands = *message;
        snj_term_set_commands(term, message->data.commands.items,
                             message->data.commands.count);
        message->data.commands.items = NULL;
        message->data.commands.count = 0u;
        return 0;
    case UI_PAUSE:
        snj_term_set_typing_pause(term, message->data.value);
        return 0;
    case UI_OPEN:
        if (snj_term_open(term, error, error_size) < 0)
            return -1;
        snj_render_attach_term(render, term);
        return 0;
    case UI_EXTERNAL:
        return message->data.value ?
            snj_term_external_begin(term, error, error_size) :
            snj_term_external_end(term, error, error_size);
    case UI_PROMPT: {
        const char *frames[SNJ_TERM_SPINNER_COUNT];
        memcpy(display->frames, message->data.prompt.frames,
               sizeof(display->frames));
        for (size_t i = 0u; i < SNJ_TERM_SPINNER_COUNT; ++i)
            frames[i] = display->frames[i];
        return snj_term_set_prompt_template(term, message->data.prompt.active,
                    message->text, frames, message->data.prompt.rate,
                    message->data.prompt.states);
    }
    case UI_SPINNERS:
        return snj_term_set_spinner_states(term, message->data.value);
    case UI_DRAFT: return snj_term_restore_draft(term, message->text);
    case UI_VIEW:
        return snj_render_set_view(render,
                                  (enum snj_render_view)message->data.value);
    case UI_SUBMITTED:
        return message->data.value ?
            snj_render_input_submitted(render, message->label, message->text) :
            snj_render_submitted(render, message->label, message->text);
    case UI_PUBLIC_BEGIN:
        return message->data.public.rollout ?
            snj_render_rollout_begin(render, message->data.public.fd,
                                    message->label) :
            snj_render_public_begin(render, message->data.public.fd,
                                   message->label);
    case UI_PUBLIC:
        return message->data.public.rollout ?
            snj_render_rollout(render, message->text, message->len, delivered) :
            snj_render_public(render, message->text, message->len, delivered);
    case UI_ORIENTATION:
        return snj_render_orientation(render, message->text, message->label,
                    message->data.orientation.turns,
                    message->data.orientation.queued,
                    message->data.orientation.resumed);
    case UI_HISTORY:
        return snj_render_history(render, message->label, message->text);
    case UI_IRC: return snj_render_irc_event(render, &message->data.irc);
    case UI_TOOL: {
        struct snj_render_block block = {
            .text = {.data = (unsigned char *)message->text,
                     .len = message->len},
            .colored_len = message->data.tool.colored_len,
            .role = message->data.tool.role
        };
        return snj_render_tool_block(render, &block);
    }
    case UI_EVENT:
        return snj_render_event(render, message->data.seq, message->text);
    case UI_RESUME:
        return snj_render_resume_hint(render, message->text, message->len);
    case UI_PROTOCOL:
        return snj_render_protocol(render, message->label, message->text,
                                   message->len);
    case UI_TRANSPORT:
        return snj_render_transport(render, (char)message->data.value,
                                    message->text, message->len);
    case UI_RAW:
        return snj_write_full((int)message->data.value,
                             message->text, message->len);
    }
    errno = EINVAL;
    return -1;
}

static void
snapshot(struct snj_ui *ui)
{
    const struct snj_term *term = &ui->display->term;
    ui->view = snj_render_view(&ui->display->render);
    ui->opened = term->opened;
    ui->prompt_wanted = term->prompt_wanted;
    ui->active = term->active;
    memcpy(ui->label, term->label, sizeof(ui->label));
}

static int
request(struct snj_ui *ui, struct ui_message *message, const char *label,
        const char *text, size_t len, struct snj_buf *delivered,
        char *error, size_t error_size)
{
    int rc = -1, saved;
    message->verbosity = ui->verbosity;
    message->len = len;
    if (label && !(message->label = snj_strdup_checked(label, SIZE_MAX)))
        goto out;
    if (text) {
        if (len == SIZE_MAX || !(message->text = malloc(len + 1u)))
            goto out;
        memcpy(message->text, text, len);
        message->text[len] = '\0';
    }
    rc = apply_message(ui->display, message, delivered, error, error_size);
    snapshot(ui);
out:
    saved = errno;
    message_free(message);
    errno = saved;
    return rc;
}

static int
send_message(struct snj_ui *ui, struct ui_message *message, const char *text)
{
    return request(ui, message, NULL, text, text ? strlen(text) : 0u,
                   NULL, NULL, 0u);
}

int
snj_ui_init(struct snj_ui *ui)
{
    memset(ui, 0, sizeof(*ui));
    ui->display = calloc(1u, sizeof(*ui->display));
    if (!ui->display)
        return -1;
    snj_term_init(&ui->display->term);
    snj_render_init(&ui->display->render, 0u);
    ui->view = SNJ_RENDER_ROLLOUT;
    return 0;
}

void
snj_ui_free(struct snj_ui *ui)
{
    if (!ui->display)
        return;
    snj_render_free(&ui->display->render);
    snj_term_close(&ui->display->term);
    message_free(&ui->display->commands);
    free(ui->display);
    ui->display = NULL;
}

int
snj_ui_text(struct snj_ui *ui, enum snj_ui_operation op, const char *text)
{
    struct ui_message message = {.kind = UI_TEXT, .data.operation = op};
    return send_message(ui, &message, text);
}

int
snj_ui_color(struct snj_ui *ui, enum snj_color_mode mode)
{
    struct ui_message message = {.kind = UI_COLOR, .data.value = mode};
    return send_message(ui, &message, NULL);
}

int
snj_ui_markdown(struct snj_ui *ui, bool enabled)
{
    struct ui_message message = {.kind = UI_MARKDOWN, .data.value = enabled};
    return send_message(ui, &message, NULL);
}

int
snj_ui_networked(struct snj_ui *ui, bool enabled, const char *nick)
{
    struct ui_message message = {
        .kind = UI_NETWORKED, .data.network.enabled = enabled
    };
    return send_message(ui, &message, nick);
}

int
snj_ui_commands(struct snj_ui *ui, const struct snj_term_command *commands,
                size_t count)
{
    struct ui_message message = {.kind = UI_COMMANDS};
    message.data.commands.items = calloc(count, sizeof(*commands));
    if (!message.data.commands.items)
        return -1;
    message.data.commands.count = count;
    for (size_t i = 0u; i < count; ++i) {
        message.data.commands.items[i].syntax =
            snj_strdup_checked(commands[i].syntax, 4096u);
        message.data.commands.items[i].description =
            snj_strdup_checked(commands[i].description, 4096u);
        if (!message.data.commands.items[i].syntax ||
            !message.data.commands.items[i].description) {
            message_free(&message);
            return -1;
        }
    }
    return send_message(ui, &message, NULL);
}

int
snj_ui_typing_pause(struct snj_ui *ui, uint32_t ms)
{
    struct ui_message message = {.kind = UI_PAUSE, .data.value = ms};
    return send_message(ui, &message, NULL);
}

uint32_t
snj_ui_pause_remaining(struct snj_ui *ui)
{
    return snj_term_typing_pause_remaining(&ui->display->term, snj_time_ms());
}

int
snj_ui_open(struct snj_ui *ui, char *error, size_t error_size)
{
    struct ui_message message = {.kind = UI_OPEN};
    if (ui->opened)
        return 0;
    return request(ui, &message, NULL, NULL, 0u, NULL, error, error_size);
}

int
snj_ui_external(struct snj_ui *ui, bool begin, char *error, size_t error_size)
{
    struct ui_message message = {.kind = UI_EXTERNAL, .data.value = begin};
    return request(ui, &message, NULL, NULL, 0u, NULL, error, error_size);
}

int
snj_ui_prompt(struct snj_ui *ui, bool active, const char *label,
              const char *const spinners[SNJ_TERM_SPINNER_COUNT],
              uint32_t per_second, unsigned int states)
{
    struct ui_message message = {
        .kind = UI_PROMPT,
        .data.prompt = {.active = active, .rate = per_second, .states = states}
    };
    for (size_t i = 0u; i < SNJ_TERM_SPINNER_COUNT; ++i) {
        if (strlen(spinners[i]) >= sizeof(message.data.prompt.frames[i])) {
            errno = EOVERFLOW;
            return -1;
        }
        memcpy(message.data.prompt.frames[i], spinners[i],
               strlen(spinners[i]) + 1u);
    }
    return send_message(ui, &message, label);
}

int
snj_ui_simple_prompt(struct snj_ui *ui, bool active)
{
    const char *frames[SNJ_TERM_SPINNER_COUNT] = {" ", " ", " "};
    return snj_ui_prompt(ui, active, active ? "» " : "› ", frames, 1u, 0u);
}

int
snj_ui_spinner_states(struct snj_ui *ui, unsigned int states)
{
    struct ui_message message = {.kind = UI_SPINNERS, .data.value = states};
    return send_message(ui, &message, NULL);
}

int
snj_ui_restore_draft(struct snj_ui *ui, const char *text)
{
    struct ui_message message = {.kind = UI_DRAFT};
    return send_message(ui, &message, text);
}

int
snj_ui_poll(struct snj_ui *ui, int timeout_ms,
            enum snj_term_action *action, char **text)
{
    int rc = snj_term_poll(&ui->display->term, timeout_ms, action, text);
    snapshot(ui);
    return rc;
}

int
snj_ui_set_view(struct snj_ui *ui, enum snj_render_view view)
{
    struct ui_message message = {.kind = UI_VIEW, .data.value = view};
    return send_message(ui, &message, NULL);
}

int
snj_ui_submitted(struct snj_ui *ui, const char *label, const char *text, bool input)
{
    struct ui_message message = {.kind = UI_SUBMITTED, .data.value = input};
    return request(ui, &message, label, text, strlen(text), NULL, NULL, 0u);
}

int
snj_ui_public_begin(struct snj_ui *ui, int fd, const char *label, bool rollout)
{
    struct ui_message message = {
        .kind = UI_PUBLIC_BEGIN, .data.public = {.fd = fd, .rollout = rollout}
    };
    return request(ui, &message, label, NULL, 0u, NULL, NULL, 0u);
}

int
snj_ui_public(struct snj_ui *ui, const char *text, size_t len,
              struct snj_buf *delivered, bool rollout)
{
    struct ui_message message = {
        .kind = UI_PUBLIC, .data.public.rollout = rollout
    };
    return request(ui, &message, NULL, text, len, delivered, NULL, 0u);
}

int
snj_ui_orientation(struct snj_ui *ui, const struct snj_session *session,
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
snj_ui_history(struct snj_ui *ui, const struct snj_session *session)
{
    struct ui_message message = {.kind = UI_HISTORY};
    return request(ui, &message, session->last_user, session->last_assistant,
                   session->last_assistant ? strlen(session->last_assistant) : 0u,
                   NULL, NULL, 0u);
}

int
snj_ui_irc_event(struct snj_ui *ui, const struct snj_irc_event *event)
{
    struct ui_message message = {.kind = UI_IRC, .data.irc = *event};
    return send_message(ui, &message, NULL);
}

static int
tool_block(struct snj_ui *ui, struct snj_render_block *block)
{
    struct ui_message message = {
        .kind = UI_TOOL, .data.tool = {
            .role = block->role, .colored_len = block->colored_len}
    };
    int rc = request(ui, &message, NULL, (char *)block->text.data,
                      block->text.len, NULL, NULL, 0u);
    snj_buf_free(&block->text);
    return rc;
}

int
snj_ui_tool_start(struct snj_ui *ui, const struct snj_response_item *call,
                  const char *workdir, uint32_t timeout_ms)
{
    struct snj_render_block block;
    if (ui->verbosity < 1u)
        return 0;
    if (snj_render_prepare_tool_start(&block, call, workdir, timeout_ms) < 0)
        return -1;
    return tool_block(ui, &block);
}

int
snj_ui_tool_finish(struct snj_ui *ui, const char *name, const json_t *result,
                   uint32_t max_output_bytes)
{
    struct snj_render_block block;
    if (ui->verbosity < 1u)
        return 0;
    if (snj_render_prepare_tool_finish(&block, name, result, max_output_bytes) < 0)
        return -1;
    return tool_block(ui, &block);
}

int
snj_ui_event(struct snj_ui *ui, uint64_t seq, const char *type)
{
    struct ui_message message = {.kind = UI_EVENT, .data.seq = seq};
    return send_message(ui, &message, type);
}

int
snj_ui_resume_hint(struct snj_ui *ui, const char *text, size_t len)
{
    struct ui_message message = {.kind = UI_RESUME};
    return request(ui, &message, NULL, text, len, NULL, NULL, 0u);
}

int
snj_ui_protocol(struct snj_ui *ui, const char *label, const char *text, size_t len)
{
    struct ui_message message = {.kind = UI_PROTOCOL};
    return request(ui, &message, label, text, len, NULL, NULL, 0u);
}

int
snj_ui_transport(struct snj_ui *ui, char direction, const char *text, size_t len)
{
    struct ui_message message = {.kind = UI_TRANSPORT, .data.value = direction};
    return request(ui, &message, NULL, text, len, NULL, NULL, 0u);
}

int
snj_ui_raw(struct snj_ui *ui, int fd, const char *text, size_t len)
{
    struct ui_message message = {.kind = UI_RAW, .data.value = (unsigned int)fd};
    return request(ui, &message, NULL, text, len, NULL, NULL, 0u);
}

int
snj_ui_history_open(struct snj_ui *ui, const char *dotdir)
{
    return snj_term_history_open(&ui->display->term, dotdir);
}

int
snj_ui_history_add(struct snj_ui *ui, const char *text)
{
    return snj_term_history_add(&ui->display->term, text);
}

bool
snj_ui_history_warning(struct snj_ui *ui)
{
    return snj_term_take_history_warning(&ui->display->term);
}
