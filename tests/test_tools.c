/* SPDX-License-Identifier: GPL-2.0-only */
#include "base.h"
#include "config.h"
#include "credential.h"
#include "json.h"
#include "tools.h"
#include "turn.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include "snj_jansson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PATCH_MODEL_MAX_FOR_TEST (512u * 1024u)

static json_t *
call_args_yield(const char *command, const char *workdir, int timeout_ms,
                int yield_ms, const char *stdin_text)
{
    json_t *args = json_object();
    assert(args);
    assert(snj_json_set_new(args, "command", json_string(command)) == 0);
    assert(snj_json_set_new(args, "workdir", json_string(workdir)) == 0);
    assert(snj_json_set_new(args, "timeout_ms", json_integer(timeout_ms)) == 0);
    assert(snj_json_set_new(args, "yield_ms", json_integer(yield_ms)) == 0);
    assert(snj_json_set_new(args, "stdin",
                            stdin_text ? json_string(stdin_text) : json_null()) == 0);
    return args;
}

static json_t *
call_args(const char *command, const char *workdir, int timeout_ms,
          const char *stdin_text)
{
    return call_args_yield(command, workdir, timeout_ms, 0, stdin_text);
}

static void
make_call_with_pty(struct snj_response_graph *graph, const char *command,
                   const char *workdir, int timeout_ms, const char *stdin_text,
                   bool pty)
{
    json_t *args = call_args(command, workdir, timeout_ms, stdin_text);
    assert(args != NULL);
    assert(snj_json_set_new(args, "pty", json_boolean(pty)) == 0);
    snj_response_graph_init(graph);
    assert(snj_response_graph_set_provider_id(graph, "resp_tool_test") == 0);
    assert(snj_response_graph_add_call(graph, "item_tool_test",
                                       "call_tool_test", "exec_command",
                                       args) == 0);
}

static void
make_call(struct snj_response_graph *graph, const char *command,
          const char *workdir, int timeout_ms, const char *stdin_text)
{
    make_call_with_pty(graph, command, workdir, timeout_ms, stdin_text, false);
}

static json_t *
run_command_full(const char *command, int timeout_ms, const char *secret,
                 const char *stdin_text, snj_tool_pump_fn pump,
                 void *pump_opaque)
{
    char cwd[4096];
    struct snj_config config;
    struct snj_credential credential;
    struct snj_response_graph graph;
    json_t *result = NULL;
    char error[256];

    assert(getcwd(cwd, sizeof(cwd)) != NULL);
    snj_config_init(&config);
    assert(config.shell != NULL);
    config.default_timeout_ms = 1000;
    config.max_timeout_ms = 5000;
    snj_credential_clear(&credential);
    if (secret) {
        credential.len = strlen(secret);
        assert(credential.len <= SNJ_CREDENTIAL_MAX);
        memcpy(credential.value, secret, credential.len + 1u);
    }
    make_call(&graph, command, cwd, timeout_ms, stdin_text);
    error[0] = '\0';
    {
        int rc = snj_tools_run(&graph.items[0], &config, &credential, cwd,
                               pump, pump_opaque, &result, error, sizeof(error));
        if (rc != 0)
            fprintf(stderr, "tool error: %s errno=%d\n", error, errno);
        assert(rc == 0);
    }
    assert(result != NULL);
    assert(snj_tool_result_valid(result) == 0);
    snj_response_graph_free(&graph);
    snj_config_free(&config);
    return result;
}

static json_t *
run_command_with_credential(const char *command, int timeout_ms,
                            const char *secret)
{
    return run_command_full(command, timeout_ms, secret, NULL, NULL, NULL);
}

static json_t *
run_command(const char *command, int timeout_ms)
{
    return run_command_with_credential(command, timeout_ms, NULL);
}

static json_t *
run_pty_command(const char *command, int timeout_ms)
{
    char cwd[4096];
    struct snj_config config;
    struct snj_credential credential;
    struct snj_response_graph graph;
    json_t *result = NULL;
    char error[256];

    assert(getcwd(cwd, sizeof(cwd)) != NULL);
    snj_config_init(&config);
    config.default_timeout_ms = 1000;
    config.max_timeout_ms = 5000;
    snj_credential_clear(&credential);
    make_call_with_pty(&graph, command, cwd, timeout_ms, NULL, true);
    error[0] = '\0';
    {
        int rc = snj_tools_run(&graph.items[0], &config, &credential, cwd,
                               NULL, NULL, &result, error, sizeof(error));
        if (rc != 0)
            fprintf(stderr, "pty tool error: %s errno=%d\n", error, errno);
        assert(rc == 0);
    }
    assert(result != NULL);
    assert(snj_tool_result_valid(result) == 0);
    snj_response_graph_free(&graph);
    snj_config_free(&config);
    return result;
}

static json_t *
run_tool_with_args(const char *name, json_t *args)
{
    char cwd[4096];
    struct snj_config config;
    struct snj_credential credential;
    struct snj_response_graph graph;
    json_t *result = NULL;
    char error[256];
    int rc;

    assert(getcwd(cwd, sizeof(cwd)) != NULL);
    snj_config_init(&config);
    config.default_timeout_ms = 1000;
    config.default_yield_ms = 1000;
    config.max_timeout_ms = 5000;
    snj_credential_clear(&credential);
    snj_response_graph_init(&graph);
    assert(snj_response_graph_set_provider_id(&graph, "resp_managed_test") == 0);
    assert(snj_response_graph_add_call(&graph, "item_managed_test",
                                       "call_managed_test", name, args) == 0);
    error[0] = '\0';
    rc = snj_tools_run(&graph.items[0], &config, &credential, cwd,
                       NULL, NULL, &result, error, sizeof(error));
    if (rc != 0)
        fprintf(stderr, "tool error: %s errno=%d\n", error, errno);
    assert(rc == 0);
    assert(result != NULL);
    assert(snj_tool_result_valid(result) == 0);
    snj_response_graph_free(&graph);
    snj_config_free(&config);
    return result;
}

static json_t *
run_managed_exec_with_pty(const char *command, int timeout_ms, int yield_ms,
                         bool pty)
{
    char cwd[4096];
    json_t *args;

    assert(getcwd(cwd, sizeof(cwd)) != NULL);
    args = call_args_yield(command, cwd, timeout_ms, yield_ms, NULL);
    assert(snj_json_set_new(args, "pty", json_boolean(pty)) == 0);
    return run_tool_with_args("exec_command", args);
}

static json_t *
run_managed_exec(const char *command, int timeout_ms, int yield_ms)
{
    return run_managed_exec_with_pty(command, timeout_ms, yield_ms, false);
}

static json_t *
run_write_stdin_call(const char *handle, const char *data, bool eof,
                     int yield_ms)
{
    json_t *args = json_object();
    assert(args != NULL);
    assert(snj_json_set_new(args, "handle", json_string(handle)) == 0);
    assert(snj_json_set_new(args, "data", json_string(data)) == 0);
    assert(snj_json_set_new(args, "eof", eof ? json_true() : json_false()) == 0);
    assert(snj_json_set_new(args, "yield_ms", json_integer(yield_ms)) == 0);
    return run_tool_with_args("write_stdin", args);
}

static void
write_text_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    assert(fwrite(text, 1u, strlen(text), f) == strlen(text));
    assert(fclose(f) == 0);
}

static char *
read_text_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long len;
    char *out;

    assert(f != NULL);
    assert(fseek(f, 0L, SEEK_END) == 0);
    len = ftell(f);
    assert(len >= 0);
    assert(fseek(f, 0L, SEEK_SET) == 0);
    out = malloc((size_t)len + 1u);
    assert(out != NULL);
    assert(fread(out, 1u, (size_t)len, f) == (size_t)len);
    out[len] = '\0';
    assert(fclose(f) == 0);
    return out;
}

static void
join_path(char *out, size_t out_size, const char *dir, const char *name)
{
    int n = snprintf(out, out_size, "%s/%s", dir, name);
    assert(n > 0 && (size_t)n < out_size);
}

static char *
make_temp_workspace(void)
{
    char tmpl[] = "/tmp/snajpagent-patch-test-XXXXXX";
    char *dir = mkdtemp(tmpl);
    assert(dir != NULL);
    return strdup(dir);
}

static void
remove_file_in_dir(const char *dir, const char *name)
{
    char path[4096];
    join_path(path, sizeof(path), dir, name);
    if (unlink(path) < 0)
        assert(errno == ENOENT);
}

static void
sleep_ms(unsigned int ms)
{
    struct timespec remaining;

    remaining.tv_sec = ms / 1000u;
    remaining.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&remaining, &remaining) < 0 && errno == EINTR)
        ;
}

static json_t *
run_apply_patch(const char *workdir, const char *patch)
{
    struct snj_config config;
    struct snj_credential credential;
    struct snj_response_graph graph;
    json_t *args = json_object();
    json_t *result = NULL;
    char error[256];
    int rc;

    assert(args != NULL);
    assert(snj_json_set_new(args, "patch", json_string(patch)) == 0);
    assert(snj_json_set_new(args, "workdir", json_string(workdir)) == 0);
    snj_config_init(&config);
    snj_credential_clear(&credential);
    snj_response_graph_init(&graph);
    assert(snj_response_graph_set_provider_id(&graph, "resp_patch_test") == 0);
    assert(snj_response_graph_add_call(&graph, "item_patch_test",
                                       "call_patch_test", "apply_patch",
                                       args) == 0);
    error[0] = '\0';
    rc = snj_tools_run(&graph.items[0], &config, &credential, workdir,
                       NULL, NULL, &result, error, sizeof(error));
    if (rc != 0)
        fprintf(stderr, "patch tool error: %s errno=%d\n", error, errno);
    assert(rc == 0);
    assert(result != NULL);
    assert(snj_tool_result_valid(result) == 0);
    snj_response_graph_free(&graph);
    snj_config_free(&config);
    return result;
}

static json_int_t
json_int_member(const json_t *object, const char *key)
{
    json_t *value = json_object_get(object, key);
    assert(json_is_integer(value));
    return json_integer_value(value);
}

static int
delay_once_pump(void *opaque, unsigned int timeout_ms)
{
    bool *delayed = opaque;
    struct timespec remaining = {0, 100000000L};

    (void)timeout_ms;
    if (*delayed)
        return 0;
    while (nanosleep(&remaining, &remaining) < 0 && errno == EINTR)
        ;
    *delayed = true;
    return 0;
}

static void
test_success_and_streams(void)
{
    json_t *result = run_command("printf out; printf err >&2", 1000);
    json_t *out = json_object_get(result, "stdout");
    json_t *err = json_object_get(result, "stderr");
    assert(strcmp(snj_json_string(result, "status"), "succeeded") == 0);
    assert(strcmp(snj_json_string(out, "retained"), "out") == 0);
    assert(strcmp(snj_json_string(err, "retained"), "err") == 0);
    json_decref(result);
}

static void
test_failure_status(void)
{
    json_t *result = run_command("exit 7", 1000);
    json_t *exit_code = json_object_get(result, "exit_code");
    assert(strcmp(snj_json_string(result, "status"), "failed") == 0);
    assert(json_is_integer(exit_code));
    assert(json_integer_value(exit_code) == 7);
    json_decref(result);
}

static void
test_timeout(void)
{
    json_t *result = run_command("sleep 2", 50);
    assert(strcmp(snj_json_string(result, "status"), "timed_out") == 0);
    json_decref(result);
}

static void
test_large_stdout_uses_blocking_child_fd(void)
{
    json_t *result = run_command(
        "perl -e 'binmode STDOUT; print q{x} x (1024 * 1024) or exit 23'",
        5000);
    json_t *out = json_object_get(result, "stdout");

    assert(strcmp(snj_json_string(result, "status"), "succeeded") == 0);
    assert(json_int_member(out, "original_bytes") == 1024 * 1024);
    assert(json_int_member(out, "retained_bytes") > 0);
    assert(json_int_member(out, "discarded_bytes") > 0);
    json_decref(result);
}

static void
test_stdin_uses_blocking_child_fd(void)
{
    bool delayed = false;
    json_t *result = run_command_full(
        "cat",
        1000, NULL, "hello", delay_once_pump, &delayed);

    assert(delayed);
    assert(strcmp(snj_json_string(result, "status"), "succeeded") == 0);
    assert(strcmp(snj_json_string(json_object_get(result, "stdout"),
                                  "retained"), "hello") == 0);
    json_decref(result);
}


static void
test_pty_merges_stdout_and_stderr(void)
{
    json_t *result = run_pty_command("printf out; printf err >&2", 1000);
    const char *merged = snj_json_string(json_object_get(result, "stdout"),
                                         "retained");

    assert(strcmp(snj_json_string(result, "status"), "succeeded") == 0);
    assert(strstr(merged, "out") != NULL);
    assert(strstr(merged, "err") != NULL);
    assert(json_int_member(json_object_get(result, "stderr"),
                           "original_bytes") == 0);
    json_decref(result);
}


static void
test_managed_pty_write_stdin_completes(void)
{
    json_t *result = run_managed_exec_with_pty(
        "printf 'ready\\n'; IFS= read -r line; printf 'pty:%s\\n' \"$line\"",
        5000, 100, true);
    const char *handle;
    json_t *next;
    const char *merged;

    assert(strcmp(snj_json_string(result, "status"), "running") == 0);
    assert(json_int_member(json_object_get(result, "stderr"),
                           "original_bytes") == 0);
    handle = snj_json_string(result, "handle");
    assert(handle != NULL && snj_hex_is_lower(handle, SNJ_ID_HEX_LEN));
    next = run_write_stdin_call(handle, "hello\r", true, 5000);
    assert(strcmp(snj_json_string(next, "status"), "succeeded") == 0);
    merged = snj_json_string(json_object_get(next, "stdout"), "retained");
    assert(strstr(merged, "hello") != NULL);
    assert(strstr(merged, "pty:hello") != NULL);
    assert(json_int_member(json_object_get(next, "stderr"),
                           "original_bytes") == 0);
    json_decref(next);
    json_decref(result);
}


static void
test_managed_process_write_stdin_completes(void)
{
    json_t *result = run_managed_exec(
        "printf 'ready\\n'; IFS= read -r line; printf 'got:%s\\n' \"$line\"",
        5000, 100);
    const char *handle;
    json_t *next;

    assert(strcmp(snj_json_string(result, "status"), "running") == 0);
    handle = snj_json_string(result, "handle");
    assert(handle != NULL && snj_hex_is_lower(handle, SNJ_ID_HEX_LEN));
    next = run_write_stdin_call(handle, "hello\n", true, 5000);
    assert(strcmp(snj_json_string(next, "status"), "succeeded") == 0);
    assert(strstr(snj_json_string(json_object_get(next, "stdout"),
                                  "retained"), "got:hello") != NULL);
    json_decref(next);
    json_decref(result);
}

static void
test_managed_process_accepts_repeated_write_stdin(void)
{
    json_t *result = run_managed_exec(
        "printf 'ready\\n'; IFS= read -r a; printf 'first:%s\\n' \"$a\"; IFS= read -r b; printf 'second:%s\\n' \"$b\"",
        5000, 100);
    const char *handle;
    json_t *next;
    json_t *done;

    assert(strcmp(snj_json_string(result, "status"), "running") == 0);
    handle = snj_json_string(result, "handle");
    assert(handle != NULL && snj_hex_is_lower(handle, SNJ_ID_HEX_LEN));
    next = run_write_stdin_call(handle, "one\n", false, 50);
    assert(strcmp(snj_json_string(next, "status"), "running") == 0);
    done = run_write_stdin_call(handle, "two\n", true, 5000);
    assert(strcmp(snj_json_string(done, "status"), "succeeded") == 0);
    assert(strstr(snj_json_string(json_object_get(done, "stdout"),
                                  "retained"), "first:one") != NULL);
    assert(strstr(snj_json_string(json_object_get(done, "stdout"),
                                  "retained"), "second:two") != NULL);
    json_decref(done);
    json_decref(next);
    json_decref(result);
}

static void
test_managed_process_poll_consumes_exit(void)
{
    json_t *result = run_managed_exec(
        "printf 'start\\n'; sleep 0.05; printf 'done\\n'",
        5000, 10);
    const char *handle;
    json_t *next;

    assert(strcmp(snj_json_string(result, "status"), "running") == 0);
    handle = snj_json_string(result, "handle");
    assert(handle != NULL);
    {
        struct timespec remaining = {0, 500000000L};
        while (nanosleep(&remaining, &remaining) < 0 && errno == EINTR)
            ;
    }
    next = run_write_stdin_call(handle, "", false, 0);
    assert(strcmp(snj_json_string(next, "status"), "succeeded") == 0);
    assert(strstr(snj_json_string(json_object_get(next, "stdout"),
                                  "retained"), "done") != NULL);
    json_decref(next);
    json_decref(result);
}

static void
test_write_stdin_rejects_unknown_handle(void)
{
    json_t *result = run_write_stdin_call("00000000000000000000000000000000",
                                          "x", false, 0);

    assert(strcmp(snj_json_string(result, "status"), "failed") == 0);
    json_decref(result);
}



static void
test_managed_process_close_returns_terminal_result(void)
{
    json_t *result = run_managed_exec(
        "printf 'ready\n'; sleep 5",
        5000, 100);
    const char *handle;
    json_t *closed = NULL;
    const char *status;
    char error[256];

    assert(strcmp(snj_json_string(result, "status"), "running") == 0);
    handle = snj_json_string(result, "handle");
    assert(handle != NULL && snj_hex_is_lower(handle, SNJ_ID_HEX_LEN));
    error[0] = '\0';
    assert(snj_tools_close_managed(handle, false, NULL, NULL,
                                   &closed, error, sizeof(error)) == 0);
    assert(closed != NULL);
    assert(snj_tool_result_valid(closed) == 0);
    status = snj_json_string(closed, "status");
    assert(strcmp(status, "running") != 0);
    assert(json_is_null(json_object_get(closed, "handle")));
    json_decref(closed);
    json_decref(result);
}

static void
test_timeout_kills_process_family(void)
{
    char *dir = make_temp_workspace();
    char marker[4096];
    char command[8192];
    json_t *result;

    join_path(marker, sizeof(marker), dir, "leaked.txt");
    assert(snprintf(command, sizeof(command),
                    "(sleep 0.25; printf leaked > '%s') & wait",
                    marker) > 0);
    result = run_command(command, 50);
    assert(strcmp(snj_json_string(result, "status"), "timed_out") == 0);
    json_decref(result);
    sleep_ms(500);
    assert(access(marker, F_OK) < 0 && errno == ENOENT);
    assert(rmdir(dir) == 0);
    free(dir);
}

static void
test_managed_close_kills_process_family(void)
{
    char *dir = make_temp_workspace();
    char marker[4096];
    char command[8192];
    json_t *result;
    json_t *closed = NULL;
    const char *handle;
    char error[256];

    join_path(marker, sizeof(marker), dir, "managed-leaked.txt");
    assert(snprintf(command, sizeof(command),
                    "(sleep 0.25; printf leaked > '%s') & wait",
                    marker) > 0);
    result = run_managed_exec(command, 5000, 50);
    assert(strcmp(snj_json_string(result, "status"), "running") == 0);
    handle = snj_json_string(result, "handle");
    assert(handle != NULL && snj_hex_is_lower(handle, SNJ_ID_HEX_LEN));
    error[0] = '\0';
    assert(snj_tools_close_managed(handle, false, NULL, NULL,
                                   &closed, error, sizeof(error)) == 0);
    assert(closed != NULL);
    assert(snj_tool_result_valid(closed) == 0);
    sleep_ms(500);
    assert(access(marker, F_OK) < 0 && errno == ENOENT);
    json_decref(closed);
    json_decref(result);
    assert(rmdir(dir) == 0);
    free(dir);
}

static void
test_provider_secret_redacted_from_output(void)
{
    json_t *result = run_command_with_credential("printf secret-value-for-test",
                                                 1000,
                                                 "secret-value-for-test");
    const char *retained = snj_json_string(json_object_get(result, "stdout"),
                                           "retained");
    assert(strstr(retained, "secret-value-for-test") == NULL);
    assert(strstr(retained, "<redacted:secret>") != NULL);
    json_decref(result);
}


static void
test_provider_secret_redacted_across_read_boundary(void)
{
    json_t *result = run_command_with_credential(
        "printf '%8190ssecret-value-for-test' ''",
        1000,
        "secret-value-for-test");
    const char *retained = snj_json_string(json_object_get(result, "stdout"),
                                           "retained");
    assert(strstr(retained, "secret-value-for-test") == NULL);
    assert(strstr(retained, "<redacted:secret>") != NULL);
    json_decref(result);
}

static void
test_provider_secret_removed_from_environment(void)
{
    json_t *result;
    setenv("OPENAI_API_KEY", "secret-value-for-test", 1);
    result = run_command("printf ${OPENAI_API_KEY-unset}", 1000);
    assert(strcmp(snj_json_string(json_object_get(result, "stdout"),
                                  "retained"), "unset") == 0);
    unsetenv("OPENAI_API_KEY");
    json_decref(result);
}

static void
test_apply_patch_add_update_delete(void)
{
    char *dir = make_temp_workspace();
    char path[4096];
    char *text;
    json_t *result;
    const char patch[] =
        "*** Begin Patch\n"
        "*** Add File: new.txt\n"
        "+alpha\n"
        "+beta\n"
        "*** Update File: a.txt\n"
        "@@\n"
        " one\n"
        "-two\n"
        "+TWO\n"
        "*** Delete File: old.txt\n"
        "*** End Patch\n";

    join_path(path, sizeof(path), dir, "a.txt");
    write_text_file(path, "one\ntwo\n");
    join_path(path, sizeof(path), dir, "old.txt");
    write_text_file(path, "bye\n");
    result = run_apply_patch(dir, patch);
    assert(strcmp(snj_json_string(result, "status"), "succeeded") == 0);
    {
        const char *model_text = snj_json_string(result, "model_text");
        assert(strstr(model_text, "Diff preview (bounded") != NULL);
        assert(strstr(model_text, "*** Update File: a.txt") != NULL);
        assert(strstr(model_text, "-two") != NULL);
        assert(strstr(model_text, "+TWO") != NULL);
        assert(strstr(model_text, "*** Delete File: old.txt") != NULL);
    }
    json_decref(result);
    join_path(path, sizeof(path), dir, "a.txt");
    text = read_text_file(path);
    assert(strcmp(text, "one\nTWO\n") == 0);
    free(text);
    join_path(path, sizeof(path), dir, "new.txt");
    text = read_text_file(path);
    assert(strcmp(text, "alpha\nbeta\n") == 0);
    free(text);
    join_path(path, sizeof(path), dir, "old.txt");
    assert(access(path, F_OK) < 0 && errno == ENOENT);
    remove_file_in_dir(dir, "a.txt");
    remove_file_in_dir(dir, "new.txt");
    assert(rmdir(dir) == 0);
    free(dir);
}

static void
test_apply_patch_rejects_ambiguous_match(void)
{
    char *dir = make_temp_workspace();
    char path[4096];
    char *text;
    json_t *result;
    const char patch[] =
        "*** Begin Patch\n"
        "*** Update File: dup.txt\n"
        "@@\n"
        "-x\n"
        "+y\n"
        "*** End Patch\n";

    join_path(path, sizeof(path), dir, "dup.txt");
    write_text_file(path, "x\nx\n");
    result = run_apply_patch(dir, patch);
    assert(strcmp(snj_json_string(result, "status"), "patch_rejected") == 0);
    json_decref(result);
    text = read_text_file(path);
    assert(strcmp(text, "x\nx\n") == 0);
    free(text);
    remove_file_in_dir(dir, "dup.txt");
    assert(rmdir(dir) == 0);
    free(dir);
}

static void
test_apply_patch_rejects_path_escape(void)
{
    char *dir = make_temp_workspace();
    json_t *result;
    const char patch[] =
        "*** Begin Patch\n"
        "*** Add File: ../evil.txt\n"
        "+nope\n"
        "*** End Patch\n";

    result = run_apply_patch(dir, patch);
    assert(strcmp(snj_json_string(result, "status"), "patch_rejected") == 0);
    json_decref(result);
    assert(rmdir(dir) == 0);
    free(dir);
}

static void
test_apply_patch_rejects_symlink_target(void)
{
    char *dir = make_temp_workspace();
    char path[4096];
    char linkpath[4096];
    char *text;
    json_t *result;
    const char patch[] =
        "*** Begin Patch\n"
        "*** Update File: link.txt\n"
        "@@\n"
        "-real\n"
        "+changed\n"
        "*** End Patch\n";

    join_path(path, sizeof(path), dir, "real.txt");
    write_text_file(path, "real\n");
    join_path(linkpath, sizeof(linkpath), dir, "link.txt");
    assert(symlink("real.txt", linkpath) == 0);
    result = run_apply_patch(dir, patch);
    assert(strcmp(snj_json_string(result, "status"), "patch_rejected") == 0);
    json_decref(result);
    text = read_text_file(path);
    assert(strcmp(text, "real\n") == 0);
    free(text);
    remove_file_in_dir(dir, "link.txt");
    remove_file_in_dir(dir, "real.txt");
    assert(rmdir(dir) == 0);
    free(dir);
}

static void
test_apply_patch_validates_before_install(void)
{
    char *dir = make_temp_workspace();
    char path[4096];
    json_t *result;
    const char patch[] =
        "*** Begin Patch\n"
        "*** Add File: added.txt\n"
        "+should-not-exist\n"
        "*** Update File: missing.txt\n"
        "@@\n"
        "-old\n"
        "+new\n"
        "*** End Patch\n";

    result = run_apply_patch(dir, patch);
    assert(strcmp(snj_json_string(result, "status"), "patch_rejected") == 0);
    json_decref(result);
    join_path(path, sizeof(path), dir, "added.txt");
    assert(access(path, F_OK) < 0 && errno == ENOENT);
    assert(rmdir(dir) == 0);
    free(dir);
}

static void
test_apply_patch_preview_is_bounded(void)
{
    char *dir = make_temp_workspace();
    char path[4096];
    char *patch;
    char *text;
    json_t *result;
    const size_t payload = 150u * 1024u;
    const char *head =
        "*** Begin Patch\n"
        "*** Add File: big.txt\n"
        "+";
    const char *tail =
        "\n"
        "*** End Patch\n";
    size_t len = strlen(head) + payload + strlen(tail);

    patch = malloc(len + 1u);
    assert(patch != NULL);
    memcpy(patch, head, strlen(head));
    memset(patch + strlen(head), 'a', payload);
    memcpy(patch + strlen(head) + payload, tail, strlen(tail) + 1u);
    result = run_apply_patch(dir, patch);
    assert(strcmp(snj_json_string(result, "status"), "succeeded") == 0);
    {
        const char *model_text = snj_json_string(result, "model_text");
        assert(strlen(model_text) < PATCH_MODEL_MAX_FOR_TEST);
        assert(strstr(model_text, "Diff preview (bounded") != NULL);
        assert(strstr(model_text, "diff preview truncated") != NULL);
    }
    json_decref(result);
    join_path(path, sizeof(path), dir, "big.txt");
    text = read_text_file(path);
    assert(strlen(text) == payload + 1u);
    free(text);
    remove_file_in_dir(dir, "big.txt");
    assert(rmdir(dir) == 0);
    free(patch);
    free(dir);
}

int
main(void)
{
    test_success_and_streams();
    test_failure_status();
    test_timeout();
    test_timeout_kills_process_family();
    test_large_stdout_uses_blocking_child_fd();
    test_stdin_uses_blocking_child_fd();
    test_pty_merges_stdout_and_stderr();
    test_managed_pty_write_stdin_completes();
    test_managed_process_write_stdin_completes();
    test_managed_process_accepts_repeated_write_stdin();
    test_managed_process_poll_consumes_exit();
    test_write_stdin_rejects_unknown_handle();
    test_managed_process_close_returns_terminal_result();
    test_managed_close_kills_process_family();
    test_provider_secret_removed_from_environment();
    test_apply_patch_add_update_delete();
    test_apply_patch_rejects_ambiguous_match();
    test_apply_patch_rejects_path_escape();
    test_apply_patch_rejects_symlink_target();
    test_apply_patch_validates_before_install();
    test_apply_patch_preview_is_bounded();
    test_provider_secret_redacted_from_output();
    test_provider_secret_redacted_across_read_boundary();
    puts("test_tools: ok");
    return 0;
}
