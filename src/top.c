/* SPDX-License-Identifier: GPL-2.0-only */
#include "top.h"
#include "app.h"
#include "base.h"
#include "config.h"
#include "fs.h"
#include "metrics.h"
#include "store.h"
#include "term.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

#define TOP_DEFAULT_DELAY 200u
#define TOP_MAX_DELAY 86400u
#define TOP_DEFAULT_ROWS 24u
#define TOP_DEFAULT_COLUMNS 120u

enum top_sort {
    TOP_SORT_ID, TOP_SORT_STATE, TOP_SORT_MODEL, TOP_SORT_TURNS, TOP_SORT_ACTIVE,
    TOP_SORT_TOTAL, TOP_SORT_SESSION, TOP_SORT_DECODE
};

static const char *const field_names[] = {
    "SESSION", "STATE", "MODEL", "TURNS", "ACTIVE", "PROCESS", "SESSION", "DECODE/s"};
static const unsigned int field_widths[] = {8u, 8u, 20u, 6u, 8u, 8u, 8u, 9u};

enum top_screen {TOP_TABLE, TOP_HELP, TOP_FIELDS, TOP_DELAY, TOP_WIDTH, TOP_PID, TOP_SIGNAL};

struct top_options {
    const char *dotdir, *config;
    struct snag_top_settings settings;
    unsigned int specified; /* Explicit delay, sort and color override saved preferences. */
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
    uint64_t decode_rate;
    uint64_t process_pid;
    uint64_t process_started_ms;
    int status_rank;
    bool metrics_valid;
};

struct top_context {
    struct top_options options;
    struct snag_store store;
    char *dotdir;
    struct top_row *rows;
    size_t row_count;
    size_t selected;
    bool color;
    enum top_screen screen;
    unsigned int widths[SNAG_TOP_FIELDS];
    size_t field_selected, edit_len;
    bool field_moving;
    char edit[128];
    char config_path[SNAG_CONFIG_PATH_MAX + 1u];
    bool cache_only;
    char catalog_pending[SNAG_SHA256_HEX_LEN + 1u];
#ifndef _WIN32
    pid_t catalog_pid;
    char catalog_attempt[SNAG_SHA256_HEX_LEN + 1u];
    bool catalog_failed;
#endif
};

static void
usage(void)
{
    (void)fprintf(stderr,
        "usage: snajpagent top [--config PATH] [--dotdir PATH] [--delay SECONDS]\n"
        "                    [--sort total|state|turns|id] [--color auto|always|never]\n");
}

static int
parse_delay(const char *text, unsigned int *value)
{
    uint64_t seconds = 0u;
    unsigned int fraction = 0u;
    unsigned int scale = 100u;
    if (!text || !*text) return -1;
    while (*text >= '0' && *text <= '9') {
        seconds = seconds * 10u + (unsigned int)(*text++ - '0');
        if (seconds > TOP_MAX_DELAY) return -1;
    }
    if (*text == '.') {
        ++text;
        if (!*text) return -1;
        while (*text >= '0' && *text <= '9' && scale) {
            fraction += (unsigned int)(*text++ - '0') * scale;
            scale /= 10u;
        }
    }
    uint64_t milliseconds = seconds * 1000u + fraction;
    if (*text || milliseconds < 200u || milliseconds > TOP_MAX_DELAY * 1000u) return -1;
    *value = (unsigned int)milliseconds;
    return 0;
}

static int
parse_options(struct top_options *options, int argc, char **argv)
{
    *options = (struct top_options){.settings = {.sort = TOP_SORT_TOTAL,
        .delay_ms = TOP_DEFAULT_DELAY, .visible = (1u << SNAG_TOP_FIELDS) - 1u,
        .fields = "01234567"}};
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
            if (!*value) return -1;
            options->config = value;
            continue;
        } else if (!strncmp(argument, "--dotdir=", 9u)) {
            value = argument + 9u;
            options->dotdir = value;
            continue;
        } else if (!strncmp(argument, "--delay=", 8u)) {
            value = argument + 8u;
            if (parse_delay(value, &delay) < 0) return -1;
            options->settings.delay_ms = delay;
            options->specified |= 1u;
            continue;
        } else if (!strncmp(argument, "--sort=", 7u)) {
            options->specified |= 2u;
            value = argument + 7u;
            if (!strcmp(value, "total")) options->settings.sort = TOP_SORT_TOTAL;
            else if (!strcmp(value, "state")) options->settings.sort = TOP_SORT_STATE;
            else if (!strcmp(value, "turns")) options->settings.sort = TOP_SORT_TURNS;
            else if (!strcmp(value, "id")) options->settings.sort = TOP_SORT_ID;
            else return -1;
            continue;
        } else if (!strncmp(argument, "--color=", 8u)) {
            options->specified |= 4u;
            value = argument + 8u;
            if (!strcmp(value, "always")) options->settings.color = 1u;
            else if (!strcmp(value, "never")) options->settings.color = 2u;
            else if (!strcmp(value, "auto")) options->settings.color = 0u;
            else return -1;
            continue;
        } else if (!strcmp(argument, "--no-color")) {
            options->specified |= 4u;
            options->settings.color = 2u;
            continue;
        } else {
            return -1;
        }
        if (!value || !*value) return -1;
        if (!strcmp(argument, "--config")) options->config = value;
        else if (!strcmp(argument, "--dotdir")) options->dotdir = value;
        else if (!strcmp(argument, "--delay") || !strcmp(argument, "-d")) {
            if (parse_delay(value, &delay) < 0) return -1;
            options->settings.delay_ms = delay;
            options->specified |= 1u;
        } else if (!strcmp(argument, "--sort")) {
            options->specified |= 2u;
            if (!strcmp(value, "total")) options->settings.sort = TOP_SORT_TOTAL;
            else if (!strcmp(value, "state")) options->settings.sort = TOP_SORT_STATE;
            else if (!strcmp(value, "turns")) options->settings.sort = TOP_SORT_TURNS;
            else if (!strcmp(value, "id")) options->settings.sort = TOP_SORT_ID;
            else return -1;
        } else if (!strcmp(argument, "--color")) {
            options->specified |= 4u;
            if (!strcmp(value, "always")) options->settings.color = 1u;
            else if (!strcmp(value, "never")) options->settings.color = 2u;
            else if (!strcmp(value, "auto")) options->settings.color = 0u;
            else return -1;
        }
    }

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
compare_rows(const struct top_row *a, const struct top_row *b, unsigned int field)
{
    if (a->status_rank != b->status_rank)
        return a->status_rank < b->status_rank ? -1 : 1;
    int order;
    if (field == TOP_SORT_ID) order = strcmp(a->id, b->id);
    else if (field == TOP_SORT_STATE) order = strcmp(a->state, b->state);
    else if (field == TOP_SORT_MODEL) order = strcmp(a->model, b->model);
    else if (field == TOP_SORT_ACTIVE) order = strcmp(a->turn_id, b->turn_id);
    else {
        uint64_t av = field == TOP_SORT_TURNS ? a->turns :
            field == TOP_SORT_SESSION ? a->session_total :
            field == TOP_SORT_DECODE ? a->decode_rate : a->process_total;
        uint64_t bv = field == TOP_SORT_TURNS ? b->turns :
            field == TOP_SORT_SESSION ? b->session_total :
            field == TOP_SORT_DECODE ? b->decode_rate : b->process_total;
        order = av == bv ? 0 : av > bv ? -1 : 1;
    }
    return order ? order : strcmp(a->id, b->id);
}

#define COMPARE(name, field) \
static int compare_##name(const void *a, const void *b) { return compare_rows(a, b, field); }
COMPARE(id, TOP_SORT_ID)
COMPARE(state, TOP_SORT_STATE)
COMPARE(model, TOP_SORT_MODEL)
COMPARE(turns, TOP_SORT_TURNS)
COMPARE(active, TOP_SORT_ACTIVE)
COMPARE(total, TOP_SORT_TOTAL)
COMPARE(session, TOP_SORT_SESSION)
COMPARE(decode, TOP_SORT_DECODE)
#undef COMPARE

static void
sort_rows(struct top_row *rows, size_t count, const struct top_options *options)
{
    int (*const compare[])(const void *, const void *) = {compare_id, compare_state,
        compare_model, compare_turns, compare_active, compare_total, compare_session,
        compare_decode};
    qsort(rows, count, sizeof(*rows), compare[options->settings.sort]);
    if (options->settings.reverse) {
        for (size_t i = 0u; i < count / 2u; ++i) {
            struct top_row temporary = rows[i];
            rows[i] = rows[count - i - 1u];
            rows[count - i - 1u] = temporary;
        }
    }
}

static int
load_rows(struct top_context *context, char *error, size_t error_size)
{
    json_t *catalog = NULL;
    size_t count;
    struct top_row *rows = NULL;
    char selected_id[SNAG_ID_HEX_LEN + 1u] = {0};

    if (context->selected < context->row_count)
        copy_text(selected_id, sizeof(selected_id), context->rows[context->selected].id);

    catalog = snag_store_catalog_monitor(&context->store,
        context->cache_only ? context->catalog_pending : NULL, error, error_size);
    if (!catalog) return -1;
    count = json_array_size(catalog);
    /* A pending journal may have no reusable cache row. Keep the complete table
     * until verification can replace it, including after a failed refresh. */
    if (context->cache_only && context->catalog_pending[0] && context->row_count) {
        json_decref(catalog);
        return 0;
    }
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
        const char *turns = json_string_value(json_array_get(cells, 3u));
        if (turns) rows[i].turns = strtoull(turns, NULL, 10);
        if (snag_metrics_read(context->store.root_fd, id, &view, metrics_error,
                sizeof(metrics_error)) == 0 && view.valid) {
            rows[i].metrics_valid = true;
            copy_text(rows[i].provider, sizeof(rows[i].provider), view.provider);
            if (view.model[0]) copy_text(rows[i].model, sizeof(rows[i].model), view.model);
            copy_text(rows[i].turn_id, sizeof(rows[i].turn_id), view.active_turn_id);
            rows[i].turns = view.turns;
            rows[i].process_total = view.process_total_tokens;
            rows[i].session_total = view.session_total_tokens;
            rows[i].decode_rate = view.decode_microtokens_per_second;
            rows[i].process_pid = view.process_pid;
            rows[i].process_started_ms = view.process_started_ms;
        }
    }
    json_decref(catalog);
    sort_rows(rows, count, &context->options);
    free(context->rows);
    context->rows = rows;
    context->row_count = count;
    context->selected = 0u;
    for (size_t i = 0u; i < count; ++i) {
        if (!strcmp(rows[i].id, selected_id)) {
            context->selected = i;
            break;
        }
    }
    return 0;
}

static void
rate_text(char out[32], uint64_t microtokens)
{
    (void)snprintf(out, 32u, "%llu.%02llu",
        (unsigned long long)(microtokens / 1000000u),
        (unsigned long long)((microtokens / 10000u) % 100u));
}

static int
append_line(struct snag_buf *out, const struct snag_buf *line, unsigned int width,
    bool interactive, bool selected, bool color, bool last)
{
    size_t cells = 0u;

    if (color && selected && snag_buf_append(out, "\033[7m", 4u) < 0) return -1;
    for (size_t i = 0u; i < line->len;) {
        size_t end = i;
        while (end < line->len && (!interactive || end - i < width - cells) &&
            (unsigned char)line->data[end] >= 32u &&
            (unsigned char)line->data[end] < 127u) ++end;
        if (end != i) {
            if (snag_buf_append(out, line->data + i, end - i) < 0) return -1;
            cells += end - i;
            i = end;
            continue;
        }
        if (color && ((line->len - i >= 4u && !memcmp(line->data + i, "\033[1m", 4u)) ||
            (line->len - i >= 5u && !memcmp(line->data + i, "\033[22m", 5u)))) {
            size_t n = line->data[i + 2u] == '1' ? 4u : 5u;
            if (snag_buf_append(out, line->data + i, n) < 0) return -1;
            i += n;
            continue;
        }
        uint32_t cp;
        size_t bytes = snag_utf8_decode((const unsigned char *)line->data + i, line->len - i, &cp);
        const char *text = (const char *)line->data + i;
        size_t length = bytes ? bytes : 1u;

        if (bytes && (cp == '\n' || cp == '\r' || cp == '\t')) {
            text = " ";
            length = 1u;
        }
        size_t amount = snag_term_text_width(text, length);
        if (amount == SIZE_MAX) return -1;
        if (interactive && amount > width - cells) break;
        if (snag_term_append_safe(out, text, length) < 0) return -1;
        cells += amount;
        i += bytes ? bytes : 1u;
    }
    if (color && selected && snag_buf_append(out, "\033[0m", 4u) < 0) return -1;
    if (!interactive) return snag_buf_putc(out, '\n');
    return snag_buf_append(out, last ? "\r" : "\r\n", last ? 1u : 2u);
}

static void
field_text(char *text, size_t size, const struct top_row *row, unsigned int field, bool commands)
{
    switch (field) {
    case TOP_SORT_ID: snprintf(text, size, "%.8s", row->id); break;
    case TOP_SORT_STATE: snprintf(text, size, "%s", row->state); break;
    case TOP_SORT_MODEL:
        if (commands) snprintf(text, size, "snajpagent --resume %s", row->id);
        else snprintf(text, size, "%s", row->model);
        break;
    case TOP_SORT_TURNS: snprintf(text, size, "%llu", (unsigned long long)row->turns); break;
    case TOP_SORT_ACTIVE:
        snprintf(text, size, "%.8s", row->turn_id[0] ? row->turn_id : "--------");
        break;
    case TOP_SORT_TOTAL: snag_format_count(text, size, row->process_total); break;
    case TOP_SORT_SESSION: snag_format_count(text, size, row->session_total); break;
    case TOP_SORT_DECODE: rate_text(text, row->decode_rate); break;
    }
}

static int
format_row(struct snag_buf *out, const struct top_context *context,
    const struct top_row *row, unsigned int columns)
{
    const struct snag_top_settings *settings = &context->options.settings;
    size_t occupied = 0u;
    for (size_t i = 0u, shown = 0u; i < SNAG_TOP_FIELDS && occupied < columns; ++i) {
        unsigned int field = (unsigned int)(settings->fields[i] - '0');
        if (!(settings->visible & (1u << field))) continue;
        char text[SNAG_MODEL_MAX_BYTES + 64u];
        if (!row) snprintf(text, sizeof(text), "%s",
            field == TOP_SORT_MODEL && settings->commands ? "COMMAND" : field_names[field]);
        else field_text(text, sizeof(text), row, field, settings->commands);
        for (size_t n = 0u; text[n]; ++n)
            if ((unsigned char)text[n] < 32u || text[n] == 127) text[n] = ' ';
        unsigned int width = context->widths[field];
        if (width > columns) width = columns;
        occupied += width + 1u;
        size_t length = strlen(text), cells = snag_term_text_width(text, length);
        if (cells == SIZE_MAX) return -1;
        size_t keep = length, used = cells;
        bool cut = cells > width;
        if (cut) {
            keep = used = 0u;
            while (keep < length) {
                uint32_t cp;
                size_t bytes = snag_utf8_decode((const unsigned char *)text + keep,
                    length - keep, &cp);
                if (!bytes) bytes = 1u;
                size_t amount = snag_term_text_width(text + keep, bytes);
                if (amount == SIZE_MAX) return -1;
                if (used + amount >= width) break;
                keep += bytes;
                used += amount;
            }
            ++used; /* A plus sign marks the cut without splitting a code point. */
        }
        if (shown++ && snag_buf_putc(out, ' ') < 0) return -1;
        if (field == settings->sort && settings->highlight && context->color &&
            snag_buf_append(out, "\033[1m", 4u) < 0) return -1;
        bool right = field >= TOP_SORT_TURNS && field != TOP_SORT_ACTIVE;
        unsigned int padding = width - (unsigned int)used;
        if (right && snag_buf_printf(out, "%*s", (int)padding, "") < 0) return -1;
        if (snag_buf_append(out, text, keep) < 0 || (cut && snag_buf_putc(out, '+') < 0)) return -1;
        if (!right && snag_buf_printf(out, "%*s", (int)padding, "") < 0) return -1;
        if (field == settings->sort && settings->highlight && context->color &&
            snag_buf_append(out, "\033[22m", 5u) < 0) return -1;
    }
    return 0;
}

static int
render(struct top_context *context, bool interactive, const char *message)
{
    struct snag_buf out = {.max = 128u * 1024u};
    struct snag_buf line = {.max = out.max};
    unsigned int height = TOP_DEFAULT_ROWS;
    unsigned int width = TOP_DEFAULT_COLUMNS;
    uint64_t process_total = 0u, session_total = 0u, turns = 0u;
    size_t shown;
    size_t first = 0u;
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
    if (context->screen == TOP_HELP || context->screen == TOP_FIELDS) {
        const char *help[] = {"snajpagent top -- interactive commands",
            "< >     previous/next visible sort field    R reverse order",
            "h ?     help    f F fields/order/sort    x sort highlight    z color",
            "k       choose owner PID then signal (default SIGTERM); Esc cancels",
            "s d     refresh delay in seconds (0.2..86400)",
            "X       extra fixed width: 0 defaults, negative automatic growth",
            "c       model/resume-command view    W save defaults to selected config",
            "Up Down j   select session    r refresh    q quit", "Press any key to return"};
        if (context->screen == TOP_HELP) {
            for (size_t i = 0u; i < sizeof(help) / sizeof(help[0]) && i < height; ++i) {
                line.len = 0u;
                if (snag_buf_append(&line, help[i], strlen(help[i])) < 0 ||
                    append_line(&out, &line, width, true, false, false,
                        i + 1u == height) < 0) goto out;
            }
        } else {
            const char title[] = "Fields: Up/Down select; Space toggle; s sort; "
                "Right move, Left finish; q return";
            if (snag_buf_append(&line, title, sizeof(title) - 1u) < 0 ||
                append_line(&out, &line, width, true, false, false, false) < 0) goto out;
            for (size_t i = 0u; i < SNAG_TOP_FIELDS && i + 1u < height; ++i) {
                unsigned int f = (unsigned int)(context->options.settings.fields[i] - '0');
                line.len = 0u;
                if (snag_buf_printf(&line, "%c %s%s%s",
                        context->options.settings.visible & (1u << f) ? '*' : ' ', field_names[f],
                        context->options.settings.sort == f ? "  [sort]" : "",
                        i == context->field_selected && context->field_moving ?
                            "  [move]" : "") < 0 ||
                    append_line(&out, &line, width, true, i == context->field_selected,
                        context->color, i + 2u == height) < 0) goto out;
            }
        }
        int rc = snag_write_full(STDOUT_FILENO, out.data, out.len);
        snag_buf_free(&out);
        snag_buf_free(&line);
        return rc;
    }
    if (message && *message) {
        if (snag_buf_printf(&line, "snajpagent top  %s", message) < 0) goto out;
    } else if (snag_buf_printf(&line,
                   "snajpagent top  delay=%u.%03us  sort=%s%s  h help  k kill  q quit",
                   context->options.settings.delay_ms / 1000u,
                   context->options.settings.delay_ms % 1000u,
                   field_names[context->options.settings.sort],
                   context->options.settings.reverse ? " reverse" : "") < 0) {
        goto out;
    }
    if (append_line(&out, &line, width, interactive, false, false, false) < 0) goto out;
    line.len = 0u;
    if (context->options.settings.extra_width < 0) {
        for (size_t r = 0u; r < context->row_count; ++r) {
            for (unsigned int f = 0u; f < SNAG_TOP_FIELDS; ++f) {
                char text[SNAG_MODEL_MAX_BYTES + 64u];
                field_text(text, sizeof(text), &context->rows[r], f,
                    context->options.settings.commands);
                size_t cells = snag_term_text_width(text, strlen(text));
                if (cells == SIZE_MAX) goto out;
                if (cells > context->widths[f]) context->widths[f] = (unsigned int)cells;
            }
        }
    }
    if (format_row(&line, context, NULL, width) < 0 ||
        append_line(&out, &line, width, interactive, false, context->color, false) < 0) goto out;
    shown = context->row_count;
    if (interactive) {
        size_t available = height > 3u ? height - 3u : 1u;

        if (shown > available) shown = available;
        if (context->selected >= shown && shown) first = context->selected - shown + 1u;
    }
    for (size_t i = 0u; i < context->row_count; ++i) {
        turns += context->rows[i].turns;
        if (UINT64_MAX - process_total >= context->rows[i].process_total)
            process_total += context->rows[i].process_total;
        else process_total = UINT64_MAX;
        if (UINT64_MAX - session_total >= context->rows[i].session_total)
            session_total += context->rows[i].session_total;
        else session_total = UINT64_MAX;
        if (i >= first && i - first < shown) {
            line.len = 0u;
            if (format_row(&line, context, &context->rows[i], width) < 0 ||
                append_line(&out, &line, width, interactive, i == context->selected,
                    context->color, false) < 0)
                goto out;
        }
    }
    snag_format_count(process, sizeof(process), process_total);
    snag_format_count(session, sizeof(session), session_total);
    if (interactive) {
        size_t lines = 2u + shown;
        while (lines + 1u < height) {
            if (snag_buf_append(&out, "\r\n", 2u) < 0) goto out;
            ++lines;
        }
    }
    line.len = 0u;
    if (snag_buf_printf(&line, "TOTAL sessions=%zu turns=%llu process=%s session=%s",
            context->row_count, (unsigned long long)turns, process, session) < 0)
        goto out;
    if (append_line(&out, &line, width, interactive, true, context->color, true) < 0 ||
        snag_write_full(STDOUT_FILENO, out.data, out.len) < 0) goto out;
    snag_buf_free(&out);
    snag_buf_free(&line);
    return 0;
out:
    snag_buf_free(&out);
    snag_buf_free(&line);
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
    if (sequence[1] == 'C') return 1003;
    if (sequence[1] == 'D') return 1004;
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
kill_confirmed(struct top_context *context, const struct top_row *target, int signal_number,
    char *message, size_t message_size)
{
    struct snag_session session;
    struct snag_metrics_view view;
    char error[256] = {0};
    int lock_fd = -1;
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};

    if (snag_metrics_read(context->store.root_fd, target->id, &view, error,
            sizeof(error)) < 0 || !view.valid || !view.process_pid || view.process_pid > INT_MAX ||
        view.process_pid != target->process_pid ||
        view.process_started_ms != target->process_started_ms) {
        (void)snag_strcpy(message, message_size, "session owner changed; select and confirm again");
        return 0;
    }
    snag_session_init(&session);
    if (snag_session_locate(&context->store, &session, target->id, NULL, NULL, error,
            sizeof(error)) < 0) {
        snag_session_close(&session);
        (void)snag_strcpy(message, message_size, "selected session is no longer live");
        return 0;
    }
    lock_fd = snag_open_read_security_at(session.dir_fd, "lock", false);
    if (lock_fd < 0 || fcntl(lock_fd, F_GETLK, &lock) < 0 || lock.l_type != F_WRLCK ||
        lock.l_pid <= 0 || (uint64_t)lock.l_pid != view.process_pid) {
        if (lock_fd >= 0) (void)close(lock_fd);
        snag_session_close(&session);
        (void)snag_strcpy(message, message_size, "session lock owner no longer matches metrics");
        return 0;
    }
    (void)close(lock_fd);
    if (kill((pid_t)view.process_pid, signal_number) < 0)
        (void)snag_strcpy(message, message_size, strerror(errno));
    else snprintf(message, message_size, "signal %d requested", signal_number);
    snag_session_close(&session);
    return 0;
}
static int
parse_signal(const char *text)
{
    static const struct {const char *name; int number;} signals[] = {
        {"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT}, {"ILL", SIGILL},
        {"ABRT", SIGABRT}, {"FPE", SIGFPE}, {"KILL", SIGKILL}, {"SEGV", SIGSEGV},
        {"PIPE", SIGPIPE}, {"ALRM", SIGALRM}, {"TERM", SIGTERM}, {"USR1", SIGUSR1},
        {"USR2", SIGUSR2}, {"CHLD", SIGCHLD}, {"CONT", SIGCONT}, {"STOP", SIGSTOP},
        {"TSTP", SIGTSTP}, {"TTIN", SIGTTIN}, {"TTOU", SIGTTOU}, {"WINCH", SIGWINCH}};
    if (!*text) return SIGTERM;
    if (!strncmp(text, "SIG", 3u)) text += 3u;
    for (size_t i = 0u; i < sizeof(signals) / sizeof(signals[0]); ++i)
        if (!strcmp(text, signals[i].name)) return signals[i].number;
    char *end;
    long number = strtol(text, &end, 10);
    return *text && !*end && number > 0 && number < 128 ? (int)number : -1;
}

static void
sort_step(struct top_context *context, int direction)
{
    struct snag_top_settings *settings = &context->options.settings;
    size_t position = 0u;
    while (position < SNAG_TOP_FIELDS && settings->fields[position] - '0' != (int)settings->sort)
        ++position;
    while (position < SNAG_TOP_FIELDS) {
        if ((direction < 0 && !position) ||
            (direction > 0 && position + 1u == SNAG_TOP_FIELDS)) return;
        position = direction < 0 ? position - 1u : position + 1u;
        unsigned int field = (unsigned int)(settings->fields[position] - '0');
        if (settings->visible & (1u << field)) { settings->sort = field; return; }
    }
}

static void
screen_key(struct top_context *context, struct top_row *target, int key,
    char *message, size_t size)
{
    struct snag_top_settings *settings = &context->options.settings;
    if (context->screen == TOP_HELP) { context->screen = TOP_TABLE; return; }
    if (context->screen == TOP_FIELDS) {
        size_t *selected = &context->field_selected;
        if (key == 'q' || key == 27 || key == '\r' || key == '\n') context->screen = TOP_TABLE;
        else if (key == 1003) context->field_moving = true;
        else if (key == 1004) context->field_moving = false;
        else if (key == 1001 || key == 1002 || key == 'j') {
            size_t next = (*selected + SNAG_TOP_FIELDS + (key == 1001 ? -1 : 1)) % SNAG_TOP_FIELDS;
            if (context->field_moving) {
                char field = settings->fields[*selected];
                settings->fields[*selected] = settings->fields[next];
                settings->fields[next] = field;
            }
            *selected = next;
        } else if (key == ' ' || key == 'd') {
            unsigned int bit = 1u << (settings->fields[*selected] - '0');
            if (settings->visible != bit) settings->visible ^= bit;
        } else if (key == 's') settings->sort = (unsigned int)(settings->fields[*selected] - '0');
        return;
    }
    if (key == 27) {
        context->screen = TOP_TABLE;
        context->edit_len = 0u;
        target->id[0] = '\0';
        return;
    }
    if (key == 127 || key == 8) {
        if (context->edit_len) context->edit[--context->edit_len] = '\0';
        return;
    }
    if (key != '\r' && key != '\n') {
        if (key >= 32 && key < 127 && context->edit_len + 1u < sizeof(context->edit)) {
            context->edit[context->edit_len++] = (char)key;
            context->edit[context->edit_len] = '\0';
        }
        return;
    }
    if (context->screen == TOP_DELAY) {
        unsigned int delay;
        if (!parse_delay(context->edit, &delay)) settings->delay_ms = delay;
        else copy_text(message, size, "delay must be 0.2..86400 seconds");
    } else if (context->screen == TOP_WIDTH) {
        char *end;
        errno = 0;
        long n = strtol(context->edit, &end, 10);
        if (*context->edit && !*end && !errno && n <= INT_MAX && n >= INT_MIN) {
            settings->extra_width = n < 0 ? -1 : (int)n;
            if (n >= 0) for (size_t i = 0u; i < SNAG_TOP_FIELDS; ++i)
                context->widths[i] = field_widths[i] + (unsigned int)n;
        } else copy_text(message, size,
            "extra width must be an integer; negative enables auto");
    } else if (context->screen == TOP_PID) {
        char *end;
        errno = 0;
        long pid = strtol(context->edit, &end, 10);
        bool use_default = !*context->edit || (!*end && pid < 0);
        if (!use_default) {
            target->id[0] = '\0';
            if (!errno && !*end && pid > 0) {
                for (size_t i = 0u; i < context->row_count; ++i) {
                    if (context->rows[i].process_pid == (uint64_t)pid &&
                        context->rows[i].status_rank < 3)
                        *target = context->rows[i];
                }
            }
        }
        if (target->id[0] && target->metrics_valid && target->process_pid) {
            context->screen = TOP_SIGNAL;
            context->edit_len = 0u;
            context->edit[0] = '\0';
            return;
        }
        copy_text(message, size, "PID is not a verified live session owner; kill cancelled");
    } else if (context->screen == TOP_SIGNAL) {
        int number = parse_signal(context->edit);
        if (number > 0) kill_confirmed(context, target, number, message, size);
        else copy_text(message, size, "invalid or zero signal; kill cancelled");
    }
    context->screen = TOP_TABLE;
    target->id[0] = '\0';
    context->edit_len = 0u;
    context->edit[0] = '\0';
}

#endif

#ifndef _WIN32
static volatile sig_atomic_t top_signal;

static void
top_interrupted(int number)
{
    top_signal = number;
}

static int
request_catalog(struct top_context *context, char *error, size_t error_size)
{
    if (context->catalog_pid) {
        int status;
        pid_t result = waitpid(context->catalog_pid, &status, WNOHANG);
        if (!result) return 0;
        if (result < 0 && errno == EINTR) return 0;
        context->catalog_pid = 0;
        if (result < 0) return snag_errorf(error, error_size, "cannot collect catalog reader");
        context->catalog_failed = !WIFEXITED(status) || WEXITSTATUS(status) != 0;
    }
    if (!context->catalog_pending[0] ||
        !strcmp(context->catalog_pending, context->catalog_attempt)) return 0;
    copy_text(context->catalog_attempt, sizeof(context->catalog_attempt), context->catalog_pending);
    context->catalog_failed = false;
    /* main dispatches top before initializing providers or other threads. The
     * sole child owns slow verification; the monitor never waits for it to draw. */
    pid_t child = fork();
    if (child < 0) return snag_errorf(error, error_size, "cannot start catalog reader");
    if (!child) {
        (void)close(STDIN_FILENO);
        (void)close(STDOUT_FILENO);
        (void)close(STDERR_FILENO);
        int rc = snag_store_catalog_refresh(&context->store, NULL, NULL, NULL, 0u);
        _exit(rc < 0 ? 1 : 0);
    }
    context->catalog_pid = child;
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
#ifndef _WIN32
    const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGCHLD};
    const size_t signal_count = sizeof(signals) / sizeof(signals[0]);
    struct sigaction saved_signals[sizeof(signals) / sizeof(signals[0])];
    size_t installed_signals = 0u;
    top_signal = 0;
#endif

    memset(&context, 0, sizeof(context));
    snag_store_init(&context.store);
    (void)snag_text_locale_init();
    option_rc = parse_options(&context.options, argc, argv);
    if (option_rc > 0) {
        return 0;
    }
    if (option_rc < 0) {
        usage();
        return 2;
    }
    context.dotdir = snag_app_dotdir(context.options.dotdir, error, sizeof(error));
    if (!context.dotdir ||
        snag_store_open(&context.store, context.dotdir, error, sizeof(error)) < 0)
        goto out;
    const char *config = context.options.config;
    if (config) {
        if (!snag_path_root_len(config) || strlen(config) > SNAG_CONFIG_PATH_MAX) {
            snag_errorf(error, sizeof(error), "--config requires a supported absolute path");
            goto out;
        }
        memcpy(context.config_path, config, strlen(config) + 1u);
        snag_path_slashes(context.config_path);
    } else {
        int n = snprintf(context.config_path, sizeof(context.config_path),
            "%s/config.ini", context.dotdir);
        if (n < 0 || (size_t)n >= sizeof(context.config_path)) {
            snag_errorf(error, sizeof(error), "configuration path is too long");
            goto out;
        }
    }
    struct snag_top_settings defaults = {.sort = TOP_SORT_TOTAL, .delay_ms = TOP_DEFAULT_DELAY,
        .visible = (1u << SNAG_TOP_FIELDS) - 1u, .fields = "01234567"};
    if (snag_config_top(context.config_path, &defaults, error, sizeof(error)) < 0)
        goto out;
    if (context.options.specified & 1u) defaults.delay_ms = context.options.settings.delay_ms;
    if (context.options.specified & 2u) defaults.sort = context.options.settings.sort;
    if (context.options.specified & 4u) defaults.color = context.options.settings.color;
    context.options.settings = defaults;
    for (size_t i = 0u; i < SNAG_TOP_FIELDS; ++i)
        context.widths[i] = field_widths[i] +
            (defaults.extra_width > 0 ? (unsigned int)defaults.extra_width : 0u);
#ifndef _WIN32
    context.color = context.options.settings.color == 1u ||
        (context.options.settings.color != 2u && isatty(STDOUT_FILENO));
#else
    context.color = false;
#endif
#ifdef _WIN32
    if (load_rows(&context, error, sizeof(error)) < 0) goto out;
    if (render(&context, false, NULL) < 0) goto out;
    rc = 0;
#else
    {
        bool interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
        char message[256] = {0};
        struct termios saved;
        struct top_row target = {0};
        bool raw = false;

        if (!interactive) {
            if (load_rows(&context, error, sizeof(error)) < 0) goto out;
            if (render(&context, false, NULL) < 0) goto out;
            rc = 0;
        } else {
            struct sigaction action = {0};
            (void)sigemptyset(&action.sa_mask);
            for (; installed_signals < signal_count; ++installed_signals) {
                /* Keep owned children waitable even if our launcher ignored SIGCHLD. */
                action.sa_handler = signals[installed_signals] == SIGCHLD ?
                    SIG_DFL : top_interrupted;
                if (sigaction(signals[installed_signals], &action,
                        &saved_signals[installed_signals]) < 0) {
                    (void)snag_errorf(error, sizeof(error),
                        "cannot install terminal signal handler");
                    goto out;
                }
            }
            if (enter_raw(&saved) < 0) {
                (void)snag_errorf(error, sizeof(error),
                    "cannot enter top terminal mode: %s", strerror(errno));
                goto out;
            }
            raw = true;
            static const char loading[] =
                "\033[?25l\033[2J\033[Hsnajpagent top: loading sessions...\r\n";
            (void)snag_write_full(STDOUT_FILENO, loading, sizeof(loading) - 1u);
            context.cache_only = true;
            for (;;) {
                if (top_signal) { rc = 128 + top_signal; break; }
                uint64_t deadline = snag_monotonic_ms() + context.options.settings.delay_ms;
                int key;
                struct pollfd input = {STDIN_FILENO, POLLIN, 0};
                if (load_rows(&context, error, sizeof(error)) < 0) break;
                if (request_catalog(&context, error, sizeof(error)) < 0)
                    break;
                if (context.screen == TOP_DELAY)
                    snprintf(message, sizeof(message),
                        "Change delay seconds (0.2..86400): %s", context.edit);
                else if (context.screen == TOP_WIDTH)
                    snprintf(message, sizeof(message),
                        "Extra fixed width (0 default, negative auto): %s", context.edit);
                else if (context.screen == TOP_PID)
                    snprintf(message, sizeof(message), "PID to signal [default %llu]: %s",
                        (unsigned long long)target.process_pid, context.edit);
                else if (context.screen == TOP_SIGNAL)
                    snprintf(message, sizeof(message), "Signal for PID %llu [15/TERM]: %s",
                        (unsigned long long)target.process_pid, context.edit);
                const char *notice = message;
                if (!*notice && context.catalog_failed)
                    notice = "metadata refresh failed; press r to retry";
                else if (!*notice && context.catalog_pending[0] && !context.row_count)
                    notice = context.catalog_pid ? "loading metadata..." :
                        "metadata unavailable; press r to retry";
                if (render(&context, true, notice) < 0) break;
                message[0] = '\0';
                uint64_t now = snag_monotonic_ms();
                int wait_ms = deadline > now ? (int)(deadline - now) : 0;
                if (poll(&input, 1u, wait_ms) < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (!input.revents) continue;
                key = read_key();
                if (key < 0 || key == 3 ||
                    (context.screen == TOP_TABLE && (key == 'q' || key == 'Q'))) {
                    rc = 0;
                    break;
                }
                if (context.screen != TOP_TABLE) {
                    screen_key(&context, &target, key, message, sizeof(message));
                    continue;
                }
                struct snag_top_settings *settings = &context.options.settings;
                if (key == 1001) move_selection(&context, -1);
                else if (key == 1002 || key == 'j') move_selection(&context, 1);
                else if (key == 'r') context.catalog_attempt[0] = '\0';
                else if (key == '<' || key == '>') sort_step(&context, key == '<' ? -1 : 1);
                else if (key == 'R') settings->reverse = !settings->reverse;
                else if (key == 'x') settings->highlight = !settings->highlight;
                else if (key == 'c') settings->commands = !settings->commands;
                else if (key == 'z') {
                    context.color = !context.color;
                    settings->color = context.color ? 1u : 2u;
                } else if (key == 'h' || key == '?') context.screen = TOP_HELP;
                else if (key == 'f' || key == 'F') context.screen = TOP_FIELDS;
                else if (key == 'W') {
                    char detail[256] = {0};
                    if (snag_config_save_top(context.config_path, settings,
                            detail, sizeof(detail)) < 0)
                        copy_text(message, sizeof(message),
                            *detail ? detail : "cannot save top preferences");
                    else copy_text(message, sizeof(message), "top preferences saved to config");
                } else if (key == 's' || key == 'd' || key == 'X' || key == 'k') {
                    context.edit_len = 0u;
                    context.edit[0] = '\0';
                    context.screen = key == 'X' ? TOP_WIDTH : key == 'k' ? TOP_PID : TOP_DELAY;
                    if (key == 'k') {
                        target = context.row_count ?
                            context.rows[context.selected] : (struct top_row){0};
                        if (target.status_rank >= 3) {
                            copy_text(message, sizeof(message), "stored sessions cannot be killed");
                            context.screen = TOP_TABLE;
                        }
                    }
                }
            }
            if (raw) {
                (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved);
                (void)snag_write_full(STDOUT_FILENO, "\033[0m\033[?25h\n", 11u);
            }
        }
    }
#endif
out:
#ifndef _WIN32
    if (context.catalog_pid) {
        pid_t result;
        do { result = waitpid(context.catalog_pid, NULL, WNOHANG); }
        while (result < 0 && errno == EINTR);
        if (!result) {
            /* SIGCHLD stays default until collection. This unreaped PID cannot
             * be reused; only our read-only backfill child receives the signal. */
            (void)kill(context.catalog_pid, SIGKILL);
            while (waitpid(context.catalog_pid, NULL, 0) < 0 && errno == EINTR) {}
        }
    }
    while (installed_signals) {
        --installed_signals;
        (void)sigaction(signals[installed_signals], &saved_signals[installed_signals], NULL);
    }
#endif
    if (rc != 0 && error[0]) (void)fprintf(stderr, "snajpagent top: %s\n", error);
    free(context.rows);
    snag_store_close(&context.store);
    free(context.dotdir);
    return rc;
}
