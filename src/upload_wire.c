/* SPDX-License-Identifier: GPL-2.0-only */
#include "upload_wire.h"
#include "base64.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static int
digit(unsigned char byte)
{
    if (byte >= 'A' && byte <= 'Z') return byte - 'A';
    if (byte >= 'a' && byte <= 'z') return byte - 'a' + 26;
    if (byte >= '0' && byte <= '9') return byte - '0' + 52;
    if (byte == '+') return 62;
    if (byte == '/') return 63;
    return -1;
}

static int
decode_base64(const char *payload, size_t length, unsigned char *compressed,
              size_t *used)
{
    size_t n = 0;

    for (size_t i = 0; i < length; i += 4u) {
        int a = digit((unsigned char)payload[i]);
        int b = digit((unsigned char)payload[i + 1u]);
        int c = payload[i + 2u] == '=' ? -2 : digit((unsigned char)payload[i + 2u]);
        int d = payload[i + 3u] == '=' ? -2 : digit((unsigned char)payload[i + 3u]);
        bool final = i + 4u == length;

        if (a < 0 || b < 0 || c == -1 || d == -1 ||
            (c == -2 && d != -2) || (!final && (c == -2 || d == -2)) ||
            (c == -2 && (b & 15) != 0) || (d == -2 && c >= 0 && (c & 3) != 0)) {
            return snag_errno(EPROTO);
        }
        compressed[n++] = (unsigned char)((a << 2) | (b >> 4));
        if (c >= 0) compressed[n++] = (unsigned char)((b << 4) | (c >> 2));
        if (d >= 0) compressed[n++] = (unsigned char)((c << 6) | d);
    }
    *used = n;
    return 0;
}

int
snag_upload_wire_decode(const char *payload, size_t length, unsigned char *out,
                        size_t capacity, size_t *written)
{
    unsigned char *compressed = NULL;
    unsigned char *decoded = NULL;
    size_t used = 0;
    int rc = -1;

    if (!payload || !out || !written || !length || length > SNAG_UPLOAD_LINE_MAX ||
        length % 4u || capacity > SNAG_UPLOAD_BLOCK_MAX) {
        return snag_errno(EINVAL);
    }
    *written = 0;
    compressed = malloc(length / 4u * 3u);
    decoded = malloc(capacity + 1u);
    if (!compressed || !decoded) {
        errno = ENOMEM;
        goto done;
    }
    if (decode_base64(payload, length, compressed, &used) < 0) goto done;

    z_stream stream = {0};
    if (inflateInit(&stream) != Z_OK) {
        errno = EIO;
        goto done;
    }
    stream.next_in = compressed;
    stream.avail_in = (uInt)used;
    stream.next_out = decoded;
    stream.avail_out = (uInt)(capacity + 1u);
    int status = inflate(&stream, Z_FINISH);
    if (status != Z_STREAM_END || stream.avail_in || stream.total_out > capacity) {
        errno = stream.total_out > capacity ? EOVERFLOW : EPROTO;
    } else {
        *written = (size_t)stream.total_out;
        memcpy(out, decoded, *written);
        rc = 0;
    }
    (void)inflateEnd(&stream);
done:
    free(decoded);
    free(compressed);
    return rc;
}

static int
append_encoded(void *opaque, const unsigned char *bytes, size_t length)
{
    return snag_buf_append(opaque, bytes, length);
}

int
snag_upload_wire_encode(const void *data, size_t length, struct snag_buf *out)
{
    if (!out || (!data && length) || length > SNAG_UPLOAD_BLOCK_MAX) {
        return snag_errno(EINVAL);
    }
    uLongf bound = compressBound((uLong)length);
    unsigned char *compressed = malloc((size_t)bound);
    if (!compressed) return snag_errno(ENOMEM);

    int rc = -1;
    const void *source = length ? data : "";
    if (compress2(compressed, &bound, source, (uLong)length, Z_DEFAULT_COMPRESSION) != Z_OK) {
        errno = EIO;
        goto done;
    }
    struct snag_base64_stream encoder = {0};
    if (snag_base64_write(&encoder, compressed, (size_t)bound, append_encoded, out) < 0 ||
        snag_base64_finish(&encoder, append_encoded, out) < 0) {
        goto done;
    }
    rc = 0;
done:
    free(compressed);
    return rc;
}
