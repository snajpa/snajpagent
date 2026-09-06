/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_TERM_HOST_H
#define SNAJPAGENT_TERM_HOST_H

#ifdef _WIN32
struct snag_signal_mask { unsigned char unused; };
struct snag_term_host {
    unsigned long input_mode;
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

#endif
