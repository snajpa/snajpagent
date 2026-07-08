/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_internal.h"
#include "base.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
set_error(char *error, size_t size, const char *fmt, ...)
{
    va_list ap;
    if (!size)
        return;
    va_start(ap, fmt);
    (void)vsnprintf(error, size, fmt, ap);
    va_end(ap);
}

static bool
copy_small(char *dst, size_t size, const char *src)
{
    size_t len;
    if (!src)
        return false;
    len = strlen(src);
    if (len >= size)
        return false;
    memcpy(dst, src, len + 1u);
    return true;
}

struct resolved_session {
    char id[SNJ_ID_HEX_LEN + 1u];
    char trash_name[SNJ_TRASH_NAME_LEN + 1u];
    bool trash;
};

static DIR *
open_store_dir(struct snj_store *store, const char *name, const char *label,
               char *error, size_t error_size)
{
    int fd = openat(store->root_fd, name, O_RDONLY | O_DIRECTORY
#ifdef O_CLOEXEC
                    | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
                    | O_NOFOLLOW
#endif
    );
    DIR *dir;

    if (fd < 0)
        return NULL;
    if (snj_store_verify_private_fd(fd, true, label, error, error_size) < 0) {
        (void)close(fd);
        return NULL;
    }
    dir = fdopendir(fd);
    if (!dir) {
        (void)close(fd);
        return NULL;
    }
    return dir;
}

static DIR *
open_sessions_dir(struct snj_store *store, char *error, size_t error_size)
{
    return open_store_dir(store, "sessions", "sessions directory",
                          error, error_size);
}

static DIR *
open_trash_dir(struct snj_store *store, char *error, size_t error_size)
{
    return open_store_dir(store, "trash", "trash directory", error, error_size);
}

static bool
trash_name_id(const char *name, char id[SNJ_ID_HEX_LEN + 1u])
{
    if (strlen(name) != SNJ_TRASH_NAME_LEN || name[SNJ_ID_HEX_LEN] != '.' ||
        !snj_hex_is_lower(name + SNJ_ID_HEX_LEN + 1u,
                          SNJ_TRASH_SUFFIX_HEX_LEN))
        return false;
    memcpy(id, name, SNJ_ID_HEX_LEN);
    id[SNJ_ID_HEX_LEN] = '\0';
    return snj_hex_is_lower(id, SNJ_ID_HEX_LEN);
}

static void
record_resolved(struct resolved_session *target, const char *id,
                const char *trash_name, unsigned int *matches)
{
    if (*matches == 0u) {
        memcpy(target->id, id, SNJ_ID_HEX_LEN + 1u);
        target->trash = trash_name != NULL;
        if (trash_name)
            memcpy(target->trash_name, trash_name, SNJ_TRASH_NAME_LEN + 1u);
        else
            target->trash_name[0] = '\0';
    }
    ++*matches;
}

static int
resolve_prefix(struct snj_store *store, const char *prefix,
               struct resolved_session *target, char *error, size_t error_size)
{
    DIR *dir;
    struct dirent *entry;
    size_t len = strlen(prefix);
    unsigned int matches = 0;

    memset(target, 0, sizeof(*target));
    if (len < 8u || len > SNJ_ID_HEX_LEN || !snj_hex_is_lower(prefix, len)) {
        set_error(error, error_size, "session id must be 8..32 lowercase hex characters");
        errno = EINVAL;
        return -1;
    }
    dir = open_sessions_dir(store, error, error_size);
    if (!dir)
        return -1;
    while ((entry = readdir(dir)) != NULL) {
        if (strlen(entry->d_name) != SNJ_ID_HEX_LEN ||
            !snj_hex_is_lower(entry->d_name, SNJ_ID_HEX_LEN) ||
            strncmp(entry->d_name, prefix, len) != 0)
            continue;
        record_resolved(target, entry->d_name, NULL, &matches);
    }
    (void)closedir(dir);

    dir = open_trash_dir(store, error, error_size);
    if (!dir)
        return -1;
    while ((entry = readdir(dir)) != NULL) {
        char id[SNJ_ID_HEX_LEN + 1u];
        if (!trash_name_id(entry->d_name, id) ||
            strncmp(id, prefix, len) != 0)
            continue;
        record_resolved(target, id, entry->d_name, &matches);
    }
    (void)closedir(dir);

    if (matches != 1u) {
        set_error(error, error_size, matches ? "session id prefix is ambiguous" :
                  "session id was not found");
        errno = matches ? EEXIST : ENOENT;
        return -1;
    }
    return 0;
}

static int
open_full_id(struct snj_store *store, struct snj_session *session,
             const char *id, char *error, size_t error_size)
{
    char *sessions = NULL;

    memcpy(session->id, id, SNJ_ID_HEX_LEN + 1u);
    sessions = snj_store_path_join(store->root_path, "sessions");
    if (sessions) {
        session->dir_path = snj_store_path_join(sessions, id);
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
        set_error(error, error_size, "cannot open session %s: %s", id,
                  strerror(errno));
        return -1;
    }
    if (snj_store_verify_private_fd(session->dir_fd, true, "session directory",
                                    error, error_size) < 0)
        return -1;
    if (snj_store_open_session_files(session, false, error, error_size) < 0 ||
        snj_store_scan_log(session, SNJ_TAIL_TRUNCATE, true,
                           error, error_size) < 0)
        return -1;
    if (session->delete_requested) {
        if (snj_session_complete_delete(store, session, error, error_size) < 0)
            return -1;
        set_error(error, error_size, "session deletion was completed");
        return 1;
    }
    return 0;
}

int
snj_session_open(struct snj_store *store, struct snj_session *session,
                 const char *prefix, char *error, size_t error_size)
{
    struct resolved_session target;

    if (resolve_prefix(store, prefix, &target, error, error_size) < 0)
        return -1;
    if (target.trash) {
        if (snj_store_complete_trash_delete(store, target.trash_name,
                                            error, error_size) < 0)
            return -1;
        set_error(error, error_size, "session deletion was completed");
        return 1;
    }
    return open_full_id(store, session, target.id, error, error_size);
}

static int
open_snapshot(struct snj_store *store, struct snj_session *session,
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
    memcpy(session->id, id, SNJ_ID_HEX_LEN + 1u);
    sessions = snj_store_path_join(store->root_path, "sessions");
    if (sessions) {
        session->dir_path = snj_store_path_join(sessions, id);
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
    if (snj_store_verify_private_fd(session->dir_fd, true, "session directory",
                                    error, error_size) < 0)
        return -1;
    session->log_fd = openat(session->dir_fd, "events.jsonl", flags);
    if (session->log_fd < 0)
        return -1;
    if (snj_store_verify_private_fd(session->log_fd, false, "event log",
                                    error, error_size) < 0)
        return -1;
    session->log_end = lseek(session->log_fd, 0, SEEK_END);
    if (session->log_end < 0)
        return -1;
    return snj_store_scan_log(session, SNJ_TAIL_IGNORE, true,
                              error, error_size);
}

static int
session_matches(struct snj_store *store, const char *id, const char *workspace,
                bool all, bool include_archived, uint64_t *last,
                uint64_t *turns, char **first, char **saved_workspace,
                char *model, size_t model_size, bool *archived)
{
    struct snj_session tmp;
    char error[128];
    int rc = -1;

    snj_session_init(&tmp);
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
        *first = snj_strdup_checked(tmp.first_user, SNJ_MAX_DIRECT_PROMPT);
    if (saved_workspace)
        *saved_workspace = snj_strdup_checked(tmp.workspace, SNJ_PATH_MAX_BYTES);
    if ((saved_workspace && !*saved_workspace) ||
        !copy_small(model, model_size, tmp.default_model))
        goto out;
    rc = 0;
out:
    snj_session_close(&tmp);
    return rc;
}

int
snj_session_open_last(struct snj_store *store, struct snj_session *session,
                      const char *workspace, bool all, char *error,
                      size_t error_size)
{
    DIR *dir;
    struct dirent *entry;
    char best[SNJ_ID_HEX_LEN + 1u] = {0};
    uint64_t best_time = 0;

    dir = open_sessions_dir(store, error, error_size);
    if (!dir)
        return -1;
    while ((entry = readdir(dir)) != NULL) {
        uint64_t last, turns;
        char *first = NULL;
        char model[SNJ_MODEL_MAX_BYTES];
        if (strlen(entry->d_name) != SNJ_ID_HEX_LEN ||
            !snj_hex_is_lower(entry->d_name, SNJ_ID_HEX_LEN) ||
            session_matches(store, entry->d_name, workspace, all, false,
                            &last, &turns, &first, NULL, model,
                            sizeof(model), NULL) < 0) {
            free(first);
            continue;
        }
        free(first);
        if (!best[0] || last > best_time ||
            (last == best_time && strcmp(entry->d_name, best) > 0)) {
            memcpy(best, entry->d_name, sizeof(best));
            best_time = last;
        }
    }
    (void)closedir(dir);
    if (!best[0]) {
        set_error(error, error_size, "no matching active session");
        errno = ENOENT;
        return -1;
    }
    return open_full_id(store, session, best, error, error_size);
}

static int
store_list(struct snj_store *store, const char *workspace, bool all,
           bool include_archived, int fd, char *error, size_t error_size)
{
    DIR *dir;
    struct dirent *entry;
    unsigned int shown = 0;

    dir = open_sessions_dir(store, error, error_size);
    if (!dir)
        return -1;
    while ((entry = readdir(dir)) != NULL) {
        uint64_t last, turns;
        char *first = NULL;
        char *saved_workspace = NULL;
        char model[SNJ_MODEL_MAX_BYTES];
        bool archived = false;
        struct snj_buf row;
        if (strlen(entry->d_name) != SNJ_ID_HEX_LEN ||
            !snj_hex_is_lower(entry->d_name, SNJ_ID_HEX_LEN) ||
            session_matches(store, entry->d_name, workspace, all,
                            include_archived, &last, &turns, &first,
                            &saved_workspace, model, sizeof(model),
                            &archived) < 0) {
            free(first);
            free(saved_workspace);
            continue;
        }
        snj_buf_init(&row, 8192u);
        if (snj_buf_printf(&row, "%.8s\t%s\t%llu\t%s\t%s%s%s\n",
                           entry->d_name, model, (unsigned long long)turns,
                           archived ? "archived" : "active",
                           first ? first : "", all ? "\t" : "",
                           all ? saved_workspace : "") < 0 ||
            snj_write_full(fd, row.data, row.len) < 0) {
            snj_buf_free(&row);
            free(first);
            free(saved_workspace);
            (void)closedir(dir);
            set_error(error, error_size, "cannot write session list");
            return -1;
        }
        snj_buf_free(&row);
        free(first);
        free(saved_workspace);
        ++shown;
    }
    (void)closedir(dir);
    if (!shown)
        set_error(error, error_size, "no matching sessions");
    return 0;
}

int
snj_store_list_active(struct snj_store *store, const char *workspace, bool all,
                      int fd, char *error, size_t error_size)
{
    return store_list(store, workspace, all, false, fd, error, error_size);
}

int
snj_store_list(struct snj_store *store, const char *workspace, bool all, int fd,
               char *error, size_t error_size)
{
    return store_list(store, workspace, all, true, fd, error, error_size);
}
