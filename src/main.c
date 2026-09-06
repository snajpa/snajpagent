/* SPDX-License-Identifier: GPL-2.0-only */
#include "app.h"
#include "cli.h"
#include "login.h"
#include "render.h"
#include "snajpagent.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
    struct snag_cli cli;
    char error[256] = "usage error";
    int rc;

    (void)signal(SIGPIPE, SIG_IGN);
    snag_cli_init(&cli);
    if (snag_cli_parse(&cli, argc, argv, error, sizeof(error)) < 0) {
        struct snag_render render;
        snag_render_init(&render, 0u);
        snag_render_set_color(&render, snag_cli_color(&cli, SNAG_COLOR_AUTO));
        (void)snag_render_error_ctx(&render, error);
        snag_cli_usage(STDERR_FILENO);
        snag_cli_free(&cli);
        return 2;
    }
    if (cli.help) {
        snag_cli_usage(STDOUT_FILENO);
        snag_cli_free(&cli);
        return 0;
    }
    if (cli.version) {
        (void)printf("%s\n", SNAJPAGENT_IDENTITY);
        snag_cli_free(&cli);
        return 0;
    }
    {
        bool handled = false;
        rc = snag_login_dispatch(&cli, &handled);
        if (!handled)
            rc = snag_app_run(&cli, argv[0]);
    }
    snag_cli_free(&cli);
    return rc;
}
