/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_index.h"
#include "fs.h"
#include "store_binary_event.h"

#include <errno.h>
#include <string.h>

static void
leaf_hash(const unsigned char entry[SNAG_BINARY_INDEX_ENTRY_SIZE], unsigned char out[32])
{
    struct snag_sha256 hash;
    const unsigned char tag = 0u;
    snag_sha256_init(&hash);
    snag_sha256_update(&hash, &tag, 1u);
    snag_sha256_update(&hash, entry, SNAG_BINARY_INDEX_ENTRY_SIZE);
    snag_sha256_final(&hash, out);
}

static void
parent_hash(const unsigned char left[32], const unsigned char right[32], unsigned char out[32])
{
    struct snag_sha256 hash;
    const unsigned char tag = 1u;
    snag_sha256_init(&hash);
    snag_sha256_update(&hash, &tag, 1u);
    snag_sha256_update(&hash, left, 32u);
    snag_sha256_update(&hash, right, 32u);
    snag_sha256_final(&hash, out);
}

int
snag_binary_index_tree_root(const struct snag_binary_index_tree *tree, unsigned char out[32])
{
    if (!tree || !out) return snag_errno(EINVAL);
    unsigned char root[32];
    bool found = false;
    for (unsigned int level = 0u; level < 64u; ++level) {
        if (!(tree->count & (UINT64_C(1) << level))) continue;
        if (found) {
            parent_hash(tree->peaks[level], root, root);
        } else {
            memcpy(root, tree->peaks[level], sizeof(root));
            found = true;
        }
    }
    if (!found) {
        struct snag_sha256 hash;
        snag_sha256_init(&hash);
        snag_sha256_final(&hash, root);
    }
    memcpy(out, root, sizeof(root));
    return 0;
}

/* Callers stage both the bytes and frontier until the whole append succeeds. */
static int
append_leaf(struct snag_buf *out, struct snag_binary_index_tree *tree,
    const unsigned char entry[SNAG_BINARY_INDEX_ENTRY_SIZE])
{
    if (out && snag_buf_append(out, entry, SNAG_BINARY_INDEX_ENTRY_SIZE) < 0) {
        return -1;
    }
    unsigned char root[32];
    leaf_hash(entry, root);
    unsigned int level = 0u;
    for (uint64_t carried = tree->count; carried & 1u; carried >>= 1u) {
        parent_hash(tree->peaks[level], root, root);
        if (out && snag_buf_append(out, root, sizeof(root)) < 0) return -1;
        memset(tree->peaks[level], 0, sizeof(tree->peaks[level]));
        ++level;
    }
    memcpy(tree->peaks[level], root, sizeof(root));
    ++tree->count;
    return 0;
}

int
snag_binary_index_tree_append(struct snag_buf *out, struct snag_binary_index_tree *tree,
    const struct snag_binary_identity *identity, const struct snag_binary_index_entry *entry)
{
    if (!tree || !identity || !entry || tree->count == UINT64_MAX ||
        entry->sequence != tree->count + 1u) {
        return snag_errno(EINVAL);
    }
    int64_t end;
    if (out && snag_binary_index_end(entry->sequence, &end) < 0) return -1;
    unsigned char leaf[SNAG_BINARY_INDEX_ENTRY_SIZE];
    if (snag_binary_index_entry_encode(leaf, identity, entry) < 0) return -1;
    unsigned char storage[SNAG_BINARY_INDEX_ENTRY_SIZE + 64u * SNAG_BINARY_INDEX_HASH_SIZE];
    struct snag_buf staged = {.data = storage, .cap = sizeof(storage), .max = sizeof(storage)};
    struct snag_binary_index_tree next = *tree;
    if (append_leaf(out ? &staged : NULL, &next, leaf) < 0 ||
        (out && snag_buf_append(out, staged.data, staged.len) < 0)) {
        return -1;
    }
    *tree = next;
    return 0;
}

static int
read_node(int fd, const struct snag_binary_identity *identity, uint64_t last,
    unsigned int level, unsigned char out[32])
{
    int64_t offset;
    if (snag_binary_index_offset(last, &offset) < 0) return -1;
    /* Decomposition supplies a last sequence divisible by 2^level. Its complete
     * prefix bounds the parent immediately following that leaf. */
    if (level) {
        offset += SNAG_BINARY_INDEX_ENTRY_SIZE + (level - 1u) * SNAG_BINARY_INDEX_HASH_SIZE;
    }
    unsigned char bytes[SNAG_BINARY_INDEX_ENTRY_SIZE];
    size_t size = level ? SNAG_BINARY_INDEX_HASH_SIZE : SNAG_BINARY_INDEX_ENTRY_SIZE;
    size_t used = 0u;
    while (used < size) {
        ssize_t count = snag_pread(fd, bytes + used, size - used, offset + (int64_t)used);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return -1;
        if (!count) return 1;
        used += (size_t)count;
    }
    if (level) {
        memcpy(out, bytes, SNAG_BINARY_INDEX_HASH_SIZE);
    } else {
        struct snag_binary_index_entry entry;
        if (snag_binary_index_entry_decode(bytes, size, identity, last, &entry) < 0) {
            return -1;
        }
        leaf_hash(bytes, out);
    }
    return 0;
}

int
snag_binary_index_tree_load(int fd, const struct snag_binary_identity *identity, uint64_t count,
    const unsigned char root[32], struct snag_binary_index_tree *out)
{
    if (fd < 0 || !identity || !root || !out) return snag_errno(EINVAL);
    int64_t end;
    if (snag_binary_index_end(count, &end) < 0) return -1;
    int rc = snag_binary_index_header_read(fd, identity);
    if (rc) return rc;
    struct snag_binary_index_tree tree = {.count = count};
    uint64_t last = 0u;
    for (unsigned int level = 64u; level-- > 0u;) {
        uint64_t width = UINT64_C(1) << level;
        if (!(count & width)) continue;
        last += width;
        rc = read_node(fd, identity, last, level, tree.peaks[level]);
        if (rc) return rc;
    }
    unsigned char actual[32];
    if (snag_binary_index_tree_root(&tree, actual) < 0) return -1;
    if (memcmp(root, actual, sizeof(actual))) return snag_errno(EINVAL);
    *out = tree;
    return 0;
}

/* Only the target's path and the logarithmic forest frontier are expanded.
 * Every complete sibling subtree is read as one persisted root. Depth is bounded
 * by the sequence width; no record table or lifetime-sized proof is allocated. */
static int
range_hash(int fd, const struct snag_binary_identity *identity, uint64_t first,
    uint64_t count, uint64_t target, const unsigned char leaf[32], unsigned char out[32])
{
    if (count == 1u && first == target) {
        memcpy(out, leaf, 32u);
        return 0;
    }
    if (!(count & (count - 1u)) && (target < first || target - first >= count)) {
        unsigned int level = 0u;
        for (uint64_t width = count; width > 1u; width >>= 1u) ++level;
        return read_node(fd, identity, first + count, level, out);
    }
    uint64_t split = 1u;
    while (split <= (count - 1u) / 2u) split <<= 1u;
    unsigned char left[32];
    unsigned char right[32];
    int rc = range_hash(fd, identity, first, split, target, leaf, left);
    if (rc) return rc;
    rc = range_hash(fd, identity, first + split, count - split, target, leaf, right);
    if (rc) return rc;
    parent_hash(left, right, out);
    return 0;
}

int
snag_binary_index_read_verified(int fd, const struct snag_binary_identity *identity,
    uint64_t count, const unsigned char root[32], uint64_t sequence,
    struct snag_binary_index_entry *out)
{
    if (fd < 0 || !identity || !root || !sequence || !out) {
        return snag_errno(EINVAL);
    }
    int64_t end;
    if (snag_binary_index_end(count, &end) < 0) return -1;
    if (sequence > count) return 1;
    struct snag_binary_index_entry entry;
    int rc = snag_binary_index_read_hint(fd, identity, sequence, &entry);
    if (rc) return rc;
    unsigned char encoded[SNAG_BINARY_INDEX_ENTRY_SIZE];
    if (snag_binary_index_entry_encode(encoded, identity, &entry) < 0) {
        return -1;
    }
    unsigned char leaf[32];
    unsigned char actual[32];
    leaf_hash(encoded, leaf);
    rc = range_hash(fd, identity, 0u, count, sequence - 1u, leaf, actual);
    if (rc) return rc;
    if (memcmp(root, actual, sizeof(actual))) return snag_errno(EINVAL);
    *out = entry;
    return 0;
}

int
snag_binary_index_turn_verified(int fd, const struct snag_binary_identity *identity,
    uint64_t count, const unsigned char root[32], uint64_t turn,
    struct snag_binary_index_entry *out)
{
    if (fd < 0 || !identity || !root || !turn || !out) {
        return snag_errno(EINVAL);
    }
    int64_t end;
    if (snag_binary_index_end(count, &end) < 0) return -1;
    uint64_t low = 1u;
    uint64_t high = count + 1u;
    while (low < high) {
        uint64_t middle = low + (high - low) / 2u;
        struct snag_binary_index_entry entry;
        int rc = snag_binary_index_read_verified(fd, identity, count, root, middle, &entry);
        if (rc) return rc;
        if (entry.turn < turn) low = middle + 1u;
        else high = middle;
    }
    if (low > count) return 1;
    struct snag_binary_index_entry entry;
    int rc = snag_binary_index_read_verified(fd, identity, count, root, low, &entry);
    if (rc) return rc;
    if (entry.turn != turn) return 1;
    if (entry.kind != SNAG_BINARY_TURN_STARTED) return snag_errno(EINVAL);
    *out = entry;
    return 0;
}
