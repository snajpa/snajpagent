/* SPDX-License-Identifier: GPL-2.0-only */
#include "ui.h"
#include "commands.h"
#include "irc_address.h"
#include "session_view.h"
#include "store_internal.h"
#include "presentation.h"
#include "fs.h"

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

json_t *
snag_ui_frontend_state(const json_t *state)
{
    static const char *const fields[] = {"seq", "end", "sha256", "presentation",
        "frontend", "prompt", "skills_catalog", "audio", "destinations", "irc_names",
        "channels", "queries", "connections"};
    json_t *copy = json_object();
    if (!copy) return NULL;
    /* Keep terminal input, routing and presentation state, without republishing
     * unused owner metadata. Retain immutable values without copying them. */
    for (size_t i = 0u; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        json_t *value = json_object_get(state, fields[i]);
        if (value && json_object_set(copy, fields[i], value) < 0) {
            json_decref(copy);
            return NULL;
        }
    }
    return copy;
}

#if SNAJPAGENT_VM && !defined(_WIN32)
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <unistd.h>

struct frontend_source {
    struct snag_store store;
    struct snag_session session;
    struct snag_binary_anchor position;
    json_t *prompt;
    int output;
    uint64_t origin;
};

static int
frontend_tail(const json_t *state, struct snag_journal_cursor *tail)
{
    const char *digest = snag_json_bounded_string(json_object_get(state, "sha256"),
        SNAG_SHA256_HEX_LEN);
    uint64_t sequence, end;
    if (!digest || !snag_hex_is_lower(digest, SNAG_SHA256_HEX_LEN) ||
        snag_json_integer_u64(state, "seq", &sequence) < 0 || sequence >= INT64_MAX ||
        snag_json_integer_u64(state, "end", &end) < 0 || end > INT64_MAX)
        return snag_errno(EINVAL);
    *tail = (struct snag_journal_cursor){.offset = (int64_t)end, .next_seq = sequence + 1u};
    memcpy(tail->prev_sha256, digest, sizeof(tail->prev_sha256));
    return 0;
}

static int
frontend_history(struct snag_ui *ui, struct frontend_source *source, const json_t *message)
{
    const char *root = snag_json_bounded_string(json_object_get(message, "dotdir"),
        SNAG_PATH_MAX_BYTES);
    const char *id = snag_json_bounded_string(json_object_get(message, "session"), SNAG_ID_HEX_LEN);
    const json_t *state = json_object_get(message, "state");
    uint64_t turns;
    struct snag_journal_cursor tail;
    if (source->session.id[0] || !root || !snag_path_root_len(root) || !id ||
        frontend_tail(state, &tail) < 0 ||
        snag_json_integer_u64(message, "turns", &turns) < 0)
        return snag_errno(EINVAL);
    struct snag_store *store = &source->store;
    struct snag_session *session = &source->session;
    char error[256];
    /* Read an owner-certified prefix without acquiring or probing its writer lock. */
    store->root_path = snag_strdup_checked(root, SNAG_PATH_MAX_BYTES);
    store->root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (!store->root_path || store->root_fd < 0 ||
        snag_store_verify_private_fd(store->root_fd, true, "state root", error, sizeof(error)) < 0)
        return -1;
    store->sessions_fd = openat(store->root_fd, "sessions",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (store->sessions_fd < 0 || snag_store_verify_private_fd(store->sessions_fd, true,
            "sessions directory", error, sizeof(error)) < 0 ||
        snag_session_history_open(store, session, id, &tail, error, sizeof(error)) < 0)
        return -1;
    if (snag_presentation_bound(json_object_get(state, "presentation"), &source->origin,
            &source->position) < 0) return -1;
    source->output = snag_open_read_security_at(session->dir_fd, SNAG_PRESENTATION_FILE, false);
    if (source->output < 0 || snag_ui_history_read(ui, root, id) < 0) return -1;
    if (snag_ui_attachment_history(ui, session, turns) < 0) return -1;
    json_t *cancelled = NULL;
    if (snag_presentation_local_records(source->output, session->id,
            &source->position, &cancelled) < 0) return -1;
    int rc = 0;
    for (size_t i = 0u; !rc && i < json_array_size(cancelled); ++i)
        rc = snag_ui_send(ui, (struct snag_ui_command){.kind = SNAG_UI_PRESENTATION_REPLAY,
            .data.presentation = {.record = json_array_get(cancelled, i), .journal = -1}});
    json_decref(cancelled);
    return rc;
}

static int
frontend_output(struct snag_ui *ui, struct frontend_source *source, const json_t *state)
{
    struct snag_journal_cursor tail;
    struct snag_binary_anchor bound;
    uint64_t origin;
    char error[256];
    if (source->output < 0 || frontend_tail(state, &tail) < 0 ||
        snag_session_history_refresh(&source->session, &tail, error, sizeof(error)) < 0 ||
        snag_presentation_bound(json_object_get(state, "presentation"), &origin, &bound) < 0)
        return -1;
    if (origin != source->origin || bound.end < source->position.end ||
        bound.next_seq < source->position.next_seq ||
        (bound.end == source->position.end &&
         memcmp(bound.digest, source->position.digest, sizeof(bound.digest))))
        return snag_errno(ESTALE);
    snag_file_info held, named;
    if (snag_fstat(source->output, &held) < 0 ||
        snag_lstat_at(source->session.dir_fd, SNAG_PRESENTATION_FILE, &named) < 0 ||
        !S_ISREG(named.st_mode) || held.st_dev != named.st_dev || held.st_ino != named.st_ino)
        return snag_errno(ESTALE);
    while (source->position.end < bound.end) {
        json_t *records = NULL;
        struct snag_binary_anchor next, observed;
        bool incomplete;
        if (snag_presentation_read(source->output, source->session.id, &bound,
                &source->position, false, SNAG_JOURNAL_PAGE_BYTES, &records, &next, &observed,
                &incomplete, NULL, NULL) < 0) return -1;
        int rc = 0;
        for (size_t i = 0u; !rc && i < json_array_size(records); ++i)
            rc = snag_ui_send(ui, (struct snag_ui_command){.kind = SNAG_UI_PRESENTATION_REPLAY,
                .data.presentation = {.record = json_object_get(json_array_get(records, i), "data"),
                    .journal = source->session.log_fd}});
        json_decref(records);
        if (rc < 0) return -1;
        if (incomplete || next.end <= source->position.end) return snag_errno(ESTALE);
        source->position = next;
    }
    return 0;
}

static int
frontend_settings(struct snag_ui *ui, const json_t *message, bool initialize)
{
    uint64_t color, level, pause, delay;
    const json_t *markdown = json_object_get(message, "markdown");
    if (!json_is_boolean(markdown) ||
        snag_json_integer_u64(message, "color", &color) < 0 || color > SNAG_COLOR_NEVER ||
        snag_json_integer_u64(message, "verbosity", &level) < 0 || level > SNAG_VERBOSITY_MAX ||
        snag_json_integer_u64(message, "typing_pause", &pause) < 0 || pause > UINT32_MAX ||
        snag_json_integer_u64(message, "tool_delay", &delay) < 0 || delay > UINT32_MAX)
        return snag_errno(EINVAL);
    if (snag_ui_send(ui, (struct snag_ui_command){.kind = SNAG_UI_COLOR,
            .data.value = (unsigned int)color}) < 0 ||
        (initialize && snag_ui_send(ui, (struct snag_ui_command){.kind = SNAG_UI_LEVEL,
            .data.value = (unsigned int)level}) < 0) ||
        snag_ui_send(ui, (struct snag_ui_command){.kind = SNAG_UI_MARKDOWN,
            .data.value = json_is_true(markdown)}) < 0)
        return -1;
    return snag_ui_send(ui, (struct snag_ui_command){.kind = SNAG_UI_PAUSE,
        .data.timing = {(uint32_t)pause, (uint32_t)delay}});
}

static int
frontend_prompt(struct snag_ui *ui, struct frontend_source *source, json_t *prompt)
{
    if (snag_ui_prompt_frame(ui, prompt) < 0) return -1;
    json_incref(prompt);
    json_decref(source->prompt);
    source->prompt = prompt;
    return 0;
}

static int
frontend_message(struct snag_ui *ui, struct frontend_source *source, const json_t *message)
{
    const char *type = snag_json_string(message, "type");
    if (!type) return snag_errno(EINVAL);
    if (!strcmp(type, "frame")) {
        const json_t *state = json_object_get(message, "state");
        const json_t *editors = json_object_get(message, "drafts");
        bool initial = !source->session.id[0];
        if (!state) {
            if (initial || !json_is_array(editors)) return snag_errno(EINVAL);
            if (snag_ui_send(ui, (struct snag_ui_command){.kind = SNAG_UI_EDITORS,
                    .data.editors = editors}) < 0) return -1;
            /* Preserve the ordinary readiness boundary after adopting a draft. */
            return snag_ui_prompt_frame(ui, source->prompt);
        }
        /* The owner supplies defaults. Later frames must leave this frontend's
         * local verbosity intact, including a command still awaiting its receipt. */
        if (frontend_settings(ui, json_object_get(state, "frontend"), initial) < 0) return -1;
        if (initial && json_object_get(message, "verbosity")) {
            uint64_t level;
            if (snag_json_integer_u64(message, "verbosity", &level) < 0 ||
                level > SNAG_VERBOSITY_MAX ||
                snag_ui_send(ui, (struct snag_ui_command){.kind = SNAG_UI_LEVEL,
                    .data.value = (unsigned int)level}) < 0) return -1;
        }
        if (snag_ui_send(ui, (struct snag_ui_command){.kind = SNAG_UI_VIEW_STATE,
                .data.voice = state}) < 0) return -1;
        if (!source->session.id[0]) {
            if (frontend_history(ui, source, message) < 0) return -1;
        } else if (frontend_output(ui, source, state) < 0) return -1;
        if (editors && snag_ui_send(ui, (struct snag_ui_command){.kind = SNAG_UI_EDITORS,
                .data.editors = editors}) < 0) return -1;
        const json_t *route = json_object_get(message, "route");
        if (route) {
            if (json_is_null(route)) {
                if (snag_ui_send(ui, (struct snag_ui_command){.kind = SNAG_UI_VIEW,
                        .data.value = SNAG_RENDER_ROLLOUT}) < 0) return -1;
            } else if (snag_ui_send(ui, (struct snag_ui_command){
                    .kind = SNAG_UI_CONVERSATION_SELECT, .route = route}) < 0) return -1;
        }
        const json_t *selection = json_object_get(message, "selection");
        if (selection && snag_ui_send(ui, (struct snag_ui_command){
                .kind = SNAG_UI_CONVERSATION_SELECT, .route = selection,
                .data.value = 1u}) < 0) return -1;
        return frontend_prompt(ui, source, json_object_get(state, "prompt"));
    }
    if (!strcmp(type, "history")) return frontend_history(ui, source, message);
    if (!strcmp(type, "state"))
        return frontend_output(ui, source, json_object_get(message, "state"));
    if (!strcmp(type, "prompt")) return frontend_prompt(ui, source, (json_t *)message);
    if (!strcmp(type, "ready")) return snag_ui_prompt_frame(ui, source->prompt);
    if (!strcmp(type, "settings")) return frontend_settings(ui, message, true);
    if (!strcmp(type, "output")) {
        const json_t *record = json_object_get(message, "record");
        if (!json_is_object(record)) return snag_errno(EINVAL);
        return snag_ui_send(ui, (struct snag_ui_command){.kind = SNAG_UI_PRESENTATION_REPLAY,
            .data.presentation = {.record = record, .journal = -1}});
    }
    if (!strcmp(type, "draft")) {
        const char *text = snag_json_string(message, "text");
        return text ? snag_ui_send(ui, (struct snag_ui_command){
            .kind = SNAG_UI_DRAFT, .text = text}) : snag_errno(EINVAL);
    }
    if (!strcmp(type, "stop")) return 1;
    uint64_t value;
    if (snag_json_integer_u64(message, "value", &value) < 0 || value > UINT_MAX)
        return snag_errno(EINVAL);
    enum snag_ui_operation operation;
    if (!strcmp(type, "color") && value <= SNAG_COLOR_NEVER)
        operation = SNAG_UI_COLOR;
    else if (!strcmp(type, "markdown") && value <= 1u)
        operation = SNAG_UI_MARKDOWN;
    else if (!strcmp(type, "verbosity"))
        operation = SNAG_UI_LEVEL;
    else
        return snag_errno(EINVAL);
    return snag_ui_send(ui, (struct snag_ui_command){.kind = operation,
        .data.value = (unsigned int)value});
}
#endif

int
snag_ui_frontend_main(const char *descriptor)
{
#if SNAJPAGENT_VM && !defined(_WIN32)
    char *end;
    errno = 0;
    long fd = descriptor ? strtol(descriptor, &end, 10) : -1;
    if (!descriptor || *descriptor < '0' || *descriptor > '9' || errno || *end ||
        fd <= STDERR_FILENO || fd > INT_MAX)
        return 2;
    int flags = fcntl((int)fd, F_GETFL);
    if (flags < 0 || snag_fd_cloexec((int)fd) < 0 ||
        fcntl((int)fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        snag_session_peer_verify((int)fd) <= 0) {
        (void)close((int)fd);
        return 2;
    }
    struct snag_view_channel channel;
    snag_view_channel_init(&channel, (int)fd);
    if (!snag_text_locale_init()) {
        snag_view_channel_close(&channel);
        return 2;
    }
    struct snag_ui ui;
    if (snag_ui_init(&ui) < 0) {
        snag_view_channel_close(&channel);
        return 2;
    }
    ui.frontend = true;
    char error[256], frontend_id[SNAG_ID_HEX_LEN + 1u];
    if (snag_random_id(frontend_id) < 0 ||
        snag_ui_send(&ui, (struct snag_ui_command){.kind = SNAG_UI_FRONTEND,
            .text = frontend_id}) < 0 ||
        snag_ui_send(&ui, (struct snag_ui_command){.kind = SNAG_UI_COMMANDS,
            .data.commands = {snag_commands, snag_command_count()}}) < 0 ||
        snag_ui_open(&ui, error, sizeof(error)) < 0) {
        snag_ui_free(&ui);
        snag_view_channel_close(&channel);
        return 2;
    }
    int result = 2;
    struct frontend_source source = {.output = -1};
    snag_store_init(&source.store);
    snag_session_init(&source.session);
    json_t *pending = NULL;
    for (;;) {
        uint64_t now = snag_monotonic_ms();
        if ((channel.write_deadline && now >= channel.write_deadline) ||
            (channel.read_deadline && now >= channel.read_deadline)) break;
        if (pending && !channel.output) {
            if (snag_view_channel_send(&channel, pending) < 0) break;
            json_decref(pending);
            pending = NULL;
        }
        if (channel.output && snag_view_channel_write(&channel) < 0) break;
        /* A queued acknowledgement may outlive a backpressured editor write.
         * Send it before waiting with both input sources disabled. */
        if (pending && !channel.output) continue;
        json_t *message = NULL;
        int rc = pending ? 0 : snag_view_channel_read(&channel, &message);
        if (rc < 0) break;
        bool received = rc > 0;
        if (received) {
            rc = frontend_message(&ui, &source, message);
            if (!rc && !strcmp(snag_json_string(message, "type"), "frame")) {
                uint64_t serial;
                if (pending || snag_json_integer_u64(message, "serial", &serial) < 0 ||
                    serial > INT64_MAX) rc = -1;
                else pending = json_pack("{s:s,s:I}", "type", "frame", "serial",
                    (json_int_t)serial);
                const json_t *audio = json_object_get(json_object_get(message, "state"), "audio");
                uint64_t epoch;
                if (pending && snag_json_integer_u64(audio, "epoch", &epoch) == 0 && epoch) {
                    int visible = snag_ui_send(&ui, (struct snag_ui_command){
                        .kind = SNAG_UI_FRONTEND_VISIBLE});
                    if (visible < 0) rc = -1;
                    if (!visible) {
                        char marker[96];
                        int length = snprintf(marker, sizeof(marker),
                            "\033]2;SNAJPAGENT-SCREEN/1:FRAME:%llu\033\\",
                            (unsigned long long)serial);
                        if (length < 0 || (size_t)length >= sizeof(marker) ||
                            snag_ui_send(&ui, (struct snag_ui_command){.kind = SNAG_UI_RAW,
                                .text = marker, .len = (size_t)length,
                                .data.value = STDOUT_FILENO}) < 0 ||
                            json_object_set_new(pending, "audio",
                                json_integer((json_int_t)epoch)) < 0) rc = -1;
                    }
                }
                if (!pending) rc = -1;
            }
            if (rc < 0) (void)fprintf(stderr, "Frontend frame failed: %s\n", strerror(errno));
            json_decref(message);
            if (rc != 0) {
                if (rc > 0) result = 0;
                break;
            }
        }
        if (!pending && !channel.output) {
            enum snag_term_action action;
            char *text = NULL;
            rc = snag_ui_poll(&ui, 0, &action, &text);
            if (rc < 0) { free(text); break; }
            if (rc > 0) {
                if (action == SNAG_TERM_FEEDBACK || action == SNAG_TERM_EDITORS) {
                    pending = ui.frontend_message;
                    ui.frontend_message = NULL;
                    free(text);
                    if (!pending) break;
                    continue;
                }
                json_t *route = ui.input_view == SNAG_RENDER_CHAT ?
                    snag_irc_conversation_route(&ui.input_conversation) : json_null();
                pending = route ? json_pack("{s:s,s:i,s:s,s:o,s:i,s:b,s:b}", "type", "input",
                    "action", (int)action, "text", text ? text : "", "route", route,
                    "verbosity", ui.input_verbosity < 0 ? (int)snag_ui_verbosity(&ui) :
                        ui.input_verbosity, "echoed", ui.input_echoed,
                    "local", ui.input_local) : NULL;
                free(text);
                if (pending && (action == SNAG_TERM_SUBMIT || action == SNAG_TERM_QUEUE ||
                        action == SNAG_TERM_DICTATE_DONE || action == SNAG_TERM_DICTATE_CANCEL) &&
                    json_object_set_new(pending, "capture", snag_ui_input_capture(&ui)) < 0) {
                    json_decref(pending);
                    pending = NULL;
                }
                if (!pending) break;
                continue;
            }
            free(text);
        }
        if (received || channel.received) continue;
        uint64_t deadline = channel.read_deadline;
        if (channel.write_deadline && (!deadline || channel.write_deadline < deadline))
            deadline = channel.write_deadline;
        now = snag_monotonic_ms();
        int timeout = deadline ? (deadline <= now ? 0 :
            deadline - now > INT_MAX ? INT_MAX : (int)(deadline - now)) : -1;
        struct pollfd wait[] = {{channel.fd, (pending ? 0 : POLLIN) |
                (channel.output ? POLLOUT : 0), 0},
            {pending || channel.output ? -1 : snag_ui_input_wake(&ui), POLLIN, 0}};
        if (poll(wait, 2u, timeout) < 0 && errno != EINTR) break;
    }
    json_decref(pending);
    snag_ui_free(&ui);
    json_decref(source.prompt);
    if (source.output >= 0) (void)close(source.output);
    snag_session_close(&source.session);
    snag_store_close(&source.store);
    snag_view_channel_close(&channel);
    return result;
#else
    (void)descriptor;
    return 2;
#endif
}
