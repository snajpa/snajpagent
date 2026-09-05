/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_LOGIN_H
#define SNAJPAGENT_LOGIN_H
#include "cli.h"
/* Runs outside the model/UI engine. False handled means normal startup. */
int snag_login_dispatch(const struct snag_cli *cli, bool *handled);
#endif
