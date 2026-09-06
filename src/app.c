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
#include <stdatomic.h>
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

static atomic_int pending_shutdown_signal;
static _Atomic(struct snag_ui *) shutdown_ui;

static void
mark_shutdown_signal(int signal_number)
{
    int expected = 0;
    int saved = errno;
    (void)atomic_compare_exchange_strong(&pending_shutdown_signal,
                                         &expected, signal_number);
    snag_ui_signal(atomic_load(&shutdown_ui));
    errno = saved;
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
    int signal_number = atomic_load(&pending_shutdown_signal);

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
    return snag_ui_text(&app->ui, SNAG_UI_ERROR, message);
}
static int
app_warning(struct app_state *app, const char *message)
{
    return snag_ui_text(&app->ui, SNAG_UI_WARNING, message);
}
static void
history_warning(struct app_state *app)
{
    if (snag_ui_history_warning(&app->ui))
        (void)app_warning(app, "prompt history is unavailable or contained damaged records");
}
static void
remember_input(struct app_state *app, const char *text)
{
    (void)snag_ui_history_add(&app->ui, text);
    history_warning(app);
}
static int
app_hostf(struct app_state *app, const char *fmt, ...)
{
    struct snag_buf text;
    va_list ap;
    int rc;

    snag_buf_init(&text, 4u * 1024u * 1024u);
    va_start(ap, fmt);
    rc = snag_buf_vprintf(&text, fmt, ap);
    va_end(ap);
    if (rc == 0)
        rc = snag_ui_text(&app->ui, SNAG_UI_HOST, (const char *)text.data);
    snag_buf_free(&text);
    return rc;
}
static int
app_runtimef(struct app_state *app, const char *fmt, ...)
{
    struct snag_buf text;
    va_list ap;
    int rc;

    if (!snag_ui_enabled(&app->ui, SNAG_PRESENT_DEBUG))
        return 0;
    snag_buf_init(&text, 4u * 1024u * 1024u);
    va_start(ap, fmt);
    rc = snag_buf_vprintf(&text, fmt, ap);
    va_end(ap);
    if (rc == 0)
        rc = snag_ui_text(&app->ui, SNAG_UI_RUNTIME, (const char *)text.data);
    snag_buf_free(&text);
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
graph_outcome_name(enum snag_graph_outcome outcome)
{
    switch (outcome) {
    case SNAG_GRAPH_CALLS: return "calls";
    case SNAG_GRAPH_FINAL: return "final";
    case SNAG_GRAPH_REFUSAL: return "refusal";
    case SNAG_GRAPH_NONPRODUCTIVE: return "nonproductive";
    case SNAG_GRAPH_CONFLICT: return "conflict";
    }
    return "unknown";
}
static const struct snag_term_command commands[] = {
    {"/help", "commands and keys"},
    {"/?", "commands and keys (alias for /help)"},
    {"/status", "session and next-turn settings"},
    {"/history", "recent terminal history"},
    {"/model [list|cache|#|SELECTOR [save|s]]", "list, refresh, or select a model"},
    {"/config", "edit and reload the active configuration"},
    {"/effort [LEVEL]", "show or set next-turn effort"},
    {"/goal [COMMAND|TEXT]", "show, start, or control a persistent goal"},
    {"/ro QUERY", "one read-only query; during a turn use Tab or /queue /ro QUERY"},
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
    {"/names", "show numbered IRC destinations, selection, room members, and modes"},
    {"/server [start [ENDPOINT]|stop]", "show, start, or stop hosting only"},
    {"/connect [ENDPOINT]", "add an outgoing connection (default localhost:6667)"},
    {"/disconnect [ENDPOINT]", "remove one or all outgoing connections; preserve hosting"},
    {"/N [TEXT]", "select destination N, or send there once without selecting"},
    {"/all TEXT", "broadcast once to all current destinations"}
};

static size_t
command_count(void)
{
    return sizeof(commands) / sizeof(commands[0]);
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
    if (!*preference || strlen(preference) >= SNAG_CONFIG_EFFORT_MAX ||
        !snag_utf8_valid((const unsigned char *)preference,
                        strlen(preference), true))
        return NULL;
    return preference;
}
static const struct snag_provider_config *
next_provider(const struct app_state *app)
{
    if (app->staged_provider)
        return app->staged_provider;
    return snag_config_provider(app->config,
        app->session.default_provider[0] ? app->session.default_provider : NULL);
}

static void
provider_capacity_source_sha256(
    const struct snag_provider_config *provider, const char *model,
    char digest[SNAG_SHA256_HEX_LEN + 1u])
{
    char source[SNAG_CONFIG_URL_MAX + SNAG_CONFIG_MODEL_MAX + 32u];
    const char *protocol = snag_provider_catalog_protocol(provider);
    size_t protocol_len = strlen(protocol);
    size_t base_url_len = strlen(provider->base_url);
    const char *upstream = snag_config_model_upstream(provider, model);
    size_t len = protocol_len + 1u + base_url_len;

    memcpy(source, protocol, protocol_len);
    source[protocol_len] = '\n';
    memcpy(source + protocol_len + 1u, provider->base_url, base_url_len);
    if (strcmp(upstream, model) != 0) {
        source[len++] = '\n';
        memcpy(source + len, upstream, strlen(upstream));
        len += strlen(upstream);
    }
    snag_sha256_hex(source, len, digest);
}

static bool
capacity_ceiling_matches(const struct app_state *app,
                         const struct snag_provider_config *provider,
                         const char *model)
{
    char source_hash[SNAG_SHA256_HEX_LEN + 1u];

    if (!app->session.capacity_ceiling_valid ||
        strcmp(app->session.capacity_ceiling_provider, provider->name) != 0 ||
        strcmp(app->session.capacity_ceiling_model, model) != 0)
        return false;
    provider_capacity_source_sha256(provider, model, source_hash);
    return strcmp(app->session.capacity_ceiling_source_sha256,
                  source_hash) == 0;
}

static void
apply_capacity_ceiling(const struct app_state *app,
                       const struct snag_provider_config *provider,
                       const char *model,
                       struct snag_model_capacity *capacity)
{
    if (capacity_ceiling_matches(app, provider, model) &&
        (!capacity->hard_input_known ||
         app->session.capacity_ceiling_input_tokens <
             capacity->hard_input_tokens)) {
        capacity->hard_input_tokens =
            app->session.capacity_ceiling_input_tokens;
        capacity->hard_input_known = true;
        capacity->source = SNAG_CAPACITY_OBSERVED;
    }
}

void
snag_app_record_model_accounting(struct app_state *app,
                                enum snag_count_capability capability,
                                uint64_t model_input_bytes,
                                uint64_t input_tokens,
                                uint64_t hard_input_tokens)
{
    char error[256] = {0};

    if (capability != SNAG_COUNT_UNKNOWN)
        app->turn_capacity.count_capability = capability;
    if (snag_model_cache_record(&app->store, &app->model_cache,
            app->turn_provider,
            snag_provider_catalog_protocol(app->turn_provider),
            app->turn_model, capability, model_input_bytes, input_tokens,
            hard_input_tokens, error, sizeof(error)) < 0)
        (void)app_warning(app, error[0] ? error :
            "model accounting observation could not be cached");
}

int
snag_app_capacity_resolve(struct app_state *app,
                         const struct snag_provider_config *provider,
                         const char *model,
                         struct snag_model_capacity *capacity,
                         char *error, size_t error_size)
{
    int cache_rc;

    if (!app || !provider || !model || !capacity) {
        snag_errorf(error, error_size, "invalid model capacity selection");
        errno = EINVAL;
        return -1;
    }
    snag_model_cache_free(&app->model_cache);
    app->capacity_cache_error[0] = '\0';
    cache_rc = snag_model_cache_load(&app->store, &app->model_cache,
                                    app->capacity_cache_error,
                                    sizeof(app->capacity_cache_error));
    if (cache_rc == 1)
        app->capacity_cache_error[0] = '\0';
    cache_rc = snag_model_capacity_resolve(&app->model_cache, app->config,
        provider, model, snag_provider_catalog_protocol(provider), capacity,
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
    const struct snag_provider_config *provider = next_provider(app);
    if (!provider) {
        snag_errorf(error, error_size,
                  "selected provider is not present in the current configuration");
        errno = ENOENT;
        return -1;
    }
    if (!effort) {
        snag_errorf(error, error_size,
                  "reasoning effort is empty, oversized, or invalid UTF-8");
        errno = ENOTSUP;
        return -1;
    }
    app->turn_model = model;
    app->turn_effort = effort;
    app->turn_provider = provider;
    if (snag_app_capacity_resolve(app, provider, model, &app->turn_capacity,
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
snag_app_commit_event(struct app_state *app, const char *type, json_t *data,
                     char *error, size_t error_size)
{
    uint64_t seq;
    off_t offset = app->session.log_end;
    if (snag_session_commit(&app->session, type, data, &seq,
                           error, error_size) < 0)
        return -1;
    struct snag_render_source source = {offset, (size_t)(app->session.log_end - offset)};
    if (snag_ui_durable(&app->ui, app->session.log_fd, source, type,
                       app->config->default_timeout_ms, app->config->max_output_bytes) < 0 ||
        snag_ui_event(&app->ui, seq, type) < 0) {
        snag_errorf(error, error_size, "durable event output failed");
        return -1;
    }
    return 0;
}
#define commit_event snag_app_commit_event
static const char *next_model(const struct app_state *app);
static const char *next_effort(const struct app_state *app);
static int
render_queue(struct app_state *app)
{
    if (app->session.pending_queue_count == 0u)
        return app_warning(app, "future-turn queue is empty");
    for (size_t i = 0; i < app->session.pending_queue_count; ++i) {
        char label[64];
        (void)snprintf(label, sizeof(label), "%zu %.8s%s › ", i + 1u,
                       app->session.pending_queue[i].queue_id,
                       app->session.pending_queue[i].read_only ? " /ro" : "");
        if (snag_ui_submitted(&app->ui, label,
                                 app->session.pending_queue[i].text, false) < 0)
            return -1;
    }
    return 0;
}

static int
format_context_meter(struct app_state *app, bool active,
                     char meter[32u])
{
    const struct snag_provider_config *provider = active ? app->turn_provider :
                                                        next_provider(app);
    const char *model = active ? app->turn_model : next_model(app);
    const char *effort = active ? app->turn_effort :
                                  resolve_effort(next_effort(app));
    struct snag_model_capacity resolved;
    const struct snag_model_capacity *capacity = &app->turn_capacity;
    char provider_source_hash[SNAG_SHA256_HEX_LEN + 1u];
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

        if (snag_app_capacity_resolve(app, provider, model, &resolved,
                                     error, sizeof(error)) < 0)
            return -1;
        capacity = &resolved;
    }
    provider_capacity_source_sha256(provider, model, provider_source_hash);
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

static unsigned int prompt_spinner_states(const struct app_state *app, bool active);

static void
prompt_hostname(char hostname[256u])
{
    if (gethostname(hostname, 256u) < 0)
        memcpy(hostname, "localhost", sizeof("localhost"));
    hostname[255u] = '\0';
    if (!snag_utf8_valid((const unsigned char *)hostname, strlen(hostname), true))
        memcpy(hostname, "localhost", sizeof("localhost"));
    for (size_t i = 0u; hostname[i]; ++i)
        if ((unsigned char)hostname[i] <= 0x20u || hostname[i] == 0x7f)
            hostname[i] = '_';
}

static int
set_input_prompt(struct app_state *app, bool active)
{
    const struct snag_provider_config *provider = active ? app->turn_provider :
                                                        next_provider(app);
    const char *model = active ? app->turn_model : next_model(app);
    const char *effort = active ? app->turn_effort :
                                  resolve_effort(next_effort(app));
    char hostname[256u], meter[32u], label[SNAG_TERM_LABEL_BYTES];
    const char *values[SNAG_PROMPT_HOUR];
    const char *spinners[SNAG_TERM_SPINNER_COUNT] = {
        app->config->prompt_spinner_goal,
        app->config->prompt_spinner_provider,
        app->config->prompt_spinner_tool
    };
    unsigned int states = prompt_spinner_states(app, active);
    unsigned int selected = app->ui.view == SNAG_RENDER_CHAT ?
                            0u : active ? 2u : 1u;

    if (!provider || !model || !effort ||
        format_context_meter(app, active, meter) < 0)
        return -1;
    prompt_hostname(hostname);
    values[0] = provider->name;
    values[1] = model;
    values[2] = effort;
    values[3] = app->irc ? snag_irc_operator_nick(app->irc) :
                          app->config->irc.operator_nick;
    values[4] = hostname;
    values[5] = meter;
    values[6] = selected == 0u ? "chat" :
                selected == 1u ? "rollout-idle" : "rollout-active";
    if (app->queue_edit_id[0]) {
        struct snag_buf out;

        snag_buf_init(&out, SNAG_TERM_LABEL_BYTES);
        for (unsigned int i = 0u; i < SNAG_TERM_SPINNER_SLOTS; ++i)
            if (snag_buf_putc(&out, SNAG_TERM_SPINNER_MARKER_BASE + i) < 0)
                goto fail;
        if (snag_buf_printf(&out, "%4s edit %zu ›", meter, app->queue_edit_number) < 0)
            goto fail;
        if (!out.len || snag_buf_putc(&out, ' ') < 0 ||
            snag_buf_terminate(&out) < 0)
            goto fail;
        memcpy(label, out.data, out.len + 1u);
        snag_buf_free(&out);
        return snag_ui_prompt(&app->ui, active, label, spinners,
            app->config->prompt_spinner_per_second, states);
fail:
        snag_buf_free(&out);
        return -1;
    }
    return snag_ui_composer(&app->ui, active, app->config->prompt, values,
        selected, spinners, app->config->prompt_spinner_per_second, states);
}

static int
validate_prompt_values(struct snag_ui *ui, const struct snag_config *config,
                       const struct snag_provider_config *provider,
                       const char *model, const char *effort)
{
    char hostname[256u];
    const char *values[SNAG_PROMPT_FIELD_COUNT];
    const char *spinners[SNAG_TERM_SPINNER_COUNT] = {
        config->prompt_spinner_goal,
        config->prompt_spinner_provider,
        config->prompt_spinner_tool
    };
    char label[SNAG_TERM_LABEL_BYTES];
    int rc = -1;

    if (!provider || !model || !effort)
        return -1;
    prompt_hostname(hostname);
    values[0] = provider->name;
    values[1] = model;
    values[2] = effort;
    values[3] = config->irc.operator_nick;
    values[4] = hostname;
    values[5] = "100%";
    values[SNAG_PROMPT_HOUR] = "23";
    values[SNAG_PROMPT_MINUTE] = "59";
    values[SNAG_PROMPT_SECOND] = "60";
    for (unsigned int mode = 0u; mode < 3u; ++mode) {
        values[6] = mode == 0u ? "chat" : mode == 1u ?
                    "rollout-idle" : "rollout-active";
        if (snag_config_prompt_expand(config->prompt, mode, values,
                SNAG_TERM_SPINNER_MARKER_BASE, label, sizeof(label)) < 0 ||
            snag_ui_validate_prompt(ui, label, spinners,
                config->prompt_spinner_per_second) < 0)
            goto out;
    }
    rc = 0;
out:
    return rc;
}

static int
validate_prompt_candidate(struct app_state *app,
                          const struct snag_config *config)
{
    const struct snag_provider_config *provider = snag_config_provider(
        config, app->session.default_provider[0] ?
        app->session.default_provider : NULL);

    return validate_prompt_values(&app->ui, config, provider, next_model(app),
                                  resolve_effort(next_effort(app)));
}

static unsigned int
prompt_spinner_states(const struct app_state *app, bool active)
{
    bool tool = app->tool_active || snag_tools_busy();
    return (app->session.goal_status == SNAG_GOAL_ACTIVE ?
            1u << SNAG_TERM_SPINNER_GOAL : 0u) |
           (active && !tool ?
            1u << SNAG_TERM_SPINNER_PROVIDER : 0u) |
           (tool ? 1u << SNAG_TERM_SPINNER_TOOL : 0u);
}

static int
tick_irc(struct app_state *app, char *error, size_t error_size)
{
    uint64_t revision = snag_irc_routing_revision(app->irc);

    if (snag_irc_tick(app->irc, 0, error, error_size) < 0 ||
        snag_app_sync_destinations(app) < 0)
        return -1;
    if (revision != snag_irc_routing_revision(app->irc)) {
        if (snag_app_irc_snapshot(app, "topology", error, error_size) < 0)
            return -1;
    }
    if (!snag_irc_identity_changed(app->irc))
        return 0;
    if (snag_app_irc_snapshot(app, "nick", error, error_size) < 0)
        return -1;
    if (!app->ui.opened || !app->ui.prompt_wanted)
        return 0;
    return set_input_prompt(app, app->ui.active);
}

static struct snag_queued_turn *
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
    struct snag_queued_turn *queued;
    struct snag_buf draft;
    int draft_rc;

    if (number == 0u || number > app->session.pending_queue_count) {
        snag_errorf(error, error_size, "queue item %zu does not exist", number);
        return 1;
    }
    queued = &app->session.pending_queue[number - 1u];
    memcpy(app->queue_edit_id, queued->queue_id, sizeof(app->queue_edit_id));
    app->queue_edit_number = number;
    app->queue_edit_was_armed = app->queue_armed;
    app->queue_armed = false;
    snag_buf_init(&draft, SNAG_MAX_QUEUED_TEXT + 8u);
    draft_rc = snag_buf_printf(&draft, "%s%s", queued->read_only ? "/ro " :
                              queued->text[0] == '/' ? "/" : "", queued->text);
    if (draft_rc == 0)
        draft_rc = snag_buf_terminate(&draft);
    if (draft_rc == 0 && set_input_prompt(app, active) == 0)
        draft_rc = snag_ui_restore_draft(&app->ui, (char *)draft.data);
    else draft_rc = -1;
    snag_buf_free(&draft);
    if (draft_rc < 0) {
        app->queue_armed = app->queue_edit_was_armed;
        app->queue_edit_id[0] = '\0';
        app->queue_edit_number = 0u;
        app->queue_edit_was_armed = false;
        snag_errorf(error, error_size, "queue editor could not be displayed");
        return -1;
    }
    return 0;
}

static int
finish_queue_edit(struct app_state *app, const char *text, bool active,
                  char *error, size_t error_size)
{
    struct snag_queued_turn *queued;
    const char *original = text;
    bool read_only;
    size_t len;
    bool restore_armed = app->queue_edit_was_armed;
    int rc = 0;

    text = snag_prompt_parse(text, &read_only);
    len = strlen(text);

    queued = queued_by_id(app, app->queue_edit_id, NULL);
    if (!queued) {
        snag_errorf(error, error_size, "the queued turn being edited no longer exists");
        (void)snag_ui_text(&app->ui, SNAG_UI_ERROR, error);
        error[0] = '\0';
        rc = 1;
        goto clear;
    }
    if (!len || len > SNAG_MAX_QUEUED_TEXT ||
        !snag_utf8_valid((const unsigned char *)text, len, true)) {
        snag_errorf(error, error_size,
                  "queued text must be nonempty valid UTF-8 within 256 KiB");
        (void)snag_ui_text(&app->ui, SNAG_UI_ERROR, error);
        error[0] = '\0';
        if (set_input_prompt(app, active) < 0 ||
            snag_ui_restore_draft(&app->ui, original) < 0)
            return -1;
        return 1;
    }
    if ((strcmp(queued->text, text) != 0 || queued->read_only != read_only) &&
        commit_event(app, "future_turn_edited",
                     snag_app_future_turn_edited_data(queued->queue_id, text, read_only),
                     error, error_size) < 0) {
        if (set_input_prompt(app, active) == 0)
            (void)snag_ui_restore_draft(&app->ui, original);
        return -1;
    }
    if (snag_ui_submitted(&app->ui,
            app->ui.label, original, false) < 0) {
        snag_errorf(error, error_size, "edited turn acknowledgement could not be rendered");
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
    char queue_id[SNAG_ID_HEX_LEN + 1u];
    bool read_only;
    const char *queued_text = snag_prompt_parse(text, &read_only);
    size_t len;
    if (!app->session.active_turn) {
        snag_errorf(error, error_size, "/queue TEXT is valid only while a turn is active");
        errno = EINVAL;
        return 1;
    }
    if (text[0] == '/' && !read_only) {
        if (text[1] != '/') {
            snag_errorf(error, error_size,
                      "queued text starting with / must use // for a literal slash");
            errno = EINVAL;
            return 1;
        }
    }
    len = strlen(queued_text);
    if (!len || len > SNAG_MAX_QUEUED_TEXT ||
        !snag_utf8_valid((const unsigned char *)queued_text, len, true)) {
        snag_errorf(error, error_size,
                  "queued text must be nonempty valid UTF-8 within 256 KiB");
        errno = EINVAL;
        return 1;
    }
    if (snag_random_id(queue_id) < 0) {
        snag_errorf(error, error_size, "cryptographic queue id generation failed");
        return -1;
    }
    if (commit_event(app, "future_turn_queued",
                     snag_app_future_turn_queued_data(app->session.active_turn_id,
                                             queue_id, queued_text, read_only),
                     error, error_size) < 0)
        return -1;
    if (snag_ui_submitted(&app->ui, "next › ", text, false) < 0) {
        snag_errorf(error, error_size, "queued turn acknowledgement could not be rendered");
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
    bool remove[SNAG_MAX_PENDING_TURNS] = {false};
    size_t matches;

    if (app->session.pending_queue_count == 0u ||
        (!all && index >= app->session.pending_queue_count)) {
        snag_errorf(error, error_size, "future-turn queue is empty");
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
                     snag_app_future_turn_cancelled_data(&app->session, remove),
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
    if (snag_app_parse_queue_argument(argument, &kind, &number) < 0) {
        snag_errorf(error, error_size,
                  "queue action expects clear, pop, N delete, Nd, N edit, or Ne");
        return 1;
    }
    switch (kind) {
    case QUEUE_COMMAND_LIST:
        return render_queue(app);
    case QUEUE_COMMAND_ADD:
        if (!active) {
            snag_errorf(error, error_size,
                      "/queue TEXT is active-only; submit it during a running turn");
            return 1;
        }
        return queue_future_turn(app, argument, true, error, error_size);
    case QUEUE_COMMAND_DELETE:
        if (number == 0u || number > app->session.pending_queue_count) {
            snag_errorf(error, error_size, "queue item %zu does not exist", number);
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
            snag_errorf(error, error_size, "future-turn queue is empty");
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
append_capacity_value(struct snag_buf *text, const char *name,
                      bool known, uint64_t value)
{
    return known ?
        snag_buf_printf(text, " · %s=%llu", name,
                       (unsigned long long)value) :
        snag_buf_printf(text, " · %s=unknown", name);
}

static int
append_advertised_capacity(struct snag_buf *text, const json_t *model)
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
    if (snag_buf_append(text, "\nadvertised", 11u) < 0)
        return -1;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        uint64_t value = 0u;
        bool known = snag_json_integer_u64((json_t *)limits,
                                          fields[i].key, &value) == 0;
        if (append_capacity_value(text, fields[i].name, known, value) < 0)
            return -1;
    }
    return 0;
}

static int
append_compact_threshold(struct snag_buf *text,
                         const struct snag_provider_config *provider,
                         const struct snag_model_capacity *capacity)
{
    const char *mode = provider->auto_compact_input_tokens ==
        SNAG_CONFIG_COMPACT_AUTO ?
            (capacity->hard_input_known ? "auto" : "auto fallback") :
            (provider->auto_compact_input_tokens ? "fixed" : "off");

    return snag_buf_printf(text, " · compact=%llu (%s)",
        (unsigned long long)snag_model_compact_threshold(provider, capacity),
        mode);
}

static int
render_status(struct app_state *app)
{
    const char *id = app->session.id;
    const struct snag_provider_config *provider = next_provider(app);
    const struct snag_model_limit_config *configured = NULL;
    struct snag_model_limit_config configured_values;
    const struct snag_model_limit_config *rule_sources[3];
    const json_t *advertised = NULL;
    struct snag_model_capacity capacity;
    struct snag_buf text;
    bool ceiling_selection_matches;
    bool ceiling_source_matches;
    char error[256] = {0};
    int rc = -1;

    if (!provider)
        return app_error(app,
            "selected provider is not present in the current configuration");
    if (snag_app_capacity_resolve(app, provider, next_model(app), &capacity,
                                 error, sizeof(error)) < 0)
        return app_error(app, error[0] ? error :
                         "model capacity could not be resolved");
    if (snag_config_resolve_limits(app->config, provider->name, next_model(app),
                                  &configured_values, rule_sources))
        configured = &configured_values;
    ceiling_selection_matches = app->session.capacity_ceiling_valid &&
        strcmp(app->session.capacity_ceiling_provider, provider->name) == 0 &&
        strcmp(app->session.capacity_ceiling_model, next_model(app)) == 0;
    ceiling_source_matches = ceiling_selection_matches &&
        capacity_ceiling_matches(app, provider, next_model(app));
    if (capacity.source_bound)
        advertised = snag_model_metadata(&app->model_cache, provider, next_model(app));
    snag_buf_init(&text, 64u * 1024u);
    if (snag_buf_printf(&text,
        "session: %s\n"
        "state: %s\n"
        "tools: %s\n"
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
        app->session.active_read_only ? "read-only query" : "normal",
        next_provider(app) ? next_provider(app)->name : "<missing>",
        app->staged_provider ? " (staged once)" : "",
        next_model(app), app->staged_model ? " (staged once)" : "",
        next_effort(app), app->staged_effort ? " (staged once)" : "",
        app->session.workspace,
        (unsigned long long)app->session.turn_count,
        app->session.pending_queue_count,
        app->session.pending_queue_count && !app->queue_armed ? " paused" : "",
        snag_ui_verbosity(&app->ui), snag_capacity_source_name(capacity.source)) < 0 ||
        append_capacity_value(&text, "hard-input",
                              capacity.hard_input_known,
                              capacity.hard_input_tokens) < 0 ||
        append_capacity_value(&text, "requested-output",
                              capacity.max_output_tokens,
                              capacity.max_output_tokens) < 0 ||
        append_compact_threshold(&text, provider, &capacity) < 0)
        goto out;
    if (capacity.effective_context_window_percent &&
        snag_buf_printf(&text, " · effective=%u%%%s",
                       capacity.effective_context_window_percent,
                       capacity.effective_context_window_derived ?
                           " (derived client policy)" : " (advertised)") < 0)
        goto out;
    if (snag_buf_printf(&text, "\nmax_parallel_commands: %u\nparallel_tool_calls: %s",
        app->session.active_turn ? app->session.max_parallel_commands : app->config->max_parallel_commands,
        (app->session.active_turn ? app->session.parallel_tool_calls : provider->parallel_tool_calls) ?
        "true" : "false") < 0)
        goto out;
    if (configured) {
        if (snag_buf_append(&text, "\nconfigured", 11u) < 0 ||
            append_capacity_value(&text, "context",
                configured->context_window_tokens,
                configured->context_window_tokens) < 0 ||
            append_capacity_value(&text, "max-input",
                configured->max_input_tokens,
                configured->max_input_tokens) < 0 ||
            append_capacity_value(&text, "max-output",
                configured->max_output_tokens,
                configured->max_output_tokens) < 0)
            goto out;
    }
    if (append_advertised_capacity(&text, advertised) < 0)
        goto out;
    if (snag_buf_printf(&text, "\nprovider model: %s", snag_config_model_upstream(provider, next_model(app))) < 0)
        goto out;
    for (size_t i = 0; i < 3u; ++i) {
        static const char *const fields[] = {"context", "max-input", "max-output"};
        const struct snag_model_limit_config *rule = rule_sources[i];
        if (rule && snag_buf_printf(&text, "\n%s rule: [model-limit %s%s%s]", fields[i],
                                    rule->provider, rule->model[0] ? "/" : "", rule->model) < 0)
            goto out;
    }
    if (capacity.cache_source_mismatch &&
        snag_buf_append(&text,
            "\ncatalog source: mismatch; advertised limits ignored",
            strlen("\ncatalog source: mismatch; advertised limits ignored")) < 0)
        goto out;
    if (app->capacity_cache_error[0] &&
        snag_buf_printf(&text, "\ncatalog: %s", app->capacity_cache_error) < 0)
        goto out;
    if (app->session.capacity_ceiling_valid) {
        if (snag_buf_printf(&text,
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
    } else if (snag_buf_append(&text, "\nobserved ceiling: unknown",
                              strlen("\nobserved ceiling: unknown")) < 0) {
        goto out;
    }
    if (snag_buf_printf(&text,
            "\naccounting: policy=%s · exact-count=%s · estimate=%s",
            provider->exact_token_count == SNAG_TOKEN_COUNT_AUTO ? "auto" :
            provider->exact_token_count == SNAG_TOKEN_COUNT_STRICT ? "exact" :
            "off",
            capacity.count_capability == SNAG_COUNT_SUPPORTED ? "supported" :
            capacity.count_capability == SNAG_COUNT_UNSUPPORTED ? "unsupported" :
            "unknown", capacity.observed_tokens_per_million_bytes ? "learned" :
            "none") < 0)
        goto out;
    if (app->session.usage_anchor_valid) {
        if (snag_buf_printf(&text,
                "\nobserved usage: input=%llu tokens · model-input=%llu bytes · provider=%s · model=%s · effort=%s",
                (unsigned long long)app->session.usage_anchor_input_tokens,
                (unsigned long long)app->session.usage_anchor_model_input_bytes,
                app->session.usage_anchor_provider,
                app->session.usage_anchor_model,
                app->session.usage_anchor_effort) < 0)
            goto out;
    } else if (snag_buf_append(&text, "\nobserved usage: unknown",
                              strlen("\nobserved usage: unknown")) < 0) {
        goto out;
    }
    if (app->irc && (snag_buf_putc(&text, '\n') < 0 ||
        snag_irc_state(app->irc, &text, NULL, 0u) < 0))
        goto out;
    if (snag_buf_terminate(&text) < 0)
        goto out;
    rc = snag_ui_text(&app->ui, SNAG_UI_HOST, (const char *)text.data);
out:
    snag_buf_free(&text);
    return rc;
}
static int
render_help(struct app_state *app)
{
    static const char keys[] =
        "Chat Enter broadcast to enabled destinations (mention to steer) · "
        "Rollout Enter private submit/add to active turn · "
        "Empty Tab switch view · Tab complete/indent/queue (chat: @nick) · "
        "Ctrl-C cancel/interrupt · Ctrl-D exit · Ctrl-J newline";
    struct snag_buf text;
    int rc = -1;

    snag_buf_init(&text, 64u * 1024u);
    for (size_t i = 0u; i < command_count(); ++i)
        if (snag_buf_printf(&text, "%-28s%s\n", commands[i].syntax,
                           commands[i].description) < 0)
            goto out;
    static const char levels[] =
        "\n\nVerbosity (-v count = /verbose N):\n"
        "0 conversation · 1 compact tool rows, no output\n"
        "2 previews: 1024 argument / 512 output characters\n"
        "3 full retained tools · 4 debug · 5 redacted protocol · 6 wire\n"
        "Chat is unchanged. Debug traces are live in /rollout only.";
    if (snag_buf_append(&text, keys, strlen(keys)) < 0 ||
        snag_buf_append(&text, levels, sizeof(levels) - 1u) < 0 ||
        snag_buf_terminate(&text) < 0)
        goto out;
    rc = snag_ui_text(&app->ui, SNAG_UI_HOST, (const char *)text.data);
out:
    snag_buf_free(&text);
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
        const struct snag_provider_config *provider = &app->config->providers[i];
        json_t *models = NULL;
        json_t *entry = NULL;
        char detail[256] = {0};

        if (snag_app_provider_models(app, provider, &models,
                                    detail, sizeof(detail)) < 0) {
            snag_errorf(error, error_size, "cannot refresh provider %s: %s",
                      provider->name, detail[0] ? detail : strerror(errno));
            goto out;
        }
        entry = json_object();
        if (!entry) {
            json_decref(models);
            snag_errorf(error, error_size, "cannot assemble model cache");
            errno = ENOMEM;
            goto out;
        }
        if (snag_json_set_new(entry, "models", models) < 0) {
            models = NULL;
            json_decref(entry);
            snag_errorf(error, error_size, "cannot assemble model cache");
            errno = ENOMEM;
            goto out;
        }
        models = NULL;
        if (snag_json_set_new(entry, "base_url",
                             json_string(provider->base_url)) < 0 ||
            snag_json_set_new(entry, "name", json_string(provider->name)) < 0 ||
            snag_json_set_new(entry, "protocol",
                json_string(snag_provider_catalog_protocol(provider))) < 0) {
            json_decref(entry);
            snag_errorf(error, error_size, "cannot assemble model cache");
            errno = ENOMEM;
            goto out;
        }
        if (json_array_append_new(providers, entry) < 0) {
            entry = NULL;
            snag_errorf(error, error_size, "cannot assemble model cache");
            errno = ENOMEM;
            goto out;
        }
        entry = NULL;
    }
    if (snag_model_cache_replace(&app->store, providers, snag_time_ms(),
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
    rc = snag_model_cache_load(&app->store, &app->model_cache,
                              error, error_size);
    if (rc == 1) {
        for (size_t i = 0; i < app->config->provider_count; ++i)
            if (app->config->providers[i].model_count)
                return 0; /* Configured models do not require discovery. */
        snag_errorf(error, error_size,
                  "model cache is empty; use /model cache while idle");
        errno = ENOENT;
        return -1;
    }
    return rc;
}
static int
append_catalog_limits(struct snag_buf *text, const json_t *model)
{
    const json_t *limits = model ? json_object_get(model, "limits") : NULL;
    const char *count = model ?
        snag_json_string(model, "count_capability") : NULL;
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
        if (snag_json_integer_u64((json_t *)limits, fields[i].key, &value) < 0)
            continue;
        if (snag_buf_printf(text, "%s%s=%llu",
                           any ? "," : " · ", fields[i].label,
                           (unsigned long long)value) < 0)
            return -1;
        any = true;
    }
    if (count)
        (void)snag_json_integer_u64(model, "observed_input_bytes",
                                   &observed);
    if (count && snag_buf_printf(text, " · count=%s · estimate=%s", count,
                                observed ? "learned" : "none") < 0)
        return -1;
    return 0;
}

static int
model_picker_entry(struct app_state *app, size_t index,
                    const char **provider, const char **model, const char **effort)
{
    return snag_model_entry(&app->model_cache, app->config, index,
                            resolve_effort(app->config->reasoning_effort), provider, model, effort);
}

struct model_catalog_view {
    const struct snag_config *config;
    struct snag_buf *text;
};

static int
append_model_row(void *opaque, size_t index, const char *provider, const char *model,
                  const char *effort, const json_t *metadata)
{
    struct model_catalog_view *view = opaque;
    struct snag_model_limit_config limits;

    if (snag_buf_printf(view->text, "\n%zu. %s / %s / %s", index, provider, model, effort) < 0 ||
        append_catalog_limits(view->text, metadata) < 0)
        return -1;
    if (snag_config_resolve_limits(view->config, provider, model, &limits, NULL)) {
        if (limits.context_window_tokens &&
            append_capacity_value(view->text, "configured-context", true, limits.context_window_tokens) < 0)
            return -1;
        if (limits.max_input_tokens &&
            append_capacity_value(view->text, "configured-input", true, limits.max_input_tokens) < 0)
            return -1;
        if (limits.max_output_tokens &&
            append_capacity_value(view->text, "configured-output", true, limits.max_output_tokens) < 0)
            return -1;
    }
    return 0;
}

static int
render_model_catalog(struct app_state *app)
{
    const struct snag_provider_config *selected = next_provider(app);
    struct snag_model_capacity capacity;
    struct snag_buf text;
    char error[256] = {0};
    char timestamp[64];
    time_t seconds;
    struct tm broken;
    struct model_catalog_view view = {app->config, &text};
    int rc = -1;

    if (!selected)
        return app_error(app,
            "selected provider is not present in the current configuration");
    if (snag_app_capacity_resolve(app, selected, next_model(app), &capacity,
                                 error, sizeof(error)) < 0)
        return app_error(app, error);
    seconds = (time_t)(app->model_cache.updated_at_ms / 1000u);
    snag_buf_init(&text, 16u * 1024u * 1024u);
    if (snag_buf_printf(&text, "selected: %s / %s / %s%s",
                       selected->name, next_model(app),
                       resolve_effort(next_effort(app)) ?
                           resolve_effort(next_effort(app)) : next_effort(app),
                       app->staged_provider || app->staged_model ||
                       app->staged_effort ? " (staged once)" : "") < 0 ||
        append_compact_threshold(&text, selected, &capacity) < 0)
        goto out;
    if (append_capacity_value(&text, "effective-context", capacity.context_window_tokens,
                                capacity.context_window_tokens) < 0 ||
        append_capacity_value(&text, "hard-input", capacity.hard_input_known, capacity.hard_input_tokens) < 0 ||
        snag_model_each(&app->model_cache, app->config,
                          resolve_effort(app->config->reasoning_effort), append_model_row, &view) < 0)
        goto out;
    if (gmtime_r(&seconds, &broken) &&
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ",
                 &broken) != 0u) {
        if (snag_buf_printf(&text, "\ncache updated: %s", timestamp) < 0)
            goto out;
    } else if (snag_buf_printf(&text, "\ncache updated: %llu ms since epoch",
                              (unsigned long long)
                                  app->model_cache.updated_at_ms) < 0) {
        goto out;
    }
    if (snag_buf_terminate(&text) < 0)
        goto out;
    rc = snag_ui_text(&app->ui, SNAG_UI_HOST, (const char *)text.data);
out:
    snag_buf_free(&text);
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
record_model_selection(struct app_state *app,
                        const char *provider,
                        const char *model, const char *effort,
                        char *error, size_t error_size)
{
    if (strcmp(app->session.default_provider, provider) == 0 &&
        strcmp(app->session.default_model, model) == 0 &&
        strcmp(app->session.default_effort, effort) == 0)
        return 0;
    return commit_event(app, "model_selection_changed",
        snag_app_model_selection_changed_data(app->session.default_provider, provider,
            app->session.default_model, model, app->session.default_effort, effort),
        error, error_size);
}

static int
commit_model_selection(struct app_state *app,
                       const struct snag_provider_config *provider,
                       const char *model, const char *effort,
                       bool known_in_cache, bool save)
{
    char error[256] = {0};
    int rc;

    if (save) {
        struct stat st;
        bool missing = app->config_allow_create && lstat(app->config_path, &st) < 0 && errno == ENOENT;
        int save_rc = missing ? snag_config_save_provider(app->config_path, true, provider,
                                model, effort, error, sizeof(error)) :
            snag_config_save_model(app->config_path, app->config_allow_create, provider->name,
                                   model, effort, error, sizeof(error));
        if (save_rc < 0)
            return app_error(app, error[0] ? error : "model settings could not be written to configuration");
    }
    if (record_model_selection(app, provider->name, model, effort, error, sizeof(error)) < 0) {
        (void)app_error(app, error[0] ? error : "model selection could not be saved");
        return -1;
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
            "model is not known in the model cache; the configured provider will still be used") < 0)
        return -1;
    return save ? app_hostf(app, "configuration saved: %s", app->config_path) : 0;
}
static int
select_cached_model(struct app_state *app, const char *value, bool save)
{
    const struct snag_provider_config *provider_config;
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
    entry_rc = model_picker_entry(app, index, &provider, &model, &effort);
    if (entry_rc != 0)
        return app_error(app, "model index is not in the displayed cache");
    provider_config = snag_config_provider(app->config, provider);
    if (!provider_config)
        return app_error(app,
            "cached provider is not configured; use /model cache");
    return commit_model_selection(app, provider_config, model, effort, true,
                                  save);
}
static int
select_typed_model(struct app_state *app, const char *value, bool save)
{
    const struct snag_provider_config *provider;
    const char *model;
    const char *effort;
    char *copy = snag_strdup_checked(value, SNAG_CONFIG_PATH_MAX);
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
    provider = count == 3u ? snag_config_provider(app->config, parts[0]) :
                             next_provider(app);
    if (!provider) {
        (void)app_error(app, count == 3u ?
            "model selector names an unconfigured provider" :
            "no provider is configured");
        goto out;
    }
    model = parts[count == 3u ? 1u : 0u];
    effort = count >= 2u ? parts[count - 1u] : next_effort(app);
    if (strlen(model) >= SNAG_CONFIG_MODEL_MAX ||
        !snag_utf8_valid((const unsigned char *)model, strlen(model), true) ||
        !resolve_effort(effort)) {
        (void)app_error(app,
            "model or effort exceeds the supported structural bounds");
        goto out;
    }
    {
        char ignored[256] = {0};
        if (snag_model_cache_load(&app->store, &app->model_cache,
                                 ignored, sizeof(ignored)) == 0) {
            const json_t *cached = snag_model_metadata(&app->model_cache, provider, model);
            known_in_cache = cached != NULL;
            if (count == 1u)
                effort = snag_model_cache_best_effort(cached,
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
        copy = snag_strdup_checked(value, SNAG_CONFIG_PATH_MAX);
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
    char sha256[SNAG_SHA256_HEX_LEN + 1u];
};

static int
snapshot_config(const char *path, struct config_snapshot *snapshot,
                char *error, size_t error_size)
{
    struct snag_sha256 digest;
    struct stat st;
    unsigned char hash[32];
    int fd;

    memset(snapshot, 0, sizeof(*snapshot));
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT)
            return 0;
        snag_errorf(error, error_size, "cannot open configuration %s: %s",
                  path, strerror(errno));
        return -1;
    }
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uintmax_t)st.st_size > SNAG_CONFIG_FILE_MAX) {
        snag_errorf(error, error_size,
                  "configuration must be a regular file no larger than 64 KiB");
        errno = EINVAL;
        (void)close(fd);
        return -1;
    }
    snag_sha256_init(&digest);
    for (;;) {
        unsigned char bytes[4096];
        ssize_t got = read(fd, bytes, sizeof(bytes));
        if (got > 0) {
            snag_sha256_update(&digest, bytes, (size_t)got);
            continue;
        }
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0) {
            snag_errorf(error, error_size, "cannot read configuration: %s",
                      strerror(errno));
            (void)close(fd);
            return -1;
        }
        break;
    }
    if (close(fd) < 0) {
        snag_errorf(error, error_size, "cannot close configuration: %s",
                  strerror(errno));
        return -1;
    }
    snag_sha256_final(&digest, hash);
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

static bool
irc_config_equal(const struct snag_config *left,
                 const struct snag_config *right)
{
    if (left->irc.listen_explicit != right->irc.listen_explicit ||
        strcmp(left->irc.listen, right->irc.listen) != 0 ||
        left->irc.client_count != right->irc.client_count ||
        strcmp(left->irc.model_nick, right->irc.model_nick) != 0 ||
        strcmp(left->irc.operator_nick, right->irc.operator_nick) != 0 ||
        left->irc.model_nick_implicit != right->irc.model_nick_implicit ||
        left->irc.operator_nick_implicit !=
            right->irc.operator_nick_implicit ||
        strcmp(left->irc.room_name, right->irc.room_name) != 0 ||
        left->irc.history_lines != right->irc.history_lines)
        return false;
    for (size_t i = 0u; i < left->irc.client_count; ++i)
        if (strcmp(left->irc.clients[i], right->irc.clients[i]) != 0)
            return false;
    return true;
}

static int
apply_network(struct app_state *app, struct snag_config *candidate,
               char *error, size_t error_size)
{
    int rc = 0;

    if (snag_irc_normalize(candidate, error, error_size) < 0)
        return 1;
    if (irc_config_equal(app->config, candidate))
        return 0;
    if (snag_irc_configure(app->irc, candidate, app->session.workspace,
                          error, error_size) < 0) {
        char original[256], rollback[256] = {0};

        (void)snag_strcpy(original, sizeof(original),
                         error[0] ? error : "IRC change failed");
        if (snag_irc_configure(app->irc, app->config, app->session.workspace,
                               rollback, sizeof(rollback)) < 0)
            snag_errorf(error, error_size, "%s; restoration failed: %s; /status shows remaining roles",
                        original, rollback[0] ? rollback : strerror(errno));
        else
            snag_errorf(error, error_size, "%s; previous roles restored", original);
        rc = 1;
    } else {
        app->config->irc = candidate->irc;
    }
    snag_irc_roles(app->irc, app->config);
    candidate->irc = app->config->irc;
    app->networked = snag_irc_enabled(app->config);
    if (snag_ui_model_nick(&app->ui,
                          snag_irc_model_nick(app->irc)) < 0 ||
        snag_app_irc_snapshot(app, "topology", NULL, 0u) < 0 ||
        tick_irc(app, NULL, 0u) < 0)
        return -1;
    return rc;
}

static void
merge_file_network(struct app_state *app, struct snag_config *candidate)
{
    const struct snag_irc_config *old = &app->irc_file_config;
    struct snag_irc_config file = candidate->irc;

    candidate->irc = app->config->irc;
    if (file.listen_explicit != old->listen_explicit ||
        strcmp(file.listen, old->listen) != 0) {
        candidate->irc.listen_explicit = file.listen_explicit;
        memcpy(candidate->irc.listen, file.listen, sizeof(file.listen));
    }
    if (file.client_count != old->client_count ||
        memcmp(file.clients, old->clients, sizeof(file.clients)) != 0) {
        candidate->irc.client_count = file.client_count;
        memcpy(candidate->irc.clients, file.clients, sizeof(file.clients));
    }
    if (strcmp(file.model_nick, old->model_nick) != 0) {
        memcpy(candidate->irc.model_nick, file.model_nick, sizeof(file.model_nick));
        candidate->irc.model_nick_implicit = file.model_nick_implicit;
    }
    if (strcmp(file.operator_nick, old->operator_nick) != 0) {
        memcpy(candidate->irc.operator_nick, file.operator_nick, sizeof(file.operator_nick));
        candidate->irc.operator_nick_implicit = file.operator_nick_implicit;
    }
    if (strcmp(file.room_name, old->room_name) != 0)
        memcpy(candidate->irc.room_name, file.room_name, sizeof(file.room_name));
    if (file.history_lines != old->history_lines)
        candidate->irc.history_lines = file.history_lines;
}

static int
reload_config(struct app_state *app, char *error, size_t error_size)
{
    struct snag_config candidate;
    struct snag_config previous;
    const char *selected_provider = app->session.default_provider[0] ?
        app->session.default_provider : NULL;
    struct snag_irc_config file_network;
    int rc = 1;

    snag_config_init(&candidate);
    if (!candidate.shell) {
        snag_errorf(error, error_size, "cannot initialize configuration defaults");
        goto out;
    }
    if (snag_config_load(&candidate,
            app->config_allow_create ? NULL : app->config_path,
            app->store.root_path, error, error_size) < 0)
        goto out;
    file_network = candidate.irc;
    merge_file_network(app, &candidate);
    if (snag_irc_normalize(&candidate, error, error_size) < 0)
        goto out;
    if (!snag_config_provider(&candidate, selected_provider)) {
        snag_errorf(error, error_size,
                  "reloaded configuration does not define the selected provider");
        errno = EINVAL;
        goto out;
    }
    if (validate_prompt_candidate(app, &candidate) < 0) {
        snag_errorf(error, error_size,
                  "reloaded prompt cannot be rendered with the current selection");
        errno = EINVAL;
        goto out;
    }
    rc = apply_network(app, &candidate, error, error_size);
    if (rc != 0)
        goto out;
    previous = *app->config;
    *app->config = candidate;
    memset(&candidate, 0, sizeof(candidate));
    app->irc_file_config = file_network;
    app->turn_provider = snag_config_provider(app->config, selected_provider);
    app->turn_model = app->session.default_model;
    app->staged_provider = NULL;
    snag_ui_color(&app->ui, snag_cli_color(app->cli, app->config->color));
    snag_ui_markdown(&app->ui, snag_cli_markdown(app->cli, app->config->markdown));
    snag_ui_model_nick(&app->ui,
                             app->networked ?
                                 app->config->irc.model_nick : NULL);
    snag_ui_commands(&app->ui, commands, command_count());
    snag_ui_typing_pause(&app->ui, app->config->typing_pause_ms);
    snag_config_free(&previous);
    rc = 0;
out:
    snag_config_free(&candidate);
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
        snag_errorf(error, error_size, "$EDITOR is not set");
        errno = ENOENT;
        return 1;
    }
    if (snag_ui_external(&app->ui, true, error, error_size) < 0)
        return -1;
    child = fork();
    if (child == 0) {
        sigset_t signals;
        sigemptyset(&signals);
        (void)sigprocmask(SIG_SETMASK, &signals, NULL);
        execl("/bin/sh", "sh", "-c", "exec $EDITOR \"$1\"",
              "snajpagent-editor", app->config_path, (char *)NULL);
        _exit(127);
    }
    if (child < 0) {
        snag_errorf(error, error_size, "cannot start $EDITOR: %s",
                  strerror(errno));
        (void)snag_ui_external(&app->ui, false, NULL, 0u);
        return -1;
    }
    do {
        got = waitpid(child, status, 0);
    } while (got < 0 && errno == EINTR);
    if (snag_ui_external(&app->ui, false, error, error_size) < 0)
        return -1;
    if (got != child) {
        snag_errorf(error, error_size, "cannot wait for $EDITOR: %s",
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
    char error[256] = {0};
    char *copy = NULL;
    char *effort = NULL;
    if (!value)
        return show_setting(app, "effort", next_effort(app),
                            app->staged_effort != NULL);
    if (active)
        return app_error(app, "/effort LEVEL is idle-only; interrupt or wait");
    copy = snag_strdup_checked(value, SNAG_CONFIG_EFFORT_MAX - 1u);
    if (copy)
        effort = trim_selector_part(copy);
    if (!effort || !resolve_effort(effort)) {
        free(copy);
        return app_error(app,
            "effort exceeds the supported structural bounds");
    }
    if (record_model_selection(app, app->session.default_provider,
            app->session.default_model, effort, error, sizeof(error)) < 0) {
        (void)app_error(app, error[0] ? error : "effort preference could not be saved");
        free(copy);
        return -1;
    }
    app->staged_effort = NULL;
    free(copy);
    return show_setting(app, "effort", app->session.default_effort, false);
}
static int
select_view(struct app_state *app, enum snag_render_view view, bool active)
{
    if (snag_ui_set_view(&app->ui, view) < 0 ||
        set_input_prompt(app, active) < 0)
        return -1;
    return 0;
}
static int
toggle_view(struct app_state *app)
{
    enum snag_render_view view;

    view = app->ui.view == SNAG_RENDER_CHAT ?
           SNAG_RENDER_ROLLOUT : SNAG_RENDER_CHAT;
    return select_view(app, view, app->session.active_turn);
}
static int
network_command(struct app_state *app, const char *line, bool *handled)
{
    struct snag_config candidate = *app->config;
    struct snag_irc_config *config = &candidate.irc;
    char copy[SNAG_CONFIG_IRC_ENDPOINT_MAX + 32u], error[512] = {0};
    char *words[4], *save = NULL, *word;
    size_t count = 0u;
    bool server, connect, disconnect;
    const char *endpoint;
    int rc;

    server = strncmp(line, "/server", 7u) == 0 &&
             (!line[7] || isspace((unsigned char)line[7]));
    connect = strncmp(line, "/connect", 8u) == 0 &&
              (!line[8] || isspace((unsigned char)line[8]));
    disconnect = strncmp(line, "/disconnect", 11u) == 0 &&
                 (!line[11] || isspace((unsigned char)line[11]));
    *handled = server || connect || disconnect;
    if (!*handled)
        return 0;
    if (!snag_strcpy(copy, sizeof(copy), line))
        return app_error(app, "network command is too long");
    for (word = strtok_r(copy, " \t", &save); word && count < 4u;
         word = strtok_r(NULL, " \t", &save))
        words[count++] = word;
    if (!count || count == 4u || (!server && count > 2u))
        return app_error(app, "usage: /server [start [ENDPOINT]|stop], /connect [ENDPOINT], /disconnect [ENDPOINT]");
    if (server) {
        if (count == 1u)
            return app_hostf(app, config->listen_explicit ?
                "hosting %s" : "hosting is off; use /server start [ENDPOINT]", config->listen);
        if (count == 2u && strcmp(words[1], "stop") == 0) {
            if (!config->listen_explicit)
                return app_hostf(app, "hosting is already off; outgoing connections unchanged");
            config->listen_explicit = false;
        } else if (count >= 2u && strcmp(words[1], "start") == 0) {
            endpoint = count == 3u ? words[2] : "localhost:6667";
            if (config->listen_explicit) {
                if (snag_irc_endpoint_equal(config->listen, endpoint))
                    return app_hostf(app, "already hosting %s", config->listen);
                return app_error(app, "already hosting another endpoint; use /server stop first");
            }
            if (!snag_strcpy(config->listen, sizeof(config->listen), endpoint))
                return app_error(app, "IRC endpoint is too long");
            config->listen_explicit = true;
        } else {
            return app_error(app, "usage: /server [start [ENDPOINT]|stop]");
        }
    } else if (connect) {
        endpoint = count == 2u ? words[1] : "localhost:6667";
        if (config->listen_explicit && snag_irc_endpoint_equal(config->listen, endpoint))
            return app_hostf(app, "already hosting %s; no self-connection needed", endpoint);
        for (size_t i = 0u; i < config->client_count; ++i)
            if (snag_irc_endpoint_equal(config->clients[i], endpoint))
                return app_hostf(app, "outgoing connection already configured: %s", endpoint);
        if (config->client_count == SNAG_CONFIG_IRC_CLIENT_MAX)
            return app_error(app, "at most 16 outgoing connections are supported");
        if (!snag_strcpy(config->clients[config->client_count],
                         sizeof(config->clients[0]), endpoint))
            return app_error(app, "IRC endpoint is too long");
        ++config->client_count;
    } else {
        size_t index;

        if (!config->client_count)
            return app_hostf(app, "no outgoing connections; hosting unchanged");
        if (count == 1u) {
            config->client_count = 0u;
            memset(config->clients, 0, sizeof(config->clients));
        } else {
            for (index = 0u; index < config->client_count; ++index)
                if (snag_irc_endpoint_equal(config->clients[index], words[1]))
                    break;
            if (index == config->client_count)
                return app_hostf(app, "outgoing endpoint is not configured: %s", words[1]);
            memmove(config->clients + index, config->clients + index + 1u,
                    (--config->client_count - index) * sizeof(config->clients[0]));
            memset(config->clients[config->client_count], 0, sizeof(config->clients[0]));
        }
    }
    rc = apply_network(app, &candidate, error, sizeof(error));
    if (rc != 0)
        return rc < 0 ? -1 : app_error(app, error);
    if (connect)
        return app_hostf(app, "outgoing connection added; /status shows connection state; /chat opens public chat");
    if (disconnect)
        return app_hostf(app, "outgoing connection%s removed; hosting unchanged",
                          count == 1u ? "s" : "");
    return app_hostf(app, config->listen_explicit ?
        "hosting started on %s; /chat opens public chat" :
        "hosting stopped; outgoing connections unchanged", config->listen);
}

static int
send_operator_routed(struct app_state *app, const char *line, const char *text,
                     enum snag_irc_event_kind kind)
{
    struct snag_buf report;
    char error[256] = {0};
    int rc;
    bool show = app->ui.input_route.count > 1u;

    snag_buf_init(&report, 8192u);
    rc = snag_irc_send_route(app->irc, &app->ui.input_route, false, kind,
                              text, &report, error, sizeof(error));
    for (size_t i = 0u; i < app->irc_destinations.count; ++i)
        if (app->irc_destinations.items[i].target.id == app->ui.selection.id &&
            !app->irc_destinations.items[i].joined)
            show = true;
    if (rc != 0 || show) {
        if (snag_buf_terminate(&report) < 0 ||
            snag_ui_text(&app->ui, rc == 0 ? SNAG_UI_HOST : SNAG_UI_WARNING,
                report.len > 1u ? (const char *)report.data : error) < 0)
            rc = -1;
    }
    snag_buf_free(&report);
    if (rc == 1)
        return snag_ui_restore_draft(&app->ui, line);
    return rc < 0 ? -1 : 0;
}

static int
handle_destination_command(struct app_state *app, const char *line, bool *handled)
{
    uint32_t id;
    size_t body;
    enum snag_irc_target_command command = snag_irc_target_parse(line, strlen(line), &id, &body);

    *handled = command != SNAG_IRC_TARGET_NONE;
    if (command == SNAG_IRC_TARGET_NONE)
        return 0;
    if (command == SNAG_IRC_TARGET_INVALID) {
        if (app_error(app, "use /N to select, /N TEXT to send once, or /all TEXT") < 0)
            return -1;
        return snag_ui_restore_draft(&app->ui, line);
    }
    if (command == SNAG_IRC_TARGET_SELECT)
        return snag_ui_select_destination(&app->ui, id) < 0 ?
            app_error(app, "destination unavailable; use /names") : 0;
    if (command == SNAG_IRC_TARGET_SEND && !app->ui.input_route.count) {
        char error[96u];
        (void)snprintf(error, sizeof(error), "destination %u is unavailable; use /names", id);
        if (app_error(app, error) < 0)
            return -1;
        return snag_ui_restore_draft(&app->ui, line);
    }
    return send_operator_routed(app, line, line + body, SNAG_IRC_MESSAGE);
}

static int
handle_common_command(struct app_state *app, const char *line, bool active,
                      bool *handled, bool *prompt_ready)
{
    char error[256] = {0};

    *handled = true;
    *prompt_ready = false;
    {
        int rc = network_command(app, line, handled);

        if (*handled)
            return rc;
        *handled = true;
    }
    if (strcmp(line, "/help") == 0 || strcmp(line, "/?") == 0)
        return render_help(app);
    if (strcmp(line, "/status") == 0)
        return render_status(app);
    if (strcmp(line, "/history") == 0)
        return snag_ui_history(&app->ui, &app->session);
    if (strcmp(line, "/chat") == 0) {
        int rc = select_view(app, SNAG_RENDER_CHAT, active);

        *prompt_ready = rc == 0;
        if (rc == 0 && !app->networked)
            rc = app_hostf(app, "chat is offline; use /connect or /server start");
        return rc;
    }
    if (strcmp(line, "/rollout") == 0) {
        int rc = select_view(app, SNAG_RENDER_ROLLOUT, active);

        *prompt_ready = rc == 0;
        return rc;
    }
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
        return snag_app_goal_command(app, line, active);
    if (strcmp(line, "/names") == 0 || strcmp(line, "/topic") == 0) {
        struct snag_buf state;
        int rc;

        snag_buf_init(&state, SNAG_MAX_IRC_SNAPSHOT);
        rc = snag_buf_printf(&state, "selected destination: %u\n", app->ui.selection.id);
        if (rc == 0)
            rc = snag_irc_state(app->irc, &state, error, sizeof(error));
        if (rc == 0)
            rc = snag_buf_terminate(&state);
        if (rc == 0)
            rc = snag_ui_text(&app->ui, SNAG_UI_HOST, (const char *)state.data);
        snag_buf_free(&state);
        return rc < 0 ? app_error(app, error[0] ? error :
                                  "IRC state could not be displayed") : 0;
    }
    if (strncmp(line, "/topic ", 7u) == 0) {
        return send_operator_routed(app, line, line + 7u, SNAG_IRC_TOPIC);
    }
    *handled = false;
    return 0;
}
int
snag_app_active_input_pump(void *opaque, unsigned int timeout_ms)
{
    struct app_state *app = opaque;
    enum snag_term_action action = SNAG_TERM_NONE;
    char *line = NULL;
    char error[256];
    int rc;
    if (capture_shutdown_signal(app) || app->interrupt_requested) {
        app->interrupt_requested = true;
        return 2;
    }
    if (app->steering_requested)
        return 1;
    bool busy = snag_tools_busy();
    if (snag_tools_service(0, snag_ui_wake_fd(&app->ui), error, sizeof(error)) < 0)
        return -1;
    for (size_t i = 0u; i < app->session.process_count; ++i)
        snag_tools_process_state(&app->session.processes[i]);
    if (busy != snag_tools_busy() &&
        snag_ui_spinner_states(&app->ui, prompt_spinner_states(app, app->ui.active)) < 0)
        return -1;
    if (app->networked) {
        error[0] = '\0';
        if (tick_irc(app, error, sizeof(error)) < 0) {
            (void)snag_ui_text(&app->ui, SNAG_UI_ERROR,
                error[0] ? error : "IRC event loop failed");
            return -1;
        }
        if (timeout_ms > 25u)
            timeout_ms = 25u;
    }
    if (app->execute || app->input_closed)
        return 0;
    rc = snag_ui_poll(&app->ui, (int)timeout_ms, true, &action, &line);
    history_warning(app);
    if (rc < 0) {
        int input_errno = errno;
        if (capture_shutdown_signal(app)) {
            app->interrupt_requested = true;
            free(line);
            return 2;
        }
        (void)snag_ui_text(&app->ui, SNAG_UI_ERROR,
            errno == EOVERFLOW ? "active submission exceeds 1 MiB" :
            errno == EILSEQ ? "active submission contains invalid UTF-8" :
            "active input could not be read");
        return input_errno == EOVERFLOW || input_errno == EILSEQ ? 0 : -1;
    }
    if (rc == 0) {
        if (app->networked) {
            error[0] = '\0';
            if (tick_irc(app, error, sizeof(error)) < 0) {
                (void)snag_ui_text(&app->ui, SNAG_UI_ERROR,
                    error[0] ? error : "IRC event loop failed");
                return -1;
            }
        }
        return 0;
    }
    if (action == SNAG_TERM_EXIT) {
        app->input_closed = true;
        app->interrupt_requested = true;
        free(line);
        return 2;
    }
    if ((action == SNAG_TERM_CANCEL || action == SNAG_TERM_INTERRUPT) &&
        app->queue_edit_id[0]) {
        app->queue_armed = app->queue_edit_was_armed;
        app->queue_edit_id[0] = '\0';
        app->queue_edit_number = 0u;
        app->queue_edit_was_armed = false;
        return set_input_prompt(app, true);
    }
    if (action == SNAG_TERM_CANCEL) {
        return set_input_prompt(app, true);
    }
    if (action == SNAG_TERM_INTERRUPT) {
        app->interrupt_requested = true;
        free(line);
        return set_input_prompt(app, true) < 0 ? -1 : 2;
    }
    if (action == SNAG_TERM_VIEW) {
        if (app->queue_edit_id[0]) {
            (void)snag_ui_text(&app->ui, SNAG_UI_ERROR,
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
            (void)snag_ui_text(&app->ui, SNAG_UI_ERROR, error);
    } else if (action == SNAG_TERM_QUEUE) {
        rc = queue_future_turn(app, line, true, error, sizeof(error));
        if (rc != 0) {
            (void)snag_ui_text(&app->ui, SNAG_UI_ERROR, error);
            if (set_input_prompt(app, true) < 0 ||
                snag_ui_restore_draft(&app->ui, line) < 0)
                rc = -1;
        } else rc = set_input_prompt(app, true);
    } else {
        bool single_line = strchr(line, '\n') == NULL;
        bool handled = false;
        bool prompt_ready = false;
        bool read_only;

        (void)snag_prompt_parse(line, &read_only);
        if (read_only) {
            (void)snag_ui_text(&app->ui, SNAG_UI_ERROR,
                "/ro cannot steer an active turn; press Tab or use /queue /ro QUERY");
            rc = set_input_prompt(app, true);
            if (rc == 0)
                rc = snag_ui_restore_draft(&app->ui, line);
            goto active_done;
        }
        rc = handle_destination_command(app, line, &handled);
        if (rc < 0)
            goto active_done;
        if (!handled && single_line && line[0] == '/' && line[1] != '/') {
            rc = handle_common_command(app, line, true, &handled,
                                       &prompt_ready);
            if (rc < 0)
                goto active_done;
        }
        if (!handled) {
            rc = handle_queue_command(app, line, true, &handled,
                                      error, sizeof(error));
            if (rc != 0 && error[0])
                (void)snag_ui_text(&app->ui, SNAG_UI_ERROR, error);
            if (rc < 0)
                goto active_done;
        }
        if (handled) {
            if (!app->queue_edit_id[0] && !prompt_ready &&
                set_input_prompt(app, true) < 0)
                rc = -1;
        } else if (single_line && line[0] == '/' && line[1] != '/') {
            (void)snag_ui_text(&app->ui, SNAG_UI_ERROR,
                "that command is unavailable while a turn is active");
            rc = set_input_prompt(app, true);
        } else {
            const char *text = line[0] == '/' && line[1] == '/' ? line + 1 : line;
            char steering_id[SNAG_ID_HEX_LEN + 1u];
            size_t len = strlen(text);
            if (!len || len > SNAG_MAX_STEERING_TEXT) {
                (void)snag_ui_text(&app->ui, SNAG_UI_ERROR,
                    "active-turn input must be nonempty valid UTF-8 within 256 KiB");
                rc = 0;
            } else if (app->ui.input_view == SNAG_RENDER_CHAT) {
                error[0] = '\0';
                rc = send_operator_routed(app, line, text, SNAG_IRC_MESSAGE);
                if (rc < 0) {
                    (void)snag_ui_text(&app->ui, SNAG_UI_ERROR,
                        error[0] ? error : "IRC message could not be queued");
                    rc = set_input_prompt(app, true);
                    if (rc == 0)
                        rc = snag_ui_restore_draft(&app->ui, line);
                } else rc = set_input_prompt(app, true);
            } else if (snag_random_id(steering_id) < 0) {
                rc = -1;
            } else {
                rc = commit_event(app, "steering_added",
                        snag_app_steering_added_data(app->session.active_turn_id,
                                            steering_id, text),
                        error, sizeof(error));
                if (rc == 0 && snag_ui_submitted(&app->ui,
                        app->ui.label, text, true) < 0)
                    rc = -1;
                if (rc < 0) {
                    (void)snag_ui_text(&app->ui, SNAG_UI_ERROR, error[0] ? error :
                                               "active-turn input could not be persisted");
                    if (set_input_prompt(app, true) == 0)
                        (void)snag_ui_restore_draft(&app->ui, line);
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
    json_t *data = snag_app_tool_finished_data(turn_id, call_id, result);
    if (!data) {
        snag_errorf(error, error_size, "cannot allocate tool completion event");
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
        struct snag_pending_call *call = &app->session.pending_calls[i];
        json_t *result;
        if (call->finished)
            continue;
        result = call->started ? snag_tool_result_outcome_unknown("owner_lost") :
                                snag_tool_result_not_run(unstarted_reason);
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
    snag_tools_close_all(user_interrupt);
    while (app->session.process_count) {
        char handle[SNAG_ID_HEX_LEN + 1u];
        json_t *result = NULL;
        memcpy(handle, app->session.processes[0].handle, sizeof(handle));
#ifdef SNAJPAGENT_TEST_FIXTURE
        result = snag_tool_result_outcome_unknown("owner_lost");
#else
        if (snag_tools_close_managed(handle, user_interrupt, snag_app_active_input_pump,
                                    app, snag_ui_wake_fd(&app->ui), &result,
                                    error, error_size) < 0)
            return -1;
#endif
        if (commit_event(app, "process_closed",
                snag_app_process_closed_data(turn_id, handle, cause, result),
                error, error_size) < 0)
            return -1;
        snag_tools_collected(handle);
    }
    return 0;
}
static int
fail_turn(struct app_state *app, const char *turn_id, const char *cause,
          const char *class_name, const char *message,
          char *error, size_t error_size)
{
    return close_active_process_for_turn(app, turn_id, cause, false,
                                         error, error_size) < 0 ||
           commit_event(app, "turn_failed",
                        snag_app_turn_failed_data(turn_id, class_name, message),
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
                        snag_app_turn_interrupted_data(turn_id, origin, reason),
                        error, error_size) < 0 ? -1 : 0;
}

static int
fail_response(struct app_state *app, const char *turn_id,
              const char *response_id, unsigned int cycle,
              const char *class_name, const char *message, json_t *partial,
              unsigned int retry_count, const char *cause,
              char *error, size_t error_size)
{
    json_t *data = snag_app_response_failed_data(
        turn_id, response_id, cycle, class_name, message, partial, retry_count);

    if (!data) {
        snag_errorf(error, error_size, "cannot allocate response failure event");
        return -1;
    }
    return commit_event(app, "response_failed", data, error, error_size) < 0 ||
           fail_turn(app, turn_id, cause, class_name, message,
                     error, error_size) < 0 ? -1 : 0;
}

static int
recover_session(struct app_state *app, char *error, size_t error_size)
{
    char turn_id[SNAG_ID_HEX_LEN + 1u];
    const char *message;
    bool has_steering;
    if (!app->session.active_turn)
        return 0;
    memcpy(turn_id, app->session.active_turn_id, sizeof(turn_id));
    has_steering = app->session.pending_steering_count != 0u;
    if (app->session.response_open) {
        if (commit_event(app, "response_interrupted",
                         snag_app_response_interrupted_data(turn_id,
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
        if (app->session.response_outcome == SNAG_GRAPH_CONFLICT) {
            message = "provider response had conflicting terminal actions";
            if (terminalize_pending(app, turn_id, "protocol_conflict",
                                    error, error_size) < 0 ||
                fail_turn(app, turn_id, "protocol_failure",
                          "protocol", message, error, error_size) < 0)
                return -1;
            return app_warning(app, "recovered a protocol-conflicted turn");
        }
        if (has_steering) {
            if (app->session.response_outcome == SNAG_GRAPH_CALLS &&
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
        case SNAG_GRAPH_FINAL:
        case SNAG_GRAPH_REFUSAL:
            if (app->session.process_count) {
                message = "recovered terminal response while a managed process was unresolved";
                if (fail_turn(app, turn_id, "protocol_failure",
                              "protocol", message, error, error_size) < 0)
                    return -1;
                return app_warning(app, "recovered a terminal response that violated managed process ordering");
            }
            if (commit_event(app, "turn_completed",
                             snag_app_turn_completed_data(turn_id,
                                                 app->session.final_response_id,
                                                 app->session.final_item_id),
                             error, error_size) < 0)
                return -1;
            return app_warning(app, "recovered a durably completed turn");
        case SNAG_GRAPH_CALLS:
            if (terminalize_pending(app, turn_id, "recovery_unstarted",
                                    error, error_size) < 0 ||
                interrupt_turn(app, turn_id, "internal_failure", false,
                               "recovery", "session_recovered",
                               error, error_size) < 0)
                return -1;
            return app_warning(app, "recovered a turn with unfinished tool work");
        case SNAG_GRAPH_CONFLICT:
            break;
        case SNAG_GRAPH_NONPRODUCTIVE:
            message = "provider completed without a final answer, refusal, or tool call";
            if (fail_turn(app, turn_id, "protocol_failure",
                          "protocol", message, error, error_size) < 0)
                return -1;
            return app_warning(app, "recovered a nonproductive response");
        }
    }
    if (app->session.response_terminal == SNAG_RESPONSE_TERMINAL_FAILED) {
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
finish_call(struct app_state *app, const char *turn_id,
             const struct snag_response_item *call, const char *handle,
             json_t *result, char *error, size_t error_size)
{
    if (!result || snag_tools_attach_output_limit(call, app->config, result) < 0) {
        json_decref(result);
        return -1;
    }
    struct snag_process_state *process = snag_session_process(&app->session, handle);
    json_t *ref = json_object_get(result, "output_ref");
    if (ref && process &&
        (snag_json_set_new(ref, "log_start", json_integer((json_int_t)process->log_offset)) < 0 ||
         snag_json_set_new(ref, "log_end", json_integer((json_int_t)app->session.log_end)) < 0)) {
        json_decref(result);
        return -1;
    }
    if (commit_pending_result(app, turn_id, call->call_id, result, error, error_size) < 0)
        return -1;
    if (handle && *handle) {
        snag_tools_collected(handle);
        struct snag_process_state *p = snag_session_process(&app->session, handle);
        if (p) {
            p->log_offset = (uint64_t)app->session.log_end;
            p->log_seq = app->session.next_seq;
            memcpy(p->log_hash, app->session.prev_sha256, sizeof(p->log_hash));
            snag_tools_process_state(p);
        }
    }
    return 0;
}

static int
execute_calls(struct app_state *app, const char *turn_id,
              const struct snag_response_graph *graph,
              const struct snag_credential *credential,
              char *error, size_t error_size)
{
    struct {
        const struct snag_response_item *call;
        char handle[SNAG_ID_HEX_LEN + 1u];
        bool started, finished, process;
    } calls[SNAG_MAX_CALLS_PER_RESPONSE] = {0};
    size_t count = 0u, finished = 0u;
    uint64_t began = snag_monotonic_ms(), deadline = UINT64_MAX;
    const char *handoff = NULL;
    int control = 0;
    bool first_wave = true;

    for (size_t i = 0u; i < graph->count; ++i) {
        const struct snag_response_item *call = &graph->items[i];
        if (call->kind != SNAG_ITEM_TOOL_CALL)
            continue;
        calls[count].call = call;
#ifndef SNAJPAGENT_TEST_FIXTURE
        calls[count].process = !app->session.active_read_only &&
            (!strcmp(call->name, "exec_command") || !strcmp(call->name, "write_stdin"));
#endif
        ++count;
    }
    while (finished < count) {
        size_t before = finished;
        bool pending = false;
        for (size_t i = 0u; i < count; ++i) {
            const struct snag_response_item *call = calls[i].call;
            json_t *result = NULL;
            if (calls[i].finished)
                continue;
            control = snag_app_active_input_pump(app, 0u);
            if (control < 0)
                return -1;
            if (control == 2 || app->interrupt_requested) {
                handoff = "turn_cancelled";
                goto handoff;
            }
            if (control == 1 || app->irc_urgent.len) {
                if (snag_app_irc_flush_urgent(app, error, error_size) < 0)
                    return -1;
                handoff = "steering_handoff";
                goto handoff;
            }
            if (calls[i].started) {
                if (snag_tools_ready(calls[i].handle)) {
                    if (snag_tools_collect(calls[i].handle, NULL, &result, error, error_size) < 0 ||
                        finish_call(app, turn_id, call, calls[i].handle, result, error, error_size) < 0)
                        return -1;
                    calls[i].finished = true;
                    ++finished;
                } else if (snag_tools_handoff(calls[i].handle)) {
                    handoff = "batch_yield";
                    goto handoff;
                } else {
                    pending = true;
                }
                continue;
            }
            if (!first_wave && snag_monotonic_ms() >= deadline) {
                handoff = "batch_yield";
                goto handoff;
            }
            if (app->session.active_read_only && !snag_read_only_tool(call->name)) {
                result = snag_tool_result("not_run", "read_only",
                    "Tool unavailable: this turn is read-only.", -1, 0u);
                if (!result)
                    return -1;
            } else if (calls[i].process) {
                uint32_t yield_ms = 0u;
                int rc = snag_tools_prepare(call, app->config, calls[i].handle, &yield_ms, &result);
                if (rc < 0)
                    return -1;
                if (yield_ms && began + yield_ms < deadline)
                    deadline = began + yield_ms;
                if (rc > 0) {
                    const char *reason = snag_json_string(result, "reason");
                    if (reason && !strcmp(reason, "process_limit")) {
                        json_decref(result);
                        continue; /* A later poll may free a slot in this wave. */
                    }
                }
            } else if (!strcmp(call->name, "write_stdin")) {
                const char *handle = snag_json_string(call->arguments, "handle");
                if (!snag_session_process(&app->session, handle))
                    result = snag_tool_result_not_run("managed_process_handle_mismatch");
                else
                    memcpy(calls[i].handle, handle, sizeof(calls[i].handle));
            }
            if (!result && !strcmp(call->name, "write_stdin")) {
                for (size_t j = 0u; j < i; ++j)
                    if (calls[j].started && !strcmp(calls[j].handle, calls[i].handle)) {
                        result = snag_tool_result_not_run("process_busy");
                        break;
                    }
            }
            if (result) {
                if (finish_call(app, turn_id, call, NULL, result, error, error_size) < 0)
                    return -1;
                calls[i].finished = true;
                ++finished;
                continue;
            }
            char digest[SNAG_SHA256_HEX_LEN + 1u];
            if (snag_tool_action_digest(call, app->session.workspace, digest) < 0 ||
                commit_event(app, "tool_started",
                    snag_app_tool_started_data(turn_id, call->call_id, digest, app->session.workspace),
                    error, error_size) < 0)
                return -1;
            calls[i].started = true;
            if (!strcmp(call->name, "exec_command")) {
                memcpy(calls[i].handle, call->call_id, sizeof(calls[i].handle));
                struct snag_process_state *p = snag_session_process(&app->session, call->call_id);
                if (p) {
                    p->log_offset = (uint64_t)app->session.log_end;
                    p->log_seq = app->session.next_seq;
                    memcpy(p->log_hash, app->session.prev_sha256, sizeof(p->log_hash));
                }
            }
            app->tool_active = true;
            if (snag_ui_spinner_states(&app->ui, prompt_spinner_states(app, true)) < 0)
                return -1;
            int rc = calls[i].process ?
                snag_tools_start(call, app->config, credential, &result, error, error_size) :
                snag_app_tool_run(app, call, credential, &result, error, error_size);
            app->tool_active = false;
            if (rc < 0) {
                json_decref(result);
                result = snag_tool_result_terminal(false, error[0] ? error : "Tool adapter failed.");
            }
            if (rc == 2 || app->interrupt_requested) {
                if (result && finish_call(app, turn_id, call, calls[i].handle, result,
                                           error, error_size) < 0)
                    return -1;
                if (result) {
                    calls[i].finished = true;
                    ++finished;
                }
                handoff = "turn_cancelled";
                goto handoff;
            }
            if (result) {
                if (finish_call(app, turn_id, call, calls[i].handle, result, error, error_size) < 0)
                    return -1;
                calls[i].finished = true;
                ++finished;
            } else {
                pending = true;
            }
            if (snag_ui_spinner_states(&app->ui, prompt_spinner_states(app, true)) < 0)
                return -1;
        }
        first_wave = false;
        if (finished == count)
            break;
        if (!pending) {
            if (finished != before)
                continue;
            /* Only capacity-blocked calls remain; no invocation here can free it. */
            handoff = "process_limit";
            goto handoff;
        }
        if (snag_monotonic_ms() >= deadline) {
            handoff = "batch_yield";
            goto handoff;
        }
        if (snag_tools_service(10, snag_ui_wake_fd(&app->ui), error, error_size) < 0)
            return -1;
    }
    return 0;

handoff:
    if (!strcmp(handoff, "turn_cancelled"))
        snag_tools_close_all(true);
    for (size_t i = 0u; i < count; ++i) {
        json_t *result = NULL;
        if (calls[i].finished)
            continue;
        if (calls[i].started && calls[i].process) {
            if (!strcmp(handoff, "turn_cancelled"))
                while (!snag_tools_ready(calls[i].handle))
                    if (snag_tools_service(10, snag_ui_wake_fd(&app->ui), error, error_size) < 0)
                        return -1;
            if (snag_tools_collect(calls[i].handle, handoff, &result, error, error_size) < 0)
                return -1;
        } else if (calls[i].started) {
            result = snag_tool_result_outcome_unknown("owner_lost");
        } else {
            result = snag_tool_result_not_run(!strcmp(handoff, "steering_handoff") ?
                                              "superseded_by_steering" : handoff);
        }
        if (finish_call(app, turn_id, calls[i].call, calls[i].started ? calls[i].handle : NULL,
                         result, error, error_size) < 0)
            return -1;
    }
    if (!strcmp(handoff, "turn_cancelled")) {
        app->interrupt_requested = true;
        if (interrupt_turn(app, turn_id, "user_interrupt", true, "user", "cancelled",
                            error, error_size) < 0)
            return -1;
        return 2;
    }
    return 0;
}

static json_t *
silent_turn_data(const char *turn_id, const char *response_id,
                 const char *reason)
{
    return json_pack("{s:s,s:s,s:s}", "reason", reason,
        "response_id", response_id, "turn_id", turn_id);
}

static int
run_turn(struct app_state *app, const char *prompt,
         const struct snag_queued_turn *queued, bool goal_turn, bool read_only)
{
    char turn_id[SNAG_ID_HEX_LEN + 1u];
    char response_id[SNAG_ID_HEX_LEN + 1u];
    char error[256];
    char provider_source_hash[SNAG_SHA256_HEX_LEN + 1u];
    char rejected_request_hash[SNAG_SHA256_HEX_LEN + 1u] = {0};
    char over_budget_request_hash[SNAG_SHA256_HEX_LEN + 1u] = {0};
    unsigned int hard_compaction_attempts = 0u;
    bool capacity_recovery_used = false;
    char *turn_prompt;
    struct snag_credential credential;
    struct snag_response_graph graph;
    json_t *steering = NULL;
    struct snag_context_projection projection = {0};
    struct snag_buf request_body = {0};
    size_t prompt_max = queued ? SNAG_MAX_QUEUED_TEXT : SNAG_MAX_DIRECT_PROMPT;
    int result = 4;
    snag_credential_clear(&credential);
    app->last_turn_refused = false;
    error[0] = '\0';
    if (!*prompt || strlen(prompt) > prompt_max ||
        !snag_utf8_valid((const unsigned char *)prompt, strlen(prompt), true)) {
        (void)app_error(app, queued ?
            "queued prompt must be nonempty valid UTF-8 within 256 KiB" :
            "prompt must be nonempty valid UTF-8 within 1 MiB");
        return 2;
    }
    if (read_only && app->networked && !app->execute &&
        select_view(app, SNAG_RENDER_ROLLOUT, false) < 0)
        return 6;
    if (prepare_turn_settings(app, error, sizeof(error)) < 0) {
        (void)app_error(app, error);
        return 2;
    }
    provider_capacity_source_sha256(app->turn_provider, app->turn_model, provider_source_hash);
#ifndef SNAJPAGENT_TEST_FIXTURE
    if (snag_auth_read(app->store.root_fd, app->turn_provider, false, NULL,
                      &credential, snag_app_active_input_pump, app,
                      error, sizeof(error)) < 0) {
        (void)app_error(app, error);
        snag_credential_clear(&credential);
        return 2;
    }
#endif
    turn_prompt = snag_strdup_checked(prompt, prompt_max);
    if (!turn_prompt) {
        (void)app_error(app, "cannot retain turn input");
        snag_credential_clear(&credential);
        return 3;
    }
    snag_response_graph_init(&graph);
    if (app->config->read_agents_md) {
        if (snag_instructions_discover(&app->turn_instructions,
                                      app->session.workspace,
                                      error, sizeof(error)) < 0) {
            (void)app_error(app, error);
            result = 3;
            goto out;
        }
    } else {
        snag_instructions_free(&app->turn_instructions);
    }
    if (snag_random_id(turn_id) < 0) {
        (void)app_error(app, "cryptographic turn id generation failed");
        result = 3;
        goto out;
    }
    if (commit_event(app, "turn_started",
                     snag_app_turn_started_data(app, turn_prompt, turn_id, queued,
                                               goal_turn, read_only),
                     error, sizeof(error)) < 0) {
        (void)app_error(app, error);
        result = 3;
        goto out;
    }
    consume_staged_settings(app);
    if (app_runtimef(app,
            "turn › %s started%s · model=%s · effort=%s · workspace=%s",
            turn_id, read_only ? " (read-only)" : "", app->turn_model,
            app->turn_effort, app->session.workspace) < 0) {
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
        struct snag_graph_decision decision;
        const char *count_method = "qualified_upper_bound";
        uint64_t response_begin_ms;
        unsigned int provider_retry_count = 0u;
        struct snag_provider_failure provider_failure;
        int provider_rc;
        snag_app_response_cycle_release(app, &graph, &steering,
                                       &projection,
                                       &request_body);
        memset(&provider_failure, 0, sizeof(provider_failure));
        error[0] = '\0';
        if (snag_app_irc_flush_urgent(app, error, sizeof(error)) < 0) {
            (void)app_error(app, error[0] ? error :
                            "urgent IRC input could not be admitted");
            result = 3;
            goto out;
        }
        steering = snag_app_steering_snapshot(&app->session);
        error[0] = '\0';
        if (!steering || snag_random_id(response_id) < 0 ||
            snag_app_request_build(app, steering, cycle, &credential,
                                   provider_source_hash, &projection,
                                   &count_method, &request_body, error, sizeof(error)) < 0) {
            if (commit_event(app, "turn_failed",
                             snag_app_turn_failed_data(turn_id, "context",
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
        provider_rc = snag_app_provider_count(app, projection.count_request, &credential,
            projection.model_input_bytes, &projection.input_tokens_bound, &count_method,
            error, sizeof(error));
        if (provider_rc == 1 && app->steering_requested) {
            app->steering_requested = false;
            --cycle;
            continue;
        }
        if (provider_rc == 2 && app->interrupt_requested) {
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
        if (provider_rc != 0 && provider_rc != SNAG_APP_COUNT_SKIPPED) {
            if (commit_event(app, "turn_failed",
                             snag_app_turn_failed_data(turn_id, "provider",
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
        if (app->irc_urgent.len) {
            --cycle;
            continue;
        }
        {
            bool compacted = false;
            bool over_hard = app->turn_capacity.hard_input_known &&
                projection.input_tokens_bound > app->turn_capacity.hard_input_tokens;
            int compact_rc;

            if (over_hard &&
                (hard_compaction_attempts >= 8u ||
                 strcmp(over_budget_request_hash, projection.request_sha256) == 0)) {
                char failure[256];

                (void)snprintf(failure, sizeof(failure),
                    "%s while reducing context estimate %llu (%s) to hard budget %llu",
                    hard_compaction_attempts >= 8u ?
                        "context compaction reached its eight-attempt bound" :
                        "context compaction repeated an over-budget request",
                    (unsigned long long)projection.input_tokens_bound, count_method,
                    (unsigned long long)app->turn_capacity.hard_input_tokens);
                if (commit_event(app, "turn_failed",
                        snag_app_turn_failed_data(turn_id, "context", failure),
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
                memcpy(over_budget_request_hash, projection.request_sha256,
                       sizeof(over_budget_request_hash));
            compact_rc = snag_app_compact_before_response(app, &credential,
                    projection.input_tokens_bound, count_method, &compacted,
                    error, sizeof(error));
            if (compact_rc == 1 && app->steering_requested) {
                app->steering_requested = false;
                --cycle;
                continue;
            }
            if (compact_rc == 2 && app->interrupt_requested) {
                if (close_active_process_for_turn(app, turn_id,
                        "user_interrupt", true, error, sizeof(error)) < 0 ||
                    commit_event(app, "turn_interrupted",
                        snag_app_turn_interrupted_data(turn_id, "user",
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
                if (commit_event(app, "turn_failed",
                                 snag_app_turn_failed_data(turn_id,
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
                --cycle;
                continue;
            }
        }
        if (capacity_recovery_used &&
            strcmp(rejected_request_hash, projection.request_sha256) == 0) {
            static const char failure[] =
                "capacity recovery produced an identical provider request";
            if (commit_event(app, "turn_failed",
                    snag_app_turn_failed_data(turn_id, "context", failure),
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
                         snag_app_response_started_data(app, turn_id, response_id,
                             cycle, &projection, count_method, provider_source_hash,
                             steering),
                         error, sizeof(error)) < 0) {
            (void)app_error(app, error[0] ? error :
                                   "response setup could not be persisted");
            result = 3;
            goto out;
        }
        if (!app->execute && set_input_prompt(app, true) < 0) {
            (void)app_error(app, "context meter could not be displayed");
            result = 6;
            goto out;
        }
        json_decref(projection.count_request);
        projection.count_request = NULL;
        if (app_runtimef(app,
                "response › %s started · turn=%s · cycle=%u · model=%s · profile=%s",
                response_id, turn_id, cycle, app->turn_model,
                SNAJPAGENT_PROFILE_ID) < 0) {
            (void)app_error(app, "response runtime facts could not be rendered");
            result = 6;
            goto out;
        }
        if (request_body.len &&
            snag_ui_protocol(&app->ui, "request.body",
                                (const char *)request_body.data,
                                request_body.len) < 0) {
            json_t *partial = json_array();
            static const char failure[] =
                "request diagnostics could not be rendered";
            if (!partial || fail_response(app, turn_id, response_id, cycle,
                                          "output", failure, partial, 0u,
                                          "output_failure", error,
                                          sizeof(error)) < 0) {
                (void)app_error(app, error[0] ? error :
                                "diagnostic output failure could not be persisted");
                result = 3;
                goto out;
            }
            (void)app_error(app, failure);
            result = 6;
            goto out;
        }
        snag_buf_free(&request_body);
        snag_app_reset_stream(app);
        response_begin_ms = snag_time_ms();
        error[0] = '\0';
        provider_rc = snag_app_provider_run(app, turn_prompt, steering, cycle,
                                   projection.create_request, &credential, &graph,
                                   &provider_failure,
                                   error, sizeof(error), &provider_retry_count);
        if (provider_rc == 0) {
            int control_rc = snag_app_active_input_pump(app, 0u);
            if (control_rc != 0)
                provider_rc = control_rc;
        }
        json_decref(projection.create_request);
        projection.create_request = NULL;
        json_decref(steering);
        steering = NULL;
        if ((provider_rc == 1 && app->steering_requested) ||
            (provider_rc == 2 && app->interrupt_requested) ||
            provider_failure.output_correction !=
                SNAG_OUTPUT_CORRECTION_NONE) {
            if (snag_app_abort_stream_item(app) < 0)
                app->stream_failed = true;
        } else if (snag_app_finish_stream_item(app) < 0) {
            app->stream_failed = true;
        }
        if (provider_rc == 1 && app->steering_requested && !app->stream_failed) {
            json_t *partial = snag_app_partial_public_json(app);
            if (!partial ||
                commit_event(app, "response_interrupted",
                    snag_app_response_interrupted_data(turn_id, response_id, cycle,
                                              "steering", "steered", partial),
                    error, sizeof(error)) < 0) {
                (void)app_error(app, error[0] ? error :
                                       "active-turn response could not be persisted");
                result = 3;
                goto out;
            }
            continue;
        }
        if (provider_rc == 2 && app->interrupt_requested && !app->stream_failed) {
            json_t *partial = snag_app_partial_public_json(app);
            if (!partial ||
                commit_event(app, "response_interrupted",
                    snag_app_response_interrupted_data(turn_id, response_id, cycle,
                                              "user", "cancelled", partial),
                    error, sizeof(error)) < 0 ||
                interrupt_turn(app, turn_id, "user_interrupt", true,
                               "user", "cancelled",
                               error, sizeof(error)) < 0) {
                (void)app_error(app, error[0] ? error :
                                "interruption could not be persisted");
                result = 3;
                goto out;
            }
            (void)app_warning(app, "turn interrupted");
            result = app->execute ? 6 : 1;
            goto out;
        }
        if (provider_failure.output_correction !=
                SNAG_OUTPUT_CORRECTION_NONE && !app->stream_failed) {
            static const char repeated[] =
                "assistant output remained invalid after one model-facing correction";
            const char *correction =
                provider_failure.output_correction ==
                    SNAG_OUTPUT_CORRECTION_EMPTY ?
                    SNAG_EMPTY_OUTPUT_CORRECTION :
                    SNAG_OVERSIZED_OUTPUT_CORRECTION;
            char correction_id[SNAG_ID_HEX_LEN + 1u];

            if (app->session.output_correction_used) {
                app->stream_failed = true;
                app->stream_errno = EPROTO;
                (void)snprintf(app->stream_error,
                               sizeof(app->stream_error), "%s", repeated);
            } else {
                json_t *partial;

                if (snag_random_id(correction_id) < 0)
                    partial = NULL;
                else
                    partial = snag_app_partial_public_json(app);
                if (!partial ||
                    commit_event(app, "response_output_correction",
                        snag_app_response_output_correction_data(
                            turn_id, response_id, cycle, correction_id,
                            correction, partial),
                        error, sizeof(error)) < 0) {
                    (void)app_error(app, error[0] ? error :
                        "assistant output correction could not be persisted");
                    result = 3;
                    goto out;
                }
                continue;
            }
        }
        if (provider_rc < 0 || app->stream_failed) {
            bool capacity_failure = provider_rc < 0 &&
                snag_provider_failure_is_capacity(&provider_failure);
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
                char provider_source_hash[SNAG_SHA256_HEX_LEN + 1u];

                provider_capacity_source_sha256(app->turn_provider, app->turn_model,
                                                provider_source_hash);

                if (strcmp(rejected_request_hash, projection.request_sha256) == 0) {
                    (void)snprintf(failure, sizeof(failure),
                                   "provider rejected an identical context request twice");
                } else if (commit_event(app, "response_capacity_rejected",
                        snag_app_response_capacity_rejected_data(
                            turn_id, response_id, cycle, projection.request_sha256,
                            &provider_failure, &app->turn_capacity,
                            provider_source_hash), error, sizeof(error)) < 0) {
                    (void)app_error(app, error[0] ? error :
                        "capacity rejection could not be persisted");
                    result = 3;
                    goto out;
                } else {
                    int recovery_rc;
                    bool ceiling_matches;

                    memcpy(rejected_request_hash, projection.request_sha256,
                           sizeof(rejected_request_hash));
                    ceiling_matches = capacity_ceiling_matches(
                        app, app->turn_provider, app->turn_model);
                    if (provider_failure.requested_input_tokens ||
                        ceiling_matches)
                        snag_app_record_model_accounting(app, SNAG_COUNT_UNKNOWN,
                            provider_failure.requested_input_tokens ?
                                projection.model_input_bytes : 0u,
                            provider_failure.requested_input_tokens ?
                                provider_failure.requested_input_tokens : 0u,
                            ceiling_matches ?
                                app->session.capacity_ceiling_input_tokens :
                                0u);
                    apply_capacity_ceiling(app, app->turn_provider,
                                           app->turn_model,
                                           &app->turn_capacity);
                    snag_app_response_cycle_release(app, &graph, &steering,
                                       &projection,
                                       &request_body);
                    for (;;) {
                        error[0] = '\0';
                        recovery_rc =
                            snag_app_compact_after_capacity_rejection(
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
                                    snag_app_turn_interrupted_data(
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
                            snag_app_turn_failed_data(turn_id, "context", failure),
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
            partial = snag_app_partial_public_json(app);
            if (!partial) {
                (void)app_error(app, "failed response prefix could not be retained");
                result = 3;
                goto out;
            }
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
            int input_rc = snag_app_active_input_pump(app, 0u);
            if (input_rc < 0) {
                (void)app_error(app, "active input could not be processed");
                result = 3;
                goto out;
            }
        }
        if (snag_response_graph_classify(&graph, &decision,
                                        error, sizeof(error)) < 0) {
            char failure[256];
            json_t *partial;
            (void)snprintf(failure, sizeof(failure), "%s",
                           error[0] ? error : "invalid provider response graph");
            partial = snag_app_partial_public_json(app);
            if (!partial) {
                (void)app_error(app, "invalid response prefix could not be retained");
                result = 3;
                goto out;
            }
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
                         snag_app_response_completed_data(turn_id, response_id, cycle, &graph),
                         error, sizeof(error)) < 0) {
            (void)app_error(app, error);
            result = 3;
            goto out;
        }
        if (graph.usage.input_known && graph.usage.input_tokens != 0u &&
            strcmp(count_method, "exact") != 0)
            snag_app_record_model_accounting(app, SNAG_COUNT_UNKNOWN,
                projection.model_input_bytes, graph.usage.input_tokens, 0u);
        error[0] = '\0';
        if (snag_app_irc_flush_urgent(app, error, sizeof(error)) < 0) {
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
                    (unsigned long long)(snag_time_ms() - response_begin_ms),
                    input_tokens, output_tokens, reasoning_tokens,
                    total_tokens) < 0) {
                (void)app_error(app, "response runtime facts could not be rendered");
                result = 6;
                goto out;
            }
        }
        if (decision.outcome == SNAG_GRAPH_CONFLICT) {
            const char *message = decision.message ? decision.message :
                "provider response contained conflicting actions";
            if (terminalize_pending(app, turn_id, "protocol_conflict",
                                    error, sizeof(error)) < 0 ||
                fail_turn(app, turn_id, "protocol_failure",
                          "protocol", message, error, sizeof(error)) < 0) {
                (void)app_error(app, error);
                result = 3;
                goto out;
            }
            (void)app_error(app, message);
            result = 4;
            goto out;
        }
        if (app->session.pending_steering_count != 0u) {
            if (decision.outcome == SNAG_GRAPH_CALLS &&
                terminalize_pending(app, turn_id, "superseded_by_steering",
                                    error, sizeof(error)) < 0) {
                (void)app_error(app, error);
                result = 3;
                goto out;
            }
            continue;
        }
        if (app->session.process_count && decision.outcome != SNAG_GRAPH_CALLS) {
            const char *message = "Unsettled commands remain; collect their terminal results before a final answer.";
            if (fail_turn(app, turn_id, "protocol_failure", "protocol", message,
                           error, sizeof(error)) < 0)
                (void)app_error(app, error);
            else
                (void)app_error(app, message);
            result = 4;
            goto out;
        }
        if (app->networked && !app->session.active_read_only &&
            app->irc_turn_replies.count && !app->session.irc_reply_reminded &&
            (decision.outcome == SNAG_GRAPH_NONPRODUCTIVE ||
             decision.outcome == SNAG_GRAPH_FINAL ||
             decision.outcome == SNAG_GRAPH_REFUSAL)) {
            char steering_id[SNAG_ID_HEX_LEN + 1u];

            if (snag_random_id(steering_id) < 0 ||
                commit_event(app, "irc_reply_reminder",
                    snag_app_steering_added_data(turn_id, steering_id,
                        SNAG_IRC_REPLY_REMINDER_TEXT),
                    error, sizeof(error)) < 0) {
                (void)app_error(app, error[0] ? error :
                                "IRC reply reminder could not be persisted");
                result = 3;
                goto out;
            }
            continue;
        }
        if (app->request_networked && decision.outcome == SNAG_GRAPH_NONPRODUCTIVE) {
            if (commit_event(app, "turn_completed_silent",
                    silent_turn_data(turn_id, response_id,
                        app->irc_turn_replies.count ?
                        "reply_reminder_exhausted" : "room_update_quiet"),
                    error, sizeof(error)) < 0) {
                (void)app_error(app, error[0] ? error :
                                "quiet IRC turn could not be completed");
                result = 3;
                goto out;
            }
            result = 0;
            goto out;
        }
        if (decision.outcome == SNAG_GRAPH_FINAL ||
            decision.outcome == SNAG_GRAPH_REFUSAL) {
            const struct snag_response_item *final = &graph.items[decision.final_index];
            if (commit_event(app, "turn_completed",
                             snag_app_turn_completed_data(turn_id, response_id,
                                                 final->local_item_id),
                             error, sizeof(error)) < 0) {
                (void)app_error(app, error);
                result = 3;
                goto out;
            }
            app->last_turn_refused = decision.outcome == SNAG_GRAPH_REFUSAL;
            if (app_runtimef(app,
                    "turn › %s completed · response=%s · item=%s",
                    turn_id, response_id, final->local_item_id) < 0) {
                (void)app_error(app, "turn runtime facts could not be rendered");
                result = 6;
                goto out;
            }
            if (app->execute &&
                snag_ui_raw(&app->ui, STDOUT_FILENO, final->text,
                               strlen(final->text)) < 0) {
                (void)app_error(app, "final answer could not be written to stdout");
                result = 6;
                goto out;
            }
            if (snag_app_compact_after_turn(app, projection.input_tokens_bound, count_method,
                                           error, sizeof(error)) < 0)
                (void)app_warning(app, error);
            result = 0;
            goto out;
        }
        if (decision.outcome == SNAG_GRAPH_CALLS) {
            int tool_rc;
            tool_rc = execute_calls(app, turn_id, &graph, &credential,
                                    error, sizeof(error));
            if (tool_rc < 0) {
                (void)app_error(app, error);
                /* An adapter/journal failure leaves effects uncertain. Stop
                 * admission and close every owned job before exiting. */
                app->input_closed = true;
                snag_tools_shutdown();
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
                (void)app_error(app, error);
                result = 3;
                goto out;
            }
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
    snag_app_response_cycle_release(app, &graph, &steering,
                                       &projection,
                                       &request_body);
    if (!app->execute && result != 6 &&
        set_input_prompt(app, false) < 0)
        result = 6;
    snag_credential_clear(&credential);
    snag_instructions_free(&app->turn_instructions);
    free(turn_prompt);
    return result;
}

static int
run_tracked_turn(struct app_state *app, const char *prompt,
                 const struct snag_queued_turn *queued, bool goal_turn,
                 bool read_only)
{
    char error[256] = {0};
    const char *reason = NULL;
    const char *message = NULL;
    int rc = run_turn(app, prompt, queued, goal_turn, read_only);

    app->irc_turn_replies.count = 0u;
    if (app->session.goal_status != SNAG_GOAL_ACTIVE)
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
    if (snag_app_goal_pause(app, reason, error, sizeof(error)) < 0) {
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
        snag_errorf(error, error_size, "cannot resolve %s workspace %s: %s",
                  label, path, strerror(errno));
        return NULL;
    }
    if (strlen(resolved) > SNAG_PATH_MAX_BYTES ||
        !snag_utf8_valid((const unsigned char *)resolved, strlen(resolved), true) ||
        stat(resolved, &st) < 0 || !S_ISDIR(st.st_mode)) {
        snag_errorf(error, error_size, "%s workspace must be an existing UTF-8 directory",
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
char *
snag_app_dotdir(const char *override, char *error, size_t error_size)
{
    const char *home = getenv("HOME");
    char *path;
    size_t len;

    if (override)
        path = snag_strdup_checked(override, SNAG_PATH_MAX_BYTES);
    else if (home && home[0] == '/') {
        size_t home_len = strlen(home);
        bool slash = home_len != 0u && home[home_len - 1u] == '/';
        const char *suffix = slash ? "." SNAJPAGENT_NAME :
                                     "/." SNAJPAGENT_NAME;
        size_t suffix_len = strlen(suffix);
        if (home_len > SNAG_PATH_MAX_BYTES - suffix_len) {
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
        snag_errorf(error, error_size,
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
        !snag_utf8_valid((const unsigned char *)path, len, true)) {
        snag_errorf(error, error_size,
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

        if (resolved && strlen(resolved) <= SNAG_PATH_MAX_BYTES &&
            snag_utf8_valid((const unsigned char *)resolved,
                           strlen(resolved), true))
            return resolved;
        free(resolved);
        return snag_strdup_checked(program, SNAG_PATH_MAX_BYTES);
    }
    path = getenv("PATH");
    if (path) {
        const char *start = path;

        for (;;) {
            const char *end = strchr(start, ':');
            size_t dir_len = end ? (size_t)(end - start) : strlen(start);
            const char *dir = dir_len ? start : ".";
            size_t actual_dir_len = dir_len ? dir_len : 1u;

            if (actual_dir_len <= SNAG_PATH_MAX_BYTES &&
                program_len <= SNAG_PATH_MAX_BYTES - actual_dir_len - 1u) {
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
                    if (resolved && strlen(resolved) <= SNAG_PATH_MAX_BYTES &&
                        snag_utf8_valid((const unsigned char *)resolved,
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
    return snag_strdup_checked(program, SNAG_PATH_MAX_BYTES);
}

static int
append_command_literal(struct snag_buf *command, const char *word)
{
    if (command->len && snag_buf_putc(command, ' ') < 0)
        return -1;
    return snag_buf_append(command, word, strlen(word));
}

static int
append_command_argument(struct snag_buf *command, const char *argument)
{
    const unsigned char *p = (const unsigned char *)argument;

    if (command->len && snag_buf_putc(command, ' ') < 0)
        return -1;
    if (snag_buf_putc(command, '\'') < 0)
        return -1;
    while (*p) {
        if (*p == '\'') {
            if (snag_buf_append(command, "'\\''", 4u) < 0)
                return -1;
        } else if (*p == '\n' || (*p >= 0x20u && *p <= 0x7eu)) {
            if (snag_buf_putc(command, *p) < 0)
                return -1;
        } else {
            if (snag_buf_putc(command, '\'') < 0 ||
                snag_buf_printf(command, "\"$(printf '\\%03o')\"",
                               (unsigned int)*p) < 0 ||
                snag_buf_putc(command, '\'') < 0)
                return -1;
        }
        ++p;
    }
    return snag_buf_putc(command, '\'');
}

static int
append_command_option(struct snag_buf *command, const char *option,
                      const char *argument)
{
    return append_command_literal(command, option) < 0 ||
           append_command_argument(command, argument) < 0 ? -1 : 0;
}

static int
build_resume_command(const struct app_state *app, const char *program,
                     const char *dotdir, struct snag_buf *command)
{
    char *resolved = resolved_program_path(program);
    const struct snag_cli *cli = app->cli;
    const struct snag_config *config = app->config;
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
    case SNAG_CLI_COLOR_UNSET: break;
    case SNAG_CLI_COLOR_AUTO:
        if (append_command_literal(command, "--color=auto") < 0)
            goto out;
        break;
    case SNAG_CLI_COLOR_ALWAYS:
        if (append_command_literal(command, "--color=always") < 0)
            goto out;
        break;
    case SNAG_CLI_COLOR_NEVER:
        if (append_command_literal(command, "--color=never") < 0)
            goto out;
        break;
    }
    if (cli->markdown == SNAG_CLI_MARKDOWN_ENABLED &&
        append_command_literal(command, "--markdown") < 0)
        goto out;
    if (cli->markdown == SNAG_CLI_MARKDOWN_DISABLED &&
        append_command_literal(command, "--no-markdown") < 0)
        goto out;
    for (unsigned int i = 0u; i < snag_ui_verbosity(&app->ui); ++i)
        if (append_command_literal(command, "-v") < 0)
            goto out;
    if (app->staged_model &&
        append_command_option(command, "-m", app->staged_model) < 0)
        goto out;
    if (app->staged_effort &&
        append_command_option(command, "--effort", app->staged_effort) < 0)
        goto out;
    if (config->irc.listen_explicit) {
        if (append_command_option(command, "--listen", config->irc.listen) < 0)
            goto out;
    } else if (append_command_literal(command, "--no-listen") < 0) {
        goto out;
    }
    if (!config->irc.client_count && append_command_literal(command, "--no-client") < 0)
        goto out;
    for (size_t i = 0u; i < config->irc.client_count; ++i)
        if (append_command_option(command, "--client", config->irc.clients[i]) < 0)
            goto out;
    if (config->irc.model_nick[0] && !config->irc.model_nick_implicit &&
        append_command_option(command, "--model-nick", config->irc.model_nick) < 0)
        goto out;
    if (config->irc.operator_nick[0] && !config->irc.operator_nick_implicit &&
        append_command_option(command, "--operator-nick", config->irc.operator_nick) < 0)
        goto out;
    if (config->irc.room_name[0] &&
        append_command_option(command, "--room-name", config->irc.room_name) < 0)
        goto out;
    if (append_command_literal(command, "--resume") < 0 ||
        append_command_argument(command, app->session.id) < 0)
        goto out;
    rc = 0;
out:
    free(resolved);
    return rc;
}

static void
write_resume_command(struct app_state *app, const char *program,
                     const char *dotdir)
{
    struct snag_buf command;

    if (!dotdir || !app->session.id[0] || app->session.delete_requested)
        return;
    snag_buf_init(&command, RESUME_COMMAND_MAX);
    if (build_resume_command(app, program, dotdir, &command) == 0)
        (void)snag_ui_resume_hint(&app->ui, (char *)command.data,
                                     command.len);
    snag_buf_free(&command);
}

static int
list_row(void *opaque, const char *text, size_t len)
{
    struct app_state *app = opaque;
    return snag_ui_raw(&app->ui, app->cli->list ? STDOUT_FILENO : STDERR_FILENO,
                      text, len);
}

static int
pick_session(struct app_state *app, const char *workspace,
             char *error, size_t error_size)
{
    const char *frames[SNAG_TERM_SPINNER_COUNT] = {" ", " ", " "};
    enum snag_term_action action;
    char *prefix = NULL;
    int rc = -1;

    if (snag_store_list(&app->store, workspace, app->cli->all, false,
                       list_row, app, error, error_size) < 0 ||
        snag_ui_open(&app->ui, error, error_size) < 0 ||
        snag_ui_prompt(&app->ui, false, "session › ", frames, 1u, 0u) < 0)
        return -1;
    do {
        rc = snag_ui_poll(&app->ui, -1, false, &action, &prefix);
    } while (rc == 0);
    if (rc < 0 ||
        action != SNAG_TERM_SUBMIT || !prefix) {
        snag_errorf(error, error_size, "session selection cancelled");
        rc = -1;
    } else if (strlen(prefix) < 8u || strlen(prefix) > SNAG_ID_HEX_LEN) {
        snag_errorf(error, error_size, "enter an 8..32 character session id prefix");
        rc = -1;
    } else {
        rc = snag_session_open(&app->store, &app->session, prefix,
                              error, error_size);
    }
    free(prefix);
    return rc;
}
static int
run_queued_chain(struct app_state *app)
{
    while (app->queue_armed && app->session.pending_queue_count != 0u) {
        const struct snag_queued_turn *queued = &app->session.pending_queue[0];
        int turn_rc = run_tracked_turn(app, queued->text, queued, false,
                                       queued->read_only);
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
        if (app->irc_urgent.len ||
             ((!app->queue_armed || app->session.pending_queue_count == 0u) &&
              app->goal_armed && app->irc_background.len)) {
            bool local_operator = false;
            char *prompt = snag_app_irc_take_pending(app, &local_operator,
                                                     true);
            if (prompt) {
                turn_rc = run_tracked_turn(app, prompt, NULL, false, false);
                free(prompt);
                if (turn_rc != 0)
                    return turn_rc;
                continue;
            }
        }
        if (app->queue_armed && !app->queue_edit_id[0] &&
            app->session.pending_queue_count != 0u) {
            turn_rc = run_queued_chain(app);
            if (turn_rc != 0)
                return turn_rc;
            continue;
        }
        if (app->goal_armed && app->session.pending_queue_count == 0u &&
            app->session.goal_status == SNAG_GOAL_ACTIVE) {
            turn_rc = run_tracked_turn(app, SNAG_GOAL_CONTINUATION_TEXT,
                                      NULL, true, false);
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
    if (initial && (snag_app_sync_destinations(app) < 0 ||
                    snag_ui_capture_route(&app->ui, initial) < 0))
        return 6;
    for (;;) {
        enum snag_term_action action = SNAG_TERM_NONE;
        bool prompt_ready = false;
        if (capture_shutdown_signal(app))
            return 0;
        if (app->input_closed)
            return 0;
        if (!prompt) {
            bool local_operator = false;
            char *irc_prompt = snag_app_irc_take_pending(
                app, &local_operator, false);

            if (irc_prompt) {
                int turn_rc;
                app->queue_armed = false;
                turn_rc = run_tracked_turn(app, irc_prompt, NULL, false, false);
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
            int poll_rc = snag_ui_poll(&app->ui,
                                        app->networked || app->irc_background.len ? 25 : -1,
                                        false, &action, &owned);
            history_warning(app);
            if (poll_rc < 0) {
                int input_errno = errno;
                if (capture_shutdown_signal(app)) {
                    free(owned);
                    return 0;
                }
                (void)app_error(app,
                    errno == EOVERFLOW ? "prompt exceeds 1 MiB" :
                    errno == EILSEQ ? "terminal input contains invalid UTF-8" :
                    "terminal input could not be read");
                if ((input_errno != EOVERFLOW && input_errno != EILSEQ) ||
                    set_input_prompt(app, false) < 0)
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
            if (action == SNAG_TERM_EXIT) {
                free(owned);
                return 0;
            }
            if (action == SNAG_TERM_CANCEL) {
                if (app->queue_edit_id[0]) {
                    app->queue_armed = app->queue_edit_was_armed;
                    app->queue_edit_id[0] = '\0';
                    app->queue_edit_number = 0u;
                    app->queue_edit_was_armed = false;
                }
                if (set_input_prompt(app, false) < 0)
                    return 6;
                continue;
            }
            if (action == SNAG_TERM_VIEW) {
                if (app->queue_edit_id[0])
                    (void)app_error(app, "queue replacement must be nonempty");
                else if (toggle_view(app) < 0)
                    return 6;
                continue;
            }
            if (action != SNAG_TERM_SUBMIT || !owned) {
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
        enum snag_render_view input_view = owned ? app->ui.input_view : app->ui.view;
        if (input_view == SNAG_RENDER_ROLLOUT) {
            if (snag_ui_submitted(&app->ui,
                    app->ui.label, prompt, true) < 0) {
                free(owned);
                return 6;
            }
        }
        {
            bool single_line = strchr(prompt, '\n') == NULL;
            bool read_only;
            const char *query = snag_prompt_parse(prompt, &read_only);
            bool handled = false;
            int local_rc = 0;
            local_rc = handle_destination_command(app, prompt, &handled);
            if (!handled && single_line && prompt[0] == '/' && prompt[1] != '/')
                local_rc = handle_common_command(app, prompt, false, &handled,
                                                 &prompt_ready);
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
                local_rc = snag_app_lifecycle_command(app, prompt, &handled, &exit_now);
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
            } else if (!read_only && single_line && prompt[0] == '/' && prompt[1] != '/') {
                (void)app_error(app, "unknown slash command");
            } else if (!read_only && input_view == SNAG_RENDER_CHAT) {
                const char *actual = prompt[0] == '/' && prompt[1] == '/' ?
                                     prompt + 1 : prompt;
                if (send_operator_routed(app, prompt, actual, SNAG_IRC_MESSAGE) < 0) {
                    free(owned);
                    return 3;
                }
            } else {
                const char *actual = query;
                int turn_rc;

                if (read_only && !*actual) {
                    (void)app_error(app, "usage: /ro QUERY (query must not be empty)");
                    free(owned);
                    owned = NULL;
                    prompt = NULL;
                    if (set_input_prompt(app, false) < 0)
                        return 6;
                    continue;
                }

                if (input_view == SNAG_RENDER_CHAT &&
                    snag_ui_submitted(&app->ui,
                        app->ui.label, actual, true) < 0) {
                    free(owned);
                    return 6;
                }
                app->queue_armed = false;
                turn_rc = run_tracked_turn(app, actual, NULL, false, read_only);
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
        if (!prompt_ready && set_input_prompt(app, false) < 0)
            return 6;
    }
}
static int
render_room_history(void *opaque, const struct snag_irc_event *event)
{
    return snag_ui_irc_event(opaque, event);
}

int
snag_app_run(const struct snag_cli *cli, const char *program)
{
    struct app_state app;
    struct app_signal_handlers signal_handlers;
    struct snag_config config;
    char error[256];
    char *dotdir = NULL;
    char *config_path = NULL;
    char *workspace = NULL;
    char *relocated_workspace = NULL;
    const char *new_model = NULL;
    const char *new_effort;
    bool goal_paused_on_resume = false;
    bool signal_handlers_installed = false;
    int rc = 3;
    memset(&app, 0, sizeof(app));
    snag_buf_init(&app.irc_urgent, SNAG_MAX_IRC_SNAPSHOT);
    snag_buf_init(&app.irc_background, SNAG_MAX_IRC_SNAPSHOT);
    snag_config_init(&config);
    snag_instructions_init(&app.turn_instructions);
    snag_model_cache_init(&app.model_cache);
    snag_store_init(&app.store);
    snag_session_init(&app.session);
    if (snag_ui_init(&app.ui) < 0)
        return 3;
    atomic_store(&shutdown_ui, &app.ui);
    snag_ui_color(&app.ui, snag_cli_color(cli, SNAG_COLOR_AUTO));
    app.cli = cli;
    app.config = &config;
    snag_tools_journal(snag_app_tool_output, snag_app_tool_read, &app);
    app.config_allow_create = cli->config_path == NULL;
    app.execute = cli->execute;
    if (install_shutdown_handlers(&signal_handlers) < 0) {
        (void)snag_ui_text(&app.ui, SNAG_UI_ERROR,
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
            (void)snag_ui_text(&app.ui, SNAG_UI_ERROR,
                                       "a UTF-8 locale is required");
            rc = 2;
            goto out;
        }
    }
    error[0] = '\0';
    dotdir = snag_app_dotdir(cli->dotdir, error, sizeof(error));
    if (!dotdir) {
        (void)snag_ui_text(&app.ui, SNAG_UI_ERROR,
                                   error[0] ? error : "dotdir is unavailable");
        rc = 2;
        goto out;
    }
    if (snag_config_load(&config, cli->config_path, dotdir,
                        error, sizeof(error)) < 0) {
        (void)snag_ui_text(&app.ui, SNAG_UI_ERROR, error);
        rc = 2;
        goto out;
    }
    if (!cli->list && ((!config.provider_count) ||
        (cli->provider && !snag_config_provider(&config, cli->provider)))) {
        (void)snag_ui_text(&app.ui, SNAG_UI_ERROR, cli->provider ?
            "--provider names an unconfigured provider" :
            "no provider is configured; run snajpagent login");
        rc = 2;
        goto out;
    }
    config_path = snag_config_path(cli->config_path, dotdir,
                                  error, sizeof(error));
    if (!config_path) {
        (void)snag_ui_text(&app.ui, SNAG_UI_ERROR, error);
        rc = 2;
        goto out;
    }
    app.config_path = config_path;
    app.irc_file_config = config.irc;
    snag_ui_color(&app.ui, snag_cli_color(cli, config.color));
    snag_ui_markdown(&app.ui, snag_cli_markdown(cli, config.markdown));
    if (!cli->execute && !cli->list &&
        snag_irc_apply_cli(&config, cli, error, sizeof(error)) < 0) {
        (void)snag_ui_text(&app.ui, SNAG_UI_ERROR, error);
        rc = 2;
        goto out;
    }
    app.networked = !cli->execute && !cli->list && snag_irc_enabled(&config);
    snag_ui_commands(&app.ui, commands, command_count());
    snag_ui_model_nick(&app.ui,
                             app.networked ? config.irc.model_nick : NULL);
    if (app.networked && snag_ui_set_view(&app.ui, SNAG_RENDER_CHAT) < 0)
        goto out;
    snag_ui_typing_pause(&app.ui, config.typing_pause_ms);
    if (snag_ui_set_verbosity(&app.ui, cli->verbosity) < 0)
        goto out;
    if (!cli->execute && !cli->list &&
        (isatty(STDIN_FILENO) != 1 || isatty(STDERR_FILENO) != 1)) {
        (void)snag_ui_text(&app.ui, SNAG_UI_ERROR,
            "interactive mode requires terminal stdin and stderr; use -e for scripts");
        rc = 2;
        goto out;
    }
    if (!cli->resume || cli->model)
        new_model = effective_model(cli->model ? cli->model : config.model);
    new_effort = cli->effort ? cli->effort : config.reasoning_effort;
    if ((!cli->resume || cli->effort) && !resolve_effort(new_effort)) {
        (void)snag_ui_text(&app.ui, SNAG_UI_ERROR,
            "reasoning effort is empty, oversized, or invalid UTF-8");
        rc = 2;
        goto out;
    }
    if (snag_store_open(&app.store, dotdir, error, sizeof(error)) < 0) {
        (void)snag_ui_text(&app.ui, SNAG_UI_ERROR, error);
        goto out;
    }
    workspace = current_workspace(error, sizeof(error));
    if (!workspace) {
        (void)snag_ui_text(&app.ui, SNAG_UI_ERROR, error);
        goto out;
    }
    if (cli->list) {
        rc = snag_store_list(&app.store, workspace, cli->all, true, list_row, &app,
                            error, sizeof(error)) < 0 ? 3 : 0;
        if (rc)
            (void)snag_ui_text(&app.ui, SNAG_UI_ERROR, error);
        goto out;
    }
    if (cli->resume) {
        const struct snag_provider_config *resume_provider;
        const char *resume_model;
        if (cli->workspace) {
            relocated_workspace = resolve_workspace_path(cli->workspace, "relocation",
                                                         error, sizeof(error));
            if (!relocated_workspace) {
                (void)snag_ui_text(&app.ui, SNAG_UI_ERROR, error);
                rc = 2;
                goto out;
            }
        }
        if (cli->resume_id)
            rc = snag_session_open(&app.store, &app.session, cli->resume_id,
                                  error, sizeof(error));
        else if (cli->last)
            rc = snag_session_open_last(&app.store, &app.session, workspace,
                                       cli->all, error, sizeof(error));
        else
            rc = pick_session(&app, workspace, error, sizeof(error));
        if (rc == 1) {
            (void)snag_ui_text(&app.ui, SNAG_UI_WARNING, error);
            rc = 0;
            goto out;
        }
        if (rc < 0) {
            (void)snag_ui_text(&app.ui, SNAG_UI_ERROR, error);
            rc = 3;
            goto out;
        }
        resume_provider = snag_config_provider(&config,
            cli->provider ? cli->provider : app.session.default_provider);
        resume_model = cli->model ? new_model : app.session.default_model;
        if (!resume_provider) {
            (void)snag_ui_text(&app.ui, SNAG_UI_ERROR, "selected provider is not configured; use --provider NAME");
            rc = 2;
            goto out;
        }
        if (!cli->execute && validate_prompt_values(&app.ui, &config,
                resume_provider,
                cli->model ? new_model : app.session.default_model,
                resolve_effort(cli->effort ? cli->effort :
                               app.session.default_effort)) < 0) {
            (void)snag_ui_text(&app.ui, SNAG_UI_ERROR,
                "configured prompt cannot be rendered with the current selection");
            rc = 2;
            goto out;
        }
        if (app.session.archived && snag_session_unarchive(&app.session, NULL, error, sizeof(error)) < 0) {
            (void)snag_ui_text(&app.ui, SNAG_UI_ERROR, error); rc = 3; goto out;
        }
        if (recover_session(&app, error, sizeof(error)) < 0) {
            (void)snag_ui_text(&app.ui, SNAG_UI_ERROR, error);
            rc = 3;
            goto out;
        }
        if (app.session.goal_status == SNAG_GOAL_ACTIVE) {
            if (snag_app_goal_pause(&app, "session_resumed",
                                   error, sizeof(error)) < 0) {
                (void)snag_ui_text(&app.ui, SNAG_UI_ERROR, error);
                rc = 3;
                goto out;
            }
            goal_paused_on_resume = true;
        }
        if (relocated_workspace &&
            strcmp(relocated_workspace, app.session.workspace) != 0 &&
            commit_event(&app, "workspace_changed",
                         snag_app_preference_changed_data("old_workspace",
                                                app.session.workspace,
                                                "new_workspace",
                                                relocated_workspace),
                         error, sizeof(error)) < 0) {
            (void)snag_ui_text(&app.ui, SNAG_UI_ERROR, error);
            rc = 3;
            goto out;
        }
        if (cli->provider &&
            record_model_selection(&app, resume_provider->name, resume_model,
                cli->effort ? cli->effort : app.session.default_effort,
                error, sizeof(error)) < 0) {
            (void)snag_ui_text(&app.ui, SNAG_UI_ERROR, error);
            rc = 3;
            goto out;
        }
        app.staged_model = cli->provider ? NULL : cli->model ? new_model : NULL;
        app.staged_effort = cli->provider ? NULL : cli->effort;
        app.turn_model = next_model(&app);
        app.turn_effort = resolve_effort(next_effort(&app));
        app.turn_provider = next_provider(&app);
    } else {
        const char *selected_workspace = cli->workspace ? cli->workspace : workspace;
        const struct snag_provider_config *selected_provider =
            snag_config_provider(&config,
                cli->provider ? cli->provider : config.provider[0] ? config.provider : NULL);
        if (!cli->execute && validate_prompt_values(
                &app.ui, &config, selected_provider, new_model,
                resolve_effort(new_effort)) < 0) {
            (void)snag_ui_text(&app.ui, SNAG_UI_ERROR,
                "configured prompt cannot be rendered with the current selection");
            rc = 2;
            goto out;
        }
        if (snag_session_create(&app.store, &app.session, selected_workspace,
                               selected_provider->name, new_model, new_effort,
                               error, sizeof(error)) < 0) {
            (void)snag_ui_text(&app.ui, SNAG_UI_ERROR, error);
            goto out;
        }
        app.turn_model = next_model(&app);
        app.turn_effort = resolve_effort(app.session.default_effort);
        app.turn_provider = selected_provider;
    }
    if (!cli->execute) {
        if (snag_irc_open(&app.irc, &config, app.session.workspace,
                         snag_app_irc_event, snag_app_irc_trace, &app,
                         error, sizeof(error)) < 0 ||
            snag_app_irc_restore(&app, error, sizeof(error)) < 0 ||
            (config.irc.listen_explicit &&
             snag_app_irc_snapshot(&app, "join", error, sizeof(error)) < 0)) {
            (void)snag_ui_text(&app.ui, SNAG_UI_ERROR, error[0] ? error :
                                   "IRC startup failed");
            rc = 3;
            goto out;
        }
    }
    if (cli->execute) {
        bool read_only;
        const char *query = snag_prompt_parse(cli->prompt, &read_only);
        rc = run_tracked_turn(&app, query, NULL, false, read_only);
        if (rc == 0 && (app.queue_armed || app.goal_armed))
            rc = run_ready_chains(&app);
        goto out;
    }
    if (snag_ui_open(&app.ui, error, sizeof(error)) < 0) {
        (void)snag_ui_text(&app.ui, SNAG_UI_ERROR, error);
        rc = 3;
        goto out;
    }
    (void)snag_ui_history_open(&app.ui, dotdir);
    history_warning(&app);
    if (snag_ui_orientation(&app.ui, &app.session, cli->resume) < 0 ||
        (app.networked && snag_irc_replay_hosted_history(
            app.irc, render_room_history, &app.ui) < 0) ||
        (cli->resume && config.resume_history_turns != 0u && !app.networked &&
         snag_ui_history(&app.ui, &app.session) < 0) ||
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
    (void)snag_ui_text(&app.ui, SNAG_UI_CLOSE, NULL);
    snag_irc_close(app.irc);
    write_resume_command(&app, program, dotdir);
    atomic_store(&shutdown_ui, NULL);
    snag_tools_shutdown();
    snag_tools_journal(NULL, NULL, NULL);
    snag_ui_free(&app.ui);
    (void)capture_shutdown_signal(&app);
    if (signal_handlers_installed)
        restore_shutdown_handlers(&signal_handlers);
    snag_buf_free(&app.irc_urgent);
    snag_buf_free(&app.irc_background);
    free(config_path);
    free(dotdir);
    free(relocated_workspace);
    free(workspace);
    snag_instructions_free(&app.turn_instructions);
    snag_model_cache_free(&app.model_cache);
    snag_session_close(&app.session);
    snag_store_close(&app.store);
    snag_config_free(&config);
    if (app.shutdown_signal > 0 && app.shutdown_signal < 128)
        rc = 128 + app.shutdown_signal;
    return rc;
}
