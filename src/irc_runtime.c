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
#define IRC_RECORDS (IRC_MAILBOX * (SNAG_CONFIG_IRC_CLIENT_MAX + 1u))

enum irc_record_kind { IRC_EVENT, IRC_TRACE, IRC_VIEW };
enum irc_command { IRC_OPERATOR, IRC_AGENT, IRC_NOTICE,
                   IRC_OPERATOR_TOPIC, IRC_AGENT_TOPIC, IRC_RESTORE };

struct irc_request {
    enum irc_command command;
    uint64_t revision;
    const char *text;
    const struct snag_irc_event *event;
    int result, saved_errno;
    char error[256u];
    bool done;
    uint64_t through;
};

struct irc_record {
    enum irc_record_kind kind;
    struct irc_owner *source;
    struct snag_irc_view view;
    struct snag_irc_event event;
    unsigned int level;
    char direction;
    char trace[SNAG_IRC_LINE_MAX];
};

struct irc_owner {
    struct snag_irc *runtime;
    struct snag_irc_core *core; /* Only this owner accesses it after startup. */
    struct snag_irc_view sent;  /* Owner-private last published view. */
    struct snag_irc_view view;  /* Engine-private admitted view. */
    char endpoint[SNAG_CONFIG_IRC_ENDPOINT_MAX + 1u];
    struct snag_irc_config settings; /* Immutable creation preferences. */
    char routing_room[SNAG_CONFIG_IRC_ROOM_MAX + 2u];
    struct snag_irc_target target;
    pthread_t thread;
    int wake[2];
    struct irc_request *request; /* Protected by the mailbox mutex. */
    size_t queued;
    bool started, stopping, finished;
    bool hosting;
};

struct snag_irc {
    struct irc_owner *owners[SNAG_CONFIG_IRC_CLIENT_MAX + 1u];
    size_t owner_count;
    uint32_t last_destination_id;
    struct snag_irc_core *history; /* Engine-owned replay/admitted history. */
    snag_irc_event_fn event_fn;
    snag_irc_trace_fn trace_fn;
    void *opaque;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    struct irc_record *records[IRC_RECORDS];
    size_t head, count;
    uint64_t published, admitted;
    uint64_t routing_revision;
    int wake[2];
    bool stopping;
    bool identity_changed; /* Mailbox-locked; retained across command drains. */
    bool nicks_changed; /* Engine-owned, retained across command drains. */
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
        if (snag_fd_cloexec(fds[i]) < 0 ||
            fcntl(fds[i], F_SETFL, O_NONBLOCK) < 0)
            return -1;
    return 0;
}

static int
publish(struct irc_owner *owner, struct irc_record *record)
{
    struct snag_irc *irc = owner->runtime;
    int rc = 0;

    record->source = owner;
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
    if (snag_irc_core_view(owner->core, &record->view) < 0)
        return -1;
    owner->sent = record->view;
    return 0;
}

static int
receive_event(void *opaque, const struct snag_irc_event *event)
{
    struct irc_owner *owner = opaque;
    struct irc_record *record;

    record = calloc(1u, sizeof(*record));
    if (!record)
        return -1;
    record->kind = IRC_EVENT;
    record->event = *event;
    if (event->local)
        (void)snag_strcpy(record->event.endpoint, sizeof(record->event.endpoint),
                           owner->endpoint);
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
        !snag_strcpy(record->event.endpoint, sizeof(record->event.endpoint),
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
    if (snag_irc_core_view(owner->core, &record->view) < 0) {
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
    struct snag_irc_core *core = owner->core;
    char *error = request->error;
    size_t size = sizeof(request->error);

    if (request->revision && request->revision != owner->sent.revision) {
        snag_errorf(error, size, "destination room changed; not performed");
        errno = ESTALE;
        return -1;
    }
    switch (request->command) {
    case IRC_OPERATOR:
        return snag_irc_core_send_operator(core, request->text, error, size);
    case IRC_AGENT:
        return snag_irc_core_send_agent(core, request->text, error, size);
    case IRC_NOTICE:
        return snag_irc_core_send_agent_notice(core, request->text, error, size);
    case IRC_OPERATOR_TOPIC:
        return snag_irc_core_set_operator_topic(core, request->text, error, size);
    case IRC_AGENT_TOPIC:
        return snag_irc_core_set_agent_topic(core, request->text, error, size);
    case IRC_RESTORE:
        return snag_irc_core_restore_event(core, request->event);
    }
    errno = EINVAL;
    return -1;
}

static void *
run_owner(void *opaque)
{
    struct irc_owner *owner = opaque;
    struct snag_irc *irc = owner->runtime;
    char error[256u];

    for (;;) {
        struct irc_request *request;
        bool stopping;
        char bytes[64u];
        int rc;

        while (read(owner->wake[0], bytes, sizeof(bytes)) > 0)
            ;
        pthread_mutex_lock(&irc->mutex);
        stopping = owner->stopping || irc->stopping || irc->failure;
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
            rc = snag_irc_core_tick(owner->core, -1, owner->wake[0],
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
    pthread_mutex_lock(&irc->mutex);
    owner->finished = true;
    wake_fd(irc->wake[1]);
    pthread_mutex_unlock(&irc->mutex);
    return NULL;
}

static int
start_owners(struct snag_irc *irc)
{
    pthread_mutex_lock(&irc->mutex);
    errno = irc->failure ? irc->failure : irc->stopping ? ECANCELED : 0;
    pthread_mutex_unlock(&irc->mutex);
    if (errno)
        return -1;
    for (size_t i = 0u; i < irc->owner_count; ++i) {
        struct irc_owner *owner = irc->owners[i];
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
stop_owners(struct snag_irc *irc)
{
    pthread_mutex_lock(&irc->mutex);
    irc->stopping = true;
    pthread_cond_broadcast(&irc->changed);
    for (size_t i = 0u; i < irc->owner_count; ++i)
        if (irc->owners[i]->started)
            wake_fd(irc->owners[i]->wake[1]);
    pthread_mutex_unlock(&irc->mutex);
    for (size_t i = 0u; i < irc->owner_count; ++i)
        if (irc->owners[i]->started) {
            pthread_join(irc->owners[i]->thread, NULL);
            irc->owners[i]->started = false;
        }
}

static int
drain(struct snag_irc *irc, int timeout_ms)
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
        struct irc_owner *owner = record->source;
        int rc = 0;

        irc->head = (irc->head + 1u) % IRC_RECORDS;
        --irc->count;
        --owner->queued;
        pthread_cond_broadcast(&irc->changed);
        if (record->kind != IRC_TRACE) {
            if (strcmp(owner->view.nicks, record->view.nicks) != 0)
                irc->nicks_changed = true;
            if (irc->owner_count && owner == irc->owners[0] &&
                (strcmp(owner->view.model, record->view.model) != 0 ||
                 strcmp(owner->view.operator, record->view.operator) != 0))
                irc->identity_changed = true;
            if (record->view.room[0]) {
                if (owner->routing_room[0] &&
                    strcmp(owner->routing_room, record->view.room) != 0) {
                    ++irc->routing_revision;
                }
                memcpy(owner->routing_room, record->view.room, sizeof(owner->routing_room));
            }
            owner->view = record->view;
            owner->target.revision = record->view.revision;
        }
        pthread_mutex_unlock(&irc->mutex);
        if (record->kind == IRC_EVENT) {
            snag_irc_core_remember(irc->history, &record->event);
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
    struct snag_irc *irc = owner->runtime;
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

static void
free_owner(struct irc_owner *owner)
{
    snag_irc_core_close(owner->core);
    for (size_t i = 0u; i < 2u; ++i)
        if (owner->wake[i] >= 0)
            close(owner->wake[i]);
    free(owner);
}

static struct irc_owner *
host_owner(const struct snag_irc *irc)
{
    return irc->owner_count && irc->owners[0]->hosting ? irc->owners[0] : NULL;
}

int
snag_irc_add(struct snag_irc *irc, const struct snag_config *config,
            const char *workspace, bool hosting, const char *endpoint,
            char *error, size_t error_size)
{
    struct irc_owner *owner;
    struct snag_config local = *config;

    for (size_t i = 0u; i < irc->owner_count; ++i) {
        if (snag_irc_endpoint_equal(irc->owners[i]->endpoint, endpoint) &&
            (!hosting || irc->owners[i]->hosting))
            return 0;
    }
    if ((hosting && host_owner(irc)) || irc->owner_count ==
        SNAG_CONFIG_IRC_CLIENT_MAX + (host_owner(irc) || hosting ? 1u : 0u)) {
        snag_errorf(error, error_size, "IRC role limit reached");
        errno = E2BIG;
        return -1;
    }
    if (irc->last_destination_id == UINT32_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    owner = calloc(1u, sizeof(*owner));
    if (!owner)
        return -1;

    owner->runtime = irc;
    owner->settings = config->irc;
    owner->hosting = hosting;
    owner->target = (struct snag_irc_target){++irc->last_destination_id, 1u};
    owner->wake[0] = owner->wake[1] = -1;
    local.irc.listen_explicit = hosting;
    local.irc.client_count = hosting ? 0u : 1u;
    if (!hosting)
        local.irc.history_lines = 0u;
    if (!snag_strcpy(owner->endpoint, sizeof(owner->endpoint), endpoint) ||
        !snag_strcpy(hosting ? local.irc.listen : local.irc.clients[0],
                    sizeof(local.irc.listen), endpoint))
        goto fail;
    if (open_wake(owner->wake) < 0)
        goto fail;
    if (snag_irc_core_open(&owner->core, &local, workspace, true, receive_event,
                           irc->trace_fn ? receive_trace : NULL, owner,
                           error, error_size) < 0 ||
        (hosting && snag_irc_core_copy_history(owner->core, irc->history, true) < 0) ||
        snag_irc_core_view(owner->core, &owner->view) < 0)
        goto fail;
    owner->sent = owner->view;
    owner->target.revision = owner->view.revision;
    memcpy(owner->routing_room, owner->view.room, sizeof(owner->routing_room));
    if (hosting) {
        memmove(irc->owners + 1u, irc->owners,
                irc->owner_count * sizeof(*irc->owners));
        irc->owners[0] = owner;
    } else {
        irc->owners[irc->owner_count] = owner;
    }
    ++irc->owner_count;
    ++irc->routing_revision;
    irc->identity_changed = irc->nicks_changed = true;
    return 0;
fail:
    free_owner(owner);
    return -1;
}

int
snag_irc_remove(struct snag_irc *irc, bool hosting, const char *endpoint,
               char *error, size_t error_size)
{
    struct irc_owner *owner = NULL;
    struct snag_irc_event event = {.kind = SNAG_IRC_DISCONNECTED};
    size_t index, pending;
    int rc = 0;

    for (index = 0u; index < irc->owner_count; ++index)
        if (irc->owners[index]->hosting == hosting &&
            snag_irc_endpoint_equal(irc->owners[index]->endpoint, endpoint)) {
            owner = irc->owners[index];
            break;
        }
    if (!owner)
        return 0;
    pthread_mutex_lock(&irc->mutex);
    owner->stopping = true;
    wake_fd(owner->wake[1]);
    pthread_mutex_unlock(&irc->mutex);
    for (;;) {
        bool done;

        /* Admit accepted records before freeing their source or its identity. */
        if (drain(irc, 25) < 0) {
            rc = -1;
            break;
        }
        pthread_mutex_lock(&irc->mutex);
        done = (!owner->started || owner->finished) && !owner->queued;
        pthread_mutex_unlock(&irc->mutex);
        if (done)
            break;
    }
    if (owner->started)
        pthread_join(owner->thread, NULL);
    if (rc < 0) {
        /* Fatal admission failure: close() still owns this retired allocation. */
        owner->started = false;
        snag_errorf(error, error_size, "cannot drain removed IRC endpoint");
        return -1;
    }
    pending = snag_irc_core_pending(owner->core);
    event.timestamp_ms = snag_time_ms();
    (void)snag_strcpy(event.endpoint, sizeof(event.endpoint), owner->endpoint);
    (void)snag_strcpy(event.room, sizeof(event.room), owner->view.room);
    (void)snag_strcpy(event.nick, sizeof(event.nick), owner->view.model);
    snag_errorf(event.text, sizeof(event.text), pending ?
        "endpoint removed; discarded %zu unsent transport bytes" :
        "endpoint removed", pending);
    free_owner(owner);
    memmove(irc->owners + index, irc->owners + index + 1u,
            (--irc->owner_count - index) * sizeof(*irc->owners));
    ++irc->routing_revision;
    irc->identity_changed = irc->nicks_changed = true;
    snag_irc_core_remember(irc->history, &event);
    return irc->event_fn ? irc->event_fn(irc->opaque, &event) : 0;
}

int
snag_irc_preferences(struct snag_irc *irc, const struct snag_config *config,
                    const char *workspace, char *error, size_t error_size)
{
    struct snag_irc_core *history = NULL;

    if (snag_irc_core_open(&history, config, workspace, false, NULL, NULL,
                          NULL, error, error_size) < 0)
        return -1;
    if (snag_irc_core_copy_history(history, irc->history, false) < 0) {
        snag_irc_core_close(history);
        return -1;
    }
    snag_irc_core_close(irc->history);
    irc->history = history;
    irc->identity_changed = true;
    return 0;
}

void
snag_irc_roles(const struct snag_irc *irc, struct snag_config *config)
{
    config->irc.listen_explicit = host_owner(irc) != NULL;
    config->irc.client_count = 0u;
    memset(config->irc.clients, 0, sizeof(config->irc.clients));
    for (size_t i = 0u; i < irc->owner_count; ++i) {
        const struct irc_owner *owner = irc->owners[i];
        char *dst = owner->hosting ? config->irc.listen :
            config->irc.clients[config->irc.client_count++];

        (void)snag_strcpy(dst, sizeof(config->irc.listen), owner->endpoint);
    }
}

uint64_t
snag_irc_routing_revision(const struct snag_irc *irc)
{
    return irc ? irc->routing_revision : 0u;
}

static bool
same_identity(const struct snag_irc_config *left, const struct snag_irc_config *right)
{
    return strcmp(left->model_nick, right->model_nick) == 0 &&
        strcmp(left->operator_nick, right->operator_nick) == 0 &&
        left->model_nick_implicit == right->model_nick_implicit &&
        left->operator_nick_implicit == right->operator_nick_implicit;
}

static bool
keep_owner(const struct irc_owner *owner, const struct snag_irc_config *config)
{
    if (!same_identity(&owner->settings, config))
        return false;
    if (owner->hosting)
        return config->listen_explicit &&
            snag_irc_endpoint_equal(owner->endpoint, config->listen) &&
            strcmp(owner->settings.room_name, config->room_name) == 0 &&
            owner->settings.history_lines == config->history_lines;
    if (config->listen_explicit &&
        snag_irc_endpoint_equal(owner->endpoint, config->listen))
        return false;
    for (size_t i = 0u; i < config->client_count; ++i)
        if (snag_irc_endpoint_equal(owner->endpoint, config->clients[i]))
            return true;
    return false;
}

int
snag_irc_configure(struct snag_irc *irc, const struct snag_config *config,
                  const char *workspace, char *error, size_t error_size)
{
    size_t next;

    for (size_t i = 0u; i < irc->owner_count; ) {
        struct irc_owner *owner = irc->owners[i];

        if (keep_owner(owner, &config->irc)) {
            ++i;
        } else if (snag_irc_remove(irc, owner->hosting, owner->endpoint,
                                   error, error_size) < 0) {
            return -1;
        }
    }
    if (config->irc.listen_explicit && snag_irc_add(irc, config, workspace,
            true, config->irc.listen, error, error_size) < 0)
        return -1;
    next = host_owner(irc) ? 1u : 0u;
    for (size_t i = 0u; i < config->irc.client_count; ++i) {
        if (config->irc.listen_explicit &&
            snag_irc_endpoint_equal(config->irc.clients[i], config->irc.listen))
            continue;
        if (snag_irc_add(irc, config, workspace, false, config->irc.clients[i],
                         error, error_size) < 0)
            return -1;
        /* Reorder pointers only; threads and pending records keep their owners. */
        for (size_t j = next; j < irc->owner_count; ++j)
            if (snag_irc_endpoint_equal(irc->owners[j]->endpoint,
                                        config->irc.clients[i])) {
                struct irc_owner *owner = irc->owners[j];

                memmove(irc->owners + next + 1u, irc->owners + next,
                        (j - next) * sizeof(*irc->owners));
                irc->owners[next++] = owner;
                break;
            }
    }
    irc->identity_changed = irc->nicks_changed = true;
    return snag_irc_preferences(irc, config, workspace, error, error_size);
}

static int
destination_order(const void *left, const void *right)
{
    const struct snag_irc_destination *a = left, *b = right;
    return a->target.id > b->target.id ? 1 : a->target.id < b->target.id ? -1 : 0;
}

void
snag_irc_destinations(const struct snag_irc *irc, struct snag_irc_destinations *out)
{
    memset(out, 0, sizeof(*out));
    for (size_t i = 0u; irc && i < irc->owner_count; ++i) {
        const struct irc_owner *owner = irc->owners[i];
        struct snag_irc_destination *item = &out->items[out->count++];
        item->target = owner->target;
        item->joined = owner->view.joined;
        (void)snag_strcpy(item->endpoint, sizeof(item->endpoint), owner->endpoint);
        (void)snag_strcpy(item->room, sizeof(item->room), owner->routing_room);
        (void)snag_strcpy(item->model, sizeof(item->model), owner->view.model);
        (void)snag_strcpy(item->operator, sizeof(item->operator), owner->view.operator);
        (void)snag_strcpy(item->nicks, sizeof(item->nicks), owner->view.nicks);
    }
    qsort(out->items, out->count, sizeof(out->items[0]), destination_order);
}

void
snag_irc_capture_route(const struct snag_irc *irc, struct snag_irc_route *out)
{
    memset(out, 0, sizeof(*out));
    for (size_t i = 0u; irc && i < irc->owner_count; ++i)
        out->targets[out->count++] = irc->owners[i]->target;
}

bool
snag_irc_event_target(const struct snag_irc *irc, const struct snag_irc_event *event,
                       struct snag_irc_target *target)
{
    for (size_t i = 0u; irc && i < irc->owner_count; ++i) {
        const struct irc_owner *owner = irc->owners[i];
        if (snag_irc_endpoint_equal(owner->endpoint, event->endpoint) &&
            strcmp(owner->routing_room, event->room) == 0) {
            *target = owner->target;
            return true;
        }
    }
    return false;
}

bool
snag_irc_local_identity(const struct snag_irc *irc,
                         const struct snag_irc_event *event, bool model)
{
    if (!event->local)
        return false;
    for (size_t i = 0u; irc && i < irc->owner_count; ++i) {
        const struct irc_owner *owner = irc->owners[i];
        if (snag_irc_endpoint_equal(owner->endpoint, event->endpoint) &&
            strcmp(model ? owner->view.model : owner->view.operator, event->nick) == 0)
            return true;
    }
    return false;
}

int
snag_irc_open(struct snag_irc **out, const struct snag_config *config,
             const char *workspace, snag_irc_event_fn event_fn,
             snag_irc_trace_fn trace_fn, void *opaque,
             char *error, size_t error_size)
{
    struct snag_irc *irc;
    int rc;

    if (!out || !config || !workspace) {
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
    irc->nicks_changed = true;
    irc->wake[0] = irc->wake[1] = -1;
    if (open_wake(irc->wake) < 0)
        goto fail;
    if (snag_irc_core_open(&irc->history, config, workspace, false, NULL, NULL,
                           NULL, error, error_size) < 0)
        goto fail;
    if (config->irc.listen_explicit && snag_irc_add(irc, config, workspace,
            true, config->irc.listen, error, error_size) < 0)
        goto fail;
    for (size_t i = 0u; i < config->irc.client_count; ++i) {
        if (snag_irc_add(irc, config, workspace, false, config->irc.clients[i],
                        error, error_size) < 0)
            goto fail;
    }
    *out = irc;
    return 0;
fail:
    snag_irc_close(irc);
    return -1;
}

void
snag_irc_close(struct snag_irc *irc)
{
    if (!irc)
        return;
    stop_owners(irc);
    for (size_t i = 0u; i < irc->owner_count; ++i) {
        free_owner(irc->owners[i]);
    }
    while (irc->count) {
        free(irc->records[irc->head]);
        irc->head = (irc->head + 1u) % IRC_RECORDS;
        --irc->count;
    }
    snag_irc_core_close(irc->history);
    for (size_t i = 0u; i < 2u; ++i)
        if (irc->wake[i] >= 0)
            close(irc->wake[i]);
    pthread_cond_destroy(&irc->changed);
    pthread_mutex_destroy(&irc->mutex);
    free(irc);
}

int
snag_irc_tick(struct snag_irc *irc, int timeout_ms,
             char *error, size_t error_size)
{
    if (irc && start_owners(irc) == 0 && drain(irc, timeout_ms) == 0)
        return 0;
    snag_errorf(error, error_size, "IRC event loop failed: %s", strerror(errno));
    return -1;
}

int
snag_irc_send_route(struct snag_irc *irc, const struct snag_irc_route *route,
                      bool model, enum snag_irc_event_kind kind, const char *text,
                      struct snag_buf *report, char *error, size_t error_size)
{
    int failed = 0;
    size_t accepted = 0u;
    enum irc_command command;
    struct snag_irc_route frozen;

    if (!route || !route->count || route->count > SNAG_IRC_DESTINATIONS_MAX) {
        snag_errorf(error, error_size, "no IRC destination selected; use /names");
        return 1;
    }
    frozen = *route;
    route = &frozen;
    if (kind != SNAG_IRC_TOPIC && kind != SNAG_IRC_MESSAGE &&
        (kind != SNAG_IRC_NOTICE || !model)) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0u; i < route->count; ++i)
        for (size_t j = 0u; j < i; ++j)
            if (route->targets[i].id == route->targets[j].id) {
                snag_errorf(error, error_size, "duplicate IRC destination");
                return 1;
            }
    if (irc && start_owners(irc) < 0)
        return -1;
    command = kind == SNAG_IRC_TOPIC ? (model ? IRC_AGENT_TOPIC : IRC_OPERATOR_TOPIC) :
              kind == SNAG_IRC_NOTICE ? IRC_NOTICE : model ? IRC_AGENT : IRC_OPERATOR;
    for (size_t i = 0u; i < route->count; ++i) {
        struct irc_owner *owner = NULL;
        struct irc_request request = {
            .command = command, .text = text, .revision = route->targets[i].revision
        };
        int rc = 1;
        for (size_t j = 0u; irc && j < irc->owner_count; ++j)
            if (irc->owners[j]->target.id == route->targets[i].id &&
                irc->owners[j]->target.revision == route->targets[i].revision)
                owner = irc->owners[j];
        if (owner)
            rc = request_owner(owner, &request);
        if (rc == 0)
            ++accepted;
        if (rc != 0) {
            failed = 1;
            snag_errorf(error, error_size, "destination %u: %s", route->targets[i].id,
                request.error[0] ? request.error : "unavailable or changed; not performed");
        }
        if (report && snag_buf_printf(report, "destination %u: %s%s\n",
                route->targets[i].id,
                rc == 0 ? (owner->view.joined ? "queued" : "queued while connecting") : "not performed: ",
                rc == 0 ? "" : request.error[0] ? request.error : "unavailable or changed") < 0)
            return -1;
        if (irc && irc->failure)
            return -1;
    }
    return failed ? (accepted ? 2 : 1) : 0;
}

static int
send_command(struct snag_irc *irc, enum irc_command command, const char *text,
             char *error, size_t error_size)
{
    bool accepted = false;

    if (!irc || !irc->owner_count) {
        errno = ENOTCONN;
        snag_errorf(error, error_size, "no active IRC destinations; use /connect or /server start");
        return -1;
    }
    if (start_owners(irc) < 0)
        return -1;
    for (size_t i = 0u; i < irc->owner_count; ++i) {
        struct irc_request request = {.command = command, .text = text};
        int rc = request_owner(irc->owners[i], &request);

        if (rc == 0) {
            accepted = true;
            continue;
        }
        snag_errorf(error, error_size, "%s", request.error[0] ?
                   request.error : strerror(errno));
        if (errno != EACCES)
            return -1;
    }
    if (accepted && error_size)
        error[0] = '\0';
    return accepted ? 0 : -1;
}

int
snag_irc_send_operator(struct snag_irc *irc, const char *text,
                      char *error, size_t error_size)
{
    return send_command(irc, IRC_OPERATOR, text, error, error_size);
}

int
snag_irc_send_agent(struct snag_irc *irc, const char *text,
                   char *error, size_t error_size)
{
    return send_command(irc, IRC_AGENT, text, error, error_size);
}

int
snag_irc_send_agent_notice(struct snag_irc *irc, const char *text,
                          char *error, size_t error_size)
{
    return send_command(irc, IRC_NOTICE, text, error, error_size);
}

int
snag_irc_set_operator_topic(struct snag_irc *irc, const char *text,
                           char *error, size_t error_size)
{
    return send_command(irc, IRC_OPERATOR_TOPIC, text, error, error_size);
}

int
snag_irc_set_agent_topic(struct snag_irc *irc, const char *text,
                        char *error, size_t error_size)
{
    return send_command(irc, IRC_AGENT_TOPIC, text, error, error_size);
}

int
snag_irc_state(const struct snag_irc *irc, struct snag_buf *out,
                 char *error, size_t error_size)
{
    if (!irc || !out) {
        errno = EINVAL;
        return -1;
    }
    if (snag_buf_printf(out,
            "[IRC room snapshot; @ marks a channel operator]\n"
            "model nick: %s\noperator nick: %s\nhosted: %s\n",
            snag_irc_model_nick(irc), snag_irc_operator_nick(irc),
            host_owner(irc) ? host_owner(irc)->endpoint : "no") < 0)
        goto fail;
    for (size_t i = 0u; i < irc->owner_count; ++i)
        if (snag_buf_printf(out, "destination[%u]: %s\n", irc->owners[i]->target.id,
                             irc->owners[i]->endpoint) < 0 ||
            snag_buf_append(out, irc->owners[i]->view.text,
                           strlen(irc->owners[i]->view.text)) < 0)
            goto fail;
    if (!irc->owner_count)
        return snag_buf_printf(out, "no active endpoints\n");
    return 0;
fail:
    snag_errorf(error, error_size, "IRC snapshot exceeds its bound");
    return -1;
}

int
snag_irc_snapshot(const struct snag_irc *irc, struct snag_buf *out,
                  char *error, size_t error_size)
{
    if (snag_irc_state(irc, out, error, error_size) < 0)
        return -1;
    return snag_irc_core_history(irc->history, out);
}

int
snag_irc_restore_event(struct snag_irc *irc, const struct snag_irc_event *event)
{
    struct irc_request request = {.command = IRC_RESTORE, .event = event};

    if (!irc || snag_irc_core_restore_event(irc->history, event) < 0)
        return -1;
    return host_owner(irc) ? request_owner(host_owner(irc), &request) : 0;
}

int
snag_irc_replay_hosted_history(const struct snag_irc *irc,
                              snag_irc_event_fn render, void *opaque)
{
    return snag_irc_core_replay_hosted_history(irc ? irc->history : NULL,
                                              render, opaque);
}

const char *
snag_irc_model_nick(const struct snag_irc *irc)
{
    return !irc ? NULL : irc->owner_count ? irc->owners[0]->view.model :
        snag_irc_core_model_nick(irc->history);
}

const char *
snag_irc_operator_nick(const struct snag_irc *irc)
{
    return !irc ? NULL : irc->owner_count ? irc->owners[0]->view.operator :
        snag_irc_core_operator_nick(irc->history);
}

const char *
snag_irc_room_name(const struct snag_irc *irc)
{
    return snag_irc_core_room_name(irc ? irc->history : NULL);
}

bool
snag_irc_identity_changed(struct snag_irc *irc)
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
snag_irc_mentions_agent(const struct snag_irc *irc, const char *endpoint,
                       const char *text)
{
    if (!irc || !endpoint || !text)
        return false;
    for (size_t i = 0u; i < irc->owner_count; ++i) {
        const struct irc_owner *owner = irc->owners[i];

        if (owner->view.joined && (strcmp(endpoint, "local") == 0 ||
            snag_irc_endpoint_equal(endpoint, owner->endpoint))
            && snag_irc_nick_mentioned(text, owner->view.model))
            return true;
    }
    return false;
}

int
snag_irc_take_nicks(struct snag_irc *irc, struct snag_buf *out)
{
    if (!irc || !irc->nicks_changed)
        return 0;
    for (size_t i = 0u; i < irc->owner_count; ++i)
        if (snag_buf_append(out, irc->owners[i]->view.nicks,
                          strlen(irc->owners[i]->view.nicks)) < 0)
            return -1;
    if (snag_buf_terminate(out) < 0)
        return -1;
    irc->nicks_changed = false;
    return 1;
}
