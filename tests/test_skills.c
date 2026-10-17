/* SPDX-License-Identifier: GPL-2.0-only */
#include "skills.h"
#include "json.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void
write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(text, 1u, strlen(text), file) == strlen(text));
    assert(fclose(file) == 0);
}

static void
snapshot_checks(void)
{
    char directory[] = "/tmp/snag-snapshot-XXXXXX";
    char path[1024];
    char resource[1024];
    char error[512] = {0};
    assert(mkdtemp(directory));
    assert(snprintf(path, sizeof(path), "%s/SKILL.md", directory) > 0);
    assert(snprintf(resource, sizeof(resource), "%s/helper.py", directory) > 0);
    const char *original = "---\nname: sample\ndescription: approved\n---\nApproved body\n";
    write_text(path, original);
    write_text(resource, "approved helper\n");
    json_t *entry = snag_skill_load(directory, false, error, sizeof(error));
    assert(entry);
    json_t *snapshot = snag_skill_snapshot(entry, error, sizeof(error));
    assert(snapshot);
    json_decref(snapshot);

    write_text(path, "---\nname: sample\ndescription: changed\n---\nUnapproved body\n");
    assert(!snag_skill_snapshot(entry, error, sizeof(error)));
    assert(strstr(error, "changed"));
    write_text(path, original);
    write_text(resource, "unapproved helper\n");
    assert(!snag_skill_snapshot(entry, error, sizeof(error)));
    write_text(resource, "approved helper\n");
    snapshot = snag_skill_snapshot(entry, error, sizeof(error));
    assert(snapshot);
    json_decref(snapshot);
    assert(chmod(resource, 0700) == 0);
    assert(!snag_skill_snapshot(entry, error, sizeof(error)));

    json_decref(entry);
    assert(unlink(resource) == 0);
    assert(unlink(path) == 0);
    assert(rmdir(directory) == 0);
}

int
main(void)
{
    snapshot_checks();
    char directory[] = "/tmp/snag-skills-XXXXXX";
    char path[1024];
    char error[512];
    assert(mkdtemp(directory));
    assert(snprintf(path, sizeof(path), "%s/SKILL.md", directory) > 0);
    write_text(path, "---\nname: sample\ndescription: >\n  Read Unicode café\n"
        "  and resources.\nmetadata: &meta\n  author: OpenAI\ncopy: *meta\n---\n"
        "# Body\nTask: $ARGUMENTS, $0, ${CLAUDE_SKILL_DIR}\n");
    json_t *skill = snag_skill_load(directory, false, error, sizeof(error));
    assert(skill);
    assert(!strcmp(snag_json_string(skill, "name"), "sample"));
    assert(strstr(snag_json_string(skill, "description"), "café"));
    assert(strstr(snag_json_string(skill, "body"), "# Body"));
    json_t *invocation = snag_skill_invocation(skill, "\"two words\" $0", error, sizeof(error));
    assert(invocation);
    const char *body = snag_json_string(invocation, "instructions");
    assert(strstr(body, "\"two words\" $0, two words,"));
    assert(strstr(body, directory));
    json_decref(invocation);
    json_decref(skill);
    assert(!snag_skill_load(directory, true, error, sizeof(error)));
    write_text(path, "---\nname: a\nname: b\ndescription: d\n---\nbody\n");
    assert(!snag_skill_load(directory, false, error, sizeof(error)));
    assert(strstr(error, "duplicate"));
    write_text(path, "---\nname: a\ndescription: d\nloop: &a [*a]\n---\nbody\n");
    assert(!snag_skill_load(directory, false, error, sizeof(error)));
    assert(strstr(error, "cyclic"));
    write_text(path, "---\nname: a\ndescription: !execute command\n---\nbody\n");
    assert(!snag_skill_load(directory, false, error, sizeof(error)));
    write_text(path, "---\nname: a\ndescription: d\nhooks: {}\n---\nbody\n");
    skill = snag_skill_load(directory, false, error, sizeof(error));
    assert(skill);
    assert(!snag_skill_invocation(skill, "", error, sizeof(error)));
    json_decref(skill);
    assert(unlink(path) == 0);
    assert(rmdir(directory) == 0);
    puts("skills: parser and literal invocation passed");
    return 0;
}
