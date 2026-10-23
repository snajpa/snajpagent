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
    return snag_json_wire_digest(document->value, max, document->sha256, &document->bytes);
}

int
snag_json_document_set(struct snag_json_document *document, json_t *value, size_t max)
{
    snag_json_document_free(document);
    document->value = value;
    if (snag_json_document_measure(document, max) == 0) return 0;
    snag_json_document_free(document);
    return -1;
}

static int
encode_string(struct snag_buf *out, const char *s, size_t len, bool ascii)
{
    static const char hex[] = "0123456789abcdef";
    static const unsigned char short_escape[32] = {
        ['\b'] = 'b', ['\t'] = 't', ['\n'] = 'n', ['\f'] = 'f', ['\r'] = 'r'};

    if (snag_buf_putc(out, '"') < 0) return -1;
    size_t start = 0u;
    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)s[i];
        if (!c && !ascii) return snag_errno(EILSEQ);
        if (c >= 0x80u) {
            uint32_t cp;
            size_t n = snag_utf8_decode((const unsigned char *)s + i, len - i, &cp);
            if (!n) return snag_errno(EILSEQ);
            if (ascii) {
                if (i > start && snag_buf_append(out, s + start, i - start) < 0) return -1;
                uint32_t units[2] = {cp, 0u};
                size_t count = 1u;
                if (cp > 0xffffu) {
                    cp -= 0x10000u;
                    units[0] = 0xd800u + (cp >> 10);
                    units[1] = 0xdc00u + (cp & 0x3ffu);
                    count = 2u;
                }
                for (size_t j = 0u; j < count; ++j) {
                    uint32_t unit = units[j];
                    unsigned char escaped[6] = {'\\', 'u',
                        (unsigned char)hex[(unit >> 12) & 15u],
                        (unsigned char)hex[(unit >> 8) & 15u],
                        (unsigned char)hex[(unit >> 4) & 15u], (unsigned char)hex[unit & 15u]};
                    if (snag_buf_append(out, escaped, sizeof(escaped)) < 0) return -1;
                }
                start = i + n;
            }
            i += n;
            continue;
        }
        if (c != '"' && c != '\\' && c > 0x1fu) {
            ++i;
            continue;
        }
        if (i > start && snag_buf_append(out, s + start, i - start) < 0) return -1;
        if (c == '"' || c == '\\') {
            if (snag_buf_putc(out, '\\') < 0 || snag_buf_putc(out, c) < 0) return -1;
        } else if (c <= 0x1fu) {
            unsigned char escaped[6] = {
                '\\', 'u', '0', '0', (unsigned char)hex[c >> 4], (unsigned char)hex[c & 15u]};
            size_t length = sizeof(escaped);
            if (ascii && short_escape[c]) {
                escaped[1] = short_escape[c];
                length = 2u;
            }
            if (snag_buf_append(out, escaped, length) < 0) return -1;
        }
        start = ++i;
    }
    if (len > start && snag_buf_append(out, s + start, len - start) < 0) return -1;
    return snag_buf_putc(out, '"');
}

enum json_encoding { ENCODE_CANONICAL, ENCODE_DIAGNOSTIC, ENCODE_MESSAGE, ENCODE_ASCII };

static int encode_value(
    const json_t *value, struct snag_buf *out, unsigned int depth, enum json_encoding encoding);

static int
encode_object(
    const json_t *value, struct snag_buf *out, unsigned int depth, enum json_encoding encoding)
{
    size_t count = json_object_size(value);
    struct snag_key_ref local_keys[16];
    struct snag_key_ref *keys = NULL;
    void *iter;
    size_t i = 0;
    int rc = -1;
    bool sorted = true;
    bool ordered = encoding != ENCODE_MESSAGE && encoding != ENCODE_ASCII;
    struct snag_key_ref previous = {0};

    iter = json_object_iter((json_t *)value);
    while (ordered && iter) {
        if (i >= count) goto out;
        struct snag_key_ref key = {.name = json_object_iter_key(iter),
            .len = json_object_iter_key_len(iter)};
        if (!key.name || !snag_utf8_valid((const unsigned char *)key.name, key.len, true))
            goto out;
        if (i && snag_key_ref_compare(&previous, &key) >= 0) sorted = false;
        previous = key;
        ++i;
        iter = json_object_iter_next((json_t *)value, iter);
    }
    if (ordered && i != count) goto out;
    /* Loaded canonical objects already have sorted insertion order. Borrow
     * their keys instead of allocating and copying an identical sort array. */
    if (!sorted) {
        if (count > SIZE_MAX / sizeof(*keys)) return snag_errno(EOVERFLOW);
        /* Small protocol objects need at most 256 bytes of sort scratch. */
        keys = count <= sizeof(local_keys) / sizeof(local_keys[0]) ? local_keys :
            calloc(count, sizeof(*keys));
        if (!keys) return -1;
        iter = json_object_iter((json_t *)value);
        for (i = 0u; i < count; ++i) {
            keys[i].name = json_object_iter_key(iter);
            keys[i].len = json_object_iter_key_len(iter);
            iter = json_object_iter_next((json_t *)value, iter);
        }
        if (keys == local_keys) {
            for (i = 1u; i < count; ++i) {
                struct snag_key_ref key = keys[i];
                size_t at = i;
                while (at && snag_key_ref_compare(keys + at - 1u, &key) > 0) {
                    keys[at] = keys[at - 1u];
                    --at;
                }
                keys[at] = key;
            }
        } else qsort(keys, count, sizeof(*keys), snag_key_ref_compare);
    }
    if (snag_buf_putc(out, '{') < 0) goto out;
    iter = json_object_iter((json_t *)value);
    for (i = 0; i < count; ++i) {
        const char *name = keys ? keys[i].name : json_object_iter_key(iter);
        size_t length = keys ? keys[i].len : json_object_iter_key_len(iter);
        json_t *member = keys ? json_object_getn(value, name, length) :
            json_object_iter_value(iter);
        if ((i && snag_buf_putc(out, ',') < 0) ||
            encode_string(out, name, length, encoding == ENCODE_ASCII) < 0 ||
            snag_buf_putc(out, ':') < 0 ||
            !member || encode_value(member, out, depth + 1u, encoding) < 0)
            goto out;
        if (!keys) iter = json_object_iter_next((json_t *)value, iter);
    }
    if (snag_buf_putc(out, '}') < 0) goto out;
    rc = 0;
out:
    if (keys != local_keys) free(keys);
    return rc;
}

static int
encode_array(
    const json_t *value, struct snag_buf *out, unsigned int depth, enum json_encoding encoding)
{
    size_t count = json_array_size(value);

    if (snag_buf_putc(out, '[') < 0) return -1;
    for (size_t i = 0; i < count; ++i) {
        json_t *member = json_array_get(value, i);
        if ((i && snag_buf_putc(out, ',') < 0) || !member ||
            encode_value(member, out, depth + 1u, encoding) < 0)
            return -1;
    }
    return snag_buf_putc(out, ']');
}

static int
encode_value(
    const json_t *value, struct snag_buf *out, unsigned int depth, enum json_encoding encoding)
{
    char number[64];
    int n;

    if (!value || depth > 48u) return snag_errno(EOVERFLOW);
    switch (json_typeof(value)) {
    case JSON_OBJECT:
        return encode_object(value, out, depth, encoding);
    case JSON_ARRAY:
        return encode_array(value, out, depth, encoding);
    case JSON_STRING:
        return encode_string(out, json_string_value(value), json_string_length(value),
            encoding == ENCODE_ASCII);
    case JSON_INTEGER: {
        json_int_t integer = json_integer_value(value);
        unsigned long long magnitude = (unsigned long long)integer;
        if (integer < 0) magnitude = 0u - magnitude;
        char *at = number + sizeof(number);
        do {
            *--at = (char)('0' + magnitude % 10u);
            magnitude /= 10u;
        } while (magnitude);
        if (integer < 0) *--at = '-';
        return snag_buf_append(out, at, (size_t)(number + sizeof(number) - at));
    }
    case JSON_TRUE:
        return snag_buf_append(out, "true", 4u);
    case JSON_FALSE:
        return snag_buf_append(out, "false", 5u);
    case JSON_NULL:
        return snag_buf_append(out, "null", 4u);
    case JSON_REAL:
        if (encoding != ENCODE_CANONICAL) {
            n = snprintf(number, sizeof(number), "%.17g", json_real_value(value));
            if (n > 0 && (size_t)n < sizeof(number) - 2u &&
                !strpbrk(number, ".eE")) {
                number[n++] = '.';
                number[n++] = '0';
            }
            break;
        }
        /* Durable canonical values must never contain floating point. */
        /* fall through */
    default:
        return snag_errno(EINVAL);
    }
    if (n <= 0 || (size_t)n >= sizeof(number)) return snag_errno(EOVERFLOW);
    return snag_buf_append(out, number, (size_t)n);
}

int
snag_json_canonical(const json_t *value, struct snag_buf *out)
{
    snag_buf_reset(out);
    return encode_value(value, out, 0u, ENCODE_CANONICAL);
}

int
snag_json_diagnostic(const json_t *value, struct snag_buf *out)
{
    snag_buf_reset(out);
    return encode_value(value, out, 0u, ENCODE_DIAGNOSTIC);
}

int
snag_json_append(const json_t *value, struct snag_buf *out)
{
    return encode_value(value, out, 0u, ENCODE_MESSAGE);
}

int
snag_json_append_ascii(const json_t *value, struct snag_buf *out)
{
    return encode_value(value, out, 0u, ENCODE_ASCII);
}

static int
inspect_value(const json_t *value, struct snag_buf *out, unsigned int depth)
{
    if (!value || depth > 48u) return snag_errno(EOVERFLOW);
    if (!json_is_object(value) && !json_is_array(value)) {
        if (encode_value(value, out, depth, ENCODE_DIAGNOSTIC) < 0) return -1;
        return snag_buf_putc(out, '\n');
    }
    bool object = json_is_object(value);
    size_t count = object ? json_object_size(value) : json_array_size(value);
    if (snag_buf_printf(out, "%s (%zu)\n", object ? "object" : "array", count) < 0) return -1;
    void *iterator = object ? json_object_iter((json_t *)value) : NULL;
    for (size_t i = 0; i < count; ++i) {
        for (unsigned int j = 0; j <= depth; ++j)
            if (snag_buf_append(out, "  ", 2u) < 0) return -1;
        json_t *child;
        if (object) {
            if (encode_string(out, json_object_iter_key(iterator),
                json_object_iter_key_len(iterator), false) < 0 ||
                snag_buf_append(out, ": ", 2u) < 0)
                return -1;
            child = json_object_iter_value(iterator);
            iterator = json_object_iter_next((json_t *)value, iterator);
        } else {
            child = json_array_get(value, i);
            if (snag_buf_printf(out, "[%zu]: ", i) < 0) return -1;
        }
        if (inspect_value(child, out, depth + 1u) < 0) return -1;
    }
    return 0;
}

int
snag_json_inspect(const json_t *value, struct snag_buf *out)
{
    return inspect_value(value, out, 0u);
}

int
snag_json_wire_digest(const json_t *value, size_t max,
    char hash[SNAG_SHA256_HEX_LEN + 1u], size_t *bytes)
{
    struct snag_buf encoded = {.max = max};
    int rc = snag_json_diagnostic(value, &encoded);
    if (!rc) {
        if (hash) snag_sha256_hex(encoded.data, encoded.len, hash);
        if (bytes) *bytes = encoded.len;
    }
    snag_buf_free(&encoded);
    return rc;
}

static int
validate_loaded(const json_t *value, unsigned int depth)
{
    void *iter;
    size_t count;

    if (!value || depth > 48u) return snag_errno(EOVERFLOW);
    switch (json_typeof(value)) {
    case JSON_OBJECT:
        count = 0u;
        iter = json_object_iter((json_t *)value);
        while (iter) {
            const char *key = json_object_iter_key(iter);
            size_t key_len = json_object_iter_key_len(iter);
            json_t *member;

            if (!key || !snag_utf8_valid((const unsigned char *)key, key_len, true))
                return snag_errno(EINVAL);
            member = json_object_getn(value, key, key_len);
            if (!member || validate_loaded(member, depth + 1u) < 0) return -1;
            ++count;
            iter = json_object_iter_next((json_t *)value, iter);
        }
        if (count != json_object_size(value)) return snag_errno(EINVAL);
        return 0;
    case JSON_ARRAY:
        count = json_array_size(value);
        for (size_t i = 0; i < count; ++i)
            if (validate_loaded(json_array_get(value, i), depth + 1u) < 0) return -1;
        return 0;
    case JSON_STRING:
        if (!snag_utf8_valid(
                (const unsigned char *)json_string_value(value), json_string_length(value), true))
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

static json_t *
load_json(const unsigned char *data, size_t len, size_t max_len, int flags, char *error,
    size_t error_size)
{
    json_error_t jerr;
    json_t *value;

    if (!len || len > max_len || !snag_utf8_valid(data, len, true)) {
        if (error_size) (void)snprintf(error, error_size, "invalid UTF-8, NUL, or JSON input size");
        errno = EINVAL;
        return NULL;
    }
    memset(&jerr, 0, sizeof(jerr));
    value = json_loadb((const char *)data, len, flags | JSON_DECODE_ANY, &jerr);
    if (!value) {
        if (error_size)
            (void)snprintf(error, error_size, "JSON at line %d column %d: %.120s", jerr.line,
                jerr.column, jerr.text);
        errno = EINVAL;
        return NULL;
    }
    if (validate_loaded(value, 0u) < 0) {
        if (error_size)
            (void)snprintf(
                error, error_size, "JSON exceeds nesting limit or contains invalid text");
        json_decref(value);
        return NULL;
    }
    return value;
}

json_t *
snag_json_load_strict(
    const unsigned char *data, size_t len, size_t max_len, char *error, size_t error_size)
{
    /* A repeated key makes a record ambiguous, which every strict caller
     * (wire redaction, events, configuration) must refuse rather than guess. */
    return load_json(data, len, max_len, JSON_REJECT_DUPLICATES, error, error_size);
}

json_t *
snag_json_load_arguments(
    const unsigned char *data, size_t len, size_t max_len, char *error, size_t error_size)
{
    /* Providers repeat keys in function arguments; the last value is the
     * effective one, and failing the whole call loses an otherwise valid turn. */
    return load_json(data, len, max_len, 0, error, error_size);
}

json_t *
snag_json_load_canonical_bounded(
    const unsigned char *data, size_t len, size_t max_len, char *error, size_t error_size)
{
    json_t *value;

    value = snag_json_load_strict(data, len, max_len, error, error_size);
    if (!value) return NULL;
    struct snag_buf encoded = {.max = max_len};
    if (snag_json_canonical(value, &encoded) < 0 || encoded.len != len ||
        memcmp(encoded.data, data, len) != 0) {
        if (error_size) (void)snprintf(error, error_size, "record is not canonical format-1 JSON");
        snag_buf_free(&encoded);
        json_decref(value);
        errno = EINVAL;
        return NULL;
    }
    snag_buf_free(&encoded);
    return value;
}

json_t *
snag_json_load_canonical(const unsigned char *data, size_t len, char *error, size_t error_size)
{
    return snag_json_load_canonical_bounded(data, len, SNAG_MAX_EVENT_LINE, error, error_size);
}

int
snag_json_digest_bounded(
    const json_t *value, size_t max, char out[SNAG_SHA256_HEX_LEN + 1u], size_t *bytes)
{
    int rc = -1;

    struct snag_buf encoded = {.max = max};
    if (snag_json_canonical(value, &encoded) == 0) {
        if (out) snag_sha256_hex(encoded.data, encoded.len, out);
        if (bytes) *bytes = encoded.len;
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

    if (!json_is_object(object)) return false;
    while (*keys) {
        size_t len = strcspn(keys, " ");
        if (!len || !json_object_getn(object, keys, len)) return false;
        ++count;
        keys += len;
        if (*keys) ++keys;
    }
    return json_object_size(object) == count;
}

bool
snag_json_arg_keys(
    const json_t *object, const char *required, const char *optional, char *error, size_t size)
{
    if (!json_is_object(object)) {
        snag_errorf(error, size, "Arguments must be a JSON object.");
        return false;
    }
    const char *missing = required;
    size_t missing_len = 0u;
    while (*missing) {
        missing_len = strcspn(missing, " ");
        if (!json_object_getn(object, missing, missing_len)) break;
        missing += missing_len + (missing[missing_len] == ' ');
        missing_len = 0u;
    }
    struct snag_buf extra = {.max = 1024u};
    bool unexpected = false;
    for (void *it = json_object_iter((json_t *)object); it;
        it = json_object_iter_next((json_t *)object, it)) {
        const char *key = json_object_iter_key(it);
        if (snag_string_in(key, required) || snag_string_in(key, optional)) continue;
        unexpected = true;
        json_t *field = json_string(strlen(key) <= 96u ? key : "<long field name>");
        if (field) (void)snag_json_canonical(field, &extra);
        json_decref(field);
        break;
    }
    bool valid = !missing_len && !unexpected;
    if (!valid) {
        (void)snag_buf_terminate(&extra);
        snag_errorf(error, size,
            "Missing required: %.*s. Unexpected argument: %s. Required: %s. Optional: %s. Key "
            "order does not matter.",
            (int)(missing_len ? missing_len : 4u), missing_len ? missing : "none",
            unexpected && extra.data ? (const char *)extra.data
            : unexpected             ? "field"
                                     : "none",
            *required ? required : "none", *optional ? optional : "none");
    }
    snag_buf_free(&extra);
    return valid;
}

const char *
snag_json_arg_name(
    const json_t *args, const char *name, const char *legacy, char *error, size_t size)
{
    if (!json_object_get(args, legacy)) return name;
    if (json_object_get(args, name)) {
        snag_errorf(
            error, size, "Ambiguous arguments: supply only %s or %s, not both.", name, legacy);
        return NULL;
    }
    return legacy;
}

bool
snag_json_arg_uint(const json_t *args, const char *key, uint64_t fallback, uint64_t min,
    uint64_t max, uint64_t *out, char *error, size_t size)
{
    const json_t *value = json_object_get(args, key);
    if (!value || json_is_null(value)) {
        *out = fallback;
        return true;
    }
    if (json_is_integer(value)) {
        json_int_t n = json_integer_value(value);
        if (n >= 0 && (uint64_t)n >= min && (uint64_t)n <= max) {
            *out = (uint64_t)n;
            return true;
        }
        snag_errorf(error, size,
            "%s=%lld is outside the allowed range %llu..%llu; use JSON null for the default.", key,
            (long long)n, (unsigned long long)min, (unsigned long long)max);
    } else {
        snag_errorf(error, size, "%s must be an integer in %llu..%llu or JSON null (not a string).",
            key, (unsigned long long)min, (unsigned long long)max);
    }
    return false;
}

bool
snag_json_arg_bool(
    const json_t *args, const char *key, bool fallback, bool *out, char *error, size_t size)
{
    const json_t *value = json_object_get(args, key);
    if (!value || json_is_null(value) || json_is_boolean(value)) {
        *out = !value || json_is_null(value) ? fallback : json_is_true(value);
        return true;
    }
    snag_errorf(error, size, "%s must be a JSON boolean or null (not a string).", key);
    return false;
}

bool
snag_json_arg_text(const json_t *args, const char *key, size_t min, size_t max, bool nullable,
    const char **out, char *error, size_t size)
{
    const json_t *value = json_object_get(args, key);
    *out = json_string_value(value);
    if ((nullable && (!value || json_is_null(value))) ||
        (snag_text_valid(*out, min, max) && strlen(*out) == json_string_length(value)))
        return true;
    snag_errorf(error, size, "%s must be UTF-8 text of %zu..%zu bytes without NUL%s.", key, min,
        max, nullable ? ", or JSON null" : "");
    return false;
}

const char *
snag_json_string(const json_t *object, const char *key)
{
    json_t *value = json_object_get(object, key);
    return json_is_string(value) ? json_string_value(value) : NULL;
}

const char *
snag_json_bounded_string(const json_t *value, size_t max)
{
    const char *text = json_string_value(value);
    size_t len = json_string_length(value);
    return text && len && len <= max && strlen(text) == len &&
                   snag_utf8_valid((const unsigned char *)text, len, true)
               ? text
               : NULL;
}

int
snag_json_integer_u64(const json_t *object, const char *key, uint64_t *out)
{
    json_t *value = json_object_get(object, key);
    json_int_t n;

    if (!json_is_integer(value)) return -1;
    n = json_integer_value(value);
    if (n < 0) return -1;
    *out = (uint64_t)n;
    return 0;
}

/* Advertised/configured limits are positive; zero is internal absence only. */
int
snag_json_merge_limit(const json_t *object, const char *key, uint64_t max, uint64_t *out)
{
    json_t *value;
    json_int_t integer;

    if (!object || !(value = json_object_get(object, key)) || json_is_null(value)) return 0;
    if (!json_is_integer(value) || (integer = json_integer_value(value)) <= 0 ||
        (uint64_t)integer > max || (*out && *out != (uint64_t)integer))
        return -1;
    *out = (uint64_t)integer;
    return 0;
}

bool
snag_json_nullable_limit(const json_t *object, const char *key, uint64_t max, uint64_t *value)
{
    *value = 0u;
    return json_object_get(object, key) && snag_json_merge_limit(object, key, max, value) == 0;
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
    if (snag_json_integer_u64(object, key, out) < 0) return -1;
    *known = true;
    return 0;
}

int
snag_json_set_new(json_t *object, const char *key, json_t *value)
{
    if (!value) return snag_errno(ENOMEM);
    if (json_object_set_new(object, key, value) < 0) return snag_errno(ENOMEM);
    return 0;
}
