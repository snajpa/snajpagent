/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_HTTP_H
#define SNAJPAGENT_HTTP_H

#include <curl/curl.h>

/* Process-lifetime initialization shared by independent HTTP owners. */
CURLcode snag_http_init(void);
CURLcode snag_http_trust(CURL *curl);
/* A provider's persistent bundle applies when SSL_CERT_FILE is unset. */
CURLcode snag_http_provider_trust(CURL *curl, const char *bundle);

#endif
