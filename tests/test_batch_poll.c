/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"
#include "base.h"
#include "config.h"
#include "json.h"
#include "tools.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif

#ifndef _WIN32
struct finish_probe {
    const char *call_id;
    bool seen;
};

static int
finished_result(void *opaque, const struct snag_session *state, uint64_t seq, const char *type,
    const json_t *data, char *error, size_t error_size)
{
    struct finish_probe *probe = opaque;
    const char *call_id;
    (void)state;
    (void)seq;
    (void)error;
    (void)error_size;
    if (strcmp(type, "tool_finished") ||
        !(call_id = snag_json_string(data, "call_id")) || strcmp(call_id, probe->call_id))
        return 0;
    json_t *result = json_object_get(data, "result");
    json_t *ref = json_object_get(result, "output_ref");
    const char *text = snag_json_string(result, "model_text");
    uint64_t exit_code, start, end;
    assert(!probe->seen && !strcmp(snag_json_string(result, "status"), "succeeded"));
    assert(!snag_json_integer_u64(result, "exit_code", &exit_code) && exit_code == 0u);
    assert(text && strstr(text, "poll-ok"));
    assert(ref && !snag_json_integer_u64(ref, "stdout_start", &start) && start == 0u);
    assert(!snag_json_integer_u64(ref, "stdout_end", &end) && end == 7u);
    probe->seen = true;
    return 0;
}

static bool projection_closes_input;
static unsigned int projection_calls;

static int
cancelled_request_build(struct app_state *app, const json_t *steering, unsigned int cycle,
    const struct snag_credential *credential, struct snag_context_projection *projection,
    const char **count_method, struct snag_buf *request_body, char *error, size_t error_size)
{
    (void)steering;
    (void)cycle;
    (void)credential;
    (void)projection;
    (void)count_method;
    (void)request_body;
    ++projection_calls;
    app->input_closed = projection_closes_input;
    return snag_fail(error, error_size, ECANCELED, "request projection cancelled");
}

/* Force the projection boundary while keeping turn execution and its journal real. */
#define snag_app_request_build cancelled_request_build
#include "../src/app.c"
#undef snag_app_request_build

struct shutdown_probe {
    unsigned int failed, recovered, interrupted;
};

static int
shutdown_result(void *opaque, const struct snag_session *state, uint64_t seq, const char *type,
    const json_t *data, char *error, size_t error_size)
{
    struct shutdown_probe *probe = opaque;
    (void)state;
    (void)seq;
    (void)error;
    (void)error_size;
    if (!strcmp(type, "turn_failed")) {
        assert(!strcmp(snag_json_string(data, "class"), "context"));
        ++probe->failed;
    } else if (!strcmp(type, "turn_recovery")) ++probe->recovered;
    else if (!strcmp(type, "turn_interrupted")) ++probe->interrupted;
    return 0;
}

static void
start_process_response(struct app_state *app, const char *turn, const char *response,
    unsigned int cycle, const struct snag_response_graph *graph)
{
    char error[512] = "";
    const char *hash = "0000000000000000000000000000000000000000000000000000000000000000";
    assert(!snag_session_commit(&app->session, "response_started",
        json_pack("{s:i,s:n,s:s,s:n,s:s,s:s,s:s,s:i,s:s,s:n,s:i,s:s,s:i,s:s,s:i,s:i,s:s,"
                  "s:s,s:s,s:s,s:s,s:n,s:s,s:b,s:[],s:s}",
            "irc_seq", 0, "baseline_sha256", "capability_version", SNAJPAGENT_CAPABILITY_VERSION,
            "compact_id", "count_method", "exact", "capacity_source", "unknown",
            "count_request_sha256", hash, "cycle", (int)cycle, "effort", "default",
            "hard_input_tokens",
            "input_tokens_bound", 1000, "model", "fixture", "model_input_bytes", 4000,
            "model_input_sha256", hash, "request_input_bytes", 3000, "request_input_count", 1,
            "request_input_sha256", hash, "profile_id", SNAJPAGENT_PROFILE_ID, "provider",
            "default", "provider_source_sha256", hash, "request_sha256", hash,
            "requested_output_tokens", "response_id", response, "source_bound", 0,
            "steering_ids", "turn_id", turn), NULL, error, sizeof(error)));
    assert(!snag_session_commit(&app->session, "response_completed",
        json_pack("{s:s,s:s,s:i,s:s,s:s,s:O,s:{s:i,s:i,s:i,s:i}}", "turn_id", turn,
            "response_id", response, "cycle", (int)cycle, "status", "completed",
            "provider_response_id",
            "fixture", "items", graph->items, "usage", "input_tokens", 17, "output_tokens", 7,
            "reasoning_tokens", 3, "total_tokens", 24), NULL, error, sizeof(error)));
}

static void
test_process_poll_and_projection_shutdown(bool close_input)
{
    char path[4096], error[512] = "";
    const char *tmp = getenv("TMPDIR");
    assert(snprintf(path, sizeof(path), "%s/snajpagent-batch-poll-XXXXXX", tmp ? tmp : "/tmp") > 0);
    assert(mkdtemp(path));
    long size = sysconf(_SC_PAGESIZE);
    assert(size > 0);
    size_t page_size = (size_t)size;
    size_t mapping_size = (sizeof(struct snag_config) + page_size - 1u) / page_size * page_size;
    struct snag_config *config = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(config != MAP_FAILED);
    snag_config_init(config);
    config->read_agents_md = false;
    config->retry_auto = false;
    assert(snag_strcpy(config->providers[0].name, sizeof(config->providers[0].name), "default"));
    snag_secret_source_free(&config->providers[0].api_key);
    assert(!snag_secret_source_parse(&config->providers[0].api_key, "\"fixture-only\"", NULL,
        error, sizeof(error)));
    size_t offset = (offsetof(struct snag_config, source_path) + page_size - 1u) /
        page_size * page_size;
    assert(offset < offsetof(struct snag_config, source_path) + sizeof(config->source_path));
    void *config_page = (unsigned char *)config + offset;
    struct snag_cli cli = {0};
    struct app_state app = {.config = config, .cli = &cli, .execute = true};
    snag_store_init(&app.store);
    snag_session_init(&app.session);
    assert(!snag_store_open(&app.store, path, error, sizeof(error)));
    assert(!snag_session_create(&app.store, &app.session, path, "default", "fixture", "default",
        error, sizeof(error)));
    assert(!snag_ui_init(&app.ui));
    snag_tools_journal(snag_app_tool_output, snag_app_tool_read, &app);
    const char *turn = "11111111111111111111111111111111";
    const char *response = "22222222222222222222222222222222";
    assert(!snag_session_commit(&app.session, "turn_started",
        json_pack("{s:s,s:i,s:s,s:n,s:n,s:s,s:s,s:b,s:[],s:{s:s,s:s,s:s,s:i,s:b}}", "turn_id",
            turn, "turn_number", 1, "input_kind", "direct", "queue_id", "queue_seq", "cwd",
            path, "text", "Run a command and collect its output.", "read_only", 0,
            "instructions", "config", "provider", "default", "model", "fixture", "effort",
            "default", "max_parallel_commands", 4, "parallel_tool_calls", 1),
        NULL, error, sizeof(error)));
    struct snag_response_graph graph = {0};
    assert(!snag_response_graph_add_call(&graph, "run", "run", "exec_command",
        json_pack("{s:s,s:i}", "command", "sleep 0.05; printf poll-ok", "yield_ms", 1000)));
    start_process_response(&app, turn, response, 1u, &graph);
    app.session.default_yield_ms = 1000u;
    app.session.max_wait_ms = 1000u;
    struct call_slot call = {.call = snag_response_graph_item(&graph, 0), .process = true};
    /* This unused page belongs to the large config object. Batch execution must
     * consume the narrow session snapshot without reading or copying that page. */
    assert(!mprotect(config_page, page_size, PROT_NONE));
    int rc = run_call_batch(&app, turn, NULL, &call, 1u, error, sizeof(error));
    assert(!mprotect(config_page, page_size, PROT_READ | PROT_WRITE));
    if (rc) (void)fprintf(stderr, "batch: %s\n", error);
    assert(rc == 0 && call.finished && !snag_tools_busy());
    assert(app.session.pending_call_count == 0u && app.session.process_count == 0u);
    struct finish_probe probe = {.call_id = call.call.call_id};
    assert(!snag_session_each_event(&app.session, finished_result, &probe, error, sizeof(error)) &&
           probe.seen);
    snag_response_graph_free(&graph);
    assert(!snag_response_graph_add_call(&graph, "live", "live", "exec_command",
        json_pack("{s:s,s:i}", "command", "sleep 30", "yield_ms", 0)));
    start_process_response(&app, turn, "33333333333333333333333333333333", 2u, &graph);
    call = (struct call_slot){.call = snag_response_graph_item(&graph, 0), .process = true};
    assert(!run_call_batch(&app, turn, NULL, &call, 1u, error, sizeof(error)));
    assert(call.started && app.session.process_count == 1u && snag_tools_busy());
    projection_closes_input = close_input;
    projection_calls = 0u;
    int turn_rc = run_tracked_turn(&app, app.session.active_prompt, NULL, false, false, NULL,
        false);
    assert(projection_calls == 1u);
    assert(turn_rc == (close_input ? 0 : 4));
    struct shutdown_probe shutdown = {0};
    assert(!snag_session_each_event(&app.session, shutdown_result, &shutdown, error,
        sizeof(error)));
    assert(shutdown.failed == (close_input ? 0u : 1u));
    assert(shutdown.recovered == (close_input ? 1u : 0u));
    assert(!shutdown.interrupted && !snag_tools_busy() && !app.session.process_count);
    snag_response_graph_free(&graph);
    snag_tools_shutdown();
    snag_tools_journal(NULL, NULL, NULL);
    snag_ui_free(&app.ui);
    snag_session_close(&app.session);
    snag_store_close(&app.store);
    snag_config_free(config);
    assert(!munmap(config, mapping_size));
}
#endif /* !_WIN32 */

int
main(void)
{
#ifndef _WIN32
    test_process_poll_and_projection_shutdown(false);
    test_process_poll_and_projection_shutdown(true);
    puts("test_batch_poll: ok");
#else
    puts("test_batch_poll: page-read oracle unavailable; skipped");
#endif
    return 0;
}
