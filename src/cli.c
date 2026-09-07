/* SPDX-License-Identifier: GPL-2.0-only */
#include "cli.h"
#include "base.h"
#include "config.h"
#include "snajpagent.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/wait.h>
#endif

void
snag_cli_init(struct snag_cli *cli)
{
    memset(cli, 0, sizeof(*cli));
    cli->color = SNAG_CLI_COLOR_UNSET;
    cli->markdown = SNAG_CLI_MARKDOWN_UNSET;
}

enum snag_color_mode
snag_cli_color(const struct snag_cli *cli, enum snag_color_mode fallback)
{
    switch (cli->color) {
    case SNAG_CLI_COLOR_AUTO: return SNAG_COLOR_AUTO;
    case SNAG_CLI_COLOR_ALWAYS: return SNAG_COLOR_ALWAYS;
    case SNAG_CLI_COLOR_NEVER: return SNAG_COLOR_NEVER;
    case SNAG_CLI_COLOR_UNSET: return fallback;
    }
    return fallback;
}

bool
snag_cli_markdown(const struct snag_cli *cli, bool fallback)
{
    return cli->markdown == SNAG_CLI_MARKDOWN_ENABLED ? true :
           cli->markdown == SNAG_CLI_MARKDOWN_DISABLED ? false : fallback;
}

void
snag_cli_free(struct snag_cli *cli)
{
    snag_instructions_free(&cli->doc_instructions);
    free(cli->prompt);
    snag_cli_init(cli);
}

static int
add_client(struct snag_cli *cli, const char *value,
           char *error, size_t error_size)
{
    if (cli->irc_client_count >= SNAG_CLI_IRC_CLIENT_MAX)
        return snag_fail(error, error_size, E2BIG, "at most %u -c options are supported",
                         SNAG_CLI_IRC_CLIENT_MAX);
    if (strlen(value) > SNAG_CONFIG_URL_MAX)
        return snag_fail(error, error_size, EOVERFLOW, "-c endpoint is too long or unavailable");
    for (size_t i = 0; i < cli->irc_client_count; ++i)
        if (strcmp(cli->irc_clients[i], value) == 0)
            return snag_fail(error, error_size, EINVAL, "duplicate -c endpoint");
    cli->irc_clients[cli->irc_client_count++] = value;
    return 0;
}

static int
set_once(const char **slot, const char *value, const char *name,
         char *error, size_t error_size)
{
    if (*slot)
        return snag_fail(error, error_size, EINVAL, "duplicate %s option", name);
    if (strlen(value) > SNAG_PATH_MAX_BYTES)
        return snag_fail(error, error_size, EOVERFLOW,
                         "%s argument is too long or unavailable", name);
    *slot = value;
    return 0;
}

static const char *
option_argument(int argc, char **argv, int *index, const char *attached,
                const char *name, char *error, size_t error_size)
{
    if (attached && *attached)
        return attached;
    if (*index + 1 >= argc) {
        (void)snag_fail(error, error_size, EINVAL, "%s requires an argument", name);
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
set_color(struct snag_cli *cli, enum snag_cli_color_mode color,
          const char *name, char *error, size_t error_size)
{
    if (cli->color != SNAG_CLI_COLOR_UNSET) {
        return snag_fail(error, error_size, EINVAL, "duplicate %s option", name);
    }
    cli->color = color;
    return 0;
}

static int
set_markdown(struct snag_cli *cli, enum snag_cli_markdown_mode markdown,
             const char *name, char *error, size_t error_size)
{
    if (cli->markdown != SNAG_CLI_MARKDOWN_UNSET) {
        return snag_fail(error, error_size, EINVAL, "duplicate %s option", name);
    }
    cli->markdown = markdown;
    return 0;
}

static int
parse_color_value(struct snag_cli *cli, const char *value,
                  const char *name, char *error, size_t error_size)
{
    enum snag_cli_color_mode color;

    if (strcmp(value, "auto") == 0)
        color = SNAG_CLI_COLOR_AUTO;
    else if (strcmp(value, "always") == 0)
        color = SNAG_CLI_COLOR_ALWAYS;
    else if (strcmp(value, "never") == 0)
        color = SNAG_CLI_COLOR_NEVER;
    else {
        return snag_fail(error, error_size, EINVAL,
                  "%s accepts auto, always, or never", name);
    }
    return set_color(cli, color, name, error, error_size);
}

static int
read_execute_prompt(struct snag_cli *cli, char *error, size_t error_size)
{
    struct snag_buf prompt = {.max = SNAG_MAX_DIRECT_PROMPT + 2u};

    if (snag_isatty(STDIN_FILENO) == 1) {
        return snag_fail(error, error_size, EINVAL,
                  "-e requires a prompt after -- or non-terminal stdin");
    }
    int rc = snag_buf_read(&prompt, STDIN_FILENO);
    if (rc < 0) {
        snag_errorf(error, error_size, rc == -2 ?
            "stdin prompt is invalid or exceeds 1 MiB" : "stdin prompt could not be read");
        snag_buf_free(&prompt);
        return -1;
    }
    if (prompt.len != 0u && prompt.data[prompt.len - 1u] == '\n') {
        --prompt.len;
        if (prompt.len != 0u && prompt.data[prompt.len - 1u] == '\r')
            --prompt.len;
    }
    if (prompt.len == 0u || prompt.len > SNAG_MAX_DIRECT_PROMPT ||
        !snag_utf8_valid(prompt.data, prompt.len, true) ||
        snag_buf_terminate(&prompt) < 0) {
        snag_errorf(error, error_size,
                  "stdin prompt is empty, invalid, or exceeds 1 MiB");
        snag_buf_free(&prompt);
        return snag_errno(EINVAL);
    }
    cli->prompt = (char *)prompt.data;
    return 0;
}

static int
parse_auth_command(struct snag_cli *cli, int argc, char **argv, int first,
                    char *error, size_t error_size)
{
    cli->auth_command = strcmp(argv[first], "logout") == 0 ? SNAG_CLI_LOGOUT : SNAG_CLI_LOGIN;
    if (cli->auth_command == SNAG_CLI_LOGIN && first + 1 < argc &&
        strcmp(argv[first + 1], "status") == 0) {
        cli->auth_command = SNAG_CLI_LOGIN_STATUS;
        ++first;
    }
    for (int i = first + 1; i < argc; ++i) {
        if (strcmp(argv[i], "--device-auth") == 0 && !cli->device_auth)
            cli->device_auth = true;
        else if (strcmp(argv[i], "--with-api-key") == 0 && !cli->with_api_key)
            cli->with_api_key = true;
        else if (argv[i][0] != '-' && !cli->auth_provider) {
            if (strlen(argv[i]) > SNAG_CONFIG_PROVIDER_NAME_MAX) {
                errno = EOVERFLOW;
                goto invalid;
            }
            cli->auth_provider = argv[i];
        } else
            goto invalid;
    }
    if (cli->update_model_cache || cli->list || cli->last || cli->all || cli->provider || cli->irc_listen || cli->irc_client_count ||
        cli->doc_instructions.count ||
        cli->irc_no_listen || cli->irc_no_client ||
        cli->irc_model_nick || cli->irc_operator_nick || cli->irc_room_name ||
        (cli->device_auth && cli->with_api_key) ||
        (cli->auth_command != SNAG_CLI_LOGIN && (cli->device_auth || cli->with_api_key || cli->model || cli->effort)))
        goto invalid;
    return 0;
invalid:
    snag_errorf(error, error_size, "invalid login/logout arguments; put common options before the command");
    return -1;
}

static int
parse_options(struct snag_cli *cli, int argc, char **argv, int *index,
              char *error, size_t error_size)
{
    const struct option {
        const char *name;
        char short_name;
        bool argument;
        const char **slot;
        bool *toggle;
    } options[] = {
        {NULL, 'C', true, &cli->workspace, NULL},
        {NULL, 'm', true, &cli->model, NULL},
        {"--model-nick", 'n', true, &cli->irc_model_nick, NULL},
        {"--operator-nick", 'o', true, &cli->irc_operator_nick, NULL},
        {"--room-name", 'r', true, &cli->irc_room_name, NULL},
        {"--dotdir", 0, true, &cli->dotdir, NULL},
        {"--provider", 0, true, &cli->provider, NULL},
        {"--config", 0, true, &cli->config_path, NULL},
        {"--effort", 0, true, &cli->effort, NULL},
        {"--listen", 's', true, &cli->irc_listen, NULL},
        {"--client", 'c', true, NULL, NULL},
        {"--last", 0, false, NULL, &cli->last},
        {"--all", 0, false, NULL, &cli->all},
        {"--resume", 0, false, NULL, &cli->resume},
        {"--no-listen", 0, false, NULL, &cli->irc_no_listen},
        {"--no-client", 0, false, NULL, &cli->irc_no_client},
        {NULL, 'e', false, NULL, &cli->execute},
        {NULL, 'l', false, NULL, &cli->list},
        {"--help", 'h', false, NULL, &cli->help},
        {"--update-model-cache", 0, false, NULL, &cli->update_model_cache},
        {NULL, 'V', false, NULL, &cli->version},
        {NULL, 'v', false, NULL, NULL},
        {NULL, 'd', true, NULL, NULL},
        {"--color", 0, true, NULL, NULL},
        {"--no-color", 0, false, NULL, NULL},
        {"--markdown", 0, false, NULL, NULL},
        {"--no-markdown", 0, false, NULL, NULL},
    };
    const char *arg = argv[*index], *p = arg + 1;
    bool long_option = *p == '-';

    do {
        const struct option *option = NULL;
        const char *attached = NULL;
        char short_name[] = {'-', *p, '\0'};
        for (size_t j = 0; j < sizeof(options) / sizeof(options[0]); ++j) {
            const struct option *candidate = &options[j];
            if (long_option) {
                if (!candidate->name)
                    continue;
                size_t len = strlen(candidate->name);
                if (strncmp(arg, candidate->name, len) ||
                    (arg[len] && !(candidate->argument && arg[len] == '=')))
                    continue;
                attached = arg[len] ? arg + len + 1u : NULL;
            } else {
                if (candidate->short_name != *p)
                    continue;
                attached = p + 1;
            }
            option = candidate;
            break;
        }
        if (!option) {
            snag_errorf(error, error_size, "unknown option %s", long_option ? arg : short_name);
            if (!long_option)
                errno = EINVAL;
            return -1;
        }
        const char *name = long_option ? option->name : short_name;
        char flag = option->short_name;
        if (option->toggle) {
            if (*option->toggle && flag != 'h' && flag != 'V')
                return snag_errorf(error, error_size, "duplicate %s option", name);
            *option->toggle = true;
            if (flag == 'h' && long_option)
                cli->manual = true;
        } else if (flag == 'v') {
            if (cli->verbosity == SNAG_VERBOSITY_MAX)
                return snag_errorf(error, error_size, "at most six -v flags are allowed");
            ++cli->verbosity;
        } else if (flag == 'c' || flag == 's') {
            if (long_option && attached && !*attached)
                return snag_fail(error, error_size, EINVAL,
                                 "%s= requires a nonempty endpoint", name);
            const char *value = optional_endpoint(argc, argv, index, attached);
            if ((flag == 'c' ? add_client(cli, value, error, error_size) :
                 set_once(option->slot, value, name, error, error_size)) < 0)
                return -1;
        } else if (option->slot || flag == 'd') {
            const char *value = option_argument(argc, argv, index, attached, name, error, error_size);
            if (!value || (option->slot ?
                set_once(option->slot, value, name, error, error_size) :
                snag_instructions_add_directory(&cli->doc_instructions, value, error, error_size)) < 0)
                return -1;
        } else if (strcmp(name, "--color") == 0) {
            if (!attached && *index + 1 < argc &&
                (strcmp(argv[*index + 1], "auto") == 0 ||
                 strcmp(argv[*index + 1], "always") == 0 ||
                 strcmp(argv[*index + 1], "never") == 0))
                attached = argv[++*index];
            if ((attached ? parse_color_value(cli, attached, name, error, error_size) :
                 set_color(cli, SNAG_CLI_COLOR_ALWAYS, name, error, error_size)) < 0)
                return -1;
        } else if (strcmp(name, "--no-color") == 0) {
            if (set_color(cli, SNAG_CLI_COLOR_NEVER, name, error, error_size) < 0)
                return -1;
        } else if (set_markdown(cli, strcmp(name, "--markdown") == 0 ?
                               SNAG_CLI_MARKDOWN_ENABLED : SNAG_CLI_MARKDOWN_DISABLED,
                               name, error, error_size) < 0) {
            return -1;
        }
        if (long_option || option->argument)
            return 0;
    } while (*++p);
    return 0;
}

int
snag_cli_parse(struct snag_cli *cli, int argc, char **argv,
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
        if (parse_options(cli, argc, argv, &i, error, error_size) < 0)
            return -1;
    }
    if ((cli->help || cli->version) &&
        (argc != 2 ||
         (strcmp(argv[1], "-h") != 0 && strcmp(argv[1], "--help") != 0 && strcmp(argv[1], "-V") != 0)))
        return snag_errorf(error, error_size, "-h, --help and -V must stand alone");
    if (cli->help || cli->version)
        return 0;
    if ((cli->irc_no_listen && cli->irc_listen) ||
        (cli->irc_no_client && cli->irc_client_count))
        return snag_errorf(error, error_size, "conflicting positive and negative IRC role options");
    if (!cli->execute && !cli->resume && !dashdash && positional >= 0 &&
        (strcmp(argv[positional], "login") == 0 || strcmp(argv[positional], "logout") == 0))
        return parse_auth_command(cli, argc, argv, positional, error, error_size);
    if (cli->list && (cli->resume || cli->execute || cli->last || cli->workspace ||
                      cli->doc_instructions.count ||
                      cli->model || cli->provider || cli->effort || cli->verbosity ||
                      cli->irc_listen || cli->irc_no_listen || cli->irc_no_client ||
                      cli->irc_client_count || cli->irc_model_nick ||
                      cli->irc_operator_nick || cli->irc_room_name))
        return snag_errorf(error, error_size,
                  "-l accepts only --config, --dotdir, --all, --update-model-cache, and presentation options");
    if (cli->execute && (cli->irc_listen ||
                         cli->irc_client_count || cli->irc_model_nick ||
                         cli->irc_operator_nick || cli->irc_room_name))
        return snag_errorf(error, error_size,
                  "-e cannot be combined with network options");
    if ((cli->irc_listen || cli->irc_client_count) &&
        positional >= 0 && !dashdash && !cli->resume)
        return snag_errorf(error, error_size,
                  "networked initial chat text must follow --");
    if (cli->last && !cli->resume)
        return snag_errorf(error, error_size, "--last requires --resume");
    if (cli->all && !cli->resume && !cli->list)
        return snag_errorf(error, error_size, "--all requires --resume or -l");
    if (cli->model && !snag_text_valid(cli->model, 1u,
        SNAG_CONFIG_MODEL_MAX + SNAG_CONFIG_PROVIDER_NAME_MAX + SNAG_CONFIG_EFFORT_MAX + 1u))
        return snag_errorf(error, error_size,
                  "model exceeds the supported structural bounds");
    if (cli->provider && !snag_text_valid(cli->provider, 1u, SNAG_CONFIG_PROVIDER_NAME_MAX))
        return snag_errorf(error, error_size, "provider name is empty or oversized");
    if (cli->effort && !snag_text_valid(cli->effort, 1u, SNAG_CONFIG_EFFORT_MAX - 1u))
        return snag_errorf(error, error_size,
                  "reasoning effort exceeds the supported structural bounds");
    if (cli->resume) {
        if (positional >= 0 && !dashdash && !cli->last) {
            if (strlen(argv[positional]) > SNAG_ID_HEX_LEN)
                return snag_fail(error, error_size, EOVERFLOW, "session id is too long or unavailable");
            cli->resume_id = argv[positional];
            ++positional;
            if (positional < argc) {
                if (strcmp(argv[positional], "--") != 0)
                    return snag_errorf(error, error_size, "resume follow-up must follow --");
                dashdash = true;
                ++positional;
            }
        }
        if (cli->last && positional >= 0 && !dashdash)
            return snag_errorf(error, error_size, "--last cannot be combined with a session id");
        if (cli->all && cli->resume_id)
            return snag_errorf(error, error_size, "--all is invalid with an exact session id");
        if (positional >= 0 && positional < argc) {
            cli->prompt = snag_join_words(argv + positional, (size_t)(argc - positional),
                                         SNAG_MAX_DIRECT_PROMPT);
            if (!cli->prompt)
                return snag_errorf(error, error_size, "prompt is invalid or exceeds 1 MiB");
        }
    } else if (!cli->list && positional >= 0 && positional < argc) {
        if (cli->execute && !dashdash)
            return snag_errorf(error, error_size, "-e requires -- before its prompt");
        cli->prompt = snag_join_words(argv + positional, (size_t)(argc - positional),
                                     SNAG_MAX_DIRECT_PROMPT);
        if (!cli->prompt)
            return -1;
    }
    if (cli->execute && !cli->prompt &&
        read_execute_prompt(cli, error, error_size) < 0)
        return -1;
    if (cli->execute && !*cli->prompt)
        return snag_errorf(error, error_size, "-e requires a nonempty prompt");
    cli->prompt_after_dashdash = dashdash;
    return 0;
}

void
snag_cli_usage(int fd)
{
    static const char text[] =
        "usage: " SNAJPAGENT_NAME " [OPTIONS] [--] [INITIAL PROMPT...]\n"
        "       " SNAJPAGENT_NAME " --resume [OPTIONS] [SESSION_ID|--last] [-- FOLLOW-UP...]\n"
        "       " SNAJPAGENT_NAME " -e [OPTIONS] [-- PROMPT...]\n"
        "       " SNAJPAGENT_NAME " -l [OPTIONS]\n"
        "       " SNAJPAGENT_NAME " [OPTIONS] login [PROVIDER] [--device-auth|--with-api-key]\n"
        "       " SNAJPAGENT_NAME " [OPTIONS] login status [PROVIDER]\n"
        "       " SNAJPAGENT_NAME " [OPTIONS] logout [PROVIDER]\n"
        "  -s, --listen[=ENDPOINT]      host the IRC server on ENDPOINT\n"
        "  -c, --client[=ENDPOINT]      connect to IRC; repeatable\n"
        "      --no-listen              suppress the configured listener\n"
        "      --no-client              suppress configured outgoing connections\n"
        "  -n, --model-nick NICK        model nick (default agent0)\n"
        "  -o, --operator-nick NICK     local operator nick\n"
        "  -r, --room-name ROOM         hosted room name\n"
        "      --dotdir DIR             private application directory\n"
        "      --config FILE            explicit configuration file\n"
        "      --provider NAME          select a configured provider (also on resume)\n"
        "      --effort LEVEL           reasoning effort override\n"
        "      --update-model-cache     refresh the provider model catalog\n"
        "      --color[=WHEN]            auto, always, or never\n"
        "      --no-color               alias for --color=never\n"
        "      --markdown               render model Markdown (default)\n"
        "      --no-markdown            show model Markdown literally\n"
        "  -C DIR                       workspace (or resume relocation)\n"
        "  -d DIR                       additional working docs with AGENTS.md; repeatable\n"
        "  -m [PROVIDER/]MODEL[/EFFORT]  model for next turn (start or resume)\n"
        "  -v                           exact detail level: repeat 1 through 6 times\n"
        "                               1 tools; 2 previews; 3 full tools;\n"
        "                               4 debug; 5 protocol; 6 wire (default 0)\n"
        "      --resume [ID|--last]      resume a durable session\n"
        "      --all                    include sessions from all workspaces\n"
        "  -e                           one-shot execution (prompt or stdin)\n"
        "  -l                           list sessions\n"
        "  -h                           show short help\n"
        "      --help                   open the manual (short help if unavailable)\n"
        "  -V                           show version\n";
    (void)snag_write_full(fd, text, sizeof(text) - 1u);
}

void
snag_cli_help(bool manual)
{
#ifndef _WIN32
    if (manual) {
        int status;
        pid_t child = fork(), got;
        if (child == 0) {
            int fd = open("/dev/null", O_WRONLY);
            if (fd >= 0) {
                (void)dup2(fd, STDERR_FILENO);
                if (fd != STDERR_FILENO) (void)close(fd);
            }
            execlp("man", "man", "1", SNAJPAGENT_NAME, (char *)NULL);
            _exit(127);
        }
        if (child > 0) {
            do {
                got = waitpid(child, &status, 0);
            } while (got < 0 && errno == EINTR);
            if (got == child && WIFEXITED(status) && WEXITSTATUS(status) == 0)
                return;
        }
    }
#else
    (void)manual;
#endif
    snag_cli_usage(STDOUT_FILENO);
}
