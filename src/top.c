/* SPDX-License-Identifier: GPL-2.0-only */
#include "top.h"
#include "app.h"
#include "base.h"
#include "config.h"
#include "metrics.h"
#include "store.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

#define TOP_DEFAULT_DELAY 30u
#define TOP_MAX_DELAY 86400u
#define TOP_DEFAULT_ROWS 24u
#define TOP_DEFAULT_COLUMNS 120u

enum top_sort {
    TOP_SORT_TOTAL,
    TOP_SORT_STATE,
    TOP_SORT_TURNS,
    TOP_SORT_ID
};

struct top_options {
    const char *config_path;
    const char *dotdir;
    enum top_sort sort;
    unsigned int delay_seconds;
    bool delay_set;
    bool color_always;
    bool color_never;
};

struct top_row {
    char id[SNAG_ID_HEX_LEN + 1u];
    char model[SNAG_MODEL_MAX_BYTES];
    char provider[SNAG_CONFIG_PROVIDER_NAME_MAX + 1u];
    char turn_id[SNAG_ID_HEX_LEN + 1u];
    char state[16];
    uint64_t turns;
    uint64_t process_total;
    uint64_t session_total;
    uint64_t prefill_rate;
    uint64_t decode_rate;
    uint64_t process_pid;
    int status_rank;
    bool metrics_valid;
};

struct top_context {
    struct top_options options;
    struct snag_config config;
    struct snag_store store;
    char *dotdir;
    struct top_row *rows;
    size_t row_count;
    size_t selected;
    bool color;
};

static void
usage(void)
{
    (void)fprintf(stderr,
        "usage: snajpagent top [--config PATH] [--dotdir PATH] [--delay SECONDS]\n"
        "                    [--sort total|state|turns|id] [--color auto|always|never]\n");
}

static int
parse_uint(const char *text, unsigned int *value)
{
    char *end;
    unsigned long parsed;

    if (!text || !*text) return -1;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno || *end || parsed > UINT_MAX) return -1;
    *value = (unsigned int)parsed;
    return 0;
}

static int
parse_options(struct top_options *options, int argc, char **argv)
{
    *options = (struct top_options){.sort = TOP_SORT_TOTAL, .delay_seconds = TOP_DEFAULT_DELAY};
    for (int i = 0; i < argc; ++i) {
        const char *argument = argv[i];
        const char *value = NULL;
        unsigned int delay;

        if (!strcmp(argument, "-h") || !strcmp(argument, "--help")) {
            usage();
            return 1;
        }
        if (!strcmp(argument, "--config") || !strcmp(argument, "--dotdir") ||
            !strcmp(argument, "--delay") || !strcmp(argument, "-d") ||
            !strcmp(argument, "--sort") || !strcmp(argument, "--color")) {
            if (++i >= argc) return -1;
            value = argv[i];
        } else if (!strncmp(argument, "--config=", 9u)) {
            value = argument + 9u;
            options->config_path = value;
            continue;
        } else if (!strncmp(argument, "--dotdir=", 9u)) {
            value = argument + 9u;
            options->dotdir = value;
            continue;
        } else if (!strncmp(argument, "--delay=", 8u)) {
            value = argument + 8u;
            if (parse_uint(value, &delay) < 0 || !delay || delay > TOP_MAX_DELAY) return -1;
            options->delay_seconds = delay;
            options->delay_set = true;
            continue;
        } else if (!strncmp(argument, "--sort=", 7u)) {
            value = argument + 7u;
            if (!strcmp(value, "total")) options->sort = TOP_SORT_TOTAL;
            else if (!strcmp(value, "state")) options->sort = TOP_SORT_STATE;
            else if (!strcmp(value, "turns")) options->sort = TOP_SORT_TURNS;
            else if (!strcmp(value, "id")) options->sort = TOP_SORT_ID;
            else return -1;
            continue;
        } else if (!strncmp(argument, "--color=", 8u)) {
            value = argument + 8u;
            if (!strcmp(value, "always")) options->color_always = true;
            else if (!strcmp(value, "never")) options->color_never = true;
            else if (strcmp(value, "auto")) return -1;
            continue;
        } else if (!strcmp(argument, "--no-color")) {
            options->color_never = true;
            continue;
        } else {
            return -1;
        }
        if (!value || !*value) return -1;
        if (!strcmp(argument, "--config")) options->config_path = value;
        else if (!strcmp(argument, "--dotdir")) options->dotdir = value;
        else if (!strcmp(argument, "--delay") || !strcmp(argument, "-d")) {
            if (parse_uint(value, &delay) < 0 || !delay || delay > TOP_MAX_DELAY) return -1;
            options->delay_seconds = delay;
            options->delay_set = true;
        } else if (!strcmp(argument, "--sort")) {
            if (!strcmp(value, "total")) options->sort = TOP_SORT_TOTAL;
            else if (!strcmp(value, "state")) options->sort = TOP_SORT_STATE;
            else if (!strcmp(value, "turns")) options->sort = TOP_SORT_TURNS;
            else if (!strcmp(value, "id")) options->sort = TOP_SORT_ID;
            else return -1;
        } else if (!strcmp(argument, "--color")) {
            if (!strcmp(value, "always")) options->color_always = true;
            else if (!strcmp(value, "never")) options->color_never = true;
            else if (strcmp(value, "auto")) return -1;
        }
    }
    if (options->color_always && options->color_never) return -1;
    return 0;
}

static void
copy_text(char *out, size_t size, const char *text)
{
    if (!text) text = "";
    (void)snag_strcpy(out, size, text);
}

static const char *
status_name(int status)
{
    static const char *const names[] = {"attached", "detached", "running", "stored"};
    return status >= 0 && status < 4 ? names[status] : "stored";
}

static int
compare_rows(const struct top_row *a, const struct top_row *b, const struct top_options *options)
{
    int order = 0;

    if (a->status_rank != b->status_rank)
        return a->status_rank < b->status_rank ? -1 : 1;
    if (options->sort == TOP_SORT_STATE) order = strcmp(a->state, b->state);
    else if (options->sort == TOP_SORT_TURNS)
        order = a->turns == b->turns ? 0 : a->turns > b->turns ? -1 : 1;
    else if (options->sort == TOP_SORT_TOTAL)
        order = a->process_total == b->process_total ? 0 : a->process_total > b->process_total ? -1 : 1;
    else order = strcmp(a->id, b->id);
    if (order) return order;
    return strcmp(a->id, b->id);
}

static void
sort_rows(struct top_row *rows, size_t count, const struct top_options *options)
{
    for (size_t i = 1u; i < count; ++i) {
        struct top_row current = rows[i];
        size_t position = i;

        while (position && compare_rows(&current, &rows[position - 1u], options) < 0) {
            rows[position] = rows[position - 1u];
            --position;
        }
        rows[position] = current;
    }
}

static int
load_rows(struct top_context *context, char *error, size_t error_size)
{
    json_t *catalog = NULL;
    size_t count;
    struct top_row *rows = NULL;

    catalog = snag_store_catalog(&context->store, NULL, UINT64_MAX, NULL, NULL, error, error_size);
    if (!catalog) return -1;
    count = json_array_size(catalog);
    if (count) {
        rows = calloc(count, sizeof(*rows));
        if (!rows) {
            json_decref(catalog);
            return snag_fail(error, error_size, ENOMEM, "cannot allocate top rows");
        }
    }
    for (size_t i = 0u; i < count; ++i) {
        json_t *item = json_array_get(catalog, i);
        json_t *cells = json_object_get(item, "cells");
        const char *id = snag_json_string(item, "id");
        json_t *rank = json_object_get(item, "status_rank");
        struct snag_metrics_view view;
        char metrics_error[256] = {0};

        if (!id || !snag_hex_is_lower(id, SNAG_ID_HEX_LEN) || !json_is_array(cells) ||
            json_array_size(cells) < 5u || !json_is_integer(rank)) {
            free(rows);
            json_decref(catalog);
            return snag_fail(error, error_size, EINVAL, "session catalog contains an invalid row");
        }
        copy_text(rows[i].id, sizeof(rows[i].id), id);
        rows[i].status_rank = (int)json_integer_value(rank);
        copy_text(rows[i].state, sizeof(rows[i].state), status_name(rows[i].status_rank));
        copy_text(rows[i].model, sizeof(rows[i].model),
            json_string_value(json_array_get(cells, 2u)));
        if (json_is_integer(json_array_get(cells, 3u)))
            rows[i].turns = (uint64_t)json_integer_value(json_array_get(cells, 3u));
        if (snag_metrics_read(context->store.root_fd, id, &view, metrics_error,
                sizeof(metrics_error)) == 0 && view.valid) {
            rows[i].metrics_valid = true;
            copy_text(rows[i].provider, sizeof(rows[i].provider), view.provider);
            if (view.model[0]) copy_text(rows[i].model, sizeof(rows[i].model), view.model);
            copy_text(rows[i].turn_id, sizeof(rows[i].turn_id), view.active_turn_id);
            rows[i].turns = view.turns;
            rows[i].process_total = view.process_total_tokens;
            rows[i].session_total = view.session_total_tokens;
            rows[i].prefill_rate = view.prefill_microtokens_per_second;
            rows[i].decode_rate = view.decode_microtokens_per_second;
            rows[i].process_pid = view.process_pid;
        }
    }
    json_decref(catalog);
    sort_rows(rows, count, &context->options);
    free(context->rows);
    context->rows = rows;
    context->row_count = count;
    if (context->selected >= count && count) context->selected = count - 1u;
    if (!count) context->selected = 0u;
    return 0;
}

static void
short_id(char out[9], const char *id)
{
    memcpy(out, id, 8u);
    out[8] = '\0';
}

static void
rate_text(char out[32], uint64_t microtokens)
{
    (void)snprintf(out, 32u, "%llu.%02llu",
        (unsigned long long)(microtokens / 1000000u),
        (unsigned long long)((microtokens / 10000u) % 100u));
}

static int
append_row(struct snag_buf *out, const struct top_row *row, bool selected, bool color)
{
    char id[9], turn[9], process[32], session[32], prefill[32], decode[32];

    short_id(id, row->id);
    short_id(turn, row->turn_id[0] ? row->turn_id : "--------");
    snag_format_count(process, sizeof(process), row->process_total);
    snag_format_count(session, sizeof(session), row->session_total);
    rate_text(prefill, row->prefill_rate);
    rate_text(decode, row->decode_rate);
    if (color && selected && snag_buf_append(out, "\033[7m", 4u) < 0) return -1;
    if (snag_buf_printf(out, "%-8s %-8s %-20.20s %6llu %-8s %8s %8s %9s %9s",
            id, row->state, row->model, (unsigned long long)row->turns, turn, process,
            session, prefill, decode) < 0)
        return -1;
    if (color && selected && snag_buf_append(out, "\033[0m", 4u) < 0) return -1;
    return snag_buf_putc(out, '\n');
}

static int
render(struct top_context *context, bool interactive, const char *message)
{
    struct snag_buf out = {.max = 128u * 1024u};
    unsigned int height = TOP_DEFAULT_ROWS;
    unsigned int width = TOP_DEFAULT_COLUMNS;
    uint64_t process_total = 0u, session_total = 0u, turns = 0u;
    size_t shown;
    char process[32], session[32];

#ifndef _WIN32
    if (interactive) {
        struct winsize size;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) {
            if (size.ws_row) height = size.ws_row;
            if (size.ws_col) width = size.ws_col;
        }
    }
#else
    (void)interactive;
#endif
    if (interactive && snag_buf_append(&out, "\033[2J\033[H", 7u) < 0) goto out;
    if (snag_buf_printf(&out, "snajpagent top  delay=%us  arrows/jk move  x kill  q quit",
            context->options.delay_seconds) < 0)
        goto out;
    if (message && *message && snag_buf_printf(&out, "  %s", message) < 0) goto out;
    {
        const char header[] =
            "SESSION  STATE    MODEL                TURNS ACTIVE   PROCESS  SESSION   PREFILL/s  DECODE/s\n";
        if (snag_buf_putc(&out, '\n') < 0 ||
            snag_buf_append(&out, header, sizeof(header) - 1u) < 0)
            goto out;
    }
    shown = context->row_count;
    if (interactive && height > 4u && shown > height - 4u) shown = height - 4u;
    for (size_t i = 0u; i < context->row_count; ++i) {
        turns += context->rows[i].turns;
        if (UINT64_MAX - process_total >= context->rows[i].process_total)
            process_total += context->rows[i].process_total;
        else process_total = UINT64_MAX;
        if (UINT64_MAX - session_total >= context->rows[i].session_total)
            session_total += context->rows[i].session_total;
        else session_total = UINT64_MAX;
        if (i < shown && append_row(&out, &context->rows[i], i == context->selected, context->color) < 0)
            goto out;
    }
    snag_format_count(process, sizeof(process), process_total);
    snag_format_count(session, sizeof(session), session_total);
    if (interactive) {
        size_t lines = 2u + shown;
        while (lines + 1u < height) {
            if (snag_buf_putc(&out, '\n') < 0) goto out;
            ++lines;
        }
    }
    if (context->color && snag_buf_append(&out, "\033[7m", 4u) < 0) goto out;
    if (snag_buf_printf(&out, "TOTAL sessions=%zu turns=%llu process=%s session=%s",
            context->row_count, (unsigned long long)turns, process, session) < 0)
        goto out;
    if (context->color && snag_buf_append(&out, "\033[0m", 4u) < 0) goto out;
    if (snag_buf_putc(&out, '\n') < 0 || snag_write_full(STDOUT_FILENO, out.data, out.len) < 0) goto out;
    snag_buf_free(&out);
    (void)width;
    return 0;
out:
    snag_buf_free(&out);
    return -1;
}

#ifndef _WIN32
static int
enter_raw(struct termios *saved)
{
    struct termios raw;

    if (tcgetattr(STDIN_FILENO, saved) < 0) return -1;
    raw = *saved;
    raw.c_iflag &= (tcflag_t)~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_lflag &= (tcflag_t)~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    raw.c_cflag = (raw.c_cflag & (tcflag_t)~(CSIZE | PARENB)) | CS8;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    return tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static int
read_key(void)
{
    unsigned char key;
    struct pollfd input = {STDIN_FILENO, POLLIN, 0};

    if (read(STDIN_FILENO, &key, 1u) != 1) return -1;
    if (key != '\033') return key;
    input.events = POLLIN;
    if (poll(&input, 1u, 50) <= 0) return 27;
    unsigned char sequence[2];
    if (read(STDIN_FILENO, sequence, sizeof(sequence)) != (ssize_t)sizeof(sequence)) return 27;
    if (sequence[0] != '[') return 27;
    if (sequence[1] == 'A') return 1001;
    if (sequence[1] == 'B') return 1002;
    return 27;
}

static void
move_selection(struct top_context *context, int direction)
{
    if (!context->row_count) return;
    if (direction < 0) {
        if (context->selected) --context->selected;
        else context->selected = context->row_count - 1u;
    } else if (context->selected + 1u < context->row_count) {
        ++context->selected;
    } else context->selected = 0u;
}

static int
kill_selected(struct top_context *context, char *message, size_t message_size)
{
    struct top_row *row;
    struct snag_session session;
    struct snag_metrics_view view;
    char error[256] = {0};

    if (!context->row_count) return 0;
    row = &context->rows[context->selected];
    if (row->status_rank >= 3) {
        (void)snag_strcpy(message, message_size, "stored sessions cannot be killed");
        return 0;
    }
    if (!row->metrics_valid || !row->process_pid) {
        (void)snag_strcpy(message, message_size, "selected session has no live metrics owner");
        return 0;
    }
    snag_session_init(&session);
    if (snag_session_locate(&context->store, &session, row->id, NULL, NULL, error,
            sizeof(error)) < 0 || !snag_session_is_live(&session)) {
        snag_session_close(&session);
        (void)snag_strcpy(message, message_size, "selected session is no longer live");
        return 0;
    }
    view = (struct snag_metrics_view){.valid = true, .process_pid = row->process_pid};
    if (snag_metrics_kill(&view, error, sizeof(error)) < 0)
        (void)snag_strcpy(message, message_size, error[0] ? error : "cannot kill selected session");
    else (void)snag_strcpy(message, message_size, "termination requested");
    snag_session_close(&session);
    return 0;
}
#endif

int
snag_top_main(int argc, char **argv)
{
    struct top_context context;
    char error[256] = {0};
    int option_rc;
    int rc = 2;

    memset(&context, 0, sizeof(context));
    snag_config_init(&context.config);
    snag_store_init(&context.store);
    option_rc = parse_options(&context.options, argc, argv);
    if (option_rc > 0) {
        snag_config_free(&context.config);
        return 0;
    }
    if (option_rc < 0) {
        usage();
        snag_config_free(&context.config);
        return 2;
    }
    context.dotdir = snag_app_dotdir(context.options.dotdir, error, sizeof(error));
    if (!context.dotdir || snag_config_load(&context.config, context.options.config_path,
            context.dotdir, error, sizeof(error)) < 0 ||
        snag_store_open(&context.store, context.dotdir, error, sizeof(error)) < 0)
        goto out;
    if (!context.options.delay_set && context.config.metrics_refresh_seconds)
        context.options.delay_seconds = context.config.metrics_refresh_seconds;
#ifndef _WIN32
    context.color = context.options.color_always ||
        (!context.options.color_never && isatty(STDOUT_FILENO));
#else
    context.color = false;
#endif
    if (load_rows(&context, error, sizeof(error)) < 0) goto out;
#ifdef _WIN32
    if (render(&context, false, NULL) < 0) goto out;
    rc = 0;
#else
    {
        bool interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
        char message[256] = {0};
        struct termios saved;
        bool confirm = false;
        bool raw = false;

        if (!interactive) {
            if (render(&context, false, NULL) < 0) goto out;
            rc = 0;
        } else if (enter_raw(&saved) < 0) {
            (void)snag_errorf(error, sizeof(error), "cannot enter top terminal mode: %s", strerror(errno));
            goto out;
        } else {
            raw = true;
            (void)write(STDOUT_FILENO, "\033[?25l", 6u);
            for (;;) {
                int key;
                struct pollfd input = {STDIN_FILENO, POLLIN, 0};
                if (load_rows(&context, error, sizeof(error)) < 0) break;
                if (render(&context, true, message) < 0) break;
                message[0] = '\0';
                if (poll(&input, 1u, context.options.delay_seconds > INT_MAX / 1000
                        ? INT_MAX : (int)(context.options.delay_seconds * 1000u)) < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (!input.revents) continue;
                key = read_key();
                if (key == 'q' || key == 'Q' || key == 3) {
                    rc = 0;
                    break;
                }
                if (confirm) {
                    if (key == 'y' || key == 'Y') {
                        (void)kill_selected(&context, message, sizeof(message));
                        confirm = false;
                    } else if (key == 'n' || key == 'N' || key == 27) {
                        (void)snag_strcpy(message, sizeof(message), "kill cancelled");
                        confirm = false;
                    }
                } else if (key == 1001 || key == 'k') move_selection(&context, -1);
                else if (key == 1002 || key == 'j') move_selection(&context, 1);
                else if (key == 'r' || key == 'R') continue;
                else if (key == 'x' || key == 'X') {
                    if (context.row_count && context.rows[context.selected].status_rank >= 3)
                        (void)snag_strcpy(message, sizeof(message),
                            "stored sessions cannot be killed");
                    else {
                        (void)snag_strcpy(message, sizeof(message),
                            "kill selected session? press y/n");
                        confirm = true;
                    }
                }
            }
            if (raw) {
                (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved);
                (void)write(STDOUT_FILENO, "\033[0m\033[?25h\n", 12u);
            }
        }
    }
#endif
out:
    if (rc != 0 && error[0]) (void)fprintf(stderr, "snajpagent top: %s\n", error);
    free(context.rows);
    snag_store_close(&context.store);
    free(context.dotdir);
    snag_config_free(&context.config);
    return rc;
}
