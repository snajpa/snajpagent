/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_RENDER_H
#define SNAJPAGENT_RENDER_H

#include "config.h"
#include "irc.h"
#include "store.h"
#include "store_binary.h"
#include "term.h"

#include <stdbool.h>
#include <stddef.h>

#define SNAG_RENDER_IRC_MARKDOWN_STATES (SNAG_CONFIG_IRC_CLIENT_MAX + 1u)

enum snag_render_view { SNAG_RENDER_CHAT, SNAG_RENDER_ROLLOUT, SNAG_RENDER_VIEW_COUNT };

struct snag_history_turn {
    char *user, *assistant;
    const char *status;
    bool timer, partial, continuation;
};

enum snag_presentation {
    SNAG_PRESENT_CONVERSATION,
    SNAG_PRESENT_TOOL,
    SNAG_PRESENT_ARGUMENTS,
    SNAG_PRESENT_OUTPUT,
    SNAG_PRESENT_CONTEXT,
    SNAG_PRESENT_DEBUG,
    SNAG_PRESENT_PROTOCOL,
    SNAG_PRESENT_WIRE,
    SNAG_PRESENT_CHAT,
    SNAG_PRESENT_FEEDBACK
};

bool snag_presentation_enabled(
    enum snag_presentation kind, unsigned int level, enum snag_render_view view);
size_t snag_presentation_limit(enum snag_presentation kind, unsigned int level);
const char *snag_verbosity_name(unsigned int level);
/* Parse /verbose and format the common feedback; invalid input preserves level. */
bool snag_verbosity_apply(const char *, unsigned int *, enum snag_render_view, char *, size_t);

struct snag_render_record;

/* The terminal and workspace sinks share these presentation attributes. */
enum snag_render_style {
    SNAG_RENDER_BOLD = 1u,
    SNAG_RENDER_DIM = 2u,
    SNAG_RENDER_UNDERLINE = 4u,
    SNAG_RENDER_REVERSE = 8u,
    SNAG_RENDER_ITALIC = 256u
};

struct snag_render_origin {
    const unsigned char *text;
    size_t len;
    uint64_t byte; /* UINT64_MAX marks generated decoration. */
    const uint64_t *map;
};

struct snag_render_sink {
    /* Inert text, style, original byte and original length. ANSI foreground
     * 1..8 occupies bits4..7, background1..8 bits9..12; zero means default. */
    int (*text)(void *, const char *, size_t, unsigned int, uint64_t, size_t);
    struct snag_render_origin source;
    void *opaque;
    unsigned int columns, style[2];
    bool logical;
};

struct snag_render_source {
    int64_t offset;
    size_t len;
    /* Native presentation pins a canonical sequence beneath its commit boundary. */
    uint64_t native_sequence;
    struct snag_binary_anchor native_boundary;
};

struct snag_markdown_state {
    char prefix[16];
    char fence_info[64];
    struct snag_buf table;
    uint64_t prefix_source, fence_source, table_source;
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
    bool prose;
    bool line_continuation;
    bool table_line;
    bool table_pending;
    bool table_active;
    bool table_disabled;
};

struct snag_irc_markdown_state {
    char endpoint[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char nick[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    char connection[SNAG_ID_HEX_LEN + 1u];
    char conversation[SNAG_ID_HEX_LEN + 1u];
    enum snag_irc_identity identity;
    char fence;
    unsigned int fence_len;
};

/* A provider citation block (U+E200 … U+E201) held until it can be rewritten
 * as one compact reference. Buffered bytes never exceed the block limit. */
#define SNAG_CITE_BLOCK_MAX 1024u

struct snag_cite_state {
    struct snag_buf pending;
    uint64_t source;
    bool active;
};

struct snag_render {
    struct snag_render_sink sink;
    struct snag_render_origin origin;
    int (*checkpoint)(void *);
    void *checkpoint_opaque;
    /* A read-only presentation consumer sanitizes durable data before markup. */
    json_t *(*filter_event)(void *, const json_t *);
    void *filter_opaque;
    unsigned int verbosity;
    bool suppress_optional;
    bool suspended;
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
    unsigned int trailing_newlines;
    unsigned int boundary;
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
    enum snag_render_view view;
    struct snag_term *term;
    struct snag_render_record *view_head[SNAG_RENDER_VIEW_COUNT];
    struct snag_render_record *view_tail[SNAG_RENDER_VIEW_COUNT];
    struct snag_render_record *rollout_open;
    void *chat_rooms;
    char chat_endpoint[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char chat_room[SNAG_CONFIG_IRC_ROOM_MAX + 2u];
    char chat_conversation[SNAG_ID_HEX_LEN + 1u];
    char chat_peer[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    enum snag_irc_identity chat_identity;
    void *backfill;
    struct snag_buf wrap_pending;
    struct snag_buf wrap_styles;
    struct snag_buf wrap_origins;
    size_t wrap_width;
    size_t public_column;
    char public_style[64u];
    bool wrap_has_word;
    bool wrap_continuation;
    unsigned char utf8_pending[4];
    size_t utf8_pending_len;
    struct snag_cite_state cite;
    struct snag_markdown_state markdown_state;
    struct snag_irc_markdown_state irc_markdown[SNAG_RENDER_IRC_MARKDOWN_STATES];
};

void snag_render_init(struct snag_render *render, unsigned int verbosity);
int snag_render_suspend(struct snag_render *, bool);
bool snag_render_enabled(const struct snag_render *render, enum snag_presentation kind);
void snag_render_free(struct snag_render *render);
void snag_render_set_color(struct snag_render *render, enum snag_color_mode mode);
void snag_render_set_markdown(struct snag_render *render, bool enabled);
void snag_render_attach_term(struct snag_render *render, struct snag_term *term);
/* End the physical block on the old, quiescent endpoint without completing
 * its semantic stream. The caller discards that endpoint's output afterward. */
int snag_render_rebind(struct snag_render *render);
enum snag_render_view snag_render_view(const struct snag_render *render);
int snag_render_set_view(struct snag_render *render, enum snag_render_view view);
int snag_render_set_chat_room(
    struct snag_render *render, const char *endpoint, const char *room, bool announce);
int snag_render_set_chat_conversation(
    struct snag_render *, const char *, const struct snag_irc_conversation_target *, bool);
bool snag_render_view_pending(const struct snag_render *render);
bool snag_render_view_runnable(const struct snag_render *render);
int snag_render_flush_pending(struct snag_render *render, size_t records);
int snag_render_backfill_start(struct snag_render *render, snag_wake_fd notify);
int snag_render_backfill_collect(struct snag_render *render);
int snag_render_orientation(struct snag_render *render, const char *cwd, const char *id,
    uint64_t turns, size_t queued, bool resumed, bool queue_armed);
int snag_render_history(struct snag_render *render, const struct snag_history_turn *turn,
    uint64_t shown, uint64_t completed, uint64_t total);
int snag_render_submitted(struct snag_render *render, const char *label, const char *text);
int snag_render_input_submitted(struct snag_render *render, const char *label, const char *text);
int snag_render_before_prompt(struct snag_render *render);
int snag_render_public_begin(struct snag_render *render, int fd, const char *label);
int snag_render_public(
    struct snag_render *render, const char *text, size_t len, struct snag_buf *delivered);
int snag_render_public_end(struct snag_render *render);
int snag_render_public_abort(struct snag_render *render);
int snag_render_rollout_begin(
    struct snag_render *render, int fd, const char *label, enum snag_presentation kind);
int snag_render_rollout(
    struct snag_render *render, const char *text, size_t len, struct snag_buf *delivered);
int snag_render_rollout_end(struct snag_render *render);
int snag_render_rollout_abort(struct snag_render *render);
int snag_render_error_ctx(struct snag_render *render, const char *message);
int snag_render_warning_ctx(struct snag_render *render, const char *message);
int snag_render_update(struct snag_render *render, const char *text);
int snag_render_help(struct snag_render *render, const char *text);
int snag_render_choices(struct snag_render *, const char *);
int snag_render_host(struct snag_render *render, const char *text);
int snag_render_voice_event(struct snag_render *, const json_t *, uint32_t, uint32_t);
int snag_render_runtime(struct snag_render *render, const char *text);
int snag_render_irc_event(struct snag_render *render, const struct snag_irc_event *event);
/* A retained event keeps validated routing separate from redacted display strings. */
int snag_render_irc_snapshot(struct snag_render *, const json_t *, const json_t *);
enum snag_render_role { SNAG_ROLE_ACTIVITY, SNAG_ROLE_SUCCESS, SNAG_ROLE_WARNING, SNAG_ROLE_ERROR };

struct snag_render_block {
    struct snag_buf text;
    struct snag_buf context;
    struct snag_buf body;
    size_t colored_len;
    enum snag_render_role role;
    enum snag_presentation body_kind;
    bool truncated;
};

/* Pure formatting of UI-owned values; no terminal writes. UINT64_MAX marks an
 * unrecorded default timeout when formatting historical tool metadata. */
int snag_render_prepare_tool_start(struct snag_render_block *block,
    const struct snag_response_item *call, const char *workdir, uint64_t default_timeout_ms,
    unsigned int level, unsigned int columns);
int snag_render_prepare_tool_finish(struct snag_render_block *block, const char *name,
    const char *call_id, const json_t *result, uint32_t max_output_bytes, unsigned int level,
    unsigned int columns);
/* Provider-hosted search rows reuse the tool-row grammar with their own
 * evidence: the action names the search, the status and sources close it. */
int snag_render_prepare_hosted_start(struct snag_render_block *block, const char *item_id,
    const json_t *action, unsigned int level, unsigned int columns);
int snag_render_prepare_hosted_finish(struct snag_render_block *block, const char *item_id,
    const char *status, const json_t *sources, unsigned int level, unsigned int columns);
void snag_render_block_free(struct snag_render_block *block);
int snag_render_durable(struct snag_render *render, int fd, struct snag_render_source source,
    const char *type, uint32_t timeout_ms, uint32_t max_output_bytes);
int snag_render_tool_block(struct snag_render *render, const struct snag_render_block *block);
int snag_render_event(struct snag_render *render, uint64_t seq, const char *type);
int snag_render_resume_hint(struct snag_render *render, const char *command, size_t command_len);
int snag_render_protocol(
    struct snag_render *render, const char *label, const char *text, size_t len);
int snag_render_transport(struct snag_render *render, char direction, const char *text, size_t len);

#endif
