/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"
#include "tools.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
render_event_seq(struct app_state *app, uint64_t seq, const char *type)
{
    return snag_ui_send(&app->ui, (struct snag_ui_command){
        .kind = SNAG_UI_EVENT, .text = type, .data.seq = seq}) < 0 ? -1 : 0;
}

static bool
span_equals(const char *text, size_t len, const char *word)
{
    return strlen(word) == len && memcmp(text, word, len) == 0;
}

int
snag_app_parse_queue_argument(const char *argument,
                             enum queue_command_kind *kind, size_t *number)
{
    const char *start = argument;
    const char *end = argument + strlen(argument);
    const char *p;
    size_t value = 0u;

    while (start < end && (*start == ' ' || *start == '\t'))
        ++start;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
        --end;
    if (start == end) {
        *kind = QUEUE_COMMAND_LIST;
        return 0;
    }
    if (span_equals(start, (size_t)(end - start), "c") ||
        span_equals(start, (size_t)(end - start), "clear")) {
        *kind = QUEUE_COMMAND_CLEAR;
        return 0;
    }
    if (span_equals(start, (size_t)(end - start), "p") ||
        span_equals(start, (size_t)(end - start), "pop")) {
        *kind = QUEUE_COMMAND_POP;
        return 0;
    }
    p = start;
    while (p < end && *p >= '0' && *p <= '9') {
        size_t digit = (size_t)(*p - '0');

        if (value > (SIZE_MAX - digit) / 10u)
            return -1;
        value = value * 10u + digit;
        ++p;
    }
    if (p == start || p == end) {
        *kind = QUEUE_COMMAND_ADD;
        return 0;
    }
    if ((*p == 'd' || *p == 'e') && p + 1u == end) {
        *kind = *p == 'd' ? QUEUE_COMMAND_DELETE : QUEUE_COMMAND_EDIT;
        *number = value;
        return 0;
    }
    if (*p != ' ' && *p != '\t')
        return -1;
    while (p < end && (*p == ' ' || *p == '\t'))
        ++p;
    if (span_equals(p, (size_t)(end - p), "d") ||
        span_equals(p, (size_t)(end - p), "delete")) {
        *kind = QUEUE_COMMAND_DELETE;
        *number = value;
        return 0;
    }
    if (span_equals(p, (size_t)(end - p), "e") ||
        span_equals(p, (size_t)(end - p), "edit")) {
        *kind = QUEUE_COMMAND_EDIT;
        *number = value;
        return 0;
    }
    return -1;
}

static int
confirm_delete(struct app_state *app, char prefix[9], char *error,
               size_t error_size)
{
    enum snag_term_action action = SNAG_TERM_NONE;
    char *line = NULL;
    int rc;

    memcpy(prefix, app->session.id, 8u);
    prefix[8] = '\0';
    if (snag_ui_text(&app->ui, SNAG_UI_HOST,
            "delete is irreversible; type the displayed 8-character id prefix to confirm") < 0 ||
        snag_ui_simple_prompt(&app->ui, false) < 0) {
        return snag_errorf(error, error_size, "delete confirmation prompt could not be displayed");
    }
    do {
        if (snag_tools_service(0, snag_ui_wake_fd(&app->ui), error, error_size) < 0)
            return -1;
        rc = snag_ui_poll(&app->ui, 25, false, &action, &line);
    } while (rc == 0);
    if (rc < 0) {
        free(line);
        return snag_errorf(error, error_size, "delete confirmation input could not be read");
    }
    if (action == SNAG_TERM_CANCEL || action == SNAG_TERM_INTERRUPT) {
        free(line);
        if (error_size)
            error[0] = '\0';
        return 1;
    }
    if (action == SNAG_TERM_EXIT || !line) {
        if (action == SNAG_TERM_EXIT)
            app->input_closed = true;
        free(line);
        snprintf(error, error_size, "delete cancelled");
        return 1;
    }
    if (strcmp(line, prefix) != 0) {
        free(line);
        (void)snag_fail(error, error_size, EINVAL, "delete confirmation did not match %.8s",
                 app->session.id);
        return 1;
    }
    free(line);
    return 0;
}

int
snag_app_lifecycle_command(struct app_state *app, const char *line,
                          bool *handled, bool *exit_now)
{
    char error[256];
    uint64_t seq;

    *handled = true;
    *exit_now = false;
    if (strcmp(line, "/archive") == 0) {
        if (app->session.process_count && snag_app_close_active_processes(app,
                app->session.active_turn_id, "user_interrupt", true, error, sizeof(error)) < 0)
            return -1;
        seq = app->session.next_seq;
        if (snag_session_archive(&app->session, &seq, error, sizeof(error)) < 0) {
            (void)snag_ui_text(&app->ui, SNAG_UI_ERROR, error);
            return -1;
        }
        if (render_event_seq(app, seq, "session_archived") < 0 ||
            snag_ui_text(&app->ui, SNAG_UI_HOST, "session archived") < 0)
            return -1;
        *exit_now = true;
        return 0;
    }
    if (strcmp(line, "/delete") == 0) {
        if (app->session.pending_log) {
            *exit_now = true;
            return 0;
        }
        char prefix[9];
        int confirm_rc = confirm_delete(app, prefix, error, sizeof(error));
        if (confirm_rc != 0) {
            if (error[0])
                (void)snag_ui_text(&app->ui, SNAG_UI_ERROR, error);
            return confirm_rc < 0 ? -1 : 0;
        }
        /* Confirmation precedes stopping any owned processes. */
        if (app->session.process_count && snag_app_close_active_processes(app,
                app->session.active_turn_id, "user_interrupt", true, error, sizeof(error)) < 0)
            return -1;
        seq = app->session.next_seq;
        if (snag_session_delete(&app->store, &app->session, prefix, &seq,
                               error, sizeof(error)) < 0) {
            (void)snag_ui_text(&app->ui, SNAG_UI_ERROR, error);
            return -1;
        }
        if (render_event_seq(app, seq, "session_delete_requested") < 0 ||
            snag_ui_text(&app->ui, SNAG_UI_HOST, "session deleted") < 0)
            return -1;
        *exit_now = true;
        return 0;
    }
    *handled = false;
    return 0;
}

static const char goal_help[] =
    "/goal                         show current goal\n"
    "/goal status                  show current goal\n"
    "/goal TEXT                    start or reword a goal\n"
    "/goal \"TEXT\"                  quote a reserved first word\n"
    "/goal set TEXT                explicitly start or reword\n"
    "/goal pause|resume            control continuation\n"
    "/goal lock|unlock             control model rewording\n"
    "/goal complete|cancel|clear   end the goal\n"
    "reserved first words: status help set pause resume lock unlock complete cancel clear";

static int
goal_error(struct app_state *app, const char *message)
{
    return snag_ui_text(&app->ui, SNAG_UI_ERROR, message);
}

static json_t *
goal_id_data(const struct snag_session *session)
{
    return json_pack("{s:s}", "goal_id", session->goal_id);
}

static json_t *
goal_actor_data(const struct snag_session *session, const char *actor)
{
    return json_pack("{s:s,s:s}", "goal_id", session->goal_id, "actor", actor);
}

static json_t *
goal_text_data(const struct snag_session *session, const char *actor,
               const char *prompt)
{
    return json_pack("{s:s,s:s,s:s}", "goal_id", session->goal_id,
                     "actor", actor, "prompt", prompt);
}

static int
commit_goal_event(struct app_state *app, const char *type, json_t *data,
                  char *error, size_t error_size)
{
    if (!data) {
        return snag_errorf(error, error_size, "cannot allocate %s event", type);
    }
    return snag_app_commit_event(app, type, data, error, error_size);
}

static int
render_goal(struct app_state *app)
{
    int rc;

    if (app->session.goal_status == SNAG_GOAL_NONE)
        return snag_ui_text(&app->ui, SNAG_UI_WARNING, "no goal has been set");
    struct snag_buf text = {.max = SNAG_MAX_GOAL_PROMPT + SNAG_MAX_GOAL_BLOCKER + 512u};
    rc = snag_buf_printf(&text,
        "goal %.8s: %s%s\n"
        "turns: %llu · revision: %llu · prompt: %zu/%u bytes\n"
        "%s",
        app->session.goal_id, snag_goal_status_name(app->session.goal_status),
        app->session.goal_locked ? " · wording locked" : " · wording unlocked",
        (unsigned long long)app->session.goal_turn_count,
        (unsigned long long)app->session.goal_revision,
        app->session.goal_prompt ? strlen(app->session.goal_prompt) : 0u,
        app->config->max_goal_prompt_bytes,
        app->session.goal_prompt ? app->session.goal_prompt : "");
    if (rc == 0 && app->session.goal_blocker)
        rc = snag_buf_printf(&text, "\nblocker: %s", app->session.goal_blocker);
    if (rc == 0 && snag_buf_terminate(&text) < 0)
        rc = -1;
    if (rc == 0)
        rc = snag_ui_text(&app->ui, SNAG_UI_HOST, (const char *)text.data);
    snag_buf_free(&text);
    return rc;
}

static char *
copy_goal_argument(const char *argument, uint32_t limit,
                   char *error, size_t error_size)
{
    const char *start = argument;
    const char *end;
    char *copy;
    size_t len;

    while (*start == ' ' || *start == '\t' || *start == '\r')
        ++start;
    end = start + strlen(start);
    while (end > start &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        --end;
    if (start < end && *start == '"') {
        if (end - start < 2 || end[-1] != '"') {
            (void)snag_fail(error, error_size, EINVAL,
                            "quoted goal wording requires a closing double quote");
            return NULL;
        }
        ++start;
        --end;
    }
    len = (size_t)(end - start);
    if (!len || len > limit || len > SNAG_MAX_GOAL_PROMPT) {
        (void)snag_fail(error, error_size, EINVAL,
                        "goal wording must contain 1..%u UTF-8 bytes", limit);
        return NULL;
    }
    copy = malloc(len + 1u);
    if (!copy)
        return NULL;
    memcpy(copy, start, len);
    copy[len] = '\0';
    if (snag_text_blank(copy) ||
        !snag_utf8_valid((const unsigned char *)copy, len, true)) {
        free(copy);
        (void)snag_fail(error, error_size, EINVAL,
                        "goal wording must be nonblank valid UTF-8");
        return NULL;
    }
    return copy;
}

static const char *
reserved_word(const char *word, size_t len)
{
    static const char *const words[] = {
        "status", "help", "set", "pause", "resume", "lock", "unlock",
        "complete", "cancel", "clear"
    };
    for (size_t i = 0u; i < sizeof(words) / sizeof(words[0]); ++i)
        if (span_equals(word, len, words[i]))
            return words[i];
    return NULL;
}

static int
start_goal(struct app_state *app, const char *prompt,
           char *error, size_t error_size)
{
    char goal_id[SNAG_ID_HEX_LEN + 1u];

    if (snag_goal_unfinished(app->session.goal_status))
        return snag_fail(error, error_size, EINVAL, "an unfinished goal already exists");
    if (snag_random_id(goal_id) < 0) {
        return snag_errorf(error, error_size,
                       "cryptographic goal id generation failed");
    }
    if (commit_goal_event(app, "goal_started",
                          json_pack("{s:s,s:s}", "goal_id", goal_id, "prompt", prompt),
                          error, error_size) < 0)
        return -1;
    app->goal_armed = true;
    return 0;
}

static int
set_goal_prompt(struct app_state *app, const char *argument)
{
    char error[256] = {0};
    char *prompt = copy_goal_argument(argument,
                                      app->config->max_goal_prompt_bytes,
                                      error, sizeof(error));
    int rc;

    if (!prompt)
        return goal_error(app, error[0] ? error : "goal wording is unavailable");
    if (snag_goal_unfinished(app->session.goal_status)) {
        if (strcmp(prompt, app->session.goal_prompt) == 0) {
            free(prompt);
            return snag_ui_text(&app->ui, SNAG_UI_WARNING,
                                           "goal wording is unchanged");
        }
        rc = commit_goal_event(app, "goal_reworded",
                               goal_text_data(&app->session, "user", prompt),
                               error, sizeof(error));
    } else {
        rc = start_goal(app, prompt, error, sizeof(error));
    }
    free(prompt);
    if (rc < 0)
        return goal_error(app, error);
    return 0;
}

int
snag_app_goal_pause(struct app_state *app, const char *reason,
                   char *error, size_t error_size)
{
    json_t *data;

    if (app->session.goal_status != SNAG_GOAL_ACTIVE) {
        app->goal_armed = false;
        return 0;
    }
    data = json_pack("{s:s,s:s}", "goal_id", app->session.goal_id, "reason", reason);
    if (!data) {
        return snag_errorf(error, error_size, "cannot allocate goal pause event");
    }
    if (commit_goal_event(app, "goal_paused", data,
                          error, error_size) < 0)
        return -1;
    app->goal_armed = false;
    return 0;
}

static int
goal_simple_command(struct app_state *app, const char *command)
{
    char error[256] = {0};
    json_t *data;
    const char *type;

    if (strcmp(command, "pause") == 0) {
        if (app->session.goal_status != SNAG_GOAL_ACTIVE)
            return goal_error(app, "only an active goal can be paused");
        if (snag_app_goal_pause(app, "user", error, sizeof(error)) < 0)
            return goal_error(app, error);
        return 0;
    }
    if (strcmp(command, "resume") == 0) {
        if (app->session.goal_status != SNAG_GOAL_PAUSED &&
            app->session.goal_status != SNAG_GOAL_BLOCKED)
            return goal_error(app, "only a paused or blocked goal can be resumed");
        if (commit_goal_event(app, "goal_resumed", goal_id_data(&app->session),
                              error, sizeof(error)) < 0)
            return goal_error(app, error);
        app->goal_armed = true;
        return 0;
    }
    if (snag_string_in(command, "lock unlock")) {
        bool locked = strcmp(command, "lock") == 0;
        if (!snag_goal_unfinished(app->session.goal_status))
            return goal_error(app, "no unfinished goal can be locked or unlocked");
        if (app->session.goal_locked == locked)
            return snag_ui_text(&app->ui, SNAG_UI_WARNING,
                locked ? "goal wording is already locked" :
                         "goal wording is already unlocked");
        data = json_pack("{s:s,s:b}", "goal_id", app->session.goal_id, "locked", locked);
        if (!data) {
            return goal_error(app, "cannot allocate goal lock event");
        }
        if (commit_goal_event(app, "goal_lock_changed", data,
                              error, sizeof(error)) < 0)
            return goal_error(app, error);
        return 0;
    }
    if (strcmp(command, "complete") == 0) {
        if (!snag_goal_unfinished(app->session.goal_status))
            return goal_error(app, "no unfinished goal can be completed");
        type = "goal_completed";
        data = goal_actor_data(&app->session, "user");
    } else {
        if (!snag_goal_unfinished(app->session.goal_status))
            return goal_error(app, "no unfinished goal can be cancelled");
        type = "goal_cancelled";
        data = goal_id_data(&app->session);
    }
    if (commit_goal_event(app, type, data, error, sizeof(error)) < 0)
        return goal_error(app, error);
    app->goal_armed = false;
    return 0;
}

int
snag_app_goal_command(struct app_state *app, const char *line, bool active)
{
    const char *argument = line + 5u;
    const char *word_end, *rest, *command;
    size_t word_len;

    (void)active;
    while (isspace((unsigned char)*argument))
        ++argument;
    if (!*argument)
        return render_goal(app);
    if (*argument == '"')
        return set_goal_prompt(app, argument);
    word_end = argument;
    while (*word_end && !isspace((unsigned char)*word_end))
        ++word_end;
    word_len = (size_t)(word_end - argument);
    if (span_equals(argument, word_len, "set")) {
        if (!*word_end)
            return goal_error(app, "/goal set requires wording");
        return set_goal_prompt(app, word_end);
    }
    command = reserved_word(argument, word_len);
    if (command) {
        rest = word_end;
        while (isspace((unsigned char)*rest))
            ++rest;
        if (*rest)
            return goal_error(app,
                "reserved /goal command has extra text; use /goal set or quotes");
        if (strcmp(command, "status") == 0)
            return render_goal(app);
        if (strcmp(command, "help") == 0)
            return snag_ui_text(&app->ui, SNAG_UI_HOST, goal_help);
        return goal_simple_command(app, command);
    }
    return set_goal_prompt(app, argument);
}

static int
tool_result(bool succeeded, const char *message, json_t **result)
{
    *result = snag_tool_result_terminal(succeeded, message);
    return *result ? 0 : -1;
}

static bool
goal_text_valid(const char *text, size_t limit)
{
    return text && *text && !snag_text_blank(text) && strlen(text) <= limit &&
           snag_utf8_valid((const unsigned char *)text, strlen(text), true);
}

int
snag_app_goal_tool(struct app_state *app,
                  const struct snag_response_item *call,
                  json_t **result, char *error, size_t error_size)
{
    const char *action;
    const char *text;
    json_t *text_value;
    json_t *data;
    size_t prompt_limit = app->config->max_goal_prompt_bytes;

    if (prompt_limit > SNAG_MAX_GOAL_PROMPT)
        prompt_limit = SNAG_MAX_GOAL_PROMPT;

    *result = NULL;
    if (call && call->name && strcmp(call->name, "create_goal") == 0) {
        const char *objective;
        char message[128];

        if (!snag_json_exact_keys(call->arguments, "objective") ||
            !(objective = snag_json_string(call->arguments, "objective")))
            return tool_result(false, "create_goal arguments are invalid", result);
        if (snag_goal_unfinished(app->session.goal_status))
            return tool_result(false, "an unfinished goal already exists", result);
        if (!goal_text_valid(objective, prompt_limit))
            return tool_result(false,
                "goal objective is blank, invalid, or exceeds the configured limit",
                result);
        if (start_goal(app, objective, error, error_size) < 0)
            return -1;
        (void)snprintf(message, sizeof(message),
                       "goal %.8s started; automatic continuation is active",
                       app->session.goal_id);
        return tool_result(true, message, result);
    }
    if (!call || strcmp(call->name, "update_goal") != 0 ||
        !snag_json_exact_keys(call->arguments, "action text") ||
        !(action = snag_json_string(call->arguments, "action")))
        return tool_result(false, "update_goal arguments are invalid", result);
    text_value = json_object_get(call->arguments, "text");
    if (app->session.goal_status != SNAG_GOAL_ACTIVE)
        return tool_result(false, "there is no active goal to update", result);
    if (strcmp(action, "rewrite") == 0) {
        text = snag_json_string(call->arguments, "text");
        if (!goal_text_valid(text, prompt_limit))
            return tool_result(false,
                "new goal wording is blank, invalid, or exceeds the configured limit",
                result);
        if (app->session.goal_locked)
            return tool_result(false,
                               "goal wording is locked by the user", result);
        if (strcmp(text, app->session.goal_prompt) == 0)
            return tool_result(true, "goal wording is unchanged", result);
        if (commit_goal_event(app, "goal_reworded",
                              goal_text_data(&app->session, "model", text),
                              error, error_size) < 0)
            return -1;
        return tool_result(true, "goal wording updated", result);
    }
    if (strcmp(action, "complete") == 0) {
        if (app->session.process_count)
            return tool_result(false, "settle command handles before completing the goal", result);
        if (!json_is_null(text_value))
            return tool_result(false,
                               "complete requires text to be null", result);
        if (commit_goal_event(app, "goal_completed",
                              goal_actor_data(&app->session, "model"),
                              error, error_size) < 0)
            return -1;
        app->goal_armed = false;
        return tool_result(true, "goal marked complete", result);
    }
    if (strcmp(action, "block") == 0) {
        text = snag_json_string(call->arguments, "text");
        if (!goal_text_valid(text, SNAG_MAX_GOAL_BLOCKER))
            return tool_result(false,
                               "block requires a bounded nonblank reason", result);
        data = json_pack("{s:s,s:s,s:s}", "goal_id", app->session.goal_id,
                         "actor", "model", "reason", text);
        if (!data) {
            return snag_errorf(error, error_size, "cannot allocate goal block event");
        }
        if (commit_goal_event(app, "goal_blocked", data,
                              error, error_size) < 0)
            return -1;
        app->goal_armed = false;
        return tool_result(true, "goal marked blocked", result);
    }
    return tool_result(false, "update_goal action is invalid", result);
}
