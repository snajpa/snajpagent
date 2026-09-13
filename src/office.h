/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_OFFICE_H
#define SNAJPAGENT_OFFICE_H
#include "base.h"
#include "wake.h"
#include "office_sheet.h"
struct snag_session;
/* Called at native startup, before parsing user CLI or starting any threads. */
void snag_office_program(const char *);
/* Absolute build-time root, or relative to the actual executable. Caller owns
 * the result; no PATH/environment lookup. */
char *snag_office_runtime(const char *program,const char *root);
/* Verify an installed runtime before it is loaded. Detection is by component
 * presence, never by PATH or a conversion command's exit status: the engine
 * still exits 0 on input it could not load. 0 when usable, -1 with a clear
 * message otherwise. */
int snag_office_verify_runtime(const char *root,char *,size_t);
/* Owned percent-encoded file URL from an absolute POSIX, drive or UNC path. */
char *snag_office_file_url(const char *path);
/* Create a fresh private profile with macro/link settings before import. */
int snag_office_profile(const char *,char *,size_t);
int snag_office_worker(int, char **);
int snag_office_export(struct snag_session *, const char *, const char *, unsigned int, unsigned int,
                       const struct snag_sheet_range *, int (*)(void *, unsigned int), void *, snag_wake_fd,
                       struct snag_buf *, json_t **, char *, size_t);
/* Worker-only: rejects active/external Office package content before import. */
int snag_office_package(const char *, char *, size_t);
int snag_office_worker_limits(const char *, char *, size_t);
/* Nonnegative: writes the applied restrictions/availability to the message;
 * 1 means OS confinement is wholly or partly unavailable, -1 is a failure. */
int snag_office_confine(const char *, const char *, const char *, char *, size_t);
#endif
