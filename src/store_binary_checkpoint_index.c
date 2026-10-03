/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_index.h"
#include "store_binary_wire.h"

#include <errno.h>
#include <string.h>

static void
put_le(unsigned char *out, uint64_t value, size_t width)
{
    for (size_t i = 0u; i < width; ++i) {
        out[i] = (unsigned char)value;
        value >>= 8u;
    }
}

static uint64_t
get_le(const unsigned char *bytes, size_t width)
{
    uint64_t value = 0u;
    for (size_t i = 0u; i < width; ++i) value |= (uint64_t)bytes[i] << (8u * i);
    return value;
}

static bool
boundary_valid(const struct snag_binary_anchor *boundary)
{
    if (!boundary || !boundary->next_seq || boundary->turns >= boundary->next_seq ||
        boundary->end < SNAG_BINARY_HEADER_SIZE || boundary->end > INT64_MAX) {
        return false;
    }
    if (boundary->end == SNAG_BINARY_HEADER_SIZE) {
        return boundary->next_seq == 1u && !boundary->turns && !boundary->previous;
    }
    return boundary->next_seq > 1u && boundary->previous >= SNAG_BINARY_HEADER_SIZE &&
        boundary->previous < boundary->end;
}

static bool
tree_valid(const struct snag_binary_index_tree *tree, const struct snag_binary_anchor *boundary)
{
    if (!tree || tree->count != boundary->next_seq - 1u) return false;
    const unsigned char zero[SNAG_BINARY_INDEX_HASH_SIZE] = {0};
    for (size_t i = 0u; i < 64u; ++i) {
        if (!(tree->count & (UINT64_C(1) << i)) && memcmp(tree->peaks[i], zero, sizeof(zero))) {
            return false;
        }
    }
    return true;
}

/* Entry encoding/decoding additionally checks its own complete shape/checksum.
 * These bounds do not establish membership or working-set completeness. */
static bool
entry_within(const struct snag_binary_index_entry *entry,
    const struct snag_binary_anchor *boundary)
{
    if (entry->sequence >= boundary->next_seq || entry->turn > boundary->turns ||
        entry->batch_offset > boundary->previous || entry->batch_offset >= boundary->end) {
        return false;
    }
    size_t minimum;
    return snag_binary_wire_size((size_t)entry->record_offset + SNAG_BINARY_RECORD_HEADER_SIZE +
        SNAG_BINARY_BATCH_FOOTER_SIZE, &minimum) == 0 &&
        minimum <= boundary->end - entry->batch_offset;
}

int
snag_binary_checkpoint_index_encode(struct snag_buf *out,
    const struct snag_binary_identity *identity, const struct snag_binary_anchor *boundary,
    const struct snag_binary_index_tree *tree, const struct snag_binary_index_entry *entries,
    size_t count)
{
    if (!out || !identity || !boundary_valid(boundary) || !tree_valid(tree, boundary) ||
        (!entries && count)) {
        return snag_errno(EINVAL);
    }
    const size_t header_size = SNAG_BINARY_CHECKPOINT_INDEX_HEADER_SIZE;
    if (count > (SIZE_MAX - header_size) / SNAG_BINARY_INDEX_ENTRY_SIZE) {
        return snag_errno(EOVERFLOW);
    }
    size_t size = header_size + count * SNAG_BINARY_INDEX_ENTRY_SIZE;
    struct snag_buf encoded = {.max = size};
    unsigned char header[SNAG_BINARY_CHECKPOINT_INDEX_HEADER_SIZE] = {0};
    put_le(header, 1u, 2u);
    put_le(header + 4u, 2u, 2u);
    put_le(header + 8u, tree->count, 8u);
    put_le(header + 16u, count, 8u);
    memcpy(header + 24u, tree->peaks, sizeof(tree->peaks));
    int rc = snag_buf_append(&encoded, header, sizeof(header));
    uint64_t previous = 0u;
    for (size_t i = 0u; rc == 0 && i < count; ++i) {
        unsigned char bytes[SNAG_BINARY_INDEX_ENTRY_SIZE];
        if (entries[i].sequence <= previous ||
            snag_binary_index_entry_encode(bytes, identity, &entries[i]) < 0 ||
            !entry_within(&entries[i], boundary)) {
            rc = snag_errno(EINVAL);
            break;
        }
        rc = snag_buf_append(&encoded, bytes, sizeof(bytes));
        previous = entries[i].sequence;
    }
    if (rc == 0) rc = snag_buf_append(out, encoded.data, encoded.len);
    snag_buf_free(&encoded);
    return rc;
}

int
snag_binary_checkpoint_index_decode(const void *data, size_t size,
    const struct snag_binary_identity *identity, const struct snag_binary_anchor *boundary,
    const unsigned char root[32], struct snag_binary_checkpoint_index *out)
{
    if (!data || !identity || !boundary_valid(boundary) || !root || !out ||
        size < SNAG_BINARY_CHECKPOINT_INDEX_HEADER_SIZE) {
        return snag_errno(EINVAL);
    }
    const unsigned char *bytes = data;
    size_t entry_size = size - SNAG_BINARY_CHECKPOINT_INDEX_HEADER_SIZE;
    if (get_le(bytes, 2u) != 1u || get_le(bytes + 2u, 2u) ||
        get_le(bytes + 4u, 2u) != 2u || get_le(bytes + 6u, 2u) ||
        entry_size % SNAG_BINARY_INDEX_ENTRY_SIZE ||
        get_le(bytes + 16u, 8u) != entry_size / SNAG_BINARY_INDEX_ENTRY_SIZE) {
        return snag_errno(EINVAL);
    }
    struct snag_binary_checkpoint_index decoded = {
        .identity = *identity, .boundary = *boundary,
        .tree = {.count = get_le(bytes + 8u, 8u)},
        .entries = bytes + SNAG_BINARY_CHECKPOINT_INDEX_HEADER_SIZE,
        .entry_count = entry_size / SNAG_BINARY_INDEX_ENTRY_SIZE
    };
    memcpy(decoded.tree.peaks, bytes + 24u, sizeof(decoded.tree.peaks));
    unsigned char computed[32];
    if (!tree_valid(&decoded.tree, boundary) ||
        snag_binary_index_tree_root(&decoded.tree, computed) < 0 || memcmp(computed, root, 32u)) {
        return snag_errno(EINVAL);
    }
    uint64_t previous = 0u;
    for (size_t i = 0u; i < decoded.entry_count; ++i) {
        const unsigned char *entry_bytes = decoded.entries + i * SNAG_BINARY_INDEX_ENTRY_SIZE;
        uint64_t sequence = get_le(entry_bytes, 8u);
        struct snag_binary_index_entry entry;
        if (sequence <= previous || snag_binary_index_entry_decode(entry_bytes,
                SNAG_BINARY_INDEX_ENTRY_SIZE, identity, sequence, &entry) < 0 ||
            !entry_within(&entry, boundary)) {
            return snag_errno(EINVAL);
        }
        previous = sequence;
    }
    *out = decoded;
    return 0;
}

int
snag_binary_checkpoint_index_find(const struct snag_binary_checkpoint_index *index,
    uint64_t sequence, struct snag_binary_index_entry *out)
{
    if (!index || !out || !sequence || (!index->entries && index->entry_count)) {
        return snag_errno(EINVAL);
    }
    size_t low = 0u, high = index->entry_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        const unsigned char *bytes = index->entries + middle * SNAG_BINARY_INDEX_ENTRY_SIZE;
        uint64_t found = get_le(bytes, 8u);
        if (found < sequence) low = middle + 1u;
        else high = middle;
    }
    if (low == index->entry_count) return 1;
    const unsigned char *bytes = index->entries + low * SNAG_BINARY_INDEX_ENTRY_SIZE;
    if (get_le(bytes, 8u) != sequence) return 1;
    return snag_binary_index_entry_decode(bytes, SNAG_BINARY_INDEX_ENTRY_SIZE,
        &index->identity, sequence, out);
}
