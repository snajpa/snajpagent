/* SPDX-License-Identifier: GPL-2.0-only */
#include "base.h"
#include "config.h"
#include "http.h"
#include "snajpagent.h"
#include "update.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv)
{
    struct snag_config config;
    snag_config_init(&config);
    if (argc == 2 && strcmp(argv[1], "--defaults") == 0) {
        printf("%s %d %s\n", SNAJPAGENT_VERSION, config.auto_update, config.update_url);
        snag_config_free(&config);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "-V") == 0) {
        puts(SNAJPAGENT_VERSION);
        snag_config_free(&config);
        return 0;
    }
    assert(argc == 3);
    snag_wake_fd wake[2];
    assert(snag_wakeup_create(wake) == 0);
    assert(snag_http_init() == CURLE_OK);
    uint64_t start = snag_monotonic_ms();
    struct snag_update *update = snag_update_start(argv[0], argv[1], wake[1]);
    printf("running %s\n", SNAJPAGENT_VERSION);
    fflush(stdout);
    assert(snag_monotonic_ms() - start < 1000u);
    if (update) {
        int timeout = atoi(argv[2]);
        assert(snag_wakeup_wait(wake[0], timeout) >= 0);
        const char *banner = snag_update_take(update);
        if (banner) {
            fputs(banner, stderr);
            assert(snag_update_take(update) == NULL);
        }
    }
    start = snag_monotonic_ms();
    snag_update_stop(update);
    assert(snag_monotonic_ms() - start < 2000u);
    puts("old process still running " SNAJPAGENT_VERSION);
    snag_wakeup_close(wake);
    snag_config_free(&config);
    return 0;
}
