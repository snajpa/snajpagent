/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_STORE_H
#define SNAJPAGENT_STORE_H

#include "json.h"
#include "turn.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#define SNJ_MODEL_MAX_BYTES 256u
#define SNJ_MAX_STEERING_TEXT (256u * 1024u)
#define SNJ_MAX_STEERING_PER_TURN 32u
#define SNJ_MAX_QUEUED_TEXT (256u * 1024u)
#define SNJ_MAX_PENDING_TURNS 128u
#define SNJ_MAX_PENDING_QUEUE_TEXT (16u * 1024u * 1024u)

enum snj_response_terminal {
    SNJ_RESPONSE_TERMINAL_NONE,
    SNJ_RESPONSE_TERMINAL_STEERED,
    SNJ_RESPONSE_TERMINAL_INTERRUPTED,
    SNJ_RESPONSE_TERMINAL_FAILED
};

struct snj_pending_call {
    char call_id[SNJ_ID_HEX_LEN + 1u];
    char action_sha256[SNJ_SHA256_HEX_LEN + 1u];
    char tool_name[16];
    char process_handle[SNJ_ID_HEX_LEN + 1u];
    bool started;
    bool finished;
};

struct snj_pending_steering {
    char steering_id[SNJ_ID_HEX_LEN + 1u];
    uint64_t seq;
    char *text;
};

struct snj_queued_turn {
    char queue_id[SNJ_ID_HEX_LEN + 1u];
    uint64_t seq;
    char *text;
};

struct snj_store {
    char *root_path;
    int root_fd;
    int sessions_fd;
    int trash_fd;
};

struct snj_session {
    char id[SNJ_ID_HEX_LEN + 1u];
    char prev_sha256[SNJ_SHA256_HEX_LEN + 1u];
    char active_turn_id[SNJ_ID_HEX_LEN + 1u];
    char active_response_id[SNJ_ID_HEX_LEN + 1u];
    char final_item_id[SNJ_ID_HEX_LEN + 1u];
    char final_response_id[SNJ_ID_HEX_LEN + 1u];
    char active_process_handle[SNJ_ID_HEX_LEN + 1u];
    char compact_id[SNJ_ID_HEX_LEN + 1u];
    char active_compact_id[SNJ_ID_HEX_LEN + 1u];
    char active_compact_source_sha256[SNJ_SHA256_HEX_LEN + 1u];
    char default_model[SNJ_MODEL_MAX_BYTES];
    char active_turn_model[SNJ_MODEL_MAX_BYTES];
    char default_effort[16];
    char *workspace;
    char trash_name[SNJ_ID_HEX_LEN + 1u + SNJ_ID_HEX_LEN + 1u];
    char *dir_path;
    char *first_user;
    char *last_user;
    char *last_assistant;
    json_t *compact_output;
    int dir_fd;
    int log_fd;
    int lock_fd;
    off_t log_end;
    uint64_t next_seq;
    uint64_t turn_count;
    uint64_t last_time_ms;
    uint64_t tool_invocations;
    uint64_t compact_seq;
    uint64_t active_compact_source_seq;
    size_t pending_steering_bytes;
    size_t pending_queue_bytes;
    unsigned int active_cycle;
    enum snj_graph_outcome response_outcome;
    struct snj_pending_call pending_calls[SNJ_MAX_CALLS_PER_RESPONSE];
    struct snj_pending_steering pending_steering[SNJ_MAX_STEERING_PER_TURN];
    struct snj_queued_turn pending_queue[SNJ_MAX_PENDING_TURNS];
    size_t pending_call_count;
    size_t pending_steering_count;
    size_t pending_queue_count;
    bool active_turn;
    bool archived;
    bool delete_requested;
    bool response_open;
    bool response_complete;
    enum snj_response_terminal response_terminal;
};

void snj_store_init(struct snj_store *store);
void snj_store_close(struct snj_store *store);
int snj_store_open(struct snj_store *store, char *error, size_t error_size);

void snj_session_init(struct snj_session *session);
void snj_session_close(struct snj_session *session);
int snj_session_create(struct snj_store *store, struct snj_session *session,
                       const char *workspace, const char *model,
                       const char *effort, char *error, size_t error_size);
int snj_session_open(struct snj_store *store, struct snj_session *session,
                     const char *prefix, char *error, size_t error_size);
int snj_session_open_last(struct snj_store *store, struct snj_session *session,
                          const char *workspace, bool all,
                          char *error, size_t error_size);
int snj_store_list(struct snj_store *store, const char *workspace, bool all,
                   int fd, char *error, size_t error_size);
int snj_store_list_active(struct snj_store *store, const char *workspace,
                          bool all, int fd, char *error, size_t error_size);
int snj_session_archive(struct snj_session *session, uint64_t *written_seq,
                        char *error, size_t error_size);
int snj_session_unarchive(struct snj_session *session, uint64_t *written_seq,
                          char *error, size_t error_size);
int snj_session_delete(struct snj_store *store, struct snj_session *session,
                       const char *confirmed_prefix, uint64_t *written_seq,
                       char *error, size_t error_size);
int snj_session_complete_delete(struct snj_store *store,
                                struct snj_session *session,
                                char *error, size_t error_size);

typedef int (*snj_session_event_fn)(void *opaque, uint64_t seq,
                                    const char *type, const json_t *data,
                                    char *error, size_t error_size);
int snj_session_each_event(struct snj_session *session,
                           snj_session_event_fn fn, void *opaque,
                           char *error, size_t error_size);

int snj_session_commit(struct snj_session *session, const char *type,
                       json_t *data, uint64_t *written_seq,
                       char *error, size_t error_size);
int snj_session_append(struct snj_session *session, const char *type,
                       json_t *data, uint64_t *written_seq,
                       char *error, size_t error_size);

#endif
