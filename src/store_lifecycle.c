/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_internal.h"
#include "base.h"
#include "json.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
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

static json_t *
origin_user_data(void)
{
    json_t *data = json_object();
    if (!data || snj_json_set_new(data, "origin", json_string("user")) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
}

int
snj_session_archive(struct snj_session *session, uint64_t *written_seq,
                    char *error, size_t error_size)
{
    return snj_session_commit(session, "session_archived", origin_user_data(),
                              written_seq, error, error_size);
}

int
snj_session_unarchive(struct snj_session *session, uint64_t *written_seq,
                      char *error, size_t error_size)
{
    return snj_session_commit(session, "session_unarchived", origin_user_data(),
                              written_seq, error, error_size);
}

static json_t *
delete_request_data(const char *prefix, const char *trash_name)
{
    json_t *data = json_object();
    if (!data ||
        snj_json_set_new(data, "confirmed_id_prefix", json_string(prefix)) < 0 ||
        snj_json_set_new(data, "trash_name", json_string(trash_name)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
}

static int
make_trash_name(const struct snj_session *session,
                char out[SNJ_TRASH_NAME_LEN + 1u], char *error,
                size_t error_size)
{
    char suffix[SNJ_TRASH_SUFFIX_HEX_LEN + 1u];
    if (snj_random_id(suffix) < 0) {
        set_error(error, error_size, "cryptographic trash suffix generation failed");
        return -1;
    }
    (void)snprintf(out, SNJ_TRASH_NAME_LEN + 1u, "%s.%s", session->id, suffix);
    return 0;
}

static int
close_fd_slot(int *fd)
{
    int rc = 0;
    if (*fd >= 0 && close(*fd) < 0)
        rc = -1;
    *fd = -1;
    return rc;
}

static int
unlink_expected_file(int dir_fd, const char *name, bool optional,
                     char *error, size_t error_size)
{
    if (unlinkat(dir_fd, name, 0) == 0)
        return 0;
    if (optional && errno == ENOENT)
        return 0;
    set_error(error, error_size, "cannot remove deleted-session %s: %s",
              name, strerror(errno));
    return -1;
}

int
snj_session_complete_delete(struct snj_store *store, struct snj_session *session,
                            char *error, size_t error_size)
{
    if (!store || !session || !session->delete_requested ||
        !session->trash_name[0] || session->dir_fd < 0) {
        set_error(error, error_size, "no completed delete intent is open");
        errno = EINVAL;
        return -1;
    }
    if (renameat(store->sessions_fd, session->id,
                 store->trash_fd, session->trash_name) < 0) {
        set_error(error, error_size, "cannot move session to trash: %s",
                  strerror(errno));
        return -1;
    }
    if (snj_sync_dir(store->sessions_fd) < 0 || snj_sync_dir(store->trash_fd) < 0) {
        set_error(error, error_size, "cannot sync delete rename: %s",
                  strerror(errno));
        return -1;
    }
    if (close_fd_slot(&session->log_fd) < 0 ||
        close_fd_slot(&session->lock_fd) < 0) {
        set_error(error, error_size, "cannot close deleted-session files: %s",
                  strerror(errno));
        return -1;
    }
    if (unlink_expected_file(session->dir_fd, "events.jsonl", false,
                             error, error_size) < 0 ||
        unlink_expected_file(session->dir_fd, "meta.json", true,
                             error, error_size) < 0 ||
        unlink_expected_file(session->dir_fd, "lock", false,
                             error, error_size) < 0)
        return -1;
    if (close_fd_slot(&session->dir_fd) < 0) {
        set_error(error, error_size, "cannot close deleted-session directory: %s",
                  strerror(errno));
        return -1;
    }
    if (unlinkat(store->trash_fd, session->trash_name, AT_REMOVEDIR) < 0) {
        set_error(error, error_size, "cannot remove deleted-session directory: %s",
                  strerror(errno));
        return -1;
    }
    if (snj_sync_dir(store->trash_fd) < 0) {
        set_error(error, error_size, "cannot sync trash cleanup: %s", strerror(errno));
        return -1;
    }
    return 0;
}


static bool
trash_basename_id(const char *name, char id[SNJ_ID_HEX_LEN + 1u])
{
    if (!name || strlen(name) != SNJ_TRASH_NAME_LEN ||
        name[SNJ_ID_HEX_LEN] != '.' ||
        !snj_hex_is_lower(name + SNJ_ID_HEX_LEN + 1u,
                          SNJ_TRASH_SUFFIX_HEX_LEN))
        return false;
    memcpy(id, name, SNJ_ID_HEX_LEN);
    id[SNJ_ID_HEX_LEN] = '\0';
    return snj_hex_is_lower(id, SNJ_ID_HEX_LEN);
}

int
snj_store_complete_trash_delete(struct snj_store *store, const char *trash_name,
                                char *error, size_t error_size)
{
    struct snj_session session;
    char id[SNJ_ID_HEX_LEN + 1u];
    int dir_fd;
    int rc = -1;

    if (!trash_basename_id(trash_name, id)) {
        set_error(error, error_size, "invalid deleted-session trash name");
        errno = EINVAL;
        return -1;
    }
    dir_fd = openat(store->trash_fd, trash_name, O_RDONLY | O_DIRECTORY
#ifdef O_CLOEXEC
                    | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
                    | O_NOFOLLOW
#endif
    );
    if (dir_fd < 0) {
        set_error(error, error_size, "cannot open deleted-session trash: %s",
                  strerror(errno));
        return -1;
    }

    snj_session_init(&session);
    memcpy(session.id, id, sizeof(session.id));
    session.dir_fd = dir_fd;
    if (snj_store_verify_private_fd(session.dir_fd, true,
                                    "deleted-session directory", error,
                                    error_size) < 0 ||
        snj_store_open_session_files(&session, false, error, error_size) < 0 ||
        snj_store_scan_log(&session, SNJ_TAIL_REJECT, true,
                           error, error_size) < 0)
        goto out;
    if (!session.delete_requested || strcmp(session.trash_name, trash_name) != 0) {
        set_error(error, error_size, "deleted-session trash intent mismatch");
        errno = EINVAL;
        goto out;
    }
    if (close_fd_slot(&session.log_fd) < 0 ||
        close_fd_slot(&session.lock_fd) < 0) {
        set_error(error, error_size, "cannot close deleted-session files: %s",
                  strerror(errno));
        goto out;
    }
    if (unlink_expected_file(session.dir_fd, "events.jsonl", false,
                             error, error_size) < 0 ||
        unlink_expected_file(session.dir_fd, "meta.json", true,
                             error, error_size) < 0 ||
        unlink_expected_file(session.dir_fd, "lock", false,
                             error, error_size) < 0)
        goto out;
    if (close_fd_slot(&session.dir_fd) < 0) {
        set_error(error, error_size, "cannot close deleted-session directory: %s",
                  strerror(errno));
        goto out;
    }
    if (unlinkat(store->trash_fd, trash_name, AT_REMOVEDIR) < 0) {
        set_error(error, error_size, "cannot remove deleted-session directory: %s",
                  strerror(errno));
        goto out;
    }
    if (snj_sync_dir(store->trash_fd) < 0) {
        set_error(error, error_size, "cannot sync trash cleanup: %s", strerror(errno));
        goto out;
    }
    rc = 0;
out:
    snj_session_close(&session);
    return rc;
}

int
snj_session_delete(struct snj_store *store, struct snj_session *session,
                   const char *confirmed_prefix, uint64_t *written_seq,
                   char *error, size_t error_size)
{
    char trash_name[SNJ_TRASH_NAME_LEN + 1u];

    if (!confirmed_prefix || strlen(confirmed_prefix) != 8u ||
        !snj_hex_is_lower(confirmed_prefix, 8u) ||
        memcmp(confirmed_prefix, session->id, 8u) != 0) {
        set_error(error, error_size, "delete confirmation did not match session id");
        errno = EINVAL;
        return -1;
    }
    if (make_trash_name(session, trash_name, error, error_size) < 0)
        return -1;
    if (snj_session_commit(session, "session_delete_requested",
                           delete_request_data(confirmed_prefix, trash_name),
                           written_seq, error, error_size) < 0)
        return -1;
    return snj_session_complete_delete(store, session, error, error_size);
}
