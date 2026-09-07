/* SPDX-License-Identifier: GPL-2.0-only */
#include "http.h"

#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>

static pthread_once_t once = PTHREAD_ONCE_INIT;
static CURLcode initialized;

static void
initialize(void)
{
    initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
}

CURLcode
snag_http_init(void)
{
    return pthread_once(&once, initialize) == 0 ? initialized : CURLE_FAILED_INIT;
}

#ifdef SNAJPAGENT_CA_BUNDLE
#include <zstd.h>
#endif

CURLcode
snag_http_trust(CURL *curl)
{
    const char *file = getenv("SSL_CERT_FILE");
    CURLcode rc;

    if (file && *file) {
        rc = curl_easy_setopt(curl, CURLOPT_CAINFO, file);
        return rc != CURLE_OK ? rc :
            curl_easy_setopt(curl, CURLOPT_PROXY_CAINFO, file);
    }
#ifdef SNAJPAGENT_CA_BUNDLE
    static const unsigned char compressed[] = {
#include SNAJPAGENT_CA_BUNDLE
    };
    unsigned long long size = ZSTD_getFrameContentSize(compressed, sizeof(compressed));
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size >= SIZE_MAX)
        return CURLE_SSL_CACERT_BADFILE;
    char *certificates = malloc((size_t)size + 1u);
    if (!certificates)
        return CURLE_OUT_OF_MEMORY;
    size_t decoded = ZSTD_decompress(certificates, (size_t)size, compressed, sizeof(compressed));
    if (ZSTD_isError(decoded) || decoded != size) {
        free(certificates);
        return CURLE_SSL_CACERT_BADFILE;
    }
    certificates[decoded] = '\0';
    struct curl_blob bundle = {
        certificates, decoded + 1u, CURL_BLOB_COPY
    };
    rc = curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &bundle);
    if (rc == CURLE_OK)
        rc = curl_easy_setopt(curl, CURLOPT_PROXY_CAINFO_BLOB, &bundle);
    free(certificates);
    return rc;
#else
    return CURLE_OK;
#endif
}
