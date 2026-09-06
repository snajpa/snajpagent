/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_TERM_HOST_H
#define SNAJPAGENT_TERM_HOST_H
#include "wake.h"
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
struct snag_signal_mask { unsigned char unused; };
struct snag_term_host {
    unsigned long input_mode;
    bool raw_input;
    unsigned short input_high;
    bool input_skip_lf;
    INPUT_RECORD input_events[16];
    unsigned int input_count, input_next;
    char input_key[32];
    unsigned int input_key_len, input_key_at, input_repeats;
    bool input_resized;
    unsigned long output_mode[2];
};
#else
#include <signal.h>
#include <termios.h>

struct snag_signal_mask { sigset_t native; };

struct snag_term_host {
    struct termios input_mode;
    struct sigaction sigint;
    struct sigaction sigwinch;
};
#endif

int snag_term_signals_block(struct snag_signal_mask *saved);
int snag_term_signals_restore(const struct snag_signal_mask *saved);
int snag_term_signals_unblock(void);
bool snag_term_host_capable(void);
unsigned int snag_term_host_columns(void);
int snag_term_input_capture(struct snag_term_host *host);
int snag_term_input_raw(struct snag_term_host *host);
int snag_term_input_restore(struct snag_term_host *host, bool flush);
int snag_term_input_flush(struct snag_term_host *host);
/* Buffer capacity is at least four bytes; incomplete UTF-16 returns EAGAIN. */
ssize_t snag_term_input_read(struct snag_term_host *host, void *buffer, size_t size);
bool snag_term_input_resized(struct snag_term_host *host);
enum {
    SNAG_TERM_WAIT_INPUT = 1,
    SNAG_TERM_WAIT_WAKE = 2,
    SNAG_TERM_WAIT_END = 4
};
/* Bitmask above, zero timeout, or -1 error; wake-only does not consume input. */
int snag_term_input_wait(struct snag_term_host *host, snag_wake_fd wake, int timeout_ms);
int snag_term_output_open(int fd);

#endif
