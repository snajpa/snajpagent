/* SPDX-License-Identifier: GPL-2.0-only */
#include "presentation.h"
#include "fs.h"
#include "store_binary_wire.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct snag_presentation_writer {
    int fd;
    uint64_t origin;
    struct snag_binary_anchor tail;
    struct snag_buf batch, wire;
    json_t *response;
};

static unsigned char
hex_byte(const char *text)
{
    static const char digits[] = "0123456789abcdef";
    return (unsigned char)((strchr(digits, text[0]) - digits) * 16u +
        (strchr(digits, text[1]) - digits));
}

struct snag_presentation_writer *
snag_presentation_writer_open(int directory, const char *id)
{
    if (!id || !snag_hex_is_lower(id, SNAG_ID_HEX_LEN)) {
        errno = EINVAL;
        return NULL;
    }
    struct snag_presentation_writer *writer = calloc(1u, sizeof(*writer));
    if (!writer) return NULL;
    writer->fd = snag_open_private_append_at(directory, SNAG_PRESENTATION_FILE, true);
    if (writer->fd < 0 && errno == EEXIST)
        writer->fd = snag_open_private_append_at(directory, SNAG_PRESENTATION_FILE, false);
    writer->batch.max = SNAG_BINARY_BATCH_MAX;
    writer->wire.max = SNAG_BINARY_WIRE_BATCH_MAX;
    snag_file_info info;
    if (writer->fd < 0 || snag_fstat(writer->fd, &info) < 0) goto fail;
    struct snag_binary_identity identity = {.created_ms = snag_time_ms()};
    for (size_t i = 0u; i < sizeof(identity.id); ++i) identity.id[i] = hex_byte(id + i * 2u);
    if (!info.st_size) {
        unsigned char header[SNAG_BINARY_HEADER_SIZE];
        snag_binary_header_encode(header, &identity);
        if (snag_write_full(writer->fd, header, sizeof(header)) < 0 ||
            snag_sync_file(writer->fd) < 0 || snag_sync_dir(directory) < 0 ||
            snag_binary_header_decode(header, sizeof(header), &identity, &writer->tail) != 0)
            goto fail;
    } else {
        struct snag_binary_identity found;
        uint64_t incomplete;
        if (snag_binary_journal_tail(writer->fd, (uint64_t)info.st_size, &writer->batch,
            &found, &writer->tail, &incomplete) < 0) goto fail;
        if (memcmp(found.id, identity.id, sizeof(identity.id))) { errno = EINVAL; goto fail; }
        if (incomplete && (snag_truncate(writer->fd, (int64_t)writer->tail.end) < 0 ||
            snag_sync_file(writer->fd) < 0)) goto fail;
    }
    return writer;
fail: {
        int cause = errno ? errno : EINVAL;
        snag_presentation_writer_close(writer);
        errno = cause;
        return NULL;
    }
}

void
snag_presentation_writer_close(struct snag_presentation_writer *writer)
{
    if (!writer) return;
    if (writer->fd >= 0) (void)close(writer->fd);
    snag_buf_free(&writer->batch);
    snag_buf_free(&writer->wire);
    json_decref(writer->response);
    free(writer);
}

static int
append_record(struct snag_presentation_writer *writer, const json_t *data, uint16_t kind,
    bool sync)
{
    char *text = json_dumps(data, JSON_COMPACT | JSON_ENCODE_ANY);
    if (!text) return -1;
    struct snag_binary_record record = {.kind = kind, .version = 1u,
        .timestamp_ms = snag_time_ms(), .payload = (const unsigned char *)text,
        .size = strlen(text)};
    struct snag_binary_anchor next;
    snag_buf_reset(&writer->batch);
    snag_buf_reset(&writer->wire);
    int rc = snag_binary_batch_encode(&writer->batch, &writer->tail, &record, 1u, 0u, &next);
    if (!rc) rc = snag_binary_wire_encode(&writer->wire, writer->batch.data, writer->batch.len);
    if (!rc) {
        rc = snag_write_full(writer->fd, writer->wire.data, writer->wire.len);
        if (rc < 0) {
            int cause = errno;
            (void)snag_truncate(writer->fd, (int64_t)writer->tail.end);
            errno = cause;
        } else {
            writer->tail = next;
            if (sync) rc = snag_sync_file(writer->fd);
        }
    }
    free(text);
    return rc;
}

int
snag_presentation_append(struct snag_presentation_writer *writer,
    const struct snag_ui_command *command)
{
    if (!writer) return 0;
    json_t *data = NULL;
    int encoded = snag_presentation_encode(command, &data);
    if (encoded != 0) return encoded < 0 ? -1 : 0;
    if (command->kind == SNAG_UI_DURABLE) {
        if (!strcmp(command->text, "response_completed")) {
            json_decref(writer->response);
            writer->response = json_incref(json_array_get(json_object_get(data, "data"), 0u));
        } else if (writer->response &&
            snag_string_in(command->text, "tool_started tool_finished") &&
            json_object_set(data, "response", writer->response) < 0) {
            json_decref(data);
            return -1;
        }
    }
    /* Streaming deltas share their closing operation's sync. */
    bool sync = command->kind == SNAG_UI_SUBMITTED || command->kind == SNAG_UI_ROLLOUT_END ||
        command->kind == SNAG_UI_ROLLOUT_ABORT || command->kind == SNAG_UI_ERROR ||
        command->kind == SNAG_UI_HOST || command->kind == SNAG_UI_HELP;
    int rc = append_record(writer, data, 1u, sync);
    json_decref(data);
    return rc;
}

int
snag_presentation_origin(int fd, const struct snag_binary_anchor *tail,
    uint64_t *next_sequence, struct snag_binary_anchor *first)
{
    if (!tail || tail->next_seq <= 1u) return snag_errno(EINVAL);
    struct snag_binary_identity identity;
    struct snag_binary_anchor begin, end;
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_batch batch;
    int rc = -1;
    uint64_t incomplete;
    if (snag_binary_journal_tail(fd, SNAG_BINARY_HEADER_SIZE, &scratch,
        &identity, &begin, &incomplete) < 0 ||
        snag_binary_batch_read(fd, tail->end, &begin, &scratch, &batch, &end) != 0)
        goto out;
    struct snag_binary_record record;
    size_t cursor = SNAG_BINARY_BATCH_HEADER_SIZE;
    uint64_t sequence;
    if (batch.count != 1u ||
        snag_binary_record_next(&batch, &cursor, &record, &sequence) != 0 ||
        sequence != 1u || record.kind != 2u || record.version != 1u || record.flags) {
        errno = EPROTO;
        goto out;
    }
    char error[128];
    json_t *data = snag_json_load_strict(record.payload, record.size,
        SNAG_MAX_EVENT_LINE, error, sizeof(error));
    uint64_t origin;
    rc = snag_json_integer_u64(data, "next_sequence", &origin);
    if (!rc && (!origin || origin > INT64_MAX)) rc = snag_errno(EINVAL);
    if (!rc) {
        *next_sequence = origin;
        *first = end;
    }
    json_decref(data);
out:
    snag_buf_free(&scratch);
    return rc;
}

int
snag_presentation_start(struct snag_presentation_writer *writer, uint64_t next_sequence)
{
    if (!writer || !next_sequence || next_sequence > INT64_MAX) return snag_errno(EINVAL);
    if (writer->origin) return 0;
    if (writer->tail.next_seq > 1u) {
        struct snag_binary_anchor first;
        return snag_presentation_origin(writer->fd, &writer->tail, &writer->origin, &first);
    }
    json_t *data = json_pack("{s:I}", "next_sequence", (json_int_t)next_sequence);
    if (!data) return -1;
    int rc = append_record(writer, data, 2u, true);
    json_decref(data);
    if (!rc) writer->origin = next_sequence;
    return rc;
}

json_t *
snag_presentation_snapshot(const struct snag_presentation_writer *writer)
{
    if (!writer || !writer->origin) return json_null();
    struct snag_buf digest = {.max = 64u};
    if (snag_base64_append(&digest, writer->tail.digest, sizeof(writer->tail.digest)) < 0) {
        snag_buf_free(&digest);
        return NULL;
    }
    json_t *value = json_pack("{s:I,s:[I,I,I,I,s#]}",
        "origin", (json_int_t)writer->origin,
        "tail", (json_int_t)writer->tail.end, (json_int_t)writer->tail.next_seq,
        (json_int_t)writer->tail.turns, (json_int_t)writer->tail.previous,
        (const char *)digest.data, digest.len);
    snag_buf_free(&digest);
    return value;
}

int
snag_presentation_bound(const json_t *snapshot, uint64_t *origin,
    struct snag_binary_anchor *anchor)
{
    const json_t *tail = json_object_get(snapshot, "tail");
    uint64_t first, fields[4];
    if (snag_json_integer_u64(snapshot, "origin", &first) < 0 || !first ||
        first > INT64_MAX || json_array_size(tail) != 5u) return snag_errno(EINVAL);
    for (size_t i = 0u; i < 4u; ++i) {
        const json_t *value = json_array_get(tail, i);
        if (!json_is_integer(value) || json_integer_value(value) < 0)
            return snag_errno(EINVAL);
        fields[i] = (uint64_t)json_integer_value(value);
    }
    if (fields[0] <= SNAG_BINARY_HEADER_SIZE || fields[1] < 2u ||
        fields[2] || fields[3] >= fields[0]) return snag_errno(EINVAL);
    const char *encoded = json_string_value(json_array_get(tail, 4u));
    struct snag_buf digest = {.max = 32u};
    int rc = encoded ? snag_base64_decode(&digest, encoded) : -1;
    if (!rc && digest.len == 32u) {
        *anchor = (struct snag_binary_anchor){.end = fields[0], .next_seq = fields[1],
            .turns = fields[2], .previous = fields[3]};
        memcpy(anchor->digest, digest.data, sizeof(anchor->digest));
        *origin = first;
    } else rc = snag_errno(EINVAL);
    snag_buf_free(&digest);
    return rc;
}

int
snag_presentation_read(int fd, const char *id, const struct snag_binary_anchor *bound,
    const struct snag_binary_anchor *position, bool reverse, size_t budget, json_t **out, struct snag_binary_anchor *begin,
    struct snag_binary_anchor *tail, bool *incomplete, bool (*cancel)(void *), void *opaque)
{
    if (!id || !snag_hex_is_lower(id, SNAG_ID_HEX_LEN) || !budget)
        return snag_errno(EINVAL);
    snag_file_info info;
    if (snag_fstat(fd, &info) < 0) return -1;
    struct snag_binary_identity identity;
    struct snag_binary_anchor current, observed;
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    json_t *records = json_array();
    uint64_t pending;
    int rc = -1;
    if (!records) goto out;
    if (cancel && cancel(opaque)) { errno = ECANCELED; goto out; }
    if (bound && bound->end > (uint64_t)info.st_size) { errno = ESTALE; goto out; }
    if (snag_binary_journal_tail(fd, bound ? bound->end : (uint64_t)info.st_size, &scratch,
        &identity, &observed, &pending) < 0) goto out;
    for (size_t i = 0u; i < sizeof(identity.id); ++i)
        if (identity.id[i] != hex_byte(id + i * 2u)) { errno = EINVAL; goto out; }
    if (bound && (bound->end != observed.end || bound->next_seq != observed.next_seq ||
        bound->previous != observed.previous || bound->turns != observed.turns ||
        memcmp(bound->digest, observed.digest, sizeof(bound->digest)))) {
        errno = ESTALE;
        goto out;
    }
    if (position && position->end) current = *position;
    else if (reverse) current = observed;
    else {
        uint64_t ignored;
        if (snag_binary_journal_tail(fd, SNAG_BINARY_HEADER_SIZE, &scratch,
            &identity, &current, &ignored) < 0) goto out;
    }
    if (current.end > observed.end || current.next_seq > observed.next_seq) { errno = ESTALE; goto out; }
    size_t bytes = 0u;
    bool public_open = false;
    while ((reverse ? current.next_seq > 1u : current.end < observed.end) &&
        (bytes < budget || public_open)) {
        if (cancel && cancel(opaque)) { errno = ECANCELED; goto out; }
        struct snag_binary_batch batch;
        struct snag_binary_anchor previous;
        if ((reverse ? snag_binary_batch_previous(fd, &current, &scratch, &batch, &previous) :
            snag_binary_batch_read(fd, observed.end, &current, &scratch, &batch, &previous)) != 0)
            goto out;
        size_t cursor = SNAG_BINARY_BATCH_HEADER_SIZE;
        struct snag_binary_record record;
        uint64_t sequence;
        if (batch.count != 1u ||
            snag_binary_record_next(&batch, &cursor, &record, &sequence) != 0 ||
            (record.kind != 1u && !(sequence == 1u && record.kind == 2u)) ||
            record.version != 1u || record.flags) {
            errno = EINVAL;
            goto out;
        }
        char error[128];
        json_t *data = snag_json_load_strict(record.payload, record.size,
            SNAG_MAX_EVENT_LINE, error, sizeof(error));
        if (!data) goto out;
        if (record.kind == 2u) json_decref(data);
        else {
            const char *op = snag_json_string(data, "op");
            if (!op) { json_decref(data); errno = EINVAL; goto out; }
            if (!strcmp(op, "public")) public_open = true;
            else if (snag_string_in(op, "end abort")) public_open = reverse;
            else if (!strcmp(op, "begin")) public_open = !reverse;
            json_t *row = json_pack("{s:I,s:o}", "seq", (json_int_t)sequence, "data", data);
            if (!row || json_array_append_new(records, row) < 0) goto out;
        }
        bytes += record.size;
        current = previous;
    }
    *out = records;
    records = NULL;
    *begin = current;
    *tail = observed;
    *incomplete = pending != 0u;
    rc = 0;
out:
    json_decref(records);
    snag_buf_free(&scratch);
    return rc;
}
