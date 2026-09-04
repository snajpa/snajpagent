/* SPDX-License-Identifier: GPL-2.0-only */
#include "cli.h"
#include "base.h"
#include "config.h"

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
    cli->color = SNJ_CLI_COLOR_UNSET;
    cli->markdown = SNJ_CLI_MARKDOWN_UNSET;
}

void
snj_cli_free(struct snj_cli *cli)
{
    free(cli->workspace);
    free(cli->dotdir);
    free(cli->model);
    free(cli->effort);
    free(cli->config_path);
    free(cli->irc_listen);
    for (size_t i = 0; i < cli->irc_client_count; ++i)
        free(cli->irc_clients[i]);
    free(cli->irc_name);
    free(cli->irc_operator_name);
    free(cli->irc_room_name);
    free(cli->resume_id);
    free(cli->prompt);
    snj_cli_init(cli);
}

static int
add_client(struct snj_cli *cli, const char *value,
           char *error, size_t error_size)
{
    char *copy;

    if (cli->irc_client_count >= SNJ_CLI_IRC_CLIENT_MAX) {
        set_error(error, error_size, "at most %u -c options are supported",
                  SNJ_CLI_IRC_CLIENT_MAX);
        errno = E2BIG;
        return -1;
    }
    copy = snj_strdup_checked(value, SNJ_CONFIG_URL_MAX);
    if (!copy) {
        set_error(error, error_size,
                  "-c endpoint is too long or unavailable");
        return -1;
    }
    for (size_t i = 0; i < cli->irc_client_count; ++i)
        if (strcmp(cli->irc_clients[i], copy) == 0) {
            free(copy);
            set_error(error, error_size, "duplicate -c endpoint");
            errno = EINVAL;
            return -1;
        }
    cli->irc_clients[cli->irc_client_count++] = copy;
    return 0;
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

static const char *
optional_endpoint(int argc, char **argv, int *index, const char *attached)
{
    if (attached && *attached)
        return attached;
    if (*index + 1 < argc && strcmp(argv[*index + 1], "--") != 0 &&
        argv[*index + 1][0] != '-') {
        ++*index;
        return argv[*index];
    }
    return "localhost:6667";
}

static int
set_color(struct snj_cli *cli, enum snj_cli_color_mode color,
          const char *name, char *error, size_t error_size)
{
    if (cli->color != SNJ_CLI_COLOR_UNSET) {
        set_error(error, error_size, "duplicate %s option", name);
        errno = EINVAL;
        return -1;
    }
    cli->color = color;
    return 0;
}

static int
set_markdown(struct snj_cli *cli, enum snj_cli_markdown_mode markdown,
             const char *name, char *error, size_t error_size)
{
    if (cli->markdown != SNJ_CLI_MARKDOWN_UNSET) {
        set_error(error, error_size, "duplicate %s option", name);
        errno = EINVAL;
        return -1;
    }
    cli->markdown = markdown;
    return 0;
}

static int
parse_color_value(struct snj_cli *cli, const char *value,
                  const char *name, char *error, size_t error_size)
{
    enum snj_cli_color_mode color;

    if (strcmp(value, "auto") == 0)
        color = SNJ_CLI_COLOR_AUTO;
    else if (strcmp(value, "always") == 0)
        color = SNJ_CLI_COLOR_ALWAYS;
    else if (strcmp(value, "never") == 0)
        color = SNJ_CLI_COLOR_NEVER;
    else {
        set_error(error, error_size,
                  "%s accepts auto, always, or never", name);
        errno = EINVAL;
        return -1;
    }
    return set_color(cli, color, name, error, error_size);
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
        case 'd':
            if (cli->irc_daemon) {
                set_error(error, error_size, "duplicate -d option");
                return -1;
            }
            cli->irc_daemon = true;
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
        case 'c':
            arg = optional_endpoint(argc, argv, index, p);
            p += strlen(p);
            if (add_client(cli, arg, error, error_size) < 0)
                return -1;
            break;
        case 's':
            arg = optional_endpoint(argc, argv, index, p);
            p += strlen(p);
            if (set_once(&cli->irc_listen, arg, "-s", error,
                         error_size) < 0)
                return -1;
            break;
        case 'C': case 'm': case 'o': case 'n': case 'r':
            arg = option_argument(argc, argv, index, p, flag == 'C' ? "-C" :
                                  flag == 'm' ? "-m" :
                                  flag == 'o' ? "-o" :
                                  flag == 'n' ? "-n" : "-r", error, error_size);
            if (!arg)
                return -1;
            p += strlen(p);
            if (flag == 'C' && set_once(&cli->workspace, arg, "-C", error, error_size) < 0)
                return -1;
            if (flag == 'm' && set_once(&cli->model, arg, "-m", error, error_size) < 0)
                return -1;
            if (flag == 'o' && set_once(&cli->irc_operator_name, arg, "-o", error, error_size) < 0)
                return -1;
            if (flag == 'n' && set_once(&cli->irc_name, arg, "-n", error, error_size) < 0)
                return -1;
            if (flag == 'r' && set_once(&cli->irc_room_name, arg, "-r", error, error_size) < 0)
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
bounded_preference(const char *value, size_t max)
{
    size_t len = strlen(value);
    return len != 0u && len < max &&
           snj_utf8_valid((const unsigned char *)value, len, true);
}

static int
read_execute_prompt(struct snj_cli *cli, char *error, size_t error_size)
{
    struct snj_buf prompt;
    unsigned char chunk[4096];

    if (isatty(STDIN_FILENO) == 1) {
        set_error(error, error_size,
                  "-e requires a prompt after -- or non-terminal stdin");
        errno = EINVAL;
        return -1;
    }
    snj_buf_init(&prompt, SNJ_MAX_DIRECT_PROMPT + 2u);
    for (;;) {
        ssize_t got = read(STDIN_FILENO, chunk, sizeof(chunk));

        if (got > 0) {
            if (snj_buf_append(&prompt, chunk, (size_t)got) < 0) {
                set_error(error, error_size,
                          "stdin prompt is invalid or exceeds 1 MiB");
                snj_buf_free(&prompt);
                return -1;
            }
            continue;
        }
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0) {
            set_error(error, error_size, "stdin prompt could not be read");
            snj_buf_free(&prompt);
            return -1;
        }
        break;
    }
    if (prompt.len != 0u && prompt.data[prompt.len - 1u] == '\n') {
        --prompt.len;
        if (prompt.len != 0u && prompt.data[prompt.len - 1u] == '\r')
            --prompt.len;
    }
    if (prompt.len == 0u || prompt.len > SNJ_MAX_DIRECT_PROMPT ||
        !snj_utf8_valid(prompt.data, prompt.len, true) ||
        snj_buf_terminate(&prompt) < 0) {
        set_error(error, error_size,
                  "stdin prompt is empty, invalid, or exceeds 1 MiB");
        snj_buf_free(&prompt);
        errno = EINVAL;
        return -1;
    }
    cli->prompt = (char *)prompt.data;
    return 0;
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
        } else if (strcmp(arg, "--daemon") == 0) {
            if (cli->irc_daemon) { set_error(error, error_size, "duplicate --daemon option"); return -1; }
            cli->irc_daemon = true;
        } else if (strcmp(arg, "--resume") == 0) {
            if (cli->resume) { set_error(error, error_size, "duplicate --resume option"); return -1; }
            cli->resume = true;
        } else if (strcmp(arg, "--no-color") == 0) {
            if (set_color(cli, SNJ_CLI_COLOR_NEVER, "--no-color",
                          error, error_size) < 0)
                return -1;
        } else if (strcmp(arg, "--markdown") == 0) {
            if (set_markdown(cli, SNJ_CLI_MARKDOWN_ENABLED, "--markdown",
                             error, error_size) < 0)
                return -1;
        } else if (strcmp(arg, "--no-markdown") == 0) {
            if (set_markdown(cli, SNJ_CLI_MARKDOWN_DISABLED, "--no-markdown",
                             error, error_size) < 0)
                return -1;
        } else if (strcmp(arg, "--color") == 0) {
            if (i + 1 < argc &&
                (strcmp(argv[i + 1], "auto") == 0 ||
                 strcmp(argv[i + 1], "always") == 0 ||
                 strcmp(argv[i + 1], "never") == 0)) {
                if (parse_color_value(cli, argv[++i], "--color",
                                      error, error_size) < 0)
                    return -1;
            } else if (set_color(cli, SNJ_CLI_COLOR_ALWAYS, "--color",
                                 error, error_size) < 0) {
                return -1;
            }
        } else if (strncmp(arg, "--color=", 8u) == 0) {
            if (parse_color_value(cli, arg + 8u, "--color",
                                  error, error_size) < 0)
                return -1;
        } else if (strcmp(arg, "--client") == 0 ||
                   strncmp(arg, "--client=", 9u) == 0) {
            const char *attached = arg[8] == '=' ? arg + 9u : NULL;
            if (attached && !*attached) {
                set_error(error, error_size,
                          "--client= requires a nonempty endpoint");
                errno = EINVAL;
                return -1;
            }
            const char *value = optional_endpoint(argc, argv, &i, attached);
            if (add_client(cli, value, error, error_size) < 0)
                return -1;
        } else if (strcmp(arg, "--listen") == 0 ||
                   strncmp(arg, "--listen=", 9u) == 0) {
            const char *attached = arg[8] == '=' ? arg + 9u : NULL;
            if (attached && !*attached) {
                set_error(error, error_size,
                          "--listen= requires a nonempty endpoint");
                errno = EINVAL;
                return -1;
            }
            const char *value = optional_endpoint(argc, argv, &i, attached);
            if (set_once(&cli->irc_listen, value, "--listen",
                         error, error_size) < 0)
                return -1;
        } else if (strcmp(arg, "--name") == 0 ||
                   strncmp(arg, "--name=", 7u) == 0) {
            const char *attached = arg[6] == '=' ? arg + 7u : NULL;
            const char *value = option_argument(argc, argv, &i, attached,
                                                "--name", error, error_size);
            if (!value || set_once(&cli->irc_name, value, "--name",
                                   error, error_size) < 0)
                return -1;
        } else if (strcmp(arg, "--operator-name") == 0 ||
                   strncmp(arg, "--operator-name=", 16u) == 0) {
            const char *attached = arg[15] == '=' ? arg + 16u : NULL;
            const char *value = option_argument(argc, argv, &i, attached,
                                                "--operator-name", error,
                                                error_size);
            if (!value || set_once(&cli->irc_operator_name, value,
                                   "--operator-name", error, error_size) < 0)
                return -1;
        } else if (strcmp(arg, "--room-name") == 0 ||
                   strncmp(arg, "--room-name=", 12u) == 0) {
            const char *attached = arg[11] == '=' ? arg + 12u : NULL;
            const char *value = option_argument(argc, argv, &i, attached,
                                                "--room-name", error,
                                                error_size);
            if (!value || set_once(&cli->irc_room_name, value, "--room-name",
                                   error, error_size) < 0)
                return -1;
        } else if (strcmp(arg, "--dotdir") == 0 ||
                   strncmp(arg, "--dotdir=", 9u) == 0) {
            const char *attached = arg[8] == '=' ? arg + 9u : NULL;
            const char *value = option_argument(argc, argv, &i, attached,
                                                "--dotdir", error, error_size);
            if (!value || set_once(&cli->dotdir, value, "--dotdir",
                                   error, error_size) < 0)
                return -1;
        } else if (strcmp(arg, "--config") == 0 ||
                   strncmp(arg, "--config=", 9u) == 0) {
            const char *attached = arg[8] == '=' ? arg + 9u : NULL;
            const char *value = option_argument(argc, argv, &i, attached,
                                                "--config", error, error_size);
            if (!value || set_once(&cli->config_path, value, "--config",
                                   error, error_size) < 0)
                return -1;
        } else if (strcmp(arg, "--effort") == 0 ||
                   strncmp(arg, "--effort=", 9u) == 0) {
            const char *attached = arg[8] == '=' ? arg + 9u : NULL;
            const char *value = option_argument(argc, argv, &i, attached,
                                                "--effort", error,
                                                error_size);
            if (!value || set_once(&cli->effort, value, "--effort",
                                   error, error_size) < 0)
                return -1;
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
                      cli->model || cli->effort || cli->verbosity ||
                      cli->irc_daemon || cli->irc_listen ||
                      cli->irc_client_count || cli->irc_name ||
                      cli->irc_operator_name || cli->irc_room_name)) {
        set_error(error, error_size,
                  "-l accepts only --config, --dotdir, --all, and color options");
        return -1;
    }
    if (cli->execute && (cli->irc_daemon || cli->irc_listen ||
                         cli->irc_client_count || cli->irc_name ||
                         cli->irc_operator_name || cli->irc_room_name)) {
        set_error(error, error_size,
                  "-e cannot be combined with network options");
        return -1;
    }
    if ((cli->irc_daemon || cli->irc_listen || cli->irc_client_count) &&
        positional >= 0 && !dashdash) {
        set_error(error, error_size,
                  "networked initial chat text must follow --");
        return -1;
    }
    if (cli->last && !cli->resume) {
        set_error(error, error_size, "--last requires --resume");
        return -1;
    }
    if (cli->all && !cli->resume && !cli->list) {
        set_error(error, error_size, "--all requires --resume or -l");
        return -1;
    }
    if (cli->model && !bounded_preference(cli->model,
                                          SNJ_CONFIG_MODEL_MAX)) {
        set_error(error, error_size,
                  "model exceeds the supported structural bounds");
        return -1;
    }
    if (cli->effort && !bounded_preference(cli->effort,
                                           SNJ_CONFIG_EFFORT_MAX)) {
        set_error(error, error_size,
                  "reasoning effort exceeds the supported structural bounds");
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
    } else if (!cli->list && positional >= 0 && positional < argc) {
        if (cli->execute && !dashdash) {
            set_error(error, error_size, "-e requires -- before its prompt");
            return -1;
        }
        cli->prompt = snj_join_words(argv + positional, (size_t)(argc - positional),
                                     SNJ_MAX_DIRECT_PROMPT);
        if (!cli->prompt)
            return -1;
    }
    if (cli->execute && !cli->prompt &&
        read_execute_prompt(cli, error, error_size) < 0)
        return -1;
    if (cli->execute && !*cli->prompt) {
        set_error(error, error_size, "-e requires a nonempty prompt");
        return -1;
    }
    cli->prompt_after_dashdash = dashdash;
    return 0;
}

void
snj_cli_usage(int fd)
{
    static const char text[] =
        "usage: snajpagent [OPTIONS] [--] [INITIAL PROMPT...]\n"
        "       snajpagent --resume [OPTIONS] [SESSION_ID|--last] [-- FOLLOW-UP...]\n"
        "       snajpagent -e [OPTIONS] [-- PROMPT...]\n"
        "       snajpagent -l [OPTIONS]\n"
        "  -d, --daemon                 host the IRC server in this process\n"
        "  -s, --listen[=ENDPOINT]      connect, or listen with -d\n"
        "  -c, --client[=ENDPOINT]      connect to IRC; repeatable\n"
        "  -n, --name NAME              required networked agent name\n"
        "  -o, --operator-name NAME     local operator name\n"
        "  -r, --room-name ROOM         hosted room name\n"
        "      --dotdir DIR             private application directory\n"
        "      --config FILE            explicit configuration file\n"
        "      --effort LEVEL           reasoning effort override\n"
        "      --color[=WHEN]            auto, always, or never\n"
        "      --no-color               alias for --color=never\n"
        "      --markdown               render model Markdown (default)\n"
        "      --no-markdown            show model Markdown literally\n"
        "  -C DIR                       workspace (or resume relocation)\n"
        "  -m MODEL                     next-turn model override\n"
        "  -v                           increase verbosity; repeatable to 6\n"
        "      --resume [ID|--last]      resume a durable session\n"
        "      --all                    include sessions from all workspaces\n"
        "  -e                           one-shot execution (prompt or stdin)\n"
        "  -l                           list sessions\n"
        "  -h                           show this help\n"
        "  -V                           show version\n";
    (void)snj_write_full(fd, text, sizeof(text) - 1u);
}
