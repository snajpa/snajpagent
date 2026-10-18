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

static int
retire_revoked(struct app_state *app, const json_t *settings, const json_t *catalog,
    char *error, size_t size)
{
    json_t *active = json_array();
    if (!active) return -1;
    int rc = 0;
    for (size_t i = 0; i < json_array_size(app->session.active_skills); ++i) {
        json_t *pin = json_array_get(app->session.active_skills, i);
        const char *id = snag_json_string(pin, "id");
        const char *digest = snag_json_string(pin, "manifest_digest");
        const char *root = snag_json_string(pin, "root");
        json_t *approval = json_object_get(json_object_get(settings, "approvals"), id);
        const char *approved_digest = snag_json_string(approval, "digest");
        const char *approved_root = snag_json_string(approval, "root");
        bool retained = digest && approved_digest && approved_root &&
            !strcmp(digest, approved_digest) && !strcmp(root, approved_root);
        const char *source = snag_json_string(approval, "source");
        json_t *roots = json_object_get(catalog, "roots");
        bool selected = false;
        for (size_t j = 0; source && j < json_array_size(roots); ++j) {
            const json_t *item = json_array_get(roots, j);
            const char *path = snag_json_string(item, "canonical");
            if (!path || !*path) path = snag_json_string(item, "path");
            size_t length = path ? strlen(path) : 0u;
            if (length && !strncmp(source, path, length) &&
                (!source[length] || path[length - 1u] == '/' || source[length] == '/')) {
                selected = true;
            }
        }
        retained = retained && selected;
        const char *arrays[] = {"banned", "disabled"};
        for (size_t j = 0; retained && j < 2u; ++j) {
            json_t *values = json_object_get(settings, arrays[j]);
            for (size_t k = 0; k < json_array_size(values); ++k) {
                const char *value = json_string_value(json_array_get(values, k));
                const char *identity = j == 0u ? snag_json_string(pin, "name") : id;
                if (!strcmp(value, identity)) retained = false;
            }
        }
        if (retained) {
            json_t *manifest = snag_skill_manifest(root, error, size);
            const char *actual = snag_json_string(manifest, "digest");
            retained = actual && !strcmp(actual, digest);
            json_decref(manifest);
        }
        if (retained && json_array_append(active, pin) < 0) { rc = -1; break; }
    }
    json_t *policy = json_pack("{s:O,s:O,s:O,s:O}",
        "approvals", json_object_get(settings, "approvals") ?
            json_object_get(settings, "approvals") : json_null(),
        "banned", json_object_get(settings, "banned") ?
            json_object_get(settings, "banned") : json_null(),
        "disabled", json_object_get(settings, "disabled") ?
            json_object_get(settings, "disabled") : json_null(),
        "roots", json_object_get(settings, "roots") ?
            json_object_get(settings, "roots") : json_null());
    char digest[SNAG_SHA256_HEX_LEN + 1u];
    bool meaningful = *app->session.skills_policy_digest ||
        json_array_size(app->session.active_skills) ||
        json_object_size(json_object_get(settings, "approvals")) ||
        json_array_size(json_object_get(settings, "banned")) ||
        json_array_size(json_object_get(settings, "disabled"));
    if (rc == 0 && meaningful && (!policy ||
        snag_json_digest_bounded(policy, SNAG_MAX_EVENT_LINE, digest, NULL) < 0)) rc = -1;
    bool changed = rc == 0 && meaningful && strcmp(digest, app->session.skills_policy_digest);
    bool revoked = json_array_size(active) < json_array_size(app->session.active_skills);
    if (rc == 0 && (changed || revoked)) {
        uint64_t boundary = changed || revoked ? app->session.next_seq :
            app->session.skills_revoked_seq;
        rc = snag_app_commit_event(app, "skills_updated", json_pack("{s:O,s:s,s:I}",
            "active", active, "policy_digest", meaningful ? digest : "",
            "revoked_seq", (json_int_t)boundary), error, size);
    } else if (rc == 0) rc = save_bindings(app, active, error, size);
    json_decref(policy);
    json_decref(active);
    return rc;
}

int
snag_app_skills_refresh(struct app_state *app, char *error, size_t size)
{
    json_t *settings = snag_skills_settings_load(app->store.root_path, error, size);
    if (!settings) return -1;
    if (app->config->skills_roots &&
        json_object_set(settings, "roots", app->config->skills_roots) < 0) {
        json_decref(settings);
        return -1;
    }
    json_t *catalog = snag_skills_discover(app->session.cwd, app->store.root_path,
        settings, error, size);
    int rc = catalog ? retire_revoked(app, settings, catalog, error, size) : -1;
    json_decref(settings);
    if (rc < 0) { json_decref(catalog); return -1; }
    if (!json_equal(catalog, app->session.skills_catalog)) {
        snag_context_skills_changed(&app->session);
    }
    json_decref(app->session.skills_catalog);
    app->session.skills_catalog = catalog;
    app->ui.view_state_seq = UINT64_MAX;
    return snag_ui_view_state(&app->ui, &app->session);
}

static json_t *
active_bindings(const struct snag_session *session)
{
    json_t *active = json_array();
    if (!active) return NULL;
    for (size_t i = 0; i < json_array_size(session->active_skills); ++i) {
        json_t *entry = json_array_get(session->active_skills, i);
        const char *goal = snag_json_string(entry, "goal_id");
        const char *turn = snag_json_string(entry, "turn_id");
        bool retained = goal && *goal ? snag_goal_unfinished(session->goal_status) &&
            !strcmp(goal, session->goal_id) :
            turn && (!strcmp(turn, session->active_turn_id) || session->active_read_only);
        if (retained && json_array_append(active, entry) < 0) {
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
        json_pack("{s:O,s:s,s:I}", "active", active,
            "policy_digest", app->session.skills_policy_digest,
            "revoked_seq", (json_int_t)app->session.skills_revoked_seq), error, size);
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
    json_t *active = active_bindings(&app->session);
    const char *goal = app->session.goal_status == SNAG_GOAL_ACTIVE &&
        !app->session.active_read_only ? app->session.goal_id : "";
    json_t *dependencies = dependency_state(app, skill);
    json_t *allowed = allowed_tools(app, skill);
    json_t *resources = snag_skill_resources(snag_json_string(skill, "root"),
        app->config->tool_output_bytes, error, size);
    if (!active || !dependencies || !allowed || !resources ||
        json_object_set_new(skill, "turn_id", json_string(app->session.active_turn_id)) < 0 ||
        json_object_set_new(skill, "goal_id", json_string(goal)) < 0 ||
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
    for (size_t i = 0; i < json_array_size(active); ++i) {
        const json_t *previous = json_array_get(active, i);
        if (!strcmp(snag_json_string(previous, "id"), snag_json_string(skill, "id")) &&
            !strcmp(snag_json_string(previous, "digest"), snag_json_string(skill, "digest")) &&
            !strcmp(snag_json_string(previous, "arguments"),
                snag_json_string(skill, "arguments"))) {
            json_t *result = json_incref((json_t *)previous);
            json_decref(active);
            json_decref(skill);
            return result;
        }
    }
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
    if (!app->session.skills_catalog && snag_app_skills_refresh(app, error, size) < 0) return -1;
    if (continuing) return 0;
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
        json_t *active = active_bindings(&app->session);
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
        char *id = strndup(start, (size_t)(end - start));
        if (!id) return -1;
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
    if (snag_app_skills_refresh(app, error, size) < 0) goto refused;
    if (!strcmp(action, "list")) {
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
        *result = activate(app, id, arguments, false, error, size);
    } else {
        (void)snag_errorf(error, size, "skills requires list, show or activate and a matching id");
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
        app->session.cwd ? app->session.cwd : ".", "--json", action,
        app->cli->all_skills ? "--all-skills" : "--json"};
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
    char *copy = strdup(action);
    if (!copy) return -1;
    char *operand = strchr(copy, ' ');
    if (operand) {
        *operand++ = 0;
        while (isspace((unsigned char)*operand)) ++operand;
    }
    json_t *result = NULL;
    if (app->cli->all_skills && (!*copy || !strcmp(copy, "list") || !strcmp(copy, "show"))) {
        int rc = manage_command(app, *copy ? copy : "list", operand, error, sizeof(error));
        free(copy);
        return rc < 0 ? snag_app_report(app, SNAG_UI_ERROR, error) : rc;
    }
    if (snag_string_in(copy,
        "install update remove create validate enable disable ban unban paths review")) {
        int rc = manage_command(app, copy, operand, error, sizeof(error));
        free(copy);
        return rc < 0 ? snag_app_report(app, SNAG_UI_ERROR, error) : rc;
    }
    if (!strcmp(copy, "approve")) {
        json_t *words = snag_skill_arguments(operand ? operand : "", error, sizeof(error));
        const char *id = json_string_value(json_array_get(words, 0u));
        const char *option = json_string_value(json_array_get(words, 1u));
        const char *digest = json_string_value(json_array_get(words, 2u));
        if (json_array_size(words) == 3u && option && !strcmp(option, "--digest") &&
            snag_skills_approve(app->store.root_path, id, digest, error, sizeof(error)) == 0 &&
            snag_app_skills_refresh(app, error, sizeof(error)) == 0) {
            result = json_pack("{s:s,s:s,s:s}", "action", "approve", "id", id, "digest", digest);
        } else if (!*error) {
            (void)snag_errorf(error, sizeof(error),
                "use /skills approve ID --digest HASH after review");
        }
        json_decref(words);
    } else if (!strcmp(copy, "clear")) {
        json_t *active = json_array();
        if (!active) { free(copy); return -1; }
        for (size_t i = 0; i < json_array_size(app->session.active_skills); ++i) {
            json_t *entry = json_array_get(app->session.active_skills, i);
            if (operand && strcmp(operand, snag_json_string(entry, "id")) &&
                strcmp(operand, snag_json_string(entry, "name"))) {
                if (json_array_append(active, entry) < 0) {
                    json_decref(active);
                    free(copy);
                    return -1;
                }
            }
        }
        if (save_bindings(app, active, error, sizeof(error)) == 0) {
            result = json_pack("{s:s,s:O}", "action", "clear", "active", active);
        }
        json_decref(active);
    } else if (snag_app_skills_refresh(app, error, sizeof(error)) == 0) {
        if (!*copy || !strcmp(copy, "list") || !strcmp(copy, "refresh")) {
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
    char *text = result ? json_dumps(result, JSON_INDENT(2) | JSON_SORT_KEYS) : NULL;
    int rc = snag_app_report(app, SNAG_UI_HOST,
        text ? text : *error ? error : "skills command failed");
    free(text);
    json_decref(result);
    free(copy);
    return rc;
}
