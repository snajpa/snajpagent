/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_APP_H
#define SNAJPAGENT_APP_H

#include "cli.h"
char *snag_app_dotdir(const char *override, char *error, size_t error_size);
/* Caller owns the complete, shell-quoted command. */
char *snag_app_resume_command(const char *program, const char *dotdir,
    const char *id, bool workspace);

int snag_app_run(const struct snag_cli *cli, const char *program);
int snag_app_owner_main(int argc, char **argv);

struct snag_app_direct;
#if SNAJPAGENT_VM
struct snag_view_channel;
enum snag_app_direct_state { SNAG_APP_DIRECT_STARTING, SNAG_APP_DIRECT_READY,
    SNAG_APP_DIRECT_FINISHED };
/* One engine per workspace process. The channel endpoint transfers on success;
 * state/stop may run while startup or a turn is active. Free stops and joins. */
struct snag_app_direct *snag_app_direct_start(const char *program, const char *dotdir,
    const char *session, const char *name, struct snag_view_channel *);
enum snag_app_direct_state snag_app_direct_state(struct snag_app_direct *,
    char session[SNAG_ID_HEX_LEN + 1u], char *error, size_t error_size, int *status);
void snag_app_direct_stop(struct snag_app_direct *);
void snag_app_direct_free(struct snag_app_direct *);
#endif /* SNAJPAGENT_VM */

#endif
