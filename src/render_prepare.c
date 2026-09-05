/* SPDX-License-Identifier: GPL-2.0-only */
#include "render.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int
preview(struct snj_buf *out, const char *text, size_t len, size_t characters,
        size_t cells, bool single_line, bool *truncated)
{
    size_t used = 0u, width = 0u;
    struct snj_buf safe;
    int rc = -1;

    snj_buf_init(&safe, 32u);

    for (size_t i = 0u; i < len;) {
        unsigned char c = (unsigned char)text[i];
        size_t n = snj_utf8_size(c), count = 1u;
        const char *piece = text + i;
        char escaped[8];
        size_t bytes;

        if (!n || n > len - i) {
            *truncated = true;
            break;
        }
        bytes = n;
        if (c < 0x20u || c == 0x7fu) {
            if (c == '\n' && !single_line) {
                /* Body line breaks count toward the same total budget. */
            } else {
                if (c == '\n' || c == '\r' || c == '\t') {
                    escaped[0] = '\\';
                    escaped[1] = c == '\n' ? 'n' : c == '\r' ? 'r' : 't';
                    bytes = 2u;
                } else {
                    (void)snprintf(escaped, sizeof(escaped), "\\x%02x", c);
                    bytes = 4u;
                }
                piece = escaped;
                count = bytes;
            }
        } else if (n > 1u) {
            snj_buf_reset(&safe);
            if (snj_term_append_safe(&safe, piece, n) < 0)
                goto out;
            if (safe.len != n || memcmp(safe.data, piece, n) != 0) {
                piece = (const char *)safe.data;
                bytes = count = safe.len;
            }
        }
        size_t w = snj_term_text_width(piece, bytes);
        if (count > characters - used || w > cells - width ||
            bytes > out->max - out->len) {
            *truncated = true;
            break;
        }
        if (snj_buf_append(out, piece, bytes) < 0)
            goto out;
        used += count;
        width += w;
        i += n;
    }
    rc = 0;
out:
    snj_buf_free(&safe);
    return rc;
}

static int
canonical_prefix(const json_t *value, struct snj_buf *out, size_t bytes,
                 bool *truncated)
{
    snj_buf_init(out, bytes);
    if (snj_json_canonical(value, out) == 0)
        return 0;
    if (errno != EOVERFLOW)
        return -1;
    *truncated = true;
    while (out->len && !snj_utf8_valid(out->data, out->len, false))
        --out->len;
    return 0;
}

static void
block_init(struct snj_render_block *block, enum snj_presentation kind)
{
    memset(block, 0, sizeof(*block));
    snj_buf_init(&block->text, 512u);
    snj_buf_init(&block->context, SNJ_PATH_MAX_BYTES * 4u + 128u);
    snj_buf_init(&block->body, SNJ_MAX_RESPONSE_GRAPH * 4u);
    block->body_kind = kind;
}

void
snj_render_block_free(struct snj_render_block *block)
{
    snj_buf_free(&block->text);
    snj_buf_free(&block->context);
    snj_buf_free(&block->body);
}

static int
summary(struct snj_render_block *block, const struct snj_buf *row,
        unsigned int columns, size_t colored_len)
{
    size_t cells = columns && columns <= 120u ? columns - 1u : 120u;
    bool truncated = false;
    int rc;

    /* Reserve an ellipsis and LF before clipping. */
    block->text.max -= 4u;
    rc = preview(&block->text, (const char *)row->data, row->len, SIZE_MAX,
                  cells ? cells - 1u : 0u, true, &truncated);
    block->text.max += 4u;
    if (rc == 0 && truncated && cells)
        rc = snj_buf_append(&block->text, "…", 3u);
    block->colored_len = colored_len < block->text.len ? colored_len : block->text.len;
    return rc < 0 ? -1 : snj_buf_putc(&block->text, '\n');
}

int
snj_render_prepare_tool_start(struct snj_render_block *block,
                              const struct snj_response_item *call,
                              const char *workdir, uint32_t default_timeout_ms,
                              unsigned int level, unsigned int columns)
{
    struct snj_buf row, args;
    bool truncated = false;
    size_t limit = snj_presentation_limit(SNJ_PRESENT_ARGUMENTS, level);
    size_t bytes = limit == SIZE_MAX ? SNJ_MAX_TOOL_ARGUMENTS * 6u + 2u :
                   limit ? limit * 4u + 16u : 512u;
    int rc = -1;

    block_init(block, SNJ_PRESENT_ARGUMENTS);
    block->role = SNJ_ROLE_ACTIVITY;
    snj_buf_init(&row, 4096u);
    if (canonical_prefix(call->arguments, &args, bytes, &truncated) < 0)
        goto out;
    bool arguments_truncated = truncated;
    if (snj_buf_printf(&row, "→ %s", call->name) < 0)
        goto out;
    size_t colored_len = row.len;
    if (snj_buf_append(&row, "  ", 2u) < 0 ||
        preview(&row, (const char *)args.data, args.len, 95u, 95u, true, &truncated) < 0 ||
        (truncated && snj_buf_append(&row, "…", 3u) < 0) ||
        summary(block, &row, columns, colored_len) < 0)
        goto out;
    if (limit) {
        block->truncated = arguments_truncated;
        if (preview(&block->body, (const char *)args.data, args.len,
                      limit, SIZE_MAX, false, &block->truncated) < 0)
            goto out;
    }
    if (snj_presentation_limit(SNJ_PRESENT_CONTEXT, level)) {
        uint64_t timeout = default_timeout_ms;
        const char *explicit_workdir = snj_json_string(call->arguments, "workdir");
        if (explicit_workdir)
            workdir = explicit_workdir;
        (void)snj_json_integer_u64(call->arguments, "timeout_ms", &timeout);
        if (snj_buf_printf(&block->context, "  workdir: %s\n  timeout: ", workdir) < 0 ||
            (timeout ? snj_buf_printf(&block->context, "%llums\n", (unsigned long long)timeout) :
                       snj_buf_append(&block->context, "none\n", 5u)) < 0)
            goto out;
    }
    rc = 0;
out:
    snj_buf_free(&args);
    snj_buf_free(&row);
    if (rc < 0)
        snj_render_block_free(block);
    return rc;
}

int
snj_render_prepare_tool_finish(struct snj_render_block *block, const char *name,
                               const json_t *result, uint32_t max_output_bytes,
                               unsigned int level, unsigned int columns)
{
    struct snj_buf row;
    const char *status = snj_json_string(result, "status");
    const char *output = snj_json_string(result, "model_text");
    const char *handle = snj_json_string(result, "handle");
    json_t *exit_value = json_object_get(result, "exit_code");
    uint64_t duration;
    size_t limit = snj_presentation_limit(SNJ_PRESENT_OUTPUT, level);
    int rc = -1;

    block_init(block, SNJ_PRESENT_OUTPUT);
    block->role = status && strcmp(status, "succeeded") == 0 ? SNJ_ROLE_SUCCESS :
                  status && strcmp(status, "failed") == 0 ? SNJ_ROLE_ERROR : SNJ_ROLE_WARNING;
    snj_buf_init(&row, 4096u);
    if (snj_buf_printf(&row, "← %s  ", name) < 0)
        goto out;
    if (json_is_integer(exit_value)) {
        block->role = json_integer_value(exit_value) ? SNJ_ROLE_ERROR : SNJ_ROLE_SUCCESS;
        if (snj_buf_printf(&row, "exit %lld", (long long)json_integer_value(exit_value)) < 0)
            goto out;
    } else if (snj_buf_printf(&row, "%s", status ? status : "unknown") < 0) {
        goto out;
    }
    if (snj_json_integer_u64(result, "duration_ms", &duration) == 0 &&
        (duration < 1000u ?
         snj_buf_printf(&row, " · %llums", (unsigned long long)duration) :
         snj_buf_printf(&row, " · %llu.%llus", (unsigned long long)(duration / 1000u),
                        (unsigned long long)(duration % 1000u / 100u))) < 0)
        goto out;
    if (handle && snj_buf_printf(&row, " · %s", handle) < 0)
        goto out;
    if (summary(block, &row, columns, row.len) < 0)
        goto out;
    if (output && limit) {
        size_t len = json_string_length(json_object_get(result, "model_text"));
        size_t shown = max_output_bytes && len > max_output_bytes ? max_output_bytes : len;
        while (shown < len && shown && ((unsigned char)output[shown] & 0xc0u) == 0x80u)
            --shown;
        block->truncated = shown < len;
        if (preview(&block->body, output, shown, limit, SIZE_MAX, false, &block->truncated) < 0)
            goto out;
    }
    rc = 0;
out:
    snj_buf_free(&row);
    if (rc < 0)
        snj_render_block_free(block);
    return rc;
}
