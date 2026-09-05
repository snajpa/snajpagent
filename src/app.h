/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_APP_H
#define SNAJPAGENT_APP_H

#include "cli.h"
char *snag_app_dotdir(const char *override, char *error, size_t error_size);

int snag_app_run(const struct snag_cli *cli, const char *program);

#endif
