/* SPDX-License-Identifier: GPL-2.0-only */
#include "json.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
test_strict_accepts_wire_json(void)
{
    static const unsigned char input[] = " \n {\"b\":1.5,\"a\":[true,null]} \t";
    char error[192] = {0};
    json_t *value = snj_json_load_strict(input, sizeof(input) - 1u,
                                         sizeof(input), error, sizeof(error));

    assert(value);
    assert(json_is_object(value));
    assert(json_is_real(json_object_get(value, "b")));
    assert(json_is_array(json_object_get(value, "a")));
    json_decref(value);
}

static void
test_strict_rejects_ambiguous_or_invalid_input(void)
{
    static const unsigned char duplicate[] = "{\"x\":1,\"x\":2}";
    static const unsigned char decoded_nul[] = "{\"x\":\"\\u0000\"}";
    static const unsigned char invalid_utf8[] = {'{', '"', 'x', '"', ':', '"',
                                                  0xc0, '"', '}'};
    struct input {
        const unsigned char *data;
        size_t len;
        size_t max;
    } inputs[] = {
        {duplicate, sizeof(duplicate) - 1u, sizeof(duplicate)},
        {decoded_nul, sizeof(decoded_nul) - 1u, sizeof(decoded_nul)},
        {invalid_utf8, sizeof(invalid_utf8), sizeof(invalid_utf8)},
        {(const unsigned char *)"{}", 2u, 1u},
        {(const unsigned char *)"", 0u, 1u}
    };

    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        char error[192] = {0};
        json_t *value = snj_json_load_strict(inputs[i].data, inputs[i].len,
                                             inputs[i].max,
                                             error, sizeof(error));
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
    json_t *value = snj_json_load_strict(text, len, len, error, sizeof(error));

    assert(value);
    json_decref(value);
    free(text);

    memset(error, 0, sizeof(error));
    text = nested_json(49u, &len);
    value = snj_json_load_strict(text, len, len, error, sizeof(error));
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

    value = snj_json_load_canonical(canonical, sizeof(canonical) - 1u,
                                    error, sizeof(error));
    assert(value);
    json_decref(value);

    value = snj_json_load_canonical(whitespace, sizeof(whitespace) - 1u,
                                    error, sizeof(error));
    assert(!value);
    value = snj_json_load_canonical(real, sizeof(real) - 1u,
                                    error, sizeof(error));
    assert(!value);
}

int
main(void)
{
    test_strict_accepts_wire_json();
    test_strict_rejects_ambiguous_or_invalid_input();
    test_nesting_limit();
    test_canonical_remains_durable_only();
    puts("test_json: ok");
    return 0;
}
