/* SPDX-License-Identifier: GPL-2.0-only */
#include "session_client.h"
#include "base.h"
#include "fs.h"
#include "term_host.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define HANDSHAKE_MS 15000u
#define STALL_MS 5000u

static void
packet_clear(struct snag_session_packet *packet)
{
    packet->used = packet->offset = 0u;
}

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

int
snag_session_client_init(struct snag_session_client *client, int terminal, int peer)
{
    memset(client, 0, sizeof(*client));
    client->terminal = client->peer = client->target = -1;
    if (snag_terminal_profile_capture(&client->profile) < 0) return -1;
    if (nonblocking(terminal) < 0 || (peer >= 0 && nonblocking(peer) < 0)) return -1;
    client->terminal = terminal;
    client->peer = peer;
    client->geometry[0] = 24u;
    client->geometry[2] = 80u;
    return 0;
}

void
snag_session_client_close(struct snag_session_client *client)
{
    if (client->terminal >= 0) (void)close(client->terminal);
    if (client->peer >= 0) (void)close(client->peer);
    if (client->target >= 0) (void)close(client->target);
    client->terminal = client->peer = client->target = -1;
}

int
snag_session_client_resize(struct snag_session_client *client, unsigned int rows, unsigned int cols)
{
    if (!rows || !cols || rows > 65535u || cols > 65535u) return snag_errno(EINVAL);
    client->geometry[0] = (unsigned char)rows;
    client->geometry[1] = (unsigned char)(rows >> 8u);
    client->geometry[2] = (unsigned char)cols;
    client->geometry[3] = (unsigned char)(cols >> 8u);
    client->resize_pending = true;
    return 0;
}

int
snag_session_client_attach(struct snag_session_client *client, int target)
{
    if (client->target >= 0 || target == client->peer) return snag_errno(EBUSY);
    if (target < 0 || nonblocking(target) < 0) return -1;
    if (snag_session_packet_set(&client->target_output, SNAG_SESSION_RESERVE, NULL, 0u) < 0)
        return -1;
    packet_clear(&client->target_input);
    client->target = target;
    client->phase = SNAG_CLIENT_RESERVING;
    client->target_deadline = snag_monotonic_ms() + HANDSHAKE_MS;
    return 0;
}

int
snag_session_client_continue(struct snag_session_client *client)
{
    if (client->target >= 0 || client->input.used || client->output_pending ||
        client->ack_pending || client->incoming.used)
        return snag_errno(EBUSY);
    if (client->peer < 0) return snag_errno(ENOTCONN);
    if (snag_session_commit_set(&client->target_output, client->geometry, &client->profile) < 0)
        return -1;
    client->target = client->peer;
    client->peer = -1;
    packet_clear(&client->target_input);
    client->phase = SNAG_CLIENT_COMMITTING;
    client->target_deadline = snag_monotonic_ms() + HANDSHAKE_MS;
    return 0;
}

int
snag_session_client_error(struct snag_session_client *client, const char *text)
{
    if (client->peer < 0) return snag_errno(ENOTCONN);
    if (client->input.used) return snag_errno(EAGAIN);
    size_t length = strlen(text);
    if (!length || length >= sizeof(client->event_data)) return snag_errno(EINVAL);
    return snag_session_packet_set(&client->input, SNAG_SESSION_ERROR, text, length);
}

static void
target_failed(struct snag_session_client *client, const char *text,
               enum snag_session_message *event)
{
    if (client->target >= 0) (void)close(client->target);
    client->target = -1;
    client->target_deadline = 0u;
    packet_clear(&client->target_input);
    packet_clear(&client->target_output);
    client->event_length = strlen(text);
    if (client->event_length >= sizeof(client->event_data))
        client->event_length = sizeof(client->event_data) - 1u;
    memcpy(client->event_data, text, client->event_length);
    client->event_data[client->event_length] = 0;
    *event = SNAG_SESSION_ERROR;
}

static int
target_read(struct snag_session_client *client, enum snag_session_message *event)
{
    struct snag_session_packet *packet = &client->target_input;
    int rc = snag_session_packet_read(client->target, packet);
    if (rc < 0) {
        target_failed(client, "destination connection ended before attachment", event);
        return 0;
    }
    if (!rc) return 0;
    enum snag_session_message type = snag_session_packet_type(packet);
    size_t length = snag_session_packet_length(packet);
    if (type == SNAG_SESSION_ERROR) {
        char message[sizeof(client->event_data)];
        if (length >= sizeof(message)) length = sizeof(message) - 1u;
        memcpy(message, packet->bytes + SNAG_SESSION_HEADER, length);
        message[length] = 0;
        target_failed(client, length ? message : "destination refused attachment", event);
        return 0;
    }
    if (type != SNAG_SESSION_READY || length || client->target_output.used ||
        client->phase == SNAG_CLIENT_READY) {
        target_failed(client, "invalid destination attachment acknowledgement", event);
        return 0;
    }
    if (client->phase == SNAG_CLIENT_RESERVING) {
        rc = snag_session_commit_set(&client->target_output, client->geometry, &client->profile);
        if (rc < 0) return -1;
        client->phase = SNAG_CLIENT_COMMITTING;
    } else client->phase = SNAG_CLIENT_READY;
    packet_clear(packet);
    return 0;
}

static bool
commit_target(struct snag_session_client *client, enum snag_session_message *event)
{
    if (client->target < 0 || client->phase != SNAG_CLIENT_READY ||
        client->output_pending || client->input.used || client->ack_pending ||
        client->incoming.used) return false;
    if (client->peer >= 0) (void)close(client->peer);
    client->peer = client->target;
    client->quitting = client->peer_ended = false;
    client->target = -1;
    client->target_deadline = 0u;
    packet_clear(&client->output);
    *event = SNAG_SESSION_READY;
    return true;
}

static int
terminal_write(struct snag_session_client *client)
{
    size_t length = snag_session_packet_length(&client->output);
    const unsigned char *p = client->output.bytes + SNAG_SESSION_HEADER;
    ssize_t n = write(client->terminal, p + client->terminal_offset,
                       length - client->terminal_offset);
    if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
    if (!n) return snag_errno(EIO);
    client->terminal_deadline = snag_monotonic_ms() + STALL_MS;
    if (memchr(p + client->terminal_offset, '\033', (size_t)n)) client->ansi_output = true;
    client->terminal_offset += (size_t)n;
    client->ack_pending = true;
    if (client->terminal_offset == length) {
        packet_clear(&client->output);
        client->output_pending = false;
        client->terminal_deadline = 0u;
    }
    return 0;
}

static int
peer_read(struct snag_session_client *client, enum snag_session_message *event)
{
    int rc = snag_session_packet_read(client->peer, &client->incoming);
    if (rc < 0) {
        client->peer_ended = errno == ECONNRESET;
        return 1;
    }
    if (!rc) return 0;
    enum snag_session_message type = snag_session_packet_type(&client->incoming);
    size_t length = snag_session_packet_length(&client->incoming);
    const unsigned char *bytes = client->incoming.bytes + SNAG_SESSION_HEADER;
    if (type == SNAG_SESSION_QUITTING && !length) {
        /* An accepted hard escape announces intent; only peer EOF completes it. */
        client->quitting = true;
        packet_clear(&client->incoming);
        return 0;
    }
    if (type == SNAG_SESSION_OUTPUT && length) {
        if (client->output_pending || client->ack_pending) return snag_errno(EPROTO);
        client->output = client->incoming;
        packet_clear(&client->incoming);
        client->output_pending = true;
        client->terminal_offset = 0u;
        client->terminal_deadline = snag_monotonic_ms() + STALL_MS;
        return 0;
    }
    bool valid = (type == SNAG_SESSION_DETACH && !length) ||
                 (type == SNAG_SESSION_SUSPEND && !length) ||
                 (type == SNAG_SESSION_EXIT && length == 1u) ||
                 (type == SNAG_SESSION_SWITCH && length >= 8u && length <= 32u) ||
                 (type == SNAG_SESSION_ERROR && length && length < sizeof(client->event_data));
    if (!valid) return snag_errno(EPROTO);
    /* Ordinary controls stay after their display bytes. Only hard-exit intent
     * is consumed ahead of the physical writer, and it still waits for EOF. */
    if (client->output_pending) return 0;
    if (type == SNAG_SESSION_SWITCH) {
        for (size_t i = 0u; i < length; ++i)
            if (!strchr("0123456789abcdef", bytes[i]) || !bytes[i]) return snag_errno(EPROTO);
    }
    memcpy(client->event_data, bytes, length);
    client->event_data[length] = 0;
    client->event_length = length;
    packet_clear(&client->incoming);
    *event = type;
    return 0;
}

static int
wait_deadline(int timeout, uint64_t now, uint64_t deadline)
{
    if (!deadline) return timeout;
    int remaining = deadline <= now ? 0 : (int)(deadline - now);
    return timeout < 0 || remaining < timeout ? remaining : timeout;
}

int
snag_session_client_step(struct snag_session_client *client, int timeout_ms,
                          enum snag_session_message *event)
{
    *event = 0;
    client->event_length = 0u;
    if (timeout_ms < -1 || client->terminal < 0) return snag_errno(EINVAL);
    uint64_t now = snag_monotonic_ms();
    if (client->terminal_deadline && now >= client->terminal_deadline) return snag_errno(ETIMEDOUT);
    if (client->peer >= 0 && client->ack_pending && !client->input.used) {
        unsigned char offset[2] = {(unsigned char)client->terminal_offset,
                                  (unsigned char)(client->terminal_offset >> 8u)};
        if (snag_session_packet_set(&client->input, SNAG_SESSION_OUTPUT_ACK,
                                     offset, sizeof(offset)) < 0) return -1;
        client->ack_pending = false;
    }
    if (client->target_deadline && now >= client->target_deadline) {
        target_failed(client, client->peer >= 0 ?
            "destination attachment timed out; original session retained" :
            "destination attachment timed out", event);
        return 0;
    }
    if (commit_target(client, event)) return 0;
    if (client->resize_pending && client->peer >= 0 && client->target < 0 && !client->input.used) {
        if (snag_session_packet_set(&client->input, SNAG_SESSION_RESIZE,
                                    client->geometry, sizeof(client->geometry)) < 0) return -1;
        client->resize_pending = false;
    }
    timeout_ms = wait_deadline(timeout_ms, now, client->target_deadline);
    timeout_ms = wait_deadline(timeout_ms, now, client->terminal_deadline);
    bool incoming_ready = client->incoming.used >= SNAG_SESSION_HEADER &&
        client->incoming.used == SNAG_SESSION_HEADER +
                                 snag_session_packet_length(&client->incoming);
    if (incoming_ready && !client->output_pending) timeout_ms = 0;
    struct pollfd fds[] = {
        {client->terminal, (client->peer >= 0 && client->target < 0 && !client->input.used ?
            POLLIN : 0) | (client->output_pending ? POLLOUT : 0), 0},
        {client->peer, (!incoming_ready ? POLLIN : 0) |
            (client->input.used ? POLLOUT : 0), 0},
        {client->target, (client->phase != SNAG_CLIENT_READY ? POLLIN : 0) |
            (client->target_output.used ? POLLOUT : 0), 0}
    };
    int rc = poll(fds, sizeof(fds) / sizeof(fds[0]), timeout_ms);
    if (rc < 0) return errno == EINTR ? 0 : -1;
    if (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) return 1;
    if (fds[1].revents & POLLHUP && client->output_pending) {
        /* A dead owner cannot wait for a stalled physical writer. Drain its
         * remaining control frames; no old display bytes are replayed later. */
        client->output_pending = false;
        client->ack_pending = false;
        client->terminal_deadline = 0u;
        packet_clear(&client->output);
    }
    if (client->output_pending && fds[0].revents & POLLOUT && terminal_write(client) < 0) return -1;
    if (client->target >= 0) {
        if (client->target_output.used && fds[2].revents & POLLOUT) {
            rc = snag_session_packet_write(client->target, &client->target_output);
            if (rc < 0) {
                target_failed(client, "cannot send destination attachment handshake", event);
                return 0;
            }
            if (rc == 1) packet_clear(&client->target_output);
        }
        if (fds[2].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
            if (client->phase == SNAG_CLIENT_READY) {
                target_failed(client, "destination disconnected before transfer", event);
            } else if (target_read(client, event) < 0) return -1;
            if (*event) return 0;
        }
    }
    if (client->peer >= 0 && client->input.used && fds[1].revents & POLLOUT &&
        !(fds[1].revents & POLLHUP)) {
        rc = snag_session_packet_write(client->peer, &client->input);
        if (rc < 0) return 1;
        if (rc == 1) packet_clear(&client->input);
    }
    if (commit_target(client, event)) return 0;
    if (client->peer >= 0 && (incoming_ready ||
        fds[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
        rc = peer_read(client, event);
        if (rc || *event) return rc;
    }
    if (fds[0].revents & POLLIN && !client->input.used && client->target < 0) {
        unsigned char bytes[SNAG_SESSION_FRAME_MAX];
        ssize_t n = read(client->terminal, bytes, sizeof(bytes));
        if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
        if (!n) return 1;
        return snag_session_packet_set(&client->input, SNAG_SESSION_INPUT, bytes, (size_t)n);
    }
    return 0;
}
static volatile sig_atomic_t terminal_signal, terminal_resize;

static void
terminal_control(int number)
{
    if (number == SIGWINCH) terminal_resize = 1;
    else terminal_signal = number;
}

static int
terminal_geometry(struct snag_session_client *client)
{
    struct winsize size;
    if (ioctl(client->terminal, TIOCGWINSZ, &size) < 0) return -1;
    return snag_session_client_resize(client, size.ws_row ? size.ws_row : 24u,
                                      size.ws_col ? size.ws_col : 80u);
}

static int
terminal_open(void)
{
    /* Reopen, rather than dup, so nonblocking I/O cannot change the shell's
     * inherited open-file description. Check the reopened device identity. */
    char path[SNAG_PATH_MAX_BYTES + 1u];
    struct stat before, after;
    int rc = ttyname_r(STDIN_FILENO, path, sizeof(path));
    if (rc) return snag_errno(rc > 0 ? rc : errno);
    if (fstat(STDIN_FILENO, &before) < 0) return -1;
    int fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (snag_fd_cloexec(fd) < 0 || fstat(fd, &after) < 0) {
        int saved = errno;
        (void)close(fd);
        return snag_errno(saved);
    }
    if (before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_rdev != after.st_rdev) {
        (void)close(fd);
        return snag_errno(ESTALE);
    }
    return fd;
}

int
snag_session_client_terminal(int peer, bool attached, uint64_t child,
                             snag_session_connect_fn connect, void *opaque,
                             char *error, size_t error_size)
{
    struct snag_session_client client = {.terminal = -1, .peer = -1, .target = -1};
    struct snag_shutdown shutdown;
    struct snag_term_host host;
    struct termios original, raw;
    bool signals = false, controls = false, raw_active = false;
    char notice[256] = {0};
    int terminal = -1, result = -1, notice_peer = -1;
    terminal_signal = terminal_resize = 0;
    if (error_size) error[0] = '\0';
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        errno = ENOTTY;
        goto out;
    }
    terminal = terminal_open();
    if (terminal < 0 || tcgetattr(terminal, &original) < 0) goto out;
    if (snag_session_client_init(&client, terminal, attached ? peer : -1) < 0) goto out;
    terminal = -1;
    if (attached) peer = -1;
    if (!snag_terminal_profile_ansi(&client.profile)) {
        errno = ENOTSUP;
        (void)snag_errorf(error, error_size, "native attachment requires an ANSI-capable TERM");
        goto out;
    }
    if (terminal_geometry(&client) < 0) goto out;
    if (!attached) {
        if (snag_session_client_attach(&client, peer) < 0) goto out;
        peer = -1;
    }
    if (snag_shutdown_install(&shutdown, terminal_control, true) < 0) goto out;
    signals = true;
    memset(&host, 0, sizeof(host));
    if (snag_term_controls_install(&host, terminal_control, terminal_control) < 0) goto out;
    controls = true;
    raw = original;
    raw.c_iflag &= (tcflag_t)~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_lflag &= (tcflag_t)~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    raw.c_cflag &= (tcflag_t)~(CSIZE | PARENB);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(client.terminal, TCSANOW, &raw) < 0) goto out;
    raw_active = true;
    while (!terminal_signal) {
        /* A diagnostic belongs to its source, never to a later session. */
        if (notice[0] && notice_peer != client.peer) notice[0] = '\0';
        if (terminal_resize) {
            terminal_resize = 0;
            if (terminal_geometry(&client) < 0) goto out;
        }
        if (notice[0] && client.target < 0) {
            if (client.peer < 0) {
                (void)snag_errorf(error, error_size, "%s", notice);
                goto out;
            }
            if (snag_session_client_error(&client, notice) == 0) notice[0] = '\0';
            else if (errno != EAGAIN) goto out;
        }
        enum snag_session_message event;
        int rc = snag_session_client_step(&client, 50, &event);
        if (rc) {
            if (rc > 0 && client.quitting && client.peer_ended) result = 0;
            else if (rc > 0)
                (void)snag_errorf(error, error_size, "session terminal connection ended");
            goto out;
        }
        if (event == SNAG_SESSION_EXIT || event == SNAG_SESSION_DETACH) {
            result = event == SNAG_SESSION_EXIT ? client.event_data[0] : 0;
            goto out;
        }
        if (event == SNAG_SESSION_ERROR) {
            notice_peer = client.peer;
            (void)snprintf(notice, sizeof(notice), "%s", client.event_data);
        } else if (event == SNAG_SESSION_SWITCH) {
            notice_peer = client.peer;
            notice[0] = '\0';
            int target = connect ? connect(opaque, (const char *)client.event_data,
                                           notice, sizeof(notice)) : snag_errno(ENOTSUP);
            if (target >= 0) {
                if (snag_session_client_attach(&client, target) < 0) {
                    int saved = errno;
                    (void)close(target);
                    (void)snprintf(notice, sizeof(notice), "cannot switch session: %s",
                        strerror(saved));
                }
            } else if (!notice[0]) {
                (void)snprintf(notice, sizeof(notice), "cannot connect session: %s",
                    strerror(errno));
            }
        } else if (event == SNAG_SESSION_SUSPEND) {
            if (client.target >= 0) {
                enum snag_session_message ignored;
                target_failed(&client, "session switch interrupted by suspension", &ignored);
                notice_peer = client.peer;
                (void)snprintf(notice, sizeof(notice), "%s", client.event_data);
            }
            while (client.input.used || client.ack_pending) {
                if (snag_session_client_step(&client, 50, &event) != 0) goto out;
                if (event == SNAG_SESSION_EXIT || event == SNAG_SESSION_DETACH) {
                    result = event == SNAG_SESSION_EXIT ? client.event_data[0] : 0;
                    goto out;
                }
            }
            if (tcsetattr(client.terminal, TCSANOW, &original) < 0) goto out;
            raw_active = false;
            if (snag_term_suspend() < 0) goto out;
            pid_t foreground;
            while (!terminal_signal && (foreground = tcgetpgrp(client.terminal)) > 0 &&
                   foreground != getpgrp())
                if (raise(SIGTTIN) < 0) goto out;
            if (terminal_signal) {
                result = 128 + terminal_signal;
                goto out;
            }
            if (tcsetattr(client.terminal, TCSANOW, &raw) < 0) goto out;
            raw_active = true;
            if (terminal_geometry(&client) < 0 || snag_session_client_continue(&client) < 0)
                goto out;
        }
    }
    result = 128 + terminal_signal;
out: {
        int saved = errno;
        if (raw_active) {
            static const char reset[] = "\033[?2004l\033[0m\r\n";
            /* Plain/narrow owners may never have enabled ANSI terminal state. */
            if (client.ansi_output)
                (void)write(client.terminal, reset, sizeof(reset) - 1u);
            if (tcsetattr(client.terminal, TCSANOW, &original) < 0 && result >= 0) {
                result = -1;
                saved = errno;
            }
        }
        if (controls) snag_term_controls_restore(&host);
        if (signals) snag_shutdown_finish(&shutdown);
        snag_session_client_close(&client);
        if (terminal >= 0) (void)close(terminal);
        if (peer >= 0) (void)close(peer);
        if (child) (void)waitpid((pid_t)child, NULL, WNOHANG);
        if (result < 0 && error_size && !error[0])
            (void)snag_errorf(error, error_size, "native terminal client: %s", strerror(saved));
        errno = saved;
        return result;
    }
}
#else
int
snag_session_client_terminal(int peer, bool attached, uint64_t child,
                             snag_session_connect_fn connect, void *opaque,
                             char *error, size_t error_size)
{
    (void)peer;
    (void)attached;
    (void)child;
    (void)connect;
    (void)opaque;
    return snag_fail(error, error_size, ENOTSUP,
        "native terminal clients are unavailable on this host");
}

int
snag_session_client_init(struct snag_session_client *client, int terminal, int peer)
{
    (void)terminal;
    (void)peer;
    memset(client, 0, sizeof(*client));
    client->terminal = client->peer = client->target = -1;
    return snag_errno(ENOTSUP);
}

void
snag_session_client_close(struct snag_session_client *client)
{
    (void)client;
}

int
snag_session_client_resize(struct snag_session_client *client, unsigned int rows, unsigned int cols)
{
    (void)client;
    (void)rows;
    (void)cols;
    return snag_errno(ENOTSUP);
}

int
snag_session_client_attach(struct snag_session_client *client, int target)
{
    (void)client;
    (void)target;
    return snag_errno(ENOTSUP);
}

int
snag_session_client_continue(struct snag_session_client *client)
{
    (void)client;
    return snag_errno(ENOTSUP);
}

int
snag_session_client_error(struct snag_session_client *client, const char *text)
{
    (void)client;
    (void)text;
    return snag_errno(ENOTSUP);
}

int
snag_session_client_step(struct snag_session_client *client, int timeout_ms,
                          enum snag_session_message *event)
{
    (void)client;
    (void)timeout_ms;
    *event = 0;
    return snag_errno(ENOTSUP);
}
#endif /* !_WIN32 */
