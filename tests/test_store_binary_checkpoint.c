/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_checkpoint.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

void test_store_binary_checkpoint(void);
void test_store_binary_controls_state(const struct snag_session *);
void test_store_binary_checkpoint_state(const struct snag_session *);

static struct snag_input_observation *
observation(struct snag_binary_checkpoint_accounting *value, size_t index)
{
    switch (index) {
    case 0u: return &value->active_accounting;
    case 1u: return &value->usage_anchor;
    case 2u: return &value->context_meter;
    default: assert(index == 3u); return &value->capacity_rejection;
    }
}

static void
fill_observation(struct snag_input_observation *value, size_t index, unsigned flags)
{
    static const uint64_t numbers[] = {
        0u, INT64_MAX, UINT64_C(1) << 63u, UINT64_MAX, UINT64_C(0x1122334455667788)};
    memset(value, 0, sizeof(*value));
    assert(snprintf(value->provider, sizeof(value->provider), "p%zu", index) == 2);
    memcpy(value->model, "m\xc3\xa9", 4u);
    if (!(index & 1u)) memcpy(value->effort, "high", 5u);
    value->valid = (flags & 1u) != 0u;
#define HEX(f, bit, digit) do { \
    if (flags & (1u << bit)) memset(value->f, digit, sizeof(value->f) - 1u); \
} while (0)
    HEX(compact_id, 1u, 'a'); HEX(provider_source_sha256, 2u, 'b');
    HEX(model_input_sha256, 3u, 'c'); HEX(request_input_sha256, 4u, 'd');
    HEX(request_sha256, 5u, 'e');
#undef HEX
    value->model_input_bytes = numbers[index % 5u];
    value->request_input_bytes = numbers[(index + 1u) % 5u];
    value->request_input_count = numbers[(index + 2u) % 5u];
    value->input_tokens = numbers[(index + 3u) % 5u];
    value->requested_output_tokens = numbers[(index + 4u) % 5u];
}

static void
assert_observation(const struct snag_input_observation *a, const struct snag_input_observation *b)
{
#define TEXT(f) assert(!strcmp(a->f, b->f))
    TEXT(provider); TEXT(model); TEXT(effort); TEXT(compact_id);
    TEXT(provider_source_sha256); TEXT(model_input_sha256);
    TEXT(request_input_sha256); TEXT(request_sha256);
#undef TEXT
#define NUMBER(f) assert(a->f == b->f)
    NUMBER(model_input_bytes); NUMBER(request_input_bytes); NUMBER(request_input_count);
    NUMBER(input_tokens); NUMBER(requested_output_tokens); NUMBER(valid);
#undef NUMBER
}

static void
roundtrip(const struct snag_binary_checkpoint_accounting *value, struct snag_buf *wire)
{
    snag_buf_reset(wire);
    assert(!snag_binary_checkpoint_accounting_encode(wire, value));
    struct snag_binary_checkpoint_accounting decoded;
    memset(&decoded, 0xa5, sizeof(decoded));
    assert(!snag_binary_checkpoint_accounting_decode(wire->data, wire->len, &decoded));
#define OBS(f) assert_observation(&value->f, &decoded.f)
    OBS(active_accounting); OBS(usage_anchor); OBS(context_meter); OBS(capacity_rejection);
#undef OBS
#define TOTAL(f) assert(value->usage_totals.f == decoded.usage_totals.f)
    TOTAL(responses); TOTAL(input_tokens); TOTAL(cached_input_tokens); TOTAL(uncached_input_tokens);
    TOTAL(output_tokens); TOTAL(reasoning_tokens); TOTAL(total_tokens); TOTAL(cached_seen);
#undef TOTAL
    struct snag_buf second;
    snag_buf_init(&second, SIZE_MAX);
    assert(!snag_binary_checkpoint_accounting_encode(&second, &decoded));
    assert(second.len == wire->len && !memcmp(second.data, wire->data, wire->len));
    snag_buf_free(&second);
}

static void
reject_decode(const void *data, size_t size)
{
    struct snag_binary_checkpoint_accounting value;
    unsigned char saved[sizeof(value)];
    memset(saved, 0xa5, sizeof(saved));
    memcpy(&value, saved, sizeof(value));
    errno = 0;
    assert(snag_binary_checkpoint_accounting_decode(data, size, &value) < 0 && errno == EINVAL);
    assert(!memcmp(saved, &value, sizeof(value)));
}

static void
reject_encode(const struct snag_binary_checkpoint_accounting *value)
{
    struct snag_buf wire;
    snag_buf_init(&wire, SIZE_MAX);
    assert(!snag_buf_append(&wire, "keep", 4u));
    errno = 0;
    assert(snag_binary_checkpoint_accounting_encode(&wire, value) < 0 && errno == EINVAL);
    assert(wire.len == 4u && !memcmp(wire.data, "keep", 4u));
    snag_buf_free(&wire);
}

void
test_store_binary_checkpoint_state(const struct snag_session *state)
{
    struct snag_binary_checkpoint_accounting value = {
        .active_accounting = state->active_accounting, .usage_anchor = state->usage_anchor,
        .context_meter = state->context_meter, .capacity_rejection = state->capacity_rejection,
        .usage_totals = state->usage_totals
    };
    struct snag_buf wire;
    snag_buf_init(&wire, SIZE_MAX);
    roundtrip(&value, &wire);
    snag_buf_free(&wire);
    test_store_binary_controls_state(state);
}

void
test_store_binary_checkpoint(void)
{
    struct snag_binary_checkpoint_accounting value = {0};
    struct snag_buf wire;
    snag_buf_init(&wire, SIZE_MAX);
    roundtrip(&value, &wire);
    assert(wire.len == 247u && wire.data[0] == 1u);
    for (size_t i = 1u; i < wire.len; ++i) assert(wire.data[i] == 0u);
    static const unsigned flags[] = {63u, 62u, 21u, 3u};
    for (size_t i = 0u; i < 4u; ++i) fill_observation(observation(&value, i), i, flags[i]);
    value.usage_totals = (struct snag_usage_totals){
        .responses = UINT64_MAX, .input_tokens = UINT64_C(1) << 63u,
        .cached_input_tokens = INT64_MAX, .uncached_input_tokens = UINT64_C(0x1122334455667788),
        .output_tokens = 1u, .reasoning_tokens = 0u, .total_tokens = 17u, .cached_seen = true};
    roundtrip(&value, &wire);
    /* Independent Python struct.pack('<H'), per-observation strings/flags,
     * optional binary IDs/hashes, '<5Q', and final '<7QB' golden. */
    char digest[SNAG_SHA256_HEX_LEN + 1u];
    snag_sha256_hex(wire.data, wire.len, digest);
    assert(wire.len == 643u);
    assert(!strcmp(digest, "622f311a7a06f2c58cc470fe219341df99592393dbbbed2e6125a67c9f55e88f"));
    for (size_t i = 0u; i < wire.len; ++i) reject_decode(wire.data, i);
    reject_decode(NULL, wire.len);
    assert(snag_binary_checkpoint_accounting_decode(wire.data, wire.len, NULL) < 0);
    reject_encode(NULL);
    assert(snag_binary_checkpoint_accounting_encode(NULL, &value) < 0);

    unsigned char bad[644];
    memcpy(bad, wire.data, wire.len);
    bad[643] = 0u;
    reject_decode(bad, sizeof(bad));
    for (size_t i = 0u; i < 3u; ++i) {
        memcpy(bad, wire.data, wire.len);
        if (i == 0u) bad[0] = 0u;
        else if (i == 1u) bad[0] = 2u;
        else bad[1] = 1u;
        reject_decode(bad, wire.len);
    }
    memcpy(bad, wire.data, wire.len);
    bad[wire.len - 1u] = 2u;
    reject_decode(bad, wire.len);
    size_t position = 2u;
    for (size_t i = 0u; i < 4u; ++i) {
        for (unsigned bit = 6u; bit < 8u; ++bit) {
            memcpy(bad, wire.data, wire.len);
            bad[position] |= (unsigned char)(1u << bit);
            reject_decode(bad, wire.len);
        }
        ++position;
        for (size_t j = 0u; j < 3u; ++j) {
            size_t length = wire.data[position] | ((size_t)wire.data[position + 1u] << 8u);
            memcpy(bad, wire.data, wire.len);
            bad[position] = bad[position + 1u] = 255u;
            reject_decode(bad, wire.len);
            position += 2u;
            if (length) {
                for (unsigned byte = 0u; byte < 2u; ++byte) {
                    memcpy(bad, wire.data, wire.len);
                    bad[position] = byte ? 255u : 0u;
                    reject_decode(bad, wire.len);
                }
            }
            position += length;
        }
        for (unsigned bit = 1u; bit <= 5u; ++bit)
            if (flags[i] & (1u << bit)) position += bit == 1u ? 16u : 32u;
        position += 40u;
    }
    assert(position + 57u == wire.len);
    for (size_t i = 0u; i < 4u; ++i) {
        for (size_t j = 0u; j < 8u; ++j) {
            for (size_t fault = 0u; fault < 3u; ++fault) {
                struct snag_binary_checkpoint_accounting changed = value;
                struct snag_input_observation *o = observation(&changed, i);
                char *fields[] = {o->provider, o->model, o->effort, o->compact_id,
                    o->provider_source_sha256, o->model_input_sha256,
                    o->request_input_sha256, o->request_sha256};
                const size_t widths[] = {sizeof(o->provider), sizeof(o->model), sizeof(o->effort),
                    sizeof(o->compact_id), sizeof(o->provider_source_sha256),
                    sizeof(o->model_input_sha256), sizeof(o->request_input_sha256),
                    sizeof(o->request_sha256)};
                if (fault == 0u) memset(fields[j], 'a', widths[j]);
                else if (j < 3u) { fields[j][0] = (char)255u; fields[j][1] = '\0'; }
                else {
                    memset(fields[j], 'a', widths[j] - 1u);
                    if (fault == 1u) fields[j][1] = '\0';
                    else fields[j][0] = 'A';
                }
                reject_encode(&changed);
            }
        }
    }
    struct snag_buf limited;
    snag_buf_init(&limited, wire.len + 3u);
    assert(!snag_buf_append(&limited, "keep", 4u));
    assert(snag_binary_checkpoint_accounting_encode(&limited, &value) < 0 && errno == EOVERFLOW);
    assert(limited.len == 4u && !memcmp(limited.data, "keep", 4u));
    ++limited.max;
    assert(!snag_binary_checkpoint_accounting_encode(&limited, &value));
    assert(limited.len == wire.len + 4u && !memcmp(limited.data + 4u, wire.data, wire.len));
    snag_buf_free(&limited);
    struct snag_buf aliased;
    snag_buf_init(&aliased, sizeof(value));
    assert(!snag_buf_append(&aliased, &value, sizeof(value)));
    aliased.max = SIZE_MAX;
    assert(!snag_binary_checkpoint_accounting_encode(&aliased,
        (const struct snag_binary_checkpoint_accounting *)aliased.data));
    assert(aliased.len == sizeof(value) + wire.len);
    assert(!memcmp(aliased.data, &value, sizeof(value)));
    assert(!memcmp(aliased.data + sizeof(value), wire.data, wire.len));
    /* Finish reading the source before adopting overlapping output. */
    assert(!snag_buf_reserve(&aliased, sizeof(value)));
    struct snag_binary_checkpoint_accounting *overlap =
        (struct snag_binary_checkpoint_accounting *)(aliased.data + sizeof(value));
    assert(!snag_binary_checkpoint_accounting_decode(overlap, wire.len, overlap));
    snag_buf_init(&limited, SIZE_MAX);
    assert(!snag_binary_checkpoint_accounting_encode(&limited, overlap));
    assert(limited.len == wire.len && !memcmp(limited.data, wire.data, wire.len));
    snag_buf_free(&limited);
    snag_buf_free(&aliased);
    for (unsigned bits = 0u; bits < 64u; ++bits) {
        for (size_t i = 0u; i < 4u; ++i) {
            struct snag_input_observation *o = observation(&value, i);
            fill_observation(o, i, (bits + (unsigned)i * 7u) & 63u);
            /* Present all-zero IDs are distinct from absent metadata. */
            if (o->compact_id[0]) memset(o->compact_id, '0', sizeof(o->compact_id) - 1u);
            if (o->request_sha256[0])
                memset(o->request_sha256, '0', sizeof(o->request_sha256) - 1u);
        }
        value.usage_totals.cached_seen = (bits & 1u) != 0u;
        roundtrip(&value, &wire);
    }
    for (size_t i = 0u; i < 4u; ++i) {
        struct snag_input_observation *o = observation(&value, i);
        memset(o->provider, 'p', sizeof(o->provider) - 1u);
        memset(o->model, 'm', sizeof(o->model) - 1u);
        memset(o->effort, 'e', sizeof(o->effort) - 1u);
    }
    roundtrip(&value, &wire);
    snag_buf_free(&wire);
}
