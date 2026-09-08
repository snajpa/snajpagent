/* SPDX-License-Identifier: GPL-2.0-only */
#include "json.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
snag_json_document_free(struct snag_json_document *document)
{
    json_decref(document->value);
    memset(document, 0, sizeof(*document));
}

int
snag_json_document_measure(struct snag_json_document *document, size_t max)
{
    document->bytes = 0u;
    document->sha256[0] = '\0';
    return snag_json_digest_bounded(document->value, max,
                                    document->sha256, &document->bytes);
}

int
snag_json_document_set(struct snag_json_document *document, json_t *value, size_t max)
{
    snag_json_document_free(document);
    document->value = value;
    if (snag_json_document_measure(document, max) == 0)
        return 0;
    snag_json_document_free(document);
    return -1;
}

static int
encode_string(struct snag_buf *out, const char *s, size_t len)
{
    static const char hex[] = "0123456789abcdef";

    if (snag_buf_putc(out, '"') < 0)
        return -1;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (!c)
            return snag_errno(EILSEQ);
        if (c >= 0x80u) {
            size_t n = snag_utf8_size(c);
            if (!n || n > len - i ||
                !snag_utf8_valid((const unsigned char *)s + i, n, true))
                return snag_errno(EILSEQ);
            if (snag_buf_append(out, s + i, n) < 0)
                return -1;
            i += n - 1u;
            continue;
        }
        if (c == '"' || c == '\\') {
            if (snag_buf_putc(out, '\\') < 0 || snag_buf_putc(out, c) < 0)
                return -1;
        } else if (c <= 0x1fu) {
            unsigned char escaped[6] = {'\\', 'u', '0', '0',
                                        (unsigned char)hex[c >> 4],
                                        (unsigned char)hex[c & 15u]};
            if (snag_buf_append(out, escaped, sizeof(escaped)) < 0)
                return -1;
        } else if (snag_buf_putc(out, c) < 0) {
            return -1;
        }
    }
    return snag_buf_putc(out, '"');
}

static int encode_value(const json_t *value, struct snag_buf *out,
                        unsigned int depth, bool allow_real);

static int
encode_object(const json_t *value, struct snag_buf *out, unsigned int depth, bool allow_real)
{
    size_t count = json_object_size(value);
    struct snag_key_ref *keys = NULL;
    void *iter;
    size_t i = 0;
    int rc = -1;

    if (count) {
        if (count > SIZE_MAX / sizeof(*keys))
            return snag_errno(EOVERFLOW);
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
            !snag_utf8_valid((const unsigned char *)keys[i].name,
                            keys[i].len, true))
            goto out;
        ++i;
        iter = json_object_iter_next((json_t *)value, iter);
    }
    if (i != count)
        goto out;
    if (count)
        qsort(keys, count, sizeof(*keys), snag_key_ref_compare);
    if (snag_buf_putc(out, '{') < 0)
        goto out;
    for (i = 0; i < count; ++i) {
        json_t *member = json_object_getn(value, keys[i].name, keys[i].len);
        if ((i && snag_buf_putc(out, ',') < 0) ||
            encode_string(out, keys[i].name, keys[i].len) < 0 ||
            snag_buf_putc(out, ':') < 0 || !member ||
            encode_value(member, out, depth + 1u, allow_real) < 0)
            goto out;
    }
    if (snag_buf_putc(out, '}') < 0)
        goto out;
    rc = 0;
out:
    free(keys);
    return rc;
}

static int
encode_array(const json_t *value, struct snag_buf *out, unsigned int depth, bool allow_real)
{
    size_t count = json_array_size(value);

    if (snag_buf_putc(out, '[') < 0)
        return -1;
    for (size_t i = 0; i < count; ++i) {
        json_t *member = json_array_get(value, i);
        if ((i && snag_buf_putc(out, ',') < 0) || !member ||
            encode_value(member, out, depth + 1u, allow_real) < 0)
            return -1;
    }
    return snag_buf_putc(out, ']');
}

static int
encode_value(const json_t *value, struct snag_buf *out, unsigned int depth, bool allow_real)
{
    char number[64];
    int n;

    if (!value || depth > 48u)
        return snag_errno(EOVERFLOW);
    switch (json_typeof(value)) {
    case JSON_OBJECT:
        return encode_object(value, out, depth, allow_real);
    case JSON_ARRAY:
        return encode_array(value, out, depth, allow_real);
    case JSON_STRING:
        return encode_string(out, json_string_value(value),
                             json_string_length(value));
    case JSON_INTEGER:
        n = snprintf(number, sizeof(number), "%lld",
                     (long long)json_integer_value(value));
        break;
    case JSON_TRUE:
        return snag_buf_append(out, "true", 4u);
    case JSON_FALSE:
        return snag_buf_append(out, "false", 5u);
    case JSON_NULL:
        return snag_buf_append(out, "null", 4u);
    case JSON_REAL:
        if (allow_real) {
            n = snprintf(number, sizeof(number), "%.17g", json_real_value(value));
            break;
        }
        /* Durable canonical values must never contain floating point. */
        /* fall through */
    default:
        return snag_errno(EINVAL);
    }
    if (n <= 0 || (size_t)n >= sizeof(number))
        return snag_errno(EOVERFLOW);
    return snag_buf_append(out, number, (size_t)n);
}

int
snag_json_canonical(const json_t *value, struct snag_buf *out)
{
    snag_buf_reset(out);
    return encode_value(value, out, 0u, false);
}

int
snag_json_diagnostic(const json_t *value, struct snag_buf *out)
{
    snag_buf_reset(out);
    return encode_value(value, out, 0u, true);
}

static int
validate_loaded(const json_t *value, unsigned int depth)
{
    void *iter;
    size_t count;

    if (!value || depth > 48u)
        return snag_errno(EOVERFLOW);
    switch (json_typeof(value)) {
    case JSON_OBJECT:
        count = 0u;
        iter = json_object_iter((json_t *)value);
        while (iter) {
            const char *key = json_object_iter_key(iter);
            size_t key_len = json_object_iter_key_len(iter);
            json_t *member;

            if (!key || !snag_utf8_valid((const unsigned char *)key,
                                         key_len, true))
                return snag_errno(EINVAL);
            member = json_object_getn(value, key, key_len);
            if (!member || validate_loaded(member, depth + 1u) < 0)
                return -1;
            ++count;
            iter = json_object_iter_next((json_t *)value, iter);
        }
        if (count != json_object_size(value))
            return snag_errno(EINVAL);
        return 0;
    case JSON_ARRAY:
        count = json_array_size(value);
        for (size_t i = 0; i < count; ++i)
            if (validate_loaded(json_array_get(value, i), depth + 1u) < 0)
                return -1;
        return 0;
    case JSON_STRING:
        if (!snag_utf8_valid((const unsigned char *)json_string_value(value),
                            json_string_length(value), true))
            return snag_errno(EINVAL);
        return 0;
    case JSON_INTEGER:
    case JSON_REAL:
    case JSON_TRUE:
    case JSON_FALSE:
    case JSON_NULL:
        return 0;
    default:
        return snag_errno(EINVAL);
    }
}

json_t *
snag_json_load_strict(const unsigned char *data, size_t len, size_t max_len,
                     char *error, size_t error_size)
{
    json_error_t jerr;
    json_t *value;

    if (!len || len > max_len || !snag_utf8_valid(data, len, true)) {
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
snag_json_load_canonical(const unsigned char *data, size_t len,
                        char *error, size_t error_size)
{
    json_t *value;

    value = snag_json_load_strict(data, len, SNAG_MAX_EVENT_LINE,
                                 error, error_size);
    if (!value)
        return NULL;
    struct snag_buf encoded = {.max = SNAG_MAX_EVENT_LINE};
    if (snag_json_canonical(value, &encoded) < 0 || encoded.len != len ||
        memcmp(encoded.data, data, len) != 0) {
        if (error_size)
            (void)snprintf(error, error_size,
                           "record is not canonical format-1 JSON");
        snag_buf_free(&encoded);
        json_decref(value);
        errno = EINVAL;
        return NULL;
    }
    snag_buf_free(&encoded);
    return value;
}

int
snag_json_digest_bounded(const json_t *value, size_t max,
                        char out[SNAG_SHA256_HEX_LEN + 1u], size_t *bytes)
{
    int rc = -1;

    struct snag_buf encoded = {.max = max};
    if (snag_json_canonical(value, &encoded) == 0) {
        if (out)
            snag_sha256_hex(encoded.data, encoded.len, out);
        if (bytes)
            *bytes = encoded.len;
        rc = 0;
    }
    snag_buf_free(&encoded);
    return rc;
}

int
snag_json_digest(const json_t *value, char out[SNAG_SHA256_HEX_LEN + 1u])
{
    return snag_json_digest_bounded(value, SNAG_MAX_EVENT_LINE, out, NULL);
}

bool
snag_json_exact_keys(const json_t *object, const char *keys)
{
    size_t count = 0u;

    if (!json_is_object(object))
        return false;
    while (*keys) {
        size_t len = strcspn(keys, " ");
        if (!len || !json_object_getn(object, keys, len))
            return false;
        ++count;
        keys += len;
        if (*keys)
            ++keys;
    }
    return json_object_size(object) == count;
}

const char *
snag_json_string(const json_t *object, const char *key)
{
    json_t *value = json_object_get(object, key);
    return json_is_string(value) ? json_string_value(value) : NULL;
}

int
snag_json_integer_u64(const json_t *object, const char *key, uint64_t *out)
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

/* Advertised/configured limits are positive; zero is internal absence only. */
int
snag_json_merge_limit(const json_t *object, const char *key, uint64_t max,
                      uint64_t *out)
{
    json_t *value;
    json_int_t integer;

    if (!object || !(value = json_object_get(object, key)) ||
        json_is_null(value))
        return 0;
    if (!json_is_integer(value) || (integer = json_integer_value(value)) <= 0 ||
        (uint64_t)integer > max ||
        (*out && *out != (uint64_t)integer))
        return -1;
    *out = (uint64_t)integer;
    return 0;
}

bool
snag_json_nullable_limit(const json_t *object, const char *key, uint64_t max,
               uint64_t *value)
{
    json_t *entry = json_object_get(object, key);
    json_int_t integer;

    *value = 0u;
    if (json_is_null(entry))
        return true;
    if (!json_is_integer(entry) || (integer = json_integer_value(entry)) <= 0 ||
        (uint64_t)integer > max)
        return false;
    *value = (uint64_t)integer;
    return true;
}

int
snag_json_optional_u64(const json_t *object, const char *key, uint64_t *out, bool *known)
{
    json_t *value = json_object_get(object, key);

    if (!value || json_is_null(value)) {
        *out = 0u;
        *known = false;
        return 0;
    }
    if (snag_json_integer_u64(object, key, out) < 0)
        return -1;
    *known = true;
    return 0;
}

int
snag_json_set_new(json_t *object, const char *key, json_t *value)
{
    if (!value)
        return snag_errno(ENOMEM);
    if (json_object_set_new(object, key, value) < 0)
        return snag_errno(ENOMEM);
    return 0;
}
