#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise terminal/process portability guards and compiler ABI regressions."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('portability', ROOT / 'tools/check_portability.py')
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)


class PortabilityTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'C compiler required for legacy directory API regression')
    def test_frontend_history_uses_portable_directory_opens(self):
        source = (ROOT / 'src/ui_frontend.c').read_text()
        root = source.split('    store->root_fd = ', 1)[1].split(';', 1)[0]
        sessions = source.split('    store->sessions_fd = ', 1)[1].split(';', 1)[0]
        body = ('typedef _Bool bool;\n#define true 1\n'
                'struct store { int root_fd, sessions_fd; };\n'
                'extern int snag_open_read(const char *, bool);\n'
                'extern int snag_open_read_at(int, const char *, bool);\n'
                'void frontend_directories(struct store *store, const char *root) {\n'
                'store->root_fd = ' + root + ';\n'
                'store->sessions_fd = ' + sessions + ';\n}\n')
        with tempfile.TemporaryDirectory(prefix='snag-frontend-open-') as directory:
            path = Path(directory) / 'directories.c'
            path.write_text(body)
            result = subprocess.run(
                ['cc', '-std=c99', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-c', str(path),
                 '-o', os.devnull], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)

    @unittest.skipUnless(shutil.which('cc'), 'C compiler required for Windows poll API regression')
    def test_send_wait_compiles_without_posix_poll(self):
        source = (ROOT / 'src/send.c').read_text()
        start = source.index(' fd = {.fd = channel->fd,')
        wait = source[source.rfind('\n', 0, start) + 1:source.index('\n    }\n', start)]
        body = ('typedef __SIZE_TYPE__ size_t;\ntypedef __UINT64_TYPE__ uint64_t;\n'
                'struct snag_view_channel { int fd; void *output; };\n'
                'typedef struct { int fd; short events, revents; } snag_socket_event;\n'
                '#define SNAG_NET_READ 1\n#define SNAG_NET_WRITE 4\n#define EINTR 4\n'
                'extern int errno;\nextern uint64_t snag_monotonic_ms(void);\n'
                'extern int snag_socket_poll(snag_socket_event *, size_t, int);\n'
                'void send_wait(struct snag_view_channel *channel, uint64_t deadline) {\n'
                'while (1) {\n' + wait + '\nbreak;\n}\n}\n')
        with tempfile.TemporaryDirectory(prefix='snag-send-poll-') as directory:
            path = Path(directory) / 'wait.c'
            path.write_text(body)
            result = subprocess.run(
                ['cc', '-std=c99', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-c', str(path),
                 '-o', os.devnull], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('#include "net.h"', source)
        self.assertNotIn('#include <poll.h>', source)

    @unittest.skipUnless(shutil.which('cc'), 'C compiler required for legacy string API regression')
    def test_prompt_value_length_compiles_without_strnlen(self):
        source = (ROOT / 'src/ui.c').read_text().split('static int\nsend_prompt(', 1)[1]
        scan = source.split('        size_t size = ', 1)[1].split(
            '        if (size > limit', 1)[0]
        body = ('typedef __SIZE_TYPE__ size_t;\n'
                'size_t prompt_value_length(const char *const values[], size_t limit) {\n'
                'size_t i = 0u;\nsize_t size = ' + scan + 'return size;\n}\n'
                'int main(void) {\nchar bounded[4] = {\'a\', \'b\', \'c\', \'d\'};\n'
                'const char *values[] = {bounded};\n'
                'if (prompt_value_length(values, 3u) != 4u) return 1;\n'
                'values[0] = "abc";\nif (prompt_value_length(values, 3u) != 3u) return 2;\n'
                'values[0] = "";\nif (prompt_value_length(values, 0u) != 0u) return 3;\n'
                'values[0] = "x";\nif (prompt_value_length(values, 0u) != 1u) return 4;\n'
                'return 0;\n}\n')
        with tempfile.TemporaryDirectory(prefix='snag-prompt-string-') as directory:
            path = Path(directory) / 'prompt.c'
            output = Path(directory) / 'prompt'
            path.write_text(body)
            result = subprocess.run(
                ['cc', '-std=c99', '-Wall', '-Wextra', '-Wpedantic', '-Werror', str(path),
                 '-o', str(output)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(output)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_runtime_curl_patch_does_not_modify_build_tools(self):
        overlay = (ROOT / 'nix/source-mirrors.nix').read_text()
        self.assertNotIn('curl-mbedtls-write.patch', overlay)
        for name in ('linux', 'macos', 'windows', 'freebsd', 'openbsd', 'netbsd', 'android'):
            with self.subTest(recipe=name):
                source = (ROOT / 'nix' / (name + '.nix')).read_text()
                curl = source.split('  curl = ', 1)[1].split('  application = ', 1)[0]
                self.assertIn('patches = (old.patches or []) ++ [ ./curl-mbedtls-write.patch ]',
                              curl)

    @unittest.skipUnless(shutil.which('clang'), 'Clang required for 32-bit compile regression')
    def test_top_settings_compile_with_32_bit_long(self):
        source = (ROOT / 'src/config.c').read_text()
        settings = (ROOT / 'src/config.h').read_text().split(
            '#define SNAG_TOP_FIELDS ', 1)[1].split('\nint ', 1)[0]
        function = source.split('static int\ntop_setting(', 1)[1].split('\n}\n', 1)[0]
        keys = source.split('static const char *const top_keys[] = ', 1)[1].split(';', 1)[0]
        body = ('typedef __SIZE_TYPE__ size_t;\ntypedef _Bool bool;\n'
                '#define true 1\n#define false 0\n#define INT_MAX __INT_MAX__\n'
                'extern int errno;\nextern size_t strlen(const char *);\n'
                'extern int strcmp(const char *, const char *);\n'
                'extern long strtol(const char *, char **, int);\n'
                '#define SNAG_TOP_FIELDS ' + settings + '\n'
                'static const char *const top_keys[] = ' + keys + ';\n'
                'int top_setting(' + function + '\n}\n')
        with tempfile.TemporaryDirectory(prefix='snag-top-abi-') as directory:
            path = Path(directory) / 'top.c'
            path.write_text(body)
            for target in ('i386-linux-gnu', 'armv6-linux-gnueabihf', 'x86_64-linux-gnu'):
                with self.subTest(target=target):
                    result = subprocess.run(
                        ['clang', '--target=' + target, '-ffreestanding', '-std=c11',
                         '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-c', str(path),
                         '-o', os.devnull],
                        capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stderr)

    @unittest.skipUnless(shutil.which('cc'), 'C compiler required for ioctl ABI regression')
    def test_workspace_resize_compiles_with_int_ioctl_request(self):
        source = (ROOT / 'src/vm_terminal.c').read_text()
        function = source.split('int\nsnag_vm_terminal_resize(', 1)[1].split('\n}\n', 1)[0]
        body = ('#define USHRT_MAX 65535\n#define EINVAL 22\n'
                '#define TIOCSWINSZ ((4U << 29) | (116 << 8) | 103 | '
                '(sizeof(struct winsize) << 16))\n'
                'struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };\n'
                'struct snag_vm_terminal { void *terminal; int fd, error;\n'
                '    struct { unsigned short rows, columns; } child; };\n'
                'extern int snag_errno(int);\nextern int ioctl(int, int, ...);\n'
                'extern void vterm_get_size(void *, int *, int *);\n'
                'extern void vterm_set_size(void *, int, int);\n'
                'extern int vterm_get_error(void *);\n'
                'int snag_vm_terminal_resize(' + function + '\n}\n')
        with tempfile.TemporaryDirectory(prefix='snag-resize-abi-') as directory:
            path = Path(directory) / 'resize.c'
            path.write_text(body)
            result = subprocess.run(
                ['cc', '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-c', str(path),
                 '-o', os.devnull], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_recursive_make_preserves_build_configuration(self):
        with tempfile.TemporaryDirectory(prefix='snag-make-flags-') as directory:
            root = Path(directory)
            for name in ('Makefile', 'config.mk', 'META'):
                shutil.copy2(ROOT / name, root / name)
            (root / 'flags.mk').write_text(
                'BUILD_VERSION ?= fixture\ninclude Makefile\n'
                'export FIXTURE_CC = $(CC)\nexport FIXTURE_CFLAGS = $(CFLAGS)\n'
                'flags-parent:\n'
                '\t$(MAKE) --no-print-directory -f flags.mk flags-child\n'
                'flags-child:\n\t@python3 verify.py\n', encoding='utf-8')
            (root / 'verify.py').write_text(
                'import os\n'
                'assert os.environ["FIXTURE_CC"] == "compiler fixture", '
                '"recursive build lost compiler"\n'
                'assert os.environ["FIXTURE_CFLAGS"] == '
                '\'-O1 -DVALUE="hello world" -pthread\', '
                '"recursive build lost flags"\n', encoding='utf-8')
            environment = {key: value for key, value in os.environ.items()
                           if key not in ('MAKEFLAGS', 'MFLAGS', 'MAKEOVERRIDES')}
            command = ['make', '--no-print-directory', '-f', 'flags.mk', 'flags-parent',
                       'CC=compiler fixture', 'CFLAGS=-O1 -DVALUE="hello world"',
                       'WITH_AV=0', 'WITH_PDF=0', 'WITH_AUDIO_DEVICE=0',
                       'HAVE_POPPLER_NEW_API=0', 'PKG_CONFIG=false', 'JANSSON_CFLAGS=',
                       'JANSSON_LIBS=', 'YAML_CFLAGS=', 'YAML_LIBS=', 'ZLIB_LIBS=',
                       'ZSTD_CFLAGS=', 'ZSTD_LIBS=', 'VTERM_CFLAGS=', 'VTERM_LIBS=']
            result = subprocess.run(command, cwd=root, env=environment, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout)

    def check_source(self, before=None, after=None):
        with tempfile.TemporaryDirectory(prefix='snag-portability-') as directory:
            root = Path(directory)
            (root / 'src').mkdir()
            for name in ('src/process_host.c', 'config.mk',
                         'IMPLEMENTATION_STATUS.md', 'QUALIFICATION.md'):
                text = (ROOT / name).read_text(encoding='utf-8')
                if name == 'src/process_host.c' and before is not None:
                    self.assertEqual(text.count(before), 1)
                    text = text.replace(before, after, 1)
                (root / name).write_text(text, encoding='utf-8')
            with mock.patch.object(CHECKER, 'ROOT', root), contextlib.redirect_stdout(io.StringIO()):
                return CHECKER.main()

    def test_current_native_and_managed_pty_paths_pass(self):
        self.assertEqual(self.check_source(), 0)

    def test_single_platform_guard_is_rejected(self):
        before = ('#if defined(SNAJPAGENT_HAVE_PTY)\n'
                  '            if (argv) exec_terminal_child')
        after = before.replace('SNAJPAGENT_HAVE_PTY', '__linux__')
        with self.assertRaisesRegex(SystemExit, 'capability guard and fallback'):
            self.check_source(before, after)

    def test_private_channel_argument_cannot_disappear(self):
        before = 'if (argv) exec_terminal_child(shell, argv, slave, environment, channel);'
        after = 'if (argv) exec_terminal_child(shell, argv, slave);'
        with self.assertRaisesRegex(SystemExit, 'capability guard and fallback'):
            self.check_source(before, after)

    def test_missing_unsupported_platform_exit_is_rejected(self):
        before = ('            exec_pty_child(shell, command, argv, directory, slave, environment);\n'
                  '#else\n            _exit(125);\n#endif')
        after = before.replace('_exit(125);', '(void)slave;')
        with self.assertRaisesRegex(SystemExit, 'capability guard and fallback'):
            self.check_source(before, after)

    def test_mac_pty_capability_cannot_disappear(self):
        before = ('#elif defined(__APPLE__) || defined(__OpenBSD__) || defined(__NetBSD__)\n'
                  '#define SNAJPAGENT_HAVE_PTY 1')
        after = before.replace('\n#define SNAJPAGENT_HAVE_PTY 1', '')
        with self.assertRaisesRegex(SystemExit, 'macOS PTY branch'):
            self.check_source(before, after)


if __name__ == '__main__':
    unittest.main()
