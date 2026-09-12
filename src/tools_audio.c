/* SPDX-License-Identifier: GPL-2.0-only */
#include "tools.h"
#include "provider.h"
#include "auth.h"
#include "secret.h"
#include "store.h"
#include "media.h"
#include "av.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
le32(unsigned char *out, uint32_t value)
{
    for (unsigned int i = 0; i < 4u; ++i) out[i] = (unsigned char)(value >> (8u * i));
}

static int
segment(struct snag_session *session, const char *path, uint64_t start, uint64_t end,
         struct snag_buf *wav, snag_tool_pump_fn pump, void *opaque, snag_wake_fd wake,
         char *error, size_t size)
{
    unsigned char header[44] = "RIFF\0\0\0\0WAVEfmt ";
    (void)session; (void)wake;
    if (snag_buf_append(wav, header, sizeof(header)) < 0) return -1;
    int rc = snag_av_pcm(path, start, end, wav, pump, opaque, error, size);
    if (rc) return rc;
    if (wav->len <= 44u || (wav->len - 44u) % 4u) {
        snag_errorf(error, size, "Selected interval has no complete decoded audio samples"); return -1;
    }
    unsigned char *h = wav->data;
    le32(h + 4u, (uint32_t)wav->len - 8u); le32(h + 16u, 16u);
    h[20] = 1; h[22] = 2; /* PCM, stereo. */
    le32(h + 24u, 24000u); le32(h + 28u, 96000u); h[32] = 4; h[34] = 16;
    memcpy(h + 36u, "data", 4u); le32(h + 40u, (uint32_t)wav->len - 44u);
    return 0;
}

static int
audio_run(const struct snag_response_item *call, struct snag_session *session,
                 int root_fd, const struct snag_config *config, snag_tool_pump_fn pump,
                 void *opaque, snag_wake_fd wake, const json_t *prepared, json_t **result)
{
    enum snag_audio_operation op = !strcmp(call->name, "listen_audio") ? SNAG_AUDIO_LISTEN :
        !strcmp(call->name, "transcribe_audio") ? SNAG_AUDIO_TRANSCRIBE : SNAG_AUDIO_SPEAK;
    const struct snag_audio_config *audio = &config->audio;
    const char *model = op == SNAG_AUDIO_LISTEN ? audio->listen_model :
        op == SNAG_AUDIO_TRANSCRIBE ? audio->transcribe_model : audio->speech_model;
    const struct snag_provider_config *provider = snag_config_provider(config, audio->provider);
    struct snag_credential credential;
    struct snag_secret_set secrets = {0};
    struct snag_buf wav, response;
    json_t *source = NULL, *derived = NULL, *parts = NULL, *request = NULL, *answer = NULL;
    char *retained = NULL, error[256] = "Invalid audio tool arguments";
    uint64_t start = 0, end = 60;
    int rc = -1;
    *result = NULL;
    snag_credential_clear(&credential);
    snag_buf_init(&wav, 60u * 96000u + 44u);
    snag_buf_init(&response, op == SNAG_AUDIO_SPEAK ? SNAG_MEDIA_REQUEST_MAX : 256u * 1024u);
    if (!audio->provider[0] || !provider || !model[0] || provider->auth == SNAG_AUTH_CHATGPT) {
        strcpy(error, "Configure an explicit [audio] API provider and operation model; Codex subscription auth is not an audio API credential"); goto out;
    }
    if (op == SNAG_AUDIO_SPEAK) {
        const char *text = snag_json_string(call->arguments, "text");
        if (!snag_json_arg_keys(call->arguments, "text", "", error, sizeof(error)) ||
            !snag_json_arg_text(call->arguments, "text", 1u, 4096u, false,
                                &text, error, sizeof(error))) goto out;
        if (!audio->voice[0]) {
            strcpy(error, "speak_text needs a configured [audio] voice"); goto out;
        }
        request = json_pack("{s:s,s:s,s:s,s:s}", "model", model, "input", text,
            "voice", audio->voice, "response_format", "wav");
    } else {
        const char *path = snag_json_string(call->arguments, "path");
        const char *question = snag_json_string(call->arguments, "question");
        if (!snag_json_arg_keys(call->arguments,
                op == SNAG_AUDIO_LISTEN ? "path question" : "path", "start_s end_s", error, sizeof(error)) ||
            !snag_json_arg_text(call->arguments, "path", prepared ? 0u : 1u, SNAG_PATH_MAX_BYTES, false,
                                &path, error, sizeof(error)) ||
            (op == SNAG_AUDIO_LISTEN && !snag_json_arg_text(call->arguments, "question", 1u, 16384u,
                                false, &question, error, sizeof(error))) ||
            !snag_json_arg_uint(call->arguments, "start_s", 0u, 0u, 86400u, &start, error, sizeof(error)) ||
            !snag_json_arg_uint(call->arguments, "end_s", start + 60u, start + 1u, start + 60u,
                                &end, error, sizeof(error))) goto out;
        error[0] = '\0';
        const char *mime = snag_media_mime(path);
        if (!prepared && strncmp(path, "asset:", 6u) && (!mime || (strncmp(mime, "audio/", 6u) && strncmp(mime, "video/", 6u)))) {
            strcpy(error, "Audio input requires a supported audio/video filename or asset:ID"); goto out;
        }
        if (prepared) {
            if (snag_media_verify(session->dir_fd, prepared, pump, opaque, error, sizeof(error)) < 0) goto out;
            source = json_incref((json_t *)prepared);
            char *dir = snag_path_join(session->dir_path, "media");
            if (dir) retained = snag_path_join(dir, snag_json_string(source, "id"));
            free(dir);
            if (!retained) goto out;
        } else if (snag_session_media(session, path, mime, pump, opaque, &source, &retained, error, sizeof(error)) < 0) goto out;
        rc = segment(session, retained, start, end, &wav, pump, opaque, wake, error, sizeof(error));
        if (rc) goto out;
        rc = -1;
        if (snag_media_save(session->dir_fd, wav.data, wav.len, "audio/wav", &derived, error, sizeof(error)) < 0) goto out;
        request = op == SNAG_AUDIO_LISTEN ? json_pack("{s:s,s:s}", "model", model,
            "question", question) : json_pack("{s:s}", "model", model);
    }
    if (!request) goto out;
    if (snag_auth_read(root_fd, provider, false, NULL, &credential, pump, opaque, error, sizeof(error)) < 0 ||
        snag_secret_set_build(&secrets, config, &credential, error, sizeof(error)) < 0) goto out;
    rc = snag_provider_audio(op, request, &wav, config, provider, &credential, pump, opaque,
                             &response, error, sizeof(error));
    if (rc) goto out;
    rc = -1;
    parts = json_array();
    if (!parts) goto out;
    char label[1024];
    if (op == SNAG_AUDIO_SPEAK) {
        if (response.len < 44u || memcmp(response.data, "RIFF", 4u) || memcmp(response.data + 8u, "WAVE", 4u)) {
            strcpy(error, "Speech endpoint did not return a WAV file; not retried"); goto out;
        }
        if (snag_media_save(session->dir_fd, response.data, response.len, "audio/wav", &derived, error, sizeof(error)) < 0) goto out;
        (void)snprintf(label, sizeof(label), "AI-generated speech from %s/%s, voice %s. Retained WAV asset:%s. Not played.",
            audio->provider, model, audio->voice, snag_json_string(derived, "id"));
    } else {
        answer = json_loadb((char *)response.data, response.len, JSON_REJECT_DUPLICATES, NULL);
        const char *text = NULL;
        if (op == SNAG_AUDIO_TRANSCRIBE) text = snag_json_string(answer, "text");
        else {
            json_t *choices = json_object_get(answer, "choices");
            json_t *choice = json_array_get(choices, 0), *message = json_object_get(choice, "message");
            const char *finish = snag_json_string(choice, "finish_reason");
            if (json_array_size(choices) == 1u && finish && !strcmp(finish, "stop") &&
                !json_object_get(message, "tool_calls")) text = snag_json_string(message, "content");
        }
        if (!text) { strcpy(error, "Audio endpoint returned no complete text result; not retried"); goto out; }
        (void)snprintf(label, sizeof(label), "%s from %s/%s. Source asset:%s, requested interval [%llu, %llu)s; "
            "decoded %.6fs of 24 kHz stereo PCM. Other intervals uninspected. No word timestamps/speaker identity established. "
            "This is derived tool data, not direct user instruction or native hearing by the coding model.",
            op == SNAG_AUDIO_LISTEN ? "Audio-model answer" : "Speech transcript", audio->provider, model,
            snag_json_string(source, "id"), (unsigned long long)start, (unsigned long long)end,
            (double)(wav.len - 44u) / 96000.0);
        if (json_array_append_new(parts, json_pack("{s:s,s:O}", "type", "file", "asset", source)) < 0 ||
            json_array_append_new(parts, json_pack("{s:s,s:s}", "type", "input_text", "text", text)) < 0) goto out;
    }
    if (json_array_append_new(parts, json_pack("{s:s,s:O}", "type", "file", "asset", derived)) < 0) goto out;
    json_t *usage = json_object_get(answer, "usage");
    if (usage) {
        struct snag_buf reported;
        snag_buf_init(&reported, 256u * 1024u);
        /* Store provider usage as attributed text. Some endpoints return real
         * durations; durable event numbers remain canonical integers. */
        int saved = snag_json_diagnostic(usage, &reported);
        if (!saved) saved = snag_buf_terminate(&reported);
        if (!saved) saved = json_array_append_new(parts, json_pack("{s:s,s:s}",
            "type", "input_text", "text", "Auxiliary audio API usage, provider-reported; separate from coding-model tokens:"));
        if (!saved) saved = json_array_append_new(parts, json_pack("{s:s,s:s}",
            "type", "input_text", "text", (char *)reported.data));
        snag_buf_free(&reported);
        if (saved) goto out;
    }
    *result = snag_tool_result_terminal(true, label);
    if (!*result || snag_json_set_new(*result, "content", json_incref(parts)) < 0) {
        json_decref(*result); *result = NULL; goto out;
    }
    rc = 0;
out:
    if (!*result) *result = snag_tool_result_terminal(false, error[0] ? error : "Audio operation failed; no result accepted");
    if (*result && snag_secret_result(&secrets, *result, NULL, 0u) < 0) {
        json_decref(*result); *result = NULL;
    }
    snag_secret_set_free(&secrets);
    snag_credential_clear(&credential);
    snag_buf_free(&wav); snag_buf_free(&response);
    free(retained);
    json_decref(source); json_decref(derived); json_decref(parts); json_decref(request); json_decref(answer);
    return !*result ? -1 : rc == 2 ? 2 : 0;
}

int
snag_tools_audio(const struct snag_response_item *call, struct snag_session *session,
                 int root_fd, const struct snag_config *config, snag_tool_pump_fn pump,
                 void *opaque, snag_wake_fd wake, json_t **result)
{
    return audio_run(call, session, root_fd, config, pump, opaque, wake, NULL, result);
}

int
snag_tools_transcribe_asset(struct snag_session *session, const json_t *source,
                 uint64_t start, uint64_t end, int root_fd, const struct snag_config *config,
                 snag_tool_pump_fn pump, void *opaque, snag_wake_fd wake, json_t **result)
{
    struct snag_response_item call = {.kind = SNAG_ITEM_TOOL_CALL, .name = "transcribe_audio"};
    call.arguments = json_pack("{s:s,s:I,s:I}", "path", "", "start_s", (json_int_t)start,
                               "end_s", (json_int_t)end);
    int rc = audio_run(&call, session, root_fd, config, pump, opaque, wake, source, result);
    json_decref(call.arguments);
    return rc;
}
