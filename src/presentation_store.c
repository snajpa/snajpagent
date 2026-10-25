/* SPDX-License-Identifier: GPL-2.0-only */
#include "presentation.h"
#include "turn.h"
#include "fs.h"
#include "store_binary_wire.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct snag_presentation_writer {
    int fd;
    char id[SNAG_ID_HEX_LEN + 1u];
    uint64_t origin;
    struct snag_binary_anchor tail;
    struct snag_buf batch, wire;
    json_t *response;
    char recovery[sizeof(".view-presentation-recovery-") + SNAG_ID_HEX_LEN];
};

static unsigned char
hex_byte(const char *text)
{
    static const char digits[] = "0123456789abcdef";
    return (unsigned char)((strchr(digits, text[0]) - digits) * 16u +
        (strchr(digits, text[1]) - digits));
}

static int
restore_response(struct snag_presentation_writer *writer, const char *id)
{
    struct snag_binary_anchor position = writer->tail;
    while (position.next_seq > 1u) {
        struct snag_binary_anchor begin, tail;
        json_t *rows = NULL;
        bool incomplete;
        if (snag_presentation_read(writer->fd, id, &writer->tail, &position, true,
            SNAG_JOURNAL_PAGE_BYTES, &rows, &begin, &tail, &incomplete, NULL, NULL) < 0)
            return -1;
        for (size_t i = 0u; i < json_array_size(rows); ++i) {
            const json_t *data = json_object_get(json_array_get(rows, i), "data");
            json_t *response = json_object_get(data, "response");
            const char *op = snag_json_string(data, "op");
            const char *type = snag_json_string(data, "text");
            if (!response && op && !strcmp(op, "durable") && type &&
                !strcmp(type, "response_completed"))
                response = json_array_get(json_object_get(data, "data"), 0u);
            if (response) {
                writer->response = json_incref(response);
                break;
            }
        }
        json_decref(rows);
        if (writer->response) return 0;
        position = begin;
    }
    return 0;
}

/* A zero-filled suffix can follow an interrupted output write. Keep the entire
 * original inode, and replace only this auxiliary stream with its verified cut.
 * Closed nonzero corruption still fails; canonical history is never repaired here. */
static int
recover_zero_tail(struct snag_presentation_writer *writer, int directory, const char *id,
    uint64_t size, struct snag_binary_identity *identity, uint64_t *incomplete)
{
    unsigned char bytes[65536];
    uint64_t end = size;
    while (end > SNAG_BINARY_HEADER_SIZE) {
        size_t count = end - SNAG_BINARY_HEADER_SIZE < sizeof(bytes) ?
            (size_t)(end - SNAG_BINARY_HEADER_SIZE) : sizeof(bytes);
        if (snag_pread(writer->fd, bytes, count, (int64_t)(end - count)) != (ssize_t)count)
            return -1;
        size_t zeros = 0u;
        while (zeros < count && !bytes[count - zeros - 1u]) ++zeros;
        end -= zeros;
        if (zeros < count) break;
    }
    if (size - end < 2u || end <= SNAG_BINARY_HEADER_SIZE) return snag_errno(EINVAL);
    ++end; /* Keep the valid batch's closing delimiter. */
    if (snag_binary_journal_tail(writer->fd, end, &writer->batch, identity,
            &writer->tail, incomplete) < 0 || *incomplete) return snag_errno(EINVAL);
    for (size_t i = 0u; i < sizeof(identity->id); ++i)
        if (identity->id[i] != hex_byte(id + i * 2u)) return snag_errno(EINVAL);
    char token[SNAG_ID_HEX_LEN + 1u];
    char stage[sizeof(".view-presentation-stage-") + SNAG_ID_HEX_LEN];
    if (snag_random_id(token) < 0) return -1;
    (void)snprintf(stage, sizeof(stage), ".view-presentation-stage-%s", token);
    (void)snprintf(writer->recovery, sizeof(writer->recovery),
        ".view-presentation-recovery-%s", token);
    int fd = snag_open_private_append_at(directory, stage, true);
    if (fd < 0) return -1;
    int rc = -1;
    bool linked = false, installed = false;
    for (uint64_t offset = 0u; offset < end;) {
        size_t count = end - offset < sizeof(bytes) ? (size_t)(end - offset) : sizeof(bytes);
        if (snag_pread(writer->fd, bytes, count, (int64_t)offset) != (ssize_t)count ||
            snag_write_full(fd, bytes, count) < 0) goto done;
        offset += count;
    }
    if (snag_sync_file(fd) < 0 ||
        snag_link_at(directory, SNAG_PRESENTATION_FILE, directory, writer->recovery) < 0) goto done;
    linked = true;
    if (snag_sync_dir(directory) < 0 ||
        snag_rename_at(directory, stage, directory, SNAG_PRESENTATION_FILE) < 0) goto done;
    installed = true;
    if (snag_sync_dir(directory) < 0) goto done;
    (void)close(writer->fd);
    writer->fd = fd;
    fd = -1;
    rc = 0;
done:;
    int saved = errno;
    if (fd >= 0) (void)close(fd);
    if (linked && !installed) (void)snag_unlink_at(directory, writer->recovery, false);
    (void)snag_unlink_at(directory, stage, false);
    errno = saved;
    return rc;
}

const char *
snag_presentation_recovery(const struct snag_presentation_writer *writer)
{
    return writer && writer->recovery[0] ? writer->recovery : NULL;
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
    memcpy(writer->id, id, sizeof(writer->id));
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
                &found, &writer->tail, &incomplete) < 0 &&
            (errno != EINVAL || recover_zero_tail(writer, directory, id,
                (uint64_t)info.st_size, &found, &incomplete) < 0)) goto fail;
        if (memcmp(found.id, identity.id, sizeof(identity.id))) { errno = EINVAL; goto fail; }
        if (incomplete && (snag_truncate(writer->fd, (int64_t)writer->tail.end) < 0 ||
            snag_sync_file(writer->fd) < 0)) goto fail;
        if (restore_response(writer, id) < 0) goto fail;
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
append_record(struct snag_presentation_writer *writer, const json_t *data, uint16_t kind)
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
        if (writer->response &&
            snag_string_in(command->text, "tool_started tool_finished") &&
            json_object_set(data, "response", writer->response) < 0) {
            json_decref(data);
            return -1;
        }
    }
    int rc = append_record(writer, data, 1u);
    if (!rc && command->kind == SNAG_UI_DURABLE &&
        !strcmp(command->text, "response_completed")) {
        json_decref(writer->response);
        writer->response = json_incref(json_array_get(json_object_get(data, "data"), 0u));
    }
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
    int rc = append_record(writer, data, 2u);
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
    const struct snag_binary_anchor *position, bool reverse, size_t budget,
    json_t **out, struct snag_binary_anchor *begin,
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
    if (current.end > observed.end || current.next_seq > observed.next_seq) {
        errno = ESTALE;
        goto out;
    }
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

int
snag_presentation_local_records(int fd, const char *id,
    const struct snag_binary_anchor *bound, json_t **selected)
{
    *selected = json_array();
    if (!*selected) return -1;
    if (bound->next_seq <= 2u) return 0;
    struct snag_binary_anchor begin, tail;
    json_t *records = NULL;
    bool incomplete;
    if (snag_presentation_read(fd, id, bound, NULL, true,
        SNAG_JOURNAL_PAGE_BYTES, &records, &begin, &tail, &incomplete, NULL, NULL) < 0)
        goto fail;
    size_t count = json_array_size(records), end = count;
    /* Model history omits local commands and cancelled edits. Retain their
     * recent output after the last model submission, on the rollout only. */
    for (size_t i = 0u; i < count; ++i) {
        const json_t *record = json_object_get(json_array_get(records, i), "data");
        const char *op = snag_json_string(record, "op");
        const char *text = snag_json_string(record, "text");
        if ((!strcmp(op, "event") && text && !strcmp(text, "turn_started")) ||
            (!strcmp(op, "submitted") && !json_integer_value(json_object_get(record, "data")) &&
                !snag_prompt_command(text))) {
            end = i;
            break;
        }
    }
    bool command = false;
    for (size_t i = end; i; --i) {
        json_t *record = json_object_get(json_array_get(records, i - 1u), "data");
        const char *op = snag_json_string(record, "op");
        const char *text = snag_json_string(record, "text");
        size_t length = text ? strlen(text) : 0u;
        bool local = !json_is_object(json_object_get(record, "route"));
        bool cancelled = false;
        if (!strcmp(op, "submitted")) {
            command = local && snag_prompt_command(text);
            cancelled = local && json_integer_value(json_object_get(record, "data")) &&
                length >= 2u && !strcmp(text + length - 2u, "^C");
        }
        if ((cancelled || (command && local && snag_string_in(op,
            "submitted host notice help runtime error warning choices"))) &&
            json_array_append(*selected, record) < 0) goto fail;
    }
    json_decref(records);
    return 0;
fail:
    json_decref(records);
    json_decref(*selected);
    *selected = NULL;
    return -1;
}

int
snag_presentation_local(struct snag_presentation_writer *writer, struct snag_render *render)
{
    if (!writer) return 0;
    json_t *records = NULL;
    if (snag_presentation_local_records(writer->fd, writer->id, &writer->tail, &records) < 0)
        return -1;
    int rc = 0;
    for (size_t i = 0u; !rc && i < json_array_size(records); ++i)
        rc = snag_presentation_replay(render, json_array_get(records, i), -1);
    json_decref(records);
    return rc;
}
