# SPDX-License-Identifier: GPL-2.0-only
"""Disposable PTY checks for the native receive-only upload boundary.

Set TRZSZ_CLIENT to a pinned, locally built trzsz wrapper for the real-client
case. The synthetic sender remains part of the ordinary fixture suite.
"""

import base64
import fcntl
import json
import os
import pty
import select
import shlex
import signal
import struct
import subprocess
import tempfile
import termios
import time
import unittest
import zlib
from pathlib import Path


ADAPTER = Path(__file__).resolve().parent / "test_upload_wire"
CLIENT = os.environ.get("TRZSZ_CLIENT", "")


def encoded(data):
    return base64.b64encode(zlib.compress(data))


def frame(kind, data, numeric=False):
    payload = str(data).encode("ascii") if numeric else encoded(data)
    return b"#" + kind.encode("ascii") + b":" + payload + b"\n"


class Session:
    def __init__(self, root, wrapper=None):
        self.stage = root / "stage"
        self.stage.mkdir(mode=0o700)
        self.receipt = root / "receipt"
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))

        def controlling_tty():
            os.setsid()
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)

        command = [str(ADAPTER), "--receive", str(self.stage), str(self.receipt)]
        if wrapper:
            command = [wrapper, "--dragfile", *command]
        self.process = subprocess.Popen(command, stdin=slave, stdout=slave, stderr=slave,
                                        preexec_fn=controlling_tty)
        os.close(slave)
        self.pending = b""
        self.read_until(b"ADAPTER_READY\r\n", 6)

    def close(self):
        os.close(self.master)
        if self.process.poll() is None:
            try:
                os.killpg(self.process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=3)

    def read_until(self, marker, seconds=5):
        deadline = time.monotonic() + seconds
        while marker not in self.pending:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f"timeout waiting for {marker!r}; tail={self.pending[-180:]!r}")
            ready, _, _ = select.select([self.master], [], [], remaining)
            if not ready:
                continue
            chunk = os.read(self.master, 65536)
            if not chunk:
                raise AssertionError(f"PTY ended before {marker!r}")
            self.pending = (self.pending + chunk)[-131072:]
        at = self.pending.index(marker) + len(marker)
        found, self.pending = self.pending[:at], self.pending[at:]
        return found

    def read_frame(self, expected):
        prefix = b"#" + expected.encode("ascii") + b":"
        seen = self.read_until(b"\n")
        if not seen.startswith(prefix):
            raise AssertionError(f"wanted {expected}, got {seen[:120]!r}")
        return seen[len(prefix):-1]

    def write(self, data):
        os.write(self.master, data)

    def finished(self, seconds=6):
        deadline = time.monotonic() + seconds
        terminal_ended = False
        while time.monotonic() < deadline:
            if self.receipt.exists() and self.receipt.stat().st_size:
                return self.receipt.read_text()
            if terminal_ended:
                time.sleep(0.02)
                continue
            ready, _, _ = select.select([self.master], [], [], 0.1)
            if ready:
                try:
                    chunk = os.read(self.master, 65536)
                    if chunk:
                        self.pending = (self.pending + chunk)[-131072:]
                    else:
                        terminal_ended = True
                except OSError:
                    terminal_ended = True
        raise AssertionError(f"no completed receiver receipt; output tail={self.pending[-180:]!r}")


class UploadClientTests(unittest.TestCase):
    def start_synthetic(self, session):
        session.write(b"\x03trz\r")
        self.assertIn(b"::TRZSZ:TRANSFER:R:1.0.0:", session.read_until(b"\r\n"))
        action = json.dumps({"confirm": True, "protocol": 4, "newline": "\n"}).encode()
        session.write(frame("ACT", action))
        config = json.loads(zlib.decompress(base64.b64decode(session.read_frame("CFG"))))
        self.assertEqual((config["protocol"], config["binary"]), (1, False))

    def test_synthetic_bad_md5_rolls_back(self):
        with tempfile.TemporaryDirectory(prefix="snag-upload-bad-") as path:
            session = Session(Path(path))
            try:
                self.start_synthetic(session)
                session.write(frame("NUM", 1, numeric=True))
                self.assertEqual(session.read_frame("SUCC"), b"1")
                session.write(frame("NAME", b"sample.bin"))
                self.assertTrue(zlib.decompress(base64.b64decode(session.read_frame("SUCC"))))
                session.write(frame("SIZE", 3, numeric=True))
                self.assertEqual(session.read_frame("SUCC"), b"3")
                session.write(frame("DATA", b"abc"))
                self.assertEqual(session.read_frame("SUCC"), b"3")
                session.write(frame("MD5", bytes(16)))
                self.assertIn(b"#FAIL:", session.read_until(b"\n"))
                self.assertIn("status=-1", session.finished())
                self.assertEqual(list(session.stage.iterdir()), [])
            finally:
                session.close()

    def test_synthetic_count_and_name_rejected_without_files(self):
        for reason in ("count", "traversal", "empty"):
            with self.subTest(reason=reason), tempfile.TemporaryDirectory(prefix="snag-upload-no-") as path:
                session = Session(Path(path))
                try:
                    self.start_synthetic(session)
                    session.write(frame("NUM", 9 if reason == "count" else 1, numeric=True))
                    if reason != "count":
                        self.assertEqual(session.read_frame("SUCC"), b"1")
                        name = b"../escape" if reason == "traversal" else b"empty.bin"
                        session.write(frame("NAME", name))
                        if reason == "empty":
                            self.assertTrue(session.read_frame("SUCC"))
                            session.write(frame("SIZE", 0, numeric=True))
                    self.assertIn(b"#FAIL:", session.read_until(b"\n"))
                    self.assertIn("status=-1", session.finished())
                    self.assertEqual(list(session.stage.iterdir()), [])
                finally:
                    session.close()

    def test_synthetic_exit_preserves_immediate_input_tail(self):
        with tempfile.TemporaryDirectory(prefix="snag-upload-tail-") as path:
            session = Session(Path(path))
            try:
                self.start_synthetic(session)
                session.write(frame("NUM", 0, numeric=True))
                self.assertEqual(session.read_frame("SUCC"), b"0")
                session.write(frame("EXIT", b"done") + b"post\r")
                self.assertIn("status=0 count=0 tail=5", session.finished())
                self.assertEqual(list(session.stage.iterdir()), [])
            finally:
                session.close()

    @unittest.skipUnless(CLIENT and Path(CLIENT).is_file(), "TRZSZ_CLIENT not supplied")
    def test_pinned_client_multiple_binary_files(self):
        with tempfile.TemporaryDirectory(prefix="snag-upload-go-") as path:
            root = Path(path)
            first = root / "all-bytes.bin"
            second = root / "second-file.dat"
            first.write_bytes(bytes(range(256)) * 16)
            second.write_bytes(bytes((i * 131) % 256 for i in range(8191)))
            session = Session(root, CLIENT)
            try:
                session.write(" ".join(shlex.quote(str(p)) for p in (first, second)).encode())
                self.assertIn("status=0 count=2", session.finished(15))
                self.assertEqual(sorted(p.read_bytes() for p in session.stage.iterdir()),
                                 sorted((first.read_bytes(), second.read_bytes())))
                self.assertEqual(session.process.wait(timeout=4), 0)
            finally:
                session.close()


if __name__ == "__main__":
    unittest.main()
