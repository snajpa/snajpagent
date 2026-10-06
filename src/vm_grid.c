/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_grid.h"
#include "unicode.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
frame_free(struct snag_vm_frame *frame)
{
    free(frame->cells);
    snag_buf_free(&frame->text);
    *frame = (struct snag_vm_frame){0};
}

void
snag_vm_grid_free(struct snag_vm_grid *grid)
{
    frame_free(&grid->front);
    frame_free(&grid->back);
    *grid = (struct snag_vm_grid){0};
}

int
snag_vm_grid_resize(struct snag_vm_grid *grid, size_t rows, size_t columns)
{
    if (!rows || !columns) return snag_errno(EINVAL);
    if (rows > SIZE_MAX / columns || rows * columns > SIZE_MAX / sizeof(struct snag_vm_cell)) {
        return snag_errno(EOVERFLOW);
    }
    if (grid->rows == rows && grid->columns == columns) return 0;
    struct snag_vm_grid next = {.rows = rows, .columns = columns,
        .ambiguous_wide = grid->ambiguous_wide};
    next.front.cells = calloc(rows * columns, sizeof(struct snag_vm_cell));
    next.back.cells = calloc(rows * columns, sizeof(struct snag_vm_cell));
    next.front.text.max = next.back.text.max = SIZE_MAX;
    if (!next.front.cells || !next.back.cells) {
        snag_vm_grid_free(&next);
        return -1;
    }
    snag_vm_grid_free(grid);
    *grid = next;
    return 0;
}

void
snag_vm_grid_begin(struct snag_vm_grid *grid)
{
    if (grid->back.cells) {
        memset(grid->back.cells, 0, grid->rows * grid->columns * sizeof(struct snag_vm_cell));
    }
    grid->back.text.len = 0u;
}

static void
clear_cell(struct snag_vm_grid *grid, size_t row, size_t column)
{
    struct snag_vm_cell *line = grid->back.cells + row * grid->columns;
    while (column && line[column].continuation) --column;
    size_t width = line[column].width ? line[column].width : 1u;
    memset(line + column, 0, width * sizeof(*line));
}

static int
put_cell(struct snag_vm_grid *grid, size_t row, size_t column, const char *text,
    size_t length, unsigned int width, unsigned int style)
{
    size_t offset = grid->back.text.len;
    if (snag_buf_append(&grid->back.text, text, length) < 0) return -1;
    for (size_t i = 0u; i < width; ++i) clear_cell(grid, row, column + i);
    struct snag_vm_cell *cell = grid->back.cells + row * grid->columns + column;
    *cell = (struct snag_vm_cell){.offset = offset, .length = length,
        .width = width, .style = style};
    for (size_t i = 1u; i < width; ++i) cell[i] = (struct snag_vm_cell){.continuation = true};
    return 0;
}

static bool
unsafe_format(uint32_t cp)
{
    return cp == 0x00adu || cp == 0x061cu || cp == 0x200bu || cp == 0x200eu || cp == 0x200fu ||
        (cp >= 0x202au && cp <= 0x202eu) || cp == 0x2060u ||
        (cp >= 0x2066u && cp <= 0x206fu) || cp == 0xfeffu || (cp >= 0xfff9u && cp <= 0xfffbu);
}

int
snag_vm_grid_text(struct snag_vm_grid *grid, size_t row, size_t column, size_t width,
    const char *text, size_t length, unsigned int style)
{
    if (!grid->back.cells || (!text && length) || style & ~15u) return snag_errno(EINVAL);
    if (row >= grid->rows || column >= grid->columns) return 0;
    if (width > grid->columns - column) width = grid->columns - column;
    size_t end = column + width, position = 0u, start = column;
    while (position < length && column < end) {
        uint32_t cp;
        size_t bytes = snag_utf8_decode((const unsigned char *)text + position,
            length - position, &cp);
        if (!bytes) cp = (unsigned char)text[position];
        if (cp == '\n') break;
        if (cp == '\t') {
            size_t spaces = 4u - ((column - start) % 4u);
            while (spaces-- && column < end) {
                if (put_cell(grid, row, column++, " ", 1u, 1u, style) < 0) return -1;
            }
            ++position;
            continue;
        }
        size_t cluster = bytes ? snag_grapheme_next((const unsigned char *)text + position,
            length - position) : 0u;
        int cells = cluster ? snag_grapheme_width((const unsigned char *)text + position,
            cluster, grid->ambiguous_wide) : -1;
        if (!bytes || cp < 0x20u || (cp >= 0x7fu && cp <= 0x9fu) ||
            unsafe_format(cp) || cells < 0) {
            char escaped[16];
            int count = snprintf(escaped, sizeof(escaped), cp <= 0xffu ? "\\x%02X" : "\\u{%X}",
                (unsigned int)cp);
            for (int i = 0; i < count && column < end; ++i) {
                if (put_cell(grid, row, column++, escaped + i, 1u, 1u, style) < 0) return -1;
            }
            position += bytes ? bytes : 1u;
            continue;
        }
        if (!cells) {
            /* A standalone combining mark needs a visible base at this boundary. */
            struct snag_buf marked = {.max = SIZE_MAX};
            int rc = snag_buf_putc(&marked, ' ');
            if (!rc) rc = snag_buf_append(&marked, text + position, cluster);
            if (!rc) rc = put_cell(grid, row, column++, (char *)marked.data,
                marked.len, 1u, style);
            snag_buf_free(&marked);
            if (rc < 0) return -1;
            position += cluster;
            continue;
        }
        if ((size_t)cells > end - column) break;
        if (put_cell(grid, row, column, text + position, cluster, (unsigned int)cells, style) < 0) {
            return -1;
        }
        position += cluster;
        column += (size_t)cells;
    }
    return 0;
}

static bool
cell_equal(const struct snag_vm_frame *a, const struct snag_vm_frame *b, size_t index)
{
    const struct snag_vm_cell *x = a->cells + index, *y = b->cells + index;
    if (x->style != y->style || x->width != y->width || x->length != y->length ||
        x->continuation != y->continuation) return false;
    return !x->length || !memcmp(a->text.data + x->offset, b->text.data + y->offset, x->length);
}

static int
set_style(struct snag_buf *out, unsigned int style)
{
    if (snag_buf_append(out, "\033[0", 3u) < 0) return -1;
    if ((style & SNAG_VM_BOLD) && snag_buf_append(out, ";1", 2u) < 0) return -1;
    if ((style & SNAG_VM_DIM) && snag_buf_append(out, ";2", 2u) < 0) return -1;
    if ((style & SNAG_VM_UNDERLINE) && snag_buf_append(out, ";4", 2u) < 0) return -1;
    if ((style & SNAG_VM_REVERSE) && snag_buf_append(out, ";7", 2u) < 0) return -1;
    return snag_buf_putc(out, 'm');
}

int
snag_vm_grid_flush(struct snag_vm_grid *grid, size_t cursor_row, size_t cursor_column,
    bool cursor_visible, int (*emit)(void *, const void *, size_t), void *opaque)
{
    if (!grid->back.cells || !emit || cursor_row >= grid->rows || cursor_column >= grid->columns) {
        return snag_errno(EINVAL);
    }
    struct snag_buf out = {.max = SIZE_MAX};
    size_t row_at = SIZE_MAX, column_at = SIZE_MAX;
    unsigned int style_at = ~0u;
    bool cursor_changed = !grid->valid || grid->cursor_row != cursor_row ||
        grid->cursor_column != cursor_column || grid->cursor_visible != cursor_visible;
    int rc = -1;
    if (!grid->valid && snag_buf_append(&out, "\033[0m\033[H\033[2J", 11u) < 0) goto out;
    for (size_t row = 0u; row < grid->rows; ++row) {
        for (size_t column = 0u; column < grid->columns; ++column) {
            size_t index = row * grid->columns + column;
            const struct snag_vm_cell *cell = grid->back.cells + index;
            if (cell->continuation ||
                (grid->valid && cell_equal(&grid->front, &grid->back, index))) {
                continue;
            }
            if (!grid->valid && !cell->length && !cell->style) continue;
            if (row_at != row || column_at != column) {
                if (snag_buf_printf(&out, "\033[%zu;%zuH", row + 1u, column + 1u) < 0) goto out;
            }
            if (style_at != cell->style && set_style(&out, cell->style) < 0) goto out;
            style_at = cell->style;
            if (snag_buf_append(&out, cell->length ? grid->back.text.data + cell->offset :
                (const unsigned char *)" ", cell->length ? cell->length : 1u) < 0) goto out;
            row_at = row;
            column_at = column + (cell->width ? cell->width : 1u);
        }
    }
    if (out.len || cursor_changed) {
        if (snag_buf_printf(&out, "\033[0m\033[%zu;%zuH\033[?25%c", cursor_row + 1u,
            cursor_column + 1u, cursor_visible ? 'h' : 'l') < 0) goto out;
        if (emit(opaque, out.data, out.len) < 0) {
            grid->valid = false;
            goto out;
        }
    }
    struct snag_vm_frame previous = grid->front;
    grid->front = grid->back;
    grid->back = previous;
    grid->valid = true;
    grid->cursor_row = cursor_row;
    grid->cursor_column = cursor_column;
    grid->cursor_visible = cursor_visible;
    rc = 0;
out:
    snag_buf_free(&out);
    return rc;
}
