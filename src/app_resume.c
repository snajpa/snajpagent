/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"
#include "base.h"
#include "cli.h"
#include "json.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int
option(json_t *args, const char *name, const char *value)
{
    if (json_array_append_new(args, json_string(name)) < 0) return -1;
    return value ? json_array_append_new(args, json_string(value)) : 0;
}

int
snag_app_save_resume_options(struct app_state *app, char *error, size_t error_size)
{
    if (!app->resume_options_ready || app->session.delete_requested) return 0;
    const struct snag_cli *cli = app->cli;
    const struct snag_config *config = app->config;
    json_t *args = json_array();
    if (!args) return -1;
    if (cli->config_path && option(args, "--config", app->config_path) < 0) goto fail;
    for (size_t i = 0u; i < cli->doc_instructions.count; ++i) {
        const char *path = cli->doc_instructions.paths[i];
        const char *slash = strrchr(path, '/');
        if (!slash || json_array_append_new(args, json_string("-d")) < 0 ||
            json_array_append_new(args, json_stringn(path, (size_t)(slash - path + 1))) < 0)
            goto fail;
    }
    enum snag_color_mode color = snag_cli_color(cli, config->color);
    if (option(args, "--color", color == SNAG_COLOR_ALWAYS ? "always" :
            color == SNAG_COLOR_NEVER ? "never" : "auto") < 0 ||
        option(args, snag_cli_markdown(cli, config->markdown) ? "--markdown" :
            "--no-markdown", NULL) < 0) goto fail;
    for (unsigned int i = 0u; i < snag_ui_verbosity(&app->ui); ++i)
        if (option(args, "-v", NULL) < 0) goto fail;
    if (option(args, config->irc.listen_explicit ? "--listen" : "--no-listen",
            config->irc.listen_explicit ? config->irc.listen : NULL) < 0) goto fail;
    if (!config->irc.client_count && option(args, "--no-client", NULL) < 0) goto fail;
    for (size_t i = 0u; i < config->irc.client_count; ++i)
        if (option(args, "--client", config->irc.clients[i]) < 0) goto fail;
    if (config->irc.model_nick[0] && !config->irc.model_nick_implicit &&
        option(args, "--model-nick", config->irc.model_nick) < 0) goto fail;
    if (config->irc.operator_nick[0] && !config->irc.operator_nick_implicit &&
        option(args, "--operator-nick", config->irc.operator_nick) < 0) goto fail;
    if (config->irc.room_name[0] && option(args, "--room-name", config->irc.room_name) < 0)
        goto fail;
    if (json_equal(args, json_object_get(app->session.strings, "resume_options"))) {
        json_decref(args);
        return 0;
    }
    return snag_app_commit_event(app, "session_options", json_pack("{s:o}", "args", args),
                                  error, error_size);
fail:
    json_decref(args);
    return snag_errorf(error, error_size, "cannot retain session resume options");
}

/* Older journals already retain the last actual IRC topology. Only recognized
 * metadata lines become arguments; chat contents never become launch options. */
static json_t *
legacy_options(const struct snag_session *session)
{
    const char *snapshot = snag_json_string(session->strings, "irc_snapshot");
    json_t *args = json_array();
    char hosted[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u] = "";
    bool clients = false, room = false;
    if (!args || !snapshot || !*snapshot) return args;
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
                if (option(args, name, !strcmp(name, "--no-listen") ? NULL : text) < 0)
                    goto fail;
                if (!strcmp(name, "--client")) clients = true;
            }
        }
        line += length + (end ? 1u : 0u);
    }
    if (!clients && option(args, "--no-client", NULL) < 0) goto fail;
    return args;
fail:
    json_decref(args);
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
