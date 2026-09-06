/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_UI_H
#define SNAJPAGENT_UI_H

#include "render.h"

/* Engine-owned preferences and last acknowledged input state, not a renderer. */
struct snag_ui {
    struct snag_ui_runtime *runtime;
    struct snag_history history;
    enum snag_render_view view;
    bool opened;
    bool prompt_wanted;
    bool active;
    uint64_t turn_generation;
    char label[SNAG_TERM_LABEL_BYTES];
    char submitted_label[SNAG_TERM_LABEL_BYTES];
    enum snag_render_view input_view;
    struct snag_irc_route input_route;
    struct snag_irc_target selection;
};

enum snag_ui_operation {
    SNAG_UI_HOST, SNAG_UI_RUNTIME, SNAG_UI_ERROR, SNAG_UI_WARNING,
    SNAG_UI_ROLLOUT_END, SNAG_UI_ROLLOUT_ABORT,
    SNAG_UI_CLOSE
};

int snag_ui_init(struct snag_ui *ui);
int snag_ui_set_verbosity(struct snag_ui *ui, unsigned int level);
unsigned int snag_ui_verbosity(const struct snag_ui *ui);
bool snag_ui_enabled(const struct snag_ui *ui, enum snag_presentation kind);
void snag_ui_free(struct snag_ui *ui);
int snag_ui_text(struct snag_ui *ui, enum snag_ui_operation op, const char *text);
int snag_ui_color(struct snag_ui *ui, enum snag_color_mode mode);
int snag_ui_markdown(struct snag_ui *ui, bool enabled);
int snag_ui_model_nick(struct snag_ui *ui, const char *nick);
int snag_ui_nicks(struct snag_ui *ui, const char *nicks);
int snag_ui_destinations(struct snag_ui *ui,
                          const struct snag_irc_destinations *destinations);
int snag_ui_select_destination(struct snag_ui *ui, uint32_t id);
int snag_ui_capture_route(struct snag_ui *ui, const char *text);
/* The immutable command catalog must outlive the UI (the app uses static data). */
int snag_ui_commands(struct snag_ui *ui, const struct snag_term_command *commands,
                      size_t count);
int snag_ui_typing_pause(struct snag_ui *ui, uint32_t ms);
uint32_t snag_ui_pause_remaining(struct snag_ui *ui);
int snag_ui_open(struct snag_ui *ui, char *error, size_t error_size);
int snag_ui_external(struct snag_ui *ui, bool begin,
                      char *error, size_t error_size);
int snag_ui_prompt(struct snag_ui *ui, bool active, const char *label,
                    const char *const spinners[SNAG_TERM_SPINNER_COUNT],
                    uint32_t per_second, unsigned int states);
int snag_ui_composer(struct snag_ui *ui, bool active, const char *format,
                    const char *const values[SNAG_PROMPT_HOUR], unsigned int mode,
                    const char *const spinners[SNAG_TERM_SPINNER_COUNT],
                    uint32_t per_second, unsigned int states);
int snag_ui_validate_prompt(struct snag_ui *ui, const char *label,
                    const char *const spinners[SNAG_TERM_SPINNER_COUNT],
                    uint32_t per_second);
int snag_ui_simple_prompt(struct snag_ui *ui, bool active);
int snag_ui_spinner_states(struct snag_ui *ui, unsigned int states);
int snag_ui_restore_draft(struct snag_ui *ui, const char *text);
int snag_ui_poll(struct snag_ui *ui, int timeout_ms,
                  bool active, enum snag_term_action *action, char **text);
int snag_ui_set_view(struct snag_ui *ui, enum snag_render_view view);
int snag_ui_submitted(struct snag_ui *ui, const char *label, const char *text,
                       bool input);
int snag_ui_public_begin(struct snag_ui *ui, int fd, const char *label,
                        enum snag_presentation kind);
int snag_ui_public(struct snag_ui *ui, const char *text, size_t len,
                   struct snag_buf *delivered);
int snag_ui_orientation(struct snag_ui *ui, const struct snag_session *session,
                         bool resumed);
int snag_ui_history(struct snag_ui *ui, const struct snag_session *session);
int snag_ui_irc_event(struct snag_ui *ui, const struct snag_irc_event *event);
int snag_ui_durable(struct snag_ui *ui, int fd, struct snag_render_source source,
                    const char *type, uint32_t timeout_ms, uint32_t max_output_bytes);
int snag_ui_event(struct snag_ui *ui, uint64_t seq, const char *type);
int snag_ui_resume_hint(struct snag_ui *ui, const char *text, size_t len);
int snag_ui_protocol(struct snag_ui *ui, const char *label,
                      const char *text, size_t len);
int snag_ui_transport(struct snag_ui *ui, char direction,
                       const char *text, size_t len);
int snag_ui_raw(struct snag_ui *ui, int fd, const char *text, size_t len);
int snag_ui_history_open(struct snag_ui *ui, const char *dotdir);
int snag_ui_history_add(struct snag_ui *ui, const char *text);
bool snag_ui_history_warning(struct snag_ui *ui);
int snag_ui_wake_fd(const struct snag_ui *ui);
void snag_ui_signal(struct snag_ui *ui);

#endif
