/* SPDX-License-Identifier: GPL-2.0-only */
#include "fixture_store_binary.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void test_store_binary_tail(void);

static int
temporary_fd(void)
{
    char *path = snag_path_join(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
        "snag-binary-tail-XXXXXX");
    assert(path);
    int fd = mkstemp(path);
    assert(fd >= 0 && !unlink(path));
    free(path);
    return fd;
}

static void
same_anchor(const struct snag_binary_anchor *left, const struct snag_binary_anchor *right)
{
    assert(left->end == right->end && left->next_seq == right->next_seq &&
        left->turns == right->turns && left->previous == right->previous &&
        !memcmp(left->digest, right->digest, sizeof(left->digest)));
}

static void
write_at(int fd, const void *data, size_t size, uint64_t offset)
{
    assert(snag_seek(fd, (int64_t)offset, SEEK_SET) == (int64_t)offset);
    assert(!snag_write_full(fd, data, size));
    assert(snag_seek(fd, 13, SEEK_SET) == 13);
}

static void
check_tail(int fd, uint64_t size, const struct snag_binary_identity *identity,
    const struct snag_binary_anchor *expected, struct snag_buf *scratch)
{
    struct snag_binary_identity found;
    struct snag_binary_anchor after;
    uint64_t incomplete = UINT64_MAX;
    assert(!snag_binary_journal_tail(fd, size, scratch, &found, &after, &incomplete));
    assert(found.created_ms == identity->created_ms &&
        !memcmp(found.id, identity->id, sizeof(found.id)));
    same_anchor(&after, expected);
    assert(incomplete == size - after.end);
    assert(snag_seek(fd, 0, SEEK_CUR) == 13);
}

static void
reject_tail(int fd, uint64_t size, struct snag_buf *scratch)
{
    struct snag_binary_identity identity = {.created_ms = 7u};
    struct snag_binary_anchor after = {.end = 9u};
    uint64_t incomplete = 11u;
    unsigned char saved_identity[sizeof(identity)];
    unsigned char saved_after[sizeof(after)];
    memcpy(saved_identity, &identity, sizeof(identity));
    memcpy(saved_after, &after, sizeof(after));
    assert(snag_binary_journal_tail(fd, size, scratch, &identity, &after, &incomplete) < 0);
    assert(!memcmp(saved_identity, &identity, sizeof(identity)) &&
        !memcmp(saved_after, &after, sizeof(after)) && incomplete == 11u);
    if (fd >= 0) assert(snag_seek(fd, 0, SEEK_CUR) == 13);
}

static void
maximum_tail(int fd, const unsigned char *header, const struct snag_binary_identity *identity,
    const struct snag_binary_anchor *root)
{
    unsigned char *payload = malloc(SNAG_MAX_EVENT_LINE);
    assert(payload);
    memset(payload, 0xa5, SNAG_MAX_EVENT_LINE);
    struct snag_binary_record record = {.kind = 77u, .version = 1u,
        .payload = payload, .size = SNAG_MAX_EVENT_LINE};
    struct snag_buf wire = {.max = SNAG_BINARY_WIRE_BATCH_MAX};
    struct snag_binary_anchor first;
    assert(!binary_fixture_append(&wire, root, &record, 1u, 0u, &first));
    assert(wire.len == SNAG_BINARY_WIRE_BATCH_MAX);
    assert(!snag_truncate(fd, 0));
    write_at(fd, header, SNAG_BINARY_HEADER_SIZE, 0u);
    write_at(fd, wire.data, wire.len, root->end);
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    check_tail(fd, first.end, identity, &first, &scratch);
    snag_buf_reset(&wire);
    struct snag_binary_anchor second;
    assert(!binary_fixture_append(&wire, &first, &record, 1u, 0u, &second));
    write_at(fd, wire.data, wire.len, first.end);
    check_tail(fd, second.end, identity, &second, &scratch);
    const size_t cuts[] = {1u, 112u, 113u, 254u, 255u, SNAG_BINARY_WIRE_BATCH_MAX - 1u};
    for (size_t i = 0u; i < sizeof(cuts) / sizeof(*cuts); ++i) {
        check_tail(fd, first.end + cuts[i], identity, &first, &scratch);
    }
    unsigned char nonzero = 1u;
    write_at(fd, &nonzero, 1u, second.end - 1u);
    reject_tail(fd, second.end, &scratch);
    /* More than one maximum frame of delimiter-free tail cannot be repairable. */
    memset(wire.data, 1, wire.len);
    write_at(fd, wire.data, wire.len, first.end);
    write_at(fd, &nonzero, 1u, first.end + wire.len);
    reject_tail(fd, first.end + wire.len + 1u, &scratch);
    snag_buf_free(&scratch);
    snag_buf_free(&wire);
    free(payload);
}

void
test_store_binary_tail(void)
{
    struct snag_binary_identity identity = {.created_ms = 42u};
    identity.id[0] = 17u;
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    snag_binary_header_encode(header, &identity);
    struct snag_binary_identity decoded;
    struct snag_binary_anchor anchors[5];
    assert(!snag_binary_header_decode(header, sizeof(header), &decoded, &anchors[0]));
    struct snag_buf file = {.max = 8192u};
    assert(!snag_buf_append(&file, header, sizeof(header)));
    /* Both decoded and wire-encoded self-consistent fake batches are payload.
     * Every torn outer prefix must still resolve to its real earlier boundary. */
    struct snag_buf nested = {.max = 1024u};
    struct snag_buf payload = {.max = 2048u};
    struct snag_binary_record record = {.kind = 77u, .version = 1u,
        .payload = (const unsigned char *)"nested", .size = 6u};
    struct snag_binary_anchor fake = anchors[0];
    fake.end += SNAG_BINARY_BATCH_HEADER_SIZE + SNAG_BINARY_RECORD_HEADER_SIZE;
    fake.previous = anchors[0].end;
    assert(!snag_binary_batch_encode(&nested, &fake, &record, 1u, 0u, NULL));
    assert(!snag_buf_append(&payload, nested.data, nested.len));
    assert(!snag_binary_wire_encode(&payload, nested.data, nested.len));
    record.payload = payload.data;
    record.size = payload.len;
    for (size_t i = 0u; i < 4u; ++i) {
        assert(!binary_fixture_append(&file, &anchors[i], &record, 1u, i, &anchors[i + 1u]));
    }
    int fd = temporary_fd();
    write_at(fd, file.data, file.len, 0u);
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    size_t current = 0u;
    for (uint64_t cut = sizeof(header); cut <= file.len; ++cut) {
        if (current < 4u && cut == anchors[current + 1u].end) ++current;
        check_tail(fd, cut, &identity, &anchors[current], &scratch);
    }
    /* Every byte in the last two batches is checked, not just footer digests.
     * Damaging a delimiter must fail rather than skipping the closed frame. */
    for (uint64_t position = anchors[2].end; position < file.len; ++position) {
        unsigned char changed = file.data[position] ^ 1u;
        write_at(fd, &changed, 1u, position);
        reject_tail(fd, file.len, &scratch);
        write_at(fd, file.data + position, 1u, position);
    }
    /* The bounded tail query leaves unrelated history verification to its reader. */
    unsigned char changed = file.data[anchors[0].end + 20u] ^ 1u;
    write_at(fd, &changed, 1u, anchors[0].end + 20u);
    check_tail(fd, file.len, &identity, &anchors[4], &scratch);
    struct snag_binary_batch batch;
    struct snag_binary_anchor after;
    assert(snag_binary_batch_read(fd, file.len, &anchors[0], &scratch, &batch, &after) < 0);
    write_at(fd, file.data + anchors[0].end + 20u, 1u, anchors[0].end + 20u);
    for (size_t i = 0u; i < sizeof(header); ++i) {
        changed = header[i] ^ 1u;
        write_at(fd, &changed, 1u, i);
        reject_tail(fd, file.len, &scratch);
        write_at(fd, header + i, 1u, i);
    }
    struct snag_binary_identity foreign = identity;
    foreign.id[0]++;
    unsigned char foreign_header[SNAG_BINARY_HEADER_SIZE];
    snag_binary_header_encode(foreign_header, &foreign);
    write_at(fd, foreign_header, sizeof(foreign_header), 0u);
    reject_tail(fd, anchors[2].end, &scratch);
    write_at(fd, header, sizeof(header), 0u);
    const unsigned char malformed[][4] = {{0u}, {1u, 0u}, {2u, 1u, 0u}, {3u, 1u, 1u, 0u}};
    for (size_t i = 0u; i < 4u; ++i) {
        write_at(fd, malformed[i], i + 1u, file.len);
        reject_tail(fd, file.len + i + 1u, &scratch);
    }
    assert(!snag_truncate(fd, (int64_t)file.len - 1));
    reject_tail(fd, file.len, &scratch);
    assert(errno == EIO);
    assert(!snag_truncate(fd, SNAG_BINARY_HEADER_SIZE - 1u));
    reject_tail(fd, SNAG_BINARY_HEADER_SIZE, &scratch);
    assert(errno == EIO);
    write_at(fd, file.data, file.len, 0u);
    reject_tail(fd, SNAG_BINARY_HEADER_SIZE - 1u, &scratch);
    reject_tail(fd, UINT64_MAX, &scratch);
    reject_tail(-1, file.len, &scratch);
    struct snag_buf small = {.max = 1u};
    reject_tail(fd, file.len, &small);
    assert(errno == EOVERFLOW && !small.len);
    uint64_t incomplete = 7u;
    after = anchors[0];
    assert(snag_binary_journal_tail(fd, file.len, NULL, &decoded, &after, &incomplete) < 0);
    assert(snag_binary_journal_tail(fd, file.len, &scratch, NULL, &after, &incomplete) < 0);
    assert(snag_binary_journal_tail(fd, file.len, &scratch, &decoded, NULL, &incomplete) < 0);
    assert(snag_binary_journal_tail(fd, file.len, &scratch, &decoded, &after, NULL) < 0);
    same_anchor(&after, &anchors[0]);
    assert(incomplete == 7u);
    maximum_tail(fd, header, &identity, &anchors[0]);
    assert(!close(fd));
    snag_buf_free(&small);
    snag_buf_free(&scratch);
    snag_buf_free(&payload);
    snag_buf_free(&nested);
    snag_buf_free(&file);
}
