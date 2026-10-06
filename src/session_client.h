/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_SESSION_CLIENT_H
#define SNAJPAGENT_SESSION_CLIENT_H

#include "session_host.h"

#include <signal.h>

enum snag_session_client_phase {
    SNAG_CLIENT_RESERVING, SNAG_CLIENT_OFFERING, SNAG_CLIENT_COMMITTING, SNAG_CLIENT_READY
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
    unsigned char voice_offer[SNAG_SESSION_VOICE_BYTES];
    bool output_pending, resize_pending, quitting, peer_ended, ack_pending;
    bool ansi_output, target_verified, peer_draining;
    bool voice_offered, voice_released, input_blocked;
};

/* Takes ownership on success; peer may be an already-attached initial socket
 * or -1. Descriptors become nonblocking and close-on-exec. */
int snag_session_client_init(struct snag_session_client *, int terminal, int peer);
void snag_session_client_close(struct snag_session_client *);
/* Takes a connected private endpoint, verifies its peer before sending the
 * profile, and retains the source until commit acknowledgement and source drain. */
int snag_session_client_attach(struct snag_session_client *, int target);
int snag_session_client_resize(struct snag_session_client *, unsigned int rows, unsigned int cols);
typedef int (*snag_session_connect_fn)(void *, const char *, char *selected, char *, size_t);
struct snag_term_host;
struct snag_session_typeahead {
    struct snag_buf bytes;
    unsigned char command[SNAG_SESSION_COMMAND_BYTES];
    char session[SNAG_ID_HEX_LEN + 1u];
    int signal;
    const volatile sig_atomic_t *cancelled;
    struct snag_term_host *suspend_terminal;
};
/* On supported hosts, own the real terminal and consume peer on every outcome. An initial owner
 * socket is already attached; other peers perform reserve/commit first.
 * An optional command reference is sent once after BOUND, before keyboard input.
 * Its acknowledgement gates following input; this call never retries the reference.
 * child is the optional owner PID created by this frontend, never a signal
 * target. Optional typeahead precedes new terminal input after BOUND; complete
 * frames consume its prefix, leaving unsent bytes on return. Its session follows
 * successful switches; signal reports a caught signal separately from an owner
 * exit code. An optional outer cancellation flag spans handler installation.
 * suspend_terminal supplies cooked modes when the caller hands over raw input.
 * Returns the explicit session exit status, or -1 with a diagnostic. */
int snag_session_client_terminal(int peer, bool attached, uint64_t child,
    snag_session_connect_fn, void *, struct snag_session_typeahead *, char *, size_t);
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
 * step. ERROR before destination acceptance leaves the source peer live. Input
 * reaches the source until destination acceptance, then waits for queued source
 * frames to drain before entering the destination. BOUND is queued before new
 * input to publish that completed frontend transition. After write-side shutdown,
 * a transport failure ends the connection rather than rolling back to the source.
 * Initial attach has no source. */
int snag_session_client_step(struct snag_session_client *, int timeout_ms,
                             enum snag_session_message *event);

#endif
