/* SPDX-License-Identifier: GPL-2.0-only */
#include "voice_rtc.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#if SNAJPAGENT_AUDIO_DEVICE
#include <rtc/rtc.h>
#include <opus/opus.h>
#include <pthread.h>
#include <stdatomic.h>

/* Bounded encoded jitter queue, not a second device/playback owner. */
#define RTP_SLOTS 64u
#define OPUS_PACKET 1500u
struct rtc_packet { uint16_t seq; size_t size; unsigned char data[OPUS_PACKET]; };
struct snag_voice_rtc {
    int pc,track;
    pthread_mutex_t mutex;
    atomic_bool gathered,failed;
    OpusEncoder *encoder;
    OpusDecoder *decoder;
    struct rtc_packet packets[RTP_SLOTS];
    uint16_t next;
    uint32_t timestamp;
    uint32_t frame_size;
    uint64_t due;
    bool receiving,output_started;
    int16_t input[480];
    uint32_t input_count;
};
static uint16_t be16(const unsigned char *p) { return (uint16_t)((uint16_t)p[0]<<8u|p[1]); }
static void gathering(int id,rtcGatheringState state,void *opaque)
{
    (void)id;
    if (state==RTC_GATHERING_COMPLETE)
        atomic_store(&((struct snag_voice_rtc *)opaque)->gathered,true);
}
static void connection(int id,rtcState state,void *opaque)
{
    (void)id;
    if (state==RTC_FAILED || state==RTC_CLOSED)
        atomic_store(&((struct snag_voice_rtc *)opaque)->failed,true);
}
static void received(int id,const char *message,int size,void *opaque)
{
    (void)id;
    struct snag_voice_rtc *r=opaque;
    const unsigned char *p=(const unsigned char *)message;
    if (size<12 || (p[0]>>6u)!=2u || (p[1]>=192u && p[1]<=223u))return;
    size_t off=12u+4u*(p[0]&15u),end=(size_t)size;
    if (off>end)return;
    if (p[0]&16u) { if (end-off<4u)return; off+=4u+4u*be16(p+off+2u); }
    if (p[0]&32u) { unsigned int pad=p[end-1u]; if (!pad || pad>end)return; end-=pad; }
    if (off>=end || end-off>OPUS_PACKET || (p[1]&127u)!=111u)return;
    uint16_t seq=be16(p+2u);
    pthread_mutex_lock(&r->mutex);
    if (!r->output_started) {pthread_mutex_unlock(&r->mutex);return;}
    if (!r->receiving) {r->next=seq;r->receiving=true;r->due=snag_monotonic_ms()+60u;}
    int16_t distance=(int16_t)(seq-r->next);
    if (distance>=0 && distance<(int)RTP_SLOTS) {
        struct rtc_packet *packet=&r->packets[seq%RTP_SLOTS];
        packet->seq=seq;packet->size=end-off;memcpy(packet->data,p+off,packet->size);
    } else if (distance>=(int)RTP_SLOTS) {
        /* A stalled consumer cannot grow an unbounded media backlog. */
        atomic_store(&r->failed,true);
    }
    pthread_mutex_unlock(&r->mutex);
}
void snag_voice_rtc_close(struct snag_voice_rtc *r)
{
    if (!r)return;
    if (r->track>=0) {rtcSetMessageCallback(r->track,NULL);rtcDeleteTrack(r->track);}
    if (r->pc>=0)rtcDeletePeerConnection(r->pc);
    if (r->encoder)opus_encoder_destroy(r->encoder);
    if (r->decoder)opus_decoder_destroy(r->decoder);
    pthread_mutex_destroy(&r->mutex);memset(r,0,sizeof(*r));free(r);
}
int snag_voice_rtc_open(struct snag_voice_rtc **out,char *error,size_t size)
{
    *out=NULL;
    struct snag_voice_rtc *r=calloc(1,sizeof(*r));
    if (!r)return -1;
    r->pc=r->track=-1;
    r->frame_size=480u;
    if (pthread_mutex_init(&r->mutex,NULL)) {free(r);return -1;}
    atomic_init(&r->gathered,false);atomic_init(&r->failed,false);
    int rc;
    r->encoder=opus_encoder_create(24000,1,OPUS_APPLICATION_VOIP,&rc);
    r->decoder=opus_decoder_create(24000,1,&rc);
    if (!r->encoder || !r->decoder)goto fail;
    opus_encoder_ctl(r->encoder,OPUS_SET_BITRATE(32000));
    rtcConfiguration config={.disableAutoNegotiation=true,.forceMediaTransport=true};
    r->pc=rtcCreatePeerConnection(&config);
    if (r->pc<0)goto fail;
    rtcSetUserPointer(r->pc,r);
    if (rtcSetGatheringStateChangeCallback(r->pc,gathering)<0 ||
        rtcSetStateChangeCallback(r->pc,connection)<0)goto fail;
    uint32_t ssrc;uint16_t sequence;
    if (snag_random_bytes((unsigned char *)&ssrc,sizeof(ssrc))<0 ||
        snag_random_bytes((unsigned char *)&sequence,sizeof(sequence))<0 ||
        snag_random_bytes((unsigned char *)&r->timestamp,sizeof(r->timestamp))<0)goto fail;
    rtcTrackInit track={.direction=RTC_DIRECTION_SENDRECV,.codec=RTC_CODEC_OPUS,
        .payloadType=111,.ssrc=ssrc,.mid="0",.name="snajpagent",.msid="voice",.trackId="audio"};
    r->track=rtcAddTrackEx(r->pc,&track);
    if (r->track<0)goto fail;
    rtcSetUserPointer(r->track,r);
    rtcPacketizerInit packetizer={.ssrc=ssrc,.cname="snajpagent",.payloadType=111,.clockRate=48000,
        .sequenceNumber=sequence,.timestamp=r->timestamp};
    if (rtcSetOpusPacketizer(r->track,&packetizer)<0 || rtcChainRtcpReceivingSession(r->track)<0 ||
        rtcChainRtcpSrReporter(r->track)<0 || rtcSetMessageCallback(r->track,received)<0 ||
        rtcSetLocalDescription(r->pc,"offer")<0)goto fail;
    *out=r;return 0;
fail:
    snag_voice_rtc_close(r);
    return snag_fail(error,size,EIO,
        "Cannot initialize native voice media transport");
}
int snag_voice_rtc_offer(struct snag_voice_rtc *r,struct snag_buf *out)
{
    if (atomic_load(&r->failed))return -1;
    if (!atomic_load(&r->gathered))return 0;
    char sdp[32768];int n=rtcGetLocalDescription(r->pc,sdp,sizeof(sdp));
    if (n<=0 || (size_t)n>sizeof(sdp) ||
        snag_buf_append(out,sdp,strlen(sdp))<0 || snag_buf_terminate(out)<0)
        return -1;
    return 1;
}
int snag_voice_rtc_answer(struct snag_voice_rtc *r,const char *sdp)
{ return rtcSetRemoteDescription(r->pc,sdp,"answer"); }
bool snag_voice_rtc_ready(struct snag_voice_rtc *r)
{
    return !atomic_load(&r->failed) && rtcIsOpen(r->track);
}
int snag_voice_rtc_input(struct snag_voice_rtc *r,const int16_t *pcm,uint32_t frames)
{
    if (atomic_load(&r->failed))return -1;
    if (!frames) {memset(r->input,0,sizeof(r->input));r->input_count=0u;return 0;}
    if (!pcm)return -1;
    while (frames) {
        uint32_t n=480u-r->input_count;if (n>frames)n=frames;
        memcpy(r->input+r->input_count,pcm,n*sizeof(*pcm));r->input_count+=n;pcm+=n;frames-=n;
        if (r->input_count<480u)continue;
        unsigned char packet[OPUS_PACKET];
        int size=opus_encode(r->encoder,r->input,480,packet,sizeof(packet));r->input_count=0u;
        if (size<0 || rtcSetTrackRtpTimestamp(r->track,r->timestamp)<0 ||
            rtcSendMessage(r->track,(const char *)packet,size)<0)return -1;
        r->timestamp+=960u;
    }
    return 0;
}
int snag_voice_rtc_output(struct snag_voice_rtc *r,int16_t *pcm,uint32_t capacity)
{
    if (atomic_load(&r->failed))return -1;
    pthread_mutex_lock(&r->mutex);
    int count=0;
    r->output_started=true;
    if (r->receiving) {
        struct rtc_packet *p=&r->packets[r->next%RTP_SLOTS];
        bool present=p->size && p->seq==r->next;
        bool later=false;
        for (size_t i=0;i<RTP_SLOTS;++i)later|=r->packets[i].size!=0u;
        /* Only a missing packet waits for reordering. The existing device ring
         * owns playback timing; pacing decoded packets here would add latency
         * and strand valid bursts behind a second clock. */
        if (present || (later && snag_monotonic_ms()>=r->due)) {
            count=opus_decode(r->decoder,present?p->data:NULL,present?(opus_int32)p->size:0,
                pcm,(int)(present?capacity:r->frame_size),0);
            if (present)p->size=0;
            if (count>0)r->frame_size=(uint32_t)count;
            ++r->next;r->due=snag_monotonic_ms()+60u;
        }
    }
    pthread_mutex_unlock(&r->mutex);return count;
}
void snag_voice_rtc_flush(struct snag_voice_rtc *r)
{
    pthread_mutex_lock(&r->mutex);
    memset(r->packets,0,sizeof(r->packets));r->receiving=false;
    opus_decoder_ctl(r->decoder,OPUS_RESET_STATE);
    pthread_mutex_unlock(&r->mutex);
}
#else
int snag_voice_rtc_open(struct snag_voice_rtc **out,char *e,size_t n)
{
    *out=NULL;
    return snag_fail(e,n,ENOTSUP,
        "Native voice is unavailable in this audio-disabled build");
}
int snag_voice_rtc_offer(struct snag_voice_rtc *r,struct snag_buf *b) {(void)r;(void)b;return -1;}
int snag_voice_rtc_answer(struct snag_voice_rtc *r,const char *s) {(void)r;(void)s;return -1;}
bool snag_voice_rtc_ready(struct snag_voice_rtc *r) {(void)r;return false;}
int snag_voice_rtc_input(struct snag_voice_rtc *r,const int16_t *p,uint32_t n)
{
    (void)r;(void)p;(void)n;return -1;
}
int snag_voice_rtc_output(struct snag_voice_rtc *r,int16_t *p,uint32_t n)
{
    (void)r;(void)p;(void)n;return -1;
}
void snag_voice_rtc_flush(struct snag_voice_rtc *r) {(void)r;}
void snag_voice_rtc_close(struct snag_voice_rtc *r) {(void)r;}
#endif
