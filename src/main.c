/* SPDX-License-Identifier: GPL-2.0-only */
#include "app.h"
#include "base.h"
#include "cli.h"
#include "login.h"
#include "render.h"
#include "snajpagent.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int
run(int argc, char **argv)
{
    struct snag_cli cli;
    char error[256] = "usage error";
    int rc;

    snag_ignore_sigpipe();
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

#ifdef _WIN32
/* Use native wide CRT startup (-municode); never apply the ANSI codepage. */
int
wmain(int argc, wchar_t **wide)
{
    char **argv = snag_wide_arguments(argc, wide);
    if (!argv) {
        (void)fprintf(stderr, "snajpagent: command-line arguments are not valid Unicode\n");
        return 2;
    }
    int rc = run(argc, argv);
    snag_arguments_free(argv);
    return rc;
}
#else
int
main(int argc, char **argv)
{
    return run(argc, argv);
}
#endif
