/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_SESSION_RELAY_H
#define SNAJPAGENT_SESSION_RELAY_H

#include "session_host.h"

enum snag_session_phase {
    SNAG_SESSION_WAIT_RESERVE, SNAG_SESSION_RESERVED,
    SNAG_SESSION_REPAINT, SNAG_SESSION_ACCEPTED, SNAG_SESSION_ATTACHED,
    SNAG_SESSION_RELEASING
};

/* One relay thread owns all fields and descriptors. Other threads communicate
 * with that owner; they never call these functions concurrently. No raw history
 * is retained: detached output is drained and the next UI attachment repaints
 * from semantic history after an explicit generation-checked barrier. */
struct snag_session_relay {
    int master, peer, reject;
    enum snag_session_phase phase;
    uint64_t generation, handshake_deadline, output_deadline, input_deadline;
    uint64_t reject_deadline, output_ack_deadline;
    struct snag_session_packet input, output, rejection;
    struct snag_terminal_profile profile;
    size_t input_offset, output_length, output_acknowledged;
    unsigned char event_data[256];
    unsigned char voice_offer[SNAG_SESSION_VOICE_BYTES];
    size_t event_length;
    bool input_pending, closing, peer_verified, reject_verified, reject_reply;
    bool voice_offered;
    unsigned char command_reference[SNAG_SESSION_COMMAND_BYTES];
    bool command_reply, command_admitted;
    bool view_reserved, view_attached;
};

/* On success takes ownership of a private PTY master and optional initial
 * authenticated socketpair peer. That initial peer is already attached. */
int snag_session_relay_init(struct snag_session_relay *, int master, int initial_peer);
void snag_session_relay_close(struct snag_session_relay *);
/* Poll/drain once. event is COMMIT (UI must reset), BOUND (frontend now owns
 * this attachment), RESIZE, DETACH, ERROR or zero.
 * ERROR is a client diagnostic in event_data/event_length until the next step;
 * it never becomes PTY input or releases the current attachment.
 * COMMAND publishes command_reference; the presentation owner sets command_admitted
 * before its next step. The relay then sends the reference acknowledgement.
 * Returns 1 on PTY EOF, 0 on progress/timeout, -1 on a PTY/poll failure. Peer
 * failures only detach. The listener is borrowed; NULL disables new accepts. */
int snag_session_relay_step(struct snag_session_relay *, const struct snag_session_listener *,
                            int timeout_ms, enum snag_session_message *event);
/* Call while presentation writes are quiescent, after old composer coordinates
 * have been forgotten. Borrows the private PTY slave to flush old output before
 * acknowledging this peer. Pending engine input is preserved. The caller must
 * own the slave's foreground terminal, or block SIGTTOU during this call.
 * A stale/disconnected peer cannot activate a later attachment. */
int snag_session_relay_activate(struct snag_session_relay *, uint64_t generation, int slave);
/* Queue a terminal control frame. DETACH/EXIT drain pending PTY output and its
 * acknowledgements before closing the client. SUSPEND retains its reservation
 * and drains output until the same client commits on continue.
 * SWITCH keeps source until the client explicitly
 * detaches after the destination acknowledges attachment; failure keeps source.
 * EAGAIN means an earlier frame is still pending; retry through the owner. */
int snag_session_relay_control(struct snag_session_relay *, enum snag_session_message,
                               const void *, size_t);

/* Called only by the presentation owner. Both transports share one lease and
 * generation. A reservation is not yet an attached controller. */
int snag_session_relay_view_reserve(struct snag_session_relay *, uint64_t *generation);
int snag_session_relay_view_bind(struct snag_session_relay *, uint64_t generation);
void snag_session_relay_view_release(struct snag_session_relay *, uint64_t generation);

#endif
