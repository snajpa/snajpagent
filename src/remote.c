/* SPDX-License-Identifier: GPL-2.0-only */
#include "remote.h"
#include "base.h"
#include "config.h"
#include "fs.h"
#include "process_host.h"
#include "term_host.h"
#include "upload.h"
#include "upload_wire.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
int
snag_remote_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    (void)fprintf(stderr, "snajpagent remote: "
                  "a native client terminal is unavailable on this host\n");
    return 2;
}
#else
#include <poll.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <termios.h>

static volatile sig_atomic_t remote_signal;
static volatile sig_atomic_t remote_resize;

static void
remote_interrupt(int number)
{
    if (number == SIGWINCH) remote_resize = 1;
    else remote_signal = number;
}

static int
remote_write(int fd, const void *bytes, size_t length)
{
    const unsigned char *data = bytes;
    while (length) {
        ssize_t amount = write(fd, data, length);
        if (amount < 0 && errno == EINTR) {
            if (remote_signal) return snag_errno(ECANCELED);
            continue;
        }
        if (amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd wait = {fd, POLLOUT, 0};
            if (poll(&wait, 1u, 50) < 0 && errno != EINTR) return -1;
            if (remote_signal) return snag_errno(ECANCELED);
            continue;
        }
        if (amount <= 0) return amount < 0 ? -1 : snag_errno(EIO);
        data += (size_t)amount;
        length -= (size_t)amount;
    }
    return 0;
}

static void
remote_usage(void)
{
    (void)fprintf(stderr,
        "Usage: snajpagent remote [--config PATH] [--dotdir PATH] [--] [COMMAND ARG...]\n"
        "Client-only terminal wrapper; does not start an agent or chat session.\n"
        "With no command, starts your local shell. Child arguments are passed literally.\n"
        "Start: snajpagent remote ssh -t target snajpagent\n"
        "Detach inside the agent: /session detach\n"
        "Reattach: snajpagent remote ssh -t target snajpagent --attach [SESSION_ID]\n"
        "Without SESSION_ID, attach offers a running-session picker.\n");
}

struct remote_transfer {
    struct snag_child *child;
    const char *downloads;
    bool relay;
    bool screen;
    bool relay_transfer;
    bool relay_frame;
    unsigned char marker[128];
    size_t marker_len;
    unsigned char keys[4096];
    size_t key_len;
    bool drop_ready;
    bool paste_passthrough;
    size_t paste_end;
    struct snag_buf input;
    uint64_t input_at;
    int drop_fd;
    char drop_name[SNAG_NAME_MAX_BYTES + 1u];
    uint64_t drop_at;
    uint64_t progress_at;
    bool progress_visible;
    char direction;
};

/* Terminal drops are literal paths, shell-quoted paths or bracketed pastes.
 * Decode quoting only: never expand variables, substitutions or glob patterns. */
static int
remote_drop_open(struct remote_transfer *client, const unsigned char *data, size_t length)
{
    char path[SNAG_PATH_MAX_BYTES + 1u];
    while (length && (data[length - 1u] == ' ' || data[length - 1u] == '\r' ||
                     data[length - 1u] == '\n')) --length;
    if (!length || length > sizeof(path) - 1u) return -1;
    for (size_t i = 0; i < length; ++i)
        if (data[i] < 0x20u || data[i] == 0x7fu) return -1;
    memcpy(path, data, length);
    path[length] = '\0';
    int fd = path[0] == '/' ? snag_open_read(path, false) : -1;
    if (fd < 0) {
        unsigned char quote = 0;
        size_t used = 0;
        for (size_t i = 0; i < length; ++i) {
            unsigned char byte = data[i];
            if (byte == quote) { quote = 0; continue; }
            if (!quote && (byte == '\'' || byte == '"')) { quote = byte; continue; }
            if (byte == '\\' && quote != '\'') {
                if (++i == length) return -1;
                byte = data[i];
                if (quote == '"' && byte != '"' && byte != '\\' && byte != '$' &&
                    byte != '`') path[used++] = '\\';
            } else if (!quote && byte == ' ') return -1;
            path[used++] = (char)byte;
        }
        if (quote || !used || path[0] != '/') return -1;
        path[used] = '\0';
        fd = snag_open_read(path, false);
    }
    if (fd < 0) return -1;
    struct stat info;
    const char *name = strrchr(path, '/') + 1u;
    size_t size = strlen(name);
    if (fstat(fd, &info) < 0 || !S_ISREG(info.st_mode) || !size ||
        size > SNAG_NAME_MAX_BYTES || !snag_utf8_valid((const unsigned char *)name, size, true)) {
        (void)close(fd);
        return -1;
    }
    memcpy(client->drop_name, name, size + 1u);
    return fd;
}

static int
remote_input_flush(struct remote_transfer *client, bool complete)
{
    const unsigned char *data = client->input.data;
    size_t length = client->input.len;
    if (!length) return 0;
    if (complete && client->drop_ready) {
        if (length >= 12u && !memcmp(data, "\033[200~", 6u) &&
            !memcmp(data + length - 6u, "\033[201~", 6u)) {
            data += 6u;
            length -= 12u;
        }
        client->drop_fd = remote_drop_open(client, data, length);
        if (client->drop_fd >= 0) {
            client->input.len = 0;
            client->drop_at = snag_monotonic_ms();
            return remote_write(client->child->fd[2], "\033[9002~", 7u);
        }
    }
    int rc = remote_write(client->child->fd[2], client->input.data, client->input.len);
    client->input.len = 0;
    return rc;
}

static int
remote_input(struct remote_transfer *client, const unsigned char *data, size_t length)
{
    static const char begin[] = "\033[200~", end[] = "\033[201~";
    if (client->relay) return remote_write(client->child->fd[2], data, length);
    for (size_t i = 0; i < length; ++i) {
        unsigned char byte = data[i];
        if (client->drop_fd >= 0) {
            if (length - i > sizeof(client->keys) - client->key_len)
                return snag_errno(ENOBUFS);
            memcpy(client->keys + client->key_len, data + i, length - i);
            client->key_len += length - i;
            return 0;
        }
        if (client->paste_passthrough) {
            if (remote_write(client->child->fd[2], &byte, 1u) < 0) return -1;
            client->paste_end = byte == (unsigned char)end[client->paste_end] ?
                client->paste_end + 1u : byte == 27u ? 1u : 0u;
            if (client->paste_end == 6u) {
                client->paste_passthrough = false;
                client->paste_end = 0;
            }
            continue;
        }
        if (!client->input.len && (i != 0 || !client->drop_ready ||
            (byte != '/' && byte != '\'' && byte != '"' && byte != 27u))) {
            return remote_write(client->child->fd[2], data + i, length - i);
        }
        if (snag_buf_putc(&client->input, byte) < 0) return -1;
        client->input_at = snag_monotonic_ms();
        size_t n = client->input.len;
        unsigned char *input = client->input.data;
        bool paste = n >= 6u && !memcmp(input, begin, 6u);
        if (paste && n >= 12u && !memcmp(input + n - 6u, end, 6u)) {
            if (remote_input_flush(client, true) < 0) return -1;
        } else if (n == 2u * SNAG_PATH_MAX_BYTES + 12u) {
            client->paste_passthrough = paste;
            if (paste) {
                for (size_t at = 6u; at < n; ++at)
                    client->paste_end = input[at] == (unsigned char)end[client->paste_end] ?
                        client->paste_end + 1u : input[at] == 27u ? 1u : 0u;
            }
            if (remote_input_flush(client, false) < 0) return -1;
        } else if (!paste && ((input[0] == 27u &&
                   (n > 6u || memcmp(input, begin, n))) || byte == '\r' || byte == '\n')) {
            if (remote_input_flush(client, input[0] != 27u) < 0) return -1;
        }
    }
    return 0;
}

static int
remote_checkpoint(void *opaque)
{
    struct remote_transfer *client = opaque;
    if (remote_signal) return 1;
    if (remote_resize) { remote_resize = 0; snag_child_resize(client->child); }
    for (size_t i = 0; i < client->key_len; ++i) {
        if (client->keys[i] != 3u) continue;
        memmove(client->keys + i, client->keys + i + 1u, client->key_len - i - 1u);
        --client->key_len;
        return 1;
    }
    struct pollfd ready = {STDIN_FILENO, POLLIN, 0};
    if (poll(&ready, 1u, 0) > 0 && (ready.revents & POLLIN)) {
        unsigned char keys[256];
        ssize_t n = read(STDIN_FILENO, keys, sizeof(keys));
        if (n <= 0) return 1;
        for (ssize_t i = 0; i < n; ++i) {
            if (keys[i] == 3u) return 1;
            if (client->key_len == sizeof(client->keys)) return 1;
            client->keys[client->key_len++] = keys[i];
        }
    }
    return 0;
}

static int
remote_download_root(struct remote_transfer *client, char **path)
{
    struct snag_buf expanded;
    snag_buf_init(&expanded, SNAG_PATH_MAX_BYTES);
    char *home = NULL;
    const char *name = client->downloads;
    int fd = -1;
    if (name[0] == '~' && name[1] == '/') {
        home = snag_home_directory();
        if (!home || snag_buf_printf(&expanded, "%s/%s", home, name + 2u) < 0 ||
            snag_buf_terminate(&expanded) < 0) goto done;
        name = (char *)expanded.data;
    }
    if (!snag_path_root_len(name)) { errno = EINVAL; goto done; }
    for (const unsigned char *at = (const unsigned char *)name; *at; ++at)
        if (*at < 0x20u || *at == 0x7fu) { errno = EINVAL; goto done; }
    if (mkdir(name, 0777) < 0 && errno != EEXIST) goto done;
    *path = snag_realpath(name);
    if (!*path) goto done;
    fd = open(*path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) { free(*path); *path = NULL; }
done:
    free(home);
    snag_buf_free(&expanded);
    return fd;
}

static int
remote_upload_choice(struct remote_transfer *client, char *path, size_t capacity)
{
    static const char question[] = "\r\nSelect local file (empty cancels): ";
    if (remote_write(STDOUT_FILENO, question, sizeof(question) - 1u) < 0) return -1;
    size_t length = 0;
    for (;;) {
        struct pollfd ready = {STDIN_FILENO, POLLIN, 0};
        int count = poll(&ready, 1u, 100);
        if (remote_signal) return -1;
        if (remote_resize) { remote_resize = 0; snag_child_resize(client->child); }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return -1;
        if (!count) continue;
        unsigned char byte;
        if (read(STDIN_FILENO, &byte, 1u) != 1) return -1;
        if (byte == 3u || byte == 27u) { path[0] = '\0'; return 0; }
        if (byte == '\r' || byte == '\n') break;
        if (byte == 127u || byte == 8u) {
            if (length) {
                --length;
                while (length && (path[length] & 0xc0u) == 0x80u) --length;
                (void)remote_write(STDOUT_FILENO, "\b \b", 3u);
            }
            continue;
        }
        if (byte < 0x20u) continue;
        if (length + 1u == capacity) return snag_errno(ENAMETOOLONG);
        path[length++] = (char)byte;
        if (remote_write(STDOUT_FILENO, &byte, 1u) < 0) return -1;
    }
    path[length] = '\0';
    (void)remote_write(STDOUT_FILENO, "\r\n", 2u);
    return length && !snag_utf8_valid((const unsigned char *)path, length, true) ? -1 : 0;
}

static int remote_output(struct remote_transfer *, const unsigned char *, size_t);

static int
remote_progress(void *opaque, uint64_t done, uint64_t total)
{
    struct remote_transfer *client = opaque;
    uint64_t now = snag_monotonic_ms();
    if (client->progress_visible && done != total && now - client->progress_at < 100u) return 0;
    unsigned int percent = done == total ? 100u :
        (unsigned int)(100.0 * (double)done / (double)total);
    if (done != total && percent > 99u) percent = 99u;
    const char *label = client->direction == 'S' ? "Download" : "Upload";
    char bar[13], text[128];
    for (unsigned int i = 0; i < 12u; ++i) bar[i] = i < percent * 12u / 100u ? '#' : '-';
    bar[12] = '\0';
    int n = snprintf(text, sizeof(text), "%s [%s] %3u%%  %llu/%llu bytes", label, bar,
                     percent, (unsigned long long)done, (unsigned long long)total);
    unsigned int columns = snag_term_host_columns();
    if (!columns) columns = 80u;
    if (n >= 0 && (unsigned int)n >= columns)
        n = snprintf(text, sizeof(text), "%s %u%%", label, percent);
    if (n < 0 || (size_t)n >= sizeof(text)) return -1;
    size_t length = (size_t)n < columns ? (size_t)n : columns - 1u;
    if (remote_write(STDOUT_FILENO, "\r\033[2K", 5u) < 0 ||
        remote_write(STDOUT_FILENO, text, length) < 0) return -1;
    client->progress_visible = true;
    client->progress_at = now;
    return 0;
}

static int
remote_transfer_run(struct remote_transfer *client, char direction)
{
    struct snag_client_result result = {0};
    char error[256] = {0};
    int rc = -1, fd = -1;
    char *directory = NULL;
    client->direction = direction;
    client->progress_visible = false;
    client->progress_at = 0u;
    if (direction == 'S') {
        fd = remote_download_root(client, &directory);
        if (fd >= 0) rc = snag_client_download(client->child->fd[0], fd, directory,
            remote_progress, remote_checkpoint, client, &result, error, sizeof(error));
    } else if (client->drop_fd >= 0) {
        fd = client->drop_fd;
        client->drop_fd = -1;
        rc = snag_client_upload(client->child->fd[0], fd, client->drop_name,
            remote_progress, remote_checkpoint, client, &result, error, sizeof(error));
    } else {
        char path[SNAG_PATH_MAX_BYTES + 1u];
        if (remote_upload_choice(client, path, sizeof(path)) == 0 && path[0]) {
            fd = snag_open_read(path, false);
            const char *name = strrchr(path, '/');
            if (fd >= 0) rc = snag_client_upload(client->child->fd[0], fd,
                name ? name + 1u : path, remote_progress, remote_checkpoint, client,
                &result, error, sizeof(error));
        }
    }
    if (fd >= 0) (void)close(fd);
    free(directory);
    if (rc < 0 && !error[0]) {
        static const char cancel[] = "{\"confirm\":false,\"protocol\":1,\"newline\":\"\\n\"}";
        struct snag_buf response;
        snag_buf_init(&response, SNAG_UPLOAD_LINE_MAX);
        if (snag_buf_append(&response, "#ACT:", 5u) == 0 &&
            snag_upload_wire_encode(cancel, sizeof(cancel) - 1u, &response) == 0 &&
            snag_buf_putc(&response, '\n') == 0)
            (void)remote_write(client->child->fd[2], response.data, response.len);
        snag_buf_free(&response);
    }
    if (client->progress_visible && remote_write(STDOUT_FILENO, "\r\033[2K", 5u) < 0) return -1;
    client->progress_visible = false;
    if (result.tail_len && remote_output(client, result.tail, result.tail_len) < 0) return -1;
    if (client->key_len && client->drop_fd < 0) {
        (void)remote_write(client->child->fd[2], client->keys, client->key_len);
        client->key_len = 0;
    }
    return 0;
}

static int
remote_passthrough(struct remote_transfer *client, const unsigned char *data, size_t length)
{
    if (!client->screen) return remote_write(STDOUT_FILENO, data, length);
    while (length) {
        unsigned char packet[132];
        size_t chunk = length > 128u ? 128u : length;
        packet[0] = 0x1bu;
        packet[1] = 'P';
        memcpy(packet + 2u, data, chunk);
        packet[chunk + 2u] = 0x1bu;
        packet[chunk + 3u] = '\\';
        if (remote_write(STDOUT_FILENO, packet, chunk + 4u) < 0) return -1;
        data += chunk;
        length -= chunk;
    }
    return 0;
}

static int
remote_output(struct remote_transfer *client, const unsigned char *data, size_t length)
{
    static const char prefix[] = "::TRZSZ:TRANSFER:";
    static const char probe[] = "\033[?9001;";
    static const char drop[] = "\033[?9002";
    for (size_t i = 0; i < length; ++i) {
        unsigned char byte = data[i];
        if (client->relay_transfer) {
            if (client->relay_frame || byte == '#') {
                size_t end = i;
                while (end < length && end - i < 128u) {
                    if (data[end++] == '\n') break;
                }
                client->relay_frame = data[end - 1u] != '\n';
                if (remote_passthrough(client, data + i, end - i) < 0) return -1;
                i = end - 1u;
                continue;
            }
            /* Restoration and subsequent UI output belong to screen's display. */
            client->relay_transfer = false;
        }
        if (!client->marker_len && byte != ':' && byte != 0x1bu) {
            size_t end = i + 1u;
            while (end < length && data[end] != ':' && data[end] != 0x1bu) ++end;
            if (remote_write(STDOUT_FILENO, data + i, end - i) < 0) return -1;
            i = end - 1u;
            continue;
        }
        client->marker[client->marker_len++] = byte;
        if (client->marker[0] == 0x1bu) {
            size_t n = client->marker_len;
            if (n <= sizeof(drop) - 1u && !memcmp(client->marker, drop, n)) continue;
            if (n == sizeof(drop) && !memcmp(client->marker, drop, n - 1u) &&
                (byte == 'h' || byte == 'l')) {
                if (client->relay) {
                    if (remote_passthrough(client, client->marker, n) < 0) return -1;
                } else client->drop_ready = byte == 'h';
                client->marker_len = 0;
                continue;
            }
            if (n <= sizeof(probe) - 1u && !memcmp(client->marker, probe, n)) continue;
            if (n > sizeof(probe) - 1u &&
                !memcmp(client->marker, probe, sizeof(probe) - 1u)) {
                if (n <= sizeof(probe) + 8u && byte >= '0' && byte <= '9') continue;
                if (byte == 'n' && n > sizeof(probe) && n <= sizeof(probe) + 9u) {
                    if (client->relay) {
                        if (remote_passthrough(client, client->marker, n) < 0) return -1;
                        client->marker_len = 0;
                        continue;
                    }
                    client->marker[n - 1u] = '\0';
                    char reply[16];
                    int count = snprintf(reply, sizeof(reply), "\033[>%sS",
                        client->marker + sizeof(probe) - 1u);
                    client->marker_len = 0;
                    if (count < 0 || (size_t)count >= sizeof(reply) ||
                        remote_write(client->child->fd[2], reply, (size_t)count) < 0) return -1;
                    continue;
                }
            }
            if (remote_write(STDOUT_FILENO, client->marker, n) < 0) return -1;
            client->marker_len = 0;
            continue;
        }
        bool candidate = client->marker_len >= sizeof(prefix) - 1u ||
            !memcmp(client->marker, prefix, client->marker_len);
        if (!candidate || client->marker_len == sizeof(client->marker)) {
            if (remote_write(STDOUT_FILENO, client->marker, client->marker_len) < 0) return -1;
            client->marker_len = 0;
        } else if (byte == '\n') {
            size_t marker_len = client->marker_len;
            client->marker[marker_len - 1u] = '\0';
            char direction, id[14] = {0};
            int used = 0;
            int matched = sscanf((char *)client->marker,
                "::TRZSZ:TRANSFER:%c:1.0.0:%13[0-9]:0%n", &direction, id, &used);
            bool valid = matched == 2 && (direction == 'R' || direction == 'S') &&
                strlen(id) == 13u && !strcmp(id + 11u, "00") && used > 0 &&
                (client->marker[used] == '\r' || !client->marker[used]);
            client->marker_len = 0;
            if (valid) {
                if (client->relay) {
                    client->marker[marker_len - 1u] = '\n';
                    if (remote_passthrough(client, client->marker, marker_len) < 0) return -1;
                    client->relay_transfer = true;
                } else if (remote_transfer_run(client, direction) < 0) return -1;
            } else {
                client->marker[marker_len - 1u] = '\n';
                if (remote_write(STDOUT_FILENO, client->marker, marker_len) < 0) return -1;
            }
        }
    }
    return 0;
}

static int
remote_upstream(struct remote_transfer *client)
{
    char id[SNAG_ID_HEX_LEN + 1u];
    if (snag_random_id(id) < 0) return -1;
    id[8] = '\0';
    unsigned long nonce = strtoul(id, NULL, 16) % 1000000000ul;
    char reply[16];
    int expected = snprintf(reply, sizeof(reply), "\033[>%luS", nonce);
    const char *sty = getenv("STY");
    unsigned attempts = sty && *sty ? 2u : 1u;
    for (unsigned attempt = 0; attempt < attempts; ++attempt) {
        char query[48];
        int size = snprintf(query, sizeof(query), "%s\033[?9001;%lun%s",
            attempt ? "\033P" : "", nonce, attempt ? "\033\\" : "");
        if (remote_write(STDOUT_FILENO, query, (size_t)size) < 0) return -1;
        uint64_t deadline = snag_monotonic_ms() + 1000u;
        while (snag_monotonic_ms() < deadline && !remote_signal) {
            struct pollfd input = {STDIN_FILENO, POLLIN, 0};
            int ready = poll(&input, 1u, 20);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0) return -1;
            if (!ready) continue;
            if (client->key_len == sizeof(client->keys)) return 0;
            ssize_t amount = read(STDIN_FILENO, client->keys + client->key_len,
                sizeof(client->keys) - client->key_len);
            if (amount <= 0) return amount < 0 ? -1 : 0;
            client->key_len += (size_t)amount;
            for (size_t at = 0; at + (size_t)expected <= client->key_len; ++at) {
                if (memcmp(client->keys + at, reply, (size_t)expected)) continue;
                memmove(client->keys + at, client->keys + at + expected,
                    client->key_len - at - (size_t)expected);
                client->key_len -= (size_t)expected;
                client->relay = true;
                return 0;
            }
        }
    }
    return 0;
}

static int
remote_proxy(const char *executable, const char *const *command, const char *downloads,
             char *error, size_t error_size)
{
    struct termios original, raw;
    struct snag_child child;
    snag_child_init(&child);
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) ||
        tcgetattr(STDIN_FILENO, &original) < 0) {
        return snag_fail(error, error_size, ENOTTY, "requires an interactive workstation terminal");
    }
    raw = original;
    raw.c_iflag &= (tcflag_t)~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_lflag &= (tcflag_t)~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    raw.c_cflag = (raw.c_cflag & (tcflag_t)~(CSIZE | PARENB)) | CS8;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) {
        snag_child_free(&child);
        return snag_errorf(error, error_size, "cannot enter client terminal mode: %s",
                            strerror(errno));
    }
    const int signals[] = {SIGWINCH, SIGTERM, SIGHUP, SIGINT};
    struct sigaction saved[4], action = {0};
    action.sa_handler = remote_interrupt;
    sigemptyset(&action.sa_mask);
    remote_signal = remote_resize = 0;
    size_t installed = 0u;
    for (; installed < sizeof(signals) / sizeof(signals[0]); ++installed) {
        if (sigaction(signals[installed], &action, &saved[installed]) < 0) break;
    }
    int rc = installed == sizeof(signals) / sizeof(signals[0]) ? 0 : -1;
    struct remote_transfer client = {.child = &child, .downloads = downloads, .drop_fd = -1};
    /* A fully escaped maximum-size path plus bracketed-paste framing. */
    snag_buf_init(&client.input, 2u * SNAG_PATH_MAX_BYTES + 12u);
    if (rc == 0) rc = remote_upstream(&client);
    const char *sty = getenv("STY");
    client.screen = sty && *sty;
    char *screen_backend = client.screen ? strdup(sty) : NULL;
    if (client.screen && !screen_backend) rc = -1;
    /* Each wrapper terminates the inherited backend at its child PTY. Relays
     * envelope probes and file frames for the parent screen, leaving UI ordinary. */
    if (rc == 0 && screen_backend) rc = unsetenv("STY");
    if (rc == 0) rc = snag_child_spawn_terminal(&child, executable, command);
    int spawn_errno = errno;
    if (screen_backend && setenv("STY", screen_backend, 1) < 0 && rc == 0) {
        spawn_errno = errno;
        rc = -1;
    }
    free(screen_backend);
    if (rc < 0) errno = spawn_errno;
    if (rc == 0 && client.key_len) {
        rc = remote_write(child.fd[2], client.keys, client.key_len);
        client.key_len = 0;
    }
    bool ended = false;
    while (rc == 0 && !ended && !remote_signal) {
        if (remote_resize) {
            remote_resize = 0;
            snag_child_resize(&child);
        }
        if (client.input.len && snag_monotonic_ms() - client.input_at >= 80u &&
            (client.input.len < 6u || memcmp(client.input.data, "\033[200~", 6u))) {
            if (remote_input_flush(&client, true) < 0) { rc = -1; break; }
        }
        if (client.drop_fd >= 0 && snag_monotonic_ms() - client.drop_at >= 5000u) {
            (void)close(client.drop_fd);
            client.drop_fd = -1;
            static const char failed[] = "\r\nRemote upload did not start; file was not sent.\r\n";
            if (remote_write(STDOUT_FILENO, failed, sizeof(failed) - 1u) < 0 ||
                remote_write(child.fd[2], client.keys, client.key_len) < 0) { rc = -1; break; }
            client.key_len = 0;
        }
        struct pollfd ready[] = {{child.fd[0], POLLIN, 0},
            {STDIN_FILENO, client.drop_fd < 0 ? POLLIN : 0, 0}};
        int count = poll(ready, 2u, client.input.len ? 20 : 100);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { rc = -1; break; }
        unsigned char bytes[8192];
        if (ready[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = snag_child_read(&child, 0u, bytes, sizeof(bytes));
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (n < 0) { rc = -1; break; }
            if (!n) ended = true;
            else if (remote_output(&client, bytes, (size_t)n) < 0) { rc = -1; break; }
        }
        if (!ended && (ready[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(STDIN_FILENO, bytes, sizeof(bytes));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { ended = true; break; }
            if (remote_input(&client, bytes, (size_t)n) < 0) { rc = -1; break; }
        }
        if (ready[0].revents & POLLNVAL) { errno = EIO; rc = -1; }
    }
    if (client.marker_len)
        (void)remote_write(STDOUT_FILENO, client.marker, client.marker_len);
    if (client.drop_fd >= 0) (void)close(client.drop_fd);
    snag_buf_free(&client.input);
    int reason = remote_signal;
    if (child.pid > 0 && (reason || rc < 0 || !ended)) {
        (void)kill(-child.pid, reason ? reason : SIGTERM);
    }
    uint64_t deadline = snag_monotonic_ms() + 1000u;
    while (snag_child_exited(&child) == 0 && snag_monotonic_ms() < deadline)
        (void)poll(NULL, 0u, 20);
    if (snag_child_exited(&child) == 1 && snag_child_reap(&child) == 0 && rc == 0)
        rc = child.signal_number > 0 ? 128 + child.signal_number : (int)child.exit_code;
    else if (rc == 0) rc = reason ? 128 + reason : 1;
    int saved_errno = errno;
    snag_child_free(&child);
    while (installed) {
        --installed;
        (void)sigaction(signals[installed], &saved[installed], NULL);
    }
    if (tcsetattr(STDIN_FILENO, TCSANOW, &original) < 0 && rc == 0) rc = -1;
    if (rc < 0)
        (void)snag_errorf(error, error_size, "client terminal failed: %s", strerror(saved_errno));
    return rc;
}

int
snag_remote_main(int argc, char **argv)
{
    char error[256] = "invalid client options";
    const char *config_path = NULL, *dotdir = NULL;
    int at = 0;
    while (at < argc && argv[at][0] == '-') {
        if (!strcmp(argv[at], "--")) { ++at; break; }
        if (!strcmp(argv[at], "--help") || !strcmp(argv[at], "-h")) {
            remote_usage();
            return argc == 1 ? 0 : 2;
        }
        if (at + 1 >= argc) { remote_usage(); return 2; }
        if (!strcmp(argv[at], "--config") && !config_path) config_path = argv[at + 1];
        else if (!strcmp(argv[at], "--dotdir") && !dotdir) dotdir = argv[at + 1];
        else { remote_usage(); return 2; }
        at += 2;
    }
    char *home = snag_home_directory();
    if (!home) {
        (void)fprintf(stderr, "snajpagent remote: workstation home is unavailable\n");
        return 2;
    }
    struct snag_buf default_dotdir;
    snag_buf_init(&default_dotdir, SNAG_CONFIG_PATH_MAX);
    if (snag_buf_printf(&default_dotdir, "%s/.snajpagent", home) < 0 ||
        snag_buf_terminate(&default_dotdir) < 0) {
        free(home);
        snag_buf_free(&default_dotdir);
        return 2;
    }
    char downloads[SNAG_CONFIG_PATH_MAX + 1u];
    if (snag_config_terminal(config_path, dotdir ? dotdir : (char *)default_dotdir.data,
                             downloads, sizeof(downloads), error, sizeof(error)) < 0) {
        (void)fprintf(stderr, "snajpagent remote: %s\n", error);
        free(home);
        snag_buf_free(&default_dotdir);
        return 2;
    }
    free(home);
    snag_buf_free(&default_dotdir);
    const char *shell = getenv("SHELL");
    if (!shell || !*shell) shell = "/bin/sh";
    char *shell_args[] = {(char *)shell, NULL};
    char **arguments = at < argc ? argv + at : shell_args;
    char *executable = snag_program_path(arguments[0]);
    if (!executable || !snag_path_root_len(executable) || snag_file_executable(executable) < 0) {
        (void)fprintf(stderr, "snajpagent remote: cannot execute %s\n", arguments[0]);
        free(executable);
        return 127;
    }
    int rc = remote_proxy(executable, (const char *const *)arguments, downloads,
                          error, sizeof(error));
    free(executable);
    if (rc < 0) { (void)fprintf(stderr, "snajpagent remote: %s\n", error); return 1; }
    return rc;
}
#endif
