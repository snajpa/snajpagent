/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_RENDER_H
#define SNAJPAGENT_RENDER_H

#include "config.h"
#include "irc.h"
#include "store.h"
#include "term.h"

#include <stdbool.h>
#include <stddef.h>

#define SNAG_RENDER_IRC_MARKDOWN_STATES (SNAG_CONFIG_IRC_CLIENT_MAX + 1u)

enum snag_render_view {
    SNAG_RENDER_CHAT,
    SNAG_RENDER_ROLLOUT,
    SNAG_RENDER_VIEW_COUNT
};

enum snag_presentation {
    SNAG_PRESENT_CONVERSATION, SNAG_PRESENT_TOOL, SNAG_PRESENT_ARGUMENTS,
    SNAG_PRESENT_OUTPUT, SNAG_PRESENT_CONTEXT, SNAG_PRESENT_REASONING,
    SNAG_PRESENT_DEBUG, SNAG_PRESENT_PROTOCOL, SNAG_PRESENT_WIRE,
    SNAG_PRESENT_CHAT, SNAG_PRESENT_FEEDBACK
};

bool snag_presentation_enabled(enum snag_presentation kind, unsigned int level,
                              enum snag_render_view view);
size_t snag_presentation_limit(enum snag_presentation kind, unsigned int level);
const char *snag_verbosity_name(unsigned int level);

struct snag_render_record;

struct snag_render_source {
    off_t offset;
    size_t len;
};

struct snag_markdown_state {
    char prefix[16];
    char fence_info[64];
    struct snag_buf table;
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

struct snag_irc_markdown_state {
    char endpoint[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char nick[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    char fence;
    unsigned int fence_len;
};

struct snag_render {
    int (*checkpoint)(void *);
    void *checkpoint_opaque;
    unsigned int verbosity;
    bool suppress_optional;
    int history_fd;
    struct snag_render_source response_source;
    struct snag_render_source irc_source;
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
    enum snag_render_view view;
    char model_nick[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    struct snag_term *term;
    struct snag_render_record *view_head[SNAG_RENDER_VIEW_COUNT];
    struct snag_render_record *view_tail[SNAG_RENDER_VIEW_COUNT];
    struct snag_render_record *rollout_open;
    struct snag_buf wrap_pending;
    size_t public_column;
    char public_style[64u];
    bool wrap_has_word;
    bool wrap_continuation;
    bool wrap_word_open;
    bool wrap_break_open;
    unsigned char utf8_pending[4];
    size_t utf8_pending_len;
    struct snag_markdown_state markdown_state;
    struct snag_irc_markdown_state irc_markdown[SNAG_RENDER_IRC_MARKDOWN_STATES];
};

void snag_render_init(struct snag_render *render, unsigned int verbosity);
bool snag_render_enabled(const struct snag_render *render,
                         enum snag_presentation kind);
void snag_render_free(struct snag_render *render);
void snag_render_set_color(struct snag_render *render, enum snag_color_mode mode);
void snag_render_set_markdown(struct snag_render *render, bool enabled);
void snag_render_set_networked(struct snag_render *render, bool networked,
                              const char *model_nick);
void snag_render_attach_term(struct snag_render *render, struct snag_term *term);
enum snag_render_view snag_render_view(const struct snag_render *render);
int snag_render_set_view(struct snag_render *render, enum snag_render_view view);
int snag_render_orientation(struct snag_render *render,
                           const char *workspace, const char *id,
                           uint64_t turns, size_t queued, bool resumed);
int snag_render_history(struct snag_render *render,
                       const char *user, const char *assistant);
int snag_render_prompt(struct snag_render *render, const char *label);
int snag_render_submitted(struct snag_render *render, const char *label,
                         const char *text);
int snag_render_input_submitted(struct snag_render *render, const char *label,
                               const char *text);
int snag_render_before_prompt(struct snag_render *render);
int snag_render_public_begin(struct snag_render *render, int fd,
                            const char *label);
int snag_render_public(struct snag_render *render, const char *text, size_t len,
                      struct snag_buf *delivered);
int snag_render_public_end(struct snag_render *render);
int snag_render_public_abort(struct snag_render *render);
int snag_render_rollout_begin(struct snag_render *render, int fd,
                             const char *label, enum snag_presentation kind);
int snag_render_rollout(struct snag_render *render, const char *text, size_t len,
                       struct snag_buf *delivered);
int snag_render_rollout_end(struct snag_render *render);
int snag_render_rollout_abort(struct snag_render *render);
int snag_render_error(const char *message);
int snag_render_warning(const char *message);
int snag_render_error_ctx(struct snag_render *render, const char *message);
int snag_render_warning_ctx(struct snag_render *render, const char *message);
int snag_render_host(struct snag_render *render, const char *text);
int snag_render_runtime(struct snag_render *render, const char *text);
int snag_render_irc_event(struct snag_render *render,
                         const struct snag_irc_event *event);
enum snag_render_role {
    SNAG_ROLE_ACTIVITY, SNAG_ROLE_SUCCESS, SNAG_ROLE_WARNING, SNAG_ROLE_ERROR
};

struct snag_render_block {
    struct snag_buf text;
    struct snag_buf context;
    struct snag_buf body;
    size_t colored_len;
    enum snag_render_role role;
    enum snag_presentation body_kind;
    bool truncated;
};

/* Pure formatting of UI-owned values; no terminal writes. */
int snag_render_prepare_tool_start(struct snag_render_block *block,
                          const struct snag_response_item *call,
                          const char *workdir, uint32_t default_timeout_ms,
                          unsigned int level, unsigned int columns);
int snag_render_prepare_tool_finish(struct snag_render_block *block, const char *name,
                           const json_t *result, uint32_t max_output_bytes,
                           unsigned int level, unsigned int columns);
void snag_render_block_free(struct snag_render_block *block);
int snag_render_durable(struct snag_render *render, int fd,
                        struct snag_render_source source, const char *type,
                        uint32_t timeout_ms, uint32_t max_output_bytes);
int snag_render_tool_block(struct snag_render *render,
                          const struct snag_render_block *block);
int snag_render_event(struct snag_render *render, uint64_t seq,
                     const char *type);
int snag_render_resume_hint(const struct snag_render *render,
                           const char *command, size_t command_len);
int snag_render_protocol(struct snag_render *render, const char *label,
                        const char *text, size_t len);
int snag_render_transport(struct snag_render *render, char direction,
                         const char *text, size_t len);

#endif
