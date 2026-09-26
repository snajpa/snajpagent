/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_UPLOAD_WIRE_H
#define SNAJPAGENT_UPLOAD_WIRE_H

#include "base.h"

/* One stop-and-wait DATA block; the encoded line bound also covers metadata. */
#define SNAG_UPLOAD_BLOCK_MAX 65536u
#define SNAG_UPLOAD_LINE_MAX 131072u

/* Payloads are standard Base64 of one complete zlib stream, without framing. */
int snag_upload_wire_decode(const char *payload, size_t length,
                            unsigned char *out, size_t capacity, size_t *written);
int snag_upload_wire_encode(const void *data, size_t length, struct snag_buf *out);

#endif
