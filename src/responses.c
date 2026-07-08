/* SPDX-License-Identifier: GPL-2.0-only */
#include "responses.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
stream_fail(struct snj_responses_stream *stream, int code, const char *fmt, ...)
{
    va_list ap;

    stream->failed = true;
    va_start(ap, fmt);
    (void)vsnprintf(stream->error, sizeof(stream->error), fmt, ap);
    va_end(ap);
    errno = code;
    return -1;
}

static bool
text_equal(const struct snj_buf *buf, const char *text, size_t len)
{
    return buf->len == len && (!len || memcmp(buf->data, text, len) == 0);
}

static int
copy_once(struct snj_responses_stream *stream, char **target,
          const char *value, size_t max, const char *label)
{
    char *copy;
    size_t len;

    if (!value || !(len = strlen(value)) || len > max ||
        !snj_utf8_valid((const unsigned char *)value, len, true))
        return stream_fail(stream, EPROTO, "invalid %s", label);
    if (*target) {
        if (strcmp(*target, value) != 0)
            return stream_fail(stream, EPROTO, "conflicting %s", label);
        return 0;
    }
    copy = snj_strdup_checked(value, max);
    if (!copy)
        return stream_fail(stream, errno ? errno : ENOMEM,
                           "cannot retain %s", label);
    *target = copy;
    return 0;
}

static int
json_index(struct snj_responses_stream *stream, const json_t *object,
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
account_bytes(struct snj_responses_stream *stream, size_t extra)
{
    if (extra > SNJ_MAX_RESPONSE_GRAPH - stream->aggregate_bytes)
        return stream_fail(stream, EOVERFLOW,
                           "response observations exceed 8 MiB");
    stream->aggregate_bytes += extra;
    return 0;
}

static void
wire_part_free(struct snj_wire_part *part)
{
    snj_buf_free(&part->text);
    memset(part, 0, sizeof(*part));
}

static void
wire_item_free(struct snj_wire_item *item)
{
    free(item->id);
    free(item->phase);
    free(item->name);
    free(item->call_id);
    snj_buf_free(&item->arguments);
    for (size_t i = 0; i < item->part_count; ++i)
        wire_part_free(&item->parts[i]);
    free(item->parts);
    memset(item, 0, sizeof(*item));
}

void
snj_responses_stream_init(struct snj_responses_stream *stream,
                          snj_responses_emit_fn emit, void *opaque)
{
    memset(stream, 0, sizeof(*stream));
    stream->emit = emit;
    stream->opaque = opaque;
}

void
snj_responses_stream_free(struct snj_responses_stream *stream)
{
    for (size_t i = 0; i < stream->item_count; ++i)
        wire_item_free(&stream->items[i]);
    free(stream->response_id);
    memset(stream, 0, sizeof(*stream));
}

const char *
snj_responses_stream_error(const struct snj_responses_stream *stream)
{
    return stream->error[0] ? stream->error : "Responses stream failed";
}

static struct snj_wire_item *
new_item(struct snj_responses_stream *stream, size_t output_index,
         enum snj_wire_item_kind kind, const char *id)
{
    struct snj_wire_item *item;

    if (output_index != stream->item_count ||
        output_index >= SNJ_MAX_RESPONSE_ITEMS) {
        (void)stream_fail(stream, EPROTO,
                          "response output indexes are not contiguous");
        return NULL;
    }
    item = &stream->items[stream->item_count];
    memset(item, 0, sizeof(*item));
    snj_buf_init(&item->arguments, SNJ_MAX_TOOL_ARGUMENTS);
    item->kind = kind;
    item->present = true;
    if (copy_once(stream, &item->id, id, SNJ_MAX_PROVIDER_ID,
                  "provider item id") < 0) {
        wire_item_free(item);
        return NULL;
    }
    ++stream->item_count;
    return item;
}

static struct snj_wire_item *
find_item(struct snj_responses_stream *stream, size_t output_index,
          const char *id, enum snj_wire_item_kind kind)
{
    struct snj_wire_item *item;

    if (output_index >= stream->item_count ||
        !(item = &stream->items[output_index])->present ||
        item->kind != kind || !id || !item->id || strcmp(item->id, id) != 0) {
        (void)stream_fail(stream, EPROTO,
                          "response item identity or order conflict");
        return NULL;
    }
    return item;
}

static struct snj_wire_part *
part_at(struct snj_responses_stream *stream, struct snj_wire_item *item,
        size_t content_index, enum snj_wire_part_kind kind, bool create)
{
    struct snj_wire_part *parts;
    struct snj_wire_part *part;
    size_t cap;

    if (content_index > item->part_count ||
        content_index >= SNJ_MAX_RESPONSE_PARTS) {
        (void)stream_fail(stream, EPROTO,
                          "message content indexes are not contiguous");
        return NULL;
    }
    if (content_index == item->part_count) {
        if (!create || stream->part_count >= SNJ_MAX_RESPONSE_PARTS) {
            (void)stream_fail(stream, EPROTO,
                              "message content part was not announced");
            return NULL;
        }
        if (item->part_count == item->part_cap) {
            cap = item->part_cap ? item->part_cap * 2u : 4u;
            if (cap > SNJ_MAX_RESPONSE_PARTS)
                cap = SNJ_MAX_RESPONSE_PARTS;
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
        snj_buf_init(&part->text, SNJ_MAX_PUBLIC_ITEM);
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

static enum snj_item_phase
phase_value(const char *phase)
{
    if (phase && strcmp(phase, "commentary") == 0)
        return SNJ_PHASE_COMMENTARY;
    if (phase && strcmp(phase, "final_answer") == 0)
        return SNJ_PHASE_FINAL_ANSWER;
    return SNJ_PHASE_NONE;
}

static enum snj_item_kind
public_kind(enum snj_wire_part_kind kind)
{
    return kind == SNJ_WIRE_PART_REFUSAL ? SNJ_ITEM_REFUSAL :
                                           SNJ_ITEM_ASSISTANT;
}

static int
emit_text(struct snj_responses_stream *stream, size_t output_index,
          struct snj_wire_item *item, enum snj_wire_part_kind kind,
          const char *text, size_t len)
{
    enum snj_item_phase phase = item->phase ? phase_value(item->phase) :
                                SNJ_PHASE_COMMENTARY;

    if (!len || !stream->emit)
        return 0;
    if (phase == SNJ_PHASE_NONE)
        return stream_fail(stream, EPROTO,
                           "assistant message has no qualified phase");
    if (kind == SNJ_WIRE_PART_REFUSAL) {
        if (item->phase && phase != SNJ_PHASE_FINAL_ANSWER)
            return stream_fail(stream, EPROTO,
                               "refusal has no final-answer phase");
        phase = SNJ_PHASE_FINAL_ANSWER;
    }
    if (stream->emit(stream->opaque, output_index, public_kind(kind), phase,
                     item->id, text, len) != 0)
        return stream_fail(stream, errno ? errno : EIO,
                           "public output consumer failed");
    return 0;
}

static int
append_part_delta(struct snj_responses_stream *stream, size_t output_index,
                  struct snj_wire_item *item, size_t content_index,
                  enum snj_wire_part_kind kind, const char *delta)
{
    struct snj_wire_part *part;
    size_t len;

    if (!delta || !snj_utf8_valid((const unsigned char *)delta,
                                  strlen(delta), true))
        return stream_fail(stream, EPROTO, "invalid public output delta");
    len = strlen(delta);
    part = part_at(stream, item, content_index, kind, false);
    if (!part)
        return -1;
    if (part->complete)
        return stream_fail(stream, EPROTO, "public delta follows completion");
    if (account_bytes(stream, len) < 0 ||
        snj_buf_append(&part->text, delta, len) < 0)
        return stream_fail(stream, EOVERFLOW,
                           "public response item exceeds its limit");
    part->value_seen = true;
    return emit_text(stream, output_index, item, kind, delta, len);
}

static int
reconcile_part(struct snj_responses_stream *stream, size_t output_index,
               struct snj_wire_item *item, size_t content_index,
               enum snj_wire_part_kind kind, const char *text,
               bool create, bool complete)
{
    struct snj_wire_part *part;
    size_t len;

    if (!text)
        return stream_fail(stream, EPROTO, "public snapshot has no text");
    len = strlen(text);
    if (len > SNJ_MAX_PUBLIC_ITEM ||
        !snj_utf8_valid((const unsigned char *)text, len, true))
        return stream_fail(stream, EPROTO, "invalid public snapshot text");
    part = part_at(stream, item, content_index, kind, create);
    if (!part)
        return -1;
    if (part->value_seen) {
        if (!text_equal(&part->text, text, len))
            return stream_fail(stream, EPROTO,
                               "public delta and snapshot disagree");
    } else {
        if (account_bytes(stream, len) < 0 ||
            snj_buf_append(&part->text, text, len) < 0)
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
part_snapshot(struct snj_responses_stream *stream, size_t output_index,
              struct snj_wire_item *item, size_t content_index,
              const json_t *part, bool create, bool complete)
{
    const char *type = snj_json_string(part, "type");
    const char *text;

    if (!json_is_object(part) || !type)
        return stream_fail(stream, EPROTO, "invalid message content part");
    if (strcmp(type, "output_text") == 0) {
        json_t *annotations = json_object_get(part, "annotations");
        json_t *logprobs = json_object_get(part, "logprobs");

        text = snj_json_string(part, "text");
        if ((annotations && (!json_is_array(annotations) ||
                             json_array_size(annotations) != 0u)) ||
            (logprobs && (!json_is_array(logprobs) ||
                          json_array_size(logprobs) != 0u)))
            return stream_fail(stream, ENOTSUP,
                               "output annotations or logprobs are unqualified");
        return reconcile_part(stream, output_index, item, content_index,
                              SNJ_WIRE_PART_TEXT, text, create, complete);
    }
    if (strcmp(type, "refusal") == 0) {
        text = snj_json_string(part, "refusal");
        return reconcile_part(stream, output_index, item, content_index,
                              SNJ_WIRE_PART_REFUSAL, text, create, complete);
    }
    return stream_fail(stream, ENOTSUP,
                       "unsupported message content type %s", type);
}

static int
reconcile_arguments(struct snj_responses_stream *stream,
                    struct snj_wire_item *item, const char *arguments,
                    bool complete)
{
    size_t len;

    if (!arguments)
        return stream_fail(stream, EPROTO,
                           "function call snapshot has no arguments");
    len = strlen(arguments);
    if (len > SNJ_MAX_TOOL_ARGUMENTS ||
        !snj_utf8_valid((const unsigned char *)arguments, len, true))
        return stream_fail(stream, EPROTO,
                           "invalid function call arguments snapshot");
    if (item->arguments_seen) {
        if (!text_equal(&item->arguments, arguments, len))
            return stream_fail(stream, EPROTO,
                               "function argument delta and snapshot disagree");
    } else {
        if (account_bytes(stream, len) < 0 ||
            snj_buf_append(&item->arguments, arguments, len) < 0)
            return stream_fail(stream, EOVERFLOW,
                               "function arguments exceed their limit");
        item->arguments_seen = true;
    }
    if (complete)
        item->arguments_complete = true;
    return 0;
}

static int
append_arguments_delta(struct snj_responses_stream *stream,
                       struct snj_wire_item *item, const char *delta)
{
    size_t len;

    if (!delta)
        return stream_fail(stream, EPROTO,
                           "function argument delta is not text");
    len = strlen(delta);
    if (!snj_utf8_valid((const unsigned char *)delta, len, true) ||
        item->arguments_complete)
        return stream_fail(stream, EPROTO,
                           "invalid function argument delta order");
    if (account_bytes(stream, len) < 0 ||
        snj_buf_append(&item->arguments, delta, len) < 0)
        return stream_fail(stream, EOVERFLOW,
                           "function arguments exceed their limit");
    item->arguments_seen = true;
    return 0;
}

static int
message_snapshot(struct snj_responses_stream *stream, size_t output_index,
                 const json_t *snapshot, bool create, bool complete)
{
    const char *id = snj_json_string(snapshot, "id");
    const char *role = snj_json_string(snapshot, "role");
    const char *phase = snj_json_string(snapshot, "phase");
    const char *status = snj_json_string(snapshot, "status");
    json_t *content = json_object_get(snapshot, "content");
    struct snj_wire_item *item;

    if (!id || !role || strcmp(role, "assistant") != 0 ||
        (phase && phase_value(phase) == SNJ_PHASE_NONE) ||
        !json_is_array(content) || !status ||
        (complete ? strcmp(status, "completed") != 0 :
                    strcmp(status, "in_progress") != 0))
        return stream_fail(stream, EPROTO,
                           "invalid assistant message snapshot");
    if (create) {
        item = output_index < stream->item_count ?
               find_item(stream, output_index, id, SNJ_WIRE_ITEM_MESSAGE) :
               new_item(stream, output_index, SNJ_WIRE_ITEM_MESSAGE, id);
    } else {
        item = find_item(stream, output_index, id, SNJ_WIRE_ITEM_MESSAGE);
    }
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
                          json_array_get(content, i), true, complete) < 0)
            return -1;
    if (complete && item->part_count != json_array_size(content))
        return stream_fail(stream, EPROTO,
                           "message completion snapshot omitted observed content");
    if (complete)
        item->complete = true;
    return 0;
}

static int
function_snapshot(struct snj_responses_stream *stream, size_t output_index,
                  const json_t *snapshot, bool create, bool complete)
{
    const char *id = snj_json_string(snapshot, "id");
    const char *call_id = snj_json_string(snapshot, "call_id");
    const char *name = snj_json_string(snapshot, "name");
    const char *arguments = snj_json_string(snapshot, "arguments");
    const char *status = snj_json_string(snapshot, "status");
    struct snj_wire_item *item;

    if (!id || !call_id || !name || !arguments || !status ||
        (complete ? strcmp(status, "completed") != 0 :
                    (strcmp(status, "in_progress") != 0 &&
                     strcmp(status, "completed") != 0)))
        return stream_fail(stream, EPROTO, "invalid function call snapshot");
    if (create) {
        item = output_index < stream->item_count ?
               find_item(stream, output_index, id,
                         SNJ_WIRE_ITEM_FUNCTION_CALL) :
               new_item(stream, output_index, SNJ_WIRE_ITEM_FUNCTION_CALL, id);
    } else {
        item = find_item(stream, output_index, id,
                         SNJ_WIRE_ITEM_FUNCTION_CALL);
    }
    if (!item ||
        copy_once(stream, &item->call_id, call_id, SNJ_MAX_PROVIDER_ID,
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
empty_array_member(struct snj_responses_stream *stream, const json_t *snapshot,
                   const char *name)
{
    json_t *value = json_object_get(snapshot, name);

    if (!json_is_array(value) || json_array_size(value) != 0u)
        return stream_fail(stream, ENOTSUP,
                           "reasoning output item contains public %s", name);
    return 0;
}

static int
reasoning_snapshot(struct snj_responses_stream *stream, size_t output_index,
                   const json_t *snapshot, bool create, bool complete)
{
    const char *id = snj_json_string(snapshot, "id");
    const char *status = snj_json_string(snapshot, "status");
    struct snj_wire_item *item;

    if (!id || (status &&
        (complete ? strcmp(status, "completed") != 0 :
                    strcmp(status, "in_progress") != 0)))
        return stream_fail(stream, EPROTO, "invalid reasoning item snapshot");
    if (empty_array_member(stream, snapshot, "content") < 0 ||
        empty_array_member(stream, snapshot, "summary") < 0)
        return -1;
    if (create) {
        item = output_index < stream->item_count ?
               find_item(stream, output_index, id, SNJ_WIRE_ITEM_REASONING) :
               new_item(stream, output_index, SNJ_WIRE_ITEM_REASONING, id);
    } else {
        item = find_item(stream, output_index, id, SNJ_WIRE_ITEM_REASONING);
    }
    if (!item)
        return -1;
    if (complete)
        item->complete = true;
    return 0;
}

static int
web_search_snapshot(struct snj_responses_stream *stream, size_t output_index,
                    const json_t *snapshot, bool create, bool complete)
{
    const char *id = snj_json_string(snapshot, "id");
    const char *status = snj_json_string(snapshot, "status");
    struct snj_wire_item *item;

    if (!id || (status &&
        (complete ? strcmp(status, "completed") != 0 :
                    (strcmp(status, "in_progress") != 0 &&
                     strcmp(status, "searching") != 0 &&
                     strcmp(status, "completed") != 0))))
        return stream_fail(stream, EPROTO, "invalid web search item snapshot");
    if (create) {
        item = output_index < stream->item_count ?
               find_item(stream, output_index, id, SNJ_WIRE_ITEM_WEB_SEARCH) :
               new_item(stream, output_index, SNJ_WIRE_ITEM_WEB_SEARCH, id);
    } else {
        item = find_item(stream, output_index, id, SNJ_WIRE_ITEM_WEB_SEARCH);
    }
    if (!item)
        return -1;
    if (complete || (status && strcmp(status, "completed") == 0))
        item->complete = true;
    return 0;
}

static int
item_snapshot(struct snj_responses_stream *stream, size_t output_index,
              const json_t *snapshot, bool create, bool complete)
{
    const char *type = snj_json_string(snapshot, "type");

    if (!json_is_object(snapshot) || !type)
        return stream_fail(stream, EPROTO, "invalid response output item");
    if (strcmp(type, "message") == 0)
        return message_snapshot(stream, output_index, snapshot,
                                create, complete);
    if (strcmp(type, "function_call") == 0)
        return function_snapshot(stream, output_index, snapshot,
                                 create, complete);
    if (strcmp(type, "reasoning") == 0)
        return reasoning_snapshot(stream, output_index, snapshot,
                                  create, complete);
    if (strcmp(type, "web_search_call") == 0)
        return web_search_snapshot(stream, output_index, snapshot,
                                   create, complete);
    return stream_fail(stream, ENOTSUP,
                       "unsupported response output item type %s", type);
}

static int
response_identity(struct snj_responses_stream *stream, const json_t *response,
                  const char *required_status)
{
    const char *id = snj_json_string(response, "id");
    const char *status = snj_json_string(response, "status");

    if (!json_is_object(response) || !id || !status ||
        strcmp(status, required_status) != 0)
        return stream_fail(stream, EPROTO,
                           "invalid response %s snapshot", required_status);
    if (copy_once(stream, &stream->response_id, id, SNJ_MAX_PROVIDER_ID,
                  "provider response id") < 0)
        return -1;
    return 0;
}

static int
handle_response_created(struct snj_responses_stream *stream, const json_t *root)
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
handle_response_in_progress(struct snj_responses_stream *stream,
                            const json_t *root)
{
    json_t *response = json_object_get(root, "response");

    if (!stream->created)
        return stream_fail(stream, EPROTO,
                           "response.in_progress precedes response.created");
    return response_identity(stream, response, "in_progress");
}

static int
handle_output_item(struct snj_responses_stream *stream, const json_t *root,
                   bool complete)
{
    size_t output_index;
    json_t *item = json_object_get(root, "item");

    if (!stream->created || json_index(stream, root, "output_index",
                                       SNJ_MAX_RESPONSE_ITEMS,
                                       &output_index) < 0)
        return -1;
    return item_snapshot(stream, output_index, item, true, complete);
}

static int
handle_content_part(struct snj_responses_stream *stream, const json_t *root,
                    bool complete)
{
    const char *item_id = snj_json_string(root, "item_id");
    size_t output_index;
    size_t content_index;
    struct snj_wire_item *item;
    json_t *part = json_object_get(root, "part");

    if (json_index(stream, root, "output_index", SNJ_MAX_RESPONSE_ITEMS,
                   &output_index) < 0 ||
        json_index(stream, root, "content_index", SNJ_MAX_RESPONSE_PARTS,
                   &content_index) < 0)
        return -1;
    item = find_item(stream, output_index, item_id, SNJ_WIRE_ITEM_MESSAGE);
    if (!item)
        return -1;
    return part_snapshot(stream, output_index, item, content_index,
                         part, true, complete);
}

static int
handle_public_delta(struct snj_responses_stream *stream, const json_t *root,
                    enum snj_wire_part_kind kind)
{
    const char *item_id = snj_json_string(root, "item_id");
    const char *delta = snj_json_string(root, "delta");
    size_t output_index;
    size_t content_index;
    struct snj_wire_item *item;

    if (json_index(stream, root, "output_index", SNJ_MAX_RESPONSE_ITEMS,
                   &output_index) < 0 ||
        json_index(stream, root, "content_index", SNJ_MAX_RESPONSE_PARTS,
                   &content_index) < 0)
        return -1;
    item = find_item(stream, output_index, item_id, SNJ_WIRE_ITEM_MESSAGE);
    return item ? append_part_delta(stream, output_index, item, content_index,
                                    kind, delta) : -1;
}

static int
handle_public_done(struct snj_responses_stream *stream, const json_t *root,
                   enum snj_wire_part_kind kind)
{
    const char *item_id = snj_json_string(root, "item_id");
    const char *text = snj_json_string(root,
        kind == SNJ_WIRE_PART_REFUSAL ? "refusal" : "text");
    size_t output_index;
    size_t content_index;
    struct snj_wire_item *item;

    if (json_index(stream, root, "output_index", SNJ_MAX_RESPONSE_ITEMS,
                   &output_index) < 0 ||
        json_index(stream, root, "content_index", SNJ_MAX_RESPONSE_PARTS,
                   &content_index) < 0)
        return -1;
    item = find_item(stream, output_index, item_id, SNJ_WIRE_ITEM_MESSAGE);
    return item ? reconcile_part(stream, output_index, item, content_index,
                                 kind, text, true, true) : -1;
}

static int
handle_arguments_delta(struct snj_responses_stream *stream, const json_t *root)
{
    const char *item_id = snj_json_string(root, "item_id");
    const char *delta = snj_json_string(root, "delta");
    size_t output_index;
    struct snj_wire_item *item;

    if (json_index(stream, root, "output_index", SNJ_MAX_RESPONSE_ITEMS,
                   &output_index) < 0)
        return -1;
    item = find_item(stream, output_index, item_id,
                     SNJ_WIRE_ITEM_FUNCTION_CALL);
    return item ? append_arguments_delta(stream, item, delta) : -1;
}

static int
handle_arguments_done(struct snj_responses_stream *stream, const json_t *root)
{
    const char *item_id = snj_json_string(root, "item_id");
    const char *arguments = snj_json_string(root, "arguments");
    size_t output_index;
    struct snj_wire_item *item;

    if (json_index(stream, root, "output_index", SNJ_MAX_RESPONSE_ITEMS,
                   &output_index) < 0)
        return -1;
    item = find_item(stream, output_index, item_id,
                     SNJ_WIRE_ITEM_FUNCTION_CALL);
    return item ? reconcile_arguments(stream, item, arguments, true) : -1;
}

static int
handle_web_search_call_event(struct snj_responses_stream *stream,
                             const json_t *root, bool complete)
{
    const char *item_id = snj_json_string(root, "item_id");
    size_t output_index;
    struct snj_wire_item *item;

    if (!stream->created ||
        json_index(stream, root, "output_index", SNJ_MAX_RESPONSE_ITEMS,
                   &output_index) < 0)
        return -1;
    item = find_item(stream, output_index, item_id, SNJ_WIRE_ITEM_WEB_SEARCH);
    if (!item)
        return -1;
    if (complete)
        item->complete = true;
    return 0;
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
parse_provider_usage(struct snj_responses_stream *stream,
                     const json_t *response)
{
    json_t *value = json_object_get(response, "usage");
    json_t *details;
    struct snj_response_usage usage;

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
    if (snj_response_usage_valid(&usage) < 0)
        return stream_fail(stream, EPROTO,
                           "inconsistent response usage");
    stream->usage = usage;
    return 0;
}

static int
handle_response_completed(struct snj_responses_stream *stream,
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
            json_array_size(output) > SNJ_MAX_RESPONSE_ITEMS)
            return stream_fail(stream, EPROTO,
                               "invalid terminal response output");
        for (size_t i = 0; i < json_array_size(output); ++i)
            if (item_snapshot(stream, i, json_array_get(output, i),
                              true, true) < 0)
                return -1;
    }
    stream->terminal = true;
    return 0;
}

static int
handle_provider_failure(struct snj_responses_stream *stream,
                        const json_t *root, const char *type)
{
    json_t *response = json_object_get(root, "response");
    json_t *error = json_object_get(root, "error");
    const char *message = NULL;

    if (json_is_object(response)) {
        json_t *nested = json_object_get(response, "error");
        if (json_is_object(nested))
            message = snj_json_string(nested, "message");
    }
    if (!message && json_is_object(error))
        message = snj_json_string(error, "message");
    return stream_fail(stream, EIO, "%s%s%s", type,
                       message ? ": " : "", message ? message : "");
}

static int
dispatch_event(struct snj_responses_stream *stream, const char *type,
               const json_t *root)
{
    if (strcmp(type, "response.created") == 0)
        return handle_response_created(stream, root);
    if (strcmp(type, "response.in_progress") == 0)
        return handle_response_in_progress(stream, root);
    if (strcmp(type, "response.output_item.added") == 0)
        return handle_output_item(stream, root, false);
    if (strcmp(type, "response.output_item.done") == 0)
        return handle_output_item(stream, root, true);
    if (strcmp(type, "response.content_part.added") == 0)
        return handle_content_part(stream, root, false);
    if (strcmp(type, "response.content_part.done") == 0)
        return handle_content_part(stream, root, true);
    if (strcmp(type, "response.output_text.delta") == 0)
        return handle_public_delta(stream, root, SNJ_WIRE_PART_TEXT);
    if (strcmp(type, "response.output_text.done") == 0)
        return handle_public_done(stream, root, SNJ_WIRE_PART_TEXT);
    if (strcmp(type, "response.refusal.delta") == 0)
        return handle_public_delta(stream, root, SNJ_WIRE_PART_REFUSAL);
    if (strcmp(type, "response.refusal.done") == 0)
        return handle_public_done(stream, root, SNJ_WIRE_PART_REFUSAL);
    if (strcmp(type, "response.function_call_arguments.delta") == 0)
        return handle_arguments_delta(stream, root);
    if (strcmp(type, "response.function_call_arguments.done") == 0)
        return handle_arguments_done(stream, root);
    if (strcmp(type, "response.web_search_call.in_progress") == 0 ||
        strcmp(type, "response.web_search_call.searching") == 0)
        return handle_web_search_call_event(stream, root, false);
    if (strcmp(type, "response.web_search_call.completed") == 0)
        return handle_web_search_call_event(stream, root, true);
    if (strcmp(type, "response.completed") == 0)
        return handle_response_completed(stream, root);
    if (strcmp(type, "response.failed") == 0 ||
        strcmp(type, "response.incomplete") == 0 ||
        strcmp(type, "error") == 0)
        return handle_provider_failure(stream, root, type);
    if (strstr(type, "reasoning") || strstr(type, "summary"))
        return stream_fail(stream, ENOTSUP,
                           "reasoning stream event is not yet qualified: %s",
                           type);
    return stream_fail(stream, ENOTSUP,
                       "unknown Responses stream event: %s", type);
}

int
snj_responses_sse_record(void *opaque, const struct snj_sse_record *record)
{
    struct snj_responses_stream *stream = opaque;
    json_t *root;
    const char *type;
    char json_error[192] = {0};
    int rc;

    if (stream->failed) {
        errno = EPROTO;
        return -1;
    }
    if (record->kind == SNJ_SSE_COMMENT)
        return 0;
    if (stream->terminal)
        return stream_fail(stream, EPROTO,
                           "Responses event follows terminal completion");
    root = snj_json_load_strict(record->data, record->data_len,
                                SNJ_MAX_SSE_EVENT,
                                json_error, sizeof(json_error));
    if (!root)
        return stream_fail(stream, EPROTO, "invalid Responses JSON: %s",
                           json_error);
    type = snj_json_string(root, "type");
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
message_observations_complete(const struct snj_wire_item *item)
{
    if (!item->part_count)
        return false;
    for (size_t i = 0; i < item->part_count; ++i)
        if (!item->parts[i].complete)
            return false;
    return true;
}

static int
build_message(struct snj_responses_stream *stream,
              struct snj_response_graph *graph,
              const struct snj_wire_item *item)
{
    enum snj_wire_part_kind kind;
    enum snj_item_phase phase;
    struct snj_buf text;
    int rc;

    phase = item->phase ? phase_value(item->phase) : SNJ_PHASE_COMMENTARY;
    if ((!item->complete && !message_observations_complete(item)) ||
        phase == SNJ_PHASE_NONE)
        return stream_fail(stream, EPROTO,
                           "assistant message did not complete coherently");
    kind = item->parts[0].kind;
    snj_buf_init(&text, SNJ_MAX_PUBLIC_ITEM + 1u);
    for (size_t i = 0; i < item->part_count; ++i) {
        const struct snj_wire_part *part = &item->parts[i];
        if (part->kind != kind || !part->complete ||
            snj_buf_append(&text, part->text.data, part->text.len) < 0) {
            snj_buf_free(&text);
            return stream_fail(stream, EPROTO,
                               "assistant message has mixed or invalid content");
        }
    }
    if (!text.len || snj_buf_terminate(&text) < 0) {
        snj_buf_free(&text);
        return stream_fail(stream, EPROTO,
                           "assistant message is empty or oversized");
    }
    if (kind == SNJ_WIRE_PART_REFUSAL) {
        if (item->part_count != 1u ||
            (item->phase && phase != SNJ_PHASE_FINAL_ANSWER)) {
            snj_buf_free(&text);
            return stream_fail(stream, EPROTO,
                               "refusal has an invalid phase or content shape");
        }
        rc = snj_response_graph_add_public(graph, SNJ_ITEM_REFUSAL,
                                            SNJ_PHASE_FINAL_ANSWER,
                                            item->id, (char *)text.data);
    } else {
        rc = snj_response_graph_add_public(graph, SNJ_ITEM_ASSISTANT, phase,
                                            item->id, (char *)text.data);
    }
    snj_buf_free(&text);
    if (rc < 0)
        return stream_fail(stream, errno ? errno : EPROTO,
                           "cannot build canonical assistant item");
    return 0;
}

static int
build_call(struct snj_responses_stream *stream,
           struct snj_response_graph *graph,
           const struct snj_wire_item *item)
{
    json_t *arguments;
    char json_error[192] = {0};

    if ((!item->complete && !item->arguments_complete) ||
        !item->arguments_complete || !item->arguments_seen ||
        !item->name || !item->call_id)
        return stream_fail(stream, EPROTO,
                           "function call did not complete coherently");
    arguments = snj_json_load_strict(item->arguments.data, item->arguments.len,
                                     SNJ_MAX_TOOL_ARGUMENTS,
                                     json_error, sizeof(json_error));
    if (!arguments || !json_is_object(arguments)) {
        if (arguments)
            json_decref(arguments);
        return stream_fail(stream, EPROTO,
                           "function arguments are not one strict object: %s",
                           json_error);
    }
    if (snj_response_graph_add_call(graph, item->id, item->call_id,
                                    item->name, arguments) < 0)
        return stream_fail(stream, errno ? errno : EPROTO,
                           "function call is outside the registered schema");
    return 0;
}


static void
normalize_implicit_message_terminal(struct snj_response_graph *graph)
{
    size_t last_assistant = graph->count;
    bool has_call = false;
    bool has_terminal_public = false;

    for (size_t i = 0; i < graph->count; ++i) {
        struct snj_response_item *item = &graph->items[i];
        if (item->kind == SNJ_ITEM_TOOL_CALL)
            has_call = true;
        if ((item->kind == SNJ_ITEM_ASSISTANT &&
             item->phase == SNJ_PHASE_FINAL_ANSWER) ||
            item->kind == SNJ_ITEM_REFUSAL)
            has_terminal_public = true;
        if (item->kind == SNJ_ITEM_ASSISTANT)
            last_assistant = i;
    }
    if (!has_call && !has_terminal_public && last_assistant < graph->count)
        graph->items[last_assistant].phase = SNJ_PHASE_FINAL_ANSWER;
}

int
snj_responses_stream_finish(struct snj_responses_stream *stream,
                            struct snj_response_graph *graph,
                            char *error, size_t error_size)
{
    struct snj_response_graph staged;
    int rc = -1;

    if (stream->failed || !stream->created || !stream->terminal ||
        !stream->response_id) {
        if (!stream->failed)
            (void)stream_fail(stream, EPROTO,
                              "Responses stream ended before completion");
        goto out;
    }
    snj_response_graph_init(&staged);
    staged.usage = stream->usage;
    if (snj_response_graph_set_provider_id(&staged, stream->response_id) < 0) {
        (void)stream_fail(stream, errno ? errno : EPROTO,
                          "invalid provider response id");
        goto staged_out;
    }
    for (size_t i = 0; i < stream->item_count; ++i) {
        struct snj_wire_item *item = &stream->items[i];
        if (!item->present) {
            (void)stream_fail(stream, EPROTO,
                              "response output contains a gap");
            goto staged_out;
        }
        if (item->kind == SNJ_WIRE_ITEM_MESSAGE) {
            if (build_message(stream, &staged, item) < 0)
                goto staged_out;
        } else if (item->kind == SNJ_WIRE_ITEM_FUNCTION_CALL) {
            if (build_call(stream, &staged, item) < 0)
                goto staged_out;
        } else if (item->kind == SNJ_WIRE_ITEM_REASONING) {
            if (!item->complete) {
                (void)stream_fail(stream, EPROTO,
                                  "reasoning item did not complete coherently");
                goto staged_out;
            }
        } else if (item->kind == SNJ_WIRE_ITEM_WEB_SEARCH) {
            if (!item->complete) {
                (void)stream_fail(stream, EPROTO,
                                  "web search item did not complete coherently");
                goto staged_out;
            }
        } else {
            (void)stream_fail(stream, EPROTO,
                              "response output item has no recognized kind");
            goto staged_out;
        }
    }
    normalize_implicit_message_terminal(&staged);
    snj_response_graph_free(graph);
    *graph = staged;
    rc = 0;
    goto out;
staged_out:
    snj_response_graph_free(&staged);
out:
    if (rc < 0 && error_size)
        (void)snprintf(error, error_size, "%s",
                       snj_responses_stream_error(stream));
    return rc;
}
