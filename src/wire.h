/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_WIRE_H
#define SNAJPAGENT_WIRE_H

#include "base.h"

#include <stddef.h>

#define SNJ_WIRE_BODY_MAX (2u * 1024u * 1024u)
#define SNJ_WIRE_HEADER_MAX (16u * 1024u)
#define SNJ_WIRE_URL_MAX (16u * 1024u)
#define SNJ_WIRE_SECRET_MAX 4096u
#define SNJ_WIRE_SECRET_COUNT_MAX 64u

struct snj_wire_secrets {
    const char *const *values;
    size_t count;
};

int snj_wire_json_redact(const unsigned char *data, size_t len,
                         const struct snj_wire_secrets *secrets,
                         struct snj_buf *out, char *error, size_t error_size);
int snj_wire_header_redact(const unsigned char *line, size_t len,
                           const struct snj_wire_secrets *secrets,
                           struct snj_buf *out);
int snj_wire_url_redact(const char *url,
                        const struct snj_wire_secrets *secrets,
                        struct snj_buf *out);
int snj_wire_body_metadata(const char *media_type, const void *data, size_t len,
                           struct snj_buf *out);

#endif
