#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise closure detection with real static, dynamic and invalid ELF inputs."""
import json
import platform
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(platform.system() == "Linux", "ELF loader checks")
class ClosureTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="snag-closure-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "main.c"
        self.source.write_text("int main(void) { return 0; }\n")
        self.cc = "/usr/bin/cc" if Path("/usr/bin/cc").exists() else shutil.which("cc")
        self.assertTrue(self.cc)

    def check_file(self, path):
        return subprocess.run([sys.executable, str(ROOT / "tools/check_dependency_closure.py"),
                               str(path), "--json-out", str(self.root / "report.json")],
                              capture_output=True, text=True, timeout=15)

    def test_static_executable_has_no_runtime_closure(self):
        binary = self.root / "static"
        subprocess.run([self.cc, "-static", str(self.source), "-o", str(binary)], check=True)
        result = self.check_file(binary)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads((self.root / "report.json").read_text())
        self.assertEqual(report["linkage"], "static")
        self.assertEqual(report["dependencies"], [])
        self.assertEqual(report["direct_needed"], [])

    def test_dynamic_executable_still_requires_provider_libraries(self):
        binary = self.root / "dynamic"
        subprocess.run([self.cc, str(self.source), "-o", str(binary)], check=True)
        result = self.check_file(binary)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("dynamic closure does not include libcurl", result.stderr)

    def test_object_and_text_are_not_static_executables(self):
        obj = self.root / "main.o"
        subprocess.run([self.cc, "-c", str(self.source), "-o", str(obj)], check=True)
        for path in (obj, self.source):
            with self.subTest(path=path.name):
                self.assertNotEqual(self.check_file(path).returncode, 0)


if __name__ == "__main__":
    unittest.main()
