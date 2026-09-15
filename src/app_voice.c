/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"
#include "audio_device.h"
#include "voice.h"
#include "voice_rtc.h"
#include "provider.h"
#include "secret.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* One live socket/device owner; the coding/session owner exchanges only
 * bounded notices and one terminal coding result. No second executor. */
#define VOICE_MESSAGE (2u*1024u*1024u)
#define VOICE_NOTICES 16u
struct app_voice {
    pthread_t thread;
    pthread_mutex_t mutex;
    atomic_bool stop,muted,mute_pending,activate,done;
    bool thread_started,announced,applied_mute,device_started,stopped_recorded,expiry_warned;
    struct snag_provider_config provider;
    struct snag_audio_config config;
    struct snag_credential credential;
    struct snag_secret_set secrets;
    struct snag_voice_socket *socket;
    struct snag_voice_rtc *rtc;
    struct snag_voice *protocol;
    struct snag_audio_device *device;
    json_t *notices[VOICE_NOTICES],*result,*context;
    size_t notice_read,notice_count,notice_bytes,notice_size[VOICE_NOTICES];
    struct snag_buf send[8],receive;
    bool send_audio[8];
    size_t send_read,send_count,send_bytes,send_offset;
    uint64_t send_deadline,start_ms,drained_ms,expires_ms;
    char connection[33],error[256],audio_item[SNAG_MAX_PROVIDER_ID+1u];
    uint32_t audio_base,gaps;
    bool gap_reported;
    char caption[2][384],caption_item[2][SNAG_MAX_PROVIDER_ID+1u];
    bool caption_dirty[2]; /* One coalesced preview per speaker, under mutex. */
    /* Session-owner-only handoff/result correlation. */
    char call[SNAG_MAX_PROVIDER_ID+1u],queue[33],turn[33];
    bool result_needed,context_dirty;
};

static int voice_record(struct app_state *app,struct app_voice *v,json_t *event)
{
    if(!event)return -1;
    char error[256];
    struct snag_buf raw,clean;snag_buf_init(&raw,VOICE_MESSAGE);snag_buf_init(&clean,VOICE_MESSAGE);
    int rc=snag_json_canonical(event,&raw);json_decref(event);
    if(!rc)rc=snag_wire_json_redact(raw.data,raw.len,&v->secrets.wire,&clean,error,sizeof(error));
    json_t *safe=!rc?json_loadb((char *)clean.data,clean.len,JSON_REJECT_DUPLICATES,NULL):NULL;
    if(!safe)rc=-1;
    if(!rc)rc=snag_app_commit_event(app,"voice_event",json_pack("{s:s,s:s,s:s,s:o}",
        "connection_id",v->connection,"provider",v->config.provider,"model",v->config.realtime_model,"event",safe),error,sizeof(error));
    snag_secret_clear(raw.data,raw.len);snag_buf_free(&raw);snag_buf_free(&clean);
    return rc;
}

static int owner_notice(void *opaque,const json_t *event)
{
    struct app_voice *v=opaque;
    const char *type=snag_json_string(event,"type"),*speaker=snag_json_string(event,"speaker");
    const char *item=snag_json_string(event,"item_id");
    unsigned int who=speaker && !strcmp(speaker,"assistant");
    if(type && !strcmp(type,"voice_caption")) {
        const char *text=snag_json_string(event,"text");
        if(!speaker || !item || !text)return -1;
        size_t len=strlen(text),size=sizeof(v->caption[who]);
        pthread_mutex_lock(&v->mutex);
        if(strcmp(item,v->caption_item[who])) {
            if(!snag_strcpy(v->caption_item[who],sizeof(v->caption_item[who]),item)) {
                pthread_mutex_unlock(&v->mutex);return -1;
            }
            v->caption[who][0]=0;
        }
        size_t old=strlen(v->caption[who]);
        if(len>=size) {
            size_t skip=len-(size-1u);
            while(skip<len && ((unsigned char)text[skip]&0xc0u)==0x80u)++skip;
            text+=skip;len-=skip;old=0;
        } else if(old+len>=size) {
            size_t skip=old+len-(size-1u);
            while(skip<old && ((unsigned char)v->caption[who][skip]&0xc0u)==0x80u)++skip;
            memmove(v->caption[who],v->caption[who]+skip,old-skip);old-=skip;
        }
        memcpy(v->caption[who]+old,text,len);v->caption[who][old+len]=0;
        v->caption_dirty[who]=true;
        pthread_mutex_unlock(&v->mutex);return 0;
    }
    char digest[SNAG_SHA256_HEX_LEN+1u];size_t bytes;
    if(snag_json_digest_bounded(event,VOICE_MESSAGE,digest,&bytes)<0)return -1;
    pthread_mutex_lock(&v->mutex);
    if(type && !strcmp(type,"voice_muted") && json_is_true(json_object_get(event,"muted"))) {
        v->caption[0][0]=0;v->caption_dirty[0]=true;
    }
    if(type && ((!strcmp(type,"voice_transcript") && item && !strcmp(item,v->caption_item[who])) ||
        !strcmp(type,"voice_asr_failed") || !strcmp(type,"voice_interrupted"))) {
        if(!strcmp(type,"voice_interrupted"))who=1u;
        if(strcmp(type,"voice_asr_failed") || (item && !strcmp(item,v->caption_item[who]))) {
            v->caption[who][0]=0;v->caption_dirty[who]=true;
        }
    }
    int rc=-1;
    if(v->notice_count<VOICE_NOTICES && bytes<=VOICE_MESSAGE-v->notice_bytes) {
        size_t at=(v->notice_read+v->notice_count)%VOICE_NOTICES;
        v->notices[at]=json_deep_copy(event);
        if(v->notices[at]) {v->notice_size[at]=bytes;v->notice_count++;v->notice_bytes+=bytes;rc=0;}
    }
    pthread_mutex_unlock(&v->mutex);return rc;
}
#ifdef SNAJPAGENT_TEST_TRANSPORT_ENDPOINTS
/* Existing transport tests exercise main-owner close without opening devices
 * or connecting to a provider. Production has no fixture entry point. */
int snag_app_voice_fixture(struct app_state *app,const json_t *notices,bool done)
{
    if(app->voice)return -1;
    struct app_voice *v=calloc(1,sizeof(*v));if(!v)return -1;
    if(pthread_mutex_init(&v->mutex,NULL)) {free(v);return -1;}
    atomic_init(&v->stop,false);atomic_init(&v->muted,false);atomic_init(&v->mute_pending,false);
    atomic_init(&v->activate,false);atomic_init(&v->done,done);
    strcpy(v->connection,"0123456789abcdef0123456789abcdef");
    strcpy(v->config.provider,"default");strcpy(v->config.realtime_model,"fixture");
    for(size_t i=0;i<8u;++i)snag_buf_init(&v->send[i],VOICE_MESSAGE);
    snag_buf_init(&v->receive,VOICE_MESSAGE);v->announced=true;app->voice=v;
    for(size_t i=0;i<json_array_size(notices);++i)
        if(owner_notice(v,json_array_get(notices,i))<0)return -1;
    return 0;
}
#endif

static int owner_send(void *opaque,const json_t *event)
{
    struct app_voice *v=opaque;
    if(v->send_count==8u)return -1;
    struct snag_buf *out=&v->send[(v->send_read+v->send_count)%8u];
    snag_buf_reset(out);
    if(snag_json_canonical(event,out)<0 || out->len>VOICE_MESSAGE-v->send_bytes)return -1;
    if(!v->send_count)v->send_deadline=snag_monotonic_ms()+2000u;
    const char *type=snag_json_string(event,"type");
    v->send_audio[(v->send_read+v->send_count)%8u]=type && !strcmp(type,"input_audio_buffer.append");
    ++v->send_count;v->send_bytes+=out->len;return 0;
}
static int owner_play(void *opaque,const char *item,const int16_t *samples,uint32_t frames)
{
    struct app_voice *v=opaque;
    if(!v->device)return -1;
    if(!frames) {snag_audio_finish(v->device);return 0;}
    if(strcmp(v->audio_item,item)) {
        if(snag_audio_pending(v->device))return -1;
        if(!snag_strcpy(v->audio_item,sizeof(v->audio_item),item))return -1;
        v->audio_base=snag_audio_delivered(v->device);
        v->gap_reported=false;
    }
    v->drained_ms=0;
    return snag_audio_play(v->device,samples,frames)==frames?0:-1;
}
static uint32_t owner_interrupt(void *opaque)
{
    struct app_voice *v=opaque;
    if(v->rtc)snag_voice_rtc_flush(v->rtc);
    if(!v->device)return 0;
    /* Read before flush, so callback progress racing the flush cannot be
     * attributed to audio heard by the user. Subtract device pipeline latency. */
    uint32_t frames=snag_audio_delivered(v->device)-v->audio_base;
    uint32_t ms=frames/24u,latency=snag_audio_latency_ms(v->device);
    snag_audio_interrupt(v->device);v->drained_ms=0;
    return ms>latency?ms-latency:0;
}
static int owner_controls(void *opaque,unsigned int timeout)
{
    (void)timeout;return atomic_load(&((struct app_voice *)opaque)->stop)?2:0;
}
static int owner_flush(struct app_voice *v)
{
    if(!v->send_count || atomic_load(&v->stop))return 0;
    if(!v->send_offset && v->send_audio[v->send_read] &&
        (atomic_load(&v->muted) || atomic_load(&v->mute_pending))) {
        struct snag_buf *out=&v->send[v->send_read];v->send_bytes-=out->len;
        snag_secret_clear(out->data,out->len);snag_buf_reset(out);
        v->send_read=(v->send_read+1u)%8u;--v->send_count;return 0;
    }
    if(snag_monotonic_ms()>v->send_deadline) {strcpy(v->error,"Realtime send stalled; stopped without replay");return -1;}
    struct snag_buf *out=&v->send[v->send_read];
    size_t before=v->send_offset;
    if(snag_provider_voice_send(v->socket,out->data,out->len,&v->send_offset,v->error,sizeof(v->error))<0)return -1;
    if(v->send_offset!=before)v->send_deadline=snag_monotonic_ms()+2000u;
    if(v->send_offset==out->len) {
        v->send_bytes-=out->len;snag_secret_clear(out->data,out->len);snag_buf_reset(out);
        v->send_read=(v->send_read+1u)%8u;--v->send_count;v->send_offset=0;
    }
    return 0;
}

static int owner_mute(struct app_voice *v)
{
    if(!snag_voice_ready(v->protocol))return 0;
    bool mute=atomic_load(&v->muted);
    /* A complete mute/unmute pair between iterations still clears old input. */
    if(atomic_exchange(&v->mute_pending,false))mute=true;
    if(mute==v->applied_mute)return 0;
    int rc=v->device?snag_audio_mute(v->device,mute):0;
    if(rc)return rc<0?-1:0;
    v->applied_mute=mute;
    if(mute && v->rtc && snag_voice_rtc_input(v->rtc,NULL,0u)<0)return -1;
    /* An unsent audio message can be withdrawn. A partially sent frame must
     * finish before clear, preserving WebSocket framing. */
    if(mute && v->send_count && !v->send_offset && v->send_audio[v->send_read]) {
        struct snag_buf *out=&v->send[v->send_read];v->send_bytes-=out->len;
        snag_secret_clear(out->data,out->len);snag_buf_reset(out);
        v->send_read=(v->send_read+1u)%8u;--v->send_count;
    }
    if(snag_voice_mute(v->protocol,mute,v->error,sizeof(v->error))<0)return -1;
    if(mute || v->device) {
        json_t *event=json_pack("{s:s,s:b}","type","voice_muted","muted",mute);
        rc=event?owner_notice(v,event):-1;json_decref(event);
    }
    return rc;
}

#ifdef SNAJPAGENT_TEST_TRANSPORT_ENDPOINTS
int snag_app_voice_fixture_mute(struct app_state *app)
{
    struct app_voice *v=app->voice;if(!v)return -1;
    struct snag_voice_io io={owner_send,owner_notice,owner_play,owner_interrupt};
    v->protocol=snag_voice_new(&io,v,"fixture","asr","voice");
    if(!v->protocol || snag_voice_begin(v->protocol,v->error,sizeof(v->error))<0)return -1;
    json_t *sent=json_loadb((char *)v->send[0].data,v->send[0].len,0,NULL);
    json_t *event=sent?json_pack("{s:s,s:O}","type","session.updated","session",json_object_get(sent,"session")):NULL;
    int rc=event?snag_voice_event(v->protocol,event,v->error,sizeof(v->error)):-1;
    json_decref(event);json_decref(sent);if(rc<0)return -1;
    snag_buf_reset(&v->send[0]);v->send_bytes=v->send_count=0;
    int16_t pcm[480]={0};
    if(snag_voice_input(v->protocol,pcm,480u,v->error,sizeof(v->error))<0)return -1;
    atomic_store(&v->muted,true);atomic_store(&v->mute_pending,true);atomic_store(&v->muted,false);
    if(owner_mute(v)<0 || !v->applied_mute || v->send_count!=1u || v->send_audio[v->send_read])return -1;
    if(owner_mute(v)<0 || v->applied_mute || v->notice_count!=1u)return -1;
    /* A late mute at flush time withdraws an untouched PCM frame too. */
    snag_buf_reset(&v->send[v->send_read]);v->send_bytes=v->send_count=0;
    if(snag_voice_input(v->protocol,pcm,480u,v->error,sizeof(v->error))<0)return -1;
    atomic_store(&v->muted,true);
    if(owner_flush(v)<0 || v->send_count)return -1;
    /* Partially sent PCM stays ahead of clear until its framing completes. */
    if(snag_voice_input(v->protocol,pcm,480u,v->error,sizeof(v->error))<0)return -1;
    v->send_offset=1u;
    if(owner_mute(v)<0 || v->send_count!=2u || !v->send_audio[v->send_read] || v->send_offset!=1u)return -1;
    snag_voice_free(v->protocol);v->protocol=NULL;
    return 0;
}
#endif

static void *voice_owner(void *opaque)
{
    struct app_voice *v=opaque;
    struct snag_voice_io io={owner_send,owner_notice,owner_play,owner_interrupt};
    v->start_ms=snag_monotonic_ms();
    v->protocol=snag_voice_new(&io,v,v->config.realtime_model,v->config.transcribe_model,v->config.voice);
    if(!v->protocol)goto done;
    if(snag_provider_native_audio(&v->provider)) {
        struct snag_buf offer={.max=32768u},answer={.max=32768u};char call[257];
        json_t *session=snag_voice_native_session(v->protocol);int rc=-1;
        if(!session || snag_voice_rtc_open(&v->rtc,v->error,sizeof(v->error))<0)goto native_done;
        while(!atomic_load(&v->stop) && snag_monotonic_ms()-v->start_ms<15000u) {
            rc=snag_voice_rtc_offer(v->rtc,&offer);
            if(rc)break;
            snag_sleep_ms(10u);
        }
        if(rc!=1) {rc=-1;snprintf(v->error,sizeof(v->error),"Native voice media preparation stopped or timed out");goto native_done;}
        rc=snag_provider_voice_call(NULL,&v->provider,&v->credential,(char *)offer.data,session,
            owner_controls,v,&answer,call,v->error,sizeof(v->error));
        if(rc)goto native_done;
        if(snag_voice_rtc_answer(v->rtc,(char *)answer.data)<0) {
            rc=-1;strcpy(v->error,"Native voice media answer could not be applied");goto native_done;
        }
        rc=snag_provider_voice_attach(&v->provider,&v->credential,call,owner_controls,v,
            &v->socket,v->error,sizeof(v->error));
native_done:
        json_decref(session);snag_buf_free(&offer);snag_buf_free(&answer);
        if(rc)goto done;
    } else if(snag_provider_voice_open(&v->provider,&v->credential,v->config.realtime_model,owner_controls,v,
        &v->socket,v->error,sizeof(v->error)))goto done;
    snag_credential_clear(&v->credential);
    if(snag_voice_begin(v->protocol,v->error,sizeof(v->error))<0)goto done;
    while(!atomic_load(&v->stop)) {
        uint64_t now=snag_monotonic_ms();
        if(v->announced && v->rtc && !snag_voice_rtc_ready(v->rtc)) {
            strcpy(v->error,"Native voice media connection stopped");break;
        }
        if((!snag_voice_ready(v->protocol) || (v->rtc && !snag_voice_rtc_ready(v->rtc))) &&
            now-v->start_ms>v->provider.connect_timeout_ms+10000u) {
            strcpy(v->error,"Voice session or media connection did not become ready");break;
        }
        if(v->expires_ms && now>=v->expires_ms) {strcpy(v->error,"Realtime session lifetime reached; restart voice explicitly");break;}
        if(v->expires_ms && !v->expiry_warned && v->expires_ms-now<=60000u) {
            json_t *event=json_pack("{s:s}","type","voice_expiring");
            int rc=event?owner_notice(v,event):-1;json_decref(event);if(rc<0)goto failed;
            v->expiry_warned=true;
        }
        if(owner_mute(v)<0)goto failed;
        if(owner_flush(v)<0)break;
        for(unsigned int i=0;i<16u;++i) {
            int rc=snag_provider_voice_receive(v->socket,&v->receive,v->error,sizeof(v->error));
            if(rc<0)goto done;
            if(!rc)break;
            json_t *event=snag_json_load_strict(v->receive.data,v->receive.len,VOICE_MESSAGE,v->error,sizeof(v->error));
            if(!event)goto done;
            const char *type=snag_json_string(event,"type");
            if(type && (!strcmp(type,"session.created") || !strcmp(type,"session.started"))) {
                uint64_t expires;
                if(!snag_json_integer_u64(json_object_get(event,"session"),"expires_at",&expires)) {
                    uint64_t seconds=(uint64_t)time(NULL);
                    if(expires<=seconds || expires-seconds>(UINT64_MAX-now)/1000u) {json_decref(event);goto failed;}
                    v->expires_ms=now+(expires-seconds)*1000u;
                }
            }
            rc=snag_voice_event(v->protocol,event,v->error,sizeof(v->error));json_decref(event);
            snag_secret_clear(v->receive.data,v->receive.len);snag_buf_reset(&v->receive);
            if(rc<0)goto done;
        }
        if(snag_voice_ready(v->protocol) && (!v->rtc || snag_voice_rtc_ready(v->rtc)) && !v->announced) {
            json_t *event=json_pack("{s:s}","type","voice_ready");
            int rc=event?owner_notice(v,event):-1;json_decref(event);if(rc<0)goto failed;
            v->announced=true;
        }
        if(v->announced && !v->device_started && !atomic_load(&v->stop) && !atomic_load(&v->muted) && atomic_load(&v->activate)) {
            if(snag_audio_open(true,true,1u,v->config.capture_device,v->config.playback_device,&v->device,v->error,sizeof(v->error))<0)break;
            /* Duplex opens gated: even mute/stop during backend startup cannot
             * accumulate stale capture. Unmute is acknowledged next iteration. */
            v->device_started=true;v->applied_mute=true;
            if(snag_voice_mute(v->protocol,true,v->error,sizeof(v->error))<0)break;
        }
        pthread_mutex_lock(&v->mutex);json_t *result=v->result;v->result=NULL;pthread_mutex_unlock(&v->mutex);
        pthread_mutex_lock(&v->mutex);json_t *context=v->context;v->context=NULL;pthread_mutex_unlock(&v->mutex);
        if(context) {
            int rc=snag_voice_context(v->protocol,context,v->error,sizeof(v->error));
            json_decref(context);if(rc<0) {json_decref(result);break;}
        }
        if(result) {
            int rc=snag_voice_result(v->protocol,snag_json_string(result,"call_id"),snag_json_string(result,"text"),v->error,sizeof(v->error));
            json_decref(result);if(rc<0)break;
        }
        if(v->device) {
            if(snag_audio_fault(v->device)) {strcpy(v->error,"Realtime audio device stopped, rerouted or overflowed");break;}
            uint32_t gaps=snag_audio_gaps(v->device);
            if(gaps!=v->gaps) {
                v->gaps=gaps;
                if(!v->gap_reported) {
                    json_t *event=json_pack("{s:s}","type","voice_buffering");
                    int rc=event?owner_notice(v,event):-1;json_decref(event);
                    if(rc<0)goto failed;
                    v->gap_reported=true;
                }
            }
            /* Only one small audio message enters the socket queue. Urgent
             * controls never wait behind a buffered stream of audio messages. */
            if(!atomic_load(&v->stop) && !atomic_load(&v->muted) && !v->applied_mute && !v->send_count) {
                int16_t pcm[480];uint32_t n=snag_audio_capture(v->device,pcm,480u);
                int rc=n?(v->rtc?snag_voice_rtc_input(v->rtc,pcm,n):
                    snag_voice_input(v->protocol,pcm,n,v->error,sizeof(v->error))):0;
                snag_secret_clear(pcm,sizeof(pcm));if(rc<0)break;
            }
            if(v->rtc) {
                for(unsigned int i=0;i<4u;++i) {
                    int16_t pcm[2880];
                    int n=snag_voice_rtc_output(v->rtc,pcm,2880u);
                    if(n<0 || (n>0 && snag_voice_native_output(v->protocol,pcm,(uint32_t)n)<0)) {
                        strcpy(v->error,"Native voice media stopped or playback fell behind");goto done;
                    }
                    if(!n)break;
                }
            }
            bool drained=snag_audio_pending(v->device)==0u;
            if(!drained)v->drained_ms=0;
            else if(!v->drained_ms)v->drained_ms=now;
            drained=drained && now-v->drained_ms>=snag_audio_latency_ms(v->device);
            if(snag_voice_respond(v->protocol,drained,v->error,sizeof(v->error))<0)break;
        }
        if(owner_flush(v)<0 || snag_provider_voice_wait(v->socket,v->send_count!=0u,10u)<0)break;
    }
    goto done;
failed:
    strcpy(v->error,"Realtime voice stopped because its device, protocol or mailbox became unavailable");
done:
    snag_audio_close(v->device);v->device=NULL;
    snag_provider_voice_close(v->socket);v->socket=NULL;
    snag_voice_rtc_close(v->rtc);v->rtc=NULL;
    snag_voice_free(v->protocol);v->protocol=NULL;
    snag_credential_clear(&v->credential);
    atomic_store_explicit(&v->done,true,memory_order_release);return NULL;
}

void snag_app_voice_close(struct app_state *app)
{
    struct app_voice *v=app->voice;if(!v)return;
    atomic_store(&v->stop,true);
    if(v->thread_started)pthread_join(v->thread,NULL);
    /* The worker is joined: preserve final notices on shutdown/error as well
     * as /voice off. Closing never accepts a previously unaccepted handoff. */
    bool failed=false;
    while(v->notice_count) {
        json_t *event=v->notices[v->notice_read];v->notices[v->notice_read]=NULL;
        v->notice_read=(v->notice_read+1u)%VOICE_NOTICES;--v->notice_count;
        const char *type=snag_json_string(event,"type");
        if(type && strcmp(type,"voice_ready") && strcmp(type,"voice_handoff") &&
            strcmp(type,"voice_buffering") && strcmp(type,"voice_expiring")) {
            if(voice_record(app,v,event)<0)failed=true;
        } else json_decref(event);
    }
    if(!v->stopped_recorded && v->announced &&
        voice_record(app,v,json_pack("{s:s,s:s}","type","voice_stopped",
            "reason",v->error[0]?v->error:"Voice stopped."))<0)failed=true;
    if(failed)snag_ui_text(&app->ui,SNAG_UI_ERROR,"Voice stopped; some final voice records could not be saved. Inspect the session journal.");
    snag_ui_audio(&app->ui,"",false);
    for(size_t i=0;i<8u;++i) {snag_secret_clear(v->send[i].data,v->send[i].len);snag_buf_free(&v->send[i]);}
    snag_secret_clear(v->receive.data,v->receive.len);snag_buf_free(&v->receive);
    snag_credential_clear(&v->credential);snag_secret_set_free(&v->secrets);json_decref(v->result);json_decref(v->context);
    pthread_mutex_destroy(&v->mutex);free(v);app->voice=NULL;
}

/* This runs only after the existing session owner has durably committed the
 * corresponding queue/turn event. The voice thread never touches app/session. */
void snag_app_voice_event(struct app_state *app,const char *type,const json_t *data)
{
    struct app_voice *v=app->voice;if(!v)return;
    if(!strcmp(type,"turn_started") || !strcmp(type,"future_turn_cancelled") ||
        !strcmp(type,"future_turn_queued") || !strcmp(type,"turn_completed") ||
        !strcmp(type,"turn_completed_silent") || !strcmp(type,"turn_failed") || !strcmp(type,"turn_interrupted"))v->context_dirty=true;
    if(!v->queue[0] || v->result_needed)return;
    if(!strcmp(type,"turn_started")) {
        const char *queue=snag_json_string(data,"queue_id"),*turn=snag_json_string(data,"turn_id");
        if(queue && !strcmp(queue,v->queue) && turn)snag_strcpy(v->turn,sizeof(v->turn),turn);
    } else if(!strcmp(type,"future_turn_cancelled")) {
        json_t *ids=json_object_get(data,"queue_ids");
        for(size_t i=0;i<json_array_size(ids);++i) {
            const char *id=json_string_value(json_array_get(ids,i));
            if(id && !strcmp(id,v->queue))v->result_needed=true;
        }
    } else {
        const char *turn=snag_json_string(data,"turn_id");
        if(!turn || !v->turn[0] || strcmp(turn,v->turn))return;
        if(!strcmp(type,"turn_completed") || !strcmp(type,"turn_completed_silent") ||
            !strcmp(type,"turn_failed") || !strcmp(type,"turn_interrupted"))v->result_needed=true;
    }
}

static int deliver_result(struct app_voice *v,const char *text)
{
    json_t *result=json_pack("{s:s,s:s}","call_id",v->call,"text",text);
    if(!result)return -1;
    pthread_mutex_lock(&v->mutex);
    if(v->result) {pthread_mutex_unlock(&v->mutex);json_decref(result);return -1;}
    v->result=result;pthread_mutex_unlock(&v->mutex);return 0;
}
int snag_app_voice_service(struct app_state *app)
{
    struct app_voice *v=app->voice;if(!v)return 0;
    char error[256];
    if(v->context_dirty && !atomic_load(&v->stop) && !atomic_load(&v->done)) {
        json_t *context=NULL;
        if(snag_session_voice_context(&app->session,&context,error,sizeof(error))<0)goto failed;
        pthread_mutex_lock(&v->mutex);json_decref(v->context);v->context=context;pthread_mutex_unlock(&v->mutex);
        v->context_dirty=false;
    }
    for(;;) {
        pthread_mutex_lock(&v->mutex);
        json_t *event=NULL;
        if(v->notice_count) {
            event=v->notices[v->notice_read];v->notices[v->notice_read]=NULL;
            v->notice_bytes-=v->notice_size[v->notice_read];
            v->notice_read=(v->notice_read+1u)%VOICE_NOTICES;--v->notice_count;
        }
        pthread_mutex_unlock(&v->mutex);
        if(!event)break;
        const char *type=snag_json_string(event,"type");
        if(!strcmp(type,"voice_ready")) {
            if(atomic_load(&v->stop) || atomic_load(&v->done)) {json_decref(event);continue;}
            bool mute=atomic_load(&v->muted);
            if(snag_ui_voice(&app->ui,mute?"[voice mic off; /voice unmute | off] ":"[voice starting mic; /voice mute | off] ")==0) {
                if(voice_record(app,v,json_pack("{s:s}","type","voice_started"))<0) {json_decref(event);goto failed;}
                atomic_store(&v->activate,true);
            } else {json_decref(event);goto failed;}
        } else if(!strcmp(type,"voice_handoff")) {
            if(atomic_load(&v->stop) || atomic_load(&v->done)) {json_decref(event);continue;}
            if(v->call[0]) {json_decref(event);goto failed;}
            json_t *source=json_pack("{s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s}","connection_id",v->connection,
                "input_id",snag_json_string(event,"input_id"),"response_id",snag_json_string(event,"response_id"),
                "call_id",snag_json_string(event,"call_id"),"provider",v->config.provider,"model",v->config.realtime_model,
                "transcript",snag_json_string(event,"transcript"),"request",snag_json_string(event,"request"));
            const char *text_keys[]={"transcript","request"};
            for(size_t i=0;source && i<2u;++i) {
                json_t *text=json_pack("{s:s}","model_text",snag_json_string(source,text_keys[i]));
                if(!text || snag_secret_result(&v->secrets,text,error,sizeof(error))<0 ||
                    json_object_set(source,text_keys[i],json_object_get(text,"model_text"))<0) {
                    json_decref(source);source=NULL;
                }
                json_decref(text);
            }
            bool duplicate=false;
            if(!source || snag_session_voice_queue(&app->session,source,v->queue,&duplicate,error,sizeof(error))<0) {
                json_decref(source);json_decref(event);goto failed;
            }
            json_decref(source);snag_strcpy(v->call,sizeof(v->call),snag_json_string(event,"call_id"));
            v->context_dirty=true;
            if(duplicate) {
                json_t *result=NULL;
                if(snag_session_voice_status(&app->session,v->queue,&result,error,sizeof(error))<0) {json_decref(event);goto failed;}
                const char *status=snag_json_string(result,"status");
                snag_strcpy(v->turn,sizeof(v->turn),snag_json_string(result,"turn_id"));
                v->result_needed=strcmp(status,"queued") && strcmp(status,"running");
                if(!strcmp(status,"queued") && snag_app_queue_arm(app,true)<0) {json_decref(result);json_decref(event);goto failed;}
                int rc=snag_ui_text(&app->ui,SNAG_UI_HOST,snag_json_string(result,"text"));
                json_decref(result);if(rc<0) {json_decref(event);goto failed;}
            } else {
                if(snag_app_queue_arm(app,true)<0) {json_decref(event);goto failed;}
                if(snag_ui_text(&app->ui,SNAG_UI_HOST,"Voice request accepted into the existing coding queue.")<0) {json_decref(event);goto failed;}
            }
        } else if(!strcmp(type,"voice_expiring")) {
            if(snag_ui_text(&app->ui,SNAG_UI_HOST,"Voice connection expires within one minute. Restart explicitly with /voice off, then /voice on.")<0) {
                json_decref(event);goto failed;
            }
        } else if(!strcmp(type,"voice_buffering")) {
            if(snag_ui_text(&app->ui,SNAG_UI_HOST,"Voice audio arrived late; playback prefill adjusted. /voice off stops voice.")<0) {
                json_decref(event);goto failed;
            }
        } else {
            if(voice_record(app,v,json_incref(event))<0) {json_decref(event);goto failed;}
            if(!strcmp(type,"voice_muted")) {
                bool mute=json_is_true(json_object_get(event,"muted"));
                if(mute==atomic_load(&v->muted) && snag_ui_voice(&app->ui,mute?
                    "[VOICE MUTED; /voice unmute | off] ":"[VOICE MIC ON; /voice mute | off] ")<0) {json_decref(event);goto failed;}
            }
            if(!strcmp(type,"voice_transcript")) {
                json_t *safe=json_pack("{s:s}","model_text",snag_json_string(event,"text"));
                if(!safe || snag_secret_result(&v->secrets,safe,error,sizeof(error))<0 ||
                    snag_ui_text(&app->ui,SNAG_UI_HOST,snag_json_string(safe,"model_text"))<0) {
                    json_decref(safe);json_decref(event);goto failed;
                }
                json_decref(safe);
            }
        }
        json_decref(event);
    }
    for(unsigned int who=0;who<2u;++who) {
        char caption[sizeof(v->caption[who])];
        pthread_mutex_lock(&v->mutex);
        bool changed=v->caption_dirty[who];
        memcpy(caption,v->caption[who],sizeof(caption));v->caption_dirty[who]=false;
        pthread_mutex_unlock(&v->mutex);
        if(changed) {
            json_t *safe=json_pack("{s:s}","model_text",caption);
            int rc=safe?snag_secret_result(&v->secrets,safe,error,sizeof(error)):-1;
            if(!rc)rc=snag_ui_caption(&app->ui,who,snag_json_string(safe,"model_text"));
            json_decref(safe);if(rc<0)goto failed;
        }
    }
    if(v->result_needed) {
        json_t *result=NULL;
        if(snag_session_voice_status(&app->session,v->queue,&result,error,sizeof(error))<0)goto failed;
        const char *text=snag_json_string(result,"text");
        int rc=voice_record(app,v,json_pack("{s:s,s:s,s:s,s:s,s:s,s:s}","type","voice_result","call_id",v->call,
            "queue_id",v->queue,"turn_id",snag_json_string(result,"turn_id"),"status",snag_json_string(result,"status"),"text",text));
        if(!rc && !atomic_load(&v->done) && !atomic_load(&v->stop))rc=deliver_result(v,text);
        json_decref(result);if(rc<0)goto failed;
        v->result_needed=false;v->call[0]=v->queue[0]=v->turn[0]=0;
    }
    if(atomic_load_explicit(&v->done,memory_order_acquire)) {
        snprintf(error,sizeof(error),"%s",v->error[0]?v->error:"Voice stopped.");
        int rc=voice_record(app,v,json_pack("{s:s,s:s}","type","voice_stopped","reason",error));
        v->stopped_recorded=!rc;
        snag_app_voice_close(app);snag_ui_audio(&app->ui,"",false);
        return rc<0?-1:snag_ui_text(&app->ui,SNAG_UI_HOST,error);
    }
    return 0;
failed:
    snag_app_voice_close(app);snag_ui_audio(&app->ui,"",false);
    return snag_ui_text(&app->ui,SNAG_UI_ERROR,"Voice stopped: its input, journal, device or UI could not be retained safely. Coding work stays with its existing owner.");
}
int snag_app_voice_command(struct app_state *app,const char *line,bool *handled)
{
    *handled=!strcmp(line,"/voice") || !strncmp(line,"/voice ",7u);
    if(!*handled || !strcmp(line,"/voice devices")) {*handled=false;return 0;}
    if(!strcmp(line,"/voice off")) {
        if(app->voice) {
            struct app_voice *v=app->voice;atomic_store(&v->stop,true);
            if(v->thread_started) {pthread_join(v->thread,NULL);v->thread_started=false;}
            if(snag_app_voice_service(app)<0)return -1;
        }
        return snag_ui_text(&app->ui,SNAG_UI_HOST,"Voice off; coding work remains under the existing session controls.");
    }
    if(!strcmp(line,"/voice mute") || !strcmp(line,"/voice unmute")) {
        if(!app->voice)return snag_ui_text(&app->ui,SNAG_UI_ERROR,"Voice is off. Use /voice on first.");
        bool mute=!strcmp(line,"/voice mute");
        if(mute==atomic_load(&app->voice->muted))return 0;
        if(mute) {
            atomic_store(&app->voice->muted,true);atomic_store(&app->voice->mute_pending,true);
        }
        int rc=snag_ui_voice(&app->ui,mute?"[voice muting; /voice unmute | off] ":"[voice starting mic; /voice mute | off] ");
        if(!rc && !mute)atomic_store(&app->voice->muted,false);
        if(rc)snag_app_voice_close(app);
        return rc<0?-1:0;
    }
    if(strcmp(line,"/voice on"))return snag_ui_text(&app->ui,SNAG_UI_HOST,"Use /voice on, off, mute, unmute or devices.");
    if(app->voice || app->audio || app->attaching)return snag_ui_text(&app->ui,SNAG_UI_ERROR,"Stop the active audio operation before starting voice.");
    struct snag_audio_config resolved;
    const struct snag_provider_config *provider=snag_provider_audio_config(app->config,
        app->session.default_provider,&resolved);
    const struct snag_audio_config *cfg=&resolved;
    if(!provider)return snag_ui_text(&app->ui,SNAG_UI_ERROR,"Selected voice provider is not configured.");
    if(snag_ui_voice(&app->ui,"[voice connecting; mic off; /voice off cancels] ")!=0)
        return snag_ui_text(&app->ui,SNAG_UI_ERROR,"Voice requires a raw interactive terminal with a visible capture prompt.");
    char message[SNAG_CONFIG_URL_MAX+SNAG_CONFIG_MODEL_MAX+SNAG_CONFIG_PROVIDER_NAME_MAX+256u],error[256];
    if(snag_session_persist(&app->store,&app->session,error,sizeof(error))<0) {
        snag_ui_audio(&app->ui,"",false);return snag_ui_text(&app->ui,SNAG_UI_ERROR,error);
    }
    snprintf(message,sizeof(message),"Voice sends microphone audio and coding context to %.*s. Use /voice mute or /voice off.",
        (int)sizeof(cfg->provider)-1,cfg->provider);
    if(snag_ui_text(&app->ui,SNAG_UI_HOST,message)<0)return -1;
    struct app_voice *v=calloc(1,sizeof(*v));if(!v)return -1;
    v->provider=*provider;v->provider.models=NULL;v->provider.model_count=0;v->config=*cfg;
    memset(&v->provider.api_key,0,sizeof(v->provider.api_key));
    snag_credential_clear(&v->credential);
    if(pthread_mutex_init(&v->mutex,NULL)) {free(v);return -1;}
    atomic_init(&v->stop,false);atomic_init(&v->muted,false);atomic_init(&v->mute_pending,false);
    atomic_init(&v->activate,false);atomic_init(&v->done,false);
    for(size_t i=0;i<8u;++i)snag_buf_init(&v->send[i],VOICE_MESSAGE);
    snag_buf_init(&v->receive,VOICE_MESSAGE);app->voice=v;
    if(snag_random_id(v->connection)<0 || snag_auth_read(app->store.root_fd,provider,false,NULL,&v->credential,
        NULL,NULL,error,sizeof(error))<0 || snag_secret_set_build(&v->secrets,app->config,&v->credential,error,sizeof(error))<0)goto failed;
    if(snag_session_voice_context(&app->session,&v->context,error,sizeof(error))<0)goto failed;
    if(pthread_create(&v->thread,NULL,voice_owner,v))goto failed;
    v->thread_started=true;return 0;
failed:
    snag_app_voice_close(app);snag_ui_audio(&app->ui,"",false);
    return snag_ui_text(&app->ui,SNAG_UI_ERROR,"Voice could not load credentials or start its connection owner; microphone stayed off.");
}
