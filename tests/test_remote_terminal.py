# SPDX-License-Identifier: GPL-2.0-only
"""Native client-only remote-mode startup and PTY ownership regressions."""
import json
import fcntl
import hashlib
import struct
import os
import pty
import select
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import termios
import textwrap
import time
import unittest
from pathlib import Path

from test_upload_client import FixtureChildren, ProductSession

PRODUCT = Path(__file__).resolve().parent / "snajpagent-fixture"
SCREEN_FIXTURE = Path(__file__).resolve().parent / "fixture_screen_state.py"


def screen_snapshot(env, name, path):
    path.unlink(missing_ok=True)
    subprocess.run(["screen", "-S", name, "-X", "hardcopy", "-h", str(path)],
                   env=env, check=True, timeout=5)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if path.is_file() and path.stat().st_size:
            return path.read_bytes()
        time.sleep(0.05)
    raise AssertionError("screen did not write its asynchronous hardcopy")


class RemoteProcess:
    def __init__(self, home, arguments, wrapped=True, cwd=None, extra_env=None, *,
                 server_children=None, winsize=None):
        self.master, self.slave = pty.openpty()
        if winsize:
            fcntl.ioctl(self.slave, termios.TIOCSWINSZ,
                        struct.pack("HHHH", winsize[0], winsize[1], 0, 0))
        self.original = termios.tcgetattr(self.slave)
        env = dict(os.environ, HOME=str(home), TERM="xterm-256color", SHELL="/bin/sh", PAGER="")
        if extra_env:
            env.update(extra_env)
            env = {key: value for key, value in env.items() if value is not None}
        for key in ("STY", "TMUX", "TMUX_PANE", "OPENAI_API_KEY"):
            env.pop(key, None)
        command = arguments if wrapped is None else [str(PRODUCT), *(["remote"] if wrapped else []), *arguments]
        def controlling_terminal():
            os.setsid()
            fcntl.ioctl(self.slave, termios.TIOCSCTTY, 0)
        self.process = subprocess.Popen(command, stdin=self.slave, stdout=self.slave,
                                        stderr=self.slave, cwd=cwd or home, env=env,
                                        preexec_fn=controlling_terminal)
        self.children = FixtureChildren(self.process.pid)
        self.server_children = server_children
        self.output = bytearray()

    def remember_children(self):
        self.children.remember()
        if self.server_children is not None:
            # Loopback SSH's remote side belongs to its private server, not this client.
            self.server_children.remember()

    def until(self, marker, timeout=5):
        deadline = time.monotonic() + timeout
        while marker not in self.output and time.monotonic() < deadline:
            self.remember_children()
            if select.select([self.master], [], [], 0.1)[0]:
                self.output.extend(os.read(self.master, 65536))
        self.remember_children()
        if marker not in self.output:
            raise AssertionError(f"missing {marker!r}: {bytes(self.output)!r}")
        return bytes(self.output)

    def until_after(self, anchor, marker, timeout=5):
        self.until(anchor, timeout)
        following = self.output.rfind(anchor) + len(anchor)
        if marker not in self.output[following:]:
            self.output = self.output[following:]
            return self.until(marker, timeout)
        return bytes(self.output)

    def wait(self, expected):
        self.remember_children()
        if sys.platform == "darwin" and self.slave is not None:
            # A parent-held slave delays final exit on macOS; after exit it
            # ceases to be a tty, so inspect modes while the client is live.
            os.close(self.slave)
            self.slave = None
        assert self.process.wait(timeout=5) == expected, bytes(self.output)
        if self.slave is not None:
            assert termios.tcgetattr(self.slave) == self.original

    def close(self):
        self.remember_children()
        self.children.close()
        if sys.platform == "darwin" and self.slave is not None:
            # A parent-side slave can keep a macOS child in E (exiting).
            os.close(self.slave)
            self.slave = None
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        if self.master is not None:
            os.close(self.master)
            self.master = None
        if self.slave is not None:
            os.close(self.slave)
            self.slave = None


class RemoteStartupTests(unittest.TestCase):
    @staticmethod
    def wait_exited(child):
        if sys.platform == "darwin" and child.slave is not None:
            os.close(child.slave)
            child.slave = None
        return child.process.wait(timeout=8)

    @staticmethod
    def wait_file(path, timeout=8):
        deadline = time.monotonic() + timeout
        while not path.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        if not path.exists():
            raise AssertionError(f"fixture did not write {path}")

    def test_exited_child_does_not_wait_for_inherited_terminal_slave(self):
        with tempfile.TemporaryDirectory(prefix="snag-remote-subshell-") as tmp:
            root = Path(tmp)
            peer = textwrap.dedent('''
                import subprocess
                import sys

                subprocess.Popen([sys.executable, "-c", "import time; time.sleep(3)"],
                                 stdin=0, stdout=1, stderr=2, start_new_session=True)
                print("FRONTEND_EXITED", flush=True)
                sys.exit(7)
            ''')
            child = RemoteProcess(root, [sys.executable, "-u", "-c", peer])
            try:
                child.until(b"FRONTEND_EXITED", 8)
                self.assertEqual(child.process.wait(timeout=1.5), 7)
            finally:
                child.close()

    def test_upload_picker_exits_when_transport_exits(self):
        with tempfile.TemporaryDirectory(prefix="snag-picker-exit-") as tmp:
            root = Path(tmp)
            release = root / "release"
            peer = textwrap.dedent(r'''
                import sys, time
                from pathlib import Path

                print("::TRZSZ:TRANSFER:R:1.0.0:0000000000000:0\r", flush=True)
                deadline = time.monotonic() + 10
                while not Path(sys.argv[1]).exists():
                    assert time.monotonic() < deadline
                    time.sleep(.02)
                sys.exit(7)
            ''')
            child = RemoteProcess(root, [sys.executable, "-u", "-c", peer, str(release)])
            try:
                child.until(b"Select local file")
                release.touch()
                child.wait(7)
            finally:
                child.close()

    def test_screen_state_download_retries_without_protocol_scrollback(self):
        with tempfile.TemporaryDirectory(prefix="snag-screen-download-") as tmp:
            root = Path(tmp).resolve()
            home = root / "client"
            home.mkdir()
            source = root / "all-bytes.bin"
            source.write_bytes(bytes(range(256)) * 4)
            child = RemoteProcess(home, [sys.executable, "-u", str(SCREEN_FIXTURE),
                                   str(PRODUCT), "--dotdir", str(root / "agent")],
                                  extra_env={"SCREEN_FIXTURE_DROP_FIRST": "1",
                                             "SCREEN_FIXTURE_DUPLICATE": "1"})
            try:
                child.until("›".encode(), 10)
                os.write(child.master, f"/send {source}\r".encode())
                target = home / "Downloads" / source.name
                output = child.until(str(target).encode(), 30)
                self.assertEqual(target.read_bytes(), source.read_bytes())
                for forbidden in (b"SNAJPAGENT-SCREEN/1", b"#DATA:", b"#CFG:",
                                  b"::TRZSZ:TRANSFER:"):
                    self.assertNotIn(forbidden, output)
                os.write(child.master, b"ping\r")
                child.until(b"pong", 8)
                os.write(child.master, b"/exit\r")
                child.until(b"--resume", 8)
                self.assertEqual(self.wait_exited(child), 0)
            finally:
                child.close()

    def test_screen_state_replayed_hello_does_not_restart_transfer(self):
        with tempfile.TemporaryDirectory(prefix="snag-screen-hello-replay-") as tmp:
            root = Path(tmp).resolve()
            home = root / "client"
            home.mkdir()
            source = root / "hello-replay.bin"
            source.write_bytes(bytes(range(256)) * 2)
            replayed = root / "replayed"
            child = RemoteProcess(home, [sys.executable, "-u", str(SCREEN_FIXTURE),
                                   str(PRODUCT), "--dotdir", str(root / "agent")],
                                  extra_env={"SCREEN_FIXTURE_REPLAY_HELLO": "1",
                                             "SCREEN_FIXTURE_HELLO_REPLAYED": str(replayed)})
            try:
                child.until("›".encode(), 10)
                mode = termios.tcgetattr(child.slave)
                os.write(child.master, f"/send {source}\r".encode())
                target = home / "Downloads" / source.name
                child.until(str(target).encode(), 15)
                output = child.until_after(str(target).encode(), "›".encode(), 8)
                self.assertEqual(target.read_bytes(), source.read_bytes())
                self.assertEqual(replayed.read_bytes(), b"active\nended\n")
                self.assertNotIn(b"SNAJPAGENT-SCREEN/1", output)
                self.assertNotIn(b"#DATA:", output)
                self.assertEqual(termios.tcgetattr(child.slave), mode)
                os.write(child.master, b"ping\r")
                output = child.until(b"pong", 8)
                self.assertNotIn(b"#SNAJPAGENT-SCREEN:READY:", output)
                self.assertNotIn(b"#ACT:", output)
                os.write(child.master, b"/exit\r")
                child.until(b"--resume", 8)
                self.assertEqual(self.wait_exited(child), 0)
            finally:
                child.close()

    def test_screen_state_upload_keeps_bytes_and_restores_prompt(self):
        with tempfile.TemporaryDirectory(prefix="snag-screen-upload-") as tmp:
            root = Path(tmp).resolve()
            home = root / "client"
            home.mkdir()
            source = root / "source file.bin"
            source.write_bytes(bytes(range(256)) * 3)
            dotdir = root / "agent"
            replayed = root / "replayed"
            child = RemoteProcess(home, [sys.executable, "-u", str(SCREEN_FIXTURE),
                                   str(PRODUCT), "--dotdir", str(dotdir)],
                                  extra_env={"SCREEN_FIXTURE_DROP_FIRST": "1",
                                             "SCREEN_FIXTURE_DUPLICATE": "1",
                                             "SCREEN_FIXTURE_REPLAY": "1",
                                             "SCREEN_FIXTURE_REPLAYED": str(replayed)})
            try:
                child.until("›".encode(), 10)
                os.write(child.master, b"/receive\r")
                child.until(b"Select local file", 10)
                os.write(child.master, f"{source}\r".encode())
                output = child.until(b"1 unsent attachment(s)", 30)
                media = next((dotdir / "sessions").glob("*/media"))
                self.assertEqual([p.read_bytes() for p in media.iterdir()],
                                 [source.read_bytes()])
                self.assertEqual(replayed.read_bytes(), b"1")
                for forbidden in (b"SNAJPAGENT-SCREEN/1", b"#DATA:", b"#CFG:",
                                  b"::TRZSZ:TRANSFER:"):
                    self.assertNotIn(forbidden, output)
                os.write(child.master, b"ping\r")
                child.until(b"pong", 8)
                os.write(child.master, b"/exit\r")
                child.until(b"--resume", 8)
                self.assertEqual(self.wait_exited(child), 0)
            finally:
                child.close()

    def test_screen_state_model_send_file_commits_receipt(self):
        with tempfile.TemporaryDirectory(prefix="snag-screen-model-send-") as tmp:
            root = Path(tmp).resolve()
            home = root / "client"
            home.mkdir()
            source = home / "tool.bin"
            source.write_bytes(bytes(range(128)))
            dotdir = root / "agent"
            child = RemoteProcess(home, [sys.executable, "-u", str(SCREEN_FIXTURE),
                                   str(PRODUCT), "--dotdir", str(dotdir)])
            try:
                child.until("›".encode(), 10)
                os.write(child.master, b"download_tool ./tool.bin\r")
                child.until(b"Client acknowledged the file digest and final EXIT.", 30)
                output = child.until(b" complete\r\n", 10)
                target = home / "Downloads" / source.name
                self.assertEqual(target.read_bytes(), source.read_bytes())
                self.assertNotIn(b"SNAJPAGENT-SCREEN/1", output)
                self.assertNotIn(b"#DATA:", output)
                log = next((dotdir / "sessions").glob("*/events.jsonl"))
                events = [json.loads(line) for line in log.read_text().splitlines()]
                self.assertEqual(sum(e["type"] == "download_queued" for e in events), 1)
                self.assertEqual(sum(e["type"] == "download_removed" for e in events), 1)
                os.write(child.master, b"/exit\r")
                child.until(b"--resume", 8)
                self.assertEqual(self.wait_exited(child), 0)
            finally:
                child.close()

    def test_screen_state_cancel_discards_partial_download_and_restores_prompt(self):
        with tempfile.TemporaryDirectory(prefix="snag-screen-cancel-") as tmp:
            root = Path(tmp).resolve()
            home = root / "client"
            home.mkdir()
            source = root / "cancel.bin"
            source.write_bytes(os.urandom(8192))
            dropped = root / "dropped"
            child = RemoteProcess(home, [sys.executable, "-u", str(SCREEN_FIXTURE),
                                   str(PRODUCT), "--dotdir", str(root / "agent")],
                                  extra_env={"SCREEN_FIXTURE_DROP_ALL": "1",
                                             "SCREEN_FIXTURE_DROPPED": str(dropped)})
            try:
                child.until("›".encode(), 10)
                mode = termios.tcgetattr(child.slave)
                os.write(child.master, f"/send {source}\r".encode())
                self.wait_file(dropped)
                os.write(child.master, b"\x03")
                output = child.until(b"Download cancelled", 12)
                self.assertFalse((home / "Downloads" / source.name).exists())
                self.assertNotIn(b"SNAJPAGENT-SCREEN/1", output)
                self.assertNotIn(b"#DATA:", output)
                self.assertEqual(termios.tcgetattr(child.slave), mode)
                os.write(child.master, b"ping\r")
                child.until(b"pong", 8)
                os.write(child.master, b"/exit\r")
                child.until(b"--resume", 8)
                self.assertEqual(self.wait_exited(child), 0)
            finally:
                child.close()

    def test_screen_state_timeout_recovers_without_partial_file(self):
        with tempfile.TemporaryDirectory(prefix="snag-screen-timeout-") as tmp:
            root = Path(tmp).resolve()
            home = root / "client"
            home.mkdir()
            source = root / "missing-frame.bin"
            source.write_bytes(bytes(range(64)))
            dropped = root / "dropped"
            child = RemoteProcess(home, [sys.executable, "-u", str(SCREEN_FIXTURE),
                                   str(PRODUCT), "--dotdir", str(root / "agent")],
                                  extra_env={"SCREEN_FIXTURE_DROP_ALL": "1",
                                             "SCREEN_FIXTURE_DROPPED": str(dropped)})
            try:
                child.until("›".encode(), 10)
                mode = termios.tcgetattr(child.slave)
                os.write(child.master, f"/send {source}\r".encode())
                self.wait_file(dropped)
                output = child.until(b"Download cancelled", 27)
                self.assertFalse((home / "Downloads" / source.name).exists())
                self.assertNotIn(b"SNAJPAGENT-SCREEN/1", output)
                self.assertEqual(termios.tcgetattr(child.slave), mode)
                os.write(child.master, b"ping\r")
                child.until(b"pong", 8)
                os.write(child.master, b"/exit\r")
                child.until(b"--resume", 8)
                self.assertEqual(self.wait_exited(child), 0)
            finally:
                child.close()

    def test_screen_state_drag_keeps_editable_draft(self):
        with tempfile.TemporaryDirectory(prefix="snag-screen-draft-") as tmp:
            root = Path(tmp).resolve()
            home = root / "client"
            home.mkdir()
            source = home / "draft-upload.bin"
            source.write_bytes(bytes(range(128)) * 3)
            dotdir = root / "agent"
            child = RemoteProcess(home, [sys.executable, "-u", str(SCREEN_FIXTURE),
                                   str(PRODUCT), "--dotdir", str(dotdir)])
            try:
                child.until("›".encode(), 10)
                mode = termios.tcgetattr(child.slave)
                os.write(child.master, b"ping")
                child.until(b"ping", 8)
                os.write(child.master,
                         b"\x1b[200~" + str(source).encode() + b"\x1b[201~")
                output = child.until(b"1 unsent attachment(s)", 20)
                media = next((dotdir / "sessions").glob("*/media"))
                self.assertEqual([p.read_bytes() for p in media.iterdir()],
                                 [source.read_bytes()])
                self.assertNotIn(b"SNAJPAGENT-SCREEN/1", output)
                self.assertEqual(termios.tcgetattr(child.slave), mode)
                os.write(child.master, b"\r")
                child.until(b"pong", 8)
                os.write(child.master, b"/exit\r")
                child.until(b"--resume", 8)
                self.assertEqual(self.wait_exited(child), 0)
            finally:
                child.close()

    def test_screen_state_resize_during_download_restores_ui(self):
        with tempfile.TemporaryDirectory(prefix="snag-screen-resize-") as tmp:
            root = Path(tmp).resolve()
            home = root / "client"
            home.mkdir()
            source = root / "resized.bin"
            source.write_bytes(bytes(range(256)) * 16)
            dropped = root / "dropped"
            winsize = root / "winsize"
            child = RemoteProcess(home, [sys.executable, "-u", str(SCREEN_FIXTURE),
                                   str(PRODUCT), "--dotdir", str(root / "agent")],
                                  extra_env={"SCREEN_FIXTURE_DROP_FIRST": "1",
                                             "SCREEN_FIXTURE_DROPPED": str(dropped),
                                             "SCREEN_FIXTURE_WINSIZE": str(winsize)})
            try:
                child.until("›".encode(), 10)
                mode = termios.tcgetattr(child.slave)
                os.write(child.master, f"/send {source}\r".encode())
                self.wait_file(dropped)
                fcntl.ioctl(child.slave, termios.TIOCSWINSZ,
                            struct.pack("HHHH", 12, 18, 0, 0))
                child.process.send_signal(signal.SIGWINCH)
                target = home / "Downloads" / source.name
                output = child.until(str(target).encode(), 30)
                self.assertEqual(target.read_bytes(), source.read_bytes())
                self.wait_file(winsize)
                self.assertEqual(struct.unpack("HHHH", winsize.read_bytes())[:2], (12, 18))
                self.assertEqual(termios.tcgetattr(child.slave), mode)
                self.assertNotIn(b"SNAJPAGENT-SCREEN/1", output)
                os.write(child.master, b"ping\r")
                child.until(b"pong", 8)
                os.write(child.master, b"/exit\r")
                child.until(b"--resume", 8)
                self.assertEqual(self.wait_exited(child), 0)
            finally:
                child.close()

    @unittest.skipUnless(shutil.which("mosh"), "stock Mosh launcher unavailable")
    def test_mosh_remote_command_options(self):
        with tempfile.TemporaryDirectory(prefix="snag-mosh-options-") as tmp:
            home = Path(tmp).resolve()
            capture = home / "ssh.json"
            ssh = home / "ssh"
            ssh.write_text(f"#!{sys.executable}\n" + textwrap.dedent('''
                import json, os, sys
                from pathlib import Path
                Path(os.environ["MOSH_TEST_CAPTURE"]).write_text(json.dumps(sys.argv[1:]))
                print("SSH_ARGUMENTS_RECORDED", flush=True)
                sys.exit(17)
            '''))
            ssh.chmod(0o755)
            client = home / "color counter"
            client.write_text(f"#!{sys.executable}\nprint(256)\n")
            client.chmod(0o755)
            command = ["/usr/local/bin/snajpagent", "--resume",
                       "0123456789abcdef0123456789abcdef", "--help", "--",
                       "a b", "$(touch forbidden)", "résumé", ""]
            cases = [
                ("mosh", []),
                (shutil.which("mosh"), ["--no-init", "--"]),
                (shutil.which("mosh"), ["--client", str(client), "--server", "mosh-server",
                    "--predict", "never", "--family", "inet", "--port", "60001",
                    "--ssh", shlex.quote(str(ssh)), "--bind-server", "any",
                    "--experimental-remote-ip", "proxy"]),
                ("mosh", ["--predict=never", "-p", "60002", "--ssh=" + shlex.quote(str(ssh))]),
                ("mosh", ["--serv", "mosh-server", "--"]),
            ]
            env = {"PATH": str(home) + os.pathsep + os.environ["PATH"],
                   "MOSH_TEST_CAPTURE": str(capture), "POSIXLY_CORRECT": None}
            for executable, options in cases:
                with self.subTest(options=options):
                    capture.unlink(missing_ok=True)
                    child = RemoteProcess(home, [executable, *options, "snajpadev", *command],
                                          extra_env=env)
                    try:
                        child.until(b"SSH_ARGUMENTS_RECORDED", 8)
                        self.assertNotEqual(self.wait_exited(child), 0)
                        args = json.loads(capture.read_text())
                        self.assertEqual(args[-3:-1], ["snajpadev", "--"])
                        remote = shlex.split(args[-1])
                        self.assertEqual(remote[remote.index("--") + 1:], command)
                        self.assertFalse((home / "forbidden").exists())
                        self.assertFalse((home / ".snajpagent").exists())
                    finally:
                        child.close()

    @unittest.skipUnless(shutil.which("mosh") and shutil.which("mosh-server"),
                         "stock Mosh client and server unavailable")
    def test_stock_local_mosh_transfers_both_directions(self):
        with tempfile.TemporaryDirectory(prefix="snag-stock-mosh-") as tmp:
            root = Path(tmp).resolve()
            home = root / "client"
            home.mkdir()
            remote_file = root / "remote.bin"
            remote_file.write_bytes(bytes(range(128)))
            local_file = home / "local.bin"
            local_file.write_bytes(bytes(reversed(range(128))))
            dotdir = root / "agent"

            def native_owners():
                # Mosh daemonizes outside the wrapper's descendant tree.
                # Match only this fixture's exact executable and private root.
                expected = " ".join([str(PRODUCT), "--dotdir", str(dotdir)])
                owners = {}
                for row in subprocess.check_output(
                        ["ps", "-axo", "pid=,command="], text=True).splitlines():
                    fields = row.split(None, 1)
                    if len(fields) != 2 or fields[1] != expected:
                        continue
                    pid = int(fields[0])
                    identity = subprocess.run(
                        ["ps", "-p", str(pid), "-o", "lstart=,command="],
                        capture_output=True, text=True, check=False).stdout.strip()
                    if identity:
                        owners[pid] = identity
                return owners

            child = RemoteProcess(home, ["mosh", "--local", "--no-init",
                                   "--predict=never", "127.0.0.1", str(PRODUCT),
                                   "--dotdir", str(dotdir)], winsize=(24, 80))
            try:
                child.until("›".encode(), 20)
                self.assertTrue(native_owners(), "stock-Mosh fixture owner was not identified")
                mode = termios.tcgetattr(child.slave)
                os.write(child.master, f"/send {remote_file}\r".encode())
                target = home / "Downloads" / remote_file.name
                output = child.until(b"Client acknowledged the file digest and final EXIT.", 35)
                self.assertEqual(target.read_bytes(), remote_file.read_bytes())
                self.assertNotIn(b"SNAJPAGENT-SCREEN/1", output)
                self.assertNotIn(b"#DATA:", output)
                os.write(child.master, b"/receive\r")
                child.until(b"Select local file", 20)
                os.write(child.master, f"{local_file}\r".encode())
                output = child.until(b"1 unsent attachment(s)", 35)
                media = next((dotdir / "sessions").glob("*/media"))
                self.assertEqual([p.read_bytes() for p in media.iterdir()],
                                 [local_file.read_bytes()])
                self.assertNotIn(b"SNAJPAGENT-SCREEN/1", output)
                os.write(child.master, b"ping\r")
                child.until(b"pong", 10)
                self.assertEqual(termios.tcgetattr(child.slave), mode)
                child.process.send_signal(signal.SIGTERM)
                # Mosh may handle the forwarded signal and exit normally.
                self.assertIn(self.wait_exited(child), (0, 128 + signal.SIGTERM))
            finally:
                child.close()
                # Disconnect intentionally preserves production owners. Stop
                # only the fixture's retained owners before removing its root.
                for pid, identity in native_owners().items():
                    current = subprocess.run(
                        ["ps", "-p", str(pid), "-o", "lstart=,command="],
                        capture_output=True, text=True, check=False).stdout.strip()
                    if current == identity:
                        try:
                            os.kill(pid, signal.SIGTERM)
                        except ProcessLookupError:
                            pass
                deadline = time.monotonic() + 5
                while native_owners() and time.monotonic() < deadline:
                    time.sleep(.05)
                self.assertFalse(native_owners(), "stock-Mosh fixture left a native owner")

    def test_screen_title_handshake_is_hidden_from_terminal(self):
        with tempfile.TemporaryDirectory(prefix="snag-title-handshake-") as tmp:
            root = Path(tmp)
            peer = textwrap.dedent(r'''
                import os
                import termios
                import tty

                original = termios.tcgetattr(0)
                tty.setraw(0)
                os.write(1, b"\x1b]2;[mosh] SNAJPAGENT-SCREEN/1:HELLO:01234567\x07")
                answer = bytearray()
                while not answer.endswith(b"\n"):
                    answer.extend(os.read(0, 128))
                termios.tcsetattr(0, termios.TCSANOW, original)
                if answer == b"#SNAJPAGENT-SCREEN:READY:01234567\n":
                    os.write(1, b"TITLE_HANDSHAKE_OK\n")
                else:
                    os.write(1, b"TITLE_HANDSHAKE_BAD_REPLY\n")
            ''')
            child = RemoteProcess(root, [sys.executable, "-u", "-c", peer])
            try:
                output = child.until(b"TITLE_HANDSHAKE_OK", 8)
                self.assertNotIn(b"SNAJPAGENT-SCREEN/1", output)
                self.assertEqual(child.process.wait(timeout=5), 0)
            finally:
                child.close()

    def test_screen_title_frames_do_not_leak_and_other_titles_pass(self):
        with tempfile.TemporaryDirectory(prefix="snag-title-bounds-") as tmp:
            root = Path(tmp)
            peer = textwrap.dedent(r'''
                import os
                import termios
                import time
                import tty

                original = termios.tcgetattr(0)
                tty.setraw(0)
                plain = b"\x1b]2;OTHER_TITLE:" + b"X" * 600 + b"\x07"
                bad = b"\x1b]2;[mosh] SNAJPAGENT-SCREEN/1:HELLO:0123456g\x07"
                huge = b"\x1b]2;[mosh] SNAJPAGENT-SCREEN/1:DATA:" + b"Y" * 600 + b"\x07"
                good = b"\x1b]2;SNAJPAGENT-SCREEN/1:HELLO:89abcdef\x1b\\"
                for title in (plain, bad, huge):
                    os.write(1, title)
                os.write(1, good[:17])
                time.sleep(0.02)
                os.write(1, good[17:])
                answer = bytearray()
                while not answer.endswith(b"\n"):
                    answer.extend(os.read(0, 128))
                termios.tcsetattr(0, termios.TCSANOW, original)
                if answer == b"#SNAJPAGENT-SCREEN:READY:89abcdef\n":
                    os.write(1, b"TITLE_BOUNDS_OK\n")
                else:
                    os.write(1, b"TITLE_BOUNDS_BAD_REPLY\n")
            ''')
            child = RemoteProcess(root, [sys.executable, "-u", "-c", peer])
            try:
                output = child.until(b"TITLE_BOUNDS_OK", 8)
                self.assertIn(b"\x1b]2;OTHER_TITLE:" + b"X" * 600 + b"\x07", output)
                self.assertNotIn(b"SNAJPAGENT-SCREEN/1", output)
                self.assertNotIn(b"Y" * 20, output)
                self.assertEqual(child.process.wait(timeout=5), 0)
            finally:
                child.close()

    def test_partial_transfer_title_cannot_escape_on_child_exit(self):
        with tempfile.TemporaryDirectory(prefix="snag-title-partial-") as tmp:
            root = Path(tmp)
            peer = textwrap.dedent(r'''
                import os

                os.write(1, b"ORDINARY_OUTPUT\n")
                os.write(1, b"\x1b]2;SNAJPAGENT-SCREEN/1:DATA:private-fragment")
            ''')
            child = RemoteProcess(root, [sys.executable, "-u", "-c", peer])
            try:
                child.until(b"ORDINARY_OUTPUT", 8)
                self.assertEqual(child.process.wait(timeout=5), 0)
                while select.select([child.master], [], [], 0)[0]:
                    try:
                        part = os.read(child.master, 8192)
                    except OSError:
                        break
                    if not part:
                        break
                    child.output.extend(part)
                self.assertNotIn(b"SNAJPAGENT-SCREEN/1", child.output)
            finally:
                child.close()

    def test_plain_terminal_startup_keeps_direct_owner(self):
        for term in ("dumb", ""):
            with self.subTest(term=term), tempfile.TemporaryDirectory(prefix="snag-plain-") as tmp:
                home = Path(tmp)
                child = RemoteProcess(home, ["--dotdir", str(home / "dotdir")],
                                      wrapped=False, extra_env={"TERM": term})
                try:
                    child.until("›".encode())
                    if hasattr(os, "pidfd_open"):
                        self.assertEqual(list(child.children.handles), [child.process.pid])
                    os.write(child.master, b"ping\n")
                    child.until(b"pong")
                    child.output.clear()
                    child.until("›".encode(), 8)
                    os.write(child.master, b"/exit\n")
                    child.until(b"--resume", 8)
                    child.wait(0)
                finally:
                    child.close()

    @unittest.skipUnless(hasattr(os, "pidfd_open"), "native attachment needs Linux")
    def test_native_attach_refuses_unsupported_terminal_without_taking_owner(self):
        with tempfile.TemporaryDirectory(prefix="snag-attach-capability-") as tmp:
            root = Path(tmp)
            original = ProductSession(root)
            home = root / "home"
            try:
                sid = original.session_dir().name
                original.write(b"/s d\r")
                self.assertEqual(original.process.wait(timeout=3), 0)
                command = [str(PRODUCT), "--dotdir", str(original.dotdir), "-A", sid]
                for term in ("dumb", "", "x" * 256):
                    with self.subTest(term=term[:10]):
                        bad = RemoteProcess(home, command, wrapped=None, extra_env={"TERM": term})
                        try:
                            message = b"File name too long" if len(term) == 256 else b"ANSI-capable TERM"
                            bad.until(message)
                            bad.wait(3)
                        finally:
                            bad.close()
                self.assertEqual(original.request_count(), 0)
                good = RemoteProcess(home, command, wrapped=None)
                try:
                    good.until(b"Attached session", 6)
                    good.until("›".encode())
                    os.write(good.master, b"ping\r")
                    good.until(b"pong")
                    os.write(good.master, b"/exit\r")
                    good.wait(0)
                finally:
                    good.close()
                self.assertEqual(original.request_count(), 1)
            finally:
                original.close()

    @unittest.skipUnless(hasattr(os, "pidfd_open") and shutil.which("screen"),
                         "native profile replacement needs Linux and GNU screen")
    def test_native_attachment_rebinds_terminal_profile(self):
        with tempfile.TemporaryDirectory(prefix="snag-attach-profile-") as tmp:
            root = Path(tmp)
            original_root = root / "original"
            original_root.mkdir(mode=0o700)
            sockets = root / "screens"
            sockets.mkdir(mode=0o700)
            config = root / "screenrc"
            config.write_text("startup_message off\naltscreen off\n")
            snapshot = root / "editor-profile"
            editor = root / "editor"
            editor.write_text("#!/bin/sh\n"
                f"printf '%s\\n%s\\n' \"$TERM\" \"${{STY-}}\" >{shlex.quote(str(snapshot))}\n"
                "printf 'PROFILE_EDITOR_DONE\\n'\n")
            editor.chmod(0o700)
            original = ProductSession(original_root,
                                      extra_env={"EDITOR": str(editor), "PAGER": str(editor)})
            home = original_root / "home"
            screen_env = dict(os.environ, SCREENDIR=str(sockets))
            try:
                sid = original.session_dir().name
                original.write(b"/s d\r")
                self.assertEqual(original.process.wait(timeout=3), 0)
                for screen in (True, False):
                    with self.subTest(screen=screen):
                        command = [str(PRODUCT), "--dotdir", str(original.dotdir), "-A", sid]
                        if screen:
                            command = ["screen", "-U", "-c", str(config), "-S", "profile", *command]
                        child = RemoteProcess(home, command, extra_env={"SCREENDIR": str(sockets)})
                        try:
                            child.until(b"Attached session", 6)
                            child.until("›".encode())
                            for action in (b"/config\r", b"/help\r"):
                                child.output.clear()
                                snapshot.unlink(missing_ok=True)
                                os.write(child.master, action)
                                child.until(b"PROFILE_EDITOR_DONE")
                                term, sty = snapshot.read_text().splitlines()
                                if screen:
                                    self.assertTrue(term.startswith("screen"), (term, sty))
                                    self.assertTrue(sty.endswith(".profile"), (term, sty))
                                else:
                                    self.assertEqual((term, sty), ("xterm-256color", ""))
                            source = home / ("screen.bin" if screen else "plain.bin")
                            source.write_bytes(bytes(range(256)) * 300)
                            os.write(child.master, f"download_tool {source}\r".encode())
                            target = home / "Downloads" / source.name
                            child.until(str(target).encode(), 12)
                            self.assertEqual(target.read_bytes(), source.read_bytes())
                            self.assertNotIn(b"#DATA:", child.output)
                            os.write(child.master, b"/s d\r" if screen else b"/exit\r")
                            child.wait(0)
                        finally:
                            child.close()
                records = [json.loads(line) for line in
                           (original.session_dir() / "events.jsonl").read_text().splitlines()]
                self.assertEqual(sum(e["type"] == "tool_finished" for e in records), 2)
                self.assertEqual(sum(e["type"] == "download_removed" for e in records), 2)
            finally:
                subprocess.run(["screen", "-S", "profile", "-X", "quit"], env=screen_env,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
                original.close()

    @unittest.skipUnless(shutil.which("tmux"), "tmux is required for rendered transfer coverage")
    def test_native_drop_keeps_one_composer_and_sealed_rollout(self):
        from tmux_terminal import TmuxTerminal, DEFAULT_ACCOUNTED_IDLE_PROMPT
        with tempfile.TemporaryDirectory(prefix="snag-drop-view-") as tmp:
            root = Path(tmp)
            launcher = root / "wrapped-agent"
            program = shlex.quote(str(PRODUCT))
            launcher.write_text(f'#!/bin/sh\nexec {program} remote {program} "$@"\n')
            launcher.chmod(0o700)
            source = root / "note.md"
            source.write_text("native transfer viewport regression\n")
            with TmuxTerminal(root / "terminal", launcher, root, root / "agent", None,
                              100, 28) as term:
                term.wait("›")
                term.submit("ping")
                term.wait("pong")
                before = term.wait(DEFAULT_ACCOUNTED_IDLE_PROMPT)
                term.send_text("\x1b[200~" + str(source) + "\x1b[201~")
                term.wait("1 unsent attachment(s)")
                after = term.wait("[1 attached]")
                self.assertEqual(after.count("pong"), 1, after)
                self.assertEqual(after.count("session id"), 1, after)
                self.assertEqual(after.count("›"), before.count("›"), after)
                term.exit()

    def test_native_drop_preserves_draft(self):
        for form in ("plain", "escaped", "quoted", "paste", "fragmented", "nested", "active", "narrow"):
            with self.subTest(form=form), tempfile.TemporaryDirectory(prefix="snag-drop-") as tmp:
                root = Path(tmp)
                name = "HANDOFF_ALL.md" if form == "plain" else "résumé 'notes'.md"
                source = root / name
                source.write_bytes(b"native dropped file\n")
                command = [str(PRODUCT), "--dotdir", str(root / "agent")]
                if form == "nested":
                    command = [str(PRODUCT), "remote", *command]
                child = RemoteProcess(root, command)
                try:
                    if form == "narrow":
                        fcntl.ioctl(child.slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 24, 0, 0))
                    child.until("›".encode(), 8)
                    if form == "active":
                        os.write(child.master, b"defer_slow_test\r")
                        child.until(b"working slowly", 8)
                    if form != "plain":
                        os.write(child.master, b"ping")
                        child.until(b"ping", 8)
                    if form == "plain":
                        data = str(source).encode() + b" "
                    elif form == "escaped":
                        data = str(source).replace(" ", "\\ ").replace("'", "\\'").encode() + b" "
                    elif form == "quoted":
                        data = ('"' + str(source) + '" ').encode()
                    else:
                        data = b"\x1b[200~" + str(source).encode() + b"\x1b[201~"
                    if form == "fragmented":
                        for at in range(0, len(data), 3):
                            os.write(child.master, data[at:at + 3])
                            time.sleep(0.005)
                    else:
                        os.write(child.master, data)
                    child.until(b"1 unsent attachment(s)", 8)
                    self.assertNotIn(b"\x1b[?1049h", child.output,
                                     "native upload blanked the conversation")
                    self.assertIn(b"Upload 100%" if form == "narrow" else b"Upload [", child.output)
                    self.assertIn(b"100%", child.output)
                    landed = list((root / "agent" / "sessions").glob("*/media/*"))
                    self.assertTrue(any(p.read_bytes() == source.read_bytes() for p in landed))
                    self.assertNotIn(b"unknown slash command", child.output)
                    if form == "active":
                        # Streamed words have composer redraws between them.
                        child.until(b" complete", 8)
                    os.write(child.master, b"ping\r" if form == "plain" else b"\r")
                    child.until(b"pong", 8)
                    os.write(child.master, b"/exit\r")
                    child.wait(0)
                finally:
                    child.close()

    def test_native_drop_leaves_generic_child_input_unchanged(self):
        with tempfile.TemporaryDirectory(prefix="snag-drop-shell-") as tmp:
            root = Path(tmp)
            source = root / "regular.md"
            source.write_text("do not transfer to generic child")
            code = textwrap.dedent('''
                import os, tty
                tty.setraw(0)
                print('READY', flush=True)
                data = b''
                while not data.endswith(b'\x1b[201~'):
                    data += os.read(0, 4096)
                print(data.hex(), flush=True)
            ''')
            child = RemoteProcess(root, [sys.executable, "-u", "-c", code])
            try:
                child.until(b"READY", 8)
                data = b"\x1b[200~" + str(source).encode() + b"\x1b[201~"
                os.write(child.master, data)
                child.until(data.hex().encode(), 8)
                child.wait(0)
            finally:
                child.close()

    def test_native_drop_keeps_nonfile_and_embedded_paths_literal(self):
        with tempfile.TemporaryDirectory(prefix="snag-drop-literal-") as tmp:
            root = Path(tmp)
            source = root / "regular.md"
            source.write_text("explicit drops only")
            link = root / "link.md"
            link.symlink_to(source)
            fifo = root / "fifo"
            os.mkfifo(fifo)
            payloads = [b"mention " + str(source).encode() + b"\r",
                        b"\x1b[A\x1b[B", str(link).encode() + b"\r",
                        str(root).encode() + b"\r", str(fifo).encode() + b"\r",
                        str(root / "missing").encode() + b"\r",
                        b"\x1b[200~ordinary text " + str(source).encode() + b"\x1b[201~",
                        b"\x1b[200~" + b"x" * 40000 + str(source).encode() + b"\x1b[201~"]
            code = textwrap.dedent(r'''
                import hashlib, json, os, sys, tty
                tty.setraw(0)
                os.write(1, b'\x1b[?9002hREADY\n')
                for size in json.loads(sys.argv[1]):
                    data = b''
                    while len(data) < size:
                        data += os.read(0, min(4096, size - len(data)))
                    print(hashlib.sha256(data).hexdigest(), flush=True)
            ''')
            child = RemoteProcess(root, [sys.executable, "-u", "-c", code,
                                         json.dumps([len(data) for data in payloads])])
            try:
                child.until(b"READY", 8)
                for data in payloads:
                    os.write(child.master, data)
                    child.until(hashlib.sha256(data).hexdigest().encode(), 8)
                child.wait(0)
            finally:
                child.close()

    def test_native_drop_cancel_keeps_draft(self):
        with tempfile.TemporaryDirectory(prefix="snag-drop-cancel-") as tmp:
            root = Path(tmp)
            source = root / "cancel.bin"
            source.write_bytes(os.urandom(65536))
            child = RemoteProcess(root, [str(PRODUCT), "--dotdir", str(root / "agent")])
            try:
                child.until("›".encode(), 8)
                os.write(child.master, b"ping")
                child.until(b"ping", 8)
                os.write(child.master, b"\x1b[200~" + str(source).encode() + b"\x1b[201~\x03")
                child.until(b"Upload cancelled", 8)
                self.assertNotIn(b"1 unsent attachment(s)", child.output)
                os.write(child.master, b"\r")
                child.until(b"pong", 8)
                os.write(child.master, b"/exit\r")
                child.wait(0)
            finally:
                child.close()

    def test_native_upload_caps_bursts_when_peer_advertises_large_blocks(self):
        with tempfile.TemporaryDirectory(prefix="snag-remote-burst-") as tmp:
            root = Path(tmp)
            upload = root / "bulk.bin"
            upload.write_bytes(os.urandom(16384))
            peer = textwrap.dedent(r'''
                import base64, hashlib, json, sys, tty, zlib
                from pathlib import Path
                sys.path.insert(0, sys.argv[1])
                from test_upload_client import frame
                tty.setraw(sys.stdin.fileno())
                def send(kind, data, numeric=False):
                    sys.stdout.buffer.write(frame(kind, data, numeric))
                    sys.stdout.buffer.flush()
                def receive(kind, numeric=False):
                    line = sys.stdin.buffer.readline()
                    prefix, payload = line.rstrip(b"\n").split(b":", 1)
                    assert prefix == b"#" + kind.encode(), line
                    return int(payload) if numeric else zlib.decompress(base64.b64decode(payload))
                sys.stdout.buffer.write(b"::TRZSZ:TRANSFER:R:1.0.0:0000000000000:0\r\n")
                sys.stdout.buffer.flush()
                assert json.loads(receive("ACT"))["native"]
                send("CFG", b'{"protocol":1,"bufsize":65536,"binary":false,"directory":false}')
                assert receive("NUM", True) == 1
                send("SUCC", 1, True)
                name = receive("NAME")
                send("SUCC", name)
                size = receive("SIZE", True)
                send("SUCC", size, True)
                data = bytearray()
                while len(data) < size:
                    block = receive("DATA")
                    assert 0 < len(block) <= 1024, len(block)
                    data.extend(block)
                    send("SUCC", len(block), True)
                assert bytes(data) == Path(sys.argv[2]).read_bytes()
                digest = hashlib.md5(data).digest()
                assert receive("MD5") == digest
                send("SUCC", digest)
                assert receive("EXIT") == b"Sent"
                print("NATIVE_UPLOAD_BURST_OK", flush=True)
            ''')
            child = RemoteProcess(root, [sys.executable, "-u", "-c", peer,
                                         str(PRODUCT.parent), str(upload)])
            try:
                child.until(b"Select local file", 8)
                os.write(child.master, str(upload).encode() + b"\r")
                child.until(b"NATIVE_UPLOAD_BURST_OK", 8)
                child.wait(0)
            finally:
                child.close()

    def test_agent_transfer_names_have_no_old_aliases(self):
        with tempfile.TemporaryDirectory(prefix="snag-remote-names-") as tmp:
            root = Path(tmp)
            child = RemoteProcess(root, [str(PRODUCT), "--dotdir", str(root / "agent")],
                                  extra_env={"PAGER": "cat"})
            try:
                child.until("›".encode(), 8)
                os.write(child.master, b"/send\r")
                child.until(b"usage: /send PATH", 8)
                child.output.clear()
                for command in (b"/upload", b"/download missing", b"/receive extra",
                                b"/send-more", b"/sendfile"):
                    os.write(child.master, command + b"\r")
                    child.until(b"unknown slash command", 8)
                    child.output.clear()
                os.write(child.master, b"/help\r")
                output = child.until(b"/receive", 8)
                output = child.until(b"/send PATH", 8)
                self.assertNotIn(b"/upload", output)
                self.assertNotIn(b"/download", output)
                child.until(b"\x1b[?2004h", 8)
                os.write(child.master, b"/exit\r")
                child.wait(0)
            finally:
                child.close()

    def test_resize_reaches_child_pty(self):
        with tempfile.TemporaryDirectory(prefix="snag-remote-resize-") as tmp:
            code = ("import fcntl,os,signal,struct,termios,time; "
                    "signal.signal(signal.SIGWINCH,lambda *_: "
                    "print('SIZE',*struct.unpack('HHHH',fcntl.ioctl(0,termios.TIOCGWINSZ,b'\\0'*8))[:2],flush=True)); "
                    "print('READY',flush=True); time.sleep(1.5)")
            child = RemoteProcess(Path(tmp), [sys.executable, "-c", code])
            try:
                child.until(b"READY")
                fcntl.ioctl(child.slave, termios.TIOCSWINSZ, struct.pack("HHHH", 31, 107, 0, 0))
                child.process.send_signal(signal.SIGWINCH)
                child.until(b"SIZE 31 107")
                child.wait(0)
            finally:
                child.close()

    def test_native_upload_cancel_restores_input(self):
        with tempfile.TemporaryDirectory(prefix="snag-remote-cancel-") as tmp:
            root = Path(tmp)
            child = RemoteProcess(root, [str(PRODUCT), "--dotdir", str(root / "agent")])
            try:
                child.until("›".encode(), 8)
                os.write(child.master, b"/receive\r")
                child.until(b"Select local file", 8)
                os.write(child.master, b"\x03")
                child.until(b"Upload cancelled", 8)
                os.write(child.master, b"ping\r")
                child.until(b"pong", 8)
                os.write(child.master, b"/exit\r")
                child.wait(0)
            finally:
                child.close()

    def test_nested_wrapper_relays_to_workstation_destination(self):
        # Darwin's bundled screen rejects the third nested TERM as too long.
        layers = (0, 1) if sys.platform == "darwin" else (0, 1, 2)
        for screen in layers:
            if screen and not shutil.which("screen"):
                continue
            with self.subTest(screen=screen), tempfile.TemporaryDirectory(prefix="snag-nested-") as tmp:
                root = Path(tmp)
                home = root / "client"
                home.mkdir()
                source = root / "nested.bin"
                source.write_bytes(bytes(range(256)) * 500)
                inner_config = root / "inner.ini"
                inner_destination = root / "wrong-endpoint"
                inner_config.write_text(f"[terminal]\ndownload_dir = {inner_destination}\n")
                command = [str(PRODUCT), "remote", "--config", str(inner_config),
                           str(PRODUCT), "--dotdir", str(root / "agent")]
                env = {}
                if screen:
                    sockets = root / "screens"
                    sockets.mkdir(mode=0o700)
                    env["SCREENDIR"] = str(sockets)
                    cfg = root / "screenrc"
                    cfg.write_text("startup_message off\naltscreen off\n")
                    if screen == 2:
                        command = command[:4] + ["screen", "-m", "-c", str(cfg),
                            "-S", "inner", *command[4:]]
                    command = ["screen", "-c", str(cfg), "-S", "nested", *command]
                child = RemoteProcess(home, command, extra_env=env)
                try:
                    child.until("›".encode(), 8)
                    os.write(child.master, f"download_tool {source}\r".encode())
                    target = home / "Downloads" / source.name
                    child.until(str(target).encode(), 12)
                    self.assertEqual(target.read_bytes(), source.read_bytes())
                    self.assertFalse(inner_destination.exists())
                    child.until_after(str(target).encode(), "›".encode(), 8)
                    upload = home / "workstation-upload.bin"
                    upload.write_bytes(os.urandom(16384))
                    os.write(child.master, b"/receive\r")
                    child.until(b"Select local file", 8)
                    os.write(child.master, str(upload).encode() + b"\r")
                    child.until(b"1 unsent attachment(s)", 12)
                    landed = list((root / "agent" / "sessions").glob("*/media/*"))
                    self.assertTrue(any(path.read_bytes() == upload.read_bytes() for path in landed))
                    child.until_after(b"1 unsent attachment(s)", "›".encode(), 8)
                    os.write(child.master, b"/exit\r")
                    child.wait(0)
                finally:
                    if screen:
                        if screen == 2:
                            subprocess.run(["screen", "-S", "inner", "-X", "quit"],
                                           env=dict(os.environ, **env), timeout=5,
                                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                        subprocess.run(["screen", "-S", "nested", "-X", "quit"],
                                       env=dict(os.environ, **env), timeout=5,
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    child.close()

    @unittest.skipUnless(shutil.which("screen"), "GNU screen unavailable")
    def test_local_screen_contains_native_workstation_wrapper(self):
        with tempfile.TemporaryDirectory(prefix="snag-local-screen-") as tmp:
            root = Path(tmp).resolve()
            home = root / "client"
            home.mkdir()
            client_config = home / "terminal.ini"
            client_config.write_text(
                f"[terminal]\ndownload_dir = {home / 'Downloads'}\n")
            sockets = root / "screens"
            sockets.mkdir(mode=0o700)
            cfg = root / "screenrc"
            cfg.write_text("startup_message off\naltscreen off\n")
            source = root / "local-screen.bin"
            source.write_bytes(b"local screen workstation bytes")
            env = {"SCREENDIR": str(sockets)}
            child = RemoteProcess(home, ["screen", "-c", str(cfg), "-S", "local",
                str(PRODUCT), "remote", "--config", str(client_config),
                str(PRODUCT), "--dotdir", str(root / "agent")],
                wrapped=None, extra_env=env)
            try:
                child.until("›".encode(), 8)
                os.write(child.master, f"/send {source}\r".encode())
                target = home / "Downloads" / source.name
                child.until(str(target).encode(), 12)
                self.assertEqual(target.read_bytes(), source.read_bytes())
                child.until_after(str(target).encode(), "›".encode(), 8)
                os.write(child.master, b"/exit\r")
                child.until(b"--resume", 8)
                subprocess.run(["screen", "-S", "local", "-X", "quit"],
                               env=dict(os.environ, **env), timeout=5,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                if sys.platform == "darwin":
                    # GNU screen can remain in final exit while its parent
                    # holds the fixture PTY; the agent has already exited.
                    os.close(child.master)
                    child.master = None
                child.wait(0)
            finally:
                subprocess.run(["screen", "-S", "local", "-X", "quit"],
                               env=dict(os.environ, **env), timeout=5,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                child.close()

    def test_literal_child_argv_exit_and_no_agent_state(self):
        with tempfile.TemporaryDirectory(prefix="snag-remote-") as tmp:
            home = Path(tmp)
            args = ["-e", "a b", "$(touch forbidden)", "résumé"]
            child = RemoteProcess(home, [sys.executable, "-c",
                "import json,sys; print(json.dumps(sys.argv[1:],ensure_ascii=False),flush=True); sys.exit(37)",
                *args])
            try:
                child.until(json.dumps(args, ensure_ascii=False).encode())
                child.wait(37)
                self.assertFalse((home / ".snajpagent").exists())
                self.assertFalse((home / "forbidden").exists())
            finally:
                child.close()

    def test_config_reads_only_terminal_without_provider_startup(self):
        with tempfile.TemporaryDirectory(prefix="snag-remote-cfg-") as tmp:
            home = Path(tmp)
            config = home / "config.ini"
            config.write_text("[provider invalid provider]\nnot even valid agent config\n"
                              "[terminal]\ndownload_dir = ~/Incoming files\n")
            child = RemoteProcess(home, ["--config", str(config), "/bin/sh", "-c", "printf CLIENT_ONLY"])
            try:
                child.until(b"CLIENT_ONLY")
                child.wait(0)
                self.assertFalse((home / ".snajpagent").exists())
            finally:
                child.close()

    def test_terminal_invalid_key_and_duplicate_rejected_before_child(self):
        for text in ("[terminal]\nprovider = nonsense\n",
                     "[terminal]\ndownload_dir = /tmp\ndownload_dir = /tmp\n"):
            with self.subTest(text=text), tempfile.TemporaryDirectory(prefix="snag-remote-bad-") as tmp:
                home = Path(tmp)
                config = home / "config.ini"
                config.write_text(text)
                child = RemoteProcess(home, ["--config", str(config), "/bin/sh", "-c", "touch launched"])
                try:
                    child.until(b"invalid [terminal] configuration")
                    child.wait(2)
                    self.assertFalse((home / "launched").exists())
                finally:
                    child.close()

    def test_external_termination_restores_terminal_and_child(self):
        with tempfile.TemporaryDirectory(prefix="snag-remote-term-") as tmp:
            child = RemoteProcess(Path(tmp), [sys.executable, "-c",
                "import os,time; print('READY',flush=True); time.sleep(60)"])
            try:
                child.until(b"READY")
                child.process.send_signal(signal.SIGTERM)
                child.wait(128 + signal.SIGTERM)
            finally:
                child.close()

    def test_native_download_default_directory_and_remote_receipt(self):
        with tempfile.TemporaryDirectory(prefix="snag-remote-download-") as tmp:
            root = Path(tmp)
            home, server = root / "client", root / "server"
            home.mkdir()
            server.mkdir()
            dotdir = root / "remote-session"
            data = bytes(range(256)) * 1000
            (server / "résumé file.bin").write_bytes(data)
            child = RemoteProcess(home, ["/bin/sh", "-c", 'cd "$1"; exec "$2" --dotdir "$3"',
                "remote-test", str(server), str(PRODUCT), str(dotdir)])
            try:
                child.until("›".encode(), 8)
                os.write(child.master, f"/send {server / 'résumé file.bin'}\r".encode())
                target = home / "Downloads" / "résumé file.bin"
                child.until(str(target).encode(), 12)
                self.assertEqual(target.read_bytes(), data)
                self.assertNotIn(b"\x1b[?1049h", child.output)
                self.assertIn(b"Download [", child.output)
                self.assertIn(b"100%", child.output)
                self.assertNotIn(b"#DATA:", child.output)
                self.assertNotIn(b"#CFG:", child.output)
                os.write(child.master, b"ping\r")
                child.until(b"pong", 8)
                os.write(child.master, b"/exit\r")
                child.wait(0)
                self.assertFalse((home / ".snajpagent").exists())
            finally:
                child.close()

    def test_native_upload_selection_then_prompt_roundtrip(self):
        with tempfile.TemporaryDirectory(prefix="snag-remote-upload-") as tmp:
            root = Path(tmp)
            home, dotdir = root / "client", root / "remote-session"
            home.mkdir()
            source = home / "notes file.txt"
            source.write_text("résumé native upload\n" * 300)
            child = RemoteProcess(home, [str(PRODUCT), "--dotdir", str(dotdir)])
            try:
                child.until("›".encode(), 8)
                os.write(child.master, b"/receive\r")
                child.until(b"Select local file", 8)
                os.write(child.master, f"{source}\r".encode())
                child.until(b"1 unsent attachment(s)", 12)
                self.assertNotIn(b"#DATA:", child.output)
                os.write(child.master, b"ping\r")
                child.until(b"pong", 8)
                os.write(child.master, b"/exit\r")
                child.wait(0)
                self.assertFalse((home / ".snajpagent").exists())
            finally:
                child.close()

    def test_native_empty_collision_and_configured_destination(self):
        with tempfile.TemporaryDirectory(prefix="snag-remote-collision-") as tmp:
            root = Path(tmp)
            home, server, destination = root / "client", root / "server", root / "chosen files"
            home.mkdir()
            server.mkdir()
            destination.mkdir()
            source = server / "empty.txt"
            source.write_bytes(b"")
            (destination / source.name).write_bytes(b"preserve")
            config = home / "client.ini"
            config.write_text(f"[terminal]\ndownload_dir = {destination}\n")
            child = RemoteProcess(home, ["--config", str(config), str(PRODUCT),
                                        "--dotdir", str(root / "remote-session")])
            try:
                child.until("›".encode(), 8)
                os.write(child.master, f"/send {source}\r".encode())
                child.until(b"Download completed:", 12)
                self.assertEqual((destination / source.name).read_bytes(), b"preserve")
                landed = [p for p in destination.iterdir() if p.name != source.name]
                self.assertEqual(len(landed), 1)
                self.assertEqual(landed[0].read_bytes(), b"")
                child.until(str(landed[0]).encode(), 5)
                os.write(child.master, b"/exit\r")
                child.wait(0)
                self.assertFalse((home / "Downloads").exists())
            finally:
                child.close()

    def queue_detached_download(self, root, data=b"queued bytes\n"):
        client_home = root / "client"
        remote_home = root / "remote"
        dotdir = root / "dotdir"
        for directory in (client_home, remote_home, dotdir):
            directory.mkdir(mode=0o700)
            directory.chmod(0o700)
        source = remote_home / "queued file.bin"
        source.write_bytes(data)
        env = dict(os.environ, HOME=str(remote_home), SNAJPAGENT_DOTDIR=str(dotdir),
                   TERM="xterm-256color")
        for key in ("STY", "TMUX", "TMUX_PANE"):
            env.pop(key, None)
        detached = subprocess.run([str(PRODUCT), "--dotdir", str(dotdir), "-e", "--",
                                   "download_tool ./queued file.bin"], cwd=remote_home, env=env,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
        self.assertEqual(detached.returncode, 0, detached.stderr)
        self.assertNotIn(b"::TRZSZ:TRANSFER:", detached.stdout + detached.stderr)
        journal = next((dotdir / "sessions").glob("*/events.jsonl"))
        events = [json.loads(line) for line in journal.read_text().splitlines()]
        queued = [e["data"] for e in events if e["type"] == "download_queued"]
        self.assertEqual(len(queued), 1)
        return client_home, remote_home, dotdir, journal, events[0]["session_id"], source, env

    def test_wrapped_resume_delivers_queued_download_and_clears_item(self):
        with tempfile.TemporaryDirectory(prefix="snag-remote-reconnect-") as tmp:
            data = ("queued reconnect bytes\n" * 2000).encode()
            root = Path(tmp)
            client_home, remote_home, dotdir, journal, sid, _source, env = \
                self.queue_detached_download(root, data)
            child = RemoteProcess(client_home, [str(PRODUCT), "--dotdir", str(dotdir), "--resume", sid])
            try:
                target = client_home / "Downloads" / "queued file.bin"
                child.until(str(target).encode(), 15)
                self.assertEqual(target.read_bytes(), data)
                self.assertNotIn(b"#DATA:", child.output)
                os.write(child.master, b"/exit\r")
                child.wait(0)
            finally:
                child.close()
            events = [json.loads(line) for line in journal.read_text().splitlines()]
            self.assertEqual([e["type"] for e in events].count("download_removed"), 1)
            self.assertEqual(events[-1]["type"], "download_removed")
            listed = subprocess.run([str(PRODUCT), "--dotdir", str(dotdir), "--resume", sid,
                                     "-e", "--", "download_queue_list"], cwd=remote_home, env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
            self.assertEqual(listed.returncode, 0, listed.stderr)
            events = [json.loads(line) for line in journal.read_text().splitlines()]
            texts = [e["data"]["result"]["model_text"] for e in events if e["type"] == "tool_finished"]
            self.assertIn("0 pending workstation download(s).", texts[-1])

    def test_unwrapped_resume_leaves_queued_download_pending(self):
        with tempfile.TemporaryDirectory(prefix="snag-remote-unwrapped-") as tmp:
            root = Path(tmp)
            client_home, remote_home, dotdir, journal, sid, _source, env = \
                self.queue_detached_download(root)
            child = RemoteProcess(remote_home, ["--dotdir", str(dotdir), "--resume", sid],
                                  wrapped=False, cwd=remote_home)
            try:
                child.until("›".encode(), 8)
                self.assertFalse((client_home / "Downloads" / "queued file.bin").exists())
                os.write(child.master, b"/exit\r")
                child.wait(0)
            finally:
                child.close()
            events = [json.loads(line) for line in journal.read_text().splitlines()]
            self.assertEqual([e["type"] for e in events].count("download_removed"), 0)
            listed = subprocess.run([str(PRODUCT), "--dotdir", str(dotdir), "--resume", sid,
                                     "-e", "--", "download_queue_list"], cwd=remote_home,
                                    env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    timeout=10)
            self.assertEqual(listed.returncode, 0, listed.stderr)
            events = [json.loads(line) for line in journal.read_text().splitlines()]
            texts = [e["data"]["result"]["model_text"] for e in events
                     if e["type"] == "tool_finished"]
            self.assertIn("1 pending workstation download(s).", texts[-1])

    def test_wrapped_resume_keeps_changed_queued_source_pending(self):
        for digest_only in (False, True):
            with self.subTest(digest_only=digest_only), tempfile.TemporaryDirectory(prefix="snag-remote-stale-") as tmp:
                root = Path(tmp)
                original = b"original queued bytes\n"
                client_home, _remote_home, dotdir, journal, sid, source, _env = \
                    self.queue_detached_download(root, original)
                saved = source.stat()
                source.write_bytes(b"x" * len(original) if digest_only else
                                   b"changed queued bytes that must not be delivered\n")
                if digest_only:
                    os.utime(source, ns=(saved.st_atime_ns, saved.st_mtime_ns))
                child = RemoteProcess(client_home, [str(PRODUCT), "--dotdir", str(dotdir), "--resume", sid])
                try:
                    error = b"queued source digest changed" if digest_only else b"queued source changed"
                    child.until(error + b"; left pending", 12)
                    self.assertFalse((client_home / "Downloads" / "queued file.bin").exists())
                    os.write(child.master, b"/exit\r")
                    child.wait(0)
                finally:
                    child.close()
                events = [json.loads(line) for line in journal.read_text().splitlines()]
                self.assertEqual([e["type"] for e in events].count("download_removed"), 0)

    @unittest.skipUnless(shutil.which("screen"), "GNU screen unavailable")
    def test_detached_screen_send_queues_then_wrapped_attach_delivers(self):
        with tempfile.TemporaryDirectory(prefix="snag-remote-detach-") as tmp:
            root = Path(tmp)
            home, remote = root / "client", root / "server"
            home.mkdir()
            remote.mkdir()
            sockets = root / "screens"
            sockets.mkdir(mode=0o700)
            config = root / "screenrc"
            config.write_text("startup_message off\naltscreen off\n")
            dotdir = root / "dotdir"
            source = remote / "report.bin"
            source.write_bytes(b"detached screen bytes\n" * 2000)
            env = {"SCREENDIR": str(sockets)}
            child = RemoteProcess(home, ["screen", "-c", str(config), "-S", "native",
                str(PRODUCT), "--dotdir", str(dotdir)], extra_env=env)
            try:
                child.until("›".encode(), 8)
                command_env = dict(os.environ, **env)
                subprocess.run(["screen", "-S", "native", "-X", "detach"],
                               env=command_env, check=True, timeout=5)
                child.wait(0)
                subprocess.run(["screen", "-S", "native", "-X", "stuff",
                                f"download_tool {source}\r"],
                               env=command_env, check=True, timeout=5)
                deadline = time.monotonic() + 4
                events = []
                while time.monotonic() < deadline:
                    journals = list((dotdir / "sessions").glob("*/events.jsonl"))
                    if not journals:
                        time.sleep(0.05)
                        continue
                    journal = journals[0]
                    events = [json.loads(line) for line in journal.read_text().splitlines()]
                    if any(e["type"] == "download_queued" for e in events):
                        break
                    time.sleep(0.05)
                self.assertTrue(any(e["type"] == "download_queued" for e in events),
                                "detached screen must queue without a transfer timeout")
                deadline = time.monotonic() + 8
                while not any(e["type"] == "turn_completed" for e in events) and \
                        time.monotonic() < deadline:
                    time.sleep(0.05)
                    events = [json.loads(line) for line in journal.read_text().splitlines()]
                self.assertTrue(any(e["type"] == "turn_completed" for e in events),
                                "the detached model turn must finish before editing a draft")
                hardcopy = root / "hardcopy"
                self.assertNotIn(b"::TRZSZ:TRANSFER:",
                                 screen_snapshot(command_env, "native", hardcopy))
                # Leave a draft with its cursor before the final character.
                # Delivery on reattachment must preserve both draft and cursor.
                subprocess.run(["screen", "-S", "native", "-X", "stuff", "pig\x1b[D"],
                               env=command_env, check=True, timeout=5)
                time.sleep(0.2)
                again = RemoteProcess(home, ["screen", "-r", "native"], extra_env=env)
                try:
                    target = home / "Downloads" / source.name
                    again.until(str(target).encode(), 12)
                    self.assertEqual(target.read_bytes(), source.read_bytes())
                    rendered = screen_snapshot(command_env, "native", hardcopy)
                    # The receipt and restored composer can reach screen in
                    # separate relay frames; wait for the actual draft redraw.
                    deadline = time.monotonic() + 5
                    while b"pig" not in rendered and time.monotonic() < deadline:
                        time.sleep(0.05)
                        rendered = screen_snapshot(command_env, "native", hardcopy)
                    self.assertIn(str(target).encode(), rendered.replace(b"\n", b""))
                    self.assertIn(b"pig", rendered)
                    os.write(again.master, b"n\r")
                    again.until(b"pong", 8)
                    os.write(again.master, b"/exit\r")
                    again.wait(0)
                finally:
                    again.close()
            finally:
                subprocess.run(["screen", "-S", "native", "-X", "quit"],
                               env=dict(os.environ, **env), stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, timeout=5)
                child.close()

    def test_help_is_pure_and_nonterminal_refusal_is_factual(self):
        with tempfile.TemporaryDirectory(prefix="snag-remote-help-") as tmp:
            home = Path(tmp)
            env = dict(os.environ, HOME=tmp)
            for option, status in (("--help", 0), ("--unknown-option", 2)):
                result = subprocess.run([str(PRODUCT), "remote", option], env=env,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
                self.assertEqual(result.returncode, status)
                self.assertIn(b"does not start an agent", result.stderr)
                self.assertIn(b"/session detach", result.stderr)
                self.assertIn(b"ssh -t target snajpagent --attach", result.stderr)
                self.assertNotIn(b"screen -", result.stderr)
            result = subprocess.run([str(PRODUCT), "remote", "/bin/sh", "-c", "echo launched"], env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn(b"launched", result.stdout)
            self.assertIn(b"interactive workstation terminal", result.stderr)
            self.assertFalse((home / ".snajpagent").exists())


if __name__ == "__main__":
    unittest.main()
