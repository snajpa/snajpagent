/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_VM_GRID_H
#define SNAJPAGENT_VM_GRID_H

#include "base.h"

enum snag_vm_style {
    SNAG_VM_BOLD = 1u, SNAG_VM_DIM = 2u, SNAG_VM_UNDERLINE = 4u, SNAG_VM_REVERSE = 8u,
    SNAG_VM_ITALIC = 256u,
    SNAG_VM_RED = 32u, SNAG_VM_GREEN = 48u, SNAG_VM_YELLOW = 64u,
    SNAG_VM_BLUE = 80u, SNAG_VM_MAGENTA = 96u, SNAG_VM_CYAN = 112u
};

struct snag_vm_cell {
    size_t offset, length;
    unsigned int style, width;
    bool continuation;
};

struct snag_vm_frame {
    struct snag_vm_cell *cells;
    struct snag_buf text;
};

struct snag_vm_grid {
    size_t rows, columns;
    struct snag_vm_frame front, back;
    size_t cursor_row, cursor_column;
    bool valid, cursor_visible, ambiguous_wide, color;
};

/* Zero-initialize. Resize failure preserves the old grid. Begin clears only the
 * composition frame; flush publishes it after successful full output delivery. */
int snag_vm_grid_resize(struct snag_vm_grid *, size_t rows, size_t columns);
void snag_vm_grid_free(struct snag_vm_grid *);
void snag_vm_grid_begin(struct snag_vm_grid *);
/* One clipped line, UTF-8 input with terminal controls rendered visibly.
 * Tabs use four-cell stops. Combining marks stay with their base cell. */
int snag_vm_grid_text(struct snag_vm_grid *, size_t row, size_t column, size_t width,
    const char *, size_t length, unsigned int style);
/* Keep the logical tab column when drawing a wrapped continuation. */
int snag_vm_grid_text_column(struct snag_vm_grid *, size_t row, size_t column, size_t width,
    const char *, size_t length, unsigned int style, size_t logical_column);
/* A failed emit invalidates the physical frame; the next flush repaints fully.
 * An unchanged frame and cursor emit nothing. Cursor coordinates are zero-based. */
int snag_vm_grid_flush(struct snag_vm_grid *, size_t cursor_row, size_t cursor_column,
    bool cursor_visible, int (*emit)(void *, const void *, size_t), void *opaque);

#endif
