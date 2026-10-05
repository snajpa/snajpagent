/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_producer.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int
replace_event(struct snag_binary_producer *writer, const struct snag_binary_event *event)
{
    struct snag_buf replacement = {.max = SNAG_MAX_EVENT_LINE};
    if (snag_binary_event_encode(&replacement, event) < 0) {
        snag_buf_free(&replacement);
        return -1;
    }
    snag_buf_free(&writer->field);
    writer->field = replacement;
    return 0;
}

static void
clear_input(struct snag_binary_input_source *source)
{
    json_decref(source->paths);
    *source = (struct snag_binary_input_source){0};
}

static struct snag_binary_input_reference
input_reference(const struct snag_binary_producer *writer, uint64_t sequence,
    enum snag_binary_input_leaf field, const unsigned char *bytes, size_t size)
{
    if (!bytes) return (struct snag_binary_input_reference){0};
    /* Decoded field views lie inside the existing bounded literal payload. */
    return (struct snag_binary_input_reference){.field = field,
        .target = {sequence, (uint32_t)(bytes - writer->field.data), (uint32_t)size}};
}

static int
remember_input(struct snag_binary_producer *writer, const struct snag_session *state,
    uint64_t sequence, const struct snag_binary_record *record)
{
    if (record->kind != SNAG_BINARY_INPUT_RECEIVED && record->kind != SNAG_BINARY_IRC_ADMITTED &&
        record->kind != SNAG_BINARY_FUTURE_TURN_QUEUED &&
        record->kind != SNAG_BINARY_FUTURE_TURN_EDITED) return 0;
    struct snag_binary_event event;
    if (snag_binary_event_decode(record, &event) < 0) return -1;
    if (event.kind == SNAG_BINARY_IRC_ADMITTED) {
        if (event.data.irc_admitted.input.kind != SNAG_BINARY_INPUT_RECEIVED) return 0;
        struct snag_binary_record input = event.data.irc_admitted.input;
        if (snag_binary_event_decode(&input, &event) < 0) return -1;
    }
    if (event.kind == SNAG_BINARY_INPUT_RECEIVED) {
        if (!state->pending_input) return snag_errno(EINVAL);
        clear_input(&writer->input);
        writer->input.creation = sequence;
        writer->input.text = input_reference(writer, sequence, SNAG_BINARY_INPUT_TEXT,
            event.data.input.text.data, event.data.input.text.size);
        writer->input.content = input_reference(writer, sequence, SNAG_BINARY_INPUT_CONTENT,
            event.data.input.content.data, event.data.input.content.size);
        writer->input.instructions = input_reference(writer, sequence,
            SNAG_BINARY_INPUT_INSTRUCTIONS, event.data.input.instructions.data,
            event.data.input.instructions.size);
        writer->input.paths = json_incref(json_object_get(state->pending_input, "instructions"));
        return 0;
    }
    size_t index = writer->queue_count;
    if (event.kind == SNAG_BINARY_FUTURE_TURN_QUEUED) {
        if (index == SIZE_MAX || state->pending_queue_count != index + 1u ||
            state->pending_queue[index].seq != sequence) return snag_errno(EINVAL);
        if (index == writer->queue_capacity) {
            size_t capacity = state->pending_queue_capacity;
            if (capacity <= index || capacity > SIZE_MAX / sizeof(*writer->queue)) {
                return snag_errno(EOVERFLOW);
            }
            struct snag_binary_input_source *grown =
                realloc(writer->queue, capacity * sizeof(*grown));
            if (!grown) return -1;
            writer->queue = grown;
            writer->queue_capacity = capacity;
        }
        writer->queue[index] = (struct snag_binary_input_source){.creation = sequence};
        memcpy(writer->queue[index].id, event.data.queued.id, sizeof(event.data.queued.id));
        ++writer->queue_count;
        writer->queue[index].content = input_reference(writer, sequence, SNAG_BINARY_INPUT_CONTENT,
            event.data.queued.content.data, event.data.queued.content.size);
    } else {
        if (state->pending_queue_count != writer->queue_count) return snag_errno(EINVAL);
        for (index = 0u; index < writer->queue_count; ++index) {
            if (!memcmp(writer->queue[index].id, event.data.queued.id,
                sizeof(event.data.queued.id))) {
                break;
            }
        }
        if (index == writer->queue_count ||
            writer->queue[index].creation != state->pending_queue[index].seq) {
            return snag_errno(EINVAL);
        }
    }
    /* Edits replace text even when identical, but never the original content. */
    writer->queue[index].text = input_reference(writer, sequence, SNAG_BINARY_INPUT_TEXT,
        event.data.queued.text.data, event.data.queued.text.size);
    return 0;
}

static int
prune_inputs(struct snag_binary_producer *writer, const struct snag_session *state, uint16_t kind)
{
    if (!state->pending_input) clear_input(&writer->input);
    if (kind != SNAG_BINARY_FUTURE_TURN_CANCELLED && kind != SNAG_BINARY_TURN_STARTED) return 0;
    size_t kept = 0u;
    for (size_t i = 0u; i < writer->queue_count; ++i) {
        if (kept < state->pending_queue_count &&
            writer->queue[i].creation == state->pending_queue[kept].seq) {
            if (i != kept) {
                writer->queue[kept] = writer->queue[i];
                writer->queue[i] = (struct snag_binary_input_source){0};
            }
            ++kept;
        } else {
            clear_input(&writer->queue[i]);
        }
    }
    writer->queue_count = kept;
    return kept == state->pending_queue_count ? 0 : snag_errno(EINVAL);
}

static int
reference_turn(struct snag_binary_producer *writer, struct snag_binary_event *event,
    const json_t *data)
{
    struct snag_binary_turn_start *turn = &event->data.started;
    if (turn->origin == SNAG_BINARY_TURN_GOAL) return 0;
    const struct snag_binary_input_source *source = &writer->input;
    bool queued = turn->origin == SNAG_BINARY_TURN_QUEUED;
    if (queued) {
        if (!writer->queue_count || writer->queue[0].creation != turn->queue_seq ||
            memcmp(writer->queue[0].id, turn->queue_id, sizeof(turn->queue_id))) {
            return snag_errno(EINVAL);
        }
        source = &writer->queue[0];
    }
    if (!source->creation) return 0;
    /* The post-reduction callback has already enforced exact receipt text/content.
     * Instructions have their own turn domain and still require comparison. */
    turn->text_ref = source->text;
    turn->text = (struct snag_binary_text){0};
    turn->content_ref = source->content;
    if (turn->content_ref.field) {
        turn->content = (struct snag_binary_content){0};
    }
    if (!queued && json_equal(source->paths, json_object_get(data, "instructions"))) {
        turn->instructions_ref = source->instructions;
        turn->instructions = (struct snag_binary_instructions){0};
    }
    return replace_event(writer, event);
}

static void
clear_output(struct snag_binary_producer *writer)
{
    free(writer->outputs);
    writer->outputs = NULL;
    writer->output_count = writer->output_capacity = 0u;
    json_decref(writer->public);
    writer->public = NULL;
}

static int
remember_output(struct snag_binary_producer *writer, const struct snag_session *state,
    uint64_t sequence, const struct snag_binary_response_output *output)
{
    size_t count = writer->output_count;
    if (output->index > count || (output->index < count && output->index + 1u != count)) {
        return snag_errno(EINVAL);
    }
    if (output->index == count) {
        if (count == writer->output_capacity) {
            size_t capacity = count ? count * 2u : 8u;
            if (capacity < count || capacity > SIZE_MAX / sizeof(*writer->outputs)) {
                return snag_errno(EOVERFLOW);
            }
            struct snag_binary_output_source *grown =
                realloc(writer->outputs, capacity * sizeof(*grown));
            if (!grown) return -1;
            writer->outputs = grown;
            writer->output_capacity = capacity;
        }
        size_t offset = (size_t)(output->item.text.data - writer->field.data);
        writer->outputs[count] = (struct snag_binary_output_source){.index = count,
            .value = {.item = output->item, .source = {
                .first = {sequence, (uint32_t)offset, (uint32_t)output->item.text.size}}}};
        /* Metadata text lives in the retained reducer array, not the field buffer. */
        writer->outputs[count].value.item.provider_id = (struct snag_binary_text){0};
        writer->outputs[count].value.item.text = (struct snag_binary_text){0};
        ++writer->output_count;
    }
    struct snag_binary_output_span *span = &writer->outputs[output->index].value.source;
    if (output->offset != span->bytes || output->item.text.size > UINT32_MAX - span->bytes ||
        json_array_size(state->response_public) != writer->output_count) {
        return snag_errno(EINVAL);
    }
    span->last_sequence = sequence;
    span->bytes += (uint32_t)output->item.text.size;
    json_t *public = json_incref(state->response_public);
    json_decref(writer->public);
    writer->public = public;
    return 0;
}

static int
output_compare(const void *left, const void *right)
{
    const struct snag_binary_output_source *a = left, *b = right;
    return memcmp(a->value.item.id, b->value.item.id, sizeof(a->value.item.id));
}

static void
reference_output(const struct snag_binary_producer *writer, struct snag_binary_public_value *value)
{
    struct snag_binary_output_source key = {.value.item = value->item};
    const struct snag_binary_output_source *source = bsearch(&key, writer->outputs,
        writer->output_count, sizeof(*writer->outputs), output_compare);
    if (!source || value->item.kind != source->value.item.kind ||
        value->item.phase != source->value.item.phase) {
        return;
    }
    struct snag_binary_output_span span = source->value.source;
    if (value->item.text.size != span.bytes) {
        if (value->item.text.size != span.first.size) return;
        span.last_sequence = span.first.sequence;
        span.bytes = span.first.size;
    }
    json_t *item = json_array_get(writer->public, source->index);
    const char *provider = snag_json_string(item, "provider_item_id");
    const char *text = snag_json_string(item, "text");
    if (!provider || !text || strlen(provider) != value->item.provider_id.size ||
        memcmp(provider, value->item.provider_id.data, value->item.provider_id.size) ||
        strlen(text) < span.bytes || memcmp(text, value->item.text.data, span.bytes)) {
        return;
    }
    value->source = span;
    value->item.text = (struct snag_binary_text){0};
}

static int
reference_snapshot(struct snag_binary_producer *writer, struct snag_binary_event *event)
{
    struct snag_binary_public_items *partial = NULL;
    struct snag_binary_graph_items *graph = NULL;
    switch (event->kind) {
    case SNAG_BINARY_RESPONSE_INTERRUPTED:
        partial = &event->data.response_interrupted.partial; break;
    case SNAG_BINARY_RESPONSE_FAILED: partial = &event->data.response_failed.partial; break;
    case SNAG_BINARY_RESPONSE_OUTPUT_CORRECTION: partial = &event->data.response_correction.partial;
        break;
    case SNAG_BINARY_RESPONSE_COMPLETED: graph = &event->data.response_completed.items; break;
    default: return 0;
    }
    if (!writer->output_count) return 0;
    /* These consumers close the response. Sort once, retaining original stream indices. */
    qsort(writer->outputs, writer->output_count, sizeof(*writer->outputs), output_compare);
    struct snag_buf items = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_binary_public_value *public = NULL;
    struct snag_binary_graph_item *values = NULL;
    size_t offset = 0u, count = graph ? graph->count : 0u;
    int rc = -1;
    if (partial) {
        struct snag_binary_public_value value;
        int next;
        while ((next = snag_binary_public_items_next(partial, &offset, &value)) == 0) {
            ++count;
        }
        if (next < 0) goto out;
        if (count && !(public = calloc(count, sizeof(*public)))) goto out;
        offset = 0u;
        for (size_t i = 0u; i < count; ++i) {
            if (snag_binary_public_items_next(partial, &offset, &public[i]) != 0) {
                goto out;
            }
            reference_output(writer, &public[i]);
        }
        if (snag_binary_public_items_encode(&items, public, count) < 0) goto out;
        *partial = (struct snag_binary_public_items){items.data, items.len};
    } else {
        if (count && !(values = calloc(count, sizeof(*values)))) goto out;
        for (size_t i = 0u; i < count; ++i) {
            if (snag_binary_graph_items_next(graph, &offset, &values[i]) != 0) {
                goto out;
            }
            if (values[i].kind != SNAG_BINARY_ITEM_TOOL_CALL) {
                reference_output(writer, &values[i].data.output);
            }
        }
        if (snag_binary_graph_items_encode(&items, values, count) < 0 ||
            snag_binary_graph_items_decode(items.data, items.len, graph) < 0) {
            goto out;
        }
    }
    rc = replace_event(writer, event);
out:
    free(public);
    free(values);
    snag_buf_free(&items);
    return rc;
}

void
snag_binary_producer_free(struct snag_binary_producer *producer)
{
    if (!producer) return;
    clear_input(&producer->input);
    for (size_t i = 0u; i < producer->queue_count; ++i) clear_input(&producer->queue[i]);
    free(producer->queue);
    clear_output(producer);
    snag_buf_free(&producer->field);
    *producer = (struct snag_binary_producer){0};
}

int
snag_binary_producer_clone(struct snag_binary_producer *destination,
    const struct snag_binary_producer *source)
{
    if (!destination || !source || destination == source ||
        (source->queue_count && !source->queue) ||
        (source->output_count && !source->outputs)) return snag_errno(EINVAL);
    if (source->queue_count > SIZE_MAX / sizeof(*source->queue) ||
        source->output_count > SIZE_MAX / sizeof(*source->outputs)) return snag_errno(EOVERFLOW);
    struct snag_binary_producer staged = {.field = {.max = SNAG_MAX_EVENT_LINE},
        .input = source->input, .public = json_incref(source->public)};
    staged.input.paths = json_incref(source->input.paths);
    if (source->queue_count) {
        staged.queue = malloc(source->queue_count * sizeof(*staged.queue));
        if (!staged.queue) goto fail;
        memcpy(staged.queue, source->queue, source->queue_count * sizeof(*staged.queue));
        staged.queue_count = staged.queue_capacity = source->queue_count;
        for (size_t i = 0u; i < staged.queue_count; ++i) json_incref(staged.queue[i].paths);
    }
    if (source->output_count) {
        staged.outputs = malloc(source->output_count * sizeof(*staged.outputs));
        if (!staged.outputs) goto fail;
        memcpy(staged.outputs, source->outputs, source->output_count * sizeof(*staged.outputs));
        staged.output_count = staged.output_capacity = source->output_count;
    }
    snag_binary_producer_free(destination);
    *destination = staged;
    return 0;
fail:
    {
        int code = errno;
        snag_binary_producer_free(&staged);
        errno = code;
    }
    return -1;
}

int
snag_binary_producer_reference(struct snag_binary_producer *producer,
    const struct snag_session *state, uint64_t sequence, struct snag_binary_record *record,
    const json_t *data)
{
    if (!producer || !state || !record || !data || !sequence ||
        record->payload != producer->field.data || record->size != producer->field.len) {
        return snag_errno(EINVAL);
    }
    if (remember_input(producer, state, sequence, record) < 0) return -1;
    uint16_t kind = record->kind;
    if (kind == SNAG_BINARY_TURN_STARTED ||
        (kind >= SNAG_BINARY_RESPONSE_OUTPUT && kind <= SNAG_BINARY_RESPONSE_COMPLETED)) {
        struct snag_binary_event event;
        if (snag_binary_event_decode(record, &event) < 0) return -1;
        if (kind == SNAG_BINARY_TURN_STARTED) {
            if (reference_turn(producer, &event, data) < 0) return -1;
        } else if (kind == SNAG_BINARY_RESPONSE_OUTPUT) {
            if (remember_output(producer, state, sequence, &event.data.response_output) < 0) {
                return -1;
            }
        } else if (reference_snapshot(producer, &event) < 0) {
            return -1;
        }
    }
    if (prune_inputs(producer, state, kind) < 0) return -1;
    if (!state->response_open) clear_output(producer);
    record->payload = producer->field.data;
    record->size = producer->field.len;
    return 0;
}
