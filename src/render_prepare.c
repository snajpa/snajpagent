/* SPDX-License-Identifier: GPL-2.0-only */
#include "render.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static size_t
first_line_len(const char *text, size_t len)
{
    const char *newline = memchr(text, '\n', len);
    return newline ? (size_t)(newline - text) + 1u : len;
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
snj_render_prepare_tool_start(struct snj_render_block *block,
                      const struct snj_response_item *call,
                      const char *workdir, uint32_t default_timeout_ms)
{
    struct snj_buf line;
    const char *label;
    const char *command;
    json_t *timeout_value;
    uint32_t timeout_ms = default_timeout_ms;
    size_t prefix_len = 0u;
    int rc = 0;

    memset(block, 0, sizeof(*block));
    label = tool_label(call->name);
    command = snj_json_string(call->arguments, "command");
    timeout_value = json_object_get(call->arguments, "timeout_ms");
    if (json_is_integer(timeout_value)) {
        json_int_t requested = json_integer_value(timeout_value);

        if (requested >= 0 && (uint64_t)requested <= UINT32_MAX)
            timeout_ms = (uint32_t)requested;
    }
    snj_buf_init(&line, SNJ_MAX_TOOL_ARGUMENTS * 2u + 4096u);
    if (snj_buf_printf(&line, "→ %s", label) < 0)
        rc = -1;
    else if (command) {
        prefix_len = line.len;
        if ((timeout_ms &&
             snj_buf_printf(&line, "  timeout=%ums", timeout_ms) < 0) ||
            (!timeout_ms &&
             snj_buf_append(&line, "  timeout=none", 14u) < 0) ||
            snj_buf_append(&line, "  ", 2u) < 0 ||
            append_shell_quoted(&line, command) < 0)
            rc = -1;
    }
    if (rc == 0 && snj_buf_putc(&line, '\n') < 0)
        rc = -1;
    if (rc == 0) {
        struct snj_buf encoded;
        snj_buf_init(&encoded, SNJ_MAX_TOOL_ARGUMENTS + 64u);
        if (snj_json_canonical(call->arguments, &encoded) < 0 ||
            snj_buf_printf(&line, "  workdir: %s\n  arguments: ", workdir) < 0 ||
            snj_buf_append(&line, encoded.data, encoded.len) < 0 ||
            snj_buf_putc(&line, '\n') < 0)
            rc = -1;
        snj_buf_free(&encoded);
    }
    block->role = SNJ_ROLE_ACTIVITY;
    block->colored_len = command ? prefix_len :
                          first_line_len((char *)line.data, line.len);
    if (rc == 0)
        block->text = line;
    else
        snj_buf_free(&line);
    return rc;
}

int
snj_render_prepare_tool_finish(struct snj_render_block *block, const char *name,
                       const json_t *result, uint32_t max_output_bytes)
{
    struct snj_buf line;
    const char *model_text;
    const char *status;
    const char *reason;
    json_t *exit_value;
    uint64_t duration = 0u;
    enum snj_render_role role = SNJ_ROLE_WARNING;
    int rc = 0;

    memset(block, 0, sizeof(*block));
    status = snj_json_string(result, "status");
    reason = snj_json_string(result, "reason");
    exit_value = json_object_get(result, "exit_code");
    (void)snj_json_integer_u64(result, "duration_ms", &duration);
    if ((status && strcmp(status, "succeeded") == 0) ||
        (json_is_integer(exit_value) && json_integer_value(exit_value) == 0))
        role = SNJ_ROLE_SUCCESS;
    else if ((status && (strcmp(status, "failed") == 0 ||
                         strcmp(status, "outcome_unknown") == 0)) ||
             (json_is_integer(exit_value) && json_integer_value(exit_value) != 0))
        role = SNJ_ROLE_ERROR;
    model_text = snj_json_string(result, "model_text");
    snj_buf_init(&line, SIZE_MAX);
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
    if (rc == 0 && model_text) {
        size_t len = json_string_length(json_object_get(result, "model_text"));
        size_t shown = len;

        if (max_output_bytes && shown > max_output_bytes) {
            shown = max_output_bytes;
            while (shown && shown < len &&
                   ((unsigned char)model_text[shown] & 0xc0u) == 0x80u)
                --shown;
        }
        if (snj_buf_append(&line, "  output:\n", 10u) < 0 ||
            snj_buf_append(&line, model_text, shown) < 0 ||
            (shown && model_text[shown - 1u] != '\n' &&
             snj_buf_putc(&line, '\n') < 0) ||
            (shown < len &&
             snj_buf_printf(&line,
                            "  <%zu output bytes hidden by max_output_bytes>\n",
                            len - shown) < 0))
            rc = -1;
    }
    block->role = role;
    block->colored_len = first_line_len((char *)line.data, line.len);
    if (rc == 0)
        block->text = line;
    else
        snj_buf_free(&line);
    return rc;
}
