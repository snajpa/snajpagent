/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_index.h"
#include "fs.h"
#include "store_binary_event.h"

#include <errno.h>
#include <string.h>

static const unsigned char index_magic[8] = "SNAGIDX";

static void
put_le(unsigned char *out, uint64_t value, size_t width)
{
    for (size_t i = 0u; i < width; ++i) {
        out[i] = (unsigned char)value;
        value >>= 8u;
    }
}

static uint64_t
get_le(const unsigned char *data, size_t width)
{
    uint64_t value = 0u;
    for (size_t i = 0u; i < width; ++i) value |= (uint64_t)data[i] << (8u * i);
    return value;
}

static void
digest(const void *data, size_t size, unsigned char out[32])
{
    struct snag_sha256 hash;
    snag_sha256_init(&hash);
    snag_sha256_update(&hash, data, size);
    snag_sha256_final(&hash, out);
}

void
snag_binary_index_header_encode(unsigned char out[SNAG_BINARY_INDEX_HEADER_SIZE],
    const struct snag_binary_identity *identity)
{
    unsigned char header[SNAG_BINARY_INDEX_HEADER_SIZE] = {0};
    unsigned char journal[SNAG_BINARY_HEADER_SIZE];
    snag_binary_header_encode(journal, identity);
    memcpy(header, index_magic, sizeof(index_magic));
    put_le(header + 10u, 1u, 2u);
    put_le(header + 12u, sizeof(header), 4u);
    put_le(header + 16u, SNAG_BINARY_INDEX_ENTRY_SIZE, 4u);
    memcpy(header + 24u, identity->id, sizeof(identity->id));
    put_le(header + 40u, identity->created_ms, 8u);
    memcpy(header + 48u, journal + SNAG_BINARY_HEADER_SIZE - 32u, 32u);
    digest(header, sizeof(header) - 32u, header + sizeof(header) - 32u);
    memcpy(out, header, sizeof(header));
}

int
snag_binary_index_header_decode(const void *data, size_t size,
    const struct snag_binary_identity *identity)
{
    if (!identity || (!data && size)) return snag_errno(EINVAL);
    if (size < SNAG_BINARY_INDEX_HEADER_SIZE) return 1;
    if (size != SNAG_BINARY_INDEX_HEADER_SIZE) return snag_errno(EINVAL);
    unsigned char expected[SNAG_BINARY_INDEX_HEADER_SIZE];
    snag_binary_index_header_encode(expected, identity);
    return memcmp(expected, data, sizeof(expected)) ? snag_errno(EINVAL) : 0;
}

static bool
entry_valid(const struct snag_binary_index_entry *entry)
{
    return entry->sequence && entry->sequence <= INT64_MAX && entry->turn < entry->sequence &&
        entry->kind && entry->batch_offset >= SNAG_BINARY_HEADER_SIZE &&
        entry->record_offset >= SNAG_BINARY_BATCH_HEADER_SIZE &&
        entry->record_offset <= SNAG_BINARY_BATCH_MAX - SNAG_BINARY_BATCH_FOOTER_SIZE -
            SNAG_BINARY_RECORD_HEADER_SIZE &&
        entry->batch_offset <= (uint64_t)INT64_MAX - entry->record_offset -
            SNAG_BINARY_RECORD_HEADER_SIZE - SNAG_BINARY_BATCH_FOOTER_SIZE;
}

static void
entry_digest(const struct snag_binary_identity *identity, const unsigned char *data,
    unsigned char out[32])
{
    unsigned char journal[SNAG_BINARY_HEADER_SIZE];
    snag_binary_header_encode(journal, identity);
    struct snag_sha256 hash;
    snag_sha256_init(&hash);
    snag_sha256_update(&hash, journal + SNAG_BINARY_HEADER_SIZE - 32u, 32u);
    snag_sha256_update(&hash, data, SNAG_BINARY_INDEX_ENTRY_SIZE - 32u);
    snag_sha256_final(&hash, out);
}

int
snag_binary_index_entry_encode(unsigned char out[SNAG_BINARY_INDEX_ENTRY_SIZE],
    const struct snag_binary_identity *identity, const struct snag_binary_index_entry *entry)
{
    if (!out || !identity || !entry || !entry_valid(entry)) {
        return snag_errno(EINVAL);
    }
    unsigned char bytes[SNAG_BINARY_INDEX_ENTRY_SIZE] = {0};
    put_le(bytes, entry->sequence, 8u);
    put_le(bytes + 8u, entry->batch_offset, 8u);
    put_le(bytes + 16u, entry->record_offset, 4u);
    put_le(bytes + 20u, entry->kind, 2u);
    put_le(bytes + 24u, entry->turn, 8u);
    memcpy(bytes + 32u, entry->batch_digest, sizeof(entry->batch_digest));
    entry_digest(identity, bytes, bytes + sizeof(bytes) - 32u);
    memcpy(out, bytes, sizeof(bytes));
    return 0;
}

int
snag_binary_index_entry_decode(const void *data, size_t size,
    const struct snag_binary_identity *identity, uint64_t sequence,
    struct snag_binary_index_entry *out)
{
    if (!data || !identity || !out || size != SNAG_BINARY_INDEX_ENTRY_SIZE) {
        return snag_errno(EINVAL);
    }
    const unsigned char *bytes = data;
    unsigned char hash[32];
    entry_digest(identity, bytes, hash);
    if (memcmp(hash, bytes + size - sizeof(hash), sizeof(hash)) || get_le(bytes + 22u, 2u)) {
        return snag_errno(EINVAL);
    }
    struct snag_binary_index_entry entry = {
        .sequence = get_le(bytes, 8u), .batch_offset = get_le(bytes + 8u, 8u),
        .record_offset = (uint32_t)get_le(bytes + 16u, 4u),
        .kind = (uint16_t)get_le(bytes + 20u, 2u), .turn = get_le(bytes + 24u, 8u)
    };
    memcpy(entry.batch_digest, bytes + 32u, sizeof(entry.batch_digest));
    if (!entry_valid(&entry) || entry.sequence != sequence) {
        return snag_errno(EINVAL);
    }
    *out = entry;
    return 0;
}

int
snag_binary_index_offset(uint64_t sequence, int64_t *out)
{
    if (!sequence || !out) return snag_errno(EINVAL);
    /* Reserve the complete entry, not just its starting file offset. */
    if (sequence > ((uint64_t)INT64_MAX - SNAG_BINARY_INDEX_HEADER_SIZE) /
        SNAG_BINARY_INDEX_ENTRY_SIZE) {
        return snag_errno(EOVERFLOW);
    }
    *out = (int64_t)(SNAG_BINARY_INDEX_HEADER_SIZE +
        (sequence - 1u) * SNAG_BINARY_INDEX_ENTRY_SIZE);
    return 0;
}

static int
checked_batch(const struct snag_binary_anchor *before, const struct snag_binary_anchor *after,
    const void *data, size_t size, struct snag_binary_batch *batch)
{
    if (!before || !after || !data) return snag_errno(EINVAL);
    struct snag_binary_anchor found;
    int rc = snag_binary_batch_decode(data, size, before, batch, &found);
    if (rc != 0) return rc < 0 ? rc : snag_errno(EINVAL);
    if (batch->size != size || found.end != after->end || found.next_seq != after->next_seq ||
        found.turns != after->turns || found.previous != after->previous ||
        memcmp(found.digest, after->digest, sizeof(found.digest))) {
        return snag_errno(EINVAL);
    }
    return 0;
}

static int
advance_turn(const struct snag_binary_record *record, uint64_t *turn)
{
    if (record->kind != SNAG_BINARY_TURN_STARTED) return 0;
    struct snag_binary_event event;
    if (*turn == UINT64_MAX || snag_binary_event_decode(record, &event) != 0 ||
        event.data.started.number != *turn + 1u) {
        return snag_errno(EINVAL);
    }
    ++*turn;
    return 0;
}

int
snag_binary_index_append_batch(struct snag_buf *out,
    const struct snag_binary_identity *identity, const struct snag_binary_anchor *before,
    const struct snag_binary_anchor *after, const void *data, size_t size)
{
    if (!out || !identity) return snag_errno(EINVAL);
    struct snag_binary_batch batch;
    if (checked_batch(before, after, data, size, &batch) < 0) return -1;
    struct snag_buf encoded = {.max = SNAG_BINARY_INDEX_BATCH_MAX};
    uint64_t turn = before->turns;
    size_t offset = SNAG_BINARY_BATCH_HEADER_SIZE;
    int rc = -1;
    for (uint32_t i = 0u; i < batch.count; ++i) {
        struct snag_binary_record record;
        struct snag_binary_index_entry entry = {
            .batch_offset = before->end, .record_offset = (uint32_t)offset
        };
        if (snag_binary_record_next(&batch, &offset, &record, &entry.sequence) != 0 ||
            advance_turn(&record, &turn) < 0) {
            goto done;
        }
        entry.kind = record.kind;
        entry.turn = turn;
        memcpy(entry.batch_digest, after->digest, sizeof(entry.batch_digest));
        unsigned char bytes[SNAG_BINARY_INDEX_ENTRY_SIZE];
        if (snag_binary_index_entry_encode(bytes, identity, &entry) < 0 ||
            snag_buf_append(&encoded, bytes, sizeof(bytes)) < 0) {
            goto done;
        }
    }
    if (turn != after->turns) {
        errno = EINVAL;
        goto done;
    }
    rc = snag_buf_append(out, encoded.data, encoded.len);
done:
    snag_buf_free(&encoded);
    return rc;
}

int
snag_binary_index_resolve(const struct snag_binary_index_entry *entry,
    const struct snag_binary_anchor *before, const struct snag_binary_anchor *after,
    const void *data, size_t size, struct snag_binary_record *out)
{
    if (!entry || !before || !after || !out || !entry_valid(entry) ||
        entry->batch_offset != before->end ||
        memcmp(entry->batch_digest, after->digest, sizeof(entry->batch_digest))) {
        return snag_errno(EINVAL);
    }
    struct snag_binary_batch batch;
    if (checked_batch(before, after, data, size, &batch) < 0) return -1;
    uint64_t turn = before->turns;
    size_t offset = SNAG_BINARY_BATCH_HEADER_SIZE;
    struct snag_binary_record selected = {0};
    bool found = false;
    for (uint32_t i = 0u; i < batch.count; ++i) {
        size_t start = offset;
        struct snag_binary_record record;
        uint64_t sequence;
        if (snag_binary_record_next(&batch, &offset, &record, &sequence) != 0 ||
            advance_turn(&record, &turn) < 0) {
            return -1;
        }
        if (sequence != entry->sequence) continue;
        if (entry->record_offset != start || entry->kind != record.kind || entry->turn != turn) {
            return snag_errno(EINVAL);
        }
        selected = record;
        found = true;
    }
    if (!found || turn != after->turns) return snag_errno(EINVAL);
    *out = selected;
    return 0;
}

static int
read_exact(int fd, unsigned char *bytes, size_t size, int64_t offset)
{
    if (fd < 0 || offset < 0 || size > (uint64_t)INT64_MAX - (uint64_t)offset) {
        return snag_errno(EINVAL);
    }
    while (size) {
        ssize_t got = snag_pread(fd, bytes, size, offset);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) return -1;
        if (!got) return 1;
        size -= (size_t)got;
        bytes += got;
        offset += got;
    }
    return 0;
}

int
snag_binary_index_read_hint(int fd, const struct snag_binary_identity *identity,
    uint64_t sequence, struct snag_binary_index_entry *out)
{
    if (!identity || !out) return snag_errno(EINVAL);
    int64_t offset;
    if (snag_binary_index_offset(sequence, &offset) < 0) return -1;
    unsigned char header[SNAG_BINARY_INDEX_HEADER_SIZE];
    int rc = read_exact(fd, header, sizeof(header), 0);
    if (rc) return rc;
    if (snag_binary_index_header_decode(header, sizeof(header), identity) < 0) {
        return -1;
    }
    unsigned char bytes[SNAG_BINARY_INDEX_ENTRY_SIZE];
    rc = read_exact(fd, bytes, sizeof(bytes), offset);
    return rc ? rc : snag_binary_index_entry_decode(bytes, sizeof(bytes), identity, sequence, out);
}

int
snag_binary_index_turn_hint(int fd, const struct snag_binary_identity *identity,
    uint64_t indexed_count, uint64_t turn, struct snag_binary_index_entry *out)
{
    if (fd < 0 || !identity || !out || !turn || indexed_count >= INT64_MAX) {
        return snag_errno(EINVAL);
    }
    uint64_t low = 1u;
    uint64_t high = indexed_count + 1u;
    while (low < high) {
        uint64_t middle = low + (high - low) / 2u;
        struct snag_binary_index_entry entry;
        int rc = snag_binary_index_read_hint(fd, identity, middle, &entry);
        if (rc) return rc;
        if (entry.turn < turn) low = middle + 1u;
        else high = middle;
    }
    if (low > indexed_count) return 1;
    struct snag_binary_index_entry entry;
    int rc = snag_binary_index_read_hint(fd, identity, low, &entry);
    if (rc) return rc;
    if (entry.turn != turn) return 1;
    if (entry.kind != SNAG_BINARY_TURN_STARTED) return snag_errno(EINVAL);
    *out = entry;
    return 0;
}
