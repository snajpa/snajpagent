/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"
#include "json.h"
#include "skills.h"
#include "tools.h"

#include <stdlib.h>
#include <string.h>

static json_t *
visit_bindings(const struct snag_session *session, bool leaving)
{
    json_t *active = json_array();
    if (!active) return NULL;
    for (size_t i = 0; i < json_array_size(session->active_skills); ++i) {
        json_t *entry = json_array_get(session->active_skills, i);
        if (snag_json_string(entry, "visit_id") || !snag_skill_binding_active(session, entry))
            continue;
        const char *goal = snag_json_string(entry, "goal_id");
        bool returning = leaving && (!goal || !*goal);
        json_t *pin = returning ? json_copy(entry) : json_incref(entry);
        if (!pin || (returning && json_object_set_new(pin, "turn_id",
                json_string(session->active_turn_id)) < 0) ||
            json_array_append(active, pin) < 0) {
            json_decref(pin);
            json_decref(active);
            return NULL;
        }
        json_decref(pin);
    }
    return active;
}

static json_t *
visit_result(const char *cwd, const json_t *visit, const char *summary)
{
    json_t *state = json_pack("{s:s,s:O}", "cwd", cwd,
        "work_context", visit ? visit : json_null());
    if (state && summary && json_object_set_new(state, "summary", json_string(summary)) < 0) {
        json_decref(state);
        state = NULL;
    }
    char *text = state ? json_dumps(state, JSON_COMPACT | JSON_SORT_KEYS) : NULL;
    json_t *result = text ? snag_tool_result_terminal(true, text) : NULL;
    free(text);
    json_decref(state);
    return result;
}

int
snag_app_work_context_tool(struct app_state *app, const struct snag_response_item *call,
    json_t **result, char *error, size_t size)
{
    const char *action = NULL, *path = NULL, *task = NULL, *summary = NULL;
    char *joined = NULL, *resolved = NULL;
    struct snag_instruction_set instructions = {0};
    json_t *visit = NULL, *active = NULL, *catalog = NULL, *paths = NULL;
    bool navigating = !strcmp(call->name, "cd");
    int rc = -1;
    *result = NULL;
    if (navigating) {
        if (!snag_json_arg_keys(call->arguments, "path", "", error, size) ||
            !snag_json_arg_text(call->arguments, "path", 1u, SNAG_PATH_MAX_BYTES, false,
                &path, error, size)) goto refused;
    } else {
        if (!snag_json_arg_keys(call->arguments, "action", "path task summary", error, size) ||
            !snag_json_arg_text(call->arguments, "action", 1u, 6u, false,
                &action, error, size) ||
            !snag_json_arg_text(call->arguments, "path", 1u, SNAG_PATH_MAX_BYTES, true,
                &path, error, size) ||
            !snag_json_arg_text(call->arguments, "task", 1u, SNAG_MAX_EVENT_LINE, true,
                &task, error, size) ||
            !snag_json_arg_text(call->arguments, "summary", 0u, SNAG_MAX_EVENT_LINE, true,
                &summary, error, size)) goto refused;
        if (!strcmp(action, "status") && !path && !task && !summary) {
            *result = visit_result(app->session.cwd, app->session.work_context, NULL);
            return *result ? 0 : -1;
        }
    }
    bool entering = action && !strcmp(action, "enter");
    bool leaving = action && !strcmp(action, "leave");
    if (!navigating && ((!entering && !leaving) ||
        (entering && (!path || !task || summary)) || (leaving && task))) {
        (void)snag_errorf(error, size,
            "use enter with path and task, leave with optional summary/return path, or status");
        goto refused;
    }
    if (app->session.active_read_only) {
        (void)snag_errorf(error, size, "a read-only turn can inspect work_context status only");
        goto refused;
    }
    const char *previous_id = snag_json_string(app->session.work_context, "id");
    if (leaving && !previous_id) {
        (void)snag_errorf(error, size, "no temporary work context is active");
        goto refused;
    }
    if (!path) path = snag_json_string(app->session.work_context, "return_cwd");
    joined = snag_path_root_len(path) ? strdup(path) : snag_path_join(app->session.cwd, path);
    resolved = joined ? snag_cwd_resolve(joined, "requested", error, size) : NULL;
    if (!resolved) goto refused;
    if (entering) {
        char id[SNAG_ID_HEX_LEN + 1u];
        if (snag_random_id(id) < 0) goto done;
        const char *back = previous_id ?
            snag_json_string(app->session.work_context, "return_cwd") : app->session.cwd;
        const char *turn = previous_id ?
            snag_json_string(app->session.work_context, "return_turn_id") :
            app->session.active_turn_id;
        visit = json_pack("{s:s,s:s,s:s,s:s,s:s}", "id", id, "root", resolved,
            "task", task, "return_cwd", back, "return_turn_id", turn);
    } else {
        visit = json_incref(navigating && app->session.work_context ?
            app->session.work_context : json_null());
    }
    active = navigating ? (app->session.active_skills ?
        json_incref(app->session.active_skills) : json_array()) :
        visit_bindings(&app->session, leaving);
    if (!visit || !active) goto done;
    /* Both operations prepare the same scope. Navigation keeps its temporary role;
     * enter/leave changes that role, preserving the original parent return point. */
    if (app->config->read_agents_md && snag_instructions_discover_context(&instructions,
        resolved, snag_json_string(visit, "root"), error, size) < 0) goto refused;
    for (size_t i = 0; i < app->cli->doc_instructions.count; ++i) {
        const char *label = app->cli->doc_instructions.labels[i];
        if (snag_instructions_add_file(&instructions, app->cli->doc_instructions.paths[i],
            error, size) < 0 || (label && snag_instructions_label_last(&instructions,
                label, error, size) < 0)) goto refused;
    }
    paths = snag_instructions_metadata_json(&instructions);
    catalog = snag_app_skills_catalog(app, resolved, visit, error, size);
    if (!paths || !catalog) goto refused;
    *result = navigating ? snag_tool_result_terminal(true, resolved) :
        visit_result(resolved, visit, summary);
    json_t *event = json_pack("{s:s,s:s,s:O,s:O,s:O}", "old_cwd", app->session.cwd,
        "new_cwd", resolved, "work_context", visit, "active", active, "instructions", paths);
    if (!*result || !event) { json_decref(event); goto done; }
    if (snag_app_commit_event(app, "work_context_changed", event, error, size) < 0) goto done;
    snag_instructions_free(&app->turn_instructions);
    app->turn_instructions = instructions;
    instructions = (struct snag_instruction_set){0};
    json_decref(app->session.skills_catalog);
    app->session.skills_catalog = catalog;
    catalog = NULL;
    snag_context_skills_changed(&app->session);
    app->ui.view_state_seq = UINT64_MAX;
    rc = 0;
    goto done;
refused:
    *result = snag_tool_result_terminal(false,
        *error ? error : "cannot prepare working context");
    rc = *result ? 0 : -1;
done:
    if (rc < 0) { json_decref(*result); *result = NULL; }
    free(joined);
    free(resolved);
    snag_instructions_free(&instructions);
    json_decref(visit);
    json_decref(active);
    json_decref(catalog);
    json_decref(paths);
    return rc;
}
