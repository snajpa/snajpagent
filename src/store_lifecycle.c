/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_internal.h"
#include "media.h"
#include "upload.h"
#include "fs.h"
#include "base.h"
#include "json.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef _WIN32
/* A live transfer holds the session lock. Once that lock is reclaimed, only
 * exactly shaped, private operation scratch may be removed on resume/delete. */
static int
remove_upload_stage(int session_fd, const char *stage, char *error, size_t error_size)
{
    char leaves[SNAG_UPLOAD_FILES_MAX][SNAG_ID_HEX_LEN + 1u];
    struct snag_file_privacy privacy;
    struct snag_directory *dir = NULL;
    size_t count = 0u;
    int stage_fd = -1, scan_fd = -1;
    const char *name;

    stage_fd = snag_open_read_at(session_fd, stage, true);
    if (stage_fd < 0 || snag_fd_privacy(stage_fd, &privacy) < 0) goto failed;
    if (!privacy.effective_owner || !privacy.private_access) { errno = EPERM; goto failed; }
    scan_fd = snag_open_read_at(stage_fd, ".", true);
    if (scan_fd < 0) goto failed;
    dir = snag_directory_open(scan_fd);
    if (!dir) goto failed;
    scan_fd = -1;
    for (;;) {
        snag_file_info info;
        int file_fd;

        errno = 0;
        name = snag_directory_next(dir);
        if (!name) {
            if (errno) goto failed;
            break;
        }
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (strlen(name) != SNAG_ID_HEX_LEN || !snag_hex_is_lower(name, SNAG_ID_HEX_LEN) ||
            count == SNAG_UPLOAD_FILES_MAX) { errno = EPERM; goto failed; }
        file_fd = snag_open_read_at(stage_fd, name, false);
        if (file_fd < 0) goto failed;
        if (snag_fstat(file_fd, &info) < 0 || snag_fd_privacy(file_fd, &privacy) < 0 ||
            !S_ISREG(info.st_mode) || info.st_nlink != 1 ||
            info.st_size < 0 || info.st_size > SNAG_MEDIA_FILE_MAX ||
            !privacy.effective_owner || !privacy.private_access) {
            (void)close(file_fd);
            errno = EPERM;
            goto failed;
        }
        if (close(file_fd) < 0) goto failed;
        memcpy(leaves[count], name, SNAG_ID_HEX_LEN + 1u);
        ++count;
    }
    if (snag_directory_close(dir) < 0) { dir = NULL; goto failed; }
    dir = NULL;
    for (size_t i = 0u; i < count; ++i)
        if (snag_unlink_at(stage_fd, leaves[i], false) < 0) goto failed;
    if (snag_sync_dir(stage_fd) < 0) goto failed;
    if (close(stage_fd) < 0) {
        stage_fd = -1;
        goto failed;
    }
    stage_fd = -1;
    if (snag_unlink_at(session_fd, stage, true) < 0 || snag_sync_dir(session_fd) < 0) goto failed;
    return 0;
failed: {
    int saved_errno = errno;
    if (dir) (void)snag_directory_close(dir);
    if (scan_fd >= 0) (void)close(scan_fd);
    if (stage_fd >= 0) (void)close(stage_fd);
    return snag_errorf(error, error_size, "cannot safely clean upload staging %s: %s",
                       stage, strerror(saved_errno));
}
}
#endif

int
snag_store_remove_upload_staging(int session_fd, char *error, size_t error_size)
{
#ifdef _WIN32
    (void)session_fd; (void)error; (void)error_size;
    return 0; /* Terminal upload does not create staging on Windows. */
#else
    for (;;) {
        char stage[SNAG_ID_HEX_LEN + 8u] = {0};
        struct snag_directory *dir;
        const char *name;
        int scan_fd = snag_open_read_at(session_fd, ".", true);
        int scan_error = 0;

        if (scan_fd < 0)
            return snag_errorf(error, error_size, "cannot inspect upload staging: %s",
                               strerror(errno));
        dir = snag_directory_open(scan_fd);
        if (!dir) {
            int saved_errno = errno;
            (void)close(scan_fd);
            return snag_errorf(error, error_size, "cannot inspect upload staging: %s",
                               strerror(saved_errno));
        }
        for (;;) {
            errno = 0;
            name = snag_directory_next(dir);
            if (!name) { scan_error = errno; break; }
            if (strncmp(name, "upload-", 7u) || strlen(name) != 7u + SNAG_ID_HEX_LEN ||
                !snag_hex_is_lower(name + 7u, SNAG_ID_HEX_LEN)) continue;
            memcpy(stage, name, sizeof(stage));
            break;
        }
        if (snag_directory_close(dir) < 0 && !scan_error) scan_error = errno;
        if (scan_error)
            return snag_errorf(error, error_size, "cannot inspect upload staging: %s",
                               strerror(scan_error));
        if (!stage[0]) return 0;
        if (remove_upload_stage(session_fd, stage, error, error_size) < 0) return -1;
    }
#endif
}

static json_t *
origin_user_data(void)
{
    return json_pack("{s:s}", "origin", "user");
}

int
snag_session_archive(struct snag_session *session, uint64_t *written_seq, char *error, size_t error_size)
{
    return snag_session_commit(session, "session_archived", origin_user_data(),
                              written_seq, error, error_size);
}

int
snag_session_unarchive(struct snag_session *session, uint64_t *written_seq, char *error, size_t error_size)
{
    return snag_session_commit(session, "session_unarchived", origin_user_data(),
                              written_seq, error, error_size);
}

static int
make_trash_name(const struct snag_session *session, char out[SNAG_TRASH_NAME_LEN + 1u], char *error,
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
    if (*fd >= 0 && close(*fd) < 0) rc = -1;
    *fd = -1;
    return rc;
}

static int
unlink_expected_file(int dir_fd, const char *name, bool optional, char *error, size_t error_size)
{
    if (snag_unlink_at(dir_fd, name, false) == 0) return 0;
    if (optional && errno == ENOENT) return 0;
    return snag_errorf(error, error_size, "cannot remove deleted-session %s: %s", name, strerror(errno));
}

static int
remove_deleted_session(struct snag_store *store, struct snag_session *session, char *error, size_t error_size)
{
    if (close_fd_slot(&session->log_fd) < 0)
        return snag_errorf(error, error_size, "cannot close deleted-session files: %s",
                  strerror(errno));
    if (snag_media_work_remove(session->dir_fd,error,error_size)!=0)return -1;
    if (snag_store_remove_upload_staging(session->dir_fd, error, error_size) < 0) return -1;
    if (snag_media_remove(session->dir_fd,error,error_size)<0)return -1;
    /* Keep the durable delete intent until other content is gone. The private
     * trash name remains the deletion marker after the final log unlink. */
    if (unlink_expected_file(session->dir_fd, "prompt_history", true, error, error_size) < 0 ||
        unlink_expected_file(session->dir_fd, "meta.json", true, error, error_size) < 0 ||
        unlink_expected_file(session->dir_fd, "events.jsonl", true, error, error_size) < 0 ||
        snag_sync_dir(session->dir_fd) < 0 || close_fd_slot(&session->lock_fd) < 0 ||
        unlink_expected_file(session->dir_fd, "lock", true, error, error_size) < 0) return -1;
    if (close_fd_slot(&session->dir_fd) < 0)
        return snag_errorf(error, error_size, "cannot close deleted-session directory: %s", strerror(errno));
    if (snag_unlink_at(store->trash_fd, session->trash_name, true) < 0)
        return snag_errorf(error, error_size, "cannot remove deleted-session directory: %s", strerror(errno));
    if (snag_sync_dir(store->trash_fd) < 0)
        return snag_errorf(error, error_size, "cannot sync trash cleanup: %s", strerror(errno));
    return 0;
}

int
snag_session_complete_delete(struct snag_store *store, struct snag_session *session,
                            char *error, size_t error_size)
{
    if (!store || !session || !session->delete_requested || !session->trash_name[0] || session->dir_fd < 0)
        return snag_fail(error, error_size, EINVAL, "no completed delete intent is open");
    if (snag_rename_at(store->sessions_fd, session->id, store->trash_fd, session->trash_name) < 0)
        return snag_errorf(error, error_size, "cannot move session to trash: %s", strerror(errno));
    if (snag_sync_dir(store->sessions_fd) < 0 || snag_sync_dir(store->trash_fd) < 0)
        return snag_errorf(error, error_size, "cannot sync delete rename: %s", strerror(errno));
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

    if (!snag_store_trash_id(trash_name, id))
        return snag_fail(error, error_size, EINVAL, "invalid deleted-session trash name");
    dir_fd = snag_open_read_security_at(store->trash_fd, trash_name, true);
    if (dir_fd < 0) return snag_errorf(error, error_size, "cannot open deleted-session trash: %s",
                  strerror(errno));

    snag_session_init(&session);
    memcpy(session.id, id, sizeof(session.id));
    session.dir_fd = dir_fd;
    if (snag_store_verify_private_fd(session.dir_fd, true,
                                    "deleted-session directory", error, error_size) < 0) goto out;
    if (snag_store_open_session_files(&session, false, error, error_size) < 0) {
        if (errno != ENOENT || session.lock_fd < 0 ||
            verify_delete_remnants(session.dir_fd, error, error_size) < 0) goto out;
        (void)snag_strcpy(session.trash_name, sizeof(session.trash_name), trash_name);
        if (error_size) error[0] = '\0';
        rc = remove_deleted_session(store, &session, error, error_size);
        goto out;
    }
    if (snag_store_scan_log(&session, SNAG_TAIL_REJECT, error, error_size) < 0) goto out;
    if (!session.delete_requested || strcmp(session.trash_name, trash_name) != 0) {
        (void)snag_fail(error, error_size, EINVAL, "deleted-session trash intent mismatch");
        goto out;
    }
    rc = remove_deleted_session(store, &session, error, error_size);
out: snag_session_close(&session);
    return rc;
}

int
snag_session_delete(struct snag_store *store, struct snag_session *session,
                   const char *confirmed_prefix, uint64_t *written_seq, char *error, size_t error_size)
{
    char trash_name[SNAG_TRASH_NAME_LEN + 1u];

    if (!confirmed_prefix || strlen(confirmed_prefix) != 8u || !snag_hex_is_lower(confirmed_prefix, 8u) ||
        memcmp(confirmed_prefix, session->id, 8u) != 0) {
        return snag_fail(error, error_size, EINVAL, "delete confirmation did not match session id");
    }
    if (make_trash_name(session, trash_name, error, error_size) < 0) return -1;
    if (snag_session_commit(session, "session_delete_requested",
                           json_pack("{s:s,s:s}", "confirmed_id_prefix", confirmed_prefix,
                                     "trash_name", trash_name), written_seq, error, error_size) < 0)
        return -1;
    return snag_session_complete_delete(store, session, error, error_size);
}
