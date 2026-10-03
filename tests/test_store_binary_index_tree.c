/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_index.h"
#include "fs.h"
#include "store_binary_event.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void test_store_binary_index_tree(void);

static const struct {
    unsigned int count;
    const char *root;
} golden[] = {
    {0u, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {1u, "e7a9fb87dd09803a6cfb8db37892029c255f72acd39014813d9d432e186a4408"},
    {2u, "898f614fae5e34a0860f9388d6a675b120b3282394e53505802b650c665d7d51"},
    {3u, "45608c4a220da2182255b989319335357d77e70a39fdb545f209424b6d9d6dda"},
    {4u, "10c78ff4a82efc8816a1995f911cc3c977c370d2f5f9ceaabb2d1d4f8244f9ca"},
    {5u, "945fc0e3f3c27c53d84a0aea0d27c7fb4446f4acb01d9502f35c46f8b8c80c21"},
    {7u, "5f51fa4584f7c58f10c33c79f665eacafe57c295389d76341eef04bb1dd0dd36"},
    {8u, "85d2d887fa64d6787e1e9d82d9f6133bd7f78bb10ba7465b54d4ca240871bf61"},
    {15u, "3b06efe65b831f99714095d153e7a0c02b3422f826ce70bbaf7a3325c5ea7164"},
    {16u, "9de1bb1370d52286eeeeb3093cb9cb71b368b26a9625a7e4923a4096faa33d34"},
    {31u, "cd07d3212534d6e52a627adfd57573226ee5e7a3de0e9b41a730ac3c07e17cf6"},
    {32u, "165400526fd50b7fb54852343aadb09e281635f3af9b39f8a95b1b6691857981"},
    {63u, "e076761bace72bbce160ec3f7bde3e65f1f88f68dea0c41658bb790e4c184d97"},
    {64u, "469f8c9e234562a2c66ca73764d86ceb3d8e8a24759dff7fdac4e8944102cc5c"},
    {65u, "340e6824284582e4377f029d2c5c0c865cfd2151d9b0654354572823856ee040"},
    {127u, "b53442c6ee3286ca4fb1c94794ed016af68d5aa8acec4df7290503ec878b775d"},
    {128u, "04e4e668f8b4f74ff7bfe77ebbb9db1879ebb193dd93e35d5e8fffa8cfb3a756"},
    {129u, "42f0c7068c6c111a5778a6e01895e3d7f95c468d199929c7e7c7f23295d02d20"},
    {255u, "9dc9368801ab7ccc7b5af2bb6f9452f4f39f9b89b277f8e3c7d388606421bbfc"},
    {256u, "74073160d22617c3c3439f8747b17cfdb1fbc0de5657890d20a8ba1ec7414c8c"},
    {257u, "678847ea539a60ed349d40c0e76d18dfd379841a835b693335fe52849e78a764"}
};

static void
assert_hex(const unsigned char hash[32], const char *hex)
{
    const char *digits = "0123456789abcdef";
    assert(strlen(hex) == 64u);
    for (size_t i = 0u; i < 32u; ++i) {
        assert(hex[i * 2u] == digits[hash[i] >> 4u]);
        assert(hex[i * 2u + 1u] == digits[hash[i] & 15u]);
    }
}

static void
hash_bytes(const void *data, size_t size, unsigned char out[32])
{
    struct snag_sha256 hash;
    snag_sha256_init(&hash);
    snag_sha256_update(&hash, data, size);
    snag_sha256_final(&hash, out);
}

static struct snag_binary_index_entry
entry_at(uint64_t sequence)
{
    uint64_t batch = (sequence - 1u) / 3u;
    struct snag_binary_index_entry entry = {
        .sequence = sequence, .batch_offset = 96u + batch * 1024u,
        .record_offset = 112u + (uint32_t)((sequence - 1u) % 3u) * 32u,
        .kind = sequence % 3u == 2u ? SNAG_BINARY_TURN_STARTED : 0x8001u,
        .turn = (sequence + 1u) / 3u
    };
    unsigned char bytes[8];
    for (unsigned int i = 0u; i < 8u; ++i) {
        bytes[i] = (unsigned char)(batch >> (i * 8u));
    }
    hash_bytes(bytes, sizeof(bytes), entry.batch_digest);
    return entry;
}

static int
temporary_fd(void)
{
    char *path = snag_path_join(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
        "snag-index-tree-XXXXXX");
    assert(path);
    int fd = mkstemp(path);
    assert(fd >= 0 && !unlink(path));
    free(path);
    return fd;
}

/* Independently traverse the RFC definition rather than the append frontier.
 * The fixed roots above were generated separately with Python hashlib/struct. */
static void
reference_root(const unsigned char *entries, size_t count, unsigned char out[32])
{
    if (!count) {
        hash_bytes("", 0u, out);
        return;
    }
    struct snag_sha256 hash;
    snag_sha256_init(&hash);
    unsigned char tag = count == 1u ? 0u : 1u;
    snag_sha256_update(&hash, &tag, 1u);
    if (count == 1u) {
        snag_sha256_update(&hash, entries, SNAG_BINARY_INDEX_ENTRY_SIZE);
    } else {
        size_t split = 1u;
        while (split * 2u < count) split *= 2u;
        unsigned char child[32];
        reference_root(entries, split, child);
        snag_sha256_update(&hash, child, sizeof(child));
        reference_root(entries + split * SNAG_BINARY_INDEX_ENTRY_SIZE, count - split, child);
        snag_sha256_update(&hash, child, sizeof(child));
    }
    snag_sha256_final(&hash, out);
}

static void
same_entry(const struct snag_binary_identity *identity,
    const struct snag_binary_index_entry *left, const struct snag_binary_index_entry *right)
{
    unsigned char a[SNAG_BINARY_INDEX_ENTRY_SIZE];
    unsigned char b[SNAG_BINARY_INDEX_ENTRY_SIZE];
    assert(!snag_binary_index_entry_encode(a, identity, left));
    assert(!snag_binary_index_entry_encode(b, identity, right));
    assert(!memcmp(a, b, sizeof(a)));
}

static void
test_all_prefixes(void)
{
    struct snag_binary_identity identity = {.created_ms = 42u};
    identity.id[0] = 17u;
    struct snag_binary_index_tree tree = {0};
    unsigned char roots[258][32];
    unsigned char entries[257u * SNAG_BINARY_INDEX_ENTRY_SIZE];
    struct snag_buf image = {.max = 65536u};
    unsigned char header[SNAG_BINARY_INDEX_HEADER_SIZE];
    snag_binary_index_header_encode(header, &identity);
    assert(!snag_buf_append(&image, header, sizeof(header)));
    for (unsigned int n = 0u; n <= 257u; ++n) {
        if (n) {
            struct snag_binary_index_entry entry = entry_at(n);
            assert(!snag_binary_index_entry_encode(
                entries + (n - 1u) * SNAG_BINARY_INDEX_ENTRY_SIZE, &identity, &entry));
            assert(!snag_binary_index_tree_append(&image, &tree, &identity, &entry));
        }
        assert(tree.count == n);
        assert(!snag_binary_index_tree_root(&tree, roots[n]));
        unsigned char expected[32];
        reference_root(entries, n, expected);
        assert(!memcmp(expected, roots[n], sizeof(expected)));
        for (size_t i = 0u; i < sizeof(golden) / sizeof(golden[0]); ++i) {
            if (golden[i].count == n) assert_hex(roots[n], golden[i].root);
        }
        int64_t end;
        assert(!snag_binary_index_end(n, &end) && (size_t)end == image.len);
    }
    int fd = temporary_fd();
    assert(!snag_write_full(fd, image.data, image.len));
    assert(snag_seek(fd, 7, SEEK_SET) == 7);
    for (unsigned int n = 0u; n <= 257u; ++n) {
        struct snag_binary_index_tree loaded;
        assert(!snag_binary_index_tree_load(fd, &identity, n, roots[n], &loaded));
        assert(loaded.count == n);
        unsigned char root[32];
        assert(!snag_binary_index_tree_root(&loaded, root));
        assert(!memcmp(root, roots[n], sizeof(root)));
        for (uint64_t sequence = 1u; sequence <= n; ++sequence) {
            struct snag_binary_index_entry entry;
            struct snag_binary_index_entry expected = entry_at(sequence);
            assert(!snag_binary_index_read_verified(fd, &identity, n, roots[n], sequence, &entry));
            same_entry(&identity, &entry, &expected);
        }
        struct snag_binary_index_entry absent;
        memset(&absent, 0x5a, sizeof(absent));
        struct snag_binary_index_entry saved;
        memcpy(&saved, &absent, sizeof(saved));
        assert(snag_binary_index_read_verified(fd, &identity, n, roots[n], n + 1u, &absent) == 1);
        assert(!memcmp(&absent, &saved, sizeof(saved)));
        for (uint64_t turn = 1u; turn <= (n + 1u) / 3u; ++turn) {
            struct snag_binary_index_entry entry;
            assert(!snag_binary_index_turn_verified(fd, &identity, n, roots[n], turn, &entry));
            assert(entry.sequence == turn * 3u - 1u && entry.kind == SNAG_BINARY_TURN_STARTED);
        }
        if (n < 257u) {
            struct snag_buf appended = {.max = 4096u};
            struct snag_binary_index_entry next = entry_at(n + 1u);
            assert(!snag_binary_index_tree_append(&appended, &loaded, &identity, &next));
            assert(!snag_binary_index_tree_root(&loaded, root));
            assert(!memcmp(root, roots[n + 1u], sizeof(root)));
            int64_t start;
            assert(!snag_binary_index_end(n, &start));
            assert(!memcmp(appended.data, image.data + (size_t)start, appended.len));
            snag_buf_free(&appended);
        }
        assert(snag_seek(fd, 0, SEEK_CUR) == 7);
    }
    assert(!close(fd));
    snag_buf_free(&image);
}

static void
write_at(int fd, int64_t offset, const void *data, size_t size)
{
    assert(snag_seek(fd, offset, SEEK_SET) == offset);
    assert(!snag_write_full(fd, data, size));
    assert(snag_seek(fd, 7, SEEK_SET) == 7);
}

static void
test_corruptions(void)
{
    struct snag_binary_identity identity = {.created_ms = 42u};
    identity.id[0] = 17u;
    struct snag_binary_index_tree tree = {0};
    struct snag_buf image = {.max = 4096u};
    unsigned char header[SNAG_BINARY_INDEX_HEADER_SIZE];
    snag_binary_index_header_encode(header, &identity);
    assert(!snag_buf_append(&image, header, sizeof(header)));
    unsigned char roots[8][32];
    for (uint64_t sequence = 1u; sequence <= 7u; ++sequence) {
        struct snag_binary_index_entry entry = entry_at(sequence);
        assert(!snag_binary_index_tree_append(&image, &tree, &identity, &entry));
        assert(!snag_binary_index_tree_root(&tree, roots[sequence]));
    }
    int fd = temporary_fd();
    assert(!snag_write_full(fd, image.data, image.len));
    struct snag_binary_index_entry entry;
    memset(&entry, 0x5a, sizeof(entry));
    struct snag_binary_index_entry sentinel;
    memcpy(&sentinel, &entry, sizeof(entry));
    struct snag_binary_index_entry forged = entry_at(3u);
    ++forged.batch_offset;
    unsigned char encoded[SNAG_BINARY_INDEX_ENTRY_SIZE];
    assert(!snag_binary_index_entry_encode(encoded, &identity, &forged));
    int64_t offset;
    assert(!snag_binary_index_offset(3u, &offset));
    write_at(fd, offset, encoded, sizeof(encoded));
    struct snag_binary_index_entry hint;
    assert(!snag_binary_index_read_hint(fd, &identity, 3u, &hint));
    assert(hint.batch_offset == forged.batch_offset);
    assert(snag_binary_index_read_verified(fd, &identity, 7u, roots[7], 3u, &entry) < 0);
    assert(!memcmp(&sentinel, &entry, sizeof(entry)));
    write_at(fd, offset, image.data + (size_t)offset, sizeof(encoded));
    /* Change the persisted root of leaves 1..4. A proof for leaf 7 uses it.
     * Leaf 1 can still be verified from its unchanged siblings and trusted root;
     * the frontier loader must reject the damaged persisted peak. */
    assert(!snag_binary_index_offset(4u, &offset));
    offset += SNAG_BINARY_INDEX_ENTRY_SIZE + SNAG_BINARY_INDEX_HASH_SIZE;
    for (size_t byte = 0u; byte < SNAG_BINARY_INDEX_HASH_SIZE; ++byte) {
        unsigned char changed = image.data[(size_t)offset + byte] ^ 1u;
        write_at(fd, offset + (int64_t)byte, &changed, 1u);
        assert(snag_binary_index_read_verified(fd, &identity, 7u, roots[7], 7u, &entry) < 0);
        assert(!memcmp(&sentinel, &entry, sizeof(entry)));
        struct snag_binary_index_tree loaded;
        memset(&loaded, 0x5a, sizeof(loaded));
        struct snag_binary_index_tree saved = loaded;
        assert(snag_binary_index_tree_load(fd, &identity, 7u, roots[7], &loaded) < 0);
        assert(!memcmp(&saved, &loaded, sizeof(loaded)));
        assert(!snag_binary_index_read_verified(fd, &identity, 7u, roots[7], 1u, &hint));
        write_at(fd, offset + (int64_t)byte, image.data + (size_t)offset + byte, 1u);
    }
    unsigned char wrong[32];
    memcpy(wrong, roots[7], sizeof(wrong));
    wrong[0] ^= 1u;
    assert(snag_binary_index_read_verified(fd, &identity, 7u, wrong, 1u, &entry) < 0);
    assert(!memcmp(&sentinel, &entry, sizeof(entry)));
    assert(snag_binary_index_read_verified(fd, &identity, 6u, roots[7], 1u, &entry) < 0);
    struct snag_binary_identity other = identity;
    other.id[0]++;
    assert(snag_binary_index_read_verified(fd, &other, 7u, roots[7], 1u, &entry) < 0);
    assert(!memcmp(&sentinel, &entry, sizeof(entry)));
    assert(snag_seek(fd, 0, SEEK_CUR) == 7);
    int64_t end;
    assert(!snag_binary_index_end(4u, &end));
    assert(!snag_truncate(fd, end - 1));
    assert(snag_binary_index_read_verified(fd, &identity, 7u, roots[7], 7u, &entry) == 1);
    struct snag_binary_index_tree loaded;
    memset(&loaded, 0x5a, sizeof(loaded));
    struct snag_binary_index_tree kept = loaded;
    assert(snag_binary_index_tree_load(fd, &identity, 4u, roots[4], &loaded) == 1);
    assert(!memcmp(&kept, &loaded, sizeof(loaded)));
    assert(!snag_binary_index_read_verified(fd, &identity, 3u, roots[3], 1u, &hint));
    assert(!snag_truncate(fd, SNAG_BINARY_INDEX_HEADER_SIZE - 1u));
    assert(snag_binary_index_read_verified(fd, &identity, 3u, roots[3], 1u, &entry) == 1);
    assert(!memcmp(&sentinel, &entry, sizeof(entry)));
    assert(!close(fd));
    snag_buf_free(&image);
}

static void
test_failures(void)
{
    struct snag_binary_identity identity = {.created_ms = 42u};
    identity.id[0] = 17u;
    struct snag_binary_index_tree tree = {0};
    struct snag_binary_index_tree saved = tree;
    struct snag_binary_index_entry entry = entry_at(1u);
    struct snag_buf out = {.max = 8u};
    assert(!snag_buf_append(&out, "keep", 4u));
    assert(snag_binary_index_tree_append(&out, &tree, &identity, &entry) < 0);
    assert(out.len == 4u && !memcmp(out.data, "keep", 4u));
    assert(!memcmp(&saved, &tree, sizeof(tree)));
    entry.sequence = 2u;
    assert(snag_binary_index_tree_append(&out, &tree, &identity, &entry) < 0);
    entry = entry_at(1u);
    assert(snag_binary_index_tree_append(NULL, &tree, &identity, &entry) < 0);
    assert(snag_binary_index_tree_append(&out, NULL, &identity, &entry) < 0);
    assert(snag_binary_index_tree_append(&out, &tree, NULL, &entry) < 0);
    assert(snag_binary_index_tree_append(&out, &tree, &identity, NULL) < 0);
    unsigned char root[32];
    memset(root, 0x5a, sizeof(root));
    assert(snag_binary_index_tree_root(NULL, root) < 0);
    assert(snag_binary_index_tree_root(&tree, NULL) < 0);
    tree.count = UINT64_MAX;
    assert(snag_binary_index_tree_root(&tree, root) < 0 && errno == EOVERFLOW);
    for (size_t i = 0u; i < sizeof(root); ++i) assert(root[i] == 0x5a);
    assert(snag_binary_index_tree_append(&out, &tree, &identity, &entry) < 0);
    int64_t offset = -7;
    assert(snag_binary_index_end(UINT64_MAX, &offset) < 0 && offset == -7);
    assert(snag_binary_index_end(0u, NULL) < 0);
    assert(!snag_binary_index_end(0u, &offset) && offset == SNAG_BINARY_INDEX_HEADER_SIZE);
    int fd = temporary_fd();
    assert(snag_binary_index_tree_load(-1, &identity, 0u, root, &tree) < 0);
    assert(snag_binary_index_tree_load(fd, NULL, 0u, root, &tree) < 0);
    assert(snag_binary_index_tree_load(fd, &identity, 0u, NULL, &tree) < 0);
    assert(snag_binary_index_tree_load(fd, &identity, 0u, root, NULL) < 0);
    assert(snag_binary_index_tree_load(fd, &identity, UINT64_MAX, root, &tree) < 0);
    assert(snag_binary_index_read_verified(-1, &identity, 0u, root, 1u, &entry) < 0);
    assert(snag_binary_index_read_verified(fd, NULL, 0u, root, 1u, &entry) < 0);
    assert(snag_binary_index_read_verified(fd, &identity, 0u, NULL, 1u, &entry) < 0);
    assert(snag_binary_index_read_verified(fd, &identity, 0u, root, 0u, &entry) < 0);
    assert(snag_binary_index_read_verified(fd, &identity, 0u, root, 1u, NULL) < 0);
    assert(snag_binary_index_read_verified(fd, &identity, UINT64_MAX, root, 1u, &entry) < 0);
    assert(snag_binary_index_turn_verified(fd, &identity, 0u, root, 1u, &entry) == 1);
    assert(snag_binary_index_turn_verified(fd, &identity, 0u, root, 0u, &entry) < 0);
    assert(snag_binary_index_turn_verified(fd, &identity, UINT64_MAX, root, 1u, &entry) < 0);
    assert(!close(fd));
    snag_buf_free(&out);
}

void
test_store_binary_index_tree(void)
{
    test_all_prefixes();
    test_corruptions();
    test_failures();
}
