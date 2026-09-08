/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_BASE64_H
#define SNAJPAGENT_BASE64_H
#include <stdbool.h>
#include <stddef.h>

/* Zero-initialize for each independent payload. No allocation, OS, file,
 * JSON, or device dependency. Sink consumes all bytes synchronously or fails;
 * a failure is sticky, because previously delivered output cannot be retried. */
typedef int (*snag_bytes_sink)(void *, const unsigned char *, size_t);
struct snag_base64_stream {
    unsigned char tail[3];
    unsigned char used;
    bool failed, finished;
};
int snag_base64_write(struct snag_base64_stream *, const void *, size_t,
                      snag_bytes_sink, void *);
int snag_base64_finish(struct snag_base64_stream *, snag_bytes_sink, void *);
#endif
