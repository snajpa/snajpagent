/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_context.h"

#include "base.h"
#include "fs.h"
#include "irc.h"
#include "store_binary_legacy.h"
#include "store_binary_wire.h"
#include "store_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct source_walk {
    int fd;
    const struct snag_context_control *control;
    const struct snag_binary_anchor *verified;
    const struct snag_binary_checkpoint_index *access;
    uint64_t prompt_input;
    struct snag_buf *additional;
};

static int
source_header(int fd, struct snag_binary_identity *identity, struct snag_binary_anchor *anchor)
{
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
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

struct source_selection {
    const struct source_walk *source;
    const char *prompt;
    json_t *rows, *seen;
    size_t matched;
};

static int
select_source(struct source_selection *selection, uint64_t sequence,
    const char *type, json_t *data)
{
    char key[32];
    (void)snprintf(key, sizeof(key), "%llu", (unsigned long long)sequence);
    if (json_object_get(selection->seen, key)) return 0;
    if (selection->prompt && snag_string_in(type, "irc_event irc_event_v2")) {
        struct snag_irc_event event;
        if (snag_irc_event_record_read(type, data, &event) < 0) return -1;
        char reference[SNAG_ID_HEX_LEN + 48u];
        (void)snprintf(reference, sizeof(reference), "[IRC update id=%s:%llu ",
            event.stream, (unsigned long long)event.sequence);
        selection->matched += event.input && event.stream[0] && event.sequence &&
            strstr(selection->prompt, reference) != NULL;
    }
    if (sequence > INT64_MAX) return snag_errno(EOVERFLOW);
    json_t *row = json_pack("{s:I,s:s,s:o}", "seq", (json_int_t)sequence,
        "type", type, "data", json_incref(data));
    if (!row || json_array_append_new(selection->rows, row) < 0 ||
        json_object_set_new(selection->seen, key, json_true()) < 0) return snag_errno(ENOMEM);
    return 0;
}

static int
source_point(struct source_selection *selection, uint64_t sequence, bool adjacent)
{
    const struct source_walk *source = selection->source;
    if (!sequence || sequence >= source->verified->next_seq) return snag_errno(ENOENT);
    if (source->control && source->control->cancelled &&
        source->control->cancelled(source->control->opaque)) return snag_errno(ECANCELED);
    const char *type = NULL;
    json_t *data = NULL;
    if (snag_binary_checkpoint_projection_read(source->fd, source->verified,
            source->access, sequence, &type, &data) < 0) return -1;
    bool marker = !strcmp(type, "session_checkpoint");
    int rc = (snag_string_in(type, "irc_event irc_event_v2") || (adjacent && marker)) ?
        select_source(selection, sequence, type, data) : snag_errno(EINVAL);
    json_decref(data);
    if (!rc && marker) rc = source_point(selection, sequence + 1u, false);
    return rc;
}

static bool
source_selection_cancelled(void *opaque)
{
    const struct source_selection *selection = opaque;
    const struct snag_context_control *control = selection->source->control;
    return control && control->cancelled && control->cancelled(control->opaque);
}

static int
source_record(void *opaque, const struct snag_binary_record *record, uint64_t sequence)
{
    struct source_selection *selection = opaque;
    if (record->kind != SNAG_BINARY_IRC_EVENT && record->kind != SNAG_BINARY_IRC_EVENT_V2 &&
        record->kind != SNAG_BINARY_LEGACY_CHECKPOINT &&
        record->kind != SNAG_BINARY_CHECKPOINT_RECEIPT) return 0;
    const char *type = NULL;
    json_t *data = NULL;
    int rc = snag_binary_checkpoint_record_project(selection->source->fd,
        selection->source->verified, selection->source->access, record, sequence, &type, &data);
    if (!rc) rc = select_source(selection, sequence, type, data);
    json_decref(data);
    return rc;
}

/* Earlier checkpoints retained the prompt's admission receipt as a text leaf,
 * without its IRC dependencies once compaction removed that admission from the
 * recent context. The authenticated receipt supplies the exact missing points. */
static int
prompt_dependency(struct source_selection *selection, uint64_t sequence)
{
    const struct source_walk *source = selection->source;
    if (!sequence || sequence >= source->verified->next_seq) return snag_errno(EINVAL);
    struct snag_binary_index_entry entry;
    int missing = sequence < source->access->boundary.next_seq ?
        snag_binary_checkpoint_index_find(source->access, sequence, &entry) : 0;
    if (missing < 0) return -1;
    struct source_walk direct = *source;
    if (missing) direct.access = NULL;
    struct source_selection scoped = *selection;
    scoped.source = &direct;
    int rc = snag_binary_checkpoint_records_read(source->fd, source->verified, direct.access,
        sequence, sequence + 1u, source_record, source_selection_cancelled, &scoped);
    selection->matched = scoped.matched;
    if (rc < 0 || !missing || !source->additional) return rc;
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_buf flat = {.max = SNAG_BINARY_INDEX_BATCH_MAX};
    struct snag_binary_batch batch;
    struct snag_binary_anchor before;
    struct snag_binary_anchor after;
    rc = -1;
    if (source_selection_cancelled(selection)) { snag_errno(ECANCELED); goto done; }
    if (snag_binary_batch_find(source->fd, &source->access->boundary, sequence,
            &scratch, &batch, &before) < 0 ||
        snag_binary_batch_read(source->fd, source->access->boundary.end,
            &before, &scratch, &batch, &after) != 0 ||
        snag_binary_index_append_batch(&flat, &source->access->identity,
            &before, &after, batch.data, batch.size) < 0) goto done;
    size_t offset = (size_t)(sequence - before.next_seq) * SNAG_BINARY_INDEX_ENTRY_SIZE;
    if (snag_binary_index_entry_decode(flat.data + offset, SNAG_BINARY_INDEX_ENTRY_SIZE,
            &source->access->identity, sequence, &entry) < 0) goto done;
    for (size_t i = 0u; i < source->additional->len / sizeof(entry); ++i) {
        const struct snag_binary_index_entry *saved =
            (const struct snag_binary_index_entry *)source->additional->data + i;
        if (saved->sequence == sequence) { rc = 0; goto done; }
    }
    rc = snag_buf_append(source->additional, &entry, sizeof(entry));
done:
    snag_buf_free(&scratch);
    snag_buf_free(&flat);
    return rc;
}

static int
prompt_sources(struct source_selection *selection)
{
    const struct source_walk *source = selection->source;
    if (!selection->prompt || !source->prompt_input) return 0;
    const char *type = NULL;
    json_t *data = NULL;
    if (snag_binary_checkpoint_projection_read(source->fd, source->verified, source->access,
            source->prompt_input, &type, &data) < 0) return -1;
    const char *text = snag_json_string(json_object_get(data, "input"), "text");
    int rc = 0;
    if (!strcmp(type, "irc_admitted") && text && !strcmp(text, selection->prompt)) {
        const json_t *sequences = json_object_get(data, "sequences");
        for (size_t i = 0u; i < json_array_size(sequences); ++i) {
            uint64_t sequence = (uint64_t)json_integer_value(json_array_get(sequences, i));
            if (!sequence || sequence >= source->prompt_input) { rc = snag_errno(EINVAL); break; }
            if (prompt_dependency(selection, sequence) < 0 ||
                prompt_dependency(selection, sequence + 1u) < 0) { rc = -1; break; }
        }
    }
    json_decref(data);
    return rc;
}

static int
compare_sources(const void *left, const void *right)
{
    json_int_t a = json_integer_value(json_object_get(*(json_t *const *)left, "seq"));
    json_int_t b = json_integer_value(json_object_get(*(json_t *const *)right, "seq"));
    return (a > b) - (a < b);
}

static int
walk_selected_sources(const struct source_walk *source, const json_t *wanted,
    const char *prompt, snag_session_event_fn fn, void *argument, char *error, size_t error_size)
{
    struct source_selection selection = {.source = source, .prompt = prompt,
        .rows = json_array(), .seen = json_object()};
    int rc = -1;
    json_t **ordered = NULL;
    if (!selection.rows || !selection.seen) { snag_errno(ENOMEM); goto done; }
    const char *key;
    json_t *value;
    json_object_foreach((json_t *)wanted, key, value) {
        uint64_t admission = json_is_integer(value) && json_integer_value(value) > 0 ?
            (uint64_t)json_integer_value(value) : 0u;
        if (!admission) {
            snag_errno(EINVAL);
            goto done;
        }
        uint64_t sequence = 0u;
        for (const char *p = key; *p; ++p) {
            if (*p < '0' || *p > '9' || sequence > (UINT64_MAX - (unsigned int)(*p - '0')) / 10u) {
                snag_errno(EINVAL);
                goto done;
            }
            sequence = sequence * 10u + (unsigned int)(*p - '0');
        }
        if (source_point(&selection, sequence, true) < 0) goto done;
        uint64_t next = sequence + 1u;
        if (next >= source->verified->next_seq) continue;
        const struct snag_binary_checkpoint_index *adjacent_access = source->access;
        if (next < source->access->boundary.next_seq) {
            struct snag_binary_index_entry adjacent;
            int found = snag_binary_checkpoint_index_find(source->access, next, &adjacent);
            if (found < 0) goto done;
            /* A suffix admission can name an unconsumed IRC event retained
             * across compaction. Its neighbour was not yet a checkpoint
             * dependency; authenticate that new lookup from the journal. */
            if (found) {
                if (admission < source->access->boundary.next_seq) {
                    snag_errno(ENOENT);
                    goto done;
                }
                adjacent_access = NULL;
            } else if (adjacent.kind != SNAG_BINARY_IRC_EVENT &&
                adjacent.kind != SNAG_BINARY_IRC_EVENT_V2 &&
                adjacent.kind != SNAG_BINARY_LEGACY_CHECKPOINT &&
                adjacent.kind != SNAG_BINARY_CHECKPOINT_RECEIPT) continue;
        }
        /* Match the legacy collector's canonical next-row seam exactly, including
         * non-input IRC metadata. A stream counter never supplies this ordinal. */
        if (snag_binary_checkpoint_records_read(source->fd, source->verified, adjacent_access,
                next, next + 1u, source_record, source_selection_cancelled, &selection) < 0)
            goto done;
    }
    if (prompt) {
        if (prompt_sources(&selection) < 0) goto done;
        if (snag_binary_checkpoint_records_read(source->fd, source->verified, source->access,
                1u, source->verified->next_seq, source_record,
                source_selection_cancelled, &selection) < 0) goto done;
        size_t expected = 0u;
        const char *part = prompt;
        while ((part = strstr(part, "[IRC update id=")) != NULL) {
            part += sizeof("[IRC update id=") - 1u;
            size_t stream = strspn(part, "0123456789abcdef");
            if (stream != SNAG_ID_HEX_LEN || part[stream] != ':') continue;
            const char *counter = part + stream + 1u;
            size_t digits = strspn(counter, "0123456789");
            /* Canonical admissions identify legacy plain labels independently.
             * Only the structured stream-reference prefix names this lookup. */
            expected += digits && counter[digits] == ' ';
        }
        if (selection.matched != expected) {
            snag_fail(error, error_size, ENOENT, "current IRC source closure unavailable");
            goto done;
        }
    }
    size_t count = json_array_size(selection.rows);
    if (count > SIZE_MAX / sizeof(*ordered)) { snag_errno(EOVERFLOW); goto done; }
    ordered = count ? malloc(count * sizeof(*ordered)) : NULL;
    if (count && !ordered) { snag_errno(ENOMEM); goto done; }
    for (size_t i = 0u; i < count; ++i) ordered[i] = json_array_get(selection.rows, i);
    if (count > 1u) qsort(ordered, count, sizeof(*ordered), compare_sources);
    for (size_t i = 0u; i < count; ++i) {
        if (source->control && source->control->cancelled &&
            source->control->cancelled(source->control->opaque)) {
            snag_errno(ECANCELED);
            goto done;
        }
        json_t *row = ordered[i];
        uint64_t sequence = (uint64_t)json_integer_value(json_object_get(row, "seq"));
        if (fn(argument, NULL, sequence, snag_json_string(row, "type"),
                json_object_get(row, "data"), error, error_size) < 0) goto done;
    }
    rc = 0;
done:
    free(ordered);
    json_decref(selection.rows);
    json_decref(selection.seen);
    return rc;
}

/* This is lookup under the already verified full-prefix anchor, not another
 * semantic replay. Import markers supply only the old adjacent-IRC lookup seam. */
static int
walk_sources(void *opaque, const json_t *wanted, const char *prompt,
    snag_session_event_fn fn, void *argument, char *error, size_t error_size)
{
    const struct source_walk *source = opaque;
    if (source->access) {
        return walk_selected_sources(source, wanted, prompt, fn, argument, error, error_size);
    }
    struct snag_binary_identity identity;
    struct snag_binary_anchor anchor;
    if (source_header(source->fd, &identity, &anchor) < 0) return -1;
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    int rc = -1;
    while (anchor.end < source->verified->end) {
        if (source->control && source->control->cancelled &&
            source->control->cancelled(source->control->opaque)) {
            snag_fail(error, error_size, ECANCELED, "context source lookup cancelled");
            goto done;
        }
        struct snag_binary_batch batch;
        struct snag_binary_anchor next;
        int read = snag_binary_batch_read(source->fd, source->verified->end, &anchor,
            &scratch, &batch, &next);
        if (read != 0) {
            if (read > 0) snag_fail(error, error_size, EAGAIN, "context source prefix changed");
            goto done;
        }
        size_t cursor = SNAG_BINARY_BATCH_HEADER_SIZE;
        for (uint32_t i = 0u; i < batch.count; ++i) {
            struct snag_binary_record record;
            uint64_t seq;
            if (snag_binary_record_next(&batch, &cursor, &record, &seq) != 0) goto done;
            const char *type = NULL;
            json_t *data = NULL;
            if (record.kind != SNAG_BINARY_IRC_EVENT && record.kind != SNAG_BINARY_IRC_EVENT_V2 &&
                record.kind != SNAG_BINARY_LEGACY_CHECKPOINT &&
                record.kind != SNAG_BINARY_CHECKPOINT_RECEIPT) continue;
            if (snag_binary_checkpoint_record_project(source->fd, source->verified,
                source->access, &record, seq, &type, &data) < 0) goto done;
            int called = fn(argument, NULL, seq, type, data, error, error_size);
            json_decref(data);
            if (called < 0) goto done;
        }
        anchor = next;
    }
    if (anchor.end != source->verified->end || anchor.next_seq != source->verified->next_seq ||
        anchor.previous != source->verified->previous || anchor.turns != source->verified->turns ||
        memcmp(anchor.digest, source->verified->digest, sizeof(anchor.digest))) {
        errno = EAGAIN;
        goto done;
    }
    rc = 0;
 done:
    snag_buf_free(&scratch);
    return rc;
}

static bool
same_anchor(const struct snag_binary_anchor *left, const struct snag_binary_anchor *right)
{
    return left->end == right->end && left->next_seq == right->next_seq &&
        left->turns == right->turns && left->previous == right->previous &&
        !memcmp(left->digest, right->digest, sizeof(left->digest));
}

static int
checkpoint_cancelled(const struct snag_context_control *control, char *error, size_t error_size)
{
    if (control && control->cancelled && control->cancelled(control->opaque)) {
        return snag_fail(error, error_size, ECANCELED, "checkpoint materialization cancelled");
    }
    return 0;
}

int
snag_store_materialize_binary_context_checkpoint(struct snag_session *source,
    struct snag_session *restored, const struct snag_binary_checkpoint_frame *frame,
    const struct snag_binary_checkpoint_receipt *receipt,
    struct snag_binary_checkpoint_sources *sources, const struct snag_context_control *control,
    char *error, size_t error_size)
{
    if (!source || !restored || source == restored || !frame || !receipt ||
        source->log_fd < 0 || (source->lock_fd < 0 && !source->snapshot_read_only) ||
        source->pending_log ||
        !snag_hex_is_lower(source->id, SNAG_ID_HEX_LEN) ||
        restored->dir_fd >= 0 || restored->log_fd >= 0 || restored->lock_fd >= 0 ||
        restored->pending_log) {
        return snag_fail(error, error_size, EINVAL, "invalid native checkpoint target");
    }
    if ((frame->core.version != 2u && frame->core.version != 3u && frame->core.version != 4u &&
        frame->core.version != SNAG_BINARY_CORE_VERSION) || frame->provider.version != 1u ||
        frame->access.version != 1u || !frame->access.size) {
        return snag_fail(error, error_size, ENOTSUP, "native checkpoint sections unavailable");
    }
    struct snag_binary_checkpoint_encoder dimensions;
    if (snag_binary_checkpoint_encoder_init(&dimensions, frame) < 0 ||
        frame->generation != receipt->generation || dimensions.total != receipt->image_size ||
        !same_anchor(&frame->boundary, &receipt->boundary) ||
        memcmp(frame->image_digest, receipt->image_digest, sizeof(frame->image_digest))) {
        return snag_fail(error, error_size, EINVAL, "checkpoint receipt/boundary mismatch");
    }
    if (checkpoint_cancelled(control, error, error_size) < 0) return -1;
    snag_file_info before;
    if (snag_fstat(source->log_fd, &before) < 0) return -1;
    if (!S_ISREG(before.st_mode) || before.st_size < 0 ||
        frame->boundary.end > (uint64_t)before.st_size) {
        return snag_fail(error, error_size, EINVAL, "checkpoint lies outside the native source");
    }
    struct snag_binary_identity identity;
    struct snag_binary_anchor root;
    if (source_header(source->log_fd, &identity, &root) < 0) return -1;
    if (identity.created_ms != frame->identity.created_ms ||
        memcmp(identity.id, frame->identity.id, sizeof(identity.id))) {
        return snag_fail(error, error_size, EINVAL, "checkpoint source identity mismatch");
    }
    struct snag_binary_checkpoint_index access;
    if (snag_binary_checkpoint_index_decode(frame->access.data, frame->access.size,
            &identity, &frame->boundary, receipt->index_root, &access) < 0) return -1;
    struct snag_session *candidate = malloc(sizeof(*candidate));
    if (!candidate) {
        return snag_fail(error, error_size, ENOMEM, "cannot allocate native restore state");
    }
    snag_session_init(candidate);
    struct snag_binary_checkpoint_sources origins = {0};
    struct snag_context_capture *capture = NULL;
    json_t *recent = NULL;
    json_t *history = NULL;
    int rc = -1;
    if (checkpoint_cancelled(control, error, error_size) < 0 ||
        snag_binary_checkpoint_core_read(source->log_fd, frame, &access, candidate,
            &origins) < 0 ||
        snag_binary_checkpoint_processes_cursors(source->log_fd, &frame->boundary, &access,
            &origins, candidate, control ? control->cancelled : NULL,
            control ? control->opaque : NULL) < 0 ||
        checkpoint_cancelled(control, error, error_size) < 0) goto done;
    struct snag_binary_checkpoint_provider provider;
    if (snag_binary_checkpoint_provider_decode(frame->provider.data, frame->provider.size,
            &provider) < 0) goto done;
    if (strcmp(candidate->id, source->id) || provider.next_seq != candidate->next_seq ||
        provider.compact_seq != candidate->compact_seq ||
        provider.rebase_seq != candidate->context_rebase_seq) {
        snag_fail(error, error_size, EINVAL, "checkpoint core/provider boundary mismatch");
        goto done;
    }
    if (snag_binary_checkpoint_provider_read(source->log_fd, &frame->boundary, &access,
            frame->provider.data, frame->provider.size, control ? control->cancelled : NULL,
            control ? control->opaque : NULL, &recent, &history) < 0) goto done;
    capture = snag_context_capture_new(control);
    if (!capture) { snag_errno(ENOMEM); goto done; }
    if (snag_context_capture_seed(capture, recent, history) < 0 ||
        snag_context_capture_bind(&capture, candidate, error, error_size) < 0 ||
        checkpoint_cancelled(control, error, error_size) < 0) goto done;
    /* The last callback precedes this stamp check: a callback cannot rewrite
     * the source after its final validation and still expose adopted state. */
    snag_file_info after;
    if (snag_fstat(source->log_fd, &after) < 0) goto done;
    if (!snag_store_binary_prefix_stable(source, &before, &after)) {
        snag_fail(error, error_size, EAGAIN,
            "native checkpoint source changed during materialization");
        goto done;
    }
    snag_session_close(restored);
    *restored = *candidate;
    snag_session_init(candidate);
    if (sources) {
        snag_binary_checkpoint_sources_free(sources);
        *sources = origins;
        origins = (struct snag_binary_checkpoint_sources){0};
    }
    rc = 0;
done:
    snag_session_close(candidate);
    free(candidate);
    snag_binary_checkpoint_sources_free(&origins);
    snag_context_capture_free(capture);
    json_decref(recent);
    json_decref(history);
    return rc;
}

static int
resume_pinned(struct snag_session *source, struct snag_session *restored,
    const struct snag_binary_checkpoint_frame *frame,
    const struct snag_binary_checkpoint_receipt *receipt, const struct snag_binary_anchor *stop,
    const struct snag_binary_checkpoint_index *available, struct snag_binary_recovery *recovered,
    struct snag_binary_checkpoint_sources *sources, struct snag_buf *additional,
    const struct snag_context_control *control, char *error, size_t error_size)
{
    if (!source || !restored || source == restored || !frame || !receipt || !stop ||
        source->log_fd < 0 || (source->lock_fd < 0 && !source->snapshot_read_only) ||
        source->pending_log ||
        restored->dir_fd >= 0 || restored->log_fd >= 0 || restored->lock_fd >= 0 ||
        restored->pending_log) {
        return snag_fail(error, error_size, EINVAL, "invalid pinned native resume target");
    }
    if (frame->access.version != 1u || !frame->access.size) {
        return snag_fail(error, error_size, ENOTSUP, "pinned resume access unavailable");
    }
    struct snag_binary_checkpoint_index embedded;
    if (!available) {
        if (snag_binary_checkpoint_index_decode(frame->access.data, frame->access.size,
                &frame->identity, &frame->boundary, receipt->index_root, &embedded) < 0) return -1;
        available = &embedded;
    }
    unsigned char root[32];
    if (!same_anchor(&available->boundary, &frame->boundary) ||
        available->identity.created_ms != frame->identity.created_ms ||
        memcmp(available->identity.id, frame->identity.id, sizeof(frame->identity.id)) ||
        snag_binary_index_tree_root(&available->tree, root) < 0 ||
        memcmp(root, receipt->index_root, sizeof(root))) {
        return snag_fail(error, error_size, EINVAL, "pinned resume source frontier mismatch");
    }
    snag_file_info before;
    if (snag_fstat(source->log_fd, &before) < 0) return -1;
    struct snag_session *candidate = malloc(sizeof(*candidate));
    if (!candidate) {
        return snag_fail(error, error_size, ENOMEM, "cannot allocate native restore state");
    }
    snag_session_init(candidate);
    struct snag_binary_checkpoint_sources origins = {0};
    struct snag_context_capture *capture = NULL;
    struct snag_binary_recovery recovery = {0};
    int rc = -1;
    if (snag_store_materialize_binary_context_checkpoint(source, candidate, frame, receipt,
            &origins, control, error, error_size) < 0) goto done;
    if (snag_context_capture_take(candidate, control, &capture, error, error_size) < 0) goto done;
    if (snag_store_reduce_binary_suffix_prefix(source, candidate, &frame->boundary,
            stop, available, snag_context_capture_event, capture,
            control ? control->cancelled : NULL, control ? control->opaque : NULL,
            &recovery, &origins, error, error_size) < 0) goto done;
    if (snag_binary_checkpoint_processes_cursors(source->log_fd, &recovery.verified, available,
        &origins, candidate, control ? control->cancelled : NULL,
        control ? control->opaque : NULL) < 0) goto done;
    struct source_walk walk = {.fd = source->log_fd, .control = control,
        .verified = &recovery.verified, .access = available,
        .prompt_input =
            origins.texts.slots[SNAG_BINARY_TEXT_ACTIVE_PROMPT].original.target.sequence,
        .additional = additional};
    if (snag_context_capture_sources(capture, candidate, walk_sources, &walk,
            error, error_size) < 0 ||
        snag_context_capture_bind(&capture, candidate, error, error_size) < 0 ||
        checkpoint_cancelled(control, error, error_size) < 0) goto done;
    snag_file_info after;
    if (snag_fstat(source->log_fd, &after) < 0) goto done;
    if (!snag_store_binary_prefix_stable(source, &before, &after)) {
        snag_fail(error, error_size, EAGAIN, "native source changed during pinned resume");
        goto done;
    }
    snag_session_close(restored);
    *restored = *candidate;
    snag_session_init(candidate);
    if (sources) {
        snag_binary_checkpoint_sources_free(sources);
        *sources = origins;
        origins = (struct snag_binary_checkpoint_sources){0};
    }
    if (recovered) *recovered = recovery;
    rc = 0;
done:
    snag_session_close(candidate);
    free(candidate);
    snag_binary_checkpoint_sources_free(&origins);
    snag_context_capture_free(capture);
    return rc;
}

int
snag_store_resume_pinned_binary_context_checkpoint(struct snag_session *source,
    struct snag_session *restored, const struct snag_binary_checkpoint_frame *frame,
    const struct snag_binary_checkpoint_receipt *receipt, const struct snag_binary_anchor *stop,
    const struct snag_binary_checkpoint_index *available,
    struct snag_binary_checkpoint_sources *sources, const struct snag_context_control *control,
    char *error, size_t error_size)
{
    return resume_pinned(source, restored, frame, receipt, stop, available, NULL,
        sources, NULL, control, error, error_size);
}

void
snag_binary_context_admission_free(struct snag_binary_context_admission *admission)
{
    if (!admission) return;
    snag_buf_free(&admission->access);
    *admission = (struct snag_binary_context_admission){0};
}

static int
compare_entries(const void *left, const void *right)
{
    const struct snag_binary_index_entry *a = left;
    const struct snag_binary_index_entry *b = right;
    return (a->sequence > b->sequence) - (a->sequence < b->sequence);
}

static int
copy_access(struct snag_buf *out, const struct snag_binary_checkpoint_index *access,
    const struct snag_buf *additional)
{
    if (!additional->len)
        return snag_binary_checkpoint_index_copy(out, access);
    struct snag_buf entries = {.max = SIZE_MAX};
    int rc = -1;
    for (size_t i = 0u; i < access->entry_count; ++i) {
        struct snag_binary_index_entry entry;
        const unsigned char *bytes = access->entries + i * SNAG_BINARY_INDEX_ENTRY_SIZE;
        uint64_t sequence = 0u;
        for (size_t j = 0u; j < 8u; ++j) sequence |= (uint64_t)bytes[j] << (8u * j);
        if (snag_binary_index_entry_decode(bytes,
                SNAG_BINARY_INDEX_ENTRY_SIZE, &access->identity, sequence, &entry) < 0 ||
            snag_buf_append(&entries, &entry, sizeof(entry)) < 0) goto done;
    }
    if (snag_buf_append(&entries, additional->data, additional->len) < 0) goto done;
    size_t count = entries.len / sizeof(struct snag_binary_index_entry);
    qsort(entries.data, count, sizeof(struct snag_binary_index_entry), compare_entries);
    rc = snag_binary_checkpoint_index_encode(out, &access->identity, &access->boundary,
        &access->tree, (const struct snag_binary_index_entry *)entries.data, count);
done:
    snag_buf_free(&entries);
    return rc;
}

static int
admission_frontier(int fd, const struct snag_binary_checkpoint_frame *frame,
    const struct snag_binary_checkpoint_receipt *receipt,
    const struct snag_binary_checkpoint_index *supplement,
    const struct snag_binary_anchor *stop, struct snag_buf *scratch,
    struct snag_binary_context_admission *admission, const struct snag_buf *additional,
    const struct snag_context_control *control, char *error, size_t error_size)
{
    struct snag_binary_checkpoint_index embedded;
    if (!supplement) {
        if (snag_binary_checkpoint_index_decode(frame->access.data, frame->access.size,
                &frame->identity, &frame->boundary, receipt->index_root, &embedded) < 0) {
            return -1;
        }
        supplement = &embedded;
    }
    admission->access.max = SIZE_MAX;
    if (copy_access(&admission->access, supplement, additional) < 0 ||
        snag_binary_checkpoint_index_decode(admission->access.data, admission->access.len,
            &frame->identity, &frame->boundary, receipt->index_root,
            &admission->available) < 0) {
        return -1;
    }
    admission->tree = supplement->tree;
    struct snag_binary_anchor cursor = frame->boundary;
    while (cursor.end < stop->end) {
        if (checkpoint_cancelled(control, error, error_size) < 0) return -1;
        struct snag_binary_batch batch;
        struct snag_binary_anchor after;
        int rc = snag_binary_batch_read(fd, stop->end, &cursor, scratch, &batch, &after);
        if (rc < 0) return -1;
        if (rc || after.end > stop->end) return snag_errno(EINVAL);
        if (snag_binary_index_tree_append_batch(NULL, &admission->tree, &frame->identity,
                &cursor, &after, batch.data, batch.size) < 0) {
            return -1;
        }
        cursor = after;
    }
    if (!same_anchor(&cursor, stop) || admission->tree.count != stop->next_seq - 1u)
        return snag_errno(EINVAL);
    return checkpoint_cancelled(control, error, error_size);
}

int
snag_store_admit_binary_context_checkpoint(struct snag_session *source,
    struct snag_session *restored, const int images[2], uint64_t floor,
    const struct snag_binary_checkpoint_index *const available[2],
    struct snag_binary_recovery *recovery, struct snag_binary_checkpoint_sources *sources,
    struct snag_binary_context_admission *admission,
    const struct snag_context_control *control, char *error, size_t error_size)
{
    if (!source || !restored || source == restored || !images || !recovery ||
        images[0] < -1 || images[1] < -1 || source->log_fd < 0 ||
        (source->lock_fd < 0 && !source->snapshot_read_only) ||
        source->pending_log ||
        restored->dir_fd >= 0 || restored->log_fd >= 0 || restored->lock_fd >= 0 ||
        restored->pending_log || floor < SNAG_BINARY_HEADER_SIZE) {
        return snag_fail(error, error_size, EINVAL, "invalid native checkpoint admission");
    }
    snag_file_info before;
    if (snag_fstat(source->log_fd, &before) < 0) return -1;
    struct snag_binary_identity identity;
    struct snag_binary_recovery found = {0};
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_buf image = {.max = SIZE_MAX};
    struct snag_session *candidate = malloc(sizeof(*candidate));
    if (!candidate) {
        return snag_fail(error, error_size, ENOMEM, "cannot allocate native restore state");
    }
    snag_session_init(candidate);
    struct snag_binary_checkpoint_sources origins = {0};
    struct snag_binary_context_admission staged = {0};
    struct snag_buf additional = {.max = SIZE_MAX};
    int rc = -1;
    if (checkpoint_cancelled(control, error, error_size) < 0 ||
        snag_binary_journal_tail(source->log_fd, before.st_size, &scratch,
            &identity, &found.verified, &found.incomplete_tail_bytes) < 0 ||
        checkpoint_cancelled(control, error, error_size) < 0) goto done;
    if (floor > found.verified.end) {
        snag_fail(error, error_size, EINVAL, "native receipt window beyond committed tail");
        goto done;
    }
    unsigned char hashes[2][32];
    const unsigned char *keys[2] = {NULL, NULL};
    for (size_t i = 0u; i < 2u; ++i) {
        if (checkpoint_cancelled(control, error, error_size) < 0) goto done;
        if (images[i] < 0) continue;
        /* A fresh four-file bundle reserves the unused slot as an empty file. */
        snag_file_info image_stat;
        if (snag_fstat(images[i], &image_stat) < 0) goto done;
        if (S_ISREG(image_stat.st_mode) && !image_stat.st_size) continue;
        if (snag_binary_checkpoint_image_probe(images[i], hashes[i]) < 0) goto done;
        keys[i] = hashes[i];
    }
    struct snag_binary_checkpoint_receipt receipts[2];
    uint64_t sequences[2] = {0};
    int pinned = snag_binary_checkpoint_receipts_find(source->log_fd, &found.verified,
        floor, keys, &scratch, receipts, sequences, control ? control->cancelled : NULL,
        control ? control->opaque : NULL);
    if (pinned < 0) goto done;
    if (!pinned) {
        snag_fail(error, error_size, ENOENT, "no canonical checkpoint in native receipt window");
        goto done;
    }
    size_t slot = (pinned & 1) ? 0u : 1u;
    if ((pinned & 3) == 3 && sequences[1] > sequences[0]) slot = 1u;
    struct snag_binary_checkpoint_frame frame;
    struct snag_binary_recovery suffix = {0};
    if (snag_binary_checkpoint_image_read(images[slot], &identity, &receipts[slot], &image,
            &frame, control ? control->cancelled : NULL, control ? control->opaque : NULL) < 0 ||
        resume_pinned(source, candidate, &frame, &receipts[slot], &found.verified,
            available ? available[slot] : NULL, &suffix, &origins, &additional,
            control, error, error_size) < 0 ||
        checkpoint_cancelled(control, error, error_size) < 0) goto done;
    if (admission) {
        if (admission_frontier(source->log_fd, &frame, &receipts[slot],
                available ? available[slot] : NULL, &found.verified, &scratch,
                &staged, &additional, control, error, error_size) < 0) goto done;
        for (size_t i = 0u; i < 2u; ++i) {
            if (!(pinned & (1 << i))) continue;
            staged.generations[i] = receipts[i].generation;
            staged.sequences[i] = sequences[i];
        }
    }
    snag_file_info after;
    if (snag_fstat(source->log_fd, &after) < 0) goto done;
    if (!snag_store_binary_prefix_stable(source, &before, &after)) {
        snag_fail(error, error_size, EAGAIN, "native source changed during checkpoint admission");
        goto done;
    }
    found.batches = suffix.batches;
    snag_session_close(restored);
    *restored = *candidate;
    snag_session_init(candidate);
    if (sources) {
        snag_binary_checkpoint_sources_free(sources);
        *sources = origins;
        origins = (struct snag_binary_checkpoint_sources){0};
    }
    *recovery = found;
    if (admission) {
        snag_binary_context_admission_free(admission);
        *admission = staged;
        staged = (struct snag_binary_context_admission){0};
    }
    rc = 0;
done:
    snag_session_close(candidate);
    free(candidate);
    snag_binary_checkpoint_sources_free(&origins);
    snag_binary_context_admission_free(&staged);
    snag_buf_free(&additional);
    snag_buf_free(&scratch);
    snag_buf_free(&image);
    return rc;
}

/* Slot sequences were joined to canonical receipts during admission. Derive
 * their physical checkpoint clock from that same independently bounded suffix,
 * never from an image's claimed location or a legacy checkpoint seek pointer. */
static int
restore_checkpoint_clock(int fd, const struct snag_binary_context_admission *admission,
    const struct snag_binary_anchor *stop, struct snag_session *state)
{
    uint64_t sequence = admission->sequences[0] > admission->sequences[1] ?
        admission->sequences[0] : admission->sequences[1];
    struct snag_binary_anchor cursor = admission->available.boundary;
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    int rc = -1;
    while (cursor.end < stop->end) {
        struct snag_binary_batch batch;
        struct snag_binary_anchor after;
        int read = snag_binary_batch_read(fd, stop->end, &cursor, &scratch, &batch, &after);
        if (read < 0) goto done;
        if (read || after.end > stop->end) {
            snag_errno(EINVAL);
            goto done;
        }
        if (sequence >= cursor.next_seq && sequence < after.next_seq) {
            state->checkpoint_seq = sequence;
            state->checkpoint_offset = (int64_t)cursor.end;
            rc = 0;
            goto done;
        }
        cursor = after;
    }
    snag_errno(EINVAL);
done:
    snag_buf_free(&scratch);
    return rc;
}

static int
checkpoint_images_open(struct snag_session *session, int images[2],
    char *error, size_t error_size)
{
    const char *names[2] = {"checkpoint.0", "checkpoint.1"};
    for (size_t i = 0u; i < 2u; ++i) {
        images[i] = snag_open_read_security_at(session->dir_fd, names[i], false);
        if (images[i] < 0) {
            if (errno == ENOENT) continue;
            return -1;
        }
        if (snag_store_verify_private_fd(images[i], false, "native checkpoint",
                error, error_size) < 0) return -1;
    }
    return 0;
}

int
snag_store_recover_binary_context(struct snag_session *session,
    struct snag_context_capture **capture, const struct snag_context_control *control,
    char *error, size_t error_size)
{
    if (!session || !session->binary || session->dir_fd < 0 || session->log_fd < 0 ||
        session->lock_fd < 0 || session->pending_log || !capture || *capture) {
        return snag_fail(error, error_size, EINVAL, "invalid native provider recovery target");
    }
    if (error_size) error[0] = '\0';
    if (checkpoint_cancelled(control, error, error_size) < 0) return -1;
    struct snag_session *candidate = malloc(sizeof(*candidate));
    if (!candidate) return snag_errno(ENOMEM);
    snag_session_init(candidate);
    struct snag_binary_checkpoint_sources current = {0};
    struct snag_binary_index_tree tree;
    struct snag_binary_anchor through;
    struct snag_binary_recovery recovery = {0};
    struct snag_binary_context_admission admission = {0};
    int images[2] = {-1, -1};
    int rc = -1;
    if (snag_session_binary_checkpoint_capture(session, &through, &tree, &current,
            error, error_size) < 0) goto done;
    snag_file_info status;
    if (snag_fstat(session->log_fd, &status) < 0) goto done;
    if (status.st_size < 0 || (uint64_t)status.st_size != through.end) {
        snag_fail(error, error_size, EBUSY, "native provider recovery has unacknowledged bytes");
        goto done;
    }
    uint64_t window = (uint64_t)SNAG_BINARY_SUFFIX_TARGET + SNAG_BINARY_WIRE_BATCH_MAX;
    uint64_t floor = through.end > window ? through.end - window : SNAG_BINARY_HEADER_SIZE;
    if (floor < SNAG_BINARY_HEADER_SIZE) floor = SNAG_BINARY_HEADER_SIZE;
    if (checkpoint_images_open(session, images, error, error_size) < 0 ||
        snag_store_admit_binary_context_checkpoint(session, candidate, images, floor, NULL,
            &recovery, NULL, &admission, control, error, error_size) < 0) goto done;
    unsigned char expected_root[32], recovered_root[32];
    if (!same_anchor(&through, &recovery.verified) || strcmp(session->id, candidate->id) ||
        snag_binary_index_tree_root(&tree, expected_root) < 0 ||
        snag_binary_index_tree_root(&admission.tree, recovered_root) < 0 ||
        memcmp(expected_root, recovered_root, sizeof(expected_root))) {
        snag_fail(error, error_size, ESTALE, "native provider recovery boundary changed");
        goto done;
    }
    rc = snag_context_capture_take(candidate, control, capture, error, error_size);
done:
    if (rc < 0 && error_size && !error[0]) {
        snag_errorf(error, error_size, "cannot recover native provider context: %s",
            strerror(errno));
    }
    int saved = errno;
    snag_session_close(candidate);
    free(candidate);
    for (size_t i = 0u; i < 2u; ++i) {
        if (images[i] >= 0) (void)close(images[i]);
    }
    snag_binary_checkpoint_sources_free(&current);
    snag_binary_context_admission_free(&admission);
    errno = saved;
    return rc;
}

int
snag_store_load_binary_session(struct snag_session *session, enum snag_tail_policy policy,
    char *error, size_t error_size)
{
    if (!session || session->dir_fd < 0 || session->log_fd < 0 || session->binary ||
        session->pending_log || (session->lock_fd < 0 && !session->snapshot_read_only) ||
        (policy != SNAG_TAIL_REJECT && policy != SNAG_TAIL_TRUNCATE &&
            policy != SNAG_TAIL_IGNORE) ||
        (session->snapshot_read_only && policy != SNAG_TAIL_IGNORE)) {
        return snag_fail(error, error_size, EINVAL, "invalid native session open");
    }
    if (error_size) error[0] = '\0';
    snag_file_info before;
    if (snag_fstat(session->log_fd, &before) < 0 || !S_ISREG(before.st_mode) ||
        before.st_size < SNAG_BINARY_HEADER_SIZE) {
        return snag_fail(error, error_size, EINVAL, "invalid native journal size");
    }
    uint64_t window = (uint64_t)SNAG_BINARY_SUFFIX_TARGET + SNAG_BINARY_WIRE_BATCH_MAX;
    uint64_t floor = (uint64_t)before.st_size > window ?
        (uint64_t)before.st_size - window : SNAG_BINARY_HEADER_SIZE;
    if (floor < SNAG_BINARY_HEADER_SIZE) floor = SNAG_BINARY_HEADER_SIZE;
    int images[2] = {-1, -1};
    struct snag_session *candidate = malloc(sizeof(*candidate));
    if (!candidate)
        return snag_fail(error, error_size, ENOMEM, "cannot allocate native open state");
    snag_session_init(candidate);
    struct snag_binary_context_admission admission = {0};
    struct snag_binary_checkpoint_sources sources = {0};
    struct snag_binary_producer producer = {0};
    struct snag_binary_recovery recovery = {0};
    struct snag_binary_identity identity;
    struct snag_binary_anchor initial;
    int rc = -1;
    int saved = 0;
    struct snag_context_control control = {.cancelled = session->history_cancel,
        .opaque = session->history_cancel_opaque};
    if (control.cancelled && control.cancelled(control.opaque)) { errno = ECANCELED; goto done; }
    if (checkpoint_images_open(session, images, error, error_size) < 0) goto done;
    if (source_header(session->log_fd, &identity, &initial) < 0 ||
        snag_store_admit_binary_context_checkpoint(session, candidate, images, floor, NULL,
            &recovery, &sources, &admission, &control, error, error_size) < 0 ||
        restore_checkpoint_clock(session->log_fd, &admission, &recovery.verified, candidate) < 0 ||
        snag_binary_producer_restore(&producer, session->log_fd, &recovery.verified,
            &admission.available, &sources, candidate,
            control.cancelled, control.opaque) < 0) goto done;
    snag_file_info after;
    if (snag_fstat(session->log_fd, &after) < 0) goto done;
    if (!snag_store_binary_prefix_stable(session, &before, &after)) {
        snag_fail(error, error_size, EAGAIN, "native source changed during session open");
        goto done;
    }
    if (recovery.incomplete_tail_bytes) {
        if (policy == SNAG_TAIL_REJECT) {
            snag_fail(error, error_size, EINVAL, "native journal has an incomplete tail");
            goto done;
        }
        if (policy == SNAG_TAIL_TRUNCATE &&
            (snag_truncate(session->log_fd, (int64_t)recovery.verified.end) < 0 ||
                snag_sync_file(session->log_fd) < 0)) goto done;
    }
    /* Borrow descriptors while staging the owner. Only complete setup transfers
     * the opener's resource custody; cleanup must leave borrowed fds alive. */
    candidate->dir_fd = session->dir_fd;
    candidate->log_fd = session->log_fd;
    candidate->lock_fd = session->lock_fd;
    candidate->snapshot_read_only = session->snapshot_read_only;
    if (snag_session_bind_binary(candidate, &identity, &recovery.verified, &admission.tree,
            &producer, &sources, NULL, error, error_size) < 0 ||
        snag_session_binary_checkpoint_setup(candidate, candidate->dir_fd,
            admission.generations, admission.sequences, &admission.available,
            error, error_size) < 0) goto done;
    if (!candidate->snapshot_read_only) {
        int index = snag_open_private_append_at(candidate->dir_fd, "history.idx", false);
        if (index < 0 && errno == ENOENT)
            index = snag_open_private_append_at(candidate->dir_fd, "history.idx", true);
        if (index >= 0) {
            char index_error[128];
            if (snag_store_verify_private_fd(index, false, "native index", index_error,
                    sizeof(index_error)) < 0 ||
                snag_session_binary_index_adopt(candidate, index,
                    index_error, sizeof(index_error)) < 0) (void)close(index);
        }
    }
    candidate->history_cancel = session->history_cancel;
    candidate->history_cancel_opaque = session->history_cancel_opaque;
    candidate->dir_path = session->dir_path;
    session->dir_path = NULL;
    session->dir_fd = session->log_fd = session->lock_fd = -1;
    snag_session_close(session);
    *session = *candidate;
    snag_session_init(candidate);
    rc = 0;
done:
    saved = errno;
    if (rc < 0 && error_size && !error[0])
        snag_errorf(error, error_size, "cannot open native session: %s", strerror(errno));
    candidate->dir_fd = candidate->log_fd = candidate->lock_fd = -1;
    snag_session_close(candidate);
    free(candidate);
    for (size_t i = 0u; i < 2u; ++i)
        if (images[i] >= 0) (void)close(images[i]);
    snag_binary_producer_free(&producer);
    snag_binary_checkpoint_sources_free(&sources);
    snag_binary_context_admission_free(&admission);
    errno = saved;
    return rc;
}

/* Replaying the exact boundary is deliberately the independent semantic oracle.
 * Checksums alone cannot prove that an otherwise valid older control/reference
 * remains current, or that a saved seam includes every required event. */
static int
verify_checkpoint(int fd, const struct snag_binary_anchor *boundary,
    struct snag_session *state, struct snag_binary_checkpoint_sources *origins,
    const void *bytes, size_t size, const struct snag_context_control *control,
    char *error, size_t error_size)
{
    struct snag_binary_identity identity;
    struct snag_binary_anchor root;
    struct snag_binary_checkpoint_frame frame;
    if (source_header(fd, &identity, &root) < 0) return -1;
    if (snag_binary_checkpoint_frame_decode(bytes, size, &identity, boundary, &frame) != 0 ||
        (frame.core.version != 2u && frame.core.version != 3u && frame.core.version != 4u &&
            frame.core.version != SNAG_BINARY_CORE_VERSION) || frame.provider.version != 1u) {
        return snag_fail(error, error_size, EINVAL, "invalid native checkpoint frame");
    }
    struct snag_binary_checkpoint_provider view;
    if (snag_binary_checkpoint_provider_decode(frame.provider.data, frame.provider.size,
        &view) < 0) return snag_fail(error, error_size, EINVAL, "invalid provider recipe");
    const json_t *recent, *history;
    if (snag_context_capture_seam(state, &recent, &history) < 0) return -1;
    struct snag_buf core = {.max = SIZE_MAX}, provider = {.max = SIZE_MAX};
    struct snag_session *loaded = malloc(sizeof(*loaded));
    if (!loaded) return snag_errno(ENOMEM);
    snag_session_init(loaded);
    struct snag_binary_checkpoint_sources loaded_origins = {0};
    struct snag_context_capture *capture = NULL;
    json_t *events = NULL, *history_events = NULL;
    int rc = -1;
    if (snag_binary_checkpoint_core_encode_version(&core, origins, state,
        frame.core.version) < 0 ||
        snag_binary_checkpoint_provider_encode(&provider, state, recent, history) < 0) goto done;
    if (core.len != frame.core.size || provider.len != frame.provider.size ||
        memcmp(core.data, frame.core.data, core.len) ||
        memcmp(provider.data, frame.provider.data, provider.len)) {
        snag_fail(error, error_size, EINVAL, "checkpoint differs from its committed prefix");
        goto done;
    }
    /* Keep strict replay as the source/epoch authority, while materializing the
     * returned state from the candidate sections. Payload values are shared from
     * its verified canonical pool; no weaker inert record decoder is needed. */
    if (snag_binary_checkpoint_core_read(fd, &frame, NULL, loaded, &loaded_origins) < 0 ||
        snag_binary_checkpoint_provider_materialize(frame.provider.data, frame.provider.size,
            recent, history, &events, &history_events) < 0) goto done;
    capture = snag_context_capture_new(control);
    if (!capture) { errno = ENOMEM; goto done; }
    if (snag_context_capture_seed(capture, events, history_events) < 0 ||
        snag_context_capture_bind(&capture, loaded, error, error_size) < 0) goto done;
    snag_session_close(state);
    *state = *loaded;
    snag_session_init(loaded);
    snag_binary_checkpoint_sources_free(origins);
    *origins = loaded_origins;
    loaded_origins = (struct snag_binary_checkpoint_sources){0};
    rc = 0;
done:
    snag_session_close(loaded);
    free(loaded);
    snag_binary_checkpoint_sources_free(&loaded_origins);
    snag_context_capture_free(capture);
    json_decref(events);
    json_decref(history_events);
    snag_buf_free(&core);
    snag_buf_free(&provider);
    return rc;
}

static int
reconcile_context(struct snag_session *source, struct snag_session *restored,
    const struct snag_binary_anchor *prefix, const void *checkpoint, size_t checkpoint_size,
    bool suffix, struct snag_binary_recovery *recovery,
    struct snag_binary_checkpoint_sources *sources, const struct snag_context_control *control,
    char *error, size_t error_size)
{
    if (!source || !restored || source == restored || !recovery ||
        restored->dir_fd >= 0 || restored->log_fd >= 0 || restored->lock_fd >= 0 ||
        restored->pending_log) {
        return snag_fail(error, error_size, EINVAL, "invalid native context replay target");
    }
    snag_file_info before, after;
    if (source->log_fd < 0)
        return snag_fail(error, error_size, EINVAL, "cannot inspect native context source");
    if (snag_fstat(source->log_fd, &before) < 0)
        return snag_fail(error, error_size, errno, "cannot inspect native context source");
    struct snag_session *candidate = malloc(sizeof(*candidate));
    if (!candidate) return snag_errno(ENOMEM);
    struct snag_context_capture *capture = snag_context_capture_new(control);
    if (!capture) {
        free(candidate);
        return snag_fail(error, error_size, ENOMEM, "cannot capture provider context");
    }
    snag_session_init(candidate);
    struct snag_binary_checkpoint_sources origins = {0};
    int rc = prefix ?
        snag_store_reconcile_binary_prefix(source, candidate, prefix, snag_context_capture_event,
            capture, recovery, sources || checkpoint ? &origins : NULL, error, error_size) :
        snag_store_reconcile_binary(source, candidate, snag_context_capture_event, capture,
            recovery, sources || checkpoint ? &origins : NULL, error, error_size);
    struct source_walk walk = {.fd = source->log_fd, .control = control,
        .verified = &recovery->verified};
    if (rc == 0) {
        rc = snag_context_capture_sources(capture, candidate, walk_sources, &walk,
            error, error_size);
    }
    if (rc == 0) rc = snag_context_capture_bind(&capture, candidate, error, error_size);
    if (rc == 0 && checkpoint) {
        rc = verify_checkpoint(source->log_fd, &recovery->verified, candidate, &origins,
            checkpoint, checkpoint_size, control, error, error_size);
        if (rc == 0 && control && control->cancelled && control->cancelled(control->opaque)) {
            rc = snag_fail(error, error_size, ECANCELED, "checkpoint verification cancelled");
        }
    }
    if (rc == 0 && suffix) {
        uint64_t prefix_batches = recovery->batches;
        rc = snag_context_capture_take(candidate, control, &capture, error, error_size);
        if (rc == 0) {
            rc = snag_store_reduce_binary_suffix(source, candidate, &recovery->verified,
                snag_context_capture_event, capture, recovery, &origins, error, error_size);
            recovery->batches += prefix_batches;
        }
        if (rc == 0) rc = snag_context_capture_sources(capture, candidate, walk_sources,
            &walk, error, error_size);
        if (rc == 0) rc = snag_context_capture_bind(&capture, candidate, error, error_size);
        if (rc == 0 && control && control->cancelled && control->cancelled(control->opaque))
            rc = snag_fail(error, error_size, ECANCELED, "checkpoint suffix replay cancelled");
    }
    if (rc == 0 && snag_fstat(source->log_fd, &after) < 0)
        rc = snag_fail(error, error_size, errno, "cannot recheck native context source");
    if (rc == 0 && !snag_file_unchanged(&before, &after)) {
        rc = snag_fail(error, error_size, EAGAIN, "native source changed during context capture");
    }
    if (rc == 0) {
        snag_session_close(restored);
        *restored = *candidate;
        snag_session_init(candidate);
        if (sources) {
            snag_binary_checkpoint_sources_free(sources);
            *sources = origins;
            origins = (struct snag_binary_checkpoint_sources){0};
        }
    }
    if (rc < 0) recovery->incomplete_tail_bytes = 0u;
    snag_session_close(candidate);
    free(candidate);
    snag_binary_checkpoint_sources_free(&origins);
    snag_context_capture_free(capture);
    return rc;
}

int
snag_store_reconcile_binary_context(struct snag_session *source,
    struct snag_session *restored, struct snag_binary_recovery *recovery,
    struct snag_binary_checkpoint_sources *sources, const struct snag_context_control *control,
    char *error, size_t error_size)
{
    return reconcile_context(source, restored, NULL, NULL, 0u, false, recovery, sources, control,
        error, error_size);
}

int
snag_store_reconcile_binary_context_prefix(struct snag_session *source,
    struct snag_session *restored, const struct snag_binary_anchor *prefix,
    struct snag_binary_recovery *recovery, struct snag_binary_checkpoint_sources *sources,
    const struct snag_context_control *control, char *error, size_t error_size)
{
    if (!prefix) return snag_fail(error, error_size, EINVAL, "missing native context boundary");
    return reconcile_context(source, restored, prefix, NULL, 0u, false, recovery, sources, control,
        error, error_size);
}

int
snag_store_verify_binary_context_checkpoint(struct snag_session *source,
    struct snag_session *restored, const struct snag_binary_anchor *prefix,
    const void *checkpoint, size_t checkpoint_size, struct snag_binary_recovery *recovery,
    struct snag_binary_checkpoint_sources *sources, const struct snag_context_control *control,
    char *error, size_t error_size)
{
    if (!prefix || !checkpoint || !checkpoint_size)
        return snag_fail(error, error_size, EINVAL, "missing native checkpoint candidate");
    return reconcile_context(source, restored, prefix, checkpoint, checkpoint_size,
        false, recovery, sources, control, error, error_size);
}

int
snag_store_resume_binary_context_checkpoint(struct snag_session *source,
    struct snag_session *restored, const struct snag_binary_anchor *prefix,
    const void *checkpoint, size_t checkpoint_size, struct snag_binary_recovery *recovery,
    struct snag_binary_checkpoint_sources *sources, const struct snag_context_control *control,
    char *error, size_t error_size)
{
    if (!prefix || !checkpoint || !checkpoint_size)
        return snag_fail(error, error_size, EINVAL, "missing native checkpoint candidate");
    return reconcile_context(source, restored, prefix, checkpoint, checkpoint_size,
        true, recovery, sources, control, error, error_size);
}
