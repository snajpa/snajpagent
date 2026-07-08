/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
render_event_seq(struct app_state *app, uint64_t seq, const char *type)
{
    return snj_render_event(&app->render, seq, type) < 0 ? -1 : 0;
}

static int
confirm_delete(struct app_state *app, char prefix[9], char *error,
               size_t error_size)
{
    enum snj_term_action action = SNJ_TERM_NONE;
    char *line = NULL;
    int rc;

    memcpy(prefix, app->session.id, 8u);
    prefix[8] = '\0';
    if (snj_render_host(&app->render,
            "delete is irreversible; type the displayed 8-character id prefix to confirm") < 0 ||
        snj_term_set_prompt(&app->term, false) < 0) {
        snprintf(error, error_size, "delete confirmation prompt could not be displayed");
        return -1;
    }
    rc = snj_term_poll(&app->term, -1, &action, &line);
    if (rc < 0) {
        free(line);
        snprintf(error, error_size, "delete confirmation input could not be read");
        return -1;
    }
    if (action == SNJ_TERM_INTERRUPT || action == SNJ_TERM_EXIT || !line) {
        free(line);
        snprintf(error, error_size, "delete cancelled");
        return 1;
    }
    if (strcmp(line, prefix) != 0) {
        free(line);
        snprintf(error, error_size, "delete confirmation did not match %.8s",
                 app->session.id);
        errno = EINVAL;
        return 1;
    }
    free(line);
    return 0;
}

int
snj_app_lifecycle_command(struct app_state *app, const char *line,
                          bool *handled, bool *exit_now)
{
    char error[256];
    uint64_t seq;

    *handled = true;
    *exit_now = false;
    if (strcmp(line, "/archive") == 0) {
        seq = app->session.next_seq;
        if (snj_session_archive(&app->session, &seq, error, sizeof(error)) < 0) {
            (void)snj_render_error_ctx(&app->render, error);
            return -1;
        }
        if (render_event_seq(app, seq, "session_archived") < 0 ||
            snj_render_host(&app->render, "session archived") < 0)
            return -1;
        *exit_now = true;
        return 0;
    }
    if (strcmp(line, "/compact") == 0) {
        if (snj_app_compact_idle_command(app, "manual",
                                         error, sizeof(error)) < 0) {
            (void)snj_render_error_ctx(&app->render, error);
            return -1;
        }
        return 0;
    }
    if (strcmp(line, "/delete") == 0) {
        char prefix[9];
        int confirm_rc = confirm_delete(app, prefix, error, sizeof(error));
        if (confirm_rc != 0) {
            (void)snj_render_error_ctx(&app->render, error);
            return confirm_rc < 0 ? -1 : 0;
        }
        seq = app->session.next_seq;
        if (snj_session_delete(&app->store, &app->session, prefix, &seq,
                               error, sizeof(error)) < 0) {
            (void)snj_render_error_ctx(&app->render, error);
            return -1;
        }
        if (render_event_seq(app, seq, "session_delete_requested") < 0 ||
            snj_render_host(&app->render, "session deleted") < 0)
            return -1;
        *exit_now = true;
        return 0;
    }
    *handled = false;
    return 0;
}
