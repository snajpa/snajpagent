/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary.h"
#include "fs.h"
#include "store_binary_wire.h"

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
    put_le(out + 10, 2u, 2u); /* Draft major 0, minor 2: zero-free batch envelope. */
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
        get_le(bytes + 8, 2u) != 0u || get_le(bytes + 10, 2u) != 2u ||
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
    return receipt && receipt->generation &&
        receipt->image_size >= SNAG_BINARY_CHECKPOINT_HEADER_SIZE +
            SNAG_BINARY_CHECKPOINT_FOOTER_SIZE + 2u &&
        receipt->image_size <= INT64_MAX && anchor_valid(&receipt->boundary) &&
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
    put_le(bytes + 144u, receipt->image_size, 8u);
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
    if (record->version != SNAG_BINARY_CHECKPOINT_RECEIPT_VERSION) return 1;
    if (record->size != SNAG_BINARY_CHECKPOINT_RECEIPT_SIZE || !record->payload) {
        return invalid();
    }
    const unsigned char *bytes = record->payload;
    struct snag_binary_checkpoint_receipt decoded = {
        .generation = get_le(bytes, 8u), .image_size = get_le(bytes + 144u, 8u), .boundary = {
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
    if (!out || !receipt_valid(receipt) || size > receipt->image_size) return invalid();
    struct snag_binary_checkpoint_frame frame;
    int rc = snag_binary_checkpoint_frame_decode(data, size, identity, &receipt->boundary, &frame);
    if (rc) return rc;
    const unsigned char *bytes = data;
    if (size != receipt->image_size || frame.generation != receipt->generation ||
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
        !frame->provider.version || !frame->provider.size || !frame->provider.data ||
        (frame->access.size && (!frame->access.version || !frame->access.data)) ||
        (!frame->access.size && frame->access.version)) {
        return invalid();
    }
    size_t size;
    if (!snag_size_add(frame->core.size, frame->provider.size, &size) ||
        !snag_size_add(size, frame->access.size, &size) ||
        !snag_size_add(size, SNAG_BINARY_CHECKPOINT_HEADER_SIZE +
            SNAG_BINARY_CHECKPOINT_FOOTER_SIZE, &size) || size > INT64_MAX) {
        return snag_errno(EOVERFLOW);
    }
    struct snag_binary_checkpoint_encoder encoder = {.frame = *frame, .total = size};
    unsigned char *header = encoder.header;
    memcpy(header, checkpoint_magic, sizeof(checkpoint_magic));
    put_le(header + 10u, 2u, 2u);
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
    put_le(header + 124u, frame->access.version, 2u);
    put_le(header + 128u, frame->core.size, 8u);
    put_le(header + 136u, frame->provider.size, 8u);
    put_le(header + 144u, frame->access.size, 8u);
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
    size_t provider_end = core_end + encoder->frame.provider.size;
    size_t footer_start = encoder->total - sizeof(encoder->footer);
    size_t digest_start = encoder->total - 32u;
    if (offset < sizeof(encoder->header)) {
        chunk = encoder->header + offset;
        count = sizeof(encoder->header) - offset;
    } else if (offset < core_end) {
        chunk = encoder->frame.core.data + offset - sizeof(encoder->header);
        count = core_end - offset;
    } else if (offset < provider_end) {
        chunk = encoder->frame.provider.data + offset - core_end;
        count = provider_end - offset;
    } else if (offset < footer_start) {
        chunk = encoder->frame.access.data + offset - provider_end;
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

static int
checkpoint_frame_view(const void *data, size_t size,
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
    uint64_t access = get_le(bytes + 144u, 8u);
    uint16_t access_version = (uint16_t)get_le(bytes + 124u, 2u);
    size_t overhead = SNAG_BINARY_CHECKPOINT_HEADER_SIZE + SNAG_BINARY_CHECKPOINT_FOOTER_SIZE;
    if (memcmp(bytes, checkpoint_magic, sizeof(checkpoint_magic)) ||
        get_le(bytes + 8u, 2u) || get_le(bytes + 10u, 2u) != 2u ||
        get_le(bytes + 12u, 4u) != SNAG_BINARY_CHECKPOINT_HEADER_SIZE ||
        total > INT64_MAX || total > SIZE_MAX || total < overhead ||
        !core || core > total - overhead || !provider || provider > total - overhead - core ||
        access != total - overhead - core - provider ||
        (access && !access_version) || (!access && access_version) ||
        !get_le(bytes + 24u, 8u) || !get_le(bytes + 120u, 2u) || !get_le(bytes + 122u, 2u) ||
        get_le(bytes + 126u, 2u) || get_le(bytes + 152u, 8u)) {
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
    if (memcmp(footer, checkpoint_footer_magic, sizeof(checkpoint_footer_magic)) ||
        get_le(footer + 8u, 8u) != total) return invalid();
    struct snag_binary_checkpoint_frame decoded = {
        .identity = *identity, .boundary = *boundary, .generation = get_le(bytes + 24u, 8u),
        .core = {.version = (uint16_t)get_le(bytes + 120u, 2u),
            .data = bytes + SNAG_BINARY_CHECKPOINT_HEADER_SIZE, .size = (size_t)core},
        .provider = {.version = (uint16_t)get_le(bytes + 122u, 2u),
            .data = bytes + SNAG_BINARY_CHECKPOINT_HEADER_SIZE + (size_t)core,
            .size = (size_t)provider},
        .access = {.version = access_version,
            .data = access ? bytes + SNAG_BINARY_CHECKPOINT_HEADER_SIZE +
                (size_t)core + (size_t)provider : NULL, .size = (size_t)access}};
    *frame = decoded;
    return 0;
}

int
snag_binary_checkpoint_frame_decode(const void *data, size_t size,
    const struct snag_binary_identity *identity, const struct snag_binary_anchor *boundary,
    struct snag_binary_checkpoint_frame *frame)
{
    if (!frame) return invalid();
    struct snag_binary_checkpoint_frame candidate;
    int rc = checkpoint_frame_view(data, size, identity, boundary, &candidate);
    if (rc) return rc;
    const unsigned char *bytes = data;
    unsigned char hash[32];
    digest(bytes, size - sizeof(hash), hash);
    if (memcmp(hash, bytes + size - sizeof(hash), sizeof(hash))) return invalid();
    *frame = candidate;
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
    const struct snag_binary_record *records, uint32_t count, uint64_t turns,
    struct snag_binary_anchor *next)
{
    size_t size = SNAG_BINARY_BATCH_HEADER_SIZE + SNAG_BINARY_BATCH_FOOTER_SIZE;
    size_t maximum = count == 1u ? SNAG_BINARY_BATCH_MAX : SNAG_BINARY_BATCH_TARGET;
    if (!out || !anchor || !anchor_valid(anchor) || !records || !count ||
        count > (SNAG_BINARY_BATCH_TARGET - size) / SNAG_BINARY_RECORD_HEADER_SIZE ||
        turns < anchor->turns || count > UINT64_MAX - anchor->next_seq) return invalid();
    for (uint32_t i = 0; i < count; ++i) {
        if (!record_valid(&records[i]) ||
            SNAG_BINARY_RECORD_HEADER_SIZE + records[i].size > maximum - size)
            return invalid();
        size += SNAG_BINARY_RECORD_HEADER_SIZE + records[i].size;
    }
    size_t wire_size;
    if (snag_binary_wire_size(size, &wire_size) < 0 ||
        wire_size > (uint64_t)INT64_MAX - anchor->end) {
        return invalid();
    }
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
    struct snag_binary_anchor prepared = {
        .end = anchor->end + wire_size, .next_seq = anchor->next_seq + count,
        .turns = turns, .previous = anchor->end
    };
    memcpy(prepared.digest, footer + sizeof(footer) - 32u, sizeof(prepared.digest));
    if (snag_buf_append(&encoded, footer + sizeof(footer) - 32u, 32u) < 0 ||
        snag_buf_append(out, encoded.data, encoded.len) < 0) goto fail;
    if (next) *next = prepared;
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
    size_t wire_size;
    if (snag_binary_wire_size((size_t)length, &wire_size) < 0 ||
        wire_size > (uint64_t)INT64_MAX - anchor->end) {
        return invalid();
    }
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
        .end = anchor->end + wire_size, .next_seq = anchor->next_seq + count,
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

int
snag_binary_checkpoint_image_probe(int fd, unsigned char image_digest[32])
{
    if (fd < 0 || !image_digest) return invalid();
    snag_file_info before, after;
    if (snag_fstat(fd, &before) < 0) return -1;
    if (!S_ISREG(before.st_mode) || before.st_size <
        SNAG_BINARY_CHECKPOINT_HEADER_SIZE + SNAG_BINARY_CHECKPOINT_FOOTER_SIZE + 2u) {
        return invalid();
    }
    unsigned char hash[32];
    if (read_full_at(fd, hash, sizeof(hash), (uint64_t)before.st_size - sizeof(hash)) < 0 ||
        snag_fstat(fd, &after) < 0) return -1;
    if (!snag_file_unchanged(&before, &after)) return snag_errno(ESTALE);
    memcpy(image_digest, hash, sizeof(hash));
    return 0;
}

int
snag_binary_checkpoint_image_read(int fd, const struct snag_binary_identity *identity,
    const struct snag_binary_checkpoint_receipt *receipt, struct snag_buf *image,
    struct snag_binary_checkpoint_frame *out, bool (*cancelled)(void *), void *opaque)
{
    if (fd < 0 || !identity || !receipt_valid(receipt) || !image || !out) return invalid();
    snag_file_info before, after;
    if (snag_fstat(fd, &before) < 0) return -1;
    if (!S_ISREG(before.st_mode) || before.st_size < 0 ||
        (uint64_t)before.st_size != receipt->image_size) return invalid();
    if (receipt->image_size > SIZE_MAX) return snag_errno(EOVERFLOW);
    if (cancelled && cancelled(opaque)) return snag_errno(ECANCELED);
    struct snag_buf staged = {.max = image->max};
    struct snag_binary_checkpoint_frame frame;
    size_t size = (size_t)receipt->image_size;
    if (snag_buf_reserve(&staged, size) < 0) goto done;
    struct snag_sha256 hashing;
    snag_sha256_init(&hashing);
    while (staged.len < size) {
        if (cancelled && cancelled(opaque)) {
            errno = ECANCELED;
            goto done;
        }
        size_t chunk = size - staged.len;
        if (chunk > 65536u) chunk = 65536u;
        if (read_full_at(fd, staged.data + staged.len, chunk, staged.len) < 0) goto done;
        size_t hashed = staged.len < size - 32u ? size - 32u - staged.len : 0u;
        if (hashed > chunk) hashed = chunk;
        snag_sha256_update(&hashing, staged.data + staged.len, hashed);
        staged.len += chunk;
    }
    unsigned char hash[32];
    snag_sha256_final(&hashing, hash);
    if (checkpoint_frame_view(staged.data, staged.len, identity, &receipt->boundary, &frame) ||
        frame.generation != receipt->generation ||
        memcmp(hash, staged.data + size - sizeof(hash), sizeof(hash)) ||
        memcmp(hash, receipt->image_digest, sizeof(hash))) {
        errno = EINVAL;
        goto done;
    }
    if (cancelled && cancelled(opaque)) {
        errno = ECANCELED;
        goto done;
    }
    if (snag_fstat(fd, &after) < 0) goto done;
    if (!snag_file_unchanged(&before, &after)) {
        errno = ESTALE;
        goto done;
    }
    snag_buf_free(image);
    *image = staged;
    *out = frame;
    return 0;
done:
    snag_buf_free(&staged);
    return -1;
}

/* An incomplete candidate can be discarded only if no earlier delimiter closes
 * it. Check the entire bounded tail without allocating its declared full size. */
static int
tail_is_open(int fd, uint64_t start, uint64_t end)
{
    unsigned char bytes[65536];
    while (start < end) {
        size_t size = end - start > sizeof(bytes) ? sizeof(bytes) : (size_t)(end - start);
        if (read_full_at(fd, bytes, size, start) < 0) return -1;
        if (memchr(bytes, 0, size)) return invalid();
        start += size;
    }
    return 1;
}

static int
read_wire_header(int fd, uint64_t offset,
    unsigned char header[SNAG_BINARY_BATCH_HEADER_SIZE])
{
    unsigned char wire[SNAG_BINARY_BATCH_HEADER_SIZE + 1u];
    if (read_full_at(fd, wire, sizeof(wire), offset) < 0) return -1;
    return snag_binary_wire_header(wire, sizeof(wire), header) == 0 ? 0 : invalid();
}

int
snag_binary_batch_read(int fd, uint64_t boundary,
    const struct snag_binary_anchor *anchor, struct snag_buf *scratch,
    struct snag_binary_batch *batch, struct snag_binary_anchor *next)
{
    if (fd < 0 || !anchor || !scratch || !batch || !next) return invalid();
    snag_buf_reset(scratch);
    if (!anchor_valid(anchor) || boundary > INT64_MAX || boundary < anchor->end) {
        return invalid();
    }
    if (boundary - anchor->end < SNAG_BINARY_BATCH_HEADER_SIZE + 1u) {
        return tail_is_open(fd, anchor->end, boundary);
    }
    unsigned char header[SNAG_BINARY_BATCH_HEADER_SIZE];
    if (read_wire_header(fd, anchor->end, header) < 0 ||
        snag_buf_append(scratch, header, sizeof(header)) < 0) {
        return -1;
    }
    if (snag_binary_batch_decode(header, sizeof(header), anchor, batch, next) != 1) {
        return invalid();
    }
    /* Only a checked header may determine allocation and physical extent. */
    size_t needed;
    if (snag_binary_wire_size((size_t)get_le(header + 8, 8u), &needed) < 0) return -1;
    if (needed > boundary - anchor->end) {
        return tail_is_open(fd, anchor->end, boundary);
    }
    struct snag_buf wire = {.max = SNAG_BINARY_WIRE_BATCH_MAX};
    int rc = -1;
    if (snag_buf_reserve(&wire, needed) < 0 ||
        read_full_at(fd, wire.data, needed, anchor->end) < 0) {
        goto done;
    }
    snag_buf_reset(scratch);
    if (snag_binary_wire_decode(scratch, wire.data, needed) < 0) goto done;
    rc = snag_binary_batch_decode(scratch->data, scratch->len, anchor, batch, next);
done:
    snag_buf_free(&wire);
    return rc;
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

/* The hash may be absent only for EOF discovery at actual physical delimiters.
 * All indexed/backward callers supply an independently established digest. The
 * predecessor turn count is provisional until that whole predecessor is checked. */
static int
read_linked_batch(int fd, uint64_t boundary, uint64_t offset, const unsigned char hash[32],
    struct snag_buf *scratch, struct snag_binary_batch *batch,
    struct snag_binary_anchor *before, struct snag_binary_anchor *after)
{
    if (offset < SNAG_BINARY_HEADER_SIZE || boundary > INT64_MAX || boundary < offset ||
        boundary - offset < SNAG_BINARY_BATCH_HEADER_SIZE + 1u) {
        return invalid();
    }
    unsigned char header[SNAG_BINARY_BATCH_HEADER_SIZE];
    if (read_wire_header(fd, offset, header) < 0) return -1;
    struct snag_binary_anchor previous;
    uint64_t length;
    size_t wire_size;
    if (batch_header_anchor(header, offset, &previous, &length) < 0 ||
        snag_binary_wire_size((size_t)length, &wire_size) < 0 ||
        wire_size > boundary - offset) {
        return invalid();
    }
    struct snag_binary_anchor checked;
    struct snag_binary_batch decoded;
    int rc = snag_binary_batch_read(fd, offset + wire_size, &previous,
        scratch, &decoded, &checked);
    if (rc < 0) return -1;
    if (rc != 0 || (hash && memcmp(checked.digest, hash, 32u))) return invalid();
    if (previous.end == SNAG_BINARY_HEADER_SIZE && verify_root(fd, &previous) < 0) {
        return -1;
    }
    *batch = decoded;
    *before = previous;
    *after = checked;
    return 0;
}

static int
verify_parent(int fd, struct snag_binary_anchor *previous, uint64_t turns)
{
    if (previous->end == SNAG_BINARY_HEADER_SIZE) return 0;
    struct snag_buf bytes = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_binary_batch parent;
    struct snag_binary_anchor before;
    struct snag_binary_anchor after;
    int rc = read_linked_batch(fd, previous->end, previous->previous, previous->digest,
        &bytes, &parent, &before, &after);
    snag_buf_free(&bytes);
    if (rc < 0) return -1;
    previous->turns = after.turns;
    if (!anchors_equal(previous, &after) || after.turns > turns) return invalid();
    return 0;
}

/* Search at most one physical batch back from a captured end. Zero bytes inside
 * the immutable file header are outside the envelope and cannot be delimiters. */
static int
last_wire_end(int fd, uint64_t end, uint64_t *out)
{
    unsigned char bytes[65536];
    uint64_t position = end;
    size_t remaining = SNAG_BINARY_WIRE_BATCH_MAX;
    while (position > SNAG_BINARY_HEADER_SIZE && remaining) {
        uint64_t available = position - SNAG_BINARY_HEADER_SIZE;
        size_t size = available > sizeof(bytes) ? sizeof(bytes) : (size_t)available;
        if (size > remaining) size = remaining;
        position -= size;
        if (read_full_at(fd, bytes, size, position) < 0) return -1;
        for (size_t i = size; i > 0u; --i) {
            if (!bytes[i - 1u]) {
                *out = position + i;
                return 0;
            }
        }
        remaining -= size;
    }
    if (position != SNAG_BINARY_HEADER_SIZE) return invalid();
    *out = SNAG_BINARY_HEADER_SIZE;
    return 0;
}

int
snag_binary_journal_tail(int fd, uint64_t boundary, struct snag_buf *scratch,
    struct snag_binary_identity *identity, struct snag_binary_anchor *out, uint64_t *incomplete)
{
    if (fd < 0 || !scratch || !identity || !out || !incomplete ||
        boundary < SNAG_BINARY_HEADER_SIZE || boundary > INT64_MAX) {
        return invalid();
    }
    snag_buf_reset(scratch);
    unsigned char header[SNAG_BINARY_HEADER_SIZE];
    struct snag_binary_identity found;
    struct snag_binary_anchor committed;
    if (read_full_at(fd, header, sizeof(header), 0u) < 0) return -1;
    if (snag_binary_header_decode(header, sizeof(header), &found, &committed) != 0) {
        return invalid();
    }
    uint64_t end;
    if (last_wire_end(fd, boundary, &end) < 0) return -1;
    if (end != SNAG_BINARY_HEADER_SIZE) {
        uint64_t start;
        if (last_wire_end(fd, end - 1u, &start) < 0) return -1;
        struct snag_binary_anchor before;
        struct snag_binary_batch batch;
        if (read_linked_batch(fd, end, start, NULL, scratch, &batch, &before, &committed) < 0) {
            return -1;
        }
        if (committed.end != end) return invalid();
        if (verify_parent(fd, &before, committed.turns) < 0) return -1;
    }
    /* A lost final delimiter must fail at its declared position, rather than
     * quietly rewinding an already complete batch to the preceding delimiter. */
    struct snag_binary_batch tail;
    struct snag_binary_anchor next;
    int rc = snag_binary_batch_read(fd, boundary, &committed, scratch, &tail, &next);
    if (rc < 0) return -1;
    if (rc != 1) return invalid();
    *identity = found;
    *out = committed;
    *incomplete = boundary - committed.end;
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
    struct snag_binary_anchor checked;
    struct snag_binary_batch current;
    if (read_linked_batch(fd, after->end, after->previous, after->digest,
            scratch, &current, &previous, &checked) < 0) {
        return -1;
    }
    if (!anchors_equal(&checked, after)) return invalid();
    if (verify_parent(fd, &previous, after->turns) < 0) return -1;
    *batch = current;
    *before = previous;
    return 0;
}

int
snag_binary_batch_at(int fd, uint64_t boundary, uint64_t offset,
    const unsigned char hash[32], struct snag_buf *scratch, struct snag_binary_batch *batch,
    struct snag_binary_anchor *before, struct snag_binary_anchor *after)
{
    if (fd < 0 || !hash || !scratch || !batch || !before || !after) return invalid();
    snag_buf_reset(scratch);
    struct snag_binary_anchor previous;
    struct snag_binary_anchor committed;
    struct snag_binary_batch found;
    if (read_linked_batch(fd, boundary, offset, hash, scratch, &found,
            &previous, &committed) < 0) {
        return -1;
    }
    if (verify_parent(fd, &previous, committed.turns) < 0) return -1;
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
snag_binary_checkpoint_receipts_find(int fd, const struct snag_binary_anchor *through,
    uint64_t floor, const unsigned char *const images[2], struct snag_buf *scratch,
    struct snag_binary_checkpoint_receipt out[2], bool (*cancelled)(void *), void *opaque)
{
    if (fd < 0 || !through || !images || !scratch || !out || !anchor_valid(through) ||
        through->turns >= through->next_seq || floor < SNAG_BINARY_HEADER_SIZE ||
        floor > through->end) {
        return invalid();
    }
    struct snag_binary_checkpoint_receipt candidates[2] = {0};
    unsigned unseen = (images[0] ? 1u : 0u) | (images[1] ? 2u : 0u);
    unsigned char hashes[2][32];
    for (size_t i = 0u; i < 2u; ++i) {
        if (images[i]) memcpy(hashes[i], images[i], 32u);
    }
    unsigned pending = 0u, verified = 0u;
    struct snag_binary_anchor cursor = *through;
    while (unseen || pending) {
        if (cancelled && cancelled(opaque)) return snag_errno(ECANCELED);
        if (cursor.end <= floor || cursor.previous < floor) break;
        struct snag_binary_batch batch;
        struct snag_binary_anchor before;
        if (snag_binary_batch_previous(fd, &cursor, scratch, &batch, &before) != 0) {
            return -1;
        }
        size_t position = SNAG_BINARY_BATCH_HEADER_SIZE;
        struct snag_binary_record record;
        uint64_t sequence;
        unsigned found = 0u;
        int rc;
        while ((rc = snag_binary_record_next(&batch, &position, &record, &sequence)) == 0) {
            if (record.kind != SNAG_BINARY_CHECKPOINT_RECEIPT) continue;
            struct snag_binary_checkpoint_receipt receipt;
            rc = snag_binary_checkpoint_receipt_decode(&record, &receipt);
            if (rc < 0) return -1;
            if (rc == 1) continue;
            if (receipt.boundary.end > before.end ||
                receipt.boundary.next_seq > before.next_seq ||
                receipt.boundary.turns > before.turns) {
                return invalid();
            }
            for (size_t i = 0u; i < 2u; ++i) {
                unsigned bit = 1u << i;
                if (((unseen | found) & bit) &&
                    !memcmp(receipt.image_digest, hashes[i], 32u)) {
                    /* Forward iteration within this backward-read batch keeps
                     * its last matching record, not the highest generation. */
                    candidates[i] = receipt;
                    unseen &= ~bit;
                    found |= bit;
                }
            }
        }
        if (rc < 0) return -1;
        for (size_t i = 0u; i < 2u; ++i) {
            unsigned bit = 1u << i;
            const struct snag_binary_anchor *target = &candidates[i].boundary;
            if ((found & bit) && target->end >= floor) pending |= bit;
            if (!(pending & bit)) continue;
            if (before.end < target->end) return invalid();
            if (before.end == target->end) {
                if (!anchors_equal(&before, target)) return invalid();
                pending &= ~bit;
                verified |= bit;
            }
        }
        cursor = before;
    }
    for (size_t i = 0u; i < 2u; ++i) {
        if (verified & (1u << i)) out[i] = candidates[i];
    }
    return (int)verified;
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
