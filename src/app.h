/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_APP_H
#define SNAJPAGENT_APP_H

#include "cli.h"
char *snj_app_dotdir(const char *override, char *error, size_t error_size);

int snj_app_run(const struct snj_cli *cli, const char *program);

#endif
