/* SPDX-License-Identifier: GPL-2.0-only */
#include "fixture_store_binary.h"
#include "store_binary_index.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void test_store_binary_checkpoint_index(void);

static void
check_digest(const void *data, size_t size, const char *hex)
{
    unsigned char bytes[32];
    struct snag_sha256 hash;
    snag_sha256_init(&hash);
    snag_sha256_update(&hash, data, size);
    snag_sha256_final(&hash, bytes);
    static const char digits[] = "0123456789abcdef";
    assert(strlen(hex) == 64u);
    for (size_t i = 0u; i < sizeof(bytes); ++i) {
        assert(hex[i * 2u] == digits[bytes[i] >> 4u]);
        assert(hex[i * 2u + 1u] == digits[bytes[i] & 15u]);
    }
}

static void
reject_decode(const void *data, size_t size, const struct snag_binary_identity *identity,
    const struct snag_binary_anchor *boundary, const unsigned char root[32])
{
    struct snag_binary_checkpoint_index out;
    unsigned char saved[sizeof(out)];
    memset(&out, 0x5a, sizeof(out));
    memcpy(saved, &out, sizeof(out));
    assert(snag_binary_checkpoint_index_decode(data, size, identity, boundary, root, &out) < 0);
    assert(!memcmp(saved, &out, sizeof(out)));
}

static void
test_metadata(void)
{
    struct snag_binary_identity identity = {.created_ms = 42u};
    identity.id[0] = 17u;
    struct snag_binary_anchor boundary = {.end = 4096u, .next_seq = 7u, .previous = 1376u};
    struct snag_binary_index_entry entries[6] = {0};
    struct snag_binary_index_tree tree = {0};
    for (size_t i = 0u; i < 6u; ++i) {
        entries[i] = (struct snag_binary_index_entry){.sequence = i + 1u,
            .batch_offset = SNAG_BINARY_HEADER_SIZE + i * 256u,
            .record_offset = SNAG_BINARY_BATCH_HEADER_SIZE, .kind = 77u};
        memset(entries[i].batch_digest, (int)i + 1, 32u);
        assert(!snag_binary_index_tree_append(NULL, &tree, &identity, &entries[i]));
    }
    struct snag_binary_index_entry active[3] = {entries[0], entries[3], entries[5]};
    struct snag_buf metadata = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_index_encode(&metadata, &identity, &boundary,
        &tree, active, 3u));
    assert(metadata.len == 2360u);
    check_digest(metadata.data, metadata.len,
        "3e877d0498741cdfcf7947b5e4476d2d019460c1620a971d6d52d13874900f15");
    unsigned char root[32];
    assert(!snag_binary_index_tree_root(&tree, root));
    struct snag_binary_checkpoint_index decoded;
    assert(!snag_binary_checkpoint_index_decode(metadata.data, metadata.len,
        &identity, &boundary, root, &decoded));
    assert(decoded.entry_count == 3u && !memcmp(&decoded.tree, &tree, sizeof(tree)));
    for (uint64_t sequence = 1u; sequence <= 8u; ++sequence) {
        struct snag_binary_index_entry entry, saved;
        memset(&entry, 0x5a, sizeof(entry));
        memcpy(&saved, &entry, sizeof(entry));
        int rc = snag_binary_checkpoint_index_find(&decoded, sequence, &entry);
        if (sequence == 1u || sequence == 4u || sequence == 6u) {
            assert(!rc && entry.sequence == sequence && entry.kind == 77u);
            assert(entry.batch_offset == entries[sequence - 1u].batch_offset);
            assert(!memcmp(entry.batch_digest, entries[sequence - 1u].batch_digest, 32u));
        } else {
            assert(rc == 1 && !memcmp(&entry, &saved, sizeof(entry)));
        }
    }
    for (size_t size = 0u; size < metadata.len; ++size) {
        reject_decode(metadata.data, size, &identity, &boundary, root);
    }
    for (size_t i = 0u; i < metadata.len; ++i) {
        metadata.data[i] ^= 1u;
        reject_decode(metadata.data, metadata.len, &identity, &boundary, root);
        metadata.data[i] ^= 1u;
    }
    struct snag_binary_identity other = identity;
    ++other.id[0];
    reject_decode(metadata.data, metadata.len, &other, &boundary, root);
    root[0] ^= 1u;
    reject_decode(metadata.data, metadata.len, &identity, &boundary, root);
    root[0] ^= 1u;
    struct snag_binary_anchor wrong = boundary;
    ++wrong.next_seq;
    reject_decode(metadata.data, metadata.len, &identity, &wrong, root);
    wrong = boundary;
    wrong.previous = entries[5].batch_offset - 1u;
    reject_decode(metadata.data, metadata.len, &identity, &wrong, root);
    wrong = boundary;
    wrong.end = entries[5].batch_offset + 1u;
    reject_decode(metadata.data, metadata.len, &identity, &wrong, root);
    assert(!snag_buf_putc(&metadata, 0));
    reject_decode(metadata.data, metadata.len, &identity, &boundary, root);
    --metadata.len;
    /* Individually checksummed entries must still be sorted and fit this prefix. */
    unsigned char *last = metadata.data + SNAG_BINARY_CHECKPOINT_INDEX_HEADER_SIZE +
        2u * SNAG_BINARY_INDEX_ENTRY_SIZE;
    for (unsigned fault = 0u; fault < 6u; ++fault) {
        struct snag_binary_index_entry changed = active[2];
        if (fault == 0u) changed = active[1];
        if (fault == 1u) changed = active[0];
        if (fault == 2u) changed.sequence = boundary.next_seq;
        if (fault == 3u) changed.batch_offset = boundary.previous + 1u;
        if (fault == 4u) changed.turn = 1u;
        if (fault == 5u) changed.record_offset = SNAG_BINARY_BATCH_MAX / 2u;
        assert(!snag_binary_index_entry_encode(last, &identity, &changed));
        reject_decode(metadata.data, metadata.len, &identity, &boundary, root);
    }
    assert(!snag_binary_index_entry_encode(last, &identity, &active[2]));
    /* A root authenticates the frontier, not an arbitrary location table.
     * Pin the whole image before accepting a structurally valid replacement.
     * Core/provider bytes here are opaque framing fixtures. */
    struct snag_binary_checkpoint_frame frame = {.identity = identity, .boundary = boundary,
        .generation = 1u,
        .core = {.version = 1u, .data = (const unsigned char *)"core", .size = 4u},
        .provider = {.version = 1u, .data = (const unsigned char *)"provider", .size = 8u},
        .access = {.version = 1u, .data = metadata.data, .size = metadata.len}};
    struct snag_buf image = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_frame_encode(&image, &frame));
    struct snag_binary_checkpoint_receipt receipt = {
        .generation = 1u, .image_size = image.len, .boundary = boundary};
    memcpy(receipt.image_digest, image.data + image.len - 32u, 32u);
    memcpy(receipt.index_root, root, 32u);
    struct snag_binary_index_entry forged = active[2];
    forged.batch_digest[0] ^= 1u;
    assert(!snag_binary_index_entry_encode(last, &identity, &forged));
    assert(!snag_binary_checkpoint_index_decode(metadata.data, metadata.len,
        &identity, &boundary, root, &decoded));
    snag_buf_reset(&image);
    assert(!snag_binary_checkpoint_frame_encode(&image, &frame));
    struct snag_binary_checkpoint_frame rejected;
    assert(snag_binary_checkpoint_frame_from_receipt(image.data, image.len,
        &identity, &receipt, &rejected) < 0);
    snag_buf_free(&image);
    assert(!snag_binary_index_entry_encode(last, &identity, &active[2]));
    /* Staging protects output and aliased inputs, including output reallocations. */
    struct snag_buf alias = {.max = SIZE_MAX};
    assert(!snag_buf_append(&alias, &tree, sizeof(tree)));
    assert(!snag_buf_append(&alias, active, sizeof(active)));
    size_t old = alias.len;
    assert(!snag_binary_checkpoint_index_encode(&alias, &identity, &boundary,
        (const struct snag_binary_index_tree *)alias.data,
        (const struct snag_binary_index_entry *)(alias.data + sizeof(tree)), 3u));
    assert(alias.len == old + metadata.len &&
        !memcmp(alias.data + old, metadata.data, metadata.len));
    assert(!memcmp(alias.data, &tree, sizeof(tree)) &&
        !memcmp(alias.data + sizeof(tree), active, sizeof(active)));
    snag_buf_free(&alias);
    struct snag_buf output = {.max = 4u};
    assert(!snag_buf_append(&output, "kept", 4u));
    assert(snag_binary_checkpoint_index_encode(&output, &identity, &boundary,
        &tree, active, 3u) < 0);
    output.max = SIZE_MAX;
    for (unsigned fault = 0u; fault < 4u; ++fault) {
        struct snag_binary_index_tree bad = tree;
        struct snag_binary_index_entry copy[3];
        memcpy(copy, active, sizeof(copy));
        if (fault == 0u) bad.peaks[0][0] = 1u;
        if (fault == 1u) ++bad.count;
        if (fault == 2u) copy[2] = copy[0];
        if (fault == 3u) copy[2].batch_offset = boundary.end;
        assert(snag_binary_checkpoint_index_encode(&output, &identity, &boundary,
            &bad, copy, 3u) < 0);
        assert(output.len == 4u && !memcmp(output.data, "kept", 4u));
    }
    assert(snag_binary_checkpoint_index_encode(&output, &identity, &boundary, &tree,
        active, SIZE_MAX / SNAG_BINARY_INDEX_ENTRY_SIZE) < 0 && errno == EOVERFLOW);
    assert(output.len == 4u && !memcmp(output.data, "kept", 4u));
    reject_decode(NULL, 0u, &identity, &boundary, root);
    reject_decode(metadata.data, metadata.len, NULL, &boundary, root);
    reject_decode(metadata.data, metadata.len, &identity, NULL, root);
    reject_decode(metadata.data, metadata.len, &identity, &boundary, NULL);
    assert(snag_binary_checkpoint_index_decode(metadata.data, metadata.len,
        &identity, &boundary, root, NULL) < 0);
    assert(snag_binary_checkpoint_index_find(NULL, 1u, &forged) < 0);
    assert(snag_binary_checkpoint_index_find(&decoded, 0u, &forged) < 0);
    assert(snag_binary_checkpoint_index_find(&decoded, 1u, NULL) < 0);
    snag_buf_free(&output);
    snag_buf_free(&metadata);
}

static void
test_extremes(void)
{
    struct snag_binary_identity identity = {.created_ms = 42u};
    unsigned char header[SNAG_BINARY_HEADER_SIZE], root[32];
    snag_binary_header_encode(header, &identity);
    struct snag_binary_anchor boundary;
    assert(!snag_binary_header_decode(header, sizeof(header), &identity, &boundary));
    struct snag_binary_index_tree tree = {0};
    assert(!snag_binary_index_tree_root(&tree, root));
    struct snag_buf bytes = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_index_encode(&bytes, &identity, &boundary, &tree, NULL, 0u));
    assert(bytes.len == SNAG_BINARY_CHECKPOINT_INDEX_HEADER_SIZE);
    struct snag_binary_checkpoint_index decoded;
    assert(!snag_binary_checkpoint_index_decode(bytes.data, bytes.len,
        &identity, &boundary, root, &decoded));
    struct snag_binary_index_entry entry = {.sequence = 1u};
    assert(snag_binary_checkpoint_index_find(&decoded, 1u, &entry) == 1 && entry.sequence == 1u);
    tree.count = UINT64_C(1) << 56u;
    memset(tree.peaks[56], 0x3a, 32u);
    boundary = (struct snag_binary_anchor){.end = (UINT64_C(1) << 62u) + 4096u,
        .previous = UINT64_C(1) << 62u, .next_seq = tree.count + 1u};
    entry = (struct snag_binary_index_entry){.sequence = tree.count,
        .batch_offset = boundary.previous, .record_offset = SNAG_BINARY_BATCH_HEADER_SIZE,
        .kind = 77u};
    memset(entry.batch_digest, 0x22, 32u);
    int64_t end;
    assert(snag_binary_index_end(tree.count, &end) < 0 && errno == EOVERFLOW);
    snag_buf_reset(&bytes);
    assert(!snag_binary_checkpoint_index_encode(&bytes, &identity, &boundary, &tree, &entry, 1u));
    assert(!snag_binary_index_tree_root(&tree, root));
    assert(!snag_binary_checkpoint_index_decode(bytes.data, bytes.len,
        &identity, &boundary, root, &decoded));
    struct snag_binary_index_entry found;
    assert(!snag_binary_checkpoint_index_find(&decoded, tree.count, &found));
    assert(found.sequence == tree.count && found.batch_offset == entry.batch_offset);
    assert(!memcmp(&decoded.tree, &tree, sizeof(tree)));
    snag_buf_free(&bytes);
}

static void
test_direct_reads(void)
{
    char *path = snag_path_join(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
        "snag-checkpoint-index-XXXXXX");
    assert(path);
    int fd = mkstemp(path);
    assert(fd >= 0 && !unlink(path));
    free(path);
    struct snag_binary_identity identity = {.created_ms = 42u};
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    snag_binary_header_encode(header, &identity);
    struct snag_binary_anchor before;
    assert(!snag_binary_header_decode(header, sizeof(header), &identity, &before));
    assert(!snag_write_full(fd, header, sizeof(header)));
    struct snag_binary_anchor anchors[7] = {before};
    struct snag_buf raw = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_buf flat = {.max = SNAG_BINARY_INDEX_BATCH_MAX};
    struct snag_binary_index_tree tree = {0};
    struct snag_binary_index_entry active[3];
    size_t count = 0u;
    for (uint64_t sequence = 1u; sequence <= 6u; ++sequence) {
        unsigned char payload[3] = {(unsigned char)sequence, 0u, 111u};
        struct snag_binary_record record = {.kind = 77u, .version = 1u,
            .payload = payload, .size = sizeof(payload)};
        struct snag_binary_anchor after;
        snag_buf_reset(&raw);
        snag_buf_reset(&flat);
        assert(!snag_binary_batch_encode(&raw, &before, &record, 1u, 0u, &after));
        assert(!binary_fixture_write(fd, raw.data, raw.len));
        assert(!snag_binary_index_tree_append_batch(NULL, &tree, &identity,
            &before, &after, raw.data, raw.len));
        assert(!snag_binary_index_append_batch(&flat, &identity, &before, &after,
            raw.data, raw.len));
        if (sequence == 1u || sequence == 4u || sequence == 6u) {
            assert(!snag_binary_index_entry_decode(flat.data, flat.len,
                &identity, sequence, &active[count++]));
        }
        before = after;
        anchors[sequence] = after;
    }
    assert(count == 3u);
    struct snag_buf metadata = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_index_encode(&metadata, &identity, &before,
        &tree, active, count));
    unsigned char root[32];
    assert(!snag_binary_index_tree_root(&tree, root));
    struct snag_binary_checkpoint_index decoded;
    assert(!snag_binary_checkpoint_index_decode(metadata.data, metadata.len,
        &identity, &before, root, &decoded));
    assert(snag_seek(fd, 13, SEEK_SET) == 13);
    for (size_t i = 0u; i < count; ++i) {
        struct snag_binary_index_entry entry;
        struct snag_binary_record record;
        assert(!snag_binary_checkpoint_index_find(&decoded, active[i].sequence, &entry));
        assert(!snag_binary_index_load_record(fd, &before, &entry, &raw, &record));
        assert(record.size == 3u && record.payload[0] == active[i].sequence &&
            !record.payload[1] && record.payload[2] == 111u);
        assert(snag_seek(fd, 0, SEEK_CUR) == 13);
    }
    struct snag_binary_batch batch;
    struct snag_binary_anchor previous;
    for (size_t i = 0u; i < count; ++i) {
        assert(!snag_binary_checkpoint_batch_find(fd, &before, &decoded, active[i].sequence,
            &raw, &batch, &previous));
        assert(batch.first_seq == active[i].sequence && previous.next_seq == active[i].sequence);
    }
    assert(!snag_binary_checkpoint_batch_find(fd, &anchors[1], &decoded, 1u,
        &raw, &batch, &previous));
    batch = (struct snag_binary_batch){.first_seq = 999u};
    previous = (struct snag_binary_anchor){.end = 99u};
    unsigned char saved_batch[sizeof(batch)], saved_before[sizeof(previous)];
    memcpy(saved_batch, &batch, sizeof(batch));
    memcpy(saved_before, &previous, sizeof(previous));
    assert(snag_binary_checkpoint_batch_find(fd, &before, &decoded, 2u,
        &raw, &batch, &previous) < 0 && errno == ENOENT);
    assert(!memcmp(saved_batch, &batch, sizeof(batch)) &&
        !memcmp(saved_before, &previous, sizeof(previous)));
    struct snag_binary_record added = {.kind = 77u, .version = 1u,
        .payload = (const unsigned char *)"later", .size = 5u};
    struct snag_binary_anchor later;
    snag_buf_reset(&raw);
    assert(!snag_binary_batch_encode(&raw, &before, &added, 1u, 0u, &later));
    assert(snag_seek(fd, (int64_t)before.end, SEEK_SET) == (int64_t)before.end);
    assert(!binary_fixture_write(fd, raw.data, raw.len));
    assert(snag_seek(fd, 13, SEEK_SET) == 13);
    assert(!snag_binary_checkpoint_batch_find(fd, &later, &decoded, 7u,
        &raw, &batch, &previous));
    assert(batch.first_seq == 7u && previous.end == before.end);
    /* Old unrelated damage blocks a lifetime walk, not pinned direct lookup
     * or lookup in the newer bounded suffix. Missing old entries still fail. */
    int64_t damaged = (int64_t)anchors[1].end + 20;
    unsigned char byte;
    assert(snag_pread(fd, &byte, 1u, damaged) == 1);
    byte ^= 1u;
    assert(snag_seek(fd, damaged, SEEK_SET) == damaged);
    assert(!snag_write_full(fd, &byte, 1u));
    assert(snag_seek(fd, 13, SEEK_SET) == 13);
    assert(!snag_binary_checkpoint_batch_find(fd, &later, &decoded, 1u,
        &raw, &batch, &previous));
    assert(!snag_binary_checkpoint_batch_find(fd, &later, &decoded, 7u,
        &raw, &batch, &previous));
    assert(snag_binary_checkpoint_batch_find(fd, &later, NULL, 1u,
        &raw, &batch, &previous) < 0);
    assert(snag_binary_checkpoint_batch_find(fd, &later, &decoded, 2u,
        &raw, &batch, &previous) < 0 && errno == ENOENT);
    struct snag_binary_checkpoint_index invalid = {0};
    assert(snag_binary_checkpoint_batch_find(fd, &later, &invalid, 7u,
        &raw, &batch, &previous) < 0 && errno == EINVAL);
    assert(snag_seek(fd, 0, SEEK_CUR) == 13);
    snag_buf_free(&metadata);
    snag_buf_free(&raw);
    snag_buf_free(&flat);
    assert(!close(fd));
}

void
test_store_binary_checkpoint_index(void)
{
    test_metadata();
    test_extremes();
    test_direct_reads();
}
