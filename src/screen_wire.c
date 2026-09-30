/* SPDX-License-Identifier: GPL-2.0-only */
#include "screen_wire.h"
#include "base64.h"
#include "base.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

static int
append_title(void *opaque, const unsigned char *bytes, size_t length)
{
    struct snag_buf *out = opaque;
    return snag_buf_append(out, bytes, length);
}

uint32_t
snag_screen_checksum(const unsigned char *bytes, size_t length)
{
    return (uint32_t)crc32(0, bytes, (uInt)length);
}

int
snag_screen_encode(char *out, size_t capacity, const char *nonce, uint32_t sequence,
                   unsigned int attempt, const unsigned char *bytes, size_t length)
{
    if (!out || !nonce || strlen(nonce) != 8u || !bytes || !length ||
        length > SNAG_SCREEN_CHUNK || attempt > 15u) {
        return snag_errno(EINVAL);
    }
    struct snag_buf encoded;
    snag_buf_init(&encoded, 4u * (SNAG_SCREEN_CHUNK / 3u) + 1u);
    struct snag_base64_stream encoder = {0};
    int rc = -1;
    if (snag_base64_write(&encoder, bytes, length, append_title, &encoded) < 0 ||
        snag_base64_finish(&encoder, append_title, &encoded) < 0) goto done;
    int n = snprintf(out, capacity, "\033]2;" SNAG_SCREEN_PREFIX
        "DATA:%s:%08x:%x:%08x:%.*s\a", nonce, sequence, attempt,
        snag_screen_checksum(bytes, length), (int)encoded.len, (char *)encoded.data);
    if (n < 0 || (size_t)n >= capacity) {
        errno = EOVERFLOW;
        goto done;
    }
    rc = n;
done:
    snag_buf_free(&encoded);
    return rc;
}

static int
hex32(const unsigned char *text, uint32_t *number)
{
    uint32_t value = 0;
    for (size_t i = 0; i < 8u; ++i) {
        unsigned char c = text[i];
        if (c >= '0' && c <= '9') value = (value << 4) | (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') value = (value << 4) | (uint32_t)(c - 'a' + 10);
        else return snag_errno(EPROTO);
    }
    *number = value;
    return 0;
}

static int
base64_digit(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int
snag_screen_decode(const unsigned char *body, size_t length, const char *nonce,
                   uint32_t *sequence, uint32_t *checksum,
                   unsigned char bytes[SNAG_SCREEN_CHUNK], size_t *written)
{
    /* DATA:nonce:sequence:attempt:crc:base64. All fields are fixed-width
     * except the bounded, padded payload. Reject truncated title updates. */
    if (!body || !nonce || !sequence || !checksum || !bytes || !written ||
        length < 5u + 8u + 1u + 8u + 1u + 1u + 1u + 8u + 1u + 4u ||
        memcmp(body, "DATA:", 5u) || memcmp(body + 5u, nonce, 8u) ||
        body[13] != ':' || body[22] != ':' || body[24] != ':' || body[33] != ':' ||
        !((body[23] >= '0' && body[23] <= '9') ||
          (body[23] >= 'a' && body[23] <= 'f')) ||
        hex32(body + 14u, sequence) < 0 || hex32(body + 25u, checksum) < 0) {
        return snag_errno(EPROTO);
    }
    size_t size = length - 34u;
    if (!size || size > 4u * (SNAG_SCREEN_CHUNK / 3u) || size % 4u) {
        return snag_errno(EPROTO);
    }
    size_t count = 0u;
    for (size_t i = 34u; i < length; i += 4u) {
        int a = base64_digit(body[i]);
        int b = base64_digit(body[i + 1u]);
        int c = body[i + 2u] == '=' ? -2 : base64_digit(body[i + 2u]);
        int d = body[i + 3u] == '=' ? -2 : base64_digit(body[i + 3u]);
        if (a < 0 || b < 0 || c == -1 || d == -1 ||
            (c == -2 && d != -2) ||
            (i + 4u != length && (c == -2 || d == -2)) ||
            (c == -2 && (b & 15)) || (d == -2 && c >= 0 && (c & 3))) {
            return snag_errno(EPROTO);
        }
        bytes[count++] = (unsigned char)((a << 2) | (b >> 4));
        if (c >= 0) bytes[count++] = (unsigned char)((b << 4) | (c >> 2));
        if (d >= 0) bytes[count++] = (unsigned char)((c << 6) | d);
    }
    if (!count || snag_screen_checksum(bytes, count) != *checksum) {
        return snag_errno(EPROTO);
    }
    *written = count;
    return 0;
}
