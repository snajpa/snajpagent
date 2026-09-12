/* SPDX-License-Identifier: GPL-2.0-only */
#include "base.h"
#include "json.h"
#include "turn.h"
#include "tools_write.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char workspace[4096];

static void
write_file_bytes(const char *relative, const void *data, size_t len)
{
    char path[8192];
    FILE *out;
    (void)snprintf(path, sizeof(path), "%s/%s", workspace, relative);
    out = fopen(path, "wb");
    assert(out);
    assert(fwrite(data, 1u, len, out) == len);
    assert(fclose(out) == 0);
}

static char *
read_file_bytes(const char *relative)
{
    static char buffer[65536];
    char path[8192];
    FILE *in;
    size_t len;
    (void)snprintf(path, sizeof(path), "%s/%s", workspace, relative);
    in = fopen(path, "rb");
    assert(in);
    len = fread(buffer, 1u, sizeof(buffer) - 1u, in);
    assert(fclose(in) == 0);
    buffer[len] = '\0';
    return buffer;
}

static bool
exists(const char *relative)
{
    char path[8192];
    (void)snprintf(path, sizeof(path), "%s/%s", workspace, relative);
    return access(path, F_OK) == 0;
}

static json_t *
invoke(const char *name, json_t *arguments)
{
    struct snag_response_item call;
    json_t *result = NULL;
    char error[256] = {0};
    int rc;

    memset(&call, 0, sizeof(call));
    call.name = (char *)name;
    call.arguments = arguments;
    if (strcmp(name, "write_file") == 0)
        rc = snag_tools_write_file(&call, workspace, &result, error, sizeof(error));
    else
        rc = snag_tools_edit_file(&call, workspace, &result, error, sizeof(error));
    assert(rc == 0);
    assert(result);
    return result;
}

static void
expect_status(json_t *result, const char *status)
{
    const char *actual = snag_json_string(result, "status");
    assert(actual && strcmp(actual, status) == 0);
    assert(snag_json_string(result, "model_text"));
    json_decref(result);
}

static void
test_write_creates_and_replaces(void)
{
    json_t *result;

    result = invoke("write_file", json_pack("{s:s,s:s}", "path", "a.txt", "content", "hello\n"));
    expect_status(result, "succeeded");
    assert(strcmp(read_file_bytes("a.txt"), "hello\n") == 0);

    result = invoke("write_file", json_pack("{s:s,s:s}", "path", "a.txt", "content", "bye\n"));
    expect_status(result, "succeeded");
    assert(strcmp(read_file_bytes("a.txt"), "bye\n") == 0);
    assert(exists("a.txt"));
}

static void
test_write_preserves_mode(void)
{
    struct stat st;
    char path[8192];
    json_t *result;

    result = invoke("write_file", json_pack("{s:s,s:s}", "path", "mode.txt", "content", "one\n"));
    expect_status(result, "succeeded");
    (void)snprintf(path, sizeof(path), "%s/mode.txt", workspace);
    assert(chmod(path, 0640) == 0);

    result = invoke("write_file", json_pack("{s:s,s:s}", "path", "mode.txt", "content", "two\n"));
    expect_status(result, "succeeded");
    assert(stat(path, &st) == 0);
    assert((st.st_mode & 0777) == 0640);
    assert(strcmp(read_file_bytes("mode.txt"), "two\n") == 0);
}

static void
test_write_rejects_unsafe_paths(void)
{
    json_t *result;

    result = invoke("write_file", json_pack("{s:s,s:s}", "path", "/tmp/absolute.txt", "content", "x"));
    expect_status(result, "failed");
    assert(access("/tmp/absolute.txt", F_OK) < 0);

    result = invoke("write_file", json_pack("{s:s,s:s}", "path", "../escape.txt", "content", "x"));
    expect_status(result, "failed");

    result = invoke("write_file", json_pack("{s:s,s:s}", "path", "missing/parent.txt", "content", "x"));
    expect_status(result, "failed");
    assert(!exists("missing/parent.txt"));
}

static void
test_edit_exact_and_ambiguous(void)
{
    struct stat st;
    char path[8192];
    json_t *result;

    write_file_bytes("edit.txt", "one two two\n", 13u);
    (void)snprintf(path, sizeof(path), "%s/edit.txt", workspace);
    assert(chmod(path, 0600) == 0);

    /* Default is exactly one match: two occurrences must change nothing. */
    result = invoke("edit_file", json_pack("{s:s,s:s,s:s}", "path", "edit.txt", "old", "two", "new", "2"));
    expect_status(result, "not_run");
    assert(strcmp(read_file_bytes("edit.txt"), "one two two\n") == 0);

    result = invoke("edit_file", json_pack("{s:s,s:s,s:s,s:i}", "path", "edit.txt", "old", "two", "new", "2", "count", 2));
    expect_status(result, "succeeded");
    assert(strcmp(read_file_bytes("edit.txt"), "one 2 2\n") == 0);
    assert(stat(path, &st) == 0);
    assert((st.st_mode & 0777) == 0600);

    result = invoke("edit_file", json_pack("{s:s,s:s,s:s}", "path", "edit.txt", "old", "absent", "new", "x"));
    expect_status(result, "not_run");
    assert(strcmp(read_file_bytes("edit.txt"), "one 2 2\n") == 0);

    result = invoke("edit_file", json_pack("{s:s,s:s,s:s}", "path", "no-such.txt", "old", "x", "new", "y"));
    expect_status(result, "failed");
}

int
main(void)
{
    char template[] = "/tmp/snajpagent-test-write-XXXXXX";
    char *dir = mkdtemp(template);
    assert(dir);
    (void)snprintf(workspace, sizeof(workspace), "%s", dir);
    test_write_creates_and_replaces();
    test_write_preserves_mode();
    test_write_rejects_unsafe_paths();
    test_edit_exact_and_ambiguous();
    puts("test_write: ok");
    return 0;
}
