/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_VM_WORKSPACE_H
#define SNAJPAGENT_VM_WORKSPACE_H

#include "fs.h"
#include "store.h"

struct snag_vm_workspace {
    struct snag_store *store;
    int root_fd, dir_fd;
    struct snag_directory_lock ownership;
    char id[SNAG_ID_HEX_LEN + 1u];
    json_t *snapshot;
    struct snag_vm_workspace *next_owned;
};

/* Workspace operations and close stay on the frontend thread that opened it. */
void snag_vm_workspace_init(struct snag_vm_workspace *);
void snag_vm_workspace_close(struct snag_vm_workspace *);
/* A workspace is created only when the frontend has useful state to retain.
 * State is the frontend's versioned layout/draft object; this layer owns its
 * private, atomic envelope and never opens an agent session or sends input. */
int snag_vm_workspace_create(struct snag_vm_workspace *, struct snag_store *,
    const char *name, const json_t *state, uint64_t activity_ms, char *, size_t);
/* NULL selector means newest valid workspace. Exact names precede ID prefixes.
 * Ambiguity reports the matching full IDs; a live workspace is never stolen. */
int snag_vm_workspace_open(struct snag_vm_workspace *, struct snag_store *,
    const char *selector, char *, size_t);
int snag_vm_workspace_save(struct snag_vm_workspace *, const char *name,
    const json_t *state, uint64_t activity_ms, char *, size_t);
/* Owned metadata array: all open/error rows, then up to stored_limit stored
 * rows, newest activity first. Listing does not create directories or snapshots.
 * Invalid snapshots produce error rows and remain untouched. */
json_t *snag_vm_workspace_list(struct snag_store *, uint64_t stored_limit, char *, size_t);

#endif
