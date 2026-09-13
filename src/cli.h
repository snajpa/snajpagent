/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_CLI_H
#define SNAJPAGENT_CLI_H

#include "config.h"
#include "instructions.h"

#include <stdbool.h>
#include <stddef.h>

#define SNAG_CLI_IRC_CLIENT_MAX 16u

enum snag_cli_auth_command {
    SNAG_CLI_AUTH_NONE, SNAG_CLI_LOGIN, SNAG_CLI_LOGIN_STATUS, SNAG_CLI_LOGOUT };

struct snag_cli {
    enum snag_cli_auth_command auth_command;
    bool device_auth;
    bool with_api_key;
    const char *auth_provider;
    bool resume;
    bool execute;
    bool list;
    bool last;
    bool all;
    bool prompt_after_dashdash;
    bool help;
    bool manual;
    bool update_model_cache;
    bool version;
    unsigned int verbosity;
    /* Immutable option values borrow argv for the lifetime of the CLI. */
    const char *color, *markdown; /* NULL means use configuration. */
    const char *workspace;
    struct snag_instruction_set doc_instructions;
    const char *dotdir;
    const char *model;
    const char *provider;
    const char *effort;
    const char *config_path;
    const char *irc_listen;
    bool irc_no_listen;
    bool irc_no_client;
    const char *irc_clients[SNAG_CLI_IRC_CLIENT_MAX];
    size_t irc_client_count;
    const char *irc_model_nick;
    const char *irc_operator_nick;
    const char *irc_room_name;
    const char *resume_id;
    char *prompt;
};

void snag_cli_init(struct snag_cli *cli);
enum snag_color_mode snag_cli_color(const struct snag_cli *cli, enum snag_color_mode fallback);
bool snag_cli_markdown(const struct snag_cli *cli, bool fallback);
void snag_cli_free(struct snag_cli *cli);
int snag_cli_parse(struct snag_cli *cli, int argc, char **argv, char *error, size_t error_size);
void snag_cli_usage(int fd);
void snag_cli_help(bool manual);

#endif
