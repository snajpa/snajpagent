/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_MEDIA_H
#define SNAJPAGENT_MEDIA_H

#include "json.h"

#define SNAG_MEDIA_FILE_MAX (256u * 1024u * 1024u)
#define SNAG_MEDIA_REQUEST_MAX (12u * 1024u * 1024u)

/* One disposable Office profile/output directory per session. The live worker
 * holds a directory lock; bounds apply only to this conversion scratch tree. */
#define SNAG_MEDIA_WORK_NAME "office-work"
#define SNAG_MEDIA_WORK_MAX (256u * 1024u * 1024u)
int snag_media_work_open(int session_fd, char *, size_t);
int snag_media_work_check(int work_fd);
/* 0 removed/absent, 1 still owned by a worker, -1 unsafe or failed cleanup. */
int snag_media_work_remove(int session_fd, char *, size_t);

/* Assets are immutable private files in the session's media directory. The
 * journal owns acceptance; a returned reference is not itself an accepted event.
 * Failed operations remove only their own newly created file. */
/* NULL mime detects the supported native still-image formats from bytes. */
int snag_media_snapshot(int session_fd, const char *workspace, const char *path,
                        const char *mime, size_t max_bytes,
                        int (*pump)(void *, unsigned int), void *opaque,
                        json_t **asset, char *error, size_t error_size);
const char *snag_media_mime(const char *path);
int snag_media_save(int session_fd, const void *bytes, size_t len, const char *mime,
                    json_t **asset, char *error, size_t error_size);
int snag_media_verify(int session_fd, const json_t *asset,
                      int (*pump)(void *, unsigned int), void *opaque, char *error, size_t error_size);
bool snag_media_valid(const json_t *asset);
/* Read and verify before exposing any bytes. On failure the buffer is unchanged. */
int snag_media_read(int session_fd, const json_t *asset, struct snag_buf *bytes,
                    char *error, size_t error_size);

/* Durable content holds asset references; resolved content holds provider parts. */
bool snag_media_content_valid(const json_t *content);
json_t *snag_media_content_resolve(int session_fd, const json_t *content,
                                  char *error, size_t error_size);
json_t *snag_media_message_content(int session_fd, const char *text, const json_t *content,
                                  char *error, size_t error_size);
/* Provider-generic high-detail image input budget plus bounded textual request
 * bytes; no per-model or per-route constant is required. A nonzero
 * configured_image_tokens is a provider-documented per-image ceiling for the
 * selected local model and supersedes the generic budget. No base64 text
 * tokenization or mutation of the request. */
int snag_media_token_bound(const json_t *, uint64_t configured_image_tokens,
                           uint64_t *, char *, size_t);
/* Accepts a request or its input array. */
bool snag_media_request_has_images(const json_t *request);

/* Compaction summarizes prior model-visible history. Omit already-consumed
 * image bytes from that auxiliary request while retaining the surrounding
 * text/tool transcript and the immutable journal assets. */
int snag_media_compaction_prepare(json_t *request, uint64_t *omitted,
                                  char *error, size_t error_size);
int snag_media_request_check(const json_t *request, char *error, size_t error_size);
int snag_media_remove(int session_fd, char *error, size_t error_size);

#endif
