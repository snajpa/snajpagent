/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_PROVIDER_RETRY_H
#define SNAJPAGENT_PROVIDER_RETRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SNAG_PROVIDER_MAX_RETRIES 2u
#define SNAG_PROVIDER_RETRY_AFTER_MAX_MS 30000u

bool snag_provider_http_status_retryable(long status);
/* status == 0 denotes a streamed failure, without an HTTP error status. */
bool snag_provider_failure_retryable(long status, const char *code,
                                     const char *type);
int snag_provider_retry_after_parse(const unsigned char *value, size_t len,
                                   uint32_t *delay_ms);
uint32_t snag_provider_retry_delay_ms(unsigned int retries_done,
                                     bool retry_after_present,
                                     uint32_t retry_after_ms);

#endif
