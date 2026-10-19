/* SPDX-License-Identifier: GPL-2.0-only */
#include "skills.h"
#include "json.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

#if defined(__linux__)
static ino_t resource_inode;
static dev_t resource_device;
static size_t resource_reads;
extern long syscall(long number, ...);

ssize_t
read(int fd, void *data, size_t size)
{
    struct stat info;
    ssize_t result = (ssize_t)syscall(SYS_read, fd, data, size);
    if (result > 0 && resource_inode && !fstat(fd, &info) &&
        info.st_ino == resource_inode && info.st_dev == resource_device) ++resource_reads;
    return result;
}
#endif

static json_malloc_t original_malloc;
static json_free_t original_free;
static size_t allocations, allocated_bytes;

static void *
counted_malloc(size_t size)
{
    ++allocations;
    allocated_bytes += size;
    return original_malloc(size);
}

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
    const char *original = "---\nname: sample\ndescription: original\n---\nApproved body\n";
    write_text(path, original);
    write_text(resource, "original helper\n");
    json_t *entry = snag_skill_load(directory, false, error, sizeof(error));
    assert(entry);
    json_t *snapshot = snag_skill_snapshot(entry, error, sizeof(error));
    assert(snapshot);
    json_decref(snapshot);

    write_text(path, "---\nname: sample\ndescription: changed\n---\nChanged body\n");
    assert(!snag_skill_snapshot(entry, error, sizeof(error)));
    assert(strstr(error, "changed"));
    write_text(path, original);
    write_text(resource, "changed helper\n");
    snapshot = snag_skill_snapshot(entry, error, sizeof(error));
    assert(snapshot);
    json_decref(snapshot);
    write_text(resource, "original helper\n");
    snapshot = snag_skill_snapshot(entry, error, sizeof(error));
    assert(snapshot);
    json_decref(snapshot);
    assert(chmod(resource, 0700) == 0);
    snapshot = snag_skill_snapshot(entry, error, sizeof(error));
    assert(snapshot);
    json_decref(snapshot);

    json_decref(entry);
    assert(unlink(resource) == 0);
    assert(unlink(path) == 0);
    assert(rmdir(directory) == 0);
}

static void
lazy_resource_cost(void)
{
    char directory[] = "/tmp/snag-lazy-cost-XXXXXX";
    char path[1024], resource[1024], error[512] = {0};
    assert(mkdtemp(directory));
    assert(snprintf(path, sizeof(path), "%s/SKILL.md", directory) > 0);
    assert(snprintf(resource, sizeof(resource), "%s/reference.bin", directory) > 0);
    write_text(path, "---\nname: sample\ndescription: Lazy resource test\n---\nTask body\n");
    json_get_alloc_funcs(&original_malloc, &original_free);
    json_set_alloc_funcs(counted_malloc, original_free);
    allocations = allocated_bytes = 0u;
    json_t *entry = snag_skill_load(directory, false, error, sizeof(error));
    assert(entry);
    json_decref(entry);
    size_t empty_allocations = allocations, empty_bytes = allocated_bytes;
    json_set_alloc_funcs(original_malloc, original_free);
    int fd = open(resource, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    char block[4096] = {0};
    for (size_t i = 0u; i < 256u; ++i) assert(write(fd, block, sizeof(block)) == sizeof(block));
    struct stat info;
    assert(!fstat(fd, &info) && !close(fd));
#if defined(__linux__)
    resource_inode = info.st_ino;
    resource_device = info.st_dev;
    resource_reads = 0u;
#endif
    json_set_alloc_funcs(counted_malloc, original_free);
    allocations = allocated_bytes = 0u;
    entry = snag_skill_load(directory, false, error, sizeof(error));
    assert(entry);
    json_decref(entry);
    assert(allocations == empty_allocations && allocated_bytes == empty_bytes);
    json_set_alloc_funcs(original_malloc, original_free);
#if defined(__linux__)
    assert(!resource_reads);
    resource_inode = 0;
#endif
    printf("lazy resources: %zu JSON allocations, %zu bytes; unchanged by 1 MiB asset\n",
        empty_allocations, empty_bytes);
    assert(!unlink(resource) && !unlink(path) && !rmdir(directory));
}

static void
test_reports_borrow_metadata_and_omit_unrequested_payloads(void)
{
    json_t *result = json_pack("{s:[{s:s,s:s,s:s}],s:i,s:i}", "entries",
        "name", "sample", "id", "sample@1234", "description", "A sample task",
        "total", 1, "remaining", 0);
    assert(result);
    void *(*allocate)(size_t);
    void (*release)(void *);
    json_get_alloc_funcs(&allocate, &release);
    json_set_alloc_funcs(counted_malloc, release);
    allocations = allocated_bytes = 0u;
    struct snag_buf report = {.max = 4096u};
    assert(snag_skills_report(&report, "list", result) == 0);
    assert(snag_buf_terminate(&report) == 0);
    assert(!allocations && !allocated_bytes);
    json_set_alloc_funcs(allocate, release);
    assert(!strcmp((char *)report.data,
        "Available skills (1 of 1):\n  sample (sample@1234) — A sample task"));
    size_t capacity = report.cap;
    json_t *entry = json_array_get(json_object_get(result, "entries"), 0u);
    char *payload = malloc(1024u * 1024u);
    assert(payload);
    memset(payload, 'x', 1024u * 1024u);
    assert(json_object_set_new(entry, "metadata",
        json_stringn(payload, 1024u * 1024u)) == 0);
    free(payload);
    snag_buf_reset(&report);
    assert(snag_skills_report(&report, "list", result) == 0);
    assert(snag_buf_terminate(&report) == 0 && report.cap == capacity);
    assert(!strstr((char *)report.data, "metadata"));
    snag_buf_free(&report);
    json_decref(result);
}

int
main(int argc, char **argv)
{
    test_reports_borrow_metadata_and_omit_unrequested_payloads();
    if (argc == 2 && !strcmp(argv[1], "--lazy-resource-cost")) {
        lazy_resource_cost();
        return 0;
    }
    assert(argc == 1);
    snapshot_checks();
    lazy_resource_cost();
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
    invocation = snag_skill_invocation(skill, "", error, sizeof(error));
    assert(invocation);
    json_decref(invocation);
    json_decref(skill);
    assert(unlink(path) == 0);
    assert(rmdir(directory) == 0);
    puts("skills: parser and literal invocation passed");
    return 0;
}
