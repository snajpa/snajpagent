/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_event.h"
#include "store.h"
#include "media.h"

#include <errno.h>
#include <string.h>

struct fields {
    const unsigned char *data;
    size_t size, offset;
    bool references;
};

static int
invalid(void)
{
    errno = EINVAL;
    return -1;
}

static int
write_uint(struct snag_buf *out, uint64_t value, size_t width)
{
    unsigned char bytes[8];
    for (size_t i = 0; i < width; ++i) {
        bytes[i] = (unsigned char)value;
        value >>= 8u;
    }
    return snag_buf_append(out, bytes, width);
}

static bool
read_uint(struct fields *fields, size_t width, uint64_t *value)
{
    if (width > fields->size - fields->offset) return false;
    uint64_t decoded = 0;
    for (size_t i = 0; i < width; ++i)
        decoded |= (uint64_t)fields->data[fields->offset + i] << (8u * i);
    fields->offset += width;
    *value = decoded;
    return true;
}

static bool
read_bytes(struct fields *fields, unsigned char *out, size_t size)
{
    if (size > fields->size - fields->offset) return false;
    memcpy(out, fields->data + fields->offset, size);
    fields->offset += size;
    return true;
}

static bool
read_id(struct fields *fields, unsigned char out[16])
{
    return read_bytes(fields, out, 16u);
}

static bool
text_valid(struct snag_binary_text text, size_t minimum, size_t maximum)
{
    return text.size >= minimum && text.size <= maximum && (!text.size ||
        (text.data && snag_utf8_valid(text.data, text.size, true)));
}

static int
write_text(struct snag_buf *out, struct snag_binary_text text, size_t minimum, size_t maximum)
{
    if (!text_valid(text, minimum, maximum)) return invalid();
    if (write_uint(out, text.size, 4u) < 0) return -1;
    return snag_buf_append(out, text.data, text.size);
}

static bool
read_text(struct fields *fields, struct snag_binary_text *text, size_t minimum, size_t maximum)
{
    uint64_t length;
    if (!read_uint(fields, 4u, &length) || length > fields->size - fields->offset)
        return false;
    struct snag_binary_text decoded = {fields->data + fields->offset, (size_t)length};
    if (!text_valid(decoded, minimum, maximum)) return false;
    fields->offset += (size_t)length;
    *text = decoded;
    return true;
}

static int
write_asset(struct snag_buf *out, const struct snag_binary_asset *asset)
{
    if (!asset->bytes || asset->bytes > SNAG_MEDIA_FILE_MAX) return invalid();
    if (snag_buf_append(out, asset->id, 16u) < 0 ||
        snag_buf_append(out, asset->sha256, 32u) < 0 ||
        write_uint(out, asset->bytes, 8u) < 0) return -1;
    return write_text(out, asset->mime, 1u, 96u);
}

static bool
read_asset(struct fields *fields, struct snag_binary_asset *asset)
{
    return read_id(fields, asset->id) && read_bytes(fields, asset->sha256, 32u) &&
        read_uint(fields, 8u, &asset->bytes) && asset->bytes &&
        asset->bytes <= SNAG_MEDIA_FILE_MAX && read_text(fields, &asset->mime, 1u, 96u);
}

static int
write_part(struct snag_buf *out, const struct snag_binary_part *part)
{
    if (part->kind < SNAG_BINARY_PART_TEXT || part->kind > SNAG_BINARY_PART_IMAGE)
        return invalid();
    if (write_uint(out, part->kind, 1u) < 0) return -1;
    if (part->kind == SNAG_BINARY_PART_TEXT)
        return write_text(out, part->text, 0u, SNAG_MAX_EVENT_LINE);
    if (write_asset(out, &part->asset) < 0) return -1;
    if (part->kind == SNAG_BINARY_PART_IMAGE) {
        if (write_uint(out, part->has_source, 1u) < 0) return -1;
        if (part->has_source && (write_asset(out, &part->source) < 0 ||
            write_text(out, part->text, 0u, 1024u) < 0)) return -1;
    }
    return 0;
}

static bool
read_part(struct fields *fields, struct snag_binary_part *part)
{
    uint64_t value;
    if (!read_uint(fields, 1u, &value) || value < SNAG_BINARY_PART_TEXT ||
        value > SNAG_BINARY_PART_IMAGE) return false;
    part->kind = (enum snag_binary_part_kind)value;
    if (part->kind == SNAG_BINARY_PART_TEXT)
        return read_text(fields, &part->text, 0u, SNAG_MAX_EVENT_LINE);
    if (!read_asset(fields, &part->asset)) return false;
    if (part->kind == SNAG_BINARY_PART_IMAGE) {
        if (!read_uint(fields, 1u, &value) || value > 1u) return false;
        part->has_source = value != 0;
        if (part->has_source && (!read_asset(fields, &part->source) ||
            !read_text(fields, &part->text, 0u, 1024u))) return false;
    }
    return true;
}

static bool
read_content(struct fields *fields, struct snag_binary_content *out)
{
    size_t start = fields->offset;
    uint64_t count;
    if (!read_uint(fields, 4u, &count) || !count ||
        count > (fields->size - fields->offset) / 5u) return false;
    for (uint64_t i = 0; i < count; ++i) {
        struct snag_binary_part part = {0};
        if (!read_part(fields, &part)) return false;
    }
    *out = (struct snag_binary_content){fields->data + start, fields->offset - start};
    return true;
}

int
snag_binary_content_encode(struct snag_buf *out, const struct snag_binary_part *parts, size_t count)
{
    struct snag_buf encoded = {.max = SNAG_MAX_EVENT_LINE};
    int rc = -1;
    if (!out || !parts || !count || count > (SNAG_MAX_EVENT_LINE - 4u) / 5u) return invalid();
    if (write_uint(&encoded, count, 4u) < 0) goto done;
    for (size_t i = 0; i < count; ++i)
        if (write_part(&encoded, &parts[i]) < 0) goto done;
    rc = snag_buf_append(out, encoded.data, encoded.len);
 done:
    snag_buf_free(&encoded);
    return rc;
}

int
snag_binary_content_decode(const void *data, size_t size, struct snag_binary_content *out)
{
    struct fields fields = {.data = data, .size = size};
    struct snag_binary_content decoded;
    if (!data || !out || size > SNAG_MAX_EVENT_LINE || !read_content(&fields, &decoded) ||
        fields.offset != size) return invalid();
    *out = decoded;
    return 0;
}

int
snag_binary_content_next(const struct snag_binary_content *content, size_t *offset,
    struct snag_binary_part *out)
{
    if (!content || !content->data || content->size < 4u || content->size > SNAG_MAX_EVENT_LINE ||
        !offset || !out || (*offset && *offset < 4u) || *offset > content->size) return invalid();
    struct fields fields = {
        .data = content->data, .size = content->size, .offset = *offset ? *offset : 4u
    };
    struct snag_binary_part decoded = {0};
    if (fields.offset == fields.size) return 1;
    if (!read_part(&fields, &decoded)) return invalid();
    *out = decoded;
    *offset = fields.offset;
    return 0;
}

static int
write_instruction(struct snag_buf *out, const struct snag_binary_instruction *instruction)
{
    if (write_uint(out, instruction->has_snapshot, 1u) < 0 ||
        write_text(out, instruction->path, 1u, SNAG_PATH_MAX_BYTES) < 0) return -1;
    if (instruction->has_snapshot && (write_uint(out, instruction->bytes, 8u) < 0 ||
        snag_buf_append(out, instruction->sha256, 32u) < 0)) return -1;
    return 0;
}

static bool
read_instruction(struct fields *fields, struct snag_binary_instruction *instruction)
{
    uint64_t value;
    if (!read_uint(fields, 1u, &value) || value > 1u ||
        !read_text(fields, &instruction->path, 1u, SNAG_PATH_MAX_BYTES)) return false;
    instruction->has_snapshot = value != 0;
    return !instruction->has_snapshot || (read_uint(fields, 8u, &instruction->bytes) &&
        read_bytes(fields, instruction->sha256, 32u));
}

static bool
read_instructions(struct fields *fields, struct snag_binary_instructions *out, bool snapshots)
{
    size_t start = fields->offset;
    uint64_t count;
    if (!read_uint(fields, 4u, &count) || count > (fields->size - fields->offset) / 6u)
        return false;
    for (uint64_t i = 0; i < count; ++i) {
        struct snag_binary_instruction instruction = {0};
        if (!read_instruction(fields, &instruction) ||
            (!snapshots && instruction.has_snapshot)) return false;
    }
    *out = (struct snag_binary_instructions){fields->data + start, fields->offset - start};
    return true;
}

int
snag_binary_instructions_encode(struct snag_buf *out,
    const struct snag_binary_instruction *instructions, size_t count)
{
    struct snag_buf encoded = {.max = SNAG_MAX_EVENT_LINE};
    int rc = -1;
    if (!out || (!instructions && count) || count > (SNAG_MAX_EVENT_LINE - 4u) / 6u)
        return invalid();
    if (write_uint(&encoded, count, 4u) < 0) goto done;
    for (size_t i = 0; i < count; ++i)
        if (write_instruction(&encoded, &instructions[i]) < 0) goto done;
    rc = snag_buf_append(out, encoded.data, encoded.len);
 done:
    snag_buf_free(&encoded);
    return rc;
}

int
snag_binary_instructions_decode(const void *data, size_t size, struct snag_binary_instructions *out)
{
    struct fields fields = {.data = data, .size = size};
    struct snag_binary_instructions decoded;
    if (!data || !out || size > SNAG_MAX_EVENT_LINE ||
        !read_instructions(&fields, &decoded, true) || fields.offset != size) return invalid();
    *out = decoded;
    return 0;
}

int
snag_binary_instructions_next(const struct snag_binary_instructions *instructions,
    size_t *offset, struct snag_binary_instruction *out)
{
    if (!instructions || !instructions->data || instructions->size < 4u ||
        instructions->size > SNAG_MAX_EVENT_LINE || !offset || !out ||
        (*offset && *offset < 4u) || *offset > instructions->size) return invalid();
    struct fields fields = {
        .data = instructions->data, .size = instructions->size, .offset = *offset ? *offset : 4u
    };
    struct snag_binary_instruction decoded = {0};
    if (fields.offset == fields.size) return 1;
    if (!read_instruction(&fields, &decoded)) return invalid();
    *out = decoded;
    *offset = fields.offset;
    return 0;
}

static int
write_content(struct snag_buf *out, struct snag_binary_content content)
{
    struct snag_binary_content decoded;
    if (snag_binary_content_decode(content.data, content.size, &decoded) < 0) return -1;
    return snag_buf_append(out, content.data, content.size);
}

static int
write_instructions(struct snag_buf *out, struct snag_binary_instructions instructions,
    bool snapshots)
{
    struct fields fields = {.data = instructions.data, .size = instructions.size};
    struct snag_binary_instructions decoded;
    if (!instructions.data || instructions.size > SNAG_MAX_EVENT_LINE ||
        !read_instructions(&fields, &decoded, snapshots) || fields.offset != fields.size)
        return invalid();
    return snag_buf_append(out, instructions.data, instructions.size);
}

static int
write_ids(struct snag_buf *out, struct snag_binary_ids ids)
{
    if ((!ids.values && ids.count) || ids.count > (SNAG_MAX_EVENT_LINE - 4u) / 16u)
        return invalid();
    if (write_uint(out, ids.count, 4u) < 0) return -1;
    return snag_buf_append(out, ids.values, ids.count * 16u);
}

static bool
read_ids(struct fields *fields, struct snag_binary_ids *ids)
{
    uint64_t count;
    if (!read_uint(fields, 4u, &count) || count > (fields->size - fields->offset) / 16u)
        return false;
    ids->values = (const unsigned char (*)[16])(fields->data + fields->offset);
    ids->count = (size_t)count;
    fields->offset += (size_t)count * 16u;
    return true;
}

static int
write_selection(struct snag_buf *out, const struct snag_binary_selection *selection)
{
    if (write_text(out, selection->provider, 1u, SNAG_CONFIG_PROVIDER_NAME_MAX) < 0 ||
        write_text(out, selection->model, 1u, SNAG_MODEL_MAX_BYTES - 1u) < 0 ||
        write_text(out, selection->effort, 1u, SNAG_EFFORT_MAX_BYTES - 1u) < 0) return -1;
    return 0;
}

static bool
read_selection(struct fields *fields, struct snag_binary_selection *selection)
{
    return read_text(fields, &selection->provider, 1u, SNAG_CONFIG_PROVIDER_NAME_MAX) &&
        read_text(fields, &selection->model, 1u, SNAG_MODEL_MAX_BYTES - 1u) &&
        read_text(fields, &selection->effort, 1u, SNAG_EFFORT_MAX_BYTES - 1u);
}

static bool
input_reference_set(const struct snag_binary_input_reference *reference)
{
    return reference->field || reference->target.sequence || reference->target.offset ||
        reference->target.size;
}

static bool
input_text_leaf(enum snag_binary_input_leaf field)
{
    return field == SNAG_BINARY_INPUT_TEXT || field == SNAG_BINARY_INPUT_VOICE_TRANSCRIPT ||
        field == SNAG_BINARY_INPUT_VOICE_REQUEST;
}

static bool
input_reference_shape(const struct snag_binary_input_reference *reference,
    enum snag_binary_input_leaf field, size_t minimum, size_t maximum)
{
    return (reference->field == field ||
        (input_text_leaf(reference->field) && input_text_leaf(field))) &&
        reference->target.size >= minimum && reference->target.size <= maximum;
}

static int
write_input_reference(struct snag_buf *out, const struct snag_binary_input_reference *reference,
    enum snag_binary_input_leaf field, size_t minimum, size_t maximum)
{
    unsigned char encoded[SNAG_BINARY_REF_SIZE];
    if (!input_reference_shape(reference, field, minimum, maximum) ||
        snag_binary_ref_encode(encoded, &reference->target) < 0) return invalid();
    if (write_uint(out, UINT32_MAX, 4u) < 0 || write_uint(out, reference->field, 1u) < 0)
        return -1;
    return snag_buf_append(out, encoded, sizeof(encoded));
}

/* The marker is outside every valid literal length/count. Version 1 retains
 * its literal parser and therefore rejects it before exposing a field view. */
static int
read_input_reference(struct fields *fields, struct snag_binary_input_reference *out,
    enum snag_binary_input_leaf field, size_t minimum, size_t maximum)
{
    if (!fields->references) return 0;
    size_t start = fields->offset;
    uint64_t value;
    if (!read_uint(fields, 4u, &value)) return -1;
    if (value != UINT32_MAX) {
        fields->offset = start;
        return 0;
    }
    struct snag_binary_input_reference reference = {0};
    unsigned char encoded[SNAG_BINARY_REF_SIZE];
    if (!read_uint(fields, 1u, &value) ||
        !read_bytes(fields, encoded, sizeof(encoded))) return -1;
    reference.field = (enum snag_binary_input_leaf)value;
    if (snag_binary_ref_decode(encoded, sizeof(encoded), &reference.target) < 0 ||
        !input_reference_shape(&reference, field, minimum, maximum)) return -1;
    *out = reference;
    return 1;
}

static int
write_input_text(struct snag_buf *out, struct snag_binary_text text,
    const struct snag_binary_input_reference *reference, enum snag_binary_input_leaf field,
    size_t maximum)
{
    if (!input_reference_set(reference)) return write_text(out, text, 1u, maximum);
    if (text.data || text.size) return invalid();
    return write_input_reference(out, reference, field, 1u, maximum);
}

static bool
read_input_text(struct fields *fields, struct snag_binary_text *text,
    struct snag_binary_input_reference *reference, enum snag_binary_input_leaf field,
    size_t maximum)
{
    int rc = read_input_reference(fields, reference, field, 1u, maximum);
    return rc > 0 || (rc == 0 && read_text(fields, text, 1u, maximum));
}

static int
write_input_content(struct snag_buf *out, struct snag_binary_content content,
    const struct snag_binary_input_reference *reference)
{
    if (!input_reference_set(reference)) return write_content(out, content);
    if (content.data || content.size) return invalid();
    return write_input_reference(out, reference, SNAG_BINARY_INPUT_CONTENT, 9u,
        SNAG_MAX_EVENT_LINE);
}

static bool
read_input_content(struct fields *fields, struct snag_binary_content *content,
    struct snag_binary_input_reference *reference)
{
    int rc = read_input_reference(fields, reference, SNAG_BINARY_INPUT_CONTENT, 9u,
        SNAG_MAX_EVENT_LINE);
    return rc > 0 || (rc == 0 && read_content(fields, content));
}

static int
write_input_instructions(struct snag_buf *out, struct snag_binary_instructions instructions,
    const struct snag_binary_input_reference *reference, bool snapshots)
{
    if (!input_reference_set(reference)) {
        return write_instructions(out, instructions, snapshots);
    }
    if (instructions.data || instructions.size) return invalid();
    return write_input_reference(out, reference, SNAG_BINARY_INPUT_INSTRUCTIONS, 4u,
        SNAG_MAX_EVENT_LINE);
}

static bool
read_input_instructions(struct fields *fields, struct snag_binary_instructions *instructions,
    struct snag_binary_input_reference *reference, bool snapshots)
{
    int rc = read_input_reference(fields, reference, SNAG_BINARY_INPUT_INSTRUCTIONS, 4u,
        SNAG_MAX_EVENT_LINE);
    return rc > 0 || (rc == 0 && read_instructions(fields, instructions, snapshots));
}

static int
write_turn_config(struct snag_buf *out, const struct snag_binary_turn_config *config)
{
    if (config->present & 0x8000u) return invalid();
    if (write_selection(out, &config->selection) < 0 ||
        write_uint(out, config->present, 2u) < 0) {
        return -1;
    }
    for (size_t i = 0; i < SNAG_BINARY_TURN_NUMBER_COUNT; ++i) {
        if ((config->present & (1u << i)) && write_uint(out, config->numbers[i], 8u) < 0) {
            return -1;
        }
    }
    if ((config->present & SNAG_BINARY_TURN_CAPABILITY) &&
        write_text(out, config->capability, 0u, SNAG_MAX_EVENT_LINE) < 0) {
        return -1;
    }
    if ((config->present & SNAG_BINARY_TURN_PROFILE) &&
        write_text(out, config->profile, 0u, SNAG_MAX_EVENT_LINE) < 0) {
        return -1;
    }
    if (config->present & SNAG_BINARY_TURN_MAX_OUTPUT) {
        if (write_uint(out, config->max_output_null ? 0u : 1u, 1u) < 0 ||
            (!config->max_output_null && write_uint(out, config->max_output_tokens, 8u) < 0)) {
            return -1;
        }
    }
    return config->present & SNAG_BINARY_TURN_PARALLEL_CALLS ?
        write_uint(out, config->parallel_calls, 1u) : 0;
}

static bool
read_turn_config(struct fields *fields, struct snag_binary_turn_config *config)
{
    uint64_t value;
    if (!read_selection(fields, &config->selection) || !read_uint(fields, 2u, &value) ||
        (value & 0x8000u)) {
        return false;
    }
    config->present = (uint16_t)value;
    for (size_t i = 0; i < SNAG_BINARY_TURN_NUMBER_COUNT; ++i) {
        if ((config->present & (1u << i)) && !read_uint(fields, 8u, &config->numbers[i])) {
            return false;
        }
    }
    if ((config->present & SNAG_BINARY_TURN_CAPABILITY) &&
        !read_text(fields, &config->capability, 0u, SNAG_MAX_EVENT_LINE)) {
        return false;
    }
    if ((config->present & SNAG_BINARY_TURN_PROFILE) &&
        !read_text(fields, &config->profile, 0u, SNAG_MAX_EVENT_LINE)) {
        return false;
    }
    if (config->present & SNAG_BINARY_TURN_MAX_OUTPUT) {
        if (!read_uint(fields, 1u, &value) || value > 1u) return false;
        config->max_output_null = value == 0u;
        if (value && !read_uint(fields, 8u, &config->max_output_tokens)) {
            return false;
        }
    }
    if (config->present & SNAG_BINARY_TURN_PARALLEL_CALLS) {
        if (!read_uint(fields, 1u, &value) || value > 1u) return false;
        config->parallel_calls = value != 0u;
    }
    return true;
}

static int
encode_turn_start(struct snag_buf *out, const struct snag_binary_turn_start *turn)
{
    if (!turn->number || (unsigned int)turn->origin > SNAG_BINARY_TURN_TIMER ||
        (turn->read_only && !turn->has_read_only)) {
        return invalid();
    }
    bool content = turn->content.data || turn->content.size ||
        input_reference_set(&turn->content_ref);
    uint64_t flags = (turn->has_read_only ? 1u : 0u) | (turn->read_only ? 2u : 0u) |
        (turn->has_received_ms ? 4u : 0u) | (content ? 8u : 0u) | (turn->workspace ? 16u : 0u);
    if (snag_buf_append(out, turn->id, 16u) < 0 || write_uint(out, turn->number, 8u) < 0 ||
        write_uint(out, turn->origin, 1u) < 0 || write_uint(out, flags, 1u) < 0 ||
        (turn->has_received_ms && write_uint(out, turn->received_ms, 8u) < 0)) {
        return -1;
    }
    if (turn->origin == SNAG_BINARY_TURN_QUEUED) {
        if (!turn->queue_seq || turn->queue_seq == UINT64_MAX) return invalid();
        if (snag_buf_append(out, turn->queue_id, 16u) < 0 ||
            write_uint(out, turn->queue_seq, 8u) < 0) {
            return -1;
        }
    }
    size_t maximum = turn->origin == SNAG_BINARY_TURN_QUEUED ?
        SNAG_MAX_QUEUED_TEXT : SNAG_MAX_DIRECT_PROMPT;
    if (write_text(out, turn->cwd, 1u, SNAG_PATH_MAX_BYTES) < 0 ||
        write_turn_config(out, &turn->config) < 0 ||
        write_input_instructions(out, turn->instructions, &turn->instructions_ref, true) < 0 ||
        write_input_text(out, turn->text, &turn->text_ref, SNAG_BINARY_INPUT_TEXT, maximum) < 0) {
        return -1;
    }
    return content ? write_input_content(out, turn->content, &turn->content_ref) : 0;
}

static bool
decode_turn_start(struct fields *fields, struct snag_binary_turn_start *turn)
{
    uint64_t value;
    uint64_t flags;
    if (!read_id(fields, turn->id) || !read_uint(fields, 8u, &turn->number) || !turn->number ||
        !read_uint(fields, 1u, &value) || value > SNAG_BINARY_TURN_TIMER ||
        !read_uint(fields, 1u, &flags) || flags > 31u ||
        ((flags & 2u) && !(flags & 1u))) {
        return false;
    }
    turn->origin = (enum snag_binary_turn_origin)value;
    turn->has_read_only = (flags & 1u) != 0;
    turn->read_only = (flags & 2u) != 0;
    turn->has_received_ms = (flags & 4u) != 0;
    turn->workspace = (flags & 16u) != 0;
    if (turn->has_received_ms && !read_uint(fields, 8u, &turn->received_ms)) {
        return false;
    }
    if (turn->origin == SNAG_BINARY_TURN_QUEUED &&
        (!read_id(fields, turn->queue_id) || !read_uint(fields, 8u, &turn->queue_seq) ||
         !turn->queue_seq || turn->queue_seq == UINT64_MAX)) {
        return false;
    }
    size_t maximum = turn->origin == SNAG_BINARY_TURN_QUEUED ?
        SNAG_MAX_QUEUED_TEXT : SNAG_MAX_DIRECT_PROMPT;
    return read_text(fields, &turn->cwd, 1u, SNAG_PATH_MAX_BYTES) &&
        read_turn_config(fields, &turn->config) &&
        read_input_instructions(fields, &turn->instructions, &turn->instructions_ref, true) &&
        read_input_text(fields, &turn->text, &turn->text_ref, SNAG_BINARY_INPUT_TEXT, maximum) &&
        (!(flags & 8u) || read_input_content(fields, &turn->content, &turn->content_ref));
}

static int
write_voice_source(struct snag_buf *out, const struct snag_binary_voice_source *voice)
{
    if (snag_buf_append(out, voice->connection_id, 16u) < 0 ||
        write_text(out, voice->input_id, 1u, SNAG_MAX_PROVIDER_ID) < 0 ||
        write_text(out, voice->response_id, 1u, SNAG_MAX_PROVIDER_ID) < 0 ||
        write_text(out, voice->call_id, 1u, SNAG_MAX_PROVIDER_ID) < 0 ||
        write_text(out, voice->provider, 1u, SNAG_CONFIG_PROVIDER_NAME_MAX) < 0 ||
        write_text(out, voice->model, 1u, SNAG_MODEL_MAX_BYTES - 1u) < 0 ||
        write_input_text(out, voice->transcript, &voice->transcript_ref,
            SNAG_BINARY_INPUT_VOICE_TRANSCRIPT, SNAG_MAX_QUEUED_TEXT - 1u) < 0 ||
        write_input_text(out, voice->request, &voice->request_ref,
            SNAG_BINARY_INPUT_VOICE_REQUEST, SNAG_MAX_QUEUED_TEXT - 1u) < 0) return -1;
    return 0;
}

static bool
read_voice_source(struct fields *fields, struct snag_binary_voice_source *voice)
{
    return read_id(fields, voice->connection_id) &&
        read_text(fields, &voice->input_id, 1u, SNAG_MAX_PROVIDER_ID) &&
        read_text(fields, &voice->response_id, 1u, SNAG_MAX_PROVIDER_ID) &&
        read_text(fields, &voice->call_id, 1u, SNAG_MAX_PROVIDER_ID) &&
        read_text(fields, &voice->provider, 1u, SNAG_CONFIG_PROVIDER_NAME_MAX) &&
        read_text(fields, &voice->model, 1u, SNAG_MODEL_MAX_BYTES - 1u) &&
        read_input_text(fields, &voice->transcript, &voice->transcript_ref,
            SNAG_BINARY_INPUT_VOICE_TRANSCRIPT, SNAG_MAX_QUEUED_TEXT - 1u) &&
        read_input_text(fields, &voice->request, &voice->request_ref,
            SNAG_BINARY_INPUT_VOICE_REQUEST, SNAG_MAX_QUEUED_TEXT - 1u);
}

static bool
queue_flags_valid(uint64_t flags, bool adding)
{
    /* readonly, armed-present, armed-value, receipt-present, content, voice. */
    return flags <= 63u && (!(flags & 4u) || (flags & 2u)) &&
        (!(flags & 32u) || (adding && !(flags & 17u)));
}

static bool
while_valid(enum snag_binary_while_turn kind, bool voice)
{
    return kind == SNAG_BINARY_WHILE_ID ||
        kind == (voice ? SNAG_BINARY_WHILE_NULL : SNAG_BINARY_WHILE_EMPTY);
}

static int
encode_queued(struct snag_buf *out, const struct snag_binary_event *event)
{
    bool adding = event->kind == SNAG_BINARY_FUTURE_TURN_QUEUED;
    bool content = event->data.queued.content.data || event->data.queued.content.size ||
        input_reference_set(&event->data.queued.content_ref);
    uint64_t flags = (event->data.queued.read_only ? 1u : 0u) |
        (event->data.queued.has_armed ? 2u : 0u) |
        (event->data.queued.has_armed && event->data.queued.armed ? 4u : 0u) |
        (event->data.queued.has_received_ms ? 8u : 0u) | (content ? 16u : 0u) |
        (event->data.queued.has_voice ? 32u : 0u);
    if (!queue_flags_valid(flags, adding) ||
        (adding && !while_valid(event->data.queued.while_kind, event->data.queued.has_voice)))
        return invalid();
    if (snag_buf_append(out, event->data.queued.id, 16u) < 0 ||
        write_uint(out, flags, 1u) < 0) return -1;
    if (adding) {
        if (write_uint(out, event->data.queued.while_kind, 1u) < 0) return -1;
        if (event->data.queued.while_kind == SNAG_BINARY_WHILE_ID &&
            snag_buf_append(out, event->data.queued.while_id, 16u) < 0) return -1;
    }
    if (event->data.queued.has_received_ms &&
        write_uint(out, event->data.queued.received_ms, 8u) < 0) return -1;
    if (write_input_text(out, event->data.queued.text, &event->data.queued.text_ref,
            SNAG_BINARY_INPUT_TEXT, SNAG_MAX_QUEUED_TEXT) < 0 ||
        (content && write_input_content(out, event->data.queued.content,
            &event->data.queued.content_ref) < 0)) return -1;
    return event->data.queued.has_voice ? write_voice_source(out, &event->data.queued.voice) : 0;
}

static bool
decode_queued(struct fields *fields, struct snag_binary_event *event)
{
    bool adding = event->kind == SNAG_BINARY_FUTURE_TURN_QUEUED;
    uint64_t flags, value;
    if (!read_id(fields, event->data.queued.id) || !read_uint(fields, 1u, &flags) ||
        !queue_flags_valid(flags, adding)) return false;
    event->data.queued.read_only = (flags & 1u) != 0;
    event->data.queued.has_armed = (flags & 2u) != 0;
    event->data.queued.armed = (flags & 4u) != 0;
    event->data.queued.has_received_ms = (flags & 8u) != 0;
    event->data.queued.has_voice = (flags & 32u) != 0;
    if (adding) {
        if (!read_uint(fields, 1u, &value) ||
            !while_valid((enum snag_binary_while_turn)value, event->data.queued.has_voice))
            return false;
        event->data.queued.while_kind = (enum snag_binary_while_turn)value;
        if (event->data.queued.while_kind == SNAG_BINARY_WHILE_ID &&
            !read_id(fields, event->data.queued.while_id)) return false;
    }
    return (!(flags & 8u) || read_uint(fields, 8u, &event->data.queued.received_ms)) &&
        read_input_text(fields, &event->data.queued.text, &event->data.queued.text_ref,
            SNAG_BINARY_INPUT_TEXT, SNAG_MAX_QUEUED_TEXT) &&
        (!(flags & 16u) || read_input_content(fields, &event->data.queued.content,
            &event->data.queued.content_ref)) &&
        (!(flags & 32u) || read_voice_source(fields, &event->data.queued.voice));
}

static int
encode_input(struct snag_buf *out, const struct snag_binary_event *event)
{
    bool content;
    switch (event->kind) {
    case SNAG_BINARY_FUTURE_TURN_QUEUED:
    case SNAG_BINARY_FUTURE_TURN_EDITED:
        return encode_queued(out, event);
    case SNAG_BINARY_INPUT_RECEIVED:
        if (event->data.input.origin != SNAG_BINARY_INPUT_DEFAULT &&
            event->data.input.origin != SNAG_BINARY_INPUT_TIMER) return invalid();
        content = event->data.input.content.data || event->data.input.content.size ||
            input_reference_set(&event->data.input.content_ref);
        if (write_uint(out, event->data.input.origin, 1u) < 0 ||
            write_uint(out, (event->data.input.read_only ? 1u : 0u) |
                           (content ? 2u : 0u), 1u) < 0 ||
            write_uint(out, event->data.input.received_ms, 8u) < 0 ||
            write_selection(out, &event->data.input.selection) < 0 ||
            write_input_instructions(out, event->data.input.instructions,
                &event->data.input.instructions_ref, false) < 0 ||
            write_input_text(out, event->data.input.text, &event->data.input.text_ref,
                SNAG_BINARY_INPUT_TEXT, SNAG_MAX_DIRECT_PROMPT) < 0) return -1;
        return content ? write_input_content(out, event->data.input.content,
            &event->data.input.content_ref) : 0;
    case SNAG_BINARY_INPUT_CANCELLED:
        return 0;
    case SNAG_BINARY_STEERING_ADDED:
    case SNAG_BINARY_IRC_REPLY_REMINDER:
        content = event->data.steering_input.content.data ||
            event->data.steering_input.content.size ||
            input_reference_set(&event->data.steering_input.content_ref);
        if (snag_buf_append(out, event->data.steering_input.id, 16u) < 0 ||
            snag_buf_append(out, event->data.steering_input.turn, 16u) < 0 ||
            write_uint(out, (event->data.steering_input.has_received_ms ? 1u : 0u) |
                           (content ? 2u : 0u), 1u) < 0) return -1;
        if (event->data.steering_input.has_received_ms &&
            write_uint(out, event->data.steering_input.received_ms, 8u) < 0) return -1;
        if (write_input_text(out, event->data.steering_input.text,
            &event->data.steering_input.text_ref, SNAG_BINARY_INPUT_TEXT,
            SNAG_MAX_STEERING_TEXT) < 0)
            return -1;
        return content ? write_input_content(out, event->data.steering_input.content,
            &event->data.steering_input.content_ref) : 0;
    case SNAG_BINARY_STEERING_DEFERRED:
        return snag_buf_append(out, event->data.turn, 16u);
    case SNAG_BINARY_INPUT_ADMITTED:
        if (!event->data.admission.time_ms) return invalid();
        if (snag_buf_append(out, event->data.admission.turn, 16u) < 0 ||
            write_uint(out, event->data.admission.time_ms, 8u) < 0) return -1;
        return write_ids(out, event->data.admission.ids);
    case SNAG_BINARY_FUTURE_QUEUE_STATE:
        return write_uint(out, event->data.queue_armed, 1u);
    case SNAG_BINARY_FUTURE_TURN_CANCELLED:
        if (event->data.queue_cancel.actor != SNAG_BINARY_USER ||
            !event->data.queue_cancel.ids.count) return invalid();
        if (write_uint(out, event->data.queue_cancel.actor, 1u) < 0) return -1;
        return write_ids(out, event->data.queue_cancel.ids);
    default:
        return invalid();
    }
}

static bool
decode_input(struct fields *fields, struct snag_binary_event *event)
{
    uint64_t value, flags;
    switch (event->kind) {
    case SNAG_BINARY_FUTURE_TURN_QUEUED:
    case SNAG_BINARY_FUTURE_TURN_EDITED:
        return decode_queued(fields, event);
    case SNAG_BINARY_INPUT_RECEIVED:
        if (!read_uint(fields, 1u, &value) || value > SNAG_BINARY_INPUT_TIMER ||
            !read_uint(fields, 1u, &flags) || flags > 3u) return false;
        event->data.input.origin = (enum snag_binary_input_origin)value;
        event->data.input.read_only = (flags & 1u) != 0;
        return read_uint(fields, 8u, &event->data.input.received_ms) &&
            read_selection(fields, &event->data.input.selection) &&
            read_input_instructions(fields, &event->data.input.instructions,
                &event->data.input.instructions_ref, false) &&
            read_input_text(fields, &event->data.input.text, &event->data.input.text_ref,
                SNAG_BINARY_INPUT_TEXT, SNAG_MAX_DIRECT_PROMPT) &&
            (!(flags & 2u) || read_input_content(fields, &event->data.input.content,
                &event->data.input.content_ref));
    case SNAG_BINARY_INPUT_CANCELLED:
        return true;
    case SNAG_BINARY_STEERING_ADDED:
    case SNAG_BINARY_IRC_REPLY_REMINDER:
        if (!read_id(fields, event->data.steering_input.id) ||
            !read_id(fields, event->data.steering_input.turn) ||
            !read_uint(fields, 1u, &flags) || flags > 3u) return false;
        event->data.steering_input.has_received_ms = (flags & 1u) != 0;
        return (!(flags & 1u) || read_uint(fields, 8u, &event->data.steering_input.received_ms)) &&
            read_input_text(fields, &event->data.steering_input.text,
                &event->data.steering_input.text_ref, SNAG_BINARY_INPUT_TEXT,
                SNAG_MAX_STEERING_TEXT) &&
            (!(flags & 2u) || read_input_content(fields, &event->data.steering_input.content,
                &event->data.steering_input.content_ref));
    case SNAG_BINARY_STEERING_DEFERRED:
        return read_id(fields, event->data.turn);
    case SNAG_BINARY_INPUT_ADMITTED:
        return read_id(fields, event->data.admission.turn) &&
            read_uint(fields, 8u, &event->data.admission.time_ms) &&
            event->data.admission.time_ms && read_ids(fields, &event->data.admission.ids);
    case SNAG_BINARY_FUTURE_QUEUE_STATE:
        if (!read_uint(fields, 1u, &value) || value > 1u) return false;
        event->data.queue_armed = value != 0;
        return true;
    case SNAG_BINARY_FUTURE_TURN_CANCELLED:
        if (!read_uint(fields, 1u, &value) || value != SNAG_BINARY_USER) return false;
        event->data.queue_cancel.actor = (enum snag_binary_actor)value;
        return read_ids(fields, &event->data.queue_cancel.ids) &&
            event->data.queue_cancel.ids.count;
    default:
        return false;
    }
}

static bool
choice_valid(struct snag_binary_context_choice choice)
{
    switch (choice.mode) {
    case SNAG_BINARY_CONTEXT_DEFAULT:
    case SNAG_BINARY_CONTEXT_MAX:
        return choice.tokens == 0u;
    case SNAG_BINARY_CONTEXT_TOKENS:
        return choice.tokens && choice.tokens <= SNAG_CONFIG_TOKEN_LIMIT_MAX;
    }
    return false;
}

static int
write_choice(struct snag_buf *out, struct snag_binary_context_choice choice)
{
    if (!choice_valid(choice)) return invalid();
    if (write_uint(out, choice.mode, 1u) < 0) return -1;
    return write_uint(out, choice.tokens, 8u);
}

static bool
read_choice(struct fields *fields, struct snag_binary_context_choice *choice)
{
    uint64_t mode, tokens;
    if (!read_uint(fields, 1u, &mode) || !read_uint(fields, 8u, &tokens)) return false;
    struct snag_binary_context_choice decoded = {(enum snag_binary_context)mode, tokens};
    if (!choice_valid(decoded)) return false;
    *choice = decoded;
    return true;
}

static int
encode_metadata(struct snag_buf *out, const struct snag_binary_event *event)
{
    switch (event->kind) {
    case SNAG_BINARY_SESSION_CREATED:
        if (event->data.created.source_format < 2u || event->data.created.source_format > 4u ||
            event->data.created.protocol != SNAG_BINARY_RESPONSES) return invalid();
        if (write_uint(out, event->data.created.source_format, 2u) < 0 ||
            write_uint(out, event->data.created.protocol, 1u) < 0 ||
            write_selection(out, &event->data.created.selection) < 0) return -1;
        return write_text(out, event->data.created.cwd, 1u, SNAG_PATH_MAX_BYTES);
    case SNAG_BINARY_CWD_CHANGED:
        if (write_text(out, event->data.cwd.before, 1u, SNAG_PATH_MAX_BYTES) < 0) return -1;
        return write_text(out, event->data.cwd.after, 1u, SNAG_PATH_MAX_BYTES);
    case SNAG_BINARY_SESSION_ARCHIVED:
    case SNAG_BINARY_SESSION_UNARCHIVED:
        if (event->data.archive.origin != SNAG_BINARY_USER) return invalid();
        return write_uint(out, event->data.archive.origin, 1u);
    case SNAG_BINARY_SESSION_DELETE_REQUESTED:
        if (snag_buf_append(out, event->data.deletion.confirmed_prefix, 4u) < 0 ||
            snag_buf_append(out, event->data.deletion.session, 16u) < 0 ||
            snag_buf_append(out, event->data.deletion.nonce, 16u) < 0) return -1;
        return 0;
    case SNAG_BINARY_BANNER_UPDATED:
        return write_text(out, event->data.banner, 0u, SNAG_BANNER_MAX);
    case SNAG_BINARY_STEERING_UPDATED:
        if (event->data.steering < SNAG_BINARY_STEERING_DEFAULT ||
            event->data.steering > SNAG_BINARY_STEERING_ALL) return invalid();
        return write_uint(out, event->data.steering, 1u);
    case SNAG_BINARY_MODEL_SELECTED:
        if (write_selection(out, &event->data.model.before) < 0) return -1;
        return write_selection(out, &event->data.model.after);
    case SNAG_BINARY_TURN_MODEL_CHANGED:
        if (snag_buf_append(out, event->data.turn_model.id, 16u) < 0 ||
            write_selection(out, &event->data.turn_model.before) < 0) return -1;
        return write_text(out, event->data.turn_model.effort, 1u, SNAG_EFFORT_MAX_BYTES - 1u);
    case SNAG_BINARY_EFFORT_CHANGED:
        if (write_text(out, event->data.effort.before, 1u, SNAG_EFFORT_MAX_BYTES - 1u) < 0)
            return -1;
        return write_text(out, event->data.effort.after, 1u, SNAG_EFFORT_MAX_BYTES - 1u);
    case SNAG_BINARY_CONTEXT_SELECTION_CHANGED:
        if (write_choice(out, event->data.context.before) < 0) return -1;
        return write_choice(out, event->data.context.after);
    case SNAG_BINARY_COMMAND_SHELL_CHANGED:
        return write_text(out, event->data.shell, 1u, SNAG_CONFIG_PATH_MAX);
    default:
        return invalid();
    }
}

static bool
decode_metadata(struct fields *fields, struct snag_binary_event *event)
{
    uint64_t value;
    switch (event->kind) {
    case SNAG_BINARY_SESSION_CREATED:
        if (!read_uint(fields, 2u, &value) || value < 2u || value > 4u) return false;
        event->data.created.source_format = (uint16_t)value;
        if (!read_uint(fields, 1u, &value) || value != SNAG_BINARY_RESPONSES) return false;
        event->data.created.protocol = (enum snag_binary_protocol)value;
        return read_selection(fields, &event->data.created.selection) &&
            read_text(fields, &event->data.created.cwd, 1u, SNAG_PATH_MAX_BYTES);
    case SNAG_BINARY_CWD_CHANGED:
        return read_text(fields, &event->data.cwd.before, 1u, SNAG_PATH_MAX_BYTES) &&
            read_text(fields, &event->data.cwd.after, 1u, SNAG_PATH_MAX_BYTES);
    case SNAG_BINARY_SESSION_ARCHIVED:
    case SNAG_BINARY_SESSION_UNARCHIVED:
        if (!read_uint(fields, 1u, &value) || value != SNAG_BINARY_USER) return false;
        event->data.archive.origin = (enum snag_binary_actor)value;
        return true;
    case SNAG_BINARY_SESSION_DELETE_REQUESTED:
        return read_bytes(fields, event->data.deletion.confirmed_prefix, 4u) &&
            read_id(fields, event->data.deletion.session) &&
            read_id(fields, event->data.deletion.nonce);
    case SNAG_BINARY_BANNER_UPDATED:
        return read_text(fields, &event->data.banner, 0u, SNAG_BANNER_MAX);
    case SNAG_BINARY_STEERING_UPDATED:
        if (!read_uint(fields, 1u, &value) || value > SNAG_BINARY_STEERING_ALL) return false;
        event->data.steering = (enum snag_binary_steering)value;
        return true;
    case SNAG_BINARY_MODEL_SELECTED:
        return read_selection(fields, &event->data.model.before) &&
            read_selection(fields, &event->data.model.after);
    case SNAG_BINARY_TURN_MODEL_CHANGED:
        return read_id(fields, event->data.turn_model.id) &&
            read_selection(fields, &event->data.turn_model.before) &&
            read_text(fields, &event->data.turn_model.effort, 1u, SNAG_EFFORT_MAX_BYTES - 1u);
    case SNAG_BINARY_EFFORT_CHANGED:
        return read_text(fields, &event->data.effort.before, 1u, SNAG_EFFORT_MAX_BYTES - 1u) &&
            read_text(fields, &event->data.effort.after, 1u, SNAG_EFFORT_MAX_BYTES - 1u);
    case SNAG_BINARY_CONTEXT_SELECTION_CHANGED:
        return read_choice(fields, &event->data.context.before) &&
            read_choice(fields, &event->data.context.after);
    case SNAG_BINARY_COMMAND_SHELL_CHANGED:
        return read_text(fields, &event->data.shell, 1u, SNAG_CONFIG_PATH_MAX);
    default:
        return false;
    }
}

static bool
actor_valid(uint64_t actor)
{
    return actor == SNAG_BINARY_USER || actor == SNAG_BINARY_MODEL;
}

static bool
pause_valid(uint64_t pause)
{
    return pause >= SNAG_BINARY_PAUSE_INPUT_CLOSED && pause <= SNAG_BINARY_PAUSE_USER;
}

static bool
timer_kind(enum snag_binary_kind kind)
{
    return kind >= SNAG_BINARY_TIMER_SCHEDULED && kind <= SNAG_BINARY_TIMER_CANCELLED;
}

static bool
goal_kind(enum snag_binary_kind kind)
{
    return kind >= SNAG_BINARY_GOAL_STARTED && kind <= SNAG_BINARY_GOAL_CANCELLED;
}

static int
encode_fields(struct snag_buf *out, const struct snag_binary_event *event)
{
    if (event->kind == SNAG_BINARY_TURN_STARTED) {
        return encode_turn_start(out, &event->data.started);
    }
    if (event->kind >= SNAG_BINARY_INPUT_RECEIVED &&
        event->kind <= SNAG_BINARY_FUTURE_TURN_EDITED) return encode_input(out, event);
    if (event->kind >= SNAG_BINARY_SESSION_CREATED &&
        event->kind <= SNAG_BINARY_COMMAND_SHELL_CHANGED) return encode_metadata(out, event);
    if (timer_kind(event->kind)) {
        if (snag_buf_append(out, event->data.timer.id, 16u) < 0) return -1;
        if (event->kind != SNAG_BINARY_TIMER_SCHEDULED) return 0;
        if (!event->data.timer.due_ms) return invalid();
        if (write_uint(out, event->data.timer.due_ms, 8u) < 0) return -1;
        return write_text(out, event->data.timer.text, 1u, SNAG_MAX_TIMER_TEXT);
    }
    if (!goal_kind(event->kind)) return invalid();
    if (snag_buf_append(out, event->data.goal.id, 16u) < 0) return -1;
    switch (event->kind) {
    case SNAG_BINARY_GOAL_REPLACED:
        if (snag_buf_append(out, event->data.goal.replacement, 16u) < 0) return -1;
        /* fall through */
    case SNAG_BINARY_GOAL_REWORDED:
    case SNAG_BINARY_GOAL_COMPLETED:
        if (!actor_valid(event->data.goal.actor)) return invalid();
        if (write_uint(out, event->data.goal.actor, 1u) < 0) return -1;
        if (event->kind == SNAG_BINARY_GOAL_COMPLETED) return 0;
        /* fall through */
    case SNAG_BINARY_GOAL_STARTED:
        return write_text(out, event->data.goal.text, 1u, SNAG_MAX_GOAL_PROMPT);
    case SNAG_BINARY_GOAL_BLOCKED:
        if (event->data.goal.actor != SNAG_BINARY_MODEL) return invalid();
        if (write_uint(out, event->data.goal.actor, 1u) < 0) return -1;
        return write_text(out, event->data.goal.text, 1u, SNAG_MAX_GOAL_BLOCKER);
    case SNAG_BINARY_GOAL_LOCK_CHANGED:
        return write_uint(out, event->data.goal.locked ? 1u : 0u, 1u);
    case SNAG_BINARY_GOAL_PAUSED:
        if (!pause_valid(event->data.goal.pause)) return invalid();
        return write_uint(out, event->data.goal.pause, 1u);
    case SNAG_BINARY_GOAL_RESUMED:
    case SNAG_BINARY_GOAL_CANCELLED:
        return 0;
    default:
        return invalid();
    }
}

uint16_t
snag_binary_event_version(enum snag_binary_kind kind)
{
    if (kind >= SNAG_BINARY_INPUT_RECEIVED && kind <= SNAG_BINARY_FUTURE_TURN_EDITED) return 2u;
    if ((kind >= SNAG_BINARY_SESSION_CREATED && kind <= SNAG_BINARY_COMMAND_SHELL_CHANGED) ||
        timer_kind(kind) || goal_kind(kind) || kind == SNAG_BINARY_TURN_STARTED) return 1u;
    return 0u;
}

int
snag_binary_event_encode(struct snag_buf *out, const struct snag_binary_event *event)
{
    if (!out || !event) return invalid();
    struct snag_buf payload = {.max = SNAG_MAX_EVENT_LINE};
    int rc = encode_fields(&payload, event);
    if (rc == 0) rc = snag_buf_append(out, payload.data, payload.len);
    snag_buf_free(&payload);
    return rc;
}

static bool
decode_fields(struct fields *fields, struct snag_binary_event *event)
{
    uint64_t value;
    if (event->kind == SNAG_BINARY_TURN_STARTED) {
        return decode_turn_start(fields, &event->data.started);
    }
    if (event->kind >= SNAG_BINARY_INPUT_RECEIVED &&
        event->kind <= SNAG_BINARY_FUTURE_TURN_EDITED) return decode_input(fields, event);
    if (event->kind >= SNAG_BINARY_SESSION_CREATED &&
        event->kind <= SNAG_BINARY_COMMAND_SHELL_CHANGED) return decode_metadata(fields, event);
    if (timer_kind(event->kind)) {
        if (!read_id(fields, event->data.timer.id)) return false;
        if (event->kind != SNAG_BINARY_TIMER_SCHEDULED) return true;
        return read_uint(fields, 8u, &event->data.timer.due_ms) &&
            event->data.timer.due_ms &&
            read_text(fields, &event->data.timer.text, 1u, SNAG_MAX_TIMER_TEXT);
    }
    if (!goal_kind(event->kind) || !read_id(fields, event->data.goal.id)) return false;
    switch (event->kind) {
    case SNAG_BINARY_GOAL_REPLACED:
        if (!read_id(fields, event->data.goal.replacement)) return false;
        /* fall through */
    case SNAG_BINARY_GOAL_REWORDED:
    case SNAG_BINARY_GOAL_COMPLETED:
        if (!read_uint(fields, 1u, &value) || !actor_valid(value)) return false;
        event->data.goal.actor = (enum snag_binary_actor)value;
        if (event->kind == SNAG_BINARY_GOAL_COMPLETED) return true;
        /* fall through */
    case SNAG_BINARY_GOAL_STARTED:
        return read_text(fields, &event->data.goal.text, 1u, SNAG_MAX_GOAL_PROMPT);
    case SNAG_BINARY_GOAL_BLOCKED:
        if (!read_uint(fields, 1u, &value) || value != SNAG_BINARY_MODEL) return false;
        event->data.goal.actor = (enum snag_binary_actor)value;
        return read_text(fields, &event->data.goal.text, 1u, SNAG_MAX_GOAL_BLOCKER);
    case SNAG_BINARY_GOAL_LOCK_CHANGED:
        if (!read_uint(fields, 1u, &value) || value > 1u) return false;
        event->data.goal.locked = value != 0u;
        return true;
    case SNAG_BINARY_GOAL_PAUSED:
        if (!read_uint(fields, 1u, &value) || !pause_valid(value)) return false;
        event->data.goal.pause = (enum snag_binary_pause)value;
        return true;
    case SNAG_BINARY_GOAL_RESUMED:
    case SNAG_BINARY_GOAL_CANCELLED:
        return true;
    default:
        return false;
    }
}

int
snag_binary_event_decode(const struct snag_binary_record *record,
    struct snag_binary_event *out)
{
    if (!record || !out || !record->kind || !record->version ||
        record->size > SNAG_MAX_EVENT_LINE ||
        (record->size && !record->payload)) return invalid();
    if (record->kind >= 0x8000u) {
        if (record->flags == SNAG_BINARY_RECORD_OPTIONAL) return 1;
        return invalid();
    }
    /* An optional bit can never downgrade a semantic record's interpretation. */
    uint16_t version = snag_binary_event_version((enum snag_binary_kind)record->kind);
    if (record->flags || !version || record->version > version) return invalid();
    struct snag_binary_event decoded = {.kind = (enum snag_binary_kind)record->kind};
    struct fields fields = {
        .data = record->payload, .size = record->size,
        .references = record->version >= 2u || record->kind == SNAG_BINARY_TURN_STARTED
    };
    if (!decode_fields(&fields, &decoded) || fields.offset != fields.size) return invalid();
    *out = decoded;
    return 0;
}

static int
input_leaf(const struct snag_binary_record *record, enum snag_binary_input_leaf field,
    struct snag_binary_text *out)
{
    struct snag_binary_event event;
    if (snag_binary_event_decode(record, &event) != 0) return invalid();
    struct snag_binary_text text = {0};
    struct snag_binary_content content = {0};
    struct snag_binary_instructions instructions = {0};
    const struct snag_binary_voice_source *voice = NULL;
    switch (event.kind) {
    case SNAG_BINARY_INPUT_RECEIVED:
        text = event.data.input.text;
        content = event.data.input.content;
        instructions = event.data.input.instructions;
        break;
    case SNAG_BINARY_TURN_STARTED:
        text = event.data.started.text;
        content = event.data.started.content;
        instructions = event.data.started.instructions;
        break;
    case SNAG_BINARY_STEERING_ADDED:
    case SNAG_BINARY_IRC_REPLY_REMINDER:
        text = event.data.steering_input.text;
        content = event.data.steering_input.content;
        break;
    case SNAG_BINARY_FUTURE_TURN_QUEUED:
    case SNAG_BINARY_FUTURE_TURN_EDITED:
        text = event.data.queued.text;
        content = event.data.queued.content;
        if (event.data.queued.has_voice) voice = &event.data.queued.voice;
        break;
    default:
        return invalid();
    }

    struct snag_binary_text leaf = {0};
    switch (field) {
    case SNAG_BINARY_INPUT_TEXT:
        leaf = text;
        break;
    case SNAG_BINARY_INPUT_CONTENT:
        leaf = (struct snag_binary_text){content.data, content.size};
        break;
    case SNAG_BINARY_INPUT_INSTRUCTIONS:
        leaf = (struct snag_binary_text){instructions.data, instructions.size};
        break;
    case SNAG_BINARY_INPUT_VOICE_TRANSCRIPT:
        if (voice) leaf = voice->transcript;
        break;
    case SNAG_BINARY_INPUT_VOICE_REQUEST:
        if (voice) leaf = voice->request;
        break;
    default:
        return invalid();
    }
    if (!leaf.data) return invalid();
    *out = leaf;
    return 0;
}

static int
find_input_leaf(const struct snag_binary_batch *batch, uint64_t sequence,
    enum snag_binary_input_leaf field, struct snag_binary_ref *reference,
    const unsigned char **view)
{
    if (!batch || !sequence || sequence == UINT64_MAX || sequence < batch->first_seq ||
        sequence - batch->first_seq >= batch->count) return invalid();
    size_t cursor = SNAG_BINARY_BATCH_HEADER_SIZE;
    struct snag_binary_record record;
    uint64_t found;
    int rc;
    while ((rc = snag_binary_record_next(batch, &cursor, &record, &found)) == 0) {
        if (found != sequence) continue;
        struct snag_binary_text leaf;
        if (input_leaf(&record, field, &leaf) < 0) return -1;
        size_t offset = (size_t)(leaf.data - record.payload);
        /* The event decoder bounds the complete payload before producing views. */
        if (offset > UINT32_MAX || leaf.size > UINT32_MAX) return invalid();
        *reference = (struct snag_binary_ref){sequence, (uint32_t)offset, (uint32_t)leaf.size};
        *view = leaf.data;
        return 0;
    }
    return rc < 0 ? rc : invalid();
}

int
snag_binary_input_ref_create(const struct snag_binary_batch *batch, uint64_t sequence,
    enum snag_binary_input_leaf field, struct snag_binary_ref *out)
{
    if (!out) return invalid();
    struct snag_binary_ref reference;
    const unsigned char *view;
    if (find_input_leaf(batch, sequence, field, &reference, &view) < 0) return -1;
    *out = reference;
    return 0;
}

int
snag_binary_input_ref_resolve(const struct snag_binary_ref *reference,
    const struct snag_binary_batch *batch, enum snag_binary_input_leaf field,
    const unsigned char **view)
{
    if (!reference || !view) return invalid();
    struct snag_binary_ref canonical;
    const unsigned char *data;
    if (find_input_leaf(batch, reference->sequence, field, &canonical, &data) < 0) return -1;
    if (reference->offset != canonical.offset || reference->size != canonical.size)
        return invalid();
    *view = data;
    return 0;
}
