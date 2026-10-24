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
    return snag_ui_send(&app->ui,
               (struct snag_ui_command){.kind = SNAG_UI_EVENT, .text = type, .data.seq = seq}) < 0
               ? -1
               : 0;
}

static bool
span_equals(const char *text, size_t len, const char *word)
{
    return strlen(word) == len && memcmp(text, word, len) == 0;
}

int
snag_app_parse_queue_argument(const char *argument, enum queue_command_kind *kind, size_t *number)
{
    const char *start = argument;
    const char *end = argument + strlen(argument);
    const char *p;
    size_t value = 0u;

    while (start < end && (*start == ' ' || *start == '\t')) ++start;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) --end;
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

        if (value > (SIZE_MAX - digit) / 10u) return -1;
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
    if (*p != ' ' && *p != '\t') return -1;
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    if (span_equals(p, (size_t)(end - p), "d") || span_equals(p, (size_t)(end - p), "delete")) {
        *kind = QUEUE_COMMAND_DELETE;
        *number = value;
        return 0;
    }
    if (span_equals(p, (size_t)(end - p), "e") || span_equals(p, (size_t)(end - p), "edit")) {
        *kind = QUEUE_COMMAND_EDIT;
        *number = value;
        return 0;
    }
    return -1;
}

static int
confirm_delete(struct app_state *app, char prefix[9], char *error, size_t error_size)
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
        if (snag_tools_service(0, snag_ui_wake_fd(&app->ui), error, error_size) < 0) return -1;
        if (snag_app_voice_service(app) < 0) return -1;
        rc = snag_ui_poll(&app->ui, 25, &action, &line);
        /* UI-local commands already ran and intentionally carry no text. */
        if (rc > 0 && action == SNAG_TERM_SUBMIT && !line) rc = 0;
        if (rc > 0 && action == SNAG_TERM_SUBMIT && line && line[0] == '/' && line[1] != '/') {
            bool handled = false, prompt_ready = false;
            (void)snag_ui_history_add(&app->ui, line);
            rc = snag_app_input_command(app, line, true, &handled, &prompt_ready);
            free(line);
            line = NULL;
            if (rc < 0) return -1;
            if (app->input_closed) {
                if (error_size) error[0] = '\0';
                return 1;
            }
            if (app->queue_edit_id[0]) {
                if (error_size)
                    snprintf(error, error_size, "delete cancelled; queue editor opened");
                return 1;
            }
            if (!handled && snag_ui_text(&app->ui, SNAG_UI_ERROR, "unknown slash command") < 0)
                return -1;
            if (app->session.pending_controls & ~SNAG_CONTROL_DELETE &&
                snag_ui_text(&app->ui, SNAG_UI_HOST,
                    "pending controls apply after delete confirmation; Ctrl-C cancels deletion") <
                    0)
                return -1;
            if (snag_ui_simple_prompt(&app->ui, false) < 0) return -1;
            rc = 0;
        }
    } while (rc == 0);
    if (rc < 0) {
        free(line);
        return snag_errorf(error, error_size, "delete confirmation input could not be read");
    }
    if (action == SNAG_TERM_CANCEL || action == SNAG_TERM_INTERRUPT) {
        free(line);
        if (error_size) error[0] = '\0';
        return 1;
    }
    if (action == SNAG_TERM_EXIT || !line) {
        if (action == SNAG_TERM_EXIT) app->input_closed = true;
        free(line);
        snprintf(error, error_size, "delete cancelled");
        return 1;
    }
    if (strcmp(line, prefix) != 0) {
        free(line);
        (void)snag_fail(
            error, error_size, EINVAL, "delete confirmation did not match %.8s", app->session.id);
        return 1;
    }
    free(line);
    return 0;
}

/* Fresh local consent for a rule-requested action. The challenge is generated
 * here and must be typed back; model, IRC and helper bytes cannot answer it.
 * Returns 0 confirmed, 1 not confirmed (or not presentable), -1 on error. */
int
snag_app_consent(struct app_state *app, const char *reason, char *error, size_t error_size)
{
    enum snag_term_action action = SNAG_TERM_NONE;
    char id[SNAG_ID_HEX_LEN + 1u];
    char text[1024];
    char *line = NULL;
    bool matched;
    int rc;

    if (!app->ui.opened || app->execute) {
        if (error_size)
            snprintf(
                error, error_size, "local confirmation is required and this run cannot present it");
        return 1;
    }
    if (snag_random_id(id) < 0) return -1;
    id[8] = '\0';
    if (snprintf(text, sizeof(text), "confirmation required: %s; type %s to confirm",
            reason && *reason ? reason : "a configured rule", id) < 0)
        return -1;
    if (snag_ui_text(&app->ui, SNAG_UI_HOST, text) < 0 ||
        snag_ui_simple_prompt(&app->ui, false) < 0)
        return snag_errorf(error, error_size, "confirmation prompt could not be displayed");
    do {
        if (snag_tools_service(0, snag_ui_wake_fd(&app->ui), error, error_size) < 0) return -1;
        if (snag_app_voice_service(app) < 0) return -1;
        rc = snag_ui_poll(&app->ui, 25, &action, &line);
        if (rc > 0 && action == SNAG_TERM_SUBMIT && !line) rc = 0;
        if (rc > 0 && action == SNAG_TERM_SUBMIT && line && line[0] == '/' && line[1] != '/') {
            bool handled = false, prompt_ready = false;
            (void)snag_ui_history_add(&app->ui, line);
            rc = snag_app_input_command(app, line, true, &handled, &prompt_ready);
            free(line);
            line = NULL;
            if (rc < 0) return -1;
            if (app->input_closed) {
                if (error_size) error[0] = '\0';
                return 1;
            }
            if (!handled && snag_ui_text(&app->ui, SNAG_UI_ERROR, "unknown slash command") < 0)
                return -1;
            if (snag_ui_simple_prompt(&app->ui, false) < 0) return -1;
            rc = 0;
        }
    } while (rc == 0);
    if (rc < 0) {
        free(line);
        return snag_errorf(error, error_size, "confirmation input could not be read");
    }
    if (action != SNAG_TERM_SUBMIT || !line) {
        free(line);
        if (error_size) snprintf(error, error_size, "confirmation was cancelled");
        return 1;
    }
    matched = !app->ui.input_interface && strcmp(line, id) == 0;
    free(line);
    if (!matched) {
        if (error_size)
            snprintf(error, error_size, "confirmation did not match the displayed challenge");
        return 1;
    }
    return 0;
}

int
snag_app_lifecycle_command(struct app_state *app, const char *line, bool *handled, bool *exit_now)
{
    char error[256];
    uint64_t seq;

    *handled = true;
    *exit_now = false;
    if (strcmp(line, "/delete") == 0) {
        if (app->session.pending_log) {
            *exit_now = true;
            return 0;
        }
        char prefix[9];
        int confirm_rc = confirm_delete(app, prefix, error, sizeof(error));
        if (confirm_rc != 0) {
            if (error[0]) (void)snag_ui_text(&app->ui, SNAG_UI_ERROR, error);
            return confirm_rc < 0 ? -1 : 0;
        }
        /* Confirmation precedes stopping any owned processes. */
        if (app->session.process_count &&
            snag_app_close_active_processes(
                app, app->session.active_turn_id, "user_interrupt", true, error, sizeof(error)) < 0)
            return -1;
        /* Unlink the private endpoint while its original writer lock is held. */
        if (app->ui.native && snag_ui_session_listen(&app->ui, NULL) < 0) return -1;
        seq = app->session.next_seq;
        if (snag_session_delete(&app->store, &app->session, prefix, &seq, error, sizeof(error)) <
            0) {
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

static int
goal_error(struct app_state *app, const char *message)
{
    return snag_ui_text(&app->ui, SNAG_UI_ERROR, message);
}

static int
commit_goal_event(struct app_state *app, const char *type, json_t *data,
    char *error, size_t error_size)
{
    if (!data) return snag_errorf(error, error_size, "cannot allocate %s event", type);
    int rc = snag_app_commit_event(app, type, data, error, error_size);
    if (!rc) app->goal_armed = snag_goals_status(&app->session) == SNAG_GOAL_ACTIVE;
    return rc;
}

static int
render_goal(struct app_state *app)
{
    if (!json_object_size(app->session.goals))
        return snag_app_report(app, SNAG_UI_WARNING, "no goal has been set");
    const char *id;
    const json_t *goal;
    json_object_foreach(app->session.goals, id, goal) {
        enum snag_goal_status status = (enum snag_goal_status)json_integer_value(
            json_object_get(goal, "status"));
        struct snag_buf text = {.max = SNAG_MAX_GOAL_PROMPT + 2u * SNAG_MAX_GOAL_BLOCKER + 512u};
        const char *prompt = snag_json_string(goal, "prompt");
        const char *blocker = snag_json_string(goal, "blocker");
        const char *parent = snag_json_string(goal, "parent");
        const char *replacement = snag_json_string(goal, "replaced_by");
        int rc = snag_buf_printf(&text,
            "goal %s: %s · wording %s\nturns: %llu · revision: %llu · prompt: %zu/%u bytes\n%s",
            id, replacement ? "replaced" : snag_goal_status_name(status),
            json_is_true(json_object_get(goal, "locked")) ? "locked" : "unlocked",
            (unsigned long long)json_integer_value(json_object_get(goal, "turns")),
            (unsigned long long)json_integer_value(json_object_get(goal, "revision")),
            strlen(prompt), app->config->max_goal_prompt_bytes, prompt);
        if (!rc && parent && *parent) rc = snag_buf_printf(&text, "\nparent goal: %s", parent);
        if (!rc && blocker && *blocker) rc = snag_buf_printf(&text, "\nblocker: %s", blocker);
        if (!rc && status == SNAG_GOAL_BLOCKED)
            rc = snag_buf_printf(&text, "\nwait channel: %s", snag_json_string(goal, "wait_for"));
        if (!rc && replacement) rc = snag_buf_printf(&text, "\nreplaced by: %s", replacement);
        if (!rc) rc = snag_buf_terminate(&text);
        if (!rc) rc = snag_app_report(app, SNAG_UI_HOST, (const char *)text.data);
        snag_buf_free(&text);
        if (rc < 0) return rc;
    }
    return 0;
}

static char *
copy_goal_argument(const char *argument, uint32_t limit, char *error, size_t error_size)
{
    const char *start = argument;
    const char *end;
    char *copy;
    size_t len;

    while (*start == ' ' || *start == '\t' || *start == '\r') ++start;
    end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) --end;
    if (start < end && *start == '"') {
        if (end - start < 2 || end[-1] != '"') {
            (void)snag_fail(
                error, error_size, EINVAL, "quoted goal wording requires a closing double quote");
            return NULL;
        }
        ++start;
        --end;
    }
    len = (size_t)(end - start);
    if (!len || len > limit || len > SNAG_MAX_GOAL_PROMPT) {
        (void)snag_fail(
            error, error_size, EINVAL, "goal wording must contain 1..%u UTF-8 bytes", limit);
        return NULL;
    }
    copy = malloc(len + 1u);
    if (!copy) return NULL;
    memcpy(copy, start, len);
    copy[len] = '\0';
    if (snag_text_blank(copy) || !snag_utf8_valid((const unsigned char *)copy, len, true)) {
        free(copy);
        (void)snag_fail(error, error_size, EINVAL, "goal wording must be nonblank valid UTF-8");
        return NULL;
    }
    return copy;
}

static int
start_goal(struct app_state *app, const char *prompt, char *error, size_t error_size)
{
    char id[SNAG_ID_HEX_LEN + 1u];
    if (snag_random_id(id) < 0)
        return snag_errorf(error, error_size, "cryptographic goal id generation failed");
    return commit_goal_event(app, "goal_started",
        json_pack("{s:s,s:s}", "goal_id", id, "prompt", prompt), error, error_size);
}

static int
replace_goal(struct app_state *app, const char *id, const char *actor, const char *prompt,
    char *error, size_t error_size)
{
    char replacement[SNAG_ID_HEX_LEN + 1u];
    if (snag_random_id(replacement) < 0) return -1;
    return commit_goal_event(app, "goal_replaced", json_pack("{s:s,s:s,s:s,s:s}",
        "goal_id", id, "new_goal_id", replacement, "actor", actor, "prompt", prompt),
        error, error_size);
}

int
snag_app_goal_pause(struct app_state *app, const char *reason, char *error, size_t error_size)
{
    /* Copy the directory: every successful commit replaces the immutable owner. */
    json_t *goals = json_incref(app->session.goals);
    const char *id;
    const json_t *goal;
    int rc = 0;
    json_object_foreach(goals, id, goal) {
        if (json_integer_value(json_object_get(goal, "status")) != SNAG_GOAL_ACTIVE) continue;
        if (commit_goal_event(app, "goal_paused",
                json_pack("{s:s,s:s}", "goal_id", id, "reason", reason), error, error_size) < 0) {
            rc = -1;
            break;
        }
    }
    json_decref(goals);
    app->goal_armed = false;
    return rc;
}

static int
change_goal(struct app_state *app, const char *command, const json_t *goal,
    char *error, size_t size)
{
    const char *id = snag_json_string(goal, "id");
    enum snag_goal_status status = (enum snag_goal_status)json_integer_value(
        json_object_get(goal, "status"));
    if (!snag_goal_unfinished(status))
        return snag_fail(error, size, EINVAL, "goal is already finished");
    const char *type;
    json_t *data;
    if (!strcmp(command, "pause")) {
        if (status == SNAG_GOAL_PAUSED) return 0;
        type = "goal_paused";
        data = json_pack("{s:s,s:s}", "goal_id", id, "reason", "user");
    } else if (!strcmp(command, "resume")) {
        if (status == SNAG_GOAL_ACTIVE) return 0;
        type = "goal_resumed";
        data = json_pack("{s:s}", "goal_id", id);
    } else if (snag_string_in(command, "lock unlock")) {
        bool locked = !strcmp(command, "lock");
        if (locked == json_is_true(json_object_get(goal, "locked"))) return 0;
        type = "goal_lock_changed";
        data = json_pack("{s:s,s:b}", "goal_id", id, "locked", locked);
    } else if (!strcmp(command, "complete")) {
        type = "goal_completed";
        data = json_pack("{s:s,s:s}", "goal_id", id, "actor", "user");
    } else {
        type = "goal_cancelled";
        data = json_pack("{s:s}", "goal_id", id);
    }
    return commit_goal_event(app, type, data, error, size);
}

int
snag_app_goal_command(struct app_state *app, const char *line, bool active)
{
    const char *argument = line + 5u;
    while (isspace((unsigned char)*argument)) ++argument;
    if (!*argument) return render_goal(app);
    const char *end = argument;
    while (*end && !isspace((unsigned char)*end)) ++end;
    char command[16] = {0};
    if ((size_t)(end - argument) < sizeof(command))
        memcpy(command, argument, (size_t)(end - argument));
    const char *rest = end;
    while (isspace((unsigned char)*rest)) ++rest;
    if (!*rest && !strcmp(command, "status")) return render_goal(app);
    if (!*rest && !strcmp(command, "help")) return snag_app_help(app, "/state goal");
    char error[256] = {0};
    bool control = snag_string_in(command, "pause resume lock unlock complete cancel clear");
    if (control) {
        int rc = 0;
        if (!*rest && snag_string_in(command, "pause resume")) {
            json_t *goals = json_incref(app->session.goals);
            const char *id;
            const json_t *goal;
            json_object_foreach(goals, id, goal) {
                (void)id;
                if (!snag_goal_unfinished((enum snag_goal_status)json_integer_value(
                        json_object_get(goal, "status")))) continue;
                if ((rc = change_goal(app, command, goal, error, sizeof(error))) < 0) break;
            }
            json_decref(goals);
        } else {
            const char *id_end = rest;
            while (*id_end && !isspace((unsigned char)*id_end)) ++id_end;
            const char *tail = id_end;
            while (isspace((unsigned char)*tail)) ++tail;
            char id[SNAG_ID_HEX_LEN + 1u] = {0};
            if (*tail || (id_end != rest && id_end - rest != SNAG_ID_HEX_LEN))
                return goal_error(app, "specify an unfinished goal ID from /goal");
            memcpy(id, rest, (size_t)(id_end - rest));
            const json_t *goal = snag_goal_select(&app->session, id);
            if (!goal) return goal_error(app, "specify an unfinished goal ID from /goal");
            rc = change_goal(app, command, goal, error, sizeof(error));
        }
        if (rc < 0) return goal_error(app, error);
        if (!strcmp(command, "resume") &&
            snag_goals_status(&app->session) == SNAG_GOAL_ACTIVE &&
            snag_app_goal_resume_input(app, active, error, sizeof(error)) < 0)
            return goal_error(app, error);
        return render_goal(app);
    }
    bool set = !strcmp(command, "set");
    const char *wording = set || !strcmp(command, "add") ? rest : argument;
    const json_t *goal = NULL;
    char id[SNAG_ID_HEX_LEN + 1u] = {0};
    if (set) {
        const char *split = wording;
        while (*split && !isspace((unsigned char)*split)) ++split;
        if (split - wording == SNAG_ID_HEX_LEN) memcpy(id, wording, SNAG_ID_HEX_LEN);
        if (*id && snag_hex_is_lower(id, SNAG_ID_HEX_LEN)) {
            goal = snag_goal_find(&app->session, id);
            if (!goal || !snag_goal_unfinished((enum snag_goal_status)json_integer_value(
                    json_object_get(goal, "status"))))
                return goal_error(app, "specify an unfinished goal ID from /goal");
            wording = split;
        } else goal = snag_goal_select(&app->session, NULL);
        if (snag_goal_unfinished(snag_goals_status(&app->session)) && !goal)
            return goal_error(app, "/goal set requires an unambiguous goal ID and wording");
    }
    char *prompt = copy_goal_argument(wording, app->config->max_goal_prompt_bytes,
        error, sizeof(error));
    if (!prompt) return goal_error(app, error[0] ? error : "goal wording unavailable");
    int rc = goal ? replace_goal(app, snag_json_string(goal, "id"), "user", prompt,
        error, sizeof(error)) : start_goal(app, prompt, error, sizeof(error));
    free(prompt);
    return rc < 0 ? goal_error(app, error) : 0;
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
snag_app_goal_tool(struct app_state *app, const struct snag_response_item *call, json_t **result,
    char *error, size_t error_size)
{
    *result = NULL;
    size_t limit = app->config->max_goal_prompt_bytes;
    if (!strcmp(call->name, "create_goal")) {
        const char *objective;
        if (!snag_json_arg_keys(call->arguments, "objective", "", error, error_size) ||
            !snag_json_arg_text(call->arguments, "objective", 1u, limit, false, &objective,
                error, error_size) || !goal_text_valid(objective, limit))
            return tool_result(false, error[0] ? error : "invalid goal objective", result);
        if (start_goal(app, objective, error, error_size) < 0) return -1;
        char message[128];
        (void)snprintf(message, sizeof(message),
            "goal %s started; automatic continuation is active",
            app->session.goal_id);
        return tool_result(true, message, result);
    }
    const char *action;
    if (!snag_json_arg_keys(call->arguments, "action", "id text wait_for", error, error_size) ||
        !snag_json_arg_text(call->arguments, "action", 1u, 8u, false, &action, error, error_size))
        return tool_result(false, error, result);
    json_t *id_value = json_object_get(call->arguments, "id");
    if (id_value && !json_is_null(id_value) && (!json_is_string(id_value) ||
            json_string_length(id_value) != SNAG_ID_HEX_LEN ||
            !snag_hex_is_lower(json_string_value(id_value), SNAG_ID_HEX_LEN)))
        return tool_result(false, "id must be a goal ID or null", result);
    const json_t *goal = snag_goal_select(&app->session, json_string_value(id_value));
    if (!goal) return tool_result(false,
        "specify an unfinished goal ID from the goal list", result);
    enum snag_goal_status status = (enum snag_goal_status)json_integer_value(
        json_object_get(goal, "status"));
    if (!snag_goal_unfinished(status))
        return tool_result(false, "goal is already finished", result);
    const char *id = snag_json_string(goal, "id");
    json_t *text_value = json_object_get(call->arguments, "text");
    json_t *wait_value = json_object_get(call->arguments, "wait_for");
    if (strcmp(action, "block") && wait_value && !json_is_null(wait_value))
        return tool_result(false, "wait_for is only valid for action=block", result);
    if (json_is_true(json_object_get(goal, "locked")) &&
        snag_string_in(action, "rewrite block"))
        return tool_result(false, "goal is locked by the operator", result);
    if (!strcmp(action, "rewrite")) {
        const char *text = json_string_value(text_value);
        if (!goal_text_valid(text, limit))
            return tool_result(false, "new goal wording exceeds the configured limit or is invalid",
                result);
        if (!strcmp(text, snag_json_string(goal, "prompt")))
            return tool_result(true, "goal wording is unchanged", result);
        char old_id[33];
        memcpy(old_id, id, sizeof(old_id));
        if (replace_goal(app, old_id, "model", text, error, error_size) < 0) return -1;
        char message[160];
        (void)snprintf(message, sizeof(message), "goal %s replaced by %s with explicit lineage",
            old_id, app->session.goal_id);
        return tool_result(true, message, result);
    }
    if (!strcmp(action, "block")) {
        const char *text = json_string_value(text_value);
        const char *wait = json_string_value(wait_value);
        if (status != SNAG_GOAL_ACTIVE || !goal_text_valid(text, SNAG_MAX_GOAL_BLOCKER))
            return tool_result(false, "block needs an active goal and a nonblank reason", result);
        if (!snag_goal_wait_valid(wait))
            return tool_result(false, "block requires wait_for naming a valid dependency", result);
        if (commit_goal_event(app, "goal_blocked", json_pack("{s:s,s:s,s:s,s:s}",
                "goal_id", id, "actor", "model", "reason", text, "wait_for", wait),
                error, error_size) < 0) return -1;
        return tool_result(true, "goal marked blocked; other active goals continue", result);
    }
    if (text_value && !json_is_null(text_value))
        return tool_result(false, "complete and resume require null text", result);
    if (!strcmp(action, "complete")) {
        if (app->session.process_count)
            return tool_result(false,
                "collect all command handles before completing a goal", result);
        if (status != SNAG_GOAL_ACTIVE)
            return tool_result(false, "only an active goal can be completed by the model", result);
        if (commit_goal_event(app, "goal_completed", json_pack("{s:s,s:s}",
                "goal_id", id, "actor", "model"), error, error_size) < 0) return -1;
        return tool_result(true, "goal completed; other active goals continue", result);
    }
    if (!strcmp(action, "resume")) {
        if (status == SNAG_GOAL_ACTIVE) return tool_result(false, "goal is already active", result);
        if (commit_goal_event(app, "goal_resumed", json_pack("{s:s}", "goal_id", id),
                error, error_size) < 0) return -1;
        return tool_result(true, "goal resumed", result);
    }
    return tool_result(false, "action must be rewrite, complete, block or resume", result);
}

int
snag_app_timer_tool(struct app_state *app, const struct snag_response_item *call, json_t **result,
    char *error, size_t error_size)
{
    *result = NULL;
    if (!snag_json_arg_keys(call->arguments, "", "action id delay_ms text recurring", error,
            error_size)) return tool_result(false, error, result);
    const char *fields[] = {"action", "id"};
    for (size_t i = 0u; i < sizeof(fields) / sizeof(*fields); ++i) {
        json_t *value = json_object_get(call->arguments, fields[i]);
        if (value && !json_is_null(value) && !json_is_string(value))
            return tool_result(false, "action and id must be strings or null", result);
    }
    json_t *recurring = json_object_get(call->arguments, "recurring");
    if (recurring && !json_is_boolean(recurring) && !json_is_null(recurring))
        return tool_result(false, "recurring must be boolean or null", result);
    const char *action = snag_json_string(call->arguments, "action");
    if (!action) action = "schedule";
    const char *id = snag_json_string(call->arguments, "id");
    const json_t *timer = id ? json_object_get(app->session.timers, id) : NULL;
    if (!strcmp(action, "list")) {
        struct snag_buf list = {.max = app->session.tool_output_bytes};
        int rc = snag_buf_printf(&list, "Armed timers: %zu\n",
            json_object_size(app->session.timers));
        const char *key;
        const json_t *entry;
        json_object_foreach(app->session.timers, key, entry) {
            if (rc < 0) break;
            rc = snag_buf_printf(&list, "id=%s due_ms=%llu interval_ms=%llu recurring=%s\n", key,
                (unsigned long long)json_integer_value(json_object_get(entry, "due_ms")),
                (unsigned long long)json_integer_value(json_object_get(entry, "interval_ms")),
                json_integer_value(json_object_get(entry, "interval_ms")) ? "true" : "false");
        }
        if (!rc) rc = snag_buf_terminate(&list);
        if (!rc) rc = tool_result(true, (const char *)list.data, result);
        snag_buf_free(&list);
        return rc;
    }
    if (!snag_string_in(action, "schedule cancel"))
        return tool_result(false, "action must be schedule, list or cancel", result);
    if (!strcmp(action, "schedule") &&
        !json_is_integer(json_object_get(call->arguments, "delay_ms")))
        return tool_result(false, "schedule requires an integer delay_ms", result);
    uint64_t delay = 0u;
    if (strcmp(action, "cancel") &&
        !snag_json_arg_uint(call->arguments, "delay_ms", 0u, 0u, UINT32_MAX, &delay,
            error, error_size)) return tool_result(false, error, result);
    if (!delay) {
        if (json_object_get(call->arguments, "text") &&
            !json_is_null(json_object_get(call->arguments, "text")))
            return tool_result(false, "cancellation requires null text", result);
        if (!id && json_object_size(app->session.timers) == 1u) {
            void *iter = json_object_iter(app->session.timers);
            id = json_object_iter_key(iter);
            timer = json_object_iter_value(iter);
        }
        if (!id && !json_object_size(app->session.timers))
            return tool_result(true, "no timer is scheduled", result);
        if (!timer) return tool_result(false, "specify an armed timer ID from timer action=list",
            result);
        if (snag_app_commit_event(app, "timer_cancelled", json_pack("{s:s}", "timer_id", id),
                error, error_size) < 0) return -1;
        return tool_result(true, "timer cancelled; other timers unchanged", result);
    }
    const char *text;
    if (!snag_json_arg_text(call->arguments, "text", 1u, SNAG_MAX_TIMER_TEXT, false, &text,
            error, error_size) || snag_text_blank(text))
        return tool_result(false, error[0] ? error : "timer text must be nonblank", result);
    char generated[SNAG_ID_HEX_LEN + 1u];
    if (id && !timer) return tool_result(false,
        "unknown timer ID; omit id to create a timer", result);
    if (!id) {
        if (snag_random_id(generated) < 0) return -1;
        id = generated;
    }
    uint64_t now = snag_time_ms();
    if (now > INT64_MAX - delay) return tool_result(false, "timer deadline overflow", result);
    json_t *data = json_pack("{s:s,s:I,s:s}", "timer_id", id,
        "due_ms", (json_int_t)(now + delay), "text", text);
    if (!data) return -1;
    if (json_is_true(recurring) && json_object_set_new(data, "interval_ms",
            json_integer((json_int_t)delay)) < 0) { json_decref(data); return -1; }
    char message[160];
    (void)snprintf(message, sizeof(message), "timer %s scheduled for %llu%s", id,
        (unsigned long long)(now + delay), json_is_true(recurring) ? "; recurring" : "");
    if (snag_app_commit_event(app, "timer_scheduled", data, error, error_size) < 0) return -1;
    return tool_result(true, message, result);
}
