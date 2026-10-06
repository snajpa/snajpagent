/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_binary_legacy.h"
#include "config.h"
#include "irc.h"
#include "json.h"
#include "media.h"
#include "store.h"
#include "turn.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static const char *const actors[] = {NULL, "user", "model"};
static const char *const pauses[] = {
    NULL, "input_closed", "provider_policy", "refusal", "session_resumed", "turn_stopped", "user"
};
static const char *const steering_modes[] = {"", "mentions", "all"};
static const char *const context_modes[] = {"default", "max", "tokens"};
static const char *const rebase_reasons[] = {NULL, "goal_recovery", "turn_recovery"};
static const char *const compact_reasons[] = {
    NULL, "manual", "proactive", "hard_budget", "provider_rejection", "model_switch",
    "image_boundary", "reduce"
};
static const char *const compact_stop_reasons[] = {
    NULL, "steering", "user", "endpoint_unavailable", "context_rejected", "error"
};
static const char *const count_methods[] = {
    NULL, "exact", "media_upper_bound", "unknown", "anchored_upper_bound",
    "statistical_upper_estimate", "qualified_upper_bound"
};
static const char *const capacity_sources[] = {
    NULL, "unknown", "advertised", "configured", "observed", "stale-catalog-ignored"
};
static const char *const public_kinds[] = {NULL, "assistant", "refusal"};
static const char *const public_phases[] = {NULL, "commentary", "final_answer"};
static const char *const byte_encodings[] = {NULL, "utf8", "base64"};
static const char *const tool_statuses[] = {
    NULL, "not_run", "outcome_unknown", "denied", "cancelled", "running", "succeeded",
    "failed", "signaled", "timed_out", "patch_rejected", "io_failed"
};
static const char *const tool_reasons[] = {
    NULL, "protocol_conflict", "read_only", "process_limit", "batch_yield", "operator_yield",
    "process_busy", "stdin_busy", "stdin_closed", "invalid_arguments", "managed_process_conflict",
    "managed_process_handle_mismatch", "recovery_unstarted", "superseded_by_steering",
    "turn_cancelled", "process_interaction_required", "rule_rejected", "owner_lost",
    "unreaped_after_sigkill", "user_denied", "timeout_handoff", "wait_timeout", "steering_handoff",
    "output_drain_timeout"
};
static const char *const process_causes[] = {
    NULL, "user_interrupt", "provider_failure", "protocol_failure", "tool_failure",
    "output_failure", "internal_failure"
};
static const char *const irc_kinds[] = {
    NULL, "connected", "disconnected", "join", "part", "quit", "nick", "message", "notice",
    "topic", "mode", "history_ready"
};
static const char *const irc_snapshot_reasons[] = {NULL, "join", "nick", "topology", "compaction"};
static const char *const irc_wake_reasons[] = {NULL, "timeout", "mention", "messages"};
static const char *const quiet_reasons[] = {
    NULL, "room_update_quiet", "reply_reminder_exhausted"
};
static const char *const interrupt_origins[] = {NULL, "user", "recovery", "output", "steering"};
static const char *const interrupt_reasons[] = {
    NULL, "cancelled", "process_lost", "output_lost", "session_recovered", "steered", "control"
};
static const char *const failure_classes[] = {
    NULL, "context", "provider", "protocol", "tool", "persistence", "resource", "output", "internal"
};

static int
invalid(void)
{
    errno = EINVAL;
    return -1;
}

static int
unsupported(void)
{
    errno = ENOTSUP;
    return -1;
}

static bool
goal_kind(enum snag_binary_kind kind)
{
    return kind >= SNAG_BINARY_GOAL_STARTED && kind <= SNAG_BINARY_GOAL_CANCELLED;
}

static bool
rule_voice_kind(enum snag_binary_kind kind)
{
    return kind == SNAG_BINARY_RULE_LOG || kind == SNAG_BINARY_RULE_TRANSFORM ||
        kind == SNAG_BINARY_AUDIO_USAGE || kind == SNAG_BINARY_VOICE_EVENT;
}

static bool
metadata_kind(enum snag_binary_kind kind)
{
    return kind >= SNAG_BINARY_SESSION_CREATED && kind <= SNAG_BINARY_SERVICE_TIER_CHANGED;
}

static bool
timer_kind(enum snag_binary_kind kind)
{
    return kind >= SNAG_BINARY_TIMER_SCHEDULED && kind <= SNAG_BINARY_TIMER_CANCELLED;
}

static bool
control_kind(enum snag_binary_kind kind)
{
    return kind >= SNAG_BINARY_CONTROL_REQUESTED && kind <= SNAG_BINARY_CONTROL_FINISHED;
}

static bool
download_kind(enum snag_binary_kind kind)
{
    return kind >= SNAG_BINARY_DOWNLOAD_QUEUED && kind <= SNAG_BINARY_DOWNLOADS_CLEARED;
}

static bool
context_kind(enum snag_binary_kind kind)
{
    return kind == SNAG_BINARY_CONTEXT_REBASED || kind == SNAG_BINARY_RESPONSE_CAPACITY_REJECTED;
}

static bool
response_end_kind(enum snag_binary_kind kind)
{
    return kind >= SNAG_BINARY_RESPONSE_INTERRUPTED &&
        kind <= SNAG_BINARY_RESPONSE_OUTPUT_CORRECTION;
}

static bool
input_receipt_kind(enum snag_binary_kind kind)
{
    return kind == SNAG_BINARY_INPUT_RECEIVED || kind == SNAG_BINARY_STEERING_ADDED ||
        kind == SNAG_BINARY_IRC_REPLY_REMINDER;
}

static bool
queued_input_kind(enum snag_binary_kind kind)
{
    return kind == SNAG_BINARY_FUTURE_TURN_QUEUED || kind == SNAG_BINARY_FUTURE_TURN_EDITED;
}

static const char *const turn_origins[] = {"direct", "queued", "goal", "timer"};
static const char *const turn_numbers[] = {
    "max_parallel_commands", "default_yield_ms", "max_wait_ms", "default_timeout_ms",
    "max_timeout_ms", "tool_output_bytes", "output_cache_bytes", "max_turn_retries"
};
static const char *const turn_values[] = {
    "prompt_schema", "replay_schema", "tool_schema", "capability_version", "profile_id",
    "max_output_tokens"
};
_Static_assert(sizeof(turn_numbers) / sizeof(turn_numbers[0]) == SNAG_BINARY_TURN_NUMBER_COUNT,
    "Turn execution field positions must match the native schema");
_Static_assert(sizeof(turn_values) / sizeof(turn_values[0]) == SNAG_BINARY_TURN_VALUE_COUNT,
    "Turn value field positions must match the native schema");

static bool
hosted_search_kind(enum snag_binary_kind kind)
{
    return kind == SNAG_BINARY_HOSTED_SEARCH_STARTED || kind == SNAG_BINARY_HOSTED_SEARCH_FINISHED;
}

static bool
legacy_kind(enum snag_binary_kind kind)
{
    return hosted_search_kind(kind) || metadata_kind(kind) || timer_kind(kind) ||
        goal_kind(kind) || rule_voice_kind(kind) ||
        control_kind(kind) || download_kind(kind) || context_kind(kind) ||
        kind == SNAG_BINARY_RESPONSE_STARTED || kind == SNAG_BINARY_RESPONSE_OUTPUT ||
        response_end_kind(kind) || kind == SNAG_BINARY_RESPONSE_COMPLETED ||
        kind == SNAG_BINARY_TOOL_STARTED || kind == SNAG_BINARY_PROCESS_OUTPUT ||
        kind == SNAG_BINARY_TOOL_FINISHED || kind == SNAG_BINARY_PROCESS_CLOSED ||
        (kind >= SNAG_BINARY_IRC_EVENT && kind <= SNAG_BINARY_IRC_EVENT_V2) ||
        (kind >= SNAG_BINARY_VOICE_TRANSFER_RECORD && kind <= SNAG_BINARY_VOICE_TRANSFER_ADOPTED) ||
        (kind >= SNAG_BINARY_COMPACTION_STARTED && kind <= SNAG_BINARY_COMPACTION_COMPLETED) ||
        (kind >= SNAG_BINARY_TURN_STARTED && kind <= SNAG_BINARY_TURN_FAILED) ||
        (kind >= SNAG_BINARY_INPUT_RECEIVED && kind <= SNAG_BINARY_FUTURE_TURN_EDITED);
}

/* Check canonical types, UTF-8, depth and size at the legacy data nesting level.
 * The actual envelope adds its own bytes; only the importer can check that total. */
static int
canonical_data(const json_t *data)
{
    if (!json_is_object(data)) return invalid();
    json_t *wrapper = json_object();
    if (!wrapper) {
        errno = ENOMEM;
        return -1;
    }
    struct snag_buf canonical = {.max = SNAG_MAX_EVENT_LINE};
    int rc = json_object_set(wrapper, "data", (json_t *)data);
    if (rc == 0) rc = snag_json_canonical(wrapper, &canonical);
    json_decref(wrapper);
    snag_buf_free(&canonical);
    return rc;
}

static int
read_text(const json_t *data, const char *key, struct snag_binary_text *out)
{
    const json_t *value = json_object_get(data, key);
    if (!json_is_string(value)) return invalid();
    out->data = (const unsigned char *)json_string_value(value);
    out->size = json_string_length(value);
    return 0;
}

static int
read_hex(const char *text, size_t length, unsigned char *out, size_t size)
{
    if (!text || length != size * 2u) return invalid();
    for (size_t i = 0u; i < size; ++i) {
        unsigned int high = text[2u * i];
        unsigned int low = text[2u * i + 1u];
        if (high >= '0' && high <= '9') high -= '0';
        else if (high >= 'a' && high <= 'f') high = high - 'a' + 10u;
        else return invalid();
        if (low >= '0' && low <= '9') low -= '0';
        else if (low >= 'a' && low <= 'f') low = low - 'a' + 10u;
        else return invalid();
        out[i] = (unsigned char)(high * 16u + low);
    }
    return 0;
}

static int
read_id(const json_t *data, const char *key, unsigned char out[16])
{
    const json_t *value = json_object_get(data, key);
    return read_hex(json_string_value(value), json_string_length(value), out, 16u);
}

static int
read_digest(const json_t *data, const char *key, unsigned char out[32])
{
    const json_t *value = json_object_get(data, key);
    return read_hex(json_string_value(value), json_string_length(value), out, 32u);
}

static int
read_name(const json_t *data, const char *key, const char *const *names, size_t count)
{
    const json_t *value = json_object_get(data, key);
    const char *text = json_string_value(value);
    if (text) {
        for (size_t i = 0u; i < count; ++i) {
            if (names[i] && strlen(names[i]) == json_string_length(value) &&
                !strcmp(names[i], text)) {
                return (int)i;
            }
        }
    }
    return invalid();
}

static int
read_created(const json_t *data, struct snag_binary_event *event)
{
    uint64_t format;
    if (snag_json_integer_u64(data, "format", &format) < 0 || format < 2u || format > 4u) {
        return invalid();
    }
    const char *cwd = format == 2u ? "workspace" : "cwd";
    const char *keys = format == 2u ?
        "default_effort default_model default_provider format protocol workspace" :
        "default_effort default_model default_provider format protocol cwd";
    const char *protocol = snag_json_string(data, "protocol");
    if (!snag_json_exact_keys(data, keys) || !protocol || strcmp(protocol, "responses")) {
        return invalid();
    }
    event->data.created.source_format = (uint16_t)format;
    event->data.created.protocol = SNAG_BINARY_RESPONSES;
    struct snag_binary_selection *selection = &event->data.created.selection;
    if (read_text(data, "default_provider", &selection->provider) < 0 ||
        read_text(data, "default_model", &selection->model) < 0 ||
        read_text(data, "default_effort", &selection->effort) < 0 ||
        read_text(data, cwd, &event->data.created.cwd) < 0) {
        return -1;
    }
    return 0;
}

static int
read_metadata(const json_t *data, struct snag_binary_event *event, struct snag_buf *scratch)
{
    switch (event->kind) {
    case SNAG_BINARY_SESSION_CREATED:
        return read_created(data, event);
    case SNAG_BINARY_CWD_CHANGED:
        if (!snag_json_exact_keys(data, "new_cwd old_cwd")) return invalid();
        if (read_text(data, "old_cwd", &event->data.cwd.before) < 0 ||
            read_text(data, "new_cwd", &event->data.cwd.after) < 0) {
            return -1;
        }
        return 0;
    case SNAG_BINARY_SESSION_ARCHIVED:
    case SNAG_BINARY_SESSION_UNARCHIVED:
        if (!snag_json_exact_keys(data, "origin") ||
            read_name(data, "origin", actors, sizeof(actors) / sizeof(actors[0])) !=
                SNAG_BINARY_USER) {
            return invalid();
        }
        event->data.archive.origin = SNAG_BINARY_USER;
        return 0;
    case SNAG_BINARY_SESSION_DELETE_REQUESTED: {
        if (!snag_json_exact_keys(data, "confirmed_id_prefix trash_name")) {
            return invalid();
        }
        const json_t *value = json_object_get(data, "trash_name");
        const char *trash = json_string_value(value);
        if (!trash || json_string_length(value) != 65u || trash[32] != '.') {
            return invalid();
        }
        if (read_hex(trash, 32u, event->data.deletion.session, 16u) < 0 ||
            read_hex(trash + 33u, 32u, event->data.deletion.nonce, 16u) < 0) {
            return -1;
        }
        value = json_object_get(data, "confirmed_id_prefix");
        return read_hex(json_string_value(value), json_string_length(value),
            event->data.deletion.confirmed_prefix, 4u);
    }
    case SNAG_BINARY_BANNER_UPDATED:
        if (!snag_json_exact_keys(data, "text")) return invalid();
        return read_text(data, "text", &event->data.banner);
    case SNAG_BINARY_STEERING_UPDATED: {
        if (!snag_json_exact_keys(data, "mode")) return invalid();
        int mode = read_name(data, "mode", steering_modes,
            sizeof(steering_modes) / sizeof(steering_modes[0]));
        if (mode < 0) return -1;
        event->data.steering = (enum snag_binary_steering)mode;
        return 0;
    }
    case SNAG_BINARY_MODEL_SELECTED: {
        if (!snag_json_exact_keys(data,
            "new_effort new_model new_provider old_effort old_model old_provider")) {
            return invalid();
        }
        struct snag_binary_selection *before = &event->data.model.before;
        struct snag_binary_selection *after = &event->data.model.after;
        if (read_text(data, "old_provider", &before->provider) < 0 ||
            read_text(data, "old_model", &before->model) < 0 ||
            read_text(data, "old_effort", &before->effort) < 0 ||
            read_text(data, "new_provider", &after->provider) < 0 ||
            read_text(data, "new_model", &after->model) < 0 ||
            read_text(data, "new_effort", &after->effort) < 0) {
            return -1;
        }
        return 0;
    }
    case SNAG_BINARY_TURN_MODEL_CHANGED: {
        if (!snag_json_exact_keys(data, "new_effort old_effort old_model old_provider turn_id")) {
            return invalid();
        }
        struct snag_binary_selection *before = &event->data.turn_model.before;
        if (read_id(data, "turn_id", event->data.turn_model.id) < 0 ||
            read_text(data, "old_provider", &before->provider) < 0 ||
            read_text(data, "old_model", &before->model) < 0 ||
            read_text(data, "old_effort", &before->effort) < 0 ||
            read_text(data, "new_effort", &event->data.turn_model.effort) < 0) {
            return -1;
        }
        return 0;
    }
    case SNAG_BINARY_EFFORT_CHANGED:
        if (!snag_json_exact_keys(data, "new_effort old_effort")) {
            return invalid();
        }
        if (read_text(data, "old_effort", &event->data.effort.before) < 0 ||
            read_text(data, "new_effort", &event->data.effort.after) < 0) {
            return -1;
        }
        return 0;
    case SNAG_BINARY_CONTEXT_SELECTION_CHANGED: {
        if (!snag_json_exact_keys(data, "new_mode new_tokens old_mode old_tokens")) {
            return invalid();
        }
        int before = read_name(data, "old_mode", context_modes,
            sizeof(context_modes) / sizeof(context_modes[0]));
        int after = read_name(data, "new_mode", context_modes,
            sizeof(context_modes) / sizeof(context_modes[0]));
        if (before < 0 || after < 0) return -1;
        event->data.context.before.mode = (enum snag_binary_context)before;
        event->data.context.after.mode = (enum snag_binary_context)after;
        if (snag_json_integer_u64(data, "old_tokens", &event->data.context.before.tokens) < 0 ||
            snag_json_integer_u64(data, "new_tokens", &event->data.context.after.tokens) < 0) {
            return -1;
        }
        return 0;
    }
    case SNAG_BINARY_COMMAND_SHELL_CHANGED:
        if (!snag_json_exact_keys(data, "shell")) return invalid();
        return read_text(data, "shell", &event->data.shell);
    case SNAG_BINARY_SESSION_NAMED:
        if (!snag_json_exact_keys(data, "name") ||
            !snag_session_name_valid(json_string_value(json_object_get(data, "name")))) {
            return invalid();
        }
        return read_text(data, "name", &event->data.name);
    case SNAG_BINARY_SERVICE_TIER_CHANGED:
        if (!snag_json_exact_keys(data, "value")) return invalid();
        return read_text(data, "value", &event->data.service_tier);
    case SNAG_BINARY_SESSION_OPTIONS:
        if (!snag_json_exact_keys(data, "args")) return invalid();
        if (snag_binary_options_encode(scratch, json_object_get(data, "args")) < 0) return -1;
        event->data.options = (struct snag_binary_options){scratch->data, scratch->len};
        return 0;
    default: return unsupported();
    }
}

static int
read_timer(const json_t *data, struct snag_binary_event *event)
{
    bool scheduled = event->kind == SNAG_BINARY_TIMER_SCHEDULED;
    if (!snag_json_exact_keys(data, scheduled ? "due_ms text timer_id" : "timer_id")) {
        return invalid();
    }
    if (read_id(data, "timer_id", event->data.timer.id) < 0) return -1;
    if (!scheduled) return 0;
    if (snag_json_integer_u64(data, "due_ms", &event->data.timer.due_ms) < 0 ||
        read_text(data, "text", &event->data.timer.text) < 0) {
        return -1;
    }
    return 0;
}

static int
read_goal(const json_t *data, struct snag_binary_event *event)
{
    const char *keys;
    switch (event->kind) {
    case SNAG_BINARY_GOAL_STARTED: keys = "goal_id prompt"; break;
    case SNAG_BINARY_GOAL_REPLACED: keys = "actor goal_id new_goal_id prompt"; break;
    case SNAG_BINARY_GOAL_REWORDED: keys = "actor goal_id prompt"; break;
    case SNAG_BINARY_GOAL_LOCK_CHANGED: keys = "goal_id locked"; break;
    case SNAG_BINARY_GOAL_PAUSED: keys = "goal_id reason"; break;
    case SNAG_BINARY_GOAL_BLOCKED: keys = "actor goal_id reason"; break;
    case SNAG_BINARY_GOAL_COMPLETED: keys = "actor goal_id"; break;
    case SNAG_BINARY_GOAL_RESUMED:
    case SNAG_BINARY_GOAL_CANCELLED: keys = "goal_id"; break;
    default: return unsupported();
    }
    if (!snag_json_exact_keys(data, keys)) return invalid();
    if (read_id(data, "goal_id", event->data.goal.id) < 0) return -1;
    if (event->kind == SNAG_BINARY_GOAL_REPLACED &&
        read_id(data, "new_goal_id", event->data.goal.replacement) < 0) {
        return -1;
    }
    if (event->kind == SNAG_BINARY_GOAL_REPLACED || event->kind == SNAG_BINARY_GOAL_REWORDED ||
        event->kind == SNAG_BINARY_GOAL_BLOCKED || event->kind == SNAG_BINARY_GOAL_COMPLETED) {
        int actor = read_name(data, "actor", actors, sizeof(actors) / sizeof(actors[0]));
        if (actor < 0) return -1;
        event->data.goal.actor = (enum snag_binary_actor)actor;
    }
    switch (event->kind) {
    case SNAG_BINARY_GOAL_STARTED:
    case SNAG_BINARY_GOAL_REPLACED:
    case SNAG_BINARY_GOAL_REWORDED:
        return read_text(data, "prompt", &event->data.goal.text);
    case SNAG_BINARY_GOAL_BLOCKED:
        return read_text(data, "reason", &event->data.goal.text);
    case SNAG_BINARY_GOAL_LOCK_CHANGED: {
        const json_t *value = json_object_get(data, "locked");
        if (!json_is_boolean(value)) return invalid();
        event->data.goal.locked = json_is_true(value);
        return 0;
    }
    case SNAG_BINARY_GOAL_PAUSED: {
        int pause = read_name(data, "reason", pauses, sizeof(pauses) / sizeof(pauses[0]));
        if (pause < 0) return -1;
        event->data.goal.pause = (enum snag_binary_pause)pause;
        return 0;
    }
    default: return 0;
    }
}

static int
read_control(const json_t *data, struct snag_binary_event *event)
{
    uint64_t control;
    bool boundary = json_object_get(data, "origin") || json_object_get(data, "source_seq");
    if ((boundary && event->kind != SNAG_BINARY_CONTROL_REQUESTED) ||
        !snag_json_exact_keys(data, boundary ? "control origin source_seq" : "control") ||
        snag_json_integer_u64(data, "control", &control) < 0 || control > UINT_MAX) {
        return invalid();
    }
    event->data.control.control = (unsigned int)control;
    event->data.control.image_boundary = boundary;
    if (boundary) {
        const char *origin = snag_json_string(data, "origin");
        if (!origin || strcmp(origin, "image_boundary") ||
            snag_json_integer_u64(data, "source_seq", &event->data.control.source_seq) < 0) {
            return invalid();
        }
    }
    return 0;
}

static int
read_download(const json_t *data, struct snag_binary_event *event)
{
    switch (event->kind) {
    case SNAG_BINARY_DOWNLOAD_QUEUED: {
        if (!snag_json_exact_keys(data, "bytes id mtime name path queued_ms sha256")) {
            return invalid();
        }
        struct snag_binary_download *value = &event->data.download_queued;
        if (read_id(data, "id", value->id) < 0 ||
            read_digest(data, "sha256", value->sha256) < 0 ||
            read_text(data, "path", &value->path) < 0 ||
            read_text(data, "name", &value->name) < 0 ||
            snag_json_integer_u64(data, "bytes", &value->bytes) < 0 ||
            snag_json_integer_u64(data, "mtime", &value->mtime) < 0 ||
            snag_json_integer_u64(data, "queued_ms", &value->queued_ms) < 0) {
            return -1;
        }
        return 0;
    }
    case SNAG_BINARY_DOWNLOAD_REMOVED:
        if (!snag_json_exact_keys(data, "id reason")) return invalid();
        if (read_id(data, "id", event->data.download_removed.id) < 0 ||
            read_text(data, "reason", &event->data.download_removed.reason) < 0) {
            return -1;
        }
        return 0;
    case SNAG_BINARY_DOWNLOADS_CLEARED:
        if (!snag_json_exact_keys(data, "reason")) return invalid();
        return read_text(data, "reason", &event->data.downloads_cleared);
    default: return unsupported();
    }
}

static int
read_context(const json_t *data, struct snag_binary_event *event)
{
    if (event->kind == SNAG_BINARY_CONTEXT_REBASED) {
        if (!snag_json_exact_keys(data, "reason turn_id")) return invalid();
        int reason = read_name(data, "reason", rebase_reasons,
            sizeof(rebase_reasons) / sizeof(rebase_reasons[0]));
        if (reason < 0) return -1;
        event->data.context_rebased.reason = (enum snag_binary_rebase_reason)reason;
        return read_id(data, "turn_id", event->data.context_rebased.turn);
    }
    if (!snag_json_exact_keys(data,
        "code context_limit_tokens cycle message observed_hard_input_tokens "
        "provider_source_sha256 request_sha256 requested_input_tokens response_id turn_id")) {
        return invalid();
    }
    const char *code = snag_json_string(data, "code");
    uint64_t cycle;
    if (!code || strcmp(code, "context_length_exceeded") ||
        snag_json_integer_u64(data, "cycle", &cycle) < 0 || cycle > UINT32_MAX) {
        return invalid();
    }
    struct snag_binary_capacity_rejection *value = &event->data.capacity_rejected;
    value->cycle = (uint32_t)cycle;
    if (read_id(data, "turn_id", value->turn) < 0 ||
        read_id(data, "response_id", value->response) < 0 ||
        read_digest(data, "provider_source_sha256", value->provider_source_sha256) < 0 ||
        read_digest(data, "request_sha256", value->request_sha256) < 0 ||
        read_text(data, "message", &value->message) < 0 ||
        !snag_json_nullable_limit(data, "context_limit_tokens",
            SNAG_CONFIG_TOKEN_LIMIT_MAX, &value->context_limit) ||
        !snag_json_nullable_limit(data, "requested_input_tokens",
            SNAG_CONFIG_TOKEN_LIMIT_MAX, &value->requested_input) ||
        !snag_json_nullable_limit(data, "observed_hard_input_tokens",
            SNAG_CONFIG_TOKEN_LIMIT_MAX, &value->observed_input)) {
        return invalid();
    }
    return 0;
}

static int
read_hosted_search(const json_t *data, struct snag_binary_event *event, struct snag_buf *scratch)
{
    bool started = event->kind == SNAG_BINARY_HOSTED_SEARCH_STARTED;
    if (started ? (!snag_json_exact_keys(data, "item_id turn_id") &&
        !snag_json_exact_keys(data, "action item_id turn_id")) :
        (!snag_json_exact_keys(data, "item_id status turn_id") &&
        !snag_json_exact_keys(data, "item_id sources status turn_id"))) return invalid();
    struct snag_binary_hosted_search *value = &event->data.hosted_search;
    if (read_id(data, "turn_id", value->turn) < 0 ||
        read_text(data, "item_id", &value->item_id) < 0 ||
        (!started && read_text(data, "status", &value->status) < 0)) return -1;
    json_t *detail = json_object_get(data, started ? "action" : "sources");
    value->has_detail = detail != NULL;
    if (!detail) return 0;
    if (snag_binary_rule_value_encode(scratch, detail) < 0) return -1;
    return snag_binary_rule_value_decode(scratch->data, scratch->len, &value->detail);
}

static int
read_rule_voice(const json_t *data, struct snag_binary_event *event, struct snag_buf *scratch)
{
    switch (event->kind) {
    case SNAG_BINARY_RULE_LOG: {
        if (!snag_json_exact_keys(data, "chain message rule")) return invalid();
        static const char *const names[] = {"chain", "message", "rule"};
        struct snag_binary_result_value *fields[] = {
            &event->data.rule_log.chain, &event->data.rule_log.message, &event->data.rule_log.rule
        };
        size_t offsets[4] = {0};
        for (size_t i = 0u; i < 3u; ++i) {
            if (snag_binary_rule_value_encode(scratch, json_object_get(data, names[i])) < 0) {
                return -1;
            }
            offsets[i + 1u] = scratch->len;
        }
        /* Bind borrowed views only after the last possible buffer growth. */
        for (size_t i = 0u; i < 3u; ++i) {
            if (snag_binary_rule_value_decode(scratch->data + offsets[i],
                offsets[i + 1u] - offsets[i], fields[i]) < 0) {
                return -1;
            }
        }
        return 0;
    }
    case SNAG_BINARY_RULE_TRANSFORM: {
        if (!snag_json_exact_keys(data, "call_id effective_sha256 original_sha256 rule")) {
            return invalid();
        }
        struct snag_binary_rule_transform *value = &event->data.rule_transform;
        if (read_id(data, "call_id", value->call) < 0 ||
            read_digest(data, "original_sha256", value->original_sha256) < 0 ||
            read_digest(data, "effective_sha256", value->effective_sha256) < 0 ||
            read_text(data, "rule", &value->rule) < 0) {
            return -1;
        }
        return 0;
    }
    case SNAG_BINARY_AUDIO_USAGE: {
        if (!snag_json_exact_keys(data, "operation provider model report")) {
            return invalid();
        }
        const char *operation = snag_json_string(data, "operation");
        if (!operation || strcmp(operation, "dictation")) return invalid();
        struct snag_binary_audio_usage *value = &event->data.audio_usage;
        if (read_text(data, "provider", &value->provider) < 0 ||
            read_text(data, "model", &value->model) < 0 ||
            read_text(data, "report", &value->report) < 0) {
            return -1;
        }
        return 0;
    }
    case SNAG_BINARY_VOICE_EVENT: {
        if (!snag_json_exact_keys(data, "connection_id provider model event")) {
            return invalid();
        }
        struct snag_binary_voice_event *value = &event->data.voice_event;
        if (read_id(data, "connection_id", value->connection) < 0 ||
            read_text(data, "provider", &value->provider) < 0 ||
            read_text(data, "model", &value->model) < 0 ||
            snag_binary_voice_body_encode(scratch, json_object_get(data, "event")) < 0) {
            return -1;
        }
        return snag_binary_voice_body_decode(scratch->data, scratch->len, &value->body);
    }
    default: return unsupported();
    }
}

/* The caller must not grow scratch after this borrowed list is bound. */
static int
read_ids(const json_t *data, const char *key, struct snag_binary_ids *out, struct snag_buf *scratch)
{
    const json_t *array = json_object_get(data, key);
    if (!json_is_array(array)) return invalid();
    size_t count = json_array_size(array), begin = scratch->len;
    if (count > (SNAG_MAX_EVENT_LINE - 4u) / 16u) return invalid();
    for (size_t i = 0u; i < count; ++i) {
        const json_t *value = json_array_get(array, i);
        unsigned char id[16];
        if (read_hex(json_string_value(value), json_string_length(value), id, sizeof(id)) < 0 ||
            snag_buf_append(scratch, id, sizeof(id)) < 0) {
            return -1;
        }
    }
    out->values = count ? (const unsigned char (*)[16])(scratch->data + begin) : NULL;
    out->count = count;
    return 0;
}

static int
instruction_paths_valid(const json_t *paths, bool snapshots)
{
    /* Shape and exact uniqueness; rootedness requires the source platform. */
    if (!json_is_array(paths)) return invalid();
    bool metadata = snapshots && json_is_object(json_array_get(paths, 0u));
    json_t *seen = json_object();
    if (!seen) return -1;
    int rc = -1;
    for (size_t i = 0u; i < json_array_size(paths); ++i) {
        const json_t *value = json_array_get(paths, i);
        if (metadata && json_is_object(value)) {
            uint64_t bytes;
            unsigned char digest[32];
            if (!snag_json_exact_keys(value, "bytes path sha256") ||
                snag_json_integer_u64(value, "bytes", &bytes) < 0 ||
                read_digest(value, "sha256", digest) < 0) {
                (void)invalid();
                goto done;
            }
            value = json_object_get(value, "path");
        }
        const char *path = json_string_value(value);
        if (!path || !*path || json_string_length(value) > SNAG_PATH_MAX_BYTES ||
            json_object_get(seen, path)) {
            (void)invalid();
            goto done;
        }
        if (snag_json_set_new(seen, path, json_true()) < 0) goto done;
    }
    rc = 0;
 done:
    json_decref(seen);
    return rc;
}

static int
read_instruction_paths(const json_t *paths, struct snag_buf *out, bool snapshots)
{
    if (instruction_paths_valid(paths, snapshots) < 0) return -1;
    size_t count = json_array_size(paths);
    struct snag_binary_instruction *items = count ? calloc(count, sizeof(*items)) : NULL;
    if (count && !items) return -1;
    for (size_t i = 0u; i < count; ++i) {
        const json_t *item = json_array_get(paths, i);
        const json_t *path = item;
        items[i].has_snapshot = json_is_object(item);
        if (items[i].has_snapshot) {
            if (snag_json_integer_u64(item, "bytes", &items[i].bytes) < 0 ||
                read_digest(item, "sha256", items[i].sha256) < 0) {
                free(items);
                return -1;
            }
            path = json_object_get(item, "path");
        }
        items[i].path = (struct snag_binary_text){
            (const unsigned char *)json_string_value(path), json_string_length(path)
        };
    }
    int rc = snag_binary_instructions_encode(out, items, count);
    free(items);
    return rc;
}

static int
read_asset(const json_t *data, struct snag_binary_asset *asset)
{
    if (read_id(data, "id", asset->id) < 0 ||
        read_digest(data, "sha256", asset->sha256) < 0 ||
        read_text(data, "mime_type", &asset->mime) < 0 ||
        snag_json_integer_u64(data, "bytes", &asset->bytes) < 0) {
        return -1;
    }
    return 0;
}

static int
read_content(const json_t *content, struct snag_buf *out)
{
    if (!snag_media_content_valid(content)) return invalid();
    size_t count = json_array_size(content);
    struct snag_binary_part *parts = calloc(count, sizeof(*parts));
    if (!parts) return -1;
    int rc = -1;
    for (size_t i = 0u; i < count; ++i) {
        const json_t *item = json_array_get(content, i);
        struct snag_binary_part *part = &parts[i];
        const char *type = snag_json_string(item, "type");
        if (!strcmp(type, "input_text")) {
            part->kind = SNAG_BINARY_PART_TEXT;
            if (read_text(item, "text", &part->text) < 0) goto done;
            continue;
        }
        part->kind = !strcmp(type, "file") ? SNAG_BINARY_PART_FILE : SNAG_BINARY_PART_IMAGE;
        if (read_asset(json_object_get(item, "asset"), &part->asset) < 0) {
            goto done;
        }
        const json_t *source = json_object_get(item, "source");
        part->has_source = source != NULL;
        if (source && (read_asset(source, &part->source) < 0 ||
            read_text(item, "note", &part->text) < 0)) {
            goto done;
        }
    }
    rc = snag_binary_content_encode(out, parts, count);
 done:
    free(parts);
    return rc;
}

static int
read_input_receipt(const json_t *data, struct snag_binary_event *event, struct snag_buf *scratch)
{
    size_t start = scratch->len;
    size_t instruction_size = 0u;
    struct snag_binary_content *content;
    if (event->kind == SNAG_BINARY_INPUT_RECEIVED) {
        if (!snag_json_arg_keys(data,
            "effort instructions model provider read_only received_at_ms text",
            "content origin", NULL, 0u) ||
            !json_is_boolean(json_object_get(data, "read_only")) ||
            (json_object_get(data, "origin") &&
             !snag_string_in(snag_json_string(data, "origin"), "timer"))) {
            return invalid();
        }
        struct snag_binary_selection *selection = &event->data.input.selection;
        if (read_text(data, "provider", &selection->provider) < 0 ||
            read_text(data, "model", &selection->model) < 0 ||
            read_text(data, "effort", &selection->effort) < 0 ||
            read_text(data, "text", &event->data.input.text) < 0 ||
            snag_json_integer_u64(data, "received_at_ms", &event->data.input.received_ms) < 0 ||
            read_instruction_paths(json_object_get(data, "instructions"), scratch, false) < 0) {
            return -1;
        }
        instruction_size = scratch->len - start;
        event->data.input.read_only = json_is_true(json_object_get(data, "read_only"));
        event->data.input.origin = json_object_get(data, "origin") ? SNAG_BINARY_INPUT_TIMER :
            SNAG_BINARY_INPUT_DEFAULT;
        content = &event->data.input.content;
    } else {
        const char *text = snag_json_string(data, "text");
        if (!snag_json_arg_keys(data, "steering_id text turn_id",
            "content received_at_ms", NULL, 0u) ||
            (event->kind == SNAG_BINARY_IRC_REPLY_REMINDER &&
             (!text || strcmp(text, SNAG_IRC_REPLY_REMINDER_TEXT)))) {
            return invalid();
        }
        if (read_id(data, "steering_id", event->data.steering_input.id) < 0 ||
            read_id(data, "turn_id", event->data.steering_input.turn) < 0 ||
            read_text(data, "text", &event->data.steering_input.text) < 0) {
            return -1;
        }
        bool received = json_object_get(data, "received_at_ms") != NULL;
        event->data.steering_input.has_received_ms = received;
        if (event->data.steering_input.has_received_ms &&
            snag_json_integer_u64(data, "received_at_ms",
                &event->data.steering_input.received_ms) < 0) {
            return -1;
        }
        content = &event->data.steering_input.content;
    }
    size_t content_start = scratch->len;
    const json_t *parts = json_object_get(data, "content");
    if (parts && read_content(parts, scratch) < 0) return -1;
    /* Bind both views only after the last possible scratch reallocation. */
    if (instruction_size) {
        event->data.input.instructions = (struct snag_binary_instructions){
            scratch->data + start, instruction_size
        };
    }
    if (parts) {
        *content = (struct snag_binary_content){
            scratch->data + content_start, scratch->len - content_start
        };
    }
    return 0;
}

static int
queued_voice_id_valid(const json_t *data)
{
    /* Legacy identity hashes exactly these two canonical JSON fields. */
    const json_t *voice = json_object_get(data, "voice");
    json_t *key = json_pack("{s:O,s:O}",
        "connection_id", json_object_get(voice, "connection_id"),
        "input_id", json_object_get(voice, "input_id"));
    if (!key) return -1;
    char digest[65];
    int rc = snag_json_digest(key, digest);
    json_decref(key);
    if (rc < 0) return -1;
    const char *id = snag_json_string(data, "queue_id");
    if (!id || strlen(id) != 32u || memcmp(id, digest, 32u)) return invalid();
    return 0;
}

static int
host_context_shape(const json_t *array)
{
    size_t count = json_array_size(array);
    if (!json_is_array(array) || count < 2u) return invalid();
    for (size_t i = 0u; i < count; ++i) {
        const json_t *item = json_array_get(array, i);
        const char *role = snag_json_string(item, "role");
        const char *text = snag_json_string(item, "content");
        if (!snag_json_exact_keys(item, "role content") || !role || strcmp(role, "user") ||
            !snag_text_valid(text, 1u, SNAG_MAX_EVENT_LINE) ||
            (!i && strcmp(text, SNAG_HOST_CONTEXT_BEGIN)) ||
            (i + 1u == count && strcmp(text, SNAG_HOST_CONTEXT_END))) {
            return invalid();
        }
    }
    return 0;
}

static int
read_host_context(const json_t *array, struct snag_buf *scratch)
{
    if (host_context_shape(array) < 0) return -1;
    size_t count = json_array_size(array);
    struct snag_binary_part *parts = calloc(count, sizeof(*parts));
    if (!parts) return -1;
    int rc = -1;
    for (size_t i = 0u; i < count; ++i) {
        parts[i].kind = SNAG_BINARY_PART_TEXT;
        if (read_text(json_array_get(array, i), "content", &parts[i].text) < 0) {
            goto done;
        }
    }
    rc = snag_binary_content_encode(scratch, parts, count);
 done:
    free(parts);
    return rc;
}

static int
read_response_start(const json_t *data, struct snag_binary_response_start *start,
    struct snag_buf *scratch)
{
    static const char common[] = "baseline_sha256 capability_version compact_id count_method "
        "count_request_sha256 cycle input_tokens_bound model model_input_sha256 profile_id "
        "request_sha256 response_id steering_ids turn_id";
    static const char full[] = "baseline_sha256 capability_version compact_id capacity_source "
        "count_method count_request_sha256 cycle effort hard_input_tokens input_tokens_bound model "
        "model_input_bytes model_input_sha256 profile_id provider provider_source_sha256 "
        "request_input_bytes request_input_count request_input_sha256 request_sha256 "
        "requested_output_tokens response_id source_bound steering_ids turn_id";
    start->full_accounting = json_object_get(data, "provider") != NULL;
    start->has_irc_seq = json_object_get(data, "irc_seq") != NULL;
    start->has_baseline = !json_is_null(json_object_get(data, "baseline_sha256"));
    start->has_compact = !json_is_null(json_object_get(data, "compact_id"));
    if (!snag_json_arg_keys(data, start->full_accounting ? full : common,
        start->full_accounting ? "irc_seq host_context" : "irc_seq", NULL, 0u)) {
        return invalid();
    }
    int method = read_name(data, "count_method", count_methods,
        sizeof(count_methods) / sizeof(count_methods[0]));
    uint64_t cycle;
    if (method < 0 || snag_json_integer_u64(data, "cycle", &cycle) < 0 || cycle > UINT32_MAX) {
        return invalid();
    }
    start->cycle = (uint32_t)cycle;
    start->count_method = (enum snag_binary_count_method)method;
    if (read_id(data, "turn_id", start->turn) < 0 ||
        read_id(data, "response_id", start->response) < 0 ||
        read_digest(data, "request_sha256", start->request_sha256) < 0 ||
        read_digest(data, "count_request_sha256", start->count_request_sha256) < 0 ||
        read_digest(data, "model_input_sha256", start->model_input_sha256) < 0 ||
        read_text(data, "model", &start->selection.model) < 0 ||
        read_text(data, "capability_version", &start->capability) < 0 ||
        read_text(data, "profile_id", &start->profile) < 0 ||
        snag_json_integer_u64(data, "input_tokens_bound", &start->input_tokens_bound) < 0 ||
        (start->has_baseline && read_digest(data, "baseline_sha256", start->baseline_sha256) < 0) ||
        (start->has_compact && read_id(data, "compact_id", start->compact) < 0) ||
        (start->has_irc_seq && snag_json_integer_u64(data, "irc_seq", &start->irc_seq) < 0)) {
        return -1;
    }
    if (start->full_accounting) {
        int source = read_name(data, "capacity_source", capacity_sources,
            sizeof(capacity_sources) / sizeof(capacity_sources[0]));
        start->has_hard_input = !json_is_null(json_object_get(data, "hard_input_tokens"));
        start->has_requested_output =
            !json_is_null(json_object_get(data, "requested_output_tokens"));
        if (source < 0 || !json_is_boolean(json_object_get(data, "source_bound"))) {
            return invalid();
        }
        start->capacity_source = (enum snag_binary_capacity_source)source;
        start->source_bound = json_is_true(json_object_get(data, "source_bound"));
        if (read_text(data, "provider", &start->selection.provider) < 0 ||
            read_text(data, "effort", &start->selection.effort) < 0 ||
            read_digest(data, "provider_source_sha256", start->provider_source_sha256) < 0 ||
            read_digest(data, "request_input_sha256", start->request_input_sha256) < 0 ||
            snag_json_integer_u64(data, "model_input_bytes", &start->model_input_bytes) < 0 ||
            snag_json_integer_u64(data, "request_input_bytes", &start->request_input_bytes) < 0 ||
            snag_json_integer_u64(data, "request_input_count", &start->request_input_count) < 0 ||
            (start->has_hard_input &&
             snag_json_integer_u64(data, "hard_input_tokens", &start->hard_input_tokens) < 0) ||
            !snag_json_nullable_limit(data, "requested_output_tokens",
                SNAG_CONFIG_TOKEN_LIMIT_MAX, &start->requested_output_tokens)) {
            return invalid();
        }
    }
    size_t ids_start = scratch->len;
    if (read_ids(data, "steering_ids", &start->steering, scratch) < 0) return -1;
    size_t host_start = scratch->len;
    const json_t *host = json_object_get(data, "host_context");
    if (host && read_host_context(host, scratch) < 0) return -1;
    start->steering.values = start->steering.count ?
        (const unsigned char (*)[16])(scratch->data + ids_start) : NULL;
    if (host) {
        start->host_context = (struct snag_binary_content){
            scratch->data + host_start, scratch->len - host_start
        };
    }
    return 0;
}

static int
read_response_ids(const json_t *data, unsigned char turn[16], unsigned char response[16],
    uint32_t *cycle)
{
    uint64_t number;
    if (read_id(data, "turn_id", turn) < 0 || read_id(data, "response_id", response) < 0 ||
        snag_json_integer_u64(data, "cycle", &number) < 0 || !number || number > UINT32_MAX) {
        return invalid();
    }
    *cycle = (uint32_t)number;
    return 0;
}

static int
read_public_item(const json_t *data, struct snag_binary_public_item *item)
{
    if (!snag_json_exact_keys(data, "kind local_item_id phase provider_item_id text")) {
        return invalid();
    }
    int kind = read_name(data, "kind", public_kinds, 3u);
    int phase = read_name(data, "phase", public_phases, 3u);
    if (kind < 0 || phase < 0 || read_id(data, "local_item_id", item->id) < 0 ||
        read_text(data, "provider_item_id", &item->provider_id) < 0 ||
        read_text(data, "text", &item->text) < 0) {
        return invalid();
    }
    item->kind = (enum snag_binary_item_kind)kind;
    item->phase = (enum snag_binary_item_phase)phase;
    return 0;
}

static int
public_item_source_valid(const json_t *item)
{
    json_t *one = json_pack("[O]", item);
    if (!one) return -1;
    int rc = snag_partial_public_validate(one, NULL, 0u);
    json_decref(one);
    return rc;
}

static int
read_public_items(const json_t *array, struct snag_binary_public_items *out,
    struct snag_buf *scratch)
{
    if (snag_partial_public_validate(array, NULL, 0u) < 0) return -1;
    size_t count = json_array_size(array), begin = scratch->len;
    struct snag_binary_public_value *values = count ? calloc(count, sizeof(*values)) : NULL;
    if (count && !values) return -1;
    int rc = -1;
    for (size_t i = 0u; i < count; ++i) {
        if (read_public_item(json_array_get(array, i), &values[i].item) < 0) {
            goto done;
        }
    }
    if (snag_binary_public_items_encode(scratch, values, count) < 0) goto done;
    *out = (struct snag_binary_public_items){scratch->data + begin, scratch->len - begin};
    rc = 0;
done:
    free(values);
    return rc;
}

static int
read_response_output(const json_t *data, struct snag_binary_response_output *value)
{
    const json_t *item = json_object_get(data, "item");
    if (!snag_json_exact_keys(data, "cycle index item offset response_id turn_id") ||
        read_response_ids(data, value->turn, value->response, &value->cycle) < 0 ||
        snag_json_integer_u64(data, "index", &value->index) < 0 ||
        snag_json_integer_u64(data, "offset", &value->offset) < 0 ||
        public_item_source_valid(item) < 0) {
        return invalid();
    }
    return read_public_item(item, &value->item);
}

static int
read_response_failure(const json_t *data, struct snag_binary_response_failure *value,
    struct snag_buf *scratch)
{
    if (!snag_json_arg_keys(data,
        "class cycle message partial_public response_id retry_count turn_id",
        "policy_stopped turn_retry_attempts new_input policy", NULL, 0u)) {
        return invalid();
    }
    uint64_t retry;
    int class_name = read_name(data, "class", failure_classes,
        sizeof(failure_classes) / sizeof(failure_classes[0]));
    if (class_name < 0 ||
        read_response_ids(data, value->turn, value->response, &value->cycle) < 0 ||
        read_text(data, "message", &value->message) < 0 ||
        snag_json_integer_u64(data, "retry_count", &retry) < 0 || retry > 2u) {
        return invalid();
    }
    value->class_name = (enum snag_binary_failure_class)class_name;
    value->retry_count = (uint8_t)retry;
    const json_t *stopped = json_object_get(data, "policy_stopped");
    const json_t *handoff = json_object_get(data, "new_input");
    const json_t *policy = json_object_get(data, "policy");
    if (stopped) {
        if (!json_is_boolean(stopped)) return invalid();
        value->present |= SNAG_BINARY_FAILURE_POLICY_STOPPED;
        value->policy_stopped = json_is_true(stopped);
    }
    if (json_object_get(data, "turn_retry_attempts")) {
        if (snag_json_integer_u64(data, "turn_retry_attempts", &value->turn_retry_attempts) < 0) {
            return invalid();
        }
        value->present |= SNAG_BINARY_FAILURE_TURN_RETRIES;
    }
    if (handoff) {
        if (!json_is_boolean(handoff)) return invalid();
        value->present |= SNAG_BINARY_FAILURE_NEW_INPUT;
        value->new_input = json_is_true(handoff);
    }
    if (policy) {
        if (!snag_json_exact_keys(policy, "code type clarification_skipped") ||
            read_text(policy, "code", &value->policy_code) < 0 ||
            read_text(policy, "type", &value->policy_type) < 0 ||
            read_text(policy, "clarification_skipped", &value->clarification_skipped) < 0) {
            return invalid();
        }
        value->present |= SNAG_BINARY_FAILURE_POLICY;
    }
    return read_public_items(json_object_get(data, "partial_public"), &value->partial, scratch);
}

static int
read_response_end(const json_t *data, struct snag_binary_event *event, struct snag_buf *scratch)
{
    if (event->kind == SNAG_BINARY_RESPONSE_FAILED) {
        return read_response_failure(data, &event->data.response_failed, scratch);
    }
    if (event->kind == SNAG_BINARY_RESPONSE_INTERRUPTED) {
        struct snag_binary_response_interruption *value = &event->data.response_interrupted;
        if (!snag_json_exact_keys(data,
            "cycle origin partial_public reason response_id turn_id")) {
            return invalid();
        }
        int origin = read_name(data, "origin", interrupt_origins,
            sizeof(interrupt_origins) / sizeof(interrupt_origins[0]));
        int reason = read_name(data, "reason", interrupt_reasons,
            sizeof(interrupt_reasons) / sizeof(interrupt_reasons[0]));
        if (origin < 0 || reason < 0 ||
            read_response_ids(data, value->turn, value->response, &value->cycle) < 0) {
            return invalid();
        }
        value->origin = (enum snag_binary_interrupt_origin)origin;
        value->reason = (enum snag_binary_interrupt_reason)reason;
        return read_public_items(json_object_get(data, "partial_public"), &value->partial, scratch);
    }
    struct snag_binary_response_correction *value = &event->data.response_correction;
    if (!snag_json_exact_keys(data,
        "correction_id cycle partial_public response_id text turn_id") ||
        read_response_ids(data, value->turn, value->response, &value->cycle) < 0 ||
        read_id(data, "correction_id", value->correction) < 0 ||
        read_text(data, "text", &value->text) < 0) {
        return invalid();
    }
    return read_public_items(json_object_get(data, "partial_public"), &value->partial, scratch);
}

static int
graph_source_valid(const json_t *items, const char *provider_id)
{
    if (!json_is_array(items)) return invalid();
    /* Classification borrows the provider/array views without mutating or freeing them. */
    struct snag_response_graph graph = {
        .provider_response_id = (char *)provider_id, .items = (json_t *)items,
        .count = json_array_size(items)
    };
    struct snag_graph_decision decision;
    return snag_response_graph_classify(&graph, &decision, NULL, 0u);
}

static int
append_provider_json(const json_t *value, size_t max, struct snag_buf *fields)
{
    struct snag_buf encoded = {.max = max};
    int rc = snag_json_canonical(value, &encoded);
    if (rc == 0) rc = snag_buf_append(fields, encoded.data, encoded.len);
    snag_buf_free(&encoded);
    return rc;
}

static int
read_graph_items(const json_t *array, struct snag_binary_graph_items *out, struct snag_buf *scratch)
{
    size_t count = json_array_size(array), begin = scratch->len;
    if (count > UINT32_MAX) return invalid();
    struct snag_binary_graph_item *items = count ? calloc(count, sizeof(*items)) : NULL;
    size_t *offsets = count ? calloc(count, sizeof(*offsets)) : NULL;
    struct snag_buf fields = {.max = SNAG_MAX_RESPONSE_GRAPH};
    int rc = -1;
    if (count && (!items || !offsets)) goto done;
    for (size_t i = 0u; i < count; ++i) {
        const json_t *item = json_array_get(array, i);
        const char *kind = snag_json_string(item, "kind");
        if (!kind) goto done;
        if (strcmp(kind, "tool_call")) {
            if (read_public_item(item, &items[i].data.output.item) < 0) {
                goto done;
            }
            items[i].kind = items[i].data.output.item.kind;
            continue;
        }
        items[i].kind = SNAG_BINARY_ITEM_TOOL_CALL;
        struct snag_binary_call *call = &items[i].data.call;
        if (read_id(item, "call_id", call->id) < 0 ||
            read_text(item, "provider_item_id", &call->provider_id) < 0 ||
            read_text(item, "provider_call_id", &call->provider_call_id) < 0 ||
            read_text(item, "name", &call->name) < 0) {
            goto done;
        }
        offsets[i] = fields.len;
        if (append_provider_json(json_object_get(item, "arguments"),
            SNAG_MAX_TOOL_ARGUMENTS, &fields) < 0) {
            goto done;
        }
        call->arguments.size = fields.len - offsets[i];
    }
    for (size_t i = 0u; i < count; ++i) {
        if (items[i].kind == SNAG_BINARY_ITEM_TOOL_CALL) {
            items[i].data.call.arguments.data = fields.data + offsets[i];
        }
    }
    if (snag_binary_graph_items_encode(scratch, items, count) < 0) goto done;
    *out = (struct snag_binary_graph_items){scratch->data + begin, scratch->len - begin,
        (uint32_t)count};
    rc = 0;
done:
    snag_buf_free(&fields);
    free(offsets);
    free(items);
    return rc;
}

static int
read_continuation(const json_t *array, uint32_t semantic_count,
    struct snag_binary_continuation *out, struct snag_buf *scratch)
{
    if (!snag_response_continuation_valid(array, semantic_count)) {
        return invalid();
    }
    size_t count = json_array_size(array), begin = scratch->len;
    struct snag_binary_continuation_item *items = count ? calloc(count, sizeof(*items)) : NULL;
    size_t *offsets = count ? calloc(count, sizeof(*offsets)) : NULL;
    struct snag_buf fields = {.max = SNAG_MAX_RESPONSE_GRAPH};
    int rc = -1;
    if (count && (!items || !offsets)) goto done;
    for (size_t i = 0u; i < count; ++i) {
        const json_t *item = json_array_get(array, i);
        offsets[i] = fields.len;
        if (snag_json_integer_u64(item, "before", &items[i].before) < 0 ||
            append_provider_json(json_object_get(item, "item"),
                SNAG_MAX_RESPONSE_GRAPH, &fields) < 0) {
            goto done;
        }
        items[i].item.size = fields.len - offsets[i];
    }
    for (size_t i = 0u; i < count; ++i) {
        items[i].item.data = fields.data + offsets[i];
    }
    if (snag_binary_continuation_encode(scratch, items, count, semantic_count) < 0) {
        goto done;
    }
    *out = (struct snag_binary_continuation){scratch->data + begin, scratch->len - begin};
    rc = 0;
done:
    snag_buf_free(&fields);
    free(offsets);
    free(items);
    return rc;
}

static int
read_response_complete(const json_t *data, struct snag_binary_response_complete *value,
    struct snag_buf *scratch)
{
    const json_t *continuation = json_object_get(data, "continuation");
    if (!snag_json_exact_keys(data, continuation ?
        "cycle items provider_response_id response_id status turn_id usage "
        "continuation continuation_scope" :
        "cycle items provider_response_id response_id status turn_id usage")) {
        return invalid();
    }
    const char *status = snag_json_string(data, "status");
    const json_t *items = json_object_get(data, "items");
    const json_t *usage = json_object_get(data, "usage");
    if (!status || strcmp(status, "completed") ||
        read_response_ids(data, value->turn, value->response, &value->cycle) < 0 ||
        read_text(data, "provider_response_id", &value->provider_id) < 0 ||
        snag_response_usage_from_json(usage, &value->usage) < 0 ||
        graph_source_valid(items, snag_json_string(data, "provider_response_id")) < 0) {
        return invalid();
    }
    value->cached_present = json_object_get(usage, "cached_tokens") != NULL;
    size_t graph_start = scratch->len;
    if (read_graph_items(items, &value->items, scratch) < 0) return -1;
    if (continuation) {
        if (read_digest(data, "continuation_scope", value->continuation_scope) < 0) {
            return -1;
        }
        value->continuation_form = json_is_null(continuation) ?
            SNAG_BINARY_CONTINUATION_NULL : SNAG_BINARY_CONTINUATION_ITEMS;
        if (value->continuation_form == SNAG_BINARY_CONTINUATION_ITEMS &&
            read_continuation(continuation, value->items.count,
                &value->continuation, scratch) < 0) {
            return -1;
        }
    }
    value->items.data = scratch->data + graph_start;
    return 0;
}

static int
read_transfer_archive(const json_t *data, struct snag_binary_voice_archive *value,
                      struct snag_buf *scratch)
{
    if (!snag_json_exact_keys(data,
        "transfer_id target_session_id source_session_id source_seq source_type data")) {
        return invalid();
    }
    if (read_id(data, "transfer_id", value->id) < 0 ||
        read_id(data, "target_session_id", value->target) < 0 ||
        read_id(data, "source_session_id", value->source) < 0 ||
        snag_json_integer_u64(data, "source_seq", &value->source_seq) < 0) {
        return -1;
    }
    size_t start = scratch->len;
    if (snag_binary_archive_fields_encode(scratch, snag_json_string(data, "source_type"),
        json_object_get(data, "data"), &value->source_kind) < 0) {
        return -1;
    }
    value->source_version = SNAG_BINARY_ARCHIVE_PUBLIC_VERSION;
    value->data = scratch->data + start;
    value->size = scratch->len - start;
    return 0;
}

static int
read_transfer_metadata(const json_t *data, struct snag_binary_event *event)
{
    bool adopted = event->kind == SNAG_BINARY_VOICE_TRANSFER_ADOPTED;
    if (!snag_json_exact_keys(data, adopted ?
        "transfer_id target_session_id source_session_id source_as_of_seq count "
        "begin_offset begin_seq begin_sha256" :
        "transfer_id target_session_id source_session_id source_as_of_seq count")) {
        return invalid();
    }
    struct snag_binary_voice_transfer *transfer = adopted ?
        &event->data.voice_transfer_adopted.transfer : &event->data.voice_transfer_sealed;
    if (read_id(data, "transfer_id", transfer->id) < 0 ||
        read_id(data, "target_session_id", transfer->target) < 0 ||
        read_id(data, "source_session_id", transfer->source) < 0 ||
        snag_json_integer_u64(data, "source_as_of_seq", &transfer->source_as_of) < 0 ||
        snag_json_integer_u64(data, "count", &transfer->count) < 0) {
        return -1;
    }
    if (!adopted) return 0;
    struct snag_binary_voice_adopted *value = &event->data.voice_transfer_adopted;
    if (snag_json_integer_u64(data, "begin_offset", &value->begin_offset) < 0 ||
        snag_json_integer_u64(data, "begin_seq", &value->begin_seq) < 0) {
        return -1;
    }
    return read_digest(data, "begin_sha256", value->begin_sha256);
}

static int
read_irc_event(const json_t *data, struct snag_binary_irc_event *event, bool routed)
{
    struct snag_irc_event source;
    if (snag_irc_event_record_read(routed ? "irc_event_v2" : "irc_event", data,
        &source) < 0) return -1;
    int kind = read_name(data, "kind", irc_kinds, 12u);
    if (kind < 0 || read_text(data, "endpoint", &event->endpoint) < 0 ||
        read_text(data, "room", &event->room) < 0 || read_text(data, "nick", &event->nick) < 0 ||
        read_text(data, "text", &event->text) < 0) {
        return -1;
    }
    event->kind = (enum snag_binary_irc_kind)kind;
    event->timestamp_ms = source.timestamp_ms;
    event->sequence = source.sequence;
    event->historical = source.historical;
    event->is_local = source.local;
    event->op = source.op;
    event->has_watermark = json_object_get(data, "stream") != NULL;
    event->has_stream = source.stream[0] != '\0';
    event->input = source.input;
    event->classified = source.classified;
    event->urgent = source.urgent;
    event->reply = source.reply;
    if (event->has_stream && read_id(data, "stream", event->stream) < 0) return -1;
    if (!routed) return 0;
    const json_t *routing = json_object_get(data, "routing");
    struct snag_binary_irc_route *route = &event->route;
    route->generation = source.route.generation;
    route->identity = source.route.identity;
    route->kind = source.route.kind;
    route->direction = source.route.direction;
    route->delivery = source.route.delivery;
    route->action = source.route.action;
    route->revised = source.route.revised;
    route->joined = source.route.joined;
    route->rejoin = source.route.rejoin;
    route->has_membership = source.route.membership[0] != '\0';
    route->has_send = source.route.send[0] != '\0';
    route->reply_captured = source.reply_captured;
    route->has_reply = source.reply_conversation[0] != '\0';
    const json_t *reply = json_object_get(data, "reply_to");
    if (read_id(routing, "connection_id", route->connection) < 0 ||
        read_id(routing, "conversation_id", route->conversation) < 0 ||
        read_text(routing, "peer", &route->peer) < 0 ||
        read_text(routing, "target", &route->target) < 0 ||
        read_text(routing, "source_message_id", &route->source) < 0 ||
        (route->has_send && read_id(routing, "send_id", route->send) < 0) ||
        (route->has_membership && read_id(routing, "membership", route->membership) < 0) ||
        (route->has_reply && (read_id(reply, "conversation_id", route->reply_conversation) < 0 ||
            read_id(reply, "membership", route->reply_membership) < 0))) return -1;
    return 0;
}

static int
read_irc(const json_t *data, struct snag_binary_event *event, struct snag_buf *scratch)
{
    if (event->kind == SNAG_BINARY_IRC_SLEEP_SET) {
        uint64_t messages;
        if (!snag_json_exact_keys(data, "until_ms messages") ||
            snag_json_integer_u64(data, "until_ms", &event->data.irc_sleep_set.until_ms) < 0 ||
            snag_json_integer_u64(data, "messages", &messages) < 0 || messages > UINT32_MAX)
            return invalid();
        event->data.irc_sleep_set.messages = (uint32_t)messages;
        return 0;
    }
    if (event->kind == SNAG_BINARY_IRC_SLEEP_WOKE) {
        int reason = read_name(data, "reason", irc_wake_reasons, 4u);
        if (!snag_json_exact_keys(data, "reason") || reason < 0) return invalid();
        event->data.irc_sleep_woke = reason;
        return 0;
    }
    if (event->kind == SNAG_BINARY_IRC_COMPACT_CONFIGURED) {
        uint64_t updates;
        if (!snag_json_exact_keys(data, "after_updates instruction") ||
            snag_json_integer_u64(data, "after_updates", &updates) < 0 || updates > UINT32_MAX ||
            read_text(data, "instruction", &event->data.irc_compact_configured.instruction) < 0) {
            return invalid();
        }
        event->data.irc_compact_configured.after_updates = (uint32_t)updates;
        return 0;
    }
    if (event->kind == SNAG_BINARY_IRC_COMPACTED) {
        uint64_t *through = &event->data.irc_compacted.through_seq;
        if (!snag_json_exact_keys(data, "through_seq count summary") ||
            snag_json_integer_u64(data, "through_seq", through) < 0 ||
            snag_json_integer_u64(data, "count", &event->data.irc_compacted.count) < 0 ||
            read_text(data, "summary", &event->data.irc_compacted.summary) < 0) return invalid();
        return 0;
    }
    if (event->kind == SNAG_BINARY_IRC_EVENT || event->kind == SNAG_BINARY_IRC_EVENT_V2) {
        return read_irc_event(data, &event->data.irc_event,
            event->kind == SNAG_BINARY_IRC_EVENT_V2);
    }
    if (event->kind == SNAG_BINARY_IRC_SNAPSHOT) {
        if (!snag_json_exact_keys(data, "reason text timestamp_ms")) {
            return invalid();
        }
        struct snag_binary_irc_snapshot *snapshot = &event->data.irc_snapshot;
        int reason = read_name(data, "reason", irc_snapshot_reasons, 5u);
        if (reason < 0 || read_text(data, "text", &snapshot->text) < 0 ||
            snag_json_integer_u64(data, "timestamp_ms", &snapshot->timestamp_ms) < 0) {
            return -1;
        }
        snapshot->reason = (enum snag_binary_irc_snapshot_reason)reason;
        return 0;
    }
    const json_t *input = json_object_get(data, "input");
    const json_t *steering = json_object_get(data, "steering");
    const json_t *sequences = json_object_get(data, "sequences");
    if (!snag_json_exact_keys(data, input ? "input sequences" :
        steering ? "sequences steering" : "sequences") ||
        !json_is_array(sequences) || !json_array_size(sequences)) {
        return invalid();
    }
    size_t count = json_array_size(sequences);
    uint64_t *values = calloc(count, sizeof(*values));
    if (!values) return -1;
    int rc = -1;
    for (size_t i = 0u; i < count; ++i) {
        const json_t *value = json_array_get(sequences, i);
        if (!json_is_integer(value) || json_integer_value(value) <= 0) {
            (void)invalid();
            goto done;
        }
        values[i] = (uint64_t)json_integer_value(value);
    }
    size_t begin = scratch->len;
    if (snag_binary_sequences_encode(scratch, values, count) < 0) goto done;
    size_t child_start = scratch->len;
    struct snag_binary_irc_admission *admission = &event->data.irc_admitted;
    if (input || steering) {
        enum snag_binary_kind kind;
        if (snag_binary_legacy_encode(scratch, input ? "input_received" : "steering_added",
            input ? input : steering, &kind) < 0) {
            goto done;
        }
        admission->input = (struct snag_binary_record){
            .kind = (uint16_t)kind, .version = snag_binary_event_version(kind),
            .payload = scratch->data + child_start, .size = scratch->len - child_start
        };
    }
    admission->sequences = (struct snag_binary_sequences){
        scratch->data + begin, child_start - begin
    };
    rc = 0;
 done:
    free(values);
    return rc;
}

static int
read_tool_result(const json_t *data, struct snag_binary_tool_result *result,
                 struct snag_buf *scratch)
{
    if (snag_tool_result_valid(data) < 0) return invalid();
    int status = read_name(data, "status", tool_statuses,
        sizeof(tool_statuses) / sizeof(tool_statuses[0]));
    int reason = json_is_null(json_object_get(data, "reason")) ? 0 :
        read_name(data, "reason", tool_reasons, sizeof(tool_reasons) / sizeof(tool_reasons[0]));
    if (status < 0 || reason < 0 ||
        read_text(data, "model_text", &result->model_text) < 0 ||
        snag_json_integer_u64(data, "duration_ms", &result->duration_ms) < 0) {
        return -1;
    }
    result->status = (enum snag_binary_tool_status)status;
    result->reason = (enum snag_binary_tool_reason)reason;
    result->has_handle = !json_is_null(json_object_get(data, "handle"));
    if (result->has_handle && read_id(data, "handle", result->handle) < 0) {
        return -1;
    }
    if (json_object_get(data, "max_output_tokens") &&
        snag_json_integer_u64(data, "max_output_tokens", &result->max_output_tokens) < 0) {
        return -1;
    }
    const char *streams[] = {"stdout", "stderr"};
    const char *from[] = {"stdout_start", "stderr_start"};
    const char *to[] = {"stdout_end", "stderr_end"};
    const json_t *ref = json_object_get(data, "output_ref");
    result->has_output_ref = ref != NULL;
    for (size_t i = 0u; i < 2u; ++i) {
        const json_t *part = json_object_get(data, streams[i]);
        struct snag_binary_tool_excerpt *excerpt = &result->streams[i];
        int encoding = read_name(part, "encoding", byte_encodings, 3u);
        if (encoding < 0 || read_text(part, "retained", &excerpt->retained) < 0 ||
            snag_json_integer_u64(part, "discarded_bytes", &excerpt->discarded_bytes) < 0 ||
            snag_json_integer_u64(part, "original_bytes", &excerpt->original_bytes) < 0 ||
            snag_json_integer_u64(part, "retained_bytes", &excerpt->retained_bytes) < 0 ||
            (ref && (snag_json_integer_u64(ref, from[i], &result->output_ref.from[i]) < 0 ||
                     snag_json_integer_u64(ref, to[i], &result->output_ref.to[i]) < 0))) {
            return -1;
        }
        excerpt->encoding = (enum snag_binary_excerpt_encoding)encoding;
    }
    if (ref) {
        struct snag_binary_tool_output_ref *out = &result->output_ref;
        if (read_id(ref, "handle", out->handle) < 0 ||
            snag_json_integer_u64(ref, "stdin_accepted", &out->stdin_accepted) < 0 ||
            snag_json_integer_u64(ref, "stdin_written", &out->stdin_written) < 0 ||
            snag_json_integer_u64(ref, "stdin_pending", &out->stdin_pending) < 0 ||
            snag_json_integer_u64(ref, "log_start", &out->log_start) < 0 ||
            snag_json_integer_u64(ref, "log_end", &out->log_end) < 0) {
            return -1;
        }
        out->stdin_open = json_is_true(json_object_get(ref, "stdin_open"));
    }
    size_t exit_start = scratch->len;
    if (snag_binary_result_value_encode(scratch, json_object_get(data, "exit_code")) < 0) {
        return -1;
    }
    size_t signal_start = scratch->len;
    if (snag_binary_result_value_encode(scratch, json_object_get(data, "signal")) < 0) {
        return -1;
    }
    size_t content_start = scratch->len;
    const json_t *content = json_object_get(data, "content");
    if (content && read_content(content, scratch) < 0) return -1;
    /* Neither native value may retain a view from before the last append. */
    if (snag_binary_result_value_decode(scratch->data + exit_start, signal_start - exit_start,
        &result->exit_code) < 0 ||
        snag_binary_result_value_decode(scratch->data + signal_start, content_start - signal_start,
            &result->signal) < 0) {
        return -1;
    }
    if (content) {
        result->content = (struct snag_binary_content){
            scratch->data + content_start, scratch->len - content_start
        };
    }
    return 0;
}

static int
read_result_event(const json_t *data, struct snag_binary_event *event, struct snag_buf *scratch)
{
    if (event->kind == SNAG_BINARY_TOOL_FINISHED) {
        struct snag_binary_tool_finish *value = &event->data.tool_finished;
        if (!snag_json_exact_keys(data, "call_id result turn_id")) {
            return invalid();
        }
        if (read_id(data, "turn_id", value->turn) < 0 ||
            read_id(data, "call_id", value->call) < 0) {
            return -1;
        }
        return read_tool_result(json_object_get(data, "result"), &value->result, scratch);
    }
    struct snag_binary_process_close *value = &event->data.process_closed;
    if (!snag_json_exact_keys(data, "cause handle result turn_id")) {
        return invalid();
    }
    int cause = read_name(data, "cause", process_causes, 7u);
    if (cause < 0 || read_id(data, "turn_id", value->turn) < 0 ||
        read_id(data, "handle", value->handle) < 0) {
        return -1;
    }
    value->cause = (enum snag_binary_process_cause)cause;
    return read_tool_result(json_object_get(data, "result"), &value->result, scratch);
}

static int
read_tool_process(const json_t *data, struct snag_binary_event *event, struct snag_buf *scratch)
{
    if (event->kind == SNAG_BINARY_TOOL_STARTED) {
        struct snag_binary_tool_start *value = &event->data.tool_started;
        if (!snag_json_exact_keys(data, "action_sha256 call_id resolved_workdir turn_id") ||
            read_id(data, "turn_id", value->turn) < 0 ||
            read_id(data, "call_id", value->call) < 0 ||
            read_digest(data, "action_sha256", value->action_sha256) < 0 ||
            read_text(data, "resolved_workdir", &value->cwd) < 0) {
            return invalid();
        }
        return 0;
    }
    struct snag_binary_process_output *value = &event->data.process_output;
    int encoding = read_name(data, "encoding", byte_encodings, 3u);
    uint64_t stream;
    if (encoding < 0 || read_id(data, "turn_id", value->turn) < 0 ||
        read_id(data, "handle", value->handle) < 0 ||
        snag_json_integer_u64(data, "offset", &value->offset) < 0 ||
        snag_json_integer_u64(data, "stream", &stream) < 0 || stream > 1u) {
        return invalid();
    }
    value->encoding = (enum snag_binary_byte_encoding)encoding;
    value->stream = (unsigned)stream;
    scratch->max = SNAG_BINARY_PROCESS_CHUNK_MAX;
    if (snag_process_output_decode(data, scratch) < 0) return -1;
    value->data = scratch->data;
    value->size = scratch->len;
    return 0;
}

static int
turn_execution_valid(const struct snag_binary_turn_config *config)
{
    uint64_t numbers[] = {4u, 10000u, 60000u, 0u, 86400000u,
        SNAG_DEFAULT_TOOL_OUTPUT_TOKENS, 1024u * 1024u, 5u};
    for (size_t i = 0u; i < SNAG_BINARY_TURN_NUMBER_COUNT; ++i) {
        if (!(config->present & (1u << i))) continue;
        numbers[i] = config->numbers[i];
        if (i == SNAG_BINARY_TURN_MAX_PARALLEL) {
            if (!numbers[i] || numbers[i] > INT64_MAX) return invalid();
        } else if (numbers[i] > UINT32_MAX) {
            return invalid();
        }
    }
    if (!numbers[SNAG_BINARY_TURN_MAX_WAIT] || !numbers[SNAG_BINARY_TURN_MAX_TIMEOUT] ||
        !numbers[SNAG_BINARY_TURN_TOOL_OUTPUT] ||
        numbers[SNAG_BINARY_TURN_DEFAULT_YIELD] > numbers[SNAG_BINARY_TURN_MAX_WAIT] ||
        numbers[SNAG_BINARY_TURN_DEFAULT_TIMEOUT] > numbers[SNAG_BINARY_TURN_MAX_TIMEOUT] ||
        numbers[SNAG_BINARY_TURN_OUTPUT_CACHE] > SNAG_CONFIG_OUTPUT_CACHE_MAX) {
        return invalid();
    }
    return 0;
}

static int
read_turn_config(const json_t *data, struct snag_binary_turn_config *config,
    struct snag_buf *scratch)
{
    if (!json_is_object(data) || read_text(data, "provider", &config->selection.provider) < 0 ||
        read_text(data, "model", &config->selection.model) < 0 ||
        read_text(data, "effort", &config->selection.effort) < 0) {
        return invalid();
    }
    json_t *extra = json_copy((json_t *)data);
    if (!extra) return -1;
    json_object_del(extra, "provider");
    json_object_del(extra, "model");
    json_object_del(extra, "effort");
    json_object_del(extra, "parallel_tool_calls");
    int rc = -1;
    for (size_t i = 0u; i < SNAG_BINARY_TURN_NUMBER_COUNT; ++i) {
        if (!json_object_get(data, turn_numbers[i])) continue;
        if (snag_json_integer_u64(data, turn_numbers[i], &config->numbers[i]) < 0) {
            goto done;
        }
        config->present |= (uint16_t)(1u << i);
        json_object_del(extra, turn_numbers[i]);
    }
    if (turn_execution_valid(config) < 0) goto done;
    const json_t *parallel = json_object_get(data, "parallel_tool_calls");
    if (parallel) {
        if (!json_is_boolean(parallel)) {
            (void)invalid();
            goto done;
        }
        config->present |= SNAG_BINARY_TURN_PARALLEL_CALLS;
        config->parallel_calls = json_is_true(parallel);
    }
    size_t offsets[SNAG_BINARY_TURN_VALUE_COUNT + 2u] = {scratch->len};
    for (size_t i = 0u; i < SNAG_BINARY_TURN_VALUE_COUNT; ++i) {
        const json_t *value = json_object_get(data, turn_values[i]);
        if (value) {
            config->present |= (uint16_t)(1u << (8u + i));
            if (snag_binary_result_value_encode(scratch, value) < 0) goto done;
            json_object_del(extra, turn_values[i]);
        }
        offsets[i + 1u] = scratch->len;
    }
    if (snag_binary_rule_value_encode(scratch, extra) < 0) goto done;
    offsets[SNAG_BINARY_TURN_VALUE_COUNT + 1u] = scratch->len;
    /* This is the final scratch growth for a turn; earlier views bind afterward. */
    for (size_t i = 0u; i <= SNAG_BINARY_TURN_VALUE_COUNT; ++i) {
        if (offsets[i] == offsets[i + 1u]) continue;
        const void *bytes = scratch->data + offsets[i];
        size_t size = offsets[i + 1u] - offsets[i];
        int decoded = i == SNAG_BINARY_TURN_VALUE_COUNT ?
            snag_binary_rule_value_decode(bytes, size, &config->extensions) :
            snag_binary_result_value_decode(bytes, size, &config->values[i]);
        if (decoded < 0) goto done;
    }
    rc = 0;
 done:
    json_decref(extra);
    return rc;
}

static int
read_turn_start(const json_t *data, struct snag_binary_turn_start *turn, struct snag_buf *scratch)
{
    if (!snag_json_arg_keys(data, "config input_kind instructions queue_id queue_seq "
        "text turn_id turn_number", "cwd workspace read_only received_at_ms content", NULL, 0u) ||
        (json_object_get(data, "cwd") && json_object_get(data, "workspace"))) {
        return invalid();
    }
    int origin = read_name(data, "input_kind", turn_origins, 4u);
    if (origin < 0) return -1;
    turn->origin = (enum snag_binary_turn_origin)origin;
    turn->workspace = json_object_get(data, "workspace") != NULL;
    turn->has_read_only = json_object_get(data, "read_only") != NULL;
    turn->read_only = json_is_true(json_object_get(data, "read_only"));
    turn->has_received_ms = json_object_get(data, "received_at_ms") != NULL;
    if (read_id(data, "turn_id", turn->id) < 0 ||
        snag_json_integer_u64(data, "turn_number", &turn->number) < 0 ||
        read_text(data, "text", &turn->text) < 0 ||
        read_text(data, turn->workspace ? "workspace" : "cwd", &turn->cwd) < 0 ||
        (turn->has_read_only && !json_is_boolean(json_object_get(data, "read_only"))) ||
        (turn->has_received_ms &&
         snag_json_integer_u64(data, "received_at_ms", &turn->received_ms) < 0)) {
        return invalid();
    }
    if (turn->origin == SNAG_BINARY_TURN_QUEUED) {
        if (read_id(data, "queue_id", turn->queue_id) < 0 ||
            snag_json_integer_u64(data, "queue_seq", &turn->queue_seq) < 0) {
            return -1;
        }
    } else if (!json_is_null(json_object_get(data, "queue_id")) ||
               !json_is_null(json_object_get(data, "queue_seq"))) {
        return invalid();
    }
    const json_t *content = json_object_get(data, "content");
    if (turn->origin == SNAG_BINARY_TURN_GOAL &&
        (content || turn->read_only ||
         strcmp((const char *)turn->text.data, SNAG_GOAL_CONTINUATION_TEXT))) {
        return invalid();
    }
    size_t instruction_start = scratch->len;
    if (read_instruction_paths(json_object_get(data, "instructions"), scratch, true) < 0) {
        return -1;
    }
    size_t content_start = scratch->len;
    if (content && read_content(content, scratch) < 0) return -1;
    size_t config_start = scratch->len;
    if (read_turn_config(json_object_get(data, "config"), &turn->config, scratch) < 0) {
        return -1;
    }
    turn->instructions = (struct snag_binary_instructions){
        scratch->data + instruction_start, content_start - instruction_start
    };
    if (content) {
        turn->content = (struct snag_binary_content){
            scratch->data + content_start, config_start - content_start
        };
    }
    return 0;
}

static int
read_voice_source(const json_t *data, struct snag_binary_voice_source *voice)
{
    if (!snag_json_exact_keys(data,
        "connection_id input_id response_id call_id provider model transcript request")) {
        return invalid();
    }
    if (read_id(data, "connection_id", voice->connection_id) < 0 ||
        read_text(data, "input_id", &voice->input_id) < 0 ||
        read_text(data, "response_id", &voice->response_id) < 0 ||
        read_text(data, "call_id", &voice->call_id) < 0 ||
        read_text(data, "provider", &voice->provider) < 0 ||
        read_text(data, "model", &voice->model) < 0 ||
        read_text(data, "transcript", &voice->transcript) < 0 ||
        read_text(data, "request", &voice->request) < 0) {
        return -1;
    }
    return 0;
}

static int
read_queued_input(const json_t *data, struct snag_binary_event *event, struct snag_buf *scratch)
{
    bool adding = event->kind == SNAG_BINARY_FUTURE_TURN_QUEUED;
    const char *required = adding ? "queue_id read_only text while_turn_id" :
        "queue_id read_only text";
    const char *optional = adding ? "armed content received_at_ms voice" :
        "armed content received_at_ms";
    if (!snag_json_arg_keys(data, required, optional, NULL, 0u) ||
        !json_is_boolean(json_object_get(data, "read_only"))) {
        return invalid();
    }
    const json_t *armed = json_object_get(data, "armed");
    const json_t *received = json_object_get(data, "received_at_ms");
    const json_t *voice = json_object_get(data, "voice");
    const json_t *content = json_object_get(data, "content");
    if (armed && !json_is_boolean(armed)) return invalid();
    event->data.queued.read_only = json_is_true(json_object_get(data, "read_only"));
    event->data.queued.has_armed = armed != NULL;
    event->data.queued.armed = json_is_true(armed);
    event->data.queued.has_received_ms = received != NULL;
    event->data.queued.has_voice = voice != NULL;
    if (read_id(data, "queue_id", event->data.queued.id) < 0 ||
        read_text(data, "text", &event->data.queued.text) < 0 ||
        (received && snag_json_integer_u64(data, "received_at_ms",
            &event->data.queued.received_ms) < 0)) {
        return -1;
    }
    if (voice) {
        if (!adding || event->data.queued.read_only || content) {
            return invalid();
        }
        if (read_voice_source(voice, &event->data.queued.voice) < 0 ||
            queued_voice_id_valid(data) < 0) {
            return -1;
        }
    }
    if (adding) {
        const json_t *value = json_object_get(data, "while_turn_id");
        if (json_is_null(value)) {
            event->data.queued.while_kind = SNAG_BINARY_WHILE_NULL;
        } else if (json_is_string(value) && !json_string_length(value)) {
            event->data.queued.while_kind = SNAG_BINARY_WHILE_EMPTY;
        } else {
            event->data.queued.while_kind = SNAG_BINARY_WHILE_ID;
            if (read_id(data, "while_turn_id", event->data.queued.while_id) < 0) {
                return -1;
            }
        }
    }
    if (content) {
        size_t start = scratch->len;
        if (read_content(content, scratch) < 0) return -1;
        event->data.queued.content = (struct snag_binary_content){
            scratch->data + start, scratch->len - start
        };
    }
    return 0;
}

static int
read_input_control(const json_t *data, struct snag_binary_event *event, struct snag_buf *scratch)
{
    switch (event->kind) {
    case SNAG_BINARY_INPUT_CANCELLED:
        return snag_json_exact_keys(data, "") ? 0 : invalid();
    case SNAG_BINARY_STEERING_DEFERRED:
        if (!snag_json_exact_keys(data, "turn_id")) return invalid();
        return read_id(data, "turn_id", event->data.turn);
    case SNAG_BINARY_INPUT_ADMITTED:
        if (!snag_json_exact_keys(data, "steering_ids time_ms turn_id")) {
            return invalid();
        }
        if (read_id(data, "turn_id", event->data.admission.turn) < 0 ||
            snag_json_integer_u64(data, "time_ms", &event->data.admission.time_ms) < 0) {
            return -1;
        }
        return read_ids(data, "steering_ids", &event->data.admission.ids, scratch);
    case SNAG_BINARY_FUTURE_QUEUE_STATE:
        if (!snag_json_exact_keys(data, "armed") ||
            !json_is_boolean(json_object_get(data, "armed"))) {
            return invalid();
        }
        event->data.queue_armed = json_is_true(json_object_get(data, "armed"));
        return 0;
    case SNAG_BINARY_FUTURE_TURN_CANCELLED: {
        const char *reason = snag_json_string(data, "reason");
        if (!snag_json_exact_keys(data, "queue_ids reason") || !reason || strcmp(reason, "user")) {
            return invalid();
        }
        event->data.queue_cancel.actor = SNAG_BINARY_USER;
        return read_ids(data, "queue_ids", &event->data.queue_cancel.ids, scratch);
    }
    default: return unsupported();
    }
}

static int
read_turn_outcome(const json_t *data, struct snag_binary_event *event)
{
    switch (event->kind) {
    case SNAG_BINARY_TURN_YIELD_REQUESTED:
    case SNAG_BINARY_TURN_CANCEL_REQUESTED:
        if (!snag_json_exact_keys(data, "turn_id")) return invalid();
        return read_id(data, "turn_id", event->data.turn);
    case SNAG_BINARY_TURN_COMPLETED:
        if (!snag_json_exact_keys(data, "final_item_id final_response_id turn_id")) {
            return invalid();
        }
        if (read_id(data, "turn_id", event->data.completed.turn) < 0 ||
            read_id(data, "final_response_id", event->data.completed.response) < 0 ||
            read_id(data, "final_item_id", event->data.completed.item) < 0) {
            return -1;
        }
        return 0;
    case SNAG_BINARY_TURN_COMPLETED_SILENT: {
        if (!snag_json_exact_keys(data, "reason response_id turn_id")) {
            return invalid();
        }
        int reason = read_name(data, "reason", quiet_reasons,
            sizeof(quiet_reasons) / sizeof(quiet_reasons[0]));
        if (reason < 0) return -1;
        event->data.silent.reason = (enum snag_binary_quiet_reason)reason;
        if (read_id(data, "turn_id", event->data.silent.turn) < 0) return -1;
        return read_id(data, "response_id", event->data.silent.response);
    }
    case SNAG_BINARY_TURN_INTERRUPTED: {
        if (!snag_json_exact_keys(data, "origin reason turn_id")) {
            return invalid();
        }
        int origin = read_name(data, "origin", interrupt_origins,
            sizeof(interrupt_origins) / sizeof(interrupt_origins[0]));
        int reason = read_name(data, "reason", interrupt_reasons,
            sizeof(interrupt_reasons) / sizeof(interrupt_reasons[0]));
        if (origin < 0 || reason < 0) return -1;
        event->data.interrupted.origin = (enum snag_binary_interrupt_origin)origin;
        event->data.interrupted.reason = (enum snag_binary_interrupt_reason)reason;
        return read_id(data, "turn_id", event->data.interrupted.turn);
    }
    case SNAG_BINARY_TURN_FAILED: {
        if (!snag_json_exact_keys(data, "class message turn_id")) {
            return invalid();
        }
        int class_id = read_name(data, "class", failure_classes,
            sizeof(failure_classes) / sizeof(failure_classes[0]));
        if (class_id < 0) return -1;
        event->data.failed.class_id = (enum snag_binary_failure_class)class_id;
        if (read_id(data, "turn_id", event->data.failed.turn) < 0) return -1;
        return read_text(data, "message", &event->data.failed.message);
    }
    case SNAG_BINARY_TURN_RECOVERY:
        if (!snag_json_arg_keys(data, "class message turn_id", "retry_attempts", NULL, 0u)) {
            return invalid();
        }
        event->data.recovery.has_retry_attempts = json_object_get(data, "retry_attempts") != NULL;
        if (read_id(data, "turn_id", event->data.recovery.turn) < 0 ||
            read_text(data, "class", &event->data.recovery.class_name) < 0 ||
            read_text(data, "message", &event->data.recovery.message) < 0 ||
            (event->data.recovery.has_retry_attempts &&
             snag_json_integer_u64(data, "retry_attempts",
                 &event->data.recovery.retry_attempts) < 0)) {
            return -1;
        }
        return 0;
    default: return unsupported();
    }
}

static int
read_count_method(const json_t *data, const char *key, enum snag_binary_count_method *out)
{
    int method = read_name(data, key, count_methods,
        sizeof(count_methods) / sizeof(count_methods[0]));
    if (method < 0) return -1;
    *out = (enum snag_binary_count_method)method;
    return 0;
}

static int
read_compact(const json_t *data, struct snag_binary_event *event, struct snag_buf *scratch)
{
    if (event->kind == SNAG_BINARY_COMPACTION_STARTED) {
        if (!snag_json_arg_keys(data,
            "capability_version compact_id count_method count_request_sha256 input_tokens_bound "
            "model predecessor_compact_id profile_id reason request_sha256 source_seq "
            "source_sha256",
            "continuation_scope compaction_model", NULL, 0u)) {
            return invalid();
        }
        struct snag_binary_compact_start *value = &event->data.compaction_started;
        int reason = read_name(data, "reason", compact_reasons,
            sizeof(compact_reasons) / sizeof(compact_reasons[0]));
        if (reason < 0) return -1;
        value->reason = (enum snag_binary_compact_reason)reason;
        value->has_predecessor = !json_is_null(json_object_get(data, "predecessor_compact_id"));
        value->has_scope = json_object_get(data, "continuation_scope") != NULL;
        value->has_compaction_model = json_object_get(data, "compaction_model") != NULL;
        if (read_id(data, "compact_id", value->id) < 0 ||
            read_count_method(data, "count_method", &value->count_method) < 0 ||
            snag_json_integer_u64(data, "source_seq", &value->source_seq) < 0 ||
            snag_json_integer_u64(data, "input_tokens_bound", &value->input_tokens) < 0 ||
            read_digest(data, "source_sha256", value->source_sha256) < 0 ||
            read_digest(data, "request_sha256", value->request_sha256) < 0 ||
            read_digest(data, "count_request_sha256", value->count_request_sha256) < 0 ||
            read_text(data, "model", &value->model) < 0 ||
            read_text(data, "capability_version", &value->capability) < 0 ||
            read_text(data, "profile_id", &value->profile) < 0 ||
            (value->has_predecessor &&
             read_id(data, "predecessor_compact_id", value->predecessor) < 0) ||
            (value->has_scope && read_digest(data, "continuation_scope", value->scope) < 0) ||
            (value->has_compaction_model &&
             read_text(data, "compaction_model", &value->compaction_model) < 0)) {
            return -1;
        }
        return 0;
    }
    if (event->kind == SNAG_BINARY_COMPACTION_INTERRUPTED) {
        if (!snag_json_exact_keys(data, "compact_id reason")) return invalid();
        struct snag_binary_compact_interrupt *value = &event->data.compaction_interrupted;
        int reason = read_name(data, "reason", compact_stop_reasons,
            sizeof(compact_stop_reasons) / sizeof(compact_stop_reasons[0]));
        if (reason < 0) return -1;
        value->reason = (enum snag_binary_compact_stop_reason)reason;
        return read_id(data, "compact_id", value->id);
    }
    if (!snag_json_arg_keys(data,
        "compact_id count_method input_tokens_bound output output_count_method "
        "output_count_request_sha256 output_sha256 output_tokens_bound source_sha256",
        "continuation_scope", NULL, 0u)) {
        return invalid();
    }
    struct snag_binary_compact_complete *value = &event->data.compaction_completed;
    value->has_scope = json_object_get(data, "continuation_scope") != NULL;
    if (read_id(data, "compact_id", value->id) < 0 ||
        read_count_method(data, "count_method", &value->count_method) < 0 ||
        read_count_method(data, "output_count_method", &value->output_count_method) < 0 ||
        snag_json_integer_u64(data, "input_tokens_bound", &value->input_tokens) < 0 ||
        snag_json_integer_u64(data, "output_tokens_bound", &value->output_tokens) < 0 ||
        read_digest(data, "output_count_request_sha256", value->output_count_request_sha256) < 0 ||
        read_digest(data, "source_sha256", value->source_sha256) < 0 ||
        read_digest(data, "output_sha256", value->output_sha256) < 0 ||
        (value->has_scope && read_digest(data, "continuation_scope", value->scope) < 0)) {
        return -1;
    }
    scratch->max = 12u * 1024u * 1024u;
    if (snag_json_canonical(json_object_get(data, "output"), scratch) < 0) {
        return -1;
    }
    value->output = (struct snag_binary_text){scratch->data, scratch->len};
    return 0;
}

int
snag_binary_legacy_encode(struct snag_buf *out, const char *type, const json_t *data,
                          enum snag_binary_kind *kind)
{
    if (!out || !type || !data || !kind) return invalid();
    struct snag_binary_event event = {0};
    if (snag_binary_event_kind(type, &event.kind) < 0) return -1;
    if (!legacy_kind(event.kind)) return unsupported();
    if (canonical_data(data) < 0) return -1;
    struct snag_buf scratch = {.max = SNAG_MAX_EVENT_LINE};
    int rc;
    if (metadata_kind(event.kind)) rc = read_metadata(data, &event, &scratch);
    else if (timer_kind(event.kind)) rc = read_timer(data, &event);
    else if (goal_kind(event.kind)) rc = read_goal(data, &event);
    else if (control_kind(event.kind)) rc = read_control(data, &event);
    else if (download_kind(event.kind)) rc = read_download(data, &event);
    else if (context_kind(event.kind)) rc = read_context(data, &event);
    else if (rule_voice_kind(event.kind)) rc = read_rule_voice(data, &event, &scratch);
    else if (hosted_search_kind(event.kind)) rc = read_hosted_search(data, &event, &scratch);
    else if (event.kind == SNAG_BINARY_TURN_STARTED) {
        rc = read_turn_start(data, &event.data.started, &scratch);
    } else if (event.kind == SNAG_BINARY_RESPONSE_STARTED) {
        rc = read_response_start(data, &event.data.response_started, &scratch);
    } else if (event.kind == SNAG_BINARY_RESPONSE_OUTPUT) {
        rc = read_response_output(data, &event.data.response_output);
    } else if (response_end_kind(event.kind)) {
        rc = read_response_end(data, &event, &scratch);
    } else if (event.kind == SNAG_BINARY_RESPONSE_COMPLETED) {
        rc = read_response_complete(data, &event.data.response_completed, &scratch);
    } else if (event.kind == SNAG_BINARY_TOOL_STARTED || event.kind == SNAG_BINARY_PROCESS_OUTPUT) {
        rc = read_tool_process(data, &event, &scratch);
    } else if (event.kind == SNAG_BINARY_TOOL_FINISHED ||
               event.kind == SNAG_BINARY_PROCESS_CLOSED) {
        rc = read_result_event(data, &event, &scratch);
    } else if (event.kind >= SNAG_BINARY_IRC_EVENT && event.kind <= SNAG_BINARY_IRC_EVENT_V2) {
        rc = read_irc(data, &event, &scratch);
    } else if (event.kind == SNAG_BINARY_VOICE_TRANSFER_RECORD) {
        rc = read_transfer_archive(data, &event.data.voice_transfer_record, &scratch);
    } else if (event.kind >= SNAG_BINARY_VOICE_TRANSFER_SEALED &&
               event.kind <= SNAG_BINARY_VOICE_TRANSFER_ADOPTED) {
        rc = read_transfer_metadata(data, &event);
    } else if (input_receipt_kind(event.kind)) {
        rc = read_input_receipt(data, &event, &scratch);
    } else if (queued_input_kind(event.kind)) {
        rc = read_queued_input(data, &event, &scratch);
    } else if (event.kind >= SNAG_BINARY_INPUT_RECEIVED &&
             event.kind <= SNAG_BINARY_FUTURE_TURN_EDITED) {
        rc = read_input_control(data, &event, &scratch);
    } else if (event.kind >= SNAG_BINARY_COMPACTION_STARTED &&
             event.kind <= SNAG_BINARY_COMPACTION_COMPLETED) {
        rc = read_compact(data, &event, &scratch);
    } else {
        rc = read_turn_outcome(data, &event);
    }
    if (rc == 0) rc = snag_binary_event_encode(out, &event);
    snag_buf_free(&scratch);
    if (rc < 0) return -1;
    *kind = event.kind;
    return 0;
}

static int
put_text(json_t *data, const char *key, struct snag_binary_text text)
{
    return snag_json_set_new(data, key, json_stringn((const char *)text.data, text.size));
}

static void
hex_text(char *text, const unsigned char *id, size_t size)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0u; i < size; ++i) {
        text[2u * i] = digits[id[i] >> 4u];
        text[2u * i + 1u] = digits[id[i] & 15u];
    }
    text[2u * size] = '\0';
}

static int
put_id(json_t *data, const char *key, const unsigned char id[16])
{
    char text[33];
    hex_text(text, id, 16u);
    return snag_json_set_new(data, key, json_string(text));
}

static int
put_digest(json_t *data, const char *key, const unsigned char digest[32])
{
    char text[65];
    hex_text(text, digest, 32u);
    return snag_json_set_new(data, key, json_string(text));
}

static int
put_created(json_t *data, const struct snag_binary_event *event)
{
    const struct snag_binary_selection *selection = &event->data.created.selection;
    const char *cwd = event->data.created.source_format == 2u ? "workspace" : "cwd";
    if (snag_json_set_new(data, "format", json_integer(event->data.created.source_format)) < 0 ||
        snag_json_set_new(data, "protocol", json_string("responses")) < 0 ||
        put_text(data, "default_provider", selection->provider) < 0 ||
        put_text(data, "default_model", selection->model) < 0 ||
        put_text(data, "default_effort", selection->effort) < 0 ||
        put_text(data, cwd, event->data.created.cwd) < 0) {
        return -1;
    }
    return 0;
}

static int
put_metadata(json_t *data, const struct snag_binary_event *event)
{
    switch (event->kind) {
    case SNAG_BINARY_SESSION_CREATED:
        return put_created(data, event);
    case SNAG_BINARY_CWD_CHANGED:
        if (put_text(data, "old_cwd", event->data.cwd.before) < 0 ||
            put_text(data, "new_cwd", event->data.cwd.after) < 0) {
            return -1;
        }
        return 0;
    case SNAG_BINARY_SESSION_ARCHIVED:
    case SNAG_BINARY_SESSION_UNARCHIVED:
        return snag_json_set_new(data, "origin", json_string("user"));
    case SNAG_BINARY_SESSION_DELETE_REQUESTED: {
        char prefix[9];
        char trash[66];
        hex_text(prefix, event->data.deletion.confirmed_prefix, 4u);
        hex_text(trash, event->data.deletion.session, 16u);
        trash[32] = '.';
        hex_text(trash + 33u, event->data.deletion.nonce, 16u);
        if (snag_json_set_new(data, "confirmed_id_prefix", json_string(prefix)) < 0 ||
            snag_json_set_new(data, "trash_name", json_string(trash)) < 0) {
            return -1;
        }
        return 0;
    }
    case SNAG_BINARY_BANNER_UPDATED:
        return put_text(data, "text", event->data.banner);
    case SNAG_BINARY_STEERING_UPDATED:
        return snag_json_set_new(data, "mode", json_string(steering_modes[event->data.steering]));
    case SNAG_BINARY_MODEL_SELECTED: {
        const struct snag_binary_selection *before = &event->data.model.before;
        const struct snag_binary_selection *after = &event->data.model.after;
        if (put_text(data, "old_provider", before->provider) < 0 ||
            put_text(data, "old_model", before->model) < 0 ||
            put_text(data, "old_effort", before->effort) < 0 ||
            put_text(data, "new_provider", after->provider) < 0 ||
            put_text(data, "new_model", after->model) < 0 ||
            put_text(data, "new_effort", after->effort) < 0) {
            return -1;
        }
        return 0;
    }
    case SNAG_BINARY_TURN_MODEL_CHANGED: {
        const struct snag_binary_selection *before = &event->data.turn_model.before;
        if (put_id(data, "turn_id", event->data.turn_model.id) < 0 ||
            put_text(data, "old_provider", before->provider) < 0 ||
            put_text(data, "old_model", before->model) < 0 ||
            put_text(data, "old_effort", before->effort) < 0 ||
            put_text(data, "new_effort", event->data.turn_model.effort) < 0) {
            return -1;
        }
        return 0;
    }
    case SNAG_BINARY_EFFORT_CHANGED:
        if (put_text(data, "old_effort", event->data.effort.before) < 0 ||
            put_text(data, "new_effort", event->data.effort.after) < 0) {
            return -1;
        }
        return 0;
    case SNAG_BINARY_CONTEXT_SELECTION_CHANGED: {
        const struct snag_binary_context_choice *before = &event->data.context.before;
        const struct snag_binary_context_choice *after = &event->data.context.after;
        if (snag_json_set_new(data, "old_mode", json_string(context_modes[before->mode])) < 0 ||
            snag_json_set_new(data, "new_mode", json_string(context_modes[after->mode])) < 0 ||
            snag_json_set_new(data, "old_tokens", json_integer((json_int_t)before->tokens)) < 0 ||
            snag_json_set_new(data, "new_tokens", json_integer((json_int_t)after->tokens)) < 0) {
            return -1;
        }
        return 0;
    }
    case SNAG_BINARY_COMMAND_SHELL_CHANGED:
        return put_text(data, "shell", event->data.shell);
    case SNAG_BINARY_SESSION_NAMED:
        return put_text(data, "name", event->data.name);
    case SNAG_BINARY_SERVICE_TIER_CHANGED:
        return put_text(data, "value", event->data.service_tier);
    case SNAG_BINARY_SESSION_OPTIONS: {
        json_t *args = json_array();
        if (!args) return -1;
        size_t offset = 0u;
        struct snag_binary_text text;
        int rc;
        while ((rc = snag_binary_options_next(&event->data.options, &offset, &text)) == 0) {
            if (json_array_append_new(args,
                json_stringn((const char *)text.data, text.size)) < 0) {
                rc = -1;
                break;
            }
        }
        if (rc < 0) {
            json_decref(args);
            return -1;
        }
        return snag_json_set_new(data, "args", args);
    }
    default: return unsupported();
    }
}

static int
put_timer(json_t *data, const struct snag_binary_event *event)
{
    if (put_id(data, "timer_id", event->data.timer.id) < 0) return -1;
    if (event->kind != SNAG_BINARY_TIMER_SCHEDULED) return 0;
    if (event->data.timer.due_ms > INT64_MAX) return invalid();
    if (snag_json_set_new(data, "due_ms", json_integer((json_int_t)event->data.timer.due_ms)) < 0 ||
        put_text(data, "text", event->data.timer.text) < 0) {
        return -1;
    }
    return 0;
}

static int
put_goal(json_t *data, const struct snag_binary_event *event)
{
    if (put_id(data, "goal_id", event->data.goal.id) < 0) return -1;
    if (event->kind == SNAG_BINARY_GOAL_REPLACED &&
        put_id(data, "new_goal_id", event->data.goal.replacement) < 0) {
        return -1;
    }
    if (event->kind == SNAG_BINARY_GOAL_REPLACED || event->kind == SNAG_BINARY_GOAL_REWORDED ||
        event->kind == SNAG_BINARY_GOAL_BLOCKED || event->kind == SNAG_BINARY_GOAL_COMPLETED) {
        if (snag_json_set_new(data, "actor", json_string(actors[event->data.goal.actor])) < 0) {
            return -1;
        }
    }
    switch (event->kind) {
    case SNAG_BINARY_GOAL_STARTED:
    case SNAG_BINARY_GOAL_REPLACED:
    case SNAG_BINARY_GOAL_REWORDED: return put_text(data, "prompt", event->data.goal.text);
    case SNAG_BINARY_GOAL_BLOCKED: return put_text(data, "reason", event->data.goal.text);
    case SNAG_BINARY_GOAL_LOCK_CHANGED:
        return snag_json_set_new(data, "locked", json_boolean(event->data.goal.locked));
    case SNAG_BINARY_GOAL_PAUSED:
        return snag_json_set_new(data, "reason", json_string(pauses[event->data.goal.pause]));
    default: return 0;
    }
}

static int
put_control(json_t *data, const struct snag_binary_event *event)
{
    const struct snag_binary_control *value = &event->data.control;
    if (snag_json_set_new(data, "control", json_integer(value->control)) < 0) {
        return -1;
    }
    if (value->image_boundary) {
        if (snag_json_set_new(data, "origin", json_string("image_boundary")) < 0 ||
            snag_json_set_new(data, "source_seq",
                json_integer((json_int_t)value->source_seq)) < 0) {
            return -1;
        }
    }
    return 0;
}

static int
put_download(json_t *data, const struct snag_binary_event *event)
{
    switch (event->kind) {
    case SNAG_BINARY_DOWNLOAD_QUEUED: {
        const struct snag_binary_download *value = &event->data.download_queued;
        if (put_id(data, "id", value->id) < 0 ||
            put_digest(data, "sha256", value->sha256) < 0 ||
            put_text(data, "path", value->path) < 0 ||
            put_text(data, "name", value->name) < 0 ||
            snag_json_set_new(data, "bytes", json_integer((json_int_t)value->bytes)) < 0 ||
            snag_json_set_new(data, "mtime", json_integer((json_int_t)value->mtime)) < 0 ||
            snag_json_set_new(data, "queued_ms", json_integer((json_int_t)value->queued_ms)) < 0) {
            return -1;
        }
        return 0;
    }
    case SNAG_BINARY_DOWNLOAD_REMOVED:
        if (put_id(data, "id", event->data.download_removed.id) < 0) return -1;
        return put_text(data, "reason", event->data.download_removed.reason);
    case SNAG_BINARY_DOWNLOADS_CLEARED:
        return put_text(data, "reason", event->data.downloads_cleared);
    default: return unsupported();
    }
}

static int
put_limit(json_t *data, const char *key, uint64_t value)
{
    if (value > INT64_MAX) return invalid();
    return snag_json_set_new(data, key, value ? json_integer((json_int_t)value) : json_null());
}

static int
put_context(json_t *data, const struct snag_binary_event *event)
{
    if (event->kind == SNAG_BINARY_CONTEXT_REBASED) {
        if (put_id(data, "turn_id", event->data.context_rebased.turn) < 0) {
            return -1;
        }
        return snag_json_set_new(data, "reason",
            json_string(rebase_reasons[event->data.context_rebased.reason]));
    }
    const struct snag_binary_capacity_rejection *value = &event->data.capacity_rejected;
    if (put_id(data, "turn_id", value->turn) < 0 ||
        put_id(data, "response_id", value->response) < 0 ||
        snag_json_set_new(data, "cycle", json_integer(value->cycle)) < 0 ||
        snag_json_set_new(data, "code", json_string("context_length_exceeded")) < 0 ||
        put_text(data, "message", value->message) < 0 ||
        put_digest(data, "provider_source_sha256", value->provider_source_sha256) < 0 ||
        put_digest(data, "request_sha256", value->request_sha256) < 0 ||
        put_limit(data, "context_limit_tokens", value->context_limit) < 0 ||
        put_limit(data, "requested_input_tokens", value->requested_input) < 0 ||
        put_limit(data, "observed_hard_input_tokens", value->observed_input) < 0) {
        return -1;
    }
    return 0;
}

static int
put_hosted_search(json_t *data, const struct snag_binary_event *event)
{
    bool started = event->kind == SNAG_BINARY_HOSTED_SEARCH_STARTED;
    const struct snag_binary_hosted_search *value = &event->data.hosted_search;
    if (put_id(data, "turn_id", value->turn) < 0 || put_text(data, "item_id", value->item_id) < 0 ||
        (!started && put_text(data, "status", value->status) < 0)) return -1;
    if (!value->has_detail) return 0;
    json_t *detail = NULL;
    if (snag_binary_rule_value_json(&value->detail, &detail) < 0) return -1;
    return snag_json_set_new(data, started ? "action" : "sources", detail);
}

static int
put_rule_voice(json_t *data, const struct snag_binary_event *event)
{
    switch (event->kind) {
    case SNAG_BINARY_RULE_LOG: {
        static const char *const names[] = {"chain", "message", "rule"};
        const struct snag_binary_result_value *fields[] = {
            &event->data.rule_log.chain, &event->data.rule_log.message, &event->data.rule_log.rule
        };
        for (size_t i = 0u; i < 3u; ++i) {
            json_t *value = NULL;
            if (snag_binary_rule_value_json(fields[i], &value) < 0) return -1;
            if (snag_json_set_new(data, names[i], value) < 0) return -1;
        }
        return 0;
    }
    case SNAG_BINARY_RULE_TRANSFORM: {
        const struct snag_binary_rule_transform *value = &event->data.rule_transform;
        if (put_id(data, "call_id", value->call) < 0 ||
            put_digest(data, "original_sha256", value->original_sha256) < 0 ||
            put_digest(data, "effective_sha256", value->effective_sha256) < 0 ||
            put_text(data, "rule", value->rule) < 0) {
            return -1;
        }
        return 0;
    }
    case SNAG_BINARY_AUDIO_USAGE: {
        const struct snag_binary_audio_usage *value = &event->data.audio_usage;
        if (snag_json_set_new(data, "operation", json_string("dictation")) < 0 ||
            put_text(data, "provider", value->provider) < 0 ||
            put_text(data, "model", value->model) < 0 ||
            put_text(data, "report", value->report) < 0) {
            return -1;
        }
        return 0;
    }
    case SNAG_BINARY_VOICE_EVENT: {
        const struct snag_binary_voice_event *value = &event->data.voice_event;
        if (put_id(data, "connection_id", value->connection) < 0 ||
            put_text(data, "provider", value->provider) < 0 ||
            put_text(data, "model", value->model) < 0) {
            return -1;
        }
        json_t *body = NULL;
        if (snag_binary_voice_body_json(&value->body, &body) < 0) return -1;
        return snag_json_set_new(data, "event", body);
    }
    default: return unsupported();
    }
}

static int
put_ids(json_t *data, const char *key, struct snag_binary_ids ids)
{
    json_t *array = json_array();
    if (!array) {
        errno = ENOMEM;
        return -1;
    }
    for (size_t i = 0u; i < ids.count; ++i) {
        char id[33];
        hex_text(id, ids.values[i], 16u);
        if (json_array_append_new(array, json_string(id)) < 0) {
            json_decref(array);
            return -1;
        }
    }
    return snag_json_set_new(data, key, array);
}

int
snag_binary_instructions_legacy(const struct snag_binary_instructions *paths, bool snapshots,
    json_t **out)
{
    if (!paths || !out) return invalid();
    json_t *array = json_array();
    if (!array) return -1;
    size_t offset = 0u;
    struct snag_binary_instruction item;
    int rc;
    while ((rc = snag_binary_instructions_next(paths, &offset, &item)) == 0) {
        json_t *value;
        if (item.has_snapshot) {
            if (!snapshots || item.bytes > INT64_MAX) {
                (void)invalid();
                goto fail;
            }
            value = json_pack("{s:I}", "bytes", (json_int_t)item.bytes);
            if (!value) goto fail;
            if (put_text(value, "path", item.path) < 0 ||
                put_digest(value, "sha256", item.sha256) < 0) {
                json_decref(value);
                goto fail;
            }
        } else {
            value = json_stringn((const char *)item.path.data, item.path.size);
        }
        if (!value || json_array_append_new(array, value) < 0) goto fail;
    }
    if (rc < 0 || instruction_paths_valid(array, snapshots) < 0) goto fail;
    *out = array;
    return 0;
 fail:
    json_decref(array);
    return -1;
}

static int
put_instruction_paths(json_t *data, const struct snag_binary_instructions *paths, bool snapshots)
{
    json_t *array = NULL;
    if (snag_binary_instructions_legacy(paths, snapshots, &array) < 0) return -1;
    return snag_json_set_new(data, "instructions", array);
}

static int
put_asset(json_t *data, const char *key, const struct snag_binary_asset *asset)
{
    if (asset->bytes > INT64_MAX) return invalid();
    json_t *value = json_object();
    if (!value) return -1;
    if (put_id(value, "id", asset->id) < 0 || put_digest(value, "sha256", asset->sha256) < 0 ||
        put_text(value, "mime_type", asset->mime) < 0 ||
        snag_json_set_new(value, "bytes", json_integer((json_int_t)asset->bytes)) < 0) {
        json_decref(value);
        return -1;
    }
    return snag_json_set_new(data, key, value);
}

static int
put_content(json_t *data, const struct snag_binary_content *content)
{
    if (!content->size) return 0;
    json_t *array = json_array();
    if (!array) return -1;
    size_t offset = 0u;
    struct snag_binary_part part;
    int rc;
    while ((rc = snag_binary_content_next(content, &offset, &part)) == 0) {
        const char *type = part.kind == SNAG_BINARY_PART_TEXT ? "input_text" :
            part.kind == SNAG_BINARY_PART_FILE ? "file" : "input_image";
        json_t *item = json_pack("{s:s}", "type", type);
        if (!item) goto fail;
        if ((part.kind == SNAG_BINARY_PART_TEXT && put_text(item, "text", part.text) < 0) ||
            (part.kind != SNAG_BINARY_PART_TEXT && put_asset(item, "asset", &part.asset) < 0) ||
            (part.has_source && (put_asset(item, "source", &part.source) < 0 ||
                                put_text(item, "note", part.text) < 0))) {
            json_decref(item);
            goto fail;
        }
        if (json_array_append_new(array, item) < 0) goto fail;
    }
    if (rc < 0) goto fail;
    if (!snag_media_content_valid(array)) {
        (void)invalid();
        goto fail;
    }
    return snag_json_set_new(data, "content", array);
 fail:
    json_decref(array);
    return -1;
}

static int
put_host_context(json_t *data, const struct snag_binary_content *host)
{
    if (!host->size) return 0;
    json_t *array = json_array();
    if (!array) return -1;
    struct snag_binary_part part;
    size_t offset = 0u;
    int rc;
    while ((rc = snag_binary_content_next(host, &offset, &part)) == 0) {
        json_t *item = json_pack("{s:s}", "role", "user");
        if (!item) goto fail;
        if (put_text(item, "content", part.text) < 0) {
            json_decref(item);
            goto fail;
        }
        if (json_array_append_new(array, item) < 0) goto fail;
    }
    if (rc < 0 || host_context_shape(array) < 0) goto fail;
    return snag_json_set_new(data, "host_context", array);
 fail:
    json_decref(array);
    return -1;
}

static int
put_response_start(json_t *data, const struct snag_binary_response_start *start)
{
    if (start->input_tokens_bound > INT64_MAX ||
        (start->has_irc_seq && start->irc_seq > INT64_MAX) ||
        (start->full_accounting && (start->model_input_bytes > INT64_MAX ||
         start->request_input_bytes > INT64_MAX || start->request_input_count > INT64_MAX ||
         (start->has_hard_input && start->hard_input_tokens > INT64_MAX)))) {
        return invalid();
    }
    if (put_id(data, "turn_id", start->turn) < 0 ||
        put_id(data, "response_id", start->response) < 0 ||
        snag_json_set_new(data, "cycle", json_integer(start->cycle)) < 0 ||
        snag_json_set_new(data, "count_method",
            json_string(count_methods[start->count_method])) < 0 ||
        snag_json_set_new(data, "input_tokens_bound",
            json_integer((json_int_t)start->input_tokens_bound)) < 0 ||
        put_digest(data, "request_sha256", start->request_sha256) < 0 ||
        put_digest(data, "count_request_sha256", start->count_request_sha256) < 0 ||
        put_digest(data, "model_input_sha256", start->model_input_sha256) < 0 ||
        put_text(data, "model", start->selection.model) < 0 ||
        put_text(data, "capability_version", start->capability) < 0 ||
        put_text(data, "profile_id", start->profile) < 0 ||
        put_ids(data, "steering_ids", start->steering) < 0) {
        return -1;
    }
    int baseline = start->has_baseline ?
        put_digest(data, "baseline_sha256", start->baseline_sha256) :
        snag_json_set_new(data, "baseline_sha256", json_null());
    if (baseline < 0) return -1;
    int compact = start->has_compact ? put_id(data, "compact_id", start->compact) :
        snag_json_set_new(data, "compact_id", json_null());
    if (compact < 0) return -1;
    if (start->full_accounting) {
        if (put_text(data, "provider", start->selection.provider) < 0 ||
            put_text(data, "effort", start->selection.effort) < 0 ||
            put_digest(data, "provider_source_sha256", start->provider_source_sha256) < 0 ||
            put_digest(data, "request_input_sha256", start->request_input_sha256) < 0 ||
            snag_json_set_new(data, "capacity_source",
                json_string(capacity_sources[start->capacity_source])) < 0 ||
            snag_json_set_new(data, "source_bound", json_boolean(start->source_bound)) < 0 ||
            snag_json_set_new(data, "model_input_bytes",
                json_integer((json_int_t)start->model_input_bytes)) < 0 ||
            snag_json_set_new(data, "request_input_bytes",
                json_integer((json_int_t)start->request_input_bytes)) < 0 ||
            snag_json_set_new(data, "request_input_count",
                json_integer((json_int_t)start->request_input_count)) < 0 ||
            snag_json_set_new(data, "hard_input_tokens", start->has_hard_input ?
                json_integer((json_int_t)start->hard_input_tokens) : json_null()) < 0 ||
            snag_json_set_new(data, "requested_output_tokens", start->has_requested_output ?
                json_integer((json_int_t)start->requested_output_tokens) : json_null()) < 0) {
            return -1;
        }
    }
    if (start->has_irc_seq &&
        snag_json_set_new(data, "irc_seq", json_integer((json_int_t)start->irc_seq)) < 0) {
        return -1;
    }
    return put_host_context(data, &start->host_context);
}

static int
put_response_ids(json_t *data, const unsigned char turn[16], const unsigned char response[16],
    uint32_t cycle)
{
    if (put_id(data, "turn_id", turn) < 0 || put_id(data, "response_id", response) < 0 ||
        snag_json_set_new(data, "cycle", json_integer(cycle)) < 0) {
        return -1;
    }
    return 0;
}

static int
put_public_item(json_t *data, const struct snag_binary_public_item *item)
{
    if (put_id(data, "local_item_id", item->id) < 0 ||
        snag_json_set_new(data, "kind", json_string(public_kinds[item->kind])) < 0 ||
        snag_json_set_new(data, "phase", json_string(public_phases[item->phase])) < 0 ||
        put_text(data, "provider_item_id", item->provider_id) < 0 ||
        put_text(data, "text", item->text) < 0) {
        return -1;
    }
    return 0;
}

static int
put_public_items(json_t *data, const char *key, const struct snag_binary_public_items *items)
{
    size_t offset = 0u;
    struct snag_binary_public_value value;
    int rc;
    while ((rc = snag_binary_public_items_next(items, &offset, &value)) == 0) {
        if (value.source.first.sequence) return unsupported();
    }
    if (rc < 0) return -1;
    json_t *array = json_array();
    if (!array) return -1;
    offset = 0u;
    while ((rc = snag_binary_public_items_next(items, &offset, &value)) == 0) {
        json_t *item = json_object();
        if (!item) goto fail;
        if (put_public_item(item, &value.item) < 0) {
            json_decref(item);
            goto fail;
        }
        if (json_array_append_new(array, item) < 0) goto fail;
    }
    if (rc < 0 || snag_partial_public_validate(array, NULL, 0u) < 0) goto fail;
    return snag_json_set_new(data, key, array);
fail:
    json_decref(array);
    return -1;
}

static int
put_response_output(json_t *data, const struct snag_binary_response_output *value)
{
    if (value->index > INT64_MAX) return invalid();
    if (put_response_ids(data, value->turn, value->response, value->cycle) < 0 ||
        snag_json_set_new(data, "index", json_integer((json_int_t)value->index)) < 0 ||
        snag_json_set_new(data, "offset", json_integer((json_int_t)value->offset)) < 0) {
        return -1;
    }
    json_t *item = json_object();
    if (!item) return -1;
    if (put_public_item(item, &value->item) < 0 || public_item_source_valid(item) < 0) {
        json_decref(item);
        return -1;
    }
    return snag_json_set_new(data, "item", item);
}

static int
put_response_failure(json_t *data, const struct snag_binary_response_failure *value)
{
    if (put_response_ids(data, value->turn, value->response, value->cycle) < 0 ||
        snag_json_set_new(data, "class", json_string(failure_classes[value->class_name])) < 0 ||
        snag_json_set_new(data, "retry_count", json_integer(value->retry_count)) < 0 ||
        put_text(data, "message", value->message) < 0 ||
        put_public_items(data, "partial_public", &value->partial) < 0) {
        return -1;
    }
    if (((value->present & SNAG_BINARY_FAILURE_POLICY_STOPPED) &&
         snag_json_set_new(data, "policy_stopped", json_boolean(value->policy_stopped)) < 0) ||
        ((value->present & SNAG_BINARY_FAILURE_TURN_RETRIES) &&
         snag_json_set_new(data, "turn_retry_attempts",
            json_integer((json_int_t)value->turn_retry_attempts)) < 0) ||
        ((value->present & SNAG_BINARY_FAILURE_NEW_INPUT) &&
         snag_json_set_new(data, "new_input", json_boolean(value->new_input)) < 0)) {
        return -1;
    }
    if (value->present & SNAG_BINARY_FAILURE_POLICY) {
        json_t *policy = json_object();
        if (!policy) return -1;
        if (put_text(policy, "code", value->policy_code) < 0 ||
            put_text(policy, "type", value->policy_type) < 0 ||
            put_text(policy, "clarification_skipped", value->clarification_skipped) < 0) {
            json_decref(policy);
            return -1;
        }
        return snag_json_set_new(data, "policy", policy);
    }
    return 0;
}

static int
put_response_end(json_t *data, const struct snag_binary_event *event)
{
    if (event->kind == SNAG_BINARY_RESPONSE_FAILED) {
        return put_response_failure(data, &event->data.response_failed);
    }
    if (event->kind == SNAG_BINARY_RESPONSE_INTERRUPTED) {
        const struct snag_binary_response_interruption *value = &event->data.response_interrupted;
        if (put_response_ids(data, value->turn, value->response, value->cycle) < 0 ||
            snag_json_set_new(data, "origin", json_string(interrupt_origins[value->origin])) < 0 ||
            snag_json_set_new(data, "reason", json_string(interrupt_reasons[value->reason])) < 0) {
            return -1;
        }
        return put_public_items(data, "partial_public", &value->partial);
    }
    const struct snag_binary_response_correction *value = &event->data.response_correction;
    if (put_response_ids(data, value->turn, value->response, value->cycle) < 0 ||
        put_id(data, "correction_id", value->correction) < 0 ||
        put_text(data, "text", value->text) < 0) {
        return -1;
    }
    return put_public_items(data, "partial_public", &value->partial);
}

static int
put_call(json_t *item, const struct snag_binary_call *call)
{
    if (snag_json_set_new(item, "kind", json_string("tool_call")) < 0 ||
        put_id(item, "call_id", call->id) < 0 ||
        put_text(item, "provider_item_id", call->provider_id) < 0 ||
        put_text(item, "provider_call_id", call->provider_call_id) < 0 ||
        put_text(item, "name", call->name) < 0) {
        return -1;
    }
    json_t *arguments = snag_json_load_canonical_bounded(call->arguments.data,
        call->arguments.size, SNAG_MAX_TOOL_ARGUMENTS, NULL, 0u);
    if (!arguments) return -1;
    return snag_json_set_new(item, "arguments", arguments);
}

static int
put_graph_items(json_t *data, const struct snag_binary_graph_items *items)
{
    size_t offset = 0u;
    struct snag_binary_graph_item value;
    int rc;
    while ((rc = snag_binary_graph_items_next(items, &offset, &value)) == 0) {
        if (value.kind != SNAG_BINARY_ITEM_TOOL_CALL && value.data.output.source.first.sequence) {
            return unsupported();
        }
    }
    if (rc < 0) return -1;
    json_t *array = json_array();
    if (!array) return -1;
    offset = 0u;
    while ((rc = snag_binary_graph_items_next(items, &offset, &value)) == 0) {
        json_t *item = json_object();
        if (!item) goto fail;
        int projected = value.kind == SNAG_BINARY_ITEM_TOOL_CALL ?
            put_call(item, &value.data.call) : put_public_item(item, &value.data.output.item);
        if (projected < 0) {
            json_decref(item);
            goto fail;
        }
        if (json_array_append_new(array, item) < 0) goto fail;
    }
    if (rc < 0 || graph_source_valid(array, snag_json_string(data, "provider_response_id")) < 0) {
        goto fail;
    }
    return snag_json_set_new(data, "items", array);
fail:
    json_decref(array);
    return -1;
}

static int
put_continuation(json_t *data, const struct snag_binary_response_complete *value)
{
    if (value->continuation_form == SNAG_BINARY_CONTINUATION_ABSENT) return 0;
    if (put_digest(data, "continuation_scope", value->continuation_scope) < 0) {
        return -1;
    }
    if (value->continuation_form == SNAG_BINARY_CONTINUATION_NULL) {
        return snag_json_set_new(data, "continuation", json_null());
    }
    json_t *array = json_array();
    if (!array) return -1;
    size_t offset = 0u;
    struct snag_binary_continuation_item value_item;
    int rc;
    while ((rc = snag_binary_continuation_next(&value->continuation, &offset, &value_item)) == 0) {
        json_t *item = snag_json_load_canonical_bounded(value_item.item.data,
            value_item.item.size, SNAG_MAX_RESPONSE_GRAPH, NULL, 0u);
        if (!item) goto fail;
        json_t *record = json_pack("{s:I,s:O}",
            "before", (json_int_t)value_item.before, "item", item);
        json_decref(item);
        if (!record || json_array_append_new(array, record) < 0) goto fail;
    }
    if (rc < 0 || !snag_response_continuation_valid(array, value->items.count)) {
        goto fail;
    }
    return snag_json_set_new(data, "continuation", array);
fail:
    json_decref(array);
    return -1;
}

static int
put_response_complete(json_t *data, const struct snag_binary_response_complete *value)
{
    if (put_response_ids(data, value->turn, value->response, value->cycle) < 0 ||
        snag_json_set_new(data, "status", json_string("completed")) < 0 ||
        put_text(data, "provider_response_id", value->provider_id) < 0 ||
        put_graph_items(data, &value->items) < 0 || put_continuation(data, value) < 0) {
        return -1;
    }
    json_t *usage = snag_response_usage_json(&value->usage);
    if (!usage) return -1;
    if (value->cached_present && !value->usage.cached_known &&
        snag_json_set_new(usage, "cached_tokens", json_null()) < 0) {
        json_decref(usage);
        return -1;
    }
    return snag_json_set_new(data, "usage", usage);
}

static int
put_transfer_archive(json_t *data, const struct snag_binary_voice_archive *value)
{
    struct snag_binary_text type;
    json_t *source = NULL;
    if (value->source_version == SNAG_BINARY_ARCHIVE_PUBLIC_VERSION) {
        struct snag_binary_archive_fields fields;
        if (snag_binary_archive_fields_decode(value->source_kind,
            value->data, value->size, &fields) < 0 ||
            snag_binary_archive_fields_json(&fields, &source) < 0) {
            return -1;
        }
        type = fields.type;
    } else {
        struct snag_binary_record record = {.kind = value->source_kind,
            .version = value->source_version, .payload = value->data, .size = value->size};
        const char *name = NULL;
        if (snag_binary_legacy_decode(&record, &name, &source) < 0) return -1;
        type = (struct snag_binary_text){(const unsigned char *)name, strlen(name)};
    }
    if (put_id(data, "transfer_id", value->id) < 0 ||
        put_id(data, "target_session_id", value->target) < 0 ||
        put_id(data, "source_session_id", value->source) < 0 ||
        snag_json_set_new(data, "source_seq", json_integer((json_int_t)value->source_seq)) < 0 ||
        put_text(data, "source_type", type) < 0) {
        json_decref(source);
        return -1;
    }
    return snag_json_set_new(data, "data", source);
}

static int
put_transfer_metadata(json_t *data, const struct snag_binary_event *event)
{
    bool adopted = event->kind == SNAG_BINARY_VOICE_TRANSFER_ADOPTED;
    const struct snag_binary_voice_transfer *transfer = adopted ?
        &event->data.voice_transfer_adopted.transfer : &event->data.voice_transfer_sealed;
    if (put_id(data, "transfer_id", transfer->id) < 0 ||
        put_id(data, "target_session_id", transfer->target) < 0 ||
        put_id(data, "source_session_id", transfer->source) < 0 ||
        snag_json_set_new(data, "source_as_of_seq",
            json_integer((json_int_t)transfer->source_as_of)) < 0 ||
        snag_json_set_new(data, "count", json_integer((json_int_t)transfer->count)) < 0) {
        return -1;
    }
    if (!adopted) return 0;
    const struct snag_binary_voice_adopted *value = &event->data.voice_transfer_adopted;
    if (value->native) return snag_errno(ENOTSUP);
    if (snag_json_set_new(data, "begin_offset",
        json_integer((json_int_t)value->begin_offset)) < 0 ||
        snag_json_set_new(data, "begin_seq", json_integer((json_int_t)value->begin_seq)) < 0) {
        return -1;
    }
    return put_digest(data, "begin_sha256", value->begin_sha256);
}

static int
put_irc_route(json_t *data, const struct snag_binary_irc_route *route)
{
    static const char *const identities[] = {"operator", "agent"};
    static const char *const conversations[] = {"connection", "channel", "query"};
    static const char *const directions[] = {"incoming", "outgoing"};
    static const char *const deliveries[] = {
        "none", "pending", "written", "acknowledged", "failed", "uncertain"
    };
    _Static_assert(SNAG_IRC_AGENT == 1 && SNAG_IRC_QUERY == 2 && SNAG_IRC_OUTGOING == 1 &&
        SNAG_IRC_UNCERTAIN == 5, "IRC route positions must match the native schema");
    json_t *routing = json_object();
    if (!routing) return -1;
    int rc = -1;
    if (put_id(routing, "connection_id", route->connection) < 0 ||
        put_id(routing, "conversation_id", route->conversation) < 0 ||
        snag_json_set_new(routing, "generation", json_integer((json_int_t)route->generation)) < 0 ||
        snag_json_set_new(routing, "identity", json_string(identities[route->identity])) < 0 ||
        snag_json_set_new(routing, "conversation_kind",
            json_string(conversations[route->kind])) < 0 ||
        snag_json_set_new(routing, "direction", json_string(directions[route->direction])) < 0 ||
        snag_json_set_new(routing, "state", json_string(deliveries[route->delivery])) < 0 ||
        snag_json_set_new(routing, "action", json_boolean(route->action)) < 0 ||
        put_text(routing, "peer", route->peer) < 0 ||
        put_text(routing, "target", route->target) < 0 ||
        put_text(routing, "source_message_id", route->source) < 0 ||
        (route->has_send ? put_id(routing, "send_id", route->send) :
            snag_json_set_new(routing, "send_id", json_string(""))) < 0 ||
        (route->revised && snag_json_set_new(routing, "revised", json_true()) < 0) ||
        (route->has_membership && (put_id(routing, "membership", route->membership) < 0 ||
            snag_json_set_new(routing, "joined", json_boolean(route->joined)) < 0 ||
            snag_json_set_new(routing, "rejoin", json_boolean(route->rejoin)) < 0))) goto done;
    if (json_object_set(data, "routing", routing) < 0) goto done;
    if (route->reply_captured) {
        json_t *reply = route->has_reply ? json_object() : json_null();
        if (!reply) goto done;
        if (route->has_reply && (put_id(reply, "conversation_id", route->reply_conversation) < 0 ||
            put_id(reply, "membership", route->reply_membership) < 0)) {
            json_decref(reply);
            goto done;
        }
        if (snag_json_set_new(data, "reply_to", reply) < 0) goto done;
    }
    rc = 0;
done:
    json_decref(routing);
    return rc;
}

static int
put_irc_event(json_t *data, const struct snag_binary_irc_event *event, bool routed)
{
    if (snag_json_set_new(data, "kind", json_string(irc_kinds[event->kind])) < 0 ||
        snag_json_set_new(data, "timestamp_ms",
            json_integer((json_int_t)event->timestamp_ms)) < 0 ||
        snag_json_set_new(data, "historical", json_boolean(event->historical)) < 0 ||
        snag_json_set_new(data, "local", json_boolean(event->is_local)) < 0 ||
        snag_json_set_new(data, "op", json_boolean(event->op)) < 0 ||
        put_text(data, "endpoint", event->endpoint) < 0 ||
        put_text(data, "room", event->room) < 0 ||
        put_text(data, "nick", event->nick) < 0 || put_text(data, "text", event->text) < 0) {
        return -1;
    }
    if (event->has_watermark) {
        int rc = event->has_stream ? put_id(data, "stream", event->stream) :
            snag_json_set_new(data, "stream", json_string(""));
        if (rc < 0 || snag_json_set_new(data, "sequence",
            json_integer((json_int_t)event->sequence)) < 0 ||
            snag_json_set_new(data, "input", json_boolean(event->input)) < 0) {
            return -1;
        }
    }
    if (event->classified && (snag_json_set_new(data, "urgent", json_boolean(event->urgent)) < 0 ||
        snag_json_set_new(data, "reply", json_boolean(event->reply)) < 0)) {
        return -1;
    }
    if (routed && put_irc_route(data, &event->route) < 0) return -1;
    struct snag_irc_event checked;
    return snag_irc_event_record_read(routed ? "irc_event_v2" : "irc_event", data, &checked);
}

static int
put_irc(json_t *data, const struct snag_binary_event *event)
{
    if (event->kind == SNAG_BINARY_IRC_SLEEP_SET) {
        if (snag_json_set_new(data, "until_ms",
            json_integer((json_int_t)event->data.irc_sleep_set.until_ms)) < 0) return -1;
        return snag_json_set_new(data, "messages",
            json_integer((json_int_t)event->data.irc_sleep_set.messages));
    }
    if (event->kind == SNAG_BINARY_IRC_SLEEP_WOKE) {
        return snag_json_set_new(data, "reason", json_string(irc_wake_reasons[
            event->data.irc_sleep_woke]));
    }
    if (event->kind == SNAG_BINARY_IRC_COMPACT_CONFIGURED) {
        if (snag_json_set_new(data, "after_updates",
            json_integer((json_int_t)event->data.irc_compact_configured.after_updates)) < 0)
            return -1;
        return put_text(data, "instruction", event->data.irc_compact_configured.instruction);
    }
    if (event->kind == SNAG_BINARY_IRC_COMPACTED) {
        if (snag_json_set_new(data, "through_seq",
            json_integer((json_int_t)event->data.irc_compacted.through_seq)) < 0 ||
            snag_json_set_new(data, "count",
            json_integer((json_int_t)event->data.irc_compacted.count)) < 0) return -1;
        return put_text(data, "summary", event->data.irc_compacted.summary);
    }
    if (event->kind == SNAG_BINARY_IRC_EVENT || event->kind == SNAG_BINARY_IRC_EVENT_V2) {
        return put_irc_event(data, &event->data.irc_event,
            event->kind == SNAG_BINARY_IRC_EVENT_V2);
    }
    if (event->kind == SNAG_BINARY_IRC_SNAPSHOT) {
        const struct snag_binary_irc_snapshot *snapshot = &event->data.irc_snapshot;
        if (snag_json_set_new(data, "reason",
            json_string(irc_snapshot_reasons[snapshot->reason])) < 0 ||
            snag_json_set_new(data, "timestamp_ms",
                json_integer((json_int_t)snapshot->timestamp_ms)) < 0) {
            return -1;
        }
        return put_text(data, "text", snapshot->text);
    }
    const struct snag_binary_irc_admission *admission = &event->data.irc_admitted;
    json_t *sequences = json_array();
    if (!sequences) return -1;
    size_t offset = 0u;
    uint64_t value;
    int rc;
    while ((rc = snag_binary_sequences_next(&admission->sequences, &offset, &value)) == 0) {
        if (json_array_append_new(sequences, json_integer((json_int_t)value)) < 0) {
            json_decref(sequences);
            return -1;
        }
    }
    if (rc < 0) {
        json_decref(sequences);
        return -1;
    }
    if (snag_json_set_new(data, "sequences", sequences) < 0) return -1;
    if (!admission->input.kind) return 0;
    const char *type = NULL;
    json_t *input = NULL;
    if (snag_binary_legacy_decode(&admission->input, &type, &input) < 0) {
        return -1;
    }
    const char *key = admission->input.kind == SNAG_BINARY_INPUT_RECEIVED ? "input" : "steering";
    return snag_json_set_new(data, key, input);
}

static int
put_tool_excerpt(json_t *data, const char *key, const struct snag_binary_tool_excerpt *excerpt)
{
    json_t *value = json_pack("{s:s,s:I,s:I,s:I}",
        "encoding", byte_encodings[excerpt->encoding],
        "discarded_bytes", (json_int_t)excerpt->discarded_bytes,
        "original_bytes", (json_int_t)excerpt->original_bytes,
        "retained_bytes", (json_int_t)excerpt->retained_bytes);
    if (!value) return -1;
    if (put_text(value, "retained", excerpt->retained) < 0) {
        json_decref(value);
        return -1;
    }
    return snag_json_set_new(data, key, value);
}

static int
put_tool_output_ref(json_t *data, const struct snag_binary_tool_output_ref *ref)
{
    json_t *value = json_pack("{s:I,s:I,s:I,s:I,s:I,s:I,s:I,s:b,s:I,s:I}",
        "stdout_start", (json_int_t)ref->from[0], "stdout_end", (json_int_t)ref->to[0],
        "stderr_start", (json_int_t)ref->from[1], "stderr_end", (json_int_t)ref->to[1],
        "stdin_accepted", (json_int_t)ref->stdin_accepted,
        "stdin_written", (json_int_t)ref->stdin_written,
        "stdin_pending", (json_int_t)ref->stdin_pending, "stdin_open", ref->stdin_open,
        "log_start", (json_int_t)ref->log_start, "log_end", (json_int_t)ref->log_end);
    if (!value) return -1;
    if (put_id(value, "handle", ref->handle) < 0) {
        json_decref(value);
        return -1;
    }
    return snag_json_set_new(data, "output_ref", value);
}

static int
put_tool_result(json_t *data, const struct snag_binary_tool_result *result)
{
    json_t *value = json_pack("{s:s,s:n,s:n,s:I}", "status", tool_statuses[result->status],
        "reason", "handle", "duration_ms", (json_int_t)result->duration_ms);
    if (!value) return -1;
    if (put_text(value, "model_text", result->model_text) < 0 ||
        put_tool_excerpt(value, "stdout", &result->streams[0]) < 0 ||
        put_tool_excerpt(value, "stderr", &result->streams[1]) < 0 ||
        (result->has_handle && put_id(value, "handle", result->handle) < 0) ||
        (result->reason && snag_json_set_new(value, "reason",
            json_string(tool_reasons[result->reason])) < 0) ||
        (result->max_output_tokens && snag_json_set_new(value, "max_output_tokens",
            json_integer((json_int_t)result->max_output_tokens)) < 0) ||
        (result->has_output_ref && put_tool_output_ref(value, &result->output_ref) < 0) ||
        put_content(value, &result->content) < 0) {
        goto fail;
    }
    const struct snag_binary_result_value *fields[] = {&result->exit_code, &result->signal};
    const char *keys[] = {"exit_code", "signal"};
    for (size_t i = 0u; i < 2u; ++i) {
        json_t *field = NULL;
        if (snag_binary_result_value_json(fields[i], &field) < 0 ||
            snag_json_set_new(value, keys[i], field) < 0) {
            goto fail;
        }
    }
    if (snag_tool_result_valid(value) < 0) {
        (void)invalid();
        goto fail;
    }
    return snag_json_set_new(data, "result", value);
 fail:
    json_decref(value);
    return -1;
}

static int
put_result_event(json_t *data, const struct snag_binary_event *event)
{
    if (event->kind == SNAG_BINARY_TOOL_FINISHED) {
        const struct snag_binary_tool_finish *value = &event->data.tool_finished;
        if (put_id(data, "turn_id", value->turn) < 0 || put_id(data, "call_id", value->call) < 0) {
            return -1;
        }
        return put_tool_result(data, &value->result);
    }
    const struct snag_binary_process_close *value = &event->data.process_closed;
    if (put_id(data, "turn_id", value->turn) < 0 || put_id(data, "handle", value->handle) < 0 ||
        snag_json_set_new(data, "cause", json_string(process_causes[value->cause])) < 0) {
        return -1;
    }
    return put_tool_result(data, &value->result);
}

static int
put_tool_process(json_t *data, const struct snag_binary_event *event)
{
    if (event->kind == SNAG_BINARY_TOOL_STARTED) {
        const struct snag_binary_tool_start *value = &event->data.tool_started;
        if (put_id(data, "turn_id", value->turn) < 0 || put_id(data, "call_id", value->call) < 0 ||
            put_digest(data, "action_sha256", value->action_sha256) < 0 ||
            put_text(data, "resolved_workdir", value->cwd) < 0) {
            return -1;
        }
        return 0;
    }
    const struct snag_binary_process_output *value = &event->data.process_output;
    if (put_id(data, "turn_id", value->turn) < 0 || put_id(data, "handle", value->handle) < 0 ||
        snag_json_set_new(data, "offset", json_integer((json_int_t)value->offset)) < 0 ||
        snag_json_set_new(data, "stream", json_integer(value->stream)) < 0 ||
        snag_json_set_new(data, "encoding", json_string(byte_encodings[value->encoding])) < 0) {
        return -1;
    }
    if (value->encoding == SNAG_BINARY_BYTES_UTF8) {
        return put_text(data, "data", (struct snag_binary_text){value->data, value->size});
    }
    struct snag_buf encoded = {.max = 4u * ((SNAG_BINARY_PROCESS_CHUNK_MAX + 2u) / 3u)};
    int rc = snag_base64_append(&encoded, value->data, value->size);
    if (rc == 0) {
        rc = put_text(data, "data", (struct snag_binary_text){encoded.data, encoded.len});
    }
    snag_buf_free(&encoded);
    return rc;
}

static int
put_turn_config(json_t *data, const struct snag_binary_turn_config *config)
{
    if (turn_execution_valid(config) < 0) return -1;
    json_t *value = NULL;
    if (snag_binary_rule_value_json(&config->extensions, &value) < 0) return -1;
    if (put_text(value, "provider", config->selection.provider) < 0 ||
        put_text(value, "model", config->selection.model) < 0 ||
        put_text(value, "effort", config->selection.effort) < 0) {
        goto fail;
    }
    for (size_t i = 0u; i < SNAG_BINARY_TURN_NUMBER_COUNT; ++i) {
        if ((config->present & (1u << i)) && snag_json_set_new(value, turn_numbers[i],
            json_integer((json_int_t)config->numbers[i])) < 0) {
            goto fail;
        }
    }
    for (size_t i = 0u; i < SNAG_BINARY_TURN_VALUE_COUNT; ++i) {
        if (!(config->present & (1u << (8u + i)))) continue;
        json_t *field = NULL;
        if (snag_binary_result_value_json(&config->values[i], &field) < 0 ||
            snag_json_set_new(value, turn_values[i], field) < 0) {
            goto fail;
        }
    }
    if ((config->present & SNAG_BINARY_TURN_PARALLEL_CALLS) &&
        snag_json_set_new(value, "parallel_tool_calls", json_boolean(config->parallel_calls)) < 0) {
        goto fail;
    }
    return snag_json_set_new(data, "config", value);
 fail:
    json_decref(value);
    return -1;
}

static int
put_turn_start(json_t *data, const struct snag_binary_turn_start *turn)
{
    if (turn->text_ref.field || turn->instructions_ref.field || turn->content_ref.field) {
        return unsupported();
    }
    if (turn->number > INT64_MAX || turn->queue_seq > INT64_MAX ||
        (turn->has_received_ms && turn->received_ms > INT64_MAX) ||
        (turn->origin == SNAG_BINARY_TURN_GOAL && (turn->read_only || turn->content.size ||
         turn->text.size != strlen(SNAG_GOAL_CONTINUATION_TEXT) ||
         memcmp(turn->text.data, SNAG_GOAL_CONTINUATION_TEXT, turn->text.size)))) {
        return invalid();
    }
    if (put_id(data, "turn_id", turn->id) < 0 ||
        snag_json_set_new(data, "turn_number", json_integer((json_int_t)turn->number)) < 0 ||
        snag_json_set_new(data, "input_kind", json_string(turn_origins[turn->origin])) < 0 ||
        put_text(data, turn->workspace ? "workspace" : "cwd", turn->cwd) < 0 ||
        put_text(data, "text", turn->text) < 0 ||
        put_turn_config(data, &turn->config) < 0 ||
        put_instruction_paths(data, &turn->instructions, true) < 0 ||
        put_content(data, &turn->content) < 0) {
        return -1;
    }
    if (turn->origin == SNAG_BINARY_TURN_QUEUED) {
        if (put_id(data, "queue_id", turn->queue_id) < 0 ||
            snag_json_set_new(data, "queue_seq", json_integer((json_int_t)turn->queue_seq)) < 0) {
            return -1;
        }
    } else if (snag_json_set_new(data, "queue_id", json_null()) < 0 ||
               snag_json_set_new(data, "queue_seq", json_null()) < 0) {
        return -1;
    }
    if ((turn->has_read_only &&
         snag_json_set_new(data, "read_only", json_boolean(turn->read_only)) < 0) ||
        (turn->has_received_ms && snag_json_set_new(data, "received_at_ms",
            json_integer((json_int_t)turn->received_ms)) < 0)) {
        return -1;
    }
    return 0;
}

static int
put_input_receipt(json_t *data, const struct snag_binary_event *event)
{
    if (event->kind == SNAG_BINARY_INPUT_RECEIVED) {
        if (event->data.input.text_ref.field || event->data.input.content_ref.field ||
            event->data.input.instructions_ref.field) {
            return unsupported();
        }
        if (event->data.input.received_ms > INT64_MAX) return invalid();
        const struct snag_binary_selection *selection = &event->data.input.selection;
        if (put_text(data, "provider", selection->provider) < 0 ||
            put_text(data, "model", selection->model) < 0 ||
            put_text(data, "effort", selection->effort) < 0 ||
            put_text(data, "text", event->data.input.text) < 0 ||
            put_instruction_paths(data, &event->data.input.instructions, false) < 0 ||
            put_content(data, &event->data.input.content) < 0 ||
            snag_json_set_new(data, "read_only", json_boolean(event->data.input.read_only)) < 0 ||
            snag_json_set_new(data, "received_at_ms",
                json_integer((json_int_t)event->data.input.received_ms)) < 0) {
            return -1;
        }
        if (event->data.input.origin == SNAG_BINARY_INPUT_TIMER) {
            return snag_json_set_new(data, "origin", json_string("timer"));
        }
        return 0;
    }
    if (event->data.steering_input.text_ref.field || event->data.steering_input.content_ref.field) {
        return unsupported();
    }
    if (event->data.steering_input.has_received_ms &&
        event->data.steering_input.received_ms > INT64_MAX) {
        return invalid();
    }
    if (put_id(data, "steering_id", event->data.steering_input.id) < 0 ||
        put_id(data, "turn_id", event->data.steering_input.turn) < 0 ||
        put_text(data, "text", event->data.steering_input.text) < 0 ||
        put_content(data, &event->data.steering_input.content) < 0) {
        return -1;
    }
    if (event->kind == SNAG_BINARY_IRC_REPLY_REMINDER &&
        strcmp(snag_json_string(data, "text"), SNAG_IRC_REPLY_REMINDER_TEXT)) {
        return invalid();
    }
    if (event->data.steering_input.has_received_ms) {
        return snag_json_set_new(data, "received_at_ms",
            json_integer((json_int_t)event->data.steering_input.received_ms));
    }
    return 0;
}

static int
put_voice_source(json_t *data, const struct snag_binary_voice_source *voice)
{
    if (voice->transcript_ref.field || voice->request_ref.field) {
        return unsupported();
    }
    json_t *value = json_object();
    if (!value) return -1;
    if (put_id(value, "connection_id", voice->connection_id) < 0 ||
        put_text(value, "input_id", voice->input_id) < 0 ||
        put_text(value, "response_id", voice->response_id) < 0 ||
        put_text(value, "call_id", voice->call_id) < 0 ||
        put_text(value, "provider", voice->provider) < 0 ||
        put_text(value, "model", voice->model) < 0 ||
        put_text(value, "transcript", voice->transcript) < 0 ||
        put_text(value, "request", voice->request) < 0) {
        json_decref(value);
        return -1;
    }
    return snag_json_set_new(data, "voice", value);
}

static int
put_queued_input(json_t *data, const struct snag_binary_event *event)
{
    if (event->data.queued.text_ref.field || event->data.queued.content_ref.field ||
        (event->data.queued.has_voice && (event->data.queued.voice.transcript_ref.field ||
                                        event->data.queued.voice.request_ref.field))) {
        return unsupported();
    }
    if (event->data.queued.has_received_ms && event->data.queued.received_ms > INT64_MAX) {
        return invalid();
    }
    if (put_id(data, "queue_id", event->data.queued.id) < 0 ||
        put_text(data, "text", event->data.queued.text) < 0 ||
        put_content(data, &event->data.queued.content) < 0 ||
        snag_json_set_new(data, "read_only", json_boolean(event->data.queued.read_only)) < 0) {
        return -1;
    }
    if (event->data.queued.has_armed &&
        snag_json_set_new(data, "armed", json_boolean(event->data.queued.armed)) < 0) {
        return -1;
    }
    if (event->data.queued.has_received_ms && snag_json_set_new(data, "received_at_ms",
        json_integer((json_int_t)event->data.queued.received_ms)) < 0) {
        return -1;
    }
    if (event->kind == SNAG_BINARY_FUTURE_TURN_QUEUED) {
        int rc;
        if (event->data.queued.while_kind == SNAG_BINARY_WHILE_ID) {
            rc = put_id(data, "while_turn_id", event->data.queued.while_id);
        } else {
            json_t *value = event->data.queued.while_kind == SNAG_BINARY_WHILE_NULL ?
                json_null() : json_string("");
            rc = snag_json_set_new(data, "while_turn_id", value);
        }
        if (rc < 0) return -1;
    }
    if (event->data.queued.has_voice) {
        if (put_voice_source(data, &event->data.queued.voice) < 0) return -1;
        return queued_voice_id_valid(data);
    }
    return 0;
}

static int
put_input_control(json_t *data, const struct snag_binary_event *event)
{
    switch (event->kind) {
    case SNAG_BINARY_INPUT_CANCELLED:
        return 0;
    case SNAG_BINARY_STEERING_DEFERRED:
        return put_id(data, "turn_id", event->data.turn);
    case SNAG_BINARY_INPUT_ADMITTED:
        if (event->data.admission.time_ms > INT64_MAX) return invalid();
        if (put_id(data, "turn_id", event->data.admission.turn) < 0 ||
            snag_json_set_new(data, "time_ms",
                json_integer((json_int_t)event->data.admission.time_ms)) < 0) {
            return -1;
        }
        return put_ids(data, "steering_ids", event->data.admission.ids);
    case SNAG_BINARY_FUTURE_QUEUE_STATE:
        return snag_json_set_new(data, "armed", json_boolean(event->data.queue_armed));
    case SNAG_BINARY_FUTURE_TURN_CANCELLED:
        if (snag_json_set_new(data, "reason", json_string("user")) < 0) {
            return -1;
        }
        return put_ids(data, "queue_ids", event->data.queue_cancel.ids);
    default: return unsupported();
    }
}

static int
put_turn_outcome(json_t *data, const struct snag_binary_event *event)
{
    switch (event->kind) {
    case SNAG_BINARY_TURN_YIELD_REQUESTED:
    case SNAG_BINARY_TURN_CANCEL_REQUESTED:
        return put_id(data, "turn_id", event->data.turn);
    case SNAG_BINARY_TURN_COMPLETED:
        if (put_id(data, "turn_id", event->data.completed.turn) < 0 ||
            put_id(data, "final_response_id", event->data.completed.response) < 0) {
            return -1;
        }
        return put_id(data, "final_item_id", event->data.completed.item);
    case SNAG_BINARY_TURN_COMPLETED_SILENT:
        if (put_id(data, "turn_id", event->data.silent.turn) < 0 ||
            put_id(data, "response_id", event->data.silent.response) < 0) {
            return -1;
        }
        return snag_json_set_new(data, "reason",
            json_string(quiet_reasons[event->data.silent.reason]));
    case SNAG_BINARY_TURN_INTERRUPTED:
        if (put_id(data, "turn_id", event->data.interrupted.turn) < 0 ||
            snag_json_set_new(data, "origin",
                json_string(interrupt_origins[event->data.interrupted.origin])) < 0) {
            return -1;
        }
        return snag_json_set_new(data, "reason",
            json_string(interrupt_reasons[event->data.interrupted.reason]));
    case SNAG_BINARY_TURN_FAILED:
        if (put_id(data, "turn_id", event->data.failed.turn) < 0 ||
            snag_json_set_new(data, "class",
                json_string(failure_classes[event->data.failed.class_id])) < 0) {
            return -1;
        }
        return put_text(data, "message", event->data.failed.message);
    case SNAG_BINARY_TURN_RECOVERY:
        if (put_id(data, "turn_id", event->data.recovery.turn) < 0 ||
            put_text(data, "class", event->data.recovery.class_name) < 0 ||
            put_text(data, "message", event->data.recovery.message) < 0) {
            return -1;
        }
        if (event->data.recovery.has_retry_attempts) {
            return snag_json_set_new(data, "retry_attempts",
                json_integer((json_int_t)event->data.recovery.retry_attempts));
        }
        return 0;
    default: return unsupported();
    }
}

static int
put_compact(json_t *data, const struct snag_binary_event *event)
{
    if (event->kind == SNAG_BINARY_COMPACTION_STARTED) {
        const struct snag_binary_compact_start *value = &event->data.compaction_started;
        if (put_id(data, "compact_id", value->id) < 0 ||
            snag_json_set_new(data, "reason", json_string(compact_reasons[value->reason])) < 0 ||
            snag_json_set_new(data, "count_method",
                json_string(count_methods[value->count_method])) < 0 ||
            snag_json_set_new(data, "source_seq",
                json_integer((json_int_t)value->source_seq)) < 0 ||
            snag_json_set_new(data, "input_tokens_bound",
                json_integer((json_int_t)value->input_tokens)) < 0 ||
            put_digest(data, "source_sha256", value->source_sha256) < 0 ||
            put_digest(data, "request_sha256", value->request_sha256) < 0 ||
            put_digest(data, "count_request_sha256", value->count_request_sha256) < 0 ||
            put_text(data, "model", value->model) < 0 ||
            put_text(data, "capability_version", value->capability) < 0 ||
            put_text(data, "profile_id", value->profile) < 0 ||
            (value->has_predecessor ? put_id(data, "predecessor_compact_id", value->predecessor) :
             snag_json_set_new(data, "predecessor_compact_id", json_null())) < 0 ||
            (value->has_scope && put_digest(data, "continuation_scope", value->scope) < 0) ||
            (value->has_compaction_model &&
             put_text(data, "compaction_model", value->compaction_model) < 0)) {
            return -1;
        }
        return 0;
    }
    if (event->kind == SNAG_BINARY_COMPACTION_INTERRUPTED) {
        const struct snag_binary_compact_interrupt *value = &event->data.compaction_interrupted;
        if (put_id(data, "compact_id", value->id) < 0) return -1;
        return snag_json_set_new(data, "reason", json_string(compact_stop_reasons[value->reason]));
    }
    const struct snag_binary_compact_complete *value = &event->data.compaction_completed;
    if (put_id(data, "compact_id", value->id) < 0 ||
        snag_json_set_new(data, "count_method",
            json_string(count_methods[value->count_method])) < 0 ||
        snag_json_set_new(data, "output_count_method",
            json_string(count_methods[value->output_count_method])) < 0 ||
        snag_json_set_new(data, "input_tokens_bound",
            json_integer((json_int_t)value->input_tokens)) < 0 ||
        snag_json_set_new(data, "output_tokens_bound",
            json_integer((json_int_t)value->output_tokens)) < 0 ||
        put_digest(data, "output_count_request_sha256", value->output_count_request_sha256) < 0 ||
        put_digest(data, "source_sha256", value->source_sha256) < 0 ||
        put_digest(data, "output_sha256", value->output_sha256) < 0 ||
        (value->has_scope && put_digest(data, "continuation_scope", value->scope) < 0)) {
        return -1;
    }
    json_t *output = snag_json_load_canonical_bounded(value->output.data, value->output.size,
        12u * 1024u * 1024u, NULL, 0u);
    if (!output) return -1;
    return snag_json_set_new(data, "output", output);
}

int
snag_binary_legacy_decode(const struct snag_binary_record *record, const char **type,
                          json_t **data)
{
    if (!record || !type || !data) return invalid();
    struct snag_binary_event event;
    int rc = snag_binary_event_decode(record, &event);
    if (rc < 0) return -1;
    if (rc != 0 || !legacy_kind(event.kind)) return unsupported();
    json_t *result = json_object();
    if (!result) {
        errno = ENOMEM;
        return -1;
    }
    if (metadata_kind(event.kind)) rc = put_metadata(result, &event);
    else if (timer_kind(event.kind)) rc = put_timer(result, &event);
    else if (goal_kind(event.kind)) rc = put_goal(result, &event);
    else if (control_kind(event.kind)) rc = put_control(result, &event);
    else if (download_kind(event.kind)) rc = put_download(result, &event);
    else if (context_kind(event.kind)) rc = put_context(result, &event);
    else if (rule_voice_kind(event.kind)) rc = put_rule_voice(result, &event);
    else if (hosted_search_kind(event.kind)) rc = put_hosted_search(result, &event);
    else if (event.kind == SNAG_BINARY_TURN_STARTED) {
        rc = put_turn_start(result, &event.data.started);
    } else if (event.kind == SNAG_BINARY_RESPONSE_STARTED) {
        rc = put_response_start(result, &event.data.response_started);
    } else if (event.kind == SNAG_BINARY_RESPONSE_OUTPUT) {
        rc = put_response_output(result, &event.data.response_output);
    } else if (response_end_kind(event.kind)) {
        rc = put_response_end(result, &event);
    } else if (event.kind == SNAG_BINARY_RESPONSE_COMPLETED) {
        rc = put_response_complete(result, &event.data.response_completed);
    } else if (event.kind == SNAG_BINARY_TOOL_STARTED || event.kind == SNAG_BINARY_PROCESS_OUTPUT) {
        rc = put_tool_process(result, &event);
    } else if (event.kind == SNAG_BINARY_TOOL_FINISHED ||
               event.kind == SNAG_BINARY_PROCESS_CLOSED) {
        rc = put_result_event(result, &event);
    } else if (event.kind >= SNAG_BINARY_IRC_EVENT && event.kind <= SNAG_BINARY_IRC_EVENT_V2) {
        rc = put_irc(result, &event);
    } else if (event.kind == SNAG_BINARY_VOICE_TRANSFER_RECORD) {
        rc = put_transfer_archive(result, &event.data.voice_transfer_record);
    } else if (event.kind >= SNAG_BINARY_VOICE_TRANSFER_SEALED &&
               event.kind <= SNAG_BINARY_VOICE_TRANSFER_ADOPTED) {
        rc = put_transfer_metadata(result, &event);
    } else if (input_receipt_kind(event.kind)) {
        rc = put_input_receipt(result, &event);
    } else if (queued_input_kind(event.kind)) {
        rc = put_queued_input(result, &event);
    } else if (event.kind >= SNAG_BINARY_INPUT_RECEIVED &&
             event.kind <= SNAG_BINARY_FUTURE_TURN_EDITED) {
        rc = put_input_control(result, &event);
    } else if (event.kind >= SNAG_BINARY_COMPACTION_STARTED &&
             event.kind <= SNAG_BINARY_COMPACTION_COMPLETED) {
        rc = put_compact(result, &event);
    } else {
        rc = put_turn_outcome(result, &event);
    }
    if (rc == 0) rc = canonical_data(result);
    if (rc < 0) {
        json_decref(result);
        return -1;
    }
    *type = snag_binary_event_name(event.kind);
    *data = result;
    return 0;
}
