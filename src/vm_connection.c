/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_connection.h"
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

struct snag_vm_connection *
snag_vm_connection_new(const char *session)
{
    struct snag_vm_connection *connection = calloc(1u, sizeof(*connection));
    if (!connection) return NULL;
    snag_view_channel_init(&connection->channel, -1);
    snag_buf_init(&connection->draft, SNAG_MAX_DIRECT_PROMPT + 1u);
    (void)snprintf(connection->session, sizeof(connection->session), "%s", session);
    return connection;
}

void
snag_vm_connection_close(struct snag_vm_connection *connection)
{
    snag_view_channel_close(&connection->channel);
    connection->bound = connection->hello = connection->submitting = false;
    connection->generation = connection->deadline = connection->receipt_at = 0u;
    connection->drafts = connection->draft_ready = connection->draft_get = false;
    connection->send_pending = connection->detaching = connection->detach_sent = false;
    connection->draft_deadline = 0u;
    json_decref(connection->draft_sent);
    connection->draft_sent = NULL;
    json_decref(connection->state);
    connection->state = NULL;
}

void
snag_vm_connections_free(struct snag_vm_connection *connection)
{
    while (connection) {
        struct snag_vm_connection *next = connection->next;
        snag_vm_connection_close(connection);
        if (connection->draft.data) memset(connection->draft.data, 0, connection->draft.len);
        snag_buf_free(&connection->draft);
        json_decref(connection->pending);
        json_decref(connection->draft_base);
        json_decref(connection->owner_draft);
        json_decref(connection->conflict_draft);
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
            "Owner attachment unavailable: %s; retained history is readable", strerror(errno));
    }
    snag_session_close(&location);
    if (fd < 0) {
        message(connection, error);
        return -1;
    }
    snag_view_channel_init(&connection->channel, fd);
    connection->deadline = snag_monotonic_ms() + 15000u;
    connection->query = connection->pending != NULL;
    connection->reconcile_pending = connection->query;
    message(connection, control ? "Attaching to owner" : "Observing owner");
    return send_message(connection, json_pack("{s:s,s:i}", "type", "hello", "version", 1));
}

int
snag_vm_draft_replace(struct snag_vm_connection *connection, size_t begin, size_t end,
    const void *text, size_t length)
{
    struct snag_buf *draft = &connection->draft;
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
    connection->cursor = begin + length;
    size_t boundary = snag_vm_text_floor((const char *)draft->data, total, connection->cursor);
    if (boundary != connection->cursor)
        connection->cursor = snag_vm_text_next((const char *)draft->data, total, boundary);
    ++connection->revision;
    connection->draft_dirty = true;
    return 0;
}

void
snag_vm_draft_cursor(struct snag_vm_connection *connection, size_t cursor)
{
    if (connection->cursor == cursor) return;
    connection->cursor = cursor;
    connection->draft_dirty = true;
    ++connection->revision;
}

static const char *
draft_text(const struct snag_vm_connection *connection)
{
    if (connection->send_pending) return snag_json_string(connection->pending, "text");
    return connection->draft.len ? (const char *)connection->draft.data : "";
}

static bool
valid_draft(const json_t *draft)
{
    const char *text = snag_json_string(draft, "text");
    const char *route = snag_json_string(draft, "route");
    uint64_t revision, cursor;
    return snag_json_exact_keys(draft, "route revision text cursor") &&
        route && !strcmp(route, "rollout") && text &&
        strlen(text) == json_string_length(json_object_get(draft, "text")) &&
        strlen(text) <= SNAG_MAX_DIRECT_PROMPT &&
        snag_json_integer_u64(draft, "revision", &revision) == 0 && revision &&
        snag_json_integer_u64(draft, "cursor", &cursor) == 0 && cursor <= strlen(text) &&
        snag_vm_text_floor(text, strlen(text), (size_t)cursor) == cursor;
}

static int
draft_baseline(struct snag_vm_connection *connection, const json_t *draft)
{
    char digest[SNAG_SHA256_HEX_LEN + 1u];
    const char *text = snag_json_string(draft, "text");
    snag_sha256_hex(text, strlen(text), digest);
    json_t *base = json_pack("{s:s,s:O,s:s}", "instance", connection->instance,
        "revision", json_object_get(draft, "revision"), "sha256", digest);
    if (!base) return -1;
    json_decref(connection->draft_base);
    connection->draft_base = base;
    ++connection->revision;
    return 0;
}

static bool
at_baseline(const struct snag_vm_connection *connection, const char *text)
{
    if (!connection->draft_base || strcmp(connection->instance,
        snag_json_string(connection->draft_base, "instance"))) return false;
    char digest[SNAG_SHA256_HEX_LEN + 1u];
    snag_sha256_hex(text, strlen(text), digest);
    return !strcmp(digest, snag_json_string(connection->draft_base, "sha256"));
}

int
snag_vm_draft_choose(struct snag_vm_connection *connection, bool local)
{
    if (!connection->draft_conflict || connection->pending ||
        (connection->bound && connection->drafts && !connection->draft_ready))
        return snag_errno(EBUSY);
    if (!local) {
        const char *text = snag_json_string(connection->conflict_draft, "text");
        if (snag_vm_draft_replace(connection, 0u, connection->draft.len, text, strlen(text)) < 0)
            return -1;
        connection->cursor = (size_t)json_integer_value(
            json_object_get(connection->conflict_draft, "cursor"));
    }
    if (connection->draft_ready) {
        if (draft_baseline(connection, connection->owner_draft) < 0) return -1;
    } else {
        json_decref(connection->draft_base);
        connection->draft_base = NULL;
    }
    connection->draft_conflict = false;
    json_decref(connection->conflict_draft);
    connection->conflict_draft = NULL;
    connection->draft_dirty = true;
    message(connection, local ? "Using workspace draft" : "Using owner draft");
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
snag_vm_connection_prepare(struct snag_vm_connection *connection)
{
    if (!connection->bound || connection->pending || !connection->draft.len ||
        connection->quitting || connection->detaching || connection->draft_conflict)
        return snag_errno(EBUSY);
    char id[SNAG_ID_HEX_LEN + 1u];
    if (snag_random_id(id) < 0) return -1;
    json_t *pending = json_pack("{s:s,s:s,s:s}", "id", id, "instance", connection->instance,
        "text", (const char *)connection->draft.data);
    if (!pending) return -1;
    connection->pending = pending;
    connection->reconcile_pending = true;
    /* The pending copy is persisted before transmission; new typing belongs
     * to the next draft and can never be erased by this request's receipt. */
    snag_buf_reset(&connection->draft);
    connection->cursor = 0u;
    message(connection, "Submission saved; awaiting durable receipt");
    return 0;
}

int
snag_vm_connection_send(struct snag_vm_connection *connection)
{
    if (!connection->bound || !connection->pending || connection->detaching)
        return snag_errno(EBUSY);
    connection->send_pending = connection->draft_dirty = true;
    connection->reconcile_pending = false;
    return 0;
}

int
snag_vm_connection_recover(struct snag_vm_connection *connection)
{
    if (!connection->pending || connection->draft.len || connection->submitting)
        return snag_errno(EBUSY);
    const char *text = snag_json_string(connection->pending, "text");
    if (snag_vm_draft_replace(connection, 0u, 0u, text, strlen(text)) < 0) return -1;
    json_decref(connection->pending);
    connection->pending = NULL;
    connection->query = false;
    connection->send_pending = connection->reconcile_pending = false;
    connection->draft_get = connection->drafts;
    connection->receipt_at = 0u;
    message(connection, "Submission recovered into draft; verify history before sending again");
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
draft_sync(struct snag_vm_connection *connection)
{
    if (!connection->bound || connection->channel.output || connection->draft_sent ||
        connection->quitting || connection->detach_sent || connection->draft_deadline) return 0;
    if (connection->drafts && connection->draft_get && !connection->reconcile_pending) {
        connection->draft_get = false;
        connection->draft_ready = false;
        connection->draft_deadline = snag_monotonic_ms() + 5000u;
        return send_message(connection, json_pack("{s:s,s:I,s:s}", "type", "draft_get",
            "generation", (json_int_t)connection->generation, "route", "rollout"));
    }
    if (connection->drafts && !connection->reconcile_pending && connection->draft_ready &&
        connection->draft_dirty && !connection->draft_conflict) {
        const char *text = draft_text(connection);
        const char *owner = snag_json_string(connection->owner_draft, "text");
        size_t cursor = connection->send_pending ? strlen(text) : connection->cursor;
        bool same = !strcmp(text, owner);
        bool owner_unchanged = at_baseline(connection, owner);
        bool local_unchanged = at_baseline(connection, text);
        bool base = connection->draft_base && !strcmp(connection->instance,
            snag_json_string(connection->draft_base, "instance"));
        if (same || owner_unchanged || (!base && !*owner)) {
            connection->draft_conflict = false;
            if (same && draft_baseline(connection, connection->owner_draft) < 0) return -1;
            if (same && cursor == (size_t)json_integer_value(
                json_object_get(connection->owner_draft, "cursor"))) {
                connection->draft_dirty = false;
            } else {
                if (connection->draft_edit == INT64_MAX) return snag_errno(EOVERFLOW);
                json_t *edit = json_pack("{s:s,s:I,s:s,s:O,s:I,s:s,s:I}",
                    "type", "draft", "generation", (json_int_t)connection->generation,
                    "route", "rollout", "revision",
                    json_object_get(connection->owner_draft, "revision"),
                    "edit", (json_int_t)++connection->draft_edit, "text", text,
                    "cursor", (json_int_t)cursor);
                if (!edit) return -1;
                connection->draft_sent = json_incref(edit);
                connection->draft_deadline = snag_monotonic_ms() + 5000u;
                return send_message(connection, edit);
            }
        } else if (!connection->send_pending && (local_unchanged || (!base && !*text))) {
            if (snag_vm_draft_replace(connection, 0u, connection->draft.len,
                owner, strlen(owner)) < 0 ||
                draft_baseline(connection, connection->owner_draft) < 0)
                return -1;
            connection->cursor = (size_t)json_integer_value(
                json_object_get(connection->owner_draft, "cursor"));
            connection->draft_dirty = connection->draft_conflict = false;
            message(connection, "Owner draft restored");
        } else {
            connection->draft_dirty = false;
            connection->draft_conflict = true;
            json_decref(connection->conflict_draft);
            connection->conflict_draft = json_incref(connection->owner_draft);
            connection->send_pending = false;
            message(connection, connection->pending ?
                "Draft conflict; :recover the unsent prompt, then :draft local or :draft owner" :
                "Draft conflict; both copies retained; :draft local or :draft owner");
        }
    }
    if (connection->send_pending && (!connection->drafts ||
        (connection->draft_ready && !connection->draft_dirty && !connection->draft_conflict))) {
        json_t *request = json_pack("{s:s,s:I,s:s,s:s}", "type", "submit",
            "generation", (json_int_t)connection->generation,
            "id", snag_json_string(connection->pending, "id"),
            "text", snag_json_string(connection->pending, "text"));
        if (!request) return -1;
        if (connection->drafts &&
            (json_object_set_new(request, "route", json_string("rollout")) < 0 ||
             json_object_set(request, "draft_revision",
                json_object_get(connection->owner_draft, "revision")) < 0)) {
            json_decref(request);
            return -1;
        }
        if (send_message(connection, request) < 0) return -1;
        connection->send_pending = false;
        connection->submitting = true;
        connection->draft_dirty = connection->draft.len != 0u;
        return 0;
    }
    if (connection->detaching && !connection->detach_sent) {
        connection->detach_sent = true;
        connection->deadline = snag_monotonic_ms() + 5000u;
        return snag_vm_connection_control(connection, "detach");
    }
    return 0;
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
        connection->draft_get = connection->drafts;
        message(connection, "Attached  i: edit prompt  :detach: keep owner running");
    } else if (!strcmp(type, "draft")) {
        json_t *draft = json_object_get(value, "draft");
        const char *status = snag_json_string(value, "status");
        uint64_t edit;
        if (!connection->drafts || !connection->bound || !valid_draft(draft) ||
            !snag_json_exact_keys(value, "type edit status draft") || !status ||
            snag_json_integer_u64(value, "edit", &edit) < 0 ||
            (edit ? (!connection->draft_sent || edit != connection->draft_edit ||
                     (strcmp(status, "accepted") && strcmp(status, "conflict"))) :
                    strcmp(status, "snapshot"))) return snag_errno(EPROTO);
        if (edit) {
            if (!strcmp(status, "accepted") && draft_baseline(connection, draft) < 0) return -1;
            json_decref(connection->draft_sent);
            connection->draft_sent = NULL;
            connection->draft_deadline = 0u;
        } else if (!connection->draft_sent) connection->draft_deadline = 0u;
        json_decref(connection->owner_draft);
        connection->owner_draft = json_incref(draft);
        connection->draft_ready = connection->draft_dirty = true;
        if (connection->draft_conflict)
            message(connection, "Draft conflict retained; :draft local or :draft owner");
        ++connection->revision;
    } else if (!strcmp(type, "state")) {
        json_t *state = json_object_get(value, "state");
        if (!json_is_object(state)) return snag_errno(EPROTO);
        json_decref(connection->state);
        connection->state = json_incref(state);
        ++connection->revision;
    } else if (!strcmp(type, "result")) {
        const char *id = snag_json_string(value, "id");
        const char *status = snag_json_string(value, "status");
        if (!id || !status) return snag_errno(EPROTO);
        if (!connection->pending || strcmp(id, snag_json_string(connection->pending, "id")))
            return 0;
        if (!strcmp(status, "pending")) {
            connection->submitting = true;
            /* A queried pending receipt is not subscribed to the original
             * submitter's completion. Recheck until it reaches a final state. */
            connection->query = true;
            connection->receipt_at = snag_monotonic_ms() + 250u;
        } else if (!strcmp(status, "committed")) {
            uint64_t seq;
            if (snag_json_integer_u64(value, "seq", &seq) < 0 || !seq ||
                strcmp(connection->instance, snag_json_string(connection->pending, "instance")))
                return snag_errno(EPROTO);
            uint64_t cleared = 0u;
            if (connection->drafts &&
                snag_json_integer_u64(value, "draft_cleared", &cleared) < 0)
                return snag_errno(EPROTO);
            if (cleared && (!connection->draft_base || cleared > (uint64_t)json_integer_value(
                json_object_get(connection->draft_base, "revision")))) {
                json_t *empty = json_pack("{s:I,s:s}", "revision", (json_int_t)cleared,
                    "text", "");
                int rc = empty ? draft_baseline(connection, empty) : -1;
                json_decref(empty);
                if (rc < 0) return -1;
            }
            json_decref(connection->pending);
            connection->pending = NULL;
            connection->submitting = false;
            connection->query = false;
            connection->receipt_at = 0u;
            connection->reconcile_pending = false;
            connection->draft_get = connection->drafts;
            message(connection, "Prompt committed");
        } else if (!strcmp(status, "unknown") || !strcmp(status, "rejected")) {
            connection->submitting = false;
            connection->query = false;
            connection->receipt_at = 0u;
            connection->reconcile_pending = true;
            message(connection, !strcmp(status, "unknown") ?
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
        if (connection->pending && id && !strcmp(id, snag_json_string(connection->pending, "id"))) {
            connection->submitting = connection->query = false;
            connection->reconcile_pending = true;
            connection->receipt_at = 0u;
        } else if (connection->pending) {
            /* A control/uncorrelated error does not reject the in-flight
             * submission. Query its own receipt before allowing recovery. */
            connection->query = true;
            connection->receipt_at = snag_monotonic_ms();
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
        bool conflict = connection->draft_conflict || connection->reconcile_pending;
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
    if (connection->bound && connection->pending && connection->query &&
        now >= connection->receipt_at && !connection->channel.output) {
        connection->query = false;
        connection->receipt_at = 0u;
        if (strcmp(connection->instance, snag_json_string(connection->pending, "instance"))) {
            message(connection, "Owner changed; submission outcome unknown; inspect history");
        } else if (send_message(connection, json_pack("{s:s,s:s}", "type", "receipt",
            "id", snag_json_string(connection->pending, "id"))) < 0) goto failed;
    }
    if (draft_sync(connection) < 0) goto failed;
    return;
failed:
    snag_vm_connection_close(connection);
    message(connection, connection->pending ?
        "Owner connection lost; submission retained; :attach reconnects and checks its receipt" :
        "Owner connection lost; draft retained; :attach reconnects");
}

int
snag_vm_connection_wait(const struct snag_vm_connection *connection, uint64_t now, int timeout)
{
    uint64_t deadlines[] = {connection->deadline, connection->channel.read_deadline,
        connection->channel.write_deadline, connection->receipt_at, connection->draft_deadline};
    for (size_t i = 0u; i < sizeof(deadlines) / sizeof(deadlines[0]); ++i) {
        if (!deadlines[i]) continue;
        int remaining = deadlines[i] > now ? (int)(deadlines[i] - now) : 0;
        if (timeout < 0 || timeout > remaining) timeout = remaining;
    }
    return timeout;
}

json_t *
snag_vm_connections_json(const struct snag_vm_connection *connection)
{
    json_t *rows = json_array();
    if (!rows) return NULL;
    for (; connection; connection = connection->next) {
        json_t *row = json_pack("{s:s,s:s,s:I,s:b,s:O,s:O,s:O}", "session", connection->session,
            "draft", connection->draft.len ? (const char *)connection->draft.data : "",
            "cursor", (json_int_t)connection->cursor, "control", connection->control,
            "pending", connection->pending ? connection->pending : json_null(),
            "base", connection->draft_base ? connection->draft_base : json_null(),
            "conflict", connection->conflict_draft ? connection->conflict_draft : json_null());
        if (!row || json_array_append_new(rows, row) < 0) {
            json_decref(rows);
            return NULL;
        }
    }
    return rows;
}

int
snag_vm_connections_load(const json_t *rows, struct snag_vm_connection **out)
{
    struct snag_vm_connection *head = NULL;
    if (!json_is_array(rows)) return snag_errno(EINVAL);
    for (size_t i = 0u; i < json_array_size(rows); ++i) {
        const json_t *row = json_array_get(rows, i);
        const char *session = snag_json_bounded_string(json_object_get(row, "session"),
            SNAG_ID_HEX_LEN);
        const char *draft = snag_json_string(row, "draft");
        uint64_t cursor;
        bool revised = json_object_get(row, "base") != NULL;
        if (!snag_json_exact_keys(row, revised ?
            "session draft cursor control pending base conflict" :
            "session draft cursor control pending") ||
            !session || strlen(session) != SNAG_ID_HEX_LEN ||
            !snag_hex_is_lower(session, SNAG_ID_HEX_LEN) || !draft ||
            strlen(draft) != json_string_length(json_object_get(row, "draft")) ||
            strlen(draft) > SNAG_MAX_DIRECT_PROMPT ||
            snag_json_integer_u64(row, "cursor", &cursor) < 0 || cursor > strlen(draft) ||
            !json_is_boolean(json_object_get(row, "control")) ||
            snag_vm_text_floor(draft, strlen(draft), (size_t)cursor) != cursor) goto failed;
        for (struct snag_vm_connection *c = head; c; c = c->next)
            if (!strcmp(c->session, session)) goto failed;
        struct snag_vm_connection *connection = snag_vm_connection_new(session);
        if (!connection) goto failed;
        connection->next = head;
        head = connection;
        if (snag_vm_draft_replace(connection, 0u, 0u, draft, strlen(draft)) < 0) goto failed;
        connection->cursor = (size_t)cursor;
        connection->control = json_is_true(json_object_get(row, "control"));
        if (revised) {
            json_t *base = json_object_get(row, "base");
            json_t *conflict = json_object_get(row, "conflict");
            if (!json_is_null(base)) {
                const char *instance = snag_json_bounded_string(json_object_get(base, "instance"),
                    SNAG_ID_HEX_LEN);
                const char *digest = snag_json_bounded_string(json_object_get(base, "sha256"),
                    SNAG_SHA256_HEX_LEN);
                uint64_t revision;
                if (!snag_json_exact_keys(base, "instance revision sha256") || !instance ||
                    strlen(instance) != SNAG_ID_HEX_LEN ||
                    !snag_hex_is_lower(instance, SNAG_ID_HEX_LEN) || !digest ||
                    strlen(digest) != SNAG_SHA256_HEX_LEN ||
                    !snag_hex_is_lower(digest, SNAG_SHA256_HEX_LEN) ||
                    snag_json_integer_u64(base, "revision", &revision) < 0 || !revision)
                    goto failed;
                connection->draft_base = json_incref(base);
            }
            if (!json_is_null(conflict)) {
                if (!valid_draft(conflict)) goto failed;
                connection->conflict_draft = json_incref(conflict);
                connection->draft_conflict = true;
            }
        }
        json_t *pending = json_object_get(row, "pending");
        if (json_is_null(pending)) continue;
        const char *id = snag_json_bounded_string(json_object_get(pending, "id"), SNAG_ID_HEX_LEN);
        const char *instance = snag_json_bounded_string(json_object_get(pending, "instance"),
            SNAG_ID_HEX_LEN);
        const char *text = snag_json_bounded_string(json_object_get(pending, "text"),
            SNAG_MAX_DIRECT_PROMPT);
        if (!snag_json_exact_keys(pending, "id instance text") || !id || !instance || !text ||
            strlen(id) != SNAG_ID_HEX_LEN || !snag_hex_is_lower(id, SNAG_ID_HEX_LEN) ||
            strlen(instance) != SNAG_ID_HEX_LEN || !snag_hex_is_lower(instance, SNAG_ID_HEX_LEN))
            goto failed;
        connection->pending = json_incref(pending);
        message(connection, "Saved submission awaiting receipt reconciliation");
    }
    *out = head;
    return 0;
failed:
    snag_vm_connections_free(head);
    return snag_errno(EINVAL);
}
