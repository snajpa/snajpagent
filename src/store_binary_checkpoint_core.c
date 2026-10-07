/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_checkpoint.h"
#include "json.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Sizes delimit the fixed-order blocks; the core version identifies the typed
 * directory/activity and goal-wait layouts. Epochs and adoption survive without
 * copied payloads. */
enum core_block {
    CONTROLS, ACCOUNTING, TEXTS, CALLS, PROCESSES, INPUTS, PAYLOADS, IRC,
    ACTIVITY, GOAL_WAIT, BLOCKS
};
#define CORE_HEADER (28u + BLOCKS * 8u)

static size_t
core_blocks(uint16_t version)
{
    return version == 2u ? IRC : version == 3u ? ACTIVITY :
        version == 4u ? GOAL_WAIT : version == SNAG_BINARY_CORE_VERSION ? BLOCKS : 0u;
}

static void
put_number(unsigned char *out, uint64_t number)
{
    for (size_t i = 0u; i < 8u; ++i) out[i] = (unsigned char)(number >> (i * 8u));
}

static uint64_t
get_number(const unsigned char *bytes)
{
    uint64_t number = 0u;
    for (size_t i = 0u; i < 8u; ++i) number |= (uint64_t)bytes[i] << (i * 8u);
    return number;
}

static void
bytes_hex(char *out, const unsigned char *bytes, size_t size)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0u; i < size; ++i) {
        out[i * 2u] = hex[bytes[i] >> 4u];
        out[i * 2u + 1u] = hex[bytes[i] & 15u];
    }
    out[size * 2u] = '\0';
}

static unsigned int
irc_status(const json_t *value)
{
    return json_is_true(value) ? 2u : json_is_false(value) ? 1u : 0u;
}

static int
irc_encode(struct snag_buf *out, const struct snag_session *state)
{
    const json_t *directory = state->irc_conversations;
    if (directory && !snag_irc_conversations_valid(directory, state->next_seq))
        return snag_errno(EINVAL);
    unsigned char header[9] = {directory ? 1u : 0u};
    put_number(header + 1u, json_object_size(directory));
    if (snag_buf_append(out, header, sizeof(header)) < 0) return -1;
    const char *id;
    const json_t *connection;
    json_object_foreach((json_t *)directory, id, connection) {
        unsigned char row[33];
        uint64_t generation;
        const json_t *status = json_object_get(connection, "connected");
        const json_t *items = json_object_get(connection, "conversations");
        for (size_t i = 0u; i < 16u; ++i) {
            unsigned int hi = id[i * 2u] <= '9' ? (unsigned)(id[i * 2u] - '0') :
                (unsigned)(id[i * 2u] - 'a') + 10u;
            unsigned int lo = id[i * 2u + 1u] <= '9' ? (unsigned)(id[i * 2u + 1u] - '0') :
                (unsigned)(id[i * 2u + 1u] - 'a') + 10u;
            row[i] = (unsigned char)(hi << 4u | lo);
        }
        if (snag_json_integer_u64(connection, "generation", &generation) < 0)
            return snag_errno(EINVAL);
        put_number(row + 16u, generation);
        row[24] = (unsigned char)(irc_status(json_object_get(status, "operator")) |
            irc_status(json_object_get(status, "agent")) << 2u);
        put_number(row + 25u, json_object_size(items));
        if (snag_buf_append(out, row, sizeof(row)) < 0) return -1;
        const char *conversation;
        const json_t *entry;
        json_object_foreach((json_t *)items, conversation, entry) {
            (void)conversation;
            uint64_t sequence;
            unsigned char bytes[8];
            if (snag_json_integer_u64(entry, "seq", &sequence) < 0)
                return snag_errno(EINVAL);
            put_number(bytes, sequence);
            if (snag_buf_append(out, bytes, sizeof(bytes)) < 0) return -1;
        }
    }
    return 0;
}

static json_t *
irc_status_value(unsigned int value)
{
    return value == 2u ? json_true() : value == 1u ? json_false() : json_null();
}

static int
irc_read(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_index *access,
    const struct snag_binary_checkpoint_section *block, json_t **out)
{
    if (block->size < 9u || !block->data) return snag_errno(EINVAL);
    const unsigned char *bytes = block->data;
    uint64_t count = get_number(bytes + 1u);
    if (bytes[0] > 1u || (!!bytes[0] != !!count) || count > (block->size - 9u) / 41u)
        return snag_errno(EINVAL);
    if (!count) {
        if (block->size != 9u) return snag_errno(EINVAL);
        *out = NULL;
        return 0;
    }
    json_t *directory = json_object();
    if (!directory) return snag_errno(ENOMEM);
    size_t position = 9u;
    for (uint64_t i = 0u; i < count; ++i) {
        if (block->size - position < 33u) goto invalid;
        const unsigned char *row = bytes + position;
        position += 33u;
        char id[SNAG_ID_HEX_LEN + 1u];
        bytes_hex(id, row, 16u);
        uint64_t generation = get_number(row + 16u), entries = get_number(row + 25u);
        unsigned int flags = row[24];
        if (!generation || generation > INT64_MAX || (flags & ~15u) ||
            (flags & 3u) == 3u || (flags >> 2u) == 3u ||
            !entries || entries > (block->size - position) / 8u ||
            json_object_get(directory, id)) goto invalid;
        json_t *connection = json_pack("{s:I,s:{s:O,s:O},s:{}}",
            "generation", (json_int_t)generation, "connected",
            "operator", irc_status_value(flags & 3u), "agent", irc_status_value(flags >> 2u),
            "conversations");
        if (!connection || snag_json_set_new(directory, id, connection) < 0) goto fail;
        json_t *items = json_object_get(connection, "conversations");
        for (uint64_t j = 0u; j < entries; ++j) {
            uint64_t sequence = get_number(bytes + position);
            position += 8u;
            if (!sequence || sequence >= through->next_seq) goto invalid;
            const char *type = NULL;
            json_t *data = NULL;
            if (snag_binary_checkpoint_projection_read(fd, through, access,
                sequence, &type, &data) < 0) goto fail;
            struct snag_irc_event event;
            bool valid = type && !strcmp(type, "irc_event_v2") &&
                !snag_irc_event_record_read(type, data, &event) &&
                !strcmp(event.route.connection, id) && event.route.generation <= generation &&
                !json_object_get(items, event.route.conversation);
            if (valid && j) {
                const char *endpoint = snag_json_string(connection, "endpoint");
                valid = endpoint && !strcmp(endpoint, event.endpoint);
            }
            if (!valid) { json_decref(data); goto invalid; }
            if (!j && snag_json_set_new(connection, "endpoint", json_string(event.endpoint)) < 0) {
                json_decref(data);
                goto fail;
            }
            json_t *entry = json_pack("{s:I,s:O}", "seq", (json_int_t)sequence, "data", data);
            json_decref(data);
            if (!entry || snag_json_set_new(items, event.route.conversation, entry) < 0) goto fail;
        }
    }
    if (position != block->size || !snag_irc_conversations_valid(directory, through->next_seq))
        goto invalid;
    *out = directory;
    return 0;
invalid:
    snag_errno(EINVAL);
fail:
    json_decref(directory);
    return -1;
}

static int
activity_encode(struct snag_buf *out, const struct snag_session *state)
{
    const json_t *activity = state->irc_activity;
    if (activity && !snag_irc_activity_valid(activity, state->irc_conversations,
        state->next_seq)) return snag_errno(EINVAL);
    const json_t *items = json_object_get(activity, "items");
    uint64_t after = 0u;
    if (activity && snag_json_integer_u64(activity, "after", &after) < 0) return -1;
    unsigned char header[17] = {activity ? 1u : 0u};
    put_number(header + 1u, after);
    put_number(header + 9u, json_object_size(items));
    if (snag_buf_append(out, header, sizeof(header)) < 0) return -1;
    const char *id;
    const json_t *item;
    json_object_foreach((json_t *)items, id, item) {
        unsigned char row[48];
        for (size_t i = 0u; i < 16u; ++i) {
            unsigned int hi = id[i * 2u] <= '9' ? (unsigned)(id[i * 2u] - '0') :
                (unsigned)(id[i * 2u] - 'a') + 10u;
            unsigned int lo = id[i * 2u + 1u] <= '9' ? (unsigned)(id[i * 2u + 1u] - '0') :
                (unsigned)(id[i * 2u + 1u] - 'a') + 10u;
            row[i] = (unsigned char)(hi << 4u | lo);
        }
        static const char *fields[] = {"seq", "time", "received", "incoming"};
        for (size_t i = 0u; i < sizeof(fields) / sizeof(fields[0]); ++i) {
            uint64_t number;
            if (snag_json_integer_u64(item, fields[i], &number) < 0) return -1;
            put_number(row + 16u + i * 8u, number);
        }
        if (snag_buf_append(out, row, sizeof(row)) < 0) return -1;
    }
    return 0;
}

static int
activity_read(const struct snag_binary_checkpoint_section *block,
    struct snag_session *state)
{
    if (!block->data || block->size < 17u) return snag_errno(EINVAL);
    const unsigned char *bytes = block->data;
    uint64_t after = get_number(bytes + 1u);
    uint64_t count = get_number(bytes + 9u);
    if (bytes[0] > 1u || after > INT64_MAX || count > (block->size - 17u) / 48u ||
        block->size != 17u + count * 48u) return snag_errno(EINVAL);
    if (!bytes[0]) {
        return !after && !count ? 0 : snag_errno(EINVAL);
    }
    json_t *activity = json_pack("{s:I,s:{}}", "after", (json_int_t)after, "items");
    if (!activity) return snag_errno(ENOMEM);
    json_t *items = json_object_get(activity, "items");
    for (uint64_t i = 0u; i < count; ++i) {
        const unsigned char *row = bytes + 17u + (size_t)i * 48u;
        char id[SNAG_ID_HEX_LEN + 1u];
        bytes_hex(id, row, 16u);
        uint64_t values[4];
        for (size_t j = 0u; j < sizeof(values) / sizeof(values[0]); ++j) {
            values[j] = get_number(row + 16u + j * 8u);
            if (values[j] > INT64_MAX) goto invalid;
        }
        if (json_object_get(items, id)) goto invalid;
        json_t *item = json_pack("{s:I,s:I,s:I,s:I}",
            "seq", (json_int_t)values[0], "time", (json_int_t)values[1],
            "received", (json_int_t)values[2], "incoming", (json_int_t)values[3]);
        if (!item || snag_json_set_new(items, id, item) < 0) goto fail;
    }
    if (!snag_irc_activity_valid(activity, state->irc_conversations, state->next_seq)) {
        goto invalid;
    }
    state->irc_activity = activity;
    return 0;
invalid:
    snag_errno(EINVAL);
fail:
    json_decref(activity);
    return -1;
}

static const struct text_slot {
    const char *key;
    size_t offset;
} text_slots[] = {
#define SLOT(f) {#f, offsetof(struct snag_session, f)}
    SLOT(cwd), SLOT(first_user), SLOT(last_user), SLOT(active_prompt), SLOT(goal_prompt),
    SLOT(goal_blocker), SLOT(timer_text), SLOT(banner_text), SLOT(steering_override), SLOT(name),
    SLOT(service_tier), SLOT(retry_auto)
#undef SLOT
};

static int
goal_wait_encode(struct snag_buf *out, const struct snag_binary_checkpoint_sources *sources,
    const struct snag_session *state)
{
    const json_t *value = json_object_get(state->strings, "goal_wait_for");
    uint64_t sequence = 0u;
    if (value) {
        sequence = sources->texts.slots[SNAG_BINARY_TEXT_GOAL_BLOCKER].declaration;
        if (state->goal_status != SNAG_GOAL_BLOCKED || !sequence ||
            !snag_goal_wait_valid(json_string_value(value))) return snag_errno(EINVAL);
    }
    unsigned char bytes[8];
    put_number(bytes, sequence);
    return snag_buf_append(out, bytes, sizeof(bytes));
}

static int
goal_wait_read(int fd, const struct snag_binary_anchor *through,
    const struct snag_binary_checkpoint_index *access,
    const struct snag_binary_checkpoint_section *block,
    const struct snag_binary_checkpoint_sources *sources, struct snag_session *state)
{
    if (block->size && (!block->data || block->size != 8u)) return snag_errno(EINVAL);
    uint64_t reference = block->size ? get_number(block->data) : 0u;
    uint64_t sequence = sources->texts.slots[SNAG_BINARY_TEXT_GOAL_BLOCKER].declaration;
    if (state->goal_status != SNAG_GOAL_BLOCKED || !sequence) {
        return reference ? snag_errno(EINVAL) : 0;
    }
    if (reference && reference != sequence) return snag_errno(EINVAL);
    const char *type = NULL;
    json_t *data = NULL;
    if (snag_binary_checkpoint_projection_read(fd, through, access, sequence, &type, &data) < 0) {
        return -1;
    }
    json_t *value = json_object_get(data, "wait_for");
    const char *id = snag_json_string(data, "goal_id");
    const char *actor = snag_json_string(data, "actor");
    /* Operator replacement keeps the blocked state, reason and wait channel;
     * the retained blocker declaration can belong to an earlier goal ID. */
    bool valid = type && !strcmp(type, "goal_blocked") && id &&
        snag_hex_is_lower(id, SNAG_ID_HEX_LEN) &&
        actor && !strcmp(actor, "model") &&
        (value ? reference && snag_goal_wait_valid(json_string_value(value)) : !reference);
    int rc = valid ? 0 : snag_errno(EINVAL);
    if (!rc && value) rc = json_object_set(state->strings, "goal_wait_for", value);
    json_decref(data);
    return rc;
}

int
snag_binary_checkpoint_core_encode_version(struct snag_buf *out,
    const struct snag_binary_checkpoint_sources *sources, const struct snag_session *state,
    uint16_t version)
{
    if (!out || !sources || !state || out->len > out->max) return snag_errno(EINVAL);
    size_t count = core_blocks(version);
    if (!count || (version == 2u && (state->irc_conversations || state->irc_activity)) ||
        (count <= GOAL_WAIT && json_object_get(state->strings, "goal_wait_for"))) {
        return snag_errno(EINVAL);
    }
    const struct snag_voice_history_root *voice = &state->voice_history;
    if (voice->adopted_seq) {
        if (voice->adopted_seq >= state->next_seq || voice->begin.next_seq < 2u ||
            voice->begin.next_seq >= voice->adopted_seq ||
            voice->begin.offset < SNAG_BINARY_HEADER_SIZE || voice->begin.offset > state->log_end ||
            !snag_hex_is_lower(voice->transfer_id, SNAG_ID_HEX_LEN) ||
            !snag_hex_is_lower(voice->begin.prev_sha256, SNAG_SHA256_HEX_LEN))
            return snag_errno(EINVAL);
    } else if (voice->transfer_id[0] || voice->begin.offset ||
        voice->begin.next_seq || voice->begin.prev_sha256[0]) return snag_errno(EINVAL);
    struct snag_buf blocks[BLOCKS] = {0}, staged = {.max = out->max - out->len};
    for (size_t i = 0u; i < BLOCKS; ++i) blocks[i].max = staged.max;
    struct snag_binary_checkpoint_accounting accounting = {
        .active_accounting = state->active_accounting, .usage_anchor = state->usage_anchor,
        .context_meter = state->context_meter, .capacity_rejection = state->capacity_rejection,
        .usage_totals = state->usage_totals};
    int rc = -1;
    if (snag_binary_checkpoint_controls_encode(&blocks[CONTROLS], state) < 0 ||
        snag_binary_checkpoint_accounting_encode(&blocks[ACCOUNTING], &accounting) < 0 ||
        snag_binary_checkpoint_texts_encode(&blocks[TEXTS], &sources->texts) < 0 ||
        snag_binary_checkpoint_calls_encode(&blocks[CALLS], &sources->calls, state) < 0 ||
        snag_binary_checkpoint_processes_encode(&blocks[PROCESSES], sources, state) < 0 ||
        snag_binary_checkpoint_inputs_encode(&blocks[INPUTS], sources, state) < 0 ||
        snag_binary_checkpoint_payloads_encode(&blocks[PAYLOADS], sources, state) < 0 ||
        (count > IRC && irc_encode(&blocks[IRC], state) < 0) ||
        (count > ACTIVITY && activity_encode(&blocks[ACTIVITY], state) < 0) ||
        (count > GOAL_WAIT && goal_wait_encode(&blocks[GOAL_WAIT], sources, state) < 0)) goto done;
    unsigned char header[CORE_HEADER] = {(unsigned char)version, 0u, (unsigned char)count, 0u};
    put_number(header + 4u, sources->active_compact);
    put_number(header + 12u, sources->response_start);
    put_number(header + 20u, voice->adopted_seq);
    for (size_t i = 0u; i < count; ++i) put_number(header + 28u + i * 8u, blocks[i].len);
    if (snag_buf_append(&staged, header, 28u + count * 8u) < 0) goto done;
    for (size_t i = 0u; i < count; ++i) {
        if (snag_buf_append(&staged, blocks[i].data, blocks[i].len) < 0) goto done;
    }
    rc = snag_buf_append(out, staged.data, staged.len);
done:
    for (size_t i = 0u; i < BLOCKS; ++i) snag_buf_free(&blocks[i]);
    snag_buf_free(&staged);
    return rc;
}

int
snag_binary_checkpoint_core_encode(struct snag_buf *out,
    const struct snag_binary_checkpoint_sources *sources, const struct snag_session *state)
{
    return snag_binary_checkpoint_core_encode_version(out, sources, state,
        SNAG_BINARY_CORE_VERSION);
}

static int
split(const struct snag_binary_checkpoint_section *core,
    struct snag_binary_checkpoint_section blocks[BLOCKS],
    struct snag_binary_checkpoint_sources *sources, uint64_t *voice)
{
    size_t count = core_blocks(core->version);
    size_t header = 28u + count * 8u;
    if (!count ||
        !core->data || core->size < header) return snag_errno(EINVAL);
    const unsigned char *bytes = core->data;
    if (bytes[0] != core->version || bytes[1] || bytes[2] != count || bytes[3])
        return snag_errno(EINVAL);
    sources->active_compact = get_number(bytes + 4u);
    sources->response_start = get_number(bytes + 12u);
    *voice = get_number(bytes + 20u);
    size_t position = header;
    for (size_t i = 0u; i < count; ++i) {
        uint64_t size = get_number(bytes + 28u + i * 8u);
        if (!size || size > core->size - position) return snag_errno(EINVAL);
        blocks[i] = (struct snag_binary_checkpoint_section){.data = bytes + position,
            .size = (size_t)size};
        position += (size_t)size;
    }
    return position == core->size ? 0 : snag_errno(EINVAL);
}

static int
copy_sources(struct snag_binary_checkpoint_sources *sources,
    const struct snag_binary_checkpoint_processes *processes,
    const struct snag_binary_checkpoint_inputs *inputs,
    const struct snag_binary_checkpoint_payloads *payloads, const struct snag_session *state)
{
    if (processes->count > SIZE_MAX / sizeof(*sources->processes) ||
        inputs->queue_count > SIZE_MAX / sizeof(*sources->queue) ||
        payloads->download_count > SIZE_MAX / sizeof(*sources->downloads))
        return snag_errno(EOVERFLOW);
    sources->process_count = processes->count;
    sources->queue_count = inputs->queue_count;
    sources->download_count = payloads->download_count;
    sources->input = inputs->input;
    sources->compact_start = payloads->compact_start;
    sources->compact_end = payloads->compact_end;
    sources->response_end = payloads->response_end;
    sources->resume_options = payloads->resume_options;
    if (sources->process_count) {
        sources->processes = calloc(sources->process_count, sizeof(*sources->processes));
        if (!sources->processes) return -1;
    }
    if (sources->queue_count) {
        sources->queue = calloc(sources->queue_count, sizeof(*sources->queue));
        if (!sources->queue) return -1;
    }
    if (sources->download_count) {
        sources->downloads = calloc(sources->download_count, sizeof(*sources->downloads));
        if (!sources->downloads) return -1;
    }
    for (size_t i = 0u; i < sources->process_count; ++i) {
        if (snag_binary_checkpoint_processes_origin(processes, i, &sources->processes[i]) < 0)
            return -1;
        memcpy(sources->processes[i].handle, state->processes[i].handle,
            sizeof(sources->processes[i].handle));
    }
    for (size_t i = 0u; i < sources->queue_count; ++i) {
        sources->queue[i].creation = get_number(inputs->queue + i * 24u);
        sources->queue[i].text = get_number(inputs->queue + i * 24u + 8u);
    }
    for (size_t i = 0u; i < sources->download_count; ++i) {
        const char *id = snag_json_string(json_array_get(state->download_queue, i), "id");
        if (!snag_hex_is_lower(id, SNAG_ID_HEX_LEN)) return snag_errno(EINVAL);
        memcpy(sources->downloads[i].id, id, sizeof(sources->downloads[i].id));
        sources->downloads[i].receipt = get_number(payloads->downloads + i * 8u);
    }
    return 0;
}

static int
check_sources(const struct snag_binary_checkpoint_sources *sources,
    const struct snag_binary_checkpoint_payloads *payloads, const struct snag_session *state)
{
    uint64_t through = sources->texts.through;
    if (through >= state->next_seq || sources->input > through ||
        sources->calls.graph > through || sources->compact_end > through ||
        sources->response_end > through || sources->resume_options > through ||
        (payloads->response_start && payloads->response_start != sources->response_start) ||
        (payloads->turn &&
            payloads->turn != sources->texts.slots[SNAG_BINARY_TEXT_ACTIVE_PROMPT].declaration))
        return snag_errno(EINVAL);
    for (size_t i = 0u; i < sources->process_count; ++i) {
        if (sources->processes[i].started > through) return snag_errno(EINVAL);
    }
    for (size_t i = 0u; i < sources->queue_count; ++i) {
        if (sources->queue[i].text > through) return snag_errno(EINVAL);
    }
    for (size_t i = 0u; i < sources->download_count; ++i) {
        if (sources->downloads[i].receipt > through) return snag_errno(EINVAL);
    }
    for (size_t i = 0u; i < state->pending_steering_count; ++i) {
        if (state->pending_steering[i].seq > through) return snag_errno(EINVAL);
    }
    return 0;
}

int
snag_binary_checkpoint_core_read(int fd, const struct snag_binary_checkpoint_frame *frame,
    const struct snag_binary_checkpoint_index *access,
    struct snag_session *out, struct snag_binary_checkpoint_sources *out_sources)
{
    if (fd < 0 || !frame || !out || !out_sources || !frame->boundary.next_seq ||
        frame->boundary.end < SNAG_BINARY_HEADER_SIZE || frame->boundary.end > INT64_MAX ||
        frame->boundary.turns >= frame->boundary.next_seq) return snag_errno(EINVAL);
    struct snag_session *state = malloc(sizeof(*state));
    if (!state) return snag_errno(ENOMEM);
    snag_session_init(state);
    struct snag_binary_checkpoint_sources sources = {0};
    struct snag_binary_checkpoint_section blocks[BLOCKS] = {0};
    struct snag_binary_checkpoint_accounting accounting;
    struct snag_binary_checkpoint_calls calls;
    struct snag_binary_checkpoint_processes processes;
    struct snag_binary_checkpoint_inputs inputs;
    struct snag_binary_checkpoint_payloads payloads;
    struct snag_binary_checkpoint_inputs_state input_state = {0};
    struct snag_binary_checkpoint_payloads_state payload_state = {0};
    uint64_t voice;
    int rc = -1;
    if (split(&frame->core, blocks, &sources, &voice) < 0) goto done;
#define DECODE(n, f, v) snag_binary_checkpoint_##f##_decode(blocks[n].data, blocks[n].size, v)
    if (DECODE(CONTROLS, controls, state) < 0 ||
        DECODE(ACCOUNTING, accounting, &accounting) < 0 ||
        DECODE(TEXTS, texts, &sources.texts) < 0 || DECODE(CALLS, calls, &calls) < 0 ||
        DECODE(PROCESSES, processes, &processes) < 0 || DECODE(INPUTS, inputs, &inputs) < 0 ||
        DECODE(PAYLOADS, payloads, &payloads) < 0) goto done;
#undef DECODE
    bytes_hex(state->id, frame->identity.id, sizeof(frame->identity.id));
    bytes_hex(state->prev_sha256, frame->boundary.digest, sizeof(frame->boundary.digest));
    state->next_seq = frame->boundary.next_seq;
    state->log_end = (int64_t)frame->boundary.end;
    state->turn_count = frame->boundary.turns;
    state->active_accounting = accounting.active_accounting;
    state->usage_anchor = accounting.usage_anchor;
    state->context_meter = accounting.context_meter;
    state->capacity_rejection = accounting.capacity_rejection;
    state->usage_totals = accounting.usage_totals;
    sources.calls = calls.source;
    const struct snag_binary_anchor *anchor = &frame->boundary;
    if (voice && snag_binary_checkpoint_voice_read(fd, anchor, access, voice, state->id,
        &state->voice_history) < 0) goto done;
    if (snag_binary_checkpoint_texts_read(fd, anchor, access, &sources.texts,
            &state->strings) < 0 ||
        snag_binary_checkpoint_calls_read(fd, anchor, access, &calls, state,
            &state->pending_calls) < 0 ||
        snag_binary_checkpoint_processes_read(fd, anchor, access, &processes, state,
            &state->processes) < 0 ||
        snag_binary_checkpoint_inputs_read(fd, anchor, access, &inputs, state, &input_state) < 0 ||
        snag_binary_checkpoint_payloads_read(fd, anchor, access, &payloads, state,
            &payload_state) < 0)
        goto done;
    if (input_state.queue_bytes != state->pending_queue_bytes ||
        input_state.steering_bytes != state->pending_steering_bytes ||
        payload_state.response_public_bytes != state->response_public_bytes) {
        snag_errno(EINVAL);
        goto done;
    }
    if (json_object_update(state->strings, input_state.strings) < 0) {
        snag_errno(ENOMEM);
        goto done;
    }
    if (payload_state.resume_options &&
        json_object_set(state->strings, "resume_options", payload_state.resume_options) < 0) {
        snag_errno(ENOMEM);
        goto done;
    }
    json_decref(payload_state.resume_options);
    payload_state.resume_options = NULL;
    for (size_t i = 0u; i < sizeof(text_slots) / sizeof(*text_slots); ++i) {
        const char *text = snag_json_string(state->strings, text_slots[i].key);
        memcpy((unsigned char *)state + text_slots[i].offset, &text, sizeof(text));
    }
    if (!json_object_size(state->strings)) { json_decref(state->strings); state->strings = NULL; }
    state->pending_call_count = state->pending_call_capacity = calls.count;
    state->process_count = state->process_capacity = processes.count;
    state->pending_input = input_state.input;
    state->pending_queue = input_state.queue;
    state->pending_queue_count = state->pending_queue_capacity = input_state.queue_count;
    state->pending_steering = input_state.steering;
    state->pending_steering_count = state->pending_steering_capacity = input_state.steering_count;
    json_decref(input_state.strings);
    input_state = (struct snag_binary_checkpoint_inputs_state){0};
    state->active_instructions = payload_state.instructions;
    state->compact_output = payload_state.compact_output;
    state->response_public = payload_state.response_public;
    state->download_queue = payload_state.downloads;
    payload_state = (struct snag_binary_checkpoint_payloads_state){0};
    if ((blocks[IRC].size &&
        irc_read(fd, anchor, access, &blocks[IRC], &state->irc_conversations) < 0) ||
        (blocks[ACTIVITY].size && activity_read(&blocks[ACTIVITY], state) < 0) ||
        goal_wait_read(fd, anchor, access, &blocks[GOAL_WAIT], &sources, state) < 0 ||
        copy_sources(&sources, &processes, &inputs, &payloads, state) < 0 ||
        check_sources(&sources, &payloads, state) < 0 ||
        snag_binary_checkpoint_epochs_check(fd, anchor, access, &sources, state) < 0) goto done;
    *out = *state;
    *out_sources = sources;
    free(state);
    return 0;
done:
    snag_binary_checkpoint_payloads_free(&payload_state);
    snag_binary_checkpoint_inputs_free(&input_state);
    snag_binary_checkpoint_sources_free(&sources);
    snag_session_close(state);
    free(state);
    return rc;
}
