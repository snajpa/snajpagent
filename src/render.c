/* SPDX-License-Identifier: GPL-2.0-only */
#include "render.h"
#include "base.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int
write_literal(int fd, const char *s)
{
    return snj_write_full(fd, s, strlen(s));
}

static int
output_begin(struct snj_render *render, bool persistent)
{
    return render->term ? snj_term_output_begin(render->term, persistent) : 0;
}

static int
output_end(struct snj_render *render)
{
    return render->term ? snj_term_output_end(render->term) : 0;
}

static int
write_block(struct snj_render *render, int fd, const char *text, size_t len,
            bool terminal_safe, bool persistent)
{
    int rc;
    int saved_errno = 0;

    if (output_begin(render, persistent) < 0)
        return -1;
    rc = terminal_safe ? snj_term_write_safe(fd, text, len) :
                         snj_write_full(fd, text, len);
    if (rc < 0)
        saved_errno = errno;
    if (output_end(render) < 0 && rc == 0)
        rc = -1;
    if (saved_errno)
        errno = saved_errno;
    return rc;
}

void
snj_render_init(struct snj_render *render, unsigned int verbosity)
{
    memset(render, 0, sizeof(*render));
    render->verbosity = verbosity > 6u ? 6u : verbosity;
    render->public_fd = -1;
    render->stdout_terminal = isatty(STDOUT_FILENO) == 1;
    render->stderr_terminal = isatty(STDERR_FILENO) == 1;
}

void
snj_render_attach_term(struct snj_render *render, struct snj_term *term)
{
    render->term = term;
}

int
snj_render_orientation(struct snj_render *render,
                       const struct snj_session *session, bool resumed)
{
    struct snj_buf line;
    int rc;

    snj_buf_init(&line, 32768u);
    if (resumed) {
        rc = snj_buf_printf(&line,
            "snajpagent · resumed %.8s · %s · %s · %llu turns · %zu queued%s\n",
            session->id, session->default_model, session->workspace,
            (unsigned long long)session->turn_count,
            session->pending_queue_count,
            session->pending_queue_count ? " paused" : "");
    } else {
        rc = snj_buf_printf(&line, "snajpagent · %s · %s · %.8s\n",
                            session->default_model, session->workspace, session->id);
    }
    if (rc == 0)
        rc = write_block(render, STDERR_FILENO, (char *)line.data, line.len,
                         render->stderr_terminal, true);
    snj_buf_free(&line);
    return rc;
}

int
snj_render_history(struct snj_render *render, const struct snj_session *session)
{
    struct snj_buf line;
    int rc = 0;

    if (!session->last_user && !session->last_assistant)
        return 0;
    snj_buf_init(&line, 4u * 1024u * 1024u);
    if (snj_buf_append(&line, "--- recent history ---\n", 23u) < 0 ||
        (session->last_user &&
         (snj_buf_append(&line, "user: ", 6u) < 0 ||
          snj_buf_append(&line, session->last_user, strlen(session->last_user)) < 0 ||
          snj_buf_putc(&line, '\n') < 0)) ||
        (session->last_assistant &&
         (snj_buf_append(&line, "assistant: ", 11u) < 0 ||
          snj_buf_append(&line, session->last_assistant,
                         strlen(session->last_assistant)) < 0 ||
          snj_buf_putc(&line, '\n') < 0)) ||
        snj_buf_append(&line, "--- end history ---\n", 20u) < 0)
        rc = -1;
    else
        rc = write_block(render, STDERR_FILENO, (char *)line.data, line.len,
                         render->stderr_terminal, true);
    snj_buf_free(&line);
    return rc;
}

int
snj_render_prompt(struct snj_render *render, const char *label)
{
    return write_block(render, STDERR_FILENO, label, strlen(label), false, false);
}

int
snj_render_submitted(struct snj_render *render, const char *label, const char *text)
{
    struct snj_buf line;
    int rc;

    if (render->term &&
        snj_term_consume_echoed_submission(render->term, label))
        return 0;
    snj_buf_init(&line, SNJ_MAX_DIRECT_PROMPT * 8u + 64u);
    if (render->public_item_open && !render->public_item_ended_lf) {
        if (snj_buf_putc(&line, '\n') < 0) {
            snj_buf_free(&line);
            return -1;
        }
        render->public_item_ended_lf = true;
    }
    rc = snj_buf_append(&line, label, strlen(label));
    if (rc == 0)
        rc = snj_buf_append(&line, text, strlen(text));
    if (rc == 0)
        rc = snj_buf_putc(&line, '\n');
    if (rc == 0)
        rc = write_block(render, STDERR_FILENO, (char *)line.data, line.len,
                         render->stderr_terminal, true);
    snj_buf_free(&line);
    return rc;
}

static size_t
utf8_sequence_size(unsigned char first)
{
    if (first < 0x80u)
        return 1u;
    if (first >= 0xc2u && first <= 0xdfu)
        return 2u;
    if (first >= 0xe0u && first <= 0xefu)
        return 3u;
    if (first >= 0xf0u && first <= 0xf4u)
        return 4u;
    return 0u;
}

int
snj_render_public_begin(struct snj_render *render, int fd, const char *label)
{
    if (render->public_item_open || render->utf8_pending_len) {
        errno = EBUSY;
        return -1;
    }
    if (fd != STDOUT_FILENO && fd != STDERR_FILENO) {
        errno = EINVAL;
        return -1;
    }
    if (output_begin(render, true) < 0)
        return -1;
    render->public_output_open = true;
    if (fd == STDOUT_FILENO && render->stdout_item_seen &&
        !render->stdout_item_ended_lf && !render->stdout_terminal &&
        write_literal(STDOUT_FILENO, "\n") < 0)
        goto fail;
    if (label && write_literal(fd, label) < 0)
        goto fail;
    render->public_fd = fd;
    render->public_item_open = true;
    render->public_item_bytes = label && *label;
    render->public_item_ended_lf = label && *label && label[strlen(label) - 1u] == '\n';
    return 0;
fail:
    {
        int saved_errno = errno;
        if (render->public_output_open)
            (void)output_end(render);
        render->public_output_open = false;
        errno = saved_errno;
        return -1;
    }
}

int
snj_render_public(struct snj_render *render, const char *text, size_t len,
                  struct snj_buf *delivered)
{
    const unsigned char *input = (const unsigned char *)text;
    struct snj_buf complete;
    size_t complete_max;
    int rc = -1;

    if (!snj_size_add(len, sizeof(render->utf8_pending), &complete_max)) {
        errno = EOVERFLOW;
        return -1;
    }
    snj_buf_init(&complete, complete_max);
    for (size_t i = 0; i < len; ++i) {
        size_t expected;

        if (render->utf8_pending_len >= sizeof(render->utf8_pending))
            goto invalid;
        render->utf8_pending[render->utf8_pending_len++] = input[i];
        expected = utf8_sequence_size(render->utf8_pending[0]);
        if (!expected || render->utf8_pending_len > expected)
            goto invalid;
        if (render->utf8_pending_len < expected)
            continue;
        if (!snj_utf8_valid(render->utf8_pending, expected, true))
            goto invalid;
        if (snj_buf_append(&complete, render->utf8_pending, expected) < 0)
            goto out;
        render->utf8_pending_len = 0;
    }
    if (complete.len) {
        bool terminal = render->public_fd == STDOUT_FILENO ?
                        render->stdout_terminal : render->stderr_terminal;
        if (!render->public_item_open) {
            errno = EINVAL;
            goto out;
        }
        if (!render->public_output_open) {
            if (output_begin(render, true) < 0)
                goto out;
            render->public_output_open = true;
        }
        if (delivered && snj_buf_reserve(delivered, complete.len) < 0)
            goto out;
        rc = terminal ?
             snj_term_write_safe(render->public_fd, (const char *)complete.data,
                                 complete.len) :
             snj_write_full(render->public_fd, complete.data, complete.len);
        if (rc < 0)
            goto out;
        if (delivered && snj_buf_append(delivered, complete.data, complete.len) < 0)
            goto out;
        render->public_item_bytes = true;
        render->public_item_ended_lf = complete.data[complete.len - 1u] == '\n';
        if (render->public_item_ended_lf && render->public_output_open) {
            if (output_end(render) < 0)
                goto out;
            render->public_output_open = false;
        }
    }
    rc = 0;
    goto out;
invalid:
    render->utf8_pending_len = 0;
    errno = EILSEQ;
out:
    snj_buf_free(&complete);
    return rc;
}

static int
close_public_item(struct snj_render *render, bool discard_incomplete)
{
    int fd;
    bool ended_lf;
    bool had_bytes;
    bool output_open;
    bool invalid = false;
    int rc = 0;
    int saved_errno = 0;

    if (render->utf8_pending_len) {
        render->utf8_pending_len = 0;
        invalid = !discard_incomplete;
    }
    if (!render->public_item_open) {
        if (invalid) {
            errno = EILSEQ;
            return -1;
        }
        return 0;
    }
    fd = render->public_fd;
    ended_lf = render->public_item_ended_lf;
    had_bytes = render->public_item_bytes;
    render->public_item_open = false;
    output_open = render->public_output_open;
    render->public_output_open = false;
    render->public_item_bytes = false;
    render->public_item_ended_lf = false;
    render->public_fd = -1;
    if (fd == STDOUT_FILENO && had_bytes) {
        render->stdout_item_seen = true;
        render->stdout_item_ended_lf = ended_lf;
        if (!ended_lf && render->stderr_terminal &&
            write_literal(STDERR_FILENO, "\n") < 0)
            rc = -1;
    } else if (fd == STDERR_FILENO && had_bytes && !ended_lf &&
               write_literal(STDERR_FILENO, "\n") < 0) {
        rc = -1;
    }
    if (rc < 0)
        saved_errno = errno;
    if (output_open && output_end(render) < 0 && rc == 0) {
        rc = -1;
        saved_errno = errno;
    }
    if (saved_errno)
        errno = saved_errno;
    if (invalid && rc == 0) {
        errno = EILSEQ;
        rc = -1;
    }
    return rc;
}

int
snj_render_public_end(struct snj_render *render)
{
    return close_public_item(render, false);
}

int
snj_render_public_abort(struct snj_render *render)
{
    return close_public_item(render, true);
}

static int
render_message(struct snj_render *render, const char *message)
{
    struct snj_buf line;
    int rc;

    snj_buf_init(&line, 16384u);
    rc = snj_buf_printf(&line, "snajpagent: %s\n", message);
    if (rc == 0)
        rc = write_block(render, STDERR_FILENO, (char *)line.data, line.len,
                         render->stderr_terminal, true);
    snj_buf_free(&line);
    return rc;
}

int
snj_render_error(const char *message)
{
    struct snj_render render;
    snj_render_init(&render, 0u);
    return render_message(&render, message);
}

int
snj_render_warning(const char *message)
{
    return snj_render_error(message);
}

int
snj_render_error_ctx(struct snj_render *render, const char *message)
{
    return render_message(render, message);
}

int
snj_render_warning_ctx(struct snj_render *render, const char *message)
{
    return render_message(render, message);
}

int
snj_render_activity(struct snj_render *render, const char *message)
{
    return render->term ? snj_term_set_status(render->term, message) : 0;
}

int
snj_render_host(struct snj_render *render, const char *text)
{
    size_t len = strlen(text);
    struct snj_buf line;
    int rc;

    snj_buf_init(&line, 4u * 1024u * 1024u);
    rc = snj_buf_append(&line, text, len);
    if (rc == 0 && (len == 0u || text[len - 1u] != '\n'))
        rc = snj_buf_putc(&line, '\n');
    if (rc == 0)
        rc = write_block(render, STDERR_FILENO, (char *)line.data, line.len,
                         render->stderr_terminal, true);
    snj_buf_free(&line);
    return rc;
}

int
snj_render_runtime(struct snj_render *render, const char *text)
{
    if (render->verbosity < 3u)
        return 0;
    return snj_render_host(render, text);
}

static const char *
tool_label(const char *name)
{
    if (strcmp(name, "exec_command") == 0)
        return "exec";
    if (strcmp(name, "write_stdin") == 0)
        return "stdin";
    if (strcmp(name, "apply_patch") == 0)
        return "patch";
    return name;
}

static size_t
utf8_prefix(const char *text, size_t limit)
{
    size_t len = strlen(text);
    if (len <= limit)
        return len;
    while (limit && (((unsigned char)text[limit] & 0xc0u) == 0x80u))
        --limit;
    return limit;
}

static int
append_shell_quoted(struct snj_buf *line, const char *text)
{
    size_t full = strlen(text);
    size_t shown = utf8_prefix(text, 2048u);

    if (snj_buf_putc(line, '\'') < 0)
        return -1;
    for (size_t i = 0; i < shown; ++i) {
        if (text[i] == '\'') {
            if (snj_buf_append(line, "'\\''", 4u) < 0)
                return -1;
        } else if (snj_buf_putc(line, (unsigned char)text[i]) < 0) {
            return -1;
        }
    }
    if (snj_buf_putc(line, '\'') < 0)
        return -1;
    if (shown != full &&
        snj_buf_printf(line, " … <%zu bytes omitted>", full - shown) < 0)
        return -1;
    return 0;
}

int
snj_render_tool_start(struct snj_render *render,
                      const struct snj_response_item *call,
                      const char *workdir)
{
    struct snj_buf line;
    const char *label;
    const char *command;
    int rc = 0;

    if (render->verbosity < 1u)
        return 0;
    label = tool_label(call->name);
    command = snj_json_string(call->arguments, "command");
    snj_buf_init(&line, SNJ_MAX_TOOL_ARGUMENTS * 2u + 4096u);
    if (snj_buf_printf(&line, "→ %s", label) < 0)
        rc = -1;
    else if (command) {
        if (snj_buf_append(&line, "  ", 2u) < 0 ||
            append_shell_quoted(&line, command) < 0)
            rc = -1;
    }
    if (rc == 0 && snj_buf_putc(&line, '\n') < 0)
        rc = -1;
    if (rc == 0 && render->verbosity >= 2u) {
        struct snj_buf encoded;
        snj_buf_init(&encoded, SNJ_MAX_TOOL_ARGUMENTS + 64u);
        if (snj_json_canonical(call->arguments, &encoded) < 0 ||
            snj_buf_printf(&line, "  workdir: %s\n  arguments: ", workdir) < 0 ||
            snj_buf_append(&line, encoded.data, encoded.len) < 0 ||
            snj_buf_putc(&line, '\n') < 0)
            rc = -1;
        snj_buf_free(&encoded);
    }
    if (rc == 0)
        rc = write_block(render, STDERR_FILENO, (char *)line.data, line.len,
                         render->stderr_terminal, true);
    snj_buf_free(&line);
    return rc;
}

int
snj_render_tool_finish(struct snj_render *render, const char *name,
                       const json_t *result)
{
    struct snj_buf line;
    const char *status;
    const char *reason;
    json_t *exit_value;
    uint64_t duration = 0u;
    int rc = 0;

    if (render->verbosity < 1u)
        return 0;
    status = snj_json_string(result, "status");
    reason = snj_json_string(result, "reason");
    exit_value = json_object_get(result, "exit_code");
    (void)snj_json_integer_u64(result, "duration_ms", &duration);
    snj_buf_init(&line, 2u * 1024u * 1024u + 4096u);
    if (snj_buf_printf(&line, "← %s  ", tool_label(name)) < 0)
        rc = -1;
    else if (json_is_integer(exit_value)) {
        if (snj_buf_printf(&line, "exit %lld",
                           (long long)json_integer_value(exit_value)) < 0)
            rc = -1;
    } else if (status && strcmp(status, "not_run") == 0) {
        if (snj_buf_printf(&line, "not run%s%s", reason ? " · " : "",
                           reason ? reason : "") < 0)
            rc = -1;
    } else if (status && strcmp(status, "outcome_unknown") == 0) {
        if (snj_buf_append(&line, "outcome unknown", 15u) < 0)
            rc = -1;
    } else if (snj_buf_append(&line, status ? status : "unknown",
                              strlen(status ? status : "unknown")) < 0) {
        rc = -1;
    }
    if (rc == 0) {
        if (duration < 1000u)
            rc = snj_buf_printf(&line, " · %llums\n",
                                (unsigned long long)duration);
        else
            rc = snj_buf_printf(&line, " · %llu.%llus\n",
                    (unsigned long long)(duration / 1000u),
                    (unsigned long long)((duration % 1000u) / 100u));
    }
    if (rc == 0 && render->verbosity >= 2u) {
        struct snj_buf encoded;
        snj_buf_init(&encoded, 1024u * 1024u);
        if (snj_json_canonical(result, &encoded) < 0 ||
            snj_buf_append(&line, "  result: ", 10u) < 0 ||
            snj_buf_append(&line, encoded.data, encoded.len) < 0 ||
            snj_buf_putc(&line, '\n') < 0)
            rc = -1;
        snj_buf_free(&encoded);
    }
    if (rc == 0)
        rc = write_block(render, STDERR_FILENO, (char *)line.data, line.len,
                         render->stderr_terminal, true);
    snj_buf_free(&line);
    return rc;
}

int
snj_render_event(struct snj_render *render, uint64_t seq, const char *type)
{
    struct snj_buf line;
    int rc = 0;

    if (render->verbosity < 4u)
        return 0;
    snj_buf_init(&line, 1024u);
    if (snj_buf_printf(&line, "event › %llu %s synced\n",
                       (unsigned long long)seq, type) < 0)
        rc = -1;
    if (rc == 0)
        rc = write_block(render, STDERR_FILENO, (char *)line.data, line.len,
                         render->stderr_terminal, true);
    snj_buf_free(&line);
    return rc;
}


static bool
diagnostic_text_valid(const char *text, size_t len, bool multiline)
{
    if (!text || !snj_utf8_valid((const unsigned char *)text, len, true))
        return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n' && multiline)
            continue;
        if (c == '\t')
            continue;
        if (c < 0x20u || c == 0x7fu)
            return false;
    }
    return true;
}

static int
protocol_warning(struct snj_render *render)
{
    static const char warning[] =
        "snajpagent: verbosity 5 exposes prompts, source code, tool output, and model content\n";

    if (render->protocol_warning_shown)
        return 0;
    if (write_block(render, STDERR_FILENO, warning, sizeof(warning) - 1u,
                    render->stderr_terminal, true) < 0)
        return -1;
    render->protocol_warning_shown = true;
    return 0;
}

int
snj_render_protocol(struct snj_render *render, const char *label,
                    const char *text, size_t len)
{
    struct snj_buf block;
    int rc = -1;

    if (render->verbosity < 5u)
        return 0;
    if (!label || !diagnostic_text_valid(label, strlen(label), false) ||
        !diagnostic_text_valid(text, len, true) ||
        len > 2u * 1024u * 1024u) {
        errno = EINVAL;
        return -1;
    }
    if (protocol_warning(render) < 0)
        return -1;
    snj_buf_init(&block, 2u * 1024u * 1024u + 4096u);
    if (snj_buf_printf(&block, "protocol › %s\n", label) < 0 ||
        snj_buf_append(&block, text, len) < 0 ||
        (len && text[len - 1u] != '\n' && snj_buf_putc(&block, '\n') < 0))
        goto out;
    rc = write_block(render, STDERR_FILENO, (const char *)block.data, block.len,
                     render->stderr_terminal, true);
out:
    snj_buf_free(&block);
    return rc;
}

int
snj_render_transport(struct snj_render *render, char direction,
                     const char *text, size_t len)
{
    struct snj_buf line;
    int rc = -1;

    if (render->verbosity < 6u)
        return 0;
    if ((direction != '>' && direction != '<') ||
        !diagnostic_text_valid(text, len, false) || len > 64u * 1024u) {
        errno = EINVAL;
        return -1;
    }
    if (protocol_warning(render) < 0)
        return -1;
    snj_buf_init(&line, 64u * 1024u + 4u);
    if (snj_buf_putc(&line, (unsigned char)direction) < 0 ||
        snj_buf_putc(&line, ' ') < 0 ||
        snj_buf_append(&line, text, len) < 0 || snj_buf_putc(&line, '\n') < 0)
        goto out;
    rc = write_block(render, STDERR_FILENO, (const char *)line.data, line.len,
                     render->stderr_terminal, true);
out:
    snj_buf_free(&line);
    return rc;
}
