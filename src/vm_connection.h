/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_VM_CONNECTION_H
#define SNAJPAGENT_VM_CONNECTION_H

#include "session_view.h"
#include "store.h"
#include "vm_editor.h"

struct snag_vm_connection;
/* Windows share a conversation's editor and pending submission. Its immutable
 * route keeps unsent text pinned across focus, nick and connection changes. */
struct snag_vm_buffer {
    struct snag_vm_buffer *next;
    struct snag_vm_connection *connection;
    json_t *route;
    struct snag_buf draft;
    struct snag_vm_editor editor;
    size_t cursor;
    json_t *pending, *draft_base, *owner_draft, *conflict_draft;
    json_t *report_open, *selection;
    char message[256];
    char endpoint[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    uint64_t receipt_at, request_window;
    uint64_t read_seq, read_received, read_after;
    bool query, submitting, send_pending, reconcile_pending;
    bool draft_ready, draft_dirty, draft_conflict, draft_get;
    bool terminal_result, terminal_auto;
};

struct snag_vm_activity {
    uint64_t after, seq, time, received, incoming, unread;
    bool known, exact;
};

void snag_vm_buffer_activity(const struct snag_vm_buffer *, struct snag_vm_activity *);
bool snag_vm_buffer_read(struct snag_vm_buffer *, uint64_t);
const json_t *snag_vm_buffer_state(const struct snag_vm_buffer *);

/* One transport and controller lease per session, shared by all buffers. */
struct snag_vm_connection {
    struct snag_vm_connection *next;
    struct snag_view_channel channel;
    char session[SNAG_ID_HEX_LEN + 1u], instance[SNAG_ID_HEX_LEN + 1u];
    struct snag_vm_buffer *buffers, *rollout, *draft_wait, *inflight, *sync_next;
    json_t *state, *draft_sent, *reports;
    char message[256];
    uint64_t generation, deadline, revision, draft_edit, draft_deadline;
    bool control, bound, hello, quitting, exited;
    bool commands, drafts, terminal_commands, irc_queries, irc_channels, irc_connections;
    bool reports_supported, reports_subscribed, detaching, detach_sent;
};

/* NULL selects the rollout. Returned buffers live until their session closes. */
struct snag_vm_buffer *snag_vm_buffer_get(struct snag_vm_connection *, const json_t *, bool);
bool snag_vm_buffer_writable(const struct snag_vm_buffer *);
bool snag_vm_buffer_supported(const struct snag_vm_buffer *);
bool snag_vm_connection_unsaved(const struct snag_vm_connection *);
void snag_vm_connection_discard(struct snag_vm_connection *);

struct snag_vm_connection *snag_vm_connection_new(const char *session);
void snag_vm_connections_free(struct snag_vm_connection *);
void snag_vm_connection_close(struct snag_vm_connection *);
int snag_vm_connection_open(struct snag_vm_connection *, struct snag_store *, bool control);
void snag_vm_connection_step(struct snag_vm_connection *);
bool snag_vm_connection_tail(const struct snag_vm_connection *, struct snag_journal_cursor *);
int snag_vm_connection_wait(const struct snag_vm_connection *, uint64_t now, int timeout);
int snag_vm_connection_control(struct snag_vm_connection *, const char *intent);
/* Prepare has no wire effects. Save the workspace before calling send. */
int snag_vm_buffer_prepare(struct snag_vm_buffer *, uint64_t window);
int snag_vm_buffer_send(struct snag_vm_buffer *);
int snag_vm_buffer_recover(struct snag_vm_buffer *);
void snag_vm_connection_detach(struct snag_vm_connection *);
int snag_vm_draft_choose(struct snag_vm_buffer *, bool local);
void snag_vm_draft_cursor(struct snag_vm_buffer *, size_t cursor);
int snag_vm_draft_replace(struct snag_vm_buffer *, size_t begin, size_t end,
    const void *text, size_t length);
json_t *snag_vm_connections_json(const struct snag_vm_connection *);
int snag_vm_connections_load(const json_t *, struct snag_vm_connection **);
/* known is the reference snapshot taken when the catalogue read began.
 * Notifications arriving after it remain newer than that disk snapshot. */
int snag_vm_reports_merge(struct snag_vm_connection *, const json_t *catalog,
    const json_t *known);

#endif
