/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_event.h"
#include "store.h"
#include "media.h"

#include <errno.h>
#include <string.h>

struct fields {
    const unsigned char *data;
    size_t size, offset;
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
    struct fields fields = {data, size, 0};
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
    struct fields fields = {content->data, content->size, *offset ? *offset : 4u};
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
    struct fields fields = {data, size, 0};
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
    struct fields fields = {instructions->data, instructions->size, *offset ? *offset : 4u};
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
write_instructions(struct snag_buf *out, struct snag_binary_instructions instructions)
{
    struct fields fields = {instructions.data, instructions.size, 0};
    struct snag_binary_instructions decoded;
    if (!instructions.data || instructions.size > SNAG_MAX_EVENT_LINE ||
        !read_instructions(&fields, &decoded, false) || fields.offset != fields.size)
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

static int
encode_input(struct snag_buf *out, const struct snag_binary_event *event)
{
    bool content;
    switch (event->kind) {
    case SNAG_BINARY_INPUT_RECEIVED:
        if (event->data.input.origin != SNAG_BINARY_INPUT_DEFAULT &&
            event->data.input.origin != SNAG_BINARY_INPUT_TIMER) return invalid();
        content = event->data.input.content.data || event->data.input.content.size;
        if (write_uint(out, event->data.input.origin, 1u) < 0 ||
            write_uint(out, (event->data.input.read_only ? 1u : 0u) |
                           (content ? 2u : 0u), 1u) < 0 ||
            write_uint(out, event->data.input.received_ms, 8u) < 0 ||
            write_selection(out, &event->data.input.selection) < 0 ||
            write_instructions(out, event->data.input.instructions) < 0 ||
            write_text(out, event->data.input.text, 1u, SNAG_MAX_DIRECT_PROMPT) < 0) return -1;
        return content ? write_content(out, event->data.input.content) : 0;
    case SNAG_BINARY_INPUT_CANCELLED:
        return 0;
    case SNAG_BINARY_STEERING_ADDED:
    case SNAG_BINARY_IRC_REPLY_REMINDER:
        content = event->data.steering_input.content.data ||
                  event->data.steering_input.content.size;
        if (snag_buf_append(out, event->data.steering_input.id, 16u) < 0 ||
            snag_buf_append(out, event->data.steering_input.turn, 16u) < 0 ||
            write_uint(out, (event->data.steering_input.has_received_ms ? 1u : 0u) |
                           (content ? 2u : 0u), 1u) < 0) return -1;
        if (event->data.steering_input.has_received_ms &&
            write_uint(out, event->data.steering_input.received_ms, 8u) < 0) return -1;
        if (write_text(out, event->data.steering_input.text, 1u, SNAG_MAX_STEERING_TEXT) < 0)
            return -1;
        return content ? write_content(out, event->data.steering_input.content) : 0;
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
    case SNAG_BINARY_INPUT_RECEIVED:
        if (!read_uint(fields, 1u, &value) || value > SNAG_BINARY_INPUT_TIMER ||
            !read_uint(fields, 1u, &flags) || flags > 3u) return false;
        event->data.input.origin = (enum snag_binary_input_origin)value;
        event->data.input.read_only = (flags & 1u) != 0;
        return read_uint(fields, 8u, &event->data.input.received_ms) &&
            read_selection(fields, &event->data.input.selection) &&
            read_instructions(fields, &event->data.input.instructions, false) &&
            read_text(fields, &event->data.input.text, 1u, SNAG_MAX_DIRECT_PROMPT) &&
            (!(flags & 2u) || read_content(fields, &event->data.input.content));
    case SNAG_BINARY_INPUT_CANCELLED:
        return true;
    case SNAG_BINARY_STEERING_ADDED:
    case SNAG_BINARY_IRC_REPLY_REMINDER:
        if (!read_id(fields, event->data.steering_input.id) ||
            !read_id(fields, event->data.steering_input.turn) ||
            !read_uint(fields, 1u, &flags) || flags > 3u) return false;
        event->data.steering_input.has_received_ms = (flags & 1u) != 0;
        return (!(flags & 1u) || read_uint(fields, 8u, &event->data.steering_input.received_ms)) &&
            read_text(fields, &event->data.steering_input.text, 1u, SNAG_MAX_STEERING_TEXT) &&
            (!(flags & 2u) || read_content(fields, &event->data.steering_input.content));
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
    if (event->kind >= SNAG_BINARY_INPUT_RECEIVED &&
        event->kind <= SNAG_BINARY_FUTURE_TURN_CANCELLED) return encode_input(out, event);
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
    if (event->kind >= SNAG_BINARY_INPUT_RECEIVED &&
        event->kind <= SNAG_BINARY_FUTURE_TURN_CANCELLED) return decode_input(fields, event);
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
    if (record->flags || record->version != 1u) return invalid();
    struct snag_binary_event decoded = {.kind = (enum snag_binary_kind)record->kind};
    struct fields fields = {.data = record->payload, .size = record->size};
    if (!decode_fields(&fields, &decoded) || fields.offset != fields.size) return invalid();
    *out = decoded;
    return 0;
}
