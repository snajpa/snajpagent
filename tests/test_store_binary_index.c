/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_index.h"
#include "fs.h"
#include "store_binary_event.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void test_store_binary_index(void);

static void
assert_bytes(const unsigned char *bytes, size_t size, const char *hex)
{
    const char *digits = "0123456789abcdef";
    assert(strlen(hex) == size * 2u);
    for (size_t i = 0u; i < size; ++i) {
        assert(hex[i * 2u] == digits[bytes[i] >> 4u]);
        assert(hex[i * 2u + 1u] == digits[bytes[i] & 15u]);
    }
}

static void
test_codecs(void)
{
    struct snag_binary_identity identity = {.created_ms = UINT64_C(0x0807060504030201)};
    for (size_t i = 0u; i < sizeof(identity.id); ++i) {
        identity.id[i] = (unsigned char)(i + 1u);
    }
    unsigned char header[SNAG_BINARY_INDEX_HEADER_SIZE];
    snag_binary_index_header_encode(header, &identity);
    assert_bytes(header, sizeof(header),
        "534e414749445800000001007000000060000000000000000102030405060708090a0b0c0d0e0f10"
        "010203040506070831dd03c3f21f0051ef07b5e6a60169931294fefac3126a175a3639b0d5e068b4"
        "11228e6487ef1fce908d43b2a23f355536d242621f7d18f776fdf23171b83952");
    assert(!snag_binary_index_header_decode(header, sizeof(header), &identity));
    for (size_t i = 0u; i < sizeof(header); ++i) {
        assert(snag_binary_index_header_decode(header, i, &identity) == 1);
        header[i] ^= 1u;
        assert(snag_binary_index_header_decode(header, sizeof(header), &identity) < 0);
        header[i] ^= 1u;
    }
    assert(snag_binary_index_header_decode(header, sizeof(header) + 1u, &identity) < 0);
    struct snag_binary_identity other = identity;
    other.id[0]++;
    assert(snag_binary_index_header_decode(header, sizeof(header), &other) < 0);
    other = identity;
    other.created_ms++;
    assert(snag_binary_index_header_decode(header, sizeof(header), &other) < 0);

    struct snag_binary_index_entry entry = {.sequence = 7u, .batch_offset = 512u,
        .record_offset = 128u, .kind = 128u, .turn = 3u};
    for (size_t i = 0u; i < sizeof(entry.batch_digest); ++i) {
        entry.batch_digest[i] = (unsigned char)i;
    }
    unsigned char bytes[SNAG_BINARY_INDEX_ENTRY_SIZE];
    assert(!snag_binary_index_entry_encode(bytes, &identity, &entry));
    assert_bytes(bytes, sizeof(bytes),
        "07000000000000000002000000000000800000008000000003000000000000000001020304050607"
        "08090a0b0c0d0e0f101112131415161718191a1b1c1d1e1ff6a3a853056b489903f4f8b6e81378c5"
        "dcc777ba3c59be5d9e906f558922f5ef");
    struct snag_binary_index_entry decoded = {0};
    assert(!snag_binary_index_entry_decode(bytes, sizeof(bytes), &identity, 7u, &decoded));
    assert(decoded.sequence == 7u && decoded.batch_offset == 512u &&
        decoded.record_offset == 128u && decoded.kind == 128u && decoded.turn == 3u &&
        !memcmp(decoded.batch_digest, entry.batch_digest, sizeof(entry.batch_digest)));
    struct snag_binary_index_entry saved;
    memcpy(&saved, &decoded, sizeof(saved));
    for (size_t i = 0u; i < sizeof(bytes); ++i) {
        assert(snag_binary_index_entry_decode(bytes, i, &identity, 7u, &decoded) < 0);
        assert(!memcmp(&saved, &decoded, sizeof(saved)));
        bytes[i] ^= 1u;
        assert(snag_binary_index_entry_decode(bytes, sizeof(bytes), &identity, 7u, &decoded) < 0);
        assert(!memcmp(&saved, &decoded, sizeof(saved)));
        bytes[i] ^= 1u;
    }
    assert(snag_binary_index_entry_decode(bytes, sizeof(bytes), &other, 7u, &decoded) < 0);
    assert(snag_binary_index_entry_decode(bytes, sizeof(bytes), &identity, 6u, &decoded) < 0);
    assert(snag_binary_index_entry_decode(bytes, sizeof(bytes) + 1u, &identity, 7u, &decoded) < 0);
    unsigned char encoded[sizeof(bytes)];
    memcpy(encoded, bytes, sizeof(encoded));
    for (unsigned int bad = 0u; bad < 8u; ++bad) {
        struct snag_binary_index_entry changed = entry;
        if (bad == 0u) changed.sequence = 0u;
        if (bad == 1u) changed.sequence = UINT64_MAX;
        if (bad == 2u) changed.turn = changed.sequence;
        if (bad == 3u) changed.kind = 0u;
        if (bad == 4u) changed.batch_offset = SNAG_BINARY_HEADER_SIZE - 1u;
        if (bad == 5u) changed.batch_offset = INT64_MAX;
        if (bad == 6u) changed.record_offset = SNAG_BINARY_BATCH_HEADER_SIZE - 1u;
        if (bad == 7u) changed.record_offset = UINT32_MAX;
        assert(snag_binary_index_entry_encode(bytes, &identity, &changed) < 0);
        assert(!memcmp(bytes, encoded, sizeof(bytes)));
    }
    int64_t offset = -7;
    assert(!snag_binary_index_offset(1u, &offset) && offset == SNAG_BINARY_INDEX_HEADER_SIZE);
    assert(!snag_binary_index_offset(7u, &offset) && offset == 688);
    uint64_t last = ((uint64_t)INT64_MAX - SNAG_BINARY_INDEX_HEADER_SIZE) /
        SNAG_BINARY_INDEX_ENTRY_SIZE;
    assert(!snag_binary_index_offset(last, &offset));
    assert(offset <= INT64_MAX - SNAG_BINARY_INDEX_ENTRY_SIZE);
    int64_t kept = offset;
    assert(snag_binary_index_offset(last + 1u, &offset) < 0 && errno == EOVERFLOW);
    assert(offset == kept);
    assert(snag_binary_index_offset(UINT64_MAX, &offset) < 0 && errno == EOVERFLOW);
    assert(snag_binary_index_offset(0u, &offset) < 0 && errno == EINVAL && offset == kept);
}

static struct snag_binary_text
text(const char *value)
{
    return (struct snag_binary_text){(const unsigned char *)value, strlen(value)};
}

static void
turn_payload(struct snag_buf *out, uint64_t number)
{
    static const unsigned char empty[4];
    struct snag_binary_event event = {.kind = SNAG_BINARY_TURN_STARTED};
    event.data.started.number = number;
    event.data.started.id[0] = (unsigned char)number;
    event.data.started.cwd = text("/w");
    event.data.started.text = text("index fixture");
    event.data.started.instructions = (struct snag_binary_instructions){empty, sizeof(empty)};
    event.data.started.config.selection = (struct snag_binary_selection){
        text("p"), text("m"), text("e")};
    snag_buf_reset(out);
    assert(!snag_binary_event_encode(out, &event));
}

static int
temporary_fd(void)
{
    char *path = snag_path_join(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
        "snag-index-XXXXXX");
    assert(path);
    int fd = mkstemp(path);
    assert(fd >= 0 && !unlink(path));
    free(path);
    return fd;
}

static void
test_batches(void)
{
    struct snag_binary_identity identity = {.created_ms = 42u};
    identity.id[0] = 17u;
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    snag_binary_header_encode(header, &identity);
    struct snag_binary_anchor anchors[3];
    assert(!snag_binary_header_decode(header, sizeof(header), &identity, &anchors[0]));
    struct snag_buf batches[2] = {{.max = SNAG_BINARY_BATCH_MAX}, {.max = SNAG_BINARY_BATCH_MAX}};
    struct snag_buf turns[3] = {{.max = SNAG_MAX_EVENT_LINE},
        {.max = SNAG_MAX_EVENT_LINE}, {.max = SNAG_MAX_EVENT_LINE}};
    for (size_t i = 0u; i < 3u; ++i) turn_payload(&turns[i], i + 1u);
    /* Index framing is separate from core-state replay. Only turn records need
     * typed interpretation here; these optional records stand for other history. */
    struct snag_binary_record records[4] = {
        {.kind = 0x8001u, .version = 1u, .flags = SNAG_BINARY_RECORD_OPTIONAL,
            .payload = header, .size = sizeof(header)},
        {.kind = SNAG_BINARY_TURN_STARTED, .version = 1u,
            .payload = turns[0].data, .size = turns[0].len},
        {.kind = 0x8002u, .version = 1u, .flags = SNAG_BINARY_RECORD_OPTIONAL},
        {.kind = SNAG_BINARY_TURN_STARTED, .version = 1u,
            .payload = turns[1].data, .size = turns[1].len}
    };
    assert(!snag_binary_batch_encode(&batches[0], &anchors[0], records, 4u, 2u));
    struct snag_binary_batch batch;
    assert(!snag_binary_batch_decode(batches[0].data, batches[0].len,
        &anchors[0], &batch, &anchors[1]));
    struct snag_binary_event output = {.kind = SNAG_BINARY_PROCESS_OUTPUT};
    output.data.process_output.turn[0] = 1u; /* An older process in turn epoch2. */
    output.data.process_output.encoding = SNAG_BINARY_BYTES_UTF8;
    output.data.process_output.data = (const unsigned char *)"old process";
    output.data.process_output.size = 11u;
    struct snag_buf captured = {.max = SNAG_MAX_EVENT_LINE};
    assert(!snag_binary_event_encode(&captured, &output));
    records[0] = (struct snag_binary_record){.kind = SNAG_BINARY_PROCESS_OUTPUT,
        .version = 1u, .payload = captured.data, .size = captured.len};
    records[1].payload = turns[2].data;
    records[1].size = turns[2].len;
    assert(!snag_binary_batch_encode(&batches[1], &anchors[1], records, 3u, 3u));
    assert(!snag_binary_batch_decode(batches[1].data, batches[1].len,
        &anchors[1], &batch, &anchors[2]));
    struct snag_buf index = {.max = 8192u};
    unsigned char index_header[SNAG_BINARY_INDEX_HEADER_SIZE];
    snag_binary_index_header_encode(index_header, &identity);
    assert(!snag_buf_append(&index, index_header, sizeof(index_header)));
    for (size_t i = 0u; i < 2u; ++i) {
        size_t length = index.len;
        struct snag_binary_anchor wrong = anchors[i + 1u];
        wrong.digest[0] ^= 1u;
        assert(snag_binary_index_append_batch(&index, &identity, &anchors[i], &wrong,
            batches[i].data, batches[i].len) < 0 && index.len == length);
        assert(snag_binary_index_append_batch(&index, &identity, &anchors[i], &anchors[i + 1u],
            batches[i].data, batches[i].len - 1u) < 0 && index.len == length);
        assert(!snag_binary_index_append_batch(&index, &identity, &anchors[i], &anchors[i + 1u],
            batches[i].data, batches[i].len));
    }
    assert(index.len == SNAG_BINARY_INDEX_HEADER_SIZE + 7u * SNAG_BINARY_INDEX_ENTRY_SIZE);
    /* A checksummed batch with inconsistent turn numbering cannot produce an index. */
    struct snag_buf wrong_turn = {.max = SNAG_MAX_EVENT_LINE};
    turn_payload(&wrong_turn, 9u);
    struct snag_binary_record wrong_records[2] = {
        {.kind = 0x8001u, .version = 1u, .flags = SNAG_BINARY_RECORD_OPTIONAL},
        {.kind = SNAG_BINARY_TURN_STARTED, .version = 1u,
            .payload = wrong_turn.data, .size = wrong_turn.len}
    };
    struct snag_buf wrong_batch = {.max = SNAG_BINARY_BATCH_MAX};
    assert(!snag_binary_batch_encode(&wrong_batch, &anchors[0], wrong_records, 2u, 1u));
    struct snag_binary_anchor wrong_after;
    assert(!snag_binary_batch_decode(wrong_batch.data, wrong_batch.len,
        &anchors[0], &batch, &wrong_after));
    size_t old_length = index.len;
    assert(snag_binary_index_append_batch(&index, &identity, &anchors[0], &wrong_after,
        wrong_batch.data, wrong_batch.len) < 0 && index.len == old_length);
    snag_buf_free(&wrong_batch);
    snag_buf_free(&wrong_turn);

    int fd = temporary_fd();
    assert(!snag_write_full(fd, index.data, index.len));
    assert(snag_seek(fd, 7, SEEK_SET) == 7);
    const uint64_t ordinal[] = {0u, 1u, 1u, 2u, 2u, 3u, 3u};
    struct snag_binary_index_entry entry;
    for (uint64_t sequence = 1u; sequence <= 7u; ++sequence) {
        assert(!snag_binary_index_read_hint(fd, &identity, sequence, &entry));
        assert(entry.sequence == sequence && entry.turn == ordinal[sequence - 1u]);
        size_t which = sequence <= 4u ? 0u : 1u;
        struct snag_binary_record record = {0};
        assert(!snag_binary_index_resolve(&entry, &anchors[which], &anchors[which + 1u],
            batches[which].data, batches[which].len, &record));
        assert(record.kind == entry.kind);
        if (sequence == 5u) {
            struct snag_binary_event event;
            assert(entry.turn == 2u && !snag_binary_event_decode(&record, &event));
            assert(event.kind == SNAG_BINARY_PROCESS_OUTPUT &&
                event.data.process_output.turn[0] == 1u);
        }
        struct snag_binary_record saved;
        memcpy(&saved, &record, sizeof(saved));
        for (unsigned int bad = 0u; bad < 5u; ++bad) {
            struct snag_binary_index_entry changed = entry;
            if (bad == 0u) changed.record_offset++;
            if (bad == 1u) changed.kind++;
            if (bad == 2u) changed.batch_digest[0] ^= 1u;
            if (bad == 3u) changed.batch_offset++;
            if (bad == 4u) changed.turn = changed.turn ? 0u : 1u;
            if (changed.turn < changed.sequence) {
                unsigned char encoded[SNAG_BINARY_INDEX_ENTRY_SIZE];
                assert(!snag_binary_index_entry_encode(encoded, &identity, &changed));
                assert(!snag_binary_index_entry_decode(encoded, sizeof(encoded),
                    &identity, sequence, &changed)); /* A valid checksum is not authority. */
            }
            assert(snag_binary_index_resolve(&changed, &anchors[which], &anchors[which + 1u],
                batches[which].data, batches[which].len, &record) < 0);
            assert(!memcmp(&record, &saved, sizeof(saved)));
        }
    }
    for (uint64_t turn = 1u; turn <= 3u; ++turn) {
        assert(!snag_binary_index_turn_hint(fd, &identity, 7u, turn, &entry));
        assert(entry.turn == turn && entry.sequence == turn * 2u &&
            entry.kind == SNAG_BINARY_TURN_STARTED);
    }
    struct snag_binary_index_entry saved;
    memcpy(&saved, &entry, sizeof(saved));
    assert(snag_binary_index_turn_hint(-1, &identity, 0u, 1u, &entry) < 0);
    assert(!memcmp(&saved, &entry, sizeof(saved)));
    assert(snag_binary_index_turn_hint(fd, &identity, 0u, 1u, &entry) == 1);
    assert(snag_binary_index_turn_hint(fd, &identity, UINT64_MAX, 1u, &entry) < 0);
    assert(snag_binary_index_turn_hint(fd, &identity, 7u, 4u, &entry) == 1);
    assert(!memcmp(&saved, &entry, sizeof(saved)));
    assert(snag_binary_index_read_hint(fd, &identity, 8u, &entry) == 1);
    assert(!memcmp(&saved, &entry, sizeof(saved)));
    assert(snag_seek(fd, 0, SEEK_CUR) == 7);
    struct snag_binary_index_entry wrong_hint;
    assert(!snag_binary_index_read_hint(fd, &identity, 4u, &wrong_hint));
    wrong_hint.kind = 0x8002u;
    unsigned char wrong_entry[SNAG_BINARY_INDEX_ENTRY_SIZE];
    assert(!snag_binary_index_entry_encode(wrong_entry, &identity, &wrong_hint));
    int64_t wrong_offset;
    assert(!snag_binary_index_offset(4u, &wrong_offset));
    assert(snag_seek(fd, wrong_offset, SEEK_SET) == wrong_offset);
    assert(!snag_write_full(fd, wrong_entry, sizeof(wrong_entry)));
    assert(snag_seek(fd, 7, SEEK_SET) == 7);
    assert(snag_binary_index_turn_hint(fd, &identity, 7u, 2u, &entry) < 0 && errno == EINVAL);
    assert(!memcmp(&saved, &entry, sizeof(saved)) && snag_seek(fd, 0, SEEK_CUR) == 7);
    assert(snag_seek(fd, wrong_offset, SEEK_SET) == wrong_offset);
    assert(!snag_write_full(fd, index.data + (size_t)wrong_offset, sizeof(wrong_entry)));
    assert(snag_seek(fd, 7, SEEK_SET) == 7);
    assert(!snag_truncate(fd, (int64_t)index.len - 1));
    assert(snag_binary_index_read_hint(fd, &identity, 7u, &entry) == 1);
    assert(!memcmp(&saved, &entry, sizeof(saved)));
    assert(!snag_binary_index_read_hint(fd, &identity, 6u, &entry));
    assert(!snag_truncate(fd, SNAG_BINARY_INDEX_HEADER_SIZE - 1u));
    assert(snag_binary_index_read_hint(fd, &identity, 1u, &entry) == 1);
    assert(!close(fd));
    snag_buf_free(&index);
    snag_buf_free(&captured);
    for (size_t i = 0u; i < 2u; ++i) snag_buf_free(&batches[i]);
    for (size_t i = 0u; i < 3u; ++i) snag_buf_free(&turns[i]);
}

static void
test_batch_limits(void)
{
    struct snag_binary_identity identity = {.created_ms = 1u};
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    snag_binary_header_encode(header, &identity);
    struct snag_binary_anchor before;
    assert(!snag_binary_header_decode(header, sizeof(header), &identity, &before));
    const uint32_t maximum = (SNAG_BINARY_BATCH_TARGET - SNAG_BINARY_BATCH_HEADER_SIZE -
        SNAG_BINARY_BATCH_FOOTER_SIZE) / SNAG_BINARY_RECORD_HEADER_SIZE;
    for (unsigned int large = 0u; large < 2u; ++large) {
        uint32_t count = large ? 1u : maximum;
        struct snag_binary_record *records = calloc(count, sizeof(*records));
        assert(records);
        unsigned char *payload = large ? calloc(1u, SNAG_MAX_EVENT_LINE) : NULL;
        assert(!large || payload);
        for (uint32_t i = 0u; i < count; ++i) {
            records[i] = (struct snag_binary_record){
                .kind = 0x8001u, .version = 1u, .flags = SNAG_BINARY_RECORD_OPTIONAL,
                .payload = payload, .size = large ? SNAG_MAX_EVENT_LINE : 0u};
        }
        struct snag_buf bytes = {.max = SNAG_BINARY_BATCH_MAX};
        struct snag_buf entries = {.max = SNAG_BINARY_INDEX_BATCH_MAX};
        assert(!snag_binary_batch_encode(&bytes, &before, records, count, 0u));
        assert(bytes.len == (large ? SNAG_BINARY_BATCH_MAX : SNAG_BINARY_BATCH_TARGET));
        struct snag_binary_batch batch;
        struct snag_binary_anchor after;
        assert(!snag_binary_batch_decode(bytes.data, bytes.len, &before, &batch, &after));
        assert(!snag_binary_index_append_batch(&entries, &identity, &before, &after,
            bytes.data, bytes.len));
        assert(entries.len == (size_t)count * SNAG_BINARY_INDEX_ENTRY_SIZE);
        struct snag_binary_index_entry entry;
        assert(!snag_binary_index_entry_decode(
            entries.data + entries.len - SNAG_BINARY_INDEX_ENTRY_SIZE,
            SNAG_BINARY_INDEX_ENTRY_SIZE, &identity, count, &entry));
        struct snag_binary_record resolved;
        assert(!snag_binary_index_resolve(&entry, &before, &after,
            bytes.data, bytes.len, &resolved));
        assert(resolved.kind == 0x8001u && resolved.size == (large ? SNAG_MAX_EVENT_LINE : 0u));
        struct snag_buf bounded = {.max = entries.len};
        assert(!snag_buf_append(&bounded, "!", 1u));
        assert(snag_binary_index_append_batch(&bounded, &identity, &before, &after,
            bytes.data, bytes.len) < 0 && bounded.len == 1u && bounded.data[0] == '!');
        snag_buf_free(&bounded);
        snag_buf_free(&entries);
        snag_buf_free(&bytes);
        free(records);
        free(payload);
    }
}

void
test_store_binary_index(void)
{
    test_codecs();
    test_batches();
    test_batch_limits();
}
