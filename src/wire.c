/* SPDX-License-Identifier: GPL-2.0-only */
#include "wire.h"
#include "json.h"

#include <errno.h>
#include "snj_jansson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void
set_error(char *error, size_t size, const char *message)
{
    if (size)
        (void)snprintf(error, size, "%s", message);
}

static int
secrets_valid(const struct snj_wire_secrets *secrets)
{
    if (!secrets)
        return 0;
    if (secrets->count > SNJ_WIRE_SECRET_COUNT_MAX ||
        (secrets->count && !secrets->values)) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < secrets->count; ++i) {
        size_t len;
        if (!secrets->values[i]) {
            errno = EINVAL;
            return -1;
        }
        len = strlen(secrets->values[i]);
        if (!len || len > SNJ_WIRE_SECRET_MAX) {
            errno = EINVAL;
            return -1;
        }
    }
    return 0;
}

static size_t
secret_at(const unsigned char *data, size_t len, size_t offset,
          const struct snj_wire_secrets *secrets)
{
    size_t best = 0u;

    if (!secrets)
        return 0u;
    for (size_t i = 0; i < secrets->count; ++i) {
        size_t n = strlen(secrets->values[i]);
        if (n > best && n <= len - offset &&
            (unsigned char)secrets->values[i][0] == data[offset] &&
            memcmp(data + offset, secrets->values[i], n) == 0)
            best = n;
    }
    return best;
}

static bool
contains_secret(const unsigned char *data, size_t len,
                const struct snj_wire_secrets *secrets)
{
    for (size_t i = 0; i < len; ++i)
        if (secret_at(data, len, i, secrets))
            return true;
    return false;
}

static int
append_redacted(struct snj_buf *out, const unsigned char *data, size_t len,
                const struct snj_wire_secrets *secrets, bool json_string)
{
    static const char marker[] = "<redacted:secret>";

    for (size_t i = 0; i < len;) {
        size_t matched = secret_at(data, len, i, secrets);
        if (matched) {
            if (snj_buf_append(out, marker, sizeof(marker) - 1u) < 0)
                return -1;
            i += matched;
            continue;
        }
        if (json_string) {
            unsigned char c = data[i++];
            static const char hex[] = "0123456789abcdef";
            if (c == '"' || c == '\\') {
                if (snj_buf_putc(out, '\\') < 0 || snj_buf_putc(out, c) < 0)
                    return -1;
            } else if (c <= 0x1fu) {
                unsigned char escaped[6] = {'\\', 'u', '0', '0',
                    (unsigned char)hex[c >> 4], (unsigned char)hex[c & 15u]};
                if (snj_buf_append(out, escaped, sizeof(escaped)) < 0)
                    return -1;
            } else if (snj_buf_putc(out, c) < 0) {
                return -1;
            }
        } else {
            unsigned char c = data[i++];
            if (c >= 0x20u && c <= 0x7eu) {
                if (snj_buf_putc(out, c) < 0)
                    return -1;
            } else if (snj_buf_printf(out, "\\x%02x", (unsigned int)c) < 0) {
                return -1;
            }
        }
    }
    return 0;
}

static bool
key_is(const char *key, size_t len, const char *wanted)
{
    return strlen(wanted) == len && strncasecmp(key, wanted, len) == 0;
}

static const char *
key_redaction(const char *key, size_t len)
{
    if (key_is(key, len, "authorization"))
        return "<redacted:authorization>";
    if (key_is(key, len, "proxy-authorization"))
        return "<redacted:proxy-authorization>";
    if (key_is(key, len, "cookie") || key_is(key, len, "set-cookie"))
        return "<redacted:cookie>";
    if (key_is(key, len, "x-api-key") || key_is(key, len, "api-key") ||
        key_is(key, len, "openai-api-key") || key_is(key, len, "api_key") ||
        key_is(key, len, "access_token") || key_is(key, len, "refresh_token") ||
        key_is(key, len, "client_secret"))
        return "<redacted:credential>";
    if (key_is(key, len, "encrypted_content"))
        return "<redacted:encrypted_reasoning>";
    return NULL;
}

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

static int encode_value(const json_t *value,
                        const struct snj_wire_secrets *secrets,
                        struct snj_buf *out, unsigned int depth);

static int
encode_string(struct snj_buf *out, const char *text, size_t len,
              const struct snj_wire_secrets *secrets)
{
    if (snj_buf_putc(out, '"') < 0 ||
        append_redacted(out, (const unsigned char *)text, len, secrets, true) < 0)
        return -1;
    return snj_buf_putc(out, '"');
}

static int
encode_object(const json_t *value, const struct snj_wire_secrets *secrets,
              struct snj_buf *out, unsigned int depth)
{
    size_t count = json_object_size(value);
    struct key_ref *keys = NULL;
    void *iter;
    size_t used = 0u;
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
    for (iter = json_object_iter((json_t *)value); iter;
         iter = json_object_iter_next((json_t *)value, iter)) {
        if (used >= count)
            goto out;
        keys[used].name = json_object_iter_key(iter);
        keys[used].len = json_object_iter_key_len(iter);
        if (!keys[used].name ||
            contains_secret((const unsigned char *)keys[used].name,
                            keys[used].len, secrets)) {
            errno = EACCES;
            goto out;
        }
        ++used;
    }
    if (used != count)
        goto out;
    if (count)
        qsort(keys, count, sizeof(*keys), key_compare);
    if (snj_buf_putc(out, '{') < 0)
        goto out;
    for (size_t i = 0; i < count; ++i) {
        json_t *member = json_object_getn(value, keys[i].name, keys[i].len);
        const char *replacement = key_redaction(keys[i].name, keys[i].len);
        if ((i && snj_buf_putc(out, ',') < 0) ||
            encode_string(out, keys[i].name, keys[i].len, NULL) < 0 ||
            snj_buf_putc(out, ':') < 0 || !member)
            goto out;
        if (replacement) {
            if (encode_string(out, replacement, strlen(replacement), NULL) < 0)
                goto out;
        } else if (encode_value(member, secrets, out, depth + 1u) < 0) {
            goto out;
        }
    }
    if (snj_buf_putc(out, '}') < 0)
        goto out;
    rc = 0;
out:
    free(keys);
    return rc;
}

static int
encode_array(const json_t *value, const struct snj_wire_secrets *secrets,
             struct snj_buf *out, unsigned int depth)
{
    size_t count = json_array_size(value);
    if (snj_buf_putc(out, '[') < 0)
        return -1;
    for (size_t i = 0; i < count; ++i) {
        if ((i && snj_buf_putc(out, ',') < 0) ||
            encode_value(json_array_get(value, i), secrets, out, depth + 1u) < 0)
            return -1;
    }
    return snj_buf_putc(out, ']');
}

static int
encode_value(const json_t *value, const struct snj_wire_secrets *secrets,
             struct snj_buf *out, unsigned int depth)
{
    char number[64];
    int n;

    if (!value || depth > 48u) {
        errno = EOVERFLOW;
        return -1;
    }
    switch (json_typeof(value)) {
    case JSON_OBJECT:
        return encode_object(value, secrets, out, depth);
    case JSON_ARRAY:
        return encode_array(value, secrets, out, depth);
    case JSON_STRING:
        return encode_string(out, json_string_value(value),
                             json_string_length(value), secrets);
    case JSON_INTEGER:
        n = snprintf(number, sizeof(number), "%lld",
                     (long long)json_integer_value(value));
        break;
    case JSON_REAL:
        n = snprintf(number, sizeof(number), "%.17g", json_real_value(value));
        break;
    case JSON_TRUE:
        return snj_buf_append(out, "true", 4u);
    case JSON_FALSE:
        return snj_buf_append(out, "false", 5u);
    case JSON_NULL:
        return snj_buf_append(out, "null", 4u);
    default:
        errno = EINVAL;
        return -1;
    }
    if (n <= 0 || (size_t)n >= sizeof(number)) {
        errno = EOVERFLOW;
        return -1;
    }
    return snj_buf_append(out, number, (size_t)n);
}

int
snj_wire_json_redact(const unsigned char *data, size_t len,
                     const struct snj_wire_secrets *secrets,
                     struct snj_buf *out, char *error, size_t error_size)
{
    json_t *value;
    int rc;

    if (!out || secrets_valid(secrets) < 0) {
        set_error(error, error_size, "invalid diagnostic secret set");
        return -1;
    }
    value = snj_json_load_strict(data, len, SNJ_WIRE_BODY_MAX,
                                 error, error_size);
    if (!value)
        return -1;
    snj_buf_reset(out);
    rc = encode_value(value, secrets, out, 0u);
    json_decref(value);
    if (rc < 0) {
        set_error(error, error_size, "sanitized JSON exceeds diagnostic bound");
        return -1;
    }
    return 0;
}

static bool
header_name_valid(const unsigned char *name, size_t len)
{
    static const char token_extra[] = "!#$%&'*+-.^_`|~";
    if (!len)
        return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') || strchr(token_extra, c)))
            return false;
    }
    return true;
}

int
snj_wire_header_redact(const unsigned char *line, size_t len,
                       const struct snj_wire_secrets *secrets,
                       struct snj_buf *out)
{
    const unsigned char *colon;
    size_t name_len;
    size_t value_start;
    const char *replacement;

    if (!line || !out || !len || len > SNJ_WIRE_HEADER_MAX ||
        secrets_valid(secrets) < 0) {
        errno = EINVAL;
        return -1;
    }
    colon = memchr(line, ':', len);
    if (!colon) {
        errno = EINVAL;
        return -1;
    }
    name_len = (size_t)(colon - line);
    if (!header_name_valid(line, name_len)) {
        errno = EINVAL;
        return -1;
    }
    value_start = name_len + 1u;
    while (value_start < len && (line[value_start] == ' ' || line[value_start] == '\t'))
        ++value_start;
    snj_buf_reset(out);
    for (size_t i = 0; i < name_len; ++i) {
        unsigned char c = line[i];
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c - 'A' + 'a');
        if (snj_buf_putc(out, c) < 0)
            return -1;
    }
    if (snj_buf_append(out, ": ", 2u) < 0)
        return -1;
    replacement = key_redaction((const char *)line, name_len);
    if (replacement) {
        if (key_is((const char *)line, name_len, "authorization") &&
            len - value_start >= 7u &&
            strncasecmp((const char *)line + value_start, "Bearer ", 7u) == 0)
            replacement = "<redacted:bearer>";
        return snj_buf_append(out, replacement, strlen(replacement));
    }
    return append_redacted(out, line + value_start, len - value_start,
                           secrets, false);
}

int
snj_wire_url_redact(const char *url, const struct snj_wire_secrets *secrets,
                    struct snj_buf *out)
{
    size_t len;
    const char *query;

    if (!url || !out || secrets_valid(secrets) < 0 ||
        (len = strlen(url)) == 0u || len > SNJ_WIRE_URL_MAX ||
        !snj_utf8_valid((const unsigned char *)url, len, true)) {
        errno = EINVAL;
        return -1;
    }
    snj_buf_reset(out);
    query = strchr(url, '?');
    if (!query)
        return append_redacted(out, (const unsigned char *)url, len,
                               secrets, false);
    if (append_redacted(out, (const unsigned char *)url,
                        (size_t)(query - url + 1), secrets, false) < 0)
        return -1;
    for (const char *p = query + 1; *p;) {
        const char *end = strchr(p, '&');
        const char *eq;
        size_t field_len;
        if (!end)
            end = url + len;
        field_len = (size_t)(end - p);
        eq = memchr(p, '=', field_len);
        if (eq) {
            if (append_redacted(out, (const unsigned char *)p,
                                (size_t)(eq - p + 1), secrets, false) < 0 ||
                snj_buf_append(out, "<redacted:query>", 16u) < 0)
                return -1;
        } else if (append_redacted(out, (const unsigned char *)p,
                                   field_len, secrets, false) < 0) {
            return -1;
        }
        if (*end == '&') {
            if (snj_buf_putc(out, '&') < 0)
                return -1;
            p = end + 1;
        } else {
            break;
        }
    }
    return 0;
}

int
snj_wire_body_metadata(const char *media_type, const void *data, size_t len,
                       struct snj_buf *out)
{
    char digest[SNJ_SHA256_HEX_LEN + 1u];
    size_t media_len;

    if (!media_type || !out || (!data && len) ||
        (media_len = strlen(media_type)) == 0u || media_len > 256u) {
        errno = EINVAL;
        return -1;
    }
    snj_sha256_hex(data, len, digest);
    snj_buf_reset(out);
    if (snj_buf_append(out, "<non-json media-type=", 21u) < 0 ||
        append_redacted(out, (const unsigned char *)media_type, media_len,
                        NULL, false) < 0 ||
        snj_buf_printf(out, " bytes=%zu sha256=%s>", len, digest) < 0)
        return -1;
    return 0;
}
