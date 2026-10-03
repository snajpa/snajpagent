/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_checkpoint.h"
#include "store_internal.h"
#include "checked_json.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

void test_store_binary_controls(void);
void test_store_binary_controls_state(const struct snag_session *);

/* Independently enumerated fields also identify every byte allowed to change
 * in the destination; all remaining session bytes are canaries. */
struct field {
    const char *name;
    size_t offset, size, wire;
    uint64_t maximum;
};
#define FIELD(f, w, m) {#f, offsetof(struct snag_session, f), \
    sizeof(((struct snag_session *)0)->f), w, m}
static const struct field texts[] = {
#define TEXT(f, n) FIELD(f, n, 0u)
    TEXT(active_turn_id, 16u), TEXT(active_response_id, 16u), TEXT(final_item_id, 16u),
    TEXT(final_response_id, 16u), TEXT(compact_id, 16u), TEXT(active_compact_id, 16u),
    TEXT(active_compact_source_sha256, 32u), TEXT(active_compact_scope, 32u),
    TEXT(default_provider, 0u), TEXT(goal_id, 16u), TEXT(goal_parent_id, 16u), TEXT(timer_id, 16u),
    TEXT(default_model, 0u), TEXT(active_turn_model, 0u), TEXT(active_turn_provider, 0u),
    TEXT(active_turn_effort, 0u), TEXT(capacity_ceiling_provider, 0u),
    TEXT(capacity_ceiling_model, 0u),
    TEXT(capacity_ceiling_source_sha256, 32u), TEXT(default_effort, 0u), TEXT(command_shell, 0u),
    TEXT(trash_name, 0u), TEXT(compact_scope, 32u), TEXT(context_rebase_turn_id, 16u)
#undef TEXT
};
static const struct field numbers[] = {
#define W(f) FIELD(f, 8u, UINT64_MAX)
#define N(f) FIELD(f, 4u, UINT32_MAX)
    W(irc_received_seq), W(irc_consumed_seq), W(response_irc_seq), N(max_parallel_commands),
    N(default_yield_ms), N(max_wait_ms), N(default_timeout_ms), N(max_timeout_ms),
    N(tool_output_bytes), N(output_cache_bytes), FIELD(context_mode, 4u, 2u),
    W(context_tokens), W(response_public_bytes), FIELD(format_version, 4u, 4u),
    W(last_time_ms), W(compact_seq), W(context_rebase_seq), W(active_compact_source_seq),
    W(capacity_ceiling_input_tokens), W(input_received_ms), W(input_first_context_ms),
    W(recovery_count), W(goal_revision), W(goal_turn_count), W(timer_due_ms),
    W(pending_steering_bytes), W(pending_queue_bytes), N(active_cycle), W(turn_retry_attempts),
    N(turn_retry_limit), FIELD(response_outcome, 4u, 4u), FIELD(pending_controls, 4u, 63u),
    FIELD(started_controls, 4u, 63u), W(compact_control_source_seq), FIELD(policy_stopped, 4u, 2u),
    FIELD(response_terminal, 4u, 3u), FIELD(goal_status, 4u, 5u)
#undef N
#undef W
};
static const struct field flags[] = {
#define FLAG(f) FIELD(f, 1u, 1u)
    FLAG(parallel_tool_calls), FLAG(context_rebase_has_new_results), FLAG(legacy_journal),
    FLAG(compact_control_image_boundary), FLAG(queue_armed), FLAG(active_turn),
    FLAG(last_turn_failed), FLAG(retry_read_only), FLAG(active_read_only), FLAG(active_queued),
    FLAG(active_goal), FLAG(cancel_requested), FLAG(response_handoff),
    FLAG(delete_requested), FLAG(response_open), FLAG(response_complete), FLAG(irc_reply_reminded),
    FLAG(output_correction_used), FLAG(steering_deferred), FLAG(goal_locked),
    FLAG(capacity_ceiling_valid)
#undef FLAG
};
#undef FIELD

static const char *outside[] = {
    "id", "prev_sha256", "log_end", "next_seq", "checkpoint_offset", "checkpoint_seq", "turn_count",
    "active_accounting", "usage_anchor", "context_meter", "capacity_rejection", "usage_totals",
    "voice_history", "processes", "pending_calls", "pending_steering", "pending_queue", "strings",
    "compact_output", "pending_input", "active_instructions", "response_public", "download_queue",
    "has_cwd", "has_first_user", "has_last_user", "has_active_prompt", "has_goal_prompt",
    "has_goal_blocker", "has_timer_text", "has_banner_text", "has_steering_override"
};

static bool
outside_field(const char *name)
{
    for (size_t i = 0u; i < COUNT(outside); ++i)
        if (!strcmp(name, outside[i])) return true;
    return false;
}

static void
same_controls(const struct snag_session *a, const struct snag_session *b)
{
    json_t *left = snag_checkpoint_state_encode(a), *right = snag_checkpoint_state_encode(b);
    assert(left && right);
    for (size_t i = 0u; i < COUNT(outside); ++i) {
        assert(!json_object_del(left, outside[i]));
        assert(!json_object_del(right, outside[i]));
    }
    assert(json_object_size(left) == 83u && json_equal(left, right));
    json_decref(left);
    json_decref(right);
}

void
test_store_binary_controls_state(const struct snag_session *state)
{
    struct snag_buf wire;
    snag_buf_init(&wire, SIZE_MAX);
    assert(!snag_binary_checkpoint_controls_encode(&wire, state));
    struct snag_session decoded;
    snag_session_init(&decoded);
    assert(!snag_binary_checkpoint_controls_decode(wire.data, wire.len, &decoded));
    same_controls(state, &decoded);
    snag_buf_free(&wire);
    snag_session_close(&decoded);
}

static void
copy_fields(unsigned char *to, const struct snag_session *from,
    const struct field *fields, size_t count)
{
    for (size_t i = 0u; i < count; ++i)
        memcpy(to + fields[i].offset, (const unsigned char *)from + fields[i].offset,
            fields[i].size);
}

static void
roundtrip(const struct snag_session *value, struct snag_buf *wire)
{
    snag_buf_reset(wire);
    assert(!snag_binary_checkpoint_controls_encode(wire, value));
    struct snag_session decoded;
    unsigned char expected[sizeof(decoded)];
    memset(&decoded, 0xa5, sizeof(decoded));
    memset(expected, 0xa5, sizeof(expected));
    copy_fields(expected, value, texts, COUNT(texts));
    copy_fields(expected, value, numbers, COUNT(numbers));
    copy_fields(expected, value, flags, COUNT(flags));
    memcpy(expected + offsetof(struct snag_session, control_seq),
        value->control_seq, sizeof(value->control_seq));
    assert(!snag_binary_checkpoint_controls_decode(wire->data, wire->len, &decoded));
    assert(!memcmp(&decoded, expected, sizeof(decoded)));
    /* Other members deliberately contain canaries, not live resource values. */
    struct snag_buf second;
    snag_buf_init(&second, SIZE_MAX);
    assert(!snag_binary_checkpoint_controls_encode(&second, &decoded));
    assert(second.len == wire->len && !memcmp(second.data, wire->data, wire->len));
    snag_buf_free(&second);
}

static void
fixture(struct snag_session *out)
{
    struct snag_session empty;
    snag_session_init(&empty);
    memset(empty.id, 'a', SNAG_ID_HEX_LEN);
    empty.next_seq = 2u;
    json_t *doc = snag_checkpoint_state_encode(&empty);
    assert(doc);
    const char *name;
    json_t *value;
    unsigned number = 0u;
    /* Seed every legacy scalar independently of the native field table, so an
     * omitted native scalar cannot disappear behind its original zero value. */
    json_object_foreach(doc, name, value) {
        if (outside_field(name)) continue;
        if (json_is_integer(value)) {
            uint64_t n = UINT64_C(0x01020304) + number++;
            if (!strcmp(name, "format_version")) n = 4u;
            else if (!strcmp(name, "context_mode") || !strcmp(name, "policy_stopped")) n = 2u;
            else if (!strcmp(name, "response_outcome")) n = 4u;
            else if (!strcmp(name, "response_terminal")) n = 3u;
            else if (!strcmp(name, "goal_status")) n = 5u;
            else if (!strcmp(name, "pending_controls") || !strcmp(name, "started_controls"))
                n = 63u;
            assert(!json_object_set_new(doc, name, json_integer((json_int_t)n)));
        } else if (json_is_boolean(value)) {
            assert(!json_object_set_new(doc, name, json_true()));
        } else if (json_is_string(value)) {
            char text[65] = "m\xc3\xa9";
            size_t size = strlen(name);
            if (size >= 3u && !strcmp(name + size - 3u, "_id")) {
                memset(text, 'a', 32u); text[32] = '\0';
            } else if (strstr(name, "sha256") || strstr(name, "scope")) {
                memset(text, 'b', 64u); text[64] = '\0';
            }
            assert(!json_object_set_new(doc, name, json_string(text)));
        }
    }
    assert(number == 37u);
    json_t *sequences = json_object_get(doc, "control_seq");
    for (size_t i = 0u; i < 6u; ++i)
        assert(!json_array_set_new(sequences, i,
            json_integer((json_int_t)(UINT64_C(0x1122334455667788) + i))));
    assert(!snag_checkpoint_state_decode(doc, out));
    json_decref(doc);
    snag_session_close(&empty);
}

static void
reject_decode(const void *data, size_t size)
{
    struct snag_session out;
    unsigned char before[sizeof(out)];
    memset(before, 0xa5, sizeof(before));
    memcpy(&out, before, sizeof(out));
    errno = 0;
    assert(snag_binary_checkpoint_controls_decode(data, size, &out) < 0 && errno == EINVAL);
    assert(!memcmp(&out, before, sizeof(out)));
}

static void
reject_encode(const struct snag_session *value)
{
    struct snag_buf wire;
    snag_buf_init(&wire, SIZE_MAX);
    assert(!snag_buf_append(&wire, "keep", 4u));
    errno = 0;
    assert(snag_binary_checkpoint_controls_encode(&wire, value) < 0 && errno == EINVAL);
    assert(wire.len == 4u && !memcmp(wire.data, "keep", 4u));
    snag_buf_free(&wire);
}

static void
set_number(struct snag_session *value, const struct field *field, uint64_t number)
{
    unsigned char *dest = (unsigned char *)value + field->offset;
    switch (field->size) {
#define SET(w) case sizeof(uint##w##_t): { \
    uint##w##_t n = (uint##w##_t)number; memcpy(dest, &n, sizeof(n)); break; }
    SET(8)
    SET(16)
    SET(32)
    SET(64)
#undef SET
    default: assert(false);
    }
}

void
test_store_binary_controls(void)
{
    struct snag_session value;
    fixture(&value);
    struct snag_buf wire;
    snag_buf_init(&wire, SIZE_MAX);
    roundtrip(&value, &wire);
    test_store_binary_controls_state(&value);
    char digest[65];
    snag_sha256_hex(wire.data, wire.len, digest);
    /* Independent Python struct/hashlib field-order golden. */
    assert(wire.len == 634u);
    assert(!strcmp(digest, "4453683d780181253a8fcf02d29dd3e56cf8da2e6c7eb075c2ff6388bb55b8d0"));
    for (size_t i = 0u; i < wire.len; ++i) reject_decode(wire.data, i);
    reject_decode(NULL, wire.len);
    reject_encode(NULL);
    assert(snag_binary_checkpoint_controls_decode(wire.data, wire.len, NULL) < 0);
    assert(snag_binary_checkpoint_controls_encode(NULL, &value) < 0);
    unsigned char bad[635];
    memcpy(bad, wire.data, wire.len);
    bad[634] = 0u;
    reject_decode(bad, sizeof(bad));
    for (unsigned version = 0u; version < 3u; ++version) {
        memcpy(bad, wire.data, wire.len);
        bad[version == 2u ? 1u : 0u] = version == 1u ? 2u : 0u;
        if (version == 2u) bad[1] = 1u;
        reject_decode(bad, wire.len);
    }
    for (size_t i = 0u; i < 32u; ++i) {
        memcpy(bad, wire.data, wire.len);
        size_t byte = 2u + i / 8u;
        unsigned char bit = (unsigned char)(1u << (i % 8u));
        if (i >= 21u) {
            bad[byte] |= bit;
            reject_decode(bad, wire.len);
        } else {
            bad[byte] &= (unsigned char)~bit;
            struct snag_session changed = value;
            bool flag = false;
            memcpy((unsigned char *)&changed + flags[i].offset, &flag, sizeof(flag));
            unsigned char expected[sizeof(changed)];
            memcpy(expected, &changed, sizeof(changed));
            assert(!snag_binary_checkpoint_controls_decode(bad, wire.len, &changed));
            assert(!memcmp(expected, &changed, sizeof(changed)));
            struct snag_buf encoded;
            snag_buf_init(&encoded, SIZE_MAX);
            assert(!snag_binary_checkpoint_controls_encode(&encoded, &changed));
            assert(encoded.len == wire.len && !memcmp(encoded.data, bad, wire.len));
            snag_buf_free(&encoded);
        }
    }
    size_t position = 6u;
    for (size_t i = 0u; i < COUNT(texts); ++i) {
        const struct field *f = &texts[i];
        memcpy(bad, wire.data, wire.len);
        if (f->wire) {
            bad[position] = 2u;
            reject_decode(bad, wire.len);
            position += 1u + f->wire;
        } else {
            bad[position] = bad[position + 1u] = 255u;
            reject_decode(bad, wire.len);
            position += 2u;
            for (unsigned c = 0u; c < 2u; ++c) {
                memcpy(bad, wire.data, wire.len);
                bad[position] = c ? 255u : 0u;
                reject_decode(bad, wire.len);
            }
            position += 3u;
        }
        for (unsigned fault = 0u; fault < 3u; ++fault) {
            struct snag_session changed = value;
            char *text = (char *)&changed + f->offset;
            if (!fault) memset(text, 'a', f->size);
            else if (f->wire) {
                if (fault == 1u) text[0] = 'A';
                else text[1] = '\0';
            } else {
                text[0] = (char)255u; text[1] = '\0';
            }
            reject_encode(&changed);
        }
    }
    assert(position == 358u);
    for (size_t i = 0u; i < COUNT(numbers); ++i) {
        const struct field *f = &numbers[i];
        if (f->maximum < UINT32_MAX) {
            struct snag_session changed = value;
            set_number(&changed, f, f->maximum + 1u);
            reject_encode(&changed);
            memcpy(bad, wire.data, wire.len);
            memset(bad + position, 255, f->wire);
            reject_decode(bad, wire.len);
            if (!strcmp(f->name, "format_version")) {
                for (uint64_t version = 0u; version < 2u; ++version) {
                    set_number(&changed, f, version);
                    reject_encode(&changed);
                }
                memset(bad + position, 0, f->wire);
                reject_decode(bad, wire.len);
                bad[position] = 1u;
                reject_decode(bad, wire.len);
            }
        } else {
            struct snag_session changed = value;
            uint64_t maximum = f->maximum;
            if (f->size < 8u) maximum = (UINT64_C(1) << (f->size * 8u)) - 1u;
            set_number(&changed, f, maximum);
            struct snag_buf encoded;
            snag_buf_init(&encoded, SIZE_MAX);
            roundtrip(&changed, &encoded);
            assert(encoded.len == wire.len);
            for (size_t j = 0u; j < f->wire; ++j)
                assert(encoded.data[position + j] == (unsigned char)(maximum >> (j * 8u)));
            snag_buf_free(&encoded);
            if (f->size < f->wire) {
                memcpy(bad, wire.data, wire.len);
                memset(bad + position, 255, f->wire);
                struct snag_session out;
                unsigned char kept[sizeof(out)];
                memset(kept, 0xa5, sizeof(kept));
                memcpy(&out, kept, sizeof(out));
                assert(snag_binary_checkpoint_controls_decode(bad, wire.len, &out) < 0 &&
                    errno == EOVERFLOW);
                assert(!memcmp(kept, &out, sizeof(out)));
            }
        }
        position += f->wire;
    }
    assert(position + 48u == wire.len);
    struct snag_buf limited;
    snag_buf_init(&limited, wire.len + 3u);
    assert(!snag_buf_append(&limited, "keep", 4u));
    assert(snag_binary_checkpoint_controls_encode(&limited, &value) < 0 && errno == EOVERFLOW);
    assert(limited.len == 4u && !memcmp(limited.data, "keep", 4u));
    ++limited.max;
    assert(!snag_binary_checkpoint_controls_encode(&limited, &value));
    assert(limited.len == wire.len + 4u && !memcmp(limited.data + 4u, wire.data, wire.len));
    snag_buf_free(&limited);
    struct snag_buf aliased;
    snag_buf_init(&aliased, sizeof(value));
    assert(!snag_buf_append(&aliased, &value, sizeof(value)));
    aliased.max = SIZE_MAX;
    assert(!snag_binary_checkpoint_controls_encode(&aliased,
        (const struct snag_session *)aliased.data));
    assert(aliased.len == sizeof(value) + wire.len &&
        !memcmp(aliased.data, &value, sizeof(value)) &&
        !memcmp(aliased.data + sizeof(value), wire.data, wire.len));
    assert(!snag_buf_reserve(&aliased, sizeof(value)));
    struct snag_session *overlap = (struct snag_session *)(aliased.data + sizeof(value));
    assert(!snag_binary_checkpoint_controls_decode(overlap, wire.len, overlap));
    snag_buf_init(&limited, SIZE_MAX);
    assert(!snag_binary_checkpoint_controls_encode(&limited, overlap));
    assert(limited.len == wire.len && !memcmp(limited.data, wire.data, wire.len));
    snag_buf_free(&limited);
    snag_buf_free(&aliased);
    for (unsigned version = 2u; version <= 4u; ++version) {
        struct snag_session minimal;
        snag_session_init(&minimal);
        minimal.format_version = version;
        roundtrip(&minimal, &wire);
        assert(wire.len == 316u && wire.data[0] == 1u && wire.data[112] == version);
        for (size_t i = 1u; i < wire.len; ++i)
            if (i != 112u) assert(wire.data[i] == 0u);
        snag_session_close(&minimal);
    }
    for (size_t i = 0u; i < COUNT(texts); ++i) {
        struct snag_session changed = value;
        char *text = (char *)&changed + texts[i].offset;
        memset(text, 0, texts[i].size);
        roundtrip(&changed, &wire);
        memset(text, texts[i].wire ? '0' : 'p', texts[i].size - 1u);
        roundtrip(&changed, &wire);
    }
    for (size_t i = 0u; i < 6u; ++i) value.control_seq[i] = UINT64_MAX - i;
    roundtrip(&value, &wire);
    snag_buf_free(&wire);
    snag_session_close(&value);
}
