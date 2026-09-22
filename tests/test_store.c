/* SPDX-License-Identifier: GPL-2.0-only */
#include "checked_json.h"
#include "store.h"
#include "media.h"
#include "fs.h"
#include "instructions.h"
#include "irc.h"
#include "snajpagent.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void
commit_event(struct snag_session *session, const char *type, json_t *data)
{
    char error[256] = {0};
    int rc = snag_session_commit(session, type, data, NULL, error, sizeof(error));
    if (rc != 0) fprintf(stderr, "%s: %s\n", type, error);
    assert(rc == 0);
}

static int
list_to_fd(void *opaque, const char *text, size_t len)
{
    return snag_write_full(*(int *)opaque, text, len);
}

static json_t *
change_data(const char *old_key, const char *old_value, const char *new_key, const char *new_value)
{
    return checked_json(json_pack("{s:s,s:s}", old_key, old_value, new_key, new_value));
}

static int
check_replay(void *opaque, const struct snag_session *state, uint64_t seq,
             const char *type, const json_t *data, char *error, size_t error_size)
{
    const struct snag_session *source = opaque;
    (void)error;
    (void)error_size;
    assert(state && state != source && state->log_fd == -1);
    assert(state->next_seq == seq + 1u);
    assert(state->log_end > 0 && state->log_end <= source->log_end);
    assert(strcmp(state->id, source->id) == 0);
    if (!strcmp(type, "turn_started")) {
        assert(state->active_turn);
        assert(strcmp(state->active_turn_id, snag_json_string(data, "turn_id")) == 0);
    }
    if (seq + 1u == source->next_seq) {
        assert(state->log_end == source->log_end);
        assert(strcmp(state->prev_sha256, source->prev_sha256) == 0);
        assert(state->pending_queue_count == source->pending_queue_count);
        assert(state->pending_queue_bytes == source->pending_queue_bytes);
    }
    return 0;
}

static json_t *
queued_data(const char *turn_id, const char *queue_id, const char *text)
{
    return checked_json(json_pack("{s:s,s:b,s:s,s:s}",
        "queue_id", queue_id, "read_only", 0, "text", text, "while_turn_id", turn_id));
}

static json_t *
edited_data(const char *queue_id, const char *text)
{
    return checked_json(json_pack("{s:s,s:b,s:s}", "queue_id", queue_id, "read_only", 0, "text", text));
}

static json_t *
turn_started_data(const struct snag_session *session, const char *turn_id)
{
    struct snag_instruction_set instructions = {0};
    json_t *metadata = snag_instructions_metadata_json(&instructions);
    assert(metadata);
    return checked_json(json_pack(
        "{s:{s:s,s:s,s:n,s:s,s:s,s:s,s:i,s:i,s:i,s:i,s:b},"
        "s:s,s:b,s:o,s:n,s:n,s:s,s:s,s:i,s:s}",
        "config", "capability_version", SNAJPAGENT_CAPABILITY_VERSION,
        "effort", session->default_effort, "max_output_tokens",
        "model", session->default_model, "provider", session->default_provider,
        "profile_id", SNAJPAGENT_PROFILE_ID, "prompt_schema", 1, "replay_schema", 1,
        "tool_schema", 1, "max_parallel_commands", 4, "parallel_tool_calls", 1,
        "input_kind", "direct", "read_only", 0, "instructions", metadata,
        "queue_id", "queue_seq", "text", "queue test", "turn_id", turn_id,
        "turn_number", (int)session->turn_count + 1, "workspace", session->workspace));
}

static json_t *
goal_started_data(const char *goal_id, const char *prompt)
{
    return checked_json(json_pack("{s:s,s:s}", "goal_id", goal_id, "prompt", prompt));
}

static json_t *
goal_actor_data(const char *goal_id, const char *actor)
{
    return checked_json(json_pack("{s:s,s:s}", "actor", actor, "goal_id", goal_id));
}

static json_t *
goal_reworded_data(const char *goal_id, const char *actor, const char *prompt)
{
    json_t *data = goal_actor_data(goal_id, actor);
    assert(snag_json_set_new(data, "prompt", json_string(prompt)) == 0);
    return data;
}

static json_t *
goal_replaced_data(const char *goal_id, const char *new_goal_id, const char *actor,
                   const char *prompt)
{
    return checked_json(json_pack("{s:s,s:s,s:s,s:s}", "actor", actor,
        "goal_id", goal_id, "new_goal_id", new_goal_id, "prompt", prompt));
}

static json_t *
goal_lock_data(const char *goal_id, bool locked)
{
    return checked_json(json_pack("{s:s,s:b}", "goal_id", goal_id, "locked", locked));
}

static json_t *
goal_reason_data(const char *goal_id, const char *actor, const char *key, const char *reason)
{
    json_t *data = actor ? goal_actor_data(goal_id, actor) : json_object();
    assert(data);
    if (!actor) assert(snag_json_set_new(data, "goal_id", json_string(goal_id)) == 0);
    assert(snag_json_set_new(data, key, json_string(reason)) == 0);
    return data;
}

static json_t *
compaction_interrupted_data(const char *compact_id, const char *reason)
{
    return checked_json(json_pack("{s:s,s:s}", "compact_id", compact_id, "reason", reason));
}

static size_t
read_file(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    ssize_t n;
    assert(fd >= 0);
    n = read(fd, buf, size - 1u);
    assert(n >= 0);
    buf[n] = '\0';
    assert(close(fd) == 0);
    return (size_t)n;
}

static int
count_event(void *opaque, const struct snag_session *state,
            uint64_t seq, const char *type, const json_t *data, char *error, size_t error_size)
{
    size_t *count = opaque;
    assert(state->next_seq == seq + 1u);
    (void)type;
    (void)data;
    (void)error;
    (void)error_size;
    assert(seq == ++*count);
    return 0;
}

static void
test_pending_session(struct snag_store *store, const char *workspace)
{
    struct snag_session session;
    char id[SNAG_ID_HEX_LEN + 1u], error[256];
    struct stat st;
    size_t count = 0u;
    int64_t end;

    snag_session_init(&session);
    assert(snag_session_prepare(&session, workspace, "default", "model", "high", error, sizeof(error)) == 0);
    memcpy(id, session.id, sizeof(id));
    assert(session.pending_log && session.log_fd == -1 && session.dir_fd == -1);
    assert(fstatat(store->sessions_fd, id, &st, 0) < 0 && errno == ENOENT);
    snag_session_close(&session);
    assert(fstatat(store->sessions_fd, id, &st, 0) < 0 && errno == ENOENT);

    assert(snag_session_prepare(&session, workspace, "default", "model", "high", error, sizeof(error)) == 0);
    assert(snag_session_commit(&session, "model_selection_changed", json_pack("{s:s,s:s,s:s,s:s,s:s,s:s}",
                  "old_provider", "default", "new_provider", "default",
                  "old_model", "model", "new_model", "selected",
                  "old_effort", "high", "new_effort", "low"), NULL, error, sizeof(error)) == 0);
    assert(snag_session_each_event(&session, count_event, &count, error, sizeof(error)) == 0 && count == 2u);
    memcpy(id, session.id, sizeof(id));
    end = session.log_end;
    /* Failed publication must leave the complete in-memory session retryable. */
    assert(mkdirat(store->sessions_fd, id, 0700) == 0);
    assert(snag_session_persist(store, &session, error, sizeof(error)) < 0);
    assert(session.pending_log && session.log_end == end && session.next_seq == 3u);
    assert(unlinkat(store->sessions_fd, id, AT_REMOVEDIR) == 0);
    assert(snag_session_persist(store, &session, error, sizeof(error)) == 0);
    assert(!session.pending_log && session.log_fd >= 0 && session.log_end == end);
    assert(snag_session_persist(store, &session, error, sizeof(error)) == 0);
    snag_session_close(&session);
    assert(snag_session_open(store, &session, id, error, sizeof(error)) == 0);
    assert(session.next_seq == 3u && session.log_end == end);
    assert(strcmp(session.default_model, "selected") == 0);
    assert(strcmp(session.default_effort, "low") == 0);
    count = 0u;
    assert(snag_session_each_event(&session, count_event, &count, error, sizeof(error)) == 0 && count == 2u);
    int history = openat(session.dir_fd, "prompt_history", O_CREAT | O_WRONLY, 0600);
    assert(history >= 0 && write(history, "session-only\n", 13u) == 13);
    assert(close(history) == 0);
    id[8] = '\0';
    assert(snag_session_delete(store, &session, id, NULL, error, sizeof(error)) == 0);
    snag_session_close(&session);
}

static void
test_failed_append_retry(struct snag_store *store, const char *workspace)
{
    struct snag_session session;
    struct rlimit saved, limited;
    char error[256], id[SNAG_ID_HEX_LEN + 1u];
    snag_session_init(&session);
    assert(snag_session_create(store, &session, workspace, "default", "test", "default",
                               error, sizeof(error)) == 0);
    memcpy(id, session.id, sizeof(id));
    int64_t end = session.log_end;
    assert(getrlimit(RLIMIT_FSIZE, &saved) == 0);
    limited = saved;
    limited.rlim_cur = (rlim_t)end + 16u;
    void (*old)(int) = signal(SIGXFSZ, SIG_IGN);
    assert(old != SIG_ERR && setrlimit(RLIMIT_FSIZE, &limited) == 0);
    assert(snag_session_commit(&session, "effort_changed",
        change_data("old_effort", "default", "new_effort", "high"), NULL, error, sizeof(error)) < 0);
    assert(setrlimit(RLIMIT_FSIZE, &saved) == 0 && signal(SIGXFSZ, old) != SIG_ERR);
    assert(session.log_end == end && !strcmp(session.default_effort, "default"));
    assert(session.write_failures == 1u);
    assert(lseek(session.log_fd, 0, SEEK_END) == end);
    assert(snag_session_commit(&session, "effort_changed",
        change_data("old_effort", "default", "new_effort", "high"), NULL, error, sizeof(error)) == 0);
    assert(session.write_failures == 1u);
    snag_session_close(&session);
    assert(snag_session_open(store, &session, id, error, sizeof(error)) == 0);
    assert(!strcmp(session.default_effort, "high"));
    snag_session_close(&session);
}

static void
test_pending_input_media(struct snag_store *store, const char *workspace)
{
    struct snag_session session;
    char id[SNAG_ID_HEX_LEN + 1u], error[256], reference[40];
    const char *turn = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
    json_t *asset = NULL, *found = NULL;
    char *path = NULL;
    snag_session_init(&session);
    assert(snag_session_create(store, &session, workspace, "default", "fixture", "medium",
                               error, sizeof(error)) == 0);
    memcpy(id, session.id, sizeof(id));
    assert(snag_media_save(session.dir_fd, "attachment", 10u, "text/plain", &asset,
                           error, sizeof(error)) == 0);
    json_t *content = checked_json(json_pack("[{s:s,s:O}]", "type", "file", "asset", asset));
    json_t *input = checked_json(json_pack("{s:s,s:[],s:s,s:s,s:b,s:I,s:s,s:O}",
        "effort", "medium", "instructions", "model", "fixture", "provider", "default",
        "read_only", 0, "received_at_ms", (json_int_t)1, "text", "queue test", "content", content));
    commit_event(&session, "input_received", json_incref(input));
    assert(json_equal(session.pending_input, input) && !session.active_turn);
    snag_session_close(&session);
    snag_session_init(&session);
    assert(snag_session_open(store, &session, id, error, sizeof(error)) == 0);
    assert(json_equal(session.pending_input, input));
    assert(snprintf(reference, sizeof(reference), "asset:%s", snag_json_string(asset, "id")) == 38);
    assert(snag_session_media(&session, reference, NULL, NULL, NULL, &found, &path,
                              error, sizeof(error)) == 0);
    assert(json_equal(found, asset));
    json_decref(found); free(path);
    uint64_t seq = session.next_seq;
    /* A pending prompt's attachments cannot disappear or be replaced on start. */
    assert(snag_session_commit(&session, "turn_started", turn_started_data(&session, turn),
                               NULL, error, sizeof(error)) < 0);
    json_t *started = turn_started_data(&session, turn);
    assert(json_object_set_new(started, "content",
        json_pack("[{s:s,s:s}]", "type", "input_text", "text", "replacement")) == 0);
    assert(snag_session_commit(&session, "turn_started", started, NULL, error, sizeof(error)) < 0);
    assert(session.next_seq == seq && json_equal(session.pending_input, input));
    started = turn_started_data(&session, turn);
    assert(json_object_set(started, "content", content) == 0);
    commit_event(&session, "turn_started", started);
    assert(session.active_turn && !session.pending_input);
    snag_session_close(&session);
    snag_session_init(&session);
    assert(snag_session_open(store, &session, id, error, sizeof(error)) == 0);
    assert(session.active_turn && !session.pending_input);
    snag_session_close(&session);
    json_decref(input); json_decref(content); json_decref(asset);
}

static void
test_closure_reserve(struct snag_store *store, const char *workspace)
{
    struct snag_session session;
    char error[256];
    snag_session_init(&session);
    assert(snag_session_create(store, &session, workspace, "default", "test", "default",
                               error, sizeof(error)) == 0);
    uint64_t seq = session.next_seq;
    int64_t end = session.log_end;
    /* Exercise admission at the event boundary without writing a million
     * records. Restore the real cursor before re-opening this private fixture. */
    session.next_seq = UINT64_C(1000000) - 256u + 1u;
    assert(snag_session_commit(&session, "effort_changed",
        change_data("old_effort", "default", "new_effort", "high"),
        NULL, error, sizeof(error)) < 0 && errno == ENOSPC);
    assert(session.log_end == end && !strcmp(session.default_effort, "default"));
    assert(snag_session_commit(&session, "session_archived", json_pack("{s:s}", "origin", "user"),
                               NULL, error, sizeof(error)) == 0);
    assert(session.archived && session.log_end > end);
    assert(ftruncate(session.log_fd, end) == 0 && fsync(session.log_fd) == 0);
    session.next_seq = seq;
    session.log_end = end;
    session.archived = false;
    snag_session_close(&session);
}

static int
check_audio_usage(void *opaque,const struct snag_session *state,uint64_t seq,const char *type,const json_t *data,char *error,size_t size)
{
    (void)state;(void)error;(void)size;
    if(strcmp(type,"audio_usage"))return 0;
    assert(seq==2u);
    assert(!strcmp(snag_json_string(data,"operation"),"dictation"));
    assert(!strcmp(snag_json_string(data,"provider"),"default"));
    assert(!strcmp(snag_json_string(data,"model"),"fixture-transcribe"));
    assert(!strcmp(snag_json_string(data,"report"),"Provider-reported duration: 0.01 seconds"));
    ++*(unsigned int *)opaque;return 0;
}

static void
test_audio_usage(struct snag_store *store,const char *workspace)
{
    struct snag_session session;char id[33],error[256];
    snag_session_init(&session);
    assert(snag_session_create(store,&session,workspace,"default",SNAJPAGENT_MODEL,"medium",error,sizeof(error))==0);
    memcpy(id,session.id,sizeof(id));
    json_t *event=json_pack("{s:s,s:s,s:s,s:s}","operation","dictation","provider","default",
        "model","fixture-transcribe","report","Provider-reported duration: 0.01 seconds");
    assert(event);
    json_t *bad=json_deep_copy(event);assert(bad);
    assert(json_object_set_new(bad,"operation",json_string("execute"))==0);
    assert(snag_session_commit(&session,"audio_usage",bad,NULL,error,sizeof(error))<0);
    bad=json_deep_copy(event);assert(bad);
    assert(json_object_set_new(bad,"report",json_null())==0);
    assert(snag_session_commit(&session,"audio_usage",bad,NULL,error,sizeof(error))<0);
    assert(session.next_seq==2u);
    assert(snag_session_commit(&session,"audio_usage",event,NULL,error,sizeof(error))==0);
    for(unsigned int replay=0;replay<2u;++replay) {
        unsigned int found=0;
        assert(snag_session_each_event(&session,check_audio_usage,&found,error,sizeof(error))==0 && found==1u);
        assert(session.next_seq==3u && !session.turn_count && !session.active_turn);
        assert(!session.usage_anchor.valid && !session.context_meter.valid && !session.pending_queue_count);
        snag_session_close(&session);snag_session_init(&session);
        if(!replay)assert(snag_session_open(store,&session,id,error,sizeof(error))==0);
    }
}

static void voice_status(struct snag_session *session,const char *queue,const char *expected,const char *turn)
{
    char error[256];json_t *result=NULL;uint64_t seq=session->next_seq;
    assert(snag_session_voice_status(session,queue,&result,error,sizeof(error))==0);
    assert(!strcmp(snag_json_string(result,"status"),expected));
    assert(!strcmp(snag_json_string(result,"turn_id"),turn));
    assert(snag_json_string(result,"text") && session->next_seq==seq);
    json_decref(result);
}

static void
test_banner_steering(struct snag_store *store,const char *workspace)
{
    struct snag_session session;char id[33],error[256];
    uint64_t durable_seq;
    snag_session_init(&session);
    assert(snag_session_create(store,&session,workspace,"default",SNAJPAGENT_MODEL,"medium",error,sizeof(error))==0);
    memcpy(id,session.id,sizeof(id));
    assert(!session.banner_text && !session.steering_override);
    commit_event(&session,"banner_updated",checked_json(json_pack("{s:s}","text","lead cursor: land r33")));
    assert(session.banner_text && !strcmp(session.banner_text,"lead cursor: land r33"));
    commit_event(&session,"steering_updated",checked_json(json_pack("{s:s}","mode","all")));
    assert(session.steering_override && !strcmp(session.steering_override,"all"));
    durable_seq = session.next_seq;
    assert(snag_session_commit(&session,"steering_updated",checked_json(json_pack("{s:s}","mode","every")),NULL,error,sizeof(error))<0);
    assert(session.next_seq == durable_seq);
    assert(session.steering_override && !strcmp(session.steering_override,"all"));
    assert(snag_session_commit(&session,"banner_updated",checked_json(json_pack("{s:i}","text",1)),NULL,error,sizeof(error))<0);
    assert(session.next_seq == durable_seq);
    assert(session.banner_text && !strcmp(session.banner_text,"lead cursor: land r33"));
    snag_session_close(&session);snag_session_init(&session);
    assert(snag_session_open(store,&session,id,error,sizeof(error))==0);
    assert(session.banner_text && !strcmp(session.banner_text,"lead cursor: land r33"));
    assert(session.steering_override && !strcmp(session.steering_override,"all"));
    commit_event(&session,"banner_updated",checked_json(json_pack("{s:s}","text","")));
    commit_event(&session,"steering_updated",checked_json(json_pack("{s:s}","mode","")));
    assert(!session.banner_text && !session.steering_override);
    snag_session_close(&session);snag_session_init(&session);
    assert(snag_session_open(store,&session,id,error,sizeof(error))==0);
    assert(!session.banner_text && !session.steering_override);
    snag_session_close(&session);
}

static void
test_voice_queue(struct snag_store *store,const char *workspace)
{
    struct snag_session session;char id[33],queue[33],again[33],error[256];bool duplicate;
    snag_session_init(&session);
    assert(snag_session_create(store,&session,workspace,"default",SNAJPAGENT_MODEL,"medium",error,sizeof(error))==0);
    memcpy(id,session.id,sizeof(id));
    json_t *source=json_pack("{s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s}",
        "connection_id","0123456789abcdef0123456789abcdef","input_id","utterance-1","response_id","response-1",
        "call_id","call-1","provider","default","model","voice-model","transcript","inspect the build",
        "request","look for compiler errors");
    assert(source);
    assert(snag_session_voice_queue(&session,source,queue,&duplicate,error,sizeof(error))==0 && !duplicate);
    assert(!session.active_turn && session.pending_queue_count==1u && session.next_seq==3u);
    voice_status(&session,queue,"queued","");
    assert(!strcmp(session.pending_queue[0].queue_id,queue));
    assert(strstr(session.pending_queue[0].text,"ASR-derived") && strstr(session.pending_queue[0].text,"inspect the build"));
    assert(strstr(session.pending_queue[0].text,"not an additional user instruction or approval"));
    uint64_t seq=session.next_seq;
    assert(snag_session_voice_queue(&session,source,again,&duplicate,error,sizeof(error))==0 && duplicate);
    assert(!strcmp(queue,again) && session.next_seq==seq && session.pending_queue_count==1u);
    /* A new call/paraphrase of an already accepted utterance keeps its first acceptance. */
    assert(json_object_set_new(source,"call_id",json_string("call-2"))==0);
    assert(json_object_set_new(source,"request",json_string("different paraphrase"))==0);
    assert(snag_session_voice_queue(&session,source,again,&duplicate,error,sizeof(error))==0 && duplicate);
    assert(session.next_seq==seq);
    snag_session_close(&session);snag_session_init(&session);
    assert(snag_session_open(store,&session,id,error,sizeof(error))==0);
    assert(session.pending_queue_count==1u && !strcmp(session.pending_queue[0].queue_id,queue));
    assert(snag_session_voice_queue(&session,source,again,&duplicate,error,sizeof(error))==0 && duplicate);
    assert(snag_session_commit(&session,"future_turn_cancelled",json_pack("{s:[s],s:s}","queue_ids",queue,"reason","user"),NULL,error,sizeof(error))==0);
    seq=session.next_seq;
    assert(snag_session_voice_queue(&session,source,again,&duplicate,error,sizeof(error))==0 && duplicate);
    assert(session.next_seq==seq && !session.pending_queue_count);
    voice_status(&session,queue,"cancelled","");
    assert(json_object_set_new(source,"input_id",json_string("utterance-2"))==0);
    assert(snag_session_voice_queue(&session,source,again,&duplicate,error,sizeof(error))==0 && !duplicate);
    assert(strcmp(queue,again) && session.pending_queue_count==1u);
    const char *turn="11111111111111111111111111111111";
    json_t *started=turn_started_data(&session,turn);
    assert(json_object_set_new(started,"input_kind",json_string("queued"))==0);
    assert(json_object_set_new(started,"queue_id",json_string(again))==0);
    assert(json_object_set_new(started,"queue_seq",json_integer((json_int_t)session.pending_queue[0].seq))==0);
    assert(json_object_set_new(started,"text",json_string(session.pending_queue[0].text))==0);
    assert(snag_session_commit(&session,"turn_started",started,NULL,error,sizeof(error))==0);
    voice_status(&session,again,"running",turn);
    assert(snag_session_commit(&session,"turn_failed",json_pack("{s:s,s:s,s:s}","turn_id",turn,
        "class","provider","message","fixture failure"),NULL,error,sizeof(error))==0);
    voice_status(&session,again,"failed",turn);
    /* Later keyboard work and replay cannot replace the original result. */
    const char *keyboard="22222222222222222222222222222222";
    started=turn_started_data(&session,keyboard);
    assert(json_object_set_new(started,"turn_number",json_integer(2))==0);
    assert(snag_session_commit(&session,"turn_started",started,NULL,error,sizeof(error))==0);
    voice_status(&session,again,"failed",turn);
    snag_session_close(&session);snag_session_init(&session);
    assert(snag_session_open(store,&session,id,error,sizeof(error))==0);
    voice_status(&session,queue,"cancelled","");voice_status(&session,again,"failed",turn);
    char *long_text=malloc(10001u);assert(long_text);memset(long_text,'x',10000u);long_text[10000]=0;
    assert(snag_session_commit(&session,"voice_event",json_pack("{s:s,s:s,s:s,s:{s:s,s:s,s:s,s:s}}",
        "connection_id","0123456789abcdef0123456789abcdef","provider","default","model","fixture",
        "event","type","voice_transcript","speaker","user","item_id","input-3","text",long_text),NULL,error,sizeof(error))==0);
    free(long_text);json_t *context=NULL;seq=session.next_seq;
    assert(snag_session_voice_context(&session,&context,error,sizeof(error))==0);
    const char *excerpt=snag_json_string(context,"recent_asr");
    assert(excerpt && strlen(excerpt)<8192u && strstr(excerpt,"[excerpt truncated]"));
    assert(!strcmp(snag_json_string(json_object_get(context,"latest_voice_handoff"),"status"),"failed"));
    assert(session.next_seq==seq);json_decref(context);
    json_t *missing=NULL;
    assert(snag_session_voice_status(&session,"ffffffffffffffffffffffffffffffff",&missing,error,sizeof(error))<0 && !missing);
    json_decref(source);snag_session_close(&session);
}

static int
cancel_media(void *opaque, unsigned int timeout_ms)
{
    unsigned int *calls = opaque;
    (void)timeout_ms;
    return ++*calls == 2u;
}

static void
test_media(const struct snag_session *session)
{
    const unsigned char data[] = {0, 1, 2, 0xff, 0x80, 7};
    char error[256];
    char *source = snag_path_join(session->workspace, "input.png");
    char *link = snag_path_join(session->workspace, "alias.png");
    json_t *asset = NULL, *bad = NULL;
    struct snag_buf bytes;
    snag_file_info info;
    unsigned int calls = 0;
    int fd, media_fd;
    assert(source && link);
    fd = open(source, O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(fd >= 0 && snag_write_full(fd, data, sizeof(data)) == 0);
    close(fd);
    assert(snag_media_snapshot(session->dir_fd, session->workspace, "input.png",
        "image/png", sizeof(data), NULL, NULL, &asset, error, sizeof(error)) == 0);
    assert(asset && snag_media_valid(asset));
    media_fd = snag_open_read_at(session->dir_fd, "media", true);
    assert(media_fd >= 0);
    assert(snag_lstat_at(media_fd, snag_json_string(asset, "id"), &info) == 0);
    assert(info.st_size == (int64_t)sizeof(data) && info.st_nlink == 1u);
    assert((info.st_mode & 077u) == 0);
    /* A caller changing/removing the original cannot affect accepted bytes. */
    fd = open(source, O_WRONLY | O_TRUNC);
    assert(fd >= 0 && snag_write_full(fd, "other", 5u) == 0);
    close(fd);
    assert(unlink(source) == 0);
    snag_buf_init(&bytes, 32u);
    assert(snag_buf_append(&bytes, "prefix", 6u) == 0);
    assert(snag_media_read(session->dir_fd, asset, &bytes, error, sizeof(error)) == 0);
    assert(bytes.len == 6u + sizeof(data));
    assert(!memcmp(bytes.data + 6u, data, sizeof(data)));
    /* Validation cannot permit a retained-reference path to escape media/. */
    bad = json_deep_copy(asset);
    assert(bad && snag_json_set_new(bad, "id", json_string("../events.jsonl")) == 0);
    assert(!snag_media_valid(bad));
    assert(snag_media_read(session->dir_fd, bad, &bytes, error, sizeof(error)) < 0);
    assert(bytes.len == 6u + sizeof(data));
    json_decref(bad);
    bad = json_deep_copy(asset);
    assert(snag_json_set_new(bad, "sha256", json_string(
        "0000000000000000000000000000000000000000000000000000000000000000")) == 0);
    assert(snag_media_read(session->dir_fd, bad, &bytes, error, sizeof(error)) < 0);
    assert(bytes.len == 6u + sizeof(data));
    json_decref(bad); bad = NULL;
    /* Path traversal rejects symlinks and binary input isn't mistaken for text. */
    fd = open(source, O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(fd >= 0 && snag_write_full(fd, data, sizeof(data)) == 0); close(fd);
    assert(symlink(source, link) == 0);
    assert(snag_media_snapshot(session->dir_fd, session->workspace, "alias.png",
        "image/png", 32u, NULL, NULL, &bad, error, sizeof(error)) < 0 && !bad);
    assert(snag_media_snapshot(session->dir_fd, session->workspace, "input.png",
        "image/png", 2u, NULL, NULL, &bad, error, sizeof(error)) < 0 && !bad);
    assert(snag_media_snapshot(session->dir_fd, session->workspace, "input.png",
        "image/png", 32u, cancel_media, &calls, &bad, error, sizeof(error)) < 0 && !bad);
    assert(errno == ECANCELED && calls == 2u);
    assert(snag_media_snapshot(session->dir_fd, session->workspace, "input.png",
        "text/plain\ninvalid", 32u, NULL, NULL, &bad, error, sizeof(error)) < 0 && !bad);
    /* Reopened session directory reads retained assets, not the live source. */
    fd = snag_open_read(session->dir_path, true);
    assert(fd >= 0);
    snag_buf_reset(&bytes);
    assert(snag_media_read(fd, asset, &bytes, error, sizeof(error)) == 0);
    close(fd);
    /* Corruption and missing files fail without exposing a partial append. */
    fd = snag_create_private_at(media_fd, snag_json_string(asset, "id"), false);
    assert(fd >= 0 && snag_write_full(fd, "X", 1u) == 0); close(fd);
    assert(snag_media_read(session->dir_fd, asset, &bytes, error, sizeof(error)) < 0);
    assert(bytes.len == sizeof(data));
    assert(snag_unlink_at(media_fd, snag_json_string(asset, "id"), false) == 0);
    assert(snag_media_read(session->dir_fd, asset, &bytes, error, sizeof(error)) < 0);
    assert(bytes.len == sizeof(data));
    assert(snag_media_snapshot(session->dir_fd, session->workspace, "input.png",
        "image/png", 32u, NULL, NULL, &bad, error, sizeof(error)) == 0);
    assert(unlink(source) == 0 && unlink(link) == 0);
    close(media_fd);
    assert(snag_media_remove(session->dir_fd, error, sizeof(error)) == 0);
    assert(snag_media_read(session->dir_fd, bad, &bytes, error, sizeof(error)) < 0);
    assert(snag_media_remove(session->dir_fd, error, sizeof(error)) == 0);
    /* Scratch cleanup and live-worker ownership also work in lean builds. */
    int work=snag_media_work_open(session->dir_fd,error,sizeof(error));
    assert(work>=0);
    assert(snag_mkdir_private_at(work,"profile")==0);
    int profile=snag_open_read_at(work,"profile",true);
    assert(profile>=0);
    int residue=snag_create_private_at(profile,"residue",true);
    assert(residue>=0 && snag_write_full(residue,"private",7u)==0);close(residue);
    assert(snag_media_work_check(work)==0);
    /* Cleanup removes an unexpected link itself, preserving its destination. */
    char *work_path=snag_path_join(session->dir_path,SNAG_MEDIA_WORK_NAME);
    char *link_path=snag_path_join(work_path,"outside");
    assert(link_path && symlink(session->dir_path,link_path)==0);
    assert(snag_media_work_check(work)<0);
    assert(snag_unlink_at(work,"outside",false)==0);
    free(link_path);free(work_path);
    residue=snag_create_private_at(profile,"too-large",true);
    assert(residue>=0 && snag_truncate(residue,SNAG_MEDIA_WORK_MAX+1ull)==0);close(residue);
    assert(snag_media_work_check(work)<0);
    assert(snag_unlink_at(profile,"too-large",false)==0);
    close(profile);
    struct snag_directory_lock lock={.fd=-1};
    assert(snag_directory_lock_acquire(work,&lock)==0);
    assert(snag_media_work_remove(session->dir_fd,error,sizeof(error))==1);
    assert(snag_media_work_open(session->dir_fd,error,sizeof(error))<0);
    /* Retained inventory can exceed the removed 1 GiB / 4096-file caps.
     * Sparse fixtures avoid allocating that logical size in the test. */
    assert(snag_mkdir_private_at(session->dir_fd,"media")==0);
    media_fd=snag_open_read_at(session->dir_fd,"media",true);
    assert(media_fd>=0);
    for(unsigned int i=0;i<4097u;++i) {
        char retained_id[33];snprintf(retained_id,sizeof(retained_id),"%032x",i);
        residue=snag_create_private_at(media_fd,retained_id,true);
        assert(residue>=0);
        if(!i)assert(snag_truncate(residue,(1ull<<30)+1u)==0);
        close(residue);
    }
    json_t *accepted=NULL;
    assert(snag_media_save(session->dir_fd,data,sizeof(data),"image/png",&accepted,error,sizeof(error))==0);
    json_decref(accepted);accepted=NULL;
    fd=open(source,O_CREAT|O_EXCL|O_RDWR,0600);
    assert(fd>=0 && snag_write_full(fd,data,sizeof(data))==0);close(fd);
    assert(snag_media_snapshot(session->dir_fd,session->workspace,"input.png","image/png",
        sizeof(data),NULL,NULL,&accepted,error,sizeof(error))==0);
    json_decref(accepted);assert(unlink(source)==0);close(media_fd);
    assert(snag_media_remove(session->dir_fd,error,sizeof(error))==0);
    assert(snag_directory_lock_release(&lock)==0);close(work);
    assert(snag_media_work_remove(session->dir_fd,error,sizeof(error))==0);
    assert(snag_media_work_remove(session->dir_fd,error,sizeof(error))==0);
    assert(snag_lstat_at(session->dir_fd,SNAG_MEDIA_WORK_NAME,&info)<0 && errno==ENOENT);
    json_decref(bad);
    json_decref(asset);
    snag_buf_free(&bytes);
    free(source); free(link);
}

static void
test_refusal_diagnostic(const char *workspace)
{
    struct snag_session session;
    char error[256] = {0};

    /* A refused transition must name the clause that failed and the call it
     * concerned, so a crash report is diagnostic without a debug build. */
    snag_session_init(&session);
    assert(snag_session_prepare(&session, workspace, "default", "model", "high",
               error, sizeof(error)) == 0);
    assert(snag_session_commit(&session, "tool_finished",
               json_pack("{s:s}", "call_id", "0123456789abcdef"), NULL, error,
               sizeof(error)) < 0);
    assert(strstr(error, "clause=keys") != NULL);
    assert(strstr(error, "call=0123456789abcdef") != NULL);
    /* The same refusal is persisted beside the session, so a later occurrence is captured without
     * anyone keeping a terminal scrollback. Read the live session's own directory rather than
     * assuming the layout, and scan the whole append-only file. */
    {
        char path[PATH_MAX];
        char content[4096];
        size_t got;
        FILE *log;
        (void)snprintf(path, sizeof(path), "%s/refusals.log", session.workspace);
        log = fopen(path, "r");
        assert(log != NULL);
        got = fread(content, 1u, sizeof(content) - 1u, log);
        content[got] = '\0';
        (void)fclose(log);
        assert(strstr(content, "clause=keys") != NULL);
        assert(strstr(content, "call=0123456789abcdef") != NULL);
    }
    snag_session_close(&session);
}

static void
test_many_queued_turns(struct snag_store *store, const char *workspace)
{
    static const char turn_id[] = "55555555555555555555555555555555";
    struct snag_session session;
    char error[256] = {0};

    snag_session_init(&session);
    assert(snag_session_create(store, &session, workspace, "default", "gpt-5.5-2026-04-23", "default",
                              error, sizeof(error)) == 0);
    commit_event(&session, "turn_started", turn_started_data(&session, turn_id));
    for (unsigned int i = 0; i < 130u; ++i) {
        char queue_id[SNAG_ID_HEX_LEN + 1u], text[32];
        (void)snprintf(queue_id, sizeof(queue_id), "%032x", 0x400000u + i);
        (void)snprintf(text, sizeof(text), "queued %u", i);
        commit_event(&session, "future_turn_queued", queued_data(turn_id, queue_id, text));
    }
    assert(session.pending_queue_count == 130u);
    assert(session.pending_queue_bytes > 0u);
    assert(strcmp(session.pending_queue[129].text, "queued 129") == 0);
    snag_session_close(&session);
}

int
main(void)
{
    char *temp = snag_path_join(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
                                "snajpagent-store-XXXXXX");
    char state[4096];
    char workspace[4096];
    char workspace2[4096];
    char id[SNAG_ID_HEX_LEN + 1u];
    char id_prefix[9];
    char trash_name[SNAG_ID_HEX_LEN + 1u + SNAG_ID_HEX_LEN + 1u];
    char list_path[4096];
    char list_buf[4096];
    char error[256];
    struct snag_store store;
    struct snag_session session;
    off_t durable_end;
    uint64_t durable_seq;
    json_t *bad;

    assert(temp && mkdtemp(temp));
    assert(snprintf(state, sizeof(state), "%s/state", temp) > 0);
    assert(snprintf(workspace, sizeof(workspace), "%s/work", temp) > 0);
    assert(mkdir(state, 0700) == 0);
    assert(mkdir(workspace, 0700) == 0);
    assert(snprintf(workspace2, sizeof(workspace2), "%s/work2", temp) > 0);
    assert(mkdir(workspace2, 0700) == 0);
    snag_store_init(&store);
    snag_session_init(&session);
    assert(snag_store_open(&store, state, error, sizeof(error)) == 0);
    test_pending_session(&store, workspace);
    test_failed_append_retry(&store, workspace);
    test_pending_input_media(&store, workspace);
    test_closure_reserve(&store, workspace);
    assert(snag_session_create(&store, &session, workspace, "default", "gpt-5.5-2026-04-23", "default",
                              error, sizeof(error)) == 0);
    test_media(&session);
    memcpy(id, session.id, sizeof(id));
    memcpy(id_prefix, session.id, 8u);
    id_prefix[8] = '\0';
    commit_event(&session, "model_selection_changed", json_pack("{s:s,s:s,s:s,s:s,s:s,s:s}",
                     "old_model", "gpt-5.5-2026-04-23", "new_model", "gpt-5.5-2026-04-23-alt",
                     "old_provider", "default", "new_provider", "default",
                     "old_effort", "default", "new_effort", "default"));
    commit_event(&session, "effort_changed", change_data("old_effort", "default", "new_effort", "high"));
    assert(strcmp(session.default_model, "gpt-5.5-2026-04-23-alt") == 0);
    assert(strcmp(session.default_effort, "high") == 0);
    commit_event(&session, "workspace_changed", change_data("old_workspace", workspace,
                     "new_workspace", workspace2));
    assert(strcmp(session.workspace, workspace2) == 0);
    {
        int writable = session.log_fd;
        uint64_t written = UINT64_MAX;
        json_t *change = change_data("old_workspace", workspace2, "new_workspace", workspace);

        session.log_fd = openat(session.dir_fd, "events.jsonl", O_RDONLY);
        assert(session.log_fd >= 0);
        struct snag_session before = session;
        assert(snag_session_commit(&session, "workspace_changed",
            json_incref(change), &written, error, sizeof(error)) < 0);
        ++before.write_failures; /* Only the process-local error diagnostic advances. */
        assert(memcmp(&session, &before, sizeof(session)) == 0);
        assert(written == UINT64_MAX);
        assert(strcmp(session.workspace, workspace2) == 0);
        assert(strcmp(snag_json_string(change, "new_workspace"), workspace) == 0);
        json_decref(change);
        assert(close(session.log_fd) == 0);
        session.log_fd = writable;
    }
    durable_end = session.log_end;
    durable_seq = session.next_seq;
    bad = json_object();
    assert(bad);
    assert(snag_json_set_new(bad, "final_item_id", json_string("00000000000000000000000000000000")) == 0);
    assert(snag_json_set_new(bad, "final_response_id", json_string("00000000000000000000000000000000")) == 0);
    assert(snag_json_set_new(bad, "turn_id", json_string("00000000000000000000000000000000")) == 0);
    assert(snag_session_commit(&session, "turn_completed", bad, NULL, error, sizeof(error)) < 0);
    assert(session.log_end == durable_end);
    assert(session.next_seq == durable_seq);
    assert(snag_write_full(session.log_fd, "incomplete", 10u) == 0);
    assert(snag_sync_file(session.log_fd) == 0);
    int crashed_work=snag_media_work_open(session.dir_fd,error,sizeof(error));
    assert(crashed_work>=0);
    int crashed_file=snag_create_private_at(crashed_work,"residue",true);
    assert(crashed_file>=0 && snag_write_full(crashed_file,"residue",7u)==0);
    close(crashed_file);close(crashed_work);
    snag_session_close(&session);

    snag_session_init(&session);
    assert(snag_session_open(&store, &session, id, error, sizeof(error)) == 0);
    snag_file_info scratch_info;
    assert(snag_lstat_at(session.dir_fd,SNAG_MEDIA_WORK_NAME,&scratch_info)<0 && errno==ENOENT);
    assert(session.log_end == durable_end);
    assert(session.next_seq == 5u);
    assert(session.turn_count == 0u);
    assert(strcmp(session.workspace, workspace2) == 0);
    assert(strcmp(session.default_model, "gpt-5.5-2026-04-23-alt") == 0);
    assert(strcmp(session.default_effort, "high") == 0);

    {
        char text[SNAG_IRC_TEXT_MAX + 2u];
        json_t *event;
        json_t *oversized;

        memset(text, 'x', sizeof(text) - 1u);
        text[sizeof(text) - 1u] = '\0';
        event = checked_json(json_pack("{s:s,s:b,s:s,s:b,s:s,s:b,s:s,s:s,s:i,s:s,s:i,s:b}",
            "endpoint", "local", "historical", 0, "kind", "message", "local", 1,
            "nick", "agent", "op", 0, "room", "#lab", "text", text,
            "timestamp_ms", 1000, "stream", "", "sequence", 0, "input", 0));
        oversized = json_deep_copy(event);
        assert(oversized);
        durable_seq = session.next_seq;
        durable_end = session.log_end;
        assert(snag_session_commit(&session, "irc_event", oversized, NULL, error, sizeof(error)) < 0);
        assert(session.next_seq == durable_seq && session.log_end == durable_end);
        text[SNAG_IRC_TEXT_MAX] = '\0';
        assert(snag_json_set_new(event, "text", json_string(text)) == 0);
        struct snag_irc_event decoded;
        static const char *const kinds[] = {
            "connected", "disconnected", "join", "part", "quit", "nick",
            "message", "notice", "topic", "mode", "history_ready" };
        for (size_t i = 0u; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
            assert(snag_json_set_new(event, "kind", json_string(kinds[i])) == 0);
            assert(snag_irc_event_read(event, &decoded) == 0);
            assert(decoded.kind == (enum snag_irc_event_kind)i);
            assert(decoded.timestamp_ms == 1000u && decoded.local && !decoded.op && !decoded.historical);
            assert(!strcmp(decoded.endpoint, "local") && !strcmp(decoded.room, "#lab") &&
                   !strcmp(decoded.nick, "agent") && !strcmp(decoded.text, text));
            json_t *encoded = snag_irc_event_data(&decoded);
            assert(encoded && json_equal(event, encoded));
            json_decref(encoded);
        }
        static const char *const invalid[] = {
            "{\"kind\":\"unknown\"}", "{\"endpoint\":\"\"}", "{\"room\":\"bad\\nroom\"}", "{\"nick\":null}",
            "{\"historical\":1}", "{\"local\":null}", "{\"op\":\"true\"}",
            "{\"timestamp_ms\":0}", "{\"timestamp_ms\":-1}", "{\"extra\":true}" };
        for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
            json_t *bad = json_deep_copy(event);
            json_t *change = json_loadb(invalid[i], strlen(invalid[i]), 0u, NULL);
            assert(bad && change);
            void *field = json_object_iter(change);
            assert(json_object_set(bad, json_object_iter_key(field), json_object_iter_value(field)) == 0);
            assert(snag_irc_event_read(bad, &decoded) < 0);
            json_decref(change);
            json_decref(bad);
        }
        assert(snag_json_set_new(event, "kind", json_string("message")) == 0);
        commit_event(&session, "irc_event", event);
        snag_session_close(&session);
        snag_session_init(&session);
        assert(snag_session_open(&store, &session, id, error, sizeof(error)) == 0);
        assert(session.next_seq == durable_seq + 1u);
    }

    {
        const char *goal1 = "11111111111111111111111111111111";
        const char *goal2 = "22222222222222222222222222222222";
        const char *goal3 = "33333333333333333333333333333333";
        const char *goal4 = "44444444444444444444444444444444";

        commit_event(&session, "goal_started", goal_started_data(goal1, "finish the release"));
        assert(session.goal_status == SNAG_GOAL_ACTIVE);
        assert(strcmp(session.goal_prompt, "finish the release") == 0);
        assert(session.goal_revision == 1u);
        assert(!session.goal_locked);
        commit_event(&session, "goal_lock_changed", goal_lock_data(goal1, true));
        durable_end = session.log_end;
        durable_seq = session.next_seq;
        assert(snag_session_commit(&session, "goal_reworded",
            goal_reworded_data(goal1, "model", "model rewrite"), NULL, error, sizeof(error)) < 0);
        assert(session.log_end == durable_end);
        assert(session.next_seq == durable_seq);
        assert(strcmp(session.goal_prompt, "finish the release") == 0);
        /* Locked also means the model may not block it, which it could do before this
         * rule existed. Finishing it stays allowed, so the goal can still complete. */
        assert(snag_session_commit(&session, "goal_blocked",
            goal_reason_data(goal1, "model", "reason", "dependency unavailable"), NULL, error, sizeof(error)) < 0);
        assert(session.goal_status == SNAG_GOAL_ACTIVE && session.goal_blocker == NULL);
        commit_event(&session, "goal_reworded",
                     goal_reworded_data(goal1, "user", "finish and publish the release"));
        assert(session.goal_revision == 2u);
        commit_event(&session, "goal_lock_changed", goal_lock_data(goal1, false));
        assert(!session.goal_locked);
        /* Unlocked again, so the model's own block and completion below are allowed. */
        commit_event(&session, "goal_paused", goal_reason_data(goal1, NULL, "reason", "user"));
        assert(session.goal_status == SNAG_GOAL_PAUSED);
        assert(snag_session_commit(&session, "goal_completed", goal_actor_data(goal1, "model"), NULL,
            error, sizeof(error)) < 0);
        assert(snag_session_commit(&session, "goal_resumed",
            goal_reason_data(goal1, NULL, "unused", "bad"), NULL, error, sizeof(error)) < 0);
        commit_event(&session, "goal_resumed", checked_json(json_pack("{s:s}", "goal_id", goal1)));
        commit_event(&session, "goal_blocked", goal_reason_data(goal1, "model", "reason",
                         "dependency unavailable"));
        assert(session.goal_status == SNAG_GOAL_BLOCKED);
        assert(strcmp(session.goal_blocker, "dependency unavailable") == 0);
        commit_event(&session, "goal_resumed", checked_json(json_pack("{s:s}", "goal_id", goal1)));
        assert(session.goal_blocker == NULL);
        commit_event(&session, "goal_completed", goal_actor_data(goal1, "model"));
        assert(session.goal_status == SNAG_GOAL_COMPLETED);
        assert(snag_session_commit(&session, "goal_started", goal_started_data(goal1, "duplicate id"), NULL,
            error, sizeof(error)) < 0);
        commit_event(&session, "goal_started", goal_started_data(goal2, "next goal"));
        commit_event(&session, "goal_cancelled", checked_json(json_pack("{s:s}", "goal_id", goal2)));
        commit_event(&session, "goal_started", goal_started_data(goal3, "copy-on-write goal"));
        session.goal_turn_count = 9u; /* replacement must establish a fresh identity counter */
        commit_event(&session, "goal_replaced",
                     goal_replaced_data(goal3, goal4, "user", "replacement goal"));
        assert(strcmp(session.goal_parent_id, goal3) == 0);
        assert(strcmp(session.goal_id, goal4) == 0);
        assert(strcmp(session.goal_prompt, "replacement goal") == 0);
        assert(session.goal_status == SNAG_GOAL_ACTIVE);
        assert(session.goal_revision == 1u);
        assert(session.goal_turn_count == 0u);
        commit_event(&session, "goal_completed", goal_actor_data(goal4, "user"));
        snag_session_close(&session);
        snag_session_init(&session);
        assert(snag_session_open(&store, &session, id, error, sizeof(error)) == 0);
        assert(session.goal_status == SNAG_GOAL_COMPLETED);
        assert(strcmp(session.goal_id, goal4) == 0);
        assert(strcmp(session.goal_parent_id, goal3) == 0);
        assert(strcmp(session.goal_prompt, "replacement goal") == 0);
        assert(session.goal_turn_count == 0u);
    }

    {
        /* The lock governs what this build accepts, not what an older build already wrote: a model
         * block recorded before the lock rule existed must still replay, while the same event
         * committed now stays refused. Self-contained, in its own store, so the rest of the suite
         * sees no extra session. */
        const char *locked_goal = "33333333333333333333333333333333";
        char legacy_state[4096], legacy_work[4096], legacy_id[SNAG_ID_HEX_LEN + 1u], legacy_error[256];
        struct snag_store legacy_store;
        struct snag_session legacy_session;
        struct snag_buf legacy_line = {.max = 1024u * 1024u};
        char digest[SNAG_SHA256_HEX_LEN + 1u], journal[4096];
        json_t *legacy_data, *legacy_event;
        FILE *file;

        assert(snprintf(legacy_state, sizeof(legacy_state), "%s/legacy-state", temp) > 0);
        assert(mkdir(legacy_state, 0700) == 0);
        assert(snprintf(legacy_work, sizeof(legacy_work), "%s/legacy-work", temp) > 0);
        assert(mkdir(legacy_work, 0700) == 0);
        snag_store_init(&legacy_store);
        snag_session_init(&legacy_session);
        assert(snag_store_open(&legacy_store, legacy_state, legacy_error, sizeof(legacy_error)) == 0);
        assert(snag_session_create(&legacy_store, &legacy_session, legacy_work, "default", "model",
                                   "high", legacy_error, sizeof(legacy_error)) == 0);
        assert(snag_strcpy(legacy_id, sizeof(legacy_id), legacy_session.id));
        commit_event(&legacy_session, "goal_started", goal_started_data(locked_goal, "legacy replay"));
        commit_event(&legacy_session, "goal_lock_changed", goal_lock_data(locked_goal, true));
        assert(snag_session_commit(&legacy_session, "goal_blocked",
                   goal_reason_data(locked_goal, "model", "reason", "refused while live"),
                   NULL, legacy_error, sizeof(legacy_error)) < 0);

        /* Write the block the way a build without the rule would have written it. */
        legacy_data = goal_reason_data(locked_goal, "model", "reason", "written before the rule");
        legacy_event = json_pack("{s:O,s:s,s:I,s:s,s:I,s:s,s:i}", "data", legacy_data,
            "prev_sha256", legacy_session.prev_sha256, "seq", (json_int_t)legacy_session.next_seq,
            "session_id", legacy_session.id, "time_ms", (json_int_t)legacy_session.last_time_ms,
            "type", "goal_blocked", "v", 1);
        json_decref(legacy_data);
        assert(legacy_event && snag_json_digest(legacy_event, digest) == 0);
        assert(json_object_set_new(legacy_event, "event_sha256", json_string(digest)) == 0);
        assert(snag_json_canonical(legacy_event, &legacy_line) == 0);
        assert(snag_buf_putc(&legacy_line, '\n') == 0);
        assert(snprintf(journal, sizeof(journal), "%s/sessions/%s/events.jsonl", legacy_state,
                        legacy_id) > 0);
        file = fopen(journal, "ab");
        assert(file && fwrite(legacy_line.data, 1u, legacy_line.len, file) == legacy_line.len);
        assert(fclose(file) == 0);
        json_decref(legacy_event);
        snag_buf_free(&legacy_line);

        /* Reopening is the replay that used to fail. */
        snag_session_close(&legacy_session);
        snag_session_init(&legacy_session);
        assert(snag_session_open(&legacy_store, &legacy_session, legacy_id,
                                 legacy_error, sizeof(legacy_error)) == 0);
        assert(legacy_session.goal_locked && legacy_session.goal_status == SNAG_GOAL_BLOCKED);
        assert(legacy_session.goal_blocker &&
               strcmp(legacy_session.goal_blocker, "written before the rule") == 0);
        snag_session_close(&legacy_session);
        snag_store_close(&legacy_store);
    }

    assert(snag_session_archive(&session, NULL, error, sizeof(error)) == 0);
    assert(session.archived);
    assert(snprintf(list_path, sizeof(list_path), "%s/list", temp) > 0);
    {
        int fd = open(list_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        assert(fd >= 0);
        assert(snag_store_list(&store, workspace2, false, false, list_to_fd, &fd, error, sizeof(error)) == 0);
        assert(close(fd) == 0);
        assert(read_file(list_path, list_buf, sizeof(list_buf)) == 0u);
    }
    {
        int fd = open(list_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        assert(fd >= 0);
        assert(snag_store_list(&store, workspace2, false, true, list_to_fd, &fd, error, sizeof(error)) == 0);
        assert(close(fd) == 0);
        assert(read_file(list_path, list_buf, sizeof(list_buf)) > 0u);
        assert(strstr(list_buf, "\tarchived\t") != NULL);
    }
    assert(snag_session_unarchive(&session, NULL, error, sizeof(error)) == 0);
    assert(!session.archived);
    assert(snag_session_delete(&store, &session, id_prefix, NULL, error, sizeof(error)) == 0);
    snag_session_close(&session);
    snag_session_init(&session);
    assert(snag_session_open(&store, &session, id, error, sizeof(error)) < 0);
    snag_session_close(&session);

    {
        static const char compact_id[] = "55555555555555555555555555555555";

        snag_session_init(&session);
        assert(snag_session_create(&store, &session, workspace, "default", "gpt-5.5-2026-04-23", "default",
                                  error, sizeof(error)) == 0);
        memcpy(id, session.id, sizeof(id));
        static const char hash[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        commit_event(&session, "compaction_started",
            checked_json(json_pack("{s:s,s:s,s:s,s:s,s:I,s:s,s:n,s:s,s:s,s:s,s:I,s:s}",
                "capability_version", SNAJPAGENT_CAPABILITY_VERSION, "compact_id", compact_id,
                "count_method", "qualified_upper_bound", "count_request_sha256", hash,
                "input_tokens_bound", (json_int_t)(1), "model", session.default_model,
                "predecessor_compact_id", "profile_id", SNAJPAGENT_PROFILE_ID, "reason", "manual",
                "request_sha256", hash, "source_seq", (json_int_t)(1), "source_sha256", hash)));
        assert(strcmp(session.active_compact_id, compact_id) == 0);
        durable_end = session.log_end;
        durable_seq = session.next_seq;
        assert(snag_session_commit(&session, "compaction_interrupted", compaction_interrupted_data(compact_id,
                                                              "invalid"), NULL, error, sizeof(error)) < 0);
        assert(session.log_end == durable_end);
        assert(session.next_seq == durable_seq);
        assert(strcmp(session.active_compact_id, compact_id) == 0);
        commit_event(&session, "compaction_interrupted", compaction_interrupted_data(compact_id, "steering"));
        assert(session.active_compact_id[0] == '\0');
        assert(session.active_compact_source_sha256[0] == '\0');
        assert(session.active_compact_source_seq == 0u);
        snag_session_close(&session);

        snag_session_init(&session);
        assert(snag_session_open(&store, &session, id, error, sizeof(error)) == 0);
        assert(session.active_compact_id[0] == '\0');
        assert(session.active_compact_source_sha256[0] == '\0');
        assert(session.active_compact_source_seq == 0u);
        snag_session_close(&session);
    }

    for (unsigned int cut = 0u; cut < 4u; ++cut) {
        snag_session_init(&session);
        assert(snag_session_create(&store, &session, workspace, "default", "gpt-5.5-2026-04-23", "default",
                                  error, sizeof(error)) == 0);
        memcpy(id, session.id, sizeof(id));
        memcpy(id_prefix, session.id, 8u);
        id_prefix[8] = '\0';
        assert(snprintf(trash_name, sizeof(trash_name), "%s.%032x",
                        session.id, 1u) == (int)(sizeof(trash_name) - 1u));
        commit_event(&session, "session_delete_requested", checked_json(json_pack("{s:s,s:s}",
            "confirmed_id_prefix", id_prefix, "trash_name", trash_name)));
        assert(renameat(store.sessions_fd, id, store.trash_fd, trash_name) == 0);
        if (cut) assert(unlinkat(session.dir_fd, "events.jsonl", 0) == 0);
        if (cut > 1u) assert(unlinkat(session.dir_fd, "lock", 0) == 0);
        if (cut == 3u) {
            int extra = openat(session.dir_fd, "unexpected", O_CREAT | O_WRONLY, 0600);
            assert(extra >= 0 && close(extra) == 0);
            struct snag_session probe;
            snag_session_init(&probe);
            assert(snag_session_open(&store, &probe, id_prefix, error, sizeof(error)) < 0);
            snag_session_close(&probe);
            assert(unlinkat(session.dir_fd, "unexpected", 0) == 0);
        }
        snag_session_close(&session);
        snag_session_init(&session);
        assert(snag_session_open(&store, &session, id_prefix, error, sizeof(error)) == 1);
        assert(openat(store.trash_fd, trash_name, O_RDONLY | O_DIRECTORY) < 0);
        assert(errno == ENOENT);
        snag_session_close(&session);
    }
    snag_session_init(&session);
    assert(snag_session_create(&store, &session, workspace,
                              "default", "gpt-5.5-2026-04-23", "default",
                              error, sizeof(error)) == 0);
    memcpy(id, session.id, sizeof(id));
    memcpy(id_prefix, session.id, 8u);
    id_prefix[8] = '\0';
    assert(snprintf(trash_name, sizeof(trash_name), "%s.%032x",
                    session.id, 1u) == (int)(sizeof(trash_name) - 1u));
    commit_event(&session, "session_delete_requested", checked_json(json_pack("{s:s,s:s}",
        "confirmed_id_prefix", id_prefix, "trash_name", trash_name)));
    crashed_work=snag_media_work_open(session.dir_fd,error,sizeof(error));
    assert(crashed_work>=0);
    crashed_file=snag_create_private_at(crashed_work,"delete-residue",true);
    assert(crashed_file>=0 && snag_write_full(crashed_file,"private",7u)==0);
    close(crashed_file);close(crashed_work);
    assert(renameat(store.sessions_fd, id, store.trash_fd, trash_name) == 0);
    snag_session_close(&session);
    snag_session_init(&session);
    assert(snag_session_open(&store, &session, id_prefix,
                            error, sizeof(error)) == 1);
    assert(openat(store.trash_fd, trash_name, O_RDONLY | O_DIRECTORY) < 0);
    assert(errno == ENOENT);
    snag_session_close(&session);

    {
        static const char turn_id[] = "11111111111111111111111111111111";
        static const char first_id[] = "22222222222222222222222222222222";
        static const char second_id[] = "33333333333333333333333333333333";
        static const char missing_id[] = "44444444444444444444444444444444";
        uint64_t first_seq;
        uint64_t second_seq;

        snag_session_init(&session);
        assert(snag_session_create(&store, &session, workspace, "default", "gpt-5.5-2026-04-23", "default",
                                  error, sizeof(error)) == 0);
        memcpy(id, session.id, sizeof(id));
        commit_event(&session, "turn_started", turn_started_data(&session, turn_id));
        json_t *enqueued = queued_data(turn_id, first_id, "first");
        commit_event(&session, "future_turn_queued", json_incref(enqueued));
        const char *first_text = session.pending_queue[0].text;
        assert(json_string_set(json_object_get(enqueued, "text"), "caller mutation") == 0);
        assert(!strcmp(first_text, "first"));
        json_decref(enqueued);
        commit_event(&session, "future_turn_queued", queued_data(turn_id, second_id, "second"));
        assert(session.pending_queue[0].text == first_text);
        first_seq = session.pending_queue[0].seq;
        second_seq = session.pending_queue[1].seq;
        durable_end = session.log_end;
        durable_seq = session.next_seq;
        assert(snag_session_commit(&session, "future_turn_edited", edited_data(missing_id, "missing"), NULL,
                                  error, sizeof(error)) < 0);
        assert(session.log_end == durable_end);
        assert(session.next_seq == durable_seq);
        assert(snag_session_commit(&session, "future_turn_edited", edited_data(first_id, "first"), NULL,
                                  error, sizeof(error)) < 0);
        assert(session.log_end == durable_end);
        assert(session.next_seq == durable_seq);
        commit_event(&session, "future_turn_edited", edited_data(second_id, "second edited"));
        int writable = session.log_fd;
        session.log_fd = openat(session.dir_fd, "events.jsonl", O_RDONLY);
        assert(session.log_fd >= 0);
        struct snag_session failed = session;
        assert(snag_session_commit(&session, "future_turn_edited",
            edited_data(first_id, "not adopted"), NULL, error, sizeof(error)) < 0);
        ++failed.write_failures;
        assert(memcmp(&session, &failed, sizeof(session)) == 0);
        assert(!strcmp(first_text, "first"));
        assert(close(session.log_fd) == 0);
        session.log_fd = writable;
        assert(session.pending_queue_count == 2u);
        assert(strcmp(session.pending_queue[0].text, "first") == 0);
        assert(strcmp(session.pending_queue[1].text, "second edited") == 0);
        assert(session.pending_queue[0].seq == first_seq);
        assert(session.pending_queue[1].seq == second_seq);
        assert(session.pending_queue_bytes == strlen("first") + strlen("second edited"));
        struct snag_session before = session;
        assert(snag_session_each_event(&session, check_replay, &session, error, sizeof(error)) == 0);
        assert(memcmp(&session, &before, sizeof(session)) == 0);
        ++session.log_end;
        ++session.next_seq;
        before = session;
        assert(snag_session_each_event(&session, check_replay, &session, error, sizeof(error)) < 0);
        assert(memcmp(&session, &before, sizeof(session)) == 0);
        --session.log_end;
        --session.next_seq;
        snag_session_close(&session);

        snag_session_init(&session);
        assert(snag_session_open(&store, &session, id, error, sizeof(error)) == 0);
        assert(session.pending_queue_count == 2u);
        assert(strcmp(session.pending_queue[0].queue_id, first_id) == 0);
        assert(strcmp(session.pending_queue[0].text, "first") == 0);
        assert(strcmp(session.pending_queue[1].queue_id, second_id) == 0);
        assert(strcmp(session.pending_queue[1].text, "second edited") == 0);
        assert(session.pending_queue[0].seq == first_seq);
        assert(session.pending_queue[1].seq == second_seq);
        assert(session.pending_queue_bytes == strlen("first") + strlen("second edited"));
        const char *invalid_cancellations[][2] = {
            {first_id, first_id}, {second_id, first_id}, {first_id, missing_id},
            {first_id, "bad"}, {first_id, NULL}
        };
        for (size_t i = 0u; i < sizeof(invalid_cancellations) / sizeof(invalid_cancellations[0]); ++i) {
            struct snag_session before_cancel = session;
            assert(snag_session_commit(&session, "future_turn_cancelled",
                checked_json(json_pack("{s:[s,s?],s:s}", "queue_ids",
                    invalid_cancellations[i][0], invalid_cancellations[i][1], "reason", "user")),
                NULL, error, sizeof(error)) < 0);
            assert(memcmp(&session, &before_cancel, sizeof(session)) == 0);
            assert(!strcmp(session.pending_queue[0].text, "first"));
            assert(!strcmp(session.pending_queue[1].text, "second edited"));
        }
        commit_event(&session, "future_turn_cancelled",
            checked_json(json_pack("{s:[s],s:s}", "queue_ids", first_id, "reason", "user")));
        assert(session.pending_queue_count == 1u && session.pending_queue_bytes == strlen("second edited"));
        assert(!strcmp(session.pending_queue[0].queue_id, second_id));
        assert(session.pending_queue[0].seq == second_seq && !session.pending_queue[1].text);
        assert(!json_object_get(session.strings, first_id));
        snag_session_close(&session);
        snag_session_init(&session);
        assert(snag_session_open(&store, &session, id, error, sizeof(error)) == 0);
        assert(session.pending_queue_count == 1u && session.pending_queue_bytes == strlen("second edited"));
        assert(!strcmp(session.pending_queue[0].text, "second edited"));
        snag_session_close(&session);
    }
    test_refusal_diagnostic(workspace);
    test_audio_usage(&store,workspace);
    test_voice_queue(&store,workspace);
    test_banner_steering(&store,workspace);

    test_many_queued_turns(&store,workspace);
    snag_store_close(&store);
    free(temp);
    puts("test_store: ok");
    return 0;
}
