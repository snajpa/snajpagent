/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_PROCESS_HOST_H
#define SNAJPAGENT_PROCESS_HOST_H
#include "wake.h"
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

struct snag_child {
    bool pty, reaped;
    int64_t exit_code;
    int signal_number;
#ifdef _WIN32
    struct snag_child_windows *native;
#else
    pid_t pid;
    int fd[3]; /* stdout, stderr, stdin */
    unsigned short rows, columns;
#endif
};

enum { SNAG_CHILD_READ = 1, SNAG_CHILD_WRITE = 2, SNAG_CHILD_END = 4, SNAG_CHILD_ERROR = 8 };
enum snag_child_signal { SNAG_CHILD_INTERRUPT, SNAG_CHILD_TERMINATE, SNAG_CHILD_KILL };
struct snag_child_event {
    struct snag_child *child;
    unsigned int stream, events, revents;
};

void snag_child_init(struct snag_child *child);
/* A child and its streams stay owned by one caller thread through cleanup. */
int snag_child_spawn(struct snag_child *child, const char *shell, const char *command,
                     const char *directory, char **environment, bool pty);
void snag_child_signal(struct snag_child *child, enum snag_child_signal signal);
/* 1 exited without reaping, 0 running, -1 error/lost ownership. */
int snag_child_exited(struct snag_child *child);
int snag_child_reap(struct snag_child *child);
void snag_child_close_stream(struct snag_child *child, unsigned int stream);
void snag_child_free(struct snag_child *child);
void snag_child_resize(struct snag_child *child);
ssize_t snag_child_read(struct snag_child *child, unsigned int stream, void *buffer, size_t size);
ssize_t snag_child_write(struct snag_child *child, const void *buffer, size_t size);
int snag_child_wait(struct snag_child_event *events, size_t count, snag_wake_fd wake, int timeout_ms);

#ifdef _WIN32
struct snag_output_broker;
/* Native pipe command with an isolated handle-inheritance owner. */
int snag_child_spawn_isolated(struct snag_child *child, const char *shell, const char *command,
                              const char *directory, char **environment);
#ifdef SNAG_LEGACY_PTY
int snag_child_spawn_legacy_pty(struct snag_child *child, const char *shell, const char *command,
                               const char *directory, char **environment);
#endif
/* Private same-executable entry point; -1 means ordinary application startup. */
int snag_output_broker_main(int argc, wchar_t **argv);
int snag_output_broker_write(struct snag_output_broker **owner, int fd, const void *bytes, size_t len,
                              int (*checkpoint)(void *), void *opaque);
/* Inheritable standard console slot, avoiding cross-process console duplication. */
/* Keep the source standard handles open until this owner is closed. */
int snag_output_broker_write_standard(struct snag_output_broker **owner, unsigned int slot,
                                       const void *bytes, size_t len,
                                       int (*checkpoint)(void *), void *opaque);
int snag_input_broker_read(struct snag_output_broker **owner, wchar_t *text, size_t capacity,
                           int (*checkpoint)(void *), void *opaque);
/* Caller keeps broker ownership alive until the read owner completes cleanup. */
void snag_output_broker_cancel(struct snag_output_broker *broker);
void snag_output_broker_close(struct snag_output_broker *broker);
#endif

#endif
