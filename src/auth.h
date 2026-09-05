/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_AUTH_H
#define SNAJPAGENT_AUTH_H

#include "config.h"
#include "credential.h"
#include "snj_jansson.h"

typedef int (*snj_auth_pump_fn)(void *, uint32_t);

struct snj_auth_tokens {
    struct snj_credential credential;
    char refresh_token[SNJ_CREDENTIAL_MAX + 1u];
    uint64_t expires_at_ms;
};

void snj_auth_clear(struct snj_auth_tokens *tokens);
void snj_auth_json_free(json_t *value);
const char *snj_auth_kind_name(enum snj_auth_kind kind);
int snj_auth_key(struct snj_auth_tokens *tokens, const char *key,
                  char *error, size_t error_size);
/* Offline load returns 1 for a missing credential, -1 for an invalid store. */
int snj_auth_load(int root_fd, const struct snj_provider_config *provider,
                  struct snj_auth_tokens *tokens, char *error, size_t error_size);
int snj_auth_save(int root_fd, const struct snj_provider_config *provider,
                  const struct snj_auth_tokens *tokens,
                  struct snj_auth_tokens *previous,
                  snj_auth_pump_fn pump, void *opaque,
                  char *error, size_t error_size);
int snj_auth_restore(int root_fd, const struct snj_provider_config *provider,
                     const struct snj_auth_tokens *expected,
                     const struct snj_auth_tokens *previous,
                     char *error, size_t error_size);
int snj_auth_logout(int root_fd, const struct snj_provider_config *provider,
                    snj_auth_pump_fn pump, void *opaque,
                    char *error, size_t error_size);
/* force only refreshes when the stored access token still equals stale. */
int snj_auth_read(int root_fd, const struct snj_provider_config *provider,
                  bool force, const char *stale, struct snj_credential *out,
                  snj_auth_pump_fn pump, void *opaque,
                  char *error, size_t error_size);
int snj_auth_device(struct snj_auth_tokens *tokens,
                    snj_auth_pump_fn pump, void *opaque,
                    char *error, size_t error_size);
int snj_auth_refresh(struct snj_auth_tokens *tokens,
                     snj_auth_pump_fn pump, void *opaque,
                     char *error, size_t error_size);
int snj_auth_token_response(json_t *response, struct snj_auth_tokens *tokens,
                            char *error, size_t error_size);

#endif
