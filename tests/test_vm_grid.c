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

int
main(void)
{
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
    snag_vm_grid_free(&grid);
    snag_buf_free(&output.bytes);
    puts("test_vm_grid: ok");
    return 0;
}
