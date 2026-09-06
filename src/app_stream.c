/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"

#include "render.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int
stream_fail(struct app_state *app, int error_number, const char *message)
{
    app->stream_failed = true;
    app->stream_errno = error_number ? error_number : EIO;
    (void)snprintf(app->stream_error, sizeof(app->stream_error), "%s", message);
    errno = app->stream_errno;
    return -1;
}

void
snag_app_clear_partial_public(struct app_state *app)
{
    for (size_t i = 0; i < app->partial_count; ++i)
        snag_buf_free(&app->partial[i].text);
    memset(app->partial, 0, sizeof(app->partial));
    app->partial_count = 0;
    app->partial_bytes = 0;
}

static struct partial_public_item *
partial_public_target(struct app_state *app, size_t graph_index,
                      enum snag_item_kind kind, enum snag_item_phase phase,
                      const char *provider_item_id, bool *created)
{
    struct partial_public_item *item;
    size_t id_len;

    *created = false;
    if (!provider_item_id || kind == SNAG_ITEM_TOOL_CALL ||
        phase == SNAG_PHASE_NONE || graph_index >= SNAG_MAX_RESPONSE_ITEMS) {
        errno = EPROTO;
        return NULL;
    }
    id_len = strlen(provider_item_id);
    if (!id_len || id_len > SNAG_MAX_PROVIDER_ID ||
        !snag_utf8_valid((const unsigned char *)provider_item_id, id_len, true)) {
        errno = EPROTO;
        return NULL;
    }
    if (app->partial_count == 0u ||
        app->partial[app->partial_count - 1u].graph_index != graph_index) {
        if (app->partial_count >= SNAG_MAX_RESPONSE_ITEMS) {
            errno = EOVERFLOW;
            return NULL;
        }
        item = &app->partial[app->partial_count++];
        memset(item, 0, sizeof(*item));
        item->graph_index = graph_index;
        item->kind = kind;
        item->phase = phase;
        if (snag_random_id(item->local_item_id) < 0) {
            memset(item, 0, sizeof(*item));
            --app->partial_count;
            return NULL;
        }
        memcpy(item->provider_item_id, provider_item_id, id_len + 1u);
        snag_buf_init(&item->text, SNAG_MAX_PUBLIC_ITEM);
        *created = true;
    } else {
        item = &app->partial[app->partial_count - 1u];
        if (item->kind != kind || item->phase != phase ||
            strcmp(item->provider_item_id, provider_item_id) != 0) {
            errno = EPROTO;
            return NULL;
        }
    }
    return item;
}

json_t *
snag_app_partial_public_json(const struct app_state *app)
{
    json_t *array = json_array();

    if (!array)
        goto fail;
    for (size_t i = 0; i < app->partial_count; ++i) {
        const struct partial_public_item *partial = &app->partial[i];
        json_t *item = json_pack("{s:s,s:s,s:s,s:s,s:s%}",
            "kind", snag_item_kind_name(partial->kind),
            "local_item_id", partial->local_item_id,
            "phase", snag_item_phase_name(partial->phase),
            "provider_item_id", partial->provider_item_id,
            "text", (const char *)partial->text.data, partial->text.len);
        if (!item || json_array_append_new(array, item) < 0)
            goto fail;
    }
    return array;
fail:
    if (array)
        json_decref(array);
    return NULL;
}

int
snag_app_finish_stream_item(struct app_state *app)
{
    if (!app->stream_item_active)
        return 0;
    app->stream_item_active = false;
    if (snag_ui_text(&app->ui, SNAG_UI_ROLLOUT_END, NULL) < 0) {
        return stream_fail(app, errno,
                           "public output item could not be finished");
    }
    return 0;
}

int
snag_app_abort_stream_item(struct app_state *app)
{
    if (!app->stream_item_active)
        return 0;
    app->stream_item_active = false;
    if (snag_ui_text(&app->ui, SNAG_UI_ROLLOUT_ABORT, NULL) < 0) {
        return stream_fail(app, errno,
                           "steered public output item could not be closed");
    }
    return 0;
}

int
snag_app_stream_public(void *opaque, size_t item_index, enum snag_item_kind kind,
                   enum snag_item_phase phase, const char *provider_item_id,
                   const char *text, size_t len)
{
    struct app_state *app = opaque;
    struct partial_public_item *partial;
    size_t partial_before;
    size_t partial_max;
    size_t remaining;
    bool partial_created;
    int fd = STDOUT_FILENO;
    const char *label = NULL;

    if (snag_app_active_input_pump(app, 0u) < 0)
        return stream_fail(app, errno, "active input failed before public output");
    while (len && !app->steering_requested && !app->interrupt_requested) {
        uint32_t remaining = snag_ui_pause_remaining(&app->ui);
        int input_rc;

        if (!remaining)
            break;
        input_rc = snag_app_active_input_pump(
            app, remaining > 20u ? 20u : remaining);
        if (input_rc < 0) {
            return stream_fail(app, errno,
                               "active input failed during public output");
        }
        if (input_rc == 1 || input_rc == 2)
            break;
    }
    if (app->steering_requested || app->interrupt_requested)
        return 0;
    if (!app->stream_item_seen || item_index != app->stream_item_index) {
        if (app->stream_item_seen && item_index <= app->stream_item_index)
            return stream_fail(app, EPROTO,
                               "public output indexes did not increase");
        if (snag_app_finish_stream_item(app) < 0)
            return -1;
        app->stream_item_seen = true;
        app->stream_item_index = item_index;
        app->stream_kind = kind;
        app->stream_phase = phase;
        app->stream_item_hidden = false;

        if (kind == SNAG_ITEM_ASSISTANT || kind == SNAG_ITEM_REFUSAL) {
            if (app->execute && phase == SNAG_PHASE_FINAL_ANSWER)
                app->stream_item_hidden = true;
            else if (app->execute)
                fd = STDERR_FILENO;
        } else {
            app->stream_item_hidden = true;
        }
        if (!app->stream_item_hidden) {
            if (snag_ui_public_begin(&app->ui, fd, label,
                                    SNAG_PRESENT_CONVERSATION) < 0) {
                return stream_fail(app, errno,
                                   "public output item could not be started");
            }
            app->stream_item_active = true;
        }
    } else if (kind != app->stream_kind || phase != app->stream_phase) {
        return stream_fail(app, EPROTO,
                           "public output item kind or phase changed");
    }
    if (app->stream_item_hidden)
        return 0;
    if (app->partial_bytes > SNAG_MAX_RESPONSE_GRAPH) {
        return stream_fail(app, EOVERFLOW,
                           "partial public output exceeds its bound");
    }
    partial = partial_public_target(app, item_index, kind, phase, provider_item_id,
                                    &partial_created);
    if (!partial) {
        return stream_fail(app, errno,
                           errno == EPROTO ?
                           "public output item identity changed" :
                           "partial public output could not be retained");
    }
    partial_before = partial->text.len;
    partial_max = partial->text.max;
    remaining = SNAG_MAX_RESPONSE_GRAPH - app->partial_bytes;
    if (partial->text.len > SIZE_MAX - remaining) {
        errno = EOVERFLOW;
        goto fail_partial;
    }
    if (partial->text.max > partial->text.len + remaining)
        partial->text.max = partial->text.len + remaining;
    if (snag_ui_public(&app->ui, text, len, &partial->text) < 0)
        goto fail_partial;
    partial->text.max = partial_max;
    app->partial_bytes += partial->text.len - partial_before;
    if (partial_created && partial->text.len == 0u) {
        snag_buf_free(&partial->text);
        memset(partial, 0, sizeof(*partial));
        --app->partial_count;
    }
    return 0;

fail_partial:
    partial->text.max = partial_max;
    if (partial_created && partial->text.len == 0u) {
        snag_buf_free(&partial->text);
        memset(partial, 0, sizeof(*partial));
        --app->partial_count;
    }
    return stream_fail(app, errno,
                       errno == EOVERFLOW ?
                       "public output exceeds its limit" :
                       "public output could not be rendered");
}

void
snag_app_response_cycle_release(struct app_state *app,
                               struct snag_response_graph *graph,
                               json_t **steering,
                               struct snag_context_projection *projection,
                               struct snag_buf *request_body)
{
    json_decref(*steering);
    *steering = NULL;
    snag_context_projection_free(projection);
    snag_buf_free(request_body);
    snag_response_graph_free(graph);
    snag_app_clear_partial_public(app);
}

void
snag_app_reset_stream(struct app_state *app)
{
    snag_app_clear_partial_public(app);
    app->stream_item_index = 0;
    app->stream_phase = SNAG_PHASE_NONE;
    app->stream_item_active = false;
    app->stream_item_seen = false;
    app->stream_item_hidden = false;
    app->stream_failed = false;
    app->stream_errno = 0;
    app->stream_error[0] = '\0';
    app->steering_requested = false;
    app->interrupt_requested = false;
}
