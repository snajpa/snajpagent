/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary.h"
#include "fs.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

static const unsigned char journal_magic[8] = "SNAGJNL";
static const unsigned char batch_magic[8] = "SNAGBAT";
static const unsigned char footer_magic[8] = "SNAGEND";
static const unsigned char checkpoint_magic[8] = "SNAGCHK";
static const unsigned char checkpoint_footer_magic[8] = "SNAGCPE";

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
snag_binary_legacy_checkpoint_encode(struct snag_buf *out,
    const struct snag_binary_legacy_checkpoint *checkpoint)
{
    if (!out || !checkpoint || !checkpoint->start || checkpoint->end <= checkpoint->start ||
        checkpoint->end > INT64_MAX) {
        return invalid();
    }
    unsigned char bytes[SNAG_BINARY_LEGACY_CHECKPOINT_SIZE];
    put_le(bytes, checkpoint->start, 8u);
    put_le(bytes + 8u, checkpoint->end, 8u);
    memcpy(bytes + 16u, checkpoint->digest, sizeof(checkpoint->digest));
    return snag_buf_append(out, bytes, sizeof(bytes));
}

int
snag_binary_legacy_checkpoint_decode(const struct snag_binary_record *record,
    struct snag_binary_legacy_checkpoint *checkpoint)
{
    if (!record || !checkpoint || record->kind != SNAG_BINARY_LEGACY_CHECKPOINT ||
        record->version != 1u || record->flags != SNAG_BINARY_RECORD_OPTIONAL ||
        record->size != SNAG_BINARY_LEGACY_CHECKPOINT_SIZE || !record->payload) {
        return invalid();
    }
    struct snag_binary_legacy_checkpoint decoded = {
        .start = get_le(record->payload, 8u), .end = get_le(record->payload + 8u, 8u)};
    if (!decoded.start || decoded.end <= decoded.start || decoded.end > INT64_MAX) {
        return invalid();
    }
    memcpy(decoded.digest, record->payload + 16u, sizeof(decoded.digest));
    *checkpoint = decoded;
    return 0;
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
receipt_valid(const struct snag_binary_checkpoint_receipt *receipt)
{
    return receipt && receipt->generation && anchor_valid(&receipt->boundary) &&
        receipt->boundary.turns < receipt->boundary.next_seq;
}

int
snag_binary_checkpoint_receipt_encode(struct snag_buf *out,
    const struct snag_binary_checkpoint_receipt *receipt)
{
    if (!out || !receipt_valid(receipt)) return invalid();
    unsigned char bytes[SNAG_BINARY_CHECKPOINT_RECEIPT_SIZE] = {0};
    put_le(bytes, receipt->generation, 8u);
    put_le(bytes + 8u, receipt->boundary.end, 8u);
    put_le(bytes + 16u, receipt->boundary.next_seq, 8u);
    put_le(bytes + 24u, receipt->boundary.turns, 8u);
    put_le(bytes + 32u, receipt->boundary.previous, 8u);
    memcpy(bytes + 40u, receipt->boundary.digest, 32u);
    memcpy(bytes + 72u, receipt->image_digest, 32u);
    memcpy(bytes + 104u, receipt->index_root, 32u);
    put_le(bytes + 138u, 2u, 2u);
    return snag_buf_append(out, bytes, sizeof(bytes));
}

int
snag_binary_checkpoint_receipt_decode(const struct snag_binary_record *record,
    struct snag_binary_checkpoint_receipt *out)
{
    if (!record || !out || record->kind != SNAG_BINARY_CHECKPOINT_RECEIPT ||
        record->flags != SNAG_BINARY_RECORD_OPTIONAL || !record->version ||
        (!record->payload && record->size)) {
        return invalid();
    }
    if (record->version != 1u) return 1;
    if (record->size != SNAG_BINARY_CHECKPOINT_RECEIPT_SIZE || !record->payload) {
        return invalid();
    }
    const unsigned char *bytes = record->payload;
    struct snag_binary_checkpoint_receipt decoded = {
        .generation = get_le(bytes, 8u), .boundary = {
            .end = get_le(bytes + 8u, 8u), .next_seq = get_le(bytes + 16u, 8u),
            .turns = get_le(bytes + 24u, 8u), .previous = get_le(bytes + 32u, 8u)}
    };
    if (!receipt_valid(&decoded) || get_le(bytes + 140u, 4u)) return invalid();
    if (get_le(bytes + 136u, 2u) || get_le(bytes + 138u, 2u) != 2u) return 1;
    memcpy(decoded.boundary.digest, bytes + 40u, 32u);
    memcpy(decoded.image_digest, bytes + 72u, 32u);
    memcpy(decoded.index_root, bytes + 104u, 32u);
    *out = decoded;
    return 0;
}

int
snag_binary_checkpoint_frame_from_receipt(const void *data, size_t size,
    const struct snag_binary_identity *identity,
    const struct snag_binary_checkpoint_receipt *receipt,
    struct snag_binary_checkpoint_frame *out)
{
    if (!out || !receipt_valid(receipt)) return invalid();
    struct snag_binary_checkpoint_frame frame;
    int rc = snag_binary_checkpoint_frame_decode(data, size, identity, &receipt->boundary, &frame);
    if (rc) return rc;
    const unsigned char *bytes = data;
    if (frame.generation != receipt->generation ||
        memcmp(bytes + size - 32u, receipt->image_digest, 32u)) {
        return invalid();
    }
    *out = frame;
    return 0;
}

int
snag_binary_checkpoint_encoder_init(struct snag_binary_checkpoint_encoder *out,
    const struct snag_binary_checkpoint_frame *frame)
{
    if (!out || !frame || !frame->generation || !anchor_valid(&frame->boundary) ||
        !frame->core.version || !frame->core.size || !frame->core.data ||
        !frame->provider.version || !frame->provider.size || !frame->provider.data) {
        return invalid();
    }
    size_t size;
    if (!snag_size_add(frame->core.size, frame->provider.size, &size) ||
        !snag_size_add(size, SNAG_BINARY_CHECKPOINT_HEADER_SIZE +
            SNAG_BINARY_CHECKPOINT_FOOTER_SIZE, &size) || size > INT64_MAX) {
        return snag_errno(EOVERFLOW);
    }
    struct snag_binary_checkpoint_encoder encoder = {.frame = *frame, .total = size};
    unsigned char *header = encoder.header;
    memcpy(header, checkpoint_magic, sizeof(checkpoint_magic));
    put_le(header + 10u, 1u, 2u);
    put_le(header + 12u, sizeof(encoder.header), 4u);
    put_le(header + 16u, size, 8u);
    put_le(header + 24u, frame->generation, 8u);
    memcpy(header + 32u, frame->identity.id, sizeof(frame->identity.id));
    put_le(header + 48u, frame->identity.created_ms, 8u);
    put_le(header + 56u, frame->boundary.end, 8u);
    put_le(header + 64u, frame->boundary.next_seq, 8u);
    put_le(header + 72u, frame->boundary.turns, 8u);
    put_le(header + 80u, frame->boundary.previous, 8u);
    memcpy(header + 88u, frame->boundary.digest, sizeof(frame->boundary.digest));
    put_le(header + 120u, frame->core.version, 2u);
    put_le(header + 122u, frame->provider.version, 2u);
    put_le(header + 128u, frame->core.size, 8u);
    put_le(header + 136u, frame->provider.size, 8u);
    unsigned char *footer = encoder.footer;
    memcpy(footer, checkpoint_footer_magic, sizeof(checkpoint_footer_magic));
    put_le(footer + 8u, size, 8u);
    snag_sha256_init(&encoder.hash);
    *out = encoder;
    return 0;
}

int
snag_binary_checkpoint_encoder_next(struct snag_binary_checkpoint_encoder *encoder,
    size_t budget, const unsigned char **data, size_t *size)
{
    if (!encoder || !budget || !data || !size ||
        encoder->total < SNAG_BINARY_CHECKPOINT_HEADER_SIZE +
            SNAG_BINARY_CHECKPOINT_FOOTER_SIZE + 2u || encoder->position > encoder->total) {
        return invalid();
    }
    if (encoder->position == encoder->total) return 1;
    size_t offset = encoder->position;
    size_t count;
    const unsigned char *chunk;
    size_t core_end = sizeof(encoder->header) + encoder->frame.core.size;
    size_t footer_start = encoder->total - sizeof(encoder->footer);
    size_t digest_start = encoder->total - 32u;
    if (offset < sizeof(encoder->header)) {
        chunk = encoder->header + offset;
        count = sizeof(encoder->header) - offset;
    } else if (offset < core_end) {
        chunk = encoder->frame.core.data + offset - sizeof(encoder->header);
        count = core_end - offset;
    } else if (offset < footer_start) {
        chunk = encoder->frame.provider.data + offset - core_end;
        count = footer_start - offset;
    } else if (offset < digest_start) {
        chunk = encoder->footer + offset - footer_start;
        count = digest_start - offset;
    } else {
        if (offset == digest_start) {
            snag_sha256_final(&encoder->hash, encoder->footer + sizeof(encoder->footer) - 32u);
        }
        chunk = encoder->footer + offset - footer_start;
        count = encoder->total - offset;
    }
    if (count > budget) count = budget;
    if (offset < digest_start) snag_sha256_update(&encoder->hash, chunk, count);
    encoder->position += count;
    *data = chunk;
    *size = count;
    return 0;
}

int
snag_binary_checkpoint_frame_encode(struct snag_buf *out,
    const struct snag_binary_checkpoint_frame *frame)
{
    if (!out) return invalid();
    struct snag_binary_checkpoint_encoder encoder;
    if (snag_binary_checkpoint_encoder_init(&encoder, frame) < 0) return -1;
    if (out->len > out->max || encoder.total > out->max - out->len) {
        return snag_errno(EOVERFLOW);
    }

    /* Keep source views alive through the final append, including aliases into
     * out. Reserve scratch once instead of reallocating it for each section. */
    struct snag_buf encoded = {.max = encoder.total};
    if (snag_buf_reserve(&encoded, encoder.total) < 0) goto fail;
    const unsigned char *data;
    size_t size;
    int rc;
    while ((rc = snag_binary_checkpoint_encoder_next(&encoder,
        SIZE_MAX, &data, &size)) == 0) {
        if (snag_buf_append(&encoded, data, size) < 0) goto fail;
    }
    if (rc < 0 || snag_buf_append(out, encoded.data, encoded.len) < 0) goto fail;
    snag_buf_free(&encoded);
    return 0;
fail:
    snag_buf_free(&encoded);
    return -1;
}

int
snag_binary_checkpoint_frame_decode(const void *data, size_t size,
    const struct snag_binary_identity *identity, const struct snag_binary_anchor *boundary,
    struct snag_binary_checkpoint_frame *frame)
{
    if ((!data && size) || !identity || !boundary || !frame || !anchor_valid(boundary)) {
        return invalid();
    }
    if (size < SNAG_BINARY_CHECKPOINT_HEADER_SIZE) return 1;
    const unsigned char *bytes = data;
    uint64_t total = get_le(bytes + 16u, 8u);
    uint64_t core = get_le(bytes + 128u, 8u);
    uint64_t provider = get_le(bytes + 136u, 8u);
    size_t overhead = SNAG_BINARY_CHECKPOINT_HEADER_SIZE + SNAG_BINARY_CHECKPOINT_FOOTER_SIZE;
    if (memcmp(bytes, checkpoint_magic, sizeof(checkpoint_magic)) ||
        get_le(bytes + 8u, 2u) || get_le(bytes + 10u, 2u) != 1u ||
        get_le(bytes + 12u, 4u) != SNAG_BINARY_CHECKPOINT_HEADER_SIZE ||
        total > INT64_MAX || total > SIZE_MAX || total < overhead ||
        !core || core > total - overhead || !provider || provider != total - overhead - core ||
        !get_le(bytes + 24u, 8u) || !get_le(bytes + 120u, 2u) || !get_le(bytes + 122u, 2u) ||
        get_le(bytes + 124u, 4u) || get_le(bytes + 144u, 8u) || get_le(bytes + 152u, 8u)) {
        return invalid();
    }
    /* Identity and every anchor member must come from independent verification,
     * not from accepting this checksum or looking up an untrusted index hint. */
    if (memcmp(bytes + 32u, identity->id, sizeof(identity->id)) ||
        get_le(bytes + 48u, 8u) != identity->created_ms ||
        get_le(bytes + 56u, 8u) != boundary->end ||
        get_le(bytes + 64u, 8u) != boundary->next_seq ||
        get_le(bytes + 72u, 8u) != boundary->turns ||
        get_le(bytes + 80u, 8u) != boundary->previous ||
        memcmp(bytes + 88u, boundary->digest, sizeof(boundary->digest))) return invalid();
    if (size < total) return 1;
    if (size != total) return invalid();
    const unsigned char *footer = bytes + size - SNAG_BINARY_CHECKPOINT_FOOTER_SIZE;
    unsigned char hash[32];
    digest(bytes, size - sizeof(hash), hash);
    if (memcmp(footer, checkpoint_footer_magic, sizeof(checkpoint_footer_magic)) ||
        get_le(footer + 8u, 8u) != total ||
        memcmp(hash, footer + SNAG_BINARY_CHECKPOINT_FOOTER_SIZE - sizeof(hash), sizeof(hash))) {
        return invalid();
    }
    struct snag_binary_checkpoint_frame decoded = {
        .identity = *identity, .boundary = *boundary, .generation = get_le(bytes + 24u, 8u),
        .core = {.version = (uint16_t)get_le(bytes + 120u, 2u),
            .data = bytes + SNAG_BINARY_CHECKPOINT_HEADER_SIZE, .size = (size_t)core},
        .provider = {.version = (uint16_t)get_le(bytes + 122u, 2u),
            .data = bytes + SNAG_BINARY_CHECKPOINT_HEADER_SIZE + (size_t)core,
            .size = (size_t)provider}};
    *frame = decoded;
    return 0;
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

static int
read_full_at(int fd, unsigned char *out, size_t size, uint64_t offset)
{
    if (offset > INT64_MAX || size > (uint64_t)INT64_MAX - offset) return invalid();
    while (size) {
        size_t chunk = size > 65536u ? 65536u : size;
        ssize_t got = snag_pread(fd, out, chunk, (int64_t)offset);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) return -1;
        if (!got) return snag_errno(EIO);
        out += (size_t)got;
        offset += (size_t)got;
        size -= (size_t)got;
    }
    return 0;
}

static bool
anchors_equal(const struct snag_binary_anchor *a, const struct snag_binary_anchor *b)
{
    return a->end == b->end && a->next_seq == b->next_seq && a->turns == b->turns &&
        a->previous == b->previous && !memcmp(a->digest, b->digest, sizeof(a->digest));
}

static int
verify_root(int fd, const struct snag_binary_anchor *anchor)
{
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    struct snag_binary_anchor root;
    struct snag_binary_identity identity;
    if (read_full_at(fd, header, sizeof(header), 0u) < 0) return -1;
    if (snag_binary_header_decode(header, sizeof(header), &identity, &root) != 0 ||
        !anchors_equal(anchor, &root)) return invalid();
    return 0;
}

/* Structural header checks precede every length-driven read/allocation. The
 * predecessor turn count is absent from this header and remains provisional. */
static int
batch_header_anchor(const unsigned char header[SNAG_BINARY_BATCH_HEADER_SIZE], uint64_t offset,
    struct snag_binary_anchor *before, uint64_t *length)
{
    struct snag_binary_anchor previous = {
        .end = offset, .next_seq = get_le(header + 24, 8u),
        .previous = get_le(header + 32, 8u)
    };
    memcpy(previous.digest, header + 40, sizeof(previous.digest));
    struct snag_binary_anchor checked;
    struct snag_binary_batch decoded;
    if (snag_binary_batch_decode(header, SNAG_BINARY_BATCH_HEADER_SIZE,
            &previous, &decoded, &checked) != 1) {
        return invalid();
    }
    *length = get_le(header + 8, 8u);
    *before = previous;
    return 0;
}

/* Authenticate one complete batch from its committed end. The returned previous
 * turn count is provisional zero: it is absent from this batch's header. */
static int
read_authenticated_batch(int fd, const struct snag_binary_anchor *after,
    struct snag_buf *scratch, struct snag_binary_batch *batch, struct snag_binary_anchor *before)
{
    if (!anchor_valid(after) || after->end == SNAG_BINARY_HEADER_SIZE) return invalid();
    uint64_t length = after->end - after->previous;
    if (length > SNAG_BINARY_BATCH_MAX || length < SNAG_BINARY_BATCH_HEADER_SIZE +
        SNAG_BINARY_RECORD_HEADER_SIZE + SNAG_BINARY_BATCH_FOOTER_SIZE) return invalid();
    unsigned char header[SNAG_BINARY_BATCH_HEADER_SIZE];
    if (read_full_at(fd, header, sizeof(header), after->previous) < 0) return -1;
    struct snag_binary_anchor previous;
    struct snag_binary_anchor checked;
    struct snag_binary_batch decoded;
    uint64_t header_length;
    if (batch_header_anchor(header, after->previous, &previous, &header_length) < 0 ||
        header_length != length) {
        return invalid();
    }
    snag_buf_reset(scratch);
    if (snag_buf_reserve(scratch, (size_t)length) < 0) return -1;
    if (read_full_at(fd, scratch->data, (size_t)length, after->previous) < 0) return -1;
    scratch->len = (size_t)length;
    if (snag_binary_batch_decode(scratch->data, scratch->len,
            &previous, &decoded, &checked) != 0 || !anchors_equal(&checked, after)) {
        return invalid();
    }
    if (previous.end == SNAG_BINARY_HEADER_SIZE && verify_root(fd, &previous) < 0) return -1;
    *batch = decoded;
    *before = previous;
    return 0;
}

int
snag_binary_batch_previous(int fd, const struct snag_binary_anchor *after,
    struct snag_buf *scratch, struct snag_binary_batch *batch, struct snag_binary_anchor *before)
{
    if (!after || !scratch || !batch || !before) return invalid();
    snag_buf_reset(scratch);
    if (!anchor_valid(after)) return invalid();
    if (after->end == SNAG_BINARY_HEADER_SIZE) return verify_root(fd, after) < 0 ? -1 : 1;
    struct snag_binary_anchor previous;
    struct snag_binary_batch current;
    if (read_authenticated_batch(fd, after, scratch, &current, &previous) < 0) return -1;
    if (previous.end != SNAG_BINARY_HEADER_SIZE) {
        unsigned char footer[SNAG_BINARY_BATCH_FOOTER_SIZE];
        if (previous.end < SNAG_BINARY_HEADER_SIZE + sizeof(footer)) return invalid();
        if (read_full_at(fd, footer, sizeof(footer), previous.end - sizeof(footer)) < 0) return -1;
        previous.turns = get_le(footer + 32, 8u);
        /* Merely comparing the footer's digest field would not authenticate its
         * turn count. Hash/decode that whole predecessor against the link in the
         * already authenticated current batch before exposing the anchor. */
        struct snag_buf parent_bytes = {.max = SNAG_BINARY_BATCH_MAX};
        struct snag_binary_batch parent;
        struct snag_binary_anchor grandparent;
        int rc = read_authenticated_batch(fd, &previous, &parent_bytes, &parent, &grandparent);
        snag_buf_free(&parent_bytes);
        if (rc < 0) return -1;
        if (previous.turns > after->turns) return invalid();
    }
    *batch = current;
    *before = previous;
    return 0;
}

int
snag_binary_batch_at(int fd, uint64_t boundary, uint64_t offset,
    const unsigned char hash[32], struct snag_buf *scratch, struct snag_binary_batch *batch,
    struct snag_binary_anchor *before, struct snag_binary_anchor *after)
{
    if (fd < 0 || !hash || !scratch || !batch || !before || !after ||
        offset < SNAG_BINARY_HEADER_SIZE || boundary > INT64_MAX || boundary < offset ||
        boundary - offset < SNAG_BINARY_BATCH_HEADER_SIZE) {
        return invalid();
    }
    unsigned char header[SNAG_BINARY_BATCH_HEADER_SIZE];
    if (read_full_at(fd, header, sizeof(header), offset) < 0) return -1;
    struct snag_binary_anchor previous;
    uint64_t length;
    if (batch_header_anchor(header, offset, &previous, &length) < 0 ||
        length > boundary - offset) {
        return invalid();
    }
    unsigned char footer[SNAG_BINARY_BATCH_FOOTER_SIZE];
    if (read_full_at(fd, footer, sizeof(footer), offset + length - sizeof(footer)) < 0) {
        return -1;
    }
    struct snag_binary_anchor committed = {
        .end = offset + length, .previous = offset,
        .next_seq = get_le(footer + 24, 8u) + 1u, .turns = get_le(footer + 32, 8u)
    };
    memcpy(committed.digest, hash, sizeof(committed.digest));
    struct snag_binary_batch found;
    if (snag_binary_batch_previous(fd, &committed, scratch, &found, &previous) < 0) {
        return -1;
    }
    *batch = found;
    *before = previous;
    *after = committed;
    return 0;
}

int
snag_binary_batch_find(int fd, const struct snag_binary_anchor *through, uint64_t sequence,
    struct snag_buf *scratch, struct snag_binary_batch *batch, struct snag_binary_anchor *before)
{
    if (!through || !scratch || !batch || !before) return invalid();
    snag_buf_reset(scratch);
    if (!anchor_valid(through) || !sequence || sequence >= through->next_seq) return invalid();
    struct snag_binary_anchor cursor = *through;
    for (;;) {
        struct snag_binary_batch found;
        struct snag_binary_anchor previous;
        int rc = snag_binary_batch_previous(fd, &cursor, scratch, &found, &previous);
        if (rc < 0) return -1;
        if (rc == 1) return invalid();
        if (sequence >= previous.next_seq) {
            *batch = found;
            *before = previous;
            return 0;
        }
        cursor = previous;
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
