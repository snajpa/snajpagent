/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"
#include "audio_device.h"
#include "av.h"
#include "provider.h"
#include "secret.h"
#include "secret_source.h"
#include "json.h"
#include "media.h"
#include <stdlib.h>
#include <string.h>

/* Engine-owned local audio. The device callback only moves bounded samples. */
struct app_audio {
    struct snag_audio_device *device;
    struct snag_av_audio *decoder;
    struct snag_buf wav;
    uint64_t started, drained;
    bool transcribing, cancelled, playing, eof;
    int16_t pending[16384];
    uint32_t frames, offset;
};

void
snag_app_audio_close(struct app_state *app)
{
    snag_app_voice_close(app);
    struct app_audio *audio = app->audio;
    if (!audio) return;
    snag_audio_close(audio->device);
    snag_av_audio_close(audio->decoder);
    if (audio->wav.data) snag_secret_clear(audio->wav.data, audio->wav.len);
    snag_buf_free(&audio->wav);
    memset(audio, 0, sizeof(*audio));
    free(audio); app->audio = NULL;
}

static int
finish(struct app_state *app, const char *message, bool error)
{
    snag_app_audio_close(app);
    int rc = snag_ui_audio(&app->ui, "", false);
    if (!rc && message) rc = snag_ui_text(&app->ui, error ? SNAG_UI_ERROR : SNAG_UI_HOST, message);
    return rc;
}

/* Auxiliary requests do not recurse into the turn executor or change config.
 * Presentation keeps editing. An accidental queued submission remains a draft. */
static int
checkpoint(void *opaque, unsigned int timeout)
{
    struct app_state *app = opaque;
    enum snag_term_action action = SNAG_TERM_NONE;
    char *text = NULL;
    if (snag_app_shutdown(app) || app->input_closed || !app->audio || app->audio->cancelled) return 2;
    int rc = snag_ui_poll(&app->ui, (int)(timeout > 25u ? 25u : timeout), &action, &text);
    if (rc < 0) return -1;
    if (action == SNAG_TERM_EXIT) app->input_closed = true;
    bool cancel = app->input_closed || action == SNAG_TERM_CANCEL || action == SNAG_TERM_INTERRUPT ||
        action == SNAG_TERM_DICTATE_CANCEL || (action == SNAG_TERM_DICTATE_DONE &&
        !app->audio->device && !app->audio->transcribing) || (text && !strcmp(text, "/play stop"));
    if (text && !cancel) {
        rc = snag_ui_insert_draft(&app->ui, text);
        if (rc > 0) rc = -1;
    }
    free(text);
    if (cancel) { app->audio->cancelled = true; return 2; }
    return rc < 0 ? -1 : 0;
}

static int
decode_checkpoint(void *opaque, unsigned int timeout)
{
    struct app_state *app = opaque;
    (void)timeout;
    return snag_app_shutdown(app) || app->input_closed || !app->audio || app->audio->cancelled ? 2 : 0;
}

static void
le32(unsigned char *out, uint32_t n)
{
    for (unsigned int i = 0; i < 4u; ++i) out[i] = (unsigned char)(n >> (i * 8u));
}

static int
transcribe(struct app_state *app)
{
    struct app_audio *audio = app->audio;
    const struct snag_audio_config *cfg = &app->config->audio;
    const struct snag_provider_config *provider = snag_config_provider(app->config, cfg->provider);
    struct snag_credential credential;
    struct snag_secret_set secrets = {0};
    struct snag_buf response, usage_text;
    char error[256] = "Dictation transcription failed; not retried.";
    json_t *request = NULL, *answer = NULL, *safe = NULL, *usage_note = NULL;
    int rc = -1;
    snag_credential_clear(&credential);
    snag_buf_init(&response, 256u * 1024u);
    snag_buf_init(&usage_text, 256u * 1024u);
    /* Close capture before loading credentials or making a paid request. */
    snag_audio_close(audio->device); audio->device = NULL;
    audio->transcribing = true;
    if (audio->wav.len <= 44u) { strcpy(error, "No microphone samples captured."); goto out; }
    if (snag_ui_audio(&app->ui, "[mic off; transcribing; Esc cancels] ", true) != 0) goto out;
    unsigned char *h = audio->wav.data;
    le32(h + 4u, (uint32_t)audio->wav.len - 8u); le32(h + 16u, 16u);
    h[20] = 1; h[22] = 1; le32(h + 24u, 24000u); le32(h + 28u, 48000u);
    h[32] = 2; h[34] = 16; memcpy(h + 36u, "data", 4u); le32(h + 40u, (uint32_t)audio->wav.len - 44u);
    request = json_pack("{s:s}", "model", cfg->transcribe_model);
    if (!request || !provider || checkpoint(app, 0u) ||
        snag_auth_read(app->store.root_fd, provider, false, NULL, &credential, checkpoint, app, error, sizeof(error)) < 0 ||
        snag_secret_set_build(&secrets, app->config, &credential, error, sizeof(error)) < 0) goto out;
    rc = snag_provider_audio(SNAG_AUDIO_TRANSCRIBE, request, &audio->wav, app->config, provider,
        &credential, checkpoint, app, &response, error, sizeof(error));
    if (rc) goto out;
    rc = -1;
    answer = json_loadb((const char *)response.data, response.len, JSON_REJECT_DUPLICATES, NULL);
    if (!answer) goto out;
    /* Usage remains separately attributed, never added to coding token counters. */
    json_t *usage = json_object_get(answer, "usage");
    if (usage) {
        struct snag_buf raw;snag_buf_init(&raw,256u*1024u);
        int reported=snag_json_diagnostic(usage,&raw);
        if(!reported)reported=snag_buf_printf(&usage_text, "Dictation API usage (%s/%s), provider-reported; separate from coding tokens: ",
                cfg->provider, cfg->transcribe_model);
        if(!reported)reported=snag_buf_append(&usage_text,raw.data,raw.len);
        snag_buf_free(&raw);
        if(reported || snag_buf_terminate(&usage_text)<0)goto out;
        usage_note = json_pack("{s:s}", "model_text", (char *)usage_text.data);
        if (!usage_note || snag_secret_result(&secrets, usage_note, error, sizeof(error)) < 0)goto out;
        const char *report=snag_json_string(usage_note,"model_text");
        json_t *event=json_pack("{s:s,s:s,s:s,s:s}","operation","dictation","provider",cfg->provider,
            "model",cfg->transcribe_model,"report",report);
        if(!event || snag_session_commit(&app->session,"audio_usage",event,NULL,error,sizeof(error))<0)goto out;
        if(snag_ui_text(&app->ui,SNAG_UI_HOST,report)<0)goto out;
    }
    const char *text = snag_json_string(answer, "text");
    if (!text || !snag_utf8_valid((const unsigned char *)text, strlen(text), true)) {
        strcpy(error, "Transcription endpoint returned no complete UTF-8 text; not retried."); goto out;
    }
    safe = json_pack("{s:s}", "model_text", text);
    if (!safe || snag_secret_result(&secrets, safe, error, sizeof(error)) < 0) goto out;
    text = snag_json_string(safe, "model_text");
    if (checkpoint(app, 0u)) { rc = 2; goto out; }
    rc = snag_ui_insert_draft(&app->ui, text);
    if (rc) { rc = -1; strcpy(error, "Transcript could not be inserted. Review the draft; request was not retried."); }
out:
    if (audio->cancelled || app->input_closed || app->shutdown_signal) rc = 2;
    snag_credential_clear(&credential); snag_secret_set_free(&secrets);
    json_decref(request); json_decref(answer); json_decref(safe); json_decref(usage_note);
    snag_secret_clear(response.data, response.len);
    snag_buf_free(&response); snag_buf_free(&usage_text);
    return finish(app, rc == 0 ? "Dictation inserted at the cursor; review before submitting." :
        rc == 2 ? "Dictation cancelled; captured sound discarded. A sent request may have been billed." : error, rc < 0);
}

int
snag_app_audio_service(struct app_state *app)
{
    if(snag_app_voice_service(app)<0)return -1;
    struct app_audio *audio = app->audio;
    if (!audio || audio->transcribing) return 0;
    if (!audio->device) return 0; /* Preparing an accepted playback asset. */
    if (snag_audio_fault(audio->device))
        return finish(app, "Audio device stopped, rerouted or overflowed; local audio discarded.", true);
    if (audio->playing) {
        char error[256] = "Audio playback decoding failed.";
        /* Fill at most one second, with bounded work per engine tick. */
        for (unsigned int chunk = 0; chunk < 32u; ++chunk) {
            if (audio->offset < audio->frames) {
                audio->offset += snag_audio_play(audio->device, audio->pending + 2u * audio->offset,
                                                 audio->frames - audio->offset);
                if (audio->offset < audio->frames) break;
            }
            if (!audio->eof && snag_audio_pending(audio->device) < 24000u) {
                int rc = snag_av_audio_read(audio->decoder, audio->pending, &audio->frames, error, sizeof(error));
                audio->offset = 0;
                if (rc) return finish(app, error, true);
                if (!audio->frames) audio->eof = true;
            } else break;
        }
        if (audio->eof && !snag_audio_pending(audio->device)) {
            if (!audio->drained) audio->drained = snag_monotonic_ms();
            if (snag_monotonic_ms() - audio->drained >= snag_audio_latency_ms(audio->device))
                return finish(app, "Playback finished (selected interval: first 60 seconds).", false);
        }
        return 0;
    }
    int16_t pcm[4096];
    uint32_t n;
    while ((n = snag_audio_capture(audio->device, pcm, 4096u)) != 0u) {
        size_t remaining = (60u * 48000u + 44u - audio->wav.len) / 2u;
        if (n > remaining) n = (uint32_t)remaining;
        unsigned char *bytes = (unsigned char *)pcm;
        for (uint32_t i = 0; i < n; ++i) {
            uint16_t value = (uint16_t)pcm[i];
            bytes[2u * i] = (unsigned char)value; bytes[2u * i + 1u] = (unsigned char)(value >> 8);
        }
        if (snag_buf_append(&audio->wav, bytes, (size_t)n * 2u) < 0)
            return finish(app, "Dictation buffer limit reached; capture discarded.", true);
        if (audio->wav.len == 60u * 48000u + 44u) break;
    }
    if (audio->wav.len == 60u * 48000u + 44u || snag_monotonic_ms() - audio->started >= 60000u)
        return transcribe(app);
    return 0;
}

int
snag_app_audio_action(struct app_state *app, enum snag_term_action action)
{
    if (!app->audio || app->audio->playing) return 0;
    if (action == SNAG_TERM_DICTATE_CANCEL) return finish(app, "Dictation cancelled; captured sound discarded.", false);
    if (snag_app_audio_service(app) < 0) return -1;
    return app->audio ? transcribe(app) : 0;
}

int
snag_app_audio_command(struct app_state *app, const char *line, bool *handled)
{
    bool dictate = !strcmp(line, "/dictate");
    bool devices = !strcmp(line, "/voice devices");
    bool play = !strcmp(line, "/play") || !strncmp(line, "/play ", 6u);
    *handled = dictate || devices || play;
    if (!*handled) return 0;
    if (play && !strcmp(line, "/play stop")) {
        if (app->audio && app->audio->playing) return finish(app, "Playback stopped.", false);
        return snag_ui_text(&app->ui, SNAG_UI_HOST, "No file playback is active.");
    }
    if (app->audio || app->voice || app->attaching)
        return snag_ui_text(&app->ui, SNAG_UI_ERROR, "Local audio or attachment preparation is busy; stop it first.");
    char error[256] = "Could not prepare local audio.";
    if (devices) {
        struct snag_buf names;
        snag_buf_init(&names, 128u * 1024u);
        int rc = snag_audio_devices(&names, error, sizeof(error));
        if (!rc) rc = snag_buf_terminate(&names);
        if (!rc) rc = snag_ui_text(&app->ui, SNAG_UI_HOST, (char *)names.data);
        else rc = snag_ui_text(&app->ui, SNAG_UI_ERROR, error);
        snag_buf_free(&names); return rc;
    }
    const struct snag_audio_config *cfg = &app->config->audio;
    const struct snag_provider_config *provider = snag_config_provider(app->config, cfg->provider);
    if (dictate && (!provider || provider->auth != SNAG_AUTH_API_KEY || !cfg->transcribe_model[0]))
        return snag_ui_text(&app->ui, SNAG_UI_ERROR, "Configure [audio] provider (auth=api_key) and transcribe_model before /dictate.");
    if (play && (strncmp(line, "/play asset:", 12u) || !snag_hex_is_lower(line + 12u, SNAG_ID_HEX_LEN)))
        return snag_ui_text(&app->ui, SNAG_UI_ERROR, "Use /play asset:ID for an accepted session audio asset, or /play stop.");
    struct app_audio *audio = calloc(1u, sizeof(*audio));
    if (!audio) return -1;
    app->audio = audio; audio->playing = play;
    snag_buf_init(&audio->wav, 60u * 48000u + 44u);
    if (play) {
        json_t *asset = NULL; char *path = NULL;
        int rc = snag_session_media(&app->session, line + 6u, NULL, checkpoint, app, &asset, &path, error, sizeof(error));
        const char *mime = snag_json_string(asset, "mime_type");
        if (!rc && (!mime || strncmp(mime, "audio/", 6u))) {
            strcpy(error, "Playback requires an accepted audio asset."); rc = -1;
        }
        if (!rc) rc = snag_av_audio_open(path, 0u, 60u, decode_checkpoint, app, &audio->decoder, error, sizeof(error));
        json_decref(asset); free(path);
        if (rc) return finish(app, error, true);
        if (snag_ui_audio(&app->ui, "[playing; /play stop] ", false) < 0) return finish(app, NULL, true);
    } else {
        unsigned char header[44] = "RIFF\0\0\0\0WAVEfmt ";
        if (snag_buf_reserve(&audio->wav, 60u * 48000u + 44u) < 0 ||
            snag_buf_append(&audio->wav, header, sizeof(header)) < 0) return finish(app, error, true);
        /* This acknowledged UI message is applied before starting the device. */
        if (snag_ui_audio(&app->ui, "[MIC ON; Enter finishes; Esc cancels] ", true) != 0)
            return finish(app, "Dictation requires a raw interactive terminal with a visible prompt.", true);
        if(snag_session_persist(&app->store,&app->session,error,sizeof(error))<0)return finish(app,error,true);
        if (checkpoint(app, 0u)) return finish(app, "Dictation cancelled before capture.", false);
    }
    if (snag_audio_open(dictate, play, play ? 2u : 1u, cfg->capture_device, cfg->playback_device,
            &audio->device, error, sizeof(error)) < 0) return finish(app, error, true);
    audio->started = snag_monotonic_ms();
    return 0;
}
