/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm_grid.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct output {
    struct snag_buf bytes;
    size_t calls;
    bool fail;
};

static int
emit(void *opaque, const void *bytes, size_t length)
{
    struct output *output = opaque;
    ++output->calls;
    assert(snag_buf_append(&output->bytes, bytes, output->fail ? length / 2u : length) == 0);
    return output->fail ? snag_errno(EIO) : 0;
}

static void
text(struct snag_vm_grid *grid, size_t row, size_t column, size_t width, const char *value)
{
    assert(snag_vm_grid_text(grid, row, column, width, value, strlen(value), 0u) == 0);
}

static void
test_terminal_glyph_publication(void)
{
    struct snag_vm_grid grid = {0};
    assert(snag_vm_grid_resize(&grid, 1u, 5u) == 0);
    static const uint32_t glyph[] = {'A', 0x301u, 0x754cu, 0x1f469u};
    static const char encoded[] = "Á界👩";
    assert(snag_vm_grid_terminal_cell(&grid, 0u, 1u, glyph, 4u, 2u,
        SNAG_VM_DIM | SNAG_VM_DOUBLE_UNDERLINE, 0x1123456u, 202u) == 0);
    struct snag_vm_cell *cell = grid.back.cells + 1u;
    assert(cell->length == sizeof(encoded) - 1u && cell->width == 2u);
    assert(cell->style == (SNAG_VM_NATIVE | SNAG_VM_DIM | SNAG_VM_DOUBLE_UNDERLINE));
    assert(cell->foreground == 0x1123456u && cell->background == 202u);
    assert(!memcmp(grid.back.text.data + cell->offset, encoded, cell->length));
    assert(cell[1].continuation);
    struct snag_vm_cell before[5];
    memcpy(before, grid.back.cells, sizeof(before));
    size_t length = grid.back.text.len;
    /* A refused reservation cannot clear either half of the existing glyph. */
    grid.back.text.max = length;
    assert(snag_vm_grid_terminal_cell(&grid, 0u, 2u, glyph, 4u, 1u,
        0u, 0u, 0u) < 0 && errno == EOVERFLOW);
    assert(grid.back.text.len == length && !memcmp(before, grid.back.cells, sizeof(before)));
    assert(!memcmp(grid.back.text.data, encoded, sizeof(encoded) - 1u));
    grid.back.text.max = SIZE_MAX;
    assert(snag_vm_grid_terminal_cell(&grid, 0u, 2u, glyph, SIZE_MAX / 4u + 1u,
        1u, 0u, 0u, 0u) < 0 && errno == EOVERFLOW);
    assert(snag_vm_grid_terminal_cell(&grid, 0u, 2u, NULL, 1u,
        1u, 0u, 0u, 0u) < 0 && errno == EINVAL);
    assert(grid.back.text.len == length && !memcmp(before, grid.back.cells, sizeof(before)));
    /* Clipped cells never inspect borrowed glyph storage. */
    assert(snag_vm_grid_terminal_cell(&grid, 0u, 4u, NULL, SIZE_MAX,
        2u, 0u, 0u, 0u) == 0);
    assert(grid.back.text.len == length && !memcmp(before, grid.back.cells, sizeof(before)));
    static const uint32_t replacement[] = {'x'};
    assert(snag_vm_grid_terminal_cell(&grid, 0u, 2u, replacement, 1u, 1u,
        0u, 0u, 0u) == 0);
    assert(!grid.back.cells[1].length && !grid.back.cells[1].continuation);
    assert(grid.back.cells[2].length == 1u && !grid.back.cells[2].continuation);
    assert(grid.back.text.data[grid.back.cells[2].offset] == 'x');
    assert(snag_vm_grid_terminal_cell(&grid, 0u, 2u, NULL, 0u, 1u,
        0u, 0u, 7u) == 0);
    assert(!grid.back.cells[2].length && grid.back.cells[2].background == 7u);
    snag_vm_grid_free(&grid);
}

int
main(void)
{
    test_terminal_glyph_publication();
    struct snag_vm_grid grid = {0};
    struct output output = {.bytes = {.max = SIZE_MAX}};
    assert(snag_vm_grid_resize(&grid, 3u, 12u) == 0);
    snag_vm_grid_begin(&grid);
    text(&grid, 0u, 0u, 12u, "é 界👩‍💻🇨🇿");
    assert(grid.back.cells[0].length == 3u && grid.back.cells[0].width == 1u);
    assert(grid.back.cells[2].width == 2u && grid.back.cells[3].continuation);
    assert(grid.back.cells[4].width == 2u && grid.back.cells[5].continuation);
    assert(grid.back.cells[6].width == 2u && grid.back.cells[7].continuation);
    text(&grid, 1u, 0u, 12u, "\033]52;c;bad\a");
    assert(snag_vm_grid_flush(&grid, 2u, 0u, true, emit, &output) == 0);
    assert(snag_buf_terminate(&output.bytes) == 0);
    assert(strstr((char *)output.bytes.data, "\\x1B]52;c;ba"));
    assert(!strstr((char *)output.bytes.data, "\033]52"));
    assert(grid.valid && output.calls == 1u);
    snag_vm_grid_begin(&grid);
    text(&grid, 0u, 0u, 12u, "é 界👩‍💻🇨🇿");
    text(&grid, 1u, 0u, 12u, "\033]52;c;bad\a");
    assert(snag_vm_grid_flush(&grid, 2u, 0u, true, emit, &output) == 0 && output.calls == 1u);
    /* Overwriting either half clears the complete prior wide cluster. */
    snag_vm_grid_begin(&grid);
    text(&grid, 0u, 0u, 12u, "界界");
    text(&grid, 0u, 1u, 12u, "x");
    assert(!grid.back.cells[0].length && grid.back.cells[1].width == 1u);
    text(&grid, 0u, 1u, 12u, "界");
    assert(grid.back.cells[1].width == 2u && grid.back.cells[2].continuation &&
        !grid.back.cells[3].length && !grid.back.cells[3].continuation);
    text(&grid, 2u, 11u, 1u, "界");
    assert(!grid.back.cells[35].length);
    /* A partial physical write keeps the prior front and forces a full redraw. */
    struct snag_vm_cell *front = grid.front.cells;
    output.fail = true;
    assert(snag_vm_grid_flush(&grid, 0u, 0u, false, emit, &output) < 0 && !grid.valid);
    assert(grid.front.cells == front);
    output.fail = false;
    output.bytes.len = 0u;
    assert(snag_vm_grid_flush(&grid, 0u, 0u, false, emit, &output) == 0 && grid.valid);
    assert(snag_buf_terminate(&output.bytes) == 0 &&
        strstr((char *)output.bytes.data, "\033[H\033[2J"));
    front = grid.front.cells;
    assert(snag_vm_grid_resize(&grid, SIZE_MAX, SIZE_MAX) < 0 && errno == EOVERFLOW);
    assert(grid.front.cells == front && grid.rows == 3u && grid.columns == 12u);
    assert(snag_vm_grid_resize(&grid, 1u, 1u) == 0 && !grid.valid);
    snag_vm_grid_begin(&grid);
    text(&grid, 0u, 0u, 1u, "界");
    assert(!grid.back.cells[0].length);
    text(&grid, 0u, 0u, 1u, "́");
    assert(grid.back.cells[0].width == 1u && grid.back.cells[0].length == 3u);
    assert(snag_vm_grid_flush(&grid, 0u, 0u, false, emit, &output) == 0);
    grid.color = true;
    assert(snag_vm_grid_resize(&grid, 62u, 320u) == 0 && grid.color);
    snag_vm_grid_begin(&grid);
    assert(snag_vm_grid_text(&grid, 30u, 160u, 160u, "status", 6u,
        SNAG_VM_BOLD | SNAG_VM_REVERSE | SNAG_VM_CYAN) == 0);
    output.bytes.len = 0u;
    assert(snag_vm_grid_flush(&grid, 61u, 319u, false, emit, &output) == 0);
    assert(snag_buf_terminate(&output.bytes) == 0);
    assert(strstr((char *)output.bytes.data, "\033[31;161H\033[0;1;7;36mstatus"));
    assert(strstr((char *)output.bytes.data, "\033[62;320H"));
    grid.color = false;
    grid.valid = false;
    snag_vm_grid_begin(&grid);
    assert(snag_vm_grid_text(&grid, 30u, 160u, 160u, "status", 6u,
        SNAG_VM_BOLD | SNAG_VM_REVERSE | SNAG_VM_CYAN) == 0);
    output.bytes.len = 0u;
    assert(snag_vm_grid_flush(&grid, 0u, 0u, false, emit, &output) == 0);
    assert(snag_buf_terminate(&output.bytes) == 0);
    assert(strstr((char *)output.bytes.data, "\033[0;1;7mstatus"));
    assert(!strstr((char *)output.bytes.data, ";36m"));
    /* Erase a contiguous native blank run once, retaining its background. */
    assert(snag_vm_grid_resize(&grid, 1u, 100u) == 0);
    snag_vm_grid_begin(&grid);
    for (size_t i = 0u; i < 100u; ++i)
        assert(snag_vm_grid_terminal_cell(&grid, 0u, i, NULL, 0u, 1u, 0u, 0u, 2u) == 0);
    snag_buf_reset(&output.bytes);
    assert(snag_vm_grid_flush(&grid, 0u, 0u, false, emit, &output) == 0);
    assert(snag_buf_terminate(&output.bytes) == 0);
    assert(strstr((char *)output.bytes.data, "\033[100X"));
    assert(!strstr((char *)output.bytes.data, "\033[1;2H"));
    snag_vm_grid_free(&grid);
    snag_buf_free(&output.bytes);
    puts("test_vm_grid: ok");
    return 0;
}
