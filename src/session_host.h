/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_SESSION_HOST_H
#define SNAJPAGENT_SESSION_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A transport slice, not a limit on session input, output or retained history. */
#define SNAG_SESSION_FRAME_MAX 16384u
#define SNAG_SESSION_HEADER 8u
#define SNAG_SESSION_ENDPOINT "terminal.sock"

enum snag_session_message {
    SNAG_SESSION_RESERVE = 1, SNAG_SESSION_READY, SNAG_SESSION_COMMIT,
    SNAG_SESSION_INPUT, SNAG_SESSION_OUTPUT, SNAG_SESSION_RESIZE,
    SNAG_SESSION_DETACH, SNAG_SESSION_EXIT, SNAG_SESSION_SWITCH,
    SNAG_SESSION_ERROR, SNAG_SESSION_SUSPEND
};

struct snag_session_packet {
    unsigned char bytes[SNAG_SESSION_HEADER + SNAG_SESSION_FRAME_MAX];
    size_t used, offset;
};

struct snag_session_listener {
    int fd, dir_fd;
    uint64_t device, inode;
};

bool snag_session_host_supported(void);
/* Caller holds the session's writer lock throughout listener lifetime. Never
 * duplicate/open/close that lock here: POSIX record locks belong to a process. */
int snag_session_listener_open(struct snag_session_listener *, int dir_fd,
                               const char *dir_path, int lock_fd);
void snag_session_listener_close(struct snag_session_listener *);
/* Accepted/connected streams are same-user, nonblocking and close-on-exec.
 * Neither connecting nor probing reserves the application attachment. */
int snag_session_listener_accept(const struct snag_session_listener *);
int snag_session_endpoint_connect(int dir_fd, const char *dir_path);
int snag_session_stream_pair(int fds[2]);
/* A complete frame returns 1; partial/would-block returns 0; errors return -1.
 * Reset a received packet only after its payload has been consumed. */
int snag_session_packet_read(int fd, struct snag_session_packet *);
int snag_session_packet_write(int fd, struct snag_session_packet *);
int snag_session_packet_set(struct snag_session_packet *, enum snag_session_message,
                             const void *, size_t);
enum snag_session_message snag_session_packet_type(const struct snag_session_packet *);
size_t snag_session_packet_length(const struct snag_session_packet *);

#endif
