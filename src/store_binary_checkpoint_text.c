/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_checkpoint.h"
#include "json.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define TEXT_WIRE_SIZE (10u + 25u * SNAG_BINARY_CHECKPOINT_TEXT_COUNT)

static const struct text_slot {
    const char *name;
    size_t offset;
} text_slots[] = {
#define SLOT(f) {#f, offsetof(struct snag_session, f)}
    SLOT(cwd), SLOT(first_user), SLOT(last_user), SLOT(active_prompt), SLOT(goal_prompt),
    SLOT(goal_blocker), SLOT(timer_text), SLOT(banner_text), SLOT(steering_override),
    {"irc_snapshot", 0u}, SLOT(name), SLOT(service_tier), {"irc_compact_instruction", 0u}
#undef SLOT
};
_Static_assert(COUNT(text_slots) == 13u && SNAG_BINARY_CHECKPOINT_TEXT_COUNT == 13u,
    "version-3 fixed text slots");

static const char *
slot_text(const struct snag_session *state, size_t slot)
{
    if (slot == SNAG_BINARY_TEXT_IRC_SNAPSHOT || slot == SNAG_BINARY_TEXT_IRC_COMPACT_INSTRUCTION)
        return snag_json_string(state->strings, text_slots[slot].name);
    const char *text;
    memcpy(&text, (const unsigned char *)state + text_slots[slot].offset, sizeof(text));
    return text;
}

static bool
same_source(const struct snag_binary_checkpoint_text_source *a,
    const struct snag_binary_checkpoint_text_source *b)
{
    return a->declaration == b->declaration && a->original.field == b->original.field &&
        a->original.target.sequence == b->original.target.sequence &&
        a->original.target.offset == b->original.target.offset &&
        a->original.target.size == b->original.target.size;
}

static bool
valid_texts(const struct snag_binary_checkpoint_texts *texts)
{
    if (!texts || !texts->through || texts->through == UINT64_MAX ||
        !texts->slots[SNAG_BINARY_TEXT_CWD].declaration) return false;
    const struct snag_binary_checkpoint_text_source empty = {0};
    for (size_t i = 0u; i < COUNT(texts->slots); ++i) {
        const struct snag_binary_checkpoint_text_source *source = &texts->slots[i];
        if (!source->declaration) {
            if (!same_source(source, &empty)) return false;
            continue;
        }
        if (source->declaration > texts->through) return false;
        const struct snag_binary_input_reference *original = &source->original;
        if (i == SNAG_BINARY_TEXT_STEERING ||
            (i == SNAG_BINARY_TEXT_IRC_SNAPSHOT && !original->target.sequence)) {
            if (i == SNAG_BINARY_TEXT_IRC_SNAPSHOT && source->declaration != 1u)
                return false;
            struct snag_binary_checkpoint_text_source mode = {.declaration = source->declaration};
            if (!same_source(source, &mode)) return false;
            continue;
        }
        unsigned char encoded[SNAG_BINARY_REF_SIZE];
        if (snag_binary_ref_encode(encoded, &original->target) < 0 ||
            original->target.sequence > source->declaration) return false;
        if (original->field) {
            if (i != SNAG_BINARY_TEXT_FIRST_USER && i != SNAG_BINARY_TEXT_LAST_USER &&
                i != SNAG_BINARY_TEXT_ACTIVE_PROMPT) return false;
            if (original->field != SNAG_BINARY_INPUT_TEXT &&
                original->field != SNAG_BINARY_INPUT_VOICE_TRANSCRIPT &&
                original->field != SNAG_BINARY_INPUT_VOICE_REQUEST) return false;
        } else if (i == SNAG_BINARY_TEXT_ACTIVE_PROMPT ||
            original->target.sequence != source->declaration) return false;
    }
    return true;
}

static int
record_source(const struct snag_binary_record *record, uint64_t sequence,
    const struct snag_binary_event *event, struct snag_binary_checkpoint_text_source *out)
{
    struct snag_binary_checkpoint_text_source source = {.declaration = sequence};
    struct snag_binary_text text = {0};
    switch (event->kind) {
    case SNAG_BINARY_SESSION_CREATED: text = event->data.created.cwd; break;
    case SNAG_BINARY_CWD_CHANGED: text = event->data.cwd.after; break;
    case SNAG_BINARY_BANNER_UPDATED: text = event->data.banner; break;
    case SNAG_BINARY_SESSION_NAMED: text = event->data.name; break;
    case SNAG_BINARY_SERVICE_TIER_CHANGED: text = event->data.service_tier; break;
    case SNAG_BINARY_IRC_SNAPSHOT: text = event->data.irc_snapshot.text; break;
    case SNAG_BINARY_IRC_COMPACT_CONFIGURED:
        text = event->data.irc_compact_configured.instruction;
        break;
    case SNAG_BINARY_TIMER_SCHEDULED: text = event->data.timer.text; break;
    case SNAG_BINARY_GOAL_STARTED:
    case SNAG_BINARY_GOAL_REPLACED:
    case SNAG_BINARY_GOAL_REWORDED:
    case SNAG_BINARY_GOAL_BLOCKED:
    case SNAG_BINARY_GOAL_BLOCKED_WAIT_FOR: text = event->data.goal.text; break;
    case SNAG_BINARY_STEERING_UPDATED:
        *out = source;
        return 0;
    case SNAG_BINARY_TURN_STARTED:
        if (event->data.started.text_ref.field) {
            source.original = event->data.started.text_ref;
            if (source.original.target.sequence >= sequence) return snag_errno(EINVAL);
            *out = source;
            return 0;
        }
        source.original.field = SNAG_BINARY_INPUT_TEXT;
        text = event->data.started.text;
        break;
    default: return snag_errno(EINVAL);
    }
    if (!text.data || !record->payload) return snag_errno(EINVAL);
    /* These views came from decoding this record, not caller-supplied pointers. */
    size_t offset = (size_t)(text.data - record->payload);
    if (offset > record->size || text.size > record->size - offset ||
        offset > UINT32_MAX || text.size > UINT32_MAX) return snag_errno(EINVAL);
    source.original.target = (struct snag_binary_ref){
        sequence, (uint32_t)offset, (uint32_t)text.size};
    *out = source;
    return 0;
}

static size_t
changed_slot(enum snag_binary_kind kind)
{
    switch (kind) {
    case SNAG_BINARY_SESSION_CREATED:
    case SNAG_BINARY_CWD_CHANGED: return SNAG_BINARY_TEXT_CWD;
    case SNAG_BINARY_BANNER_UPDATED: return SNAG_BINARY_TEXT_BANNER;
    case SNAG_BINARY_SESSION_NAMED: return SNAG_BINARY_TEXT_NAME;
    case SNAG_BINARY_SERVICE_TIER_CHANGED: return SNAG_BINARY_TEXT_SERVICE_TIER;
    case SNAG_BINARY_IRC_SNAPSHOT: return SNAG_BINARY_TEXT_IRC_SNAPSHOT;
    case SNAG_BINARY_IRC_COMPACT_CONFIGURED: return SNAG_BINARY_TEXT_IRC_COMPACT_INSTRUCTION;
    case SNAG_BINARY_STEERING_UPDATED: return SNAG_BINARY_TEXT_STEERING;
    case SNAG_BINARY_TIMER_SCHEDULED: return SNAG_BINARY_TEXT_TIMER;
    case SNAG_BINARY_GOAL_STARTED:
    case SNAG_BINARY_GOAL_REPLACED:
    case SNAG_BINARY_GOAL_REWORDED: return SNAG_BINARY_TEXT_GOAL_PROMPT;
    case SNAG_BINARY_GOAL_BLOCKED:
    case SNAG_BINARY_GOAL_BLOCKED_WAIT_FOR: return SNAG_BINARY_TEXT_GOAL_BLOCKER;
    case SNAG_BINARY_TURN_STARTED: return SNAG_BINARY_TEXT_ACTIVE_PROMPT;
    default: return COUNT(text_slots);
    }
}

int
snag_binary_checkpoint_texts_step(struct snag_binary_checkpoint_texts *texts,
    const struct snag_binary_record *record, uint64_t sequence, const struct snag_session *state)
{
    if (!texts || !record || !state || !sequence || sequence == UINT64_MAX ||
        sequence <= texts->through) return snag_errno(EINVAL);
    if (texts->through) {
        if (!valid_texts(texts)) return snag_errno(EINVAL);
    } else {
        const struct snag_binary_checkpoint_text_source empty = {0};
        if (sequence != 1u || record->kind != SNAG_BINARY_SESSION_CREATED)
            return snag_errno(EINVAL);
        for (size_t i = 0u; i < COUNT(texts->slots); ++i)
            if (!same_source(&texts->slots[i], &empty)) return snag_errno(EINVAL);
    }
    enum snag_binary_kind kind = (enum snag_binary_kind)record->kind;
    if (record->flags || !snag_binary_event_name(kind)) return snag_errno(EINVAL);
    struct snag_binary_checkpoint_texts next = *texts;
    size_t slot = changed_slot(kind);
    if (slot < COUNT(texts->slots)) {
        struct snag_binary_event event;
        int decoded = snag_binary_event_decode(record, &event);
        if (decoded != 0) return decoded < 0 ? -1 : snag_errno(EINVAL);
        struct snag_binary_checkpoint_text_source source;
        if (record_source(record, sequence, &event, &source) < 0) return -1;
        const char *retained = slot_text(state, slot);
        bool present = event.kind == SNAG_BINARY_BANNER_UPDATED ? event.data.banner.size != 0u :
            event.kind != SNAG_BINARY_STEERING_UPDATED ||
            event.data.steering != SNAG_BINARY_STEERING_DEFAULT;
        if (present != (retained != NULL)) return snag_errno(EINVAL);
        if (retained && slot != SNAG_BINARY_TEXT_STEERING &&
            strlen(retained) != source.original.target.size) return snag_errno(EINVAL);
        if (retained && source.original.target.sequence == sequence &&
            memcmp(retained, record->payload + source.original.target.offset,
                source.original.target.size)) return snag_errno(EINVAL);
        if (retained && slot == SNAG_BINARY_TEXT_STEERING &&
            strcmp(retained, event.data.steering == SNAG_BINARY_STEERING_ALL ? "all" : "mentions"))
            return snag_errno(EINVAL);
        next.slots[slot] = source;
        bool first = event.kind == SNAG_BINARY_GOAL_STARTED ||
            (event.kind == SNAG_BINARY_TURN_STARTED &&
             event.data.started.origin != SNAG_BINARY_TURN_GOAL);
        bool last = first || ((event.kind == SNAG_BINARY_GOAL_REPLACED ||
            event.kind == SNAG_BINARY_GOAL_REWORDED) && event.data.goal.actor == SNAG_BINARY_USER);
        if (first && !next.slots[SNAG_BINARY_TEXT_FIRST_USER].declaration)
            next.slots[SNAG_BINARY_TEXT_FIRST_USER] = source;
        if (last) next.slots[SNAG_BINARY_TEXT_LAST_USER] = source;
    }
    if (kind == SNAG_BINARY_SESSION_CREATED) {
        const char *snapshot = slot_text(state, SNAG_BINARY_TEXT_IRC_SNAPSHOT);
        if (snapshot && *snapshot) return snag_errno(EINVAL);
        next.slots[SNAG_BINARY_TEXT_IRC_SNAPSHOT] =
            (struct snag_binary_checkpoint_text_source){.declaration = sequence};
    }
    for (size_t i = 0u; i < COUNT(texts->slots); ++i) {
        const char *text = slot_text(state, i);
        if (!text) memset(&next.slots[i], 0, sizeof(next.slots[i]));
        else if (!next.slots[i].declaration) return snag_errno(EINVAL);
    }
    next.through = sequence;
    if (!valid_texts(&next)) return snag_errno(EINVAL);
    *texts = next;
    return 0;
}

static void
put64(unsigned char *out, uint64_t value)
{
    for (size_t i = 0u; i < 8u; ++i) out[i] = (unsigned char)(value >> (i * 8u));
}

static uint64_t
get64(const unsigned char *in)
{
    uint64_t value = 0u;
    for (size_t i = 0u; i < 8u; ++i) value |= (uint64_t)in[i] << (i * 8u);
    return value;
}

int
snag_binary_checkpoint_texts_encode(struct snag_buf *out,
    const struct snag_binary_checkpoint_texts *texts)
{
    if (!out || !valid_texts(texts)) return snag_errno(EINVAL);
    size_t count = texts->slots[SNAG_BINARY_TEXT_IRC_COMPACT_INSTRUCTION].declaration ?
        COUNT(texts->slots) : texts->slots[SNAG_BINARY_TEXT_SERVICE_TIER].declaration ?
        SNAG_BINARY_TEXT_IRC_COMPACT_INSTRUCTION : SNAG_BINARY_TEXT_SERVICE_TIER;
    unsigned char encoded[TEXT_WIRE_SIZE] = {0};
    encoded[0] = count == SNAG_BINARY_TEXT_SERVICE_TIER ? 1u :
        count == SNAG_BINARY_TEXT_IRC_COMPACT_INSTRUCTION ? 2u : 3u;
    put64(encoded + 2u, texts->through);
    for (size_t i = 0u; i < count; ++i) {
        const struct snag_binary_checkpoint_text_source *source = &texts->slots[i];
        unsigned char *entry = encoded + 10u + 25u * i;
        put64(entry, source->declaration);
        entry[8] = (unsigned char)source->original.field;
        if (source->original.target.sequence &&
            snag_binary_ref_encode(entry + 9u, &source->original.target) < 0) return -1;
    }
    return snag_buf_append(out, encoded, 10u + 25u * count);
}

int
snag_binary_checkpoint_texts_decode(const void *data, size_t size,
    struct snag_binary_checkpoint_texts *out)
{
    if (!data || !out || size < 2u) return snag_errno(EINVAL);
    const unsigned char *bytes = data;
    if (bytes[0] < 1u || bytes[0] > 3u || bytes[1]) return snag_errno(EINVAL);
    size_t count = bytes[0] == 1u ? SNAG_BINARY_TEXT_SERVICE_TIER :
        bytes[0] == 2u ? SNAG_BINARY_TEXT_IRC_COMPACT_INSTRUCTION :
        SNAG_BINARY_CHECKPOINT_TEXT_COUNT;
    if (size != 10u + 25u * count) return snag_errno(EINVAL);
    struct snag_binary_checkpoint_texts value = {.through = get64(bytes + 2u)};
    static const unsigned char zero[SNAG_BINARY_REF_SIZE] = {0};
    for (size_t i = 0u; i < count; ++i) {
        const unsigned char *entry = bytes + 10u + 25u * i;
        struct snag_binary_checkpoint_text_source *source = &value.slots[i];
        source->declaration = get64(entry);
        source->original.field = entry[8];
        if (memcmp(entry + 9u, zero, sizeof(zero)) &&
            snag_binary_ref_decode(entry + 9u, SNAG_BINARY_REF_SIZE, &source->original.target) < 0)
            return -1;
    }
    if (!valid_texts(&value)) return snag_errno(EINVAL);
    *out = value;
    return 0;
}

static bool
slot_accepts(size_t slot, const struct snag_binary_event *event)
{
    switch (slot) {
    case SNAG_BINARY_TEXT_CWD:
        return event->kind == SNAG_BINARY_SESSION_CREATED || event->kind == SNAG_BINARY_CWD_CHANGED;
    case SNAG_BINARY_TEXT_FIRST_USER:
    case SNAG_BINARY_TEXT_LAST_USER:
        return event->kind == SNAG_BINARY_GOAL_STARTED ||
            (event->kind == SNAG_BINARY_TURN_STARTED &&
             event->data.started.origin != SNAG_BINARY_TURN_GOAL) ||
            (slot == SNAG_BINARY_TEXT_LAST_USER &&
             (event->kind == SNAG_BINARY_GOAL_REPLACED ||
              event->kind == SNAG_BINARY_GOAL_REWORDED) &&
             event->data.goal.actor == SNAG_BINARY_USER);
    case SNAG_BINARY_TEXT_ACTIVE_PROMPT: return event->kind == SNAG_BINARY_TURN_STARTED;
    case SNAG_BINARY_TEXT_GOAL_PROMPT:
        return event->kind == SNAG_BINARY_GOAL_STARTED ||
            event->kind == SNAG_BINARY_GOAL_REPLACED ||
            event->kind == SNAG_BINARY_GOAL_REWORDED;
    case SNAG_BINARY_TEXT_GOAL_BLOCKER:
        return (event->kind == SNAG_BINARY_GOAL_BLOCKED ||
            event->kind == SNAG_BINARY_GOAL_BLOCKED_WAIT_FOR) &&
            event->data.goal.actor == SNAG_BINARY_MODEL;
    case SNAG_BINARY_TEXT_TIMER: return event->kind == SNAG_BINARY_TIMER_SCHEDULED;
    case SNAG_BINARY_TEXT_BANNER: return event->kind == SNAG_BINARY_BANNER_UPDATED;
    case SNAG_BINARY_TEXT_NAME: return event->kind == SNAG_BINARY_SESSION_NAMED;
    case SNAG_BINARY_TEXT_SERVICE_TIER: return event->kind == SNAG_BINARY_SERVICE_TIER_CHANGED;
    case SNAG_BINARY_TEXT_IRC_COMPACT_INSTRUCTION:
        return event->kind == SNAG_BINARY_IRC_COMPACT_CONFIGURED;
    case SNAG_BINARY_TEXT_IRC_SNAPSHOT:
        return event->kind == SNAG_BINARY_SESSION_CREATED ||
            event->kind == SNAG_BINARY_IRC_SNAPSHOT;
    case SNAG_BINARY_TEXT_STEERING:
        return event->kind == SNAG_BINARY_STEERING_UPDATED &&
            event->data.steering != SNAG_BINARY_STEERING_DEFAULT;
    default: return false;
    }
}

static int
find_declaration(const struct snag_binary_batch *batch, uint64_t sequence,
    struct snag_binary_record *record)
{
    size_t offset = SNAG_BINARY_BATCH_HEADER_SIZE;
    uint64_t found;
    int rc;
    while ((rc = snag_binary_record_next(batch, &offset, record, &found)) == 0)
        if (found == sequence) return 0;
    return rc < 0 ? -1 : snag_errno(EINVAL);
}

int
snag_binary_checkpoint_texts_read(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_index *access,
    const struct snag_binary_checkpoint_texts *texts, json_t **out)
{
    if (fd < 0 || !through || !out || !valid_texts(texts) || texts->through >= through->next_seq)
        return snag_errno(EINVAL);
    json_t *strings = json_object();
    if (!strings) return snag_errno(ENOMEM);
    struct snag_buf scratch;
    snag_buf_init(&scratch, SNAG_BINARY_BATCH_MAX);
    int rc = -1;
    for (size_t i = 0u; i < COUNT(texts->slots); ++i) {
        const struct snag_binary_checkpoint_text_source *source = &texts->slots[i];
        if (!source->declaration) continue;
        struct snag_binary_batch batch;
        struct snag_binary_anchor before;
        struct snag_binary_record record;
        struct snag_binary_event event;
        struct snag_binary_checkpoint_text_source expected;
        if (snag_binary_checkpoint_batch_find(fd, through, access, source->declaration,
                &scratch, &batch, &before) < 0 ||
            find_declaration(&batch, source->declaration, &record) < 0) goto done;
        if (changed_slot((enum snag_binary_kind)record.kind) == COUNT(text_slots)) {
            errno = EINVAL;
            goto done;
        }
        int decoded = snag_binary_event_decode(&record, &event);
        if (decoded != 0) {
            if (decoded > 0) errno = EINVAL;
            goto done;
        }
        bool empty_snapshot = i == SNAG_BINARY_TEXT_IRC_SNAPSHOT &&
            event.kind == SNAG_BINARY_SESSION_CREATED;
        if (empty_snapshot) {
            expected = (struct snag_binary_checkpoint_text_source){.declaration = 1u};
        } else if (record_source(&record, source->declaration, &event, &expected) < 0) {
            goto done;
        }
        if (!slot_accepts(i, &event) || !same_source(source, &expected)) {
            errno = EINVAL;
            goto done;
        }
        const unsigned char *text;
        size_t size = source->original.target.size;
        if (empty_snapshot) {
            text = (const unsigned char *)"";
        } else if (i == SNAG_BINARY_TEXT_IRC_SNAPSHOT) {
            text = event.data.irc_snapshot.text.data;
        } else if (i == SNAG_BINARY_TEXT_STEERING) {
            text = (const unsigned char *)(event.data.steering == SNAG_BINARY_STEERING_ALL ?
                "all" : "mentions");
            size = strlen((const char *)text);
        } else if (source->original.field) {
            if (source->original.target.sequence != source->declaration &&
                snag_binary_checkpoint_batch_find(fd, through, access,
                    source->original.target.sequence, &scratch, &batch, &before) < 0) goto done;
            if (snag_binary_input_ref_resolve(&source->original.target, &batch,
                    source->original.field, &text) < 0) goto done;
        } else {
            struct snag_binary_control_text_source original;
            if (snag_binary_control_text_ref_resolve(&source->original.target, &batch,
                    event.kind, &original) < 0) goto done;
            text = original.text.data;
        }
        if ((i == SNAG_BINARY_TEXT_BANNER && !size) ||
            !snag_utf8_valid(text, size, true)) { errno = EINVAL; goto done; }
        json_t *value = json_stringn((const char *)text, size);
        if (!value) { errno = ENOMEM; goto done; }
        if (snag_json_set_new(strings, text_slots[i].name, value) < 0) goto done;
    }
    *out = strings;
    strings = NULL;
    rc = 0;
 done:
    snag_buf_free(&scratch);
    json_decref(strings);
    return rc;
}
