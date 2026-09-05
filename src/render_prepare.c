/* SPDX-License-Identifier: GPL-2.0-only */
#include "render.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int
preview(struct snag_buf *out, const char *text, size_t len, size_t characters,
        size_t cells, bool single_line, bool *truncated)
{
    size_t used = 0u, width = 0u;
    struct snag_buf safe;
    int rc = -1;

    snag_buf_init(&safe, 32u);

    for (size_t i = 0u; i < len;) {
        unsigned char c = (unsigned char)text[i];
        size_t n = snag_utf8_size(c), count = 1u;
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
            snag_buf_reset(&safe);
            if (snag_term_append_safe(&safe, piece, n) < 0)
                goto out;
            if (safe.len != n || memcmp(safe.data, piece, n) != 0) {
                piece = (const char *)safe.data;
                bytes = count = safe.len;
            }
        }
        size_t w = snag_term_text_width(piece, bytes);
        if (count > characters - used || w > cells - width ||
            bytes > out->max - out->len) {
            *truncated = true;
            break;
        }
        if (snag_buf_append(out, piece, bytes) < 0)
            goto out;
        used += count;
        width += w;
        i += n;
    }
    rc = 0;
out:
    snag_buf_free(&safe);
    return rc;
}

static int
canonical_prefix(const json_t *value, struct snag_buf *out, size_t bytes,
                 bool *truncated)
{
    snag_buf_init(out, bytes);
    if (snag_json_canonical(value, out) == 0)
        return 0;
    if (errno != EOVERFLOW)
        return -1;
    *truncated = true;
    while (out->len && !snag_utf8_valid(out->data, out->len, false))
        --out->len;
    return 0;
}

static void
block_init(struct snag_render_block *block, enum snag_presentation kind)
{
    memset(block, 0, sizeof(*block));
    snag_buf_init(&block->text, 512u);
    snag_buf_init(&block->context, SNAG_PATH_MAX_BYTES * 4u + 128u);
    snag_buf_init(&block->body, SNAG_MAX_RESPONSE_GRAPH * 4u);
    block->body_kind = kind;
}

void
snag_render_block_free(struct snag_render_block *block)
{
    snag_buf_free(&block->text);
    snag_buf_free(&block->context);
    snag_buf_free(&block->body);
}

static int
summary(struct snag_render_block *block, const struct snag_buf *row,
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
        rc = snag_buf_append(&block->text, "…", 3u);
    block->colored_len = colored_len < block->text.len ? colored_len : block->text.len;
    return rc < 0 ? -1 : snag_buf_putc(&block->text, '\n');
}

int
snag_render_prepare_tool_start(struct snag_render_block *block,
                              const struct snag_response_item *call,
                              const char *workdir, uint32_t default_timeout_ms,
                              unsigned int level, unsigned int columns)
{
    struct snag_buf row, args;
    bool truncated = false;
    size_t limit = snag_presentation_limit(SNAG_PRESENT_ARGUMENTS, level);
    size_t bytes = limit == SIZE_MAX ? SNAG_MAX_TOOL_ARGUMENTS * 6u + 2u :
                   limit ? limit * 4u + 16u : 512u;
    int rc = -1;

    block_init(block, SNAG_PRESENT_ARGUMENTS);
    block->role = SNAG_ROLE_ACTIVITY;
    snag_buf_init(&row, 4096u);
    if (canonical_prefix(call->arguments, &args, bytes, &truncated) < 0)
        goto out;
    bool arguments_truncated = truncated;
    if (snag_buf_printf(&row, "→ %s", call->name) < 0)
        goto out;
    size_t colored_len = row.len;
    if (call->call_id[0] && snag_buf_printf(&row, " [%.8s]", call->call_id) < 0)
        goto out;
    if (snag_buf_append(&row, "  ", 2u) < 0 ||
        preview(&row, (const char *)args.data, args.len, 95u, 95u, true, &truncated) < 0 ||
        (truncated && snag_buf_append(&row, "…", 3u) < 0) ||
        summary(block, &row, columns, colored_len) < 0)
        goto out;
    if (limit) {
        block->truncated = arguments_truncated;
        if (preview(&block->body, (const char *)args.data, args.len,
                      limit, SIZE_MAX, false, &block->truncated) < 0)
            goto out;
    }
    if (snag_presentation_limit(SNAG_PRESENT_CONTEXT, level)) {
        uint64_t timeout = default_timeout_ms;
        const char *explicit_workdir = snag_json_string(call->arguments, "workdir");
        if (explicit_workdir)
            workdir = explicit_workdir;
        (void)snag_json_integer_u64(call->arguments, "timeout_ms", &timeout);
        if (snag_buf_printf(&block->context, "  workdir: %s\n  timeout: ", workdir) < 0 ||
            (timeout ? snag_buf_printf(&block->context, "%llums\n", (unsigned long long)timeout) :
                       snag_buf_append(&block->context, "none\n", 5u)) < 0)
            goto out;
    }
    rc = 0;
out:
    snag_buf_free(&args);
    snag_buf_free(&row);
    if (rc < 0)
        snag_render_block_free(block);
    return rc;
}

int
snag_render_prepare_tool_finish(struct snag_render_block *block, const char *name,
                               const json_t *result, uint32_t max_output_bytes,
                               unsigned int level, unsigned int columns)
{
    struct snag_buf row;
    const char *status = snag_json_string(result, "status");
    const char *output = snag_json_string(result, "model_text");
    const char *handle = snag_json_string(result, "handle");
    json_t *ref = json_object_get(result, "output_ref");
    if (ref)
        handle = snag_json_string(ref, "handle");
    json_t *exit_value = json_object_get(result, "exit_code");
    uint64_t duration;
    size_t limit = snag_presentation_limit(SNAG_PRESENT_OUTPUT, level);
    int rc = -1;

    block_init(block, SNAG_PRESENT_OUTPUT);
    block->role = status && strcmp(status, "succeeded") == 0 ? SNAG_ROLE_SUCCESS :
                  status && strcmp(status, "failed") == 0 ? SNAG_ROLE_ERROR : SNAG_ROLE_WARNING;
    snag_buf_init(&row, 4096u);
    if (snag_buf_printf(&row, "← %s  ", name) < 0)
        goto out;
    if (json_is_integer(exit_value)) {
        block->role = json_integer_value(exit_value) ? SNAG_ROLE_ERROR : SNAG_ROLE_SUCCESS;
        if (snag_buf_printf(&row, "exit %lld", (long long)json_integer_value(exit_value)) < 0)
            goto out;
    } else if (snag_buf_printf(&row, "%s", status ? status : "unknown") < 0) {
        goto out;
    }
    if (snag_json_integer_u64(result, "duration_ms", &duration) == 0 &&
        (duration < 1000u ?
         snag_buf_printf(&row, " · %llums", (unsigned long long)duration) :
         snag_buf_printf(&row, " · %llu.%llus", (unsigned long long)(duration / 1000u),
                        (unsigned long long)(duration % 1000u / 100u))) < 0)
        goto out;
    if (handle && snag_buf_printf(&row, " · %s", handle) < 0)
        goto out;
    if (summary(block, &row, columns, row.len) < 0)
        goto out;
    if (output && limit && !ref) {
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
    snag_buf_free(&row);
    if (rc < 0)
        snag_render_block_free(block);
    return rc;
}
