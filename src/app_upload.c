/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"
#include "fs.h"
#include "media.h"
#include "tools.h"
#include "upload.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

static int
transfer_checkpoint(void *opaque)
{
    struct app_state *app = opaque;
    return app->input_closed || app->shutdown_signal ? 1 : 0;
}

static int
prepare_checkpoint(void *opaque, unsigned int timeout_ms)
{
    struct app_state *app = opaque;
    enum snag_term_action action = SNAG_TERM_NONE;
    char *text = NULL;
    (void)timeout_ms;
    if (transfer_checkpoint(app)) return 2;
    if (snag_ui_poll(&app->ui, 0, &action, &text) < 0) return -1;
    if (action == SNAG_TERM_EXIT) app->input_closed = true;
    if (text && !*text) { free(text); return 0; }
    if (text) {
        int rc = snag_ui_send(&app->ui,
                              (struct snag_ui_command){.kind = SNAG_UI_DRAFT, .text = text});
        free(text);
        return rc < 0 ? -1 : 1;
    }
    if (action == SNAG_TERM_CANCEL || action == SNAG_TERM_INTERRUPT ||
        action == SNAG_TERM_EXIT) return 2;
    return 0;
}

static void
raw_transfer_mode(struct termios *mode)
{
    mode->c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    mode->c_oflag &= ~OPOST;
    mode->c_cflag |= CS8;
    mode->c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    mode->c_cc[VMIN] = 1;
    mode->c_cc[VTIME] = 0;
}

static int
stage_open(struct app_state *app, char name[SNAG_ID_HEX_LEN + 8u])
{
    for (unsigned int attempt = 0; attempt < 4u; ++attempt) {
        char id[SNAG_ID_HEX_LEN + 1u];
        if (snag_random_id(id) < 0) return -1;
        (void)snprintf(name, SNAG_ID_HEX_LEN + 8u, "upload-%s", id);
        if (snag_mkdir_private_at(app->session.dir_fd, name) == 0) {
            int fd = snag_open_inspect_at(app->session.dir_fd, name);
            if (fd < 0) (void)snag_unlink_at(app->session.dir_fd, name, true);
            return fd;
        }
        if (errno != EEXIST) return -1;
    }
    return snag_errno(EEXIST);
}

static char *
staged_path(const struct snag_session *session, const char *stage, const char *leaf)
{
    size_t n = strlen(session->dir_path) + strlen(stage) + strlen(leaf) + 3u;
    if (n > SNAG_PATH_MAX_BYTES) { errno = ENAMETOOLONG; return NULL; }
    char *path = malloc(n);
    if (path) (void)snprintf(path, n, "%s/%s/%s", session->dir_path, stage, leaf);
    return path;
}

static void
rollback_parts(struct app_state *app, const json_t *parts, const json_t *extra)
{
    for (size_t i = 0u; i <= json_array_size(parts); ++i) {
        const json_t *part = i == json_array_size(parts) ? extra : json_array_get(parts, i);
        if (!part) continue;
        snag_app_discard_part(app, part);
    }
}

static int
prepare_files(struct app_state *app, const char *stage, struct snag_upload_result *result,
              char *error, size_t error_size)
{
    json_t *addition = json_array();
    json_t *next = app->draft_content ? json_copy(app->draft_content) : json_array();
    int rc = -1, status = -1;
    if (!addition || !next) { errno = ENOMEM; goto out; }
    for (size_t i = 0; i < result->count; ++i) {
        struct snag_upload_file *file = &result->files[i];
        char *path = staged_path(&app->session, stage, file->leaf);
        json_t *part = NULL;
        if (!path) goto out;
        rc = snag_app_prepare_attachment(app, path, file->name, prepare_checkpoint,
                                         &part, error, error_size);
        free(path);
        if (rc || !part) {
            json_decref(part);
            goto out;
        }
        if (json_array_append(addition, part) < 0) {
            rollback_parts(app, NULL, part);
            json_decref(part);
            goto out;
        }
        json_decref(part);
        if (json_array_append(next, json_array_get(addition, i)) < 0) goto out;
        if (!snag_media_content_valid(next)) {
            (void)snag_errorf(error, error_size, "uploaded images exceed the 12 MiB input budget");
            errno = EFBIG;
            goto out;
        }
    }
    json_decref(app->draft_content);
    app->draft_content = next;
    next = NULL;
    status = 0;
out:
    if (status < 0 && addition) rollback_parts(app, addition, NULL);
    json_decref(addition);
    json_decref(next);
    if (status < 0 && !error[0])
        (void)snag_errorf(error, error_size, "Upload attachment preparation interrupted or failed");
    return status;
}
#endif

int
snag_app_upload_command(struct app_state *app, bool directory)
{
#ifdef _WIN32
    (void)directory;
    return snag_ui_text(&app->ui, SNAG_UI_ERROR, "Terminal upload is not available on this host.");
#else
    struct snag_upload_result result = {0};
    char error[256] = "upload could not start";
    char stage[SNAG_ID_HEX_LEN + 8u] = {0};
    int stage_fd = -1, tty = -1, rc = -1;
    bool leased = false, raw = false, restored = true;
    struct termios saved;
    if (app->attaching)
        return snag_ui_text(&app->ui, SNAG_UI_ERROR, "Attachment preparation is already active.");
    if (snag_session_persist(&app->store, &app->session, error, sizeof(error)) < 0) goto out;
    if (json_array_size(app->draft_content) >= SNAG_UPLOAD_FILES_MAX) {
        (void)snag_errorf(error, sizeof(error),
                          "At most eight files may be attached to one input.");
        goto out;
    }
    stage_fd = stage_open(app, stage);
    if (stage_fd < 0) {
        (void)snag_errorf(error, sizeof(error), "Cannot stage upload: %s", strerror(errno));
        goto out;
    }
    app->attaching = true;
    if (snag_ui_external(&app->ui, true, error, sizeof(error)) < 0) goto out;
    leased = true;
    tty = open("/dev/tty", O_RDWR | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (tty < 0 || tcgetattr(tty, &saved) < 0) {
        (void)snag_errorf(error, sizeof(error),
                          "Cannot open transfer terminal: %s", strerror(errno));
        goto out;
    }
    struct termios mode = saved;
    raw_transfer_mode(&mode);
    if (tcsetattr(tty, TCSANOW, &mode) < 0) {
        (void)snag_errorf(error, sizeof(error), "Cannot enter transfer mode: %s", strerror(errno));
        goto out;
    }
    raw = true;
    rc = snag_upload_receive(tty, stage_fd,
        SNAG_UPLOAD_FILES_MAX - json_array_size(app->draft_content), directory,
        transfer_checkpoint, app, &result, error, sizeof(error));
    if (rc == 0 && result.count == 0u) rc = 1; /* No attachment to publish. */
out:
    if (raw && tcsetattr(tty, TCSANOW, &saved) < 0) {
        restored = false;
        rc = -1;
        (void)snag_errorf(error, sizeof(error),
                          "Cannot restore transfer terminal: %s", strerror(errno));
    }
    if (tty >= 0) close(tty);
    char replay_error[256] = {0};
    if (leased && restored && snag_ui_external_replay(&app->ui, result.tail, result.tail_len,
                                          replay_error, sizeof(replay_error)) < 0) {
        restored = false;
        rc = -1;
        (void)snag_errorf(error, sizeof(error), "Cannot restore transfer display: %s",
                          replay_error[0] ? replay_error : strerror(errno));
    }
    if (rc == 0 && prepare_files(app, stage, &result, error, sizeof(error)) < 0) rc = -1;
    if (stage_fd >= 0) {
        snag_upload_cleanup(stage_fd, &result);
        close(stage_fd);
        (void)snag_unlink_at(app->session.dir_fd, stage, true);
    }
    app->attaching = false;
    if (!restored) return -1; /* Uncertain terminal ownership is a runtime failure. */
    if (rc == 0) {
        bool handled;
        return snag_app_media_command(app, "/attachments", &handled);
    }
    if (rc == 1)
        return snag_ui_text(&app->ui, SNAG_UI_HOST, "Upload cancelled; no files attached.");
    return snag_ui_text(&app->ui, SNAG_UI_ERROR, error);
#endif
}

int
snag_app_download_queue(struct app_state *app, const char *path, json_t **result,
                        char *error, size_t error_size)
{
    *result = NULL;
    json_t *asset = NULL;
    char *source = NULL;
    const char *name = NULL;
    int fd = -1, rc = -1;
    snag_file_info info;
    char id[SNAG_ID_HEX_LEN + 1u];
    char sha[SNAG_SHA256_HEX_LEN + 1u];

    if (!strncmp(path, "asset:", 6u)) {
        if (snag_session_media(&app->session, path, NULL, prepare_checkpoint, app,
                               &asset, &source, error, error_size) < 0) goto out;
        name = snag_json_string(asset, "name");
    } else {
        source = snag_path_root_len(path) ? strdup(path) : snag_path_join(app->session.cwd, path);
    }
    if (!source) goto out;
    if (!name || !*name) {
        const char *slash = strrchr(source, '/');
        name = slash ? slash + 1u : source;
    }
    if (!*name || strlen(name) > SNAG_NAME_MAX_BYTES ||
        !snag_utf8_valid((const unsigned char *)name, strlen(name), true)) {
        (void)snag_fail(error, error_size, EINVAL, "download needs a valid file name");
        goto out;
    }
    for (size_t i = 0; name[i]; ++i)
        if ((unsigned char)name[i] < 0x20u || name[i] == 0x7f ||
            name[i] == '/' || name[i] == '\\') {
            (void)snag_fail(error, error_size, EINVAL, "download needs a printable leaf file name");
            goto out;
        }
    fd = snag_open_inspect_path(app->session.cwd, source);
    if (fd < 0 || snag_fstat(fd, &info) < 0 || !S_ISREG(info.st_mode) || info.st_size < 0) {
        (void)snag_errorf(error, error_size, "Download queue requires a readable regular file: %s",
                          strerror(errno));
        goto out;
    }
    struct snag_sha256 hash;
    snag_sha256_init(&hash);
    unsigned char bytes[65536];
    for (;;) {
        ssize_t n = read(fd, bytes, sizeof(bytes));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            (void)snag_errorf(error, error_size,
                "Cannot hash queued file: %s", strerror(errno));
            goto out;
        }
        if (!n) break;
        snag_sha256_update(&hash, bytes, (size_t)n);
    }
    snag_sha256_final_hex(&hash, sha);
    if (snag_random_id(id) < 0) goto out;
    json_t *event = json_pack("{s:s,s:s,s:s,s:I,s:I,s:s,s:I}",
        "id", id, "path", source, "name", name, "bytes", (json_int_t)info.st_size,
        "mtime", (json_int_t)info.st_mtime, "sha256", sha,
        "queued_ms", (json_int_t)snag_time_ms());
    if (!event || snag_app_commit_event(app, "download_queued", event, error, error_size) < 0)
        goto out;
    char message[SNAG_PATH_MAX_BYTES + SNAG_ID_HEX_LEN + 128u];
    (void)snprintf(message, sizeof(message),
        "Queued download %s for the next wrapped workstation client; not delivered yet: %s",
        id, source);
    *result = snag_tool_result_terminal(true, message);
    rc = *result ? 0 : -1;
out:
    if (fd >= 0) (void)close(fd);
    free(source);
    json_decref(asset);
    if (rc < 0 && !*result)
        *result = snag_tool_result_terminal(false,
            error[0] ? error : "Download queue failed.");
    return *result ? 0 : -1;
}

int
snag_app_download(struct app_state *app, const char *path, json_t **result,
                  char *error, size_t error_size)
{
    *result = NULL;
#ifdef _WIN32
    (void)app;
    (void)path;
    (void)error;
    (void)error_size;
    *result = snag_tool_result_terminal(false, "Terminal download is not available on this host.");
    return *result ? 0 : -1;
#else
    struct snag_upload_result transfer = {0};
    json_t *asset = NULL;
    char *source = NULL;
    const char *name = NULL;
    int input = -1, tty = -1, rc = -1;
    bool leased = false, raw = false, restored = true, owning = false;
    struct termios saved;
    snag_file_info info;

    if (app->execute || !app->ui.opened || snag_isatty(STDERR_FILENO) != 1) {
        (void)snag_errorf(error, error_size, "Download needs an interactive POSIX terminal.");
        goto out;
    }
    if (app->attaching) {
        (void)snag_errorf(error, error_size,
                          "A terminal transfer or attachment is already active.");
        goto out;
    }
    if (!strncmp(path, "asset:", 6u)) {
        if (snag_session_media(&app->session, path, NULL, prepare_checkpoint, app,
                               &asset, &source, error, error_size) < 0) goto out;
        name = snag_json_string(asset, "name");
    } else {
        source = snag_path_root_len(path) ? strdup(path) : snag_path_join(app->session.cwd, path);
    }
    if (!source) goto out;
    if (!name || !*name) {
        const char *slash = strrchr(source, '/');
        name = slash ? slash + 1u : source;
    }
    input = snag_open_read(source, false);
    if (input < 0 || snag_fstat(input, &info) < 0 || !S_ISREG(info.st_mode)) {
        (void)snag_errorf(error, error_size, "Download requires a readable regular file: %s",
                          input < 0 ? strerror(errno) : "directories are not supported");
        goto out;
    }
    app->attaching = true;
    owning = true;
    if (snag_ui_external(&app->ui, true, error, error_size) < 0) goto out;
    leased = true;
    tty = open("/dev/tty", O_RDWR | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (tty < 0 || tcgetattr(tty, &saved) < 0) {
        (void)snag_errorf(error, error_size, "Cannot open transfer terminal: %s", strerror(errno));
        goto out;
    }
    struct termios mode = saved;
    raw_transfer_mode(&mode);
    if (tcsetattr(tty, TCSANOW, &mode) < 0) {
        (void)snag_errorf(error, error_size, "Cannot enter transfer mode: %s", strerror(errno));
        goto out;
    }
    raw = true;
    rc = snag_download_send(tty, input, name, transfer_checkpoint, app,
                            &transfer, error, error_size);
out:
    if (raw && tcsetattr(tty, TCSANOW, &saved) < 0) {
        restored = false;
        (void)snag_errorf(error, error_size, "Cannot restore transfer terminal: %s",
                          strerror(errno));
    }
    if (tty >= 0) close(tty);
    if (input >= 0) close(input);
    char replay_error[256] = {0};
    if (leased && snag_ui_external_replay(&app->ui, transfer.tail, transfer.tail_len,
                                          replay_error, sizeof(replay_error)) < 0) {
        (void)snag_errorf(error, error_size, "Cannot restore transfer input: %s", replay_error);
        restored = false;
    }
    if (owning) app->attaching = false;
    free(source);
    json_decref(asset);
    if (!restored) return -1;
    char receipt[SNAG_PATH_MAX_BYTES + 128u];
    if (rc == 0 && transfer.receipt[0])
        (void)snprintf(receipt, sizeof(receipt), "Download completed: %s\n"
                       "Client acknowledged the file digest and final EXIT.", transfer.receipt);
    *result = snag_tool_result_terminal(rc == 0, rc == 0 ?
        transfer.receipt[0] ? receipt :
        "Download completed: client acknowledged the file digest and final EXIT." : rc == 1 ?
        "Download cancelled; a partial file may remain on the workstation." :
        error[0] ? error : "Download failed.");
    return *result ? 0 : -1;
#endif
}
