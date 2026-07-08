/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_CONFIG_H
#define SNAJPAGENT_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SNJ_CONFIG_MODEL_MAX 256u
#define SNJ_CONFIG_PATH_MAX (16u * 1024u)
#define SNJ_CONFIG_FILE_MAX (64u * 1024u)
#define SNJ_CONFIG_URL_MAX 2048u
#define SNJ_CONFIG_SECRET_ENV_MAX 64u
#define SNJ_CONFIG_ENV_NAME_MAX 255u

enum snj_color_mode {
    SNJ_COLOR_AUTO,
    SNJ_COLOR_NEVER
};

struct snj_config {
    char model[SNJ_CONFIG_MODEL_MAX];
    char reasoning_effort[16];
    uint32_t connect_timeout_ms;
    uint32_t idle_timeout_ms;
    uint32_t request_timeout_ms;
    uint32_t auto_compact_input_tokens;
    bool provider_exact_token_count;
    bool provider_native_compaction;
    char provider_base_url[SNJ_CONFIG_URL_MAX];
    char provider_api_key_env[SNJ_CONFIG_ENV_NAME_MAX + 1u];
    unsigned int verbosity;
    enum snj_color_mode color;
    unsigned int resume_history_turns;
    char *shell;
    uint32_t default_yield_ms;
    uint32_t default_timeout_ms;
    uint32_t max_timeout_ms;
    char *secret_env[SNJ_CONFIG_SECRET_ENV_MAX];
    size_t secret_env_count;
};

void snj_config_init(struct snj_config *config);
void snj_config_free(struct snj_config *config);
int snj_config_load(struct snj_config *config, const char *explicit_path,
                    char *error, size_t error_size);

#endif
