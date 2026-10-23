#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Keep the portability guard sensitive to both native PTY branches."""
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
