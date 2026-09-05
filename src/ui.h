/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_UI_H
#define SNAJPAGENT_UI_H

#include "render.h"

/* Engine-owned preferences and last acknowledged input state, not a renderer. */
struct snj_ui {
    struct snj_ui_runtime *runtime;
    struct snj_history history;
    enum snj_render_view view;
    bool opened;
    bool prompt_wanted;
    bool active;
    uint64_t turn_generation;
    char label[SNJ_TERM_LABEL_BYTES];
    char submitted_label[SNJ_TERM_LABEL_BYTES];
    enum snj_render_view input_view;
};

enum snj_ui_operation {
    SNJ_UI_HOST, SNJ_UI_RUNTIME, SNJ_UI_ERROR, SNJ_UI_WARNING,
    SNJ_UI_ROLLOUT_END, SNJ_UI_ROLLOUT_ABORT,
    SNJ_UI_CLOSE
};

int snj_ui_init(struct snj_ui *ui);
int snj_ui_set_verbosity(struct snj_ui *ui, unsigned int level);
unsigned int snj_ui_verbosity(const struct snj_ui *ui);
bool snj_ui_enabled(const struct snj_ui *ui, enum snj_presentation kind);
void snj_ui_free(struct snj_ui *ui);
int snj_ui_text(struct snj_ui *ui, enum snj_ui_operation op, const char *text);
int snj_ui_color(struct snj_ui *ui, enum snj_color_mode mode);
int snj_ui_markdown(struct snj_ui *ui, bool enabled);
int snj_ui_networked(struct snj_ui *ui, bool enabled, const char *nick);
int snj_ui_nicks(struct snj_ui *ui, const char *nicks);
int snj_ui_commands(struct snj_ui *ui, const struct snj_term_command *commands,
                      size_t count);
int snj_ui_typing_pause(struct snj_ui *ui, uint32_t ms);
uint32_t snj_ui_pause_remaining(struct snj_ui *ui);
int snj_ui_open(struct snj_ui *ui, char *error, size_t error_size);
int snj_ui_external(struct snj_ui *ui, bool begin,
                      char *error, size_t error_size);
int snj_ui_prompt(struct snj_ui *ui, bool active, const char *label,
                    const char *const spinners[SNJ_TERM_SPINNER_COUNT],
                    uint32_t per_second, unsigned int states);
int snj_ui_composer(struct snj_ui *ui, bool active, const char *format,
                    const char *const values[SNJ_PROMPT_HOUR], unsigned int mode,
                    const char *const spinners[SNJ_TERM_SPINNER_COUNT],
                    uint32_t per_second, unsigned int states);
int snj_ui_validate_prompt(struct snj_ui *ui, const char *label,
                    const char *const spinners[SNJ_TERM_SPINNER_COUNT],
                    uint32_t per_second);
int snj_ui_simple_prompt(struct snj_ui *ui, bool active);
int snj_ui_spinner_states(struct snj_ui *ui, unsigned int states);
int snj_ui_restore_draft(struct snj_ui *ui, const char *text);
int snj_ui_poll(struct snj_ui *ui, int timeout_ms,
                  bool active, enum snj_term_action *action, char **text);
int snj_ui_set_view(struct snj_ui *ui, enum snj_render_view view);
int snj_ui_submitted(struct snj_ui *ui, const char *label, const char *text,
                       bool input);
int snj_ui_public_begin(struct snj_ui *ui, int fd, const char *label,
                        enum snj_presentation kind);
int snj_ui_public(struct snj_ui *ui, const char *text, size_t len,
                   struct snj_buf *delivered);
int snj_ui_orientation(struct snj_ui *ui, const struct snj_session *session,
                         bool resumed);
int snj_ui_history(struct snj_ui *ui, const struct snj_session *session);
int snj_ui_irc_event(struct snj_ui *ui, const struct snj_irc_event *event);
int snj_ui_durable(struct snj_ui *ui, int fd, struct snj_render_source source,
                    const char *type, uint32_t timeout_ms, uint32_t max_output_bytes);
int snj_ui_event(struct snj_ui *ui, uint64_t seq, const char *type);
int snj_ui_resume_hint(struct snj_ui *ui, const char *text, size_t len);
int snj_ui_protocol(struct snj_ui *ui, const char *label,
                      const char *text, size_t len);
int snj_ui_transport(struct snj_ui *ui, char direction,
                       const char *text, size_t len);
int snj_ui_raw(struct snj_ui *ui, int fd, const char *text, size_t len);
int snj_ui_history_open(struct snj_ui *ui, const char *dotdir);
int snj_ui_history_add(struct snj_ui *ui, const char *text);
bool snj_ui_history_warning(struct snj_ui *ui);
int snj_ui_wake_fd(const struct snj_ui *ui);
void snj_ui_signal(struct snj_ui *ui);

#endif
