# SPDX-License-Identifier: GPL-2.0-only
"""Disposable PTY checks for the native receive-only upload boundary.

Set TRZSZ_CLIENT to a pinned, locally built trzsz wrapper for the real-client
case. The synthetic sender remains part of the ordinary fixture suite.
"""

import atexit
import base64
import fcntl
import hashlib
import json
import os
import pty
import re
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
PRODUCT = Path(__file__).resolve().parent / "snajpagent-fixture"
CLIENT = os.environ.get("TRZSZ_CLIENT", "")


def encoded(data):
    return base64.b64encode(zlib.compress(data))


def frame(kind, data, numeric=False):
    payload = str(data).encode("ascii") if numeric else encoded(data)
    return b"#" + kind.encode("ascii") + b":" + payload + b"\n"


class FixtureChildren:
    """Pin only verified descendants of a fixture-owned process on Linux."""

    def __init__(self, pid, parent=None):
        self.handles = {}
        if hasattr(os, "pidfd_open") and hasattr(signal, "pidfd_send_signal"):
            self.pin(pid, os.getpid() if parent is None else parent)
        atexit.register(self.close)
        self.remember()

    def pin(self, pid, parent, parent_fd=None):
        if pid in self.handles:
            return
        try:
            fd = os.pidfd_open(pid)
        except ProcessLookupError:
            return
        try:
            status = Path(f"/proc/{pid}/status").read_text()
            parents = [fd] if parent_fd is None else [fd, parent_fd]
            if re.search(rf"^PPid:\s+{parent}$", status, re.M) and not select.select(parents, [], [], 0)[0]:
                self.handles[pid] = fd
                fd = None
        except FileNotFoundError:
            pass
        finally:
            if fd is not None:
                os.close(fd)

    def remember(self, stop=False):
        pending = list(self.handles)
        seen = set()
        while pending:
            pid = pending.pop()
            if pid in seen:
                continue
            seen.add(pid)
            fd = self.handles[pid]
            if select.select([fd], [], [], 0)[0]:
                continue
            try:
                if stop:
                    signal.pidfd_send_signal(fd, signal.SIGSTOP)
                for task in Path(f"/proc/{pid}/task").iterdir():
                    for child in (task / "children").read_text().split():
                        self.pin(int(child), pid, fd)
                        if int(child) in self.handles:
                            pending.append(int(child))
            except (FileNotFoundError, ProcessLookupError):
                pass

    def close(self):
        atexit.unregister(self.close)
        try:
            # Freeze known parents before discovering their last descendants.
            # Detached owners remain pinned even after their frontend is reaped.
            self.remember(stop=True)
        finally:
            try:
                for fd in self.handles.values():
                    try:
                        signal.pidfd_send_signal(fd, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                pending = list(self.handles.values())
                deadline = time.monotonic() + 3
                while pending:
                    left = deadline - time.monotonic()
                    if left <= 0:
                        raise AssertionError("pinned fixture processes did not exit")
                    ready = select.select(pending, [], [], left)[0]
                    pending = [fd for fd in pending if fd not in ready]
            finally:
                for fd in self.handles.values():
                    os.close(fd)
                self.handles.clear()


class Session:
    def __init__(self, root, wrapper=None, *, command=None,
                 ready=b"ADAPTER_READY\r\n", cwd=None, env=None):
        self.stage = root / "stage"
        self.stage.mkdir(mode=0o700)
        self.receipt = root / "receipt"
        self.master, slave = pty.openpty()
        self.slave_name = os.ttyname(slave)
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))

        def controlling_tty():
            os.setsid()
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)

        if command is None:
            command = [str(ADAPTER), "--receive", str(self.stage), str(self.receipt)]
        if wrapper:
            command = [wrapper, "--dragfile", *command]
        env = dict(os.environ, TERM="xterm-256color") if env is None else dict(env)
        # A disposable PTY is not the operator's inherited screen/tmux window.
        # A screen launched below sets its own STY for the product child.
        env.pop("STY", None)
        env.pop("TMUX", None)
        env.pop("TMUX_PANE", None)
        env["TERM"] = "xterm-256color"
        self.process = subprocess.Popen(command, stdin=slave, stdout=slave, stderr=slave,
                                        preexec_fn=controlling_tty, cwd=cwd, env=env)
        self.children = FixtureChildren(self.process.pid)
        os.close(slave)
        self.pending = b""
        try:
            self.read_until(ready, 6)
        except BaseException:
            self.close()
            raise

    def close(self):
        self.children.close()
        if self.master is not None:
            os.close(self.master)
            self.master = None
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
            self.children.remember()
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
        self.children.remember()
        at = self.pending.index(marker) + len(marker)
        found, self.pending = self.pending[:at], self.pending[at:]
        return found

    def read_frame(self, expected):
        prefix = b"#" + expected.encode("ascii") + b":"
        seen = self.read_until(b"\n")
        seen = re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]", b"", seen).lstrip(b"\r")
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


class ProductSession(Session):
    def __init__(self, root, wrapper=None, extra_env=None):
        self.dotdir = root / "dotdir"
        home = root / "home"
        for path in (self.dotdir, home):
            path.mkdir(mode=0o700)
            path.chmod(0o700)
        env = dict(os.environ)
        env.update(HOME=str(home), TERM="xterm-256color", PAGER="",
                   SNAJPAGENT_DOTDIR=str(self.dotdir))
        if extra_env:
            env.update(extra_env)
        super().__init__(root, wrapper, command=[str(PRODUCT), "--dotdir", str(self.dotdir)],
                         ready="› ".encode(), cwd=home, env=env)

    def session_dir(self):
        sessions = list((self.dotdir / "sessions").glob("*/events.jsonl"))
        if len(sessions) != 1:
            raise AssertionError(f"wanted one saved session, got {sessions}")
        return sessions[0].parent

    def media_bytes(self):
        media = self.session_dir() / "media"
        return sorted(p.read_bytes() for p in media.iterdir()) if media.exists() else []

    def request_count(self):
        with (self.session_dir() / "events.jsonl").open(encoding="utf-8") as saved:
            return sum(json.loads(line).get("type") == "response_started" for line in saved)

    def tty_settings(self):
        descriptor = os.open(self.slave_name, os.O_RDWR | os.O_NOCTTY)
        try:
            return termios.tcgetattr(descriptor)
        finally:
            os.close(descriptor)


class UploadClientTests(unittest.TestCase):
    def start_synthetic(self, session):
        session.write(b"\x03trz\r")
        session.read_until(b"::TRZSZ:TRANSFER:R:1.0.0:")
        marker = session.read_until(b"\r\n")
        self.assertRegex(marker, rb"^[0-9]{11}00:0\r\n$")
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


class UploadProductTests(unittest.TestCase):
    @unittest.skipUnless(hasattr(os, "pidfd_open"), "native attachment needs Linux")
    def test_native_upload_disconnect_cancels_before_replacement_input(self):
        for partial in (False, True):
            with self.subTest(partial=partial):
                self.check_native_upload_disconnect(partial)

    def check_native_upload_disconnect(self, partial):
        with tempfile.TemporaryDirectory(prefix="snag-upload-rebind-") as tmp:
            root = Path(tmp)
            original_root, replacement_root = root / "original", root / "replacement"
            original_root.mkdir(mode=0o700)
            replacement_root.mkdir(mode=0o700)
            original = ProductSession(original_root)
            replacement = None
            try:
                retained = original_root / "retained.txt"
                retained.write_bytes(b"keep the already accepted attachment\n")
                original.write(f"/attach {retained}\r".encode())
                original.read_until(b"1 unsent attachment(s)")
                sid = original.session_dir().name
                if partial:
                    self.start_synthetic(original)
                    original.write(frame("NUM", 1, numeric=True))
                    self.assertEqual(original.read_frame("SUCC"), b"1")
                    original.write(frame("NAME", b"partial.bin"))
                    self.assertTrue(original.read_frame("SUCC"))
                    original.write(frame("SIZE", 6, numeric=True))
                    self.assertEqual(original.read_frame("SUCC"), b"6")
                    original.write(frame("DATA", b"abc"))
                    self.assertEqual(original.read_frame("SUCC"), b"3")
                    original.write(b"#DATA:")
                else:
                    original.write(b"/receive\r")
                    original.read_until(b"::TRZSZ:TRANSFER:R:1.0.0:")
                    original.write(b"#ACT:")
                # Incomplete old protocol input must not become a draft.
                time.sleep(0.05)
                original.process.terminate()
                original.process.wait(timeout=3)
                env = dict(os.environ, HOME=str(original_root / "home"))
                replacement = Session(replacement_root,
                    command=[str(PRODUCT), "--dotdir", str(original.dotdir), "-A", sid],
                    ready=b"Attached session", cwd=original_root / "home", env=env)
                replacement.read_until("›".encode())
                replacement.write(b"ping\r")
                replacement.read_until(b"pong")
                replacement.read_until("›".encode())
                replacement.write(b"/exit\r")
                replacement.read_until(b"--resume")
                self.assertEqual(replacement.process.wait(timeout=3), 0)
                self.assertEqual(original.request_count(), 1)
                self.assertFalse(list(original.session_dir().glob("upload-*")))
                self.assertEqual(original.media_bytes(), [retained.read_bytes()])
            finally:
                if replacement is not None:
                    replacement.close()
                original.close()

    @unittest.skipUnless(hasattr(os, "pidfd_open"), "native owner cleanup needs Linux pidfds")
    def test_native_fixture_cleanup_after_frontend_loss(self):
        with tempfile.TemporaryDirectory(prefix="snag-owner-cleanup-") as tmp:
            session = ProductSession(Path(tmp))
            sibling = subprocess.Popen(["/bin/sleep", "30"])
            pinned = []
            try:
                rejected = FixtureChildren(sibling.pid, parent=os.getpid() + 1)
                self.assertFalse(rejected.handles)
                rejected.close()
                session.children.remember()
                pinned = [os.dup(fd) for pid, fd in session.children.handles.items()
                          if pid != session.process.pid]
                self.assertTrue(pinned, "native engine was not pinned")
                session.process.terminate()
                session.process.wait(timeout=3)
                self.assertLess(len(select.select(pinned, [], [], 0)[0]), len(pinned))
                session.close()
                self.assertEqual(set(select.select(pinned, [], [], 0)[0]), set(pinned))
                self.assertIsNone(sibling.poll(), "cleanup touched a sibling fixture")
            finally:
                session.close()
                for fd in pinned:
                    os.close(fd)
                sibling.terminate()
                sibling.wait(timeout=3)

    def start_synthetic(self, session, launch=b"/receive\r"):
        session.write(launch)
        session.read_until(b"::TRZSZ:TRANSFER:R:1.0.0:", 7)
        marker = session.read_until(b"\r\n")
        self.assertRegex(marker, rb"^[0-9]{11}00:0\r\n$")
        action = json.dumps({"confirm": True, "protocol": 4, "newline": "\n"}).encode()
        session.write(frame("ACT", action))
        config = json.loads(zlib.decompress(base64.b64decode(session.read_frame("CFG"))))
        self.assertEqual((config["protocol"], config["binary"]), (1, False))

    def test_attach_generic_binary_uses_same_retained_file_path(self):
        with tempfile.TemporaryDirectory(prefix="snag-upload-product-") as path:
            root = Path(path)
            source = root / "example.bin"
            source.write_bytes(bytes(range(256)) * 16)
            session = ProductSession(root)
            try:
                session.write(f"/attach {source}\r".encode())
                session.read_until(b"1 unsent attachment(s)", 8)
                self.assertEqual(session.media_bytes(), [source.read_bytes()])
                self.assertEqual(session.request_count(), 0)
                session.write(b"/exit\r")
                self.assertEqual(session.process.wait(timeout=4), 0)
            finally:
                session.close()

    @unittest.skipUnless(CLIENT and Path(CLIENT).is_file(), "TRZSZ_CLIENT not supplied")
    def test_pinned_client_product_multifile_retains_without_sending(self):
        with tempfile.TemporaryDirectory(prefix="snag-upload-product-go-") as path:
            root = Path(path)
            binary = root / "all-bytes.bin"
            text = root / "script.txt"
            binary.write_bytes(bytes(range(256)) * 16)
            text.write_bytes(b"a retained file, not a prompt\n")
            session = ProductSession(root, CLIENT)
            try:
                paths = " ".join(shlex.quote(str(p)) for p in (binary, text))
                session.write(paths.encode())
                session.read_until(b"2 unsent attachment(s)", 20)
                self.assertEqual(session.media_bytes(), sorted([binary.read_bytes(), text.read_bytes()]))
                self.assertEqual(session.request_count(), 0)
                session.write(b"ping\r")
                session.read_until(b"pong", 10)
                self.assertGreater(session.request_count(), 0)
                session.write(b"/exit\r")
                self.assertEqual(session.process.wait(timeout=4), 0)
            finally:
                session.close()

    @unittest.skipUnless(CLIENT and Path(CLIENT).is_file(), "TRZSZ_CLIENT not supplied")
    def test_pinned_client_product_bad_image_rolls_back(self):
        with tempfile.TemporaryDirectory(prefix="snag-upload-product-invalid-image-") as path:
            root = Path(path)
            first = root / "valid.bin"
            first.write_bytes(bytes(range(256)) * 16)
            damaged = root / "broken.png"
            damaged.write_bytes(b"\x89PNG\r\n\x1a\n" + b"not a PNG image" * 8)
            session = ProductSession(root, CLIENT)
            try:
                paths = " ".join(shlex.quote(str(p)) for p in (first, damaged))
                session.write(paths.encode())
                session.read_until(b"Linked media decoding failed", 20)
                self.assertEqual(session.media_bytes(), [])
                self.assertEqual(session.request_count(), 0)
                session.write(b"ping\r")
                session.read_until(b"pong", 10)
                session.write(b"/exit\r")
                self.assertEqual(session.process.wait(timeout=4), 0)
            finally:
                session.close()

    def test_explicit_upload_bad_digest_preserves_existing_pending(self):
        with tempfile.TemporaryDirectory(prefix="snag-upload-product-failure-") as path:
            root = Path(path)
            original = root / "existing.txt"
            original.write_bytes(b"keep this existing attachment\n")
            session = ProductSession(root)
            try:
                session.write(f"/attach {original}\r".encode())
                session.read_until(b"1 unsent attachment(s)", 8)
                self.start_synthetic(session)
                session.write(frame("NUM", 1, numeric=True))
                self.assertEqual(session.read_frame("SUCC"), b"1")
                session.write(frame("NAME", b"bad.bin"))
                self.assertTrue(session.read_frame("SUCC"))
                session.write(frame("SIZE", 3, numeric=True))
                self.assertEqual(session.read_frame("SUCC"), b"3")
                session.write(frame("DATA", b"abc"))
                self.assertEqual(session.read_frame("SUCC"), b"3")
                session.write(frame("MD5", bytes(16)))
                self.assertIn(b"#FAIL:", session.read_until(b"\n"))
                session.read_until(b"file contents or digest", 8)
                session.write(b"/attachments\r")
                session.read_until(b"1 unsent attachment(s)", 8)
                self.assertEqual(session.media_bytes(), [original.read_bytes()])
                self.assertEqual(list(session.session_dir().glob("upload-*")), [])
                self.assertEqual(session.request_count(), 0)
                session.write(b"/exit\r")
                self.assertEqual(session.process.wait(timeout=4), 0)
            finally:
                session.close()
    def test_explicit_upload_success_replays_immediate_keyboard_tail(self):
        with tempfile.TemporaryDirectory(prefix="snag-upload-product-tail-") as path:
            session = ProductSession(Path(path))
            try:
                before = session.tty_settings()
                self.start_synthetic(session)
                self.assertEqual(session.request_count(), 0)
                session.write(frame("NUM", 1, numeric=True))
                self.assertEqual(session.read_frame("SUCC"), b"1")
                session.write(frame("NAME", b"synthetic.bin"))
                self.assertTrue(session.read_frame("SUCC"))
                session.write(frame("SIZE", 3, numeric=True))
                self.assertEqual(session.read_frame("SUCC"), b"3")
                session.write(frame("DATA", b"abc"))
                self.assertEqual(session.read_frame("SUCC"), b"3")
                session.write(frame("MD5", hashlib.md5(b"abc").digest()))
                self.assertTrue(session.read_frame("SUCC"))
                session.write(frame("EXIT", b"done") + b"ping\r")
                session.read_until(b"1 unsent attachment(s)", 8)
                session.read_until(b"pong", 8)
                self.assertEqual(session.media_bytes(), [b"abc"])
                self.assertEqual(session.request_count(), 1)
                self.assertEqual(session.tty_settings(), before)
                session.write(b"/exit\r")
                self.assertEqual(session.process.wait(timeout=4), 0)
            finally:
                session.close()

    def test_stock_directory_mode_explicitly_rejected_without_attachment(self):
        with tempfile.TemporaryDirectory(prefix="snag-upload-product-dir-") as path:
            session = ProductSession(Path(path))
            try:
                before = session.tty_settings()
                session.write(b"\x03trz -d\r")
                session.read_until(b"::TRZSZ:TRANSFER:D:1.0.0:", 7)
                session.read_until(b"\r\n")
                action = json.dumps({"confirm": True, "protocol": 4, "newline": "\n"}).encode()
                session.write(frame("ACT", action))
                self.assertIn(b"#FAIL:", session.read_until(b"\n"))
                session.read_until(b"handshake:", 8)
                self.assertEqual(session.media_bytes(), [])
                self.assertEqual(session.request_count(), 0)
                self.assertEqual(session.tty_settings(), before)
                session.write(b"ping\r")
                session.read_until(b"pong", 8)
                session.write(b"/exit\r")
                self.assertEqual(session.process.wait(timeout=4), 0)
            finally:
                session.close()


if __name__ == "__main__":
    unittest.main()
