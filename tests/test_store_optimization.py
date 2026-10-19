#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Offline compression preserves records and supports ordinary resume/appends."""
import fcntl
import hashlib
import json
import os
import shlex
from pathlib import Path
import signal
import struct
import subprocess
import sys
import tempfile
import unittest
import shutil

from store_history import create_legacy, read_events, READER
from test_session_listing import canonical
from tmux_terminal import FakeResponses, write_irc_config

BINARY = Path(sys.argv.pop(1)).resolve()


def frames(path):
    """Decode just the fixed-block wire envelope, independently of the C reader."""
    data = path.read_bytes()
    assert data[:8] == b"SNAGJNL\0"
    offset = 96
    while offset < len(data):
        end = data.index(0, offset)
        wire = data[offset:end]
        decoded = bytearray()
        for block in range(0, len(wire), 255):
            chunk = wire[block:block + 255]
            at = 0
            while at < len(chunk):
                count = chunk[at]
                decoded.extend(chunk[at + 1:at + count])
                at += count
                if at < len(chunk):
                    decoded.append(0)
        assert decoded[:8] == b"SNAGBAT\0"
        yield offset, end + 1, decoded
        offset = end + 1


def storage_records(path):
    return subprocess.check_output([str(READER), "--storage-records", str(path)], timeout=30)


class Optimization(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="snag-optimize-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.dotdir = self.root / "dotdir"
        self.dotdir.mkdir(mode=0o700)

    def seed(self, count=20):
        source = create_legacy(self.dotdir, self.root, "fake", "fixture")
        previous = json.loads(source.read_bytes().splitlines()[-1])
        with source.open("ab") as stream:
            for i in range(count):
                event = dict(data={"chain": None, "message": "žluťoučký payload " * 1024, "rule": i},
                             prev_sha256=previous["event_sha256"], seq=previous["seq"] + 1,
                             session_id=previous["session_id"], time_ms=previous["time_ms"] + 1,
                             type="rule_log", v=previous["v"])
                if "checkpoint_offset" in previous:
                    event["checkpoint_offset"] = previous["checkpoint_offset"]
                event["event_sha256"] = hashlib.sha256(canonical(event).encode()).hexdigest()
                stream.write(canonical(event).encode() + b"\n")
                previous = event
        return source

    def run_command(self, command="optimize", expected=0, **options):
        result = subprocess.run([str(BINARY), command, "--dotdir", str(self.dotdir)],
                                capture_output=True, text=True, timeout=60, **options)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result

    def test_legacy_archive_and_native_records_are_lossless(self):
        source = self.seed(70)
        with source.open("ab") as stream:
            stream.write(b'{"unsealed":"exact original tail')
        original = source.read_bytes()
        self.run_command("convert")
        native = source.parent / "journal.bin"
        records = storage_records(native)
        files_before = sum(p.stat().st_size for p in source.parent.rglob("*") if p.is_file())
        subprocess.run([str(READER), "--append-optional", str(native)], check=True, timeout=30)
        records = storage_records(native)
        result = self.run_command()
        self.assertIn("Optimized: 1", result.stdout)
        self.assertFalse(result.stderr)
        self.assertEqual(native.read_bytes()[10:12], b"\3\0")
        self.assertTrue(storage_records(native).startswith(records))
        archive = source.parent / ".legacy-source/events.jsonl.zst"
        self.assertEqual(subprocess.check_output([str(READER), "--archive-bytes", str(archive)]),
                         original)
        self.assertFalse(archive.with_suffix("").exists())
        self.assertEqual(sum(e["type"] == "rule_log" for e in read_events(native)), 70)
        sizes = [len(frame) for _, _, frame in frames(native)]
        self.assertTrue(all(size < 10000 for size in sizes), sizes)
        files_after = sum(p.stat().st_size for p in source.parent.rglob("*") if p.is_file())
        self.assertLess(files_after, files_before // 10)
        records = storage_records(native)
        self.run_command()
        self.assertTrue(storage_records(native).startswith(records))
        self.assertFalse((source.parent / ".optimize").exists())

    def test_locked_session_skips_on_stderr_and_keeps_lock_inode(self):
        locked, good = self.seed(1), self.seed(1)
        before = locked.read_bytes()
        inode = (good.parent / "lock").stat().st_ino
        with (locked.parent / "lock").open("r+b") as held:
            fcntl.lockf(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = self.run_command()
        self.assertIn(f"{locked.parent.name} skipped: locked", result.stderr)
        self.assertNotIn("skipped: locked", result.stdout)
        self.assertIn("Optimized: 1; already-current: 0; skipped: 1; failed: 0", result.stdout)
        self.assertEqual(locked.read_bytes(), before)
        self.assertFalse((locked.parent / ".optimize").exists())
        self.assertEqual((good.parent / "lock").stat().st_ino, inode)

    def test_corruption_and_incomplete_native_tail_leave_source_untouched(self):
        source = self.seed()
        self.run_command()
        native = source.parent / "journal.bin"
        valid = native.read_bytes()
        for damaged in (valid + b"unverified tail", valid[:240] + bytes([valid[240] ^ 1]) + valid[241:]):
            native.write_bytes(damaged)
            result = self.run_command(expected=1)
            self.assertIn(f"{native.parent.name} failed:", result.stderr)
            self.assertEqual(native.read_bytes(), damaged)
            self.assertFalse((native.parent / ".optimize").exists())
        native.write_bytes(valid)
        self.run_command()

    def test_resume_then_raw_append_and_reoptimize(self):
        provider = FakeResponses()
        self.addCleanup(provider.close)
        provider.runtime_handler = lambda handler, request, sequence: provider.reply(
            handler, provider.response_body(sequence, "optimization fixture reply").encode())
        config = self.root / "config.ini"
        write_irc_config(config, provider.port, "host-model")
        env = {**os.environ, "HOME": str(self.root), "SNAJPAGENT_IRC_UI_KEY": "irc-ui-secret"}
        prefix = [str(BINARY), "--config", str(config), "--dotdir", str(self.dotdir)]
        result = subprocess.run(prefix + ["-N", "optimized", "-e", "--", "saved input " * 1000],
                                cwd=self.root, env=env, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        native = next(self.dotdir.glob("sessions/*/journal.bin"))
        original_records = storage_records(native)
        self.run_command()
        packed = native.read_bytes()
        self.assertTrue(storage_records(native).startswith(original_records))
        self.assertTrue(any(struct.unpack_from("<Q", frame, 72)[0] for _, _, frame in frames(native)))
        result = subprocess.run(prefix + ["--resume", native.parent.name, "-e", "--", "resumed input"],
                                cwd=self.root, env=env, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(native.read_bytes()[:len(packed)], packed)
        appended = [frame for start, _, frame in frames(native) if start >= len(packed)]
        self.assertTrue(appended)
        self.assertTrue(all(not struct.unpack_from("<Q", frame, 72)[0] for frame in appended))
        records = storage_records(native)
        self.run_command()
        self.assertTrue(storage_records(native).startswith(records))
        self.assertIn("resumed input", [row["data"]["text"] for row in read_events(native)
                                       if row["type"] == "turn_started"])
        self.assertFalse(provider.failure, provider.failure)

    def test_help_and_serial_interface(self):
        result = subprocess.run([str(BINARY), "optimize", "--help"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0)
        self.assertIn("one at a time", result.stdout)
        result = subprocess.run([str(BINARY), "optimize", "--jobs", "2"], capture_output=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("Optimized: 0", self.run_command().stdout)

    @unittest.skipUnless(sys.platform == "linux" and os.uname().machine == "x86_64",
                         "native pread instrumentation is Linux x86_64")
    def test_resume_reads_only_checkpoint_and_bounded_blocks(self):
        # A real context rebase leaves a fixed recent-provider overlap. Growing
        # its archived prefix must not make resume replay all older blocks.
        costs = []
        for count in (1000, 2000):
            source = self.seed(count)
            subprocess.run([str(READER), "--rebase-context", str(self.dotdir),
                            source.parent.name], check=True, timeout=30)
            self.run_command()
            result = subprocess.check_output([str(READER), "--resume-reads", str(self.dotdir),
                                              source.parent.name], text=True, timeout=30)
            calls, size = map(int, result.split())
            self.assertLess(calls, 256)
            self.assertLess(size, 1024 * 1024)
            costs.append((calls, size))
        self.assertLessEqual(costs[1][0], costs[0][0] + 20)

    def test_interrupt_preserves_original_and_returns_interrupted(self):
        source = self.seed(100)
        original = source.read_bytes()
        process = subprocess.Popen([str(BINARY), "optimize", "--dotdir", str(self.dotdir)],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            self.assertIn("checking", process.stdout.readline())
            process.send_signal(signal.SIGINT)
            output, error = process.communicate(timeout=30)
            self.assertEqual(process.returncode, 130, output + error)
            self.assertIn("; interrupted", output)
            self.assertEqual(source.read_bytes(), original)
            self.assertFalse((source.parent / ".optimize").exists())
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stdout.close()
            process.stderr.close()

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux publication interposer")
    def test_every_publication_selection_recovers_after_process_death(self):
        dynamic = subprocess.run(["ldd", str(BINARY)], capture_output=True, text=True)
        if "libc.so" not in dynamic.stdout:
            self.skipTest("fault interposition requires the native dynamic fixture build")
        library = self.root / "optimization-stop.so"
        subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-shared", "-fPIC", "-O2",
                        str(Path(__file__).with_name("optimization_stop.c")), "-ldl",
                        "-o", str(library)], check=True)
        source = self.seed()
        self.run_command("convert")
        native = source.parent / "journal.bin"
        records = storage_records(native)
        saved = self.root / "original"
        shutil.copytree(source.parent, saved)
        original = native.read_bytes()
        result = self.run_command(expected=1, env={**os.environ, "LD_PRELOAD": str(library),
            "SNAJPAGENT_TEST_OPTIMIZATION_RENAME_FAIL": "1"})
        self.assertIn("failed:", result.stderr)
        self.assertEqual(native.read_bytes(), original)
        self.assertTrue((source.parent / ".optimize").exists())
        self.run_command()
        self.assertTrue(storage_records(native).startswith(records))
        for point in range(1, 7):
            with self.subTest(point=point):
                shutil.rmtree(source.parent)
                shutil.copytree(saved, source.parent)
                self.run_command(expected=86, env={**os.environ, "LD_PRELOAD": str(library),
                    "SNAJPAGENT_TEST_OPTIMIZATION_STOP": str(point)})
                self.assertTrue((source.parent / ".optimize").exists())
                # An immutable reader may return a verified old/new view or
                # refuse unavailable caches. It must not fabricate mixed data.
                snapshot = subprocess.run([str(READER), "--read-history", str(self.dotdir),
                                           native.parent.name], capture_output=True, timeout=30)
                if snapshot.returncode == 0:
                    rows = [json.loads(line) for line in snapshot.stdout.splitlines()]
                    self.assertEqual(sum(e["type"] == "rule_log" for e in rows), 20)
                self.run_command()
                self.assertTrue(storage_records(native).startswith(records))
                self.assertFalse((source.parent / ".optimize").exists())
                self.assertEqual(sum(e["type"] == "rule_log" for e in read_events(native)), 20)


if __name__ == "__main__":
    unittest.main()
