/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_public.h"
#include "secret_source.h"
#include "turn.h"
#include "vm_source.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct public_item {
    struct snag_buf text;
    char key[SNAG_ID_HEX_LEN + 24u];
    const char *response, *kind, *phase, *state, *resolved_state;
    uint64_t ordinal, seq, last_seq, begin, end;
    bool source_exact;
};

struct projection {
    struct public_item *items;
    size_t count, capacity, bytes;
    json_t *by_key;
};

static struct public_item *
find_item(struct projection *projection, const char *response, uint64_t ordinal, uint64_t seq)
{
    char key[SNAG_ID_HEX_LEN + 24u];
    if (!response || !snag_hex_is_lower(response, SNAG_ID_HEX_LEN)) {
        errno = EINVAL;
        return NULL;
    }
    (void)snprintf(key, sizeof(key), "%s/%llu", response, (unsigned long long)ordinal);
    json_t *found = json_object_get(projection->by_key, key);
    if (found) return projection->items + (size_t)json_integer_value(found);
    if (projection->count == projection->capacity) {
        size_t capacity = projection->capacity ? projection->capacity * 2u : 16u;
        if (capacity <= projection->capacity || capacity > SIZE_MAX / sizeof(*projection->items)) {
            errno = EOVERFLOW;
            return NULL;
        }
        struct public_item *items = realloc(projection->items, capacity * sizeof(*items));
        if (!items) return NULL;
        projection->items = items;
        projection->capacity = capacity;
    }
    if (json_object_set_new(projection->by_key, key, json_integer((json_int_t)projection->count)) <
        0)
        return NULL;
    struct public_item *item = &projection->items[projection->count++];
    *item = (struct public_item){.response = response,
        .ordinal = ordinal,
        .seq = seq,
        .state = "streaming",
        .source_exact = true,
        .text = {.max = SNAG_MEMORY_LIMIT / 2u}};
    memcpy(item->key, key, strlen(key) + 1u);
    return item;
}

static bool
is_public(const json_t *value)
{
    const char *kind = snag_json_string(value, "kind");
    return kind && (!strcmp(kind, "assistant") || !strcmp(kind, "refusal"));
}

static int
record_item(struct projection *projection, const char *response, uint64_t ordinal, uint64_t seq,
    const json_t *value, uint64_t offset, uint64_t source_length, const char *state)
{
    const char *text = snag_json_string(value, "text");
    const char *phase = snag_json_string(value, "phase");
    if (!is_public(value) || !text || !phase || offset > SNAG_MAX_PUBLIC_ITEM ||
        source_length > SNAG_MAX_PUBLIC_ITEM - offset)
        return snag_errno(EINVAL);
    struct public_item *item = find_item(projection, response, ordinal, seq);
    if (!item) return -1;
    bool replace = strcmp(state, "streaming") != 0;
    if (!replace && item->last_seq &&
        (item->end != offset || strcmp(item->kind, snag_json_string(value, "kind")) ||
            strcmp(item->phase, phase) || strcmp(item->state, "streaming")))
        return snag_errno(EINVAL);
    if (replace) {
        projection->bytes -= item->text.len;
        snag_secret_clear(item->text.data, item->text.len);
        item->text.len = 0u;
        item->source_exact = true;
    }
    size_t length = strlen(text);
    if (source_length != length) item->source_exact = false;
    /* This is a working projection of a bounded event page, not retained
     * history. Leave half the existing process budget for source/grid caches. */
    if (length > SNAG_MEMORY_LIMIT / 2u - projection->bytes) return snag_errno(EOVERFLOW);
    if (snag_buf_append(&item->text, text, length) < 0) return -1;
    projection->bytes += length;
    if (!item->last_seq || replace) item->begin = offset;
    item->end = offset + source_length;
    item->last_seq = seq;
    item->kind = snag_json_string(value, "kind");
    item->phase = phase;
    item->state = state;
    return 0;
}

const char *
snag_vm_public_state(const char *type)
{
    return !strcmp(type, "response_completed")           ? "complete"
           : !strcmp(type, "response_interrupted")       ? "interrupted"
           : !strcmp(type, "response_failed")            ? "failed"
           : !strcmp(type, "response_output_correction") ? "corrected"
                                                         : NULL;
}

static size_t
slice_boundary(const char *text, size_t length, uint64_t offset)
{
    size_t at = offset < length ? (size_t)offset : length;
    while (at && at < length && ((unsigned char)text[at] & 0xc0u) == 0x80u) --at;
    return at;
}

static int
resolved_output(struct projection *projection, const char *response, uint64_t ordinal, uint64_t seq,
    const json_t *value, uint64_t offset, const json_t *snapshot)
{
    const json_t *canonical = json_array_get(json_object_get(snapshot, "items"), (size_t)ordinal);
    const char *text = snag_json_string(canonical, "text");
    const char *original = snag_json_string(value, "text");
    if (!original || offset > SNAG_MAX_PUBLIC_ITEM ||
        strlen(original) > SNAG_MAX_PUBLIC_ITEM - offset)
        return snag_errno(EINVAL);
    const char *state = text ? snag_json_string(snapshot, "state") : "unconfirmed";
    if (!state) return snag_errno(EINVAL);
    size_t begin = (size_t)offset, length = strlen(original);
    json_t *part = json_copy((json_t *)(text ? canonical : value));
    if (!part) return -1;
    int rc = -1;
    if (text) {
        size_t end = slice_boundary(text, strlen(text), offset + length);
        begin = slice_boundary(text, strlen(text), offset);
        length = end - begin;
        if (json_object_set_new(part, "text", json_stringn(text + begin, length)) < 0) goto out;
    }
    if (!length) {
        rc = 0;
        goto out;
    }
    rc = record_item(projection, response, ordinal, seq, part, begin, length, "streaming");
    if (!rc) {
        struct public_item *item = find_item(projection, response, ordinal, seq);
        item->resolved_state = state;
        item->last_seq = (uint64_t)json_integer_value(json_object_get(snapshot, "seq"));
    }
out:
    json_decref(part);
    return rc;
}

static int
project_event(struct projection *projection, const json_t *event, uint64_t seq)
{
    const char *type = snag_json_string(event, "type");
    const json_t *data = json_object_get(event, "data");
    const char *response = snag_json_string(data, "response_id");
    if (!type) return snag_errno(EINVAL);
    if (!strcmp(type, "response_output")) {
        const json_t *item = json_object_get(data, "item");
        uint64_t ordinal, offset, length;
        const char *text = snag_json_string(item, "text");
        if (!text || snag_json_integer_u64(data, "index", &ordinal) < 0 ||
            snag_json_integer_u64(data, "offset", &offset) < 0)
            return snag_errno(EINVAL);
        const json_t *snapshot = json_object_get(event, "public_snapshot");
        if (snapshot)
            return resolved_output(projection, response, ordinal, seq, item, offset, snapshot);
        length = strlen(text);
        if (json_object_get(event, "source_text_bytes") &&
            snag_json_integer_u64(event, "source_text_bytes", &length) < 0)
            return -1;
        return record_item(projection, response, ordinal, seq, item, offset, length, "streaming");
    }
    const char *state = snag_vm_public_state(type);
    if (!state) return 0;
    const json_t *items =
        json_object_get(data, !strcmp(state, "complete") ? "items" : "partial_public");
    const json_t *lengths = json_object_get(event, "source_public_bytes");
    if (!json_is_array(items) || (lengths && !json_is_array(lengths))) return snag_errno(EINVAL);
    if (!response) return snag_errno(EINVAL);
    /* Retain observed output absent from a terminal snapshot, clearly labelled
     * as unconfirmed. It must neither disappear nor remain marked streaming. */
    for (size_t i = 0u; i < projection->count; ++i) {
        if (!strcmp(projection->items[i].response, response))
            projection->items[i].state = "unconfirmed";
    }
    uint64_t ordinal = 0u;
    for (size_t i = 0u; i < json_array_size(items); ++i) {
        const json_t *item = json_array_get(items, i);
        if (!is_public(item)) continue;
        const char *text = snag_json_string(item, "text");
        if (!text) return snag_errno(EINVAL);
        uint64_t length = strlen(text);
        if (lengths) {
            const json_t *value = json_array_get(lengths, (size_t)ordinal);
            if (!json_is_integer(value) || json_integer_value(value) < 0) return snag_errno(EINVAL);
            length = (uint64_t)json_integer_value(value);
        }
        size_t begin = 0u;
        const json_t *prior = json_object_get(event, "public_before");
        if (prior) {
            char key[SNAG_ID_HEX_LEN + 24u], index[24];
            (void)snprintf(key, sizeof(key), "%s/%llu", response, (unsigned long long)ordinal);
            (void)snprintf(index, sizeof(index), "%llu", (unsigned long long)ordinal);
            const json_t *found = json_object_get(projection->by_key, key);
            uint64_t covered = found ? projection->items[json_integer_value(found)].begin
                                     : (uint64_t)json_integer_value(json_object_get(prior, index));
            begin = slice_boundary(text, strlen(text), covered);
            length = strlen(text) - begin;
        }
        json_t *part = json_copy((json_t *)item);
        int rc = part ? 0 : -1;
        if (!rc && begin) rc = json_object_set_new(part, "text", json_string(text + begin));
        if (!rc && (length || !prior))
            rc = record_item(projection, response, ordinal, seq, part, begin, length, state);
        json_decref(part);
        if (rc < 0) return -1;
        ++ordinal;
    }
    if (lengths && ordinal != json_array_size(lengths)) return snag_errno(EINVAL);
    return 0;
}

int
snag_vm_public_source_bytes(json_t *event, const json_t *source)
{
    const char *type = snag_json_string(event, "type");
    if (!type) return snag_errno(EINVAL);
    if (!strcmp(type, "response_output")) {
        const json_t *text = json_object_get(json_object_get(source, "item"), "text");
        if (!json_is_string(text)) return snag_errno(EINVAL);
        return json_object_set_new(
            event, "source_text_bytes", json_integer(json_string_length(text)));
    }
    bool completed = !strcmp(type, "response_completed");
    if (!completed && strcmp(type, "response_interrupted") && strcmp(type, "response_failed") &&
        strcmp(type, "response_output_correction"))
        return 0;
    const json_t *items = json_object_get(source, completed ? "items" : "partial_public");
    if (!json_is_array(items)) return snag_errno(EINVAL);
    json_t *lengths = json_array();
    if (!lengths) return -1;
    for (size_t i = 0u; i < json_array_size(items); ++i) {
        const json_t *item = json_array_get(items, i);
        if (!is_public(item)) continue;
        const json_t *text = json_object_get(item, "text");
        if (!json_is_string(text) ||
            json_array_append_new(lengths, json_integer(json_string_length(text))) < 0) {
            json_decref(lengths);
            return snag_errno(EINVAL);
        }
    }
    return json_object_set_new(event, "source_public_bytes", lengths);
}

static json_t *
redact_text(const struct public_item *item, const struct snag_wire_secrets *secrets, json_t *map)
{
    struct snag_buf text = {.max = SNAG_MEMORY_LIMIT / 2u};
    bool streaming = !strcmp(item->state, "streaming") || !strcmp(item->state, "unconfirmed");
    for (size_t at = 0u; at < item->text.len;) {
        const unsigned char *bytes = item->text.data + at;
        size_t remaining = item->text.len - at;
        size_t matched =
            snag_wire_secret_span(bytes, remaining, !at && item->begin, streaming, secrets);
        if (matched) {
            if (item->source_exact &&
                snag_vm_source_replace(map, text.len, item->begin + at, 17u, matched) < 0)
                goto failed;
            if (snag_buf_append(&text, "<redacted:secret>", 17u) < 0) goto failed;
            at += matched;
        } else {
            if (snag_buf_putc(&text, item->text.data[at++]) < 0) goto failed;
        }
    }
    /* A caller may supply already-redacted text with only its original byte
     * count. That preserves the whole source span, not invented inner offsets.
     * The native transcript worker supplies raw validated events. */
    if (!item->source_exact && text.len && item->end > item->begin &&
        snag_vm_source_replace(map, 0u, item->begin, text.len, item->end - item->begin) < 0) {
        goto failed;
    }
    json_t *result = json_stringn(text.data ? (const char *)text.data : "", text.len);
    snag_buf_free(&text);
    return result;
failed:
    snag_buf_free(&text);
    return NULL;
}

json_t *
snag_vm_public_blocks(
    const json_t *events, const struct snag_wire_secrets *secrets, char *error, size_t size)
{
    struct projection projection = {.by_key = json_object()};
    json_t *result = NULL;
    if (!projection.by_key) goto out;
    if (!json_is_array(events) || (secrets && secrets->count && !secrets->values)) {
        errno = EINVAL;
        goto out;
    }
    uint64_t previous = 0u;
    for (size_t i = 0u; i < json_array_size(events); ++i) {
        const json_t *event = json_array_get(events, i);
        uint64_t seq;
        if (snag_json_integer_u64(event, "seq", &seq) < 0 || seq <= previous) {
            errno = EINVAL;
            goto out;
        }
        if (project_event(&projection, event, seq) < 0) goto out;
        previous = seq;
    }
    result = json_array();
    for (size_t i = 0u; result && i < projection.count; ++i) {
        struct public_item *item = &projection.items[i];
        if (item->resolved_state) item->state = item->resolved_state;
        json_t *map = json_array();
        json_t *text = map ? redact_text(item, secrets, map) : NULL;
        json_t *block =
            text ? json_pack("{s:s,s:s,s:I,s:I,s:I,s:I,s:I,s:s,s:s,s:s,s:o,s:O}", "key", item->key,
                       "response_id", item->response, "ordinal", (json_int_t)item->ordinal, "seq",
                       (json_int_t)item->seq, "last_seq", (json_int_t)item->last_seq,
                       "source_begin", (json_int_t)item->begin, "source_end", (json_int_t)item->end,
                       "kind", item->kind, "phase", item->phase, "state", item->state, "text", text,
                       "source_map", map)
                 : NULL;
        json_decref(map);
        if (!block || json_array_append_new(result, block) < 0) {
            json_decref(result);
            result = NULL;
        }
    }
out:
    if (!result)
        (void)snag_fail(error, size, errno ? errno : EINVAL,
            "cannot project response text: incomplete or invalid source range");
    for (size_t i = 0u; i < projection.count; ++i) {
        snag_secret_clear(projection.items[i].text.data, projection.items[i].text.len);
        snag_buf_free(&projection.items[i].text);
    }
    free(projection.items);
    json_decref(projection.by_key);
    return result;
}
