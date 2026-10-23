/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"
#include "fs.h"
#include "json.h"
#include "tools.h"
#include "term.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool
resume_enabled(const json_t *job)
{
    const char *hook = snag_json_string(job, "on_resume");
    return json_is_true(json_object_get(job, "enabled")) && hook && *hook;
}

static int
output_open(struct app_state *app, const char *id, unsigned int stream, bool writing)
{
    char name[80];
    snprintf(name, sizeof(name), "running-%s-%u.log", id, stream);
    if (writing) {
        int fd = snag_open_private_append_at(app->session.dir_fd, name, true);
        if (fd >= 0 || errno != EEXIST) return fd;
        return snag_open_private_append_at(app->session.dir_fd, name, false);
    }
    int fd = snag_open_read_security_at(app->session.dir_fd, name, false);
    if (fd < 0) return -1;
    snag_file_info st;
    struct snag_file_privacy privacy;
    if (snag_fstat(fd, &st) < 0 || snag_fd_privacy(fd, &privacy) < 0 ||
        !S_ISREG(st.st_mode) || st.st_nlink != 1u ||
        !privacy.effective_owner || !privacy.private_access) {
        close(fd);
        return snag_errno(EACCES);
    }
    return fd;
}

static int
save_commands(struct app_state *app, json_t *jobs, char *error, size_t size)
{
    return snag_app_commit_event(app, "pinned_commands_updated",
        json_pack("{s:O}", "commands", jobs), error, size);
}

static json_t *
job_state(struct app_state *app, const json_t *job)
{
    const char *id = snag_json_string(job, "id");
    json_t *state = snag_tools_pinned_state(id);
    if (!state) {
        const char *failure = snag_json_string(app->pinned_errors, id);
        state = json_pack("{s:s}", "state", failure ? "start_failed" : "stopped");
        if (failure && json_object_set_new(state, "error", json_string(failure)) < 0) {
            json_decref(state);
            return NULL;
        }
    }
    if (!state || json_object_set(state, "id", json_object_get(job, "id")) < 0 ||
        json_object_set(state, "name", json_object_get(job, "name")) < 0 ||
        json_object_set_new(state, "resume", json_boolean(resume_enabled(job))) < 0) {
        json_decref(state);
        return NULL;
    }
    return state;
}

int
snag_app_running_snapshot(struct app_state *app)
{
    if (!json_array_size(app->session.pinned_commands)) return 0;
    json_t *states = json_array();
    if (!states) return -1;
    for (size_t i = 0u; i < json_array_size(app->session.pinned_commands); ++i) {
        json_t *state = job_state(app, json_array_get(app->session.pinned_commands, i));
        if (!state || json_array_append_new(states, state) < 0) {
            json_decref(states);
            return -1;
        }
    }
    json_decref(app->session.pinned_status);
    app->session.pinned_status = states;
    return 0;
}

static int
start_job(struct app_state *app, const json_t *job, bool resuming,
    const struct snag_credential *credential, char *error, size_t size)
{
    const char *id = snag_json_string(job, "id");
    int fds[2] = {-1, -1};
    for (unsigned int s = 0u; s < 2u; ++s) {
        fds[s] = output_open(app, id, s, true);
        if (fds[s] < 0) goto failed;
    }
    if (snag_tools_pinned_start(id,
            snag_json_string(job, resuming ? "on_resume" : "command"),
            snag_json_string(job, "workdir"), snag_json_string(job, "shell"), fds,
            app->config, credential, error, size) == 0) return 0;
failed:
    for (unsigned int s = 0u; s < 2u; ++s)
        if (fds[s] >= 0) close(fds[s]);
    if (!*error) snag_errorf(error, size, "command start failed: %s", strerror(errno));
    if (!app->pinned_errors) app->pinned_errors = json_object();
    if (app->pinned_errors)
        (void)json_object_set_new(app->pinned_errors, id, json_string(error));
    return -1;
}

int
snag_app_running_resume(struct app_state *app)
{
    for (size_t i = 0u; i < json_array_size(app->session.pinned_commands); ++i) {
        const json_t *job = json_array_get(app->session.pinned_commands, i);
        char error[256] = {0};
        if (!resume_enabled(job)) continue;
        if (start_job(app, job, true, NULL, error, sizeof(error)) < 0 &&
            snag_ui_text(&app->ui, SNAG_UI_WARNING, error) < 0) return -1;
    }
    return snag_app_running_snapshot(app);
}

static const json_t *
find_job(struct app_state *app, const char *key, char *error, size_t size)
{
    const json_t *found = NULL;
    if (!key || !*key) {
        snag_errorf(error, size, "command ID or name is required");
        return NULL;
    }
    size_t length = strlen(key);
    for (size_t i = 0u; i < json_array_size(app->session.pinned_commands); ++i) {
        const json_t *job = json_array_get(app->session.pinned_commands, i);
        const char *id = snag_json_string(job, "id");
        if (strcmp(key, snag_json_string(job, "name")) && strncmp(key, id, length)) continue;
        if (found) {
            snag_errorf(error, size, "ambiguous command ID or name; use the full ID");
            return NULL;
        }
        found = job;
    }
    if (!found) snag_errorf(error, size, "unknown pinned command");
    return found;
}

static int
stop_jobs(struct app_state *app, const char *id, char *error, size_t size)
{
    json_t *jobs = json_array();
    if (!jobs) return -1;
    for (size_t i = 0u; i < json_array_size(app->session.pinned_commands); ++i) {
        const json_t *job = json_array_get(app->session.pinned_commands, i);
        bool selected = !id || !strcmp(id, snag_json_string(job, "id"));
        json_t *next = selected ? json_copy((json_t *)job) : json_incref((json_t *)job);
        if (!next || (selected && json_object_set_new(next, "enabled", json_false()) < 0)) {
            json_decref(next);
            json_decref(jobs);
            return -1;
        }
        if (json_array_append_new(jobs, next) < 0) {
            json_decref(jobs);
            return -1;
        }
    }
    int rc = json_equal(jobs, app->session.pinned_commands) ? 0 :
        save_commands(app, jobs, error, size);
    if (!rc) {
        for (size_t i = 0u; i < json_array_size(jobs); ++i) {
            const char *key = snag_json_string(json_array_get(jobs, i), "id");
            if (!id || !strcmp(id, key)) snag_tools_signal(key);
        }
    }
    json_decref(jobs);
    return rc;
}

static int
append_output(struct app_state *app, const char *id, struct snag_buf *out)
{
    for (unsigned int s = 0u; s < 2u; ++s) {
        int fd = output_open(app, id, s, false);
        if (fd < 0) {
            if (errno == ENOENT) continue;
            return -1;
        }
        snag_file_info st;
        unsigned char bytes[4096];
        int rc = snag_fstat(fd, &st);
        if (!rc) {
            int64_t from = st.st_size > (int64_t)sizeof(bytes) ?
                st.st_size - (int64_t)sizeof(bytes) : 0;
            ssize_t n = snag_pread(fd, bytes, sizeof(bytes), from);
            rc = n < 0 ? -1 : snag_buf_append(out, bytes, (size_t)n);
        }
        close(fd);
        if (rc < 0) return -1;
    }
    return 0;
}

static json_t *
result_value(json_t *value, bool succeeded)
{
    char *text = value ? json_dumps(value, JSON_COMPACT) : NULL;
    json_t *result = text ? snag_tool_result_terminal(succeeded, text) : NULL;
    free(text);
    json_decref(value);
    return result;
}

int
snag_app_running_tool(struct app_state *app, const struct snag_response_item *call,
    const struct snag_credential *credential, json_t **result, char *error, size_t size)
{
    const char *action = NULL, *id = NULL, *name = NULL, *command = NULL;
    const char *hook = NULL, *workdir = NULL, *data = NULL;
    bool eof = false;
    *result = NULL;
    if (!snag_json_arg_keys(call->arguments, "action",
            "id name command on_resume workdir data eof", error, size) ||
        !snag_json_arg_text(call->arguments, "action", 1u, 8u, false, &action, error, size) ||
        !snag_json_arg_text(call->arguments, "id", 1u, 128u, true, &id, error, size) ||
        !snag_json_arg_text(call->arguments, "name", 1u, 128u, true, &name, error, size) ||
        !snag_json_arg_text(call->arguments, "command", 1u, 262144u, true,
            &command, error, size) ||
        !snag_json_arg_text(call->arguments, "on_resume", 0u, 262144u, true,
            &hook, error, size) ||
        !snag_json_arg_text(call->arguments, "workdir", 1u, SNAG_PATH_MAX_BYTES, true,
            &workdir, error, size) ||
        !snag_json_arg_text(call->arguments, "data", 0u, 1048576u, true, &data, error, size) ||
        !snag_json_arg_bool(call->arguments, "eof", false, &eof, error, size)) goto refused;
    bool reading = !strcmp(action, "list") || !strcmp(action, "show");
    if (!reading && app->session.active_read_only) {
        snag_errorf(error, size, "read-only turns can list or inspect commands only");
        goto refused;
    }
    if (!strcmp(action, "list") && !id && !name && !command && !hook && !workdir &&
        !data && !eof) {
        if (snag_app_running_snapshot(app) < 0) return -1;
        *result = result_value(app->session.pinned_status ?
            json_incref(app->session.pinned_status) : json_array(), true);
        return *result ? 0 : -1;
    }
    if (!strcmp(action, "start") && name && command && !id && !data && !eof) {
        char key[SNAG_ID_HEX_LEN + 1u];
        char *directory = workdir ? (workdir[0] == '/' ? strdup(workdir) :
            snag_path_join(app->session.cwd, workdir)) : strdup(app->session.cwd);
        if (!directory || snag_random_id(key) < 0) {
            free(directory);
            return -1;
        }
        json_t *job = json_pack("{s:s,s:s,s:s,s:s,s:s,s:s,s:b}", "id", key,
            "name", name, "command", command, "workdir", directory,
            "shell", app->session.command_shell[0] ?
                app->session.command_shell : app->config->shell,
            "on_resume", hook ? hook : "", "enabled", true);
        free(directory);
        json_t *jobs = app->session.pinned_commands ?
            json_copy(app->session.pinned_commands) : json_array();
        if (!job || !jobs || json_array_append(jobs, job) < 0) {
            json_decref(job);
            json_decref(jobs);
            return -1;
        }
        if (!snag_pinned_commands_valid(jobs)) {
            json_decref(jobs);
            json_decref(job);
            snag_errorf(error, size,
                "invalid command definition; names must not contain control characters");
            goto refused;
        }
        int rc = save_commands(app, jobs, error, size);
        json_decref(jobs);
        if (rc < 0) {
            json_decref(job);
            return -1;
        }
        rc = start_job(app, job, false, credential, error, size);
        json_t *state = job_state(app, job);
        json_decref(job);
        *result = result_value(state, rc == 0);
        return *result ? 0 : -1;
    }
    if (name || command || hook || workdir || (!strcmp(action, "show") && (data || eof)))
        goto usage;
    if (!strcmp(action, "stop") && id && !strcmp(id, "all") && !data && !eof) {
        if (stop_jobs(app, NULL, error, size) < 0) return -1;
        *result = snag_tool_result_terminal(true,
            "Pinned commands stopping; resume hooks disabled.");
        return *result ? 0 : -1;
    }
    const json_t *job = find_job(app, id, error, size);
    if (!job) goto refused;
    id = snag_json_string(job, "id");
    if (!strcmp(action, "show")) {
        if (snag_tools_pinned_flush(id, error, size) < 0) return -1;
        json_t *state = job_state(app, job);
        struct snag_buf output = {.max = 8192u};
        struct snag_buf encoded = {.max = 16384u};
        if (!state || append_output(app, id, &output) < 0 ||
            json_object_set(state, "definition", (json_t *)job) < 0 ||
            snag_base64_append(&encoded, output.data, output.len) < 0 ||
            json_object_set_new(state, "output_base64",
                json_stringn(encoded.len ? (char *)encoded.data : "", encoded.len)) < 0) {
            json_decref(state);
            snag_buf_free(&output);
            snag_buf_free(&encoded);
            return -1;
        }
        snag_buf_free(&output);
        snag_buf_free(&encoded);
        *result = result_value(state, true);
    } else if (!strcmp(action, "write")) {
        if (snag_tools_pinned_input(id, data, eof, error, size) < 0) goto refused;
        *result = snag_tool_result_terminal(true, "Input queued; acceptance is not delivery.");
    } else if (!strcmp(action, "stop") && !data && !eof) {
        char key[SNAG_ID_HEX_LEN + 1u];
        memcpy(key, id, sizeof(key));
        if (stop_jobs(app, key, error, size) < 0) return -1;
        *result = snag_tool_result_terminal(true, "Command stopping; resume hook disabled.");
    } else {
usage:
        snag_errorf(error, size, "use start(name,command[,on_resume,workdir]), list, "
            "show(id), write(id[,data,eof]), or stop(id|all)");
        goto refused;
    }
    return *result ? 0 : -1;
refused:
    *result = snag_tool_result_not_run("invalid_arguments");
    if (*result && json_object_set_new(*result, "model_text", json_string(error)) < 0) return -1;
    return *result ? 0 : -1;
}

static int
running_report(struct app_state *app, struct snag_buf *text, bool full)
{
    size_t shown = 0u, total = 0u;
    size_t budget = full ? SIZE_MAX : 8u;
    for (size_t i = 0u; i < app->session.pending_call_count; ++i) {
        const struct snag_pending_call *call = &app->session.pending_calls[i];
        if (call->finished) continue;
        ++total;
        if (shown >= budget) continue;
        const char *id = call->process_handle[0] ? call->process_handle : call->call_id;
        if (snag_buf_printf(text, "%.8s  tool  %-12s %s\n", id,
                call->started ? "running" : "pending", call->tool_name) < 0) return -1;
        ++shown;
    }
    for (size_t i = 0u; i < app->session.process_count; ++i) {
        const struct snag_process_state *process = &app->session.processes[i];
        bool listed = false;
        for (size_t j = 0u; j < app->session.pending_call_count; ++j) {
            const struct snag_pending_call *call = &app->session.pending_calls[j];
            listed |= !call->finished && !strcmp(call->process_handle, process->handle);
        }
        if (listed) continue;
        ++total;
        if (shown >= budget) continue;
        char preview[49];
        snprintf(preview, sizeof(preview), "%.48s", process->command);
        for (size_t n = 0u; preview[n]; ++n)
            if ((unsigned char)preview[n] < 32u || preview[n] == 127) preview[n] = ' ';
        if (snag_buf_printf(text, "%.8s  tool  %-12s %s\n", process->handle,
                process->ready ? "ready" : "running", preview) < 0) return -1;
        ++shown;
    }
    for (size_t i = 0u; i < json_array_size(app->session.pinned_commands); ++i) {
        const json_t *job = json_array_get(app->session.pinned_commands, i);
        ++total;
        if (shown >= budget) continue;
        json_t *state = job_state(app, job);
        if (!state) return -1;
        int rc = snag_buf_printf(text, "%.8s  pin   %-12s %.32s  resume:%s\n",
            snag_json_string(job, "id"), snag_json_string(state, "state"),
            snag_json_string(job, "name"), resume_enabled(job) ? "yes" : "no");
        json_decref(state);
        if (rc < 0) return -1;
        ++shown;
    }
    if (!total) return snag_buf_printf(text, "No running commands.\n");
    return total > shown ?
        snag_buf_printf(text, "%zu more; /r list shows all.\n", total - shown) : 0;
}

int
snag_app_running_command(struct app_state *app, const char *line, bool *handled)
{
    *handled = !strncmp(line, "/r", 2u) && (!line[2] || line[2] == ' ');
    if (!*handled) return 0;
    const char *argument = line + 2u;
    while (*argument == ' ') ++argument;
    char error[256] = {0};
    struct snag_buf report = {.max = SNAG_MAX_EVENT_LINE};
    int rc = 0;
    if (!*argument || !strcmp(argument, "list")) {
        rc = running_report(app, &report, *argument != '\0');
    } else if (!strncmp(argument, "k ", 2u) || !strncmp(argument, "kill ", 5u)) {
        const char *target = argument + (argument[1] == ' ' ? 2u : 5u);
        while (*target == ' ') ++target;
        bool all = !strcmp(target, "-a") || !strcmp(target, "--all");
        const json_t *job = NULL;
        char key[SNAG_ID_HEX_LEN + 1u] = {0};
        size_t matches = 0u;
        if (!all && *target) {
            for (size_t i = 0u; i < json_array_size(app->session.pinned_commands); ++i) {
                const json_t *candidate = json_array_get(app->session.pinned_commands, i);
                const char *id = snag_json_string(candidate, "id");
                if (!strcmp(target, snag_json_string(candidate, "name")) ||
                    !strncmp(id, target, strlen(target))) {
                    job = candidate;
                    memcpy(key, id, sizeof(key));
                    ++matches;
                }
            }
            for (size_t i = 0u; i < app->session.process_count; ++i) {
                const char *handle = app->session.processes[i].handle;
                if (!strncmp(handle, target, strlen(target))) {
                    memcpy(key, handle, sizeof(key));
                    ++matches;
                }
            }
        }
        if (!all && matches != 1u)
            rc = snag_errorf(error, sizeof(error),
                matches ? "ambiguous command ID or name" : "unknown stoppable command");
        if (!rc && (all || job))
            rc = stop_jobs(app, all ? NULL : key, error, sizeof(error));
        if (!rc) {
            if (all) {
                for (size_t i = 0u; i < app->session.process_count; ++i)
                    snag_tools_signal(app->session.processes[i].handle);
            } else if (!job) {
                snag_tools_signal(key);
            }
            snag_buf_free(&report);
            return snag_ui_text(&app->ui, SNAG_UI_NOTICE,
                all ? "Commands stopping; pinned resume hooks disabled." : "Command stopping.");
        }
    } else {
        const json_t *job = find_job(app, argument, error, sizeof(error));
        if (!job) rc = -1;
        else {
            json_t *state = job_state(app, job);
            if (!state) rc = -1;
            else rc = snag_buf_printf(&report,
                "%s  %s  %s\nPTY; resume:%s\nDirectory: %s\nCommand: %s\n",
                snag_json_string(job, "id"), snag_json_string(state, "state"),
                snag_json_string(job, "name"), resume_enabled(job) ? "yes" : "no",
                snag_json_string(job, "workdir"), snag_json_string(job, "command"));
            if (!rc && snag_ui_verbosity(&app->ui) && resume_enabled(job))
                rc = snag_buf_printf(&report, "On resume: %s\n",
                    snag_json_string(job, "on_resume"));
            if (!rc) rc = snag_buf_printf(&report, "Output tail (up to 4 KiB per stream):\n");
            if (!rc) rc = append_output(app, snag_json_string(job, "id"), &report);
            json_decref(state);
        }
    }
    struct snag_buf safe = {.max = SNAG_MAX_EVENT_LINE};
    if (!rc) rc = snag_term_append_safe(&safe, (char *)report.data, report.len);
    if (!rc) rc = snag_buf_terminate(&safe);
    if (!rc) rc = snag_app_report(app, SNAG_UI_HOST, (char *)safe.data);
    snag_buf_free(&safe);
    snag_buf_free(&report);
    return rc < 0 ? snag_ui_text(&app->ui, SNAG_UI_ERROR,
        *error ? error : "could not inspect running commands") : 0;
}
