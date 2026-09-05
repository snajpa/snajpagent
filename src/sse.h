/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_SSE_H
#define SNAJPAGENT_SSE_H

#include "base.h"

#include <stdbool.h>
#include <stddef.h>

#define SNAG_MAX_SSE_EVENT (1024u * 1024u)
#define SNAG_MAX_PROVIDER_WIRE (64u * 1024u * 1024u)
#define SNAG_MAX_SSE_NAME 4096u

enum snag_sse_record_kind {
    SNAG_SSE_EVENT,
    SNAG_SSE_COMMENT
};

struct snag_sse_record {
    enum snag_sse_record_kind kind;
    const unsigned char *event;
    size_t event_len;
    const unsigned char *id;
    size_t id_len;
    const unsigned char *data;
    size_t data_len;
};

/* The callback may retain no record pointers and returns zero or minus one. */
typedef int (*snag_sse_record_fn)(void *opaque,
                                 const struct snag_sse_record *record);

struct snag_sse_parser {
    struct snag_buf line;
    struct snag_buf event;
    struct snag_buf id;
    struct snag_buf data;
    snag_sse_record_fn record;
    void *opaque;
    size_t wire_bytes;
    bool data_seen;
    bool pending_cr;
    bool failed;
};

void snag_sse_init(struct snag_sse_parser *parser, snag_sse_record_fn record,
                  void *opaque);
void snag_sse_free(struct snag_sse_parser *parser);
int snag_sse_feed(struct snag_sse_parser *parser, const void *data, size_t len,
                 char *error, size_t error_size);
int snag_sse_finish(struct snag_sse_parser *parser,
                   char *error, size_t error_size);

#endif
