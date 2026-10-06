/* SPDX-License-Identifier: GPL-2.0-only */
#include "fixture_store_binary.h"
#include "store_binary_index.h"
#include "store_binary_event.h"

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
    struct snag_buf copied = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_index_copy(&copied, &decoded));
    assert(copied.len == metadata.len && !memcmp(copied.data, metadata.data, metadata.len));
    assert(!snag_binary_checkpoint_index_copy(&copied, &decoded));
    assert(copied.len == 2u * metadata.len &&
        !memcmp(copied.data + metadata.len, metadata.data, metadata.len));
    size_t saved_len = copied.len;
    struct snag_binary_checkpoint_index bad_copy = decoded;
    ++bad_copy.boundary.next_seq;
    assert(snag_binary_checkpoint_index_copy(&copied, &bad_copy) < 0 && copied.len == saved_len);
    bad_copy = decoded;
    bad_copy.entries = NULL;
    assert(snag_binary_checkpoint_index_copy(&copied, &bad_copy) < 0 && copied.len == saved_len);
    bad_copy = decoded;
    bad_copy.entry_count = SIZE_MAX;
    assert(snag_binary_checkpoint_index_copy(&copied, &bad_copy) < 0 &&
        errno == EOVERFLOW && copied.len == saved_len);
    /* Borrowed inputs survive a destination reallocation during the atomic append. */
    struct snag_binary_checkpoint_index borrowed;
    assert(!snag_binary_checkpoint_index_decode(copied.data, metadata.len,
        &identity, &boundary, root, &borrowed));
    assert(!snag_binary_checkpoint_index_copy(&copied, &borrowed));
    assert(copied.len == 3u * metadata.len &&
        !memcmp(copied.data + 2u * metadata.len, metadata.data, metadata.len));
    snag_buf_free(&copied);
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

struct range_visit {
    uint64_t sequences[9]; /* The largest journal fixture below has nine records. */
    size_t count;
    unsigned calls, stop;
    uint64_t fail_at;
    int failure;
};

static int
collect_range(void *opaque, const struct snag_binary_record *record, uint64_t sequence)
{
    struct range_visit *visit = opaque;
    assert(visit->count < sizeof(visit->sequences) / sizeof(*visit->sequences));
    if (visit->count) assert(sequence > visit->sequences[visit->count - 1u]);
    assert(record->kind == 77u || record->kind == SNAG_BINARY_TURN_STARTED);
    if (record->kind == 77u) {
        assert(record->size == 3u || record->size == 5u);
        if (record->size == 3u) assert(record->payload[0] == sequence && !record->payload[1]);
        else assert(sequence == 7u && !memcmp(record->payload, "later", 5u));
    }
    visit->sequences[visit->count++] = sequence;
    if (sequence == visit->fail_at) {
        errno = EACCES;
        return visit->failure;
    }
    return 0;
}

static bool
cancel_range(void *opaque)
{
    struct range_visit *visit = opaque;
    return ++visit->calls == visit->stop;
}

static void
check_ranges(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_index *access, bool grouped)
{
    for (uint64_t first = 1u; first <= through->next_seq; ++first) {
        for (uint64_t end = first; end <= through->next_seq; ++end) {
            for (unsigned mode = 0u; mode < 2u; ++mode) {
                struct range_visit visit = {0};
                assert(!snag_binary_checkpoint_records_read(fd, through, mode ? access : NULL,
                    first, end, collect_range, NULL, &visit));
                size_t count = 0u;
                for (uint64_t sequence = first; sequence < end; ++sequence) {
                    bool listed = grouped ? sequence == 2u || sequence == 3u ||
                        sequence == 5u || sequence == 6u : sequence == 1u ||
                        sequence == 4u || sequence == 6u;
                    if (!mode || listed || sequence >= access->boundary.next_seq) {
                        assert(count < visit.count && visit.sequences[count++] == sequence);
                    }
                }
                assert(count == visit.count);
                assert(snag_seek(fd, 0, SEEK_CUR) == 13);
            }
        }
    }
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
    check_ranges(fd, &later, &decoded, false);
    for (unsigned mode = 0u; mode < 2u; ++mode) {
        struct range_visit visit = {0};
        assert(!snag_binary_checkpoint_records_read(fd, &later, mode ? &decoded : NULL,
            1u, later.next_seq, collect_range, cancel_range, &visit));
        unsigned checks = visit.calls;
        for (unsigned stop = 1u; stop <= checks; ++stop) {
            visit = (struct range_visit){.stop = stop};
            assert(snag_binary_checkpoint_records_read(fd, &later, mode ? &decoded : NULL,
                1u, later.next_seq, collect_range, cancel_range, &visit) < 0);
            assert(errno == ECANCELED && visit.calls == stop);
        }
        for (int failure = -1; failure <= 1; failure += 2) {
            visit = (struct range_visit){.fail_at = 4u, .failure = failure};
            assert(snag_binary_checkpoint_records_read(fd, &later, mode ? &decoded : NULL,
                1u, later.next_seq, collect_range, NULL, &visit) < 0);
            assert(errno == (failure < 0 ? EACCES : EINVAL));
        }
        assert(snag_seek(fd, 0, SEEK_CUR) == 13);
    }
    struct range_visit visit = {0};
    assert(!snag_binary_checkpoint_records_read(fd, &anchors[1], &decoded,
        1u, 2u, collect_range, NULL, &visit));
    assert(visit.count == 1u && visit.sequences[0] == 1u);
    struct snag_buf empty_bytes = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_index_encode(&empty_bytes, &identity, &before,
        &tree, NULL, 0u));
    struct snag_binary_checkpoint_index empty;
    assert(!snag_binary_checkpoint_index_decode(empty_bytes.data, empty_bytes.len,
        &identity, &before, root, &empty));
    visit = (struct range_visit){0};
    assert(!snag_binary_checkpoint_records_read(fd, &later, &empty,
        1u, later.next_seq, collect_range, NULL, &visit));
    assert(visit.count == 1u && visit.sequences[0] == 7u);
    snag_buf_free(&empty_bytes);
    for (unsigned fault = 0u; fault < 6u; ++fault) {
        visit = (struct range_visit){0};
        assert(snag_binary_checkpoint_records_read(fault == 0u ? -1 : fd,
            fault == 1u ? NULL : &later, &decoded, fault == 2u ? 0u : 7u,
            fault == 3u ? 6u : fault == 4u ? later.next_seq + 1u : later.next_seq,
            fault == 5u ? NULL : collect_range, NULL, &visit) < 0 && errno == EINVAL);
        assert(!visit.count);
    }
    int closed = dup(fd);
    assert(closed >= 0 && !close(closed));
    assert(snag_binary_checkpoint_records_read(closed, &later, &decoded,
        1u, later.next_seq, collect_range, NULL, &visit) < 0 && errno == EBADF);
    assert(!visit.count);
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
    visit = (struct range_visit){0};
    assert(!snag_binary_checkpoint_records_read(fd, &later, &decoded,
        1u, later.next_seq, collect_range, NULL, &visit));
    assert(visit.count == 4u && visit.sequences[0] == 1u && visit.sequences[1] == 4u &&
        visit.sequences[2] == 6u && visit.sequences[3] == 7u);
    visit = (struct range_visit){0};
    assert(snag_binary_checkpoint_records_read(fd, &later, NULL,
        1u, later.next_seq, collect_range, NULL, &visit) < 0 && !visit.count);
    struct snag_binary_checkpoint_index invalid = {0};
    assert(snag_binary_checkpoint_batch_find(fd, &later, &invalid, 7u,
        &raw, &batch, &previous) < 0 && errno == EINVAL);
    assert(snag_binary_checkpoint_records_read(fd, &later, &invalid,
        1u, later.next_seq, collect_range, NULL, &visit) < 0 && errno == EINVAL);
    assert(snag_seek(fd, 0, SEEK_CUR) == 13);
    snag_buf_free(&metadata);
    snag_buf_free(&raw);
    snag_buf_free(&flat);
    assert(!close(fd));
}

static void
range_turn(struct snag_buf *out, uint64_t number)
{
    static const unsigned char empty[4];
    struct snag_binary_event event = {.kind = SNAG_BINARY_TURN_STARTED};
    event.data.started.number = number;
    event.data.started.id[0] = (unsigned char)number;
    event.data.started.cwd = (struct snag_binary_text){(const unsigned char *)"/w", 2u};
    event.data.started.text = (struct snag_binary_text){(const unsigned char *)"range", 5u};
    event.data.started.instructions = (struct snag_binary_instructions){empty, sizeof(empty)};
    event.data.started.config.selection = (struct snag_binary_selection){
        {(const unsigned char *)"p", 1u}, {(const unsigned char *)"m", 1u},
        {(const unsigned char *)"e", 1u}};
    snag_buf_reset(out);
    assert(!snag_binary_event_encode(out, &event));
}

struct cursor_visit {
    struct snag_binary_cursor cuts[10];
    uint64_t first;
    size_t count, calls, cancel_at, stop_at;
    int result;
};

static bool
cursor_cancelled(void *opaque)
{
    struct cursor_visit *visit = opaque;
    return ++visit->calls == visit->cancel_at;
}

static int
collect_cursor(void *opaque, const struct snag_binary_record *record, uint64_t sequence,
    const struct snag_binary_cursor *after)
{
    struct cursor_visit *visit = opaque;
    assert(record && sequence == visit->first + visit->count && after->next_seq == sequence + 1u);
    assert(visit->count < sizeof(visit->cuts) / sizeof(*visit->cuts));
    visit->cuts[visit->count++] = *after;
    if (visit->count == visit->stop_at) {
        if (visit->result < 0) errno = EIO;
        return visit->result;
    }
    return 0;
}

static bool
same_cursor(const struct snag_binary_cursor *a, const struct snag_binary_cursor *b)
{
    return a->next_seq == b->next_seq && a->record_offset == b->record_offset &&
        a->before.end == b->before.end && a->before.next_seq == b->before.next_seq &&
        a->before.turns == b->before.turns && a->before.previous == b->before.previous &&
        !memcmp(a->before.digest, b->before.digest, sizeof(a->before.digest));
}

static void
check_cursors(int fd, const struct snag_binary_anchor anchors[4])
{
    struct snag_binary_cursor initial = {.before = anchors[0], .next_seq = 1u,
        .record_offset = SNAG_BINARY_BATCH_HEADER_SIZE}, cursor = initial;
    struct cursor_visit visit = {.first = 1u};
    assert(!snag_binary_cursor_read(fd, &anchors[3], anchors[3].next_seq,
        &cursor, collect_cursor, cursor_cancelled, &visit));
    assert(visit.count == 9u && cursor.next_seq == anchors[3].next_seq &&
        cursor.before.end == anchors[3].end &&
        cursor.record_offset == SNAG_BINARY_BATCH_HEADER_SIZE);
    size_t calls = visit.calls;
    struct snag_binary_cursor cuts[10] = {initial};
    memcpy(cuts + 1u, visit.cuts, visit.count * sizeof(*visit.cuts));
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    for (unsigned group = 0u; group < 3u; ++group) {
        struct snag_binary_batch batch;
        struct snag_binary_anchor after;
        assert(!snag_binary_batch_read(fd, anchors[3].end, &anchors[group],
            &scratch, &batch, &after));
        for (uint64_t seq = anchors[group].next_seq; seq <= after.next_seq; ++seq) {
            struct snag_binary_cursor captured;
            assert(!snag_binary_cursor_capture(&anchors[group], &after, &batch, seq, &captured));
            assert(same_cursor(&captured, &cuts[seq - 1u]));
        }
        unsigned char saved[sizeof(cursor)];
        memcpy(saved, &cursor, sizeof(cursor));
        struct snag_binary_anchor bad = after;
        bad.digest[0] ^= 1u;
        assert(snag_binary_cursor_capture(&anchors[group], &bad, &batch,
            anchors[group].next_seq, &cursor) < 0 && errno == EINVAL);
        assert(!memcmp(saved, &cursor, sizeof(cursor)));
    }
    /* Every real cut, including a partial batch, supports exact empty/range
     * reads and pausing after one accepted record. Later bytes remain bounded. */
    for (uint64_t first = 1u; first <= anchors[3].next_seq; ++first) {
        for (uint64_t end = first; end <= anchors[3].next_seq; ++end) {
            cursor = cuts[first - 1u];
            visit = (struct cursor_visit){.first = first};
            assert(!snag_binary_cursor_read(fd, &anchors[3], end, &cursor,
                collect_cursor, NULL, &visit));
            assert(visit.count == end - first && same_cursor(&cursor, &cuts[end - 1u]));
        }
        cursor = cuts[first - 1u];
        visit = (struct cursor_visit){.first = first, .stop_at = 1u, .result = 1};
        int expected = first < anchors[3].next_seq ? 1 : 0;
        assert(snag_binary_cursor_read(fd, &anchors[3], anchors[3].next_seq,
            &cursor, collect_cursor, NULL, &visit) == expected);
        assert(same_cursor(&cursor, &cuts[first - 1u + (unsigned)expected]));
    }
    /* Cancellation at every observed check, even final adoption, is atomic. */
    for (size_t at = 1u; at <= calls; ++at) {
        cursor = initial;
        visit = (struct cursor_visit){.first = 1u, .cancel_at = at};
        assert(snag_binary_cursor_read(fd, &anchors[3], anchors[3].next_seq,
            &cursor, collect_cursor, cursor_cancelled, &visit) < 0 && errno == ECANCELED);
        assert(same_cursor(&cursor, &initial));
    }
    for (unsigned fault = 0u; fault < 7u; ++fault) {
        cursor = cuts[1];
        if (fault == 0u) ++cursor.record_offset;
        if (fault == 1u) ++cursor.next_seq;
        if (fault == 2u) cursor.before.digest[0] ^= 1u;
        if (fault == 3u) cursor.record_offset = SNAG_BINARY_BATCH_HEADER_SIZE - 1u;
        if (fault == 4u) cursor.next_seq = cursor.before.next_seq;
        if (fault == 5u) cursor.before.end = anchors[3].end;
        unsigned char saved[sizeof(cursor)];
        memcpy(saved, &cursor, sizeof(cursor));
        visit = (struct cursor_visit){.first = cursor.next_seq};
        uint64_t end = anchors[3].next_seq + (fault == 6u);
        assert(snag_binary_cursor_read(fd, &anchors[3], end,
            &cursor, collect_cursor, NULL, &visit) < 0);
        assert(!memcmp(saved, &cursor, sizeof(cursor)) && !visit.count);
    }
    for (int result = -1; result <= 2; result += 3) {
        cursor = initial;
        visit = (struct cursor_visit){.first = 1u, .stop_at = 2u, .result = result};
        assert(snag_binary_cursor_read(fd, &anchors[3], anchors[3].next_seq,
            &cursor, collect_cursor, NULL, &visit) < 0 && errno == (result < 0 ? EIO : EINVAL));
        assert(visit.count == 2u && same_cursor(&cursor, &initial));
    }
    assert(snag_seek(fd, 0, SEEK_CUR) == 13);
    snag_buf_free(&scratch);
}

static void
check_guarded_cursors(int fd, const struct snag_binary_anchor anchors[4],
    const struct snag_binary_identity *identity, const struct snag_binary_checkpoint_index *sparse)
{
    struct snag_binary_cursor initial = {.before = anchors[0], .next_seq = 1u,
        .record_offset = SNAG_BINARY_BATCH_HEADER_SIZE}, cursor = initial;
    struct cursor_visit visit = {.first = 1u};
    assert(!snag_binary_cursor_read(fd, &anchors[3], anchors[3].next_seq,
        &cursor, collect_cursor, NULL, &visit));
    struct snag_binary_cursor cuts[10] = {initial};
    memcpy(cuts + 1u, visit.cuts, visit.count * sizeof(*visit.cuts));
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_buf flat = {.max = SNAG_BINARY_INDEX_BATCH_MAX};
    struct snag_buf bytes = {.max = SIZE_MAX};
    struct snag_binary_index_entry entries[6];
    for (unsigned group = 0u; group < 2u; ++group) {
        struct snag_binary_batch batch;
        struct snag_binary_anchor after;
        assert(!snag_binary_batch_read(fd, anchors[3].end, &anchors[group],
            &scratch, &batch, &after));
        snag_buf_reset(&flat);
        assert(!snag_binary_index_append_batch(&flat, identity, &anchors[group],
            &after, batch.data, batch.size));
        for (unsigned i = 0u; i < 3u; ++i) {
            assert(!snag_binary_index_entry_decode(flat.data + i * SNAG_BINARY_INDEX_ENTRY_SIZE,
                SNAG_BINARY_INDEX_ENTRY_SIZE, identity, anchors[group].next_seq + i,
                &entries[group * 3u + i]));
        }
    }
    assert(!snag_binary_checkpoint_index_encode(&bytes, identity, &anchors[2],
        &sparse->tree, entries, 6u));
    unsigned char hash[32];
    assert(!snag_binary_index_tree_root(&sparse->tree, hash));
    struct snag_binary_checkpoint_index full;
    assert(!snag_binary_checkpoint_index_decode(bytes.data, bytes.len,
        identity, &anchors[2], hash, &full));
    for (uint64_t first = 1u; first <= 10u; ++first) {
        for (uint64_t end = first; end <= 10u; ++end) {
            cursor = cuts[first - 1u];
            visit = (struct cursor_visit){.first = first};
            assert(!snag_binary_checkpoint_cursor_read(fd, &anchors[3], &full, end,
                &cursor, collect_cursor, cursor_cancelled, &visit));
            assert(visit.count == end - first && same_cursor(&cursor, &cuts[end - 1u]));
        }
    }
    cursor = initial;
    visit = (struct cursor_visit){.first = 1u};
    assert(!snag_binary_checkpoint_cursor_read(fd, &anchors[3], &full, 10u,
        &cursor, collect_cursor, cursor_cancelled, &visit));
    size_t calls = visit.calls;
    for (size_t cancelled = 1u; cancelled <= calls; ++cancelled) {
        cursor = initial;
        visit = (struct cursor_visit){.first = 1u, .cancel_at = cancelled};
        assert(snag_binary_checkpoint_cursor_read(fd, &anchors[3], &full, 10u,
            &cursor, collect_cursor, cursor_cancelled, &visit) < 0 && errno == ECANCELED);
        assert(!memcmp(&cursor, &initial, sizeof(cursor)));
    }
    cursor = initial;
    visit = (struct cursor_visit){.first = 1u};
    assert(snag_binary_checkpoint_cursor_read(fd, &anchors[3], NULL, 10u,
        &cursor, collect_cursor, NULL, &visit) < 0 && errno == EINVAL && !visit.count);
    assert(snag_binary_checkpoint_cursor_read(fd, &anchors[3], sparse, 10u,
        &cursor, collect_cursor, NULL, &visit) < 0 && errno == ENOENT && !visit.count);
    assert(!memcmp(&cursor, &initial, sizeof(cursor)));
    cursor = cuts[1];
    struct snag_binary_cursor saved = cursor;
    visit = (struct cursor_visit){.first = 2u};
    assert(snag_binary_checkpoint_cursor_read(fd, &anchors[3], sparse, 10u,
        &cursor, collect_cursor, NULL, &visit) < 0 && errno == ENOENT && visit.count == 2u);
    assert(!memcmp(&cursor, &saved, sizeof(cursor)));
    visit = (struct cursor_visit){.first = 2u};
    assert(!snag_binary_checkpoint_cursor_read(fd, &anchors[3], sparse, 4u,
        &cursor, collect_cursor, NULL, &visit));
    assert(visit.count == 2u && same_cursor(&cursor, &cuts[3]));
    cursor = cuts[6];
    visit = (struct cursor_visit){.first = 7u};
    assert(!snag_binary_checkpoint_cursor_read(fd, &anchors[3], sparse, 10u,
        &cursor, collect_cursor, NULL, &visit));
    assert(visit.count == 3u && same_cursor(&cursor, &cuts[9]));
    assert(snag_seek(fd, 0, SEEK_CUR) == 13);
    snag_buf_free(&bytes);
    snag_buf_free(&flat);
    snag_buf_free(&scratch);
}

static void
test_grouped_ranges(void)
{
    char *path = snag_path_join(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
        "snag-checkpoint-range-XXXXXX");
    assert(path);
    int fd = mkstemp(path);
    assert(fd >= 0 && !unlink(path));
    free(path);
    struct snag_binary_identity identity = {.created_ms = 42u};
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    snag_binary_header_encode(header, &identity);
    struct snag_binary_anchor before, anchors[4];
    assert(!snag_binary_header_decode(header, sizeof(header), &identity, &before));
    assert(!snag_write_full(fd, header, sizeof(header)));
    anchors[0] = before;
    struct snag_buf raw = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_buf flat = {.max = SNAG_BINARY_INDEX_BATCH_MAX};
    struct snag_buf turn = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_index_tree tree = {0};
    struct snag_binary_index_entry active[4];
    size_t count = 0u;
    for (unsigned group = 0u; group < 3u; ++group) {
        unsigned char payloads[3][3];
        struct snag_binary_record records[3];
        for (unsigned i = 0u; i < 3u; ++i) {
            payloads[i][0] = (unsigned char)(before.next_seq + i);
            payloads[i][1] = 0u;
            payloads[i][2] = 111u;
            records[i] = (struct snag_binary_record){.kind = 77u, .version = 1u,
                .payload = payloads[i], .size = sizeof(payloads[i])};
        }
        if (group < 2u) {
            range_turn(&turn, group + 1u);
            records[1] = (struct snag_binary_record){.kind = SNAG_BINARY_TURN_STARTED,
                .version = snag_binary_event_version(SNAG_BINARY_TURN_STARTED),
                .payload = turn.data, .size = turn.len};
        }
        struct snag_binary_anchor after;
        snag_buf_reset(&raw);
        assert(!snag_binary_batch_encode(&raw, &before, records, 3u,
            before.turns + (group < 2u), &after));
        assert(!binary_fixture_write(fd, raw.data, raw.len));
        if (group < 2u) {
            assert(!snag_binary_index_tree_append_batch(NULL, &tree, &identity,
                &before, &after, raw.data, raw.len));
            snag_buf_reset(&flat);
            assert(!snag_binary_index_append_batch(&flat, &identity, &before, &after,
                raw.data, raw.len));
            for (unsigned i = 1u; i < 3u; ++i) {
                assert(!snag_binary_index_entry_decode(flat.data + i * SNAG_BINARY_INDEX_ENTRY_SIZE,
                    SNAG_BINARY_INDEX_ENTRY_SIZE, &identity, before.next_seq + i,
                    &active[count++]));
            }
        }
        before = after;
        anchors[group + 1u] = after;
    }
    assert(count == 4u);
    struct snag_buf metadata = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_index_encode(&metadata, &identity, &anchors[2],
        &tree, active, count));
    unsigned char root[32];
    assert(!snag_binary_index_tree_root(&tree, root));
    struct snag_binary_checkpoint_index decoded;
    assert(!snag_binary_checkpoint_index_decode(metadata.data, metadata.len,
        &identity, &anchors[2], root, &decoded));
    assert(snag_seek(fd, 13, SEEK_SET) == 13);
    check_cursors(fd, anchors);
    check_guarded_cursors(fd, anchors, &identity, &decoded);
    check_ranges(fd, &anchors[3], &decoded, true);
    /* Well-formed, checksummed table replacements still need canonical checks. */
    for (unsigned fault = 0u; fault < 5u; ++fault) {
        struct snag_binary_index_entry entry = active[0];
        if (fault == 0u) ++entry.record_offset;
        if (fault == 1u) entry.kind = 78u;
        if (fault == 2u) entry.turn = 0u;
        if (fault == 3u) entry.batch_digest[0] ^= 1u;
        if (fault == 4u) {
            entry.sequence = 1u;
            entry.turn = 0u;
        }
        assert(!snag_binary_index_entry_encode(metadata.data +
            SNAG_BINARY_CHECKPOINT_INDEX_HEADER_SIZE, &identity, &entry));
        assert(!snag_binary_checkpoint_index_decode(metadata.data, metadata.len,
            &identity, &anchors[2], root, &decoded));
        struct range_visit visit = {0};
        assert(snag_binary_checkpoint_records_read(fd, &anchors[3], &decoded,
            1u, anchors[3].next_seq, collect_range, NULL, &visit) < 0 && errno == EINVAL);
        assert(!visit.count && snag_seek(fd, 0, SEEK_CUR) == 13);
    }
    assert(!snag_binary_index_entry_encode(metadata.data +
        SNAG_BINARY_CHECKPOINT_INDEX_HEADER_SIZE, &identity, &active[0]));
    assert(!snag_binary_checkpoint_index_decode(metadata.data, metadata.len,
        &identity, &anchors[2], root, &decoded));
    /* Corrupt a selected physical batch; no callback from it is exposed. */
    unsigned char byte;
    int64_t offset = (int64_t)anchors[0].end + 20;
    assert(snag_pread(fd, &byte, 1u, offset) == 1);
    unsigned char changed = byte ^ 1u;
    assert(snag_seek(fd, offset, SEEK_SET) == offset);
    assert(!snag_write_full(fd, &changed, 1u));
    assert(snag_seek(fd, 13, SEEK_SET) == 13);
    struct range_visit visit = {0};
    assert(snag_binary_checkpoint_records_read(fd, &anchors[3], &decoded,
        1u, anchors[3].next_seq, collect_range, NULL, &visit) < 0 && !visit.count);
    assert(snag_seek(fd, offset, SEEK_SET) == offset);
    assert(!snag_write_full(fd, &byte, 1u));
    /* A valid envelope/count cannot substitute for the typed turn number. */
    range_turn(&turn, 9u);
    unsigned char first[3] = {1u, 0u, 111u}, last[3] = {3u, 0u, 111u};
    struct snag_binary_record records[3] = {
        {.kind = 77u, .version = 1u, .payload = first, .size = sizeof(first)},
        {.kind = SNAG_BINARY_TURN_STARTED,
            .version = snag_binary_event_version(SNAG_BINARY_TURN_STARTED),
            .payload = turn.data, .size = turn.len},
        {.kind = 77u, .version = 1u, .payload = last, .size = sizeof(last)}};
    snag_buf_reset(&raw);
    struct snag_binary_anchor bad;
    assert(!snag_binary_batch_encode(&raw, &anchors[0], records, 3u, 1u, &bad));
    assert(!snag_truncate(fd, (int64_t)anchors[0].end));
    assert(snag_seek(fd, (int64_t)anchors[0].end, SEEK_SET) == (int64_t)anchors[0].end);
    assert(!binary_fixture_write(fd, raw.data, raw.len));
    assert(snag_seek(fd, 13, SEEK_SET) == 13);
    visit = (struct range_visit){0};
    assert(snag_binary_checkpoint_records_read(fd, &bad, NULL,
        1u, bad.next_seq, collect_range, NULL, &visit) < 0 && errno == EINVAL);
    assert(visit.count == 1u && snag_seek(fd, 0, SEEK_CUR) == 13);
    snag_buf_free(&metadata);
    snag_buf_free(&raw);
    snag_buf_free(&flat);
    snag_buf_free(&turn);
    assert(!close(fd));
}

void
test_store_binary_checkpoint_index(void)
{
    test_metadata();
    test_extremes();
    test_direct_reads();
    test_grouped_ranges();
}
