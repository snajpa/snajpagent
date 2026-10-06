/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_connection.h"
#include "irc.h"
#include "vm_report.h"
#include "vm_text.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
message(struct snag_vm_connection *connection, const char *text)
{
    (void)snprintf(connection->message, sizeof(connection->message), "%s", text);
    ++connection->revision;
}

static bool
valid_route(const json_t *route)
{
    if (json_is_string(route)) {
        return json_string_length(route) == 7u && !strcmp(json_string_value(route), "rollout");
    }
    struct snag_irc_conversation_target target;
    return snag_view_conversation_read(route, &target) == 0;
}

static bool
valid_activity(const json_t *state, uint64_t next)
{
    if (!json_object_get(state, "irc_activity_after")) return true;
    uint64_t after, total = 0u;
    if (snag_json_integer_u64(state, "irc_activity_after", &after) < 0 || after >= next)
        return false;
    const char *catalogs[] = {"queries", "channels", "connections"};
    for (size_t kind = 0u; kind < 3u; ++kind) {
        const json_t *rows = json_object_get(state, catalogs[kind]);
        if (!json_is_array(rows)) return false;
        for (size_t i = 0u; i < json_array_size(rows); ++i) {
            const json_t *row = json_array_get(rows, i);
            const json_t *item = json_object_get(row, "activity");
            uint64_t seq;
            if (!item || snag_json_integer_u64(row, "seq", &seq) < 0 || !seq || seq >= next)
                return false;
            if (json_is_null(item)) continue;
            if (!snag_irc_activity_item_valid(item, after, seq + 1u)) return false;
            uint64_t count = (uint64_t)json_integer_value(json_object_get(item, "received"));
            if (count > next - 1u - after - total) return false;
            total += count;
        }
    }
    return true;
}

bool
snag_vm_buffer_writable(const struct snag_vm_buffer *buffer)
{
    return buffer && (!json_is_object(buffer->route) ||
        !strcmp(snag_json_string(buffer->route, "identity"), "operator"));
}

bool
snag_vm_buffer_supported(const struct snag_vm_buffer *buffer)
{
    return buffer && (!json_is_object(buffer->route) ||
        (json_object_get(buffer->route, "room") ? buffer->connection->irc_channels :
            json_object_get(buffer->route, "peer") ? buffer->connection->irc_queries :
            buffer->connection->irc_connections));
}

static void
buffer_message(struct snag_vm_buffer *buffer, const char *text)
{
    (void)snprintf(buffer->message, sizeof(buffer->message), "%s", text);
    ++buffer->connection->revision;
}

static bool
same_history(const json_t *a, const json_t *b)
{
    if (!json_is_object(a) || !json_is_object(b)) return json_equal(a, b);
    if (!json_equal(json_object_get(a, "connection"), json_object_get(b, "connection")))
        return false;
    const char *room = snag_json_string(a, "room");
    const char *other = snag_json_string(b, "room");
    if (room || other) {
        enum snag_irc_casemapping mapping = (enum snag_irc_casemapping)
            json_integer_value(json_object_get(a, "casemapping"));
        return room && other && snag_irc_name_equal(mapping, room, other);
    }
    bool query = json_object_get(a, "peer") != NULL;
    if (query != (json_object_get(b, "peer") != NULL)) return false;
    return !query ||
        (json_equal(json_object_get(a, "conversation"), json_object_get(b, "conversation")) &&
         json_equal(json_object_get(a, "identity"), json_object_get(b, "identity")));
}

void
snag_vm_buffer_activity(const struct snag_vm_buffer *buffer, struct snag_vm_activity *activity)
{
    memset(activity, 0, sizeof(*activity));
    if (!buffer || !json_is_object(buffer->route)) return;
    const json_t *state = buffer->connection->state;
    activity->known = snag_json_integer_u64(state, "irc_activity_after", &activity->after) == 0;
    const char *kind = json_object_get(buffer->route, "room") ? "channels" :
        json_object_get(buffer->route, "peer") ? "queries" : "connections";
    const json_t *rows = json_object_get(state, kind);
    for (size_t i = 0u; i < json_array_size(rows); ++i) {
        const json_t *row = json_array_get(rows, i);
        if (!same_history(buffer->route, json_object_get(row, "route"))) continue;
        const json_t *item = json_object_get(row, "activity");
        uint64_t seq = (uint64_t)json_integer_value(json_object_get(item, "seq"));
        if (seq > activity->seq) {
            activity->seq = seq;
            activity->time = (uint64_t)json_integer_value(json_object_get(item, "time"));
        }
        activity->received += (uint64_t)json_integer_value(json_object_get(item, "received"));
        seq = (uint64_t)json_integer_value(json_object_get(item, "incoming"));
        if (seq > activity->incoming) activity->incoming = seq;
    }
    if (!activity->known) return;
    if (activity->incoming <= buffer->read_seq && activity->after <= buffer->read_seq) {
        activity->exact = true;
    } else if (activity->after == buffer->read_after &&
        activity->received >= buffer->read_received) {
        activity->exact = true;
        activity->unread = activity->received - buffer->read_received;
    }
}

bool
snag_vm_buffer_read(struct snag_vm_buffer *buffer, uint64_t through)
{
    struct snag_vm_activity activity;
    snag_vm_buffer_activity(buffer, &activity);
    if (!activity.known || through < activity.seq || through < activity.after) return false;
    bool changed = false;
    for (struct snag_vm_buffer *b = buffer->connection->buffers; b; b = b->next) {
        if (!same_history(buffer->route, b->route) || !same_history(b->route, buffer->route) ||
            b->read_seq >= through) continue;
        b->read_seq = through;
        b->read_received = activity.received;
        b->read_after = activity.after;
        changed = true;
    }
    return changed;
}

const json_t *
snag_vm_buffer_state(const struct snag_vm_buffer *buffer)
{
    if (!buffer || !json_is_object(buffer->route)) return NULL;
    const char *kind = json_object_get(buffer->route, "room") ? "channels" :
        json_object_get(buffer->route, "peer") ? "queries" : "connections";
    const json_t *rows = json_object_get(buffer->connection->state, kind);
    for (size_t i = 0u; i < json_array_size(rows); ++i) {
        const json_t *row = json_array_get(rows, i);
        const json_t *route = json_object_get(row, "route");
        if (json_equal(json_object_get(buffer->route, "conversation"),
            json_object_get(route, "conversation"))) return row;
    }
    return NULL;
}

struct snag_vm_buffer *
snag_vm_buffer_get(struct snag_vm_connection *connection, const json_t *route, bool create)
{
    if (!connection) return NULL;
    if (!route && connection->rollout) return connection->rollout;
    if (route && !valid_route(route)) { errno = EINVAL; return NULL; }
    for (struct snag_vm_buffer *b = connection->buffers; route && b; b = b->next)
        if (json_equal(b->route, route)) return b;
    if (!create) return NULL;
    struct snag_vm_buffer *buffer = calloc(1u, sizeof(*buffer));
    if (!buffer) return NULL;
    buffer->route = route ? json_deep_copy(route) : json_string("rollout");
    if (!buffer->route) { free(buffer); return NULL; }
    buffer->connection = connection;
    for (struct snag_vm_buffer *b = connection->buffers; b; b = b->next) {
        if (same_history(buffer->route, b->route) && same_history(b->route, buffer->route) &&
            b->read_seq > buffer->read_seq) {
            buffer->read_seq = b->read_seq;
            buffer->read_received = b->read_received;
            buffer->read_after = b->read_after;
        }
    }
    snag_buf_init(&buffer->draft, SNAG_MAX_DIRECT_PROMPT + 1u);
    buffer->draft_get = connection->drafts;
    buffer->next = connection->buffers;
    connection->buffers = buffer;
    if (json_is_string(buffer->route)) connection->rollout = buffer;
    return buffer;
}

bool
snag_vm_connection_unsaved(const struct snag_vm_connection *connection)
{
    for (const struct snag_vm_buffer *b = connection->buffers; b; b = b->next)
        if (b->draft.len || b->pending || b->draft_conflict) return true;
    return false;
}

void
snag_vm_connection_discard(struct snag_vm_connection *connection)
{
    for (struct snag_vm_buffer *b = connection->buffers; b; b = b->next) {
        snag_vm_editor_reset(&b->editor);
        snag_buf_reset(&b->draft);
        b->cursor = 0u;
        json_decref(b->pending);
        b->pending = NULL;
        json_decref(b->origin);
        b->origin = NULL;
        json_decref(b->conflict_draft);
        b->conflict_draft = NULL;
        b->draft_conflict = b->send_pending = b->reconcile_pending = b->draft_dirty = false;
    }
    connection->inflight = NULL;
}

struct snag_vm_connection *
snag_vm_connection_new(const char *session)
{
    struct snag_vm_connection *connection = calloc(1u, sizeof(*connection));
    if (!connection) return NULL;
    connection->reports = json_array();
    if (!connection->reports) { free(connection); return NULL; }
    snag_view_channel_init(&connection->channel, -1);
    if (!snag_vm_buffer_get(connection, NULL, true)) {
        snag_vm_connections_free(connection);
        return NULL;
    }
    (void)snprintf(connection->session, sizeof(connection->session), "%s", session);
    return connection;
}

void
snag_vm_connection_close(struct snag_vm_connection *connection)
{
    snag_view_channel_close(&connection->channel);
    connection->bound = connection->hello = false;
    connection->generation = connection->deadline = connection->draft_deadline = 0u;
    connection->commands = connection->terminal_commands = connection->irc_queries = false;
    connection->irc_channels = connection->irc_connections = false;
    connection->reports_supported = connection->reports_subscribed = false;
    connection->drafts = connection->detaching = connection->detach_sent = false;
    connection->draft_wait = connection->inflight = NULL;
    json_decref(connection->draft_sent);
    connection->draft_sent = NULL;
    json_decref(connection->state);
    connection->state = NULL;
    for (struct snag_vm_buffer *b = connection->buffers; b; b = b->next) {
        b->submitting = b->send_pending = b->draft_ready = b->draft_get = false;
        b->terminal_result = b->terminal_auto = false;
        b->receipt_at = 0u;
        json_decref(b->report_open);
        b->report_open = NULL;
        json_decref(b->selection);
        b->selection = NULL;
    }
}

void
snag_vm_connections_free(struct snag_vm_connection *connection)
{
    while (connection) {
        struct snag_vm_connection *next = connection->next;
        snag_vm_connection_close(connection);
        while (connection->buffers) {
            struct snag_vm_buffer *b = connection->buffers;
            connection->buffers = b->next;
            snag_vm_editor_reset(&b->editor);
            if (b->draft.data) memset(b->draft.data, 0, b->draft.len);
            snag_buf_free(&b->draft);
            json_decref(b->route);
            json_decref(b->pending);
            json_decref(b->origin);
            json_decref(b->draft_base);
            json_decref(b->owner_draft);
            json_decref(b->conflict_draft);
            free(b);
        }
        json_decref(connection->reports);
        free(connection);
        connection = next;
    }
}
static int
send_message(struct snag_vm_connection *connection, json_t *value)
{
    int rc = value ? snag_view_channel_send(&connection->channel, value) : -1;
    json_decref(value);
    return rc;
}

int
snag_vm_connection_open(struct snag_vm_connection *connection, struct snag_store *store,
    bool control)
{
    snag_vm_connection_close(connection);
    connection->control = control;
    connection->exited = connection->quitting = false;
    struct snag_session location;
    snag_session_init(&location);
    char error[256];
    int fd = -1;
    if (snag_session_locate(store, &location, connection->session, NULL, NULL,
        error, sizeof(error)) == 0) {
        fd = snag_session_view_connect(location.dir_fd, location.dir_path);
        if (fd < 0) (void)snprintf(error, sizeof(error),
            "Owner attachment unavailable: %s; use :classic for its terminal or read history",
            strerror(errno));
    }
    snag_session_close(&location);
    if (fd < 0) {
        message(connection, error);
        return -1;
    }
    snag_view_channel_init(&connection->channel, fd);
    connection->deadline = snag_monotonic_ms() + 15000u;
    for (struct snag_vm_buffer *b = connection->buffers; b; b = b->next) {
        b->query = b->pending != NULL;
        b->reconcile_pending = b->query;
    }
    message(connection, control ? "Attaching to owner" : "Observing owner");
    return send_message(connection, json_pack("{s:s,s:i}", "type", "hello", "version", 1));
}

int
snag_vm_draft_replace(struct snag_vm_buffer *buffer, size_t begin, size_t end,
    const void *text, size_t length)
{
    struct snag_vm_connection *connection = buffer->connection;
    struct snag_buf *draft = &buffer->draft;
    if (begin > end || end > draft->len || length > SNAG_MAX_DIRECT_PROMPT ||
        draft->len - (end - begin) > SNAG_MAX_DIRECT_PROMPT - length ||
        !snag_utf8_valid(text, length, true)) return snag_errno(EINVAL);
    size_t total = draft->len - (end - begin) + length;
    if (snag_buf_reserve(draft, total >= draft->len ? total - draft->len + 1u : 1u) < 0)
        return -1;
    memmove(draft->data + begin + length, draft->data + end, draft->len - end);
    if (length) memcpy(draft->data + begin, text, length);
    draft->len = total;
    draft->data[total] = 0;
    buffer->cursor = begin + length;
    size_t boundary = snag_vm_text_floor((const char *)draft->data, total, buffer->cursor);
    if (boundary != buffer->cursor)
        buffer->cursor = snag_vm_text_next((const char *)draft->data, total, boundary);
    ++connection->revision;
    buffer->draft_dirty = true;
    buffer->editor.completing = false;
    return 0;
}

void
snag_vm_draft_cursor(struct snag_vm_buffer *buffer, size_t cursor)
{
    struct snag_vm_connection *connection = buffer->connection;
    if (buffer->cursor == cursor) return;
    buffer->cursor = cursor;
    buffer->editor.completing = false;
    buffer->draft_dirty = true;
    ++connection->revision;
}

static const char *
draft_text(const struct snag_vm_buffer *buffer)
{
    if (buffer->send_pending) return snag_json_string(buffer->pending, "text");
    return buffer->draft.len ? (const char *)buffer->draft.data : "";
}

static bool
valid_draft(const json_t *draft)
{
    const char *text = snag_json_string(draft, "text");
    const json_t *route = json_object_get(draft, "route");
    uint64_t revision, cursor;
    return snag_json_exact_keys(draft, "route revision text cursor") &&
        valid_route(route) && text &&
        strlen(text) == json_string_length(json_object_get(draft, "text")) &&
        strlen(text) <= SNAG_MAX_DIRECT_PROMPT &&
        snag_json_integer_u64(draft, "revision", &revision) == 0 && revision &&
        snag_json_integer_u64(draft, "cursor", &cursor) == 0 && cursor <= strlen(text) &&
        snag_vm_text_floor(text, strlen(text), (size_t)cursor) == cursor;
}

static int
draft_baseline(struct snag_vm_buffer *buffer, const json_t *draft)
{
    struct snag_vm_connection *connection = buffer->connection;
    char digest[SNAG_SHA256_HEX_LEN + 1u];
    const char *text = snag_json_string(draft, "text");
    snag_sha256_hex(text, strlen(text), digest);
    json_t *base = json_pack("{s:s,s:O,s:s}", "instance", connection->instance,
        "revision", json_object_get(draft, "revision"), "sha256", digest);
    if (!base) return -1;
    json_decref(buffer->draft_base);
    buffer->draft_base = base;
    ++connection->revision;
    return 0;
}

/* Completed commands and terminal requirements both acknowledge consumption
 * of the submitted owner draft. Recovery must compare against that new base. */
static int
receipt_draft(struct snag_vm_buffer *buffer, const json_t *value)
{
    struct snag_vm_connection *connection = buffer->connection;
    uint64_t cleared = 0u;
    if (connection->drafts &&
        snag_json_integer_u64(value, "draft_cleared", &cleared) < 0)
        return snag_errno(EPROTO);
    if (cleared && (!buffer->draft_base || cleared > (uint64_t)json_integer_value(
        json_object_get(buffer->draft_base, "revision")))) {
        json_t *empty = json_pack("{s:I,s:s}", "revision", (json_int_t)cleared,
            "text", "");
        int rc = empty ? draft_baseline(buffer, empty) : -1;
        json_decref(empty);
        if (rc < 0) return -1;
    }
    return 0;
}

static bool
at_baseline(const struct snag_vm_buffer *buffer, const char *text)
{
    struct snag_vm_connection *connection = buffer->connection;
    if (!buffer->draft_base || strcmp(connection->instance,
        snag_json_string(buffer->draft_base, "instance"))) return false;
    char digest[SNAG_SHA256_HEX_LEN + 1u];
    snag_sha256_hex(text, strlen(text), digest);
    return !strcmp(digest, snag_json_string(buffer->draft_base, "sha256"));
}

int
snag_vm_draft_choose(struct snag_vm_buffer *buffer, bool local)
{
    struct snag_vm_connection *connection = buffer->connection;
    if (!buffer->draft_conflict || buffer->pending ||
        (connection->bound && connection->drafts && !buffer->draft_ready))
        return snag_errno(EBUSY);
    if (!local) {
        const char *text = snag_json_string(buffer->conflict_draft, "text");
        if (snag_vm_draft_replace(buffer, 0u, buffer->draft.len, text, strlen(text)) < 0)
            return -1;
        buffer->cursor = (size_t)json_integer_value(
            json_object_get(buffer->conflict_draft, "cursor"));
        snag_vm_editor_reset(&buffer->editor);
    }
    if (buffer->draft_ready) {
        if (draft_baseline(buffer, buffer->owner_draft) < 0) return -1;
    } else {
        json_decref(buffer->draft_base);
        buffer->draft_base = NULL;
    }
    buffer->draft_conflict = false;
    json_decref(buffer->conflict_draft);
    buffer->conflict_draft = NULL;
    buffer->draft_dirty = true;
    buffer_message(buffer, local ? "Using workspace draft" : "Using owner draft");
    return 0;
}

void
snag_vm_connection_detach(struct snag_vm_connection *connection)
{
    if (!connection->bound || connection->quitting) {
        snag_vm_connection_close(connection);
        return;
    }
    connection->detaching = true;
    message(connection, "Saving owner draft before detach");
}

int
snag_vm_buffer_prepare(struct snag_vm_buffer *buffer, struct snag_vm_buffer *source,
    uint64_t window)
{
    struct snag_vm_connection *connection = buffer->connection;
    if (!snag_vm_buffer_writable(buffer) || !snag_vm_buffer_writable(source))
        return snag_errno(EACCES);
    if (!snag_vm_buffer_supported(buffer)) return snag_errno(ENOTSUP);
    if (!connection->bound || buffer->pending || source->pending || !source->draft.len ||
        connection->quitting || connection->detaching || source->draft_conflict)
        return snag_errno(EBUSY);
    const char *text = (const char *)source->draft.data;
    bool forwarded = source != buffer;
    if (forwarded && !snag_prompt_command(text)) return snag_errno(EINVAL);
    if (snag_prompt_command(text) && !connection->commands)
        return snag_errno(ENOTSUP);
    char id[SNAG_ID_HEX_LEN + 1u];
    if (snag_random_id(id) < 0) return -1;
    json_t *pending = json_pack("{s:s,s:s,s:s}", "id", id, "instance", connection->instance,
        "text", text);
    if (!pending) return -1;
    json_t *origin = forwarded ? json_pack("{s:s,s:O}", "session", source->connection->session,
        "route", source->route) : NULL;
    if (forwarded && (!origin || json_object_set(pending, "origin", origin) < 0)) {
        json_decref(origin);
        json_decref(pending);
        return -1;
    }
    if (snag_vm_draft_replace(source, 0u, source->draft.len, NULL, 0u) < 0) {
        json_decref(origin);
        json_decref(pending);
        return -1;
    }
    buffer->pending = pending;
    json_decref(buffer->origin);
    buffer->origin = origin;
    buffer->request_window = window;
    buffer->terminal_auto = connection->terminal_commands;
    buffer->reconcile_pending = true;
    snag_vm_editor_reset(&source->editor);
    /* The pending copy is persisted before transmission; new typing belongs
     * to the next draft and can never be erased by this request's receipt. */
    buffer_message(buffer, "Submission saved; awaiting durable receipt");
    return 0;
}

int
snag_vm_buffer_send(struct snag_vm_buffer *buffer)
{
    struct snag_vm_connection *connection = buffer->connection;
    if (!connection->bound || !buffer->pending || connection->detaching)
        return snag_errno(EBUSY);
    buffer->send_pending = true;
    if (!buffer->origin) buffer->draft_dirty = true;
    buffer->reconcile_pending = false;
    return 0;
}

int
snag_vm_buffer_recover(struct snag_vm_buffer *buffer, struct snag_vm_buffer *target)
{
    struct snag_vm_connection *connection = buffer->connection;
    if (!buffer->pending || target->draft.len || buffer->submitting)
        return snag_errno(EBUSY);
    const char *text = snag_json_string(buffer->pending, "text");
    if (snag_vm_draft_replace(target, 0u, 0u, text, strlen(text)) < 0) return -1;
    snag_vm_editor_reset(&target->editor);
    json_decref(buffer->pending);
    buffer->pending = NULL;
    json_decref(buffer->origin);
    buffer->origin = NULL;
    buffer->terminal_result = buffer->terminal_auto = false;
    buffer->query = false;
    buffer->send_pending = buffer->reconcile_pending = false;
    buffer->draft_get = connection->drafts;
    buffer->receipt_at = 0u;
    buffer_message(target, "Submission recovered into draft; verify history before sending again");
    ++connection->revision;
    return 0;
}

int
snag_vm_connection_control(struct snag_vm_connection *connection, const char *intent)
{
    if (!connection->bound || connection->channel.output) return snag_errno(EBUSY);
    int rc = send_message(connection, json_pack("{s:s,s:I}", "type", intent,
        "generation", (json_int_t)connection->generation));
    if (!rc && !strcmp(intent, "quit")) {
        connection->quitting = true;
        message(connection, "Waiting for session shutdown");
    }
    return rc;
}

/* Reconcile only on edits or owner replies. Idle polling never hashes a draft.
 * One outstanding revision check coalesces typing while preserving newer text. */
static int
draft_sync(struct snag_vm_buffer *buffer)
{
    struct snag_vm_connection *connection = buffer->connection;
    bool forwarded = buffer->pending && buffer->origin;
    if (!snag_vm_buffer_writable(buffer) || !snag_vm_buffer_supported(buffer)) return 0;
    if (!connection->bound || connection->channel.output || connection->draft_sent ||
        connection->quitting || connection->detach_sent || connection->draft_deadline) return 0;
    if (!forwarded && connection->drafts && buffer->draft_get && !buffer->reconcile_pending) {
        connection->draft_wait = buffer;
        buffer->draft_get = false;
        buffer->draft_ready = false;
        connection->draft_deadline = snag_monotonic_ms() + 5000u;
        return send_message(connection, json_pack("{s:s,s:I,s:O}", "type", "draft_get",
            "generation", (json_int_t)connection->generation, "route", buffer->route));
    }
    if (!forwarded && connection->drafts && !buffer->reconcile_pending && buffer->draft_ready &&
        buffer->draft_dirty && !buffer->draft_conflict) {
        const char *text = draft_text(buffer);
        const char *owner = snag_json_string(buffer->owner_draft, "text");
        size_t cursor = buffer->send_pending ? strlen(text) : buffer->cursor;
        bool same = !strcmp(text, owner);
        bool owner_unchanged = at_baseline(buffer, owner);
        bool local_unchanged = at_baseline(buffer, text);
        bool base = buffer->draft_base && !strcmp(connection->instance,
            snag_json_string(buffer->draft_base, "instance"));
        if (same || owner_unchanged || (!base && !*owner)) {
            buffer->draft_conflict = false;
            if (same && draft_baseline(buffer, buffer->owner_draft) < 0) return -1;
            if (same && cursor == (size_t)json_integer_value(
                json_object_get(buffer->owner_draft, "cursor"))) {
                buffer->draft_dirty = false;
            } else {
                if (connection->draft_edit == INT64_MAX) return snag_errno(EOVERFLOW);
                json_t *edit = json_pack("{s:s,s:I,s:O,s:O,s:I,s:s,s:I}",
                    "type", "draft", "generation", (json_int_t)connection->generation,
                    "route", buffer->route, "revision",
                    json_object_get(buffer->owner_draft, "revision"),
                    "edit", (json_int_t)++connection->draft_edit, "text", text,
                    "cursor", (json_int_t)cursor);
                if (!edit) return -1;
                connection->draft_wait = buffer;
                connection->draft_sent = json_incref(edit);
                connection->draft_deadline = snag_monotonic_ms() + 5000u;
                return send_message(connection, edit);
            }
        } else if (!buffer->send_pending && (local_unchanged || (!base && !*text))) {
            if (snag_vm_draft_replace(buffer, 0u, buffer->draft.len,
                owner, strlen(owner)) < 0 ||
                draft_baseline(buffer, buffer->owner_draft) < 0)
                return -1;
            buffer->cursor = (size_t)json_integer_value(
                json_object_get(buffer->owner_draft, "cursor"));
            snag_vm_editor_reset(&buffer->editor);
            buffer->draft_dirty = buffer->draft_conflict = false;
            buffer_message(buffer, "Owner draft restored");
        } else {
            buffer->draft_dirty = false;
            buffer->draft_conflict = true;
            json_decref(buffer->conflict_draft);
            buffer->conflict_draft = json_incref(buffer->owner_draft);
            buffer->send_pending = false;
            buffer_message(buffer, buffer->pending ?
                "Draft conflict; :recover the unsent prompt, then :draft local or :draft owner" :
                "Draft conflict; both copies retained; :draft local or :draft owner");
        }
    }
    if (!connection->inflight && buffer->send_pending && (forwarded || !connection->drafts ||
        (buffer->draft_ready && !buffer->draft_dirty && !buffer->draft_conflict))) {
        bool command = snag_prompt_command(snag_json_string(buffer->pending, "text"));
        json_t *request = json_pack("{s:s,s:I,s:s,s:s}", "type", command ? "command" : "submit",
            "generation", (json_int_t)connection->generation,
            "id", snag_json_string(buffer->pending, "id"),
            "text", snag_json_string(buffer->pending, "text"));
        if (!request) return -1;
        if ((command || json_is_object(buffer->route)) &&
            json_object_set(request, "route", buffer->route) < 0) {
            json_decref(request);
            return -1;
        }
        if (!forwarded && connection->drafts &&
            (json_object_set(request, "route", buffer->route) < 0 ||
             json_object_set(request, "draft_revision",
                json_object_get(buffer->owner_draft, "revision")) < 0)) {
            json_decref(request);
            return -1;
        }
        if (send_message(connection, request) < 0) return -1;
        buffer->send_pending = false;
        buffer->submitting = true;
        connection->inflight = buffer;
        if (!forwarded) buffer->draft_dirty = buffer->draft.len != 0u;
        return 0;
    }
    return 0;
}

static int
state_tail(const json_t *state, struct snag_journal_cursor *tail)
{
    uint64_t seq, end, schema;
    const char *hash = snag_json_bounded_string(json_object_get(state, "sha256"),
        SNAG_SHA256_HEX_LEN);
    if (snag_json_integer_u64(state, "seq", &seq) < 0 ||
        snag_json_integer_u64(state, "end", &end) < 0 || end > INT64_MAX ||
        snag_json_integer_u64(state, "schema", &schema) < 0 || schema < 2u || schema > 4u ||
        !hash || !snag_hex_is_lower(hash, SNAG_SHA256_HEX_LEN) ||
        ((end == 0u) != (seq == 0u)) || (!end && strspn(hash, "0") != SNAG_SHA256_HEX_LEN))
        return snag_errno(EPROTO);
    *tail = (struct snag_journal_cursor){.offset = (int64_t)end, .next_seq = seq + 1u};
    memcpy(tail->prev_sha256, hash, sizeof(tail->prev_sha256));
    return 0;
}

bool
snag_vm_connection_tail(const struct snag_vm_connection *connection, struct snag_journal_cursor *tail)
{
    return connection && connection->state && state_tail(connection->state, tail) == 0;
}

static int
retain_report(struct snag_vm_connection *connection, const json_t *report, bool replay)
{
    if (!snag_vm_report_valid(report)) return snag_errno(EPROTO);
    for (size_t i = 0u; i < json_array_size(connection->reports); ++i) {
        const json_t *existing = json_array_get(connection->reports, i);
        if (strcmp(snag_json_string(existing, "id"), snag_json_string(report, "id"))) continue;
        if (!json_equal(existing, report)) return snag_errno(EPROTO);
        /* Replayed owner reports arrive in creation order. Move their existing
         * references to the tail without losing older-owner/legacy reports. */
        if (replay && i + 1u < json_array_size(connection->reports)) {
            if (json_array_append(connection->reports, (json_t *)report) < 0) return -1;
            (void)json_array_remove(connection->reports, i);
            ++connection->revision;
        }
        return 0;
    }
    if (json_array_append(connection->reports, (json_t *)report) < 0) return -1;
    ++connection->revision;
    return 1;
}

int
snag_vm_reports_merge(struct snag_vm_connection *connection, const json_t *catalog,
    const json_t *known)
{
    json_t *ids = json_object();
    json_t *disk_ids = json_object();
    json_t *known_ids = json_object();
    json_t *merged = json_array();
    int rc = -1;
    if (!ids || !disk_ids || !known_ids || !merged ||
        !json_is_array(catalog) || !json_is_array(known)) goto out;
    const json_t *sources[] = {connection->reports, catalog, known};
    for (size_t source = 0u; source < 3u; ++source) {
        for (size_t i = 0u; i < json_array_size(sources[source]); ++i) {
            json_t *report = json_array_get(sources[source], i);
            if (!snag_vm_report_valid(report)) { errno = EILSEQ; goto out; }
            const char *id = snag_json_string(report, "id");
            const json_t *previous = json_object_get(ids, id);
            if (previous && !json_equal(previous, report)) { errno = ESTALE; goto out; }
            if (json_object_set(ids, id, report) < 0 ||
                (source == 1u && json_object_set(disk_ids, id, report) < 0) ||
                (source == 2u && json_object_set(known_ids, id, report) < 0)) goto out;
        }
    }
    for (unsigned int phase = 0u; phase < 3u; ++phase) {
        const json_t *rows = phase == 1u ? catalog : connection->reports;
        for (size_t i = 0u; i < json_array_size(rows); ++i) {
            json_t *report = json_array_get(rows, i);
            const char *id = snag_json_string(report, "id");
            if (phase != 1u && (json_object_get(disk_ids, id) ||
                (json_object_get(known_ids, id) != NULL) != (phase == 0u))) continue;
            if (json_array_append(merged, report) < 0) goto out;
        }
    }
    if (!json_equal(connection->reports, merged)) {
        json_decref(connection->reports);
        connection->reports = json_incref(merged);
        ++connection->revision;
    }
    rc = 0;
out:
    json_decref(ids);
    json_decref(disk_ids);
    json_decref(known_ids);
    json_decref(merged);
    return rc;
}

static int
receive(struct snag_vm_connection *connection, const json_t *value)
{
    const char *type = snag_json_string(value, "type");
    if (!type) return snag_errno(EPROTO);
    if (!strcmp(type, "capabilities")) {
        const char *session = snag_json_bounded_string(json_object_get(value, "session"),
            SNAG_ID_HEX_LEN);
        const char *instance = snag_json_bounded_string(json_object_get(value, "instance"),
            SNAG_ID_HEX_LEN);
        json_t *features = json_object_get(value, "features");
        static const char *const required[] = {"observe", "control", "submit", "receipts",
            "cancel", "quit", "detach"};
        if (connection->hello || !session || strcmp(session, connection->session) ||
            !instance || strlen(instance) != SNAG_ID_HEX_LEN ||
            !snag_hex_is_lower(instance, SNAG_ID_HEX_LEN) ||
            json_integer_value(json_object_get(value, "version")) != 1) return snag_errno(EPROTO);
        for (size_t i = 0u; i < sizeof(required) / sizeof(required[0]); ++i) {
            bool found = false;
            for (size_t j = 0u; j < json_array_size(features); ++j) {
                const char *feature = json_string_value(json_array_get(features, j));
                if (feature && !strcmp(feature, required[i])) found = true;
            }
            if (!found) return snag_errno(ENOTSUP);
        }
        for (size_t j = 0u; j < json_array_size(features); ++j) {
            const char *feature = json_string_value(json_array_get(features, j));
            if (feature && !strcmp(feature, "drafts")) connection->drafts = true;
            if (feature && !strcmp(feature, "irc_queries")) connection->irc_queries = true;
            if (feature && !strcmp(feature, "irc_channels")) connection->irc_channels = true;
            if (feature && !strcmp(feature, "irc_connections")) connection->irc_connections = true;
            if (feature && !strcmp(feature, "commands")) connection->commands = true;
            if (feature && !strcmp(feature, "terminal_commands"))
                connection->terminal_commands = true;
            if (feature && !strcmp(feature, "reports")) connection->reports_supported = true;
        }
        memcpy(connection->instance, instance, sizeof(connection->instance));
        connection->hello = true;
        if (connection->control)
            return send_message(connection, json_pack("{s:s}", "type", "reserve"));
        connection->deadline = 0u;
        message(connection, "Read-only owner observation");
    } else if (!connection->hello) return snag_errno(EPROTO);
    else if (!strcmp(type, "reserved")) {
        if (!connection->control || connection->generation ||
            snag_json_integer_u64(value, "generation", &connection->generation) < 0 ||
            !connection->generation) return snag_errno(EPROTO);
        return send_message(connection, json_pack("{s:s,s:I}", "type", "commit",
            "generation", (json_int_t)connection->generation));
    } else if (!strcmp(type, "bound")) {
        uint64_t generation;
        if (snag_json_integer_u64(value, "generation", &generation) < 0 ||
            !generation || generation != connection->generation) return snag_errno(EPROTO);
        connection->bound = true;
        connection->deadline = 0u;
        for (struct snag_vm_buffer *b = connection->buffers; b; b = b->next)
            b->draft_get = connection->drafts;
        message(connection, "Attached  i: edit prompt  :detach: keep owner running");
    } else if (!strcmp(type, "draft")) {
        json_t *draft = json_object_get(value, "draft");
        struct snag_vm_buffer *buffer = snag_vm_buffer_get(connection,
            json_object_get(draft, "route"), false);
        if (!buffer) return snag_errno(EPROTO);
        const char *status = snag_json_string(value, "status");
        uint64_t edit;
        if (!connection->drafts || !connection->bound || !valid_draft(draft) ||
            !snag_json_exact_keys(value, "type edit status draft") || !status ||
            snag_json_integer_u64(value, "edit", &edit) < 0 ||
            (edit ? (!connection->draft_sent || connection->draft_wait != buffer ||
                     edit != connection->draft_edit ||
                     (strcmp(status, "accepted") && strcmp(status, "conflict"))) :
                    strcmp(status, "snapshot"))) return snag_errno(EPROTO);
        if (edit) {
            if (!strcmp(status, "accepted") && draft_baseline(buffer, draft) < 0) return -1;
            json_decref(connection->draft_sent);
            connection->draft_sent = NULL;
            connection->draft_deadline = 0u;
            connection->draft_wait = NULL;
        } else if (!connection->draft_sent && connection->draft_wait == buffer) {
            connection->draft_deadline = 0u;
            connection->draft_wait = NULL;
        }
        json_decref(buffer->owner_draft);
        buffer->owner_draft = json_incref(draft);
        buffer->draft_ready = buffer->draft_dirty = true;
        if (buffer->draft_conflict)
            buffer_message(buffer, "Draft conflict retained; :draft local or :draft owner");
        ++connection->revision;
    } else if (!strcmp(type, "state")) {
        json_t *state = json_object_get(value, "state");
        struct snag_journal_cursor next, previous;
        if (state_tail(state, &next) < 0 || !valid_activity(state, next.next_seq) ||
            !json_is_boolean(json_object_get(state, "active")) ||
            !json_is_string(json_object_get(state, "provider")) ||
            !json_is_string(json_object_get(state, "model")) ||
            !json_is_string(json_object_get(state, "effort"))) return snag_errno(EPROTO);
        if (snag_vm_connection_tail(connection, &previous) &&
            (next.offset < previous.offset || next.next_seq < previous.next_seq ||
             ((next.offset == previous.offset) != (next.next_seq == previous.next_seq)) ||
             (next.offset == previous.offset && strcmp(next.prev_sha256, previous.prev_sha256)) ||
             json_integer_value(json_object_get(state, "schema")) <
             json_integer_value(json_object_get(connection->state, "schema"))))
            return snag_errno(EPROTO);
        const char *catalogs[] = {"queries", "channels", "connections"};
        for (size_t kind = 0u; kind < 3u; ++kind) {
            const json_t *rows = json_object_get(state, catalogs[kind]);
            if (rows && !json_is_array(rows)) return snag_errno(EPROTO);
            bool changed = !json_equal(rows, json_object_get(connection->state, catalogs[kind]));
            for (size_t i = 0u; changed && i < json_array_size(rows); ++i) {
                const json_t *row = json_array_get(rows, i);
                const char *endpoint = snag_json_bounded_string(json_object_get(row, "endpoint"),
                    SNAG_CONFIG_IRC_ENDPOINT_MAX);
                const json_t *route = json_object_get(row, "route");
                if (!endpoint || !json_is_object(route) ||
                    (json_object_get(route, "room") != NULL) != (kind == 1u) ||
                    (json_object_get(route, "peer") != NULL) != (kind == 0u))
                    return snag_errno(EPROTO);
                struct snag_vm_buffer *b = snag_vm_buffer_get(connection, route, true);
                if (!b) return -1;
                (void)snag_strcpy(b->endpoint, sizeof(b->endpoint), endpoint);
            }
        }
        json_decref(connection->state);
        connection->state = json_incref(state);
        ++connection->revision;
    } else if (!strcmp(type, "report")) {
        const json_t *report = json_object_get(value, "report");
        const json_t *error_value = json_object_get(value, "error");
        const char *error = json_string_value(error_value);
        if (!connection->reports_subscribed || !error || strlen(error) > 256u ||
            strlen(error) != json_string_length(error_value) ||
            !snag_json_exact_keys(value, "type report error")) return snag_errno(EPROTO);
        if (json_is_null(report)) {
            if (!*error) return snag_errno(EPROTO);
            message(connection, error);
        } else {
            int retained = retain_report(connection, report, true);
            if (retained < 0) return -1;
            if (retained) message(connection, "Command output retained; :reports opens it");
        }
    } else if (!strcmp(type, "reports_ready")) {
        if (!connection->reports_subscribed || !snag_json_exact_keys(value, "type"))
            return snag_errno(EPROTO);
    } else if (!strcmp(type, "result")) {
        const char *id = snag_json_string(value, "id");
        const char *status = snag_json_string(value, "status");
        if (!id || !status) return snag_errno(EPROTO);
        struct snag_vm_buffer *buffer;
        for (buffer = connection->buffers; buffer; buffer = buffer->next)
            if (buffer->pending && !strcmp(id, snag_json_string(buffer->pending, "id"))) break;
        if (!buffer) return 0;
        if (strcmp(status, "pending") && connection->inflight == buffer)
            connection->inflight = NULL;
        if (!strcmp(status, "pending")) {
            buffer->submitting = true;
            connection->inflight = buffer;
            /* A queried pending receipt is not subscribed to the original
             * submitter's completion. Recheck until it reaches a final state. */
            buffer->query = true;
            buffer->receipt_at = snag_monotonic_ms() + 250u;
        } else if (!strcmp(status, "terminal")) {
            if (!snag_prompt_command(snag_json_string(buffer->pending, "text")))
                return snag_errno(EPROTO);
            if (strcmp(connection->instance, snag_json_string(buffer->pending, "instance")))
                return snag_errno(EPROTO);
            if (receipt_draft(buffer, value) < 0) return -1;
            buffer->submitting = buffer->query = false;
            buffer->receipt_at = 0u;
            buffer->reconcile_pending = buffer->terminal_result = true;
            buffer_message(buffer,
                "Command needs :classic; not executed; :recover restores its text");
        } else if (!strcmp(status, "committed") || !strcmp(status, "completed")) {
            uint64_t seq;
            if (snag_json_integer_u64(value, "seq", &seq) < 0 || !seq ||
                strcmp(connection->instance, snag_json_string(buffer->pending, "instance")))
                return snag_errno(EPROTO);
            bool command = !strcmp(status, "completed");
            if (command) {
                json_t *report = json_object_get(value, "report");
                const char *outcome = snag_json_string(value, "outcome");
                if (!snag_prompt_command(snag_json_string(buffer->pending, "text")) ||
                    !outcome || (strcmp(outcome, "ok") && strcmp(outcome, "error")) ||
                    (!json_is_null(report) && (!snag_vm_report_valid(report) ||
                     strcmp(snag_json_string(report, "command"),
                        snag_json_string(buffer->pending, "text"))))) return snag_errno(EPROTO);
            }
            if (receipt_draft(buffer, value) < 0) return -1;
            if (command && !json_is_null(json_object_get(value, "report"))) {
                json_t *report = json_object_get(value, "report");
                if (retain_report(connection, report, false) < 0) return -1;
                json_decref(buffer->report_open);
                const char *text = snag_json_string(buffer->pending, "text");
                size_t verb = strcspn(text, " \t\r\n");
                bool private_send = (verb == 4u && !strncmp(text, "/msg", verb)) ||
                    (verb == 7u && !strncmp(text, "/notice", verb)) ||
                    (verb == 3u && !strncmp(text, "/me", verb));
                bool ok = !strcmp(snag_json_string(value, "outcome"), "ok");
                buffer->report_open = private_send && ok ? NULL : json_incref(report);
            }
            json_t *selection = json_object_get(value, "selection");
            if (selection && (!command || !json_is_object(selection) || !valid_route(selection)))
                return snag_errno(EPROTO);
            json_decref(buffer->selection);
            buffer->selection = json_incref(selection);
            json_decref(buffer->pending);
            buffer->pending = NULL;
            buffer->terminal_result = buffer->terminal_auto = false;
            buffer->submitting = false;
            buffer->query = false;
            buffer->receipt_at = 0u;
            buffer->reconcile_pending = false;
            buffer->draft_get = connection->drafts;
            if (command) {
                const char *error = snag_json_string(value, "report_error");
                buffer_message(buffer, error && *error ? error :
                    "Command completed; :reports reopens output");
            } else buffer_message(buffer, json_is_object(buffer->route) ?
                "Conversation message admitted" : "Prompt committed");
        } else if (!strcmp(status, "unknown") || !strcmp(status, "rejected")) {
            buffer->submitting = false;
            buffer->query = false;
            buffer->receipt_at = 0u;
            buffer->reconcile_pending = true;
            buffer_message(buffer, !strcmp(status, "unknown") ?
                "Submission outcome unknown; inspect history, then :recover if needed" :
                "Submission rejected; :recover restores its text");
        } else return snag_errno(EPROTO);
    } else if (!strcmp(type, "error")) {
        const char *id = snag_json_string(value, "id");
        uint64_t edit;
        if (json_object_get(value, "edit")) {
            if (snag_json_integer_u64(value, "edit", &edit) < 0 || !edit ||
                !connection->draft_sent || edit != connection->draft_edit)
                return snag_errno(EPROTO);
            /* Preserve local text and stop automatic updates after a refused
             * edit. A fresh attachment re-reads the owner before retrying. */
            return snag_errno(EPROTO);
        }
        for (struct snag_vm_buffer *buffer = connection->buffers; buffer; buffer = buffer->next) {
            if (!buffer->pending) continue;
            if (id && !strcmp(id, snag_json_string(buffer->pending, "id"))) {
                buffer->submitting = buffer->query = false;
                buffer->reconcile_pending = true;
                buffer->receipt_at = 0u;
                if (connection->inflight == buffer) connection->inflight = NULL;
            } else if (!id) {
                /* Control errors do not resolve a submission's outcome. */
                buffer->query = true;
                buffer->receipt_at = snag_monotonic_ms();
            }
        }
        if (!connection->detaching) connection->deadline = 0u;
        const char *error = snag_json_string(value, "message");
        message(connection, error ? error : "Owner refused the request");
    } else if (!strcmp(type, "exit")) {
        connection->exited = true;
        connection->control = false;
        snag_vm_connection_close(connection);
        message(connection, "Session stopped");
    } else if (!strcmp(type, "detached")) {
        bool conflict = false;
        for (struct snag_vm_buffer *b = connection->buffers; b; b = b->next)
            conflict |= b->draft_conflict || b->reconcile_pending;
        snag_vm_connection_close(connection);
        message(connection, conflict ? "Detached; unresolved draft retained in workspace" :
            "Detached; owner continues running");
    } else if (strcmp(type, "control")) return snag_errno(EPROTO);
    return 0;
}

void
snag_vm_connection_step(struct snag_vm_connection *connection)
{
    if (connection->channel.fd < 0) return;
    uint64_t now = snag_monotonic_ms();
    uint64_t deadlines[] = {connection->deadline, connection->channel.read_deadline,
        connection->channel.write_deadline, connection->draft_deadline};
    for (size_t i = 0u; i < sizeof(deadlines) / sizeof(deadlines[0]); ++i)
        if (deadlines[i] && now >= deadlines[i]) goto failed;
    if (connection->channel.output) {
        if (snag_view_channel_write(&connection->channel) < 0) goto failed;
        /* The channel already enforces wire progress. Start the reply wait
         * after sending, including when a large edit spans multiple frames. */
        if (connection->draft_deadline) connection->draft_deadline = now + 5000u;
        return;
    }
    json_t *value = NULL;
    int rc = snag_view_channel_read(&connection->channel, &value);
    if (rc > 0) rc = receive(connection, value);
    json_decref(value);
    if (rc < 0) goto failed;
    if (connection->hello && connection->reports_supported && !connection->reports_subscribed &&
        !connection->channel.output) {
        if (send_message(connection, json_pack("{s:s}", "type", "reports")) < 0) goto failed;
        connection->reports_subscribed = true;
    }
    for (struct snag_vm_buffer *b = connection->buffers; b; b = b->next) {
        if (!connection->bound || !b->pending || !b->query ||
            now < b->receipt_at || connection->channel.output) continue;
        b->query = false;
        b->receipt_at = 0u;
        if (strcmp(connection->instance, snag_json_string(b->pending, "instance"))) {
            buffer_message(b, "Owner changed; submission outcome unknown; inspect history");
        } else if (send_message(connection, json_pack("{s:s,s:s}", "type", "receipt",
            "id", snag_json_string(b->pending, "id"))) < 0) goto failed;
    }
    struct snag_vm_buffer *start = connection->sync_next ?
        connection->sync_next : connection->buffers;
    struct snag_vm_buffer *buffer = start;
    do {
        connection->sync_next = buffer->next ? buffer->next : connection->buffers;
        if (draft_sync(buffer) < 0) goto failed;
        if (connection->channel.output) break;
        buffer = connection->sync_next;
    } while (buffer != start);
    if (connection->bound && connection->detaching && !connection->detach_sent &&
        !connection->channel.output && !connection->draft_wait) {
        for (struct snag_vm_buffer *b = connection->buffers; b; b = b->next) {
            if (!snag_vm_buffer_writable(b) || !snag_vm_buffer_supported(b)) continue;
            if (connection->drafts && !b->reconcile_pending && !b->draft_conflict &&
                (b->draft_get || !b->draft_ready || b->draft_dirty)) return;
        }
        connection->detach_sent = true;
        connection->deadline = snag_monotonic_ms() + 5000u;
        if (snag_vm_connection_control(connection, "detach") < 0) goto failed;
    }
    return;
failed:
    snag_vm_connection_close(connection);
    message(connection, "Owner connection lost; drafts/submissions retained; :attach reconnects");
}

int
snag_vm_connection_wait(const struct snag_vm_connection *connection, uint64_t now, int timeout)
{
    uint64_t receipt_at = 0u;
    for (const struct snag_vm_buffer *b = connection->buffers; b; b = b->next) {
        if (b->receipt_at && (!receipt_at || b->receipt_at < receipt_at))
            receipt_at = b->receipt_at;
    }
    uint64_t deadlines[] = {connection->deadline, connection->channel.read_deadline,
        connection->channel.write_deadline, receipt_at, connection->draft_deadline};
    for (size_t i = 0u; i < sizeof(deadlines) / sizeof(deadlines[0]); ++i) {
        if (!deadlines[i]) continue;
        int remaining = deadlines[i] > now ? (int)(deadlines[i] - now) : 0;
        if (timeout < 0 || timeout > remaining) timeout = remaining;
    }
    return timeout;
}

static json_t *
buffer_json(const struct snag_vm_buffer *buffer)
{
    return json_pack("{s:O,s:s,s:I,s:O,s:O,s:O,s:I,s:s,s:{s:I,s:I,s:I}}", "route", buffer->route,
        "draft", buffer->draft.len ? (const char *)buffer->draft.data : "",
        "cursor", (json_int_t)buffer->cursor,
        "pending", buffer->pending ? buffer->pending : json_null(),
        "base", buffer->draft_base ? buffer->draft_base : json_null(),
        "conflict", buffer->conflict_draft ? buffer->conflict_draft : json_null(),
        "window", (json_int_t)buffer->request_window, "endpoint", buffer->endpoint,
        "read", "seq", (json_int_t)buffer->read_seq,
        "received", (json_int_t)buffer->read_received, "after", (json_int_t)buffer->read_after);
}

json_t *
snag_vm_connections_json(const struct snag_vm_connection *connection)
{
    json_t *rows = json_array();
    if (!rows) return NULL;
    for (; connection; connection = connection->next) {
        json_t *buffers = json_array();
        if (!buffers) goto failed;
        for (const struct snag_vm_buffer *b = connection->buffers; b; b = b->next) {
            json_t *saved = buffer_json(b);
            if (!saved || json_array_append_new(buffers, saved) < 0) {
                json_decref(buffers);
                goto failed;
            }
        }
        json_t *row = json_pack("{s:s,s:b,s:O,s:o}", "session", connection->session,
            "control", connection->control, "reports", connection->reports, "buffers", buffers);
        if (!row || json_array_append_new(rows, row) < 0) goto failed;
    }
    return rows;
failed:
    json_decref(rows);
    return NULL;
}

static int
load_buffer(struct snag_vm_connection *connection, const json_t *row, bool legacy)
{
    const json_t *route = legacy ? NULL : json_object_get(row, "route");
    const char *draft = snag_json_string(row, "draft");
    const json_t *read = json_object_get(row, "read");
    uint64_t cursor, window = 0u;
    const char *endpoint = legacy ? "" : snag_json_string(row, "endpoint");
    if (!endpoint || strlen(endpoint) > SNAG_CONFIG_IRC_ENDPOINT_MAX ||
        (!legacy && strlen(endpoint) != json_string_length(json_object_get(row, "endpoint"))) ||
        (!legacy && (!snag_json_exact_keys(row, read ?
            "route draft cursor pending base conflict window endpoint read" :
            "route draft cursor pending base conflict window endpoint") ||
            !valid_route(route) || snag_json_integer_u64(row, "window", &window) < 0)) ||
        !draft || strlen(draft) > SNAG_MAX_DIRECT_PROMPT ||
        strlen(draft) != json_string_length(json_object_get(row, "draft")) ||
        snag_json_integer_u64(row, "cursor", &cursor) < 0 || cursor > strlen(draft) ||
        snag_vm_text_floor(draft, strlen(draft), (size_t)cursor) != cursor) return -1;
    struct snag_vm_buffer *buffer = snag_vm_buffer_get(connection, route, true);
    if (!buffer || snag_vm_draft_replace(buffer, 0u, 0u, draft, strlen(draft)) < 0) return -1;
    if (read) {
        uint64_t seq, received, after;
        if (!snag_json_exact_keys(read, "seq received after") ||
            snag_json_integer_u64(read, "seq", &seq) < 0 ||
            snag_json_integer_u64(read, "received", &received) < 0 ||
            snag_json_integer_u64(read, "after", &after) < 0 || after > seq ||
            received > seq - after) return -1;
        if (seq > buffer->read_seq) {
            buffer->read_seq = seq;
            buffer->read_received = received;
            buffer->read_after = after;
        }
    }
    buffer->cursor = (size_t)cursor;
    buffer->request_window = window;
    (void)snag_strcpy(buffer->endpoint, sizeof(buffer->endpoint), endpoint);
    json_t *base = json_object_get(row, "base");
    json_t *conflict = json_object_get(row, "conflict");
    if (base && !json_is_null(base)) {
        const char *instance = snag_json_bounded_string(json_object_get(base, "instance"),
            SNAG_ID_HEX_LEN);
        const char *digest = snag_json_bounded_string(json_object_get(base, "sha256"),
            SNAG_SHA256_HEX_LEN);
        uint64_t revision;
        if (!snag_json_exact_keys(base, "instance revision sha256") || !instance ||
            !snag_hex_is_lower(instance, SNAG_ID_HEX_LEN) || !digest ||
            !snag_hex_is_lower(digest, SNAG_SHA256_HEX_LEN) ||
            snag_json_integer_u64(base, "revision", &revision) < 0 || !revision) return -1;
        buffer->draft_base = json_incref(base);
    }
    if (conflict && !json_is_null(conflict)) {
        if (!valid_draft(conflict) ||
            !json_equal(json_object_get(conflict, "route"), buffer->route)) return -1;
        buffer->conflict_draft = json_incref(conflict);
        buffer->draft_conflict = true;
    }
    json_t *pending = json_object_get(row, "pending");
    if (json_is_null(pending)) return 0;
    const char *id = snag_json_bounded_string(json_object_get(pending, "id"), SNAG_ID_HEX_LEN);
    const char *instance = snag_json_bounded_string(json_object_get(pending, "instance"),
        SNAG_ID_HEX_LEN);
    const char *text = snag_json_bounded_string(json_object_get(pending, "text"),
        SNAG_MAX_DIRECT_PROMPT);
    json_t *origin = json_object_get(pending, "origin");
    if (!snag_json_exact_keys(pending, origin ? "id instance text origin" : "id instance text") ||
        !id || !instance || !text ||
        !snag_hex_is_lower(id, SNAG_ID_HEX_LEN) ||
        !snag_hex_is_lower(instance, SNAG_ID_HEX_LEN)) return -1;
    if (origin) {
        const char *session = snag_json_bounded_string(json_object_get(origin, "session"),
            SNAG_ID_HEX_LEN);
        if (!snag_json_exact_keys(origin, "session route") || !session ||
            !snag_hex_is_lower(session, SNAG_ID_HEX_LEN) ||
            !strcmp(session, connection->session) || !snag_prompt_command(text) ||
            !valid_route(json_object_get(origin, "route"))) return -1;
    }
    for (struct snag_vm_buffer *b = connection->buffers; b; b = b->next)
        if (b->pending && !strcmp(id, snag_json_string(b->pending, "id"))) return -1;
    buffer->pending = json_incref(pending);
    buffer->origin = json_incref(origin);
    buffer_message(buffer, "Saved submission awaiting receipt reconciliation");
    return 0;
}

int
snag_vm_connections_load(const json_t *rows, struct snag_vm_connection **out)
{
    struct snag_vm_connection *head = NULL;
    if (!json_is_array(rows)) return snag_errno(EINVAL);
    for (size_t i = json_array_size(rows); i > 0u; --i) {
        const json_t *row = json_array_get(rows, i - 1u);
        const char *session = snag_json_bounded_string(json_object_get(row, "session"),
            SNAG_ID_HEX_LEN);
        const json_t *buffers = json_object_get(row, "buffers");
        bool revised = json_object_get(row, "base") != NULL;
        bool reports = json_object_get(row, "reports") != NULL;
        const char *keys = buffers ? "session control reports buffers" : reports ?
            "session draft cursor control pending base conflict reports" : revised ?
            "session draft cursor control pending base conflict" :
            "session draft cursor control pending";
        if (!snag_json_exact_keys(row, keys) || !session ||
            !snag_hex_is_lower(session, SNAG_ID_HEX_LEN) ||
            !json_is_boolean(json_object_get(row, "control"))) goto failed;
        for (struct snag_vm_connection *c = head; c; c = c->next)
            if (!strcmp(c->session, session)) goto failed;
        struct snag_vm_connection *connection = snag_vm_connection_new(session);
        if (!connection) goto failed;
        connection->next = head;
        head = connection;
        connection->control = json_is_true(json_object_get(row, "control"));
        if (buffers) {
            if (!json_is_array(buffers)) goto failed;
            for (size_t j = json_array_size(buffers); j > 0u; --j) {
                const json_t *saved = json_array_get(buffers, j - 1u);
                for (size_t k = 0u; k + 1u < j; ++k)
                    if (json_equal(json_object_get(saved, "route"),
                        json_object_get(json_array_get(buffers, k), "route"))) goto failed;
                if (load_buffer(connection, saved, false) < 0) goto failed;
            }
        } else if (load_buffer(connection, row, true) < 0) goto failed;
        if (reports) {
            json_t *saved = json_object_get(row, "reports");
            if (!json_is_array(saved)) goto failed;
            for (size_t j = 0u; j < json_array_size(saved); ++j) {
                json_t *report = json_array_get(saved, j);
                if (!snag_vm_report_valid(report)) goto failed;
                for (size_t k = 0u; k < j; ++k)
                    if (!strcmp(snag_json_string(report, "id"),
                        snag_json_string(json_array_get(saved, k), "id"))) goto failed;
            }
            json_decref(connection->reports);
            connection->reports = json_incref(saved);
        }
    }
    for (struct snag_vm_connection *c = head; c; c = c->next) {
        for (struct snag_vm_buffer *b = c->buffers; b; b = b->next) {
            if (!b->origin) continue;
            struct snag_vm_buffer *source = NULL;
            for (struct snag_vm_connection *owner = head; owner; owner = owner->next)
                if (!strcmp(owner->session, snag_json_string(b->origin, "session")))
                    source = snag_vm_buffer_get(owner, json_object_get(b->origin, "route"), false);
            if (!snag_vm_buffer_writable(source) || source->pending) goto failed;
            for (struct snag_vm_connection *owner = head; owner; owner = owner->next)
                for (struct snag_vm_buffer *other = owner->buffers; other; other = other->next)
                    if (other != b && json_equal(b->origin, other->origin)) goto failed;
        }
    }
    *out = head;
    return 0;
failed:
    snag_vm_connections_free(head);
    return snag_errno(EINVAL);
}
