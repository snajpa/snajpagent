/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_CONFIG_H
#define SNAJPAGENT_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SNAG_CONFIG_MODEL_MAX 256u
#define SNAG_CONFIG_EFFORT_MAX 64u
#define SNAG_CONFIG_PROMPT_MAX 1024u
#define SNAG_CONFIG_SPINNER_MAX 69u
#define SNAG_CONFIG_SPINNER_FRAMES_MAX 16u
#define SNAG_CONFIG_PATH_MAX (16u * 1024u)
#define SNAG_CONFIG_FILE_MAX (64u * 1024u)
#define SNAG_CONFIG_URL_MAX 2048u
#define SNAG_CONFIG_SECRET_ENV_MAX 64u
#define SNAG_CONFIG_ENV_NAME_MAX 255u
#define SNAG_CONFIG_PROVIDER_MAX 16u
#define SNAG_CONFIG_PROVIDER_NAME_MAX 63u
#define SNAG_CONFIG_MODEL_LIMIT_MAX 128u
#define SNAG_CONFIG_TOKEN_LIMIT_MAX UINT64_C(4000000000)
/* Outside the numeric compaction range; zero continues to mean disabled. */
#define SNAG_CONFIG_COMPACT_AUTO UINT32_MAX
#define SNAG_DEFAULT_TOOL_OUTPUT_TOKENS 6000u
#define SNAG_CONFIG_IRC_CLIENT_MAX 16u
#define SNAG_CONFIG_IRC_ENDPOINT_MAX 255u
#define SNAG_CONFIG_IRC_NICK_MAX 30u
#define SNAG_CONFIG_IRC_ROOM_MAX 50u

enum snag_color_mode {
    SNAG_COLOR_AUTO,
    SNAG_COLOR_ALWAYS,
    SNAG_COLOR_NEVER
};

enum snag_prompt_field {
    SNAG_PROMPT_PROVIDER,
    SNAG_PROMPT_MODEL,
    SNAG_PROMPT_EFFORT,
    SNAG_PROMPT_OPERATOR,
    SNAG_PROMPT_HOST,
    SNAG_PROMPT_CONTEXT,
    SNAG_PROMPT_MODE,
    SNAG_PROMPT_HOUR,
    SNAG_PROMPT_MINUTE,
    SNAG_PROMPT_SECOND,
    SNAG_PROMPT_FIELD_COUNT
};

enum snag_token_count_mode {
    SNAG_TOKEN_COUNT_AUTO,
    SNAG_TOKEN_COUNT_OFF,
    SNAG_TOKEN_COUNT_STRICT
};

enum snag_auth_kind {
    SNAG_AUTH_ENV,
    SNAG_AUTH_API_KEY,
    SNAG_AUTH_CHATGPT
};

#define SNAG_CHATGPT_BASE "https://chatgpt.com/backend-api/codex"

struct snag_provider_config {
    char name[SNAG_CONFIG_PROVIDER_NAME_MAX + 1u];
    enum snag_auth_kind auth;
    uint32_t connect_timeout_ms;
    uint32_t idle_timeout_ms;
    uint32_t request_timeout_ms;
    uint32_t auto_compact_input_tokens;
    enum snag_token_count_mode exact_token_count;
    bool native_compaction;
    bool parallel_tool_calls;
    char base_url[SNAG_CONFIG_URL_MAX];
    char api_key_env[SNAG_CONFIG_ENV_NAME_MAX + 1u];
    char openrouter_referer[SNAG_CONFIG_URL_MAX];
    char openrouter_title[SNAG_CONFIG_MODEL_MAX];
};

/* Configured/advertised limits use zero for unknown; positive values are known. */
struct snag_model_limit_config {
    char provider[SNAG_CONFIG_PROVIDER_NAME_MAX + 1u];
    char model[SNAG_CONFIG_MODEL_MAX];
    uint64_t context_window_tokens;
    uint64_t max_input_tokens;
    uint64_t max_output_tokens;
};

struct snag_config {
    char provider[SNAG_CONFIG_PROVIDER_NAME_MAX + 1u];
    char model[SNAG_CONFIG_MODEL_MAX];
    char reasoning_effort[SNAG_CONFIG_EFFORT_MAX];
    uint32_t max_goal_prompt_bytes;
    bool read_agents_md;
    struct snag_provider_config providers[SNAG_CONFIG_PROVIDER_MAX];
    size_t provider_count;
    struct snag_model_limit_config model_limits[SNAG_CONFIG_MODEL_LIMIT_MAX];
    size_t model_limit_count;
    enum snag_color_mode color;
    bool markdown;
    unsigned int resume_history_turns;
    uint32_t typing_pause_ms;
    char prompt[SNAG_CONFIG_PROMPT_MAX + 1u];
    char prompt_spinner_goal[SNAG_CONFIG_SPINNER_MAX];
    char prompt_spinner_provider[SNAG_CONFIG_SPINNER_MAX];
    char prompt_spinner_tool[SNAG_CONFIG_SPINNER_MAX];
    uint32_t prompt_spinner_per_second;
    bool irc_listen_explicit;
    char irc_listen[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char irc_clients[SNAG_CONFIG_IRC_CLIENT_MAX]
                    [SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    size_t irc_client_count;
    char irc_model_nick[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    char irc_operator_nick[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    bool irc_model_nick_implicit;
    bool irc_operator_nick_implicit;
    char irc_room_name[SNAG_CONFIG_IRC_ROOM_MAX + 2u];
    uint32_t irc_history_lines;
    char *shell;
    uint32_t default_yield_ms;
    uint32_t max_parallel_commands;
    uint32_t default_timeout_ms;
    uint32_t max_timeout_ms;
    uint32_t max_output_tokens;
    uint32_t max_output_bytes;
    char *secret_env[SNAG_CONFIG_SECRET_ENV_MAX];
    size_t secret_env_count;
};

void snag_config_init(struct snag_config *config);
void snag_config_free(struct snag_config *config);
int snag_config_load(struct snag_config *config, const char *explicit_path,
                    const char *dotdir,
                    char *error, size_t error_size);
char *snag_config_path(const char *explicit_path, const char *dotdir,
                      char *error, size_t error_size);
int snag_config_save_model(const char *path, bool allow_create,
                          const char *provider, const char *model,
                          const char *effort,
                          char *error, size_t error_size);
int snag_config_save_provider(const char *path, bool allow_create,
                             const struct snag_provider_config *provider,
                             const char *initial_model, const char *effort,
                             char *error, size_t error_size);
int snag_config_validate_provider(const struct snag_provider_config *provider,
                                 char *error, size_t error_size);
int snag_config_prompt_expand(const char *text, unsigned int mode,
                             const char *const values[SNAG_PROMPT_FIELD_COUNT],
                             unsigned char marker,
                             char *label, size_t label_size);
const struct snag_provider_config *snag_config_provider(
    const struct snag_config *config, const char *name);
bool snag_config_provider_is_openrouter(const struct snag_provider_config *provider);
const struct snag_model_limit_config *snag_config_model_limit(
    const struct snag_config *config, const char *provider, const char *model);

#endif
