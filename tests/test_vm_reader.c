/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_reader.h"
#include "fs.h"
#include "history_view.h"
#include "json.h"
#include "vm_connection.h"
#include "vm_report.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#endif

static struct snag_vm_read_result *
await_page(struct snag_vm_reader *reader, uint64_t generation)
{
    uint64_t start = snag_monotonic_ms();
    struct snag_vm_read_result *result;
    while (!(result = snag_vm_reader_take(reader))) {
        assert(snag_monotonic_ms() - start < 10000u);
        assert(snag_wakeup_wait(snag_vm_reader_fd(reader), 100) >= 0);
    }
    assert(result->generation == generation);
    return result;
}

static bool
cancel_report(void *opaque)
{
    unsigned int *calls = opaque;
    return ++*calls >= 3u;
}

static void
report_test(struct snag_store *store, const char *root)
{
    struct snag_session source;
    char error[256] = "";
    snag_session_init(&source);
    assert(snag_session_create(store, &source, root, "default", "reports", "high",
        error, sizeof(error)) == 0);
    char sid[SNAG_ID_HEX_LEN + 1u], id[SNAG_ID_HEX_LEN + 1u], name[64];
    memcpy(sid, source.id, sizeof(sid));
    assert(snag_random_id(id) == 0);
    (void)snprintf(name, sizeof(name), ".view-report-%s", id);
    const char *values[] = {"test-secret-value"};
    struct snag_wire_secrets secrets = {.values = values, .count = 1u};
    size_t length = 7u * 1024u * 1024u;
    unsigned char *data = malloc(length);
    assert(data);
    memset(data, 'x', length);
    memcpy(data + 65530u, values[0], strlen(values[0]));
    data[0] = 0xff;
    data[1] = 0;
    struct snag_sha256 digest;
    char hash[SNAG_SHA256_HEX_LEN + 1u];
    snag_sha256_init(&digest);
    snag_sha256_update(&digest, data, length);
    snag_sha256_final_hex(&digest, hash);
    int fd = snag_create_private_at(source.dir_fd, name, true);
    assert(fd >= 0 && snag_write_full(fd, data, length) == 0 && close(fd) == 0);
    json_t *report = json_pack("{s:s,s:I,s:s,s:s}", "id", id, "bytes", (json_int_t)length,
        "sha256", hash, "command", "/status test-secret-value");
    assert(snag_vm_report_valid(report));
    unsigned int calls = 0u;
    assert(!snag_vm_report_read(store, sid, report, 80u, &secrets,
        cancel_report, &calls, error, sizeof(error)) && errno == ECANCELED);
    /* Report reads remain available after the session's owner exits. */
    snag_session_close(&source);
    struct snag_vm_reader *reader = snag_vm_reader_open(store, &secrets, error, sizeof(error));
    assert(reader);
    struct snag_vm_read_request request = {.kind = SNAG_VM_READ_REPORT, .report = report,
        .columns = 80u};
    memcpy(request.session_id, sid, sizeof(sid));
    uint64_t generation = snag_vm_reader_request(reader, &request);
    assert(generation);
    assert(json_object_set_new(report, "command", json_string("changed after request")) == 0);
    struct snag_vm_read_result *result = await_page(reader, generation);
    assert(!result->error_number && result->document && !result->events);
    assert(!strcmp(snag_json_string(result->request.report, "command"),
        "/status test-secret-value"));
    const char *text = snag_json_string(snag_vm_document_block(result->document, 0u), "text");
    assert(text && !strncmp(text, "\\xff\\x00", 8u) && strstr(text, "[redacted]") &&
        !strstr(text, values[0]));
    assert(snag_vm_document_rows(result->document) > 90000u);
    snag_vm_read_result_free(result);
    assert(snag_session_locate(store, &source, sid, NULL, NULL, error, sizeof(error)) == 0);
    /* Same-size edits, truncation and permissions fail closed. */
    assert(snag_unlink_at(source.dir_fd, name, false) == 0);
    data[10] = 'y';
    fd = snag_create_private_at(source.dir_fd, name, true);
    assert(fd >= 0 && snag_write_full(fd, data, length) == 0 && close(fd) == 0);
    result = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(result->error_number == ESTALE && !result->document);
    snag_vm_read_result_free(result);
    assert(snag_unlink_at(source.dir_fd, name, false) == 0);
    fd = snag_create_private_at(source.dir_fd, name, true);
    assert(fd >= 0 && snag_write_full(fd, data, length - 1u) == 0 && close(fd) == 0);
    result = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(result->error_number == ESTALE && !result->document);
    snag_vm_read_result_free(result);
#ifndef _WIN32
    assert(fchmodat(source.dir_fd, name, 0644, 0) == 0);
    result = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(result->error_number && !result->document);
    snag_vm_read_result_free(result);
    assert(snag_unlink_at(source.dir_fd, name, false) == 0);
    assert(symlinkat("events.jsonl", source.dir_fd, name) == 0);
    result = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(result->error_number && !result->document);
    snag_vm_read_result_free(result);
#endif
    assert(snag_unlink_at(source.dir_fd, name, false) == 0);
    result = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(result->error_number == ENOENT && !result->document);
    snag_vm_read_result_free(result);
    assert(json_object_set_new(report, "id", json_string("../../events.jsonl")) == 0);
    assert(!snag_vm_report_valid(report));
    assert(!snag_vm_reader_request(reader, &request));
    json_decref(report);
    snag_vm_reader_close(reader);
    snag_session_close(&source);
    free(data);
}

static json_t *
large_context(void *opaque, const struct snag_session *session)
{
    (void)opaque;
    (void)session;
    size_t size = 17u * 1024u * 1024u;
    char *bytes = malloc(size);
    assert(bytes);
    memset(bytes, 'x', size);
    json_t *value = json_stringn(bytes, size);
    free(bytes);
    assert(value);
    return json_pack("{s:o}", "private_checkpoint_payload", value);
}

static void
projection_test(void)
{
    const char *values[] = {"test-secret-value"};
    struct snag_wire_secrets secrets = {.values = values, .count = 1u};
    json_t *data = json_pack("{s:s,s:s}", "continuation", "opaque-private-payload",
        "text", "before test-secret-value after");
    char error[256];
    char *text = snag_history_event_data(3u, "response_completed", data,
        &secrets, error, sizeof(error));
    assert(text && !strstr(text, "test-secret-value") &&
        !strstr(text, "opaque-private-payload") && strstr(text, "provider_payload_omitted"));
    assert(!strcmp(snag_json_string(data, "continuation"), "opaque-private-payload"));
    free(text);
    json_decref(data);
    data = json_pack("{s:[{s:s,s:s}]}", "output", "encrypted_content", "opaque-private-payload",
        "text", "test-secret-value");
    text = snag_history_event_data(4u, "compaction_completed", data,
        &secrets, error, sizeof(error));
    assert(text && !strstr(text, "test-secret-value") && !strstr(text, "opaque-private-payload"));
    free(text);
    json_decref(data);
}

static void
mode_identity_test(struct snag_store *store, const char *root)
{
    struct snag_session source;
    char error[256];
    snag_session_init(&source);
    assert(snag_session_create(store, &source, root, "default", "mode-test", "high",
        error, sizeof(error)) == 0);
    source.on_checkpoint = large_context;
    assert(snag_session_checkpoint(&source, error, sizeof(error)) == 0);
    source.on_checkpoint = NULL;
    struct snag_vm_read_request request = {.refresh = true};
    memcpy(request.session_id, source.id, sizeof(request.session_id));
    request.tail.offset = source.log_end;
    request.tail.next_seq = source.next_seq;
    memcpy(request.tail.prev_sha256, source.prev_sha256, sizeof(request.tail.prev_sha256));
    char *bytes = malloc((size_t)source.log_end);
    assert(bytes);
    int fd = snag_open_read_at(source.dir_fd, "events.jsonl", false);
    assert(fd >= 0);
    size_t used = 0u;
    while (used < (size_t)source.log_end) {
        ssize_t count = read(fd, bytes + used, (size_t)source.log_end - used);
        assert(count > 0);
        used += (size_t)count;
    }
    assert(close(fd) == 0);
    for (unsigned int trusted = 0u; trusted < 2u; ++trusted) {
        struct snag_vm_reader *reader = snag_vm_reader_open(store, NULL, error, sizeof(error));
        assert(reader);
        request.trusted_tail = trusted != 0u;
        struct snag_vm_read_result *result = await_page(reader,
            snag_vm_reader_request(reader, &request));
        assert(!result->error_number);
        snag_vm_read_result_free(result);
        /* Supersede a checkpoint read before changing modes. Its validated
         * cached source must survive whether cancellation catches queued,
         * running or already completed work. */
        request.reverse = true;
        assert(snag_vm_reader_request(reader, &request));
        assert(snag_sleep_ms(10u) == 0);
        snag_vm_reader_cancel(reader);
        assert(snag_rename_at(source.dir_fd, "events.jsonl", source.dir_fd, "events.before") == 0);
        fd = snag_create_private_at(source.dir_fd, "events.jsonl", true);
        assert(fd >= 0 && snag_write_full(fd, bytes, used) == 0 && close(fd) == 0);
        request.trusted_tail = !request.trusted_tail;
        result = await_page(reader, snag_vm_reader_request(reader, &request));
        assert(result->error_number == ESTALE && !result->events);
        snag_vm_read_result_free(result);
        snag_vm_reader_close(reader);
        assert(snag_unlink_at(source.dir_fd, "events.jsonl", false) == 0);
        assert(snag_rename_at(source.dir_fd, "events.before", source.dir_fd, "events.jsonl") == 0);
    }
    free(bytes);
    /* A queued owner watermark may lag a complete snapshot. Preserve the
     * already displayed prefix until certification catches up. */
    struct snag_vm_reader *reader = snag_vm_reader_open(store, NULL, error, sizeof(error));
    assert(reader);
    assert(snag_session_commit(&source, "effort_changed", json_pack("{s:s,s:s}",
        "old_effort", "high", "new_effort", "low"), NULL, error, sizeof(error)) == 0);
    request.trusted_tail = false;
    request.tail_only = true;
    struct snag_vm_read_result *result = await_page(reader,
        snag_vm_reader_request(reader, &request));
    assert(!result->error_number && result->best_effort && result->tail.offset == source.log_end);
    request.previous = result->tail;
    snag_vm_read_result_free(result);
    request.trusted_tail = true;
    result = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(!result->error_number && result->best_effort && result->unchanged);
    snag_vm_read_result_free(result);
    request.tail = request.previous;
    result = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(!result->error_number && !result->best_effort && result->unchanged);
    snag_vm_read_result_free(result);
    /* A new committed bound must be compared with the displayed bound, not
     * with itself; otherwise if_changed would drop every owner update. */
    assert(snag_session_commit(&source, "effort_changed", json_pack("{s:s,s:s}",
        "old_effort", "low", "new_effort", "high"), NULL, error, sizeof(error)) == 0);
    request.tail.offset = source.log_end;
    request.tail.next_seq = source.next_seq;
    memcpy(request.tail.prev_sha256, source.prev_sha256, sizeof(request.tail.prev_sha256));
    request.tail_only = false;
    request.if_changed = true;
    request.reverse = false;
    request.cursor = request.previous;
    assert(write(source.log_fd, "{unfinished", 11u) == 11);
    result = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(!result->error_number && !result->unchanged && !result->best_effort &&
        !result->incomplete && json_array_size(result->events) == 1u);
    snag_vm_read_result_free(result);
    request.trusted_tail = false;
    result = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(!result->error_number && result->best_effort && result->incomplete);
    snag_vm_read_result_free(result);
    assert(snag_truncate(source.log_fd, source.log_end) == 0);
    snag_vm_reader_close(reader);
    snag_session_close(&source);
}

static void
owner_state_test(void)
{
#ifndef _WIN32
    for (unsigned int variant = 0u; variant < 9u; ++variant) {
        int sockets[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        for (unsigned int i = 0u; i < 2u; ++i)
            assert(fcntl(sockets[i], F_SETFL, O_NONBLOCK) == 0);
        struct snag_vm_connection *connection =
            snag_vm_connection_new("0123456789abcdef0123456789abcdef");
        assert(connection);
        assert(snag_vm_draft_replace(connection, 0u, 0u, "retained draft", 14u) == 0);
        snag_view_channel_init(&connection->channel, sockets[0]);
        connection->hello = true;
        char hash[SNAG_SHA256_HEX_LEN + 1u];
        memset(hash, 'a', sizeof(hash) - 1u);
        hash[sizeof(hash) - 1u] = 0;
        connection->state = json_pack("{s:i,s:i,s:s,s:i,s:b,s:s,s:s,s:s}",
            "seq", 10, "end", 1000, "sha256", hash, "schema", 4,
            "active", 0, "provider", "provider", "model", "model", "effort", "high");
        json_t *state = json_deep_copy(connection->state);
        assert(state);
        if (variant == 1u) {
            assert(json_object_set_new(state, "seq", json_integer(11)) == 0);
            assert(json_object_set_new(state, "end", json_integer(1100)) == 0);
        } else if (variant == 2u)
            assert(json_object_set_new(state, "sha256", json_string("bad")) == 0);
        else if (variant == 3u)
            assert(json_object_set_new(state, "end", json_integer(1100)) == 0);
        else if (variant == 4u) {
            assert(json_object_set_new(state, "seq", json_integer(9)) == 0);
            assert(json_object_set_new(state, "end", json_integer(900)) == 0);
        } else if (variant == 5u) {
            hash[0] = 'b';
            assert(json_object_set_new(state, "sha256", json_string(hash)) == 0);
        } else if (variant == 6u)
            assert(json_object_set_new(state, "schema", json_integer(5)) == 0);
        else if (variant == 7u)
            assert(json_object_set_new(state, "active", json_integer(1)) == 0);
        else if (variant == 8u)
            assert(json_object_set_new(state, "seq", json_integer(-1)) == 0);
        struct snag_view_channel peer;
        snag_view_channel_init(&peer, sockets[1]);
        json_t *message = json_pack("{s:s,s:O}", "type", "state", "state", state);
        assert(message && snag_view_channel_send(&peer, message) == 0);
        json_decref(message);
        uint64_t deadline = snag_monotonic_ms() + 5000u;
        uint64_t revision = connection->revision;
        while (connection->revision == revision && connection->channel.fd >= 0) {
            assert(snag_monotonic_ms() < deadline);
            if (peer.output) assert(snag_view_channel_write(&peer) >= 0);
            snag_vm_connection_step(connection);
        }
        struct snag_journal_cursor tail;
        if (variant < 2u) {
            assert(connection->channel.fd >= 0 && snag_vm_connection_tail(connection, &tail));
            assert(json_equal(connection->state, state) && tail.next_seq == 11u + variant);
        } else assert(connection->channel.fd < 0 && !snag_vm_connection_tail(connection, &tail));
        assert(connection->draft.len == 14u &&
            !memcmp(connection->draft.data, "retained draft", 14u));
        json_decref(state);
        snag_view_channel_close(&peer);
        snag_vm_connections_free(connection);
    }
#endif /* _WIN32 */
}

int
main(void)
{
    projection_test();
    owner_state_test();
    char *root = snag_path_join(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
        "snajpagent-vm-reader-XXXXXX");
    char error[256];
    assert(root && mkdtemp(root));
    char *resolved = snag_realpath(root);
    assert(resolved);
    free(root);
    root = resolved;
    struct snag_store store;
    struct snag_session source;
    snag_store_init(&store);
    snag_session_init(&source);
    assert(snag_store_open(&store, root, error, sizeof(error)) == 0);
    assert(snag_session_create(&store, &source, root, "default", "test-secret-value", "high",
        error, sizeof(error)) == 0);
    source.on_checkpoint = large_context;
    assert(snag_session_checkpoint(&source, error, sizeof(error)) == 0);
    source.on_checkpoint = NULL;
    struct snag_vm_read_request request = {.trusted_tail = true};
    memcpy(request.session_id, source.id, sizeof(request.session_id));
    request.tail.offset = source.log_end;
    request.tail.next_seq = source.next_seq;
    memcpy(request.tail.prev_sha256, source.prev_sha256, sizeof(request.tail.prev_sha256));
    char secret[] = "test-secret-value";
    const char *values[] = {secret};
    struct snag_wire_secrets secrets = {.values = values, .count = 1u};
    struct snag_vm_reader *reader = snag_vm_reader_open(&store, &secrets, error, sizeof(error));
    assert(reader);
    /* The worker keeps its own redaction snapshot and request values. */
    memset(secret, 'y', strlen(secret));
    uint64_t generation = snag_vm_reader_request(reader, &request);
    assert(generation);
    struct snag_vm_read_result *result = await_page(reader, generation);
    assert(!result->error_number && !result->best_effort && !result->more);
    assert(json_array_size(result->events) == 2u && result->cursor.next_seq == source.next_seq);
    char *dump = json_dumps(result->events, JSON_COMPACT);
    assert(dump && !strstr(dump, "test-secret-value") &&
        !strstr(dump, "private_checkpoint_payload") && strlen(dump) < 4096u);
    free(dump);
    assert(result->tail.offset == source.log_end);
    snag_vm_read_result_free(result);
    /* Navigation reuses the verified boundary and returns reverse scan order. */
    request.reverse = true;
    generation = snag_vm_reader_request(reader, &request);
    result = await_page(reader, generation);
    assert(!result->error_number && result->more && json_array_size(result->events) == 1u);
    assert(json_integer_value(json_object_get(json_array_get(result->events, 0u), "seq")) == 2);
    request.before_seq = result->cursor.next_seq;
    snag_vm_read_result_free(result);
    generation = snag_vm_reader_request(reader, &request);
    result = await_page(reader, generation);
    assert(!result->error_number && !result->more && json_array_size(result->events) == 1u);
    snag_vm_read_result_free(result);
    /* A queued or in-progress old page must never surface after newer work. */
    request.before_seq = 0u;
    for (unsigned int i = 0u; i < 200u; ++i) {
        request.reverse = (i % 2u) != 0u;
        generation = snag_vm_reader_request(reader, &request);
        assert(generation);
    }
    result = await_page(reader, generation);
    assert(!result->error_number && result->request.reverse);
    snag_vm_read_result_free(result);
    assert(!snag_vm_reader_take(reader));
    /* Snapshot reads keep unfinished bytes and never acquire writer ownership. */
    assert(write(source.log_fd, "{unfinished", 11u) == 11);
    request.trusted_tail = false;
    request.refresh = true;
    request.reverse = false;
    generation = snag_vm_reader_request(reader, &request);
    result = await_page(reader, generation);
    assert(!result->error_number && result->best_effort && result->incomplete &&
        result->tail.offset == source.log_end);
    assert(snag_seek(source.log_fd, 0, SEEK_END) == source.log_end + 11);
    snag_vm_read_result_free(result);
    assert(snag_truncate(source.log_fd, source.log_end) == 0);
    /* No reducer replay or ownership transfer is needed to read a live owner. */
    assert(snag_session_commit(&source, "effort_changed", json_pack("{s:s,s:s}",
        "old_effort", "high", "new_effort", "low"), NULL, error, sizeof(error)) == 0);
    generation = snag_vm_reader_request(reader, &request);
    result = await_page(reader, generation);
    assert(!result->error_number && !result->incomplete && result->more);
    request.cursor = result->cursor;
    request.refresh = false;
    snag_vm_read_result_free(result);
    generation = snag_vm_reader_request(reader, &request);
    result = await_page(reader, generation);
    assert(!result->error_number && !result->more && result->cursor.next_seq == source.next_seq);
    snag_vm_read_result_free(result);
    /* Metadata polling leaves the existing projection untouched when idle. */
    struct snag_vm_read_request poll = request;
    poll.refresh = poll.if_changed = poll.project = true;
    poll.previous.offset = source.log_end;
    poll.previous.next_seq = source.next_seq;
    memcpy(poll.previous.prev_sha256, source.prev_sha256, sizeof(poll.previous.prev_sha256));
    generation = snag_vm_reader_request(reader, &poll);
    result = await_page(reader, generation);
    assert(!result->error_number && result->unchanged && !result->document && !result->events);
    snag_vm_read_result_free(result);
    poll.tail_only = true;
    --poll.previous.next_seq;
    generation = snag_vm_reader_request(reader, &poll);
    result = await_page(reader, generation);
    assert(!result->error_number && !result->unchanged && !result->document && !result->events);
    snag_vm_read_result_free(result);
    /* Catalogue reads use the same cancellable worker and immutable secrets.
     * Identifying an in-process writer avoids dropping its POSIX lock. */
    struct snag_vm_read_request catalog = {.kind = SNAG_VM_READ_SESSIONS, .stored_limit = 10u};
    memcpy(catalog.owned_session_id, source.id, sizeof(catalog.owned_session_id));
    generation = snag_vm_reader_request(reader, &catalog);
    result = await_page(reader, generation);
    assert(!result->error_number && !result->events && json_array_size(result->catalog) == 1u);
    assert(!strcmp(snag_json_string(json_array_get(result->catalog, 0u), "id"), source.id));
    dump = json_dumps(result->catalog, JSON_COMPACT);
    assert(dump && !strstr(dump, "test-secret-value"));
    free(dump);
    snag_vm_read_result_free(result);
    for (unsigned int i = 0u; i < 20u; ++i) {
        assert(snag_vm_reader_request(reader, &catalog));
        generation = snag_vm_reader_request(reader, &request);
    }
    result = await_page(reader, generation);
    assert(!result->error_number && result->events && !result->catalog);
    snag_vm_read_result_free(result);
    /* Display projection stays on the worker; no raw source payload reaches
     * the frontend, in either scan direction or through checkpoint pages. */
    struct snag_vm_read_request projected = request;
    projected.project = true;
    projected.verbosity = 4u;
    projected.columns = 80u;
    projected.cursor = (struct snag_journal_cursor){0};
    generation = snag_vm_reader_request(reader, &projected);
    result = await_page(reader, generation);
    assert(!result->error_number && result->blocks && !result->events && !result->catalog);
    dump = json_dumps(result->blocks, JSON_COMPACT);
    assert(dump && !strstr(dump, "test-secret-value") &&
        !strstr(dump, "private_checkpoint_payload"));
    free(dump);
    snag_vm_read_result_free(result);
    projected.reverse = true;
    generation = snag_vm_reader_request(reader, &projected);
    result = await_page(reader, generation);
    assert(!result->error_number && result->blocks && !result->events);
    json_int_t sequence = 0;
    for (size_t i = 0u; i < json_array_size(result->blocks); ++i) {
        const json_t *block = json_array_get(result->blocks, i);
        json_int_t current = json_integer_value(json_object_get(block, "seq"));
        assert(current >= sequence);
        sequence = current;
    }
    snag_vm_read_result_free(result);
#ifndef _WIN32
    pid_t probe = fork();
    assert(probe >= 0);
    if (!probe) {
        _exit(snag_lock_file(source.lock_fd, false) < 0 && errno == EAGAIN ? 0 : 1);
    }
    int status;
    assert(waitpid(probe, &status, 0) == probe && WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif
    /* Switching buffers retains the original descriptor and its verified tail.
     * Otherwise returning after source replacement would silently open a new file. */
    struct snag_session second;
    snag_session_init(&second);
    assert(snag_session_create(&store, &second, root, "default", "second", "high",
        error, sizeof(error)) == 0);
    struct snag_vm_read_request other = {.refresh = true};
    memcpy(other.session_id, second.id, sizeof(other.session_id));
    other.retained_sessions = json_pack("[s]", source.id);
    generation = snag_vm_reader_request(reader, &other);
    assert(json_array_set_new(other.retained_sessions, 0u, json_string("mutated")) == 0);
    json_decref(other.retained_sessions);
    other.retained_sessions = NULL;
    result = await_page(reader, generation);
    assert(!result->error_number && result->events);
    snag_vm_read_result_free(result);
    snag_session_close(&second);
    /* Replacement invalidates the old descriptor even with identical contents. */
    assert(snag_rename_at(source.dir_fd, "events.jsonl", source.dir_fd, "events.before") == 0);
    int replacement = snag_create_private_at(source.dir_fd, "events.jsonl", true);
    assert(replacement >= 0 && close(replacement) == 0);
    generation = snag_vm_reader_request(reader, &request);
    result = await_page(reader, generation);
    assert(result->error_number == ESTALE && !result->events);
    snag_vm_read_result_free(result);
    assert(snag_unlink_at(source.dir_fd, "events.jsonl", false) == 0);
    assert(snag_rename_at(source.dir_fd, "events.before", source.dir_fd, "events.jsonl") == 0);
    generation = snag_vm_reader_request(reader, &request);
    snag_vm_reader_cancel(reader);
    assert(!snag_vm_reader_take(reader));
    /* Shutdown cancels a large read and joins its worker before releasing store. */
    assert(snag_vm_reader_request(reader, &catalog) > generation);
    snag_vm_reader_close(reader);
    snag_session_close(&source);
    mode_identity_test(&store, root);
    report_test(&store, root);
    snag_store_close(&store);
    free(root);
    puts("test_vm_reader: ok");
    return 0;
}
