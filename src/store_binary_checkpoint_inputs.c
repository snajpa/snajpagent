/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_checkpoint.h"
#include "json.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define INPUT_HEADER 26u
#define QUEUE_ENTRY 24u
#define STEERING_ENTRY 16u

static void
put_number(unsigned char *out, uint64_t value)
{
    for (size_t i = 0u; i < 8u; ++i) out[i] = (unsigned char)(value >> (i * 8u));
}

static uint64_t
get_number(const unsigned char *data)
{
    uint64_t value = 0u;
    for (size_t i = 0u; i < 8u; ++i) value |= (uint64_t)data[i] << (i * 8u);
    return value;
}

static bool
valid_entries(const struct snag_binary_checkpoint_inputs *view)
{
    if (view->input == UINT64_MAX || (view->queue_count && !view->queue) ||
        (view->steering_count && !view->steering)) return false;
    uint64_t previous = 0u;
    for (size_t i = 0u; i < view->queue_count; ++i) {
        const unsigned char *entry = view->queue + i * QUEUE_ENTRY;
        uint64_t creation = get_number(entry), text = get_number(entry + 8u);
        if (creation <= previous || text < creation || text == UINT64_MAX) return false;
        previous = creation;
    }
    previous = 0u;
    for (size_t i = 0u; i < view->steering_count; ++i) {
        uint64_t receipt = get_number(view->steering + i * STEERING_ENTRY);
        if (receipt <= previous || receipt == UINT64_MAX) return false;
        previous = receipt;
    }
    return true;
}

int
snag_binary_checkpoint_inputs_decode(const void *data, size_t size,
    struct snag_binary_checkpoint_inputs *out)
{
    if (!data || !out || size < INPUT_HEADER) return snag_errno(EINVAL);
    const unsigned char *bytes = data;
    uint64_t queue = get_number(bytes + 10u), steering = get_number(bytes + 18u);
    if (bytes[0] != 1u || bytes[1] || queue > (size - INPUT_HEADER) / QUEUE_ENTRY)
        return snag_errno(EINVAL);
    size_t rest = size - INPUT_HEADER - (size_t)queue * QUEUE_ENTRY;
    if (rest % STEERING_ENTRY || steering != rest / STEERING_ENTRY) return snag_errno(EINVAL);
    struct snag_binary_checkpoint_inputs value = {
        .input = get_number(bytes + 2u), .queue = bytes + INPUT_HEADER,
        .steering = bytes + INPUT_HEADER + (size_t)queue * QUEUE_ENTRY,
        .queue_count = (size_t)queue, .steering_count = (size_t)steering};
    if (!valid_entries(&value)) return snag_errno(EINVAL);
    *out = value;
    return 0;
}

int
snag_binary_checkpoint_inputs_encode(struct snag_buf *out,
    const struct snag_binary_checkpoint_sources *sources, const struct snag_session *state)
{
    if (!out || !sources || !state || sources->queue_count != state->pending_queue_count ||
        !!sources->input != !!state->pending_input ||
        (sources->queue_count && (!sources->queue || !state->pending_queue)) ||
        (state->pending_steering_count && !state->pending_steering)) return snag_errno(EINVAL);
    size_t queue = sources->queue_count, steering = state->pending_steering_count;
    if (queue > (SIZE_MAX - INPUT_HEADER) / QUEUE_ENTRY) return snag_errno(EOVERFLOW);
    size_t size = INPUT_HEADER + queue * QUEUE_ENTRY;
    if (steering > (SIZE_MAX - size) / STEERING_ENTRY) return snag_errno(EOVERFLOW);
    size += steering * STEERING_ENTRY;
    struct snag_buf encoded = {.max = size};
    unsigned char bytes[INPUT_HEADER] = {1u};
    put_number(bytes + 2u, sources->input);
    put_number(bytes + 10u, queue);
    put_number(bytes + 18u, steering);
    int rc = snag_buf_append(&encoded, bytes, INPUT_HEADER);
    for (size_t i = 0u; !rc && i < queue; ++i) {
        if (sources->queue[i].creation != state->pending_queue[i].seq) {
            rc = snag_errno(EINVAL);
            break;
        }
        put_number(bytes, sources->queue[i].creation);
        put_number(bytes + 8u, sources->queue[i].text);
        put_number(bytes + 16u, state->pending_queue[i].first_context_ms);
        rc = snag_buf_append(&encoded, bytes, QUEUE_ENTRY);
    }
    for (size_t i = 0u; !rc && i < steering; ++i) {
        put_number(bytes, state->pending_steering[i].seq);
        put_number(bytes + 8u, state->pending_steering[i].first_context_ms);
        rc = snag_buf_append(&encoded, bytes, STEERING_ENTRY);
    }
    struct snag_binary_checkpoint_inputs view;
    if (!rc) rc = snag_binary_checkpoint_inputs_decode(encoded.data, encoded.len, &view);
    if (!rc) rc = snag_buf_append(out, encoded.data, encoded.len);
    snag_buf_free(&encoded);
    return rc;
}

void
snag_binary_checkpoint_inputs_free(struct snag_binary_checkpoint_inputs_state *state)
{
    if (!state) return;
    for (size_t i = 0u; i < state->queue_count; ++i) json_decref(state->queue[i].content);
    for (size_t i = 0u; i < state->steering_count; ++i) json_decref(state->steering[i].content);
    free(state->queue);
    free(state->steering);
    json_decref(state->input);
    json_decref(state->strings);
    memset(state, 0, sizeof(*state));
}

static int
retain_text(struct snag_binary_checkpoint_inputs_state *state, const json_t *receipt,
    const char *id, size_t limit, size_t total_limit, size_t *total, const char **text)
{
    json_t *value = json_object_get(receipt, "text");
    size_t size = json_string_length(value);
    if (!id || !snag_hex_is_lower(id, SNAG_ID_HEX_LEN) || !json_is_string(value) || !size ||
        size > limit || *total > total_limit - size || json_object_get(state->strings, id))
        return snag_errno(EINVAL);
    if (json_object_set(state->strings, id, value) < 0) return snag_errno(ENOMEM);
    *text = json_string_value(value);
    *total += size;
    return 0;
}

static int
received_time(const json_t *receipt, uint64_t fallback, uint64_t *out)
{
    if (json_object_get(receipt, "received_at_ms"))
        return snag_json_integer_u64(receipt, "received_at_ms", out);
    *out = fallback;
    return 0;
}

static int
read_queue(int fd, const struct snag_binary_anchor *anchor,
    const struct snag_binary_checkpoint_index *access, const unsigned char *entry,
    struct snag_binary_checkpoint_inputs_state *state, struct snag_queued_turn *out)
{
    uint64_t creation = get_number(entry), text = get_number(entry + 8u), timestamp;
    json_t *original = NULL, *latest = NULL;
    int rc = -1;
    if (snag_binary_checkpoint_receipt_read(fd, anchor, access, creation,
            SNAG_BINARY_FUTURE_TURN_QUEUED, &original, &timestamp) < 0) goto done;
    if (text != creation) {
        if (snag_binary_checkpoint_receipt_read(fd, anchor, access, text,
                SNAG_BINARY_FUTURE_TURN_EDITED, &latest, &timestamp) < 0) goto done;
    } else {
        latest = json_incref(original);
    }
    const char *id = snag_json_string(original, "queue_id");
    const char *edited_id = snag_json_string(latest, "queue_id");
    if (!id || !edited_id || strcmp(id, edited_id) ||
        !json_is_boolean(json_object_get(latest, "read_only"))) {
        snag_errno(EINVAL);
        goto done;
    }
    if (retain_text(state, latest, id, SNAG_MAX_QUEUED_TEXT, SNAG_MAX_PENDING_QUEUE_TEXT,
            &state->queue_bytes, &out->text) < 0 ||
        received_time(latest, timestamp, &out->received_ms) < 0) goto done;
    memcpy(out->queue_id, id, sizeof(out->queue_id));
    out->seq = creation;
    out->first_context_ms = get_number(entry + 16u);
    out->read_only = json_is_true(json_object_get(latest, "read_only"));
    /* An edit's content is admitted history but does not replace queue content. */
    out->content = json_incref(json_object_get(original, "content"));
    rc = 0;
done:
    json_decref(latest);
    json_decref(original);
    return rc;
}

static int
read_steering(int fd, const struct snag_binary_anchor *anchor,
    const struct snag_binary_checkpoint_index *access, const unsigned char *entry,
    const struct snag_session *snapshot, struct snag_binary_checkpoint_inputs_state *state,
    struct snag_pending_steering *out)
{
    uint64_t receipt = get_number(entry), timestamp;
    json_t *data = NULL;
    if (snag_binary_checkpoint_receipt_read(fd, anchor, access, receipt,
            SNAG_BINARY_STEERING_ADDED, &data, &timestamp) < 0) return -1;
    int rc = -1;
    const char *id = snag_json_string(data, "steering_id");
    const char *turn = snag_json_string(data, "turn_id");
    if (!snapshot->active_turn || !turn || strcmp(turn, snapshot->active_turn_id)) {
        snag_errno(EINVAL);
        goto done;
    }
    if (retain_text(state, data, id, SNAG_MAX_STEERING_TEXT, SNAG_MAX_PENDING_STEERING_BYTES,
            &state->steering_bytes, &out->text) < 0 ||
        received_time(data, timestamp, &out->received_ms) < 0) goto done;
    memcpy(out->steering_id, id, sizeof(out->steering_id));
    out->seq = receipt;
    out->first_context_ms = get_number(entry + 8u);
    out->content = json_incref(json_object_get(data, "content"));
    rc = 0;
done:
    json_decref(data);
    return rc;
}

int
snag_binary_checkpoint_inputs_read(int fd, const struct snag_binary_anchor *anchor,
    const struct snag_binary_checkpoint_index *access,
    const struct snag_binary_checkpoint_inputs *view, const struct snag_session *snapshot,
    struct snag_binary_checkpoint_inputs_state *out)
{
    if (fd < 0 || !anchor || !anchor->next_seq || !view || !snapshot || !out)
        return snag_errno(EINVAL);
    if (view->queue_count > SIZE_MAX / QUEUE_ENTRY ||
        view->queue_count > SIZE_MAX / sizeof(struct snag_queued_turn) ||
        view->steering_count > SIZE_MAX / STEERING_ENTRY ||
        view->steering_count > SIZE_MAX / sizeof(struct snag_pending_steering))
        return snag_errno(EOVERFLOW);
    if (!valid_entries(view) || (view->input && snapshot->active_turn)) return snag_errno(EINVAL);
    struct snag_binary_checkpoint_inputs_state state = {0};
    uint64_t timestamp;
    if (view->input && snag_binary_checkpoint_receipt_read(fd, anchor, access, view->input,
            SNAG_BINARY_INPUT_RECEIVED, &state.input, &timestamp) < 0) goto fail;
    state.strings = json_object();
    if (!state.strings) {
        snag_errno(ENOMEM);
        goto fail;
    }
    if (view->queue_count) {
        state.queue = calloc(view->queue_count, sizeof(*state.queue));
        if (!state.queue) goto fail;
        state.queue_count = view->queue_count;
    }
    if (view->steering_count) {
        state.steering = calloc(view->steering_count, sizeof(*state.steering));
        if (!state.steering) goto fail;
        state.steering_count = view->steering_count;
    }
    for (size_t i = 0u; i < state.queue_count; ++i) {
        if (read_queue(fd, anchor, access, view->queue + i * QUEUE_ENTRY, &state,
                &state.queue[i]) < 0) goto fail;
    }
    for (size_t i = 0u; i < state.steering_count; ++i) {
        if (read_steering(fd, anchor, access, view->steering + i * STEERING_ENTRY, snapshot,
                &state, &state.steering[i]) < 0) goto fail;
    }
    *out = state;
    return 0;
fail:
    snag_binary_checkpoint_inputs_free(&state);
    return -1;
}
