/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_navigation.h"
#include "unicode.h"
#include "vm_source.h"
#include "vm_text.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum target_kind { TARGET_NONE, TARGET_LINE, TARGET_GLYPH, TARGET_WORD };
struct snag_vm_navigation {
    struct snag_vm_navigation_request request;
    enum target_kind target_kind;
    uint64_t target, target_line, remaining;
    bool fixed, found_start, done, retry, any, field, line_open, text_found;
    struct snag_vm_anchor result, first, previous, line_first, line_last, line_text, column_at;
    struct snag_vm_anchor unit, field_end;
    struct snag_grapheme_state boundary;
    struct snag_grapheme_cells cells;
    uint64_t line, glyph, words, start_glyph, line_glyph, source_end;
    size_t column;
    unsigned int previous_kind, unit_kind, unit_columns, stream;
    bool previous_newline, unit_pending, unit_newline, unit_start;
    char key[160], handle[SNAG_ID_HEX_LEN + 1u];
};

struct snag_vm_navigation *
snag_vm_navigation_open(const struct snag_vm_navigation_request *request)
{
    if (!request || request->kind <= SNAG_VM_NAV_NONE || request->kind > SNAG_VM_NAV_LINE_END ||
        !request->count) { errno = EINVAL; return NULL; }
    struct snag_vm_navigation *nav = calloc(1u, sizeof(*nav));
    if (!nav) return NULL;
    nav->request = *request;
    nav->remaining = request->count;
    if (request->kind == SNAG_VM_NAV_LINE) {
        nav->target_kind = TARGET_LINE;
        nav->target = request->count - 1u;
    }
    return nav;
}

void
snag_vm_navigation_close(struct snag_vm_navigation *nav)
{
    free(nav);
}

static uint64_t
plus(uint64_t a, uint64_t b)
{
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}

static bool
first_text(const struct snag_vm_navigation *nav)
{
    return nav->request.kind == SNAG_VM_NAV_LINE || nav->request.kind == SNAG_VM_NAV_LAST ||
        nav->request.kind == SNAG_VM_NAV_LINE_FIRST;
}

static void
start(struct snag_vm_navigation *nav)
{
    nav->found_start = true;
    nav->start_glyph = nav->glyph;
    uint64_t count = nav->request.count;
    switch (nav->request.kind) {
    case SNAG_VM_NAV_UP:
    case SNAG_VM_NAV_DOWN:
    case SNAG_VM_NAV_LINE_START:
    case SNAG_VM_NAV_LINE_FIRST:
    case SNAG_VM_NAV_LINE_END:
        nav->target_kind = TARGET_LINE;
        if (nav->request.kind == SNAG_VM_NAV_UP)
            nav->target = count > nav->line ? 0u : nav->line - count;
        else if (nav->request.kind == SNAG_VM_NAV_DOWN) nav->target = plus(nav->line, count);
        else if (nav->request.kind == SNAG_VM_NAV_LINE_END) {
            nav->target = plus(nav->line, count - 1u);
            nav->request.column = SIZE_MAX;
            nav->column_at = nav->line_last;
        } else {
            nav->target = nav->line;
            nav->request.column = 0u;
            nav->column_at = nav->line_first;
        }
        nav->retry = nav->target < nav->line;
        break;
    case SNAG_VM_NAV_LEFT:
    case SNAG_VM_NAV_RIGHT:
        nav->target_kind = TARGET_GLYPH;
        nav->target_line = nav->line;
        nav->target = nav->request.kind == SNAG_VM_NAV_RIGHT ? plus(nav->glyph, count) :
            count > nav->glyph - nav->line_glyph ? nav->line_glyph : nav->glyph - count;
        nav->retry = nav->target < nav->glyph;
        break;
    case SNAG_VM_NAV_WORD_PREVIOUS:
        if (!nav->words) { nav->result = nav->first; nav->done = true; }
        else {
            nav->target_kind = TARGET_WORD;
            nav->target = count > nav->words ? 0u : nav->words - count;
            nav->retry = true;
        }
        break;
    default:
        break;
    }
}

static void
word_end(struct snag_vm_navigation *nav)
{
    if (nav->request.kind != SNAG_VM_NAV_WORD_END || !nav->found_start ||
        !nav->glyph || nav->glyph - 1u <= nav->start_glyph) return;
    if (!--nav->remaining) { nav->result = nav->previous; nav->done = true; }
}

static void
line_end(struct snag_vm_navigation *nav, const struct snag_vm_anchor *newline)
{
    if (!nav->line_open) return;
    if (nav->target_kind == TARGET_LINE || nav->request.kind == SNAG_VM_NAV_LAST) {
        struct snag_vm_anchor at = first_text(nav) ?
            nav->text_found ? nav->line_text : nav->line_last : nav->column_at;
        nav->result = at;
        if (nav->target_kind == TARGET_LINE && nav->line >= nav->target) nav->done = true;
    } else if (nav->target_kind == TARGET_GLYPH && nav->line == nav->target_line) {
        nav->result = nav->request.operate ? *newline : nav->line_last;
        nav->done = true;
    }
    nav->line_open = nav->text_found = false;
    nav->column = 0u;
    ++nav->line;
}

static int
unit(struct snag_vm_navigation *nav)
{
    if (!nav->unit_pending || nav->done || nav->retry) return 0;
    nav->unit_pending = false;
    const struct snag_vm_anchor *at = &nav->unit;
    if (!nav->any) { nav->first = *at; nav->any = true; }
    if (!nav->line_open) {
        nav->line_open = true;
        nav->line_first = nav->line_last = nav->line_text = nav->column_at = *at;
        nav->line_glyph = nav->glyph;
    }
    if (nav->previous_kind && nav->unit_kind != nav->previous_kind) word_end(nav);
    if (nav->done) return 0;
    if (nav->unit_start && !nav->found_start && !nav->fixed) start(nav);
    if (nav->done || nav->retry) return 0;
    bool word = (nav->unit_kind && nav->unit_kind != nav->previous_kind) ||
        (nav->unit_newline && (nav->previous_newline || !nav->glyph));
    if (word) {
        if (nav->target_kind == TARGET_WORD && nav->words == nav->target) {
            nav->result = *at;
            nav->done = true;
        } else if (nav->request.kind == SNAG_VM_NAV_WORD_NEXT && nav->found_start &&
            nav->glyph > nav->start_glyph && !--nav->remaining) {
            nav->result = *at;
            nav->done = true;
        }
        ++nav->words;
    }
    if (nav->target_kind == TARGET_GLYPH && nav->glyph == nav->target) {
        nav->result = nav->unit_newline && !nav->request.operate ? nav->line_last : *at;
        nav->done = true;
    }
    if (nav->done) return 0;
    if (!nav->unit_newline) {
        if (nav->column <= nav->request.column) nav->column_at = *at;
        nav->line_last = *at;
        if (!nav->text_found && nav->unit_kind) {
            nav->line_text = *at;
            nav->text_found = true;
        }
        int width = nav->unit_columns ? (int)nav->unit_columns :
            snag_grapheme_cells_width(&nav->cells);
        if (width < 0) return -1;
        if (!width) width = 1;
        if ((size_t)width > SIZE_MAX - nav->column) return snag_errno(EOVERFLOW);
        nav->column += (size_t)width;
    } else line_end(nav, at);
    if (nav->request.kind == SNAG_VM_NAV_WORD_NEXT || nav->request.kind == SNAG_VM_NAV_WORD_END)
        nav->result = nav->unit_newline && !nav->request.operate ? nav->line_last : *at;
    nav->previous = *at;
    nav->previous_kind = nav->unit_kind;
    nav->previous_newline = nav->unit_newline;
    ++nav->glyph;
    nav->cells = (struct snag_grapheme_cells){0};
    nav->unit_columns = 0u;
    nav->unit_start = false;
    return 0;
}

static bool
hit(const struct snag_vm_navigation *nav, const json_t *block, bool heading,
    uint64_t begin, uint64_t end)
{
    const struct snag_vm_anchor *start = &nav->request.start;
    return start->heading == heading && !strcmp(start->key, snag_json_string(block, "key")) &&
        start->byte >= begin && (start->byte < end || (begin == end && start->byte == begin));
}

static int
field(struct snag_vm_navigation *nav, const json_t *block, bool heading,
    bool (*cancel)(void *), void *opaque)
{
    const char *text = snag_json_string(block, heading ? "label" : "text");
    if (!text || !*text) return 0;
    size_t length = strlen(text);
    if (heading && length && text[length - 1u] == '\n') --length;
    for (size_t at = 0u; at < length && !nav->done && !nav->retry;) {
        if (cancel && cancel(opaque)) return snag_errno(ECANCELED);
        uint32_t cp;
        size_t bytes = snag_utf8_decode((const unsigned char *)text + at, length - at, &cp);
        if (!bytes) return snag_errno(EILSEQ);
        uint64_t begin = heading ? at : snag_vm_source_position(block, at, true);
        uint64_t end = heading ? at + bytes : snag_vm_source_position(block, at + bytes, true);
        if (!heading && begin == end) {
            if (unit(nav) < 0) return -1;
            if (nav->done || nav->retry) break;
            size_t shown = at, width = 0u;
            do {
                struct snag_vm_glyph glyph = snag_vm_glyph(text + shown, length - shown,
                    nav->column + width, false);
                if (glyph.columns > UINT_MAX - width) return snag_errno(EOVERFLOW);
                width += glyph.columns;
                shown += glyph.bytes;
            } while (shown < length && snag_vm_source_position(block, shown, true) == begin);
            nav->unit = (struct snag_vm_anchor){
                .seq = (uint64_t)json_integer_value(json_object_get(block, "seq")),
                .byte = begin, .order = snag_vm_search_order(block)};
            (void)snprintf(nav->unit.key, sizeof(nav->unit.key), "%s",
                snag_json_string(block, "key"));
            nav->unit_columns = (unsigned int)width;
            nav->unit_kind = 2u;
            nav->unit_newline = false;
            nav->unit_pending = true;
            nav->unit_start = hit(nav, block, false, begin,
                snag_vm_source_position(block, shown, true));
            nav->boundary = (struct snag_grapheme_state){0};
            at = shown;
            continue;
        }
        if (snag_grapheme_feed(&nav->boundary, cp)) {
            if (unit(nav) < 0) return -1;
            if (nav->done || nav->retry) break;
            nav->unit = (struct snag_vm_anchor){
                .seq = (uint64_t)json_integer_value(json_object_get(block, "seq")),
                .byte = heading ? at : snag_vm_source_position(block, at, true),
                .order = snag_vm_search_order(block), .heading = heading};
            (void)snprintf(nav->unit.key, sizeof(nav->unit.key), "%s",
                snag_json_string(block, "key"));
            nav->unit_kind = snag_unicode_word_class(cp);
            nav->unit_newline = cp == '\n';
            nav->unit_pending = true;
        }
        nav->unit_start = nav->unit_start || hit(nav, block, heading, begin, end);
        struct snag_vm_glyph glyph = snag_vm_glyph(text + at, bytes, nav->column, false);
        if (glyph.escaped[0] || glyph.tab) nav->unit_columns = glyph.columns;
        else if (!glyph.newline && snag_grapheme_cells_feed(&nav->cells, cp, false) < 0) return -1;
        at += bytes;
    }
    nav->field_end = (struct snag_vm_anchor){
        .seq = (uint64_t)json_integer_value(json_object_get(block, "seq")),
        .byte = heading ? length : snag_vm_source_position(block, length, true),
        .order = snag_vm_search_order(block), .heading = heading};
    (void)snprintf(nav->field_end.key, sizeof(nav->field_end.key), "%s",
        snag_json_string(block, "key"));
    return 0;
}

static int
finish_field(struct snag_vm_navigation *nav)
{
    if (unit(nav) < 0) return -1;
    if (nav->done || nav->retry) return 0;
    if (nav->line_open) {
        nav->unit = nav->field_end;
        nav->unit_pending = nav->unit_newline = true;
        nav->unit_kind = 0u;
        nav->unit_start = !strcmp(nav->request.start.key, nav->field_end.key) &&
            nav->request.start.heading == nav->field_end.heading &&
            nav->request.start.byte == nav->field_end.byte;
        if (unit(nav) < 0) return -1;
    }
    nav->boundary = (struct snag_grapheme_state){0};
    return 0;
}

int
snag_vm_navigation_block(struct snag_vm_navigation *nav, const json_t *block,
    bool (*cancel)(void *), void *opaque)
{
    if (nav->done || nav->retry) return 1;
    const char *key = snag_json_string(block, "key"), *handle = snag_json_string(block, "handle");
    uint64_t begin, end;
    if (!key || strlen(key) >= sizeof(nav->key) ||
        snag_json_integer_u64(block, "source_begin", &begin) < 0 ||
        snag_json_integer_u64(block, "source_end", &end) < 0) return snag_errno(EINVAL);
    unsigned int stream = (unsigned int)json_integer_value(json_object_get(block, "stream"));
    bool contiguous = nav->field && begin == nav->source_end &&
        (handle ? !strcmp(handle, nav->handle) && stream == nav->stream : !strcmp(key, nav->key));
    if (!contiguous) {
        if (finish_field(nav) < 0) return -1;
        if (nav->done || nav->retry) return 1;
        if (field(nav, block, true, cancel, opaque) < 0 || finish_field(nav) < 0) return -1;
    }
    if (!nav->done && !nav->retry && field(nav, block, false, cancel, opaque) < 0) return -1;
    (void)snprintf(nav->key, sizeof(nav->key), "%s", key);
    (void)snprintf(nav->handle, sizeof(nav->handle), "%s", handle ? handle : "");
    nav->source_end = end;
    nav->stream = stream;
    nav->field = true;
    return nav->done || nav->retry ? 1 : 0;
}

int
snag_vm_navigation_finish(struct snag_vm_navigation *nav, struct snag_vm_anchor *out)
{
    if (finish_field(nav) < 0) return -1;
    if (nav->retry) {
        struct snag_vm_navigation_request request = nav->request;
        enum target_kind kind = nav->target_kind;
        uint64_t target = nav->target, line = nav->target_line;
        memset(nav, 0, sizeof(*nav));
        nav->request = request;
        nav->target_kind = kind;
        nav->target = target;
        nav->target_line = line;
        nav->fixed = nav->found_start = true;
        return 1;
    }
    if (!nav->any || (!nav->found_start && nav->request.kind != SNAG_VM_NAV_LINE &&
        nav->request.kind != SNAG_VM_NAV_LAST)) return snag_errno(ESTALE);
    *out = nav->result;
    return 0;
}
