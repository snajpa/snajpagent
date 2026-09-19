/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_PROVIDER_H
#define SNAJPAGENT_PROVIDER_H

#include "config.h"
#include "credential.h"
#include "responses.h"
#include "turn.h"

#include "snag_jansson.h"
#include <stddef.h>

struct snag_ui;

typedef int (*snag_provider_pump_fn)(void *opaque, unsigned int timeout_ms);

/* Pump: -1 failure, 0 continue, 1 steer, 2 cancel, 3 new non-steering input.
 * New input lets a healthy response finish but prevents further retries. */
#define SNAG_PROVIDER_NEW_INPUT 3

/* Optional native operation is absent; no semantic output was returned. */
#define SNAG_PROVIDER_UNSUPPORTED 4
/* Typed context overflow from a count or native compaction operation. */
#define SNAG_PROVIDER_CONTEXT_OVERFLOW 5

/* Borrowed for one synchronous call; refreshed credentials remain private. */
struct snag_provider_connection {
    const struct snag_config *config;
    const struct snag_provider_config *provider;
    const struct snag_credential *credential;
    struct snag_ui *render;
    snag_provider_pump_fn pump;
    void *pump_opaque;
    /* Stable per-conversation identity, sent to the proxy as its cache-affinity key; NULL omits it. */
    const char *session_id;
};

enum snag_audio_operation { SNAG_AUDIO_LISTEN, SNAG_AUDIO_TRANSCRIBE, SNAG_AUDIO_SPEAK };
/* Resolve optional audio preferences against the selected coding provider. */
const struct snag_provider_config *snag_provider_audio_config(const struct snag_config *,
    const char *selected, struct snag_audio_config *);
bool snag_provider_native_audio(const struct snag_provider_config *);
/* One paid request only: no automatic retry, body diagnostics or coding history.
 * Listen takes {model,question} metadata and streams WAV as base64. Both input
 * operations borrow WAV bytes for the duration of the call. Output rolls
 * back on failure. Native subscription transcription uses its own endpoint. */
int snag_provider_audio(enum snag_audio_operation operation, const json_t *request,
                         const struct snag_buf *wav, const struct snag_config *config,
                         const struct snag_provider_config *provider,
                         const struct snag_credential *credential,
                         snag_provider_pump_fn pump, void *opaque, struct snag_buf *output,
                         char *error, size_t error_size);

/* One owner, one realtime WSS connection. No device, journal, retry or coding
 * executor here. Caller owns message buffers and retries only unsent bytes.
 * receive: 1 whole text message, 0 incomplete/would-block, -1 stopped/error.
 * Clear the receive buffer after each whole message; preserve it otherwise. */
struct snag_voice_socket;
int snag_provider_voice_call(const struct snag_config *,const struct snag_provider_config *,
    const struct snag_credential *,const char *sdp,const json_t *session,
    snag_provider_pump_fn,void *,struct snag_buf *answer,char call[257],char *,size_t);
int snag_provider_voice_attach(const struct snag_provider_config *,const struct snag_credential *,
    const char *call,snag_provider_pump_fn,void *,struct snag_voice_socket **,char *,size_t);
int snag_provider_voice_open(const struct snag_provider_config *,const struct snag_credential *,
    const char *model,snag_provider_pump_fn,void *,struct snag_voice_socket **,char *,size_t);
int snag_provider_voice_send(struct snag_voice_socket *,const void *,size_t,size_t *,char *,size_t);
int snag_provider_voice_receive(struct snag_voice_socket *,struct snag_buf *,char *,size_t);
int snag_provider_voice_wait(struct snag_voice_socket *,bool writing,unsigned int timeout);
void snag_provider_voice_close(struct snag_voice_socket *);

/* Fixed-issuer auth transport: bounded, cancellable, and never body-logged. */
int snag_provider_auth_post(const char *issuer, const char *path, const char *type,
                            const void *body, size_t size, json_t **response,
                            long *status, snag_provider_pump_fn pump, void *opaque,
                            char *error, size_t error_size);

/* Responses operations return 1 for steering or 2 for cancellation. */
int snag_provider_responses_count(struct snag_provider_connection connection,
                                 const json_t *request, uint64_t *input_tokens, bool *endpoint_unsupported,
                                 char *error, size_t error_size, unsigned int *retry_count);

int snag_provider_responses_compact(struct snag_provider_connection connection,
                                   const json_t *request, struct snag_json_document *output,
                                   char *error, size_t error_size, unsigned int *retry_count);

int snag_provider_responses_create(struct snag_provider_connection connection,
                                  const json_t *request, snag_responses_emit_fn emit,
                                  void *emit_opaque, snag_responses_hosted_fn hosted,
                                  void *hosted_opaque, struct snag_response_graph *graph,
                                  struct snag_provider_failure *failure, char *error, size_t error_size,
                                  unsigned int *retry_count);

int snag_provider_models_list(struct snag_provider_connection connection,
                             json_t **models, char *error, size_t error_size);

const char *snag_provider_catalog_protocol( const struct snag_provider_config *provider);

#endif
