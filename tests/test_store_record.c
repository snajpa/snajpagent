/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_record.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct input {
    const unsigned char *data;
    size_t length;
    size_t chunk;
    bool interrupted;
};

static ssize_t
read_chunk(void *opaque, void *buffer, size_t length, int64_t offset)
{
    struct input *input = opaque;
    assert(offset >= 0 && (uint64_t)offset <= input->length);
    assert(length <= input->length - (size_t)offset);
    if (input->interrupted) {
        input->interrupted = false;
        return snag_errno(EINTR);
    }
    if (length > input->chunk) length = input->chunk;
    memcpy(buffer, input->data + offset, length);
    return (ssize_t)length;
}

static json_t *
envelope(json_t *data, bool indexed, char digest[65])
{
    const char *zero = "0000000000000000000000000000000000000000000000000000000000000000";
    json_t *event = json_pack("{s:o,s:s,s:i,s:s,s:i,s:s,s:i}", "data", data,
        "prev_sha256", zero, "seq", 17, "session_id", "0123456789abcdef0123456789abcdef",
        "time_ms", 321, "type", "session_checkpoint", "v", indexed ? 2 : 1);
    assert(event);
    if (indexed) assert(json_object_set_new(event, "checkpoint_offset", json_integer(12)) == 0);
    assert(snag_json_digest_bounded(event, SIZE_MAX, digest, NULL) == 0);
    assert(json_object_set_new(event, "event_sha256", json_string(digest)) == 0);
    return event;
}

static void
test_metadata_and_digest(void)
{
    const size_t chunks[] = {1u, 2u, 7u, 4096u, 8191u, 8192u};
    for (unsigned int indexed = 0u; indexed < 2u; ++indexed) {
        json_t *context = json_pack("{s:[i,I,I,b,b,n,s]}", "values", 0,
            (json_int_t)INT64_MIN, (json_int_t)INT64_MAX, true, false, "quote\"\\\nž😀");
        assert(context);
        char *long_key = malloc(20002u);
        assert(long_key);
        memset(long_key, 'x', 20000u);
        long_key[20000] = '\0';
        assert(json_object_set_new(context, long_key, json_string("first")) == 0);
        long_key[19999] = 'y';
        assert(json_object_set_new(context, long_key, json_string("second")) == 0);
        free(long_key);
        json_t *data = json_pack("{s:o,s:i,s:i,s:{s:s}}", "context", context,
            "format", 1, "snapshot_v", 2, "state", "text", "history remains available");
        char digest[65];
        json_t *event = envelope(data, indexed != 0u, digest);
        struct snag_buf encoded = {.max = SIZE_MAX};
        assert(snag_json_canonical(event, &encoded) == 0);
        assert(snag_buf_putc(&encoded, '\n') == 0);
        for (size_t i = 0u; i < sizeof(chunks) / sizeof(chunks[0]); ++i) {
            struct input input = {encoded.data, encoded.len, chunks[i], true};
            char streamed[65];
            json_t *record = snag_store_checkpoint_metadata(read_chunk, &input,
                0, (int64_t)encoded.len, streamed);
            assert(record && !strcmp(digest, streamed));
            json_t *metadata = json_object_get(record, "data");
            assert(json_object_size(metadata) == 3u);
            assert(json_integer_value(json_object_get(metadata, "format")) == 1);
            assert(json_integer_value(json_object_get(metadata, "snapshot_v")) == 2);
            assert(json_is_true(json_object_get(metadata, "provider_view")));
            assert(!json_object_get(metadata, "state") && !json_object_get(metadata, "context"));
            assert(!strcmp(snag_json_string(record, "event_sha256"), digest));
            assert(json_integer_value(json_object_get(record, "seq")) == 17);
            assert(json_integer_value(json_object_get(record, "v")) == (indexed ? 2 : 1));
            assert((json_object_get(record, "checkpoint_offset") != NULL) == (indexed != 0u));
            json_decref(record);
        }
        json_decref(event);
        snag_buf_free(&encoded);
    }
}

static void
test_boundaries(void)
{
    char digest[65];
    json_t *event = envelope(json_object(), true, digest);
    struct snag_buf text = {.max = SIZE_MAX};
    assert(snag_json_canonical(event, &text) == 0 && snag_buf_putc(&text, '\n') == 0);
    for (size_t end = 1u; end < text.len; ++end) {
        struct input input = {text.data, text.len, 5u, false};
        assert(!snag_store_checkpoint_metadata(read_chunk, &input, 0, (int64_t)end, digest));
    }
    for (unsigned int trial = 0u; trial < 8u; ++trial) {
        json_t *bad = json_copy(event);
        assert(bad);
        if (trial == 0u) assert(json_object_set_new(bad, "v", json_integer(1)) == 0);
        if (trial == 1u) assert(json_object_del(bad, "checkpoint_offset") == 0);
        if (trial == 2u) assert(json_object_set_new(bad, "seq", json_integer(-1)) == 0);
        if (trial == 3u) assert(json_object_set_new(bad, "time_ms", json_true()) == 0);
        if (trial == 4u) assert(json_object_set_new(bad, "data", json_array()) == 0);
        if (trial == 5u) assert(json_object_set_new(bad, "type", json_string("other")) == 0);
        if (trial == 6u) assert(json_object_set_new(bad, "event_sha256", json_string("a")) == 0);
        if (trial == 7u) assert(json_object_set_new(bad, "extra", json_null()) == 0);
        assert(snag_json_canonical(bad, &text) == 0 && snag_buf_putc(&text, '\n') == 0);
        struct input input = {text.data, text.len, 5u, false};
        assert(!snag_store_checkpoint_metadata(read_chunk, &input, 0, (int64_t)text.len, digest));
        json_decref(bad);
    }
    snag_buf_free(&text);
    json_decref(event);
}

static ssize_t
cancelled_read(void *opaque, void *buffer, size_t length, int64_t offset)
{
    if (!offset) return snag_errno(ECANCELED);
    return read_chunk(opaque, buffer, length, offset);
}

static void
test_cancelled_read(void)
{
    char digest[65];
    json_t *event = envelope(json_object(), true, digest);
    struct snag_buf text = {.max = SIZE_MAX};
    assert(snag_json_canonical(event, &text) == 0 && snag_buf_putc(&text, '\n') == 0);
    struct input input = {text.data, text.len, 5u, false};
    errno = 0;
    assert(!snag_store_checkpoint_metadata(cancelled_read, &input, 0, (int64_t)text.len, digest));
    assert(errno == ECANCELED);
    snag_buf_free(&text);
    json_decref(event);
}

static void
test_depth(void)
{
    char digest[65];
    json_t *event = envelope(json_pack("{s:i}", "a", 1), true, digest);
    struct snag_buf encoded = {.max = SIZE_MAX};
    assert(snag_json_canonical(event, &encoded) == 0 && snag_buf_terminate(&encoded) == 0);
    const char *scalar = strstr((char *)encoded.data, "\"a\":1");
    assert(scalar);
    size_t prefix = (size_t)(scalar - (char *)encoded.data) + 4u;
    for (unsigned int depth = 46u; depth <= 48u; ++depth) {
        struct snag_buf text = {.max = SIZE_MAX};
        assert(snag_buf_append(&text, encoded.data, prefix) == 0);
        for (unsigned int i = 0u; i < depth; ++i) assert(snag_buf_putc(&text, '[') == 0);
        assert(snag_buf_putc(&text, '1') == 0);
        for (unsigned int i = 0u; i < depth; ++i) assert(snag_buf_putc(&text, ']') == 0);
        assert(snag_buf_append(&text, encoded.data + prefix + 1u,
            encoded.len - prefix - 1u) == 0);
        char error[256];
        json_t *oracle = snag_json_load_canonical_bounded(text.data, text.len,
            SIZE_MAX, error, sizeof(error));
        assert(snag_buf_putc(&text, '\n') == 0);
        struct input input = {text.data, text.len, 5u, false};
        json_t *record = snag_store_checkpoint_metadata(read_chunk, &input,
            0, (int64_t)text.len, digest);
        assert((record != NULL) == (depth == 46u));
        assert((oracle != NULL) == (record != NULL));
        json_decref(record);
        json_decref(oracle);
        snag_buf_free(&text);
    }
    snag_buf_free(&encoded);
    json_decref(event);
}

static void
test_canonical_parity(void)
{
    const char *payloads[] = {
        "{}", "{\"\":null}", "{\"a\":{\"\\u0001\":1,\"\\\"\":2,\"z\":3,\"é\":4}}",
        "{\"context\":null,\"format\":false,\"snapshot_v\":\"2\"}",
        "{\"z\":-9223372036854775808}", "{\"z\":9223372036854775807}",
        "{\"a\":1,\"a\":2}", "{\"b\":1,\"a\":2}", "{\"a\":1 }",
        "{\"a\":1.0}", "{\"a\":1e1}", "{\"a\":-0}", "{\"a\":01}",
        "{\"a\":9223372036854775808}", "{\"a\":-9223372036854775809}",
        "{\"a\":\"\\n\"}", "{\"a\":\"\\u001F\"}", "{\"a\":\"\\u0000\"}",
        "{\"a\":\"\\/\"}", "{\"a\":\"\\u0061\"}", "{\"a\":\"\\ud800\"}",
        "{\"a\":\"\300\200\"}", "{\"a\":\"\355\240\200\"}",
        "{\"a\":\"\364\220\200\200\"}", "{\"a\":[true,false,null,[],{}]}"
    };
    char expected[65];
    json_t *template = envelope(json_object(), true, expected);
    struct snag_buf encoded = {.max = SIZE_MAX};
    assert(snag_json_canonical(template, &encoded) == 0 && snag_buf_terminate(&encoded) == 0);
    const char *placeholder = strstr((char *)encoded.data, "\"data\":{}");
    assert(placeholder);
    size_t prefix = (size_t)(placeholder - (char *)encoded.data) + 7u;
    for (size_t i = 0u; i < sizeof(payloads) / sizeof(payloads[0]); ++i) {
        struct snag_buf text = {.max = SIZE_MAX};
        assert(snag_buf_append(&text, encoded.data, prefix) == 0);
        assert(snag_buf_append(&text, payloads[i], strlen(payloads[i])) == 0);
        assert(snag_buf_append(&text, encoded.data + prefix + 2u,
            encoded.len - prefix - 2u) == 0);
        char error[256];
        json_t *oracle = snag_json_load_canonical_bounded(text.data, text.len,
            SIZE_MAX, error, sizeof(error));
        assert(snag_buf_putc(&text, '\n') == 0);
        struct input input = {text.data, text.len, 3u, false};
        char digest[65];
        json_t *record = snag_store_checkpoint_metadata(read_chunk, &input,
            0, (int64_t)text.len, digest);
        if ((record != NULL) != (oracle != NULL)) {
            fprintf(stderr, "payload %zu: %s\n", i, payloads[i]);
        }
        assert((record != NULL) == (oracle != NULL));
        if (oracle) {
            assert(json_object_del(oracle, "event_sha256") == 0);
            assert(snag_json_digest_bounded(oracle, SIZE_MAX, expected, NULL) == 0);
            assert(!strcmp(digest, expected));
        }
        json_decref(record);
        json_decref(oracle);
        snag_buf_free(&text);
    }
    snag_buf_free(&encoded);
    json_decref(template);
}

int
main(void)
{
    test_metadata_and_digest();
    test_canonical_parity();
    test_boundaries();
    test_cancelled_read();
    test_depth();
    puts("test_store_record: ok");
    return 0;
}
