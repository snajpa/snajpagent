/* SPDX-License-Identifier: GPL-2.0-only */
#include "irc_address.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int
decode(char *out, size_t capacity, const char *text, size_t length, char *error, size_t size)
{
    size_t used = 0u;
    for (size_t i = 0u; i < length; ++i) {
        unsigned char byte = (unsigned char)text[i];
        if (byte == '%') {
            if (i + 2u >= length || text[i + 1u] != '2' ||
                (text[i + 2u] != '5' && text[i + 2u] != 'F' && text[i + 2u] != 'f'))
                return snag_fail(error, size, EINVAL, "address escapes are %%2F and %%25");
            byte = text[i + 2u] == '5' ? '%' : '/';
            i += 2u;
        }
        if (byte < 0x20u || byte == 0x7fu)
            return snag_fail(error, size, EINVAL, "address contains a control character");
        if (used + 1u >= capacity)
            return snag_fail(error, size, EOVERFLOW, "address component is too long");
        out[used++] = (char)byte;
    }
    out[used] = '\0';
    if (!used || !snag_utf8_valid((const unsigned char *)out, used, true) || snag_text_blank(out))
        return snag_fail(error, size, EINVAL, "address component is empty or invalid UTF-8");
    return 0;
}

int
snag_irc_address_parse(struct snag_irc_address *out, const char *text,
    enum snag_irc_address_form form, char *error, size_t size)
{
    if (!out || !text || (form != SNAG_IRC_BUFFER_ADDRESS && form != SNAG_IRC_MESSAGE_ADDRESS))
        return snag_fail(error, size, EINVAL, "invalid address form");
    const char *parts[3] = {text};
    size_t lengths[3] = {0}, count = 1u;
    for (const char *at = text; ; ++at) {
        if (*at != '/' && *at) continue;
        lengths[count - 1u] = (size_t)(at - parts[count - 1u]);
        if (!*at) break;
        if (count == 3u) return snag_fail(error, size, EINVAL,
            "address has too many components; escape literal slash as %%2F");
        parts[count++] = at + 1u;
    }
    if (form == SNAG_IRC_BUFFER_ADDRESS && count == 2u)
        return snag_fail(error, size, EINVAL, "connection address needs a trailing slash");
    struct snag_irc_address address = {.kind = SNAG_IRC_CONVERSATION};
    if (form == SNAG_IRC_BUFFER_ADDRESS && count == 1u) {
        address.kind = SNAG_IRC_TRANSCRIPT;
        if (decode(address.session, sizeof(address.session), parts[0], lengths[0], error, size) < 0)
            return -1;
    } else {
        if (count == 3u &&
            decode(address.session, sizeof(address.session), parts[0], lengths[0], error, size) < 0)
            return -1;
        if (count >= 2u && decode(address.endpoint, sizeof(address.endpoint), parts[count - 2u],
            lengths[count - 2u], error, size) < 0) return -1;
        if (form == SNAG_IRC_BUFFER_ADDRESS && !lengths[count - 1u]) {
            address.kind = SNAG_IRC_CONNECTION;
        } else if (decode(address.target, sizeof(address.target), parts[count - 1u],
            lengths[count - 1u], error, size) < 0) {
            return -1;
        }
    }
    *out = address;
    return 0;
}

static int
encode(struct snag_buf *out, const char *component)
{
    for (const unsigned char *at = (const unsigned char *)component; *at; ++at) {
        if (*at == '/' || *at == '%') {
            if (snag_buf_append(out, *at == '/' ? "%2F" : "%25", 3u) < 0) return -1;
        } else if (snag_buf_putc(out, *at) < 0) {
            return -1;
        }
    }
    return 0;
}

char *
snag_irc_address_format(const struct snag_irc_address *address)
{
    struct snag_buf out = {.max = 3u * sizeof(*address)};
    if (address->session[0] && (encode(&out, address->session) < 0 ||
        (address->kind != SNAG_IRC_TRANSCRIPT && snag_buf_putc(&out, '/') < 0))) goto failed;
    if (address->endpoint[0] && (encode(&out, address->endpoint) < 0 ||
        snag_buf_putc(&out, '/') < 0)) goto failed;
    if (address->kind == SNAG_IRC_CONVERSATION && encode(&out, address->target) < 0) goto failed;
    if (snag_buf_terminate(&out) < 0) goto failed;
    return (char *)out.data;
failed:
    snag_buf_free(&out);
    return NULL;
}

int
snag_irc_address_operand(const char *input, char **operand, const char **rest, char *error, size_t size)
{
    *operand = NULL;
    *rest = input;
    struct snag_buf out = {.max = 3u * sizeof(struct snag_irc_address)};
    const char *at = input;
    unsigned char quote = 0u;
    while (isspace((unsigned char)*at)) ++at;
    for (; *at; ++at) {
        unsigned char byte = (unsigned char)*at;
        if (!quote && isspace(byte)) break;
        if (byte == quote) {
            quote = 0u;
            continue;
        }
        if (!quote && (byte == '\'' || byte == '"')) {
            quote = byte;
            continue;
        }
        if (byte == '\\' && quote != '\'') {
            if (!at[1]) goto invalid;
            byte = (unsigned char)*++at;
        }
        if (snag_buf_putc(&out, byte) < 0) goto invalid;
    }
    if (quote || !out.len || snag_buf_terminate(&out) < 0) goto invalid;
    while (isspace((unsigned char)*at)) ++at;
    *operand = (char *)out.data;
    *rest = at;
    return 0;
invalid:
    snag_buf_free(&out);
    return snag_fail(error, size, EINVAL,
        "address operand is missing, oversized or has unfinished quoting");
}
