/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_import.h"
#include "fs.h"
#include "store_binary_legacy.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct output_source {
    struct snag_binary_public_value value;
    size_t index;
};

struct input_source {
    uint64_t creation;
    unsigned char id[16];
    struct snag_binary_input_reference text, content, instructions;
    json_t *paths;
};

struct import_writer {
    int fd;
    int source_fd;
    struct snag_binary_anchor anchor;
    struct snag_binary_record *records;
    size_t count, capacity;
    struct snag_buf payload, field;
    struct snag_sha256 semantic;
    uint64_t source_end, turns;
    struct output_source *outputs;
    size_t output_count, output_capacity;
    json_t *public;
    struct input_source input, *queue;
    size_t queue_count, queue_capacity;
};

static void
hex_bytes(unsigned char *out, const char *hex, size_t size)
{
    const char *digits = "0123456789abcdef";
    for (size_t i = 0u; i < size; ++i) {
        unsigned high = (unsigned)(strchr(digits, hex[i * 2u]) - digits);
        unsigned low = (unsigned)(strchr(digits, hex[i * 2u + 1u]) - digits);
        out[i] = (unsigned char)(high * 16u + low);
    }
}

static int
semantic_event(void *opaque, const struct snag_session *state, uint64_t sequence,
    const char *type, const json_t *data, char *error, size_t error_size)
{
    if (!strcmp(type, "session_checkpoint")) return 0;
    char digest[SNAG_SHA256_HEX_LEN + 1u];
    json_t *logical = NULL;
    if (!strcmp(type, "voice_transfer_adopted")) {
        /* Each side validates its own physical cursor. Identity, logical start,
         * source coverage and count must remain exactly equal across formats. */
        logical = json_deep_copy(data);
        if (!logical) return snag_errno(ENOMEM);
        json_object_del(logical, "begin_offset");
        json_object_del(logical, "begin_sha256");
    }
    int rc = snag_json_digest(logical ? logical : data, digest);
    json_decref(logical);
    if (rc < 0) {
        return snag_fail(error, error_size, errno, "cannot digest imported semantic event");
    }
    unsigned char position[16];
    for (size_t i = 0u; i < 8u; ++i) {
        position[i] = (unsigned char)(sequence >> (8u * i));
        position[i + 8u] = (unsigned char)(state->last_time_ms >> (8u * i));
    }
    struct snag_sha256 *hash = opaque;
    snag_sha256_update(hash, position, sizeof(position));
    snag_sha256_update(hash, type, strlen(type) + 1u);
    snag_sha256_update(hash, digest, SNAG_SHA256_HEX_LEN);
    return 0;
}

static int
flush_batch(struct import_writer *writer)
{
    if (!writer->count) return 0;
    size_t offset = 0u;
    for (size_t i = 0u; i < writer->count; ++i) {
        writer->records[i].payload = writer->payload.data + offset;
        offset += writer->records[i].size;
    }
    struct snag_buf bytes = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_batch batch;
    struct snag_binary_anchor next;
    int rc = snag_binary_batch_encode(&bytes, &writer->anchor, writer->records,
        (uint32_t)writer->count, writer->turns);
    if (rc == 0) {
        rc = snag_binary_batch_decode(bytes.data, bytes.len, &writer->anchor, &batch, &next);
    }
    if (rc == 0) rc = snag_write_full(writer->fd, bytes.data, bytes.len);
    int code = errno;
    snag_buf_free(&bytes);
    if (rc == 0) {
        writer->anchor = next;
        writer->count = 0u;
        snag_buf_reset(&writer->payload);
    }
    errno = code;
    return rc;
}

static int
append_record(struct import_writer *writer, struct snag_binary_record record)
{
    size_t overhead = SNAG_BINARY_BATCH_HEADER_SIZE + SNAG_BINARY_BATCH_FOOTER_SIZE;
    size_t added = SNAG_BINARY_RECORD_HEADER_SIZE + writer->field.len;
    size_t used = overhead + writer->count * SNAG_BINARY_RECORD_HEADER_SIZE + writer->payload.len;
    if (writer->count && (used > SNAG_BINARY_BATCH_TARGET ||
        added > SNAG_BINARY_BATCH_TARGET - used) && flush_batch(writer) < 0) {
        return -1;
    }
    if (writer->count == writer->capacity) {
        size_t maximum = (SNAG_BINARY_BATCH_TARGET - overhead) / SNAG_BINARY_RECORD_HEADER_SIZE;
        size_t capacity = writer->capacity ? writer->capacity * 2u : 32u;
        if (capacity > maximum) capacity = maximum;
        if (capacity <= writer->count) return snag_errno(EOVERFLOW);
        struct snag_binary_record *grown = realloc(writer->records, capacity * sizeof(*grown));
        if (!grown) return -1;
        writer->records = grown;
        writer->capacity = capacity;
    }
    if (snag_buf_append(&writer->payload, writer->field.data, writer->field.len) < 0) {
        return -1;
    }
    record.size = writer->field.len;
    record.payload = NULL; /* Bound after the aggregate buffer stops growing. */
    writer->records[writer->count++] = record;
    return 0;
}

static int
replace_event(struct import_writer *writer, const struct snag_binary_event *event)
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
clear_input(struct input_source *source)
{
    json_decref(source->paths);
    *source = (struct input_source){0};
}

static struct snag_binary_input_reference
input_reference(const struct import_writer *writer, uint64_t sequence,
    enum snag_binary_input_leaf field, const unsigned char *bytes, size_t size)
{
    if (!bytes) return (struct snag_binary_input_reference){0};
    /* Decoded field views lie inside the existing bounded literal payload. */
    return (struct snag_binary_input_reference){.field = field,
        .target = {sequence, (uint32_t)(bytes - writer->field.data), (uint32_t)size}};
}

static int
remember_input(struct import_writer *writer, const struct snag_session *state,
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
            struct input_source *grown = realloc(writer->queue, capacity * sizeof(*grown));
            if (!grown) return -1;
            writer->queue = grown;
            writer->queue_capacity = capacity;
        }
        writer->queue[index] = (struct input_source){.creation = sequence};
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
prune_inputs(struct import_writer *writer, const struct snag_session *state, uint16_t kind)
{
    if (!state->pending_input) clear_input(&writer->input);
    if (kind != SNAG_BINARY_FUTURE_TURN_CANCELLED && kind != SNAG_BINARY_TURN_STARTED) return 0;
    size_t kept = 0u;
    for (size_t i = 0u; i < writer->queue_count; ++i) {
        if (kept < state->pending_queue_count &&
            writer->queue[i].creation == state->pending_queue[kept].seq) {
            if (i != kept) {
                writer->queue[kept] = writer->queue[i];
                writer->queue[i] = (struct input_source){0};
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
reference_turn(struct import_writer *writer, struct snag_binary_event *event, const json_t *data)
{
    struct snag_binary_turn_start *turn = &event->data.started;
    if (turn->origin == SNAG_BINARY_TURN_GOAL) return 0;
    const struct input_source *source = &writer->input;
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
clear_output(struct import_writer *writer)
{
    free(writer->outputs);
    writer->outputs = NULL;
    writer->output_count = writer->output_capacity = 0u;
    json_decref(writer->public);
    writer->public = NULL;
}

static int
remember_output(struct import_writer *writer, const struct snag_session *state,
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
            struct output_source *grown = realloc(writer->outputs, capacity * sizeof(*grown));
            if (!grown) return -1;
            writer->outputs = grown;
            writer->output_capacity = capacity;
        }
        size_t offset = (size_t)(output->item.text.data - writer->field.data);
        writer->outputs[count] = (struct output_source){.index = count,
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
    const struct output_source *a = left, *b = right;
    return memcmp(a->value.item.id, b->value.item.id, sizeof(a->value.item.id));
}

static void
reference_output(const struct import_writer *writer, struct snag_binary_public_value *value)
{
    struct output_source key = {.value.item = value->item};
    const struct output_source *source = bsearch(&key, writer->outputs, writer->output_count,
        sizeof(*writer->outputs), output_compare);
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
reference_snapshot(struct import_writer *writer, struct snag_binary_event *event)
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

static int
reference_voice(struct import_writer *writer, const struct snag_session *state,
    const struct snag_binary_record *record, char *error, size_t error_size)
{
    struct snag_binary_event event;
    if (snag_binary_event_decode(record, &event) < 0) return -1;
    struct snag_binary_voice_adopted *value = &event.data.voice_transfer_adopted;
    /* This prefix has already passed the strict legacy walker. A real line
     * boundary plus the exact next sequence and predecessor digest validates
     * the old cursor without another lifetime scan or a growing offset table. */
    if (value->begin_offset >= (uint64_t)state->log_end) return snag_errno(EINVAL);
    struct snag_session view = *state;
    view.log_fd = writer->source_fd;
    struct snag_journal_cursor cursor;
    if (snag_store_legacy_cursor_at(&view, (int64_t)value->begin_offset, &cursor,
        error, error_size) < 0) return -1;
    if (cursor.next_seq != value->begin_seq ||
        strcmp(cursor.prev_sha256, state->voice_history.begin.prev_sha256))
        return snag_errno(EINVAL);
    value->native = true;
    value->begin_offset = 0u;
    memset(value->begin_sha256, 0, sizeof(value->begin_sha256));
    return replace_event(writer, &event);
}

static int
reference_result(struct import_writer *writer, const struct snag_session *state,
    const struct snag_binary_record *record, char *error, size_t error_size)
{
    struct snag_binary_event event;
    if (snag_binary_event_decode(record, &event) < 0) return -1;
    struct snag_binary_tool_result *result = event.kind == SNAG_BINARY_TOOL_FINISHED ?
        &event.data.tool_finished.result : &event.data.process_closed.result;
    if (!result->has_output_ref) return 0;
    struct snag_binary_tool_output_ref *ref = &result->output_ref;
    if (ref->log_end > writer->source_end) return snag_errno(EINVAL);
    struct snag_session view = *state;
    view.log_fd = writer->source_fd;
    if (ref->log_end) {
        struct snag_journal_cursor first, end;
        if (snag_store_legacy_cursor_at(&view, (int64_t)ref->log_start, &first,
                error, error_size) < 0 ||
            snag_store_legacy_cursor_at(&view, (int64_t)ref->log_end, &end,
                error, error_size) < 0) return -1;
        ref->first_sequence = first.next_seq;
        ref->end_sequence = end.next_seq;
    }
    ref->native = true;
    return replace_event(writer, &event);
}

static int
import_event(void *opaque, const struct snag_session *state, uint64_t sequence,
    const char *type, const json_t *data, char *error, size_t error_size)
{
    struct import_writer *writer = opaque;
    if (sequence == 1u) {
        struct snag_binary_identity identity = {.created_ms = state->last_time_ms};
        hex_bytes(identity.id, state->id, sizeof(identity.id));
        unsigned char header[SNAG_BINARY_HEADER_SIZE];
        snag_binary_header_encode(header, &identity);
        if (snag_binary_header_decode(header, sizeof(header), &identity, &writer->anchor) < 0 ||
            snag_write_full(writer->fd, header, sizeof(header)) < 0) {
            goto fail;
        }
    }
    snag_buf_reset(&writer->field);
    struct snag_binary_record record = {.version = 1u, .timestamp_ms = state->last_time_ms};
    if (!strcmp(type, "session_checkpoint")) {
        struct snag_binary_legacy_checkpoint checkpoint = {
            .start = writer->source_end, .end = (uint64_t)state->log_end};
        hex_bytes(checkpoint.digest, state->prev_sha256, sizeof(checkpoint.digest));
        if (snag_binary_legacy_checkpoint_encode(&writer->field, &checkpoint) < 0) {
            goto fail;
        }
        record.kind = SNAG_BINARY_LEGACY_CHECKPOINT;
        record.flags = SNAG_BINARY_RECORD_OPTIONAL;
    } else {
        enum snag_binary_kind kind;
        if (snag_binary_legacy_encode(&writer->field, type, data, &kind) < 0) goto fail;
        record.kind = (uint16_t)kind;
        record.version = snag_binary_event_version(kind);
        record.payload = writer->field.data;
        record.size = writer->field.len;
        if (kind == SNAG_BINARY_VOICE_TRANSFER_ADOPTED &&
            reference_voice(writer, state, &record, error, error_size) < 0) goto fail;
        if ((kind == SNAG_BINARY_TOOL_FINISHED || kind == SNAG_BINARY_PROCESS_CLOSED) &&
            reference_result(writer, state, &record, error, error_size) < 0) goto fail;
        if (remember_input(writer, state, sequence, &record) < 0) goto fail;
        if (kind == SNAG_BINARY_TURN_STARTED ||
            (kind >= SNAG_BINARY_RESPONSE_OUTPUT && kind <= SNAG_BINARY_RESPONSE_COMPLETED)) {
            struct snag_binary_event event;
            if (snag_binary_event_decode(&record, &event) < 0) goto fail;
            if (kind == SNAG_BINARY_TURN_STARTED) {
                if (reference_turn(writer, &event, data) < 0) goto fail;
            } else if (kind == SNAG_BINARY_RESPONSE_OUTPUT) {
                if (remember_output(writer, state, sequence, &event.data.response_output) < 0) {
                    goto fail;
                }
            } else if (reference_snapshot(writer, &event) < 0) {
                goto fail;
            }
        }
    }
    if (sequence != writer->anchor.next_seq + writer->count ||
        state->log_end <= 0 || (uint64_t)state->log_end <= writer->source_end) {
        errno = EINVAL;
        goto fail;
    }
    /* Flush uses the preceding event's turn count. Update only after append. */
    if (append_record(writer, record) < 0) goto fail;
    if (prune_inputs(writer, state, record.kind) < 0) goto fail;
    writer->source_end = (uint64_t)state->log_end;
    writer->turns = state->turn_count;
    if (!state->response_open) clear_output(writer);
    return semantic_event(&writer->semantic, state, sequence, type, data, error, error_size);
fail:
    return snag_fail(error, error_size, errno, "cannot stage legacy record %llu",
        (unsigned long long)sequence);
}

static int
compare_core(const struct snag_session *legacy, const struct snag_session *native)
{
    json_t *left = snag_checkpoint_state_encode(legacy);
    json_t *right = snag_checkpoint_state_encode(native);
    int rc = -1;
    if (!left || !right) {
        errno = ENOMEM;
        goto out;
    }
    const char *coordinates[] = {"prev_sha256", "log_end", "checkpoint_offset", "checkpoint_seq"};
    for (size_t i = 0u; i < sizeof(coordinates) / sizeof(coordinates[0]); ++i) {
        json_object_del(left, coordinates[i]);
        json_object_del(right, coordinates[i]);
    }
    json_t *old_voice = json_object_get(left, "voice_history");
    json_t *new_voice = json_object_get(right, "voice_history");
    json_object_del(old_voice, "begin.offset");
    json_object_del(new_voice, "begin.offset");
    json_object_del(old_voice, "begin.prev_sha256");
    json_object_del(new_voice, "begin.prev_sha256");
    if (!json_equal(left, right) || legacy->next_seq != native->next_seq ||
        legacy->last_time_ms != native->last_time_ms) {
        errno = EINVAL;
    } else {
        rc = 0;
    }
out:
    json_decref(left);
    json_decref(right);
    return rc;
}

int
snag_store_import_binary_journal(struct snag_session *source, int destination,
    struct snag_session *restored, struct snag_binary_import_result *result,
    char *error, size_t error_size)
{
    if (!source || !restored || source == restored || !result || destination < 0 ||
        source->log_fd < 0 || source->lock_fd < 0 || source->pending_log ||
        restored->dir_fd >= 0 || restored->log_fd >= 0 || restored->lock_fd >= 0 ||
        restored->pending_log || !snag_hex_is_lower(source->id, SNAG_ID_HEX_LEN)) {
        return snag_fail(error, error_size, EINVAL, "invalid locked journal staging arguments");
    }
    *result = (struct snag_binary_import_result){0};
    snag_file_info before, after, output, lock;
    if (snag_fstat(source->log_fd, &before) < 0 || !S_ISREG(before.st_mode) || before.st_size < 0 ||
        snag_fstat(source->lock_fd, &lock) < 0 || snag_fstat(destination, &output) < 0 ||
        !S_ISREG(output.st_mode) || output.st_size != 0 ||
        (before.st_dev == output.st_dev && before.st_ino == output.st_ino) ||
        (lock.st_dev == output.st_dev && lock.st_ino == output.st_ino)) {
        return snag_fail(error, error_size, EINVAL,
            "staging requires a separate empty regular file");
    }
    if (snag_seek(destination, 0, SEEK_SET) < 0) {
        return snag_fail(error, error_size, errno, "cannot position journal staging file");
    }
    struct snag_binary_checkpoint_sources sources = {0};
    struct snag_session legacy, native, staged;
    snag_session_init(&legacy);
    snag_session_init(&native);
    snag_session_init(&staged);
    memcpy(staged.id, source->id, sizeof(staged.id));
    /* Borrow descriptors only for the read-only verifier; never close them. */
    staged.log_fd = destination;
    staged.lock_fd = source->lock_fd;
    struct import_writer writer = {.fd = destination, .source_fd = source->log_fd,
        .payload = {.max = SNAG_MAX_EVENT_LINE}, .field = {.max = SNAG_MAX_EVENT_LINE}};
    snag_sha256_init(&writer.semantic);
    int rc = snag_store_reconcile_legacy(source, &legacy, import_event, &writer,
        &result->legacy, error, error_size);
    if (rc < 0) goto out;
    if (flush_batch(&writer) < 0) {
        rc = snag_fail(error, error_size, errno, "cannot finish staged native journal");
        goto out;
    }
    snag_sha256_final(&writer.semantic, result->semantic_digest);
    memcpy(result->source_sha256, legacy.prev_sha256, sizeof(result->source_sha256));
    struct snag_sha256 semantic;
    snag_sha256_init(&semantic);
    rc = snag_store_reconcile_binary(&staged, &native, semantic_event, &semantic,
        &result->native, &sources, error, error_size);
    if (rc < 0) goto out;
    unsigned char digest[32];
    snag_sha256_final(&semantic, digest);
    if (result->native.incomplete_tail_bytes ||
        memcmp(digest, result->semantic_digest, sizeof(digest))) {
        rc = snag_fail(error, error_size, EINVAL,
            "staged native journal differs from legacy source");
        goto out;
    }
    if (compare_core(&legacy, &native) < 0) {
        rc = snag_fail(error, error_size, errno, "cannot verify staged native core state");
        goto out;
    }
    if (snag_fstat(source->log_fd, &after) < 0 || !snag_file_unchanged(&before, &after)) {
        rc = snag_fail(error, error_size, EAGAIN,
            "legacy source changed during staging verification");
        goto out;
    }
    snag_session_close(restored);
    *restored = native;
    result->sources = sources;
    sources = (struct snag_binary_checkpoint_sources){0};
    snag_session_init(&native);
out:
    {
        int code = errno;
        snag_binary_checkpoint_sources_free(&sources);
        clear_input(&writer.input);
        for (size_t i = 0u; i < writer.queue_count; ++i) clear_input(&writer.queue[i]);
        free(writer.queue);
        clear_output(&writer);
        snag_session_close(&native);
        snag_session_close(&legacy);
        free(writer.records);
        snag_buf_free(&writer.payload);
        snag_buf_free(&writer.field);
        errno = code;
    }
    return rc;
}
