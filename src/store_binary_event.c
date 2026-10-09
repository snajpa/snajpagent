/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_event.h"
#include "irc.h"
#include "json.h"
#include "media.h"
#include "store.h"
#include "store_binary_index.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
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
goal_wait_valid(struct snag_binary_text text)
{
    if (!text_valid(text, 1u, SNAG_MAX_GOAL_BLOCKER)) return false;
    char *copy = malloc(text.size + 1u);
    if (!copy) return false;
    memcpy(copy, text.data, text.size);
    copy[text.size] = '\0';
    bool valid = snag_goal_wait_valid(copy);
    free(copy);
    return valid;
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

static bool
read_terminated_text(struct fields *fields, struct snag_binary_text *out)
{
    size_t remaining = fields->size - fields->offset;
    size_t limit = remaining < SNAG_PATH_MAX_BYTES + 1u ? remaining : SNAG_PATH_MAX_BYTES + 1u;
    const unsigned char *start = fields->data + fields->offset;
    const unsigned char *end = memchr(start, 0, limit);
    if (!end) return false;
    struct snag_binary_text text = {start, (size_t)(end - start)};
    if (!text_valid(text, 1u, SNAG_PATH_MAX_BYTES)) return false;
    fields->offset += text.size + 1u;
    *out = text;
    return true;
}

static bool
read_options(struct fields *fields, struct snag_binary_options *out)
{
    size_t start = fields->offset;
    uint64_t count;
    if (!read_uint(fields, 4u, &count) || count > (fields->size - fields->offset) / 2u) {
        return false;
    }
    bool argument = false;
    for (uint64_t i = 0u; i < count; ++i) {
        struct snag_binary_text text;
        if (!read_terminated_text(fields, &text)) return false;
        if (argument) {
            argument = false;
            continue;
        }
        int arity = snag_session_option_arity((const char *)text.data);
        if (arity < 0) return false;
        argument = arity != 0;
    }
    if (argument) return false;
    *out = (struct snag_binary_options){fields->data + start, fields->offset - start};
    return true;
}

int
snag_binary_options_encode(struct snag_buf *out, const json_t *args)
{
    if (!out || !snag_session_options_valid(args) || json_array_size(args) > UINT32_MAX) {
        return invalid();
    }
    struct snag_buf staged = {.max = SNAG_MAX_EVENT_LINE};
    int rc = write_uint(&staged, json_array_size(args), 4u);
    for (size_t i = 0u; rc == 0 && i < json_array_size(args); ++i) {
        const json_t *arg = json_array_get(args, i);
        struct snag_binary_text text = {
            (const unsigned char *)json_string_value(arg), json_string_length(arg)};
        /* A terminator fits every legacy argument list within the existing
         * event budget and lets readers apply the shared grammar in place. */
        if (!text_valid(text, 1u, SNAG_PATH_MAX_BYTES)) rc = invalid();
        else if (snag_buf_append(&staged, text.data, text.size) < 0) rc = -1;
        else rc = write_uint(&staged, 0u, 1u);
    }
    if (rc == 0) rc = snag_buf_append(out, staged.data, staged.len);
    snag_buf_free(&staged);
    return rc;
}

int
snag_binary_options_decode(const void *data, size_t size, struct snag_binary_options *out)
{
    struct fields fields = {.data = data, .size = size};
    struct snag_binary_options value;
    if (!data || !out || size > SNAG_MAX_EVENT_LINE || !read_options(&fields, &value) ||
        fields.offset != size) return invalid();
    *out = value;
    return 0;
}

int
snag_binary_options_next(const struct snag_binary_options *options, size_t *offset,
    struct snag_binary_text *out)
{
    if (!options || !options->data || options->size < 4u ||
        options->size > SNAG_MAX_EVENT_LINE || !offset || !out ||
        (*offset && *offset < 4u) || *offset > options->size) return invalid();
    struct fields fields = {.data = options->data, .size = options->size,
        .offset = *offset ? *offset : 4u};
    if (fields.offset == fields.size) return 1;
    struct snag_binary_text value;
    if (!read_terminated_text(&fields, &value)) return invalid();
    *offset = fields.offset;
    *out = value;
    return 0;
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
    if (!text_valid(instruction->path, 1u, SNAG_PATH_MAX_BYTES)) return invalid();
    if (write_uint(out, instruction->has_snapshot, 1u) < 0 ||
        snag_buf_append(out, instruction->path.data, instruction->path.size) < 0 ||
        snag_buf_putc(out, 0) < 0) {
        return -1;
    }
    if (instruction->has_snapshot && (write_uint(out, instruction->bytes, 8u) < 0 ||
        snag_buf_append(out, instruction->sha256, 32u) < 0)) return -1;
    return 0;
}

static bool
read_instruction(struct fields *fields, struct snag_binary_instruction *instruction)
{
    uint64_t value;
    if (!read_uint(fields, 1u, &value) || value > 1u) return false;
    if (!read_terminated_text(fields, &instruction->path)) return false;
    instruction->has_snapshot = value != 0;
    return !instruction->has_snapshot || (read_uint(fields, 8u, &instruction->bytes) &&
        read_bytes(fields, instruction->sha256, 32u));
}

static bool
read_instructions(struct fields *fields, struct snag_binary_instructions *out, bool snapshots)
{
    size_t start = fields->offset;
    uint64_t count;
    if (!read_uint(fields, 4u, &count) || count > (fields->size - fields->offset) / 3u) {
        return false;
    }
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
    if (!out || (!instructions && count) || count > (SNAG_MAX_EVENT_LINE - 4u) / 3u) {
        return invalid();
    }
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

static int write_turn_config(struct snag_buf *, const struct snag_binary_turn_config *);
static bool read_turn_config(struct fields *, struct snag_binary_turn_config *);

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

static bool
session_name_valid(struct snag_binary_text text)
{
    if (!text_valid(text, 1u, SNAG_PATH_MAX_BYTES)) return false;
    char name[SNAG_PATH_MAX_BYTES + 1u];
    memcpy(name, text.data, text.size);
    name[text.size] = '\0';
    return snag_session_name_valid(name);
}

static bool
retry_auto_valid(struct snag_binary_text text)
{
    return text.data && ((text.size == 2u && !memcmp(text.data, "on", 2u)) ||
        (text.size == 3u && !memcmp(text.data, "off", 3u)));
}

static bool
service_tier_valid(struct snag_binary_text text)
{
    return text.data && ((text.size == 8u && !memcmp(text.data, "priority", 8u)) ||
        (text.size == 7u && !memcmp(text.data, "default", 7u)));
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
    case SNAG_BINARY_SESSION_NAMED:
        if (!session_name_valid(event->data.name)) return invalid();
        return write_text(out, event->data.name, 1u, SNAG_PATH_MAX_BYTES);
    case SNAG_BINARY_SERVICE_TIER_CHANGED:
        if (!service_tier_valid(event->data.service_tier)) return invalid();
        return write_text(out, event->data.service_tier, 7u, 8u);
    case SNAG_BINARY_FALLBACK_CHANGED:
        return write_text(out, event->data.fallback, 1u, SNAG_CONFIG_SELECTOR_MAX - 1u);
    case SNAG_BINARY_TURN_FALLBACK_STARTED:
        if (snag_buf_append(out, event->data.turn_fallback.id, 16u) < 0 ||
            write_selection(out, &event->data.turn_fallback.selection) < 0) return -1;
        return write_choice(out, event->data.turn_fallback.context);
    case SNAG_BINARY_RETRY_AUTO_CHANGED:
        if (!retry_auto_valid(event->data.retry_auto)) return invalid();
        return write_text(out, event->data.retry_auto, 2u, 3u);
    case SNAG_BINARY_SESSION_OPTIONS: {
        struct snag_binary_options view;
        if (snag_binary_options_decode(event->data.options.data,
            event->data.options.size, &view) < 0) return -1;
        return snag_buf_append(out, view.data, view.size);
    }
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
    case SNAG_BINARY_SESSION_NAMED:
        return read_text(fields, &event->data.name, 1u, SNAG_PATH_MAX_BYTES) &&
            session_name_valid(event->data.name);
    case SNAG_BINARY_SERVICE_TIER_CHANGED:
        return read_text(fields, &event->data.service_tier, 7u, 8u) &&
            service_tier_valid(event->data.service_tier);
    case SNAG_BINARY_FALLBACK_CHANGED:
        return read_text(fields, &event->data.fallback, 1u, SNAG_CONFIG_SELECTOR_MAX - 1u);
    case SNAG_BINARY_TURN_FALLBACK_STARTED:
        return read_id(fields, event->data.turn_fallback.id) &&
            read_selection(fields, &event->data.turn_fallback.selection) &&
            read_choice(fields, &event->data.turn_fallback.context);
    case SNAG_BINARY_RETRY_AUTO_CHANGED:
        return read_text(fields, &event->data.retry_auto, 2u, 3u) &&
            retry_auto_valid(event->data.retry_auto);
    case SNAG_BINARY_SESSION_OPTIONS:
        return read_options(fields, &event->data.options);
    default:
        return false;
    }
}

static bool
response_host_context_valid(const struct snag_binary_content *content)
{
    struct snag_binary_content decoded;
    if (snag_binary_content_decode(content->data, content->size, &decoded) < 0) return false;
    struct snag_binary_part part;
    size_t offset = 0u, count = 0u;
    int rc;
    while ((rc = snag_binary_content_next(&decoded, &offset, &part)) == 0) {
        if (part.kind != SNAG_BINARY_PART_TEXT || !part.text.size) return false;
        ++count;
    }
    return rc == 1 && count >= 2u;
}

static bool
response_start_valid(const struct snag_binary_response_start *start)
{
    if (!start->cycle || start->count_method < SNAG_BINARY_COUNT_EXACT ||
        start->count_method > SNAG_BINARY_COUNT_QUALIFIED_UPPER_BOUND ||
        (start->count_method == SNAG_BINARY_COUNT_UNKNOWN && start->input_tokens_bound) ||
        start->has_baseline != (start->count_method == SNAG_BINARY_COUNT_ANCHORED_UPPER_BOUND))
        return false;
    if (!start->full_accounting) return !start->source_bound && !start->has_hard_input &&
        !start->has_requested_output && !start->host_context.size;
    return start->capacity_source >= SNAG_BINARY_CAPACITY_UNKNOWN &&
        start->capacity_source <= SNAG_BINARY_CAPACITY_STALE_CATALOG_IGNORED &&
        start->model_input_bytes && start->request_input_bytes &&
        (!start->has_requested_output || (start->requested_output_tokens &&
         start->requested_output_tokens <= SNAG_CONFIG_TOKEN_LIMIT_MAX)) &&
        (!start->host_context.size || response_host_context_valid(&start->host_context));
}

static int
encode_response_start(struct snag_buf *out, const struct snag_binary_response_start *start)
{
    if (!response_start_valid(start)) return invalid();
    unsigned int flags = (start->full_accounting ? 1u : 0u) | (start->has_irc_seq ? 2u : 0u) |
        (start->host_context.size ? 4u : 0u) | (start->has_baseline ? 8u : 0u) |
        (start->has_compact ? 16u : 0u) | (start->source_bound ? 32u : 0u) |
        (start->has_hard_input ? 64u : 0u) | (start->has_requested_output ? 128u : 0u);
    if (write_uint(out, flags, 2u) < 0 || snag_buf_append(out, start->turn, 16u) < 0 ||
        snag_buf_append(out, start->response, 16u) < 0 || write_uint(out, start->cycle, 4u) < 0 ||
        write_uint(out, start->count_method, 1u) < 0 ||
        write_uint(out, start->input_tokens_bound, 8u) < 0 ||
        snag_buf_append(out, start->request_sha256, 32u) < 0 ||
        snag_buf_append(out, start->count_request_sha256, 32u) < 0 ||
        snag_buf_append(out, start->model_input_sha256, 32u) < 0 ||
        write_text(out, start->selection.model, 1u, SNAG_MODEL_MAX_BYTES - 1u) < 0 ||
        write_text(out, start->capability, 1u, SNAG_MAX_EVENT_LINE) < 0 ||
        write_text(out, start->profile, 1u, SNAG_MAX_EVENT_LINE) < 0) return -1;
    if (start->has_baseline && snag_buf_append(out, start->baseline_sha256, 32u) < 0) return -1;
    if (start->has_compact && snag_buf_append(out, start->compact, 16u) < 0) return -1;
    if (write_ids(out, start->steering) < 0) return -1;
    if (start->full_accounting) {
        if (write_text(out, start->selection.provider, 1u, SNAG_CONFIG_PROVIDER_NAME_MAX) < 0 ||
            write_text(out, start->selection.effort, 1u, SNAG_EFFORT_MAX_BYTES - 1u) < 0 ||
            write_uint(out, start->capacity_source, 1u) < 0 ||
            snag_buf_append(out, start->provider_source_sha256, 32u) < 0 ||
            snag_buf_append(out, start->request_input_sha256, 32u) < 0 ||
            write_uint(out, start->model_input_bytes, 8u) < 0 ||
            write_uint(out, start->request_input_bytes, 8u) < 0 ||
            write_uint(out, start->request_input_count, 8u) < 0) return -1;
        if (start->has_hard_input && write_uint(out, start->hard_input_tokens, 8u) < 0) return -1;
        if (start->has_requested_output &&
            write_uint(out, start->requested_output_tokens, 8u) < 0) return -1;
    }
    if (start->has_irc_seq && write_uint(out, start->irc_seq, 8u) < 0) return -1;
    return start->host_context.size ?
        snag_buf_append(out, start->host_context.data, start->host_context.size) : 0;
}

static bool
decode_response_start(struct fields *fields, struct snag_binary_response_start *start)
{
    uint64_t flags, value;
    if (!read_uint(fields, 2u, &flags) || flags > 255u) return false;
    /* Snapshot and capacity fields require the full-accounting shape. */
    if (!(flags & 1u) && (flags & (4u | 32u | 64u | 128u))) return false;
    if (!read_id(fields, start->turn) ||
        !read_id(fields, start->response) || !read_uint(fields, 4u, &value)) return false;
    start->cycle = (uint32_t)value;
    start->full_accounting = (flags & 1u) != 0u;
    start->has_irc_seq = (flags & 2u) != 0u;
    start->has_baseline = (flags & 8u) != 0u;
    start->has_compact = (flags & 16u) != 0u;
    start->source_bound = (flags & 32u) != 0u;
    start->has_hard_input = (flags & 64u) != 0u;
    start->has_requested_output = (flags & 128u) != 0u;
    if (!read_uint(fields, 1u, &value)) return false;
    start->count_method = (enum snag_binary_count_method)value;
    if (!read_uint(fields, 8u, &start->input_tokens_bound) ||
        !read_bytes(fields, start->request_sha256, 32u) ||
        !read_bytes(fields, start->count_request_sha256, 32u) ||
        !read_bytes(fields, start->model_input_sha256, 32u) ||
        !read_text(fields, &start->selection.model, 1u, SNAG_MODEL_MAX_BYTES - 1u) ||
        !read_text(fields, &start->capability, 1u, SNAG_MAX_EVENT_LINE) ||
        !read_text(fields, &start->profile, 1u, SNAG_MAX_EVENT_LINE)) return false;
    if (start->has_baseline && !read_bytes(fields, start->baseline_sha256, 32u)) return false;
    if (start->has_compact && !read_id(fields, start->compact)) return false;
    if (!read_ids(fields, &start->steering)) return false;
    if (start->full_accounting) {
        if (!read_text(fields, &start->selection.provider, 1u, SNAG_CONFIG_PROVIDER_NAME_MAX) ||
            !read_text(fields, &start->selection.effort, 1u, SNAG_EFFORT_MAX_BYTES - 1u) ||
            !read_uint(fields, 1u, &value)) return false;
        start->capacity_source = (enum snag_binary_capacity_source)value;
        if (!read_bytes(fields, start->provider_source_sha256, 32u) ||
            !read_bytes(fields, start->request_input_sha256, 32u) ||
            !read_uint(fields, 8u, &start->model_input_bytes) ||
            !read_uint(fields, 8u, &start->request_input_bytes) ||
            !read_uint(fields, 8u, &start->request_input_count)) return false;
        if (start->has_hard_input && !read_uint(fields, 8u, &start->hard_input_tokens))
            return false;
        if (start->has_requested_output &&
            !read_uint(fields, 8u, &start->requested_output_tokens)) return false;
    }
    if (start->has_irc_seq && !read_uint(fields, 8u, &start->irc_seq)) return false;
    if ((flags & 4u) && !read_content(fields, &start->host_context)) return false;
    return response_start_valid(start);
}

static bool
provider_id_valid(struct snag_binary_text text)
{
    if (!text_valid(text, 1u, SNAG_MAX_PROVIDER_ID)) return false;
    char terminated[SNAG_MAX_PROVIDER_ID + 1u];
    memcpy(terminated, text.data, text.size);
    terminated[text.size] = '\0';
    return snag_provider_id_valid(terminated);
}

static bool
public_metadata_valid(const struct snag_binary_public_item *item)
{
    return (item->kind == SNAG_BINARY_ITEM_ASSISTANT || item->kind == SNAG_BINARY_ITEM_REFUSAL) &&
        (item->phase == SNAG_BINARY_PHASE_COMMENTARY ||
         item->phase == SNAG_BINARY_PHASE_FINAL_ANSWER) &&
        (item->kind != SNAG_BINARY_ITEM_REFUSAL || item->phase == SNAG_BINARY_PHASE_FINAL_ANSWER) &&
        provider_id_valid(item->provider_id);
}

static bool
response_output_valid(const struct snag_binary_response_output *output)
{
    return output->cycle && output->offset <= SNAG_MAX_PUBLIC_ITEM &&
        output->item.text.size <= SNAG_MAX_PUBLIC_ITEM - output->offset &&
        public_metadata_valid(&output->item) &&
        text_valid(output->item.text, 1u, SNAG_MAX_PUBLIC_ITEM);
}

static int
write_public_metadata(struct snag_buf *out, const struct snag_binary_public_item *item)
{
    if (!public_metadata_valid(item)) return invalid();
    if (write_uint(out, item->kind, 1u) < 0 || write_uint(out, item->phase, 1u) < 0 ||
        snag_buf_append(out, item->id, 16u) < 0) {
        return -1;
    }
    return write_text(out, item->provider_id, 1u, SNAG_MAX_PROVIDER_ID);
}

static bool
read_public_metadata(struct fields *fields, struct snag_binary_public_item *item)
{
    uint64_t value;
    if (!read_uint(fields, 1u, &value)) return false;
    item->kind = (enum snag_binary_item_kind)value;
    if (!read_uint(fields, 1u, &value)) return false;
    item->phase = (enum snag_binary_item_phase)value;
    return read_id(fields, item->id) &&
        read_text(fields, &item->provider_id, 1u, SNAG_MAX_PROVIDER_ID) &&
        public_metadata_valid(item);
}

static int
encode_response_output(struct snag_buf *out, const struct snag_binary_response_output *output)
{
    if (!response_output_valid(output)) return invalid();
    const struct snag_binary_public_item *item = &output->item;
    if (snag_buf_append(out, output->turn, 16u) < 0 ||
        snag_buf_append(out, output->response, 16u) < 0 ||
        write_uint(out, output->cycle, 4u) < 0 || write_uint(out, output->index, 8u) < 0 ||
        write_uint(out, output->offset, 8u) < 0 || write_public_metadata(out, item) < 0) {
        return -1;
    }
    return write_text(out, item->text, 1u, SNAG_MAX_PUBLIC_ITEM);
}

static bool
decode_response_output(struct fields *fields, struct snag_binary_response_output *output)
{
    uint64_t value;
    if (!read_id(fields, output->turn) || !read_id(fields, output->response) ||
        !read_uint(fields, 4u, &value)) return false;
    output->cycle = (uint32_t)value;
    return read_uint(fields, 8u, &output->index) && read_uint(fields, 8u, &output->offset) &&
        read_public_metadata(fields, &output->item) &&
        read_text(fields, &output->item.text, 1u, SNAG_MAX_PUBLIC_ITEM) &&
        response_output_valid(output);
}

static bool
output_span_set(const struct snag_binary_output_span *span)
{
    return span->first.sequence || span->first.offset || span->first.size ||
        span->last_sequence || span->bytes;
}

static int
write_public_value(struct snag_buf *out, const struct snag_binary_public_value *value)
{
    if (write_public_metadata(out, &value->item) < 0) return -1;
    if (!output_span_set(&value->source)) {
        return write_text(out, value->item.text, 1u, SNAG_MAX_PUBLIC_ITEM);
    }
    if (value->item.text.data || value->item.text.size) return invalid();
    unsigned char encoded[SNAG_BINARY_OUTPUT_SPAN_SIZE];
    if (snag_binary_output_span_encode(encoded, &value->source) < 0 ||
        write_uint(out, UINT32_MAX, 4u) < 0) {
        return -1;
    }
    return snag_buf_append(out, encoded, sizeof(encoded));
}

static bool
read_public_value(struct fields *fields, struct snag_binary_public_value *value)
{
    if (!read_public_metadata(fields, &value->item)) return false;
    size_t start = fields->offset;
    uint64_t length;
    if (!read_uint(fields, 4u, &length)) return false;
    if (length != UINT32_MAX) {
        fields->offset = start;
        return read_text(fields, &value->item.text, 1u, SNAG_MAX_PUBLIC_ITEM);
    }
    if (fields->size - fields->offset < SNAG_BINARY_OUTPUT_SPAN_SIZE ||
        snag_binary_output_span_decode(fields->data + fields->offset,
            SNAG_BINARY_OUTPUT_SPAN_SIZE, &value->source) < 0) {
        return false;
    }
    fields->offset += SNAG_BINARY_OUTPUT_SPAN_SIZE;
    return true;
}

static bool
read_public_items(struct fields *fields, struct snag_binary_public_items *out)
{
    size_t start = fields->offset;
    uint64_t count;
    /* Metadata plus minimum provider ID and literal text occupy 28 bytes. */
    if (!read_uint(fields, 4u, &count) || count > (fields->size - fields->offset) / 28u) {
        return false;
    }
    for (uint64_t i = 0u; i < count; ++i) {
        struct snag_binary_public_value value = {0};
        if (!read_public_value(fields, &value)) return false;
    }
    *out = (struct snag_binary_public_items){fields->data + start, fields->offset - start};
    return true;
}

int
snag_binary_public_items_encode(struct snag_buf *out,
    const struct snag_binary_public_value *items, size_t count)
{
    if (!out || (!items && count) || count > (SNAG_MAX_EVENT_LINE - 4u) / 28u) {
        return invalid();
    }
    struct snag_buf encoded = {.max = SNAG_MAX_EVENT_LINE};
    int rc = -1;
    if (write_uint(&encoded, count, 4u) < 0) goto done;
    for (size_t i = 0u; i < count; ++i) {
        if (write_public_value(&encoded, &items[i]) < 0) goto done;
    }
    rc = snag_buf_append(out, encoded.data, encoded.len);
done:
    snag_buf_free(&encoded);
    return rc;
}

int
snag_binary_public_items_decode(const void *data, size_t size, struct snag_binary_public_items *out)
{
    struct fields fields = {.data = data, .size = size};
    struct snag_binary_public_items decoded;
    if (!data || !out || size > SNAG_MAX_EVENT_LINE ||
        !read_public_items(&fields, &decoded) || fields.offset != size) {
        return invalid();
    }
    *out = decoded;
    return 0;
}

int
snag_binary_public_items_next(const struct snag_binary_public_items *items, size_t *offset,
    struct snag_binary_public_value *out)
{
    if (!items || !items->data || items->size < 4u || items->size > SNAG_MAX_EVENT_LINE ||
        !offset || !out || (*offset && *offset < 4u) || *offset > items->size) {
        return invalid();
    }
    struct fields fields = {
        .data = items->data, .size = items->size, .offset = *offset ? *offset : 4u
    };
    if (fields.offset == fields.size) return 1;
    struct snag_binary_public_value decoded = {0};
    if (!read_public_value(&fields, &decoded)) return invalid();
    *out = decoded;
    *offset = fields.offset;
    return 0;
}

static bool
provider_object_valid(struct snag_binary_text text, bool reasoning)
{
    size_t max = reasoning ? SNAG_MAX_RESPONSE_GRAPH : SNAG_MAX_TOOL_ARGUMENTS;
    if (!text_valid(text, 2u, max)) return false;
    json_t *object = snag_json_load_canonical_bounded(text.data, text.size, max, NULL, 0u);
    bool valid = json_is_object(object) && (!reasoning || snag_reasoning_item_valid(object));
    json_decref(object);
    return valid;
}

static bool
call_valid(const struct snag_binary_call *call)
{
    if (!provider_id_valid(call->provider_id) || !provider_id_valid(call->provider_call_id) ||
        !text_valid(call->name, 1u, SNAG_MAX_EVENT_LINE)) {
        return false;
    }
    /* The existing catalog validator takes a terminated string. */
    struct snag_buf name = {.max = SNAG_MAX_EVENT_LINE + 1u};
    bool valid = snag_buf_append(&name, call->name.data, call->name.size) == 0 &&
        snag_buf_putc(&name, 0u) == 0 && snag_tool_name_valid((const char *)name.data);
    snag_buf_free(&name);
    return valid && provider_object_valid(call->arguments, false);
}

static int
write_graph_item(struct snag_buf *out, const struct snag_binary_graph_item *item)
{
    if (item->kind == SNAG_BINARY_ITEM_ASSISTANT || item->kind == SNAG_BINARY_ITEM_REFUSAL) {
        if (item->kind != item->data.output.item.kind) return invalid();
        return write_public_value(out, &item->data.output);
    }
    if (item->kind != SNAG_BINARY_ITEM_TOOL_CALL || !call_valid(&item->data.call)) {
        return invalid();
    }
    const struct snag_binary_call *call = &item->data.call;
    if (write_uint(out, item->kind, 1u) < 0 || snag_buf_append(out, call->id, 16u) < 0 ||
        write_text(out, call->provider_id, 1u, SNAG_MAX_PROVIDER_ID) < 0 ||
        write_text(out, call->provider_call_id, 1u, SNAG_MAX_PROVIDER_ID) < 0 ||
        write_text(out, call->name, 1u, SNAG_MAX_EVENT_LINE) < 0) {
        return -1;
    }
    return write_text(out, call->arguments, 2u, SNAG_MAX_TOOL_ARGUMENTS);
}

static bool
read_graph_item(struct fields *fields, struct snag_binary_graph_item *out)
{
    size_t start = fields->offset;
    uint64_t kind;
    if (!read_uint(fields, 1u, &kind)) return false;
    out->kind = (enum snag_binary_item_kind)kind;
    if (kind == SNAG_BINARY_ITEM_ASSISTANT || kind == SNAG_BINARY_ITEM_REFUSAL) {
        fields->offset = start;
        return read_public_value(fields, &out->data.output);
    }
    if (kind != SNAG_BINARY_ITEM_TOOL_CALL) return false;
    struct snag_binary_call *call = &out->data.call;
    return read_id(fields, call->id) &&
        read_text(fields, &call->provider_id, 1u, SNAG_MAX_PROVIDER_ID) &&
        read_text(fields, &call->provider_call_id, 1u, SNAG_MAX_PROVIDER_ID) &&
        read_text(fields, &call->name, 1u, SNAG_MAX_EVENT_LINE) &&
        read_text(fields, &call->arguments, 2u, SNAG_MAX_TOOL_ARGUMENTS) && call_valid(call);
}

static bool
read_graph_items(struct fields *fields, struct snag_binary_graph_items *out)
{
    size_t start = fields->offset;
    uint64_t count;
    if (!read_uint(fields, 4u, &count) || count > (fields->size - fields->offset) / 28u) {
        return false;
    }
    for (uint64_t i = 0u; i < count; ++i) {
        struct snag_binary_graph_item item = {0};
        if (!read_graph_item(fields, &item)) return false;
    }
    *out = (struct snag_binary_graph_items){fields->data + start, fields->offset - start,
        (uint32_t)count};
    return true;
}

int
snag_binary_graph_items_encode(struct snag_buf *out,
    const struct snag_binary_graph_item *items, size_t count)
{
    if (!out || (!items && count) || count > (SNAG_MAX_EVENT_LINE - 4u) / 28u) {
        return invalid();
    }
    struct snag_buf encoded = {.max = SNAG_MAX_EVENT_LINE};
    int rc = -1;
    if (write_uint(&encoded, count, 4u) < 0) goto done;
    for (size_t i = 0u; i < count; ++i) {
        if (write_graph_item(&encoded, &items[i]) < 0) goto done;
    }
    rc = snag_buf_append(out, encoded.data, encoded.len);
done:
    snag_buf_free(&encoded);
    return rc;
}

int
snag_binary_graph_items_decode(const void *data, size_t size, struct snag_binary_graph_items *out)
{
    struct fields fields = {.data = data, .size = size};
    struct snag_binary_graph_items decoded;
    if (!data || !out || size > SNAG_MAX_EVENT_LINE ||
        !read_graph_items(&fields, &decoded) || fields.offset != size) {
        return invalid();
    }
    *out = decoded;
    return 0;
}

int
snag_binary_graph_items_next(const struct snag_binary_graph_items *items, size_t *offset,
    struct snag_binary_graph_item *out)
{
    if (!items || !items->data || items->size < 4u || items->size > SNAG_MAX_EVENT_LINE ||
        !offset || !out || (*offset && *offset < 4u) || *offset > items->size) {
        return invalid();
    }
    struct fields fields = {
        .data = items->data, .size = items->size, .offset = *offset ? *offset : 4u
    };
    if (fields.offset == fields.size) return 1;
    struct snag_binary_graph_item decoded = {0};
    if (!read_graph_item(&fields, &decoded)) return invalid();
    *out = decoded;
    *offset = fields.offset;
    return 0;
}

static bool
read_continuation_item(struct fields *fields, struct snag_binary_continuation_item *out)
{
    return read_uint(fields, 8u, &out->before) &&
        read_text(fields, &out->item, 2u, SNAG_MAX_RESPONSE_GRAPH) &&
        provider_object_valid(out->item, true);
}

static bool
read_continuation(struct fields *fields, uint32_t semantic_count,
    struct snag_binary_continuation *out)
{
    size_t start = fields->offset;
    uint64_t count;
    uint64_t previous = 0u;
    if (!read_uint(fields, 4u, &count) || count > (fields->size - fields->offset) / 14u) {
        return false;
    }
    for (uint64_t i = 0u; i < count; ++i) {
        struct snag_binary_continuation_item item;
        if (!read_continuation_item(fields, &item) || item.before < previous ||
            item.before > semantic_count) {
            return false;
        }
        previous = item.before;
    }
    *out = (struct snag_binary_continuation){fields->data + start, fields->offset - start};
    return true;
}

int
snag_binary_continuation_encode(struct snag_buf *out,
    const struct snag_binary_continuation_item *items, size_t count, uint32_t semantic_count)
{
    if (!out || (!items && count) || count > (SNAG_MAX_EVENT_LINE - 4u) / 14u) {
        return invalid();
    }
    struct snag_buf encoded = {.max = SNAG_MAX_EVENT_LINE};
    uint64_t previous = 0u;
    int rc = -1;
    if (write_uint(&encoded, count, 4u) < 0) goto done;
    for (size_t i = 0u; i < count; ++i) {
        if (items[i].before < previous || items[i].before > semantic_count ||
            !provider_object_valid(items[i].item, true)) {
            (void)invalid();
            goto done;
        }
        if (write_uint(&encoded, items[i].before, 8u) < 0 ||
            write_text(&encoded, items[i].item, 2u, SNAG_MAX_RESPONSE_GRAPH) < 0) {
            goto done;
        }
        previous = items[i].before;
    }
    rc = snag_buf_append(out, encoded.data, encoded.len);
done:
    snag_buf_free(&encoded);
    return rc;
}

int
snag_binary_continuation_decode(const void *data, size_t size, uint32_t semantic_count,
    struct snag_binary_continuation *out)
{
    struct fields fields = {.data = data, .size = size};
    struct snag_binary_continuation decoded;
    if (!data || !out || size > SNAG_MAX_EVENT_LINE ||
        !read_continuation(&fields, semantic_count, &decoded) || fields.offset != size) {
        return invalid();
    }
    *out = decoded;
    return 0;
}

int
snag_binary_continuation_next(const struct snag_binary_continuation *items, size_t *offset,
    struct snag_binary_continuation_item *out)
{
    if (!items || !items->data || items->size < 4u || items->size > SNAG_MAX_EVENT_LINE ||
        !offset || !out || (*offset && *offset < 4u) || *offset > items->size) {
        return invalid();
    }
    struct fields fields = {
        .data = items->data, .size = items->size, .offset = *offset ? *offset : 4u
    };
    if (fields.offset == fields.size) return 1;
    struct snag_binary_continuation_item decoded;
    if (!read_continuation_item(&fields, &decoded)) return invalid();
    *out = decoded;
    *offset = fields.offset;
    return 0;
}

static bool
usage_valid(const struct snag_response_usage *usage, bool cached_present)
{
    return (!usage->cached_known || (cached_present &&
        usage->cached_input_tokens <= (uint64_t)INT64_MAX)) &&
        snag_response_usage_valid(usage) == 0;
}

static int
write_usage(struct snag_buf *out, const struct snag_response_usage *usage, bool cached_present)
{
    if (!usage_valid(usage, cached_present)) return invalid();
    unsigned flags = (usage->input_known ? 1u : 0u) | (usage->output_known ? 2u : 0u) |
        (usage->reasoning_known ? 4u : 0u) | (usage->total_known ? 8u : 0u) |
        (usage->cached_known ? 16u : 0u) | (cached_present ? 32u : 0u);
    const uint64_t tokens[] = {usage->input_tokens, usage->output_tokens,
        usage->reasoning_tokens, usage->total_tokens, usage->cached_input_tokens};
    if (write_uint(out, flags, 1u) < 0) return -1;
    for (size_t i = 0u; i < sizeof(tokens) / sizeof(tokens[0]); ++i) {
        if ((flags & (1u << i)) && write_uint(out, tokens[i], 8u) < 0) {
            return -1;
        }
    }
    return 0;
}

static bool
read_usage(struct fields *fields, struct snag_response_usage *usage, bool *cached_present)
{
    uint64_t flags;
    if (!read_uint(fields, 1u, &flags) || flags > 63u) return false;
    usage->input_known = (flags & 1u) != 0u;
    usage->output_known = (flags & 2u) != 0u;
    usage->reasoning_known = (flags & 4u) != 0u;
    usage->total_known = (flags & 8u) != 0u;
    usage->cached_known = (flags & 16u) != 0u;
    *cached_present = (flags & 32u) != 0u;
    uint64_t *tokens[] = {&usage->input_tokens, &usage->output_tokens,
        &usage->reasoning_tokens, &usage->total_tokens, &usage->cached_input_tokens};
    for (size_t i = 0u; i < sizeof(tokens) / sizeof(tokens[0]); ++i) {
        if ((flags & (1u << i)) && !read_uint(fields, 8u, tokens[i])) {
            return false;
        }
    }
    return usage_valid(usage, *cached_present);
}

static int
encode_response_complete(struct snag_buf *out, const struct snag_binary_response_complete *value)
{
    struct snag_binary_graph_items items;
    if (!value->cycle || !provider_id_valid(value->provider_id) ||
        value->continuation_form < SNAG_BINARY_CONTINUATION_ABSENT ||
        value->continuation_form > SNAG_BINARY_CONTINUATION_ITEMS ||
        snag_binary_graph_items_decode(value->items.data, value->items.size, &items) < 0) {
        return invalid();
    }
    if (snag_buf_append(out, value->turn, 16u) < 0 ||
        snag_buf_append(out, value->response, 16u) < 0 || write_uint(out, value->cycle, 4u) < 0 ||
        write_text(out, value->provider_id, 1u, SNAG_MAX_PROVIDER_ID) < 0 ||
        snag_buf_append(out, items.data, items.size) < 0 ||
        write_usage(out, &value->usage, value->cached_present) < 0 ||
        write_uint(out, value->continuation_form, 1u) < 0) {
        return -1;
    }
    if (value->continuation_form == SNAG_BINARY_CONTINUATION_ABSENT) return 0;
    if (snag_buf_append(out, value->continuation_scope, 32u) < 0) return -1;
    if (value->continuation_form == SNAG_BINARY_CONTINUATION_NULL) return 0;
    struct snag_binary_continuation continuation;
    if (snag_binary_continuation_decode(value->continuation.data, value->continuation.size,
        items.count, &continuation) < 0) {
        return -1;
    }
    return snag_buf_append(out, continuation.data, continuation.size);
}

static bool
decode_response_complete(struct fields *fields, struct snag_binary_response_complete *out)
{
    uint64_t value;
    if (!read_id(fields, out->turn) || !read_id(fields, out->response) ||
        !read_uint(fields, 4u, &value)) {
        return false;
    }
    out->cycle = (uint32_t)value;
    if (!out->cycle || !read_text(fields, &out->provider_id, 1u, SNAG_MAX_PROVIDER_ID) ||
        !provider_id_valid(out->provider_id) || !read_graph_items(fields, &out->items) ||
        !read_usage(fields, &out->usage, &out->cached_present) || !read_uint(fields, 1u, &value) ||
        value > SNAG_BINARY_CONTINUATION_ITEMS) {
        return false;
    }
    out->continuation_form = (enum snag_binary_continuation_form)value;
    if (value == SNAG_BINARY_CONTINUATION_ABSENT) return true;
    if (!read_bytes(fields, out->continuation_scope, 32u)) return false;
    return value == SNAG_BINARY_CONTINUATION_NULL ||
        read_continuation(fields, out->items.count, &out->continuation);
}

static bool
response_interruption_valid(const struct snag_binary_response_interruption *value)
{
    return value->cycle && value->origin >= SNAG_BINARY_INTERRUPT_USER &&
        value->origin <= SNAG_BINARY_INTERRUPT_STEERING &&
        value->reason >= SNAG_BINARY_INTERRUPT_CANCELLED &&
        value->reason <= SNAG_BINARY_INTERRUPT_CONTROL &&
        value->reason != SNAG_BINARY_INTERRUPT_SESSION_RECOVERED &&
        ((value->origin == SNAG_BINARY_INTERRUPT_STEERING) ==
         (value->reason == SNAG_BINARY_INTERRUPT_STEERED));
}

static int
encode_response_interruption(struct snag_buf *out,
    const struct snag_binary_response_interruption *value)
{
    struct snag_binary_public_items decoded;
    if (!response_interruption_valid(value) ||
        snag_binary_public_items_decode(value->partial.data, value->partial.size, &decoded) < 0) {
        return invalid();
    }
    if (snag_buf_append(out, value->turn, 16u) < 0 ||
        snag_buf_append(out, value->response, 16u) < 0 || write_uint(out, value->cycle, 4u) < 0 ||
        write_uint(out, value->origin, 1u) < 0 || write_uint(out, value->reason, 1u) < 0) {
        return -1;
    }
    return snag_buf_append(out, decoded.data, decoded.size);
}

static bool
decode_response_interruption(struct fields *fields, struct snag_binary_response_interruption *out)
{
    uint64_t value;
    if (!read_id(fields, out->turn) || !read_id(fields, out->response) ||
        !read_uint(fields, 4u, &value)) {
        return false;
    }
    out->cycle = (uint32_t)value;
    if (!read_uint(fields, 1u, &value)) return false;
    out->origin = (enum snag_binary_interrupt_origin)value;
    if (!read_uint(fields, 1u, &value)) return false;
    out->reason = (enum snag_binary_interrupt_reason)value;
    return response_interruption_valid(out) && read_public_items(fields, &out->partial);
}

static bool
response_failure_valid(const struct snag_binary_response_failure *value)
{
    /* Historical schemas add these fields in order; no holes or unknown bits. */
    if (value->present > 15u || (value->present & (value->present + 1u))) {
        return false;
    }
    return value->cycle && value->class_name >= SNAG_BINARY_FAILURE_CONTEXT &&
        value->class_name <= SNAG_BINARY_FAILURE_INTERNAL &&
        value->class_name != SNAG_BINARY_FAILURE_TOOL &&
        value->class_name != SNAG_BINARY_FAILURE_PERSISTENCE && value->retry_count <= 2u &&
        (!(value->present & SNAG_BINARY_FAILURE_TURN_RETRIES) ||
         value->turn_retry_attempts <= (uint64_t)UINT32_MAX + 1u);
}

static int
encode_response_failure(struct snag_buf *out, const struct snag_binary_response_failure *value)
{
    struct snag_binary_public_items partial;
    if (!response_failure_valid(value) ||
        snag_binary_public_items_decode(value->partial.data, value->partial.size, &partial) < 0) {
        return invalid();
    }
    if (snag_buf_append(out, value->turn, 16u) < 0 ||
        snag_buf_append(out, value->response, 16u) < 0 || write_uint(out, value->cycle, 4u) < 0 ||
        write_uint(out, value->class_name, 1u) < 0 || write_uint(out, value->retry_count, 1u) < 0 ||
        write_uint(out, value->present, 1u) < 0 || write_text(out, value->message, 0u, 8192u) < 0 ||
        snag_buf_append(out, partial.data, partial.size) < 0) {
        return -1;
    }
    if ((value->present & SNAG_BINARY_FAILURE_POLICY_STOPPED) &&
        write_uint(out, value->policy_stopped ? 1u : 0u, 1u) < 0) {
        return -1;
    }
    if ((value->present & SNAG_BINARY_FAILURE_TURN_RETRIES) &&
        write_uint(out, value->turn_retry_attempts, 8u) < 0) {
        return -1;
    }
    if ((value->present & SNAG_BINARY_FAILURE_NEW_INPUT) &&
        write_uint(out, value->new_input ? 1u : 0u, 1u) < 0) {
        return -1;
    }
    if ((value->present & SNAG_BINARY_FAILURE_POLICY) &&
        (write_text(out, value->policy_code, 0u, 63u) < 0 ||
         write_text(out, value->policy_type, 0u, 63u) < 0 ||
         write_text(out, value->clarification_skipped, 1u, 127u) < 0)) {
        return -1;
    }
    return 0;
}

static bool
decode_response_failure(struct fields *fields, struct snag_binary_response_failure *out)
{
    uint64_t value;
    if (!read_id(fields, out->turn) || !read_id(fields, out->response) ||
        !read_uint(fields, 4u, &value)) {
        return false;
    }
    out->cycle = (uint32_t)value;
    if (!read_uint(fields, 1u, &value)) return false;
    out->class_name = (enum snag_binary_failure_class)value;
    if (!read_uint(fields, 1u, &value)) return false;
    out->retry_count = (uint8_t)value;
    if (!read_uint(fields, 1u, &value)) return false;
    out->present = (uint8_t)value;
    if (!response_failure_valid(out) || !read_text(fields, &out->message, 0u, 8192u) ||
        !read_public_items(fields, &out->partial)) {
        return false;
    }
    if (out->present & SNAG_BINARY_FAILURE_POLICY_STOPPED) {
        if (!read_uint(fields, 1u, &value) || value > 1u) return false;
        out->policy_stopped = value != 0u;
    }
    if ((out->present & SNAG_BINARY_FAILURE_TURN_RETRIES) &&
        !read_uint(fields, 8u, &out->turn_retry_attempts)) {
        return false;
    }
    if (out->present & SNAG_BINARY_FAILURE_NEW_INPUT) {
        if (!read_uint(fields, 1u, &value) || value > 1u) return false;
        out->new_input = value != 0u;
    }
    if ((out->present & SNAG_BINARY_FAILURE_POLICY) &&
        (!read_text(fields, &out->policy_code, 0u, 63u) ||
         !read_text(fields, &out->policy_type, 0u, 63u) ||
         !read_text(fields, &out->clarification_skipped, 1u, 127u))) {
        return false;
    }
    return response_failure_valid(out);
}

static bool
text_is(struct snag_binary_text text, const char *literal)
{
    size_t size = strlen(literal);
    return text.data && text.size == size && !memcmp(text.data, literal, size);
}

/* The public list has already passed structural decoding. */
static bool
response_correction_valid(const struct snag_binary_response_correction *value)
{
    bool cyber = text_is(value->text, SNAG_CYBER_CLARIFICATION);
    if (!value->cycle || (!cyber && !text_is(value->text, SNAG_EMPTY_OUTPUT_CORRECTION) &&
        !text_is(value->text, SNAG_OVERSIZED_OUTPUT_CORRECTION))) {
        return false;
    }
    if (!cyber) return true;
    size_t offset = 0u;
    struct snag_binary_public_value item;
    int rc;
    while ((rc = snag_binary_public_items_next(&value->partial, &offset, &item)) == 0) {
        if (item.item.kind != SNAG_BINARY_ITEM_ASSISTANT) return false;
    }
    return rc == 1;
}

static int
encode_response_correction(struct snag_buf *out,
    const struct snag_binary_response_correction *value)
{
    struct snag_binary_public_items partial;
    if (snag_binary_public_items_decode(value->partial.data, value->partial.size, &partial) < 0 ||
        !response_correction_valid(value)) {
        return invalid();
    }
    if (snag_buf_append(out, value->turn, 16u) < 0 ||
        snag_buf_append(out, value->response, 16u) < 0 || write_uint(out, value->cycle, 4u) < 0 ||
        snag_buf_append(out, value->correction, 16u) < 0 ||
        write_text(out, value->text, 1u, SNAG_MAX_EVENT_LINE) < 0) {
        return -1;
    }
    return snag_buf_append(out, partial.data, partial.size);
}

static bool
decode_response_correction(struct fields *fields, struct snag_binary_response_correction *out)
{
    uint64_t value;
    if (!read_id(fields, out->turn) || !read_id(fields, out->response) ||
        !read_uint(fields, 4u, &value)) {
        return false;
    }
    out->cycle = (uint32_t)value;
    return read_id(fields, out->correction) &&
        read_text(fields, &out->text, 1u, SNAG_MAX_EVENT_LINE) &&
        read_public_items(fields, &out->partial) && response_correction_valid(out);
}

static bool
turn_outcome_kind(enum snag_binary_kind kind)
{
    return kind >= SNAG_BINARY_TURN_YIELD_REQUESTED && kind <= SNAG_BINARY_TURN_FAILED;
}

static int
encode_turn_outcome(struct snag_buf *out, const struct snag_binary_event *event)
{
    switch (event->kind) {
    case SNAG_BINARY_TURN_YIELD_REQUESTED:
    case SNAG_BINARY_TURN_CANCEL_REQUESTED:
        return snag_buf_append(out, event->data.turn, 16u);
    case SNAG_BINARY_TURN_COMPLETED:
        if (snag_buf_append(out, event->data.completed.turn, 16u) < 0 ||
            snag_buf_append(out, event->data.completed.response, 16u) < 0) return -1;
        return snag_buf_append(out, event->data.completed.item, 16u);
    case SNAG_BINARY_TURN_COMPLETED_SILENT:
        if (event->data.silent.reason < SNAG_BINARY_QUIET_ROOM_UPDATE ||
            event->data.silent.reason > SNAG_BINARY_QUIET_REPLY_EXHAUSTED) return invalid();
        if (snag_buf_append(out, event->data.silent.turn, 16u) < 0 ||
            snag_buf_append(out, event->data.silent.response, 16u) < 0) return -1;
        return write_uint(out, event->data.silent.reason, 1u);
    case SNAG_BINARY_TURN_INTERRUPTED:
        if (event->data.interrupted.origin < SNAG_BINARY_INTERRUPT_USER ||
            event->data.interrupted.origin > SNAG_BINARY_INTERRUPT_OUTPUT ||
            event->data.interrupted.reason < SNAG_BINARY_INTERRUPT_CANCELLED ||
            event->data.interrupted.reason > SNAG_BINARY_INTERRUPT_SESSION_RECOVERED)
            return invalid();
        if (snag_buf_append(out, event->data.interrupted.turn, 16u) < 0 ||
            write_uint(out, event->data.interrupted.origin, 1u) < 0) return -1;
        return write_uint(out, event->data.interrupted.reason, 1u);
    case SNAG_BINARY_TURN_FAILED:
        if (event->data.failed.class_id < SNAG_BINARY_FAILURE_CONTEXT ||
            event->data.failed.class_id > SNAG_BINARY_FAILURE_INTERNAL) return invalid();
        if (snag_buf_append(out, event->data.failed.turn, 16u) < 0 ||
            write_uint(out, event->data.failed.class_id, 1u) < 0) return -1;
        return write_text(out, event->data.failed.message, 0u, 8192u);
    case SNAG_BINARY_TURN_RECOVERY:
        if (event->data.recovery.has_retry_attempts &&
            event->data.recovery.retry_attempts > (uint64_t)UINT32_MAX + 1u) return invalid();
        if (snag_buf_append(out, event->data.recovery.turn, 16u) < 0 ||
            write_uint(out, event->data.recovery.has_retry_attempts ? 1u : 0u, 1u) < 0) return -1;
        if (event->data.recovery.has_retry_attempts &&
            write_uint(out, event->data.recovery.retry_attempts, 8u) < 0) return -1;
        if (write_text(out, event->data.recovery.class_name, 0u, SNAG_MAX_EVENT_LINE) < 0)
            return -1;
        return write_text(out, event->data.recovery.message, 0u, 8192u);
    default:
        return invalid();
    }
}

static bool
decode_turn_outcome(struct fields *fields, struct snag_binary_event *event)
{
    uint64_t value;
    switch (event->kind) {
    case SNAG_BINARY_TURN_YIELD_REQUESTED:
    case SNAG_BINARY_TURN_CANCEL_REQUESTED:
        return read_id(fields, event->data.turn);
    case SNAG_BINARY_TURN_COMPLETED:
        return read_id(fields, event->data.completed.turn) &&
            read_id(fields, event->data.completed.response) &&
            read_id(fields, event->data.completed.item);
    case SNAG_BINARY_TURN_COMPLETED_SILENT:
        if (!read_id(fields, event->data.silent.turn) ||
            !read_id(fields, event->data.silent.response) ||
            !read_uint(fields, 1u, &value) || value < SNAG_BINARY_QUIET_ROOM_UPDATE ||
            value > SNAG_BINARY_QUIET_REPLY_EXHAUSTED) return false;
        event->data.silent.reason = (enum snag_binary_quiet_reason)value;
        return true;
    case SNAG_BINARY_TURN_INTERRUPTED:
        if (!read_id(fields, event->data.interrupted.turn) ||
            !read_uint(fields, 1u, &value) || value < SNAG_BINARY_INTERRUPT_USER ||
            value > SNAG_BINARY_INTERRUPT_OUTPUT) return false;
        event->data.interrupted.origin = (enum snag_binary_interrupt_origin)value;
        if (!read_uint(fields, 1u, &value) || value < SNAG_BINARY_INTERRUPT_CANCELLED ||
            value > SNAG_BINARY_INTERRUPT_SESSION_RECOVERED) return false;
        event->data.interrupted.reason = (enum snag_binary_interrupt_reason)value;
        return true;
    case SNAG_BINARY_TURN_FAILED:
        if (!read_id(fields, event->data.failed.turn) || !read_uint(fields, 1u, &value) ||
            value < SNAG_BINARY_FAILURE_CONTEXT || value > SNAG_BINARY_FAILURE_INTERNAL)
            return false;
        event->data.failed.class_id = (enum snag_binary_failure_class)value;
        return read_text(fields, &event->data.failed.message, 0u, 8192u);
    case SNAG_BINARY_TURN_RECOVERY:
        if (!read_id(fields, event->data.recovery.turn) ||
            !read_uint(fields, 1u, &value) || value > 1u) return false;
        event->data.recovery.has_retry_attempts = value != 0u;
        if (event->data.recovery.has_retry_attempts &&
            (!read_uint(fields, 8u, &event->data.recovery.retry_attempts) ||
             event->data.recovery.retry_attempts > (uint64_t)UINT32_MAX + 1u)) return false;
        return read_text(fields, &event->data.recovery.class_name, 0u, SNAG_MAX_EVENT_LINE) &&
            read_text(fields, &event->data.recovery.message, 0u, 8192u);
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
    return kind >= SNAG_BINARY_GOAL_STARTED && kind <= SNAG_BINARY_GOAL_BLOCKED_WAIT_FOR;
}

/* Count each field's historical owners against the canonical journal depth. */
#define SNAG_BINARY_FIELD_VALUE_DEPTH_MAX 48u
#define SNAG_BINARY_RESULT_VALUE_END 7u

static int
write_result_string(struct snag_buf *out, const char *text, size_t size)
{
    if (!text || size > SNAG_MAX_EVENT_LINE ||
        !snag_utf8_valid((const unsigned char *)text, size, true)) {
        return invalid();
    }
    if (snag_buf_putc(out, SNAG_BINARY_RESULT_STRING) < 0 ||
        snag_buf_append(out, text, size) < 0) {
        return -1;
    }
    return snag_buf_putc(out, 0u);
}

static int write_result_value(struct snag_buf *, const json_t *, unsigned int);

static int
write_result_object(struct snag_buf *out, const json_t *object, unsigned int depth)
{
    size_t count = json_object_size(object);
    /* Every entry needs at least a string tag, key terminator and value tag. */
    if (count > (SNAG_MAX_EVENT_LINE - 2u) / 3u) return invalid();
    struct snag_key_ref *keys = count ? calloc(count, sizeof(*keys)) : NULL;
    if (count && !keys) return -1;
    size_t i = 0u;
    int rc = -1;
    for (void *iter = json_object_iter((json_t *)object); iter;
        iter = json_object_iter_next((json_t *)object, iter)) {
        if (i == count) {
            invalid();
            goto done;
        }
        keys[i].name = json_object_iter_key(iter);
        keys[i].len = json_object_iter_key_len(iter);
        if (!keys[i].name || !snag_utf8_valid((const unsigned char *)keys[i].name,
            keys[i].len, true)) {
            invalid();
            goto done;
        }
        ++i;
    }
    if (i != count) {
        invalid();
        goto done;
    }
    if (count) qsort(keys, count, sizeof(*keys), snag_key_ref_compare);
    if (snag_buf_putc(out, SNAG_BINARY_RESULT_OBJECT) < 0) goto done;
    for (i = 0u; i < count; ++i) {
        if (write_result_string(out, keys[i].name, keys[i].len) < 0 ||
            write_result_value(out, json_object_getn(object, keys[i].name, keys[i].len),
                depth + 1u) < 0) {
            goto done;
        }
    }
    rc = snag_buf_putc(out, SNAG_BINARY_RESULT_VALUE_END);
 done:
    free(keys);
    return rc;
}

static int
write_result_value(struct snag_buf *out, const json_t *value, unsigned int depth)
{
    if (!value) return invalid();
    if (depth > SNAG_BINARY_FIELD_VALUE_DEPTH_MAX) {
        return snag_errno(EOVERFLOW);
    }
    switch (json_typeof(value)) {
    case JSON_NULL:
        return snag_buf_putc(out, SNAG_BINARY_RESULT_NULL);
    case JSON_FALSE:
        return snag_buf_putc(out, SNAG_BINARY_RESULT_FALSE);
    case JSON_TRUE:
        return snag_buf_putc(out, SNAG_BINARY_RESULT_TRUE);
    case JSON_INTEGER: {
        int64_t number = json_integer_value(value);
        if (number >= 0 && number <= 127) {
            return snag_buf_putc(out, 128u + (unsigned char)number);
        }
        uint64_t bits = number < 0 ? (uint64_t)(-(number + 1)) * 2u + 1u : (uint64_t)number * 2u;
        if (snag_buf_putc(out, SNAG_BINARY_RESULT_INTEGER) < 0) return -1;
        do {
            unsigned char byte = (unsigned char)(bits & 127u);
            bits >>= 7u;
            if (bits) byte |= 128u;
            if (snag_buf_putc(out, byte) < 0) return -1;
        } while (bits);
        return 0;
    }
    case JSON_STRING:
        return write_result_string(out, json_string_value(value), json_string_length(value));
    case JSON_ARRAY: {
        size_t count = json_array_size(value);
        if (count > SNAG_MAX_EVENT_LINE - 2u) return invalid();
        if (snag_buf_putc(out, SNAG_BINARY_RESULT_ARRAY) < 0) return -1;
        for (size_t i = 0u; i < count; ++i) {
            if (write_result_value(out, json_array_get(value, i), depth + 1u) < 0) {
                return -1;
            }
        }
        return snag_buf_putc(out, SNAG_BINARY_RESULT_VALUE_END);
    }
    case JSON_OBJECT:
        return write_result_object(out, value, depth);
    default:
        return invalid();
    }
}

int
snag_binary_result_value_encode(struct snag_buf *out, const json_t *value)
{
    if (!out) return invalid();
    struct snag_buf encoded = {.max = SNAG_MAX_EVENT_LINE};
    int rc = write_result_value(&encoded, value, 3u);
    struct snag_binary_result_value checked;
    if (rc == 0) rc = snag_binary_result_value_decode(encoded.data, encoded.len, &checked);
    if (rc == 0) rc = snag_buf_append(out, encoded.data, encoded.len);
    snag_buf_free(&encoded);
    return rc;
}

static bool
read_result_string(struct fields *fields, struct snag_binary_text *out)
{
    const unsigned char *begin = fields->data + fields->offset;
    const unsigned char *end = memchr(begin, 0, fields->size - fields->offset);
    if (!end) return false;
    size_t size = (size_t)(end - begin);
    if (!snag_utf8_valid(begin, size, true)) return false;
    *out = (struct snag_binary_text){begin, size};
    fields->offset += size + 1u;
    return true;
}

static bool
read_result_integer(struct fields *fields, int64_t *out)
{
    uint64_t bits = 0u;
    for (unsigned int i = 0u; i < 10u; ++i) {
        uint64_t byte;
        if (!read_uint(fields, 1u, &byte) || (i == 9u && byte > 1u)) {
            return false;
        }
        bits |= (byte & 127u) << (i * 7u);
        if (!(byte & 128u)) {
            if (i && !byte) return false;
            int64_t number = bits & 1u ? -1 - (int64_t)(bits >> 1u) : (int64_t)(bits >> 1u);
            if (number >= 0 && number <= 127) return false;
            *out = number;
            return true;
        }
    }
    return false;
}

static int
result_size_add(size_t *size, size_t extra)
{
    if (extra > SNAG_MAX_EVENT_LINE - *size) return snag_errno(EOVERFLOW);
    *size += extra;
    return 0;
}

static int
result_string_size(struct snag_binary_text text, size_t *out)
{
    size_t size = 2u;
    for (size_t i = 0u; i < text.size; ++i) {
        unsigned char byte = text.data[i];
        size_t extra = byte < 0x20u ? 6u : byte == '"' || byte == '\\' ? 2u : 1u;
        if (result_size_add(&size, extra) < 0) return -1;
    }
    *out = size;
    return 0;
}

static size_t
result_integer_size(int64_t number)
{
    uint64_t magnitude = number < 0 ? (uint64_t)(-(number + 1)) + 1u : (uint64_t)number;
    size_t size = number < 0 ? 1u : 0u;
    do {
        size++;
        magnitude /= 10u;
    } while (magnitude);
    return size;
}

static int read_result_value(struct fields *, unsigned int,
    struct snag_binary_result_value *, json_t **);

static int
read_result_container(struct fields *fields, unsigned int depth, bool object,
    size_t *canonical_size, json_t **out)
{
    json_t *container = out ? (object ? json_object() : json_array()) : NULL;
    if (out && !container) return snag_errno(ENOMEM);
    struct snag_key_ref previous = {0};
    size_t size = 2u;
    bool first = true;
    while (fields->offset < fields->size) {
        if (fields->data[fields->offset] == SNAG_BINARY_RESULT_VALUE_END) {
            fields->offset++;
            *canonical_size = size;
            if (out) *out = container;
            return 0;
        }
        struct snag_binary_text key = {0};
        if (!first && result_size_add(&size, 1u) < 0) goto fail;
        first = false;
        if (object) {
            uint64_t tag;
            if (!read_uint(fields, 1u, &tag) || tag != SNAG_BINARY_RESULT_STRING ||
                !read_result_string(fields, &key)) {
                invalid();
                goto fail;
            }
            struct snag_key_ref current = {(const char *)key.data, key.size};
            if (previous.name && snag_key_ref_compare(&previous, &current) >= 0) {
                invalid();
                goto fail;
            }
            previous = current;
            size_t key_size;
            if (result_string_size(key, &key_size) < 0 || result_size_add(&size, key_size) < 0 ||
                result_size_add(&size, 1u) < 0) {
                goto fail;
            }
        }
        json_t *member = NULL;
        struct snag_binary_result_value view;
        if (read_result_value(fields, depth + 1u, &view, out ? &member : NULL) < 0) {
            goto fail;
        }
        if (result_size_add(&size, view.canonical_size) < 0) {
            json_decref(member);
            goto fail;
        }
        if (out && (object ? json_object_set_new(container, (const char *)key.data, member) :
            json_array_append_new(container, member)) < 0) {
            errno = ENOMEM;
            goto fail;
        }
    }
    invalid();
 fail:
    json_decref(container);
    return -1;
}

static int
read_result_value(struct fields *fields, unsigned int depth,
    struct snag_binary_result_value *out, json_t **json_out)
{
    if (depth > SNAG_BINARY_FIELD_VALUE_DEPTH_MAX) {
        return snag_errno(EOVERFLOW);
    }
    size_t begin = fields->offset;
    uint64_t tag;
    if (!read_uint(fields, 1u, &tag)) return invalid();
    struct snag_binary_result_value decoded = {.data = fields->data + begin};
    json_t *value = NULL;
    if (tag >= 128u) {
        decoded.kind = SNAG_BINARY_RESULT_INTEGER;
        decoded.integer = (int64_t)(tag - 128u);
        decoded.canonical_size = result_integer_size(decoded.integer);
        if (json_out) value = json_integer(decoded.integer);
    } else {
        decoded.kind = (enum snag_binary_result_value_kind)tag;
        switch (decoded.kind) {
        case SNAG_BINARY_RESULT_NULL:
            decoded.canonical_size = 4u;
            if (json_out) value = json_null();
            break;
        case SNAG_BINARY_RESULT_FALSE:
            decoded.canonical_size = 5u;
            if (json_out) value = json_false();
            break;
        case SNAG_BINARY_RESULT_TRUE:
            decoded.canonical_size = 4u;
            if (json_out) value = json_true();
            break;
        case SNAG_BINARY_RESULT_INTEGER:
            if (!read_result_integer(fields, &decoded.integer)) {
                return invalid();
            }
            decoded.canonical_size = result_integer_size(decoded.integer);
            if (json_out) value = json_integer(decoded.integer);
            break;
        case SNAG_BINARY_RESULT_STRING: {
            struct snag_binary_text text;
            if (!read_result_string(fields, &text)) return invalid();
            if (result_string_size(text, &decoded.canonical_size) < 0) {
                return -1;
            }
            if (json_out) value = json_stringn((const char *)text.data, text.size);
            break;
        }
        case SNAG_BINARY_RESULT_ARRAY:
        case SNAG_BINARY_RESULT_OBJECT:
            if (read_result_container(fields, depth, decoded.kind == SNAG_BINARY_RESULT_OBJECT,
                &decoded.canonical_size, json_out ? &value : NULL) < 0) {
                return -1;
            }
            break;
        default:
            return invalid();
        }
    }
    if (json_out && !value) return snag_errno(ENOMEM);
    decoded.size = fields->offset - begin;
    if (out) *out = decoded;
    if (json_out) *json_out = value;
    return 0;
}

int
snag_binary_result_value_decode(const void *data, size_t size, struct snag_binary_result_value *out)
{
    if (!data || !out || size > SNAG_MAX_EVENT_LINE) return invalid();
    struct fields fields = {.data = data, .size = size};
    struct snag_binary_result_value decoded;
    if (read_result_value(&fields, 3u, &decoded, NULL) < 0) return -1;
    if (fields.offset != size) return invalid();
    *out = decoded;
    return 0;
}

int
snag_binary_result_value_json(const struct snag_binary_result_value *value, json_t **out)
{
    if (!value || !value->data || !out || value->size > SNAG_MAX_EVENT_LINE) {
        return invalid();
    }
    struct fields fields = {.data = value->data, .size = value->size};
    json_t *decoded = NULL;
    if (read_result_value(&fields, 3u, NULL, &decoded) < 0) return -1;
    if (fields.offset != fields.size) {
        json_decref(decoded);
        return invalid();
    }
    *out = decoded;
    return 0;
}

int
snag_binary_rule_value_encode(struct snag_buf *out, const json_t *value)
{
    if (!out) return invalid();
    struct snag_buf encoded = {.max = SNAG_MAX_EVENT_LINE};
    int rc = write_result_value(&encoded, value, 2u);
    struct snag_binary_result_value checked;
    if (rc == 0) rc = snag_binary_rule_value_decode(encoded.data, encoded.len, &checked);
    if (rc == 0) rc = snag_buf_append(out, encoded.data, encoded.len);
    snag_buf_free(&encoded);
    return rc;
}

int
snag_binary_rule_value_decode(const void *data, size_t size, struct snag_binary_result_value *out)
{
    if (!data || !out || size > SNAG_MAX_EVENT_LINE) return invalid();
    struct fields fields = {.data = data, .size = size};
    struct snag_binary_result_value decoded;
    if (read_result_value(&fields, 2u, &decoded, NULL) < 0) return -1;
    if (fields.offset != size) return invalid();
    *out = decoded;
    return 0;
}

int
snag_binary_rule_value_json(const struct snag_binary_result_value *value, json_t **out)
{
    if (!value || !value->data || !out || value->size > SNAG_MAX_EVENT_LINE) {
        return invalid();
    }
    struct fields fields = {.data = value->data, .size = value->size};
    json_t *decoded = NULL;
    if (read_result_value(&fields, 2u, NULL, &decoded) < 0) return -1;
    if (fields.offset != fields.size) {
        json_decref(decoded);
        return invalid();
    }
    *out = decoded;
    return 0;
}

static bool
hosted_search_kind(enum snag_binary_kind kind)
{
    return kind == SNAG_BINARY_HOSTED_SEARCH_STARTED || kind == SNAG_BINARY_HOSTED_SEARCH_FINISHED;
}

static bool
hosted_detail_valid(bool started, const struct snag_binary_result_value *value)
{
    if (started) {
        return value->kind == SNAG_BINARY_RESULT_OBJECT &&
            value->canonical_size <= SNAG_MAX_HOSTED_ACTION;
    }
    if (value->kind != SNAG_BINARY_RESULT_ARRAY) return false;
    struct fields fields = {.data = value->data, .size = value->size, .offset = 1u};
    while (fields.offset < fields.size) {
        uint64_t tag;
        if (!read_uint(&fields, 1u, &tag)) return false;
        if (tag == SNAG_BINARY_RESULT_VALUE_END) return fields.offset == fields.size;
        struct snag_binary_text text;
        if (tag != SNAG_BINARY_RESULT_STRING || !read_result_string(&fields, &text) ||
            !text_valid(text, 1u, SNAG_MAX_HOSTED_SOURCE_URL)) return false;
    }
    return false;
}

static int
encode_hosted_search(struct snag_buf *out, const struct snag_binary_event *event)
{
    const struct snag_binary_hosted_search *value = &event->data.hosted_search;
    bool started = event->kind == SNAG_BINARY_HOSTED_SEARCH_STARTED;
    if (!provider_id_valid(value->item_id) ||
        (started && (value->status.data || value->status.size)) ||
        (!value->has_detail && (value->detail.data || value->detail.size))) return invalid();
    if (snag_buf_append(out, value->turn, 16u) < 0 ||
        write_text(out, value->item_id, 1u, SNAG_MAX_PROVIDER_ID) < 0 ||
        (!started && write_text(out, value->status, 1u, 64u) < 0) ||
        write_uint(out, value->has_detail, 1u) < 0) return -1;
    if (value->has_detail) {
        struct snag_binary_result_value checked;
        if (snag_binary_rule_value_decode(value->detail.data, value->detail.size, &checked) < 0) {
            return -1;
        }
        if (!hosted_detail_valid(started, &checked)) return invalid();
        return snag_buf_append(out, checked.data, checked.size);
    }
    return 0;
}

static bool
decode_hosted_search(struct fields *fields, struct snag_binary_event *event)
{
    struct snag_binary_hosted_search *value = &event->data.hosted_search;
    bool started = event->kind == SNAG_BINARY_HOSTED_SEARCH_STARTED;
    uint64_t present;
    if (!read_id(fields, value->turn) ||
        !read_text(fields, &value->item_id, 1u, SNAG_MAX_PROVIDER_ID) ||
        !provider_id_valid(value->item_id) ||
        (!started && !read_text(fields, &value->status, 1u, 64u)) ||
        !read_uint(fields, 1u, &present) || present > 1u) return false;
    value->has_detail = present != 0u;
    return !value->has_detail || (read_result_value(fields, 2u, &value->detail, NULL) == 0 &&
        hosted_detail_valid(started, &value->detail));
}

static bool
tool_excerpt_valid(const struct snag_binary_tool_excerpt *value)
{
    if (value->original_bytes > INT64_MAX || value->retained_bytes > INT64_MAX ||
        value->discarded_bytes > value->original_bytes ||
        !text_valid(value->retained, 0u, SNAG_MAX_EVENT_LINE)) {
        return false;
    }
    if (value->encoding == SNAG_BINARY_EXCERPT_UTF8) {
        return value->retained.size == value->retained_bytes;
    }
    return value->encoding == SNAG_BINARY_EXCERPT_BASE64 && value->retained.size % 4u == 0u;
}

static bool
tool_output_ref_valid(const struct snag_binary_tool_result *result)
{
    const struct snag_binary_tool_output_ref *ref = &result->output_ref;
    if (!result->max_output_tokens || ref->log_end > INT64_MAX || ref->log_start > ref->log_end ||
        ref->stdin_accepted > INT64_MAX || ref->stdin_written > ref->stdin_accepted ||
        ref->stdin_pending > ref->stdin_accepted - ref->stdin_written ||
        (!ref->native && (ref->first_sequence || ref->end_sequence)) ||
        (ref->native && (ref->end_sequence > INT64_MAX ||
            ref->first_sequence > ref->end_sequence ||
            ((ref->first_sequence == ref->end_sequence) != (ref->log_start == ref->log_end)) ||
            ((!ref->first_sequence) != (!ref->end_sequence)) ||
            ((!ref->first_sequence) != (!ref->log_start && !ref->log_end))))) {
        return false;
    }
    for (size_t i = 0u; i < 2u; ++i) {
        if (ref->to[i] > INT64_MAX || ref->from[i] > ref->to[i] ||
            result->streams[i].original_bytes != ref->to[i] - ref->from[i]) {
            return false;
        }
    }
    return true;
}

static bool
tool_result_content_valid(struct snag_binary_content content)
{
    if (!content.data && !content.size) return true;
    struct snag_binary_content checked;
    if (snag_binary_content_decode(content.data, content.size, &checked) < 0) {
        return false;
    }
    size_t offset = 0u;
    uint64_t bytes = 0u;
    struct snag_binary_part part;
    int rc;
    while ((rc = snag_binary_content_next(&checked, &offset, &part)) == 0) {
        if (part.kind == SNAG_BINARY_PART_IMAGE) {
            if (part.asset.bytes > SNAG_MEDIA_REQUEST_MAX - bytes) return false;
            bytes += part.asset.bytes;
        }
    }
    return rc == 1;
}

static bool
tool_result_valid(const struct snag_binary_tool_result *result)
{
    struct snag_binary_result_value exit_code;
    struct snag_binary_result_value signal;
    if (result->duration_ms > INT64_MAX ||
        result->max_output_tokens > SNAG_CONFIG_TOKEN_LIMIT_MAX ||
        result->has_handle != (result->status == SNAG_BINARY_TOOL_RUNNING) ||
        !text_valid(result->model_text, 0u, SNAG_MAX_EVENT_LINE) ||
        !tool_excerpt_valid(&result->streams[0]) || !tool_excerpt_valid(&result->streams[1]) ||
        (result->has_output_ref && !tool_output_ref_valid(result)) ||
        (!result->has_output_ref && result->output_ref.native) ||
        !tool_result_content_valid(result->content) ||
        snag_binary_result_value_decode(result->exit_code.data, result->exit_code.size,
            &exit_code) < 0 ||
        snag_binary_result_value_decode(result->signal.data, result->signal.size, &signal) < 0) {
        return false;
    }
    enum snag_binary_tool_reason reason = result->reason;
    switch (result->status) {
    case SNAG_BINARY_TOOL_NOT_RUN:
        return (reason >= SNAG_BINARY_TOOL_PROTOCOL_CONFLICT &&
            reason <= SNAG_BINARY_TOOL_RULE_REJECTED) ||
            (reason >= SNAG_BINARY_TOOL_MCP_UNAVAILABLE &&
                reason <= SNAG_BINARY_TOOL_MCP_TRANSPORT);
    case SNAG_BINARY_TOOL_OUTCOME_UNKNOWN:
        return reason == SNAG_BINARY_TOOL_OWNER_LOST ||
            reason == SNAG_BINARY_TOOL_UNREAPED_AFTER_SIGKILL ||
            reason == SNAG_BINARY_TOOL_MCP_UNKNOWN;
    case SNAG_BINARY_TOOL_DENIED:
        return reason == SNAG_BINARY_TOOL_USER_DENIED;
    case SNAG_BINARY_TOOL_CANCELLED:
        return reason == SNAG_BINARY_TOOL_TURN_CANCELLED;
    case SNAG_BINARY_TOOL_RUNNING:
        return reason == SNAG_BINARY_TOOL_REASON_NONE ||
            reason == SNAG_BINARY_TOOL_TIMEOUT_HANDOFF ||
            reason == SNAG_BINARY_TOOL_WAIT_TIMEOUT || reason == SNAG_BINARY_TOOL_OPERATOR_YIELD ||
            reason == SNAG_BINARY_TOOL_BATCH_YIELD || reason == SNAG_BINARY_TOOL_STEERING_HANDOFF;
    case SNAG_BINARY_TOOL_SUCCEEDED:
    case SNAG_BINARY_TOOL_FAILED:
    case SNAG_BINARY_TOOL_SIGNALED:
        if (reason != SNAG_BINARY_TOOL_REASON_NONE &&
            reason != SNAG_BINARY_TOOL_OUTPUT_DRAIN_TIMEOUT) {
            return false;
        }
        return result->status == SNAG_BINARY_TOOL_SIGNALED ?
            exit_code.kind == SNAG_BINARY_RESULT_NULL && signal.kind == SNAG_BINARY_RESULT_INTEGER :
            exit_code.kind == SNAG_BINARY_RESULT_INTEGER && signal.kind == SNAG_BINARY_RESULT_NULL;
    case SNAG_BINARY_TOOL_TIMED_OUT:
    case SNAG_BINARY_TOOL_PATCH_REJECTED:
    case SNAG_BINARY_TOOL_IO_FAILED:
        return reason == SNAG_BINARY_TOOL_REASON_NONE;
    default:
        return false;
    }
}

static int
write_tool_excerpt(struct snag_buf *out, const struct snag_binary_tool_excerpt *value)
{
    if (write_uint(out, value->encoding, 1u) < 0 ||
        write_uint(out, value->discarded_bytes, 8u) < 0 ||
        write_uint(out, value->original_bytes, 8u) < 0 ||
        write_uint(out, value->retained_bytes, 8u) < 0) {
        return -1;
    }
    return write_text(out, value->retained, 0u, SNAG_MAX_EVENT_LINE);
}

static int
write_tool_output_ref(struct snag_buf *out, const struct snag_binary_tool_output_ref *value)
{
    if (snag_buf_append(out, value->handle, 16u) < 0) return -1;
    for (size_t i = 0u; i < 2u; ++i) {
        if (write_uint(out, value->from[i], 8u) < 0 || write_uint(out, value->to[i], 8u) < 0) {
            return -1;
        }
    }
    if (write_uint(out, value->stdin_accepted, 8u) < 0 ||
        write_uint(out, value->stdin_written, 8u) < 0 ||
        write_uint(out, value->stdin_pending, 8u) < 0 ||
        write_uint(out, value->stdin_open ? 1u : 0u, 1u) < 0 ||
        write_uint(out, value->log_start, 8u) < 0) {
        return -1;
    }
    if (write_uint(out, value->log_end, 8u) < 0) return -1;
    if (value->native && (write_uint(out, value->first_sequence, 8u) < 0 ||
        write_uint(out, value->end_sequence, 8u) < 0)) return -1;
    return 0;
}

static int
write_tool_result(struct snag_buf *out, const struct snag_binary_tool_result *value)
{
    if (!tool_result_valid(value)) return invalid();
    unsigned int flags = (value->max_output_tokens ? 1u : 0u) |
        (value->has_output_ref ? 2u : 0u) | (value->content.data ? 4u : 0u) |
        (value->output_ref.native ? 8u : 0u);
    if (write_uint(out, value->status, 1u) < 0 || write_uint(out, value->reason, 1u) < 0 ||
        write_uint(out, flags, 1u) < 0 || write_uint(out, value->duration_ms, 8u) < 0 ||
        snag_buf_append(out, value->exit_code.data, value->exit_code.size) < 0 ||
        snag_buf_append(out, value->signal.data, value->signal.size) < 0 ||
        (value->has_handle && snag_buf_append(out, value->handle, 16u) < 0) ||
        write_text(out, value->model_text, 0u, SNAG_MAX_EVENT_LINE) < 0 ||
        write_tool_excerpt(out, &value->streams[0]) < 0 ||
        write_tool_excerpt(out, &value->streams[1]) < 0 ||
        (value->max_output_tokens && write_uint(out, value->max_output_tokens, 8u) < 0) ||
        (value->has_output_ref && write_tool_output_ref(out, &value->output_ref) < 0)) {
        return -1;
    }
    return value->content.data ? write_content(out, value->content) : 0;
}

static bool
read_tool_excerpt(struct fields *fields, struct snag_binary_tool_excerpt *out)
{
    uint64_t encoding;
    if (!read_uint(fields, 1u, &encoding) ||
        !read_uint(fields, 8u, &out->discarded_bytes) ||
        !read_uint(fields, 8u, &out->original_bytes) ||
        !read_uint(fields, 8u, &out->retained_bytes) ||
        !read_text(fields, &out->retained, 0u, SNAG_MAX_EVENT_LINE)) {
        return false;
    }
    out->encoding = (enum snag_binary_excerpt_encoding)encoding;
    return true;
}

static bool
read_tool_output_ref(struct fields *fields, struct snag_binary_tool_output_ref *out)
{
    if (!read_bytes(fields, out->handle, 16u)) return false;
    for (size_t i = 0u; i < 2u; ++i) {
        if (!read_uint(fields, 8u, &out->from[i]) || !read_uint(fields, 8u, &out->to[i])) {
            return false;
        }
    }
    uint64_t open;
    if (!read_uint(fields, 8u, &out->stdin_accepted) ||
        !read_uint(fields, 8u, &out->stdin_written) ||
        !read_uint(fields, 8u, &out->stdin_pending) ||
        !read_uint(fields, 1u, &open) || open > 1u ||
        !read_uint(fields, 8u, &out->log_start) || !read_uint(fields, 8u, &out->log_end)) {
        return false;
    }
    out->stdin_open = open != 0u;
    return true;
}

static bool
read_tool_result(struct fields *fields, struct snag_binary_tool_result *out)
{
    uint64_t status;
    uint64_t reason;
    uint64_t flags;
    if (!read_uint(fields, 1u, &status) || !read_uint(fields, 1u, &reason) ||
        !read_uint(fields, 1u, &flags) || (flags & ~(fields->references ? 15u : 7u)) ||
        ((flags & 8u) && !(flags & 2u)) ||
        !read_uint(fields, 8u, &out->duration_ms) ||
        read_result_value(fields, 3u, &out->exit_code, NULL) < 0 ||
        read_result_value(fields, 3u, &out->signal, NULL) < 0) {
        return false;
    }
    out->status = (enum snag_binary_tool_status)status;
    out->reason = (enum snag_binary_tool_reason)reason;
    out->has_handle = out->status == SNAG_BINARY_TOOL_RUNNING;
    out->has_output_ref = (flags & 2u) != 0u;
    out->output_ref.native = (flags & 8u) != 0u;
    if ((out->has_handle && !read_bytes(fields, out->handle, 16u)) ||
        !read_text(fields, &out->model_text, 0u, SNAG_MAX_EVENT_LINE) ||
        !read_tool_excerpt(fields, &out->streams[0]) ||
        !read_tool_excerpt(fields, &out->streams[1]) ||
        ((flags & 1u) && (!read_uint(fields, 8u, &out->max_output_tokens) ||
            !out->max_output_tokens)) ||
        (out->has_output_ref && !read_tool_output_ref(fields, &out->output_ref)) ||
        (out->output_ref.native &&
            (!read_uint(fields, 8u, &out->output_ref.first_sequence) ||
             !read_uint(fields, 8u, &out->output_ref.end_sequence))) ||
        ((flags & 4u) && !read_content(fields, &out->content))) {
        return false;
    }
    return tool_result_valid(out);
}

static bool
process_close_valid(const struct snag_binary_process_close *value)
{
    if (value->cause < SNAG_BINARY_PROCESS_USER_INTERRUPT ||
        value->cause > SNAG_BINARY_PROCESS_INTERNAL_FAILURE) {
        return false;
    }
    enum snag_binary_tool_status status = value->result.status;
    return status == SNAG_BINARY_TOOL_SUCCEEDED || status == SNAG_BINARY_TOOL_FAILED ||
        status == SNAG_BINARY_TOOL_SIGNALED || status == SNAG_BINARY_TOOL_TIMED_OUT ||
        status == SNAG_BINARY_TOOL_CANCELLED || status == SNAG_BINARY_TOOL_OUTCOME_UNKNOWN ||
        status == SNAG_BINARY_TOOL_IO_FAILED;
}

static int
encode_tool_finish(struct snag_buf *out, const struct snag_binary_tool_finish *value)
{
    if (snag_buf_append(out, value->turn, 16u) < 0 ||
        snag_buf_append(out, value->call, 16u) < 0) {
        return -1;
    }
    return write_tool_result(out, &value->result);
}

static bool
decode_tool_finish(struct fields *fields, struct snag_binary_tool_finish *out)
{
    return read_bytes(fields, out->turn, 16u) && read_bytes(fields, out->call, 16u) &&
        read_tool_result(fields, &out->result);
}

static int
encode_process_close(struct snag_buf *out, const struct snag_binary_process_close *value)
{
    if (!process_close_valid(value)) return invalid();
    if (snag_buf_append(out, value->turn, 16u) < 0 ||
        snag_buf_append(out, value->handle, 16u) < 0 || write_uint(out, value->cause, 1u) < 0) {
        return -1;
    }
    return write_tool_result(out, &value->result);
}

static bool
decode_process_close(struct fields *fields, struct snag_binary_process_close *out)
{
    uint64_t cause;
    if (!read_bytes(fields, out->turn, 16u) || !read_bytes(fields, out->handle, 16u) ||
        !read_uint(fields, 1u, &cause) || !read_tool_result(fields, &out->result)) {
        return false;
    }
    out->cause = (enum snag_binary_process_cause)cause;
    return process_close_valid(out);
}

static int
encode_tool_start(struct snag_buf *out, const struct snag_binary_tool_start *value)
{
    if (snag_buf_append(out, value->turn, 16u) < 0 ||
        snag_buf_append(out, value->call, 16u) < 0 ||
        snag_buf_append(out, value->action_sha256, 32u) < 0) {
        return -1;
    }
    return write_text(out, value->cwd, 1u, SNAG_PATH_MAX_BYTES);
}

static bool
decode_tool_start(struct fields *fields, struct snag_binary_tool_start *out)
{
    return read_id(fields, out->turn) && read_id(fields, out->call) &&
        read_bytes(fields, out->action_sha256, 32u) &&
        read_text(fields, &out->cwd, 1u, SNAG_PATH_MAX_BYTES);
}

static bool
process_output_valid(const struct snag_binary_process_output *value)
{
    return value->data && value->size && value->size <= SNAG_BINARY_PROCESS_CHUNK_MAX &&
        value->stream <= 1u && value->offset <= (uint64_t)INT64_MAX - value->size &&
        ((value->encoding == SNAG_BINARY_BYTES_UTF8 &&
          snag_utf8_valid(value->data, value->size, true)) ||
         value->encoding == SNAG_BINARY_BYTES_BASE64);
}

static int
encode_process_output(struct snag_buf *out, const struct snag_binary_process_output *value)
{
    if (!process_output_valid(value)) return invalid();
    if (snag_buf_append(out, value->turn, 16u) < 0 ||
        snag_buf_append(out, value->handle, 16u) < 0 || write_uint(out, value->offset, 8u) < 0 ||
        write_uint(out, value->stream, 1u) < 0 || write_uint(out, value->encoding, 1u) < 0 ||
        write_uint(out, value->size, 4u) < 0) {
        return -1;
    }
    return snag_buf_append(out, value->data, value->size);
}

static bool
decode_process_output(struct fields *fields, struct snag_binary_process_output *out)
{
    uint64_t value;
    if (!read_id(fields, out->turn) || !read_id(fields, out->handle) ||
        !read_uint(fields, 8u, &out->offset) || !read_uint(fields, 1u, &value)) {
        return false;
    }
    out->stream = (unsigned)value;
    if (!read_uint(fields, 1u, &value)) return false;
    out->encoding = (enum snag_binary_byte_encoding)value;
    if (!read_uint(fields, 4u, &value) || value > fields->size - fields->offset) {
        return false;
    }
    out->data = fields->data + fields->offset;
    out->size = (size_t)value;
    if (!process_output_valid(out)) return false;
    fields->offset += out->size;
    return true;
}

static bool
irc_text_valid(struct snag_binary_text text, size_t minimum, size_t maximum)
{
    if (!text_valid(text, minimum, maximum)) return false;
    for (size_t i = 0u; i < text.size; ++i) {
        if (text.data[i] < 0x20u || text.data[i] == 0x7fu) return false;
    }
    return true;
}

static bool
irc_event_valid(const struct snag_binary_irc_event *event)
{
    return event->kind >= SNAG_BINARY_IRC_CONNECTED &&
        event->kind <= SNAG_BINARY_IRC_HISTORY_READY && event->timestamp_ms &&
        event->timestamp_ms <= INT64_MAX && event->sequence <= INT64_MAX &&
        event->has_stream == (event->sequence != 0u) &&
        (event->has_watermark || (!event->has_stream && !event->input)) &&
        (event->classified || (!event->urgent && !event->reply)) &&
        irc_text_valid(event->endpoint, 1u, SNAG_CONFIG_IRC_ENDPOINT_MAX) &&
        irc_text_valid(event->room, 0u, SNAG_CONFIG_IRC_ROOM_MAX + 1u) &&
        irc_text_valid(event->nick, 0u, SNAG_CONFIG_IRC_NICK_MAX) &&
        irc_text_valid(event->text, 0u, SNAG_IRC_TEXT_MAX);
}

static int
encode_irc_event(struct snag_buf *out, const struct snag_binary_irc_event *event)
{
    if (!irc_event_valid(event)) return invalid();
    unsigned int flags = (event->historical ? 1u : 0u) | (event->is_local ? 2u : 0u) |
        (event->op ? 4u : 0u) | (event->has_watermark ? 8u : 0u) |
        (event->has_stream ? 16u : 0u) | (event->input ? 32u : 0u) |
        (event->classified ? 64u : 0u) | (event->urgent ? 128u : 0u) |
        (event->reply ? 256u : 0u) | (event->echo_expected ? 512u : 0u);
    if (write_uint(out, event->kind, 1u) < 0 || write_uint(out, flags, 2u) < 0 ||
        write_uint(out, event->timestamp_ms, 8u) < 0) {
        return -1;
    }
    if (event->has_stream && (snag_buf_append(out, event->stream, 16u) < 0 ||
        write_uint(out, event->sequence, 8u) < 0)) {
        return -1;
    }
    if (write_text(out, event->endpoint, 1u, SNAG_CONFIG_IRC_ENDPOINT_MAX) < 0 ||
        write_text(out, event->room, 0u, SNAG_CONFIG_IRC_ROOM_MAX + 1u) < 0 ||
        write_text(out, event->nick, 0u, SNAG_CONFIG_IRC_NICK_MAX) < 0) {
        return -1;
    }
    return write_text(out, event->text, 0u, SNAG_IRC_TEXT_MAX);
}

static bool
decode_irc_event(struct fields *fields, struct snag_binary_irc_event *event)
{
    uint64_t kind, flags;
    if (!read_uint(fields, 1u, &kind) || !read_uint(fields, 2u, &flags) || flags > 1023u ||
        !read_uint(fields, 8u, &event->timestamp_ms)) {
        return false;
    }
    event->kind = (enum snag_binary_irc_kind)kind;
    event->historical = (flags & 1u) != 0u;
    event->is_local = (flags & 2u) != 0u;
    event->op = (flags & 4u) != 0u;
    event->has_watermark = (flags & 8u) != 0u;
    event->has_stream = (flags & 16u) != 0u;
    event->input = (flags & 32u) != 0u;
    event->classified = (flags & 64u) != 0u;
    event->urgent = (flags & 128u) != 0u;
    event->reply = (flags & 256u) != 0u;
    event->echo_expected = (flags & 512u) != 0u;
    if (event->has_stream && (!read_id(fields, event->stream) ||
        !read_uint(fields, 8u, &event->sequence))) {
        return false;
    }
    return read_text(fields, &event->endpoint, 1u, SNAG_CONFIG_IRC_ENDPOINT_MAX) &&
        read_text(fields, &event->room, 0u, SNAG_CONFIG_IRC_ROOM_MAX + 1u) &&
        read_text(fields, &event->nick, 0u, SNAG_CONFIG_IRC_NICK_MAX) &&
        read_text(fields, &event->text, 0u, SNAG_IRC_TEXT_MAX) && irc_event_valid(event);
}

static bool
irc_route_valid(const struct snag_binary_irc_event *event)
{
    const struct snag_binary_irc_route *route = &event->route;
    bool chat = event->kind == SNAG_BINARY_IRC_MESSAGE || event->kind == SNAG_BINARY_IRC_NOTICE;
    bool sent_input = chat && event->is_local && !event->historical && event->input &&
        route->kind == SNAG_IRC_CHANNEL && route->identity == SNAG_IRC_OPERATOR &&
        route->direction == SNAG_IRC_OUTGOING &&
        (route->delivery == SNAG_IRC_WRITTEN || route->delivery == SNAG_IRC_ACKNOWLEDGED);
    bool visible = route->kind == SNAG_IRC_CHANNEL ||
        (route->kind == SNAG_IRC_QUERY && route->identity == SNAG_IRC_AGENT);
    if (route->has_membership && (event->kind == SNAG_BINARY_IRC_CONNECTED ||
        event->kind == SNAG_BINARY_IRC_DISCONNECTED)) visible = false;
    return irc_event_valid(event) && event->has_watermark && route->generation &&
        (!event->echo_expected || (chat && event->is_local && !event->historical &&
            route->kind == SNAG_IRC_CHANNEL && route->direction == SNAG_IRC_OUTGOING)) &&
        route->generation <= INT64_MAX && (unsigned)route->identity <= SNAG_IRC_AGENT &&
        (unsigned)route->kind <= SNAG_IRC_QUERY &&
        (unsigned)route->direction <= SNAG_IRC_OUTGOING &&
        (unsigned)route->delivery <= SNAG_IRC_UNCERTAIN &&
        irc_text_valid(route->peer, 0u, SNAG_CONFIG_IRC_NICK_MAX) &&
        irc_text_valid(route->target, 0u, SNAG_CONFIG_IRC_ROOM_MAX + 1u) &&
        irc_text_valid(route->source, 0u, SNAG_IRC_LINE_MAX) &&
        (route->has_membership || (!route->joined && !route->rejoin)) &&
        (!route->has_membership || (route->kind == SNAG_IRC_CHANNEL &&
            ((event->kind != SNAG_BINARY_IRC_CONNECTED &&
                event->kind != SNAG_BINARY_IRC_DISCONNECTED) ||
             (event->is_local && !event->historical &&
                ((event->kind == SNAG_BINARY_IRC_CONNECTED) == route->joined))))) &&
        (!route->revised || (route->direction == SNAG_IRC_OUTGOING &&
            route->delivery >= SNAG_IRC_ACKNOWLEDGED)) &&
        (route->kind == SNAG_IRC_QUERY ? route->peer.size && !event->room.size :
            !route->peer.size) &&
        (route->kind != SNAG_IRC_CHANNEL || (event->room.size &&
            event->room.size == route->target.size &&
            !memcmp(event->room.data, route->target.data, event->room.size))) &&
        (route->kind != SNAG_IRC_CONNECTION_EVENTS ||
            (!event->room.size && event->kind != SNAG_BINARY_IRC_MESSAGE)) &&
        (!chat || route->target.size) && (!route->action ||
            event->kind == SNAG_BINARY_IRC_MESSAGE) &&
        (route->direction == SNAG_IRC_INCOMING ?
            route->delivery == SNAG_IRC_DELIVERY_NONE && !route->has_send :
            route->delivery != SNAG_IRC_DELIVERY_NONE && route->has_send &&
                (!event->input || sent_input)) &&
        (visible || (!event->input && !event->urgent && !event->reply)) &&
        ((!event->historical && event->kind == SNAG_BINARY_IRC_MESSAGE &&
            (route->direction == SNAG_IRC_INCOMING || sent_input)) ||
            (!event->urgent && !event->reply)) && (!event->reply || event->urgent) &&
        (route->reply_captured || !route->has_reply) &&
        (!route->reply_captured || (event->classified && event->reply &&
            route->kind == SNAG_IRC_CHANNEL));
}

static int
encode_irc_route(struct snag_buf *out, const struct snag_binary_irc_event *event)
{
    if (!irc_route_valid(event)) return invalid();
    const struct snag_binary_irc_route *route = &event->route;
    unsigned int flags = (route->action ? 1u : 0u) | (route->revised ? 2u : 0u) |
        (route->has_membership ? 4u : 0u) | (route->joined ? 8u : 0u) |
        (route->rejoin ? 16u : 0u) | (route->has_send ? 32u : 0u) |
        (route->reply_captured ? 64u : 0u) | (route->has_reply ? 128u : 0u);
    if (encode_irc_event(out, event) < 0 || write_uint(out, flags, 1u) < 0 ||
        write_uint(out, route->generation, 8u) < 0 ||
        write_uint(out, route->identity, 1u) < 0 || write_uint(out, route->kind, 1u) < 0 ||
        write_uint(out, route->direction, 1u) < 0 || write_uint(out, route->delivery, 1u) < 0 ||
        snag_buf_append(out, route->connection, 16u) < 0 ||
        snag_buf_append(out, route->conversation, 16u) < 0 ||
        write_text(out, route->peer, 0u, SNAG_CONFIG_IRC_NICK_MAX) < 0 ||
        write_text(out, route->target, 0u, SNAG_CONFIG_IRC_ROOM_MAX + 1u) < 0 ||
        write_text(out, route->source, 0u, SNAG_IRC_LINE_MAX) < 0 ||
        (route->has_send && snag_buf_append(out, route->send, 16u) < 0) ||
        (route->has_membership && snag_buf_append(out, route->membership, 16u) < 0) ||
        (route->has_reply && (snag_buf_append(out, route->reply_conversation, 16u) < 0 ||
            snag_buf_append(out, route->reply_membership, 16u) < 0))) return -1;
    return 0;
}

static bool
decode_irc_route(struct fields *fields, struct snag_binary_irc_event *event)
{
    struct snag_binary_irc_route *route = &event->route;
    uint64_t flags, value;
    if (!decode_irc_event(fields, event) || !read_uint(fields, 1u, &flags) ||
        !read_uint(fields, 8u, &route->generation) || !read_uint(fields, 1u, &value)) return false;
    route->identity = (enum snag_irc_identity)value;
    if (!read_uint(fields, 1u, &value)) return false;
    route->kind = (enum snag_irc_conversation_kind)value;
    if (!read_uint(fields, 1u, &value)) return false;
    route->direction = (enum snag_irc_direction)value;
    if (!read_uint(fields, 1u, &value)) return false;
    route->delivery = (enum snag_irc_delivery)value;
    route->action = (flags & 1u) != 0u;
    route->revised = (flags & 2u) != 0u;
    route->has_membership = (flags & 4u) != 0u;
    route->joined = (flags & 8u) != 0u;
    route->rejoin = (flags & 16u) != 0u;
    route->has_send = (flags & 32u) != 0u;
    route->reply_captured = (flags & 64u) != 0u;
    route->has_reply = (flags & 128u) != 0u;
    return read_id(fields, route->connection) && read_id(fields, route->conversation) &&
        read_text(fields, &route->peer, 0u, SNAG_CONFIG_IRC_NICK_MAX) &&
        read_text(fields, &route->target, 0u, SNAG_CONFIG_IRC_ROOM_MAX + 1u) &&
        read_text(fields, &route->source, 0u, SNAG_IRC_LINE_MAX) &&
        (!route->has_send || read_id(fields, route->send)) &&
        (!route->has_membership || read_id(fields, route->membership)) &&
        (!route->has_reply || (read_id(fields, route->reply_conversation) &&
            read_id(fields, route->reply_membership))) && irc_route_valid(event);
}

static int
encode_irc_settings(struct snag_buf *out, const struct snag_binary_event *event)
{
    switch (event->kind) {
    case SNAG_BINARY_IRC_SLEEP_SET:
        if (event->data.irc_sleep_set.until_ms > INT64_MAX ||
            (event->data.irc_sleep_set.until_ms && !event->data.irc_sleep_set.messages))
            return invalid();
        if (write_uint(out, event->data.irc_sleep_set.until_ms, 8u) < 0) return -1;
        return write_uint(out, event->data.irc_sleep_set.messages, 4u);
    case SNAG_BINARY_IRC_SLEEP_WOKE:
        if (event->data.irc_sleep_woke < SNAG_BINARY_IRC_WAKE_TIMEOUT ||
            event->data.irc_sleep_woke > SNAG_BINARY_IRC_WAKE_MESSAGES) return invalid();
        return write_uint(out, event->data.irc_sleep_woke, 1u);
    case SNAG_BINARY_IRC_COMPACT_CONFIGURED:
        if (write_uint(out, event->data.irc_compact_configured.after_updates, 4u) < 0) return -1;
        return write_text(out, event->data.irc_compact_configured.instruction,
            0u, SNAG_MAX_STEERING_TEXT);
    case SNAG_BINARY_IRC_COMPACTED:
        if (!event->data.irc_compacted.through_seq || !event->data.irc_compacted.count ||
            event->data.irc_compacted.through_seq > INT64_MAX ||
            event->data.irc_compacted.count > INT64_MAX) return invalid();
        if (write_uint(out, event->data.irc_compacted.through_seq, 8u) < 0 ||
            write_uint(out, event->data.irc_compacted.count, 8u) < 0) return -1;
        return write_text(out, event->data.irc_compacted.summary, 1u, SNAG_MAX_IRC_SNAPSHOT);
    default: return invalid();
    }
}

static bool
decode_irc_settings(struct fields *fields, struct snag_binary_event *event)
{
    uint64_t value;
    switch (event->kind) {
    case SNAG_BINARY_IRC_SLEEP_SET:
        if (!read_uint(fields, 8u, &event->data.irc_sleep_set.until_ms) ||
            !read_uint(fields, 4u, &value)) return false;
        event->data.irc_sleep_set.messages = (uint32_t)value;
        return event->data.irc_sleep_set.until_ms <= INT64_MAX &&
            (!event->data.irc_sleep_set.until_ms || event->data.irc_sleep_set.messages);
    case SNAG_BINARY_IRC_SLEEP_WOKE:
        if (!read_uint(fields, 1u, &value) || value < SNAG_BINARY_IRC_WAKE_TIMEOUT ||
            value > SNAG_BINARY_IRC_WAKE_MESSAGES) return false;
        event->data.irc_sleep_woke = value;
        return true;
    case SNAG_BINARY_IRC_COMPACT_CONFIGURED:
        if (!read_uint(fields, 4u, &value)) return false;
        event->data.irc_compact_configured.after_updates = (uint32_t)value;
        return read_text(fields, &event->data.irc_compact_configured.instruction,
            0u, SNAG_MAX_STEERING_TEXT);
    case SNAG_BINARY_IRC_COMPACTED:
        return read_uint(fields, 8u, &event->data.irc_compacted.through_seq) &&
            read_uint(fields, 8u, &event->data.irc_compacted.count) &&
            event->data.irc_compacted.through_seq && event->data.irc_compacted.count &&
            event->data.irc_compacted.through_seq <= INT64_MAX &&
            event->data.irc_compacted.count <= INT64_MAX &&
            read_text(fields, &event->data.irc_compacted.summary, 1u, SNAG_MAX_IRC_SNAPSHOT);
    default: return false;
    }
}

static int
encode_irc_snapshot(struct snag_buf *out, const struct snag_binary_irc_snapshot *snapshot)
{
    if (snapshot->reason < SNAG_BINARY_IRC_SNAPSHOT_JOIN ||
        snapshot->reason > SNAG_BINARY_IRC_SNAPSHOT_COMPACTION || !snapshot->timestamp_ms ||
        snapshot->timestamp_ms > INT64_MAX) {
        return invalid();
    }
    if (write_uint(out, snapshot->reason, 1u) < 0 ||
        write_uint(out, snapshot->timestamp_ms, 8u) < 0) {
        return -1;
    }
    return write_text(out, snapshot->text, 1u, SNAG_MAX_IRC_SNAPSHOT);
}

static bool
decode_irc_snapshot(struct fields *fields, struct snag_binary_irc_snapshot *snapshot)
{
    uint64_t reason;
    if (!read_uint(fields, 1u, &reason) || reason < SNAG_BINARY_IRC_SNAPSHOT_JOIN ||
        reason > SNAG_BINARY_IRC_SNAPSHOT_COMPACTION) {
        return false;
    }
    snapshot->reason = (enum snag_binary_irc_snapshot_reason)reason;
    return read_uint(fields, 8u, &snapshot->timestamp_ms) && snapshot->timestamp_ms &&
        snapshot->timestamp_ms <= INT64_MAX &&
        read_text(fields, &snapshot->text, 1u, SNAG_MAX_IRC_SNAPSHOT);
}

static bool
read_sequence(struct fields *fields, uint64_t *out)
{
    uint64_t value = 0u;
    for (unsigned int shift = 0u; shift < 63u; shift += 7u) {
        uint64_t byte;
        if (!read_uint(fields, 1u, &byte)) return false;
        value |= (byte & 127u) << shift;
        if (!(byte & 128u)) {
            if (!value || (shift && !byte)) return false;
            *out = value;
            return true;
        }
    }
    return false;
}

static bool
read_sequences(struct fields *fields, struct snag_binary_sequences *out)
{
    size_t begin = fields->offset;
    uint64_t count, previous = 0u;
    if (!read_uint(fields, 4u, &count) || !count || count > fields->size - fields->offset) {
        return false;
    }
    for (uint64_t i = 0u; i < count; ++i) {
        uint64_t value;
        if (!read_sequence(fields, &value) || value <= previous) return false;
        previous = value;
    }
    *out = (struct snag_binary_sequences){fields->data + begin, fields->offset - begin};
    return true;
}

int
snag_binary_sequences_encode(struct snag_buf *out, const uint64_t *values, size_t count)
{
    if (!out || !values || !count || count > SNAG_MAX_EVENT_LINE - 4u) return invalid();
    struct snag_buf encoded = {.max = SNAG_MAX_EVENT_LINE};
    int rc = -1;
    uint64_t previous = 0u;
    if (write_uint(&encoded, count, 4u) < 0) goto done;
    for (size_t i = 0u; i < count; ++i) {
        uint64_t value = values[i];
        if (value <= previous || value > INT64_MAX) {
            invalid();
            goto done;
        }
        previous = value;
        do {
            unsigned char byte = (unsigned char)(value & 127u);
            value >>= 7u;
            if (value) byte |= 128u;
            if (snag_buf_putc(&encoded, byte) < 0) goto done;
        } while (value);
    }
    rc = snag_buf_append(out, encoded.data, encoded.len);
 done:
    snag_buf_free(&encoded);
    return rc;
}

int
snag_binary_sequences_decode(const void *data, size_t size, struct snag_binary_sequences *out)
{
    struct fields fields = {.data = data, .size = size};
    struct snag_binary_sequences decoded;
    if (!data || !out || size > SNAG_MAX_EVENT_LINE || !read_sequences(&fields, &decoded) ||
        fields.offset != size) {
        return invalid();
    }
    *out = decoded;
    return 0;
}

int
snag_binary_sequences_next(const struct snag_binary_sequences *sequences,
    size_t *offset, uint64_t *out)
{
    if (!sequences || !sequences->data || sequences->size < 5u ||
        sequences->size > SNAG_MAX_EVENT_LINE || !offset || !out ||
        (*offset && *offset < 4u) || *offset > sequences->size) {
        return invalid();
    }
    struct fields fields = {.data = sequences->data, .size = sequences->size,
        .offset = *offset ? *offset : 4u};
    if (fields.offset == fields.size) return 1;
    uint64_t decoded;
    if (!read_sequence(&fields, &decoded)) return invalid();
    *out = decoded;
    *offset = fields.offset;
    return 0;
}

static bool
irc_admission_input_valid(const struct snag_binary_record *record)
{
    if (!record->kind) {
        return !record->version && !record->flags && !record->size && !record->payload;
    }
    if (record->kind != SNAG_BINARY_INPUT_RECEIVED && record->kind != SNAG_BINARY_STEERING_ADDED) {
        return false;
    }
    struct snag_binary_event decoded;
    return snag_binary_event_decode(record, &decoded) == 0;
}

static int
encode_irc_admission(struct snag_buf *out, const struct snag_binary_irc_admission *admission)
{
    struct snag_binary_sequences sequences;
    const struct snag_binary_record *input = &admission->input;
    if (!irc_admission_input_valid(input) ||
        snag_binary_sequences_decode(admission->sequences.data, admission->sequences.size,
            &sequences) < 0) {
        return invalid();
    }
    if (snag_buf_append(out, sequences.data, sequences.size) < 0 ||
        write_uint(out, input->kind, 2u) < 0) {
        return -1;
    }
    if (!input->kind) return 0;
    if (write_uint(out, input->version, 2u) < 0 || write_uint(out, input->size, 4u) < 0) {
        return -1;
    }
    return snag_buf_append(out, input->payload, input->size);
}

static bool
decode_irc_admission(struct fields *fields, struct snag_binary_irc_admission *admission)
{
    uint64_t value;
    if (!read_sequences(fields, &admission->sequences) || !read_uint(fields, 2u, &value)) {
        return false;
    }
    struct snag_binary_record *input = &admission->input;
    input->kind = (uint16_t)value;
    if (!input->kind) return true;
    if (!read_uint(fields, 2u, &value)) return false;
    input->version = (uint16_t)value;
    if (!read_uint(fields, 4u, &value) || value > fields->size - fields->offset) return false;
    input->payload = fields->data + fields->offset;
    input->size = (size_t)value;
    if (!irc_admission_input_valid(input)) return false;
    fields->offset += input->size;
    return true;
}

static const char *const voice_kind_names[] = {
    "voice_started", "voice_stopped", "voice_transcript", "voice_usage", "voice_asr_failed",
    "voice_interrupted", "voice_response", "voice_result", "voice_muted"
};

#define VOICE_FIELD(name) {#name, offsetof(struct snag_binary_voice_fields, name)}
static const struct {
    const char *name;
    size_t offset;
} voice_fields[] = {
    VOICE_FIELD(arguments), VOICE_FIELD(call_id), VOICE_FIELD(covered_next_seq),
    VOICE_FIELD(covered_offset), VOICE_FIELD(covered_sha256), VOICE_FIELD(delay_ms),
    VOICE_FIELD(disposition), VOICE_FIELD(effort), VOICE_FIELD(error), VOICE_FIELD(http_status),
    VOICE_FIELD(input), VOICE_FIELD(input_id), VOICE_FIELD(interrupted), VOICE_FIELD(item_id),
    VOICE_FIELD(item_index), VOICE_FIELD(kind), VOICE_FIELD(metrics), VOICE_FIELD(model),
    VOICE_FIELD(muted), VOICE_FIELD(operation), VOICE_FIELD(output), VOICE_FIELD(output_id),
    VOICE_FIELD(played_ms), VOICE_FIELD(presentation), VOICE_FIELD(provider), VOICE_FIELD(queue_id),
    VOICE_FIELD(reason), VOICE_FIELD(reply), VOICE_FIELD(report), VOICE_FIELD(response_id),
    VOICE_FIELD(result), VOICE_FIELD(retry_after_ms), VOICE_FIELD(source),
    VOICE_FIELD(source_as_of_seq), VOICE_FIELD(source_call_id), VOICE_FIELD(source_through_seq),
    VOICE_FIELD(speaker), VOICE_FIELD(status), VOICE_FIELD(stream), VOICE_FIELD(submitted_id),
    VOICE_FIELD(summary), VOICE_FIELD(target_session_id), VOICE_FIELD(text), VOICE_FIELD(tool),
    VOICE_FIELD(tool_call_id), VOICE_FIELD(turn_id)
};
#undef VOICE_FIELD

#define VOICE_FIELD_COUNT (sizeof(voice_fields) / sizeof(voice_fields[0]))
_Static_assert(VOICE_FIELD_COUNT == 46u, "The voice v1 field set is fixed");

static const struct snag_binary_result_value *
voice_field_view(const struct snag_binary_voice_fields *fields, size_t index)
{
    return (const void *)((const unsigned char *)fields + voice_fields[index].offset);
}

static bool
voice_named_field(struct snag_binary_text key)
{
    if (key.size == 4u && !memcmp(key.data, "type", 4u)) return true;
    for (size_t i = 0u; i < VOICE_FIELD_COUNT; ++i) {
        if (key.size == strlen(voice_fields[i].name) &&
            !memcmp(key.data, voice_fields[i].name, key.size)) {
            return true;
        }
    }
    return false;
}

static bool
read_extensions(struct fields *fields, struct snag_binary_result_value *out,
    bool (*named_field)(struct snag_binary_text))
{
    if (read_result_value(fields, 2u, out, NULL) < 0 || out->kind != SNAG_BINARY_RESULT_OBJECT) {
        return false;
    }
    struct fields keys = {.data = out->data, .size = out->size, .offset = 1u};
    while (keys.offset + 1u < keys.size) {
        uint64_t tag;
        struct snag_binary_text key;
        if (!read_uint(&keys, 1u, &tag) || tag != SNAG_BINARY_RESULT_STRING ||
            !read_result_string(&keys, &key) || named_field(key) ||
            read_result_value(&keys, 3u, NULL, NULL) < 0) {
            return false;
        }
    }
    return keys.offset == keys.size - 1u;
}

static bool
turn_config_named_field(struct snag_binary_text key)
{
    static const char *const names[] = {
        "provider", "model", "effort", "max_parallel_commands", "default_yield_ms",
        "max_wait_ms", "default_timeout_ms", "max_timeout_ms", "tool_output_bytes",
        "output_cache_bytes", "max_turn_retries", "prompt_schema", "replay_schema",
        "tool_schema", "capability_version", "profile_id", "max_output_tokens",
        "parallel_tool_calls"
    };
    for (size_t i = 0u; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (key.size == strlen(names[i]) && !memcmp(key.data, names[i], key.size)) {
            return true;
        }
    }
    return false;
}

static int
write_turn_config(struct snag_buf *out, const struct snag_binary_turn_config *config)
{
    if (config->present & 0x8000u) return invalid();
    if (write_selection(out, &config->selection) < 0 ||
        write_uint(out, config->present, 2u) < 0) {
        return -1;
    }
    for (size_t i = 0u; i < SNAG_BINARY_TURN_NUMBER_COUNT; ++i) {
        if ((config->present & (1u << i)) && write_uint(out, config->numbers[i], 8u) < 0) {
            return -1;
        }
    }
    for (size_t i = 0u; i < SNAG_BINARY_TURN_VALUE_COUNT; ++i) {
        if (!(config->present & (1u << (8u + i)))) continue;
        const struct snag_binary_result_value *value = &config->values[i];
        struct snag_binary_result_value checked;
        if (snag_binary_result_value_decode(value->data, value->size, &checked) < 0 ||
            snag_buf_append(out, checked.data, checked.size) < 0) {
            return -1;
        }
    }
    if ((config->present & SNAG_BINARY_TURN_PARALLEL_CALLS) &&
        write_uint(out, config->parallel_calls, 1u) < 0) {
        return -1;
    }
    if (!config->extensions.data && !config->extensions.size) {
        const unsigned char empty[] = {SNAG_BINARY_RESULT_OBJECT, SNAG_BINARY_RESULT_VALUE_END};
        return snag_buf_append(out, empty, sizeof(empty));
    }
    if (!config->extensions.data || config->extensions.size > SNAG_MAX_EVENT_LINE) {
        return invalid();
    }
    struct fields extensions = {.data = config->extensions.data, .size = config->extensions.size};
    struct snag_binary_result_value checked;
    if (!read_extensions(&extensions, &checked, turn_config_named_field) ||
        extensions.offset != extensions.size) {
        return invalid();
    }
    return snag_buf_append(out, checked.data, checked.size);
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
    for (size_t i = 0u; i < SNAG_BINARY_TURN_NUMBER_COUNT; ++i) {
        if ((config->present & (1u << i)) && !read_uint(fields, 8u, &config->numbers[i])) {
            return false;
        }
    }
    for (size_t i = 0u; i < SNAG_BINARY_TURN_VALUE_COUNT; ++i) {
        if ((config->present & (1u << (8u + i))) &&
            read_result_value(fields, 3u, &config->values[i], NULL) < 0) {
            return false;
        }
    }
    if (config->present & SNAG_BINARY_TURN_PARALLEL_CALLS) {
        if (!read_uint(fields, 1u, &value) || value > 1u) return false;
        config->parallel_calls = value != 0u;
    }
    return read_extensions(fields, &config->extensions, turn_config_named_field);
}

static bool
read_voice_body(struct fields *fields, struct snag_binary_voice_body *out)
{
    uint64_t kind;
    uint64_t mask;
    if (!read_uint(fields, 1u, &kind) || kind < SNAG_BINARY_VOICE_STARTED ||
        kind > SNAG_BINARY_VOICE_MUTED || !read_uint(fields, 8u, &mask) ||
        (mask >> VOICE_FIELD_COUNT)) {
        return false;
    }
    out->kind = (enum snag_binary_voice_kind)kind;
    for (size_t i = 0u; i < VOICE_FIELD_COUNT; ++i) {
        if (!(mask & (UINT64_C(1) << i))) continue;
        struct snag_binary_result_value *value =
            (void *)((unsigned char *)&out->fields + voice_fields[i].offset);
        if (read_result_value(fields, 3u, value, NULL) < 0) return false;
    }
    return read_extensions(fields, &out->extensions, voice_named_field);
}

int
snag_binary_voice_body_decode(const void *data, size_t size, struct snag_binary_voice_body *out)
{
    if (!data || !size || size > SNAG_MAX_EVENT_LINE || !out) return invalid();
    struct fields fields = {.data = data, .size = size};
    struct snag_binary_voice_body decoded = {0};
    if (!read_voice_body(&fields, &decoded) || fields.offset != size) {
        return invalid();
    }
    *out = decoded;
    return 0;
}

static int
encode_voice_body(struct snag_buf *out, const struct snag_binary_voice_body *body)
{
    if (body->kind < SNAG_BINARY_VOICE_STARTED || body->kind > SNAG_BINARY_VOICE_MUTED) {
        return invalid();
    }
    uint64_t mask = 0u;
    for (size_t i = 0u; i < VOICE_FIELD_COUNT; ++i) {
        const struct snag_binary_result_value *value = voice_field_view(&body->fields, i);
        if (!!value->data != !!value->size) return invalid();
        if (value->data) mask |= UINT64_C(1) << i;
    }
    if (write_uint(out, body->kind, 1u) < 0 || write_uint(out, mask, 8u) < 0) {
        return -1;
    }
    for (size_t i = 0u; i < VOICE_FIELD_COUNT; ++i) {
        const struct snag_binary_result_value *value = voice_field_view(&body->fields, i);
        if (!value->data) continue;
        struct snag_binary_result_value checked;
        if (snag_binary_result_value_decode(value->data, value->size, &checked) < 0 ||
            snag_buf_append(out, value->data, value->size) < 0) {
            return -1;
        }
    }
    if (!body->extensions.data && !body->extensions.size) {
        const unsigned char empty[] = {SNAG_BINARY_RESULT_OBJECT, SNAG_BINARY_RESULT_VALUE_END};
        return snag_buf_append(out, empty, sizeof(empty));
    }
    if (!body->extensions.data || body->extensions.size > SNAG_MAX_EVENT_LINE) {
        return invalid();
    }
    struct fields extensions = {.data = body->extensions.data, .size = body->extensions.size};
    struct snag_binary_result_value checked;
    if (!read_extensions(&extensions, &checked, voice_named_field) ||
        extensions.offset != extensions.size) {
        return invalid();
    }
    return snag_buf_append(out, checked.data, checked.size);
}

int
snag_binary_voice_body_encode(struct snag_buf *out, const json_t *body)
{
    if (!out || !json_is_object(body)) return invalid();
    const json_t *type = json_object_get(body, "type");
    const char *name = json_string_value(type);
    size_t kind = 0u;
    for (size_t i = 0u; name && i < sizeof(voice_kind_names) / sizeof(voice_kind_names[0]); ++i) {
        if (json_string_length(type) == strlen(voice_kind_names[i]) &&
            !memcmp(name, voice_kind_names[i], json_string_length(type))) {
            kind = i + 1u;
            break;
        }
    }
    if (!kind) return invalid();
    json_t *extensions = json_object();
    if (!extensions) return snag_errno(ENOMEM);
    int rc = -1;
    struct snag_buf encoded = {.max = SNAG_MAX_EVENT_LINE};
    for (void *iter = json_object_iter((json_t *)body); iter;
        iter = json_object_iter_next((json_t *)body, iter)) {
        struct snag_binary_text key = {.data = (const unsigned char *)json_object_iter_key(iter),
            .size = json_object_iter_key_len(iter)};
        if (!snag_utf8_valid(key.data, key.size, true)) {
            invalid();
            goto done;
        }
        if (!voice_named_field(key) && json_object_set(extensions,
                (const char *)key.data, json_object_iter_value(iter)) < 0) {
            snag_errno(ENOMEM);
            goto done;
        }
    }
    uint64_t mask = 0u;
    for (size_t i = 0u; i < VOICE_FIELD_COUNT; ++i) {
        if (json_object_get(body, voice_fields[i].name)) {
            mask |= UINT64_C(1) << i;
        }
    }
    if (write_uint(&encoded, kind, 1u) < 0 || write_uint(&encoded, mask, 8u) < 0) {
        goto done;
    }
    for (size_t i = 0u; i < VOICE_FIELD_COUNT; ++i) {
        const json_t *value = json_object_get(body, voice_fields[i].name);
        if (value && write_result_value(&encoded, value, 3u) < 0) goto done;
    }
    if (write_result_value(&encoded, extensions, 2u) < 0) goto done;
    struct snag_binary_voice_body checked;
    if (snag_binary_voice_body_decode(encoded.data, encoded.len, &checked) < 0) {
        goto done;
    }
    rc = snag_buf_append(out, encoded.data, encoded.len);
 done:
    snag_buf_free(&encoded);
    json_decref(extensions);
    return rc;
}

int
snag_binary_voice_body_json(const struct snag_binary_voice_body *body, json_t **out)
{
    if (!body || !out) return invalid();
    /* Share persisted admission and ignore untrusted cached value metadata. */
    struct snag_buf encoded = {.max = SNAG_MAX_EVENT_LINE};
    struct snag_binary_voice_body checked;
    json_t *object = NULL;
    int rc = -1;
    if (encode_voice_body(&encoded, body) < 0 ||
        snag_binary_voice_body_decode(encoded.data, encoded.len, &checked) < 0) {
        goto done;
    }
    struct fields extensions = {.data = checked.extensions.data, .size = checked.extensions.size};
    if (read_result_value(&extensions, 2u, NULL, &object) < 0) goto done;
    if (json_object_set_new(object, "type", json_string(voice_kind_names[checked.kind - 1u])) < 0) {
        snag_errno(ENOMEM);
        goto done;
    }
    for (size_t i = 0u; i < VOICE_FIELD_COUNT; ++i) {
        const struct snag_binary_result_value *field = voice_field_view(&checked.fields, i);
        if (!field->data) continue;
        json_t *value = NULL;
        if (snag_binary_result_value_json(field, &value) < 0) goto done;
        if (json_object_set_new(object, voice_fields[i].name, value) < 0) {
            snag_errno(ENOMEM);
            goto done;
        }
    }
    *out = object;
    object = NULL;
    rc = 0;
 done:
    json_decref(object);
    snag_buf_free(&encoded);
    return rc;
}

static int
encode_voice_event(struct snag_buf *out, const struct snag_binary_voice_event *value)
{
    if (snag_buf_append(out, value->connection, 16u) < 0 ||
        write_text(out, value->provider, 1u, SNAG_CONFIG_PROVIDER_NAME_MAX) < 0 ||
        write_text(out, value->model, 1u, SNAG_MODEL_MAX_BYTES - 1u) < 0) {
        return -1;
    }
    return encode_voice_body(out, &value->body);
}

static bool
decode_voice_event(struct fields *fields, struct snag_binary_voice_event *out)
{
    return read_id(fields, out->connection) &&
        read_text(fields, &out->provider, 1u, SNAG_CONFIG_PROVIDER_NAME_MAX) &&
        read_text(fields, &out->model, 1u, SNAG_MODEL_MAX_BYTES - 1u) &&
        read_voice_body(fields, &out->body);
}

static int
encode_audio_usage(struct snag_buf *out, const struct snag_binary_audio_usage *value)
{
    if (write_text(out, value->provider, 1u, SNAG_CONFIG_PROVIDER_NAME_MAX) < 0 ||
        write_text(out, value->model, 1u, SNAG_MODEL_MAX_BYTES - 1u) < 0) {
        return -1;
    }
    return write_text(out, value->report, 1u, 256u * 1024u - 1u);
}

static bool
decode_audio_usage(struct fields *fields, struct snag_binary_audio_usage *out)
{
    return read_text(fields, &out->provider, 1u, SNAG_CONFIG_PROVIDER_NAME_MAX) &&
        read_text(fields, &out->model, 1u, SNAG_MODEL_MAX_BYTES - 1u) &&
        read_text(fields, &out->report, 1u, 256u * 1024u - 1u);
}

/* Frozen public-archive field positions. New positions need a new enclosed version. */
struct archive_schema {
    uint16_t kind;
    const char *const *names;
    size_t count;
};

#define ARCHIVE_SCHEMA(kind, ...) \
    {kind, (const char *const[]){__VA_ARGS__}, \
     sizeof((const char *const[]){__VA_ARGS__}) / sizeof(const char *)}
static const struct archive_schema archive_schemas[] = {
    ARCHIVE_SCHEMA(SNAG_BINARY_SESSION_CREATED, "cwd", "default_effort", "default_model",
        "default_provider", "format", "protocol", "workspace"),
    ARCHIVE_SCHEMA(SNAG_BINARY_CWD_CHANGED, "new_cwd", "old_cwd"),
    ARCHIVE_SCHEMA(SNAG_BINARY_SESSION_ARCHIVED, "origin"),
    ARCHIVE_SCHEMA(SNAG_BINARY_SESSION_UNARCHIVED, "origin"),
    ARCHIVE_SCHEMA(SNAG_BINARY_SESSION_DELETE_REQUESTED, "confirmed_id_prefix", "trash_name"),
    ARCHIVE_SCHEMA(SNAG_BINARY_BANNER_UPDATED, "text"),
    ARCHIVE_SCHEMA(SNAG_BINARY_STEERING_UPDATED, "mode"),
    ARCHIVE_SCHEMA(SNAG_BINARY_MODEL_SELECTED, "new_effort", "new_model", "new_provider",
        "old_effort", "old_model", "old_provider"),
    ARCHIVE_SCHEMA(SNAG_BINARY_TURN_MODEL_CHANGED, "new_effort", "old_effort", "old_model",
        "old_provider", "turn_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_EFFORT_CHANGED, "new_effort", "old_effort"),
    ARCHIVE_SCHEMA(SNAG_BINARY_CONTEXT_SELECTION_CHANGED, "new_mode", "new_tokens", "old_mode",
        "old_tokens"),
    ARCHIVE_SCHEMA(SNAG_BINARY_COMMAND_SHELL_CHANGED, "shell"),
    ARCHIVE_SCHEMA(SNAG_BINARY_SESSION_NAMED, "name"),
    ARCHIVE_SCHEMA(SNAG_BINARY_SESSION_OPTIONS, "args"),
    ARCHIVE_SCHEMA(SNAG_BINARY_SERVICE_TIER_CHANGED, "value"),
    ARCHIVE_SCHEMA(SNAG_BINARY_RETRY_AUTO_CHANGED, "value"),
    ARCHIVE_SCHEMA(SNAG_BINARY_FALLBACK_CHANGED, "value"),
    ARCHIVE_SCHEMA(SNAG_BINARY_TURN_FALLBACK_STARTED, "turn_id", "provider", "model", "effort",
        "context_mode", "context_tokens"),
    ARCHIVE_SCHEMA(SNAG_BINARY_CONTROL_REQUESTED, "control", "origin", "source_seq"),
    ARCHIVE_SCHEMA(SNAG_BINARY_CONTROL_STARTED, "control"),
    ARCHIVE_SCHEMA(SNAG_BINARY_CONTROL_FINISHED, "control"),
    ARCHIVE_SCHEMA(SNAG_BINARY_TIMER_SCHEDULED, "due_ms", "text", "timer_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_TIMER_FIRED, "timer_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_TIMER_CANCELLED, "timer_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_GOAL_STARTED, "goal_id", "prompt"),
    ARCHIVE_SCHEMA(SNAG_BINARY_GOAL_REPLACED, "actor", "goal_id", "new_goal_id", "prompt"),
    ARCHIVE_SCHEMA(SNAG_BINARY_GOAL_REWORDED, "actor", "goal_id", "prompt"),
    ARCHIVE_SCHEMA(SNAG_BINARY_GOAL_LOCK_CHANGED, "goal_id", "locked"),
    ARCHIVE_SCHEMA(SNAG_BINARY_GOAL_PAUSED, "goal_id", "reason"),
    ARCHIVE_SCHEMA(SNAG_BINARY_GOAL_BLOCKED, "actor", "goal_id", "reason"),
    ARCHIVE_SCHEMA(SNAG_BINARY_GOAL_BLOCKED_WAIT_FOR, "actor", "goal_id", "reason", "wait_for"),
    ARCHIVE_SCHEMA(SNAG_BINARY_GOAL_COMPLETED, "actor", "goal_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_GOAL_RESUMED, "goal_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_GOAL_CANCELLED, "goal_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_INPUT_RECEIVED, "content", "effort", "instructions", "model",
        "origin", "provider", "read_only", "received_at_ms", "text"),
    {SNAG_BINARY_INPUT_CANCELLED, NULL, 0u},
    ARCHIVE_SCHEMA(SNAG_BINARY_STEERING_ADDED, "content", "received_at_ms", "steering_id", "text",
        "turn_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_IRC_REPLY_REMINDER, "content", "received_at_ms", "steering_id",
        "text", "turn_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_STEERING_DEFERRED, "turn_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_INPUT_ADMITTED, "steering_ids", "time_ms", "turn_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_FUTURE_QUEUE_STATE, "armed"),
    ARCHIVE_SCHEMA(SNAG_BINARY_FUTURE_TURN_CANCELLED, "queue_ids", "reason"),
    ARCHIVE_SCHEMA(SNAG_BINARY_FUTURE_TURN_QUEUED, "armed", "content", "queue_id", "read_only",
        "received_at_ms", "text", "voice", "while_turn_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_FUTURE_TURN_EDITED, "armed", "content", "queue_id", "read_only",
        "received_at_ms", "text"),
    ARCHIVE_SCHEMA(SNAG_BINARY_TURN_STARTED, "config", "content", "cwd", "input_kind",
        "instructions", "queue_id", "queue_seq", "read_only", "received_at_ms", "text", "turn_id",
        "turn_number", "workspace"),
    ARCHIVE_SCHEMA(SNAG_BINARY_TURN_YIELD_REQUESTED, "turn_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_TURN_CANCEL_REQUESTED, "turn_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_TURN_RECOVERY, "class", "message", "retry_attempts", "turn_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_TURN_COMPLETED, "final_item_id", "final_response_id", "turn_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_TURN_COMPLETED_SILENT, "reason", "response_id", "turn_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_TURN_INTERRUPTED, "origin", "reason", "turn_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_TURN_FAILED, "class", "message", "turn_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_RESPONSE_STARTED, "baseline_sha256", "capability_version",
        "capacity_source", "compact_id", "count_method", "count_request_sha256", "cycle",
        "effort", "hard_input_tokens", "host_context", "input_tokens_bound", "irc_seq", "model",
        "model_input_bytes", "model_input_sha256", "profile_id", "provider",
        "provider_source_sha256", "request_input_bytes", "request_input_count",
        "request_input_sha256", "request_sha256", "requested_output_tokens", "response_id",
        "source_bound", "steering_ids", "turn_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_RESPONSE_OUTPUT, "cycle", "index", "item", "offset", "response_id",
        "turn_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_RESPONSE_INTERRUPTED, "cycle", "origin", "partial_public",
        "reason", "response_id", "turn_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_RESPONSE_FAILED, "class", "cycle", "message", "new_input",
        "partial_public", "policy", "policy_stopped", "response_id", "retry_count", "turn_id",
        "turn_retry_attempts"),
    ARCHIVE_SCHEMA(SNAG_BINARY_RESPONSE_OUTPUT_CORRECTION, "correction_id", "cycle",
        "partial_public", "response_id", "text", "turn_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_RESPONSE_COMPLETED, "continuation", "continuation_scope", "cycle",
        "items", "provider_payload_omitted", "provider_response_id", "response_id", "status",
        "turn_id", "usage"),
    ARCHIVE_SCHEMA(SNAG_BINARY_RESPONSE_CAPACITY_REJECTED, "code", "context_limit_tokens",
        "cycle", "message", "observed_hard_input_tokens", "provider_source_sha256",
        "request_sha256", "requested_input_tokens", "response_id", "turn_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_TOOL_STARTED, "action_sha256", "call_id", "resolved_workdir",
        "turn_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_HOSTED_SEARCH_STARTED, "action", "item_id", "turn_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_HOSTED_SEARCH_FINISHED, "item_id", "sources", "status",
        "turn_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_TOOL_FINISHED, "call_id", "result", "turn_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_PROCESS_OUTPUT, "data", "encoding", "handle", "offset", "stream",
        "turn_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_PROCESS_CLOSED, "cause", "handle", "result", "turn_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_IRC_EVENT, "endpoint", "historical", "input", "kind", "local",
        "nick", "op", "reply", "room", "sequence", "stream", "text", "timestamp_ms", "urgent"),
    ARCHIVE_SCHEMA(SNAG_BINARY_IRC_SNAPSHOT, "reason", "text", "timestamp_ms"),
    ARCHIVE_SCHEMA(SNAG_BINARY_IRC_ADMITTED, "input", "sequences", "steering"),
    ARCHIVE_SCHEMA(SNAG_BINARY_IRC_SLEEP_SET, "until_ms", "messages"),
    ARCHIVE_SCHEMA(SNAG_BINARY_IRC_SLEEP_WOKE, "reason"),
    ARCHIVE_SCHEMA(SNAG_BINARY_IRC_COMPACT_CONFIGURED, "after_updates", "instruction"),
    ARCHIVE_SCHEMA(SNAG_BINARY_IRC_COMPACTED, "through_seq", "count", "summary"),
    ARCHIVE_SCHEMA(SNAG_BINARY_COMPACTION_STARTED, "capability_version", "compact_id",
        "compaction_model", "continuation_scope", "count_method", "count_request_sha256",
        "input_tokens_bound", "model", "predecessor_compact_id", "profile_id", "reason",
        "request_sha256", "source_seq", "source_sha256"),
    ARCHIVE_SCHEMA(SNAG_BINARY_COMPACTION_INTERRUPTED, "compact_id", "reason"),
    ARCHIVE_SCHEMA(SNAG_BINARY_COMPACTION_COMPLETED, "compact_id", "continuation_scope",
        "count_method", "input_tokens_bound", "output", "output_count_method",
        "output_count_request_sha256", "output_sha256", "output_tokens_bound",
        "provider_payload_omitted", "source_sha256"),
    ARCHIVE_SCHEMA(SNAG_BINARY_CONTEXT_REBASED, "reason", "turn_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_DOWNLOAD_QUEUED, "bytes", "id", "mtime", "name", "path",
        "queued_ms", "sha256"),
    ARCHIVE_SCHEMA(SNAG_BINARY_DOWNLOAD_REMOVED, "id", "reason"),
    ARCHIVE_SCHEMA(SNAG_BINARY_DOWNLOADS_CLEARED, "reason"),
    ARCHIVE_SCHEMA(SNAG_BINARY_RULE_LOG, "chain", "message", "rule"),
    ARCHIVE_SCHEMA(SNAG_BINARY_RULE_TRANSFORM, "call_id", "effective_sha256", "original_sha256",
        "rule"),
    ARCHIVE_SCHEMA(SNAG_BINARY_AUDIO_USAGE, "model", "operation", "provider", "report"),
    ARCHIVE_SCHEMA(SNAG_BINARY_VOICE_EVENT, "connection_id", "event", "model", "provider"),
    ARCHIVE_SCHEMA(SNAG_BINARY_VOICE_TRANSFER_ADOPTED, "begin_offset", "begin_seq",
        "begin_sha256", "count", "session_boundary", "source_as_of_seq", "source_session_id",
        "target_session_id", "transfer_id"),
    ARCHIVE_SCHEMA(SNAG_BINARY_ARCHIVE_CHECKPOINT_KIND, "covers_through_seq", "provider_view",
        "snapshot_v"),
};
#undef ARCHIVE_SCHEMA

static const struct archive_schema *
archive_schema_find(uint16_t kind)
{
    static const struct archive_schema unassigned = {0};
    if (!kind) return &unassigned;
    for (size_t i = 0u; i < sizeof(archive_schemas) / sizeof(archive_schemas[0]); ++i) {
        if (archive_schemas[i].kind == kind) return &archive_schemas[i];
    }
    return NULL;
}

static const char *
archive_schema_name(const struct archive_schema *schema)
{
    return schema->kind == SNAG_BINARY_ARCHIVE_CHECKPOINT_KIND ? "session_checkpoint" :
        snag_binary_event_name((enum snag_binary_kind)schema->kind);
}

static bool
archive_text_is(struct snag_binary_text text, const char *name)
{
    return name && text.size == strlen(name) && !memcmp(text.data, name, text.size);
}

static bool
archive_type_forbidden(struct snag_binary_text type)
{
    return archive_text_is(type, "voice_transfer_record") ||
        archive_text_is(type, "voice_transfer_sealed");
}

static const struct archive_schema *
archive_schema_type(struct snag_binary_text type)
{
    for (size_t i = 0u; i < sizeof(archive_schemas) / sizeof(archive_schemas[0]); ++i) {
        if (archive_text_is(type, archive_schema_name(&archive_schemas[i]))) {
            return &archive_schemas[i];
        }
    }
    return archive_schema_find(0u);
}

static bool
archive_named_field(const struct archive_schema *schema, struct snag_binary_text key)
{
    for (size_t i = 0u; i < schema->count; ++i) {
        if (archive_text_is(key, schema->names[i])) return true;
    }
    return false;
}

const char *
snag_binary_archive_field_name(uint16_t kind, size_t index)
{
    const struct archive_schema *schema = archive_schema_find(kind);
    return schema && index < schema->count ? schema->names[index] : NULL;
}

int
snag_binary_archive_fields_decode(uint16_t kind, const void *data, size_t size,
                                  struct snag_binary_archive_fields *out)
{
    const struct archive_schema *schema = archive_schema_find(kind);
    if (!data || !out || !schema || schema->count > SNAG_BINARY_ARCHIVE_FIELDS_MAX ||
        size > SNAG_MAX_EVENT_LINE) {
        return invalid();
    }
    struct fields fields = {.data = data, .size = size};
    struct snag_binary_archive_fields decoded = {.kind = kind, .data = data, .size = size};
    if (!kind) {
        if (!read_text(&fields, &decoded.type, 1u, SNAG_MAX_EVENT_LINE) ||
            archive_type_forbidden(decoded.type) || archive_schema_type(decoded.type)->kind) {
            return invalid();
        }
    } else {
        const char *name = archive_schema_name(schema);
        decoded.type = (struct snag_binary_text){(const unsigned char *)name, strlen(name)};
    }
    size_t mask_bytes = (schema->count + 7u) / 8u;
    if ((mask_bytes && !read_uint(&fields, mask_bytes, &decoded.present)) ||
        (schema->count < 64u && (decoded.present >> schema->count))) {
        return invalid();
    }
    for (size_t i = 0u; i < schema->count; ++i) {
        if ((decoded.present & (UINT64_C(1) << i)) &&
            read_result_value(&fields, 3u, &decoded.values[i], NULL) < 0) {
            return -1;
        }
    }
    if (read_result_value(&fields, 2u, &decoded.extensions, NULL) < 0) return -1;
    if (decoded.extensions.kind != SNAG_BINARY_RESULT_OBJECT || fields.offset != fields.size) {
        return invalid();
    }
    struct fields keys = {.data = decoded.extensions.data, .size = decoded.extensions.size,
        .offset = 1u};
    while (keys.offset + 1u < keys.size) {
        uint64_t tag;
        struct snag_binary_text key;
        if (!read_uint(&keys, 1u, &tag) || tag != SNAG_BINARY_RESULT_STRING ||
            !read_result_string(&keys, &key) || archive_named_field(schema, key)) {
            return invalid();
        }
        if (read_result_value(&keys, 3u, NULL, NULL) < 0) return -1;
    }
    if (keys.offset != keys.size - 1u) return invalid();
    *out = decoded;
    return 0;
}

int
snag_binary_archive_fields_encode(struct snag_buf *out, const char *type,
                                  const json_t *data, uint16_t *kind)
{
    if (!out || !type || !json_is_object(data) || !kind) return invalid();
    struct snag_binary_text name = {(const unsigned char *)type, strlen(type)};
    if (!text_valid(name, 1u, SNAG_MAX_EVENT_LINE) || archive_type_forbidden(name)) {
        return invalid();
    }
    const struct archive_schema *schema = archive_schema_type(name);
    if (!schema->kind) {
        enum snag_binary_kind assigned;
        if (!snag_binary_event_kind(type, &assigned)) return snag_errno(ENOTSUP);
    }
    if (schema->count > SNAG_BINARY_ARCHIVE_FIELDS_MAX) return invalid();
    json_t *extensions = json_copy((json_t *)data);
    if (!extensions) return snag_errno(ENOMEM);
    struct snag_buf encoded = {.max = SNAG_MAX_EVENT_LINE};
    int rc = -1;
    uint64_t mask = 0u;
    for (size_t i = 0u; i < schema->count; ++i) {
        if (json_object_get(data, schema->names[i])) {
            mask |= UINT64_C(1) << i;
            (void)json_object_del(extensions, schema->names[i]);
        }
    }
    size_t mask_bytes = (schema->count + 7u) / 8u;
    if ((!schema->kind && write_text(&encoded, name, 1u, SNAG_MAX_EVENT_LINE) < 0) ||
        (mask_bytes && write_uint(&encoded, mask, mask_bytes) < 0)) {
        goto done;
    }
    for (size_t i = 0u; i < schema->count; ++i) {
        if ((mask & (UINT64_C(1) << i)) &&
            write_result_value(&encoded, json_object_get(data, schema->names[i]), 3u) < 0) {
            goto done;
        }
    }
    if (write_result_value(&encoded, extensions, 2u) < 0) goto done;
    struct snag_binary_archive_fields checked;
    if (snag_binary_archive_fields_decode(schema->kind, encoded.data, encoded.len, &checked) < 0) {
        goto done;
    }
    rc = snag_buf_append(out, encoded.data, encoded.len);
    if (!rc) *kind = schema->kind;
 done:
    json_decref(extensions);
    snag_buf_free(&encoded);
    return rc;
}

int
snag_binary_archive_fields_json(const struct snag_binary_archive_fields *fields, json_t **out)
{
    if (!fields || !out) return invalid();
    struct snag_binary_archive_fields checked;
    if (snag_binary_archive_fields_decode(fields->kind, fields->data, fields->size, &checked) < 0) {
        return -1;
    }
    json_t *data = NULL;
    if (snag_binary_rule_value_json(&checked.extensions, &data) < 0) return -1;
    const struct archive_schema *schema = archive_schema_find(checked.kind);
    for (size_t i = 0u; i < schema->count; ++i) {
        if (!(checked.present & (UINT64_C(1) << i))) continue;
        json_t *value = NULL;
        if (snag_binary_result_value_json(&checked.values[i], &value) < 0 ||
            snag_json_set_new(data, schema->names[i], value) < 0) {
            json_decref(data);
            return -1;
        }
    }
    *out = data;
    return 0;
}

static int
voice_archive_validate(const struct snag_binary_voice_archive *value)
{
    if (!value->source_seq || value->source_seq > INT64_MAX ||
        value->source_kind == SNAG_BINARY_VOICE_TRANSFER_RECORD ||
        value->source_kind == SNAG_BINARY_VOICE_TRANSFER_SEALED) {
        return invalid();
    }
    if (value->source_version == SNAG_BINARY_ARCHIVE_PUBLIC_VERSION) {
        struct snag_binary_archive_fields decoded;
        return snag_binary_archive_fields_decode(value->source_kind,
            value->data, value->size, &decoded);
    }
    struct snag_binary_record source = {.kind = value->source_kind,
        .version = value->source_version, .payload = value->data, .size = value->size};
    struct snag_binary_event decoded;
    return snag_binary_event_decode(&source, &decoded);
}

static int
encode_voice_archive(struct snag_buf *out, const struct snag_binary_voice_archive *value)
{
    if (voice_archive_validate(value) != 0) return -1;
    if (snag_buf_append(out, value->id, 16u) < 0 ||
        snag_buf_append(out, value->target, 16u) < 0 ||
        snag_buf_append(out, value->source, 16u) < 0 ||
        write_uint(out, value->source_seq, 8u) < 0 ||
        write_uint(out, value->source_kind, 2u) < 0 ||
        write_uint(out, value->source_version, 2u) < 0 ||
        write_uint(out, value->size, 4u) < 0) {
        return -1;
    }
    return snag_buf_append(out, value->data, value->size);
}

static bool
decode_voice_archive(struct fields *fields, struct snag_binary_voice_archive *out)
{
    uint64_t kind;
    uint64_t version;
    uint64_t size;
    if (!read_id(fields, out->id) || !read_id(fields, out->target) ||
        !read_id(fields, out->source) || !read_uint(fields, 8u, &out->source_seq) ||
        !read_uint(fields, 2u, &kind) || !read_uint(fields, 2u, &version) ||
        !read_uint(fields, 4u, &size) || size > fields->size - fields->offset) {
        return false;
    }
    out->source_kind = (uint16_t)kind;
    out->source_version = (uint16_t)version;
    out->data = fields->data + fields->offset;
    out->size = (size_t)size;
    fields->offset += out->size;
    return voice_archive_validate(out) == 0;
}

static bool
voice_transfer_valid(const struct snag_binary_voice_transfer *value)
{
    return value->source_as_of && value->source_as_of <= INT64_MAX &&
        value->count && value->count <= INT64_MAX;
}

static int
encode_voice_transfer(struct snag_buf *out, const struct snag_binary_voice_transfer *value)
{
    if (!voice_transfer_valid(value)) return invalid();
    if (snag_buf_append(out, value->id, 16u) < 0 ||
        snag_buf_append(out, value->target, 16u) < 0 ||
        snag_buf_append(out, value->source, 16u) < 0 ||
        write_uint(out, value->source_as_of, 8u) < 0) {
        return -1;
    }
    return write_uint(out, value->count, 8u);
}

static bool
decode_voice_transfer(struct fields *fields, struct snag_binary_voice_transfer *out)
{
    return read_id(fields, out->id) && read_id(fields, out->target) &&
        read_id(fields, out->source) && read_uint(fields, 8u, &out->source_as_of) &&
        read_uint(fields, 8u, &out->count) && voice_transfer_valid(out);
}

static int
encode_voice_adopted(struct snag_buf *out, const struct snag_binary_voice_adopted *value)
{
    static const unsigned char zero[32] = {0};
    if (value->begin_seq < 2u || value->begin_seq > INT64_MAX ||
        (value->native ? (value->begin_offset || memcmp(value->begin_sha256, zero, 32u)) :
        (!value->begin_offset || value->begin_offset > INT64_MAX))) {
        return invalid();
    }
    if (write_uint(out, value->native, 1u) < 0 ||
        encode_voice_transfer(out, &value->transfer) < 0) return -1;
    if (value->native) return write_uint(out, value->begin_seq, 8u);
    if (write_uint(out, value->begin_offset, 8u) < 0 ||
        write_uint(out, value->begin_seq, 8u) < 0) {
        return -1;
    }
    return snag_buf_append(out, value->begin_sha256, 32u);
}

static bool
decode_voice_adopted(struct fields *fields, struct snag_binary_voice_adopted *out)
{
    uint64_t native = 0u;
    if (fields->references && (!read_uint(fields, 1u, &native) || native > 1u)) return false;
    out->native = native != 0u;
    if (!decode_voice_transfer(fields, &out->transfer)) return false;
    if (out->native) {
        return read_uint(fields, 8u, &out->begin_seq) && out->begin_seq >= 2u &&
            out->begin_seq <= INT64_MAX;
    }
    return read_uint(fields, 8u, &out->begin_offset) && out->begin_offset &&
        out->begin_offset <= INT64_MAX && read_uint(fields, 8u, &out->begin_seq) &&
        out->begin_seq >= 2u && out->begin_seq <= INT64_MAX &&
        read_bytes(fields, out->begin_sha256, 32u);
}

static int
encode_rule_log(struct snag_buf *out, const struct snag_binary_rule_log *value)
{
    const struct snag_binary_result_value *fields[] = {
        &value->chain, &value->message, &value->rule};
    for (size_t i = 0u; i < 3u; ++i) {
        struct snag_binary_result_value checked;
        if (snag_binary_rule_value_decode(fields[i]->data, fields[i]->size, &checked) < 0 ||
            snag_buf_append(out, fields[i]->data, fields[i]->size) < 0) {
            return -1;
        }
    }
    return 0;
}

static bool
decode_rule_log(struct fields *fields, struct snag_binary_rule_log *out)
{
    return read_result_value(fields, 2u, &out->chain, NULL) == 0 &&
        read_result_value(fields, 2u, &out->message, NULL) == 0 &&
        read_result_value(fields, 2u, &out->rule, NULL) == 0;
}

static int
encode_download(struct snag_buf *out, const struct snag_binary_download *value)
{
    if (value->bytes > INT64_MAX || value->mtime > INT64_MAX || value->queued_ms > INT64_MAX) {
        return invalid();
    }
    if (snag_buf_append(out, value->id, 16u) < 0 ||
        snag_buf_append(out, value->sha256, 32u) < 0 || write_uint(out, value->bytes, 8u) < 0 ||
        write_uint(out, value->mtime, 8u) < 0 || write_uint(out, value->queued_ms, 8u) < 0 ||
        write_text(out, value->path, 1u, SNAG_PATH_MAX_BYTES) < 0) {
        return -1;
    }
    /* NAME_MAX and root syntax are source-platform admission rules. */
    return write_text(out, value->name, 1u, SNAG_MAX_EVENT_LINE);
}

static bool
decode_download(struct fields *fields, struct snag_binary_download *out)
{
    return read_id(fields, out->id) && read_bytes(fields, out->sha256, 32u) &&
        read_uint(fields, 8u, &out->bytes) && out->bytes <= INT64_MAX &&
        read_uint(fields, 8u, &out->mtime) && out->mtime <= INT64_MAX &&
        read_uint(fields, 8u, &out->queued_ms) && out->queued_ms <= INT64_MAX &&
        read_text(fields, &out->path, 1u, SNAG_PATH_MAX_BYTES) &&
        read_text(fields, &out->name, 1u, SNAG_MAX_EVENT_LINE);
}

static bool
compact_count_valid(enum snag_binary_count_method method, uint64_t tokens, bool start)
{
    return method >= SNAG_BINARY_COUNT_EXACT && method <= SNAG_BINARY_COUNT_QUALIFIED_UPPER_BOUND &&
        (start || method != SNAG_BINARY_COUNT_ANCHORED_UPPER_BOUND) && tokens <= INT64_MAX &&
        (method == SNAG_BINARY_COUNT_UNKNOWN ? tokens == 0u : tokens != 0u);
}

static bool
compact_start_valid(const struct snag_binary_compact_start *value)
{
    return value->source_seq && value->source_seq <= INT64_MAX &&
        value->reason >= SNAG_BINARY_COMPACT_MANUAL &&
        value->reason <= SNAG_BINARY_COMPACT_REDUCE &&
        compact_count_valid(value->count_method, value->input_tokens, true);
}

static int
encode_compact_start(struct snag_buf *out, const struct snag_binary_compact_start *value)
{
    if (!compact_start_valid(value)) return invalid();
    unsigned int flags = (value->has_predecessor ? 1u : 0u) | (value->has_scope ? 2u : 0u) |
        (value->has_compaction_model ? 4u : 0u);
    if (snag_buf_append(out, value->id, 16u) < 0 || write_uint(out, flags, 1u) < 0 ||
        write_uint(out, value->reason, 1u) < 0 || write_uint(out, value->count_method, 1u) < 0 ||
        write_uint(out, value->source_seq, 8u) < 0 ||
        write_uint(out, value->input_tokens, 8u) < 0 ||
        snag_buf_append(out, value->source_sha256, 32u) < 0 ||
        snag_buf_append(out, value->request_sha256, 32u) < 0 ||
        snag_buf_append(out, value->count_request_sha256, 32u) < 0 ||
        write_text(out, value->model, 1u, SNAG_MODEL_MAX_BYTES - 1u) < 0 ||
        write_text(out, value->capability, 1u, SNAG_MAX_EVENT_LINE) < 0 ||
        write_text(out, value->profile, 1u, SNAG_MAX_EVENT_LINE) < 0) {
        return -1;
    }
    if (value->has_predecessor && snag_buf_append(out, value->predecessor, 16u) < 0) {
        return -1;
    }
    if (value->has_scope && snag_buf_append(out, value->scope, 32u) < 0) {
        return -1;
    }
    return value->has_compaction_model ?
        write_text(out, value->compaction_model, 1u, SNAG_MAX_EVENT_LINE) : 0;
}

static bool
decode_compact_start(struct fields *fields, struct snag_binary_compact_start *out)
{
    uint64_t flags;
    uint64_t reason;
    uint64_t method;
    if (!read_id(fields, out->id) || !read_uint(fields, 1u, &flags) || flags > 7u ||
        !read_uint(fields, 1u, &reason) || !read_uint(fields, 1u, &method) ||
        !read_uint(fields, 8u, &out->source_seq) || !read_uint(fields, 8u, &out->input_tokens) ||
        !read_bytes(fields, out->source_sha256, 32u) ||
        !read_bytes(fields, out->request_sha256, 32u) ||
        !read_bytes(fields, out->count_request_sha256, 32u) ||
        !read_text(fields, &out->model, 1u, SNAG_MODEL_MAX_BYTES - 1u) ||
        !read_text(fields, &out->capability, 1u, SNAG_MAX_EVENT_LINE) ||
        !read_text(fields, &out->profile, 1u, SNAG_MAX_EVENT_LINE)) {
        return false;
    }
    out->reason = (enum snag_binary_compact_reason)reason;
    out->count_method = (enum snag_binary_count_method)method;
    out->has_predecessor = (flags & 1u) != 0u;
    out->has_scope = (flags & 2u) != 0u;
    out->has_compaction_model = (flags & 4u) != 0u;
    if (out->has_predecessor && !read_id(fields, out->predecessor)) {
        return false;
    }
    if (out->has_scope && !read_bytes(fields, out->scope, 32u)) return false;
    return (!out->has_compaction_model ||
        read_text(fields, &out->compaction_model, 1u, SNAG_MAX_EVENT_LINE)) &&
        compact_start_valid(out);
}

static int
encode_compact_interrupt(struct snag_buf *out, const struct snag_binary_compact_interrupt *value)
{
    if (value->reason < SNAG_BINARY_COMPACT_STOP_STEERING ||
        value->reason > SNAG_BINARY_COMPACT_STOP_ERROR) {
        return invalid();
    }
    if (snag_buf_append(out, value->id, 16u) < 0) return -1;
    return write_uint(out, value->reason, 1u);
}

static bool
decode_compact_interrupt(struct fields *fields, struct snag_binary_compact_interrupt *out)
{
    uint64_t reason;
    if (!read_id(fields, out->id) || !read_uint(fields, 1u, &reason) ||
        reason < SNAG_BINARY_COMPACT_STOP_STEERING || reason > SNAG_BINARY_COMPACT_STOP_ERROR) {
        return false;
    }
    out->reason = (enum snag_binary_compact_stop_reason)reason;
    return true;
}

static bool
compact_output_valid(const struct snag_binary_compact_complete *value)
{
    const size_t maximum = 12u * 1024u * 1024u; /* Existing provider-output contract. */
    if (!text_valid(value->output, 2u, maximum)) return false;
    json_t *array = snag_json_load_canonical_bounded(value->output.data, value->output.size,
        maximum, NULL, 0u);
    bool valid = json_is_array(array) && json_array_size(array);
    for (size_t i = 0u; valid && i < json_array_size(array); ++i) {
        json_t *item = json_array_get(array, i);
        valid = json_is_object(item) && snag_json_string(item, "type");
    }
    json_decref(array);
    if (!valid) return false;
    struct snag_sha256 hash;
    unsigned char digest[32];
    snag_sha256_init(&hash);
    snag_sha256_update(&hash, value->output.data, value->output.size);
    snag_sha256_final(&hash, digest);
    return !memcmp(digest, value->output_sha256, sizeof(digest));
}

static bool
compact_complete_valid(const struct snag_binary_compact_complete *value)
{
    return compact_count_valid(value->count_method, value->input_tokens, false) &&
        compact_count_valid(value->output_count_method, value->output_tokens, false) &&
        compact_output_valid(value);
}

static int
encode_compact_complete(struct snag_buf *out, const struct snag_binary_compact_complete *value)
{
    if (!compact_complete_valid(value)) return invalid();
    if (snag_buf_append(out, value->id, 16u) < 0 ||
        write_uint(out, value->has_scope ? 1u : 0u, 1u) < 0 ||
        write_uint(out, value->count_method, 1u) < 0 ||
        write_uint(out, value->output_count_method, 1u) < 0 ||
        write_uint(out, value->input_tokens, 8u) < 0 ||
        write_uint(out, value->output_tokens, 8u) < 0 ||
        snag_buf_append(out, value->output_count_request_sha256, 32u) < 0 ||
        snag_buf_append(out, value->source_sha256, 32u) < 0 ||
        snag_buf_append(out, value->output_sha256, 32u) < 0) {
        return -1;
    }
    if (value->has_scope && snag_buf_append(out, value->scope, 32u) < 0) {
        return -1;
    }
    return write_text(out, value->output, 2u, 12u * 1024u * 1024u);
}

static bool
decode_compact_complete(struct fields *fields, struct snag_binary_compact_complete *out)
{
    uint64_t scope;
    uint64_t method;
    uint64_t output_method;
    if (!read_id(fields, out->id) || !read_uint(fields, 1u, &scope) || scope > 1u ||
        !read_uint(fields, 1u, &method) || !read_uint(fields, 1u, &output_method) ||
        !read_uint(fields, 8u, &out->input_tokens) || !read_uint(fields, 8u, &out->output_tokens) ||
        !read_bytes(fields, out->output_count_request_sha256, 32u) ||
        !read_bytes(fields, out->source_sha256, 32u) ||
        !read_bytes(fields, out->output_sha256, 32u)) {
        return false;
    }
    out->count_method = (enum snag_binary_count_method)method;
    out->output_count_method = (enum snag_binary_count_method)output_method;
    out->has_scope = scope != 0u;
    if (out->has_scope && !read_bytes(fields, out->scope, 32u)) return false;
    return read_text(fields, &out->output, 2u, 12u * 1024u * 1024u) && compact_complete_valid(out);
}

static bool
control_kind(enum snag_binary_kind kind)
{
    return kind >= SNAG_BINARY_CONTROL_REQUESTED && kind <= SNAG_BINARY_CONTROL_FINISHED;
}

static bool
control_valid(enum snag_binary_kind kind, const struct snag_binary_control *value)
{
    if (!value->control || value->control > SNAG_CONTROL_RELOAD ||
        (value->control & (value->control - 1u)) || value->source_seq > INT64_MAX) {
        return false;
    }
    if (!value->image_boundary) return value->source_seq == 0u;
    return kind == SNAG_BINARY_CONTROL_REQUESTED && value->control == SNAG_CONTROL_COMPACT &&
        value->source_seq != 0u;
}

static int
encode_control(struct snag_buf *out, enum snag_binary_kind kind,
    const struct snag_binary_control *value)
{
    if (!control_valid(kind, value)) return invalid();
    if (write_uint(out, value->control, 1u) < 0) return -1;
    if (kind != SNAG_BINARY_CONTROL_REQUESTED) return 0;
    if (write_uint(out, value->image_boundary ? 1u : 0u, 1u) < 0) return -1;
    return value->image_boundary ? write_uint(out, value->source_seq, 8u) : 0;
}

static bool
decode_control(struct fields *fields, enum snag_binary_kind kind, struct snag_binary_control *out)
{
    uint64_t control;
    uint64_t origin;
    if (!read_uint(fields, 1u, &control)) return false;
    out->control = (unsigned int)control;
    if (kind == SNAG_BINARY_CONTROL_REQUESTED) {
        if (!read_uint(fields, 1u, &origin) || origin > 1u) return false;
        out->image_boundary = origin != 0u;
        if (out->image_boundary && !read_uint(fields, 8u, &out->source_seq)) {
            return false;
        }
    }
    return control_valid(kind, out);
}

static bool
rebase_reason_valid(enum snag_binary_rebase_reason reason)
{
    return reason == SNAG_BINARY_REBASE_GOAL_RECOVERY || reason == SNAG_BINARY_REBASE_TURN_RECOVERY;
}

static int
encode_context_rebase(struct snag_buf *out, const struct snag_binary_context_rebase *value)
{
    if (!rebase_reason_valid(value->reason)) return invalid();
    if (snag_buf_append(out, value->turn, 16u) < 0) return -1;
    return write_uint(out, value->reason, 1u);
}

static bool
decode_context_rebase(struct fields *fields, struct snag_binary_context_rebase *out)
{
    uint64_t reason;
    if (!read_id(fields, out->turn) || !read_uint(fields, 1u, &reason)) {
        return false;
    }
    out->reason = (enum snag_binary_rebase_reason)reason;
    return rebase_reason_valid(out->reason);
}

static bool
capacity_rejection_valid(const struct snag_binary_capacity_rejection *value)
{
    return value->cycle && value->context_limit <= SNAG_CONFIG_TOKEN_LIMIT_MAX &&
        value->requested_input <= SNAG_CONFIG_TOKEN_LIMIT_MAX &&
        value->observed_input <= SNAG_CONFIG_TOKEN_LIMIT_MAX &&
        text_valid(value->message, 0u, 255u);
}

static int
encode_capacity_rejection(struct snag_buf *out, const struct snag_binary_capacity_rejection *value)
{
    if (!capacity_rejection_valid(value)) return invalid();
    if (snag_buf_append(out, value->turn, 16u) < 0 ||
        snag_buf_append(out, value->response, 16u) < 0 || write_uint(out, value->cycle, 4u) < 0 ||
        snag_buf_append(out, value->provider_source_sha256, 32u) < 0 ||
        snag_buf_append(out, value->request_sha256, 32u) < 0 ||
        write_uint(out, value->context_limit, 8u) < 0 ||
        write_uint(out, value->requested_input, 8u) < 0 ||
        write_uint(out, value->observed_input, 8u) < 0) {
        return -1;
    }
    return write_text(out, value->message, 0u, 255u);
}

static bool
decode_capacity_rejection(struct fields *fields, struct snag_binary_capacity_rejection *out)
{
    uint64_t cycle;
    if (!read_id(fields, out->turn) || !read_id(fields, out->response) ||
        !read_uint(fields, 4u, &cycle) || !read_bytes(fields, out->provider_source_sha256, 32u) ||
        !read_bytes(fields, out->request_sha256, 32u) ||
        !read_uint(fields, 8u, &out->context_limit) ||
        !read_uint(fields, 8u, &out->requested_input) ||
        !read_uint(fields, 8u, &out->observed_input) ||
        !read_text(fields, &out->message, 0u, 255u)) {
        return false;
    }
    out->cycle = (uint32_t)cycle;
    return capacity_rejection_valid(out);
}

static int
encode_fields(struct snag_buf *out, const struct snag_binary_event *event)
{
    if (event->kind == SNAG_BINARY_AUDIO_USAGE) {
        return encode_audio_usage(out, &event->data.audio_usage);
    }
    if (event->kind == SNAG_BINARY_VOICE_EVENT) {
        return encode_voice_event(out, &event->data.voice_event);
    }
    if (event->kind == SNAG_BINARY_VOICE_TRANSFER_RECORD) {
        return encode_voice_archive(out, &event->data.voice_transfer_record);
    }
    if (event->kind == SNAG_BINARY_VOICE_TRANSFER_SEALED) {
        return encode_voice_transfer(out, &event->data.voice_transfer_sealed);
    }
    if (event->kind == SNAG_BINARY_VOICE_TRANSFER_ADOPTED) {
        return encode_voice_adopted(out, &event->data.voice_transfer_adopted);
    }
    if (event->kind == SNAG_BINARY_RULE_LOG) {
        return encode_rule_log(out, &event->data.rule_log);
    }
    if (event->kind == SNAG_BINARY_RULE_TRANSFORM) {
        const struct snag_binary_rule_transform *value = &event->data.rule_transform;
        if (snag_buf_append(out, value->call, 16u) < 0 ||
            snag_buf_append(out, value->original_sha256, 32u) < 0 ||
            snag_buf_append(out, value->effective_sha256, 32u) < 0) {
            return -1;
        }
        return write_text(out, value->rule, 0u, SNAG_MAX_EVENT_LINE);
    }
    if (event->kind == SNAG_BINARY_DOWNLOAD_QUEUED) {
        return encode_download(out, &event->data.download_queued);
    }
    if (event->kind == SNAG_BINARY_DOWNLOAD_REMOVED) {
        if (snag_buf_append(out, event->data.download_removed.id, 16u) < 0) {
            return -1;
        }
        return write_text(out, event->data.download_removed.reason, 0u, 1024u);
    }
    if (event->kind == SNAG_BINARY_DOWNLOADS_CLEARED) {
        return write_text(out, event->data.downloads_cleared, 0u, 1024u);
    }
    if (event->kind == SNAG_BINARY_COMPACTION_STARTED) {
        return encode_compact_start(out, &event->data.compaction_started);
    }
    if (event->kind == SNAG_BINARY_COMPACTION_INTERRUPTED) {
        return encode_compact_interrupt(out, &event->data.compaction_interrupted);
    }
    if (event->kind == SNAG_BINARY_COMPACTION_COMPLETED) {
        return encode_compact_complete(out, &event->data.compaction_completed);
    }
    if (control_kind(event->kind)) {
        return encode_control(out, event->kind, &event->data.control);
    }
    if (event->kind == SNAG_BINARY_CONTEXT_REBASED) {
        return encode_context_rebase(out, &event->data.context_rebased);
    }
    if (event->kind == SNAG_BINARY_RESPONSE_CAPACITY_REJECTED) {
        return encode_capacity_rejection(out, &event->data.capacity_rejected);
    }
    if (event->kind == SNAG_BINARY_RESPONSE_STARTED)
        return encode_response_start(out, &event->data.response_started);
    if (event->kind == SNAG_BINARY_RESPONSE_OUTPUT)
        return encode_response_output(out, &event->data.response_output);
    if (event->kind == SNAG_BINARY_RESPONSE_INTERRUPTED) {
        return encode_response_interruption(out, &event->data.response_interrupted);
    }
    if (event->kind == SNAG_BINARY_RESPONSE_FAILED) {
        return encode_response_failure(out, &event->data.response_failed);
    }
    if (event->kind == SNAG_BINARY_RESPONSE_OUTPUT_CORRECTION) {
        return encode_response_correction(out, &event->data.response_correction);
    }
    if (event->kind == SNAG_BINARY_RESPONSE_COMPLETED) {
        return encode_response_complete(out, &event->data.response_completed);
    }
    if (event->kind == SNAG_BINARY_TOOL_STARTED) {
        return encode_tool_start(out, &event->data.tool_started);
    }
    if (hosted_search_kind(event->kind)) return encode_hosted_search(out, event);
    if (event->kind == SNAG_BINARY_TOOL_FINISHED) {
        return encode_tool_finish(out, &event->data.tool_finished);
    }
    if (event->kind == SNAG_BINARY_PROCESS_OUTPUT) {
        return encode_process_output(out, &event->data.process_output);
    }
    if (event->kind == SNAG_BINARY_PROCESS_CLOSED) {
        return encode_process_close(out, &event->data.process_closed);
    }
    if (event->kind == SNAG_BINARY_IRC_EVENT) {
        return event->data.irc_event.echo_expected ? invalid() :
            encode_irc_event(out, &event->data.irc_event);
    }
    if (event->kind == SNAG_BINARY_IRC_EVENT_V2) {
        return encode_irc_route(out, &event->data.irc_event);
    }
    if (event->kind == SNAG_BINARY_IRC_SNAPSHOT) {
        return encode_irc_snapshot(out, &event->data.irc_snapshot);
    }
    if (event->kind == SNAG_BINARY_IRC_ADMITTED) {
        return encode_irc_admission(out, &event->data.irc_admitted);
    }
    if (event->kind >= SNAG_BINARY_IRC_SLEEP_SET && event->kind <= SNAG_BINARY_IRC_COMPACTED)
        return encode_irc_settings(out, event);
    if (event->kind == SNAG_BINARY_TURN_STARTED) {
        return encode_turn_start(out, &event->data.started);
    }
    if (turn_outcome_kind(event->kind)) return encode_turn_outcome(out, event);
    if (event->kind >= SNAG_BINARY_INPUT_RECEIVED &&
        event->kind <= SNAG_BINARY_FUTURE_TURN_EDITED) return encode_input(out, event);
    if ((event->kind >= SNAG_BINARY_SESSION_CREATED &&
            event->kind <= SNAG_BINARY_SERVICE_TIER_CHANGED) ||
        event->kind == SNAG_BINARY_RETRY_AUTO_CHANGED ||
        event->kind == SNAG_BINARY_FALLBACK_CHANGED ||
        event->kind == SNAG_BINARY_TURN_FALLBACK_STARTED) {
        return encode_metadata(out, event);
    }
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
    case SNAG_BINARY_GOAL_BLOCKED_WAIT_FOR:
        if (event->data.goal.actor != SNAG_BINARY_MODEL) return invalid();
        if (event->kind == SNAG_BINARY_GOAL_BLOCKED && event->data.goal.wait_for.size) {
            return invalid();
        }
        if (event->kind == SNAG_BINARY_GOAL_BLOCKED_WAIT_FOR &&
            !goal_wait_valid(event->data.goal.wait_for)) return invalid();
        if (write_uint(out, event->data.goal.actor, 1u) < 0) return -1;
        if (write_text(out, event->data.goal.text, 1u, SNAG_MAX_GOAL_BLOCKER) < 0) return -1;
        return event->kind == SNAG_BINARY_GOAL_BLOCKED ? 0 :
            write_text(out, event->data.goal.wait_for, 1u, SNAG_MAX_GOAL_BLOCKER);
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

static const struct {
    enum snag_binary_kind kind;
    const char *name;
} event_names[] = {
    {SNAG_BINARY_SESSION_CREATED, "session_created"},
    {SNAG_BINARY_CWD_CHANGED, "cwd_changed"},
    {SNAG_BINARY_SESSION_ARCHIVED, "session_archived"},
    {SNAG_BINARY_SESSION_UNARCHIVED, "session_unarchived"},
    {SNAG_BINARY_SESSION_DELETE_REQUESTED, "session_delete_requested"},
    {SNAG_BINARY_BANNER_UPDATED, "banner_updated"},
    {SNAG_BINARY_STEERING_UPDATED, "steering_updated"},
    {SNAG_BINARY_MODEL_SELECTED, "model_selection_changed"},
    {SNAG_BINARY_TURN_MODEL_CHANGED, "turn_model_changed"},
    {SNAG_BINARY_EFFORT_CHANGED, "effort_changed"},
    {SNAG_BINARY_CONTEXT_SELECTION_CHANGED, "context_selection_changed"},
    {SNAG_BINARY_COMMAND_SHELL_CHANGED, "command_shell_changed"},
    {SNAG_BINARY_SESSION_NAMED, "session_named"},
    {SNAG_BINARY_SESSION_OPTIONS, "session_options"},
    {SNAG_BINARY_SERVICE_TIER_CHANGED, "service_tier_changed"},
    {SNAG_BINARY_RETRY_AUTO_CHANGED, "retry_auto_changed"},
    {SNAG_BINARY_FALLBACK_CHANGED, "fallback_changed"},
    {SNAG_BINARY_TURN_FALLBACK_STARTED, "turn_fallback_started"},
    {SNAG_BINARY_CONTROL_REQUESTED, "control_requested"},
    {SNAG_BINARY_CONTROL_STARTED, "control_started"},
    {SNAG_BINARY_CONTROL_FINISHED, "control_finished"},
    {SNAG_BINARY_TIMER_SCHEDULED, "timer_scheduled"},
    {SNAG_BINARY_TIMER_FIRED, "timer_fired"},
    {SNAG_BINARY_TIMER_CANCELLED, "timer_cancelled"},
    {SNAG_BINARY_GOAL_STARTED, "goal_started"},
    {SNAG_BINARY_GOAL_REPLACED, "goal_replaced"},
    {SNAG_BINARY_GOAL_REWORDED, "goal_reworded"},
    {SNAG_BINARY_GOAL_LOCK_CHANGED, "goal_lock_changed"},
    {SNAG_BINARY_GOAL_PAUSED, "goal_paused"},
    {SNAG_BINARY_GOAL_BLOCKED, "goal_blocked"},
    {SNAG_BINARY_GOAL_COMPLETED, "goal_completed"},
    {SNAG_BINARY_GOAL_RESUMED, "goal_resumed"},
    {SNAG_BINARY_GOAL_CANCELLED, "goal_cancelled"},
    {SNAG_BINARY_GOAL_BLOCKED_WAIT_FOR, "goal_blocked_wait_for"},
    {SNAG_BINARY_INPUT_RECEIVED, "input_received"},
    {SNAG_BINARY_INPUT_CANCELLED, "input_cancelled"},
    {SNAG_BINARY_STEERING_ADDED, "steering_added"},
    {SNAG_BINARY_IRC_REPLY_REMINDER, "irc_reply_reminder"},
    {SNAG_BINARY_STEERING_DEFERRED, "steering_deferred"},
    {SNAG_BINARY_INPUT_ADMITTED, "input_admitted"},
    {SNAG_BINARY_FUTURE_QUEUE_STATE, "future_queue_state"},
    {SNAG_BINARY_FUTURE_TURN_CANCELLED, "future_turn_cancelled"},
    {SNAG_BINARY_FUTURE_TURN_QUEUED, "future_turn_queued"},
    {SNAG_BINARY_FUTURE_TURN_EDITED, "future_turn_edited"},
    {SNAG_BINARY_TURN_STARTED, "turn_started"},
    {SNAG_BINARY_TURN_YIELD_REQUESTED, "turn_yield_requested"},
    {SNAG_BINARY_TURN_CANCEL_REQUESTED, "turn_cancel_requested"},
    {SNAG_BINARY_TURN_RECOVERY, "turn_recovery"},
    {SNAG_BINARY_TURN_COMPLETED, "turn_completed"},
    {SNAG_BINARY_TURN_COMPLETED_SILENT, "turn_completed_silent"},
    {SNAG_BINARY_TURN_INTERRUPTED, "turn_interrupted"},
    {SNAG_BINARY_TURN_FAILED, "turn_failed"},
    {SNAG_BINARY_RESPONSE_STARTED, "response_started"},
    {SNAG_BINARY_RESPONSE_OUTPUT, "response_output"},
    {SNAG_BINARY_RESPONSE_INTERRUPTED, "response_interrupted"},
    {SNAG_BINARY_RESPONSE_FAILED, "response_failed"},
    {SNAG_BINARY_RESPONSE_OUTPUT_CORRECTION, "response_output_correction"},
    {SNAG_BINARY_RESPONSE_COMPLETED, "response_completed"},
    {SNAG_BINARY_RESPONSE_CAPACITY_REJECTED, "response_capacity_rejected"},
    {SNAG_BINARY_HOSTED_SEARCH_STARTED, "hosted_search_started"},
    {SNAG_BINARY_HOSTED_SEARCH_FINISHED, "hosted_search_finished"},
    {SNAG_BINARY_TOOL_STARTED, "tool_started"},
    {SNAG_BINARY_TOOL_FINISHED, "tool_finished"},
    {SNAG_BINARY_PROCESS_OUTPUT, "process_output"},
    {SNAG_BINARY_PROCESS_CLOSED, "process_closed"},
    {SNAG_BINARY_IRC_EVENT, "irc_event"},
    {SNAG_BINARY_IRC_EVENT_V2, "irc_event_v2"},
    {SNAG_BINARY_IRC_SNAPSHOT, "irc_snapshot"},
    {SNAG_BINARY_IRC_ADMITTED, "irc_admitted"},
    {SNAG_BINARY_IRC_SLEEP_SET, "irc_sleep_set"},
    {SNAG_BINARY_IRC_SLEEP_WOKE, "irc_sleep_woke"},
    {SNAG_BINARY_IRC_COMPACT_CONFIGURED, "irc_compact_configured"},
    {SNAG_BINARY_IRC_COMPACTED, "irc_compacted"},
    {SNAG_BINARY_COMPACTION_STARTED, "compaction_started"},
    {SNAG_BINARY_COMPACTION_INTERRUPTED, "compaction_interrupted"},
    {SNAG_BINARY_COMPACTION_COMPLETED, "compaction_completed"},
    {SNAG_BINARY_CONTEXT_REBASED, "context_rebased"},
    {SNAG_BINARY_DOWNLOAD_QUEUED, "download_queued"},
    {SNAG_BINARY_DOWNLOAD_REMOVED, "download_removed"},
    {SNAG_BINARY_DOWNLOADS_CLEARED, "downloads_cleared"},
    {SNAG_BINARY_RULE_LOG, "rule_log"},
    {SNAG_BINARY_RULE_TRANSFORM, "rule_transform"},
    {SNAG_BINARY_AUDIO_USAGE, "audio_usage"},
    {SNAG_BINARY_VOICE_EVENT, "voice_event"},
    {SNAG_BINARY_VOICE_TRANSFER_RECORD, "voice_transfer_record"},
    {SNAG_BINARY_VOICE_TRANSFER_SEALED, "voice_transfer_sealed"},
    {SNAG_BINARY_VOICE_TRANSFER_ADOPTED, "voice_transfer_adopted"},
};

const char *
snag_binary_event_name(enum snag_binary_kind kind)
{
    for (size_t i = 0u; i < sizeof(event_names) / sizeof(event_names[0]); ++i) {
        if (event_names[i].kind == kind) return event_names[i].name;
    }
    return NULL;
}

int
snag_binary_event_kind(const char *name, enum snag_binary_kind *out)
{
    if (!name || !out) return invalid();
    for (size_t i = 0u; i < sizeof(event_names) / sizeof(event_names[0]); ++i) {
        if (!strcmp(name, event_names[i].name)) {
            *out = event_names[i].kind;
            return 0;
        }
    }
    return invalid();
}

uint16_t
snag_binary_event_version(enum snag_binary_kind kind)
{
    if (control_kind(kind) ||
        kind == SNAG_BINARY_RULE_LOG || kind == SNAG_BINARY_RULE_TRANSFORM ||
        kind == SNAG_BINARY_AUDIO_USAGE || kind == SNAG_BINARY_VOICE_EVENT ||
        kind == SNAG_BINARY_VOICE_TRANSFER_RECORD || kind == SNAG_BINARY_VOICE_TRANSFER_SEALED ||
        (kind >= SNAG_BINARY_COMPACTION_STARTED && kind <= SNAG_BINARY_CONTEXT_REBASED) ||
        (kind >= SNAG_BINARY_DOWNLOAD_QUEUED && kind <= SNAG_BINARY_DOWNLOADS_CLEARED) ||
        kind == SNAG_BINARY_RESPONSE_CAPACITY_REJECTED) {
        return 1u;
    }
    if (kind == SNAG_BINARY_VOICE_TRANSFER_ADOPTED ||
        (kind >= SNAG_BINARY_INPUT_RECEIVED && kind <= SNAG_BINARY_FUTURE_TURN_EDITED)) return 2u;
    if ((kind >= SNAG_BINARY_SESSION_CREATED && kind <= SNAG_BINARY_SERVICE_TIER_CHANGED) ||
        kind == SNAG_BINARY_RETRY_AUTO_CHANGED ||
        kind == SNAG_BINARY_FALLBACK_CHANGED || kind == SNAG_BINARY_TURN_FALLBACK_STARTED ||
        timer_kind(kind) || goal_kind(kind) || hosted_search_kind(kind) ||
        kind == SNAG_BINARY_TURN_STARTED ||
        turn_outcome_kind(kind) || kind == SNAG_BINARY_RESPONSE_STARTED ||
        kind == SNAG_BINARY_RESPONSE_OUTPUT || kind == SNAG_BINARY_RESPONSE_INTERRUPTED ||
        kind == SNAG_BINARY_RESPONSE_FAILED || kind == SNAG_BINARY_RESPONSE_OUTPUT_CORRECTION ||
        kind == SNAG_BINARY_RESPONSE_COMPLETED || kind == SNAG_BINARY_TOOL_STARTED ||
        kind == SNAG_BINARY_PROCESS_OUTPUT || kind == SNAG_BINARY_IRC_EVENT ||
        kind == SNAG_BINARY_IRC_EVENT_V2 ||
        kind == SNAG_BINARY_IRC_SNAPSHOT ||
        (kind >= SNAG_BINARY_IRC_ADMITTED && kind <= SNAG_BINARY_IRC_COMPACTED)) {
        return 1u;
    }
    if (kind == SNAG_BINARY_TOOL_FINISHED || kind == SNAG_BINARY_PROCESS_CLOSED) {
        return 2u;
    }
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
    if (event->kind == SNAG_BINARY_AUDIO_USAGE) {
        return decode_audio_usage(fields, &event->data.audio_usage);
    }
    if (event->kind == SNAG_BINARY_VOICE_EVENT) {
        return decode_voice_event(fields, &event->data.voice_event);
    }
    if (event->kind == SNAG_BINARY_VOICE_TRANSFER_RECORD) {
        return decode_voice_archive(fields, &event->data.voice_transfer_record);
    }
    if (event->kind == SNAG_BINARY_VOICE_TRANSFER_SEALED) {
        return decode_voice_transfer(fields, &event->data.voice_transfer_sealed);
    }
    if (event->kind == SNAG_BINARY_VOICE_TRANSFER_ADOPTED) {
        return decode_voice_adopted(fields, &event->data.voice_transfer_adopted);
    }
    if (event->kind == SNAG_BINARY_RULE_LOG) {
        return decode_rule_log(fields, &event->data.rule_log);
    }
    if (event->kind == SNAG_BINARY_RULE_TRANSFORM) {
        struct snag_binary_rule_transform *transform = &event->data.rule_transform;
        return read_id(fields, transform->call) &&
            read_bytes(fields, transform->original_sha256, 32u) &&
            read_bytes(fields, transform->effective_sha256, 32u) &&
            read_text(fields, &transform->rule, 0u, SNAG_MAX_EVENT_LINE);
    }
    if (event->kind == SNAG_BINARY_DOWNLOAD_QUEUED) {
        return decode_download(fields, &event->data.download_queued);
    }
    if (event->kind == SNAG_BINARY_DOWNLOAD_REMOVED) {
        return read_id(fields, event->data.download_removed.id) &&
            read_text(fields, &event->data.download_removed.reason, 0u, 1024u);
    }
    if (event->kind == SNAG_BINARY_DOWNLOADS_CLEARED) {
        return read_text(fields, &event->data.downloads_cleared, 0u, 1024u);
    }
    if (event->kind == SNAG_BINARY_COMPACTION_STARTED) {
        return decode_compact_start(fields, &event->data.compaction_started);
    }
    if (event->kind == SNAG_BINARY_COMPACTION_INTERRUPTED) {
        return decode_compact_interrupt(fields, &event->data.compaction_interrupted);
    }
    if (event->kind == SNAG_BINARY_COMPACTION_COMPLETED) {
        return decode_compact_complete(fields, &event->data.compaction_completed);
    }
    if (control_kind(event->kind)) {
        return decode_control(fields, event->kind, &event->data.control);
    }
    if (event->kind == SNAG_BINARY_CONTEXT_REBASED) {
        return decode_context_rebase(fields, &event->data.context_rebased);
    }
    if (event->kind == SNAG_BINARY_RESPONSE_CAPACITY_REJECTED) {
        return decode_capacity_rejection(fields, &event->data.capacity_rejected);
    }
    if (event->kind == SNAG_BINARY_RESPONSE_STARTED)
        return decode_response_start(fields, &event->data.response_started);
    if (event->kind == SNAG_BINARY_RESPONSE_OUTPUT)
        return decode_response_output(fields, &event->data.response_output);
    if (event->kind == SNAG_BINARY_RESPONSE_INTERRUPTED) {
        return decode_response_interruption(fields, &event->data.response_interrupted);
    }
    if (event->kind == SNAG_BINARY_RESPONSE_FAILED) {
        return decode_response_failure(fields, &event->data.response_failed);
    }
    if (event->kind == SNAG_BINARY_RESPONSE_OUTPUT_CORRECTION) {
        return decode_response_correction(fields, &event->data.response_correction);
    }
    if (event->kind == SNAG_BINARY_RESPONSE_COMPLETED) {
        return decode_response_complete(fields, &event->data.response_completed);
    }
    if (event->kind == SNAG_BINARY_TOOL_STARTED) {
        return decode_tool_start(fields, &event->data.tool_started);
    }
    if (hosted_search_kind(event->kind)) return decode_hosted_search(fields, event);
    if (event->kind == SNAG_BINARY_TOOL_FINISHED) {
        return decode_tool_finish(fields, &event->data.tool_finished);
    }
    if (event->kind == SNAG_BINARY_PROCESS_OUTPUT) {
        return decode_process_output(fields, &event->data.process_output);
    }
    if (event->kind == SNAG_BINARY_PROCESS_CLOSED) {
        return decode_process_close(fields, &event->data.process_closed);
    }
    if (event->kind == SNAG_BINARY_IRC_EVENT) {
        return decode_irc_event(fields, &event->data.irc_event) &&
            !event->data.irc_event.echo_expected;
    }
    if (event->kind == SNAG_BINARY_IRC_EVENT_V2) {
        return decode_irc_route(fields, &event->data.irc_event);
    }
    if (event->kind == SNAG_BINARY_IRC_SNAPSHOT) {
        return decode_irc_snapshot(fields, &event->data.irc_snapshot);
    }
    if (event->kind == SNAG_BINARY_IRC_ADMITTED) {
        return decode_irc_admission(fields, &event->data.irc_admitted);
    }
    if (event->kind >= SNAG_BINARY_IRC_SLEEP_SET && event->kind <= SNAG_BINARY_IRC_COMPACTED)
        return decode_irc_settings(fields, event);
    if (event->kind == SNAG_BINARY_TURN_STARTED) {
        return decode_turn_start(fields, &event->data.started);
    }
    if (turn_outcome_kind(event->kind)) return decode_turn_outcome(fields, event);
    if (event->kind >= SNAG_BINARY_INPUT_RECEIVED &&
        event->kind <= SNAG_BINARY_FUTURE_TURN_EDITED) return decode_input(fields, event);
    if ((event->kind >= SNAG_BINARY_SESSION_CREATED &&
            event->kind <= SNAG_BINARY_SERVICE_TIER_CHANGED) ||
        event->kind == SNAG_BINARY_RETRY_AUTO_CHANGED ||
        event->kind == SNAG_BINARY_FALLBACK_CHANGED ||
        event->kind == SNAG_BINARY_TURN_FALLBACK_STARTED) {
        return decode_metadata(fields, event);
    }
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
    case SNAG_BINARY_GOAL_BLOCKED_WAIT_FOR:
        if (!read_uint(fields, 1u, &value) || value != SNAG_BINARY_MODEL) return false;
        event->data.goal.actor = (enum snag_binary_actor)value;
        if (!read_text(fields, &event->data.goal.text, 1u, SNAG_MAX_GOAL_BLOCKER)) return false;
        return event->kind == SNAG_BINARY_GOAL_BLOCKED ||
            (read_text(fields, &event->data.goal.wait_for, 1u, SNAG_MAX_GOAL_BLOCKER) &&
                goal_wait_valid(event->data.goal.wait_for));
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
    case SNAG_BINARY_IRC_ADMITTED:
        return input_leaf(&event.data.irc_admitted.input, field, out);
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
    case SNAG_BINARY_RESPONSE_OUTPUT_CORRECTION:
        text = event.data.response_correction.text;
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
find_record(const struct snag_binary_batch *batch, uint64_t sequence,
    struct snag_binary_record *out)
{
    if (!batch || !sequence || sequence == UINT64_MAX || sequence < batch->first_seq ||
        sequence - batch->first_seq >= batch->count) return invalid();
    size_t cursor = SNAG_BINARY_BATCH_HEADER_SIZE;
    struct snag_binary_record record;
    uint64_t found;
    int rc;
    while ((rc = snag_binary_record_next(batch, &cursor, &record, &found)) == 0) {
        if (found != sequence) continue;
        *out = record;
        return 0;
    }
    return rc < 0 ? rc : invalid();
}

static int
find_input_leaf(const struct snag_binary_batch *batch, uint64_t sequence,
    enum snag_binary_input_leaf field, struct snag_binary_ref *reference,
    const unsigned char **view)
{
    struct snag_binary_record record;
    struct snag_binary_text leaf;
    if (find_record(batch, sequence, &record) < 0 || input_leaf(&record, field, &leaf) < 0)
        return -1;
    size_t offset = (size_t)(leaf.data - record.payload);
    /* The event decoder bounds the complete payload before producing views. */
    if (offset > UINT32_MAX || leaf.size > UINT32_MAX) return invalid();
    *reference = (struct snag_binary_ref){sequence, (uint32_t)offset, (uint32_t)leaf.size};
    *view = leaf.data;
    return 0;
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

static int
find_control_text(const struct snag_binary_batch *batch, uint64_t sequence,
    enum snag_binary_kind kind, struct snag_binary_ref *reference,
    struct snag_binary_control_text_source *out)
{
    struct snag_binary_record record;
    struct snag_binary_control_text_source source = {0};
    if (find_record(batch, sequence, &record) < 0) return -1;
    if (record.kind != kind || snag_binary_event_decode(&record, &source.event) != 0) {
        return invalid();
    }
    switch (kind) {
    case SNAG_BINARY_SESSION_CREATED:
        source.text = source.event.data.created.cwd;
        break;
    case SNAG_BINARY_CWD_CHANGED:
        source.text = source.event.data.cwd.after;
        break;
    case SNAG_BINARY_BANNER_UPDATED:
        source.text = source.event.data.banner;
        break;
    case SNAG_BINARY_SESSION_NAMED:
        source.text = source.event.data.name;
        break;
    case SNAG_BINARY_SERVICE_TIER_CHANGED:
        source.text = source.event.data.service_tier;
        break;
    case SNAG_BINARY_RETRY_AUTO_CHANGED:
        source.text = source.event.data.retry_auto;
        break;
    case SNAG_BINARY_IRC_COMPACT_CONFIGURED:
        source.text = source.event.data.irc_compact_configured.instruction;
        break;
    case SNAG_BINARY_TIMER_SCHEDULED:
        source.text = source.event.data.timer.text;
        break;
    case SNAG_BINARY_GOAL_STARTED:
    case SNAG_BINARY_GOAL_REPLACED:
    case SNAG_BINARY_GOAL_REWORDED:
    case SNAG_BINARY_GOAL_BLOCKED:
    case SNAG_BINARY_GOAL_BLOCKED_WAIT_FOR:
        source.text = source.event.data.goal.text;
        break;
    default:
        return invalid();
    }
    size_t offset = (size_t)(source.text.data - record.payload);
    if (offset > UINT32_MAX || source.text.size > UINT32_MAX) return invalid();
    *reference = (struct snag_binary_ref){sequence, (uint32_t)offset, (uint32_t)source.text.size};
    *out = source;
    return 0;
}

int
snag_binary_control_text_ref_create(const struct snag_binary_batch *batch, uint64_t sequence,
    enum snag_binary_kind kind, struct snag_binary_ref *out)
{
    if (!out) return invalid();
    struct snag_binary_ref reference;
    struct snag_binary_control_text_source source;
    if (find_control_text(batch, sequence, kind, &reference, &source) < 0) return -1;
    *out = reference;
    return 0;
}

int
snag_binary_control_text_ref_resolve(const struct snag_binary_ref *reference,
    const struct snag_binary_batch *batch, enum snag_binary_kind kind,
    struct snag_binary_control_text_source *out)
{
    if (!reference || !out) return invalid();
    struct snag_binary_ref canonical;
    struct snag_binary_control_text_source source;
    if (find_control_text(batch, reference->sequence, kind, &canonical, &source) < 0) return -1;
    if (reference->offset != canonical.offset || reference->size != canonical.size) {
        return invalid();
    }
    *out = source;
    return 0;
}

static int
find_output_text(const struct snag_binary_batch *batch, uint64_t sequence,
    struct snag_binary_ref *reference, struct snag_binary_response_output *out)
{
    struct snag_binary_record record;
    struct snag_binary_event event;
    if (find_record(batch, sequence, &record) < 0) return -1;
    if (record.kind != SNAG_BINARY_RESPONSE_OUTPUT) return invalid();
    if (snag_binary_event_decode(&record, &event) != 0) return -1;
    struct snag_binary_text text = event.data.response_output.item.text;
    size_t offset = (size_t)(text.data - record.payload);
    if (offset > UINT32_MAX || text.size > UINT32_MAX) return invalid();
    *reference = (struct snag_binary_ref){sequence, (uint32_t)offset, (uint32_t)text.size};
    *out = event.data.response_output;
    return 0;
}

int
snag_binary_output_ref_create(const struct snag_binary_batch *batch, uint64_t sequence,
    struct snag_binary_ref *out)
{
    if (!out) return invalid();
    struct snag_binary_ref reference;
    struct snag_binary_response_output source;
    if (find_output_text(batch, sequence, &reference, &source) < 0) return -1;
    *out = reference;
    return 0;
}

int
snag_binary_output_ref_resolve(const struct snag_binary_ref *reference,
    const struct snag_binary_batch *batch, struct snag_binary_response_output *out)
{
    if (!reference || !out) return invalid();
    struct snag_binary_ref canonical;
    struct snag_binary_response_output source;
    if (find_output_text(batch, reference->sequence, &canonical, &source) < 0) return -1;
    if (reference->offset != canonical.offset || reference->size != canonical.size)
        return invalid();
    *out = source;
    return 0;
}

static int
find_completed_response(const struct snag_binary_batch *batch, uint64_t sequence,
    struct snag_binary_record *record, struct snag_binary_response_complete *out)
{
    struct snag_binary_event event;
    if (find_record(batch, sequence, record) < 0) return -1;
    if (record->kind != SNAG_BINARY_RESPONSE_COMPLETED) return invalid();
    if (snag_binary_event_decode(record, &event) != 0) return -1;
    *out = event.data.response_completed;
    return 0;
}

static bool
graph_original(const struct snag_binary_graph_item *item, enum snag_binary_item_kind kind)
{
    return item->kind == kind && (kind == SNAG_BINARY_ITEM_TOOL_CALL ||
        !item->data.output.source.first.sequence);
}

int
snag_binary_graph_ref_create(const struct snag_binary_batch *batch, uint64_t sequence,
    uint32_t index, enum snag_binary_item_kind kind, struct snag_binary_ref *out)
{
    if (!out) return invalid();
    struct snag_binary_record record;
    struct snag_binary_response_complete source;
    if (find_completed_response(batch, sequence, &record, &source) < 0) {
        return -1;
    }
    if (index >= source.items.count) return invalid();
    size_t cursor = 0u;
    for (uint32_t i = 0u; i <= index; ++i) {
        size_t start = cursor ? cursor : 4u;
        struct snag_binary_graph_item item;
        if (snag_binary_graph_items_next(&source.items, &cursor, &item) != 0) {
            return invalid();
        }
        if (i != index) continue;
        if (!graph_original(&item, kind)) return invalid();
        /* The decoded event bounds every item within the 16MiB payload. */
        size_t offset = (size_t)(source.items.data - record.payload) + start;
        *out = (struct snag_binary_ref){sequence, (uint32_t)offset, (uint32_t)(cursor - start)};
        return 0;
    }
    return invalid();
}

int
snag_binary_graph_ref_resolve(const struct snag_binary_ref *reference,
    const struct snag_binary_batch *batch, enum snag_binary_item_kind kind,
    struct snag_binary_graph_source *out)
{
    if (!reference || !out) return invalid();
    struct snag_binary_record record;
    struct snag_binary_response_complete source;
    if (find_completed_response(batch, reference->sequence, &record, &source) < 0) {
        return -1;
    }
    size_t cursor = 0u;
    for (uint32_t i = 0u; i < source.items.count; ++i) {
        size_t start = cursor ? cursor : 4u;
        struct snag_binary_graph_item item;
        if (snag_binary_graph_items_next(&source.items, &cursor, &item) != 0) {
            return invalid();
        }
        size_t offset = (size_t)(source.items.data - record.payload) + start;
        if (reference->offset != offset) continue;
        if (reference->size != cursor - start || !graph_original(&item, kind)) {
            return invalid();
        }
        struct snag_binary_graph_source decoded = {
            .cycle = source.cycle, .index = i, .item = item
        };
        memcpy(decoded.turn, source.turn, 16u);
        memcpy(decoded.response, source.response, 16u);
        *out = decoded;
        return 0;
    }
    return invalid();
}

static int
find_continuation(const struct snag_binary_batch *batch, uint64_t sequence,
    struct snag_binary_ref *reference, struct snag_binary_continuation_source *out)
{
    struct snag_binary_record record;
    struct snag_binary_response_complete source;
    if (find_completed_response(batch, sequence, &record, &source) < 0) {
        return -1;
    }
    if (source.continuation_form != SNAG_BINARY_CONTINUATION_ITEMS) {
        return invalid();
    }
    size_t offset = (size_t)(source.continuation.data - record.payload);
    *reference = (struct snag_binary_ref){sequence, (uint32_t)offset,
        (uint32_t)source.continuation.size};
    struct snag_binary_continuation_source decoded = {
        .cycle = source.cycle, .items = source.continuation
    };
    memcpy(decoded.turn, source.turn, 16u);
    memcpy(decoded.response, source.response, 16u);
    memcpy(decoded.scope, source.continuation_scope, 32u);
    *out = decoded;
    return 0;
}

int
snag_binary_continuation_ref_create(const struct snag_binary_batch *batch, uint64_t sequence,
    struct snag_binary_ref *out)
{
    if (!out) return invalid();
    struct snag_binary_ref reference;
    struct snag_binary_continuation_source source;
    if (find_continuation(batch, sequence, &reference, &source) < 0) return -1;
    *out = reference;
    return 0;
}

int
snag_binary_continuation_ref_resolve(const struct snag_binary_ref *reference,
    const struct snag_binary_batch *batch, struct snag_binary_continuation_source *out)
{
    if (!reference || !out) return invalid();
    struct snag_binary_ref canonical;
    struct snag_binary_continuation_source source;
    if (find_continuation(batch, reference->sequence, &canonical, &source) < 0) {
        return -1;
    }
    if (reference->offset != canonical.offset || reference->size != canonical.size) {
        return invalid();
    }
    *out = source;
    return 0;
}

static int
find_compact_output(const struct snag_binary_batch *batch, uint64_t sequence,
    struct snag_binary_ref *reference, struct snag_binary_compact_complete *out)
{
    struct snag_binary_record record;
    struct snag_binary_event event;
    if (find_record(batch, sequence, &record) < 0) return -1;
    if (record.kind != SNAG_BINARY_COMPACTION_COMPLETED) return invalid();
    if (snag_binary_event_decode(&record, &event) != 0) return -1;
    struct snag_binary_compact_complete source = event.data.compaction_completed;
    *reference = (struct snag_binary_ref){sequence,
        (uint32_t)(source.output.data - record.payload), (uint32_t)source.output.size};
    *out = source;
    return 0;
}

int
snag_binary_compact_ref_create(const struct snag_binary_batch *batch, uint64_t sequence,
    struct snag_binary_ref *out)
{
    if (!out) return invalid();
    struct snag_binary_ref reference;
    struct snag_binary_compact_complete source;
    if (find_compact_output(batch, sequence, &reference, &source) < 0) {
        return -1;
    }
    *out = reference;
    return 0;
}

int
snag_binary_compact_ref_resolve(const struct snag_binary_ref *reference,
    const struct snag_binary_batch *batch, struct snag_binary_compact_complete *out)
{
    if (!reference || !out) return invalid();
    struct snag_binary_ref canonical;
    struct snag_binary_compact_complete source;
    if (find_compact_output(batch, reference->sequence, &canonical, &source) < 0) {
        return -1;
    }
    if (reference->offset != canonical.offset || reference->size != canonical.size) {
        return invalid();
    }
    *out = source;
    return 0;
}

static int
find_result(const struct snag_binary_batch *batch, uint64_t sequence, enum snag_binary_kind kind,
    struct snag_binary_ref *reference, struct snag_binary_result_source *out)
{
    if (kind != SNAG_BINARY_TOOL_FINISHED && kind != SNAG_BINARY_PROCESS_CLOSED) {
        return invalid();
    }
    struct snag_binary_record record;
    struct snag_binary_event event;
    if (find_record(batch, sequence, &record) < 0) return -1;
    if (record.kind != kind) return invalid();
    if (snag_binary_event_decode(&record, &event) != 0) return -1;
    struct snag_binary_result_source source = {.kind = kind};
    size_t offset = 32u;
    if (kind == SNAG_BINARY_TOOL_FINISHED) {
        memcpy(source.turn, event.data.tool_finished.turn, 16u);
        memcpy(source.owner, event.data.tool_finished.call, 16u);
        source.result = event.data.tool_finished.result;
    } else {
        memcpy(source.turn, event.data.process_closed.turn, 16u);
        memcpy(source.owner, event.data.process_closed.handle, 16u);
        source.cause = event.data.process_closed.cause;
        source.result = event.data.process_closed.result;
        offset++;
    }
    /* Both verified profiles put the complete result after their owner prefix. */
    *reference = (struct snag_binary_ref){sequence, (uint32_t)offset,
        (uint32_t)(record.size - offset)};
    *out = source;
    return 0;
}

int
snag_binary_result_ref_create(const struct snag_binary_batch *batch, uint64_t sequence,
    enum snag_binary_kind kind, struct snag_binary_ref *out)
{
    if (!out) return invalid();
    struct snag_binary_ref reference;
    struct snag_binary_result_source source;
    if (find_result(batch, sequence, kind, &reference, &source) < 0) return -1;
    *out = reference;
    return 0;
}

int
snag_binary_result_ref_resolve(const struct snag_binary_ref *reference,
    const struct snag_binary_batch *batch, enum snag_binary_kind kind,
    struct snag_binary_result_source *out)
{
    if (!reference || !out) return invalid();
    struct snag_binary_ref canonical;
    struct snag_binary_result_source source;
    if (find_result(batch, reference->sequence, kind, &canonical, &source) < 0) {
        return -1;
    }
    if (reference->offset != canonical.offset || reference->size != canonical.size) {
        return invalid();
    }
    *out = source;
    return 0;
}

static int
find_process_bytes(const struct snag_binary_batch *batch, uint64_t sequence,
    struct snag_binary_ref *reference, struct snag_binary_process_output *out)
{
    struct snag_binary_record record;
    struct snag_binary_event event;
    if (find_record(batch, sequence, &record) < 0) return -1;
    if (record.kind != SNAG_BINARY_PROCESS_OUTPUT) return invalid();
    if (snag_binary_event_decode(&record, &event) != 0) return -1;
    struct snag_binary_process_output source = event.data.process_output;
    size_t offset = (size_t)(source.data - record.payload);
    *reference = (struct snag_binary_ref){sequence, (uint32_t)offset, (uint32_t)source.size};
    *out = source;
    return 0;
}

int
snag_binary_process_ref_create(const struct snag_binary_batch *batch, uint64_t sequence,
    struct snag_binary_ref *out)
{
    if (!out) return invalid();
    struct snag_binary_ref reference;
    struct snag_binary_process_output source;
    if (find_process_bytes(batch, sequence, &reference, &source) < 0) return -1;
    *out = reference;
    return 0;
}

int
snag_binary_process_ref_resolve(const struct snag_binary_ref *reference,
    const struct snag_binary_batch *batch, struct snag_binary_process_output *out)
{
    if (!reference || !out) return invalid();
    struct snag_binary_ref canonical;
    struct snag_binary_process_output source;
    if (find_process_bytes(batch, reference->sequence, &canonical, &source) < 0) {
        return -1;
    }
    if (reference->offset != canonical.offset || reference->size != canonical.size) {
        return invalid();
    }
    *out = source;
    return 0;
}

static bool
output_span_valid(const struct snag_binary_output_span *span)
{
    unsigned char encoded[SNAG_BINARY_REF_SIZE];
    return span && snag_binary_ref_encode(encoded, &span->first) == 0 && span->first.size &&
        span->last_sequence >= span->first.sequence && span->last_sequence != UINT64_MAX &&
        span->bytes <= SNAG_MAX_PUBLIC_ITEM && span->bytes >= span->first.size &&
        ((span->first.sequence == span->last_sequence) == (span->bytes == span->first.size));
}

int
snag_binary_output_span_encode(unsigned char out[SNAG_BINARY_OUTPUT_SPAN_SIZE],
    const struct snag_binary_output_span *span)
{
    if (!out || !output_span_valid(span)) return invalid();
    unsigned char encoded[SNAG_BINARY_OUTPUT_SPAN_SIZE];
    if (snag_binary_ref_encode(encoded, &span->first) < 0) return -1;
    for (size_t i = 0u; i < 8u; ++i)
        encoded[16u + i] = (unsigned char)(span->last_sequence >> (8u * i));
    for (size_t i = 0u; i < 4u; ++i)
        encoded[24u + i] = (unsigned char)(span->bytes >> (8u * i));
    memcpy(out, encoded, sizeof(encoded));
    return 0;
}

int
snag_binary_output_span_decode(const void *data, size_t size, struct snag_binary_output_span *out)
{
    if (!data || !out || size != SNAG_BINARY_OUTPUT_SPAN_SIZE) return invalid();
    struct snag_binary_output_span span;
    if (snag_binary_ref_decode(data, SNAG_BINARY_REF_SIZE, &span.first) < 0) return -1;
    struct fields fields = {.data = data, .size = size, .offset = SNAG_BINARY_REF_SIZE};
    uint64_t bytes;
    if (!read_uint(&fields, 8u, &span.last_sequence) || !read_uint(&fields, 4u, &bytes))
        return invalid();
    span.bytes = (uint32_t)bytes;
    if (!output_span_valid(&span)) return invalid();
    *out = span;
    return 0;
}

static bool
public_metadata_equal(const struct snag_binary_public_item *a,
    const struct snag_binary_public_item *b)
{
    return a->kind == b->kind && a->phase == b->phase && !memcmp(a->id, b->id, 16u) &&
        a->provider_id.size == b->provider_id.size &&
        !memcmp(a->provider_id.data, b->provider_id.data, a->provider_id.size);
}

int
snag_binary_output_span_resolve(int fd, uint64_t boundary,
    const struct snag_binary_anchor *anchor, const struct snag_binary_output_span *span,
    const unsigned char turn[16], const unsigned char response[16], uint32_t cycle,
    const struct snag_binary_public_item *item, struct snag_buf *out)
{
    if (!out || !anchor || !output_span_valid(span) || !turn || !response || !cycle ||
        !item || item->text.data || item->text.size || !public_metadata_valid(item) ||
        anchor->next_seq > span->first.sequence) return invalid();
    struct snag_binary_anchor position = *anchor, next;
    struct snag_buf scratch = {.max = SNAG_BINARY_BATCH_MAX};
    struct snag_buf text = {.max = span->bytes};
    uint64_t index = 0u;
    int rc = -1;
    while (position.next_seq <= span->last_sequence) {
        struct snag_binary_batch batch;
        int read = snag_binary_batch_read(fd, boundary, &position, &scratch, &batch, &next);
        if (read < 0) goto out;
        if (read > 0) goto invalid_span;
        size_t cursor = SNAG_BINARY_BATCH_HEADER_SIZE;
        struct snag_binary_record record;
        uint64_t sequence;
        while ((read = snag_binary_record_next(&batch, &cursor, &record, &sequence)) == 0) {
            if (sequence < span->first.sequence) continue;
            if (sequence > span->last_sequence) goto invalid_span;
            if (record.kind != SNAG_BINARY_RESPONSE_OUTPUT) {
                if (sequence == span->first.sequence || sequence == span->last_sequence)
                    goto invalid_span;
                continue;
            }
            struct snag_binary_event event;
            if (snag_binary_event_decode(&record, &event) != 0) goto out;
            const struct snag_binary_response_output *source = &event.data.response_output;
            if (memcmp(source->turn, turn, 16u) || memcmp(source->response, response, 16u) ||
                source->cycle != cycle || !public_metadata_equal(&source->item, item) ||
                source->offset != text.len) goto invalid_span;
            if (sequence == span->first.sequence) {
                size_t offset = (size_t)(source->item.text.data - record.payload);
                if (span->first.offset != offset || span->first.size != source->item.text.size)
                    goto invalid_span;
                index = source->index;
            } else if (index != source->index) {
                goto invalid_span;
            }
            if (snag_buf_append(&text, source->item.text.data, source->item.text.size) < 0)
                goto out;
            if (sequence == span->last_sequence) {
                if (text.len != span->bytes) goto invalid_span;
                rc = snag_buf_append(out, text.data, text.len);
                goto out;
            }
        }
        if (read < 0) goto out;
        position = next;
    }
invalid_span:
    (void)invalid();
out:
    snag_buf_free(&text);
    snag_buf_free(&scratch);
    return rc;
}

struct span_read {
    const struct snag_binary_output_span *span;
    const unsigned char *turn, *response;
    uint32_t cycle;
    const struct snag_binary_public_item *item;
    struct snag_buf text;
    uint64_t index, last;
    bool first, sparse;
};

static int
read_span_fragment(void *opaque, const struct snag_binary_record *record, uint64_t sequence)
{
    struct span_read *state = opaque;
    const struct snag_binary_output_span *span = state->span;
    if (record->kind != SNAG_BINARY_RESPONSE_OUTPUT) {
        return sequence == span->first.sequence || sequence == span->last_sequence ? invalid() : 0;
    }
    struct snag_binary_event event;
    int decoded = snag_binary_event_decode(record, &event);
    if (decoded != 0) return decoded < 0 ? -1 : invalid();
    const struct snag_binary_response_output *source = &event.data.response_output;
    if (memcmp(source->turn, state->turn, 16u) ||
        memcmp(source->response, state->response, 16u) || source->cycle != state->cycle ||
        !public_metadata_equal(&source->item, state->item))
        return invalid();
    /* A checkpoint can retain only part of an older output span. Let the
     * caller fetch its authenticated closure before treating it as corrupt. */
    if (source->offset != state->text.len)
        return snag_errno(state->sparse && source->offset > state->text.len ? ENOENT : EINVAL);
    if (sequence == span->first.sequence) {
        size_t offset = (size_t)(source->item.text.data - record->payload);
        if (span->first.offset != offset || span->first.size != source->item.text.size)
            return invalid();
        state->index = source->index;
        state->first = true;
    } else if (!state->first || state->index != source->index) {
        return invalid();
    }
    if (snag_buf_append(&state->text, source->item.text.data, source->item.text.size) < 0)
        return -1;
    state->last = sequence;
    return 0;
}

int
snag_binary_output_span_read(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_index *access, const struct snag_binary_output_span *span,
    const unsigned char turn[16], const unsigned char response[16], uint32_t cycle,
    const struct snag_binary_public_item *item, struct snag_buf *out)
{
    if (!out || !through || !output_span_valid(span) || !turn || !response || !cycle ||
        !item || item->text.data || item->text.size || !public_metadata_valid(item) ||
        span->last_sequence >= through->next_seq) return invalid();
    struct span_read state = {.span = span, .turn = turn, .response = response,
        .cycle = cycle, .item = item, .text = {.max = span->bytes}, .sparse = access != NULL};
    int rc = snag_binary_checkpoint_records_read(fd, through, access,
        span->first.sequence, span->last_sequence + 1u, read_span_fragment, NULL, &state);
    if (rc == 0) {
        rc = state.first && state.last == span->last_sequence && state.text.len == span->bytes ?
            snag_buf_append(out, state.text.data, state.text.len) :
            snag_errno(state.sparse ? ENOENT : EINVAL);
    }
    snag_buf_free(&state.text);
    return rc;
}
