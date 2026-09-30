/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_SCREEN_WIRE_H
#define SNAJPAGENT_SCREEN_WIRE_H

#include <stddef.h>
#include <stdint.h>

/* A title update is screen state, not a byte stream. One outstanding chunk
 * stays in that state until its matching acknowledgement arrives. */
#define SNAG_SCREEN_CHUNK 120u
#define SNAG_SCREEN_PREFIX "SNAJPAGENT-SCREEN/1:"

int snag_screen_encode(char *out, size_t capacity, const char *nonce, uint32_t sequence,
                       unsigned int attempt, const unsigned char *bytes, size_t length);
int snag_screen_decode(const unsigned char *body, size_t length, const char *nonce,
                       uint32_t *sequence, uint32_t *checksum,
                       unsigned char bytes[SNAG_SCREEN_CHUNK], size_t *written);
uint32_t snag_screen_checksum(const unsigned char *bytes, size_t length);

#endif
