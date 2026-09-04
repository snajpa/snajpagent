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
        struct snj_render render;
        enum snj_color_mode color = SNJ_COLOR_AUTO;

        if (cli.color == SNJ_CLI_COLOR_ALWAYS)
            color = SNJ_COLOR_ALWAYS;
        else if (cli.color == SNJ_CLI_COLOR_NEVER)
            color = SNJ_COLOR_NEVER;
        snj_render_init(&render, 0u);
        snj_render_set_color(&render, color);
        (void)snj_render_error_ctx(&render, error);
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
        (void)printf("%s\n", SNAJPAGENT_IDENTITY);
        snj_cli_free(&cli);
        return 0;
    }
    rc = snj_app_run(&cli, argv[0]);
    snj_cli_free(&cli);
    return rc;
}
