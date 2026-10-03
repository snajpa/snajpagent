/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_replay.h"
#include "fs.h"
#include "store_binary_legacy.h"
#include "store_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Queue creation determines order and content. Edits replace text without
 * changing that creation sequence or content. Keep original field provenance
 * alongside the provisional reducer state, never inferred from equal bytes. */
struct replay_context {
    int fd;
    snag_session_event_fn fn;
    void *opaque;
    uint64_t sequence;
    uint64_t response_sequence;
    struct snag_binary_anchor through;
    struct snag_binary_checkpoint_sources sources;
    size_t process_capacity;
    size_t queue_capacity, download_capacity;
};

static void
bytes_hex(char *out, const unsigned char *bytes, size_t size)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0u; i < size; ++i) {
        out[i * 2u] = hex[bytes[i] >> 4u];
        out[i * 2u + 1u] = hex[bytes[i] & 15u];
    }
    out[size * 2u] = '\0';
}

static int
read_identity(int fd, uint64_t boundary, struct snag_binary_identity *identity,
    struct snag_binary_anchor *anchor)
{
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    if (boundary < sizeof(header)) return snag_errno(EINVAL);
    size_t position = 0u;
    while (position < sizeof(header)) {
        ssize_t got = snag_pread(fd, header + position, sizeof(header) - position,
            (int64_t)position);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) return -1;
        if (!got) return snag_errno(EIO);
        position += (size_t)got;
    }
    return snag_binary_header_decode(header, sizeof(header), identity, anchor);
}

static int
update_process_sources(struct replay_context *context, const struct snag_session *state,
    enum snag_binary_kind kind)
{
    struct snag_binary_checkpoint_sources *sources = &context->sources;
    size_t old_count = sources->process_count;
    if (state->process_count > old_count) {
        if (old_count == SIZE_MAX || state->process_count != old_count + 1u ||
            kind != SNAG_BINARY_TOOL_STARTED || !sources->calls.graph ||
            sources->calls.graph >= context->sequence) return snag_errno(EINVAL);
        if (state->process_count > context->process_capacity) {
            size_t capacity = state->process_capacity;
            if (capacity < state->process_count ||
                capacity > SIZE_MAX / sizeof(*sources->processes))
                return snag_errno(EOVERFLOW);
            void *grown = realloc(sources->processes, capacity * sizeof(*sources->processes));
            if (!grown) return -1;
            sources->processes = grown;
            context->process_capacity = capacity;
        }
        struct snag_binary_checkpoint_process_source *added = &sources->processes[old_count];
        *added = (struct snag_binary_checkpoint_process_source){
            .started = context->sequence, .call = sources->calls};
        memcpy(added->handle, state->processes[old_count].handle, sizeof(added->handle));
        ++old_count;
    }
    size_t previous = 0u;
    for (size_t i = 0u; i < state->process_count; ++i) {
        while (previous < old_count &&
            strcmp(sources->processes[previous].handle, state->processes[i].handle)) ++previous;
        if (previous == old_count) return snag_errno(EINVAL);
        sources->processes[i] = sources->processes[previous++];
    }
    sources->process_count = state->process_count;
    return 0;
}

static int
update_download_sources(struct replay_context *context, const struct snag_session *state,
    enum snag_binary_kind kind)
{
    struct snag_binary_checkpoint_sources *sources = &context->sources;
    if (kind != SNAG_BINARY_DOWNLOAD_QUEUED && kind != SNAG_BINARY_DOWNLOAD_REMOVED &&
        kind != SNAG_BINARY_DOWNLOADS_CLEARED) return 0;
    size_t count = json_array_size(state->download_queue), previous = 0u;
    size_t old_count = sources->download_count;
    if (kind == SNAG_BINARY_DOWNLOAD_QUEUED) {
        if (old_count == SIZE_MAX || count != old_count + 1u) return snag_errno(EINVAL);
        if (count > context->download_capacity) {
            size_t capacity = context->download_capacity;
            if (capacity > SIZE_MAX / 2u) return snag_errno(EOVERFLOW);
            capacity = capacity ? capacity * 2u : 8u;
            if (capacity > SIZE_MAX / sizeof(*sources->downloads)) return snag_errno(EOVERFLOW);
            void *grown = realloc(sources->downloads, capacity * sizeof(*sources->downloads));
            if (!grown) return -1;
            sources->downloads = grown;
            context->download_capacity = capacity;
        }
        const char *id = snag_json_string(json_array_get(state->download_queue, old_count), "id");
        if (!id || !snag_hex_is_lower(id, SNAG_ID_HEX_LEN)) return snag_errno(EINVAL);
        sources->downloads[old_count].receipt = context->sequence;
        memcpy(sources->downloads[old_count].id, id, sizeof(sources->downloads[old_count].id));
        ++old_count;
    }
    for (size_t i = 0u; i < count; ++i) {
        const char *id = snag_json_string(json_array_get(state->download_queue, i), "id");
        if (!id) return snag_errno(EINVAL);
        while (previous < old_count && strcmp(id, sources->downloads[previous].id)) ++previous;
        if (previous == old_count) return snag_errno(EINVAL);
        sources->downloads[i] = sources->downloads[previous++];
    }
    sources->download_count = count;
    return 0;
}

static int
update_sources(struct replay_context *context, const struct snag_session *state,
    enum snag_binary_kind kind, const json_t *data)
{
    struct snag_binary_checkpoint_sources *sources = &context->sources;
    if (kind == SNAG_BINARY_SESSION_OPTIONS) sources->resume_options = context->sequence;
    if (!json_object_get(state->strings, "resume_options")) sources->resume_options = 0u;
    if (kind == SNAG_BINARY_RESPONSE_STARTED) {
        context->response_sequence = context->sequence;
        sources->response_start = context->sequence;
    }
    if (!state->response_open) context->response_sequence = 0u;
    if (!state->active_response_id[0]) sources->response_start = 0u;
    if (kind == SNAG_BINARY_RESPONSE_OUTPUT) sources->response_end = context->sequence;
    if (!state->response_public) sources->response_end = 0u;
    if (kind == SNAG_BINARY_COMPACTION_STARTED) sources->active_compact = context->sequence;
    if (kind == SNAG_BINARY_COMPACTION_COMPLETED) {
        if (!sources->active_compact) return snag_errno(EINVAL);
        sources->compact_start = sources->active_compact;
        sources->compact_end = context->sequence;
    }
    if (!state->active_compact_id[0]) sources->active_compact = 0u;
    if (update_download_sources(context, state, kind) < 0) return -1;
    if (kind == SNAG_BINARY_INPUT_RECEIVED ||
        (kind == SNAG_BINARY_IRC_ADMITTED && json_object_get(data, "input"))) {
        sources->input = context->sequence;
    }
    if (!state->pending_input) sources->input = 0u;

    if (kind == SNAG_BINARY_FUTURE_TURN_QUEUED) {
        if (sources->queue_count == SIZE_MAX ||
            state->pending_queue_count != sources->queue_count + 1u ||
            state->pending_queue[sources->queue_count].seq != context->sequence) {
            return snag_errno(EINVAL);
        }
        if (sources->queue_count == context->queue_capacity) {
            size_t capacity = state->pending_queue_capacity;
            if (capacity <= sources->queue_count || capacity > SIZE_MAX / sizeof(*sources->queue))
                return snag_errno(EOVERFLOW);
            struct snag_binary_checkpoint_queue_source *grown =
                realloc(sources->queue, capacity * sizeof(*grown));
            if (!grown) return -1;
            sources->queue = grown;
            context->queue_capacity = capacity;
        }
        sources->queue[sources->queue_count++] = (struct snag_binary_checkpoint_queue_source){
            .creation = context->sequence, .text = context->sequence};
    } else if (kind == SNAG_BINARY_FUTURE_TURN_EDITED) {
        const char *id = snag_json_string(data, "queue_id");
        if (!id || sources->queue_count != state->pending_queue_count) return snag_errno(EINVAL);
        for (size_t i = 0u; i < sources->queue_count; ++i) {
            if (strcmp(id, state->pending_queue[i].queue_id)) continue;
            if (sources->queue[i].creation != state->pending_queue[i].seq) {
                return snag_errno(EINVAL);
            }
            sources->queue[i].text = context->sequence;
            return 0;
        }
        return snag_errno(EINVAL);
    } else if (kind == SNAG_BINARY_FUTURE_TURN_CANCELLED || kind == SNAG_BINARY_TURN_STARTED) {
        /* Cancellation preserves order; a queued turn consumes the first item.
         * Non-queued turns leave this list alone. No completed history is kept. */
        size_t previous = 0u;
        for (size_t i = 0u; i < state->pending_queue_count; ++i) {
            uint64_t creation = state->pending_queue[i].seq;
            while (previous < sources->queue_count && sources->queue[previous].creation < creation)
                ++previous;
            if (previous == sources->queue_count || sources->queue[previous].creation != creation)
                return snag_errno(EINVAL);
            sources->queue[i] = sources->queue[previous++];
        }
        sources->queue_count = state->pending_queue_count;
    }
    return 0;
}

/* The same three field shapes occur in distinct receipt/turn payloads. Keep
 * their typed views and declarations together while hydrating temporary values. */
struct input_fields {
    struct snag_binary_text *text;
    struct snag_binary_content *content;
    struct snag_binary_instructions *instructions;
    struct snag_binary_input_reference *text_ref, *content_ref, *instructions_ref;
};

static struct input_fields
input_fields(struct snag_binary_event *event)
{
    switch (event->kind) {
    case SNAG_BINARY_INPUT_RECEIVED:
        return (struct input_fields){
            &event->data.input.text, &event->data.input.content, &event->data.input.instructions,
            &event->data.input.text_ref, &event->data.input.content_ref,
            &event->data.input.instructions_ref};
    case SNAG_BINARY_FUTURE_TURN_QUEUED:
    case SNAG_BINARY_FUTURE_TURN_EDITED:
        return (struct input_fields){
            &event->data.queued.text, &event->data.queued.content, NULL,
            &event->data.queued.text_ref, &event->data.queued.content_ref, NULL};
    case SNAG_BINARY_STEERING_ADDED:
    case SNAG_BINARY_IRC_REPLY_REMINDER:
        return (struct input_fields){
            &event->data.steering_input.text, &event->data.steering_input.content, NULL,
            &event->data.steering_input.text_ref, &event->data.steering_input.content_ref, NULL};
    case SNAG_BINARY_TURN_STARTED:
        return (struct input_fields){
            &event->data.started.text, &event->data.started.content,
            &event->data.started.instructions, &event->data.started.text_ref,
            &event->data.started.content_ref, &event->data.started.instructions_ref};
    default:
        return (struct input_fields){0};
    }
}

static int
turn_field_matches(const struct replay_context *context,
    const struct snag_binary_turn_start *turn, const struct snag_binary_input_reference *reference,
    enum snag_binary_input_leaf field)
{
    bool queued = turn->origin == SNAG_BINARY_TURN_QUEUED;
    uint64_t wanted = context->sources.input;
    if (queued) {
        const struct snag_binary_checkpoint_queue_source *origin = &context->sources.queue[0];
        wanted = field == SNAG_BINARY_INPUT_TEXT ? origin->text : origin->creation;
        if (wanted < origin->creation) return snag_errno(EINVAL);
    }
    if (!wanted || wanted >= context->sequence) return snag_errno(EINVAL);
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_batch batch;
    struct snag_binary_anchor before;
    int rc = -1;
    if (snag_binary_batch_find(context->fd, &context->through, wanted,
            &scratch, &batch, &before) < 0) goto done;
    size_t cursor = SNAG_BINARY_BATCH_HEADER_SIZE;
    struct snag_binary_record record;
    uint64_t sequence;
    while ((rc = snag_binary_record_next(&batch, &cursor, &record, &sequence)) == 0) {
        if (sequence != wanted) continue;
        struct snag_binary_event source;
        rc = snag_errno(EINVAL);
        if (record.flags || snag_binary_event_decode(&record, &source) < 0) goto done;
        if (queued) {
            enum snag_binary_kind kind = wanted == context->sources.queue[0].creation ?
                SNAG_BINARY_FUTURE_TURN_QUEUED : SNAG_BINARY_FUTURE_TURN_EDITED;
            if (record.kind != kind ||
                memcmp(source.data.queued.id, turn->queue_id, sizeof(turn->queue_id))) goto done;
        } else if (record.kind == SNAG_BINARY_IRC_ADMITTED) {
            struct snag_binary_record input = source.data.irc_admitted.input;
            if (input.kind != SNAG_BINARY_INPUT_RECEIVED ||
                snag_binary_event_decode(&input, &source) < 0) goto done;
        } else if (record.kind != SNAG_BINARY_INPUT_RECEIVED) {
            goto done;
        }
        struct input_fields fields = input_fields(&source);
        const struct snag_binary_input_reference *declared = field == SNAG_BINARY_INPUT_TEXT ?
            fields.text_ref : field == SNAG_BINARY_INPUT_CONTENT ?
            fields.content_ref : fields.instructions_ref;
        if (!declared) goto done;
        struct snag_binary_input_reference canonical = *declared;
        if (declared->field) {
            /* The current receipt may declare an older original, but cannot
             * introduce a chain or acquire its identity from equal bytes. */
            if (!canonical.target.sequence || canonical.target.sequence >= wanted) goto done;
        } else {
            canonical.field = field;
            /* Embedded literal offsets remain relative to the outer record. */
            if (snag_binary_input_ref_create(&batch, wanted, field, &canonical.target) < 0)
                goto done;
        }
        rc = reference->field == canonical.field &&
            reference->target.sequence == canonical.target.sequence &&
            reference->target.offset == canonical.target.offset &&
            reference->target.size == canonical.target.size ? 0 : snag_errno(EINVAL);
        goto done;
    }
    if (rc == 1) rc = snag_errno(EINVAL);
 done:
    snag_buf_free(&scratch);
    return rc;
}

static int
read_input_field(const struct replay_context *context,
    const struct snag_binary_turn_start *turn, const struct snag_binary_input_reference *reference,
    enum snag_binary_input_leaf field, struct snag_buf *scratch, const unsigned char **view)
{
    if (!reference->target.sequence || reference->target.sequence >= context->sequence)
        return snag_errno(EINVAL);
    /* Queued receipts carry no instruction list; that declaration belongs to
     * the current turn, while its text/content still bind to queue receipts. */
    bool declared_by_turn = turn && (turn->origin == SNAG_BINARY_TURN_GOAL ||
        (turn->origin == SNAG_BINARY_TURN_QUEUED && field == SNAG_BINARY_INPUT_INSTRUCTIONS));
    if (turn && !declared_by_turn &&
        turn_field_matches(context, turn, reference, field) < 0) {
        return -1;
    }
    struct snag_binary_batch batch;
    struct snag_binary_anchor before;
    if (snag_binary_batch_find(context->fd, &context->through, reference->target.sequence,
            scratch, &batch, &before) < 0) return -1;
    /* Text destinations may reuse compatible original text roles. The decoder
     * checks destination shape; this checks the exact original role.
     * A source field containing a reference has no literal view and fails. */
    return snag_binary_input_ref_resolve(&reference->target, &batch, reference->field, view);
}

static int
preflight_public(const struct replay_context *context,
    const struct snag_binary_public_value *value, size_t *bytes)
{
    bool referenced = value->source.first.sequence != 0u;
    if (referenced && (!context->response_sequence ||
        value->source.first.sequence <= context->response_sequence ||
        value->source.last_sequence >= context->sequence)) {
        return snag_errno(EINVAL);
    }
    /* Materialization must fit the existing literal-record projection.
     * Check fanout before retaining assembled text for individual items. */
    size_t size = referenced ? value->source.bytes : value->item.text.size;
    if (size > SNAG_MAX_EVENT_LINE - *bytes) return snag_errno(EFBIG);
    *bytes += size;
    return 0;
}

static int
resolve_public(const struct replay_context *context, const unsigned char turn[16],
    const unsigned char response[16], uint32_t cycle, struct snag_binary_public_value *item,
    struct snag_buf *scratch, struct snag_buf *text)
{
    if (!item->source.first.sequence) return 0;
    struct snag_binary_batch batch;
    struct snag_binary_anchor before;
    text->max = SNAG_MAX_EVENT_LINE;
    if (snag_binary_batch_find(context->fd, &context->through, item->source.first.sequence,
            scratch, &batch, &before) < 0 ||
        snag_binary_output_span_resolve(context->fd, context->through.end, &before,
            &item->source, turn, response, cycle, &item->item, text) < 0) {
        return -1;
    }
    item->item.text = (struct snag_binary_text){text->data, text->len};
    item->source = (struct snag_binary_output_span){0};
    return 0;
}

struct partial_fields {
    const unsigned char *turn, *response;
    uint32_t cycle;
    struct snag_binary_public_items *items;
};

static struct partial_fields
partial_fields(struct snag_binary_event *event)
{
    switch (event->kind) {
    case SNAG_BINARY_RESPONSE_INTERRUPTED:
        return (struct partial_fields){event->data.response_interrupted.turn,
            event->data.response_interrupted.response, event->data.response_interrupted.cycle,
            &event->data.response_interrupted.partial};
    case SNAG_BINARY_RESPONSE_FAILED:
        return (struct partial_fields){event->data.response_failed.turn,
            event->data.response_failed.response, event->data.response_failed.cycle,
            &event->data.response_failed.partial};
    case SNAG_BINARY_RESPONSE_OUTPUT_CORRECTION:
        return (struct partial_fields){event->data.response_correction.turn,
            event->data.response_correction.response, event->data.response_correction.cycle,
            &event->data.response_correction.partial};
    default:
        return (struct partial_fields){0};
    }
}

static int
project_partial(const struct replay_context *context, const struct snag_binary_record *record,
    const char **type, json_t **data)
{
    struct snag_binary_event event;
    if (snag_binary_event_decode(record, &event) < 0) return -1;
    struct partial_fields fields = partial_fields(&event);
    size_t offset = 0u, count = 0u, bytes = 0u;
    bool references = false;
    struct snag_binary_public_value value;
    int rc;
    while ((rc = snag_binary_public_items_next(fields.items, &offset, &value)) == 0) {
        if (preflight_public(context, &value, &bytes) < 0) return -1;
        references |= value.source.first.sequence != 0u;
        ++count;
    }
    if (rc < 0) return -1;
    if (!references) {
        return snag_binary_legacy_decode(record, type, data);
    }

    struct snag_binary_public_value *values = calloc(count, sizeof(*values));
    struct snag_buf *texts = calloc(count, sizeof(*texts));
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_buf items = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf literal = {.max = SNAG_MAX_EVENT_LINE};
    rc = -1;
    if (!values || !texts) goto done;
    offset = 0u;
    for (size_t i = 0u; i < count; ++i) {
        if (snag_binary_public_items_next(fields.items, &offset, &values[i]) != 0) {
            goto done;
        }
        if (resolve_public(context, fields.turn, fields.response, fields.cycle,
            &values[i], &scratch, &texts[i]) < 0) {
            goto done;
        }
    }
    if (snag_binary_public_items_encode(&items, values, count) < 0) goto done;
    *fields.items = (struct snag_binary_public_items){items.data, items.len};
    if (snag_binary_event_encode(&literal, &event) < 0) goto done;
    struct snag_binary_record resolved = *record;
    resolved.payload = literal.data;
    resolved.size = literal.len;
    resolved.version = snag_binary_event_version(event.kind);
    rc = snag_binary_legacy_decode(&resolved, type, data);
done:
    snag_buf_free(&literal);
    snag_buf_free(&items);
    snag_buf_free(&scratch);
    if (texts) {
        for (size_t i = 0u; i < count; ++i) snag_buf_free(&texts[i]);
    }
    free(texts);
    free(values);
    return rc;
}

static int
project_graph(const struct replay_context *context, const struct snag_binary_record *record,
    const char **type, json_t **data)
{
    struct snag_binary_event event;
    if (snag_binary_event_decode(record, &event) < 0) return -1;
    struct snag_binary_response_complete *fields = &event.data.response_completed;
    size_t offset = 0u, count = 0u, bytes = 0u;
    bool references = false;
    struct snag_binary_graph_item value;
    int rc;
    while ((rc = snag_binary_graph_items_next(&fields->items, &offset, &value)) == 0) {
        if (value.kind == SNAG_BINARY_ITEM_TOOL_CALL) {
            size_t size = value.data.call.arguments.size;
            if (size > SNAG_MAX_EVENT_LINE - bytes) return snag_errno(EFBIG);
            bytes += size;
        } else {
            if (preflight_public(context, &value.data.output, &bytes) < 0) {
                return -1;
            }
            references |= value.data.output.source.first.sequence != 0u;
        }
        ++count;
    }
    if (rc < 0) return -1;
    if (!references) {
        return snag_binary_legacy_decode(record, type, data);
    }

    struct snag_binary_graph_item *values = calloc(count, sizeof(*values));
    struct snag_buf *texts = calloc(count, sizeof(*texts));
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_buf items = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf literal = {.max = SNAG_MAX_EVENT_LINE};
    rc = -1;
    if (!values || !texts) goto done;
    offset = 0u;
    for (size_t i = 0u; i < count; ++i) {
        if (snag_binary_graph_items_next(&fields->items, &offset, &values[i]) != 0) {
            goto done;
        }
        if (values[i].kind != SNAG_BINARY_ITEM_TOOL_CALL &&
            resolve_public(context, fields->turn, fields->response, fields->cycle,
                &values[i].data.output, &scratch, &texts[i]) < 0) {
            goto done;
        }
    }
    if (snag_binary_graph_items_encode(&items, values, count) < 0 ||
        snag_binary_graph_items_decode(items.data, items.len, &fields->items) < 0 ||
        snag_binary_event_encode(&literal, &event) < 0) {
        goto done;
    }
    struct snag_binary_record resolved = *record;
    resolved.payload = literal.data;
    resolved.size = literal.len;
    resolved.version = snag_binary_event_version(event.kind);
    rc = snag_binary_legacy_decode(&resolved, type, data);
done:
    snag_buf_free(&literal);
    snag_buf_free(&items);
    snag_buf_free(&scratch);
    if (texts) {
        for (size_t i = 0u; i < count; ++i) snag_buf_free(&texts[i]);
    }
    free(texts);
    free(values);
    return rc;
}

static int
resolve_voice(const struct replay_context *context, const struct snag_binary_record *record,
    struct snag_binary_event *event)
{
    if (snag_binary_event_decode(record, event) < 0) return -1;
    struct snag_binary_voice_adopted *value = &event->data.voice_transfer_adopted;
    if (!value->native) return snag_errno(ENOTSUP);
    if (value->begin_seq >= context->sequence ||
        value->transfer.count > context->sequence - value->begin_seq) return snag_errno(EINVAL);
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_batch batch;
    struct snag_binary_anchor before;
    int rc = snag_binary_batch_find(context->fd, &context->through, value->begin_seq,
        &scratch, &batch, &before);
    if (!rc) {
        /* The logical start can be inside a batch. The native cursor resumes
         * from its authenticated predecessor and skips earlier records. */
        value->begin_offset = before.end;
        memcpy(value->begin_sha256, before.digest, sizeof(value->begin_sha256));
        value->native = false;
    }
    snag_buf_free(&scratch);
    return rc;
}

int
snag_binary_checkpoint_voice_read(int fd, const struct snag_binary_anchor *anchor,
    uint64_t sequence, const char *id, struct snag_voice_history_root *out)
{
    if (!anchor || !out || !snag_hex_is_lower(id, SNAG_ID_HEX_LEN) ||
        sequence < 2u || sequence >= anchor->next_seq) return snag_errno(EINVAL);
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_batch batch;
    struct snag_binary_anchor before;
    struct snag_binary_record record;
    struct snag_binary_event event;
    size_t offset = SNAG_BINARY_BATCH_HEADER_SIZE;
    uint64_t found;
    int rc = snag_binary_batch_find(fd, anchor, sequence, &scratch, &batch, &before);
    if (rc < 0) goto done;
    while ((rc = snag_binary_record_next(&batch, &offset, &record, &found)) == 0) {
        if (found == sequence) break;
    }
    if (rc || record.kind != SNAG_BINARY_VOICE_TRANSFER_ADOPTED) {
        rc = snag_errno(EINVAL);
        goto done;
    }
    struct replay_context context = {.fd = fd, .through = *anchor, .sequence = sequence};
    rc = resolve_voice(&context, &record, &event);
    if (rc < 0) goto done;
    const struct snag_binary_voice_adopted *value = &event.data.voice_transfer_adopted;
    char target[SNAG_ID_HEX_LEN + 1u];
    bytes_hex(target, value->transfer.target, sizeof(value->transfer.target));
    if (strcmp(target, id)) {
        rc = snag_errno(EINVAL);
        goto done;
    }
    struct snag_voice_history_root root = {.adopted_seq = sequence,
        .begin = {.offset = (int64_t)value->begin_offset, .next_seq = value->begin_seq}};
    bytes_hex(root.transfer_id, value->transfer.id, sizeof(value->transfer.id));
    bytes_hex(root.begin.prev_sha256, value->begin_sha256, sizeof(value->begin_sha256));
    *out = root;
done:
    snag_buf_free(&scratch);
    return rc;
}

static int
project_voice(const struct replay_context *context, const struct snag_binary_record *record,
    const char **type, json_t **data)
{
    struct snag_binary_event event;
    if (resolve_voice(context, record, &event) < 0) return -1;
    struct snag_buf literal = {.max = SNAG_MAX_EVENT_LINE};
    int rc = snag_binary_event_encode(&literal, &event);
    if (!rc) {
        struct snag_binary_record resolved = *record;
        resolved.payload = literal.data;
        resolved.size = literal.len;
        resolved.version = snag_binary_event_version(event.kind);
        rc = snag_binary_legacy_decode(&resolved, type, data);
    }
    snag_buf_free(&literal);
    return rc;
}

static int
project_record(const struct replay_context *context, const struct snag_session *state,
    const struct snag_binary_record *record, const char **type, json_t **data)
{
    if (record->kind == SNAG_BINARY_TOOL_FINISHED || record->kind == SNAG_BINARY_PROCESS_CLOSED) {
        struct snag_binary_event event;
        if (snag_binary_event_decode(record, &event) < 0) return -1;
        const struct snag_binary_tool_result *result = event.kind == SNAG_BINARY_TOOL_FINISHED ?
            &event.data.tool_finished.result : &event.data.process_closed.result;
        if (result->has_output_ref) {
            if (!result->output_ref.native) return snag_errno(ENOTSUP);
            /* The verified prefix is gapless. A half-open range may end at
             * this record, but may not include its own result or a later one.
             * Presentation coordinates stay unchanged; they are not I/O hints. */
            if (result->output_ref.end_sequence > context->sequence) return snag_errno(EINVAL);
        }
    }
    if (record->kind == SNAG_BINARY_VOICE_TRANSFER_ADOPTED)
        return project_voice(context, record, type, data);
    if (record->kind == SNAG_BINARY_RESPONSE_INTERRUPTED ||
        record->kind == SNAG_BINARY_RESPONSE_FAILED ||
        record->kind == SNAG_BINARY_RESPONSE_OUTPUT_CORRECTION) {
        return project_partial(context, record, type, data);
    }
    if (record->kind == SNAG_BINARY_RESPONSE_COMPLETED) {
        return project_graph(context, record, type, data);
    }
    bool embedded = record->kind == SNAG_BINARY_IRC_ADMITTED;
    if (!embedded && record->kind != SNAG_BINARY_INPUT_RECEIVED &&
        record->kind != SNAG_BINARY_FUTURE_TURN_QUEUED &&
        record->kind != SNAG_BINARY_STEERING_ADDED &&
        record->kind != SNAG_BINARY_IRC_REPLY_REMINDER &&
        record->kind != SNAG_BINARY_FUTURE_TURN_EDITED && record->kind != SNAG_BINARY_TURN_STARTED)
        return snag_binary_legacy_decode(record, type, data);
    struct snag_binary_event event, outer;
    if (snag_binary_event_decode(record, &event) < 0) return -1;
    if (embedded) {
        outer = event;
        if (outer.data.irc_admitted.input.kind != SNAG_BINARY_INPUT_RECEIVED &&
            outer.data.irc_admitted.input.kind != SNAG_BINARY_STEERING_ADDED)
            return snag_binary_legacy_decode(record, type, data);
        if (snag_binary_event_decode(&outer.data.irc_admitted.input, &event) < 0) return -1;
    }
    struct input_fields fields = input_fields(&event);
    struct snag_binary_voice_source *voice = event.kind == SNAG_BINARY_FUTURE_TURN_QUEUED &&
        event.data.queued.has_voice ? &event.data.queued.voice : NULL;
    if (!fields.text_ref->field && !fields.content_ref->field &&
        (!fields.instructions_ref || !fields.instructions_ref->field) &&
        (!voice || (!voice->transcript_ref.field && !voice->request_ref.field))) {
        return snag_binary_legacy_decode(record, type, data);
    }
    struct snag_binary_turn_start *turn = event.kind == SNAG_BINARY_TURN_STARTED ?
        &event.data.started : NULL;
    if (turn) {
        bool queued = turn->origin == SNAG_BINARY_TURN_QUEUED;
        if (turn->origin == SNAG_BINARY_TURN_GOAL) {
            /* Goal turns are host-generated, with no input receipt. Current
             * goal authority is independent of the canonical field source;
             * the strict reducer also checks the fixed prompt and no content. */
            if (state->goal_status != SNAG_GOAL_ACTIVE || state->pending_input)
                return snag_errno(EINVAL);
        } else if (queued) {
            char queue_id[SNAG_ID_HEX_LEN + 1u];
            bytes_hex(queue_id, turn->queue_id, sizeof(turn->queue_id));
            if (!state->pending_queue_count ||
                context->sources.queue_count != state->pending_queue_count ||
                strcmp(queue_id, state->pending_queue[0].queue_id) ||
                turn->queue_seq != state->pending_queue[0].seq ||
                context->sources.queue[0].creation != turn->queue_seq) return snag_errno(EINVAL);
        } else if (!state->pending_input || !context->sources.input) {
            return snag_errno(EINVAL);
        }
    }

    struct snag_buf text = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_buf content = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_buf instructions = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_buf transcript = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_buf request = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_buf literal = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_buf enclosing = {.max = SNAG_MAX_EVENT_LINE};
    const unsigned char *view;
    int rc = -1;
    if (fields.text_ref->field) {
        if (read_input_field(context, turn, fields.text_ref,
                SNAG_BINARY_INPUT_TEXT, &text, &view) < 0) goto done;
        *fields.text = (struct snag_binary_text){view, fields.text_ref->target.size};
        *fields.text_ref = (struct snag_binary_input_reference){0};
    }
    if (fields.content_ref->field) {
        if (read_input_field(context, turn, fields.content_ref,
                SNAG_BINARY_INPUT_CONTENT, &content, &view) < 0 ||
            snag_binary_content_decode(view, fields.content_ref->target.size,
                fields.content) < 0) goto done;
        *fields.content_ref = (struct snag_binary_input_reference){0};
    }
    if (fields.instructions_ref && fields.instructions_ref->field) {
        if (read_input_field(context, turn, fields.instructions_ref,
                SNAG_BINARY_INPUT_INSTRUCTIONS, &instructions, &view) < 0 ||
            snag_binary_instructions_decode(view, fields.instructions_ref->target.size,
                fields.instructions) < 0) goto done;
        *fields.instructions_ref = (struct snag_binary_input_reference){0};
    }
    if (voice && voice->transcript_ref.field) {
        if (read_input_field(context, NULL, &voice->transcript_ref,
                SNAG_BINARY_INPUT_VOICE_TRANSCRIPT, &transcript, &view) < 0) goto done;
        voice->transcript = (struct snag_binary_text){view, voice->transcript_ref.target.size};
        voice->transcript_ref = (struct snag_binary_input_reference){0};
    }
    if (voice && voice->request_ref.field) {
        if (read_input_field(context, NULL, &voice->request_ref,
                SNAG_BINARY_INPUT_VOICE_REQUEST, &request, &view) < 0) goto done;
        voice->request = (struct snag_binary_text){view, voice->request_ref.target.size};
        voice->request_ref = (struct snag_binary_input_reference){0};
    }
    /* Revalidate hydrated fields without borrowing the source's receipt
     * metadata or authority. These temporary bytes are never persisted. */
    if (snag_binary_event_encode(&literal, &event) < 0) goto done;
    struct snag_binary_record resolved = *record;
    resolved.payload = literal.data;
    resolved.size = literal.len;
    resolved.version = snag_binary_event_version(event.kind);
    if (embedded) {
        outer.data.irc_admitted.input.payload = literal.data;
        outer.data.irc_admitted.input.size = literal.len;
        outer.data.irc_admitted.input.version = resolved.version;
        if (snag_binary_event_encode(&enclosing, &outer) < 0) goto done;
        resolved.payload = enclosing.data;
        resolved.size = enclosing.len;
        resolved.version = snag_binary_event_version(outer.kind);
    }
    rc = snag_binary_legacy_decode(&resolved, type, data);
 done:
    snag_buf_free(&enclosing);
    snag_buf_free(&literal);
    snag_buf_free(&request);
    snag_buf_free(&transcript);
    snag_buf_free(&instructions);
    snag_buf_free(&content);
    snag_buf_free(&text);
    return rc;
}

int
snag_binary_checkpoint_receipt_read(int fd, const struct snag_binary_anchor *anchor,
    uint64_t wanted, enum snag_binary_kind kind, json_t **out, uint64_t *timestamp)
{
    if (fd < 0 || !anchor || !wanted || wanted >= anchor->next_seq || !out || !timestamp ||
        (kind != SNAG_BINARY_INPUT_RECEIVED && kind != SNAG_BINARY_FUTURE_TURN_QUEUED &&
         kind != SNAG_BINARY_FUTURE_TURN_EDITED && kind != SNAG_BINARY_STEERING_ADDED))
        return snag_errno(EINVAL);
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_batch batch;
    struct snag_binary_anchor before;
    json_t *data = NULL;
    int rc = snag_binary_batch_find(fd, anchor, wanted, &scratch, &batch, &before);
    if (rc < 0) goto done;
    struct snag_binary_record record;
    size_t offset = SNAG_BINARY_BATCH_HEADER_SIZE;
    uint64_t sequence;
    while ((rc = snag_binary_record_next(&batch, &offset, &record, &sequence)) == 0) {
        if (sequence != wanted) continue;
        struct snag_binary_event event;
        rc = snag_errno(EINVAL);
        if (record.flags || snag_binary_event_decode(&record, &event) != 0) goto done;
        bool embedded = event.kind == SNAG_BINARY_IRC_ADMITTED;
        enum snag_binary_kind found = embedded ?
            event.data.irc_admitted.input.kind : event.kind;
        if (!embedded && kind == SNAG_BINARY_STEERING_ADDED &&
            found == SNAG_BINARY_RESPONSE_OUTPUT_CORRECTION) {
            /* The reducer creates steering from this prompt, not its partial
             * public snapshot. Those spans belong to provider/history state. */
            const struct snag_binary_response_correction *value = &event.data.response_correction;
            char turn[SNAG_ID_HEX_LEN + 1u], id[SNAG_ID_HEX_LEN + 1u];
            bytes_hex(turn, value->turn, sizeof(value->turn));
            bytes_hex(id, value->correction, sizeof(value->correction));
            data = json_pack("{s:s,s:s,s:o}", "turn_id", turn, "steering_id", id,
                "text", json_stringn((const char *)value->text.data, value->text.size));
            if (!data) {
                snag_errno(ENOMEM);
                goto done;
            }
            *out = json_incref(data);
            *timestamp = record.timestamp_ms;
            rc = 0;
            goto done;
        }
        if (found != kind && !(kind == SNAG_BINARY_STEERING_ADDED &&
            found == SNAG_BINARY_IRC_REPLY_REMINDER)) goto done;
        struct replay_context context = {.fd = fd, .through = *anchor, .sequence = wanted};
        const char *type;
        if (project_record(&context, NULL, &record, &type, &data) < 0) goto done;
        json_t *receipt = embedded ? json_object_get(data,
            kind == SNAG_BINARY_INPUT_RECEIVED ? "input" : "steering") : data;
        if (!json_is_object(receipt)) {
            snag_errno(EINVAL);
            goto done;
        }
        *out = json_incref(receipt);
        *timestamp = record.timestamp_ms;
        rc = 0;
        goto done;
    }
    if (rc > 0) rc = snag_errno(EINVAL);
done:
    json_decref(data);
    snag_buf_free(&scratch);
    return rc;
}

static int
reduce_record(struct replay_context *context, struct snag_session *state,
    const struct snag_binary_record *record,
    uint64_t sequence, char *error, size_t error_size)
{
    if (record->kind == SNAG_BINARY_LEGACY_CHECKPOINT) {
        struct snag_binary_legacy_checkpoint checkpoint;
        if (snag_binary_legacy_checkpoint_decode(record, &checkpoint) < 0) {
            return snag_fail(error, error_size, errno, "invalid legacy checkpoint marker");
        }
        return 0;
    }
    if (record->flags & SNAG_BINARY_RECORD_OPTIONAL) {
        if (snag_binary_event_name((enum snag_binary_kind)record->kind))
            return snag_fail(error, error_size, EINVAL, "semantic record marked optional");
        return 0;
    }
    const char *type = NULL;
    json_t *data = NULL;
    if (project_record(context, state, record, &type, &data) < 0)
        return snag_fail(error, error_size, errno,
            "cannot project native record %llu (kind %u, version %u)",
            (unsigned long long)sequence, record->kind, record->version);
    int rc = snag_store_reduce_event(state, type, data, sequence, error, error_size);
    if (rc == 0 && update_sources(context, state,
            (enum snag_binary_kind)record->kind, data) < 0) {
        rc = snag_fail(error, error_size, errno, "native input provenance does not match replay");
    }
    if (rc == 0 && snag_binary_checkpoint_texts_step(&context->sources.texts,
            record, sequence, state) < 0)
        rc = snag_fail(error, error_size, errno, "native text provenance does not match replay");
    if (rc == 0) {
        if (!state->pending_call_count) {
            context->sources.calls = (struct snag_binary_checkpoint_call_source){0};
        } else if (record->kind == SNAG_BINARY_RESPONSE_COMPLETED) {
            context->sources.calls = (struct snag_binary_checkpoint_call_source){
                .graph = sequence, .cwd = context->sources.texts.slots[SNAG_BINARY_TEXT_CWD]};
        }
    }
    if (rc == 0 && update_process_sources(context, state,
            (enum snag_binary_kind)record->kind) < 0) {
        rc = snag_fail(error, error_size, errno, "native process origins do not match replay");
    }
    if (rc == 0 && context->fn) {
        state->next_seq = sequence + 1u;
        rc = context->fn(context->opaque, state, sequence, type, data, error, error_size);
    }
    json_decref(data);
    return rc;
}

static int
replay_batches(struct replay_context *context, struct snag_session *state,
    const struct snag_binary_identity *identity, struct snag_binary_anchor *position,
    uint64_t boundary, struct snag_binary_recovery *recovery, char *error, size_t error_size)
{
    struct snag_binary_anchor anchor = *position;
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    int rc = -1;
    for (;;) {
        recovery->problem_seq = anchor.next_seq;
        recovery->problem_start = anchor.end;
        recovery->problem_end = boundary;
        struct snag_binary_batch batch;
        struct snag_binary_anchor next;
        int read = snag_binary_batch_read(context->fd, boundary, &anchor,
            &scratch, &batch, &next);
        if (read < 0) {
            snag_fail(error, error_size, errno, "invalid native batch at byte %llu",
                (unsigned long long)anchor.end);
            goto done;
        }
        if (read == 1) {
            if (anchor.next_seq == 1u) {
                snag_fail(error, error_size, EINVAL, "native journal has no committed creation");
                goto done;
            }
            break;
        }
        size_t cursor = SNAG_BINARY_BATCH_HEADER_SIZE;
        context->through = next;
        for (uint32_t i = 0u; i < batch.count; ++i) {
            struct snag_binary_record record;
            uint64_t sequence;
            recovery->problem_seq = batch.first_seq + i;
            recovery->problem_start = anchor.end;
            recovery->problem_end = next.end;
            if (snag_binary_record_next(&batch, &cursor, &record, &sequence) != 0) {
                snag_fail(error, error_size, EINVAL, "invalid verified native record");
                goto done;
            }
            if (sequence == 1u && (record.kind != SNAG_BINARY_SESSION_CREATED || record.flags ||
                record.timestamp_ms != identity->created_ms)) {
                snag_fail(error, error_size, EINVAL, "native creation does not match its header");
                goto done;
            }
            state->next_seq = sequence;
            state->last_time_ms = record.timestamp_ms;
            context->sequence = sequence;
            if (reduce_record(context, state, &record, sequence, error, error_size) < 0) {
                goto done;
            }
        }
        if (next.turns != state->turn_count) {
            recovery->problem_start = anchor.end;
            recovery->problem_end = next.end;
            snag_fail(error, error_size, EINVAL, "native batch turn count does not match replay");
            goto done;
        }
        state->next_seq = next.next_seq;
        state->log_end = (int64_t)next.end;
        bytes_hex(state->prev_sha256, next.digest, sizeof(next.digest));
        anchor = next;
        *position = anchor;
        recovery->verified = anchor;
        ++recovery->batches;
    }
    rc = 0;
done:
    snag_buf_free(&scratch);
    return rc;
}

static void
normalize_sources(struct snag_binary_checkpoint_sources *sources)
{
    if (!sources->process_count) {
        free(sources->processes);
        sources->processes = NULL;
    }
    if (!sources->queue_count) {
        free(sources->queue);
        sources->queue = NULL;
    }
    if (!sources->download_count) {
        free(sources->downloads);
        sources->downloads = NULL;
    }
}

static int
reconcile_binary(struct snag_session *source, struct snag_session *restored,
    const struct snag_binary_anchor *prefix, snag_session_event_fn fn, void *opaque,
    struct snag_binary_recovery *recovery, struct snag_binary_checkpoint_sources *sources,
    char *error, size_t error_size)
{
    if (!source || !restored || source == restored || !recovery || source->log_fd < 0 ||
        source->lock_fd < 0 || source->pending_log || restored->dir_fd >= 0 ||
        restored->log_fd >= 0 || restored->lock_fd >= 0 || restored->pending_log ||
        !snag_hex_is_lower(source->id, SNAG_ID_HEX_LEN)) {
        return snag_fail(error, error_size, EINVAL,
            "invalid locked native replay source/destination");
    }
    snag_file_info before, after;
    if (snag_fstat(source->log_fd, &before) < 0 || !S_ISREG(before.st_mode) || before.st_size < 0)
        return snag_fail(error, error_size, EINVAL, "cannot inspect native source");
    uint64_t boundary = (uint64_t)before.st_size;
    /* Copy before clearing recovery: prefix may be its previous verified anchor. */
    struct snag_binary_anchor expected = {0};
    if (prefix) {
        expected = *prefix;
        if (expected.end < SNAG_BINARY_HEADER_SIZE || expected.end > boundary)
            return snag_fail(error, error_size, EINVAL, "native prefix lies outside the source");
        boundary = expected.end;
    }
    *recovery = (struct snag_binary_recovery){.problem_seq = 1u, .problem_end = boundary};
    struct snag_binary_identity identity;
    struct snag_binary_anchor anchor;
    if (read_identity(source->log_fd, boundary, &identity, &anchor) < 0)
        return snag_fail(error, error_size, errno, "invalid or incomplete native journal header");
    char id[SNAG_ID_HEX_LEN + 1u];
    bytes_hex(id, identity.id, sizeof(identity.id));
    if (strcmp(id, source->id))
        return snag_fail(error, error_size, EINVAL, "native journal session identity mismatch");
    recovery->verified = anchor;
    struct snag_session state;
    snag_session_init(&state);
    memcpy(state.id, id, sizeof(state.id));
    struct replay_context context = {.fd = source->log_fd, .fn = fn, .opaque = opaque};
    int rc = -1;
    if (replay_batches(&context, &state, &identity, &anchor, boundary, recovery,
        error, error_size) < 0) goto done;
    if (prefix && (anchor.end != expected.end || anchor.next_seq != expected.next_seq ||
        anchor.previous != expected.previous || anchor.turns != expected.turns ||
        memcmp(anchor.digest, expected.digest, sizeof(anchor.digest)))) {
        snag_fail(error, error_size, EINVAL, "native prefix does not match its committed boundary");
        goto done;
    }
    if (snag_fstat(source->log_fd, &after) < 0 || !snag_file_unchanged(&before, &after)) {
        recovery->problem_seq = 1u;
        recovery->problem_start = 0u;
        recovery->problem_end = boundary;
        snag_fail(error, error_size, EAGAIN, "native source changed during replay");
        goto done;
    }
    recovery->incomplete_tail_bytes = boundary - anchor.end;
    recovery->problem_seq = 0u;
    recovery->problem_start = recovery->problem_end = 0u;
    snag_session_close(restored);
    *restored = state;
    normalize_sources(&context.sources);
    if (sources) {
        snag_binary_checkpoint_sources_free(sources);
        *sources = context.sources;
        context.sources = (struct snag_binary_checkpoint_sources){0};
    }
    rc = 0;
 done:
    if (rc < 0) snag_session_close(&state);
    snag_binary_checkpoint_sources_free(&context.sources);
    return rc;
}

int
snag_store_reconcile_binary(struct snag_session *source, struct snag_session *restored,
    snag_session_event_fn fn, void *opaque, struct snag_binary_recovery *recovery,
    struct snag_binary_checkpoint_sources *sources, char *error, size_t error_size)
{
    return reconcile_binary(source, restored, NULL, fn, opaque, recovery, sources,
        error, error_size);
}

int
snag_store_reconcile_binary_prefix(struct snag_session *source, struct snag_session *restored,
    const struct snag_binary_anchor *prefix, snag_session_event_fn fn, void *opaque,
    struct snag_binary_recovery *recovery, struct snag_binary_checkpoint_sources *sources,
    char *error, size_t error_size)
{
    if (!prefix) return snag_fail(error, error_size, EINVAL, "missing native prefix boundary");
    return reconcile_binary(source, restored, prefix, fn, opaque, recovery, sources,
        error, error_size);
}

int
snag_store_reduce_binary_suffix(struct snag_session *source, struct snag_session *state,
    const struct snag_binary_anchor *start, snag_session_event_fn fn, void *opaque,
    struct snag_binary_recovery *recovery, struct snag_binary_checkpoint_sources *sources,
    char *error, size_t error_size)
{
    if (!source || !state || source == state || !start || !recovery || !sources ||
        source->log_fd < 0 || source->lock_fd < 0 || source->pending_log ||
        state->dir_fd >= 0 || state->log_fd >= 0 || state->lock_fd >= 0 || state->pending_log ||
        state->on_commit || state->on_commit_free || state->on_commit_opaque ||
        state->on_checkpoint || state->checkpoint_context || state->checkpoint_state ||
        !state->format_version || !snag_hex_is_lower(source->id, SNAG_ID_HEX_LEN) ||
        start->next_seq <= 1u || start->end < SNAG_BINARY_HEADER_SIZE ||
        state->next_seq != start->next_seq || state->log_end < 0 ||
        (uint64_t)state->log_end != start->end || state->turn_count != start->turns ||
        sources->process_count != state->process_count ||
        sources->queue_count != state->pending_queue_count ||
        sources->download_count != json_array_size(state->download_queue) ||
        (sources->process_count && !sources->processes) ||
        (sources->queue_count && !sources->queue) ||
        (sources->download_count && !sources->downloads) ||
        (state->response_open && (!sources->response_start ||
            sources->response_start >= start->next_seq))) {
        return snag_fail(error, error_size, EINVAL, "invalid provisional native suffix state");
    }
    /* Copy first: start may alias the caller's previous recovery.verified. */
    struct snag_binary_anchor anchor = *start, root;
    *recovery = (struct snag_binary_recovery){.verified = anchor,
        .problem_seq = anchor.next_seq, .problem_start = anchor.end};
    snag_file_info before, after;
    if (snag_fstat(source->log_fd, &before) < 0 || !S_ISREG(before.st_mode) ||
        before.st_size < 0 || anchor.end > (uint64_t)before.st_size)
        return snag_fail(error, error_size, EINVAL, "cannot inspect native suffix source");
    uint64_t boundary = (uint64_t)before.st_size;
    recovery->problem_end = boundary;
    struct snag_binary_identity identity;
    if (read_identity(source->log_fd, boundary, &identity, &root) < 0)
        return snag_fail(error, error_size, errno, "invalid native suffix source header");
    char id[SNAG_ID_HEX_LEN + 1u], hash[65];
    bytes_hex(id, identity.id, sizeof(identity.id));
    bytes_hex(hash, anchor.digest, sizeof(anchor.digest));
    if (strcmp(id, source->id) || strcmp(id, state->id) || strcmp(hash, state->prev_sha256))
        return snag_fail(error, error_size, EINVAL, "native suffix identity/boundary mismatch");
    struct replay_context context = {.fd = source->log_fd, .fn = fn, .opaque = opaque,
        .sequence = anchor.next_seq - 1u, .through = anchor, .sources = *sources,
        .response_sequence = state->response_open ? sources->response_start : 0u,
        .process_capacity = sources->process_count, .queue_capacity = sources->queue_count,
        .download_capacity = sources->download_count};
    *sources = (struct snag_binary_checkpoint_sources){0};
    int rc = replay_batches(&context, state, &identity, &anchor, boundary, recovery,
        error, error_size);
    /* Return ownership even on failure: this candidate is disposable, not the
     * caller's adopted session. Nothing dispatches or repairs the source. */
    *sources = context.sources;
    if (rc == 0 && (snag_fstat(source->log_fd, &after) < 0 ||
        !snag_file_unchanged(&before, &after))) {
        rc = snag_fail(error, error_size, EAGAIN, "native source changed during suffix replay");
    }
    if (rc == 0) {
        normalize_sources(sources);
        recovery->incomplete_tail_bytes = boundary - anchor.end;
        recovery->problem_seq = recovery->problem_start = recovery->problem_end = 0u;
    }
    return rc;
}
