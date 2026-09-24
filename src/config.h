/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_CONFIG_H
#define SNAJPAGENT_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "secret_source.h"
#include "snag_jansson.h"

#define SNAG_CONFIG_MODEL_MAX 256u
#define SNAG_CONFIG_EFFORT_MAX 64u
#define SNAG_CONFIG_PROMPT_MAX 1024u
#define SNAG_CONFIG_PAGER_MAX 256u
#define SNAG_CONFIG_SPINNER_MAX 69u
#define SNAG_CONFIG_PATH_MAX (16u * 1024u)
#define SNAG_CONFIG_FILE_MAX (64u * 1024u)
#define SNAG_CONFIG_URL_MAX 2048u
#define SNAG_CONFIG_ENV_NAME_MAX 255u
#define SNAG_CONFIG_PROVIDER_NAME_MAX 63u
#define SNAG_CONFIG_STEERING_MAX 8u
#define SNAG_CONFIG_TOKEN_LIMIT_MAX UINT64_C(4000000000)
#define SNAG_CONFIG_OUTPUT_CACHE_MAX (64u * 1024u * 1024u)
/* Outside the numeric compaction range; zero continues to mean disabled. */
#define SNAG_CONFIG_COMPACT_AUTO UINT32_MAX
#define SNAG_DEFAULT_TOOL_OUTPUT_TOKENS 6000u
#define SNAG_CONFIG_IRC_CLIENT_MAX 16u
#define SNAG_CONFIG_IRC_ENDPOINT_MAX 255u
#define SNAG_CONFIG_IRC_NICK_MAX 30u
#define SNAG_CONFIG_IRC_ROOM_MAX 50u

enum snag_color_mode {
    SNAG_COLOR_AUTO, SNAG_COLOR_ALWAYS, SNAG_COLOR_NEVER };

enum snag_prompt_field {
    SNAG_PROMPT_PROVIDER, SNAG_PROMPT_MODEL, SNAG_PROMPT_EFFORT, SNAG_PROMPT_OPERATOR,
    SNAG_PROMPT_HOST, SNAG_PROMPT_CONTEXT, SNAG_PROMPT_MODE, SNAG_PROMPT_QUEUE,
    SNAG_PROMPT_HOUR, SNAG_PROMPT_MINUTE, SNAG_PROMPT_SECOND, SNAG_PROMPT_FIELD_COUNT };

enum snag_token_count_mode {
    SNAG_TOKEN_COUNT_AUTO, SNAG_TOKEN_COUNT_OFF, SNAG_TOKEN_COUNT_STRICT };

enum snag_auth_kind {
    SNAG_AUTH_API_KEY, SNAG_AUTH_CHATGPT, SNAG_AUTH_META };

#define SNAG_META_BASE "https://api.meta.ai/v1"
#define SNAG_META_BASE_BARE "https://api.meta.ai"

static inline bool
snag_is_meta_base(const char *url)
{
    return url && (strcmp(url, SNAG_META_BASE) == 0 ||
            strcmp(url, SNAG_META_BASE_BARE) == 0);
}

#define SNAG_CHATGPT_BASE "https://chatgpt.com/backend-api/codex"

/* Bounded desired networking state; socket health belongs to the IRC owners. */
struct snag_irc_config {
    bool listen_explicit;
    char listen[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    char clients[SNAG_CONFIG_IRC_CLIENT_MAX][SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    size_t client_count;
    char model_nick[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    char operator_nick[SNAG_CONFIG_IRC_NICK_MAX + 1u];
    bool model_nick_implicit;
    bool operator_nick_implicit;
    char room_name[SNAG_CONFIG_IRC_ROOM_MAX + 2u];
    uint32_t history_lines;
};

struct snag_provider_model {
    char name[SNAG_CONFIG_PROVIDER_NAME_MAX + 1u];
    char upstream[SNAG_CONFIG_MODEL_MAX];
};

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
    /* Endpoints whose chat templates reject instruction roles after the first
     * message (llama.cpp) receive the trailing host boundary in the user
     * transport slot instead of a developer item. */
    bool leading_instructions;
    char base_url[SNAG_CONFIG_URL_MAX];
    struct snag_secret_source api_key;
    char openrouter_referer[SNAG_CONFIG_URL_MAX];
    char openrouter_title[SNAG_CONFIG_MODEL_MAX];
    struct snag_provider_model *models;
    size_t model_count;
};

enum snag_model_execution_field {
    SNAG_MODEL_EXEC_DEFAULT_YIELD = 1u << 0,
    SNAG_MODEL_EXEC_MAX_WAIT = 1u << 1,
    SNAG_MODEL_EXEC_MAX_PARALLEL = 1u << 2,
    SNAG_MODEL_EXEC_DEFAULT_TIMEOUT = 1u << 3,
    SNAG_MODEL_EXEC_MAX_TIMEOUT = 1u << 4,
    SNAG_MODEL_EXEC_TOOL_OUTPUT = 1u << 5,
    SNAG_MODEL_EXEC_OUTPUT_CACHE = 1u << 6
};

struct snag_execution_config {
    uint32_t default_yield_ms, max_wait_ms, max_parallel_commands;
    uint32_t default_timeout_ms, max_timeout_ms;
    uint32_t tool_output_bytes, output_cache_bytes;
};

/* Session context-window selection for the selected model. Mode names are
 * durable session-log values and must stay stable. */
enum snag_context_mode {
    SNAG_CONTEXT_MODE_DEFAULT = 0, /* the provider's normal working window */
    SNAG_CONTEXT_MODE_MAX,         /* the advertised maximum client ceiling */
    SNAG_CONTEXT_MODE_TOKENS       /* explicit operator token count */
};

struct snag_context_choice {
    enum snag_context_mode mode;
    uint64_t tokens; /* SNAG_CONTEXT_MODE_TOKENS only */
};

/* Configured/advertised limits use zero for unknown; positive values are known. */
struct snag_model_limit_config {
    char provider[SNAG_CONFIG_PROVIDER_NAME_MAX + 1u];
    char model[SNAG_CONFIG_MODEL_MAX];
    char steering[SNAG_CONFIG_STEERING_MAX + 1u];
    uint64_t context_window_tokens;
    uint64_t max_input_tokens;
    uint64_t max_output_tokens;
    uint64_t image_tokens; /* Per-image ceiling for local image-request bounding. */
    uint32_t execution_present;
    uint32_t default_yield_ms, max_wait_ms, max_parallel_commands;
    uint32_t default_timeout_ms, max_timeout_ms;
    uint32_t tool_output_bytes, output_cache_bytes;
    json_t *reasoning_efforts; /* Owned by config; resolved rules borrow it. */
};

struct snag_audio_config {
    char provider[SNAG_CONFIG_PROVIDER_NAME_MAX + 1u];
    char listen_model[SNAG_CONFIG_MODEL_MAX];
    char transcribe_model[SNAG_CONFIG_MODEL_MAX];
    char speech_model[SNAG_CONFIG_MODEL_MAX];
    char realtime_model[SNAG_CONFIG_MODEL_MAX];
    char voice[128];
    char capture_device[256], playback_device[256];
};
struct snag_rules;

struct snag_config {
    struct snag_audio_config audio;
    char provider[SNAG_CONFIG_PROVIDER_NAME_MAX + 1u];
    char model[SNAG_CONFIG_MODEL_MAX];
    char reasoning_effort[SNAG_CONFIG_EFFORT_MAX];
    uint32_t max_goal_prompt_bytes;
    uint32_t max_turn_retries;
    bool read_agents_md;
    bool auto_update;
    char update_url[SNAG_CONFIG_URL_MAX];
    struct snag_provider_config *providers;
    size_t provider_count, provider_capacity;
    struct snag_model_limit_config *model_limits;
    size_t model_limit_count, model_limit_capacity;
    enum snag_color_mode color;
    bool markdown;
    uint64_t resume_history_turns;
    uint32_t typing_pause_ms;
    char pager[SNAG_CONFIG_PAGER_MAX];
    char prompt[SNAG_CONFIG_PROMPT_MAX + 1u];
    char prompt_spinner_goal[SNAG_CONFIG_SPINNER_MAX];
    char prompt_spinner_provider[SNAG_CONFIG_SPINNER_MAX];
    char prompt_spinner_tool[SNAG_CONFIG_SPINNER_MAX];
    uint32_t prompt_spinner_per_second, prompt_tool_spinner_off_delay_ms;
    struct snag_irc_config irc;
    char *shell;
    uint32_t default_yield_ms;
    uint32_t max_wait_ms;
    uint32_t max_parallel_commands;
    uint32_t default_timeout_ms;
    uint32_t max_timeout_ms;
    uint32_t max_output_tokens;
    uint32_t max_output_bytes;
    uint32_t output_cache_bytes;
    struct snag_rules *rules;
    struct snag_secret_source *secrets;
    size_t secret_count, secret_capacity;
    char source_path[SNAG_CONFIG_PATH_MAX + 1u];
};

void snag_config_init(struct snag_config *config);
/* Growable protected-value list; parses and retains one additional source. */
int snag_config_add_secret(struct snag_config *config, const char *value, const char *source_path,
                           char *error, size_t error_size);
void snag_config_provider_init(struct snag_provider_config *provider, const char *name);
bool snag_config_name_valid(const char *name);
void snag_config_free(struct snag_config *config);
bool snag_config_efforts_valid(const json_t *efforts);
int snag_config_load(struct snag_config *config, const char *explicit_path, const char *dotdir,
                          char *error, size_t error_size);
int snag_config_shell_validate(const char *shell, char *error, size_t error_size);
char *snag_config_path(const char *explicit_path, const char *dotdir, char *error, size_t error_size);
int snag_config_save_model(const char *path, bool allow_create, const char *provider, const char *model,
                          const char *effort, char *error, size_t error_size);
int snag_config_save_provider(const char *path, bool allow_create,
                             const struct snag_provider_config *provider,
                             const char *initial_model, const char *effort, char *error, size_t error_size);
int snag_config_validate_provider(const struct snag_provider_config *provider,
                                 char *error, size_t error_size);
int snag_config_prompt_expand(const char *text, unsigned int mode,
                             const char *const values[SNAG_PROMPT_FIELD_COUNT], unsigned char marker,
                             char *label, size_t label_size);
const struct snag_provider_config *snag_config_provider( const struct snag_config *config, const char *name);
bool snag_config_provider_is_openrouter(const struct snag_provider_config *provider);
const char *snag_config_model_upstream(const struct snag_provider_config *provider, const char *model);
/* Returns numeric-limit presence; out also borrows the resolved effort list. */
bool snag_config_resolve_limits(const struct snag_config *config, const char *provider, const char *model,
                               struct snag_model_limit_config *out,
                               const struct snag_model_limit_config *sources[3]);
int snag_config_resolve_execution(const struct snag_config *config, const char *provider,
                                  const char *model, struct snag_execution_config *out,
                                  char *error, size_t error_size);

/* Exact provider+model entry for per-model IRC steering; NULL when absent. */
const struct snag_model_limit_config *
snag_config_model_limit_exact(const struct snag_config *config,
    const char *provider, const char *model);

#endif
