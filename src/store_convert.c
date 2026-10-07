/* SPDX-License-Identifier: GPL-2.0-only */
#include "store_convert.h"
#include "app.h"
#include "base.h"
#include "context.h"
#include "fs.h"
#include "store_binary_context.h"
#include "store_binary_import.h"
#include "store_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

enum conversion_outcome { CONVERTED, CURRENT, SKIPPED, FAILED };

struct conversion_pool {
    struct snag_store *store;
    struct snag_directory *directory;
    pthread_mutex_t lock;
    uint64_t counts[4];
    bool exhausted;
    bool enumeration_failed;
};

static atomic_bool conversion_stopped;

static void
conversion_signal(int signal_number)
{
    (void)signal_number;
    atomic_store_explicit(&conversion_stopped, true, memory_order_relaxed);
}

static bool
conversion_cancelled(void *opaque)
{
    (void)opaque;
    return atomic_load_explicit(&conversion_stopped, memory_order_relaxed);
}

static size_t
conversion_processors(void)
{
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwNumberOfProcessors ? info.dwNumberOfProcessors : 1u;
#else
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (size_t)count : 1u;
#endif
}

static struct snag_directory *
conversion_directory(struct snag_store *store, char *error, size_t size)
{
    int fd = snag_open_read_security_at(store->root_fd, "sessions", true);
    if (fd < 0) return NULL;
    if (snag_store_verify_private_fd(fd, true, "sessions directory", error, size) < 0) {
        (void)close(fd);
        return NULL;
    }
    struct snag_directory *directory = snag_directory_open(fd);
    if (!directory) (void)close(fd);
    return directory;
}

static bool
conversion_id(const char *name)
{
    return strlen(name) == SNAG_ID_HEX_LEN && snag_hex_is_lower(name, SNAG_ID_HEX_LEN);
}

static int
conversion_count(struct snag_store *store, size_t *count, char *error, size_t size)
{
    struct snag_directory *directory = conversion_directory(store, error, size);
    if (!directory) return -1;
    *count = 0u;
    const char *name;
    int rc = 0;
    while ((name = snag_directory_next(directory)) != NULL) {
        if (!conversion_id(name)) continue;
        if (*count == SIZE_MAX) {
            rc = snag_fail(error, size, EOVERFLOW, "conversion directory count overflow");
            break;
        }
        ++*count;
    }
    if (!name && errno) {
        rc = snag_errorf(error, size, "cannot enumerate sessions: %s", strerror(errno));
    }
    int saved = errno;
    (void)snag_directory_close(directory);
    errno = saved;
    return rc;
}

static bool
conversion_next(struct conversion_pool *pool, char id[SNAG_ID_HEX_LEN + 1u])
{
    bool available = false;
    (void)pthread_mutex_lock(&pool->lock);
    while (!pool->exhausted && !conversion_cancelled(NULL)) {
        const char *name = snag_directory_next(pool->directory);
        if (!name) {
            pool->exhausted = true;
            if (errno) {
                pool->enumeration_failed = true;
                (void)fprintf(stderr, "convert: cannot enumerate sessions: %s\n", strerror(errno));
            }
            break;
        }
        if (!conversion_id(name)) continue;
        memcpy(id, name, SNAG_ID_HEX_LEN + 1u);
        (void)printf("%s checking\n", id);
        (void)fflush(stdout);
        available = true;
        break;
    }
    (void)pthread_mutex_unlock(&pool->lock);
    return available;
}

static enum conversion_outcome
conversion_session(struct snag_store *store, const char *id, char *error, size_t size)
{
    struct snag_session source;
    snag_session_init(&source);
    struct snag_binary_import_result result = {0};
    struct snag_context_control control = {.cancelled = conversion_cancelled};
    enum conversion_outcome outcome = FAILED;
    if (snag_store_open_session_directory(store, &source, id, error, size) < 0) goto done;
    int format = snag_store_open_session_files(&source, false, error, size);
    if (format < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EBUSY) {
            outcome = SKIPPED;
            (void)snag_errorf(error, size, "locked");
            goto done;
        }
        if (errno != ENOENT || source.lock_fd < 0 || source.log_fd >= 0) goto done;
        /* The original lock still guards an interrupted publication gap. */
        int retained = snag_open_read_security_at(source.dir_fd, ".legacy-source", true);
        if (retained < 0) goto done;
        if (snag_store_verify_private_fd(retained, true, "retained legacy directory",
                error, size) < 0) {
            (void)close(retained);
            goto done;
        }
        source.log_fd = snag_open_read_security_at(retained, "events.jsonl", false);
        int saved = errno;
        (void)close(retained);
        errno = saved;
        if (source.log_fd < 0 || snag_store_verify_private_fd(source.log_fd, false,
                "retained legacy journal", error, size) < 0) goto done;
        snag_file_info info;
        if (snag_fstat(source.log_fd, &info) < 0) goto done;
        source.log_end = info.st_size;
        format = 0;
    }
    if (conversion_cancelled(NULL)) {
        outcome = SKIPPED;
        (void)snag_errorf(error, size, "cancelled");
        goto done;
    }
    if (format) {
        snag_file_info info;
        if (snag_fstat(source.log_fd, &info) < 0) goto done;
        /* Immutable admission keeps tail bytes; extent equality below rejects
         * incomplete native journals without truncation or cache writes. */
        source.snapshot_read_only = true;
        if (snag_store_load_binary_session(&source, SNAG_TAIL_IGNORE, error, size) < 0) goto done;
        if (source.log_end != info.st_size) {
            (void)snag_fail(error, size, EBADMSG, "native journal has an unverified tail");
            goto done;
        }
        outcome = CURRENT;
    } else if (!snag_store_convert_binary_directory(&source, &result, &control, error, size)) {
        outcome = CONVERTED;
        if (result.legacy.incomplete_tail_bytes) {
            (void)snag_errorf(error, size, "unsealed source tail retained");
        }
    } else if (errno == ECANCELED) {
        outcome = SKIPPED;
        (void)snag_errorf(error, size, "cancelled");
    }
done:
    if (outcome == FAILED && !error[0]) {
        (void)snag_errorf(error, size, "%s", strerror(errno ? errno : EIO));
    }
    snag_binary_checkpoint_sources_free(&result.sources);
    snag_session_close(&source);
    return outcome;
}

static void *
conversion_worker(void *opaque)
{
    struct conversion_pool *pool = opaque;
    char id[SNAG_ID_HEX_LEN + 1u];
    while (conversion_next(pool, id)) {
        char error[512] = {0};
        enum conversion_outcome outcome = conversion_session(pool->store, id, error, sizeof(error));
        const char *names[4] = {"converted", "already-current", "skipped", "failed"};
        (void)pthread_mutex_lock(&pool->lock);
        ++pool->counts[outcome];
        (void)printf("%s %s%s%s\n", id, names[outcome], error[0] ? ": " : "", error);
        (void)fflush(stdout);
        (void)pthread_mutex_unlock(&pool->lock);
    }
    return NULL;
}

static void
conversion_usage(FILE *stream)
{
    (void)fprintf(stream, "Usage: snajpagent convert [--dotdir DIR] [--jobs N]\n"
        "Convert stopped saved sessions to native storage, retaining original journals.\n"
        "Default jobs: online processors, bounded by discovered session count.\n");
}

int
snag_convert_main(int argc, char **argv)
{
    const char *directory = NULL;
    uint64_t requested = conversion_processors();
    char error[512] = {0};
    for (int i = 0; i < argc; ++i) {
        const char *argument = argv[i];
        if (!strcmp(argument, "--help") || !strcmp(argument, "-h")) {
            conversion_usage(stdout);
            return 0;
        }
        if (!strcmp(argument, "--dotdir") && i + 1 < argc) {
            directory = argv[++i];
        } else if (!strncmp(argument, "--dotdir=", 9u)) {
            directory = argument + 9u;
        } else {
            const char *jobs = NULL;
            if (!strcmp(argument, "--jobs") && i + 1 < argc) jobs = argv[++i];
            else if (!strncmp(argument, "--jobs=", 7u)) jobs = argument + 7u;
            if (!jobs || snag_parse_count(jobs, &requested) < 0 || !requested) {
                (void)fprintf(stderr, "convert: expected --dotdir DIR or positive --jobs N\n");
                conversion_usage(stderr);
                return 2;
            }
        }
    }
    char *dotdir = snag_app_dotdir(directory, error, sizeof(error));
    struct snag_store store;
    snag_store_init(&store);
    struct conversion_pool pool = {.store = &store};
    size_t count = 0u;
    int rc = 3;
    if (!dotdir || snag_store_open(&store, dotdir, error, sizeof(error)) < 0 ||
        conversion_count(&store, &count, error, sizeof(error)) < 0) goto done;
    size_t workers = requested < count ? (size_t)requested : count;
    pool.directory = conversion_directory(&store, error, sizeof(error));
    if (!pool.directory) goto done;
    int code = pthread_mutex_init(&pool.lock, NULL);
    if (code) {
        (void)snag_errorf(error, sizeof(error),
            "cannot initialize conversion pool: %s", strerror(code));
        goto done;
    }
    pthread_t *threads = NULL;
    size_t started = 0u;
    bool startup_failed = false;
    if (workers > 1u) {
        if (workers - 1u > SIZE_MAX / sizeof(*threads) ||
            !(threads = calloc(workers - 1u, sizeof(*threads)))) {
            (void)snag_errorf(error, sizeof(error), "cannot allocate conversion workers");
            (void)pthread_mutex_destroy(&pool.lock);
            goto done;
        }
    }
    atomic_store_explicit(&conversion_stopped, false, memory_order_relaxed);
    void (*interrupt)(int) = SIG_ERR;
    void (*terminate)(int) = SIG_ERR;
    /* C permits signal-handler atomic operations only on lock-free objects.
     * Other platforms retain ordinary termination and the same crash repair. */
    if (atomic_is_lock_free(&conversion_stopped)) {
        interrupt = signal(SIGINT, conversion_signal);
        terminate = signal(SIGTERM, conversion_signal);
    }
    (void)printf("Conversion workers: %zu\n", workers);
    for (size_t i = 1u; i < workers; ++i) {
        code = pthread_create(&threads[started], NULL, conversion_worker, &pool);
        if (code) {
            startup_failed = true;
            (void)fprintf(stderr, "convert: cannot start another worker: %s\n", strerror(code));
            break;
        }
        ++started;
    }
    (void)conversion_worker(&pool);
    for (size_t i = 0u; i < started; ++i) (void)pthread_join(threads[i], NULL);
    if (interrupt != SIG_ERR) (void)signal(SIGINT, interrupt);
    if (terminate != SIG_ERR) (void)signal(SIGTERM, terminate);
    (void)printf("Converted: %" PRIu64 "; already-current: %" PRIu64
        "; skipped: %" PRIu64 "; failed: %" PRIu64 "%s\n",
        pool.counts[CONVERTED], pool.counts[CURRENT], pool.counts[SKIPPED], pool.counts[FAILED],
        conversion_cancelled(NULL) ? "; interrupted" : "");
    rc = conversion_cancelled(NULL) ? 130 :
        (pool.counts[FAILED] || pool.enumeration_failed || startup_failed ? 1 : 0);
    free(threads);
    (void)pthread_mutex_destroy(&pool.lock);
done:
    if (rc == 3) (void)fprintf(stderr, "convert: %s\n", error[0] ? error : strerror(errno));
    if (pool.directory) (void)snag_directory_close(pool.directory);
    snag_store_close(&store);
    free(dotdir);
    return rc;
}
