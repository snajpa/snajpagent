/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
render_event_seq(struct app_state *app, uint64_t seq, const char *type)
{
    return snag_ui_event(&app->ui, seq, type) < 0 ? -1 : 0;
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
        snprintf(error, error_size, "delete confirmation prompt could not be displayed");
        return -1;
    }
    do {
        rc = snag_ui_poll(&app->ui, -1, false, &action, &line);
    } while (rc == 0);
    if (rc < 0) {
        free(line);
        snprintf(error, error_size, "delete confirmation input could not be read");
        return -1;
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
        snprintf(error, error_size, "delete confirmation did not match %.8s",
                 app->session.id);
        errno = EINVAL;
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
    if (strcmp(line, "/compact") == 0) {
        if (snag_app_compact_idle_command(app, "manual",
                                         error, sizeof(error)) < 0) {
            (void)snag_ui_text(&app->ui, SNAG_UI_ERROR, error);
            return -1;
        }
        return 0;
    }
    if (strcmp(line, "/delete") == 0) {
        char prefix[9];
        int confirm_rc = confirm_delete(app, prefix, error, sizeof(error));
        if (confirm_rc != 0) {
            if (error[0])
                (void)snag_ui_text(&app->ui, SNAG_UI_ERROR, error);
            return confirm_rc < 0 ? -1 : 0;
        }
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
    "/goal complete|cancel         end the goal\n"
    "reserved first words: status help set pause resume lock unlock complete cancel";

static int
goal_error(struct app_state *app, const char *message)
{
    return snag_ui_text(&app->ui, SNAG_UI_ERROR, message);
}

static json_t *
goal_id_data(const struct snag_session *session)
{
    json_t *data = json_object();
    if (!data ||
        snag_json_set_new(data, "goal_id", json_string(session->goal_id)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
}

static json_t *
goal_actor_data(const struct snag_session *session, const char *actor)
{
    json_t *data = goal_id_data(session);
    if (!data || snag_json_set_new(data, "actor", json_string(actor)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
}

static json_t *
goal_text_data(const struct snag_session *session, const char *actor,
               const char *prompt)
{
    json_t *data = goal_actor_data(session, actor);
    if (!data || snag_json_set_new(data, "prompt", json_string(prompt)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
}

static json_t *
goal_started_data(const char *goal_id, const char *prompt)
{
    json_t *data = json_object();
    if (!data || snag_json_set_new(data, "goal_id", json_string(goal_id)) < 0 ||
        snag_json_set_new(data, "prompt", json_string(prompt)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
}

static int
commit_goal_event(struct app_state *app, const char *type, json_t *data,
                  char *error, size_t error_size)
{
    if (!data) {
        (void)snprintf(error, error_size, "cannot allocate %s event", type);
        return -1;
    }
    return snag_app_commit_event(app, type, data, error, error_size);
}

static int
render_goal(struct app_state *app)
{
    struct snag_buf text;
    int rc;

    if (app->session.goal_status == SNAG_GOAL_NONE)
        return snag_ui_text(&app->ui, SNAG_UI_WARNING, "no goal has been set");
    snag_buf_init(&text, SNAG_MAX_GOAL_PROMPT + SNAG_MAX_GOAL_BLOCKER + 512u);
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
            (void)snprintf(error, error_size,
                           "quoted goal wording requires a closing double quote");
            errno = EINVAL;
            return NULL;
        }
        ++start;
        --end;
    }
    len = (size_t)(end - start);
    if (!len || len > limit || len > SNAG_MAX_GOAL_PROMPT) {
        (void)snprintf(error, error_size,
                       "goal wording must contain 1..%u UTF-8 bytes", limit);
        errno = EINVAL;
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
        (void)snprintf(error, error_size,
                       "goal wording must be nonblank valid UTF-8");
        errno = EINVAL;
        return NULL;
    }
    return copy;
}

static bool
reserved_word(const char *word, size_t len)
{
    static const char *const words[] = {
        "status", "help", "set", "pause", "resume", "lock", "unlock",
        "complete", "cancel"
    };
    for (size_t i = 0u; i < sizeof(words) / sizeof(words[0]); ++i)
        if (strlen(words[i]) == len && memcmp(word, words[i], len) == 0)
            return true;
    return false;
}

static int
start_goal(struct app_state *app, const char *prompt,
           char *error, size_t error_size)
{
    char goal_id[SNAG_ID_HEX_LEN + 1u];

    if (snag_goal_unfinished(app->session.goal_status)) {
        (void)snprintf(error, error_size, "an unfinished goal already exists");
        errno = EINVAL;
        return -1;
    }
    if (snag_random_id(goal_id) < 0) {
        (void)snprintf(error, error_size,
                       "cryptographic goal id generation failed");
        return -1;
    }
    if (commit_goal_event(app, "goal_started",
                          goal_started_data(goal_id, prompt),
                          error, error_size) < 0)
        return -1;
    app->goal_armed = true;
    if (app->session.pending_queue_count != 0u && !app->queue_edit_id[0])
        app->queue_armed = true;
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
        free(prompt);
        if (rc < 0)
            return goal_error(app, error);
        return 0;
    }
    rc = start_goal(app, prompt, error, sizeof(error));
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
    data = goal_id_data(&app->session);
    if (!data || snag_json_set_new(data, "reason", json_string(reason)) < 0) {
        if (data)
            json_decref(data);
        (void)snprintf(error, error_size, "cannot allocate goal pause event");
        return -1;
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
        return snag_ui_text(&app->ui, SNAG_UI_HOST,
                               "goal paused at the current turn boundary");
    }
    if (strcmp(command, "resume") == 0) {
        if (app->session.goal_status != SNAG_GOAL_PAUSED &&
            app->session.goal_status != SNAG_GOAL_BLOCKED)
            return goal_error(app, "only a paused or blocked goal can be resumed");
        if (commit_goal_event(app, "goal_resumed", goal_id_data(&app->session),
                              error, sizeof(error)) < 0)
            return goal_error(app, error);
        app->goal_armed = true;
        if (app->session.pending_queue_count != 0u && !app->queue_edit_id[0])
            app->queue_armed = true;
        return snag_ui_text(&app->ui, SNAG_UI_HOST, "goal resumed");
    }
    if (strcmp(command, "lock") == 0 || strcmp(command, "unlock") == 0) {
        bool locked = strcmp(command, "lock") == 0;
        if (!snag_goal_unfinished(app->session.goal_status))
            return goal_error(app, "no unfinished goal can be locked or unlocked");
        if (app->session.goal_locked == locked)
            return snag_ui_text(&app->ui, SNAG_UI_WARNING,
                locked ? "goal wording is already locked" :
                         "goal wording is already unlocked");
        data = goal_id_data(&app->session);
        if (!data || snag_json_set_new(data, "locked", json_boolean(locked)) < 0) {
            if (data)
                json_decref(data);
            return goal_error(app, "cannot allocate goal lock event");
        }
        if (commit_goal_event(app, "goal_lock_changed", data,
                              error, sizeof(error)) < 0)
            return goal_error(app, error);
        return snag_ui_text(&app->ui, SNAG_UI_HOST,
            locked ? "goal wording locked against model changes" :
                     "goal wording unlocked for model changes");
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
    const char *argument;
    const char *word_end;
    size_t word_len;

    (void)active;
    if (strcmp(line, "/goal") == 0 || strcmp(line, "/goal status") == 0)
        return render_goal(app);
    if (strcmp(line, "/goal help") == 0)
        return snag_ui_text(&app->ui, SNAG_UI_HOST, goal_help);
    if (strncmp(line, "/goal ", 6u) != 0)
        return goal_error(app, "invalid /goal command; use /goal help");
    argument = line + 6u;
    while (*argument == ' ' || *argument == '\t' || *argument == '\r')
        ++argument;
    if (*argument == '"')
        return set_goal_prompt(app, argument);
    word_end = argument;
    while (*word_end && *word_end != ' ' && *word_end != '\t' &&
           *word_end != '\r')
        ++word_end;
    word_len = (size_t)(word_end - argument);
    if (word_len == 3u && memcmp(argument, "set", 3u) == 0) {
        if (!*word_end)
            return goal_error(app, "/goal set requires wording");
        return set_goal_prompt(app, word_end);
    }
    if (reserved_word(argument, word_len)) {
        if (*word_end)
            return goal_error(app,
                "reserved /goal command has extra text; use /goal set or quotes");
        if (word_len == 6u && memcmp(argument, "status", 6u) == 0)
            return render_goal(app);
        if (word_len == 4u && memcmp(argument, "help", 4u) == 0)
            return snag_ui_text(&app->ui, SNAG_UI_HOST, goal_help);
        return goal_simple_command(app, argument);
    }
    return set_goal_prompt(app, argument);
}

static int
tool_result(bool succeeded, const char *message, json_t **result)
{
    *result = snag_tool_result_terminal(succeeded, message);
    return *result ? 0 : -1;
}

int
snag_app_goal_tool(struct app_state *app,
                  const struct snag_response_item *call,
                  json_t **result, char *error, size_t error_size)
{
    static const char *const keys[] = {"action", "text"};
    const char *action;
    const char *text;
    json_t *text_value;
    json_t *data;

    *result = NULL;
    if (call && call->name && strcmp(call->name, "create_goal") == 0) {
        static const char *const create_keys[] = {"objective"};
        const char *objective;
        size_t len;
        char message[128];

        if (!snag_json_exact_keys(call->arguments, create_keys, 1u) ||
            !(objective = snag_json_string(call->arguments, "objective")))
            return tool_result(false, "create_goal arguments are invalid", result);
        if (snag_goal_unfinished(app->session.goal_status))
            return tool_result(false, "an unfinished goal already exists", result);
        if (!*objective || snag_text_blank(objective) ||
            (len = strlen(objective)) > app->config->max_goal_prompt_bytes ||
            len > SNAG_MAX_GOAL_PROMPT ||
            !snag_utf8_valid((const unsigned char *)objective, len, true))
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
        !snag_json_exact_keys(call->arguments, keys, 2u) ||
        !(action = snag_json_string(call->arguments, "action")))
        return tool_result(false, "update_goal arguments are invalid", result);
    text_value = json_object_get(call->arguments, "text");
    if (app->session.goal_status != SNAG_GOAL_ACTIVE)
        return tool_result(false, "there is no active goal to update", result);
    if (strcmp(action, "rewrite") == 0) {
        size_t len;
        text = snag_json_string(call->arguments, "text");
        if (!text || !*text || snag_text_blank(text) ||
            (len = strlen(text)) > app->config->max_goal_prompt_bytes ||
            len > SNAG_MAX_GOAL_PROMPT ||
            !snag_utf8_valid((const unsigned char *)text, len, true))
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
        size_t len;
        text = snag_json_string(call->arguments, "text");
        if (!text || !*text || snag_text_blank(text) ||
            (len = strlen(text)) > SNAG_MAX_GOAL_BLOCKER ||
            !snag_utf8_valid((const unsigned char *)text, len, true))
            return tool_result(false,
                               "block requires a bounded nonblank reason", result);
        data = goal_actor_data(&app->session, "model");
        if (!data || snag_json_set_new(data, "reason", json_string(text)) < 0) {
            if (data)
                json_decref(data);
            (void)snprintf(error, error_size, "cannot allocate goal block event");
            return -1;
        }
        if (commit_goal_event(app, "goal_blocked", data,
                              error, error_size) < 0)
            return -1;
        app->goal_armed = false;
        if (snag_ui_text(&app->ui, SNAG_UI_HOST, "goal blocked by model") < 0)
            return -1;
        return tool_result(true, "goal marked blocked", result);
    }
    return tool_result(false, "update_goal action is invalid", result);
}
