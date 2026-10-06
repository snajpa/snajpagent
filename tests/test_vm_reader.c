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

/* Deliberately bypass reducer semantics for projection fixtures (including
 * foreign/reused call IDs). Envelopes and chain hashes remain fully verified. */
static void
projection_record(struct snag_session *source, const char *type, json_t *data)
{
    json_t *event = json_pack("{s:o,s:s,s:I,s:s,s:I,s:s,s:i,s:I}", "data", data,
        "prev_sha256", source->prev_sha256, "seq", (json_int_t)source->next_seq,
        "session_id", source->id, "time_ms", (json_int_t)source->last_time_ms,
        "type", type, "v", 2, "checkpoint_offset", (json_int_t)source->checkpoint_offset);
    char digest[SNAG_SHA256_HEX_LEN + 1u];
    struct snag_buf line = {.max = SNAG_MAX_EVENT_LINE};
    assert(event && snag_json_digest(event, digest) == 0);
    assert(json_object_set_new(event, "event_sha256", json_string(digest)) == 0);
    assert(snag_json_canonical(event, &line) == 0 && snag_buf_putc(&line, '\n') == 0);
    assert(snag_write_full(source->log_fd, line.data, line.len) == 0);
    source->log_end += (int64_t)line.len;
    ++source->next_seq;
    memcpy(source->prev_sha256, digest, sizeof(digest));
    snag_buf_free(&line);
    json_decref(event);
}

static void
dependency_test(struct snag_store *store, const char *root)
{
    const char *turn = "0123456789abcdef0123456789abcdef";
    const char *foreign = "fedcba9876543210fedcba9876543210";
    const char *values[] = {"test-secret-value"};
    struct snag_wire_secrets secrets = {.values = values, .count = 1u};
    for (unsigned int variant = 0u; variant < 6u; ++variant) {
        struct snag_session source;
        snag_session_init(&source);
        char error[256] = "";
        assert(snag_session_create(store, &source, root, "default", "dependencies", "high",
            error, sizeof(error)) == 0);
        projection_record(&source, "response_completed", json_pack("{s:s,s:[{s:s,s:s,s:s,s:{}}]}",
            "turn_id", turn, "items", "kind", "tool_call", "call_id", "reused-call",
            "name", "exec_command", "arguments"));
        projection_record(&source, "response_completed", json_pack("{s:s,s:[{s:s,s:s,s:s,s:{}}]}",
            "turn_id", variant == 5u ? turn : foreign,
            "items", "kind", "tool_call", "call_id", "reused-call",
            "name", "write_stdin", "arguments"));
        int64_t begin = source.log_end;
        const char *chunks[] = {"prefix test-", "error\n", "secret-value €"};
        const unsigned int streams[] = {0u, 1u, 0u};
        size_t lengths[2] = {0u, 0u};
        for (size_t i = 0u; i < 3u; ++i) {
            unsigned int stream = streams[i];
            projection_record(&source, "process_output", json_pack("{s:s,s:s,s:i,s:I,s:s,s:s}",
                "turn_id", turn, "handle", turn, "stream", (int)stream,
                "offset", (json_int_t)lengths[stream], "encoding", "utf8", "data", chunks[i]));
            lengths[stream] += strlen(chunks[i]);
        }
        int64_t end = source.log_end;
        struct snag_vm_read_request request = {.trusted_tail = true, .project = true,
            .verbosity = 2u, .columns = 80u};
        request.cursor.offset = end;
        request.cursor.next_seq = source.next_seq;
        memcpy(request.cursor.prev_sha256, source.prev_sha256, sizeof(source.prev_sha256));
        json_t *ref = json_pack("{s:s,s:I,s:I,s:i,s:I,s:i,s:I}", "handle", turn,
            "log_start", (json_int_t)begin + (variant == 1u),
            "log_end", (json_int_t)end + (variant == 2u ? 1 : 0),
            "stdout_start", 0, "stdout_end", (json_int_t)lengths[0] + (variant == 3u),
            "stderr_start", 0, "stderr_end", (json_int_t)lengths[1]);
        projection_record(&source, "tool_finished", json_pack("{s:s,s:s,s:{s:s,s:i,s:o}}",
            "turn_id", variant == 4u ? "missing-turn" : turn, "call_id", "reused-call",
            "result", "status", "succeeded", "exit_code", 0, "output_ref", ref));
        request.tail.offset = source.log_end;
        request.tail.next_seq = source.next_seq;
        memcpy(request.tail.prev_sha256, source.prev_sha256, sizeof(source.prev_sha256));
        memcpy(request.session_id, source.id, sizeof(source.id));
        struct snag_vm_reader *reader = snag_vm_reader_open(store, &secrets, error, sizeof(error));
        assert(reader);
        struct snag_vm_read_result *result = await_page(reader,
            snag_vm_reader_request(reader, &request));
        if (variant >= 1u && variant <= 3u) {
            assert(result->error_number && !result->document);
        } else {
            if (result->error_number) (void)fprintf(stderr, "%s\n", result->error);
            assert(!result->error_number && json_array_size(result->blocks) == 1u);
            const json_t *block = json_array_get(result->blocks, 0u);
            assert(strstr(snag_json_string(block, "label"),
                variant == 4u ? "tool" : variant == 5u ? "write_stdin" : "exec_command"));
            const char *body = snag_json_string(block, "text");
            assert(strstr(body, "prefix <redacted:secret>") && strstr(body, "error") &&
                strstr(body, "€") && !strstr(body, "test-secret-value"));
            assert((json_object_get(block, "needs_call") != NULL) == (variant == 4u));
            assert(result->cursor.offset == request.tail.offset &&
                result->cursor.next_seq == request.tail.next_seq);
        }
        snag_vm_read_result_free(result);
        snag_vm_reader_close(reader);
        char prefix[9];
        memcpy(prefix, source.id, 8u);
        prefix[8] = '\0';
        assert(snag_session_delete(store, &source, prefix, NULL, error, sizeof(error)) == 0);
        snag_session_close(&source);
    }
}

static bool
grow_snapshot_source(void *opaque)
{
    int *fd = opaque;
    assert(snag_write_full(*fd, "x", 1u) == 0);
    return false;
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
    int file_fd = snag_open_read_at(source.dir_fd, name, false);
    assert(file_fd >= 0);
    calls = 0u;
    assert(!snag_vm_report_file(source.dir_fd, "/cat source", "/cat source\n", 12u,
        file_fd, cancel_report, &calls) && errno == ECANCELED);
    bool incomplete;
    error[0] = '\0';
    json_t *catalog = snag_vm_report_catalog(store, sid, NULL, NULL,
        &incomplete, error, sizeof(error));
    assert(catalog && !incomplete && json_array_size(catalog) == 0u);
    json_decref(catalog);
    int changing = snag_create_private_at(source.dir_fd, "snapshot-input", true);
    assert(changing >= 0);
    assert(!snag_vm_report_file(source.dir_fd, "/cat changed", "/cat changed\n", 13u,
        changing, grow_snapshot_source, &changing) && errno == ESTALE);
    assert(close(changing) == 0);
    json_t *snapshot = snag_vm_report_file(source.dir_fd, "/cat source", "/cat source\n", 12u,
        file_fd, NULL, NULL);
    assert(snapshot && close(file_fd) == 0);
    assert(json_integer_value(json_object_get(snapshot, "bytes")) == (json_int_t)length + 12);
    struct snag_vm_document *file_view = snag_vm_report_read(store, sid, snapshot, 80u,
        &secrets, NULL, NULL, error, sizeof(error));
    assert(file_view);
    const char *file_text = snag_json_string(snag_vm_document_block(file_view, 0u), "text");
    assert(!strncmp(file_text, "/cat source\n\\xff\\x00", 20u));
    assert(strstr(file_text, "[redacted]") && !strstr(file_text, values[0]));
    snag_vm_document_free(file_view);
    json_decref(snapshot);
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

static void
report_catalog_test(struct snag_store *store, const char *root)
{
    struct snag_session source;
    char error[256] = "";
    snag_session_init(&source);
    assert(snag_session_create(store, &source, root, "default", "catalog", "high",
        error, sizeof(error)) == 0);
    json_t *reports = json_array();
    for (unsigned int i = 0u; i < 4u; ++i) {
        json_t *report = snag_vm_report_store(source.dir_fd, "/status", "private output", 14u);
        assert(report && json_array_append_new(reports, report) == 0);
    }
    bool incomplete;
    json_t *catalog = snag_vm_report_catalog(store, source.id, NULL, NULL,
        &incomplete, error, sizeof(error));
    assert(catalog && !incomplete && json_equal(catalog, reports));
    json_decref(catalog);
    int fd = snag_open_private_append_at(source.dir_fd, ".view-reports.jsonl", false);
    assert(fd >= 0);
    snag_file_info info;
    assert(snag_fstat(fd, &info) == 0);
    int64_t original = info.st_size;
    assert(snag_write_full(fd, "{\"partial\":", 11u) == 0);
    struct snag_vm_reader *reader = snag_vm_reader_open(store, NULL, error, sizeof(error));
    assert(reader);
    json_t *known = json_array();
    struct snag_vm_read_request request = {.kind = SNAG_VM_READ_REPORTS, .known_reports = known};
    memcpy(request.session_id, source.id, sizeof(request.session_id));
    uint64_t generation = snag_vm_reader_request(reader, &request);
    assert(generation && json_array_append(known, json_array_get(reports, 0u)) == 0);
    struct snag_vm_read_result *result = await_page(reader, generation);
    assert(!result->error_number && result->incomplete && json_equal(result->catalog, reports));
    assert(!json_array_size(result->request.known_reports));
    snag_vm_read_result_free(result);
    json_decref(known);
    snag_vm_reader_close(reader);
    assert(snag_truncate(fd, original) == 0);

    /* A catalogue can outgrow one read without losing cancellation or
     * duplicating repeated immutable references. */
    char *row = json_dumps(json_array_get(reports, 0u), JSON_COMPACT);
    assert(row);
    size_t length = strlen(row);
    row[length] = '\n';
    for (size_t i = 0u; i < 3u * 65536u / length + 1u; ++i)
        assert(snag_write_full(fd, row, length + 1u) == 0);
    free(row);
    unsigned int calls = 0u;
    assert(!snag_vm_report_catalog(store, source.id, cancel_report, &calls,
        &incomplete, error, sizeof(error)) && errno == ECANCELED);
    error[0] = '\0';
    catalog = snag_vm_report_catalog(store, source.id, NULL, NULL,
        &incomplete, error, sizeof(error));
    assert(catalog && !incomplete && json_equal(catalog, reports));
    json_decref(catalog);
    assert(snag_write_full(fd, "{}\n", 3u) == 0);
    assert(!snag_vm_report_catalog(store, source.id, NULL, NULL,
        &incomplete, error, sizeof(error)) && errno == EILSEQ);
    assert(snag_truncate(fd, original) == 0 && close(fd) == 0);
#ifndef _WIN32
    assert(fchmodat(source.dir_fd, ".view-reports.jsonl", 0644, 0) == 0);
    error[0] = '\0';
    assert(!snag_vm_report_catalog(store, source.id, NULL, NULL,
        &incomplete, error, sizeof(error)));
    assert(snag_unlink_at(source.dir_fd, ".view-reports.jsonl", false) == 0);
    assert(symlinkat("events.jsonl", source.dir_fd, ".view-reports.jsonl") == 0);
    error[0] = '\0';
    assert(!snag_vm_report_catalog(store, source.id, NULL, NULL,
        &incomplete, error, sizeof(error)));
#endif
    struct snag_vm_connection *c = snag_vm_connection_new(source.id);
    assert(c);
    assert(json_array_append(c->reports, json_array_get(reports, 0u)) == 0);
    assert(json_array_append(c->reports, json_array_get(reports, 2u)) == 0);
    known = json_deep_copy(c->reports);
    assert(known && json_array_append(c->reports, json_array_get(reports, 3u)) == 0);
    catalog = json_pack("[O,O]", json_array_get(reports, 1u), json_array_get(reports, 2u));
    assert(catalog && snag_vm_reports_merge(c, catalog, known) == 0);
    assert(json_equal(c->reports, reports));
    json_t *conflict = json_deep_copy(json_array_get(catalog, 1u));
    assert(conflict && json_object_set_new(conflict, "command", json_string("changed")) == 0);
    assert(json_array_set_new(catalog, 1u, conflict) == 0);
    assert(snag_vm_reports_merge(c, catalog, known) < 0 && errno == ESTALE);
    assert(json_equal(c->reports, reports));
    json_decref(catalog);
    json_decref(known);
    json_decref(reports);
    snag_vm_connections_free(c);
    snag_session_close(&source);
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
    dependency_test(&store, root);
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
    report_catalog_test(&store, root);
    snag_store_close(&store);
    free(root);
    puts("test_vm_reader: ok");
    return 0;
}
