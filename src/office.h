/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_OFFICE_H
#define SNAJPAGENT_OFFICE_H
#include "base.h"
#include "wake.h"
#include "office_sheet.h"
struct snag_session;
/* Called at native startup, before parsing user CLI or starting any threads. */
void snag_office_program(const char *);
int snag_office_worker(int, char **);
int snag_office_export(struct snag_session *, const char *, const char *, unsigned int, unsigned int,
                       const struct snag_sheet_range *, int (*)(void *, unsigned int), void *, snag_wake_fd,
                       struct snag_buf *, json_t **, char *, size_t);
/* Worker-only: rejects active/external Office package content before import. */
int snag_office_package(const char *, char *, size_t);
int snag_office_worker_limits(const char *, char *, size_t);
int snag_office_confine(const char *, const char *, const char *, char *, size_t);
#endif
