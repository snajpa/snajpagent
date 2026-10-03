/* SPDX-License-Identifier: GPL-2.0-only */
#include "base.h"
#include "fixture_store_binary.h"
#include "fs.h"
#include "store_binary.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void test_store_binary_receipts_find(void);

struct receipt_fixture {
    int fd;
    struct snag_binary_identity identity;
    struct snag_binary_anchor anchors[7];
    struct snag_binary_checkpoint_receipt receipts[2];
    struct snag_buf images[2], wire;
};

static void
make_image(struct receipt_fixture *fixture, size_t slot, size_t boundary)
{
    static const unsigned char core[] = "core", provider[] = "provider";
    struct snag_binary_checkpoint_frame frame = {
        .identity = fixture->identity, .boundary = fixture->anchors[boundary],
        .generation = 20u - slot,
        .core = {.version = 1u, .data = core, .size = sizeof(core)},
        .provider = {.version = 1u, .data = provider, .size = sizeof(provider)}
    };
    struct snag_buf *image = &fixture->images[slot];
    image->max = 4096u;
    snag_buf_reset(image);
    assert(!snag_binary_checkpoint_frame_encode(image, &frame));
    struct snag_binary_checkpoint_receipt *receipt = &fixture->receipts[slot];
    memset(receipt, 0, sizeof(*receipt));
    receipt->generation = frame.generation;
    receipt->boundary = frame.boundary;
    memcpy(receipt->image_digest, image->data + image->len - 32u, 32u);
    memset(receipt->index_root, (int)(slot + 1u), 32u);
}

static void
append_batch(struct receipt_fixture *fixture, size_t index,
    const struct snag_binary_record *records, uint32_t count, uint64_t turns)
{
    assert(!binary_fixture_append(&fixture->wire, &fixture->anchors[index - 1u],
        records, count, turns, &fixture->anchors[index]));
}

static void
build_fixture(struct receipt_fixture *fixture, unsigned fault)
{
    fixture->identity.created_ms = 42u;
    fixture->identity.id[0] = 17u;
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    snag_binary_header_encode(header, &fixture->identity);
    struct snag_binary_identity checked;
    assert(!snag_binary_header_decode(header, sizeof(header), &checked, &fixture->anchors[0]));
    fixture->wire.max = 4096u;
    snag_buf_reset(&fixture->wire);
    assert(!snag_buf_append(&fixture->wire, header, sizeof(header)));
    struct snag_binary_record data = {.kind = 77u, .version = 1u,
        .payload = (const unsigned char *)"body", .size = 4u};
    append_batch(fixture, 1u, &data, 1u, 1u);
    make_image(fixture, 1u, 1u);
    struct snag_buf old = {.max = 4096u};
    assert(!snag_binary_checkpoint_receipt_encode(&old, &fixture->receipts[1]));
    struct snag_binary_record metadata = {.kind = SNAG_BINARY_CHECKPOINT_RECEIPT,
        .version = 1u, .flags = SNAG_BINARY_RECORD_OPTIONAL,
        .payload = old.data, .size = old.len};
    append_batch(fixture, 2u, &metadata, 1u, 1u);
    append_batch(fixture, 3u, &data, 1u, 2u);
    make_image(fixture, 0u, fault == 14u ? 0u : 3u);
    struct snag_binary_checkpoint_receipt receipt = fixture->receipts[0];
    if (fault == 1u) --receipt.boundary.end;
    if (fault == 2u) --receipt.boundary.next_seq;
    if (fault == 3u) --receipt.boundary.turns;
    if (fault == 4u) ++receipt.boundary.previous;
    if (fault == 5u) receipt.boundary.digest[0] ^= 1u;
    if (fault == 6u) ++receipt.boundary.end;
    if (fault == 7u) ++receipt.boundary.next_seq;
    if (fault == 8u) ++receipt.boundary.turns;
    struct snag_buf payloads[2] = {{.max = 4096u}, {.max = 4096u}};
    ++receipt.generation;
    assert(!snag_binary_checkpoint_receipt_encode(&payloads[0], &receipt));
    --receipt.generation;
    assert(!snag_binary_checkpoint_receipt_encode(&payloads[1], &receipt));
    struct snag_binary_record records[3] = {metadata, metadata, metadata};
    for (size_t i = 0u; i < 2u; ++i) {
        struct snag_binary_record *record = &records[i * 2u];
        record->payload = payloads[i].data;
        record->size = payloads[i].len;
        if (fault == 9u) memset(payloads[i].data, 0, 8u);
        if (fault == 10u) record->flags = 0u;
        if (fault == 11u) --record->size;
        if (fault == 12u) record->version = 2u;
        if (fault == 13u) payloads[i].data[138u] = 3u;
    }
    append_batch(fixture, 4u, records, 3u, 2u);
    append_batch(fixture, 5u, &data, 1u, 3u);
    metadata.version = 2u;
    metadata.payload = NULL;
    metadata.size = 0u;
    append_batch(fixture, 6u, &metadata, 1u, 3u);
    assert(!snag_truncate(fixture->fd, 0));
    assert(snag_seek(fixture->fd, 0, SEEK_SET) == 0);
    assert(!snag_write_full(fixture->fd, fixture->wire.data, fixture->wire.len));
    assert(snag_seek(fixture->fd, 13, SEEK_SET) == 13);
    for (size_t i = 0u; i < 2u; ++i) snag_buf_free(&payloads[i]);
    snag_buf_free(&old);
}

static void
check_find(struct receipt_fixture *fixture, const struct snag_binary_anchor *through,
    uint64_t floor, const unsigned char *const images[2], struct snag_buf *scratch, int expected)
{
    struct snag_binary_checkpoint_receipt found[2], saved[2];
    memset(found, 0x5a, sizeof(found));
    memcpy(saved, found, sizeof(found));
    int rc = snag_binary_checkpoint_receipts_find(fixture->fd, through, floor,
        images, scratch, found, NULL, NULL);
    assert(rc == expected);
    for (size_t i = 0u; i < 2u; ++i) {
        if (rc > 0 && ((unsigned)rc & (1u << i))) {
            assert(found[i].generation == fixture->receipts[i].generation);
            assert(!memcmp(found[i].index_root, fixture->receipts[i].index_root, 32u));
            struct snag_binary_checkpoint_frame frame;
            assert(!snag_binary_checkpoint_frame_from_receipt(fixture->images[i].data,
                fixture->images[i].len, &fixture->identity, &found[i], &frame));
        } else {
            assert(!memcmp(&found[i], &saved[i], sizeof(found[i])));
        }
    }
    assert(snag_seek(fixture->fd, 0, SEEK_CUR) == 13);
}

struct cancellation {
    unsigned calls, stop;
};

static bool
cancelled(void *opaque)
{
    struct cancellation *cancel = opaque;
    return ++cancel->calls == cancel->stop;
}

void
test_store_binary_receipts_find(void)
{
    char *path = snag_path_join(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
        "snag-binary-receipts-XXXXXX");
    assert(path);
    struct receipt_fixture fixture = {.fd = mkstemp(path)};
    assert(fixture.fd >= 0);
    assert(!unlink(path));
    free(path);
    build_fixture(&fixture, 0u);
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    const unsigned char *images[2] = {
        fixture.receipts[0].image_digest, fixture.receipts[1].image_digest
    };
    struct snag_binary_anchor *through = &fixture.anchors[6];
    for (uint64_t floor = SNAG_BINARY_HEADER_SIZE; floor <= through->end; ++floor) {
        int expected = (floor <= fixture.anchors[3].end ? 1 : 0) |
            (floor <= fixture.anchors[1].end ? 2 : 0);
        check_find(&fixture, through, floor, images, &scratch, expected);
    }
    for (size_t i = 0u; i < 7u; ++i) {
        int expected = i < 2u ? 0 : i < 4u ? 2 : 3;
        check_find(&fixture, &fixture.anchors[i], SNAG_BINARY_HEADER_SIZE,
            images, &scratch, expected);
    }
    for (unsigned present = 0u; present < 4u; ++present) {
        const unsigned char *selected[2] = {
            present & 1u ? images[0] : NULL, present & 2u ? images[1] : NULL
        };
        check_find(&fixture, through, SNAG_BINARY_HEADER_SIZE,
            selected, &scratch, (int)present);
    }
    unsigned char unknown[32] = {0};
    const unsigned char *missing[2] = {unknown, images[1]};
    check_find(&fixture, through, SNAG_BINARY_HEADER_SIZE, missing, &scratch, 2);
    /* Both image names may have the same digest; each slot is independently pinned. */
    const unsigned char *duplicates[2] = {images[0], images[0]};
    struct snag_binary_checkpoint_receipt found[2], saved[2];
    assert(snag_binary_checkpoint_receipts_find(fixture.fd, through,
        SNAG_BINARY_HEADER_SIZE, duplicates, &scratch, found, NULL, NULL) == 3);
    assert(found[0].generation == 20u && found[1].generation == 20u);
    for (unsigned stop = 1u; stop <= 6u; ++stop) {
        struct cancellation cancel = {.stop = stop};
        memset(found, 0x5a, sizeof(found));
        memcpy(saved, found, sizeof(found));
        int rc = snag_binary_checkpoint_receipts_find(fixture.fd, through,
            SNAG_BINARY_HEADER_SIZE, images, &scratch, found, cancelled, &cancel);
        if (stop <= 5u) {
            assert(rc == -1 && errno == ECANCELED);
            assert(!memcmp(found, saved, sizeof(found)));
        } else {
            assert(rc == 3 && cancel.calls == 5u);
        }
        assert(snag_seek(fixture.fd, 0, SEEK_CUR) == 13);
    }
    /* Copy image keys before the first scratch read invalidates their views. */
    snag_buf_reset(&scratch);
    assert(!snag_buf_append(&scratch, images[0], 32u));
    assert(!snag_buf_append(&scratch, images[1], 32u));
    const unsigned char *borrowed[2] = {scratch.data, scratch.data + 32u};
    check_find(&fixture, through, SNAG_BINARY_HEADER_SIZE, borrowed, &scratch, 3);
    for (unsigned fault = 1u; fault <= 14u; ++fault) {
        build_fixture(&fixture, fault);
        check_find(&fixture, through, SNAG_BINARY_HEADER_SIZE, images, &scratch,
            fault <= 11u ? -1 : fault <= 13u ? 2 : 3);
    }
    build_fixture(&fixture, 0u);
    /* The first image needs no lifetime-prefix scan. Its ancestor is batch 3;
     * old damage matters only when the second image's older capture is used. */
    unsigned char changed = fixture.wire.data[SNAG_BINARY_HEADER_SIZE + 20u] ^ 1u;
    assert(snag_seek(fixture.fd, SNAG_BINARY_HEADER_SIZE + 20u, SEEK_SET) ==
        SNAG_BINARY_HEADER_SIZE + 20u);
    assert(!snag_write_full(fixture.fd, &changed, 1u));
    assert(snag_seek(fixture.fd, 13, SEEK_SET) == 13);
    const unsigned char *one[2] = {images[0], NULL};
    check_find(&fixture, through, SNAG_BINARY_HEADER_SIZE, one, &scratch, 1);
    check_find(&fixture, through, SNAG_BINARY_HEADER_SIZE, images, &scratch, -1);
    build_fixture(&fixture, 0u);
    assert(!snag_truncate(fixture.fd, (int64_t)through->end - 1));
    check_find(&fixture, through, SNAG_BINARY_HEADER_SIZE, images, &scratch, -1);
    struct snag_binary_identity identity;
    struct snag_binary_anchor committed;
    uint64_t incomplete;
    assert(!snag_binary_journal_tail(fixture.fd, through->end - 1u, &scratch,
        &identity, &committed, &incomplete));
    assert(committed.end == fixture.anchors[5].end && incomplete);
    check_find(&fixture, &committed, SNAG_BINARY_HEADER_SIZE, images, &scratch, 3);
    build_fixture(&fixture, 0u);
    check_find(&fixture, through, SNAG_BINARY_HEADER_SIZE - 1u, images, &scratch, -1);
    check_find(&fixture, through, through->end + 1u, images, &scratch, -1);
    struct snag_buf tiny = {.max = 1u};
    check_find(&fixture, through, SNAG_BINARY_HEADER_SIZE, images, &tiny, -1);
    snag_buf_free(&tiny);
    memset(found, 0x5a, sizeof(found));
    memcpy(saved, found, sizeof(found));
    assert(snag_binary_checkpoint_receipts_find(-1, through, SNAG_BINARY_HEADER_SIZE,
        images, &scratch, found, NULL, NULL) < 0);
    assert(snag_binary_checkpoint_receipts_find(fixture.fd, NULL, SNAG_BINARY_HEADER_SIZE,
        images, &scratch, found, NULL, NULL) < 0);
    assert(snag_binary_checkpoint_receipts_find(fixture.fd, through, SNAG_BINARY_HEADER_SIZE,
        NULL, &scratch, found, NULL, NULL) < 0);
    assert(snag_binary_checkpoint_receipts_find(fixture.fd, through, SNAG_BINARY_HEADER_SIZE,
        images, NULL, found, NULL, NULL) < 0);
    assert(snag_binary_checkpoint_receipts_find(fixture.fd, through, SNAG_BINARY_HEADER_SIZE,
        images, &scratch, NULL, NULL, NULL) < 0);
    assert(!memcmp(found, saved, sizeof(found)));
    snag_buf_free(&scratch);
    for (size_t i = 0u; i < 2u; ++i) snag_buf_free(&fixture.images[i]);
    snag_buf_free(&fixture.wire);
    assert(!close(fixture.fd));
}
