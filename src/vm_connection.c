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
    return 0;
}

int
snag_vm_connection_prepare(struct snag_vm_connection *connection)
{
    if (!connection->bound || connection->pending || !connection->draft.len ||
        connection->quitting) return snag_errno(EBUSY);
    char id[SNAG_ID_HEX_LEN + 1u];
    if (snag_random_id(id) < 0) return -1;
    json_t *pending = json_pack("{s:s,s:s,s:s}", "id", id, "instance", connection->instance,
        "text", (const char *)connection->draft.data);
    if (!pending) return -1;
    connection->pending = pending;
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
    if (!connection->bound || !connection->pending || connection->channel.output)
        return snag_errno(EBUSY);
    int rc = send_message(connection, json_pack("{s:s,s:I,s:s,s:s}", "type", "submit",
        "generation", (json_int_t)connection->generation,
        "id", snag_json_string(connection->pending, "id"),
        "text", snag_json_string(connection->pending, "text")));
    if (!rc) connection->submitting = true;
    return rc;
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
        message(connection, "Attached  i: edit prompt  :detach: keep owner running");
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
            json_decref(connection->pending);
            connection->pending = NULL;
            connection->submitting = false;
            connection->query = false;
            connection->receipt_at = 0u;
            message(connection, "Prompt committed");
        } else if (!strcmp(status, "unknown") || !strcmp(status, "rejected")) {
            connection->submitting = false;
            connection->query = false;
            connection->receipt_at = 0u;
            message(connection, !strcmp(status, "unknown") ?
                "Submission outcome unknown; inspect history, then :recover if needed" :
                "Submission rejected; :recover restores its text");
        } else return snag_errno(EPROTO);
    } else if (!strcmp(type, "error")) {
        const char *id = snag_json_string(value, "id");
        if (connection->pending && id && !strcmp(id, snag_json_string(connection->pending, "id"))) {
            connection->submitting = connection->query = false;
            connection->receipt_at = 0u;
        } else if (connection->pending) {
            /* A control/uncorrelated error does not reject the in-flight
             * submission. Query its own receipt before allowing recovery. */
            connection->query = true;
            connection->receipt_at = snag_monotonic_ms();
        }
        connection->deadline = 0u;
        const char *error = snag_json_string(value, "message");
        message(connection, error ? error : "Owner refused the request");
    } else if (!strcmp(type, "exit")) {
        connection->exited = true;
        connection->control = false;
        snag_vm_connection_close(connection);
        message(connection, "Session stopped");
    } else if (!strcmp(type, "detached")) {
        connection->control = false;
        snag_vm_connection_close(connection);
        message(connection, "Detached; owner continues running");
    } else if (strcmp(type, "control")) return snag_errno(EPROTO);
    return 0;
}

void
snag_vm_connection_step(struct snag_vm_connection *connection)
{
    if (connection->channel.fd < 0) return;
    uint64_t now = snag_monotonic_ms();
    uint64_t deadlines[] = {connection->deadline, connection->channel.read_deadline,
        connection->channel.write_deadline};
    for (size_t i = 0u; i < sizeof(deadlines) / sizeof(deadlines[0]); ++i)
        if (deadlines[i] && now >= deadlines[i]) goto failed;
    if (connection->channel.output) {
        if (snag_view_channel_write(&connection->channel) < 0) goto failed;
        return;
    }
    json_t *value = NULL;
    int rc = snag_view_channel_read(&connection->channel, &value);
    if (rc > 0) rc = receive(connection, value);
    json_decref(value);
    if (rc < 0) goto failed;
    if (connection->hello && connection->pending && connection->query &&
        now >= connection->receipt_at && !connection->channel.output) {
        connection->query = false;
        connection->receipt_at = 0u;
        if (strcmp(connection->instance, snag_json_string(connection->pending, "instance"))) {
            message(connection, "Owner changed; submission outcome unknown; inspect history");
        } else if (send_message(connection, json_pack("{s:s,s:s}", "type", "receipt",
            "id", snag_json_string(connection->pending, "id"))) < 0) goto failed;
    }
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
        connection->channel.write_deadline, connection->receipt_at};
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
        json_t *row = json_pack("{s:s,s:s,s:I,s:b,s:O}", "session", connection->session,
            "draft", connection->draft.len ? (const char *)connection->draft.data : "",
            "cursor", (json_int_t)connection->cursor, "control", connection->control,
            "pending", connection->pending ? connection->pending : json_null());
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
        if (!snag_json_exact_keys(row, "session draft cursor control pending") ||
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
