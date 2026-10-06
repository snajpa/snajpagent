/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_reader.h"
#include "fs.h"
#include "history_view.h"
#include "json.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

int
main(void)
{
    projection_test();
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
    request.refresh = true;
    assert(snag_vm_reader_request(reader, &request) > generation);
    snag_vm_reader_close(reader);
    snag_session_close(&source);
    snag_store_close(&store);
    free(root);
    puts("test_vm_reader: ok");
    return 0;
}
