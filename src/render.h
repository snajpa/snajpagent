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

enum snj_render_view {
    SNJ_RENDER_CHAT,
    SNJ_RENDER_ROLLOUT,
    SNJ_RENDER_VIEW_COUNT
};

enum snj_presentation {
    SNJ_PRESENT_CONVERSATION, SNJ_PRESENT_TOOL, SNJ_PRESENT_ARGUMENTS,
    SNJ_PRESENT_OUTPUT, SNJ_PRESENT_CONTEXT, SNJ_PRESENT_REASONING,
    SNJ_PRESENT_DEBUG, SNJ_PRESENT_PROTOCOL, SNJ_PRESENT_WIRE,
    SNJ_PRESENT_CHAT, SNJ_PRESENT_FEEDBACK
};

bool snj_presentation_enabled(enum snj_presentation kind, unsigned int level,
                              enum snj_render_view view);
size_t snj_presentation_limit(enum snj_presentation kind, unsigned int level);
const char *snj_verbosity_name(unsigned int level);

struct snj_render_record;

struct snj_render_source {
    off_t offset;
    size_t len;
};

struct snj_markdown_state {
    char prefix[16];
    char fence_info[64];
    struct snj_buf table;
    size_t prefix_len;
    size_t fence_info_len;
    size_t delimiter_len;
    size_t table_header_len;
    size_t table_line_start;
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
    bool table_header;
    bool link_url;
    bool link_after_label;
    bool escape;
    bool previous_word;
    bool delimiter_previous_word;
    bool style_painted;
    bool prose;
    bool table_line;
    bool table_pending;
    bool table_active;
    bool table_disabled;
};

struct snj_irc_markdown_state {
    char endpoint[SNJ_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char nick[SNJ_CONFIG_IRC_NICK_MAX + 1u];
    char fence;
    unsigned int fence_len;
};

struct snj_render {
    int (*checkpoint)(void *);
    void *checkpoint_opaque;
    unsigned int verbosity;
    bool suppress_optional;
    int history_fd;
    struct snj_render_source response_source;
    struct snj_render_source irc_source;
    bool stdout_terminal;
    bool stderr_terminal;
    int public_fd;
    bool public_item_open;
    bool public_output_open;
    bool public_item_bytes;
    bool public_item_ended_lf;
    unsigned int public_trailing_newlines;
    unsigned int previous_public_newlines;
    int previous_public_fd;
    bool previous_public_item;
    bool previous_public_markdown;
    bool stdout_item_seen;
    bool stdout_item_ended_lf;
    bool protocol_warning_shown;
    bool color_stdout;
    bool color_stderr;
    bool markdown;
    bool markdown_rendering;
    bool markdown_measuring;
    bool markdown_preserve_fence;
    bool markdown_prose_bullets;
    bool networked;
    enum snj_render_view view;
    char model_nick[SNJ_CONFIG_IRC_NICK_MAX + 1u];
    struct snj_term *term;
    struct snj_render_record *view_head[SNJ_RENDER_VIEW_COUNT];
    struct snj_render_record *view_tail[SNJ_RENDER_VIEW_COUNT];
    struct snj_render_record *rollout_open;
    struct snj_buf wrap_pending;
    size_t public_column;
    char public_style[64u];
    bool wrap_has_word;
    bool wrap_continuation;
    bool wrap_word_open;
    bool wrap_break_open;
    unsigned char utf8_pending[4];
    size_t utf8_pending_len;
    struct snj_markdown_state markdown_state;
    struct snj_irc_markdown_state irc_markdown[SNJ_RENDER_IRC_MARKDOWN_STATES];
};

void snj_render_init(struct snj_render *render, unsigned int verbosity);
bool snj_render_enabled(const struct snj_render *render,
                         enum snj_presentation kind);
void snj_render_free(struct snj_render *render);
void snj_render_set_color(struct snj_render *render, enum snj_color_mode mode);
void snj_render_set_markdown(struct snj_render *render, bool enabled);
void snj_render_set_networked(struct snj_render *render, bool networked,
                              const char *model_nick);
void snj_render_attach_term(struct snj_render *render, struct snj_term *term);
enum snj_render_view snj_render_view(const struct snj_render *render);
int snj_render_set_view(struct snj_render *render, enum snj_render_view view);
int snj_render_orientation(struct snj_render *render,
                           const char *workspace, const char *id,
                           uint64_t turns, size_t queued, bool resumed);
int snj_render_history(struct snj_render *render,
                       const char *user, const char *assistant);
int snj_render_prompt(struct snj_render *render, const char *label);
int snj_render_submitted(struct snj_render *render, const char *label,
                         const char *text);
int snj_render_input_submitted(struct snj_render *render, const char *label,
                               const char *text);
int snj_render_before_prompt(struct snj_render *render);
int snj_render_public_begin(struct snj_render *render, int fd,
                            const char *label);
int snj_render_public(struct snj_render *render, const char *text, size_t len,
                      struct snj_buf *delivered);
int snj_render_public_end(struct snj_render *render);
int snj_render_public_abort(struct snj_render *render);
int snj_render_rollout_begin(struct snj_render *render, int fd,
                             const char *label, enum snj_presentation kind);
int snj_render_rollout(struct snj_render *render, const char *text, size_t len,
                       struct snj_buf *delivered);
int snj_render_rollout_end(struct snj_render *render);
int snj_render_rollout_abort(struct snj_render *render);
int snj_render_error(const char *message);
int snj_render_warning(const char *message);
int snj_render_error_ctx(struct snj_render *render, const char *message);
int snj_render_warning_ctx(struct snj_render *render, const char *message);
int snj_render_host(struct snj_render *render, const char *text);
int snj_render_runtime(struct snj_render *render, const char *text);
int snj_render_irc_event(struct snj_render *render,
                         const struct snj_irc_event *event);
enum snj_render_role {
    SNJ_ROLE_ACTIVITY, SNJ_ROLE_SUCCESS, SNJ_ROLE_WARNING, SNJ_ROLE_ERROR
};

struct snj_render_block {
    struct snj_buf text;
    struct snj_buf context;
    struct snj_buf body;
    size_t colored_len;
    enum snj_render_role role;
    enum snj_presentation body_kind;
    bool truncated;
};

/* Pure formatting of UI-owned values; no terminal writes. */
int snj_render_prepare_tool_start(struct snj_render_block *block,
                          const struct snj_response_item *call,
                          const char *workdir, uint32_t default_timeout_ms,
                          unsigned int level, unsigned int columns);
int snj_render_prepare_tool_finish(struct snj_render_block *block, const char *name,
                           const json_t *result, uint32_t max_output_bytes,
                           unsigned int level, unsigned int columns);
void snj_render_block_free(struct snj_render_block *block);
int snj_render_durable(struct snj_render *render, int fd,
                        struct snj_render_source source, const char *type,
                        uint32_t timeout_ms, uint32_t max_output_bytes);
int snj_render_tool_block(struct snj_render *render,
                          const struct snj_render_block *block);
int snj_render_event(struct snj_render *render, uint64_t seq,
                     const char *type);
int snj_render_resume_hint(const struct snj_render *render,
                           const char *command, size_t command_len);
int snj_render_protocol(struct snj_render *render, const char *label,
                        const char *text, size_t len);
int snj_render_transport(struct snj_render *render, char direction,
                         const char *text, size_t len);

#endif
