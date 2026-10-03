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
    {1u, "96f812e601b40c51a81152dc02d9aabf69ac74fffb603aa5590be08bc33c74b1"},
    {2u, "81d6ba94c9c2578a86cdc022330f146523de0877c907ab8163588e903b2b707e"},
    {3u, "f5506e1a44e29b49334883a4c6eb9da2e2e2f6ae9cd957742aada1c8cfe54fb3"},
    {4u, "ace7ad44878e673d2c25c976f2d5ab320ea7a619d68347905613abcee447ba8f"},
    {5u, "bb4b34015a0ce3d105dfdae14f6a16fe1954b4837ed8057b07bf86699bc65658"},
    {7u, "9e0726ace96952c00145404ea04528aaa403e2f37d2507a23f1c888bf8ef511c"},
    {8u, "850dd05e45faa20ad66e8a8121899f9ab76848e6eb18cca8743ca2183e0f5947"},
    {15u, "24ad554c110f4be7d8ab204862cb90bf618dcac829e0797ee2aac423d087dd03"},
    {16u, "cb18008399c9d6429ec933840d86c44d99f970065782b576dcaccde4ab3f068d"},
    {31u, "015fd71656970851c7e656535aff5c0276d590002fecee6e97c68622e28d1756"},
    {32u, "86136b745458ccf9dccd530b9f63566e7e71f08f3873518537ff81cf33a836de"},
    {63u, "45ffc8e7a8a3629d69412cb805635715edd2f813c9e01d8d20b6fd3255a71da1"},
    {64u, "340129cb390cf29d554e746b545a9e0c5a1b9f4f6662bc93771132b8377607c4"},
    {65u, "4c9665dbc5e4d591d46bcd0d5c8ab60425ce2ccca0c289fc17d38316dab49282"},
    {127u, "b18af7517d8e27762c460e5885eb776a19fd4ba4b4836c9ae0c737273328c160"},
    {128u, "b650272fbb7f78603993c299259cfaf83090aa5bc096f0963a5630efd806087d"},
    {129u, "edf0a828e8d7b2d266fb2b1336404eb95338006629da6a5c2d3b7aa2124cd593"},
    {255u, "405eb10d313a409c0e5f2ef602b12d5c93f33edd230ca3161a1a772e92dfa2e8"},
    {256u, "096cf0e930e5ae492925663e29bae5ba17fc0e341bfa83de9662ba6d4a9c9718"},
    {257u, "6bd55bf71e7aed8a81686697a05594047a49ef10be83e8abc0d548636cb14527"}
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
