/* SPDX-License-Identifier: GPL-2.0-only */
#include "remote.h"
#include "base.h"
#include "config.h"
#include "fs.h"
#include "process_host.h"
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
        "Example: snajpagent remote ssh -t snajpadev screen -S sessionname snajpagent\n");
}

struct remote_transfer {
    struct snag_child *child;
    const char *downloads;
    unsigned char marker[128];
    size_t marker_len;
    unsigned char keys[4096];
    size_t key_len;
    bool remote_ready_sent;
};

static int
remote_checkpoint(void *opaque)
{
    struct remote_transfer *client = opaque;
    if (remote_signal) return 1;
    if (remote_resize) { remote_resize = 0; snag_child_resize(client->child); }
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

static bool
remote_contains(const unsigned char *data, size_t length, const char *needle)
{
    size_t n = strlen(needle);
    if (!n || length < n) return false;
    for (size_t i = 0; i <= length - n; ++i)
        if (memcmp(data + i, needle, n) == 0) return true;
    return false;
}

static void
remote_maybe_announce(struct remote_transfer *client, const unsigned char *data, size_t length)
{
    static const unsigned char ready[] = "\033[>S";
    if (client->remote_ready_sent) return;
    if (!remote_contains(data, length, "snajpagent ") &&
        !remote_contains(data, length, " \342\200\272 ")) return;
    if (remote_write(client->child->fd[2], ready, sizeof(ready) - 1u) == 0)
        client->remote_ready_sent = true;
}

static int
remote_transfer_run(struct remote_transfer *client, char direction)
{
    struct snag_client_result result;
    char error[256] = {0};
    int rc = -1, fd = -1;
    char *directory = NULL;
    if (direction == 'S') {
        fd = remote_download_root(client, &directory);
        if (fd >= 0) rc = snag_client_download(client->child->fd[0], fd, directory,
            remote_checkpoint, client, &result, error, sizeof(error));
    } else {
        char path[SNAG_PATH_MAX_BYTES + 1u];
        if (remote_upload_choice(client, path, sizeof(path)) == 0 && path[0]) {
            fd = snag_open_read(path, false);
            const char *name = strrchr(path, '/');
            if (fd >= 0) rc = snag_client_upload(client->child->fd[0], fd,
                name ? name + 1u : path, remote_checkpoint, client, &result, error, sizeof(error));
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
    if (client->key_len) {
        (void)remote_write(client->child->fd[2], client->keys, client->key_len);
        client->key_len = 0;
    }
    return 0;
}

static int
remote_output(struct remote_transfer *client, const unsigned char *data, size_t length)
{
    static const char prefix[] = "::TRZSZ:TRANSFER:";
    for (size_t i = 0; i < length; ++i) {
        unsigned char byte = data[i];
        if (!client->marker_len && byte != ':') {
            size_t end = i + 1u;
            while (end < length && data[end] != ':') ++end;
            if (remote_write(STDOUT_FILENO, data + i, end - i) < 0) return -1;
            remote_maybe_announce(client, data + i, end - i);
            i = end - 1u;
            continue;
        }
        client->marker[client->marker_len++] = byte;
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
                if (remote_transfer_run(client, direction) < 0) return -1;
            } else {
                client->marker[marker_len - 1u] = '\n';
                if (remote_write(STDOUT_FILENO, client->marker, marker_len) < 0) return -1;
                remote_maybe_announce(client, client->marker, marker_len);
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
    if (snag_child_spawn_terminal(&child, executable, command) < 0) {
        return snag_errorf(error, error_size, "cannot start child terminal: %s", strerror(errno));
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
    struct remote_transfer client = {.child = &child, .downloads = downloads};
    bool ended = false;
    while (rc == 0 && !ended && !remote_signal) {
        if (remote_resize) {
            remote_resize = 0;
            snag_child_resize(&child);
        }
        struct pollfd ready[] = {{child.fd[0], POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
        int count = poll(ready, 2u, 100);
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
            if (remote_write(child.fd[2], bytes, (size_t)n) < 0) { rc = -1; break; }
        }
        if (ready[0].revents & POLLNVAL) { errno = EIO; rc = -1; }
    }
    if (client.marker_len)
        (void)remote_write(STDOUT_FILENO, client.marker, client.marker_len);
    int reason = remote_signal;
    if (reason || rc < 0 || !ended) {
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
