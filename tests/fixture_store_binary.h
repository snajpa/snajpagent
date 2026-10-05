/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_FIXTURE_STORE_BINARY_H
#define SNAJPAGENT_FIXTURE_STORE_BINARY_H

#include "fs.h"
#include "store_binary_index.h"
#include "store_binary_wire.h"

#include <assert.h>
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

/* Independently build dense fixture membership from canonical prefix bytes.
 * Production working-set closure selection is deliberately outside this oracle. */
static inline void
binary_fixture_access(int fd, const struct snag_binary_anchor *through,
    struct snag_buf *bytes, struct snag_binary_checkpoint_index *out)
{
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    assert(snag_pread(fd, header, sizeof(header), 0) == sizeof(header));
    struct snag_binary_identity identity;
    struct snag_binary_anchor cursor;
    assert(!snag_binary_header_decode(header, sizeof(header), &identity, &cursor));
    struct snag_binary_index_tree tree = {0};
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_buf flat = {.max = SNAG_BINARY_INDEX_BATCH_MAX};
    struct snag_buf entries = {.max = SIZE_MAX};
    while (cursor.end < through->end) {
        struct snag_binary_batch batch;
        struct snag_binary_anchor after;
        assert(!snag_binary_batch_read(fd, through->end, &cursor, &scratch, &batch, &after));
        assert(after.end <= through->end);
        assert(!snag_binary_index_tree_append_batch(NULL, &tree, &identity,
            &cursor, &after, batch.data, batch.size));
        snag_buf_reset(&flat);
        assert(!snag_binary_index_append_batch(&flat, &identity, &cursor, &after,
            batch.data, batch.size));
        for (uint64_t sequence = cursor.next_seq; sequence < after.next_seq; ++sequence) {
            struct snag_binary_index_entry entry;
            size_t offset = (size_t)(sequence - cursor.next_seq) * SNAG_BINARY_INDEX_ENTRY_SIZE;
            assert(!snag_binary_index_entry_decode(flat.data + offset,
                SNAG_BINARY_INDEX_ENTRY_SIZE, &identity, sequence, &entry));
            assert(!snag_buf_append(&entries, &entry, sizeof(entry)));
        }
        cursor = after;
    }
    assert(cursor.end == through->end && cursor.next_seq == through->next_seq &&
        cursor.turns == through->turns && cursor.previous == through->previous &&
        !memcmp(cursor.digest, through->digest, sizeof(cursor.digest)));
    assert(!snag_binary_checkpoint_index_encode(bytes, &identity, through, &tree,
        (const struct snag_binary_index_entry *)entries.data,
        entries.len / sizeof(struct snag_binary_index_entry)));
    unsigned char root[32];
    assert(!snag_binary_index_tree_root(&tree, root));
    assert(!snag_binary_checkpoint_index_decode(bytes->data, bytes->len,
        &identity, through, root, out));
    snag_buf_free(&entries);
    snag_buf_free(&flat);
    snag_buf_free(&scratch);
}

/* A structurally valid table with one mandatory old point omitted. */
static inline void
binary_fixture_access_omit(const struct snag_binary_checkpoint_index *source,
    uint64_t sequence, struct snag_buf *bytes, struct snag_binary_checkpoint_index *out)
{
    assert(sequence && sequence <= source->entry_count);
    size_t before = (size_t)(sequence - 1u) * SNAG_BINARY_INDEX_ENTRY_SIZE;
    size_t after = (size_t)sequence * SNAG_BINARY_INDEX_ENTRY_SIZE;
    size_t size = source->entry_count * SNAG_BINARY_INDEX_ENTRY_SIZE;
    assert(!snag_buf_append(bytes, source->entries, before));
    assert(!snag_buf_append(bytes, source->entries + after, size - after));
    *out = *source;
    out->entries = (const unsigned char *)bytes->data;
    --out->entry_count;
}

#endif
