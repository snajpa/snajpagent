/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"
#include "base.h"
#include "cli.h"
#include "json.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct resume_options {
    const json_t *stored;
    json_t *args;
    size_t used;
};

static int
retain_prefix(struct resume_options *options)
{
    options->args = json_array();
    if (!options->args) return -1;
    for (size_t i = 0u; i < options->used; ++i)
        if (json_array_append(options->args, json_array_get(options->stored, i)) < 0) return -1;
    return 0;
}

/* Borrow unchanged arguments and allocate only when the persisted choices differ.
 * View/verbosity commands can then save immediately without rebuilding the list. */
static int
argument(struct resume_options *options, const char *text, size_t length)
{
    json_t *previous = json_array_get(options->stored, options->used);
    bool same = json_is_string(previous) && json_string_length(previous) == length &&
        !memcmp(json_string_value(previous), text, length);
    if (!same && !options->args && retain_prefix(options) < 0) return -1;
    if (options->args) {
        int rc = same ? json_array_append(options->args, previous) :
            json_array_append_new(options->args, json_stringn(text, length));
        if (rc < 0) return -1;
    }
    ++options->used;
    return 0;
}

static int
option(struct resume_options *options, const char *name, const char *value)
{
    if (argument(options, name, strlen(name)) < 0) return -1;
    return value ? argument(options, value, strlen(value)) : 0;
}

int
snag_app_save_resume_options(struct app_state *app, char *error, size_t error_size)
{
    if (!app->resume_options_ready || app->session.delete_requested) return 0;
    const struct snag_cli *cli = app->cli;
    const struct snag_config *config = app->config;
    struct resume_options options = {
        .stored = json_object_get(app->session.strings, "resume_options")};
    if (cli->config_path && option(&options, "--config", app->config_path) < 0) goto fail;
    for (size_t i = 0u; i < cli->doc_instructions.count; ++i) {
        const char *path = cli->doc_instructions.paths[i];
        const char *slash = strrchr(path, '/');
        if (!slash || option(&options, "-d", NULL) < 0 ||
            argument(&options, path, (size_t)(slash - path + 1)) < 0)
            goto fail;
        if (cli->doc_instructions.labels[i] &&
            option(&options, "-dl", cli->doc_instructions.labels[i]) < 0)
            goto fail;
    }
    enum snag_color_mode color = snag_cli_color(cli, config->color);
    if (option(&options, "--color", color == SNAG_COLOR_ALWAYS ? "always" :
            color == SNAG_COLOR_NEVER ? "never" : "auto") < 0 ||
        option(&options, snag_cli_markdown(cli, config->markdown) ? "--markdown" :
            "--no-markdown", NULL) < 0) goto fail;
    for (unsigned int i = 0u; i < snag_ui_verbosity(&app->ui); ++i)
        if (option(&options, "-v", NULL) < 0) goto fail;
    if (option(&options, config->irc.listen_explicit ? "--listen" : "--no-listen",
            config->irc.listen_explicit ? config->irc.listen : NULL) < 0) goto fail;
    if (!config->irc.client_count && option(&options, "--no-client", NULL) < 0) goto fail;
    for (size_t i = 0u; i < config->irc.client_count; ++i)
        if (option(&options, "--client", config->irc.clients[i]) < 0) goto fail;
    if (config->irc.model_nick[0] && !config->irc.model_nick_implicit &&
        option(&options, "--model-nick", config->irc.model_nick) < 0) goto fail;
    if (config->irc.operator_nick[0] && !config->irc.operator_nick_implicit &&
        option(&options, "--operator-nick", config->irc.operator_nick) < 0) goto fail;
    if (config->irc.room_name[0] && option(&options, "--room-name", config->irc.room_name) < 0)
        goto fail;
    const char *view = cli->execute ? (!cli->view ? NULL :
        cli->view == SNAG_CLI_VIEW_CHAT ? "chat" : "rollout") :
        snag_ui_view(&app->ui) == SNAG_RENDER_CHAT ? "chat" : "rollout";
    if (view && option(&options, "--view", view) < 0) goto fail;
    if (!options.args && options.used == json_array_size(options.stored)) return 0;
    if (!options.args && retain_prefix(&options) < 0) goto fail;
    return snag_app_commit_event(app, "session_options", json_pack("{s:o}", "args", options.args),
                                  error, error_size);
fail:
    json_decref(options.args);
    return snag_errorf(error, error_size, "cannot retain session resume options");
}

/* Older journals already retain the last actual IRC topology. Only recognized
 * metadata lines become arguments; chat contents never become launch options. */
static json_t *
legacy_options(const struct snag_session *session)
{
    const char *snapshot = snag_json_string(session->strings, "irc_snapshot");
    struct resume_options options = {.args = json_array()};
    char hosted[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u] = "";
    bool clients = false, room = false;
    if (!options.args || !snapshot || !*snapshot) return options.args;
    for (const char *line = snapshot; *line; ) {
        const char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        const char *name = NULL, *value = NULL;
        if (length == 8u && !memcmp(line, "history:", 8u)) break;
        if (!strncmp(line, "model nick: ", 12u)) {
            name = "--model-nick";
            value = line + 12u;
        } else if (!strncmp(line, "operator nick: ", 15u)) {
            name = "--operator-nick";
            value = line + 15u;
        } else if (!strncmp(line, "hosted: ", 8u)) {
            name = "--listen";
            value = line + 8u;
        } else if (!strncmp(line, "destination[", 12u)) {
            const char *colon = memchr(line, ':', length);
            if (colon && colon > line && colon[-1] == ']' && colon[1] == ' ') {
                name = "--client";
                value = colon + 2u;
            }
        } else if (!room && !strncmp(line, "room: ", 6u)) {
            name = "--room-name";
            value = line + 6u;
            room = true;
        }
        if (value) {
            size_t size = length - (size_t)(value - line);
            char text[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
            if (size >= sizeof(text)) goto fail;
            memcpy(text, value, size);
            text[size] = '\0';
            if (!strcmp(name, "--listen")) {
                if (!strcmp(text, "no")) name = "--no-listen";
                else memcpy(hosted, text, size + 1u);
            }
            if (strcmp(name, "--client") || strcmp(text, hosted)) {
                if (option(&options, name, !strcmp(name, "--no-listen") ? NULL : text) < 0)
                    goto fail;
                if (!strcmp(name, "--client")) clients = true;
            }
        }
        line += length + (end ? 1u : 0u);
    }
    if (!clients && option(&options, "--no-client", NULL) < 0) goto fail;
    return options.args;
fail:
    json_decref(options.args);
    return NULL;
}

int
snag_app_restore_resume_options(struct snag_cli *effective, struct snag_cli *saved,
    const struct snag_session *session, json_t **retained, char *error, size_t error_size)
{
    json_t *stored = json_object_get(session->strings, "resume_options");
    json_t *args = stored ? json_incref(stored) : legacy_options(session);
    if (!args) return -1;
    *retained = args;
    size_t count = json_array_size(args);
    if (!snag_session_options_valid(args) || count > INT_MAX - 1u)
        return snag_errorf(error, error_size, "invalid saved resume options");
    char **argv = calloc(count + 2u, sizeof(*argv));
    if (!argv) return -1;
    argv[0] = "snajpagent";
    size_t used = 1u;
    for (size_t i = 0u; i < count; ++i) {
        const char *arg = json_string_value(json_array_get(args, i));
        /* Explicit replacement directories need not keep removed old roots
         * readable just to discard them during argument merging. */
        if (!strcmp(arg, "-d") && effective->doc_instructions.count) {
            ++i;
            continue;
        }
        argv[used++] = (char *)arg;
    }
    int rc = snag_cli_parse(saved, (int)used, argv, error, error_size);
    free(argv);
    if (rc < 0) return -1;
    if (!effective->config_path) effective->config_path = saved->config_path;
    if (!effective->color) effective->color = saved->color;
    if (!effective->markdown) effective->markdown = saved->markdown;
    if (!effective->view) effective->view = saved->view;
    if (!effective->verbosity) effective->verbosity = saved->verbosity;
    if (!effective->doc_instructions.count) effective->doc_instructions = saved->doc_instructions;
    if (!effective->irc_listen && !effective->irc_no_listen) {
        effective->irc_listen = saved->irc_listen;
        effective->irc_no_listen = saved->irc_no_listen;
    }
    if (!effective->irc_client_count && !effective->irc_no_client) {
        effective->irc_client_count = saved->irc_client_count;
        memcpy(effective->irc_clients, saved->irc_clients, sizeof(effective->irc_clients));
        effective->irc_no_client = saved->irc_no_client;
    }
    if (!effective->irc_model_nick) effective->irc_model_nick = saved->irc_model_nick;
    if (!effective->irc_operator_nick) effective->irc_operator_nick = saved->irc_operator_nick;
    if (!effective->irc_room_name) effective->irc_room_name = saved->irc_room_name;
    return 0;
}
