/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_MCP_H
#define SNAJPAGENT_MCP_H

#include "config.h"
#include "turn.h"

struct snag_cli;
struct snag_session;
struct snag_mcp;
typedef int (*snag_mcp_pump_fn)(void *, uint32_t);

/* Each session owns a client. Configuration and catalog changes take effect
 * between turns; the borrowed schema array stays valid through that turn. */
struct snag_mcp *snag_mcp_open(int root_fd, struct snag_config *config);
void snag_mcp_configure(struct snag_mcp *, struct snag_mcp *);
void snag_mcp_close(struct snag_mcp *client);
void snag_mcp_poll(struct snag_mcp *client);
bool snag_mcp_watching(const struct snag_mcp *client);
int snag_mcp_prepare(struct snag_mcp *client, const struct snag_session *session,
    snag_mcp_pump_fn pump, void *opaque);
const json_t *snag_mcp_tools(const struct snag_mcp *client);
int snag_mcp_call(struct snag_mcp *client, const struct snag_response_item *call,
    snag_mcp_pump_fn pump, void *opaque, json_t **result);
int snag_mcp_command(struct snag_mcp *client, const char *command, struct snag_buf *report,
    snag_mcp_pump_fn pump, void *opaque);
int snag_mcp_cli(const struct snag_cli *cli);

#endif
