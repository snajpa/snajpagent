/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_grid.h"
#include "vm_text.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct rows {
    struct snag_vm_text_row items[64];
    size_t count;
};

static int
collect(void *opaque, const struct snag_vm_text_row *row)
{
    struct rows *rows = opaque;
    assert(rows->count < sizeof(rows->items) / sizeof(rows->items[0]));
    rows->items[rows->count++] = *row;
    return 0;
}

int
main(void)
{
    const char *text = "é界👩‍💻\tA\ńZ";
    size_t length = strlen(text);
    const size_t boundaries[] = {0u, 3u, 6u, 17u, 18u, 19u, 20u, 22u, 23u};
    assert(length == 23u);
    for (size_t i = 0u; i + 1u < sizeof(boundaries) / sizeof(boundaries[0]); ++i) {
        size_t next = boundaries[i + 1u];
        assert(snag_vm_text_next(text, length, boundaries[i]) == next);
        assert(snag_vm_text_previous(text, length, next) == boundaries[i]);
        for (size_t at = boundaries[i]; at < next; ++at)
            assert(snag_vm_text_floor(text, length, at) == boundaries[i]);
    }
    assert(snag_vm_text_floor(text, length, SIZE_MAX) == length);
    assert(snag_vm_text_next(text, length, length) == length);
    assert(snag_vm_text_previous(text, length, 0u) == 0u);
    assert(snag_vm_text_line_start(text, length, 22u) == 20u);
    assert(snag_vm_text_line_end(text, length, 3u) == 19u);
    assert(snag_vm_text_column(text, length, 18u, false) == 8u);
    const size_t hits[] = {0u, 3u, 3u, 6u, 6u, 17u, 17u, 17u, 18u, 19u};
    for (size_t col = 0u; col < sizeof(hits) / sizeof(hits[0]); ++col)
        assert(snag_vm_text_at_column(text, length, 0u, col, false) == hits[col]);
    assert(snag_vm_text_at_column(text, length, 20u, SIZE_MAX, false) == length);
    struct rows rows = {0};
    assert(snag_vm_text_wrap(text, length, 5u, false, collect, &rows) == 0);
    assert(rows.count == 3u && rows.items[0].begin == 0u && rows.items[0].end == 17u &&
        rows.items[0].columns == 5u && rows.items[1].begin == 17u &&
        rows.items[1].end == 19u && rows.items[1].logical_column == 5u &&
        rows.items[1].columns == 4u && rows.items[2].line == 1u);
    for (size_t width = 1u; width < 20u; ++width) {
        rows = (struct rows){0};
        assert(snag_vm_text_wrap(text, length, width, false, collect, &rows) == 0);
        size_t end = 0u;
        for (size_t i = 0u; i < rows.count; ++i) {
            const struct snag_vm_text_row *row = rows.items + i;
            assert(row->begin == end || (row->begin == end + 1u && text[end] == '\n'));
            assert(row->end >= row->begin && row->columns <= width);
            assert(snag_vm_text_floor(text, length, row->begin) == row->begin);
            assert(snag_vm_text_floor(text, length, row->end) == row->end);
            end = row->end;
        }
        assert(end == length);
    }
    rows = (struct rows){0};
    assert(snag_vm_text_wrap("\n\n", 2u, 1u, false, collect, &rows) == 0 && rows.count == 3u);
    rows = (struct rows){0};
    assert(snag_vm_text_wrap(NULL, 0u, 1u, false, collect, &rows) == 0 && rows.count == 1u);
    assert(snag_vm_text_wrap("", 0u, 0u, false, collect, &rows) < 0);
    struct snag_vm_grid grid = {0};
    assert(snag_vm_grid_resize(&grid, 3u, 5u) == 0);
    snag_vm_grid_begin(&grid);
    assert(snag_vm_grid_text_column(&grid, 0u, 0u, 5u, "\tz", 2u, 0u, 5u) == 0);
    const struct snag_vm_cell *z = &grid.back.cells[3];
    assert(z->length == 1u && grid.back.text.data[z->offset] == 'z');
    struct snag_vm_glyph glyph = snag_vm_glyph("\033", 1u, 0u, false);
    assert(glyph.bytes == 1u && glyph.columns == 4u && !strcmp(glyph.escaped, "\\x1B"));
    glyph = snag_vm_glyph("\r\n", 2u, 0u, false);
    assert(glyph.bytes == 1u && glyph.columns == 4u);
    glyph = snag_vm_glyph("\xff", 1u, 0u, false);
    assert(glyph.bytes == 1u && glyph.columns == 4u && !strcmp(glyph.escaped, "\\xFF"));
    glyph = snag_vm_glyph("\u202e", 3u, 0u, false);
    assert(glyph.bytes == 3u && !strcmp(glyph.escaped, "\\u{202E}"));
    glyph = snag_vm_glyph("·", 2u, 0u, true);
    assert(glyph.bytes == 2u && glyph.columns == 2u);
    glyph = snag_vm_glyph("·", 2u, 0u, false);
    assert(glyph.columns == 1u);
    snag_vm_grid_free(&grid);
    puts("test_vm_text: ok");
    return 0;
}
