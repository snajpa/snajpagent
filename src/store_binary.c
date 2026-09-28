/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary.h"
#include "fs.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

static const unsigned char journal_magic[8] = "SNAGJNL";
static const unsigned char batch_magic[8] = "SNAGBAT";
static const unsigned char footer_magic[8] = "SNAGEND";

static void
put_le(unsigned char *out, uint64_t value, size_t width)
{
    for (size_t i = 0; i < width; ++i) {
        out[i] = (unsigned char)value;
        value >>= 8u;
    }
}

static uint64_t
get_le(const unsigned char *data, size_t width)
{
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i) value |= (uint64_t)data[i] << (8u * i);
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

static int
invalid(void)
{
    errno = EINVAL;
    return -1;
}

static bool
reference_valid(const struct snag_binary_ref *reference)
{
    return reference->sequence && reference->sequence < UINT64_MAX &&
        reference->offset <= SNAG_MAX_EVENT_LINE &&
        reference->size <= SNAG_MAX_EVENT_LINE - reference->offset;
}

int
snag_binary_ref_encode(unsigned char out[SNAG_BINARY_REF_SIZE],
    const struct snag_binary_ref *reference)
{
    if (!reference_valid(reference)) return invalid();
    put_le(out, reference->sequence, 8u);
    put_le(out + 8, reference->offset, 4u);
    put_le(out + 12, reference->size, 4u);
    return 0;
}

int
snag_binary_ref_decode(const void *data, size_t size, struct snag_binary_ref *reference)
{
    const unsigned char *bytes = data;
    if (size != SNAG_BINARY_REF_SIZE) return invalid();
    struct snag_binary_ref decoded = {
        .sequence = get_le(bytes, 8u), .offset = (uint32_t)get_le(bytes + 8, 4u),
        .size = (uint32_t)get_le(bytes + 12, 4u)
    };
    if (!reference_valid(&decoded)) return invalid();
    *reference = decoded;
    return 0;
}

int
snag_binary_ref_resolve(const struct snag_binary_ref *reference,
    const struct snag_binary_batch *batch, uint16_t kind, uint16_t version,
    const unsigned char **view)
{
    if (!reference_valid(reference) || !kind || !version ||
        reference->sequence < batch->first_seq ||
        reference->sequence - batch->first_seq >= batch->count) return invalid();
    size_t cursor = SNAG_BINARY_BATCH_HEADER_SIZE;
    struct snag_binary_record record;
    uint64_t sequence;
    int rc;
    while ((rc = snag_binary_record_next(batch, &cursor, &record, &sequence)) == 0) {
        if (sequence != reference->sequence) continue;
        if (record.kind != kind || record.version != version || record.flags ||
            reference->offset > record.size ||
            reference->size > record.size - reference->offset) return invalid();
        *view = record.payload + reference->offset;
        return 0;
    }
    return rc < 0 ? rc : invalid();
}

void
snag_binary_header_encode(unsigned char out[SNAG_BINARY_HEADER_SIZE],
    const struct snag_binary_identity *identity)
{
    memset(out, 0, SNAG_BINARY_HEADER_SIZE);
    memcpy(out, journal_magic, sizeof(journal_magic));
    put_le(out + 10, 1u, 2u); /* Draft major 0, minor 1. */
    put_le(out + 12, SNAG_BINARY_HEADER_SIZE, 4u);
    /* Required reader/writer feature bits at 16/24 and reserved bytes at 56
     * are zero in this draft. */
    memcpy(out + 32, identity->id, sizeof(identity->id));
    put_le(out + 48, identity->created_ms, 8u);
    digest(out, 64u, out + 64);
}

int
snag_binary_header_decode(const void *data, size_t size,
    struct snag_binary_identity *identity, struct snag_binary_anchor *anchor)
{
    const unsigned char *bytes = data;
    unsigned char hash[32];
    if (size < SNAG_BINARY_HEADER_SIZE) return 1;
    if (memcmp(bytes, journal_magic, sizeof(journal_magic)) ||
        get_le(bytes + 8, 2u) != 0u || get_le(bytes + 10, 2u) != 1u ||
        get_le(bytes + 12, 4u) != SNAG_BINARY_HEADER_SIZE ||
        get_le(bytes + 16, 8u) || get_le(bytes + 24, 8u) ||
        get_le(bytes + 56, 8u)) return invalid();
    digest(bytes, 64u, hash);
    if (memcmp(hash, bytes + 64, sizeof(hash))) return invalid();
    struct snag_binary_identity decoded = {.created_ms = get_le(bytes + 48, 8u)};
    memcpy(decoded.id, bytes + 32, sizeof(decoded.id));
    struct snag_binary_anchor start = {.end = SNAG_BINARY_HEADER_SIZE, .next_seq = 1u};
    memcpy(start.digest, hash, sizeof(hash));
    *identity = decoded;
    *anchor = start;
    return 0;
}

static bool
anchor_valid(const struct snag_binary_anchor *anchor)
{
    return anchor->end >= SNAG_BINARY_HEADER_SIZE && anchor->end <= INT64_MAX &&
        anchor->next_seq && (anchor->end == SNAG_BINARY_HEADER_SIZE ?
            anchor->previous == 0u && anchor->next_seq == 1u && anchor->turns == 0u :
            anchor->previous >= SNAG_BINARY_HEADER_SIZE && anchor->previous < anchor->end);
}

static bool
record_valid(const struct snag_binary_record *record)
{
    return record->kind && record->version &&
        !(record->flags & ~SNAG_BINARY_RECORD_OPTIONAL) &&
        record->size <= SNAG_MAX_EVENT_LINE && (!record->size || record->payload);
}

int
snag_binary_batch_encode(struct snag_buf *out, const struct snag_binary_anchor *anchor,
    const struct snag_binary_record *records, uint32_t count, uint64_t turns)
{
    size_t size = SNAG_BINARY_BATCH_HEADER_SIZE + SNAG_BINARY_BATCH_FOOTER_SIZE;
    size_t maximum = count == 1u ? SNAG_BINARY_BATCH_MAX : SNAG_BINARY_BATCH_TARGET;
    if (!anchor_valid(anchor) || !records || !count ||
        count > (SNAG_BINARY_BATCH_TARGET - size) / SNAG_BINARY_RECORD_HEADER_SIZE ||
        turns < anchor->turns || count > UINT64_MAX - anchor->next_seq) return invalid();
    for (uint32_t i = 0; i < count; ++i) {
        if (!record_valid(&records[i]) ||
            SNAG_BINARY_RECORD_HEADER_SIZE + records[i].size > maximum - size)
            return invalid();
        size += SNAG_BINARY_RECORD_HEADER_SIZE + records[i].size;
    }
    if (size > (uint64_t)INT64_MAX - anchor->end) return invalid();
    /* Build separately: a failed allocation or invalid record cannot leave a
     * partial batch in the caller's pending transaction. */
    struct snag_buf encoded = {.max = SNAG_BINARY_BATCH_MAX};
    unsigned char header[SNAG_BINARY_BATCH_HEADER_SIZE] = {0};
    memcpy(header, batch_magic, sizeof(batch_magic));
    put_le(header + 8, size, 8u);
    put_le(header + 16, count, 4u);
    put_le(header + 20, sizeof(header), 4u);
    put_le(header + 24, anchor->next_seq, 8u);
    put_le(header + 32, anchor->previous, 8u);
    memcpy(header + 40, anchor->digest, sizeof(anchor->digest));
    digest(header, sizeof(header) - 32u, header + sizeof(header) - 32u);
    if (snag_buf_append(&encoded, header, sizeof(header)) < 0) goto fail;
    for (uint32_t i = 0; i < count; ++i) {
        unsigned char record[SNAG_BINARY_RECORD_HEADER_SIZE] = {0};
        put_le(record, SNAG_BINARY_RECORD_HEADER_SIZE + records[i].size, 8u);
        put_le(record + 8, anchor->next_seq + i, 8u);
        put_le(record + 16, records[i].timestamp_ms, 8u);
        put_le(record + 24, records[i].kind, 2u);
        put_le(record + 26, records[i].version, 2u);
        put_le(record + 28, records[i].flags, 4u);
        if (snag_buf_append(&encoded, record, sizeof(record)) < 0 ||
            (records[i].size && snag_buf_append(&encoded, records[i].payload,
                records[i].size) < 0)) goto fail;
    }
    unsigned char footer[SNAG_BINARY_BATCH_FOOTER_SIZE] = {0};
    memcpy(footer, footer_magic, sizeof(footer_magic));
    put_le(footer + 8, anchor->end, 8u);
    put_le(footer + 16, size, 8u);
    put_le(footer + 24, anchor->next_seq + count - 1u, 8u);
    put_le(footer + 32, turns, 8u);
    if (snag_buf_append(&encoded, footer, sizeof(footer) - 32u) < 0) goto fail;
    digest(encoded.data, encoded.len, footer + sizeof(footer) - 32u);
    if (snag_buf_append(&encoded, footer + sizeof(footer) - 32u, 32u) < 0 ||
        snag_buf_append(out, encoded.data, encoded.len) < 0) goto fail;
    snag_buf_free(&encoded);
    return 0;
fail:
    snag_buf_free(&encoded);
    return -1;
}

static int
record_read(const unsigned char *data, size_t remaining,
    struct snag_binary_record *record, uint64_t *sequence, size_t *length)
{
    if (remaining < SNAG_BINARY_RECORD_HEADER_SIZE) return invalid();
    uint64_t size = get_le(data, 8u);
    if (size < SNAG_BINARY_RECORD_HEADER_SIZE || size > remaining ||
        size - SNAG_BINARY_RECORD_HEADER_SIZE > SNAG_MAX_EVENT_LINE) return invalid();
    struct snag_binary_record decoded = {
        .kind = (uint16_t)get_le(data + 24, 2u),
        .version = (uint16_t)get_le(data + 26, 2u),
        .flags = (uint32_t)get_le(data + 28, 4u),
        .timestamp_ms = get_le(data + 16, 8u),
        .payload = data + SNAG_BINARY_RECORD_HEADER_SIZE,
        .size = (size_t)size - SNAG_BINARY_RECORD_HEADER_SIZE
    };
    if (!record_valid(&decoded)) return invalid();
    *record = decoded;
    *sequence = get_le(data + 8, 8u);
    *length = (size_t)size;
    return 0;
}

int
snag_binary_batch_decode(const void *data, size_t size,
    const struct snag_binary_anchor *anchor, struct snag_binary_batch *batch,
    struct snag_binary_anchor *next)
{
    const unsigned char *bytes = data;
    if (!anchor_valid(anchor)) return invalid();
    if (size < SNAG_BINARY_BATCH_HEADER_SIZE) return 1;
    unsigned char header_hash[32];
    digest(bytes, SNAG_BINARY_BATCH_HEADER_SIZE - sizeof(header_hash), header_hash);
    if (memcmp(header_hash, bytes + SNAG_BINARY_BATCH_HEADER_SIZE - sizeof(header_hash),
            sizeof(header_hash))) return invalid();
    uint64_t length = get_le(bytes + 8, 8u);
    uint64_t count = get_le(bytes + 16, 4u);
    if (memcmp(bytes, batch_magic, sizeof(batch_magic)) ||
        get_le(bytes + 20, 4u) != SNAG_BINARY_BATCH_HEADER_SIZE ||
        !count || count > UINT64_MAX - anchor->next_seq ||
        count > (SNAG_BINARY_BATCH_TARGET - SNAG_BINARY_BATCH_HEADER_SIZE -
            SNAG_BINARY_BATCH_FOOTER_SIZE) / SNAG_BINARY_RECORD_HEADER_SIZE ||
        length > (count == 1u ? SNAG_BINARY_BATCH_MAX : SNAG_BINARY_BATCH_TARGET) ||
        length < SNAG_BINARY_BATCH_HEADER_SIZE + SNAG_BINARY_BATCH_FOOTER_SIZE +
            count * SNAG_BINARY_RECORD_HEADER_SIZE ||
        length > (uint64_t)INT64_MAX - anchor->end ||
        get_le(bytes + 24, 8u) != anchor->next_seq ||
        get_le(bytes + 32, 8u) != anchor->previous ||
        memcmp(bytes + 40, anchor->digest, sizeof(anchor->digest)) ||
        get_le(bytes + 72, 8u)) return invalid();
    if (size < length) return 1;
    size_t footer_offset = (size_t)length - SNAG_BINARY_BATCH_FOOTER_SIZE;
    const unsigned char *footer = bytes + footer_offset;
    if (memcmp(footer, footer_magic, sizeof(footer_magic)) ||
        get_le(footer + 8, 8u) != anchor->end || get_le(footer + 16, 8u) != length ||
        get_le(footer + 24, 8u) != anchor->next_seq + count - 1u ||
        get_le(footer + 32, 8u) < anchor->turns || get_le(footer + 40, 8u)) return invalid();
    unsigned char hash[32];
    digest(bytes, (size_t)length - sizeof(hash), hash);
    if (memcmp(hash, bytes + length - sizeof(hash), sizeof(hash))) return invalid();
    size_t offset = SNAG_BINARY_BATCH_HEADER_SIZE;
    for (uint32_t i = 0; i < count; ++i) {
        struct snag_binary_record record;
        uint64_t sequence;
        size_t record_size;
        if (record_read(bytes + offset, footer_offset - offset,
                &record, &sequence, &record_size) < 0 ||
            sequence != anchor->next_seq + i) return invalid();
        offset += record_size;
    }
    if (offset != footer_offset) return invalid();
    struct snag_binary_anchor committed = {
        .end = anchor->end + length, .next_seq = anchor->next_seq + count,
        .turns = get_le(footer + 32, 8u), .previous = anchor->end
    };
    memcpy(committed.digest, hash, sizeof(hash));
    *batch = (struct snag_binary_batch){
        .data = bytes, .size = (size_t)length, .count = (uint32_t)count,
        .first_seq = anchor->next_seq
    };
    *next = committed;
    return 0;
}

int
snag_binary_batch_read(int fd, uint64_t boundary,
    const struct snag_binary_anchor *anchor, struct snag_buf *scratch,
    struct snag_binary_batch *batch, struct snag_binary_anchor *next)
{
    snag_buf_reset(scratch);
    if (!anchor_valid(anchor) || boundary > INT64_MAX || boundary < anchor->end)
        return invalid();
    if (boundary - anchor->end < SNAG_BINARY_BATCH_HEADER_SIZE) return 1;
    size_t needed = SNAG_BINARY_BATCH_HEADER_SIZE;
    for (;;) {
        if (snag_buf_reserve(scratch, needed - scratch->len) < 0) return -1;
        while (scratch->len < needed) {
            size_t chunk = needed - scratch->len;
            if (chunk > 65536u) chunk = 65536u;
            ssize_t got = snag_pread(fd, scratch->data + scratch->len, chunk,
                (int64_t)(anchor->end + scratch->len));
            if (got < 0 && errno == EINTR) continue;
            if (got < 0) return -1;
            if (!got) {
                errno = EIO; /* File became shorter than the verified boundary. */
                return -1;
            }
            scratch->len += (size_t)got;
        }
        int rc = snag_binary_batch_decode(scratch->data, scratch->len, anchor, batch, next);
        if (rc != 1) return rc;
        /* The checksummed header has passed all length/sequence/link bounds.
         * Never allocate based on an unchecked on-disk length. */
        uint64_t length = get_le(scratch->data + 8, 8u);
        if (length > boundary - anchor->end) return 1;
        needed = (size_t)length;
    }
}

int
snag_binary_record_next(const struct snag_binary_batch *batch, size_t *cursor,
    struct snag_binary_record *record, uint64_t *sequence)
{
    if (batch->size < SNAG_BINARY_BATCH_HEADER_SIZE + SNAG_BINARY_BATCH_FOOTER_SIZE ||
        *cursor < SNAG_BINARY_BATCH_HEADER_SIZE ||
        *cursor > batch->size - SNAG_BINARY_BATCH_FOOTER_SIZE) return invalid();
    size_t remaining = batch->size - SNAG_BINARY_BATCH_FOOTER_SIZE - *cursor;
    if (!remaining) return 1;
    size_t size;
    if (record_read(batch->data + *cursor, remaining, record, sequence, &size) < 0)
        return -1;
    *cursor += size;
    return 0;
}
