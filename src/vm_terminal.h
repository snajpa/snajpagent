/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_VM_TERMINAL_H
#define SNAJPAGENT_VM_TERMINAL_H

#include "vm_grid.h"
#include "vm_input.h"
#include "vm_layout.h"

struct snag_vm_terminal;

/* Borrow the private channel; the child frontend takes no session-owner lease. */
struct snag_vm_terminal *snag_vm_terminal_frontend(const char *program, int channel,
    unsigned int rows, unsigned int columns,
    int (*emit)(void *, const void *, size_t), void *opaque);
int snag_vm_terminal_fd(const struct snag_vm_terminal *);
bool snag_vm_terminal_writing(const struct snag_vm_terminal *);
int snag_vm_terminal_step(struct snag_vm_terminal *);
/* Last frontend frame marker consumed from its ordered terminal byte stream. */
uint64_t snag_vm_terminal_frame(const struct snag_vm_terminal *);
/* Request follows queued input. Take returns only its matching owner checkpoint. */
int snag_vm_terminal_checkpoint(struct snag_vm_terminal *, bool force);
json_t *snag_vm_terminal_checkpoint_snapshot(struct snag_vm_terminal *);
int snag_vm_terminal_resize(struct snag_vm_terminal *, unsigned int rows, unsigned int columns);
int snag_vm_terminal_input(struct snag_vm_terminal *, const struct snag_vm_input_event *);
/* Queue only replies to transactions emitted by this frontend. */
int snag_vm_terminal_reply(struct snag_vm_terminal *, const struct snag_vm_input_event *);
size_t snag_vm_terminal_history_rows(const struct snag_vm_terminal *);
bool snag_vm_terminal_mouse(const struct snag_vm_terminal *);
bool snag_vm_terminal_continued(const struct snag_vm_terminal *, size_t offset, size_t row);
int snag_vm_terminal_draw(struct snag_vm_terminal *, struct snag_vm_grid *,
    const struct snag_vm_rectangle *, size_t offset, size_t *cursor_row, size_t *cursor_column);
/* Stop and join only the replaceable frontend, never its independent owner. */
void snag_vm_terminal_free(struct snag_vm_terminal *);

#endif
