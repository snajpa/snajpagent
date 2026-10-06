/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_VM_EDITOR_H
#define SNAJPAGENT_VM_EDITOR_H

#include "vm_input.h"
#include "vm_selection.h"

struct snag_vm_buffer;
struct snag_vm_undo;

/* History belongs to a conversation draft; the unnamed register belongs to the
 * workspace and can be pasted into any composer. Neither enters model context. */
struct snag_vm_editor {
    struct snag_vm_undo *undo, *redo;
    struct snag_buf original;
    size_t original_cursor, count, operator_count, column;
    size_t completion_begin, completion_prefix, completion_end;
    unsigned int operator, prefix;
    bool grouping, column_valid, column_display;
    bool completing;
};

struct snag_vm_motion {
    size_t at;
    bool valid, inclusive, lines;
};
/* Shared pure movement for a composer or one projected logical text field. */
struct snag_vm_motion snag_vm_editor_motion(struct snag_vm_editor *, const char *,
    size_t length, size_t at, unsigned int key, size_t count, bool counted,
    bool prefixed, bool insert, size_t columns, size_t rows, size_t top);

enum snag_vm_edit_result {
    SNAG_VM_EDIT_ERROR = -1, SNAG_VM_EDIT_UNUSED, SNAG_VM_EDIT_DONE,
    SNAG_VM_EDIT_INSERT, SNAG_VM_EDIT_NORMAL, SNAG_VM_EDIT_SUBMIT, SNAG_VM_EDIT_CENTER,
    SNAG_VM_EDIT_YANK
};

void snag_vm_editor_reset(struct snag_vm_editor *);
int snag_vm_editor_begin(struct snag_vm_buffer *);
int snag_vm_editor_end(struct snag_vm_buffer *);
int snag_vm_editor_replace(struct snag_vm_buffer *, size_t begin, size_t end,
    const void *text, size_t length);
int snag_vm_editor_undo(struct snag_vm_buffer *, bool redo, size_t count);
void snag_vm_editor_normal(struct snag_vm_buffer *, bool from_insert);
enum snag_vm_edit_result snag_vm_editor_key(struct snag_vm_buffer *,
    struct snag_vm_register *, const struct snag_vm_input_event *, bool insert,
    size_t columns, size_t rows, size_t top);

#endif
