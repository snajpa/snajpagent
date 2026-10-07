/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_wire.h"

#include <errno.h>
#include <string.h>

int
snag_binary_wire_size(size_t size, size_t *out)
{
    if (!out) return snag_errno(EINVAL);
    if (size > SNAG_BINARY_BATCH_MAX) return snag_errno(EOVERFLOW);
    *out = size + (size + SNAG_BINARY_WIRE_BLOCK - 1u) / SNAG_BINARY_WIRE_BLOCK + 1u;
    return 0;
}

static void
encode_block(unsigned char *out, const unsigned char *data, size_t size)
{
    size_t mark = 0u;
    size_t position = 1u;
    unsigned int code = 1u;
    for (size_t i = 0u; i < size; ++i) {
        if (!data[i]) {
            out[mark] = (unsigned char)code;
            mark = position++;
            code = 1u;
        } else {
            out[position++] = data[i];
            ++code;
        }
    }
    out[mark] = (unsigned char)code;
}

int
snag_binary_wire_encode(struct snag_buf *out, const void *data, size_t size)
{
    if (!out || (!data && size)) return snag_errno(EINVAL);
    size_t needed;
    if (snag_binary_wire_size(size, &needed) < 0) return -1;
    struct snag_buf staged = {.max = SNAG_BINARY_WIRE_BATCH_MAX};
    if (snag_buf_reserve(&staged, needed) < 0) return -1;
    const unsigned char *bytes = data;
    size_t position = 0u;
    while (position < size) {
        size_t chunk = size - position;
        if (chunk > SNAG_BINARY_WIRE_BLOCK) chunk = SNAG_BINARY_WIRE_BLOCK;
        encode_block(staged.data + staged.len, bytes + position, chunk);
        staged.len += chunk + 1u;
        position += chunk;
    }
    staged.data[staged.len++] = 0u;
    int rc = snag_buf_append(out, staged.data, staged.len);
    snag_buf_free(&staged);
    return rc;
}

static int
decode_block(unsigned char *out, const unsigned char *data, size_t size)
{
    size_t position = 0u;
    size_t used = 0u;
    while (position < size) {
        unsigned int code = data[position++];
        if (!code || code - 1u > size - position) return snag_errno(EINVAL);
        for (unsigned int i = 1u; i < code; ++i) {
            unsigned char byte = data[position++];
            if (!byte) return snag_errno(EINVAL);
            out[used++] = byte;
        }
        if (position < size) out[used++] = 0u;
    }
    return 0;
}

int
snag_binary_wire_decode(struct snag_buf *out, const void *data, size_t size)
{
    if (!out || !data || !size) return snag_errno(EINVAL);
    if (size > SNAG_BINARY_WIRE_BATCH_MAX) return snag_errno(EOVERFLOW);
    const unsigned char *bytes = data;
    if (bytes[size - 1u]) return snag_errno(EINVAL);
    size_t payload = size - 1u;
    size_t remainder = payload % (SNAG_BINARY_WIRE_BLOCK + 1u);
    if (remainder == 1u) return snag_errno(EINVAL);
    size_t needed = payload - payload / (SNAG_BINARY_WIRE_BLOCK + 1u) - (remainder != 0u);
    if (needed > SNAG_BINARY_BATCH_MAX) return snag_errno(EOVERFLOW);
    struct snag_buf staged = {.max = SNAG_BINARY_BATCH_MAX};
    if (snag_buf_reserve(&staged, needed) < 0) return -1;
    size_t position = 0u;
    int rc = -1;
    while (position < payload) {
        size_t chunk = payload - position;
        if (chunk > SNAG_BINARY_WIRE_BLOCK + 1u) {
            chunk = SNAG_BINARY_WIRE_BLOCK + 1u;
        }
        if (decode_block(staged.data + staged.len, bytes + position, chunk) < 0) {
            goto done;
        }
        staged.len += chunk - 1u;
        position += chunk;
    }
    rc = snag_buf_append(out, staged.data, staged.len);
done:
    snag_buf_free(&staged);
    return rc;
}

int
snag_binary_wire_header(const void *data, size_t size,
    unsigned char out[SNAG_BINARY_BATCH_HEADER_SIZE])
{
    if (!out || (!data && size)) return snag_errno(EINVAL);
    const unsigned char *bytes = data;
    unsigned char header[SNAG_BINARY_BATCH_HEADER_SIZE];
    size_t position = 0u;
    size_t used = 0u;
    while (used < sizeof(header)) {
        if (position == size) return 1;
        unsigned int code = bytes[position++];
        if (!code || code - 1u > SNAG_BINARY_WIRE_BLOCK + 1u - position) {
            return snag_errno(EINVAL);
        }
        size_t chunk = code - 1u;
        if (chunk > sizeof(header) - used) chunk = sizeof(header) - used;
        if (chunk > size - position) return 1;
        for (size_t i = 0u; i < chunk; ++i) {
            if (!bytes[position]) return snag_errno(EINVAL);
            header[used++] = bytes[position++];
        }
        if (used == sizeof(header)) break;
        /* A following nonzero code proves that this is an interior separator,
         * not the end of a shorter block whose last run carries no zero. */
        if (position == size) return 1;
        if (!bytes[position]) return snag_errno(EINVAL);
        header[used++] = 0u;
    }
    memcpy(out, header, sizeof(header));
    return 0;
}
