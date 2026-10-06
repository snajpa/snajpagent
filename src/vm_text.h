/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_VM_TEXT_H
#define SNAJPAGENT_VM_TEXT_H

#include "base.h"

struct snag_vm_glyph {
    size_t bytes;
    unsigned int columns;
    bool newline, tab, base;
    char escaped[16];
};

/* One source unit under the grid's inert-control/tab/cluster display policy.
 * column is the logical line's display column, retained across soft wraps. */
struct snag_vm_glyph snag_vm_glyph(const char *, size_t, size_t column, bool ambiguous_wide);
size_t snag_vm_text_line_start(const char *, size_t length, size_t at);
size_t snag_vm_text_line_end(const char *, size_t length, size_t at);
size_t snag_vm_text_floor(const char *, size_t length, size_t at);
/* Next starts at a source-unit boundary; use floor for arbitrary byte anchors. */
size_t snag_vm_text_next(const char *, size_t length, size_t at);
size_t snag_vm_text_previous(const char *, size_t length, size_t at);
size_t snag_vm_text_column(const char *, size_t length, size_t at, bool ambiguous_wide);
/* A hit on any part of a wide/escaped/tab unit resolves to its first byte. */
size_t snag_vm_text_at_column(const char *, size_t length, size_t line_start,
    size_t column, bool ambiguous_wide);

struct snag_vm_text_row {
    size_t begin, end, line, logical_column, columns;
    bool clipped;
};

/* Whole source units wrap together. A unit wider than a tiny viewport is
 * clipped once, with the full source range retained for navigation/copy. */
int snag_vm_text_wrap(const char *, size_t length, size_t columns, bool ambiguous_wide,
    int (*emit)(void *, const struct snag_vm_text_row *), void *);

#endif
