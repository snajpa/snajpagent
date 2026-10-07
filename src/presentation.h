/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_PRESENTATION_H
#define SNAJPAGENT_PRESENTATION_H

#include "ui.h"

/* Apply one typed session output operation to either terminal or styled sink.
 * Return 1 for a control/editor operation, 0 when applied, -1 on failure.
 * Borrowed command storage remains alive through the call. */
int snag_presentation_apply(struct snag_render *, const struct snag_ui_command *,
    struct snag_buf *delivered);

/* Encoded output owns its strings; control operations return 1 and no record.
 * Journal descriptors are supplied by the reader, never stored as integers. */
int snag_presentation_encode(const struct snag_ui_command *, json_t **record);
int snag_presentation_replay(struct snag_render *, const json_t *, int journal_fd);
/* Public channel events share local identities; queries retain exact identity. */
bool snag_presentation_event_selected(const json_t *route, const char *type, const json_t *data);

#define SNAG_PRESENTATION_FILE ".view-presentation.snb"

/* The display thread is the sole writer under the session's existing lock.
 * This auxiliary stream contains presentation only, never model state. */
struct snag_presentation_writer;
struct snag_presentation_writer *snag_presentation_writer_open(int directory, const char *id);
void snag_presentation_writer_close(struct snag_presentation_writer *);
/* Bind a new presentation stream to the first canonical event after its
 * historical prefix. Existing streams retain their original boundary. */
int snag_presentation_start(struct snag_presentation_writer *, uint64_t next_sequence);
json_t *snag_presentation_snapshot(const struct snag_presentation_writer *);
int snag_presentation_bound(const json_t *, uint64_t *, struct snag_binary_anchor *);
int snag_presentation_append(struct snag_presentation_writer *, const struct snag_ui_command *);
int snag_presentation_origin(int fd, const struct snag_binary_anchor *tail,
    uint64_t *next_sequence, struct snag_binary_anchor *first);
/* Read a reverse page under the captured complete tail. An optional end must
 * be a boundary returned by this reader for the same immutable prefix. The
 * caller owns the private descriptor and cancellation/lifetime. */
int snag_presentation_read(int fd, const char *id, const struct snag_binary_anchor *bound,
    const struct snag_binary_anchor *position, bool reverse, size_t budget,
    json_t **records, struct snag_binary_anchor *begin,
    struct snag_binary_anchor *tail, bool *incomplete,
    bool (*cancel)(void *), void *opaque);

#endif
