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
    struct snj_instruction_set set;
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

    snj_instructions_init(&set);
    assert(snj_instructions_discover(&set, leaf, error, sizeof(error)) == 0);
    assert(set.count == 4u);
    assert(strstr(set.sources[0].text, "global override") != NULL);
    assert(strstr(set.sources[1].text, "root guidance") != NULL);
    assert(strstr(set.sources[2].text, "sub override") != NULL);
    assert(strstr(set.sources[3].text, "leaf guidance") != NULL);
    assert(set.bytes == strlen("global override\nroot guidance\nsub override\nleaf guidance\n"));
    metadata = snj_instructions_metadata_json(&set);
    assert(metadata);
    assert(snj_instructions_metadata_valid(metadata, error, sizeof(error)) == 0);
    assert(snj_instructions_match_metadata(&set, metadata, error, sizeof(error)) == 0);
    json_decref(metadata);
    snj_instructions_free(&set);

    assert(snprintf(path, sizeof(path), "%s/AGENTS.override.md", leaf) > 0);
    write_file(path, "bad\xff\n");
    assert(snj_instructions_discover(&set, leaf, error, sizeof(error)) < 0);
    assert(errno == EILSEQ || errno == EINVAL);
    (void)unlink(path);
    assert(snprintf(path, sizeof(path), "%s/.git", sub) > 0);
    assert(symlink("../repo/.git", path) == 0);
    assert(snj_instructions_discover(&set, leaf, error, sizeof(error)) < 0);
    assert(errno == EINVAL);
    (void)unlink(path);
    snj_instructions_free(&set);

    puts("test_instructions: ok");
    return 0;
}
