/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>

/* Older clients could persist several snapshots for one ID and arguments.
 * Exercise that internal shape without exporting a production test hook. */
#include "../src/app_skills.c"

static struct app_state app;
static struct snag_config config;

static void
test_legacy_bindings(void)
{
    char path[4096], directory[4096], source[4096], error[512] = "";
    const char *tmp = getenv("TMPDIR");
    assert(snprintf(path, sizeof(path), "%s/snajpagent-app-skills-XXXXXX",
        tmp ? tmp : "/tmp") > 0);
    assert(mkdtemp(path));
    assert(!setenv("HOME", path, 1));
    assert(snprintf(directory, sizeof(directory), "%s/skills", path) > 0);
    assert(!mkdir(directory, 0700));
    assert(snprintf(directory, sizeof(directory), "%s/skills/sample", path) > 0);
    assert(!mkdir(directory, 0700));
    assert(snprintf(source, sizeof(source), "%s/SKILL.md", directory) > 0);
    FILE *file = fopen(source, "w");
    assert(file);
    assert(fputs("---\nname: sample\ndescription: Legacy binding fixture\n---\nCurrent body\n",
        file) >= 0);
    assert(!fclose(file));
    snag_config_init(&config);
    app.config = &config;
    app.execute = true;
    snag_store_init(&app.store);
    snag_session_init(&app.session);
    assert(!snag_store_open(&app.store, path, error, sizeof(error)));
    assert(!snag_session_create(&app.store, &app.session, path, "default", "fixture", "default",
        error, sizeof(error)));
    assert(!snag_ui_init(&app.ui));
    const char *turn = "11111111111111111111111111111111";
    assert(!snag_session_commit(&app.session, "turn_started",
        json_pack("{s:s,s:i,s:s,s:n,s:n,s:s,s:s,s:b,s:[],s:{s:s,s:s,s:s,s:i,s:b}}", "turn_id",
            turn, "turn_number", 1, "input_kind", "direct", "queue_id", "queue_seq", "cwd",
            path, "text", "Check legacy skill bindings.", "read_only", 0, "instructions", "config",
            "provider", "default", "model", "fixture", "effort", "default",
            "max_parallel_commands", 4, "parallel_tool_calls", 1),
        NULL, error, sizeof(error)));
    app.session.skills_catalog = snag_app_skills_catalog(&app, path, NULL, error, sizeof(error));
    assert(app.session.skills_catalog);
    json_t *current = activate(&app, "sample", "first", true, error, sizeof(error));
    json_t *other = activate(&app, "sample", "second", true, error, sizeof(error));
    assert(current && other);
    json_t *old = json_copy(current);
    assert(old);
    assert(!json_object_set_new(old, "digest", json_string(
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")));
    assert(!json_object_set_new(old, "instructions", json_string("Old body")));
    for (size_t order = 0; order < 2u; ++order) {
        for (size_t reactivate = 0; reactivate < 2u; ++reactivate) {
            json_t *legacy = json_array();
            assert(legacy && !json_array_append(legacy, order ? old : current));
            assert(!json_array_append(legacy, order ? current : old));
            assert(!json_array_append(legacy, other));
            assert(!snag_session_commit(&app.session, "skills_updated",
                json_pack("{s:O}", "active", legacy), NULL, error, sizeof(error)));
            json_decref(legacy);
            if (reactivate) {
                json_t *binding = activate(&app, "sample", "first", true, error, sizeof(error));
                assert(binding && json_array_size(app.session.active_skills) == 2u);
                assert(json_equal(json_array_get(app.session.active_skills, 0), other));
                assert(json_equal(json_array_get(app.session.active_skills, 1), current));
                json_t *saved = json_array_get(app.session.active_skills, 1);
                json_t *reused = activate(&app, "sample", "first", true, error, sizeof(error));
                assert(reused == saved);
                json_decref(reused);
                json_decref(binding);
            }
            size_t removed = 0;
            assert(!unload(&app, order ? snag_json_string(current, "id") : "sample",
                &removed, error, sizeof(error)));
            assert(removed == (reactivate ? 2u : 3u));
            assert(!json_array_size(app.session.active_skills));
        }
    }
    json_decref(old);
    json_decref(other);
    json_decref(current);
    snag_ui_free(&app.ui);
    snag_session_close(&app.session);
    snag_store_close(&app.store);
    snag_config_free(&config);
    assert(!unlink(source) && !rmdir(directory));
    assert(snprintf(directory, sizeof(directory), "%s/skills", path) > 0);
    assert(!rmdir(directory));
}
#endif

int
main(void)
{
#ifndef _WIN32
    test_legacy_bindings();
    puts("test_app_skills: ok");
#else
    puts("test_app_skills: POSIX fixture unavailable; skipped");
#endif
    return 0;
}
