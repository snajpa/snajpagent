/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_WIRE_H
#define SNAJPAGENT_WIRE_H

#include "base.h"

#include <stddef.h>

#define SNAG_WIRE_BODY_MAX (2u * 1024u * 1024u)
#define SNAG_WIRE_HEADER_MAX (16u * 1024u)
#define SNAG_WIRE_URL_MAX (16u * 1024u)
#define SNAG_WIRE_SECRET_MAX 16384u
#define SNAG_WIRE_SECRET_COUNT_MAX 81u

struct snag_wire_secrets {
    const char *const *values;
    size_t count;
};

int snag_wire_json_redact(const unsigned char *data, size_t len,
                         const struct snag_wire_secrets *secrets,
                         struct snag_buf *out, char *error, size_t error_size);
int snag_wire_header_redact(const unsigned char *line, size_t len,
                           const struct snag_wire_secrets *secrets,
                           struct snag_buf *out);

#endif
