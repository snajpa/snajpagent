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

static int
make_trash_name(const struct snag_session *session,
                char out[SNAG_TRASH_NAME_LEN + 1u], char *error,
                size_t error_size)
{
    char suffix[SNAG_TRASH_SUFFIX_HEX_LEN + 1u];
    if (snag_random_id(suffix) < 0)
        return snag_errorf(error, error_size, "cryptographic trash suffix generation failed");
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
    return snag_errorf(error, error_size, "cannot remove deleted-session %s: %s",
              name, strerror(errno));
}

static int
remove_deleted_session(struct snag_store *store, struct snag_session *session,
                        char *error, size_t error_size)
{
    if (close_fd_slot(&session->log_fd) < 0)
        return snag_errorf(error, error_size, "cannot close deleted-session files: %s",
                  strerror(errno));
    /* Keep the durable delete intent until other content is gone. The private
     * trash name remains the deletion marker after the final log unlink. */
    if (unlink_expected_file(session->dir_fd, "prompt_history", true, error, error_size) < 0 ||
        unlink_expected_file(session->dir_fd, "meta.json", true, error, error_size) < 0 ||
        unlink_expected_file(session->dir_fd, "events.jsonl", true, error, error_size) < 0 ||
        snag_sync_dir(session->dir_fd) < 0 || close_fd_slot(&session->lock_fd) < 0 ||
        unlink_expected_file(session->dir_fd, "lock", true, error, error_size) < 0)
        return -1;
    if (close_fd_slot(&session->dir_fd) < 0)
        return snag_errorf(error, error_size, "cannot close deleted-session directory: %s",
                  strerror(errno));
    if (snag_unlink_at(store->trash_fd, session->trash_name, true) < 0)
        return snag_errorf(error, error_size, "cannot remove deleted-session directory: %s",
                  strerror(errno));
    if (snag_sync_dir(store->trash_fd) < 0)
        return snag_errorf(error, error_size, "cannot sync trash cleanup: %s", strerror(errno));
    return 0;
}

int
snag_session_complete_delete(struct snag_store *store, struct snag_session *session,
                            char *error, size_t error_size)
{
    if (!store || !session || !session->delete_requested ||
        !session->trash_name[0] || session->dir_fd < 0) {
        return snag_fail(error, error_size, EINVAL, "no completed delete intent is open");
    }
    if (snag_rename_at(store->sessions_fd, session->id,
                 store->trash_fd, session->trash_name) < 0)
        return snag_errorf(error, error_size, "cannot move session to trash: %s",
                  strerror(errno));
    if (snag_sync_dir(store->sessions_fd) < 0 || snag_sync_dir(store->trash_fd) < 0)
        return snag_errorf(error, error_size, "cannot sync delete rename: %s",
                  strerror(errno));
    return remove_deleted_session(store, session, error, error_size);
}

/* A crash may leave only the reserved trash directory and lock/metadata after
 * events.jsonl was removed. Never recursively remove unexpected content. */
static int
verify_delete_remnants(int dir_fd, char *error, size_t error_size)
{
    int fd = snag_open_read_security_at(dir_fd, ".", true);
    if (fd < 0) return -1;
    struct snag_directory *dir = snag_directory_open(fd);
    if (!dir) { (void)close(fd); return -1; }
    const char *name;
    int rc = 0;
    while ((name = snag_directory_next(dir)) != NULL) {
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (strcmp(name, "lock") && strcmp(name, "meta.json")) {
            rc = snag_fail(error, error_size, EINVAL, "unexpected content in deleted-session trash");
            break;
        }
        int file = snag_open_read_security_at(dir_fd, name, false);
        if (file < 0) { rc = -1; break; }
        rc = snag_store_verify_private_fd(file, false, "deleted-session remnant", error, error_size);
        (void)close(file);
        if (rc < 0) break;
    }
    if (!rc && errno) rc = -1;
    if (snag_directory_close(dir) < 0) rc = -1;
    return rc;
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
        return snag_fail(error, error_size, EINVAL, "invalid deleted-session trash name");
    }
    dir_fd = snag_open_read_security_at(store->trash_fd, trash_name, true);
    if (dir_fd < 0)
        return snag_errorf(error, error_size, "cannot open deleted-session trash: %s",
                  strerror(errno));

    snag_session_init(&session);
    memcpy(session.id, id, sizeof(session.id));
    session.dir_fd = dir_fd;
    if (snag_store_verify_private_fd(session.dir_fd, true,
                                    "deleted-session directory", error, error_size) < 0)
        goto out;
    if (snag_store_open_session_files(&session, false, error, error_size) < 0) {
        if (errno != ENOENT || session.lock_fd < 0 ||
            verify_delete_remnants(session.dir_fd, error, error_size) < 0) goto out;
        (void)snag_strcpy(session.trash_name, sizeof(session.trash_name), trash_name);
        if (error_size) error[0] = '\0';
        rc = remove_deleted_session(store, &session, error, error_size);
        goto out;
    }
    if (snag_store_scan_log(&session, SNAG_TAIL_REJECT, error, error_size) < 0)
        goto out;
    if (!session.delete_requested || strcmp(session.trash_name, trash_name) != 0) {
        (void)snag_fail(error, error_size, EINVAL, "deleted-session trash intent mismatch");
        goto out;
    }
    rc = remove_deleted_session(store, &session, error, error_size);
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
        return snag_fail(error, error_size, EINVAL, "delete confirmation did not match session id");
    }
    if (make_trash_name(session, trash_name, error, error_size) < 0)
        return -1;
    if (snag_session_commit(session, "session_delete_requested",
                           json_pack("{s:s,s:s}", "confirmed_id_prefix", confirmed_prefix,
                                     "trash_name", trash_name),
                           written_seq, error, error_size) < 0)
        return -1;
    return snag_session_complete_delete(store, session, error, error_size);
}
