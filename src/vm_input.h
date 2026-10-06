/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_VM_INPUT_H
#define SNAJPAGENT_VM_INPUT_H

#include "base.h"

enum snag_vm_input_kind {
    SNAG_VM_KEY, SNAG_VM_TEXT, SNAG_VM_PASTE_BEGIN, SNAG_VM_PASTE_TEXT,
    SNAG_VM_PASTE_END, SNAG_VM_MOUSE, SNAG_VM_FOCUS, SNAG_VM_TERMINAL_REPLY
};

enum snag_vm_key {
    SNAG_VM_KEY_UNKNOWN = 256, SNAG_VM_KEY_ESCAPE, SNAG_VM_KEY_ENTER,
    SNAG_VM_KEY_TAB, SNAG_VM_KEY_BACKSPACE, SNAG_VM_KEY_UP, SNAG_VM_KEY_DOWN,
    SNAG_VM_KEY_LEFT, SNAG_VM_KEY_RIGHT, SNAG_VM_KEY_HOME, SNAG_VM_KEY_END,
    SNAG_VM_KEY_PAGE_UP, SNAG_VM_KEY_PAGE_DOWN, SNAG_VM_KEY_INSERT, SNAG_VM_KEY_DELETE,
    SNAG_VM_KEY_F1, SNAG_VM_KEY_F2, SNAG_VM_KEY_F3, SNAG_VM_KEY_F4
};

enum snag_vm_modifier { SNAG_VM_SHIFT = 1u, SNAG_VM_ALT = 2u, SNAG_VM_CTRL = 4u };

struct snag_vm_input_event {
    enum snag_vm_input_kind kind;
    unsigned int key, modifiers;
    const unsigned char *text;
    size_t length;
    unsigned int row, column, button;
    bool release, focused;
};

struct snag_vm_input {
    /* Private clipboard replies contain eight uint32 fields plus framing. */
    unsigned char sequence[128], utf8[4];
    size_t sequence_length, utf8_length, utf8_expected, paste_match;
    unsigned int utf8_modifiers;
    uint64_t sequence_since;
    bool discard, string, string_escape, paste;
};

/* Zero-initialize. Text is borrowed for the callback duration. Normal text is
 * valid UTF-8; malformed bytes become U+FFFD. Paste bytes remain literal and
 * streamed between begin/end events, for one atomic composer edit. */
typedef int (*snag_vm_input_emit)(void *, const struct snag_vm_input_event *);
int snag_vm_input_feed(struct snag_vm_input *, const void *, size_t, uint64_t now_ms,
    snag_vm_input_emit, void *);
/* Call even without another key: lone Esc resolves after 35 ms. Incomplete
 * control sequences expire after 1 s; bracketed paste has no inter-byte timer. */
int snag_vm_input_expire(struct snag_vm_input *, uint64_t now_ms, snag_vm_input_emit, void *);
int snag_vm_input_wait_ms(const struct snag_vm_input *, uint64_t now_ms);

#endif
