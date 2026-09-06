/* SPDX-License-Identifier: GPL-2.0-only */
#include "term_host.h"
#include <errno.h>

#ifdef _WIN32
int
snag_term_signals_block(struct snag_signal_mask *saved)
{
    saved->unused = 0;
    return 0;
}

int
snag_term_signals_restore(const struct snag_signal_mask *saved)
{
    (void)saved;
    return 0;
}

int
snag_term_signals_unblock(void)
{
    return 0;
}
#else
#include <pthread.h>

static int
mask_signals(int how, const sigset_t *set, sigset_t *saved)
{
    int error = pthread_sigmask(how, set, saved);

    if (error) {
        errno = error;
        return -1;
    }
    return 0;
}

static void
terminal_signals(sigset_t *signals)
{
    sigemptyset(signals);
    sigaddset(signals, SIGINT);
    sigaddset(signals, SIGWINCH);
}

int
snag_term_signals_block(struct snag_signal_mask *saved)
{
    sigset_t signals;
    terminal_signals(&signals);
    return mask_signals(SIG_BLOCK, &signals, &saved->native);
}

int
snag_term_signals_restore(const struct snag_signal_mask *saved)
{
    return mask_signals(SIG_SETMASK, &saved->native, NULL);
}

int
snag_term_signals_unblock(void)
{
    sigset_t signals;
    terminal_signals(&signals);
    return mask_signals(SIG_UNBLOCK, &signals, NULL);
}
#endif
