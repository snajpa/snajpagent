/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_text.h"
#include "unicode.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static bool
unsafe_format(uint32_t cp)
{
    return cp == 0x00adu || cp == 0x061cu || cp == 0x200bu || cp == 0x200eu || cp == 0x200fu ||
        (cp >= 0x202au && cp <= 0x202eu) || cp == 0x2060u ||
        (cp >= 0x2066u && cp <= 0x206fu) || cp == 0xfeffu || (cp >= 0xfff9u && cp <= 0xfffbu);
}

struct snag_vm_glyph
snag_vm_glyph(const char *text, size_t length, size_t column, bool ambiguous)
{
    struct snag_vm_glyph glyph = {0};
    if (!length) return glyph;
    uint32_t cp;
    size_t bytes = snag_utf8_decode((const unsigned char *)text, length, &cp);
    if (!bytes) cp = (unsigned char)text[0];
    glyph.bytes = bytes ? bytes : 1u;
    if (cp == '\n') {
        glyph.newline = true;
        return glyph;
    }
    if (cp == '\t') {
        glyph.tab = true;
        glyph.columns = 4u - (unsigned int)(column % 4u);
        return glyph;
    }
    size_t cluster = bytes ? snag_grapheme_next((const unsigned char *)text, length) : 0u;
    int cells = cluster ? snag_grapheme_width((const unsigned char *)text, cluster, ambiguous) : -1;
    if (!bytes || cp < 0x20u || (cp >= 0x7fu && cp <= 0x9fu) || unsafe_format(cp) || cells < 0) {
        int count = snprintf(glyph.escaped, sizeof(glyph.escaped),
            cp <= 0xffu ? "\\x%02X" : "\\u{%X}", (unsigned int)cp);
        glyph.columns = (unsigned int)count;
        return glyph;
    }
    glyph.bytes = cluster;
    glyph.base = !cells;
    glyph.columns = cells ? (unsigned int)cells : 1u;
    return glyph;
}

size_t
snag_vm_text_line_start(const char *text, size_t length, size_t at)
{
    if (at > length) at = length;
    while (at && text[at - 1u] != '\n') --at;
    return at;
}

size_t
snag_vm_text_line_end(const char *text, size_t length, size_t at)
{
    if (at > length) at = length;
    while (at < length && text[at] != '\n') ++at;
    return at;
}

size_t
snag_vm_text_floor(const char *text, size_t length, size_t at)
{
    if (at > length) at = length;
    size_t cursor = snag_vm_text_line_start(text, length, at);
    while (cursor < at) {
        size_t next = cursor + snag_vm_glyph(text + cursor, length - cursor, 0u, false).bytes;
        if (next > at) break;
        cursor = next;
    }
    return cursor;
}

size_t
snag_vm_text_next(const char *text, size_t length, size_t at)
{
    if (at > length) at = length;
    return at < length ? at + snag_vm_glyph(text + at, length - at, 0u, false).bytes : length;
}

size_t
snag_vm_text_previous(const char *text, size_t length, size_t at)
{
    if (at > length) at = length;
    if (!at) return 0u;
    return snag_vm_text_floor(text, length, at - 1u);
}

size_t
snag_vm_text_column(const char *text, size_t length, size_t at, bool ambiguous)
{
    at = snag_vm_text_floor(text, length, at);
    size_t cursor = snag_vm_text_line_start(text, length, at), column = 0u;
    while (cursor < at) {
        struct snag_vm_glyph glyph = snag_vm_glyph(text + cursor, length - cursor, column, ambiguous);
        if (column > SIZE_MAX - glyph.columns) return SIZE_MAX;
        column += glyph.columns;
        cursor += glyph.bytes;
    }
    return column;
}

size_t
snag_vm_text_at_column(const char *text, size_t length, size_t start, size_t target, bool ambiguous)
{
    size_t at = snag_vm_text_line_start(text, length, start), column = 0u;
    while (at < length) {
        struct snag_vm_glyph glyph = snag_vm_glyph(text + at, length - at, column, ambiguous);
        if (glyph.newline || column >= target || glyph.columns > target - column) break;
        column += glyph.columns;
        at += glyph.bytes;
    }
    return at;
}

int
snag_vm_text_wrap(const char *text, size_t length, size_t columns, bool ambiguous,
    int (*emit)(void *, const struct snag_vm_text_row *), void *opaque)
{
    if ((!text && length) || !columns || !emit) return snag_errno(EINVAL);
    struct snag_vm_text_row row = {0};
    size_t at = 0u, column = 0u;
    while (at < length) {
        struct snag_vm_glyph glyph = snag_vm_glyph(text + at, length - at, column, ambiguous);
        if (!glyph.newline && row.columns && glyph.columns > columns - row.columns) {
            row.end = at;
            int rc = emit(opaque, &row);
            if (rc) return rc;
            row = (struct snag_vm_text_row){.begin = at, .line = row.line,
                .logical_column = column};
        }
        if (glyph.newline) {
            row.end = at;
            int rc = emit(opaque, &row);
            if (rc) return rc;
            row = (struct snag_vm_text_row){.begin = at + glyph.bytes, .line = row.line + 1u};
            column = 0u;
        } else {
            if (column > SIZE_MAX - glyph.columns) return snag_errno(EOVERFLOW);
            column += glyph.columns;
            row.clipped = glyph.columns > columns;
            row.columns += glyph.columns > columns ? columns : glyph.columns;
        }
        at += glyph.bytes;
    }
    row.end = at;
    return emit(opaque, &row);
}
