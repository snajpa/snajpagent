/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_UI_H
#define SNAJPAGENT_UI_H
#include "wake.h"

#include "render.h"
#include "session_host.h"

/* Engine-owned preferences and last acknowledged input state, not a renderer. */
struct snag_ui {
    struct snag_ui_runtime *runtime;
    struct snag_history history;
    enum snag_render_view view;
    bool opened;
    bool native, native_continuing;
    struct snag_terminal_profile profile;
    bool prompt_wanted;
    bool active, input_active, input_echoed, input_view_applied;
    bool input_error;
    bool input_interface, input_terminal_command;
    /* Engine-local capture for the duration of semantic command dispatch. */
    struct snag_buf *command_report;
    bool command_error, command_report_passthrough;
    bool view_listening;
    char view_request[SNAG_ID_HEX_LEN + 1u];
    uint64_t view_state_seq;
    /* Engine-only semantic output acknowledgement. Borrowed text, no terminal frames. */
    void (*observe)(void *, const char *, const char *, const char *);
    void *observe_opaque;
    uint64_t turn_generation;
    uint64_t input_received_ms;
    char label[SNAG_TERM_LABEL_BYTES];
    char submitted_label[SNAG_TERM_LABEL_BYTES];
    enum snag_render_view input_view;
    struct snag_irc_route input_route;
    struct snag_irc_conversation_target input_conversation, input_address_conversation;
    struct snag_irc_scopes input_scopes;
    uint32_t input_destination;
    struct snag_irc_target selection;
};

enum snag_ui_operation {
    SNAG_UI_HOST, SNAG_UI_HELP, SNAG_UI_RUNTIME, SNAG_UI_ERROR, SNAG_UI_WARNING,
    SNAG_UI_ROLLOUT_END, SNAG_UI_ROLLOUT_ABORT, SNAG_UI_CLOSE,
    SNAG_UI_LEVEL, SNAG_UI_COLOR, SNAG_UI_MARKDOWN, SNAG_UI_DESTINATIONS, SNAG_UI_IRC_NAMES,
    SNAG_UI_SELECT, SNAG_UI_ROUTE, SNAG_UI_CONVERSATION, SNAG_UI_CONVERSATION_SELECT,
    SNAG_UI_COMMANDS, SNAG_UI_PAUSE,
    SNAG_UI_OPEN, SNAG_UI_EXTERNAL, SNAG_UI_PROMPT, SNAG_UI_HOLD, SNAG_UI_SPINNERS,
    SNAG_UI_DRAFT, SNAG_UI_INSERT, SNAG_UI_AUDIO, SNAG_UI_CAPTION,
    SNAG_UI_VIEW, SNAG_UI_SUBMITTED, SNAG_UI_PUBLIC_BEGIN, SNAG_UI_PUBLIC, SNAG_UI_VALIDATE,
    SNAG_UI_ORIENTATION, SNAG_UI_HISTORY, SNAG_UI_IRC, SNAG_UI_DURABLE, SNAG_UI_EVENT,
    SNAG_UI_RESUME, SNAG_UI_PROTOCOL, SNAG_UI_TRANSPORT, SNAG_UI_RAW, SNAG_UI_HISTORY_SNAPSHOT,
    SNAG_UI_UPDATE, SNAG_UI_SESSION_START, SNAG_UI_SESSION_LISTEN, SNAG_UI_SESSION_CONTROL,
    SNAG_UI_SESSION_REBIND, SNAG_UI_SESSION_OFFER, SNAG_UI_SESSION_PROGRESS,
    SNAG_UI_SESSION_REFUSE, SNAG_UI_SESSION_RELEASED, SNAG_UI_SESSION_BOUND,
    SNAG_UI_SESSION_READY,
    SNAG_UI_INPUT, SNAG_UI_VOICE_EVENT, SNAG_UI_VIEW_STATE, SNAG_UI_VIEW_RESULT,
    SNAG_UI_COMMAND_RESULT, SNAG_UI_COMMAND_REPORT, SNAG_UI_STOP
};

struct snag_ui_prompt {
    char *source;
    bool active;
    uint32_t rate;
    unsigned int states, mode;
    char frames[SNAG_TERM_SPINNER_COUNT][80];
    char *values[SNAG_PROMPT_HOUR];
};

/* Borrowed command bytes remain valid until the synchronous call returns.
 * Prompt/history payloads transfer their retained ownership to the UI. */
struct snag_ui_command {
    enum snag_ui_operation kind;
    const char *text;
    const char *label;
    size_t len;
    union {
        unsigned int value;
        struct snag_session_process *session_process;
        struct { uint64_t generation; unsigned char *bytes; bool *present;
            unsigned int mode; } session_voice;
        const struct snag_session *session;
        struct snag_ui_prompt prompt;
        struct { uint32_t typing_pause_ms, tool_spinner_off_delay_ms; } timing;
        struct { int fd; enum snag_presentation kind; } public;
        struct { uint64_t turns; size_t queued; bool resumed, queue_armed; } orientation;
        struct { const struct snag_history_turn *turn; uint64_t shown, completed, total; } replay;
        const struct snag_irc_event *irc;
        const struct snag_irc_conversation_target *conversation;
        const json_t *voice;
        struct { int fd; struct snag_render_source source;
                 uint32_t timeout_ms, max_output_bytes; } durable;
        uint64_t seq;
        /* The immutable command catalog must outlive the UI. */
        struct { const struct snag_term_command *items; size_t count; } commands;
        struct { struct snag_history_snapshot entries; bool refresh; } history;
        const struct snag_irc_destinations *destinations;
        struct snag_irc_route *route;
    } data;
};

int snag_ui_send(struct snag_ui *ui, struct snag_ui_command command);

int snag_ui_init(struct snag_ui *ui);
/* Transfer owner descriptors to the presentation thread before opening input. */
int snag_ui_session_start(struct snag_ui *, struct snag_session_process *);
int snag_ui_session_listen(struct snag_ui *, const struct snag_session *);
int snag_ui_view_state(struct snag_ui *, const struct snag_session *);
int snag_ui_view_result(struct snag_ui *, const char *id, const char *status,
                        uint64_t seq, const char *event);
int snag_ui_command_result(struct snag_ui *, const json_t *);
int snag_ui_command_report(struct snag_ui *, const json_t *, const char *error);
int snag_ui_session_control(struct snag_ui *, enum snag_session_message, const void *, size_t);
uint64_t snag_ui_session_pending(const struct snag_ui *);
/* Current frontend-bound native attachment identity; zero until source drain
 * and binding finish, while absent/suspended, or using a direct terminal.
 * Changes also identify exclusive-transfer lease loss. */
uint64_t snag_ui_session_attachment(const struct snag_ui *);
uint64_t snag_ui_session_releasing(const struct snag_ui *);
uint64_t snag_ui_session_failures(const struct snag_ui *);
int snag_ui_session_rebind(struct snag_ui *, uint64_t generation);
int snag_ui_session_ready(struct snag_ui *, uint64_t generation);
int snag_ui_update(struct snag_ui *ui, const char *program, const char *url);
int snag_ui_set_verbosity(struct snag_ui *ui, unsigned int level);
unsigned int snag_ui_verbosity(const struct snag_ui *ui);
enum snag_render_view snag_ui_view(const struct snag_ui *ui);
bool snag_ui_enabled(const struct snag_ui *ui, enum snag_presentation kind);
void snag_ui_free(struct snag_ui *ui);
int snag_ui_text(struct snag_ui *ui, enum snag_ui_operation op, const char *text);
int snag_ui_capture_route(struct snag_ui *ui, const char *text);
uint32_t snag_ui_pause_remaining(struct snag_ui *ui);
int snag_ui_open(struct snag_ui *ui, char *error, size_t error_size);
int snag_ui_external(struct snag_ui *ui, bool begin, char *error, size_t error_size);
/* Resume the terminal input worker after external ownership; enqueue bytes
 * already received after the transfer's final EXIT before restarting it. */
int snag_ui_external_replay(struct snag_ui *ui, const unsigned char *tail, size_t length,
                            char *error, size_t error_size);
int snag_ui_hold(struct snag_ui *ui, bool active);
int snag_ui_prompt(struct snag_ui *ui, bool active, const char *label,
                    const char *const spinners[SNAG_TERM_SPINNER_COUNT],
                    uint32_t per_second, unsigned int states);
/* Non-NULL submitted renders a fresh immutable label without replacing the editor. */
int snag_ui_composer(struct snag_ui *ui, bool active, const char *format,
                    const char *const values[SNAG_PROMPT_HOUR], unsigned int mode,
                    const char *const spinners[SNAG_TERM_SPINNER_COUNT],
                    uint32_t per_second, unsigned int states, const char *submitted);
int snag_ui_validate_prompt(struct snag_ui *ui, const char *label,
                    const char *const spinners[SNAG_TERM_SPINNER_COUNT], uint32_t per_second);
int snag_ui_simple_prompt(struct snag_ui *ui, bool active);
bool snag_ui_leaving(const struct snag_ui *ui);
bool snag_ui_interrupt_pending(const struct snag_ui *ui);
bool snag_ui_yield_pending(const struct snag_ui *ui);
int snag_ui_insert_draft(struct snag_ui *, const char *);
int snag_ui_audio(struct snag_ui *, const char *, bool);
/* Acknowledged live-capture label, with normal keyboard submission enabled. */
int snag_ui_voice(struct snag_ui *, const char *);
int snag_ui_caption(struct snag_ui *, unsigned int speaker, const char *);
/* Admission to the same action queue as keyboard input, without replacing the
 * draft. A matching attachment (zero for a direct terminal) is required.
 * Success acknowledges admission only; the session owner executes the input. */
int snag_ui_input(struct snag_ui *, const char *, uint64_t attachment);
int snag_ui_poll(struct snag_ui *ui, int timeout_ms,
                 enum snag_term_action *action, char **text);
int snag_ui_submitted(struct snag_ui *ui, const char *label, const char *text,
                       bool input);
int snag_ui_public(struct snag_ui *ui, const char *text, size_t len,
                   struct snag_buf *delivered);
int snag_ui_orientation(struct snag_ui *ui, const struct snag_session *session,
                         bool resumed);
int snag_ui_history(struct snag_ui *ui, struct snag_session *session, uint64_t count);
int snag_ui_history_report(struct snag_ui *, struct snag_session *, uint64_t, struct snag_buf *);
int snag_ui_history_open(struct snag_ui *ui, const char *dotdir, const char *session_dir);
int snag_ui_history_add(struct snag_ui *ui, const char *text);
bool snag_ui_history_warning(struct snag_ui *ui);
snag_wake_fd snag_ui_wake_fd(const struct snag_ui *ui);
void snag_ui_signal(struct snag_ui *ui);

#endif
