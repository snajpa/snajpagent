/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_UNICODE_H
#define SNAJPAGENT_UNICODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Unicode 17 extended grapheme boundaries (UAX #29). Returns the first cluster's
 * byte length, or zero for empty input/invalid initial UTF-8. A later malformed
 * byte starts a new invalid span, preserving the preceding valid cluster. */
size_t snag_grapheme_next(const unsigned char *, size_t);
/* Terminal-cell policy for one valid cluster: emoji/flags/keycaps occupy two
 * cells; combining marks have no independent width. Ambiguous East Asian width
 * is explicit. Controls/invalid input return -1; isolated marks return zero. */
int snag_grapheme_width(const unsigned char *, size_t, bool ambiguous_wide);
/* Editor words: blank, Unicode letter/number/underscore, other nonblank. */
unsigned int snag_unicode_word_class(uint32_t);

#endif
