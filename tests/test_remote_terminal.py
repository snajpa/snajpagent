# SPDX-License-Identifier: GPL-2.0-only
"""Native client-only remote-mode startup and PTY ownership regressions."""
import json
import fcntl
import struct
import os
import pty
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import termios
import time
import unittest
from pathlib import Path

PRODUCT = Path(__file__).resolve().parent / "snajpagent-fixture"


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
    def __init__(self, home, arguments, wrapped=True, cwd=None, extra_env=None):
        self.master, self.slave = pty.openpty()
        self.original = termios.tcgetattr(self.slave)
        env = dict(os.environ, HOME=str(home), TERM="xterm-256color", SHELL="/bin/sh")
        if extra_env:
            env.update(extra_env)
        for key in ("STY", "TMUX", "TMUX_PANE", "OPENAI_API_KEY"):
            env.pop(key, None)
        command = arguments if wrapped is None else [str(PRODUCT), *(["remote"] if wrapped else []), *arguments]
        def controlling_terminal():
            os.setsid()
            fcntl.ioctl(self.slave, termios.TIOCSCTTY, 0)
        self.process = subprocess.Popen(command, stdin=self.slave, stdout=self.slave,
                                        stderr=self.slave, cwd=cwd or home, env=env,
                                        preexec_fn=controlling_terminal)
        self.output = bytearray()

    def until(self, marker, timeout=5):
        deadline = time.monotonic() + timeout
        while marker not in self.output and time.monotonic() < deadline:
            if select.select([self.master], [], [], 0.1)[0]:
                self.output.extend(os.read(self.master, 65536))
        if marker not in self.output:
            raise AssertionError(f"missing {marker!r}: {bytes(self.output)!r}")
        return bytes(self.output)

    def wait(self, expected):
        assert self.process.wait(timeout=5) == expected, bytes(self.output)
        assert termios.tcgetattr(self.slave) == self.original

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        os.close(self.master)
        os.close(self.slave)


class RemoteStartupTests(unittest.TestCase):
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
                os.write(child.master, b"/upload\r")
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
        for screen in (0, 1, 2):
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
                    upload = home / "workstation-upload.bin"
                    upload.write_bytes(os.urandom(16384))
                    os.write(child.master, b"/upload\r")
                    child.until(b"Select local file", 8)
                    os.write(child.master, str(upload).encode() + b"\r")
                    child.until(b"1 unsent attachment(s)", 12)
                    landed = list((root / "agent" / "sessions").glob("*/media/*"))
                    self.assertTrue(any(path.read_bytes() == upload.read_bytes() for path in landed))
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
            root = Path(tmp)
            home = root / "client"
            home.mkdir()
            sockets = root / "screens"
            sockets.mkdir(mode=0o700)
            cfg = root / "screenrc"
            cfg.write_text("startup_message off\naltscreen off\n")
            source = root / "local-screen.bin"
            source.write_bytes(b"local screen workstation bytes")
            env = {"SCREENDIR": str(sockets)}
            child = RemoteProcess(home, ["screen", "-c", str(cfg), "-S", "local",
                str(PRODUCT), "remote", str(PRODUCT), "--dotdir", str(root / "agent")],
                wrapped=None, extra_env=env)
            try:
                child.until("›".encode(), 8)
                os.write(child.master, f"/download {source}\r".encode())
                target = home / "Downloads" / source.name
                child.until(str(target).encode(), 12)
                self.assertEqual(target.read_bytes(), source.read_bytes())
                os.write(child.master, b"/exit\r")
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
                os.write(child.master, f"/download {server / 'résumé file.bin'}\r".encode())
                target = home / "Downloads" / "résumé file.bin"
                child.until(str(target).encode(), 12)
                self.assertEqual(target.read_bytes(), data)
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
                os.write(child.master, b"/upload\r")
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
                os.write(child.master, f"/download {source}\r".encode())
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
                hardcopy = root / "hardcopy"
                self.assertNotIn(b"::TRZSZ:TRANSFER:",
                                 screen_snapshot(command_env, "native", hardcopy))
                # Leave a draft with its cursor before the final character.
                # Delivery on reattachment must preserve both draft and cursor.
                time.sleep(1.2)
                subprocess.run(["screen", "-S", "native", "-X", "stuff", "pig\x1b[D"],
                               env=command_env, check=True, timeout=5)
                time.sleep(0.2)
                again = RemoteProcess(home, ["screen", "-r", "native"], extra_env=env)
                try:
                    target = home / "Downloads" / source.name
                    again.until(str(target).encode(), 12)
                    self.assertEqual(target.read_bytes(), source.read_bytes())
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
            result = subprocess.run([str(PRODUCT), "remote", "--help"], env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
            self.assertEqual(result.returncode, 0)
            self.assertIn(b"does not start an agent", result.stderr)
            result = subprocess.run([str(PRODUCT), "remote", "/bin/sh", "-c", "echo launched"], env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn(b"launched", result.stdout)
            self.assertIn(b"interactive workstation terminal", result.stderr)
            self.assertFalse((home / ".snajpagent").exists())


if __name__ == "__main__":
    unittest.main()
