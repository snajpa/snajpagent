/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_RENDER_H
#define SNAJPAGENT_RENDER_H

#include "config.h"
#include "irc.h"
#include "store.h"
#include "term.h"

#include <stdbool.h>
#include <stddef.h>

#define SNJ_RENDER_IRC_MARKDOWN_STATES (SNJ_CONFIG_IRC_CLIENT_MAX + 1u)

struct snj_markdown_state {
    char prefix[16];
    char fence_info[64];
    size_t prefix_len;
    size_t fence_info_len;
    size_t delimiter_len;
    unsigned int fence_len;
    unsigned int code_ticks;
    char fence;
    char delimiter;
    bool line_start;
    bool fence_header;
    bool heading;
    bool quote;
    bool strong;
    bool emphasis;
    bool strike;
    bool inline_code;
    bool link_url;
    bool link_after_label;
    bool escape;
    bool previous_word;
    bool delimiter_previous_word;
    bool style_painted;
};

struct snj_irc_markdown_state {
    char endpoint[SNJ_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char nick[SNJ_CONFIG_IRC_NAME_MAX + 1u];
    char fence;
    unsigned int fence_len;
};

struct snj_render {
    unsigned int verbosity;
    bool stdout_terminal;
    bool stderr_terminal;
    int public_fd;
    bool public_item_open;
    bool public_output_open;
    bool public_item_bytes;
    bool public_item_ended_lf;
    bool stdout_item_seen;
    bool stdout_item_ended_lf;
    bool protocol_warning_shown;
    bool color_stdout;
    bool color_stderr;
    bool markdown;
    bool markdown_rendering;
    bool markdown_preserve_fence;
    bool networked;
    char agent_name[SNJ_CONFIG_IRC_NAME_MAX + 1u];
    struct snj_term *term;
    struct snj_buf wrap_pending;
    size_t public_column;
    bool wrap_has_word;
    bool wrap_continuation;
    bool wrap_word_open;
    unsigned char utf8_pending[4];
    size_t utf8_pending_len;
    struct snj_markdown_state markdown_state;
    struct snj_irc_markdown_state irc_markdown[SNJ_RENDER_IRC_MARKDOWN_STATES];
};

void snj_render_init(struct snj_render *render, unsigned int verbosity);
void snj_render_set_color(struct snj_render *render, enum snj_color_mode mode);
void snj_render_set_markdown(struct snj_render *render, bool enabled);
void snj_render_set_networked(struct snj_render *render, bool networked,
                              const char *agent_name);
void snj_render_attach_term(struct snj_render *render, struct snj_term *term);
int snj_render_orientation(struct snj_render *render,
                           const struct snj_session *session, bool resumed);
int snj_render_history(struct snj_render *render,
                       const struct snj_session *session);
int snj_render_prompt(struct snj_render *render, const char *label);
int snj_render_submitted(struct snj_render *render, const char *label,
                         const char *text);
int snj_render_public_begin(struct snj_render *render, int fd,
                            const char *label);
int snj_render_public(struct snj_render *render, const char *text, size_t len,
                      struct snj_buf *delivered);
int snj_render_public_end(struct snj_render *render);
int snj_render_public_abort(struct snj_render *render);
int snj_render_error(const char *message);
int snj_render_warning(const char *message);
int snj_render_error_ctx(struct snj_render *render, const char *message);
int snj_render_warning_ctx(struct snj_render *render, const char *message);
int snj_render_activity(struct snj_render *render, const char *message);
int snj_render_host(struct snj_render *render, const char *text);
int snj_render_runtime(struct snj_render *render, const char *text);
int snj_render_irc_event(struct snj_render *render,
                         const struct snj_irc_event *event);
int snj_render_tool_start(struct snj_render *render,
                          const struct snj_response_item *call,
                          const char *workdir);
int snj_render_tool_finish(struct snj_render *render, const char *name,
                           const json_t *result);
int snj_render_event(struct snj_render *render, uint64_t seq,
                     const char *type);
int snj_render_protocol(struct snj_render *render, const char *label,
                        const char *text, size_t len);
int snj_render_transport(struct snj_render *render, char direction,
                         const char *text, size_t len);

#endif
