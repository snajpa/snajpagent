/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

void test_store_binary_receipt(void);

static void
assert_receipt(const struct snag_binary_checkpoint_receipt *a,
    const struct snag_binary_checkpoint_receipt *b)
{
    assert(a->generation == b->generation && a->image_size == b->image_size &&
        a->boundary.end == b->boundary.end &&
        a->boundary.next_seq == b->boundary.next_seq && a->boundary.turns == b->boundary.turns &&
        a->boundary.previous == b->boundary.previous);
    assert(!memcmp(a->boundary.digest, b->boundary.digest, 32u));
    assert(!memcmp(a->image_digest, b->image_digest, 32u));
    assert(!memcmp(a->index_root, b->index_root, 32u));
}

static void
reject_receipt(const struct snag_binary_record *record, int expected)
{
    struct snag_binary_checkpoint_receipt decoded;
    memset(&decoded, 0x5a, sizeof(decoded));
    struct snag_binary_checkpoint_receipt saved;
    memcpy(&saved, &decoded, sizeof(saved));
    assert(snag_binary_checkpoint_receipt_decode(record, &decoded) == expected);
    assert(!memcmp(&decoded, &saved, sizeof(decoded)));
}

static void
test_codec(void)
{
    struct snag_binary_checkpoint_receipt receipt = {
        .generation = UINT64_C(0x0102030405060708),
        .image_size = UINT64_C(0x1122334455667788), .boundary = {
            .end = 0x11223344u, .next_seq = 0x5566u, .turns = 0x11u, .previous = 0x223344u}
    };
    for (size_t i = 0u; i < 32u; ++i) {
        receipt.boundary.digest[i] = (unsigned char)i;
        receipt.image_digest[i] = (unsigned char)(32u + i);
        receipt.index_root[i] = (unsigned char)(64u + i);
    }
    struct snag_buf encoded = {.max = 2u * SNAG_BINARY_CHECKPOINT_RECEIPT_SIZE};
    assert(!snag_binary_checkpoint_receipt_encode(&encoded, &receipt));
    assert(encoded.len == 152u);
    /* Independent Python struct.pack fixture, including index format and reserved bytes. */
    const char *hex =
        "0807060504030201443322110000000066550000000000001100000000000000"
        "4433220000000000000102030405060708090a0b0c0d0e0f1011121314151617"
        "18191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f3031323334353637"
        "38393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f5051525354555657"
        "58595a5b5c5d5e5f00000200000000008877665544332211";
    assert(strlen(hex) == encoded.len * 2u);
    for (size_t i = 0u; i < encoded.len; ++i) {
        unsigned int value;
        assert(sscanf(hex + 2u * i, "%2x", &value) == 1 && value == encoded.data[i]);
    }
    struct snag_binary_record record = {.kind = SNAG_BINARY_CHECKPOINT_RECEIPT,
        .version = SNAG_BINARY_CHECKPOINT_RECEIPT_VERSION, .flags = SNAG_BINARY_RECORD_OPTIONAL,
        .payload = encoded.data, .size = encoded.len};
    struct snag_binary_checkpoint_receipt decoded;
    assert(!snag_binary_checkpoint_receipt_decode(&record, &decoded));
    assert_receipt(&decoded, &receipt);
    for (size_t size = 0u; size < encoded.len; ++size) {
        struct snag_binary_record bad = record;
        bad.size = size;
        reject_receipt(&bad, -1);
    }
    for (size_t fault = 0u; fault < 17u; ++fault) {
        unsigned char bytes[SNAG_BINARY_CHECKPOINT_RECEIPT_SIZE + 1u] = {0};
        memcpy(bytes, encoded.data, encoded.len);
        struct snag_binary_record bad = record;
        bad.payload = bytes;
        if (fault == 0u) --bad.kind;
        if (fault == 1u) bad.version = 0u;
        if (fault == 2u) bad.flags = 0u;
        if (fault == 3u) bad.flags |= 2u;
        if (fault == 4u) ++bad.size;
        if (fault == 5u) bad.payload = NULL;
        if (fault == 6u) memset(bytes, 0, 8u);
        if (fault == 7u) memset(bytes + 8u, 0, 8u);
        if (fault == 8u) bytes[15u] = 0x80u;
        if (fault == 9u) memset(bytes + 16u, 0, 8u);
        if (fault == 10u) memcpy(bytes + 24u, bytes + 16u, 8u);
        if (fault == 11u) memset(bytes + 32u, 0, 8u);
        if (fault == 12u) memcpy(bytes + 32u, bytes + 8u, 8u);
        if (fault == 13u) bytes[140u] = 1u;
        if (fault == 14u) memset(bytes + 144u, 0, 8u);
        if (fault == 15u) {
            memset(bytes + 144u, 0, 8u);
            bytes[144u] = 209u;
        }
        if (fault == 16u) bytes[151u] = 0x80u;
        reject_receipt(&bad, -1);
    }
    for (size_t fault = 0u; fault < 3u; ++fault) {
        unsigned char bytes[SNAG_BINARY_CHECKPOINT_RECEIPT_SIZE];
        memcpy(bytes, encoded.data, sizeof(bytes));
        struct snag_binary_record unknown = record;
        unknown.payload = bytes;
        if (fault == 0u) ++unknown.version;
        if (fault == 1u) bytes[136u] = 1u;
        if (fault == 2u) bytes[138u] = 3u;
        reject_receipt(&unknown, 1);
    }
    struct snag_binary_record future = {.kind = SNAG_BINARY_CHECKPOINT_RECEIPT,
        .version = SNAG_BINARY_CHECKPOINT_RECEIPT_VERSION + 1u,
        .flags = SNAG_BINARY_RECORD_OPTIONAL};
    reject_receipt(&future, 1);
    future.version = 1u;
    future.payload = encoded.data;
    future.size = 144u;
    reject_receipt(&future, 1);
    reject_receipt(NULL, -1);
    assert(snag_binary_checkpoint_receipt_decode(&record, NULL) < 0);
    assert(!snag_binary_checkpoint_receipt_encode(&encoded, &receipt));
    assert(encoded.len == encoded.max && !memcmp(encoded.data, encoded.data + 152u, 152u));
    assert(snag_binary_checkpoint_receipt_encode(&encoded, &receipt) < 0 && errno == EOVERFLOW);
    assert(encoded.len == encoded.max && !memcmp(encoded.data, encoded.data + 152u, 152u));
    size_t kept = encoded.len;
    receipt.generation = 0u;
    assert(snag_binary_checkpoint_receipt_encode(&encoded, &receipt) < 0 && encoded.len == kept);
    assert(snag_binary_checkpoint_receipt_encode(&encoded, NULL) < 0 && encoded.len == kept);
    receipt.generation = UINT64_MAX;
    assert(snag_binary_checkpoint_receipt_encode(NULL, &receipt) < 0);
    snag_buf_reset(&encoded);
    receipt.boundary = (struct snag_binary_anchor){.end = SNAG_BINARY_HEADER_SIZE, .next_seq = 1u};
    assert(!snag_binary_checkpoint_receipt_encode(&encoded, &receipt));
    record.payload = encoded.data;
    assert(!snag_binary_checkpoint_receipt_decode(&record, &decoded));
    assert_receipt(&decoded, &receipt);
    receipt.boundary.next_seq = 2u;
    assert(snag_binary_checkpoint_receipt_encode(&encoded, &receipt) < 0);
    receipt.boundary.next_seq = 1u;
    const uint64_t edges[] = {210u, INT64_MAX};
    for (size_t i = 0u; i < sizeof(edges) / sizeof(edges[0]); ++i) {
        snag_buf_reset(&encoded);
        receipt.image_size = edges[i];
        assert(!snag_binary_checkpoint_receipt_encode(&encoded, &receipt));
        record.payload = encoded.data;
        assert(!snag_binary_checkpoint_receipt_decode(&record, &decoded));
        assert_receipt(&decoded, &receipt);
    }
    const uint64_t bad_sizes[] = {0u, 209u, (uint64_t)INT64_MAX + 1u, UINT64_MAX};
    for (size_t i = 0u; i < sizeof(bad_sizes) / sizeof(bad_sizes[0]); ++i) {
        receipt.image_size = bad_sizes[i];
        assert(snag_binary_checkpoint_receipt_encode(&encoded, &receipt) < 0);
        assert(encoded.len == SNAG_BINARY_CHECKPOINT_RECEIPT_SIZE);
    }
    snag_buf_free(&encoded);
}

static void
reject_image(const struct snag_buf *image, const struct snag_binary_identity *identity,
    const struct snag_binary_checkpoint_receipt *receipt, int expected)
{
    struct snag_binary_checkpoint_frame frame;
    memset(&frame, 0x5a, sizeof(frame));
    struct snag_binary_checkpoint_frame saved;
    memcpy(&saved, &frame, sizeof(saved));
    assert(snag_binary_checkpoint_frame_from_receipt(image->data, image->len,
        identity, receipt, &frame) == expected);
    assert(!memcmp(&frame, &saved, sizeof(frame)));
}

static void
test_image_binding(void)
{
    unsigned char core[] = {0u, 4u, 0xffu};
    struct snag_binary_checkpoint_frame frame = {.identity = {.created_ms = 42u},
        .generation = 9u, .boundary = {.end = 512u, .next_seq = 7u, .turns = 2u, .previous = 96u},
        .core = {.version = 2u, .data = core, .size = sizeof(core)},
        .provider = {.version = 1u, .data = (const unsigned char *)"p", .size = 1u}};
    frame.identity.id[0] = 17u;
    for (size_t i = 0u; i < 32u; ++i) {
        frame.boundary.digest[i] = (unsigned char)i;
    }
    struct snag_buf encoded = {.max = 4096u};
    assert(!snag_binary_checkpoint_frame_encode(&encoded, &frame));
    struct snag_binary_checkpoint_receipt receipt = {
        .generation = frame.generation, .image_size = encoded.len, .boundary = frame.boundary};
    memcpy(receipt.image_digest, encoded.data + encoded.len - 32u, 32u);
    struct snag_binary_checkpoint_frame decoded;
    assert(!snag_binary_checkpoint_frame_from_receipt(encoded.data, encoded.len,
        &frame.identity, &receipt, &decoded));
    assert(decoded.core.size == sizeof(core) && !memcmp(decoded.core.data, core, sizeof(core)));
    assert(decoded.provider.size == 1u && decoded.provider.data[0] == 'p');
    struct snag_buf payload = {.max = SNAG_BINARY_CHECKPOINT_RECEIPT_SIZE};
    assert(!snag_binary_checkpoint_receipt_encode(&payload, &receipt));
    struct snag_binary_record metadata = {.kind = SNAG_BINARY_CHECKPOINT_RECEIPT,
        .version = SNAG_BINARY_CHECKPOINT_RECEIPT_VERSION, .flags = SNAG_BINARY_RECORD_OPTIONAL,
        .payload = payload.data, .size = payload.len};
    struct snag_buf committed = {.max = SNAG_BINARY_BATCH_MAX};
    assert(!snag_binary_batch_encode(&committed, &frame.boundary, &metadata, 1u,
        frame.boundary.turns, NULL));
    struct snag_binary_batch batch;
    struct snag_binary_anchor after;
    assert(!snag_binary_batch_decode(committed.data, committed.len,
        &frame.boundary, &batch, &after));
    size_t cursor = SNAG_BINARY_BATCH_HEADER_SIZE;
    uint64_t sequence;
    assert(!snag_binary_record_next(&batch, &cursor, &metadata, &sequence));
    assert(sequence == receipt.boundary.next_seq && after.next_seq == sequence + 1u &&
        after.turns == receipt.boundary.turns);
    struct snag_binary_checkpoint_receipt unpacked;
    assert(!snag_binary_checkpoint_receipt_decode(&metadata, &unpacked));
    assert_receipt(&unpacked, &receipt);
    assert(!snag_binary_checkpoint_frame_from_receipt(encoded.data, encoded.len,
        &frame.identity, &unpacked, &decoded));
    snag_buf_free(&committed);
    snag_buf_free(&payload);
    for (size_t size = 0u; size < encoded.len; ++size) {
        struct snag_buf cut = encoded;
        cut.len = size;
        reject_image(&cut, &frame.identity, &receipt, 1);
    }
    for (size_t i = 0u; i < encoded.len; ++i) {
        encoded.data[i] ^= 1u;
        reject_image(&encoded, &frame.identity, &receipt, -1);
        encoded.data[i] ^= 1u;
    }
    for (size_t fault = 0u; fault < 9u; ++fault) {
        struct snag_binary_checkpoint_receipt wrong = receipt;
        if (fault == 0u) ++wrong.generation;
        if (fault == 1u) ++wrong.boundary.end;
        if (fault == 2u) ++wrong.boundary.next_seq;
        if (fault == 3u) ++wrong.boundary.turns;
        if (fault == 4u) ++wrong.boundary.previous;
        if (fault == 5u) wrong.boundary.digest[0] ^= 1u;
        if (fault == 6u) wrong.image_digest[0] ^= 1u;
        if (fault == 7u) --wrong.image_size;
        if (fault == 8u) ++wrong.image_size;
        reject_image(&encoded, &frame.identity, &wrong, -1);
    }
    for (size_t fault = 0u; fault < 5u; ++fault) {
        struct snag_binary_checkpoint_frame changed = frame;
        unsigned char changed_core[] = {0u, 5u, 0xffu};
        if (fault == 0u) changed.core.data = changed_core;
        if (fault == 1u) ++changed.generation;
        if (fault == 2u) ++changed.boundary.turns;
        if (fault == 3u) ++changed.identity.created_ms;
        if (fault == 4u) changed.identity.id[0] ^= 1u;
        struct snag_buf rewritten = {.max = 4096u};
        assert(!snag_binary_checkpoint_frame_encode(&rewritten, &changed));
        assert(!snag_binary_checkpoint_frame_decode(rewritten.data, rewritten.len,
            &changed.identity, &changed.boundary, &decoded));
        reject_image(&rewritten, &frame.identity, &receipt, -1);
        snag_buf_free(&rewritten);
    }
    /* A claimed oversize is rejected before touching bytes outside this allocation. */
    struct snag_buf oversized = encoded;
    oversized.len = SIZE_MAX;
    reject_image(&oversized, &frame.identity, &receipt, -1);
    /* Image binding alone does not validate the separately pinned index root. */
    receipt.index_root[0] ^= 1u;
    assert(!snag_binary_checkpoint_frame_from_receipt(encoded.data, encoded.len,
        &frame.identity, &receipt, &decoded));
    reject_image(&encoded, NULL, &receipt, -1);
    reject_image(&encoded, &frame.identity, NULL, -1);
    assert(snag_binary_checkpoint_frame_from_receipt(encoded.data, encoded.len,
        &frame.identity, &receipt, NULL) < 0);
    snag_buf_free(&encoded);
}

void
test_store_binary_receipt(void)
{
    test_codec();
    test_image_binding();
}
