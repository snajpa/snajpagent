/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_checkpoint.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* Wire order is explicit and independent of structure layout or padding. */
static const struct text_field {
    size_t offset, width, bytes;
} observation_texts[] = {
#define TEXT(f, n) {offsetof(struct snag_input_observation, f), \
    sizeof(((struct snag_input_observation *)0)->f), n}
    TEXT(provider, 0u), TEXT(model, 0u), TEXT(effort, 0u), TEXT(compact_id, 16u),
    TEXT(provider_source_sha256, 32u), TEXT(model_input_sha256, 32u),
    TEXT(request_input_sha256, 32u), TEXT(request_sha256, 32u)
#undef TEXT
};

static const size_t observation_numbers[] = {
    offsetof(struct snag_input_observation, model_input_bytes),
    offsetof(struct snag_input_observation, request_input_bytes),
    offsetof(struct snag_input_observation, request_input_count),
    offsetof(struct snag_input_observation, input_tokens),
    offsetof(struct snag_input_observation, requested_output_tokens)
};

static const size_t total_numbers[] = {
    offsetof(struct snag_usage_totals, responses),
    offsetof(struct snag_usage_totals, input_tokens),
    offsetof(struct snag_usage_totals, cached_input_tokens),
    offsetof(struct snag_usage_totals, uncached_input_tokens),
    offsetof(struct snag_usage_totals, output_tokens),
    offsetof(struct snag_usage_totals, reasoning_tokens),
    offsetof(struct snag_usage_totals, total_tokens)
};

static const size_t observations[] = {
    offsetof(struct snag_binary_checkpoint_accounting, active_accounting),
    offsetof(struct snag_binary_checkpoint_accounting, usage_anchor),
    offsetof(struct snag_binary_checkpoint_accounting, context_meter),
    offsetof(struct snag_binary_checkpoint_accounting, capacity_rejection)
};

static int
put_number(struct snag_buf *out, uint64_t value, size_t width)
{
    unsigned char bytes[8];
    for (size_t i = 0u; i < width; ++i) bytes[i] = (unsigned char)(value >> (i * 8u));
    return snag_buf_append(out, bytes, width);
}

static int
put_text(struct snag_buf *out, const void *base, const struct text_field *field)
{
    const unsigned char *text = (const unsigned char *)base + field->offset;
    const unsigned char *end = memchr(text, 0, field->width);
    if (!end) return snag_errno(EINVAL);
    size_t size = (size_t)(end - text);
    if (!field->bytes) {
        if (size > UINT16_MAX || !snag_utf8_valid(text, size, true))
            return snag_errno(EINVAL);
        if (put_number(out, size, 2u) < 0 || snag_buf_append(out, text, size) < 0) return -1;
    } else if (size) {
        if (size != field->bytes * 2u || !snag_hex_is_lower((const char *)text, size))
            return snag_errno(EINVAL);
        unsigned char bytes[32];
        for (size_t j = 0u; j < field->bytes; ++j) {
            unsigned char a = text[j * 2u], b = text[j * 2u + 1u];
            unsigned high = a <= '9' ? (unsigned)(a - '0') : (unsigned)(a - 'a') + 10u;
            unsigned low = b <= '9' ? (unsigned)(b - '0') : (unsigned)(b - 'a') + 10u;
            bytes[j] = (unsigned char)((high << 4u) | low);
        }
        if (snag_buf_append(out, bytes, field->bytes) < 0) return -1;
    }
    return 0;
}

static int
encode_observation(struct snag_buf *out, const struct snag_input_observation *value)
{
    const unsigned char *base = (const unsigned char *)value;
    unsigned char flags = value->valid ? 1u : 0u;
    for (size_t i = 3u; i < sizeof(observation_texts) / sizeof(*observation_texts); ++i)
        if (base[observation_texts[i].offset]) flags |= (unsigned char)(1u << (i - 2u));
    if (snag_buf_putc(out, flags) < 0) return -1;
    for (size_t i = 0u; i < sizeof(observation_texts) / sizeof(*observation_texts); ++i)
        if (put_text(out, base, &observation_texts[i]) < 0) return -1;
    for (size_t i = 0u; i < sizeof(observation_numbers) / sizeof(*observation_numbers); ++i) {
        uint64_t number;
        memcpy(&number, base + observation_numbers[i], sizeof(number));
        if (put_number(out, number, 8u) < 0) return -1;
    }
    return 0;
}

int
snag_binary_checkpoint_accounting_encode(struct snag_buf *out,
    const struct snag_binary_checkpoint_accounting *value)
{
    if (!out || !value) return snag_errno(EINVAL);
    struct snag_buf encoded;
    /* The fixed number of bounded C metadata fields bounds allocation here.
     * The enclosing checkpoint has no single-event size limit. */
    snag_buf_init(&encoded, SIZE_MAX);
    int rc = put_number(&encoded, 1u, 2u);
    const unsigned char *base = (const unsigned char *)value;
    for (size_t i = 0u; !rc && i < sizeof(observations) / sizeof(*observations); ++i) {
        const struct snag_input_observation *observation =
            (const struct snag_input_observation *)(base + observations[i]);
        rc = encode_observation(&encoded, observation);
    }
    base = (const unsigned char *)&value->usage_totals;
    for (size_t i = 0u; !rc && i < sizeof(total_numbers) / sizeof(*total_numbers); ++i) {
        uint64_t number;
        memcpy(&number, base + total_numbers[i], sizeof(number));
        rc = put_number(&encoded, number, 8u);
    }
    if (!rc) rc = snag_buf_putc(&encoded, value->usage_totals.cached_seen ? 1u : 0u);
    if (!rc) rc = snag_buf_append(out, encoded.data, encoded.len);
    snag_buf_free(&encoded);
    return rc;
}

struct fields {
    const unsigned char *data;
    size_t size, offset;
};

static const unsigned char *
take(struct fields *fields, size_t size)
{
    if (size > fields->size - fields->offset) return NULL;
    const unsigned char *bytes = fields->data + fields->offset;
    fields->offset += size;
    return bytes;
}

static int
get_number(struct fields *fields, size_t width, uint64_t *out)
{
    const unsigned char *bytes = take(fields, width);
    if (!bytes) return snag_errno(EINVAL);
    uint64_t value = 0u;
    for (size_t i = 0u; i < width; ++i) value |= (uint64_t)bytes[i] << (i * 8u);
    *out = value;
    return 0;
}

static int
get_text(struct fields *fields, void *base, const struct text_field *field, bool present)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char *text = (unsigned char *)base + field->offset;
    if (!field->bytes) {
        uint64_t size;
        if (get_number(fields, 2u, &size) < 0 || size >= field->width)
            return snag_errno(EINVAL);
        const unsigned char *bytes = take(fields, (size_t)size);
        if (!bytes || !snag_utf8_valid(bytes, (size_t)size, true)) return snag_errno(EINVAL);
        memcpy(text, bytes, (size_t)size);
    } else if (present) {
        const unsigned char *bytes = take(fields, field->bytes);
        if (!bytes) return snag_errno(EINVAL);
        for (size_t j = 0u; j < field->bytes; ++j) {
            text[j * 2u] = (unsigned char)hex[bytes[j] >> 4u];
            text[j * 2u + 1u] = (unsigned char)hex[bytes[j] & 15u];
        }
    }
    return 0;
}

static int
decode_observation(struct fields *fields, struct snag_input_observation *value)
{
    uint64_t flags;
    if (get_number(fields, 1u, &flags) < 0 || flags & ~UINT64_C(63)) return snag_errno(EINVAL);
    value->valid = (flags & 1u) != 0u;
    unsigned char *base = (unsigned char *)value;
    for (size_t i = 0u; i < sizeof(observation_texts) / sizeof(*observation_texts); ++i) {
        bool present = i < 3u || (flags & (UINT64_C(1) << (i - 2u))) != 0u;
        if (get_text(fields, base, &observation_texts[i], present) < 0) return -1;
    }
    for (size_t i = 0u; i < sizeof(observation_numbers) / sizeof(*observation_numbers); ++i) {
        uint64_t number;
        if (get_number(fields, 8u, &number) < 0) return -1;
        memcpy(base + observation_numbers[i], &number, sizeof(number));
    }
    return 0;
}

int
snag_binary_checkpoint_accounting_decode(const void *data, size_t size,
    struct snag_binary_checkpoint_accounting *out)
{
    if (!data || !out) return snag_errno(EINVAL);
    struct fields fields = {.data = data, .size = size};
    uint64_t version;
    if (get_number(&fields, 2u, &version) < 0 || version != 1u) return snag_errno(EINVAL);
    struct snag_binary_checkpoint_accounting value = {0};
    unsigned char *base = (unsigned char *)&value;
    for (size_t i = 0u; i < sizeof(observations) / sizeof(*observations); ++i) {
        struct snag_input_observation *observation =
            (struct snag_input_observation *)(base + observations[i]);
        if (decode_observation(&fields, observation) < 0) return -1;
    }
    base = (unsigned char *)&value.usage_totals;
    for (size_t i = 0u; i < sizeof(total_numbers) / sizeof(*total_numbers); ++i) {
        uint64_t number;
        if (get_number(&fields, 8u, &number) < 0) return -1;
        memcpy(base + total_numbers[i], &number, sizeof(number));
    }
    uint64_t seen;
    if (get_number(&fields, 1u, &seen) < 0 || seen > 1u || fields.offset != fields.size)
        return snag_errno(EINVAL);
    value.usage_totals.cached_seen = seen != 0u;
    *out = value;
    return 0;
}

/* Version-1 control field order. Common frame fields and payload owners are
 * excluded; copying a session structure here would serialize resources. */
static const struct text_field control_texts[] = {
#define TEXT(f, n) {offsetof(struct snag_session, f), sizeof(((struct snag_session *)0)->f), n}
    TEXT(active_turn_id, 16u), TEXT(active_response_id, 16u),
    TEXT(final_item_id, 16u), TEXT(final_response_id, 16u),
    TEXT(compact_id, 16u), TEXT(active_compact_id, 16u),
    TEXT(active_compact_source_sha256, 32u), TEXT(active_compact_scope, 32u),
    TEXT(default_provider, 0u), TEXT(goal_id, 16u), TEXT(goal_parent_id, 16u),
    TEXT(timer_id, 16u), TEXT(default_model, 0u), TEXT(active_turn_model, 0u),
    TEXT(active_turn_provider, 0u), TEXT(active_turn_effort, 0u),
    TEXT(capacity_ceiling_provider, 0u), TEXT(capacity_ceiling_model, 0u),
    TEXT(capacity_ceiling_source_sha256, 32u), TEXT(default_effort, 0u),
    TEXT(command_shell, 0u), TEXT(trash_name, 0u), TEXT(compact_scope, 32u),
    TEXT(context_rebase_turn_id, 16u)
#undef TEXT
};

static const struct number_field {
    size_t offset, width, wire;
    uint64_t maximum;
} control_numbers[] = {
#define NUMBER(f, w, m) {offsetof(struct snag_session, f), \
    sizeof(((struct snag_session *)0)->f), w, m}
#define U64(f) NUMBER(f, 8u, UINT64_MAX)
#define U32(f) NUMBER(f, 4u, UINT32_MAX)
    U64(irc_received_seq), U64(irc_consumed_seq), U64(response_irc_seq),
    U32(max_parallel_commands), U32(default_yield_ms), U32(max_wait_ms),
    U32(default_timeout_ms), U32(max_timeout_ms), U32(tool_output_bytes), U32(output_cache_bytes),
    NUMBER(context_mode, 4u, SNAG_CONTEXT_MODE_TOKENS),
    U64(context_tokens), U64(response_public_bytes), NUMBER(format_version, 4u, 4u),
    U64(last_time_ms), U64(compact_seq), U64(context_rebase_seq), U64(active_compact_source_seq),
    U64(capacity_ceiling_input_tokens), U64(input_received_ms), U64(input_first_context_ms),
    U64(recovery_count), U64(goal_revision), U64(goal_turn_count), U64(timer_due_ms),
    U64(pending_steering_bytes), U64(pending_queue_bytes), U32(active_cycle),
    U64(turn_retry_attempts), U32(turn_retry_limit),
    NUMBER(response_outcome, 4u, SNAG_GRAPH_CONFLICT),
    NUMBER(pending_controls, 4u, 127u), NUMBER(started_controls, 4u, 127u),
    U64(compact_control_source_seq), NUMBER(policy_stopped, 4u, SNAG_POLICY_STOP_REFUSAL),
    NUMBER(response_terminal, 4u, SNAG_RESPONSE_TERMINAL_FAILED),
    NUMBER(goal_status, 4u, SNAG_GOAL_CANCELLED)
#undef U32
#undef U64
#undef NUMBER
};

static const size_t control_flags[] = {
#define FLAG(f) offsetof(struct snag_session, f)
    FLAG(parallel_tool_calls), FLAG(context_rebase_has_new_results), FLAG(legacy_journal),
    FLAG(compact_control_image_boundary), FLAG(queue_armed), FLAG(active_turn),
    FLAG(last_turn_failed), FLAG(retry_read_only), FLAG(active_read_only), FLAG(active_queued),
    FLAG(active_goal), FLAG(cancel_requested), FLAG(response_handoff),
    FLAG(delete_requested), FLAG(response_open), FLAG(response_complete), FLAG(irc_reply_reminded),
    FLAG(output_correction_used), FLAG(steering_deferred), FLAG(goal_locked),
    FLAG(capacity_ceiling_valid)
#undef FLAG
};

_Static_assert(COUNT(control_flags) == 21u, "version-1 control flags");
_Static_assert(sizeof(((struct snag_session *)0)->control_seq) == 7u * sizeof(uint64_t),
    "version-2 control sequences");

static int
load_number(const void *base, const struct number_field *field, uint64_t *out)
{
    const unsigned char *bytes = (const unsigned char *)base + field->offset;
    uint64_t number;
    switch (field->width) {
#define LOAD(w) case sizeof(uint##w##_t): { \
    uint##w##_t value; memcpy(&value, bytes, sizeof(value)); number = value; break; }
    LOAD(8)
    LOAD(16)
    LOAD(32)
    LOAD(64)
#undef LOAD
    default: return snag_errno(EINVAL);
    }
    if (number > field->maximum) return snag_errno(EINVAL);
    *out = number;
    return 0;
}

static int
save_number(void *base, const struct number_field *field, uint64_t number)
{
    if (number > field->maximum) return snag_errno(EINVAL);
    unsigned char *bytes = (unsigned char *)base + field->offset;
    if (field->width < sizeof(number) && number >> (field->width * 8u))
        return snag_errno(EOVERFLOW);
    switch (field->width) {
#define SAVE(w) case sizeof(uint##w##_t): { \
    uint##w##_t value = (uint##w##_t)number; memcpy(bytes, &value, sizeof(value)); return 0; }
    SAVE(8)
    SAVE(16)
    SAVE(32)
    SAVE(64)
#undef SAVE
    default: return snag_errno(EINVAL);
    }
}

int
snag_binary_checkpoint_controls_encode(struct snag_buf *out, const struct snag_session *value)
{
    if (!out || !value || value->format_version < 2u || value->format_version > 4u)
        return snag_errno(EINVAL);
    struct snag_buf encoded;
    snag_buf_init(&encoded, SIZE_MAX);
    const unsigned char *base = (const unsigned char *)value;
    uint32_t flags = 0u;
    for (size_t i = 0u; i < COUNT(control_flags); ++i) {
        bool flag;
        memcpy(&flag, base + control_flags[i], sizeof(flag));
        if (flag) flags |= UINT32_C(1) << i;
    }
    int rc = put_number(&encoded, 2u, 2u);
    if (!rc) rc = put_number(&encoded, flags, 4u);
    for (size_t i = 0u; !rc && i < COUNT(control_texts); ++i) {
        const struct text_field *field = &control_texts[i];
        if (field->bytes) rc = snag_buf_putc(&encoded, base[field->offset] ? 1u : 0u);
        if (!rc) rc = put_text(&encoded, value, field);
    }
    for (size_t i = 0u; !rc && i < COUNT(control_numbers); ++i) {
        uint64_t number;
        rc = load_number(value, &control_numbers[i], &number);
        if (!rc) rc = put_number(&encoded, number, control_numbers[i].wire);
    }
    for (size_t i = 0u; !rc && i < COUNT(value->control_seq); ++i)
        rc = put_number(&encoded, value->control_seq[i], 8u);
    if (!rc) rc = snag_buf_append(out, encoded.data, encoded.len);
    snag_buf_free(&encoded);
    return rc;
}

int
snag_binary_checkpoint_controls_decode(const void *data, size_t size, struct snag_session *out)
{
    if (!data || !out) return snag_errno(EINVAL);
    struct fields fields = {.data = data, .size = size};
    uint64_t version, flags;
    if (get_number(&fields, 2u, &version) < 0 || (version != 1u && version != 2u) ||
        get_number(&fields, 4u, &flags) < 0 || flags >> COUNT(control_flags))
        return snag_errno(EINVAL);
    struct snag_session value = {0};
    unsigned char *base = (unsigned char *)&value;
    for (size_t i = 0u; i < COUNT(control_flags); ++i) {
        bool flag = (flags & (UINT64_C(1) << i)) != 0u;
        memcpy(base + control_flags[i], &flag, sizeof(flag));
    }
    for (size_t i = 0u; i < COUNT(control_texts); ++i) {
        const struct text_field *field = &control_texts[i];
        uint64_t present = 1u;
        if (field->bytes && (get_number(&fields, 1u, &present) < 0 || present > 1u))
            return snag_errno(EINVAL);
        if (get_text(&fields, &value, field, present != 0u) < 0) return -1;
    }
    for (size_t i = 0u; i < COUNT(control_numbers); ++i) {
        uint64_t number;
        if (get_number(&fields, control_numbers[i].wire, &number) < 0 ||
            save_number(&value, &control_numbers[i], number) < 0) return -1;
    }
    size_t sequences = version == 1u ? 6u : COUNT(value.control_seq);
    if (version == 1u && ((value.pending_controls | value.started_controls) & ~63u))
        return snag_errno(EINVAL);
    for (size_t i = 0u; i < sequences; ++i)
        if (get_number(&fields, 8u, &value.control_seq[i]) < 0) return -1;
    if (value.format_version < 2u || fields.offset != fields.size) return snag_errno(EINVAL);
    unsigned char *target = (unsigned char *)out;
    for (size_t i = 0u; i < COUNT(control_texts); ++i) {
        const struct text_field *field = &control_texts[i];
        memcpy(target + field->offset, base + field->offset, field->width);
    }
    for (size_t i = 0u; i < COUNT(control_numbers); ++i) {
        const struct number_field *field = &control_numbers[i];
        memcpy(target + field->offset, base + field->offset, field->width);
    }
    for (size_t i = 0u; i < COUNT(control_flags); ++i)
        memcpy(target + control_flags[i], base + control_flags[i], sizeof(bool));
    memcpy(out->control_seq, value.control_seq, sizeof(value.control_seq));
    return 0;
}
