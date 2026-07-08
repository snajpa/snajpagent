/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_PROVIDER_H
#define SNAJPAGENT_PROVIDER_H

#include "config.h"
#include "credential.h"
#include "render.h"
#include "responses.h"
#include "turn.h"

#include "snj_jansson.h"
#include <stddef.h>

typedef int (*snj_provider_pump_fn)(void *opaque, unsigned int timeout_ms);

int snj_provider_responses_count(const json_t *count_request,
                                 const struct snj_config *config,
                                 const struct snj_credential *credential,
                                 struct snj_render *render,
                                 snj_provider_pump_fn pump,
                                 void *pump_opaque,
                                 uint64_t *input_tokens,
                                 char *error, size_t error_size,
                                 int *cancel_code,
                                 unsigned int *retry_count);

int snj_provider_responses_compact(const json_t *compact_request,
                                   const struct snj_config *config,
                                   const struct snj_credential *credential,
                                   struct snj_render *render,
                                   snj_provider_pump_fn pump,
                                   void *pump_opaque,
                                   json_t **output,
                                   uint64_t *output_tokens_bound,
                                   char *error, size_t error_size,
                                   int *cancel_code,
                                   unsigned int *retry_count);

int snj_provider_responses_create(const json_t *create_request,
                                  const struct snj_config *config,
                                  const struct snj_credential *credential,
                                  struct snj_render *render,
                                  snj_responses_emit_fn emit,
                                  void *emit_opaque,
                                  snj_provider_pump_fn pump,
                                  void *pump_opaque,
                                  struct snj_response_graph *graph,
                                  char *error, size_t error_size,
                                  int *cancel_code,
                                  unsigned int *retry_count);

#endif
