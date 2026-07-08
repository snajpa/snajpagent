/* SPDX-License-Identifier: GPL-2.0-only */
#include "app.h"
#include "cli.h"
#include "render.h"
#include "snajpagent.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
    struct snj_cli cli;
    char error[256] = "usage error";
    int rc;

    (void)signal(SIGPIPE, SIG_IGN);
    snj_cli_init(&cli);
    if (snj_cli_parse(&cli, argc, argv, error, sizeof(error)) < 0) {
        (void)snj_render_error(error);
        snj_cli_usage(STDERR_FILENO);
        snj_cli_free(&cli);
        return 2;
    }
    if (cli.help) {
        snj_cli_usage(STDOUT_FILENO);
        snj_cli_free(&cli);
        return 0;
    }
    if (cli.version) {
        (void)printf("snajpagent %s\n", SNAJPAGENT_VERSION);
        snj_cli_free(&cli);
        return 0;
    }
    rc = snj_app_run(&cli);
    snj_cli_free(&cli);
    return rc;
}
