/* SPDX-License-Identifier: GPL-2.0-only */
#include "session_relay.h"
#include "base.h"

#include <errno.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* Bound a silent handshake and blocked kernel I/O without retaining a raw
 * spool or indefinitely blocking the engine behind an unresponsive terminal. */
#define HANDSHAKE_MS 15000u
#define STALL_MS 5000u

static int
nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    int descriptor = fcntl(fd, F_GETFD);

    if (flags < 0 || descriptor < 0 ||
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, descriptor | FD_CLOEXEC) < 0) return -1;
    return 0;
}

static void
peer_drop(struct snag_session_relay *relay)
{
    if (relay->peer >= 0) (void)close(relay->peer);
    relay->peer = -1;
    relay->phase = SNAG_SESSION_WAIT_RESERVE;
    relay->handshake_deadline = 0u;
    relay->input_deadline = 0u;
    relay->output_deadline = 0u;
    relay->output_ack_deadline = 0u;
    relay->output_length = relay->output_acknowledged = 0u;
    relay->input.used = relay->input.offset = 0u;
    relay->output.used = relay->output.offset = 0u;
    relay->input_pending = relay->closing = false;
    relay->peer_verified = false;
    relay->voice_offered = false;
    relay->command_reply = relay->command_admitted = false;
    relay->input_offset = 0u;
}

static void
reject_drop(struct snag_session_relay *relay)
{
    if (relay->reject >= 0) (void)close(relay->reject);
    relay->reject = -1;
    relay->reject_deadline = 0u;
    relay->rejection.used = relay->rejection.offset = 0u;
    relay->reject_verified = false;
    relay->reject_reply = false;
}

int
snag_session_relay_init(struct snag_session_relay *relay, int master, int initial_peer)
{
    memset(relay, 0, sizeof(*relay));
    relay->master = relay->peer = relay->reject = -1;
    if (nonblocking(master) < 0 ||
        (initial_peer >= 0 && nonblocking(initial_peer) < 0)) return -1;
    relay->master = master;
    relay->peer = initial_peer;
    relay->peer_verified = initial_peer >= 0;
    relay->phase = initial_peer >= 0 ? SNAG_SESSION_ATTACHED : SNAG_SESSION_WAIT_RESERVE;
    relay->generation = initial_peer >= 0 ? 1u : 0u;
    return 0;
}

void
snag_session_relay_close(struct snag_session_relay *relay)
{
    peer_drop(relay);
    reject_drop(relay);
    if (relay->master >= 0) (void)close(relay->master);
    relay->master = -1;
}

static int
queue_output(struct snag_session_relay *relay, enum snag_session_message type,
              const void *data, size_t length)
{
    if (relay->output.used) return snag_errno(EAGAIN);
    if (snag_session_packet_set(&relay->output, type, data, length) < 0) return -1;
    relay->output_deadline = snag_monotonic_ms() + STALL_MS;
    return 0;
}

static int
master_read(struct snag_session_relay *relay, bool discard)
{
    unsigned char bytes[SNAG_SESSION_FRAME_MAX];
    ssize_t n = read(relay->master, bytes, sizeof(bytes));

    if (n > 0) {
        if (discard) return 0;
        if (queue_output(relay, SNAG_SESSION_OUTPUT, bytes, (size_t)n) < 0) return -1;
        relay->output_length = (size_t)n;
        relay->output_acknowledged = 0u;
        return 0;
    }
    if (!n || errno == EIO) return 1;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
}

int
snag_session_relay_control(struct snag_session_relay *relay, enum snag_session_message type,
                           const void *data, size_t length)
{
    if (relay->peer < 0) return snag_errno(ENOTCONN);
    if (type != SNAG_SESSION_DETACH && type != SNAG_SESSION_EXIT &&
        type != SNAG_SESSION_SWITCH && type != SNAG_SESSION_SUSPEND &&
        type != SNAG_SESSION_QUITTING && type != SNAG_SESSION_PROGRESS &&
        type != SNAG_SESSION_ERROR && type != SNAG_SESSION_RELEASED) return snag_errno(EINVAL);
    if ((type == SNAG_SESSION_SWITCH || type == SNAG_SESSION_SUSPEND) &&
        relay->phase != SNAG_SESSION_ATTACHED) return snag_errno(EBUSY);
    if (relay->closing) return snag_errno(EALREADY);
    if ((type == SNAG_SESSION_PROGRESS || type == SNAG_SESSION_ERROR) &&
        relay->phase != SNAG_SESSION_REPAINT) return snag_errno(ESTALE);
    if (type == SNAG_SESSION_PROGRESS && length) return snag_errno(EINVAL);
    if (type == SNAG_SESSION_RELEASED &&
        (relay->phase != SNAG_SESSION_RELEASING || length != 17u || !data ||
            memcmp(data, relay->voice_offer, 16u) ||
            ((const unsigned char *)data)[16] > SNAG_SESSION_VOICE_MUTED))
        return snag_errno(EINVAL);
    if (type == SNAG_SESSION_SWITCH && (length < 8u || length > SNAG_ID_HEX_LEN) &&
        length != SNAG_ID_HEX_LEN + SNAG_SESSION_VOICE_BYTES) return snag_errno(EINVAL);
    /* An ACK can clear the last frame in a poll that did not watch the master.
     * Drain already-written PTY output before closing, then await its physical
     * acknowledgement. Hard escape can still precede a stalled writer. */
    if (type == SNAG_SESSION_DETACH || type == SNAG_SESSION_EXIT) {
        if (relay->output.used || relay->output_length) return snag_errno(EAGAIN);
        if (relay->phase == SNAG_SESSION_ATTACHED || relay->phase == SNAG_SESSION_ACCEPTED) {
            if (master_read(relay, false) < 0) return -1;
            if (relay->output.used) return snag_errno(EAGAIN);
        }
    }
    if (queue_output(relay, type, data, length) < 0) return -1;
    relay->closing = type == SNAG_SESSION_DETACH || type == SNAG_SESSION_EXIT ||
        type == SNAG_SESSION_ERROR || type == SNAG_SESSION_RELEASED;
    if (type == SNAG_SESSION_SWITCH) {
        relay->voice_offered = length > SNAG_ID_HEX_LEN;
        if (relay->voice_offered) {
            memcpy(relay->voice_offer, (const unsigned char *)data + SNAG_ID_HEX_LEN,
                sizeof(relay->voice_offer));
        }
    }
    if (type == SNAG_SESSION_PROGRESS)
        relay->handshake_deadline = snag_monotonic_ms() + HANDSHAKE_MS;
    if (type == SNAG_SESSION_SUSPEND) {
        /* A stopped client retains its reservation without an I/O deadline.
         * On continue it commits fresh geometry through the repaint barrier. */
        relay->phase = SNAG_SESSION_RESERVED;
        relay->voice_offered = false;
        ++relay->generation;
    }
    return 0;
}

int
snag_session_relay_activate(struct snag_session_relay *relay, uint64_t generation, int slave)
{
    if (relay->peer < 0 || relay->phase != SNAG_SESSION_REPAINT ||
        relay->generation != generation) return snag_errno(ESTALE);
    if (relay->output.used) return snag_errno(EAGAIN);
    /* Flush both Linux queues in order while presentation is quiescent: slave
     * output feeds the master's line discipline. EAGAIN from read alone races
     * pending flip-buffer work; flushing only master input leaves that work. */
    if (tcflush(slave, TCOFLUSH) < 0 || tcflush(relay->master, TCIFLUSH) < 0) return -1;
    if (queue_output(relay, SNAG_SESSION_READY, NULL, 0u) < 0) return -1;
    relay->handshake_deadline = snag_monotonic_ms() + HANDSHAKE_MS;
    relay->phase = SNAG_SESSION_ACCEPTED;
    return 0;
}

static int
resize_terminal(struct snag_session_relay *relay, bool commit)
{
    const unsigned char *p = relay->input.bytes + SNAG_SESSION_HEADER;
    struct winsize size = {0};

    size_t expected = commit ? SNAG_SESSION_COMMIT_BYTES : 4u;
    if (snag_session_packet_length(&relay->input) != expected) return snag_errno(EPROTO);
    size.ws_row = (unsigned short)((unsigned int)p[0] | (unsigned int)p[1] << 8u);
    size.ws_col = (unsigned short)((unsigned int)p[2] | (unsigned int)p[3] << 8u);
    if (!size.ws_row || !size.ws_col) return snag_errno(EPROTO);
    return ioctl(relay->master, (unsigned int)TIOCSWINSZ, &size);
}

static int
peer_message(struct snag_session_relay *relay, enum snag_session_message *event)
{
    enum snag_session_message type = snag_session_packet_type(&relay->input);
    size_t length = snag_session_packet_length(&relay->input);

    /* An attached peer may finish in-flight input/controls after SUSPEND.
     * A fresh reservation still has a handshake deadline and cannot send
     * these frames before activation. */
    bool suspended = relay->phase == SNAG_SESSION_RESERVED && !relay->handshake_deadline;
    if (type == SNAG_SESSION_INPUT && length &&
        (relay->phase == SNAG_SESSION_ATTACHED || suspended)) {
        relay->input_pending = true;
        relay->input_offset = 0u;
        relay->input_deadline = snag_monotonic_ms() + STALL_MS;
        return 0;
    }
    if (relay->phase == SNAG_SESSION_WAIT_RESERVE) {
        if (type != SNAG_SESSION_RESERVE || length) return snag_errno(EPROTO);
        if (queue_output(relay, SNAG_SESSION_READY, NULL, 0u) < 0) return -1;
        relay->phase = SNAG_SESSION_RESERVED;
    } else if (relay->phase == SNAG_SESSION_RESERVED && type == SNAG_SESSION_OFFER) {
        const unsigned char *offer = relay->input.bytes + SNAG_SESSION_HEADER;
        if (relay->voice_offered || relay->output.used || length != SNAG_SESSION_VOICE_BYTES ||
            offer[SNAG_SESSION_VOICE_MODE] > SNAG_SESSION_VOICE_MUTED) return snag_errno(EPROTO);
        memcpy(relay->voice_offer, offer, sizeof(relay->voice_offer));
        relay->voice_offered = true;
        if (queue_output(relay, SNAG_SESSION_READY, NULL, 0u) < 0) return -1;
    } else if (relay->phase == SNAG_SESSION_RESERVED && type == SNAG_SESSION_COMMIT) {
        if (relay->output.used || relay->output_length) return snag_errno(EPROTO);
        if (length != SNAG_SESSION_COMMIT_BYTES) return snag_errno(EPROTO);
        struct snag_terminal_profile profile;
        const unsigned char *p = relay->input.bytes + SNAG_SESSION_HEADER + 4u;
        memcpy(profile.term, p, SNAG_TERMINAL_NAME_BYTES);
        memcpy(profile.sty, p + SNAG_TERMINAL_NAME_BYTES, SNAG_TERMINAL_NAME_BYTES);
        memcpy(profile.tmux, p + 2u * SNAG_TERMINAL_NAME_BYTES, SNAG_TERMINAL_NAME_BYTES);
        memcpy(profile.pane, p + 3u * SNAG_TERMINAL_NAME_BYTES, SNAG_TERMINAL_NAME_BYTES);
        if (!snag_terminal_profile_ansi(&profile)) return snag_errno(EPROTO);
        if (resize_terminal(relay, true) < 0) return -1;
        relay->profile = profile;
        relay->phase = SNAG_SESSION_REPAINT;
        relay->handshake_deadline = snag_monotonic_ms() + HANDSHAKE_MS;
        *event = SNAG_SESSION_COMMIT;
    } else if (relay->phase == SNAG_SESSION_ACCEPTED && type == SNAG_SESSION_BOUND) {
        if (length != (relay->voice_offered ? 1u : 0u)) return snag_errno(EPROTO);
        if (relay->voice_offered) {
            unsigned char mode = relay->input.bytes[SNAG_SESSION_HEADER];
            if (mode > SNAG_SESSION_VOICE_MUTED) return snag_errno(EPROTO);
            relay->voice_offer[SNAG_SESSION_VOICE_MODE] = mode;
        }
        relay->phase = SNAG_SESSION_ATTACHED;
        relay->handshake_deadline = 0u;
        *event = SNAG_SESSION_BOUND;
    } else if (relay->phase == SNAG_SESSION_ATTACHED || suspended ||
        (relay->phase == SNAG_SESSION_ACCEPTED && type == SNAG_SESSION_OUTPUT_ACK)) {
        if (type == SNAG_SESSION_OUTPUT_ACK && length == 2u) {
            const unsigned char *p = relay->input.bytes + SNAG_SESSION_HEADER;
            size_t offset = (size_t)p[0] | (size_t)p[1] << 8u;
            if (offset <= relay->output_acknowledged || offset > relay->output_length)
                return snag_errno(EPROTO);
            relay->output_acknowledged = offset;
            relay->output_ack_deadline = snag_monotonic_ms() + STALL_MS;
            if (offset == relay->output_length) {
                relay->output_length = relay->output_acknowledged = 0u;
                relay->output_ack_deadline = 0u;
            }
        } else if (type == SNAG_SESSION_COMMAND && !suspended &&
            length == SNAG_SESSION_COMMAND_BYTES && !relay->command_reply) {
            const unsigned char *reference = relay->input.bytes + SNAG_SESSION_HEADER;
            for (size_t i = 0u; i < length; ++i)
                if (!reference[i] || !strchr("0123456789abcdef", reference[i]))
                    return snag_errno(EPROTO);
            memcpy(relay->command_reference, reference, length);
            relay->command_reply = true;
            relay->command_admitted = false;
            *event = SNAG_SESSION_COMMAND;
        } else if (type == SNAG_SESSION_RESIZE) {
            if (resize_terminal(relay, false) < 0) return -1;
            *event = SNAG_SESSION_RESIZE;
        } else if (type == SNAG_SESSION_ERROR && length && length < sizeof(relay->event_data)) {
            memcpy(relay->event_data, relay->input.bytes + SNAG_SESSION_HEADER, length);
            relay->event_data[length] = 0;
            relay->event_length = length;
            *event = SNAG_SESSION_ERROR;
        } else if (type == SNAG_SESSION_RELEASE && relay->voice_offered && length == 16u &&
            !memcmp(relay->input.bytes + SNAG_SESSION_HEADER, relay->voice_offer, 16u)) {
            /* Drain late source output while the engine retires its media and
             * supplies the final requested mode. EOF cannot replace that receipt. */
            relay->phase = SNAG_SESSION_RELEASING;
            relay->handshake_deadline = snag_monotonic_ms() + HANDSHAKE_MS;
            relay->output_length = relay->output_acknowledged = 0u;
            relay->output_ack_deadline = 0u;
            *event = SNAG_SESSION_RELEASE;
        } else if (type == SNAG_SESSION_DETACH && !length) {
            peer_drop(relay);
            *event = SNAG_SESSION_DETACH;
        } else return snag_errno(EPROTO);
    } else return snag_errno(EPROTO);
    relay->input.used = relay->input.offset = 0u;
    return 0;
}

static int
master_write(struct snag_session_relay *relay)
{
    size_t length = snag_session_packet_length(&relay->input);
    const unsigned char *p = relay->input.bytes + SNAG_SESSION_HEADER;
    ssize_t n = write(relay->master, p + relay->input_offset, length - relay->input_offset);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        return -1;
    }
    if (n > 0) relay->input_deadline = snag_monotonic_ms() + STALL_MS;
    relay->input_offset += (size_t)n;
    if (relay->input_offset == length) {
        relay->input_pending = false;
        relay->input_deadline = 0u;
        relay->input.used = relay->input.offset = 0u;
    }
    return 0;
}

static void
peer_write(struct snag_session_relay *relay, enum snag_session_message *event)
{
    size_t before = relay->output.offset;
    int rc = snag_session_packet_write(relay->peer, &relay->output);

    if (rc < 0 || (rc == 1 && relay->closing)) {
        peer_drop(relay);
        *event = SNAG_SESSION_DETACH;
    } else if (rc == 1) {
        if (snag_session_packet_type(&relay->output) == SNAG_SESSION_OUTPUT &&
            relay->phase != SNAG_SESSION_RELEASING)
            relay->output_ack_deadline = snag_monotonic_ms() + STALL_MS;
        relay->output.used = relay->output.offset = 0u;
        relay->output_deadline = 0u;
    } else if (before != relay->output.offset) {
        relay->output_deadline = snag_monotonic_ms() + STALL_MS;
    }
}

static int
deadline_wait(int timeout, uint64_t now, uint64_t deadline)
{
    if (!deadline) return timeout;
    int remaining = deadline <= now ? 0 : (int)(deadline - now);
    return timeout < 0 || remaining < timeout ? remaining : timeout;
}

static void
expire(struct snag_session_relay *relay, uint64_t now, enum snag_session_message *event)
{
    if ((relay->handshake_deadline && now >= relay->handshake_deadline) ||
        (relay->output_deadline && now >= relay->output_deadline) ||
        (relay->output_ack_deadline && now >= relay->output_ack_deadline) ||
        (relay->input_deadline && now >= relay->input_deadline)) {
        peer_drop(relay);
        *event = SNAG_SESSION_DETACH;
    }
    if (relay->reject_deadline && now >= relay->reject_deadline) reject_drop(relay);
}

static void
accept_peer(struct snag_session_relay *relay, const struct snag_session_listener *listener)
{
    int fd = snag_session_listener_accept(listener);

    if (fd < 0) return;
    int verified = snag_session_peer_verify(fd);
    if (verified < 0) {
        (void)close(fd);
        return;
    }
    /* Read the request before reserving anything. A status reader never changes
     * the current peer, attachment generation, repaint or terminal input. */
    relay->reject = fd;
    relay->reject_verified = verified == 1;
    relay->reject_deadline = snag_monotonic_ms() + STALL_MS;
}

static int
candidate_message(struct snag_session_relay *relay)
{
    enum snag_session_message type = snag_session_packet_type(&relay->rejection);
    if (snag_session_packet_length(&relay->rejection)) return snag_errno(EPROTO);
    if (type == SNAG_SESSION_STATUS) {
        unsigned char attached = relay->view_attached || (relay->peer >= 0 &&
            (relay->phase == SNAG_SESSION_ATTACHED ||
             (relay->phase == SNAG_SESSION_RESERVED && !relay->handshake_deadline)));
        relay->reject_reply = true;
        return snag_session_packet_set(&relay->rejection, SNAG_SESSION_STATUS, &attached, 1u);
    }
    if (type != SNAG_SESSION_RESERVE) return snag_errno(EPROTO);
    if (relay->peer >= 0 || relay->view_reserved) {
        static const char busy[] = "session already has a terminal or attachment reservation";
        relay->reject_reply = true;
        return snag_session_packet_set(&relay->rejection, SNAG_SESSION_ERROR,
                                        busy, sizeof(busy) - 1u);
    }
    relay->peer = relay->reject;
    relay->peer_verified = true;
    relay->reject = -1;
    reject_drop(relay);
    relay->phase = SNAG_SESSION_RESERVED;
    ++relay->generation;
    relay->handshake_deadline = snag_monotonic_ms() + HANDSHAKE_MS;
    return queue_output(relay, SNAG_SESSION_READY, NULL, 0u);
}

int
snag_session_relay_step(struct snag_session_relay *relay,
                        const struct snag_session_listener *listener, int timeout_ms,
                        enum snag_session_message *event)
{
    *event = 0;
    relay->event_length = 0u;
    if (timeout_ms < -1 || relay->master < 0) return snag_errno(EINVAL);
    uint64_t now = snag_monotonic_ms();
    if (relay->command_reply && !relay->output.used) {
        unsigned char reply[SNAG_SESSION_COMMAND_BYTES + 1u];
        memcpy(reply, relay->command_reference, SNAG_SESSION_COMMAND_BYTES);
        reply[SNAG_SESSION_COMMAND_BYTES] = relay->command_admitted;
        if (queue_output(relay, SNAG_SESSION_COMMAND, reply, sizeof(reply)) < 0) return -1;
        relay->command_reply = false;
    }
    expire(relay, now, event);
    if (*event) timeout_ms = 0;
    timeout_ms = deadline_wait(timeout_ms, now, relay->handshake_deadline);
    timeout_ms = deadline_wait(timeout_ms, now, relay->output_deadline);
    timeout_ms = deadline_wait(timeout_ms, now, relay->output_ack_deadline);
    timeout_ms = deadline_wait(timeout_ms, now, relay->input_deadline);
    timeout_ms = deadline_wait(timeout_ms, now, relay->reject_deadline);
    bool output = relay->peer >= 0 && (relay->phase == SNAG_SESSION_ATTACHED ||
        relay->phase == SNAG_SESSION_ACCEPTED);
    struct pollfd fds[] = {
        {relay->master, (!output || (!relay->output.used && !relay->output_length) ? POLLIN : 0) |
            (relay->input_pending ? POLLOUT : 0), 0},
        {relay->peer, (!relay->input_pending && !relay->closing ? POLLIN : 0) |
            (relay->output.used ? POLLOUT : 0), 0},
        {relay->reject, relay->reject_reply ? POLLOUT : POLLIN, 0},
        {listener && relay->reject < 0 ? listener->fd : -1, POLLIN, 0}
    };
    int rc = poll(fds, sizeof(fds) / sizeof(fds[0]), timeout_ms);
    if (rc < 0) return errno == EINTR ? 0 : -1;
    expire(relay, snag_monotonic_ms(), event);
    if (relay->peer >= 0 && relay->peer == fds[1].fd) {
        /* Readiness from this poll predates the queued acceptance write. A
         * frontend cannot have bound to an acceptance it has not received. */
        if (relay->phase == SNAG_SESSION_ACCEPTED && relay->output.used &&
            snag_session_packet_type(&relay->output) == SNAG_SESSION_READY &&
            fds[1].revents & POLLIN) {
            peer_drop(relay);
            *event = SNAG_SESSION_DETACH;
        } else if (relay->output.used && fds[1].revents & POLLOUT) {
            peer_write(relay, event);
        }
        /* A hung-up socket remains poll-ready even with no requested events.
         * Drop it now rather than spinning behind blocked PTY input. */
        if (relay->peer >= 0 && fds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            peer_drop(relay);
            *event = SNAG_SESSION_DETACH;
        }
        if (relay->peer >= 0 && !relay->input_pending && !relay->closing &&
            fds[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
            rc = relay->peer_verified ? 1 : snag_session_peer_verify(relay->peer);
            if (rc == 1) {
                relay->peer_verified = true;
                rc = snag_session_packet_read(relay->peer, &relay->input);
            }
            if (rc < 0 || (rc == 1 && peer_message(relay, event) < 0)) {
                peer_drop(relay);
                *event = SNAG_SESSION_DETACH;
            }
        }
    }
    if (relay->input_pending && fds[0].revents & POLLOUT) {
        if (master_write(relay) < 0) return -1;
    }
    if (fds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
        output = relay->peer >= 0 && (relay->phase == SNAG_SESSION_ATTACHED ||
            relay->phase == SNAG_SESSION_ACCEPTED);
        if (!output || (!relay->output.used && !relay->output_length)) {
            rc = master_read(relay, !output);
            if (rc != 0) return rc;
        }
    }
    if (relay->reject >= 0 && relay->reject == fds[2].fd && fds[2].revents) {
        rc = relay->reject_verified ? 1 : snag_session_peer_verify(relay->reject);
        if (rc == 1) {
            relay->reject_verified = true;
            if (relay->reject_reply) {
                rc = snag_session_packet_write(relay->reject, &relay->rejection);
            } else {
                rc = snag_session_packet_read(relay->reject, &relay->rejection);
                if (rc == 1) rc = candidate_message(relay);
            }
        }
        if (rc != 0) reject_drop(relay);
    }
    if (fds[3].revents & POLLIN) accept_peer(relay, listener);
    return 0;
}
#else
int
snag_session_relay_init(struct snag_session_relay *relay, int master, int initial_peer)
{
    (void)master;
    (void)initial_peer;
    memset(relay, 0, sizeof(*relay));
    relay->master = relay->peer = relay->reject = -1;
    return snag_errno(ENOTSUP);
}

void
snag_session_relay_close(struct snag_session_relay *relay)
{
    (void)relay;
}

int
snag_session_relay_step(struct snag_session_relay *relay,
                        const struct snag_session_listener *listener, int timeout_ms,
                        enum snag_session_message *event)
{
    (void)relay;
    (void)listener;
    (void)timeout_ms;
    *event = 0;
    return snag_errno(ENOTSUP);
}

int
snag_session_relay_activate(struct snag_session_relay *relay, uint64_t generation, int slave)
{
    (void)relay;
    (void)generation;
    (void)slave;
    return snag_errno(ENOTSUP);
}

int
snag_session_relay_control(struct snag_session_relay *relay, enum snag_session_message type,
                           const void *data, size_t length)
{
    (void)relay;
    (void)type;
    (void)data;
    (void)length;
    return snag_errno(ENOTSUP);
}
#endif /* !_WIN32 */

int
snag_session_relay_view_reserve(struct snag_session_relay *relay, uint64_t *generation)
{
    if (relay->master < 0) return snag_errno(ENOTSUP);
    if (relay->peer >= 0 || relay->view_reserved) return snag_errno(EBUSY);
    relay->view_reserved = true;
    relay->view_attached = false;
    *generation = ++relay->generation;
    return 0;
}

int
snag_session_relay_view_bind(struct snag_session_relay *relay, uint64_t generation)
{
    if (!relay->view_reserved || relay->generation != generation) return snag_errno(ESTALE);
    relay->view_attached = true;
    return 0;
}

void
snag_session_relay_view_release(struct snag_session_relay *relay, uint64_t generation)
{
    if (!relay->view_reserved || relay->generation != generation) return;
    relay->view_reserved = relay->view_attached = false;
}
