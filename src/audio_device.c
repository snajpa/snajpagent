/* SPDX-License-Identifier: GPL-2.0-only */
#include "audio_device.h"
#include "pcm.h"
#include <stdlib.h>
#include <string.h>

#if SNAJPAGENT_AUDIO_DEVICE
#include <speex/speex_echo.h>

/* Budget 20 ms frames and a 250 ms filter tail for device delay and reflections. */
#define SNAG_ECHO_FRAMES 480u
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_NULL
#include <miniaudio.h>

struct snag_audio_device {
    ma_context context;
    ma_device device;
    struct snag_pcm input, output;
    struct snag_pcm_playout playout;
    int16_t *input_samples, *output_samples;
    atomic_uint mute, silenced, fault, delivered;
    unsigned int channels;
    SpeexEchoState *echo;
    uint32_t echo_frames;
    int16_t echo_capture[SNAG_ECHO_FRAMES];
    int16_t echo_render[2u * SNAG_ECHO_FRAMES];
    bool context_ready, device_ready, capture, playback;
};

static int
capture_pcm(struct snag_audio_device *owner, const int16_t *input, const int16_t *output,
    uint32_t frames)
{
    if (!owner->echo) {
        return frames <= 48000u - snag_pcm_available(&owner->input) &&
            snag_pcm_write(&owner->input, input, frames) == frames ? 0 : -1;
    }
    if (!output) return -1;
    while (frames) {
        uint32_t count = SNAG_ECHO_FRAMES - owner->echo_frames;
        if (count > frames) count = frames;
        memcpy(owner->echo_capture + owner->echo_frames, input, count * sizeof(*input));
        memcpy(owner->echo_render + owner->echo_frames * owner->channels, output,
            count * owner->channels * sizeof(*output));
        owner->echo_frames += count;
        input += count;
        output += count * owner->channels;
        frames -= count;
        if (owner->echo_frames == SNAG_ECHO_FRAMES) {
            int16_t filtered[SNAG_ECHO_FRAMES];
            owner->echo_frames = 0u;
            speex_echo_cancellation(owner->echo, owner->echo_capture, owner->echo_render, filtered);
            if (snag_pcm_available(&owner->input) > 48000u - SNAG_ECHO_FRAMES ||
                snag_pcm_write(&owner->input, filtered, SNAG_ECHO_FRAMES) != SNAG_ECHO_FRAMES) {
                return -1;
            }
        }
    }
    return 0;
}

static void
callback(ma_device *device, void *output, const void *input, ma_uint32 frames)
{
    struct snag_audio_device *owner = device->pUserData;
    unsigned int gate = atomic_load_explicit(&owner->mute, memory_order_acquire);
    /* The reference includes exactly what this callback renders, including silence. */
    if (owner->playback && output) {
        uint32_t count = frames * owner->channels;
        memset(output, 0, (size_t)count * sizeof(int16_t));
        if (atomic_load_explicit(&owner->fault, memory_order_acquire)) {
            snag_pcm_discard(&owner->output);
        } else {
            uint32_t n = owner->capture ?
                snag_pcm_playout_read(&owner->playout, &owner->output, output, count) :
                snag_pcm_read(&owner->output, output, count);
            atomic_fetch_add_explicit(&owner->delivered, n / owner->channels, memory_order_relaxed);
        }
    }
    if (!owner->capture) return;
    if (gate & 1u) {
        /* No DSP access follows this acknowledgement; unmute may reset it. */
        atomic_store_explicit(&owner->silenced, gate, memory_order_release);
    } else if (!atomic_load_explicit(&owner->fault, memory_order_relaxed)) {
        if (!input || capture_pcm(owner, input, output, frames) < 0) {
            atomic_store_explicit(&owner->fault, 1u, memory_order_release);
        }
    }
}

static void
notification(const ma_device_notification *event)
{
    if (event->type == ma_device_notification_type_stopped ||
        event->type == ma_device_notification_type_interruption_began ||
        event->type == ma_device_notification_type_rerouted) {
        struct snag_audio_device *owner = event->pDevice->pUserData;
        atomic_fetch_or_explicit(&owner->mute, 1u, memory_order_release);
        atomic_store_explicit(&owner->fault, 2u, memory_order_release);
    }
}

static const ma_device_id *
select_device(const ma_device_info *list, ma_uint32 count, const char *name, bool *valid)
{
    const ma_device_id *found = NULL;
    *valid = true;
    if (!name || !*name) return NULL;
    for (ma_uint32 i = 0; i < count; ++i) if (!strcmp(list[i].name, name)) {
        if (found) { *valid = false; return NULL; }
        found = &list[i].id;
    }
    *valid = found != NULL;
    return found;
}

int
snag_audio_devices(struct snag_buf *out, char *error, size_t size)
{
    ma_context context;
    if (ma_context_init(NULL, 0, NULL, &context) != MA_SUCCESS) {
        snag_errorf(error, size, "No supported audio backend is available"); return -1;
    }
    ma_device_info *play, *capture; ma_uint32 play_count, capture_count;
    int rc = -1;
    if (ma_context_get_devices(&context, &play, &play_count, &capture, &capture_count) == MA_SUCCESS &&
        play_count <= 256u && capture_count <= 256u) {
        rc = snag_buf_printf(out, "Audio devices (exact names; empty selection uses system default):\n");
        for (ma_uint32 i = 0; !rc && i < play_count; ++i)
            rc = snag_buf_printf(out, "playback: %s%s\n", play[i].name, play[i].isDefault ? " [default]" : "");
        for (ma_uint32 i = 0; !rc && i < capture_count; ++i)
            rc = snag_buf_printf(out, "capture: %s%s\n", capture[i].name, capture[i].isDefault ? " [default]" : "");
    }
    ma_context_uninit(&context);
    if (rc) snag_errorf(error, size, "Audio device enumeration failed or exceeded its bound");
    return rc;
}

void
snag_audio_close(struct snag_audio_device *owner)
{
    if (!owner) return;
    if (owner->device_ready) ma_device_uninit(&owner->device);
    if (owner->context_ready) ma_context_uninit(&owner->context);
    if (owner->echo) speex_echo_state_destroy(owner->echo);
    memset(owner->echo_capture, 0, sizeof(owner->echo_capture));
    if (owner->input_samples) memset(owner->input_samples, 0, 65536u * sizeof(int16_t));
    free(owner->input_samples); free(owner->output_samples); free(owner);
}

static struct snag_audio_device *
audio_buffers(bool capture, bool playback, unsigned int channels)
{
    struct snag_audio_device *owner = calloc(1u, sizeof(*owner));
    if (!owner) return NULL;
    owner->capture = capture; owner->playback = playback; owner->channels = channels;
    atomic_init(&owner->mute, capture && playback ? 1u : 0u); atomic_init(&owner->silenced, 0u);
    atomic_init(&owner->fault, 0u); atomic_init(&owner->delivered, 0u);
    snag_pcm_playout_init(&owner->playout, 480u * channels);
    if (capture) {
        owner->input_samples = calloc(65536u, sizeof(int16_t));
        if (snag_pcm_init(&owner->input, owner->input_samples, 65536u) < 0) goto failed;
    }
    if (playback) {
        owner->output_samples = calloc(1048576u * channels, sizeof(int16_t));
        if (snag_pcm_init(&owner->output, owner->output_samples, 1048576u * channels) < 0) goto failed;
    }
    if (capture && playback) {
        int rate = 24000;
        owner->echo = speex_echo_state_init_mc(SNAG_ECHO_FRAMES, rate / 4, 1, (int)channels);
        if (!owner->echo || speex_echo_ctl(owner->echo, SPEEX_ECHO_SET_SAMPLING_RATE, &rate) < 0) {
            goto failed;
        }
    }
    return owner;
failed:
    snag_audio_close(owner);
    return NULL;
}

int
snag_audio_open(bool capture, bool playback, unsigned int channels,
    const char *input_name, const char *output_name, struct snag_audio_device **out,
    char *error, size_t size)
{
    *out = NULL;
    if ((!capture && !playback) || channels < 1u || channels > 2u) return -1;
    struct snag_audio_device *owner = audio_buffers(capture, playback, channels);
    if (!owner) return -1;
    if (ma_context_init(NULL, 0, NULL, &owner->context) != MA_SUCCESS) goto failed;
    owner->context_ready = true;
    ma_device_config config = ma_device_config_init(capture ?
        (playback ? ma_device_type_duplex : ma_device_type_capture) : ma_device_type_playback);
    ma_device_info *play, *inputs; ma_uint32 play_count, input_count;
    if (ma_context_get_devices(&owner->context, &play, &play_count, &inputs, &input_count) != MA_SUCCESS) goto failed;
    bool valid;
    if (capture) {
        config.capture.pDeviceID = select_device(inputs, input_count, input_name, &valid);
        if (!valid) goto failed;
        config.capture.format = ma_format_s16; config.capture.channels = 1;
    }
    if (playback) {
        config.playback.pDeviceID = select_device(play, play_count, output_name, &valid);
        if (!valid) goto failed;
        config.playback.format = ma_format_s16; config.playback.channels = channels;
    }
    config.sampleRate = 24000; config.periodSizeInMilliseconds = 20;
    config.dataCallback = callback; config.notificationCallback = notification; config.pUserData = owner;
    if (ma_device_init(&owner->context, &config, &owner->device) != MA_SUCCESS) goto failed;
    owner->device_ready = true;
    if (ma_device_start(&owner->device) != MA_SUCCESS) goto failed;
    *out = owner;
    return 0;
failed:
    snag_errorf(error, size, "Cannot open/start requested audio device; check permission, exact names and backend availability");
    snag_audio_close(owner);
    return -1;
}

uint32_t snag_audio_capture(struct snag_audio_device *owner, int16_t *samples, uint32_t frames)
{
    return owner->capture && !(atomic_load(&owner->mute) & 1u) && !atomic_load(&owner->fault) ?
        snag_pcm_read(&owner->input, samples, frames) : 0;
}
uint32_t snag_audio_play(struct snag_audio_device *owner, const int16_t *samples, uint32_t frames)
{
    if (!owner->playback || atomic_load(&owner->fault)) return 0;
    uint32_t pending = snag_pcm_available(&owner->output) / owner->channels;
    if (pending > 720000u) return 0;
    if (frames > 720000u - pending) frames = 720000u - pending;
    return snag_pcm_write(&owner->output, samples, frames * owner->channels) / owner->channels;
}
void snag_audio_interrupt(struct snag_audio_device *owner)
{
    if (owner->playback) { snag_pcm_flush(&owner->output); snag_audio_finish(owner); }
}
void snag_audio_finish(struct snag_audio_device *owner)
{
    if(owner->playback)snag_pcm_playout_end(&owner->playout, &owner->output);
}
uint32_t snag_audio_gaps(const struct snag_audio_device *owner)
{ return atomic_load_explicit(&owner->playout.gaps, memory_order_relaxed); }
int snag_audio_mute(struct snag_audio_device *owner, bool mute)
{
    if (!owner->capture) return -1;
    unsigned int gate = atomic_load(&owner->mute);
    if (mute) { if (!(gate & 1u)) atomic_store(&owner->mute, gate + 1u); return 0; }
    if (!(gate & 1u)) return 0;
    if (atomic_load_explicit(&owner->silenced, memory_order_acquire) != gate) return 1;
    snag_pcm_discard(&owner->input);
    if (owner->echo) {
        owner->echo_frames = 0u;
        memset(owner->echo_capture, 0, sizeof(owner->echo_capture));
        memset(owner->echo_render, 0, sizeof(owner->echo_render));
        speex_echo_state_reset(owner->echo);
    }
    atomic_store(&owner->mute, gate + 1u);
    return 0;
}
#ifdef SNAJPAGENT_TEST_TRANSPORT_ENDPOINTS
struct snag_audio_device *
snag_audio_fixture_duplex(void)
{
    struct snag_audio_device *owner = audio_buffers(true, true, 1u);
    if (owner) owner->device.pUserData = owner;
    return owner;
}

void
snag_audio_fixture_io(struct snag_audio_device *owner, int16_t *out, const int16_t *in,
    uint32_t frames)
{
    callback(&owner->device, out, in, frames);
}

/* Exercise the actual callback/gate without initializing any backend. */
int snag_audio_fixture_capture(void)
{
    struct snag_audio_device owner={0};int16_t ring[1024],input[480],out[480];
    owner.capture=true;owner.device.pUserData=&owner;
    atomic_init(&owner.mute,1u);atomic_init(&owner.silenced,0u);
    atomic_init(&owner.fault,0u);atomic_init(&owner.delivered,0u);
    if(snag_pcm_init(&owner.input,ring,1024u)<0)return -1;
    for(size_t i=0;i<480u;++i)input[i]=(int16_t)i;
    callback(&owner.device,NULL,input,480u);
    if(snag_audio_capture(&owner,out,480u) || snag_pcm_available(&owner.input))return -1;
    if(snag_audio_mute(&owner,false))return -1;
    callback(&owner.device,NULL,input,480u);
    if(snag_audio_capture(&owner,out,480u)!=480u || memcmp(input,out,sizeof(input)))return -1;
    callback(&owner.device,NULL,input,480u); /* An old capture prefix. */
    if(snag_audio_mute(&owner,true) || snag_audio_capture(&owner,out,480u) || snag_audio_mute(&owner,false)!=1)return -1;
    callback(&owner.device,NULL,input,480u);
    if(snag_audio_mute(&owner,false) || snag_pcm_available(&owner.input))return -1;
    callback(&owner.device,NULL,input,480u);
    return snag_audio_capture(&owner,out,480u)==480u && !memcmp(input,out,sizeof(input))?0:-1;
}
#endif
int snag_audio_fault(const struct snag_audio_device *owner) { return (int)atomic_load(&owner->fault); }
uint32_t snag_audio_pending(const struct snag_audio_device *owner)
{
    return owner->playback ? snag_pcm_available(&owner->output) / owner->channels : 0;
}
uint32_t snag_audio_delivered(const struct snag_audio_device *owner) { return atomic_load(&owner->delivered); }
uint32_t snag_audio_latency_ms(const struct snag_audio_device *owner)
{
    uint64_t frames = (uint64_t)owner->device.playback.internalPeriodSizeInFrames * owner->device.playback.internalPeriods;
    uint32_t rate = owner->device.playback.internalSampleRate;
    return rate ? (uint32_t)(frames * 1000u / rate) : 0;
}
#else
int snag_audio_devices(struct snag_buf *out, char *error, size_t size)
{ (void)out; snag_errorf(error, size, "This custom build excludes audio devices (WITH_AUDIO_DEVICE=0)"); return -1; }
int snag_audio_open(bool capture, bool playback, unsigned int channels, const char *input, const char *output,
                     struct snag_audio_device **device, char *error, size_t size)
{ (void)capture; (void)playback; (void)channels; (void)input; (void)output; *device = NULL; return snag_audio_devices(NULL, error, size); }
void snag_audio_close(struct snag_audio_device *device) { (void)device; }
uint32_t snag_audio_capture(struct snag_audio_device *d, int16_t *p, uint32_t n) { (void)d; (void)p; (void)n; return 0; }
uint32_t snag_audio_play(struct snag_audio_device *d, const int16_t *p, uint32_t n) { (void)d; (void)p; (void)n; return 0; }
void snag_audio_interrupt(struct snag_audio_device *d) { (void)d; }
void snag_audio_finish(struct snag_audio_device *d) { (void)d; }
uint32_t snag_audio_gaps(const struct snag_audio_device *d) { (void)d; return 0; }
int snag_audio_mute(struct snag_audio_device *d, bool m) { (void)d; (void)m; return -1; }
int snag_audio_fault(const struct snag_audio_device *d) { (void)d; return 1; }
uint32_t snag_audio_pending(const struct snag_audio_device *d) { (void)d; return 0; }
uint32_t snag_audio_delivered(const struct snag_audio_device *d) { (void)d; return 0; }
uint32_t snag_audio_latency_ms(const struct snag_audio_device *d) { (void)d; return 0; }
#endif
