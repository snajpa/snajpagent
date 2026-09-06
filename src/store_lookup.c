/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_internal.h"
#include "base.h"
#include "fs.h"

#include <errno.h>
#include <fcntl.h>
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
    int fd = snag_open_private_dir_at(store->root_fd, name);
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
    (void)snag_directory_close(dir);

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
    (void)snag_directory_close(dir);

    if (matches != 1u) {
        snag_errorf(error, error_size, matches ? "session id prefix is ambiguous" :
                  "session id was not found");
        errno = matches ? EEXIST : ENOENT;
        return -1;
    }
    return 0;
}

static int
open_full_id(struct snag_store *store, struct snag_session *session,
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
    session->dir_fd = openat(store->sessions_fd, id, O_RDONLY | O_DIRECTORY
#ifdef O_CLOEXEC
                             | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
                             | O_NOFOLLOW
#endif
    );
    if (session->dir_fd < 0) {
        snag_errorf(error, error_size, "cannot open session %s: %s", id,
                  strerror(errno));
        return -1;
    }
    if (snag_store_verify_private_fd(session->dir_fd, true, "session directory",
                                    error, error_size) < 0)
        return -1;
    if (snag_store_open_session_files(session, false, error, error_size) < 0 ||
        snag_store_scan_log(session, SNAG_TAIL_TRUNCATE, true,
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
    char *sessions = NULL;
    int flags = O_RDONLY;

#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    memcpy(session->id, id, SNAG_ID_HEX_LEN + 1u);
    sessions = snag_path_join(store->root_path, "sessions");
    if (sessions) {
        session->dir_path = snag_path_join(sessions, id);
        free(sessions);
    }
    if (!session->dir_path)
        return -1;
    session->dir_fd = openat(store->sessions_fd, id, O_RDONLY | O_DIRECTORY
#ifdef O_CLOEXEC
                             | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
                             | O_NOFOLLOW
#endif
    );
    if (session->dir_fd < 0)
        return -1;
    if (snag_store_verify_private_fd(session->dir_fd, true, "session directory",
                                    error, error_size) < 0)
        return -1;
    session->log_fd = openat(session->dir_fd, "events.jsonl", flags);
    if (session->log_fd < 0)
        return -1;
    if (snag_store_verify_private_fd(session->log_fd, false, "event log",
                                    error, error_size) < 0)
        return -1;
    session->log_end = lseek(session->log_fd, 0, SEEK_END);
    if (session->log_end < 0)
        return -1;
    return snag_store_scan_log(session, SNAG_TAIL_IGNORE, true,
                              error, error_size);
}

static int
session_matches(struct snag_store *store, const char *id, const char *workspace,
                bool all, bool include_archived, uint64_t *last,
                uint64_t *turns, char **first, char **saved_workspace,
                char *model, size_t model_size, bool *archived)
{
    struct snag_session tmp;
    char error[128];
    int rc = -1;

    snag_session_init(&tmp);
    if (open_snapshot(store, &tmp, id, error, sizeof(error)) < 0)
        goto out;
    if (tmp.delete_requested || (!include_archived && tmp.archived) ||
        (!all && strcmp(tmp.workspace, workspace) != 0))
        goto out;
    *last = tmp.last_time_ms;
    *turns = tmp.turn_count;
    if (archived)
        *archived = tmp.archived;
    if (tmp.first_user)
        *first = snag_strdup_checked(tmp.first_user, SNAG_MAX_DIRECT_PROMPT);
    if (saved_workspace)
        *saved_workspace = snag_strdup_checked(tmp.workspace, SNAG_PATH_MAX_BYTES);
    if ((saved_workspace && !*saved_workspace) ||
        !snag_strcpy(model, model_size, tmp.default_model))
        goto out;
    rc = 0;
out:
    snag_session_close(&tmp);
    return rc;
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
        uint64_t last, turns;
        char *first = NULL;
        char model[SNAG_MODEL_MAX_BYTES];
        if (strlen(entry) != SNAG_ID_HEX_LEN ||
            !snag_hex_is_lower(entry, SNAG_ID_HEX_LEN) ||
            session_matches(store, entry, workspace, all, false,
                            &last, &turns, &first, NULL, model,
                            sizeof(model), NULL) < 0) {
            free(first);
            continue;
        }
        free(first);
        if (!best[0] || last > best_time ||
            (last == best_time && strcmp(entry, best) > 0)) {
            memcpy(best, entry, sizeof(best));
            best_time = last;
        }
    }
    (void)snag_directory_close(dir);
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
        uint64_t last, turns;
        char *first = NULL;
        char *saved_workspace = NULL;
        char model[SNAG_MODEL_MAX_BYTES];
        bool archived = false;
        struct snag_buf row;
        if (strlen(entry) != SNAG_ID_HEX_LEN ||
            !snag_hex_is_lower(entry, SNAG_ID_HEX_LEN) ||
            session_matches(store, entry, workspace, all,
                            include_archived, &last, &turns, &first,
                            &saved_workspace, model, sizeof(model),
                            &archived) < 0) {
            free(first);
            free(saved_workspace);
            continue;
        }
        snag_buf_init(&row, 8192u);
        if (snag_buf_printf(&row, "%.8s\t%s\t%llu\t%s\t%s%s%s\n",
                           entry, model, (unsigned long long)turns,
                           archived ? "archived" : "active",
                           first ? first : "", all ? "\t" : "",
                           all ? saved_workspace : "") < 0 ||
            emit(opaque, (const char *)row.data, row.len) < 0) {
            snag_buf_free(&row);
            free(first);
            free(saved_workspace);
            (void)snag_directory_close(dir);
            snag_errorf(error, error_size, "cannot write session list");
            return -1;
        }
        snag_buf_free(&row);
        free(first);
        free(saved_workspace);
        ++shown;
    }
    (void)snag_directory_close(dir);
    if (!shown)
        snag_errorf(error, error_size, "no matching sessions");
    return 0;
}
