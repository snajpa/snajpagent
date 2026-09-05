/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_AUTH_H
#define SNAJPAGENT_AUTH_H

#include "config.h"
#include "credential.h"
#include "snag_jansson.h"

typedef int (*snag_auth_pump_fn)(void *, uint32_t);

struct snag_auth_tokens {
    struct snag_credential credential;
    char refresh_token[SNAG_CREDENTIAL_MAX + 1u];
    uint64_t expires_at_ms;
};

void snag_auth_clear(struct snag_auth_tokens *tokens);
void snag_auth_json_free(json_t *value);
const char *snag_auth_kind_name(enum snag_auth_kind kind);
int snag_auth_key(struct snag_auth_tokens *tokens, const char *key,
                  char *error, size_t error_size);
/* Offline load returns 1 for a missing credential, -1 for an invalid store. */
int snag_auth_load(int root_fd, const struct snag_provider_config *provider,
                  struct snag_auth_tokens *tokens, char *error, size_t error_size);
int snag_auth_save(int root_fd, const struct snag_provider_config *provider,
                  const struct snag_auth_tokens *tokens,
                  struct snag_auth_tokens *previous,
                  snag_auth_pump_fn pump, void *opaque,
                  char *error, size_t error_size);
int snag_auth_restore(int root_fd, const struct snag_provider_config *provider,
                     const struct snag_auth_tokens *expected,
                     const struct snag_auth_tokens *previous,
                     char *error, size_t error_size);
int snag_auth_logout(int root_fd, const struct snag_provider_config *provider,
                    snag_auth_pump_fn pump, void *opaque,
                    char *error, size_t error_size);
/* force only refreshes when the stored access token still equals stale. */
int snag_auth_read(int root_fd, const struct snag_provider_config *provider,
                  bool force, const char *stale, struct snag_credential *out,
                  snag_auth_pump_fn pump, void *opaque,
                  char *error, size_t error_size);
int snag_auth_device(struct snag_auth_tokens *tokens,
                    snag_auth_pump_fn pump, void *opaque,
                    char *error, size_t error_size);
int snag_auth_refresh(struct snag_auth_tokens *tokens,
                     snag_auth_pump_fn pump, void *opaque,
                     char *error, size_t error_size);
int snag_auth_token_response(json_t *response, struct snag_auth_tokens *tokens,
                            char *error, size_t error_size);

#endif
