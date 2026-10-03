/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_checkpoint.h"
#include "fs.h"
#include "checked_json.h"

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

void test_store_binary_texts(void);
void test_store_binary_texts_state(int, const struct snag_binary_anchor *,
    const struct snag_binary_checkpoint_texts *, const struct snag_session *);
void test_store_binary_texts_bad(int, const struct snag_binary_anchor *,
    const struct snag_binary_checkpoint_texts *);

static const struct { const char *name; size_t offset; } slots[] = {
#define SLOT(f) {#f, offsetof(struct snag_session, f)}
    SLOT(cwd), SLOT(first_user), SLOT(last_user), SLOT(active_prompt), SLOT(goal_prompt),
    SLOT(goal_blocker), SLOT(timer_text), SLOT(banner_text), SLOT(steering_override),
    {"irc_snapshot", 0u}
#undef SLOT
};

static void
same(const struct snag_binary_checkpoint_texts *a, const struct snag_binary_checkpoint_texts *b)
{
    assert(a->through == b->through);
    for (size_t i = 0u; i < COUNT(a->slots); ++i) {
        const struct snag_binary_checkpoint_text_source *x = &a->slots[i], *y = &b->slots[i];
        assert(x->declaration == y->declaration && x->original.field == y->original.field &&
            x->original.target.sequence == y->original.target.sequence &&
            x->original.target.offset == y->original.target.offset &&
            x->original.target.size == y->original.target.size);
    }
}

static void
roundtrip(const struct snag_binary_checkpoint_texts *value, struct snag_buf *wire)
{
    snag_buf_reset(wire);
    assert(!snag_binary_checkpoint_texts_encode(wire, value));
    assert(wire->len == 260u);
    struct snag_binary_checkpoint_texts restored;
    memset(&restored, 0xa5, sizeof(restored));
    assert(!snag_binary_checkpoint_texts_decode(wire->data, wire->len, &restored));
    same(value, &restored);
    struct snag_buf again = {.max = SIZE_MAX};
    assert(!snag_binary_checkpoint_texts_encode(&again, &restored));
    assert(again.len == wire->len && !memcmp(again.data, wire->data, wire->len));
    snag_buf_free(&again);
}

void
test_store_binary_texts_state(int fd, const struct snag_binary_anchor *anchor,
    const struct snag_binary_checkpoint_texts *value, const struct snag_session *state)
{
    assert(value->through < anchor->next_seq);
    struct snag_buf wire = {.max = SIZE_MAX};
    roundtrip(value, &wire);
    struct snag_binary_checkpoint_texts restored;
    assert(!snag_binary_checkpoint_texts_decode(wire.data, wire.len, &restored));
    json_t *old = json_string("keep"), *strings = old;
    int64_t position = snag_seek(fd, 0, SEEK_CUR);
    assert(position >= 0);
    assert(!snag_binary_checkpoint_texts_read(fd, anchor, &restored, &strings));
    assert(!strcmp(json_string_value(old), "keep") && old != strings);
    assert(snag_seek(fd, 0, SEEK_CUR) == position);
    size_t present = 0u;
    for (size_t i = 0u; i < COUNT(slots); ++i) {
        const char *expected = snag_json_string(state->strings, "irc_snapshot");
        if (i != SNAG_BINARY_TEXT_IRC_SNAPSHOT) {
            memcpy(&expected, (const unsigned char *)state + slots[i].offset, sizeof(expected));
        }
        json_t *actual = json_object_get(strings, slots[i].name);
        if (expected) {
            ++present;
            assert(value->slots[i].declaration && json_is_string(actual));
            assert(strlen(expected) == json_string_length(actual) &&
                !memcmp(expected, json_string_value(actual), strlen(expected)));
        } else assert(!actual && !value->slots[i].declaration);
    }
    assert(json_object_size(strings) == present);
    json_decref(strings);
    json_decref(old);
    snag_buf_free(&wire);
}

static void
reject_read(int fd, const struct snag_binary_anchor *anchor,
    const struct snag_binary_checkpoint_texts *value)
{
    json_t *old = json_string("keep"), *out = old;
    assert(snag_binary_checkpoint_texts_read(fd, anchor, value, &out) < 0);
    assert(out == old && !strcmp(json_string_value(out), "keep"));
    json_decref(old);
}

void
test_store_binary_texts_bad(int fd, const struct snag_binary_anchor *anchor,
    const struct snag_binary_checkpoint_texts *value)
{
    reject_read(-1, anchor, value);
    reject_read(fd, NULL, value);
    reject_read(fd, anchor, NULL);
    assert(snag_binary_checkpoint_texts_read(fd, anchor, value, NULL) < 0);
    int closed = dup(fd);
    assert(closed >= 0 && !close(closed));
    reject_read(closed, anchor, value);
    struct snag_binary_anchor wrong = *anchor;
    wrong.digest[0] ^= 1u;
    reject_read(fd, &wrong, value);
    struct snag_binary_checkpoint_texts changed = *value;
    changed.through = anchor->next_seq;
    reject_read(fd, anchor, &changed);
    for (size_t i = 0u; i < COUNT(value->slots); ++i) {
        if (!value->slots[i].declaration || i == SNAG_BINARY_TEXT_STEERING) continue;
        for (unsigned fault = 0u; fault < 3u; ++fault) {
            changed = *value;
            struct snag_binary_ref *ref = &changed.slots[i].original.target;
            if (!fault) ++ref->offset;
            else if (fault == 1u) ++ref->size;
            else {
                /* Well-formed control locator, wrong declaration role. */
                changed.slots[i] = value->slots[SNAG_BINARY_TEXT_CWD];
                if (i == SNAG_BINARY_TEXT_CWD) continue;
            }
            reject_read(fd, anchor, &changed);
        }
    }
    changed = *value;
    if (value->slots[SNAG_BINARY_TEXT_GOAL_PROMPT].declaration) {
        changed.slots[SNAG_BINARY_TEXT_FIRST_USER] = value->slots[SNAG_BINARY_TEXT_GOAL_PROMPT];
        reject_read(fd, anchor, &changed); /* This fixture's prompt is a model reword. */
        changed = *value;
        changed.slots[SNAG_BINARY_TEXT_LAST_USER] = value->slots[SNAG_BINARY_TEXT_GOAL_PROMPT];
        reject_read(fd, anchor, &changed);
    }
}

static void
reject_decode(const void *bytes, size_t size)
{
    struct snag_binary_checkpoint_texts out;
    unsigned char before[sizeof(out)];
    memset(before, 0xa5, sizeof(before));
    memcpy(&out, before, sizeof(out));
    errno = 0;
    assert(snag_binary_checkpoint_texts_decode(bytes, size, &out) < 0 && errno == EINVAL);
    assert(!memcmp(before, &out, sizeof(out)));
}

static void
reject_encode(const struct snag_binary_checkpoint_texts *value)
{
    struct snag_buf out = {.max = SIZE_MAX};
    assert(!snag_buf_append(&out, "keep", 4u));
    assert(snag_binary_checkpoint_texts_encode(&out, value) < 0 && errno == EINVAL);
    assert(out.len == 4u && !memcmp(out.data, "keep", 4u));
    snag_buf_free(&out);
}

static struct snag_binary_text
text(const char *value)
{
    return (struct snag_binary_text){(const unsigned char *)value, strlen(value)};
}

static void
step_test(void)
{
    struct snag_binary_event event = {.kind = SNAG_BINARY_SESSION_CREATED};
    event.data.created.source_format = 4u;
    event.data.created.protocol = SNAG_BINARY_RESPONSES;
    event.data.created.selection = (struct snag_binary_selection){text("p"), text("m"), text("e")};
    event.data.created.cwd = text("/");
    struct snag_buf payload = {.max = SIZE_MAX};
    assert(!snag_binary_event_encode(&payload, &event));
    struct snag_binary_record record = {
        .kind = (uint16_t)event.kind, .version = 1u, .payload = payload.data, .size = payload.len
    };
    struct snag_session state;
    snag_session_init(&state);
    state.cwd = "/";
    assert(!state.strings);
    state.strings = checked_json(json_pack("{s:s}", "irc_snapshot", ""));
    struct snag_binary_checkpoint_texts empty = {0}, sources = empty;
    for (size_t size = 0u; size < payload.len; ++size) {
        record.size = size;
        assert(snag_binary_checkpoint_texts_step(&sources, &record, 1u, &state) < 0);
        same(&empty, &sources);
    }
    record.size = payload.len;
    assert(snag_binary_checkpoint_texts_step(NULL, &record, 1u, &state) < 0);
    assert(snag_binary_checkpoint_texts_step(&sources, NULL, 1u, &state) < 0);
    assert(snag_binary_checkpoint_texts_step(&sources, &record, 1u, NULL) < 0);
    assert(snag_binary_checkpoint_texts_step(&sources, &record, 0u, &state) < 0);
    assert(snag_binary_checkpoint_texts_step(&sources, &record, 2u, &state) < 0);
    assert(snag_binary_checkpoint_texts_step(&sources, &record, UINT64_MAX, &state) < 0);
    state.cwd = NULL;
    assert(snag_binary_checkpoint_texts_step(&sources, &record, 1u, &state) < 0);
    state.cwd = "!";
    assert(snag_binary_checkpoint_texts_step(&sources, &record, 1u, &state) < 0);
    same(&empty, &sources);
    state.cwd = "/";
    assert(!snag_binary_checkpoint_texts_step(&sources, &record, 1u, &state));
    assert(sources.through == 1u && sources.slots[SNAG_BINARY_TEXT_CWD].declaration == 1u);
    assert(sources.slots[SNAG_BINARY_TEXT_IRC_SNAPSHOT].declaration == 1u);
    assert(!sources.slots[SNAG_BINARY_TEXT_IRC_SNAPSHOT].original.target.sequence);
    struct snag_binary_checkpoint_texts saved = sources;
    assert(snag_binary_checkpoint_texts_step(&sources, &record, 1u, &state) < 0);
    same(&saved, &sources);
    event.kind = SNAG_BINARY_STEERING_UPDATED;
    event.data.steering = SNAG_BINARY_STEERING_MENTIONS;
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    record.kind = (uint16_t)event.kind;
    record.payload = payload.data;
    record.size = payload.len;
    state.steering_override = "all";
    assert(snag_binary_checkpoint_texts_step(&sources, &record, 2u, &state) < 0);
    same(&saved, &sources);
    state.steering_override = "mentions";
    assert(!snag_binary_checkpoint_texts_step(&sources, &record, 2u, &state));
    assert(sources.slots[SNAG_BINARY_TEXT_STEERING].declaration == 2u);
    saved = sources;
    event.kind = SNAG_BINARY_IRC_SNAPSHOT;
    event.data.irc_snapshot = (struct snag_binary_irc_snapshot){
        .reason = SNAG_BINARY_IRC_SNAPSHOT_JOIN, .timestamp_ms = 1u,
        .text = text("retained room snapshot")};
    snag_buf_reset(&payload);
    assert(!snag_binary_event_encode(&payload, &event));
    record.kind = (uint16_t)event.kind;
    record.payload = payload.data;
    record.size = payload.len;
    assert(snag_binary_checkpoint_texts_step(&sources, &record, 3u, &state) < 0);
    same(&saved, &sources);
    assert(!snag_json_set_new(state.strings, "irc_snapshot",
        json_string("retained room snapshot")));
    assert(!snag_binary_checkpoint_texts_step(&sources, &record, 3u, &state));
    assert(sources.slots[SNAG_BINARY_TEXT_IRC_SNAPSHOT].declaration == 3u);
    assert(sources.slots[SNAG_BINARY_TEXT_IRC_SNAPSHOT].original.target.sequence == 3u);
    snag_buf_free(&payload);
    snag_session_close(&state);
}

void
test_store_binary_texts(void)
{
    step_test();
    const uint64_t base = UINT64_C(0x0102030405060700);
    struct snag_binary_checkpoint_texts value = {.through = base + 15u};
    static const unsigned fields[] = {0u, 1u, 1u, 5u, 0u, 0u, 0u, 0u, 0u, 0u};
    for (size_t i = 0u; i < COUNT(value.slots); ++i) {
        value.slots[i].declaration = base + i + 1u;
        if (i == SNAG_BINARY_TEXT_STEERING) continue;
        value.slots[i].original.field = fields[i];
        value.slots[i].original.target = (struct snag_binary_ref){
            fields[i] ? base + 1u : base + i + 1u, (uint32_t)(20u + i), (uint32_t)i};
    }
    struct snag_buf wire = {.max = SIZE_MAX};
    roundtrip(&value, &wire);
    char hash[65];
    snag_sha256_hex(wire.data, wire.len, hash);
    /* Independent Python struct/hashlib fixture. */
    assert(!strcmp(hash, "7023d1dfb168eb085be1482173376e6bc86fad2313f6c9fdef5ff29ac88586ff"));
    for (size_t size = 0u; size < wire.len; ++size) reject_decode(wire.data, size);
    reject_decode(NULL, wire.len);
    assert(snag_binary_checkpoint_texts_decode(wire.data, wire.len, NULL) < 0);
    reject_encode(NULL);
    assert(snag_binary_checkpoint_texts_encode(NULL, &value) < 0);
    unsigned char bad[261];
    for (unsigned version = 0u; version < 3u; ++version) {
        memcpy(bad, wire.data, wire.len);
        bad[0] = version ? 2u : 0u;
        if (version == 2u) { bad[0] = 1u; bad[1] = 1u; }
        reject_decode(bad, wire.len);
    }
    memcpy(bad, wire.data, wire.len);
    bad[260] = 0u;
    reject_decode(bad, sizeof(bad));
    for (size_t i = 0u; i < COUNT(value.slots); ++i) {
        size_t pos = 10u + 25u * i;
        for (unsigned fault = 0u; fault < 5u; ++fault) {
            memcpy(bad, wire.data, wire.len);
            if (!fault) memset(bad + pos, 255, 8u);
            else if (fault == 1u) bad[pos + 8u] = 255u;
            else if (fault == 2u) memset(bad + pos, 0, 8u);
            else if (fault == 3u) memset(bad + pos + 9u, 255, 8u);
            else {
                memset(bad + pos + 9u, 0, 8u);
                bad[pos + 17u] = 1u;
            }
            if (i == SNAG_BINARY_TEXT_STEERING && fault == 2u) {
                struct snag_binary_checkpoint_texts cleared;
                assert(!snag_binary_checkpoint_texts_decode(bad, wire.len, &cleared));
                assert(!cleared.slots[i].declaration);
            } else reject_decode(bad, wire.len);
        }
        struct snag_binary_checkpoint_texts changed = value;
        memset(&changed.slots[i], 0, sizeof(changed.slots[i]));
        if (i == SNAG_BINARY_TEXT_CWD) reject_encode(&changed);
        else roundtrip(&changed, &wire);
        roundtrip(&value, &wire);
        changed = value;
        changed.slots[i].original.field = SNAG_BINARY_INPUT_CONTENT;
        reject_encode(&changed);
    }
    struct snag_binary_checkpoint_texts changed = value;
    changed.slots[SNAG_BINARY_TEXT_IRC_SNAPSHOT] =
        (struct snag_binary_checkpoint_text_source){.declaration = 1u};
    roundtrip(&changed, &wire);
    changed.slots[SNAG_BINARY_TEXT_IRC_SNAPSHOT].declaration = 2u;
    reject_encode(&changed);
    changed = value;
    changed.through = 0u;
    reject_encode(&changed);
    changed.through = UINT64_MAX;
    reject_encode(&changed);
    changed.through = base;
    reject_encode(&changed);
    changed = value;
    changed.slots[SNAG_BINARY_TEXT_ACTIVE_PROMPT].original.field =
        SNAG_BINARY_INPUT_VOICE_TRANSCRIPT;
    roundtrip(&changed, &wire);
    roundtrip(&value, &wire);
    struct snag_buf limited = {.max = wire.len + 3u};
    assert(!snag_buf_append(&limited, "keep", 4u));
    assert(snag_binary_checkpoint_texts_encode(&limited, &value) < 0 && errno == EOVERFLOW);
    assert(limited.len == 4u && !memcmp(limited.data, "keep", 4u));
    ++limited.max;
    assert(!snag_binary_checkpoint_texts_encode(&limited, &value));
    assert(limited.len == wire.len + 4u && !memcmp(limited.data + 4u, wire.data, wire.len));
    snag_buf_free(&limited);
    struct snag_buf alias = {.max = sizeof(value)};
    assert(!snag_buf_append(&alias, &value, sizeof(value)));
    alias.max = SIZE_MAX;
    assert(!snag_binary_checkpoint_texts_encode(&alias,
        (const struct snag_binary_checkpoint_texts *)alias.data));
    assert(!memcmp(alias.data, &value, sizeof(value)) &&
        !memcmp(alias.data + sizeof(value), wire.data, wire.len));
    assert(!snag_buf_reserve(&alias, sizeof(value)));
    struct snag_binary_checkpoint_texts *overlap =
        (struct snag_binary_checkpoint_texts *)(alias.data + sizeof(value));
    assert(!snag_binary_checkpoint_texts_decode(overlap, wire.len, overlap));
    same(&value, overlap);
    snag_buf_free(&alias);
    snag_buf_free(&wire);
}
