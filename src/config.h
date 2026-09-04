/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_CONFIG_H
#define SNAJPAGENT_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SNJ_CONFIG_MODEL_MAX 256u
#define SNJ_CONFIG_EFFORT_MAX 64u
#define SNJ_CONFIG_PATH_MAX (16u * 1024u)
#define SNJ_CONFIG_FILE_MAX (64u * 1024u)
#define SNJ_CONFIG_URL_MAX 2048u
#define SNJ_CONFIG_SECRET_ENV_MAX 64u
#define SNJ_CONFIG_ENV_NAME_MAX 255u
#define SNJ_CONFIG_PROVIDER_MAX 16u
#define SNJ_CONFIG_PROVIDER_NAME_MAX 63u
#define SNJ_CONFIG_MODEL_LIMIT_MAX 128u
#define SNJ_CONFIG_TOKEN_LIMIT_MAX UINT64_C(4000000000)
#define SNJ_DEFAULT_TOOL_OUTPUT_TOKENS 4000u
#define SNJ_CONFIG_IRC_CLIENT_MAX 16u
#define SNJ_CONFIG_IRC_ENDPOINT_MAX 255u
#define SNJ_CONFIG_IRC_NICK_MAX 30u
#define SNJ_CONFIG_IRC_ROOM_MAX 50u

enum snj_color_mode {
    SNJ_COLOR_AUTO,
    SNJ_COLOR_ALWAYS,
    SNJ_COLOR_NEVER
};

struct snj_provider_config {
    char name[SNJ_CONFIG_PROVIDER_NAME_MAX + 1u];
    uint32_t connect_timeout_ms;
    uint32_t idle_timeout_ms;
    uint32_t request_timeout_ms;
    uint32_t auto_compact_input_tokens;
    bool exact_token_count;
    bool native_compaction;
    char base_url[SNJ_CONFIG_URL_MAX];
    char api_key_env[SNJ_CONFIG_ENV_NAME_MAX + 1u];
    char openrouter_referer[SNJ_CONFIG_URL_MAX];
    char openrouter_title[SNJ_CONFIG_MODEL_MAX];
};

struct snj_model_limit_config {
    char provider[SNJ_CONFIG_PROVIDER_NAME_MAX + 1u];
    char model[SNJ_CONFIG_MODEL_MAX];
    uint64_t context_window_tokens;
    uint64_t max_input_tokens;
    uint64_t max_output_tokens;
    bool context_window_known;
    bool max_input_known;
    bool max_output_known;
};

struct snj_config {
    char provider[SNJ_CONFIG_PROVIDER_NAME_MAX + 1u];
    char model[SNJ_CONFIG_MODEL_MAX];
    char reasoning_effort[SNJ_CONFIG_EFFORT_MAX];
    uint32_t max_goal_prompt_bytes;
    bool read_agents_md;
    struct snj_provider_config providers[SNJ_CONFIG_PROVIDER_MAX];
    size_t provider_count;
    struct snj_model_limit_config model_limits[SNJ_CONFIG_MODEL_LIMIT_MAX];
    size_t model_limit_count;
    unsigned int verbosity;
    enum snj_color_mode color;
    bool markdown;
    unsigned int resume_history_turns;
    uint32_t typing_pause_ms;
    bool irc_listen_explicit;
    char irc_listen[SNJ_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char irc_clients[SNJ_CONFIG_IRC_CLIENT_MAX]
                    [SNJ_CONFIG_IRC_ENDPOINT_MAX + 1u];
    size_t irc_client_count;
    char irc_model_nick[SNJ_CONFIG_IRC_NICK_MAX + 1u];
    char irc_operator_nick[SNJ_CONFIG_IRC_NICK_MAX + 1u];
    char irc_room_name[SNJ_CONFIG_IRC_ROOM_MAX + 2u];
    uint32_t irc_history_lines;
    char *shell;
    uint32_t default_yield_ms;
    uint32_t default_timeout_ms;
    uint32_t max_timeout_ms;
    uint32_t default_max_output_tokens;
    uint32_t max_output_bytes;
    char *secret_env[SNJ_CONFIG_SECRET_ENV_MAX];
    size_t secret_env_count;
};

void snj_config_init(struct snj_config *config);
void snj_config_free(struct snj_config *config);
int snj_config_load(struct snj_config *config, const char *explicit_path,
                    const char *dotdir,
                    char *error, size_t error_size);
char *snj_config_path(const char *explicit_path, const char *dotdir,
                      char *error, size_t error_size);
int snj_config_save_model(const char *path, bool allow_create,
                          const char *provider, const char *model,
                          const char *effort,
                          char *error, size_t error_size);
const struct snj_provider_config *snj_config_provider(
    const struct snj_config *config, const char *name);
const struct snj_model_limit_config *snj_config_model_limit(
    const struct snj_config *config, const char *provider, const char *model);

#endif
