/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_TOOLS_H
#define SNAJPAGENT_TOOLS_H

#include "config.h"
#include "credential.h"
#include "turn.h"

#include "snj_jansson.h"
#include <stdbool.h>
#include <stddef.h>

/* Return 0 for a completed tool result, 2 for user cancellation with *result set,
 * and -1 only when the host adapter itself failed before a factual result could
 * be constructed. */
typedef int (*snj_tool_pump_fn)(void *opaque, unsigned int timeout_ms);

/* The engine owns the journal. Chunks are borrowed, already secret-redacted.
 * read returns a bounded head/tail excerpt for the exact byte interval. */
typedef int (*snj_tool_output_fn)(void *, const char *, unsigned int,
                                 uint64_t, const void *, size_t);
typedef int (*snj_tool_read_fn)(void *, const char *, unsigned int,
                               uint64_t, uint64_t, struct snj_buf *);
void snj_tools_journal(snj_tool_output_fn write, snj_tool_read_fn read, void *opaque);
/* Validates without effects. 1 means rejected with a factual not-run result. */
int snj_tools_prepare(const struct snj_response_item *, const struct snj_config *,
                       char handle[SNJ_ID_HEX_LEN + 1u], uint32_t *yield_ms,
                       json_t **rejected);
int snj_tools_start(const struct snj_response_item *, const struct snj_config *,
                     const struct snj_credential *, json_t **result,
                     char *error, size_t error_size);
int snj_tools_service(int timeout_ms, int wake_fd, char *error, size_t error_size);
bool snj_tools_ready(const char *handle);
bool snj_tools_busy(void);
const char *snj_tools_handoff(const char *handle);
int snj_tools_collect(const char *handle, const char *reason, json_t **result,
                       char *error, size_t error_size);
void snj_tools_collected(const char *handle);
void snj_tools_process_state(struct snj_process_state *state);
void snj_tools_close_all(bool user_interrupt);
void snj_tools_shutdown(void);

int snj_tools_read_only(const struct snj_response_item *call,
                        const char *workspace, snj_tool_pump_fn pump,
                        void *opaque, json_t **result);

int snj_tools_run(const struct snj_response_item *call,
                  const struct snj_config *config,
                  const struct snj_credential *credential,
                  const char *session_workspace,
                  snj_tool_pump_fn pump, void *pump_opaque, int wake_fd,
                  json_t **result,
                  char *error, size_t error_size);

int snj_tools_attach_output_limit(const struct snj_response_item *call,
                                  const struct snj_config *config,
                                  json_t *result);

int snj_tools_close_managed(const char *handle, bool user_interrupt,
                            snj_tool_pump_fn pump, void *pump_opaque, int wake_fd,
                            json_t **result,
                            char *error, size_t error_size);

#endif
