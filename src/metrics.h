/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_METRICS_H
#define SNAJPAGENT_METRICS_H

#include "store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SNAG_METRICS_REFRESH_SECONDS SNAG_CONFIG_METRICS_REFRESH_SECONDS

struct snag_metrics_sample {
    const struct snag_session *session;
    const struct snag_usage_totals *program_usage;
    const char *provider;
    const char *model;
    const char *active_turn_id;
    const char *state;
    uint64_t process_started_ms;
    uint64_t process_pid;
    uint64_t observed_ms;
    uint64_t turn_started_ms;
    uint64_t turn_output_tokens;
    uint64_t response_started_ms;
    uint64_t last_response_ms;
    uint64_t last_response_output_tokens;
    bool turn_active;
    bool response_open;
};

struct snag_metrics_view {
    char provider[SNAG_CONFIG_PROVIDER_NAME_MAX + 1u];
    char model[SNAG_MODEL_MAX_BYTES];
    char active_turn_id[SNAG_ID_HEX_LEN + 1u];
    char state[16];
    uint64_t process_started_ms;
    uint64_t process_pid;
    uint64_t observed_ms;
    uint64_t turns;
    uint64_t process_responses;
    uint64_t process_input_tokens;
    uint64_t process_output_tokens;
    uint64_t process_reasoning_tokens;
    uint64_t process_total_tokens;
    uint64_t session_responses;
    uint64_t session_input_tokens;
    uint64_t session_output_tokens;
    uint64_t session_reasoning_tokens;
    uint64_t session_total_tokens;
    uint64_t prefill_microtokens_per_second;
    uint64_t decode_microtokens_per_second;
    uint64_t last_response_ms;
    uint64_t process_uptime_ms;
    bool turn_active;
    bool response_open;
    bool valid;
};

int snag_metrics_write(int root_fd, const struct snag_metrics_sample *, char *, size_t);
int snag_metrics_read(int root_fd, const char *session_id, struct snag_metrics_view *, char *, size_t);
uint64_t snag_metrics_usage_total(const struct snag_usage_totals *);
int snag_metrics_kill(const struct snag_metrics_view *, char *, size_t);

#endif
