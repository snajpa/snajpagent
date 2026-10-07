/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_producer.h"
#include "store_binary_checkpoint.h"
#include "store_binary_legacy.h"
#include "store_internal.h"

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

int
snag_binary_producer_live_result(struct snag_binary_producer *producer,
    const struct snag_session *committed, struct snag_binary_record *record)
{
    if (record->kind != SNAG_BINARY_TOOL_FINISHED && record->kind != SNAG_BINARY_PROCESS_CLOSED)
        return 0;
    struct snag_binary_event event;
    if (snag_binary_event_decode(record, &event) < 0) return -1;
    struct snag_binary_tool_result *result = event.kind == SNAG_BINARY_TOOL_FINISHED ?
        &event.data.tool_finished.result : &event.data.process_closed.result;
    if (!result->has_output_ref) return 0;
    struct snag_binary_tool_output_ref *ref = &result->output_ref;
    if (ref->native) return snag_errno(EINVAL);
    if (event.kind == SNAG_BINARY_PROCESS_CLOSED &&
        memcmp(event.data.process_closed.handle, ref->handle, sizeof(ref->handle)))
        return snag_errno(EINVAL);
    if (ref->log_end) {
        char handle[SNAG_ID_HEX_LEN + 1u];
        static const char hex[] = "0123456789abcdef";
        for (size_t i = 0u; i < sizeof(ref->handle); ++i) {
            handle[2u * i] = hex[ref->handle[i] >> 4];
            handle[2u * i + 1u] = hex[ref->handle[i] & 15u];
        }
        handle[SNAG_ID_HEX_LEN] = '\0';
        const struct snag_process_state *process = NULL;
        for (size_t i = 0u; i < committed->process_count; ++i) {
            if (!strcmp(committed->processes[i].handle, handle)) {
                process = &committed->processes[i];
                break;
            }
        }
        /* These are captured native cursors, not a lookup by caller-supplied
         * presentation offset. A stale endpoint must be recaptured by its
         * engine owner, never guessed or widened to include later records. */
        if (!process || committed->log_end < 0 ||
            ref->log_end != (uint64_t)committed->log_end ||
            ref->log_start != process->log_offset || !process->log_seq ||
            process->log_seq > committed->next_seq ||
            !snag_hex_is_lower(process->log_hash, SNAG_SHA256_HEX_LEN)) return snag_errno(EINVAL);
        ref->first_sequence = process->log_seq;
        ref->end_sequence = committed->next_seq;
    }
    ref->native = true;
    if (replace_event(producer, &event) < 0) return -1;
    record->payload = producer->field.data;
    record->size = producer->field.len;
    return 0;
}

static void
clear_input(struct snag_binary_input_source *source)
{
    json_decref(source->paths);
    *source = (struct snag_binary_input_source){0};
}

static struct snag_binary_input_reference
input_reference(const struct snag_binary_record *record, uint64_t sequence,
    enum snag_binary_input_leaf field, const unsigned char *bytes, size_t size)
{
    if (!bytes) return (struct snag_binary_input_reference){0};
    /* Decoded field views lie inside the existing bounded literal payload. */
    return (struct snag_binary_input_reference){.field = field,
        .target = {sequence, (uint32_t)(bytes - record->payload), (uint32_t)size}};
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
        writer->input.text = input_reference(record, sequence, SNAG_BINARY_INPUT_TEXT,
            event.data.input.text.data, event.data.input.text.size);
        writer->input.content = input_reference(record, sequence, SNAG_BINARY_INPUT_CONTENT,
            event.data.input.content.data, event.data.input.content.size);
        writer->input.instructions = input_reference(record, sequence,
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
        writer->queue[index].content = input_reference(record, sequence, SNAG_BINARY_INPUT_CONTENT,
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
    writer->queue[index].text = input_reference(record, sequence, SNAG_BINARY_INPUT_TEXT,
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
    uint64_t sequence, const struct snag_binary_record *record,
    const struct snag_binary_response_output *output)
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
        size_t offset = (size_t)(output->item.text.data - record->payload);
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
            if (remember_output(producer, state, sequence, record,
                    &event.data.response_output) < 0) {
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

static bool
id_equal(const unsigned char id[16], const char *text)
{
    static const char hex[] = "0123456789abcdef";
    if (!snag_hex_is_lower(text, SNAG_ID_HEX_LEN)) return false;
    for (size_t i = 0u; i < 16u; ++i) {
        if (text[i * 2u] != hex[id[i] >> 4u] || text[i * 2u + 1u] != hex[id[i] & 15u])
            return false;
    }
    return true;
}

static int
restore_record(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_index *access, uint64_t sequence,
    struct snag_buf *scratch, struct snag_binary_record *out,
    bool (*cancelled)(void *), void *opaque)
{
    if (cancelled && cancelled(opaque)) return snag_errno(ECANCELED);
    struct snag_binary_batch batch;
    struct snag_binary_anchor before;
    if (snag_binary_checkpoint_batch_find(fd, through, access, sequence,
            scratch, &batch, &before) < 0) return -1;
    size_t position = SNAG_BINARY_BATCH_HEADER_SIZE;
    struct snag_binary_record record;
    uint64_t found;
    int rc;
    while ((rc = snag_binary_record_next(&batch, &position, &record, &found)) == 0) {
        if (found != sequence) continue;
        if (record.flags) return snag_errno(EINVAL);
        *out = record;
        return 0;
    }
    return rc < 0 ? -1 : snag_errno(ENOENT);
}

struct restore_response {
    struct snag_binary_producer *producer;
    struct snag_session *state;
    uint64_t last;
    bool (*cancelled)(void *);
    void *opaque;
};

static bool
restore_cancelled(void *opaque)
{
    struct restore_response *restore = opaque;
    return restore->cancelled && restore->cancelled(restore->opaque);
}

static int
restore_output(void *opaque, const struct snag_binary_record *record, uint64_t sequence)
{
    struct restore_response *restore = opaque;
    uint16_t kind = record->kind;
    if (kind == SNAG_BINARY_RESPONSE_STARTED || kind == SNAG_BINARY_RESPONSE_COMPLETED ||
        kind == SNAG_BINARY_RESPONSE_FAILED || kind == SNAG_BINARY_RESPONSE_INTERRUPTED ||
        kind == SNAG_BINARY_RESPONSE_OUTPUT_CORRECTION || kind == SNAG_BINARY_TURN_STARTED ||
        kind == SNAG_BINARY_TURN_COMPLETED || kind == SNAG_BINARY_TURN_COMPLETED_SILENT ||
        kind == SNAG_BINARY_TURN_INTERRUPTED || kind == SNAG_BINARY_TURN_FAILED)
        return snag_errno(EINVAL);
    if (kind != SNAG_BINARY_RESPONSE_OUTPUT) return 0;
    struct snag_binary_event event;
    const char *type;
    json_t *data = NULL;
    if (record->flags) return snag_errno(EINVAL);
    if (snag_binary_event_decode(record, &event) < 0 ||
        snag_binary_legacy_decode(record, &type, &data) < 0) return -1;
    int rc = snag_store_reduce_event(restore->state, type, data, sequence, NULL, 0u);
    json_decref(data);
    if (!rc) rc = remember_output(restore->producer, restore->state, sequence,
        record, &event.data.response_output);
    if (!rc) restore->last = sequence;
    return rc;
}

int
snag_binary_producer_restore(struct snag_binary_producer *out, int fd,
    const struct snag_binary_anchor *through, const struct snag_binary_checkpoint_index *access,
    const struct snag_binary_checkpoint_sources *sources, const struct snag_session *state,
    bool (*cancelled)(void *), void *opaque)
{
    if (!out || fd < 0 || !through || !access || !sources || !state ||
        !through->next_seq || through->end < SNAG_BINARY_HEADER_SIZE ||
        through->turns >= through->next_seq ||
        through->next_seq != state->next_seq || through->turns != state->turn_count ||
        (!!sources->input != !!state->pending_input) ||
        sources->queue_count != state->pending_queue_count ||
        state->pending_queue_count > state->pending_queue_capacity ||
        (sources->queue_count && (!sources->queue || !state->pending_queue)))
        return snag_errno(EINVAL);
    struct snag_binary_producer staged = {.field = {.max = SNAG_MAX_EVENT_LINE}};
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_record record;
    struct snag_session *response = NULL;
    int rc = -1;
    if (sources->input) {
        if (restore_record(fd, through, access, sources->input, &scratch,
                &record, cancelled, opaque) < 0) goto done;
        if (record.kind != SNAG_BINARY_INPUT_RECEIVED && record.kind != SNAG_BINARY_IRC_ADMITTED) {
            errno = EINVAL;
            goto done;
        }
        if (remember_input(&staged, state, sources->input, &record) < 0) goto done;
        if (!staged.input.creation) { errno = EINVAL; goto done; }
    }
    for (size_t i = 0u; i < sources->queue_count; ++i) {
        const struct snag_binary_checkpoint_queue_source *source = &sources->queue[i];
        if (source->creation != state->pending_queue[i].seq ||
            !source->text || source->text < source->creation) { errno = EINVAL; goto done; }
        if (restore_record(fd, through, access, source->creation, &scratch,
                &record, cancelled, opaque) < 0) goto done;
        if (record.kind != SNAG_BINARY_FUTURE_TURN_QUEUED) { errno = EINVAL; goto done; }
        struct snag_session view = *state;
        view.pending_queue_count = i + 1u;
        if (remember_input(&staged, &view, source->creation, &record) < 0) goto done;
        if (!id_equal(staged.queue[i].id, state->pending_queue[i].queue_id)) {
            errno = EINVAL;
            goto done;
        }
        if (source->text != source->creation) {
            if (restore_record(fd, through, access, source->text, &scratch,
                    &record, cancelled, opaque) < 0) goto done;
            if (record.kind != SNAG_BINARY_FUTURE_TURN_EDITED) { errno = EINVAL; goto done; }
            if (remember_input(&staged, &view, source->text, &record) < 0) goto done;
            if (staged.queue[i].text.target.sequence != source->text) {
                errno = EINVAL;
                goto done;
            }
        }
    }
    if (state->response_open) {
        if (!sources->response_start || sources->response_end >= through->next_seq ||
            (sources->response_end && sources->response_end <= sources->response_start)) {
            errno = EINVAL;
            goto done;
        }
        if (restore_record(fd, through, access, sources->response_start, &scratch,
                &record, cancelled, opaque) < 0) goto done;
        struct snag_binary_event event;
        if (record.kind != SNAG_BINARY_RESPONSE_STARTED) { errno = EINVAL; goto done; }
        if (snag_binary_event_decode(&record, &event) < 0) goto done;
        const struct snag_binary_response_start *start = &event.data.response_started;
        if (!id_equal(start->turn, state->active_turn_id) ||
            !id_equal(start->response, state->active_response_id) ||
            start->cycle != state->active_cycle) { errno = EINVAL; goto done; }
        response = calloc(1u, sizeof(*response));
        if (!response) goto done;
        snag_session_init(response);
        response->response_open = true;
        response->active_cycle = state->active_cycle;
        memcpy(response->active_turn_id, state->active_turn_id, sizeof(response->active_turn_id));
        memcpy(response->active_response_id, state->active_response_id,
            sizeof(response->active_response_id));
        struct restore_response restore = {.producer = &staged, .state = response,
            .cancelled = cancelled, .opaque = opaque};
        if (sources->response_end && snag_binary_checkpoint_records_read(fd, through, access,
                sources->response_start + 1u, sources->response_end + 1u,
                restore_output, restore_cancelled, &restore) < 0) goto done;
        if (restore.last != sources->response_end ||
            ((response->response_public || state->response_public) &&
                !json_equal(response->response_public, state->response_public)) ||
            response->response_public_bytes != state->response_public_bytes) {
            errno = EINVAL;
            goto done;
        }
    }
    if (cancelled && cancelled(opaque)) { errno = ECANCELED; goto done; }
    snag_binary_producer_free(out);
    *out = staged;
    staged = (struct snag_binary_producer){0};
    rc = 0;
done:
    {
        int code = errno;
        if (response) snag_session_close(response);
        free(response);
        snag_buf_free(&scratch);
        snag_binary_producer_free(&staged);
        return rc ? snag_errno(code ? code : EINVAL) : 0;
    }
}
