/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_checkpoint.h"

#include "fs.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct closure_capture {
    int fd;
    const struct snag_binary_anchor *through;
    const struct snag_binary_checkpoint_index *available;
    bool (*cancelled)(void *);
    void *opaque;
    struct snag_buf roots, needed, entries;
    struct snag_buf *ranges; /* Non-NULL only during pure producer query capture. */
    struct snag_buf scratch, flat;
    struct snag_binary_batch batch;
    struct snag_binary_anchor before, after;
};

static bool
cancel_capture(void *opaque)
{
    struct closure_capture *capture = opaque;
    return capture->cancelled && capture->cancelled(capture->opaque);
}

static int
need(struct closure_capture *capture, struct snag_buf *out, uint64_t sequence)
{
    if (!sequence) return 0;
    if (cancel_capture(capture)) return snag_errno(ECANCELED);
    if (sequence >= capture->through->next_seq) return snag_errno(EINVAL);
    return snag_buf_append(out, &sequence, sizeof(sequence));
}

static int
sequence_order(const void *left, const void *right)
{
    uint64_t a, b;
    memcpy(&a, left, sizeof(a));
    memcpy(&b, right, sizeof(b));
    return a < b ? -1 : a > b;
}

static void
unique(struct snag_buf *buffer)
{
    size_t count = buffer->len / sizeof(uint64_t);
    if (!count) return;
    uint64_t *sequences = (uint64_t *)buffer->data;
    qsort(sequences, count, sizeof(*sequences), sequence_order);
    size_t kept = 1u;
    for (size_t i = 1u; i < count; ++i) {
        if (sequences[i] != sequences[kept - 1u]) sequences[kept++] = sequences[i];
    }
    buffer->len = kept * sizeof(*sequences);
}

/* Retain only one bounded physical batch/index scratch. Even cached payloads
 * require old table membership, so a neighboring point cannot fill an old gap. */
static int
load(struct closure_capture *capture, uint64_t sequence,
    struct snag_binary_record *record, struct snag_binary_index_entry *entry)
{
    if (cancel_capture(capture)) return snag_errno(ECANCELED);
    if (!capture->batch.count || sequence < capture->before.next_seq ||
        sequence >= capture->after.next_seq) {
        if (snag_binary_checkpoint_batch_find(capture->fd, capture->through,
                capture->available, sequence, &capture->scratch, &capture->batch,
                &capture->before) < 0) return -1;
        int read = snag_binary_batch_read(capture->fd, capture->through->end, &capture->before,
            &capture->scratch, &capture->batch, &capture->after);
        if (read != 0) return read < 0 ? -1 : snag_errno(EIO);
        snag_buf_reset(&capture->flat);
        if (snag_binary_index_append_batch(&capture->flat, &capture->available->identity,
                &capture->before, &capture->after, capture->batch.data,
                capture->batch.size) < 0) return -1;
    }
    size_t offset = (size_t)(sequence - capture->before.next_seq) * SNAG_BINARY_INDEX_ENTRY_SIZE;
    struct snag_binary_index_entry canonical;
    if (snag_binary_index_entry_decode(capture->flat.data + offset,
            SNAG_BINARY_INDEX_ENTRY_SIZE, &capture->available->identity, sequence,
            &canonical) < 0) return -1;
    if (sequence < capture->available->boundary.next_seq) {
        struct snag_binary_index_entry listed;
        int found = snag_binary_checkpoint_index_find(capture->available, sequence, &listed);
        if (found != 0) return found < 0 ? -1 : snag_errno(ENOENT);
        if (listed.batch_offset != canonical.batch_offset || listed.turn != canonical.turn ||
            listed.record_offset != canonical.record_offset || listed.kind != canonical.kind ||
            memcmp(listed.batch_digest, canonical.batch_digest, sizeof(listed.batch_digest)))
            return snag_errno(EINVAL);
    }
    size_t cursor = canonical.record_offset;
    uint64_t found;
    if (snag_binary_record_next(&capture->batch, &cursor, record, &found) != 0 ||
        found != sequence) return snag_errno(EINVAL);
    *entry = canonical;
    return 0;
}

static int
collect_output(void *opaque, const struct snag_binary_record *record, uint64_t sequence)
{
    struct closure_capture *capture = opaque;
    return record->kind == SNAG_BINARY_RESPONSE_OUTPUT ?
        need(capture, &capture->needed, sequence) : 0;
}

static int
collect_transform(void *opaque, const struct snag_binary_record *record, uint64_t sequence)
{
    struct closure_capture *capture = opaque;
    return record->kind == SNAG_BINARY_RULE_TRANSFORM ?
        need(capture, &capture->needed, sequence) : 0;
}

static int
collect_range(struct closure_capture *capture, uint64_t first, uint64_t end,
    int (*visit)(void *, const struct snag_binary_record *, uint64_t))
{
    if (!first) return end ? snag_errno(EINVAL) : 0;
    if (first > end || end > capture->through->next_seq) return snag_errno(EINVAL);
    if (cancel_capture(capture)) return snag_errno(ECANCELED);
    if (capture->ranges) {
        uint64_t range[3] = {first, end, visit == collect_transform ?
            SNAG_BINARY_RULE_TRANSFORM : SNAG_BINARY_RESPONSE_OUTPUT};
        return snag_buf_append(capture->ranges, range, sizeof(range));
    }
    return snag_binary_checkpoint_records_read(capture->fd, capture->through,
        capture->available, first, end, visit, cancel_capture, capture);
}

static int
collect_field(struct closure_capture *capture,
    const struct snag_binary_input_reference *reference, uint64_t owner)
{
    if (!reference->field) return 0;
    if (!reference->target.sequence || reference->target.sequence >= owner)
        return snag_errno(EINVAL);
    return need(capture, &capture->needed, reference->target.sequence);
}

static int
collect_span(struct closure_capture *capture,
    const struct snag_binary_output_span *span, uint64_t owner)
{
    if (!span->first.sequence) return 0;
    unsigned char bytes[SNAG_BINARY_OUTPUT_SPAN_SIZE];
    if (snag_binary_output_span_encode(bytes, span) < 0) return -1;
    if (span->last_sequence >= owner) return snag_errno(EINVAL);
    if (need(capture, &capture->needed, span->first.sequence) < 0 ||
        need(capture, &capture->needed, span->last_sequence) < 0) return -1;
    return collect_range(capture, span->first.sequence, span->last_sequence + 1u, collect_output);
}

static int
collect_public(struct closure_capture *capture,
    const struct snag_binary_public_items *items, uint64_t owner)
{
    size_t cursor = 0u;
    struct snag_binary_public_value value;
    int rc;
    while ((rc = snag_binary_public_items_next(items, &cursor, &value)) == 0) {
        if (collect_span(capture, &value.source, owner) < 0) return -1;
    }
    return rc < 0 ? -1 : 0;
}

static int
collect_dependencies(struct closure_capture *capture,
    const struct snag_binary_record *record, uint64_t owner)
{
    if (record->kind == SNAG_BINARY_LEGACY_CHECKPOINT) return 0;
    struct snag_binary_event event;
    int rc = snag_binary_event_decode(record, &event);
    if (rc != 0) return rc < 0 ? -1 : snag_errno(EINVAL);
    if (event.kind == SNAG_BINARY_IRC_ADMITTED) {
        size_t offset = 0u;
        uint64_t sequence;
        while ((rc = snag_binary_sequences_next(&event.data.irc_admitted.sequences,
                &offset, &sequence)) == 0) {
            if (!sequence || sequence >= owner) return snag_errno(EINVAL);
            /* Canonical admission lookup keeps its exact next-row seam, including
             * non-input IRC and the discriminator proving a non-IRC neighbor. */
            if (need(capture, &capture->needed, sequence) < 0 ||
                need(capture, &capture->needed, sequence + 1u) < 0) return -1;
        }
        if (rc < 0) return -1;
        if (!event.data.irc_admitted.input.kind) return 0;
        struct snag_binary_record embedded = event.data.irc_admitted.input;
        rc = snag_binary_event_decode(&embedded, &event);
        if (rc != 0) return rc < 0 ? -1 : snag_errno(EINVAL);
    }
    const struct snag_binary_input_reference *fields[5] = {0};
    switch (event.kind) {
    case SNAG_BINARY_INPUT_RECEIVED:
        fields[0] = &event.data.input.text_ref;
        fields[1] = &event.data.input.content_ref;
        fields[2] = &event.data.input.instructions_ref;
        break;
    case SNAG_BINARY_TURN_STARTED:
        fields[0] = &event.data.started.text_ref;
        fields[1] = &event.data.started.content_ref;
        fields[2] = &event.data.started.instructions_ref;
        break;
    case SNAG_BINARY_FUTURE_TURN_QUEUED:
    case SNAG_BINARY_FUTURE_TURN_EDITED:
        fields[0] = &event.data.queued.text_ref;
        fields[1] = &event.data.queued.content_ref;
        if (event.data.queued.has_voice) {
            fields[3] = &event.data.queued.voice.transcript_ref;
            fields[4] = &event.data.queued.voice.request_ref;
        }
        break;
    case SNAG_BINARY_STEERING_ADDED:
    case SNAG_BINARY_IRC_REPLY_REMINDER:
        fields[0] = &event.data.steering_input.text_ref;
        fields[1] = &event.data.steering_input.content_ref;
        break;
    case SNAG_BINARY_VOICE_TRANSFER_ADOPTED:
        if (!event.data.voice_transfer_adopted.native ||
            event.data.voice_transfer_adopted.begin_seq >= owner)
            return snag_errno(EINVAL);
        return need(capture, &capture->needed, event.data.voice_transfer_adopted.begin_seq);
    case SNAG_BINARY_RESPONSE_INTERRUPTED:
        return collect_public(capture, &event.data.response_interrupted.partial, owner);
    case SNAG_BINARY_RESPONSE_FAILED:
        return collect_public(capture, &event.data.response_failed.partial, owner);
    case SNAG_BINARY_RESPONSE_OUTPUT_CORRECTION:
        return collect_public(capture, &event.data.response_correction.partial, owner);
    case SNAG_BINARY_RESPONSE_COMPLETED: {
        size_t cursor = 0u;
        struct snag_binary_graph_item item;
        while ((rc = snag_binary_graph_items_next(&event.data.response_completed.items,
                &cursor, &item)) == 0) {
            if (item.kind != SNAG_BINARY_ITEM_TOOL_CALL &&
                collect_span(capture, &item.data.output.source, owner) < 0) return -1;
        }
        return rc < 0 ? -1 : 0;
    }
    default:
        break;
    }
    for (size_t i = 0u; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        if (fields[i] && collect_field(capture, fields[i], owner) < 0) return -1;
    }
    return 0;
}

static int
collect_call(struct closure_capture *capture,
    const struct snag_binary_checkpoint_call_source *source, uint64_t end)
{
    if (!source->graph) return 0;
    /* Call/process labels consume graph metadata, not its public text. A
     * retained provider graph independently enters the projection root set. */
    if (need(capture, &capture->needed, source->graph) < 0 ||
        need(capture, &capture->needed, source->cwd.declaration) < 0 ||
        need(capture, &capture->needed, source->cwd.original.target.sequence) < 0) return -1;
    return collect_range(capture, source->graph + 1u, end, collect_transform);
}

static uint64_t
number(const unsigned char *bytes)
{
    uint64_t value = 0u;
    for (size_t i = 0u; i < 8u; ++i) value |= (uint64_t)bytes[i] << (i * 8u);
    return value;
}

static int
collect_roots(struct closure_capture *capture,
    const struct snag_binary_checkpoint_sources *sources, const struct snag_session *state,
    const struct snag_binary_checkpoint_provider *provider)
{
    for (size_t i = 0u; i < SNAG_BINARY_CHECKPOINT_TEXT_COUNT; ++i) {
        const struct snag_binary_checkpoint_text_source *source = &sources->texts.slots[i];
        if (need(capture, &capture->roots, source->declaration) < 0 ||
            need(capture, &capture->needed, source->original.target.sequence) < 0) return -1;
    }
    if (collect_call(capture, &sources->calls, capture->through->next_seq) < 0) return -1;
    for (size_t i = 0u; i < sources->process_count; ++i) {
        const struct snag_binary_checkpoint_process_source *source = &sources->processes[i];
        if (need(capture, &capture->roots, source->started) < 0 ||
            collect_call(capture, &source->call, source->started) < 0) return -1;
    }
    for (size_t i = 0u; i < sources->queue_count; ++i) {
        if (need(capture, &capture->roots, sources->queue[i].creation) < 0 ||
            need(capture, &capture->roots, sources->queue[i].text) < 0) return -1;
    }
    for (size_t i = 0u; i < state->pending_steering_count; ++i) {
        if (need(capture, &capture->roots, state->pending_steering[i].seq) < 0) return -1;
    }
    for (size_t i = 0u; i < sources->download_count; ++i) {
        if (need(capture, &capture->roots, sources->downloads[i].receipt) < 0) return -1;
    }
    uint64_t fixed[] = {sources->input, sources->compact_start, sources->compact_end,
        sources->active_compact, sources->response_start, sources->response_end,
        sources->resume_options, state->voice_history.adopted_seq};
    for (size_t i = 0u; i < sizeof(fixed) / sizeof(fixed[0]); ++i) {
        if (need(capture, &capture->roots, fixed[i]) < 0) return -1;
    }
    if (sources->response_end && collect_range(capture, sources->response_start + 1u,
            sources->response_end + 1u, collect_output) < 0) return -1;
    for (size_t i = 0u; i < provider->recent_count; ++i) {
        if (need(capture, &capture->roots, number(provider->recent + i * 36u)) < 0) return -1;
    }
    for (size_t i = 0u; i < provider->history_count; ++i) {
        if (need(capture, &capture->roots, number(provider->history + i * 8u)) < 0) return -1;
    }
    return 0;
}

void
snag_binary_checkpoint_access_plan_free(struct snag_binary_checkpoint_access_plan *plan)
{
    if (!plan) return;
    snag_buf_free(&plan->roots);
    snag_buf_free(&plan->needed);
    snag_buf_free(&plan->ranges);
    *plan = (struct snag_binary_checkpoint_access_plan){0};
}

int
snag_binary_checkpoint_access_plan_build(struct snag_binary_checkpoint_access_plan *out,
    const struct snag_binary_anchor *through, const struct snag_binary_checkpoint_sources *sources,
    const struct snag_session *state, const void *provider_bytes, size_t provider_size,
    bool (*cancelled)(void *), void *opaque)
{
    if (!out || !through || !sources || !state ||
        !through->next_seq || through->next_seq != state->next_seq ||
        sources->texts.through >= through->next_seq ||
        sources->process_count != state->process_count ||
        sources->queue_count != state->pending_queue_count ||
        (sources->process_count && !sources->processes) ||
        (sources->queue_count && !sources->queue) ||
        (sources->download_count && !sources->downloads) ||
        (state->pending_steering_count && !state->pending_steering)) return snag_errno(EINVAL);
    struct snag_binary_checkpoint_provider provider;
    if (snag_binary_checkpoint_provider_decode(provider_bytes, provider_size, &provider) < 0)
        return -1;
    if (provider.next_seq != through->next_seq) return snag_errno(EINVAL);
    struct snag_binary_checkpoint_access_plan staged = {.boundary = *through,
        .ranges = {.max = SIZE_MAX}};
    struct closure_capture capture = {.through = through, .cancelled = cancelled,
        .opaque = opaque, .roots = {.max = SIZE_MAX}, .needed = {.max = SIZE_MAX},
        .ranges = &staged.ranges};
    int rc = collect_roots(&capture, sources, state, &provider);
    if (!rc && cancel_capture(&capture)) rc = snag_errno(ECANCELED);
    if (rc < 0) {
        int saved = errno;
        snag_buf_free(&capture.roots);
        snag_buf_free(&capture.needed);
        snag_binary_checkpoint_access_plan_free(&staged);
        return snag_errno(saved);
    }
    unique(&capture.roots);
    unique(&capture.needed);
    staged.roots = capture.roots;
    staged.needed = capture.needed;
    snag_binary_checkpoint_access_plan_free(out);
    *out = staged;
    return 0;
}

int
snag_binary_checkpoint_access_plan_read(int fd,
    const struct snag_binary_checkpoint_access_plan *plan,
    const struct snag_binary_checkpoint_index *available,
    const struct snag_binary_index_tree *frontier, bool (*cancelled)(void *), void *opaque,
    struct snag_buf *out)
{
    if (fd < 0 || !plan || !available || !frontier || !out || !plan->boundary.next_seq ||
        frontier->count != plan->boundary.next_seq - 1u ||
        plan->roots.len % sizeof(uint64_t) || plan->needed.len % sizeof(uint64_t) ||
        plan->ranges.len % (3u * sizeof(uint64_t)) ||
        (plan->roots.len && !plan->roots.data) || (plan->needed.len && !plan->needed.data) ||
        (plan->ranges.len && !plan->ranges.data)) return snag_errno(EINVAL);
    const struct snag_binary_anchor *through = &plan->boundary;
    if (cancelled && cancelled(opaque)) return snag_errno(ECANCELED);
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    struct snag_binary_identity identity;
    struct snag_binary_anchor root;
    size_t position = 0u;
    while (position < sizeof(header)) {
        ssize_t got = snag_pread(fd, header + position, sizeof(header) - position,
            (int64_t)position);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) return -1;
        if (!got) return snag_errno(EIO);
        position += (size_t)got;
    }
    if (snag_binary_header_decode(header, sizeof(header), &identity, &root) < 0) return -1;
    if (identity.created_ms != available->identity.created_ms ||
        memcmp(identity.id, available->identity.id, sizeof(identity.id))) return snag_errno(EINVAL);
    struct closure_capture capture = {.fd = fd, .through = through, .available = available,
        .cancelled = cancelled, .opaque = opaque, .roots = {.max = SIZE_MAX},
        .needed = {.max = SIZE_MAX}, .entries = {.max = SIZE_MAX},
        .scratch = {.max = SNAG_BINARY_BATCH_MAX}, .flat = {.max = SNAG_BINARY_INDEX_BATCH_MAX}};
    int rc = -1;
    if (snag_buf_append(&capture.roots, plan->roots.data, plan->roots.len) < 0 ||
        snag_buf_append(&capture.needed, plan->needed.data, plan->needed.len) < 0) goto done;
    for (size_t i = 0u; i < plan->ranges.len / (3u * sizeof(uint64_t)); ++i) {
        uint64_t range[3];
        memcpy(range, plan->ranges.data + i * sizeof(range), sizeof(range));
        if (range[2] != SNAG_BINARY_RULE_TRANSFORM &&
            range[2] != SNAG_BINARY_RESPONSE_OUTPUT) { snag_errno(EINVAL); goto done; }
        if (collect_range(&capture, range[0], range[1], range[2] == SNAG_BINARY_RULE_TRANSFORM ?
                collect_transform : collect_output) < 0) goto done;
    }
    for (size_t i = 0u; i < capture.roots.len / sizeof(uint64_t); ++i) {
        uint64_t sequence = ((const uint64_t *)capture.roots.data)[i];
        struct snag_binary_record record;
        struct snag_binary_index_entry entry;
        if (load(&capture, sequence, &record, &entry) < 0 ||
            collect_dependencies(&capture, &record, sequence) < 0) goto done;
    }
    if (snag_buf_append(&capture.needed, capture.roots.data, capture.roots.len) < 0) goto done;
    unique(&capture.needed);
    for (size_t i = 0u; i < capture.needed.len / sizeof(uint64_t); ++i) {
        uint64_t sequence = ((const uint64_t *)capture.needed.data)[i];
        struct snag_binary_record record;
        struct snag_binary_index_entry entry;
        if (load(&capture, sequence, &record, &entry) < 0 ||
            snag_buf_append(&capture.entries, &entry, sizeof(entry)) < 0) goto done;
    }
    if (cancel_capture(&capture)) { snag_errno(ECANCELED); goto done; }
    rc = snag_binary_checkpoint_index_encode(out, &identity, through, frontier,
        (const struct snag_binary_index_entry *)capture.entries.data,
        capture.entries.len / sizeof(struct snag_binary_index_entry));
done:
    snag_buf_free(&capture.roots);
    snag_buf_free(&capture.needed);
    snag_buf_free(&capture.entries);
    snag_buf_free(&capture.scratch);
    snag_buf_free(&capture.flat);
    return rc;
}

int
snag_binary_checkpoint_access_capture(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_index *available,
    const struct snag_binary_index_tree *frontier,
    const struct snag_binary_checkpoint_sources *sources, const struct snag_session *state,
    const void *provider_bytes, size_t provider_size, bool (*cancelled)(void *), void *opaque,
    struct snag_buf *out)
{
    if (fd < 0 || !through || !available || !frontier || !out) return snag_errno(EINVAL);
    struct snag_binary_checkpoint_access_plan plan = {0};
    if (snag_binary_checkpoint_access_plan_build(&plan, through, sources, state,
            provider_bytes, provider_size, cancelled, opaque) < 0) return -1;
    int rc = snag_binary_checkpoint_access_plan_read(fd, &plan, available, frontier,
        cancelled, opaque, out);
    int saved = errno;
    snag_binary_checkpoint_access_plan_free(&plan);
    errno = saved;
    return rc;
}
