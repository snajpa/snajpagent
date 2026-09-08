/* SPDX-License-Identifier: GPL-2.0-only */
#include "checked_json.h"
#include "base.h"
#include "config.h"
#include "credential.h"
#include "secret.h"
#include "json.h"
#include "tools.h"
#include "tools_patch.h"
#include "process_host.h"
#include "turn.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <signal.h>
#include <stdatomic.h>
#include <pthread.h>
#include "snag_jansson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static _Atomic uint64_t atomic_sequence;

static void *
advance_atomic_sequence(void *unused)
{
    (void)unused;
    for (unsigned int i = 0; i < 1024u; ++i)
        atomic_fetch_add(&atomic_sequence, 1u);
    return NULL;
}

static void
test_atomic_sequence(void)
{
    const uint64_t initial = UINT64_C(0x12345678fffffc00);
    pthread_t thread;
    void *result;
    atomic_store(&atomic_sequence, initial);
    assert(pthread_create(&thread, NULL, advance_atomic_sequence, NULL) == 0);
    (void)advance_atomic_sequence(NULL);
    assert(pthread_join(thread, &result) == 0 && result == NULL);
    assert(atomic_load(&atomic_sequence) == initial + 2048u);
    assert(atomic_exchange(&atomic_sequence, 0u) == initial + 2048u);
    assert(atomic_load(&atomic_sequence) == 0u);
}

static void
test_child_wait_ownership(void)
{
    struct snag_child child;
    char *environment[] = {"PATH=/usr/bin:/bin", NULL};
    snag_child_init(&child);
    assert(snag_child_spawn(&child, "/bin/sh", "exit 7", "/", environment, false) == 0);
    uint64_t deadline = snag_monotonic_ms() + 1000u;
    int exited;
    while ((exited = snag_child_exited(&child)) == 0 && snag_monotonic_ms() < deadline)
        assert(snag_sleep_ms(1u) == 0);
    assert(exited == 1 && snag_child_exited(&child) == 1 && !child.reaped);
    int status;
    assert(waitpid(child.pid, &status, 0) == child.pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 7);
    assert(snag_child_exited(&child) < 0 && errno == ECHILD && child.reaped);
    snag_child_free(&child);
    child.pid = getpid();
    assert(snag_child_exited(&child) < 0 && errno == ECHILD && child.reaped);
    snag_child_free(&child);
}

static void
test_child_interrupt_mask(void)
{
    sigset_t blocked, saved, current;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    assert(sigprocmask(SIG_BLOCK, &blocked, &saved) == 0);
    for (unsigned int pty = 0; pty < 2u; ++pty) {
        struct snag_child child;
        char *environment[] = {"PATH=/usr/bin:/bin", NULL};
        snag_child_init(&child);
        assert(snag_child_spawn(&child, "/bin/sh", "printf ready; exec sleep 30", "/",
                                environment, pty != 0) == 0);
        struct snag_child_event event = {&child, 0u, SNAG_CHILD_READ, 0};
        assert(snag_child_wait(&event, 1u, SNAG_WAKE_INVALID, 1000) > 0);
        char bytes[32];
        assert(snag_child_read(&child, 0u, bytes, sizeof(bytes)) > 0);
        uint64_t start = snag_monotonic_ms();
        snag_child_signal(&child, SNAG_CHILD_INTERRUPT);
        while (snag_child_exited(&child) == 0 && snag_monotonic_ms() - start < 1000u)
            assert(snag_sleep_ms(1u) == 0);
        assert(snag_child_exited(&child) == 1 && snag_child_reap(&child) == 0);
        assert(child.signal_number == SIGINT);
        snag_child_free(&child);
    }
    assert(sigprocmask(SIG_SETMASK, NULL, &current) == 0 && sigismember(&current, SIGINT));
    assert(sigprocmask(SIG_SETMASK, &saved, NULL) == 0);
}

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
    if (!len)
        return 0;
    if (len <= out->max)
        return snag_buf_append(out, source->data + from, len);
    size_t head = out->max / 2u, tail = out->max - head;
    return snag_buf_append(out, source->data + from, head) < 0 ||
           snag_buf_append(out, source->data + to - tail, tail) < 0 ? -1 : 0;
}

static json_t *
close_command(const char *handle)
{
    json_t *result = NULL;
    char error[256] = {0};
    int rc = snag_tools_close_managed(handle, false, NULL, NULL, -1,
                                     &result, error, sizeof(error));
    if (rc == 0)
        snag_tools_collected(handle);
    if (rc != 0)
        fprintf(stderr, "close command: %s\n", error);
    assert(rc == 0 && result && snag_tool_result_valid(result) == 0);
    return result;
}

static json_t *
call_args_yield(const char *command, const char *workdir, int timeout_ms,
                int yield_ms, const char *stdin_text)
{
    return checked_json(json_pack("{s:s,s:s,s:o,s:I,s:n,s:o}",
        "command", command, "workdir", workdir,
        "timeout_ms", timeout_ms < 0 ? json_null() : json_integer(timeout_ms),
        "yield_ms", (json_int_t)(yield_ms), "max_output_tokens",
        "stdin", stdin_text ? json_string(stdin_text) : json_null()));
}

static void
make_call(struct snag_response_graph *graph, const char *command,
          const char *workdir, int timeout_ms, const char *stdin_text)
{
    json_t *args = call_args_yield(command, workdir, timeout_ms, 0, stdin_text);
    assert(args != NULL);
    assert(snag_json_set_new(args, "pty", json_false()) == 0);
    *graph = (struct snag_response_graph){0};
    assert(snag_response_graph_set_provider_id(graph, "resp_tool_test") == 0);
    assert(snag_response_graph_add_call(graph, "item_tool_test",
                                       "call_tool_test", "exec_command",
                                       args) == 0);
}

static json_t *
run_call(struct snag_response_graph *graph, struct snag_config *config,
         const char *workspace, const char *secret, snag_tool_pump_fn pump, void *opaque)
{
    struct snag_credential credential;
    struct snag_response_item call = snag_response_graph_item(graph, 0u);
    json_t *result = NULL;
    char error[256] = {0};

    snag_credential_clear(&credential);
    if (secret) {
        credential.len = strlen(secret);
        assert(credential.len <= SNAG_CREDENTIAL_MAX);
        memcpy(credential.value, secret, credential.len + 1u);
    }
    int rc = snag_tools_run(&call, config, &credential, workspace,
                            pump, opaque, -1, &result, error, sizeof(error));
    if (rc != 0)
        fprintf(stderr, "%s tool error: %s errno=%d\n", call.name, error, errno);
    assert(rc == 0);
    assert(result != NULL);
    assert(snag_tool_result_valid(result) == 0);
    snag_response_graph_free(graph);
    snag_config_free(config);
    return result;
}

static json_t *
run_command_full(const char *command, int timeout_ms, const char *secret,
                 const char *stdin_text, snag_tool_pump_fn pump,
                 void *pump_opaque, int selected_limit,
                 uint32_t ceiling)
{
    char cwd[4096];
    struct snag_config config;
    struct snag_response_graph graph;

    assert(getcwd(cwd, sizeof(cwd)) != NULL);
    snag_config_init(&config);
    assert(config.shell != NULL);
    config.default_timeout_ms = 0;
    config.max_timeout_ms = 5000;
    config.max_output_tokens = ceiling;
    make_call(&graph, command, cwd, timeout_ms, stdin_text);
    struct snag_response_item call = snag_response_graph_item(&graph, 0u);
    if (selected_limit >= 0)
        assert(json_object_set_new(call.arguments,
            "max_output_tokens", json_integer(selected_limit)) == 0);
    json_t *result = run_call(&graph, &config, cwd, secret, pump, pump_opaque);
    assert(json_integer_value(json_object_get(result,
               "max_output_tokens")) ==
           (selected_limit >= 0 && (uint32_t)selected_limit < ceiling ?
            (uint32_t)selected_limit : ceiling));
    return result;
}

static json_t *
run_tool_with_wait(const char *name, json_t *args,
                   snag_tool_pump_fn pump, void *pump_opaque, uint32_t max_wait_ms)
{
    char cwd[4096];
    struct snag_config config;

    assert(getcwd(cwd, sizeof(cwd)) != NULL);
    snag_config_init(&config);
    config.default_timeout_ms = 1000;
    config.default_yield_ms = 1000;
    config.max_wait_ms = max_wait_ms;
    config.max_timeout_ms = 5000;
    struct snag_response_graph graph = {0};
    assert(snag_response_graph_set_provider_id(&graph, "resp_managed_test") == 0);
    assert(snag_response_graph_add_call(&graph, "item_managed_test",
                                       "call_managed_test", name, args) == 0);
    return run_call(&graph, &config, cwd, NULL, pump, pump_opaque);
}

static json_t *
run_tool_with_args(const char *name, json_t *args)
{
    return run_tool_with_wait(name, args, NULL, NULL, 60000u);
}

static json_t *
run_managed_exec(const char *command, int timeout_ms, int yield_ms)
{
    char cwd[4096];
    json_t *args;

    assert(getcwd(cwd, sizeof(cwd)) != NULL);
    args = call_args_yield(command, cwd, timeout_ms, yield_ms, NULL);
    assert(snag_json_set_new(args, "pty", json_false()) == 0);
    return run_tool_with_args("exec_command", args);
}

static json_t *
run_write_stdin_call(const char *handle, const char *data, bool eof,
                     int yield_ms, int max_output_tokens)
{
    json_t *args = checked_json(json_pack("{s:s,s:s,s:o,s:b,s:I,s:o}",
        "handle", handle, "data", data, "eof", eof ? json_true() : json_false(), "terminate", 0,
        "yield_ms", (json_int_t)(yield_ms),
        "max_output_tokens", max_output_tokens < 0 ? json_null() : json_integer(max_output_tokens)));
    return run_tool_with_args("write_stdin", args);
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
    const char *handle;
    uint64_t started = snag_time_ms();
    bool requested = false;

    assert(getcwd(cwd, sizeof(cwd)) != NULL);
    args = call_args_yield("sleep 2", cwd, 4000, 0, NULL);
    assert(snag_json_set_new(args, "pty", json_false()) == 0);
    result = run_tool_with_wait("exec_command", args,
                                handoff_once_pump, &requested, 60000u);
    assert(requested);
    assert(snag_time_ms() - started < 1000u);
    assert(strcmp(snag_json_string(result, "status"), "running") == 0);
    assert(strcmp(snag_json_string(result, "reason"),
                  "steering_handoff") == 0);
    assert(strstr(snag_json_string(result, "model_text"),
                  "steering arrived") != NULL);
    handle = snag_json_string(result, "handle");
    assert(handle != NULL);
    json_t *closed = close_command(handle);
    assert(json_integer_value(json_object_get(closed,
               "max_output_tokens")) == 6000);
    json_decref(closed);
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

    assert(handle != NULL);
    for (size_t i = 0u; i < sizeof(requests) / sizeof(requests[0]); ++i) {
        json_t *result = run_write_stdin_call(handle, "", false, 1,
                                               requests[i]);
        assert(strcmp(snag_json_string(result, "status"), "running") == 0);
        assert(json_integer_value(json_object_get(result, "max_output_tokens")) ==
               (requests[i] == 42 ? 42 : 6000));
        json_decref(result);
    }
    json_t *closed = close_command(handle);
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
    struct snag_response_item call = snag_response_graph_item(&graph, 0u);
    for (unsigned int i = 0u; i < 2u; ++i) {
        if (!i)
            assert(json_object_del(call.arguments, "max_output_tokens") == 0);
        else
            assert(json_object_set_new(call.arguments, "max_output_tokens", json_integer(0)) == 0);
        result = NULL;
        assert(snag_tools_run(&call, &config, &credential, cwd,
                             NULL, NULL, -1, &result, error, sizeof(error)) == 0);
        assert(!strcmp(snag_json_string(result, "status"), "not_run"));
        json_decref(result);
        result = snag_tool_result_terminal(false, "invalid arguments");
        assert(result != NULL);
        config.max_output_tokens = 123u;
        assert(snag_tools_attach_output_limit(&call, &config, result) == 0);
        assert(json_integer_value(json_object_get(result, "max_output_tokens")) == 123);
        json_decref(result);
    }
    snag_response_graph_free(&graph);
    snag_config_free(&config);
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
    next = run_write_stdin_call(handle, "one\n", false, 50, -1);
    assert(strcmp(snag_json_string(next, "status"), "running") == 0);
    done = run_write_stdin_call(handle, "two\n", true, 5000, -1);
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
test_wait_limit_and_pending_termination(void)
{
    char cwd[4096], handle[SNAG_ID_HEX_LEN + 1u], error[256] = {0};
    assert(getcwd(cwd, sizeof(cwd)) != NULL);
    /* Ignore TERM so the test observes an actually pending termination. */
    json_t *args = call_args_yield("trap '' TERM; printf ready; while :; do sleep 1; done",
                                 cwd, 5000, 0, NULL);
    assert(snag_json_set_new(args, "pty", json_false()) == 0);
    json_t *result = run_tool_with_wait("exec_command", args, NULL, NULL, 100u);
    assert(!strcmp(snag_json_string(result, "status"), "running"));
    assert(!strcmp(snag_json_string(result, "reason"), "wait_timeout"));
    assert(strstr(snag_json_string(result, "model_text"), "max_wait_ms=100"));
    assert(snag_strcpy(handle, sizeof(handle), snag_json_string(result, "handle")));
    json_decref(result);
    /* Repeated waits reset the budget and preserve the same process. */
    args = json_pack("{s:s,s:s,s:b,s:b,s:i}", "handle", handle, "data", "",
                     "eof", 0, "terminate", 0, "yield_ms", 600000);
    assert(snag_json_set_new(args, "max_output_tokens", json_null()) == 0);
    result = run_tool_with_wait("write_stdin", args, NULL, NULL, 100u);
    assert(!strcmp(snag_json_string(result, "reason"), "wait_timeout"));
    assert(!strcmp(snag_json_string(result, "handle"), handle));
    json_decref(result);
    args = json_pack("{s:s,s:s,s:b,s:b,s:i}", "handle", handle, "data", "",
                     "eof", 0, "terminate", 1, "yield_ms", 0);
    assert(snag_json_set_new(args, "max_output_tokens", json_null()) == 0);
    uint64_t began = snag_monotonic_ms();
    result = run_tool_with_wait("write_stdin", args, NULL, NULL, 100u);
    assert(snag_monotonic_ms() - began < 1500u);
    assert(!strcmp(snag_json_string(result, "status"), "running"));
    assert(!strcmp(snag_json_string(result, "reason"), "wait_timeout"));
    assert(!strcmp(snag_json_string(result, "handle"), handle));
    assert(strstr(snag_json_string(result, "model_text"), "Termination was already requested"));
    json_decref(result);
    /* An operator handoff wins over the timer while closing. */
    assert(snag_tools_collect(handle, "operator_yield", &result, error, sizeof(error)) == 0);
    assert(snag_tool_result_valid(result) == 0);
    assert(!strcmp(snag_json_string(result, "reason"), "operator_yield"));
    assert(strstr(snag_json_string(result, "model_text"), "operator requested /yield"));
    assert(strstr(snag_json_string(result, "model_text"), "Termination was already requested"));
    json_decref(result);
    snag_tools_collected(handle);
    assert(snag_tools_close_managed(handle, false, NULL, NULL, -1,
                                   &result, error, sizeof(error)) == 0);
    assert(strcmp(snag_json_string(result, "status"), "running"));
    json_decref(result);
}

static void
test_managed_process_close_returns_terminal_result(void)
{
    json_t *result = run_managed_exec(
        "printf 'ready\n'; sleep 5",
        5000, 100);
    const char *handle;
    const char *status;

    assert(strcmp(snag_json_string(result, "status"), "running") == 0);
    handle = snag_json_string(result, "handle");
    assert(handle != NULL && snag_hex_is_lower(handle, SNAG_ID_HEX_LEN));
    json_t *closed = close_command(handle);
    assert(json_integer_value(json_object_get(closed,
               "max_output_tokens")) == 6000);
    status = snag_json_string(closed, "status");
    assert(strcmp(status, "running") != 0);
    assert(json_is_null(json_object_get(closed, "handle")));
    json_decref(closed);
    json_decref(result);
}

static void
test_managed_close_kills_process_family(void)
{
    char dir[] = "/tmp/snajpagent-patch-test-XXXXXX";
    char marker[4096];
    char command[8192];
    json_t *result;
    const char *handle;

    assert(mkdtemp(dir) != NULL);
    int n = snprintf(marker, sizeof(marker), "%s/%s", dir, "managed-leaked.txt");
    assert(n > 0 && (size_t)n < sizeof(marker));
    assert(snprintf(command, sizeof(command),
                    "(sleep 0.25; printf leaked > '%s') & wait",
                    marker) > 0);
    result = run_managed_exec(command, 5000, 50);
    assert(strcmp(snag_json_string(result, "status"), "running") == 0);
    handle = snag_json_string(result, "handle");
    assert(handle != NULL && snag_hex_is_lower(handle, SNAG_ID_HEX_LEN));
    json_t *closed = close_command(handle);
    sleep_ms(500);
    assert(access(marker, F_OK) < 0 && errno == ENOENT);
    json_decref(closed);
    json_decref(result);
    assert(rmdir(dir) == 0);
}

static void
test_provider_secret_redacted(const char *command)
{
    json_t *result = run_command_full(command, 1000, "secret-value-for-test",
                                      NULL, NULL, NULL, -1, 6000u);
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
    result = run_command_full("printf ${OPENAI_API_KEY-unset}", 1000,
                              NULL, NULL, NULL, NULL, -1, 6000u);
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
    snag_config_provider_init(&config.providers[1], "second");
    config.provider_count = 2u;
    assert(snprintf(config.providers[1].name,
                    sizeof(config.providers[1].name), "second") > 0);
    assert(snag_secret_source_parse(&config.providers[1].api_key, "${SECOND_PROVIDER_KEY}",
                                    NULL, error, sizeof(error)) == 0);
    assert(setenv("SECOND_PROVIDER_KEY", "second-provider-secret", 1) == 0);
    snag_credential_clear(&credential);
    make_call(&graph,
              "printf \"${SECOND_PROVIDER_KEY-unset}:second-provider-secret\"",
              cwd, 1000, NULL);
    struct snag_response_item call = snag_response_graph_item(&graph, 0u);
    assert(snag_tools_run(&call, &config, &credential, cwd,
                         NULL, NULL, -1, &result, error, sizeof(error)) == 0);
    retained = snag_json_string(json_object_get(result, "stdout"), "retained");
    assert(strcmp(retained, "unset:<redacted:secret>") == 0);
    assert(unsetenv("SECOND_PROVIDER_KEY") == 0);
    json_decref(result);
    snag_response_graph_free(&graph);
    snag_config_free(&config);
}

static void
test_secret_snapshot_rotation(void)
{
    struct snag_config config;
    struct snag_secret_set secrets = {0};
    char error[256] = {0};
    static const char body[] = "{\"text\":\"old-protected new-protected literal-protected\"}";

    snag_config_init(&config);
    config.secret_count = 2u;
    assert(snag_secret_source_parse(&config.secrets[0], "${SNAG_ROTATING_SECRET}", NULL, error, sizeof(error)) == 0);
    assert(snag_secret_source_parse(&config.secrets[1], "\"literal-protected\"", NULL, error, sizeof(error)) == 0);
    assert(setenv("SNAG_ROTATING_SECRET", "old-protected", 1) == 0);
    assert(snag_secret_set_build(&secrets, &config, NULL, error, sizeof(error)) == 0);
    assert(setenv("SNAG_ROTATING_SECRET", "new-protected", 1) == 0);
    assert(snag_secret_set_build(&secrets, &config, NULL, error, sizeof(error)) == 0);
    snag_config_free(&config);
    assert(unsetenv("SNAG_ROTATING_SECRET") == 0);
    struct snag_buf result = {.max = 4096u};
    assert(snag_wire_json_redact((const unsigned char *)body, strlen(body), &secrets.wire,
                                &result, error, sizeof(error)) == 0);
    assert(snag_buf_terminate(&result) == 0);
    assert(!strstr((const char *)result.data, "old-protected"));
    assert(!strstr((const char *)result.data, "new-protected"));
    assert(!strstr((const char *)result.data, "literal-protected"));
    {
        json_t *native = snag_tool_result_terminal(true, "old-protected literal-protected");
        assert(native && snag_secret_result(&secrets, native, error, sizeof(error)) == 0);
        assert(strcmp(snag_json_string(native, "status"), "succeeded") == 0);
        assert(strstr(snag_json_string(native, "model_text"), "<redacted:secret>"));
        assert(!strstr(snag_json_string(native, "model_text"), "old-protected"));
        json_decref(native);
    }
    snag_buf_free(&result);
    snag_secret_set_free(&secrets);
    snag_config_init(&config);
    config.secret_count = 1u;
    assert(snag_secret_source_parse(&config.secrets[0], "${SNAG_ROTATING_SECRET}", NULL, error, sizeof(error)) == 0);
    assert(snag_secret_set_build(&secrets, &config, NULL, error, sizeof(error)) < 0);
    snag_secret_set_free(&secrets);
    snag_config_free(&config);
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
        struct snag_response_item call = snag_response_graph_item(&graph, 0u);
        assert(snag_tools_prepare(&call, &config, handles[i], &yield, &result) == 0);
        assert(snag_tools_start(&call, &config, &credential, &result, error, sizeof(error)) == 0);
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
    struct snag_response_item call = snag_response_graph_item(&graph, 0u);
    assert(snag_tools_prepare(&call, &config, unused, &yield, &result) == 1);
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
    json_t *rejected = run_write_stdin_call(handle, "duplicate", false, 0, -1);
    assert(!strcmp(snag_json_string(rejected, "reason"), "stdin_busy"));
    json_decref(rejected);
    json_t *closed = run_tool_with_args("write_stdin",
        checked_json(json_pack("{s:s,s:s,s:b,s:b,s:i,s:n}",
            "handle", handle, "data", "", "eof", 0, "terminate", 1,
            "yield_ms", 0, "max_output_tokens")));
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
        struct snag_response_item call = snag_response_graph_item(&graph, 0u);
        assert(snag_tools_prepare(&call, &config, handle, &yield, &result) == 0);
        assert(snag_tools_start(&call, &config, &credential, &result, error, sizeof(error)) == 0);
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
    test_atomic_sequence();
    test_child_wait_ownership();
    test_child_interrupt_mask();
    (void)signal(SIGPIPE, SIG_IGN);
    snag_tools_journal(retain_output, read_output, NULL);
    test_process_capacity_and_ready_collection();
    test_steering_with_blocked_stdin();
    test_journal_failure_closes_owned_commands();
    test_command_output_limit_selection();
    test_managed_output_ceiling();
    test_command_output_limit_is_required_and_positive();
    test_stdin_uses_blocking_child_fd();
    test_managed_process_hands_off_on_steering();
    test_wait_limit_and_pending_termination();
    test_managed_process_accepts_repeated_write_stdin();
    test_managed_process_close_returns_terminal_result();
    test_managed_close_kills_process_family();
    test_provider_secret_removed_from_environment();
    test_all_provider_secrets_removed_and_redacted();
    test_secret_snapshot_rotation();
    test_apply_patch_rejects_null_result();
    test_provider_secret_redacted("printf secret-value-for-test");
    test_provider_secret_redacted("printf '%8190ssecret-value-for-test' ''");
    puts("test_tools: ok");
    snag_tools_shutdown();
    for (size_t i = 0u; i < output_count; ++i)
        for (unsigned int s = 0u; s < 2u; ++s)
            snag_buf_free(&output_journal[i].streams[s]);
    return 0;
}
