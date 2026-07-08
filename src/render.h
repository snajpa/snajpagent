/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_RENDER_H
#define SNAJPAGENT_RENDER_H

#include "store.h"
#include "term.h"

#include <stdbool.h>
#include <stddef.h>

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
    struct snj_term *term;
    unsigned char utf8_pending[4];
    size_t utf8_pending_len;
};

void snj_render_init(struct snj_render *render, unsigned int verbosity);
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
