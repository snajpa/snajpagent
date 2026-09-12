/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_PROVIDER_H
#define SNAJPAGENT_PROVIDER_H

#include "config.h"
#include "credential.h"
#include "responses.h"
#include "turn.h"

#include "snag_jansson.h"
#include <stddef.h>

struct snag_ui;

typedef int (*snag_provider_pump_fn)(void *opaque, unsigned int timeout_ms);

/* Pump: -1 failure, 0 continue, 1 steer, 2 cancel, 3 new non-steering input.
 * New input lets a healthy response finish but prevents further retries. */
#define SNAG_PROVIDER_NEW_INPUT 3

/* Optional native operation is absent; no semantic output was returned. */
#define SNAG_PROVIDER_UNSUPPORTED 4
/* Typed context overflow from a count or native compaction operation. */
#define SNAG_PROVIDER_CONTEXT_OVERFLOW 5

/* Borrowed for one synchronous call; refreshed credentials remain private. */
struct snag_provider_connection {
    const struct snag_config *config;
    const struct snag_provider_config *provider;
    const struct snag_credential *credential;
    struct snag_ui *render;
    snag_provider_pump_fn pump;
    void *pump_opaque;
};

/* Fixed-issuer auth transport: bounded, cancellable, and never body-logged. */
int snag_provider_auth_post(const char *issuer, const char *path, const char *type,
                            const void *body, size_t size, json_t **response,
                            long *status, snag_provider_pump_fn pump, void *opaque,
                            char *error, size_t error_size);

/* Responses operations return 1 for steering or 2 for cancellation. */
int snag_provider_responses_count(struct snag_provider_connection connection,
                                 const json_t *request, uint64_t *input_tokens, bool *endpoint_unsupported,
                                 char *error, size_t error_size, unsigned int *retry_count);

int snag_provider_responses_compact(struct snag_provider_connection connection,
                                   const json_t *request, struct snag_json_document *output,
                                   char *error, size_t error_size, unsigned int *retry_count);

int snag_provider_responses_create(struct snag_provider_connection connection,
                                  const json_t *request, snag_responses_emit_fn emit,
                                  void *emit_opaque, struct snag_response_graph *graph,
                                  struct snag_provider_failure *failure, char *error, size_t error_size,
                                  unsigned int *retry_count);

int snag_provider_models_list(struct snag_provider_connection connection,
                             json_t **models, char *error, size_t error_size);

const char *snag_provider_catalog_protocol( const struct snag_provider_config *provider);

#endif
