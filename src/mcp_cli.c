/* SPDX-License-Identifier: GPL-2.0-only */
#include "mcp_internal.h"
#include "app.h"
#include "cli.h"
#include "store.h"
#include "term_host.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t cancelled;

static void
cancel(int signal)
{
    (void)signal;
    cancelled = 1;
}

static int
pump(void *opaque, uint32_t wait_ms)
{
    (void)opaque;
    if (cancelled) return 2;
    return wait_ms && snag_sleep_ms(wait_ms) < 0 ? 2 : 0;
}

int
snag_mcp_cli(const struct snag_cli *cli)
{
    struct snag_config config;
    struct snag_store store;
    struct snag_shutdown shutdown = {0};
    bool signals = false;
    struct snag_mcp *client = NULL;
    struct snag_buf report = {.max = SNAG_MAX_PROVIDER_WIRE};
    char error[256] = {0};
    int rc = 2;
    char *dotdir = NULL;
    snag_config_init(&config);
    snag_store_init(&store);
    if (cli->mcp_argc > 2) {
        (void)fprintf(stderr, "usage: snajpagent [--config FILE] [--dotdir DIR] "
            "mcp list|status|tools|reload|login|logout [SERVER]\n");
        goto done;
    }
    dotdir = snag_app_dotdir(cli->dotdir, error, sizeof(error));
    if (!dotdir || snag_config_load(&config, cli->config_path, dotdir, error, sizeof(error)) < 0 ||
        snag_store_open(&store, dotdir, error, sizeof(error)) < 0 ||
        !(client = snag_mcp_open(store.root_fd, &config))) goto done;
    cancelled = 0;
    if (snag_shutdown_install(&shutdown, cancel, false) < 0) goto done;
    signals = true;
    const char *verb = cli->mcp_argc ? cli->mcp_argv[0] : "list";
    const char *name = cli->mcp_argc == 2 ? cli->mcp_argv[1] : "";
    struct snag_mcp_server *server = NULL;
    for (size_t i = 0u; i < client->count; ++i)
        if (!strcmp(client->servers[i].name, name)) server = &client->servers[i];
    if (!snag_string_in(verb, "list status tools reload login logout") ||
        (strcmp(verb, "list") && !server) || (!strcmp(verb, "list") && *name)) {
        (void)snag_errorf(error, sizeof(error), "invalid MCP command or unknown server");
        goto done;
    }
    if (!strcmp(verb, "login")) {
        if (!server) {
            (void)snag_errorf(error, sizeof(error), "unknown MCP server");
            goto done;
        }
        rc = snag_mcp_login(client, server, pump, NULL) < 0 ? 2 : 0;
        if (rc) (void)snag_strcpy(error, sizeof(error), server->error);
        else (void)printf("%s: OAuth login saved. Use /configure in existing sessions.\n", name);
        goto done;
    }
    if (snag_string_in(verb, "tools status reload")) {
        if (!server) {
            (void)snag_errorf(error, sizeof(error), "unknown MCP server");
            goto done;
        }
        if (!server->enabled || snag_mcp_auth_refresh(client, server, pump, NULL) < 0 ||
            snag_mcp_catalog(server, pump, NULL) < 0) {
            (void)snag_strcpy(error, sizeof(error), server->error);
            if (strcmp(verb, "status")) goto done;
        }
    }
    if (!strcmp(verb, "reload")) {
        (void)printf("%s: catalog loaded; %zu tools. Use /mcp reload %s in a running session.\n",
            name, json_array_size(server->catalog), name);
        rc = 0;
        goto done;
    }
    struct snag_buf command = {.max = SNAG_CONFIG_FILE_MAX};
    if (snag_buf_printf(&command, "%s %s", verb, name) < 0 ||
        snag_buf_terminate(&command) < 0 ||
        snag_mcp_command(client, (char *)command.data, &report, pump, NULL) < 0) {
        snag_buf_free(&command);
        goto done;
    }
    snag_buf_free(&command);
    if (report.len) (void)fwrite(report.data, 1u, report.len, stdout);
    rc = *error ? 2 : 0;
done:
    if (signals) snag_shutdown_detach(&shutdown);
    snag_mcp_close(client);
    snag_store_close(&store);
    snag_config_free(&config);
    snag_buf_free(&report);
    free(dotdir);
    if (rc) (void)fprintf(stderr, "snajpagent: %s\n", *error ? error : "MCP command failed");
    if (signals) snag_shutdown_finish(&shutdown);
    return rc;
}
