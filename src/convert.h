/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_CONVERT_H
#define SNAJPAGENT_CONVERT_H
#include "base.h"
#include "wake.h"

/* argv[0] is a fixed converter name, found only in absolute PATH directories.
 * No shell, inherited credentials, stdin, unbounded output, or background jobs. */
int snag_convert(const char *const *argv, const char *workdir,
                 struct snag_buf *output,
                 int (*pump)(void *, unsigned int), void *opaque, snag_wake_fd wake,
                 char *error, size_t error_size);
/* Bounded converter diagnostics for decoded timestamp provenance. */
int snag_convert_capture(const char *const *argv, const char *workdir,
                         struct snag_buf *output, struct snag_buf *trace,
                         int (*pump)(void *, unsigned int), void *opaque, snag_wake_fd wake,
                         char *error, size_t error_size);
/* Fixed caller-owned absolute executable (internal linked Office worker). */
int snag_convert_internal(const char *const *, const char *, struct snag_buf *,
                          int (*)(void *, unsigned int), void *, snag_wake_fd, char *, size_t);
#endif
