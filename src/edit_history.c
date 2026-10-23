/* SPDX-License-Identifier: GPL-2.0-only */
#include "edit_history.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct snag_edit {
    struct snag_edit *next;
    size_t begin, before_size, after_size, before_cursor, after_cursor;
    unsigned char text[];
};

static void
edits_free(struct snag_edit *edit)
{
    while (edit) {
        struct snag_edit *next = edit->next;
        memset(edit->text, 0, edit->before_size + edit->after_size);
        free(edit);
        edit = next;
    }
}

void
snag_edit_history_free(struct snag_edit_history *history)
{
    edits_free(history->undo);
    edits_free(history->redo);
    if (history->original.data) memset(history->original.data, 0, history->original.len);
    snag_buf_free(&history->original);
    memset(history, 0, sizeof(*history));
}

int
snag_edit_begin(struct snag_edit_history *history, const struct snag_buf *draft, size_t cursor)
{
    if (history->grouping) return 0;
    history->original.max = SNAG_MAX_DIRECT_PROMPT + 1u;
    snag_buf_reset(&history->original);
    if (snag_buf_append(&history->original, draft->data, draft->len) < 0) return -1;
    history->cursor = cursor;
    history->grouping = true;
    return 0;
}

int
snag_edit_end(struct snag_edit_history *history, const struct snag_buf *draft, size_t cursor)
{
    if (!history->grouping) return 0;
    const unsigned char *before = history->original.data, *after = draft->data;
    size_t old_size = history->original.len, new_size = draft->len;
    size_t prefix = 0u, suffix = 0u;
    while (prefix < old_size && prefix < new_size && before[prefix] == after[prefix]) ++prefix;
    if (prefix != old_size || prefix != new_size) {
        /* Delta edges remain UTF-8 boundaries when codepoints share bytes. */
        while (prefix && prefix < old_size && (before[prefix] & 0xc0u) == 0x80u) --prefix;
        while (suffix < old_size - prefix && suffix < new_size - prefix &&
            before[old_size - suffix - 1u] == after[new_size - suffix - 1u]) ++suffix;
        while (suffix && (before[old_size - suffix] & 0xc0u) == 0x80u) --suffix;
        size_t old_length = old_size - prefix - suffix, new_length = new_size - prefix - suffix;
        struct snag_edit *edit = malloc(sizeof(*edit) + old_length + new_length);
        if (!edit) return -1;
        *edit = (struct snag_edit){.next = history->undo, .begin = prefix,
            .before_size = old_length, .after_size = new_length,
            .before_cursor = history->cursor, .after_cursor = cursor};
        if (old_length) memcpy(edit->text, before + prefix, old_length);
        if (new_length) memcpy(edit->text + old_length, after + prefix, new_length);
        history->undo = edit;
        edits_free(history->redo);
        history->redo = NULL;
    }
    if (history->original.data) memset(history->original.data, 0, history->original.len);
    snag_buf_reset(&history->original);
    history->grouping = false;
    return 0;
}

int
snag_edit_undo(struct snag_edit_history *history, struct snag_buf *draft, size_t *cursor,
    bool redo, size_t count)
{
    if (snag_edit_end(history, draft, *cursor) < 0) return -1;
    struct snag_edit **from = redo ? &history->redo : &history->undo;
    struct snag_edit **to = redo ? &history->undo : &history->redo;
    while (count-- && *from) {
        struct snag_edit *edit = *from;
        size_t remove = redo ? edit->before_size : edit->after_size;
        size_t insert = redo ? edit->after_size : edit->before_size;
        const unsigned char *text = edit->text + (redo ? edit->before_size : 0u);
        const unsigned char *expected = edit->text + (redo ? 0u : edit->before_size);
        size_t next_cursor = redo ? edit->after_cursor : edit->before_cursor;
        if (edit->begin > draft->len || remove > draft->len - edit->begin ||
            insert > SNAG_MAX_DIRECT_PROMPT - (draft->len - remove) ||
            next_cursor > draft->len - remove + insert ||
            (remove && memcmp(draft->data + edit->begin, expected, remove)))
            return snag_errno(EINVAL);
        size_t total = draft->len - remove + insert;
        if (snag_buf_reserve(draft, total >= draft->len ? total - draft->len + 1u : 1u) < 0)
            return -1;
        memmove(draft->data + edit->begin + insert, draft->data + edit->begin + remove,
            draft->len - edit->begin - remove);
        if (insert) memcpy(draft->data + edit->begin, text, insert);
        draft->len = total;
        draft->data[total] = 0;
        *cursor = next_cursor;
        *from = edit->next;
        edit->next = *to;
        *to = edit;
    }
    return 0;
}

static json_t *
edits_save(const struct snag_edit *edit)
{
    json_t *rows = json_array();
    if (!rows) return NULL;
    for (; edit; edit = edit->next) {
        json_t *row = json_pack("[I,s#,s#,I,I]", (json_int_t)edit->begin,
            (const char *)edit->text, edit->before_size,
            (const char *)edit->text + edit->before_size, edit->after_size,
            (json_int_t)edit->before_cursor, (json_int_t)edit->after_cursor);
        if (!row || json_array_append_new(rows, row) < 0) { json_decref(rows); return NULL; }
    }
    return rows;
}

json_t *
snag_edit_history_save(const struct snag_edit_history *history)
{
    return json_pack("{s:o,s:o}", "undo", edits_save(history->undo),
        "redo", edits_save(history->redo));
}

static int
edits_load(struct snag_edit **head, const json_t *rows)
{
    if (!json_is_array(rows)) return snag_errno(EINVAL);
    struct snag_edit **tail = head;
    for (size_t i = 0u; i < json_array_size(rows); ++i) {
        const json_t *row = json_array_get(rows, i);
        uint64_t begin, before_cursor, after_cursor;
        const char *before = json_string_value(json_array_get(row, 1u));
        const char *after = json_string_value(json_array_get(row, 2u));
        size_t a = json_string_length(json_array_get(row, 1u));
        size_t b = json_string_length(json_array_get(row, 2u));
        const json_t *offset = json_array_get(row, 0u);
        const json_t *old_cursor = json_array_get(row, 3u);
        const json_t *new_cursor = json_array_get(row, 4u);
        if (!json_is_array(row) || json_array_size(row) != 5u ||
            !json_is_integer(offset) || json_integer_value(offset) < 0 ||
            !json_is_integer(old_cursor) || json_integer_value(old_cursor) < 0 ||
            !json_is_integer(new_cursor) || json_integer_value(new_cursor) < 0 ||
            !before || !after || a > SNAG_MAX_DIRECT_PROMPT || b > SNAG_MAX_DIRECT_PROMPT ||
            strlen(before) != a || strlen(after) != b ||
            !snag_utf8_valid((const unsigned char *)before, a, true) ||
            !snag_utf8_valid((const unsigned char *)after, b, true)) return snag_errno(EINVAL);
        begin = (uint64_t)json_integer_value(offset);
        before_cursor = (uint64_t)json_integer_value(old_cursor);
        after_cursor = (uint64_t)json_integer_value(new_cursor);
        if (begin > SNAG_MAX_DIRECT_PROMPT || a > SNAG_MAX_DIRECT_PROMPT - begin ||
            b > SNAG_MAX_DIRECT_PROMPT - begin || before_cursor > SNAG_MAX_DIRECT_PROMPT ||
            after_cursor > SNAG_MAX_DIRECT_PROMPT) return snag_errno(EINVAL);
        struct snag_edit *edit = malloc(sizeof(*edit) + a + b);
        if (!edit) return -1;
        *edit = (struct snag_edit){.begin = (size_t)begin, .before_size = a, .after_size = b,
            .before_cursor = (size_t)before_cursor, .after_cursor = (size_t)after_cursor};
        memcpy(edit->text, before, a);
        memcpy(edit->text + a, after, b);
        *tail = edit;
        tail = &edit->next;
    }
    return 0;
}

int
snag_edit_history_load(struct snag_edit_history *history, const json_t *value)
{
    struct snag_edit_history next = {0};
    if (!json_is_object(value) || json_object_size(value) != 2u) return snag_errno(EINVAL);
    if (edits_load(&next.undo, json_object_get(value, "undo")) < 0 ||
        edits_load(&next.redo, json_object_get(value, "redo")) < 0) {
        int saved = errno;
        snag_edit_history_free(&next);
        return snag_errno(saved);
    }
    snag_edit_history_free(history);
    *history = next;
    return 0;
}
