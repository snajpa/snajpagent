/* SPDX-License-Identifier: GPL-2.0-only */
#include "provider_retry.h"

#include <assert.h>
#include <stdio.h>

static void
test_http_retry_classes(void)
{
    assert(!snag_provider_http_status_retryable(200));
    assert(!snag_provider_http_status_retryable(400));
    assert(!snag_provider_http_status_retryable(401));
    assert(snag_provider_http_status_retryable(408));
    assert(snag_provider_http_status_retryable(429));
    assert(snag_provider_http_status_retryable(500));
    assert(!snag_provider_http_status_retryable(501));
    assert(snag_provider_http_status_retryable(599));
    assert(!snag_provider_http_status_retryable(600));
}

static void
test_retry_after_parser(void)
{
    uint32_t delay = 123u;

    assert(snag_provider_retry_after_parse((const unsigned char *)"0", 1u,
                                          &delay) == 0);
    assert(delay == 0u);
    assert(snag_provider_retry_after_parse((const unsigned char *)" 2\t", 3u,
                                          &delay) == 0);
    assert(delay == 2000u);
    assert(snag_provider_retry_after_parse((const unsigned char *)"30", 2u,
                                          &delay) == 0);
    assert(delay == SNAG_PROVIDER_RETRY_AFTER_MAX_MS);
    assert(snag_provider_retry_after_parse((const unsigned char *)"31", 2u,
                                          &delay) < 0);
    assert(snag_provider_retry_after_parse((const unsigned char *)"1.5", 3u,
                                          &delay) < 0);
    assert(snag_provider_retry_after_parse((const unsigned char *)"Wed, 21 Oct 2015 07:28:00 GMT",
                                          29u, &delay) < 0);
}

static void
test_retry_delays(void)
{
    assert(snag_provider_retry_delay_ms(0u, false, 0u) == 250u);
    assert(snag_provider_retry_delay_ms(1u, false, 0u) == 500u);
    assert(snag_provider_retry_delay_ms(8u, false, 0u) == 2000u);
    assert(snag_provider_retry_delay_ms(0u, true, 3000u) == 3000u);
    assert(snag_provider_retry_delay_ms(0u, true,
        SNAG_PROVIDER_RETRY_AFTER_MAX_MS + 1u) == 250u);
}

int
main(void)
{
    test_http_retry_classes();
    test_retry_after_parser();
    test_retry_delays();
    puts("test_provider_retry: ok");
    return 0;
}
