/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_SESSION_CLIENT_H
#define SNAJPAGENT_SESSION_CLIENT_H

#include "session_host.h"

enum snag_session_client_phase {
    SNAG_CLIENT_RESERVING, SNAG_CLIENT_COMMITTING, SNAG_CLIENT_READY
};

/* Single-threaded socket/terminal pump. The caller owns terminal modes and
 * signal handling. This object owns its descriptors, never a session engine.
 * One bounded frame per direction plus an incoming control; no raw history. */
struct snag_session_client {
    int terminal, peer, target;
    enum snag_session_client_phase phase;
    struct snag_session_packet input, output, incoming, target_input, target_output;
    unsigned char geometry[4];
    struct snag_terminal_profile profile;
    uint64_t target_deadline, terminal_deadline;
    size_t terminal_offset, event_length;
    unsigned char event_data[256];
    bool output_pending, resize_pending, quitting, peer_ended, ack_pending;
    bool ansi_output;
};

/* Takes ownership on success; peer may be an already-attached initial socket
 * or -1. Descriptors become nonblocking and close-on-exec. */
int snag_session_client_init(struct snag_session_client *, int terminal, int peer);
void snag_session_client_close(struct snag_session_client *);
/* Takes an authenticated connected target on success, retaining the current
 * peer until destination commit acknowledgement and pending writes complete. */
int snag_session_client_attach(struct snag_session_client *, int target);
int snag_session_client_resize(struct snag_session_client *, unsigned int rows, unsigned int cols);
typedef int (*snag_session_connect_fn)(void *, const char *, char *, size_t);
/* On supported hosts, own the real terminal and consume peer on every outcome. An initial owner
 * socket is already attached; other peers perform reserve/commit first.
 * child is the optional owner PID created by this frontend, never a signal
 * target. Returns the explicit session exit status, or -1 with a diagnostic. */
int snag_session_client_terminal(int peer, bool attached, uint64_t child,
                                 snag_session_connect_fn, void *, char *, size_t);
/* Continue an existing SUSPEND reservation through a fresh repaint barrier.
 * First finish pending frames with step; the same reserved peer accepts its
 * in-flight keyboard input, but no newly reserved peer has that permission. */
int snag_session_client_continue(struct snag_session_client *);
/* Queue a diagnostic back to the source presentation owner after a failed
 * switch; EAGAIN retains the caller's obligation to retry this notice. */
int snag_session_client_error(struct snag_session_client *, const char *);
/* Poll once: 0 progress/timeout, 1 terminal/peer EOF, -1 transport failure.
 * event is READY after attachment, ERROR for a failed target, or a received
 * terminal control. Its payload is in event_data/event_length until the next
 * step. ERROR during switching leaves the source peer live. Physical input is
 * held during attach. */
int snag_session_client_step(struct snag_session_client *, int timeout_ms,
                             enum snag_session_message *event);

#endif
