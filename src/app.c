/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"
#include "commands.h"
#include "store_internal.h"
#include "http.h"
#include "media.h"
#include "fs.h"
#include "base.h"
#include "config.h"
#include "context.h"
#include "credential.h"
#include "json.h"
#include "provider.h"
#include "provider_retry.h"
#include "render.h"
#include "rules.h"
#include "secret.h"
#include "session_client.h"
#include "snajpagent.h"
#include "store.h"
#include "term_host.h"
#include "turn.h"
#include "tools.h"
#if SNAJPAGENT_VM
#include "session_view.h"
#include "vm_report.h"
#endif
#include "wire.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RESUME_COMMAND_MAX (4u * 1024u * 1024u)

#if SNAJPAGENT_VM
struct snag_app_direct {
    pthread_mutex_t lock;
    pthread_t thread;
    struct snag_ui *ui;
    struct snag_view_channel channel;
    char *program, *dotdir, *resume, *name;
    char session[SNAG_ID_HEX_LEN + 1u], error[256];
    bool ready, finished, stop;
    int status;
};

static atomic_bool direct_busy;

struct app_view_command {
    const char *line;
    json_t *snapshot, *selection;
    int verbosity;
    bool chained;
};
struct app_view_terminal {
    char id[SNAG_ID_HEX_LEN + 1u];
    char *command;
    struct snag_buf report;
    struct snag_pager *pager;
    unsigned int controls;
    bool dispatching, failed;
};
static int view_terminal_finish(struct app_state *);
static void view_terminal_free(struct app_state *);
static int view_control_publish(struct app_state *, unsigned int, bool);
static const char *const view_control_names[] = {"/config (completion)",
    "/model cache (completion)", "/compact (completion)", "/archive (completion)",
    "/delete (completion)", "/retry (completion)", "/configure (completion)"};
#endif /* SNAJPAGENT_VM */

static atomic_int pending_shutdown_signal;
static _Atomic(struct snag_ui *) shutdown_ui;
static void write_resume_command(struct app_state *, const char *, const char *);
static int submit_idle(struct app_state *, const char *, enum snag_render_view, bool *);

static void
mark_shutdown_signal(int signal_number)
{
    int expected = 0;
    int saved = errno;
    (void)atomic_compare_exchange_strong(&pending_shutdown_signal, &expected, signal_number);
    snag_ui_signal(atomic_load(&shutdown_ui));
    errno = saved;
}

bool
snag_app_shutdown(struct app_state *app)
{
    int signal_number = atomic_load(&pending_shutdown_signal);

    if (!signal_number) return false;
    if (!app->shutdown_signal) app->shutdown_signal = (int)signal_number;
    app->input_closed = true;
    return true;
}

/* Owned by run_tracked_turn; never retained in app or session state. */
struct turn_retry {
    uint64_t attempts;
    uint32_t limit;
    enum snag_goal_status goal_status;
    bool pending, new_input;
    bool compaction_bounded;
    char last_failure_code[64];
    char last_failure_type[64];
    char last_failure_message[256];
};

static const char *
fallback_model(const struct app_state *app)
{
    const char *value = app->session.fallback_model[0] ? app->session.fallback_model
                                                     : app->config->fallback_model;
    return value[0] ? value : "off";
}

bool
snag_app_retry_allowed(const void *opaque)
{
    const struct app_state *app = opaque;

    return app->session.retry_auto ? !strcmp(app->session.retry_auto, "on")
                                   : app->config->retry_auto;
}

static bool
turn_retry_available(const struct app_state *app, const struct turn_retry *retry)
{
    return snag_app_retry_allowed(app) && !retry->compaction_bounded && !app->turn_policy_stopped &&
           !app->interrupt_requested && !app->input_closed &&
           (app->session.goal_status == SNAG_GOAL_ACTIVE ||
               (app->session.goal_status == retry->goal_status && retry->attempts < retry->limit));
}

static int
failure_notice(struct app_state *app, const char *message, bool recovering)
{
    uint64_t now = snag_monotonic_ms();
    if ((app->session.goal_status == SNAG_GOAL_ACTIVE || app->session.active_turn) &&
        app->recovery_delay_ms && !strcmp(app->recovery_error, message) &&
        now - app->recovery_notice_ms < 30000u)
        return 0;
    (void)snag_strcpy(app->recovery_error, sizeof(app->recovery_error), message);
    app->recovery_notice_ms = now;
    if (recovering) {
        struct snag_buf text = {.max = 4096u};
        int rc = snag_buf_printf(&text, "%s; recovering automatically", message);
        if (!rc) rc = snag_ui_text(&app->ui, SNAG_UI_WARNING, (const char *)text.data);
        snag_buf_free(&text);
        return rc;
    }
    return snag_ui_text(&app->ui, SNAG_UI_ERROR, message);
}

static int
app_error(struct app_state *app, const char *message)
{
    return failure_notice(app, message, false);
}
static int list_row(void *opaque, const char *text, size_t len);
static unsigned int list_columns(const struct app_state *app);
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
persist_session(struct app_state *app, char *error, size_t error_size)
{
    if (snag_app_save_resume_options(app, error, error_size) < 0) return -1;
    int rc = snag_session_persist(&app->store, &app->session, error, error_size);
    if (rc == 0 && app->ui.opened) {
        (void)snag_history_bind(&app->ui.history, app->session.dir_path);
        history_warning(app);
    }
    return rc;
}
static int
app_textf(struct app_state *app, enum snag_ui_operation operation, const char *fmt, ...)
{
    va_list ap;
    int rc;

    if (operation == SNAG_UI_RUNTIME && !snag_ui_enabled(&app->ui, SNAG_PRESENT_DEBUG)) return 0;
    struct snag_buf text = {.max = 4u * 1024u * 1024u};
    va_start(ap, fmt);
    rc = snag_buf_vprintf(&text, fmt, ap);
    va_end(ap);
    if (rc == 0) rc = snag_ui_text(&app->ui, operation, (const char *)text.data);
    snag_buf_free(&text);
    return rc;
}

int
snag_app_report(struct app_state *app, enum snag_ui_operation operation, const char *text)
{
    if (!app->command_report) return snag_ui_text(&app->ui, operation, text);
    size_t len = strlen(text);
    if (snag_term_append_safe(app->command_report, text, len) < 0) return -1;
    if ((!len || text[len - 1u] != '\n') && snag_buf_putc(app->command_report, '\n') < 0) return -1;
    return snag_ui_send(&app->ui,
        (struct snag_ui_command){.kind = operation, .text = text, .len = len, .retain_only = true});
}

static int
app_reportf(struct app_state *app, const char *format, ...)
{
    struct snag_buf text = {.max = SIZE_MAX};
    va_list args;

    va_start(args, format);
    int rc = snag_buf_vprintf(&text, format, args);
    va_end(args);
    if (!rc) rc = snag_app_report(app, SNAG_UI_HOST, (const char *)text.data);
    snag_buf_free(&text);
    return rc;
}
static int
service_attachment(struct app_state *app, bool external)
{
    snag_mcp_poll(app->mcp);
    if (snag_ui_view_state(&app->ui, &app->session) < 0) return -1;
    if (snag_app_voice_attachment_service(app) < 0) return -1;
    uint64_t generation = snag_ui_session_pending(&app->ui);
    if (generation) {
        int prepared = snag_app_voice_attachment_prepare(app, generation);
        if (prepared <= 0) return prepared;
        if (snag_ui_session_rebind(&app->ui, generation) < 0) return errno == ESTALE ? 0 : -1;
        if (!app->ui.native_continuing) {
            app->attachment_history_pending = true;
            app->remote_verified = app->remote_available = false;
            app->remote_nonce[0] = '\0';
            app->remote_probe_at = app->remote_reply_at = 0u;
            app->remote_attachment = 0u;
        }
    }
    int rc = 0;
    if (!external && app->attachment_history_pending) {
        app->attachment_history_pending = false;
        rc = app_textf(app, SNAG_UI_HOST,
            "Attached session %s; recent history follows. Use /history N for earlier turns.",
            app->session.id);
        if (!rc) rc = snag_ui_history(&app->ui, &app->session, app->config->resume_history_turns);
    }
    if (generation && snag_ui_session_ready(&app->ui, generation) < 0 && errno != ESTALE) return -1;
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
    case SNAG_GRAPH_CALLS:
        return "calls";
    case SNAG_GRAPH_FINAL:
        return "final";
    case SNAG_GRAPH_REFUSAL:
        return "refusal";
    case SNAG_GRAPH_NONPRODUCTIVE:
        return "nonproductive";
    case SNAG_GRAPH_CONFLICT:
        return "conflict";
    }
    return "unknown";
}
static const char *
effective_model(const char *model)
{
    return strcmp(model, "default") == 0 ? SNAJPAGENT_MODEL : model;
}
static const char *
resolve_effort(const char *preference)
{
    if (!preference || strcmp(preference, "default") == 0) return "medium";
    if (!snag_text_valid(preference, 1u, SNAG_CONFIG_EFFORT_MAX - 1u)) return NULL;
    return preference;
}
static const struct snag_provider_config *
next_provider(const struct app_state *app)
{
    return snag_config_provider(
        app->config, app->session.default_provider[0] ? app->session.default_provider : NULL);
}

static void
provider_capacity_source_sha256(const struct snag_provider_config *provider, const char *model,
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
capacity_ceiling_matches(
    const struct app_state *app, const struct snag_provider_config *provider, const char *model)
{
    char source_hash[SNAG_SHA256_HEX_LEN + 1u];

    if (!app->session.capacity_ceiling_valid ||
        strcmp(app->session.capacity_ceiling_provider, provider->name) != 0 ||
        strcmp(app->session.capacity_ceiling_model, model) != 0)
        return false;
    provider_capacity_source_sha256(provider, model, source_hash);
    return strcmp(app->session.capacity_ceiling_source_sha256, source_hash) == 0;
}

static void
apply_capacity_ceiling(const struct app_state *app, const struct snag_provider_config *provider,
    const char *model, struct snag_model_capacity *capacity)
{
    if (capacity_ceiling_matches(app, provider, model) &&
        (!capacity->hard_input_known ||
            app->session.capacity_ceiling_input_tokens < capacity->hard_input_tokens)) {
        capacity->hard_input_tokens = app->session.capacity_ceiling_input_tokens;
        capacity->hard_input_known = true;
        capacity->source = SNAG_CAPACITY_OBSERVED;
    }
}

void
snag_app_record_model_accounting(
    struct app_state *app, enum snag_count_capability capability, uint64_t hard_input_tokens)
{
    char error[256] = {0};

    if (capability != SNAG_COUNT_UNKNOWN) app->turn_capacity.count_capability = capability;
    if (snag_model_cache_record(&app->store, &app->model_cache, app->turn_provider,
            snag_provider_catalog_protocol(app->turn_provider), app->turn_model, capability,
            hard_input_tokens, error, sizeof(error)) < 0)
        (void)app_warning(
            app, error[0] ? error : "model accounting observation could not be cached");
}

int
snag_app_context_preview(struct app_state *app, const struct snag_provider_config *provider,
    const char *model, const struct snag_context_choice *choice,
    struct snag_model_capacity *capacity, char *error, size_t error_size)
{
    int cache_rc;

    if (!app || !provider || !model || !choice || !capacity)
        return snag_fail(error, error_size, EINVAL, "invalid model capacity selection");
    cache_rc = snag_model_capacity_resolve(&app->model_cache, app->config, provider, model,
        snag_provider_catalog_protocol(provider), choice, capacity, error, error_size);
    if (cache_rc == 0) apply_capacity_ceiling(app, provider, model, capacity);
    return cache_rc;
}

int
snag_app_capacity_resolve(struct app_state *app, const struct snag_provider_config *provider,
    const char *model, struct snag_model_capacity *capacity, char *error, size_t error_size)
{
    struct snag_context_choice choice = {SNAG_CONTEXT_MODE_DEFAULT, 0u};

    if (!app) return snag_fail(error, error_size, EINVAL, "invalid model capacity selection");
    choice.mode = app->session.context_mode;
    choice.tokens = app->session.context_tokens;
    if (app->session.turn_fallback_active &&
        !strcmp(provider->name, app->session.active_turn_provider) &&
        !strcmp(model, app->session.active_turn_model))
        choice = app->session.turn_fallback_context;
    return snag_app_context_preview(app, provider, model, &choice, capacity, error, error_size);
}

static int
prepare_turn_settings(struct app_state *app, char *error, size_t error_size)
{
    const char *model = app->session.active_turn ? app->session.active_turn_model
                        : app->session.pending_input
                            ? snag_json_string(app->session.pending_input, "model")
                            : app->session.default_model;
    const char *effort_preference = app->session.active_turn ? app->session.active_turn_effort
                                    : app->session.pending_input
                                        ? snag_json_string(app->session.pending_input, "effort")
                                        : app->session.default_effort;
    const char *effort = resolve_effort(effort_preference);
    const struct snag_provider_config *provider =
        app->session.active_turn
            ? snag_config_provider(app->config, app->session.active_turn_provider)
        : app->session.pending_input ? snag_config_provider(app->config,
                                           snag_json_string(app->session.pending_input, "provider"))
                                     : next_provider(app);
    if (!provider) {
        return snag_fail(error, error_size, ENOENT,
            "selected provider is not present in the current configuration");
    }
    if (!effort) {
        return snag_fail(
            error, error_size, ENOTSUP, "reasoning effort is empty, oversized, or invalid UTF-8");
    }
    /* Keep this identity through turn_completed and its post-turn measurement.
     * Both defaults and the reducer's active-turn storage can change earlier. */
    if (!snag_strcpy(app->turn_model_value, sizeof(app->turn_model_value), model) ||
        !snag_strcpy(app->turn_effort_value, sizeof(app->turn_effort_value), effort))
        return snag_fail(error, error_size, EINVAL, "turn model or effort is oversized");
    app->turn_model = app->turn_model_value;
    app->turn_effort = app->turn_effort_value;
    app->turn_provider = provider;
    if (snag_app_capacity_resolve(app, provider, model, &app->turn_capacity, error, error_size) < 0)
        return -1;
    return 0;
}
static unsigned int prompt_spinner_states(const struct app_state *app);
static int set_input_prompt(struct app_state *app, bool active);
static void context_observe(struct app_state *app, const char *type, const json_t *data);

static int
commit_event_with_request(struct app_state *app, const char *type, json_t *data,
    const char *request, char *error, size_t error_size)
{
    uint64_t seq;
    enum snag_goal_status previous_goal_status = app->session.goal_status;
    if (app->ui.input_received_ms &&
        snag_string_in(type, "steering_added future_turn_queued future_turn_edited") &&
        snag_json_set_new(
            data, "received_at_ms", json_integer((json_int_t)app->ui.input_received_ms)) < 0) {
        json_decref(data);
        return -1;
    }
    const json_t *routing = json_object_get(data, "routing");
    const char *conversation = snag_json_string(routing, "conversation_kind");
    const char *kind = snag_json_string(data, "kind");
    bool private_chat = !strcmp(type, "irc_event_v2") && conversation &&
                        !strcmp(conversation, "query") && kind &&
                        snag_string_in(kind, "message notice");
    if ((private_chat ||
            snag_string_in(type, "goal_started control_requested future_turn_queued input_received "
                                 "irc_admitted voice_event")) &&
        persist_session(app, error, error_size) < 0) {
        ++app->session.write_failures;
        json_decref(data);
        return -1;
    }
    char view_request[SNAG_ID_HEX_LEN + 1u] = {0};
    if (request) (void)snag_strcpy(view_request, sizeof(view_request), request);
    json_t *voice_event = app->voice ? json_incref(data) : NULL;
    json_t *accounting =
        snag_string_in(type, "response_started response_completed") ? json_incref(data) : NULL;
    int committed = snag_session_commit(&app->session, type, data, &seq, error, error_size);
    if (!committed && voice_event) snag_app_voice_event(app, type, voice_event);
    json_decref(voice_event);
    if (!committed && accounting) context_observe(app, type, accounting);
    json_decref(accounting);
    if (committed < 0) return -1;
    if (!strcmp(type, "context_rebased")) app->prompt_context.valid = false;
    if (view_request[0]) {
        /* Receipt publication follows fsync/reducer admission. A disconnected
         * frontend never changes a successfully committed input into failure. */
        (void)snag_ui_view_result(&app->ui, view_request, "committed", seq, type);
    }
    /* Present committed goal state before any notice can redraw the composer. */
    if (previous_goal_status != app->session.goal_status && app->ui.opened &&
        snag_ui_send(&app->ui, (struct snag_ui_command){.kind = SNAG_UI_SPINNERS,
                                   .data.value = prompt_spinner_states(app)}) < 0)
        return snag_errorf(error, error_size, "goal prompt state could not be displayed");
    if (snag_string_in(type, "steering_added future_turn_queued future_turn_edited"))
        ++app->input_generation;
    /* Chunk durability must not insert debug notices inside the public text. */
    if (!strcmp(type, "response_output")) return 0;
    struct snag_render_source source = {.offset = app->session.committed_start,
        .len = (size_t)(app->session.committed_end - app->session.committed_start)};
    if (app->session.binary) {
        source.native_sequence = seq;
        if (snag_session_binary_checkpoint_capture(
                &app->session, &source.native_boundary, NULL, NULL, error, error_size) < 0)
            return -1;
    }
    if ((!app->session.pending_log &&
            snag_ui_send(&app->ui,
                (struct snag_ui_command){.kind = SNAG_UI_DURABLE,
                    .text = type,
                    .data.durable = {app->session.log_fd, source, app->config->default_timeout_ms,
                        app->config->max_output_bytes}}) < 0) ||
        snag_ui_send(&app->ui,
            (struct snag_ui_command){.kind = SNAG_UI_EVENT, .text = type, .data.seq = seq}) < 0) {
        return snag_errorf(error, error_size, "durable event output failed");
    }
    /* Accounting and its display projection are adopted before repainting. */
    if (app->ui.opened &&
        snag_string_in(
            type, "response_started response_completed compaction_completed context_rebased") &&
        set_input_prompt(app, app->session.active_turn) < 0)
        return snag_errorf(error, error_size, "context prompt could not be updated");
    if (strcmp(type, "turn_failed") == 0 && app->session.goal_status != SNAG_GOAL_ACTIVE)
        return app_warning(app, "turn failed; try /retry to continue");
    return 0;
}
int
snag_app_commit_event(
    struct app_state *app, const char *type, json_t *data, char *error, size_t error_size)
{
    return commit_event_with_request(app, type, data, NULL, error, error_size);
}
#define commit_event snag_app_commit_event

int
snag_app_queue_arm(struct app_state *app, bool armed)
{
    char error[256] = {0};
    armed = armed && app->session.pending_queue_count != 0u;
    if (app->session.queue_armed != armed &&
        commit_event(app, "future_queue_state", json_pack("{s:b}", "armed", armed), error,
            sizeof(error)) < 0) {
        (void)app_error(app, error);
        return -1;
    }
    return 0;
}

static void
view_control_requested(struct app_state *app, unsigned int control)
{
#if SNAJPAGENT_VM
    if (app->view_terminal && app->view_terminal->dispatching)
        app->view_terminal->controls |= control;
    if (app->view_command) app->view_controls |= control;
#else
    (void)app;
    (void)control;
#endif
}

static int
request_control(struct app_state *app, unsigned int control, const char *name)
{
    char error[256] = {0};
    if (app->session.pending_controls & control) {
        view_control_requested(app, control);
        return app_textf(app, SNAG_UI_HOST, "%s already pending or applying", name);
    }
    if (commit_event(app, "control_requested", json_pack("{s:i}", "control", (int)control), error,
            sizeof(error)) < 0)
        return app_error(app, error), -1;
    view_control_requested(app, control);
    if (!app->applying_controls) app->control_requested = true;
    return app_textf(
        app, SNAG_UI_HOST, "%s accepted; applying at the next safe request boundary", name);
}

/* Consume exactly the parts admitted to this input, even if presentation fails
 * after durable append. Parts added while auth/I/O ran remain in the composer. */
static int
commit_input(struct app_state *app, const char *type, json_t *data, const json_t *content,
    char *error, size_t error_size)
{
    json_t *remaining = app->draft_content ? json_array() : NULL;
    if (app->draft_content && !remaining) {
        json_decref(data);
        return -1;
    }
    for (size_t i = 0; i < json_array_size(app->draft_content); ++i) {
        json_t *part = json_array_get(app->draft_content, i);
        bool accepted = false;
        for (size_t j = 0; j < json_array_size(content); ++j)
            if (json_equal(part, json_array_get(content, j))) {
                accepted = true;
                break;
            }
        if (!accepted && json_array_append(remaining, part) < 0) {
            json_decref(remaining);
            json_decref(data);
            return -1;
        }
    }
    if (content && snag_json_set_new(data, "content", json_incref((json_t *)content)) < 0) {
        json_decref(data);
        json_decref(remaining);
        return -1;
    }
    const char *request =
        snag_string_in(type, "input_received steering_added future_turn_queued future_turn_edited")
            ? app->ui.view_request
            : NULL;
    uint64_t before = app->session.next_seq;
    int rc = commit_event_with_request(app, type, data, request, error, error_size);
    if (app->session.next_seq != before) {
        json_decref(app->draft_content);
        app->draft_content = remaining;
        remaining = NULL;
        if (!json_array_size(app->draft_content)) {
            json_decref(app->draft_content);
            app->draft_content = NULL;
        }
    }
    json_decref(remaining);
    return rc;
}

static int
render_queue(struct app_state *app)
{
    if (app->session.pending_queue_count == 0u) {
        return snag_app_report(app, SNAG_UI_WARNING, "future-turn queue is empty");
    }
    for (size_t i = 0; i < app->session.pending_queue_count; ++i) {
        char label[64];
        (void)snprintf(label, sizeof(label), "%zu %.8s%s › ", i + 1u,
            app->session.pending_queue[i].queue_id,
            app->session.pending_queue[i].read_only ? " /ro" : "");
        const char *text = app->session.pending_queue[i].text;
        int rc = app->command_report ? app_reportf(app, "%s%s", label, text)
                                     : snag_ui_submitted(&app->ui, label, text, false);
        if (rc < 0) return -1;
    }
    return 0;
}

static bool
context_meter_matches(struct app_state *app, const struct snag_provider_config *provider,
    const char *model, const char *effort)
{
    char hash[SNAG_SHA256_HEX_LEN + 1u];
    if (!provider || !model || !effort) return false;
    provider_capacity_source_sha256(provider, model, hash);
    return snag_input_observation_matches(
        &app->session.context_meter, provider->name, model, effort, hash, app->session.compact_id);
}

bool
snag_app_measured_input(struct app_state *app, uint64_t *tokens)
{
    if (!context_meter_matches(app, app->turn_provider, app->turn_model, app->turn_effort))
        return false;
    *tokens = app->session.context_meter.input_tokens;
    return true;
}

static uint64_t
context_estimate(const struct app_state *app, const struct snag_input_observation *input)
{
    const struct snag_input_observation *anchor = &app->session.usage_anchor;
    long double estimate = (long double)input->model_input_bytes / 4.0L;
    if (anchor->input_tokens && anchor->model_input_bytes &&
        snag_input_observation_matches(anchor, input->provider, input->model, input->effort,
            input->provider_source_sha256, anchor->compact_id))
        estimate = (long double)input->model_input_bytes * anchor->input_tokens /
                   anchor->model_input_bytes;
    if (estimate >= (long double)UINT64_MAX) return UINT64_MAX;
    uint64_t rounded = (uint64_t)estimate;
    return rounded + (estimate > (long double)rounded);
}

static void
context_observe(struct app_state *app, const char *type, const json_t *data)
{
    if (!strcmp(type, "response_started")) {
        app->prompt_context = app->session.active_accounting;
        const char *method = snag_json_string(data, "count_method");
        app->prompt_context_estimated = strcmp(method, "exact") != 0;
        if (!strcmp(method, "unknown"))
            app->prompt_context.input_tokens = context_estimate(app, &app->prompt_context);
        app->prompt_context_output_bytes = 0u;
        app->prompt_context_output_tokens = 0u;
        app->prompt_context_output_known = false;
    } else {
        struct snag_response_usage usage;
        if (snag_response_usage_from_json(json_object_get(data, "usage"), &usage) < 0) return;
        if (usage.input_known) {
            app->prompt_context.input_tokens = usage.input_tokens;
            app->prompt_context_estimated = false;
        }
        app->prompt_context_output_known = usage.output_known;
        app->prompt_context_output_tokens = usage.output_tokens;
    }
}

static int
context_preview(struct app_state *app, const struct snag_provider_config *provider,
    const char *model, const char *effort, const struct snag_model_capacity *capacity)
{
    struct snag_context_projection projection = {0};
    struct snag_context_control control = {.preview = true,
        .history_orientation = app->history_orientation,
        .goal_recovery_rebase = app->history_recovery_rebase,
        .mcp_tools = snag_mcp_tools(app->mcp)};
    struct snag_input_observation input = {0};
    json_t *steering = snag_app_steering_snapshot(&app->session);
    char error[256] = {0};
    int rc = snag_context_build(&app->session, model, effort, app->session.active_cycle + 1u,
        steering, capacity->max_output_tokens, capacity->max_output_tokens != 0u, app->config,
        app->session.compact_scope, &app->turn_instructions, NULL, &projection, error,
        sizeof(error), &control);
    if (!rc && snag_strcpy(input.provider, sizeof(input.provider), provider->name) &&
        snag_strcpy(input.model, sizeof(input.model), model) &&
        snag_strcpy(input.effort, sizeof(input.effort), effort)) {
        provider_capacity_source_sha256(provider, model, input.provider_source_sha256);
        memcpy(input.compact_id, app->session.compact_id, sizeof(input.compact_id));
        input.model_input_bytes = projection.model_input.bytes;
        input.input_tokens = context_estimate(app, &input);
        input.valid = true;
        app->prompt_context = input;
        app->prompt_context_estimated = true;
        app->prompt_context_output_bytes = 0u;
        app->prompt_context_output_tokens = 0u;
        app->prompt_context_output_known = true;
    } else rc = -1;
    snag_context_projection_free(&projection);
    json_decref(steering);
    return rc;
}

static int
format_context_meter(struct app_state *app, bool active, char meter[32u])
{
    const struct snag_provider_config *provider = active ? app->turn_provider : next_provider(app);
    const char *model = active ? app->turn_model : app->session.default_model;
    const char *effort = active ? app->turn_effort : resolve_effort(app->session.default_effort);
    struct snag_model_capacity resolved;
    const struct snag_model_capacity *capacity = &app->turn_capacity;
    uint64_t used;
    uint64_t hard;
    unsigned int percent;
    int n;

    if (!active && app->session.turn_count == 0u && !app->session.last_user) {
        memcpy(meter, "0", sizeof("0"));
        return 0;
    }
    if (!provider || !model || !effort) return snag_errno(EINVAL);
    if (!active) {
        char error[256] = {0};

        if (snag_app_capacity_resolve(app, provider, model, &resolved, error, sizeof(error)) < 0) {
            /* Keep recovery commands available when a saved selection needs
             * catalog facts that are absent. Requests still resolve strictly. */
            memcpy(meter, "?", sizeof("?"));
            return 0;
        }
        capacity = &resolved;
    }
    if (!capacity->hard_input_known) {
        memcpy(meter, "?", sizeof("?"));
        return 0;
    }
    char hash[SNAG_SHA256_HEX_LEN + 1u];
    provider_capacity_source_sha256(provider, model, hash);
    if (!snag_input_observation_matches(
            &app->prompt_context, provider->name, model, effort, hash, app->session.compact_id) &&
        context_preview(app, provider, model, effort, capacity) < 0) {
        memcpy(meter, "?", sizeof("?"));
        return 0;
    }
    used = app->prompt_context.input_tokens;
    uint64_t output = app->prompt_context_output_known
                          ? app->prompt_context_output_tokens
                          : app->prompt_context_output_bytes / 4u +
                                (app->prompt_context_output_bytes % 4u != 0u);
    used = output > UINT64_MAX - used ? UINT64_MAX : used + output;
    hard = capacity->hard_input_tokens;
    if (used >= hard) {
        percent = 100u;
    } else {
        percent = (unsigned int)((used * 100u + hard - 1u) / hard);
    }
    bool estimated = app->prompt_context_estimated ||
                     (!app->prompt_context_output_known && app->prompt_context_output_bytes);
    n = snprintf(meter, 32u, "%s%u", estimated ? "~" : "", percent);
    if (n < 0 || n >= 32) return snag_errno(EOVERFLOW);
    return 0;
}

static void
prompt_hostname(char *hostname, size_t size)
{
    if (snag_hostname(hostname, size) < 0) (void)snag_strcpy(hostname, size, "localhost");
    hostname[size - 1u] = '\0';
    if (!snag_utf8_valid((const unsigned char *)hostname, strlen(hostname), true))
        (void)snag_strcpy(hostname, size, "localhost");
    for (size_t i = 0u; hostname[i]; ++i)
        if ((unsigned char)hostname[i] <= 0x20u || hostname[i] == 0x7f) hostname[i] = '_';
}

static int
render_prompt(struct app_state *app, bool active, const char *submitted)
{
    const struct snag_provider_config *provider =
        active || submitted ? app->turn_provider : next_provider(app);
    const char *model = active || submitted ? app->turn_model : app->session.default_model;
    const char *effort =
        active || submitted ? app->turn_effort : resolve_effort(app->session.default_effort);
    char hostname[256u], meter[32u], label[SNAG_TERM_LABEL_BYTES];
    char queue[64u] = "";
    const char *values[SNAG_PROMPT_HOUR];
    const char *spinners[SNAG_TERM_SPINNER_COUNT] = {app->config->prompt_spinner_goal,
        app->config->prompt_spinner_provider, app->config->prompt_spinner_tool};
    unsigned int states = prompt_spinner_states(app);
    unsigned int selected = submitted                                    ? 1u
                            : snag_ui_view(&app->ui) == SNAG_RENDER_CHAT ? 0u
                            : active                                     ? 2u
                                                                         : 1u;

    if (!provider || !model || !effort || format_context_meter(app, active || submitted, meter) < 0)
        return -1;
    if (!submitted)
        memcpy(app->prompt_context_value, meter, strlen(meter) + 1u);
    prompt_hostname(hostname, sizeof(hostname));
    values[0] = provider->name;
    values[1] = model;
    values[2] = effort;
    values[3] = app->irc ? snag_irc_operator_nick(app->irc) : app->config->irc.operator_nick;
    values[4] = hostname;
    values[5] = meter;
    values[6] = selected == 0u ? "chat" : selected == 1u ? "rollout-idle" : "rollout-active";
    (void)snprintf(queue, sizeof(queue), "%zu", app->session.pending_queue_count);
    if (json_array_size(app->draft_content))
        snprintf(queue + strlen(queue), sizeof(queue) - strlen(queue), " [%zu attached]",
            json_array_size(app->draft_content));
    values[SNAG_PROMPT_QUEUE] = queue;
    values[SNAG_PROMPT_MODEL_NICK] =
        app->irc ? snag_irc_model_nick(app->irc) : app->config->irc.model_nick;
    values[SNAG_PROMPT_SESSION_NAME] = app->session.name ? app->session.name : "";
    if (!submitted && app->queue_edit_id[0]) {
        struct snag_buf out = {.max = SNAG_TERM_LABEL_BYTES};
        for (unsigned int i = 0u; i < SNAG_TERM_SPINNER_SLOTS; ++i)
            if (snag_buf_putc(&out, SNAG_TERM_SPINNER_MARKER_BASE + i) < 0) goto fail;
        if (snag_buf_printf(&out, "%3s%% edit %zu ›", meter, app->queue_edit_number) < 0) goto fail;
        if (!out.len || snag_buf_putc(&out, ' ') < 0 || snag_buf_terminate(&out) < 0) goto fail;
        memcpy(label, out.data, out.len + 1u);
        snag_buf_free(&out);
        return snag_ui_prompt(
            &app->ui, active, label, spinners, app->config->prompt_spinner_per_second, states);
    fail:
        snag_buf_free(&out);
        return -1;
    }
    return snag_ui_composer(&app->ui, active, app->config->prompt, values, selected, spinners,
        app->config->prompt_spinner_per_second, states, submitted);
}

static int
set_input_prompt(struct app_state *app, bool active)
{
    return render_prompt(app, active, NULL);
}

int
snag_app_context_progress(void *opaque, size_t output_bytes)
{
    struct app_state *app = opaque;
    app->prompt_context_output_bytes = output_bytes;
    if (!app->ui.opened) return 0;
    char value[32u];
    if (format_context_meter(app, app->session.active_turn, value) < 0) return -1;
    return strcmp(value, app->prompt_context_value)
               ? set_input_prompt(app, app->session.active_turn)
               : 0;
}

int
snag_app_context_refresh(struct app_state *app)
{
    app->prompt_context.valid = false;
    return app->ui.opened ? set_input_prompt(app, app->session.active_turn) : 0;
}

static int
ensure_turn_prompt(struct app_state *app)
{
    /* A pending provider request is already a future steering target. Show
     * the composer once per turn; automatic response boundaries never hide it. */
    if (!app->session.active_turn || app->turn_prompt_seen) return 0;
    if (set_input_prompt(app, true) < 0) return -1;
    app->turn_prompt_seen = true;
    return 0;
}

static int
validate_prompt_values(struct snag_ui *ui, const struct snag_config *config,
    const struct snag_provider_config *provider, const char *model, const char *effort,
    const char *session_name)
{
    char hostname[256u];
    const char *values[SNAG_PROMPT_FIELD_COUNT];
    const char *spinners[SNAG_TERM_SPINNER_COUNT] = {
        config->prompt_spinner_goal, config->prompt_spinner_provider, config->prompt_spinner_tool};
    char label[SNAG_TERM_LABEL_BYTES];
    int rc = -1;

    if (!provider || !model || !effort) return -1;
    prompt_hostname(hostname, sizeof(hostname));
    values[0] = provider->name;
    values[1] = model;
    values[2] = effort;
    values[3] = config->irc.operator_nick;
    values[4] = hostname;
    values[5] = "100";
    values[SNAG_PROMPT_MODEL_NICK] = config->irc.model_nick;
    values[SNAG_PROMPT_SESSION_NAME] = session_name ? session_name : "";
    values[SNAG_PROMPT_HOUR] = "23";
    values[SNAG_PROMPT_MINUTE] = "59";
    values[SNAG_PROMPT_SECOND] = "60";
    for (unsigned int full = 0u; full < 2u; ++full) {
        values[SNAG_PROMPT_QUEUE] = full ? "999" : "0";
        for (unsigned int mode = 0u; mode < 3u; ++mode) {
            values[6] = mode == 0u ? "chat" : mode == 1u ? "rollout-idle" : "rollout-active";
            if (snag_config_prompt_expand(config->prompt, mode, values,
                    SNAG_TERM_SPINNER_MARKER_BASE, label, sizeof(label)) < 0 ||
                snag_ui_validate_prompt(ui, label, spinners, config->prompt_spinner_per_second) < 0)
                goto out;
        }
    }
    rc = 0;
out:
    return rc;
}

static int
validate_prompt_candidate(
    struct app_state *app, const struct snag_config *config, const char *session_name)
{
    const struct snag_provider_config *provider = snag_config_provider(
        config, app->session.default_provider[0] ? app->session.default_provider : NULL);

    return validate_prompt_values(&app->ui, config, provider, app->session.default_model,
        resolve_effort(app->session.default_effort), session_name);
}

static unsigned int
prompt_spinner_states(const struct app_state *app)
{
    bool tool = app->tool_active || snag_tools_busy();
    return (app->session.goal_status == SNAG_GOAL_ACTIVE ? 1u << SNAG_TERM_SPINNER_GOAL : 0u) |
           (app->provider_active && !tool ? 1u << SNAG_TERM_SPINNER_PROVIDER : 0u) |
           (tool ? 1u << SNAG_TERM_SPINNER_TOOL : 0u);
}

int
snag_app_provider_activity(struct app_state *app, bool active)
{
    int saved_errno = errno;
    app->provider_active = active;
    int rc = app->ui.opened
                 ? snag_ui_send(&app->ui, (struct snag_ui_command){.kind = SNAG_UI_SPINNERS,
                                              .data.value = prompt_spinner_states(app)})
                 : 0;
    if (rc == 0) errno = saved_errno;
    return rc;
}

int
snag_app_request_ready(void *opaque)
{
    struct app_state *app = opaque;

    if (!app->session.active_turn || !app->session.response_open) return 0;
    app->provider_request_ready = true;
    return 0;
}

static int
tick_irc(struct app_state *app, char *error, size_t error_size)
{
    uint64_t revision = snag_irc_routing_revision(app->irc);

    if (snag_irc_tick(app->irc, 0, error, error_size) < 0 || snag_app_sync_destinations(app) < 0)
        return -1;
    if (revision != snag_irc_routing_revision(app->irc)) {
        if (snag_app_irc_snapshot(app, "topology", error, error_size) < 0) return -1;
    }
    if (snag_app_irc_sleeping(app, error, error_size) < 0) return -1;
    if (!snag_irc_identity_changed(app->irc)) return 0;
    const char *operator_nick = snag_irc_operator_nick(app->irc);
    if (operator_nick && strcmp(app->config->irc.operator_nick, operator_nick)) {
        if (!snag_strcpy(app->config->irc.operator_nick, sizeof(app->config->irc.operator_nick),
                operator_nick))
            return -1;
        app->config->irc.operator_nick_implicit = false;
    }
    if (snag_app_irc_snapshot(app, "nick", error, error_size) < 0) return -1;
    if (snag_app_save_resume_options(app, error, error_size) < 0) return -1;
    if (!app->ui.opened || !app->ui.prompt_wanted) return 0;
    return set_input_prompt(app, app->ui.active);
}

static struct snag_queued_turn *
queued_by_id(struct app_state *app, const char *queue_id, size_t *index)
{
    for (size_t i = 0; i < app->session.pending_queue_count; ++i) {
        if (strcmp(app->session.pending_queue[i].queue_id, queue_id) == 0) {
            if (index) *index = i;
            return &app->session.pending_queue[i];
        }
    }
    return NULL;
}

static int
begin_queue_edit(struct app_state *app, size_t number, bool active, char *error, size_t error_size)
{
    struct snag_queued_turn *queued;
    int draft_rc;

    if (number == 0u || number > app->session.pending_queue_count) {
        snag_errorf(error, error_size, "queue item %zu does not exist", number);
        return 1;
    }
    queued = &app->session.pending_queue[number - 1u];
    /* Arming appends an event and swaps the staged session into place. Read the
     * old queue entry before that commit invalidates its text pointer. */
    struct snag_buf draft = {.max = SNAG_MAX_QUEUED_TEXT + 8u};
    draft_rc = snag_buf_printf(&draft, "%s%s",
        queued->read_only        ? "/ro "
        : queued->text[0] == '/' ? "/"
                                 : "",
        queued->text);
    if (draft_rc == 0) draft_rc = snag_buf_terminate(&draft);
    if (draft_rc < 0) {
        snag_buf_free(&draft);
        return snag_errorf(error, error_size, "queue editor could not be displayed");
    }
    memcpy(app->queue_edit_id, queued->queue_id, sizeof(app->queue_edit_id));
    app->queue_edit_number = number;
    app->queue_edit_was_armed = app->session.queue_armed;
    if (snag_app_queue_arm(app, false) < 0) {
        app->queue_edit_id[0] = '\0';
        app->queue_edit_number = 0u;
        app->queue_edit_was_armed = false;
        snag_buf_free(&draft);
        return -1;
    }
    if (draft_rc == 0 && set_input_prompt(app, active) == 0)
        draft_rc = snag_ui_send(
            &app->ui, (struct snag_ui_command){.kind = SNAG_UI_DRAFT, .text = (char *)draft.data});
    else
        draft_rc = -1;
    snag_buf_free(&draft);
    if (draft_rc < 0) {
        if (snag_app_queue_arm(app, app->queue_edit_was_armed) < 0) return -1;
        app->queue_edit_id[0] = '\0';
        app->queue_edit_number = 0u;
        app->queue_edit_was_armed = false;
        return snag_errorf(error, error_size, "queue editor could not be displayed");
    }
    return 0;
}

static int
finish_queue_edit(
    struct app_state *app, const char *text, bool active, char *error, size_t error_size)
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
        snag_errorf(error, error_size, "queued text must be nonempty valid UTF-8 within 256 KiB");
        (void)snag_ui_text(&app->ui, SNAG_UI_ERROR, error);
        error[0] = '\0';
        if (set_input_prompt(app, active) < 0 || snag_ui_restore_input(&app->ui, original) < 0)
            return -1;
        return 1;
    }
    if (commit_event(app, "future_turn_edited",
            json_pack("{s:b,s:b,s:s,s:s}", "armed", restore_armed && app->session.active_turn,
                "read_only", read_only, "queue_id", queued->queue_id, "text", text),
            error, error_size) < 0) {
        if (set_input_prompt(app, active) == 0) (void)snag_ui_restore_input(&app->ui, original);
        return -1;
    }
    if (snag_ui_submitted(&app->ui, app->ui.label, original, false) < 0)
        return snag_errorf(error, error_size, "edited turn acknowledgement could not be rendered");
clear:
    app->queue_edit_id[0] = '\0';
    app->queue_edit_number = 0u;
    app->queue_edit_was_armed = false;
    if (set_input_prompt(app, active) < 0) return -1;
    return rc;
}
struct voice_steering_lookup {
    const char *id;
    const char *text;
    bool found;
    bool conflict;
};

static int
voice_steering_find(void *opaque, const struct snag_session *session, uint64_t seq,
    const char *type, const json_t *data, char *error, size_t size)
{
    (void)session;
    (void)seq;
    (void)error;
    (void)size;
    struct voice_steering_lookup *lookup = opaque;
    const char *id = snag_json_string(data, "steering_id");
    if (!strcmp(type, "steering_added") && id && !strcmp(id, lookup->id)) {
        lookup->found = true;
        const char *text = snag_json_string(data, "text");
        lookup->conflict = !text || strcmp(text, lookup->text);
    }
    return 0;
}

int
snag_app_voice_submit(struct app_state *app, const json_t *source, const char *target,
    char id[SNAG_ID_HEX_LEN + 1u], json_t **result, char *error, size_t size)
{
    struct snag_buf prompt = {.max = SNAG_MAX_STEERING_TEXT};
    *result = NULL;
    id[0] = '\0';
    if (!target || (strcmp(target, "queue") && (!app->session.active_turn ||
                                                   strcmp(target, app->session.active_turn_id)))) {
        *result = snag_tool_result_terminal(
            false, "Target turn is no longer active; input not submitted.");
        return *result ? 0 : -1;
    }
    if (snag_session_voice_prompt(source, &prompt, error, size) < 0) {
        snag_buf_free(&prompt);
        *result = snag_tool_result_terminal(false, error);
        return *result ? 0 : -1;
    }
    if (!strcmp(target, "queue")) {
        bool duplicate = false;
        int rc = snag_session_voice_queue(&app->session, source, id, &duplicate, error, size);
        snag_buf_free(&prompt);
        if (rc < 0) return -1;
        json_t *state = NULL;
        if (snag_session_voice_status(&app->session, id, &state, error, size) < 0) return -1;
        if (!strcmp(snag_json_string(state, "status"), "queued") &&
            snag_app_queue_arm(app, true) < 0) {
            json_decref(state);
            return -1;
        }
        *result = snag_tool_result_terminal(true, snag_json_string(state, "text"));
        json_decref(state);
        return *result ? 0 : -1;
    }
    json_t *identity = json_pack("{s:s,s:s,s:s,s:s}", "kind", "voice_steering", "connection_id",
        snag_json_string(source, "connection_id"), "input_id", snag_json_string(source, "input_id"),
        "turn_id", target);
    char digest[SNAG_SHA256_HEX_LEN + 1u];
    int rc =
        identity ? snag_json_digest_bounded(identity, SNAG_MAX_STEERING_TEXT, digest, NULL) : -1;
    json_decref(identity);
    if (rc < 0) {
        snag_buf_free(&prompt);
        return -1;
    }
    memcpy(id, digest, SNAG_ID_HEX_LEN);
    id[SNAG_ID_HEX_LEN] = '\0';
    struct voice_steering_lookup lookup = {.id = id, .text = (const char *)prompt.data};
    rc = snag_session_each_event(&app->session, voice_steering_find, &lookup, error, size);
    if (!rc && !lookup.found) {
        /* An unrelated keyboard draft's receipt time is not voice provenance. */
        uint64_t received = app->ui.input_received_ms;
        app->ui.input_received_ms = 0u;
        rc = commit_event(app, "steering_added",
            snag_app_steering_added_data(target, id, (const char *)prompt.data), error, size);
        app->ui.input_received_ms = received;
    }
    snag_buf_free(&prompt);
    if (rc < 0) return -1;
    if (lookup.conflict) {
        *result = snag_tool_result_terminal(false,
            "Voice input identity already has different accepted steering; nothing changed.");
        return *result ? 0 : -1;
    }
    if (!lookup.found && !app->session.steering_deferred && !app->session.active_compact_id[0]) {
        app->steering_requested = true;
    }
    const char *text = lookup.found ? "Steering was already accepted."
                       : app->steering_requested
                           ? "Steering accepted for the active turn."
                           : "Steering saved for the next permitted boundary.";
    *result = snag_tool_result_terminal(true, text);
    return *result ? 0 : -1;
}

int
snag_app_voice_interrupt(
    struct app_state *app, const char *turn, json_t **result, char *error, size_t size)
{
    *result = NULL;
    if (!turn || !app->session.active_turn || strcmp(turn, app->session.active_turn_id)) {
        *result = snag_tool_result_terminal(
            false, "Target turn is no longer active; nothing interrupted.");
        return *result ? 0 : -1;
    }
    if (!app->session.cancel_requested && commit_event(app, "turn_cancel_requested",
                                              json_pack("{s:s}", "turn_id", turn), error, size) < 0)
        return -1;
    app->interrupt_requested = true;
    *result = snag_tool_result_terminal(true, "Turn cancellation requested.");
    return *result ? 0 : -1;
}

static int
queue_future_turn(struct app_state *app, const char *text, bool arm, char *error, size_t error_size)
{
    char queue_id[SNAG_ID_HEX_LEN + 1u];
    bool read_only;
    const char *queued_text = snag_prompt_parse(text, &read_only);
    size_t len;
    if (text[0] == '/' && !read_only) {
        if (text[1] != '/') {
            (void)snag_fail(error, error_size, EINVAL,
                "queued text starting with / must use // for a literal slash");
            return 1;
        }
    }
    len = strlen(queued_text);
    if (!len || len > SNAG_MAX_QUEUED_TEXT ||
        !snag_utf8_valid((const unsigned char *)queued_text, len, true)) {
        (void)snag_fail(
            error, error_size, EINVAL, "queued text must be nonempty valid UTF-8 within 256 KiB");
        return 1;
    }
    if (snag_random_id(queue_id) < 0)
        return snag_errorf(error, error_size, "cryptographic queue id generation failed");
    if (commit_input(app, "future_turn_queued",
            json_pack("{s:b,s:b,s:s,s:s,s:s}", "armed", arm || app->session.queue_armed,
                "read_only", read_only, "queue_id", queue_id, "text", queued_text, "while_turn_id",
                app->session.active_turn_id),
            app->draft_content, error, error_size) < 0)
        return -1;
    if (snag_ui_submitted(&app->ui, "queued (/next or /q c) › ", text, false) < 0)
        return snag_errorf(error, error_size, "queued turn acknowledgement could not be rendered");
    return 0;
}
static int
remove_queued_turns(struct app_state *app, size_t index, bool all, char *error, size_t error_size)
{
    json_t *ids;
    size_t matches;

    if (app->session.pending_queue_count == 0u ||
        (!all && index >= app->session.pending_queue_count)) {
        snag_errorf(error, error_size, "future-turn queue is empty");
        return 1;
    }
    matches = all ? app->session.pending_queue_count : 1u;
    ids = json_array();
    if (ids)
        for (size_t i = 0u; i < matches; ++i)
            if (json_array_append_new(
                    ids, json_string(app->session.pending_queue[all ? i : index].queue_id)) < 0) {
                json_decref(ids);
                ids = NULL;
                break;
            }
    if (commit_event(app, "future_turn_cancelled",
            ids ? json_pack("{s:o,s:s}", "queue_ids", ids, "reason", "user") : NULL, error,
            error_size) < 0)
        return -1;
    {
        char message[96];
        (void)snprintf(message, sizeof(message), "%zu future turn%s cancelled", matches,
            matches == 1u ? "" : "s");
        return app_warning(app, message);
    }
}

static int
handle_queue_command(struct app_state *app, const char *line, bool active, bool *handled,
    char *error, size_t error_size)
{
    enum queue_command_kind kind;
    const char *argument;
    size_t number = 0u;

    *handled = true;
    if (snag_string_in(line, "/queue /q")) {
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
        snag_errorf(
            error, error_size, "queue action expects clear, pop, N delete, Nd, N edit, or Ne");
        return 1;
    }
    switch (kind) {
    case QUEUE_COMMAND_LIST:
        return render_queue(app);
    case QUEUE_COMMAND_ADD:
        return queue_future_turn(app, argument, active, error, error_size);
    case QUEUE_COMMAND_DELETE:
        if (number == 0u || number > app->session.pending_queue_count) {
            snag_errorf(error, error_size, "queue item %zu does not exist", number);
            return 1;
        }
        return remove_queued_turns(app, number - 1u, false, error, error_size);
    case QUEUE_COMMAND_EDIT:
        return begin_queue_edit(app, number, active, error, error_size);
    case QUEUE_COMMAND_CLEAR:
        return remove_queued_turns(app, 0u, true, error, error_size);
    case QUEUE_COMMAND_POP:
        if (app->session.pending_queue_count == 0u) {
            snag_errorf(error, error_size, "future-turn queue is empty");
            return 1;
        }
        return remove_queued_turns(
            app, app->session.pending_queue_count - 1u, false, error, error_size);
    }
    return snag_errno(EINVAL);
}

static int
append_capacity_value(struct snag_buf *text, const char *name, bool known, uint64_t value)
{
    return known ? snag_buf_printf(text, " · %s=%llu", name, (unsigned long long)value)
                 : snag_buf_printf(text, " · %s=unknown", name);
}

static int
append_advertised_capacity(struct snag_buf *text, const json_t *model)
{
    static const struct {
        const char *key;
        const char *name;
    } fields[] = {{"context_window_tokens", "context"},
        {"max_context_window_tokens", "max-context"},
        {"input_context_window_tokens", "input-context"}, {"max_input_tokens", "max-input"},
        {"max_output_tokens", "max-output"}, {"auto_compact_input_tokens", "auto-compact"},
        {"effective_context_window_percent", "effective-percent"}};
    const json_t *limits = model ? json_object_get(model, "limits") : NULL;

    if (!json_is_object(limits)) return 0;
    if (snag_buf_append(text, "\nadvertised", 11u) < 0) return -1;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        uint64_t value = 0u;
        bool known = snag_json_integer_u64((json_t *)limits, fields[i].key, &value) == 0;
        if (append_capacity_value(text, fields[i].name, known, value) < 0) return -1;
    }
    return 0;
}

static int
append_compact_threshold(struct snag_buf *text, const struct snag_provider_config *provider,
    const struct snag_model_capacity *capacity)
{
    const char *mode = provider->auto_compact_input_tokens == SNAG_CONFIG_COMPACT_AUTO
                           ? (capacity->hard_input_known ? "auto" : "auto fallback")
                           : (provider->auto_compact_input_tokens ? "fixed" : "off");

    return snag_buf_printf(text, " · compact=%llu (%s)",
        (unsigned long long)snag_model_compact_threshold(provider, capacity), mode);
}

static int
render_status(struct app_state *app)
{
    const char *id = app->session.id;
    const struct snag_provider_config *provider =
        app->session.active_turn ? app->turn_provider : next_provider(app);
    const char *model = app->session.active_turn ? app->turn_model : app->session.default_model;
    const char *effort = app->session.active_turn ? app->turn_effort : app->session.default_effort;
    struct snag_context_choice choice = app->session.turn_fallback_active
        ? app->session.turn_fallback_context
        : (struct snag_context_choice){app->session.context_mode, app->session.context_tokens};
    const struct snag_model_limit_config *configured = NULL;
    struct snag_model_limit_config configured_values;
    const struct snag_model_limit_config *rule_sources[3];
    const json_t *advertised = NULL;
    struct snag_model_capacity capacity;
    bool ceiling_selection_matches;
    bool ceiling_source_matches;
    char error[256] = {0};
    int rc = -1;

    if (!provider)
        return app_error(app, "selected provider is not present in the current configuration");
    if (snag_app_capacity_resolve(
            app, provider, model, &capacity, error, sizeof(error)) < 0)
        return app_error(app, error[0] ? error : "model capacity could not be resolved");
    if (snag_config_resolve_limits(app->config, provider->name, model,
            &configured_values, rule_sources))
        configured = &configured_values;
    ceiling_selection_matches =
        app->session.capacity_ceiling_valid &&
        strcmp(app->session.capacity_ceiling_provider, provider->name) == 0 &&
        strcmp(app->session.capacity_ceiling_model, model) == 0;
    ceiling_source_matches = ceiling_selection_matches &&
                             capacity_ceiling_matches(app, provider, model);
    if (capacity.source_bound)
        advertised = snag_model_metadata(&app->model_cache, provider, model);
    struct snag_buf text = {.max = 64u * 1024u};
    if (snag_buf_printf(&text,
            "session: %s\n"
            "name: %s\n"
            "state: %s\n"
            "tools: %s\n"
            "provider: %s\n"
            "model: %s\n"
            "effort: %s\n"
            "fast: %s\n"
            "automatic retry: %s (%s)\n"
            "fallback: %s%s\n"
            "cwd: %s\n"
            "turns: %llu\n"
            "queue: %zu%s\n"
            "verbosity: %u\n"
            "context: source=%s",
            id, app->session.name ? app->session.name : "-",
            app->session.active_turn ? "active" : "idle",
            app->session.active_read_only ? "read-only query" : "normal",
            provider->name, model, effort,
            snag_string_in(app->session.service_tier, "priority") ? "ON (priority requested)"
            : app->session.service_tier                           ? "OFF (standard requested)"
                                                                  : "OFF (provider default)",
            snag_app_retry_allowed(app) ? "ON" : "OFF",
            app->session.retry_auto ? "session override" : "configuration", fallback_model(app),
            app->session.turn_fallback_active ? " (serving this turn)" : "", app->session.cwd,
            (unsigned long long)app->session.turn_count, app->session.pending_queue_count,
            app->session.pending_queue_count && !app->session.queue_armed ? " paused" : "",
            snag_ui_verbosity(&app->ui), snag_capacity_source_name(capacity.source)) < 0 ||
        append_capacity_value(
            &text, "hard-input", capacity.hard_input_known, capacity.hard_input_tokens) < 0 ||
        append_capacity_value(&text, "requested-output", capacity.max_output_tokens,
            capacity.max_output_tokens) < 0 ||
        append_compact_threshold(&text, provider, &capacity) < 0)
        goto out;
    if (choice.mode == SNAG_CONTEXT_MODE_TOKENS) {
        if (snag_buf_printf(&text, " · selection=tokens:%llu",
                (unsigned long long)choice.tokens) < 0)
            goto out;
    } else if (choice.mode == SNAG_CONTEXT_MODE_MAX &&
               snag_buf_printf(&text, " · selection=max") < 0)
        goto out;
    if (capacity.effective_context_window_percent &&
        snag_buf_printf(&text, " · effective=%u%%%s", capacity.effective_context_window_percent,
            capacity.effective_context_window_derived ? " (derived client policy)"
                                                      : " (advertised)") < 0)
        goto out;
    if (configured) {
        if (snag_buf_append(&text, "\nconfigured", 11u) < 0 ||
            append_capacity_value(&text, "context", configured->context_window_tokens,
                configured->context_window_tokens) < 0 ||
            append_capacity_value(&text, "max-input", configured->max_input_tokens,
                configured->max_input_tokens) < 0 ||
            append_capacity_value(&text, "max-output", configured->max_output_tokens,
                configured->max_output_tokens) < 0)
            goto out;
    }
    if (append_advertised_capacity(&text, advertised) < 0) goto out;
    if (snag_buf_printf(&text, "\nprovider model: %s",
            snag_config_model_upstream(provider, model)) < 0)
        goto out;
    for (size_t i = 0; i < 3u; ++i) {
        static const char *const fields[] = {"context", "max-input", "max-output"};
        const struct snag_model_limit_config *rule = rule_sources[i];
        if (rule && snag_buf_printf(&text, "\n%s rule: [model-limit %s%s%s]", fields[i],
                        rule->provider, rule->model[0] ? "/" : "", rule->model) < 0)
            goto out;
    }
    if (capacity.cache_source_mismatch &&
        snag_buf_append(&text, "\ncatalog source: mismatch; advertised limits ignored",
            strlen("\ncatalog source: mismatch; advertised limits ignored")) < 0)
        goto out;
    if (app->capacity_cache_error[0] &&
        snag_buf_printf(&text, "\ncatalog: %s", app->capacity_cache_error) < 0)
        goto out;
    if (app->session.capacity_ceiling_valid) {
        if (snag_buf_printf(&text,
                "\nobserved ceiling: hard-input=%llu · provider=%s · model=%s · binding=%s%s",
                (unsigned long long)app->session.capacity_ceiling_input_tokens,
                app->session.capacity_ceiling_provider, app->session.capacity_ceiling_model,
                ceiling_source_matches      ? "current"
                : ceiling_selection_matches ? "source mismatch"
                                            : "different selection",
                ceiling_source_matches ? "" : "; ignored") < 0)
            goto out;
    } else if (snag_buf_append(&text, "\nobserved ceiling: unknown",
                   strlen("\nobserved ceiling: unknown")) < 0) {
        goto out;
    }
    if (snag_buf_printf(&text, "\naccounting: policy=%s · exact-count=%s",
            provider->exact_token_count == SNAG_TOKEN_COUNT_AUTO     ? "auto"
            : provider->exact_token_count == SNAG_TOKEN_COUNT_STRICT ? "exact"
                                                                     : "off",
            capacity.count_capability == SNAG_COUNT_SUPPORTED     ? "supported"
            : capacity.count_capability == SNAG_COUNT_UNSUPPORTED ? "unsupported"
                                                                  : "unknown") < 0)
        goto out;
    if (app->session.context_meter.valid) {
        bool matches = context_meter_matches(
            app, provider, model, resolve_effort(effort));
        if (snag_buf_printf(&text,
                "\nobserved usage: input=%llu tokens · provider=%s · model=%s · effort=%s · %s",
                (unsigned long long)app->session.context_meter.input_tokens,
                app->session.context_meter.provider, app->session.context_meter.model,
                app->session.context_meter.effort,
                matches ? "last measurement; matching selection and compaction"
                        : "historical; different selection or compaction") < 0)
            goto out;
    } else if (snag_buf_append(
                   &text, "\nobserved usage: unknown", strlen("\nobserved usage: unknown")) < 0) {
        goto out;
    }
    if (app->session.usage_totals.responses) {
        const struct snag_usage_totals *totals = &app->session.usage_totals;
        char counted[5][24];
        snag_format_count(counted[0], sizeof(counted[0]), totals->responses);
        snag_format_count(counted[1], sizeof(counted[1]), totals->input_tokens);
        snag_format_count(counted[2], sizeof(counted[2]), totals->cached_input_tokens);
        snag_format_count(counted[3], sizeof(counted[3]), totals->uncached_input_tokens);
        snag_format_count(counted[4], sizeof(counted[4]), totals->output_tokens);
        if (snag_buf_printf(&text,
                "\nsession usage: %s responses · input %s tokens (cached %s, uncached %s) · output "
                "%s",
                counted[0], counted[1], counted[2], counted[3], counted[4]) < 0)
            goto out;
        if (!totals->cached_seen &&
            snag_buf_append(&text,
                "\ncache: the provider reported no cached input tokens in this session",
                strlen("\ncache: the provider reported no cached input tokens in this session")) <
                0)
            goto out;
    }
    if (app->program_usage.responses) {
        const struct snag_usage_totals *program = &app->program_usage;
        char counted[6][24];
        snag_format_count(counted[0], sizeof(counted[0]), program->responses);
        snag_format_count(counted[1], sizeof(counted[1]), program->input_tokens);
        snag_format_count(counted[2], sizeof(counted[2]), program->cached_input_tokens);
        snag_format_count(counted[3], sizeof(counted[3]), program->uncached_input_tokens);
        snag_format_count(counted[4], sizeof(counted[4]), program->output_tokens);
        snag_format_count(counted[5], sizeof(counted[5]), program->total_tokens);
        if (snag_buf_printf(&text,
                "\nprogram usage: %s responses · input %s tokens (cached %s, uncached %s) · output "
                "%s · total %s",
                counted[0], counted[1], counted[2], counted[3], counted[4], counted[5]) < 0)
            goto out;
    }
    if (app->turn_started_ms) {
        uint64_t now = snag_monotonic_ms();
        uint64_t turn_ms = now > app->turn_started_ms ? now - app->turn_started_ms : 0u;
        uint64_t turn_milli =
            snag_rate_microtokens_per_second(app->turn_output_tokens, turn_ms) / 1000u;
        uint64_t last_milli = snag_rate_microtokens_per_second(
                                  app->last_response_output_tokens, app->last_response_ms) /
                              1000u;
        char turn[96], last[64];
        if (turn_ms < 1000u)
            (void)snprintf(turn, sizeof(turn), "too early to rate");
        else
            (void)snprintf(turn, sizeof(turn), "%llu.%01llu tok/s over %llu.%01llu s",
                (unsigned long long)(turn_milli / 1000u),
                (unsigned long long)((turn_milli % 1000u) / 100u),
                (unsigned long long)(turn_ms / 1000u),
                (unsigned long long)((turn_ms % 1000u) / 100u));
        (void)snprintf(last, sizeof(last), "%llu.%01llu tok/s",
            (unsigned long long)(last_milli / 1000u),
            (unsigned long long)((last_milli % 1000u) / 100u));
        if (snag_buf_printf(&text, "\nspeed: this turn %s · last response %s", turn, last) < 0)
            goto out;
    }
    if (snag_buf_printf(&text, "\nmax_parallel_commands: %u\nparallel_tool_calls: %s",
            app->session.active_turn ? app->session.max_parallel_commands
                                     : app->config->max_parallel_commands,
            (app->session.active_turn ? app->session.parallel_tool_calls
                                      : provider->parallel_tool_calls)
                ? "true"
                : "false") < 0)
        goto out;
    if (app->irc &&
        (snag_buf_putc(&text, '\n') < 0 || snag_irc_state(app->irc, &text, NULL, 0u) < 0))
        goto out;
    if (snag_buf_terminate(&text) < 0) goto out;
    rc = snag_app_report(app, SNAG_UI_HOST, (const char *)text.data);
out:
    snag_buf_free(&text);
    return rc;
}
static bool page_reference(struct app_state *app, const char *text, size_t length);

int
snag_app_help_text(struct snag_buf *text, const char *command)
{
    static const char legend[] = "Syntax: [optional], A|B alternatives, UPPERCASE values.\n";
    static const char settings[] =
        "\nNotes\n"
        "Model/effort: next response onward, until changed; save (s) also writes config.\n"
        "Omitted model effort: highest cached effort/default, then current effort.\n"
        "ENDPOINT: host[:port] or [IPv6][:port]; IPv6 brackets literal; port 6667.\n"
        "Queue: idle adds paused; active adds armed; N is the displayed position.\n"
        "Queue TEXT may begin /ro for read-only work; //TEXT escapes a leading slash.\n";
    static const char keys[] =
        "\nKeyboard\n"
        "Keys: blank Enter new prompt; Enter submit/steer (chat: send).\n"
        "Empty Tab switch view/channel/query; Shift-Tab reverse.\n"
        "Tab complete/indent/queue (chat: @nick); query tabs: chat Tab switches.\n"
        "Ctrl-C cancel draft or interrupt; empty Ctrl-D exit, otherwise delete.\n"
        "Ctrl-J newline; Up/Down history; Ctrl-R search; Ctrl-L redraw.\n"
        "Verbosity: 0 conversation; 1 tool rows; 2 previews; 3 retained tools;\n"
        "4 debug; 5 redacted protocol; 6 wire. Traces appear in rollout.\n"
        "Full reference: man snajpagent (snajpagent --help).\n";
    static const struct {
        const char *first;
        const char *title;
    } sections[] = {
        {"/help", "Help and settings"},
        {"/model [list|cache]", "Models and context"},
        {"/state", "Goals and queued work"},
        {"/session", "Session history"},
        {"/cat PATH", "Files and media"},
        {"/chat [ADDRESS]", "Network chat"},
    };
    size_t prefix = command ? strlen(command) : 0u;

    if (snag_buf_append(text, legend, sizeof(legend) - 1u) < 0) return -1;
    for (size_t i = 0u; i < snag_command_count(); ++i) {
        if (command &&
            (strncmp(snag_commands[i].syntax, command, prefix) ||
                (snag_commands[i].syntax[prefix] && snag_commands[i].syntax[prefix] != ' ')))
            continue;
        if (!command) {
            for (size_t j = 0u; j < sizeof(sections) / sizeof(sections[0]); ++j) {
                if (!strcmp(snag_commands[i].syntax, sections[j].first) &&
                    snag_buf_printf(text, "\n%s\n", sections[j].title) < 0) {
                    return -1;
                }
            }
        }
        if (snag_buf_printf(
                text, "%s — %s\n", snag_commands[i].syntax, snag_commands[i].description) < 0) {
            return -1;
        }
    }
    if (!command && (snag_buf_append(text, settings, sizeof(settings) - 1u) < 0 ||
                        snag_buf_append(text, keys, sizeof(keys) - 1u) < 0)) {
        return -1;
    }
    return snag_buf_terminate(text);
}

int
snag_app_help(struct app_state *app, const char *command)
{
    struct snag_buf text = {.max = 64u * 1024u};
    int rc = snag_app_help_text(&text, command);
    if (!rc && (app->command_report || !page_reference(app, (const char *)text.data, text.len))) {
        rc = snag_app_report(app, SNAG_UI_HELP, (const char *)text.data);
    }
    snag_buf_free(&text);
    return rc;
}
static int
show_setting(struct app_state *app, const char *name, const char *value)
{
    return app_reportf(app, "%s for next turn: %s (until changed)", name, value);
}
static int
refresh_model_cache(struct app_state *app, char *error, size_t error_size)
{
    json_t *providers = json_array();
    bool applying = app->applying_controls;
    int rc = -1;

    if (!providers) return snag_errno(ENOMEM);
    app->applying_controls = true;
    if (!app->session.active_turn && !app->input_closed) {
        app->interrupt_requested = false;
        app->steering_requested = false;
    }
    if (app->ui.opened && set_input_prompt(app, true) < 0) goto out;
    for (size_t i = 0; i < app->config->provider_count; ++i) {
        const struct snag_provider_config *provider = &app->config->providers[i];
        json_t *models = NULL;
        json_t *entry = NULL;
        char detail[256] = {0};

        if (snag_app_provider_activity(app, true) < 0) goto out;
        int model_rc = snag_app_provider_models(app, provider, &models, detail, sizeof(detail));
        if (snag_app_provider_activity(app, false) < 0) {
            json_decref(models);
            goto out;
        }
        if (model_rc != 0) {
            if (model_rc > 0) {
                snag_errorf(
                    error, error_size, "model discovery interrupted; previous cache retained");
                goto out;
            }
            snag_errorf(error, error_size, "cannot refresh provider %s: %s", provider->name,
                detail[0] ? detail : strerror(errno));
            goto out;
        }
        entry = json_pack("{s:o,s:s,s:s,s:s}", "models", models, "base_url", provider->base_url,
            "name", provider->name, "protocol", snag_provider_catalog_protocol(provider));
        if (!entry || json_array_append_new(providers, entry) < 0) {
            (void)snag_fail(error, error_size, ENOMEM, "cannot assemble model cache");
            goto out;
        }
    }
    if (snag_model_cache_replace(
            &app->store, providers, snag_time_ms(), &app->model_cache, error, error_size) < 0)
        goto out;
    app->capacity_cache_error[0] = '\0';
    rc = 0;
out:
    json_decref(providers);
    app->applying_controls = applying;
    if (app->ui.opened && set_input_prompt(app, app->session.active_turn) < 0) return -1;
    return rc;
}
static int
load_model_cache(struct app_state *app, bool refresh, char *error, size_t error_size)
{
    int rc;
    if (refresh) return refresh_model_cache(app, error, error_size);
    rc = snag_model_cache_reload_if_changed(&app->store, &app->model_cache, error, error_size);
    if (rc == 0) app->capacity_cache_error[0] = '\0';
    if (rc == 1) {
        if (app->model_cache.providers) return 0;
        for (size_t i = 0; i < app->config->provider_count; ++i)
            if (app->config->providers[i].model_count)
                return 0; /* Configured models do not require discovery. */
        return snag_fail(error, error_size, ENOENT, "model cache is empty; use /model cache");
    }
    return rc;
}
static int
append_catalog_limits(struct snag_buf *text, const json_t *model)
{
    const json_t *limits = model ? json_object_get(model, "limits") : NULL;
    const char *count = model ? snag_json_string(model, "count_capability") : NULL;
    static const struct {
        const char *key;
        const char *label;
    } fields[] = {{"context_window_tokens", "context"},
        {"max_context_window_tokens", "max-context"}, {"max_input_tokens", "input"},
        {"max_output_tokens", "output"}};
    bool any = false;

    if (!json_is_object(limits)) return 0;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        uint64_t value;
        if (snag_json_integer_u64((json_t *)limits, fields[i].key, &value) < 0) continue;
        if (snag_buf_printf(text, "%s%s=%llu", any ? "," : " · ", fields[i].label,
                (unsigned long long)value) < 0)
            return -1;
        any = true;
    }
    if (count && snag_buf_printf(text, " · count=%s", count) < 0) return -1;
    return 0;
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
        if (limits.context_window_tokens && append_capacity_value(view->text, "configured-context",
                                                true, limits.context_window_tokens) < 0)
            return -1;
        if (limits.max_input_tokens && append_capacity_value(view->text, "configured-input", true,
                                           limits.max_input_tokens) < 0)
            return -1;
        if (limits.max_output_tokens && append_capacity_value(view->text, "configured-output", true,
                                            limits.max_output_tokens) < 0)
            return -1;
    }
    return 0;
}

static void service_external(void *opaque);
static int suspend_external(void *opaque);

static int
start_pager(
    struct app_state *app, const char *command, const char *path, const char *text, size_t length)
{
    char error[256] = {0};
    if (app->pager) return snag_errno(EBUSY);
    char *report = text ? snag_strdup_checked(text, length) : NULL;
    if (text && !report) return -1;
    if (snag_ui_external(&app->ui, true, error, sizeof(error)) < 0) {
        free(report);
        return -1;
    }
    app->pager =
        snag_pager_start(command, path, text, length, app->ui.native ? &app->ui.profile : NULL);
    if (!app->pager) {
        int failure = errno;
        (void)snag_ui_external(&app->ui, false, error, sizeof(error));
        free(report);
        return snag_errno(failure);
    }
    app->pager_report = report;
    return 0;
}

static int
service_pager(struct app_state *app)
{
    if (!app->pager) return 0;
    bool shown = false;
    char error[256] = {0};
    int rc = snag_pager_poll(app->pager, &shown, suspend_external, app), failure = errno;
    if (!rc) return 0;
#if SNAJPAGENT_VM
    bool terminal = app->view_terminal && app->view_terminal->pager == app->pager;
    struct snag_buf *previous_report = app->ui.command_report;
    bool previous_passthrough = app->ui.command_report_passthrough;
    bool previous_error = app->ui.command_error;
    if (terminal) {
        app->view_terminal->pager = NULL;
        app->ui.command_report = &app->view_terminal->report;
        app->ui.command_report_passthrough = true;
        app->ui.command_error = false;
    }
#endif /* SNAJPAGENT_VM */
    snag_pager_close(app->pager);
    app->pager = NULL;
    char *report = app->pager_report;
    app->pager_report = NULL;
    int restored = snag_ui_external(&app->ui, false, error, sizeof(error));
    if (restored == 0 && (!shown || rc < 0)) {
        if (report)
            restored = snag_ui_text(&app->ui, SNAG_UI_HELP, report);
        else
            restored = app_textf(app, SNAG_UI_ERROR, "cannot run pager: %s",
                rc < 0 ? strerror(failure) : "command could not start");
    }
    if (shown && report && app->ui.observe)
        app->ui.observe(app->ui.observe_opaque, "help", report, NULL);
    free(report);
#if SNAJPAGENT_VM
    if (terminal) {
        app->view_terminal->failed |= rc < 0 || !shown || restored < 0 || app->ui.command_error;
        app->ui.command_report = previous_report;
        app->ui.command_report_passthrough = previous_passthrough;
        app->ui.command_error = previous_error;
        if (view_terminal_finish(app) < 0) restored = -1;
    }
#endif
    return restored;
}

/* The configured pager command, or NULL for direct reference display. */
static const char *
pager_command(const struct app_state *app)
{
    const char *value = app->config->pager;
    const char *pager;

    if (!*value || strcmp(value, "off") == 0) return NULL;
    if (strcmp(value, "on") != 0) return value;
    pager = getenv("PAGER");
    if (!pager || !strcmp(pager, "less")) return snag_default_pager();
    return *pager ? pager : NULL;
}

#if SNAJPAGENT_VM
static bool
view_file_cancel(void *opaque)
{
    return snag_app_active_input_pump(opaque, 0u) != 0;
}
#endif

/* /cat is operator-only: classic display uses the pager; a semantic command
 * keeps an immutable snapshot. Neither path adds file bytes to model context. */
static int
page_local_file(struct app_state *app, const char *argument)
{
    const char *command;
    char *input = NULL;
    char *path = NULL;
    char *resolved = NULL;
    snag_file_info info;
    size_t length;
    int fd;
    int rc;
    int saved;
#if SNAJPAGENT_VM
    struct app_view_command *view = app->view_command;
#else
    const void *view = NULL;
#endif

    while (isspace((unsigned char)*argument)) ++argument;
    if (!*argument) return app_error(app, "usage: /cat PATH");
    command = pager_command(app);
    if (!view && !command) {
        return app_error(app, !strcmp(app->config->pager, "off") || !*app->config->pager
                                  ? "pager is off; set [ui] pager to on or a command"
                                  : "no pager is available; set $PAGER or configure [ui] pager");
    }
    if (!view && snag_isatty(STDERR_FILENO) != 1) {
        return app_error(app, "/cat needs an interactive terminal");
    }
    if (!(input = strdup(argument))) {
        return app_error(app, "cannot read path: out of memory");
    }
    length = strlen(input);
    while (length && isspace((unsigned char)input[length - 1u])) input[--length] = '\0';
    if (length >= 2u && (input[0] == '\'' || input[0] == '"') && input[length - 1u] == input[0]) {
        memmove(input, input + 1u, length - 2u);
        input[length - 2u] = '\0';
    }
    if (!*input) {
        rc = app_error(app, "usage: /cat PATH");
        goto out;
    }

    if (input[0] == '~' && (input[1] == '/' || input[1] == '\\')) {
        char *home = snag_home_directory();
        if (!home) {
            rc = app_error(app, "home directory is unavailable");
            goto out;
        }
        path = snag_path_join(home, input + 2u);
        free(home);
    } else if (snag_path_root_len(input)) {
        path = strdup(input);
    } else {
        path = snag_path_join(app->session.cwd, input);
    }
    if (!path) {
        rc = app_textf(app, SNAG_UI_ERROR, "cannot read path: %s", strerror(errno));
        goto out;
    }
    resolved = snag_realpath(path);
    if (!resolved) {
        rc = app_textf(app, SNAG_UI_ERROR, "cannot open file: %s", strerror(errno));
        goto out;
    }
    fd = snag_open_read(resolved, false);
    if (fd < 0) {
        saved = errno;
        rc = app_textf(app, SNAG_UI_ERROR, "cannot open file: %s", strerror(saved));
        goto out;
    }
    rc = snag_fstat(fd, &info);
    saved = errno;
    if (rc < 0 || !S_ISREG(info.st_mode)) {
        (void)close(fd);
        const char *reason = rc < 0 ? strerror(saved) : "not a regular file";
        rc = app_textf(app, SNAG_UI_ERROR, "cannot open file: %s", reason);
        goto out;
    }
#if SNAJPAGENT_VM
    if (view) {
        if (!app->session.active_turn) app->interrupt_requested = false;
        view->snapshot = snag_vm_report_file(app->session.dir_fd, view->line,
            app->command_report->data, app->command_report->len, fd, view_file_cancel, app);
        saved = errno;
        (void)close(fd);
        rc = view->snapshot ? 0
                            : app_textf(app, SNAG_UI_ERROR, "cannot retain file snapshot: %s",
                                  strerror(saved));
        goto out;
    }
#endif /* SNAJPAGENT_VM */
    (void)close(fd);
    rc = start_pager(app, command, resolved, NULL, 0u);
    saved = errno;
    if (rc < 0) {
        rc = app_textf(app, SNAG_UI_ERROR, "cannot run pager: %s", strerror(saved));
    }
out:
    free(input);
    free(path);
    free(resolved);
    return rc;
}

/* Reference text shares terminal ownership and fallback with the model catalogue. */
static bool
page_reference(struct app_state *app, const char *text, size_t length)
{
    const char *command = pager_command(app);

    if (!command || app->execute || !app->ui.opened || app->ui.input_interface ||
        snag_isatty(STDIN_FILENO) != 1 || snag_isatty(STDERR_FILENO) != 1) {
        return false;
    }
    return start_pager(app, command, NULL, text, length) == 0;
}

/* Format the catalogue timestamp for the refresh report and the listing footer. */
static void
cache_timestamp(char *buffer, size_t size, uint64_t updated_at_ms)
{
    time_t seconds = (time_t)(updated_at_ms / 1000u);
    struct tm broken;

    if (snag_gmtime(&seconds, &broken) &&
        strftime(buffer, size, "%Y-%m-%dT%H:%M:%SZ", &broken) != 0u)
        return;
    (void)snprintf(buffer, size, "%llu ms since epoch", (unsigned long long)updated_at_ms);
}

static int
report_cache_updated(struct app_state *app)
{
    char timestamp[64];

    cache_timestamp(timestamp, sizeof(timestamp), app->model_cache.updated_at_ms);
    return app_textf(app, SNAG_UI_HOST, "cache updated: %s", timestamp);
}

static int
render_model_catalog(struct app_state *app)
{
    const struct snag_provider_config *selected = next_provider(app);
    struct snag_model_capacity capacity;
    struct snag_buf text;
    char error[256] = {0};
    char timestamp[64];
    struct model_catalog_view view = {app->config, &text};
    int rc = -1;

    if (!selected)
        return app_error(app, "selected provider is not present in the current configuration");
    if (snag_app_capacity_resolve(
            app, selected, app->session.default_model, &capacity, error, sizeof(error)) < 0)
        return app_error(app, error);
    snag_buf_init(&text, 16u * 1024u * 1024u);
    if (snag_buf_printf(&text, "selected: %s / %s / %s", selected->name, app->session.default_model,
            resolve_effort(app->session.default_effort)
                ? resolve_effort(app->session.default_effort)
                : app->session.default_effort) < 0 ||
        append_compact_threshold(&text, selected, &capacity) < 0)
        goto out;
    if (append_capacity_value(&text, "effective-context", capacity.context_window_tokens,
            capacity.context_window_tokens) < 0 ||
        append_capacity_value(
            &text, "hard-input", capacity.hard_input_known, capacity.hard_input_tokens) < 0 ||
        snag_model_each(&app->model_cache, app->config,
            resolve_effort(app->config->reasoning_effort), append_model_row, &view) < 0)
        goto out;
    cache_timestamp(timestamp, sizeof(timestamp), app->model_cache.updated_at_ms);
    if (snag_buf_printf(&text, "\ncache updated: %s", timestamp) < 0) goto out;
    if (snag_buf_terminate(&text) < 0) goto out;
    if (!app->command_report && page_reference(app, (const char *)text.data, text.len))
        rc = 0;
    else
        rc = snag_app_report(app, SNAG_UI_HOST, (const char *)text.data);
out:
    snag_buf_free(&text);
    return rc;
}
static bool
parse_model_index(const char *value, size_t *index)
{
    size_t number = 0u;
    const unsigned char *p = (const unsigned char *)value;
    if (*p == '#') ++p;
    if (!*p) return false;
    for (; *p; ++p) {
        size_t digit;
        if (*p < '0' || *p > '9') return false;
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
    while (*part && isspace((unsigned char)*part)) ++part;
    end = part + strlen(part);
    while (end > part && isspace((unsigned char)end[-1])) --end;
    *end = '\0';
    return part;
}
static int
change_retry_auto(struct app_state *app, const char *argument)
{
    bool enabled = snag_app_retry_allowed(app);

    if (*argument && strcmp(argument, "on") && strcmp(argument, "off"))
        return app_error(app, "usage: /retry auto [on|off]");
    enabled = *argument ? !strcmp(argument, "on") : !enabled;
    const char *value = enabled ? "on" : "off";
    if (!app->session.retry_auto || strcmp(app->session.retry_auto, value)) {
        char error[256] = {0};
        if (commit_event(app, "retry_auto_changed", json_pack("{s:s}", "value", value), error,
                sizeof(error)) < 0)
            return app_error(app, error), -1;
    }
    snag_app_irc_summary_retry_policy(app);
    return app_textf(
        app, SNAG_UI_HOST, "Automatic retry: %s (session override)", enabled ? "ON" : "OFF");
}

static int
change_fast(struct app_state *app, const char *argument)
{
    const char *current = app->session.service_tier;
    bool enabled = snag_string_in(current, "priority");
    if (argument && strcmp(argument, "on") && strcmp(argument, "off") && strcmp(argument, "status"))
        return app_error(app, "usage: /fast [on|off|status]");
    if (!argument || strcmp(argument, "status")) {
        enabled = argument ? !strcmp(argument, "on") : !enabled;
        const char *tier = enabled ? "priority" : "default";
        if (!current || strcmp(current, tier)) {
            char error[256] = {0};
            if (commit_event(app, "service_tier_changed", json_pack("{s:s}", "value", tier), error,
                    sizeof(error)) < 0)
                return app_error(app, error), -1;
        }
    }
    return app_textf(app, SNAG_UI_HOST, "Fast mode: %s (%s); model and effort unchanged",
        enabled ? "ON" : "OFF",
        enabled                     ? "priority for subsequent requests"
        : app->session.service_tier ? "standard for subsequent requests"
                                    : "provider default");
}

static int
record_model_selection(struct app_state *app, const char *provider, const char *model,
    const char *effort, char *error, size_t error_size)
{
    if (strcmp(app->session.default_provider, provider) == 0 &&
        strcmp(app->session.default_model, model) == 0 &&
        strcmp(app->session.default_effort, effort) == 0)
        return 0;
    return commit_event(app, "model_selection_changed",
        json_pack("{s:s,s:s,s:s,s:s,s:s,s:s}", "new_effort", effort, "new_model", model,
            "new_provider", provider, "old_effort", app->session.default_effort, "old_model",
            app->session.default_model, "old_provider", app->session.default_provider),
        error, error_size);
}

static int
record_context_selection(struct app_state *app, const struct snag_context_choice *choice,
    char *error, size_t error_size)
{
    if (!choice || (choice->mode == app->session.context_mode &&
                       choice->tokens == app->session.context_tokens))
        return 0;
    if (commit_event(app, "context_selection_changed",
            json_pack("{s:s,s:I,s:s,s:I}", "new_mode", snag_context_mode_name(choice->mode),
                "new_tokens", (json_int_t)choice->tokens, "old_mode",
                snag_context_mode_name(app->session.context_mode), "old_tokens",
                (json_int_t)app->session.context_tokens),
            error, error_size) < 0)
        return -1;
    if (app->session.active_turn) {
        /* Retain the serving model's capacity until its replacement starts. */
        if (app->turn_provider && app->turn_model &&
            !strcmp(app->turn_provider->name, app->session.default_provider) &&
            !strcmp(app->turn_model, app->session.default_model) &&
            snag_app_capacity_resolve(app, app->turn_provider, app->turn_model,
                &app->turn_capacity, error, error_size) < 0)
            return -1;
        app->model_switch_requested = true;
    }
    return 0;
}

static int report_context(struct app_state *app, const struct snag_provider_config *provider,
    const struct snag_context_choice *choice);

static int
commit_model_selection(struct app_state *app, const struct snag_provider_config *provider,
    const char *model, const char *effort, bool known_in_cache, bool save,
    const struct snag_context_choice *choice)
{
    char error[256] = {0};
    bool current_turn = app->session.active_turn;
    int rc;

    if (choice) {
        struct snag_model_capacity capacity;
        if (save && choice->mode != SNAG_CONTEXT_MODE_TOKENS)
            return app_error(app, "saving a context default requires an explicit token count");
        if (snag_app_context_preview(app, provider, model, choice, &capacity,
                error, sizeof(error)) < 0)
            return app_error(app, error);
    }
    if (save && choice) {
        if (snag_config_save_model_context(app->config, app->config_path,
                app->config_allow_create, provider->name, model, effort, choice->tokens,
                error, sizeof(error)) < 0)
            return app_error(app, error[0] ? error : "model settings could not be saved");
    } else if (save) {
        snag_file_info st;
        bool missing =
            app->config_allow_create && snag_lstat(app->config_path, &st) < 0 && errno == ENOENT;
        int save_rc = missing ? snag_config_save_provider(app->config_path, true, provider, model,
                                    effort, error, sizeof(error))
                              : snag_config_save_model(app->config_path, app->config_allow_create,
                                    provider->name, model, effort, error, sizeof(error));
        if (save_rc < 0)
            return app_error(
                app, error[0] ? error : "model settings could not be written to configuration");
    }
    if (record_model_selection(app, provider->name, model, effort, error, sizeof(error)) < 0) {
        (void)app_error(app, error[0] ? error : "model selection could not be saved");
        return -1;
    }
    if (record_context_selection(app, choice, error, sizeof(error)) < 0) {
        (void)app_error(app, error[0] ? error : "context selection could not be saved");
        return -1;
    }
    if (current_turn &&
        (app->session.turn_fallback_active ||
            strcmp(app->session.active_turn_provider, app->session.default_provider) != 0 ||
            strcmp(app->session.active_turn_model, app->session.default_model) != 0 ||
            strcmp(app->session.active_turn_effort, resolve_effort(app->session.default_effort)) !=
                0))
        app->model_switch_requested = true;
    if (save) {
        (void)snprintf(app->config->provider, sizeof(app->config->provider), "%s", provider->name);
        (void)snprintf(app->config->model, sizeof(app->config->model), "%s", model);
        (void)snprintf(
            app->config->reasoning_effort, sizeof(app->config->reasoning_effort), "%s", effort);
    }
    rc = app_textf(app, SNAG_UI_HOST, "model for %s: %s / %s / %s (until changed)",
        current_turn ? "next response in this turn" : "next turn", provider->name, model, effort);
    if (rc < 0) return rc;
    if (!known_in_cache &&
        app_warning(app,
            "model is not known in the model cache; the configured provider will still be used") <
            0)
        return -1;
    if (choice && report_context(app, provider, choice) < 0) return -1;
    return save ? app_textf(app, SNAG_UI_HOST, "configuration saved: %s", app->config_path) : 0;
}
static int
select_cached_model(struct app_state *app, const char *value, bool save,
    const struct snag_context_choice *choice)
{
    const struct snag_provider_config *provider_config;
    const char *provider;
    const char *model;
    const char *effort;
    char error[256] = {0};
    size_t index;
    int entry_rc;

    if (!parse_model_index(value, &index)) return 1;
    if (load_model_cache(app, false, error, sizeof(error)) < 0) return app_error(app, error);
    entry_rc = snag_model_entry(&app->model_cache, app->config, index,
        resolve_effort(app->config->reasoning_effort), &provider, &model, &effort);
    if (entry_rc != 0) return app_error(app, "model index is not in the displayed cache");
    provider_config = snag_config_provider(app->config, provider);
    if (!provider_config)
        return app_error(app, "cached provider is not configured; use /model cache");
    return commit_model_selection(app, provider_config, model, effort, true, save, choice);
}
static int
select_typed_model(struct app_state *app, char *value, bool save,
    const struct snag_context_choice *choice)
{
    const struct snag_provider_config *provider;
    const char *model;
    const char *effort;
    char *parts[3];
    size_t count;
    int split;
    bool known_in_cache = false;

    split = snag_model_split_selector(value, parts);
    if (split < 0) {
        (void)app_error(
            app, split == -2 ? "model selector has mismatched quotes; wrap a whole component in "
                               "matching '...' or \"...\""
                             : "model selector has more than three slash-separated components; "
                               "quote a component or use a cached number for IDs containing slash");
        return -1;
    }
    count = (size_t)split;
    for (size_t i = 0; i < count; ++i) {
        parts[i] = trim_selector_part(parts[i]);
        if (!parts[i][0]) {
            (void)app_error(app, "model selector contains an empty component");
            return -1;
        }
    }
    provider = count == 3u ? snag_config_provider(app->config, parts[0]) : next_provider(app);
    model = parts[count == 3u ? 1u : 0u];
    if (count == 1u) {
        /* A quoted vendor/model designator keeps its slash; a configured
         * leading name still selects the provider. */
        char *slash = strchr(model, '/');
        if (slash) {
            *slash = '\0';
            const struct snag_provider_config *named = snag_config_provider(app->config, model);
            if (named) {
                provider = named;
                model = slash + 1u;
            } else {
                *slash = '/';
            }
        }
        if (!model[0]) {
            (void)app_error(app, "model selector contains an empty component");
            return -1;
        }
    }
    if (!provider) {
        (void)app_error(app, count == 3u ? "model selector names an unconfigured provider"
                                         : "no provider is configured");
        return -1;
    }
    effort = count >= 2u ? parts[count - 1u] : app->session.default_effort;
    if (strlen(model) >= SNAG_CONFIG_MODEL_MAX ||
        !snag_utf8_valid((const unsigned char *)model, strlen(model), true) ||
        !resolve_effort(effort)) {
        (void)app_error(app, "model or effort exceeds the supported structural bounds");
        return -1;
    }
    {
        char ignored[256] = {0};
        const json_t *cached = NULL;
        (void)load_model_cache(app, false, ignored, sizeof(ignored));
        cached = snag_model_metadata(&app->model_cache, provider, model);
        known_in_cache = cached != NULL;
        if (count == 1u)
            effort = snag_model_best_effort(
                app->config, provider->name, model, cached, resolve_effort(effort));
    }
    return commit_model_selection(
        app, provider, model, resolve_effort(effort), known_in_cache, save, choice);
}

static bool
strip_model_save_suffix(char *selector)
{
    char *end = selector + strlen(selector);
    char *word;
    size_t len;

    while (end > selector && isspace((unsigned char)end[-1])) --end;
    word = end;
    while (word > selector && !isspace((unsigned char)word[-1])) --word;
    len = (size_t)(end - word);
    if (word == selector ||
        !((len == 1u && word[0] == 's') || (len == 4u && memcmp(word, "save", 4u) == 0)))
        return false;
    while (word > selector && isspace((unsigned char)word[-1])) --word;
    *word = '\0';
    return true;
}

static int
resolve_fallback(struct app_state *app, const char *value, struct snag_model_selection *selected,
    char *error, size_t error_size)
{
    struct snag_model_capacity capacity;
    char cache_error[256] = {0};
    (void)load_model_cache(app, false, cache_error, sizeof(cache_error));
    if (snag_model_select_selector(&app->model_cache, app->config, value, next_provider(app),
            resolve_effort(app->session.default_effort), selected, error, error_size) < 0)
        return -1;
    if (!selected->context_set)
        selected->context = (struct snag_context_choice){SNAG_CONTEXT_MODE_DEFAULT, 0u};
    const char *effort = resolve_effort(selected->effort);
    if (!effort) return snag_fail(error, error_size, EINVAL, "invalid fallback effort");
    if (effort != selected->effort)
        (void)snag_strcpy(selected->effort, sizeof(selected->effort), effort);
    return snag_app_context_preview(app, selected->provider, selected->model,
        &selected->context, &capacity, error, error_size);
}

static int
change_fallback(struct app_state *app, const char *argument)
{
    char copy[SNAG_CONFIG_SELECTOR_MAX + 8u];
    char value[SNAG_CONFIG_SELECTOR_MAX];
    char error[256] = {0};
    struct snag_model_selection selected = {0};

    if (!argument)
        return app_textf(app, SNAG_UI_HOST, "Fallback: %s%s", fallback_model(app),
            app->session.turn_fallback_active ? " (serving this turn)" : "");
    if (!snag_strcpy(copy, sizeof(copy), argument))
        return app_error(app, "fallback selector is too long");
    char *selector = trim_selector_part(copy);
    bool save = strip_model_save_suffix(selector);
    if (!strcmp(selector, "off")) {
        memcpy(value, "off", sizeof("off"));
    } else {
        if (resolve_fallback(app, selector, &selected, error, sizeof(error)) < 0)
            return app_error(app, error);
        char context[32] = "";
        if (selected.context_set) {
            if (selected.context.mode == SNAG_CONTEXT_MODE_TOKENS)
                (void)snprintf(context, sizeof(context), ":%llu",
                    (unsigned long long)selected.context.tokens);
            else
                (void)snprintf(context, sizeof(context), ":%s",
                    snag_context_mode_name(selected.context.mode));
        }
        char model_quote = strchr(selected.model, '"') ? '\'' : '"';
        char effort_quote = strchr(selected.effort, '"') ? '\'' : '"';
        if (strchr(selected.model, model_quote) || strchr(selected.effort, effort_quote))
            return app_error(app, "fallback components cannot contain both quote characters");
        int n = snprintf(value, sizeof(value), "%s/%c%s%c/%c%s%c%s", selected.provider->name,
            model_quote, selected.model, model_quote, effort_quote, selected.effort, effort_quote,
            context);
        if (n < 0 || (size_t)n >= sizeof(value))
            return app_error(app, "fallback selector too long");
    }
    if (save && snag_config_save_fallback(app->config, app->config_path,
            app->config_allow_create, value, error, sizeof(error)) < 0)
        return app_error(app, error);
    if (strcmp(app->session.fallback_model, value) &&
        commit_event(app, "fallback_changed", json_pack("{s:s}", "value", value), error,
            sizeof(error)) < 0)
        return app_error(app, error), -1;
    if (app_textf(app, SNAG_UI_HOST, "Fallback: %s%s", value,
            !strcmp(value, "off") ? "" : " (automatic on terminal cyber_policy errors)") < 0)
        return -1;
    return save ? app_textf(app, SNAG_UI_HOST, "configuration saved: %s", app->config_path) : 0;
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
        if (!copy) return app_error(app, "model selector is too long");
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
        if (active || (!app->applying_controls && !app->session.pending_log))
            rc = request_control(app, SNAG_CONTROL_CACHE, "/model cache");
        else if (load_model_cache(app, true, error, sizeof(error)) < 0)
            rc = app_error(app, error);
        else
            rc = report_cache_updated(app);
        free(copy);
        return rc;
    }
    save = strip_model_save_suffix(selector);
    struct snag_context_choice choice = {0};
    int context_set = snag_model_context_suffix(selector, &choice, error, sizeof(error));
    if (context_set < 0) {
        free(copy);
        return app_error(app, error);
    }
    selector = trim_selector_part(selector);
    rc = select_cached_model(app, selector, save, context_set ? &choice : NULL);
    if (rc == 1)
        rc = select_typed_model(app, selector, save, context_set ? &choice : NULL);
    free(copy);
    return rc;
}

struct model_tool_rows {
    const struct snag_model_cache *cache;
    const struct snag_config *config;
    const struct snag_model_selection *selected;
    struct snag_buf *listing;
    bool found;
    bool truncated;
};

static int
model_tool_cached_row(void *opaque, size_t index, const char *provider, const char *model,
    const char *effort, const json_t *metadata)
{
    struct model_tool_rows *rows = opaque;
    const struct snag_provider_config *configured = snag_config_provider(rows->config, provider);
    char line[SNAG_CONFIG_PROVIDER_NAME_MAX + SNAG_CONFIG_MODEL_MAX + SNAG_CONFIG_EFFORT_MAX + 48u];
    int length;

    if (!configured || !metadata || snag_model_metadata(rows->cache, configured, model) != metadata)
        return 0;
    if (rows->selected) {
        rows->found = !strcmp(rows->selected->provider->name, provider) &&
                      !strcmp(rows->selected->model, model) &&
                      !strcmp(rows->selected->effort, effort);
        return rows->found ? 1 : 0;
    }
    length = snprintf(line, sizeof(line), "\n%zu. %s / %s / %s", index, provider, model, effort);
    if (length < 0 || (size_t)length >= sizeof(line)) return -1;
    if (rows->listing->len + (size_t)length + 96u > rows->listing->max) {
        rows->truncated = true;
        return 1;
    }
    return snag_buf_append(rows->listing, line, (size_t)length) < 0 ? -1 : 0;
}

int
snag_app_select_model_tool(struct app_state *app, const struct snag_response_item *call,
    json_t **result, char *error, size_t error_size)
{
    const char *selector = NULL;
    const struct snag_provider_config *fallback = next_provider(app);
    struct snag_model_selection selected = {0};
    struct model_tool_rows rows = {0};
    struct snag_buf message = {.max = SNAG_CONFIG_MODEL_MAX + SNAG_CONFIG_PROVIDER_NAME_MAX +
                                      SNAG_CONFIG_EFFORT_MAX + 128u};

    *result = NULL;
    if (!app->config->allow_model_change) {
        *result = snag_tool_result_terminal(false,
            "select_model is disabled; enable [agent] allow_model_change to permit model changes");
        return *result ? 0 : -1;
    }
    if (!snag_json_arg_keys(call->arguments, "selector", "", error, error_size) ||
        !snag_json_arg_text(call->arguments, "selector", 1u, SNAG_CONFIG_PATH_MAX, false, &selector,
            error, error_size)) {
        *result = snag_tool_result_terminal(false, error);
        return *result ? 0 : -1;
    }
    if (!strcmp(selector, "cache")) {
        struct snag_buf listing = {.max = 64u * 1024u};

        if (refresh_model_cache(app, error, error_size) < 0) {
            *result =
                snag_tool_result_terminal(false, error[0] ? error : "model cache refresh failed");
            return *result ? 0 : -1;
        }
        rows = (struct model_tool_rows){
            .cache = &app->model_cache, .config = app->config, .listing = &listing};
        if (snag_buf_printf(&listing, "model cache updated; available cached rows:") < 0 ||
            snag_model_each(&app->model_cache, app->config, app->session.default_effort,
                model_tool_cached_row, &rows) < 0 ||
            (rows.truncated && snag_buf_printf(&listing, "\n... more rows omitted") < 0) ||
            snag_buf_terminate(&listing) < 0) {
            snag_buf_free(&listing);
            return snag_errorf(error, error_size, "cache updated, but model listing failed");
        }
        *result = snag_tool_result_terminal(true, (const char *)listing.data);
        snag_buf_free(&listing);
        return *result ? 0 : -1;
    }
    if (!app->model_cache.providers) {
        snag_errorf(
            error, error_size, "model cache is empty; use select_model selector cache to refresh");
        *result = snag_tool_result_terminal(false, error);
        return *result ? 0 : -1;
    }
    if (snag_model_select_selector(&app->model_cache, app->config, selector, fallback,
            app->session.default_effort, &selected, error, error_size) < 0) {
        *result = snag_tool_result_terminal(false, error[0] ? error : "invalid model selector");
        return *result ? 0 : -1;
    }
    rows = (struct model_tool_rows){
        .cache = &app->model_cache, .config = app->config, .selected = &selected};
    if (snag_model_each(&app->model_cache, app->config, app->session.default_effort,
            model_tool_cached_row, &rows) < 0)
        return -1;
    if (!rows.found) {
        *result = snag_tool_result_terminal(false,
            "model selector is not a current cached provider/model/effort; "
            "use selector cache to refresh");
        return *result ? 0 : -1;
    }
    const struct snag_context_choice *choice = selected.context_set ? &selected.context : NULL;
    struct snag_model_capacity capacity;
    if (choice && snag_app_context_preview(app, selected.provider, selected.model, choice,
                      &capacity, error, error_size) < 0) {
        *result = snag_tool_result_terminal(false, error);
        return *result ? 0 : -1;
    }
    if (commit_model_selection(
            app, selected.provider, selected.model, selected.effort, true, false, choice) < 0)
        return snag_errorf(error, error_size, "cannot apply model selection");
    if (snag_buf_printf(&message, "model for %s: %s / %s / %s",
            app->session.active_turn ? "next response in this turn" : "next turn",
            selected.provider->name, selected.model, selected.effort) < 0) {
        snag_buf_free(&message);
        return -1;
    }
    if (choice) {
        int appended = choice->mode == SNAG_CONTEXT_MODE_TOKENS
                           ? snag_buf_printf(&message, "; context=%llu tokens",
                                 (unsigned long long)choice->tokens)
                           : snag_buf_printf(&message, "; context=%s",
                                 snag_context_mode_name(choice->mode));
        if (appended < 0) {
            snag_buf_free(&message);
            return -1;
        }
    }
    *result = snag_tool_result_terminal(true, (const char *)message.data);
    snag_buf_free(&message);
    return *result ? 0 : -1;
}

struct config_snapshot {
    bool exists;
    char sha256[SNAG_SHA256_HEX_LEN + 1u];
};

static int
snapshot_config(const char *path, struct config_snapshot *snapshot, char *error, size_t error_size)
{
    struct snag_buf text = {.max = SNAG_CONFIG_FILE_MAX};
    snag_file_info st;
    int fd, rc = -1;

    memset(snapshot, 0, sizeof(*snapshot));
    fd = snag_open_read(path, false);
    if (fd < 0) {
        if (errno == ENOENT) return 0;
        return snag_errorf(
            error, error_size, "cannot open configuration %s: %s", path, strerror(errno));
    }
    if (snag_fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uintmax_t)st.st_size > SNAG_CONFIG_FILE_MAX) {
        (void)snag_fail(error, error_size, EINVAL,
            "configuration must be a regular file no larger than 64 KiB");
        goto out;
    }
    if (snag_buf_read(&text, fd) < 0) {
        snag_errorf(error, error_size, "cannot read configuration: %s", strerror(errno));
        goto out;
    }
    snag_sha256_hex(text.data, text.len, snapshot->sha256);
    snapshot->exists = true;
    rc = 0;
out:
    snag_secret_clear(text.data, text.len);
    snag_buf_free(&text);
    if (close(fd) < 0 && rc == 0)
        rc = snag_errorf(error, error_size, "cannot close configuration: %s", strerror(errno));
    return rc;
}

static bool
same_config_snapshot(const struct config_snapshot *left, const struct config_snapshot *right)
{
    return left->exists == right->exists &&
           (!left->exists || strcmp(left->sha256, right->sha256) == 0);
}

static bool
irc_config_equal(const struct snag_config *left, const struct snag_config *right)
{
    if (left->irc.listen_explicit != right->irc.listen_explicit ||
        strcmp(left->irc.listen, right->irc.listen) != 0 ||
        left->irc.client_count != right->irc.client_count ||
        strcmp(left->irc.model_nick, right->irc.model_nick) != 0 ||
        strcmp(left->irc.operator_nick, right->irc.operator_nick) != 0 ||
        left->irc.model_nick_implicit != right->irc.model_nick_implicit ||
        left->irc.operator_nick_implicit != right->irc.operator_nick_implicit ||
        strcmp(left->irc.room_name, right->irc.room_name) != 0 ||
        left->irc.history_lines != right->irc.history_lines)
        return false;
    for (size_t i = 0u; i < left->irc.client_count; ++i)
        if (strcmp(left->irc.clients[i], right->irc.clients[i]) != 0) return false;
    return true;
}

static int
apply_network(struct app_state *app, struct snag_config *candidate, char *error, size_t error_size)
{
    int rc = 0;

    if (snag_irc_normalize(candidate, error, error_size) < 0) return 1;
    if (irc_config_equal(app->config, candidate)) return 0;
    if (snag_irc_configure(app->irc, candidate, app->session.cwd, error, error_size) < 0) {
        char original[256], rollback[256] = {0};

        (void)snag_strcpy(original, sizeof(original), error[0] ? error : "IRC change failed");
        if (snag_irc_configure(
                app->irc, app->config, app->session.cwd, rollback, sizeof(rollback)) < 0)
            snag_errorf(error, error_size,
                "%s; restoration failed: %s; /status shows remaining roles", original,
                rollback[0] ? rollback : strerror(errno));
        else
            snag_errorf(error, error_size, "%s; previous roles restored", original);
        rc = 1;
    } else {
        app->config->irc = candidate->irc;
    }
    snag_irc_roles(app->irc, app->config);
    candidate->irc = app->config->irc;
    app->networked = snag_irc_enabled(app->config);
    if (snag_app_irc_snapshot(app, "topology", NULL, 0u) < 0 || tick_irc(app, NULL, 0u) < 0)
        return -1;
    return rc;
}

static void
merge_file_network(struct app_state *app, struct snag_config *candidate)
{
    const struct snag_irc_config *old = &app->irc_file_config;
    struct snag_irc_config file = candidate->irc;

    candidate->irc = app->config->irc;
    if (file.listen_explicit != old->listen_explicit || strcmp(file.listen, old->listen) != 0) {
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
    if (file.history_lines != old->history_lines) candidate->irc.history_lines = file.history_lines;
}

static int
reload_config(struct app_state *app, char *error, size_t error_size)
{
    struct snag_config candidate;
    struct snag_config previous;
    struct snag_mcp *mcp_candidate = NULL;
    struct snag_model_cache cache = {0};
    const char *selected_provider =
        app->session.default_provider[0] ? app->session.default_provider : NULL;
    struct snag_irc_config file_network;
    int rc = 1;

    snag_config_init(&candidate);
    if (!candidate.shell) {
        snag_errorf(error, error_size, "cannot initialize configuration defaults");
        goto out;
    }
    if (snag_config_load(&candidate, app->config_allow_create ? NULL : app->config_path,
            app->store.root_path, error, error_size) < 0)
        goto out;
    file_network = candidate.irc;
    merge_file_network(app, &candidate);
    if (snag_irc_normalize(&candidate, error, error_size) < 0) goto out;
    if (!snag_config_provider(&candidate, selected_provider) ||
        (app->session.active_turn &&
            !snag_config_provider(&candidate, app->session.active_turn_provider)) ||
        (app->session.pending_input &&
            !snag_config_provider(
                &candidate, snag_json_string(app->session.pending_input, "provider")))) {
        (void)snag_fail(error, error_size, EINVAL,
            "reloaded configuration does not define the selected provider");
        goto out;
    }
    if (validate_prompt_candidate(app, &candidate, app->session.name) < 0) {
        (void)snag_fail(error, error_size, EINVAL,
            "reloaded prompt cannot be rendered with the current selection");
        goto out;
    }
    if (snag_auth_config_open(app->store.root_fd, &candidate, error, error_size) < 0) {
        goto out;
    }
#ifndef SNAJPAGENT_TEST_FIXTURE
    if (snag_auth_config_check(
            snag_config_provider(&candidate, selected_provider), error, error_size) < 0 ||
        (app->session.active_turn && snag_auth_config_check(snag_config_provider(&candidate,
                                                                app->session.active_turn_provider),
                                         error, error_size) < 0) ||
        (app->session.pending_input &&
            snag_auth_config_check(snag_config_provider(&candidate,
                                       snag_json_string(app->session.pending_input, "provider")),
                error, error_size) < 0)) {
        goto out;
    }
#endif
    for (size_t i = 0; i < candidate.secret_count; ++i) {
        char *value = NULL;
        if (snag_secret_source_resolve(&candidate.secrets[i], &value, error, error_size) < 0)
            goto out;
        snag_secret_bytes_free(value);
    }
    if (snag_model_cache_load(&app->store, &cache, error, error_size) < 0) goto out;
    candidate.mcp_credentials = app->config->mcp_credentials
        ? json_deep_copy(app->config->mcp_credentials) : json_array();
    if (!candidate.mcp_credentials) goto out;
    mcp_candidate = snag_mcp_open(app->store.root_fd, &candidate);
    if (!mcp_candidate) goto out;
    rc = apply_network(app, &candidate, error, error_size);
    if (rc != 0) goto out;
    snag_mcp_configure(app->mcp, mcp_candidate);
    mcp_candidate = NULL;
    snag_app_irc_summary_close(app);
    app->irc_summary_attempt_count = 0;
    previous = *app->config;
    *app->config = candidate;
    memset(&candidate, 0, sizeof(candidate));
    if (cache.providers) {
        snag_model_cache_free(&app->model_cache);
        app->model_cache = cache;
        memset(&cache, 0, sizeof(cache));
    }
    app->capacity_cache_error[0] = '\0';
    app->irc_file_config = file_network;
    app->turn_provider = snag_config_provider(app->config,
        app->session.active_turn ? app->session.active_turn_provider : selected_provider);
    if (!app->session.active_turn) {
        app->turn_model = app->session.default_model;
        app->turn_effort = resolve_effort(app->session.default_effort);
    }
    snag_ui_send(&app->ui, (struct snag_ui_command){.kind = SNAG_UI_COLOR,
                               .data.value = snag_cli_color(app->cli, app->config->color)});
    snag_ui_send(&app->ui, (struct snag_ui_command){.kind = SNAG_UI_MARKDOWN,
                               .data.value = snag_cli_markdown(app->cli, app->config->markdown)});
    snag_ui_send(&app->ui, (struct snag_ui_command){.kind = SNAG_UI_COMMANDS,
                               .data.commands = {snag_commands, snag_command_count()}});
    snag_ui_send(&app->ui, (struct snag_ui_command){.kind = SNAG_UI_PAUSE,
                               .data.timing = {app->config->typing_pause_ms,
                                   app->config->prompt_tool_spinner_off_delay_ms}});
    snag_auth_config_close(&previous);
    snag_config_free(&previous);
    rc = 0;
out:
    snag_mcp_close(mcp_candidate);
    snag_model_cache_free(&cache);
    snag_auth_config_close(&candidate);
    snag_config_free(&candidate);
    return rc;
}

static int
suspend_external(void *opaque)
{
    struct app_state *app = opaque;
    if (app->ui.native) return snag_ui_session_control(&app->ui, SNAG_SESSION_SUSPEND, NULL, 0u);
    return snag_term_suspend();
}

static void
service_external(void *opaque)
{
    struct app_state *app = opaque;
    char error[256] = {0};
    /* Do not apply ordinary audio prompts while an external program owns input.
     * A lost native controller must close audio before replacement activates. */
    if (app->ui.native && !snag_ui_session_attachment(&app->ui) && snag_app_audio_service(app) < 0)
        app->interrupt_requested = true;
    if (service_attachment(app, true) < 0) app->interrupt_requested = true;
    /* The editor owns terminal input; the engine still owns live jobs and IRC. */
    if (snag_tools_service(0, snag_ui_wake_fd(&app->ui), error, sizeof(error)) < 0)
        app->interrupt_requested = true;
    if (app->networked && tick_irc(app, error, sizeof(error)) < 0) app->interrupt_requested = true;
    (void)snag_app_shutdown(app);
}

static int
run_config_editor(struct app_state *app, bool *success, char *error, size_t error_size)
{
    const char *editor = getenv("EDITOR");

    if (!editor || !*editor) {
        (void)snag_fail(error, error_size, ENOENT, "$EDITOR is not set");
        return 1;
    }
    if (snag_ui_external(&app->ui, true, error, error_size) < 0) return -1;
    int rc = snag_editor_run(app->config_path, success, service_external, suspend_external, app,
        app->ui.native ? &app->ui.profile : NULL);
    int saved = errno;
    if (snag_ui_external(&app->ui, false, error, error_size) < 0) return -1;
    if (rc < 0) {
        errno = saved;
        return snag_errorf(error, error_size, "cannot run $EDITOR: %s", strerror(errno));
    }
    return 0;
}

static int
change_config(struct app_state *app, bool active)
{
    struct config_snapshot before;
    struct config_snapshot after;
    char error[256] = {0};
    bool editor_success = false;
    int rc;

    if (active || (!app->applying_controls && !app->session.pending_log))
        return request_control(app, SNAG_CONTROL_CONFIG, "/config");
    if (snapshot_config(app->config_path, &before, error, sizeof(error)) < 0)
        return app_error(app, error);
    rc = run_config_editor(app, &editor_success, error, sizeof(error));
    if (rc != 0) return rc < 0 ? -1 : app_error(app, error);
    error[0] = '\0';
    if (snapshot_config(app->config_path, &after, error, sizeof(error)) < 0)
        return app_error(app, error);
    if (same_config_snapshot(&before, &after)) {
        if (!editor_success)
            return app_error(app, "$EDITOR exited unsuccessfully; configuration unchanged");
        return app_textf(app, SNAG_UI_HOST, "configuration unchanged: %s", app->config_path);
    }
    error[0] = '\0';
    rc = reload_config(app, error, sizeof(error));
    if (rc != 0) {
        int render_rc = app_error(app, error[0] ? error : "configuration reload failed");
        return rc < 0 || render_rc < 0 ? -1 : 0;
    }
    if (!editor_success) {
        if (app_warning(app, "$EDITOR exited unsuccessfully after changing the configuration") < 0)
            return -1;
    }
    return app_textf(app, SNAG_UI_HOST, "configuration reloaded: %s", app->config_path);
}

static int
change_effort(struct app_state *app, const char *value, bool active)
{
    char error[256] = {0};
    char *copy = NULL;
    char *effort = NULL;
    if (!value) return show_setting(app, "effort", app->session.default_effort);
    copy = snag_strdup_checked(value, SNAG_CONFIG_EFFORT_MAX - 1u);
    if (copy) effort = trim_selector_part(copy);
    if (!effort || !resolve_effort(effort)) {
        free(copy);
        return app_error(app, "effort exceeds the supported structural bounds");
    }
    if (record_model_selection(app, app->session.default_provider, app->session.default_model,
            effort, error, sizeof(error)) < 0) {
        (void)app_error(app, error[0] ? error : "effort preference could not be saved");
        free(copy);
        return -1;
    }
    free(copy);
    if (active && app->session.turn_fallback_active) app->model_switch_requested = true;
    return app_textf(app, SNAG_UI_HOST, "effort for %s: %s (until changed)",
        active ? "next response in this turn" : "next turn", app->session.default_effort);
}

static int
report_context(struct app_state *app, const struct snag_provider_config *provider,
    const struct snag_context_choice *choice)
{
    struct snag_model_capacity capacity;
    struct snag_model_limit_config rule;
    const struct snag_model_limit_config *rule_sources[3];
    struct snag_buf text = {.max = 4096u};
    char error[256] = {0};
    uint64_t selected;
    bool over_budget;
    int rc = -1;

    if (snag_app_context_preview(
            app, provider, app->session.default_model, choice, &capacity, error, sizeof(error)) < 0)
        return app_error(app, error[0] ? error : "context capacity could not be resolved");
    selected = choice->mode == SNAG_CONTEXT_MODE_MAX      ? capacity.max_context_window_tokens
               : choice->mode == SNAG_CONTEXT_MODE_TOKENS ? choice->tokens
                                                          : capacity.context_window_tokens;
    if (snag_buf_printf(&text, "context for %s: %s (until changed)",
            app->session.active_turn ? "next response in this turn" : "next turn",
            choice->mode == SNAG_CONTEXT_MODE_MAX ? "max (advertised maximum)"
            : choice->mode == SNAG_CONTEXT_MODE_TOKENS
                ? "explicit token window"
                : "default (configured or advertised working window)") < 0)
        goto out;
    if (snag_buf_append(&text, "\nselected=", strlen("\nselected=")) < 0) goto out;
    if (selected) {
        if (snag_buf_printf(&text, "%llu", (unsigned long long)selected) < 0) goto out;
    } else if (snag_buf_append(&text, "unknown", strlen("unknown")) < 0) {
        goto out;
    }
    if (snag_buf_printf(&text, " tokens · reserve=%llu · effective=%u%%",
            (unsigned long long)capacity.max_output_tokens,
            capacity.effective_context_window_percent) < 0 ||
        append_capacity_value(
            &text, "input-budget", capacity.hard_input_known, capacity.hard_input_tokens) < 0 ||
        append_compact_threshold(&text, provider, &capacity) < 0)
        goto out;
    if (choice->mode != SNAG_CONTEXT_MODE_DEFAULT &&
        snag_config_resolve_limits(
            app->config, provider->name, app->session.default_model, &rule, rule_sources) &&
        rule.context_window_tokens &&
        snag_buf_append(&text, "\nconfigured context rule is ignored by the session selection",
            strlen("\nconfigured context rule is ignored by the session selection")) < 0)
        goto out;
    over_budget = capacity.hard_input_known &&
                  context_meter_matches(app, provider, app->session.default_model,
                      resolve_effort(app->session.default_effort)) &&
                  app->session.context_meter.input_tokens >=
                      snag_model_compact_threshold(provider, &capacity);
    if (over_budget &&
        snag_buf_append(&text,
            "\nthe next request compacts first: the measured input is over this budget",
            strlen("\nthe next request compacts first: the measured input is over this budget")) <
            0)
        goto out;
    if (snag_buf_terminate(&text) < 0) goto out;
    rc = snag_app_report(app, SNAG_UI_HOST, (const char *)text.data);
out:
    snag_buf_free(&text);
    return rc;
}

static int
change_context(struct app_state *app, const char *value, bool active)
{
    const struct snag_provider_config *provider = next_provider(app);
    struct snag_context_choice choice = {SNAG_CONTEXT_MODE_DEFAULT, 0u};
    struct snag_model_capacity capacity;
    char error[256] = {0};
    char *copy = NULL;
    char *word = NULL;
    char *end = NULL;
    bool save = false;

    if (!provider)
        return app_error(app, "selected provider is not present in the current configuration");
    choice.mode = app->session.context_mode;
    choice.tokens = app->session.context_tokens;
    if (!value) return report_context(app, provider, &choice);
    copy = snag_strdup_checked(value, SNAG_CONFIG_PATH_MAX);
    if (!copy) return app_error(app, "context selector is too long");
    word = copy;
    while (*word && isspace((unsigned char)*word)) ++word;
    end = word + strlen(word);
    while (end > word && isspace((unsigned char)end[-1])) --end;
    *end = '\0';
    if (!*word) {
        free(copy);
        return report_context(app, provider, &choice);
    }
    end = word;
    while (*end && !isspace((unsigned char)*end)) ++end;
    if (*end) {
        *end++ = '\0';
        char *suffix = trim_selector_part(end);
        if (strcmp(suffix, "s") && strcmp(suffix, "save")) {
            free(copy);
            return app_error(app, "context accepts default, max, or a token count with s/save");
        }
        save = true;
    }
    if (snag_model_parse_context(word, &choice, error, sizeof(error)) < 0) {
        free(copy);
        return app_error(app, error);
    }
    free(copy);
    if (save && choice.mode != SNAG_CONTEXT_MODE_TOKENS)
        return app_error(app, "saving a context default requires a token count: /context N s|save");
    /* Resolve the candidate before recording it: an impossible choice must not
     * reach the session log, and the operator sees the reconciled reserve and
     * compaction budget for the choice they made. */
    if (snag_app_context_preview(app, provider, app->session.default_model, &choice, &capacity,
            error, sizeof(error)) < 0)
        return app_error(app, error[0] ? error : "context capacity could not be resolved");
    if (save &&
        snag_config_save_context(app->config, app->config_path, app->config_allow_create,
            provider->name, app->session.default_model, choice.tokens, error, sizeof(error)) < 0)
        return app_error(app, error[0] ? error : "context default could not be saved");
    if (record_context_selection(app, &choice, error, sizeof(error)) < 0)
        return app_error(app, error[0] ? error : "context selection could not be saved");
    if (active && app->session.turn_fallback_active) app->model_switch_requested = true;
    int rc = report_context(app, provider, &choice);
    if (rc < 0) return rc;
    return save ? app_textf(app, SNAG_UI_HOST, "configuration saved: %s", app->config_path) : 0;
}

static int
select_view(struct app_state *app, enum snag_render_view view, bool active)
{
    if (snag_ui_send(&app->ui, (struct snag_ui_command){.kind = SNAG_UI_VIEW, .data.value = view}) <
            0 ||
        set_input_prompt(app, active) < 0)
        return -1;
    return 0;
}
/* A user-requested view switch. Announce it before the repaint, which can take a moment, so the
 * keystroke is visibly accepted, and refuse to start another switch while one is in flight so
 * repeated requests cannot queue a second one. */
static int
user_switch_view(struct app_state *app, enum snag_render_view view, bool active)
{
    if (snag_ui_view(&app->ui) == view) return select_view(app, view, active);
    if (app->switch_target != SNAG_RENDER_VIEW_COUNT &&
        snag_ui_view(&app->ui) != app->switch_target &&
        snag_monotonic_ms() < app->switch_deadline_ms)
        return 0;
    if (app_textf(app, SNAG_UI_HOST, "switching to %s",
            view == SNAG_RENDER_CHAT ? "chat" : "rollout") < 0)
        return -1;
    if (select_view(app, view, active) < 0) return -1;
    app->switch_target = view;
    app->switch_deadline_ms = snag_monotonic_ms() + 500u;
    return 0;
}

static int
toggle_view(struct app_state *app)
{
    enum snag_render_view view;

    view = snag_ui_view(&app->ui) == SNAG_RENDER_CHAT ? SNAG_RENDER_ROLLOUT : SNAG_RENDER_CHAT;
    return user_switch_view(app, view, app->session.active_turn);
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

    server = strncmp(line, "/server", 7u) == 0 && (!line[7] || isspace((unsigned char)line[7]));
    connect = strncmp(line, "/connect", 8u) == 0 && (!line[8] || isspace((unsigned char)line[8]));
    disconnect =
        strncmp(line, "/disconnect", 11u) == 0 && (!line[11] || isspace((unsigned char)line[11]));
    *handled = server || connect || disconnect;
    if (!*handled) return 0;
    /* Model tools mutate live owners directly. Start terminal commands from
     * those authoritative roles so /disconnect also sees tool-created clients. */
    if (app->irc) snag_irc_roles(app->irc, &candidate);
    if (!snag_strcpy(copy, sizeof(copy), line))
        return app_error(app, "network command is too long");
    for (word = strtok_r(copy, " \t", &save); word && count < 4u;
        word = strtok_r(NULL, " \t", &save))
        words[count++] = word;
    if (!count || count == 4u || (!server && count > 2u))
        return app_error(app,
            "usage: /server [start [ENDPOINT]|stop], /connect [ENDPOINT], /disconnect [ENDPOINT]");
    if (server) {
        if (count == 1u)
            return app_reportf(app,
                config->listen_explicit ? "hosting %s"
                                        : "hosting is off; use /server start [ENDPOINT]",
                config->listen);
        if (count == 2u && strcmp(words[1], "stop") == 0) {
            if (!config->listen_explicit)
                return app_textf(
                    app, SNAG_UI_HOST, "hosting is already off; outgoing connections unchanged");
            config->listen_explicit = false;
        } else if (count >= 2u && strcmp(words[1], "start") == 0) {
            endpoint = count == 3u ? words[2] : "localhost:6667";
            if (config->listen_explicit) {
                if (snag_irc_endpoint_equal(config->listen, endpoint))
                    return app_textf(app, SNAG_UI_HOST, "already hosting %s", config->listen);
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
            return app_textf(
                app, SNAG_UI_HOST, "already hosting %s; no self-connection needed", endpoint);
        for (size_t i = 0u; i < config->client_count; ++i)
            if (snag_irc_endpoint_equal(config->clients[i], endpoint))
                return app_textf(
                    app, SNAG_UI_HOST, "outgoing connection already configured: %s", endpoint);
        if (config->client_count == SNAG_CONFIG_IRC_CLIENT_MAX)
            return app_error(app, "at most 16 outgoing connections are supported");
        if (!snag_strcpy(
                config->clients[config->client_count], sizeof(config->clients[0]), endpoint))
            return app_error(app, "IRC endpoint is too long");
        ++config->client_count;
    } else {
        size_t index;

        if (!config->client_count)
            return app_textf(app, SNAG_UI_HOST, "no outgoing connections; hosting unchanged");
        if (count == 1u) {
            config->client_count = 0u;
            memset(config->clients, 0, sizeof(config->clients));
        } else {
            for (index = 0u; index < config->client_count; ++index)
                if (snag_irc_endpoint_equal(config->clients[index], words[1])) break;
            if (index == config->client_count)
                return app_textf(
                    app, SNAG_UI_HOST, "outgoing endpoint is not configured: %s", words[1]);
            memmove(config->clients + index, config->clients + index + 1u,
                (--config->client_count - index) * sizeof(config->clients[0]));
            memset(config->clients[config->client_count], 0, sizeof(config->clients[0]));
        }
    }
    rc = apply_network(app, &candidate, error, sizeof(error));
    if (rc != 0) return rc < 0 ? -1 : app_error(app, error);
    if (connect)
        return app_textf(app, SNAG_UI_HOST,
            "outgoing connection added; /status shows connection state; /chat opens public chat");
    if (disconnect)
        return app_textf(app, SNAG_UI_HOST, "outgoing connection%s removed; hosting unchanged",
            count == 1u ? "s" : "");
    return app_textf(app, SNAG_UI_HOST,
        config->listen_explicit ? "hosting started on %s; /chat opens public chat"
                                : "hosting stopped; outgoing connections unchanged",
        config->listen);
}

int
snag_app_irc_select_conversation(
    struct app_state *app, const struct snag_irc_conversation_target *target)
{
#if SNAJPAGENT_VM
    if (app->view_command) {
        json_t *route = snag_irc_conversation_route(target);
        if (!route) return -1;
        json_decref(app->view_command->selection);
        app->view_command->selection = route;
        if (app->view_command->chained) {
            app->ui.input_conversation = *target;
            app->ui.input_view = SNAG_RENDER_CHAT;
        }
        return 0;
    }
#endif
    return snag_ui_send(&app->ui,
        (struct snag_ui_command){.kind = SNAG_UI_CONVERSATION_SELECT, .data.conversation = target});
}

static int
send_operator_conversation(
    struct app_state *app, const char *line, const char *text, enum snag_irc_event_kind kind)
{
    struct snag_irc_conversation_target frozen = app->ui.input_conversation;
    const struct snag_irc_conversation_target *target = &frozen;
    char id[SNAG_ID_HEX_LEN + 1u] = {0};
    if (app->ui.input_interface && app->ui.view_request[0]) {
        memcpy(id, app->ui.view_request, sizeof(id));
        app->ui.view_request[0] = '\0';
    }
    char error[256] = {0};
    struct snag_buf report = {.max = SNAG_MAX_IRC_SNAPSHOT};
    int rc;
    if (target->identity != SNAG_IRC_OPERATOR) {
        rc = snag_fail(error, sizeof(error), EACCES,
            "Viewing the model's chat; use %s %s to reply as yourself",
            target->kind == SNAG_IRC_QUERY ? "/query" : "/chat",
            target->kind == SNAG_IRC_QUERY ? target->peer : target->room);
    } else if (kind != SNAG_IRC_MESSAGE && kind != SNAG_IRC_NOTICE) {
        rc = snag_fail(error, sizeof(error), EINVAL, "this command requires a channel");
    } else {
        rc = snag_app_irc_conversation_send(
            app, target, kind, text, false, &report, error, sizeof(error));
    }
    bool accepted = report.len != 0u;
    if (id[0]) {
        int result =
            snag_ui_view_result(&app->ui, id, rc == 0 || accepted ? "committed" : "rejected",
                app->session.next_seq - 1u, rc < 0 ? error : "irc_event_v2");
        snag_buf_free(&report);
        return result;
    }
    if (rc < 0) {
        if (snag_buf_printf(&report, "%s", error[0] ? error : "conversation send failed") < 0 ||
            snag_buf_terminate(&report) < 0 || app_error(app, (const char *)report.data) < 0)
            goto fail;
        if (!accepted && snag_ui_restore_input(&app->ui, line) < 0) goto fail;
    }
    snag_buf_free(&report);
    return 0;
fail:
    snag_buf_free(&report);
    return -1;
}

static int
send_operator_routed(
    struct app_state *app, const char *line, const char *text, enum snag_irc_event_kind kind)
{
    char error[256] = {0};
    int rc;
    bool show = app->ui.input_route.count > 1u;
    uint32_t destination;
    size_t body;
    enum snag_irc_target_command command =
        snag_irc_target_parse(line, strlen(line), &destination, &body);
    if (app->ui.input_view == SNAG_RENDER_CHAT && app->ui.input_conversation.conversation[0] &&
        kind != SNAG_IRC_NICK && command == SNAG_IRC_TARGET_NONE)
        return send_operator_conversation(app, line, text, kind);

    struct snag_buf report = {.max = 8192u};
    rc = snag_irc_send_route(
        app->irc, &app->ui.input_route, false, kind, text, &report, error, sizeof(error));
    if ((rc == 0 || rc == 2) && persist_session(app, error, sizeof(error)) < 0) {
        snag_buf_reset(&report);
        rc = -1;
    }
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
    if (rc == 1) return snag_ui_restore_input(&app->ui, line);
    return rc < 0 ? -1 : 0;
}

static int
change_nick(struct app_state *app, const char *line)
{
    const char *model_nick = app->irc ? snag_irc_model_nick(app->irc) : NULL;
    const char *operator_nick = app->irc ? snag_irc_operator_nick(app->irc) : NULL;
    const char *start = line + 5u;
    const char *end;
    struct snag_buf nick = {.max = 64u * 1024u};
    int rc;

    while (isspace((unsigned char)*start)) ++start;
    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) --end;
    if (end == start) {
        return app_reportf(app, "model nick: %s\noperator nick: %s",
            model_nick && *model_nick ? model_nick : "<none>",
            operator_nick && *operator_nick ? operator_nick : "<none>");
    }
    if (snag_buf_printf(&nick, "%.*s", (int)(end - start), start) < 0 ||
        snag_buf_terminate(&nick) < 0) {
        snag_buf_free(&nick);
        return -1;
    }
    rc = send_operator_routed(app, line, (const char *)nick.data, SNAG_IRC_NICK);
    snag_buf_free(&nick);
    return rc;
}

static const char *
effective_steering(struct app_state *app, bool *overridden)
{
    const struct snag_model_limit_config *limit;
    const struct snag_provider_config *provider;

    if (app->session.steering_override && *app->session.steering_override) {
        if (overridden) *overridden = true;
        return app->session.steering_override;
    }
    provider = next_provider(app);
    if (provider) {
        limit =
            snag_config_model_limit_exact(app->config, provider->name, app->session.default_model);
        if (limit && limit->steering[0]) {
            if (overridden) *overridden = false;
            return limit->steering;
        }
    }
    if (overridden) *overridden = false;
    return "mentions";
}

static int
change_steering(struct app_state *app, const char *line)
{
    const char *start = line + 9u;
    const char *end;
    char error[256] = {0};
    char mode[16];
    size_t len;
    bool overridden;
    const char *effective;

    while (isspace((unsigned char)*start)) ++start;
    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) --end;
    if (end == start) {
        effective = effective_steering(app, &overridden);
        return app_reportf(app, "steering for next turn: %s (%s)", effective,
            overridden ? "session override" : "config");
    }
    len = (size_t)(end - start);
    if (len >= sizeof(mode)) return app_error(app, "usage: /steering [mentions|all|clear]");
    memcpy(mode, start, len);
    mode[len] = '\0';
    if (strcmp(mode, "clear") == 0) {
        mode[0] = '\0';
    } else if (strcmp(mode, "mentions") != 0 && strcmp(mode, "all") != 0) {
        return app_error(app, "usage: /steering [mentions|all|clear]");
    }
    if (snag_app_commit_event(
            app, "steering_updated", json_pack("{s:s}", "mode", mode), error, sizeof(error)) < 0)
        return app_error(app, error[0] ? error : "steering could not be updated");
    effective = effective_steering(app, &overridden);
    return app_textf(app, SNAG_UI_HOST, "steering for next turn: %s (%s)", effective,
        overridden ? "session override" : "config");
}

static int
change_banner(struct app_state *app, const char *line)
{
    const char *start = line + 7u;
    const char *end;
    struct snag_buf text = {.max = SNAG_BANNER_MAX + 1u};
    char error[256] = {0};
    int rc;

    while (isspace((unsigned char)*start)) ++start;
    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) --end;
    if (end == start) {
        if (!app->session.banner_text || !*app->session.banner_text)
            return app_reportf(app, "banner: none");
        return app_reportf(app, "banner:\n%s", app->session.banner_text);
    }
    if ((size_t)(end - start) == 5u && strncmp(start, "clear", 5u) == 0) {
        if (snag_app_commit_event(
                app, "banner_updated", json_pack("{s:s}", "text", ""), error, sizeof(error)) < 0)
            return app_error(app, error[0] ? error : "banner could not be cleared");
        return app_textf(app, SNAG_UI_HOST, "banner cleared");
    }
    if (snag_buf_printf(&text, "%.*s", (int)(end - start), start) < 0 ||
        snag_buf_terminate(&text) < 0) {
        snag_buf_free(&text);
        return -1;
    }
    if (!snag_text_valid((const char *)text.data, 1u, SNAG_BANNER_MAX)) {
        snag_buf_free(&text);
        return app_error(app, "banner must be nonblank valid UTF-8 within 4 KiB");
    }
    rc = snag_app_commit_event(app, "banner_updated",
        json_pack("{s:s}", "text", (const char *)text.data), error, sizeof(error));
    snag_buf_free(&text);
    if (rc < 0) return app_error(app, error[0] ? error : "banner could not be updated");
    return app_textf(
        app, SNAG_UI_HOST, "banner updated; echoed after compaction and at next-turn start");
}

static int
handle_destination_command(struct app_state *app, const char *line, bool *handled)
{
    uint32_t id;
    size_t body;
    enum snag_irc_target_command command = snag_irc_target_parse(line, strlen(line), &id, &body);

    *handled = command != SNAG_IRC_TARGET_NONE;
    if (command == SNAG_IRC_TARGET_NONE) return 0;
    if (command == SNAG_IRC_TARGET_INVALID) {
        if (app_error(app, "use /N to select, /N TEXT to send once, or /all TEXT") < 0) return -1;
        return snag_ui_restore_input(&app->ui, line);
    }
    if (command == SNAG_IRC_TARGET_SELECT)
        return snag_ui_send(
                   &app->ui, (struct snag_ui_command){.kind = SNAG_UI_SELECT, .data.value = id}) < 0
                   ? app_error(app, "destination unavailable; use /names")
                   : 0;
    if (command == SNAG_IRC_TARGET_SEND && !app->ui.input_route.count) {
        char error[96u];
        (void)snprintf(error, sizeof(error), "destination %u is unavailable; use /names", id);
        if (app_error(app, error) < 0) return -1;
        return snag_ui_restore_input(&app->ui, line);
    }
    return send_operator_routed(app, line, line + body, SNAG_IRC_MESSAGE);
}

/* The current transport/tool invocation has unwound before this function runs.
 * An editor is an external effect: journal its start and never relaunch an
 * ambiguous previous invocation automatically after a crash. */
static int
apply_controls(struct app_state *app)
{
    if (app->applying_controls || app->session.response_open || app->session.pending_call_count)
        return 0;
    unsigned int deferred = 0u;
    if (!app->session.pending_controls) {
        app->control_requested = false;
        return 0;
    }
    app->applying_controls = true;
    app->control_requested = false;
    if (!app->session.active_turn && !app->input_closed) {
        app->steering_requested = false;
    }
    int result = 0;
    while (app->session.pending_controls & ~deferred) {
        if (app->input_closed || snag_ui_leaving(&app->ui)) break;
        unsigned int pending = app->session.pending_controls & ~deferred;
        unsigned int bit = 0u;
        unsigned int index = 0u;
        uint64_t first = UINT64_MAX;
        for (unsigned int i = 0u; i < 7u; ++i)
            if ((pending & (1u << i)) && app->session.control_seq[i] < first) {
                first = app->session.control_seq[i];
                bit = 1u << i;
                index = i;
            }
        if (!bit) {
            result = -1;
            break;
        }
        uint64_t failures = app->session.write_failures;
        char error[256] = {0};
        bool started = (app->session.started_controls & bit) != 0u;
        if (!started && commit_event(app, "control_started",
                            json_pack("{s:i}", "control", (int)bit), error, sizeof(error)) < 0) {
            result = -1;
            break;
        }
        int rc = 0;
        bool exit_now = false, handled = false;
#if SNAJPAGENT_VM
        bool terminal = app->view_terminal && (app->view_terminal->controls & bit);
        bool notification = !terminal && (app->view_controls & bit);
        struct snag_buf *previous_app_report = app->command_report;
        struct snag_buf *previous_report = app->ui.command_report;
        bool previous_passthrough = app->ui.command_report_passthrough;
        bool previous_error = app->ui.command_error;
        if (notification) {
            struct snag_buf *report = &app->view_control_reports[index];
            report->max = SIZE_MAX;
            if (!report->len && snag_buf_printf(report, "%s\n", view_control_names[index]) < 0) {
                result = -1;
                break;
            }
            app->command_report = report;
        }
        if (terminal || notification) {
            app->ui.command_report = terminal ? &app->view_terminal->report : app->command_report;
            app->ui.command_report_passthrough = terminal;
            app->ui.command_error = false;
        }
#else
        (void)index;
#endif /* SNAJPAGENT_VM */
        if (bit == SNAG_CONTROL_RELOAD) {
            rc = reload_config(app, error, sizeof(error));
            if (!rc)
                rc = app_textf(app, SNAG_UI_HOST, "configuration reloaded: %s", app->config_path);
        } else if (bit == SNAG_CONTROL_CONFIG) {
            if (started) {
                (void)app_warning(app, "Previous configuration editor outcome is uncertain; "
                                       "reloading the saved file without reopening the editor.");
                rc = reload_config(app, error, sizeof(error));
            } else
                rc = change_config(app, false);
        } else if (bit == SNAG_CONTROL_CACHE) {
            rc = change_model(app, "cache", false);
        } else if (bit == SNAG_CONTROL_COMPACT) {
            if (!app->execute && snag_ui_hold(&app->ui, app->session.active_turn) < 0) {
                result = rc = -1;
            } else {
                rc = app->session.compact_control_image_boundary
                         ? snag_app_compact_image_boundary(app, error, sizeof(error))
                         : snag_app_compact_requested(app, error, sizeof(error));
            }
        } else if (bit == SNAG_CONTROL_DELETE) {
            rc = snag_app_lifecycle_command(app, "/delete", &handled, &exit_now);
        }
        /* Retired archive controls finish without an effect when old sessions resume. */
        if (error[0] && app_error(app, error) < 0) result = -1;
#if SNAJPAGENT_VM
        if (terminal || notification) {
            bool failed = rc < 0 || app->ui.command_error || result < 0;
            if (terminal) app->view_terminal->failed |= failed;
            if (notification && failed) app->view_control_errors |= bit;
            app->command_report = previous_app_report;
            app->ui.command_report = previous_report;
            app->ui.command_report_passthrough = previous_passthrough;
            app->ui.command_error = previous_error;
        }
#endif
        if (result < 0) break;
        if (bit == SNAG_CONTROL_DELETE && exit_now && app->session.delete_requested) {
            app->input_closed = true;
            break; /* Deleted journal can no longer record a completion. */
        }
        if (app->session.write_failures != failures || app->session.append_rollback_pending) {
            result = -1;
            break; /* Preserve intent when its effects could not be recorded. */
        }
        if (rc == SNAG_APP_COMPACT_DEFERRED) {
            deferred |= bit;
            continue;
        }
        if (commit_event(app, "control_finished", json_pack("{s:i}", "control", (int)bit), error,
                sizeof(error)) < 0) {
            result = -1;
            break;
        }
#if SNAJPAGENT_VM
        if (terminal) {
            app->view_terminal->controls &= ~bit;
            if (view_terminal_finish(app) < 0) {
                result = -1;
                break;
            }
        }
        if (notification && view_control_publish(app, index, rc > 0) < 0) {
            result = -1;
            break;
        }
#endif
        if (exit_now) {
            app->input_closed = true;
            break;
        }
        if (bit == SNAG_CONTROL_COMPACT && rc == 2) {
            if (!app->session.active_turn && snag_app_queue_arm(app, false) < 0) result = -1;
            break;
        }
        if (rc < 0 && bit != SNAG_CONTROL_COMPACT) {
            result = -1;
            break;
        }
    }
    app->applying_controls = false;
    if (result == 0 && !app->session.pending_controls && app->turn_prompt_seen &&
        app->session.active_turn && app->ui.opened && !app->execute && !app->input_closed &&
        set_input_prompt(app, true) < 0)
        result = -1;
    return result;
}

static const char goal_actions_text[] =
    "goal actions: status, set TEXT, pause, resume, lock, unlock, complete, cancel, clear, help";

static int
state_command(struct app_state *app, const char *line, bool active)
{
    const char *argument = line + 6u;

    while (isspace((unsigned char)*argument)) ++argument;
    if (!*argument) {
        if (render_status(app) < 0) return -1;
        if (snag_app_goal_command(app, "/goal", active) < 0) return -1;
        return snag_app_report(app, SNAG_UI_HOST, goal_actions_text);
    }
    if (strncmp(argument, "goal", 4u) == 0 &&
        (!argument[4] || isspace((unsigned char)argument[4]))) {
        if (!argument[4]) {
            if (snag_app_goal_command(app, "/goal", active) < 0) return -1;
            return snag_app_report(app, SNAG_UI_HOST, goal_actions_text);
        }
        size_t rest = strlen(argument + 4u);
        char *mapped = malloc(5u + rest + 1u);
        int rc;

        if (!mapped) return snag_errno(ENOMEM);
        memcpy(mapped, "/goal", 5u);
        memcpy(mapped + 5u, argument + 4u, rest + 1u);
        rc = snag_app_goal_command(app, mapped, active);
        free(mapped);
        return rc;
    }
    return app_error(app, "unknown /state section; use /state, /state goal, or /help");
}

static int
handle_common_command(
    struct app_state *app, const char *line, bool active, bool *handled, bool *prompt_ready)
{
    char error[256] = {0};

    *handled = true;
    *prompt_ready = false;
    {
        int rc = snag_app_voice_command(app, line, handled);
        if (*handled) return rc;
        rc = snag_app_audio_command(app, line, handled);
        if (*handled) return rc;
        if ((app->audio || app->voice) &&
            (!strncmp(line, "/attach", 7u) || !strcmp(line, "/receive") ||
                !strcmp(line, "/receive -d"))) {
            *handled = true;
            return app_error(app, "Stop local audio before preparing an attachment.");
        }
        rc = snag_app_media_command(app, line, handled);
        if (*handled) return rc;
        rc = network_command(app, line, handled);

        if (*handled) return rc;
        *handled = true;
    }
    if (strcmp(line, "/help") == 0 || strcmp(line, "/?") == 0) return snag_app_help(app, NULL);
    if (snag_verbosity_command(line, strlen(line))) {
        unsigned int level = snag_ui_verbosity(&app->ui);
        bool pane = app->ui.input_interface && app->ui.input_verbosity >= 0;
        if (pane) level = (unsigned int)app->ui.input_verbosity;
#if SNAJPAGENT_VM
        if (pane && app->view_command && app->view_command->verbosity >= 0)
            level = (unsigned int)app->view_command->verbosity;
#endif
        char feedback[192];
        enum snag_render_view view = app->ui.input_interface ? app->ui.input_view :
            snag_ui_view(&app->ui);
        if (!snag_verbosity_apply(line, &level, view, feedback, sizeof(feedback)))
            return app_error(app, feedback);
#if SNAJPAGENT_VM
        if (pane && app->view_command) app->view_command->verbosity = (int)level;
#endif
        if (!pane && level != snag_ui_verbosity(&app->ui) &&
            snag_ui_set_verbosity(&app->ui, level) < 0) return -1;
        return app_reportf(app, "%s", feedback);
    }
    if (strncmp(line, "/compact", 8u) == 0 && (!line[8] || isspace((unsigned char)line[8]))) {
        const char *rest = line + 8u;
        while (isspace((unsigned char)*rest)) ++rest;
        if (*rest) return app_error(app, "usage: /compact");
        if (!app->session.turn_count)
            return app_textf(app, SNAG_UI_HOST, "nothing to compact before the first prompt");
        return request_control(app, SNAG_CONTROL_COMPACT, "/compact");
    }
    if (active && !strcmp(line, "/delete")) return request_control(app, SNAG_CONTROL_DELETE, line);
    if (active && !strcmp(line, "/next")) {
        if (!app->session.pending_queue_count) return app_error(app, "future-turn queue is empty");
        if (snag_app_queue_arm(app, true) < 0) return -1;
        return app_textf(app, SNAG_UI_HOST, "queued work armed for the next full turn");
    }
    if (!strcmp(line, "/retry")) {
        if (!active) return app_error(app, "no failed turn to retry");
        app->yield_requested = app->tool_waiting;
        return request_control(app, SNAG_CONTROL_RETRY, "/retry");
    }
    if (!strncmp(line, "/retry", 6u) && isspace((unsigned char)line[6])) {
        const char *argument = line + 6u;
        while (isspace((unsigned char)*argument)) ++argument;
        if (strncmp(argument, "auto", 4u) || (argument[4] && !isspace((unsigned char)argument[4])))
            return app_error(app, "usage: /retry auto [on|off]");
        argument += 4u;
        while (isspace((unsigned char)*argument)) ++argument;
        return change_retry_auto(app, argument);
    }
    if (active && strcmp(line, "/exit") == 0) {
        app->input_closed = true;
        app->interrupt_requested = true;
        return 0;
    }
    if (strcmp(line, "/yield") == 0) {
        if (!app->tool_waiting)
            return app_textf(app, SNAG_UI_HOST, "No active tool wait to yield.");
        char error[256] = {0};
        if (commit_event(app, "turn_yield_requested",
                json_pack("{s:s}", "turn_id", app->session.active_turn_id), error,
                sizeof(error)) < 0)
            return -1;
        app->yield_requested = true;
        return app_textf(
            app, SNAG_UI_HOST, "Operator requested tool yield; returning control to the model.");
    }
    if (strcmp(line, "/status") == 0) return render_status(app);
    if ((!strncmp(line, "/session", 8u) && (!line[8] || isspace((unsigned char)line[8]))) ||
        (!strncmp(line, "/s", 2u) && (!line[2] || isspace((unsigned char)line[2])))) {
        const char *argument = line + (line[2] == 'e' ? 8u : 2u);
        char error[256] = {0};
        while (isspace((unsigned char)*argument)) ++argument;
        size_t len = strlen(argument);
        while (len && isspace((unsigned char)argument[len - 1u])) --len;
        if ((len == 1u && argument[0] == 'd') || (len == 6u && !strncmp(argument, "detach", 6u))) {
            if (!app->ui.native) return app_error(app, "native attachment is unavailable here");
            write_resume_command(app, app->program, app->store.root_path);
            if (snag_ui_session_control(&app->ui, SNAG_SESSION_DETACH, NULL, 0u) < 0)
                return app_error(app, "cannot detach terminal client");
            return 0;
        }
        size_t verb = 0u;
        while (verb < len && !isspace((unsigned char)argument[verb])) ++verb;
        if (verb == 4u && !strncmp(argument, "name", 4u)) {
            while (verb < len && isspace((unsigned char)argument[verb])) ++verb;
            json_t *name = json_stringn(argument + verb, len - verb);
            if (!name || !snag_session_name_valid(json_string_value(name))) {
                json_decref(name);
                return app_error(app, "usage: /session name NAME (nonempty single-line UTF-8)");
            }
            if (!app->execute &&
                validate_prompt_candidate(app, app->config, json_string_value(name)) < 0) {
                json_decref(name);
                return app_error(app, "session name does not fit the configured prompt");
            }
            if (commit_event(app, "session_named", json_pack("{s:o}", "name", name), error,
                    sizeof(error)) < 0 ||
                persist_session(app, error, sizeof(error)) < 0)
                return app_error(app, error);
            return app_textf(app, SNAG_UI_HOST, "session name: %s", app->session.name);
        }
        if ((verb == 1u && argument[0] == 'a') ||
            (verb == 6u && !strncmp(argument, "attach", 6u))) {
            if (!app->ui.native) return app_error(app, "native attachment is unavailable here");
            while (verb < len && isspace((unsigned char)argument[verb])) ++verb;
            if (len == verb || len - verb > SNAG_ID_HEX_LEN)
                return app_error(
                    app, "usage: /session attach|a ID (unique 1..32 character prefix)");
            char prefix[SNAG_ID_HEX_LEN + 1u];
            memcpy(prefix, argument + verb, len - verb);
            prefix[len - verb] = '\0';
            struct snag_session target;
            snag_session_init(&target);
            int rc = snag_session_locate(
                &app->store, &target, prefix, list_row, app, error, sizeof(error));
            if (!rc && !strcmp(target.id, app->session.id))
                rc = snag_errorf(error, sizeof(error), "already attached to this session");
            if (!rc) {
                rc = snag_app_voice_switch_request(app, target.id, error, sizeof(error));
                if (rc > 0 && snag_ui_session_control(&app->ui, SNAG_SESSION_SWITCH, target.id,
                                  strlen(target.id)) < 0) {
                    rc = snag_errorf(
                        error, sizeof(error), "cannot switch terminal: %s", strerror(errno));
                }
            }
            snag_session_close(&target);
            return rc < 0 ? app_error(app, error) : 0;
        }
        if (len && !(len == 1u && argument[0] == 'l') &&
            (len != 4u || strncmp(argument, "list", 4u)))
            return app_error(
                app, "usage: /session [list|l|name NAME|attach|a ID|detach|d] (alias /s)");
        if (app_reportf(app, "current session: %s%s\n%s sessions:", app->session.id,
                app->session.pending_log ? " (not yet saved)" : "", len ? "saved" : "running") < 0)
            return -1;
        if (snag_store_list(&app->store, &app->session, len ? UINT64_MAX : 0u, list_columns(app),
                list_row, app, error, sizeof(error)) < 0) {
            return app_error(app, error);
        }
        return error[0] ? app_reportf(app, "%s", error) : 0;
    }
    if (strncmp(line, "/history", 8u) == 0 && (!line[8] || isspace((unsigned char)line[8]))) {
        const char *argument = line + 8u;
        uint64_t count = 1u;
        while (isspace((unsigned char)*argument)) ++argument;
        if (*argument && snag_parse_count(argument, &count) < 0)
            return app_error(app, "usage: /history [N]");
        return app->command_report
                   ? snag_ui_history_report(&app->ui, &app->session, count, app->command_report)
                   : snag_ui_history(&app->ui, &app->session, count);
    }
    if (strncmp(line, "/cat", 4u) == 0 && (!line[4] || isspace((unsigned char)line[4])))
        return page_local_file(app, line + 4u);
    if (!strncmp(line, "/send", 5u) && (!line[5] || isspace((unsigned char)line[5]))) {
        const char *path = line + 5u;
        json_t *result = NULL;
        char error[256] = {0};
        while (isspace((unsigned char)*path)) ++path;
        if (!*path) return app_error(app, "usage: /send PATH");
        int rc = snag_app_download(app, path, &result, error, sizeof(error));
        if (rc == 0) {
            const char *message = snag_json_string(result, "model_text");
            rc = snag_ui_text(&app->ui,
                (!strcmp(snag_json_string(result, "status"), "succeeded")) ? SNAG_UI_HOST
                                                                           : SNAG_UI_ERROR,
                message ? message : "Send finished.");
        }
        json_decref(result);
        return rc;
    }
    int irc_rc = snag_app_irc_command(app, line, handled);
    if (*handled || irc_rc < 0) return irc_rc;
    *handled = true;
    if (strcmp(line, "/chat") == 0) {
        int rc = app->ui.input_view_applied ? set_input_prompt(app, active)
                                            : user_switch_view(app, SNAG_RENDER_CHAT, active);

        *prompt_ready = rc == 0;
        if (rc == 0 && !app->networked)
            rc = app_textf(app, SNAG_UI_HOST, "chat is offline; use /connect or /server start");
        return rc;
    }
    if (strcmp(line, "/rollout") == 0) {
        int rc = app->ui.input_view_applied ? set_input_prompt(app, active)
                                            : user_switch_view(app, SNAG_RENDER_ROLLOUT, active);

        *prompt_ready = rc == 0;
        return rc;
    }
    if (!strcmp(line, "/fallback")) return change_fallback(app, NULL);
    if (!strncmp(line, "/fallback ", 10u)) return change_fallback(app, line + 10u);
    if (strcmp(line, "/model") == 0) return change_model(app, NULL, active);
    if (strncmp(line, "/model ", 7u) == 0) return change_model(app, line + 7u, active);
    if (!strncmp(line, "/fast", 5u) && (!line[5] || isspace((unsigned char)line[5]))) {
        const char *argument = line + 5u;
        while (isspace((unsigned char)*argument)) ++argument;
        return change_fast(app, *argument ? argument : NULL);
    }
    if (strcmp(line, "/config") == 0)
        return (app->audio || app->voice)
                   ? app_error(app, "Stop local audio before reloading configuration.")
                   : change_config(app, active);
    if (!strncmp(line, "/mcp", 4u) && (!line[4] || isspace((unsigned char)line[4]))) {
        struct snag_buf report = {.max = SNAG_MAX_EVENT_LINE};
        int rc = 0;
        const char *argument = line + 4u;
        while (isspace((unsigned char)*argument)) ++argument;
        if (!strncmp(argument, "login", 5u) && isspace((unsigned char)argument[5])) {
            struct snag_buf command = {.max = SNAG_PATH_MAX_BYTES * 3u};
            const char *server = argument + 5u;
            while (isspace((unsigned char)*server)) ++server;
            rc = snag_command_argument(&command, app->program);
            if (!rc) rc = snag_command_argument(&command, "--config");
            if (!rc) rc = snag_command_argument(&command, app->config_path);
            if (!rc) rc = snag_command_argument(&command, "--dotdir");
            if (!rc) rc = snag_command_argument(&command, app->store.root_path);
            if (!rc) rc = snag_command_argument(&command, "mcp");
            if (!rc) rc = snag_command_argument(&command, "login");
            if (!rc) rc = snag_command_argument(&command, server);
            if (!rc) rc = snag_command_finish(&command);
            if (!rc) rc = snag_buf_terminate(&command);
            if (!rc) rc = snag_buf_printf(&report,
                "Run this in a shell, then /configure here:\n%s\n", command.data);
            snag_buf_free(&command);
        } else {
            rc = snag_mcp_command(app->mcp, line + 4u, &report, snag_app_active_input_pump, app);
        }
        if (!rc) rc = snag_buf_terminate(&report);
        if (!rc) rc = app_reportf(app, "%s", report.data ? (char *)report.data : "");
        snag_buf_free(&report);
        return rc;
    }
    if (strcmp(line, "/configure") == 0) {
        if (app->audio || app->voice) {
            return app_error(app, "Stop local audio before reloading configuration.");
        }
        return request_control(app, SNAG_CONTROL_RELOAD, "/configure");
    }
    if (strcmp(line, "/effort") == 0) return change_effort(app, NULL, active);
    if (strncmp(line, "/effort ", 8u) == 0) return change_effort(app, line + 8u, active);
    if (strcmp(line, "/context") == 0) return change_context(app, NULL, active);
    if (strncmp(line, "/context ", 9u) == 0) return change_context(app, line + 9u, active);
    if (strcmp(line, "/state") == 0 || strncmp(line, "/state ", 7u) == 0)
        return state_command(app, line, active);
    if (strncmp(line, "/goal", 5u) == 0 && (!line[5] || isspace((unsigned char)line[5]))) {
        /* /goal is an alias for /state goal. */
        size_t rest = strlen(line + 5u);
        char *mapped = malloc(11u + rest + 1u);
        int rc;

        if (!mapped) return snag_errno(ENOMEM);
        memcpy(mapped, "/state goal", 11u);
        memcpy(mapped + 11u, line + 5u, rest + 1u);
        rc = state_command(app, mapped, active);
        free(mapped);
        return rc;
    }
    if (snag_string_in(line, "/names /topic")) {
        int rc;

        struct snag_buf state = {.max = SNAG_MAX_IRC_SNAPSHOT};
        rc = snag_buf_printf(&state, "selected destination: %u\n", app->ui.selection.id);
        if (rc == 0) rc = snag_irc_state(app->irc, &state, error, sizeof(error));
        if (rc == 0) rc = snag_buf_terminate(&state);
        if (rc == 0) rc = snag_app_report(app, SNAG_UI_HOST, (const char *)state.data);
        snag_buf_free(&state);
        return rc < 0 ? app_error(app, error[0] ? error : "IRC state could not be displayed") : 0;
    }
    if (strncmp(line, "/topic ", 7u) == 0)
        return send_operator_routed(app, line, line + 7u, SNAG_IRC_TOPIC);
    if (strcmp(line, "/nick") == 0 || strncmp(line, "/nick ", 6u) == 0)
        return change_nick(app, line);
    if (strcmp(line, "/steering") == 0 || strncmp(line, "/steering ", 10u) == 0)
        return change_steering(app, line);
    if (strcmp(line, "/banner") == 0 || strncmp(line, "/banner ", 8u) == 0)
        return change_banner(app, line);
    *handled = false;
    return 0;
}
static int
cancel_queue_edit(struct app_state *app, bool active)
{
    bool editing = app->queue_edit_id[0] != '\0';

    if (editing) {
        if (snag_app_queue_arm(app, app->queue_edit_was_armed) < 0) return -1;
        app->queue_edit_id[0] = '\0';
        app->queue_edit_number = 0u;
        app->queue_edit_was_armed = false;
        /* The editor label and its restored draft are separate synchronous UI
         * requests. Input can win the scheduling gap after the label appears,
         * so an immediate Ctrl-C may be consumed before that draft request.
         * Explicitly clear any late restoration before acknowledging the
         * normal prompt; otherwise the next Ctrl-C only clears stale text
         * instead of interrupting the still-active turn. */
        if (snag_ui_send(&app->ui, (struct snag_ui_command){.kind = SNAG_UI_DRAFT, .text = ""}) < 0)
            return -1;
    }
    return set_input_prompt(app, active);
}

static int
flush_download_queue(struct app_state *app)
{
    if (!app->session.download_queue || !json_array_size(app->session.download_queue)) return 0;
    if (app->pager || app->session.active_turn || app->attaching || app->execute || !app->ui.opened)
        return 0;
    json_t *snapshot = json_deep_copy(app->session.download_queue);
    if (!snapshot) return -1;
    int rc = 0;
    for (size_t i = 0; rc == 0 && i < json_array_size(snapshot); ++i) {
        json_t *item = json_array_get(snapshot, i);
        const char *id = snag_json_string(item, "id");
        char error[256] = {0};
        json_t *result = NULL;
        rc = snag_app_download_pending(app, item, NULL, &result, error, sizeof(error));
        if (rc == 0) {
            const char *message = snag_json_string(result, "model_text");
            rc = snag_ui_text(&app->ui,
                !strcmp(snag_json_string(result, "status"), "succeeded") ? SNAG_UI_HOST
                                                                         : SNAG_UI_ERROR,
                message ? message : "Send finished.");
        }
        if (rc == 0 && result && !strcmp(snag_json_string(result, "status"), "succeeded")) {
            if (snag_app_commit_event(app, "download_removed",
                    json_pack("{s:s,s:s}", "id", id, "reason", "delivered to wrapped client"),
                    error, sizeof(error)) < 0) {
                rc = snag_ui_text(&app->ui, SNAG_UI_ERROR,
                    error[0] ? error : "delivered download could not be removed from queue");
            }
        }
        json_decref(result);
    }
    json_decref(snapshot);
    return rc;
}

static int
input_view_toggle(struct app_state *app)
{
    if (app->queue_edit_id[0]) {
        (void)app_error(app, "queue replacement must be nonempty");
        return 0;
    }
    return toggle_view(app);
}

static int
input_single_command(
    struct app_state *app, const char *line, bool active, bool *handled, bool *prompt_ready)
{
    bool single_line = strchr(line, '\n') == NULL;
    char error[256] = {0};
    bool read_only;
    (void)snag_prompt_parse(line, &read_only);
    if (active && read_only) {
        *handled = true;
        *prompt_ready = false;
        int rc = queue_future_turn(app, line, true, error, sizeof(error));
        if (rc && error[0]) (void)app_error(app, error);
        return rc;
    }
    if (snag_prompt_command(line) && !app->ui.input_echoed &&
        snag_ui_submitted(&app->ui, app->ui.label, line, true) < 0)
        return -1;
    int rc = handle_destination_command(app, line, handled);

    if (rc < 0 || *handled) return rc;
    if (single_line && line[0] == '/' && line[1] != '/') {
        rc = handle_common_command(app, line,
            active || app->applying_controls || snag_ui_leaving(&app->ui), handled, prompt_ready);
        if (rc < 0 || *handled) return rc;
    }
    if (active || single_line) {
        rc = handle_queue_command(app, line, active, handled, error, sizeof(error));
        if (rc != 0 && error[0]) (void)app_error(app, error);
    }
    return rc;
}

static bool
command_leaves_session(const char *line)
{
    if (!strcmp(line, "/exit") || !strcmp(line, "/delete")) return true;
    if (!strncmp(line, "/session ", 9u) || !strncmp(line, "/s ", 3u)) {
        const char *rest = line + (line[2] == 'e' ? 9u : 3u);
        size_t length = strcspn(rest, " \t");
        return (length == 1u && (rest[0] == 'd' || rest[0] == 'a')) ||
            (length == 6u && !memcmp(rest, "detach", length)) ||
            (length == 6u && !memcmp(rest, "attach", length));
    }
    return false;
}

static int
input_commands(struct app_state *app, const char *source,
    const struct snag_command_line *commands, bool active, bool *handled, bool *prompt_ready)
{
    if (!commands->count && !commands->error)
        return input_single_command(app, source, active, handled, prompt_ready);
    *handled = true;
    *prompt_ready = false;
    bool echoed = app->ui.input_echoed;
    if (!echoed && snag_ui_submitted(&app->ui, app->ui.label, source, true) < 0) return -1;
    if (commands->error) return app_error(app, commands->error);
    app->ui.input_echoed = true;
    bool previous_error = app->ui.command_error;
    app->ui.command_error = false;
    int rc = 0;
    size_t offset = 0u;
    for (size_t i = 0u; i < commands->count; ++i) {
        const char *line = (const char *)commands->parts.data + offset;
        offset += strlen(line) + 1u;
        bool read_only;
        (void)snag_prompt_parse(line, &read_only);
        bool canonical = commands->chained || strcmp(line, source);
        bool starts_turn = canonical && !active &&
            (read_only || !strcmp(line, "/retry"));
#if SNAJPAGENT_VM
        if (app->view_command && commands->chained) app->view_command->line = line;
#endif
        if (commands->chained && !app->ui.input_interface &&
            snag_ui_capture_route(&app->ui, line) < 0) {
            rc = -1;
            break;
        }
        rc = starts_turn ? 0 : input_single_command(app, line, active, handled, prompt_ready);
#if SNAJPAGENT_VM
        if (!rc && commands->chained && app->view_command && app->view_command->snapshot) {
            rc = snag_ui_command_report(&app->ui, app->view_command->snapshot, "");
            json_decref(app->view_command->snapshot);
            app->view_command->snapshot = NULL;
        }
#endif
        if (canonical && !rc && (starts_turn || !*handled)) {
            *handled = true;
            if (!active) {
                int submitted = submit_idle(app, line, app->ui.input_view, prompt_ready);
                if (submitted == 1) app->input_closed = true;
                else if (submitted) rc = -1;
            } else rc = app_error(app, "unknown slash command");
        }
        while (!rc && commands->chained && app->pager &&
            !app->interrupt_requested && !snag_app_shutdown(app)) {
            service_external(app);
            rc = service_pager(app);
            if (!rc && app->pager) (void)snag_sleep_ms(25u);
        }
        if (rc || app->ui.command_error || app->input_closed || snag_app_shutdown(app) ||
            (commands->chained && app->interrupt_requested) ||
            (commands->chained && command_leaves_session(line))) break;
    }
    app->ui.command_error |= previous_error;
    app->ui.input_echoed = echoed;
    return rc;
}

#if SNAJPAGENT_VM
/* Commands requiring terminal-bound input or IRC scope hand off before any
 * effect. This list grows only when that command has a semantic adapter. */
static bool
view_single_command_native(const char *line, bool chained)
{
    size_t length = strcspn(line, " \t\r\n");
    char verb[32];
    if (length >= sizeof(verb)) return false;
    memcpy(verb, line, length);
    verb[length] = '\0';
    if (snag_string_in(verb,
            "/help /? /status /history /model /fallback /fast /effort /context /retry "
            "/state /goal /steering /banner /configure /compact /yield /verbose /cat "
            "/attachments /detach "
            "/query /msg /notice /me /chat /join /part /names /topic /connections /whois /nick"))
        return true;
    if (strcmp(verb, "/session") && strcmp(verb, "/s")) {
        if (!chained) return false;
        uint32_t id;
        size_t body;
        if (snag_irc_target_parse(line, strlen(line), &id, &body) != SNAG_IRC_TARGET_NONE)
            return false;
        for (size_t i = 0u; i < snag_command_count(); ++i) {
            const char *syntax = snag_commands[i].syntax;
            if (!strncmp(syntax, verb, length) &&
                (!syntax[length] || isspace((unsigned char)syntax[length]))) return false;
        }
        return true;
    }
    const char *argument = line + length;
    while (isspace((unsigned char)*argument)) ++argument;
    return !*argument || !strcmp(argument, "l") || !strcmp(argument, "list") ||
           (!strncmp(argument, "name", 4u) &&
               (!argument[4] || isspace((unsigned char)argument[4])));
}

/* Published files are immutable presentation, never events or provider input.
 * A receipt exposes a random basename only after all bytes and the directory
 * entry are durable. Session deletion also removes its retained reports. */
static json_t *
view_command_report(struct app_state *app, const char *command, const struct snag_buf *text)
{
    return snag_vm_report_store(app->session.dir_fd, command, text->data, text->len);
}

static int
view_control_publish(struct app_state *app, unsigned int index, bool interrupted)
{
    struct snag_buf *text = &app->view_control_reports[index];
    int captured = snag_buf_printf(text, "Result: %s\n",
        app->view_control_errors & (1u << index) ? "error"
        : interrupted                            ? "interrupted"
                                                 : "completed");
    json_t *report =
        captured < 0 ? NULL : view_command_report(app, view_control_names[index], text);
    char error[256] = "";
    if (!report)
        (void)snprintf(
            error, sizeof(error), "cannot retain completion report: %s", strerror(errno));
    int rc = snag_ui_command_report(&app->ui, report, error);
    json_decref(report);
    snag_buf_free(text);
    app->view_controls &= ~(1u << index);
    app->view_control_errors &= ~(1u << index);
    return rc;
}

static int
view_command_complete(struct app_state *app, const char *id, const char *line,
    const struct snag_buf *report, bool failed, bool terminal, json_t *snapshot,
    json_t *selection, int verbosity)
{
    json_t *saved = snapshot ? json_incref(snapshot) : view_command_report(app, line, report);
    char error[256] = "";
    if (!saved)
        (void)snprintf(error, sizeof(error), "cannot retain command report: %s", strerror(errno));
    json_t *result = json_pack("{s:s,s:s,s:I,s:s,s:O,s:s,s:b}", "id", id, "status", "completed",
        "seq", (json_int_t)(app->session.next_seq - 1u), "outcome", failed ? "error" : "ok",
        "report", saved ? saved : json_null(), "report_error", error, "return_terminal", terminal);
    json_decref(saved);
    int rc = result ? 0 : -1;
    if (!rc && selection) rc = json_object_set(result, "selection", selection);
    if (!rc && verbosity >= 0 && (!failed || snag_command_chained(line, strlen(line))))
        rc = json_object_set_new(result, "verbosity", json_integer(verbosity));
    if (!rc) rc = snag_ui_command_result(&app->ui, result);
    json_decref(result);
    return rc;
}

static void
view_terminal_free(struct app_state *app)
{
    if (!app->view_terminal) return;
    free(app->view_terminal->command);
    snag_buf_free(&app->view_terminal->report);
    free(app->view_terminal);
    app->view_terminal = NULL;
}

static int
view_terminal_finish(struct app_state *app)
{
    struct app_view_terminal *command = app->view_terminal;
    if (!command || command->dispatching || command->controls || command->pager) return 0;
    int rc = view_command_complete(app, command->id, command->command, &command->report,
        command->failed, true, NULL, NULL, -1);
    view_terminal_free(app);
    return rc;
}

static bool
view_terminal_finite(const char *line)
{
    return !strcmp(line, "/config") || !strcmp(line, "/receive") || !strcmp(line, "/receive -d") ||
           (!strncmp(line, "/attach", 7u) && (!line[7] || isspace((unsigned char)line[7]))) ||
           (!strncmp(line, "/send", 5u) && (!line[5] || isspace((unsigned char)line[5]))) ||
           (!strncmp(line, "/cat", 4u) && (!line[4] || isspace((unsigned char)line[4])));
}

static int
view_input_command(
    struct app_state *app, const char *line, const struct snag_command_line *commands,
    bool active, bool *handled, bool *prompt_ready)
{
    char id[SNAG_ID_HEX_LEN + 1u];
    memcpy(id, app->ui.view_request, sizeof(id));
    /* A command may pump input while working. Its owner receipt stays pending,
     * but it must not be mistaken for a rejected prompt by the next poll. */
    app->ui.view_request[0] = '\0';
    *handled = true;
    *prompt_ready = false;
    bool terminal = app->ui.input_terminal_command;
    bool native = true, finite = false;
    size_t offset = 0u;
    for (size_t i = 0u; i < commands->count; ++i) {
        const char *part = (const char *)commands->parts.data + offset;
        native &= view_single_command_native(part, commands->chained);
        finite |= view_terminal_finite(part);
        offset += strlen(part) + 1u;
    }
    if (!terminal && !commands->error && !native) {
        if (app->ui.input_conversation.conversation[0])
            return snag_ui_view_result(&app->ui, id, "rejected", 0u,
                "open the rollout to use this command's whole-terminal interface");
        json_t *result = json_pack("{s:s,s:s,s:I,s:s}", "id", id, "status", "terminal", "seq",
            (json_int_t)(app->session.next_seq - 1u), "reason",
            "command requires the session's whole-terminal interface");
        int rc = result ? snag_ui_command_result(&app->ui, result) : -1;
        json_decref(result);
        return rc;
    }
    struct snag_buf report = {.max = SIZE_MAX};
    int rc = snag_term_append_safe(&report, line, strlen(line));
    if (!rc) rc = snag_buf_putc(&report, '\n');
    if (rc < 0) {
        snag_buf_free(&report);
        return snag_ui_view_result(&app->ui, id, "rejected", 0u, "cannot retain command");
    }
    finite &= terminal;
    if (finite) {
        if (app->view_terminal) {
            snag_buf_free(&report);
            return snag_ui_view_result(
                &app->ui, id, "rejected", 0u, "another terminal operation is pending");
        }
        app->view_terminal = calloc(1u, sizeof(*app->view_terminal));
        if (!app->view_terminal || !(app->view_terminal->command = strdup(line))) {
            view_terminal_free(app);
            snag_buf_free(&report);
            return snag_ui_view_result(
                &app->ui, id, "rejected", 0u, "cannot retain terminal operation");
        }
        memcpy(app->view_terminal->id, id, sizeof(id));
        app->view_terminal->dispatching = true;
    }
    struct snag_buf *previous_app_report = app->command_report;
    struct snag_buf *previous_report = app->ui.command_report;
    bool previous_passthrough = app->ui.command_report_passthrough;
    bool previous_error = app->ui.command_error;
    app->command_report = terminal ? NULL : &report;
    app->ui.command_report = &report;
    app->ui.command_report_passthrough = terminal;
    app->ui.command_error = false;
    struct app_view_command capture = {
        .line = line, .verbosity = -1, .chained = commands->chained};
    struct app_view_command *previous_capture = app->view_command;
    app->view_command = terminal ? NULL : &capture;
    rc = input_commands(app, line, commands, active, handled, prompt_ready);
    app->view_command = previous_capture;
    if (!rc && !*handled) rc = app_error(app, "unknown slash command");
    bool failed = rc < 0 || app->ui.command_error;
    app->command_report = previous_app_report;
    app->ui.command_report = previous_report;
    app->ui.command_report_passthrough = previous_passthrough;
    app->ui.command_error = previous_error;
    *handled = true;
    if (finite) {
        app->view_terminal->report = report;
        app->view_terminal->failed = failed;
        app->view_terminal->pager = app->pager;
        app->view_terminal->dispatching = false;
        int completed = view_terminal_finish(app);
        return rc < 0 ? rc : completed;
    }
    int published = view_command_complete(app, id, line, &report, failed, false,
        capture.snapshot, capture.selection, capture.verbosity);
    json_decref(capture.selection);
    json_decref(capture.snapshot);
    snag_buf_free(&report);
    return rc < 0 ? rc : published;
}
#endif

static int
input_line_command(struct app_state *app, const char *line,
    const struct snag_command_line *commands, bool active, bool *handled, bool *prompt_ready)
{
    /* Idle retry starts a turn through submit_idle. Keep its frontend request
     * until the same durable input admission used by ordinary prompts. */
    if (!active && !commands->error && !commands->chained && !strcmp(line, "/retry") &&
        (app->session.last_turn_failed || app->session.active_turn || app->session.pending_input)) {
        *handled = *prompt_ready = false;
        return app->ui.input_echoed ? 0 :
            snag_ui_submitted(&app->ui, app->ui.label, line, true);
    }
#if SNAJPAGENT_VM
    if (app->ui.input_interface && app->ui.view_request[0] &&
        app->ui.input_view == SNAG_RENDER_CHAT && app->ui.input_conversation.conversation[0] &&
        !snag_prompt_command(line)) {
        *handled = true;
        *prompt_ready = false;
        const char *text = line[0] == '/' && line[1] == '/' ? line + 1 : line;
        return send_operator_conversation(app, line, text, SNAG_IRC_MESSAGE);
    }
    if ((app->ui.input_interface || app->ui.input_terminal_command) && app->ui.view_request[0] &&
        snag_prompt_command(line))
        return view_input_command(app, line, commands, active, handled, prompt_ready);
#endif
    if (app->command_report || app->ui.command_report) {
        /* A deferred operation can pump a replacement classic terminal.
         * Its commands own their output while the outer report is suspended. */
        struct snag_buf *previous_app_report = app->command_report;
        struct snag_buf *previous_report = app->ui.command_report;
        bool previous_passthrough = app->ui.command_report_passthrough;
        bool previous_error = app->ui.command_error;
#if SNAJPAGENT_VM
        struct app_view_command *previous_capture = app->view_command;
        app->view_command = NULL;
#endif
        app->command_report = app->ui.command_report = NULL;
        app->ui.command_report_passthrough = app->ui.command_error = false;
        int rc = input_commands(app, line, commands, active, handled, prompt_ready);
        app->command_report = previous_app_report;
        app->ui.command_report = previous_report;
        app->ui.command_report_passthrough = previous_passthrough;
        app->ui.command_error = previous_error;
#if SNAJPAGENT_VM
        app->view_command = previous_capture;
#endif
        return rc;
    }
    if (!app->ui.opened || app->ui.input_interface || app->execute || !pager_command(app) ||
        snag_isatty(STDIN_FILENO) != 1 || snag_isatty(STDERR_FILENO) != 1) {
        return input_commands(app, line, commands, active, handled, prompt_ready);
    }

    struct snag_buf report = {.max = SIZE_MAX};
    app->command_report = &report;
    int rc = input_commands(app, line, commands, active, handled, prompt_ready);
    app->command_report = NULL;
    if (rc == 0 && report.len) {
        rc = snag_buf_terminate(&report);
        if (!rc && !page_reference(app, (const char *)report.data, report.len)) {
            rc = snag_ui_text(&app->ui, SNAG_UI_HELP, (const char *)report.data);
        }
    }
    snag_buf_free(&report);
    return rc;
}

int
snag_app_input_command(
    struct app_state *app, const char *line, bool active, bool *handled, bool *prompt_ready)
{
    struct snag_command_line commands = {0};
    if (snag_prompt_command(line) &&
        snag_command_line_parse(line, strlen(line), '/', &commands) < 0 && !commands.error) {
        snag_buf_free(&commands.parts);
        return -1;
    }
    int rc = input_line_command(app, line, &commands, active, handled, prompt_ready);
    snag_buf_free(&commands.parts);
    return rc;
}

int
snag_app_active_input_pump(void *opaque, unsigned int timeout_ms)
{
    struct app_state *app = opaque;
again:;
    bool leaving = snag_ui_leaving(&app->ui);
    enum snag_term_action action = SNAG_TERM_NONE;
    char *line = NULL;
    char error[256];
    int rc;
    if (snag_app_flush_public(app, false) < 0) return -1;
    if (service_pager(app) < 0 || service_attachment(app, app->pager != NULL) < 0) return -1;
    if (app->pager && timeout_ms > 25u) timeout_ms = 25u;
    if (snag_mcp_watching(app->mcp) && timeout_ms > 100u) timeout_ms = 100u;
    if (snag_app_shutdown(app) || (app->interrupt_requested && !leaving)) {
        snag_app_audio_close(app);
        app->interrupt_requested = true;
        return 2;
    }
    if (!leaving &&
        (app->steering_requested || (app->control_requested && !app->applying_controls) ||
            (app->model_switch_requested && app->session.response_open)))
        return 1;
    if ((!app->pager || (app->ui.native && !snag_ui_session_attachment(&app->ui))) &&
        snag_app_audio_service(app) < 0)
        return -1;
    if ((app->audio || app->voice) && timeout_ms > 25u) timeout_ms = 25u;
    bool busy = snag_tools_busy();
    if (snag_tools_service(0, snag_ui_wake_fd(&app->ui), error, sizeof(error)) < 0) {
        (void)app_error(app, error);
        if (!app->recovery_wait) return -1;
    }
    for (size_t i = 0u; i < app->session.process_count; ++i)
        snag_tools_process_state(&app->session.processes[i]);
    if (busy != snag_tools_busy() &&
        snag_ui_send(&app->ui, (struct snag_ui_command){.kind = SNAG_UI_SPINNERS,
                                   .data.value = prompt_spinner_states(app)}) < 0)
        return -1;
    if (app->networked) {
        error[0] = '\0';
        if (tick_irc(app, error, sizeof(error)) < 0) {
            (void)snag_ui_text(&app->ui, SNAG_UI_ERROR, error[0] ? error : "IRC event loop failed");
            if (!app->recovery_wait) return -1;
        }
        if (timeout_ms > 25u) timeout_ms = 25u;
    }
    if (app->execute || app->input_closed) return 0;
    /* Input remains actionable before response.created. An early steer rebuilds
     * the active request; a foreground slash command owns the composer until
     * it finishes. Neither needs a provider acknowledgement to be displayed. */
    rc = snag_ui_poll(&app->ui, (int)timeout_ms, &action, &line);
    history_warning(app);
    if (rc < 0) {
        int input_errno = errno;
        if (snag_app_shutdown(app)) {
            app->interrupt_requested = true;
            free(line);
            return 2;
        }
        const char *message = "terminal presentation or dispatch failed";
        if (app->ui.input_error) {
            message = input_errno == EOVERFLOW ? "active submission exceeds 1 MiB"
                      : input_errno == EILSEQ  ? "active submission contains invalid UTF-8"
                                               : "active input could not be read";
        }
        (void)snag_ui_text(&app->ui, SNAG_UI_ERROR, message);
        if (input_errno == EOVERFLOW || input_errno == EILSEQ) return 0;
        app->input_closed = true;
        return -1;
    }
    if (rc == 0) {
        if (app->networked) {
            error[0] = '\0';
            if (tick_irc(app, error, sizeof(error)) < 0) {
                (void)snag_ui_text(
                    &app->ui, SNAG_UI_ERROR, error[0] ? error : "IRC event loop failed");
                return -1;
            }
        }
        return 0;
    }
    if (action == SNAG_TERM_DICTATE_DONE || action == SNAG_TERM_DICTATE_CANCEL) {
        free(line);
        return snag_app_audio_action(app, action);
    }
    if (action == SNAG_TERM_EXIT) {
        snag_app_audio_close(app);
        app->input_closed = true;
        app->interrupt_requested = true;
        free(line);
        return 2;
    }
    if (action == SNAG_TERM_CANCEL || (action == SNAG_TERM_INTERRUPT && app->queue_edit_id[0]))
        return cancel_queue_edit(app, true);
    if (action == SNAG_TERM_INTERRUPT) {
        if (app->session.active_turn &&
            commit_event(app, "turn_cancel_requested",
                json_pack("{s:s}", "turn_id", app->session.active_turn_id), error,
                sizeof(error)) < 0) {
            free(line);
            return -1;
        }
        if (!app->session.active_turn && app->session.pending_input &&
            commit_event(app, "input_cancelled", json_object(), error, sizeof(error)) < 0) {
            free(line);
            return -1;
        }
        app->interrupt_requested = true;
        free(line);
        return 2;
    }
    if (action == SNAG_TERM_REMOTE_READY) {
        snag_app_remote_reply(app, line);
        free(line);
        return 0;
    }
    if (action == SNAG_TERM_UPLOAD) {
        bool directory = line && !strcmp(line, "trz -d");
        free(line);
        rc = snag_app_upload_command(app, directory);
        return rc < 0 ? rc : set_input_prompt(app, true);
    }
    if (action == SNAG_TERM_VIEW) return input_view_toggle(app);
    if (!line) return 0;
    remember_input(app, line);
    error[0] = '\0';
    if (app->queue_edit_id[0] && !snag_prompt_command(line)) {
        rc = finish_queue_edit(app, line, true, error, sizeof(error));
        if (rc != 0 && error[0]) (void)snag_ui_text(&app->ui, SNAG_UI_ERROR, error);
    } else if (action == SNAG_TERM_QUEUE ||
               (!app->ui.input_active && app->session.active_turn &&
                   app->ui.input_view == SNAG_RENDER_ROLLOUT && !snag_prompt_command(line))) {
        rc = queue_future_turn(app, line, true, error, sizeof(error));
        if (rc != 0) {
            (void)snag_ui_text(&app->ui, SNAG_UI_ERROR, error);
            if (set_input_prompt(app, true) < 0 || snag_ui_restore_input(&app->ui, line) < 0)
                rc = -1;
        } else
            rc = set_input_prompt(app, true);
    } else {
        bool single_line = strchr(line, '\n') == NULL;
        bool handled = false;
        bool prompt_ready = false;
        rc = snag_app_input_command(app, line, true, &handled, &prompt_ready);
        if (rc < 0) goto active_done;
        if (handled) {
            if (!app->queue_edit_id[0] && !prompt_ready) {
                if (app->model_switch_requested) {
                    rc = snag_ui_hold(&app->ui, true);
                    if (rc == 0) app->turn_prompt_seen = false;
                } else
                    rc = set_input_prompt(app, true);
            }
        } else if (single_line && line[0] == '/' && line[1] != '/') {
            (void)snag_ui_text(&app->ui, SNAG_UI_ERROR, "unknown slash command");
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
                    if (rc == 0) rc = snag_ui_restore_input(&app->ui, line);
                } else
                    rc = set_input_prompt(app, true);
            } else if (!app->session.active_turn) {
                rc = queue_future_turn(app, text, true, error, sizeof(error));
                if (rc == 0 && app->recovery_wait) app->steering_requested = true;
            } else if (snag_random_id(steering_id) < 0) {
                rc = -1;
            } else {
                rc = commit_input(app, "steering_added",
                    snag_app_steering_added_data(app->session.active_turn_id, steering_id, text),
                    app->draft_content, error, sizeof(error));
                if (rc == 0 && !app->ui.input_echoed &&
                    snag_ui_submitted(&app->ui, app->ui.label, text, true) < 0)
                    rc = -1;
                if (rc < 0) {
                    (void)snag_ui_text(&app->ui, SNAG_UI_ERROR,
                        error[0] ? error : "active-turn input could not be persisted");
                    if (set_input_prompt(app, true) == 0)
                        (void)snag_ui_restore_input(&app->ui, line);
                } else {
                    /* A steer during pre-response compaction belongs in the
                     * next projection, not in an interruption of compaction.
                     * Explicitly deferred steers likewise wait for turn end. */
                    if (!app->session.steering_deferred && !app->session.active_compact_id[0])
                        app->steering_requested = true;
                    if (app->steering_requested) {
                        app->provider_request_ready = false;
                        rc = set_input_prompt(app, true);
                    } else
                        rc = set_input_prompt(app, true);
                }
            }
            if (!app->steering_requested && !app->model_switch_requested && rc >= 0 &&
                set_input_prompt(app, true) < 0)
                rc = -1;
        }
    }
active_done:
    app->ui.input_received_ms = 0u;
    free(line);
    if (rc < 0) return -1;
    if (snag_ui_leaving(&app->ui) && !app->input_closed) goto again;
    return app->interrupt_requested ? 2
           : app->steering_requested || (app->control_requested && !app->applying_controls) ||
                   (app->model_switch_requested && app->session.response_open)
               ? 1
               : 0;
}
static int
commit_pending_result(struct app_state *app, const char *turn_id, const char *call_id,
    json_t *result, char *error, size_t error_size)
{
    json_t *data =
        json_pack("{s:s,s:O,s:s}", "call_id", call_id, "result", result, "turn_id", turn_id);
    json_decref(result);
    if (!data) return snag_errorf(error, error_size, "cannot allocate tool completion event");
    return commit_event(app, "tool_finished", data, error, error_size);
}
static int
terminalize_pending(struct app_state *app, const char *turn_id, const char *unstarted_reason,
    char *error, size_t error_size)
{
    char id[SNAG_ID_HEX_LEN + 1u];
    memcpy(id, turn_id, sizeof(id));
    size_t count = app->session.pending_call_count;
    for (size_t i = 0; i < count; ++i) {
        struct snag_pending_call *call = &app->session.pending_calls[i];
        json_t *result;
        if (call->finished) continue;
        char handle[SNAG_ID_HEX_LEN + 1u];
        memcpy(handle, call->process_handle, sizeof(handle));
        if (call->started && handle[0] && app->recovery_wait) {
            if (snag_tools_collect(handle, "steering_handoff", &result, error, error_size) < 0)
                return -1;
        } else {
            result = call->started ? snag_tool_result_outcome_unknown("owner_lost")
                                   : snag_tool_result_not_run(unstarted_reason);
        }
        if (result && snag_app_recovered_output(app, handle, result) < 0) {
            json_decref(result);
            return -1;
        }
        if (!result || commit_pending_result(app, id, call->call_id, result, error, error_size) < 0)
            return -1;
        if (handle[0] && app->recovery_wait) snag_tools_collected(handle);
    }
    return 0;
}
int
snag_app_close_active_processes(struct app_state *app, const char *turn_id, const char *cause,
    bool user_interrupt, char *error, size_t error_size)
{
    snag_tools_close_all(user_interrupt);
    while (app->session.process_count) {
        char handle[SNAG_ID_HEX_LEN + 1u];
        json_t *result = NULL;
        memcpy(handle, app->session.processes[0].handle, sizeof(handle));
#ifdef SNAJPAGENT_TEST_FIXTURE
        result = snag_tool_result_outcome_unknown("owner_lost");
#else
        if (snag_tools_close_managed(handle, user_interrupt, snag_app_active_input_pump, app,
                snag_ui_wake_fd(&app->ui), &result, error, error_size) < 0)
            return -1;
#endif
        if (result && snag_app_recovered_output(app, handle, result) < 0) {
            json_decref(result);
            return -1;
        }
        json_t *data = json_pack("{s:s,s:s,s:O,s:s}", "cause", cause, "handle", handle, "result",
            result, "turn_id", turn_id);
        json_decref(result);
        if (commit_event(app, "process_closed", data, error, error_size) < 0) return -1;
        snag_tools_collected(handle);
    }
    return 0;
}
static int
fail_turn(struct app_state *app, struct turn_retry *retry, const char *turn_id, const char *cause,
    const char *class_name, const char *message, char *error, size_t error_size)
{
    if (turn_retry_available(app, retry) ||
        (app->turn_policy_stopped && app->session.process_count && !app->execute)) {
        json_t *data = snag_app_turn_failed_data(turn_id, class_name, message);
        if (snag_json_set_new(
                data, "retry_attempts", json_integer((json_int_t)(retry->attempts + 1u))) < 0) {
            json_decref(data);
            return -1;
        }
        int rc = commit_event(app, "turn_recovery", data, error, error_size);
        if (rc == 0) retry->pending = !app->turn_policy_stopped;
        return rc;
    }
    return snag_app_close_active_processes(app, turn_id, cause, false, error, error_size) < 0 ||
                   commit_event(app, "turn_failed",
                       snag_app_turn_failed_data(turn_id, class_name, message), error,
                       error_size) < 0
               ? -1
               : 0;
}

static int
interrupt_turn(struct app_state *app, const char *turn_id, const char *cause, bool user_interrupt,
    const char *origin, const char *reason, char *error, size_t error_size)
{
    char id[SNAG_ID_HEX_LEN + 1u];
    snag_app_irc_summary_close(app);
    memcpy(id, turn_id, sizeof(id));
    if (snag_app_close_active_processes(app, id, cause, user_interrupt, error, error_size) < 0)
        return -1;
    if (app->input_closed && !app->session.cancel_requested)
        return commit_event(app, "turn_recovery",
            snag_app_turn_failed_data(
                id, "internal", "Session closed; unfinished turn retained for resume."),
            error, error_size);
    return commit_event(app, "turn_interrupted",
        json_pack("{s:s,s:s,s:s}", "origin", origin, "reason", reason, "turn_id", id), error,
        error_size);
}

static int
record_response_failure(struct app_state *app, struct turn_retry *retry, const char *turn_id,
    const char *response_id, unsigned int cycle, const char *class_name, const char *message,
    json_t *partial, unsigned int retry_count,
    const struct snag_provider_failure *provider_failure, char *error, size_t error_size)
{
    if (!partial) partial = json_array();
    json_t *data = json_pack("{s:s,s:I,s:s,s:O,s:s,s:I,s:s}", "class", class_name, "cycle",
        (json_int_t)cycle, "message", message, "partial_public", partial, "response_id",
        response_id, "retry_count", (json_int_t)retry_count, "turn_id", turn_id);
    json_decref(partial);
    if (data &&
        (snag_json_set_new(data, "policy_stopped", json_boolean(app->turn_policy_stopped)) < 0 ||
            snag_json_set_new(data, "turn_retry_attempts",
                json_integer((json_int_t)(retry->attempts + 1u))) < 0 ||
            snag_json_set_new(data, "new_input", json_boolean(retry->new_input)) < 0)) {
        json_decref(data);
        data = NULL;
    }
    if (data && provider_failure && snag_provider_failure_is_policy(provider_failure) &&
        snag_json_set_new(data, "policy",
            json_pack("{s:s,s:s,s:s}", "code", provider_failure->code, "type",
                provider_failure->type, "clarification_skipped",
                provider_failure->clarification_skipped)) < 0) {
        json_decref(data);
        data = NULL;
    }
    if (!data) return snag_errorf(error, error_size, "cannot allocate response failure event");
    return commit_event(app, "response_failed", data, error, error_size);
}

static int
fail_response(struct app_state *app, struct turn_retry *retry, const char *turn_id,
    const char *response_id, unsigned int cycle, const char *class_name, const char *message,
    json_t *partial, unsigned int retry_count, const char *cause,
    const struct snag_provider_failure *provider_failure, char *error, size_t error_size)
{
    return record_response_failure(app, retry, turn_id, response_id, cycle, class_name, message,
               partial, retry_count, provider_failure, error, error_size) < 0 ||
            fail_turn(app, retry, turn_id, cause, class_name, message, error, error_size) < 0
        ? -1 : 0;
}

static int
start_turn_fallback(struct app_state *app, struct turn_retry *retry, const char *turn_id,
    const char *response_id, unsigned int cycle, unsigned int retry_count,
    const struct snag_provider_failure *failure, char *error, size_t error_size)
{
    if (app->session.turn_fallback_used || failure->new_input || app->interrupt_requested ||
        app->input_closed || app->model_switch_requested || app->control_requested ||
        (strcmp(failure->code, "cyber_policy") && strcmp(failure->type, "cyber_policy")) ||
        !strcmp(fallback_model(app), "off")) return 0;
    struct snag_model_selection selected = {0};
    char selection_error[256] = {0};
    if (resolve_fallback(app, fallback_model(app), &selected, selection_error,
            sizeof(selection_error)) < 0) {
        if (app_warning(app, selection_error) < 0) return -1;
        return 0;
    }
    json_t *partial = snag_app_partial_public_json(app);
    if (!partial) return -1;
    char message[256];
    (void)snag_strcpy(message, sizeof(message), error[0] ? error : "provider cyber_policy error");
    if (record_response_failure(app, retry, turn_id, response_id, cycle, "provider", message,
            partial, retry_count, failure, error, error_size) < 0 ||
        commit_event(app, "turn_fallback_started",
            json_pack("{s:s,s:s,s:s,s:s,s:s,s:I}", "turn_id", turn_id,
                "provider", selected.provider->name, "model", selected.model, "effort",
                selected.effort, "context_mode", snag_context_mode_name(selected.context.mode),
                "context_tokens", (json_int_t)selected.context.tokens), error, error_size) < 0)
        return -1;
    if (app_textf(app, SNAG_UI_HOST, "Fallback for this turn: %s/%s/%s (cyber_policy)",
            selected.provider->name, selected.model, selected.effort) < 0) return -1;
    return 1;
}

static int
recover_session(struct app_state *app, char *error, size_t error_size)
{
    struct snag_session *session = &app->session;
    if (session->active_compact_id[0] &&
        commit_event(app, "compaction_interrupted",
            json_pack("{s:s,s:s}", "compact_id", session->active_compact_id, "reason", "error"),
            error, error_size) < 0)
        return -1;
    if (!session->active_turn) return 0;
    char turn_id[SNAG_ID_HEX_LEN + 1u];
    memcpy(turn_id, session->active_turn_id, sizeof(turn_id));
    if (session->response_open &&
        commit_event(app, "response_interrupted",
            snag_app_response_interrupted_data(turn_id, session->active_response_id,
                session->active_cycle, "recovery", "process_lost",
                session->response_public ? json_copy(session->response_public) : NULL),
            error, error_size) < 0)
        return -1;
    if (session->cancel_requested) {
        if (terminalize_pending(app, turn_id, "turn_cancelled", error, error_size) < 0 ||
            interrupt_turn(app, turn_id, "user_interrupt", false, "user", "cancelled", error,
                error_size) < 0 ||
            snag_app_goal_pause(app, "user", error, error_size) < 0)
            return -1;
        return app_warning(app, "recovered deliberate turn cancellation");
    }
    if (session->response_handoff ||
        (!session->policy_stopped && session->goal_status != SNAG_GOAL_ACTIVE &&
            session->turn_retry_attempts > session->turn_retry_limit)) {
        if (terminalize_pending(app, turn_id, "recovery_unstarted", error, error_size) < 0 ||
            snag_app_close_active_processes(
                app, turn_id, "internal_failure", false, error, error_size) < 0 ||
            commit_event(app, "turn_failed",
                snag_app_turn_failed_data(turn_id, "provider",
                    session->response_handoff ? "Failed request handed off to accepted new input."
                                              : "Automatic retry budget exhausted before shutdown; "
                                                "use /retry to continue."),
                error, error_size) < 0)
            return -1;
        return 0;
    }
    if (session->response_complete && snag_session_pending_steering_unadmitted(session) &&
        !session->process_count &&
        (session->response_outcome == SNAG_GRAPH_FINAL ||
            session->response_outcome == SNAG_GRAPH_REFUSAL)) {
        bool refusal = session->response_outcome == SNAG_GRAPH_REFUSAL;
        if (commit_event(app, "turn_completed",
                snag_app_turn_completed_data(
                    turn_id, session->final_response_id, session->final_item_id),
                error, error_size) < 0)
            return -1;
        if (refusal && snag_app_goal_pause(app, "refusal", error, error_size) < 0) return -1;
        return app_warning(app, "recovered a durably completed turn");
    }
    /* Reconstruct facts, never rerun a tool from its journal entry. Results
     * without a durable completion remain explicitly uncertain. */
    if (terminalize_pending(app, turn_id, "recovery_unstarted", error, error_size) < 0 ||
        snag_app_close_active_processes(
            app, turn_id, "internal_failure", false, error, error_size) < 0 ||
        commit_event(app, "turn_recovery",
            snag_app_turn_failed_data(turn_id, "internal",
                "Session resumed; retain completed results and inspect uncertain side effects "
                "before further actions."),
            error, error_size) < 0)
        return -1;
    if (session->policy_stopped) {
        if (snag_app_goal_pause(app,
                session->policy_stopped == SNAG_POLICY_STOP_REFUSAL ? "refusal" : "provider_policy",
                error, error_size) < 0)
            return -1;
        return app_warning(
            app, "recovered provider policy stop; clarify the task before continuing");
    }
    return app_warning(
        app, "recovered unfinished turn; continuing from durable context and tool results");
}
static int
finish_call(struct app_state *app, const char *turn_id, const struct snag_response_item *call,
    const char *handle, json_t *result, const char *insert, char *error, size_t error_size)
{
    if (!result || snag_tools_attach_output_limit(call, app->config, result) < 0) {
        json_decref(result);
        return -1;
    }
    /* Policy insertions are trusted host projections delivered with the call's
     * own outcome, so they are journaled and replayed with it. */
    if (insert && *insert) {
        const char *old = snag_json_string(result, "model_text");
        struct snag_buf text;
        snag_buf_init(&text, SNAG_RULE_TEXT_MAX + 8192u);
        if (snag_buf_printf(&text, "%s\n[policy] %s", old ? old : "", insert) < 0 ||
            snag_buf_terminate(&text) < 0 ||
            snag_json_set_new(result, "model_text", json_string((const char *)text.data)) < 0) {
            snag_buf_free(&text);
            json_decref(result);
            return -1;
        }
        snag_buf_free(&text);
    }
    struct snag_process_state *process = snag_session_process(&app->session, handle);
    json_t *ref = json_object_get(result, "output_ref");
    if (ref && process &&
        (snag_json_set_new(ref, "log_start", json_integer((json_int_t)process->log_offset)) < 0 ||
            snag_json_set_new(ref, "log_end", json_integer((json_int_t)app->session.log_end)) <
                0)) {
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

/* One model tool call is filtered before any native preparation or dispatch.
 * Matching is filtering, not containment: dispatch and the tool's own argument
 * validation still decide what may run. */
struct call_rule_host {
    struct app_state *app;
    char message[512];
};

static int
call_rule_effect(void *opaque, const struct snag_rule *rule, struct snag_rule_frame *frame,
    char *error, size_t error_size)
{
    struct call_rule_host *host = opaque;
    const char *tool = snag_json_string(frame->envelope, "tool");
    const char *message = snag_rule_message(rule);
    char line[256];
    json_t *data;
    int rc;

    /* Fixed audit line: every match is journaled the same way, so no rule can
     * smuggle formatting through the log. */
    (void)snprintf(line, sizeof(line), "rule=%s decision=%s tool=%.128s", snag_rule_name(rule),
        snag_rule_verb(rule) == SNAG_RULE_DENY ? "deny" : "allow", tool ? tool : "");
    data =
        json_pack("{s:s,s:s,s:s}", "rule", snag_rule_name(rule), "chain", "out", "message", line);
    if (!data) return -1;
    rc = snag_app_commit_event(host->app, "rule_log", data, error, error_size);
    if (rc < 0) return -1;
    if (snag_rule_verb(rule) == SNAG_RULE_DENY && message)
        (void)snprintf(host->message, sizeof(host->message), "%s", message);
    return 0;
}

static int
call_rule_check(struct app_state *app, const struct snag_response_item *call, bool *rejected,
    char *message, size_t message_size, char **insertion, char *error, size_t error_size)
{
    struct call_rule_host host;
    struct snag_rule_frame frame;
    struct snag_rule_verdict verdict;
    struct snag_buf text;
    json_t *envelope, *arguments;
    bool owned;
    char rule_error[256] = "";

    *rejected = false;
    message[0] = '\0';
    *insertion = NULL;
    if (snag_rules_empty(app->config->rules)) return 0;
    owned = true;
    arguments = snag_response_arguments(call);
    snag_buf_init(&text, SNAG_MAX_TOOL_ARGUMENTS);
    if (!arguments || snag_json_diagnostic(arguments, &text) < 0 || snag_buf_terminate(&text) < 0) {
        snag_buf_free(&text);
        if (owned) json_decref(arguments);
        return snag_errorf(error, error_size, "tool call could not be canonicalized for rules");
    }
    envelope = json_pack("{s:s,s:s,s:s,s:s,s:O,s:s}", "boundary", "out", "kind", "tool_call",
        "surface", "model", "tool", call->name ? call->name : "", "value", arguments, "text",
        (const char *)text.data);
    snag_buf_free(&text);
    if (owned) json_decref(arguments);
    if (!envelope) return -1;
    memset(&host, 0, sizeof(host));
    host.app = app;
    frame.envelope = envelope;
    if (snag_rules_eval(app->config->rules, &frame, call_rule_effect, &host, &verdict, rule_error,
            sizeof(rule_error)) < 0) {
        json_decref(envelope);
        return snag_errorf(error, error_size, "rule evaluation failed: %s",
            rule_error[0] ? rule_error : "invalid rules");
    }
    json_decref(envelope);
    if (verdict.rejected) {
        *rejected = true;
        (void)snprintf(message, message_size, "%s",
            host.message[0] ? host.message : "Tool call rejected by the configured rules.");
    }
    return 0;
}

struct call_slot {
    struct snag_response_item call;
    char handle[SNAG_ID_HEX_LEN + 1u];
    char action_sha256[SNAG_SHA256_HEX_LEN + 1u];
    bool started, finished, process;
    bool rule_rejected;
    char rule_message[512];
    char *insertion;
};

static int
run_call_batch(struct app_state *app, const char *turn_id, const struct snag_credential *credential,
    struct call_slot *calls, size_t count, char *error, size_t error_size)
{
    size_t finished = 0u;
    uint64_t began = snag_monotonic_ms(), deadline = UINT64_MAX;
    const char *handoff = NULL;
    int control = 0;
    bool first_wave = true;
    /* Steering is a handoff after this accepted response's valid calls cross
     * their durable admission boundary, not permission to discard the response.
     * Commands started below are returned alive after the admission wave; short
     * adapters retain their factual outcome. */
    bool steering_handoff =
        app->session.pending_steering_count != 0u && !app->session.steering_deferred;

    /* Bind each proposal before cd can change the execution directory for
     * later calls. Replay retains this response-time identity independently
     * of the current directory recorded at admission. */
    for (size_t i = 0u; i < count; ++i) {
        if (snag_tool_action_digest(&calls[i].call, app->session.cwd, calls[i].action_sha256) < 0 ||
            call_rule_check(app, &calls[i].call, &calls[i].rule_rejected, calls[i].rule_message,
                sizeof(calls[i].rule_message), &calls[i].insertion, error, error_size) < 0)
            return -1;
    }
    while (finished < count) {
        size_t before = finished;
        bool pending = false;
        for (size_t i = 0u; i < count; ++i) {
            const struct snag_response_item *call = &calls[i].call;
            struct snag_config process_config = *app->config;
            process_config.default_yield_ms = app->session.default_yield_ms;
            process_config.max_wait_ms = app->session.max_wait_ms;
            process_config.max_parallel_commands = app->session.max_parallel_commands;
            process_config.default_timeout_ms = app->session.default_timeout_ms;
            process_config.max_timeout_ms = app->session.max_timeout_ms;
            process_config.max_output_tokens = app->session.tool_output_bytes;
            process_config.output_cache_bytes = app->session.output_cache_bytes;
            if (app->session.command_shell[0]) process_config.shell = app->session.command_shell;
            json_t *result = NULL;
            bool refresh = false, cancelled = false;
            if (calls[i].finished) continue;
            if (calls[i].rule_rejected) {
                result =
                    snag_tool_result("not_run", "rule_rejected", calls[i].rule_message, -1, 0u);
                if (!result) return -1;
                goto complete;
            }
            control = snag_app_active_input_pump(app, 0u);
            if (control < 0) {
                snag_errorf(error, error_size, "active input or command processing failed");
                return -1;
            }
            if (control == 2 || app->interrupt_requested) {
                handoff = "turn_cancelled";
                goto handoff;
            }
            if (control == 1 || app->irc_urgent.len ||
                (app->irc_sleep_released && app->irc_background.len)) {
                if (snag_app_irc_flush_urgent(app, error, error_size) < 0) return -1;
                steering_handoff = true;
            }
            if (app->yield_requested) {
                handoff = "operator_yield";
                goto handoff;
            }
            if (calls[i].started) {
                if (snag_tools_ready(calls[i].handle)) {
                    if (snag_tools_collect(calls[i].handle, NULL, &result, error, error_size) < 0 ||
                        !result)
                        return -1;
                    goto complete;
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
                result = snag_tool_result(
                    "not_run", "read_only", "Tool unavailable: this turn is read-only.", -1, 0u);
                if (!result) return -1;
            } else if (calls[i].process) {
                uint32_t yield_ms = 0u;
                int rc = snag_tools_prepare(call, &process_config, app->session.cwd,
                    app->session.max_parallel_commands, calls[i].handle, &yield_ms, &result);
                if (rc < 0) return -1;
                /* An explicitly requested zero yields after this admission
                 * wave. An omitted/null yield with default_yield_ms=0 waits
                 * for the configured max_wait_ms instead. */
                const json_t *requested_yield = json_object_get(call->arguments, "yield_ms");
                if (!requested_yield)
                    requested_yield = json_object_get(call->arguments, "yield_time_ms");
                bool immediate =
                    json_is_integer(requested_yield) && json_integer_value(requested_yield) == 0;
                if ((yield_ms || immediate) && began <= UINT64_MAX - yield_ms &&
                    began + yield_ms < deadline)
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
            if (result) goto complete;
            if (commit_event(app, "tool_started",
                    json_pack("{s:s,s:s,s:s,s:s}", "action_sha256", calls[i].action_sha256,
                        "call_id", call->call_id, "resolved_workdir", app->session.cwd, "turn_id",
                        turn_id),
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
            if (snag_ui_send(&app->ui, (struct snag_ui_command){.kind = SNAG_UI_SPINNERS,
                                           .data.value = prompt_spinner_states(app)}) < 0)
                return -1;
            int rc = calls[i].process
                         ? snag_tools_start(call, &process_config, credential, app->session.cwd,
                               &result, error, error_size)
                         : snag_app_tool_run(app, call, credential, &result, error, error_size);
            app->tool_active = false;
            if (rc < 0) {
                json_decref(result);
                result =
                    snag_tool_result_terminal(false, error[0] ? error : "Tool adapter failed.");
            }
            refresh = true;
            cancelled = rc == 2 || app->interrupt_requested;
        complete:
            if (result) {
                if (finish_call(app, turn_id, call, calls[i].started ? calls[i].handle : NULL,
                        result, calls[i].insertion, error, error_size) < 0)
                    return -1;
                free(calls[i].insertion);
                calls[i].insertion = NULL;
                calls[i].finished = true;
                ++finished;
            } else {
                pending = true;
            }
            if (cancelled) {
                handoff = "turn_cancelled";
                goto handoff;
            }
            if (refresh &&
                snag_ui_send(&app->ui, (struct snag_ui_command){.kind = SNAG_UI_SPINNERS,
                                           .data.value = prompt_spinner_states(app)}) < 0)
                return -1;
        }
        first_wave = false;
        if (finished == count) break;
        if (steering_handoff) {
            handoff = "steering_handoff";
            goto handoff;
        }
        if (!pending) {
            if (finished != before) continue;
            /* Only capacity-blocked calls remain; no invocation here can free it. */
            handoff = "process_limit";
            goto handoff;
        }
        if (snag_monotonic_ms() >= deadline) {
            handoff = "batch_yield";
            goto handoff;
        }
        if (snag_tools_service(10, snag_ui_wake_fd(&app->ui), error, error_size) < 0) return -1;
    }
    return 0;

handoff:
    if (!strcmp(handoff, "turn_cancelled")) snag_tools_close_all(true);
    for (size_t i = 0u; i < count; ++i) {
        json_t *result = NULL;
        if (calls[i].finished) continue;
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
            /* Steering attempted every call in the admission wave. Anything
             * still unstarted was capacity-blocked, not superseded by input. */
            result = snag_tool_result_not_run(
                !strcmp(handoff, "steering_handoff") ? "process_limit" : handoff);
        }
        if (finish_call(app, turn_id, &calls[i].call, calls[i].started ? calls[i].handle : NULL,
                result, calls[i].insertion, error, error_size) < 0)
            return -1;
        free(calls[i].insertion);
        calls[i].insertion = NULL;
    }
    if (!strcmp(handoff, "turn_cancelled")) {
        app->interrupt_requested = true;
        if (interrupt_turn(
                app, turn_id, "user_interrupt", true, "user", "cancelled", error, error_size) < 0)
            return -1;
        return 2;
    }
    return 0;
}

static int
execute_calls(struct app_state *app, const char *turn_id, const struct snag_response_graph *graph,
    const struct snag_credential *credential, char *error, size_t error_size)
{
    struct call_slot *calls;
    size_t count = 0u, slot = 0u;
    int rc;

    for (size_t i = 0u; i < graph->count; ++i)
        if (snag_response_graph_item(graph, i).kind == SNAG_ITEM_TOOL_CALL) ++count;
    calls = count ? calloc(count, sizeof(*calls)) : NULL;
    if (count && !calls) {
        snag_errorf(error, error_size, "cannot allocate the call batch");
        return -1;
    }
    for (size_t i = 0u; i < graph->count; ++i) {
        struct snag_response_item view = snag_response_graph_item(graph, i);
        const struct snag_response_item *call = &view;
        if (call->kind != SNAG_ITEM_TOOL_CALL) continue;
        calls[slot].call = *call;
#ifndef SNAJPAGENT_TEST_FIXTURE
        calls[slot].process =
            !app->session.active_read_only &&
            (!strcmp(call->name, "exec_command") || !strcmp(call->name, "write_stdin"));
        app->tool_waiting |= calls[slot].process;
#endif
        ++slot;
    }
    rc = run_call_batch(app, turn_id, credential, calls, count, error, error_size);
    free(calls);
    return rc;
}

/* Return the command exit status after the durable transition. Pre-response
 * failures retain process ownership while a retry remains available. */
static int
finish_turn_failure(struct app_state *app, struct turn_retry *retry, const char *turn_id,
    const char *cause, const char *class_name, const char *message, char *error, size_t error_size)
{
    int rc =
        (cause || turn_retry_available(app, retry) || retry->attempts || app->session.process_count)
            ? fail_turn(app, retry, turn_id, cause ? cause : "internal_failure", class_name,
                  message, error, error_size)
            : commit_event(app, "turn_failed",
                  snag_app_turn_failed_data(turn_id, class_name, message), error, error_size);

    (void)app_error(app, rc < 0 ? error : message);
    return rc < 0 ? 3 : 4;
}

static int
finish_user_interrupt(struct app_state *app, const char *turn_id, char *error, size_t error_size)
{
    if (interrupt_turn(
            app, turn_id, "user_interrupt", true, "user", "cancelled", error, error_size) < 0) {
        (void)app_error(app, error[0] ? error : "interruption could not be persisted");
        return 3;
    }
    (void)app_warning(app, "turn interrupted");
    return app->execute ? 6 : 1;
}

/* The caller owns the ordinary-turn budget; active goals have no ceiling. */
static int
turn_recovery_wait(struct app_state *app, struct turn_retry *retry)
{
    enum snag_goal_status initial_goal_status = app->session.goal_status;
    bool goal = initial_goal_status == SNAG_GOAL_ACTIVE;
    bool policy = app->turn_policy_stopped;
    unsigned int delay = app->recovery_delay_ms ? app->recovery_delay_ms : 250u;
    uint64_t deadline = snag_monotonic_ms() + delay;
    app->recovery_delay_ms = delay < 30000u / 2u ? delay * 2u : 30000u;
    app->recovery_wait = true;
    /* Retried requests and policy stops remain future steering targets; only
     * foreground slash commands take the composer away. */
    if (!app->execute) (void)ensure_turn_prompt(app);
    app->steering_requested = false;
    if (policy) {
        (void)app_warning(app,
            "Provider policy rejection; press Ctrl-C, then clarify the task to continue. "
            "Running commands retained.");
    } else if (delay <= 1000u || snag_monotonic_ms() - app->recovery_status_ms >= 30000u) {
        if (goal)
            (void)app_textf(app, SNAG_UI_HOST,
                "Goal active; retrying after error in %.2f seconds (Ctrl-C interrupts)",
                delay / 1000.0);
        else
            (void)app_textf(app, SNAG_UI_HOST,
                "Retrying turn after error (%llu/%u) in %.2f seconds (Ctrl-C interrupts)",
                (unsigned long long)retry->attempts, retry->limit, delay / 1000.0);
        app->recovery_status_ms = snag_monotonic_ms();
    }
    while (!app->input_closed && (policy || snag_app_retry_allowed(app)) &&
           (goal ? (app->session.goal_status == SNAG_GOAL_ACTIVE ||
                       (app->session.goal_status == SNAG_GOAL_PAUSED && app->session.process_count))
                 : app->session.goal_status == initial_goal_status) &&
           (policy || snag_monotonic_ms() < deadline ||
               (goal && app->session.goal_status == SNAG_GOAL_PAUSED))) {
        int rc = snag_app_active_input_pump(app, 25u);
        if (rc == 2) break;
        if (app->control_requested && !app->session.response_open &&
            !app->session.pending_call_count) {
            if (apply_controls(app) < 0) {
                app->recovery_wait = false;
                return -1;
            }
            if (app->input_closed || app->interrupt_requested) break;
            if (!app->steering_requested) continue;
        }
        if (rc == 1) {
            if (policy || !goal || app->session.goal_status != SNAG_GOAL_PAUSED) break;
            app->steering_requested = false;
        }
        /* One-shot has no terminal poll; a failed input owner must not spin. */
        if (app->execute || rc < 0) (void)snag_sleep_ms(25u);
    }
    app->recovery_wait = false;
    if (app->interrupt_requested || app->input_closed) return 2;
    if (!policy && !snag_app_retry_allowed(app)) return 3;
    if (policy && app->steering_requested) {
        app->turn_policy_stopped = SNAG_POLICY_STOP_NONE;
        app->steering_requested = false;
        return 0;
    }
    if (app->session.goal_status != initial_goal_status) return 1;
    app->steering_requested = false;
    return 0;
}

static int
wait_for_command_or_input(struct app_state *app)
{
    /* Ready results need collection immediately. A live command may instead
     * need the answer to the question that the model has just displayed. */
    for (size_t i = 0u; i < app->session.process_count; ++i)
        if (snag_tools_ready(app->session.processes[i].handle)) return 3;
    if (app_textf(app, SNAG_UI_HOST,
            "Waiting for command completion or your input; /yield returns to the model, "
            "Ctrl-C interrupts.") < 0 ||
        ensure_turn_prompt(app) < 0)
        return -1;
    app->tool_waiting = true;
    int rc = 0;
    while (!app->input_closed && !app->interrupt_requested) {
        rc = snag_app_active_input_pump(app, 25u);
        if (rc < 0 || rc == 2) break;
        if (app->control_requested && apply_controls(app) < 0) {
            rc = -1;
            break;
        }
        if (app->steering_requested || app->yield_requested || app->irc_urgent.len ||
            (app->irc_sleep_released && app->irc_background.len))
            break;
        bool ready = false;
        for (size_t i = 0u; i < app->session.process_count; ++i)
            ready |= snag_tools_ready(app->session.processes[i].handle);
        if (ready) break;
    }
    app->tool_waiting = app->yield_requested = false;
    if (app->input_closed || app->interrupt_requested) return 2;
    return rc < 0 ? -1 : 0;
}

static int
admit_input(struct app_state *app, char *error, size_t error_size)
{
    json_t *ids = json_array();
    if (!ids) return -1;
    for (size_t i = 0; !app->session.steering_deferred && i < app->session.pending_steering_count;
        ++i) {
        const struct snag_pending_steering *p = &app->session.pending_steering[i];
        if (!p->first_context_ms && json_array_append_new(ids, json_string(p->steering_id)) < 0) {
            json_decref(ids);
            return -1;
        }
    }
    if (app->session.input_first_context_ms && !json_array_size(ids)) {
        json_decref(ids);
        return 0;
    }
    json_t *data = json_pack("{s:O,s:I,s:s}", "steering_ids", ids, "time_ms",
        (json_int_t)snag_time_ms(), "turn_id", app->session.active_turn_id);
    json_decref(ids);
    return commit_event(app, "input_admitted", data, error, error_size);
}

static int
run_turn(struct app_state *app, struct turn_retry *retry, const char *prompt,
    const struct snag_queued_turn *queued, bool goal_turn, bool timer_turn, bool read_only,
    const json_t *content)
{
    char turn_id[SNAG_ID_HEX_LEN + 1u];
    char response_id[SNAG_ID_HEX_LEN + 1u];
    char error[256];
    const char *report_message = error;
    char provider_source_hash[SNAG_SHA256_HEX_LEN + 1u];
    char rejected_request_hash[SNAG_SHA256_HEX_LEN + 1u] = {0};
    char over_budget_request_hash[SNAG_SHA256_HEX_LEN + 1u] = {0};
    unsigned int hard_compaction_attempts = 0u, cyber_clarifications = 0u;
    bool continuing = app->session.active_turn;
    bool fallback_started = false;
    unsigned int next_cycle = continuing ? app->session.active_cycle + 1u : 1u;
    struct snag_credential credential;
    struct snag_response_graph graph;
    json_t *steering = NULL;
    struct snag_context_projection projection = {0};
    struct snag_execution_config execution = {0};
    struct snag_buf request_body = {0};
    size_t prompt_max = queued ? SNAG_MAX_QUEUED_TEXT : SNAG_MAX_DIRECT_PROMPT;
    int result = 4;
    snag_credential_clear(&credential);
    error[0] = '\0';
    if (!snag_text_valid(prompt, 1u, prompt_max)) {
        (void)app_error(app, queued ? "queued prompt must be nonempty valid UTF-8 within 256 KiB"
                                    : "prompt must be nonempty valid UTF-8 within 1 MiB");
        return 2;
    }
    if (persist_session(app, error, sizeof(error)) < 0) {
        (void)app_error(app, error);
        return 3;
    }
    if (read_only && app->networked && !app->execute &&
        select_view(app, SNAG_RENDER_ROLLOUT, false) < 0)
        return 6;
    if (prepare_turn_settings(app, error, sizeof(error)) < 0) {
        (void)app_error(app, error);
        return 2;
    }
    if (continuing) {
        execution = (struct snag_execution_config){app->session.default_yield_ms,
            app->session.max_wait_ms, app->session.max_parallel_commands,
            app->session.default_timeout_ms, app->session.max_timeout_ms,
            app->session.tool_output_bytes, app->session.output_cache_bytes};
    } else if (snag_config_resolve_execution(app->config, app->turn_provider->name, app->turn_model,
                   &execution, error, sizeof(error)) < 0) {
        (void)app_error(app, error);
        return 2;
    }
    provider_capacity_source_sha256(app->turn_provider, app->turn_model, provider_source_hash);
    graph = (struct snag_response_graph){0};
    if (continuing) {
        snag_instructions_free(&app->turn_instructions);
        if (app->session.active_instructions &&
            snag_instructions_metadata_valid(
                app->session.active_instructions, error, sizeof(error)) < 0)
            goto fail;
        for (size_t i = 0; i < json_array_size(app->session.active_instructions); ++i) {
            char *path = snag_strdup_checked(
                json_string_value(json_array_get(app->session.active_instructions, i)),
                SNAG_PATH_MAX_BYTES);
            if (!path) goto fail;
            if (snag_instructions_add_owned(&app->turn_instructions, path, error, sizeof(error)) <
                0)
                goto fail;
        }
    } else {
        if (app->config->read_agents_md) {
            if (snag_instructions_discover(
                    &app->turn_instructions, app->session.cwd, error, sizeof(error)) < 0)
                goto fail;
        } else
            snag_instructions_free(&app->turn_instructions);
        json_t *saved = app->session.pending_input
                            ? json_object_get(app->session.pending_input, "instructions")
                            : NULL;
        size_t count = saved ? json_array_size(saved) : app->cli->doc_instructions.count;
        for (size_t i = 0; i < count; ++i) {
            const char *path = saved ? json_string_value(json_array_get(saved, i))
                                     : app->cli->doc_instructions.paths[i];
            if (snag_instructions_add_file(&app->turn_instructions, path, error, sizeof(error)) < 0)
                goto fail;
        }
    }
    if (continuing) {
        memcpy(turn_id, app->session.active_turn_id, sizeof(turn_id));
    } else if (snag_random_id(turn_id) < 0) {
        report_message = "cryptographic turn id generation failed";
        goto fail;
    }
    if (!continuing) {
        app->turn_started_ms = snag_monotonic_ms();
        app->turn_output_tokens = 0u;
        app->turn_prompt_seen = false;
    }
    if (!continuing &&
        commit_input(app, "turn_started",
            json_pack(
                "{s:{s:s,s:s,s:o,s:s,s:s,s:s,s:i,s:i,s:i,s:i,s:I,s:I,s:I,s:I,s:I,s:I,s:b,s:I},"
                "s:s,s:o,s:I,s:b,s:s?,s:o,s:s,s:s,s:I,s:s}",
                "config", "capability_version", SNAJPAGENT_CAPABILITY_VERSION, "effort",
                app->turn_effort, "max_output_tokens",
                app->turn_capacity.max_output_tokens
                    ? json_integer((json_int_t)app->turn_capacity.max_output_tokens)
                    : json_null(),
                "model", app->turn_model, "provider", app->turn_provider->name, "profile_id",
                SNAJPAGENT_PROFILE_ID, "prompt_schema", 1, "replay_schema", 1, "tool_schema", 1,
                "max_parallel_commands", (int)execution.max_parallel_commands, "default_yield_ms",
                (json_int_t)execution.default_yield_ms, "max_wait_ms",
                (json_int_t)execution.max_wait_ms, "default_timeout_ms",
                (json_int_t)execution.default_timeout_ms, "max_timeout_ms",
                (json_int_t)execution.max_timeout_ms, "tool_output_bytes",
                (json_int_t)execution.tool_output_bytes, "output_cache_bytes",
                (json_int_t)execution.output_cache_bytes, "parallel_tool_calls",
                app->turn_provider->parallel_tool_calls, "max_turn_retries",
                (json_int_t)retry->limit, "input_kind",
                goal_turn    ? "goal"
                : queued     ? "queued"
                : timer_turn ? "timer"
                             : "direct",
                "instructions", snag_instructions_metadata_json(&app->turn_instructions),
                "received_at_ms",
                (json_int_t)(queued ? queued->received_ms : app->input_received_ms), "read_only",
                read_only, "queue_id", queued ? queued->queue_id : NULL, "queue_seq",
                queued ? json_integer((json_int_t)queued->seq) : json_null(), "text", prompt,
                "turn_id", turn_id, "turn_number", (json_int_t)(app->session.turn_count + 1u),
                "cwd", app->session.cwd),
            content, error, sizeof(error)) < 0) {
        goto fail;
    }
    if (!continuing && queued && !app->execute && render_prompt(app, false, prompt) < 0) {
        report_message = "queued prompt could not be rendered";
        goto output_fail;
    }
    if (!app->execute && ensure_turn_prompt(app) < 0) {
        report_message = "active prompt could not be displayed";
        goto output_fail;
    }
    if (app_textf(app, SNAG_UI_RUNTIME, "turn › %s started%s · model=%s · effort=%s · cwd=%s",
            turn_id, read_only ? " (read-only)" : "", app->turn_model, app->turn_effort,
            app->session.cwd) < 0) {
        report_message = "turn runtime facts could not be rendered";
        goto output_fail;
    }
    for (unsigned int cycle = next_cycle; cycle != 0u; ++cycle) {
        struct snag_graph_decision decision;
        const char *count_method = "unknown";
        uint64_t response_begin_ms;
        unsigned int provider_retry_count = 0u;
        struct snag_provider_failure provider_failure;
        int provider_rc;
        snag_app_response_cycle_release(app, &graph, &steering, &projection, &request_body);
        memset(&provider_failure, 0, sizeof(provider_failure));
        error[0] = '\0';
        bool reconfigured =
            (app->session.pending_controls & (SNAG_CONTROL_CONFIG | SNAG_CONTROL_RELOAD)) != 0u;
        if (apply_controls(app) < 0) goto fail;
        if (app->input_closed) {
            result = 0;
            goto out;
        }
        if (app->interrupt_requested) goto user_interrupted;
        app->provider_request_ready = false;
        bool selected_new_model =
            app->session.active_turn &&
            (!app->session.turn_fallback_active || app->model_switch_requested) &&
            (app->session.turn_fallback_active ||
                strcmp(app->session.active_turn_provider, app->session.default_provider) != 0 ||
                strcmp(app->session.active_turn_model, app->session.default_model) != 0 ||
                strcmp(app->session.active_turn_effort,
                    resolve_effort(app->session.default_effort)) != 0);
        if (selected_new_model &&
            commit_event(app, "turn_model_changed",
                json_pack("{s:s,s:s,s:s,s:s,s:s}", "new_effort",
                    resolve_effort(app->session.default_effort), "old_provider",
                    app->session.active_turn_provider, "old_model", app->session.active_turn_model,
                    "old_effort", app->session.active_turn_effort, "turn_id", turn_id),
                error, sizeof(error)) < 0)
            goto fail;
        bool settings_changed = fallback_started || reconfigured || selected_new_model ||
            strcmp(app->turn_provider->name, app->session.active_turn_provider) ||
            strcmp(app->turn_model, app->session.active_turn_model) ||
            strcmp(app->turn_effort, app->session.active_turn_effort);
        app->model_switch_requested = false;
        fallback_started = false;
        if (settings_changed) {
            if (prepare_turn_settings(app, error, sizeof(error)) < 0) goto fail;
            provider_capacity_source_sha256(
                app->turn_provider, app->turn_model, provider_source_hash);
            if (!app->execute && set_input_prompt(app, true) < 0) goto fail;
        }
        if (!app->execute && ensure_turn_prompt(app) < 0) goto fail;
#ifndef SNAJPAGENT_TEST_FIXTURE
        /* Recovery must apply /model and /configure before checking credentials;
         * the old provider's failed snapshot must not prevent either remedy. */
        bool initial_auth = credential.len == 0u;
        if (initial_auth || settings_changed) {
            snag_credential_clear(&credential);
            if (snag_auth_read(app->store.root_fd, app->turn_provider, false, NULL, &credential,
                    snag_app_active_input_pump, app, error, sizeof(error)) < 0) {
                result = initial_auth ? 2 : 3;
                goto report;
            }
        }
#endif

        if (snag_app_irc_flush_urgent(app, error, sizeof(error)) < 0) {
            report_message = error[0] ? error : "urgent IRC input could not be admitted";
            goto fail;
        }
        if (admit_input(app, error, sizeof(error)) < 0) {
            (void)app_error(app, error);
            result = 3;
            goto out;
        }
        /* steering_requested is only the wake/interrupt edge. Once the
         * durable pending steers have been admitted to this request, keeping
         * the edge set would spuriously interrupt or add an empty cycle. */
        if (!app->session.steering_deferred) app->steering_requested = false;
        bool minimal_rebased_request = app->session.context_rebase_seq > app->session.compact_seq &&
                                       !app->session.context_rebase_has_new_results &&
                                       strcmp(app->session.context_rebase_turn_id, turn_id) == 0;
        /* A rejection survives fresh-input handoff, interruption and reopen. Only
         * the rejected binding and uncompacted lineage require preparation here. */
        if (snag_input_observation_matches(&app->session.capacity_rejection,
                app->turn_provider->name, app->turn_model, app->turn_effort, provider_source_hash,
                app->session.compact_id)) {
            app->history_orientation = SNAG_HISTORY_ORIENTATION_RECOVERY;
            app->history_recovery_rebase = !minimal_rebased_request;
            apply_capacity_ceiling(app, app->turn_provider, app->turn_model, &app->turn_capacity);
            /* A rejected provider request survives a restart. Project the
             * durable current turn, not another pass over the same archive. */
        }
        steering = snag_app_steering_snapshot(&app->session);
        error[0] = '\0';
        if (!steering || snag_random_id(response_id) < 0) {
            if (app->interrupt_requested) goto user_interrupted;
            result = finish_turn_failure(app, retry, turn_id, NULL, "context",
                error[0] ? error : "response context projection failed", error, sizeof(error));
            goto out;
        }
        bool pure_history_recovery =
            minimal_rebased_request ||
            (app->history_orientation == SNAG_HISTORY_ORIENTATION_RECOVERY &&
                app->history_recovery_rebase);
        if (snag_app_request_build(app, steering, cycle, &credential, &projection, &count_method,
                &request_body, error, sizeof(error)) < 0) {
            bool image_boundary =
                errno == EFBIG && strstr(error, "Image request exceeds 12 MiB") != NULL;
            if (app->interrupt_requested) goto user_interrupted;
            if (image_boundary && !pure_history_recovery) {
                app->history_orientation = SNAG_HISTORY_ORIENTATION_RECOVERY;
                app->history_recovery_rebase = true;
                goto rebuild_request;
            }
            if (errno == E2BIG && !pure_history_recovery) {
                /* A model change can invalidate an opaque compact and restore
                 * an archive larger than the request limit. Compact a bounded
                 * prefix before projection; token counting cannot run yet. */
                bool compacted = false;
                int compact_rc = hard_compaction_attempts ? 0 :
                    snag_app_compact_oversized_request(app, &credential,
                        &compacted, error, sizeof(error));
                if (compact_rc == 1 && (app->steering_requested ||
                        app->control_requested || app->model_switch_requested))
                    goto steered_before_response;
                if (compact_rc == 2 && app->interrupt_requested) goto user_interrupted;
                if (compact_rc == 0) {
                    if (compacted) {
                        ++hard_compaction_attempts;
                    } else {
                        app->history_orientation = SNAG_HISTORY_ORIENTATION_RECOVERY;
                        app->history_recovery_rebase = true;
                    }
                    goto rebuild_request;
                }
                if (app->compaction_bounded) retry->compaction_bounded = true;
            } else if (errno == E2BIG) {
                /* Even the current-turn recovery envelope cannot fit. Repeating
                 * the identical local projection cannot make progress. */
                retry->compaction_bounded = true;
            }
            if (app->session.goal_status == SNAG_GOAL_ACTIVE)
                app->history_orientation = SNAG_HISTORY_ORIENTATION_RECOVERY;
            result = finish_turn_failure(app, retry, turn_id, NULL, "context",
                error[0] ? error : "response context projection failed", error, sizeof(error));
            goto out;
        }
        if (snag_app_provider_activity(app, true) < 0) goto fail;
        provider_rc = snag_app_provider_count(app, projection.count_request.value, &credential,
            &projection.input_tokens_bound, &count_method, error, sizeof(error));
        if (snag_app_provider_activity(app, false) < 0) goto fail;
        if (provider_rc == 1 &&
            (app->steering_requested || app->control_requested || app->model_switch_requested))
            goto steered_before_response;
        if (provider_rc == 2 && app->interrupt_requested) goto user_interrupted;
        if (provider_rc == SNAG_PROVIDER_CONTEXT_OVERFLOW) {
            if (!pure_history_recovery) {
                app->history_orientation = SNAG_HISTORY_ORIENTATION_RECOVERY;
                app->history_recovery_rebase = true;
                goto rebuild_request;
            }
            result = finish_turn_failure(app, retry, turn_id, NULL, "context",
                "input-token counter rejected the minimal recovery request", error, sizeof(error));
            goto out;
        }
        if (provider_rc != 0 && provider_rc != SNAG_APP_COUNT_SKIPPED) {
            result = finish_turn_failure(app, retry, turn_id, NULL, "provider",
                error[0] ? error : "input-token count failed", error, sizeof(error));
            goto out;
        }
        if (app->irc_urgent.len || (app->irc_sleep_released && app->irc_background.len))
            goto rebuild_request;
        {
            bool compacted = false;
            bool over_hard = !strcmp(count_method, "exact") &&
                             app->turn_capacity.hard_input_known &&
                             projection.input_tokens_bound > app->turn_capacity.hard_input_tokens;
            int compact_rc;

            if (over_hard && !pure_history_recovery && hard_compaction_attempts) {
                /* One checkpoint can summarize an ordinary prefix. If the
                 * active turn still exceeds the window, let the model fetch
                 * needed history rather than walking every older tool group.
                 * Select this fallback only after recounting the summary: an
                 * eager rebase discarded even a successful, fitting compact. */
                app->history_orientation = SNAG_HISTORY_ORIENTATION_RECOVERY;
                app->history_recovery_rebase = true;
                goto rebuild_request;
            }
            if (over_hard && pure_history_recovery) {
                char failure[256];
                (void)snprintf(failure, sizeof(failure),
                    "minimal recovery request input count %llu (%s) exceeds hard budget %llu",
                    (unsigned long long)projection.input_tokens_bound, count_method,
                    (unsigned long long)app->turn_capacity.hard_input_tokens);
                result = finish_turn_failure(
                    app, retry, turn_id, NULL, "context", failure, error, sizeof(error));
                goto out;
            }
            if (over_hard &&
                (hard_compaction_attempts >= 8u ||
                    strcmp(over_budget_request_hash, projection.create_request.sha256) == 0)) {
                char failure[256];

                (void)snprintf(failure, sizeof(failure),
                    "%s while reducing context input count %llu (%s) to hard budget %llu",
                    hard_compaction_attempts >= 8u
                        ? "context compaction reached its eight-attempt bound"
                        : "context compaction repeated an over-budget request",
                    (unsigned long long)projection.input_tokens_bound, count_method,
                    (unsigned long long)app->turn_capacity.hard_input_tokens);
                result = finish_turn_failure(
                    app, retry, turn_id, NULL, "context", failure, error, sizeof(error));
                goto out;
            }
            if (over_hard)
                memcpy(over_budget_request_hash, projection.create_request.sha256,
                    sizeof(over_budget_request_hash));
            if (over_hard && hard_compaction_attempts) {
                /* This is the rebuilt, remeasured request after earlier
                 * compaction. Only now is it truthful to say context remains
                 * over budget and another pass is actually beginning. */
                char notice[192];
                (void)snprintf(notice, sizeof(notice),
                    "context still over the model window after %u compaction%s; "
                    "compacting further (Ctrl-C interrupts)",
                    hard_compaction_attempts, hard_compaction_attempts == 1u ? "" : "s");
                (void)snag_ui_text(&app->ui, SNAG_UI_HOST, notice);
            }
            compact_rc = snag_app_compact_before_response(app, &credential,
                projection.input_tokens_bound, count_method, &compacted, error, sizeof(error));
            if (compact_rc == 1 && app->steering_requested) goto steered_before_response;
            if (compact_rc == 2 && app->interrupt_requested) goto user_interrupted;
            if (compact_rc != 0) {
                /* The consecutive-failure bound is terminal for this turn:
                 * retrying would only fail again until new input clears it. */
                if (app->compaction_bounded) retry->compaction_bounded = true;
                result = finish_turn_failure(app, retry, turn_id, NULL,
                    over_hard ? "context" : "provider",
                    error[0] ? error : "pre-response compaction failed", error, sizeof(error));
                goto out;
            }
            if (compacted) {
                if (over_hard) ++hard_compaction_attempts;
                goto rebuild_request;
            }
        }
        if (rejected_request_hash[0] &&
            strcmp(rejected_request_hash, projection.create_request.sha256) == 0) {
            static const char failure[] =
                "capacity recovery produced an identical provider request";
            result = finish_turn_failure(
                app, retry, turn_id, NULL, "context", failure, error, sizeof(error));
            goto out;
        }
        const char *request_tier =
            snag_json_string(projection.create_request.value, "service_tier");
        if (app->control_requested ||
            strcmp(request_tier ? request_tier : "",
                app->session.service_tier ? app->session.service_tier : ""))
            goto rebuild_request;
        if (app->history_recovery_rebase &&
            commit_event(app, "context_rebased",
                json_pack("{s:s,s:s}", "reason",
                    app->session.active_goal ? "goal_recovery" : "turn_recovery", "turn_id",
                    turn_id),
                error, sizeof(error)) < 0) {
            report_message = error[0] ? error : "recovery context boundary could not be persisted";
            goto fail;
        }
        app->response_started_ms = snag_monotonic_ms();
        if (commit_event(app, "response_started",
                snag_app_response_started_data(app, turn_id, response_id, cycle, &projection,
                    count_method, provider_source_hash, steering),
                error, sizeof(error)) < 0) {
            report_message = error[0] ? error : "response setup could not be persisted";
            goto fail;
        }
        app->history_orientation = SNAG_HISTORY_ORIENTATION_NONE;
        app->history_recovery_rebase = false;
        json_decref(projection.count_request.value);
        projection.count_request.value = NULL;
        if (app_textf(app, SNAG_UI_RUNTIME,
                "response › %s started · turn=%s · cycle=%u · model=%s · profile=%s", response_id,
                turn_id, cycle, app->turn_model, SNAJPAGENT_PROFILE_ID) < 0) {
            report_message = "response runtime facts could not be rendered";
            goto output_fail;
        }
        if (request_body.len &&
            snag_ui_send(&app->ui, (struct snag_ui_command){.kind = SNAG_UI_PROTOCOL,
                                       .label = "request.body",
                                       .text = (const char *)request_body.data,
                                       .len = request_body.len}) < 0) {
            json_t *partial = json_array();
            static const char failure[] = "request diagnostics could not be rendered";
            if (!partial ||
                fail_response(app, retry, turn_id, response_id, cycle, "output", failure, partial,
                    0u, "output_failure", NULL, error, sizeof(error)) < 0) {
                report_message =
                    error[0] ? error : "diagnostic output failure could not be persisted";
                goto fail;
            }
            report_message = failure;
            goto output_fail;
        }
        snag_buf_free(&request_body);
        snag_app_reset_stream(app);
        response_begin_ms = snag_time_ms();
        error[0] = '\0';
        if (snag_app_provider_activity(app, true) < 0) goto fail;
        snag_app_irc_summary_start(app, &projection, &credential);
        provider_rc = snag_app_provider_run(app, prompt, steering, cycle,
            projection.create_request.value, &credential, &graph, &provider_failure, error,
            sizeof(error), &provider_retry_count);
        app->provider_request_ready = false;
        if (snag_app_provider_activity(app, false) < 0) goto fail;
        if (provider_rc < 0 && provider_failure.retry_after_ms > app->recovery_delay_ms)
            app->recovery_delay_ms = provider_failure.retry_after_ms;
        if (provider_rc == 0) {
            int control_rc = snag_app_active_input_pump(app, 0u);
            if (control_rc != 0) provider_rc = control_rc;
        }
        json_decref(projection.create_request.value);
        projection.create_request.value = NULL;
        json_decref(steering);
        steering = NULL;
        bool steered = provider_rc == 1 && app->steering_requested;
        bool controlled =
            provider_rc == 1 && (app->control_requested || app->model_switch_requested);
        bool interrupted = provider_rc == 2 && app->interrupt_requested;
        (void)snag_app_close_stream_item(
            app, steered || interrupted ||
                     provider_failure.output_correction != SNAG_OUTPUT_CORRECTION_NONE);
        if ((steered || controlled || interrupted) && !app->stream_failed) {
            json_t *partial = snag_app_partial_public_json(app);
            if (!partial || commit_event(app, "response_interrupted",
                                snag_app_response_interrupted_data(turn_id, response_id, cycle,
                                    steered ? "steering" : "user",
                                    steered      ? "steered"
                                    : controlled ? "control"
                                                 : "cancelled",
                                    partial),
                                error, sizeof(error)) < 0) {
                report_message = error[0]  ? error
                                 : steered ? "active-turn response could not be persisted"
                                           : "interruption could not be persisted";
                goto fail;
            }
            if (steered || controlled) {
                cyber_clarifications = 0u;
                continue;
            }
            goto user_interrupted;
        }
        bool cyber_clarification =
            provider_failure.output_correction == SNAG_OUTPUT_CORRECTION_CYBER_POLICY;
        if (provider_failure.output_correction != SNAG_OUTPUT_CORRECTION_NONE &&
            !app->stream_failed &&
            (!cyber_clarification || cyber_clarifications < SNAG_CYBER_CLARIFICATIONS_MAX)) {
            static const char repeated[] =
                "assistant output remained invalid after one model-facing correction";
            const char *correction =
                cyber_clarification ? SNAG_CYBER_CLARIFICATION
                : provider_failure.output_correction == SNAG_OUTPUT_CORRECTION_EMPTY
                    ? SNAG_EMPTY_OUTPUT_CORRECTION
                    : SNAG_OVERSIZED_OUTPUT_CORRECTION;
            char correction_id[SNAG_ID_HEX_LEN + 1u];

            if (!cyber_clarification && app->session.output_correction_used) {
                app->stream_failed = true;
                app->stream_errno = EPROTO;
                (void)snprintf(app->stream_error, sizeof(app->stream_error), "%s", repeated);
            } else {
                json_t *partial;

                if (snag_random_id(correction_id) < 0)
                    partial = NULL;
                else
                    partial = snag_app_partial_public_json(app);
                json_t *data = partial ? json_pack("{s:s,s:I,s:O,s:s,s:s,s:s}", "correction_id",
                                             correction_id, "cycle", (json_int_t)cycle,
                                             "partial_public", partial, "response_id", response_id,
                                             "text", correction, "turn_id", turn_id)
                                       : NULL;
                int correction_rc = partial ? commit_event(app, "response_output_correction", data,
                                                  error, sizeof(error))
                                            : -1;
                json_decref(partial);
                if (correction_rc < 0) {
                    report_message =
                        error[0] ? error : "assistant output correction could not be persisted";
                    goto fail;
                }
                if (cyber_clarification) {
                    char notice[128];
                    snprintf(notice, sizeof(notice),
                        "provider clarification %u/%u after cyber_policy; preserving original task "
                        "scope",
                        ++cyber_clarifications, SNAG_CYBER_CLARIFICATIONS_MAX);
                    if (app_warning(app, notice) < 0) {
                        result = 6;
                        goto out;
                    }
                }
                continue;
            }
        }
        if (snag_provider_failure_is_policy(&provider_failure) &&
            !provider_failure.clarification_skipped[0])
            (void)snag_strcpy(provider_failure.clarification_skipped,
                sizeof(provider_failure.clarification_skipped),
                app->stream_failed ? "output_failure" : "clarification_limit");
        cyber_clarifications = 0u;
        if (provider_rc < 0 && !app->stream_failed) {
            int fallback_rc = start_turn_fallback(app, retry, turn_id, response_id, cycle,
                provider_retry_count, &provider_failure, error, sizeof(error));
            if (fallback_rc < 0) goto fail;
            if (fallback_rc > 0) {
                fallback_started = true;
                continue;
            }
        }
        if (provider_rc < 0 || app->stream_failed) {
            bool capacity_failure =
                provider_rc < 0 && snag_provider_failure_is_capacity(&provider_failure);
            bool replay_safe = capacity_failure && !app->stream_failed &&
                               !provider_failure.new_input && app->partial_count == 0u &&
                               graph.count == 0u && !app->stream_item_seen;
            const char *class_name = app->stream_failed
                                         ? (app->stream_errno == EPROTO         ? "protocol"
                                               : app->stream_errno == EOVERFLOW ? "resource"
                                                                                : "output")
                                     : capacity_failure ? "context"
                                                        : "provider";
            int exit_status = app->stream_failed && strcmp(class_name, "output") == 0 ? 6 : 4;
            char failure[256];
            json_t *partial;
            (void)snprintf(failure, sizeof(failure), "%s",
                app->stream_failed
                    ? (app->stream_error[0] ? app->stream_error
                                            : "assistant output could not be delivered")
                    : (error[0] ? error : "provider response failed"));
            if (replay_safe && !pure_history_recovery &&
                strcmp(rejected_request_hash, projection.create_request.sha256)) {
                if (commit_event(app, "response_capacity_rejected",
                        snag_app_response_capacity_rejected_data(turn_id, response_id, cycle,
                            projection.create_request.sha256, &provider_failure,
                            &app->turn_capacity, provider_source_hash),
                        error, sizeof(error)) < 0) {
                    report_message = error[0] ? error : "capacity rejection could not be persisted";
                    goto fail;
                }
                app->history_orientation = SNAG_HISTORY_ORIENTATION_RECOVERY;
                app->history_recovery_rebase = true;
                memcpy(rejected_request_hash, projection.create_request.sha256,
                    sizeof(rejected_request_hash));
                if (capacity_ceiling_matches(app, app->turn_provider, app->turn_model))
                    snag_app_record_model_accounting(
                        app, SNAG_COUNT_UNKNOWN, app->session.capacity_ceiling_input_tokens);
                continue;
            }
            partial = snag_app_partial_public_json(app);
            if (!partial) {
                report_message = "failed response prefix could not be retained";
                goto fail;
            }
            /* Fresh queued/chat input keeps its existing failed-request handoff. */
            retry->new_input = !app->stream_failed && provider_failure.new_input;
            if (!provider_failure.new_input && snag_provider_failure_is_policy(&provider_failure))
                app->turn_policy_stopped = SNAG_POLICY_STOP_PROVIDER;
            if (!app->stream_failed && provider_failure.new_input &&
                app->session.goal_status != SNAG_GOAL_ACTIVE)
                retry->limit = retry->attempts;
            /* A repeated identical provider rejection would replay byte-identically:
             * stop the turn instead of burning the whole budget on it. */
            if (!provider_failure.new_input &&
                !snag_provider_failure_is_policy(&provider_failure) && !capacity_failure &&
                !app->stream_failed && app->session.goal_status != SNAG_GOAL_ACTIVE &&
                provider_failure.output_correction == SNAG_OUTPUT_CORRECTION_NONE &&
                (provider_failure.code[0] || provider_failure.type[0]) &&
                !snag_provider_failure_retryable(0, provider_failure.code, provider_failure.type)) {
                if (!strcmp(retry->last_failure_code, provider_failure.code) &&
                    !strcmp(retry->last_failure_type, provider_failure.type) &&
                    !strcmp(retry->last_failure_message, provider_failure.message))
                    retry->limit = retry->attempts;
                else {
                    (void)snag_strcpy(retry->last_failure_code, sizeof(retry->last_failure_code),
                        provider_failure.code);
                    (void)snag_strcpy(retry->last_failure_type, sizeof(retry->last_failure_type),
                        provider_failure.type);
                    (void)snag_strcpy(retry->last_failure_message,
                        sizeof(retry->last_failure_message), provider_failure.message);
                }
            }
            if (fail_response(app, retry, turn_id, response_id, cycle, class_name, failure, partial,
                    provider_retry_count,
                    app->stream_failed
                        ? (app->stream_errno == EPROTO ? "protocol_failure" : "output_failure")
                        : "provider_failure",
                    &provider_failure, error, sizeof(error)) < 0) {
                goto fail;
            }
            (void)failure_notice(app, failure, retry->pending && !retry->new_input);
            result = !app->stream_failed && provider_failure.new_input ? SNAG_APP_INPUT_READY
                                                                       : exit_status;
            goto out;
        }
        if (!app->execute) {
            int input_rc = snag_app_active_input_pump(app, 0u);
            if (input_rc < 0) {
                report_message = "active input could not be processed";
                goto fail;
            }
            /* Input can arrive after the provider has returned but before its
             * complete graph is journaled. A durable Ctrl-C still wins this
             * turn boundary; do not silently turn its cancellation request
             * into a successful final answer. */
            if (input_rc == 2 || app->interrupt_requested) {
                json_t *partial = snag_app_partial_public_json(app);
                if (!partial || commit_event(app, "response_interrupted",
                                    snag_app_response_interrupted_data(
                                        turn_id, response_id, cycle, "user", "cancelled", partial),
                                    error, sizeof(error)) < 0) {
                    report_message = error[0] ? error : "late interruption could not be persisted";
                    goto fail;
                }
                goto user_interrupted;
            }
            if (input_rc == 1 && app->control_requested) {
                json_t *partial = snag_app_partial_public_json(app);
                if (!partial || commit_event(app, "response_interrupted",
                                    snag_app_response_interrupted_data(
                                        turn_id, response_id, cycle, "user", "control", partial),
                                    error, sizeof(error)) < 0) {
                    report_message = error[0] ? error : "late control could not be persisted";
                    goto fail;
                }
                continue;
            }
        }
        if (snag_response_graph_classify(&graph, &decision, error, sizeof(error)) < 0) {
            char failure[256];
            json_t *partial;
            (void)snprintf(failure, sizeof(failure), "%s",
                error[0] ? error : "invalid provider response graph");
            partial = snag_app_partial_public_json(app);
            if (!partial) {
                report_message = "invalid response prefix could not be retained";
                goto fail;
            }
            if (fail_response(app, retry, turn_id, response_id, cycle, "protocol", failure, partial,
                    provider_retry_count, "protocol_failure", NULL, error, sizeof(error)) < 0) {
                goto fail;
            }
            (void)app_error(app, failure);
            result = 4;
            goto out;
        }
        if (commit_event(app, "response_completed",
                json_pack("{s:I,s:o,s:s,s:s,s:s,s:s,s:o,s:O,s:s}", "cycle", (json_int_t)cycle,
                    "items", snag_response_graph_json(&graph), "provider_response_id",
                    graph.provider_response_id, "response_id", response_id, "status", "completed",
                    "turn_id", turn_id, "usage", snag_response_usage_json(&graph.usage),
                    "continuation", graph.continuation ? graph.continuation : json_null(),
                    "continuation_scope", projection.continuation_scope),
                error, sizeof(error)) < 0) {
            goto fail;
        }
        /* Bound repeated hard-budget compactions while rebuilding one request,
         * not for the whole (possibly very long) active turn. A completed
         * provider response is genuine progress; its new tool/assistant group
         * may need another checkpoint later instead of a summary-less rebase. */
        hard_compaction_attempts = 0u;
        over_budget_request_hash[0] = '\0';
        {
            struct snag_usage_totals *totals = &app->program_usage;
            const struct snag_response_usage *u = &graph.usage;
            uint64_t now = snag_monotonic_ms();
            if (u->cached_known || u->input_known || u->output_known || u->reasoning_known ||
                u->total_known) {
                if (totals->responses != UINT64_MAX) ++totals->responses;
                if (u->input_known && totals->input_tokens <= UINT64_MAX - u->input_tokens)
                    totals->input_tokens += u->input_tokens;
                if (u->cached_known &&
                    (!u->input_known || u->cached_input_tokens <= u->input_tokens)) {
                    totals->cached_seen = true;
                    if (totals->cached_input_tokens <= UINT64_MAX - u->cached_input_tokens)
                        totals->cached_input_tokens += u->cached_input_tokens;
                    if (u->input_known &&
                        totals->uncached_input_tokens <=
                            UINT64_MAX - (u->input_tokens - u->cached_input_tokens))
                        totals->uncached_input_tokens += u->input_tokens - u->cached_input_tokens;
                } else if (u->input_known &&
                           totals->uncached_input_tokens <= UINT64_MAX - u->input_tokens) {
                    totals->uncached_input_tokens += u->input_tokens;
                }
                if (u->output_known && totals->output_tokens <= UINT64_MAX - u->output_tokens)
                    totals->output_tokens += u->output_tokens;
                if (u->reasoning_known &&
                    totals->reasoning_tokens <= UINT64_MAX - u->reasoning_tokens)
                    totals->reasoning_tokens += u->reasoning_tokens;
                if (u->total_known && totals->total_tokens <= UINT64_MAX - u->total_tokens)
                    totals->total_tokens += u->total_tokens;
                if (u->output_known) {
                    app->last_response_output_tokens = u->output_tokens;
                    if (app->turn_output_tokens <= UINT64_MAX - u->output_tokens)
                        app->turn_output_tokens += u->output_tokens;
                }
            }
            app->last_response_ms = (app->response_started_ms && now > app->response_started_ms)
                                        ? now - app->response_started_ms
                                        : 0u;
        }
        error[0] = '\0';
        if (snag_app_irc_flush_urgent(app, error, sizeof(error)) < 0) {
            report_message = error[0] ? error : "urgent IRC input could not be admitted";
            goto fail;
        }
        {
            char input_tokens[32];
            char output_tokens[32];
            char reasoning_tokens[32];
            char total_tokens[32];
            usage_number(input_tokens, graph.usage.input_known, graph.usage.input_tokens);
            usage_number(output_tokens, graph.usage.output_known, graph.usage.output_tokens);
            usage_number(
                reasoning_tokens, graph.usage.reasoning_known, graph.usage.reasoning_tokens);
            usage_number(total_tokens, graph.usage.total_known, graph.usage.total_tokens);
            if (app_textf(app, SNAG_UI_RUNTIME,
                    "response › %s completed · provider=%s · outcome=%s · items=%zu · "
                    "duration=%llums · tokens=%s/%s/%s/%s",
                    response_id, graph.provider_response_id, graph_outcome_name(decision.outcome),
                    graph.count, (unsigned long long)(snag_time_ms() - response_begin_ms),
                    input_tokens, output_tokens, reasoning_tokens, total_tokens) < 0) {
                report_message = "response runtime facts could not be rendered";
                goto output_fail;
            }
        }
        if (decision.outcome == SNAG_GRAPH_CONFLICT) {
            const char *message = decision.message
                                      ? decision.message
                                      : "provider response contained conflicting actions";
            if (terminalize_pending(app, turn_id, "protocol_conflict", error, sizeof(error)) < 0 ||
                fail_turn(app, retry, turn_id, "protocol_failure", "protocol", message, error,
                    sizeof(error)) < 0) {
                goto fail;
            }
            (void)app_error(app, message);
            result = 4;
            goto out;
        }
        /* Once a complete response is durable, ordinary late steering keeps
         * this turn alive and becomes the next cycle's exact context. Model-
         * requested deferral is different: it intentionally crosses the turn
         * boundary and waits for the next turn. Calls are admitted below before
         * either path can advance. */
        if (app->session.pending_steering_count && !app->session.steering_deferred &&
            decision.outcome != SNAG_GRAPH_CALLS) {
            app->steering_requested = false;
            continue;
        }
        /* Steering admitted while the provider was generating does not erase
         * calls from the accepted response. execute_calls() admits the valid
         * wave and hands running commands back before the next cycle sees it. */
        if (decision.outcome == SNAG_GRAPH_REFUSAL)
            app->turn_policy_stopped = SNAG_POLICY_STOP_REFUSAL;
        if (app->session.process_count && decision.outcome != SNAG_GRAPH_CALLS) {
            if (!app->execute && decision.outcome == SNAG_GRAPH_FINAL) {
                int wait_rc = wait_for_command_or_input(app);
                if (wait_rc < 0) goto fail;
                if (wait_rc == 2) goto user_interrupted;
                if (wait_rc == 0) continue;
            }
            const char *message = SNAG_UNSETTLED_COMMANDS_MESSAGE;
            if (fail_turn(app, retry, turn_id, "protocol_failure", "protocol", message, error,
                    sizeof(error)) < 0)
                (void)app_error(app, error);
            else
                (void)app_error(app, message);
            result = 4;
            goto out;
        }
        if (decision.outcome == SNAG_GRAPH_CALLS || decision.outcome == SNAG_GRAPH_FINAL) {
            retry->attempts = 0u;
            retry->last_failure_code[0] = '\0';
            retry->last_failure_type[0] = '\0';
            retry->last_failure_message[0] = '\0';
            app->recovery_delay_ms = 0u;
        }
        if (app->networked && !app->session.active_read_only && !app->session.steering_deferred &&
            snag_app_irc_replies_pending(app) && !app->session.irc_reply_reminded &&
            (decision.outcome == SNAG_GRAPH_NONPRODUCTIVE || decision.outcome == SNAG_GRAPH_FINAL ||
                decision.outcome == SNAG_GRAPH_REFUSAL)) {
            char steering_id[SNAG_ID_HEX_LEN + 1u];

            if (snag_random_id(steering_id) < 0 ||
                commit_event(app, "irc_reply_reminder",
                    snag_app_steering_added_data(
                        turn_id, steering_id, SNAG_IRC_REPLY_REMINDER_TEXT),
                    error, sizeof(error)) < 0) {
                report_message = error[0] ? error : "IRC reply reminder could not be persisted";
                goto fail;
            }
            continue;
        }
        if (app->request_networked && decision.outcome == SNAG_GRAPH_NONPRODUCTIVE) {
            if (commit_event(app, "turn_completed_silent",
                    json_pack("{s:s,s:s,s:s}", "reason",
                        snag_app_irc_replies_pending(app) ? "reply_reminder_exhausted"
                                                          : "room_update_quiet",
                        "response_id", response_id, "turn_id", turn_id),
                    error, sizeof(error)) < 0) {
                report_message = error[0] ? error : "quiet IRC turn could not be completed";
                goto fail;
            }
            result = 0;
            goto out;
        }
        if (decision.outcome == SNAG_GRAPH_FINAL || decision.outcome == SNAG_GRAPH_REFUSAL) {
            struct snag_response_item view = snag_response_graph_item(&graph, decision.final_index);
            const struct snag_response_item *final = &view;
            if (commit_event(app, "turn_completed",
                    snag_app_turn_completed_data(turn_id, response_id, final->local_item_id), error,
                    sizeof(error)) < 0) {
                goto fail;
            }
            if (app_textf(app, SNAG_UI_RUNTIME, "turn › %s completed · response=%s · item=%s",
                    turn_id, response_id, final->local_item_id) < 0) {
                report_message = "turn runtime facts could not be rendered";
                goto output_fail;
            }
            if (app->ui.opened && !app->execute &&
                (app->session.goal_status == SNAG_GOAL_PAUSED ||
                    app->session.goal_status == SNAG_GOAL_BLOCKED) &&
                app->session.pending_queue_count == 0u && !app->session.pending_steering_count &&
                app_textf(app, SNAG_UI_WARNING, "idle: goal %s%s%s",
                    app->session.goal_status == SNAG_GOAL_PAUSED ? "paused"
                                                                 : "blocked, waiting for ",
                    app->session.goal_status == SNAG_GOAL_BLOCKED
                        ? snag_goal_wait_for(&app->session)
                        : "",
                    app->session.timer_id[0] ? "; timer scheduled" : "") < 0) {
                report_message = "idle-state notice could not be rendered";
                goto output_fail;
            }
            if (app->execute &&
                snag_ui_send(&app->ui, (struct snag_ui_command){.kind = SNAG_UI_RAW,
                                           .data.value = (unsigned int)(STDOUT_FILENO),
                                           .text = final->text,
                                           .len = strlen(final->text)}) < 0) {
                report_message = "final answer could not be written to stdout";
                goto output_fail;
            }
            if (snag_app_compact_after_turn(
                    app, projection.input_tokens_bound, count_method, error, sizeof(error)) < 0)
                (void)app_warning(app, error);
            result = 0;
            goto out;
        }
        if (decision.outcome == SNAG_GRAPH_CALLS) {
            int tool_rc;
            tool_rc = execute_calls(app, turn_id, &graph, &credential, error, sizeof(error));
            app->tool_waiting = app->yield_requested = false;
            if (tool_rc < 0) {
                (void)app_error(app, error);
                /* An adapter/journal failure leaves effects uncertain. Stop
                 * admission and close every owned job before exiting. */
                if (app->session.goal_status != SNAG_GOAL_ACTIVE) {
                    app->input_closed = true;
                    snag_tools_shutdown();
                }
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
            const char *message =
                decision.message ? decision.message : "provider response was not actionable";
            result = finish_turn_failure(
                app, retry, turn_id, "protocol_failure", "protocol", message, error, sizeof(error));
            goto out;
        }
    steered_before_response:
        app->steering_requested = false;
    rebuild_request:
        --cycle;
    }
    {
        static const char message[] = "response-cycle counter exhausted";
        result = finish_turn_failure(
            app, retry, turn_id, "internal_failure", "resource", message, error, sizeof(error));
    }
    goto out;
user_interrupted:
    result = finish_user_interrupt(app, turn_id, error, sizeof(error));
    goto out;
output_fail:
    result = 6;
    goto report;
fail:
    result = 3;
report:
    (void)app_error(app, report_message);
out:
    snag_app_response_cycle_release(app, &graph, &steering, &projection, &request_body);
    snag_credential_clear(&credential);
    snag_instructions_free(&app->turn_instructions);
    return result;
}

json_t *
snag_app_input_received_data(
    struct app_state *app, const char *text, bool read_only, bool timer_turn)
{
    const struct snag_provider_config *provider = next_provider(app);
    const char *effort = resolve_effort(app->session.default_effort);
    if (!provider || !effort || !snag_text_valid(text, 1u, SNAG_MAX_DIRECT_PROMPT)) return NULL;
    json_t *data = json_pack("{s:s,s:o,s:s,s:s,s:b,s:I,s:s}", "effort", effort, "instructions",
        snag_instructions_metadata_json(&app->cli->doc_instructions), "model",
        app->session.default_model, "provider", provider->name, "read_only", read_only,
        "received_at_ms",
        (json_int_t)(app->ui.input_received_ms ? app->ui.input_received_ms : snag_time_ms()),
        "text", text);
    if (timer_turn && data && json_object_set_new(data, "origin", json_string("timer")) < 0) {
        json_decref(data);
        return NULL;
    }
    return data;
}

static int
run_tracked_turn(struct app_state *app, const char *prompt, const struct snag_queued_turn *queued,
    bool goal_turn, bool read_only, const json_t *content, bool timer_turn)
{
    if (app->session.active_turn) {
        read_only = app->session.active_read_only;
        goal_turn = app->session.active_goal;
        if (strcmp(prompt, app->session.active_prompt)) {
            char id[SNAG_ID_HEX_LEN + 1u], error[256] = {0};
            if (snag_random_id(id) < 0 ||
                commit_input(app, "steering_added",
                    snag_app_steering_added_data(app->session.active_turn_id, id, prompt), content,
                    error, sizeof(error)) < 0) {
                (void)app_error(app, error);
                return 3;
            }
        }
        prompt = app->session.active_prompt;
        content = NULL;
    }
    if (!app->session.active_turn && !queued && !goal_turn) {
        char error[256] = {0};
        if (!snag_text_valid(prompt, 1u, SNAG_MAX_DIRECT_PROMPT)) {
            (void)app_error(app, "prompt must be nonempty valid UTF-8 within 1 MiB");
            return 2;
        }
        if (!app->session.pending_input) {
            if (commit_input(app, "input_received",
                    snag_app_input_received_data(app, prompt, read_only, timer_turn), content,
                    error, sizeof(error)) < 0) {
                (void)app_error(app, error);
                return 3;
            }
            /* New operator input clears the consecutive compaction-failure
             * bound, so a retry can attempt the summary request again. */
            app->compaction_failures = 0u;
            app->compaction_bounded = false;
        } else if (strcmp(prompt, snag_json_string(app->session.pending_input, "text"))) {
            struct snag_buf queued_text = {.max = SNAG_MAX_DIRECT_PROMPT + 8u};
            int rc = snag_buf_printf(&queued_text, "%s%s",
                read_only          ? "/ro "
                : prompt[0] == '/' ? "/"
                                   : "",
                prompt);
            if (!rc) rc = snag_buf_terminate(&queued_text);
            if (!rc)
                rc = queue_future_turn(app, (char *)queued_text.data, true, error, sizeof(error));
            snag_buf_free(&queued_text);
            if (rc) {
                (void)app_error(app, error);
                return rc < 0 ? 3 : 2;
            }
        }
        prompt = snag_json_string(app->session.pending_input, "text");
        content = json_object_get(app->session.pending_input, "content");
        timer_turn = snag_json_string(app->session.pending_input, "origin") != NULL;
        read_only = json_is_true(json_object_get(app->session.pending_input, "read_only"));
        app->input_received_ms = (uint64_t)json_integer_value(
            json_object_get(app->session.pending_input, "received_at_ms"));
        /* Room traffic admitted as a turn prompt is not a submission: the
         * durable trail and the chat view already show it. */
        if (!app->execute && !snag_irc_prompt(prompt) &&
            snag_ui_submitted(
                &app->ui, timer_turn ? "timer › " : app->ui.label, prompt, !timer_turn) < 0)
            return 6;
        if (snag_ui_leaving(&app->ui)) return 0;
    }
    /* A queued entry can be consumed by turn_started: own the input across retries. */
    char *retained = snag_strdup_checked(prompt, SNAG_MAX_DIRECT_PROMPT);
    json_t *retained_content = json_incref((json_t *)content);
    uint64_t turns = app->session.turn_count;
    struct snag_queued_turn queued_copy;
    int rc;
    if (!retained) {
        json_decref(retained_content);
        return 3;
    }
    app->turn_policy_stopped =
        app->session.active_turn ? app->session.policy_stopped : SNAG_POLICY_STOP_NONE;
    if (app->turn_policy_stopped) {
        free(retained);
        json_decref(retained_content);
        (void)app_warning(app, "Provider policy rejection; clarify the task to continue.");
        return 4;
    }
    struct turn_retry retry = {
        .attempts = app->session.active_turn ? app->session.turn_retry_attempts : 0u,
        .limit = app->session.active_turn ? app->session.turn_retry_limit
                                          : app->config->max_turn_retries,
        .goal_status = app->session.goal_status};
    if (retry.goal_status != SNAG_GOAL_ACTIVE) app->recovery_delay_ms = 0u;
    if (queued) {
        queued_copy = *queued;
        queued_copy.text = retained;
        queued_copy.content = retained_content;
        queued = &queued_copy;
    }
    if (!app->input_closed) app->interrupt_requested = false;
    if (!app->input_received_ms) app->input_received_ms = snag_time_ms();
    app->ui.input_received_ms = 0u;
    for (;;) {
        if (queued && !app->session.active_turn) {
            const struct snag_queued_turn *current = NULL;
            for (size_t i = 0; i < app->session.pending_queue_count; ++i)
                if (!strcmp(app->session.pending_queue[i].queue_id, queued_copy.queue_id))
                    current = &app->session.pending_queue[i];
            if (!current) {
                rc = SNAG_APP_INPUT_READY;
                break;
            }
            if (strcmp(current->text, retained)) {
                char *updated = snag_strdup_checked(current->text, SNAG_MAX_QUEUED_TEXT);
                if (!updated) {
                    int wait_rc = turn_recovery_wait(app, &retry);
                    if (wait_rc) {
                        rc = wait_rc < 0 ? 3 : 1;
                        break;
                    }
                    continue;
                }
                free(retained);
                retained = updated;
            }
            json_decref(retained_content);
            retained_content = json_incref(current->content);
            queued_copy = *current;
            queued_copy.text = retained;
            queued_copy.content = retained_content;
            read_only = queued_copy.read_only;
        }
        retry.pending = false;
        rc = run_turn(
            app, &retry, retained, queued, goal_turn, timer_turn, read_only, retained_content);
        if (app->session.turn_count != turns) queued = NULL;
        if (app->turn_policy_stopped) {
            retry.attempts = 0u;
            char error[256] = {0};
            const char *reason = app->turn_policy_stopped == SNAG_POLICY_STOP_REFUSAL
                                     ? "refusal"
                                     : "provider_policy";
            if (snag_app_goal_pause(app, reason, error, sizeof(error)) < 0) {
                (void)app_error(app, error);
                rc = 3;
                break;
            }
        }
        if (app->turn_policy_stopped && app->session.process_count && !app->execute) {
            int wait_rc = turn_recovery_wait(app, &retry);
            if (wait_rc < 0) {
                rc = 3;
                break;
            }
            if (wait_rc == 0 && !app->turn_policy_stopped) continue;
        }
        bool goal = app->session.goal_status == SNAG_GOAL_ACTIVE && snag_app_retry_allowed(app);
        if (app->turn_policy_stopped || (!goal && !retry.pending) || app->input_closed ||
            app->interrupt_requested || rc == 0 ||
            (rc == SNAG_APP_INPUT_READY && !app->session.active_turn))
            break;
        if (!goal) ++retry.attempts;
        int wait_rc = turn_recovery_wait(app, &retry);
        if (wait_rc < 0) {
            rc = 3;
            break;
        }
        if (wait_rc == 3) {
            char error[256] = {0};
            rc = fail_turn(app, &retry, app->session.active_turn_id, "retry_disabled", "internal",
                     "Automatic retry disabled; use /retry to continue.", error, sizeof(error)) < 0
                     ? 3
                     : 4;
            if (rc == 3) (void)app_error(app, error);
            break;
        }
        if (wait_rc) {
            if (app->session.active_turn) {
                char error[256] = {0};
                if (interrupt_turn(app, app->session.active_turn_id, "user_interrupt", true, "user",
                        "cancelled", error, sizeof(error)) < 0)
                    (void)app_error(app, error);
            }
            if (wait_rc == 2) (void)app_warning(app, "turn interrupted");
            rc = wait_rc == 2 ? (app->execute ? 6 : 1) : 0;
            break;
        }
        if (app->session.active_turn) {
            /* Reconcile uncertain tool outcomes before a fresh model request.
             * Journal failure keeps this loop closed to new tool admissions. */
            char error[256] = {0};
            int repair = 0;
            for (;;) {
                repair = 0;
                if (app->session.pending_call_count) {
                    app->recovery_wait = true;
                    repair = terminalize_pending(app, app->session.active_turn_id,
                        "recovery_unstarted", error, sizeof(error));
                    app->recovery_wait = false;
                }
                if (!repair && app->session.response_open)
                    repair = commit_event(app, "response_interrupted",
                        snag_app_response_interrupted_data(app->session.active_turn_id,
                            app->session.active_response_id, app->session.active_cycle, "recovery",
                            "process_lost",
                            app->session.response_public ? json_copy(app->session.response_public)
                                                         : NULL),
                        error, sizeof(error));
                if (!repair && (app->session.append_rollback_pending || rc == 3 || rc == 6 ||
                                   app->session.response_complete ||
                                   app->session.response_terminal != SNAG_RESPONSE_TERMINAL_NONE))
                    repair = commit_event(app, "turn_recovery",
                        snag_app_turn_failed_data(app->session.active_turn_id, "resource",
                            "recovering failed turn state"),
                        error, sizeof(error));
                if (!repair) break;
                (void)app_error(app, error);
                if (!goal) break;
                int wait_rc = turn_recovery_wait(app, &retry);
                if (wait_rc < 0) {
                    rc = 3;
                    break;
                }
                if (wait_rc) break;
            }
            if (repair && (!goal || rc == 3 || !snag_app_retry_allowed(app))) {
                rc = 3;
                break;
            }
            if (app->interrupt_requested || app->input_closed ||
                (goal && app->session.goal_status != SNAG_GOAL_ACTIVE)) {
                rc = 1;
                break;
            }
        }
    }
    if (app->interrupt_requested && !app->input_closed &&
        app->session.goal_status == SNAG_GOAL_ACTIVE) {
        char error[256] = {0};
        if (snag_app_goal_pause(app, "user", error, sizeof(error)) < 0) (void)app_error(app, error);
    }
    if (app->session.active_turn && !app->session.delete_requested &&
        (app->input_closed || app->interrupt_requested) &&
        (app->session.response_open || app->session.response_complete ||
            app->session.process_count || app->session.pending_call_count || !app->input_closed)) {
        char error[256] = {0};
        if (interrupt_turn(app, app->session.active_turn_id, "user_interrupt", true, "user",
                "cancelled", error, sizeof(error)) < 0)
            (void)app_error(app, error);
    }
    app->recovery_delay_ms = 0u;
    app->input_received_ms = 0u;
    app->ui.input_received_ms = 0u;
    json_decref(retained_content);
    app->irc_turn_replies.count = 0u;
    json_decref(app->irc_turn_conversations);
    app->irc_turn_conversations = NULL;
    free(retained);
    /* Goal pause and retained-turn cleanup must precede the next idle prompt. */
    if (!app->execute && rc != 6 && set_input_prompt(app, false) < 0) rc = 6;
    return rc;
}

char *
snag_app_dotdir(const char *override, char *error, size_t error_size)
{
    char *home = override ? NULL : snag_home_directory();
    char *path;
    size_t len;

    if (override)
        path = snag_strdup_checked(override, SNAG_PATH_MAX_BYTES);
    else if (snag_path_root_len(home)) {
        size_t home_len = strlen(home);
        bool slash = home_len != 0u && home[home_len - 1u] == '/';
        const char *suffix = slash ? "." SNAJPAGENT_NAME : "/." SNAJPAGENT_NAME;
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
    } else {
        free(home);
        (void)snag_fail(error, error_size, EINVAL,
            "HOME is unavailable for the default dotdir; use --dotdir DIR");
        return NULL;
    }
    free(home);
    if (!path) return NULL;
    snag_path_slashes(path);
    len = strlen(path);
    size_t root_len = snag_path_root_len(path);
    while (len > (root_len ? root_len : 1u) && path[len - 1u] == '/') path[--len] = '\0';
    if (!root_len || !snag_utf8_valid((const unsigned char *)path, len, true)) {
        snag_errorf(
            error, error_size, "dotdir must be an absolute UTF-8 path within the supported limit");
        free(path);
        errno = EINVAL;
        return NULL;
    }
    return path;
}

static int
append_command_literal(struct snag_buf *command, const char *word)
{
    if (command->len && snag_buf_putc(command, ' ') < 0) return -1;
    return snag_buf_append(command, word, strlen(word));
}

static int
append_command_option(struct snag_buf *command, const char *option, const char *argument)
{
    return append_command_literal(command, option) < 0 ||
                   snag_command_argument(command, argument) < 0
               ? -1
               : 0;
}

char *
snag_app_resume_command(const char *program, const char *dotdir, const char *id, bool workspace)
{
    struct snag_buf command = {.max = RESUME_COMMAND_MAX};
    char *resolved = snag_program_path(program && *program ? program : SNAJPAGENT_NAME);
    char *default_dir = snag_app_dotdir(NULL, NULL, 0u);
    snag_file_info actual, standard;
    bool is_default =
        default_dir &&
        (!strcmp(dotdir, default_dir) ||
            (snag_stat(dotdir, &actual) == 0 && snag_stat(default_dir, &standard) == 0 &&
                actual.st_dev == standard.st_dev && actual.st_ino == standard.st_ino));
    char *result = NULL;

    free(default_dir);
    if (!resolved) return NULL;
    if (snag_command_argument(&command, resolved) < 0 ||
        (workspace && append_command_literal(&command, "vm") < 0) ||
        (!is_default && append_command_option(&command, "--dotdir", dotdir) < 0) ||
        append_command_literal(&command, "--resume") < 0 ||
        snag_command_argument(&command, id) < 0 || snag_command_finish(&command) < 0 ||
        snag_buf_terminate(&command) < 0)
        goto out;
    result = (char *)command.data;
    command.data = NULL;
out:
    free(resolved);
    snag_buf_free(&command);
    return result;
}

static void
write_resume_command(struct app_state *app, const char *program, const char *dotdir)
{
    if (!dotdir || app->session.log_fd < 0 || app->session.delete_requested) return;
    char *command = snag_app_resume_command(program, dotdir, app->session.id, false);
    if (command)
        (void)snag_ui_send(
            &app->ui, (struct snag_ui_command){
                          .kind = SNAG_UI_RESUME, .text = command, .len = strlen(command)});
    free(command);
}

static unsigned int
list_columns(const struct app_state *app)
{
    int fd = app->cli->list ? STDOUT_FILENO : STDERR_FILENO;
    if (snag_isatty(fd) != 1) return 0u;
    unsigned int columns = snag_term_host_columns();
    return columns ? columns : 80u;
}

static int
list_row(void *opaque, const char *text, size_t len)
{
    struct app_state *app = opaque;
    if (app->command_report) return snag_buf_append(app->command_report, text, len);
    return snag_ui_send(
        &app->ui, (struct snag_ui_command){.kind = SNAG_UI_RAW,
                      .data.value = (unsigned int)(app->cli->list ? STDOUT_FILENO : STDERR_FILENO),
                      .text = text,
                      .len = len});
}

static int
pick_session_id(
    struct app_state *app, uint64_t stored_limit, char **id, char *error, size_t error_size)
{
    const char *frames[SNAG_TERM_SPINNER_COUNT] = {" ", " ", " "};
    enum snag_term_action action;
    char *prefix = NULL;
    int rc = -1;

    *id = NULL;
    if (snag_store_list(&app->store, NULL, stored_limit, list_columns(app), list_row, app, error,
            error_size) < 0 ||
        snag_ui_open(&app->ui, error, error_size) < 0 ||
        snag_ui_prompt(&app->ui, false, "session › ", frames, 1u, 0u) < 0)
        return -1;
    do {
        rc = snag_ui_poll(&app->ui, -1, &action, &prefix);
    } while (rc == 0);
    if (rc < 0 || action != SNAG_TERM_SUBMIT || !prefix) {
        snag_errorf(error, error_size, "session selection cancelled");
        rc = -1;
    } else if (!prefix[0] || strlen(prefix) > SNAG_ID_HEX_LEN) {
        snag_errorf(error, error_size, "enter a unique 1..32 character session id prefix");
        rc = -1;
    } else {
        *id = prefix;
        return 0;
    }
    free(prefix);
    return rc;
}

static int
pick_session(struct app_state *app, char *error, size_t error_size)
{
    char *prefix = NULL;
    int rc = pick_session_id(app, UINT64_MAX, &prefix, error, error_size);
    if (!rc) rc = snag_session_open(&app->store, &app->session, prefix, error, error_size);
    free(prefix);
    return rc;
}
static int
run_queued_chain(struct app_state *app)
{
    while (
        !app->input_closed && app->session.queue_armed && app->session.pending_queue_count != 0u) {
        const struct snag_queued_turn *queued = &app->session.pending_queue[0];
        int turn_rc = run_tracked_turn(
            app, queued->text, queued, false, queued->read_only, queued->content, false);
        if (turn_rc != 0 && turn_rc != SNAG_APP_INPUT_READY) {
            if (!app->input_closed && snag_app_queue_arm(app, false) < 0) return 3;
            return turn_rc;
        }
    }
    return 0;
}

static bool
timer_due(const struct app_state *app)
{
    return app && app->session.timer_id[0] && app->session.timer_due_ms &&
           snag_time_ms() >= app->session.timer_due_ms;
}

static int
run_due_timer(struct app_state *app)
{
    char timer_id[SNAG_ID_HEX_LEN + 1u];
    char *text;
    char error[256] = {0};
    int rc;

    if (!timer_due(app)) return 0;
    memcpy(timer_id, app->session.timer_id, sizeof(timer_id));
    text = snag_strdup_checked(app->session.timer_text, SNAG_MAX_TIMER_TEXT);
    if (!text) return 3;
    if (commit_event(app, "timer_fired", json_pack("{s:s}", "timer_id", timer_id), error,
            sizeof(error)) < 0) {
        free(text);
        (void)app_error(app, error);
        return 3;
    }
    rc = run_tracked_turn(app, text, NULL, false, false, NULL, true);
    free(text);
    return rc;
}

static int
idle_poll_timeout(const struct app_state *app)
{
    int timeout =
        app->pager || app->audio || app->voice || app->networked || app->irc_background.len ? 25
                                                                                            : -1;
    if (snag_mcp_watching(app->mcp) && (timeout < 0 || timeout > 100)) timeout = 100;
    if (json_array_size(app->session.download_queue) && (timeout < 0 || timeout > 250))
        timeout = 250;
    if (app->session.timer_id[0] && app->session.timer_due_ms) {
        uint64_t now = snag_time_ms();
        uint64_t remaining = app->session.timer_due_ms > now ? app->session.timer_due_ms - now : 0u;
        if (remaining < (uint64_t)(timeout < 0 ? INT_MAX : timeout))
            timeout = remaining > (uint64_t)INT_MAX ? INT_MAX : (int)remaining;
    }
    return timeout;
}

static int
run_ready_chains(struct app_state *app)
{
    for (;;) {
        int turn_rc;

        if (app->input_closed || snag_ui_leaving(&app->ui)) {
            app->goal_armed = false;
            return 0;
        }
        if (apply_controls(app) < 0) return 3;
        if (app->input_closed) return 0;
        if (!app->session.active_turn && app->session.pending_input) {
            turn_rc = run_tracked_turn(app, snag_json_string(app->session.pending_input, "text"),
                NULL, false, json_is_true(json_object_get(app->session.pending_input, "read_only")),
                json_object_get(app->session.pending_input, "content"), false);
            if (turn_rc != 0 && turn_rc != SNAG_APP_INPUT_READY) return turn_rc;
            continue;
        }
        if (app->session.active_turn) {
            /* Post-cancel deferral: a user interrupt ends the turn with no
             * new turn from model/queue/reminder/goal sources. Pending user
             * input and IRC traffic still wake via their own branches below;
             * everything else waits for the next direct turn start, which
             * clears interrupt_requested on entry to run_tracked_turn. */
            if (app->interrupt_requested) return 0;
            if (app->session.policy_stopped) return 0;
            if (app->session.active_goal && app->session.goal_status != SNAG_GOAL_ACTIVE) return 0;
            turn_rc = run_tracked_turn(app, app->session.active_prompt, NULL,
                app->session.active_goal, app->session.active_read_only, NULL, false);
            if (turn_rc != 0 && turn_rc != SNAG_APP_INPUT_READY) return turn_rc;
            continue;
        }
        if (app->irc_urgent.len || (app->irc_sleep_released && app->irc_background.len) ||
            ((!app->session.queue_armed || app->session.pending_queue_count == 0u) &&
                app->goal_armed && app->irc_background.len)) {
            bool local_operator = false;
            char *prompt = snag_app_irc_take_pending(app, &local_operator, true);
            if (prompt) {
                turn_rc = run_tracked_turn(app, prompt, NULL, false, false, NULL, false);
                free(prompt);
                if (turn_rc != 0 && turn_rc != SNAG_APP_INPUT_READY) return turn_rc;
                continue;
            }
        }
        /* Post-cancel deferral (see above): queue/timer/goal wait for the
         * next direct turn start; IRC already had its chance above. */
        if (app->interrupt_requested) return 0;
        if (app->session.queue_armed && !app->queue_edit_id[0] &&
            app->session.pending_queue_count != 0u) {
            turn_rc = run_queued_chain(app);
            if (turn_rc != 0 && turn_rc != SNAG_APP_INPUT_READY) return turn_rc;
            continue;
        }
        if (timer_due(app)) {
            turn_rc = run_due_timer(app);
            if (turn_rc != 0 && turn_rc != SNAG_APP_INPUT_READY) return turn_rc;
            continue;
        }
        if (app->goal_armed && !app->session.last_turn_failed &&
            app->session.pending_queue_count == 0u &&
            app->session.goal_status == SNAG_GOAL_ACTIVE) {
            turn_rc =
                run_tracked_turn(app, SNAG_GOAL_CONTINUATION_TEXT, NULL, true, false, NULL, false);
            if (turn_rc != 0 && turn_rc != SNAG_APP_INPUT_READY) return turn_rc;
            continue;
        }
        if (!app->execute && app->ui.opened && !app->ui.prompt_wanted &&
            set_input_prompt(app, false) < 0)
            return 6;
        return 0;
    }
}

static int
submit_idle(
    struct app_state *app, const char *prompt, enum snag_render_view input_view, bool *prompt_ready)
{
    bool single_line = strchr(prompt, '\n') == NULL;
    bool read_only, handled = false, exit_now = false;
    const char *query = snag_prompt_parse(prompt, &read_only);
    bool retry = single_line && strcmp(prompt, "/retry") == 0;
    int rc = 0;

    if (app->queue_edit_id[0] && !snag_prompt_command(prompt)) {
        char error[256] = {0};
        rc = finish_queue_edit(app, prompt, false, error, sizeof(error));
        if (rc != 0 && error[0]) (void)app_error(app, error);
        *prompt_ready = true;
        return rc < 0 ? 3 : 0;
    }
    if (snag_text_blank(prompt)) return 0;
    /* A non-blank user submission ends post-cancel deferral: the user is
     * present, so subsequent ready chains (queue/timer/goal) may run. The
     * direct turn itself also clears interrupt_requested on entry. */
    app->interrupt_requested = false;
    rc = snag_app_input_command(app, prompt, app->session.active_turn, &handled, prompt_ready);
    if (rc < 0) return 3;
    if (!handled && single_line && prompt[0] == '/' && prompt[1] != '/') {
        rc = snag_app_lifecycle_command(app, prompt, &handled, &exit_now);
        if (rc < 0 || exit_now) return rc < 0 ? 3 : 1;
    }
    if (handled) {
        rc = run_ready_chains(app);
    } else if (single_line && strcmp(prompt, "/exit") == 0) {
        return 1;
    } else if (single_line && strcmp(prompt, "/next") == 0) {
        if (app->session.pending_queue_count == 0u) {
            (void)app_error(app, "future-turn queue is empty");
        } else {
            if (snag_app_queue_arm(app, true) < 0) return 3;
            rc = run_ready_chains(app);
        }
    } else if (retry && !app->session.last_turn_failed && !app->session.active_turn &&
               !app->session.pending_input) {
        (void)app_error(app, "no failed turn to retry");
    } else if (!retry && !read_only && single_line && prompt[0] == '/' && prompt[1] != '/') {
        (void)app_error(app, "unknown slash command");
    } else if (!retry && !read_only && input_view == SNAG_RENDER_CHAT) {
        if (send_operator_routed(app, prompt, query, SNAG_IRC_MESSAGE) < 0) return 3;
    } else {
        if (retry) {
            query = app->session.active_turn ? app->session.active_prompt
                    : app->session.pending_input
                        ? snag_json_string(app->session.pending_input, "text")
                        : "Retry the last failed turn. Continue from the retained "
                          "conversation and tool results; do not repeat completed actions.";
            read_only = app->session.active_turn ? app->session.active_read_only
                                                 : app->session.retry_read_only;
        }
        if (read_only && !*query) {
            (void)app_error(app, "usage: /ro QUERY (query must not be empty)");
            return 0;
        }
        if (snag_app_queue_arm(app, false) < 0) return 3;
        rc = run_tracked_turn(app, query, NULL, false, read_only, app->draft_content, false);
        if (rc == 3 || rc == 6) return rc;
        if ((rc == 0 || rc == SNAG_APP_INPUT_READY) &&
            (app->session.queue_armed || app->goal_armed || app->session.pending_controls))
            rc = run_ready_chains(app);
        else if (rc != 0 && !app->input_closed && snag_app_queue_arm(app, false) < 0)
            return 3;
    }
    return rc == 3 || rc == 6 ? rc : 0;
}

static int
interactive_loop(struct app_state *app, const char *initial)
{
    char *owned = NULL;
    int rc = 0;

    if (set_input_prompt(app, false) < 0 ||
        (initial &&
            (snag_app_sync_destinations(app) < 0 || snag_ui_capture_route(&app->ui, initial) < 0)))
        return 6;
    if (apply_controls(app) < 0) return 3;
    if (app->input_closed) return 0;
    if (!initial && (app->session.queue_armed || app->goal_armed || app->session.active_turn ||
                        app->session.pending_input || timer_due(app))) {
        rc = run_ready_chains(app);
        if (rc == 3 || rc == 6) return rc;
    }
    if (!initial && !app->session.active_turn && !app->ui.prompt_wanted &&
        set_input_prompt(app, false) < 0)
        return 6;
    for (;;) {
        enum snag_term_action action = SNAG_TERM_NONE;
        bool prompt_ready = false;
        const char *prompt = initial;

        initial = NULL;
        free(owned);
        owned = NULL;
        if (service_pager(app) < 0 || service_attachment(app, app->pager != NULL) < 0) {
            rc = 6;
            break;
        }
        if (snag_app_shutdown(app) || app->input_closed) {
            rc = 0;
            break;
        }
        if (snag_app_irc_summary_take(app, NULL, 0) < 0) {
            rc = 6;
            break;
        }
        if ((!app->pager || (app->ui.native && !snag_ui_session_attachment(&app->ui))) &&
            snag_app_audio_service(app) < 0) {
            rc = 6;
            break;
        }
        if (!prompt && !snag_ui_leaving(&app->ui) && app->session.queue_armed &&
            !app->queue_edit_id[0] && app->session.pending_queue_count) {
            rc = run_ready_chains(app);
            if (rc == 3 || rc == 6) break;
            continue;
        }
        if (!prompt) {
            bool local_operator = false;
            owned = snag_app_irc_take_pending(app, &local_operator, false);
            if (owned) {
                if (snag_app_queue_arm(app, false) < 0) {
                    rc = 3;
                    break;
                }
                rc = run_tracked_turn(app, owned, NULL, false, false, NULL, false);
                if (rc == 3 || rc == 6) break;
                if ((rc == 0 || rc == SNAG_APP_INPUT_READY) &&
                    (app->session.queue_armed || app->goal_armed || timer_due(app))) {
                    rc = run_ready_chains(app);
                    if (rc == 3 || rc == 6) break;
                }
                continue;
            }
            /* A recovered policy/goal stop can retain a turn without a running
             * provider. Keep empty-draft Ctrl-C distinct from draft clearing. */
            if (app->ui.active != app->session.active_turn &&
                set_input_prompt(app, app->session.active_turn) < 0)
                goto ui_failed;
            if (snag_monotonic_ms() - app->remote_reply_at >= 2000u) app->remote_available = false;
            if (!app->pager && json_array_size(app->session.download_queue) &&
                !app->session.active_turn && snag_monotonic_ms() - app->remote_probe_at >= 1000u &&
                snag_app_remote_probe(app) < 0)
                goto ui_failed;
            int poll_rc = snag_ui_poll(&app->ui, idle_poll_timeout(app), &action, &owned);
            if (owned) app->input_received_ms = app->ui.input_received_ms;
            history_warning(app);
            if (poll_rc < 0) {
                int input_errno = errno;
                if (snag_app_shutdown(app)) {
                    rc = 0;
                    break;
                }
                const char *message = "terminal presentation or dispatch failed";
                if (app->ui.input_error) {
                    message = input_errno == EOVERFLOW ? "prompt exceeds 1 MiB"
                              : input_errno == EILSEQ  ? "terminal input contains invalid UTF-8"
                                                       : "terminal input could not be read";
                }
                (void)app_error(app, message);
                if ((input_errno != EOVERFLOW && input_errno != EILSEQ) ||
                    set_input_prompt(app, false) < 0)
                    goto ui_failed;
                continue;
            }
            if (app->networked) {
                char error[256] = {0};
                if (tick_irc(app, error, sizeof(error)) < 0) {
                    (void)app_error(app, error[0] ? error : "IRC event loop failed");
                    rc = 3;
                    break;
                }
            }
            if (poll_rc == 0) {
                if (timer_due(app)) {
                    rc = run_ready_chains(app);
                    if (rc == 3 || rc == 6) break;
                }
                continue;
            }
            if (action == SNAG_TERM_DICTATE_DONE || action == SNAG_TERM_DICTATE_CANCEL) {
                free(owned);
                owned = NULL;
                if (snag_app_audio_action(app, action) < 0) return 6;
                continue;
            }
            if (action == SNAG_TERM_EXIT) {
                rc = 0;
                break;
            }
            if (action == SNAG_TERM_CANCEL || action == SNAG_TERM_INTERRUPT) {
                char error[256] = {0};
                if (action == SNAG_TERM_INTERRUPT && app->session.active_turn &&
                    !app->queue_edit_id[0]) {
                    char turn_id[SNAG_ID_HEX_LEN + 1u];
                    memcpy(turn_id, app->session.active_turn_id, sizeof(turn_id));
                    if (commit_event(app, "turn_cancel_requested",
                            json_pack("{s:s}", "turn_id", turn_id), error, sizeof(error)) < 0 ||
                        terminalize_pending(app, turn_id, "turn_cancelled", error, sizeof(error)) <
                            0 ||
                        interrupt_turn(app, turn_id, "user_interrupt", true, "user", "cancelled",
                            error, sizeof(error)) < 0 ||
                        snag_app_goal_pause(app, "user", error, sizeof(error)) < 0)
                        goto ui_failed;
                }
                if (!app->session.active_turn && app->session.pending_input &&
                    commit_event(app, "input_cancelled", json_object(), error, sizeof(error)) < 0)
                    goto ui_failed;
                if (cancel_queue_edit(app, false) < 0) goto ui_failed;
                continue;
            }
            if (action == SNAG_TERM_REMOTE_READY) {
                snag_app_remote_reply(app, owned);
                free(owned);
                owned = NULL;
                if (app->remote_verified) {
                    app->remote_verified = false;
                    bool newly_available = !app->remote_available;
                    app->remote_available = true;
                    if (newly_available && flush_download_queue(app) < 0) goto ui_failed;
                }
                continue;
            }
            if (action == SNAG_TERM_UPLOAD) {
                bool directory = owned && !strcmp(owned, "trz -d");
                free(owned);
                owned = NULL;
                if (snag_app_upload_command(app, directory) < 0 || set_input_prompt(app, false) < 0)
                    goto ui_failed;
                continue;
            }
            if (action == SNAG_TERM_VIEW) {
                if (input_view_toggle(app) < 0) goto ui_failed;
                continue;
            }
            if (action != SNAG_TERM_SUBMIT || !owned) {
                if (set_input_prompt(app, false) < 0) goto ui_failed;
                continue;
            }
            prompt = owned;
            remember_input(app, prompt);
        }
        rc = submit_idle(
            app, prompt, owned ? app->ui.input_view : snag_ui_view(&app->ui), &prompt_ready);
        if (rc != 0) break;
        if (!prompt_ready && set_input_prompt(app, false) < 0) goto ui_failed;
    }
    free(owned);
    return rc == 1 ? 0 : rc;
ui_failed:
    free(owned);
    return 6;
}
static int
render_room_history(void *opaque, const struct snag_irc_event *event)
{
    return snag_ui_send(opaque, (struct snag_ui_command){.kind = SNAG_UI_IRC, .data.irc = event});
}

static void
owner_report(int *fd, const char *session, const char *error)
{
    if (*fd < 0) return;
    struct snag_session_packet packet = {0};
    char message[512];
    if (error) {
        (void)snprintf(message, sizeof(message), "%s%s%s", *error ? error : "Owner startup failed",
            session && *session ? "; session " : "", session ? session : "");
    }
    const char *text = error ? message : session;
    if (snag_session_packet_set(
            &packet, error ? SNAG_SESSION_ERROR : SNAG_SESSION_READY, text, strlen(text)) == 0)
        (void)snag_session_packet_write(*fd, &packet);
    (void)close(*fd);
    *fd = -1;
}

#if SNAJPAGENT_VM
static void
direct_report(struct snag_app_direct *direct, const char *session, const char *error)
{
    if (!direct) return;
    (void)pthread_mutex_lock(&direct->lock);
    if (session && *session) (void)snag_strcpy(direct->session, sizeof(direct->session), session);
    if (error && *error)
        (void)snag_strcpy(direct->error, sizeof(direct->error), error);
    else if (session && *session)
        direct->ready = true;
    (void)pthread_mutex_unlock(&direct->lock);
}

static void
direct_ui(struct snag_app_direct *direct, struct snag_ui *ui)
{
    if (!direct) return;
    (void)pthread_mutex_lock(&direct->lock);
    direct->ui = ui;
    if (ui && direct->stop) snag_ui_request_exit(ui);
    (void)pthread_mutex_unlock(&direct->lock);
}
#endif /* SNAJPAGENT_VM */

static int
run_owner(const struct snag_cli *cli, const char *program, struct snag_session_process *process,
    int report_fd, struct snag_app_direct *direct)
{
    struct snag_cli effective = *cli, saved;
    json_t *saved_options = NULL;
    snag_cli_init(&saved);
    cli = &effective;
    struct app_state app;
    struct snag_shutdown signal_handlers;
    struct snag_config config;
    char error[256] = "";
    const char *invalid_message = error;
    char *dotdir = NULL;
    char *config_path = NULL;
    char *cwd = NULL;
    const char *new_model = NULL;
    const char *new_effort;
    struct snag_model_selection selection = {0};
    bool signal_handlers_installed = false;
    int rc = 3;
    memset(&app, 0, sizeof(app));
    app.program = program;
    app.switch_target = SNAG_RENDER_VIEW_COUNT;
    snag_buf_init(&app.irc_urgent, SNAG_MAX_IRC_SNAPSHOT);
    snag_buf_init(&app.irc_urgent_refs, SNAG_MAX_IRC_SNAPSHOT);
    snag_buf_init(&app.irc_background_refs, SNAG_MAX_IRC_SNAPSHOT);
    snag_buf_init(&app.irc_background, SNAG_MAX_IRC_SNAPSHOT);
    snag_config_init(&config);
    app.turn_instructions = (struct snag_instruction_set){0};
    app.model_cache = (struct snag_model_cache){0};
    snag_store_init(&app.store);
    snag_session_init(&app.session);
    (void)snag_http_init();
    if (snag_ui_init(&app.ui) < 0) {
        owner_report(&report_fd, NULL, "Cannot initialize owner UI");
#if SNAJPAGENT_VM
        direct_report(direct, NULL, "Cannot initialize owner UI");
#endif
        return 3;
    }
#if SNAJPAGENT_VM
    if (direct && snag_ui_session_direct(&app.ui, &direct->channel) < 0) {
        direct_report(direct, NULL, "Cannot start direct owner UI");
        snag_ui_free(&app.ui);
        return 3;
    }
    direct_ui(direct, &app.ui);
#endif /* SNAJPAGENT_VM */
    if (process && snag_ui_session_start(&app.ui, process) < 0) {
        owner_report(&report_fd, NULL, "Cannot start native owner UI");
        snag_ui_free(&app.ui);
        return 3;
    }
    atomic_store(&shutdown_ui, &app.ui);
    snag_ui_send(&app.ui, (struct snag_ui_command){.kind = SNAG_UI_COLOR,
                              .data.value = snag_cli_color(cli, SNAG_COLOR_AUTO)});
    app.cli = cli;
    app.config = &config;
    snag_tools_journal(snag_app_tool_output, snag_app_tool_read, &app);
    app.config_allow_create = cli->config_path == NULL;
    app.execute = cli->execute;
    pending_shutdown_signal = 0;
    if (!direct && snag_shutdown_install(&signal_handlers, mark_shutdown_signal, true) < 0) {
        invalid_message = "cannot install shutdown signal handlers";
        goto invalid;
    }
    signal_handlers_installed = direct == NULL;
    if (!snag_text_locale_init()) {
        invalid_message = "a UTF-8 locale is required";
        goto invalid;
    }
    error[0] = '\0';
    dotdir = snag_app_dotdir(cli->dotdir, error, sizeof(error));
    if (!dotdir) {
        invalid_message = error[0] ? error : "dotdir is unavailable";
        goto invalid;
    }
    if (cli->resume) {
        if (snag_store_open(&app.store, dotdir, error, sizeof(error)) < 0) goto fail;
        if (cli->session_name) {
            char id[SNAG_ID_HEX_LEN + 1u];
            rc = snag_store_find_name(
                &app.store, cli->session_name, id, list_row, &app, error, sizeof(error));
            if (!rc) rc = snag_session_open(&app.store, &app.session, id, error, sizeof(error));
        } else if (cli->resume_id)
            rc = snag_session_open(&app.store, &app.session, cli->resume_id, error, sizeof(error));
        else if (cli->last)
            rc = snag_session_open_last(&app.store, &app.session, error, sizeof(error));
        else
            rc = pick_session(&app, error, sizeof(error));
        if (rc == 1) {
            (void)snag_ui_text(&app.ui, SNAG_UI_WARNING, error);
            rc = 0;
            goto out;
        }
        if (rc < 0) {
            goto fail;
        }
        if (snag_app_restore_resume_options(
                &effective, &saved, &app.session, &saved_options, error, sizeof(error)) < 0)
            goto invalid;
    }
    app.config_allow_create = cli->config_path == NULL;
    if (snag_config_load(&config, cli->config_path, dotdir, error, sizeof(error)) < 0) goto invalid;
    if (direct && snag_ui_leaving(&app.ui)) {
        rc = 0;
        goto out;
    }
    if (!cli->list && ((!config.provider_count) ||
                          (cli->provider && !snag_config_provider(&config, cli->provider)))) {
        invalid_message = cli->provider ? "--provider names an unconfigured provider"
                                        : "no provider is configured; run snajpagent login";
        goto invalid;
    }
    config_path = snag_config_path(cli->config_path, dotdir, error, sizeof(error));
    if (!config_path) goto invalid;
    if (!cli->resume && snag_store_open(&app.store, dotdir, error, sizeof(error)) < 0) goto fail;
    if (snag_auth_config_open(app.store.root_fd, &config, error, sizeof(error)) < 0) {
        goto invalid;
    }
    app.mcp = snag_mcp_open(app.store.root_fd, &config);
    if (!app.mcp) goto fail;
    if (cli->update_model_cache) {
        if (refresh_model_cache(&app, error, sizeof(error)) < 0) goto fail;
    } else {
        (void)snag_model_cache_load(&app.store, &app.model_cache, app.capacity_cache_error,
            sizeof(app.capacity_cache_error));
    }
    app.config_path = config_path;
    app.irc_file_config = config.irc;
    snag_ui_send(&app.ui, (struct snag_ui_command){.kind = SNAG_UI_COLOR,
                              .data.value = snag_cli_color(cli, config.color)});
    snag_ui_send(&app.ui, (struct snag_ui_command){.kind = SNAG_UI_MARKDOWN,
                              .data.value = snag_cli_markdown(cli, config.markdown)});
    if (!cli->list && snag_irc_apply_cli(&config, cli, error, sizeof(error)) < 0) goto invalid;
    app.networked = !cli->execute && !cli->list && snag_irc_enabled(&config);
    snag_ui_send(&app.ui, (struct snag_ui_command){.kind = SNAG_UI_COMMANDS,
                              .data.commands = {snag_commands, snag_command_count()}});
    if (app.networked && snag_ui_send(&app.ui, (struct snag_ui_command){.kind = SNAG_UI_VIEW,
                                                   .data.value = SNAG_RENDER_CHAT}) < 0)
        goto out;
    snag_ui_send(&app.ui,
        (struct snag_ui_command){.kind = SNAG_UI_PAUSE,
            .data.timing = {config.typing_pause_ms, config.prompt_tool_spinner_off_delay_ms}});
    if (snag_ui_set_verbosity(&app.ui, cli->verbosity) < 0) goto out;
    if (!direct && !cli->execute && !cli->list &&
        (snag_isatty(STDIN_FILENO) != 1 || snag_isatty(STDERR_FILENO) != 1)) {
        invalid_message = "interactive mode requires terminal stdin and stderr; use -e for scripts";
        goto invalid;
    }
    new_model = effective_model(config.model);
    new_effort = cli->effort ? cli->effort : config.reasoning_effort;
    if (cli->model) {
        if (snag_model_select_selector(&app.model_cache, &config, cli->model,
                snag_config_provider(&config, cli->provider), new_effort, &selection, error,
                sizeof(error)) < 0) {
            goto invalid;
        }
        new_model = effective_model(selection.model);
        new_effort = cli->effort ? cli->effort : selection.effort;
        struct snag_model_capacity capacity;
        if (selection.context_set &&
            snag_app_context_preview(&app, selection.provider, new_model, &selection.context,
                &capacity, error, sizeof(error)) < 0)
            goto invalid;
    }
    if ((!cli->resume || cli->effort || cli->model) && !resolve_effort(new_effort)) {
        invalid_message = "reasoning effort is empty, oversized, or invalid UTF-8";
        goto invalid;
    }
#ifdef SNAJPAGENT_UPDATE_URL
    if (!cli->list && config.auto_update) (void)snag_ui_update(&app.ui, program, config.update_url);
#endif
    /* The launch directory does not silently choose the agent's file root.
     * New sessions begin at HOME; recorded roots survive a resume. */
    char *home = snag_home_directory();
    cwd = home ? snag_cwd_resolve(home, "home", error, sizeof(error)) : NULL;
    free(home);
    if (!cwd)
        (void)snag_errorf(
            error, sizeof(error), "cannot use the home directory as the default working directory");
    if (!cwd) goto fail;
    if (cli->list) {
        rc = snag_store_list(&app.store, NULL, cli->list_stored, list_columns(&app), list_row, &app,
                 error, sizeof(error)) < 0
                 ? 3
                 : 0;
        if (rc) (void)snag_ui_text(&app.ui, SNAG_UI_ERROR, error);
        goto out;
    }
    if (cli->resume) {
        const struct snag_provider_config *resume_provider;
        const char *resume_model;
        resume_provider = cli->model
                              ? selection.provider
                              : snag_config_provider(&config,
                                    cli->provider ? cli->provider : app.session.default_provider);
        resume_model = cli->model ? new_model : app.session.default_model;
        if (!resume_provider) {
            invalid_message = "selected provider is not configured; use --provider NAME";
            goto invalid;
        }
        if (!cli->execute &&
            validate_prompt_values(&app.ui, &config, resume_provider,
                cli->model ? new_model : app.session.default_model,
                resolve_effort(cli->model || cli->effort ? new_effort : app.session.default_effort),
                app.session.name) < 0) {
            invalid_message = "configured prompt cannot be rendered with the current selection";
            goto invalid;
        }
        if (recover_session(&app, error, sizeof(error)) < 0) {
            goto fail;
        }
        app.history_orientation = SNAG_HISTORY_ORIENTATION_RECOVERY;
        app.history_recovery_rebase = app.session.active_turn;
        app.goal_armed = app.session.goal_status == SNAG_GOAL_ACTIVE;
        if ((cli->provider || cli->model || cli->effort) &&
            record_model_selection(&app, resume_provider->name, resume_model,
                cli->model || cli->effort ? new_effort : app.session.default_effort, error,
                sizeof(error)) < 0) {
            goto fail;
        }
        app.turn_model = app.session.default_model;
        app.turn_effort = resolve_effort(app.session.default_effort);
        app.turn_provider = next_provider(&app);
    } else {
        const struct snag_provider_config *selected_provider =
            cli->model ? selection.provider
                       : snag_config_provider(&config, cli->provider        ? cli->provider
                                                       : config.provider[0] ? config.provider
                                                                            : NULL);
        if (!cli->execute && validate_prompt_values(&app.ui, &config, selected_provider, new_model,
                                 resolve_effort(new_effort), cli->session_name) < 0) {
            invalid_message = "configured prompt cannot be rendered with the current selection";
            goto invalid;
        }
        if (snag_session_prepare(&app.session, cwd, selected_provider->name, new_model, new_effort,
                error, sizeof(error)) < 0) {
            goto fail;
        }
        if (cli->session_name &&
            (commit_event(&app, "session_named", json_pack("{s:s}", "name", cli->session_name),
                 error, sizeof(error)) < 0 ||
                persist_session(&app, error, sizeof(error)) < 0))
            goto fail;
        snag_context_start_new(&app.session);
        app.turn_model = app.session.default_model;
        app.turn_effort = resolve_effort(app.session.default_effort);
        app.turn_provider = selected_provider;
    }
    if (selection.context_set &&
        record_context_selection(&app, &selection.context, error, sizeof(error)) < 0)
        goto fail;
    app.resume_options_ready = true;
    if (snag_app_save_resume_options(&app, error, sizeof(error)) < 0) goto fail;
    if (direct && snag_ui_leaving(&app.ui)) {
        rc = 0;
        goto out;
    }
    if (!cli->execute) {
        if (snag_irc_open(&app.irc, &config, app.session.cwd, snag_app_irc_event,
                snag_app_irc_trace, &app, error, sizeof(error)) < 0 ||
            snag_irc_bind_conversations(app.irc, app.session.irc_conversations) < 0 ||
            snag_app_irc_restore(&app, error, sizeof(error)) < 0 ||
            ((cli->resume || config.irc.listen_explicit) &&
                snag_app_irc_snapshot(&app, "join", error, sizeof(error)) < 0)) {
            (void)snag_ui_text(&app.ui, SNAG_UI_ERROR, error[0] ? error : "IRC startup failed");
            rc = 3;
            goto out;
        }
    }
    if (cli->execute) {
        rc = 0;
        if (cli->prompt) {
            bool read_only;
            const char *query = snag_prompt_parse(cli->prompt, &read_only);
            rc = run_tracked_turn(&app, query, NULL, false, read_only, NULL, false);
        }
        if ((rc == 0 || rc == SNAG_APP_INPUT_READY) &&
            (!cli->prompt || app.session.queue_armed || app.goal_armed || timer_due(&app)))
            rc = run_ready_chains(&app);
        goto out;
    }
    if (snag_ui_open(&app.ui, error, sizeof(error)) < 0) goto fail;
    if ((app.ui.native || app.ui.direct) &&
        (persist_session(&app, error, sizeof(error)) < 0 ||
            snag_ui_session_listen(&app.ui, &app.session) < 0)) {
        if (!error[0])
            (void)snag_errorf(
                error, sizeof(error), "cannot publish session attachment: %s", strerror(errno));
        goto fail;
    }
    (void)snag_ui_history_open(&app.ui, dotdir, app.session.dir_path);
    history_warning(&app);
    if (snag_ui_orientation(&app.ui, &app.session, cli->resume) < 0 ||
        (app.networked &&
            snag_irc_replay_hosted_history(app.irc, render_room_history, &app.ui) < 0) ||
        (cli->resume && snag_ui_history(&app.ui, &app.session, config.resume_history_turns) < 0) ||
        (cli->resume && app.session.goal_status != SNAG_GOAL_NONE &&
            snag_app_goal_command(&app, "/goal", false) < 0) ||
        (cli->resume && app.session.pending_queue_count != 0u && !app.session.queue_armed &&
            app_warning(&app, "queued future turns are paused; use /next to continue FIFO") < 0)) {
        rc = snag_ui_leaving(&app.ui) ? 0 : 6;
        goto out;
    }
    owner_report(&report_fd, app.session.id, NULL);
#if SNAJPAGENT_VM
    direct_report(direct, app.session.id, NULL);
#endif
    rc = interactive_loop(&app, cli->prompt);
    goto out;
invalid:
    rc = 2;
    goto report;
fail:
    rc = 3;
report:
    (void)snag_ui_text(&app.ui, SNAG_UI_ERROR, invalid_message);
out:
    owner_report(&report_fd, app.session.log_fd >= 0 ? app.session.id : NULL, invalid_message);
#if SNAJPAGENT_VM
    if (direct && rc)
        direct_report(direct, NULL,
            invalid_message[0] ? invalid_message
                               : "Direct session stopped before startup completed");
#endif
    if (app.session.log_fd >= 0 && snag_app_save_resume_options(&app, error, sizeof(error)) < 0) {
        (void)snag_ui_text(&app.ui, SNAG_UI_ERROR, error);
        if (!rc) rc = 3;
    }
    snag_app_voice_attachment_close(&app);
    if (app.pager) {
        snag_pager_close(app.pager);
        app.pager = NULL;
        free(app.pager_report);
        app.pager_report = NULL;
        (void)snag_ui_external(&app.ui, false, error, sizeof(error));
    }
    snag_app_irc_summary_close(&app);
    snag_app_audio_close(&app);
    (void)snag_app_shutdown(&app);
    /* A POSIX process lock is released by closing any descriptor on its inode. */
    (void)snag_ui_text(&app.ui, SNAG_UI_CLOSE, NULL);
    (void)snag_history_merge(&app.ui.history);
    history_warning(&app);
    snag_irc_close(app.irc);
    write_resume_command(&app, program, dotdir);
    if (signal_handlers_installed) snag_shutdown_detach(&signal_handlers);
    atomic_store(&shutdown_ui, NULL);
    snag_tools_shutdown();
    snag_tools_journal(NULL, NULL, NULL);
    if (app.ui.native || app.ui.direct) {
        /* Stop accepting controllers before releasing the original writer
         * lock. EXIT acknowledges a stopped owner to the frontend. */
        (void)snag_ui_session_listen(&app.ui, NULL);
        snag_session_close(&app.session);
        unsigned char status =
            (unsigned char)(app.shutdown_signal > 0 ? 128 + app.shutdown_signal : rc);
        (void)snag_ui_session_control(&app.ui, SNAG_SESSION_EXIT, &status, 1u);
    }
#if SNAJPAGENT_VM
    direct_ui(direct, NULL);
#endif
    snag_ui_free(&app.ui);
#if SNAJPAGENT_VM
    view_terminal_free(&app);
    for (size_t i = 0u; i < sizeof(view_control_names) / sizeof(view_control_names[0]); ++i)
        snag_buf_free(&app.view_control_reports[i]);
#endif
    (void)snag_app_shutdown(&app);
    snag_buf_free(&app.irc_urgent);
    snag_buf_free(&app.irc_urgent_refs);
    snag_buf_free(&app.irc_background_refs);
    snag_buf_free(&app.irc_background);
    json_decref(app.irc_urgent_conversations);
    json_decref(app.irc_turn_conversations);
    json_decref(app.irc_request_conversations);
    snag_buf_free(&app.output_cache.data);
    snag_app_clear_partial_public(&app);
    free(app.partial);
    snag_cli_free(&saved);
    json_decref(saved_options);
    free(config_path);
    free(dotdir);
    free(cwd);
    snag_instructions_free(&app.turn_instructions);
    snag_model_cache_free(&app.model_cache);
    json_decref(app.draft_content);
    snag_mcp_close(app.mcp);
    snag_session_close(&app.session);
    snag_store_close(&app.store);
    snag_auth_config_close(&config);
    snag_config_free(&config);
    if (signal_handlers_installed) snag_shutdown_finish(&signal_handlers);
    if (app.shutdown_signal > 0 && app.shutdown_signal < 128) rc = 128 + app.shutdown_signal;
    return rc;
}

#if SNAJPAGENT_VM
static void *
direct_main(void *opaque)
{
    struct snag_app_direct *direct = opaque;
    struct snag_cli cli;
    snag_cli_init(&cli);
    cli.dotdir = direct->dotdir;
    cli.resume = direct->resume != NULL;
    cli.resume_id = direct->resume;
    cli.session_name = direct->name;
    int status = run_owner(&cli, direct->program, NULL, -1, direct);
    snag_cli_free(&cli);
    snag_view_channel_close(&direct->channel);
    (void)pthread_mutex_lock(&direct->lock);
    direct->status = status;
    direct->finished = true;
    (void)pthread_mutex_unlock(&direct->lock);
    return NULL;
}

static void
direct_free(struct snag_app_direct *direct)
{
    free(direct->program);
    free(direct->dotdir);
    free(direct->resume);
    free(direct->name);
    (void)pthread_mutex_destroy(&direct->lock);
    free(direct);
}

struct snag_app_direct *
snag_app_direct_start(const char *program, const char *dotdir, const char *session,
    const char *name, struct snag_view_channel *channel)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&direct_busy, &expected, true)) {
        errno = EBUSY;
        return NULL;
    }
    struct snag_app_direct *direct = calloc(1u, sizeof(*direct));
    if (!direct) goto failed;
    int error = pthread_mutex_init(&direct->lock, NULL);
    if (error) {
        free(direct);
        errno = error;
        goto failed;
    }
    direct->program = strdup(program);
    direct->dotdir = strdup(dotdir);
    direct->resume = session ? strdup(session) : NULL;
    direct->name = name ? strdup(name) : NULL;
    if (!direct->program || !direct->dotdir || (session && !direct->resume) ||
        (name && !direct->name))
        goto strings;
    struct snag_view_channel pair[2];
    if (snag_view_channel_pair(pair) < 0) goto strings;
    direct->channel = pair[1];
    pthread_attr_t attributes;
    error = pthread_attr_init(&attributes);
    if (!error) {
        size_t stack_size = 0u;
        error = pthread_attr_getstacksize(&attributes, &stack_size);
        /* The engine runs the same nested store/provider paths as the main
         * thread. Small libc defaults cannot hold their working frames. */
        if (!error && stack_size < 8u * 1024u * 1024u)
            error = pthread_attr_setstacksize(&attributes, 8u * 1024u * 1024u);
        if (!error) error = pthread_create(&direct->thread, &attributes, direct_main, direct);
        (void)pthread_attr_destroy(&attributes);
    }
    if (error) {
        snag_view_channel_close(&pair[0]);
        snag_view_channel_close(&direct->channel);
        errno = error;
        goto strings;
    }
    *channel = pair[0];
    return direct;
strings:
    direct_free(direct);
failed:
    atomic_store(&direct_busy, false);
    return NULL;
}

enum snag_app_direct_state
snag_app_direct_state(struct snag_app_direct *direct, char session[SNAG_ID_HEX_LEN + 1u],
    char *error, size_t error_size, int *status)
{
    (void)pthread_mutex_lock(&direct->lock);
    memcpy(session, direct->session, sizeof(direct->session));
    if (error && error_size) (void)snprintf(error, error_size, "%s", direct->error);
    *status = direct->status;
    enum snag_app_direct_state state = direct->finished ? SNAG_APP_DIRECT_FINISHED
                                       : direct->ready  ? SNAG_APP_DIRECT_READY
                                                        : SNAG_APP_DIRECT_STARTING;
    (void)pthread_mutex_unlock(&direct->lock);
    return state;
}

void
snag_app_direct_stop(struct snag_app_direct *direct)
{
    if (!direct) return;
    (void)pthread_mutex_lock(&direct->lock);
    direct->stop = true;
    if (direct->ui) snag_ui_request_exit(direct->ui);
    (void)pthread_mutex_unlock(&direct->lock);
}

void
snag_app_direct_free(struct snag_app_direct *direct)
{
    if (!direct) return;
    snag_app_direct_stop(direct);
    (void)pthread_join(direct->thread, NULL);
    direct_free(direct);
    atomic_store(&direct_busy, false);
}
#endif /* SNAJPAGENT_VM */

static int
attachment_candidate(void *opaque, const char *text, size_t length)
{
    (void)opaque;
    return snag_write_full(STDERR_FILENO, text, length);
}

static int
connect_session(void *opaque, const char *prefix, char *selected, char *error, size_t error_size)
{
    const struct snag_cli *cli = opaque;
    struct snag_store store;
    struct snag_session target;
    char *dotdir = snag_app_dotdir(cli->dotdir, error, error_size);
    int peer = -1;
    snag_store_init(&store);
    snag_session_init(&target);
    if (dotdir && snag_store_open(&store, dotdir, error, error_size) == 0 &&
        snag_session_locate(
            &store, &target, prefix, attachment_candidate, NULL, error, error_size) == 0) {
        peer = snag_session_endpoint_connect(target.dir_fd, target.dir_path);
        if (peer >= 0 && selected) memcpy(selected, target.id, sizeof(target.id));
        if (peer < 0)
            (void)snag_errorf(error, error_size,
                "session %s has no reachable native owner (%s); use --resume after it stops",
                target.id, strerror(errno));
    }
    snag_session_close(&target);
    snag_store_close(&store);
    free(dotdir);
    return peer;
}

static int
select_startup_session(
    const struct snag_cli *cli, char **selected, bool *live, char *error, size_t error_size)
{
    struct app_state app = {.cli = cli};
    struct snag_session target;
    char id[SNAG_ID_HEX_LEN + 1u];
    char *picked = NULL;
    const char *prefix = cli->resume ? cli->resume_id : cli->attach_id;
    char *dotdir = snag_app_dotdir(cli->dotdir, error, error_size);
    int rc = -1;

    *selected = NULL;
    snag_store_init(&app.store);
    snag_session_init(&target);
    if (!dotdir || snag_store_open(&app.store, dotdir, error, error_size) < 0) goto out;
    if (!prefix) {
        if (cli->session_name) {
            if (snag_store_find_name(&app.store, cli->session_name, id, attachment_candidate, NULL,
                    error, error_size) < 0)
                goto out;
            prefix = id;
        } else if (cli->last) {
            if (snag_store_find_last(&app.store, id, error, error_size) < 0) goto out;
            prefix = id;
        } else {
            if (snag_ui_init(&app.ui) < 0) goto out;
            rc = pick_session_id(&app, cli->resume ? UINT64_MAX : 0u, &picked, error, error_size);
            snag_ui_free(&app.ui);
            if (rc < 0) goto out;
            prefix = picked;
        }
    }
    rc = snag_session_locate(
        &app.store, &target, prefix, attachment_candidate, NULL, error, error_size);
    if (rc < 0) goto out;
    *selected = snag_strdup_checked(target.id, SNAG_ID_HEX_LEN);
    if (!*selected) {
        rc = -1;
        goto out;
    }
    if (live) *live = snag_session_is_live(&target);
out:
    snag_session_close(&target);
    snag_store_close(&app.store);
    free(picked);
    free(dotdir);
    return rc;
}

static int
attach_session(const struct snag_cli *cli, char *error, size_t error_size)
{
    char *selected = NULL;
    int peer;
    if (!snag_session_host_supported())
        return snag_errorf(error, error_size, "native attachment is unavailable on this host");
    if (!snag_text_locale_init())
        return snag_errorf(error, error_size, "a UTF-8 locale is required");
    if (!snag_isatty(STDIN_FILENO) || !snag_isatty(STDOUT_FILENO) || !snag_isatty(STDERR_FILENO))
        return snag_errorf(
            error, error_size, "attachment requires terminal stdin, stdout and stderr");
    if (select_startup_session(cli, &selected, NULL, error, error_size) < 0) return -1;
    peer = connect_session((void *)cli, selected, NULL, error, error_size);
    free(selected);
    if (peer < 0) return -1;
    return snag_session_client_terminal(
        peer, false, 0u, connect_session, (void *)cli, NULL, error, error_size);
}

static int
run_session(const struct snag_cli *cli, const char *program)
{
    struct snag_session_process process = {.master = -1, .slave = -1, .peer = -1};
    char error[256] = {0};
    int rc;
    if (cli->attach) {
        rc = attach_session(cli, error, sizeof(error));
    } else if (cli->execute || cli->list || !snag_session_host_supported() ||
               !snag_term_host_capable()) {
        return run_owner(cli, program, NULL, -1, NULL);
    } else {
        rc = snag_session_process_start(&process);
        if (rc < 0) {
            if (errno == ENOTTY) return run_owner(cli, program, NULL, -1, NULL);
            (void)snag_errorf(
                error, sizeof(error), "cannot start native session: %s", strerror(errno));
        } else if (rc == 0) {
            rc = run_owner(cli, program, &process, -1, NULL);
            snag_session_process_close(&process);
            return rc;
        } else {
            rc = snag_session_client_terminal(process.peer, true, process.child, connect_session,
                (void *)cli, NULL, error, sizeof(error));
            process.peer = -1;
            snag_session_process_close(&process);
        }
    }
    if (rc < 0) {
        (void)fprintf(stderr, "%s: %s\n", program, error[0] ? error : strerror(errno));
        return 3;
    }
    return rc;
}

int
snag_app_run(const struct snag_cli *cli, const char *program)
{
    if (!cli->resume || cli->execute || !snag_session_host_supported() ||
        !snag_isatty(STDIN_FILENO) || !snag_isatty(STDOUT_FILENO) || !snag_isatty(STDERR_FILENO) ||
        !snag_text_locale_init()) {
        return run_session(cli, program);
    }
    char error[256] = {0};
    char *id = NULL;
    bool live = false;
    if (select_startup_session(cli, &id, &live, error, sizeof(error)) < 0) {
        (void)fprintf(stderr, "%s: %s\n", program, error[0] ? error : strerror(errno));
        return 3;
    }
    struct snag_cli selected = *cli;
    selected.session_name = NULL;
    selected.last = false;
    selected.resume_id = id;
    if (live) {
        selected.resume = false;
        selected.attach = true;
        selected.attach_id = id;
        (void)fprintf(stderr, "%s: session is running; attaching with its existing settings%s\n",
            program, cli->prompt ? "; follow-up was not submitted; enter it after attaching" : "");
    }
    int rc = run_session(&selected, program);
    free(id);
    return rc;
}

int
snag_app_owner_main(int argc, char **argv)
{
    int report_fd = 3;
    if (argc != 5 || snag_fd_cloexec(4) < 0 || snag_fd_cloexec(report_fd) < 0 ||
        snag_session_peer_verify(report_fd) < 0) {
        char error[256];
        (void)snprintf(
            error, sizeof(error), "Invalid owner bootstrap channel: %s", strerror(errno));
        owner_report(&report_fd, NULL, error);
        return 2;
    }
    struct snag_cli cli;
    snag_cli_init(&cli);
    cli.dotdir = argv[2];
    if (!strcmp(argv[3], "--resume") && snag_hex_is_lower(argv[4], SNAG_ID_HEX_LEN)) {
        cli.resume = true;
        cli.resume_id = argv[4];
    } else if (!strcmp(argv[3], "--new") && (!*argv[4] || snag_session_name_valid(argv[4]))) {
        cli.session_name = *argv[4] ? argv[4] : NULL;
    } else {
        owner_report(&report_fd, NULL, "Invalid owner launch arguments");
        return 2;
    }
    /* This exec starts before any UI/HTTP/reader threads. The bootstrap closes
     * its initial controller immediately; the surviving owner keeps its PTY. */
    struct snag_session_process process;
    int rc = snag_session_process_start(&process);
    (void)close(4);
    if (rc < 0) {
        char error[256];
        (void)snprintf(error, sizeof(error), "Cannot start native owner: %s", strerror(errno));
        owner_report(&report_fd, cli.resume_id, error);
        return 3;
    }
    if (!rc)
        rc = run_owner(&cli, argv[0], &process, report_fd, NULL);
    else {
        (void)close(report_fd);
        rc = 0;
    }
    snag_session_process_close(&process);
    snag_cli_free(&cli);
    return rc;
}
