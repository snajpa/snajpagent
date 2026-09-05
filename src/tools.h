/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_TOOLS_H
#define SNAJPAGENT_TOOLS_H

#include "config.h"
#include "credential.h"
#include "turn.h"

#include "snag_jansson.h"
#include <stdbool.h>
#include <stddef.h>

/* Return 0 for a completed tool result, 2 for user cancellation with *result set,
 * and -1 only when the host adapter itself failed before a factual result could
 * be constructed. */
typedef int (*snag_tool_pump_fn)(void *opaque, unsigned int timeout_ms);

/* The engine owns the journal. Chunks are borrowed, already secret-redacted.
 * read returns a bounded head/tail excerpt for the exact byte interval. */
typedef int (*snag_tool_output_fn)(void *, const char *, unsigned int,
                                 uint64_t, const void *, size_t);
typedef int (*snag_tool_read_fn)(void *, const char *, unsigned int,
                               uint64_t, uint64_t, struct snag_buf *);
void snag_tools_journal(snag_tool_output_fn write, snag_tool_read_fn read, void *opaque);
/* Validates without effects. 1 means rejected with a factual not-run result. */
int snag_tools_prepare(const struct snag_response_item *, const struct snag_config *,
                       char handle[SNAG_ID_HEX_LEN + 1u], uint32_t *yield_ms,
                       json_t **rejected);
int snag_tools_start(const struct snag_response_item *, const struct snag_config *,
                     const struct snag_credential *, json_t **result,
                     char *error, size_t error_size);
int snag_tools_service(int timeout_ms, int wake_fd, char *error, size_t error_size);
bool snag_tools_ready(const char *handle);
bool snag_tools_busy(void);
const char *snag_tools_handoff(const char *handle);
int snag_tools_collect(const char *handle, const char *reason, json_t **result,
                       char *error, size_t error_size);
void snag_tools_collected(const char *handle);
void snag_tools_process_state(struct snag_process_state *state);
void snag_tools_close_all(bool user_interrupt);
void snag_tools_shutdown(void);

int snag_tools_read_only(const struct snag_response_item *call,
                        const char *workspace, snag_tool_pump_fn pump,
                        void *opaque, json_t **result);

int snag_tools_run(const struct snag_response_item *call,
                  const struct snag_config *config,
                  const struct snag_credential *credential,
                  const char *session_workspace,
                  snag_tool_pump_fn pump, void *pump_opaque, int wake_fd,
                  json_t **result,
                  char *error, size_t error_size);

int snag_tools_attach_output_limit(const struct snag_response_item *call,
                                  const struct snag_config *config,
                                  json_t *result);

int snag_tools_close_managed(const char *handle, bool user_interrupt,
                            snag_tool_pump_fn pump, void *pump_opaque, int wake_fd,
                            json_t **result,
                            char *error, size_t error_size);

#endif
