/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_SESSION_HOST_H
#define SNAJPAGENT_SESSION_HOST_H

#include "base.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A transport slice, not a limit on session input, output or retained history. */
#define SNAG_SESSION_FRAME_MAX 16384u
#define SNAG_SESSION_HEADER 8u
#define SNAG_SESSION_ENDPOINT "terminal.sock"
#define SNAG_SESSION_COMMIT_BYTES (4u + 2u * SNAG_TERMINAL_NAME_BYTES)

/* OUTPUT_ACK carries a two-byte little-endian cumulative write offset within
 * the current OUTPUT frame. QUITTING announces a hard escape, completed by EOF. */
enum snag_session_message {
    SNAG_SESSION_RESERVE = 1, SNAG_SESSION_READY, SNAG_SESSION_COMMIT,
    SNAG_SESSION_INPUT, SNAG_SESSION_OUTPUT, SNAG_SESSION_RESIZE,
    SNAG_SESSION_DETACH, SNAG_SESSION_EXIT, SNAG_SESSION_SWITCH,
    SNAG_SESSION_ERROR, SNAG_SESSION_SUSPEND, SNAG_SESSION_QUITTING, SNAG_SESSION_OUTPUT_ACK,
    SNAG_SESSION_STATUS
};

struct snag_session_packet {
    unsigned char bytes[SNAG_SESSION_HEADER + SNAG_SESSION_FRAME_MAX];
    size_t used, offset;
};

struct snag_session_listener {
    int fd, dir_fd;
    uint64_t device, inode;
};

struct snag_session_process {
    int master, slave, peer;
    uint64_t child;
    struct snag_terminal_profile profile;
};

bool snag_session_host_supported(void);
/* Call before creating any threads, with all three standard descriptors on
 * the same terminal. Returns 1 in the replaceable frontend (peer and child
 * set), 0 in its surviving owner (private master/slave/peer set), or -1 before
 * forking on failure. The owner has a separate session/controlling terminal;
 * frontend EOF never closes its private PTY. Setup failure in the child exits
 * with status 125. This does not acquire a session lock or publish an endpoint. */
int snag_session_process_start(struct snag_session_process *);
/* Close owned descriptors only; never signal the child or any process group. */
void snag_session_process_close(struct snag_session_process *);
/* Redraw an external foreground job on this owner's private controlling PTY. */
int snag_session_process_redraw(const struct snag_session_process *);
/* Caller holds the session's writer lock throughout listener lifetime. Never
 * duplicate/open/close that lock here: POSIX record locks belong to a process. */
int snag_session_listener_open(struct snag_session_listener *, int dir_fd,
                               const char *dir_path, int lock_fd);
void snag_session_listener_close(struct snag_session_listener *);
/* Checked private endpoints return nonblocking, close-on-exec streams. On
 * message-credential hosts, callers must finish peer_verify before admitting
 * the first frame. Connecting or probing never reserves an attachment. */
int snag_session_listener_accept(const struct snag_session_listener *);
int snag_session_endpoint_connect(int dir_fd, const char *dir_path);
/* Read-only status: 1 attached, 0 detached, -1 unavailable. Never reserves a
 * terminal. STATUS has an empty request and a one-byte boolean response. */
int snag_session_endpoint_status(int dir_fd, const char *dir_path);
int snag_session_stream_pair(int fds[2]);
/* 1 same effective user, 0 awaiting first kernel credential, -1 refusal/error.
 * Does not consume frame bytes. Cache success only for this connection. */
int snag_session_peer_verify(int fd);
/* A complete frame returns 1; partial/would-block returns 0; errors return -1.
 * Reset a received packet only after its payload has been consumed. */
int snag_session_packet_read(int fd, struct snag_session_packet *);
int snag_session_packet_write(int fd, struct snag_session_packet *);
int snag_session_packet_set(struct snag_session_packet *, enum snag_session_message,
                             const void *, size_t);
int snag_session_commit_set(struct snag_session_packet *, const unsigned char geometry[4],
                             const struct snag_terminal_profile *);
enum snag_session_message snag_session_packet_type(const struct snag_session_packet *);
size_t snag_session_packet_length(const struct snag_session_packet *);

#endif
