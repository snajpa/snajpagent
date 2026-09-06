/* SPDX-License-Identifier: GPL-2.0-only */
#include "responses.h"
#include "config.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
stream_fail(struct snag_responses_stream *stream, int code, const char *fmt, ...)
{
    va_list ap;

    stream->failed = true;
    va_start(ap, fmt);
    (void)vsnprintf(stream->error, sizeof(stream->error), fmt, ap);
    va_end(ap);
    errno = code;
    return -1;
}

static int
failure_limit(const json_t *object, const char *key, uint64_t *value)
{
    json_t *entry;
    json_int_t integer;

    if (!object || !(entry = json_object_get(object, key)) ||
        json_is_null(entry))
        return 0;
    if (!json_is_integer(entry) || (integer = json_integer_value(entry)) <= 0 ||
        (uint64_t)integer > SNAG_CONFIG_TOKEN_LIMIT_MAX ||
        (*value && *value != (uint64_t)integer))
        return -1;
    *value = (uint64_t)integer;
    return 0;
}

bool
snag_provider_failure_is_capacity(const struct snag_provider_failure *failure)
{
    return failure && strcmp(failure->code,
                             "context_length_exceeded") == 0;
}

int
snag_provider_failure_from_json(const json_t *root,
                               struct snag_provider_failure *failure)
{
    static const char *const limit_keys[] = {
        "context_limit", "context_length", "max_context_length",
        "max_context_tokens"
    };
    static const char *const requested_keys[] = {
        "input_tokens", "requested_tokens", "requested_input_tokens"
    };
    json_t *object;
    json_t *response;
    const char *code;
    const char *message;

    if (!failure)
        return -1;
    memset(failure, 0, sizeof(*failure));
    if (!json_is_object(root))
        return 0;
    object = json_object_get(root, "error");
    response = json_object_get(root, "response");
    if (json_is_object(response)) {
        json_t *nested = json_object_get(response, "error");
        if (json_is_object(nested))
            object = nested;
        else if (!json_is_object(object)) {
            nested = json_object_get(response, "incomplete_details");
            if (json_is_object(nested))
                object = nested;
        }
    }
    if (!json_is_object(object) &&
        (json_object_get(root, "code") || json_object_get(root, "reason")))
        object = (json_t *)root;
    if (!json_is_object(object))
        return object && !json_is_null(object) ? -1 : 0;
    {
        const char *keys[] = {"code", "reason", "type"};
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            json_t *value = json_object_get(object, keys[i]);
            if (value && !json_is_null(value) &&
                (!json_is_string(value) || json_string_length(value) >= 64u ||
                 !snag_utf8_valid((const unsigned char *)json_string_value(value),
                                 json_string_length(value), true)))
                return -1;
        }
    }
    code = snag_json_string(object, "code");
    if (!code)
        code = snag_json_string(object, "reason");
    message = snag_json_string(object, "message");
    if (code)
        memcpy(failure->code, code, strlen(code) + 1u);
    /* A top-level SSE type is the event name, not the error category. */
    if (object != root && snag_json_string(object, "type"))
        snprintf(failure->type, sizeof(failure->type), "%s",
                 snag_json_string(object, "type"));
    if (message && strlen(message) < sizeof(failure->message) &&
        snag_utf8_valid((const unsigned char *)message, strlen(message), true))
        memcpy(failure->message, message, strlen(message) + 1u);
    for (size_t i = 0; i < sizeof(limit_keys) / sizeof(limit_keys[0]); ++i)
        if (failure_limit(object, limit_keys[i],
                          &failure->context_limit_tokens) < 0)
            return -1;
    for (size_t i = 0; i < sizeof(requested_keys) / sizeof(requested_keys[0]); ++i)
        if (failure_limit(object, requested_keys[i],
                          &failure->requested_input_tokens) < 0)
            return -1;
    return 0;
}

static bool
text_equal(const struct snag_buf *buf, const char *text, size_t len)
{
    return buf->len == len && (!len || memcmp(buf->data, text, len) == 0);
}

static int
copy_once(struct snag_responses_stream *stream, char **target,
          const char *value, size_t max, const char *label)
{
    char *copy;
    size_t len;

    if (!value || !(len = strlen(value)) || len > max ||
        !snag_utf8_valid((const unsigned char *)value, len, true))
        return stream_fail(stream, EPROTO, "invalid %s", label);
    if (*target) {
        if (strcmp(*target, value) != 0)
            return stream_fail(stream, EPROTO, "conflicting %s", label);
        return 0;
    }
    copy = snag_strdup_checked(value, max);
    if (!copy)
        return stream_fail(stream, errno ? errno : ENOMEM,
                           "cannot retain %s", label);
    *target = copy;
    return 0;
}

static int
json_index(struct snag_responses_stream *stream, const json_t *object,
           const char *key, size_t limit, size_t *out)
{
    json_t *value;
    json_int_t integer;

    if (!out)
        return stream_fail(stream, EINVAL, "missing index destination");
    *out = 0u;
    value = json_object_get(object, key);
    if (!json_is_integer(value) || (integer = json_integer_value(value)) < 0 ||
        (uint64_t)integer >= (uint64_t)limit)
        return stream_fail(stream, EPROTO, "invalid %s", key);
    *out = (size_t)integer;
    return 0;
}

static int
account_bytes(struct snag_responses_stream *stream, size_t extra)
{
    if (extra > SNAG_MAX_RESPONSE_GRAPH - stream->aggregate_bytes)
        return stream_fail(stream, EOVERFLOW,
                           "response observations exceed 8 MiB");
    stream->aggregate_bytes += extra;
    return 0;
}

static void
wire_part_free(struct snag_wire_part *part)
{
    snag_buf_free(&part->text);
    memset(part, 0, sizeof(*part));
}

static void
wire_item_free(struct snag_wire_item *item)
{
    free(item->id);
    free(item->phase);
    free(item->name);
    free(item->call_id);
    snag_buf_free(&item->arguments);
    for (size_t i = 0; i < item->part_count; ++i)
        wire_part_free(&item->parts[i]);
    free(item->parts);
    memset(item, 0, sizeof(*item));
}

void
snag_responses_stream_init(struct snag_responses_stream *stream,
                          snag_responses_emit_fn emit, void *opaque)
{
    memset(stream, 0, sizeof(*stream));
    stream->emit = emit;
    stream->opaque = opaque;
}

void
snag_responses_stream_free(struct snag_responses_stream *stream)
{
    for (size_t i = 0; i < stream->item_count; ++i)
        wire_item_free(&stream->items[i]);
    free(stream->response_id);
    memset(stream, 0, sizeof(*stream));
}

const char *
snag_responses_stream_error(const struct snag_responses_stream *stream)
{
    return stream->error[0] ? stream->error : "Responses stream failed";
}

static struct snag_wire_item *
new_item(struct snag_responses_stream *stream, size_t output_index,
         enum snag_wire_item_kind kind, const char *id)
{
    struct snag_wire_item *item;

    if (output_index != stream->item_count ||
        output_index >= SNAG_MAX_RESPONSE_ITEMS) {
        (void)stream_fail(stream, EPROTO,
                          "response output indexes are not contiguous");
        return NULL;
    }
    item = &stream->items[stream->item_count];
    memset(item, 0, sizeof(*item));
    snag_buf_init(&item->arguments, SNAG_MAX_TOOL_ARGUMENTS);
    item->kind = kind;
    item->present = true;
    if (id && copy_once(stream, &item->id, id, SNAG_MAX_PROVIDER_ID,
                        "provider item id") < 0) {
        wire_item_free(item);
        return NULL;
    }
    ++stream->item_count;
    return item;
}

static struct snag_wire_item *
find_item(struct snag_responses_stream *stream, size_t output_index,
          const char *id, enum snag_wire_item_kind kind)
{
    struct snag_wire_item *item;

    if (output_index >= stream->item_count ||
        !(item = &stream->items[output_index])->present ||
        item->kind != kind || !id || !item->id || strcmp(item->id, id) != 0) {
        (void)stream_fail(stream, EPROTO,
                          "response item identity or order conflict");
        return NULL;
    }
    return item;
}

static struct snag_wire_part *
part_at(struct snag_responses_stream *stream, struct snag_wire_item *item,
        size_t content_index, enum snag_wire_part_kind kind, bool create)
{
    struct snag_wire_part *parts;
    struct snag_wire_part *part;
    size_t cap;

    if (content_index > item->part_count ||
        content_index >= SNAG_MAX_RESPONSE_PARTS) {
        (void)stream_fail(stream, EPROTO,
                          "message content indexes are not contiguous");
        return NULL;
    }
    if (content_index == item->part_count) {
        if (!create || stream->part_count >= SNAG_MAX_RESPONSE_PARTS) {
            (void)stream_fail(stream, EPROTO,
                              "message content part was not announced");
            return NULL;
        }
        if (item->part_count == item->part_cap) {
            cap = item->part_cap ? item->part_cap * 2u : 4u;
            if (cap > SNAG_MAX_RESPONSE_PARTS)
                cap = SNAG_MAX_RESPONSE_PARTS;
            parts = realloc(item->parts, cap * sizeof(*parts));
            if (!parts) {
                (void)stream_fail(stream, ENOMEM,
                                  "cannot allocate message content parts");
                return NULL;
            }
            memset(parts + item->part_cap, 0,
                   (cap - item->part_cap) * sizeof(*parts));
            item->parts = parts;
            item->part_cap = cap;
        }
        part = &item->parts[item->part_count++];
        memset(part, 0, sizeof(*part));
        snag_buf_init(&part->text, SNAG_MAX_PUBLIC_ITEM);
        part->kind = kind;
        part->present = true;
        ++stream->part_count;
        return part;
    }
    part = &item->parts[content_index];
    if (!part->present || part->kind != kind) {
        (void)stream_fail(stream, EPROTO,
                          "message content kind changed");
        return NULL;
    }
    return part;
}

static enum snag_item_phase
phase_value(const char *phase)
{
    if (phase && strcmp(phase, "commentary") == 0)
        return SNAG_PHASE_COMMENTARY;
    if (phase && strcmp(phase, "final_answer") == 0)
        return SNAG_PHASE_FINAL_ANSWER;
    return SNAG_PHASE_NONE;
}

static enum snag_item_kind
public_kind(enum snag_wire_part_kind kind)
{
    return kind == SNAG_WIRE_PART_REFUSAL ? SNAG_ITEM_REFUSAL :
                                           SNAG_ITEM_ASSISTANT;
}

static int
emit_text(struct snag_responses_stream *stream, size_t output_index,
          struct snag_wire_item *item, enum snag_wire_part_kind kind,
          const char *text, size_t len)
{
    enum snag_item_phase phase = item->phase ? phase_value(item->phase) :
                                SNAG_PHASE_COMMENTARY;

    if (!len || !stream->emit)
        return 0;
    if (phase == SNAG_PHASE_NONE)
        return stream_fail(stream, EPROTO,
                           "assistant message has no qualified phase");
    if (kind == SNAG_WIRE_PART_REFUSAL) {
        if (item->phase && phase != SNAG_PHASE_FINAL_ANSWER)
            return stream_fail(stream, EPROTO,
                               "refusal has no final-answer phase");
        phase = SNAG_PHASE_FINAL_ANSWER;
    }
    if (stream->emit(stream->opaque, output_index, public_kind(kind), phase,
                     item->id, text, len) != 0)
        return stream_fail(stream, errno ? errno : EIO,
                           "public output consumer failed");
    return 0;
}

static int
append_part_delta(struct snag_responses_stream *stream, size_t output_index,
                  struct snag_wire_item *item, size_t content_index,
                  enum snag_wire_part_kind kind, const char *delta)
{
    struct snag_wire_part *part;
    size_t len;

    if (!delta || !snag_utf8_valid((const unsigned char *)delta,
                                  strlen(delta), true))
        return stream_fail(stream, EPROTO, "invalid public output delta");
    len = strlen(delta);
    part = part_at(stream, item, content_index, kind, false);
    if (!part)
        return -1;
    if (part->complete)
        return stream_fail(stream, EPROTO, "public delta follows completion");
    if (len > SNAG_MAX_PUBLIC_ITEM - part->text.len) {
        stream->output_correction = SNAG_OUTPUT_CORRECTION_OVERSIZED;
        return stream_fail(stream, EOVERFLOW,
                           SNAG_OVERSIZED_OUTPUT_CORRECTION);
    }
    if (account_bytes(stream, len) < 0 ||
        snag_buf_append(&part->text, delta, len) < 0)
        return stream_fail(stream, EOVERFLOW,
                           "public response item exceeds its limit");
    part->value_seen = true;
    return emit_text(stream, output_index, item, kind, delta, len);
}

static int
reconcile_part(struct snag_responses_stream *stream, size_t output_index,
               struct snag_wire_item *item, size_t content_index,
               enum snag_wire_part_kind kind, const char *text,
               bool complete)
{
    struct snag_wire_part *part;
    size_t len;

    if (!text)
        return stream_fail(stream, EPROTO, "public snapshot has no text");
    len = strlen(text);
    if (!snag_utf8_valid((const unsigned char *)text, len, true))
        return stream_fail(stream, EPROTO, "invalid public snapshot text");
    part = part_at(stream, item, content_index, kind, true);
    if (!part)
        return -1;
    if (len > SNAG_MAX_PUBLIC_ITEM) {
        stream->output_correction = SNAG_OUTPUT_CORRECTION_OVERSIZED;
        return stream_fail(stream, EOVERFLOW,
                           SNAG_OVERSIZED_OUTPUT_CORRECTION);
    }
    if (part->value_seen) {
        if (!text_equal(&part->text, text, len))
            return stream_fail(stream, EPROTO,
                               "public delta and snapshot disagree");
    } else {
        if (account_bytes(stream, len) < 0 ||
            snag_buf_append(&part->text, text, len) < 0)
            return stream_fail(stream, EOVERFLOW,
                               "public response item exceeds its limit");
        part->value_seen = true;
        if (emit_text(stream, output_index, item, kind, text, len) < 0)
            return -1;
    }
    if (complete)
        part->complete = true;
    return 0;
}

static int
part_snapshot(struct snag_responses_stream *stream, size_t output_index,
              struct snag_wire_item *item, size_t content_index,
              const json_t *part, bool complete)
{
    const char *type = snag_json_string(part, "type");
    const char *text;

    if (!json_is_object(part) || !type)
        return stream_fail(stream, EPROTO, "invalid message content part");
    if (strcmp(type, "output_text") == 0) {
        text = snag_json_string(part, "text");
        return reconcile_part(stream, output_index, item, content_index,
                              SNAG_WIRE_PART_TEXT, text, complete);
    }
    if (strcmp(type, "refusal") == 0) {
        text = snag_json_string(part, "refusal");
        return reconcile_part(stream, output_index, item, content_index,
                              SNAG_WIRE_PART_REFUSAL, text, complete);
    }
    {
        struct snag_wire_part *wire_part = part_at(stream, item, content_index,
                                                  SNAG_WIRE_PART_INERT, true);

        if (!wire_part)
            return -1;
        if (complete)
            wire_part->complete = true;
        return 0;
    }
}

static int
reconcile_arguments(struct snag_responses_stream *stream,
                    struct snag_wire_item *item, const char *arguments,
                    bool complete)
{
    size_t len;

    if (!arguments)
        return stream_fail(stream, EPROTO,
                           "function call snapshot has no arguments");
    len = strlen(arguments);
    if (len > SNAG_MAX_TOOL_ARGUMENTS ||
        !snag_utf8_valid((const unsigned char *)arguments, len, true))
        return stream_fail(stream, EPROTO,
                           "invalid function call arguments snapshot");
    if (item->arguments_seen) {
        if (!text_equal(&item->arguments, arguments, len))
            return stream_fail(stream, EPROTO,
                               "function argument delta and snapshot disagree");
    } else {
        if (account_bytes(stream, len) < 0 ||
            snag_buf_append(&item->arguments, arguments, len) < 0)
            return stream_fail(stream, EOVERFLOW,
                               "function arguments exceed their limit");
        item->arguments_seen = true;
    }
    if (complete)
        item->arguments_complete = true;
    return 0;
}

static int
append_arguments_delta(struct snag_responses_stream *stream,
                       struct snag_wire_item *item, const char *delta)
{
    size_t len;

    if (!delta)
        return stream_fail(stream, EPROTO,
                           "function argument delta is not text");
    len = strlen(delta);
    if (!snag_utf8_valid((const unsigned char *)delta, len, true) ||
        item->arguments_complete)
        return stream_fail(stream, EPROTO,
                           "invalid function argument delta order");
    if (account_bytes(stream, len) < 0 ||
        snag_buf_append(&item->arguments, delta, len) < 0)
        return stream_fail(stream, EOVERFLOW,
                           "function arguments exceed their limit");
    item->arguments_seen = true;
    return 0;
}

static int
message_snapshot(struct snag_responses_stream *stream, size_t output_index,
                 const json_t *snapshot, bool complete)
{
    const char *id = snag_json_string(snapshot, "id");
    const char *role = snag_json_string(snapshot, "role");
    const char *phase = snag_json_string(snapshot, "phase");
    const char *status = snag_json_string(snapshot, "status");
    json_t *content = json_object_get(snapshot, "content");
    struct snag_wire_item *item;

    if (!id || !role || strcmp(role, "assistant") != 0 ||
        (phase && phase_value(phase) == SNAG_PHASE_NONE) ||
        !json_is_array(content) || !status ||
        (complete ? strcmp(status, "completed") != 0 :
                    strcmp(status, "in_progress") != 0))
        return stream_fail(stream, EPROTO,
                           "invalid assistant message snapshot");
    item = output_index < stream->item_count ?
           find_item(stream, output_index, id, SNAG_WIRE_ITEM_MESSAGE) :
           new_item(stream, output_index, SNAG_WIRE_ITEM_MESSAGE, id);
    if (!item)
        return -1;
    if (phase) {
        if (copy_once(stream, &item->phase, phase, 32u,
                      "assistant phase") < 0)
            return -1;
        item->phase_present = true;
    }
    for (size_t i = 0; i < json_array_size(content); ++i)
        if (part_snapshot(stream, output_index, item, i,
                          json_array_get(content, i), complete) < 0)
            return -1;
    if (complete && item->part_count != json_array_size(content))
        return stream_fail(stream, EPROTO,
                           "message completion snapshot omitted observed content");
    if (complete)
        item->complete = true;
    return 0;
}

static int
function_snapshot(struct snag_responses_stream *stream, size_t output_index,
                  const json_t *snapshot, bool complete)
{
    const char *id = snag_json_string(snapshot, "id");
    const char *call_id = snag_json_string(snapshot, "call_id");
    const char *name = snag_json_string(snapshot, "name");
    const char *arguments = snag_json_string(snapshot, "arguments");
    const char *status = snag_json_string(snapshot, "status");
    struct snag_wire_item *item;

    if (!id || !call_id || !name || !arguments || !status ||
        (complete ? strcmp(status, "completed") != 0 :
                    (strcmp(status, "in_progress") != 0 &&
                     strcmp(status, "completed") != 0)))
        return stream_fail(stream, EPROTO, "invalid function call snapshot");
    item = output_index < stream->item_count ?
           find_item(stream, output_index, id, SNAG_WIRE_ITEM_FUNCTION_CALL) :
           new_item(stream, output_index, SNAG_WIRE_ITEM_FUNCTION_CALL, id);
    if (!item ||
        copy_once(stream, &item->call_id, call_id, SNAG_MAX_PROVIDER_ID,
                  "provider call id") < 0 ||
        copy_once(stream, &item->name, name, 64u, "function name") < 0 ||
        reconcile_arguments(stream, item, arguments,
                            complete || strcmp(status, "completed") == 0) < 0)
        return -1;
    if (complete || strcmp(status, "completed") == 0)
        item->complete = true;
    return 0;
}

static int
inert_snapshot(struct snag_responses_stream *stream, size_t output_index)
{
    struct snag_wire_item *item;

    if (output_index < stream->item_count) {
        item = &stream->items[output_index];
        if (!item->present || item->kind != SNAG_WIRE_ITEM_INERT)
            return stream_fail(stream, EPROTO,
                               "response item kind or order conflict");
        return 0;
    }
    return new_item(stream, output_index, SNAG_WIRE_ITEM_INERT, NULL) ? 0 : -1;
}

static int
item_snapshot(struct snag_responses_stream *stream, size_t output_index,
              const json_t *snapshot, bool complete)
{
    const char *type = snag_json_string(snapshot, "type");

    if (!json_is_object(snapshot) || !type)
        return stream_fail(stream, EPROTO, "invalid response output item");
    if (strcmp(type, "message") == 0)
        return message_snapshot(stream, output_index, snapshot, complete);
    if (strcmp(type, "function_call") == 0)
        return function_snapshot(stream, output_index, snapshot, complete);
    return inert_snapshot(stream, output_index);
}

static int
response_identity(struct snag_responses_stream *stream, const json_t *response,
                  const char *required_status)
{
    const char *id = snag_json_string(response, "id");
    const char *status = snag_json_string(response, "status");

    if (!json_is_object(response) || !id || !status ||
        strcmp(status, required_status) != 0)
        return stream_fail(stream, EPROTO,
                           "invalid response %s snapshot", required_status);
    if (copy_once(stream, &stream->response_id, id, SNAG_MAX_PROVIDER_ID,
                  "provider response id") < 0)
        return -1;
    return 0;
}

static int
handle_response_created(struct snag_responses_stream *stream, const json_t *root)
{
    json_t *response = json_object_get(root, "response");
    json_t *output;

    if (stream->created || response_identity(stream, response, "in_progress") < 0)
        return stream_fail(stream, EPROTO,
                           "duplicate or invalid response.created event");
    output = json_object_get(response, "output");
    if (output && (!json_is_array(output) || json_array_size(output) != 0u))
        return stream_fail(stream, EPROTO,
                           "response.created contains nonempty output");
    stream->created = true;
    return 0;
}

static int
handle_output_item(struct snag_responses_stream *stream, const json_t *root,
                   bool complete)
{
    size_t output_index;
    json_t *item = json_object_get(root, "item");

    if (!stream->created || json_index(stream, root, "output_index",
                                       SNAG_MAX_RESPONSE_ITEMS,
                                       &output_index) < 0)
        return -1;
    return item_snapshot(stream, output_index, item, complete);
}

static int
handle_content_part(struct snag_responses_stream *stream, const json_t *root,
                    bool complete)
{
    const char *item_id = snag_json_string(root, "item_id");
    size_t output_index;
    size_t content_index;
    struct snag_wire_item *item;
    json_t *part = json_object_get(root, "part");

    if (json_index(stream, root, "output_index", SNAG_MAX_RESPONSE_ITEMS,
                   &output_index) < 0 ||
        json_index(stream, root, "content_index", SNAG_MAX_RESPONSE_PARTS,
                   &content_index) < 0)
        return -1;
    item = find_item(stream, output_index, item_id, SNAG_WIRE_ITEM_MESSAGE);
    if (!item)
        return -1;
    return part_snapshot(stream, output_index, item, content_index,
                         part, complete);
}

static int
handle_public_delta(struct snag_responses_stream *stream, const json_t *root,
                    enum snag_wire_part_kind kind)
{
    const char *item_id = snag_json_string(root, "item_id");
    const char *delta = snag_json_string(root, "delta");
    size_t output_index;
    size_t content_index;
    struct snag_wire_item *item;

    if (json_index(stream, root, "output_index", SNAG_MAX_RESPONSE_ITEMS,
                   &output_index) < 0 ||
        json_index(stream, root, "content_index", SNAG_MAX_RESPONSE_PARTS,
                   &content_index) < 0)
        return -1;
    item = find_item(stream, output_index, item_id, SNAG_WIRE_ITEM_MESSAGE);
    return item ? append_part_delta(stream, output_index, item, content_index,
                                    kind, delta) : -1;
}

static int
handle_public_done(struct snag_responses_stream *stream, const json_t *root,
                   enum snag_wire_part_kind kind)
{
    const char *item_id = snag_json_string(root, "item_id");
    const char *text = snag_json_string(root,
        kind == SNAG_WIRE_PART_REFUSAL ? "refusal" : "text");
    size_t output_index;
    size_t content_index;
    struct snag_wire_item *item;

    if (json_index(stream, root, "output_index", SNAG_MAX_RESPONSE_ITEMS,
                   &output_index) < 0 ||
        json_index(stream, root, "content_index", SNAG_MAX_RESPONSE_PARTS,
                   &content_index) < 0)
        return -1;
    item = find_item(stream, output_index, item_id, SNAG_WIRE_ITEM_MESSAGE);
    return item ? reconcile_part(stream, output_index, item, content_index,
                                 kind, text, true) : -1;
}

static int
handle_arguments_delta(struct snag_responses_stream *stream, const json_t *root)
{
    const char *item_id = snag_json_string(root, "item_id");
    const char *delta = snag_json_string(root, "delta");
    size_t output_index;
    struct snag_wire_item *item;

    if (json_index(stream, root, "output_index", SNAG_MAX_RESPONSE_ITEMS,
                   &output_index) < 0)
        return -1;
    item = find_item(stream, output_index, item_id,
                     SNAG_WIRE_ITEM_FUNCTION_CALL);
    return item ? append_arguments_delta(stream, item, delta) : -1;
}

static int
handle_arguments_done(struct snag_responses_stream *stream, const json_t *root)
{
    const char *item_id = snag_json_string(root, "item_id");
    const char *arguments = snag_json_string(root, "arguments");
    size_t output_index;
    struct snag_wire_item *item;

    if (json_index(stream, root, "output_index", SNAG_MAX_RESPONSE_ITEMS,
                   &output_index) < 0)
        return -1;
    item = find_item(stream, output_index, item_id,
                     SNAG_WIRE_ITEM_FUNCTION_CALL);
    return item ? reconcile_arguments(stream, item, arguments, true) : -1;
}

static int
provider_usage_member(const json_t *object, const char *key,
                      uint64_t *number, bool *known)
{
    json_t *value = json_object_get(object, key);

    if (!value || json_is_null(value)) {
        *number = 0u;
        *known = false;
        return 0;
    }
    if (!json_is_integer(value) || json_integer_value(value) < 0)
        return -1;
    *number = (uint64_t)json_integer_value(value);
    *known = true;
    return 0;
}

static int
parse_provider_usage(struct snag_responses_stream *stream,
                     const json_t *response)
{
    json_t *value = json_object_get(response, "usage");
    json_t *details;
    struct snag_response_usage usage;

    memset(&usage, 0, sizeof(usage));
    if (!value || json_is_null(value)) {
        stream->usage = usage;
        return 0;
    }
    if (!json_is_object(value) ||
        provider_usage_member(value, "input_tokens", &usage.input_tokens,
                              &usage.input_known) < 0 ||
        provider_usage_member(value, "output_tokens", &usage.output_tokens,
                              &usage.output_known) < 0 ||
        provider_usage_member(value, "total_tokens", &usage.total_tokens,
                              &usage.total_known) < 0)
        return stream_fail(stream, EPROTO, "invalid response usage");
    details = json_object_get(value, "output_tokens_details");
    if (details && !json_is_null(details)) {
        if (!json_is_object(details) ||
            provider_usage_member(details, "reasoning_tokens",
                                  &usage.reasoning_tokens,
                                  &usage.reasoning_known) < 0)
            return stream_fail(stream, EPROTO,
                               "invalid response reasoning usage");
    }
    if (snag_response_usage_valid(&usage) < 0)
        return stream_fail(stream, EPROTO,
                           "inconsistent response usage");
    stream->usage = usage;
    return 0;
}

static int
handle_response_completed(struct snag_responses_stream *stream,
                          const json_t *root)
{
    json_t *response = json_object_get(root, "response");
    json_t *output;

    if (!stream->created || stream->terminal ||
        response_identity(stream, response, "completed") < 0)
        return -1;
    if (parse_provider_usage(stream, response) < 0)
        return -1;
    output = json_object_get(response, "output");
    if (output) {
        if (!json_is_array(output) ||
            json_array_size(output) > SNAG_MAX_RESPONSE_ITEMS)
            return stream_fail(stream, EPROTO,
                               "invalid terminal response output");
        for (size_t i = 0; i < json_array_size(output); ++i)
            if (item_snapshot(stream, i, json_array_get(output, i),
                              true) < 0)
                return -1;
    }
    stream->terminal = true;
    return 0;
}

static int
handle_provider_failure(struct snag_responses_stream *stream,
                        const json_t *root, const char *type)
{
    json_t *response = json_object_get(root, "response");
    json_t *error = json_object_get(root, "error");
    const char *message = NULL;
    struct snag_provider_failure failure;

    if (json_is_object(response)) {
        json_t *nested = json_object_get(response, "error");
        if (json_is_object(nested))
            message = snag_json_string(nested, "message");
    }
    if (!message && json_is_object(error))
        message = snag_json_string(error, "message");
    if (!message && strcmp(type, "error") == 0)
        message = snag_json_string(root, "message");
    if (snag_provider_failure_from_json(root, &failure) < 0)
        return stream_fail(stream, EPROTO,
                           "invalid structured provider failure");
    stream->provider_failure = failure;
    const char *kind = failure.code[0] ? failure.code : failure.type;
    return stream_fail(stream, EIO, "%s%s%s%s%s%s", type,
                       kind[0] ? " [" : "", kind, kind[0] ? "]" : "",
                       message ? ": " : "", message ? message : "");
}

static int
dispatch_event(struct snag_responses_stream *stream, const char *type,
               const json_t *root)
{
    json_t *output = json_object_get(json_object_get(root, "response"), "output");
    if (output && (!json_is_array(output) || json_array_size(output)))
        stream->retry_unsafe = true;
    if (strcmp(type, "keepalive") == 0)
        return 0;
    if (strcmp(type, "response.created") == 0)
        return handle_response_created(stream, root);
    /* Only lifecycle notices prove no output or hosted-tool activity. */
    if (strcmp(type, "response.queued") != 0 &&
        strcmp(type, "response.in_progress") != 0 &&
        strcmp(type, "response.failed") != 0 &&
        strcmp(type, "response.incomplete") != 0 && strcmp(type, "error") != 0)
        stream->retry_unsafe = true;
    if (strcmp(type, "response.output_item.added") == 0)
        return handle_output_item(stream, root, false);
    if (strcmp(type, "response.output_item.done") == 0)
        return handle_output_item(stream, root, true);
    if (strcmp(type, "response.content_part.added") == 0)
        return handle_content_part(stream, root, false);
    if (strcmp(type, "response.content_part.done") == 0)
        return handle_content_part(stream, root, true);
    if (strcmp(type, "response.output_text.delta") == 0)
        return handle_public_delta(stream, root, SNAG_WIRE_PART_TEXT);
    if (strcmp(type, "response.output_text.done") == 0)
        return handle_public_done(stream, root, SNAG_WIRE_PART_TEXT);
    if (strcmp(type, "response.refusal.delta") == 0)
        return handle_public_delta(stream, root, SNAG_WIRE_PART_REFUSAL);
    if (strcmp(type, "response.refusal.done") == 0)
        return handle_public_done(stream, root, SNAG_WIRE_PART_REFUSAL);
    if (strcmp(type, "response.function_call_arguments.delta") == 0)
        return handle_arguments_delta(stream, root);
    if (strcmp(type, "response.function_call_arguments.done") == 0)
        return handle_arguments_done(stream, root);
    if (strcmp(type, "response.completed") == 0)
        return handle_response_completed(stream, root);
    if (strcmp(type, "response.failed") == 0 ||
        strcmp(type, "response.incomplete") == 0 ||
        strcmp(type, "error") == 0)
        return handle_provider_failure(stream, root, type);
    if (strncmp(type, "response.", sizeof("response.") - 1u) == 0)
        return 0;
    return stream_fail(stream, ENOTSUP,
                       "unknown Responses stream event: %s", type);
}

int
snag_responses_sse_record(void *opaque, const struct snag_sse_record *record)
{
    struct snag_responses_stream *stream = opaque;
    json_t *root;
    const char *type;
    char json_error[192] = {0};
    int rc;

    if (stream->failed) {
        errno = EPROTO;
        return -1;
    }
    if (record->kind == SNAG_SSE_COMMENT)
        return 0;
    /* OpenRouter appends an SSE sentinel after the Responses terminal event. */
    if (stream->terminal && !record->event_len && record->data_len == 6u &&
        memcmp(record->data, "[DONE]", 6u) == 0)
        return 0;
    if (stream->terminal)
        return stream_fail(stream, EPROTO,
                           "Responses event follows terminal completion");
    root = snag_json_load_strict(record->data, record->data_len,
                                SNAG_MAX_SSE_EVENT,
                                json_error, sizeof(json_error));
    if (!root)
        return stream_fail(stream, EPROTO, "invalid Responses JSON: %s",
                           json_error);
    type = snag_json_string(root, "type");
    if (!json_is_object(root) || !type) {
        json_decref(root);
        return stream_fail(stream, EPROTO,
                           "Responses event has no type");
    }
    if (record->event_len &&
        (strlen(type) != record->event_len ||
         memcmp(type, record->event, record->event_len) != 0)) {
        json_decref(root);
        return stream_fail(stream, EPROTO,
                           "SSE event name and JSON type disagree");
    }
    rc = dispatch_event(stream, type, root);
    json_decref(root);
    return rc;
}

static bool
message_observations_complete(const struct snag_wire_item *item)
{
    if (!item->part_count)
        return false;
    for (size_t i = 0; i < item->part_count; ++i)
        if (!item->parts[i].complete)
            return false;
    return true;
}

static int
build_message(struct snag_responses_stream *stream,
              struct snag_response_graph *graph,
              const struct snag_wire_item *item)
{
    enum snag_wire_part_kind kind;
    enum snag_item_phase phase;
    struct snag_buf text;
    size_t public_parts = 0u;
    int rc;

    phase = item->phase ? phase_value(item->phase) : SNAG_PHASE_COMMENTARY;
    if ((!item->complete && !message_observations_complete(item)) ||
        phase == SNAG_PHASE_NONE)
        return stream_fail(stream, EPROTO,
                           "assistant message did not complete coherently");
    kind = SNAG_WIRE_PART_NONE;
    snag_buf_init(&text, SNAG_MAX_PUBLIC_ITEM + 1u);
    for (size_t i = 0; i < item->part_count; ++i) {
        const struct snag_wire_part *part = &item->parts[i];

        if (!part->complete) {
            snag_buf_free(&text);
            return stream_fail(stream, EPROTO,
                               "assistant message has mixed or invalid content");
        }
        if (part->kind == SNAG_WIRE_PART_INERT)
            continue;
        if (kind == SNAG_WIRE_PART_NONE)
            kind = part->kind;
        if (part->kind != kind) {
            snag_buf_free(&text);
            return stream_fail(stream, EPROTO,
                               "assistant message has mixed or invalid content");
        }
        if (part->text.len > SNAG_MAX_PUBLIC_ITEM - text.len) {
            stream->output_correction = SNAG_OUTPUT_CORRECTION_OVERSIZED;
            snag_buf_free(&text);
            return 1;
        }
        if (snag_buf_append(&text, part->text.data, part->text.len) < 0) {
            snag_buf_free(&text);
            return stream_fail(stream, errno ? errno : ENOMEM,
                               "cannot retain assistant message");
        }
        ++public_parts;
    }
    if (kind == SNAG_WIRE_PART_NONE) {
        snag_buf_free(&text);
        if (!item->part_count) {
            stream->output_correction = SNAG_OUTPUT_CORRECTION_EMPTY;
            return 1;
        }
        return 0;
    }
    if (!text.len) {
        stream->output_correction = SNAG_OUTPUT_CORRECTION_EMPTY;
        snag_buf_free(&text);
        return 1;
    }
    if (snag_buf_terminate(&text) < 0) {
        snag_buf_free(&text);
        return stream_fail(stream, errno ? errno : ENOMEM,
                           "cannot terminate assistant message");
    }
    if (kind == SNAG_WIRE_PART_REFUSAL) {
        if (public_parts != 1u ||
            (item->phase && phase != SNAG_PHASE_FINAL_ANSWER)) {
            snag_buf_free(&text);
            return stream_fail(stream, EPROTO,
                               "refusal has an invalid phase or content shape");
        }
        rc = snag_response_graph_add_public(graph, SNAG_ITEM_REFUSAL,
                                            SNAG_PHASE_FINAL_ANSWER,
                                            item->id, (char *)text.data);
    } else {
        rc = snag_response_graph_add_public(graph, SNAG_ITEM_ASSISTANT, phase,
                                            item->id, (char *)text.data);
    }
    snag_buf_free(&text);
    if (rc < 0)
        return stream_fail(stream, errno ? errno : EPROTO,
                           "cannot build canonical assistant item");
    return 0;
}

static int
build_call(struct snag_responses_stream *stream,
           struct snag_response_graph *graph,
           const struct snag_wire_item *item)
{
    json_t *arguments;
    char json_error[192] = {0};

    if (!item->arguments_complete || !item->arguments_seen ||
        !item->name || !item->call_id)
        return stream_fail(stream, EPROTO,
                           "function call did not complete coherently");
    arguments = snag_json_load_strict(item->arguments.data, item->arguments.len,
                                     SNAG_MAX_TOOL_ARGUMENTS,
                                     json_error, sizeof(json_error));
    if (!arguments || !json_is_object(arguments)) {
        if (arguments)
            json_decref(arguments);
        return stream_fail(stream, EPROTO,
                           "function arguments are not one strict object: %s",
                           json_error);
    }
    if (snag_response_graph_add_call(graph, item->id, item->call_id,
                                    item->name, arguments) < 0)
        return stream_fail(stream, errno ? errno : EPROTO,
                           "function call is outside the registered schema");
    return 0;
}

static void
normalize_implicit_message_terminal(struct snag_response_graph *graph)
{
    size_t last_assistant = graph->count;
    bool has_call = false;
    bool has_terminal_public = false;

    for (size_t i = 0; i < graph->count; ++i) {
        struct snag_response_item *item = &graph->items[i];
        if (item->kind == SNAG_ITEM_TOOL_CALL)
            has_call = true;
        if ((item->kind == SNAG_ITEM_ASSISTANT &&
             item->phase == SNAG_PHASE_FINAL_ANSWER) ||
            item->kind == SNAG_ITEM_REFUSAL)
            has_terminal_public = true;
        if (item->kind == SNAG_ITEM_ASSISTANT)
            last_assistant = i;
    }
    if (!has_call && !has_terminal_public && last_assistant < graph->count)
        graph->items[last_assistant].phase = SNAG_PHASE_FINAL_ANSWER;
}

int
snag_responses_stream_finish(struct snag_responses_stream *stream,
                            struct snag_response_graph *graph,
                            char *error, size_t error_size)
{
    struct snag_response_graph staged;
    int rc = -1;

    if (stream->failed || !stream->created || !stream->terminal ||
        !stream->response_id) {
        if (!stream->failed)
            (void)stream_fail(stream, EPROTO,
                              "Responses stream ended before completion");
        goto out;
    }
    snag_response_graph_init(&staged);
    staged.usage = stream->usage;
    if (snag_response_graph_set_provider_id(&staged, stream->response_id) < 0) {
        (void)stream_fail(stream, errno ? errno : EPROTO,
                          "invalid provider response id");
        goto staged_out;
    }
    for (size_t i = 0; i < stream->item_count; ++i) {
        struct snag_wire_item *item = &stream->items[i];
        if (!item->present) {
            (void)stream_fail(stream, EPROTO,
                              "response output contains a gap");
            goto staged_out;
        }
        if (item->kind == SNAG_WIRE_ITEM_MESSAGE) {
            rc = build_message(stream, &staged, item);
            if (rc != 0)
                goto staged_out;
        } else if (item->kind == SNAG_WIRE_ITEM_FUNCTION_CALL) {
            if (build_call(stream, &staged, item) < 0)
                goto staged_out;
        } else if (item->kind != SNAG_WIRE_ITEM_INERT) {
            (void)stream_fail(stream, EPROTO,
                              "response output item has no recognized kind");
            goto staged_out;
        }
    }
    normalize_implicit_message_terminal(&staged);
    snag_response_graph_free(graph);
    *graph = staged;
    rc = 0;
    goto out;
staged_out:
    snag_response_graph_free(&staged);
out:
    if (rc > 0 && error_size)
        (void)snprintf(error, error_size, "%s",
            stream->output_correction == SNAG_OUTPUT_CORRECTION_EMPTY ?
                SNAG_EMPTY_OUTPUT_CORRECTION :
                SNAG_OVERSIZED_OUTPUT_CORRECTION);
    else if (rc < 0 && error_size)
        (void)snprintf(error, error_size, "%s",
                       snag_responses_stream_error(stream));
    return rc;
}
