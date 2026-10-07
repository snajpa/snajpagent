/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_reader.h"
#include "history_view.h"
#include "irc.h"
#include "json.h"
#include "render.h"
#include "session_view.h"
#include "vm_public.h"
#include "vm_report.h"
#include "vm_text.h"
#include "vm_transcript.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct source_view {
    struct snag_session session;
    bool best_effort, incomplete, verified;
    char public_id[SNAG_ID_HEX_LEN + 1u];
    json_t *public_snapshot;
    struct snag_journal_cursor public_scanned;
    struct source_view *next;
};

struct snag_vm_reader {
    struct snag_store *store;
    struct snag_wire_secrets secrets;
    char **secret_values;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t condition;
    snag_wake_fd wake[2];
    atomic_uint_fast64_t generation;
    atomic_bool stop;
    uint64_t working_generation;
    uint64_t progress_generation, progress_bytes, progress_events, progress_total;
    struct snag_vm_read_result *pending, *completed;
    struct source_view *views, *current;
};

void
snag_vm_read_result_free(struct snag_vm_read_result *result)
{
    if (!result) return;
    json_decref(result->request.retained_sessions);
    json_decref(result->request.report);
    json_decref(result->request.route);
    json_decref(result->request.known_reports);
    json_decref(result->request.inline_reports);
    free((char *)result->request.query);
    json_decref(result->events);
    json_decref(result->catalog);
    json_decref(result->blocks);
    snag_vm_document_free(result->document);
    snag_vm_register_free(&result->copied);
    free(result);
}

static bool
read_canceled(void *opaque)
{
    struct snag_vm_reader *reader = opaque;
    return atomic_load(&reader->stop) ||
        reader->working_generation != atomic_load(&reader->generation);
}

static void
view_close(struct snag_vm_reader *reader)
{
    struct source_view **link = &reader->views;
    while (*link && *link != reader->current) link = &(*link)->next;
    if (*link) {
        struct source_view *old = *link;
        *link = old->next;
        snag_session_close(&old->session);
        json_decref(old->public_snapshot);
        free(old);
    }
    reader->current = NULL;
}

static bool
source_retained(const struct snag_vm_read_request *request, const char *id)
{
    if (request->kind == SNAG_VM_READ_HISTORY && !strcmp(id, request->session_id)) return true;
    for (size_t i = 0u; i < json_array_size(request->retained_sessions); ++i) {
        const char *retained = json_string_value(json_array_get(request->retained_sessions, i));
        if (!strcmp(id, retained)) return true;
    }
    return false;
}

static void
retain_views(struct snag_vm_reader *reader, const struct snag_vm_read_request *request)
{
    struct source_view *view = reader->views;
    while (view) {
        struct source_view *next = view->next;
        if (!source_retained(request, view->session.id)) {
            reader->current = view;
            view_close(reader);
        }
        view = next;
    }
    reader->current = NULL;
}

static struct snag_journal_cursor
view_tail(const struct snag_session *view)
{
    struct snag_journal_cursor tail = {.offset = view->log_end, .next_seq = view->next_seq};
    memcpy(tail.prev_sha256, view->prev_sha256, sizeof(tail.prev_sha256));
    return tail;
}

static int
view_open(struct snag_vm_reader *reader, const struct snag_vm_read_request *request,
    char *error, size_t size)
{
    struct source_view *source = reader->views;
    while (source && strcmp(source->session.id, request->session_id)) source = source->next;
    reader->current = source;
    if (source && request->pin_tail && request->trusted_tail &&
        source->session.log_fd >= 0 && source->session.log_end > request->tail.offset) {
        view_close(reader);
        return view_open(reader, request, error, size);
    }
    if (!source) {
        source = calloc(1u, sizeof(*source));
        if (!source) return -1;
        snag_session_init(&source->session);
        source->session.history_cancel = read_canceled;
        source->session.history_cancel_opaque = reader;
        source->next = reader->views;
        reader->views = reader->current = source;
    }
    struct snag_session *view = &source->session;
    if (view->log_fd >= 0) {
        int rc;
        if (!request->trusted_tail && (request->refresh || !source->best_effort)) {
            rc = snag_session_history_observe(view, &source->incomplete, error, size);
            if (!rc) source->best_effort = true;
            return rc;
        }
        struct snag_journal_cursor tail = request->trusted_tail ? request->tail : view_tail(view);
        /* A snapshot can lead a queued owner notification. Keep its verified
         * prefix and current certification until the owner catches up. */
        if (request->trusted_tail && tail.offset < view->log_end &&
            tail.next_seq < view->next_seq) {
            tail = view_tail(view);
            return snag_session_history_refresh(view, &tail, error, size);
        }
        rc = snag_session_history_refresh(view, &tail, error, size);
        if (!rc) {
            source->best_effort = !request->trusted_tail;
            if (request->trusted_tail) source->incomplete = false;
        }
        return rc;
    }
    source->best_effort = !request->trusted_tail;
    source->incomplete = false;
    int rc;
    if (request->trusted_tail) {
        rc = snag_session_history_open(reader->store, view, request->session_id,
            &request->tail, error, size);
    } else {
        rc = snag_session_history_snapshot(reader->store, view, request->session_id,
            &source->incomplete, error, size);
    }
    source->verified = rc == 0;
    return rc;
}

struct read_page {
    struct snag_vm_reader *reader;
    json_t *events;
    const json_t *route;
    unsigned int verbosity;
    bool project;
};

static bool
conversation_event(const json_t *route, const char *type, const json_t *data)
{
    if (!route) return strcmp(type, "irc_event") && strcmp(type, "irc_event_v2");
    if (json_object_get(route, "room")) {
        if (strcmp(type, "irc_event") && strcmp(type, "irc_event_v2")) return false;
        enum snag_irc_casemapping mapping = (enum snag_irc_casemapping)
            json_integer_value(json_object_get(route, "casemapping"));
        const char *room = snag_json_string(data, "room");
        if (!room || !snag_irc_name_equal(mapping, snag_json_string(route, "room"), room))
            return false;
        /* Public channel history is shared by the two local identities.
         * Legacy room records predate durable connection IDs. */
        if (!strcmp(type, "irc_event"))
            return json_equal(json_object_get(route, "endpoint"),
                json_object_get(data, "endpoint"));
        const json_t *routing = json_object_get(data, "routing");
        const char *kind = snag_json_string(routing, "conversation_kind");
        return kind && !strcmp(kind, "channel") &&
            json_equal(json_object_get(route, "connection"),
                json_object_get(routing, "connection_id"));
    }
    if (strcmp(type, "irc_event_v2")) return false;
    const json_t *routing = json_object_get(data, "routing");
    if (!json_object_get(route, "peer")) {
        const char *kind = snag_json_string(routing, "conversation_kind");
        return kind && !strcmp(kind, "connection") &&
            json_equal(json_object_get(route, "connection"),
                json_object_get(routing, "connection_id"));
    }
    static const char *const fields[] = {"connection", "conversation", "identity"};
    static const char *const stored[] = {"connection_id", "conversation_id", "identity"};
    for (size_t i = 0u; i < sizeof(fields) / sizeof(fields[0]); ++i)
        if (!json_equal(json_object_get(route, fields[i]),
            json_object_get(routing, stored[i]))) return false;
    return true;
}

static int
read_event(void *opaque, const struct snag_session *state, uint64_t seq,
    const char *type, const json_t *data, char *error, size_t size)
{
    struct read_page *page = opaque;
    (void)state;
    if (read_canceled(page->reader)) {
        return snag_fail(error, size, ECANCELED, "history read canceled");
    }
    if (!conversation_event(page->route, type, data)) return 0;
    if (page->project) {
        /* Lower levels load bounded previews from tool result references. */
        if (page->verbosity < 3u && !strcmp(type, "process_output")) return 0;
        json_t *event = json_pack("{s:I,s:s,s:O}", "seq", (json_int_t)seq,
            "type", type, "data", data);
        return event ? json_array_append_new(page->events, event) : -1;
    }
    char *text = snag_history_event_data(seq, type, data, &page->reader->secrets, error, size);
    if (!text) return -1;
    json_error_t parse;
    json_t *filtered = json_loads(text, JSON_REJECT_DUPLICATES, &parse);
    free(text);
    if (!filtered) return snag_fail(error, size, EINVAL, "invalid public history projection");
    json_t *event = json_pack("{s:I,s:s,s:o}", "seq", (json_int_t)seq,
        "type", type, "data", filtered);
    if (event && snag_vm_public_source_bytes(event, data) < 0) {
        json_decref(event);
        return -1;
    }
    if (!event || json_array_append_new(page->events, event) < 0) return -1;
    return 0;
}

struct public_dependencies {
    struct snag_vm_reader *reader;
    json_t *responses, *pending;
};

static json_t *
public_snapshot(const char *state, uint64_t seq, const json_t *data)
{
    const json_t *items = json_object_get(data, !strcmp(state, "complete") ?
        "items" : "partial_public");
    if (!json_is_array(items)) { errno = EINVAL; return NULL; }
    json_t *public = json_array();
    if (!public) return NULL;
    for (size_t i = 0u; i < json_array_size(items); ++i) {
        json_t *item = json_array_get(items, i);
        if (!snag_string_in(snag_json_string(item, "kind"), "assistant refusal")) continue;
        if (json_array_append(public, item) < 0) { json_decref(public); return NULL; }
    }
    return json_pack("{s:s,s:I,s:o}", "state", state, "seq", (json_int_t)seq, "items", public);
}

static int
resolve_public(void *opaque, const struct snag_session *session, uint64_t seq,
    const char *type, const json_t *data, char *error, size_t size)
{
    struct public_dependencies *public = opaque;
    (void)session;
    (void)error;
    (void)size;
    if (read_canceled(public->reader)) return snag_errno(ECANCELED);
    const char *id = snag_json_string(data, "response_id");
    json_t *response = id ? json_object_get(public->pending, id) : NULL;
    const char *state = snag_vm_public_state(type);
    if (response && state) {
        json_t *snapshot = public_snapshot(state, seq, data);
        if (!snapshot || json_object_set_new(response, "snapshot", snapshot) < 0) return -1;
        (void)json_object_del(public->pending, id);
    }
    return json_object_size(public->pending) ? 0 : SNAG_JOURNAL_STOP_AFTER;
}

static int
prior_public(void *opaque, const struct snag_session *session, uint64_t seq,
    const char *type, const json_t *data, char *error, size_t size)
{
    struct public_dependencies *public = opaque;
    (void)session;
    (void)seq;
    (void)error;
    (void)size;
    if (read_canceled(public->reader)) return snag_errno(ECANCELED);
    const char *id = snag_json_string(data, "response_id");
    json_t *response = id ? json_object_get(public->pending, id) : NULL;
    if (response && !strcmp(type, "response_started")) {
        (void)json_object_del(public->pending, id);
    } else if (response && !strcmp(type, "response_output")) {
        uint64_t ordinal, offset;
        const char *text = snag_json_string(json_object_get(data, "item"), "text");
        if (!text || snag_json_integer_u64(data, "index", &ordinal) < 0 ||
            snag_json_integer_u64(data, "offset", &offset) < 0 ||
            offset > (uint64_t)INT64_MAX - strlen(text)) return snag_errno(EINVAL);
        char index[24];
        (void)snprintf(index, sizeof(index), "%llu", (unsigned long long)ordinal);
        json_t *before = json_object_get(response, "before");
        if (!json_object_get(before, index) && json_object_set_new(before, index,
            json_integer((json_int_t)(offset + strlen(text)))) < 0) return -1;
    }
    return json_object_size(public->pending) ? 0 : SNAG_JOURNAL_STOP_AFTER;
}

static int
load_public(struct snag_vm_reader *reader, json_t *events, char *error, size_t size)
{
    struct public_dependencies public = {.reader = reader,
        .responses = json_object(), .pending = json_object()};
    struct snag_session *view = &reader->current->session;
    struct snag_journal_cursor position = view->history_cursor;
    int rc = -1;
    if (!public.responses || !public.pending) goto out;
    size_t count = json_array_size(events);
    for (size_t i = 0u; i < count; ++i) {
        if (read_canceled(reader)) { errno = ECANCELED; goto out; }
        const json_t *event = json_array_get(events, i);
        const char *type = snag_json_string(event, "type");
        const char *state = snag_vm_public_state(type);
        if (!state && strcmp(type, "response_output")) continue;
        const json_t *data = json_object_get(event, "data");
        const char *id = snag_json_string(data, "response_id");
        if (!id || !snag_hex_is_lower(id, SNAG_ID_HEX_LEN)) { errno = EINVAL; goto out; }
        json_t *response = json_object_get(public.responses, id);
        if (!response) {
            if (json_object_set_new(public.responses, id, json_pack("{s:{}}", "before")) < 0)
                goto out;
            response = json_object_get(public.responses, id);
        }
        if (state) {
            uint64_t seq = (uint64_t)json_integer_value(json_object_get(event, "seq"));
            json_t *snapshot = public_snapshot(state, seq, data);
            if (!snapshot || json_object_set_new(response, "snapshot", snapshot) < 0 ||
                json_object_set(public.pending, id, response) < 0) goto out;
        }
    }
    /* A response beginning inside this page has no earlier streamed prefix. */
    for (size_t i = 0u; i < count; ++i) {
        if (read_canceled(reader)) { errno = ECANCELED; goto out; }
        const json_t *event = json_array_get(events, i);
        if (strcmp(snag_json_string(event, "type"), "response_started")) continue;
        const char *id = snag_json_string(json_object_get(event, "data"), "response_id");
        if (id) (void)json_object_del(public.pending, id);
    }
    uint64_t before = (uint64_t)json_integer_value(
        json_object_get(json_array_get(events, 0u), "seq"));
    while (before > 1u && json_object_size(public.pending)) {
        if (snag_session_each_event_reverse(view, before, SNAG_JOURNAL_PAGE_BYTES,
            prior_public, &public, &before, error, size) < 0) goto out;
    }
    (void)json_object_clear(public.pending);
    const char *id;
    json_t *response;
    json_object_foreach(public.responses, id, response) {
        if (!json_object_get(response, "snapshot") &&
            json_object_set(public.pending, id, response) < 0) goto out;
    }
    struct source_view *source = reader->current;
    response = json_object_get(public.pending, source->public_id);
    if (response && source->public_snapshot) {
        if (json_object_set(response, "snapshot", source->public_snapshot) < 0) goto out;
        (void)json_object_del(public.pending, source->public_id);
    }
    if (count && json_object_size(public.pending)) {
        struct snag_journal_cursor after;
        uint64_t last = (uint64_t)json_integer_value(
            json_object_get(json_array_get(events, count - 1u), "seq"));
        if (snag_session_history_cursor_before(view, last + 1u, &after, error, size) < 0) goto out;
        /* Native responses are sequential. Keep the last response crossing a
         * page edge so neighboring pages and growing tails reuse its scan. */
        const char *cache = json_object_size(public.pending) == 1u ?
            json_object_iter_key(json_object_iter(public.pending)) : NULL;
        char cache_id[SNAG_ID_HEX_LEN + 1u] = "";
        if (cache) {
            memcpy(cache_id, cache, sizeof(cache_id));
            if (!strcmp(cache_id, source->public_id) &&
                source->public_scanned.next_seq > after.next_seq) after = source->public_scanned;
        }
        while (after.offset < view->log_end && json_object_size(public.pending)) {
            if (snag_session_each_event_forward(view, &after, SNAG_JOURNAL_PAGE_BYTES,
                resolve_public, &public, error, size) < 0) goto out;
        }
        if (*cache_id) {
            response = json_object_get(public.responses, cache_id);
            json_t *snapshot = json_incref(json_object_get(response, "snapshot"));
            json_decref(source->public_snapshot);
            source->public_snapshot = snapshot;
            source->public_scanned = after;
            memcpy(source->public_id, cache_id, sizeof(cache_id));
        }
    }
    for (size_t i = 0u; i < count; ++i) {
        if (read_canceled(reader)) { errno = ECANCELED; goto out; }
        json_t *event = json_array_get(events, i);
        const char *owner = snag_json_string(json_object_get(event, "data"), "response_id");
        response = owner ? json_object_get(public.responses, owner) : NULL;
        if (!response) continue;
        json_t *snapshot = json_object_get(response, "snapshot");
        if (snapshot && json_object_set(event, "public_snapshot", snapshot) < 0) goto out;
        if (snag_vm_public_state(snag_json_string(event, "type")) &&
            json_object_set(event, "public_before", json_object_get(response, "before")) < 0)
            goto out;
    }
    rc = 0;
out:
    view->history_cursor = position;
    json_decref(public.responses);
    json_decref(public.pending);
    return rc;
}

struct call_dependencies {
    struct snag_vm_reader *reader;
    json_t *pending;
};

static int
resolve_calls(void *opaque, const struct snag_session *state, uint64_t seq,
    const char *type, const json_t *data, char *error, size_t size)
{
    struct call_dependencies *calls = opaque;
    (void)state;
    (void)seq;
    (void)error;
    (void)size;
    if (read_canceled(calls->reader)) return snag_errno(ECANCELED);
    if (strcmp(type, "response_completed")) return 0;
    const char *turn = snag_json_string(data, "turn_id");
    const json_t *items = json_object_get(data, "items");
    for (size_t i = 0u; turn && i < json_array_size(items); ++i) {
        json_t *call = json_array_get(items, i);
        const char *id = snag_json_string(call, "call_id");
        if (!snag_string_in(snag_json_string(call, "kind"), "tool_call") || !id) continue;
        for (size_t n = json_array_size(calls->pending); n; --n) {
            json_t *event = json_array_get(calls->pending, n - 1u);
            const json_t *tool = json_object_get(event, "data");
            const char *owner = snag_json_string(tool, "turn_id");
            const char *wanted = snag_json_string(tool, "call_id");
            if (!owner || !wanted || strcmp(owner, turn) || strcmp(wanted, id)) continue;
            if (json_object_set(event, "call", call) < 0) return -1;
            (void)json_array_remove(calls->pending, n - 1u);
        }
    }
    return json_array_size(calls->pending) ? 0 : SNAG_JOURNAL_STOP_AFTER;
}

static int
load_calls(struct snag_vm_reader *reader, json_t *events, char *error, size_t size)
{
    struct call_dependencies calls = {.reader = reader, .pending = json_array()};
    if (!calls.pending) return -1;
    int rc = -1;
    /* Walk the page backwards so reused provider call IDs resolve to their
     * nearest preceding response in the same turn, before any redaction. */
    for (size_t n = json_array_size(events); n; --n) {
        if (read_canceled(reader)) { errno = ECANCELED; goto out; }
        json_t *event = json_array_get(events, n - 1u);
        const char *type = snag_json_string(event, "type");
        if (snag_string_in(type, "tool_started tool_finished")) {
            if (!json_object_get(event, "call") && json_array_append(calls.pending, event) < 0)
                goto out;
        } else if (resolve_calls(&calls, NULL, 0u, type,
            json_object_get(event, "data"), error, size) < 0) goto out;
    }
    struct snag_session *view = &reader->current->session;
    struct snag_journal_cursor position = view->history_cursor;
    uint64_t before = (uint64_t)json_integer_value(
        json_object_get(json_array_get(events, 0u), "seq"));
    rc = 0;
    while (before > 1u && json_array_size(calls.pending)) {
        rc = snag_session_each_event_reverse(view, before, SNAG_JOURNAL_PAGE_BYTES,
            resolve_calls, &calls, &before, error, size);
        if (rc < 0) break;
    }
    view->history_cursor = position;
    for (size_t n = 0u; !rc && n < json_array_size(calls.pending); ++n)
        rc = json_object_set(json_array_get(calls.pending, n), "call", json_null());
out:
    json_decref(calls.pending);
    return rc;
}

struct output_preview {
    struct snag_vm_reader *reader;
    const char *handle;
    uint64_t from[2], end[2], needed[2], covered[2], end_seq;
    json_t *events;
};

static bool
preview_ready(const struct output_preview *preview)
{
    return preview->covered[0] >= preview->needed[0] &&
        preview->covered[1] >= preview->needed[1];
}

static int
preview_event(void *opaque, const struct snag_session *state, uint64_t seq,
    const char *type, const json_t *data, char *error, size_t size)
{
    struct output_preview *preview = opaque;
    (void)state;
    (void)error;
    (void)size;
    if (read_canceled(preview->reader)) return snag_errno(ECANCELED);
    if (seq >= preview->end_seq) return snag_errno(EINVAL);
    const char *handle = snag_json_string(data, "handle");
    if (strcmp(type, "process_output") || !handle || strcmp(handle, preview->handle)) return 0;
    uint64_t stream, offset;
    if (snag_json_integer_u64(data, "stream", &stream) < 0 || stream > 1u ||
        snag_json_integer_u64(data, "offset", &offset) < 0) return snag_errno(EINVAL);
    if (offset < preview->from[stream] || preview->covered[stream] >= preview->needed[stream])
        return 0;
    struct snag_buf decoded = {.max = 16384u};
    int rc = snag_process_output_decode(data, &decoded);
    if (!rc && (offset != preview->covered[stream] || offset > preview->end[stream] ||
        decoded.len > preview->end[stream] - offset)) rc = snag_errno(EINVAL);
    if (!rc) {
        preview->covered[stream] += decoded.len;
        json_t *event = json_pack("{s:I,s:s,s:O}", "seq", (json_int_t)seq,
            "type", type, "data", data);
        rc = event ? json_array_append_new(preview->events, event) : -1;
    }
    snag_secret_clear(decoded.data, decoded.len);
    snag_buf_free(&decoded);
    return rc < 0 ? rc : preview_ready(preview) ? SNAG_JOURNAL_STOP_AFTER : 0;
}

static int
load_preview(struct snag_vm_reader *reader, json_t *event, const json_t *ref,
    unsigned int columns, char *error, size_t size)
{
    struct output_preview preview = {.reader = reader, .handle = snag_json_string(ref, "handle")};
    if (!preview.handle) return snag_errno(EINVAL);
    size_t limit = snag_presentation_limit(SNAG_PRESENT_OUTPUT, 2u), secret = 0u;
    for (size_t i = 0u; i < reader->secrets.count; ++i) {
        size_t length = strlen(reader->secrets.values[i]);
        if (length > secret) secret = length;
    }
    /* Four bytes per displayed scalar plus redaction/UTF-8 lookahead. Keep
     * whole output records; the extra final record is at most 16KiB. */
    if (secret > SIZE_MAX - 3u || limit > (SIZE_MAX - secret - 3u) / 4u)
        return snag_errno(EOVERFLOW);
    size_t capture = limit * 4u + secret + 3u;
    const char *starts[] = {"stdout_start", "stderr_start"};
    const char *ends[] = {"stdout_end", "stderr_end"};
    for (size_t i = 0u; i < 2u; ++i) {
        if (snag_json_integer_u64(ref, starts[i], &preview.from[i]) < 0 ||
            snag_json_integer_u64(ref, ends[i], &preview.end[i]) < 0 ||
            preview.from[i] > preview.end[i]) return snag_errno(EINVAL);
        uint64_t bytes = preview.end[i] - preview.from[i];
        preview.needed[i] = preview.from[i] + (bytes < capture ? bytes : capture);
        preview.covered[i] = preview.from[i];
    }
    if (preview_ready(&preview)) return 0;
    struct snag_session *view = &reader->current->session;
    struct snag_journal_cursor cursor, tail;
    uint64_t sequence = (uint64_t)json_integer_value(json_object_get(event, "seq"));
    if (snag_session_history_output_range(view, sequence, ref,
        &cursor, &tail, error, size) < 0) return -1;
    preview.end_seq = tail.next_seq;
    int rc = -1;
    json_t *blocks = NULL;
    struct snag_buf text = {.max = SNAG_MEMORY_LIMIT / 2u};
    preview.events = json_array();
    if (!preview.events) goto out;
    while (cursor.next_seq < tail.next_seq && !preview_ready(&preview)) {
        if (snag_session_each_event_forward(view, &cursor, SNAG_JOURNAL_PAGE_BYTES,
            preview_event, &preview, error, size) < 0) goto out;
    }
    if (!preview_ready(&preview) || cursor.next_seq > tail.next_seq) {
        errno = EINVAL;
        goto out;
    }
    blocks = snag_vm_transcript_blocks(preview.events, 3u, columns, false, true, true,
        &reader->secrets, read_canceled, reader, error, size);
    if (!blocks) goto out;
    size_t characters = 0u;
    bool truncated = preview.covered[0] < preview.end[0] || preview.covered[1] < preview.end[1];
    for (size_t i = 0u; i < json_array_size(blocks); ++i) {
        const json_t *block = json_array_get(blocks, i);
        const char *body = snag_json_string(block, "text");
        size_t length = strlen(body), shown = 0u;
        while (shown < length && characters < limit) {
            uint32_t cp;
            size_t unit = snag_utf8_decode((const unsigned char *)body + shown,
                length - shown, &cp);
            if (!unit) { errno = EILSEQ; goto out; }
            shown += unit;
            ++characters;
        }
        if (shown && (snag_buf_printf(&text, "[%s]\n", snag_json_string(block, "label")) < 0 ||
            snag_buf_append(&text, body, shown) < 0 || snag_buf_putc(&text, '\n') < 0)) goto out;
        truncated = truncated || shown < length;
    }
    if (json_object_set_new(event, "output_preview", json_stringn(
        text.data ? (const char *)text.data : "", text.len)) < 0 ||
        json_object_set_new(event, "preview_truncated", json_boolean(truncated)) < 0) goto out;
    rc = 0;
out:
    json_decref(blocks);
    json_decref(preview.events);
    snag_buf_free(&text);
    return rc;
}

static void
read_page(struct snag_vm_reader *, struct snag_vm_read_result *);

static void
scan_progress(struct snag_vm_reader *reader, uint64_t bytes, uint64_t events, uint64_t total)
{
    (void)pthread_mutex_lock(&reader->lock);
    reader->progress_generation = reader->working_generation;
    reader->progress_bytes = bytes;
    reader->progress_events = events;
    reader->progress_total = total;
    snag_wakeup_send(reader->wake[1]);
    (void)pthread_mutex_unlock(&reader->lock);
}

static int
scan_rows(struct snag_vm_reader *reader, struct snag_vm_read_result *result)
{
    const struct snag_vm_navigation_request *motion = &result->request.navigation;
    bool down = motion->kind == SNAG_VM_NAV_ROW_DOWN;
    struct snag_vm_read_result page = {.request = result->request};
    page.request.navigation.kind = SNAG_VM_NAV_NONE;
    page.request.project = true;
    uint64_t remaining = motion->count;
    bool started = false;
    int rc = -1;
    for (;;) {
        read_page(reader, &page);
        if (page.error_number) {
            memcpy(result->error, page.error, sizeof(result->error));
            errno = page.error_number;
            break;
        }
        if (!started) {
            result->best_effort = page.best_effort;
            result->incomplete = page.incomplete;
        }
        result->tail = page.tail;
        page.request.tail = page.tail;
        page.request.trusted_tail = page.request.pin_tail = true;
        size_t rows = snag_vm_document_rows(page.document);
        size_t at = down ? 0u : rows ? rows - 1u : 0u;
        if (!started) {
            at = snag_vm_document_locate_source(page.document, motion->start.key,
                motion->start.seq, motion->start.byte, motion->start.heading);
            struct snag_vm_document_row row;
            if (snag_vm_document_row(page.document, at, &row) < 0 ||
                strcmp(snag_json_string(snag_vm_document_block(page.document, row.block),
                    "key"), motion->start.key) || row.heading != motion->start.heading ||
                motion->start.byte < snag_vm_document_source(page.document, &row, row.begin) ||
                motion->start.byte > snag_vm_document_source(page.document, &row, row.end)) {
                /* Forward and reverse byte pages can end at different records.
                 * Reach the saved start before counting the requested rows. */
                if (!page.request.reverse && page.more &&
                    motion->start.seq >= page.cursor.next_seq) {
                    page.request.cursor = page.cursor;
                    goto next_page;
                }
                errno = ESTALE;
                break;
            }
            started = true;
        }
        if (rows) {
            size_t available = down ? rows - at - 1u : at;
            size_t step = remaining < available ? (size_t)remaining : available;
            at = down ? at + step : at - step;
            remaining -= step;
            struct snag_vm_document_row row;
            if (snag_vm_document_row(page.document, at, &row) < 0) break;
            const json_t *block = snag_vm_document_block(page.document, row.block);
            const char *text = snag_vm_document_text(page.document, &row);
            size_t column = motion->column > SIZE_MAX - row.column ? SIZE_MAX :
                row.column + motion->column;
            size_t byte = snag_vm_text_at_column(text, strlen(text), row.begin, column, false);
            if (byte >= row.end) byte = row.end > row.begin ?
                snag_vm_text_previous(text, strlen(text), row.end) : row.begin;
            result->match = (struct snag_vm_anchor){
                .seq = (uint64_t)json_integer_value(json_object_get(block, "seq")),
                .byte = snag_vm_document_source(page.document, &row, byte),
                .order = snag_vm_search_order(block), .heading = row.heading};
            (void)snprintf(result->match.key, sizeof(result->match.key), "%s",
                snag_json_string(block, "key"));
        }
        scan_progress(reader, (uint64_t)page.cursor.offset, page.cursor.next_seq,
            (uint64_t)page.tail.offset);
        struct snag_journal_cursor begin = page.request.reverse ? page.cursor : page.request.cursor;
        if ((!remaining && rows) || (down ? !page.more : begin.next_seq <= 1u)) { rc = 0; break; }
        if (rows) --remaining;
        page.request.reverse = !down;
        page.request.cursor = page.cursor;
        page.request.before_seq = down ? 0u : begin.next_seq;
next_page:
        snag_vm_document_free(page.document);
        page.document = NULL;
        json_decref(page.blocks);
        page.blocks = NULL;
    }
    result->found = rc == 0;
    snag_vm_document_free(page.document);
    json_decref(page.blocks);
    json_decref(page.events);
    return rc;
}

static int
scan_text(struct snag_vm_reader *reader, struct snag_vm_read_result *result)
{
    const struct snag_vm_read_request *request = &result->request;
    bool copying = request->selection.kind != SNAG_VM_SELECT_NONE;
    bool navigating = request->navigation.kind != SNAG_VM_NAV_NONE;
    struct snag_vm_search *search = copying || navigating ? NULL :
        snag_vm_search_open(request->query, request->ignorecase, request->search_reverse,
            &request->search_start);
    struct snag_vm_copy *copy = copying ? snag_vm_copy_open(&request->selection,
        reader->store->root_fd) : NULL;
    struct snag_vm_navigation *navigation = navigating ?
        snag_vm_navigation_open(&request->navigation) : NULL;
    if (copying ? !copy : navigating ? !navigation : !search) return -1;
    int rc = -1;
    struct snag_vm_read_result page = {.request = *request};
    page.request.query = NULL;
    page.request.selection.kind = SNAG_VM_SELECT_NONE;
    page.request.navigation.kind = SNAG_VM_NAV_NONE;
    page.request.blocks_only = page.request.project = true;
    page.request.reverse = page.request.tail_only = page.request.if_changed = false;
    page.request.before_seq = 0u;
    struct snag_journal_cursor begin = navigating ? request->cursor :
        (struct snag_journal_cursor){0};
    page.request.cursor = begin;
    char first_key[160] = "";
    bool certified = false;
    unsigned int pass = 0u;
again:
    for (;;) {
        read_page(reader, &page);
        if (page.error_number) {
            (void)snprintf(result->error, sizeof(result->error), "%s incomplete: %.230s",
                copying ? "Copy" : navigating ? "Navigation" : "Search", page.error);
            errno = page.error_number;
            goto out;
        }
        result->tail = page.tail;
        if (!certified) {
            certified = true;
            /* Later pages pin this observed bound using the cursor API. Keep
             * its original certification, including an old owner's suffix. */
            result->best_effort = page.best_effort;
            result->incomplete = page.incomplete;
        }
        page.request.tail = page.tail;
        page.request.trusted_tail = request->kind == SNAG_VM_READ_HISTORY;
        page.request.pin_tail = page.request.trusted_tail;
        page.request.refresh = false;
        bool done = false;
        for (size_t i = 0u;; ++i) {
            const json_t *block = page.document ? snag_vm_document_block(page.document, i) :
                json_array_get(page.blocks, i);
            if (!block) break;
            if (navigating && !first_key[0])
                (void)snprintf(first_key, sizeof(first_key), "%s", snag_json_string(block, "key"));
            int step = copying ? snag_vm_copy_block(copy, block, read_canceled, reader) :
                navigating ? snag_vm_navigation_block(navigation, block, read_canceled, reader) :
                snag_vm_search_block(search, block, read_canceled, reader);
            if (step < 0) goto out;
            if (step > 0) { done = true; break; }
        }
        scan_progress(reader, (uint64_t)page.cursor.offset,
            page.cursor.next_seq ? page.cursor.next_seq - 1u : 0u, (uint64_t)page.tail.offset);
        if (!page.more || done) break;
        page.request.cursor = page.cursor;
        json_decref(page.blocks);
        page.blocks = NULL;
    }
    if (copying) {
        if (read_canceled(reader)) { errno = ECANCELED; goto out; }
        if (snag_vm_copy_finish(copy, &result->copied, read_canceled, reader) < 0) goto out;
    } else if (navigating) {
        int status = snag_vm_navigation_finish(navigation, &result->match);
        bool restart = false;
        /* The first visible field may start mid-line or mid-word. A result
         * there needs the retained prefix to distinguish a clamp from a hit. */
        if (begin.next_seq > 1u && ((status < 0 && errno == ESTALE) ||
            (!status && !strcmp(first_key, result->match.key)))) {
            snag_vm_navigation_close(navigation);
            navigation = snag_vm_navigation_open(&request->navigation);
            if (!navigation) goto out;
            begin = (struct snag_journal_cursor){0};
            first_key[0] = 0;
            pass = 0u;
            restart = true;
            status = 1;
        }
        if (status < 0) goto out;
        if (status) {
            if (!restart && pass++) { errno = ELOOP; goto out; }
            json_decref(page.blocks);
            page.blocks = NULL;
            snag_vm_document_free(page.document);
            page.document = NULL;
            page.request.cursor = begin;
            goto again;
        }
        result->found = true;
    } else result->found = snag_vm_search_result(search, &result->match, &result->wrapped);
    rc = 0;
out:
    snag_vm_search_close(search);
    snag_vm_copy_close(copy);
    snag_vm_navigation_close(navigation);
    json_decref(page.events);
    json_decref(page.blocks);
    snag_vm_document_free(page.document);
    return rc;
}

/* Command receipts belong to the operator transcript. Their immutable report
 * files are read on this worker, never on the keyboard/rendering thread. */
static int
project_reports(struct snag_vm_reader *reader, struct snag_vm_read_result *result)
{
    const struct snag_vm_read_request *request = &result->request;
    uint64_t first = request->reverse ? result->cursor.next_seq : request->cursor.next_seq;
    uint64_t end = request->reverse ? request->before_seq ? request->before_seq :
        result->tail.next_seq : result->cursor.next_seq;
    size_t count = json_array_size(request->inline_reports), at = 0u;
    for (size_t i = 0u; i < count; ++i) {
        const json_t *item = json_array_get(request->inline_reports, i);
        uint64_t seq = (uint64_t)json_integer_value(json_object_get(item, "seq"));
        if (seq < first || seq >= end) continue;
        struct snag_vm_document *document = snag_vm_report_read(reader->store,
            snag_json_string(item, "session"), json_object_get(item, "report"),
            request->columns, &reader->secrets, read_canceled, reader,
            result->error, sizeof(result->error));
        if (!document) return -1;
        json_t *block = json_deep_copy(snag_vm_document_block(document, 0u));
        snag_vm_document_free(document);
        if (!block) return -1;
        while (at < json_array_size(result->blocks) &&
            (uint64_t)json_integer_value(json_object_get(
                json_array_get(result->blocks, at), "seq")) <= seq) ++at;
        int rc = json_object_set_new(block, "seq", json_integer((json_int_t)seq));
        if (!rc) rc = json_object_set_new(block, "kind", json_string("command"));
        if (!rc) rc = json_object_set_new(block, "ordinal", json_integer((json_int_t)i));
        if (!rc) rc = json_array_insert(result->blocks, at++, block);
        json_decref(block);
        if (rc < 0) return -1;
    }
    return 0;
}

static int
project_history(struct snag_vm_reader *reader, struct snag_vm_read_result *result)
{
    const struct snag_vm_read_request *request = &result->request;
    json_t *events = json_array();
    if (!events) return -1;
    int rc = -1;
    size_t count = json_array_size(result->events);
    for (size_t i = 0u; i < count; ++i)
        if (json_array_append(events, json_array_get(result->events,
            request->reverse ? count - i - 1u : i)) < 0) goto out;
    if (load_public(reader, events, result->error, sizeof(result->error)) < 0 ||
        (request->verbosity && load_calls(reader, events,
            result->error, sizeof(result->error)) < 0)) goto out;
    for (size_t i = 0u; request->verbosity == 2u && i < count; ++i) {
        json_t *event = json_array_get(events, i);
        if (strcmp(snag_json_string(event, "type"), "tool_finished")) continue;
        const json_t *ref = json_object_get(json_object_get(
            json_object_get(event, "data"), "result"), "output_ref");
        if (ref && load_preview(reader, event, ref, request->columns,
            result->error, sizeof(result->error)) < 0) goto out;
    }
    json_decref(result->blocks);
    result->blocks = snag_vm_transcript_blocks(events, request->verbosity,
        request->columns, request->plain, request->no_color, request->blocks_only, &reader->secrets,
        read_canceled, reader,
        result->error, sizeof(result->error));
    if (!result->blocks || project_reports(reader, result) < 0) goto out;
    if (!request->blocks_only) {
        snag_vm_document_free(result->document);
        result->document = snag_vm_document_open(result->blocks,
            request->columns ? request->columns : 80u, read_canceled, reader);
        if (!result->document) goto out;
    }
    rc = 0;
out:
    json_decref(events);
    return rc;
}

static void
read_page(struct snag_vm_reader *reader, struct snag_vm_read_result *result)
{
    const struct snag_vm_read_request *request = &result->request;
    retain_views(reader, request);
    if (request->navigation.kind == SNAG_VM_NAV_ROW_UP ||
        request->navigation.kind == SNAG_VM_NAV_ROW_DOWN) {
        if (scan_rows(reader, result) < 0) goto failed;
        return;
    }
    if (request->query || request->selection.kind || request->navigation.kind) {
        if (scan_text(reader, result) < 0) goto failed;
        return;
    }
    if (request->kind == SNAG_VM_READ_REPORTS) {
        result->catalog = snag_vm_report_catalog(reader->store, request->session_id,
            read_canceled, reader, &result->incomplete, result->error, sizeof(result->error));
        if (!result->catalog) goto failed;
        return;
    }
    if (request->kind == SNAG_VM_READ_REPORT) {
        result->document = snag_vm_report_read(reader->store, request->session_id,
            request->report, request->columns, &reader->secrets, read_canceled, reader,
            result->error, sizeof(result->error));
        if (!result->document) goto failed;
        return;
    }
    if (request->kind == SNAG_VM_READ_SESSIONS) {
        struct snag_session *owned = NULL;
        if (request->owned_session_id[0]) {
            /* This identity-only placeholder owns no descriptors or state. */
            owned = calloc(1u, sizeof(*owned));
            if (!owned) goto failed;
            memcpy(owned->id, request->owned_session_id, sizeof(owned->id));
        }
        json_t *catalog = snag_store_catalog(reader->store, owned, request->stored_limit,
            read_canceled, reader, result->error, sizeof(result->error));
        free(owned);
        char *encoded = catalog ? json_dumps(catalog, JSON_COMPACT) : NULL;
        json_decref(catalog);
        if (!encoded) goto failed;
        struct snag_buf filtered = {.max = SNAG_MEMORY_LIMIT / 4u};
        int rc = snag_wire_json_redact_bounded((const unsigned char *)encoded, strlen(encoded),
            filtered.max, &reader->secrets, &filtered, result->error, sizeof(result->error));
        snag_secret_bytes_free(encoded);
        json_error_t parse;
        if (!rc) result->catalog = json_loadb((const char *)filtered.data, filtered.len, 0u, &parse);
        snag_buf_free(&filtered);
        if (!result->catalog) goto failed;
        return;
    }
    if (view_open(reader, request, result->error, sizeof(result->error)) < 0) goto failed;
    struct snag_session *view = &reader->current->session;
    result->tail = view_tail(view);
    result->best_effort = reader->current->best_effort;
    result->incomplete = reader->current->incomplete;
    result->unchanged = request->previous.offset == result->tail.offset &&
        request->previous.next_seq == result->tail.next_seq &&
        !strcmp(request->previous.prev_sha256, result->tail.prev_sha256);
    if (request->tail_only || (request->if_changed && result->unchanged)) return;
    result->events = json_array();
    if (!result->events) goto failed;
    struct read_page page = {.reader = reader, .events = result->events,
        .route = request->route, .project = request->project, .verbosity = request->verbosity};
    uint64_t before = request->before_seq;
    result->cursor = request->cursor;
    result->end = result->tail;
    if (request->reverse && before && snag_session_history_cursor_before(view, before,
        &result->end, result->error, sizeof(result->error)) < 0) goto failed;
    do {
        if (request->reverse) {
            if (snag_session_each_event_reverse(view, before,
                SNAG_JOURNAL_PAGE_BYTES, read_event, &page, &before,
                result->error, sizeof(result->error)) < 0) goto failed;
            result->cursor = view->history_cursor;
            result->more = before != 0u;
        } else {
            if (snag_session_each_event_forward(view, &result->cursor,
                SNAG_JOURNAL_PAGE_BYTES, read_event, &page,
                result->error, sizeof(result->error)) < 0) goto failed;
            result->more = result->cursor.next_seq < result->tail.next_seq;
        }
        if (request->project && project_history(reader, result) < 0) goto failed;
    } while (result->more && ((request->route && !json_array_size(result->events)) ||
        (request->project && !request->blocks_only && request->rows &&
         snag_vm_document_rows(result->document) < request->rows)));
    if (request->project) {
        json_decref(result->events);
        result->events = NULL;
    }
    return;
failed:
    result->error_number = errno ? errno : EIO;
    if (!result->error[0]) {
        (void)snag_errorf(result->error, sizeof(result->error), "history read: %s",
            strerror(result->error_number));
    }
    json_decref(result->events);
    result->events = NULL;
    /* Superseding a page keeps its previously verified source pinned. An
     * interrupted initial open has no verified bound and must be discarded. */
    if (result->error_number != ECANCELED || !reader->current || !reader->current->verified)
        view_close(reader);
}

static void *
reader_main(void *opaque)
{
    struct snag_vm_reader *reader = opaque;
    for (;;) {
        (void)pthread_mutex_lock(&reader->lock);
        while (!reader->pending && !atomic_load(&reader->stop)) {
            (void)pthread_cond_wait(&reader->condition, &reader->lock);
        }
        if (atomic_load(&reader->stop)) {
            (void)pthread_mutex_unlock(&reader->lock);
            break;
        }
        /* Request allocation happens in the caller so OOM is synchronous. */
        struct snag_vm_read_result *result = reader->pending;
        reader->pending = NULL;
        reader->working_generation = result->generation;
        (void)pthread_mutex_unlock(&reader->lock);
        read_page(reader, result);
        (void)pthread_mutex_lock(&reader->lock);
        if (!read_canceled(reader)) {
            reader->completed = result;
            result = NULL;
            snag_wakeup_send(reader->wake[1]);
        }
        (void)pthread_mutex_unlock(&reader->lock);
        snag_vm_read_result_free(result);
    }
    while (reader->views) {
        reader->current = reader->views;
        view_close(reader);
    }
    return NULL;
}

static void
free_reader(struct snag_vm_reader *reader)
{
    for (size_t i = 0u; i < reader->secrets.count; ++i) {
        snag_secret_clear(reader->secret_values[i], strlen(reader->secret_values[i]));
        free(reader->secret_values[i]);
    }
    free(reader->secret_values);
    snag_wakeup_close(reader->wake);
    snag_vm_read_result_free(reader->pending);
    snag_vm_read_result_free(reader->completed);
    free(reader);
}

struct snag_vm_reader *
snag_vm_reader_open(struct snag_store *store, const struct snag_wire_secrets *secrets,
    char *error, size_t size)
{
    if (!store || store->sessions_fd < 0 || !store->root_path) {
        (void)snag_fail(error, size, EINVAL, "history reader requires an open store");
        return NULL;
    }
    struct snag_vm_reader *reader = calloc(1u, sizeof(*reader));
    if (!reader) return NULL;
    reader->store = store;
    reader->wake[0] = reader->wake[1] = SNAG_WAKE_INVALID;
    atomic_init(&reader->generation, 0u);
    atomic_init(&reader->stop, false);
    if (secrets && secrets->count) {
        if (secrets->count > SIZE_MAX / sizeof(char *)) { errno = EOVERFLOW; goto failed; }
        reader->secret_values = calloc(secrets->count, sizeof(char *));
        if (!reader->secret_values) goto failed;
        reader->secrets.values = (const char *const *)reader->secret_values;
        for (size_t i = 0u; i < secrets->count; ++i) {
            char *copy = snag_strdup_checked(secrets->values[i], SNAG_WIRE_SECRET_MAX);
            if (!copy) goto failed;
            reader->secret_values[reader->secrets.count++] = copy;
        }
    }
    if (snag_wakeup_create(reader->wake) < 0) goto failed;
    int rc = pthread_mutex_init(&reader->lock, NULL);
    if (rc) { errno = rc; goto failed; }
    rc = pthread_cond_init(&reader->condition, NULL);
    if (rc) {
        (void)pthread_mutex_destroy(&reader->lock);
        errno = rc;
        goto failed;
    }
    rc = pthread_create(&reader->thread, NULL, reader_main, reader);
    if (rc) {
        (void)pthread_cond_destroy(&reader->condition);
        (void)pthread_mutex_destroy(&reader->lock);
        errno = rc;
        goto failed;
    }
    return reader;
failed: {
    int saved = errno;
    free_reader(reader);
    (void)snag_fail(error, size, saved, "cannot start history reader: %s", strerror(saved));
    return NULL;
}
}

void
snag_vm_reader_cancel(struct snag_vm_reader *reader)
{
    (void)pthread_mutex_lock(&reader->lock);
    (void)atomic_fetch_add(&reader->generation, 1u);
    struct snag_vm_read_result *pending = reader->pending, *completed = reader->completed;
    reader->pending = reader->completed = NULL;
    (void)pthread_mutex_unlock(&reader->lock);
    snag_vm_read_result_free(pending);
    snag_vm_read_result_free(completed);
}

uint64_t
snag_vm_reader_request(struct snag_vm_reader *reader, const struct snag_vm_read_request *request)
{
    struct snag_irc_conversation_target target;
    if (!reader || !request ||
        (request->route && (request->kind != SNAG_VM_READ_HISTORY ||
         snag_view_conversation_read(request->route, &target) < 0)) ||
        (request->kind != SNAG_VM_READ_HISTORY && request->kind != SNAG_VM_READ_SESSIONS &&
         request->kind != SNAG_VM_READ_REPORT && request->kind != SNAG_VM_READ_REPORTS) ||
        request->verbosity > SNAG_VERBOSITY_MAX ||
        (request->kind != SNAG_VM_READ_SESSIONS &&
         !snag_hex_is_lower(request->session_id, SNAG_ID_HEX_LEN)) ||
        (request->owned_session_id[0] &&
         !snag_hex_is_lower(request->owned_session_id, SNAG_ID_HEX_LEN))) {
        errno = EINVAL;
        return 0u;
    }
    if ((request->kind == SNAG_VM_READ_REPORT && !snag_vm_report_valid(request->report)) ||
        (request->query && ((!*request->query || strlen(request->query) > SNAG_MAX_DIRECT_PROMPT) ||
         (request->kind != SNAG_VM_READ_HISTORY && request->kind != SNAG_VM_READ_REPORT))) ||
        (request->retained_sessions && !json_is_array(request->retained_sessions)) ||
        (request->kind == SNAG_VM_READ_REPORTS && !json_is_array(request->known_reports))) {
        errno = EINVAL;
        return 0u;
    }
    if (request->navigation.kind && (request->navigation.kind > SNAG_VM_NAV_ROW_DOWN ||
        request->navigation.kind < SNAG_VM_NAV_NONE || !request->navigation.count ||
        request->query || request->selection.kind ||
        (request->kind != SNAG_VM_READ_HISTORY && request->kind != SNAG_VM_READ_REPORT))) {
        errno = EINVAL;
        return 0u;
    }
    if (request->trusted_tail && (request->tail.offset < 0 || !request->tail.next_seq ||
        !snag_hex_is_lower(request->tail.prev_sha256, SNAG_SHA256_HEX_LEN) ||
        ((request->tail.offset == 0) != (request->tail.next_seq == 1u)) ||
        (!request->tail.offset &&
         strspn(request->tail.prev_sha256, "0") != SNAG_SHA256_HEX_LEN))) {
        errno = EINVAL;
        return 0u;
    }
    for (size_t i = 0u; i < json_array_size(request->retained_sessions); ++i) {
        const json_t *id = json_array_get(request->retained_sessions, i);
        if (json_string_length(id) != SNAG_ID_HEX_LEN ||
            !snag_hex_is_lower(json_string_value(id), SNAG_ID_HEX_LEN)) {
            errno = EINVAL;
            return 0u;
        }
    }
    struct snag_vm_read_result *result = calloc(1u, sizeof(*result));
    if (!result) return 0u;
    result->request = *request;
    result->request.retained_sessions = request->retained_sessions ?
        json_deep_copy(request->retained_sessions) : NULL;
    result->request.report = request->report ? json_deep_copy(request->report) : NULL;
    result->request.route = request->route ? json_deep_copy(request->route) : NULL;
    result->request.known_reports = request->known_reports ?
        json_deep_copy(request->known_reports) : NULL;
    result->request.inline_reports = request->inline_reports ?
        json_deep_copy(request->inline_reports) : NULL;
    result->request.query = request->query ? strdup(request->query) : NULL;
    if ((request->retained_sessions && !result->request.retained_sessions) ||
        (request->report && !result->request.report) ||
        (request->route && !result->request.route) ||
        (request->known_reports && !result->request.known_reports) ||
        (request->inline_reports && !result->request.inline_reports) ||
        (request->query && !result->request.query)) {
        snag_vm_read_result_free(result);
        return 0u;
    }
    (void)pthread_mutex_lock(&reader->lock);
    uint64_t generation = atomic_fetch_add(&reader->generation, 1u) + 1u;
    result->generation = generation;
    struct snag_vm_read_result *pending = reader->pending, *completed = reader->completed;
    reader->pending = result;
    reader->completed = NULL;
    (void)pthread_cond_signal(&reader->condition);
    (void)pthread_mutex_unlock(&reader->lock);
    snag_vm_read_result_free(pending);
    snag_vm_read_result_free(completed);
    return generation;
}

snag_wake_fd
snag_vm_reader_fd(const struct snag_vm_reader *reader)
{
    return reader->wake[0];
}

struct snag_vm_read_result *
snag_vm_reader_take(struct snag_vm_reader *reader)
{
    snag_wakeup_drain(reader->wake[0]);
    (void)pthread_mutex_lock(&reader->lock);
    struct snag_vm_read_result *result = reader->completed;
    reader->completed = NULL;
    (void)pthread_mutex_unlock(&reader->lock);
    return result;
}

bool
snag_vm_reader_progress(struct snag_vm_reader *reader, uint64_t generation,
    uint64_t *bytes, uint64_t *events, uint64_t *total)
{
    (void)pthread_mutex_lock(&reader->lock);
    bool current = generation && reader->progress_generation == generation;
    if (current) {
        *bytes = reader->progress_bytes;
        *events = reader->progress_events;
        *total = reader->progress_total;
    }
    (void)pthread_mutex_unlock(&reader->lock);
    return current;
}

void
snag_vm_reader_close(struct snag_vm_reader *reader)
{
    if (!reader) return;
    atomic_store(&reader->stop, true);
    (void)pthread_mutex_lock(&reader->lock);
    (void)pthread_cond_signal(&reader->condition);
    (void)pthread_mutex_unlock(&reader->lock);
    (void)pthread_join(reader->thread, NULL);
    (void)pthread_cond_destroy(&reader->condition);
    (void)pthread_mutex_destroy(&reader->lock);
    free_reader(reader);
}
