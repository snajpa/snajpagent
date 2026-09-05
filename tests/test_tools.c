/* SPDX-License-Identifier: GPL-2.0-only */
#include "base.h"
#include "config.h"
#include "credential.h"
#include "json.h"
#include "tools.h"
#include "tools_patch.h"
#include "turn.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <signal.h>
#include "snag_jansson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATCH_MODEL_MAX_FOR_TEST (512u * 1024u)

static struct {
    char handle[SNAG_ID_HEX_LEN + 1u];
    struct snag_buf streams[2];
} output_journal[128];
static size_t output_count;
static bool fail_output;

static size_t
output_index(const char *handle)
{
    size_t i;
    for (i = 0u; i < output_count; ++i)
        if (!strcmp(output_journal[i].handle, handle))
            return i;
    assert(i < 128u);
    memcpy(output_journal[i].handle, handle, sizeof(output_journal[i].handle));
    for (unsigned int s = 0u; s < 2u; ++s)
        snag_buf_init(&output_journal[i].streams[s], 4u * 1024u * 1024u);
    ++output_count;
    return i;
}

static int
retain_output(void *opaque, const char *handle, unsigned int stream,
               uint64_t offset, const void *bytes, size_t len)
{
    (void)opaque;
    if (fail_output) {
        errno = ENOSPC;
        return -1;
    }
    struct snag_buf *out = &output_journal[output_index(handle)].streams[stream];
    assert(offset == out->len);
    return snag_buf_append(out, bytes, len);
}

static int
read_output(void *opaque, const char *handle, unsigned int stream,
             uint64_t from, uint64_t to, struct snag_buf *out)
{
    (void)opaque;
    struct snag_buf *source = &output_journal[output_index(handle)].streams[stream];
    assert(from <= to && to <= source->len);
    size_t len = (size_t)(to - from);
    if (len <= out->max)
        return snag_buf_append(out, source->data + from, len);
    size_t head = out->max / 2u, tail = out->max - head;
    return snag_buf_append(out, source->data + from, head) < 0 ||
           snag_buf_append(out, source->data + to - tail, tail) < 0 ? -1 : 0;
}

static int
close_command(const char *handle, bool user_interrupt, snag_tool_pump_fn pump,
                void *opaque, int wake_fd, json_t **result, char *error, size_t size)
{
    int rc = snag_tools_close_managed(handle, user_interrupt, pump, opaque, wake_fd,
                                     result, error, size);
    if (rc == 0)
        snag_tools_collected(handle);
    return rc;
}

static json_t *
call_args_yield(const char *command, const char *workdir, int timeout_ms,
                int yield_ms, const char *stdin_text)
{
    json_t *args = json_object();
    assert(args);
    assert(snag_json_set_new(args, "command", json_string(command)) == 0);
    assert(snag_json_set_new(args, "workdir", json_string(workdir)) == 0);
    assert(snag_json_set_new(args, "timeout_ms",
                            timeout_ms < 0 ? json_null() :
                                             json_integer(timeout_ms)) == 0);
    assert(snag_json_set_new(args, "yield_ms", json_integer(yield_ms)) == 0);
    assert(snag_json_set_new(args, "max_output_tokens", json_null()) == 0);
    assert(snag_json_set_new(args, "stdin",
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
make_call_with_pty(struct snag_response_graph *graph, const char *command,
                   const char *workdir, int timeout_ms, const char *stdin_text,
                   bool pty)
{
    json_t *args = call_args(command, workdir, timeout_ms, stdin_text);
    assert(args != NULL);
    assert(snag_json_set_new(args, "pty", json_boolean(pty)) == 0);
    snag_response_graph_init(graph);
    assert(snag_response_graph_set_provider_id(graph, "resp_tool_test") == 0);
    assert(snag_response_graph_add_call(graph, "item_tool_test",
                                       "call_tool_test", "exec_command",
                                       args) == 0);
}

static void
make_call(struct snag_response_graph *graph, const char *command,
          const char *workdir, int timeout_ms, const char *stdin_text)
{
    make_call_with_pty(graph, command, workdir, timeout_ms, stdin_text, false);
}

static json_t *
run_command_full(const char *command, int timeout_ms, const char *secret,
                 const char *stdin_text, snag_tool_pump_fn pump,
                 void *pump_opaque, int selected_limit,
                 uint32_t ceiling)
{
    char cwd[4096];
    struct snag_config config;
    struct snag_credential credential;
    struct snag_response_graph graph;
    json_t *result = NULL;
    char error[256];

    assert(getcwd(cwd, sizeof(cwd)) != NULL);
    snag_config_init(&config);
    assert(config.shell != NULL);
    config.default_timeout_ms = 0;
    config.max_timeout_ms = 5000;
    config.max_output_tokens = ceiling;
    snag_credential_clear(&credential);
    if (secret) {
        credential.len = strlen(secret);
        assert(credential.len <= SNAG_CREDENTIAL_MAX);
        memcpy(credential.value, secret, credential.len + 1u);
    }
    make_call(&graph, command, cwd, timeout_ms, stdin_text);
    if (selected_limit >= 0)
        assert(json_object_set_new(graph.items[0].arguments,
            "max_output_tokens", json_integer(selected_limit)) == 0);
    error[0] = '\0';
    {
        int rc = snag_tools_run(&graph.items[0], &config, &credential, cwd,
                               pump, pump_opaque, -1, &result, error, sizeof(error));
        if (rc != 0)
            fprintf(stderr, "tool error: %s errno=%d\n", error, errno);
        assert(rc == 0);
    }
    assert(result != NULL);
    assert(snag_tool_result_valid(result) == 0);
    assert(json_integer_value(json_object_get(result,
               "max_output_tokens")) ==
           (selected_limit >= 0 && (uint32_t)selected_limit < ceiling ?
            (uint32_t)selected_limit : ceiling));
    snag_response_graph_free(&graph);
    snag_config_free(&config);
    return result;
}

static json_t *
run_command_with_credential(const char *command, int timeout_ms,
                            const char *secret)
{
    return run_command_full(command, timeout_ms, secret, NULL, NULL, NULL,
                            -1, 6000u);
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
    struct snag_config config;
    struct snag_credential credential;
    struct snag_response_graph graph;
    json_t *result = NULL;
    char error[256];

    assert(getcwd(cwd, sizeof(cwd)) != NULL);
    snag_config_init(&config);
    config.default_timeout_ms = 1000;
    config.max_timeout_ms = 5000;
    snag_credential_clear(&credential);
    make_call_with_pty(&graph, command, cwd, timeout_ms, NULL, true);
    error[0] = '\0';
    {
        int rc = snag_tools_run(&graph.items[0], &config, &credential, cwd,
                               NULL, NULL, -1, &result, error, sizeof(error));
        if (rc != 0)
            fprintf(stderr, "pty tool error: %s errno=%d\n", error, errno);
        assert(rc == 0);
    }
    assert(result != NULL);
    assert(snag_tool_result_valid(result) == 0);
    snag_response_graph_free(&graph);
    snag_config_free(&config);
    return result;
}

static json_t *
run_tool_with_args_pump(const char *name, json_t *args,
                        snag_tool_pump_fn pump, void *pump_opaque)
{
    char cwd[4096];
    struct snag_config config;
    struct snag_credential credential;
    struct snag_response_graph graph;
    json_t *result = NULL;
    char error[256];
    int rc;

    assert(getcwd(cwd, sizeof(cwd)) != NULL);
    snag_config_init(&config);
    config.default_timeout_ms = 1000;
    config.default_yield_ms = 1000;
    config.max_timeout_ms = 5000;
    snag_credential_clear(&credential);
    snag_response_graph_init(&graph);
    assert(snag_response_graph_set_provider_id(&graph, "resp_managed_test") == 0);
    assert(snag_response_graph_add_call(&graph, "item_managed_test",
                                       "call_managed_test", name, args) == 0);
    error[0] = '\0';
    rc = snag_tools_run(&graph.items[0], &config, &credential, cwd,
                       pump, pump_opaque, -1, &result, error, sizeof(error));
    if (rc != 0)
        fprintf(stderr, "tool error: %s errno=%d\n", error, errno);
    assert(rc == 0);
    assert(result != NULL);
    assert(snag_tool_result_valid(result) == 0);
    snag_response_graph_free(&graph);
    snag_config_free(&config);
    return result;
}

static json_t *
run_tool_with_args(const char *name, json_t *args)
{
    return run_tool_with_args_pump(name, args, NULL, NULL);
}

static json_t *
run_managed_exec_with_pty(const char *command, int timeout_ms, int yield_ms,
                         bool pty)
{
    char cwd[4096];
    json_t *args;

    assert(getcwd(cwd, sizeof(cwd)) != NULL);
    args = call_args_yield(command, cwd, timeout_ms, yield_ms, NULL);
    assert(snag_json_set_new(args, "pty", json_boolean(pty)) == 0);
    return run_tool_with_args("exec_command", args);
}

static json_t *
run_managed_exec(const char *command, int timeout_ms, int yield_ms)
{
    return run_managed_exec_with_pty(command, timeout_ms, yield_ms, false);
}

static json_t *
run_write_stdin_call_limit(const char *handle, const char *data, bool eof,
                           int yield_ms, int max_output_tokens)
{
    json_t *args = json_object();
    assert(args != NULL);
    assert(snag_json_set_new(args, "handle", json_string(handle)) == 0);
    assert(snag_json_set_new(args, "data", json_string(data)) == 0);
    assert(snag_json_set_new(args, "eof", eof ? json_true() : json_false()) == 0);
    assert(snag_json_set_new(args, "terminate", json_false()) == 0);
    assert(snag_json_set_new(args, "yield_ms", json_integer(yield_ms)) == 0);
    assert(snag_json_set_new(args, "max_output_tokens",
        max_output_tokens < 0 ? json_null() :
                                json_integer(max_output_tokens)) == 0);
    return run_tool_with_args("write_stdin", args);
}

static json_t *
run_write_stdin_call(const char *handle, const char *data, bool eof,
                     int yield_ms)
{
    return run_write_stdin_call_limit(handle, data, eof, yield_ms, -1);
}

static json_t *
run_terminate_call(const char *handle, const char *data, bool eof)
{
    json_t *args = json_object();
    assert(args != NULL);
    assert(snag_json_set_new(args, "handle", json_string(handle)) == 0);
    assert(snag_json_set_new(args, "data", json_string(data)) == 0);
    assert(snag_json_set_new(args, "eof", eof ? json_true() : json_false()) == 0);
    assert(snag_json_set_new(args, "terminate", json_true()) == 0);
    assert(snag_json_set_new(args, "yield_ms", json_integer(0)) == 0);
    assert(snag_json_set_new(args, "max_output_tokens", json_null()) == 0);
    return run_tool_with_args("write_stdin", args);
}

static json_t *
run_malformed_write_stdin_call(const char *handle)
{
    json_t *args = json_object();

    assert(args != NULL);
    assert(snag_json_set_new(args, "handle", json_string(handle)) == 0);
    assert(snag_json_set_new(args, "data", json_string("")) == 0);
    assert(snag_json_set_new(args, "eof", json_false()) == 0);
    assert(snag_json_set_new(args, "terminate", json_false()) == 0);
    assert(snag_json_set_new(args, "yield_ms", json_integer(0)) == 0);
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
    struct snag_config config;
    struct snag_credential credential;
    struct snag_response_graph graph;
    json_t *args = json_object();
    json_t *result = NULL;
    char error[256];
    int rc;

    assert(args != NULL);
    assert(snag_json_set_new(args, "patch", json_string(patch)) == 0);
    assert(snag_json_set_new(args, "workdir", json_string(workdir)) == 0);
    snag_config_init(&config);
    snag_credential_clear(&credential);
    snag_response_graph_init(&graph);
    assert(snag_response_graph_set_provider_id(&graph, "resp_patch_test") == 0);
    assert(snag_response_graph_add_call(&graph, "item_patch_test",
                                       "call_patch_test", "apply_patch",
                                       args) == 0);
    error[0] = '\0';
    rc = snag_tools_run(&graph.items[0], &config, &credential, workdir,
                       NULL, NULL, -1, &result, error, sizeof(error));
    if (rc != 0)
        fprintf(stderr, "patch tool error: %s errno=%d\n", error, errno);
    assert(rc == 0);
    assert(result != NULL);
    assert(snag_tool_result_valid(result) == 0);
    snag_response_graph_free(&graph);
    snag_config_free(&config);
    return result;
}

static void
test_apply_patch_rejects_null_result(void)
{
    char error[256] = {0};

    errno = 0;
    assert(snag_tools_apply_patch(NULL, NULL, NULL,
                                 error, sizeof(error)) < 0);
    assert(errno == EINVAL);
    assert(strstr(error, "result destination") != NULL);
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

static int
handoff_once_pump(void *opaque, unsigned int timeout_ms)
{
    bool *requested = opaque;

    (void)timeout_ms;
    if (*requested)
        return 0;
    *requested = true;
    return 1;
}

static void
test_managed_process_hands_off_on_steering(void)
{
    char cwd[4096];
    json_t *args;
    json_t *result;
    json_t *closed = NULL;
    const char *handle;
    char error[256] = {0};
    uint64_t started = snag_time_ms();
    bool requested = false;

    assert(getcwd(cwd, sizeof(cwd)) != NULL);
    args = call_args_yield("sleep 2", cwd, 4000, 0, NULL);
    assert(snag_json_set_new(args, "pty", json_false()) == 0);
    result = run_tool_with_args_pump("exec_command", args,
                                     handoff_once_pump, &requested);
    assert(requested);
    assert(snag_time_ms() - started < 1000u);
    assert(strcmp(snag_json_string(result, "status"), "running") == 0);
    assert(strcmp(snag_json_string(result, "reason"),
                  "steering_handoff") == 0);
    assert(strstr(snag_json_string(result, "model_text"),
                  "steering arrived") != NULL);
    handle = snag_json_string(result, "handle");
    assert(handle != NULL);
    assert(close_command(handle, false, NULL, NULL, -1,
                                   &closed, error, sizeof(error)) == 0);
    assert(closed != NULL && snag_tool_result_valid(closed) == 0);
    assert(json_integer_value(json_object_get(closed,
               "max_output_tokens")) == 6000);
    json_decref(closed);
    json_decref(result);
}

static void
test_success_and_streams(void)
{
    json_t *result = run_command("printf out; printf err >&2", 1000);
    json_t *out = json_object_get(result, "stdout");
    json_t *err = json_object_get(result, "stderr");
    assert(strcmp(snag_json_string(result, "status"), "succeeded") == 0);
    assert(strcmp(snag_json_string(out, "retained"), "out") == 0);
    assert(strcmp(snag_json_string(err, "retained"), "err") == 0);
    json_decref(result);
}

static void
test_command_output_limit_selection(void)
{
    static const int requests[] = {-1, 1, 123, 6789, 6790, INT32_MAX};
    for (size_t i = 0u; i < sizeof(requests) / sizeof(requests[0]); ++i) {
        json_t *result = run_command_full("printf unchanged", 1000, NULL,
            NULL, NULL, NULL, requests[i], 6789u);

        json_t *ref = json_object_get(result, "output_ref");
        struct snag_buf *full = &output_journal[output_index(snag_json_string(ref, "handle"))].streams[0];
        assert(full->len == 9u && !memcmp(full->data, "unchanged", 9u));
        assert(json_int_member(json_object_get(result, "stdout"), "original_bytes") == 9);
        json_decref(result);
    }
}

static void
test_managed_output_ceiling(void)
{
    json_t *started = run_managed_exec("read line; printf '%s' \"$line\"", 5000, 1);
    const char *handle = snag_json_string(started, "handle");
    static const int requests[] = {-1, 42, 6000, 6001};
    json_t *closed = NULL;
    char error[256] = {0};

    assert(handle != NULL);
    for (size_t i = 0u; i < sizeof(requests) / sizeof(requests[0]); ++i) {
        json_t *result = run_write_stdin_call_limit(handle, "", false, 1,
                                                    requests[i]);
        assert(strcmp(snag_json_string(result, "status"), "running") == 0);
        assert(json_integer_value(json_object_get(result, "max_output_tokens")) ==
               (requests[i] == 42 ? 42 : 6000));
        json_decref(result);
    }
    assert(close_command(handle, false, NULL, NULL, -1,
                                   &closed, error, sizeof(error)) == 0);
    assert(json_integer_value(json_object_get(closed, "max_output_tokens")) == 6000);
    json_decref(closed);
    json_decref(started);
}

static void
test_command_output_limit_is_required_and_positive(void)
{
    char cwd[4096];
    struct snag_config config;
    struct snag_credential credential;
    struct snag_response_graph graph;
    json_t *result = NULL;
    char error[256] = {0};

    assert(getcwd(cwd, sizeof(cwd)) != NULL);
    snag_config_init(&config);
    snag_credential_clear(&credential);
    make_call(&graph, "printf never-run", cwd, 1000, NULL);
    assert(json_object_del(graph.items[0].arguments,
                           "max_output_tokens") == 0);
    assert(snag_tools_run(&graph.items[0], &config, &credential, cwd,
                         NULL, NULL, -1, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "not_run"));
    json_decref(result);
    result = snag_tool_result_terminal(false, "invalid arguments");
    assert(result != NULL);
    config.max_output_tokens = 123u;
    assert(snag_tools_attach_output_limit(&graph.items[0], &config, result) == 0);
    assert(json_integer_value(json_object_get(result, "max_output_tokens")) == 123);
    json_decref(result);
    result = NULL;
    assert(json_object_set_new(graph.items[0].arguments,
               "max_output_tokens", json_integer(0)) == 0);
    assert(snag_tools_run(&graph.items[0], &config, &credential, cwd,
                         NULL, NULL, -1, &result, error, sizeof(error)) == 0);
    assert(!strcmp(snag_json_string(result, "status"), "not_run"));
    json_decref(result);
    result = snag_tool_result_terminal(false, "invalid arguments");
    assert(result != NULL);
    assert(snag_tools_attach_output_limit(&graph.items[0], &config, result) == 0);
    assert(json_integer_value(json_object_get(result, "max_output_tokens")) == 123);
    json_decref(result);
    snag_response_graph_free(&graph);
    snag_config_free(&config);
}

static void
test_failure_status(void)
{
    json_t *result = run_command("exit 7", 1000);
    json_t *exit_code = json_object_get(result, "exit_code");
    assert(strcmp(snag_json_string(result, "status"), "failed") == 0);
    assert(json_is_integer(exit_code));
    assert(json_integer_value(exit_code) == 7);
    json_decref(result);
}

static void
test_timeout_hands_off_without_killing(void)
{
    json_t *result = run_command("sleep 0.15; printf survived", 20);
    const char *handle;
    json_t *completed;

    assert(strcmp(snag_json_string(result, "status"), "running") == 0);
    assert(strcmp(snag_json_string(result, "reason"), "timeout_handoff") == 0);
    assert(strstr(snag_json_string(result, "model_text"),
                  "process continues in the background") != NULL);
    handle = snag_json_string(result, "handle");
    assert(handle != NULL && snag_hex_is_lower(handle, SNAG_ID_HEX_LEN));
    sleep_ms(250);
    completed = run_write_stdin_call_limit(handle, "", false, 0, 222);
    assert(strcmp(snag_json_string(completed, "status"), "succeeded") == 0);
    assert(json_integer_value(json_object_get(completed,
               "max_output_tokens")) == 222);
    assert(strcmp(snag_json_string(json_object_get(completed, "stdout"),
                                  "retained"), "survived") == 0);
    json_decref(completed);
    json_decref(result);
}

static void
test_no_timeout(void)
{
    json_t *result = run_command("sleep 0.05; printf no-timeout", -1);

    assert(strcmp(snag_json_string(result, "status"), "succeeded") == 0);
    assert(strcmp(snag_json_string(json_object_get(result, "stdout"),
                                  "retained"), "no-timeout") == 0);
    json_decref(result);
}

static void
test_large_stdout_is_complete_for_model(void)
{
    json_t *result = run_command(
        "perl -e 'binmode STDOUT; print q{x} x (1024 * 1024) or exit 23'",
        5000);
    json_t *out = json_object_get(result, "stdout");

    assert(strcmp(snag_json_string(result, "status"), "succeeded") == 0);
    assert(json_int_member(out, "original_bytes") == 1024 * 1024);
    assert(json_int_member(out, "retained_bytes") == 6000);
    assert(json_int_member(out, "discarded_bytes") == 1024 * 1024 - 6000);
    const char *handle = snag_json_string(json_object_get(result, "output_ref"), "handle");
    assert(output_journal[output_index(handle)].streams[0].len == 1024u * 1024u);
    assert(strlen(snag_json_string(result, "model_text")) < 7000u);
    json_decref(result);
}

static void
test_binary_stdout_is_complete_for_model(void)
{
    json_t *result = run_command(
        "perl -e 'binmode STDOUT; print pack(q{C*}, 0, 255)'", 1000);
    json_t *out = json_object_get(result, "stdout");

    assert(strcmp(snag_json_string(out, "encoding"), "base64") == 0);
    assert(strcmp(snag_json_string(out, "retained"), "AP8=") == 0);
    assert(json_int_member(out, "discarded_bytes") == 0);
    assert(strstr(snag_json_string(result, "model_text"), "AP8=") != NULL);
    json_decref(result);
}

static void
test_stdin_uses_blocking_child_fd(void)
{
    bool delayed = false;
    json_t *result = run_command_full(
        "cat",
        1000, NULL, "hello", delay_once_pump, &delayed, -1, 4000u);

    assert(delayed);
    assert(strcmp(snag_json_string(result, "status"), "succeeded") == 0);
    assert(strcmp(snag_json_string(json_object_get(result, "stdout"),
                                  "retained"), "hello") == 0);
    json_decref(result);
}

static void
test_pty_merges_stdout_and_stderr(void)
{
    json_t *result = run_pty_command("printf out; printf err >&2", 1000);
    const char *merged = snag_json_string(json_object_get(result, "stdout"),
                                         "retained");

    assert(strcmp(snag_json_string(result, "status"), "succeeded") == 0);
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

    assert(strcmp(snag_json_string(result, "status"), "running") == 0);
    assert(json_int_member(json_object_get(result, "stderr"),
                           "original_bytes") == 0);
    handle = snag_json_string(result, "handle");
    assert(handle != NULL && snag_hex_is_lower(handle, SNAG_ID_HEX_LEN));
    next = run_write_stdin_call(handle, "hello\r", true, 5000);
    assert(strcmp(snag_json_string(next, "status"), "succeeded") == 0);
    merged = snag_json_string(json_object_get(next, "stdout"), "retained");
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

    assert(strcmp(snag_json_string(result, "status"), "running") == 0);
    handle = snag_json_string(result, "handle");
    assert(handle != NULL && snag_hex_is_lower(handle, SNAG_ID_HEX_LEN));
    next = run_write_stdin_call(handle, "hello\n", true, 5000);
    assert(strcmp(snag_json_string(next, "status"), "succeeded") == 0);
    assert(strstr(snag_json_string(json_object_get(next, "stdout"),
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

    assert(strcmp(snag_json_string(result, "status"), "running") == 0);
    handle = snag_json_string(result, "handle");
    assert(handle != NULL && snag_hex_is_lower(handle, SNAG_ID_HEX_LEN));
    next = run_write_stdin_call(handle, "one\n", false, 50);
    assert(strcmp(snag_json_string(next, "status"), "running") == 0);
    done = run_write_stdin_call(handle, "two\n", true, 5000);
    assert(strcmp(snag_json_string(done, "status"), "succeeded") == 0);
    assert(strstr(snag_json_string(json_object_get(next, "stdout"),
                                  "retained"), "first:one") != NULL);
    assert(strstr(snag_json_string(json_object_get(done, "stdout"),
                                  "retained"), "first:one") == NULL);
    assert(strstr(snag_json_string(json_object_get(done, "stdout"),
                                  "retained"), "second:two") != NULL);
    json_decref(done);
    json_decref(next);
    json_decref(result);
}

static void
test_managed_process_without_timeout(void)
{
    json_t *result = run_managed_exec(
        "printf 'start\\n'; sleep 0.05; printf 'done\\n'",
        -1, 10);
    const char *handle;
    json_t *next;

    assert(strcmp(snag_json_string(result, "status"), "running") == 0);
    handle = snag_json_string(result, "handle");
    assert(handle != NULL);
    {
        struct timespec remaining = {0, 500000000L};
        while (nanosleep(&remaining, &remaining) < 0 && errno == EINTR)
            ;
    }
    next = run_write_stdin_call(handle, "", false, 0);
    assert(strcmp(snag_json_string(next, "status"), "succeeded") == 0);
    assert(strstr(snag_json_string(json_object_get(next, "stdout"),
                                  "retained"), "done") != NULL);
    json_decref(next);
    json_decref(result);
}

static void
test_write_stdin_rejects_unknown_handle(void)
{
    json_t *result = run_write_stdin_call("00000000000000000000000000000000",
                                          "x", false, 0);

    assert(strcmp(snag_json_string(result, "status"), "not_run") == 0);
    json_decref(result);
}

static void
test_wrong_handle_does_not_touch_active_process(void)
{
    json_t *result = run_managed_exec(
        "IFS= read -r line; printf 'got:%s\\n' \"$line\"", 5000, 50);
    const char *handle;
    json_t *wrong;
    json_t *done;

    assert(strcmp(snag_json_string(result, "status"), "running") == 0);
    handle = snag_json_string(result, "handle");
    assert(handle != NULL && snag_hex_is_lower(handle, SNAG_ID_HEX_LEN));
    wrong = run_write_stdin_call("00000000000000000000000000000000",
                                 "wrong\\n", true, 0);
    assert(strcmp(snag_json_string(wrong, "status"), "not_run") == 0);
    done = run_write_stdin_call(handle, "right\\n", true, 5000);
    assert(strcmp(snag_json_string(done, "status"), "succeeded") == 0);
    assert(strstr(snag_json_string(json_object_get(done, "stdout"),
                                  "retained"), "got:right") != NULL);
    assert(strstr(snag_json_string(json_object_get(done, "stdout"),
                                  "retained"), "got:wrong") == NULL);
    json_decref(done);
    json_decref(wrong);
    json_decref(result);
}

static void
test_malformed_interaction_preserves_active_process(void)
{
    json_t *result = run_managed_exec(
        "IFS= read -r line; printf 'got:%s\\n' \"$line\"", 5000, 50);
    const char *handle;
    json_t *rejected;
    json_t *done;

    assert(strcmp(snag_json_string(result, "status"), "running") == 0);
    handle = snag_json_string(result, "handle");
    assert(handle != NULL && snag_hex_is_lower(handle, SNAG_ID_HEX_LEN));
    rejected = run_malformed_write_stdin_call(handle);
    assert(strcmp(snag_json_string(rejected, "status"), "not_run") == 0);
    assert(json_is_null(json_object_get(rejected, "handle")));
    done = run_write_stdin_call(handle, "right\\n", true, 5000);
    assert(strcmp(snag_json_string(done, "status"), "succeeded") == 0);
    assert(strstr(snag_json_string(json_object_get(done, "stdout"),
                                  "retained"), "got:right") != NULL);
    json_decref(done);
    json_decref(rejected);
    json_decref(result);
}

static void
test_write_stdin_terminates_managed_process(void)
{
    json_t *result = run_managed_exec("sleep 5", 5000, 50);
    const char *handle;
    json_t *terminated;
    const char *status;

    assert(strcmp(snag_json_string(result, "status"), "running") == 0);
    handle = snag_json_string(result, "handle");
    assert(handle != NULL && snag_hex_is_lower(handle, SNAG_ID_HEX_LEN));
    terminated = run_terminate_call(handle, "", false);
    status = snag_json_string(terminated, "status");
    assert(status != NULL && strcmp(status, "running") != 0);
    assert(json_is_null(json_object_get(terminated, "handle")));
    json_decref(terminated);
    json_decref(result);
}

static void
test_invalid_termination_preserves_managed_process(void)
{
    json_t *result = run_managed_exec("sleep 5", 5000, 50);
    const char *handle;
    json_t *rejected;
    json_t *terminated;

    assert(strcmp(snag_json_string(result, "status"), "running") == 0);
    handle = snag_json_string(result, "handle");
    assert(handle != NULL && snag_hex_is_lower(handle, SNAG_ID_HEX_LEN));
    rejected = run_terminate_call(handle, "must not be written", false);
    assert(strcmp(snag_json_string(rejected, "status"), "not_run") == 0);
    assert(json_is_null(json_object_get(rejected, "handle")));
    json_decref(rejected);
    rejected = run_terminate_call(handle, "", true);
    assert(strcmp(snag_json_string(rejected, "status"), "not_run") == 0);
    assert(json_is_null(json_object_get(rejected, "handle")));
    json_decref(rejected);
    terminated = run_terminate_call(handle, "", false);
    assert(strcmp(snag_json_string(terminated, "status"), "running") != 0);
    json_decref(terminated);
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

    assert(strcmp(snag_json_string(result, "status"), "running") == 0);
    handle = snag_json_string(result, "handle");
    assert(handle != NULL && snag_hex_is_lower(handle, SNAG_ID_HEX_LEN));
    error[0] = '\0';
    assert(close_command(handle, false, NULL, NULL, -1,
                                   &closed, error, sizeof(error)) == 0);
    assert(closed != NULL);
    assert(snag_tool_result_valid(closed) == 0);
    assert(json_integer_value(json_object_get(closed,
               "max_output_tokens")) == 6000);
    status = snag_json_string(closed, "status");
    assert(strcmp(status, "running") != 0);
    assert(json_is_null(json_object_get(closed, "handle")));
    json_decref(closed);
    json_decref(result);
}

static void
test_timeout_handoff_preserves_process_family(void)
{
    char *dir = make_temp_workspace();
    char marker[4096];
    char command[8192];
    json_t *result;
    json_t *completed;
    const char *handle;

    join_path(marker, sizeof(marker), dir, "leaked.txt");
    assert(snprintf(command, sizeof(command),
                    "(sleep 0.25; printf leaked > '%s') & wait",
                    marker) > 0);
    result = run_command(command, 50);
    assert(strcmp(snag_json_string(result, "status"), "running") == 0);
    handle = snag_json_string(result, "handle");
    assert(handle != NULL);
    sleep_ms(500);
    completed = run_write_stdin_call(handle, "", false, 0);
    assert(strcmp(snag_json_string(completed, "status"), "succeeded") == 0);
    assert(access(marker, F_OK) == 0);
    assert(unlink(marker) == 0);
    json_decref(completed);
    json_decref(result);
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
    assert(strcmp(snag_json_string(result, "status"), "running") == 0);
    handle = snag_json_string(result, "handle");
    assert(handle != NULL && snag_hex_is_lower(handle, SNAG_ID_HEX_LEN));
    error[0] = '\0';
    assert(close_command(handle, false, NULL, NULL, -1,
                                   &closed, error, sizeof(error)) == 0);
    assert(closed != NULL);
    assert(snag_tool_result_valid(closed) == 0);
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
    const char *retained = snag_json_string(json_object_get(result, "stdout"),
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
    const char *retained = snag_json_string(json_object_get(result, "stdout"),
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
    assert(strcmp(snag_json_string(json_object_get(result, "stdout"),
                                  "retained"), "unset") == 0);
    unsetenv("OPENAI_API_KEY");
    json_decref(result);
}

static void
test_all_provider_secrets_removed_and_redacted(void)
{
    char cwd[4096];
    struct snag_config config;
    struct snag_credential credential;
    struct snag_response_graph graph;
    json_t *result = NULL;
    const char *retained;
    char error[256] = {0};

    assert(getcwd(cwd, sizeof(cwd)) != NULL);
    snag_config_init(&config);
    config.providers[1] = config.providers[0];
    config.provider_count = 2u;
    assert(snprintf(config.providers[1].name,
                    sizeof(config.providers[1].name), "second") > 0);
    assert(snprintf(config.providers[1].api_key_env,
                    sizeof(config.providers[1].api_key_env),
                    "SECOND_PROVIDER_KEY") > 0);
    assert(setenv("SECOND_PROVIDER_KEY", "second-provider-secret", 1) == 0);
    snag_credential_clear(&credential);
    make_call(&graph,
              "printf \"${SECOND_PROVIDER_KEY-unset}:second-provider-secret\"",
              cwd, 1000, NULL);
    assert(snag_tools_run(&graph.items[0], &config, &credential, cwd,
                         NULL, NULL, -1, &result, error, sizeof(error)) == 0);
    retained = snag_json_string(json_object_get(result, "stdout"), "retained");
    assert(strcmp(retained, "unset:<redacted:secret>") == 0);
    assert(unsetenv("SECOND_PROVIDER_KEY") == 0);
    json_decref(result);
    snag_response_graph_free(&graph);
    snag_config_free(&config);
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
    assert(strcmp(snag_json_string(result, "status"), "succeeded") == 0);
    {
        const char *model_text = snag_json_string(result, "model_text");
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
test_patch_line_endings(void)
{
    static const struct {
        const char *before, *after;
    } cases[] = {
        {"a\n\nb\n", "a\n\nB\n"},
        {"a\r\n\r\nb\r\n", "a\r\n\r\nB\r\n"},
        {"a\n\nb", "a\n\nB"},
        {"a\r\n\r\nb", "a\r\n\r\nB"},
        {"a\nb\r\n", NULL},
        {"a\rb\n", NULL},
        {"a\r\nb\n", NULL}
    };
    char *dir = make_temp_workspace();
    char path[4096];

    join_path(path, sizeof(path), dir, "lines");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        json_t *result;
        char *text;
        write_text_file(path, cases[i].before);
        result = run_apply_patch(dir, "*** Begin Patch\r\n*** Update File: lines\r\n"
                                     "@@\r\n-b\r\n+B\r\n*** End Patch\r\n");
        assert(strcmp(snag_json_string(result, "status"),
                      cases[i].after ? "succeeded" : "patch_rejected") == 0);
        text = read_text_file(path);
        assert(strcmp(text, cases[i].after ? cases[i].after : cases[i].before) == 0);
        free(text);
        json_decref(result);
    }
    remove_file_in_dir(dir, "lines");
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
    assert(strcmp(snag_json_string(result, "status"), "patch_rejected") == 0);
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
    assert(strcmp(snag_json_string(result, "status"), "patch_rejected") == 0);
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
    assert(strcmp(snag_json_string(result, "status"), "patch_rejected") == 0);
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
    assert(strcmp(snag_json_string(result, "status"), "patch_rejected") == 0);
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
    assert(strcmp(snag_json_string(result, "status"), "succeeded") == 0);
    {
        const char *model_text = snag_json_string(result, "model_text");
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

static int
ro_cancel(void *opaque, unsigned int timeout_ms)
{
    (void)opaque;
    (void)timeout_ms;
    return 2;
}

static void
check_read_tool(const char *workspace, const char *name, const char *arguments,
                 bool success, const char *expected, snag_tool_pump_fn pump)
{
    struct snag_response_item call = {0};
    json_t *result = NULL;
    char error[256] = {0};

    call.kind = SNAG_ITEM_TOOL_CALL;
    call.name = (char *)name;
    call.arguments = snag_json_load_strict((const unsigned char *)arguments,
        strlen(arguments), 8192u, error, sizeof(error));
    assert(call.arguments);
    assert(snag_tools_read_only(&call, workspace, pump, NULL, &result) ==
            (pump ? 2 : 0));
    assert(snag_tool_result_valid(result) == 0);
    assert(strcmp(snag_json_string(result, "status"), success ? "succeeded" : "failed") == 0);
    assert(strstr(snag_json_string(result, "model_text"), expected));
    json_decref(result);
    json_decref(call.arguments);
}

static void
test_native_read_tools(void)
{
    char temp[4096], path[4096];
    const char *scratch = getenv("TMPDIR");
    bool read_only;
    FILE *file;

    assert(strcmp(snag_prompt_parse("/ro\n inspect", &read_only), "inspect") == 0 && read_only);
    assert(!*snag_prompt_parse("/ro  \t", &read_only) && read_only);
    assert(strcmp(snag_prompt_parse("//ro inspect", &read_only), "/ro inspect") == 0 && !read_only);
    assert(strcmp(snag_prompt_parse("/root", &read_only), "/root") == 0 && !read_only);
    assert(snprintf(temp, sizeof(temp), "%s/ro-tools-XXXXXX", scratch ? scratch : "/tmp") > 0);
    assert(mkdtemp(temp));
    join_path(path, sizeof(path), temp, "a ; echo nope");
    write_text_file(path, "Alpha\nβeta\nlast");
    join_path(path, sizeof(path), temp, "sub");
    assert(mkdir(path, 0700) == 0);
    join_path(path, sizeof(path), temp, "sub/.hidden");
    write_text_file(path, "Alpha nested\n");
    join_path(path, sizeof(path), temp, "link");
    assert(symlink("sub", path) == 0);
    join_path(path, sizeof(path), temp, "pipe");
    assert(mkfifo(path, 0600) == 0);
    join_path(path, sizeof(path), temp, "binary");
    file = fopen(path, "wb");
    assert(file);
    for (size_t i = 0; i < 70000u; ++i)
        assert(fputc(0, file) != EOF);
    assert(fclose(file) == 0);

    check_read_tool(temp, "read_file", "{\"path\":\"a ; echo nope\",\"start_line\":null,\"end_line\":null}",
        true, "1:Alpha\n2:βeta\n3:last", NULL);
    check_read_tool(temp, "read_file", "{\"path\":\"a ; echo nope\",\"start_line\":2,\"end_line\":2}",
        true, "2:βeta\n", NULL);
    check_read_tool(temp, "read_file", "{\"path\":\"a ; echo nope\",\"start_line\":4,\"end_line\":null}",
        false, "beyond end", NULL);
    check_read_tool(temp, "read_file", "{\"path\":\"a ; echo nope\",\"start_line\":3,\"end_line\":2}",
        false, "Invalid", NULL);
    check_read_tool(temp, "read_file", "{\"path\":\"binary\",\"start_line\":null,\"end_line\":null}",
        false, "Non-text", NULL);
    check_read_tool(temp, "read_file", "{\"path\":\"link/.hidden\",\"start_line\":null,\"end_line\":null}",
        false, "Cannot open", NULL);
    check_read_tool(temp, "read_file", "{\"path\":\"pipe\",\"start_line\":null,\"end_line\":null}",
        false, "Cannot open", NULL);
    check_read_tool(temp, "list_files", "{\"path\":\".\",\"recursive\":true,\"offset\":null,\"limit\":null}",
        true, "./sub/.hidden\tfile", NULL);
    check_read_tool(temp, "list_files", "{\"path\":\".\",\"recursive\":false,\"offset\":null,\"limit\":1}",
        true, "More results (repeat with next_offset); returned=1; next_offset=1", NULL);
    check_read_tool(temp, "list_files", "{\"path\":\".\",\"recursive\":false,\"offset\":1,\"limit\":1}",
        true, "./binary\tfile", NULL);
    check_read_tool(temp, "grep", "{\"path\":\".\",\"pattern\":\"^alpha\",\"recursive\":null,\"ignore_case\":true,\"literal\":null,\"offset\":null,\"limit\":null}",
        true, "./sub/.hidden:1:Alpha nested", NULL);
    check_read_tool(temp, "grep", "{\"path\":\".\",\"pattern\":\"missing\",\"recursive\":true,\"ignore_case\":null,\"literal\":true,\"offset\":null,\"limit\":null}",
        true, "Complete; returned=0; next_offset=0; skipped_nontext_or_special=3", NULL);
    check_read_tool(temp, "grep", "{\"path\":\".\",\"pattern\":\"[\",\"recursive\":true,\"ignore_case\":null,\"literal\":false,\"offset\":null,\"limit\":null}",
        false, "", NULL);
    check_read_tool(temp, "grep", "{\"path\":\".\",\"pattern\":\"Alpha\",\"recursive\":true,\"ignore_case\":null,\"literal\":true,\"offset\":1,\"limit\":1}",
        true, "./sub/.hidden:1:Alpha nested", NULL);
    check_read_tool(temp, "read_file", "{\"path\":\"a ; echo nope\",\"start_line\":null,\"end_line\":null}",
        false, "interrupted", ro_cancel);
    join_path(path, sizeof(path), temp, "large");
    file = fopen(path, "wb");
    assert(file);
    for (size_t i = 0; i < 50000u; ++i)
        assert(fputs("1234567890\n", file) >= 0);
    assert(fclose(file) == 0);
    check_read_tool(temp, "read_file", "{\"path\":\"large\",\"start_line\":null,\"end_line\":null}",
        false, "narrower line range", NULL);
    check_read_tool(temp, "read_file", "{\"path\":\"large\",\"start_line\":49999,\"end_line\":50000}",
        true, "50000:1234567890", NULL);
    remove_file_in_dir(temp, "large");
    remove_file_in_dir(temp, "binary");
    remove_file_in_dir(temp, "pipe");
    remove_file_in_dir(temp, "link");
    remove_file_in_dir(temp, "sub/.hidden");
    join_path(path, sizeof(path), temp, "sub");
    assert(rmdir(path) == 0);
    remove_file_in_dir(temp, "a ; echo nope");
    assert(rmdir(temp) == 0);
}

static void
test_process_capacity_and_ready_collection(void)
{
    struct snag_config config;
    struct snag_credential credential;
    struct snag_response_graph graph;
    char cwd[4096], error[256] = {0}, handles[SNAG_MAX_PROCESSES][SNAG_ID_HEX_LEN + 1u];
    snag_config_init(&config);
    config.max_parallel_commands = SNAG_MAX_PROCESSES;
    snag_credential_clear(&credential);
    assert(getcwd(cwd, sizeof(cwd)));
    for (size_t i = 0u; i < SNAG_MAX_PROCESSES; ++i) {
        uint32_t yield;
        json_t *result = NULL;
        make_call(&graph, "printf slot", cwd, 1000, NULL);
        assert(snag_tools_prepare(&graph.items[0], &config, handles[i], &yield, &result) == 0);
        assert(snag_tools_start(&graph.items[0], &config, &credential, &result, error, sizeof(error)) == 0);
        assert(!result);
        snag_response_graph_free(&graph);
    }
    uint64_t deadline = snag_monotonic_ms() + 3000u;
    while (snag_tools_busy()) {
        assert(snag_monotonic_ms() < deadline);
        assert(snag_tools_service(10, -1, error, sizeof(error)) == 0);
    }
    /* Exited/uncollected jobs still consume all slots. */
    char unused[SNAG_ID_HEX_LEN + 1u];
    uint32_t yield;
    json_t *result = NULL;
    make_call(&graph, "printf forbidden", cwd, 1000, NULL);
    assert(snag_tools_prepare(&graph.items[0], &config, unused, &yield, &result) == 1);
    assert(!strcmp(snag_json_string(result, "reason"), "process_limit"));
    json_decref(result);
    snag_response_graph_free(&graph);
    for (size_t i = SNAG_MAX_PROCESSES; i > 0u; --i) {
        result = NULL;
        assert(snag_tools_collect(handles[i - 1u], NULL, &result, error, sizeof(error)) == 0);
        assert(snag_tool_result_valid(result) == 0);
        assert(!strcmp(snag_json_string(result, "status"), "succeeded"));
        assert(!strcmp(snag_json_string(json_object_get(result, "stdout"), "retained"), "slot"));
        snag_tools_collected(handles[i - 1u]);
        json_decref(result);
    }
    snag_config_free(&config);
}

static int
steer_after_input(void *opaque, unsigned int timeout_ms)
{
    unsigned int *calls = opaque;
    (void)timeout_ms;
    return ++*calls == 3u ? 1 : 0;
}

static void
test_steering_with_blocked_stdin(void)
{
    char *input = malloc(1024u * 1024u + 1u);
    unsigned int calls = 0u;
    assert(input);
    memset(input, 'x', 1024u * 1024u);
    input[1024u * 1024u] = '\0';
    uint64_t start = snag_monotonic_ms();
    json_t *result = run_command_full("sleep 5", 5000, NULL, input,
                                     steer_after_input, &calls, -1, 6000u);
    free(input);
    assert(snag_monotonic_ms() - start < 1000u);
    assert(!strcmp(snag_json_string(result, "status"), "running"));
    json_t *ref = json_object_get(result, "output_ref");
    assert(json_int_member(ref, "stdin_accepted") == 1024 * 1024);
    assert(json_int_member(ref, "stdin_pending") > 0);
    const char *handle = snag_json_string(result, "handle");
    json_t *rejected = run_write_stdin_call(handle, "duplicate", false, 0);
    assert(!strcmp(snag_json_string(rejected, "reason"), "stdin_busy"));
    json_decref(rejected);
    json_t *closed = run_terminate_call(handle, "", false);
    assert(strcmp(snag_json_string(closed, "status"), "running"));
    assert(json_int_member(json_object_get(closed, "output_ref"), "stdin_pending") == 0);
    json_decref(closed);
    json_decref(result);
}

static void
test_journal_failure_closes_owned_commands(void)
{
    struct snag_config config;
    struct snag_credential credential;
    struct snag_response_graph graph;
    char cwd[4096], handle[SNAG_ID_HEX_LEN + 1u], error[256] = {0};
    snag_config_init(&config);
    snag_credential_clear(&credential);
    assert(getcwd(cwd, sizeof(cwd)));
    for (unsigned int i = 0u; i < 2u; ++i) {
        uint32_t yield;
        json_t *result = NULL;
        make_call(&graph, "printf pending; sleep 5", cwd, 5000, NULL);
        assert(snag_tools_prepare(&graph.items[0], &config, handle, &yield, &result) == 0);
        assert(snag_tools_start(&graph.items[0], &config, &credential, &result, error, sizeof(error)) == 0);
        snag_response_graph_free(&graph);
    }
    fail_output = true;
    uint64_t deadline = snag_monotonic_ms() + 2000u;
    while (snag_tools_service(10, -1, error, sizeof(error)) == 0)
        assert(snag_monotonic_ms() < deadline);
    assert(errno == ENOSPC);
    snag_tools_shutdown();
    assert(!snag_tools_busy());
    fail_output = false;
    snag_config_free(&config);
}

int
main(void)
{
    (void)signal(SIGPIPE, SIG_IGN);
    snag_tools_journal(retain_output, read_output, NULL);
    test_process_capacity_and_ready_collection();
    test_steering_with_blocked_stdin();
    test_journal_failure_closes_owned_commands();
    test_native_read_tools();
    test_success_and_streams();
    test_command_output_limit_selection();
    test_managed_output_ceiling();
    test_command_output_limit_is_required_and_positive();
    test_failure_status();
    test_timeout_hands_off_without_killing();
    test_no_timeout();
    test_timeout_handoff_preserves_process_family();
    test_large_stdout_is_complete_for_model();
    test_binary_stdout_is_complete_for_model();
    test_stdin_uses_blocking_child_fd();
    test_pty_merges_stdout_and_stderr();
    test_managed_pty_write_stdin_completes();
    test_managed_process_write_stdin_completes();
    test_managed_process_accepts_repeated_write_stdin();
    test_managed_process_without_timeout();
    test_managed_process_hands_off_on_steering();
    test_write_stdin_rejects_unknown_handle();
    test_wrong_handle_does_not_touch_active_process();
    test_malformed_interaction_preserves_active_process();
    test_write_stdin_terminates_managed_process();
    test_invalid_termination_preserves_managed_process();
    test_managed_process_close_returns_terminal_result();
    test_managed_close_kills_process_family();
    test_provider_secret_removed_from_environment();
    test_all_provider_secrets_removed_and_redacted();
    test_apply_patch_rejects_null_result();
    test_apply_patch_add_update_delete();
    test_patch_line_endings();
    test_apply_patch_rejects_ambiguous_match();
    test_apply_patch_rejects_path_escape();
    test_apply_patch_rejects_symlink_target();
    test_apply_patch_validates_before_install();
    test_apply_patch_preview_is_bounded();
    test_provider_secret_redacted_from_output();
    test_provider_secret_redacted_across_read_boundary();
    puts("test_tools: ok");
    snag_tools_shutdown();
    for (size_t i = 0u; i < output_count; ++i)
        for (unsigned int s = 0u; s < 2u; ++s)
            snag_buf_free(&output_journal[i].streams[s]);
    return 0;
}
