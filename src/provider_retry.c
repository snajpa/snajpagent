/* SPDX-License-Identifier: GPL-2.0-only */
#include "provider_retry.h"

#include <limits.h>

#define SNJ_PROVIDER_BACKOFF_BASE_MS 250u
#define SNJ_PROVIDER_BACKOFF_MAX_MS 2000u

bool
snj_provider_http_status_retryable(long status)
{
    return status == 408 || status == 429 ||
           (status >= 500 && status <= 599);
}

int
snj_provider_retry_after_parse(const unsigned char *value, size_t len,
                               uint32_t *delay_ms)
{
    uint64_t seconds = 0u;
    size_t i = 0u;
    size_t first;

    if (delay_ms)
        *delay_ms = 0u;
    if (!value || !delay_ms)
        return -1;
    while (i < len && (value[i] == ' ' || value[i] == '\t'))
        ++i;
    first = i;
    while (i < len && value[i] >= '0' && value[i] <= '9') {
        uint64_t digit = (uint64_t)(value[i] - '0');
        if (seconds > (UINT64_MAX - digit) / 10u)
            return -1;
        seconds = seconds * 10u + digit;
        ++i;
    }
    if (i == first)
        return -1;
    while (i < len && (value[i] == ' ' || value[i] == '\t'))
        ++i;
    if (i != len || seconds > SNJ_PROVIDER_RETRY_AFTER_MAX_MS / 1000u)
        return -1;
    *delay_ms = (uint32_t)seconds * 1000u;
    return 0;
}

uint32_t
snj_provider_retry_delay_ms(unsigned int retries_done,
                            bool retry_after_present,
                            uint32_t retry_after_ms)
{
    uint32_t delay = SNJ_PROVIDER_BACKOFF_BASE_MS;

    if (retry_after_present && retry_after_ms <= SNJ_PROVIDER_RETRY_AFTER_MAX_MS)
        return retry_after_ms;
    while (retries_done > 0u && delay < SNJ_PROVIDER_BACKOFF_MAX_MS) {
        if (delay > SNJ_PROVIDER_BACKOFF_MAX_MS / 2u) {
            delay = SNJ_PROVIDER_BACKOFF_MAX_MS;
            break;
        }
        delay *= 2u;
        --retries_done;
    }
    return delay;
}
