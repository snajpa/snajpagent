/* SPDX-License-Identifier: GPL-2.0-only */
#include "store.h"
#include "instructions.h"
#include "irc.h"
#include "snajpagent.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int
list_to_fd(void *opaque, const char *text, size_t len)
{
    return snj_write_full(*(int *)opaque, text, len);
}

static json_t *
change_data(const char *old_key, const char *old_value,
            const char *new_key, const char *new_value)
{
    json_t *data = json_object();
    assert(data);
    assert(snj_json_set_new(data, old_key, json_string(old_value)) == 0);
    assert(snj_json_set_new(data, new_key, json_string(new_value)) == 0);
    return data;
}

static json_t *
delete_data(const char *prefix, const char *trash_name)
{
    json_t *data = json_object();
    assert(data);
    assert(snj_json_set_new(data, "confirmed_id_prefix",
                            json_string(prefix)) == 0);
    assert(snj_json_set_new(data, "trash_name", json_string(trash_name)) == 0);
    return data;
}

static json_t *
turn_started_data(const struct snj_session *session, const char *turn_id)
{
    struct snj_instruction_set instructions;
    json_t *config = json_object();
    json_t *data = json_object();
    json_t *metadata;

    snj_instructions_init(&instructions);
    metadata = snj_instructions_metadata_json(&instructions);
    snj_instructions_free(&instructions);
    assert(config && data && metadata);
    assert(snj_json_set_new(config, "capability_version",
                            json_string(SNAJPAGENT_CAPABILITY_VERSION)) == 0);
    assert(snj_json_set_new(config, "effort",
                            json_string(session->default_effort)) == 0);
    assert(snj_json_set_new(config, "max_output_tokens", json_null()) == 0);
    assert(snj_json_set_new(config, "model",
                            json_string(session->default_model)) == 0);
    assert(snj_json_set_new(config, "provider",
                            json_string(session->default_provider)) == 0);
    assert(snj_json_set_new(config, "profile_id",
                            json_string(SNAJPAGENT_PROFILE_ID)) == 0);
    assert(snj_json_set_new(config, "prompt_schema", json_integer(1)) == 0);
    assert(snj_json_set_new(config, "replay_schema", json_integer(1)) == 0);
    assert(snj_json_set_new(config, "tool_schema", json_integer(1)) == 0);
    assert(snj_json_set_new(data, "config", config) == 0);
    assert(snj_json_set_new(data, "input_kind", json_string("direct")) == 0);
    assert(snj_json_set_new(data, "read_only", json_false()) == 0);
    assert(snj_json_set_new(data, "instructions", metadata) == 0);
    assert(snj_json_set_new(data, "queue_id", json_null()) == 0);
    assert(snj_json_set_new(data, "queue_seq", json_null()) == 0);
    assert(snj_json_set_new(data, "text", json_string("queue test")) == 0);
    assert(snj_json_set_new(data, "turn_id", json_string(turn_id)) == 0);
    assert(snj_json_set_new(data, "turn_number", json_integer(1)) == 0);
    assert(snj_json_set_new(data, "workspace",
                            json_string(session->workspace)) == 0);
    return data;
}

static json_t *
queued_data(const char *turn_id, const char *queue_id, const char *text)
{
    json_t *data = json_object();

    assert(data);
    assert(snj_json_set_new(data, "queue_id", json_string(queue_id)) == 0);
    assert(snj_json_set_new(data, "read_only", json_false()) == 0);
    assert(snj_json_set_new(data, "text", json_string(text)) == 0);
    assert(snj_json_set_new(data, "while_turn_id", json_string(turn_id)) == 0);
    return data;
}

static json_t *
edited_data(const char *queue_id, const char *text)
{
    json_t *data = json_object();

    assert(data);
    assert(snj_json_set_new(data, "queue_id", json_string(queue_id)) == 0);
    assert(snj_json_set_new(data, "read_only", json_false()) == 0);
    assert(snj_json_set_new(data, "text", json_string(text)) == 0);
    return data;
}

static json_t *
goal_started_data(const char *goal_id, const char *prompt)
{
    json_t *data = json_object();
    assert(data);
    assert(snj_json_set_new(data, "goal_id", json_string(goal_id)) == 0);
    assert(snj_json_set_new(data, "prompt", json_string(prompt)) == 0);
    return data;
}

static json_t *
goal_actor_data(const char *goal_id, const char *actor)
{
    json_t *data = json_object();
    assert(data);
    assert(snj_json_set_new(data, "actor", json_string(actor)) == 0);
    assert(snj_json_set_new(data, "goal_id", json_string(goal_id)) == 0);
    return data;
}

static json_t *
goal_reworded_data(const char *goal_id, const char *actor,
                   const char *prompt)
{
    json_t *data = goal_actor_data(goal_id, actor);
    assert(snj_json_set_new(data, "prompt", json_string(prompt)) == 0);
    return data;
}

static json_t *
goal_lock_data(const char *goal_id, bool locked)
{
    json_t *data = json_object();
    assert(data);
    assert(snj_json_set_new(data, "goal_id", json_string(goal_id)) == 0);
    assert(snj_json_set_new(data, "locked", json_boolean(locked)) == 0);
    return data;
}

static json_t *
goal_reason_data(const char *goal_id, const char *actor,
                 const char *key, const char *reason)
{
    json_t *data = actor ? goal_actor_data(goal_id, actor) : json_object();
    assert(data);
    if (!actor)
        assert(snj_json_set_new(data, "goal_id", json_string(goal_id)) == 0);
    assert(snj_json_set_new(data, key, json_string(reason)) == 0);
    return data;
}

static json_t *
compaction_started_data(const struct snj_session *session,
                        const char *compact_id)
{
    static const char hash[] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    json_t *data = json_object();

    assert(data);
    assert(snj_json_set_new(data, "capability_version",
                            json_string(SNAJPAGENT_CAPABILITY_VERSION)) == 0);
    assert(snj_json_set_new(data, "compact_id", json_string(compact_id)) == 0);
    assert(snj_json_set_new(data, "count_method",
                            json_string("qualified_upper_bound")) == 0);
    assert(snj_json_set_new(data, "count_request_sha256",
                            json_string(hash)) == 0);
    assert(snj_json_set_new(data, "input_tokens_bound", json_integer(1)) == 0);
    assert(snj_json_set_new(data, "model",
                            json_string(session->default_model)) == 0);
    assert(snj_json_set_new(data, "predecessor_compact_id", json_null()) == 0);
    assert(snj_json_set_new(data, "profile_id",
                            json_string(SNAJPAGENT_PROFILE_ID)) == 0);
    assert(snj_json_set_new(data, "reason", json_string("manual")) == 0);
    assert(snj_json_set_new(data, "request_sha256", json_string(hash)) == 0);
    assert(snj_json_set_new(data, "source_seq", json_integer(1)) == 0);
    assert(snj_json_set_new(data, "source_sha256", json_string(hash)) == 0);
    return data;
}

static json_t *
compaction_interrupted_data(const char *compact_id, const char *reason)
{
    json_t *data = json_object();

    assert(data);
    assert(snj_json_set_new(data, "compact_id", json_string(compact_id)) == 0);
    assert(snj_json_set_new(data, "reason", json_string(reason)) == 0);
    return data;
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

int
main(void)
{
    char temp[] = "/tmp/snajpagent-store-XXXXXX";
    char state[4096];
    char workspace[4096];
    char workspace2[4096];
    char id[SNJ_ID_HEX_LEN + 1u];
    char id_prefix[9];
    char trash_name[SNJ_ID_HEX_LEN + 1u + SNJ_ID_HEX_LEN + 1u];
    char list_path[4096];
    char list_buf[4096];
    char error[256];
    struct snj_store store;
    struct snj_session session;
    off_t durable_end;
    uint64_t durable_seq;
    json_t *bad;

    assert(mkdtemp(temp));
    assert(snprintf(state, sizeof(state), "%s/state", temp) > 0);
    assert(snprintf(workspace, sizeof(workspace), "%s/work", temp) > 0);
    assert(mkdir(state, 0700) == 0);
    assert(mkdir(workspace, 0700) == 0);
    assert(snprintf(workspace2, sizeof(workspace2), "%s/work2", temp) > 0);
    assert(mkdir(workspace2, 0700) == 0);
    snj_store_init(&store);
    snj_session_init(&session);
    assert(snj_store_open(&store, state, error, sizeof(error)) == 0);
    assert(snj_session_create(&store, &session, workspace,
                              "default", "gpt-5.5-2026-04-23", "default",
                              error, sizeof(error)) == 0);
    memcpy(id, session.id, sizeof(id));
    memcpy(id_prefix, session.id, 8u);
    id_prefix[8] = '\0';
    assert(snj_session_commit(&session, "model_changed",
        change_data("old_model", "gpt-5.5-2026-04-23",
                    "new_model", "gpt-5.5-2026-04-23-alt"),
        NULL, error, sizeof(error)) == 0);
    assert(snj_session_commit(&session, "effort_changed",
        change_data("old_effort", "default", "new_effort", "high"),
        NULL, error, sizeof(error)) == 0);
    assert(strcmp(session.default_model, "gpt-5.5-2026-04-23-alt") == 0);
    assert(strcmp(session.default_effort, "high") == 0);
    assert(snj_session_commit(&session, "workspace_changed",
        change_data("old_workspace", workspace,
                    "new_workspace", workspace2),
        NULL, error, sizeof(error)) == 0);
    assert(strcmp(session.workspace, workspace2) == 0);
    durable_end = session.log_end;
    durable_seq = session.next_seq;
    bad = json_object();
    assert(bad);
    assert(snj_json_set_new(bad, "final_item_id",
                            json_string("00000000000000000000000000000000")) == 0);
    assert(snj_json_set_new(bad, "final_response_id",
                            json_string("00000000000000000000000000000000")) == 0);
    assert(snj_json_set_new(bad, "turn_id",
                            json_string("00000000000000000000000000000000")) == 0);
    assert(snj_session_commit(&session, "turn_completed", bad, NULL,
                              error, sizeof(error)) < 0);
    assert(session.log_end == durable_end);
    assert(session.next_seq == durable_seq);
    assert(snj_write_full(session.log_fd, "incomplete", 10u) == 0);
    assert(snj_sync_file(session.log_fd) == 0);
    snj_session_close(&session);

    snj_session_init(&session);
    assert(snj_session_open(&store, &session, id, error, sizeof(error)) == 0);
    assert(session.log_end == durable_end);
    assert(session.next_seq == 5u);
    assert(session.turn_count == 0u);
    assert(strcmp(session.workspace, workspace2) == 0);
    assert(strcmp(session.default_model, "gpt-5.5-2026-04-23-alt") == 0);
    assert(strcmp(session.default_effort, "high") == 0);

    {
        char text[SNJ_IRC_TEXT_MAX + 2u];
        json_t *event;
        json_t *oversized;

        memset(text, 'x', sizeof(text) - 1u);
        text[sizeof(text) - 1u] = '\0';
        event = json_object();
        assert(event);
        assert(snj_json_set_new(event, "endpoint", json_string("local")) == 0);
        assert(snj_json_set_new(event, "historical", json_false()) == 0);
        assert(snj_json_set_new(event, "kind", json_string("message")) == 0);
        assert(snj_json_set_new(event, "local", json_true()) == 0);
        assert(snj_json_set_new(event, "nick", json_string("agent")) == 0);
        assert(snj_json_set_new(event, "op", json_false()) == 0);
        assert(snj_json_set_new(event, "room", json_string("#lab")) == 0);
        assert(snj_json_set_new(event, "text", json_string(text)) == 0);
        assert(snj_json_set_new(event, "timestamp_ms", json_integer(1000)) == 0);
        oversized = json_deep_copy(event);
        assert(oversized);
        durable_seq = session.next_seq;
        durable_end = session.log_end;
        assert(snj_session_commit(&session, "irc_event", oversized, NULL,
                                  error, sizeof(error)) < 0);
        assert(session.next_seq == durable_seq && session.log_end == durable_end);
        text[SNJ_IRC_TEXT_MAX] = '\0';
        assert(snj_json_set_new(event, "text", json_string(text)) == 0);
        assert(snj_session_commit(&session, "irc_event", event, NULL,
                                  error, sizeof(error)) == 0);
        snj_session_close(&session);
        snj_session_init(&session);
        assert(snj_session_open(&store, &session, id, error, sizeof(error)) == 0);
        assert(session.next_seq == durable_seq + 1u);
    }

    {
        const char *goal1 = "11111111111111111111111111111111";
        const char *goal2 = "22222222222222222222222222222222";

        assert(snj_session_commit(&session, "goal_started",
            goal_started_data(goal1, "finish the release"), NULL,
            error, sizeof(error)) == 0);
        assert(session.goal_status == SNJ_GOAL_ACTIVE);
        assert(strcmp(session.goal_prompt, "finish the release") == 0);
        assert(session.goal_revision == 1u);
        assert(!session.goal_locked);
        assert(snj_session_commit(&session, "goal_lock_changed",
            goal_lock_data(goal1, true), NULL, error, sizeof(error)) == 0);
        durable_end = session.log_end;
        durable_seq = session.next_seq;
        assert(snj_session_commit(&session, "goal_reworded",
            goal_reworded_data(goal1, "model", "model rewrite"), NULL,
            error, sizeof(error)) < 0);
        assert(session.log_end == durable_end);
        assert(session.next_seq == durable_seq);
        assert(strcmp(session.goal_prompt, "finish the release") == 0);
        assert(snj_session_commit(&session, "goal_reworded",
            goal_reworded_data(goal1, "user", "finish and publish the release"),
            NULL, error, sizeof(error)) == 0);
        assert(session.goal_revision == 2u);
        assert(snj_session_commit(&session, "goal_lock_changed",
            goal_lock_data(goal1, false), NULL, error, sizeof(error)) == 0);
        assert(!session.goal_locked);
        assert(snj_session_commit(&session, "goal_lock_changed",
            goal_lock_data(goal1, true), NULL, error, sizeof(error)) == 0);
        assert(snj_session_commit(&session, "goal_paused",
            goal_reason_data(goal1, NULL, "reason", "user"), NULL,
            error, sizeof(error)) == 0);
        assert(session.goal_status == SNJ_GOAL_PAUSED);
        assert(snj_session_commit(&session, "goal_completed",
            goal_actor_data(goal1, "model"), NULL,
            error, sizeof(error)) < 0);
        assert(snj_session_commit(&session, "goal_resumed",
            goal_reason_data(goal1, NULL, "unused", "bad"), NULL,
            error, sizeof(error)) < 0);
        {
            json_t *resume = json_object();
            assert(resume);
            assert(snj_json_set_new(resume, "goal_id",
                                    json_string(goal1)) == 0);
            assert(snj_session_commit(&session, "goal_resumed", resume, NULL,
                                      error, sizeof(error)) == 0);
        }
        assert(snj_session_commit(&session, "goal_blocked",
            goal_reason_data(goal1, "model", "reason",
                             "dependency unavailable"), NULL,
            error, sizeof(error)) == 0);
        assert(session.goal_status == SNJ_GOAL_BLOCKED);
        assert(strcmp(session.goal_blocker, "dependency unavailable") == 0);
        {
            json_t *resume = json_object();
            assert(resume);
            assert(snj_json_set_new(resume, "goal_id",
                                    json_string(goal1)) == 0);
            assert(snj_session_commit(&session, "goal_resumed", resume, NULL,
                                      error, sizeof(error)) == 0);
        }
        assert(session.goal_blocker == NULL);
        assert(snj_session_commit(&session, "goal_completed",
            goal_actor_data(goal1, "model"), NULL,
            error, sizeof(error)) == 0);
        assert(session.goal_status == SNJ_GOAL_COMPLETED);
        assert(snj_session_commit(&session, "goal_started",
            goal_started_data(goal1, "duplicate id"), NULL,
            error, sizeof(error)) < 0);
        assert(snj_session_commit(&session, "goal_started",
            goal_started_data(goal2, "next goal"), NULL,
            error, sizeof(error)) == 0);
        {
            json_t *cancel = json_object();
            assert(cancel);
            assert(snj_json_set_new(cancel, "goal_id",
                                    json_string(goal2)) == 0);
            assert(snj_session_commit(&session, "goal_cancelled", cancel, NULL,
                                      error, sizeof(error)) == 0);
        }
        snj_session_close(&session);
        snj_session_init(&session);
        assert(snj_session_open(&store, &session, id,
                                error, sizeof(error)) == 0);
        assert(session.goal_status == SNJ_GOAL_CANCELLED);
        assert(strcmp(session.goal_id, goal2) == 0);
        assert(strcmp(session.goal_prompt, "next goal") == 0);
        assert(session.goal_turn_count == 0u);
    }

    assert(snj_session_archive(&session, NULL, error, sizeof(error)) == 0);
    assert(session.archived);
    assert(snprintf(list_path, sizeof(list_path), "%s/list", temp) > 0);
    {
        int fd = open(list_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        assert(fd >= 0);
        assert(snj_store_list(&store, workspace2, false, false, list_to_fd, &fd,
                                     error, sizeof(error)) == 0);
        assert(close(fd) == 0);
        assert(read_file(list_path, list_buf, sizeof(list_buf)) == 0u);
    }
    {
        int fd = open(list_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        assert(fd >= 0);
        assert(snj_store_list(&store, workspace2, false, true, list_to_fd, &fd,
                              error, sizeof(error)) == 0);
        assert(close(fd) == 0);
        assert(read_file(list_path, list_buf, sizeof(list_buf)) > 0u);
        assert(strstr(list_buf, "\tarchived\t") != NULL);
    }
    assert(snj_session_unarchive(&session, NULL, error, sizeof(error)) == 0);
    assert(!session.archived);
    assert(snj_session_delete(&store, &session, id_prefix, NULL,
                              error, sizeof(error)) == 0);
    snj_session_close(&session);
    snj_session_init(&session);
    assert(snj_session_open(&store, &session, id, error, sizeof(error)) < 0);
    snj_session_close(&session);

    {
        static const char compact_id[] =
            "55555555555555555555555555555555";

        snj_session_init(&session);
        assert(snj_session_create(&store, &session, workspace,
                                  "default", "gpt-5.5-2026-04-23", "default",
                                  error, sizeof(error)) == 0);
        memcpy(id, session.id, sizeof(id));
        assert(snj_session_commit(&session, "compaction_started",
                                  compaction_started_data(&session, compact_id),
                                  NULL, error, sizeof(error)) == 0);
        assert(strcmp(session.active_compact_id, compact_id) == 0);
        durable_end = session.log_end;
        durable_seq = session.next_seq;
        assert(snj_session_commit(&session, "compaction_interrupted",
                                  compaction_interrupted_data(compact_id,
                                                              "invalid"),
                                  NULL, error, sizeof(error)) < 0);
        assert(session.log_end == durable_end);
        assert(session.next_seq == durable_seq);
        assert(strcmp(session.active_compact_id, compact_id) == 0);
        assert(snj_session_commit(&session, "compaction_interrupted",
                                  compaction_interrupted_data(compact_id,
                                                              "steering"),
                                  NULL, error, sizeof(error)) == 0);
        assert(session.active_compact_id[0] == '\0');
        assert(session.active_compact_source_sha256[0] == '\0');
        assert(session.active_compact_source_seq == 0u);
        snj_session_close(&session);

        snj_session_init(&session);
        assert(snj_session_open(&store, &session, id,
                                error, sizeof(error)) == 0);
        assert(session.active_compact_id[0] == '\0');
        assert(session.active_compact_source_sha256[0] == '\0');
        assert(session.active_compact_source_seq == 0u);
        snj_session_close(&session);
    }

    snj_session_init(&session);
    assert(snj_session_create(&store, &session, workspace,
                              "default", "gpt-5.5-2026-04-23", "default",
                              error, sizeof(error)) == 0);
    memcpy(id, session.id, sizeof(id));
    memcpy(id_prefix, session.id, 8u);
    id_prefix[8] = '\0';
    assert(snprintf(trash_name, sizeof(trash_name), "%s.%032x",
                    session.id, 1u) == (int)(sizeof(trash_name) - 1u));
    assert(snj_session_commit(&session, "session_delete_requested",
                              delete_data(id_prefix, trash_name), NULL,
                              error, sizeof(error)) == 0);
    assert(renameat(store.sessions_fd, id, store.trash_fd, trash_name) == 0);
    snj_session_close(&session);
    snj_session_init(&session);
    assert(snj_session_open(&store, &session, id_prefix,
                            error, sizeof(error)) == 1);
    assert(openat(store.trash_fd, trash_name, O_RDONLY | O_DIRECTORY) < 0);
    assert(errno == ENOENT);
    snj_session_close(&session);

    {
        static const char turn_id[] = "11111111111111111111111111111111";
        static const char first_id[] = "22222222222222222222222222222222";
        static const char second_id[] = "33333333333333333333333333333333";
        static const char missing_id[] = "44444444444444444444444444444444";
        uint64_t first_seq;
        uint64_t second_seq;

        snj_session_init(&session);
        assert(snj_session_create(&store, &session, workspace,
                                  "default", "gpt-5.5-2026-04-23", "default",
                                  error, sizeof(error)) == 0);
        memcpy(id, session.id, sizeof(id));
        assert(snj_session_commit(&session, "turn_started",
                                  turn_started_data(&session, turn_id), NULL,
                                  error, sizeof(error)) == 0);
        assert(snj_session_commit(&session, "future_turn_queued",
                                  queued_data(turn_id, first_id, "first"), NULL,
                                  error, sizeof(error)) == 0);
        assert(snj_session_commit(&session, "future_turn_queued",
                                  queued_data(turn_id, second_id, "second"), NULL,
                                  error, sizeof(error)) == 0);
        first_seq = session.pending_queue[0].seq;
        second_seq = session.pending_queue[1].seq;
        durable_end = session.log_end;
        durable_seq = session.next_seq;
        assert(snj_session_commit(&session, "future_turn_edited",
                                  edited_data(missing_id, "missing"), NULL,
                                  error, sizeof(error)) < 0);
        assert(session.log_end == durable_end);
        assert(session.next_seq == durable_seq);
        assert(snj_session_commit(&session, "future_turn_edited",
                                  edited_data(first_id, "first"), NULL,
                                  error, sizeof(error)) < 0);
        assert(session.log_end == durable_end);
        assert(session.next_seq == durable_seq);
        assert(snj_session_commit(&session, "future_turn_edited",
                                  edited_data(second_id, "second edited"), NULL,
                                  error, sizeof(error)) == 0);
        assert(session.pending_queue_count == 2u);
        assert(strcmp(session.pending_queue[0].text, "first") == 0);
        assert(strcmp(session.pending_queue[1].text, "second edited") == 0);
        assert(session.pending_queue[0].seq == first_seq);
        assert(session.pending_queue[1].seq == second_seq);
        assert(session.pending_queue_bytes ==
               strlen("first") + strlen("second edited"));
        snj_session_close(&session);

        snj_session_init(&session);
        assert(snj_session_open(&store, &session, id,
                                error, sizeof(error)) == 0);
        assert(session.pending_queue_count == 2u);
        assert(strcmp(session.pending_queue[0].queue_id, first_id) == 0);
        assert(strcmp(session.pending_queue[0].text, "first") == 0);
        assert(strcmp(session.pending_queue[1].queue_id, second_id) == 0);
        assert(strcmp(session.pending_queue[1].text, "second edited") == 0);
        assert(session.pending_queue[0].seq == first_seq);
        assert(session.pending_queue[1].seq == second_seq);
        assert(session.pending_queue_bytes ==
               strlen("first") + strlen("second edited"));
        snj_session_close(&session);
    }
    snj_store_close(&store);
    puts("test_store: ok");
    return 0;
}
