/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_VM_CONNECTION_H
#define SNAJPAGENT_VM_CONNECTION_H

#include "session_view.h"
#include "store.h"
#include "vm_editor.h"

/* One buffer/lease per session, shared by every split displaying it. Drafts
 * and unresolved submissions outlive their windows and transport connections. */
struct snag_vm_connection {
    struct snag_vm_connection *next;
    struct snag_view_channel channel;
    char session[SNAG_ID_HEX_LEN + 1u], instance[SNAG_ID_HEX_LEN + 1u];
    struct snag_buf draft;
    struct snag_vm_editor editor;
    size_t cursor;
    json_t *pending, *state, *draft_base, *owner_draft, *draft_sent, *conflict_draft;
    json_t *reports;
    char message[256];
    uint64_t generation, deadline, receipt_at, revision;
    uint64_t draft_edit, draft_deadline;
    bool control, bound, hello, query, submitting, quitting, exited;
    bool commands, drafts, draft_ready, draft_dirty, draft_conflict, draft_get;
    bool terminal_commands, terminal_result, terminal_auto;
    bool send_pending, reconcile_pending, detaching, detach_sent;
};

struct snag_vm_connection *snag_vm_connection_new(const char *session);
void snag_vm_connections_free(struct snag_vm_connection *);
void snag_vm_connection_close(struct snag_vm_connection *);
int snag_vm_connection_open(struct snag_vm_connection *, struct snag_store *, bool control);
void snag_vm_connection_step(struct snag_vm_connection *);
bool snag_vm_connection_tail(const struct snag_vm_connection *, struct snag_journal_cursor *);
int snag_vm_connection_wait(const struct snag_vm_connection *, uint64_t now, int timeout);
int snag_vm_connection_control(struct snag_vm_connection *, const char *intent);
/* Prepare has no wire effects. Save the workspace before calling send. */
int snag_vm_connection_prepare(struct snag_vm_connection *);
int snag_vm_connection_send(struct snag_vm_connection *);
int snag_vm_connection_recover(struct snag_vm_connection *);
void snag_vm_connection_detach(struct snag_vm_connection *);
int snag_vm_draft_choose(struct snag_vm_connection *, bool local);
void snag_vm_draft_cursor(struct snag_vm_connection *, size_t cursor);
int snag_vm_draft_replace(struct snag_vm_connection *, size_t begin, size_t end,
    const void *text, size_t length);
json_t *snag_vm_connections_json(const struct snag_vm_connection *);
int snag_vm_connections_load(const json_t *, struct snag_vm_connection **);

#endif
