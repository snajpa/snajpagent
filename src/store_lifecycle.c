/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_internal.h"
#include "fs.h"
#include "base.h"
#include "json.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static json_t *
origin_user_data(void)
{
    return json_pack("{s:s}", "origin", "user");
}

int
snag_session_archive(struct snag_session *session, uint64_t *written_seq,
                    char *error, size_t error_size)
{
    return snag_session_commit(session, "session_archived", origin_user_data(),
                              written_seq, error, error_size);
}

int
snag_session_unarchive(struct snag_session *session, uint64_t *written_seq,
                      char *error, size_t error_size)
{
    return snag_session_commit(session, "session_unarchived", origin_user_data(),
                              written_seq, error, error_size);
}

static json_t *
delete_request_data(const char *prefix, const char *trash_name)
{
    return json_pack("{s:s,s:s}", "confirmed_id_prefix", prefix,
                     "trash_name", trash_name);
}

static int
make_trash_name(const struct snag_session *session,
                char out[SNAG_TRASH_NAME_LEN + 1u], char *error,
                size_t error_size)
{
    char suffix[SNAG_TRASH_SUFFIX_HEX_LEN + 1u];
    if (snag_random_id(suffix) < 0) {
        snag_errorf(error, error_size, "cryptographic trash suffix generation failed");
        return -1;
    }
    (void)snprintf(out, SNAG_TRASH_NAME_LEN + 1u, "%s.%s", session->id, suffix);
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
    if (snag_unlink_at(dir_fd, name, false) == 0)
        return 0;
    if (optional && errno == ENOENT)
        return 0;
    snag_errorf(error, error_size, "cannot remove deleted-session %s: %s",
              name, strerror(errno));
    return -1;
}

int
snag_session_complete_delete(struct snag_store *store, struct snag_session *session,
                            char *error, size_t error_size)
{
    if (!store || !session || !session->delete_requested ||
        !session->trash_name[0] || session->dir_fd < 0) {
        snag_errorf(error, error_size, "no completed delete intent is open");
        errno = EINVAL;
        return -1;
    }
    if (snag_rename_at(store->sessions_fd, session->id,
                 store->trash_fd, session->trash_name) < 0) {
        snag_errorf(error, error_size, "cannot move session to trash: %s",
                  strerror(errno));
        return -1;
    }
    if (snag_sync_dir(store->sessions_fd) < 0 || snag_sync_dir(store->trash_fd) < 0) {
        snag_errorf(error, error_size, "cannot sync delete rename: %s",
                  strerror(errno));
        return -1;
    }
    if (close_fd_slot(&session->log_fd) < 0 ||
        close_fd_slot(&session->lock_fd) < 0) {
        snag_errorf(error, error_size, "cannot close deleted-session files: %s",
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
        snag_errorf(error, error_size, "cannot close deleted-session directory: %s",
                  strerror(errno));
        return -1;
    }
    if (snag_unlink_at(store->trash_fd, session->trash_name, true) < 0) {
        snag_errorf(error, error_size, "cannot remove deleted-session directory: %s",
                  strerror(errno));
        return -1;
    }
    if (snag_sync_dir(store->trash_fd) < 0) {
        snag_errorf(error, error_size, "cannot sync trash cleanup: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int
snag_store_complete_trash_delete(struct snag_store *store, const char *trash_name,
                                char *error, size_t error_size)
{
    struct snag_session session;
    char id[SNAG_ID_HEX_LEN + 1u];
    int dir_fd;
    int rc = -1;

    if (!snag_store_trash_id(trash_name, id)) {
        snag_errorf(error, error_size, "invalid deleted-session trash name");
        errno = EINVAL;
        return -1;
    }
    dir_fd = snag_open_read_security_at(store->trash_fd, trash_name, true);
    if (dir_fd < 0) {
        snag_errorf(error, error_size, "cannot open deleted-session trash: %s",
                  strerror(errno));
        return -1;
    }

    snag_session_init(&session);
    memcpy(session.id, id, sizeof(session.id));
    session.dir_fd = dir_fd;
    if (snag_store_verify_private_fd(session.dir_fd, true,
                                    "deleted-session directory", error,
                                    error_size) < 0 ||
        snag_store_open_session_files(&session, false, error, error_size) < 0 ||
        snag_store_scan_log(&session, SNAG_TAIL_REJECT, true,
                           error, error_size) < 0)
        goto out;
    if (!session.delete_requested || strcmp(session.trash_name, trash_name) != 0) {
        snag_errorf(error, error_size, "deleted-session trash intent mismatch");
        errno = EINVAL;
        goto out;
    }
    if (close_fd_slot(&session.log_fd) < 0 ||
        close_fd_slot(&session.lock_fd) < 0) {
        snag_errorf(error, error_size, "cannot close deleted-session files: %s",
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
        snag_errorf(error, error_size, "cannot close deleted-session directory: %s",
                  strerror(errno));
        goto out;
    }
    if (snag_unlink_at(store->trash_fd, trash_name, true) < 0) {
        snag_errorf(error, error_size, "cannot remove deleted-session directory: %s",
                  strerror(errno));
        goto out;
    }
    if (snag_sync_dir(store->trash_fd) < 0) {
        snag_errorf(error, error_size, "cannot sync trash cleanup: %s", strerror(errno));
        goto out;
    }
    rc = 0;
out:
    snag_session_close(&session);
    return rc;
}

int
snag_session_delete(struct snag_store *store, struct snag_session *session,
                   const char *confirmed_prefix, uint64_t *written_seq,
                   char *error, size_t error_size)
{
    char trash_name[SNAG_TRASH_NAME_LEN + 1u];

    if (!confirmed_prefix || strlen(confirmed_prefix) != 8u ||
        !snag_hex_is_lower(confirmed_prefix, 8u) ||
        memcmp(confirmed_prefix, session->id, 8u) != 0) {
        snag_errorf(error, error_size, "delete confirmation did not match session id");
        errno = EINVAL;
        return -1;
    }
    if (make_trash_name(session, trash_name, error, error_size) < 0)
        return -1;
    if (snag_session_commit(session, "session_delete_requested",
                           delete_request_data(confirmed_prefix, trash_name),
                           written_seq, error, error_size) < 0)
        return -1;
    return snag_session_complete_delete(store, session, error, error_size);
}
