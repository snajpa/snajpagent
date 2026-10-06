/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_transcript.h"
#include "history_view.h"
#include "irc.h"
#include "render.h"
#include "secret_source.h"
#include "store.h"
#include "vm_public.h"
#include "vm_source.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        offset > INT64_MAX - decoded.len || decoded.len > SNAG_MEMORY_LIMIT / 2u - view->bytes)
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

static size_t
secret_span(const struct output_stream *stream, size_t at, const struct snag_wire_secrets *secrets)
{
    size_t left = stream->raw.len - at;
    const unsigned char *bytes = stream->raw.data + at;
    size_t match = snag_wire_secret_match(bytes, left, secrets);
    if (!secrets) return match;
    for (size_t i = 0u; i < secrets->count; ++i) {
        const char *secret = secrets->values[i];
        if (!secret) continue;
        size_t length = strlen(secret);
        /* A loaded range can end or begin in the middle of a protected value.
         * Mask matching partial boundary text until neighboring bytes arrive. */
        if (length > left && !memcmp(secret, bytes, left)) match = left;
        if (!at && stream->begin) {
            for (size_t suffix = 1u; suffix < length && suffix <= left; ++suffix) {
                if (!memcmp(secret + length - suffix, bytes, suffix) && suffix > match)
                    match = suffix;
            }
        }
    }
    return match;
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
            size_t matched = secret_span(stream, at, view->secrets);
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
    rc = block ? 0 : -1;
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
    if (!strcmp(type, "input_received")) label = "operator";
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
snag_vm_transcript_blocks(const json_t *events, unsigned int verbosity, unsigned int columns,
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
