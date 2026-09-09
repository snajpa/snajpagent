/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_UI_H
#define SNAJPAGENT_UI_H
#include "wake.h"

#include "render.h"

/* Engine-owned preferences and last acknowledged input state, not a renderer. */
struct snag_ui {
    struct snag_ui_runtime *runtime;
    struct snag_history history;
    enum snag_render_view view;
    bool opened;
    bool prompt_wanted;
    bool active, input_active;
    uint64_t turn_generation;
    uint64_t input_received_ms;
    char label[SNAG_TERM_LABEL_BYTES];
    char submitted_label[SNAG_TERM_LABEL_BYTES];
    enum snag_render_view input_view;
    struct snag_irc_route input_route;
    struct snag_irc_target selection;
};

enum snag_ui_operation {
    SNAG_UI_HOST, SNAG_UI_HELP, SNAG_UI_RUNTIME, SNAG_UI_ERROR, SNAG_UI_WARNING,
    SNAG_UI_ROLLOUT_END, SNAG_UI_ROLLOUT_ABORT, SNAG_UI_CLOSE,
    SNAG_UI_LEVEL, SNAG_UI_COLOR, SNAG_UI_MARKDOWN, SNAG_UI_DESTINATIONS,
    SNAG_UI_SELECT, SNAG_UI_ROUTE, SNAG_UI_COMMANDS, SNAG_UI_PAUSE,
    SNAG_UI_OPEN, SNAG_UI_EXTERNAL, SNAG_UI_PROMPT, SNAG_UI_SPINNERS, SNAG_UI_DRAFT,
    SNAG_UI_VIEW, SNAG_UI_SUBMITTED, SNAG_UI_PUBLIC_BEGIN, SNAG_UI_PUBLIC, SNAG_UI_VALIDATE,
    SNAG_UI_ORIENTATION, SNAG_UI_HISTORY, SNAG_UI_IRC, SNAG_UI_DURABLE, SNAG_UI_EVENT,
    SNAG_UI_RESUME, SNAG_UI_PROTOCOL, SNAG_UI_TRANSPORT, SNAG_UI_RAW, SNAG_UI_HISTORY_SNAPSHOT, SNAG_UI_UPDATE, SNAG_UI_STOP
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
        struct snag_ui_prompt prompt;
        struct { uint32_t typing_pause_ms, tool_spinner_off_delay_ms; } timing;
        struct { int fd; enum snag_presentation kind; } public;
        struct { uint64_t turns; size_t queued; bool resumed, queue_armed; } orientation;
        struct { const struct snag_history_turn *turn; uint64_t shown, completed, total; } replay;
        const struct snag_irc_event *irc;
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
int snag_ui_update(struct snag_ui *ui, const char *program, const char *url);
int snag_ui_set_verbosity(struct snag_ui *ui, unsigned int level);
unsigned int snag_ui_verbosity(const struct snag_ui *ui);
bool snag_ui_enabled(const struct snag_ui *ui, enum snag_presentation kind);
void snag_ui_free(struct snag_ui *ui);
int snag_ui_text(struct snag_ui *ui, enum snag_ui_operation op, const char *text);
int snag_ui_capture_route(struct snag_ui *ui, const char *text);
uint32_t snag_ui_pause_remaining(struct snag_ui *ui);
int snag_ui_open(struct snag_ui *ui, char *error, size_t error_size);
int snag_ui_external(struct snag_ui *ui, bool begin,
                      char *error, size_t error_size);
int snag_ui_prompt(struct snag_ui *ui, bool active, const char *label,
                    const char *const spinners[SNAG_TERM_SPINNER_COUNT],
                    uint32_t per_second, unsigned int states);
/* Non-NULL submitted renders a fresh immutable label without replacing the editor. */
int snag_ui_composer(struct snag_ui *ui, bool active, const char *format,
                    const char *const values[SNAG_PROMPT_HOUR], unsigned int mode,
                    const char *const spinners[SNAG_TERM_SPINNER_COUNT],
                    uint32_t per_second, unsigned int states, const char *submitted);
int snag_ui_validate_prompt(struct snag_ui *ui, const char *label,
                    const char *const spinners[SNAG_TERM_SPINNER_COUNT],
                    uint32_t per_second);
int snag_ui_simple_prompt(struct snag_ui *ui, bool active);
bool snag_ui_leaving(const struct snag_ui *ui);
int snag_ui_poll(struct snag_ui *ui, int timeout_ms,
                 enum snag_term_action *action, char **text);
int snag_ui_submitted(struct snag_ui *ui, const char *label, const char *text,
                       bool input);
int snag_ui_public(struct snag_ui *ui, const char *text, size_t len,
                   struct snag_buf *delivered);
int snag_ui_orientation(struct snag_ui *ui, const struct snag_session *session,
                         bool resumed);
int snag_ui_history(struct snag_ui *ui, struct snag_session *session, uint64_t count);
int snag_ui_history_open(struct snag_ui *ui, const char *dotdir, const char *session_dir);
int snag_ui_history_add(struct snag_ui *ui, const char *text);
bool snag_ui_history_warning(struct snag_ui *ui);
snag_wake_fd snag_ui_wake_fd(const struct snag_ui *ui);
void snag_ui_signal(struct snag_ui *ui);

#endif
