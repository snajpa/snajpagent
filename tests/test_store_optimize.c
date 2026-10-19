/* SPDX-License-Identifier: GPL-2.0-only */
#include "fixture_store_binary.h"
#include "fs.h"
#include "store_binary_wire.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zstd.h>

void test_store_optimize(void);
int fixture_storage_command(const char *operation, const char *path);

/* Expose exact canonical record bytes, including unknown optional metadata;
 * the public semantic history deliberately omits those records. */
int
fixture_storage_command(const char *operation, const char *path)
{
    bool append = !strcmp(operation, "--append-optional");
    int fd = append ? snag_open_history(path) : snag_open_read(path, false);
    assert(fd >= 0);
    struct snag_buf bytes = {.max = SNAG_BINARY_BATCH_MAX};
    if (!strcmp(operation, "--archive-bytes")) {
        assert(!snag_buf_read(&bytes, fd));
        unsigned char *decoded = malloc(SNAG_MAX_EVENT_LINE);
        assert(decoded);
        size_t size = ZSTD_decompress(decoded, SNAG_MAX_EVENT_LINE, bytes.data, bytes.len);
        assert(!ZSTD_isError(size));
        assert(fwrite(decoded, 1u, size, stdout) == size);
        free(decoded);
    } else {
        unsigned char header[SNAG_BINARY_HEADER_SIZE];
        assert(snag_pread(fd, header, sizeof(header), 0) == sizeof(header));
        struct snag_binary_identity identity;
        struct snag_binary_anchor cursor;
        assert(!snag_binary_header_decode(header, sizeof(header), &identity, &cursor));
        snag_file_info info;
        assert(!snag_fstat(fd, &info));
        uint64_t timestamp = 0u;
        while (cursor.end < (uint64_t)info.st_size) {
            struct snag_binary_batch batch;
            struct snag_binary_anchor next;
            assert(!snag_binary_batch_read(fd, info.st_size, &cursor, &bytes, &batch, &next));
            size_t size = batch.size - SNAG_BINARY_BATCH_HEADER_SIZE -
                          SNAG_BINARY_BATCH_FOOTER_SIZE;
            if (!append)
                assert(fwrite(batch.data + SNAG_BINARY_BATCH_HEADER_SIZE, 1u, size, stdout) ==
                       size);
            size_t offset = SNAG_BINARY_BATCH_HEADER_SIZE;
            struct snag_binary_record record;
            uint64_t sequence;
            while (!snag_binary_record_next(&batch, &offset, &record, &sequence))
                timestamp = record.timestamp_ms;
            cursor = next;
        }
        if (append) {
            static const unsigned char payload[] = "unknown optional metadata\0exact bytes";
            struct snag_binary_record record = {.kind = 65534u, .version = 9u,
                .flags = SNAG_BINARY_RECORD_OPTIONAL, .timestamp_ms = timestamp + 1u,
                .payload = payload, .size = sizeof(payload)};
            struct snag_buf encoded = {.max = SNAG_BINARY_BATCH_MAX};
            struct snag_buf wire = {.max = SNAG_BINARY_WIRE_BATCH_MAX};
            assert(!snag_binary_batch_encode(&encoded, &cursor, &record, 1u, cursor.turns, NULL));
            assert(!snag_binary_wire_encode(&wire, encoded.data, encoded.len));
            assert(!snag_write_full(fd, wire.data, wire.len));
            snag_buf_free(&encoded);
            snag_buf_free(&wire);
        }
    }
    snag_buf_free(&bytes);
    assert(!close(fd));
    return 0;
}

void
test_store_optimize(void)
{
    struct snag_binary_identity identity = {.id = {1u}, .created_ms = 123u};
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    snag_binary_optimized_header_encode(header, &identity);
    assert(header[10] == 3u);
    struct snag_binary_anchor initial, after, tail;
    assert(!snag_binary_header_decode(header, sizeof(header), &identity, &initial));
    unsigned char *payload = malloc(2u * 1024u * 1024u);
    assert(payload);
    memset(payload, 'x', 2u * 1024u * 1024u);
    struct snag_binary_record records[16];
    for (size_t i = 0u; i < 16u; ++i) records[i] = (struct snag_binary_record){
        .kind = 7u, .version = 1u, .timestamp_ms = 124u + i,
        .payload = payload, .size = 32768u};
    struct snag_buf decoded = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_buf wire = {.max = SNAG_BINARY_WIRE_BATCH_MAX};
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    assert(!snag_binary_batch_encode(&decoded, &initial, records, 16u, 0u, &after));
    assert(!snag_binary_batch_pack(&wire, &decoded, &initial, &after));
    assert(wire.len < decoded.len / 100u && after.end == sizeof(header) + wire.len);
    unsigned char *original = malloc(decoded.len);
    assert(original);
    size_t original_size = decoded.len;
    memcpy(original, decoded.data, decoded.len);
    char path[] = "/tmp/snag-packed-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0 && !unlink(path));
    assert(!snag_write_full(fd, header, sizeof(header)));
    assert(!snag_write_full(fd, wire.data, wire.len));
    struct snag_binary_batch batch;
    assert(!snag_binary_batch_read(fd, after.end, &initial, &scratch, &batch, &tail));
    assert(scratch.len == original_size && !memcmp(scratch.data, original, original_size));
    assert(!memcmp(&tail, &after, sizeof(tail)));
    struct snag_binary_anchor before;
    assert(!snag_binary_batch_previous(fd, &after, &scratch, &batch, &before));
    assert(!memcmp(&before, &initial, sizeof(before)));
    struct snag_buf access_bytes = {.max = SIZE_MAX};
    struct snag_binary_checkpoint_index access;
    binary_fixture_access(fd, &after, &access_bytes, &access);
    assert(!snag_binary_checkpoint_batch_find(fd, &after, &access, 16u,
        &scratch, &batch, &before));
    struct snag_binary_index_entry location;
    assert(!snag_binary_checkpoint_index_find(&access, 16u, &location));
    assert(location.record_offset > after.end);
    struct snag_binary_index_tree tree = {0};
    struct snag_buf index_bytes = {.max = SNAG_BINARY_INDEX_TREE_BATCH_MAX};
    unsigned char index_header[SNAG_BINARY_INDEX_HEADER_SIZE], root[32];
    snag_binary_index_header_encode(index_header, &identity);
    assert(!snag_buf_append(&index_bytes, index_header, sizeof(index_header)));
    assert(!snag_binary_index_tree_append_batch(&index_bytes, &tree, &identity,
        &initial, &after, decoded.data, decoded.len));
    assert(!snag_binary_index_tree_root(&tree, root));
    char index_path[] = "/tmp/snag-packed-index-XXXXXX";
    int index_fd = mkstemp(index_path);
    assert(index_fd >= 0 && !unlink(index_path));
    assert(!snag_write_full(index_fd, index_bytes.data, index_bytes.len));
    struct snag_binary_index_entry verified;
    assert(!snag_binary_index_read_verified(index_fd, &identity, tree.count, root, 16u,
        &verified));
    assert(verified.record_offset == location.record_offset &&
        verified.batch_offset == location.batch_offset &&
        !memcmp(verified.batch_digest, location.batch_digest, sizeof(verified.batch_digest)));
    assert(!close(index_fd));
    snag_buf_free(&index_bytes);
    assert(!snag_binary_batch_find(fd, &after, 16u, &scratch, &batch, &before));
    assert(batch.first_seq == 1u && batch.count == 16u);
    snag_buf_free(&access_bytes);
    assert(snag_binary_batch_read(fd, after.end - 1u, &initial, &scratch, &batch, &tail) == 1);

    /* A normal raw append can follow a compressed block without rewriting it. */
    snag_buf_reset(&decoded);
    snag_buf_reset(&wire);
    assert(!snag_binary_batch_encode(&decoded, &after, records, 1u, 0u, &tail));
    assert(!snag_binary_wire_encode(&wire, decoded.data, decoded.len));
    assert(!snag_write_full(fd, wire.data, wire.len));
    assert(!snag_binary_batch_read(fd, tail.end, &after, &scratch, &batch, &before));
    assert(before.next_seq == 18u);
    assert(!snag_binary_batch_previous(fd, &tail, &scratch, &batch, &before));
    assert(!memcmp(&before, &after, sizeof(before)));

    /* A single event may exceed the aggregate target; incompressible events
     * retain raw framing. Both paths preserve payload bytes. */
    records[0].size = 2u * 1024u * 1024u;
    snag_buf_reset(&decoded);
    snag_buf_reset(&wire);
    assert(!snag_binary_batch_encode(&decoded, &initial, records, 1u, 0u, &tail));
    assert(!snag_binary_batch_pack(&wire, &decoded, &initial, &tail));
    assert(wire.len < 4096u);
    assert(!snag_truncate(fd, sizeof(header)) && snag_seek(fd, sizeof(header), SEEK_SET) >= 0);
    assert(!snag_write_full(fd, wire.data, wire.len));
    assert(!snag_binary_batch_read(fd, tail.end, &initial, &scratch, &batch, &before));
    assert(batch.size == decoded.len && !memcmp(batch.data, decoded.data, decoded.len));
    uint32_t random = 1u;
    for (size_t i = 0u; i < 32768u; ++i) {
        random ^= random << 13u;
        random ^= random >> 17u;
        random ^= random << 5u;
        payload[i] = (unsigned char)random;
    }
    records[0].size = 32768u;
    snag_buf_reset(&decoded);
    snag_buf_reset(&wire);
    assert(!snag_binary_batch_encode(&decoded, &initial, records, 1u, 0u, &tail));
    assert(!snag_binary_batch_pack(&wire, &decoded, &initial, &tail));
    assert(!memcmp(decoded.data + 72u, "\0\0\0\0\0\0\0\0", 8u));
    assert(!snag_truncate(fd, sizeof(header)) && snag_seek(fd, sizeof(header), SEEK_SET) >= 0);
    assert(!snag_write_full(fd, wire.data, wire.len));
    assert(!snag_binary_batch_read(fd, tail.end, &initial, &scratch, &batch, &before));
    assert(!memcmp(batch.data, decoded.data, decoded.len));
    assert(!close(fd));
    free(original);
    free(payload);
    snag_buf_free(&decoded);
    snag_buf_free(&wire);
    snag_buf_free(&scratch);
}
