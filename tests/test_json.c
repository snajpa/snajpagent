/* SPDX-License-Identifier: GPL-2.0-only */
#include "json.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This test executable owns calloc so canonicalization's scratch allocations
 * remain visible with both the system and bundled Jansson implementations. */
static atomic_size_t calloc_count;
static atomic_bool calloc_fails;

struct reference_threads {
    json_t *value;
    atomic_uint ready;
    atomic_bool start;
};

static void
wait_for_reference_threads(struct reference_threads *shared)
{
    atomic_fetch_add(&shared->ready, 1u);
    while (!atomic_load(&shared->start)) assert(snag_sleep_ms(1u) == 0);
}

static void *
retain_references(void *opaque)
{
    struct reference_threads *shared = opaque;
    wait_for_reference_threads(shared);
    for (size_t i = 0u; i < 100000u; ++i) json_incref(shared->value);
    return NULL;
}

static void *
release_references(void *opaque)
{
    struct reference_threads *shared = opaque;
    wait_for_reference_threads(shared);
    for (size_t i = 0u; i < 100000u; ++i) json_decref(shared->value);
    return NULL;
}

static void
test_immutable_references_cross_threads(void)
{
    struct reference_threads shared = {.value = json_string("shared immutable value")};
    pthread_t threads[4];
    assert(shared.value);
    for (unsigned int phase = 0u; phase < 2u; ++phase) {
        atomic_init(&shared.ready, 0u);
        atomic_init(&shared.start, false);
        for (size_t i = 0u; i < 4u; ++i)
            assert(pthread_create(&threads[i], NULL,
                phase ? release_references : retain_references, &shared) == 0);
        uint64_t deadline = snag_monotonic_ms() + 5000u;
        while (atomic_load(&shared.ready) != 4u && snag_monotonic_ms() < deadline)
            assert(snag_sleep_ms(1u) == 0);
        assert(atomic_load(&shared.ready) == 4u);
        atomic_store(&shared.start, true);
        for (size_t i = 0u; i < 4u; ++i) assert(pthread_join(threads[i], NULL) == 0);
        assert(shared.value->refcount == (phase ? 1u : 400001u));
    }
    assert(!strcmp(json_string_value(shared.value), "shared immutable value"));
    json_decref(shared.value);
    json_t *immortal[] = {json_true(), json_false(), json_null()};
    for (size_t i = 0u; i < 3u; ++i) {
        assert(json_incref(immortal[i]) == immortal[i]);
        json_decref(immortal[i]);
        assert(immortal[i]->refcount == (size_t)-1);
    }
}

void *
calloc(size_t count, size_t size)
{
    void *(*volatile allocate)(size_t) = malloc;
    ++calloc_count;
    if (atomic_load(&calloc_fails)) { errno = ENOMEM; return NULL; }
    if (size && count > SIZE_MAX / size) { errno = ENOMEM; return NULL; }
    void *result = allocate(count * size);
    if (result) memset(result, 0, count * size);
    return result;
}

static size_t number_formats;

static int
count_number_format(char *output, size_t size, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    ++number_formats;
    int result = vsnprintf(output, size, format, arguments);
    va_end(arguments);
    return result;
}

/* Bind the formatter counter to this source even with fortified libc macros. */
#undef snprintf
#define snprintf count_number_format
#include "../src/json.c"
#undef snprintf

static void
test_integer_encoding_uses_decimal_digits(void)
{
    static const struct {
        json_int_t value;
        const char *text;
    } cases[] = {{0, "0"}, {1, "1"}, {-1, "-1"}, {10, "10"}, {-10, "-10"},
        {INT64_MAX, "9223372036854775807"}, {INT64_MIN, "-9223372036854775808"}};
    struct snag_buf out = {.max = 128u};
    assert(snag_buf_reserve(&out, 128u) == 0);
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        json_t *value = json_integer(cases[i].value);
        assert(value);
        int (*encoders[])(const json_t *, struct snag_buf *) = {snag_json_canonical,
            snag_json_diagnostic, snag_json_append, snag_json_append_ascii};
        for (size_t j = 0u; j < sizeof(encoders) / sizeof(encoders[0]); ++j) {
            snag_buf_reset(&out);
            number_formats = 0u;
            assert(encoders[j](value, &out) == 0);
            assert(!number_formats);
            assert(out.len == strlen(cases[i].text));
            assert(!memcmp(out.data, cases[i].text, out.len));
        }
        snag_buf_reset(&out);
        assert(snag_buf_append(&out, "prefix:", 7u) == 0);
        assert(snag_json_append(value, &out) == 0);
        assert(out.len == 7u + strlen(cases[i].text));
        assert(!memcmp(out.data + 7u, cases[i].text, out.len - 7u));
        json_decref(value);
    }
    snag_buf_free(&out);
    struct snag_buf small = {.max = 1u};
    json_t *value = json_integer(-10);
    assert(value && snag_json_append(value, &small) < 0);
    assert(!small.len);
    json_decref(value);
    snag_buf_free(&small);
}

static void
test_sorted_keys_borrow_existing_storage(void)
{
    json_t *sorted = json_pack("{s:i,s:i,s:i}", "a", 1, "b", 2, "c", 3);
    json_t *unsorted = json_pack("{s:i,s:i,s:i}", "c", 3, "a", 1, "b", 2);
    struct snag_buf out = {.max = 256u};
    assert(sorted && unsorted && snag_buf_reserve(&out, 256u) == 0);
    calloc_count = 0u;
    assert(snag_json_canonical(sorted, &out) == 0);
    assert(calloc_count == 0u); /* No allocation or copied scratch-key array. */
    assert(out.len == 19u && !memcmp(out.data, "{\"a\":1,\"b\":2,\"c\":3}", out.len));
    out.len = 0u;
    assert(snag_json_canonical(unsorted, &out) == 0);
    assert(calloc_count == 0u); /* Small unordered objects sort without heap scratch. */
    assert(out.len == 19u && !memcmp(out.data, "{\"a\":1,\"b\":2,\"c\":3}", out.len));
    snag_buf_free(&out);
    json_decref(unsorted);
    json_decref(sorted);
}

static void
test_large_object_sort_preserves_allocation_failure(void)
{
    json_t *value = json_object();
    struct snag_buf out = {.max = 1024u};
    assert(value && snag_buf_reserve(&out, out.max) == 0);
    for (unsigned int i = 17u; i > 0u; --i) {
        char key[8];
        assert(snprintf(key, sizeof(key), "k%02u", i) == 3);
        assert(json_object_set_new(value, key, json_integer(i)) == 0);
    }
    calloc_count = 0u;
    atomic_store(&calloc_fails, true);
    assert(snag_json_canonical(value, &out) < 0 && errno == ENOMEM);
    atomic_store(&calloc_fails, false);
    assert(calloc_count == 1u && !out.len);
    assert(snag_json_canonical(value, &out) == 0 && calloc_count == 2u);
    const char *expected = "{\"k01\":1,\"k02\":2,\"k03\":3,\"k04\":4,\"k05\":5,\"k06\":6,"
        "\"k07\":7,\"k08\":8,\"k09\":9,\"k10\":10,\"k11\":11,\"k12\":12,\"k13\":13,"
        "\"k14\":14,\"k15\":15,\"k16\":16,\"k17\":17}";
    assert(out.len == strlen(expected) && !memcmp(out.data, expected, out.len));
    json_decref(value);
    snag_buf_free(&out);
}

static void
test_object_iteration(void)
{
    json_t *object = json_pack("{s:{s:i,s:i},s:{s:i}}", "a", "x", 1, "skip", 2, "b", "y", 3);
    assert(object);
    const char *key;
    json_t *value;
    unsigned int count = 0u;
    json_int_t sum = 0;
    json_object_foreach(object, key, value)
    {
        assert(!strcmp(key, "a") || !strcmp(key, "b"));
        const char *field;
        json_t *number;
        json_object_foreach(value, field, number)
        {
            ++count;
            if (!strcmp(field, "skip")) continue;
            sum += json_integer_value(number);
        }
    }
    assert(count == 3u && sum == 4);
    count = 0u;
    json_object_foreach(object, key, value)
    {
        ++count;
        break;
    }
    assert(count == 1u);
    json_decref(object);
    object = json_object();
    assert(object);
    json_object_foreach(object, key, value)
    {
        ++count;
    }
    json_object_foreach(NULL, key, value)
    {
        ++count;
    }
    assert(count == 1u);
    json_decref(object);
}

static void
test_strict_accepts_wire_json(void)
{
    static const unsigned char input[] = " \n {\"b\":1.5,\"a\":[true,null]} \t";
    char error[192] = {0};
    json_t *value =
        snag_json_load_strict(input, sizeof(input) - 1u, sizeof(input), error, sizeof(error));

    assert(value);
    assert(json_is_object(value));
    assert(json_is_real(json_object_get(value, "b")));
    assert(json_is_array(json_object_get(value, "a")));
    json_decref(value);
}

static void
test_arguments_resolve_duplicate_keys_last_wins(void)
{
    static const unsigned char duplicate[] = "{\"x\":1,\"x\":2}";
    char error[192] = {0};
    json_t *value = snag_json_load_arguments(
        duplicate, sizeof(duplicate) - 1u, sizeof(duplicate), error, sizeof(error));

    assert(value);
    assert(json_is_object(value));
    assert(json_integer_value(json_object_get(value, "x")) == 2);
    json_decref(value);
    /* The strict loader still refuses the same input: records must stay
     * unambiguous, only provider arguments tolerate a repeat. */
    assert(snag_json_load_strict(
               duplicate, sizeof(duplicate) - 1u, sizeof(duplicate), error, sizeof(error)) == NULL);
}

static void
test_arguments_reject_decoded_nul(void)
{
    static const unsigned char decoded_nul[] = "{\"x\":\"\\u0000\"}";
    char error[192] = {0};

    /* Provider arguments tolerate duplicate keys, not an embedded NUL that
     * would truncate a C-string tool parameter after JSON decoding. */
    assert(snag_json_load_arguments(decoded_nul, sizeof(decoded_nul) - 1u, sizeof(decoded_nul),
               error, sizeof(error)) == NULL);
    assert(error[0]);
}

static void
test_strict_rejects_ambiguous_or_invalid_input(void)
{
    static const unsigned char duplicate[] = "{\"x\":1,\"x\":2}";
    static const unsigned char decoded_nul[] = "{\"x\":\"\\u0000\"}";
    static const unsigned char invalid_utf8[] = {'{', '"', 'x', '"', ':', '"', 0xc0, '"', '}'};
    struct input {
        const unsigned char *data;
        size_t len;
        size_t max;
    } inputs[] = {{duplicate, sizeof(duplicate) - 1u, sizeof(duplicate)},
        {decoded_nul, sizeof(decoded_nul) - 1u, sizeof(decoded_nul)},
        {invalid_utf8, sizeof(invalid_utf8), sizeof(invalid_utf8)},
        {(const unsigned char *)"{}", 2u, 1u}, {(const unsigned char *)"", 0u, 1u}};

    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        char error[192] = {0};
        json_t *value = snag_json_load_strict(
            inputs[i].data, inputs[i].len, inputs[i].max, error, sizeof(error));
        assert(!value);
        assert(error[0]);
    }
}

static unsigned char *
nested_json(size_t levels, size_t *length)
{
    unsigned char *text = malloc(levels * 2u + 2u);

    assert(text);
    memset(text, '[', levels);
    text[levels] = '0';
    memset(text + levels + 1u, ']', levels);
    *length = levels * 2u + 1u;
    return text;
}

static void
test_nesting_limit(void)
{
    char error[192] = {0};
    size_t len;
    unsigned char *text = nested_json(48u, &len);
    json_t *value = snag_json_load_strict(text, len, len, error, sizeof(error));

    assert(value);
    json_decref(value);
    free(text);

    memset(error, 0, sizeof(error));
    text = nested_json(49u, &len);
    value = snag_json_load_strict(text, len, len, error, sizeof(error));
    assert(!value);
    assert(strstr(error, "nesting"));
    free(text);
}

static void
test_canonical_remains_durable_only(void)
{
    static const unsigned char canonical[] = "{\"a\":1,\"b\":2}";
    static const unsigned char whitespace[] = " {\"a\":1,\"b\":2}";
    static const unsigned char real[] = "1.5";
    char error[192] = {0};
    json_t *value;

    value = snag_json_load_canonical(canonical, sizeof(canonical) - 1u, error, sizeof(error));
    assert(value);
    json_decref(value);

    value = snag_json_load_canonical(whitespace, sizeof(whitespace) - 1u, error, sizeof(error));
    assert(!value);
    value = snag_json_load_canonical(real, sizeof(real) - 1u, error, sizeof(error));
    assert(!value);
}

static void
test_tool_argument_diagnostics(void)
{
    char error[512];
    uint64_t n;
    bool b;
    const char *text;
    json_t *args = json_pack("{s:n,s:s}", "count", "payload", "private-value");
    assert(snag_json_arg_keys(args, "count payload", "", error, sizeof(error)));
    assert(!snag_json_arg_keys(args, "count", "", error, sizeof(error)));
    assert(strstr(error, "Unexpected argument: \"payload\""));
    assert(!snag_json_arg_keys(args, "count data", "", error, sizeof(error)));
    assert(strstr(error, "data") && !strstr(error, "private-value"));
    assert(snag_json_arg_uint(args, "count", 5u, 1u, 10u, &n, error, sizeof(error)) && n == 5u);
    assert(json_object_set_new(args, "count", json_integer(11)) == 0);
    assert(!snag_json_arg_uint(args, "count", 5u, 1u, 10u, &n, error, sizeof(error)));
    assert(strstr(error, "count=11") && strstr(error, "1..10"));
    assert(json_object_set_new(args, "count", json_string("null")) == 0);
    assert(!snag_json_arg_uint(args, "count", 5u, 1u, 10u, &n, error, sizeof(error)));
    assert(strstr(error, "integer") && strstr(error, "null"));
    assert(!snag_json_arg_bool(args, "count", false, &b, error, sizeof(error)));
    assert(strstr(error, "boolean"));
    assert(!snag_json_arg_text(args, "payload", 0u, 1u, false, &text, error, sizeof(error)));
    assert(strstr(error, "payload") && !strstr(error, "private-value"));
    assert(snag_json_arg_text(args, "payload", 0u, 20u, false, &text, error, sizeof(error)));
    assert(json_object_set_new(args, "payload", json_stringn("x\0y", 3u)) == 0);
    assert(!snag_json_arg_text(args, "payload", 0u, 20u, false, &text, error, sizeof(error)));
    assert(json_object_set_new(args, "payload", json_null()) == 0);
    assert(
        snag_json_arg_text(args, "payload", 0u, 20u, true, &text, error, sizeof(error)) && !text);
    assert(!snag_json_arg_text(args, "payload", 0u, 20u, false, &text, error, sizeof(error)));
    assert(json_object_del(args, "count") == 0);
    assert(snag_json_arg_keys(args, "payload", "count", error, sizeof(error)));
    assert(snag_json_arg_uint(args, "count", 5u, 1u, 10u, &n, error, sizeof(error)) && n == 5u);
    assert(snag_json_arg_bool(args, "count", true, &b, error, sizeof(error)) && b);
    assert(snag_json_arg_text(args, "count", 0u, 20u, true, &text, error, sizeof(error)) && !text);
    assert(json_object_set_new(args, "legacy", json_null()) == 0);
    assert(!strcmp(snag_json_arg_name(args, "count", "legacy", error, sizeof(error)), "legacy"));
    assert(json_object_set_new(args, "count", json_null()) == 0);
    assert(!snag_json_arg_name(args, "count", "legacy", error, sizeof(error)));
    assert(strstr(error, "Ambiguous") && strstr(error, "count") && strstr(error, "legacy"));
    assert(!snag_json_arg_keys(args, "missing", "payload count", error, sizeof(error)));
    assert(strstr(error, "missing") && strstr(error, "legacy") && !strstr(error, "private-value"));
    json_decref(args);
}

static void
test_long_canonical_string(void)
{
    enum { RUN = 65536 };
    static const char tail[] = {'"', '\\', '\x1f', '\xe2', '\x82', '\xac'};
    static const char encoded_tail[] = "\\\"\\\\\\u001f\xe2\x82\xac\"";
    struct snag_buf out = {.max = 2u * RUN};
    char *text = malloc(RUN + sizeof(tail));
    json_t *value;

    assert(text);
    memset(text, 'x', RUN);
    memcpy(text + RUN, tail, sizeof(tail));
    value = json_stringn(text, RUN + sizeof(tail));
    assert(value);
    assert(snag_json_canonical(value, &out) == 0);
    assert(out.len == RUN + 1u + sizeof(encoded_tail) - 1u);
    assert(out.data[0] == '"');
    assert(memcmp(out.data + 1u, text, RUN) == 0);
    assert(memcmp(out.data + RUN + 1u, encoded_tail, sizeof(encoded_tail) - 1u) == 0);
    snag_buf_free(&out);
    json_decref(value);
    free(text);
}

static void
test_diagnostic_roundtrip_preserves_real_types(void)
{
    const double numbers[] = {0.0, -0.0, 1.0, -1.0, 1.25, 1e30, 1e-30};
    for (size_t i = 0u; i < sizeof(numbers) / sizeof(numbers[0]); ++i) {
        json_t *value = json_real(numbers[i]);
        struct snag_buf encoded = {.max = 256u};
        assert(value && snag_json_diagnostic(value, &encoded) == 0);
        json_t *decoded = snag_json_load_strict(encoded.data, encoded.len, encoded.max, NULL, 0u);
        assert(decoded && json_is_real(decoded) && json_equal(value, decoded));
        if (i == 1u)
            assert(encoded.len == 4u && !memcmp(encoded.data, "-0.0", 4u));
        assert(snag_json_canonical(value, &encoded) < 0 && errno == EINVAL);
        json_decref(decoded);
        snag_buf_free(&encoded);
        json_decref(value);
    }
}

static void
test_message_json_appends_in_insertion_order(void)
{
    json_t *value = json_pack("{s:i,s:f}", "z", 1, "a", 1.0);
    struct snag_buf text = {.max = 64u};
    assert(value && snag_buf_append(&text, "prefix:", 7u) == 0);
    assert(snag_json_append(value, &text) == 0);
    static const char expected[] = "prefix:{\"z\":1,\"a\":1.0}";
    assert(text.len == sizeof(expected) - 1u && !memcmp(text.data, expected, text.len));
    snag_buf_reset(&text);
    assert(snag_json_diagnostic(value, &text) == 0);
    assert(text.len == 15u && !memcmp(text.data, "{\"a\":1.0,\"z\":1}", text.len));
    text.max = text.len;
    assert(snag_json_append(value, &text) < 0 && errno == EOVERFLOW);
    snag_buf_free(&text);
    json_decref(value);
}

static void
test_ascii_json_retains_terminal_control_escaping(void)
{
    json_t *value = json_pack("{s:s}", "text", "café 🐱\033\007\302\234");
    struct snag_buf text = {.max = 256u};
    assert(value && snag_buf_append(&text, "prefix:", 7u) == 0);
    assert(snag_json_append_ascii(value, &text) == 0);
    assert(!memcmp(text.data, "prefix:", 7u));
    for (size_t i = 7u; i < text.len; ++i) assert(text.data[i] >= 0x20u && text.data[i] < 0x7fu);
    json_t *decoded = snag_json_load_strict(text.data + 7u, text.len - 7u, text.max, NULL, 0u);
    assert(decoded && json_equal(value, decoded));
    json_decref(decoded);
    text.max = text.len;
    assert(snag_json_append_ascii(value, &text) < 0 && errno == EOVERFLOW);
    snag_buf_free(&text);
    json_decref(value);
}

static void *
refuse_json_scratch(size_t size)
{
    (void)size;
    return NULL;
}

static void
test_ascii_json_uses_the_existing_output_buffer(void)
{
    char draft[8192];
    memset(draft, 'x', sizeof(draft));
    json_t *value = json_pack("{s:s%,s:[s,i,f,b,n]}", "draft", draft, sizeof(draft),
        "é", "\"\\\b\f\n\r\t\033\007é\302\234🐱\364\217\277\277", -17, 1.5, 1);
    assert(value && json_object_set_new(value, "nul", json_stringn("x\0y", 3u)) == 0);
    struct snag_buf text = {.max = 65536u};
    assert(snag_buf_reserve(&text, text.max) == 0);
    assert(snag_buf_append(&text, "prefix:", 7u) == 0);
    json_malloc_t allocate;
    json_free_t release;
    json_get_alloc_funcs(&allocate, &release);
    json_set_alloc_funcs(refuse_json_scratch, release);
    int rc = snag_json_append_ascii(value, &text);
    json_set_alloc_funcs(allocate, release);
    assert(rc == 0);
    assert(!memcmp(text.data, "prefix:", 7u));
    for (size_t i = 7u; i < text.len; ++i) assert(text.data[i] >= 0x20u && text.data[i] < 0x7fu);
    json_t *decoded = json_loadb((const char *)text.data + 7u, text.len - 7u, JSON_ALLOW_NUL, NULL);
    assert(decoded && json_equal(value, decoded));
    json_decref(decoded);
    size_t length = text.len;
    snag_buf_reset(&text);
    text.max = length;
    assert(snag_buf_append(&text, "prefix:", 7u) == 0);
    assert(snag_json_append_ascii(value, &text) == 0 && text.len == length);
    snag_buf_reset(&text);
    text.max = length - 1u;
    assert(snag_buf_append(&text, "prefix:", 7u) == 0);
    assert(snag_json_append_ascii(value, &text) < 0 && errno == EOVERFLOW);
    snag_buf_free(&text);
    json_decref(value);

    value = json_string("\b\t\n\f\r");
    text.max = 256u;
    assert(value && snag_json_append_ascii(value, &text) == 0);
    static const char short_escapes[] = "\"\\b\\t\\n\\f\\r\"";
    assert(text.len == sizeof(short_escapes) - 1u && !memcmp(text.data, short_escapes, text.len));
    snag_buf_free(&text);
    json_decref(value);

    static const char invalid[] = {'x', (char)0xc0, 'y'};
    value = json_stringn_nocheck(invalid, sizeof(invalid));
    text.max = 256u;
    assert(value && snag_json_append_ascii(value, &text) < 0 && errno == EILSEQ);
    snag_buf_free(&text);
    json_decref(value);
}

static void
test_human_inspection_preserves_values_and_appends(void)
{
    json_t *value = json_pack("{s:[s,i,b,n],s:f}", "line\nbreak", " exact\ntext ",
        42, 1, "real", 1.25);
    assert(value);
    struct snag_buf text = {.max = 4096u};
    assert(snag_buf_append(&text, "Arguments: ", 11u) == 0);
    assert(snag_json_inspect(value, &text) == 0);
    assert(snag_buf_terminate(&text) == 0);
    assert(!strcmp((char *)text.data,
        "Arguments: object (2)\n"
        "  \"line\\u000abreak\": array (4)\n"
        "    [0]: \" exact\\u000atext \"\n"
        "    [1]: 42\n"
        "    [2]: true\n"
        "    [3]: null\n"
        "  \"real\": 1.25\n"));
    snag_buf_free(&text);
    text.max = 8u;
    assert(snag_json_inspect(value, &text) < 0);
    snag_buf_free(&text);
    json_decref(value);
}

int
main(void)
{
    test_large_object_sort_preserves_allocation_failure();
    test_tool_argument_diagnostics();
    test_long_canonical_string();
    test_diagnostic_roundtrip_preserves_real_types();
    test_message_json_appends_in_insertion_order();
    test_ascii_json_retains_terminal_control_escaping();
    test_ascii_json_uses_the_existing_output_buffer();
    json_t *fields = json_pack("{s:i,s:n}", "name", 1, "name_suffix");
    assert(fields && snag_json_exact_keys(fields, "name name_suffix"));
    assert(snag_json_exact_keys(fields, "name_suffix name"));
    assert(!snag_json_exact_keys(fields, "name"));
    assert(!snag_json_exact_keys(fields, "name name_suffix extra"));
    assert(!snag_json_exact_keys(fields, "name suffix"));
    assert(!snag_json_exact_keys(fields, ""));
    assert(!snag_json_exact_keys(NULL, ""));
    assert(!snag_json_exact_keys(json_null(), ""));
    assert(json_object_del(fields, "name") == 0);
    assert(json_object_del(fields, "name_suffix") == 0);
    assert(snag_json_exact_keys(fields, ""));
    json_decref(fields);
    const char *limits[] = {"{}", "{\"n\":null}", "{\"n\":0}", "{\"n\":1}", "{\"n\":100}",
        "{\"n\":101}", "{\"n\":-1}", "{\"n\":1.0}", "{\"n\":true}", "{\"n\":\"1\"}"};
    for (size_t i = 0; i < sizeof(limits) / sizeof(limits[0]); ++i) {
        json_t *limit = json_loadb(limits[i], strlen(limits[i]), 0, NULL);
        uint64_t value = 999u;
        assert(limit);
        assert(
            snag_json_nullable_limit(limit, "n", 100u, &value) == (i == 1u || i == 3u || i == 4u));
        assert(value == (i == 3u ? 1u : i == 4u ? 100u : 0u));
        json_decref(limit);
    }
    json_t *strings = json_pack("[n,b,s,s,s,s%]", 1, "", "okay", "café", "a\0b", (size_t)3u);
    assert(strings && !snag_json_bounded_string(NULL, 5u));
    for (size_t i = 0u; i < json_array_size(strings); ++i) {
        json_t *string = json_array_get(strings, i);
        const char *text = snag_json_bounded_string(string, 5u);
        assert((text != NULL) == (i == 3u || i == 4u));
        assert(!text || text == json_string_value(string));
        assert((snag_json_bounded_string(string, 4u) != NULL) == (i == 3u));
        assert(!snag_json_bounded_string(string, 0u));
    }
    json_decref(strings);
    struct snag_json_document document = {0};
    json_t *value = json_pack("{s:i}", "a", 1);
    char expected[SNAG_SHA256_HEX_LEN + 1u];
    assert(value && snag_json_document_set(&document, json_incref(value), 7u) == 0);
    snag_sha256_hex("{\"a\":1}", 7u, expected);
    assert(document.value == value && document.bytes == 7u);
    assert(!strcmp(document.sha256, expected));
    assert(json_object_set_new(value, "a", json_integer(12)) == 0);
    assert(snag_json_document_measure(&document, 7u) < 0);
    assert(document.value == value && !document.bytes && !document.sha256[0]);
    assert(snag_json_document_measure(&document, 8u) == 0 && document.bytes == 8u);
    assert(snag_json_document_set(&document, json_string("too large"), 1u) < 0);
    assert(!document.value && !document.bytes && !document.sha256[0]);
    assert(json_integer_value(json_object_get(value, "a")) == 12);
    json_decref(value);
    snag_json_document_free(&document);
    test_integer_encoding_uses_decimal_digits();
    test_sorted_keys_borrow_existing_storage();
    test_immutable_references_cross_threads();
    test_object_iteration();
    test_strict_accepts_wire_json();
    test_arguments_resolve_duplicate_keys_last_wins();
    test_arguments_reject_decoded_nul();
    test_strict_rejects_ambiguous_or_invalid_input();
    test_nesting_limit();
    test_canonical_remains_durable_only();
    test_human_inspection_preserves_values_and_appends();
    puts("test_json: ok");
    return 0;
}
