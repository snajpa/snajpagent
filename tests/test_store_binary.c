/* SPDX-License-Identifier: GPL-2.0-only */
#include "fixture_store_binary.h"
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
test_references(const struct snag_binary_batch *batch)
{
    struct snag_binary_ref reference = {
        .sequence = UINT64_C(0x0807060504030201), .offset = 0x010203u, .size = 0x040506u
    };
    unsigned char encoded[SNAG_BINARY_REF_SIZE];
    assert(!snag_binary_ref_encode(encoded, &reference));
    assert_bytes(encoded, sizeof(encoded), "01020304050607080302010006050400");
    struct snag_binary_ref decoded;
    assert(!snag_binary_ref_decode(encoded, sizeof(encoded), &decoded));
    assert(decoded.sequence == reference.sequence && decoded.offset == reference.offset &&
        decoded.size == reference.size);
    for (size_t i = 0; i < sizeof(encoded); ++i) {
        struct snag_binary_ref kept = decoded;
        assert(snag_binary_ref_decode(encoded, i, &kept) < 0);
        assert(!memcmp(&kept, &decoded, sizeof(kept)));
    }
    assert(snag_binary_ref_decode(encoded, sizeof(encoded) + 1u, &decoded) < 0);
    unsigned char saved[sizeof(encoded)];
    memcpy(saved, encoded, sizeof(saved));
    reference.sequence = 0;
    assert(snag_binary_ref_encode(encoded, &reference) < 0);
    assert(!memcmp(encoded, saved, sizeof(encoded)));
    reference.sequence = UINT64_MAX;
    assert(snag_binary_ref_encode(encoded, &reference) < 0);
    reference.sequence = 1u;
    reference.offset = UINT32_MAX;
    reference.size = 2u;
    assert(snag_binary_ref_encode(encoded, &reference) < 0);
    reference.offset = SNAG_MAX_EVENT_LINE;
    reference.size = 1u;
    assert(snag_binary_ref_encode(encoded, &reference) < 0);
    reference.size = 0u;
    assert(!snag_binary_ref_encode(encoded, &reference));
    assert(!snag_binary_ref_decode(encoded, sizeof(encoded), &decoded));
    encoded[12] = 1u;
    assert(snag_binary_ref_decode(encoded, sizeof(encoded), &decoded) < 0);
    memset(encoded, 0, 8u);
    assert(snag_binary_ref_decode(encoded, sizeof(encoded), &decoded) < 0);

    reference = (struct snag_binary_ref){
        .sequence = batch->first_seq + 1u, .offset = 1u, .size = 3u
    };
    const unsigned char sentinel = 0u;
    const unsigned char *view = &sentinel;
    assert(snag_binary_ref_resolve(&reference, batch, 7u, 1u, &view) < 0);
    assert(view == &sentinel); /* Required state cannot refer to optional metadata. */
    reference.sequence = batch->first_seq;
    assert(!snag_binary_ref_resolve(&reference, batch, 7u, 1u, &view));
    static const unsigned char slice[] = {'b', 'c', 0};
    assert(!memcmp(view, slice, sizeof(slice)));
    view = &sentinel;
    assert(snag_binary_ref_resolve(&reference, batch, 8u, 1u, &view) < 0);
    assert(view == &sentinel);
    assert(snag_binary_ref_resolve(&reference, batch, 7u, 2u, &view) < 0);
    assert(view == &sentinel);
    reference.offset = 4u;
    reference.size = 2u;
    assert(snag_binary_ref_resolve(&reference, batch, 7u, 1u, &view) < 0);
    assert(view == &sentinel);
    reference.offset = 6u;
    reference.size = 0u;
    assert(snag_binary_ref_resolve(&reference, batch, 7u, 1u, &view) < 0);
    reference.offset = 5u;
    assert(!snag_binary_ref_resolve(&reference, batch, 7u, 1u, &view));
    reference.sequence = batch->first_seq - 1u;
    assert(snag_binary_ref_resolve(&reference, batch, 7u, 1u, &view) < 0);
    reference.sequence = batch->first_seq + batch->count;
    assert(snag_binary_ref_resolve(&reference, batch, 7u, 1u, &view) < 0);
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
assert_anchor(const struct snag_binary_anchor *a, const struct snag_binary_anchor *b)
{
    assert(a->end == b->end && a->next_seq == b->next_seq && a->turns == b->turns);
    assert(a->previous == b->previous && !memcmp(a->digest, b->digest, sizeof(a->digest)));
}

static void
test_encoded_anchor(const struct snag_binary_anchor *root)
{
    static const unsigned char payload[] = {'a', 0u, 'b'};
    struct snag_binary_record records[] = {
        {.kind = 77u, .version = 1u, .payload = payload, .size = sizeof(payload)},
        {.kind = 0x8fffu, .version = 1u, .flags = SNAG_BINARY_RECORD_OPTIONAL}
    };
    struct snag_buf encoded = {.max = SNAG_BINARY_BATCH_MAX};
    assert(!snag_buf_append(&encoded, "kept", 4u));
    struct snag_binary_anchor prepared;
    assert(!snag_binary_batch_encode(&encoded, root, records, 2u, 0u, &prepared));
    struct snag_binary_batch batch;
    struct snag_binary_anchor verified;
    assert(!snag_binary_batch_decode(encoded.data + 4u, encoded.len - 4u,
        root, &batch, &verified));
    assert_anchor(&prepared, &verified);
    size_t wire_size;
    assert(!snag_binary_wire_size(batch.size, &wire_size));
    assert(prepared.end == root->end + wire_size);
    size_t original_size = encoded.len;
    struct snag_binary_anchor cursor = *root;
    records[0].payload = encoded.data + 4u + SNAG_BINARY_BATCH_HEADER_SIZE +
        SNAG_BINARY_RECORD_HEADER_SIZE;
    assert(!snag_binary_batch_encode(&encoded, &cursor, records, 2u, 0u, &cursor));
    assert_anchor(&cursor, &verified);
    assert(encoded.len == original_size + batch.size &&
        !memcmp(encoded.data + 4u, encoded.data + original_size, batch.size));
    records[0].payload = payload;
    struct snag_binary_anchor sentinel = {.end = 17u, .next_seq = 19u, .turns = 3u};
    for (size_t fault = 0u; fault < 9u; ++fault) {
        struct snag_binary_anchor before = *root;
        struct snag_binary_record bad[2] = {records[0], records[1]};
        uint32_t count = 2u;
        if (fault == 0u) bad[0].kind = 0u;
        if (fault == 1u) bad[0].version = 0u;
        if (fault == 2u) bad[0].flags = 2u;
        if (fault == 3u) bad[0].size = SNAG_MAX_EVENT_LINE + 1u;
        if (fault == 4u) count = 0u;
        if (fault == 5u) count = UINT32_MAX;
        if (fault == 6u) {
            before.end = 200u;
            before.previous = root->end;
            before.next_seq = UINT64_MAX;
        }
        if (fault == 7u) {
            before.end = INT64_MAX - 1u;
            before.previous = root->end;
        }
        if (fault == 8u) {
            before.end = (uint64_t)INT64_MAX - batch.size;
            before.previous = root->end;
        }
        size_t kept = encoded.len;
        prepared = sentinel;
        assert(snag_binary_batch_encode(&encoded, &before, bad, count, 0u, &prepared) < 0);
        assert_anchor(&prepared, &sentinel);
        assert(encoded.len == kept && !memcmp(encoded.data, "kept", 4u));
    }
    struct snag_buf small = {.max = 4u + batch.size - 1u};
    assert(!snag_buf_append(&small, "kept", 4u));
    prepared = sentinel;
    assert(snag_binary_batch_encode(&small, root, records, 2u, 0u, &prepared) < 0);
    assert(errno == EOVERFLOW && small.len == 4u && !memcmp(small.data, "kept", 4u));
    assert_anchor(&prepared, &sentinel);
    assert(snag_binary_batch_encode(NULL, root, records, 2u, 0u, &prepared) < 0);
    assert(snag_binary_batch_encode(&encoded, NULL, records, 2u, 0u, &prepared) < 0);
    assert(snag_binary_batch_encode(&encoded, root, NULL, 2u, 0u, &prepared) < 0);
    assert_anchor(&prepared, &sentinel);
    snag_buf_free(&small);
    snag_buf_free(&encoded);
}

static void
reject_previous(int fd, const struct snag_binary_anchor *after, struct snag_buf *scratch)
{
    struct snag_binary_anchor before = *after;
    struct snag_binary_anchor saved = before;
    struct snag_binary_batch batch = {.data = (const unsigned char *)"unchanged", .size = 7u};
    struct snag_binary_batch original;
    memcpy(&original, &batch, sizeof(original));
    assert(snag_binary_batch_previous(fd, after, scratch, &batch, &before) < 0);
    assert_anchor(&before, &saved);
    assert(!memcmp(&batch, &original, sizeof(batch)));
    assert(snag_seek(fd, 0, SEEK_CUR) == 13);
}

static void
restore_file(int fd, const unsigned char *header, const struct snag_buf *joined)
{
    assert(!snag_truncate(fd, 0));
    assert(snag_seek(fd, 0, SEEK_SET) == 0);
    assert(!snag_write_full(fd, header, SNAG_BINARY_HEADER_SIZE));
    assert(!snag_write_full(fd, joined->data, joined->len));
    assert(snag_seek(fd, 13, SEEK_SET) == 13);
}

static void
reject_at(int fd, uint64_t boundary, uint64_t offset, const unsigned char hash[32],
    struct snag_buf *scratch)
{
    struct snag_binary_anchor before = {.end = 41u};
    struct snag_binary_anchor after = {.end = 42u};
    struct snag_binary_anchor saved_before = before;
    struct snag_binary_anchor saved_after = after;
    struct snag_binary_batch batch = {.data = (const unsigned char *)"kept", .size = 7u};
    struct snag_binary_batch saved;
    memcpy(&saved, &batch, sizeof(saved));
    assert(snag_binary_batch_at(fd, boundary, offset, hash, scratch, &batch, &before, &after) < 0);
    assert_anchor(&before, &saved_before);
    assert_anchor(&after, &saved_after);
    assert(!memcmp(&saved, &batch, sizeof(saved)));
    if (fd >= 0) assert(snag_seek(fd, 0, SEEK_CUR) == 13);
}

static void
test_direct_reader(int fd, const struct snag_buf *joined, const struct snag_binary_anchor *root,
    const struct snag_binary_anchor *middle, const struct snag_binary_anchor *tail)
{
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    const struct snag_binary_anchor *ends[] = {middle, tail};
    const struct snag_binary_anchor *starts[] = {root, middle};
    for (size_t i = 0u; i < 2u; ++i) {
        struct snag_binary_batch batch;
        struct snag_binary_anchor before;
        struct snag_binary_anchor after;
        assert(!snag_binary_batch_at(fd, tail->end, starts[i]->end, ends[i]->digest,
            &scratch, &batch, &before, &after));
        assert_anchor(&before, starts[i]);
        assert_anchor(&after, ends[i]);
        struct snag_buf expected = {.max = SNAG_BINARY_BATCH_MAX};
        size_t size = (size_t)(ends[i]->end - starts[i]->end);
        assert(!snag_binary_wire_decode(&expected,
            joined->data + starts[i]->end - root->end, size));
        assert(batch.size == expected.len && !memcmp(batch.data, expected.data, expected.len));
        snag_buf_free(&expected);
        assert(!snag_binary_batch_at(fd, ends[i]->end, starts[i]->end, ends[i]->digest,
            &scratch, &batch, &before, &after));
    }
    reject_at(fd, tail->end - 1u, middle->end, tail->digest, &scratch);
    reject_at(fd, UINT64_MAX, middle->end, tail->digest, &scratch);
    const uint64_t offsets[] = {0u, root->end - 1u, middle->end + 1u, tail->end, UINT64_MAX};
    for (size_t i = 0u; i < 5u; ++i) {
        reject_at(fd, tail->end, offsets[i], tail->digest, &scratch);
    }
    for (size_t i = 0u; i < sizeof(tail->digest); ++i) {
        unsigned char bad[32];
        memcpy(bad, tail->digest, sizeof(bad));
        bad[i] ^= 1u;
        reject_at(fd, tail->end, middle->end, bad, &scratch);
    }
    reject_at(fd, tail->end, middle->end, NULL, &scratch);
    reject_at(-1, tail->end, middle->end, tail->digest, &scratch);
    struct snag_buf small = {.max = 1u};
    reject_at(fd, tail->end, middle->end, tail->digest, &small);
    assert(errno == EOVERFLOW && !small.len);
    snag_buf_free(&small);
    snag_buf_free(&scratch);
}

static void
test_backward_reader(int fd, const unsigned char *header, const struct snag_buf *joined,
    const struct snag_binary_anchor *root, const struct snag_binary_anchor *middle,
    const struct snag_binary_anchor *tail)
{
    test_direct_reader(fd, joined, root, middle, tail);
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_batch batch;
    struct snag_binary_anchor before;
    assert(!snag_binary_batch_previous(fd, tail, &scratch, &batch, &before));
    assert_anchor(&before, middle);
    assert(batch.first_seq == middle->next_seq);
    struct snag_buf expected = {.max = SNAG_BINARY_BATCH_MAX};
    assert(!snag_binary_wire_decode(&expected, joined->data + middle->end - root->end,
        (size_t)(tail->end - middle->end)));
    assert(batch.size == expected.len && !memcmp(batch.data, expected.data, expected.len));
    snag_buf_free(&expected);
    /* The output cursor may alias the input; its anchor works forward too. */
    struct snag_binary_anchor cursor = before;
    assert(!snag_binary_batch_previous(fd, &cursor, &scratch, &batch, &cursor));
    assert_anchor(&cursor, root);
    struct snag_binary_batch saved;
    memcpy(&saved, &batch, sizeof(saved));
    before = *tail;
    assert(snag_binary_batch_previous(fd, root, &scratch, &batch, &before) == 1);
    assert_anchor(&before, tail);
    assert(!memcmp(&saved, &batch, sizeof(saved)));
    assert(!snag_binary_batch_read(fd, tail->end, &cursor, &scratch, &batch, &before));
    assert_anchor(&before, middle);
    for (uint64_t seq = 1u; seq < tail->next_seq; ++seq) {
        assert(!snag_binary_batch_find(fd, tail, seq, &scratch, &batch, &before));
        assert_anchor(&before, seq < middle->next_seq ? root : middle);
        assert(seq >= batch.first_seq && seq - batch.first_seq < batch.count);
    }
    memcpy(&saved, &batch, sizeof(saved));
    cursor = before;
    const uint64_t invalid_seq[] = {0u, tail->next_seq, UINT64_MAX};
    for (size_t i = 0u; i < 3u; ++i) {
        assert(snag_binary_batch_find(fd, tail, invalid_seq[i], &scratch, &batch, &before) < 0);
        assert_anchor(&before, &cursor);
        assert(!memcmp(&saved, &batch, sizeof(saved)));
    }
    assert(snag_seek(fd, 0, SEEK_CUR) == 13);
    for (unsigned int i = 0u; i < 5u; ++i) {
        cursor = *tail;
        if (!i) ++cursor.end;
        else if (i == 1u) ++cursor.next_seq;
        else if (i == 2u) ++cursor.turns;
        else if (i == 3u) ++cursor.previous;
        else cursor.digest[0] ^= 1u;
        reject_previous(fd, &cursor, &scratch);
    }
    cursor = *root;
    cursor.digest[0] ^= 1u;
    reject_previous(fd, &cursor, &scratch);
    struct snag_buf small = {.max = 1u};
    reject_previous(fd, tail, &small);
    assert(errno == EOVERFLOW && !small.len);
    snag_buf_free(&small);

    /* Corrupt the current record, predecessor timestamp, predecessor turn count
     * and header identity independently. The digest field alone stays unchanged
     * for the predecessor cases: a footer-only check would not authenticate it. */
    const uint64_t positions[] = {
        middle->end + SNAG_BINARY_BATCH_HEADER_SIZE + 16u,
        root->end + SNAG_BINARY_BATCH_HEADER_SIZE + 16u,
        middle->end - SNAG_BINARY_BATCH_FOOTER_SIZE + 32u, 32u
    };
    for (size_t i = 0u; i < 4u; ++i) {
        unsigned char byte = positions[i] < root->end ? header[positions[i]] :
            joined->data[positions[i] - root->end];
        byte ^= 1u;
        assert(snag_seek(fd, (int64_t)positions[i], SEEK_SET) == (int64_t)positions[i]);
        assert(!snag_write_full(fd, &byte, 1u));
        assert(snag_seek(fd, 13, SEEK_SET) == 13);
        reject_previous(fd, tail, &scratch);
        reject_at(fd, tail->end, middle->end, tail->digest, &scratch);
        restore_file(fd, header, joined);
    }
    /* A different self-consistent file identity also cannot supply this root. */
    unsigned char foreign_header[SNAG_BINARY_HEADER_SIZE];
    struct snag_binary_identity foreign;
    struct snag_binary_anchor foreign_root;
    assert(!snag_binary_header_decode(header, SNAG_BINARY_HEADER_SIZE, &foreign, &foreign_root));
    foreign.id[0] ^= 1u;
    snag_binary_header_encode(foreign_header, &foreign);
    assert(snag_seek(fd, 0, SEEK_SET) == 0);
    assert(!snag_write_full(fd, foreign_header, sizeof(foreign_header)));
    assert(snag_seek(fd, 13, SEEK_SET) == 13);
    reject_previous(fd, tail, &scratch);
    reject_previous(fd, root, &scratch);
    reject_at(fd, tail->end, root->end, middle->digest, &scratch);
    restore_file(fd, header, joined);
    assert(!snag_truncate(fd, (int64_t)tail->end - 1));
    reject_previous(fd, tail, &scratch);
    assert(errno == EIO);
    reject_at(fd, tail->end, middle->end, tail->digest, &scratch);
    assert(errno == EIO);
    assert(!snag_truncate(fd, SNAG_BINARY_HEADER_SIZE - 1u));
    reject_previous(fd, root, &scratch);
    assert(errno == EIO);
    restore_file(fd, header, joined);
    /* Appends beyond the captured committed boundary do not move that snapshot. */
    assert(snag_seek(fd, (int64_t)tail->end, SEEK_SET) == (int64_t)tail->end);
    assert(!snag_write_full(fd, "uncommitted", 11u));
    assert(snag_seek(fd, 13, SEEK_SET) == 13);
    assert(!snag_binary_batch_previous(fd, tail, &scratch, &batch, &before));
    assert_anchor(&before, middle);
    test_direct_reader(fd, joined, root, middle, tail);
    restore_file(fd, header, joined);
    snag_buf_free(&scratch);
}

/* Every non-final byte must stay nonzero, including in a truncated candidate.
 * A valid header declaring more bytes cannot hide an already closed envelope. */
static void
test_wire_tails(int fd, const struct snag_binary_anchor *before, const struct snag_buf *joined,
    uint64_t boundary, struct snag_buf *scratch)
{
    size_t start = (size_t)(before->end - SNAG_BINARY_HEADER_SIZE);
    size_t size = joined->len - start;
    struct snag_binary_batch batch = {.data = (const unsigned char *)"kept", .size = 4u};
    struct snag_binary_batch saved;
    memcpy(&saved, &batch, sizeof(saved));
    for (size_t i = 0u; i + 1u < size; ++i) {
        unsigned char zero = 0u;
        assert(joined->data[start + i] != 0u);
        int64_t position = (int64_t)(before->end + i);
        assert(snag_seek(fd, position, SEEK_SET) == position);
        assert(!snag_write_full(fd, &zero, 1u));
        struct snag_binary_anchor next = *before;
        assert(snag_binary_batch_read(fd, (uint64_t)position + 1u, before,
            scratch, &batch, &next) < 0);
        assert_anchor(&next, before);
        assert(!memcmp(&batch, &saved, sizeof(saved)));
        assert(snag_binary_batch_read(fd, boundary, before, scratch, &batch, &next) < 0);
        assert_anchor(&next, before);
        assert(snag_seek(fd, position, SEEK_SET) == position);
        assert(!snag_write_full(fd, joined->data + start + i, 1u));
    }
    unsigned char one = 1u;
    assert(snag_seek(fd, (int64_t)boundary - 1, SEEK_SET) == (int64_t)boundary - 1);
    assert(!snag_write_full(fd, &one, 1u));
    struct snag_binary_anchor next = *before;
    assert(snag_binary_batch_read(fd, boundary, before, scratch, &batch, &next) < 0);
    assert_anchor(&next, before);
    assert(snag_binary_batch_read(fd, boundary - 1u, before, scratch, &batch, &next) == 1);

    struct snag_buf raw = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_buf wire = {.max = SNAG_BINARY_WIRE_BATCH_MAX};
    assert(!snag_binary_wire_decode(&raw, joined->data + start, size));
    uint64_t larger = raw.len + 500u;
    for (size_t i = 0u; i < 8u; ++i) raw.data[8u + i] = (unsigned char)(larger >> (8u * i));
    rehash(raw.data, SNAG_BINARY_BATCH_HEADER_SIZE);
    assert(!snag_binary_wire_encode(&wire, raw.data, raw.len));
    assert(wire.len == size);
    assert(snag_seek(fd, (int64_t)before->end, SEEK_SET) == (int64_t)before->end);
    assert(!snag_write_full(fd, wire.data, wire.len));
    assert(snag_binary_batch_read(fd, boundary, before, scratch, &batch, &next) < 0);
    assert_anchor(&next, before);
    assert(!memcmp(&batch, &saved, sizeof(saved)));
    assert(scratch->len == SNAG_BINARY_BATCH_HEADER_SIZE);
    assert(snag_seek(fd, (int64_t)before->end, SEEK_SET) == (int64_t)before->end);
    assert(!snag_write_full(fd, joined->data + start, size));
    assert(snag_seek(fd, 13, SEEK_SET) == 13);
    snag_buf_free(&raw);
    snag_buf_free(&wire);
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
    test_backward_reader(fd, header, joined, anchor, next, &candidate);
    for (uint64_t cut = next->end; cut < boundary; ++cut) {
        candidate = *next;
        assert(snag_binary_batch_read(fd, cut, next, &scratch, &batch, &candidate) == 1);
        assert(!memcmp(&candidate, next, sizeof(candidate)));
        assert(scratch.len <= SNAG_BINARY_BATCH_HEADER_SIZE);
    }
    test_wire_tails(fd, next, joined, boundary, &scratch);
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
    assert(scratch.len <= SNAG_BINARY_BATCH_HEADER_SIZE);
    snag_buf_free(&scratch);
    assert(!close(fd));
    assert(!unlink(path));
    free(path);
}

static void
test_direct_isolation(int fd, const struct snag_binary_anchor *root,
    const struct snag_binary_anchor *tail)
{
    struct snag_buf encoded = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_anchor anchors[3] = {*tail};
    struct snag_binary_record record = {.kind = 77u, .version = 1u,
        .payload = (const unsigned char *)"direct", .size = 6u};
    struct snag_binary_batch batch;
    assert(snag_seek(fd, (int64_t)tail->end, SEEK_SET) == (int64_t)tail->end);
    for (size_t i = 0u; i < 2u; ++i) {
        snag_buf_reset(&encoded);
        assert(!snag_binary_batch_encode(&encoded, &anchors[i], &record, 1u, tail->turns, NULL));
        assert(!snag_binary_batch_decode(encoded.data, encoded.len,
            &anchors[i], &batch, &anchors[i + 1u]));
        assert(!binary_fixture_write(fd, encoded.data, encoded.len));
    }
    /* The fourth batch and its predecessor are sufficient. Damage elsewhere in
     * the immutable prefix is discovered when that history is actually queried. */
    uint64_t damaged = root->end + SNAG_BINARY_BATCH_HEADER_SIZE + SNAG_BINARY_RECORD_HEADER_SIZE;
    unsigned char original;
    assert(snag_pread(fd, &original, 1u, (int64_t)damaged) == 1);
    unsigned char bad = original ^ 1u;
    assert(snag_seek(fd, (int64_t)damaged, SEEK_SET) == (int64_t)damaged);
    assert(!snag_write_full(fd, &bad, 1u));
    assert(snag_seek(fd, 13, SEEK_SET) == 13);
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_anchor before;
    struct snag_binary_anchor after;
    assert(!snag_binary_batch_at(fd, anchors[2].end, anchors[1].end, anchors[2].digest,
        &scratch, &batch, &before, &after));
    assert_anchor(&before, &anchors[1]);
    assert_anchor(&after, &anchors[2]);
    assert(!memcmp(batch.data, encoded.data, encoded.len));
    assert(snag_binary_batch_find(fd, &anchors[2], 1u, &scratch, &batch, &before) < 0);
    assert(snag_seek(fd, (int64_t)damaged, SEEK_SET) == (int64_t)damaged);
    assert(!snag_write_full(fd, &original, 1u));
    /* Every physical byte, including the delimiter, is checked. */
    struct snag_buf wire = {.max = SNAG_BINARY_WIRE_BATCH_MAX};
    assert(!snag_binary_wire_encode(&wire, encoded.data, encoded.len));
    for (size_t i = 0u; i < wire.len; ++i) {
        int64_t position = (int64_t)(anchors[1].end + i);
        bad = wire.data[i] ^ 1u;
        assert(snag_seek(fd, position, SEEK_SET) == position);
        assert(!snag_write_full(fd, &bad, 1u));
        assert(snag_seek(fd, 13, SEEK_SET) == 13);
        reject_at(fd, anchors[2].end, anchors[1].end, anchors[2].digest, &scratch);
        assert(snag_seek(fd, position, SEEK_SET) == position);
        assert(!snag_write_full(fd, wire.data + i, 1u));
    }
    /* Rechecksummed unsafe lengths still fail before allocating batch storage. */
    unsigned char invalid_header[SNAG_BINARY_BATCH_HEADER_SIZE];
    memcpy(invalid_header, encoded.data, sizeof(invalid_header));
    memset(invalid_header + 8u, 0xff, 8u);
    rehash(invalid_header, sizeof(invalid_header));
    assert(snag_seek(fd, (int64_t)anchors[1].end, SEEK_SET) == (int64_t)anchors[1].end);
    snag_buf_reset(&wire);
    assert(!snag_binary_wire_encode(&wire, invalid_header, sizeof(invalid_header)));
    assert(!snag_write_full(fd, wire.data, wire.len - 1u));
    assert(snag_seek(fd, 13, SEEK_SET) == 13);
    struct snag_buf empty = {.max = SNAG_BINARY_BATCH_MAX};
    reject_at(fd, anchors[2].end, anchors[1].end, anchors[2].digest, &empty);
    assert(!empty.data && !empty.cap && !empty.len);
    assert(snag_seek(fd, (int64_t)anchors[1].end, SEEK_SET) == (int64_t)anchors[1].end);
    assert(!binary_fixture_write(fd, encoded.data, encoded.len));
    assert(snag_seek(fd, 13, SEEK_SET) == 13);
    snag_buf_free(&scratch);
    snag_buf_free(&wire);
    snag_buf_free(&encoded);
}

static void
test_backward_special(const unsigned char *header, const struct snag_binary_anchor *root)
{
    struct snag_binary_record record = {.kind = 77u, .version = 1u,
        .payload = (const unsigned char *)"nested", .size = 6u};
    struct snag_binary_anchor fake = *root;
    fake.end += SNAG_BINARY_BATCH_HEADER_SIZE + SNAG_BINARY_RECORD_HEADER_SIZE;
    fake.previous = root->end;
    struct snag_buf nested = {.max = SNAG_BINARY_BATCH_MAX};
    assert(!snag_binary_batch_encode(&nested, &fake, &record, 1u, 0u, NULL));
    struct snag_binary_batch batch;
    struct snag_binary_anchor next;
    /* A payload can contain a self-consistent decoded batch image. Its physical
     * envelope and fabricated predecessor cannot establish journal membership. */
    assert(!snag_binary_batch_decode(nested.data, nested.len, &fake, &batch, &next));
    struct snag_binary_ref reference = {.sequence = 1u, .size = 6u};
    const unsigned char *view;
    assert(!snag_binary_ref_resolve(&reference, &batch, 77u, 1u, &view));
    assert(!memcmp(view, "nested", 6u));
    unsigned char *large = calloc(1u, SNAG_MAX_EVENT_LINE);
    assert(large);
    memcpy(large, nested.data, nested.len);
    struct snag_binary_record outer = {.kind = 0x8000u, .version = 1u,
        .flags = SNAG_BINARY_RECORD_OPTIONAL, .payload = large, .size = SNAG_MAX_EVENT_LINE};
    struct snag_buf first = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_buf second = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_anchor middle;
    struct snag_binary_anchor tail;
    assert(!snag_binary_batch_encode(&first, root, &outer, 1u, 2u, NULL));
    assert(first.len == SNAG_BINARY_BATCH_MAX);
    assert(!snag_binary_batch_decode(first.data, first.len, root, &batch, &middle));
    assert(!snag_binary_batch_encode(&second, &middle, &record, 1u, 3u, NULL));
    assert(!snag_binary_batch_decode(second.data, second.len, &middle, &batch, &tail));
    char *path = snag_path_join(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
        "snajpagent-backward-XXXXXX");
    assert(path);
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(!snag_write_full(fd, header, SNAG_BINARY_HEADER_SIZE));
    assert(!binary_fixture_write(fd, first.data, first.len));
    assert(!binary_fixture_write(fd, second.data, second.len));
    assert(snag_seek(fd, 13, SEEK_SET) == 13);
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_anchor before;
    assert(!snag_binary_batch_previous(fd, &tail, &scratch, &batch, &before));
    assert_anchor(&before, &middle);
    assert(!snag_binary_batch_find(fd, &tail, 1u, &scratch, &batch, &before));
    assert_anchor(&before, root);
    assert(batch.size == SNAG_BINARY_BATCH_MAX && batch.count == 1u);
    view = (const unsigned char *)"unchanged";
    const unsigned char *saved = view;
    assert(snag_binary_ref_resolve(&reference, &batch, 77u, 1u, &view) < 0 && view == saved);
    size_t cursor = SNAG_BINARY_BATCH_HEADER_SIZE;
    struct snag_binary_record found;
    uint64_t sequence;
    assert(!snag_binary_record_next(&batch, &cursor, &found, &sequence));
    assert(sequence == 1u && found.kind == 0x8000u && found.size == SNAG_MAX_EVENT_LINE);
    assert(!memcmp(found.payload, nested.data, nested.len));
    assert(!snag_binary_batch_find(fd, &tail, 2u, &scratch, &batch, &before));
    reference.sequence = 2u;
    assert(!snag_binary_ref_resolve(&reference, &batch, 77u, 1u, &view));
    assert(!memcmp(view, "nested", 6u));
    assert(snag_seek(fd, 0, SEEK_CUR) == 13);

    struct snag_binary_anchor direct_after;
    assert(!snag_binary_batch_at(fd, tail.end, root->end, middle.digest,
        &scratch, &batch, &before, &direct_after));
    assert_anchor(&before, root);
    assert_anchor(&direct_after, &middle);
    assert(batch.size == SNAG_BINARY_BATCH_MAX);
    assert(!snag_binary_batch_at(fd, tail.end, middle.end, tail.digest,
        &scratch, &batch, &before, &direct_after));
    assert_anchor(&before, &middle);
    assert_anchor(&direct_after, &tail);
    reject_at(fd, tail.end, fake.end, next.digest, &scratch);
    test_direct_isolation(fd, root, &tail);

    /* Both batches have valid hashes, but the child lies about cumulative turns.
     * Recover the authenticated parent counter instead of assuming zero. */
    fake = middle;
    fake.turns = 0u;
    snag_buf_reset(&second);
    assert(!snag_binary_batch_encode(&second, &fake, &record, 1u, 1u, NULL));
    assert(!snag_binary_batch_decode(second.data, second.len, &fake, &batch, &next));
    assert(snag_seek(fd, (int64_t)middle.end, SEEK_SET) == (int64_t)middle.end);
    assert(!binary_fixture_write(fd, second.data, second.len));
    assert(snag_seek(fd, 13, SEEK_SET) == 13);
    reject_previous(fd, &next, &scratch);
    assert(errno == EINVAL);
    reject_at(fd, next.end, middle.end, next.digest, &scratch);
    assert(errno == EINVAL);
    assert(!close(fd));
    assert(!unlink(path));
    free(path);
    snag_buf_free(&nested);
    snag_buf_free(&first);
    snag_buf_free(&second);
    snag_buf_free(&scratch);
    free(large);
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

static void
checkpoint_decode_fails(const void *data, size_t size,
    const struct snag_binary_identity *identity, const struct snag_binary_anchor *anchor, int rc)
{
    struct snag_binary_checkpoint_frame frame = {.generation = 73u,
        .core = {.version = 9u, .data = (const unsigned char *)"kept", .size = 4u}};
    unsigned char before[sizeof(frame)];
    memcpy(before, &frame, sizeof(frame));
    assert(snag_binary_checkpoint_frame_decode(data, size, identity, anchor, &frame) == rc);
    assert(!memcmp(before, &frame, sizeof(frame)));
}

static void
checkpoint_stream_matches(const struct snag_binary_checkpoint_frame *frame,
    const unsigned char *expected, size_t total, size_t budget)
{
    struct snag_binary_checkpoint_encoder encoder;
    assert(!snag_binary_checkpoint_encoder_init(&encoder, frame));
    assert(encoder.total == total && !encoder.position);
    static const unsigned char sentinel[] = "kept";
    const unsigned char *chunk = sentinel;
    size_t size = 17u;
    struct snag_binary_checkpoint_encoder saved;
    memcpy(&saved, &encoder, sizeof(saved));
    assert(snag_binary_checkpoint_encoder_next(&encoder, 0u, &chunk, &size) < 0);
    assert(!memcmp(&saved, &encoder, sizeof(saved)) && size == 17u &&
        chunk == sentinel);
    assert(snag_binary_checkpoint_encoder_next(&encoder, budget, NULL, &size) < 0);
    assert(snag_binary_checkpoint_encoder_next(&encoder, budget, &chunk, NULL) < 0);
    assert(!memcmp(&saved, &encoder, sizeof(saved)));
    size_t offset = 0u;
    while (offset < total) {
        assert(!snag_binary_checkpoint_encoder_next(&encoder, budget, &chunk, &size));
        assert(size && size <= budget && size <= total - offset);
        assert(!memcmp(chunk, expected + offset, size));
        if (offset >= SNAG_BINARY_CHECKPOINT_HEADER_SIZE &&
            offset < SNAG_BINARY_CHECKPOINT_HEADER_SIZE + frame->core.size) {
            assert(chunk == frame->core.data + offset - SNAG_BINARY_CHECKPOINT_HEADER_SIZE);
        } else if (offset >= SNAG_BINARY_CHECKPOINT_HEADER_SIZE + frame->core.size &&
            offset < SNAG_BINARY_CHECKPOINT_HEADER_SIZE + frame->core.size + frame->provider.size) {
            assert(chunk == frame->provider.data + offset -
                SNAG_BINARY_CHECKPOINT_HEADER_SIZE - frame->core.size);
        } else if (offset >= SNAG_BINARY_CHECKPOINT_HEADER_SIZE + frame->core.size +
            frame->provider.size && offset < total - SNAG_BINARY_CHECKPOINT_FOOTER_SIZE) {
            assert(chunk == frame->access.data + offset - SNAG_BINARY_CHECKPOINT_HEADER_SIZE -
                frame->core.size - frame->provider.size);
        }
        offset += size;
        assert(encoder.position == offset);
    }
    memcpy(&saved, &encoder, sizeof(saved));
    const unsigned char *kept = chunk;
    size_t kept_size = size;
    assert(snag_binary_checkpoint_encoder_next(&encoder, budget, &chunk, &size) == 1);
    assert(snag_binary_checkpoint_encoder_next(&encoder, budget, &chunk, &size) == 1);
    assert(!memcmp(&saved, &encoder, sizeof(saved)) && chunk == kept && size == kept_size);
}

static void
test_checkpoint_access(struct snag_binary_checkpoint_frame frame)
{
    static const unsigned char access[] = {0u, 1u, 2u, 128u, 'a'};
    frame.access = (struct snag_binary_checkpoint_section){1u, access, sizeof(access)};
    struct snag_buf encoded = {.max = 1024u};
    assert(!snag_binary_checkpoint_frame_encode(&encoded, &frame));
    assert_bytes(encoded.data, encoded.len,
        "534e414743484b0000000200a0000000dc0000000000000088776655443322110001020304050607"
        "08090a0b0c0d0e0f0807060504030201470100000000000002000000000000000000000000000000"
        "6000000000000000779a3cacad07d9d88bf0606fefb185d27a0df6754016387dad0e71953699a30e"
        "01000200010000000300000000000000040000000000000005000000000000000000000000000000"
        "630072800100700001028061534e414743504500dc000000000000009b076b48e8f985f2fabd5148"
        "99685d032e3ce16bfed3c20e80b2d7164eb5a102");
    size_t length = encoded.len;
    for (size_t budget = 1u; budget <= length + 1u; ++budget) {
        checkpoint_stream_matches(&frame, encoded.data, length, budget);
    }
    checkpoint_stream_matches(&frame, encoded.data, length, SIZE_MAX);
    struct snag_binary_checkpoint_frame decoded;
    assert(!snag_binary_checkpoint_frame_decode(encoded.data, length,
        &frame.identity, &frame.boundary, &decoded));
    assert(decoded.access.version == 1u && decoded.access.size == sizeof(access) &&
        decoded.access.data == decoded.provider.data + decoded.provider.size &&
        !memcmp(decoded.access.data, access, sizeof(access)));
    for (size_t i = 0u; i < length; ++i) {
        checkpoint_decode_fails(encoded.data, i, &frame.identity, &frame.boundary, 1);
        encoded.data[i] ^= 1u;
        checkpoint_decode_fails(encoded.data, length, &frame.identity, &frame.boundary, -1);
        encoded.data[i] ^= 1u;
    }
    const size_t fields[] = {124u, 126u, 144u, 152u};
    for (size_t i = 0u; i < sizeof(fields) / sizeof(*fields); ++i) {
        unsigned char bad[220];
        assert(sizeof(bad) == length);
        memcpy(bad, encoded.data, length);
        bad[fields[i]] ^= 1u;
        rehash(bad, sizeof(bad));
        checkpoint_decode_fails(bad, sizeof(bad), &frame.identity, &frame.boundary, -1);
    }
    assert(!snag_binary_checkpoint_frame_encode(&encoded, &decoded));
    assert(encoded.len == 2u * length && !memcmp(encoded.data, encoded.data + length, length));
    for (unsigned int fault = 0u; fault < 6u; ++fault) {
        struct snag_binary_checkpoint_frame bad = frame;
        if (fault == 0u) bad.access.version = 0u;
        if (fault == 1u) bad.access.data = NULL;
        if (fault == 2u) bad.access.size = 0u;
        if (fault == 3u) bad.access.size = SIZE_MAX;
        if (fault == 4u) bad.access.size = SIZE_MAX - bad.core.size - bad.provider.size;
        if (fault == 5u) bad.access.size = INT64_MAX;
        struct snag_binary_checkpoint_encoder encoder, saved;
        memset(&encoder, 0x5a, sizeof(encoder));
        memcpy(&saved, &encoder, sizeof(saved));
        assert(snag_binary_checkpoint_encoder_init(&encoder, &bad) < 0);
        assert(errno == (fault < 3u ? EINVAL : EOVERFLOW));
        assert(!memcmp(&encoder, &saved, sizeof(saved)));
        assert(snag_binary_checkpoint_frame_encode(&encoded, &bad) < 0);
        assert(encoded.len == 2u * length && !memcmp(encoded.data, encoded.data + length, length));
    }
    snag_buf_free(&encoded);
}

static void
test_checkpoint_frames(const struct snag_binary_identity *identity,
    const struct snag_binary_anchor *anchor)
{
    /* These are framing test bytes, not core/provider state bodies. */
    static const unsigned char core[] = {'c', 0, 'r'};
    static const unsigned char provider[] = {128u, 1u, 0, 'p'};
    struct snag_binary_checkpoint_frame frame = {.identity = *identity, .boundary = *anchor,
        .generation = UINT64_C(0x1122334455667788),
        .core = {.version = 1u, .data = core, .size = sizeof(core)},
        .provider = {.version = 2u, .data = provider, .size = sizeof(provider)}};
    test_checkpoint_access(frame);
    struct snag_buf encoded = {.max = 1024u};
    assert(!snag_binary_checkpoint_frame_encode(&encoded, &frame));
    assert_bytes(encoded.data, encoded.len,
        "534e414743484b0000000200a0000000d70000000000000088776655443322110001020304050607"
        "08090a0b0c0d0e0f0807060504030201470100000000000002000000000000000000000000000000"
        "6000000000000000779a3cacad07d9d88bf0606fefb185d27a0df6754016387dad0e71953699a30e"
        "01000200000000000300000000000000040000000000000000000000000000000000000000000000"
        "63007280010070534e414743504500d7000000000000001d28ce02ba6bd34f99f4f82b4bf5c2ca9a"
        "c85d17d67999325382c69ac381f370");
    size_t length = encoded.len;
    /* Every small quantum crosses different header/body/footer/hash boundaries.
     * The fixture above is independently hashed, not generated by this stream. */
    for (size_t budget = 1u; budget <= length + 1u; ++budget) {
        checkpoint_stream_matches(&frame, encoded.data, length, budget);
    }
    checkpoint_stream_matches(&frame, encoded.data, length, SIZE_MAX);
    struct snag_binary_checkpoint_frame decoded;
    assert(!snag_binary_checkpoint_frame_decode(encoded.data, encoded.len,
        identity, anchor, &decoded));
    assert(decoded.generation == frame.generation &&
        !memcmp(&decoded.identity, identity, sizeof(*identity)) &&
        !memcmp(&decoded.boundary, anchor, sizeof(*anchor)));
    assert(decoded.core.version == 1u && decoded.core.size == sizeof(core) &&
        decoded.core.data == encoded.data + SNAG_BINARY_CHECKPOINT_HEADER_SIZE &&
        !memcmp(decoded.core.data, core, sizeof(core)));
    assert(decoded.provider.version == 2u && decoded.provider.size == sizeof(provider) &&
        decoded.provider.data == decoded.core.data + sizeof(core) &&
        !memcmp(decoded.provider.data, provider, sizeof(provider)));
    for (size_t i = 0; i < length; ++i) {
        checkpoint_decode_fails(encoded.data, i, identity, anchor, 1);
        encoded.data[i] ^= 1u;
        checkpoint_decode_fails(encoded.data, length, identity, anchor, -1);
        encoded.data[i] ^= 1u;
    }

    assert(!decoded.access.version && !decoded.access.size && !decoded.access.data);
    unsigned char old_format[215];
    memcpy(old_format, encoded.data, sizeof(old_format));
    old_format[10] = 1u;
    rehash(old_format, sizeof(old_format));
    checkpoint_decode_fails(old_format, sizeof(old_format), identity, anchor, -1);

    /* A rehashed envelope cannot change identity, any anchor member, framing,
     * reserved features or required section presence. */
    static const size_t malformed[] = {0u, 8u, 10u, 12u, 16u, 24u, 32u, 48u, 56u, 64u,
        72u, 80u, 88u, 120u, 122u, 124u, 127u, 128u, 136u, 144u, 151u, 152u, 159u,
        167u, 175u};
    for (size_t i = 0; i < sizeof(malformed) / sizeof(*malformed); ++i) {
        unsigned char bad[215];
        assert(sizeof(bad) == length);
        memcpy(bad, encoded.data, length);
        size_t offset = malformed[i];
        if (offset == 24u) memset(bad + offset, 0, 8u);
        else if (offset == 120u || offset == 122u) memset(bad + offset, 0, 2u);
        else bad[offset] ^= 1u;
        rehash(bad, sizeof(bad));
        checkpoint_decode_fails(bad, sizeof(bad), identity, anchor, -1);
    }
    static const size_t sizes[] = {16u, 128u, 136u};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i) {
        for (unsigned int fill = 0u; fill < 2u; ++fill) {
            unsigned char bad[215];
            memcpy(bad, encoded.data, length);
            memset(bad + sizes[i], fill ? 255 : 0, 8u);
            rehash(bad, sizeof(bad));
            checkpoint_decode_fails(bad, sizeof(bad), identity, anchor, -1);
            checkpoint_decode_fails(bad, SNAG_BINARY_CHECKPOINT_HEADER_SIZE,
                identity, anchor, -1);
        }
    }
    for (unsigned int fault = 0u; fault < 7u; ++fault) {
        struct snag_binary_identity foreign = *identity;
        struct snag_binary_anchor other = *anchor;
        if (fault == 0u) foreign.id[0] ^= 1u;
        if (fault == 1u) foreign.created_ms++;
        if (fault == 2u) other.end++;
        if (fault == 3u) other.next_seq++;
        if (fault == 4u) other.turns++;
        if (fault == 5u) other.previous++;
        if (fault == 6u) other.digest[0] ^= 1u;
        checkpoint_decode_fails(encoded.data, length, &foreign, &other, -1);
    }

    /* The final append may reallocate the same buffer that owns both views. */
    assert(!snag_binary_checkpoint_frame_encode(&encoded, &decoded));
    assert(encoded.len == 2u * length && !memcmp(encoded.data, encoded.data + length, length));
    checkpoint_decode_fails(encoded.data, encoded.len, identity, anchor, -1);
    checkpoint_decode_fails(encoded.data, length + 1u, identity, anchor, -1);
    assert(!snag_binary_checkpoint_frame_decode(encoded.data + length, length,
        identity, anchor, &decoded));
    encoded.len = length;

    for (unsigned int fault = 0u; fault < 16u; ++fault) {
        struct snag_binary_checkpoint_frame bad = frame;
        if (fault == 0u) bad.generation = 0u;
        if (fault == 1u) bad.core.version = 0u;
        if (fault == 2u) bad.provider.version = 0u;
        if (fault == 3u) bad.core.size = 0u;
        if (fault == 4u) bad.provider.size = 0u;
        if (fault == 5u) bad.core.data = NULL;
        if (fault == 6u) bad.provider.data = NULL;
        if (fault == 7u) bad.boundary.end = SNAG_BINARY_HEADER_SIZE - 1u;
        if (fault == 8u) bad.boundary.end = (uint64_t)INT64_MAX + 1u;
        if (fault == 9u) bad.boundary.next_seq = 0u;
        if (fault == 10u) bad.boundary.previous = bad.boundary.end;
        if (fault == 11u) bad.core.size = SIZE_MAX;
        if (fault == 12u) bad.provider.size = SIZE_MAX - bad.core.size;
        if (fault == 13u) bad.core.size = SIZE_MAX / 2u;
        if (fault == 14u) encoded.max = 2u * length - 1u;
        if (fault == 15u) encoded.max = encoded.len - 1u;
        if (fault < 13u) {
            struct snag_binary_checkpoint_encoder encoder;
            memset(&encoder, 0x5a, sizeof(encoder));
            struct snag_binary_checkpoint_encoder saved;
            memcpy(&saved, &encoder, sizeof(saved));
            assert(snag_binary_checkpoint_encoder_init(&encoder, &bad) < 0);
            assert(!memcmp(&encoder, &saved, sizeof(saved)));
        }
        struct snag_buf before = encoded;
        errno = 0;
        assert(snag_binary_checkpoint_frame_encode(&encoded, &bad) < 0);
        assert(errno == (fault < 11u ? EINVAL : EOVERFLOW));
        assert(!memcmp(&encoded, &before, sizeof(encoded)) &&
            !memcmp(encoded.data, encoded.data + length, length));
        encoded.max = 1024u;
    }
    checkpoint_decode_fails(NULL, 1u, identity, anchor, -1);
    checkpoint_decode_fails(NULL, 0u, identity, anchor, 1);
    checkpoint_decode_fails(encoded.data, length, NULL, anchor, -1);
    checkpoint_decode_fails(encoded.data, length, identity, NULL, -1);
    assert(snag_binary_checkpoint_frame_decode(encoded.data, length,
        identity, anchor, NULL) < 0);
    assert(snag_binary_checkpoint_frame_encode(NULL, &frame) < 0);
    assert(snag_binary_checkpoint_frame_encode(&encoded, NULL) < 0);
    struct snag_binary_checkpoint_encoder encoder = {0};
    assert(snag_binary_checkpoint_encoder_init(NULL, &frame) < 0);
    assert(snag_binary_checkpoint_encoder_init(&encoder, NULL) < 0);
    const unsigned char *unused = NULL;
    size_t unused_size = 0u;
    assert(snag_binary_checkpoint_encoder_next(NULL, 1u, &unused, &unused_size) < 0);
    assert(snag_binary_checkpoint_encoder_next(&encoder, 1u, &unused, &unused_size) < 0);

    /* Carry full-width generations and body versions to their own decoders;
     * framing never silently accepts an unknown version as known state. */
    encoded.len = 0u;
    frame.generation = UINT64_MAX;
    frame.core.version = UINT16_MAX;
    frame.provider.version = UINT16_MAX - 1u;
    assert(!snag_binary_checkpoint_frame_encode(&encoded, &frame));
    assert(!snag_binary_checkpoint_frame_decode(encoded.data, encoded.len,
        identity, anchor, &decoded));
    assert(decoded.generation == UINT64_MAX && decoded.core.version == UINT16_MAX &&
        decoded.provider.version == UINT16_MAX - 1u);

    struct snag_binary_record record = {.kind = 7u, .version = 1u, .payload = core,
        .size = sizeof(core), .timestamp_ms = identity->created_ms};
    struct snag_buf journal = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_batch batch;
    struct snag_binary_anchor next;
    assert(!snag_binary_batch_encode(&journal, anchor, &record, 1u, anchor->turns, NULL));
    assert(!snag_binary_batch_decode(journal.data, journal.len, anchor, &batch, &next));
    checkpoint_decode_fails(encoded.data, encoded.len, identity, &next, -1);
    frame.boundary = next;
    encoded.len = 0u;
    assert(!snag_binary_checkpoint_frame_encode(&encoded, &frame));
    checkpoint_decode_fails(encoded.data, encoded.len, identity, anchor, -1);
    assert(!snag_binary_checkpoint_frame_decode(encoded.data, encoded.len,
        identity, &next, &decoded));
    snag_buf_free(&journal);
    snag_buf_free(&encoded);

    /* Snapshot size follows complete active state, independently of event size. */
    size_t large_size = SNAG_MAX_EVENT_LINE + 1u;
    unsigned char *large = malloc(large_size);
    assert(large);
    memset(large, 0xabu, large_size);
    frame.core.data = large;
    frame.core.size = large_size;
    encoded.max = SIZE_MAX;
    assert(!snag_binary_checkpoint_frame_encode(&encoded, &frame));
    assert(!snag_binary_checkpoint_frame_decode(encoded.data, encoded.len,
        identity, &next, &decoded));
    assert(decoded.core.size == large_size && !memcmp(decoded.core.data, large, large_size));
    assert(decoded.provider.size == sizeof(provider) &&
        !memcmp(decoded.provider.data, provider, sizeof(provider)));
    checkpoint_stream_matches(&frame, encoded.data, encoded.len, 65536u);
    snag_buf_free(&encoded);
    free(large);
}

void
test_store_binary(void)
{
    struct snag_binary_legacy_checkpoint marker = {.start = 0x102u, .end = 0x405u};
    memset(marker.digest, 0xabu, sizeof(marker.digest));
    struct snag_buf marker_bytes = {.max = SNAG_BINARY_LEGACY_CHECKPOINT_SIZE};
    assert(!snag_binary_legacy_checkpoint_encode(&marker_bytes, &marker));
    assert(marker_bytes.len == 48u && marker_bytes.data[0] == 2u &&
        marker_bytes.data[1] == 1u && marker_bytes.data[8] == 5u &&
        marker_bytes.data[9] == 4u && marker_bytes.data[16] == 0xabu);
    struct snag_binary_record marker_record = {.kind = SNAG_BINARY_LEGACY_CHECKPOINT,
        .version = 1u, .flags = SNAG_BINARY_RECORD_OPTIONAL,
        .payload = marker_bytes.data, .size = marker_bytes.len};
    struct snag_binary_legacy_checkpoint decoded_marker;
    assert(!snag_binary_legacy_checkpoint_decode(&marker_record, &decoded_marker));
    assert(!memcmp(&marker, &decoded_marker, sizeof(marker)));
    for (unsigned int fault = 0u; fault < 8u; ++fault) {
        struct snag_binary_record bad = marker_record;
        unsigned char bytes[48];
        memcpy(bytes, marker_bytes.data, sizeof(bytes));
        bad.payload = bytes;
        if (fault == 0u) bad.kind++;
        if (fault == 1u) bad.version++;
        if (fault == 2u) bad.flags = 0u;
        if (fault == 3u) bad.size--;
        if (fault == 4u) memset(bytes, 0, 8u);
        if (fault == 5u) memcpy(bytes + 8u, bytes, 8u);
        if (fault == 6u) bytes[15] = 0x80u;
        if (fault == 7u) bad.payload = NULL;
        decoded_marker = marker;
        assert(snag_binary_legacy_checkpoint_decode(&bad, &decoded_marker) < 0);
        assert(!memcmp(&marker, &decoded_marker, sizeof(marker)));
    }
    assert(snag_binary_legacy_checkpoint_encode(&marker_bytes, &marker) < 0);
    assert(marker_bytes.len == 48u && !memcmp(marker_bytes.data + 16u, marker.digest, 32u));
    marker.start = marker.end;
    assert(snag_binary_legacy_checkpoint_encode(&marker_bytes, &marker) < 0);
    assert(marker_bytes.len == 48u);
    snag_buf_free(&marker_bytes);

    struct snag_binary_identity identity = {.created_ms = UINT64_C(0x0102030405060708)};
    for (size_t i = 0; i < sizeof(identity.id); ++i) identity.id[i] = (unsigned char)i;
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    snag_binary_header_encode(header, &identity);
    /* Independent explicit little-endian fixtures; neither C layout nor an
     * encode/decode round trip supplies their expected bytes. */
    assert_bytes(header, sizeof(header),
        "534e41474a4e4c000000020060000000000000000000000000000000000000000001020304050607"
        "08090a0b0c0d0e0f0807060504030201000000000000000010a796059b17bf9ad3178b99bf4af435"
        "3a1d038ad9749aa99cdab968b500278d");
    struct snag_binary_identity decoded = {0};
    struct snag_binary_anchor anchor = {0};
    for (size_t i = 0; i < sizeof(header); ++i) {
        assert(snag_binary_header_decode(header, i, &decoded, &anchor) == 1);
        assert(!decoded.created_ms && !anchor.next_seq);
    }
    assert(!snag_binary_header_decode(header, sizeof(header), &decoded, &anchor));
    assert(!memcmp(identity.id, decoded.id, sizeof(identity.id)));
    assert(identity.created_ms == decoded.created_ms);
    test_encoded_anchor(&anchor);
    test_backward_special(header, &anchor);
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
    const unsigned int old_minor[] = {0u, 1u, 3u, 65535u};
    for (size_t i = 0u; i < 4u; ++i) {
        unsigned char bad[SNAG_BINARY_HEADER_SIZE];
        memcpy(bad, header, sizeof(bad));
        bad[10] = (unsigned char)old_minor[i];
        bad[11] = (unsigned char)(old_minor[i] >> 8u);
        rehash(bad, sizeof(bad));
        struct snag_binary_anchor kept = anchor;
        assert(snag_binary_header_decode(bad, sizeof(bad), &decoded, &kept) < 0);
        assert_anchor(&kept, &anchor);
    }
    static const unsigned char payload[] = {'a', 'b', 'c', 0, 'z'};
    struct snag_binary_record record = {
        .kind = 7u, .version = 1u, .timestamp_ms = identity.created_ms,
        .payload = payload, .size = sizeof(payload)
    };
    struct snag_buf encoded = {.max = SNAG_BINARY_BATCH_MAX};
    assert(!snag_binary_batch_encode(&encoded, &anchor, &record, 1u, 0u, NULL));
    assert_bytes(encoded.data, encoded.len,
        "534e414742415400e500000000000000010000007000000001000000000000000000000000000000"
        "10a796059b17bf9ad3178b99bf4af4353a1d038ad9749aa99cdab968b500278d0000000000000000"
        "bf7d080b1eb86dcc6506f3970cd9ea1659f3b242bebe555ec99527178ac86c5e2500000000000000"
        "010000000000000008070605040302010700010000000000616263007a534e4147454e4400600000"
        "0000000000e500000000000000010000000000000000000000000000000000000000000000779a3c"
        "acad07d9d88bf0606fefb185d27a0df6754016387dad0e71953699a30e");
    struct snag_binary_batch batch = {0};
    struct snag_binary_anchor next = anchor;
    for (size_t i = 0; i < encoded.len; ++i) {
        assert(snag_binary_batch_decode(encoded.data, i, &anchor, &batch, &next) == 1);
        assert(!batch.size && !memcmp(&next, &anchor, sizeof(next)));
    }
    assert(!snag_binary_batch_decode(encoded.data, encoded.len, &anchor, &batch, &next));
    assert(batch.count == 1u && batch.first_seq == 1u && batch.size == encoded.len);
    assert(next.previous == sizeof(header) && next.end == sizeof(header) + encoded.len + 2u);
    assert(next.next_seq == 2u && !next.turns);
    test_checkpoint_frames(&identity, &next);
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
    assert(!snag_binary_batch_encode(&second, &next, records, 2u, 1u, NULL));
    struct snag_binary_anchor third;
    assert(!snag_binary_batch_decode(second.data, second.len, &next, &batch, &third));
    test_references(&batch);
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
    struct snag_buf wire = {.max = 2u * SNAG_BINARY_WIRE_BATCH_MAX};
    assert(!snag_binary_wire_encode(&wire, encoded.data, encoded.len));
    assert(!snag_binary_wire_encode(&wire, second.data, second.len));
    test_file_reader(header, &wire, &anchor, &next);
    snag_buf_free(&wire);
    snag_buf_free(&joined);
    snag_buf_free(&second);
    /* Invalid input/allocation bounds do not append partially encoded batches. */
    size_t saved = encoded.len;
    struct snag_binary_anchor invalid = next;
    invalid.next_seq = UINT64_MAX;
    assert(snag_binary_batch_encode(&encoded, &invalid, &record, 1u, 1u, NULL) < 0);
    invalid = next;
    invalid.end = INT64_MAX;
    assert(snag_binary_batch_encode(&encoded, &invalid, &record, 1u, 1u, NULL) < 0);
    assert(snag_binary_batch_encode(&encoded, &third, &record, 1u, 0u, NULL) < 0);
    assert(snag_binary_batch_encode(&encoded, &next, NULL, 0u, 1u, NULL) < 0);
    assert(snag_binary_batch_encode(&encoded, &next, &record, UINT32_MAX, 1u, NULL) < 0);
    assert(encoded.len == saved);
    struct snag_buf too_small = {.max = 1u};
    assert(snag_binary_batch_encode(&too_small, &anchor, &record, 1u, 0u, NULL) < 0);
    assert(!too_small.len);
    snag_buf_free(&too_small);
    snag_buf_free(&encoded);
    /* One maximum-size event fits alone; two records must fit the batch target. */
    encoded.max = SNAG_BINARY_BATCH_MAX;
    unsigned char *large = calloc(1u, SNAG_MAX_EVENT_LINE);
    assert(large);
    record.payload = large;
    record.size = SNAG_MAX_EVENT_LINE;
    assert(!snag_binary_batch_encode(&encoded, &anchor, &record, 1u, 0u, NULL));
    assert(encoded.len == SNAG_BINARY_BATCH_MAX);
    assert(!snag_binary_batch_decode(encoded.data, encoded.len, &anchor, &batch, &next));
    snag_buf_free(&encoded);
    encoded.max = SNAG_BINARY_BATCH_MAX;
    records[0] = record;
    records[0].size = SNAG_BINARY_BATCH_TARGET;
    assert(snag_binary_batch_encode(&encoded, &anchor, records, 2u, 0u, NULL) < 0);
    record.size = SNAG_MAX_EVENT_LINE + 1u;
    assert(snag_binary_batch_encode(&encoded, &anchor, &record, 1u, 0u, NULL) < 0);
    assert(!encoded.len);
    free(large);
}
