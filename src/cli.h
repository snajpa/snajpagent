/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_CLI_H
#define SNAJPAGENT_CLI_H

#include <stdbool.h>
#include <stddef.h>

struct snj_cli {
    bool resume;
    bool execute;
    bool list;
    bool last;
    bool all;
    bool no_color;
    bool help;
    bool version;
    unsigned int verbosity;
    char *workspace;
    char *model;
    char *effort;
    char *config_path;
    char *resume_id;
    char *prompt;
};

void snj_cli_init(struct snj_cli *cli);
void snj_cli_free(struct snj_cli *cli);
int snj_cli_parse(struct snj_cli *cli, int argc, char **argv,
                  char *error, size_t error_size);
void snj_cli_usage(int fd);

#endif
