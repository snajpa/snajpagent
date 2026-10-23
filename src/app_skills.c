/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"
#include "json.h"
#include "process_host.h"
#include "skills.h"
#include "tools.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int save_bindings(struct app_state *, json_t *, char *, size_t);

json_t *
snag_app_skills_catalog(struct app_state *app, const char *cwd, const json_t *work_context,
    char *error, size_t size)
{
    json_t *settings = snag_skills_settings_load(app->store.root_path, error, size);
    if (!settings) return NULL;
    if (app->config->skills_roots &&
        json_object_set(settings, "roots", app->config->skills_roots) < 0) {
        json_decref(settings);
        return NULL;
    }
    const char *root = snag_json_string(work_context, "root");
    if (root && json_object_set_new(settings, "visit_root", json_string(root)) < 0) {
        json_decref(settings);
        return NULL;
    }
    json_t *catalog = snag_skills_discover(cwd, app->store.root_path,
        settings, error, size);
    json_decref(settings);
    return catalog;
}

int
snag_app_skills_refresh(struct app_state *app, char *error, size_t size)
{
    json_t *catalog = snag_app_skills_catalog(app, app->session.cwd,
        app->session.work_context, error, size);
    if (!catalog) return -1;
    if (!json_equal(catalog, app->session.skills_catalog)) {
        snag_context_skills_changed(&app->session);
    }
    json_decref(app->session.skills_catalog);
    app->session.skills_catalog = catalog;
    app->ui.view_state_seq = UINT64_MAX;
    return snag_ui_view_state(&app->ui, &app->session);
}

static json_t *
active_bindings(const struct snag_session *session, const json_t *replacement)
{
    json_t *active = json_array();
    if (!active) return NULL;
    for (size_t i = 0; i < json_array_size(session->active_skills); ++i) {
        json_t *entry = json_array_get(session->active_skills, i);
        if (replacement &&
            !strcmp(snag_json_string(entry, "id"), snag_json_string(replacement, "id")) &&
            !strcmp(snag_json_string(entry, "arguments"),
                snag_json_string(replacement, "arguments"))) continue;
        if (snag_skill_binding_active(session, entry) && json_array_append(active, entry) < 0) {
            json_decref(active);
            return NULL;
        }
    }
    return active;
}

static int
save_bindings(struct app_state *app, json_t *active, char *error, size_t size)
{
    if (json_equal(active, app->session.active_skills) ||
        (!app->session.active_skills && !json_array_size(active))) return 0;
    return snag_app_commit_event(app, "skills_updated",
        json_pack("{s:O}", "active", active), error, size);
}

static json_t *
dependency_state(const struct app_state *app, const json_t *skill)
{
    json_t *requirements = json_object_get(json_object_get(
        json_object_get(skill, "openai"), "dependencies"), "tools");
    json_t *result = json_array();
    if (!result) return NULL;
    for (size_t i = 0; i < json_array_size(requirements); ++i) {
        json_t *item = json_array_get(requirements, i);
        json_t *state = snag_mcp_requirement(app->mcp, item);
        if (!state || json_array_append_new(result, state) < 0) {
            json_decref(result);
            return NULL;
        }
    }
    return result;
}

static json_t *
allowed_tools(const struct app_state *app, const json_t *skill)
{
    const char *hint = snag_json_string(json_object_get(skill, "metadata"), "allowed-tools");
    json_t *result = json_array();
    if (!hint || !*hint) return result;
    json_t *catalog = snag_context_tool_catalog(app->config, &app->session,
        app->session.active_turn_provider, snag_mcp_tools(app->mcp));
    char *copy = strdup(hint);
    if (!copy || !catalog || !result) goto fail;
    char *save = NULL;
    for (char *name = strtok_r(copy, " ,\t\r\n", &save); name;
        name = strtok_r(NULL, " ,\t\r\n", &save)) {
        bool available = false;
        for (size_t i = 0; i < json_array_size(catalog); ++i) {
            const char *actual = snag_json_string(json_array_get(catalog, i), "name");
            if (actual && !strcmp(actual, name)) available = true;
        }
        if (json_array_append_new(result, json_pack("{s:s,s:s,s:b}", "name", name,
            "state", available ? "available" : "unavailable", "grants_permission", false)) < 0)
            goto fail;
    }
    free(copy);
    json_decref(catalog);
    return result;
fail:
    free(copy);
    json_decref(catalog);
    json_decref(result);
    return NULL;
}

static json_t *
activate(struct app_state *app, const char *id, const char *arguments, bool explicit_selection,
    char *error, size_t size)
{
    json_t *skill = snag_skills_activate(app->session.skills_catalog, id, arguments,
        explicit_selection, error, size);
    if (!skill) return NULL;
    /* Revalidate instructions, then reuse the binding before collecting resources
     * or capability diagnostics that are already part of its pinned snapshot. */
    json_t *reusable = NULL;
    size_t matches = 0;
    for (size_t i = 0; i < json_array_size(app->session.active_skills); ++i) {
        json_t *previous = json_array_get(app->session.active_skills, i);
        if (snag_skill_binding_active(&app->session, previous) &&
            !strcmp(snag_json_string(previous, "id"), snag_json_string(skill, "id")) &&
            !strcmp(snag_json_string(previous, "arguments"),
                snag_json_string(skill, "arguments"))) {
            if (++matches > 1u) break;
            if (!strcmp(snag_json_string(previous, "digest"), snag_json_string(skill, "digest")))
                reusable = previous;
        }
    }
    if (matches == 1u && reusable) {
        json_decref(skill);
        return json_incref(reusable);
    }
    json_t *active = active_bindings(&app->session, skill);
    const char *visit = snag_json_string(app->session.work_context, "id");
    const char *goal = !visit && app->session.goal_status == SNAG_GOAL_ACTIVE &&
        !app->session.active_read_only ? app->session.goal_id : "";
    json_t *dependencies = dependency_state(app, skill);
    json_t *allowed = allowed_tools(app, skill);
    json_t *resources = snag_skill_resources(snag_json_string(skill, "root"),
        app->config->tool_output_bytes, error, size);
    if (!active || !dependencies || !allowed || !resources ||
        json_object_set_new(skill, "turn_id", json_string(app->session.active_turn_id)) < 0 ||
        json_object_set_new(skill, "goal_id", json_string(goal)) < 0 ||
        (visit && json_object_set_new(skill, "visit_id", json_string(visit)) < 0) ||
        json_object_set(skill, "dependencies", dependencies) < 0 ||
        json_object_set(skill, "allowed_tools", allowed) < 0 ||
        json_object_set(skill, "resources", resources) < 0) {
        json_decref(dependencies);
        json_decref(allowed);
        json_decref(resources);
        json_decref(active);
        json_decref(skill);
        return NULL;
    }
    json_decref(dependencies);
    json_decref(allowed);
    json_decref(resources);
    if (json_array_append(active, skill) < 0 || save_bindings(app, active, error, size) < 0) {
        json_decref(skill);
        skill = NULL;
    } else if (snag_ui_send(&app->ui, (struct snag_ui_command){.kind = SNAG_UI_NOTICE,
            .label = "Skill loaded: ", .text = snag_json_string(skill, "name")}) < 0) {
        (void)snag_errorf(error, size, "skill loaded but success notice failed");
        json_decref(skill);
        skill = NULL;
    }
    json_decref(active);
    return skill;
}

static const char *
prompt_skill(const json_t *catalog, const char *text, size_t length)
{
    json_t *entries = json_object_get(catalog, "entries");
    for (size_t i = 0; i < json_array_size(entries); ++i) {
        const json_t *entry = json_array_get(entries, i);
        const char *id = snag_json_string(entry, "id");
        const char *name = snag_json_string(entry, "name");
        if (strlen(id) == length && !memcmp(id, text, length)) return id;
        if (json_is_true(json_object_get(entry, "selected")) &&
            strlen(name) == length && !memcmp(name, text, length)) return name;
    }
    return NULL;
}

int
snag_app_skills_begin(struct app_state *app, const char *prompt, bool continuing,
    bool retry_binding, char *error, size_t size)
{
    if (continuing) return app->session.skills_catalog ? 0 :
        snag_app_skills_refresh(app, error, size);
    if (retry_binding && app->session.active_skills) {
        json_t *active = json_deep_copy(app->session.active_skills);
        if (!active) return -1;
        int rc = 0;
        for (size_t i = 0; i < json_array_size(active); ++i) {
            json_t *skill = json_array_get(active, i);
            const char *goal = snag_json_string(skill, "goal_id");
            if ((!goal || !*goal) && json_object_set_new(skill, "turn_id",
                json_string(app->session.active_turn_id)) < 0) { rc = -1; break; }
        }
        if (rc == 0) rc = save_bindings(app, active, error, size);
        json_decref(active);
        return rc;
    }
    if (snag_app_skills_refresh(app, error, size) < 0) return -1;
    if (!app->session.active_read_only) {
        json_t *active = active_bindings(&app->session, NULL);
        if (!active) return -1;
        int rc = save_bindings(app, active, error, size);
        json_decref(active);
        if (rc < 0) return rc;
    }
    if (!prompt) return 0;
    if (!strncmp(prompt, "/skills use ", 12u)) {
        const char *start = prompt + 12u;
        while (isspace((unsigned char)*start)) ++start;
        const char *end = start;
        while (*end && !isspace((unsigned char)*end)) ++end;
        const char *arguments = end;
        while (isspace((unsigned char)*arguments)) ++arguments;
        if (strncmp(arguments, "--", 2u) ||
            (arguments[2] && !isspace((unsigned char)arguments[2]))) {
            (void)snag_errorf(error, size, "use /skills use ID -- REQUEST");
            return 1;
        }
        arguments += 2u;
        while (isspace((unsigned char)*arguments)) ++arguments;
        size_t length = (size_t)(end - start);
        char *id = malloc(length + 1u);
        if (!id) return -1;
        memcpy(id, start, length);
        id[length] = '\0';
        json_t *skill = activate(app, id, arguments, true, error, size);
        free(id);
        if (!skill) return 1;
        json_decref(skill);
        return 0;
    }
    size_t code = 0;
    for (size_t i = 0; prompt[i];) {
        if (prompt[i] == '\\' && prompt[i + 1u]) { i += 2u; continue; }
        if (prompt[i] == '`') {
            size_t run = 1u;
            while (prompt[i + run] == '`') ++run;
            if (!code) code = run;
            else if (code == run) code = 0;
            i += run;
            continue;
        }
        if (prompt[i] != '$' || code ||
            (i && !isspace((unsigned char)prompt[i - 1u]) && prompt[i - 1u] != '(')) {
            ++i;
            continue;
        }
        size_t start = ++i;
        while (isalnum((unsigned char)prompt[i]) || prompt[i] == '-' || prompt[i] == '_' ||
            prompt[i] == '@') ++i;
        if (start == i) continue;
        const char *id = prompt_skill(app->session.skills_catalog, prompt + start, i - start);
        if (!id) continue;
        json_t *skill = activate(app, id, "", true, error, size);
        if (!skill) return 1;
        json_decref(skill);
    }
    return 0;
}

static int
unload(struct app_state *app, const char *id, size_t *removed, char *error, size_t size)
{
    json_t *active = json_array();
    const char *matched_id = NULL;
    if (!active) return -1;
    for (size_t i = 0; i < json_array_size(app->session.active_skills); ++i) {
        json_t *entry = json_array_get(app->session.active_skills, i);
        if (id && strcmp(id, snag_json_string(entry, "id")) &&
            strcmp(id, snag_json_string(entry, "name"))) {
            if (json_array_append(active, entry) < 0) {
                json_decref(active);
                return -1;
            }
        } else if (id) {
            const char *entry_id = snag_json_string(entry, "id");
            if (matched_id && strcmp(matched_id, entry_id)) {
                json_decref(active);
                return snag_errorf(error, size, "skill name is ambiguous; use a qualified ID");
            }
            matched_id = entry_id;
        }
    }
    *removed = json_array_size(app->session.active_skills) - json_array_size(active);
    int rc = save_bindings(app, active, error, size);
    json_decref(active);
    if (rc < 0) return rc;
    if (snag_ui_send(&app->ui, (struct snag_ui_command){.kind = SNAG_UI_NOTICE,
        .label = *removed && id ? "Skill unloaded: " : "",
        .text = !*removed ? "No skills unloaded." : id ? id : "Skills unloaded."}) < 0) {
        return snag_errorf(error, size, "skill bindings saved but unload notice failed");
    }
    return 0;
}

int
snag_app_skills_tool(struct app_state *app, const struct snag_response_item *call,
    json_t **result, char *error, size_t size)
{
    const char *action = snag_json_string(call->arguments, "action");
    const char *id = snag_json_string(call->arguments, "id");
    const char *arguments = snag_json_string(call->arguments, "arguments");
    const char *query = snag_json_string(call->arguments, "query");
    if (!action || !snag_json_arg_keys(call->arguments, "action",
        "id arguments query offset limit", error, size)) goto refused;
    if ((!app->session.skills_catalog || !strcmp(action, "refresh")) &&
        snag_app_skills_refresh(app, error, size) < 0) goto refused;
    if (!strcmp(action, "list") || !strcmp(action, "refresh")) {
        uint64_t offset = 0;
        uint64_t limit = 30;
        bool known;
        if (snag_json_optional_u64(call->arguments, "offset", &offset, &known) < 0 ||
            snag_json_optional_u64(call->arguments, "limit", &limit, &known) < 0) goto refused;
        if (!known) limit = 30u;
        *result = snag_skills_list(app->session.skills_catalog, query,
            (size_t)offset, (size_t)limit);
    } else if (!strcmp(action, "show") && id) {
        const json_t *entry = snag_skills_resolve(app->session.skills_catalog, id, error, size);
        if (entry) *result = snag_skill_snapshot(entry, error, size);
    } else if (!strcmp(action, "activate") && id) {
        json_t *binding = activate(app, id, arguments, false, error, size);
        *result = binding ? json_copy(binding) : NULL;
        json_decref(binding);
        /* The current-state projection supplies the pinned instructions once.
         * Keep the terminal receipt useful without duplicating them in history. */
        if (*result) {
            json_object_del(*result, "body");
            json_object_del(*result, "instructions");
        }
    } else if (!strcmp(action, "deactivate") && id && *id) {
        size_t removed;
        if (unload(app, id, &removed, error, size) == 0) {
            *result = json_pack("{s:s,s:s,s:I}", "action", "deactivate", "id", id,
                "removed", (json_int_t)removed);
        }
    } else {
        (void)snag_errorf(error, size,
            "skills requires list, refresh, show, activate or deactivate and a matching id");
    }
    if (*result) {
        if (json_object_set(*result, "skills_generation",
            json_object_get(app->session.skills_catalog, "generation")) < 0) return -1;
        char *text = json_dumps(*result, JSON_COMPACT | JSON_SORT_KEYS);
        json_decref(*result);
        *result = text ? snag_tool_result_terminal(true, text) : NULL;
        free(text);
        return *result ? 0 : -1;
    }
refused:
    *result = snag_tool_result_terminal(false, *error ? error : "invalid skills arguments");
    return *result ? 0 : -1;
}

/* The standalone CLI owns retrieval; model-driven installation uses exec_command
 * and its ordinary yielding. Terminal commands keep pumping input while it runs. */
static int
manage_command(struct app_state *app, const char *action, const char *operand,
    char *error, size_t size)
{
    char *program = snag_program_path(app->program);
    json_t *words = snag_skill_arguments(operand ? operand : "", error, size);
    const char *fixed[] = {program, "skills", "--dotdir", app->store.root_path,
        "--config", app->config->source_path, "--cwd",
        app->session.cwd ? app->session.cwd : ".", action};
    size_t fixed_count = sizeof(fixed) / sizeof(fixed[0]);
    const char **argv = calloc(fixed_count + json_array_size(words) + 1u, sizeof(*argv));
    struct snag_child child;
    snag_child_init(&child);
    struct snag_buf output = {.max = SNAG_MAX_EVENT_LINE};
    int rc = -1;
    if (!program || !words || !argv) goto done;
    for (size_t i = 0; i < fixed_count + json_array_size(words); ++i) {
        argv[i] = i < fixed_count ? fixed[i] :
            json_string_value(json_array_get(words, i - fixed_count));
    }
    if (snag_child_spawn_host(&child, argv) < 0) goto done;
    snag_child_close_stream(&child, 2u);
    bool reading[2] = {true, true};
    bool canceled = false;
    while (true) {
        for (unsigned int stream = 0; stream < 2u; ++stream) {
            if (!reading[stream]) continue;
            unsigned char buffer[65536];
            ssize_t amount = snag_child_read(&child, stream, buffer, sizeof(buffer));
            if (amount > 0 && snag_buf_append(&output, buffer, (size_t)amount) < 0) goto done;
            if (amount == 0 || (amount < 0 && errno != EINTR && errno != EAGAIN &&
                errno != EWOULDBLOCK)) {
                reading[stream] = false;
                snag_child_close_stream(&child, stream);
            }
        }
        int exited = snag_child_exited(&child);
        if (exited < 0) goto done;
        if (exited > 0 && !reading[0] && !reading[1]) {
            if (snag_child_reap(&child) < 0) goto done;
            bool success = child.exit_code == 0;
            if (snag_buf_terminate(&output) < 0) goto done;
            rc = snag_app_report(app, success ? SNAG_UI_HOST : SNAG_UI_ERROR,
                (char *)output.data);
            if (success && rc == 0) rc = snag_app_skills_refresh(app, error, size);
            if (canceled) rc = snag_errorf(error, size, "skills management interrupted");
            break;
        }
        if (snag_app_active_input_pump(app, 25u) != 0 && !canceled) {
            snag_child_signal(&child, SNAG_CHILD_TERMINATE);
            /* The CLI joins its Git child and removes its owned staging tree.
             * Keep draining until that cleanup finishes instead of killing the owner. */
            canceled = true;
        }
    }
done:
    snag_child_free(&child);
    free(argv);
    free(program);
    json_decref(words);
    snag_buf_free(&output);
    return rc;
}

static int
report_loaded_skills(struct app_state *app)
{
    size_t count = json_array_size(app->session.active_skills);
    if (!count) {
        return snag_app_report(app, SNAG_UI_HOST,
            "No skills loaded. Use /skills list to browse available skills.");
    }
    struct snag_buf text = {.max = SIZE_MAX};
    int rc = snag_buf_printf(&text, "Loaded skills (%zu):", count);
    for (size_t i = 0; rc == 0 && i < count; ++i) {
        const json_t *skill = json_array_get(app->session.active_skills, i);
        rc = snag_buf_printf(&text, "\n  %s (%s)",
            snag_json_string(skill, "name"), snag_json_string(skill, "id"));
    }
    if (rc == 0) rc = snag_app_report(app, SNAG_UI_HOST, (char *)text.data);
    snag_buf_free(&text);
    return rc;
}

int
snag_app_skills_command(struct app_state *app, const char *line, bool *handled)
{
    *handled = !strncmp(line, "/skills", 7u) &&
        (!line[7] || isspace((unsigned char)line[7]));
    if (!*handled) return 0;
    const char *action = line + 7u;
    while (isspace((unsigned char)*action)) ++action;
    if (!strncmp(action, "use ", 4u)) { *handled = false; return 0; }
    char error[512] = {0};
    if (!*action) {
        if (snag_app_skills_refresh(app, error, sizeof(error)) < 0)
            return snag_app_report(app, SNAG_UI_ERROR, error);
        return report_loaded_skills(app);
    }
    char *copy = strdup(action);
    if (!copy) return -1;
    char *operand = strchr(copy, ' ');
    if (operand) {
        *operand++ = 0;
        while (isspace((unsigned char)*operand)) ++operand;
    }
    json_t *result = NULL;
    if (snag_string_in(copy,
        "install update remove create validate paths")) {
        int rc = manage_command(app, copy, operand, error, sizeof(error));
        free(copy);
        return rc < 0 ? snag_app_report(app, SNAG_UI_ERROR, error) : rc;
    }
    if (!strcmp(copy, "clear")) {
        size_t removed;
        int rc = unload(app, operand, &removed, error, sizeof(error));
        if (rc < 0) rc = snag_app_report(app, SNAG_UI_ERROR, error);
        free(copy);
        return rc;
    } else if (snag_app_skills_refresh(app, error, sizeof(error)) == 0) {
        if (!strcmp(copy, "list") || !strcmp(copy, "refresh")) {
            result = snag_skills_list(app->session.skills_catalog, operand, 0u, 50u);
        } else if (operand && (!strcmp(copy, "show") || !strcmp(copy, "prompt"))) {
            const json_t *entry = snag_skills_resolve(app->session.skills_catalog,
                operand, error, sizeof(error));
            if (entry && !strcmp(copy, "prompt")) {
                const char *prompt = snag_json_string(json_object_get(
                    json_object_get(entry, "openai"), "interface"), "default_prompt");
                free(copy);
                return prompt ? snag_ui_restore_input(&app->ui, prompt) :
                    snag_app_report(app, SNAG_UI_HOST, "skill has no default prompt");
            }
            if (entry) result = snag_skill_snapshot(entry, error, sizeof(error));
        } else {
            (void)snag_errorf(error, sizeof(error),
                "use /skills [list [QUERY]|show ID|use ID -- REQUEST|"
                "clear [ID]|refresh|prompt ID]");
        }
    }
    struct snag_buf text = {.max = SIZE_MAX};
    int rc;
    if (result) {
        rc = snag_skills_report(&text, copy, result);
        if (rc == 0) rc = snag_buf_terminate(&text);
        if (rc == 0) rc = snag_app_report(app, SNAG_UI_HOST, (char *)text.data);
    } else {
        rc = snag_app_report(app, SNAG_UI_ERROR, *error ? error : "skills command failed");
    }
    snag_buf_free(&text);
    json_decref(result);
    free(copy);
    return rc;
}
