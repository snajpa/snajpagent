/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_TERM_H
#define SNAJPAGENT_TERM_H
#include "wake.h"

#include "base.h"
#include "history.h"
#include "irc.h"
#include "term_host.h"

#include <stdbool.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define SNAG_TERM_SPINNER_COUNT 3u
#define SNAG_TERM_SPINNER_SLOTS 2u
#define SNAG_TERM_SPINNER_MARKER_BASE 0xfdu

enum snag_term_spinner_id {
    SNAG_TERM_SPINNER_GOAL,
    SNAG_TERM_SPINNER_PROVIDER,
    SNAG_TERM_SPINNER_TOOL
};

struct snag_prompt_clock {
    bool captured;
    bool valid;
    int hour;
    int minute;
    int second;
};

struct snag_term_spinner {
    char value[80];
    size_t frame_offset[16];
    unsigned char frame_len[16];
    unsigned char inactive_len;
    unsigned char frame_count;
};

enum snag_term_action {
    SNAG_TERM_NONE,
    SNAG_TERM_SUBMIT,
    SNAG_TERM_QUEUE,
    SNAG_TERM_VIEW,
    SNAG_TERM_CANCEL,
    SNAG_TERM_INTERRUPT,
    SNAG_TERM_EXIT
};

struct snag_term_command {
    const char *syntax;
    const char *description;
};

struct snag_term {
    int (*input_checkpoint)(void *);
    void *input_opaque;
    int output_fd[2];
    bool input_only, cancel_pending;
    struct snag_term_host host;
    struct snag_buf draft;
    struct snag_buf search_label;
    struct snag_buf search_query;
    struct snag_buf output_cell;
    struct snag_buf output_line;
    struct snag_buf painted_prompt;
    size_t painted_label_len;
    unsigned int painted_columns;
    bool painted_color;
    uint64_t last_output_ms;
    size_t output_columns;
    size_t output_cell_width;
    char output_cell_style[64u];
    struct snag_history_snapshot history;
    char *history_draft;
    char *search_original;
    char *nicks;
    struct snag_irc_destinations *destinations;
    struct snag_irc_target destination;
    const struct snag_term_command *commands;
    size_t cursor;
    size_t command_count;
    size_t history_pos;
    size_t search_original_cursor;
    size_t search_pos;
    size_t rendered_rows;
    size_t rendered_cursor_row;
    size_t rendered_cursor_col;
    unsigned int columns;
    unsigned int output_depth;
    bool defer_redraw;
    uint32_t typing_pause_ms;
    uint64_t last_input_ms;
    uint64_t ctrl_c_since_ms;
    unsigned int ctrl_c_count;
    unsigned char utf8_pending[4];
    size_t utf8_pending_len;
    unsigned char escape[8];
    size_t escape_len;
    unsigned char input[256];
    size_t input_pos;
    size_t input_len;
    size_t paste_end_match;
    char label[SNAG_TERM_LABEL_BYTES];
    char destination_label[SNAG_TERM_LABEL_BYTES + 128u];
    char prompt_template[SNAG_TERM_LABEL_BYTES];
    struct snag_prompt_clock prompt_clock;
    struct snag_term_spinner spinner[SNAG_TERM_SPINNER_COUNT];
    uint64_t spinner_epoch_ms;
    uint32_t spinner_per_second;
    unsigned int spinner_states;
    bool opened;
    bool raw;
    bool paste;
    bool bracketed_paste;
    bool prompt_wanted;
    bool prompt_visible;
    bool active;
    bool capable;
    bool sigint_installed;
    bool sigwinch_installed;
    bool line_submission_echoed;
    bool typing_active;
    bool output_seen;
    bool output_ended_lf;
    bool output_detour;
    bool color;
    bool chat;
    bool redraw_after_output;
    bool rendered_end_at_margin;
    bool rendered_cursor_pending_wrap;
    bool searching;
    bool search_failed;
    bool history_refresh_requested;
    bool input_backlog;
    bool local_backlog;
};

void snag_term_init(struct snag_term *term);
int snag_term_set_destinations(struct snag_term *term,
                              const struct snag_irc_destinations *destinations);
int snag_term_select_destination(struct snag_term *term, uint32_t id);
void snag_term_destination_prefix(const struct snag_term *term,
                                   char *out, size_t size);
void snag_term_destination_route(const struct snag_term *term,
                                 const char *text, struct snag_irc_route *route);
void snag_term_capture_prompt_clock(struct snag_term *term, time_t seconds);
void snag_term_set_commands(struct snag_term *term,
                           const struct snag_term_command *commands,
                           size_t count);
int snag_term_open(struct snag_term *term, char *error, size_t error_size);
void snag_term_close(struct snag_term *term);
int snag_term_external_begin(struct snag_term *term,
                            char *error, size_t error_size);
int snag_term_external_end(struct snag_term *term,
                          char *error, size_t error_size);
int snag_term_set_prompt_label(struct snag_term *term, bool active,
                              const char *label);
int snag_term_set_prompt_template(struct snag_term *term, bool active,
                                 const char *label,
                                 const char *const spinners[SNAG_TERM_SPINNER_COUNT],
                                 uint32_t per_second, unsigned int states);
int snag_term_set_spinner_states(struct snag_term *term, unsigned int states);
int snag_term_hide(struct snag_term *term);
int snag_term_output_begin(struct snag_term *term, bool persistent);
int snag_term_output_end(struct snag_term *term);
int snag_term_poll(struct snag_term *term, int timeout_ms, snag_wake_fd wake_fd,
                  enum snag_term_action *action, char **text);
int snag_term_history_set(struct snag_term *term,
                         struct snag_history_snapshot *snapshot, bool refresh);
int snag_term_restore_draft(struct snag_term *term, const char *text);
void snag_term_set_typing_pause(struct snag_term *term, uint32_t pause_ms);
void snag_term_set_color(struct snag_term *term, bool enabled);
uint32_t snag_term_typing_pause_remaining(const struct snag_term *term,
                                         uint64_t now_ms);
bool snag_term_typing_active(const struct snag_term *term);
int snag_term_note_output(struct snag_term *term, const char *text, size_t len,
                         const char *style);
unsigned int snag_term_columns(const struct snag_term *term);
size_t snag_term_text_width(const char *text, size_t len);
bool snag_term_consume_echoed_submission(struct snag_term *term,
                                        const char *label);
int snag_term_write_safe(int fd, const char *text, size_t len);
int snag_term_append_safe(struct snag_buf *out, const char *text, size_t len);
int snag_term_write(int fd, const void *text, size_t len);

#endif
