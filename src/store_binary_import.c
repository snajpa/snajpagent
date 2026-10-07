/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_import.h"
#include "context.h"
#include "fs.h"
#include "store_binary_legacy.h"
#include "store_binary_wire.h"
#include "store_binary_producer.h"
#include "store_binary_context.h"
#include "store_binary_index.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int
bootstrap_native_checkpoint(struct snag_session *session, int index_fd,
    const struct snag_context_control *control, char *error, size_t error_size)
{
    struct snag_binary_anchor boundary;
    struct snag_binary_index_tree tree;
    struct snag_binary_checkpoint_sources sources = {0};
    struct snag_binary_checkpoint_access_plan plan = {0};
    struct snag_buf empty = {.max = SIZE_MAX};
    struct snag_buf provider = {.max = SIZE_MAX};
    struct snag_buf selected = {.max = SIZE_MAX};
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    unsigned char root[32];
    struct snag_binary_identity identity;
    struct snag_binary_anchor initial;
    int rc = -1;
    if (snag_session_binary_checkpoint_capture(session, &boundary, &tree, &sources,
        error, error_size) < 0) goto out;
    ssize_t got = snag_pread(session->log_fd, header, sizeof(header), 0);
    if (got != sizeof(header)) {
        if (got >= 0) errno = EIO;
        goto out;
    }
    struct snag_binary_checkpoint_index available;
    if (snag_binary_header_decode(header, sizeof(header), &identity, &initial) < 0 ||
        snag_binary_index_tree_root(&tree, root) < 0 ||
        snag_binary_checkpoint_index_encode(&empty, &identity, &boundary, &tree, NULL, 0u) < 0 ||
        snag_binary_checkpoint_index_decode(empty.data, empty.len, &identity,
            &boundary, root, &available) < 0) goto out;
    const json_t *recent = NULL;
    const json_t *history = NULL;
    if (snag_context_capture_seam(session, &recent, &history) < 0 ||
        snag_binary_checkpoint_provider_encode(&provider, session, recent, history) < 0 ||
        snag_binary_checkpoint_access_plan_build(&plan, &boundary, &sources, session,
            provider.data, provider.len, NULL, NULL) < 0 ||
        snag_binary_checkpoint_access_plan_read(session->log_fd, index_fd, &plan, &available,
            &tree, control ? control->cancelled : NULL,
            control ? control->opaque : NULL, &selected) < 0) goto out;
    struct snag_binary_checkpoint_index captured;
    uint64_t generations[2] = {0u, 0u};
    uint64_t sequences[2] = {0u, 0u};
    if (snag_binary_checkpoint_index_decode(selected.data, selected.len, &identity,
            &boundary, root, &captured) < 0 ||
        snag_session_binary_checkpoint_setup(session, session->dir_fd, generations, sequences,
            &captured, error, error_size) < 0 ||
        snag_session_checkpoint(session, error, error_size) < 0 ||
        snag_session_binary_index_status(session, error, error_size) < 0 ||
        snag_session_binary_checkpoint_capture(session, &boundary, &tree, &sources,
            error, error_size) < 0 ||
        snag_binary_index_header_read(index_fd, &identity) < 0 ||
        snag_binary_index_tree_root(&tree, root) < 0) goto out;
    /* Cache health alone is not lookup proof. Check the newly acknowledged root. */
    struct snag_binary_index_tree loaded;
    if (snag_binary_index_tree_load(index_fd, &identity, tree.count, root, &loaded) < 0) goto out;
    if (memcmp(&tree, &loaded, sizeof(tree))) {
        (void)snag_fail(error, error_size, EBADMSG, "native creation cache frontier mismatch");
        goto out;
    }
    rc = 0;
out:
    if (rc < 0 && error && error_size && !error[0])
        (void)snag_errorf(error, error_size, "cannot bootstrap native checkpoint: %s",
            strerror(errno ? errno : EIO));
    int saved = errno;
    snag_binary_checkpoint_access_plan_free(&plan);
    snag_binary_checkpoint_sources_free(&sources);
    snag_buf_free(&empty);
    snag_buf_free(&provider);
    snag_buf_free(&selected);
    errno = saved;
    return rc;
}

static int
native_name_available(struct snag_store *store, const char *id, char *error, size_t error_size)
{
    snag_file_info st;
    if (!snag_lstat_at(store->sessions_fd, id, &st))
        return snag_fail(error, error_size, EEXIST, "session name already exists: %s", id);
    if (errno != ENOENT)
        return snag_errorf(error, error_size, "cannot inspect session name: %s", strerror(errno));
    return 0;
}

int
snag_store_persist_binary_session(struct snag_store *store, struct snag_session *prepared,
    char *error, size_t error_size)
{
    if (!store || store->sessions_fd < 0 || !store->root_path || !prepared ||
        !prepared->pending_log || prepared->binary || prepared->snapshot_read_only ||
        prepared->log_end < 0 || (uint64_t)prepared->log_end != prepared->pending_log->len ||
        prepared->dir_fd >= 0 || prepared->log_fd >= 0 || prepared->lock_fd >= 0 ||
        !snag_hex_is_lower(prepared->id, SNAG_ID_HEX_LEN))
        return snag_fail(error, error_size, EINVAL, "invalid native creation owners");
    if (error && error_size) error[0] = '\0';
    struct snag_directory_lock names = {.fd = -1};
    struct snag_session *sessions = malloc(2u * sizeof(*sessions));
    if (!sessions) return snag_errno(ENOMEM);
    struct snag_session *candidate = &sessions[0];
    struct snag_session *old = &sessions[1];
    snag_session_init(candidate);
    struct snag_buf index = {.max = SIZE_MAX};
    char nonce[SNAG_ID_HEX_LEN + 1u];
    char stage[2u * SNAG_ID_HEX_LEN + sizeof(".creating--")];
    char *parent = NULL;
    char *final_path = NULL;
    bool created = false;
    bool published = false;
    int index_fd = -1;
    int rc = -1;
    int final_error = 0;
    if (snag_directory_lock_acquire(store->sessions_fd, &names) < 0) {
        (void)snag_errorf(error, error_size, "cannot lock session names: %s", strerror(errno));
        goto out;
    }
    if (native_name_available(store, prepared->id, error, error_size) < 0) goto out;
    if (snag_directory_lock_release(&names) < 0) goto out;
    if (snag_random_id(nonce) < 0) goto out;
    (void)snprintf(stage, sizeof(stage), ".creating-%s-%s", prepared->id, nonce);
    parent = snag_path_join(store->root_path, "sessions");
    if (!parent) goto out;
    final_path = snag_path_join(parent, prepared->id);
    candidate->dir_path = snag_path_join(parent, stage);
    if (!final_path || !candidate->dir_path) goto out;
    if (snag_mkdir_private_at(store->sessions_fd, stage) < 0) goto out;
    created = true;
    candidate->dir_fd = snag_open_read_security_at(store->sessions_fd, stage, true);
    if (candidate->dir_fd < 0 || snag_store_verify_private_fd(candidate->dir_fd, true,
            "provisional native session directory", error, error_size) < 0) goto out;
    candidate->lock_fd = snag_create_private_at(candidate->dir_fd, "lock", true);
    if (candidate->lock_fd < 0 || snag_lock_file(candidate->lock_fd, false) < 0 ||
        snag_sync_file(candidate->lock_fd) < 0) goto out;
    candidate->log_fd = snag_create_private_at(candidate->dir_fd, "journal.bin", true);
    if (candidate->log_fd < 0 ||
        snag_store_seed_binary_session(prepared, candidate, &index, error, error_size) < 0)
        goto out;
    index_fd = snag_create_private_at(candidate->dir_fd, "history.idx", true);
    if (index_fd < 0 || snag_write_full(index_fd, index.data, index.len) < 0 ||
        snag_sync_file(index_fd) < 0) goto out;
    const char *slots[2] = {"checkpoint.0", "checkpoint.1"};
    for (size_t i = 0u; i < 2u; ++i) {
        int fd = snag_create_private_at(candidate->dir_fd, slots[i], true);
        if (fd < 0) goto out;
        int synced = snag_sync_file(fd);
        int saved = errno;
        (void)close(fd);
        errno = saved;
        if (synced < 0) goto out;
    }
    int query_fd = index_fd;
    if (snag_session_binary_index_adopt(candidate, index_fd, error, error_size) < 0) goto out;
    index_fd = -1;
    if (bootstrap_native_checkpoint(candidate, query_fd, NULL, error, error_size) < 0 ||
        snag_sync_file(query_fd) < 0) goto out;
    if (snag_directory_lock_acquire(store->sessions_fd, &names) < 0 ||
        native_name_available(store, prepared->id, error, error_size) < 0 ||
        snag_rename_at(store->sessions_fd, stage, store->sessions_fd, prepared->id) < 0)
        goto out;
    published = true;
    int unlock_error = snag_directory_lock_release(&names) < 0 ? errno : 0;
    free(candidate->dir_path);
    candidate->dir_path = final_path;
    final_path = NULL;
    *old = *prepared;
    *prepared = *candidate;
    snag_session_init(candidate);
    snag_session_close(old);
    /* The public name now belongs to the native owner even if durability fails. */
    int synced = snag_sync_dir(store->sessions_fd);
    if (synced || unlock_error) {
        int why = synced < 0 ? errno : synced > 0 ? ENOTSUP : unlock_error;
        (void)snag_fail(error, error_size, why,
            "session %s published; parent directory durability uncertain: %s",
            prepared->id, strerror(why));
        goto out;
    }
    rc = 0;
out:
    final_error = errno ? errno : EIO;
    if (rc < 0 && error && error_size && !error[0])
        (void)snag_errorf(error, error_size, "native session creation failed: %s",
            strerror(final_error));
    if (names.fd >= 0) (void)snag_directory_lock_release(&names);
    if (rc < 0 && created && !published) {
        const char *message = error && error_size && error[0] ? error : strerror(final_error);
        char *reason = strdup(message);
        if (reason) {
            (void)snag_errorf(error, error_size, "%s; provisional native session: %s",
                reason, candidate->dir_path);
            free(reason);
        }
    }
    if (index_fd >= 0) (void)close(index_fd);
    snag_session_close(candidate);
    free(sessions);
    snag_buf_free(&index);
    free(parent);
    free(final_path);
    errno = final_error;
    return rc;
}

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
    bool indexed, stream_index;
    int index_fd;
    const struct snag_context_control *control;
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
    if (rc == 0 && writer->indexed)
        rc = snag_binary_index_tree_append_batch(&writer->index, &writer->tree, &writer->identity,
            &writer->anchor, &next, bytes.data, bytes.len);
    if (rc == 0 && writer->stream_index) {
        rc = snag_write_full(writer->index_fd, writer->index.data, writer->index.len);
        if (!rc) snag_buf_reset(&writer->index);
    }
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

struct stopped_semantic {
    struct snag_sha256 hash;
    const struct snag_context_control *control;
};

static int
stopped_semantic_event(void *opaque, const struct snag_session *state, uint64_t sequence,
    const char *type, const json_t *data, char *error, size_t error_size)
{
    struct stopped_semantic *read = opaque;
    if (read->control && read->control->cancelled &&
        read->control->cancelled(read->control->opaque)) {
        return snag_fail(error, error_size, ECANCELED, "stopped native verification cancelled");
    }
    return semantic_event(&read->hash, state, sequence, type, data, error, error_size);
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
    if (writer->control && writer->control->cancelled &&
        writer->control->cancelled(writer->control->opaque)) {
        return snag_fail(error, error_size, ECANCELED, "stopped legacy import cancelled");
    }
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
        if (writer->indexed) {
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
    struct snag_session *sessions = malloc(3u * sizeof(*sessions));
    if (!sessions) return snag_errno(ENOMEM);
    struct snag_session *legacy = &sessions[0];
    struct snag_session *native = &sessions[1];
    struct snag_session *staged = &sessions[2];
    snag_session_init(legacy);
    snag_session_init(native);
    snag_session_init(staged);
    memcpy(staged->id, source->id, sizeof(staged->id));
    /* Borrow descriptors only for the read-only verifier; never close them. */
    staged->log_fd = destination;
    staged->lock_fd = source->lock_fd;
    struct import_writer writer = {.fd = destination, .source_fd = source->log_fd,
        .payload = {.max = SNAG_MAX_EVENT_LINE},
        .producer.field = {.max = SNAG_MAX_EVENT_LINE}};
    snag_sha256_init(&writer.semantic);
    int rc = snag_store_reconcile_legacy(source, legacy, import_event, &writer,
        &result->legacy, error, error_size);
    if (rc < 0) goto out;
    if (flush_batch(&writer) < 0) {
        rc = snag_fail(error, error_size, errno, "cannot finish staged native journal");
        goto out;
    }
    snag_sha256_final(&writer.semantic, result->semantic_digest);
    memcpy(result->source_sha256, legacy->prev_sha256, sizeof(result->source_sha256));
    struct snag_sha256 semantic;
    snag_sha256_init(&semantic);
    rc = snag_store_reconcile_binary(staged, native, semantic_event, &semantic,
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
    if (compare_core(legacy, native) < 0) {
        rc = snag_fail(error, error_size, errno, "cannot verify staged native core state");
        goto out;
    }
    if (snag_fstat(source->log_fd, &after) < 0 || !snag_file_unchanged(&before, &after)) {
        rc = snag_fail(error, error_size, EAGAIN,
            "legacy source changed during staging verification");
        goto out;
    }
    snag_session_close(restored);
    *restored = *native;
    result->sources = sources;
    sources = (struct snag_binary_checkpoint_sources){0};
    snag_session_init(native);
out:
    {
        int code = errno;
        snag_binary_checkpoint_sources_free(&sources);
        snag_binary_producer_free(&writer.producer);
        snag_session_close(native);
        snag_session_close(legacy);
        free(sessions);
        free(writer.records);
        snag_buf_free(&writer.payload);
        errno = code;
    }
    return rc;
}

int
snag_store_stage_binary_session(struct snag_session *source, struct snag_session *target,
    int index_fd, struct snag_binary_import_result *result,
    const struct snag_context_control *control, char *error, size_t error_size)
{
    if (!source || !target || source == target || !result || index_fd < 0 ||
        source->log_fd < 0 || source->lock_fd < 0 || source->pending_log || source->binary ||
        target->log_fd < 0 || target->lock_fd < 0 || target->pending_log || target->binary ||
        target->snapshot_read_only || target->id[0] ||
        !snag_hex_is_lower(source->id, SNAG_ID_HEX_LEN)) {
        return snag_fail(error, error_size, EINVAL, "invalid stopped native stage owners");
    }
    *result = (struct snag_binary_import_result){0};
    if (control && control->cancelled && control->cancelled(control->opaque))
        return snag_fail(error, error_size, ECANCELED, "stopped native staging cancelled");
    snag_file_info before, after, journal, index, lock, source_lock;
    if (snag_fstat(source->log_fd, &before) < 0 || !S_ISREG(before.st_mode) ||
        before.st_size < 0 || snag_fstat(target->log_fd, &journal) < 0 ||
        snag_fstat(index_fd, &index) < 0 || snag_fstat(target->lock_fd, &lock) < 0 ||
        !S_ISREG(journal.st_mode) || journal.st_size != 0 || !S_ISREG(index.st_mode) ||
        index.st_size != 0 || !S_ISREG(lock.st_mode) ||
        snag_fstat(source->lock_fd, &source_lock) < 0 ||
        (journal.st_dev == source_lock.st_dev && journal.st_ino == source_lock.st_ino) ||
        (index.st_dev == source_lock.st_dev && index.st_ino == source_lock.st_ino) ||
        (lock.st_dev == source_lock.st_dev && lock.st_ino == source_lock.st_ino) ||
        (journal.st_dev == before.st_dev && journal.st_ino == before.st_ino) ||
        (index.st_dev == before.st_dev && index.st_ino == before.st_ino) ||
        (journal.st_dev == index.st_dev && journal.st_ino == index.st_ino) ||
        (journal.st_dev == lock.st_dev && journal.st_ino == lock.st_ino) ||
        (index.st_dev == lock.st_dev && index.st_ino == lock.st_ino)) {
        return snag_fail(error, error_size, EINVAL, "native stage needs separate empty files");
    }
    if (snag_store_verify_private_fd(target->log_fd, false, "staged native journal",
            error, error_size) < 0 ||
        snag_store_verify_private_fd(index_fd, false, "staged native index",
            error, error_size) < 0 ||
        snag_seek(target->log_fd, 0, SEEK_SET) < 0 || snag_seek(index_fd, 0, SEEK_SET) < 0) {
        return -1;
    }
    struct snag_session *sessions = malloc(4u * sizeof(*sessions));
    if (!sessions) return snag_errno(ENOMEM);
    struct snag_session *legacy = &sessions[0];
    struct snag_session *verified = &sessions[1];
    struct snag_session *candidate = &sessions[2];
    struct snag_session *view = &sessions[3];
    snag_session_init(legacy);
    snag_session_init(verified);
    snag_session_init(candidate);
    snag_session_init(view);
    memcpy(view->id, source->id, sizeof(view->id));
    view->log_fd = target->log_fd;
    view->lock_fd = target->lock_fd;
    struct snag_binary_checkpoint_sources sources = {0};
    struct import_writer writer = {.fd = target->log_fd, .source_fd = source->log_fd,
        .indexed = true, .stream_index = true, .index_fd = index_fd, .control = control,
        .index = {.max = SNAG_BINARY_INDEX_TREE_BATCH_MAX + SNAG_BINARY_INDEX_HEADER_SIZE},
        .payload = {.max = SNAG_MAX_EVENT_LINE},
        .producer.field = {.max = SNAG_MAX_EVENT_LINE}};
    snag_sha256_init(&writer.semantic);
    int rc = snag_store_reconcile_legacy(source, legacy, import_event, &writer,
        &result->legacy, error, error_size);
    if (rc < 0) goto done;
    if (flush_batch(&writer) < 0 || snag_sync_file(target->log_fd) < 0 ||
        snag_sync_file(index_fd) < 0) {
        rc = snag_fail(error, error_size, errno, "cannot finish stopped native stage");
        goto done;
    }
    snag_sha256_final(&writer.semantic, result->semantic_digest);
    memcpy(result->source_sha256, legacy->prev_sha256, sizeof(result->source_sha256));
    struct stopped_semantic semantic = {.control = control};
    snag_sha256_init(&semantic.hash);
    rc = snag_store_reconcile_binary(view, verified, stopped_semantic_event, &semantic,
        &result->native, &sources, error, error_size);
    if (rc < 0) goto done;
    unsigned char actual[32];
    snag_sha256_final(&semantic.hash, actual);
    if (result->native.incomplete_tail_bytes ||
        memcmp(actual, result->semantic_digest, sizeof(actual)) ||
        compare_core(legacy, verified) < 0) {
        rc = snag_fail(error, error_size, EINVAL,
            "stopped native stage differs from legacy source");
        goto done;
    }
    rc = snag_store_reconcile_binary_context(view, candidate, &result->native,
        &sources, control, error, error_size);
    if (rc < 0) goto done;
    if (compare_core(legacy, candidate) < 0 || snag_fstat(source->log_fd, &after) < 0 ||
        !snag_file_unchanged(&before, &after)) {
        rc = snag_fail(error, error_size, EAGAIN, "legacy source changed during native staging");
        goto done;
    }
    unsigned char root[32];
    struct snag_binary_index_tree loaded;
    if (snag_binary_index_tree_root(&writer.tree, root) < 0 ||
        snag_binary_index_tree_load(index_fd, &writer.identity, writer.tree.count, root,
            &loaded) < 0 || memcmp(&writer.tree, &loaded, sizeof(loaded))) {
        rc = snag_fail(error, error_size, EBADMSG, "stopped native index frontier mismatch");
        goto done;
    }
    candidate->log_fd = target->log_fd;
    candidate->lock_fd = target->lock_fd;
    rc = snag_session_bind_binary(candidate, &writer.identity, &result->native.verified,
        &writer.tree, &writer.producer, &sources, NULL, error, error_size);
    if (rc < 0) goto done;
    candidate->dir_fd = target->dir_fd;
    candidate->dir_path = target->dir_path;
    *target = *candidate;
    snag_session_init(candidate);
    result->sources = sources;
    sources = (struct snag_binary_checkpoint_sources){0};
done:
    {
        int saved = errno;
        candidate->log_fd = candidate->lock_fd = -1;
        snag_session_close(candidate);
        snag_session_close(verified);
        snag_session_close(legacy);
        free(sessions);
        snag_binary_checkpoint_sources_free(&sources);
        snag_binary_producer_free(&writer.producer);
        free(writer.records);
        snag_buf_free(&writer.payload);
        snag_buf_free(&writer.index);
        errno = saved;
    }
    return rc;
}

int
snag_store_convert_binary_directory(struct snag_session *source,
    struct snag_binary_import_result *result, const struct snag_context_control *control,
    char *error, size_t error_size)
{
    if (!source || !result || source->dir_fd < 0 || !source->dir_path ||
        source->log_fd < 0 || source->lock_fd < 0 || source->binary || source->pending_log ||
        source->snapshot_read_only) {
        return snag_fail(error, error_size, EINVAL, "invalid stopped conversion directory");
    }
    *result = (struct snag_binary_import_result){0};
    if (error && error_size) error[0] = '\0';
    snag_file_info before, path;
    if (snag_fstat(source->log_fd, &before) < 0) return -1;
    if (!snag_lstat_at(source->dir_fd, "journal.bin", &path) || errno != ENOENT)
        return snag_fail(error, error_size, EEXIST, "native journal already exists");
    bool retained = snag_lstat_at(source->dir_fd, "events.jsonl", &path) < 0;
    if (retained && errno != ENOENT) return -1;
    if (!retained && !snag_file_unchanged(&before, &path))
        return snag_fail(error, error_size, ESTALE, "legacy source path changed");
    int rollback = -1;
    int index = -1;
    bool created = false;
    bool published = false;
    struct snag_session *candidate = malloc(sizeof(*candidate));
    if (!candidate) return snag_errno(ENOMEM);
    snag_session_init(candidate);
    char nonce[SNAG_ID_HEX_LEN + 1u];
    char stage[SNAG_ID_HEX_LEN + sizeof(".converting-")];
    int rc = -1;
    if (snag_random_id(nonce) < 0) goto done;
    (void)snprintf(stage, sizeof(stage), ".converting-%s", nonce);
    candidate->dir_path = snag_path_join(source->dir_path, stage);
    if (!candidate->dir_path || snag_mkdir_private_at(source->dir_fd, stage) < 0) goto done;
    created = true;
    candidate->dir_fd = snag_open_read_security_at(source->dir_fd, stage, true);
    if (candidate->dir_fd < 0 || snag_store_verify_private_fd(candidate->dir_fd, true,
            "provisional conversion directory", error, error_size) < 0) goto done;
    candidate->lock_fd = snag_create_private_at(candidate->dir_fd, "lock", true);
    candidate->log_fd = snag_create_private_at(candidate->dir_fd, "journal.bin", true);
    index = snag_create_private_at(candidate->dir_fd, "history.idx", true);
    if (candidate->lock_fd < 0 || candidate->log_fd < 0 || index < 0 ||
        snag_lock_file(candidate->lock_fd, false) < 0 ||
        snag_store_stage_binary_session(source, candidate, index, result,
            control, error, error_size) < 0) goto done;
    const char *slots[2] = {"checkpoint.0", "checkpoint.1"};
    for (size_t i = 0u; i < 2u; ++i) {
        int fd = snag_create_private_at(candidate->dir_fd, slots[i], true);
        if (fd < 0) goto done;
        int synced = snag_sync_file(fd);
        int saved = errno;
        (void)close(fd);
        errno = saved;
        if (synced < 0) goto done;
    }
    int query = index;
    if (snag_session_binary_index_adopt(candidate, index, error, error_size) < 0) goto done;
    index = -1;
    if (bootstrap_native_checkpoint(candidate, query, control, error, error_size) < 0 ||
        snag_sync_file(query) < 0 || snag_sync_dir(candidate->dir_fd) < 0 ||
        snag_sync_dir(source->dir_fd) < 0) goto done;
    if (control && control->cancelled && control->cancelled(control->opaque)) {
        rc = snag_fail(error, error_size, ECANCELED, "stopped conversion cancelled before cutover");
        goto done;
    }
    if (snag_fstat(source->log_fd, &path) < 0 || !snag_file_unchanged(&before, &path)) {
        rc = snag_fail(error, error_size, EAGAIN, "legacy source changed before cutover");
        goto done;
    }
    rollback = snag_open_read_security_at(source->dir_fd, ".legacy-source", true);
    if (rollback < 0 && errno == ENOENT && !retained) {
        if (snag_mkdir_private_at(source->dir_fd, ".legacy-source") < 0) goto done;
        rollback = snag_open_read_security_at(source->dir_fd, ".legacy-source", true);
    }
    if (rollback < 0 || snag_store_verify_private_fd(rollback, true,
            "retained legacy directory", error, error_size) < 0) goto done;
    if (retained) {
        if (snag_lstat_at(rollback, "events.jsonl", &path) < 0 ||
            !snag_file_unchanged(&before, &path)) {
            rc = snag_fail(error, error_size, ESTALE, "retained legacy source changed");
            goto done;
        }
    } else {
        if (!snag_lstat_at(rollback, "events.jsonl", &path) || errno != ENOENT) {
            rc = snag_fail(error, error_size, EEXIST, "retained legacy source already exists");
            goto done;
        }
        if (snag_lstat_at(source->dir_fd, "events.jsonl", &path) < 0 ||
            !snag_file_unchanged(&before, &path)) {
            rc = snag_fail(error, error_size, ESTALE, "legacy source path changed before cutover");
            goto done;
        }
    }
    if (!snag_lstat_at(source->dir_fd, "journal.bin", &path) || errno != ENOENT) {
        rc = snag_fail(error, error_size, EEXIST, "native selection changed before cutover");
        goto done;
    }
    /* Stop the provisional publisher before moving its directory-bound slots. */
    snag_session_unbind_binary(candidate);
    const char *derived[3] = {"history.idx", "checkpoint.0", "checkpoint.1"};
    for (size_t i = 0u; i < 3u; ++i) {
        if (snag_rename_at(candidate->dir_fd, derived[i], source->dir_fd, derived[i]) < 0)
            goto done;
    }
    if (!retained && snag_rename_at(source->dir_fd, "events.jsonl", rollback,
            "events.jsonl") < 0) goto done;
    if (snag_sync_dir(rollback) < 0 || snag_sync_dir(candidate->dir_fd) < 0 ||
        snag_sync_dir(source->dir_fd) < 0 ||
        snag_rename_at(candidate->dir_fd, "journal.bin", source->dir_fd, "journal.bin") < 0)
        goto done;
    published = true;
    if (snag_sync_dir(candidate->dir_fd) < 0 || snag_sync_dir(source->dir_fd) < 0) goto done;
    rc = 0;
done:
    {
        int saved = errno;
        if (index >= 0) (void)close(index);
        if (rollback >= 0) (void)close(rollback);
        snag_session_close(candidate);
        free(candidate);
        if (rc < 0) {
            if (published) {
                (void)snag_fail(error, error_size, saved,
                    "native conversion selected; directory durability is uncertain: %s",
                    strerror(saved));
            } else if (created && error && error_size && !error[0]) {
                (void)snag_errorf(error, error_size,
                    "conversion incomplete; provisional files retained at %s/%s: %s",
                    source->dir_path, stage, strerror(saved));
            }
        }
        errno = saved;
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
        .index = {.max = index->max}, .indexed = true,
        .producer.field = {.max = SNAG_MAX_EVENT_LINE}};
    snag_sha256_init(&writer.semantic);
    struct snag_session *sessions = malloc(3u * sizeof(*sessions));
    if (!sessions) return snag_errno(ENOMEM);
    struct snag_session *source = &sessions[0];
    struct snag_session *candidate = &sessions[1];
    struct snag_session *verified = &sessions[2];
    snag_session_init(source);
    snag_session_init(candidate);
    snag_session_init(verified);
    memcpy(source->id, prepared->id, sizeof(source->id));
    source->log_fd = target->log_fd;
    source->lock_fd = target->lock_fd;
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
    rc = snag_store_reconcile_binary(source, verified, semantic_event, &semantic,
        &recovery, NULL, error, error_size);
    if (rc < 0) goto done;
    unsigned char expected[32];
    unsigned char actual[32];
    snag_sha256_final(&writer.semantic, expected);
    snag_sha256_final(&semantic, actual);
    if (recovery.incomplete_tail_bytes || memcmp(expected, actual, sizeof(actual)) ||
        compare_core(prepared, verified) < 0) {
        rc = snag_fail(error, error_size, EINVAL, "native seed differs from its prepared source");
        goto done;
    }
    rc = snag_store_reconcile_binary_context(source, candidate, &recovery, &sources,
        NULL, error, error_size);
    if (rc < 0) goto done;
    candidate->log_fd = target->log_fd;
    candidate->lock_fd = target->lock_fd;
    rc = snag_session_bind_binary(candidate, &writer.identity, &recovery.verified,
        &writer.tree, &writer.producer, &sources, NULL, error, error_size);
    if (rc < 0) goto done;
    candidate->dir_fd = target->dir_fd;
    candidate->dir_path = target->dir_path;
    *target = *candidate;
    snag_session_init(candidate);
    snag_buf_free(index);
    *index = writer.index;
    writer.index = (struct snag_buf){0};
done:
    {
        int saved = errno;
        candidate->log_fd = candidate->lock_fd = -1;
        snag_session_close(candidate);
        snag_session_close(verified);
        free(sessions);
        snag_binary_checkpoint_sources_free(&sources);
        snag_binary_producer_free(&writer.producer);
        free(writer.records);
        snag_buf_free(&writer.payload);
        snag_buf_free(&writer.index);
        errno = saved;
    }
    return rc;
}
