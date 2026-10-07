/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_checkpoint.h"
#include "json.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#define PROVIDER_HEADER 48u
#define RECENT_ROW 36u
#define HISTORY_ROW 8u

static void
put_number(unsigned char *out, uint64_t number)
{
    for (size_t i = 0u; i < 8u; ++i) out[i] = (unsigned char)(number >> (i * 8u));
}

static uint64_t
get_number(const unsigned char *bytes)
{
    uint64_t number = 0u;
    for (size_t i = 0u; i < 8u; ++i) number |= (uint64_t)bytes[i] << (i * 8u);
    return number;
}

static int
recent_row(unsigned char row[RECENT_ROW], const json_t *entry)
{
    uint64_t seq, time;
    const json_t *turn_value = json_object_get(entry, "turn");
    const char *turn = json_string_value(turn_value);
    size_t turn_size = json_string_length(turn_value);
    if (snag_json_integer_u64(entry, "seq", &seq) < 0 ||
        snag_json_integer_u64(entry, "time", &time) < 0 || !turn ||
        (turn_size && (turn_size != 32u || !snag_hex_is_lower(turn, 32u)))) {
        return snag_errno(EINVAL);
    }
    put_number(row, seq);
    put_number(row + 8u, time);
    static const char *const flags[] = {"active", "unfinished", "processes"};
    for (size_t i = 0u; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        const json_t *value = json_object_get(entry, flags[i]);
        if (!json_is_boolean(value)) return snag_errno(EINVAL);
        if (json_is_true(value)) row[16u] |= (unsigned char)(1u << i);
    }
    if (turn[0]) {
        row[16u] |= 8u;
        for (size_t i = 0u; i < 16u; ++i) {
            unsigned high = (unsigned char)turn[i * 2u];
            unsigned low = (unsigned char)turn[i * 2u + 1u];
            high = high <= '9' ? high - '0' : high - 'a' + 10u;
            low = low <= '9' ? low - '0' : low - 'a' + 10u;
            row[20u + i] = (unsigned char)((high << 4u) | low);
        }
    }
    return 0;
}

int
snag_binary_checkpoint_provider_decode(const void *data, size_t size,
    struct snag_binary_checkpoint_provider *out)
{
    if (!data || !out || size < PROVIDER_HEADER) return snag_errno(EINVAL);
    const unsigned char *bytes = data;
    /* Version 1, rebuild disposition 1; all feature/reserved bits must be zero. */
    static const unsigned char tag[8] = {1u, 0u, 1u, 0u, 0u, 0u, 0u, 0u};
    if (memcmp(bytes, tag, sizeof(tag))) return snag_errno(EINVAL);
    struct snag_binary_checkpoint_provider view = {
        .next_seq = get_number(bytes + 8u), .compact_seq = get_number(bytes + 16u),
        .rebase_seq = get_number(bytes + 24u), .recent = bytes + PROVIDER_HEADER};
    uint64_t recent = get_number(bytes + 32u), history = get_number(bytes + 40u);
    if (view.next_seq < 2u || view.compact_seq >= view.next_seq ||
        view.rebase_seq >= view.next_seq || !recent ||
        recent > (size - PROVIDER_HEADER) / RECENT_ROW) return snag_errno(EINVAL);
    view.recent_count = (size_t)recent;
    size_t remaining = size - PROVIDER_HEADER - view.recent_count * RECENT_ROW;
    if (remaining % HISTORY_ROW || history != remaining / HISTORY_ROW)
        return snag_errno(EINVAL);
    view.history_count = (size_t)history;
    view.history = view.recent + view.recent_count * RECENT_ROW;
    uint64_t previous = 0u;
    for (size_t i = 0u; i < view.recent_count; ++i) {
        const unsigned char *row = view.recent + i * RECENT_ROW;
        uint64_t seq = get_number(row);
        if (seq <= previous || seq >= view.next_seq ||
            row[16u] > 15u || row[17u] || row[18u] || row[19u] ||
            ((row[16u] & 1u) && !(row[16u] & 8u))) return snag_errno(EINVAL);
        if (!(row[16u] & 8u)) {
            for (size_t j = 20u; j < RECENT_ROW; ++j)
                if (row[j]) return snag_errno(EINVAL);
        }
        previous = seq;
    }
    previous = 0u;
    for (size_t i = 0u; i < view.history_count; ++i) {
        uint64_t seq = get_number(view.history + i * HISTORY_ROW);
        if (seq <= previous || seq >= view.next_seq) return snag_errno(EINVAL);
        previous = seq;
    }
    *out = view;
    return 0;
}

int
snag_binary_checkpoint_provider_encode(struct snag_buf *out, const struct snag_session *state,
    const json_t *recent, const json_t *history)
{
    if (!out || !state || out->len > out->max || !json_is_array(recent) ||
        !json_is_array(history)) return snag_errno(EINVAL);
    struct snag_buf staged = {.max = out->max - out->len};
    unsigned char header[PROVIDER_HEADER] = {1u, 0u, 1u, 0u};
    put_number(header + 8u, state->next_seq);
    put_number(header + 16u, state->compact_seq);
    put_number(header + 24u, state->context_rebase_seq);
    put_number(header + 32u, json_array_size(recent));
    put_number(header + 40u, json_array_size(history));
    int rc = -1;
    if (snag_buf_append(&staged, header, sizeof(header)) < 0) goto done;
    for (size_t i = 0u; i < json_array_size(recent); ++i) {
        unsigned char row[RECENT_ROW] = {0};
        if (recent_row(row, json_array_get(recent, i)) < 0 ||
            snag_buf_append(&staged, row, sizeof(row)) < 0) goto done;
    }
    for (size_t i = 0u; i < json_array_size(history); ++i) {
        unsigned char row[HISTORY_ROW];
        uint64_t seq;
        if (snag_json_integer_u64(json_array_get(history, i), "seq", &seq) < 0) {
            errno = EINVAL;
            goto done;
        }
        put_number(row, seq);
        if (snag_buf_append(&staged, row, sizeof(row)) < 0) goto done;
    }
    struct snag_binary_checkpoint_provider view;
    if (snag_binary_checkpoint_provider_decode(staged.data, staged.len, &view) < 0) goto done;
    rc = snag_buf_append(out, staged.data, staged.len);
done:
    snag_buf_free(&staged);
    return rc;
}

static const json_t *
source_entry(const json_t *source, size_t *cursor, uint64_t wanted, bool historical)
{
    while (*cursor < json_array_size(source)) {
        const json_t *entry = json_array_get(source, (*cursor)++);
        uint64_t seq;
        if (snag_json_integer_u64(entry, "seq", &seq) < 0 || seq > wanted) break;
        if (seq < wanted) continue;
        const char *type = snag_json_string(entry, "type");
        enum snag_binary_kind kind;
        if (!type || json_string_length(json_object_get(entry, "type")) != strlen(type) ||
            !json_is_object(json_object_get(entry, "data"))) break;
        if (historical) {
            if (!snag_string_in(type, "irc_event irc_event_v2 session_checkpoint")) break;
            if (!strcmp(type, "session_checkpoint") &&
                json_object_size(json_object_get(entry, "data"))) break;
        } else if (snag_binary_event_kind(type, &kind) < 0) {
            break;
        }
        return entry;
    }
    errno = EINVAL;
    return NULL;
}

int
snag_binary_checkpoint_provider_materialize(const void *data, size_t size,
    const json_t *source_recent, const json_t *source_history, json_t **recent, json_t **history)
{
    if (!recent || !history || recent == history || !json_is_array(source_recent) ||
        !json_is_array(source_history)) return snag_errno(EINVAL);
    struct snag_binary_checkpoint_provider view;
    if (snag_binary_checkpoint_provider_decode(data, size, &view) < 0) return -1;
    json_t *events = json_array(), *sources = json_array();
    int rc = -1;
    if (!events || !sources) { errno = ENOMEM; goto done; }
    size_t cursor = 0u;
    for (size_t i = 0u; i < view.recent_count; ++i) {
        const unsigned char *row = view.recent + i * RECENT_ROW;
        uint64_t seq = get_number(row), time = get_number(row + 8u);
        if (seq > INT64_MAX || time > INT64_MAX) { errno = EOVERFLOW; goto done; }
        const json_t *entry = source_entry(source_recent, &cursor, seq, false);
        if (!entry) goto done;
        char turn[33] = {0};
        static const char hex[] = "0123456789abcdef";
        if (row[16u] & 8u) {
            for (size_t j = 0u; j < 16u; ++j) {
                turn[j * 2u] = hex[row[20u + j] >> 4u];
                turn[j * 2u + 1u] = hex[row[20u + j] & 15u];
            }
        }
        json_t *event = json_pack("{s:I,s:O,s:O,s:I,s:s,s:b,s:b,s:b}",
            "seq", (json_int_t)seq, "type", json_object_get(entry, "type"),
            "data", json_object_get(entry, "data"), "time", (json_int_t)time, "turn", turn,
            "active", !!(row[16u] & 1u), "unfinished", !!(row[16u] & 2u),
            "processes", !!(row[16u] & 4u));
        int appended = event ? json_array_append(events, event) : -1;
        json_decref(event);
        if (appended < 0) { errno = ENOMEM; goto done; }
    }
    cursor = 0u;
    for (size_t i = 0u; i < view.history_count; ++i) {
        uint64_t seq = get_number(view.history + i * HISTORY_ROW);
        if (seq > INT64_MAX) { errno = EOVERFLOW; goto done; }
        const json_t *entry = source_entry(source_history, &cursor, seq, true);
        if (!entry) goto done;
        json_t *event = json_pack("{s:I,s:O,s:O}", "seq", (json_int_t)seq,
            "type", json_object_get(entry, "type"), "data", json_object_get(entry, "data"));
        int appended = event ? json_array_append(sources, event) : -1;
        json_decref(event);
        if (appended < 0) { errno = ENOMEM; goto done; }
    }
    *recent = events;
    *history = sources;
    events = sources = NULL;
    rc = 0;
done:
    json_decref(events);
    json_decref(sources);
    return rc;
}

struct source_rows {
    int fd;
    const struct snag_binary_anchor *through;
    const struct snag_binary_checkpoint_index *access;
    const unsigned char *rows;
    size_t count, stride, at;
    bool historical;
    bool (*cancelled)(void *);
    void *opaque;
    json_t *out;
};

static bool
source_rows_cancelled(void *opaque)
{
    struct source_rows *rows = opaque;
    return rows->cancelled && rows->cancelled(rows->opaque);
}

static int
source_row_append(struct source_rows *rows, uint64_t sequence, const char *type, json_t *data)
{
    if (rows->historical && !snag_string_in(type, "irc_event irc_event_v2 session_checkpoint")) {
        json_decref(data);
        return snag_errno(EINVAL);
    }
    json_t *entry = json_pack("{s:I,s:s,s:O}", "seq", (json_int_t)sequence,
        "type", type, "data", data);
    json_decref(data);
    if (!entry || json_array_append_new(rows->out, entry) < 0) return snag_errno(ENOMEM);
    ++rows->at;
    return 0;
}

static int
source_row_read(void *opaque, const struct snag_binary_record *record, uint64_t sequence)
{
    struct source_rows *rows = opaque;
    if (rows->at >= rows->count || sequence != get_number(rows->rows + rows->at * rows->stride))
        return snag_errno(EINVAL);
    bool marker = record->kind == SNAG_BINARY_CHECKPOINT_RECEIPT ||
        record->kind == SNAG_BINARY_LEGACY_CHECKPOINT;
    if (record->flags != (marker ? SNAG_BINARY_RECORD_OPTIONAL : 0u)) return snag_errno(EINVAL);
    const char *type;
    json_t *data = NULL;
    if (snag_binary_checkpoint_record_project(rows->fd, rows->through, rows->access,
        record, sequence, &type, &data) < 0) return -1;
    return source_row_append(rows, sequence, type, data);
}

static int
read_source_rows(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_index *access, const unsigned char *rows,
    size_t count, size_t stride, bool historical, bool (*cancelled)(void *), void *opaque,
    json_t *out)
{
    struct source_rows selected = {.fd = fd, .through = through, .access = access,
        .rows = rows, .count = count, .stride = stride, .historical = historical,
        .cancelled = cancelled, .opaque = opaque, .out = out};
    struct snag_buf locations = {.max = SIZE_MAX};
    size_t captured = 0u;
    int rc = -1;
    /* Restrict the admitted location table to required rows. The batch walker
     * authenticates each shared batch once without reading unrelated sources. */
    while (access && captured < count) {
        uint64_t sequence = get_number(rows + captured * stride);
        if (sequence >= access->boundary.next_seq) break;
        if (source_rows_cancelled(&selected)) { errno = ECANCELED; goto done; }
        if (sequence > INT64_MAX) { errno = EOVERFLOW; goto done; }
        struct snag_binary_index_entry entry;
        int found = snag_binary_checkpoint_index_find(access, sequence, &entry);
        if (found != 0) {
            if (found > 0) errno = ENOENT;
            goto done;
        }
        unsigned char bytes[SNAG_BINARY_INDEX_ENTRY_SIZE];
        if (snag_binary_index_entry_encode(bytes, &access->identity, &entry) < 0 ||
            snag_buf_append(&locations, bytes, sizeof(bytes)) < 0) goto done;
        ++captured;
    }
    if (captured) {
        struct snag_binary_checkpoint_index subset = *access;
        subset.entries = locations.data;
        subset.entry_count = captured;
        uint64_t first = get_number(rows), last = get_number(rows + (captured - 1u) * stride);
        if (snag_binary_checkpoint_records_read(fd, through, &subset, first, last + 1u,
            source_row_read, source_rows_cancelled, &selected) < 0) goto done;
        if (selected.at != captured) { errno = ENOENT; goto done; }
    }
    while (selected.at < count) {
        if (source_rows_cancelled(&selected)) { errno = ECANCELED; goto done; }
        uint64_t sequence = get_number(rows + selected.at * stride);
        if (sequence > INT64_MAX) { errno = EOVERFLOW; goto done; }
        const char *type;
        json_t *data = NULL;
        if (snag_binary_checkpoint_projection_read(fd, through, access, sequence,
                &type, &data) < 0 || source_row_append(&selected, sequence, type, data) < 0)
            goto done;
    }
    rc = 0;
done:
    snag_buf_free(&locations);
    return rc;
}

int
snag_binary_checkpoint_provider_read(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_index *access, const void *data, size_t size,
    bool (*cancelled)(void *), void *opaque, json_t **recent, json_t **history)
{
    if (fd < 0 || !through || !recent || !history || recent == history) return snag_errno(EINVAL);
    struct snag_binary_checkpoint_provider view;
    if (snag_binary_checkpoint_provider_decode(data, size, &view) < 0) return -1;
    if (view.next_seq != through->next_seq) return snag_errno(EINVAL);
    json_t *events = json_array();
    json_t *sources = json_array();
    int rc = -1;
    if (!events || !sources) { snag_errno(ENOMEM); goto done; }
    if (read_source_rows(fd, through, access, view.recent, view.recent_count, RECENT_ROW,
            false, cancelled, opaque, events) < 0 ||
        read_source_rows(fd, through, access, view.history, view.history_count, HISTORY_ROW,
            true, cancelled, opaque, sources) < 0) goto done;
    if (cancelled && cancelled(opaque)) { snag_errno(ECANCELED); goto done; }
    rc = snag_binary_checkpoint_provider_materialize(data, size, events, sources, recent, history);
done:
    json_decref(events);
    json_decref(sources);
    return rc;
}
