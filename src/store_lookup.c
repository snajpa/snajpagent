/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_internal.h"
#include "base.h"
#include "fs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct resolved_session {
    char id[SNAG_ID_HEX_LEN + 1u];
    char trash_name[SNAG_TRASH_NAME_LEN + 1u];
    bool trash;
};

static struct snag_directory *
open_store_dir(struct snag_store *store, const char *name, const char *label,
               char *error, size_t error_size)
{
    int fd = snag_open_read_security_at(store->root_fd, name, true);
    struct snag_directory *dir;

    if (fd < 0)
        return NULL;
    if (snag_store_verify_private_fd(fd, true, label, error, error_size) < 0) {
        (void)close(fd);
        return NULL;
    }
    dir = snag_directory_open(fd);
    if (!dir) {
        (void)close(fd);
        return NULL;
    }
    return dir;
}

static struct snag_directory *
open_sessions_dir(struct snag_store *store, char *error, size_t error_size)
{
    return open_store_dir(store, "sessions", "sessions directory",
                          error, error_size);
}

static struct snag_directory *
open_trash_dir(struct snag_store *store, char *error, size_t error_size)
{
    return open_store_dir(store, "trash", "trash directory", error, error_size);
}

static int
finish_directory(struct snag_directory *dir, char *error, size_t error_size)
{
    int saved = errno;

    if (snag_directory_close(dir) < 0 && !saved)
        saved = errno;
    if (!saved)
        return 0;
    snag_errorf(error, error_size, "cannot read session directory: %s", strerror(saved));
    errno = saved;
    return -1;
}

bool
snag_store_trash_id(const char *name, char id[SNAG_ID_HEX_LEN + 1u])
{
    if (!name || strlen(name) != SNAG_TRASH_NAME_LEN || name[SNAG_ID_HEX_LEN] != '.' ||
        !snag_hex_is_lower(name + SNAG_ID_HEX_LEN + 1u,
                          SNAG_TRASH_SUFFIX_HEX_LEN))
        return false;
    memcpy(id, name, SNAG_ID_HEX_LEN);
    id[SNAG_ID_HEX_LEN] = '\0';
    return snag_hex_is_lower(id, SNAG_ID_HEX_LEN);
}

static void
record_resolved(struct resolved_session *target, const char *id,
                const char *trash_name, unsigned int *matches)
{
    if (*matches == 0u) {
        memcpy(target->id, id, SNAG_ID_HEX_LEN + 1u);
        target->trash = trash_name != NULL;
        if (trash_name)
            memcpy(target->trash_name, trash_name, SNAG_TRASH_NAME_LEN + 1u);
        else
            target->trash_name[0] = '\0';
    }
    ++*matches;
}

static int
resolve_prefix(struct snag_store *store, const char *prefix,
               struct resolved_session *target, char *error, size_t error_size)
{
    struct snag_directory *dir;
    const char *entry;
    size_t len = strlen(prefix);
    unsigned int matches = 0;

    memset(target, 0, sizeof(*target));
    if (len < 8u || len > SNAG_ID_HEX_LEN || !snag_hex_is_lower(prefix, len)) {
        snag_errorf(error, error_size, "session id must be 8..32 lowercase hex characters");
        errno = EINVAL;
        return -1;
    }
    dir = open_sessions_dir(store, error, error_size);
    if (!dir)
        return -1;
    while ((entry = snag_directory_next(dir)) != NULL) {
        if (strlen(entry) != SNAG_ID_HEX_LEN ||
            !snag_hex_is_lower(entry, SNAG_ID_HEX_LEN) ||
            strncmp(entry, prefix, len) != 0)
            continue;
        record_resolved(target, entry, NULL, &matches);
    }
    if (finish_directory(dir, error, error_size) < 0)
        return -1;

    dir = open_trash_dir(store, error, error_size);
    if (!dir)
        return -1;
    while ((entry = snag_directory_next(dir)) != NULL) {
        char id[SNAG_ID_HEX_LEN + 1u];
        if (!snag_store_trash_id(entry, id) ||
            strncmp(id, prefix, len) != 0)
            continue;
        record_resolved(target, id, entry, &matches);
    }
    if (finish_directory(dir, error, error_size) < 0)
        return -1;

    if (matches != 1u) {
        snag_errorf(error, error_size, matches ? "session id prefix is ambiguous" :
                  "session id was not found");
        errno = matches ? EEXIST : ENOENT;
        return -1;
    }
    return 0;
}

static int
open_session_dir(struct snag_store *store, struct snag_session *session,
                  const char *id, char *error, size_t error_size)
{
    char *sessions = NULL;

    memcpy(session->id, id, SNAG_ID_HEX_LEN + 1u);
    sessions = snag_path_join(store->root_path, "sessions");
    if (sessions) {
        session->dir_path = snag_path_join(sessions, id);
        free(sessions);
    }
    if (!session->dir_path)
        return -1;
    session->dir_fd = snag_open_read_security_at(store->sessions_fd, id, true);
    if (session->dir_fd < 0) {
        snag_errorf(error, error_size, "cannot open session %s: %s", id,
                  strerror(errno));
        return -1;
    }
    return snag_store_verify_private_fd(session->dir_fd, true, "session directory",
                                        error, error_size);
}

static int
open_full_id(struct snag_store *store, struct snag_session *session,
             const char *id, char *error, size_t error_size)
{
    if (open_session_dir(store, session, id, error, error_size) < 0)
        return -1;
    if (snag_store_open_session_files(session, false, error, error_size) < 0 ||
        snag_store_scan_log(session, SNAG_TAIL_TRUNCATE,
                           error, error_size) < 0)
        return -1;
    if (session->delete_requested) {
        if (snag_session_complete_delete(store, session, error, error_size) < 0)
            return -1;
        snag_errorf(error, error_size, "session deletion was completed");
        return 1;
    }
    return 0;
}

int
snag_session_open(struct snag_store *store, struct snag_session *session,
                 const char *prefix, char *error, size_t error_size)
{
    struct resolved_session target;

    if (resolve_prefix(store, prefix, &target, error, error_size) < 0)
        return -1;
    if (target.trash) {
        if (snag_store_complete_trash_delete(store, target.trash_name,
                                            error, error_size) < 0)
            return -1;
        snag_errorf(error, error_size, "session deletion was completed");
        return 1;
    }
    return open_full_id(store, session, target.id, error, error_size);
}

static int
open_snapshot(struct snag_store *store, struct snag_session *session,
              const char *id, char *error, size_t error_size)
{
    if (open_session_dir(store, session, id, error, error_size) < 0)
        return -1;
    session->log_fd = snag_open_read_security_at(session->dir_fd, "events.jsonl", false);
    if (session->log_fd < 0)
        return -1;
    if (snag_store_verify_private_fd(session->log_fd, false, "event log",
                                    error, error_size) < 0)
        return -1;
    session->log_end = snag_seek(session->log_fd, 0, SEEK_END);
    if (session->log_end < 0)
        return -1;
    return snag_store_scan_log(session, SNAG_TAIL_IGNORE,
                              error, error_size);
}

/* On success the caller owns the snapshot; no selected-field copies. */
static int
matching_snapshot(struct snag_store *store, struct snag_session *snapshot,
                   const char *id, const char *workspace,
                   bool all, bool include_archived)
{
    char error[128];

    snag_session_init(snapshot);
    if (strlen(id) == SNAG_ID_HEX_LEN && snag_hex_is_lower(id, SNAG_ID_HEX_LEN) &&
        open_snapshot(store, snapshot, id, error, sizeof(error)) == 0 &&
        !snapshot->delete_requested && (include_archived || !snapshot->archived) &&
        (all || strcmp(snapshot->workspace, workspace) == 0))
        return 0;
    snag_session_close(snapshot);
    return -1;
}

int
snag_session_open_last(struct snag_store *store, struct snag_session *session,
                      const char *workspace, bool all, char *error,
                      size_t error_size)
{
    struct snag_directory *dir;
    const char *entry;
    char best[SNAG_ID_HEX_LEN + 1u] = {0};
    uint64_t best_time = 0;

    dir = open_sessions_dir(store, error, error_size);
    if (!dir)
        return -1;
    while ((entry = snag_directory_next(dir)) != NULL) {
        struct snag_session snapshot;
        if (matching_snapshot(store, &snapshot, entry, workspace, all, false) < 0)
            continue;
        uint64_t last = snapshot.last_time_ms;
        if (!best[0] || last > best_time ||
            (last == best_time && strcmp(entry, best) > 0)) {
            memcpy(best, entry, sizeof(best));
            best_time = last;
        }
        snag_session_close(&snapshot);
    }
    if (finish_directory(dir, error, error_size) < 0)
        return -1;
    if (!best[0]) {
        snag_errorf(error, error_size, "no matching active session");
        errno = ENOENT;
        return -1;
    }
    return open_full_id(store, session, best, error, error_size);
}

int
snag_store_list(struct snag_store *store, const char *workspace, bool all,
                bool include_archived, snag_store_emit_fn emit, void *opaque,
                char *error, size_t error_size)
{
    struct snag_directory *dir;
    const char *entry;
    unsigned int shown = 0;

    dir = open_sessions_dir(store, error, error_size);
    if (!dir)
        return -1;
    while ((entry = snag_directory_next(dir)) != NULL) {
        struct snag_session snapshot;
        struct snag_buf row;
        if (matching_snapshot(store, &snapshot, entry, workspace,
                               all, include_archived) < 0)
            continue;
        snag_buf_init(&row, 8192u);
        if (snag_buf_printf(&row, "%.8s\t%s\t%llu\t%s\t%s%s%s\n",
                           entry, snapshot.default_model,
                           (unsigned long long)snapshot.turn_count,
                           snapshot.archived ? "archived" : "active",
                           snapshot.first_user ? snapshot.first_user : "",
                           all ? "\t" : "", all ? snapshot.workspace : "") < 0 ||
            emit(opaque, (const char *)row.data, row.len) < 0) {
            snag_buf_free(&row);
            snag_session_close(&snapshot);
            (void)snag_directory_close(dir);
            snag_errorf(error, error_size, "cannot write session list");
            return -1;
        }
        snag_buf_free(&row);
        snag_session_close(&snapshot);
        ++shown;
    }
    if (finish_directory(dir, error, error_size) < 0)
        return -1;
    if (!shown)
        snag_errorf(error, error_size, "no matching sessions");
    return 0;
}
