/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"
#include "base.h"
#include "config.h"
#include "context.h"
#include "credential.h"
#include "json.h"
#include "provider.h"
#include "render.h"
#include "secret.h"
#include "snajpagent.h"
#include "store.h"
#include "turn.h"
#include "tools.h"
#include "wire.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <langinfo.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
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
static int
app_error(struct app_state *app, const char *message)
{
    return snj_render_error_ctx(&app->render, message);
}
static int
app_warning(struct app_state *app, const char *message)
{
    return snj_render_warning_ctx(&app->render, message);
}
static int
app_hostf(struct app_state *app, const char *fmt, ...)
{
    struct snj_buf text;
    va_list ap;
    int needed;
    int rc;
    snj_buf_init(&text, 4u * 1024u * 1024u);
    va_start(ap, fmt);
    needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed < 0 || (size_t)needed > text.max) {
        snj_buf_free(&text);
        errno = EOVERFLOW;
        return -1;
    }
    if (snj_buf_reserve(&text, (size_t)needed + 1u) < 0) {
        snj_buf_free(&text);
        return -1;
    }
    va_start(ap, fmt);
    (void)vsnprintf((char *)text.data, (size_t)needed + 1u, fmt, ap);
    va_end(ap);
    text.len = (size_t)needed;
    rc = snj_render_host(&app->render, (const char *)text.data);
    snj_buf_free(&text);
    return rc;
}
static int
app_runtimef(struct app_state *app, const char *fmt, ...)
{
    struct snj_buf text;
    va_list ap;
    int needed;
    int rc;
    if (app->render.verbosity < 3u)
        return 0;
    snj_buf_init(&text, 4u * 1024u * 1024u);
    va_start(ap, fmt);
    needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed < 0 || (size_t)needed > text.max) {
        snj_buf_free(&text);
        errno = EOVERFLOW;
        return -1;
    }
    if (snj_buf_reserve(&text, (size_t)needed + 1u) < 0) {
        snj_buf_free(&text);
        return -1;
    }
    va_start(ap, fmt);
    (void)vsnprintf((char *)text.data, (size_t)needed + 1u, fmt, ap);
    va_end(ap);
    text.len = (size_t)needed;
    rc = snj_render_runtime(&app->render, (const char *)text.data);
    snj_buf_free(&text);
    return rc;
}
static void
usage_number(char out[32], bool known, uint64_t value)
{
    if (known)
        (void)snprintf(out, 32u, "%llu", (unsigned long long)value);
    else
        (void)snprintf(out, 32u, "?");
}
static const char *
graph_outcome_name(enum snj_graph_outcome outcome)
{
    switch (outcome) {
    case SNJ_GRAPH_CALLS: return "calls";
    case SNJ_GRAPH_FINAL: return "final";
    case SNJ_GRAPH_REFUSAL: return "refusal";
    case SNJ_GRAPH_NONPRODUCTIVE: return "nonproductive";
    case SNJ_GRAPH_CONFLICT: return "conflict";
    }
    return "unknown";
}
static const char *
resolve_model(const struct snj_config *config, const char *selector)
{
    if (!selector || strcmp(selector, "default") == 0 ||
        strcmp(selector, SNAJPAGENT_MODEL) == 0)
        return SNAJPAGENT_MODEL;
    if (strcmp(selector, SNAJPAGENT_MODEL_SELECTOR) == 0) {
        if (config && !config->provider_exact_token_count &&
            !config->provider_native_compaction)
            return SNAJPAGENT_MODEL_SELECTOR;
        return SNAJPAGENT_MODEL;
    }
    return NULL;
}
static const char *
resolve_effort(const char *preference)
{
    static const char *const supported[] = {
        "none", "low", "medium", "high", "xhigh"
    };
    if (!preference || strcmp(preference, "default") == 0)
        return "medium";
    for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); ++i)
        if (strcmp(preference, supported[i]) == 0)
            return supported[i];
    return NULL;
}
static int
prepare_turn_settings(struct app_state *app, char *error, size_t error_size)
{
    const char *model = app->staged_model ? app->staged_model :
                                           app->session.default_model;
    const char *resolved_model = app->staged_model ?
        resolve_model(app->config, app->staged_model) : app->session.default_model;
    const char *effort_preference = app->staged_effort ? app->staged_effort :
                                                        app->session.default_effort;
    const char *effort = resolve_effort(effort_preference);
    if (!resolved_model || resolved_model[0] == '\0') {
        set_error(error, error_size,
                  "%s is unavailable for new work; choose an active model",
                  model);
        errno = ENOTSUP;
        return -1;
    }
    if (!effort) {
        set_error(error, error_size,
                  "reasoning effort %s is unsupported by %s",
                  effort_preference, model);
        errno = ENOTSUP;
        return -1;
    }
    app->turn_model = resolved_model;
    app->turn_effort = effort;
    return 0;
}
static void
consume_staged_settings(struct app_state *app)
{
    app->staged_model = NULL;
    app->staged_effort = NULL;
}
static int
commit_event(struct app_state *app, const char *type, json_t *data,
             char *error, size_t error_size)
{
    uint64_t seq;
    if (snj_session_commit(&app->session, type, data, &seq,
                           error, error_size) < 0)
        return -1;
    if (snj_render_event(&app->render, seq, type) < 0) {
        set_error(error, error_size, "durable event output failed");
        return -1;
    }
    return 0;
}
static int
render_queue(struct app_state *app)
{
    if (app->session.pending_queue_count == 0u)
        return app_warning(app, "future-turn queue is empty");
    for (size_t i = 0; i < app->session.pending_queue_count; ++i) {
        char label[64];
        (void)snprintf(label, sizeof(label), "next %.8s › ",
                       app->session.pending_queue[i].queue_id);
        if (snj_render_submitted(&app->render, label,
                                 app->session.pending_queue[i].text) < 0)
            return -1;
    }
    return 0;
}
static int
queue_future_turn(struct app_state *app, const char *text, bool arm,
                  char *error, size_t error_size)
{
    char queue_id[SNJ_ID_HEX_LEN + 1u];
    const char *queued_text = text;
    size_t len;
    if (!app->session.active_turn) {
        set_error(error, error_size, "/queue TEXT is valid only while a turn is active");
        errno = EINVAL;
        return 1;
    }
    if (queued_text[0] == '/') {
        if (queued_text[1] != '/') {
            set_error(error, error_size,
                      "queued text starting with / must use // for a literal slash");
            errno = EINVAL;
            return 1;
        }
        ++queued_text;
    }
    len = strlen(queued_text);
    if (!len || len > SNJ_MAX_QUEUED_TEXT ||
        !snj_utf8_valid((const unsigned char *)queued_text, len, true)) {
        set_error(error, error_size,
                  "queued text must be nonempty valid UTF-8 within 256 KiB");
        errno = EINVAL;
        return 1;
    }
    if (snj_random_id(queue_id) < 0) {
        set_error(error, error_size, "cryptographic queue id generation failed");
        return -1;
    }
    if (commit_event(app, "future_turn_queued",
                     snj_app_future_turn_queued_data(app->session.active_turn_id,
                                             queue_id, queued_text),
                     error, error_size) < 0)
        return -1;
    if (snj_render_submitted(&app->render, "next › ", queued_text) < 0) {
        set_error(error, error_size, "queued turn acknowledgement could not be rendered");
        return -1;
    }
    if (arm)
        app->queue_armed = true;
    return 0;
}
static bool
lower_hex_prefix(const char *s)
{
    size_t len = strlen(s);
    if (len < 8u || len > SNJ_ID_HEX_LEN)
        return false;
    for (size_t i = 0; i < len; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') ||
              (s[i] >= 'a' && s[i] <= 'f')))
            return false;
    return true;
}
static int
cancel_future_turns(struct app_state *app, const char *selector,
                    char *error, size_t error_size)
{
    bool remove[SNJ_MAX_PENDING_TURNS] = {false};
    size_t matches = 0;
    if (strcmp(selector, "all") == 0) {
        if (app->session.pending_queue_count == 0u) {
            set_error(error, error_size, "future-turn queue is empty");
            return 1;
        }
        for (size_t i = 0; i < app->session.pending_queue_count; ++i)
            remove[i] = true;
        matches = app->session.pending_queue_count;
    } else {
        size_t len;
        if (!lower_hex_prefix(selector)) {
            set_error(error, error_size,
                      "/unqueue expects all or an 8..32 lowercase-hex id prefix");
            return 1;
        }
        len = strlen(selector);
        for (size_t i = 0; i < app->session.pending_queue_count; ++i) {
            if (strncmp(app->session.pending_queue[i].queue_id, selector, len) == 0) {
                remove[i] = true;
                ++matches;
            }
        }
        if (matches != 1u) {
            set_error(error, error_size, matches ?
                      "queue id prefix is ambiguous" : "queue id prefix was not found");
            return 1;
        }
    }
    if (commit_event(app, "future_turn_cancelled",
                     snj_app_future_turn_cancelled_data(&app->session, remove),
                     error, error_size) < 0)
        return -1;
    if (app->session.pending_queue_count == 0u)
        app->queue_armed = false;
    {
        char message[96];
        (void)snprintf(message, sizeof(message), "%zu future turn%s cancelled",
                       matches, matches == 1u ? "" : "s");
        return app_warning(app, message);
    }
}
static const char *
next_model(const struct app_state *app)
{
    return app->staged_model ? app->staged_model : app->session.default_model;
}
static const char *
next_effort(const struct app_state *app)
{
    return app->staged_effort ? app->staged_effort :
                                app->session.default_effort;
}
static int
render_status(struct app_state *app)
{
    const char *id = app->render.verbosity >= 3u ? app->session.id : NULL;
    return app_hostf(app,
        "session: %s\n"
        "state: %s\n"
        "model: %s%s\n"
        "effort: %s%s\n"
        "workspace: %s\n"
        "turns: %llu\n"
        "queue: %zu%s\n"
        "verbosity: %u\n"
        "context: not counted",
        id ? id : (char [9]){app->session.id[0], app->session.id[1],
                             app->session.id[2], app->session.id[3],
                             app->session.id[4], app->session.id[5],
                             app->session.id[6], app->session.id[7], '\0'},
        app->session.active_turn ? "active" : "idle",
        next_model(app), app->staged_model ? " (staged once)" : "",
        next_effort(app), app->staged_effort ? " (staged once)" : "",
        app->session.workspace,
        (unsigned long long)app->session.turn_count,
        app->session.pending_queue_count,
        app->session.pending_queue_count && !app->queue_armed ? " paused" : "",
        app->render.verbosity);
}
static int
render_help(struct app_state *app)
{
    static const char text[] =
        "/help                     commands and keys\n"
        "/status                   session and next-turn settings\n"
        "/history                  recent terminal history\n"
        "/model [MODEL]            show or set next-turn model\n"
        "/effort [LEVEL]           show or set next-turn effort\n"
        "/verbose [0..6]           show or set this process's verbosity\n"
        "/queue [TEXT]             list or queue future turns\n"
        "/unqueue ID|all           cancel queued turns\n"
        "/next                     run the oldest paused turn\n"
        "/archive                  archive idle session and exit\n"
        "/delete                   delete idle session after confirmation\n"
        "/exit                     preserve session and exit\n"
        "Enter submit/steer · Tab indent/queue · Ctrl-C clear/interrupt · Ctrl-J newline";
    return snj_render_host(&app->render, text);
}
static int
show_setting(struct app_state *app, const char *name, const char *value,
             bool staged)
{
    return app_hostf(app, "%s for next turn: %s%s", name, value,
                     staged ? " (staged once)" : "");
}
static int
change_model(struct app_state *app, const char *selector, bool active)
{
    const char *resolved;
    char error[256];
    if (!selector)
        return show_setting(app, "model", next_model(app),
                            app->staged_model != NULL);
    if (active)
        return app_error(app, "/model MODEL is idle-only; interrupt or wait");
    if (strchr(selector, ' ') ||
        !(resolved = resolve_model(app->config, selector)))
        return app_error(app,
            "model selector is not in the compiled release profile");
    app->staged_model = NULL;
    if (strcmp(resolved, app->session.default_model) != 0) {
        error[0] = '\0';
        if (commit_event(app, "model_changed",
                snj_app_preference_changed_data("old_model",
                                        app->session.default_model,
                                        "new_model", resolved),
                error, sizeof(error)) < 0) {
            (void)app_error(app, error[0] ? error :
                            "model preference could not be saved");
            return -1;
        }
    }
    return show_setting(app, "model", app->session.default_model, false);
}
static int
change_effort(struct app_state *app, const char *value, bool active)
{
    char error[256];
    if (!value)
        return show_setting(app, "effort", next_effort(app),
                            app->staged_effort != NULL);
    if (active)
        return app_error(app, "/effort LEVEL is idle-only; interrupt or wait");
    if (strchr(value, ' ') || !resolve_effort(value))
        return app_error(app,
            "effort is unsupported by the selected model");
    app->staged_effort = NULL;
    if (strcmp(value, app->session.default_effort) != 0) {
        error[0] = '\0';
        if (commit_event(app, "effort_changed",
                snj_app_preference_changed_data("old_effort",
                                        app->session.default_effort,
                                        "new_effort", value),
                error, sizeof(error)) < 0) {
            (void)app_error(app, error[0] ? error :
                            "effort preference could not be saved");
            return -1;
        }
    }
    return show_setting(app, "effort", app->session.default_effort, false);
}
static int
change_verbosity(struct app_state *app, const char *value)
{
    if (!value)
        return app_hostf(app, "verbosity: %u", app->render.verbosity);
    if (value[0] < '0' || value[0] > '6' || value[1] != '\0')
        return app_error(app, "/verbose expects one integer from 0 through 6");
    app->render.verbosity = (unsigned int)(value[0] - '0');
    return app_hostf(app, "verbosity: %u", app->render.verbosity);
}
static int
handle_common_command(struct app_state *app, const char *line, bool active,
                      bool *handled)
{
    *handled = true;
    if (strcmp(line, "/help") == 0)
        return render_help(app);
    if (strcmp(line, "/status") == 0)
        return render_status(app);
    if (strcmp(line, "/history") == 0)
        return snj_render_history(&app->render, &app->session);
    if (strcmp(line, "/verbose") == 0)
        return change_verbosity(app, NULL);
    if (strncmp(line, "/verbose ", 9u) == 0)
        return change_verbosity(app, line + 9u);
    if (strcmp(line, "/model") == 0)
        return change_model(app, NULL, active);
    if (strncmp(line, "/model ", 7u) == 0)
        return change_model(app, line + 7u, active);
    if (strcmp(line, "/effort") == 0)
        return change_effort(app, NULL, active);
    if (strncmp(line, "/effort ", 8u) == 0)
        return change_effort(app, line + 8u, active);
    *handled = false;
    return 0;
}
int
snj_app_active_input_pump(void *opaque, unsigned int timeout_ms)
{
    struct app_state *app = opaque;
    enum snj_term_action action = SNJ_TERM_NONE;
    char *line = NULL;
    char error[256];
    int rc;
    if (app->execute || app->input_closed)
        return 0;
    rc = snj_term_poll(&app->term, (int)timeout_ms, &action, &line);
    if (rc < 0) {
        (void)snj_render_error_ctx(&app->render,
            errno == EOVERFLOW ? "active submission exceeds 1 MiB" :
            errno == EILSEQ ? "active submission contains invalid UTF-8" :
            "active input could not be read");
        return 0;
    }
    if (rc == 0) {
        uint64_t now = snj_time_ms();
        if (!app->activity_shown && now >= app->active_since_ms &&
            now - app->active_since_ms >= 750u) {
            if (snj_render_activity(&app->render, "working…") < 0)
                return -1;
            app->activity_shown = true;
        }
        return 0;
    }
    if (action == SNJ_TERM_EXIT) {
        app->input_closed = true;
        free(line);
        return 0;
    }
    if (action == SNJ_TERM_INTERRUPT) {
        app->interrupt_requested = true;
        if (snj_render_activity(&app->render, "interrupting…") < 0)
            return -1;
        free(line);
        return 2;
    }
    if (!line)
        return 0;
    error[0] = '\0';
    if (action == SNJ_TERM_QUEUE) {
        rc = queue_future_turn(app, line, true, error, sizeof(error));
        if (rc != 0) {
            (void)snj_render_error_ctx(&app->render, error);
            if (snj_term_set_prompt(&app->term, true) < 0 ||
                snj_term_restore_draft(&app->term, line) < 0)
                rc = -1;
        } else {
            (void)snj_term_history_add(&app->term, line);
            rc = snj_term_set_prompt(&app->term, true);
        }
    } else {
        bool single_line = strchr(line, '\n') == NULL;
        bool handled = false;
        if (single_line && line[0] == '/' && line[1] != '/') {
            rc = handle_common_command(app, line, true, &handled);
            if (rc < 0)
                goto active_done;
        }
        if (handled) {
            rc = snj_term_set_prompt(&app->term, true);
        } else if (single_line && strcmp(line, "/queue") == 0) {
            rc = render_queue(app);
            if (snj_term_set_prompt(&app->term, true) < 0)
                rc = -1;
        } else if (single_line && strncmp(line, "/queue ", 7u) == 0) {
            rc = queue_future_turn(app, line + 7u, true, error, sizeof(error));
            if (rc != 0)
                (void)snj_render_error_ctx(&app->render, error);
            if (snj_term_set_prompt(&app->term, true) < 0)
                rc = -1;
        } else if (single_line && strncmp(line, "/unqueue ", 9u) == 0) {
            rc = cancel_future_turns(app, line + 9u, error, sizeof(error));
            if (rc != 0)
                (void)snj_render_error_ctx(&app->render, error);
            if (snj_term_set_prompt(&app->term, true) < 0)
                rc = -1;
        } else if (single_line && line[0] == '/' && line[1] != '/') {
            (void)snj_render_error_ctx(&app->render,
                "that command is unavailable while a turn is active");
            rc = snj_term_set_prompt(&app->term, true);
        } else {
            const char *text = line[0] == '/' && line[1] == '/' ? line + 1 : line;
            char steering_id[SNJ_ID_HEX_LEN + 1u];
            size_t len = strlen(text);
            if (!len || len > SNJ_MAX_STEERING_TEXT) {
                (void)snj_render_error_ctx(&app->render,
                    "steering must be nonempty valid UTF-8 within 256 KiB");
                rc = 0;
            } else if (snj_random_id(steering_id) < 0) {
                rc = -1;
            } else {
                rc = commit_event(app, "steering_added",
                        snj_app_steering_added_data(app->session.active_turn_id,
                                            steering_id, text),
                        error, sizeof(error));
                if (rc == 0)
                    rc = snj_render_submitted(&app->render, "steer › ", text);
                if (rc < 0) {
                    (void)snj_render_error_ctx(&app->render, error[0] ? error :
                                               "steering could not be persisted");
                    if (snj_term_set_prompt(&app->term, true) == 0)
                        (void)snj_term_restore_draft(&app->term, line);
                } else {
                    (void)snj_term_history_add(&app->term, line);
                    app->steering_requested = true;
                }
            }
            if (!app->steering_requested && rc >= 0 &&
                snj_term_set_prompt(&app->term, true) < 0)
                rc = -1;
        }
    }
active_done:
    free(line);
    if (rc < 0)
        return -1;
    return app->steering_requested ? 1 : 0;
}
static int
commit_pending_result(struct app_state *app, const char *turn_id,
                      const char *call_id, json_t *result,
                      char *error, size_t error_size)
{
    json_t *data = snj_app_tool_finished_data(turn_id, call_id, result);
    if (!data) {
        set_error(error, error_size, "cannot allocate tool completion event");
        return -1;
    }
    return commit_event(app, "tool_finished", data, error, error_size);
}
static int
terminalize_pending(struct app_state *app, const char *turn_id,
                    const char *unstarted_reason, char *error,
                    size_t error_size)
{
    size_t count = app->session.pending_call_count;
    for (size_t i = 0; i < count; ++i) {
        struct snj_pending_call *call = &app->session.pending_calls[i];
        json_t *result;
        if (call->finished)
            continue;
        result = call->started ? snj_tool_result_outcome_unknown("owner_lost") :
                                snj_tool_result_not_run(unstarted_reason);
        if (!result || commit_pending_result(app, turn_id, call->call_id, result,
                                             error, error_size) < 0)
            return -1;
    }
    return 0;
}
static int
close_active_process_for_turn(struct app_state *app, const char *turn_id,
                              const char *cause, bool user_interrupt,
                              char *error, size_t error_size)
{
    char handle[SNJ_ID_HEX_LEN + 1u];
    json_t *result = NULL;
    json_t *data;
    if (!app->session.active_process_handle[0])
        return 0;
    memcpy(handle, app->session.active_process_handle, sizeof(handle));
#ifdef SNAJPAGENT_TEST_FIXTURE
    (void)user_interrupt;
    result = snj_tool_result_outcome_unknown("owner_lost");
#else
    if (snj_tools_close_managed(handle, user_interrupt, snj_app_active_input_pump, app,
                                &result, error, error_size) < 0)
        return -1;
#endif
    data = snj_app_process_closed_data(turn_id, handle, cause, result);
    if (!data) {
        set_error(error, error_size, "cannot allocate process closure event");
        return -1;
    }
    return commit_event(app, "process_closed", data, error, error_size);
}
static int
recover_session(struct app_state *app, char *error, size_t error_size)
{
    char turn_id[SNJ_ID_HEX_LEN + 1u];
    const char *message;
    bool has_steering;
    if (!app->session.active_turn)
        return 0;
    memcpy(turn_id, app->session.active_turn_id, sizeof(turn_id));
    has_steering = app->session.pending_steering_count != 0u;
    if (app->session.response_open) {
        if (commit_event(app, "response_interrupted",
                         snj_app_response_interrupted_data(turn_id,
                                                   app->session.active_response_id,
                                                   app->session.active_cycle,
                                                   "recovery", "process_lost",
                                                   NULL),
                         error, error_size) < 0 ||
            close_active_process_for_turn(app, turn_id, "internal_failure",
                                          false, error, error_size) < 0 ||
            commit_event(app, "turn_interrupted", snj_app_turn_interrupted_data(turn_id, "recovery", "session_recovered"),
                         error, error_size) < 0)
            return -1;
        return app_warning(app, "recovered an interrupted turn");
    }
    if (app->session.response_complete) {
        if (app->session.response_outcome == SNJ_GRAPH_CONFLICT) {
            message = "provider response had conflicting terminal actions";
            if (terminalize_pending(app, turn_id, "protocol_conflict",
                                    error, error_size) < 0 ||
                close_active_process_for_turn(app, turn_id, "protocol_failure",
                                              false, error, error_size) < 0 ||
                commit_event(app, "turn_failed",
                             snj_app_turn_failed_data(turn_id, "protocol", message),
                             error, error_size) < 0)
                return -1;
            return app_warning(app, "recovered a protocol-conflicted turn");
        }
        if (has_steering) {
            if (app->session.response_outcome == SNJ_GRAPH_CALLS &&
                terminalize_pending(app, turn_id, "superseded_by_steering",
                                    error, error_size) < 0)
                return -1;
            if (close_active_process_for_turn(app, turn_id, "internal_failure",
                                              false, error, error_size) < 0 ||
                commit_event(app, "turn_interrupted", snj_app_turn_interrupted_data(turn_id, "recovery", "session_recovered"),
                             error, error_size) < 0)
                return -1;
            return app_warning(app,
                "recovered a turn whose pending steering could not be resumed automatically");
        }
        switch (app->session.response_outcome) {
        case SNJ_GRAPH_FINAL:
        case SNJ_GRAPH_REFUSAL:
            if (app->session.active_process_handle[0] != '\0') {
                message = "recovered terminal response while a managed process was unresolved";
                if (close_active_process_for_turn(app, turn_id, "protocol_failure",
                                                  false, error, error_size) < 0 ||
                    commit_event(app, "turn_failed",
                                 snj_app_turn_failed_data(turn_id, "protocol", message),
                                 error, error_size) < 0)
                    return -1;
                return app_warning(app, "recovered a terminal response that violated managed process ordering");
            }
            if (commit_event(app, "turn_completed",
                             snj_app_turn_completed_data(turn_id,
                                                 app->session.final_response_id,
                                                 app->session.final_item_id),
                             error, error_size) < 0)
                return -1;
            return app_warning(app, "recovered a durably completed turn");
        case SNJ_GRAPH_CALLS:
            if (terminalize_pending(app, turn_id, "recovery_unstarted",
                                    error, error_size) < 0 ||
                close_active_process_for_turn(app, turn_id, "internal_failure",
                                              false, error, error_size) < 0 ||
                commit_event(app, "turn_interrupted", snj_app_turn_interrupted_data(turn_id, "recovery", "session_recovered"),
                             error, error_size) < 0)
                return -1;
            return app_warning(app, "recovered a turn with unfinished tool work");
        case SNJ_GRAPH_CONFLICT:
            break;
        case SNJ_GRAPH_NONPRODUCTIVE:
            message = "provider completed without a final answer, refusal, or tool call";
            if (close_active_process_for_turn(app, turn_id, "protocol_failure",
                                              false, error, error_size) < 0 ||
                commit_event(app, "turn_failed",
                             snj_app_turn_failed_data(turn_id, "protocol", message),
                             error, error_size) < 0)
                return -1;
            return app_warning(app, "recovered a nonproductive response");
        }
    }
    if (app->session.response_terminal == SNJ_RESPONSE_TERMINAL_FAILED) {
        message = "provider response had already failed before process recovery";
        if (close_active_process_for_turn(app, turn_id, "provider_failure",
                                          false, error, error_size) < 0 ||
            commit_event(app, "turn_failed",
                         snj_app_turn_failed_data(turn_id, "provider", message),
                         error, error_size) < 0)
            return -1;
        return app_warning(app, "recovered a provider-failed turn");
    }
    if (close_active_process_for_turn(app, turn_id, "internal_failure",
                                      false, error, error_size) < 0 ||
        commit_event(app, "turn_interrupted", snj_app_turn_interrupted_data(turn_id, "recovery", "session_recovered"),
                     error, error_size) < 0)
        return -1;
    return app_warning(app, "recovered an interrupted turn");
}
static int
execute_calls(struct app_state *app, const char *turn_id,
              const struct snj_response_graph *graph,
              const struct snj_credential *credential,
              char *error, size_t error_size)
{
    for (size_t i = 0; i < graph->count; ++i) {
        const struct snj_response_item *call = &graph->items[i];
        char digest[SNJ_SHA256_HEX_LEN + 1u];
        json_t *result = NULL;
        char tool_error[256];
        if (call->kind != SNJ_ITEM_TOOL_CALL)
            continue;
        if (snj_tool_action_digest(call, app->session.workspace, digest) < 0) {
            set_error(error, error_size, "cannot digest tool action");
            return -1;
        }
        if (commit_event(app, "tool_started",
                         snj_app_tool_started_data(turn_id, call->call_id, digest,
                                           app->session.workspace),
                         error, error_size) < 0)
            return -1;
        if (snj_render_tool_start(&app->render, call,
                                  app->session.workspace) < 0) {
            set_error(error, error_size, "tool activity could not be rendered");
            return -1;
        }
        tool_error[0] = '\0';
        {
            int run_rc = snj_app_tool_run(app, call, credential, &result, tool_error,
                                  sizeof(tool_error));
            if (run_rc == 2) {
                if (!result)
                    result = snj_tool_result_not_run("turn_cancelled");
                if (!result ||
                    commit_pending_result(app, turn_id, call->call_id, result,
                                          error, error_size) < 0 ||
                    terminalize_pending(app, turn_id, "turn_cancelled",
                                        error, error_size) < 0 ||
                    close_active_process_for_turn(app, turn_id, "user_interrupt",
                                                  true, error, error_size) < 0 ||
                    commit_event(app, "turn_interrupted",
                                 snj_app_turn_interrupted_data(turn_id, "user", "cancelled"),
                                 error, error_size) < 0)
                    return -1;
                set_error(error, error_size, "turn cancelled");
                return 2;
            }
            if (run_rc < 0) {
                if (result) {
                    json_decref(result);
                    result = NULL;
                }
                if (!tool_error[0])
                    (void)snprintf(tool_error, sizeof(tool_error),
                                   "tool adapter failed");
                result = snj_tool_result_terminal(false, tool_error);
            }
        }
        if (!result)
            return -1;
        {
            const char *status = snj_json_string(result, "status");
            bool yielded = status && strcmp(status, "running") == 0;
            json_t *render_result = app->render.verbosity >= 1u ?
                                    json_deep_copy(result) : NULL;
            if (commit_pending_result(app, turn_id, call->call_id, result,
                                      error, error_size) < 0) {
                if (render_result)
                    json_decref(render_result);
                return -1;
            }
            if (render_result &&
                snj_render_tool_finish(&app->render, call->name,
                                       render_result) < 0) {
                json_decref(render_result);
                set_error(error, error_size,
                          "tool result could not be rendered");
                return -1;
            }
            if (render_result)
                json_decref(render_result);
            if (yielded &&
                terminalize_pending(app, turn_id,
                                    "process_interaction_required",
                                    error, error_size) < 0)
                return -1;
            if (yielded)
                return 0;
        }
    }
    return 0;
}
static int
run_turn(struct app_state *app, const char *prompt,
         const struct snj_queued_turn *queued)
{
    char turn_id[SNJ_ID_HEX_LEN + 1u];
    char response_id[SNJ_ID_HEX_LEN + 1u];
    char input_hash[SNJ_SHA256_HEX_LEN + 1u];
    char request_hash[SNJ_SHA256_HEX_LEN + 1u];
    char count_request_hash[SNJ_SHA256_HEX_LEN + 1u];
    char error[256];
    uint64_t input_tokens_bound = 0;
    char *turn_prompt;
    struct snj_credential credential;
    size_t prompt_max = queued ? SNJ_MAX_QUEUED_TEXT : SNJ_MAX_DIRECT_PROMPT;
    int result = 4;
    snj_credential_clear(&credential);
    error[0] = '\0';
    if (!*prompt || strlen(prompt) > prompt_max ||
        !snj_utf8_valid((const unsigned char *)prompt, strlen(prompt), true)) {
        (void)app_error(app, queued ?
            "queued prompt must be nonempty valid UTF-8 within 256 KiB" :
            "prompt must be nonempty valid UTF-8 within 1 MiB");
        return 2;
    }
    if (prepare_turn_settings(app, error, sizeof(error)) < 0) {
        (void)app_error(app, error);
        return 2;
    }
#ifndef SNAJPAGENT_TEST_FIXTURE
    if (snj_credential_read(&credential, app->config->provider_api_key_env,
                            error, sizeof(error)) < 0) {
        (void)app_error(app, error);
        snj_credential_clear(&credential);
        return 2;
    }
#endif
    turn_prompt = snj_strdup_checked(prompt, prompt_max);
    if (!turn_prompt) {
        (void)app_error(app, "cannot retain turn input");
        return 3;
    }
    if (snj_instructions_discover(&app->turn_instructions, app->session.workspace,
                                  error, sizeof(error)) < 0) {
        (void)app_error(app, error);
        result = 3;
        goto out;
    }
    if (snj_random_id(turn_id) < 0) {
        (void)app_error(app, "cryptographic turn id generation failed");
        result = 3;
        goto out;
    }
    if (commit_event(app, "turn_started",
                     snj_app_turn_started_data(app, turn_prompt, turn_id, queued),
                     error, sizeof(error)) < 0) {
        (void)app_error(app, error);
        result = 3;
        goto out;
    }
    consume_staged_settings(app);
    if (app_runtimef(app,
            "turn › %s started · model=%s · effort=%s · workspace=%s",
            turn_id, app->turn_model, app->turn_effort, app->session.workspace) < 0) {
        (void)app_error(app, "turn runtime facts could not be rendered");
        result = 6;
        goto out;
    }
    if (!app->execute && snj_term_set_prompt(&app->term, true) < 0) {
        (void)app_error(app, "active composer could not be displayed");
        result = 6;
        goto out;
    }
    for (unsigned int cycle = 1u; cycle <= SNJ_MAX_RESPONSE_CYCLES; ++cycle) {
        struct snj_response_graph graph;
        struct snj_graph_decision decision;
        json_t *steering = NULL;
        json_t *create_request = NULL;
        json_t *count_request = NULL;
        const char *count_method = "qualified_upper_bound";
        struct snj_buf request_body;
        uint64_t response_begin_ms;
        unsigned int provider_retry_count = 0u;
        int provider_rc;
        snj_response_graph_init(&graph);
        snj_buf_init(&request_body, SNJ_WIRE_BODY_MAX);
        steering = snj_app_steering_snapshot(&app->session);
        error[0] = '\0';
        if (!steering || snj_random_id(response_id) < 0 ||
            snj_app_request_digests(app, turn_prompt, steering, cycle, &credential,
                            input_hash, request_hash, count_request_hash,
                            &input_tokens_bound,
                            &request_body, &create_request, &count_request,
                            error, sizeof(error)) < 0) {
            snj_app_response_cycle_release(app, &graph, &steering,
                                           &create_request, &count_request,
                                           &request_body);
            if (commit_event(app, "turn_failed",
                             snj_app_turn_failed_data(turn_id, "context",
                                 error[0] ? error :
                                 "response context projection failed"),
                             error, sizeof(error)) < 0) {
                (void)app_error(app, error);
                result = 3;
            } else {
                (void)app_error(app, error[0] ? error :
                                "response context projection failed");
                result = 4;
            }
            goto out;
        }
#ifndef SNAJPAGENT_TEST_FIXTURE
        if (app->config->provider_exact_token_count) {
#endif
            provider_rc = snj_app_provider_count(app, count_request, &credential,
                                         &input_tokens_bound, error, sizeof(error));
            if (provider_rc < 0) {
                snj_app_response_cycle_release(app, &graph, &steering,
                                               &create_request, &count_request,
                                               &request_body);
                if (commit_event(app, "turn_failed",
                                 snj_app_turn_failed_data(turn_id, "provider",
                                     error[0] ? error :
                                     "input-token count failed"),
                                 error, sizeof(error)) < 0) {
                    (void)app_error(app, error);
                    result = 3;
                } else {
                    (void)app_error(app, error[0] ? error :
                                    "input-token count failed");
                    result = 4;
                }
                goto out;
            }
#ifndef SNAJPAGENT_TEST_FIXTURE
            count_method = "exact";
        }
#endif
        {
            bool compacted = false;
            if (snj_app_compact_before_response(app, &credential,
                    input_tokens_bound, count_method, &compacted,
                    error, sizeof(error)) < 0) {
                snj_app_response_cycle_release(app, &graph, &steering,
                                               &create_request, &count_request,
                                               &request_body);
                if (commit_event(app, "turn_failed",
                                 snj_app_turn_failed_data(turn_id, "provider",
                                     error[0] ? error :
                                     "pre-response compaction failed"),
                                 error, sizeof(error)) < 0) {
                    (void)app_error(app, error);
                    result = 3;
                } else {
                    (void)app_error(app, error[0] ? error :
                                    "pre-response compaction failed");
                    result = 4;
                }
                goto out;
            }
            if (compacted) {
                snj_app_response_cycle_release(app, &graph, &steering,
                                               &create_request, &count_request,
                                               &request_body);
                --cycle;
                continue;
            }
        }
        if (commit_event(app, "response_started",
                         snj_app_response_started_data(turn_id, response_id, cycle,
                                               app->session.compact_id,
                                               app->turn_model,
                                               input_hash, request_hash,
                                               count_request_hash, count_method,
                                               input_tokens_bound,
                                               steering),
                         error, sizeof(error)) < 0) {
            snj_app_response_cycle_release(app, &graph, &steering,
                                           &create_request, &count_request,
                                           &request_body);
            (void)app_error(app, error[0] ? error :
                                   "response setup could not be persisted");
            result = 3;
            goto out;
        }
        json_decref(count_request);
        count_request = NULL;
        if (app_runtimef(app,
                "response › %s started · turn=%s · cycle=%u · model=%s · profile=%s",
                response_id, turn_id, cycle, app->turn_model,
                SNAJPAGENT_PROFILE_ID) < 0) {
            json_decref(steering);
            if (create_request)
                json_decref(create_request);
            snj_buf_free(&request_body);
            snj_response_graph_free(&graph);
            (void)app_error(app, "response runtime facts could not be rendered");
            result = 6;
            goto out;
        }
        if (request_body.len &&
            snj_render_protocol(&app->render, "request.body",
                                (const char *)request_body.data,
                                request_body.len) < 0) {
            json_t *partial = json_array();
            json_t *failed;
            static const char failure[] =
                "request diagnostics could not be rendered";
            snj_buf_free(&request_body);
            if (create_request)
                json_decref(create_request);
            json_decref(steering);
            failed = partial ?
                snj_app_response_failed_data(turn_id, response_id, cycle,
                                     "output", failure, partial, 0u) : NULL;
            partial = NULL;
            if (!failed ||
                commit_event(app, "response_failed", failed,
                             error, sizeof(error)) < 0 ||
                close_active_process_for_turn(app, turn_id, "output_failure",
                                              false, error, sizeof(error)) < 0 ||
                commit_event(app, "turn_failed",
                             snj_app_turn_failed_data(turn_id, "output", failure),
                             error, sizeof(error)) < 0) {
                snj_response_graph_free(&graph);
                (void)app_error(app, error[0] ? error :
                                "diagnostic output failure could not be persisted");
                result = 3;
                goto out;
            }
            snj_response_graph_free(&graph);
            (void)app_error(app, failure);
            result = 6;
            goto out;
        }
        snj_buf_free(&request_body);
        app->stream_graph = &graph;
        snj_app_reset_stream(app);
        response_begin_ms = snj_time_ms();
        app->active_since_ms = response_begin_ms;
        app->activity_shown = false;
        snj_term_clear_status(&app->term);
        error[0] = '\0';
        provider_rc = snj_app_provider_run(app, turn_prompt, steering, cycle,
                                   create_request, &credential, &graph,
                                   error, sizeof(error), &provider_retry_count);
        json_decref(create_request);
        json_decref(steering);
        if ((provider_rc == 1 && app->steering_requested) ||
            (provider_rc == 2 && app->interrupt_requested)) {
            if (snj_app_abort_stream_item(app) < 0)
                app->stream_failed = true;
        } else if (snj_app_finish_stream_item(app) < 0) {
            app->stream_failed = true;
        }
        if (provider_rc == 1 && app->steering_requested && !app->stream_failed) {
            json_t *partial = snj_app_partial_public_json(app);
            if (!partial ||
                commit_event(app, "response_interrupted",
                    snj_app_response_interrupted_data(turn_id, response_id, cycle,
                                              "steering", "steered", partial),
                    error, sizeof(error)) < 0) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                (void)app_error(app, error[0] ? error :
                                       "steered response could not be persisted");
                result = 3;
                goto out;
            }
            snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
            continue;
        }
        if (provider_rc == 2 && app->interrupt_requested && !app->stream_failed) {
            json_t *partial = snj_app_partial_public_json(app);
            if (!partial ||
                commit_event(app, "response_interrupted",
                    snj_app_response_interrupted_data(turn_id, response_id, cycle,
                                              "user", "cancelled", partial),
                    error, sizeof(error)) < 0 ||
                close_active_process_for_turn(app, turn_id, "user_interrupt",
                                              true, error, sizeof(error)) < 0 ||
                commit_event(app, "turn_interrupted",
                    snj_app_turn_interrupted_data(turn_id, "user", "cancelled"),
                    error, sizeof(error)) < 0) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                (void)app_error(app, error[0] ? error :
                                "interruption could not be persisted");
                result = 3;
                goto out;
            }
            snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
            (void)app_warning(app, "turn interrupted");
            result = app->execute ? 6 : 1;
            goto out;
        }
        if (provider_rc < 0 || app->stream_failed) {
            const char *class_name = app->stream_failed ? "output" : "provider";
            int exit_status = app->stream_failed ? 6 : 4;
            char failure[256];
            json_t *partial;
            (void)snprintf(failure, sizeof(failure), "%s",
                           app->stream_failed ?
                           "assistant output could not be delivered" :
                           (error[0] ? error : "provider response failed"));
            partial = snj_app_partial_public_json(app);
            if (!partial) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                (void)app_error(app, "failed response prefix could not be retained");
                result = 3;
                goto out;
            }
            snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
            if (commit_event(app, "response_failed",
                             snj_app_response_failed_data(turn_id, response_id, cycle,
                                                  class_name, failure, partial,
                                                  provider_retry_count),
                             error, sizeof(error)) < 0 ||
                close_active_process_for_turn(app, turn_id,
                                              app->stream_failed ?
                                              "output_failure" : "provider_failure",
                                              false, error, sizeof(error)) < 0 ||
                commit_event(app, "turn_failed",
                             snj_app_turn_failed_data(turn_id, class_name, failure),
                             error, sizeof(error)) < 0) {
                (void)app_error(app, error);
                result = 3;
                goto out;
            }
            (void)app_error(app, failure);
            result = exit_status;
            goto out;
        }
        if (!app->execute) {
            int input_rc = snj_app_active_input_pump(app, 0u);
            if (input_rc < 0) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                (void)app_error(app, "active input could not be processed");
                result = 3;
                goto out;
            }
        }
        if (snj_response_graph_classify(&graph, &decision,
                                        error, sizeof(error)) < 0) {
            char failure[256];
            json_t *partial;
            (void)snprintf(failure, sizeof(failure), "%s",
                           error[0] ? error : "invalid provider response graph");
            partial = snj_app_partial_public_json(app);
            if (!partial) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                (void)app_error(app, "invalid response prefix could not be retained");
                result = 3;
                goto out;
            }
            snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
            if (commit_event(app, "response_failed",
                             snj_app_response_failed_data(turn_id, response_id, cycle,
                                                  "protocol", failure, partial,
                                                  provider_retry_count),
                             error, sizeof(error)) < 0 ||
                close_active_process_for_turn(app, turn_id, "protocol_failure",
                                              false, error, sizeof(error)) < 0 ||
                commit_event(app, "turn_failed",
                             snj_app_turn_failed_data(turn_id, "protocol", failure),
                             error, sizeof(error)) < 0) {
                (void)app_error(app, error);
                result = 3;
                goto out;
            }
            (void)app_error(app, failure);
            result = 4;
            goto out;
        }
        if (commit_event(app, "response_completed",
                         snj_app_response_completed_data(turn_id, response_id, cycle, &graph),
                         error, sizeof(error)) < 0) {
            snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
            (void)app_error(app, error);
            result = 3;
            goto out;
        }
        {
            char input_tokens[32];
            char output_tokens[32];
            char reasoning_tokens[32];
            char total_tokens[32];
            usage_number(input_tokens, graph.usage.input_known,
                         graph.usage.input_tokens);
            usage_number(output_tokens, graph.usage.output_known,
                         graph.usage.output_tokens);
            usage_number(reasoning_tokens, graph.usage.reasoning_known,
                         graph.usage.reasoning_tokens);
            usage_number(total_tokens, graph.usage.total_known,
                         graph.usage.total_tokens);
            if (app_runtimef(app,
                    "response › %s completed · provider=%s · outcome=%s · items=%zu · duration=%llums · tokens=%s/%s/%s/%s",
                    response_id, graph.provider_response_id,
                    graph_outcome_name(decision.outcome), graph.count,
                    (unsigned long long)(snj_time_ms() - response_begin_ms),
                    input_tokens, output_tokens, reasoning_tokens,
                    total_tokens) < 0) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                (void)app_error(app, "response runtime facts could not be rendered");
                result = 6;
                goto out;
            }
        }
        if (!snj_app_managed_continuation_graph_matches(app, &graph, &decision)) {
            static const char message[] =
                "provider response violated unresolved managed process ordering";
            if ((app->session.pending_call_count != 0u &&
                 terminalize_pending(app, turn_id, "managed_process_conflict",
                                     error, sizeof(error)) < 0) ||
                close_active_process_for_turn(app, turn_id, "protocol_failure",
                                              false, error, sizeof(error)) < 0 ||
                commit_event(app, "turn_failed",
                             snj_app_turn_failed_data(turn_id, "protocol", message),
                             error, sizeof(error)) < 0) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                (void)app_error(app, error);
                result = 3;
                goto out;
            }
            snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
            (void)app_error(app, message);
            result = 4;
            goto out;
        }
        if (decision.outcome == SNJ_GRAPH_CONFLICT) {
            const char *message = decision.message ? decision.message :
                "provider response contained conflicting actions";
            if (terminalize_pending(app, turn_id, "protocol_conflict",
                                    error, sizeof(error)) < 0 ||
                close_active_process_for_turn(app, turn_id, "protocol_failure",
                                              false, error, sizeof(error)) < 0 ||
                commit_event(app, "turn_failed",
                             snj_app_turn_failed_data(turn_id, "protocol", message),
                             error, sizeof(error)) < 0) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                (void)app_error(app, error);
                result = 3;
                goto out;
            }
            snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
            (void)app_error(app, message);
            result = 4;
            goto out;
        }
        if (app->session.pending_steering_count != 0u) {
            if (decision.outcome == SNJ_GRAPH_CALLS &&
                terminalize_pending(app, turn_id, "superseded_by_steering",
                                    error, sizeof(error)) < 0) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                (void)app_error(app, error);
                result = 3;
                goto out;
            }
            snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
            continue;
        }
        if (decision.outcome == SNJ_GRAPH_FINAL ||
            decision.outcome == SNJ_GRAPH_REFUSAL) {
            const struct snj_response_item *final = &graph.items[decision.final_index];
            if (commit_event(app, "turn_completed",
                             snj_app_turn_completed_data(turn_id, response_id,
                                                 final->local_item_id),
                             error, sizeof(error)) < 0) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                (void)app_error(app, error);
                result = 3;
                goto out;
            }
            if (app_runtimef(app,
                    "turn › %s completed · response=%s · item=%s",
                    turn_id, response_id, final->local_item_id) < 0) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                (void)app_error(app, "turn runtime facts could not be rendered");
                result = 6;
                goto out;
            }
            if (app->execute &&
                snj_write_full(STDOUT_FILENO, final->text,
                               strlen(final->text)) < 0) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                (void)app_error(app, "final answer could not be written to stdout");
                result = 6;
                goto out;
            }
            if (snj_app_compact_after_turn(app, input_tokens_bound, count_method,
                                           error, sizeof(error)) < 0)
                (void)app_warning(app, error);
            snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
            result = 0;
            goto out;
        }
        if (decision.outcome == SNJ_GRAPH_CALLS) {
            int tool_rc;
            if (decision.call_count >
                SNJ_MAX_TOOL_INVOCATIONS - app->session.tool_invocations) {
                static const char message[] =
                    "turn exceeded 128 tool invocations";
                if (terminalize_pending(app, turn_id, "turn_cancelled",
                                        error, sizeof(error)) < 0 ||
                    close_active_process_for_turn(app, turn_id, "internal_failure",
                                                  false, error, sizeof(error)) < 0 ||
                    commit_event(app, "turn_failed",
                                 snj_app_turn_failed_data(turn_id, "resource", message),
                                 error, sizeof(error)) < 0) {
                    snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                    (void)app_error(app, error);
                    result = 3;
                    goto out;
                }
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                (void)app_error(app, message);
                result = 4;
                goto out;
            }
            tool_rc = execute_calls(app, turn_id, &graph, &credential,
                                    error, sizeof(error));
            snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
            if (tool_rc < 0) {
                (void)app_error(app, error);
                result = 3;
                goto out;
            }
            if (tool_rc > 0) {
                if (tool_rc == 2) {
                    (void)app_warning(app, "turn interrupted");
                    result = app->execute ? 6 : 1;
                } else {
                    (void)app_error(app, error);
                    result = 4;
                }
                goto out;
            }
            continue;
        }
        {
            const char *message = decision.message ? decision.message :
                "provider response was not actionable";
            if (close_active_process_for_turn(app, turn_id, "protocol_failure",
                                              false, error, sizeof(error)) < 0 ||
                commit_event(app, "turn_failed",
                             snj_app_turn_failed_data(turn_id, "protocol", message),
                             error, sizeof(error)) < 0) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                (void)app_error(app, error);
                result = 3;
                goto out;
            }
            snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
            (void)app_error(app, message);
            result = 4;
            goto out;
        }
    }
    {
        static const char message[] = "turn exceeded 64 response cycles";
        if (close_active_process_for_turn(app, turn_id, "internal_failure",
                                          false, error, sizeof(error)) < 0 ||
            commit_event(app, "turn_failed",
                         snj_app_turn_failed_data(turn_id, "resource", message),
                         error, sizeof(error)) < 0) {
            (void)app_error(app, error);
            result = 3;
            goto out;
        }
        (void)app_error(app, message);
        result = 4;
    }
out:
    app->stream_graph = NULL;
    snj_app_clear_partial_public(app);
    snj_term_clear_status(&app->term);
    if (!app->execute && result != 6 &&
        snj_term_set_prompt(&app->term, false) < 0)
        result = 6;
    snj_credential_clear(&credential);
    snj_instructions_free(&app->turn_instructions);
    free(turn_prompt);
    return result;
}
static char *
resolve_workspace_path(const char *path, const char *label,
                       char *error, size_t error_size)
{
    char *resolved = realpath(path, NULL);
    struct stat st;
    if (!resolved) {
        set_error(error, error_size, "cannot resolve %s workspace %s: %s",
                  label, path, strerror(errno));
        return NULL;
    }
    if (strlen(resolved) > SNJ_PATH_MAX_BYTES ||
        !snj_utf8_valid((const unsigned char *)resolved, strlen(resolved), true) ||
        stat(resolved, &st) < 0 || !S_ISDIR(st.st_mode)) {
        set_error(error, error_size, "%s workspace must be an existing UTF-8 directory",
                  label);
        free(resolved);
        errno = EINVAL;
        return NULL;
    }
    return resolved;
}
static char *
current_workspace(char *error, size_t error_size)
{
    return resolve_workspace_path(".", "current", error, error_size);
}
static int
pick_session(struct app_state *app, const char *workspace,
             char *error, size_t error_size)
{
    char prefix[128];
    size_t len;
    if (snj_store_list_active(&app->store, workspace, app->cli->all, STDERR_FILENO,
                       error, error_size) < 0)
        return -1;
    if (snj_render_prompt(&app->render, "session › ") < 0 ||
        !fgets(prefix, sizeof(prefix), stdin)) {
        set_error(error, error_size, "session selection cancelled");
        return -1;
    }
    len = strlen(prefix);
    if (len && prefix[len - 1u] == '\n')
        prefix[--len] = '\0';
    if (len < 8u || len > SNJ_ID_HEX_LEN) {
        set_error(error, error_size, "enter an 8..32 character session id prefix");
        return -1;
    }
    return snj_session_open(&app->store, &app->session, prefix,
                            error, error_size);
}
static int
run_queued_chain(struct app_state *app)
{
    while (app->queue_armed && app->session.pending_queue_count != 0u) {
        const struct snj_queued_turn *queued = &app->session.pending_queue[0];
        int turn_rc = run_turn(app, queued->text, queued);
        if (turn_rc != 0) {
            app->queue_armed = false;
            return turn_rc;
        }
    }
    if (app->session.pending_queue_count == 0u)
        app->queue_armed = false;
    return 0;
}
static int
interactive_loop(struct app_state *app, const char *initial)
{
    char *owned = NULL;
    const char *prompt = initial;
    if (snj_term_set_prompt(&app->term, false) < 0)
        return 6;
    for (;;) {
        enum snj_term_action action = SNJ_TERM_NONE;
        if (app->input_closed)
            return 0;
        if (!prompt) {
            int poll_rc = snj_term_poll(&app->term, -1, &action, &owned);
            if (poll_rc < 0) {
                (void)app_error(app,
                    errno == EOVERFLOW ? "prompt exceeds 1 MiB" :
                    errno == EILSEQ ? "terminal input contains invalid UTF-8" :
                    "terminal input could not be read");
                if (snj_term_set_prompt(&app->term, false) < 0)
                    return 6;
                continue;
            }
            if (poll_rc == 0)
                continue;
            if (action == SNJ_TERM_EXIT) {
                free(owned);
                return 0;
            }
            if (action != SNJ_TERM_SUBMIT || !owned) {
                free(owned);
                owned = NULL;
                if (snj_term_set_prompt(&app->term, false) < 0)
                    return 6;
                continue;
            }
            prompt = owned;
        }
        if (snj_render_submitted(&app->render, "› ", prompt) < 0) {
            free(owned);
            return 6;
        }
        if (snj_term_history_add(&app->term, prompt) < 0)
            (void)app_warning(app, "submission was accepted but history retention failed");
        {
            bool single_line = strchr(prompt, '\n') == NULL;
            bool handled = false;
            int local_rc = 0;
            if (single_line && prompt[0] == '/' && prompt[1] != '/')
                local_rc = handle_common_command(app, prompt, false, &handled);
            if (local_rc < 0) { free(owned); return 3; }
            if (!handled && single_line && prompt[0] == '/' && prompt[1] != '/') {
                bool exit_now = false;
                local_rc = snj_app_lifecycle_command(app, prompt, &handled, &exit_now);
                if (local_rc < 0 || exit_now) { free(owned); return local_rc < 0 ? 3 : 0; }
            }
            if (handled) {
                /* local view or preference command completed */
            } else if (single_line && strcmp(prompt, "/exit") == 0) {
                free(owned);
                return 0;
            } else if (single_line && strcmp(prompt, "/queue") == 0) {
                (void)render_queue(app);
            } else if (single_line && strncmp(prompt, "/queue ", 7u) == 0) {
                (void)app_error(app,
                    "/queue TEXT is active-only; submit it during a running turn");
            } else if (single_line && strncmp(prompt, "/unqueue ", 9u) == 0) {
                char error[256];
                int cancel_rc;
                error[0] = '\0';
                cancel_rc = cancel_future_turns(app, prompt + 9u,
                                                error, sizeof(error));
                if (cancel_rc != 0)
                    (void)app_error(app, error);
                if (cancel_rc < 0) {
                    free(owned);
                    return 3;
                }
            } else if (single_line && strcmp(prompt, "/next") == 0) {
                int chain_rc;
                if (app->session.pending_queue_count == 0u) {
                    (void)app_error(app, "future-turn queue is empty");
                } else {
                    app->queue_armed = true;
                    chain_rc = run_queued_chain(app);
                    if (chain_rc == 3 || chain_rc == 6) {
                        free(owned);
                        return chain_rc;
                    }
                }
            } else if (single_line && prompt[0] == '/' && prompt[1] != '/') {
                (void)app_error(app, "unknown slash command");
            } else {
                const char *actual = prompt[0] == '/' && prompt[1] == '/' ?
                                     prompt + 1 : prompt;
                int turn_rc;
                app->queue_armed = false;
                turn_rc = run_turn(app, actual, NULL);
                if (turn_rc == 3 || turn_rc == 6) {
                    free(owned);
                    return turn_rc;
                }
                if (turn_rc == 0 && app->queue_armed) {
                    int chain_rc = run_queued_chain(app);
                    if (chain_rc == 3 || chain_rc == 6) {
                        free(owned);
                        return chain_rc;
                    }
                } else if (turn_rc != 0) {
                    app->queue_armed = false;
                }
            }
        }
        free(owned);
        owned = NULL;
        prompt = NULL;
        if (snj_term_set_prompt(&app->term, false) < 0)
            return 6;
    }
}
int
snj_app_run(const struct snj_cli *cli)
{
    struct app_state app;
    struct snj_config config;
    char error[256];
    char *workspace = NULL;
    char *relocated_workspace = NULL;
    const char *new_model = NULL;
    const char *new_effort;
    unsigned int effective_verbosity;
    int rc = 3;
    memset(&app, 0, sizeof(app));
    snj_config_init(&config);
    snj_instructions_init(&app.turn_instructions);
    snj_store_init(&app.store);
    snj_session_init(&app.session);
    snj_term_init(&app.term);
    snj_render_init(&app.render, 0u);
    app.cli = cli;
    app.config = &config;
    app.execute = cli->execute;
    {
        const char *locale_name = setlocale(LC_CTYPE, "");
        const char *codeset = locale_name ? nl_langinfo(CODESET) : NULL;
        if (!locale_name || !codeset ||
            (strcasecmp(codeset, "UTF-8") != 0 &&
             strcasecmp(codeset, "UTF8") != 0)) {
            (void)snj_render_error("a UTF-8 locale is required");
            rc = 2;
            goto out;
        }
    }
    error[0] = '\0';
    if (snj_config_load(&config, cli->config_path,
                        error, sizeof(error)) < 0) {
        (void)snj_render_error(error);
        rc = 2;
        goto out;
    }
    effective_verbosity = config.verbosity;
    if (effective_verbosity < 6u) {
        unsigned int room = 6u - effective_verbosity;
        effective_verbosity += cli->verbosity < room ? cli->verbosity : room;
    }
    app.render.verbosity = effective_verbosity;
    if (!cli->execute && !cli->list &&
        (isatty(STDIN_FILENO) != 1 || isatty(STDERR_FILENO) != 1)) {
        (void)snj_render_error("interactive mode requires terminal stdin and stderr; use -e for scripts");
        rc = 2;
        goto out;
    }
    if (!cli->resume || cli->model) {
        new_model = resolve_model(&config, cli->model ? cli->model : config.model);
        if (!new_model) {
            (void)snj_render_error("model selector is not in the compiled release profile");
            rc = 2;
            goto out;
        }
    }
    new_effort = cli->effort ? cli->effort : config.reasoning_effort;
    if ((!cli->resume || cli->effort) && !resolve_effort(new_effort)) {
        (void)snj_render_error("reasoning effort is unsupported by the selected model");
        rc = 2;
        goto out;
    }
    if (snj_store_open(&app.store, error, sizeof(error)) < 0) {
        (void)snj_render_error(error);
        goto out;
    }
    workspace = current_workspace(error, sizeof(error));
    if (!workspace) {
        (void)snj_render_error(error);
        goto out;
    }
    if (cli->list) {
        rc = snj_store_list(&app.store, workspace, cli->all, STDOUT_FILENO,
                            error, sizeof(error)) < 0 ? 3 : 0;
        if (rc)
            (void)snj_render_error(error);
        goto out;
    }
    if (cli->resume) {
        if (cli->workspace) {
            relocated_workspace = resolve_workspace_path(cli->workspace, "relocation",
                                                         error, sizeof(error));
            if (!relocated_workspace) {
                (void)snj_render_error(error);
                rc = 2;
                goto out;
            }
        }
        if (cli->resume_id)
            rc = snj_session_open(&app.store, &app.session, cli->resume_id,
                                  error, sizeof(error));
        else if (cli->last)
            rc = snj_session_open_last(&app.store, &app.session, workspace,
                                       cli->all, error, sizeof(error));
        else
            rc = pick_session(&app, workspace, error, sizeof(error));
        if (rc == 1) {
            (void)snj_render_warning(error);
            rc = 0;
            goto out;
        }
        if (rc < 0) {
            (void)snj_render_error(error);
            rc = 3;
            goto out;
        }
        if (app.session.archived && snj_session_unarchive(&app.session, NULL, error, sizeof(error)) < 0) {
            (void)snj_render_error(error); rc = 3; goto out;
        }
        if (recover_session(&app, error, sizeof(error)) < 0) {
            (void)snj_render_error(error);
            rc = 3;
            goto out;
        }
        if (relocated_workspace &&
            strcmp(relocated_workspace, app.session.workspace) != 0 &&
            commit_event(&app, "workspace_changed",
                         snj_app_preference_changed_data("old_workspace",
                                                app.session.workspace,
                                                "new_workspace",
                                                relocated_workspace),
                         error, sizeof(error)) < 0) {
            (void)snj_render_error(error);
            rc = 3;
            goto out;
        }
        app.staged_model = cli->model ? new_model : NULL;
        app.staged_effort = cli->effort;
        app.turn_model = next_model(&app);
        app.turn_effort = resolve_effort(next_effort(&app));
    } else {
        const char *selected_workspace = cli->workspace ? cli->workspace : workspace;
        if (snj_session_create(&app.store, &app.session, selected_workspace,
                               new_model, new_effort, error, sizeof(error)) < 0) {
            (void)snj_render_error(error);
            goto out;
        }
        app.turn_model = app.session.default_model;
        app.turn_effort = resolve_effort(app.session.default_effort);
    }
    if (cli->execute) {
        rc = run_turn(&app, cli->prompt, NULL);
        goto out;
    }
    if (snj_term_open(&app.term, error, sizeof(error)) < 0) {
        (void)snj_render_error(error);
        rc = 3;
        goto out;
    }
    snj_render_attach_term(&app.render, &app.term);
    if (app.session.last_user)
        (void)snj_term_history_add(&app.term, app.session.last_user);
    if (snj_render_orientation(&app.render, &app.session, cli->resume) < 0 ||
        (cli->resume && config.resume_history_turns != 0u &&
         snj_render_history(&app.render, &app.session) < 0) ||
        (cli->resume && app.session.pending_queue_count != 0u &&
         app_warning(&app,
             "queued future turns are paused; use /next to continue FIFO") < 0)) {
        rc = 6;
        goto out;
    }
    rc = interactive_loop(&app, cli->prompt);
out:
    snj_term_close(&app.term);
    free(relocated_workspace);
    free(workspace);
    snj_instructions_free(&app.turn_instructions);
    snj_session_close(&app.session);
    snj_store_close(&app.store);
    snj_config_free(&config);
    return rc;
}
