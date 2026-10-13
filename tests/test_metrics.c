/* SPDX-License-Identifier: GPL-2.0-only */
#include "metrics.h"
#include "base.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void
make_id(char id[SNAG_ID_HEX_LEN + 1u])
{
    for (size_t i = 0u; i < SNAG_ID_HEX_LEN; ++i) id[i] = "0123456789abcdef"[i % 16u];
    id[SNAG_ID_HEX_LEN] = '\0';
}

int
main(void)
{
    char directory[] = "/tmp/snajpagent-metrics-XXXXXX";
    char path[512], error[256] = {0};
    struct snag_session session;
    struct snag_usage_totals process = {
        .responses = 2u, .input_tokens = 100u, .output_tokens = 200u,
        .reasoning_tokens = 30u, .total_tokens = 330u};
    struct snag_metrics_sample sample;
    struct snag_metrics_view view;
    struct stat info;
    mode_t old_mask;
    int root;
    int n;

    assert(mkdtemp(directory));
    root = open(directory, O_RDONLY | O_DIRECTORY);
    assert(root >= 0);
    memset(&session, 0, sizeof(session));
    make_id(session.id);
    session.turn_count = 4u;
    session.usage_totals = (struct snag_usage_totals){
        .responses = 7u, .input_tokens = 500u, .output_tokens = 600u,
        .reasoning_tokens = 70u, .total_tokens = 1170u};
    session.active_accounting.input_tokens = 100u;
    sample = (struct snag_metrics_sample){
        .session = &session,
        .program_usage = &process,
        .provider = "fixture",
        .model = "model/test",
        .active_turn_id = "abcdefabcdefabcdefabcdefabcdefab",
        .state = "attached",
        .process_started_ms = 1000u,
        .process_pid = 1234u,
        .observed_ms = 6000u,
        .turn_started_ms = 2000u,
        .turn_output_tokens = 40u,
        .response_started_ms = 3000u,
        .last_response_ms = 450u,
        .last_response_output_tokens = 50u,
        .turn_active = true,
        .response_open = true};
    old_mask = umask(077u);
    assert(snag_metrics_write(root, &sample, error, sizeof(error)) == 0);
    (void)umask(old_mask);
    assert(snag_metrics_read(root, session.id, &view, error, sizeof(error)) == 0);
    assert(view.valid);
    assert(!strcmp(view.provider, "fixture"));
    assert(!strcmp(view.model, "model/test"));
    assert(!strcmp(view.state, "attached"));
    assert(!strcmp(view.active_turn_id, sample.active_turn_id));
    assert(view.turns == 4u);
    assert(view.process_total_tokens == 330u);
    assert(view.session_total_tokens == 1170u);
    assert(view.prefill_microtokens_per_second == 100000000u);
    assert(view.decode_microtokens_per_second == 13333333u);
    assert(view.process_uptime_ms == 5000u);
    assert(view.process_pid == 1234u);
    assert(view.turn_active && view.response_open);
    assert(snag_metrics_usage_total(&(struct snag_usage_totals){
               .input_tokens = 2u, .output_tokens = 3u, .reasoning_tokens = 4u}) == 9u);
    n = snprintf(path, sizeof(path), "%s/metrics/%s.prom", directory, session.id);
    assert(n > 0 && (size_t)n < sizeof(path));
    assert(stat(path, &info) == 0);
    assert((info.st_mode & 0777u) == 0600u);
    assert(access(path, R_OK) == 0);
    n = snprintf(path, sizeof(path), "%s/metrics/%s.tmp", directory, session.id);
    assert(n > 0 && (size_t)n < sizeof(path));
    assert(access(path, F_OK) < 0);
    assert(close(root) == 0);
    n = snprintf(path, sizeof(path), "%s/metrics/%s.prom", directory, session.id);
    assert(n > 0 && (size_t)n < sizeof(path));
    assert(unlink(path) == 0);
    n = snprintf(path, sizeof(path), "%s/metrics", directory);
    assert(n > 0 && (size_t)n < sizeof(path));
    assert(rmdir(path) == 0);
    assert(rmdir(directory) == 0);
    puts("test_metrics: ok");
    return 0;
}
