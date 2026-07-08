/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_TERM_H
#define SNAJPAGENT_TERM_H

#include "base.h"

#include <stdbool.h>
#include <signal.h>
#include <stddef.h>
#include <termios.h>

#define SNJ_TERM_HISTORY_COUNT 100u
#define SNJ_TERM_HISTORY_BYTES (4u * 1024u * 1024u)

enum snj_term_action {
    SNJ_TERM_NONE,
    SNJ_TERM_SUBMIT,
    SNJ_TERM_QUEUE,
    SNJ_TERM_INTERRUPT,
    SNJ_TERM_EXIT
};

struct snj_term {
    struct termios saved;
    struct sigaction saved_sigint;
    struct sigaction saved_sigwinch;
    struct snj_buf draft;
    char *history[SNJ_TERM_HISTORY_COUNT];
    char *history_draft;
    size_t cursor;
    size_t history_count;
    size_t history_bytes;
    size_t history_pos;
    size_t rendered_rows;
    size_t rendered_cursor_row;
    unsigned int columns;
    unsigned int output_depth;
    unsigned char utf8_pending[4];
    size_t utf8_pending_len;
    unsigned char escape[8];
    size_t escape_len;
    unsigned char input[256];
    size_t input_pos;
    size_t input_len;
    size_t paste_end_match;
    char label[16];
    char status[64];
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
};

void snj_term_init(struct snj_term *term);
int snj_term_open(struct snj_term *term, char *error, size_t error_size);
void snj_term_close(struct snj_term *term);
int snj_term_set_prompt(struct snj_term *term, bool active);
int snj_term_hide(struct snj_term *term);
int snj_term_show(struct snj_term *term);
int snj_term_output_begin(struct snj_term *term, bool persistent);
int snj_term_output_end(struct snj_term *term);
int snj_term_set_status(struct snj_term *term, const char *status);
void snj_term_clear_status(struct snj_term *term);
int snj_term_poll(struct snj_term *term, int timeout_ms,
                  enum snj_term_action *action, char **text);
int snj_term_history_add(struct snj_term *term, const char *text);
int snj_term_restore_draft(struct snj_term *term, const char *text);
bool snj_term_consume_echoed_submission(struct snj_term *term,
                                        const char *label);
int snj_term_write_safe(int fd, const char *text, size_t len);

#endif
