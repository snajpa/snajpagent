/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_STORE_BINARY_WIRE_H
#define SNAJPAGENT_STORE_BINARY_WIRE_H

#include "store_binary.h"

/* A one-byte run code represents up to 254 data bytes. Independently coded
 * blocks have exactly one byte of overhead; only the whole-frame delimiter is
 * zero. Fixed geometry lets a checked decoded header determine the wire length. */
#define SNAG_BINARY_WIRE_BLOCK 254u
#define SNAG_BINARY_WIRE_BATCH_MAX (SNAG_BINARY_BATCH_MAX + \
    (SNAG_BINARY_BATCH_MAX + SNAG_BINARY_WIRE_BLOCK - 1u) / SNAG_BINARY_WIRE_BLOCK + 1u)

/* Byte envelope only: no batch checksum, identity or membership is established.
 * Encode/decode append atomically, including when input borrows output storage.
 * Exactly one final zero is required for decode; all other zero bytes fail.
 * Empty data has the single-byte envelope zero, but is not a valid journal batch.
 * The existing decoded batch size bound determines the wire allocation bound. */
int snag_binary_wire_size(size_t decoded_size, size_t *out);
int snag_binary_wire_encode(struct snag_buf *out, const void *data, size_t size);
int snag_binary_wire_decode(struct snag_buf *out, const void *data, size_t size);
/* Decode just the first batch header, without interpreting it. Return 0 for a
 * complete header, 1 for insufficient input, -1 for malformed prefix/arguments.
 * Nonzero results preserve out. A caller still checks header fields/hash before
 * length-driven reads and validates the complete envelope/batch before adoption. */
int snag_binary_wire_header(const void *data, size_t size,
    unsigned char out[SNAG_BINARY_BATCH_HEADER_SIZE]);

#endif
