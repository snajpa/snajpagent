/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "vm_reader.h"
#include "fixture_store_legacy.h"
#include "fs.h"
#include "history_view.h"
#include "irc.h"
#include "json.h"
#include "session_view.h"
#include "vm_connection.h"
#include "vm_report.h"
#include "vm_source.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
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
    assert(!source->binary);
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

struct catalog_stack_test {
    struct snag_store *store;
    struct snag_session *owned;
    json_t *catalog;
    char error[256];
};

static void *
small_stack_catalog(void *opaque)
{
    struct catalog_stack_test *test = opaque;
    test->catalog = snag_store_catalog(test->store, test->owned, 10u,
        NULL, NULL, test->error, sizeof(test->error));
    return NULL;
}

static void
native_page_test(struct snag_store *store, const char *root)
{
    struct snag_session source;
    snag_session_init(&source);
    char error[256] = "";
    assert(!snag_session_create(store, &source, root, "default", "native-pages", "high",
        error, sizeof(error)) && source.binary);
    json_t *data = json_pack("{s:s,s:s}", "goal_id", "80000000000000000000000000000000",
        "prompt", "native page input");
    assert(data && !snag_session_commit(&source, "goal_started", data, NULL, error, sizeof(error)));
    /* Match the shipped musl worker stack on every test host. Native catalog
     * admission must own large provisional state and I/O buffers on the heap. */
    struct catalog_stack_test stack = {.store = store, .owned = &source};
    pthread_attr_t attributes;
    pthread_t worker;
    assert(!pthread_attr_init(&attributes));
    assert(!pthread_attr_setstacksize(&attributes, 128u * 1024u));
    assert(!pthread_create(&worker, &attributes, small_stack_catalog, &stack));
    assert(!pthread_attr_destroy(&attributes));
    assert(!pthread_join(worker, NULL));
    assert(stack.catalog && json_array_size(stack.catalog) == 1u);
    json_decref(stack.catalog);
    struct snag_vm_read_request request = {.trusted_tail = true};
    memcpy(request.session_id, source.id, sizeof(request.session_id));
    request.tail.offset = source.log_end;
    request.tail.next_seq = source.next_seq;
    memcpy(request.tail.prev_sha256, source.prev_sha256, sizeof(source.prev_sha256));
    struct snag_vm_reader *reader = snag_vm_reader_open(store, NULL, error, sizeof(error));
    assert(reader);
    struct snag_vm_read_result *result = await_page(reader,
        snag_vm_reader_request(reader, &request));
    if (result->error_number) fprintf(stderr, "native page: %s\n", result->error);
    assert(!result->error_number && json_is_array(result->events));
    bool found = false;
    for (size_t i = 0u; i < json_array_size(result->events); ++i) {
        const json_t *event = json_array_get(result->events, i);
        const char *type = snag_json_string(event, "type");
        if (type && !strcmp(type, "goal_started")) {
            assert(!strcmp(snag_json_string(json_object_get(event, "data"), "prompt"),
                "native page input"));
            found = true;
        }
    }
    assert(found);
    snag_vm_read_result_free(result);
    snag_vm_reader_close(reader);
    char prefix[9];
    memcpy(prefix, source.id, 8u);
    prefix[8] = '\0';
    assert(!snag_session_delete(store, &source, prefix, NULL, error, sizeof(error)));
    snag_session_close(&source);
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
        assert(legacy_fixture_create(store, &source, root, "default", "dependencies", "high",
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

static void
viewport_test(struct snag_store *store, const char *root)
{
    struct snag_session source;
    snag_session_init(&source);
    char error[256] = "";
    assert(legacy_fixture_create(store, &source, root, "default", "viewport", "high",
        error, sizeof(error)) == 0);
    char *padding = malloc(256u * 1024u + 1u);
    assert(padding);
    memset(padding, 'x', 256u * 1024u);
    padding[256u * 1024u] = '\0';
    for (unsigned int i = 0u; i < 80u; ++i) {
        char text[40];
        (void)snprintf(text, sizeof(text), "viewport-row-%02u", i);
        projection_record(&source, "input_received", json_pack("{s:s}", "text", text));
        projection_record(&source, "provider_request", json_pack("{s:s}", "payload", padding));
    }
    free(padding);
    struct snag_vm_read_request request = {.trusted_tail = true, .project = true,
        .reverse = true, .columns = 255u, .rows = 60u};
    memcpy(request.session_id, source.id, sizeof(request.session_id));
    request.tail.offset = source.log_end;
    request.tail.next_seq = source.next_seq;
    memcpy(request.tail.prev_sha256, source.prev_sha256, sizeof(request.tail.prev_sha256));
    struct snag_vm_reader *reader = snag_vm_reader_open(store, NULL, error, sizeof(error));
    assert(reader);
    struct snag_vm_read_result *result = await_page(reader,
        snag_vm_reader_request(reader, &request));
    assert(!result->error_number);
    assert(snag_vm_document_rows(result->document) >= 60u);
    assert(result->cursor.offset > 0 && result->cursor.offset < source.log_end);
    request.before_seq = result->cursor.next_seq;
    snag_vm_read_result_free(result);
    result = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(!result->error_number && !result->more && result->cursor.offset == 0);
    assert(snag_vm_document_rows(result->document) > 0u);
    snag_vm_read_result_free(result);
    snag_vm_reader_close(reader);
    snag_session_close(&source);
}

static struct snag_journal_cursor
public_cursor(const struct snag_session *source)
{
    struct snag_journal_cursor cursor = {.offset = source->log_end, .next_seq = source->next_seq};
    memcpy(cursor.prev_sha256, source->prev_sha256, sizeof(cursor.prev_sha256));
    return cursor;
}

static json_t *
response_item(const char *text)
{
    return json_pack("{s:s,s:s,s:s}", "kind", "assistant", "phase", "commentary", "text", text);
}

static const json_t *
response_block(const struct snag_vm_read_result *page)
{
    const json_t *found = NULL;
    for (size_t i = 0u; i < json_array_size(page->blocks); ++i) {
        const json_t *block = json_array_get(page->blocks, i);
        if (strcmp(snag_json_string(block, "kind"), "assistant")) continue;
        assert(!found);
        found = block;
    }
    return found;
}

static void
public_offset_bounds_test(struct snag_store *store, const char *root)
{
    static const char id[] = "0123456789abcdef0123456789abcdef";
    static const uint64_t offsets[] = {(uint64_t)UINT32_MAX + 10u,
        (uint64_t)INT64_MAX - 3u, (uint64_t)INT64_MAX - 2u};
    for (size_t i = 0u; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        struct snag_session source;
        snag_session_init(&source);
        char error[256] = "";
        assert(legacy_fixture_create(store, &source, root, "default", "public-offsets", "high",
            error, sizeof(error)) == 0);
        projection_record(&source, "response_started", json_pack("{s:s}", "response_id", id));
        projection_record(&source, "response_output", json_pack("{s:s,s:i,s:I,s:o}",
            "response_id", id, "index", 0, "offset", (json_int_t)offsets[i],
            "item", response_item("end")));
        struct snag_vm_read_request request = {.trusted_tail = true, .project = true,
            .columns = 80u, .cursor = public_cursor(&source)};
        projection_record(&source, "response_completed", json_pack("{s:s,s:[]}",
            "response_id", id, "items"));
        request.tail = public_cursor(&source);
        memcpy(request.session_id, source.id, sizeof(source.id));
        struct snag_vm_reader *reader = snag_vm_reader_open(store, NULL, error, sizeof(error));
        assert(reader);
        struct snag_vm_read_result *page = await_page(reader,
            snag_vm_reader_request(reader, &request));
        assert(page->error_number == (i == 2u ? EINVAL : 0));
        if (i != 2u) assert(page->blocks && page->cursor.offset == request.tail.offset);
        snag_vm_read_result_free(page);
        snag_vm_reader_close(reader);
        snag_session_close(&source);
    }
}

static void
public_dependency_test(struct snag_store *store, const char *root)
{
    static const char id[] = "0123456789abcdef0123456789abcdef";
    static const char *const terminal[] = {"response_completed", "response_failed",
        "response_interrupted", "response_output_correction", "response_failed",
        "response_completed"};
    static const char *const states[] = {"complete", "failed", "interrupted", "corrected",
        "unconfirmed", "streaming"};
    for (size_t variant = 0u; variant < 6u; ++variant) {
        struct snag_session source;
        snag_session_init(&source);
        char error[256] = "";
        assert(legacy_fixture_create(store, &source, root, "default", "public-pages", "high",
            error, sizeof(error)) == 0);
        projection_record(&source, "response_started", json_pack("{s:s}", "response_id", id));
        projection_record(&source, "response_output", json_pack("{s:s,s:i,s:i,s:o}",
            "response_id", id, "index", 0, "offset", 0, "item", response_item("old-a")));
        struct snag_journal_cursor first = public_cursor(&source);
        projection_record(&source, "response_output", json_pack("{s:s,s:i,s:i,s:o}",
            "response_id", id, "index", 0, "offset", 5, "item", response_item("bcdef")));
        struct snag_journal_cursor second = public_cursor(&source);
        json_t *items = json_array();
        assert(items);
        if (variant != 4u) assert(json_array_append_new(items, response_item("new-界-rest")) == 0);
        projection_record(&source, terminal[variant], json_pack("{s:s,s:o,s:s}",
            "response_id", id, !strcmp(terminal[variant], "response_completed") ?
                "items" : "partial_public", items, "continuation", "private-provider-payload"));
        struct snag_vm_read_request request = {.trusted_tail = true, .project = true,
            .columns = 80u, .reverse = true, .before_seq = first.next_seq};
        request.tail = variant == 5u ? second : public_cursor(&source);
        memcpy(request.session_id, source.id, sizeof(source.id));
        struct snag_vm_reader *reader = snag_vm_reader_open(store, NULL, error, sizeof(error));
        assert(reader);
        struct snag_vm_read_result *page = await_page(reader,
            snag_vm_reader_request(reader, &request));
        assert(!page->error_number);
        const json_t *block = response_block(page);
        assert(block && !strcmp(snag_json_string(block, "text"), variant < 4u ? "new-" : "old-a"));
        assert(!strcmp(snag_json_string(block, "state"), states[variant]));
        assert(page->cursor.next_seq == 1u);
        snag_vm_read_result_free(page);
        request.reverse = false;
        request.before_seq = 0u;
        request.cursor = first;
        page = await_page(reader, snag_vm_reader_request(reader, &request));
        assert(!page->error_number && page->cursor.offset == request.tail.offset);
        block = response_block(page);
        assert(block && !strcmp(snag_json_string(block, "text"),
            variant < 4u ? "界-rest" : "bcdef"));
        assert(json_integer_value(json_object_get(block, "source_begin")) ==
            (variant < 4u ? 4 : 5));
        assert(!strcmp(snag_json_string(block, "state"), states[variant]));
        char *dump = json_dumps(page->blocks, JSON_COMPACT);
        assert(dump && !strstr(dump, "private-provider-payload"));
        free(dump);
        snag_vm_read_result_free(page);
        request.cursor = second;
        page = await_page(reader, snag_vm_reader_request(reader, &request));
        assert(!page->error_number && page->cursor.offset == request.tail.offset);
        block = response_block(page);
        if (variant < 4u) {
            assert(block && !strcmp(snag_json_string(block, "text"), "st"));
            assert(json_integer_value(json_object_get(block, "source_begin")) == 10);
        } else assert(!block);
        snag_vm_read_result_free(page);
        if (variant == 5u) {
            request.tail = public_cursor(&source);
            request.reverse = true;
            request.before_seq = first.next_seq;
            page = await_page(reader, snag_vm_reader_request(reader, &request));
            assert(!page->error_number);
            block = response_block(page);
            assert(block && !strcmp(snag_json_string(block, "text"), "new-") &&
                !strcmp(snag_json_string(block, "state"), "complete"));
            snag_vm_read_result_free(page);
        }
        snag_vm_reader_close(reader);
        char prefix[9];
        memcpy(prefix, source.id, 8u);
        prefix[8] = '\0';
        assert(snag_session_delete(store, &source, prefix, NULL, error, sizeof(error)) == 0);
        snag_session_close(&source);
    }
}

static void
public_full_pages_test(struct snag_store *store, const char *root)
{
    static const char id[] = "0123456789abcdef0123456789abcdef";
    const char *values[] = {"test-secret-value"};
    struct snag_wire_secrets secrets = {.values = values, .count = 1u};
    size_t length = 1152u * 1024u;
    char *text = malloc(length + 1u);
    char *expected = malloc(4u * length);
    assert(text && expected && strlen(values[0]) == 17u);
    text[length] = '\0';
    struct snag_session source;
    snag_session_init(&source);
    char error[256] = "";
    assert(legacy_fixture_create(store, &source, root, "default", "full-public-pages", "high",
        error, sizeof(error)) == 0);
    projection_record(&source, "response_started", json_pack("{s:s}", "response_id", id));
    json_t *items = json_array();
    assert(items);
    for (size_t i = 0u; i < 4u; ++i) {
        memset(text, 'a' + (int)i, length);
        memcpy(text + 100u, values[0], 17u);
        projection_record(&source, "response_output", json_pack("{s:s,s:I,s:i,s:o}",
            "response_id", id, "index", (json_int_t)i, "offset", 0, "item", response_item(text)));
        memset(text, 'A' + (int)i, length);
        memcpy(text + 100u, values[0], 17u);
        assert(json_array_append_new(items, response_item(text)) == 0);
        memcpy(expected + i * length, text, length);
        memcpy(expected + i * length + 100u, "<redacted:secret>", 17u);
    }
    projection_record(&source, "response_completed", json_pack("{s:s,s:o}",
        "response_id", id, "items", items));
    free(text);
    struct snag_vm_read_request request = {.trusted_tail = true, .project = true, .columns = 80u};
    request.tail = public_cursor(&source);
    memcpy(request.session_id, source.id, sizeof(source.id));
    struct snag_vm_reader *reader = snag_vm_reader_open(store, &secrets, error, sizeof(error));
    assert(reader);
    for (unsigned int direction = 0u; direction < 2u; ++direction) {
        request.reverse = direction != 0u;
        request.cursor = (struct snag_journal_cursor){0};
        uint64_t covered[4] = {0};
        if (direction) for (size_t i = 0u; i < 4u; ++i) covered[i] = length;
        size_t pages = 0u;
        for (;;) {
            struct snag_vm_read_result *page = await_page(reader,
                snag_vm_reader_request(reader, &request));
            assert(!page->error_number);
            size_t count = json_array_size(page->blocks);
            for (size_t n = 0u; n < count; ++n) {
                const json_t *block = json_array_get(page->blocks, direction ? count - n - 1u : n);
                if (strcmp(snag_json_string(block, "kind"), "assistant")) continue;
                size_t ordinal = (size_t)json_integer_value(json_object_get(block, "ordinal"));
                uint64_t begin = (uint64_t)json_integer_value(
                    json_object_get(block, "source_begin"));
                uint64_t end = (uint64_t)json_integer_value(json_object_get(block, "source_end"));
                assert(ordinal < 4u && end <= length && begin < end);
                assert(covered[ordinal] == (direction ? end : begin));
                covered[ordinal] = direction ? begin : end;
                const char *body = snag_json_string(block, "text");
                assert(strlen(body) == end - begin &&
                    !memcmp(body, expected + ordinal * length + begin, end - begin));
                assert(!strcmp(snag_json_string(block, "state"), "complete"));
            }
            ++pages;
            bool more = page->more;
            request.cursor = page->cursor;
            request.before_seq = direction && more ? page->cursor.next_seq : 0u;
            snag_vm_read_result_free(page);
            if (!more) break;
        }
        assert(pages >= 2u);
        for (size_t i = 0u; i < 4u; ++i) assert(covered[i] == (direction ? 0u : length));
    }
    snag_vm_reader_close(reader);
    free(expected);
    char prefix[9];
    memcpy(prefix, source.id, 8u);
    prefix[8] = '\0';
    assert(snag_session_delete(store, &source, prefix, NULL, error, sizeof(error)) == 0);
    snag_session_close(&source);
}

static void
search_blocks_test(void)
{
    struct snag_vm_anchor start = {.heading = true}, found;
    bool wrapped;
    json_t *block = json_pack("{s:s,s:i,s:i,s:i,s:s}", "key", "public/0", "seq", 1,
        "source_begin", 0, "source_end", 21, "text", "Straße Σςσ FFI ﬃ");
    static const char *const queries[] = {"STRASSE", "σσσ", "ffi", "ﬃ", "strasse"};
    static const uint64_t offsets[] = {0u, 8u, 15u, 15u, 0u};
    for (size_t i = 0u; i < 5u; ++i) {
        struct snag_vm_search *search = snag_vm_search_open(queries[i], true, false, &start);
        assert(search && snag_vm_search_block(search, block, NULL, NULL) == 0);
        assert(snag_vm_search_result(search, &found, &wrapped) && !wrapped);
        assert(found.byte == offsets[i]);
        snag_vm_search_close(search);
    }
    struct snag_vm_search *search = snag_vm_search_open("STRASSE", false, false, &start);
    assert(search && snag_vm_search_block(search, block, NULL, NULL) == 0);
    assert(!snag_vm_search_result(search, &found, &wrapped));
    snag_vm_search_close(search);
    json_decref(block);
    block = json_pack("{s:s,s:i,s:i,s:i,s:s,s:s,s:i}", "key", "event/1/output", "seq", 1,
        "source_begin", 0, "source_end", 4, "text", "neeD", "handle", "process", "stream", 0);
    search = snag_vm_search_open("needle", true, false, &start);
    assert(search && snag_vm_search_block(search, block, NULL, NULL) == 0);
    assert(!snag_vm_search_result(search, &found, &wrapped));
    assert(json_object_set_new(block, "key", json_string("event/2/output")) == 0);
    assert(json_object_set_new(block, "seq", json_integer(2)) == 0);
    assert(json_object_set_new(block, "source_begin", json_integer(4)) == 0);
    assert(json_object_set_new(block, "source_end", json_integer(6)) == 0);
    assert(json_object_set_new(block, "text", json_string("LE")) == 0);
    assert(snag_vm_search_block(search, block, NULL, NULL) == 0);
    assert(snag_vm_search_result(search, &found, &wrapped) && !wrapped && found.byte == 0u &&
        found.seq == 1u && !strcmp(found.key, "event/1/output"));
    snag_vm_search_close(search);
    json_decref(block);
}

static void
search_history_test(struct snag_store *store, const char *root)
{
    static const char id[] = "0123456789abcdef0123456789abcdef";
    struct snag_session source;
    snag_session_init(&source);
    char error[256] = "";
    assert(legacy_fixture_create(store, &source, root, "default", "search-pages", "high",
        error, sizeof(error)) == 0);
    projection_record(&source, "response_started", json_pack("{s:s}", "response_id", id));
    json_t *items = json_array();
    assert(items);
    char *padding = malloc(1152u * 1024u + 1u);
    assert(padding);
    memset(padding, 'x', 1152u * 1024u);
    padding[1152u * 1024u] = '\0';
    for (unsigned int i = 0u; i < 3u; ++i) {
        projection_record(&source, "response_output", json_pack("{s:s,s:i,s:i,s:o}",
            "response_id", id, "index", (int)i, "offset", 0, "item", response_item(padding)));
        assert(json_array_append_new(items, response_item(padding)) == 0);
    }
    free(padding);
    size_t length = 1024u * 1024u;
    char *body = malloc(length + 32u);
    assert(body);
    memset(body, 'a', length);
    memcpy(body + length - 3u, "nee", 4u);
    int64_t boundary = source.log_end;
    uint64_t seq = source.next_seq;
    projection_record(&source, "response_output", json_pack("{s:s,s:i,s:i,s:o}",
        "response_id", id, "index", 3, "offset", 0, "item", response_item(body)));
    projection_record(&source, "response_output", json_pack("{s:s,s:i,s:I,s:o}",
        "response_id", id, "index", 3, "offset", (json_int_t)length,
        "item", response_item("dle test-secret-value")));
    strcat(body, "dle test-secret-value");
    assert(json_array_append_new(items, response_item(body)) == 0);
    projection_record(&source, "response_completed", json_pack("{s:s,s:o}",
        "response_id", id, "items", items));
    free(body);
    const char *values[] = {"test-secret-value"};
    struct snag_wire_secrets secrets = {.values = values, .count = 1u};
    struct snag_vm_reader *reader = snag_vm_reader_open(store, &secrets, error, sizeof(error));
    assert(reader);
    struct snag_vm_read_request request = {.trusted_tail = true, .project = true,
        .query = "needle", .columns = 80u};
    request.search_start.heading = true;
    request.tail = public_cursor(&source);
    memcpy(request.session_id, source.id, sizeof(source.id));
    for (unsigned int i = 0u; i < 3u; ++i) {
        struct snag_vm_read_result *result = await_page(reader,
            snag_vm_reader_request(reader, &request));
        if (result->error_number || !result->found || result->match.seq != seq ||
            result->match.byte != length - 3u || result->wrapped != (i == 1u))
            (void)fprintf(stderr, "search %u: %s, found=%d seq=%llu byte=%llu wrap=%d\n", i,
                result->error, result->found, (unsigned long long)result->match.seq,
                (unsigned long long)result->match.byte, result->wrapped);
        assert(!result->error_number && result->found && result->match.seq == seq &&
            result->match.byte == length - 3u && result->wrapped == (i == 1u));
        uint64_t bytes, events, total;
        assert(snag_vm_reader_progress(reader, result->generation, &bytes, &events, &total));
        assert(bytes == (uint64_t)request.tail.offset && total == bytes &&
            events + 1u == request.tail.next_seq);
        request.search_start = result->match;
        if (i == 1u) { ++request.search_start.byte; request.search_reverse = true; }
        snag_vm_read_result_free(result);
    }
    struct snag_vm_read_request copy_request = request;
    copy_request.query = NULL;
    copy_request.pin_tail = true;
    copy_request.selection.kind = SNAG_VM_SELECT_CHAR;
    copy_request.selection.first = request.search_start;
    copy_request.selection.first.byte = length - 3u;
    copy_request.selection.last = copy_request.selection.first;
    copy_request.selection.last.byte = length + 2u;
    struct snag_vm_read_result *copied = await_page(reader,
        snag_vm_reader_request(reader, &copy_request));
    char selected[6];
    assert(!copied->error_number && copied->copied.length == sizeof(selected));
    assert(snag_vm_register_read(&copied->copied, 0u, selected, sizeof(selected)) == 0 &&
        !memcmp(selected, "needle", sizeof(selected)));
    snag_vm_read_result_free(copied);
    copy_request.selection.kind = SNAG_VM_SELECT_LINE;
    copied = await_page(reader, snag_vm_reader_request(reader, &copy_request));
    assert(!copied->error_number && copied->copied.file && !copied->copied.text.len);
    char suffix[sizeof("dle <redacted:secret>\n") - 1u];
    assert(snag_vm_register_read(&copied->copied, copied->copied.length - sizeof(suffix),
        suffix, sizeof(suffix)) == 0 && !memcmp(suffix, "dle <redacted:secret>\n", sizeof(suffix)));
    snag_vm_read_result_free(copied);
    struct snag_vm_read_request movement = copy_request;
    movement.selection.kind = SNAG_VM_SELECT_NONE;
    movement.navigation = (struct snag_vm_navigation_request){.kind = SNAG_VM_NAV_RIGHT,
        .start = copy_request.selection.first, .count = 5u};
    copied = await_page(reader, snag_vm_reader_request(reader, &movement));
    assert(!copied->error_number && copied->found && copied->match.byte == length + 2u);
    movement.navigation.start = copied->match;
    snag_vm_read_result_free(copied);
    movement.navigation.kind = SNAG_VM_NAV_LEFT;
    copied = await_page(reader, snag_vm_reader_request(reader, &movement));
    assert(!copied->error_number && copied->found && copied->match.byte == length - 3u);
    snag_vm_read_result_free(copied);
    request.query = "test-secret-value";
    struct snag_vm_read_result *result = await_page(reader,
        snag_vm_reader_request(reader, &request));
    assert(!result->error_number && !result->found);
    snag_vm_read_result_free(result);
    projection_record(&source, "response_completed", json_pack("{s:s,s:[o]}",
        "response_id", "fedcba9876543210fedcba9876543210", "items",
        response_item("new-tail-marker")));
    request.query = "new-tail-marker";
    result = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(!result->error_number && !result->found);
    snag_vm_read_result_free(result);
    request.tail = public_cursor(&source);
    result = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(!result->error_number && result->found);
    struct snag_vm_anchor new_match = result->match;
    snag_vm_read_result_free(result);
    /* Another window observed the new tail. The selection still names its
     * original boundary and must not inherit that larger cached source. */
    struct snag_vm_read_request pinned = copy_request;
    pinned.selection.first = pinned.selection.last = new_match;
    pinned.selection.last.byte += 2u;
    struct snag_vm_read_result *old = await_page(reader,
        snag_vm_reader_request(reader, &pinned));
    assert(old->error_number == ESTALE && !old->copied.length);
    snag_vm_read_result_free(old);
    request.trusted_tail = false;
    request.refresh = true;
    result = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(!result->error_number && result->found && result->best_effort);
    snag_vm_read_result_free(result);
    request.trusted_tail = true;
    request.query = "needle";
    assert(snag_vm_reader_request(reader, &request));
    request.query = NULL;
    request.reverse = true;
    result = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(!result->error_number && result->document);
    snag_vm_read_result_free(result);
    int fd = openat(source.dir_fd, "events.jsonl", O_RDWR);
    assert(fd >= 0);
    char original;
    assert(pread(fd, &original, 1u, boundary + 5) == 1);
    assert(pwrite(fd, "!", 1u, boundary + 5) == 1);
    request.query = "absent";
    result = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(result->error_number && !result->found && strstr(result->error, "Search incomplete"));
    snag_vm_read_result_free(result);
    assert(pwrite(fd, &original, 1u, boundary + 5) == 1 && close(fd) == 0);
    snag_vm_reader_close(reader);
    char prefix[9];
    memcpy(prefix, source.id, 8u);
    prefix[8] = '\0';
    assert(snag_session_delete(store, &source, prefix, NULL, error, sizeof(error)) == 0);
    snag_session_close(&source);
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
    const json_t *file_block = snag_vm_document_block(file_view, 0u);
    assert(snag_vm_source_position(file_block, 12u, true) == 12u);
    assert(snag_vm_source_position(file_block, 16u, true) == 13u);
    assert(snag_vm_source_position(file_block, 20u, true) == 14u);
    assert(snag_vm_source_position(file_block, 65542u, false) == 65548u);
    assert(snag_vm_source_position(file_block, 65548u, true) == 65542u);
    assert(snag_vm_source_position(file_block, 65558u, true) == 65542u + strlen(values[0]));
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
    request.query = "[redacted]";
    result = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(!result->error_number && result->found);
    snag_vm_read_result_free(result);
    request.query = values[0];
    result = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(!result->error_number && !result->found);
    snag_vm_read_result_free(result);
    request.query = NULL;
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
    assert(legacy_fixture_create(store, &source, root, "default", "mode-test", "high",
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
        const char *journal = source.binary ? "journal.bin" : "events.jsonl";
    assert(snag_rename_at(source.dir_fd, journal, source.dir_fd, "events.before") == 0);
        fd = snag_create_private_at(source.dir_fd, journal, true);
        assert(fd >= 0 && snag_write_full(fd, bytes, used) == 0 && close(fd) == 0);
        request.trusted_tail = !request.trusted_tail;
        result = await_page(reader, snag_vm_reader_request(reader, &request));
        assert(result->error_number == ESTALE && !result->events);
        snag_vm_read_result_free(result);
        snag_vm_reader_close(reader);
        assert(snag_unlink_at(source.dir_fd, journal, false) == 0);
        assert(snag_rename_at(source.dir_fd, "events.before", source.dir_fd, journal) == 0);
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
    for (unsigned int variant = 0u; variant < 12u; ++variant) {
        int sockets[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        for (unsigned int i = 0u; i < 2u; ++i)
            assert(fcntl(sockets[i], F_SETFL, O_NONBLOCK) == 0);
        struct snag_vm_connection *connection =
            snag_vm_connection_new("0123456789abcdef0123456789abcdef");
        assert(connection);
        assert(snag_vm_draft_replace(connection->rollout, 0u, 0u, "retained draft", 14u) == 0);
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
        else if (variant >= 9u) {
            assert(json_object_set_new(state, "irc_activity_after", json_integer(0)) == 0);
            assert(json_object_set_new(state, "queries", json_array()) == 0);
            assert(json_object_set_new(state, "channels", json_array()) == 0);
            assert(json_object_set_new(state, "connections", json_array()) == 0);
            if (variant == 10u)
                assert(json_object_set_new(state, "irc_activity_after", json_integer(11)) == 0);
            if (variant == 11u) {
                json_t *row = json_pack("{s:i,s:{s:i,s:i,s:i,s:i}}", "seq", 10,
                    "activity", "seq", 10, "time", 1, "received", 11, "incoming", 10);
                assert(row && json_array_append_new(json_object_get(state, "queries"), row) == 0);
            }
        }
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
        if (variant < 2u || variant == 9u) {
            assert(connection->channel.fd >= 0 && snag_vm_connection_tail(connection, &tail));
            assert(json_equal(connection->state, state) &&
                tail.next_seq == (variant == 1u ? 12u : 11u));
        } else assert(connection->channel.fd < 0 && !snag_vm_connection_tail(connection, &tail));
        assert(connection->rollout->draft.len == 14u &&
            !memcmp(connection->rollout->draft.data, "retained draft", 14u));
        json_decref(state);
        snag_view_channel_close(&peer);
        snag_vm_connections_free(connection);
    }
#endif /* _WIN32 */
}

static void
query_history_test(struct snag_store *store, const char *root)
{
    struct snag_session source;
    snag_session_init(&source);
    char error[256] = "";
    assert(legacy_fixture_create(store, &source, root, "default", "query-pages", "high",
        error, sizeof(error)) == 0);
    struct snag_irc_event irc = {.routed = true, .kind = SNAG_IRC_MESSAGE,
        .timestamp_ms = 1u, .endpoint = "test:6667", .nick = "first",
        .text = "old epoch marker", .route = {
            .connection = "11111111111111111111111111111111",
            .conversation = "22222222222222222222222222222222",
            .generation = 1u, .identity = SNAG_IRC_OPERATOR, .kind = SNAG_IRC_QUERY,
            .peer = "first", .target = "operator"}};
    uint64_t first = source.next_seq;
    projection_record(&source, "irc_event_v2", snag_irc_event_data(&irc));
    json_t *route = json_pack("{s:s,s:s,s:i,s:s,s:s}",
        "connection", irc.route.connection, "conversation", irc.route.conversation,
        "generation", 1, "identity", "operator", "peer", "first");
    assert(route);
    char *padding = malloc(1024u * 1024u + 1u);
    assert(padding);
    memset(padding, 'x', 1024u * 1024u);
    padding[1024u * 1024u] = 0;
    for (unsigned int part = 0u; part < 2u; ++part) {
        for (unsigned int i = 0u; i < 5u; ++i)
            projection_record(&source, "response_output", json_pack("{s:s,s:i,s:i,s:o}",
                "response_id", "33333333333333333333333333333333", "index", (int)i,
                "offset", 0, "item", response_item(padding)));
        if (!part) {
            irc.route.generation = 2u;
            strcpy(irc.route.peer, "renamed");
            strcpy(irc.text, "new epoch marker");
            projection_record(&source, "irc_event_v2", snag_irc_event_data(&irc));
        }
    }
    free(padding);
    strcpy(irc.route.conversation, "44444444444444444444444444444444");
    strcpy(irc.text, "foreign query marker");
    projection_record(&source, "irc_event_v2", snag_irc_event_data(&irc));
    struct snag_vm_reader *reader = snag_vm_reader_open(store, NULL, error, sizeof(error));
    assert(reader);
    struct snag_vm_read_request request = {.project = true, .columns = 80u,
        .verbosity = 1u, .trusted_tail = true, .route = route};
    memcpy(request.session_id, source.id, sizeof(request.session_id));
    request.tail = public_cursor(&source);
    struct snag_vm_read_result *page = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(!page->error_number && json_array_size(page->blocks) == 1u);
    assert(!strcmp(snag_json_string(json_array_get(page->blocks, 0u), "text"), "old epoch marker"));
    snag_vm_read_result_free(page);
    request.reverse = true;
    page = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(!page->error_number && json_array_size(page->blocks) == 1u);
    assert(!strcmp(snag_json_string(json_array_get(page->blocks, 0u), "text"), "new epoch marker"));
    request.before_seq = page->cursor.next_seq;
    snag_vm_read_result_free(page);
    page = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(!page->error_number && json_array_size(page->blocks) == 1u);
    assert(json_integer_value(json_object_get(json_array_get(page->blocks, 0u), "seq")) ==
        (json_int_t)first);
    snag_vm_read_result_free(page);
    request.reverse = false;
    request.before_seq = 0u;
    request.query = "foreign query marker";
    page = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(!page->error_number && !page->found);
    snag_vm_read_result_free(page);
    request.query = "new epoch marker";
    page = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(!page->error_number && page->found && page->match.seq > first);
    snag_vm_read_result_free(page);
    snag_vm_reader_close(reader);
    json_decref(route);
    char prefix[9];
    memcpy(prefix, source.id, 8u);
    prefix[8] = 0;
    assert(snag_session_delete(store, &source, prefix, NULL, error, sizeof(error)) == 0);
    snag_session_close(&source);
}

static void
channel_history_test(struct snag_store *store, const char *root)
{
    struct snag_session source;
    snag_session_init(&source);
    char error[256] = "";
    assert(legacy_fixture_create(store, &source, root, "default", "channel-history", "high",
        error, sizeof(error)) == 0);
    struct snag_irc_event event = {.routed = true, .kind = SNAG_IRC_MESSAGE,
        .timestamp_ms = 1u, .endpoint = "test:6667", .room = "#Side", .nick = "peer",
        .text = "operator public", .route = {
            .connection = "11111111111111111111111111111111",
            .conversation = "22222222222222222222222222222222",
            .membership = "33333333333333333333333333333333",
            .generation = 1u, .identity = SNAG_IRC_OPERATOR, .kind = SNAG_IRC_CHANNEL,
            .joined = true, .target = "#Side"}};
    struct snag_irc_conversation_target target = {.kind = SNAG_IRC_CHANNEL,
        .connection = "11111111111111111111111111111111",
        .conversation = "22222222222222222222222222222222",
        .membership = "33333333333333333333333333333333",
        .generation = 1u, .identity = SNAG_IRC_OPERATOR, .room = "#side",
        .endpoint = "test:6667", .casemapping = SNAG_IRC_RFC1459};
    json_t *route = snag_view_conversation_route(&target);
    assert(route);
    struct snag_irc_conversation_target decoded;
    assert(snag_view_conversation_read(route, &decoded) == 0);
    assert(decoded.kind == SNAG_IRC_CHANNEL && !strcmp(decoded.room, target.room));
    projection_record(&source, "irc_event_v2", snag_irc_event_data(&event));
    event.route.identity = SNAG_IRC_AGENT;
    event.route.generation = 2u;
    strcpy(event.route.conversation, "44444444444444444444444444444444");
    strcpy(event.route.membership, "55555555555555555555555555555555");
    strcpy(event.text, "agent public later membership");
    projection_record(&source, "irc_event_v2", snag_irc_event_data(&event));
    event.routed = false;
    strcpy(event.text, "legacy public");
    projection_record(&source, "irc_event", snag_irc_event_data(&event));
    strcpy(event.endpoint, "other:6667");
    strcpy(event.text, "foreign legacy");
    projection_record(&source, "irc_event", snag_irc_event_data(&event));
    event.routed = true;
    strcpy(event.route.connection, "66666666666666666666666666666666");
    strcpy(event.text, "foreign connection");
    projection_record(&source, "irc_event_v2", snag_irc_event_data(&event));
    strcpy(event.route.connection, target.connection);
    strcpy(event.endpoint, target.endpoint);
    strcpy(event.room, "#other");
    strcpy(event.text, "foreign room");
    projection_record(&source, "irc_event_v2", snag_irc_event_data(&event));
    event.room[0] = 0;
    event.route.kind = SNAG_IRC_QUERY;
    event.route.membership[0] = 0;
    event.route.joined = false;
    strcpy(event.route.peer, "peer");
    strcpy(event.route.target, "agent");
    strcpy(event.text, "foreign private");
    projection_record(&source, "irc_event_v2", snag_irc_event_data(&event));
    event.kind = SNAG_IRC_NOTICE;
    event.route.kind = SNAG_IRC_CONNECTION_EVENTS;
    event.route.peer[0] = 0;
    event.route.identity = SNAG_IRC_OPERATOR;
    strcpy(event.text, "operator server notice");
    projection_record(&source, "irc_event_v2", snag_irc_event_data(&event));
    event.route.identity = SNAG_IRC_AGENT;
    event.route.generation = 3u;
    strcpy(event.text, "agent later server notice");
    projection_record(&source, "irc_event_v2", snag_irc_event_data(&event));
    strcpy(event.route.connection, "66666666666666666666666666666666");
    strcpy(event.text, "foreign server notice");
    projection_record(&source, "irc_event_v2", snag_irc_event_data(&event));
    struct snag_vm_reader *reader = snag_vm_reader_open(store, NULL, error, sizeof(error));
    assert(reader);
    struct snag_vm_read_request request = {.project = true, .columns = 80u,
        .verbosity = 1u, .trusted_tail = true, .route = route};
    memcpy(request.session_id, source.id, sizeof(request.session_id));
    request.tail = public_cursor(&source);
    struct snag_vm_read_result *page = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(!page->error_number && json_array_size(page->blocks) == 3u);
    snag_vm_read_result_free(page);
    request.query = "foreign";
    page = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(!page->error_number && !page->found);
    snag_vm_read_result_free(page);
    request.query = "legacy public";
    page = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(!page->error_number && page->found);
    snag_vm_read_result_free(page);
    assert(json_object_del(route, "membership") == 0);
    assert(snag_view_conversation_read(route, &decoded) < 0);
    json_decref(route);
    target.kind = SNAG_IRC_CONNECTION_EVENTS;
    route = snag_view_conversation_route(&target);
    assert(route && snag_view_conversation_read(route, &decoded) == 0);
    assert(decoded.kind == SNAG_IRC_CONNECTION_EVENTS && !decoded.peer[0] && !decoded.room[0]);
    assert(!strcmp(decoded.endpoint, target.endpoint));
    request.route = route;
    request.query = NULL;
    page = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(!page->error_number && json_array_size(page->blocks) == 2u);
    snag_vm_read_result_free(page);
    request.query = "foreign";
    page = await_page(reader, snag_vm_reader_request(reader, &request));
    assert(!page->error_number && !page->found);
    snag_vm_read_result_free(page);
    assert(json_object_set_new(route, "peer", json_string("ambiguous")) == 0);
    assert(snag_view_conversation_read(route, &decoded) < 0);
    json_decref(route);
    snag_vm_reader_close(reader);
    char prefix[9];
    memcpy(prefix, source.id, 8u);
    prefix[8] = 0;
    assert(snag_session_delete(store, &source, prefix, NULL, error, sizeof(error)) == 0);
    snag_session_close(&source);
}

static void
conversation_snapshot_test(void)
{
    struct snag_vm_connection *owner =
        snag_vm_connection_new("0123456789abcdef0123456789abcdef");
    json_t *route = json_pack("{s:s,s:s,s:i,s:s,s:s}",
        "connection", "11111111111111111111111111111111",
        "conversation", "22222222222222222222222222222222",
        "generation", 1, "identity", "operator", "peer", "first");
    assert(owner && route);
    struct snag_vm_buffer *first = snag_vm_buffer_get(owner, route, true);
    assert(first && first == snag_vm_buffer_get(owner, route, true));
    assert(snag_vm_buffer_writable(first));
    assert(snag_vm_draft_replace(first, 0u, 0u, "private draft", 13u) == 0);
    assert(json_object_set_new(route, "generation", json_integer(2)) == 0);
    struct snag_vm_buffer *later = snag_vm_buffer_get(owner, route, true);
    assert(later && later != first && !later->draft.len);
    assert(json_object_set_new(route, "identity", json_string("agent")) == 0);
    struct snag_vm_buffer *agent = snag_vm_buffer_get(owner, route, true);
    assert(agent && !snag_vm_buffer_writable(agent));
    assert(snag_vm_buffer_prepare(agent, agent, 1u) < 0 && errno == EACCES);
    assert(!owner->rollout->draft.len && snag_vm_connection_unsaved(owner));

    struct snag_irc_conversation_target channel = {.kind = SNAG_IRC_CHANNEL,
        .connection = "11111111111111111111111111111111",
        .conversation = "33333333333333333333333333333333",
        .membership = "44444444444444444444444444444444",
        .generation = 1u, .identity = SNAG_IRC_OPERATOR, .room = "#room",
        .endpoint = "test:6667", .casemapping = SNAG_IRC_ASCII};
    json_t *channel_route = snag_view_conversation_route(&channel);
    struct snag_vm_buffer *room = snag_vm_buffer_get(owner, channel_route, true);
    assert(room && snag_vm_buffer_writable(room) && !snag_vm_buffer_supported(room));
    owner->irc_queries = true;
    assert(snag_vm_buffer_supported(first) && !snag_vm_buffer_supported(room));
    owner->irc_channels = true;
    assert(snag_vm_buffer_supported(room));
    assert(snag_vm_draft_replace(room, 0u, 0u, "channel draft", 13u) == 0);
    owner->state = json_pack("{s:i,s:[{s:O,s:{s:i,s:i,s:i,s:i}}]}",
        "irc_activity_after", 10, "queries", "route", first->route, "activity",
        "seq", 20, "time", 100, "received", 2, "incoming", 18);
    assert(owner->state);
    struct snag_vm_activity activity;
    snag_vm_buffer_activity(first, &activity);
    assert(activity.known && !activity.exact && activity.seq == 20u && activity.received == 2u);
    assert(!snag_vm_buffer_read(first, 19u) && snag_vm_buffer_read(first, 20u));
    assert(later->read_seq == 20u && !agent->read_seq && !room->read_seq);
    assert(first->draft.len == 13u && !later->draft.len);
    snag_vm_buffer_activity(later, &activity);
    assert(activity.exact && !activity.unread);
    json_t *counter = json_object_get(json_array_get(json_object_get(owner->state, "queries"), 0u),
        "activity");
    assert(json_object_set_new(counter, "seq", json_integer(25)) == 0);
    assert(json_object_set_new(counter, "incoming", json_integer(25)) == 0);
    assert(json_object_set_new(counter, "received", json_integer(3)) == 0);
    snag_vm_buffer_activity(first, &activity);
    assert(activity.exact && activity.unread == 1u);
    assert(json_object_set_new(owner->state, "irc_activity_after", json_integer(0)) == 0);
    snag_vm_buffer_activity(first, &activity);
    assert(activity.known && !activity.exact);
    assert(snag_vm_buffer_read(first, 25u));
    assert(json_object_del(owner->state, "irc_activity_after") == 0);
    snag_vm_buffer_activity(first, &activity);
    assert(!activity.known && !snag_vm_buffer_read(first, 30u));
    json_t *saved = snag_vm_connections_json(owner);
    struct snag_vm_connection *restored = NULL;
    assert(saved && snag_vm_connections_load(saved, &restored) == 0);
    assert(restored && !restored->rollout->draft.len);
    json_t *roundtrip = snag_vm_connections_json(restored);
    assert(roundtrip && json_equal(saved, roundtrip));
    json_decref(roundtrip);
    struct snag_vm_buffer *copy = snag_vm_buffer_get(restored, first->route, false);
    assert(copy && copy->draft.len == first->draft.len &&
        !memcmp(copy->draft.data, first->draft.data, first->draft.len));
    assert(copy->read_seq == 25u && copy->read_received == 3u && !copy->read_after);
    assert(snag_vm_buffer_get(restored, later->route, false));
    assert(!snag_vm_buffer_writable(snag_vm_buffer_get(restored, agent->route, false)));
    struct snag_vm_buffer *restored_room = snag_vm_buffer_get(restored, channel_route, false);
    assert(restored_room && restored_room->draft.len == 13u &&
        !memcmp(restored_room->draft.data, "channel draft", 13u));
    json_decref(channel_route);
    snag_vm_connections_free(restored);
    restored = NULL;
    json_t *buffers = json_object_get(json_array_get(saved, 0u), "buffers");
    json_t *read = json_object_get(json_array_get(buffers, 0u), "read");
    json_t *valid_read = json_deep_copy(read);
    assert(json_object_set_new(read, "received", json_integer(INT64_MAX)) == 0);
    assert(snag_vm_connections_load(saved, &restored) < 0 && !restored);
    assert(json_object_set_new(json_array_get(buffers, 0u), "read", valid_read) == 0);
    assert(json_array_append(buffers, json_array_get(buffers, 0u)) == 0);
    assert(snag_vm_connections_load(saved, &restored) < 0 && !restored);
    snag_vm_connection_discard(owner);
    assert(!snag_vm_connection_unsaved(owner));
    json_decref(saved);
    json_decref(route);
    snag_vm_connections_free(owner);
}

static void
forwarded_snapshot_test(void)
{
    struct snag_vm_connection *source =
        snag_vm_connection_new("11111111111111111111111111111111");
    struct snag_vm_connection *target =
        snag_vm_connection_new("22222222222222222222222222222222");
    assert(source && target);
    source->next = target;
    target->bound = target->commands = true;
    strcpy(target->instance, "33333333333333333333333333333333");
    const char *text = "/query other/server/peer hello";
    assert(snag_vm_draft_replace(source->rollout, 0u, 0u, text, strlen(text)) == 0);
    assert(snag_vm_draft_replace(target->rollout, 0u, 0u, "kept", 4u) == 0);
    assert(snag_vm_buffer_prepare(target->rollout, source->rollout, 7u) == 0);
    assert(!source->rollout->draft.len && target->rollout->draft.len == 4u);
    assert(!strcmp(snag_json_string(target->rollout->pending, "text"), text));
    assert(target->rollout->request_window == 7u && target->rollout->origin);

    json_t *saved = snag_vm_connections_json(source);
    struct snag_vm_connection *restored = NULL;
    assert(saved && snag_vm_connections_load(saved, &restored) == 0);
    json_t *roundtrip = snag_vm_connections_json(restored);
    assert(roundtrip && json_equal(saved, roundtrip));
    json_decref(roundtrip);
    snag_vm_connections_free(restored);
    restored = NULL;
    json_t *buffers = json_object_get(json_array_get(saved, 1u), "buffers");
    json_t *pending = json_object_get(json_array_get(buffers, 0u), "pending");
    json_t *origin = json_object_get(pending, "origin");
    assert(json_object_set_new(origin, "session", json_string(target->session)) == 0);
    assert(snag_vm_connections_load(saved, &restored) < 0 && !restored);
    assert(json_object_set_new(origin, "session",
        json_string("44444444444444444444444444444444")) == 0);
    assert(snag_vm_connections_load(saved, &restored) < 0 && !restored);
    assert(json_object_set_new(origin, "session", json_string(source->session)) == 0);
    assert(json_object_set_new(pending, "text", json_string("not a command")) == 0);
    assert(snag_vm_connections_load(saved, &restored) < 0 && !restored);
    assert(json_object_set_new(pending, "text", json_string(text)) == 0);
    assert(json_object_set_new(origin, "route", json_string("missing")) == 0);
    assert(snag_vm_connections_load(saved, &restored) < 0 && !restored);
    json_decref(saved);

    assert(snag_vm_draft_replace(source->rollout, 0u, 0u, "new", 3u) == 0);
    assert(snag_vm_buffer_recover(target->rollout, source->rollout) < 0 && errno == EBUSY);
    assert(snag_vm_draft_replace(source->rollout, 0u, 3u, NULL, 0u) == 0);
    assert(snag_vm_buffer_recover(target->rollout, source->rollout) == 0);
    assert(!strcmp((const char *)source->rollout->draft.data, text));
    assert(!strcmp((const char *)target->rollout->draft.data, "kept"));
    assert(!target->rollout->pending && !target->rollout->origin);
    snag_vm_connections_free(source);
}

int
main(void)
{
#if defined(__GLIBC__)
    /* Exercise the worker with the static musl build's default stack size. */
    pthread_attr_t attributes;
    assert(pthread_attr_init(&attributes) == 0);
    assert(pthread_attr_setstacksize(&attributes, 128u * 1024u) == 0);
    assert(pthread_setattr_default_np(&attributes) == 0);
    assert(pthread_attr_destroy(&attributes) == 0);
#endif
    projection_test();
    owner_state_test();
    conversation_snapshot_test();
    forwarded_snapshot_test();
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
    native_page_test(&store, root);
    dependency_test(&store, root);
    viewport_test(&store, root);
    public_offset_bounds_test(&store, root);
    public_dependency_test(&store, root);
    public_full_pages_test(&store, root);
    search_blocks_test();
    search_history_test(&store, root);
    query_history_test(&store, root);
    channel_history_test(&store, root);
    assert(legacy_fixture_create(&store, &source, root, "default", "test-secret-value", "high",
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
