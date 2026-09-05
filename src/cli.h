/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_CLI_H
#define SNAJPAGENT_CLI_H

#include <stdbool.h>
#include <stddef.h>

#define SNJ_CLI_IRC_CLIENT_MAX 16u

enum snj_cli_color_mode {
    SNJ_CLI_COLOR_UNSET,
    SNJ_CLI_COLOR_AUTO,
    SNJ_CLI_COLOR_ALWAYS,
    SNJ_CLI_COLOR_NEVER
};

enum snj_cli_markdown_mode {
    SNJ_CLI_MARKDOWN_UNSET,
    SNJ_CLI_MARKDOWN_ENABLED,
    SNJ_CLI_MARKDOWN_DISABLED
};

enum snj_cli_auth_command {
    SNJ_CLI_AUTH_NONE,
    SNJ_CLI_LOGIN,
    SNJ_CLI_LOGIN_STATUS,
    SNJ_CLI_LOGOUT
};

struct snj_cli {
    enum snj_cli_auth_command auth_command;
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
    enum snj_cli_color_mode color;
    enum snj_cli_markdown_mode markdown;
    char *workspace;
    char *dotdir;
    char *model;
    char *effort;
    char *config_path;
    char *irc_listen;
    char *irc_clients[SNJ_CLI_IRC_CLIENT_MAX];
    size_t irc_client_count;
    char *irc_model_nick;
    char *irc_operator_nick;
    char *irc_room_name;
    char *resume_id;
    char *prompt;
};

void snj_cli_init(struct snj_cli *cli);
void snj_cli_free(struct snj_cli *cli);
int snj_cli_parse(struct snj_cli *cli, int argc, char **argv,
                  char *error, size_t error_size);
void snj_cli_usage(int fd);

#endif
