/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary.h"
#include "fs.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void test_store_binary(void);

static void
assert_bytes(const unsigned char *bytes, size_t size, const char *hex)
{
    static const char digits[] = "0123456789abcdef";
    assert(strlen(hex) == size * 2u);
    for (size_t i = 0; i < size; ++i) {
        assert(hex[i * 2u] == digits[bytes[i] >> 4u]);
        assert(hex[i * 2u + 1u] == digits[bytes[i] & 15u]);
    }
}

static void
rehash(unsigned char *bytes, size_t size)
{
    struct snag_sha256 hash;
    snag_sha256_init(&hash);
    snag_sha256_update(&hash, bytes, size - 32u);
    snag_sha256_final(&hash, bytes + size - 32u);
}

static void
test_file_reader(const unsigned char *header, const struct snag_buf *joined,
    const struct snag_binary_anchor *anchor, const struct snag_binary_anchor *next)
{
    char *path = snag_path_join(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
        "snajpagent-binary-XXXXXX");
    assert(path);
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(!snag_write_full(fd, header, SNAG_BINARY_HEADER_SIZE));
    assert(!snag_write_full(fd, joined->data, joined->len));
    uint64_t boundary = SNAG_BINARY_HEADER_SIZE + joined->len;
    assert(snag_seek(fd, 13, SEEK_SET) == 13);
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_batch batch;
    struct snag_binary_anchor candidate;
    assert(!snag_binary_batch_read(fd, boundary, anchor, &scratch, &batch, &candidate));
    assert(candidate.end == next->end && candidate.next_seq == next->next_seq);
    assert(snag_seek(fd, 0, SEEK_CUR) == 13);
    assert(!snag_binary_batch_read(fd, boundary, next, &scratch, &batch, &candidate));
    assert(candidate.end == boundary && candidate.next_seq == 4u);
    for (uint64_t cut = next->end; cut < boundary; ++cut) {
        candidate = *next;
        assert(snag_binary_batch_read(fd, cut, next, &scratch, &batch, &candidate) == 1);
        assert(!memcmp(&candidate, next, sizeof(candidate)));
        assert(scratch.len <= SNAG_BINARY_BATCH_HEADER_SIZE);
    }
    assert(snag_seek(fd, 0, SEEK_END) == (int64_t)boundary);
    assert(snag_binary_batch_read(fd, next->end - 1u, next,
        &scratch, &batch, &candidate) < 0);
    assert(snag_binary_batch_read(fd, UINT64_MAX, next,
        &scratch, &batch, &candidate) < 0);
    /* Unexpected EOF beneath a promised source boundary is a read failure,
     * not permission to discard the rest of a formerly complete batch. */
    assert(!snag_truncate(fd, (int64_t)boundary - 1));
    assert(snag_binary_batch_read(fd, boundary, next,
        &scratch, &batch, &candidate) < 0 && errno == EIO);
    assert(!snag_truncate(fd, (int64_t)next->end + 5));
    assert(snag_binary_batch_read(fd, boundary, next,
        &scratch, &batch, &candidate) < 0 && errno == EIO);
    /* Checksummed header rejects a corrupted length before allocating it. */
    assert(snag_seek(fd, (int64_t)next->end, SEEK_SET) == (int64_t)next->end);
    size_t second_offset = (size_t)(next->end - SNAG_BINARY_HEADER_SIZE);
    assert(!snag_write_full(fd, joined->data + second_offset,
        joined->len - second_offset));
    assert(snag_seek(fd, (int64_t)next->end + 8, SEEK_SET) == (int64_t)next->end + 8);
    const unsigned char wrong = 0xffu;
    assert(!snag_write_full(fd, &wrong, 1u));
    assert(snag_binary_batch_read(fd, boundary, next, &scratch, &batch, &candidate) < 0);
    assert(scratch.len == SNAG_BINARY_BATCH_HEADER_SIZE);
    snag_buf_free(&scratch);
    assert(!close(fd));
    assert(!unlink(path));
    free(path);
}

static void
reject_batch(const struct snag_buf *encoded, const struct snag_binary_anchor *anchor)
{
    struct snag_binary_batch batch = {0}, before = batch;
    struct snag_binary_anchor next = *anchor;
    assert(snag_binary_batch_decode(encoded->data, encoded->len,
        anchor, &batch, &next) < 0);
    assert(!memcmp(&next, anchor, sizeof(next)));
    assert(!memcmp(&batch, &before, sizeof(batch)));
}

void
test_store_binary(void)
{
    struct snag_binary_identity identity = {.created_ms = UINT64_C(0x0102030405060708)};
    for (size_t i = 0; i < sizeof(identity.id); ++i) identity.id[i] = (unsigned char)i;
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    snag_binary_header_encode(header, &identity);
    /* Independent explicit little-endian fixtures; neither C layout nor an
     * encode/decode round trip supplies their expected bytes. */
    assert_bytes(header, sizeof(header),
        "534e41474a4e4c00000001006000000000000000000000000000000000000000"
        "000102030405060708090a0b0c0d0e0f08070605040302010000000000000000"
        "8e943e61bd08692bc6f5e3b9c0711533fd76a2276eb9813bb7c6eb1e8959dcdb");
    struct snag_binary_identity decoded = {0};
    struct snag_binary_anchor anchor = {0};
    for (size_t i = 0; i < sizeof(header); ++i) {
        assert(snag_binary_header_decode(header, i, &decoded, &anchor) == 1);
        assert(!decoded.created_ms && !anchor.next_seq);
    }
    assert(!snag_binary_header_decode(header, sizeof(header), &decoded, &anchor));
    assert(!memcmp(identity.id, decoded.id, sizeof(identity.id)));
    assert(identity.created_ms == decoded.created_ms);
    assert(anchor.end == sizeof(header) && anchor.next_seq == 1u && !anchor.turns);
    for (size_t i = 0; i < sizeof(header); ++i) {
        unsigned char bad[sizeof(header)];
        memcpy(bad, header, sizeof(bad));
        bad[i] ^= 1u;
        struct snag_binary_anchor kept = anchor;
        assert(snag_binary_header_decode(bad, sizeof(bad), &decoded, &kept) < 0);
        assert(!memcmp(&kept, &anchor, sizeof(kept)));
    }
    /* Unsupported versions/features stay rejected even with a valid checksum,
     * including writer requirements that a read-only parser might overlook. */
    static const size_t unsupported[] = {8u, 10u, 12u, 16u, 24u, 56u};
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(*unsupported); ++i) {
        unsigned char bad[sizeof(header)];
        memcpy(bad, header, sizeof(bad));
        bad[unsupported[i]] ^= 1u;
        rehash(bad, sizeof(bad));
        assert(snag_binary_header_decode(bad, sizeof(bad), &decoded, &anchor) < 0);
    }
    static const unsigned char payload[] = {'a', 'b', 'c', 0, 'z'};
    struct snag_binary_record record = {
        .kind = 7u, .version = 1u, .timestamp_ms = identity.created_ms,
        .payload = payload, .size = sizeof(payload)
    };
    struct snag_buf encoded = {.max = SNAG_BINARY_BATCH_MAX};
    assert(!snag_binary_batch_encode(&encoded, &anchor, &record, 1u, 0u));
    assert_bytes(encoded.data, encoded.len,
        "534e414742415400e50000000000000001000000700000000100000000000000"
        "00000000000000008e943e61bd08692bc6f5e3b9c0711533fd76a2276eb9813b"
        "b7c6eb1e8959dcdb0000000000000000e4cd61a31ac3ff3752125a7d1c9a0f2b"
        "f974fca21e24ff4f4e0cc9b68a7e544625000000000000000100000000000000"
        "08070605040302010700010000000000616263007a534e4147454e4400600000"
        "0000000000e50000000000000001000000000000000000000000000000000000"
        "0000000000a912c826c16f98a7eb7ccadbd3d458a45575d4e72b072cde445baf"
        "caa820a234");
    struct snag_binary_batch batch = {0};
    struct snag_binary_anchor next = anchor;
    for (size_t i = 0; i < encoded.len; ++i) {
        assert(snag_binary_batch_decode(encoded.data, i, &anchor, &batch, &next) == 1);
        assert(!batch.size && !memcmp(&next, &anchor, sizeof(next)));
    }
    assert(!snag_binary_batch_decode(encoded.data, encoded.len, &anchor, &batch, &next));
    assert(batch.count == 1u && batch.first_seq == 1u && batch.size == encoded.len);
    assert(next.previous == sizeof(header) && next.end == sizeof(header) + encoded.len);
    assert(next.next_seq == 2u && !next.turns);
    struct snag_binary_record read;
    size_t cursor = SNAG_BINARY_BATCH_HEADER_SIZE;
    uint64_t sequence = 0;
    assert(!snag_binary_record_next(&batch, &cursor, &read, &sequence));
    assert(sequence == 1u && read.kind == 7u && read.version == 1u && !read.flags);
    assert(read.timestamp_ms == identity.created_ms && read.size == sizeof(payload));
    assert(!memcmp(read.payload, payload, read.size));
    assert(snag_binary_record_next(&batch, &cursor, &read, &sequence) == 1);
    cursor = batch.size;
    assert(snag_binary_record_next(&batch, &cursor, &read, &sequence) < 0);
    /* Every byte is authenticated, including header, payload and footer. */
    for (size_t i = 0; i < encoded.len; ++i) {
        encoded.data[i] ^= 1u;
        reject_batch(&encoded, &anchor);
        encoded.data[i] ^= 1u;
    }
    /* Authenticated malformed framing: the checksum alone is insufficient. */
    static const size_t malformed[] = {
        16u, 20u, 24u, 32u, 40u, 72u, /* count/header/sequence/link/reserved */
        112u, 120u, 136u, 138u, 140u, /* record length/sequence/kind/version/flags */
        157u, 165u, 173u, 189u /* footer start/length/sequence/reserved */
    };
    for (size_t i = 0; i < sizeof(malformed) / sizeof(*malformed); ++i) {
        struct snag_buf bad = {.max = encoded.len};
        assert(!snag_buf_append(&bad, encoded.data, encoded.len));
        /* Zero kind/version; unsupported flag bit; flip other fields. */
        if (malformed[i] == 136u || malformed[i] == 138u) bad.data[malformed[i]] = 0u;
        else bad.data[malformed[i]] ^= malformed[i] == 140u ? 2u : 1u;
        rehash(bad.data, SNAG_BINARY_BATCH_HEADER_SIZE);
        rehash(bad.data, bad.len);
        reject_batch(&bad, &anchor);
        snag_buf_free(&bad);
    }
    /* A complete prefix is returned even when another batch follows it. */
    struct snag_binary_record records[2] = {record, record};
    records[1].flags = SNAG_BINARY_RECORD_OPTIONAL;
    struct snag_buf second = {.max = SNAG_BINARY_BATCH_MAX};
    assert(!snag_binary_batch_encode(&second, &next, records, 2u, 1u));
    struct snag_binary_anchor third;
    assert(!snag_binary_batch_decode(second.data, second.len, &next, &batch, &third));
    assert(third.next_seq == 4u && third.turns == 1u && third.previous == next.end);
    reject_batch(&second, &anchor); /* predecessor mismatch */
    struct snag_buf joined = {.max = encoded.len + second.len};
    assert(!snag_buf_append(&joined, encoded.data, encoded.len));
    assert(!snag_buf_append(&joined, second.data, second.len));
    assert(!snag_binary_batch_decode(joined.data, joined.len, &anchor, &batch, &next));
    assert(batch.size == encoded.len);
    for (size_t i = 0; i < second.len; ++i) {
        struct snag_binary_anchor kept = next;
        assert(snag_binary_batch_decode(joined.data + encoded.len, i,
            &next, &batch, &kept) == 1);
        assert(!memcmp(&kept, &next, sizeof(kept)));
    }
    test_file_reader(header, &joined, &anchor, &next);
    snag_buf_free(&joined);
    snag_buf_free(&second);
    /* Invalid input/allocation bounds do not append partially encoded batches. */
    size_t saved = encoded.len;
    struct snag_binary_anchor invalid = next;
    invalid.next_seq = UINT64_MAX;
    assert(snag_binary_batch_encode(&encoded, &invalid, &record, 1u, 1u) < 0);
    invalid = next;
    invalid.end = INT64_MAX;
    assert(snag_binary_batch_encode(&encoded, &invalid, &record, 1u, 1u) < 0);
    assert(snag_binary_batch_encode(&encoded, &third, &record, 1u, 0u) < 0);
    assert(snag_binary_batch_encode(&encoded, &next, NULL, 0u, 1u) < 0);
    assert(snag_binary_batch_encode(&encoded, &next, &record, UINT32_MAX, 1u) < 0);
    assert(encoded.len == saved);
    struct snag_buf too_small = {.max = 1u};
    assert(snag_binary_batch_encode(&too_small, &anchor, &record, 1u, 0u) < 0);
    assert(!too_small.len);
    snag_buf_free(&too_small);
    snag_buf_free(&encoded);
    /* One maximum-size event fits alone; two records must fit the batch target. */
    encoded.max = SNAG_BINARY_BATCH_MAX;
    unsigned char *large = calloc(1u, SNAG_MAX_EVENT_LINE);
    assert(large);
    record.payload = large;
    record.size = SNAG_MAX_EVENT_LINE;
    assert(!snag_binary_batch_encode(&encoded, &anchor, &record, 1u, 0u));
    assert(encoded.len == SNAG_BINARY_BATCH_MAX);
    assert(!snag_binary_batch_decode(encoded.data, encoded.len, &anchor, &batch, &next));
    snag_buf_free(&encoded);
    encoded.max = SNAG_BINARY_BATCH_MAX;
    records[0] = record;
    records[0].size = SNAG_BINARY_BATCH_TARGET;
    assert(snag_binary_batch_encode(&encoded, &anchor, records, 2u, 0u) < 0);
    record.size = SNAG_MAX_EVENT_LINE + 1u;
    assert(snag_binary_batch_encode(&encoded, &anchor, &record, 1u, 0u) < 0);
    assert(!encoded.len);
    free(large);
}
