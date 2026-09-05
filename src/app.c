/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"
#include "base.h"
#include "config.h"
#include "context.h"
#include "credential.h"
#include "json.h"
#include "provider.h"
#include "render.h"
#include "secret.h"
#include "snajpagent.h"
#include "store.h"
#include "turn.h"
#include "tools.h"
#include "wire.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <langinfo.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RESUME_COMMAND_MAX (4u * 1024u * 1024u)

struct app_signal_handlers {
    struct sigaction saved_sigint;
    struct sigaction saved_sighup;
    struct sigaction saved_sigterm;
    bool sigint_installed;
    bool sighup_installed;
    bool sigterm_installed;
};

static volatile sig_atomic_t pending_shutdown_signal;

static void
mark_shutdown_signal(int signal_number)
{
    if (!pending_shutdown_signal)
        pending_shutdown_signal = signal_number;
}

static void
restore_shutdown_handlers(struct app_signal_handlers *handlers)
{
    if (handlers->sigterm_installed)
        (void)sigaction(SIGTERM, &handlers->saved_sigterm, NULL);
    if (handlers->sighup_installed)
        (void)sigaction(SIGHUP, &handlers->saved_sighup, NULL);
    if (handlers->sigint_installed)
        (void)sigaction(SIGINT, &handlers->saved_sigint, NULL);
    memset(handlers, 0, sizeof(*handlers));
}

static int
install_shutdown_handlers(struct app_signal_handlers *handlers)
{
    struct sigaction action;

    memset(handlers, 0, sizeof(*handlers));
    memset(&action, 0, sizeof(action));
    action.sa_handler = mark_shutdown_signal;
    sigemptyset(&action.sa_mask);
    pending_shutdown_signal = 0;
    if (sigaction(SIGINT, &action, &handlers->saved_sigint) < 0)
        return -1;
    handlers->sigint_installed = true;
    if (sigaction(SIGHUP, &action, &handlers->saved_sighup) < 0)
        goto fail;
    handlers->sighup_installed = true;
    if (sigaction(SIGTERM, &action, &handlers->saved_sigterm) < 0)
        goto fail;
    handlers->sigterm_installed = true;
    return 0;
fail:
    restore_shutdown_handlers(handlers);
    return -1;
}

static bool
capture_shutdown_signal(struct app_state *app)
{
    sig_atomic_t signal_number = pending_shutdown_signal;

    if (!signal_number)
        return false;
    if (!app->shutdown_signal)
        app->shutdown_signal = (int)signal_number;
    app->input_closed = true;
    return true;
}

static int
app_error(struct app_state *app, const char *message)
{
    return snj_render_error_ctx(&app->render, message);
}
static int
app_warning(struct app_state *app, const char *message)
{
    return snj_render_warning_ctx(&app->render, message);
}
static void
history_warning(struct app_state *app)
{
    if (snj_term_take_history_warning(&app->term))
        (void)app_warning(app, "prompt history is unavailable or contained damaged records");
}
static void
remember_input(struct app_state *app, const char *text)
{
    (void)snj_term_history_add(&app->term, text);
    history_warning(app);
}
static int
app_hostf(struct app_state *app, const char *fmt, ...)
{
    struct snj_buf text;
    va_list ap;
    int needed;
    int rc;
    snj_buf_init(&text, 4u * 1024u * 1024u);
    va_start(ap, fmt);
    needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed < 0 || (size_t)needed > text.max) {
        snj_buf_free(&text);
        errno = EOVERFLOW;
        return -1;
    }
    if (snj_buf_reserve(&text, (size_t)needed + 1u) < 0) {
        snj_buf_free(&text);
        return -1;
    }
    va_start(ap, fmt);
    (void)vsnprintf((char *)text.data, (size_t)needed + 1u, fmt, ap);
    va_end(ap);
    text.len = (size_t)needed;
    rc = snj_render_host(&app->render, (const char *)text.data);
    snj_buf_free(&text);
    return rc;
}
static int
app_runtimef(struct app_state *app, const char *fmt, ...)
{
    struct snj_buf text;
    va_list ap;
    int needed;
    int rc;
    if (app->render.verbosity < 3u)
        return 0;
    snj_buf_init(&text, 4u * 1024u * 1024u);
    va_start(ap, fmt);
    needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed < 0 || (size_t)needed > text.max) {
        snj_buf_free(&text);
        errno = EOVERFLOW;
        return -1;
    }
    if (snj_buf_reserve(&text, (size_t)needed + 1u) < 0) {
        snj_buf_free(&text);
        return -1;
    }
    va_start(ap, fmt);
    (void)vsnprintf((char *)text.data, (size_t)needed + 1u, fmt, ap);
    va_end(ap);
    text.len = (size_t)needed;
    rc = snj_render_runtime(&app->render, (const char *)text.data);
    snj_buf_free(&text);
    return rc;
}
static void
usage_number(char out[32], bool known, uint64_t value)
{
    if (known)
        (void)snprintf(out, 32u, "%llu", (unsigned long long)value);
    else
        (void)snprintf(out, 32u, "?");
}
static const char *
graph_outcome_name(enum snj_graph_outcome outcome)
{
    switch (outcome) {
    case SNJ_GRAPH_CALLS: return "calls";
    case SNJ_GRAPH_FINAL: return "final";
    case SNJ_GRAPH_REFUSAL: return "refusal";
    case SNJ_GRAPH_NONPRODUCTIVE: return "nonproductive";
    case SNJ_GRAPH_CONFLICT: return "conflict";
    }
    return "unknown";
}
static const struct snj_term_command commands[] = {
    {"/help", "commands and keys"},
    {"/?", "commands and keys (alias for /help)"},
    {"/status", "session and next-turn settings"},
    {"/history", "recent terminal history"},
    {"/model [list|cache|#|SELECTOR [save|s]]", "list, refresh, or select a model"},
    {"/config", "edit and reload the active configuration"},
    {"/effort [LEVEL]", "show or set next-turn effort"},
    {"/goal [COMMAND|TEXT]", "show, start, or control a persistent goal"},
    {"/verbose [0..6]", "show or set this process's verbosity"},
    {"/queue [TEXT|ACTION]", "list/add/edit/delete/clear/pop queued turns (/q alias)"},
    {"/next", "run the oldest paused turn"},
    {"/archive", "archive idle session and exit"},
    {"/compact", "compact the idle session context"},
    {"/delete", "delete idle session after confirmation"},
    {"/exit", "preserve session and exit"},
    {"/chat", "show IRC room activity"},
    {"/rollout", "show local model activity"},
    {"/topic [TEXT]", "show or change the IRC room topic"},
    {"/names", "show IRC endpoints, room members, and operator flags"}
};

static size_t
command_count(const struct app_state *app)
{
    size_t count = sizeof(commands) / sizeof(commands[0]);
    return app->networked ? count : count - 4u;
}
static const char *
effective_model(const char *model)
{
    return strcmp(model, "default") == 0 ? SNAJPAGENT_MODEL : model;
}
static const char *
resolve_effort(const char *preference)
{
    if (!preference || strcmp(preference, "default") == 0)
        return "medium";
    if (!*preference || strlen(preference) >= SNJ_CONFIG_EFFORT_MAX ||
        !snj_utf8_valid((const unsigned char *)preference,
                        strlen(preference), true))
        return NULL;
    return preference;
}
static const struct snj_provider_config *
next_provider(const struct app_state *app)
{
    if (app->staged_provider)
        return app->staged_provider;
    return snj_config_provider(app->config,
        app->session.default_provider[0] ? app->session.default_provider : NULL);
}

static void
provider_capacity_source_sha256(
    const struct snj_provider_config *provider,
    char digest[SNJ_SHA256_HEX_LEN + 1u])
{
    char source[SNJ_CONFIG_URL_MAX + 16u];
    const char *protocol = snj_provider_catalog_protocol(provider);
    size_t protocol_len = strlen(protocol);
    size_t base_url_len = strlen(provider->base_url);

    memcpy(source, protocol, protocol_len);
    source[protocol_len] = '\n';
    memcpy(source + protocol_len + 1u, provider->base_url, base_url_len);
    snj_sha256_hex(source, protocol_len + 1u + base_url_len, digest);
}

static bool
capacity_ceiling_matches(const struct app_state *app,
                         const struct snj_provider_config *provider,
                         const char *model)
{
    char source_hash[SNJ_SHA256_HEX_LEN + 1u];

    if (!app->session.capacity_ceiling_valid ||
        strcmp(app->session.capacity_ceiling_provider, provider->name) != 0 ||
        strcmp(app->session.capacity_ceiling_model, model) != 0)
        return false;
    provider_capacity_source_sha256(provider, source_hash);
    return strcmp(app->session.capacity_ceiling_source_sha256,
                  source_hash) == 0;
}

static void
apply_capacity_ceiling(const struct app_state *app,
                       const struct snj_provider_config *provider,
                       const char *model,
                       struct snj_model_capacity *capacity)
{
    if (capacity_ceiling_matches(app, provider, model) &&
        (!capacity->hard_input_known ||
         app->session.capacity_ceiling_input_tokens <
             capacity->hard_input_tokens)) {
        capacity->hard_input_tokens =
            app->session.capacity_ceiling_input_tokens;
        capacity->hard_input_known = true;
        capacity->source = SNJ_CAPACITY_OBSERVED;
    }
}

void
snj_app_record_model_accounting(struct app_state *app,
                                enum snj_count_capability capability,
                                uint64_t model_input_bytes,
                                uint64_t input_tokens,
                                uint64_t hard_input_tokens)
{
    char error[256] = {0};

    if (capability != SNJ_COUNT_UNKNOWN)
        app->turn_capacity.count_capability = capability;
    if (snj_model_cache_record(&app->store, &app->model_cache,
            app->turn_provider,
            snj_provider_catalog_protocol(app->turn_provider),
            app->turn_model, capability, model_input_bytes, input_tokens,
            hard_input_tokens, error, sizeof(error)) < 0)
        (void)app_warning(app, error[0] ? error :
            "model accounting observation could not be cached");
}

int
snj_app_capacity_resolve(struct app_state *app,
                         const struct snj_provider_config *provider,
                         const char *model,
                         struct snj_model_capacity *capacity,
                         char *error, size_t error_size)
{
    int cache_rc;

    if (!app || !provider || !model || !capacity) {
        snj_errorf(error, error_size, "invalid model capacity selection");
        errno = EINVAL;
        return -1;
    }
    snj_model_cache_free(&app->model_cache);
    app->capacity_cache_error[0] = '\0';
    cache_rc = snj_model_cache_load(&app->store, &app->model_cache,
                                    app->capacity_cache_error,
                                    sizeof(app->capacity_cache_error));
    if (cache_rc == 1)
        app->capacity_cache_error[0] = '\0';
    cache_rc = snj_model_capacity_resolve(&app->model_cache, app->config,
        provider, model, snj_provider_catalog_protocol(provider), capacity,
        error, error_size);
    if (cache_rc == 0)
        apply_capacity_ceiling(app, provider, model, capacity);
    return cache_rc;
}

static int
prepare_turn_settings(struct app_state *app, char *error, size_t error_size)
{
    const char *model = app->staged_model ? app->staged_model :
                                           app->session.default_model;
    const char *effort_preference = app->staged_effort ? app->staged_effort :
                                                        app->session.default_effort;
    const char *effort = resolve_effort(effort_preference);
    const struct snj_provider_config *provider = next_provider(app);
    if (!provider) {
        snj_errorf(error, error_size,
                  "selected provider is not present in the current configuration");
        errno = ENOENT;
        return -1;
    }
    if (!effort) {
        snj_errorf(error, error_size,
                  "reasoning effort is empty, oversized, or invalid UTF-8");
        errno = ENOTSUP;
        return -1;
    }
    app->turn_model = model;
    app->turn_effort = effort;
    app->turn_provider = provider;
    if (snj_app_capacity_resolve(app, provider, model, &app->turn_capacity,
                                 error, error_size) < 0)
        return -1;
    return 0;
}
static void
consume_staged_settings(struct app_state *app)
{
    app->staged_model = NULL;
    app->staged_effort = NULL;
    app->staged_provider = NULL;
}
int
snj_app_commit_event(struct app_state *app, const char *type, json_t *data,
                     char *error, size_t error_size)
{
    uint64_t seq;
    if (snj_session_commit(&app->session, type, data, &seq,
                           error, error_size) < 0)
        return -1;
    if (snj_render_event(&app->render, seq, type) < 0) {
        snj_errorf(error, error_size, "durable event output failed");
        return -1;
    }
    return 0;
}
#define commit_event snj_app_commit_event
static const char *next_model(const struct app_state *app);
static const char *next_effort(const struct app_state *app);
static int
render_queue(struct app_state *app)
{
    if (app->session.pending_queue_count == 0u)
        return app_warning(app, "future-turn queue is empty");
    for (size_t i = 0; i < app->session.pending_queue_count; ++i) {
        char label[64];
        (void)snprintf(label, sizeof(label), "%zu %.8s › ", i + 1u,
                       app->session.pending_queue[i].queue_id);
        if (snj_render_submitted(&app->render, label,
                                 app->session.pending_queue[i].text) < 0)
            return -1;
    }
    return 0;
}

static int
format_context_meter(struct app_state *app, bool active,
                     char meter[32u])
{
    const struct snj_provider_config *provider = active ? app->turn_provider :
                                                        next_provider(app);
    const char *model = active ? app->turn_model : next_model(app);
    const char *effort = active ? app->turn_effort :
                                  resolve_effort(next_effort(app));
    struct snj_model_capacity resolved;
    const struct snj_model_capacity *capacity = &app->turn_capacity;
    char provider_source_hash[SNJ_SHA256_HEX_LEN + 1u];
    uint64_t used;
    uint64_t hard;
    unsigned int percent;
    int n;

    if (!provider || !model || !effort) {
        errno = EINVAL;
        return -1;
    }
    if (!active) {
        char error[256] = {0};

        if (snj_app_capacity_resolve(app, provider, model, &resolved,
                                     error, sizeof(error)) < 0)
            return -1;
        capacity = &resolved;
    }
    provider_capacity_source_sha256(provider, provider_source_hash);
    if (!app->session.context_meter_valid ||
        strcmp(app->session.context_meter_provider, provider->name) != 0 ||
        strcmp(app->session.context_meter_model, model) != 0 ||
        strcmp(app->session.context_meter_effort, effort) != 0 ||
        strcmp(app->session.context_meter_provider_source_sha256,
               provider_source_hash) != 0 ||
        strcmp(app->session.context_meter_compact_id,
               app->session.compact_id) != 0) {
        memcpy(meter, "0%", sizeof("0%"));
        return 0;
    }
    if (!capacity->hard_input_known) {
        memcpy(meter, "?%", sizeof("?%"));
        return 0;
    }
    used = app->session.context_meter_input_tokens;
    hard = capacity->hard_input_tokens;
    if (used >= hard) {
        percent = 100u;
    } else {
        percent = (unsigned int)((used * 100u + hard - 1u) / hard);
    }
    n = snprintf(meter, 32u, "%u%%", percent);
    if (n < 0 || n >= 32) {
        errno = EOVERFLOW;
        return -1;
    }
    return 0;
}

static int
format_input_label(struct app_state *app, bool active,
                   char label[SNJ_TERM_LABEL_BYTES])
{
    const struct snj_provider_config *provider = active ? app->turn_provider :
                                                        next_provider(app);
    const char *model = active ? app->turn_model : next_model(app);
    const char *effort = active ? app->turn_effort :
                                  resolve_effort(next_effort(app));
    char hostname[256u], meter[32u];
    const char *values[7];
    unsigned int selected = snj_render_view(&app->render) == SNJ_RENDER_CHAT ?
                            0u : active ? 2u : 1u;

    if (!provider || !model || !effort ||
        format_context_meter(app, active, meter) < 0)
        return -1;
    if (gethostname(hostname, sizeof(hostname)) < 0)
        memcpy(hostname, "localhost", sizeof("localhost"));
    hostname[sizeof(hostname) - 1u] = '\0';
    if (!snj_utf8_valid((const unsigned char *)hostname, strlen(hostname), true))
        memcpy(hostname, "localhost", sizeof("localhost"));
    for (size_t i = 0u; hostname[i]; ++i)
        if ((unsigned char)hostname[i] <= 0x20u || hostname[i] == 0x7f)
            hostname[i] = '_';
    values[0] = provider->name;
    values[1] = model;
    values[2] = effort;
    values[3] = app->networked ? snj_irc_operator_nick(app->irc) : "";
    values[4] = hostname;
    values[5] = meter;
    values[6] = selected == 0u ? "chat" :
                selected == 1u ? "rollout-idle" : "rollout-active";
    if (app->queue_edit_id[0]) {
        struct snj_buf out;

        snj_buf_init(&out, SNJ_TERM_LABEL_BYTES - 1u);
        if (snj_buf_printf(&out, "edit %zu %s", app->queue_edit_number, meter) < 0)
            goto fail;
        for (unsigned int i = 0u; i < SNJ_TERM_SPINNER_COUNT; ++i)
            if (snj_buf_putc(&out, SNJ_TERM_SPINNER_MARKER_BASE + i) < 0)
                goto fail;
        if (snj_buf_append(&out, "›", strlen("›")) < 0)
            goto fail;
        if (!out.len || snj_buf_putc(&out, ' ') < 0 ||
            snj_buf_terminate(&out) < 0)
            goto fail;
        memcpy(label, out.data, out.len + 1u);
        snj_buf_free(&out);
        return 0;
fail:
        snj_buf_free(&out);
        return -1;
    }
    return snj_config_prompt_expand(app->config->prompt, selected, values,
                                    SNJ_TERM_SPINNER_MARKER_BASE, label,
                                    SNJ_TERM_LABEL_BYTES);
}

static int
validate_prompt_values(const struct snj_config *config,
                       const struct snj_provider_config *provider,
                       const char *model, const char *effort,
                       bool networked)
{
    char hostname[256u];
    const char *values[7];
    const char *spinners[SNJ_TERM_SPINNER_COUNT] = {
        config->prompt_spinner_goal,
        config->prompt_spinner_provider,
        config->prompt_spinner_tool
    };
    struct snj_term probe;
    char label[SNJ_TERM_LABEL_BYTES];
    int rc = -1;

    if (!provider || !model || !effort)
        return -1;
    if (gethostname(hostname, sizeof(hostname)) < 0)
        memcpy(hostname, "localhost", sizeof("localhost"));
    hostname[sizeof(hostname) - 1u] = '\0';
    if (!snj_utf8_valid((const unsigned char *)hostname, strlen(hostname), true))
        memcpy(hostname, "localhost", sizeof("localhost"));
    for (size_t i = 0u; hostname[i]; ++i)
        if ((unsigned char)hostname[i] <= 0x20u || hostname[i] == 0x7f)
            hostname[i] = '_';
    values[0] = provider->name;
    values[1] = model;
    values[2] = effort;
    values[3] = networked ? config->irc_operator_nick : "";
    values[4] = hostname;
    values[5] = "100%";
    snj_term_init(&probe);
    for (unsigned int mode = 0u; mode < 3u; ++mode) {
        values[6] = mode == 0u ? "chat" : mode == 1u ?
                    "rollout-idle" : "rollout-active";
        if (snj_config_prompt_expand(config->prompt, mode, values,
                SNJ_TERM_SPINNER_MARKER_BASE, label, sizeof(label)) < 0 ||
            snj_term_set_prompt_template(&probe, mode == 2u, label, spinners,
                config->prompt_spinner_per_second,
                (1u << SNJ_TERM_SPINNER_COUNT) - 1u) < 0)
            goto out;
    }
    rc = 0;
out:
    snj_term_close(&probe);
    return rc;
}

static int
validate_prompt_candidate(struct app_state *app,
                          const struct snj_config *config)
{
    const struct snj_provider_config *provider = snj_config_provider(
        config, app->session.default_provider[0] ?
        app->session.default_provider : NULL);

    return validate_prompt_values(config, provider, next_model(app),
                                  resolve_effort(next_effort(app)),
                                  snj_irc_enabled(config));
}

static unsigned int
prompt_spinner_states(const struct app_state *app, bool active)
{
    return (app->session.goal_status == SNJ_GOAL_ACTIVE ?
            1u << SNJ_TERM_SPINNER_GOAL : 0u) |
           (active && !app->tool_active ?
            1u << SNJ_TERM_SPINNER_PROVIDER : 0u) |
           (app->tool_active ? 1u << SNJ_TERM_SPINNER_TOOL : 0u);
}

static int
set_input_prompt(struct app_state *app, bool active)
{
    char label[SNJ_TERM_LABEL_BYTES];
    const char *spinners[SNJ_TERM_SPINNER_COUNT] = {
        app->config->prompt_spinner_goal,
        app->config->prompt_spinner_provider,
        app->config->prompt_spinner_tool
    };
    unsigned int states = prompt_spinner_states(app, active);

    if (format_input_label(app, active, label) < 0 ||
        snj_render_before_prompt(&app->render) < 0)
        return -1;
    return snj_term_set_prompt_template(&app->term, active, label, spinners,
        app->config->prompt_spinner_per_second, states);
}

static int
tick_irc(struct app_state *app, char *error, size_t error_size)
{
    char model[SNJ_CONFIG_IRC_NICK_MAX + 1u];
    char operator[SNJ_CONFIG_IRC_NICK_MAX + 1u];
    char label[SNJ_TERM_LABEL_BYTES];

    (void)snprintf(model, sizeof(model), "%s", snj_irc_model_nick(app->irc));
    (void)snprintf(operator, sizeof(operator), "%s",
                   snj_irc_operator_nick(app->irc));
    if (snj_irc_tick(app->irc, 0, error, error_size) < 0)
        return -1;
    if (strcmp(model, snj_irc_model_nick(app->irc)) == 0 &&
        strcmp(operator, snj_irc_operator_nick(app->irc)) == 0)
        return 0;
    if (snj_app_irc_snapshot(app, "nick", error, error_size) < 0)
        return -1;
    if (!app->term.opened || !app->term.prompt_wanted)
        return 0;
    if (format_input_label(app, app->term.active, label) < 0)
        return -1;
    return strcmp(label, app->term.label) == 0 ? 0 :
        set_input_prompt(app, app->term.active);
}

static struct snj_queued_turn *
queued_by_id(struct app_state *app, const char *queue_id, size_t *index)
{
    for (size_t i = 0; i < app->session.pending_queue_count; ++i) {
        if (strcmp(app->session.pending_queue[i].queue_id, queue_id) == 0) {
            if (index)
                *index = i;
            return &app->session.pending_queue[i];
        }
    }
    return NULL;
}

static int
begin_queue_edit(struct app_state *app, size_t number, bool active,
                 char *error, size_t error_size)
{
    struct snj_queued_turn *queued;

    if (number == 0u || number > app->session.pending_queue_count) {
        snj_errorf(error, error_size, "queue item %zu does not exist", number);
        return 1;
    }
    queued = &app->session.pending_queue[number - 1u];
    memcpy(app->queue_edit_id, queued->queue_id, sizeof(app->queue_edit_id));
    app->queue_edit_number = number;
    app->queue_edit_was_armed = app->queue_armed;
    app->queue_armed = false;
    if (set_input_prompt(app, active) < 0 ||
        snj_term_restore_draft(&app->term, queued->text) < 0) {
        app->queue_armed = app->queue_edit_was_armed;
        app->queue_edit_id[0] = '\0';
        app->queue_edit_number = 0u;
        app->queue_edit_was_armed = false;
        snj_errorf(error, error_size, "queue editor could not be displayed");
        return -1;
    }
    return 0;
}

static int
finish_queue_edit(struct app_state *app, const char *text, bool active,
                  char *error, size_t error_size)
{
    struct snj_queued_turn *queued;
    size_t len = strlen(text);
    bool restore_armed = app->queue_edit_was_armed;
    int rc = 0;

    queued = queued_by_id(app, app->queue_edit_id, NULL);
    if (!queued) {
        snj_errorf(error, error_size, "the queued turn being edited no longer exists");
        (void)snj_render_error_ctx(&app->render, error);
        error[0] = '\0';
        rc = 1;
        goto clear;
    }
    if (!len || len > SNJ_MAX_QUEUED_TEXT ||
        !snj_utf8_valid((const unsigned char *)text, len, true)) {
        snj_errorf(error, error_size,
                  "queued text must be nonempty valid UTF-8 within 256 KiB");
        (void)snj_render_error_ctx(&app->render, error);
        error[0] = '\0';
        if (set_input_prompt(app, active) < 0 ||
            snj_term_restore_draft(&app->term, text) < 0)
            return -1;
        return 1;
    }
    if (strcmp(queued->text, text) != 0 &&
        commit_event(app, "future_turn_edited",
                     snj_app_future_turn_edited_data(queued->queue_id, text),
                     error, error_size) < 0) {
        if (set_input_prompt(app, active) == 0)
            (void)snj_term_restore_draft(&app->term, text);
        return -1;
    }
    if (snj_render_submitted(&app->render,
            snj_term_prompt_label(&app->term), text) < 0) {
        snj_errorf(error, error_size, "edited turn acknowledgement could not be rendered");
        return -1;
    }
clear:
    app->queue_edit_id[0] = '\0';
    app->queue_edit_number = 0u;
    app->queue_edit_was_armed = false;
    if (restore_armed && app->session.active_turn &&
        app->session.pending_queue_count != 0u)
        app->queue_armed = true;
    if (set_input_prompt(app, active) < 0)
        return -1;
    return rc;
}
static int
queue_future_turn(struct app_state *app, const char *text, bool arm,
                  char *error, size_t error_size)
{
    char queue_id[SNJ_ID_HEX_LEN + 1u];
    const char *queued_text = text;
    size_t len;
    if (!app->session.active_turn) {
        snj_errorf(error, error_size, "/queue TEXT is valid only while a turn is active");
        errno = EINVAL;
        return 1;
    }
    if (queued_text[0] == '/') {
        if (queued_text[1] != '/') {
            snj_errorf(error, error_size,
                      "queued text starting with / must use // for a literal slash");
            errno = EINVAL;
            return 1;
        }
        ++queued_text;
    }
    len = strlen(queued_text);
    if (!len || len > SNJ_MAX_QUEUED_TEXT ||
        !snj_utf8_valid((const unsigned char *)queued_text, len, true)) {
        snj_errorf(error, error_size,
                  "queued text must be nonempty valid UTF-8 within 256 KiB");
        errno = EINVAL;
        return 1;
    }
    if (snj_random_id(queue_id) < 0) {
        snj_errorf(error, error_size, "cryptographic queue id generation failed");
        return -1;
    }
    if (commit_event(app, "future_turn_queued",
                     snj_app_future_turn_queued_data(app->session.active_turn_id,
                                             queue_id, queued_text),
                     error, error_size) < 0)
        return -1;
    if (snj_render_submitted(&app->render, "next › ", queued_text) < 0) {
        snj_errorf(error, error_size, "queued turn acknowledgement could not be rendered");
        return -1;
    }
    if (arm)
        app->queue_armed = true;
    return 0;
}
static int
remove_queued_turns(struct app_state *app, size_t index, bool all,
                    char *error, size_t error_size)
{
    bool remove[SNJ_MAX_PENDING_TURNS] = {false};
    size_t matches;

    if (app->session.pending_queue_count == 0u ||
        (!all && index >= app->session.pending_queue_count)) {
        snj_errorf(error, error_size, "future-turn queue is empty");
        return 1;
    }
    if (all) {
        for (size_t i = 0; i < app->session.pending_queue_count; ++i)
            remove[i] = true;
        matches = app->session.pending_queue_count;
    } else {
        remove[index] = true;
        matches = 1u;
    }
    if (commit_event(app, "future_turn_cancelled",
                     snj_app_future_turn_cancelled_data(&app->session, remove),
                     error, error_size) < 0)
        return -1;
    if (app->session.pending_queue_count == 0u)
        app->queue_armed = false;
    {
        char message[96];
        (void)snprintf(message, sizeof(message), "%zu future turn%s cancelled",
                       matches, matches == 1u ? "" : "s");
        return app_warning(app, message);
    }
}

static int
handle_queue_command(struct app_state *app, const char *line, bool active,
                     bool *handled, char *error, size_t error_size)
{
    enum queue_command_kind kind;
    const char *argument;
    size_t number = 0u;

    *handled = true;
    if (strcmp(line, "/queue") == 0 || strcmp(line, "/q") == 0) {
        argument = "";
    } else if (strncmp(line, "/queue ", 7u) == 0) {
        argument = line + 7u;
    } else if (strncmp(line, "/q ", 3u) == 0) {
        argument = line + 3u;
    } else {
        *handled = false;
        return 0;
    }
    if (snj_app_parse_queue_argument(argument, &kind, &number) < 0) {
        snj_errorf(error, error_size,
                  "queue action expects clear, pop, N delete, Nd, N edit, or Ne");
        return 1;
    }
    switch (kind) {
    case QUEUE_COMMAND_LIST:
        return render_queue(app);
    case QUEUE_COMMAND_ADD:
        if (!active) {
            snj_errorf(error, error_size,
                      "/queue TEXT is active-only; submit it during a running turn");
            return 1;
        }
        return queue_future_turn(app, argument, true, error, error_size);
    case QUEUE_COMMAND_DELETE:
        if (number == 0u || number > app->session.pending_queue_count) {
            snj_errorf(error, error_size, "queue item %zu does not exist", number);
            return 1;
        }
        return remove_queued_turns(app, number - 1u, false,
                                   error, error_size);
    case QUEUE_COMMAND_EDIT:
        return begin_queue_edit(app, number, active, error, error_size);
    case QUEUE_COMMAND_CLEAR:
        return remove_queued_turns(app, 0u, true, error, error_size);
    case QUEUE_COMMAND_POP:
        if (app->session.pending_queue_count == 0u) {
            snj_errorf(error, error_size, "future-turn queue is empty");
            return 1;
        }
        return remove_queued_turns(app,
            app->session.pending_queue_count - 1u, false,
            error, error_size);
    }
    errno = EINVAL;
    return -1;
}
static const char *
next_model(const struct app_state *app)
{
    return app->staged_model ? app->staged_model : app->session.default_model;
}
static const char *
next_effort(const struct app_state *app)
{
    return app->staged_effort ? app->staged_effort :
                                app->session.default_effort;
}

static int
append_capacity_value(struct snj_buf *text, const char *name,
                      bool known, uint64_t value)
{
    return known ?
        snj_buf_printf(text, " · %s=%llu", name,
                       (unsigned long long)value) :
        snj_buf_printf(text, " · %s=unknown", name);
}

static int
append_advertised_capacity(struct snj_buf *text, const json_t *model)
{
    static const struct {
        const char *key;
        const char *name;
    } fields[] = {
        {"context_window_tokens", "context"},
        {"max_context_window_tokens", "max-context"},
        {"input_context_window_tokens", "input-context"},
        {"max_input_tokens", "max-input"},
        {"max_output_tokens", "max-output"},
        {"auto_compact_input_tokens", "auto-compact"},
        {"effective_context_window_percent", "effective-percent"}
    };
    const json_t *limits = model ? json_object_get(model, "limits") : NULL;

    if (!json_is_object(limits))
        return 0;
    if (snj_buf_append(text, "\nadvertised", 11u) < 0)
        return -1;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        uint64_t value = 0u;
        bool known = snj_json_integer_u64((json_t *)limits,
                                          fields[i].key, &value) == 0;
        if (append_capacity_value(text, fields[i].name, known, value) < 0)
            return -1;
    }
    return 0;
}

static int
render_status(struct app_state *app)
{
    const char *id = app->render.verbosity >= 3u ? app->session.id : NULL;
    const struct snj_provider_config *provider = next_provider(app);
    const struct snj_model_limit_config *configured = NULL;
    const json_t *advertised = NULL;
    struct snj_model_capacity capacity;
    struct snj_buf text;
    bool ceiling_selection_matches;
    bool ceiling_source_matches;
    char error[256] = {0};
    int rc = -1;

    if (!provider)
        return app_error(app,
            "selected provider is not present in the current configuration");
    if (snj_app_capacity_resolve(app, provider, next_model(app), &capacity,
                                 error, sizeof(error)) < 0)
        return app_error(app, error[0] ? error :
                         "model capacity could not be resolved");
    configured = snj_config_model_limit(app->config, provider->name,
                                        next_model(app));
    ceiling_selection_matches = app->session.capacity_ceiling_valid &&
        strcmp(app->session.capacity_ceiling_provider, provider->name) == 0 &&
        strcmp(app->session.capacity_ceiling_model, next_model(app)) == 0;
    ceiling_source_matches = ceiling_selection_matches &&
        capacity_ceiling_matches(app, provider, next_model(app));
    if (capacity.source_bound)
        advertised = snj_model_cache_find(&app->model_cache, provider->name,
                                          next_model(app));
    snj_buf_init(&text, 64u * 1024u);
    if (snj_buf_printf(&text,
        "session: %s\n"
        "state: %s\n"
        "provider: %s%s\n"
        "model: %s%s\n"
        "effort: %s%s\n"
        "workspace: %s\n"
        "turns: %llu\n"
        "queue: %zu%s\n"
        "verbosity: %u\n"
        "context: source=%s",
        id ? id : (char [9]){app->session.id[0], app->session.id[1],
                             app->session.id[2], app->session.id[3],
                             app->session.id[4], app->session.id[5],
                             app->session.id[6], app->session.id[7], '\0'},
        app->session.active_turn ? "active" : "idle",
        next_provider(app) ? next_provider(app)->name : "<missing>",
        app->staged_provider ? " (staged once)" : "",
        next_model(app), app->staged_model ? " (staged once)" : "",
        next_effort(app), app->staged_effort ? " (staged once)" : "",
        app->session.workspace,
        (unsigned long long)app->session.turn_count,
        app->session.pending_queue_count,
        app->session.pending_queue_count && !app->queue_armed ? " paused" : "",
        app->render.verbosity, snj_capacity_source_name(capacity.source)) < 0 ||
        append_capacity_value(&text, "hard-input",
                              capacity.hard_input_known,
                              capacity.hard_input_tokens) < 0 ||
        append_capacity_value(&text, "requested-output",
                              capacity.max_output_known,
                              capacity.max_output_tokens) < 0)
        goto out;
    if (capacity.effective_context_window_known &&
        snj_buf_printf(&text, " · effective=%u%%%s",
                       capacity.effective_context_window_percent,
                       capacity.effective_context_window_derived ?
                           " (derived client policy)" : " (advertised)") < 0)
        goto out;
    if (configured) {
        if (snj_buf_append(&text, "\nconfigured", 11u) < 0 ||
            append_capacity_value(&text, "context",
                configured->context_window_known,
                configured->context_window_tokens) < 0 ||
            append_capacity_value(&text, "max-input",
                configured->max_input_known,
                configured->max_input_tokens) < 0 ||
            append_capacity_value(&text, "max-output",
                configured->max_output_known,
                configured->max_output_tokens) < 0)
            goto out;
    }
    if (append_advertised_capacity(&text, advertised) < 0)
        goto out;
    if (capacity.cache_source_mismatch &&
        snj_buf_append(&text,
            "\ncatalog source: mismatch; advertised limits ignored",
            strlen("\ncatalog source: mismatch; advertised limits ignored")) < 0)
        goto out;
    if (app->capacity_cache_error[0] &&
        snj_buf_printf(&text, "\ncatalog: %s", app->capacity_cache_error) < 0)
        goto out;
    if (app->session.capacity_ceiling_valid) {
        if (snj_buf_printf(&text,
                "\nobserved ceiling: hard-input=%llu · provider=%s · model=%s · binding=%s%s",
                (unsigned long long)
                    app->session.capacity_ceiling_input_tokens,
                app->session.capacity_ceiling_provider,
                app->session.capacity_ceiling_model,
                ceiling_source_matches ? "current" :
                    ceiling_selection_matches ? "source mismatch" :
                                                "different selection",
                ceiling_source_matches ? "" : "; ignored") < 0)
            goto out;
    } else if (snj_buf_append(&text, "\nobserved ceiling: unknown",
                              strlen("\nobserved ceiling: unknown")) < 0) {
        goto out;
    }
    if (snj_buf_printf(&text,
            "\naccounting: policy=%s · exact-count=%s · estimate=%s",
            provider->exact_token_count == SNJ_TOKEN_COUNT_AUTO ? "auto" :
            provider->exact_token_count == SNJ_TOKEN_COUNT_STRICT ? "exact" :
            "off",
            capacity.count_capability == SNJ_COUNT_SUPPORTED ? "supported" :
            capacity.count_capability == SNJ_COUNT_UNSUPPORTED ? "unsupported" :
            "unknown", capacity.observed_tokens_per_million_bytes ? "learned" :
            "none") < 0)
        goto out;
    if (app->session.usage_anchor_valid) {
        if (snj_buf_printf(&text,
                "\nobserved usage: input=%llu tokens · model-input=%llu bytes · provider=%s · model=%s · effort=%s",
                (unsigned long long)app->session.usage_anchor_input_tokens,
                (unsigned long long)app->session.usage_anchor_model_input_bytes,
                app->session.usage_anchor_provider,
                app->session.usage_anchor_model,
                app->session.usage_anchor_effort) < 0)
            goto out;
    } else if (snj_buf_append(&text, "\nobserved usage: unknown",
                              strlen("\nobserved usage: unknown")) < 0) {
        goto out;
    }
    if (snj_buf_terminate(&text) < 0)
        goto out;
    rc = snj_render_host(&app->render, (const char *)text.data);
out:
    snj_buf_free(&text);
    return rc;
}
static int
render_help(struct app_state *app)
{
    static const char ordinary_keys[] =
        "Enter submit/add to active turn · Empty Tab no-op · "
        "Tab complete/indent/queue · Ctrl-C cancel/interrupt · Ctrl-D exit · Ctrl-J newline";
    static const char network_keys[] =
        "Enter submit/add to active turn · Empty Tab switch view · "
        "Tab complete/indent/queue · Ctrl-C cancel/interrupt · Ctrl-D exit · Ctrl-J newline";
    const char *keys = app->networked ? network_keys : ordinary_keys;
    struct snj_buf text;
    int rc = -1;

    snj_buf_init(&text, 64u * 1024u);
    for (size_t i = 0u; i < command_count(app); ++i)
        if (snj_buf_printf(&text, "%-28s%s\n", commands[i].syntax,
                           commands[i].description) < 0)
            goto out;
    if (snj_buf_append(&text, keys, strlen(keys)) < 0 ||
        snj_buf_terminate(&text) < 0)
        goto out;
    rc = snj_render_host(&app->render, (const char *)text.data);
out:
    snj_buf_free(&text);
    return rc;
}
static int
show_setting(struct app_state *app, const char *name, const char *value,
             bool staged)
{
    return app_hostf(app, "%s for next turn: %s%s", name, value,
                     staged ? " (staged once)" : "");
}
static int
refresh_model_cache(struct app_state *app, char *error, size_t error_size)
{
    json_t *providers = json_array();
    int rc = -1;

    if (!providers) {
        errno = ENOMEM;
        return -1;
    }
    for (size_t i = 0; i < app->config->provider_count; ++i) {
        const struct snj_provider_config *provider = &app->config->providers[i];
        json_t *models = NULL;
        json_t *entry = NULL;
        char detail[256] = {0};

        if (snj_app_provider_models(app, provider, &models,
                                    detail, sizeof(detail)) < 0) {
            snj_errorf(error, error_size, "cannot refresh provider %s: %s",
                      provider->name, detail[0] ? detail : strerror(errno));
            goto out;
        }
        entry = json_object();
        if (!entry) {
            json_decref(models);
            snj_errorf(error, error_size, "cannot assemble model cache");
            errno = ENOMEM;
            goto out;
        }
        if (snj_json_set_new(entry, "models", models) < 0) {
            models = NULL;
            json_decref(entry);
            snj_errorf(error, error_size, "cannot assemble model cache");
            errno = ENOMEM;
            goto out;
        }
        models = NULL;
        if (snj_json_set_new(entry, "base_url",
                             json_string(provider->base_url)) < 0 ||
            snj_json_set_new(entry, "name", json_string(provider->name)) < 0 ||
            snj_json_set_new(entry, "protocol",
                json_string(snj_provider_catalog_protocol(provider))) < 0) {
            json_decref(entry);
            snj_errorf(error, error_size, "cannot assemble model cache");
            errno = ENOMEM;
            goto out;
        }
        if (json_array_append_new(providers, entry) < 0) {
            entry = NULL;
            snj_errorf(error, error_size, "cannot assemble model cache");
            errno = ENOMEM;
            goto out;
        }
        entry = NULL;
    }
    if (snj_model_cache_replace(&app->store, providers, snj_time_ms(),
                                &app->model_cache, error, error_size) < 0)
        goto out;
    rc = 0;
out:
    json_decref(providers);
    return rc;
}
static int
load_model_cache(struct app_state *app, bool refresh,
                 char *error, size_t error_size)
{
    int rc;
    if (refresh)
        return refresh_model_cache(app, error, error_size);
    rc = snj_model_cache_load(&app->store, &app->model_cache,
                              error, error_size);
    if (rc == 1) {
        snj_errorf(error, error_size,
                  "model cache is empty; use /model cache while idle");
        errno = ENOENT;
        return -1;
    }
    return rc;
}
static int
append_catalog_limits(struct snj_buf *text, const json_t *model)
{
    const json_t *limits = model ? json_object_get(model, "limits") : NULL;
    const char *count = model ?
        snj_json_string(model, "count_capability") : NULL;
    static const struct {
        const char *key;
        const char *label;
    } fields[] = {
        {"context_window_tokens", "context"},
        {"max_context_window_tokens", "max-context"},
        {"max_input_tokens", "input"},
        {"max_output_tokens", "output"}
    };
    uint64_t observed = 0u;
    bool any = false;

    if (!json_is_object(limits))
        return 0;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        uint64_t value;
        if (snj_json_integer_u64((json_t *)limits, fields[i].key, &value) < 0)
            continue;
        if (snj_buf_printf(text, "%s%s=%llu",
                           any ? "," : " · ", fields[i].label,
                           (unsigned long long)value) < 0)
            return -1;
        any = true;
    }
    if (count)
        (void)snj_json_integer_u64(model, "observed_model_input_bytes",
                                   &observed);
    if (count && snj_buf_printf(text, " · count=%s · estimate=%s", count,
                                observed ? "learned" : "none") < 0)
        return -1;
    return 0;
}

static int
render_model_catalog(struct app_state *app)
{
    const struct snj_provider_config *selected = next_provider(app);
    struct snj_buf text;
    char timestamp[64];
    time_t seconds = (time_t)(app->model_cache.updated_at_ms / 1000u);
    struct tm broken;
    size_t count = snj_model_cache_entry_count(&app->model_cache);
    int rc = -1;

    if (!selected)
        return app_error(app,
            "selected provider is not present in the current configuration");
    snj_buf_init(&text, 16u * 1024u * 1024u);
    if (snj_buf_printf(&text, "selected: %s / %s / %s%s",
                       selected->name, next_model(app),
                       resolve_effort(next_effort(app)) ?
                           resolve_effort(next_effort(app)) : next_effort(app),
                       app->staged_provider || app->staged_model ||
                       app->staged_effort ? " (staged once)" : "") < 0)
        goto out;
    for (size_t index = 1u; index <= count; ++index) {
        const char *provider;
        const char *model;
        const char *effort;
        if (snj_model_cache_entry(&app->model_cache, index,
                                  resolve_effort(app->config->reasoning_effort),
                                  &provider, &model, &effort) != 0 ||
            snj_buf_printf(&text, "\n%zu. %s / %s / %s",
                           index, provider, model, effort) < 0)
            goto out;
        if (append_catalog_limits(&text,
                snj_model_cache_find(&app->model_cache, provider, model)) < 0)
            goto out;
    }
    if (gmtime_r(&seconds, &broken) &&
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ",
                 &broken) != 0u) {
        if (snj_buf_printf(&text, "\ncache updated: %s", timestamp) < 0)
            goto out;
    } else if (snj_buf_printf(&text, "\ncache updated: %llu ms since epoch",
                              (unsigned long long)
                                  app->model_cache.updated_at_ms) < 0) {
        goto out;
    }
    if (snj_buf_terminate(&text) < 0)
        goto out;
    rc = snj_render_host(&app->render, (const char *)text.data);
out:
    snj_buf_free(&text);
    return rc;
}
static bool
parse_model_index(const char *value, size_t *index)
{
    size_t number = 0u;
    const unsigned char *p = (const unsigned char *)value;
    if (*p == '#')
        ++p;
    if (!*p)
        return false;
    for (; *p; ++p) {
        size_t digit;
        if (*p < '0' || *p > '9')
            return false;
        digit = (size_t)(*p - '0');
        if (number > (SIZE_MAX - digit) / 10u)
            number = SIZE_MAX;
        else
            number = number * 10u + digit;
    }
    *index = number;
    return true;
}
static char *
trim_selector_part(char *part)
{
    char *end;
    while (*part && isspace((unsigned char)*part))
        ++part;
    end = part + strlen(part);
    while (end > part && isspace((unsigned char)end[-1]))
        --end;
    *end = '\0';
    return part;
}
static int
commit_model_selection(struct app_state *app,
                       const struct snj_provider_config *provider,
                       const char *model, const char *effort,
                       bool known_in_cache, bool save)
{
    const char *old_provider = app->session.default_provider;
    char error[256] = {0};
    int rc;

    if (save && snj_config_save_model(app->config_path,
            app->config_allow_create, provider->name, model, effort,
            error, sizeof(error)) < 0)
        return app_error(app, error[0] ? error :
                         "model settings could not be written to configuration");
    if (!old_provider[0])
        old_provider = "";
    if (strcmp(old_provider, provider->name) != 0 ||
        strcmp(app->session.default_model, model) != 0 ||
        strcmp(app->session.default_effort, effort) != 0) {
        if (commit_event(app, "model_selection_changed",
                snj_app_model_selection_changed_data(
                    old_provider, provider->name,
                    app->session.default_model, model,
                    app->session.default_effort, effort),
                error, sizeof(error)) < 0) {
            (void)app_error(app, error[0] ? error :
                            "model selection could not be saved");
            return -1;
        }
    }
    app->staged_provider = NULL;
    app->staged_model = NULL;
    app->staged_effort = NULL;
    if (save) {
        (void)snprintf(app->config->provider, sizeof(app->config->provider),
                       "%s", provider->name);
        (void)snprintf(app->config->model, sizeof(app->config->model),
                       "%s", model);
        (void)snprintf(app->config->reasoning_effort,
                       sizeof(app->config->reasoning_effort), "%s", effort);
    }
    rc = app_hostf(app, "model for next turn: %s / %s / %s",
                   provider->name, model, effort);
    if (rc < 0)
        return rc;
    if (!known_in_cache && app_warning(app,
            "model is not known in the model cache; it will still be sent unchanged") < 0)
        return -1;
    return save ? app_hostf(app, "configuration saved: %s", app->config_path) : 0;
}
static int
select_cached_model(struct app_state *app, const char *value, bool save)
{
    const struct snj_provider_config *provider_config;
    const char *provider;
    const char *model;
    const char *effort;
    char error[256] = {0};
    size_t index;
    int entry_rc;

    if (!parse_model_index(value, &index))
        return 1;
    if (load_model_cache(app, false, error, sizeof(error)) < 0)
        return app_error(app, error);
    entry_rc = snj_model_cache_entry(&app->model_cache, index,
                                     resolve_effort(app->config->reasoning_effort),
                                     &provider, &model, &effort);
    if (entry_rc != 0)
        return app_error(app, "model index is not in the displayed cache");
    provider_config = snj_config_provider(app->config, provider);
    if (!provider_config)
        return app_error(app,
            "cached provider is not configured; use /model cache");
    return commit_model_selection(app, provider_config, model, effort, true,
                                  save);
}
static int
select_typed_model(struct app_state *app, const char *value, bool save)
{
    const struct snj_provider_config *provider;
    const char *model;
    const char *effort;
    char *copy = snj_strdup_checked(value, SNJ_CONFIG_PATH_MAX);
    char *parts[3];
    size_t count = 0u;
    bool known_in_cache = false;
    int rc = -1;

    if (!copy)
        return app_error(app, "model selector is too long");
    parts[count++] = copy;
    for (char *p = copy; *p; ++p) {
        if (*p != '/')
            continue;
        if (count == 3u) {
            (void)app_error(app,
                "model selector has more than three slash-separated components; use a cached number for model IDs containing slash");
            goto out;
        }
        *p = '\0';
        parts[count++] = p + 1u;
    }
    for (size_t i = 0; i < count; ++i) {
        parts[i] = trim_selector_part(parts[i]);
        if (!parts[i][0]) {
            (void)app_error(app, "model selector contains an empty component");
            goto out;
        }
    }
    provider = count == 3u ? snj_config_provider(app->config, parts[0]) :
                             snj_config_provider(app->config, NULL);
    if (!provider) {
        (void)app_error(app, count == 3u ?
            "model selector names an unconfigured provider" :
            "no provider is configured");
        goto out;
    }
    model = parts[count == 3u ? 1u : 0u];
    effort = count >= 2u ? parts[count - 1u] : next_effort(app);
    if (strlen(model) >= SNJ_CONFIG_MODEL_MAX ||
        !snj_utf8_valid((const unsigned char *)model, strlen(model), true) ||
        !resolve_effort(effort)) {
        (void)app_error(app,
            "model or effort exceeds the supported structural bounds");
        goto out;
    }
    {
        char ignored[256] = {0};
        if (snj_model_cache_load(&app->store, &app->model_cache,
                                 ignored, sizeof(ignored)) == 0) {
            const json_t *cached = snj_model_cache_find(&app->model_cache,
                                                        provider->name, model);
            known_in_cache = cached != NULL;
            if (count == 1u)
                effort = snj_model_cache_best_effort(cached,
                                                      resolve_effort(effort));
        }
    }
    rc = commit_model_selection(app, provider, model, resolve_effort(effort),
                                known_in_cache, save);
out:
    free(copy);
    return rc;
}

static bool
strip_model_save_suffix(char *selector)
{
    char *end = selector + strlen(selector);
    char *word;
    size_t len;

    while (end > selector && isspace((unsigned char)end[-1]))
        --end;
    word = end;
    while (word > selector && !isspace((unsigned char)word[-1]))
        --word;
    len = (size_t)(end - word);
    if (word == selector || !((len == 1u && word[0] == 's') ||
                              (len == 4u && memcmp(word, "save", 4u) == 0)))
        return false;
    while (word > selector && isspace((unsigned char)word[-1]))
        --word;
    *word = '\0';
    return true;
}

static int
change_model(struct app_state *app, const char *value, bool active)
{
    char error[256] = {0};
    char *copy = NULL;
    char *selector = NULL;
    bool save = false;
    int rc;

    if (value) {
        copy = snj_strdup_checked(value, SNJ_CONFIG_PATH_MAX);
        if (!copy)
            return app_error(app, "model selector is too long");
        selector = trim_selector_part(copy);
    }
    if (!selector || strcmp(selector, "list") == 0) {
        rc = load_model_cache(app, false, error, sizeof(error));
        if (rc < 0)
            rc = app_error(app, error);
        else
            rc = render_model_catalog(app);
        free(copy);
        return rc;
    }
    if (strcmp(selector, "cache") == 0) {
        if (active)
            rc = app_error(app,
                "/model cache is idle-only; interrupt or wait");
        else if (load_model_cache(app, true, error, sizeof(error)) < 0)
            rc = app_error(app, error);
        else
            rc = render_model_catalog(app);
        free(copy);
        return rc;
    }
    save = strip_model_save_suffix(selector);
    if (active) {
        free(copy);
        return app_error(app,
            "/model selection is idle-only; interrupt or wait");
    }
    rc = select_cached_model(app, selector, save);
    if (rc == 1)
        rc = select_typed_model(app, selector, save);
    free(copy);
    return rc;
}

struct config_snapshot {
    bool exists;
    char sha256[SNJ_SHA256_HEX_LEN + 1u];
};

static int
snapshot_config(const char *path, struct config_snapshot *snapshot,
                char *error, size_t error_size)
{
    struct snj_sha256 digest;
    struct stat st;
    unsigned char hash[32];
    int fd;

    memset(snapshot, 0, sizeof(*snapshot));
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT)
            return 0;
        snj_errorf(error, error_size, "cannot open configuration %s: %s",
                  path, strerror(errno));
        return -1;
    }
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uintmax_t)st.st_size > SNJ_CONFIG_FILE_MAX) {
        snj_errorf(error, error_size,
                  "configuration must be a regular file no larger than 64 KiB");
        errno = EINVAL;
        (void)close(fd);
        return -1;
    }
    snj_sha256_init(&digest);
    for (;;) {
        unsigned char bytes[4096];
        ssize_t got = read(fd, bytes, sizeof(bytes));
        if (got > 0) {
            snj_sha256_update(&digest, bytes, (size_t)got);
            continue;
        }
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0) {
            snj_errorf(error, error_size, "cannot read configuration: %s",
                      strerror(errno));
            (void)close(fd);
            return -1;
        }
        break;
    }
    if (close(fd) < 0) {
        snj_errorf(error, error_size, "cannot close configuration: %s",
                  strerror(errno));
        return -1;
    }
    snj_sha256_final(&digest, hash);
    for (size_t i = 0u; i < sizeof(hash); ++i)
        (void)snprintf(snapshot->sha256 + i * 2u, 3u, "%02x", hash[i]);
    snapshot->exists = true;
    return 0;
}

static bool
same_config_snapshot(const struct config_snapshot *left,
                     const struct config_snapshot *right)
{
    return left->exists == right->exists &&
           (!left->exists || strcmp(left->sha256, right->sha256) == 0);
}

static unsigned int
configured_verbosity(const struct app_state *app,
                     const struct snj_config *config)
{
    unsigned int value = config->verbosity;
    if (value < 6u) {
        unsigned int room = 6u - value;
        value += app->cli->verbosity < room ? app->cli->verbosity : room;
    }
    return value;
}

static enum snj_color_mode
configured_color(const struct app_state *app,
                 const struct snj_config *config)
{
    if (app->cli->color == SNJ_CLI_COLOR_AUTO)
        return SNJ_COLOR_AUTO;
    if (app->cli->color == SNJ_CLI_COLOR_ALWAYS)
        return SNJ_COLOR_ALWAYS;
    if (app->cli->color == SNJ_CLI_COLOR_NEVER)
        return SNJ_COLOR_NEVER;
    return config->color;
}

static bool
configured_markdown(const struct app_state *app,
                    const struct snj_config *config)
{
    if (app->cli->markdown == SNJ_CLI_MARKDOWN_ENABLED)
        return true;
    if (app->cli->markdown == SNJ_CLI_MARKDOWN_DISABLED)
        return false;
    return config->markdown;
}

static bool
irc_config_equal(const struct snj_config *left,
                 const struct snj_config *right)
{
    if (left->irc_listen_explicit != right->irc_listen_explicit ||
        strcmp(left->irc_listen, right->irc_listen) != 0 ||
        left->irc_client_count != right->irc_client_count ||
        strcmp(left->irc_model_nick, right->irc_model_nick) != 0 ||
        strcmp(left->irc_operator_nick, right->irc_operator_nick) != 0 ||
        strcmp(left->irc_room_name, right->irc_room_name) != 0 ||
        left->irc_history_lines != right->irc_history_lines)
        return false;
    for (size_t i = 0u; i < left->irc_client_count; ++i)
        if (strcmp(left->irc_clients[i], right->irc_clients[i]) != 0)
            return false;
    return true;
}

static int
open_configured_irc(struct app_state *app, const struct snj_config *config,
                    char *error, size_t error_size)
{
    if (!snj_irc_enabled(config)) {
        app->irc = NULL;
        return 0;
    }
    if (snj_irc_open(&app->irc, config, app->session.workspace,
                     snj_app_irc_event, snj_app_irc_trace, app,
                     error, error_size) < 0)
        return -1;
    if (snj_app_irc_restore(app, error, error_size) < 0) {
        snj_irc_close(app->irc);
        app->irc = NULL;
        return -1;
    }
    return 0;
}

static int
reload_config(struct app_state *app, char *error, size_t error_size)
{
    struct snj_config candidate;
    struct snj_config previous;
    const char *selected_provider = app->session.default_provider[0] ?
        app->session.default_provider : NULL;
    bool old_networked;
    bool new_networked;
    bool replace_irc;
    int rc = 1;

    snj_config_init(&candidate);
    if (!candidate.shell) {
        snj_errorf(error, error_size, "cannot initialize configuration defaults");
        goto out;
    }
    if (snj_config_load(&candidate,
            app->config_allow_create ? NULL : app->config_path,
            app->store.root_path, error, error_size) < 0 ||
        snj_irc_apply_cli(&candidate, app->cli, error, error_size) < 0)
        goto out;
    if (!snj_config_provider(&candidate, selected_provider)) {
        snj_errorf(error, error_size,
                  "reloaded configuration does not define the selected provider");
        errno = EINVAL;
        goto out;
    }
    if (validate_prompt_candidate(app, &candidate) < 0) {
        snj_errorf(error, error_size,
                  "reloaded prompt cannot be rendered with the current selection");
        errno = EINVAL;
        goto out;
    }
    old_networked = snj_irc_enabled(app->config);
    new_networked = snj_irc_enabled(&candidate);
    replace_irc = old_networked != new_networked ||
                  (old_networked && !irc_config_equal(app->config, &candidate));
    if (replace_irc) {
        snj_irc_close(app->irc);
        app->irc = NULL;
        if (open_configured_irc(app, &candidate, error, error_size) < 0) {
            char replacement_error[256];
            char rollback_error[256] = {0};

            (void)snprintf(replacement_error, sizeof(replacement_error), "%s",
                           error[0] ? error : "IRC replacement failed");
            if (old_networked &&
                open_configured_irc(app, app->config,
                                    rollback_error,
                                    sizeof(rollback_error)) < 0) {
                snj_errorf(error, error_size,
                          "%s; prior IRC configuration could not be restored: %s",
                          replacement_error,
                          rollback_error[0] ? rollback_error : "unknown error");
                rc = -1;
                goto out;
            }
            snj_errorf(error, error_size, "%s; previous configuration remains active",
                      replacement_error);
            goto out;
        }
    }
    previous = *app->config;
    *app->config = candidate;
    memset(&candidate, 0, sizeof(candidate));
    app->networked = new_networked;
    app->turn_provider = snj_config_provider(app->config, selected_provider);
    app->staged_provider = NULL;
    snj_render_set_color(&app->render, configured_color(app, app->config));
    snj_render_set_markdown(&app->render,
                            configured_markdown(app, app->config));
    app->render.verbosity = configured_verbosity(app, app->config);
    snj_render_set_networked(&app->render, app->networked,
                             app->networked ?
                                 app->config->irc_model_nick : NULL);
    snj_term_set_commands(&app->term, commands, command_count(app));
    snj_term_set_typing_pause(&app->term, app->config->typing_pause_ms);
    snj_config_free(&previous);
    if (replace_irc && app->networked &&
        app->config->irc_listen_explicit &&
        snj_app_irc_snapshot(app, "join", error, error_size) < 0) {
        rc = -1;
        goto out;
    }
    rc = 0;
out:
    snj_config_free(&candidate);
    return rc;
}

static int
run_config_editor(struct app_state *app, int *status,
                  char *error, size_t error_size)
{
    const char *editor = getenv("EDITOR");
    pid_t child;
    pid_t got;

    if (!editor || !*editor) {
        snj_errorf(error, error_size, "$EDITOR is not set");
        errno = ENOENT;
        return 1;
    }
    if (snj_term_external_begin(&app->term, error, error_size) < 0)
        return -1;
    child = fork();
    if (child == 0) {
        execl("/bin/sh", "sh", "-c", "exec $EDITOR \"$1\"",
              "snajpagent-editor", app->config_path, (char *)NULL);
        _exit(127);
    }
    if (child < 0) {
        snj_errorf(error, error_size, "cannot start $EDITOR: %s",
                  strerror(errno));
        (void)snj_term_external_end(&app->term, NULL, 0u);
        return -1;
    }
    do {
        got = waitpid(child, status, 0);
    } while (got < 0 && errno == EINTR);
    if (snj_term_external_end(&app->term, error, error_size) < 0)
        return -1;
    if (got != child) {
        snj_errorf(error, error_size, "cannot wait for $EDITOR: %s",
                  strerror(errno));
        return -1;
    }
    return 0;
}

static int
change_config(struct app_state *app, bool active)
{
    struct config_snapshot before;
    struct config_snapshot after;
    char error[256] = {0};
    int editor_status = 0;
    int rc;

    if (active)
        return app_error(app, "/config is idle-only; interrupt or wait");
    if (snapshot_config(app->config_path, &before,
                        error, sizeof(error)) < 0)
        return app_error(app, error);
    rc = run_config_editor(app, &editor_status, error, sizeof(error));
    if (rc != 0)
        return rc < 0 ? -1 : app_error(app, error);
    error[0] = '\0';
    if (snapshot_config(app->config_path, &after,
                        error, sizeof(error)) < 0)
        return app_error(app, error);
    if (same_config_snapshot(&before, &after)) {
        if (!WIFEXITED(editor_status) || WEXITSTATUS(editor_status) != 0)
            return app_error(app, "$EDITOR exited unsuccessfully; configuration unchanged");
        return app_hostf(app, "configuration unchanged: %s", app->config_path);
    }
    error[0] = '\0';
    rc = reload_config(app, error, sizeof(error));
    if (rc != 0) {
        int render_rc = app_error(app, error[0] ? error :
                                  "configuration reload failed");
        return rc < 0 || render_rc < 0 ? -1 : 0;
    }
    if (!WIFEXITED(editor_status) || WEXITSTATUS(editor_status) != 0) {
        if (app_warning(app,
                "$EDITOR exited unsuccessfully after changing the configuration") < 0)
            return -1;
    }
    return app_hostf(app, "configuration reloaded: %s", app->config_path);
}

static int
change_effort(struct app_state *app, const char *value, bool active)
{
    char error[256];
    char *copy = NULL;
    char *effort = NULL;
    if (!value)
        return show_setting(app, "effort", next_effort(app),
                            app->staged_effort != NULL);
    if (active)
        return app_error(app, "/effort LEVEL is idle-only; interrupt or wait");
    copy = snj_strdup_checked(value, SNJ_CONFIG_EFFORT_MAX - 1u);
    if (copy)
        effort = trim_selector_part(copy);
    if (!effort || !resolve_effort(effort)) {
        free(copy);
        return app_error(app,
            "effort exceeds the supported structural bounds");
    }
    app->staged_effort = NULL;
    if (strcmp(effort, app->session.default_effort) != 0) {
        error[0] = '\0';
        if (commit_event(app, "effort_changed",
                snj_app_preference_changed_data("old_effort",
                                        app->session.default_effort,
                                        "new_effort", effort),
                error, sizeof(error)) < 0) {
            (void)app_error(app, error[0] ? error :
                            "effort preference could not be saved");
            free(copy);
            return -1;
        }
    }
    free(copy);
    return show_setting(app, "effort", app->session.default_effort, false);
}
static int
change_verbosity(struct app_state *app, const char *value)
{
    if (!value)
        return app_hostf(app, "verbosity: %u", app->render.verbosity);
    if (value[0] < '0' || value[0] > '6' || value[1] != '\0')
        return app_error(app, "/verbose expects one integer from 0 through 6");
    app->render.verbosity = (unsigned int)(value[0] - '0');
    return app_hostf(app, "verbosity: %u", app->render.verbosity);
}
static int
toggle_view(struct app_state *app)
{
    int rc;

    if (!app->networked)
        return 0;
    rc = snj_render_set_view(&app->render,
        snj_render_view(&app->render) == SNJ_RENDER_CHAT ?
        SNJ_RENDER_ROLLOUT : SNJ_RENDER_CHAT);
    return rc < 0 ? rc : set_input_prompt(app, app->session.active_turn);
}
static int
handle_common_command(struct app_state *app, const char *line, bool active,
                      bool *handled)
{
    char error[256] = {0};

    *handled = true;
    if (strcmp(line, "/help") == 0 || strcmp(line, "/?") == 0)
        return render_help(app);
    if (strcmp(line, "/status") == 0)
        return render_status(app);
    if (strcmp(line, "/history") == 0)
        return snj_render_history(&app->render, &app->session);
    if (app->networked && strcmp(line, "/chat") == 0) {
        int rc = snj_render_set_view(&app->render, SNJ_RENDER_CHAT);
        return rc < 0 ? rc : set_input_prompt(app, active);
    }
    if (app->networked && strcmp(line, "/rollout") == 0) {
        int rc = snj_render_set_view(&app->render, SNJ_RENDER_ROLLOUT);
        return rc < 0 ? rc : set_input_prompt(app, active);
    }
    if (strcmp(line, "/verbose") == 0)
        return change_verbosity(app, NULL);
    if (strncmp(line, "/verbose ", 9u) == 0)
        return change_verbosity(app, line + 9u);
    if (strcmp(line, "/model") == 0)
        return change_model(app, NULL, active);
    if (strncmp(line, "/model ", 7u) == 0)
        return change_model(app, line + 7u, active);
    if (strcmp(line, "/config") == 0)
        return change_config(app, active);
    if (strcmp(line, "/effort") == 0)
        return change_effort(app, NULL, active);
    if (strncmp(line, "/effort ", 8u) == 0)
        return change_effort(app, line + 8u, active);
    if (strcmp(line, "/goal") == 0 || strncmp(line, "/goal ", 6u) == 0)
        return snj_app_goal_command(app, line, active);
    if (app->networked &&
        (strcmp(line, "/names") == 0 || strcmp(line, "/topic") == 0)) {
        struct snj_buf state;
        int rc;

        snj_buf_init(&state, SNJ_MAX_IRC_SNAPSHOT);
        rc = snj_irc_snapshot(app->irc, &state, error, sizeof(error));
        if (rc == 0)
            rc = snj_buf_terminate(&state);
        if (rc == 0)
            rc = snj_render_host(&app->render, (const char *)state.data);
        snj_buf_free(&state);
        return rc < 0 ? app_error(app, error[0] ? error :
                                  "IRC state could not be displayed") : 0;
    }
    if (app->networked && strncmp(line, "/topic ", 7u) == 0) {
        if (snj_irc_set_operator_topic(app->irc, line + 7u,
                                       error, sizeof(error)) < 0)
            return app_error(app, error[0] ? error :
                              "IRC topic could not be changed");
        return 0;
    }
    *handled = false;
    return 0;
}
int
snj_app_active_input_pump(void *opaque, unsigned int timeout_ms)
{
    struct app_state *app = opaque;
    enum snj_term_action action = SNJ_TERM_NONE;
    char *line = NULL;
    char error[256];
    int rc;
    if (capture_shutdown_signal(app)) {
        app->interrupt_requested = true;
        return 2;
    }
    if (app->networked) {
        error[0] = '\0';
        if (tick_irc(app, error, sizeof(error)) < 0) {
            (void)snj_render_error_ctx(&app->render,
                error[0] ? error : "IRC event loop failed");
            return -1;
        }
        if (timeout_ms > 25u)
            timeout_ms = 25u;
    }
    if (app->execute || app->input_closed)
        return 0;
    rc = snj_term_poll(&app->term, (int)timeout_ms, &action, &line);
    history_warning(app);
    if (rc < 0) {
        if (capture_shutdown_signal(app)) {
            app->interrupt_requested = true;
            free(line);
            return 2;
        }
        (void)snj_render_error_ctx(&app->render,
            errno == EOVERFLOW ? "active submission exceeds 1 MiB" :
            errno == EILSEQ ? "active submission contains invalid UTF-8" :
            "active input could not be read");
        return 0;
    }
    if (rc == 0) {
        if (app->networked) {
            error[0] = '\0';
            if (tick_irc(app, error, sizeof(error)) < 0) {
                (void)snj_render_error_ctx(&app->render,
                    error[0] ? error : "IRC event loop failed");
                return -1;
            }
        }
        return 0;
    }
    if (action == SNJ_TERM_EXIT) {
        app->input_closed = true;
        free(line);
        return 0;
    }
    if ((action == SNJ_TERM_CANCEL || action == SNJ_TERM_INTERRUPT) &&
        app->queue_edit_id[0]) {
        app->queue_armed = app->queue_edit_was_armed;
        app->queue_edit_id[0] = '\0';
        app->queue_edit_number = 0u;
        app->queue_edit_was_armed = false;
        return set_input_prompt(app, true);
    }
    if (action == SNJ_TERM_CANCEL) {
        return 0;
    }
    if (action == SNJ_TERM_INTERRUPT) {
        app->interrupt_requested = true;
        free(line);
        return 2;
    }
    if (action == SNJ_TERM_VIEW) {
        if (app->queue_edit_id[0]) {
            (void)snj_render_error_ctx(&app->render,
                "queue replacement must be nonempty");
            return 0;
        }
        return toggle_view(app);
    }
    if (!line)
        return 0;
    remember_input(app, line);
    error[0] = '\0';
    if (app->queue_edit_id[0]) {
        rc = finish_queue_edit(app, line, true, error, sizeof(error));
        if (rc != 0 && error[0])
            (void)snj_render_error_ctx(&app->render, error);
    } else if (action == SNJ_TERM_QUEUE) {
        rc = queue_future_turn(app, line, true, error, sizeof(error));
        if (rc != 0) {
            (void)snj_render_error_ctx(&app->render, error);
            if (set_input_prompt(app, true) < 0 ||
                snj_term_restore_draft(&app->term, line) < 0)
                rc = -1;
        } else rc = set_input_prompt(app, true);
    } else {
        bool single_line = strchr(line, '\n') == NULL;
        bool handled = false;
        if (single_line && line[0] == '/' && line[1] != '/') {
            rc = handle_common_command(app, line, true, &handled);
            if (rc < 0)
                goto active_done;
        }
        if (!handled && single_line) {
            rc = handle_queue_command(app, line, true, &handled,
                                      error, sizeof(error));
            if (rc != 0 && error[0])
                (void)snj_render_error_ctx(&app->render, error);
            if (rc < 0)
                goto active_done;
        }
        if (handled) {
            if (!app->queue_edit_id[0] && set_input_prompt(app, true) < 0)
                rc = -1;
        } else if (single_line && line[0] == '/' && line[1] != '/') {
            (void)snj_render_error_ctx(&app->render,
                "that command is unavailable while a turn is active");
            rc = set_input_prompt(app, true);
        } else {
            const char *text = line[0] == '/' && line[1] == '/' ? line + 1 : line;
            char steering_id[SNJ_ID_HEX_LEN + 1u];
            size_t len = strlen(text);
            if (!len || len > SNJ_MAX_STEERING_TEXT) {
                (void)snj_render_error_ctx(&app->render,
                    "active-turn input must be nonempty valid UTF-8 within 256 KiB");
                rc = 0;
            } else if (app->networked) {
                error[0] = '\0';
                rc = snj_irc_send_operator(app->irc, text,
                                           error, sizeof(error));
                if (rc < 0) {
                    (void)snj_render_error_ctx(&app->render,
                        error[0] ? error : "IRC message could not be queued");
                    if (set_input_prompt(app, true) == 0)
                        (void)snj_term_restore_draft(&app->term, line);
                } else rc = set_input_prompt(app, true);
            } else if (snj_random_id(steering_id) < 0) {
                rc = -1;
            } else {
                rc = commit_event(app, "steering_added",
                        snj_app_steering_added_data(app->session.active_turn_id,
                                            steering_id, text),
                        error, sizeof(error));
                if (rc == 0 && snj_render_input_submitted(&app->render,
                        snj_term_prompt_label(&app->term), text) < 0)
                    rc = -1;
                if (rc < 0) {
                    (void)snj_render_error_ctx(&app->render, error[0] ? error :
                                               "active-turn input could not be persisted");
                    if (set_input_prompt(app, true) == 0)
                        (void)snj_term_restore_draft(&app->term, line);
                } else {
                    app->steering_requested = true;
                    rc = set_input_prompt(app, true);
                }
            }
            if (!app->steering_requested && rc >= 0 &&
                set_input_prompt(app, true) < 0)
                rc = -1;
        }
    }
active_done:
    free(line);
    if (rc < 0)
        return -1;
    return app->steering_requested ? 1 : 0;
}
static int
commit_pending_result(struct app_state *app, const char *turn_id,
                      const char *call_id, json_t *result,
                      char *error, size_t error_size)
{
    json_t *data = snj_app_tool_finished_data(turn_id, call_id, result);
    if (!data) {
        snj_errorf(error, error_size, "cannot allocate tool completion event");
        return -1;
    }
    return commit_event(app, "tool_finished", data, error, error_size);
}
static int
terminalize_pending(struct app_state *app, const char *turn_id,
                    const char *unstarted_reason, char *error,
                    size_t error_size)
{
    size_t count = app->session.pending_call_count;
    for (size_t i = 0; i < count; ++i) {
        struct snj_pending_call *call = &app->session.pending_calls[i];
        json_t *result;
        if (call->finished)
            continue;
        result = call->started ? snj_tool_result_outcome_unknown("owner_lost") :
                                snj_tool_result_not_run(unstarted_reason);
        if (!result || commit_pending_result(app, turn_id, call->call_id, result,
                                             error, error_size) < 0)
            return -1;
    }
    return 0;
}
static int
close_active_process_for_turn(struct app_state *app, const char *turn_id,
                              const char *cause, bool user_interrupt,
                              char *error, size_t error_size)
{
    char handle[SNJ_ID_HEX_LEN + 1u];
    json_t *result = NULL;
    json_t *data;
    if (!app->session.active_process_handle[0])
        return 0;
    memcpy(handle, app->session.active_process_handle, sizeof(handle));
#ifdef SNAJPAGENT_TEST_FIXTURE
    (void)user_interrupt;
    result = snj_tool_result_outcome_unknown("owner_lost");
#else
    if (snj_tools_close_managed(handle, user_interrupt, snj_app_active_input_pump, app,
                                &result, error, error_size) < 0)
        return -1;
#endif
    data = snj_app_process_closed_data(turn_id, handle, cause, result);
    if (!data) {
        snj_errorf(error, error_size, "cannot allocate process closure event");
        return -1;
    }
    return commit_event(app, "process_closed", data, error, error_size);
}
static int
fail_turn(struct app_state *app, const char *turn_id, const char *cause,
          const char *class_name, const char *message,
          char *error, size_t error_size)
{
    return close_active_process_for_turn(app, turn_id, cause, false,
                                         error, error_size) < 0 ||
           commit_event(app, "turn_failed",
                        snj_app_turn_failed_data(turn_id, class_name, message),
                        error, error_size) < 0 ? -1 : 0;
}

static int
interrupt_turn(struct app_state *app, const char *turn_id,
               const char *cause, bool user_interrupt,
               const char *origin, const char *reason,
               char *error, size_t error_size)
{
    return close_active_process_for_turn(app, turn_id, cause, user_interrupt,
                                         error, error_size) < 0 ||
           commit_event(app, "turn_interrupted",
                        snj_app_turn_interrupted_data(turn_id, origin, reason),
                        error, error_size) < 0 ? -1 : 0;
}

static int
fail_response(struct app_state *app, const char *turn_id,
              const char *response_id, unsigned int cycle,
              const char *class_name, const char *message, json_t *partial,
              unsigned int retry_count, const char *cause,
              char *error, size_t error_size)
{
    json_t *data = snj_app_response_failed_data(
        turn_id, response_id, cycle, class_name, message, partial, retry_count);

    if (!data) {
        snj_errorf(error, error_size, "cannot allocate response failure event");
        return -1;
    }
    return commit_event(app, "response_failed", data, error, error_size) < 0 ||
           fail_turn(app, turn_id, cause, class_name, message,
                     error, error_size) < 0 ? -1 : 0;
}

static int
recover_session(struct app_state *app, char *error, size_t error_size)
{
    char turn_id[SNJ_ID_HEX_LEN + 1u];
    const char *message;
    bool has_steering;
    if (!app->session.active_turn)
        return 0;
    memcpy(turn_id, app->session.active_turn_id, sizeof(turn_id));
    has_steering = app->session.pending_steering_count != 0u;
    if (app->session.response_open) {
        if (commit_event(app, "response_interrupted",
                         snj_app_response_interrupted_data(turn_id,
                                                   app->session.active_response_id,
                                                   app->session.active_cycle,
                                                   "recovery", "process_lost",
                                                   NULL),
                         error, error_size) < 0 ||
            interrupt_turn(app, turn_id, "internal_failure", false,
                           "recovery", "session_recovered",
                           error, error_size) < 0)
            return -1;
        return app_warning(app, "recovered an interrupted turn");
    }
    if (app->session.response_complete) {
        if (app->session.response_outcome == SNJ_GRAPH_CONFLICT) {
            message = "provider response had conflicting terminal actions";
            if (terminalize_pending(app, turn_id, "protocol_conflict",
                                    error, error_size) < 0 ||
                fail_turn(app, turn_id, "protocol_failure",
                          "protocol", message, error, error_size) < 0)
                return -1;
            return app_warning(app, "recovered a protocol-conflicted turn");
        }
        if (has_steering) {
            if (app->session.response_outcome == SNJ_GRAPH_CALLS &&
                terminalize_pending(app, turn_id, "superseded_by_steering",
                                    error, error_size) < 0)
                return -1;
            if (interrupt_turn(app, turn_id, "internal_failure", false,
                               "recovery", "session_recovered",
                               error, error_size) < 0)
                return -1;
            return app_warning(app,
                "recovered a turn whose pending active-turn input could not be resumed automatically");
        }
        switch (app->session.response_outcome) {
        case SNJ_GRAPH_FINAL:
        case SNJ_GRAPH_REFUSAL:
            if (app->session.active_process_handle[0] != '\0') {
                message = "recovered terminal response while a managed process was unresolved";
                if (fail_turn(app, turn_id, "protocol_failure",
                              "protocol", message, error, error_size) < 0)
                    return -1;
                return app_warning(app, "recovered a terminal response that violated managed process ordering");
            }
            if (commit_event(app, "turn_completed",
                             snj_app_turn_completed_data(turn_id,
                                                 app->session.final_response_id,
                                                 app->session.final_item_id),
                             error, error_size) < 0)
                return -1;
            return app_warning(app, "recovered a durably completed turn");
        case SNJ_GRAPH_CALLS:
            if (terminalize_pending(app, turn_id, "recovery_unstarted",
                                    error, error_size) < 0 ||
                interrupt_turn(app, turn_id, "internal_failure", false,
                               "recovery", "session_recovered",
                               error, error_size) < 0)
                return -1;
            return app_warning(app, "recovered a turn with unfinished tool work");
        case SNJ_GRAPH_CONFLICT:
            break;
        case SNJ_GRAPH_NONPRODUCTIVE:
            message = "provider completed without a final answer, refusal, or tool call";
            if (fail_turn(app, turn_id, "protocol_failure",
                          "protocol", message, error, error_size) < 0)
                return -1;
            return app_warning(app, "recovered a nonproductive response");
        }
    }
    if (app->session.response_terminal == SNJ_RESPONSE_TERMINAL_FAILED) {
        message = "provider response had already failed before process recovery";
        if (fail_turn(app, turn_id, "provider_failure",
                      "provider", message, error, error_size) < 0)
            return -1;
        return app_warning(app, "recovered a provider-failed turn");
    }
    if (interrupt_turn(app, turn_id, "internal_failure", false,
                       "recovery", "session_recovered",
                       error, error_size) < 0)
        return -1;
    return app_warning(app, "recovered an interrupted turn");
}
static int
execute_calls(struct app_state *app, const char *turn_id,
              const struct snj_response_graph *graph,
              const struct snj_credential *credential,
              char *error, size_t error_size)
{
    for (size_t i = 0; i < graph->count; ++i) {
        const struct snj_response_item *call = &graph->items[i];
        char digest[SNJ_SHA256_HEX_LEN + 1u];
        json_t *result = NULL;
        char tool_error[256];
        if (call->kind != SNJ_ITEM_TOOL_CALL)
            continue;
        if (snj_tool_action_digest(call, app->session.workspace, digest) < 0) {
            snj_errorf(error, error_size, "cannot digest tool action");
            return -1;
        }
        if (commit_event(app, "tool_started",
                         snj_app_tool_started_data(turn_id, call->call_id, digest,
                                           app->session.workspace),
                         error, error_size) < 0)
            return -1;
        if (snj_render_tool_start(&app->render, call,
                                  app->session.workspace,
                                  app->config->default_timeout_ms) < 0) {
            snj_errorf(error, error_size, "tool activity could not be rendered");
            return -1;
        }
        tool_error[0] = '\0';
        {
            int run_rc;

            app->tool_active = true;
            if (snj_term_set_spinner_states(&app->term,
                    prompt_spinner_states(app, true)) < 0) {
                app->tool_active = false;
                snj_errorf(error, error_size,
                          "tool status could not be displayed");
                return -1;
            }
            run_rc = snj_app_tool_run(app, call, credential, &result, tool_error,
                                      sizeof(tool_error));
            app->tool_active = false;
            if (snj_term_set_spinner_states(&app->term,
                    prompt_spinner_states(app, true)) < 0) {
                if (result)
                    json_decref(result);
                snj_errorf(error, error_size,
                          "provider status could not be restored");
                return -1;
            }
            if (run_rc == 2) {
                if (!result)
                    result = snj_tool_result_not_run("turn_cancelled");
                if (!result ||
                    snj_tools_attach_output_limit(call, app->config,
                                                  result) < 0 ||
                    commit_pending_result(app, turn_id, call->call_id, result,
                                          error, error_size) < 0 ||
                    terminalize_pending(app, turn_id, "turn_cancelled",
                                        error, error_size) < 0 ||
                    interrupt_turn(app, turn_id, "user_interrupt", true,
                                   "user", "cancelled",
                                   error, error_size) < 0)
                    return -1;
                snj_errorf(error, error_size, "turn cancelled");
                return 2;
            }
            if (run_rc < 0) {
                if (result) {
                    json_decref(result);
                    result = NULL;
                }
                if (!tool_error[0])
                    (void)snprintf(tool_error, sizeof(tool_error),
                                   "tool adapter failed");
                result = snj_tool_result_terminal(false, tool_error);
            }
        }
        if (!result)
            return -1;
        if (snj_tools_attach_output_limit(call, app->config, result) < 0) {
            json_decref(result);
            snj_errorf(error, error_size,
                      "command output token limit could not be retained");
            return -1;
        }
        {
            const char *status = snj_json_string(result, "status");
            bool yielded = status && strcmp(status, "running") == 0;
            json_t *render_result = app->render.verbosity >= 1u ?
                                    json_incref(result) : NULL;
            if (commit_pending_result(app, turn_id, call->call_id, result,
                                      error, error_size) < 0) {
                if (render_result)
                    json_decref(render_result);
                return -1;
            }
            if (render_result &&
                snj_render_tool_finish(&app->render, call->name,
                                       render_result,
                                       app->config->max_output_bytes) < 0) {
                json_decref(render_result);
                snj_errorf(error, error_size,
                          "tool result could not be rendered");
                return -1;
            }
            if (render_result)
                json_decref(render_result);
            if (yielded &&
                terminalize_pending(app, turn_id,
                                    "process_interaction_required",
                                    error, error_size) < 0)
                return -1;
            if (yielded)
                return 0;
        }
    }
    return 0;
}

static json_t *
silent_turn_data(const char *turn_id, const char *response_id,
                 const char *reason)
{
    json_t *data = json_object();

    if (!data ||
        snj_json_set_new(data, "reason", json_string(reason)) < 0 ||
        snj_json_set_new(data, "response_id", json_string(response_id)) < 0 ||
        snj_json_set_new(data, "turn_id", json_string(turn_id)) < 0) {
        if (data)
            json_decref(data);
        return NULL;
    }
    return data;
}

static int
run_turn(struct app_state *app, const char *prompt,
         const struct snj_queued_turn *queued, bool goal_turn)
{
    char turn_id[SNJ_ID_HEX_LEN + 1u];
    char response_id[SNJ_ID_HEX_LEN + 1u];
    char input_hash[SNJ_SHA256_HEX_LEN + 1u];
    char request_hash[SNJ_SHA256_HEX_LEN + 1u];
    char count_request_hash[SNJ_SHA256_HEX_LEN + 1u];
    char error[256];
    uint64_t input_tokens_bound = 0;
    uint64_t model_input_bytes = 0;
    uint64_t request_input_bytes = 0;
    uint64_t request_input_count = 0;
    char request_input_hash[SNJ_SHA256_HEX_LEN + 1u];
    char provider_source_hash[SNJ_SHA256_HEX_LEN + 1u];
    char rejected_request_hash[SNJ_SHA256_HEX_LEN + 1u] = {0};
    char over_budget_request_hash[SNJ_SHA256_HEX_LEN + 1u] = {0};
    unsigned int hard_compaction_attempts = 0u;
    bool capacity_recovery_used = false;
    char *turn_prompt;
    struct snj_credential credential;
    size_t prompt_max = queued ? SNJ_MAX_QUEUED_TEXT : SNJ_MAX_DIRECT_PROMPT;
    int result = 4;
    snj_credential_clear(&credential);
    app->last_turn_refused = false;
    app->irc_turn_replied = false;
    error[0] = '\0';
    if (!*prompt || strlen(prompt) > prompt_max ||
        !snj_utf8_valid((const unsigned char *)prompt, strlen(prompt), true)) {
        (void)app_error(app, queued ?
            "queued prompt must be nonempty valid UTF-8 within 256 KiB" :
            "prompt must be nonempty valid UTF-8 within 1 MiB");
        return 2;
    }
    if (prepare_turn_settings(app, error, sizeof(error)) < 0) {
        (void)app_error(app, error);
        return 2;
    }
    provider_capacity_source_sha256(app->turn_provider, provider_source_hash);
#ifndef SNAJPAGENT_TEST_FIXTURE
    if (snj_credential_read(&credential, app->turn_provider->api_key_env,
                            error, sizeof(error)) < 0) {
        (void)app_error(app, error);
        snj_credential_clear(&credential);
        return 2;
    }
#endif
    turn_prompt = snj_strdup_checked(prompt, prompt_max);
    if (!turn_prompt) {
        (void)app_error(app, "cannot retain turn input");
        return 3;
    }
    if (app->config->read_agents_md) {
        if (snj_instructions_discover(&app->turn_instructions,
                                      app->session.workspace,
                                      error, sizeof(error)) < 0) {
            (void)app_error(app, error);
            result = 3;
            goto out;
        }
    } else {
        snj_instructions_free(&app->turn_instructions);
    }
    if (snj_random_id(turn_id) < 0) {
        (void)app_error(app, "cryptographic turn id generation failed");
        result = 3;
        goto out;
    }
    if (commit_event(app, "turn_started",
                     snj_app_turn_started_data(app, turn_prompt, turn_id, queued,
                                               goal_turn),
                     error, sizeof(error)) < 0) {
        (void)app_error(app, error);
        result = 3;
        goto out;
    }
    consume_staged_settings(app);
    if (app_runtimef(app,
            "turn › %s started · model=%s · effort=%s · workspace=%s",
            turn_id, app->turn_model, app->turn_effort, app->session.workspace) < 0) {
        (void)app_error(app, "turn runtime facts could not be rendered");
        result = 6;
        goto out;
    }
    if (!app->execute && set_input_prompt(app, true) < 0) {
        (void)app_error(app, "active composer could not be displayed");
        result = 6;
        goto out;
    }
    for (unsigned int cycle = 1u; cycle != 0u; ++cycle) {
        struct snj_response_graph graph;
        struct snj_graph_decision decision;
        json_t *steering = NULL;
        json_t *create_request = NULL;
        json_t *count_request = NULL;
        const char *count_method = "qualified_upper_bound";
        struct snj_buf request_body;
        uint64_t response_begin_ms;
        unsigned int provider_retry_count = 0u;
        struct snj_provider_failure provider_failure;
        int provider_rc;
        memset(&provider_failure, 0, sizeof(provider_failure));
        error[0] = '\0';
        if (snj_app_irc_flush_urgent(app, error, sizeof(error)) < 0) {
            (void)app_error(app, error[0] ? error :
                            "urgent IRC input could not be admitted");
            result = 3;
            goto out;
        }
        snj_response_graph_init(&graph);
        snj_buf_init(&request_body, SNJ_WIRE_BODY_MAX);
        steering = snj_app_steering_snapshot(&app->session);
        error[0] = '\0';
        if (!steering || snj_random_id(response_id) < 0 ||
            snj_app_request_digests(app, turn_prompt, steering, cycle, &credential,
                            input_hash, request_hash, count_request_hash,
                            &input_tokens_bound,
                            &model_input_bytes, &request_input_bytes,
                            &request_input_count, request_input_hash,
                            provider_source_hash,
                            &count_method,
                            &request_body, &create_request, &count_request,
                            error, sizeof(error)) < 0) {
            snj_app_response_cycle_release(app, &graph, &steering,
                                           &create_request, &count_request,
                                           &request_body);
            if (commit_event(app, "turn_failed",
                             snj_app_turn_failed_data(turn_id, "context",
                                 error[0] ? error :
                                 "response context projection failed"),
                             error, sizeof(error)) < 0) {
                (void)app_error(app, error);
                result = 3;
            } else {
                (void)app_error(app, error[0] ? error :
                                "response context projection failed");
                result = 4;
            }
            goto out;
        }
        provider_rc = snj_app_provider_count(app, count_request, &credential,
            model_input_bytes, &input_tokens_bound, &count_method,
            error, sizeof(error));
        if (provider_rc == 1 && app->steering_requested) {
            snj_app_response_cycle_release(app, &graph, &steering,
                                           &create_request, &count_request,
                                           &request_body);
            app->steering_requested = false;
            --cycle;
            continue;
        }
        if (provider_rc == 2 && app->interrupt_requested) {
            snj_app_response_cycle_release(app, &graph, &steering,
                                           &create_request, &count_request,
                                           &request_body);
            if (interrupt_turn(app, turn_id, "user_interrupt", true,
                               "user", "cancelled",
                               error, sizeof(error)) < 0) {
                (void)app_error(app, error[0] ? error :
                                "interruption could not be persisted");
                result = 3;
            } else {
                (void)app_warning(app, "turn interrupted");
                result = app->execute ? 6 : 1;
            }
            goto out;
        }
        if (provider_rc != 0 && provider_rc != SNJ_APP_COUNT_SKIPPED) {
            snj_app_response_cycle_release(app, &graph, &steering,
                                           &create_request, &count_request,
                                           &request_body);
            if (commit_event(app, "turn_failed",
                             snj_app_turn_failed_data(turn_id, "provider",
                                 error[0] ? error :
                                 "input-token count failed"),
                             error, sizeof(error)) < 0) {
                (void)app_error(app, error);
                result = 3;
            } else {
                (void)app_error(app, error[0] ? error :
                                "input-token count failed");
                result = 4;
            }
            goto out;
        }
        if (app->networked && app->irc_urgent.len) {
            snj_app_response_cycle_release(app, &graph, &steering,
                                           &create_request, &count_request,
                                           &request_body);
            --cycle;
            continue;
        }
        {
            bool compacted = false;
            bool over_hard = app->turn_capacity.hard_input_known &&
                input_tokens_bound > app->turn_capacity.hard_input_tokens;
            int compact_rc;

            if (over_hard &&
                (hard_compaction_attempts >= 8u ||
                 strcmp(over_budget_request_hash, request_hash) == 0)) {
                char failure[256];

                (void)snprintf(failure, sizeof(failure),
                    "%s while reducing context estimate %llu (%s) to hard budget %llu",
                    hard_compaction_attempts >= 8u ?
                        "context compaction reached its eight-attempt bound" :
                        "context compaction repeated an over-budget request",
                    (unsigned long long)input_tokens_bound, count_method,
                    (unsigned long long)app->turn_capacity.hard_input_tokens);
                snj_app_response_cycle_release(app, &graph, &steering,
                                               &create_request, &count_request,
                                               &request_body);
                if (commit_event(app, "turn_failed",
                        snj_app_turn_failed_data(turn_id, "context", failure),
                        error, sizeof(error)) < 0) {
                    (void)app_error(app, error);
                    result = 3;
                } else {
                    (void)app_error(app, failure);
                    result = 4;
                }
                goto out;
            }
            if (over_hard)
                memcpy(over_budget_request_hash, request_hash,
                       sizeof(over_budget_request_hash));
            compact_rc = snj_app_compact_before_response(app, &credential,
                    input_tokens_bound, count_method, &compacted,
                    error, sizeof(error));
            if (compact_rc == 1 && app->steering_requested) {
                snj_app_response_cycle_release(app, &graph, &steering,
                                               &create_request, &count_request,
                                               &request_body);
                app->steering_requested = false;
                --cycle;
                continue;
            }
            if (compact_rc == 2 && app->interrupt_requested) {
                snj_app_response_cycle_release(app, &graph, &steering,
                                               &create_request, &count_request,
                                               &request_body);
                if (close_active_process_for_turn(app, turn_id,
                        "user_interrupt", true, error, sizeof(error)) < 0 ||
                    commit_event(app, "turn_interrupted",
                        snj_app_turn_interrupted_data(turn_id, "user",
                                                      "cancelled"),
                        error, sizeof(error)) < 0) {
                    (void)app_error(app, error[0] ? error :
                                    "interruption could not be persisted");
                    result = 3;
                } else {
                    (void)app_warning(app, "turn interrupted");
                    result = app->execute ? 6 : 1;
                }
                goto out;
            }
            if (compact_rc != 0) {
                snj_app_response_cycle_release(app, &graph, &steering,
                                               &create_request, &count_request,
                                               &request_body);
                if (commit_event(app, "turn_failed",
                                 snj_app_turn_failed_data(turn_id,
                                     over_hard ? "context" : "provider",
                                     error[0] ? error :
                                     "pre-response compaction failed"),
                                 error, sizeof(error)) < 0) {
                    (void)app_error(app, error);
                    result = 3;
                } else {
                    (void)app_error(app, error[0] ? error :
                                    "pre-response compaction failed");
                    result = 4;
                }
                goto out;
            }
            if (compacted) {
                if (over_hard)
                    ++hard_compaction_attempts;
                snj_app_response_cycle_release(app, &graph, &steering,
                                               &create_request, &count_request,
                                               &request_body);
                --cycle;
                continue;
            }
        }
        if (capacity_recovery_used &&
            strcmp(rejected_request_hash, request_hash) == 0) {
            static const char failure[] =
                "capacity recovery produced an identical provider request";
            snj_app_response_cycle_release(app, &graph, &steering,
                                           &create_request, &count_request,
                                           &request_body);
            if (commit_event(app, "turn_failed",
                    snj_app_turn_failed_data(turn_id, "context", failure),
                    error, sizeof(error)) < 0) {
                (void)app_error(app, error);
                result = 3;
            } else {
                (void)app_error(app, failure);
                result = 4;
            }
            goto out;
        }
        if (commit_event(app, "response_started",
                         snj_app_response_started_data(turn_id, response_id, cycle,
                                               app->session.compact_id,
                                               app->turn_model,
                                               input_hash, request_hash,
                                               count_request_hash, count_method,
                                               input_tokens_bound,
                                               model_input_bytes,
                                               request_input_bytes,
                                               request_input_count,
                                               request_input_hash,
                                               strcmp(count_method,
                                                   "anchored_upper_bound") == 0 ?
                                                   app->session.usage_anchor_model_input_sha256 :
                                                   NULL,
                                               app->turn_provider->name,
                                               app->turn_effort,
                                               provider_source_hash,
                                               &app->turn_capacity,
                                               steering),
                         error, sizeof(error)) < 0) {
            snj_app_response_cycle_release(app, &graph, &steering,
                                           &create_request, &count_request,
                                           &request_body);
            (void)app_error(app, error[0] ? error :
                                   "response setup could not be persisted");
            result = 3;
            goto out;
        }
        if (!app->execute && set_input_prompt(app, true) < 0) {
            snj_app_response_cycle_release(app, &graph, &steering,
                                           &create_request, &count_request,
                                           &request_body);
            (void)app_error(app, "context meter could not be displayed");
            result = 6;
            goto out;
        }
        json_decref(count_request);
        count_request = NULL;
        if (app_runtimef(app,
                "response › %s started · turn=%s · cycle=%u · model=%s · profile=%s",
                response_id, turn_id, cycle, app->turn_model,
                SNAJPAGENT_PROFILE_ID) < 0) {
            json_decref(steering);
            if (create_request)
                json_decref(create_request);
            snj_buf_free(&request_body);
            snj_response_graph_free(&graph);
            (void)app_error(app, "response runtime facts could not be rendered");
            result = 6;
            goto out;
        }
        if (request_body.len &&
            snj_render_protocol(&app->render, "request.body",
                                (const char *)request_body.data,
                                request_body.len) < 0) {
            json_t *partial = json_array();
            static const char failure[] =
                "request diagnostics could not be rendered";
            snj_buf_free(&request_body);
            if (create_request)
                json_decref(create_request);
            json_decref(steering);
            if (!partial || fail_response(app, turn_id, response_id, cycle,
                                          "output", failure, partial, 0u,
                                          "output_failure", error,
                                          sizeof(error)) < 0) {
                snj_response_graph_free(&graph);
                (void)app_error(app, error[0] ? error :
                                "diagnostic output failure could not be persisted");
                result = 3;
                goto out;
            }
            snj_response_graph_free(&graph);
            (void)app_error(app, failure);
            result = 6;
            goto out;
        }
        snj_buf_free(&request_body);
        app->stream_graph = &graph;
        snj_app_reset_stream(app);
        response_begin_ms = snj_time_ms();
        error[0] = '\0';
        provider_rc = snj_app_provider_run(app, turn_prompt, steering, cycle,
                                   create_request, &credential, &graph,
                                   &provider_failure,
                                   error, sizeof(error), &provider_retry_count);
        json_decref(create_request);
        json_decref(steering);
        if ((provider_rc == 1 && app->steering_requested) ||
            (provider_rc == 2 && app->interrupt_requested) ||
            provider_failure.output_correction !=
                SNJ_OUTPUT_CORRECTION_NONE) {
            if (snj_app_abort_stream_item(app) < 0)
                app->stream_failed = true;
        } else if (snj_app_finish_stream_item(app) < 0) {
            app->stream_failed = true;
        }
        if (provider_rc == 1 && app->steering_requested && !app->stream_failed) {
            json_t *partial = snj_app_partial_public_json(app);
            if (!partial ||
                commit_event(app, "response_interrupted",
                    snj_app_response_interrupted_data(turn_id, response_id, cycle,
                                              "steering", "steered", partial),
                    error, sizeof(error)) < 0) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                (void)app_error(app, error[0] ? error :
                                       "active-turn response could not be persisted");
                result = 3;
                goto out;
            }
            snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
            continue;
        }
        if (provider_rc == 2 && app->interrupt_requested && !app->stream_failed) {
            json_t *partial = snj_app_partial_public_json(app);
            if (!partial ||
                commit_event(app, "response_interrupted",
                    snj_app_response_interrupted_data(turn_id, response_id, cycle,
                                              "user", "cancelled", partial),
                    error, sizeof(error)) < 0 ||
                interrupt_turn(app, turn_id, "user_interrupt", true,
                               "user", "cancelled",
                               error, sizeof(error)) < 0) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                (void)app_error(app, error[0] ? error :
                                "interruption could not be persisted");
                result = 3;
                goto out;
            }
            snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
            (void)app_warning(app, "turn interrupted");
            result = app->execute ? 6 : 1;
            goto out;
        }
        if (provider_failure.output_correction !=
                SNJ_OUTPUT_CORRECTION_NONE && !app->stream_failed) {
            static const char repeated[] =
                "assistant output remained invalid after one model-facing correction";
            const char *correction =
                provider_failure.output_correction ==
                    SNJ_OUTPUT_CORRECTION_EMPTY ?
                    SNJ_EMPTY_OUTPUT_CORRECTION :
                    SNJ_OVERSIZED_OUTPUT_CORRECTION;
            char correction_id[SNJ_ID_HEX_LEN + 1u];

            if (app->session.output_correction_used) {
                app->stream_failed = true;
                app->stream_errno = EPROTO;
                (void)snprintf(app->stream_error,
                               sizeof(app->stream_error), "%s", repeated);
            } else {
                json_t *partial;

                if (snj_random_id(correction_id) < 0)
                    partial = NULL;
                else
                    partial = snj_app_partial_public_json(app);
                if (!partial ||
                    commit_event(app, "response_output_correction",
                        snj_app_response_output_correction_data(
                            turn_id, response_id, cycle, correction_id,
                            correction, partial),
                        error, sizeof(error)) < 0) {
                    snj_app_response_cycle_release(app, &graph, NULL, NULL,
                                                   NULL, NULL);
                    (void)app_error(app, error[0] ? error :
                        "assistant output correction could not be persisted");
                    result = 3;
                    goto out;
                }
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL,
                                               NULL);
                continue;
            }
        }
        if (provider_rc < 0 || app->stream_failed) {
            bool capacity_failure = provider_rc < 0 &&
                snj_provider_failure_is_capacity(&provider_failure);
            bool replay_safe = capacity_failure && !app->stream_failed &&
                app->partial_count == 0u && graph.count == 0u &&
                !app->stream_item_seen;
            const char *class_name = app->stream_failed ?
                (app->stream_errno == EPROTO ? "protocol" :
                 app->stream_errno == EOVERFLOW ? "resource" : "output") :
                capacity_failure ? "context" : "provider";
            int exit_status = app->stream_failed &&
                strcmp(class_name, "output") == 0 ? 6 : 4;
            char failure[256];
            json_t *partial;
            (void)snprintf(failure, sizeof(failure), "%s",
                           app->stream_failed ?
                           (app->stream_error[0] ? app->stream_error :
                            "assistant output could not be delivered") :
                           (error[0] ? error : "provider response failed"));
            if (replay_safe && !capacity_recovery_used) {
                bool compacted = false;
                char provider_source_hash[SNJ_SHA256_HEX_LEN + 1u];

                provider_capacity_source_sha256(app->turn_provider,
                                                provider_source_hash);

                if (strcmp(rejected_request_hash, request_hash) == 0) {
                    (void)snprintf(failure, sizeof(failure),
                                   "provider rejected an identical context request twice");
                } else if (commit_event(app, "response_capacity_rejected",
                        snj_app_response_capacity_rejected_data(
                            turn_id, response_id, cycle, request_hash,
                            &provider_failure, &app->turn_capacity,
                            provider_source_hash), error, sizeof(error)) < 0) {
                    snj_app_response_cycle_release(app, &graph, NULL, NULL,
                                                   NULL, NULL);
                    (void)app_error(app, error[0] ? error :
                        "capacity rejection could not be persisted");
                    result = 3;
                    goto out;
                } else {
                    int recovery_rc;
                    bool ceiling_matches;

                    memcpy(rejected_request_hash, request_hash,
                           sizeof(rejected_request_hash));
                    ceiling_matches = capacity_ceiling_matches(
                        app, app->turn_provider, app->turn_model);
                    if (provider_failure.requested_input_known ||
                        ceiling_matches)
                        snj_app_record_model_accounting(app, SNJ_COUNT_UNKNOWN,
                            provider_failure.requested_input_known ?
                                model_input_bytes : 0u,
                            provider_failure.requested_input_known ?
                                provider_failure.requested_input_tokens : 0u,
                            ceiling_matches ?
                                app->session.capacity_ceiling_input_tokens :
                                0u);
                    apply_capacity_ceiling(app, app->turn_provider,
                                           app->turn_model,
                                           &app->turn_capacity);
                    snj_app_response_cycle_release(app, &graph, NULL, NULL,
                                                   NULL, NULL);
                    for (;;) {
                        error[0] = '\0';
                        recovery_rc =
                            snj_app_compact_after_capacity_rejection(
                                app, &credential, &compacted,
                                error, sizeof(error));
                        if (recovery_rc == 1 && app->steering_requested) {
                            app->steering_requested = false;
                            continue;
                        }
                        if (recovery_rc == 2 && app->interrupt_requested) {
                            if (close_active_process_for_turn(app, turn_id,
                                    "user_interrupt", true,
                                    error, sizeof(error)) < 0 ||
                                commit_event(app, "turn_interrupted",
                                    snj_app_turn_interrupted_data(
                                        turn_id, "user", "cancelled"),
                                    error, sizeof(error)) < 0) {
                                (void)app_error(app, error[0] ? error :
                                    "interruption could not be persisted");
                                result = 3;
                            } else {
                                (void)app_warning(app, "turn interrupted");
                                result = app->execute ? 6 : 1;
                            }
                            goto out;
                        }
                        break;
                    }
                    if (recovery_rc == 0 && compacted) {
                        capacity_recovery_used = true;
                        continue;
                    }
                    (void)snprintf(failure, sizeof(failure),
                        "context capacity rejection could not be reduced%s%.*s",
                        error[0] ? ": " : "", 190,
                        error[0] ? error : "");
                    if (commit_event(app, "turn_failed",
                            snj_app_turn_failed_data(turn_id, "context", failure),
                            error, sizeof(error)) < 0) {
                        (void)app_error(app, error);
                        result = 3;
                    } else {
                        (void)app_error(app, failure);
                        result = 4;
                    }
                    goto out;
                }
            }
            partial = snj_app_partial_public_json(app);
            if (!partial) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                (void)app_error(app, "failed response prefix could not be retained");
                result = 3;
                goto out;
            }
            snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
            if (fail_response(app, turn_id, response_id, cycle, class_name,
                              failure, partial, provider_retry_count,
                              app->stream_failed ?
                              (app->stream_errno == EPROTO ?
                               "protocol_failure" : "output_failure") :
                              "provider_failure",
                              error, sizeof(error)) < 0) {
                (void)app_error(app, error);
                result = 3;
                goto out;
            }
            (void)app_error(app, failure);
            result = exit_status;
            goto out;
        }
        if (!app->execute) {
            int input_rc = snj_app_active_input_pump(app, 0u);
            if (input_rc < 0) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                (void)app_error(app, "active input could not be processed");
                result = 3;
                goto out;
            }
        }
        if (snj_response_graph_classify(&graph, &decision,
                                        error, sizeof(error)) < 0) {
            char failure[256];
            json_t *partial;
            (void)snprintf(failure, sizeof(failure), "%s",
                           error[0] ? error : "invalid provider response graph");
            partial = snj_app_partial_public_json(app);
            if (!partial) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                (void)app_error(app, "invalid response prefix could not be retained");
                result = 3;
                goto out;
            }
            snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
            if (fail_response(app, turn_id, response_id, cycle, "protocol",
                              failure, partial, provider_retry_count,
                              "protocol_failure", error, sizeof(error)) < 0) {
                (void)app_error(app, error);
                result = 3;
                goto out;
            }
            (void)app_error(app, failure);
            result = 4;
            goto out;
        }
        if (commit_event(app, "response_completed",
                         snj_app_response_completed_data(turn_id, response_id, cycle, &graph),
                         error, sizeof(error)) < 0) {
            snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
            (void)app_error(app, error);
            result = 3;
            goto out;
        }
        if (graph.usage.input_known && graph.usage.input_tokens != 0u &&
            strcmp(count_method, "exact") != 0)
            snj_app_record_model_accounting(app, SNJ_COUNT_UNKNOWN,
                model_input_bytes, graph.usage.input_tokens, 0u);
        error[0] = '\0';
        if (snj_app_irc_flush_urgent(app, error, sizeof(error)) < 0) {
            snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
            (void)app_error(app, error[0] ? error :
                            "urgent IRC input could not be admitted");
            result = 3;
            goto out;
        }
        {
            char input_tokens[32];
            char output_tokens[32];
            char reasoning_tokens[32];
            char total_tokens[32];
            usage_number(input_tokens, graph.usage.input_known,
                         graph.usage.input_tokens);
            usage_number(output_tokens, graph.usage.output_known,
                         graph.usage.output_tokens);
            usage_number(reasoning_tokens, graph.usage.reasoning_known,
                         graph.usage.reasoning_tokens);
            usage_number(total_tokens, graph.usage.total_known,
                         graph.usage.total_tokens);
            if (app_runtimef(app,
                    "response › %s completed · provider=%s · outcome=%s · items=%zu · duration=%llums · tokens=%s/%s/%s/%s",
                    response_id, graph.provider_response_id,
                    graph_outcome_name(decision.outcome), graph.count,
                    (unsigned long long)(snj_time_ms() - response_begin_ms),
                    input_tokens, output_tokens, reasoning_tokens,
                    total_tokens) < 0) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                (void)app_error(app, "response runtime facts could not be rendered");
                result = 6;
                goto out;
            }
        }
        {
            enum snj_managed_continuation managed =
                snj_app_managed_continuation_classify(app, &graph, &decision);

            if (managed == SNJ_MANAGED_CONTINUATION_HANDLE_MISMATCH) {
                if (terminalize_pending(app, turn_id,
                                        "managed_process_handle_mismatch",
                                        error, sizeof(error)) < 0) {
                    snj_app_response_cycle_release(app, &graph, NULL, NULL,
                                                   NULL, NULL);
                    (void)app_error(app, error);
                    result = 3;
                    goto out;
                }
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL,
                                               NULL);
                continue;
            }
            if (managed == SNJ_MANAGED_CONTINUATION_ORDERING_VIOLATION) {
                static const char message[] =
                    "provider response violated unresolved managed process ordering";
                if ((app->session.pending_call_count != 0u &&
                     terminalize_pending(app, turn_id,
                                         "managed_process_conflict",
                                         error, sizeof(error)) < 0) ||
                    fail_turn(app, turn_id, "protocol_failure",
                              "protocol", message, error, sizeof(error)) < 0) {
                    snj_app_response_cycle_release(app, &graph, NULL, NULL,
                                                   NULL, NULL);
                    (void)app_error(app, error);
                    result = 3;
                    goto out;
                }
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL,
                                               NULL);
                (void)app_error(app, message);
                result = 4;
                goto out;
            }
        }
        if (decision.outcome == SNJ_GRAPH_CONFLICT) {
            const char *message = decision.message ? decision.message :
                "provider response contained conflicting actions";
            if (terminalize_pending(app, turn_id, "protocol_conflict",
                                    error, sizeof(error)) < 0 ||
                fail_turn(app, turn_id, "protocol_failure",
                          "protocol", message, error, sizeof(error)) < 0) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                (void)app_error(app, error);
                result = 3;
                goto out;
            }
            snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
            (void)app_error(app, message);
            result = 4;
            goto out;
        }
        if (app->session.pending_steering_count != 0u) {
            if (decision.outcome == SNJ_GRAPH_CALLS &&
                terminalize_pending(app, turn_id, "superseded_by_steering",
                                    error, sizeof(error)) < 0) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                (void)app_error(app, error);
                result = 3;
                goto out;
            }
            snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
            continue;
        }
        if (app->networked && app->irc_turn_local_operator &&
            !app->irc_turn_replied && !app->session.irc_reply_reminded &&
            (decision.outcome == SNJ_GRAPH_NONPRODUCTIVE ||
             decision.outcome == SNJ_GRAPH_FINAL ||
             decision.outcome == SNJ_GRAPH_REFUSAL)) {
            char steering_id[SNJ_ID_HEX_LEN + 1u];

            if (snj_random_id(steering_id) < 0 ||
                commit_event(app, "irc_reply_reminder",
                    snj_app_steering_added_data(turn_id, steering_id,
                        SNJ_IRC_REPLY_REMINDER_TEXT),
                    error, sizeof(error)) < 0) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL,
                                               NULL, NULL);
                (void)app_error(app, error[0] ? error :
                                "IRC reply reminder could not be persisted");
                result = 3;
                goto out;
            }
            snj_app_response_cycle_release(app, &graph, NULL, NULL,
                                           NULL, NULL);
            continue;
        }
        if (app->networked && decision.outcome == SNJ_GRAPH_NONPRODUCTIVE) {
            if (commit_event(app, "turn_completed_silent",
                    silent_turn_data(turn_id, response_id,
                        app->irc_turn_local_operator && !app->irc_turn_replied ?
                        "reply_reminder_exhausted" : "room_update_quiet"),
                    error, sizeof(error)) < 0) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL,
                                               NULL, NULL);
                (void)app_error(app, error[0] ? error :
                                "quiet IRC turn could not be completed");
                result = 3;
                goto out;
            }
            snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
            result = 0;
            goto out;
        }
        if (decision.outcome == SNJ_GRAPH_FINAL ||
            decision.outcome == SNJ_GRAPH_REFUSAL) {
            const struct snj_response_item *final = &graph.items[decision.final_index];
            if (commit_event(app, "turn_completed",
                             snj_app_turn_completed_data(turn_id, response_id,
                                                 final->local_item_id),
                             error, sizeof(error)) < 0) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                (void)app_error(app, error);
                result = 3;
                goto out;
            }
            app->last_turn_refused = decision.outcome == SNJ_GRAPH_REFUSAL;
            if (app_runtimef(app,
                    "turn › %s completed · response=%s · item=%s",
                    turn_id, response_id, final->local_item_id) < 0) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                (void)app_error(app, "turn runtime facts could not be rendered");
                result = 6;
                goto out;
            }
            if (app->execute &&
                snj_write_full(STDOUT_FILENO, final->text,
                               strlen(final->text)) < 0) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                (void)app_error(app, "final answer could not be written to stdout");
                result = 6;
                goto out;
            }
            if (snj_app_compact_after_turn(app, input_tokens_bound, count_method,
                                           error, sizeof(error)) < 0)
                (void)app_warning(app, error);
            snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
            result = 0;
            goto out;
        }
        if (decision.outcome == SNJ_GRAPH_CALLS) {
            int tool_rc;
            tool_rc = execute_calls(app, turn_id, &graph, &credential,
                                    error, sizeof(error));
            snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
            if (tool_rc < 0) {
                (void)app_error(app, error);
                result = 3;
                goto out;
            }
            if (tool_rc > 0) {
                if (tool_rc == 2) {
                    (void)app_warning(app, "turn interrupted");
                    result = app->execute ? 6 : 1;
                } else {
                    (void)app_error(app, error);
                    result = 4;
                }
                goto out;
            }
            continue;
        }
        {
            const char *message = decision.message ? decision.message :
                "provider response was not actionable";
            if (fail_turn(app, turn_id, "protocol_failure",
                          "protocol", message, error, sizeof(error)) < 0) {
                snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
                (void)app_error(app, error);
                result = 3;
                goto out;
            }
            snj_app_response_cycle_release(app, &graph, NULL, NULL, NULL, NULL);
            (void)app_error(app, message);
            result = 4;
            goto out;
        }
    }
    {
        static const char message[] = "response-cycle counter exhausted";
        if (fail_turn(app, turn_id, "internal_failure",
                      "resource", message, error, sizeof(error)) < 0) {
            (void)app_error(app, error);
            result = 3;
            goto out;
        }
        (void)app_error(app, message);
        result = 4;
    }
out:
    app->stream_graph = NULL;
    snj_app_clear_partial_public(app);
    if (!app->execute && result != 6 &&
        set_input_prompt(app, false) < 0)
        result = 6;
    snj_credential_clear(&credential);
    snj_instructions_free(&app->turn_instructions);
    free(turn_prompt);
    return result;
}

static int
run_tracked_turn(struct app_state *app, const char *prompt,
                 const struct snj_queued_turn *queued, bool goal_turn)
{
    char error[256] = {0};
    const char *reason = NULL;
    const char *message = NULL;
    int rc = run_turn(app, prompt, queued, goal_turn);

    if (app->session.goal_status != SNJ_GOAL_ACTIVE)
        return rc;
    if (app->input_closed) {
        reason = "input_closed";
    } else if (app->last_turn_refused) {
        reason = "refusal";
        message = "goal paused after model refusal";
    } else if (rc != 0) {
        reason = "turn_stopped";
        message = "goal paused after the turn stopped";
    }
    if (!reason)
        return rc;
    if (snj_app_goal_pause(app, reason, error, sizeof(error)) < 0) {
        (void)app_error(app, error[0] ? error : "goal pause could not be saved");
        return 3;
    }
    if (message && app_warning(app, message) < 0)
        return 6;
    return rc;
}

static char *
resolve_workspace_path(const char *path, const char *label,
                       char *error, size_t error_size)
{
    char *resolved = realpath(path, NULL);
    struct stat st;
    if (!resolved) {
        snj_errorf(error, error_size, "cannot resolve %s workspace %s: %s",
                  label, path, strerror(errno));
        return NULL;
    }
    if (strlen(resolved) > SNJ_PATH_MAX_BYTES ||
        !snj_utf8_valid((const unsigned char *)resolved, strlen(resolved), true) ||
        stat(resolved, &st) < 0 || !S_ISDIR(st.st_mode)) {
        snj_errorf(error, error_size, "%s workspace must be an existing UTF-8 directory",
                  label);
        free(resolved);
        errno = EINVAL;
        return NULL;
    }
    return resolved;
}
static char *
current_workspace(char *error, size_t error_size)
{
    return resolve_workspace_path(".", "current", error, error_size);
}
static char *
resolve_dotdir(const char *override, char *error, size_t error_size)
{
    const char *home = getenv("HOME");
    char *path;
    size_t len;

    if (override)
        path = snj_strdup_checked(override, SNJ_PATH_MAX_BYTES);
    else if (home && home[0] == '/') {
        size_t home_len = strlen(home);
        bool slash = home_len != 0u && home[home_len - 1u] == '/';
        const char *suffix = slash ? "." SNAJPAGENT_NAME :
                                     "/." SNAJPAGENT_NAME;
        size_t suffix_len = strlen(suffix);
        if (home_len > SNJ_PATH_MAX_BYTES - suffix_len) {
            errno = ENAMETOOLONG;
            path = NULL;
        } else {
            path = malloc(home_len + suffix_len + 1u);
            if (path) {
                memcpy(path, home, home_len);
                memcpy(path + home_len, suffix, suffix_len + 1u);
            }
        }
    }
    else {
        snj_errorf(error, error_size,
                  "HOME is unavailable for the default dotdir; use --dotdir DIR");
        errno = EINVAL;
        return NULL;
    }
    if (!path)
        return NULL;
    len = strlen(path);
    while (len > 1u && path[len - 1u] == '/')
        path[--len] = '\0';
    if (path[0] != '/' ||
        !snj_utf8_valid((const unsigned char *)path, len, true)) {
        snj_errorf(error, error_size,
                  "dotdir must be an absolute UTF-8 path within the supported limit");
        free(path);
        errno = EINVAL;
        return NULL;
    }
    return path;
}

static char *
resolved_program_path(const char *program)
{
    const char *path;
    size_t program_len;

    if (!program || !*program)
        program = SNAJPAGENT_NAME;
    program_len = strlen(program);
    if (strchr(program, '/')) {
        char *resolved = realpath(program, NULL);

        if (resolved && strlen(resolved) <= SNJ_PATH_MAX_BYTES &&
            snj_utf8_valid((const unsigned char *)resolved,
                           strlen(resolved), true))
            return resolved;
        free(resolved);
        return snj_strdup_checked(program, SNJ_PATH_MAX_BYTES);
    }
    path = getenv("PATH");
    if (path) {
        const char *start = path;

        for (;;) {
            const char *end = strchr(start, ':');
            size_t dir_len = end ? (size_t)(end - start) : strlen(start);
            const char *dir = dir_len ? start : ".";
            size_t actual_dir_len = dir_len ? dir_len : 1u;

            if (actual_dir_len <= SNJ_PATH_MAX_BYTES &&
                program_len <= SNJ_PATH_MAX_BYTES - actual_dir_len - 1u) {
                size_t size = actual_dir_len + 1u + program_len + 1u;
                char *candidate = malloc(size);

                if (candidate) {
                    char *resolved;

                    memcpy(candidate, dir, actual_dir_len);
                    candidate[actual_dir_len] = '/';
                    memcpy(candidate + actual_dir_len + 1u, program,
                           program_len + 1u);
                    resolved = access(candidate, X_OK) == 0 ?
                               realpath(candidate, NULL) : NULL;
                    free(candidate);
                    if (resolved && strlen(resolved) <= SNJ_PATH_MAX_BYTES &&
                        snj_utf8_valid((const unsigned char *)resolved,
                                       strlen(resolved), true))
                        return resolved;
                    free(resolved);
                }
            }
            if (!end)
                break;
            start = end + 1u;
        }
    }
    return snj_strdup_checked(program, SNJ_PATH_MAX_BYTES);
}

static int
append_command_literal(struct snj_buf *command, const char *word)
{
    if (command->len && snj_buf_putc(command, ' ') < 0)
        return -1;
    return snj_buf_append(command, word, strlen(word));
}

static int
append_command_argument(struct snj_buf *command, const char *argument)
{
    const unsigned char *p = (const unsigned char *)argument;

    if (command->len && snj_buf_putc(command, ' ') < 0)
        return -1;
    if (snj_buf_putc(command, '\'') < 0)
        return -1;
    while (*p) {
        if (*p == '\'') {
            if (snj_buf_append(command, "'\\''", 4u) < 0)
                return -1;
        } else if (*p == '\n' || (*p >= 0x20u && *p <= 0x7eu)) {
            if (snj_buf_putc(command, *p) < 0)
                return -1;
        } else {
            if (snj_buf_putc(command, '\'') < 0 ||
                snj_buf_printf(command, "\"$(printf '\\%03o')\"",
                               (unsigned int)*p) < 0 ||
                snj_buf_putc(command, '\'') < 0)
                return -1;
        }
        ++p;
    }
    return snj_buf_putc(command, '\'');
}

static int
append_command_option(struct snj_buf *command, const char *option,
                      const char *argument)
{
    return append_command_literal(command, option) < 0 ||
           append_command_argument(command, argument) < 0 ? -1 : 0;
}

static int
build_resume_command(const struct app_state *app, const char *program,
                     const char *dotdir, struct snj_buf *command)
{
    char *resolved = resolved_program_path(program);
    const struct snj_cli *cli = app->cli;
    const struct snj_config *config = app->config;
    int rc = -1;

    if (!resolved)
        return -1;
    if (append_command_argument(command, resolved) < 0 ||
        append_command_option(command, "--dotdir", dotdir) < 0)
        goto out;
    if (cli->config_path &&
        append_command_option(command, "--config", cli->config_path) < 0)
        goto out;
    switch (cli->color) {
    case SNJ_CLI_COLOR_UNSET: break;
    case SNJ_CLI_COLOR_AUTO:
        if (append_command_literal(command, "--color=auto") < 0)
            goto out;
        break;
    case SNJ_CLI_COLOR_ALWAYS:
        if (append_command_literal(command, "--color=always") < 0)
            goto out;
        break;
    case SNJ_CLI_COLOR_NEVER:
        if (append_command_literal(command, "--color=never") < 0)
            goto out;
        break;
    }
    if (cli->markdown == SNJ_CLI_MARKDOWN_ENABLED &&
        append_command_literal(command, "--markdown") < 0)
        goto out;
    if (cli->markdown == SNJ_CLI_MARKDOWN_DISABLED &&
        append_command_literal(command, "--no-markdown") < 0)
        goto out;
    for (unsigned int i = 0u; i < cli->verbosity; ++i)
        if (append_command_literal(command, "-v") < 0)
            goto out;
    if (app->staged_model &&
        append_command_option(command, "-m", app->staged_model) < 0)
        goto out;
    if (app->staged_effort &&
        append_command_option(command, "--effort", app->staged_effort) < 0)
        goto out;
    if (app->networked) {
        if (config->irc_listen_explicit &&
            append_command_option(command, "--listen",
                                  config->irc_listen) < 0)
            goto out;
        for (size_t i = 0u; i < config->irc_client_count; ++i)
            if (append_command_option(command, "--client",
                                      config->irc_clients[i]) < 0)
                goto out;
        if (append_command_option(command, "--model-nick",
                                  config->irc_model_nick) < 0 ||
            append_command_option(command, "--operator-nick",
                                  config->irc_operator_nick) < 0)
            goto out;
        if (config->irc_listen_explicit && config->irc_room_name[0] &&
            append_command_option(command, "--room-name",
                                  config->irc_room_name) < 0)
            goto out;
    }
    if (append_command_literal(command, "--resume") < 0 ||
        append_command_argument(command, app->session.id) < 0)
        goto out;
    rc = 0;
out:
    free(resolved);
    return rc;
}

static void
write_resume_command(const struct app_state *app, const char *program,
                     const char *dotdir)
{
    struct snj_buf command;

    if (!dotdir || !app->session.id[0] || app->session.delete_requested)
        return;
    snj_buf_init(&command, RESUME_COMMAND_MAX);
    if (build_resume_command(app, program, dotdir, &command) == 0)
        (void)snj_render_resume_hint(&app->render, (char *)command.data,
                                     command.len);
    snj_buf_free(&command);
}

static int
pick_session(struct app_state *app, const char *workspace,
             char *error, size_t error_size)
{
    char prefix[128];
    size_t len;
    if (snj_store_list_active(&app->store, workspace, app->cli->all, STDERR_FILENO,
                       error, error_size) < 0)
        return -1;
    if (snj_render_prompt(&app->render, "session › ") < 0 ||
        !fgets(prefix, sizeof(prefix), stdin)) {
        snj_errorf(error, error_size, "session selection cancelled");
        return -1;
    }
    len = strlen(prefix);
    if (len && prefix[len - 1u] == '\n')
        prefix[--len] = '\0';
    if (len < 8u || len > SNJ_ID_HEX_LEN) {
        snj_errorf(error, error_size, "enter an 8..32 character session id prefix");
        return -1;
    }
    return snj_session_open(&app->store, &app->session, prefix,
                            error, error_size);
}
static int
run_queued_chain(struct app_state *app)
{
    while (app->queue_armed && app->session.pending_queue_count != 0u) {
        const struct snj_queued_turn *queued = &app->session.pending_queue[0];
        int turn_rc = run_tracked_turn(app, queued->text, queued, false);
        if (turn_rc != 0) {
            app->queue_armed = false;
            return turn_rc;
        }
    }
    if (app->session.pending_queue_count == 0u)
        app->queue_armed = false;
    return 0;
}

static int
run_ready_chains(struct app_state *app)
{
    for (;;) {
        int turn_rc;

        if (app->input_closed) {
            app->queue_armed = false;
            app->goal_armed = false;
            return 0;
        }
        if (app->networked &&
            (app->irc_urgent.len ||
             ((!app->queue_armed || app->session.pending_queue_count == 0u) &&
              app->goal_armed && app->irc_background.len))) {
            bool local_operator = false;
            char *prompt = snj_app_irc_take_pending(app, &local_operator,
                                                     true);
            if (prompt) {
                app->irc_turn_local_operator = local_operator;
                turn_rc = run_tracked_turn(app, prompt, NULL, false);
                app->irc_turn_local_operator = false;
                free(prompt);
                if (turn_rc != 0)
                    return turn_rc;
                continue;
            }
        }
        if (app->queue_armed && app->session.pending_queue_count != 0u) {
            turn_rc = run_queued_chain(app);
            if (turn_rc != 0)
                return turn_rc;
            continue;
        }
        if (app->goal_armed &&
            app->session.goal_status == SNJ_GOAL_ACTIVE) {
            turn_rc = run_tracked_turn(app, SNJ_GOAL_CONTINUATION_TEXT,
                                      NULL, true);
            if (turn_rc != 0)
                return turn_rc;
            continue;
        }
        return 0;
    }
}

static int
interactive_loop(struct app_state *app, const char *initial)
{
    char *owned = NULL;
    const char *prompt = initial;
    if (set_input_prompt(app, false) < 0)
        return 6;
    for (;;) {
        enum snj_term_action action = SNJ_TERM_NONE;
        if (capture_shutdown_signal(app))
            return 0;
        if (app->input_closed)
            return 0;
        if (!prompt && app->networked) {
            bool local_operator = false;
            char *irc_prompt = snj_app_irc_take_pending(
                app, &local_operator, false);

            if (irc_prompt) {
                int turn_rc;
                app->queue_armed = false;
                app->irc_turn_local_operator = local_operator;
                turn_rc = run_tracked_turn(app, irc_prompt, NULL, false);
                app->irc_turn_local_operator = false;
                free(irc_prompt);
                if (turn_rc == 3 || turn_rc == 6)
                    return turn_rc;
                if (turn_rc == 0 &&
                    (app->queue_armed || app->goal_armed)) {
                    int chain_rc = run_ready_chains(app);
                    if (chain_rc == 3 || chain_rc == 6)
                        return chain_rc;
                }
                continue;
            }
        }
        if (!prompt) {
            int poll_rc = snj_term_poll(&app->term,
                                        app->networked ? 25 : -1,
                                        &action, &owned);
            history_warning(app);
            if (poll_rc < 0) {
                if (capture_shutdown_signal(app)) {
                    free(owned);
                    return 0;
                }
                (void)app_error(app,
                    errno == EOVERFLOW ? "prompt exceeds 1 MiB" :
                    errno == EILSEQ ? "terminal input contains invalid UTF-8" :
                    "terminal input could not be read");
                if (set_input_prompt(app, false) < 0)
                    return 6;
                continue;
            }
            if (app->networked) {
                char irc_error[256] = {0};
                if (tick_irc(app, irc_error, sizeof(irc_error)) < 0) {
                    (void)app_error(app, irc_error[0] ? irc_error :
                                    "IRC event loop failed");
                    return 3;
                }
            }
            if (poll_rc == 0)
                continue;
            if (action == SNJ_TERM_EXIT) {
                free(owned);
                return 0;
            }
            if (action == SNJ_TERM_CANCEL) {
                if (app->queue_edit_id[0]) {
                    app->queue_armed = app->queue_edit_was_armed;
                    app->queue_edit_id[0] = '\0';
                    app->queue_edit_number = 0u;
                    app->queue_edit_was_armed = false;
                    if (set_input_prompt(app, false) < 0)
                        return 6;
                }
                continue;
            }
            if (action == SNJ_TERM_VIEW) {
                if (app->queue_edit_id[0])
                    (void)app_error(app, "queue replacement must be nonempty");
                else if (toggle_view(app) < 0)
                    return 6;
                continue;
            }
            if (action != SNJ_TERM_SUBMIT || !owned) {
                free(owned);
                owned = NULL;
                if (set_input_prompt(app, false) < 0)
                    return 6;
                continue;
            }
            prompt = owned;
        }
        if (owned)
            remember_input(app, prompt);
        if (app->queue_edit_id[0]) {
            char edit_error[256] = {0};
            int edit_rc = finish_queue_edit(app, prompt, false,
                                            edit_error, sizeof(edit_error));

            if (edit_rc != 0 && edit_error[0])
                (void)app_error(app, edit_error);
            free(owned);
            owned = NULL;
            prompt = NULL;
            if (edit_rc < 0)
                return 3;
            continue;
        }
        if (!app->networked) {
            if (snj_render_input_submitted(&app->render,
                    snj_term_prompt_label(&app->term), prompt) < 0) {
                free(owned);
                return 6;
            }
        }
        {
            bool single_line = strchr(prompt, '\n') == NULL;
            bool handled = false;
            int local_rc = 0;
            if (single_line && prompt[0] == '/' && prompt[1] != '/')
                local_rc = handle_common_command(app, prompt, false, &handled);
            if (local_rc < 0) { free(owned); return 3; }
            if (!handled && single_line) {
                char queue_error[256] = {0};

                local_rc = handle_queue_command(app, prompt, false, &handled,
                                                 queue_error,
                                                 sizeof(queue_error));
                if (local_rc != 0 && queue_error[0])
                    (void)app_error(app, queue_error);
                if (local_rc < 0) { free(owned); return 3; }
            }
            if (!handled && single_line && prompt[0] == '/' && prompt[1] != '/') {
                bool exit_now = false;
                local_rc = snj_app_lifecycle_command(app, prompt, &handled, &exit_now);
                if (local_rc < 0 || exit_now) { free(owned); return local_rc < 0 ? 3 : 0; }
            }
            if (handled) {
                int chain_rc = run_ready_chains(app);
                if (chain_rc == 3 || chain_rc == 6) {
                    free(owned);
                    return chain_rc;
                }
            } else if (single_line && strcmp(prompt, "/exit") == 0) {
                free(owned);
                return 0;
            } else if (single_line && strcmp(prompt, "/next") == 0) {
                int chain_rc;
                if (app->session.pending_queue_count == 0u) {
                    (void)app_error(app, "future-turn queue is empty");
                } else {
                    app->queue_armed = true;
                    chain_rc = run_ready_chains(app);
                    if (chain_rc == 3 || chain_rc == 6) {
                        free(owned);
                        return chain_rc;
                    }
                }
            } else if (single_line && prompt[0] == '/' && prompt[1] != '/') {
                (void)app_error(app, "unknown slash command");
            } else if (app->networked) {
                const char *actual = prompt[0] == '/' && prompt[1] == '/' ?
                                     prompt + 1 : prompt;
                char irc_error[256] = {0};

                if (snj_irc_send_operator(app->irc, actual, irc_error,
                                          sizeof(irc_error)) < 0)
                    (void)app_error(app, irc_error[0] ? irc_error :
                                    "IRC message could not be queued");
            } else {
                const char *actual = prompt[0] == '/' && prompt[1] == '/' ?
                                     prompt + 1 : prompt;
                int turn_rc;
                app->queue_armed = false;
                turn_rc = run_tracked_turn(app, actual, NULL, false);
                if (turn_rc == 3 || turn_rc == 6) {
                    free(owned);
                    return turn_rc;
                }
                if (turn_rc == 0 &&
                    (app->queue_armed || app->goal_armed)) {
                    int chain_rc = run_ready_chains(app);
                    if (chain_rc == 3 || chain_rc == 6) {
                        free(owned);
                        return chain_rc;
                    }
                } else if (turn_rc != 0) {
                    app->queue_armed = false;
                }
            }
        }
        free(owned);
        owned = NULL;
        prompt = NULL;
        if (set_input_prompt(app, false) < 0)
            return 6;
    }
}
int
snj_app_run(const struct snj_cli *cli, const char *program)
{
    struct app_state app;
    struct app_signal_handlers signal_handlers;
    struct snj_config config;
    char error[256];
    char *dotdir = NULL;
    char *config_path = NULL;
    char *workspace = NULL;
    char *relocated_workspace = NULL;
    const char *new_model = NULL;
    const char *new_effort;
    unsigned int effective_verbosity;
    bool goal_paused_on_resume = false;
    bool signal_handlers_installed = false;
    int rc = 3;
    memset(&app, 0, sizeof(app));
    snj_buf_init(&app.irc_urgent, SNJ_MAX_STEERING_TEXT + 1u);
    snj_buf_init(&app.irc_background, SNJ_MAX_STEERING_TEXT + 1u);
    snj_config_init(&config);
    snj_instructions_init(&app.turn_instructions);
    snj_model_cache_init(&app.model_cache);
    snj_store_init(&app.store);
    snj_session_init(&app.session);
    snj_term_init(&app.term);
    snj_render_init(&app.render, 0u);
    snj_render_set_color(&app.render,
        cli->color == SNJ_CLI_COLOR_NEVER ? SNJ_COLOR_NEVER :
        cli->color == SNJ_CLI_COLOR_ALWAYS ? SNJ_COLOR_ALWAYS :
                                              SNJ_COLOR_AUTO);
    app.cli = cli;
    app.config = &config;
    app.config_allow_create = cli->config_path == NULL;
    app.execute = cli->execute;
    if (install_shutdown_handlers(&signal_handlers) < 0) {
        (void)snj_render_error_ctx(&app.render,
                                   "cannot install shutdown signal handlers");
        rc = 2;
        goto out;
    }
    signal_handlers_installed = true;
    {
        const char *locale_name = setlocale(LC_CTYPE, "");
        const char *codeset = locale_name ? nl_langinfo(CODESET) : NULL;
        if (!locale_name || !codeset ||
            (strcasecmp(codeset, "UTF-8") != 0 &&
             strcasecmp(codeset, "UTF8") != 0)) {
            (void)snj_render_error_ctx(&app.render,
                                       "a UTF-8 locale is required");
            rc = 2;
            goto out;
        }
    }
    error[0] = '\0';
    dotdir = resolve_dotdir(cli->dotdir, error, sizeof(error));
    if (!dotdir) {
        (void)snj_render_error_ctx(&app.render,
                                   error[0] ? error : "dotdir is unavailable");
        rc = 2;
        goto out;
    }
    if (snj_config_load(&config, cli->config_path, dotdir,
                        error, sizeof(error)) < 0) {
        (void)snj_render_error_ctx(&app.render, error);
        rc = 2;
        goto out;
    }
    config_path = snj_config_path(cli->config_path, dotdir,
                                  error, sizeof(error));
    if (!config_path) {
        (void)snj_render_error_ctx(&app.render, error);
        rc = 2;
        goto out;
    }
    app.config_path = config_path;
    {
        enum snj_color_mode color = config.color;
        if (cli->color == SNJ_CLI_COLOR_AUTO)
            color = SNJ_COLOR_AUTO;
        else if (cli->color == SNJ_CLI_COLOR_ALWAYS)
            color = SNJ_COLOR_ALWAYS;
        else if (cli->color == SNJ_CLI_COLOR_NEVER)
            color = SNJ_COLOR_NEVER;
        snj_render_set_color(&app.render, color);
    }
    snj_render_set_markdown(&app.render,
        cli->markdown == SNJ_CLI_MARKDOWN_ENABLED ? true :
        cli->markdown == SNJ_CLI_MARKDOWN_DISABLED ? false : config.markdown);
    if (!cli->execute && !cli->list &&
        snj_irc_apply_cli(&config, cli, error, sizeof(error)) < 0) {
        (void)snj_render_error_ctx(&app.render, error);
        rc = 2;
        goto out;
    }
    app.networked = !cli->execute && !cli->list && snj_irc_enabled(&config);
    snj_term_set_commands(&app.term, commands, command_count(&app));
    snj_render_set_networked(&app.render, app.networked,
                             app.networked ? config.irc_model_nick : NULL);
    snj_term_set_typing_pause(&app.term, config.typing_pause_ms);
    effective_verbosity = configured_verbosity(&app, &config);
    app.render.verbosity = effective_verbosity;
    if (!cli->execute && !cli->list &&
        (isatty(STDIN_FILENO) != 1 || isatty(STDERR_FILENO) != 1)) {
        (void)snj_render_error_ctx(&app.render,
            "interactive mode requires terminal stdin and stderr; use -e for scripts");
        rc = 2;
        goto out;
    }
    if (!cli->resume || cli->model)
        new_model = effective_model(cli->model ? cli->model : config.model);
    new_effort = cli->effort ? cli->effort : config.reasoning_effort;
    if ((!cli->resume || cli->effort) && !resolve_effort(new_effort)) {
        (void)snj_render_error_ctx(&app.render,
            "reasoning effort is empty, oversized, or invalid UTF-8");
        rc = 2;
        goto out;
    }
    if (snj_store_open(&app.store, dotdir, error, sizeof(error)) < 0) {
        (void)snj_render_error_ctx(&app.render, error);
        goto out;
    }
    workspace = current_workspace(error, sizeof(error));
    if (!workspace) {
        (void)snj_render_error_ctx(&app.render, error);
        goto out;
    }
    if (cli->list) {
        rc = snj_store_list(&app.store, workspace, cli->all, STDOUT_FILENO,
                            error, sizeof(error)) < 0 ? 3 : 0;
        if (rc)
            (void)snj_render_error_ctx(&app.render, error);
        goto out;
    }
    if (cli->resume) {
        if (cli->workspace) {
            relocated_workspace = resolve_workspace_path(cli->workspace, "relocation",
                                                         error, sizeof(error));
            if (!relocated_workspace) {
                (void)snj_render_error_ctx(&app.render, error);
                rc = 2;
                goto out;
            }
        }
        if (cli->resume_id)
            rc = snj_session_open(&app.store, &app.session, cli->resume_id,
                                  error, sizeof(error));
        else if (cli->last)
            rc = snj_session_open_last(&app.store, &app.session, workspace,
                                       cli->all, error, sizeof(error));
        else
            rc = pick_session(&app, workspace, error, sizeof(error));
        if (rc == 1) {
            (void)snj_render_warning_ctx(&app.render, error);
            rc = 0;
            goto out;
        }
        if (rc < 0) {
            (void)snj_render_error_ctx(&app.render, error);
            rc = 3;
            goto out;
        }
        if (!cli->execute && validate_prompt_values(&config,
                snj_config_provider(&config, app.session.default_provider),
                cli->model ? new_model : app.session.default_model,
                resolve_effort(cli->effort ? cli->effort :
                               app.session.default_effort),
                app.networked) < 0) {
            (void)snj_render_error_ctx(&app.render,
                "configured prompt cannot be rendered with the current selection");
            rc = 2;
            goto out;
        }
        if (app.session.archived && snj_session_unarchive(&app.session, NULL, error, sizeof(error)) < 0) {
            (void)snj_render_error_ctx(&app.render, error); rc = 3; goto out;
        }
        if (recover_session(&app, error, sizeof(error)) < 0) {
            (void)snj_render_error_ctx(&app.render, error);
            rc = 3;
            goto out;
        }
        if (app.session.goal_status == SNJ_GOAL_ACTIVE) {
            if (snj_app_goal_pause(&app, "session_resumed",
                                   error, sizeof(error)) < 0) {
                (void)snj_render_error_ctx(&app.render, error);
                rc = 3;
                goto out;
            }
            goal_paused_on_resume = true;
        }
        if (relocated_workspace &&
            strcmp(relocated_workspace, app.session.workspace) != 0 &&
            commit_event(&app, "workspace_changed",
                         snj_app_preference_changed_data("old_workspace",
                                                app.session.workspace,
                                                "new_workspace",
                                                relocated_workspace),
                         error, sizeof(error)) < 0) {
            (void)snj_render_error_ctx(&app.render, error);
            rc = 3;
            goto out;
        }
        app.staged_model = cli->model ? new_model : NULL;
        app.staged_effort = cli->effort;
        app.turn_model = next_model(&app);
        app.turn_effort = resolve_effort(next_effort(&app));
        app.turn_provider = next_provider(&app);
    } else {
        const char *selected_workspace = cli->workspace ? cli->workspace : workspace;
        const struct snj_provider_config *selected_provider =
            snj_config_provider(&config,
                                config.provider[0] ? config.provider : NULL);
        if (!cli->execute && validate_prompt_values(
                &config, selected_provider, new_model,
                resolve_effort(new_effort), app.networked) < 0) {
            (void)snj_render_error_ctx(&app.render,
                "configured prompt cannot be rendered with the current selection");
            rc = 2;
            goto out;
        }
        if (snj_session_create(&app.store, &app.session, selected_workspace,
                               selected_provider->name, new_model, new_effort,
                               error, sizeof(error)) < 0) {
            (void)snj_render_error_ctx(&app.render, error);
            goto out;
        }
        app.turn_model = app.session.default_model;
        app.turn_effort = resolve_effort(app.session.default_effort);
        app.turn_provider = selected_provider;
    }
    if (app.networked) {
        if (snj_irc_open(&app.irc, &config, app.session.workspace,
                         snj_app_irc_event, snj_app_irc_trace, &app,
                         error, sizeof(error)) < 0 ||
            snj_app_irc_restore(&app, error, sizeof(error)) < 0 ||
            (config.irc_listen_explicit &&
             snj_app_irc_snapshot(&app, "join", error, sizeof(error)) < 0)) {
            (void)snj_render_error_ctx(&app.render, error[0] ? error :
                                   "IRC startup failed");
            rc = 3;
            goto out;
        }
    }
    if (cli->execute) {
        rc = run_tracked_turn(&app, cli->prompt, NULL, false);
        if (rc == 0 && (app.queue_armed || app.goal_armed))
            rc = run_ready_chains(&app);
        goto out;
    }
    if (snj_term_open(&app.term, error, sizeof(error)) < 0) {
        (void)snj_render_error_ctx(&app.render, error);
        rc = 3;
        goto out;
    }
    snj_render_attach_term(&app.render, &app.term);
    (void)snj_term_history_open(&app.term, dotdir);
    history_warning(&app);
    if (snj_render_orientation(&app.render, &app.session, cli->resume) < 0 ||
        (cli->resume && config.resume_history_turns != 0u &&
         (!app.networked || app.render.verbosity >= 1u) &&
         snj_render_history(&app.render, &app.session) < 0) ||
        (cli->resume && app.session.pending_queue_count != 0u &&
         app_warning(&app,
             "queued future turns are paused; use /next to continue FIFO") < 0) ||
        (goal_paused_on_resume &&
         app_warning(&app,
             "active goal was paused on resume; use /goal resume to continue") < 0)) {
        rc = 6;
        goto out;
    }
    rc = interactive_loop(&app, cli->prompt);
out:
    (void)capture_shutdown_signal(&app);
    snj_render_free(&app.render);
    snj_term_close(&app.term);
    snj_irc_close(app.irc);
    write_resume_command(&app, program, dotdir);
    (void)capture_shutdown_signal(&app);
    if (signal_handlers_installed)
        restore_shutdown_handlers(&signal_handlers);
    snj_buf_free(&app.irc_urgent);
    snj_buf_free(&app.irc_background);
    free(config_path);
    free(dotdir);
    free(relocated_workspace);
    free(workspace);
    snj_instructions_free(&app.turn_instructions);
    snj_model_cache_free(&app.model_cache);
    snj_session_close(&app.session);
    snj_store_close(&app.store);
    snj_config_free(&config);
    if (app.shutdown_signal > 0 && app.shutdown_signal < 128)
        rc = 128 + app.shutdown_signal;
    return rc;
}
