#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Stopped collection conversion keeps originals and independent outcomes."""
import fcntl
import os
import signal
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from store_history import create_legacy, read_events
from test_session_listing import append_event

BINARY = Path(sys.argv.pop(1)).resolve()
FILES = ("journal.bin", "history.idx", "checkpoint.0", "checkpoint.1")


class Conversion(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="snag-conversion-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.dotdir = self.root / "dotdir"
        self.cwd = self.root / "cwd"
        self.dotdir.mkdir(mode=0o700)
        self.cwd.mkdir(mode=0o700)

    def seed(self):
        return create_legacy(self.dotdir, self.cwd, "fake", "fixture")

    def convert(self, *options, expected=0):
        result = subprocess.run([str(BINARY), "convert", "--dotdir", str(self.dotdir),
                                 *options], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result.stdout

    def retained(self, source, before):
        self.assertFalse(source.exists())
        saved = source.parent / ".legacy-source" / source.name
        self.assertEqual(saved.read_bytes(), before)
        for name in FILES:
            self.assertTrue((source.parent / name).is_file(), name)
        return source.parent / "journal.bin"

    def test_parallel_conversion_and_current_read_only(self):
        sources = [self.seed() for _ in range(3)]
        for i, source in enumerate(sources):
            append_event(source, "goal_started", {"goal_id": f"{i + 1:032x}",
                                                  "prompt": f"conversion goal {i}"})
        before = {source: source.read_bytes() for source in sources}
        output = self.convert("--jobs", "2")
        self.assertIn("Conversion workers: 2", output)
        self.assertIn("Converted: 3; already-current: 0; skipped: 0; failed: 0", output)
        for i, source in enumerate(sources):
            journal = self.retained(source, before[source])
            goal = next(row for row in read_events(journal) if row["type"] == "goal_started")
            self.assertEqual(goal["data"]["prompt"], f"conversion goal {i}")
        native = {source.parent / name: (source.parent / name).read_bytes()
                  for source in sources for name in FILES}
        output = self.convert("--jobs=99")
        self.assertIn("Conversion workers: 3", output)
        self.assertIn("Converted: 0; already-current: 3; skipped: 0; failed: 0", output)
        self.assertEqual(native, {path: path.read_bytes() for path in native})

    def test_locked_and_failed_sessions_do_not_stop_other_work(self):
        locked, broken, good = (self.seed() for _ in range(3))
        broken.write_bytes(b"invalid canonical journal\n")
        before = {path: path.read_bytes() for path in (locked, broken, good)}
        with (locked.parent / "lock").open("r+b") as held:
            fcntl.lockf(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
            output = self.convert("--jobs", "3", expected=1)
        self.assertIn(f"{locked.parent.name} skipped: locked", output)
        self.assertIn(f"{broken.parent.name} failed:", output)
        self.assertIn("Converted: 1; already-current: 0; skipped: 1; failed: 1", output)
        for path in (locked, broken):
            self.assertEqual(path.read_bytes(), before[path])
            self.assertFalse((path.parent / "journal.bin").exists())
        self.retained(good, before[good])

    def test_interrupted_retention_repairs_from_original(self):
        source = self.seed()
        before = source.read_bytes()
        old = source.parent / ".legacy-source"
        old.mkdir(mode=0o700)
        source.rename(old / source.name)
        output = self.convert("--jobs", "1")
        self.assertIn("Converted: 1; already-current: 0; skipped: 0; failed: 0", output)
        self.retained(source, before)

    def test_unsealed_legacy_tail_stays_in_retained_source(self):
        source = self.seed()
        with source.open("ab") as stream:
            stream.write(b'{"v":12')
        before = source.read_bytes()
        output = self.convert("--jobs", "1")
        self.assertIn("unsealed source tail retained", output)
        self.retained(source, before)

    def test_native_tail_is_not_truncated_or_reported_current(self):
        source = self.seed()
        self.convert("--jobs", "1")
        native = source.parent / "journal.bin"
        with native.open("ab") as stream:
            stream.write(b"unverified tail")
        before = native.read_bytes()
        output = self.convert("--jobs", "1", expected=1)
        self.assertIn("Converted: 0; already-current: 0; skipped: 0; failed: 1", output)
        self.assertIn("unverified tail", output)
        self.assertEqual(native.read_bytes(), before)

    def test_invalid_derived_checkpoint_rebuilds_from_canonical_records(self):
        source = self.seed()
        append_event(source, "goal_started", {"goal_id": "1" * 32,
                                              "prompt": "canonical goal survives"})
        append_event(source, "session_checkpoint", {"format": 4, "snapshot_v": 999,
                                                    "state": None, "context": None})
        before = source.read_bytes()
        self.convert("--jobs", "1")
        native = self.retained(source, before)
        goal = next(row for row in read_events(native) if row["type"] == "goal_started")
        self.assertEqual(goal["data"]["prompt"], "canonical goal survives")

    def test_interrupt_preserves_source_and_reports_interrupted(self):
        source = self.seed()
        for i in range(256):
            append_event(source, "rule_log", {"chain": None, "message": "x" * 4096, "rule": i})
        before = source.read_bytes()
        process = subprocess.Popen([str(BINARY), "convert", "--dotdir", str(self.dotdir),
                                    "--jobs", "1"], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        try:
            self.assertIn("Conversion workers: 1", process.stdout.readline())
            self.assertIn("checking", process.stdout.readline())
            process.send_signal(signal.SIGINT)
            output, error = process.communicate(timeout=30)
            self.assertEqual(process.returncode, 130, output + error)
            self.assertIn("; interrupted", output)
            self.assertEqual(source.read_bytes(), before)
            self.assertFalse((source.parent / "journal.bin").exists())
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stdout.close()
            process.stderr.close()

    def test_help_empty_collection_and_bad_jobs(self):
        result = subprocess.run([str(BINARY), "convert", "--help"], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0)
        self.assertIn("--jobs N", result.stdout)
        self.assertIn("Converted: 0; already-current: 0; skipped: 0; failed: 0", self.convert())
        for jobs in ("0", "bad", "-1"):
            self.convert("--jobs", jobs, expected=2)


if __name__ == "__main__":
    unittest.main()
