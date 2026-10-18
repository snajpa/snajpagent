/* SPDX-License-Identifier: GPL-2.0-only */
#include "app_internal.h"
#include "cli.h"
#include "json.h"
#include "store_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static json_malloc_t original_malloc;
static json_free_t original_free;
static size_t allocations, allocated_bytes;
static bool counting, fail_allocation;

static void *
counted_malloc(size_t size)
{
    if (counting) {
        ++allocations;
        allocated_bytes += size;
        if (fail_allocation) return NULL;
    }
    return original_malloc(size);
}

/* Presentation getters are the boundary of this storage test. PTY coverage
 * exercises the real owner, UI worker and active-turn command dispatch. */
unsigned int
snag_ui_verbosity(const struct snag_ui *ui)
{
    return (unsigned int)ui->input_verbosity;
}

enum snag_render_view
snag_ui_view(const struct snag_ui *ui)
{
    return ui->view;
}

int
snag_app_commit_event(struct app_state *app, const char *type, json_t *data,
    char *error, size_t error_size)
{
    return snag_session_commit(&app->session, type, data, NULL, error, error_size);
}

static json_t *
saved_args(struct app_state *app)
{
    return json_object_get(app->session.strings, "resume_options");
}

static void
assert_view(const json_t *args, const char *view)
{
    size_t count = json_array_size(args);
    assert(count >= 2u);
    assert(!strcmp(json_string_value(json_array_get(args, count - 2u)), "--view"));
    assert(!strcmp(json_string_value(json_array_get(args, count - 1u)), view));
}

void
test_resume_preferences(struct snag_store *store, const char *cwd)
{
    struct snag_cli cli;
    struct snag_config config;
    struct app_state app = {0};
    char error[256] = {0};
    snag_cli_init(&cli);
    snag_config_init(&config);
    cli.execute = true;
    app.cli = &cli;
    app.config = &config;
    app.resume_options_ready = true;
    app.ui.view = SNAG_RENDER_ROLLOUT;
    snag_session_init(&app.session);
    assert(!snag_session_create(store, &app.session, cwd, "default", "fixture", "default",
        error, sizeof(error)));
    assert(!snag_app_save_resume_options(&app, error, sizeof(error)));
    json_t *initial = json_deep_copy(saved_args(&app));
    assert(initial);
    uint64_t sequence = app.session.next_seq;
    json_get_alloc_funcs(&original_malloc, &original_free);
    json_set_alloc_funcs(counted_malloc, original_free);
    counting = true;
    for (size_t i = 0u; i < 1000u; ++i)
        assert(!snag_app_save_resume_options(&app, error, sizeof(error)));
    counting = false;
    printf("unchanged resume preferences: %zu allocations, %zu bytes\n", allocations,
        allocated_bytes);
    fflush(stdout);
    assert(allocations == 0u && allocated_bytes == 0u);
    assert(app.session.next_seq == sequence);
    app.ui.input_verbosity = 3;
    assert(!snag_app_save_resume_options(&app, error, sizeof(error)));
    assert(app.session.next_seq == sequence + 1u);
    app.ui.input_verbosity = 0;
    assert(!snag_app_save_resume_options(&app, error, sizeof(error)));
    assert(json_equal(initial, saved_args(&app)));
    json_decref(initial);
    cli.execute = false;
    assert(!snag_app_save_resume_options(&app, error, sizeof(error)));
    assert_view(saved_args(&app), "rollout");
    sequence = app.session.next_seq;
    json_t *before = json_incref(saved_args(&app));
    app.ui.view = SNAG_RENDER_CHAT;
    counting = fail_allocation = true;
    assert(snag_app_save_resume_options(&app, error, sizeof(error)) < 0);
    counting = fail_allocation = false;
    assert(json_equal(before, saved_args(&app)) && app.session.next_seq == sequence);
    assert(!snag_app_save_resume_options(&app, error, sizeof(error)));
    assert_view(saved_args(&app), "chat");
    assert_view(before, "rollout");
    json_decref(before);
    assert(!snag_session_checkpoint(&app.session, error, sizeof(error)));
    char id[SNAG_ID_HEX_LEN + 1u];
    assert(snag_strcpy(id, sizeof(id), app.session.id));
    snag_session_close(&app.session);
    assert(!snag_session_open(store, &app.session, id, error, sizeof(error)));
    assert_view(saved_args(&app), "chat");
    struct snag_cli effective, saved;
    snag_cli_init(&effective);
    snag_cli_init(&saved);
    json_t *retained = NULL;
    assert(!snag_app_restore_resume_options(&effective, &saved, &app.session, &retained,
        error, sizeof(error)));
    assert(effective.view == SNAG_CLI_VIEW_CHAT);
    effective.execute = true;
    app.cli = &effective;
    app.ui.view = SNAG_RENDER_ROLLOUT;
    assert(!snag_app_save_resume_options(&app, error, sizeof(error)));
    assert_view(saved_args(&app), "chat");
    json_decref(retained);
    snag_cli_free(&saved);
    json_set_alloc_funcs(original_malloc, original_free);
    snag_session_close(&app.session);
    snag_config_free(&config);
    snag_cli_free(&cli);
}
