/* SPDX-License-Identifier: GPL-2.0-only */
#include "wire.h"
#include "json.h"

#include <errno.h>
#include "snag_jansson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int
secrets_valid(const struct snag_wire_secrets *secrets)
{
    if (!secrets)
        return 0;
    if (secrets->count > SNAG_WIRE_SECRET_COUNT_MAX ||
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
        if (!len || len > SNAG_WIRE_SECRET_MAX) {
            errno = EINVAL;
            return -1;
        }
    }
    return 0;
}

size_t
snag_wire_secret_match(const unsigned char *data, size_t len,
                       const struct snag_wire_secrets *secrets)
{
    size_t best = 0u;

    if (!secrets)
        return 0u;
    for (size_t i = 0; i < secrets->count; ++i) {
        if (!secrets->values[i])
            continue;
        size_t n = strlen(secrets->values[i]);
        if (n > best && n <= len &&
            (unsigned char)secrets->values[i][0] == data[0] &&
            memcmp(data, secrets->values[i], n) == 0)
            best = n;
    }
    return best;
}

static bool
contains_secret(const unsigned char *data, size_t len,
                const struct snag_wire_secrets *secrets)
{
    for (size_t i = 0; i < len; ++i)
        if (snag_wire_secret_match(data + i, len - i, secrets))
            return true;
    return false;
}

static int
append_redacted(struct snag_buf *out, const unsigned char *data, size_t len,
                const struct snag_wire_secrets *secrets, bool header)
{
    static const char marker[] = "<redacted:secret>";

    for (size_t i = 0; i < len;) {
        size_t matched = snag_wire_secret_match(data + i, len - i, secrets);
        if (matched) {
            if (snag_buf_append(out, marker, sizeof(marker) - 1u) < 0)
                return -1;
            i += matched;
            continue;
        }
        unsigned char c = data[i++];
        if (header && (c < 0x20u || c > 0x7eu)) {
            if (snag_buf_printf(out, "\\x%02x", (unsigned int)c) < 0)
                return -1;
        } else if (snag_buf_putc(out, c) < 0) {
            return -1;
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

/* Mutate only the private parsed diagnostic tree, returning an owned reference.
 * Keys containing a secret fail closed: redacting them could merge members. */
static json_t *
redact_value(json_t *value, const struct snag_wire_secrets *secrets)
{
    if (json_is_string(value)) {
        struct snag_buf text;
        json_t *redacted = NULL;
        snag_buf_init(&text, SNAG_WIRE_BODY_MAX);
        if (append_redacted(&text, (const unsigned char *)json_string_value(value),
                            json_string_length(value), secrets, false) == 0)
            redacted = json_stringn(text.data ? (char *)text.data : "", text.len);
        snag_buf_free(&text);
        return redacted;
    }
    if (json_is_object(value)) {
        for (void *iter = json_object_iter(value); iter;
             iter = json_object_iter_next(value, iter)) {
            const char *key = json_object_iter_key(iter);
            size_t len = json_object_iter_key_len(iter);
            const char *replacement = key_redaction(key, len);
            if (contains_secret((const unsigned char *)key, len, secrets)) {
                errno = EACCES;
                return NULL;
            }
            json_t *child = replacement ? json_string(replacement) :
                redact_value(json_object_iter_value(iter), secrets);
            if (!child || json_object_set_new(value, key, child) < 0)
                return NULL;
        }
    } else if (json_is_array(value)) {
        for (size_t i = 0; i < json_array_size(value); ++i) {
            json_t *child = redact_value(json_array_get(value, i), secrets);
            if (!child || json_array_set_new(value, i, child) < 0)
                return NULL;
        }
    }
    return json_incref(value);
}

int
snag_wire_json_redact(const unsigned char *data, size_t len,
                     const struct snag_wire_secrets *secrets,
                     struct snag_buf *out, char *error, size_t error_size)
{
    json_t *value;
    int rc;

    if (!out || secrets_valid(secrets) < 0) {
        snag_errorf(error, error_size, "invalid diagnostic secret set");
        return -1;
    }
    value = snag_json_load_strict(data, len, SNAG_WIRE_BODY_MAX,
                                 error, error_size);
    if (!value)
        return -1;
    snag_buf_reset(out);
    json_t *redacted = redact_value(value, secrets);
    rc = redacted ? snag_json_diagnostic(redacted, out) : -1;
    json_decref(redacted);
    json_decref(value);
    if (rc < 0) {
        snag_errorf(error, error_size, "sanitized JSON exceeds diagnostic bound");
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
snag_wire_header_redact(const unsigned char *line, size_t len,
                       const struct snag_wire_secrets *secrets,
                       struct snag_buf *out)
{
    const unsigned char *colon;
    size_t name_len;
    size_t value_start;
    const char *replacement;

    if (!line || !out || !len || len > SNAG_WIRE_HEADER_MAX ||
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
    snag_buf_reset(out);
    for (size_t i = 0; i < name_len; ++i) {
        unsigned char c = line[i];
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c - 'A' + 'a');
        if (snag_buf_putc(out, c) < 0)
            return -1;
    }
    if (snag_buf_append(out, ": ", 2u) < 0)
        return -1;
    replacement = key_redaction((const char *)line, name_len);
    if (replacement) {
        if (key_is((const char *)line, name_len, "authorization") &&
            len - value_start >= 7u &&
            strncasecmp((const char *)line + value_start, "Bearer ", 7u) == 0)
            replacement = "<redacted:bearer>";
        return snag_buf_append(out, replacement, strlen(replacement));
    }
    return append_redacted(out, line + value_start, len - value_start,
                           secrets, true);
}
