/* SPDX-License-Identifier: GPL-2.0-only */
#include "metrics.h"
#include "base.h"
#include "fs.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#define SNAG_METRICS_FILE_MAX (256u * 1024u)

static int
metrics_directory(int root_fd)
{
    int fd;

    if (snag_mkdir_private_at(root_fd, "metrics") < 0 && errno != EEXIST) return -1;
    fd = snag_open_read_security_at(root_fd, "metrics", true);
    return fd;
}

static int
metric_label(struct snag_buf *out, const char *name, const char *value)
{
    if (snag_buf_printf(out, "%s=\"", name) < 0) return -1;
    for (const unsigned char *p = (const unsigned char *)(value ? value : ""); *p; ++p) {
        if (*p == '\\' || *p == '"' || *p == '\n') {
            if (snag_buf_putc(out, '\\') < 0) return -1;
            if (*p == '\n') {
                if (snag_buf_putc(out, 'n') < 0) return -1;
                continue;
            }
        }
        if (snag_buf_putc(out, *p) < 0) return -1;
    }
    return snag_buf_putc(out, '"');
}

static int
metric_header(struct snag_buf *out, const char *name, const char *help, const char *type)
{
    return snag_buf_printf(out, "# HELP %s %s\n# TYPE %s %s\n", name, help, name, type);
}

static int
metric_u64(struct snag_buf *out, const char *name, uint64_t value)
{
    return snag_buf_printf(out, "%s %llu\n", name, (unsigned long long)value);
}

static int
metric_typed_u64(struct snag_buf *out, const char *name, const char *help, const char *type,
    uint64_t value)
{
    return metric_header(out, name, help, type) < 0 || metric_u64(out, name, value) < 0 ? -1 : 0;
}

static int
metric_rate(struct snag_buf *out, const char *name, uint64_t microtokens)
{
    return snag_buf_printf(out, "%s %llu.%06llu\n", name,
        (unsigned long long)(microtokens / 1000000u),
        (unsigned long long)(microtokens % 1000000u));
}

static int
metric_info(struct snag_buf *out, const struct snag_metrics_sample *sample)
{
    const char prefix[] = "snajpagent_session_info{";

    if (snag_buf_append(out, prefix, sizeof(prefix) - 1u) < 0 ||
        metric_label(out, "session_id", sample->session->id) < 0 ||
        snag_buf_putc(out, ',') < 0 || metric_label(out, "provider", sample->provider) < 0 ||
        snag_buf_putc(out, ',') < 0 || metric_label(out, "model", sample->model) < 0 ||
        snag_buf_putc(out, ',') < 0 ||
        metric_label(out, "active_turn_id", sample->active_turn_id) < 0 ||
        snag_buf_putc(out, ',') < 0 || metric_label(out, "state", sample->state) < 0 ||
        snag_buf_append(out, "} 1\n", 4u) < 0)
        return -1;
    return 0;
}

uint64_t
snag_metrics_usage_total(const struct snag_usage_totals *usage)
{
    uint64_t total;

    if (!usage) return 0u;
    if (usage->total_tokens) return usage->total_tokens;
    total = usage->input_tokens;
    if (UINT64_MAX - total >= usage->output_tokens) total += usage->output_tokens;
    else return UINT64_MAX;
    if (UINT64_MAX - total >= usage->reasoning_tokens) total += usage->reasoning_tokens;
    else return UINT64_MAX;
    return total;
}

int
snag_metrics_write(int root_fd, const struct snag_metrics_sample *sample, char *error,
    size_t error_size)
{
    struct snag_buf text = {.max = SNAG_METRICS_FILE_MAX};
    const struct snag_usage_totals *process = sample ? sample->program_usage : NULL;
    const struct snag_usage_totals *session = sample ? &sample->session->usage_totals : NULL;
    uint64_t prefill_elapsed = 0u, decode_elapsed = sample ? sample->last_response_ms : 0u;
    uint64_t prefill_tokens = 0u, decode_tokens = sample ? sample->last_response_output_tokens : 0u;
    uint64_t process_uptime = 0u;
    int metrics_fd = -1, output = -1, rc = -1;
    char temporary[SNAG_ID_HEX_LEN + 32u];
    char path[SNAG_ID_HEX_LEN + sizeof(".prom")];

    if (!sample || !sample->session || !sample->session->id[0] || !sample->program_usage ||
        !sample->provider || !sample->model || !sample->state || root_fd < 0)
        return snag_fail(error, error_size, EINVAL, "invalid metrics sample");
    if (sample->observed_ms >= sample->process_started_ms)
        process_uptime = sample->observed_ms - sample->process_started_ms;
    if (sample->response_started_ms > sample->turn_started_ms) {
        prefill_elapsed = sample->response_started_ms - sample->turn_started_ms;
        prefill_tokens = sample->session->active_accounting.input_tokens;
    }
    if (sample->turn_active && sample->response_open && sample->response_started_ms &&
        sample->observed_ms > sample->response_started_ms) {
        decode_elapsed = sample->observed_ms - sample->response_started_ms;
        decode_tokens = sample->turn_output_tokens;
    }
    metrics_fd = metrics_directory(root_fd);
    if (metrics_fd < 0) {
        (void)snag_errorf(error, error_size, "cannot open metrics directory: %s", strerror(errno));
        goto out;
    }
    (void)snprintf(path, sizeof(path), "%s.prom", sample->session->id);
    (void)snprintf(temporary, sizeof(temporary), "%s.tmp", sample->session->id);
    (void)snag_unlink_at(metrics_fd, temporary, false);
    output = snag_create_output_at(metrics_fd, temporary);
    if (output < 0) {
        (void)snag_errorf(error, error_size, "cannot create metrics file: %s", strerror(errno));
        goto out;
    }
    if (metric_header(&text, "snajpagent_session_info", "Current session identity.", "gauge") < 0 ||
        metric_info(&text, sample) < 0 ||
        metric_typed_u64(&text, "snajpagent_session_turns_total", "Completed session turns.",
            "counter", sample->session->turn_count) < 0 ||
        metric_typed_u64(&text, "snajpagent_session_process_responses_total",
            "Responses reported during this owner process.", "counter", process->responses) < 0 ||
        metric_typed_u64(&text, "snajpagent_session_process_input_tokens_total",
            "Input tokens reported during this owner process.", "counter", process->input_tokens) < 0 ||
        metric_typed_u64(&text, "snajpagent_session_process_output_tokens_total",
            "Output tokens reported during this owner process.", "counter", process->output_tokens) < 0 ||
        metric_typed_u64(&text, "snajpagent_session_process_reasoning_tokens_total",
            "Reasoning tokens reported during this owner process.", "counter",
            process->reasoning_tokens) < 0 ||
        metric_typed_u64(&text, "snajpagent_session_process_total_tokens_total",
            "Total tokens reported during this owner process.", "counter",
            snag_metrics_usage_total(process)) < 0 ||
        metric_typed_u64(&text, "snajpagent_session_responses_total",
            "Responses accumulated by this session.", "counter", session->responses) < 0 ||
        metric_typed_u64(&text, "snajpagent_session_input_tokens_total",
            "Input tokens accumulated by this session.", "counter", session->input_tokens) < 0 ||
        metric_typed_u64(&text, "snajpagent_session_output_tokens_total",
            "Output tokens accumulated by this session.", "counter", session->output_tokens) < 0 ||
        metric_typed_u64(&text, "snajpagent_session_reasoning_tokens_total",
            "Reasoning tokens accumulated by this session.", "counter", session->reasoning_tokens) < 0 ||
        metric_typed_u64(&text, "snajpagent_session_total_tokens_total",
            "Total tokens accumulated by this session.", "counter",
            snag_metrics_usage_total(session)) < 0 ||
        metric_header(&text, "snajpagent_session_prefill_tokens_per_second",
            "Estimated input prefill rate.", "gauge") < 0 ||
        metric_rate(&text, "snajpagent_session_prefill_tokens_per_second",
            snag_rate_microtokens_per_second(prefill_tokens, prefill_elapsed)) < 0 ||
        metric_header(&text, "snajpagent_session_decode_tokens_per_second",
            "Estimated output decode rate.", "gauge") < 0 ||
        metric_rate(&text, "snajpagent_session_decode_tokens_per_second",
            snag_rate_microtokens_per_second(decode_tokens, decode_elapsed)) < 0 ||
        metric_typed_u64(&text, "snajpagent_session_last_response_duration_ms",
            "Duration of the most recent response.", "gauge", sample->last_response_ms) < 0 ||
        metric_typed_u64(&text, "snajpagent_session_process_started_ms",
            "Monotonic owner process start time.", "gauge", sample->process_started_ms) < 0 ||
        metric_typed_u64(&text, "snajpagent_session_process_uptime_ms",
            "Owner process uptime.", "gauge", process_uptime) < 0 ||
        metric_typed_u64(&text, "snajpagent_session_observed_ms",
            "Monotonic sample time.", "gauge", sample->observed_ms) < 0 ||
        metric_typed_u64(&text, "snajpagent_session_process_pid",
            "Owner process id.", "gauge", sample->process_pid) < 0 ||
        metric_typed_u64(&text, "snajpagent_session_turn_active",
            "Whether a turn is active.", "gauge", sample->turn_active ? 1u : 0u) < 0 ||
        metric_typed_u64(&text, "snajpagent_session_response_open",
            "Whether a response stream is open.", "gauge", sample->response_open ? 1u : 0u) < 0 ||
        snag_write_full(output, text.data, text.len) < 0 || snag_fsync(output) < 0 ||
        close(output) < 0)
        goto out;
    output = -1;
    if (snag_rename_at(metrics_fd, temporary, metrics_fd, path) < 0) {
        (void)snag_errorf(error, error_size, "cannot publish metrics file: %s", strerror(errno));
        goto out;
    }
    if (snag_sync_dir(metrics_fd) < 0) {
        (void)snag_errorf(error, error_size, "cannot sync metrics directory: %s", strerror(errno));
        goto out;
    }
    rc = 0;
out:
    if (output >= 0) (void)close(output);
    if (rc < 0) (void)snag_unlink_at(metrics_fd, temporary, false);
    if (metrics_fd >= 0) (void)close(metrics_fd);
    snag_buf_free(&text);
    return rc;
}

static int
parse_u64_metric(const char *line, const char *name, uint64_t *value)
{
    const char *p = line;
    char *end;
    unsigned long long parsed;

    if (strncmp(p, name, strlen(name)) != 0 || p[strlen(name)] != ' ') return 0;
    errno = 0;
    parsed = strtoull(p + strlen(name) + 1u, &end, 10);
    if (errno || end == p + strlen(name) + 1u || (*end && *end != '\n')) return -1;
    *value = (uint64_t)parsed;
    return 1;
}

static int
parse_rate_metric(const char *line, const char *name, uint64_t *value)
{
    const char *p = line;
    char *end;
    uint64_t whole, fraction = 0u;
    size_t digits = 0u;

    if (strncmp(p, name, strlen(name)) != 0 || p[strlen(name)] != ' ') return 0;
    p += strlen(name) + 1u;
    errno = 0;
    whole = strtoull(p, &end, 10);
    if (errno || end == p || (*end != '.' && *end != '\n' && *end != '\0')) return -1;
    if (whole > UINT64_MAX / 1000000u) return -1;
    if (*end == '.') {
        p = end + 1u;
        while (p[digits] >= '0' && p[digits] <= '9' && digits < 6u) {
            fraction = fraction * 10u + (uint64_t)(p[digits] - '0');
            ++digits;
        }
        while (digits < 6u) {
            fraction *= 10u;
            ++digits;
        }
        end = (char *)(p + digits);
    }
    if (*end && *end != '\n') return -1;
    *value = whole * 1000000u + fraction;
    return 1;
}

static int
info_label(const char *line, const char *name, char *out, size_t size)
{
    char needle[64];
    const char *begin, *end;
    size_t length;

    (void)snprintf(needle, sizeof(needle), "%s=\"", name);
    begin = strstr(line, needle);
    if (!begin) return 0;
    begin += strlen(needle);
    end = strchr(begin, '"');
    if (!end || (size_t)(end - begin) >= size) return -1;
    length = (size_t)(end - begin);
    memcpy(out, begin, length);
    out[length] = '\0';
    return 1;
}

int
snag_metrics_read(int root_fd, const char *session_id, struct snag_metrics_view *view, char *error,
    size_t error_size)
{
    int metrics_fd = -1, input = -1, rc = -1;
    snag_file_info info;
    char path[SNAG_ID_HEX_LEN + sizeof(".prom")];
    char *text = NULL;
    ssize_t got;

    if (root_fd < 0 || !session_id || !snag_hex_is_lower(session_id, SNAG_ID_HEX_LEN) || !view)
        return snag_fail(error, error_size, EINVAL, "invalid metrics identity");
    memset(view, 0, sizeof(*view));
    metrics_fd = metrics_directory(root_fd);
    if (metrics_fd < 0) goto out;
    (void)snprintf(path, sizeof(path), "%s.prom", session_id);
    input = snag_open_read_security_at(metrics_fd, path, false);
    if (input < 0) goto out;
    if (snag_fstat(input, &info) < 0 || info.st_size < 0 || (uint64_t)info.st_size > SNAG_METRICS_FILE_MAX)
        goto out;
    text = malloc((size_t)info.st_size + 1u);
    if (!text) goto out;
    got = snag_pread(input, text, (size_t)info.st_size, 0);
    if (got != info.st_size) goto out;
    text[got] = '\0';
    for (char *line = text; line && *line;) {
        char *next = strchr(line, '\n');
        uint64_t value;
        int parsed;
        if (next) *next++ = '\0';
        if ((parsed = info_label(line, "provider", view->provider, sizeof(view->provider))) < 0 ||
            (parsed = info_label(line, "model", view->model, sizeof(view->model))) < 0 ||
            (parsed = info_label(line, "active_turn_id", view->active_turn_id,
                       sizeof(view->active_turn_id))) < 0 ||
            (parsed = info_label(line, "state", view->state, sizeof(view->state))) < 0)
            goto out;
        if ((parsed = parse_u64_metric(line, "snajpagent_session_turns_total", &value)) < 0) goto out;
        if (parsed) view->turns = value;
        if ((parsed = parse_u64_metric(line, "snajpagent_session_process_responses_total", &value)) < 0) goto out;
        if (parsed) view->process_responses = value;
        if ((parsed = parse_u64_metric(line, "snajpagent_session_process_input_tokens_total", &value)) < 0) goto out;
        if (parsed) view->process_input_tokens = value;
        if ((parsed = parse_u64_metric(line, "snajpagent_session_process_output_tokens_total", &value)) < 0) goto out;
        if (parsed) view->process_output_tokens = value;
        if ((parsed = parse_u64_metric(line, "snajpagent_session_process_reasoning_tokens_total", &value)) < 0) goto out;
        if (parsed) view->process_reasoning_tokens = value;
        if ((parsed = parse_u64_metric(line, "snajpagent_session_process_total_tokens_total", &value)) < 0) goto out;
        if (parsed) view->process_total_tokens = value;
        if ((parsed = parse_u64_metric(line, "snajpagent_session_responses_total", &value)) < 0) goto out;
        if (parsed) view->session_responses = value;
        if ((parsed = parse_u64_metric(line, "snajpagent_session_input_tokens_total", &value)) < 0) goto out;
        if (parsed) view->session_input_tokens = value;
        if ((parsed = parse_u64_metric(line, "snajpagent_session_output_tokens_total", &value)) < 0) goto out;
        if (parsed) view->session_output_tokens = value;
        if ((parsed = parse_u64_metric(line, "snajpagent_session_reasoning_tokens_total", &value)) < 0) goto out;
        if (parsed) view->session_reasoning_tokens = value;
        if ((parsed = parse_u64_metric(line, "snajpagent_session_total_tokens_total", &value)) < 0) goto out;
        if (parsed) view->session_total_tokens = value;
        if ((parsed = parse_rate_metric(line, "snajpagent_session_prefill_tokens_per_second", &value)) < 0) goto out;
        if (parsed) view->prefill_microtokens_per_second = value;
        if ((parsed = parse_rate_metric(line, "snajpagent_session_decode_tokens_per_second", &value)) < 0) goto out;
        if (parsed) view->decode_microtokens_per_second = value;
        if ((parsed = parse_u64_metric(line, "snajpagent_session_last_response_duration_ms", &value)) < 0) goto out;
        if (parsed) view->last_response_ms = value;
        if ((parsed = parse_u64_metric(line, "snajpagent_session_process_started_ms", &value)) < 0) goto out;
        if (parsed) view->process_started_ms = value;
        if ((parsed = parse_u64_metric(line, "snajpagent_session_process_uptime_ms", &value)) < 0) goto out;
        if (parsed) view->process_uptime_ms = value;
        if ((parsed = parse_u64_metric(line, "snajpagent_session_observed_ms", &value)) < 0) goto out;
        if (parsed) view->observed_ms = value;
        if ((parsed = parse_u64_metric(line, "snajpagent_session_process_pid", &value)) < 0) goto out;
        if (parsed) view->process_pid = value;
        if ((parsed = parse_u64_metric(line, "snajpagent_session_turn_active", &value)) < 0) goto out;
        if (parsed) view->turn_active = value != 0u;
        if ((parsed = parse_u64_metric(line, "snajpagent_session_response_open", &value)) < 0) goto out;
        if (parsed) view->response_open = value != 0u;
        if (next && *next) line = next;
        else break;
    }
    view->valid = true;
    rc = 0;
out:
    if (input >= 0) (void)close(input);
    if (metrics_fd >= 0) (void)close(metrics_fd);
    free(text);
    if (rc < 0 && error_size && !error[0])
        (void)snag_errorf(error, error_size, "cannot read metrics file: %s", strerror(errno));
    return rc;
}

int
snag_metrics_kill(const struct snag_metrics_view *view, char *error, size_t error_size)
{
#ifdef _WIN32
    (void)view;
    return snag_fail(error, error_size, ENOTSUP, "killing sessions from top is unavailable on Windows");
#else
    if (!view || !view->valid || !view->process_pid)
        return snag_fail(error, error_size, ESRCH, "session has no live owner process");
    if (view->process_pid > (uint64_t)INT_MAX)
        return snag_fail(error, error_size, EINVAL, "session owner process id is invalid");
    if (kill((pid_t)view->process_pid, SIGTERM) < 0)
        return snag_errorf(error, error_size, "cannot terminate session owner: %s", strerror(errno));
    return 0;
#endif
}
