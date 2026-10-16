/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"
#include "fs.h"
#include "media.h"
#include "tmux.h"
#include "tools.h"
#include "upload.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int
prepare_checkpoint(void *opaque, unsigned int timeout_ms)
{
    struct app_state *app = opaque;
    enum snag_term_action action = SNAG_TERM_NONE;
    char *text = NULL;
    (void)timeout_ms;
    if (app->input_closed || app->shutdown_signal) return 2;
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

#ifndef _WIN32
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <termios.h>

struct app_transfer {
    pthread_t thread;
    atomic_bool done, cancel;
    uint64_t attachment;
    struct snag_terminal_profile profile;
    struct snag_upload_result result;
    struct termios saved;
    int tty, input, stage_fd, rc, cause;
    size_t slots;
    bool started, leased, raw, native, upload, fast, allow_legacy;
    char stage[SNAG_ID_HEX_LEN + 8u];
    char *source, *name;
    json_t *pending;
    char error[256];
};

static bool
transfer_replaced(const struct app_state *app, const struct app_transfer *transfer)
{
    return app->ui.native && (!transfer->attachment ||
        transfer->attachment != snag_ui_session_attachment(&app->ui));
}

static int
transfer_checkpoint(void *opaque)
{
    const struct app_transfer *transfer = opaque;
    return atomic_load(&transfer->cancel);
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

static bool
transfer_fast_client(const struct app_state *app)
{
    return app->remote_available &&
        snag_monotonic_ms() - app->remote_reply_at < 2000u;
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

#ifndef _WIN32
static struct app_transfer *
transfer_new(struct app_state *app, bool upload)
{
    struct app_transfer *transfer = calloc(1u, sizeof(*transfer));
    if (!transfer) return NULL;
    atomic_init(&transfer->done, false);
    atomic_init(&transfer->cancel, false);
    transfer->tty = transfer->input = transfer->stage_fd = -1;
    transfer->attachment = snag_ui_session_attachment(&app->ui);
    transfer->native = app->ui.native;
    transfer->profile = app->ui.profile;
    transfer->upload = upload;
    transfer->fast = transfer_fast_client(app);
    transfer->rc = -1;
    return transfer;
}

static void *
transfer_owner(void *opaque)
{
    struct app_transfer *transfer = opaque;
    const struct snag_terminal_profile *profile = transfer->native ? &transfer->profile : NULL;
    if (transfer->upload)
        transfer->rc = snag_upload_receive(transfer->tty, transfer->stage_fd, transfer->slots,
            false, profile, !transfer->fast, true, transfer_checkpoint, transfer,
            &transfer->result, transfer->error, sizeof(transfer->error));
    else
        transfer->rc = snag_download_send(transfer->tty, transfer->input, transfer->name,
            transfer->pending ? snag_json_string(transfer->pending, "sha256") : NULL,
            profile, !transfer->fast, transfer->allow_legacy, transfer_checkpoint, transfer,
            &transfer->result, transfer->error, sizeof(transfer->error));
    transfer->cause = errno;
    atomic_store(&transfer->done, true);
    return NULL;
}

static int
transfer_start(struct app_state *app, struct app_transfer *transfer)
{
    app->attaching = true;
    app->transfer = transfer;
    if (snag_ui_external(&app->ui, true, transfer->error, sizeof(transfer->error)) < 0)
        return -1;
    transfer->leased = true;
    transfer->tty = open("/dev/tty", O_RDWR | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (transfer->tty < 0 || tcgetattr(transfer->tty, &transfer->saved) < 0)
        return snag_errorf(transfer->error, sizeof(transfer->error),
            "Cannot open transfer terminal: %s", strerror(errno));
    struct termios mode = transfer->saved;
    raw_transfer_mode(&mode);
    if (tcsetattr(transfer->tty, TCSANOW, &mode) < 0)
        return snag_errorf(transfer->error, sizeof(transfer->error),
            "Cannot enter transfer mode: %s", strerror(errno));
    transfer->raw = true;
    int rc = pthread_create(&transfer->thread, NULL, transfer_owner, transfer);
    if (rc)
        return snag_errorf(transfer->error, sizeof(transfer->error),
            "Cannot start transfer: %s", strerror(rc));
    transfer->started = true;
    return 0;
}

static bool
transfer_queued(const struct app_state *app, const struct app_transfer *transfer)
{
    const char *id = snag_json_string(transfer->pending, "id");
    for (size_t i = 0u; id && i < json_array_size(app->session.download_queue); ++i)
        if (!strcmp(id, snag_json_string(json_array_get(app->session.download_queue, i), "id")))
            return true;
    return false;
}

static int
transfer_finish(struct app_state *app, struct app_transfer *transfer, json_t **result,
    bool *unavailable)
{
    if (transfer->started) pthread_join(transfer->thread, NULL);
    int rc = transfer->rc;
    bool restored = true;
    if (unavailable)
        *unavailable = rc < 0 && !transfer->fast && transfer->cause == ENOTSUP;
    if (transfer->leased && transfer_replaced(app, transfer)) {
        transfer->result.tail_len = 0u;
        if (transfer->tty >= 0 && tcflush(transfer->tty, TCIFLUSH) < 0) restored = false;
    }
    if (transfer->raw && tcsetattr(transfer->tty, TCSANOW, &transfer->saved) < 0)
        restored = false;
    if (transfer->tty >= 0) close(transfer->tty);
    if (transfer->input >= 0) close(transfer->input);
    if (!restored) {
        rc = -1;
        (void)snag_errorf(transfer->error, sizeof(transfer->error),
            "Cannot restore transfer terminal: %s", strerror(errno));
    }
    if (transfer->upload) {
        if (rc == 0 && !transfer->result.count) rc = 1;
        if (rc == 0 && prepare_files(app, transfer->stage, &transfer->result,
                transfer->error, sizeof(transfer->error)) < 0)
            rc = -1;
        if (transfer->stage_fd >= 0) {
            snag_upload_cleanup(transfer->stage_fd, &transfer->result);
            close(transfer->stage_fd);
            (void)snag_unlink_at(app->session.dir_fd, transfer->stage, true);
        }
    }
    char error[256] = "";
    if (transfer->leased && snag_ui_external_replay(&app->ui, transfer->result.tail,
            transfer->result.tail_len, error, sizeof(error)) < 0)
        restored = false;
    if (!restored) {
        rc = -1;
        (void)snag_errorf(transfer->error, sizeof(transfer->error),
            "Cannot restore transfer display: %s", error[0] ? error : strerror(errno));
    }
    if (!transfer->upload && rc == 0 && transfer_queued(app, transfer)) {
        if (snag_app_commit_event(app, "download_removed", json_pack("{s:s,s:s}", "id",
                snag_json_string(transfer->pending, "id"), "reason", "delivered to wrapped client"),
                transfer->error, sizeof(transfer->error)) < 0)
            rc = -1;
    }
    app->transfer = NULL;
    app->attaching = false;
    int status = 0;
    if (transfer->upload) {
        if (rc == 0) {
            bool handled;
            status = snag_app_media_command(app, "/attachments", &handled);
        } else
            status = snag_ui_text(&app->ui, rc == 1 ? SNAG_UI_HOST : SNAG_UI_ERROR,
                rc == 1 ? "Upload cancelled; no files attached." : transfer->error);
    } else {
        char receipt[SNAG_PATH_MAX_BYTES + 128u];
        (void)snprintf(receipt, sizeof(receipt), "Download completed: %s%s"
            "Client acknowledged the file digest and final EXIT.", transfer->result.receipt,
            transfer->result.receipt[0] ? "\n" : "");
        json_t *finished = snag_tool_result_terminal(rc == 0, rc == 0 ? receipt : rc == 1 ?
            "Download cancelled; a partial file may remain on the workstation." :
            transfer->error[0] ? transfer->error : "Download failed.");
        if (result) *result = finished;
        else {
            status = finished ? snag_ui_text(&app->ui, rc == 0 ? SNAG_UI_HOST : SNAG_UI_ERROR,
                snag_json_string(finished, "model_text")) : -1;
            json_decref(finished);
        }
        if (result && !finished) status = -1;
    }
    free(transfer->source);
    free(transfer->name);
    json_decref(transfer->pending);
    free(transfer);
    return restored ? status : -1;
}

static int
transfer_wait(struct app_state *app, struct app_transfer *transfer, json_t **result,
    bool *unavailable)
{
    uint32_t wait = app->session.default_yield_ms ? app->session.default_yield_ms :
        app->session.max_wait_ms;
    if (wait > app->session.max_wait_ms) wait = app->session.max_wait_ms;
    uint64_t deadline = snag_monotonic_ms() + wait;
    bool retain_yield = app->tool_active || app->tool_waiting;
    bool previous_yield = app->yield_requested;
    app->transfer_waiting = true;
    while (!atomic_load(&transfer->done)) {
        int rc = snag_app_active_input_pump(app, 0u);
        if (rc < 0 || rc == 2 || app->interrupt_requested) {
            atomic_store(&transfer->cancel, true);
            break;
        }
        if (rc == 1 || app->yield_requested || snag_monotonic_ms() >= deadline) break;
        (void)snag_sleep_ms(10u);
    }
    app->transfer_waiting = false;
    if (!retain_yield) app->yield_requested = previous_yield;
    if (atomic_load(&transfer->done)) return transfer_finish(app, transfer, result, unavailable);
    if (result) {
        char text[256];
        (void)snprintf(text, sizeof(text), "Download pending%s%s; transfer continues in the "
            "background. Delivery requires client digest and final EXIT acknowledgement.%s",
            transfer->pending ? " " : "",
            transfer->pending ? snag_json_string(transfer->pending, "id") : "",
            transfer->pending ? " Use download_queue list to inspect the pending intent." : "");
        *result = snag_tool_result_terminal(true, text);
        return *result ? 0 : -1;
    }
    return snag_ui_text(&app->ui, SNAG_UI_HOST,
        "Upload pending; transfer continues in the background. Completed files join the draft.");
}
#endif

int
snag_app_transfer_service(struct app_state *app)
{
#ifdef _WIN32
    (void)app;
    return 0;
#else
    struct app_transfer *transfer = app->transfer;
    if (!transfer) return 0;
    if (app->input_closed || app->shutdown_signal || app->interrupt_requested ||
        transfer_replaced(app, transfer) || (transfer->pending && !transfer_queued(app, transfer)))
        atomic_store(&transfer->cancel, true);
    if (app->transfer_waiting || !atomic_load(&transfer->done)) return 0;
    if (transfer_finish(app, transfer, NULL, NULL) < 0) return -1;
    return snag_app_context_refresh(app);
#endif
}

void
snag_app_transfer_close(struct app_state *app)
{
#ifndef _WIN32
    if (!app->transfer) return;
    atomic_store(&app->transfer->cancel, true);
    (void)transfer_finish(app, app->transfer, NULL, NULL);
#else
    (void)app;
#endif
}

int
snag_app_upload_command(struct app_state *app, bool directory)
{
#ifdef _WIN32
    (void)directory;
    return snag_ui_text(&app->ui, SNAG_UI_ERROR, "Terminal upload is not available on this host.");
#else
    if (directory)
        return snag_ui_text(&app->ui, SNAG_UI_ERROR, "Directory uploads are unsupported.");
    if (app->attaching)
        return snag_ui_text(&app->ui, SNAG_UI_ERROR, "Attachment preparation is already active.");
    char error[256] = "";
    if (snag_session_persist(&app->store, &app->session, error, sizeof(error)) < 0)
        return snag_ui_text(&app->ui, SNAG_UI_ERROR, error);
    if (json_array_size(app->draft_content) >= SNAG_UPLOAD_FILES_MAX)
        return snag_ui_text(&app->ui, SNAG_UI_ERROR,
            "At most eight files may be attached to one input.");
    struct app_transfer *transfer = transfer_new(app, true);
    if (!transfer) return -1;
    transfer->slots = SNAG_UPLOAD_FILES_MAX - json_array_size(app->draft_content);
    transfer->stage_fd = stage_open(app, transfer->stage);
    if (transfer->stage_fd < 0 || transfer_start(app, transfer) < 0) {
        if (!transfer->error[0])
            (void)snag_errorf(transfer->error, sizeof(transfer->error),
                "Cannot stage upload: %s", strerror(errno));
        return transfer_finish(app, transfer, NULL, NULL);
    }
    return transfer_wait(app, transfer, NULL, NULL);
#endif
}

int
snag_app_remote_probe(struct app_state *app)
{
    if (app->attaching || app->pager || app->execute || !app->ui.opened ||
        snag_isatty(STDERR_FILENO) != 1) return 0;
    app->remote_attachment = snag_ui_session_attachment(&app->ui);
    if (app->ui.native && !app->remote_attachment) {
        app->remote_verified = app->remote_available = false;
        app->remote_nonce[0] = '\0';
        app->remote_probe_at = snag_monotonic_ms();
        return 0;
    }
    char id[SNAG_ID_HEX_LEN + 1u];
    if (snag_random_id(id) < 0) return -1;
    id[8] = '\0';
    (void)snprintf(app->remote_nonce, sizeof(app->remote_nonce), "%lu",
                   strtoul(id, NULL, 16) % 1000000000ul);
    app->remote_probe_at = snag_monotonic_ms();
    app->remote_verified = false;
    char query[48];
    const char *sty = getenv("STY");
    bool screen = app->ui.native ? app->ui.profile.sty[0] != '\0' : sty && *sty;
    int n = snprintf(query, sizeof(query), "%s\033[?9001;%sn%s",
                     screen ? "\033P" : "", app->remote_nonce, screen ? "\033\\" : "");
    struct snag_terminal_profile captured;
    const struct snag_terminal_profile *profile = &app->ui.profile;
    if (!app->ui.native) {
        if (snag_terminal_profile_capture(&captured) < 0) return 0;
        profile = &captured;
    }
    if (profile->tmux[0]) {
        n = snprintf(query, sizeof(query), "\033[?9001;%sn", app->remote_nonce);
        int fd = snag_tmux_output_open(profile, NULL);
        if (fd < 0) return 0;
        if (n > 0 && (size_t)n < sizeof(query)) {
            ssize_t ignored = write(fd, query, (size_t)n);
            (void)ignored;
        }
        (void)close(fd);
        return 0;
    }
    return n < 0 || (size_t)n >= sizeof(query) ? -1 :
        snag_ui_send(&app->ui, (struct snag_ui_command){.kind = SNAG_UI_RAW,
            .data.value = STDERR_FILENO, .text = query, .len = (size_t)n});
}

void
snag_app_remote_reply(struct app_state *app, const char *nonce)
{
    if (nonce && app->remote_nonce[0] && !strcmp(nonce, app->remote_nonce) &&
        (!app->ui.native || (app->remote_attachment &&
         app->remote_attachment == snag_ui_session_attachment(&app->ui))) &&
        snag_monotonic_ms() - app->remote_probe_at < 1000u) {
        app->remote_verified = true;
        app->remote_reply_at = snag_monotonic_ms();
        app->remote_nonce[0] = '\0';
    }
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

static int
app_download(struct app_state *app, const char *path, const json_t *pending,
             bool allow_legacy, bool *unavailable, json_t **result,
             char *error, size_t error_size)
{
    *result = NULL;
    if (unavailable) *unavailable = false;
#ifdef _WIN32
    (void)app;
    (void)path;
    (void)pending;
    (void)allow_legacy;
    (void)error;
    (void)error_size;
    *result = snag_tool_result_terminal(false, "Terminal download is not available on this host.");
    return *result ? 0 : -1;
#else
    json_t *asset = NULL;
    char *source = NULL;
    const char *name = NULL;
    int input = -1;
    snag_file_info info;
    if (app->execute || !app->ui.opened || snag_isatty(STDERR_FILENO) != 1) {
        (void)snag_errorf(error, error_size, "Download needs an interactive POSIX terminal.");
        goto rejected;
    }
    if (app->attaching) {
        (void)snag_errorf(error, error_size,
                          "A terminal transfer or attachment is already active.");
        goto rejected;
    }
    if (!strncmp(path, "asset:", 6u)) {
        if (snag_session_media(&app->session, path, NULL, prepare_checkpoint, app,
                               &asset, &source, error, error_size) < 0) goto rejected;
        name = snag_json_string(asset, "name");
    } else {
        source = snag_path_root_len(path) ? strdup(path) : snag_path_join(app->session.cwd, path);
    }
    if (!source) goto rejected;
    if (!name || !*name) {
        const char *slash = strrchr(source, '/');
        name = slash ? slash + 1u : source;
    }
    input = snag_open_read(source, false);
    if (input < 0 || snag_fstat(input, &info) < 0 || !S_ISREG(info.st_mode)) {
        (void)snag_errorf(error, error_size, "Download requires a readable regular file: %s",
                          input < 0 ? strerror(errno) : "directories are not supported");
        goto rejected;
    }
    const char *expected = pending ? snag_json_string(pending, "sha256") : NULL;
    if (pending) {
        if (info.st_size != json_integer_value(json_object_get(pending, "bytes")) ||
            info.st_mtime != json_integer_value(json_object_get(pending, "mtime"))) {
            (void)snag_errorf(error, error_size, "queued source changed; left pending: %s", source);
            goto rejected;
        }
        struct snag_sha256 hash;
        snag_sha256_init(&hash);
        unsigned char bytes[65536];
        for (;;) {
            ssize_t n = read(input, bytes, sizeof(bytes));
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) goto rejected;
            if (!n) break;
            snag_sha256_update(&hash, bytes, (size_t)n);
        }
        char actual[SNAG_SHA256_HEX_LEN + 1u];
        snag_sha256_final_hex(&hash, actual);
        if (strcmp(actual, expected)) {
            (void)snag_errorf(error, error_size,
                "queued source digest changed; left pending: %s", source);
            goto rejected;
        }
        if (lseek(input, 0, SEEK_SET) < 0) goto rejected;
        name = snag_json_string(pending, "name");
    }
    struct app_transfer *transfer = transfer_new(app, false);
    if (!transfer) goto rejected;
    transfer->input = input;
    transfer->source = source;
    transfer->name = strdup(name);
    transfer->pending = json_incref((json_t *)pending);
    transfer->allow_legacy = allow_legacy;
    json_decref(asset);
    if (!transfer->name || transfer_start(app, transfer) < 0)
        return transfer_finish(app, transfer, result, unavailable);
    return transfer_wait(app, transfer, result, unavailable);
rejected:
    if (input >= 0) close(input);
    free(source);
    json_decref(asset);
    *result = snag_tool_result_terminal(false, error[0] ? error : "Download failed.");
    return *result ? 0 : -1;
#endif
}

int
snag_app_download(struct app_state *app, const char *path, json_t **result,
                  char *error, size_t error_size)
{
    return app_download(app, path, NULL, true, NULL, result, error, error_size);
}

int
snag_app_download_pending(struct app_state *app, const json_t *item,
                          bool *unavailable, json_t **result,
                          char *error, size_t error_size)
{
    return app_download(app, snag_json_string(item, "path"), item, false, unavailable,
                        result, error, error_size);
}
