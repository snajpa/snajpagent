/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_import.h"
#include "fs.h"
#include "store_binary_legacy.h"
#include "store_binary_wire.h"
#include "store_binary_producer.h"
#include "store_binary_context.h"
#include "store_binary_index.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct import_writer {
    int fd;
    int source_fd;
    struct snag_binary_anchor anchor;
    struct snag_binary_record *records;
    size_t count, capacity;
    struct snag_buf payload;
    struct snag_binary_producer producer;
    struct snag_sha256 semantic;
    struct snag_buf *prepared_log;
    struct snag_buf index;
    struct snag_binary_identity identity;
    struct snag_binary_index_tree tree;
    uint64_t source_end, turns;
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
    struct snag_binary_anchor next;
    int rc = snag_binary_batch_encode(&bytes, &writer->anchor, writer->records,
        (uint32_t)writer->count, writer->turns, &next);
    if (rc == 0 && writer->prepared_log)
        rc = snag_binary_index_tree_append_batch(&writer->index, &writer->tree, &writer->identity,
            &writer->anchor, &next, bytes.data, bytes.len);
    struct snag_buf wire = {.max = SNAG_BINARY_WIRE_BATCH_MAX};
    if (rc == 0) rc = snag_binary_wire_encode(&wire, bytes.data, bytes.len);
    if (rc == 0) rc = snag_write_full(writer->fd, wire.data, wire.len);
    int code = errno;
    snag_buf_free(&wire);
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
    size_t added = SNAG_BINARY_RECORD_HEADER_SIZE + writer->producer.field.len;
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
    if (snag_buf_append(&writer->payload, writer->producer.field.data,
        writer->producer.field.len) < 0) {
        return -1;
    }
    record.size = writer->producer.field.len;
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
    snag_buf_free(&writer->producer.field);
    writer->producer.field = replacement;
    return 0;
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
    view.pending_log = writer->prepared_log;
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
    view.pending_log = writer->prepared_log;
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
        writer->identity = identity;
        if (writer->prepared_log) {
            unsigned char index_header[SNAG_BINARY_INDEX_HEADER_SIZE];
            snag_binary_index_header_encode(index_header, &identity);
            if (snag_buf_append(&writer->index, index_header, sizeof(index_header)) < 0) goto fail;
        }
    }
    snag_buf_reset(&writer->producer.field);
    struct snag_binary_record record = {.version = 1u, .timestamp_ms = state->last_time_ms};
    if (!strcmp(type, "session_checkpoint")) {
        struct snag_binary_legacy_checkpoint checkpoint = {
            .start = writer->source_end, .end = (uint64_t)state->log_end};
        hex_bytes(checkpoint.digest, state->prev_sha256, sizeof(checkpoint.digest));
        if (snag_binary_legacy_checkpoint_encode(&writer->producer.field, &checkpoint) < 0) {
            goto fail;
        }
        record.kind = SNAG_BINARY_LEGACY_CHECKPOINT;
        record.flags = SNAG_BINARY_RECORD_OPTIONAL;
    } else {
        enum snag_binary_kind kind;
        if (snag_binary_legacy_encode(&writer->producer.field, type, data, &kind) < 0) goto fail;
        record.kind = (uint16_t)kind;
        record.version = snag_binary_event_version(kind);
        record.payload = writer->producer.field.data;
        record.size = writer->producer.field.len;
        if (kind == SNAG_BINARY_VOICE_TRANSFER_ADOPTED &&
            reference_voice(writer, state, &record, error, error_size) < 0) goto fail;
        if ((kind == SNAG_BINARY_TOOL_FINISHED || kind == SNAG_BINARY_PROCESS_CLOSED) &&
            reference_result(writer, state, &record, error, error_size) < 0) goto fail;
        record.payload = writer->producer.field.data;
        record.size = writer->producer.field.len;
        if (snag_binary_producer_reference(&writer->producer, state, sequence,
            &record, data) < 0) goto fail;
    }
    if (sequence != writer->anchor.next_seq + writer->count ||
        state->log_end <= 0 || (uint64_t)state->log_end <= writer->source_end) {
        errno = EINVAL;
        goto fail;
    }
    /* Flush uses the preceding event's turn count. Update only after append. */
    if (append_record(writer, record) < 0) goto fail;
    writer->source_end = (uint64_t)state->log_end;
    writer->turns = state->turn_count;
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
        .payload = {.max = SNAG_MAX_EVENT_LINE},
        .producer.field = {.max = SNAG_MAX_EVENT_LINE}};
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
        snag_binary_producer_free(&writer.producer);
        snag_session_close(&native);
        snag_session_close(&legacy);
        free(writer.records);
        snag_buf_free(&writer.payload);
        errno = code;
    }
    return rc;
}

int
snag_store_seed_binary_session(struct snag_session *prepared, struct snag_session *target,
    struct snag_buf *index, char *error, size_t error_size)
{
    if (!prepared || !target || !index || index->len || prepared == target ||
        !prepared->pending_log ||
        prepared->binary || prepared->snapshot_read_only || prepared->dir_fd >= 0 ||
        prepared->log_fd >= 0 ||
        prepared->lock_fd >= 0 || !snag_hex_is_lower(prepared->id, SNAG_ID_HEX_LEN) ||
        target->pending_log || target->binary || target->snapshot_read_only || target->id[0] ||
        target->on_commit || target->on_commit_free || target->on_commit_opaque ||
        target->on_checkpoint || target->checkpoint_state || target->checkpoint_context ||
        target->log_fd < 0 || target->lock_fd < 0 || target->next_seq != 1u) {
        return snag_fail(error, error_size, EINVAL, "invalid prepared native seed owners");
    }
    snag_file_info output;
    snag_file_info lock;
    if (snag_fstat(target->log_fd, &output) < 0 || !S_ISREG(output.st_mode) ||
        output.st_size != 0 || snag_fstat(target->lock_fd, &lock) < 0 ||
        !S_ISREG(lock.st_mode) ||
        (output.st_dev == lock.st_dev && output.st_ino == lock.st_ino)) {
        return snag_fail(error, error_size, EINVAL,
            "native seed requires a separate empty journal");
    }
    if (snag_seek(target->log_fd, 0, SEEK_SET) < 0)
        return snag_fail(error, error_size, errno, "cannot position native seed journal");
    struct import_writer writer = {.fd = target->log_fd, .source_fd = -1,
        .prepared_log = prepared->pending_log, .payload = {.max = SNAG_MAX_EVENT_LINE},
        .index = {.max = index->max},
        .producer.field = {.max = SNAG_MAX_EVENT_LINE}};
    snag_sha256_init(&writer.semantic);
    struct snag_session source;
    struct snag_session candidate;
    struct snag_session verified;
    snag_session_init(&source);
    snag_session_init(&candidate);
    snag_session_init(&verified);
    memcpy(source.id, prepared->id, sizeof(source.id));
    source.log_fd = target->log_fd;
    source.lock_fd = target->lock_fd;
    struct snag_binary_recovery recovery;
    struct snag_binary_checkpoint_sources sources = {0};
    int rc = snag_session_each_event(prepared, import_event, &writer, error, error_size);
    if (rc < 0) goto done;
    if (writer.anchor.next_seq + writer.count != prepared->next_seq ||
        writer.source_end != (uint64_t)prepared->log_end || !writer.source_end) {
        rc = snag_fail(error, error_size, EINVAL, "native seed does not cover prepared events");
        goto done;
    }
    if (flush_batch(&writer) < 0 || snag_sync_file(target->log_fd) < 0) {
        rc = snag_fail(error, error_size, errno, "cannot finish native seed journal");
        goto done;
    }
    struct snag_sha256 semantic;
    snag_sha256_init(&semantic);
    rc = snag_store_reconcile_binary(&source, &verified, semantic_event, &semantic,
        &recovery, NULL, error, error_size);
    if (rc < 0) goto done;
    unsigned char expected[32];
    unsigned char actual[32];
    snag_sha256_final(&writer.semantic, expected);
    snag_sha256_final(&semantic, actual);
    if (recovery.incomplete_tail_bytes || memcmp(expected, actual, sizeof(actual)) ||
        compare_core(prepared, &verified) < 0) {
        rc = snag_fail(error, error_size, EINVAL, "native seed differs from its prepared source");
        goto done;
    }
    rc = snag_store_reconcile_binary_context(&source, &candidate, &recovery, &sources,
        NULL, error, error_size);
    if (rc < 0) goto done;
    candidate.log_fd = target->log_fd;
    candidate.lock_fd = target->lock_fd;
    rc = snag_session_bind_binary(&candidate, &writer.identity, &recovery.verified,
        &writer.tree, &writer.producer, &sources, NULL, error, error_size);
    if (rc < 0) goto done;
    candidate.dir_fd = target->dir_fd;
    candidate.dir_path = target->dir_path;
    *target = candidate;
    snag_session_init(&candidate);
    snag_buf_free(index);
    *index = writer.index;
    writer.index = (struct snag_buf){0};
done:
    {
        int saved = errno;
        candidate.log_fd = candidate.lock_fd = -1;
        snag_session_close(&candidate);
        snag_session_close(&verified);
        snag_binary_checkpoint_sources_free(&sources);
        snag_binary_producer_free(&writer.producer);
        free(writer.records);
        snag_buf_free(&writer.payload);
        snag_buf_free(&writer.index);
        errno = saved;
    }
    return rc;
}
