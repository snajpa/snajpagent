/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_transcript.h"
#include "history_view.h"
#include "irc.h"
#include "presentation.h"
#include "render.h"
#include "secret_source.h"
#include "store.h"
#include "vm_public.h"
#include "vm_source.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct output_fragment {
    uint64_t seq, begin, end;
    uint64_t display_begin, display_end;
    size_t stream, at, length;
    struct snag_buf text;
    json_t *map;
    bool binary;
};

struct output_stream {
    char handle[SNAG_ID_HEX_LEN + 1u];
    unsigned int stream;
    uint64_t begin, end;
    struct snag_buf raw;
};

struct transcript {
    json_t *blocks, *calls;
    struct output_fragment *fragments;
    struct output_stream *streams;
    size_t fragment_count, stream_count, bytes;
    unsigned int level, columns;
    const struct snag_wire_secrets *secrets;
    bool (*cancel)(void *);
    void *cancel_opaque;
};

static bool
canceled(struct transcript *view)
{
    if (!view->cancel || !view->cancel(view->cancel_opaque)) return false;
    errno = ECANCELED;
    return true;
}

static json_t *
append_block(struct transcript *view, uint64_t seq, const char *kind, const char *label,
    const void *text, size_t length, uint64_t begin, uint64_t end)
{
    if (length > SNAG_MEMORY_LIMIT / 2u - view->bytes) {
        errno = EOVERFLOW;
        return NULL;
    }
    char key[96];
    (void)snprintf(key, sizeof(key), "event/%llu/%s", (unsigned long long)seq, kind);
    json_t *body = json_stringn(text ? text : "", length);
    json_t *block = body ? json_pack("{s:s,s:I,s:I,s:s,s:s,s:o,s:I,s:I,s:I}",
        "key", key, "seq", (json_int_t)seq, "last_seq", (json_int_t)seq,
        "kind", kind, "label", label, "text", body,
        "source_begin", (json_int_t)begin, "source_end", (json_int_t)end,
        "order", (json_int_t)json_array_size(view->blocks)) : NULL;
    if (!block || json_array_append_new(view->blocks, block) < 0) return NULL;
    view->bytes += length;
    return block;
}

static int
collect_output(struct transcript *view, const json_t *event)
{
    const json_t *data = json_object_get(event, "data");
    const char *handle = snag_json_string(data, "handle");
    uint64_t stream, offset, seq;
    if (!handle || strlen(handle) != SNAG_ID_HEX_LEN ||
        !snag_hex_is_lower(handle, SNAG_ID_HEX_LEN) ||
        snag_json_integer_u64(data, "stream", &stream) < 0 || stream > 1u ||
        snag_json_integer_u64(data, "offset", &offset) < 0 ||
        snag_json_integer_u64(event, "seq", &seq) < 0) return snag_errno(EINVAL);
    size_t index = 0u;
    while (index < view->stream_count && (strcmp(view->streams[index].handle, handle) ||
        view->streams[index].stream != stream)) ++index;
    if (index == view->stream_count) {
        if (index >= SIZE_MAX / sizeof(*view->streams)) return snag_errno(EOVERFLOW);
        struct output_stream *grown = realloc(view->streams, (index + 1u) * sizeof(*grown));
        if (!grown) return -1;
        view->streams = grown;
        view->streams[index] = (struct output_stream){.stream = (unsigned int)stream,
            .begin = offset, .end = offset, .raw = {.max = SNAG_MEMORY_LIMIT / 2u}};
        memcpy(view->streams[index].handle, handle, SNAG_ID_HEX_LEN + 1u);
        ++view->stream_count;
    }
    struct output_stream *output = &view->streams[index];
    if (offset != output->end) return snag_errno(EINVAL);
    struct snag_buf decoded = {.max = 16384u};
    int rc = -1;
    if (snag_process_output_decode(data, &decoded) < 0 || !decoded.len ||
        offset > (uint64_t)INT64_MAX - decoded.len ||
        decoded.len > SNAG_MEMORY_LIMIT / 2u - view->bytes)
        goto out;
    size_t count = view->fragment_count;
    if (count >= SIZE_MAX / sizeof(*view->fragments)) {
        errno = EOVERFLOW;
        goto out;
    }
    struct output_fragment *grown = realloc(view->fragments, (count + 1u) * sizeof(*grown));
    if (!grown) goto out;
    view->fragments = grown;
    view->fragments[count] = (struct output_fragment){.seq = seq, .begin = offset,
        .end = offset + decoded.len, .stream = index, .at = output->raw.len,
        .length = decoded.len, .text = {.max = 17u * 16384u + 4u}};
    ++view->fragment_count;
    view->fragments[count].map = json_array();
    if (!view->fragments[count].map) goto out;
    if (snag_buf_append(&output->raw, decoded.data, decoded.len) < 0) goto out;
    output->end += decoded.len;
    view->bytes += decoded.len;
    rc = 0;
out:
    snag_secret_clear(decoded.data, decoded.len);
    snag_buf_free(&decoded);
    return rc;
}

static int
decode_output(struct transcript *view)
{
    for (size_t s = 0u; s < view->stream_count; ++s) {
        const struct output_stream *stream = &view->streams[s];
        size_t fragment = 0u;
        for (size_t at = 0u; at < stream->raw.len;) {
            if (canceled(view)) return -1;
            while (fragment < view->fragment_count && (view->fragments[fragment].stream != s ||
                at >= view->fragments[fragment].at + view->fragments[fragment].length)) ++fragment;
            if (fragment == view->fragment_count) return snag_errno(EINVAL);
            struct output_fragment *part = &view->fragments[fragment];
            if (!part->text.len) part->display_begin = stream->begin + at;
            size_t matched = snag_wire_secret_span(stream->raw.data + at,
                stream->raw.len - at, !at && stream->begin, true, view->secrets);
            if (matched) {
                if (snag_vm_source_replace(part->map, part->text.len,
                    stream->begin + at, 17u, matched) < 0) return -1;
                if (snag_buf_append(&part->text, "<redacted:secret>", 17u) < 0) return -1;
                at += matched;
                part->display_end = stream->begin + at;
                continue;
            }
            uint32_t cp;
            size_t bytes = snag_utf8_decode(stream->raw.data + at, stream->raw.len - at, &cp);
            if (!bytes || !cp) {
                if (snag_vm_source_replace(part->map, part->text.len,
                    stream->begin + at, 4u, 1u) < 0) return -1;
                if (snag_buf_printf(&part->text, "\\x%02X", stream->raw.data[at]) < 0) return -1;
                part->binary = true;
                ++at;
            } else {
                if (snag_buf_append(&part->text, stream->raw.data + at, bytes) < 0) return -1;
                at += bytes;
            }
            part->display_end = stream->begin + at;
        }
    }
    return 0;
}

static int
output_blocks(struct transcript *view)
{
    if (view->level < 3u) return 0;
    for (size_t i = 0u; i < view->fragment_count; ++i) {
        const struct output_fragment *part = &view->fragments[i];
        const struct output_stream *stream = &view->streams[part->stream];
        if (!part->text.len) continue;
        char label[96];
        (void)snprintf(label, sizeof(label), "%.8s %s%s", stream->handle,
            stream->stream ? "stderr" : "stdout", part->binary ? " (binary escaped)" : "");
        json_t *block = append_block(view, part->seq, "output", label, part->text.data,
            part->text.len, part->display_begin, part->display_end);
        uint64_t last_seq = part->seq;
        if (part->display_end > part->end) {
            for (size_t j = i + 1u; j < view->fragment_count; ++j) {
                const struct output_fragment *next = &view->fragments[j];
                if (next->stream != part->stream) continue;
                last_seq = next->seq;
                if (next->end >= part->display_end) break;
            }
        }
        if (!block || json_object_set_new(block, "last_seq", json_integer(last_seq)) < 0 ||
            json_object_set(block, "source_map", part->map) < 0 ||
            json_object_set_new(block, "handle", json_string(stream->handle)) < 0 ||
            json_object_set_new(block, "stream", json_integer(stream->stream)) < 0) return -1;
    }
    return 0;
}

static int
public_blocks(struct transcript *view, const json_t *events, char *error, size_t size)
{
    json_t *blocks = snag_vm_public_blocks(events, view->secrets, error, size);
    if (!blocks) return -1;
    int rc = -1;
    for (size_t i = 0u; i < json_array_size(blocks); ++i) {
        json_t *public = json_array_get(blocks, i);
        const char *text = snag_json_string(public, "text");
        const char *kind = snag_json_string(public, "kind");
        const char *phase = snag_json_string(public, "phase");
        const char *state = snag_json_string(public, "state");
        char label[96];
        (void)snprintf(label, sizeof(label), "%s %s%s%s", kind, phase,
            strcmp(state, "complete") ? " / " : "", strcmp(state, "complete") ? state : "");
        uint64_t seq = (uint64_t)json_integer_value(json_object_get(public, "seq"));
        json_t *block = append_block(view, seq, kind, label, text, strlen(text),
            (uint64_t)json_integer_value(json_object_get(public, "source_begin")),
            (uint64_t)json_integer_value(json_object_get(public, "source_end")));
        if (!block) goto out;
        static const char *const keys[] = {
            "key", "last_seq", "response_id", "ordinal", "phase", "state", "source_map"};
        for (size_t k = 0u; k < sizeof(keys) / sizeof(keys[0]); ++k)
            if (json_object_set(block, keys[k], json_object_get(public, keys[k])) < 0) goto out;
    }
    rc = 0;
out:
    json_decref(blocks);
    return rc;
}

static int
call_metadata(struct transcript *view, const json_t *data)
{
    const json_t *items = json_object_get(data, "items");
    for (size_t i = 0u; i < json_array_size(items); ++i) {
        json_t *item = json_array_get(items, i);
        const char *kind = snag_json_string(item, "kind");
        const char *call = snag_json_string(item, "call_id");
        if (kind && !strcmp(kind, "tool_call") && call &&
            json_object_set(view->calls, call, item) < 0) return -1;
    }
    return 0;
}

static int
tool_block(struct transcript *view, uint64_t seq, const char *type, const json_t *data)
{
    if (!view->level) return 0;
    const char *id = snag_json_string(data, "call_id");
    if (!id) return snag_errno(EINVAL);
    const json_t *metadata = json_object_get(data, "call");
    if (!metadata) metadata = json_object_get(view->calls, id);
    const char *name = snag_json_string(metadata, "name");
    const char *workdir = snag_json_string(data, "resolved_workdir");
    bool start = !strcmp(type, "tool_started");
    struct snag_render_block formatted = {0};
    int rc = -1;
    if (name && start) {
        struct snag_response_item call = {.name = (char *)name, .call_id = id,
            .arguments = json_object_get(metadata, "arguments")};
        rc = snag_render_prepare_tool_start(&formatted, &call, workdir ? workdir : "",
            UINT64_MAX, view->level, view->columns);
    } else if (!start) {
        rc = snag_render_prepare_tool_finish(&formatted, name ? name : "tool", id,
            json_object_get(data, "result"), 0u, view->level, view->columns);
    } else {
        snag_buf_init(&formatted.text, 512u);
        rc = snag_buf_printf(&formatted.text, "→ tool %.32s", id);
    }
    if (rc < 0) return -1;
    rc = -1;
    struct snag_buf body = {.max = SNAG_MEMORY_LIMIT / 2u};
    if (snag_buf_append(&body, formatted.context.data, formatted.context.len) < 0 ||
        snag_buf_append(&body, formatted.body.data, formatted.body.len) < 0 ||
        (formatted.truncated && snag_buf_append(&body, "\n…", 4u) < 0) ||
        snag_buf_terminate(&formatted.text) < 0) goto out;
    const char *preview = snag_json_string(data, "output_preview");
    if (view->level == 2u && preview &&
        (snag_buf_append(&body, preview, strlen(preview)) < 0 ||
         (json_is_true(json_object_get(data, "preview_truncated")) &&
          snag_buf_append(&body, "…\n", 4u) < 0))) goto out;
    json_t *block = append_block(view, seq, type, (const char *)formatted.text.data,
        body.data, body.len, 0u, body.len);
    rc = block ? json_object_set_new(block, "role", json_integer(formatted.role)) : -1;
    if (block && !rc) {
        json_t *parts = json_pack("{s:s#,s:s#,s:I,s:i,s:b}",
            "context", formatted.context.data ? (const char *)formatted.context.data : "",
            formatted.context.len,
            "body", formatted.body.data ? (const char *)formatted.body.data : "",
            formatted.body.len,
            "colored", (json_int_t)formatted.colored_len,
            "kind", formatted.body_kind, "truncated", formatted.truncated);
        if (parts && view->level == 2u && preview) {
            bool truncated = json_is_true(json_object_get(data, "preview_truncated"));
            if (json_object_set_new(parts, "preview", json_string(preview)) < 0 ||
                json_object_set_new(parts, "preview_truncated",
                    json_boolean(truncated)) < 0) {
                json_decref(parts);
                parts = NULL;
            }
        }
        rc = parts ? json_object_set_new(block, "tool", parts) : -1;
    }
    if (block && !name) rc = json_object_set_new(block, "needs_call", json_string(id));
out:
    snag_buf_free(&body);
    snag_render_block_free(&formatted);
    return rc;
}

static int
text_block(struct transcript *view, uint64_t seq, const char *kind, const char *label,
    const char *text, const char *source, json_t **out)
{
    json_t *block = append_block(view, seq, kind, label, text, strlen(text), 0u,
        strlen(source ? source : text));
    if (!block) return -1;
    if (source) {
        json_t *map = snag_vm_source_redactions(source, text, view->secrets);
        if (!map || json_object_set_new(block, "source_map", map) < 0) return -1;
    }
    if (out) *out = block;
    return 0;
}

static int
event_block(struct transcript *view, uint64_t seq, const char *type,
    const json_t *data, const json_t *source)
{
    if (!strcmp(type, "response_completed")) return call_metadata(view, data);
    if (!strcmp(type, "tool_started") || !strcmp(type, "tool_finished"))
        return tool_block(view, seq, type, data);
    const char *text = snag_json_string(data, "text");
    const char *field = "text";
    const char *label = NULL;
    if (!strcmp(type, "input_received")) {
        if (snag_irc_prompt(text)) return 0;
        label = "operator";
    }
    else if (!strcmp(type, "steering_added")) label = "operator / steering";
    else if (!strcmp(type, "future_turn_queued")) label = "operator / queued";
    else if (!strcmp(type, "future_turn_edited")) label = "operator / edited queue";
    else if (!strcmp(type, "goal_started")) {
        label = "goal";
        field = "prompt";
        text = snag_json_string(data, "prompt");
    } else if (!strcmp(type, "goal_blocked")) {
        const char *wait_for = snag_json_string(data, "wait_for");
        const char *reason = snag_json_string(data, "reason");
        if (!reason) return snag_errno(EINVAL);
        struct snag_buf heading = {.max = SNAG_MAX_EVENT_LINE};
        int rc = snag_buf_printf(&heading, "goal blocked; waiting for %s",
            wait_for ? wait_for : "unspecified (older goal record)");
        if (!rc) rc = snag_buf_terminate(&heading);
        if (!rc) rc = text_block(view, seq, "goal", (const char *)heading.data,
            reason, snag_json_string(source, "reason"), NULL);
        snag_buf_free(&heading);
        return rc;
    }
    else if (snag_string_in(type, "irc_event irc_event_v2")) {
        const char *endpoint = snag_json_string(data, "endpoint");
        const char *room = snag_json_string(data, "room");
        const char *nick = snag_json_string(data, "nick");
        const char *kind = snag_json_string(data, "kind");
        if (!endpoint || !room || !nick || !kind || !text) return snag_errno(EINVAL);
        const json_t *routing = json_object_get(data, "routing");
        const char *identity = snag_json_string(routing, "identity");
        const char *conversation = snag_json_string(routing, "conversation_kind");
        const char *target = conversation && !strcmp(conversation, "query") ?
            snag_json_string(routing, "peer") : room;
        if (routing && (!identity || !conversation || !target)) return snag_errno(EINVAL);
        /* Redaction can expand labels beyond the native routing-field width.
         * These owned strings are display data, never network addresses. */
        struct snag_buf heading = {.max = SNAG_MAX_EVENT_LINE};
        int rc = snag_buf_printf(&heading, "%s/%s <%s> %s", endpoint,
            target, *nick ? nick : "server", !strcmp(kind, "message") ? "" : kind);
        if (!rc && routing) rc = snag_buf_printf(&heading, " [%s %s]", identity, conversation);
        const char *source_text = snag_json_string(source, "text");
        const char *direction = snag_json_string(routing, "direction");
        if (!rc && direction && !strcmp(direction, "outgoing")) {
            const char *state = snag_json_string(routing, "state");
            const char *send = snag_json_string(routing, "send_id");
            bool revised = json_is_true(json_object_get(routing, "revised"));
            rc = state && send ? snag_buf_printf(&heading, " [send %.8s %s%s%s]", send,
                state, revised ? "; server text" : "",
                json_is_true(json_object_get(routing, "action")) ? "; action" : "") :
                snag_errno(EINVAL);
            if (state && strcmp(state, "pending") && !revised) text = source_text = "";
        }
        if (!rc) rc = snag_buf_terminate(&heading);
        json_t *block = NULL;
        if (!rc) rc = text_block(view, seq, "irc", (const char *)heading.data,
            text, source_text, &block);
        if (rc < 0) block = NULL;
        if (block && (json_object_set(block, "irc_event", (json_t *)source) < 0 ||
            json_object_set(block, "irc_display", (json_t *)data) < 0)) block = NULL;
        if (block && (json_object_set_new(block, "endpoint", json_string(endpoint)) < 0 ||
            json_object_set_new(block, "target", json_string(target)) < 0 ||
            (routing && json_object_set(block, "routing", (json_t *)routing) < 0))) block = NULL;
        snag_buf_free(&heading);
        return block ? 0 : -1;
    } else if (!strcmp(type, "response_failed") || !strcmp(type, "turn_failed")) {
        label = type;
        field = "message";
        text = snag_json_string(data, "message");
    } else if (!strcmp(type, "response_interrupted") || !strcmp(type, "turn_interrupted")) {
        label = "interrupted";
        field = "reason";
        text = snag_json_string(data, "reason");
    } else if (!strcmp(type, "compaction_completed") || !strcmp(type, "irc_compaction_completed")) {
        label = "context compacted";
        field = NULL;
        text = "Earlier transcript remains in retained history.";
    } else if (!strcmp(type, "session_created")) {
        label = "session";
        field = "cwd";
        text = snag_json_string(data, "cwd");
    }
    if (!label) return 0;
    if (!text) text = "";
    return text_block(view, seq, type, label, text,
        field ? snag_json_string(source, field) : NULL, NULL);
}

struct formatted_text {
    struct snag_buf text;
    json_t *styles, *origins;
};

static int
formatted_span(void *opaque, const char *text, size_t length, unsigned int style,
    uint64_t source, size_t source_length)
{
    struct formatted_text *out = opaque;
    if (!length) return 0;
    size_t begin = out->text.len;
    if (snag_buf_append(&out->text, text, length) < 0) return -1;
    if (source != UINT64_MAX) {
        size_t count = json_array_size(out->origins);
        json_t *last = count ? json_array_get(out->origins, count - 1u) : NULL;
        uint64_t previous = last ? (uint64_t)json_integer_value(json_array_get(last, 2u)) : 0u;
        size_t bytes = last ? (size_t)json_integer_value(json_array_get(last, 3u)) : 0u;
        size_t end = last ? (size_t)json_integer_value(json_array_get(last, 1u)) : 0u;
        size_t start = last ? (size_t)json_integer_value(json_array_get(last, 0u)) : 0u;
        if (last && end == begin && previous + bytes == source &&
            end - start == bytes && length == source_length) {
            if (json_array_set_new(last, 1u, json_integer(out->text.len)) < 0 ||
                json_array_set_new(last, 3u, json_integer(bytes + length)) < 0) return -1;
        } else if (json_array_append_new(out->origins, json_pack("[I,I,I,I]",
            (json_int_t)begin, (json_int_t)out->text.len, (json_int_t)source,
            (json_int_t)source_length)) < 0) return -1;
    }
    size_t count = json_array_size(out->styles);
    json_t *last = count ? json_array_get(out->styles, count - 1u) : NULL;
    if (last && (unsigned int)json_integer_value(json_array_get(last, 2u)) == style)
        return json_array_set_new(last, 1u, json_integer(out->text.len));
    return json_array_append_new(out->styles,
        json_pack("[I,I,i]", (json_int_t)begin, (json_int_t)out->text.len, style));
}

static int
format_block(struct snag_render *render, json_t *block)
{
    const char *kind = snag_json_string(block, "kind");
    const char *text = snag_json_string(block, "text");
    const char *label = snag_json_string(block, "label");
    render->sink.source = (struct snag_render_origin){(const unsigned char *)text,
        strlen(text), 0u, NULL};
    if (!strcmp(kind, "assistant") || !strcmp(kind, "refusal")) {
        if (snag_render_public_begin(render, STDOUT_FILENO, NULL) < 0 ||
            snag_render_public(render, text, strlen(text), NULL) < 0) return -1;
        const char *state = snag_json_string(block, "state");
        return state && strcmp(state, "complete") ? snag_render_public_abort(render) :
            snag_render_public_end(render);
    }
    if (!strcmp(kind, "reasoning") || !strcmp(kind, "session_created")) return 0;
    if (!strcmp(kind, "input_received")) return snag_render_input_submitted(render, "› ", text);
    if (snag_string_in(kind, "steering_added future_turn_queued future_turn_edited"))
        return snag_render_input_submitted(render, "» ", text);
    if (!strcmp(kind, "tool_started") || !strcmp(kind, "tool_finished")) {
        const json_t *parts = json_object_get(block, "tool");
        const char *context = snag_json_string(parts, "context");
        const char *body = snag_json_string(parts, "body");
        struct snag_render_block tool = {
            .text = {.data = (unsigned char *)label, .len = strlen(label)},
            .context = {.data = (unsigned char *)context, .len = context ? strlen(context) : 0u},
            .body = {.data = (unsigned char *)body, .len = body ? strlen(body) : 0u},
            .colored_len = (size_t)json_integer_value(json_object_get(parts, "colored")),
            .role = (enum snag_render_role)json_integer_value(json_object_get(block, "role")),
            .body_kind = (enum snag_presentation)json_integer_value(json_object_get(parts, "kind")),
            .truncated = json_is_true(json_object_get(parts, "truncated"))};
        render->origin = (struct snag_render_origin){(const unsigned char *)context,
            tool.context.len, 0u, NULL};
        render->sink.source = (struct snag_render_origin){(const unsigned char *)body,
            tool.body.len, tool.context.len, NULL};
        int rc = snag_render_tool_block(render, &tool);
        render->origin = (struct snag_render_origin){0};
        const char *preview = snag_json_string(parts, "preview");
        if (!rc && preview) {
            struct snag_render_block output = {.body_kind = SNAG_PRESENT_OUTPUT,
                .body = {.data = (unsigned char *)preview, .len = strlen(preview)},
                .truncated = json_is_true(json_object_get(parts, "preview_truncated"))};
            render->sink.source = (struct snag_render_origin){(const unsigned char *)preview,
                strlen(preview), tool.context.len + tool.body.len, NULL};
            rc = snag_render_tool_block(render, &output);
        }
        return rc;
    }
    if (!strcmp(kind, "irc")) {
        const json_t *display = json_object_get(block, "irc_display");
        const char *body = snag_json_string(display, "text");
        render->sink.source = (struct snag_render_origin){(const unsigned char *)body,
            strlen(body), 0u, NULL};
        return snag_render_irc_snapshot(render, json_object_get(block, "irc_event"), display);
    }
    if (snag_string_in(kind, "turn_failed response_failed"))
        return snag_render_error_ctx(render, text);
    if (!strcmp(kind, "event")) return snag_render_event(render,
        (uint64_t)json_integer_value(json_object_get(block, "seq")), label);
    if (!strcmp(kind, "output"))
        return formatted_span(render->sink.opaque, text, strlen(text), 0u, 0u, strlen(text));
    if (snag_string_in(kind, "response_interrupted turn_interrupted"))
        return snag_render_warning_ctx(render, "turn interrupted");
    if (!strcmp(kind, "goal") || !strcmp(kind, "goal_started")) {
        struct snag_buf message = {.max = SNAG_MAX_EVENT_LINE};
        int rc = snag_buf_printf(&message, "%s: %s", label, text);
        if (!rc) rc = snag_buf_terminate(&message);
        if (!rc) rc = snag_render_warning_ctx(render, (const char *)message.data);
        snag_buf_free(&message);
        return rc;
    }
    if (snag_string_in(kind, "compaction_completed irc_compaction_completed"))
        return snag_render_warning_ctx(render, label);
    return 0;
}

static int
format_checkpoint(void *opaque)
{
    return canceled(opaque) ? -1 : 0;
}

static int
format_blocks(struct transcript *view, json_t *blocks, const json_t *route,
    bool plain, bool no_color, bool logical)
{
    struct formatted_text out = {.text = {.max = SNAG_MEMORY_LIMIT / 2u}};
    struct snag_term term = {.columns = view->columns};
    struct snag_render render;
    snag_render_init(&render, view->level);
    render.term = &term;
    render.checkpoint = format_checkpoint;
    render.checkpoint_opaque = view;
    render.stdout_terminal = render.stderr_terminal = true;
    render.color_stdout = render.color_stderr = !no_color;
    render.markdown = !plain;
    if (route) {
        render.view = SNAG_RENDER_CHAT;
        const char *room = snag_json_string(route, "room");
        const char *conversation = snag_json_string(route, "conversation");
        const char *endpoint = snag_json_string(route, "endpoint");
        (void)snag_strcpy(render.chat_room, sizeof(render.chat_room), room ? room : "");
        (void)snag_strcpy(render.chat_endpoint, sizeof(render.chat_endpoint),
            endpoint ? endpoint : "");
        (void)snag_strcpy(render.chat_conversation, sizeof(render.chat_conversation),
            room ? "" : conversation ? conversation : snag_json_string(route, "connection"));
    }
    render.sink = (struct snag_render_sink){.text = formatted_span, .opaque = &out,
        .columns = view->columns, .logical = logical};
    int rc = -1;
    for (size_t i = 0u; i < json_array_size(blocks); ++i) {
        json_t *block = json_array_get(blocks, i);
        snag_buf_reset(&out.text);
        out.styles = json_array();
        out.origins = json_array();
        if (!out.styles || !out.origins || format_block(&render, block) < 0) goto done;
        if (json_object_set_new(block, "display", json_stringn(
                out.text.data ? (const char *)out.text.data : "", out.text.len)) < 0 ||
            json_object_set(block, "styles", out.styles) < 0 ||
            json_object_set_new(block, "prompt_gap",
                json_integer(snag_term_prompt_separation(&term))) < 0 ||
            json_object_set(block, "format_map", out.origins) < 0) goto done;
        json_decref(out.styles);
        out.styles = NULL;
        json_decref(out.origins);
        out.origins = NULL;
        (void)json_object_del(block, "tool");
        (void)json_object_del(block, "irc_event");
        (void)json_object_del(block, "irc_display");
    }
    rc = 0;
done:
    snag_render_free(&render);
    snag_buf_free(&out.text);
    json_decref(out.styles);
    json_decref(out.origins);
    return rc;
}

static int
compare_blocks(const void *a, const void *b)
{
    const json_t *left = *(const json_t *const *)a, *right = *(const json_t *const *)b;
    json_int_t ls = json_integer_value(json_object_get(left, "seq"));
    json_int_t rs = json_integer_value(json_object_get(right, "seq"));
    if (ls != rs) return ls < rs ? -1 : 1;
    json_int_t lo = json_integer_value(json_object_get(left, "order"));
    json_int_t ro = json_integer_value(json_object_get(right, "order"));
    return lo < ro ? -1 : lo > ro;
}

static json_t *
sort_blocks(struct transcript *view)
{
    size_t count = json_array_size(view->blocks);
    json_t **rows = count ? malloc(count * sizeof(*rows)) : NULL;
    if (count && !rows) return NULL;
    for (size_t i = 0u; i < count; ++i) rows[i] = json_array_get(view->blocks, i);
    if (count > 1u) qsort(rows, count, sizeof(*rows), compare_blocks);
    json_t *result = json_array();
    for (size_t i = 0u; result && i < count; ++i) {
        (void)json_object_del(rows[i], "order");
        if (json_array_append(result, rows[i]) < 0) {
            json_decref(result);
            result = NULL;
        }
    }
    free(rows);
    return result;
}

json_t *
snag_vm_transcript_blocks(const json_t *events, const json_t *route,
    unsigned int verbosity, unsigned int columns,
    bool plain, bool no_color, bool logical,
    const struct snag_wire_secrets *secrets, bool (*cancel)(void *), void *opaque,
    char *error, size_t size)
{
    struct transcript view = {.blocks = json_array(), .calls = json_object(), .level = verbosity,
        .columns = columns, .secrets = secrets, .cancel = cancel, .cancel_opaque = opaque};
    json_t *result = NULL;
    if (!view.blocks || !view.calls || !json_is_array(events) || verbosity > SNAG_VERBOSITY_MAX ||
        (secrets && secrets->count && !secrets->values)) goto out;
    uint64_t previous = 0u;
    for (size_t i = 0u; i < json_array_size(events); ++i) {
        const json_t *event = json_array_get(events, i);
        const char *type = snag_json_string(event, "type");
        uint64_t seq;
        if (canceled(&view) || !type || snag_json_integer_u64(event, "seq", &seq) < 0 ||
            seq <= previous) goto out;
        previous = seq;
        if (!strcmp(type, "process_output") && collect_output(&view, event) < 0) goto out;
    }
    if (decode_output(&view) < 0) goto out;
    /* Decoded source is separate from the bounded display-block budget. */
    view.bytes = 0u;
    if (public_blocks(&view, events, error, size) < 0 || output_blocks(&view) < 0) goto out;
    for (size_t i = 0u; i < json_array_size(events); ++i) {
        const json_t *event = json_array_get(events, i);
        const char *type = snag_json_string(event, "type");
        uint64_t seq = (uint64_t)json_integer_value(json_object_get(event, "seq"));
        if (canceled(&view)) goto out;
        if (!strcmp(type, "process_output")) continue;
        json_t *source = json_copy(json_object_get(event, "data"));
        if (!source) goto out;
        static const char *const references[] = {"call", "output_preview", "preview_truncated"};
        bool copied = true;
        for (size_t k = 0u; k < sizeof(references) / sizeof(references[0]); ++k) {
            json_t *value = json_object_get(event, references[k]);
            if (value && json_object_set(source, references[k], value) < 0) copied = false;
        }
        if (!copied) { json_decref(source); goto out; }
        char *filtered = snag_history_event_data(seq, type, source, secrets, error, size);
        json_decref(source);
        json_t *data = filtered ? json_loads(filtered, JSON_REJECT_DUPLICATES, NULL) : NULL;
        free(filtered);
        if (!data) goto out;
        int rc = event_block(&view, seq, type, data, json_object_get(event, "data"));
        if (!rc && verbosity >= 4u)
            rc = append_block(&view, seq, "event", type, NULL, 0u, 0u, 0u) ? 0 : -1;
        json_decref(data);
        if (rc < 0) goto out;
    }
    result = sort_blocks(&view);
    if (result && format_blocks(&view, result, route, plain, no_color, logical) < 0) {
        json_decref(result);
        result = NULL;
    }
out:
    if (!result) (void)snag_fail(error, size, errno ? errno : EINVAL,
        "cannot project transcript source range");
    for (size_t i = 0u; i < view.stream_count; ++i) {
        snag_secret_clear(view.streams[i].raw.data, view.streams[i].raw.len);
        snag_buf_free(&view.streams[i].raw);
    }
    for (size_t i = 0u; i < view.fragment_count; ++i) {
        snag_buf_free(&view.fragments[i].text);
        json_decref(view.fragments[i].map);
    }
    free(view.streams);
    free(view.fragments);
    json_decref(view.blocks);
    json_decref(view.calls);
    return result;
}

static json_t *
filter_process(const json_t *value, const struct snag_wire_secrets *secrets)
{
    const json_t *data = json_object_get(value, "data");
    struct snag_buf source = {.max = 16384u};
    struct snag_buf text = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf encoded = {.max = SNAG_MAX_EVENT_LINE};
    json_t *copy = NULL;
    uint64_t offset;
    if (snag_process_output_decode(data, &source) < 0 ||
        snag_json_integer_u64(data, "offset", &offset) < 0) goto out;
    for (size_t at = 0u; at < source.len;) {
        size_t matched = snag_wire_secret_span(source.data + at, source.len - at,
            !at && offset, true, secrets);
        if (matched) {
            if (snag_buf_append(&text, "<redacted:secret>", 17u) < 0) goto out;
            at += matched;
        } else if (snag_buf_putc(&text, source.data[at++]) < 0) goto out;
    }
    bool binary = !strcmp(snag_json_string(data, "encoding"), "base64");
    if (binary && snag_base64_append(&encoded, text.data, text.len) < 0) goto out;
    const struct snag_buf *body = binary ? &encoded : &text;
    json_t *payload = json_copy((json_t *)data);
    if (!payload) goto out;
    if (json_object_set_new(payload, "data", json_stringn(
        body->data ? (const char *)body->data : "", body->len)) == 0) {
        copy = json_copy((json_t *)value);
        if (copy && json_object_set(copy, "data", payload) < 0) {
            json_decref(copy);
            copy = NULL;
        }
    }
    json_decref(payload);
out:
    snag_secret_clear(source.data, source.len);
    snag_buf_free(&source);
    snag_buf_free(&text);
    snag_buf_free(&encoded);
    return copy;
}

static json_t *
filter_output(void *opaque, const json_t *value)
{
    const struct snag_wire_secrets *secrets = opaque;
    const char *type = snag_json_string(value, "type");
    json_t *process = type && !strcmp(type, "process_output") ?
        filter_process(value, secrets) : json_incref((json_t *)value);
    if (!process) return NULL;
    char *encoded = json_dumps(process, JSON_COMPACT | JSON_ENCODE_ANY);
    json_decref(process);
    if (!encoded) return NULL;
    struct snag_buf filtered = {.max = SNAG_MEMORY_LIMIT / 2u};
    char error[128];
    int rc = snag_wire_json_redact_bounded((const unsigned char *)encoded, strlen(encoded),
        SNAG_MEMORY_LIMIT / 2u, secrets, &filtered, error, sizeof(error));
    snag_secret_bytes_free(encoded);
    json_t *result = rc ? NULL : json_loadb((const char *)filtered.data,
        filtered.len, JSON_DECODE_ANY, NULL);
    snag_buf_free(&filtered);
    return result;
}

/* Match across fragment boundaries before Markdown can remove secret bytes.
 * Retain the original fragment order, including notices between fragments. */
static int
filter_public(json_t *records, size_t start, size_t end,
    const struct snag_wire_secrets *secrets, bool partial_end)
{
    struct snag_buf source = {.max = SNAG_MAX_PUBLIC_ITEM};
    struct snag_buf decoded = {.max = SNAG_MAX_PUBLIC_ITEM};
    int rc = -1;
    for (size_t i = start; i < end; ++i) {
        const json_t *data = json_object_get(json_array_get(records, i), "data");
        if (strcmp(snag_json_string(data, "op"), "public")) continue;
        const char *text = snag_json_string(data, "text");
        if (!text || snag_base64_decode(&source, text) < 0) goto out;
    }
    size_t offset = 0u, skip = 0u;
    for (size_t i = start; i < end; ++i) {
        json_t *data = json_object_get(json_array_get(records, i), "data");
        if (strcmp(snag_json_string(data, "op"), "public")) continue;
        snag_buf_reset(&decoded);
        if (snag_base64_decode(&decoded, snag_json_string(data, "text")) < 0) goto out;
        size_t limit = offset + decoded.len;
        struct snag_buf text = {.max = SNAG_MAX_PUBLIC_ITEM};
        while (offset < limit) {
            if (skip) { --skip; ++offset; continue; }
            size_t matched = snag_wire_secret_span(source.data + offset,
                source.len - offset, false, partial_end, secrets);
            if (matched) {
                if (snag_buf_append(&text, "<redacted:secret>", 17u) < 0) break;
                skip = matched;
            } else {
                if (snag_buf_putc(&text, source.data[offset]) < 0) break;
                ++offset;
            }
        }
        struct snag_buf encoded = {.max = SNAG_MAX_EVENT_LINE};
        rc = offset == limit ? snag_base64_append(&encoded, text.data, text.len) : -1;
        if (!rc) rc = json_object_set_new(data, "text", json_stringn(
            encoded.data ? (const char *)encoded.data : "", encoded.len));
        snag_buf_free(&text);
        snag_buf_free(&encoded);
        if (rc < 0) goto out;
    }
    rc = 0;
out:
    snag_secret_clear(source.data, source.len);
    snag_secret_clear(decoded.data, decoded.len);
    snag_buf_free(&source);
    snag_buf_free(&decoded);
    return rc;
}

static json_t *
filter_operation(const json_t *record, const struct snag_wire_secrets *secrets)
{
    const char *op = snag_json_string(record, "op");
    if (!op) { errno = EINVAL; return NULL; }
    /* Public bytes have already been matched across their complete stream. */
    if (!strcmp(op, "public") || !strcmp(op, "durable")) return json_incref((json_t *)record);
    bool bytes = snag_string_in(op, "protocol transport resume");
    if (!bytes) return filter_output((void *)secrets, record);
    json_t *copy = json_copy((json_t *)record), *filtered = NULL;
    struct snag_buf text = {.max = SNAG_MAX_EVENT_LINE};
    const char *encoded = snag_json_string(record, "text");
    if (!copy || !encoded || snag_base64_decode(&text, encoded) < 0 ||
        json_object_set_new(copy, "text", json_stringn((const char *)text.data, text.len)) < 0)
        goto out;
    filtered = filter_output((void *)secrets, copy);
    snag_buf_reset(&text);
    const json_t *value = json_object_get(filtered, "text");
    if (filtered && (snag_base64_append(&text, (const unsigned char *)json_string_value(value),
        json_string_length(value)) < 0 || json_object_set_new(filtered, "text",
        json_stringn((const char *)text.data, text.len)) < 0)) {
        json_decref(filtered);
        filtered = NULL;
    }
out:
    snag_secret_clear(text.data, text.len);
    snag_buf_free(&text);
    json_decref(copy);
    return filtered;
}

static int
output_block(json_t *blocks, struct formatted_text *out, uint64_t seq, uint64_t last,
    unsigned int gap)
{
    if (!out->text.len) return 0;
    char key[80];
    (void)snprintf(key, sizeof(key), "presentation:%llu", (unsigned long long)seq);
    json_t *text = json_stringn((const char *)out->text.data, out->text.len);
    json_t *block = text ? json_pack("{s:s,s:I,s:I,s:s,s:s,s:O,s:o,s:O}",
        "key", key, "seq", (json_int_t)seq, "last_seq", (json_int_t)last,
        "kind", "presentation", "label", "", "text", text, "display", text,
        "styles", out->styles) : NULL;
    if (!block || json_array_append_new(blocks, block) < 0) return -1;
    if (json_object_set_new(block, "prompt_gap", json_integer(gap)) < 0) return -1;
    if (json_array_size(out->origins) && json_object_set(block, "format_map", out->origins) < 0)
        return -1;
    uint64_t end = snag_vm_source_position(block, out->text.len, true);
    if (end > INT64_MAX ||
        json_object_set_new(block, "source_begin", json_integer(0)) < 0 ||
        json_object_set_new(block, "source_end", json_integer((json_int_t)end)) < 0)
        return -1;
    snag_buf_reset(&out->text);
    json_decref(out->styles);
    json_decref(out->origins);
    out->styles = json_array();
    out->origins = json_array();
    if (!out->styles || !out->origins) return -1;
    return 0;
}

static bool
presentation_selected(const json_t *route, const json_t *data)
{
    if (!strcmp(snag_json_string(data, "op"), "irc")) {
        const json_t *event = json_object_get(data, "data");
        return snag_presentation_event_selected(route,
            json_object_get(event, "routing") ? "irc_event_v2" : "irc_event", event);
    }
    const json_t *target = json_object_get(data, "route");
    if (!route || !target) return !route && !target;
    static const char *const fields[] = {"connection", "conversation", "identity"};
    for (size_t i = 0u; i < sizeof(fields) / sizeof(*fields); ++i)
        if (!json_equal(json_object_get(route, fields[i]), json_object_get(target, fields[i])))
            return false;
    return true;
}

json_t *
snag_vm_presentation_blocks(const json_t *records, const json_t *route, uint64_t origin,
    int journal_fd, int legacy_fd, unsigned int verbosity, unsigned int columns, bool plain,
    bool no_color, bool logical, const struct snag_wire_secrets *secrets, bool (*cancel)(void *),
    void *opaque, char *error, size_t size)
{
    struct transcript view = {.cancel = cancel, .cancel_opaque = opaque};
    struct formatted_text out = {.text = {.max = SNAG_MEMORY_LIMIT / 2u},
        .styles = json_array(), .origins = json_array()};
    struct snag_term term = {.columns = columns};
    struct snag_render render;
    snag_render_init(&render, verbosity);
    render.term = &term;
    render.stdout_terminal = render.stderr_terminal = true;
    render.color_stdout = render.color_stderr = !no_color;
    render.markdown = !plain;
    render.checkpoint = format_checkpoint;
    render.checkpoint_opaque = &view;
    render.filter_event = filter_output;
    render.filter_opaque = (void *)secrets;
    render.sink = (struct snag_render_sink){.text = formatted_span, .opaque = &out,
        .columns = columns, .logical = logical};
    json_t *input = json_array(), *blocks = json_array();
    bool open = false;
    int active_fd = -1;
    uint64_t first = 0u, last = 0u;
    if (!input || !blocks || !out.styles || !out.origins) goto failed;
    /* A chat page must not copy or redact unrelated streamed model output. */
    for (size_t i = 0u; i < json_array_size(records); ++i) {
        if (canceled(&view)) goto failed;
        const json_t *row = json_array_get(records, i);
        const json_t *data = json_object_get(row, "data");
        if (!snag_json_string(data, "op")) { errno = EINVAL; goto failed; }
        if (presentation_selected(route, data) &&
            json_array_append_new(input, json_deep_copy(row)) < 0) {
            goto failed;
        }
    }
    size_t count = json_array_size(input), start = 0u;
    for (size_t i = 0u; i <= count; ++i) {
        const char *op = snag_json_string(json_object_get(json_array_get(input, i), "data"), "op");
        if (i == count || snag_string_in(op, "begin end abort")) {
            if (filter_public(input, start, i, secrets, i == count || strcmp(op, "end")) < 0)
                goto failed;
            start = i + 1u;
        }
    }
    for (size_t i = 0u; i < count; ++i) {
        if (canceled(&view)) goto failed;
        const json_t *row = json_array_get(input, i);
        uint64_t sequence;
        if (snag_json_integer_u64(row, "seq", &sequence) < 0 || sequence < 2u ||
            sequence - 2u > INT64_MAX - origin) goto failed;
        uint64_t seq = origin + sequence - 2u;
        json_t *data = filter_operation(json_object_get(row, "data"), secrets);
        if (!data) goto failed;
        const char *op = snag_json_string(data, "op");
        if (!strcmp(op, "irc")) {
            const json_t *event = json_object_get(data, "data");
            const json_t *routing = json_object_get(event, "routing");
            render.view = SNAG_RENDER_CHAT;
            const char *room = snag_json_string(event, "room");
            const char *endpoint = snag_json_string(event, "endpoint");
            const char *kind = snag_json_string(routing, "conversation_kind");
            const char *conversation = !kind || !strcmp(kind, "channel") ? "" :
                snag_json_string(routing, !strcmp(kind, "query") ?
                    "conversation_id" : "connection_id");
            (void)snag_strcpy(render.chat_room, sizeof(render.chat_room), room ? room : "");
            (void)snag_strcpy(render.chat_endpoint, sizeof(render.chat_endpoint),
                endpoint ? endpoint : "");
            (void)snag_strcpy(render.chat_conversation, sizeof(render.chat_conversation),
                conversation ? conversation : "");
        }
        if (!open) {
            if (output_block(blocks, &out, first, last, snag_term_prompt_separation(&term)) < 0) {
                json_decref(data);
                goto failed;
            }
            first = seq;
            render.sink.source.byte = 0u;
        }
        last = seq;
        if (!strcmp(op, "begin")) open = true;
        int fd = journal_fd;
        if (!strcmp(op, "durable")) {
            const json_t *source = json_array_get(json_object_get(data, "data"), 0u);
            if (!json_integer_value(json_object_get(source, "sequence"))) fd = legacy_fd;
            if (fd < 0) { json_decref(data); errno = ESTALE; goto failed; }
            /* Each reference names its backend. Queued tool records only use
             * one backend within a response; an import occurs between owners. */
            if (render.history_fd >= 0 && active_fd != fd) {
                (void)close(render.history_fd);
                render.history_fd = -1;
            }
        }
        active_fd = fd;
        bool closed = snag_string_in(op, "end abort");
        int rc = snag_presentation_replay(&render, data, fd);
        if (!rc) rc = snag_render_flush_pending(&render, SIZE_MAX);
        json_decref(data);
        if (rc < 0) goto failed;
        if (closed) open = false;
    }
    if (output_block(blocks, &out, first, last, snag_term_prompt_separation(&term)) < 0)
        goto failed;
    goto done;
failed:
    if (!errno) errno = EINVAL;
    (void)snag_errorf(error, size, "cannot render retained session output: %s", strerror(errno));
    json_decref(blocks);
    blocks = NULL;
done:
    snag_render_free(&render);
    snag_buf_free(&out.text);
    json_decref(out.styles);
    json_decref(out.origins);
    json_decref(input);
    return blocks;
}
