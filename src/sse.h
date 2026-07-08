/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_SSE_H
#define SNAJPAGENT_SSE_H

#include "base.h"

#include <stdbool.h>
#include <stddef.h>

#define SNJ_MAX_SSE_EVENT (1024u * 1024u)
#define SNJ_MAX_PROVIDER_WIRE (64u * 1024u * 1024u)
#define SNJ_MAX_SSE_NAME 4096u

enum snj_sse_record_kind {
    SNJ_SSE_EVENT,
    SNJ_SSE_COMMENT
};

struct snj_sse_record {
    enum snj_sse_record_kind kind;
    const unsigned char *event;
    size_t event_len;
    const unsigned char *id;
    size_t id_len;
    const unsigned char *data;
    size_t data_len;
};

/* The callback may retain no record pointers and returns zero or minus one. */
typedef int (*snj_sse_record_fn)(void *opaque,
                                 const struct snj_sse_record *record);

struct snj_sse_parser {
    struct snj_buf line;
    struct snj_buf event;
    struct snj_buf id;
    struct snj_buf data;
    snj_sse_record_fn record;
    void *opaque;
    size_t wire_bytes;
    bool data_seen;
    bool pending_cr;
    bool failed;
};

void snj_sse_init(struct snj_sse_parser *parser, snj_sse_record_fn record,
                  void *opaque);
void snj_sse_free(struct snj_sse_parser *parser);
int snj_sse_feed(struct snj_sse_parser *parser, const void *data, size_t len,
                 char *error, size_t error_size);
int snj_sse_finish(struct snj_sse_parser *parser,
                   char *error, size_t error_size);

#endif
