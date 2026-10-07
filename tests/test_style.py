#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the actual style checker in an archive without Git history."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

CHECKER = pathlib.Path(__file__).resolve().parents[1] / "tools/check_style.py"


class StyleTests(unittest.TestCase):
    def check_source(self, source):
        with tempfile.TemporaryDirectory(prefix="snag-style-") as directory:
            root = pathlib.Path(directory)
            (root / "src").mkdir()
            (root / "src/existing.c").write_text(source)
            return subprocess.run([sys.executable, str(CHECKER)], cwd=root,
                                  capture_output=True, text=True, timeout=10)

    def test_default_rejects_existing_minified_code(self):
        result = self.check_source("void f(void) { if(1) return; }\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn("keyword needs a space", result.stderr)

    def test_default_rejects_existing_long_line(self):
        result = self.check_source("int x = " + "1 + " * 30 + "0;\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn("hard limit 100", result.stderr)

    def test_literals_and_block_comments_keep_their_contents(self):
        result = self.check_source(
            'const char *url = "https://example.invalid/if(//";\n'
            'const char *long_token = "' + "x" * 120 + '";\n'
            '/* while( and // here are text */\n'
            'void\nf(void)\n{\n    if (1) return;\n}\n')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_default_still_rejects_line_comments(self):
        result = self.check_source("// invalid comment\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn("// comment", result.stderr)


if __name__ == "__main__":
    unittest.main()
