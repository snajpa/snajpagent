/* SPDX-License-Identifier: GPL-2.0-only */
#include "cli.h"
#include "base.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
set_error(char *error, size_t size, const char *fmt, ...)
{
    va_list ap;
    if (!size)
        return;
    va_start(ap, fmt);
    (void)vsnprintf(error, size, fmt, ap);
    va_end(ap);
}

void
snj_cli_init(struct snj_cli *cli)
{
    memset(cli, 0, sizeof(*cli));
}

void
snj_cli_free(struct snj_cli *cli)
{
    free(cli->workspace);
    free(cli->model);
    free(cli->effort);
    free(cli->config_path);
    free(cli->resume_id);
    free(cli->prompt);
    snj_cli_init(cli);
}

static int
set_once(char **slot, const char *value, const char *name,
         char *error, size_t error_size)
{
    if (*slot) {
        set_error(error, error_size, "duplicate %s option", name);
        errno = EINVAL;
        return -1;
    }
    *slot = snj_strdup_checked(value, SNJ_PATH_MAX_BYTES);
    if (!*slot) {
        set_error(error, error_size, "%s argument is too long or unavailable", name);
        return -1;
    }
    return 0;
}

static const char *
option_argument(int argc, char **argv, int *index, const char *attached,
                const char *name, char *error, size_t error_size)
{
    if (attached && *attached)
        return attached;
    if (*index + 1 >= argc) {
        set_error(error, error_size, "%s requires an argument", name);
        errno = EINVAL;
        return NULL;
    }
    ++*index;
    return argv[*index];
}

static int
parse_short(struct snj_cli *cli, int argc, char **argv, int *index,
            char *error, size_t error_size)
{
    const char *p = argv[*index] + 1;

    while (*p) {
        char flag = *p++;
        const char *arg;
        switch (flag) {
        case 'v':
            if (cli->verbosity < 6u)
                ++cli->verbosity;
            break;
        case 'r':
            if (cli->resume) {
                set_error(error, error_size, "duplicate -r option");
                return -1;
            }
            cli->resume = true;
            break;
        case 'e':
            if (cli->execute) {
                set_error(error, error_size, "duplicate -e option");
                return -1;
            }
            cli->execute = true;
            break;
        case 'l':
            if (cli->list) {
                set_error(error, error_size, "duplicate -l option");
                return -1;
            }
            cli->list = true;
            break;
        case 'h': cli->help = true; break;
        case 'V': cli->version = true; break;
        case 'C': case 'm': case 'o': case 'c':
            arg = option_argument(argc, argv, index, p, flag == 'C' ? "-C" :
                                  flag == 'm' ? "-m" :
                                  flag == 'o' ? "-o" : "-c", error, error_size);
            if (!arg)
                return -1;
            p += strlen(p);
            if (flag == 'C' && set_once(&cli->workspace, arg, "-C", error, error_size) < 0)
                return -1;
            if (flag == 'm' && set_once(&cli->model, arg, "-m", error, error_size) < 0)
                return -1;
            if (flag == 'o' && set_once(&cli->effort, arg, "-o", error, error_size) < 0)
                return -1;
            if (flag == 'c' && set_once(&cli->config_path, arg, "-c", error, error_size) < 0)
                return -1;
            break;
        default:
            set_error(error, error_size, "unknown option -%c", flag);
            errno = EINVAL;
            return -1;
        }
    }
    return 0;
}

static bool
effort_valid(const char *s)
{
    static const char *const values[] = {
        "default", "none", "minimal", "low", "medium", "high", "xhigh"
    };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        if (strcmp(s, values[i]) == 0)
            return true;
    return false;
}

int
snj_cli_parse(struct snj_cli *cli, int argc, char **argv,
              char *error, size_t error_size)
{
    int i;
    int positional = -1;
    bool dashdash = false;

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "--") == 0) {
            dashdash = true;
            positional = i + 1;
            break;
        }
        if (arg[0] != '-' || arg[1] == '\0') {
            positional = i;
            break;
        }
        if (strcmp(arg, "--last") == 0) {
            if (cli->last) { set_error(error, error_size, "duplicate --last option"); return -1; }
            cli->last = true;
        } else if (strcmp(arg, "--all") == 0) {
            if (cli->all) { set_error(error, error_size, "duplicate --all option"); return -1; }
            cli->all = true;
        } else if (strcmp(arg, "--no-color") == 0) {
            if (cli->no_color) { set_error(error, error_size, "duplicate --no-color option"); return -1; }
            cli->no_color = true;
        } else if (arg[1] == '-') {
            set_error(error, error_size, "unknown option %s", arg);
            return -1;
        } else if (parse_short(cli, argc, argv, &i, error, error_size) < 0) {
            return -1;
        }
    }
    if ((cli->help || cli->version) &&
        (argc != 2 ||
         (strcmp(argv[1], "-h") != 0 && strcmp(argv[1], "-V") != 0))) {
        set_error(error, error_size, "-h and -V must stand alone");
        return -1;
    }
    if (cli->help || cli->version)
        return 0;
    if (cli->list && (cli->resume || cli->execute || cli->last || cli->workspace ||
                      cli->model || cli->effort || cli->verbosity)) {
        set_error(error, error_size, "-l accepts only -c, --all, and --no-color");
        return -1;
    }
    if (cli->last && !cli->resume) {
        set_error(error, error_size, "--last requires -r");
        return -1;
    }
    if (cli->all && !cli->resume && !cli->list) {
        set_error(error, error_size, "--all requires -r or -l");
        return -1;
    }
    if (cli->effort && !effort_valid(cli->effort)) {
        set_error(error, error_size, "invalid reasoning effort");
        return -1;
    }
    if (cli->resume) {
        if (positional >= 0 && !dashdash && !cli->last) {
            cli->resume_id = snj_strdup_checked(argv[positional], SNJ_ID_HEX_LEN);
            if (!cli->resume_id) {
                set_error(error, error_size, "session id is too long or unavailable");
                return -1;
            }
            ++positional;
            if (positional < argc) {
                if (strcmp(argv[positional], "--") != 0) {
                    set_error(error, error_size, "resume follow-up must follow --");
                    return -1;
                }
                dashdash = true;
                ++positional;
            }
        }
        if (cli->last && positional >= 0 && !dashdash) {
            set_error(error, error_size, "--last cannot be combined with a session id");
            return -1;
        }
        if (cli->all && cli->resume_id) {
            set_error(error, error_size, "--all is invalid with an exact session id");
            return -1;
        }
        if (positional >= 0 && positional < argc) {
            cli->prompt = snj_join_words(argv + positional, (size_t)(argc - positional),
                                         SNJ_MAX_DIRECT_PROMPT);
            if (!cli->prompt) {
                set_error(error, error_size, "prompt is invalid or exceeds 1 MiB");
                return -1;
            }
        }
    } else if (!cli->list && positional >= 0) {
        if (cli->execute && !dashdash) {
            set_error(error, error_size, "-e requires -- before its prompt");
            return -1;
        }
        cli->prompt = snj_join_words(argv + positional, (size_t)(argc - positional),
                                     SNJ_MAX_DIRECT_PROMPT);
        if (!cli->prompt)
            return -1;
    }
    if (cli->execute && (!cli->prompt || !*cli->prompt)) {
        set_error(error, error_size, "-e requires a nonempty prompt");
        return -1;
    }
    return 0;
}

void
snj_cli_usage(int fd)
{
    static const char text[] =
        "usage: snajpagent [OPTIONS] [--] [INITIAL PROMPT...]\n"
        "       snajpagent -r [OPTIONS] [SESSION_ID|--last] [-- FOLLOW-UP...]\n"
        "       snajpagent -e [OPTIONS] -- PROMPT...\n"
        "       snajpagent -l [OPTIONS]\n";
    (void)snj_write_full(fd, text, sizeof(text) - 1u);
}
