/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_CLI_H
#define SNAJPAGENT_CLI_H

#include "config.h"
#include "instructions.h"

#include <stdbool.h>
#include <stddef.h>

#define SNAG_CLI_IRC_CLIENT_MAX 16u

enum snag_cli_color_mode {
    SNAG_CLI_COLOR_UNSET,
    SNAG_CLI_COLOR_AUTO,
    SNAG_CLI_COLOR_ALWAYS,
    SNAG_CLI_COLOR_NEVER
};

enum snag_cli_markdown_mode {
    SNAG_CLI_MARKDOWN_UNSET,
    SNAG_CLI_MARKDOWN_ENABLED,
    SNAG_CLI_MARKDOWN_DISABLED
};

enum snag_cli_auth_command {
    SNAG_CLI_AUTH_NONE,
    SNAG_CLI_LOGIN,
    SNAG_CLI_LOGIN_STATUS,
    SNAG_CLI_LOGOUT
};

struct snag_cli {
    enum snag_cli_auth_command auth_command;
    bool device_auth;
    bool with_api_key;
    char *auth_provider;
    bool resume;
    bool execute;
    bool list;
    bool last;
    bool all;
    bool prompt_after_dashdash;
    bool help;
    bool version;
    unsigned int verbosity;
    enum snag_cli_color_mode color;
    enum snag_cli_markdown_mode markdown;
    char *workspace;
    struct snag_instruction_set doc_instructions;
    char *dotdir;
    char *model;
    char *provider;
    char *effort;
    char *config_path;
    char *irc_listen;
    bool irc_no_listen;
    bool irc_no_client;
    char *irc_clients[SNAG_CLI_IRC_CLIENT_MAX];
    size_t irc_client_count;
    char *irc_model_nick;
    char *irc_operator_nick;
    char *irc_room_name;
    char *resume_id;
    char *prompt;
};

void snag_cli_init(struct snag_cli *cli);
enum snag_color_mode snag_cli_color(const struct snag_cli *cli,
                                    enum snag_color_mode fallback);
bool snag_cli_markdown(const struct snag_cli *cli, bool fallback);
void snag_cli_free(struct snag_cli *cli);
int snag_cli_parse(struct snag_cli *cli, int argc, char **argv,
                  char *error, size_t error_size);
void snag_cli_usage(int fd);

#endif
