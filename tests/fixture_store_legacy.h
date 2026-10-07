/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_FIXTURE_STORE_LEGACY_H
#define SNAJPAGENT_FIXTURE_STORE_LEGACY_H

#include "fs.h"
#include "store_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Explicit released-format fixtures for JSONL byte/envelope recovery tests.
 * Production creation uses the native factory; this constructor is test-only. */
static inline int
legacy_fixture_persist(struct snag_store *store, struct snag_session *session,
    char *error, size_t error_size)
{
    struct snag_session disk = *session;
    char *parent;
    int rc = -1;

    if (!session->pending_log) return 0;
    if (snag_mkdir_private_at(store->sessions_fd, session->id) < 0) {
        snag_errorf(error, error_size, "cannot create session directory: %s", strerror(errno));
        return -1;
    }
    parent = snag_path_join(store->root_path, "sessions");
    disk.dir_path = parent ? snag_path_join(parent, session->id) : NULL;
    free(parent);
    if (!disk.dir_path) goto out;
    disk.dir_fd = snag_open_read_security_at(store->sessions_fd, session->id, true);
    if (disk.dir_fd < 0 ||
        snag_store_verify_private_fd(disk.dir_fd, true, "session directory",
            error, error_size) < 0 ||
        snag_store_open_session_files(&disk, true, error, error_size) < 0) goto out;
    if (snag_write_full(disk.log_fd, session->pending_log->data, session->pending_log->len) < 0 ||
        snag_sync_file(disk.log_fd) < 0 || snag_sync_dir(disk.dir_fd) < 0 ||
        snag_sync_dir(store->sessions_fd) < 0) {
        snag_errorf(error, error_size, "cannot persist new session: %s", strerror(errno));
        goto out;
    }
    session->dir_path = disk.dir_path;
    session->dir_fd = disk.dir_fd;
    session->log_fd = disk.log_fd;
    session->lock_fd = disk.lock_fd;
    snag_buf_free(session->pending_log);
    free(session->pending_log);
    session->pending_log = NULL;
    return 0;
out:
    if (disk.log_fd >= 0) (void)close(disk.log_fd);
    if (disk.lock_fd >= 0) (void)close(disk.lock_fd);
    if (disk.dir_fd >= 0) {
        (void)snag_unlink_at(disk.dir_fd, "events.jsonl", false);
        (void)snag_unlink_at(disk.dir_fd, "lock", false);
        (void)close(disk.dir_fd);
    }
    free(disk.dir_path);
    (void)snag_unlink_at(store->sessions_fd, session->id, true);
    return rc;
}

static inline int
legacy_fixture_create(struct snag_store *store, struct snag_session *session,
    const char *cwd, const char *provider, const char *model, const char *effort,
    char *error, size_t error_size)
{
    if (snag_session_prepare(session, cwd, provider, model, effort, error, error_size) < 0)
        return -1;
    return legacy_fixture_persist(store, session, error, error_size);
}

/* Seed an explicit old-format CLI fixture; ordinary creation stays native. */
static inline int
legacy_fixture_command(const char *dotdir, const char *cwd, const char *provider,
    const char *model, const char *effort)
{
    struct snag_store store;
    snag_store_init(&store);
    struct snag_session session;
    snag_session_init(&session);
    char error[1024] = "";
    int rc = -1;

    if (snag_store_open(&store, dotdir, error, sizeof(error)) < 0 ||
        legacy_fixture_create(&store, &session, cwd, provider, model, effort,
            error, sizeof(error)) < 0) goto done;
    if (puts(session.id) == EOF || fflush(stdout)) goto done;
    rc = 0;
done:
    if (rc < 0) fprintf(stderr, "legacy fixture: %s\n", error[0] ? error : strerror(errno));
    snag_session_close(&session);
    snag_store_close(&store);
    return rc < 0 ? 1 : 0;
}

#endif /* SNAJPAGENT_FIXTURE_STORE_LEGACY_H */
