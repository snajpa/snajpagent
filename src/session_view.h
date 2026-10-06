/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_SESSION_VIEW_H
#define SNAJPAGENT_SESSION_VIEW_H

#include "json.h"
#include "session_relay.h"

/* SV/1 frames carry offset/total (little endian u64), then JSON bytes. The
 * message bound admits the existing 1 MiB prompt even when every byte needs a
 * six-byte JSON escape, plus a frame of routing/receipt metadata. */
#define SNAG_VIEW_MESSAGE_MAX (6u * SNAG_MAX_DIRECT_PROMPT + SNAG_SESSION_FRAME_MAX)
struct snag_view_channel {
    int fd;
    bool verified;
    struct snag_session_packet incoming, outgoing;
    struct snag_buf input;
    uint64_t total, read_deadline, write_deadline;
    char *output;
    size_t output_len, output_offset;
};

void snag_view_channel_init(struct snag_view_channel *, int fd);
void snag_view_channel_close(struct snag_view_channel *);
/* Queue one JSON message; borrows the value. EAGAIN preserves pending output. */
int snag_view_channel_send(struct snag_view_channel *, const json_t *);
/* One transport slice per call. 1 complete, 0 partial/would-block, -1 failure.
 * A received JSON object transfers ownership to the caller. */
int snag_view_channel_write(struct snag_view_channel *);
int snag_view_channel_read(struct snag_view_channel *, json_t **);

struct snag_view_server;
struct snag_view_callbacks {
    /* All callbacks run on the presentation owner, never on the engine. */
    void (*bound)(void *, uint64_t generation);
    int (*submit)(void *, const char *id, const char *text, uint64_t generation,
        bool terminal);
    int (*control)(void *, bool quit);
    void *opaque;
};

#if SNAJPAGENT_VM
struct snag_view_server *snag_view_server_open(int dir_fd, const char *path, int lock_fd,
    const char *session, struct snag_session_relay *, struct snag_view_callbacks);
void snag_view_server_close(struct snag_view_server *);
/* Stop admission before dropping the writer lock; publish exit after it closes. */
void snag_view_server_stop(struct snag_view_server *);
void snag_view_server_exit(struct snag_view_server *, unsigned int status);
bool snag_view_server_busy(const struct snag_view_server *);
/* Nonblocking service. Malformed/stalled peers are isolated from the owner. */
void snag_view_server_step(struct snag_view_server *);
/* Borrow immutable snapshots/receipts. Replaces pending coalescible state;
 * receipts survive a connection loss for the lifetime of this owner. */
void snag_view_server_state(struct snag_view_server *, const json_t *);
int snag_view_server_result(struct snag_view_server *, const char *id,
    const char *status, uint64_t seq, const char *event);
int snag_view_server_command_result(struct snag_view_server *, const json_t *);
int snag_view_server_report(struct snag_view_server *, const json_t *, const char *error);
bool snag_view_server_attached(const struct snag_view_server *);
/* Called only for the bound terminal peer, after its repaint barrier. */
int snag_view_server_terminal(struct snag_view_server *, const unsigned char *reference);
int snag_view_server_terminal_generation(struct snag_view_server *, const char *, uint64_t *);

#else
static inline int
snag_view_server_report(struct snag_view_server *server, const json_t *report, const char *error)
{
    (void)server;
    (void)report;
    (void)error;
    return 0;
}

static inline int
snag_view_server_terminal_generation(struct snag_view_server *server, const char *id,
    uint64_t *generation)
{
    (void)server;
    (void)id;
    *generation = 0u;
    return 0;
}

static inline int
snag_view_server_terminal(struct snag_view_server *server, const unsigned char *reference)
{
    (void)server;
    (void)reference;
    return -1;
}

static inline int
snag_view_server_command_result(struct snag_view_server *server, const json_t *result)
{
    (void)server;
    (void)result;
    return 0;
}

static inline void
snag_view_server_stop(struct snag_view_server *server)
{
    (void)server;
}

static inline void
snag_view_server_exit(struct snag_view_server *server, unsigned int status)
{
    (void)server;
    (void)status;
}

static inline bool
snag_view_server_busy(const struct snag_view_server *server)
{
    (void)server;
    return false;
}

static inline void
snag_view_server_close(struct snag_view_server *server)
{
    (void)server;
}

static inline void
snag_view_server_step(struct snag_view_server *server)
{
    (void)server;
}

static inline void
snag_view_server_state(struct snag_view_server *server, const json_t *state)
{
    (void)server;
    (void)state;
}

static inline int
snag_view_server_result(struct snag_view_server *server, const char *id,
    const char *status, uint64_t seq, const char *event)
{
    (void)server;
    (void)id;
    (void)status;
    (void)seq;
    (void)event;
    return 0;
}

static inline bool
snag_view_server_attached(const struct snag_view_server *server)
{
    (void)server;
    return false;
}
#endif /* SNAJPAGENT_VM */

#endif
