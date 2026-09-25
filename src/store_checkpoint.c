/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_internal.h"
#include "base.h"
#include "fs.h"
#include "json.h"
#include "snajpagent.h"
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A private, replaceable recovery accelerator. The journal is still the record.
 * Checkpoints are trusted only up to their own offset; the suffix is replayed
 * and verified against the stored chain head. Never serialize C pointers. */
enum checkpoint_kind { CK_TEXT, CK_UNSIGNED, CK_BOOLEAN };
struct checkpoint_field { const char *name; size_t offset, width; enum checkpoint_kind kind; };
#define S(T, f) {#f, offsetof(T, f), sizeof(((T *)0)->f), CK_TEXT}
#define U(T, f) {#f, offsetof(T, f), sizeof(((T *)0)->f), CK_UNSIGNED}
#define B(T, f) {#f, offsetof(T, f), sizeof(((T *)0)->f), CK_BOOLEAN}
#define COUNT(x) (sizeof(x) / sizeof((x)[0]))
static const struct checkpoint_field session_fields[] = {
    S(struct snag_session, id),
    S(struct snag_session, prev_sha256),
    S(struct snag_session, active_turn_id),
    S(struct snag_session, active_response_id),
    S(struct snag_session, final_item_id),
    S(struct snag_session, final_response_id),
    S(struct snag_session, compact_id),
    S(struct snag_session, active_compact_id),
    S(struct snag_session, active_compact_source_sha256),
    S(struct snag_session, active_compact_scope),
    S(struct snag_session, default_provider),
    S(struct snag_session, goal_id),
    S(struct snag_session, goal_parent_id),
    S(struct snag_session, timer_id),
    S(struct snag_session, default_model),
    S(struct snag_session, active_turn_model),
    S(struct snag_session, active_turn_provider),
    S(struct snag_session, active_turn_effort),
    S(struct snag_session, capacity_ceiling_provider),
    S(struct snag_session, capacity_ceiling_model),
    S(struct snag_session, capacity_ceiling_source_sha256),
    S(struct snag_session, default_effort),
    S(struct snag_session, command_shell),
    S(struct snag_session, trash_name),
    S(struct snag_session, compact_scope),
    S(struct snag_session, context_rebase_turn_id),
    U(struct snag_session, irc_received_seq),
    U(struct snag_session, irc_consumed_seq),
    U(struct snag_session, response_irc_seq),
    U(struct snag_session, max_parallel_commands),
    U(struct snag_session, default_yield_ms),
    U(struct snag_session, max_wait_ms),
    U(struct snag_session, default_timeout_ms),
    U(struct snag_session, max_timeout_ms),
    U(struct snag_session, tool_output_bytes),
    U(struct snag_session, output_cache_bytes),
    U(struct snag_session, context_mode),
    U(struct snag_session, context_tokens),
    U(struct snag_session, response_public_bytes),
    U(struct snag_session, log_end),
    U(struct snag_session, next_seq),
    U(struct snag_session, checkpoint_offset),
    U(struct snag_session, checkpoint_seq),
    U(struct snag_session, format_version),
    U(struct snag_session, turn_count),
    U(struct snag_session, last_time_ms),
    U(struct snag_session, compact_seq),
    U(struct snag_session, context_rebase_seq),
    U(struct snag_session, active_compact_source_seq),
    U(struct snag_session, capacity_ceiling_input_tokens),
    U(struct snag_session, input_received_ms),
    U(struct snag_session, input_first_context_ms),
    U(struct snag_session, recovery_count),
    U(struct snag_session, goal_revision),
    U(struct snag_session, goal_turn_count),
    U(struct snag_session, timer_due_ms),
    U(struct snag_session, pending_steering_bytes),
    U(struct snag_session, pending_queue_bytes),
    U(struct snag_session, active_cycle),
    U(struct snag_session, turn_retry_attempts),
    U(struct snag_session, turn_retry_limit),
    U(struct snag_session, response_outcome),
    U(struct snag_session, pending_controls),
    U(struct snag_session, started_controls),
    U(struct snag_session, compact_control_source_seq),
    U(struct snag_session, policy_stopped),
    U(struct snag_session, response_terminal),
    U(struct snag_session, goal_status),
    B(struct snag_session, parallel_tool_calls),
    B(struct snag_session, context_rebase_has_new_results),
    B(struct snag_session, legacy_journal),
    B(struct snag_session, compact_control_image_boundary),
    B(struct snag_session, queue_armed),
    B(struct snag_session, active_turn),
    B(struct snag_session, last_turn_failed),
    B(struct snag_session, retry_read_only),
    B(struct snag_session, active_read_only),
    B(struct snag_session, active_queued),
    B(struct snag_session, active_goal),
    B(struct snag_session, cancel_requested),
    B(struct snag_session, response_handoff),
    B(struct snag_session, archived),
    B(struct snag_session, delete_requested),
    B(struct snag_session, response_open),
    B(struct snag_session, response_complete),
    B(struct snag_session, irc_reply_reminded),
    B(struct snag_session, output_correction_used),
    B(struct snag_session, steering_deferred),
    B(struct snag_session, goal_locked),
    B(struct snag_session, capacity_ceiling_valid),
};
static const struct checkpoint_field observation_fields[] = {
    S(struct snag_input_observation, provider),
    S(struct snag_input_observation, model),
    S(struct snag_input_observation, effort),
    S(struct snag_input_observation, compact_id),
    S(struct snag_input_observation, provider_source_sha256),
    S(struct snag_input_observation, model_input_sha256),
    S(struct snag_input_observation, request_input_sha256),
    S(struct snag_input_observation, request_sha256),
    U(struct snag_input_observation, model_input_bytes),
    U(struct snag_input_observation, request_input_bytes),
    U(struct snag_input_observation, request_input_count),
    U(struct snag_input_observation, input_tokens),
    U(struct snag_input_observation, requested_output_tokens),
    B(struct snag_input_observation, valid),
};
static const struct checkpoint_field usage_fields[] = {
    U(struct snag_usage_totals, responses),
    U(struct snag_usage_totals, input_tokens),
    U(struct snag_usage_totals, cached_input_tokens),
    U(struct snag_usage_totals, uncached_input_tokens),
    U(struct snag_usage_totals, output_tokens),
    U(struct snag_usage_totals, reasoning_tokens),
    U(struct snag_usage_totals, total_tokens),
    B(struct snag_usage_totals, cached_seen),
};
static const struct checkpoint_field process_fields[] = {
    S(struct snag_process_state, handle),
    S(struct snag_process_state, command),
    S(struct snag_process_state, workdir),
    S(struct snag_process_state, log_hash),
    U(struct snag_process_state, input_accepted),
    U(struct snag_process_state, input_written),
    U(struct snag_process_state, input_pending),
    U(struct snag_process_state, log_offset),
    U(struct snag_process_state, log_seq),
    B(struct snag_process_state, ready),
    B(struct snag_process_state, draining),
};
static const struct checkpoint_field call_fields[] = {
    S(struct snag_pending_call, call_id),
    S(struct snag_pending_call, action_sha256),
    S(struct snag_pending_call, tool_name),
    S(struct snag_pending_call, process_handle),
    S(struct snag_pending_call, command),
    S(struct snag_pending_call, workdir),
    B(struct snag_pending_call, started),
    B(struct snag_pending_call, finished),
};
static const struct checkpoint_field steering_fields[] = {
    S(struct snag_pending_steering, steering_id),
    U(struct snag_pending_steering, seq),
    U(struct snag_pending_steering, received_ms),
    U(struct snag_pending_steering, first_context_ms),
};
static const struct checkpoint_field queue_fields[] = {
    S(struct snag_queued_turn, queue_id),
    U(struct snag_queued_turn, seq),
    U(struct snag_queued_turn, received_ms),
    U(struct snag_queued_turn, first_context_ms),
    B(struct snag_queued_turn, read_only),
};

static json_t *
encode_fields(const void *source, const struct checkpoint_field *fields, size_t count)
{
    json_t *out = json_object();
    if (!out) return NULL;
    for (size_t i = 0; i < count; ++i) {
        const struct checkpoint_field *f = &fields[i];
        const unsigned char *p = (const unsigned char *)source + f->offset;
        uint64_t n = 0;
        json_t *item = NULL;
        if (f->kind == CK_TEXT) {
            size_t len = strnlen((const char *)p, f->width);
            if (len == f->width) goto fail;
            item = json_stringn((const char *)p, len);
        } else if (f->kind == CK_BOOLEAN) {
            bool value;
            memcpy(&value, p, sizeof(value));
            item = json_boolean(value);
        } else {
            if (f->width == sizeof(uint64_t)) memcpy(&n, p, f->width);
            else if (f->width == sizeof(uint32_t)) { uint32_t v; memcpy(&v, p, sizeof(v)); n = v; }
            else if (f->width == sizeof(unsigned int)) {
                unsigned int v;
                memcpy(&v, p, sizeof(v));
                n = v;
            }
            else goto fail;
            if (n > INT64_MAX) goto fail;
            item = json_integer((json_int_t)n);
        }
        if (!item || json_object_set_new(out, f->name, item) < 0) goto fail;
    }
    return out;
fail:
    json_decref(out);
    return NULL;
}
static int
decode_fields(const json_t *source, void *target,
              const struct checkpoint_field *fields, size_t count)
{
    if (!json_is_object(source)) return -1;
    for (size_t i = 0; i < count; ++i) {
        const struct checkpoint_field *f = &fields[i];
        unsigned char *p = (unsigned char *)target + f->offset;
        const json_t *value = json_object_get(source, f->name);
        if (f->kind == CK_TEXT) {
            const char *text = json_string_value(value);
            if (!text || json_string_length(value) >= f->width ||
                strlen(text) != json_string_length(value)) return -1;
            memcpy(p, text, json_string_length(value) + 1u);
        } else if (f->kind == CK_BOOLEAN) {
            bool b;
            if (!json_is_boolean(value)) return -1;
            b = json_is_true(value);
            memcpy(p, &b, sizeof(b));
        } else {
            uint64_t n;
            if (!json_is_integer(value) || json_integer_value(value) < 0) return -1;
            n = (uint64_t)json_integer_value(value);
            if (f->width == sizeof(uint64_t)) memcpy(p, &n, f->width);
            else if (f->width == sizeof(uint32_t) && n <= UINT32_MAX) {
                uint32_t v = (uint32_t)n; memcpy(p, &v, sizeof(v));
            } else if (f->width == sizeof(unsigned int) && n <= UINT_MAX) {
                unsigned int v = (unsigned int)n; memcpy(p, &v, sizeof(v));
            } else return -1;
        }
    }
    return 0;
}

static int
set_item(json_t *object, const char *key, json_t *value)
{
    if (!value) return -1;
    return json_object_set_new(object, key, value);
}

static json_t *
encode_array(const void *items, size_t count, size_t width,
             const struct checkpoint_field *fields, size_t nfields)
{
    json_t *array = json_array();
    if (!array) return NULL;
    for (size_t i = 0; i < count; ++i) {
        const unsigned char *entry = (const unsigned char *)items + i * width;
        json_t *object = encode_fields(entry, fields, nfields);
        if (!object || json_array_append_new(array, object) < 0) goto fail;
    }
    return array;
fail:
    json_decref(array);
    return NULL;
}

static int
decode_array(const json_t *array, void **out, size_t *count, size_t width,
             const struct checkpoint_field *fields, size_t nfields)
{
    if (!json_is_array(array) || json_array_size(array) > SIZE_MAX / width) return -1;
    size_t n = json_array_size(array);
    void *result = calloc(n ? n : 1u, width);
    if (!result) return -1;
    for (size_t i = 0; i < n; ++i) {
        if (decode_fields(json_array_get(array, i), (unsigned char *)result + i * width,
                          fields, nfields) < 0) { free(result); return -1; }
    }
    *out = result;
    *count = n;
    return 0;
}

static json_t *
encode_state(const struct snag_session *s)
{
    json_t *out = encode_fields(s, session_fields, COUNT(session_fields));
    if (!out) return NULL;
#define PUT(k, v) do { if (set_item(out, k, v) < 0) goto fail; } while (0)
#define OBS(f) PUT(#f, encode_fields(&s->f, observation_fields, COUNT(observation_fields)))
    OBS(active_accounting); OBS(usage_anchor); OBS(context_meter); OBS(capacity_rejection);
#undef OBS
    PUT("usage_totals", encode_fields(&s->usage_totals, usage_fields, COUNT(usage_fields)));
    json_t *controls = json_array();
    if (!controls) goto fail;
    for (size_t i = 0; i < COUNT(s->control_seq); ++i)
        if (s->control_seq[i] > INT64_MAX ||
            json_array_append_new(controls, json_integer((json_int_t)s->control_seq[i])) < 0) {
            json_decref(controls); goto fail;
        }
    PUT("control_seq", controls);
    json_t *processes = encode_array(s->processes, s->process_count,
        sizeof(*s->processes), process_fields, COUNT(process_fields));
    if (!processes) goto fail;
    for (size_t i = 0; i < s->process_count; ++i) {
        json_t *item = json_array_get(processes, i);
        json_t *output = json_pack("[I,I]", (json_int_t)s->processes[i].output_bytes[0],
            (json_int_t)s->processes[i].output_bytes[1]);
        json_t *collected = json_pack("[I,I]", (json_int_t)s->processes[i].collected_bytes[0],
            (json_int_t)s->processes[i].collected_bytes[1]);
        if (set_item(item, "output_bytes", output) < 0 ||
            set_item(item, "collected_bytes", collected) < 0) { json_decref(processes); goto fail; }
    }
    PUT("processes", processes);
    PUT("pending_calls", encode_array(s->pending_calls, s->pending_call_count,
        sizeof(*s->pending_calls), call_fields, COUNT(call_fields)));
    json_t *steering = encode_array(s->pending_steering, s->pending_steering_count,
        sizeof(*s->pending_steering), steering_fields, COUNT(steering_fields));
    if (!steering) goto fail;
    for (size_t i = 0; i < s->pending_steering_count; ++i) {
        if (set_item(json_array_get(steering, i), "content",
                     s->pending_steering[i].content ?
                     json_incref(s->pending_steering[i].content) : json_null()) < 0) {
            json_decref(steering); goto fail;
        }
    }
    PUT("pending_steering", steering);
    json_t *queue = encode_array(s->pending_queue, s->pending_queue_count,
        sizeof(*s->pending_queue), queue_fields, COUNT(queue_fields));
    if (!queue) goto fail;
    for (size_t i = 0; i < s->pending_queue_count; ++i) {
        if (set_item(json_array_get(queue, i), "content",
                     s->pending_queue[i].content ?
                     json_incref(s->pending_queue[i].content) : json_null()) < 0) {
            json_decref(queue); goto fail;
        }
    }
    PUT("pending_queue", queue);
#define JSON_FIELD(f) PUT(#f, s->f ? json_incref(s->f) : json_null())
    JSON_FIELD(strings); JSON_FIELD(compact_output); JSON_FIELD(pending_input);
    JSON_FIELD(active_instructions); JSON_FIELD(response_public);
#undef JSON_FIELD
#define TEXT_FIELD(f) PUT("has_" #f, json_boolean(s->f != NULL))
    TEXT_FIELD(cwd); TEXT_FIELD(first_user); TEXT_FIELD(last_user);
    TEXT_FIELD(active_prompt); TEXT_FIELD(goal_prompt); TEXT_FIELD(goal_blocker);
    TEXT_FIELD(timer_text); TEXT_FIELD(banner_text); TEXT_FIELD(steering_override);
#undef TEXT_FIELD
#undef PUT
    return out;
fail:
    json_decref(out);
    return NULL;
}

static int
decode_pairs(const json_t *value, uint64_t result[2])
{
    if (!json_is_array(value) || json_array_size(value) != 2u) return -1;
    for (size_t i = 0; i < 2u; ++i) {
        const json_t *n = json_array_get(value, i);
        if (!json_is_integer(n) || json_integer_value(n) < 0) return -1;
        result[i] = (uint64_t)json_integer_value(n);
    }
    return 0;
}

static int
decode_state(const json_t *data, struct snag_session *s)
{
    if (!json_is_object(data) ||
        decode_fields(data, s, session_fields, COUNT(session_fields)) < 0) return -1;
#define OBS(f) if (decode_fields(json_object_get(data, #f), &s->f, observation_fields, \
                           COUNT(observation_fields)) < 0) return -1
    OBS(active_accounting); OBS(usage_anchor); OBS(context_meter); OBS(capacity_rejection);
#undef OBS
    if (decode_fields(json_object_get(data, "usage_totals"), &s->usage_totals,
                      usage_fields, COUNT(usage_fields)) < 0) return -1;
    const json_t *controls = json_object_get(data, "control_seq");
    if (!json_is_array(controls) || json_array_size(controls) != COUNT(s->control_seq)) return -1;
    for (size_t i = 0; i < COUNT(s->control_seq); ++i) {
        const json_t *value = json_array_get(controls, i);
        if (!json_is_integer(value) || json_integer_value(value) < 0) return -1;
        s->control_seq[i] = (uint64_t)json_integer_value(value);
    }
    if (decode_array(json_object_get(data, "processes"), (void **)&s->processes,
                     &s->process_count, sizeof(*s->processes),
                     process_fields, COUNT(process_fields)) < 0)
        return -1;
    s->process_capacity = s->process_count;
    const json_t *processes = json_object_get(data, "processes");
    for (size_t i = 0; i < s->process_count; ++i) {
        const json_t *p = json_array_get(processes, i);
        if (decode_pairs(json_object_get(p, "output_bytes"), s->processes[i].output_bytes) < 0 ||
            decode_pairs(json_object_get(p, "collected_bytes"),
                         s->processes[i].collected_bytes) < 0) return -1;
    }
    if (decode_array(json_object_get(data, "pending_calls"), (void **)&s->pending_calls,
                     &s->pending_call_count, sizeof(*s->pending_calls),
                     call_fields, COUNT(call_fields)) < 0)
        return -1;
    s->pending_call_capacity = s->pending_call_count;
    if (decode_array(json_object_get(data, "pending_steering"), (void **)&s->pending_steering,
                     &s->pending_steering_count, sizeof(*s->pending_steering),
                     steering_fields, COUNT(steering_fields)) < 0) return -1;
    s->pending_steering_capacity = s->pending_steering_count;
    if (decode_array(json_object_get(data, "pending_queue"), (void **)&s->pending_queue,
                     &s->pending_queue_count, sizeof(*s->pending_queue),
                     queue_fields, COUNT(queue_fields)) < 0) return -1;
    s->pending_queue_capacity = s->pending_queue_count;
#define JSON_FIELD(f) do { \
    const json_t *value = json_object_get(data, #f); \
    if (!value) return -1; \
    s->f = json_is_null(value) ? NULL : json_incref((json_t *)value); \
} while (0)
    JSON_FIELD(strings); JSON_FIELD(compact_output); JSON_FIELD(pending_input);
    JSON_FIELD(active_instructions); JSON_FIELD(response_public);
#undef JSON_FIELD
    if (s->strings && !json_is_object(s->strings)) return -1;
    /* Replayed suffix events replace entries in strings. The checkpoint's
     * serialized state is also needed later to restore provider context;
     * sharing its mutable object would silently change the saved snapshot. */
    if (s->strings) {
        json_t *copy = json_copy(s->strings);
        if (!copy) return -1;
        json_decref(s->strings);
        s->strings = copy;
    }
#define TEXT_FIELD(f) do { \
    const json_t *value = json_object_get(data, "has_" #f); \
    if (!json_is_boolean(value)) return -1; \
    if (json_is_true(value) && !(s->f = snag_json_string(s->strings, #f))) return -1; \
} while (0)
    TEXT_FIELD(cwd); TEXT_FIELD(first_user); TEXT_FIELD(last_user);
    TEXT_FIELD(active_prompt); TEXT_FIELD(goal_prompt); TEXT_FIELD(goal_blocker);
    TEXT_FIELD(timer_text); TEXT_FIELD(banner_text); TEXT_FIELD(steering_override);
#undef TEXT_FIELD
    for (size_t i = 0; i < s->pending_steering_count; ++i) {
        struct snag_pending_steering *item = &s->pending_steering[i];
        const json_t *entry = json_array_get(json_object_get(data, "pending_steering"), i);
        const json_t *content = json_object_get(entry, "content");
        if (!content || !(item->text = snag_json_string(s->strings, item->steering_id))) return -1;
        item->content = json_is_null(content) ? NULL : json_incref((json_t *)content);
    }
    for (size_t i = 0; i < s->pending_queue_count; ++i) {
        struct snag_queued_turn *item = &s->pending_queue[i];
        const json_t *entry = json_array_get(json_object_get(data, "pending_queue"), i);
        const json_t *content = json_object_get(entry, "content");
        if (!content || !(item->text = snag_json_string(s->strings, item->queue_id))) return -1;
        item->content = json_is_null(content) ? NULL : json_incref((json_t *)content);
    }
    if (!s->id[0] || !snag_hex_is_lower(s->id, SNAG_ID_HEX_LEN) ||
        !snag_hex_is_lower(s->prev_sha256, SNAG_SHA256_HEX_LEN) ||
        s->log_end < 0 || s->next_seq < 2u ||
        (s->format_version != 2u && s->format_version != 3u && s->format_version != 4u)) return -1;
    return 0;
}

/* A checkpoint event contains the pre-append state; replay of that one event
 * advances its chain head without a self-referential event digest. */
json_t *
snag_checkpoint_state_encode(const struct snag_session *session)
{
    return encode_state(session);
}

int
snag_checkpoint_state_decode(const json_t *data, struct snag_session *state)
{
    snag_session_init(state);
    return decode_state(data, state);
}
