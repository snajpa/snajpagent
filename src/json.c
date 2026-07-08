/* SPDX-License-Identifier: GPL-2.0-only */
#include "json.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct key_ref {
    const char *name;
    size_t len;
};

static int
key_compare(const void *left, const void *right)
{
    const struct key_ref *a = left;
    const struct key_ref *b = right;
    size_t n = a->len < b->len ? a->len : b->len;
    int cmp = memcmp(a->name, b->name, n);

    if (cmp)
        return cmp;
    return (a->len > b->len) - (a->len < b->len);
}

static int
encode_string(struct snj_buf *out, const char *s, size_t len)
{
    static const char hex[] = "0123456789abcdef";

    if (!snj_utf8_valid((const unsigned char *)s, len, true) ||
        snj_buf_putc(out, '"') < 0)
        return -1;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            if (snj_buf_putc(out, '\\') < 0 || snj_buf_putc(out, c) < 0)
                return -1;
        } else if (c <= 0x1fu) {
            unsigned char escaped[6] = {'\\', 'u', '0', '0',
                                        (unsigned char)hex[c >> 4],
                                        (unsigned char)hex[c & 15u]};
            if (snj_buf_append(out, escaped, sizeof(escaped)) < 0)
                return -1;
        } else if (snj_buf_putc(out, c) < 0) {
            return -1;
        }
    }
    return snj_buf_putc(out, '"');
}

static int encode_value(const json_t *value, struct snj_buf *out,
                        unsigned int depth);

static int
encode_object(const json_t *value, struct snj_buf *out, unsigned int depth)
{
    size_t count = json_object_size(value);
    struct key_ref *keys = NULL;
    void *iter;
    size_t i = 0;
    int rc = -1;

    if (count) {
        if (count > SIZE_MAX / sizeof(*keys)) {
            errno = EOVERFLOW;
            return -1;
        }
        keys = calloc(count, sizeof(*keys));
        if (!keys)
            return -1;
    }
    iter = json_object_iter((json_t *)value);
    while (iter) {
        if (i >= count)
            goto out;
        keys[i].name = json_object_iter_key(iter);
        keys[i].len = json_object_iter_key_len(iter);
        if (!keys[i].name ||
            !snj_utf8_valid((const unsigned char *)keys[i].name,
                            keys[i].len, true))
            goto out;
        ++i;
        iter = json_object_iter_next((json_t *)value, iter);
    }
    if (i != count)
        goto out;
    if (count)
        qsort(keys, count, sizeof(*keys), key_compare);
    if (snj_buf_putc(out, '{') < 0)
        goto out;
    for (i = 0; i < count; ++i) {
        json_t *member = json_object_getn(value, keys[i].name, keys[i].len);
        if ((i && snj_buf_putc(out, ',') < 0) ||
            encode_string(out, keys[i].name, keys[i].len) < 0 ||
            snj_buf_putc(out, ':') < 0 || !member ||
            encode_value(member, out, depth + 1u) < 0)
            goto out;
    }
    if (snj_buf_putc(out, '}') < 0)
        goto out;
    rc = 0;
out:
    free(keys);
    return rc;
}

static int
encode_array(const json_t *value, struct snj_buf *out, unsigned int depth)
{
    size_t count = json_array_size(value);

    if (snj_buf_putc(out, '[') < 0)
        return -1;
    for (size_t i = 0; i < count; ++i) {
        json_t *member = json_array_get(value, i);
        if ((i && snj_buf_putc(out, ',') < 0) || !member ||
            encode_value(member, out, depth + 1u) < 0)
            return -1;
    }
    return snj_buf_putc(out, ']');
}

static int
encode_value(const json_t *value, struct snj_buf *out, unsigned int depth)
{
    char number[64];
    int n;

    if (!value || depth > 48u) {
        errno = EOVERFLOW;
        return -1;
    }
    switch (json_typeof(value)) {
    case JSON_OBJECT:
        return encode_object(value, out, depth);
    case JSON_ARRAY:
        return encode_array(value, out, depth);
    case JSON_STRING:
        return encode_string(out, json_string_value(value),
                             json_string_length(value));
    case JSON_INTEGER:
        n = snprintf(number, sizeof(number), "%lld",
                     (long long)json_integer_value(value));
        if (n <= 0 || (size_t)n >= sizeof(number)) {
            errno = EOVERFLOW;
            return -1;
        }
        return snj_buf_append(out, number, (size_t)n);
    case JSON_TRUE:
        return snj_buf_append(out, "true", 4u);
    case JSON_FALSE:
        return snj_buf_append(out, "false", 5u);
    case JSON_NULL:
        return snj_buf_append(out, "null", 4u);
    case JSON_REAL:
    default:
        errno = EINVAL;
        return -1;
    }
}

int
snj_json_canonical(const json_t *value, struct snj_buf *out)
{
    snj_buf_reset(out);
    return encode_value(value, out, 0u);
}

static int
validate_loaded(const json_t *value, unsigned int depth)
{
    void *iter;
    size_t count;

    if (!value || depth > 48u) {
        errno = EOVERFLOW;
        return -1;
    }
    switch (json_typeof(value)) {
    case JSON_OBJECT:
        count = 0u;
        iter = json_object_iter((json_t *)value);
        while (iter) {
            const char *key = json_object_iter_key(iter);
            size_t key_len = json_object_iter_key_len(iter);
            json_t *member;

            if (!key || !snj_utf8_valid((const unsigned char *)key,
                                         key_len, true)) {
                errno = EINVAL;
                return -1;
            }
            member = json_object_getn(value, key, key_len);
            if (!member || validate_loaded(member, depth + 1u) < 0)
                return -1;
            ++count;
            iter = json_object_iter_next((json_t *)value, iter);
        }
        if (count != json_object_size(value)) {
            errno = EINVAL;
            return -1;
        }
        return 0;
    case JSON_ARRAY:
        count = json_array_size(value);
        for (size_t i = 0; i < count; ++i)
            if (validate_loaded(json_array_get(value, i), depth + 1u) < 0)
                return -1;
        return 0;
    case JSON_STRING:
        if (!snj_utf8_valid((const unsigned char *)json_string_value(value),
                            json_string_length(value), true)) {
            errno = EINVAL;
            return -1;
        }
        return 0;
    case JSON_INTEGER:
    case JSON_REAL:
    case JSON_TRUE:
    case JSON_FALSE:
    case JSON_NULL:
        return 0;
    default:
        errno = EINVAL;
        return -1;
    }
}

json_t *
snj_json_load_strict(const unsigned char *data, size_t len, size_t max_len,
                     char *error, size_t error_size)
{
    json_error_t jerr;
    json_t *value;

    if (!len || len > max_len || !snj_utf8_valid(data, len, true)) {
        if (error_size)
            (void)snprintf(error, error_size,
                           "invalid UTF-8, NUL, or JSON input size");
        errno = EINVAL;
        return NULL;
    }
    memset(&jerr, 0, sizeof(jerr));
    value = json_loadb((const char *)data, len,
                       JSON_REJECT_DUPLICATES | JSON_DECODE_ANY, &jerr);
    if (!value) {
        if (error_size)
            (void)snprintf(error, error_size, "JSON at line %d column %d: %.120s",
                           jerr.line, jerr.column, jerr.text);
        errno = EINVAL;
        return NULL;
    }
    if (validate_loaded(value, 0u) < 0) {
        if (error_size)
            (void)snprintf(error, error_size,
                           "JSON exceeds nesting limit or contains invalid text");
        json_decref(value);
        return NULL;
    }
    return value;
}

json_t *
snj_json_load_canonical(const unsigned char *data, size_t len,
                        char *error, size_t error_size)
{
    json_t *value;
    struct snj_buf encoded;

    value = snj_json_load_strict(data, len, SNJ_MAX_EVENT_LINE,
                                 error, error_size);
    if (!value)
        return NULL;
    snj_buf_init(&encoded, SNJ_MAX_EVENT_LINE);
    if (snj_json_canonical(value, &encoded) < 0 || encoded.len != len ||
        memcmp(encoded.data, data, len) != 0) {
        if (error_size)
            (void)snprintf(error, error_size,
                           "record is not canonical format-1 JSON");
        snj_buf_free(&encoded);
        json_decref(value);
        errno = EINVAL;
        return NULL;
    }
    snj_buf_free(&encoded);
    return value;
}

int
snj_json_digest(const json_t *value, char out[SNJ_SHA256_HEX_LEN + 1u])
{
    struct snj_buf encoded;
    int rc = -1;

    snj_buf_init(&encoded, SNJ_MAX_EVENT_LINE);
    if (snj_json_canonical(value, &encoded) == 0) {
        snj_sha256_hex(encoded.data, encoded.len, out);
        rc = 0;
    }
    snj_buf_free(&encoded);
    return rc;
}

bool
snj_json_exact_keys(const json_t *object, const char *const *keys, size_t count)
{
    if (!json_is_object(object) || json_object_size(object) != count)
        return false;
    for (size_t i = 0; i < count; ++i)
        if (!json_object_get(object, keys[i]))
            return false;
    return true;
}

const char *
snj_json_string(const json_t *object, const char *key)
{
    json_t *value = json_object_get(object, key);
    return json_is_string(value) ? json_string_value(value) : NULL;
}

int
snj_json_integer_u64(const json_t *object, const char *key, uint64_t *out)
{
    json_t *value = json_object_get(object, key);
    json_int_t n;

    if (!json_is_integer(value))
        return -1;
    n = json_integer_value(value);
    if (n < 0)
        return -1;
    *out = (uint64_t)n;
    return 0;
}

int
snj_json_set_new(json_t *object, const char *key, json_t *value)
{
    if (!value) {
        errno = ENOMEM;
        return -1;
    }
    if (json_object_set_new(object, key, value) < 0) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}
