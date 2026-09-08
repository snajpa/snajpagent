/* SPDX-License-Identifier: GPL-2.0-only */
#include "base64.h"
#include <errno.h>

static void
encode(const unsigned char *in, unsigned int count, unsigned char out[4])
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned int v = (unsigned int)in[0] << 16;
    if (count > 1u) v |= (unsigned int)in[1] << 8;
    if (count > 2u) v |= in[2];
    out[0] = (unsigned char)alphabet[v >> 18];
    out[1] = (unsigned char)alphabet[(v >> 12) & 63u];
    out[2] = count > 1u ? (unsigned char)alphabet[(v >> 6) & 63u] : '=';
    out[3] = count > 2u ? (unsigned char)alphabet[v & 63u] : '=';
}

int
snag_base64_write(struct snag_base64_stream *s, const void *bytes, size_t len,
                  snag_bytes_sink sink, void *opaque)
{
    const unsigned char *in = bytes;
    unsigned char out[256];
    size_t n = 0;
    if (!s || !sink || (!bytes && len) || s->failed || s->finished || s->used > 2u) {
        errno = EINVAL; return -1;
    }
    while (len) {
        s->tail[s->used++] = *in++; --len;
        if (s->used != 3u) continue;
        encode(s->tail, 3u, out + n); n += 4u; s->used = 0;
        if (n == sizeof(out)) {
            if (sink(opaque, out, n)) { s->failed = true; return -1; }
            n = 0;
        }
    }
    if (n && sink(opaque, out, n)) { s->failed = true; return -1; }
    return 0;
}

int
snag_base64_finish(struct snag_base64_stream *s, snag_bytes_sink sink, void *opaque)
{
    unsigned char out[4];
    if (!s || !sink || s->failed || s->finished || s->used > 2u) { errno = EINVAL; return -1; }
    if (s->used) {
        encode(s->tail, s->used, out);
        if (sink(opaque, out, sizeof(out))) { s->failed = true; return -1; }
    }
    s->used = 0; s->finished = true;
    return 0;
}
