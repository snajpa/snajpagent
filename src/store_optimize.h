/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_STORE_OPTIMIZE_H
#define SNAJPAGENT_STORE_OPTIMIZE_H

#include "store.h"

struct snag_context_control;

/* Exclusive session-lock owners only. Recover a cutover before opening files. */
int snag_store_recover_optimization(struct snag_session *, char *error, size_t error_size);
int snag_store_optimize_directory(struct snag_session *, const struct snag_context_control *,
    char *error, size_t error_size);
int snag_optimize_main(int argc, char **argv);

#endif
