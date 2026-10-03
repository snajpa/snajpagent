/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_FIXTURE_STORE_BINARY_H
#define SNAJPAGENT_FIXTURE_STORE_BINARY_H

#include "fs.h"
#include "store_binary_wire.h"

#include <errno.h>
#include <string.h>

/* Fixture buffers model actual journal bytes. Record views need separately
 * owned decoded scratch so their addresses never masquerade as file offsets. */
static inline int
binary_fixture_read(const void *data, size_t size, const struct snag_binary_anchor *before,
    struct snag_buf *scratch, struct snag_binary_batch *batch, struct snag_binary_anchor *after)
{
    const unsigned char *end = memchr(data, 0, size);
    if (!end) return snag_errno(EINVAL);
    snag_buf_reset(scratch);
    size_t length = (size_t)(end - (const unsigned char *)data) + 1u;
    if (snag_binary_wire_decode(scratch, data, length) < 0) {
        return -1;
    }
    return snag_binary_batch_decode(scratch->data, scratch->len, before, batch, after);
}

static inline int
binary_fixture_append(struct snag_buf *out, const struct snag_binary_anchor *before,
    const struct snag_binary_record *records, uint32_t count, uint64_t turns,
    struct snag_binary_anchor *after)
{
    struct snag_buf bytes = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_anchor next;
    int rc = snag_binary_batch_encode(&bytes, before, records, count, turns, &next);
    if (rc == 0) rc = snag_binary_wire_encode(out, bytes.data, bytes.len);
    if (rc == 0 && after) *after = next;
    snag_buf_free(&bytes);
    return rc;
}

static inline int
binary_fixture_write(int fd, const void *data, size_t size)
{
    struct snag_buf wire = {.max = SNAG_BINARY_WIRE_BATCH_MAX};
    int rc = snag_binary_wire_encode(&wire, data, size);
    if (rc == 0) rc = snag_write_full(fd, wire.data, wire.len);
    snag_buf_free(&wire);
    return rc;
}

#endif
