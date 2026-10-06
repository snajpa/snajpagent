/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_CLIPBOARD_TRANSFER_H
#define SNAJPAGENT_CLIPBOARD_TRANSFER_H

#include "clipboard.h"
#include "screen_wire.h"

enum snag_copy_state {
    SNAG_COPY_CONNECTING, SNAG_COPY_SENDING, SNAG_COPY_VERIFYING, SNAG_COPY_CANCELING,
    SNAG_COPY_WRITTEN, SNAG_COPY_SEQUENCE_SENT, SNAG_COPY_CANCELED,
    SNAG_COPY_UNAVAILABLE, SNAG_COPY_FAILED, SNAG_COPY_UNCERTAIN
};
struct snag_copy_result {
    enum snag_copy_state state;
    uint64_t bytes, length;
    bool remote;
};

struct snag_clipboard_send;
/* Source remains owned by the caller and must be READY and immutable until close. */
struct snag_clipboard_send *snag_clipboard_send_open(struct snag_clipboard *, uint64_t now);
/* One complete CSI probe or checked OSC title; 0 means no output yet. */
int snag_clipboard_send_output(struct snag_clipboard_send *, uint64_t now,
    char *out, size_t capacity);
/* Complete input CSI. Unrelated/late replies return false and change no state. */
bool snag_clipboard_send_input(struct snag_clipboard_send *, const void *, size_t, uint64_t now);
void snag_clipboard_send_cancel(struct snag_clipboard_send *, uint64_t now);
void snag_clipboard_send_result(const struct snag_clipboard_send *, struct snag_copy_result *);
int snag_clipboard_send_wait(const struct snag_clipboard_send *, uint64_t now);
void snag_clipboard_send_close(struct snag_clipboard_send *);

struct snag_clipboard_receive;
/* Native backend is copied; NULL discovers it. No clipboard is read. */
struct snag_clipboard_receive *snag_clipboard_receive_open(enum snag_clipboard_policy,
    const struct snag_clipboard_backend *);
/* Body after SNAG_SCREEN_PREFIX, with no OSC delimiters. Returns reply CSI length,
 * 0 for ignored input, or -1 for allocation/output-capacity errors. */
int snag_clipboard_receive_title(struct snag_clipboard_receive *, const void *, size_t,
    uint64_t now, char *reply, size_t capacity);
void snag_clipboard_receive_poll(struct snag_clipboard_receive *, uint64_t now);
snag_wake_fd snag_clipboard_receive_fd(const struct snag_clipboard_receive *);
/* Claim a verified OSC operation for publication. Cancellation after this
 * point cannot promise to leave the terminal clipboard untouched. */
struct snag_clipboard *snag_clipboard_receive_osc(struct snag_clipboard_receive *);
void snag_clipboard_receive_osc_done(struct snag_clipboard_receive *, bool success);
void snag_clipboard_receive_close(struct snag_clipboard_receive *);

#endif
