/* SPDX-License-Identifier: GPL-2.0-only */
#include "instructions.h"
#include "json.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void
write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fwrite(text, 1u, strlen(text), f) == strlen(text));
    assert(fclose(f) == 0);
}

static void
mkdir_checked(const char *path)
{
    assert(mkdir(path, 0700) == 0 || errno == EEXIST);
}

int
main(void)
{
    char temp[] = "/tmp/snajpagent-instructions-XXXXXX";
    char home[4096];
    char config[4096];
    char repo[4096];
    char sub[4096];
    char leaf[4096];
    char path[4096];
    char error[256];
    struct snag_instruction_set set;
    json_t *metadata;

    assert(mkdtemp(temp));
    assert(snprintf(home, sizeof(home), "%s/home", temp) > 0);
    assert(snprintf(config, sizeof(config), "%s/.config/snajpagent", home) > 0);
    assert(snprintf(repo, sizeof(repo), "%s/repo", temp) > 0);
    assert(snprintf(sub, sizeof(sub), "%s/sub", repo) > 0);
    assert(snprintf(leaf, sizeof(leaf), "%s/leaf", sub) > 0);
    mkdir_checked(home);
    assert(snprintf(path, sizeof(path), "%s/.config", home) > 0);
    mkdir_checked(path);
    mkdir_checked(config);
    mkdir_checked(repo);
    mkdir_checked(sub);
    mkdir_checked(leaf);
    assert(snprintf(path, sizeof(path), "%s/.git", repo) > 0);
    mkdir_checked(path);
    assert(setenv("HOME", home, 1) == 0);
    assert(unsetenv("XDG_CONFIG_HOME") == 0);

    assert(snprintf(path, sizeof(path), "%s/AGENTS.override.md", config) > 0);
    write_file(path, "global override\n");
    assert(snprintf(path, sizeof(path), "%s/AGENTS.md", repo) > 0);
    write_file(path, "root guidance\n");
    assert(snprintf(path, sizeof(path), "%s/AGENTS.override.md", sub) > 0);
    write_file(path, "sub override\n");
    assert(snprintf(path, sizeof(path), "%s/AGENTS.md", leaf) > 0);
    write_file(path, "leaf guidance\n");

    snag_instructions_init(&set);
    assert(snag_instructions_discover(&set, leaf, error, sizeof(error)) == 0);
    assert(set.count == 4u);
    assert(strstr(set.paths[0], "/snajpagent/AGENTS.override.md") != NULL);
    assert(strstr(set.paths[1], "/repo/AGENTS.md") != NULL);
    assert(strstr(set.paths[2], "/sub/AGENTS.override.md") != NULL);
    assert(strcmp(set.paths[3], path) == 0);
    assert(snag_instructions_add_directory(&set, leaf, error, sizeof(error)) == 0);
    assert(set.count == 4u); /* repeated roots do not duplicate pointers */
    metadata = snag_instructions_metadata_json(&set);
    assert(metadata);
    assert(snag_instructions_metadata_valid(metadata, error, sizeof(error)) == 0);
    assert(snag_instructions_match_metadata(&set, metadata, error, sizeof(error)) == 0);
    write_file(path, "Changed after discovery; only pointers are frozen.\n");
    assert(snag_instructions_match_metadata(&set, metadata, error, sizeof(error)) == 0);
    assert(json_array_append_new(metadata, json_string(path)) == 0);
    assert(snag_instructions_metadata_valid(metadata, error, sizeof(error)) < 0);
    json_array_clear(metadata);
    assert(json_array_append_new(metadata, json_string("relative/AGENTS.md")) == 0);
    assert(snag_instructions_metadata_valid(metadata, error, sizeof(error)) < 0);
    json_decref(metadata);
    snag_instructions_free(&set);

    assert(snprintf(path, sizeof(path), "%s/AGENTS.override.md", leaf) > 0);
    write_file(path, "bad\xff\n");
    assert(snag_instructions_discover(&set, leaf, error, sizeof(error)) == 0);
    assert(strcmp(set.paths[3], path) == 0); /* content is not read by discovery */
    (void)unlink(path);
    assert(symlink("AGENTS.md", path) == 0);
    assert(snag_instructions_discover(&set, leaf, error, sizeof(error)) < 0);
    assert(errno == EINVAL);
    assert(unlink(path) == 0);
    assert(snag_instructions_add_directory(&set, temp, error, sizeof(error)) < 0);
    assert(snag_instructions_add_directory(&set, "", error, sizeof(error)) < 0);
    assert(snprintf(path, sizeof(path), "%s/.git", sub) > 0);
    assert(symlink("../repo/.git", path) == 0);
    assert(snag_instructions_discover(&set, leaf, error, sizeof(error)) < 0);
    assert(errno == EINVAL);
    (void)unlink(path);
    snag_instructions_free(&set);

    puts("test_instructions: ok");
    return 0;
}
