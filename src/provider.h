/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_PROVIDER_H
#define SNAJPAGENT_PROVIDER_H

#include "config.h"
#include "credential.h"
#include "ui.h"
#include "responses.h"
#include "turn.h"

#include "snj_jansson.h"
#include <stddef.h>

typedef int (*snj_provider_pump_fn)(void *opaque, unsigned int timeout_ms);

/* Optional native operation is absent; no semantic output was returned. */
#define SNJ_PROVIDER_UNSUPPORTED 4

/* Fixed-issuer auth transport: bounded, cancellable, and never body-logged. */
int snj_provider_auth_post(const char *issuer, const char *path, const char *type,
                            const void *body, size_t size, json_t **response,
                            long *status, snj_provider_pump_fn pump, void *opaque,
                            char *error, size_t error_size);

int snj_provider_responses_count(const json_t *count_request,
                                 const struct snj_config *config,
                                 const struct snj_provider_config *provider,
                                 const struct snj_credential *credential,
                                 struct snj_ui *render,
                                 snj_provider_pump_fn pump,
                                 void *pump_opaque,
                                 uint64_t *input_tokens,
                                 bool *endpoint_unsupported,
                                 char *error, size_t error_size,
                                 int *cancel_code,
                                 unsigned int *retry_count);

int snj_provider_responses_compact(const json_t *compact_request,
                                   const struct snj_config *config,
                                   const struct snj_provider_config *provider,
                                   const struct snj_credential *credential,
                                   struct snj_ui *render,
                                   snj_provider_pump_fn pump,
                                   void *pump_opaque,
                                   json_t **output,
                                   uint64_t *output_tokens_bound,
                                   char *error, size_t error_size,
                                   int *cancel_code,
                                   unsigned int *retry_count);

int snj_provider_responses_create(const json_t *create_request,
                                  const struct snj_config *config,
                                  const struct snj_provider_config *provider,
                                  const struct snj_credential *credential,
                                  struct snj_ui *render,
                                  snj_responses_emit_fn emit,
                                  void *emit_opaque,
                                  snj_provider_pump_fn pump,
                                  void *pump_opaque,
                                  struct snj_response_graph *graph,
                                  struct snj_provider_failure *failure,
                                  char *error, size_t error_size,
                                  int *cancel_code,
                                  unsigned int *retry_count);

int snj_provider_models_list(const struct snj_config *config,
                             const struct snj_provider_config *provider,
                             const struct snj_credential *credential,
                             struct snj_ui *render,
                             snj_provider_pump_fn pump, void *pump_opaque,
                             json_t **models,
                             char *error, size_t error_size);

const char *snj_provider_catalog_protocol(
    const struct snj_provider_config *provider);

#endif
