/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"
#include "media.h"
#include "tools.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int
attachment_checkpoint(void *opaque, unsigned int timeout_ms)
{
    struct app_state *app = opaque;
    enum snag_term_action action = SNAG_TERM_NONE;
    char *text = NULL;
    (void)timeout_ms;
    if (app->interrupt_requested || app->input_closed || app->shutdown_signal) return 2;
    if (snag_ui_poll(&app->ui, 0, &action, &text) < 0) return -1;
    if (action == SNAG_TERM_EXIT) { app->input_closed = true; free(text); return 2; }
    if (action == SNAG_TERM_CANCEL || action == SNAG_TERM_INTERRUPT) { free(text); return 2; }
    /* A second submission cancels preparation, but remains editable. It is not
     * silently submitted before the first attachment's acceptance boundary. */
    if (text) {
        int rc = snag_ui_send(&app->ui,(struct snag_ui_command){.kind=SNAG_UI_DRAFT,.text=text});
        free(text);
        return rc < 0 ? -1 : 1;
    }
    return 0;
}

static int
show_attachments(struct app_state *app)
{
    struct snag_buf text;
    snag_buf_init(&text, 4096u);
    int rc = snag_buf_printf(&text, "%zu unsent attachment(s); sent only with private Enter/queue/steer.\n",
                              json_array_size(app->draft_content));
    for (size_t i = 0; rc == 0 && i < json_array_size(app->draft_content); ++i) {
        json_t *asset = json_object_get(json_array_get(app->draft_content, i), "asset");
        rc = snag_buf_printf(&text, "%zu: %s, %lld bytes, asset:%s\n", i + 1u,
            snag_json_string(asset, "mime_type"),
            (long long)json_integer_value(json_object_get(asset, "bytes")), snag_json_string(asset, "id"));
        const char *note = snag_json_string(json_array_get(app->draft_content, i), "note");
        if (!rc && note) rc = snag_buf_printf(&text, "   %s\n", note);
    }
    if (rc == 0 && snag_buf_terminate(&text) == 0)
        rc = snag_ui_text(&app->ui, SNAG_UI_HOST, (char *)text.data);
    else rc = -1;
    snag_buf_free(&text);
    return rc;
}

void
snag_app_discard_part(struct app_state *app, const json_t *part)
{
    const json_t *asset = json_object_get(part, "asset");
    const json_t *source = json_object_get(part, "source");
    if (asset) (void)snag_media_discard(app->session.dir_fd, asset);
    if (source) (void)snag_media_discard(app->session.dir_fd, source);
}

int
snag_app_prepare_attachment(struct app_state *app, const char *path, const char *name,
                            int (*pump)(void *, unsigned int), json_t **part,
                            char *error, size_t error_size)
{
    const char *mime = snag_media_mime(name);
    json_t *asset = NULL;
    int rc;
    *part = NULL;
    if (!mime) {
        rc = snag_image_prepare(&app->session, path, 0u, NULL, pump, app,
                                part, error, error_size);
        if (rc >= 0 || errno != ENOTSUP) return rc;
        /* Unknown bytes are retained as a file, not rejected as a bad image. */
        mime = "application/octet-stream";
        error[0] = '\0';
    }
    rc = snag_media_snapshot(app->session.dir_fd, app->session.cwd, path, mime,
                             SNAG_MEDIA_FILE_MAX, pump, app, &asset, error, error_size);
    if (rc == 0) {
        *part = json_pack("{s:s,s:O}", "type", "file", "asset", asset);
        if (!*part) {
            (void)snag_media_discard(app->session.dir_fd, asset);
            rc = snag_errorf(error, error_size, "Cannot record retained attachment");
        }
    }
    json_decref(asset);
    return rc;
}

int
snag_app_media_command(struct app_state *app, const char *line, bool *handled)
{
    bool attach = !strcmp(line, "/attach") || !strncmp(line, "/attach ", 8u);
    bool detach = !strcmp(line, "/detach") || !strncmp(line, "/detach ", 8u);
    bool upload = !strcmp(line, "/upload");
    *handled = attach || detach || upload || !strcmp(line, "/attachments");
    if (!*handled) return 0;
    if (app->attaching) return snag_ui_text(&app->ui, SNAG_UI_ERROR, "Attachment preparation is already active.");
    if (upload) return snag_app_upload_command(app, false);
    if (!attach && !detach) return show_attachments(app);
    const char *arg = strchr(line, ' ');
    if (!arg || !arg[1]) return snag_ui_text(&app->ui, SNAG_UI_ERROR,
        "Use /attach PATH (the entire remainder is a literal path), /attachments, or /detach N|all.");
    ++arg;
    json_t *next = app->draft_content ? json_copy(app->draft_content) : json_array();
    if (!next) return -1;
    if (detach) {
        char *end;
        errno = 0;
        unsigned long n = strtoul(arg, &end, 10);
        if (!strcmp(arg, "all")) {
            json_decref(next); next = json_array();
            if (!next) return -1;
        } else if (errno || *end || !n || n > json_array_size(next) ||
                   json_array_remove(next, (size_t)n - 1u) < 0) {
            json_decref(next);
            return snag_ui_text(&app->ui, SNAG_UI_ERROR, "Attachment number is not in /attachments.");
        }
    } else {
        if (json_array_size(next) >= 8u) {
            json_decref(next);
            return snag_ui_text(&app->ui, SNAG_UI_ERROR, "At most eight files may be attached to one input.");
        }
        char error[256] = "Could not prepare attachment.";
        json_t *part = NULL;
        if(snag_session_persist(&app->store,&app->session,error,sizeof(error))<0) {
            json_decref(next);return snag_ui_text(&app->ui,SNAG_UI_ERROR,error);
        }
        app->attaching = true;
        int rc = snag_app_prepare_attachment(app, arg, arg, attachment_checkpoint,
                                             &part, error, sizeof(error));
        app->attaching = false;
        if (rc || !part) { json_decref(next); return snag_ui_text(&app->ui, SNAG_UI_ERROR, error); }
        if (json_array_append(next, part) < 0) {
            snag_app_discard_part(app, part);
            json_decref(part); json_decref(next); return -1;
        }
        json_decref(part);
        if (!snag_media_content_valid(next)) {
            snag_app_discard_part(app, json_array_get(next, json_array_size(next) - 1u));
            json_decref(next);
            return snag_ui_text(&app->ui, SNAG_UI_ERROR, "Attached images exceed 12 MiB; remove one or use smaller images.");
        }
    }
    json_decref(app->draft_content);
    app->draft_content = json_array_size(next) ? next : NULL;
    if (!app->draft_content) json_decref(next);
    return show_attachments(app);
}
