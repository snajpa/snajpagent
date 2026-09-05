/* SPDX-License-Identifier: GPL-2.0-only */
#include "irc_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IRC_MAILBOX 64u
#define IRC_RECORDS (IRC_MAILBOX * (SNJ_CONFIG_IRC_CLIENT_MAX + 1u))

enum irc_record_kind { IRC_EVENT, IRC_TRACE, IRC_VIEW };
enum irc_command { IRC_OPERATOR, IRC_AGENT, IRC_NOTICE,
                   IRC_OPERATOR_TOPIC, IRC_AGENT_TOPIC, IRC_RESTORE };

struct irc_request {
    enum irc_command command;
    const char *text;
    const struct snj_irc_event *event;
    int result, saved_errno;
    char error[256u];
    bool done;
    uint64_t through;
};

struct irc_record {
    enum irc_record_kind kind;
    size_t source;
    struct snj_irc_view view;
    struct snj_irc_event event;
    unsigned int level;
    char direction;
    char trace[SNJ_IRC_LINE_MAX];
};

struct irc_owner {
    struct snj_irc *runtime;
    struct snj_irc_core *core; /* Only this owner accesses it after startup. */
    struct snj_irc_view sent;  /* Owner-private last published view. */
    struct snj_irc_view view;  /* Engine-private admitted view. */
    char endpoint[SNJ_CONFIG_IRC_ENDPOINT_MAX + 1u];
    size_t index;
    pthread_t thread;
    int wake[2];
    struct irc_request *request; /* Protected by the mailbox mutex. */
    size_t queued;
    bool started;
};

struct snj_irc {
    struct irc_owner owners[SNJ_CONFIG_IRC_CLIENT_MAX + 1u];
    size_t owner_count;
    struct snj_irc_core *history; /* Engine-owned replay/admitted history. */
    snj_irc_event_fn event_fn;
    snj_irc_trace_fn trace_fn;
    void *opaque;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    struct irc_record *records[IRC_RECORDS];
    size_t head, count;
    uint64_t published, admitted;
    int wake[2];
    bool stopping, hosting;
    bool identity_changed; /* Mailbox-locked; retained across command drains. */
    int failure;
};

static void
wake_fd(int fd)
{
    char byte = 0;
    while (write(fd, &byte, 1u) < 0 && errno == EINTR)
        ;
}

static int
open_wake(int fds[2])
{
    if (pipe(fds) < 0)
        return -1;
    for (size_t i = 0u; i < 2u; ++i)
        if (snj_fd_cloexec(fds[i]) < 0 ||
            fcntl(fds[i], F_SETFL, O_NONBLOCK) < 0)
            return -1;
    return 0;
}

static int
publish(struct irc_owner *owner, struct irc_record *record)
{
    struct snj_irc *irc = owner->runtime;
    int rc = 0;

    record->source = owner->index;
    pthread_mutex_lock(&irc->mutex);
    while (owner->queued == IRC_MAILBOX && !irc->stopping && !irc->failure)
        pthread_cond_wait(&irc->changed, &irc->mutex);
    if (irc->stopping || irc->failure) {
        free(record);
        rc = -1;
    } else {
        ++owner->queued;
        ++irc->published;
        irc->records[(irc->head + irc->count++) % IRC_RECORDS] = record;
        wake_fd(irc->wake[1]);
    }
    pthread_mutex_unlock(&irc->mutex);
    return rc;
}

static int
capture_view(struct irc_owner *owner, struct irc_record *record)
{
    if (snj_irc_core_view(owner->core, &record->view) < 0)
        return -1;
    owner->sent = record->view;
    return 0;
}

static int
receive_event(void *opaque, const struct snj_irc_event *event)
{
    struct irc_owner *owner = opaque;
    struct irc_record *record;

    /* A broadcast local send has one application echo, from the primary owner. */
    if (owner->index && event->local &&
        (event->kind == SNJ_IRC_MESSAGE || event->kind == SNJ_IRC_NOTICE))
        return 0;
    record = calloc(1u, sizeof(*record));
    if (!record)
        return -1;
    record->kind = IRC_EVENT;
    record->event = *event;
    if (capture_view(owner, record) < 0) {
        free(record);
        return -1;
    }
    return publish(owner, record);
}

static int
receive_trace(void *opaque, unsigned int level, char direction,
              const char *endpoint, const char *text, size_t len)
{
    struct irc_owner *owner = opaque;
    struct irc_record *record = calloc(1u, sizeof(*record));

    if (!record)
        return -1;
    record->kind = IRC_TRACE;
    record->level = level;
    record->direction = direction;
    if (len >= sizeof(record->trace) ||
        !snj_strcpy(record->event.endpoint, sizeof(record->event.endpoint),
                    endpoint)) {
        free(record);
        errno = EOVERFLOW;
        return -1;
    }
    memcpy(record->trace, text, len);
    return publish(owner, record);
}

static int
refresh_view(struct irc_owner *owner)
{
    struct irc_record *record = calloc(1u, sizeof(*record));

    if (!record)
        return -1;
    if (snj_irc_core_view(owner->core, &record->view) < 0) {
        free(record);
        return -1;
    }
    if (memcmp(&owner->sent, &record->view, sizeof(record->view)) == 0) {
        free(record);
        return 0;
    }
    owner->sent = record->view;
    record->kind = IRC_VIEW;
    return publish(owner, record);
}

static int
execute(struct irc_owner *owner, struct irc_request *request)
{
    struct snj_irc_core *core = owner->core;
    char *error = request->error;
    size_t size = sizeof(request->error);

    switch (request->command) {
    case IRC_OPERATOR:
        return snj_irc_core_send_operator(core, request->text, error, size);
    case IRC_AGENT:
        return snj_irc_core_send_agent(core, request->text, error, size);
    case IRC_NOTICE:
        return snj_irc_core_send_agent_notice(core, request->text, error, size);
    case IRC_OPERATOR_TOPIC:
        return snj_irc_core_set_operator_topic(core, request->text, error, size);
    case IRC_AGENT_TOPIC:
        return snj_irc_core_set_agent_topic(core, request->text, error, size);
    case IRC_RESTORE:
        return snj_irc_core_restore_event(core, request->event);
    }
    errno = EINVAL;
    return -1;
}

static void *
run_owner(void *opaque)
{
    struct irc_owner *owner = opaque;
    struct snj_irc *irc = owner->runtime;
    char error[256u];

    for (;;) {
        struct irc_request *request;
        bool stopping;
        char bytes[64u];
        int rc;

        while (read(owner->wake[0], bytes, sizeof(bytes)) > 0)
            ;
        pthread_mutex_lock(&irc->mutex);
        stopping = irc->stopping || irc->failure;
        request = owner->request;
        owner->request = NULL;
        pthread_mutex_unlock(&irc->mutex);
        if (stopping)
            break;
        if (request) {
            request->result = execute(owner, request);
            request->saved_errno = errno;
            rc = refresh_view(owner);
            pthread_mutex_lock(&irc->mutex);
            request->done = true;
            request->through = irc->published;
            wake_fd(irc->wake[1]);
            pthread_mutex_unlock(&irc->mutex);
        } else {
            rc = snj_irc_core_tick(owner->core, -1, owner->wake[0],
                                    error, sizeof(error));
            if (rc == 0)
                rc = refresh_view(owner);
        }
        if (rc < 0) {
            pthread_mutex_lock(&irc->mutex);
            if (!irc->failure)
                irc->failure = errno ? errno : EIO;
            pthread_cond_broadcast(&irc->changed);
            wake_fd(irc->wake[1]);
            pthread_mutex_unlock(&irc->mutex);
            break;
        }
    }
    return NULL;
}

static int
start_owners(struct snj_irc *irc)
{
    pthread_mutex_lock(&irc->mutex);
    errno = irc->failure ? irc->failure : irc->stopping ? ECANCELED : 0;
    pthread_mutex_unlock(&irc->mutex);
    if (errno)
        return -1;
    for (size_t i = 0u; i < irc->owner_count; ++i) {
        struct irc_owner *owner = &irc->owners[i];
        int rc;

        if (owner->started)
            continue;
        rc = pthread_create(&owner->thread, NULL, run_owner, owner);
        if (rc) {
            errno = rc;
            return -1;
        }
        owner->started = true;
    }
    return 0;
}

static void
stop_owners(struct snj_irc *irc)
{
    pthread_mutex_lock(&irc->mutex);
    irc->stopping = true;
    pthread_cond_broadcast(&irc->changed);
    for (size_t i = 0u; i < irc->owner_count; ++i)
        if (irc->owners[i].started)
            wake_fd(irc->owners[i].wake[1]);
    pthread_mutex_unlock(&irc->mutex);
    for (size_t i = 0u; i < irc->owner_count; ++i)
        if (irc->owners[i].started) {
            pthread_join(irc->owners[i].thread, NULL);
            irc->owners[i].started = false;
        }
}

static int
drain(struct snj_irc *irc, int timeout_ms)
{
    struct pollfd fd = {irc->wake[0], POLLIN, 0};
    char bytes[64u];
    size_t remaining;
    int rc;

    do {
        rc = poll(&fd, 1u, timeout_ms);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0)
        return -1;
    while (read(irc->wake[0], bytes, sizeof(bytes)) > 0)
        ;
    pthread_mutex_lock(&irc->mutex);
    remaining = irc->count < IRC_MAILBOX ? irc->count : IRC_MAILBOX;
    while (remaining--) {
        struct irc_record *record = irc->records[irc->head];
        int rc = 0;

        irc->head = (irc->head + 1u) % IRC_RECORDS;
        --irc->count;
        --irc->owners[record->source].queued;
        pthread_cond_broadcast(&irc->changed);
        if (record->kind != IRC_TRACE) {
            if (!record->source &&
                (strcmp(irc->owners[0].view.model, record->view.model) != 0 ||
                 strcmp(irc->owners[0].view.operator, record->view.operator) != 0))
                irc->identity_changed = true;
            irc->owners[record->source].view = record->view;
        }
        pthread_mutex_unlock(&irc->mutex);
        if (record->kind == IRC_EVENT) {
            snj_irc_core_remember(irc->history, &record->event);
            if (irc->event_fn)
                rc = irc->event_fn(irc->opaque, &record->event);
        } else if (record->kind == IRC_TRACE && irc->trace_fn) {
            rc = irc->trace_fn(irc->opaque, record->level, record->direction,
                              record->event.endpoint, record->trace,
                              strlen(record->trace));
        }
        free(record);
        pthread_mutex_lock(&irc->mutex);
        ++irc->admitted;
        if (rc < 0) {
            irc->failure = errno ? errno : EIO;
            pthread_cond_broadcast(&irc->changed);
            break;
        }
    }
    if (irc->count)
        wake_fd(irc->wake[1]);
    errno = irc->failure;
    pthread_mutex_unlock(&irc->mutex);
    return errno ? -1 : 0;
}

static int
request_owner(struct irc_owner *owner, struct irc_request *request)
{
    struct snj_irc *irc = owner->runtime;
    bool done;

    if (!owner->started)
        return execute(owner, request);
    request->done = false;
    pthread_mutex_lock(&irc->mutex);
    owner->request = request;
    wake_fd(owner->wake[1]);
    pthread_mutex_unlock(&irc->mutex);
    do {
        if (drain(irc, 25) < 0) {
            int saved = errno;

            /* The request's stack lifetime ends only after its owner stops. */
            stop_owners(irc);
            errno = saved;
            return -1;
        }
        pthread_mutex_lock(&irc->mutex);
        done = request->done && irc->admitted >= request->through;
        pthread_mutex_unlock(&irc->mutex);
    } while (!done);
    /* Completion cannot overtake the command's emitted records. */
    errno = request->saved_errno;
    return request->result;
}

static int
add_owner(struct snj_irc *irc, const struct snj_config *config,
          const char *workspace, char *error, size_t error_size)
{
    struct irc_owner *owner = &irc->owners[irc->owner_count];

    owner->runtime = irc;
    owner->index = irc->owner_count++;
    owner->wake[0] = owner->wake[1] = -1;
    (void)snj_strcpy(owner->endpoint, sizeof(owner->endpoint),
                      config->irc_listen_explicit ? config->irc_listen :
                                                   config->irc_clients[0]);
    if (open_wake(owner->wake) < 0)
        return -1;
    if (snj_irc_core_open(&owner->core, config, workspace, true, receive_event,
                           irc->trace_fn ? receive_trace : NULL, owner,
                           error, error_size) < 0 ||
        snj_irc_core_view(owner->core, &owner->view) < 0)
        return -1;
    owner->sent = owner->view;
    return 0;
}

int
snj_irc_open(struct snj_irc **out, const struct snj_config *config,
             const char *workspace, snj_irc_event_fn event_fn,
             snj_irc_trace_fn trace_fn, void *opaque,
             char *error, size_t error_size)
{
    struct snj_irc *irc;
    struct snj_config local;
    int rc;

    if (!out || !config || !workspace || !snj_irc_enabled(config)) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    irc = calloc(1u, sizeof(*irc));
    if (!irc)
        return -1;
    rc = pthread_mutex_init(&irc->mutex, NULL);
    if (rc) {
        free(irc);
        errno = rc;
        return -1;
    }
    rc = pthread_cond_init(&irc->changed, NULL);
    if (rc) {
        pthread_mutex_destroy(&irc->mutex);
        free(irc);
        errno = rc;
        return -1;
    }
    irc->event_fn = event_fn;
    irc->trace_fn = trace_fn;
    irc->opaque = opaque;
    irc->hosting = config->irc_listen_explicit;
    irc->wake[0] = irc->wake[1] = -1;
    if (open_wake(irc->wake) < 0)
        goto fail;
    if (snj_irc_core_open(&irc->history, config, workspace, false, NULL, NULL,
                           NULL, error, error_size) < 0)
        goto fail;
    /* Config copies never leave startup; owners receive private protocol state. */
    local = *config;
    local.irc_client_count = 0u;
    if (irc->hosting && add_owner(irc, &local, workspace, error, error_size) < 0)
        goto fail;
    local.irc_listen_explicit = false;
    local.irc_client_count = 1u;
    local.irc_history_lines = 0u; /* Only the engine and hosted server retain it. */
    for (size_t i = 0u; i < config->irc_client_count; ++i) {
        if (irc->hosting &&
            snj_irc_endpoint_equal(config->irc_clients[i], config->irc_listen))
            continue;
        (void)snj_strcpy(local.irc_clients[0], sizeof(local.irc_clients[0]),
                          config->irc_clients[i]);
        if (add_owner(irc, &local, workspace, error, error_size) < 0)
            goto fail;
    }
    *out = irc;
    return 0;
fail:
    snj_irc_close(irc);
    return -1;
}

void
snj_irc_close(struct snj_irc *irc)
{
    if (!irc)
        return;
    stop_owners(irc);
    for (size_t i = 0u; i < irc->owner_count; ++i) {
        struct irc_owner *owner = &irc->owners[i];

        snj_irc_core_close(owner->core);
        for (size_t j = 0u; j < 2u; ++j)
            if (owner->wake[j] >= 0)
                close(owner->wake[j]);
    }
    while (irc->count) {
        free(irc->records[irc->head]);
        irc->head = (irc->head + 1u) % IRC_RECORDS;
        --irc->count;
    }
    snj_irc_core_close(irc->history);
    for (size_t i = 0u; i < 2u; ++i)
        if (irc->wake[i] >= 0)
            close(irc->wake[i]);
    pthread_cond_destroy(&irc->changed);
    pthread_mutex_destroy(&irc->mutex);
    free(irc);
}

int
snj_irc_tick(struct snj_irc *irc, int timeout_ms,
             char *error, size_t error_size)
{
    if (irc && start_owners(irc) == 0 && drain(irc, timeout_ms) == 0)
        return 0;
    snj_errorf(error, error_size, "IRC event loop failed: %s", strerror(errno));
    return -1;
}

static int
send_command(struct snj_irc *irc, enum irc_command command, const char *text,
             char *error, size_t error_size)
{
    bool accepted = false;

    if (!irc || start_owners(irc) < 0)
        return -1;
    for (size_t i = 0u; i < irc->owner_count; ++i) {
        struct irc_request request = {.command = command, .text = text};
        int rc = request_owner(&irc->owners[i], &request);

        if (rc == 0) {
            accepted = true;
            continue;
        }
        snj_errorf(error, error_size, "%s", request.error[0] ?
                   request.error : strerror(errno));
        if (errno != EACCES)
            return -1;
    }
    if (accepted && error_size)
        error[0] = '\0';
    return accepted ? 0 : -1;
}

int
snj_irc_send_operator(struct snj_irc *irc, const char *text,
                      char *error, size_t error_size)
{
    return send_command(irc, IRC_OPERATOR, text, error, error_size);
}

int
snj_irc_send_agent(struct snj_irc *irc, const char *text,
                   char *error, size_t error_size)
{
    return send_command(irc, IRC_AGENT, text, error, error_size);
}

int
snj_irc_send_agent_notice(struct snj_irc *irc, const char *text,
                          char *error, size_t error_size)
{
    return send_command(irc, IRC_NOTICE, text, error, error_size);
}

int
snj_irc_set_operator_topic(struct snj_irc *irc, const char *text,
                           char *error, size_t error_size)
{
    return send_command(irc, IRC_OPERATOR_TOPIC, text, error, error_size);
}

int
snj_irc_set_agent_topic(struct snj_irc *irc, const char *text,
                        char *error, size_t error_size)
{
    return send_command(irc, IRC_AGENT_TOPIC, text, error, error_size);
}

int
snj_irc_snapshot(const struct snj_irc *irc, struct snj_buf *out,
                 char *error, size_t error_size)
{
    if (!irc || !out) {
        errno = EINVAL;
        return -1;
    }
    if (snj_buf_printf(out,
            "[IRC room snapshot; @ marks a channel operator]\n"
            "model nick: %s\noperator nick: %s\nhosted: %s\n",
            snj_irc_model_nick(irc), snj_irc_operator_nick(irc),
            irc->hosting ? irc->owners[0].endpoint : "no") < 0)
        goto fail;
    for (size_t i = 0u; i < irc->owner_count; ++i)
        if (snj_buf_append(out, irc->owners[i].view.text,
                           strlen(irc->owners[i].view.text)) < 0)
            goto fail;
    if (snj_irc_core_history(irc->history, out) < 0)
        goto fail;
    return 0;
fail:
    snj_errorf(error, error_size, "IRC snapshot exceeds its bound");
    return -1;
}

int
snj_irc_restore_event(struct snj_irc *irc, const struct snj_irc_event *event)
{
    struct irc_request request = {.command = IRC_RESTORE, .event = event};

    if (!irc || snj_irc_core_restore_event(irc->history, event) < 0)
        return -1;
    return irc->hosting ? request_owner(&irc->owners[0], &request) : 0;
}

int
snj_irc_replay_hosted_history(const struct snj_irc *irc,
                              snj_irc_event_fn render, void *opaque)
{
    return snj_irc_core_replay_hosted_history(irc ? irc->history : NULL,
                                              render, opaque);
}

const char *
snj_irc_model_nick(const struct snj_irc *irc)
{
    return irc ? irc->owners[0].view.model : NULL;
}

const char *
snj_irc_operator_nick(const struct snj_irc *irc)
{
    return irc ? irc->owners[0].view.operator : NULL;
}

const char *
snj_irc_room_name(const struct snj_irc *irc)
{
    return snj_irc_core_room_name(irc ? irc->history : NULL);
}

bool
snj_irc_identity_changed(struct snj_irc *irc)
{
    bool changed;

    if (!irc)
        return false;
    pthread_mutex_lock(&irc->mutex);
    changed = irc->identity_changed;
    irc->identity_changed = false;
    pthread_mutex_unlock(&irc->mutex);
    return changed;
}

bool
snj_irc_mentions_agent(const struct snj_irc *irc, const char *endpoint,
                       const char *text)
{
    if (!irc || !endpoint || !text)
        return false;
    for (size_t i = 0u; i < irc->owner_count; ++i) {
        const struct irc_owner *owner = &irc->owners[i];

        if (owner->view.joined && snj_irc_endpoint_equal(endpoint, owner->endpoint)
            && snj_irc_nick_mentioned(text, owner->view.model))
            return true;
    }
    return false;
}
