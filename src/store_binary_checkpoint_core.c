/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_checkpoint.h"
#include "json.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Each component keeps its own version. Sizes delimit the fixed-order blocks;
 * the two extra epochs and adoption reference survive without copied payloads. */
enum core_block { CONTROLS, ACCOUNTING, TEXTS, CALLS, PROCESSES, INPUTS, PAYLOADS, BLOCKS };
#define CORE_HEADER (28u + BLOCKS * 8u)

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

static const struct text_slot {
    const char *key;
    size_t offset;
} text_slots[] = {
#define SLOT(f) {#f, offsetof(struct snag_session, f)}
    SLOT(cwd), SLOT(first_user), SLOT(last_user), SLOT(active_prompt), SLOT(goal_prompt),
    SLOT(goal_blocker), SLOT(timer_text), SLOT(banner_text), SLOT(steering_override), SLOT(name)
#undef SLOT
};

int
snag_binary_checkpoint_core_encode(struct snag_buf *out,
    const struct snag_binary_checkpoint_sources *sources, const struct snag_session *state)
{
    if (!out || !sources || !state || out->len > out->max) return snag_errno(EINVAL);
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
        snag_binary_checkpoint_payloads_encode(&blocks[PAYLOADS], sources, state) < 0) goto done;
    unsigned char header[CORE_HEADER] = {SNAG_BINARY_CORE_VERSION, 0u, BLOCKS, 0u};
    put_number(header + 4u, sources->active_compact);
    put_number(header + 12u, sources->response_start);
    put_number(header + 20u, voice->adopted_seq);
    for (size_t i = 0u; i < BLOCKS; ++i) put_number(header + 28u + i * 8u, blocks[i].len);
    if (snag_buf_append(&staged, header, sizeof(header)) < 0) goto done;
    for (size_t i = 0u; i < BLOCKS; ++i) {
        if (snag_buf_append(&staged, blocks[i].data, blocks[i].len) < 0) goto done;
    }
    rc = snag_buf_append(out, staged.data, staged.len);
done:
    for (size_t i = 0u; i < BLOCKS; ++i) snag_buf_free(&blocks[i]);
    snag_buf_free(&staged);
    return rc;
}

static int
split(const struct snag_binary_checkpoint_section *core,
    struct snag_binary_checkpoint_section blocks[BLOCKS],
    struct snag_binary_checkpoint_sources *sources, uint64_t *voice)
{
    if (core->version != SNAG_BINARY_CORE_VERSION || !core->data || core->size < CORE_HEADER)
        return snag_errno(EINVAL);
    const unsigned char *bytes = core->data;
    if (bytes[0] != SNAG_BINARY_CORE_VERSION || bytes[1] || bytes[2] != BLOCKS || bytes[3])
        return snag_errno(EINVAL);
    sources->active_compact = get_number(bytes + 4u);
    sources->response_start = get_number(bytes + 12u);
    *voice = get_number(bytes + 20u);
    size_t position = CORE_HEADER;
    for (size_t i = 0u; i < BLOCKS; ++i) {
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
    struct snag_session *out, struct snag_binary_checkpoint_sources *out_sources)
{
    if (fd < 0 || !frame || !out || !out_sources || !frame->boundary.next_seq ||
        frame->boundary.end < SNAG_BINARY_HEADER_SIZE || frame->boundary.end > INT64_MAX ||
        frame->boundary.turns >= frame->boundary.next_seq) return snag_errno(EINVAL);
    struct snag_session state;
    snag_session_init(&state);
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
    if (DECODE(CONTROLS, controls, &state) < 0 ||
        DECODE(ACCOUNTING, accounting, &accounting) < 0 ||
        DECODE(TEXTS, texts, &sources.texts) < 0 || DECODE(CALLS, calls, &calls) < 0 ||
        DECODE(PROCESSES, processes, &processes) < 0 || DECODE(INPUTS, inputs, &inputs) < 0 ||
        DECODE(PAYLOADS, payloads, &payloads) < 0) goto done;
#undef DECODE
    bytes_hex(state.id, frame->identity.id, sizeof(frame->identity.id));
    bytes_hex(state.prev_sha256, frame->boundary.digest, sizeof(frame->boundary.digest));
    state.next_seq = frame->boundary.next_seq;
    state.log_end = (int64_t)frame->boundary.end;
    state.turn_count = frame->boundary.turns;
    state.active_accounting = accounting.active_accounting;
    state.usage_anchor = accounting.usage_anchor;
    state.context_meter = accounting.context_meter;
    state.capacity_rejection = accounting.capacity_rejection;
    state.usage_totals = accounting.usage_totals;
    sources.calls = calls.source;
    const struct snag_binary_anchor *anchor = &frame->boundary;
    if (voice && snag_binary_checkpoint_voice_read(fd, anchor, voice, state.id,
        &state.voice_history) < 0) goto done;
    if (snag_binary_checkpoint_texts_read(fd, anchor, NULL, &sources.texts, &state.strings) < 0 ||
        snag_binary_checkpoint_calls_read(fd, anchor, &calls, &state, &state.pending_calls) < 0 ||
        snag_binary_checkpoint_processes_read(fd, anchor, &processes, &state,
            &state.processes) < 0 ||
        snag_binary_checkpoint_inputs_read(fd, anchor, &inputs, &state, &input_state) < 0 ||
        snag_binary_checkpoint_payloads_read(fd, anchor, &payloads, &state, &payload_state) < 0)
        goto done;
    if (input_state.queue_bytes != state.pending_queue_bytes ||
        input_state.steering_bytes != state.pending_steering_bytes ||
        payload_state.response_public_bytes != state.response_public_bytes) {
        snag_errno(EINVAL);
        goto done;
    }
    if (json_object_update(state.strings, input_state.strings) < 0) {
        snag_errno(ENOMEM);
        goto done;
    }
    if (payload_state.resume_options &&
        json_object_set(state.strings, "resume_options", payload_state.resume_options) < 0) {
        snag_errno(ENOMEM);
        goto done;
    }
    json_decref(payload_state.resume_options);
    payload_state.resume_options = NULL;
    for (size_t i = 0u; i < sizeof(text_slots) / sizeof(*text_slots); ++i) {
        const char *text = snag_json_string(state.strings, text_slots[i].key);
        memcpy((unsigned char *)&state + text_slots[i].offset, &text, sizeof(text));
    }
    if (!json_object_size(state.strings)) { json_decref(state.strings); state.strings = NULL; }
    state.pending_call_count = state.pending_call_capacity = calls.count;
    state.process_count = state.process_capacity = processes.count;
    state.pending_input = input_state.input;
    state.pending_queue = input_state.queue;
    state.pending_queue_count = state.pending_queue_capacity = input_state.queue_count;
    state.pending_steering = input_state.steering;
    state.pending_steering_count = state.pending_steering_capacity = input_state.steering_count;
    json_decref(input_state.strings);
    input_state = (struct snag_binary_checkpoint_inputs_state){0};
    state.active_instructions = payload_state.instructions;
    state.compact_output = payload_state.compact_output;
    state.response_public = payload_state.response_public;
    state.download_queue = payload_state.downloads;
    payload_state = (struct snag_binary_checkpoint_payloads_state){0};
    if (copy_sources(&sources, &processes, &inputs, &payloads, &state) < 0 ||
        check_sources(&sources, &payloads, &state) < 0 ||
        snag_binary_checkpoint_epochs_check(fd, anchor, &sources, &state) < 0) goto done;
    *out = state;
    *out_sources = sources;
    return 0;
done:
    snag_binary_checkpoint_payloads_free(&payload_state);
    snag_binary_checkpoint_inputs_free(&input_state);
    snag_binary_checkpoint_sources_free(&sources);
    snag_session_close(&state);
    return rc;
}
