/* SPDX-License-Identifier: GPL-2.0-only */
#include "vm.h"
#include "app.h"
#include "config.h"
#include "irc_address.h"
#include "json.h"
#include "secret.h"
#include "snajpagent.h"
#include "term_host.h"
#include "vm_grid.h"
#include "vm_input.h"
#include "vm_layout.h"
#include "vm_reader.h"
#include "vm_text.h"
#include "vm_workspace.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if SNAJPAGENT_VM
enum view_kind { VIEW_SESSIONS, VIEW_WORKSPACES, VIEW_HELP };
static const char *const view_names[] = {"sessions", "workspaces", "help"};
static const char *const help_rows[] = {
    "j/k or arrows: select    gg/G: first/last    Ctrl-D/U: half page",
    "/: filter picker    R: refresh    Ctrl-L: redraw    Ctrl-Z: suspend",
    ":sessions    :workspaces    :help",
    ":split or :sp    :vsplit or :vsp    :close    :q    :qa",
    "Ctrl-W s/v: split    Ctrl-W w/h/j/k/l: focus    Ctrl-W =: equalize",
    "Ctrl-W +/-: height    Ctrl-W >/<: width    Ctrl-W q: close",
    ":workspace    :workspace name NAME    :workspace save",
    "Workspace names accept quoted text. :q in these pickers closes the window.",
    "Workspace selection restores its layout. Bracketed paste never runs commands."
};

struct vm_window {
    uint64_t id;
    enum view_kind kind;
    size_t selected, top;
    char selected_id[SNAG_ID_HEX_LEN + 1u];
    char *filter;
    struct snag_vm_rectangle rectangle;
};

struct vm {
    struct snag_store store;
    struct snag_config config;
    struct snag_secret_set secrets;
    struct snag_vm_workspace *workspace;
    struct snag_vm_reader *reader;
    struct snag_vm_layout *layout;
    struct vm_window *windows;
    size_t count, focus;
    uint64_t next_window, generation, stored_limit, save_at;
    json_t *sessions, *workspaces;
    struct snag_term_host terminal;
    struct snag_vm_grid grid;
    struct snag_vm_input input;
    struct snag_buf command, paste;
    size_t command_cursor;
    char mode, prefix;
    char message[512];
    int output;
    bool dirty, save_dirty, meaningful, quit, suspend, entering, paste_failed;
};

static volatile sig_atomic_t stopped, resized;

static void
stop_signal(int number)
{
    stopped = number;
}

static void
resize_signal(int number)
{
    (void)number;
    resized = 1;
}

static void
notice(struct vm *vm, const char *text)
{
    (void)snprintf(vm->message, sizeof(vm->message), "%s", text);
    vm->dirty = true;
}

static void
changed(struct vm *vm)
{
    vm->dirty = vm->save_dirty = vm->meaningful = true;
    vm->save_at = snag_monotonic_ms() + 1000u;
}

static void
windows_free(struct vm_window *windows, size_t count)
{
    for (size_t i = 0u; i < count; ++i) free(windows[i].filter);
    free(windows);
}

static json_t *
state_snapshot(const struct vm *vm)
{
    json_t *windows = json_array();
    if (!windows) return NULL;
    for (size_t i = 0u; i < vm->count; ++i) {
        const struct vm_window *window = &vm->windows[i];
        json_t *row = json_pack("{s:I,s:s,s:s,s:I,s:I,s:s}", "id", (json_int_t)window->id,
            "kind", view_names[window->kind], "selected", window->selected_id,
            "top", (json_int_t)window->top, "row", (json_int_t)window->selected,
            "filter", window->filter ? window->filter : "");
        if (!row || json_array_append_new(windows, row) < 0) {
            json_decref(windows);
            return NULL;
        }
    }
    json_t *layout = snag_vm_layout_json(vm->layout);
    if (!layout) {
        json_decref(windows);
        return NULL;
    }
    return json_pack("{s:i,s:I,s:o,s:o}", "v", 1,
        "focus", (json_int_t)vm->windows[vm->focus].id, "layout", layout, "windows", windows);
}

static int
state_restore(struct vm *vm, const json_t *state, char *error, size_t size)
{
    struct snag_vm_layout *layout = NULL;
    struct vm_window *windows = NULL;
    uint64_t focus_id = 0u, next = 0u;
    size_t count = json_array_size(json_object_get(state, "windows")), focus = SIZE_MAX;
    if (!snag_json_exact_keys(state, "v focus layout windows") ||
        json_integer_value(json_object_get(state, "v")) != 1 || !count ||
        snag_json_integer_u64(state, "focus", &focus_id) < 0 ||
        !(layout = snag_vm_layout_load(json_object_get(state, "layout"), error, size)) ||
        snag_vm_layout_count(layout) != count || count > SIZE_MAX / sizeof(*windows) ||
        !(windows = calloc(count, sizeof(*windows)))) goto invalid;
    for (size_t i = 0u; i < count; ++i) {
        json_t *row = json_array_get(json_object_get(state, "windows"), i);
        const char *kind = snag_json_string(row, "kind");
        const char *selected = snag_json_string(row, "selected");
        const char *filter = snag_json_string(row, "filter");
        uint64_t top, selected_row;
        struct vm_window *window = &windows[i];
        if (!snag_json_exact_keys(row, "id kind selected top row filter") ||
            snag_json_integer_u64(row, "id", &window->id) < 0 ||
            !snag_vm_layout_contains(layout, window->id) || !kind || !selected || !filter ||
            strlen(filter) != json_string_length(json_object_get(row, "filter")) ||
            strlen(selected) != json_string_length(json_object_get(row, "selected")) ||
            (*selected && (strlen(selected) != SNAG_ID_HEX_LEN ||
                          !snag_hex_is_lower(selected, SNAG_ID_HEX_LEN))) ||
            snag_json_integer_u64(row, "top", &top) < 0 || top > SIZE_MAX ||
            snag_json_integer_u64(row, "row", &selected_row) < 0 || selected_row > SIZE_MAX)
            goto invalid;
        for (size_t j = 0u; j < i; ++j) if (windows[j].id == window->id) goto invalid;
        size_t k = 0u;
        while (k < 3u && strcmp(kind, view_names[k])) ++k;
        if (k == 3u) goto invalid;
        window->kind = (enum view_kind)k;
        window->top = (size_t)top;
        window->selected = (size_t)selected_row;
        memcpy(window->selected_id, selected, strlen(selected) + 1u);
        window->filter = snag_strdup_checked(filter, SNAG_MAX_DIRECT_PROMPT);
        if (!window->filter) goto invalid;
        if (window->id == focus_id) focus = i;
        if (window->id > next) next = window->id;
    }
    if (focus == SIZE_MAX || next == INT64_MAX) goto invalid;
    windows_free(vm->windows, vm->count);
    snag_vm_layout_free(vm->layout);
    vm->windows = windows;
    vm->layout = layout;
    vm->count = count;
    vm->focus = focus;
    vm->next_window = next + 1u;
    vm->dirty = true;
    return 0;
invalid:
    windows_free(windows, windows ? count : 0u);
    snag_vm_layout_free(layout);
    return snag_fail(error, size, EINVAL, "unsupported or invalid workspace views");
}

static int
save(struct vm *vm, const char *name)
{
    if (!vm->meaningful && !name) return 0;
    json_t *state = state_snapshot(vm);
    char error[256] = "cannot save workspace";
    int rc = -1;
    if (state) {
        if (vm->workspace->dir_fd < 0) {
            rc = snag_vm_workspace_create(vm->workspace, &vm->store, name ? name : "",
                state, snag_time_ms(), error, sizeof(error));
        } else {
            if (!name) name = snag_json_string(vm->workspace->snapshot, "name");
            rc = snag_vm_workspace_save(vm->workspace, name, state,
                snag_time_ms(), error, sizeof(error));
        }
    }
    json_decref(state);
    if (rc < 0) notice(vm, error);
    else vm->save_dirty = false;
    /* Failed storage stays visible; retry on another edit or explicit save. */
    vm->save_at = 0u;
    return rc;
}

static void
refresh(struct vm *vm)
{
    char error[256] = "cannot read workspace list";
    json_t *rows = snag_vm_workspace_list(&vm->store, vm->stored_limit, error, sizeof(error));
    if (rows) {
        json_decref(vm->workspaces);
        vm->workspaces = rows;
    } else notice(vm, error);
    struct snag_vm_read_request request = {.kind = SNAG_VM_READ_SESSIONS,
        .stored_limit = vm->stored_limit};
    vm->generation = snag_vm_reader_request(vm->reader, &request);
    if (!vm->generation) notice(vm, "Cannot start session list loading");
    vm->dirty = true;
}

static int
restore_workspace(struct vm *vm, const char *selector)
{
    struct snag_vm_workspace *next = calloc(1u, sizeof(*next));
    if (!next) return -1;
    snag_vm_workspace_init(next);
    char error[256];
    if (save(vm, NULL) < 0) {
        free(next);
        return -1;
    }
    if (snag_vm_workspace_open(next, &vm->store, selector, error, sizeof(error)) < 0 ||
        state_restore(vm, json_object_get(next->snapshot, "state"), error, sizeof(error)) < 0) {
        snag_vm_workspace_close(next);
        free(next);
        notice(vm, error);
        return -1;
    }
    snag_vm_workspace_close(vm->workspace);
    free(vm->workspace);
    vm->workspace = next;
    vm->meaningful = true;
    vm->save_dirty = false;
    notice(vm, "Workspace restored");
    refresh(vm);
    return 0;
}

static void
split(struct vm *vm, enum snag_vm_split axis)
{
    if (vm->count == SIZE_MAX / sizeof(*vm->windows) || vm->next_window > INT64_MAX) return;
    struct vm_window copy = vm->windows[vm->focus];
    copy.filter = snag_strdup_checked(copy.filter ? copy.filter : "", SNAG_MAX_DIRECT_PROMPT);
    if (!copy.filter) return;
    struct vm_window *grown = realloc(vm->windows, (vm->count + 1u) * sizeof(*grown));
    if (!grown) {
        free(copy.filter);
        return;
    }
    vm->windows = grown;
    if (snag_vm_layout_split(vm->layout, copy.id, vm->next_window, axis) < 0) {
        free(copy.filter);
        notice(vm, "Cannot split this window further");
        return;
    }
    copy.id = vm->next_window++;
    vm->windows[vm->count] = copy;
    vm->focus = vm->count++;
    changed(vm);
}

static void
close_window(struct vm *vm)
{
    if (vm->count == 1u) {
        if (save(vm, NULL) == 0) vm->quit = true;
        return;
    }
    if (snag_vm_layout_close(vm->layout, vm->windows[vm->focus].id) < 0) return;
    free(vm->windows[vm->focus].filter);
    memmove(vm->windows + vm->focus, vm->windows + vm->focus + 1u,
        (vm->count - vm->focus - 1u) * sizeof(*vm->windows));
    if (vm->focus >= --vm->count) vm->focus = vm->count - 1u;
    changed(vm);
}

static void
view(struct vm *vm, enum view_kind kind)
{
    struct vm_window *window = &vm->windows[vm->focus];
    window->kind = kind;
    window->selected = window->top = 0u;
    window->selected_id[0] = 0;
    free(window->filter);
    window->filter = NULL;
    changed(vm);
}

static void
command(struct vm *vm, const char *text)
{
    char *word = NULL, error[256];
    const char *rest;
    if (snag_irc_address_operand(text, &word, &rest, error, sizeof(error)) < 0) {
        notice(vm, error);
        return;
    }
    if (!strcmp(word, "workspace")) {
        if (!*rest) {
            (void)snprintf(vm->message, sizeof(vm->message), "%s  %s%s",
                vm->workspace->id[0] ? vm->workspace->id : "Unsaved workspace",
                vm->workspace->snapshot ? snag_json_string(vm->workspace->snapshot, "name") : "",
                vm->save_dirty ? "  [modified]" : "");
        } else if (!strcmp(rest, "save")) {
            vm->meaningful = true;
            if (save(vm, NULL) == 0) notice(vm, "Workspace saved");
        } else if (!strncmp(rest, "name ", 5u)) {
            char *name = NULL;
            const char *tail;
            if (snag_irc_address_operand(rest + 5u, &name, &tail, error, sizeof(error)) < 0)
                notice(vm, error);
            else if (*tail) notice(vm, "Quote a workspace name containing spaces");
            else if (save(vm, name) == 0) {
                vm->meaningful = true;
                notice(vm, "Workspace named and saved");
            }
            free(name);
        } else notice(vm, "Use :workspace, :workspace save, or :workspace name NAME");
    } else if (*rest) notice(vm, "Unexpected command argument");
    else if (!strcmp(word, "q") || !strcmp(word, "close")) close_window(vm);
    else if (!strcmp(word, "qa")) {
        if (save(vm, NULL) == 0) vm->quit = true;
    } else if (!strcmp(word, "split") || !strcmp(word, "sp")) split(vm, SNAG_VM_HORIZONTAL);
    else if (!strcmp(word, "vsplit") || !strcmp(word, "vsp")) split(vm, SNAG_VM_VERTICAL);
    else if (!strcmp(word, "sessions")) view(vm, VIEW_SESSIONS);
    else if (!strcmp(word, "workspaces")) {
        refresh(vm);
        view(vm, VIEW_WORKSPACES);
    } else if (!strcmp(word, "help")) view(vm, VIEW_HELP);
    else notice(vm, "Unknown workspace command; use :help");
    free(word);
    vm->dirty = true;
}

static json_t *
view_rows(struct vm *vm, const struct vm_window *window)
{
    return window->kind == VIEW_SESSIONS ? vm->sessions : vm->workspaces;
}

static bool
matches(const struct vm_window *window, const json_t *row)
{
    const char *filter = window->filter;
    if (!filter || !*filter) return true;
    const char *id = snag_json_string(row, "id"), *name = snag_json_string(row, "name");
    if ((id && strstr(id, filter)) || (name && strstr(name, filter))) return true;
    json_t *cells = json_object_get(row, "cells");
    for (size_t i = 0u; i < json_array_size(cells); ++i) {
        const char *cell = json_string_value(json_array_get(cells, i));
        if (cell && strstr(cell, filter)) return true;
    }
    return false;
}

static size_t
row_count(struct vm *vm, const struct vm_window *window)
{
    if (window->kind == VIEW_HELP) return sizeof(help_rows) / sizeof(help_rows[0]);
    size_t count = 0u;
    json_t *rows = view_rows(vm, window);
    for (size_t i = 0u; i < json_array_size(rows); ++i)
        if (matches(window, json_array_get(rows, i))) ++count;
    return count;
}

static json_t *
selected_row(struct vm *vm, const struct vm_window *window)
{
    json_t *rows = view_rows(vm, window);
    size_t visible = 0u;
    for (size_t i = 0u; i < json_array_size(rows); ++i) {
        json_t *row = json_array_get(rows, i);
        if (matches(window, row) && visible++ == window->selected) return row;
    }
    return NULL;
}

static void
selection(struct vm *vm, size_t at)
{
    struct vm_window *window = &vm->windows[vm->focus];
    size_t count = row_count(vm, window);
    window->selected = !count ? 0u : at < count ? at : count - 1u;
    const char *id = window->kind == VIEW_HELP ? NULL :
        snag_json_string(selected_row(vm, window), "id");
    (void)snprintf(window->selected_id, sizeof(window->selected_id), "%s", id ? id : "");
    vm->dirty = true;
    if (count) changed(vm);
}

static void
move(struct vm *vm, bool down, size_t amount)
{
    size_t at = vm->windows[vm->focus].selected;
    selection(vm, down ? amount > SIZE_MAX - at ? SIZE_MAX : at + amount :
        at > amount ? at - amount : 0u);
}

static void
focus_direction(struct vm *vm, unsigned int key)
{
    const struct snag_vm_rectangle *from = &vm->windows[vm->focus].rectangle;
    size_t best = vm->focus;
    uint64_t distance = UINT64_MAX;
    for (size_t i = 0u; i < vm->count; ++i) {
        const struct snag_vm_rectangle *to = &vm->windows[i].rectangle;
        int64_t y = (int64_t)to->row * 2 + to->rows - ((int64_t)from->row * 2 + from->rows);
        int64_t x = (int64_t)to->column * 2 + to->columns -
            ((int64_t)from->column * 2 + from->columns);
        if (!to->visible || (key == 'h' && x >= 0) || (key == 'l' && x <= 0) ||
            (key == 'k' && y >= 0) || (key == 'j' && y <= 0)) continue;
        uint64_t score = (uint64_t)llabs(x) + (uint64_t)llabs(y);
        if (score < distance) {
            distance = score;
            best = i;
        }
    }
    if (best != vm->focus) {
        vm->focus = best;
        changed(vm);
    }
}

static int
insert_command(struct vm *vm, const void *text, size_t size)
{
    if (snag_buf_reserve(&vm->command, size) < 0) {
        notice(vm, "Command is too long");
        return 0;
    }
    memmove(vm->command.data + vm->command_cursor + size,
        vm->command.data + vm->command_cursor, vm->command.len - vm->command_cursor);
    memcpy(vm->command.data + vm->command_cursor, text, size);
    vm->command.len += size;
    vm->command_cursor += size;
    vm->dirty = true;
    return 0;
}

static int
input_event(void *opaque, const struct snag_vm_input_event *event)
{
    struct vm *vm = opaque;
    struct vm_window *window = &vm->windows[vm->focus];
    if (event->kind == SNAG_VM_PASTE_BEGIN) {
        snag_buf_reset(&vm->paste);
        vm->paste_failed = false;
        return 0;
    }
    if (event->kind == SNAG_VM_PASTE_TEXT) {
        if (!vm->paste_failed && snag_buf_append(&vm->paste, event->text, event->length) < 0)
            vm->paste_failed = true;
        return 0;
    }
    if (event->kind == SNAG_VM_PASTE_END) {
        if (vm->paste_failed) {
            notice(vm, "Paste exceeds the input limit");
            return 0;
        }
        if (vm->mode && snag_utf8_valid(vm->paste.data, vm->paste.len, true))
            return insert_command(vm, vm->paste.data, vm->paste.len);
        notice(vm, "Open command or filter input before pasting text");
        return 0;
    }
    if (event->kind == SNAG_VM_MOUSE) {
        if (event->release) return 0;
        for (size_t i = 0u; i < vm->count; ++i) {
            const struct snag_vm_rectangle *r = &vm->windows[i].rectangle;
            if (!r->visible || event->row < r->row || event->row - r->row >= r->rows ||
                event->column < r->column || event->column - r->column >= r->columns) continue;
            vm->focus = i;
            if (event->button == 64u || event->button == 65u) move(vm, event->button == 65u, 3u);
            else if (event->button == 0u && event->row - r->row + 1u < r->rows)
                selection(vm, vm->windows[i].top + event->row - r->row);
            vm->dirty = true;
            break;
        }
        return 0;
    }
    unsigned int key = event->kind == SNAG_VM_TEXT && event->length == 1u ?
        event->text[0] : event->kind == SNAG_VM_KEY ? event->key : 0u;
    bool control = (event->modifiers & SNAG_VM_CTRL) != 0u;
    if (vm->mode) {
        const char *bytes = (const char *)vm->command.data;
        size_t at = vm->command_cursor, end = at;
        if (event->kind == SNAG_VM_TEXT) return insert_command(vm, event->text, event->length);
        if (key == SNAG_VM_KEY_ESCAPE || (control && key == 'c')) vm->mode = 0;
        else if (key == SNAG_VM_KEY_ENTER) {
            if (snag_buf_putc(&vm->command, 0u) < 0) return -1;
            --vm->command.len;
            char mode = vm->mode;
            vm->mode = 0;
            if (mode == ':') command(vm, (const char *)vm->command.data);
            else {
                char *filter = snag_strdup_checked((const char *)vm->command.data,
                    SNAG_MAX_DIRECT_PROMPT);
                if (!filter) return -1;
                free(window->filter);
                window->filter = filter;
                window->selected = window->top = 0u;
                window->selected_id[0] = 0;
                changed(vm);
            }
        } else if (key == SNAG_VM_KEY_LEFT) at = snag_vm_text_previous(bytes, vm->command.len, at);
        else if (key == SNAG_VM_KEY_RIGHT) at = snag_vm_text_next(bytes, vm->command.len, at);
        else if (key == SNAG_VM_KEY_HOME) at = 0u;
        else if (control && key == 'u') {
            memmove(vm->command.data, vm->command.data + at, vm->command.len - at);
            vm->command.len -= at;
            at = 0u;
        }
        else if (key == SNAG_VM_KEY_END) at = vm->command.len;
        else if (key == SNAG_VM_KEY_BACKSPACE) {
            at = snag_vm_text_previous(bytes, vm->command.len, at);
            memmove(vm->command.data + at, vm->command.data + end, vm->command.len - end);
            vm->command.len -= end - at;
        } else if (key == SNAG_VM_KEY_DELETE) {
            end = snag_vm_text_next(bytes, vm->command.len, at);
            memmove(vm->command.data + at, vm->command.data + end, vm->command.len - end);
            vm->command.len -= end - at;
        }
        if (key != SNAG_VM_KEY_ENTER) vm->command_cursor = at;
        vm->dirty = true;
        return 0;
    }
    if (key == SNAG_VM_KEY_ESCAPE) vm->prefix = 0;
    else if (control && key == 'z') vm->suspend = true;
    else if (control && key == 'l') {
        resized = 1;
        vm->dirty = true;
    }
    else if (control && key == 'w') vm->prefix = 'w';
    else if (vm->prefix == 'w') {
        vm->prefix = 0;
        if (key == 's' || key == 'v') split(vm, key == 'v' ? SNAG_VM_VERTICAL : SNAG_VM_HORIZONTAL);
        else if (key == 'q' || key == 'c') close_window(vm);
        else if (key == 'w') {
            vm->focus = (vm->focus + 1u) % vm->count;
            changed(vm);
        }
        else if (key == 'h' || key == 'j' || key == 'k' || key == 'l') focus_direction(vm, key);
        else if (key == '=') {
            snag_vm_layout_equalize(vm->layout);
            changed(vm);
        }
        else if (key == '+' || key == '-' || key == '>' || key == '<') {
            (void)snag_vm_layout_resize(vm->layout, window->id,
                key == '>' || key == '<' ? SNAG_VM_VERTICAL : SNAG_VM_HORIZONTAL,
                key == '+' || key == '>' ? 5 : -5);
            changed(vm);
        }
    } else if (key == ':' || key == '/') {
        vm->mode = (char)key;
        vm->prefix = 0;
        snag_buf_reset(&vm->command);
        vm->command_cursor = 0u;
        vm->dirty = true;
    } else if (key == 'g') {
        if (vm->prefix == 'g') {
            selection(vm, 0u);
            vm->prefix = 0;
        }
        else vm->prefix = 'g';
    } else {
        vm->prefix = 0;
        if (key == 'G') selection(vm, SIZE_MAX);
        else if (key == 'j' || key == SNAG_VM_KEY_DOWN) move(vm, true, 1u);
        else if (key == 'k' || key == SNAG_VM_KEY_UP) move(vm, false, 1u);
        else if (key == SNAG_VM_KEY_PAGE_DOWN || (control && key == 'd'))
            move(vm, true, window->rectangle.rows / 2u + 1u);
        else if (key == SNAG_VM_KEY_PAGE_UP || (control && key == 'u'))
            move(vm, false, window->rectangle.rows / 2u + 1u);
        else if (key == 'R') refresh(vm);
        else if (key == SNAG_VM_KEY_ENTER) {
            json_t *row = selected_row(vm, window);
            const char *id = snag_json_string(row, "id");
            if (id && window->kind == VIEW_WORKSPACES) (void)restore_workspace(vm, id);
            else if (id && window->kind == VIEW_SESSIONS)
                notice(vm, "Session selected; use snajpagent --resume with its ID");
        }
    }
    return 0;
}

static int
input_ready(struct vm *vm, int timeout)
{
    int ready = snag_term_input_wait(&vm->terminal, snag_vm_reader_fd(vm->reader), timeout);
    if (ready < 0) return errno == EINTR ? 0 : -1;
    if (ready & SNAG_TERM_WAIT_END) {
        vm->quit = true;
        return 0;
    }
    if (ready > 0 && (ready & SNAG_TERM_WAIT_INPUT)) {
        unsigned char bytes[8192];
        ssize_t count = snag_term_input_read(&vm->terminal, bytes, sizeof(bytes));
        if (count < 0 && errno != EAGAIN && errno != EINTR) return -1;
        if (count > 0 && snag_vm_input_feed(&vm->input, bytes, (size_t)count,
            snag_monotonic_ms(), input_event, vm) < 0) return -1;
    }
    return snag_vm_input_expire(&vm->input, snag_monotonic_ms(), input_event, vm);
}

static int
output_checkpoint(void *opaque)
{
    struct vm *vm = opaque;
    if (stopped || vm->quit) return snag_errno(ECANCELED);
    return vm->entering ? 0 : input_ready(vm, 0);
}

static int
emit(void *opaque, const void *text, size_t length)
{
    struct vm *vm = opaque;
    return snag_term_output_write(&vm->terminal, vm->output, text, length, true,
        output_checkpoint, vm);
}

static int
draw_window(void *opaque, const struct snag_vm_rectangle *rectangle)
{
    struct vm *vm = opaque;
    size_t index = 0u;
    while (index < vm->count && vm->windows[index].id != rectangle->window) ++index;
    if (index == vm->count) return -1;
    struct vm_window *window = &vm->windows[index];
    window->rectangle = *rectangle;
    if (!rectangle->visible) return 0;
    size_t height = rectangle->rows > 1u ? rectangle->rows - 1u : 0u;
    size_t count = row_count(vm, window);
    if (window->selected >= count) window->selected = count ? count - 1u : 0u;
    if (window->top > window->selected) window->top = window->selected;
    if (height && window->selected - window->top >= height)
        window->top = window->selected - height + 1u;
    json_t *rows = view_rows(vm, window);
    size_t visible = 0u;
    for (size_t i = 0u; i < (window->kind == VIEW_HELP ? count : json_array_size(rows)); ++i) {
        json_t *row = json_array_get(rows, i);
        if (window->kind != VIEW_HELP && !matches(window, row)) continue;
        size_t position = visible++;
        if (position < window->top || position - window->top >= height) continue;
        struct snag_buf text = {.max = SNAG_MAX_DIRECT_PROMPT};
        if (window->kind == VIEW_HELP) {
            (void)snag_buf_printf(&text, "%s", help_rows[i]);
        } else if (window->kind == VIEW_WORKSPACES) {
            const char *error = snag_json_string(row, "error");
            const char *name = snag_json_string(row, "name");
            (void)snag_buf_printf(&text, "%.8s %-6s %s%s", snag_json_string(row, "id"),
                error ? "error" : json_is_true(json_object_get(row, "open")) ? "open" : "stored",
                name ? name : "", error ? error : "");
        } else {
            json_t *cells = json_object_get(row, "cells");
            static const size_t order[] = {0u, 4u, 1u, 2u, 3u, 5u, 6u};
            for (size_t c = 0u; c < sizeof(order) / sizeof(order[0]); ++c) {
                const char *cell = json_string_value(json_array_get(cells, order[c]));
                if (cell) (void)snag_buf_printf(&text, "%s%s", c ? "  " : "", cell);
            }
        }
        int rc = snag_vm_grid_text(&vm->grid, rectangle->row + position - window->top,
            rectangle->column, rectangle->columns, (const char *)text.data, text.len,
            position == window->selected ? SNAG_VM_REVERSE : 0u);
        snag_buf_free(&text);
        if (rc < 0) return -1;
    }
    char status[512];
    (void)snprintf(status, sizeof(status), "%s  %zu/%zu%s%s", view_names[window->kind],
        count ? window->selected + 1u : 0u, count,
        window->filter && *window->filter ? " /" : "", window->filter ? window->filter : "");
    return snag_vm_grid_text(&vm->grid, rectangle->row + rectangle->rows - 1u,
        rectangle->column, rectangle->columns, status, strlen(status),
        index == vm->focus ? SNAG_VM_BOLD | SNAG_VM_REVERSE : SNAG_VM_REVERSE);
}

static int
draw(struct vm *vm)
{
    size_t rows = snag_term_host_rows(), columns = snag_term_host_columns();
    if (!rows) rows = 1u;
    if (!columns) columns = 1u;
    if (snag_vm_grid_resize(&vm->grid, rows, columns) < 0) return -1;
    vm->dirty = false;
    snag_vm_grid_begin(&vm->grid);
    if (snag_vm_layout_place(vm->layout, vm->windows[vm->focus].id,
        rows > 1u ? (unsigned int)rows - 1u : 1u, (unsigned int)columns, draw_window, vm) < 0)
        return -1;
    size_t cursor = 0u;
    if (vm->mode) {
        size_t start = 0u;
        size_t column = snag_vm_text_column((const char *)vm->command.data,
            vm->command.len, vm->command_cursor, false);
        if (column + 2u > columns) start = snag_vm_text_at_column((const char *)vm->command.data,
            vm->command.len, 0u, column + 2u - columns, false);
        size_t start_column = snag_vm_text_column((const char *)vm->command.data,
            vm->command.len, start, false);
        if (snag_vm_grid_text(&vm->grid, rows - 1u, 0u, 1u, &vm->mode, 1u, SNAG_VM_BOLD) < 0 ||
            snag_vm_grid_text_column(&vm->grid, rows - 1u, 1u, columns - 1u,
                vm->command.data ? (const char *)vm->command.data + start : "",
                vm->command.len - start, 0u, start_column) < 0) return -1;
        cursor = column - start_column + 1u;
        if (cursor >= columns) cursor = columns - 1u;
    } else if (snag_vm_grid_text(&vm->grid, rows - 1u, 0u, columns,
        vm->message, strlen(vm->message), 0u) < 0) return -1;
    return snag_vm_grid_flush(&vm->grid, rows - 1u, cursor, vm->mode != 0, emit, vm);
}

static void
collect(struct vm *vm)
{
    struct snag_vm_read_result *result = snag_vm_reader_take(vm->reader);
    if (!result) return;
    if (result->error_number) notice(vm, result->error);
    else if (result->generation == vm->generation) {
        json_decref(vm->sessions);
        vm->sessions = result->catalog;
        result->catalog = NULL;
        for (size_t w = 0u; w < vm->count; ++w) {
            struct vm_window *window = &vm->windows[w];
            if (window->kind != VIEW_SESSIONS || !window->selected_id[0]) continue;
            size_t visible = 0u;
            for (size_t i = 0u; i < json_array_size(vm->sessions); ++i) {
                json_t *row = json_array_get(vm->sessions, i);
                if (!matches(window, row)) continue;
                if (!strcmp(snag_json_string(row, "id"), window->selected_id))
                    window->selected = visible;
                ++visible;
            }
        }
        vm->dirty = true;
    }
    snag_vm_read_result_free(result);
}

static int
enter_screen(struct vm *vm)
{
    static const char modes[] = "\033[?1049h\033[?2004h\033[?1000h\033[?1006h\033[?1004h\033[?25l";
    vm->entering = true;
    int rc = snag_term_input_raw(&vm->terminal);
    if (!rc) rc = snag_term_output_mode(&vm->terminal, true);
    if (!rc) rc = emit(vm, modes, sizeof(modes) - 1u);
    vm->entering = false;
    vm->grid.valid = false;
    vm->dirty = true;
    return rc;
}

static int
restore_checkpoint(void *opaque)
{
    uint64_t *deadline = opaque;
    return snag_monotonic_ms() >= *deadline ? snag_errno(ETIMEDOUT) : 0;
}

static void
leave_screen(struct vm *vm)
{
    static const char modes[] = "\033[0m\033[?25h\033[?1004l\033[?1006l"
        "\033[?1000l\033[?2004l\033[?1049l";
    uint64_t deadline = snag_monotonic_ms() + 250u;
    (void)snag_term_output_write(&vm->terminal, vm->output, modes, sizeof(modes) - 1u,
        false, restore_checkpoint, &deadline);
    (void)snag_term_output_mode(&vm->terminal, false);
    (void)snag_term_input_restore(&vm->terminal, false);
}

static int
interactive(struct vm *vm)
{
    if (!snag_isatty(STDIN_FILENO) || !snag_isatty(STDOUT_FILENO) || !snag_term_host_capable()) {
        (void)fprintf(stderr, "snajpagent: vm requires an interactive terminal\n");
        return 2;
    }
    struct snag_shutdown shutdown = {0};
    bool captured = false, controls = false, entered = false;
    int rc = -1;
    stopped = resized = 0;
    vm->output = snag_term_output_open(&vm->terminal, STDOUT_FILENO);
    if (vm->output < 0 || snag_term_input_capture(&vm->terminal) < 0) goto out;
    captured = true;
    if (snag_shutdown_install(&shutdown, stop_signal, true) < 0 ||
        snag_term_controls_install(&vm->terminal, stop_signal, resize_signal) < 0) goto out;
    controls = true;
    entered = true;
    if (enter_screen(vm) < 0) goto out;
    refresh(vm);
    while (!vm->quit && !stopped) {
        collect(vm);
        if (resized || snag_term_input_resized(&vm->terminal)) {
            resized = 0;
            vm->grid.valid = false;
            vm->dirty = true;
        }
        if (vm->suspend) {
            vm->suspend = false;
            if (snag_term_can_suspend() && save(vm, NULL) == 0) {
                leave_screen(vm);
                (void)snag_term_suspend();
                if (enter_screen(vm) < 0) goto out;
            }
        }
        if (vm->save_dirty && vm->save_at && snag_monotonic_ms() >= vm->save_at)
            (void)save(vm, NULL);
        if (vm->dirty && draw(vm) < 0) {
            if (vm->quit || stopped) break;
            goto out;
        }
        int timeout = snag_vm_input_wait_ms(&vm->input, snag_monotonic_ms());
        if (vm->save_at) {
            uint64_t now = snag_monotonic_ms();
            int remaining = vm->save_at > now ? (int)(vm->save_at - now) : 0;
            if (timeout < 0 || timeout > remaining) timeout = remaining;
        }
        if (vm->dirty || vm->suspend) timeout = 0;
        if (input_ready(vm, timeout) < 0) goto out;
    }
    rc = save(vm, NULL) < 0 ? 1 : 0;
out:
    snag_shutdown_detach(&shutdown);
    if (entered) leave_screen(vm);
    else if (captured) (void)snag_term_input_restore(&vm->terminal, false);
    if (controls) snag_term_controls_restore(&vm->terminal);
    snag_shutdown_finish(&shutdown);
    if (vm->output >= 0) (void)close(vm->output);
    vm->output = -1;
    snag_term_host_close(&vm->terminal);
    if (rc < 0) {
        (void)fprintf(stderr, "snajpagent: workspace terminal failed: %s\n", strerror(errno));
    } else if (rc) {
        (void)fprintf(stderr, "snajpagent: %s\n", vm->message);
    }
    return rc < 0 ? 1 : rc;
}

static int
list_workspaces(struct vm *vm, char *error, size_t size)
{
    json_t *rows = snag_vm_workspace_list(&vm->store, vm->stored_limit, error, size);
    if (!rows) return -1;
    (void)puts("ID\tNAME\tSTATUS\tACTIVITY_MS");
    for (size_t i = 0u; i < json_array_size(rows); ++i) {
        json_t *row = json_array_get(rows, i);
        const char *name = snag_json_string(row, "name"), *problem = snag_json_string(row, "error");
        (void)printf("%s\t%s\t%s\t%lld%s%s\n", snag_json_string(row, "id"), name ? name : "",
            problem ? "error" : json_is_true(json_object_get(row, "open")) ? "open" : "stored",
            (long long)json_integer_value(json_object_get(row, "activity_ms")),
            problem ? "\t" : "", problem ? problem : "");
    }
    json_decref(rows);
    return ferror(stdout) ? -1 : 0;
}

static void
usage(void)
{
    (void)puts("Usage: snajpagent vm [--dotdir DIR] [-N NAME | --resume WORKSPACE | --last]\n"
        "       snajpagent vm [--dotdir DIR] -l [STORED_COUNT]\n"
        "       snajpagent vm --help | --version\n\n"
        "Open a Vim workspace with the session picker; :help lists available controls.\n"
        "-l lists saved workspaces, with all open rows and ten stored rows by default.");
}

int
snag_vm_main(int argc, char **argv)
{
    const char *dotdir_option = NULL, *name = NULL, *resume = NULL;
    bool list = false, last = false;
    struct vm vm = {.output = -1, .stored_limit = 10u, .next_window = 2u};
    char error[256] = "invalid workspace arguments";
    int rc = 2;
    snag_store_init(&vm.store);
    snag_config_init(&vm.config);
    vm.workspace = calloc(1u, sizeof(*vm.workspace));
    if (!vm.workspace) return 1;
    snag_vm_workspace_init(vm.workspace);
    snag_buf_init(&vm.command, SNAG_MAX_DIRECT_PROMPT + 1u);
    snag_buf_init(&vm.paste, SNAG_MAX_DIRECT_PROMPT);
    for (int i = 0; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
            usage();
            rc = 0;
            goto out;
        }
        if (!strcmp(arg, "--version") || !strcmp(arg, "-V")) {
            (void)puts(SNAJPAGENT_IDENTITY);
            rc = 0;
            goto out;
        }
        if (!strcmp(arg, "--dotdir") && !dotdir_option && i + 1 < argc) dotdir_option = argv[++i];
        else if (!strcmp(arg, "-N") && !name && i + 1 < argc) name = argv[++i];
        else if (!strcmp(arg, "--resume") && !resume && i + 1 < argc) resume = argv[++i];
        else if (!strcmp(arg, "--last") && !last) last = true;
        else if ((!strcmp(arg, "-l") || !strcmp(arg, "--list")) && !list) {
            list = true;
            if (i + 1 < argc && argv[i + 1][0] != '-' &&
                snag_parse_count(argv[++i], &vm.stored_limit) < 0) goto invalid;
        } else goto invalid;
    }
    if ((unsigned int)(name != NULL) + (resume != NULL) + last + list > 1u) goto invalid;
    if (!list && (!snag_isatty(STDIN_FILENO) || !snag_isatty(STDOUT_FILENO) ||
        !snag_term_host_capable())) {
        (void)snprintf(error, sizeof(error), "vm requires an interactive terminal");
        goto failed;
    }
    char *dotdir = snag_app_dotdir(dotdir_option, error, sizeof(error));
    if (!dotdir) goto failed;
    int opened = snag_store_open(&vm.store, dotdir, error, sizeof(error));
    free(dotdir);
    if (opened < 0) goto failed;
    if (list) {
        rc = list_workspaces(&vm, error, sizeof(error));
        if (rc < 0) goto failed;
        goto out;
    }
    vm.windows = calloc(1u, sizeof(*vm.windows));
    vm.layout = snag_vm_layout_new(1u);
    if (!vm.windows || !vm.layout) goto failed;
    vm.count = 1u;
    vm.windows[0].id = 1u;
    if (snag_config_load(&vm.config, NULL, vm.store.root_path, error, sizeof(error)) < 0 ||
        snag_secret_set_build(&vm.secrets, &vm.config, NULL, error, sizeof(error)) < 0) goto failed;
    vm.reader = snag_vm_reader_open(&vm.store, &vm.secrets.wire, error, sizeof(error));
    if (!vm.reader) goto failed;
    notice(&vm, "NORMAL  :help for controls  / filter  R refresh");
    if (resume || last) {
        if (restore_workspace(&vm, resume) < 0) {
            (void)snprintf(error, sizeof(error), "%s", vm.message);
            goto failed;
        }
    } else if (name && save(&vm, name) < 0) {
        (void)snprintf(error, sizeof(error), "%s", vm.message);
        goto failed;
    }
    if (name) vm.meaningful = true;
    rc = interactive(&vm);
    goto out;
invalid:
    usage();
failed:
    (void)fprintf(stderr, "snajpagent: %s\n", error);
    rc = 2;
out:
    snag_vm_reader_close(vm.reader);
    snag_vm_workspace_close(vm.workspace);
    free(vm.workspace);
    snag_vm_layout_free(vm.layout);
    windows_free(vm.windows, vm.count);
    snag_vm_grid_free(&vm.grid);
    snag_buf_free(&vm.command);
    snag_buf_free(&vm.paste);
    json_decref(vm.sessions);
    json_decref(vm.workspaces);
    snag_secret_set_free(&vm.secrets);
    snag_config_free(&vm.config);
    snag_store_close(&vm.store);
    return rc;
}
#else
int
snag_vm_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    (void)fprintf(stderr, "snajpagent: Vim mode is unavailable in this build (WITH_VM=0)\n");
    return 2;
}
#endif /* SNAJPAGENT_VM */
