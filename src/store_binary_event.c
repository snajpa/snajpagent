/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_event.h"
#include "store.h"

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
read_id(struct fields *fields, unsigned char out[16])
{
    if (16u > fields->size - fields->offset) return false;
    memcpy(out, fields->data + fields->offset, 16u);
    fields->offset += 16u;
    return true;
}

static bool
text_valid(struct snag_binary_text text, size_t maximum)
{
    return text.data && text.size && text.size <= maximum &&
        snag_utf8_valid(text.data, text.size, true);
}

static int
write_text(struct snag_buf *out, struct snag_binary_text text, size_t maximum)
{
    if (!text_valid(text, maximum)) return invalid();
    if (write_uint(out, text.size, 4u) < 0) return -1;
    return snag_buf_append(out, text.data, text.size);
}

static bool
read_text(struct fields *fields, struct snag_binary_text *text, size_t maximum)
{
    uint64_t length;
    if (!read_uint(fields, 4u, &length) || length > fields->size - fields->offset)
        return false;
    struct snag_binary_text decoded = {fields->data + fields->offset, (size_t)length};
    if (!text_valid(decoded, maximum)) return false;
    fields->offset += (size_t)length;
    *text = decoded;
    return true;
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
    if (timer_kind(event->kind)) {
        if (snag_buf_append(out, event->data.timer.id, 16u) < 0) return -1;
        if (event->kind != SNAG_BINARY_TIMER_SCHEDULED) return 0;
        if (!event->data.timer.due_ms) return invalid();
        if (write_uint(out, event->data.timer.due_ms, 8u) < 0) return -1;
        return write_text(out, event->data.timer.text, SNAG_MAX_TIMER_TEXT);
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
        return write_text(out, event->data.goal.text, SNAG_MAX_GOAL_PROMPT);
    case SNAG_BINARY_GOAL_BLOCKED:
        if (event->data.goal.actor != SNAG_BINARY_MODEL) return invalid();
        if (write_uint(out, event->data.goal.actor, 1u) < 0) return -1;
        return write_text(out, event->data.goal.text, SNAG_MAX_GOAL_BLOCKER);
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
    if (timer_kind(event->kind)) {
        if (!read_id(fields, event->data.timer.id)) return false;
        if (event->kind != SNAG_BINARY_TIMER_SCHEDULED) return true;
        return read_uint(fields, 8u, &event->data.timer.due_ms) &&
            event->data.timer.due_ms &&
            read_text(fields, &event->data.timer.text, SNAG_MAX_TIMER_TEXT);
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
        return read_text(fields, &event->data.goal.text, SNAG_MAX_GOAL_PROMPT);
    case SNAG_BINARY_GOAL_BLOCKED:
        if (!read_uint(fields, 1u, &value) || value != SNAG_BINARY_MODEL) return false;
        event->data.goal.actor = (enum snag_binary_actor)value;
        return read_text(fields, &event->data.goal.text, SNAG_MAX_GOAL_BLOCKER);
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
    if (!record->kind || !record->version || record->size > SNAG_MAX_EVENT_LINE ||
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
