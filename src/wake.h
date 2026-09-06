/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_WAKE_H
#define SNAJPAGENT_WAKE_H

#include <stdint.h>

#ifdef _WIN32
typedef uintptr_t snag_wake_fd;
#else
typedef int snag_wake_fd;
#endif
#define SNAG_WAKE_INVALID ((snag_wake_fd)-1)

int snag_wakeup_create(snag_wake_fd pair[2]);
void snag_wakeup_close(snag_wake_fd pair[2]);
void snag_wakeup_send(snag_wake_fd writer);
void snag_wakeup_drain(snag_wake_fd reader);
/* 1 ready, 0 timeout, -1 error. An absent source permits only finite waits. */
int snag_wakeup_wait(snag_wake_fd reader, int timeout_ms);

#endif
