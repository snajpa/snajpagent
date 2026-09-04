/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_TERM_H
#define SNAJPAGENT_TERM_H

#include "base.h"

#include <stdbool.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <termios.h>

#define SNJ_TERM_HISTORY_COUNT 100u
#define SNJ_TERM_HISTORY_BYTES (4u * 1024u * 1024u)
#define SNJ_TERM_LABEL_BYTES 512u
#define SNJ_TERM_SPINNER_COUNT 3u
#define SNJ_TERM_SPINNER_MARKER_BASE 0xfdu

enum snj_term_spinner_id {
    SNJ_TERM_SPINNER_GOAL,
    SNJ_TERM_SPINNER_PROVIDER,
    SNJ_TERM_SPINNER_TOOL
};

struct snj_term_spinner {
    const char *value;
    size_t label_offset;
    size_t frame_offset[16];
    unsigned char frame_len[16];
    unsigned char inactive_len;
    unsigned char frame_count;
    unsigned char current_len;
    bool present;
};

enum snj_term_action {
    SNJ_TERM_NONE,
    SNJ_TERM_SUBMIT,
    SNJ_TERM_QUEUE,
    SNJ_TERM_VIEW,
    SNJ_TERM_CANCEL,
    SNJ_TERM_INTERRUPT,
    SNJ_TERM_EXIT
};

struct snj_term_command {
    const char *syntax;
    const char *description;
};

struct snj_term {
    struct termios saved;
    struct sigaction saved_sigint;
    struct sigaction saved_sigwinch;
    struct snj_buf draft;
    struct snj_buf search_label;
    struct snj_buf search_query;
    char *history[SNJ_TERM_HISTORY_COUNT];
    char *history_draft;
    char *history_path;
    char *search_original;
    const struct snj_term_command *commands;
    size_t cursor;
    size_t command_count;
    size_t history_count;
    size_t history_bytes;
    size_t history_pos;
    size_t search_original_cursor;
    size_t search_pos;
    size_t rendered_rows;
    size_t rendered_cursor_row;
    size_t rendered_cursor_col;
    unsigned int columns;
    unsigned int output_depth;
    uint32_t typing_pause_ms;
    uint64_t last_input_ms;
    unsigned char utf8_pending[4];
    size_t utf8_pending_len;
    unsigned char escape[8];
    size_t escape_len;
    unsigned char input[256];
    size_t input_pos;
    size_t input_len;
    size_t paste_end_match;
    char label[SNJ_TERM_LABEL_BYTES];
    char prompt_template[SNJ_TERM_LABEL_BYTES];
    struct snj_term_spinner spinner[SNJ_TERM_SPINNER_COUNT];
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
    bool color;
    bool networked;
    bool rendered_end_at_margin;
    bool rendered_cursor_pending_wrap;
    bool searching;
    bool search_failed;
    bool history_warning;
    bool history_warned;
};

void snj_term_init(struct snj_term *term);
void snj_term_set_commands(struct snj_term *term,
                           const struct snj_term_command *commands,
                           size_t count);
int snj_term_open(struct snj_term *term, char *error, size_t error_size);
void snj_term_close(struct snj_term *term);
int snj_term_external_begin(struct snj_term *term,
                            char *error, size_t error_size);
int snj_term_external_end(struct snj_term *term,
                          char *error, size_t error_size);
int snj_term_set_prompt(struct snj_term *term, bool active);
int snj_term_set_prompt_label(struct snj_term *term, bool active,
                              const char *label);
int snj_term_set_prompt_template(struct snj_term *term, bool active,
                                 const char *label,
                                 const char *const spinners[SNJ_TERM_SPINNER_COUNT],
                                 uint32_t per_second, unsigned int states);
int snj_term_set_spinner_states(struct snj_term *term, unsigned int states);
const char *snj_term_prompt_label(const struct snj_term *term);
int snj_term_hide(struct snj_term *term);
int snj_term_show(struct snj_term *term);
int snj_term_output_begin(struct snj_term *term, bool persistent);
int snj_term_output_end(struct snj_term *term);
int snj_term_poll(struct snj_term *term, int timeout_ms,
                  enum snj_term_action *action, char **text);
int snj_term_history_open(struct snj_term *term, const char *dotdir);
int snj_term_history_add(struct snj_term *term, const char *text);
bool snj_term_take_history_warning(struct snj_term *term);
int snj_term_restore_draft(struct snj_term *term, const char *text);
void snj_term_set_typing_pause(struct snj_term *term, uint32_t pause_ms);
void snj_term_set_color(struct snj_term *term, bool enabled, bool networked);
uint32_t snj_term_typing_pause_remaining(const struct snj_term *term,
                                         uint64_t now_ms);
bool snj_term_typing_active(const struct snj_term *term);
void snj_term_note_output(struct snj_term *term, const char *text, size_t len);
unsigned int snj_term_columns(const struct snj_term *term);
size_t snj_term_text_width(const char *text, size_t len);
bool snj_term_consume_echoed_submission(struct snj_term *term,
                                        const char *label);
int snj_term_write_safe(int fd, const char *text, size_t len);

#endif
