# SPDX-License-Identifier: GPL-2.0-only
"""Terminal download checks using the existing upload PTY fixture."""

import hashlib
import json
import os
import shutil
import shlex
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

from store_history import journal_paths, read_events
from test_upload_client import CLIENT, PRODUCT, Session, frame


class DownloadSession(Session):
    def __init__(self, root, wrapper=None, command=None, screen=False):
        self.home = root / "home"
        self.dotdir = root / "dotdir"
        self.downloads = root / "downloads"
        for directory in (self.home, self.dotdir, self.downloads):
            directory.mkdir(mode=0o700)
        (self.home / ".trzsz.conf").write_text(
            f"DefaultDownloadPath = {self.downloads}\n", encoding="utf-8")
        env = dict(os.environ, HOME=str(self.home), TERM="xterm-256color", PAGER="",
                   SNAJPAGENT_DOTDIR=str(self.dotdir))
        command = command or [str(PRODUCT), "--dotdir", str(self.dotdir)]
        self.screen_name = None
        self.screen_env = None
        if screen:
            sockets = root / "screens"
            sockets.mkdir(mode=0o700)
            cfg = root / "screenrc"
            cfg.write_text(f"startup_message off\ndefscrollback 1000\naltscreen {screen if isinstance(screen, str) else 'off'}\n")
            env["SCREENDIR"] = str(sockets)
            self.screen_name = "dl"
            self.screen_env = env
            command = ["screen", "-c", str(cfg), "-S", self.screen_name, *command]
        super().__init__(root, wrapper, command=command, ready="› ".encode(),
                         cwd=self.home, env=env)

    def close(self):
        if self.screen_name:
            subprocess.run(["screen", "-S", self.screen_name, "-X", "quit"],
                           env=self.screen_env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=5)
        super().close()

    def hardcopy(self):
        output = self.home / "hardcopy"
        subprocess.run(["screen", "-S", self.screen_name, "-X", "hardcopy", "-h", str(output)],
                       env=self.screen_env, check=True, timeout=5)
        return output.read_bytes()

    def events(self):
        journals = journal_paths(self.dotdir)
        return read_events(journals[0])

    def download(self, name, data, request=None, expected_name=None):
        (self.home / name).write_bytes(data)
        self.write(request or f"/send ./{name}\r".encode())
        if request and request.startswith(b"download_tool "):
            self.read_until(b"\x1b[?9001;")
            nonce = self.read_until(b"n")[:-1]
            self.write(b"\x1b[>" + nonce + b"S")
        self.read_until(b"::TRZSZ:TRANSFER:S:1.0.0:")
        marker = self.read_until(b"\r\n")
        assert len(marker) == 17 and marker[:-4].isdigit() and marker.endswith(b"00:0\r\n"), marker
        self.write(frame("ACT", json.dumps(
            {"confirm": True, "protocol": 4, "newline": "\n"}).encode()))
        config = json.loads(self.decoded("CFG"))
        assert config["protocol"] == 1 and not config["binary"]
        assert self.read_frame("NUM") == b"1"
        self.write(frame("SUCC", 1, numeric=True))
        received_name = self.decoded("NAME")
        assert received_name == (expected_name or name.encode()), received_name
        self.write(frame("SUCC", name.encode()))
        size = int(self.read_frame("SIZE"))
        assert size == len(data)
        self.write(frame("SUCC", size, numeric=True))
        received = b""
        while len(received) < size:
            block = self.decoded("DATA")
            received += block
            self.write(frame("SUCC", len(block), numeric=True))
        assert received == data
        return self.decoded("MD5")

    def decoded(self, kind):
        import base64
        import zlib
        return zlib.decompress(base64.b64decode(self.read_frame(kind)))

    def finish(self, digest, tail=b"", marker=b"Download completed:"):
        self.write(frame("SUCC", digest) + frame("EXIT", b"Saved") + tail)
        self.read_until(marker)

    def exit(self):
        self.write(b"/exit\r")
        if self.screen_name and sys.platform == "darwin":
            # The parent master can hold a macOS screen frontend in E (exiting).
            self.read_until(b"--resume", 5)
            os.close(self.master)
            self.master = None
        assert self.process.wait(timeout=5) == 0


class DownloadTests(unittest.TestCase):
    @unittest.skipUnless(hasattr(os, "pidfd_open"), "native attachment needs Linux")
    def test_native_download_disconnect_preserves_pending_intent(self):
        for tool in (False, True):
            with self.subTest(tool=tool), tempfile.TemporaryDirectory(prefix="snag-download-rebind-") as tmp:
                root = Path(tmp)
                original_root, replacement_root = root / "original", root / "replacement"
                original_root.mkdir(mode=0o700)
                replacement_root.mkdir(mode=0o700)
                original = DownloadSession(original_root)
                replacement = None
                data = b"source stays intact across terminal replacement\n"
                source = original.home / "report.bin"
                source.write_bytes(data)
                try:
                    sid = journal_paths(original.dotdir)[0].parent.name
                    original.write(b"download_tool ./report.bin\r" if tool else b"/send ./report.bin\r")
                    if tool:
                        original.read_until(b"\x1b[?9001;")
                        nonce = original.read_until(b"n")[:-1]
                        original.write(b"\x1b[>" + nonce + b"S")
                    original.read_until(b"::TRZSZ:TRANSFER:S:1.0.0:")
                    original.write(b"#ACT:")
                    time.sleep(0.05)
                    original.process.terminate()
                    original.process.wait(timeout=3)
                    replacement = Session(replacement_root,
                        command=[str(PRODUCT), "--dotdir", str(original.dotdir), "-A", sid],
                        ready=b"", cwd=original.home, env=dict(os.environ, HOME=str(original.home)))
                    seen = replacement.read_until(b"Attached session", 6)
                    seen += replacement.read_until("›".encode())
                    replacement.write(b"ping\r")
                    seen += replacement.read_until(b"pong")
                    seen += replacement.read_until("›".encode())
                    self.assertNotIn(b"::TRZSZ:TRANSFER:", seen)
                    replacement.write(b"/exit\r")
                    replacement.read_until(b"--resume")
                    self.assertEqual(replacement.process.wait(timeout=3), 0)
                    log = original.events()
                    self.assertEqual(sum(e["type"] == "download_queued" for e in log), int(tool))
                    self.assertFalse(any(e["type"] == "download_removed" for e in log))
                    self.assertEqual(sum(e["type"] == "tool_finished" for e in log), int(tool))
                    self.assertEqual(sum(e["type"] == "turn_completed" for e in log), 1 + int(tool))
                    self.assertEqual(source.read_bytes(), data)
                finally:
                    if replacement is not None:
                        replacement.close()
                    original.close()

    def test_binary_and_empty_file_and_keyboard_tail(self):
        with tempfile.TemporaryDirectory(prefix="snag-download-") as path:
            session = DownloadSession(Path(path))
            try:
                for name, data in (("résumé bytes.bin", bytes(range(256)) * 300), ("empty", b"")):
                    digest = session.download(name, data)
                    self.assertEqual(digest, hashlib.md5(data).digest())
                    session.finish(digest, b"ping\r")
                    session.read_until(b"pong", 8)
                self.assertFalse(any(e["type"] == "attachment_added" for e in session.events()))
                self.assertNotIn(b"#DATA:", b"".join(
                    p.read_bytes() for p in journal_paths(session.dotdir)))
                session.exit()
            finally:
                session.close()

    def test_model_tool_and_accepted_asset_complete_with_durable_results(self):
        with tempfile.TemporaryDirectory(prefix="snag-download-tool-") as path:
            session = DownloadSession(Path(path))
            try:
                data = b"private file bytes; never a tool result"
                digest = session.download("report.bin", data, b"download_tool ./report.bin\r")
                session.finish(digest)
                session.read_until(b" complete")
                results = [e["data"]["result"] for e in session.events() if e["type"] == "tool_finished"]
                self.assertEqual(results[-1]["status"], "succeeded")
                self.assertNotIn(data.decode(), json.dumps(results))
                session.write(b"/attach report.bin\r")
                session.read_until(b"asset:")
                identifier = session.read_until(b"\r\n").strip()[:32]
                self.assertEqual(len(identifier), 32)
                session.write(b"ping\r")
                session.read_until(b"pong", 8)
                request = b"download_tool asset:" + identifier + b"\r"
                digest = session.download("report.bin", data, request, expected_name=identifier)
                session.finish(digest)
                session.read_until(b" complete")
                self.assertEqual([e["data"]["result"]["status"] for e in session.events()
                                  if e["type"] == "tool_finished"], ["succeeded", "succeeded"])
                session.exit()
            finally:
                session.close()

    def test_model_tool_rejects_unknown_arguments_and_read_only(self):
        with tempfile.TemporaryDirectory(prefix="snag-download-refuse-") as path:
            session = DownloadSession(Path(path))
            try:
                for command in (b"download_tool_bad\r", b"/ro download_tool report.bin\r"):
                    session.write(command)
                    output = session.read_until(b" complete", 10)
                    self.assertNotIn(b"::TRZSZ:TRANSFER:", output)
                results = [e["data"]["result"] for e in session.events() if e["type"] == "tool_finished"]
                self.assertEqual([r["status"] for r in results], ["failed", "not_run"])
                self.assertIn("extra", results[0]["model_text"])
                self.assertIn("read-only", results[1]["model_text"])
                session.exit()
            finally:
                session.close()

    def test_one_shot_tool_queues_without_a_live_workstation_client(self):
        with tempfile.TemporaryDirectory(prefix="snag-download-queue-") as path:
            root = Path(path)
            home, dotdir = root / "home", root / "dotdir"
            home.mkdir(mode=0o700)
            dotdir.mkdir(mode=0o700)
            (home / "report.bin").write_bytes(b"queued bytes\n")
            env = dict(os.environ, HOME=str(home), SNAJPAGENT_DOTDIR=str(dotdir))
            first = subprocess.run([str(PRODUCT), "--dotdir", str(dotdir), "-e", "--",
                                    "download_tool ./report.bin"], cwd=home, env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
            self.assertEqual(first.returncode, 0, first.stderr)
            self.assertNotIn(b"::TRZSZ:TRANSFER:", first.stdout + first.stderr)
            journal = journal_paths(dotdir)[0]
            events = read_events(journal)
            queued = [e["data"] for e in events if e["type"] == "download_queued"]
            self.assertEqual(len(queued), 1)
            self.assertEqual((queued[0]["name"], queued[0]["bytes"]), ("report.bin", 13))
            self.assertRegex(queued[0]["id"], r"^[0-9a-f]{32}$")
            self.assertRegex(queued[0]["sha256"], r"^[0-9a-f]{64}$")
            results = [e["data"]["result"] for e in events if e["type"] == "tool_finished"]
            self.assertEqual(results[-1]["status"], "succeeded")
            self.assertIn("not delivered yet", results[-1]["model_text"])
            sid = journal.parent.name
            resumed = subprocess.run([str(PRODUCT), "--dotdir", str(dotdir), "--resume", sid,
                                      "-e", "--", "download_queue_list"], cwd=home, env=env,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
            self.assertEqual(resumed.returncode, 0, resumed.stderr)
            events = read_events(journal)
            model_text = [e["data"]["result"]["model_text"] for e in events
                          if e["type"] == "tool_finished"][-1]
            self.assertIn("1 pending workstation download(s)", model_text)
            self.assertIn(queued[0]["id"], model_text)
            self.assertIn("sha256=", model_text)

    def test_download_queue_remove_clear_and_read_only_guard(self):
        with tempfile.TemporaryDirectory(prefix="snag-download-queue-tool-") as path:
            root = Path(path)
            home, dotdir = root / "home", root / "dotdir"
            home.mkdir(mode=0o700)
            dotdir.mkdir(mode=0o700)
            env = dict(os.environ, HOME=str(home), SNAJPAGENT_DOTDIR=str(dotdir))
            for name in ("one.bin", "two.bin"):
                (home / name).write_bytes(name.encode())
            first = subprocess.run([str(PRODUCT), "--dotdir", str(dotdir), "-e", "--",
                                    "download_tool ./one.bin"], cwd=home, env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
            self.assertEqual(first.returncode, 0, first.stderr)
            journal = journal_paths(dotdir)[0]
            sid = journal.parent.name
            second = subprocess.run([str(PRODUCT), "--dotdir", str(dotdir), "--resume", sid,
                                     "-e", "--", "download_tool ./two.bin"], cwd=home, env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
            self.assertEqual(second.returncode, 0, second.stderr)
            events = read_events(journal)
            queued = [e["data"] for e in events if e["type"] == "download_queued"]
            self.assertEqual(len(queued), 2)
            for prompt in (f"download_queue_remove {queued[0]['id']}", "download_queue_list",
                           "/ro download_queue_readonly_clear", "download_queue_clear",
                           "download_queue_list"):
                run = subprocess.run([str(PRODUCT), "--dotdir", str(dotdir), "--resume", sid,
                                      "-e", "--", prompt], cwd=home, env=env,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
                self.assertEqual(run.returncode, 0, run.stderr)
            events = read_events(journal)
            results = [e["data"]["result"] for e in events if e["type"] == "tool_finished"]
            texts = [r["model_text"] for r in results]
            self.assertTrue(any("Removed one pending download" in text for text in texts))
            self.assertTrue(any("1 pending workstation download(s)" in text for text in texts))
            readonly = [r for r in results if "read-only" in r["model_text"]]
            self.assertEqual(len(readonly), 1)
            self.assertEqual(readonly[0]["status"], "failed")
            self.assertTrue(any("Cleared the pending download queue" in text for text in texts))
            self.assertEqual([e["type"] for e in events].count("download_removed"), 1)
            self.assertEqual([e["type"] for e in events].count("downloads_cleared"), 1)
            self.assertEqual((home / "one.bin").read_bytes(), b"one.bin")
            self.assertEqual((home / "two.bin").read_bytes(), b"two.bin")

    @unittest.skipUnless(shutil.which("screen"), "requires GNU screen")
    def test_screen_cancellation_keeps_display_clean_and_input_working(self):
        with tempfile.TemporaryDirectory(prefix="snag-cancel-screen-") as path:
            session = DownloadSession(Path(path), screen=True)
            try:
                (session.home / "cancel").write_bytes(b"abc")
                session.write(f"/send {session.home / 'cancel'}\r".encode())
                session.read_until(b"::TRZSZ:TRANSFER:S:")
                session.read_until(b"\r\n")
                session.write(b"\x03")
                session.read_until(b"Download cancelled;")
                session.write(b"ping\r")
                session.read_until(b"pong", 8)
                visible = session.hardcopy()
                self.assertNotIn(b"::TRZSZ:TRANSFER:", visible)
                self.assertNotIn(b"#CFG:", visible)
                session.exit()
            finally:
                session.close()

    def test_wrong_digest_is_failure_and_terminal_recovers(self):
        with tempfile.TemporaryDirectory(prefix="snag-download-digest-") as path:
            session = DownloadSession(Path(path))
            try:
                session.download("bad.bin", b"abc")
                session.write(frame("SUCC", b"x" * 16))
                session.read_frame("FAIL")
                session.read_until(b"download digest:")
                session.write(b"ping\r")
                session.read_until(b"pong", 8)
                session.exit()
            finally:
                session.close()

    def test_failed_model_transfer_retains_stable_pending_item(self):
        with tempfile.TemporaryDirectory(prefix="snag-download-pending-") as path:
            session = DownloadSession(Path(path))
            try:
                session.download("pending.bin", b"abc", request=b"download_tool ./pending.bin\r")
                session.write(frame("SUCC", b"x" * 16))
                session.read_frame("FAIL")
                session.read_until(b"download digest:")
                session.read_until(b" complete")
                events = session.events()
                queued = [event for event in events if event["type"] == "download_queued"]
                self.assertEqual(len(queued), 1)
                self.assertFalse(any(event["type"] == "download_removed" for event in events))
                item_id = queued[0]["data"]["id"]
                session.write(b"download_queue_list\r")
                session.read_until(b" complete")
                results = [event["data"]["result"] for event in session.events()
                           if event["type"] == "tool_finished"]
                self.assertIn(item_id, results[-1]["model_text"])
                self.assertEqual((session.home / "pending.bin").read_bytes(), b"abc")
                session.exit()
            finally:
                session.close()

    def test_cancel_and_ctrl_c_restore_terminal(self):
        for action in (frame("ACT", b'{"confirm":false,"protocol":4,"newline":"\\n"}'), b"\x03"):
            with self.subTest(action=action), tempfile.TemporaryDirectory(prefix="snag-download-cancel-") as path:
                session = DownloadSession(Path(path))
                try:
                    (session.home / "cancel").write_bytes(b"abc")
                    session.write(f"/send {session.home / 'cancel'}\r".encode())
                    session.read_until(b"::TRZSZ:TRANSFER:S:")
                    session.read_until(b"\r\n")
                    session.write(action)
                    session.read_until(b"Download cancelled;")
                    session.write(b"ping\r")
                    session.read_until(b"pong", 8)
                    session.exit()
                finally:
                    session.close()

    def test_invalid_sources_are_rejected_before_handshake(self):
        with tempfile.TemporaryDirectory(prefix="snag-download-invalid-") as path:
            session = DownloadSession(Path(path))
            try:
                (session.home / "real").write_bytes(b"abc")
                (session.home / "link").symlink_to("real")
                os.mkfifo(session.home / "fifo")
                for name in ("missing", ".", "link", "fifo"):
                    session.write(f"/send {name}\r".encode())
                    output = session.read_until(b"Download requires a readable regular file:")
                    self.assertNotIn(b"::TRZSZ:TRANSFER:", output)
                    session.read_until("› ".encode())
                session.write(b"/send\r")
                session.read_until(b"usage: /send PATH")
                session.exit()
            finally:
                session.close()

    @unittest.skipUnless(CLIENT, "requires the pinned real trzsz client")
    def test_plain_screen_term_is_not_a_screen_backend(self):
        with tempfile.TemporaryDirectory(prefix="snag-download-term-") as path:
            root = Path(path)
            command = ["env", "TERM=screen-256color", str(PRODUCT),
                       "--dotdir", str(root / "dotdir")]
            session = DownloadSession(root, CLIENT, command=command)
            try:
                data = os.urandom(70000)
                (session.home / "report").write_bytes(data)
                session.write(b"/send report\r")
                session.read_until(b"Download completed:", 15)
                self.assertEqual((session.downloads / "report").read_bytes(), data)
                session.exit()
            finally:
                session.close()

    @unittest.skipUnless(CLIENT and Path("/proc/version").is_file(), "requires Linux procfs and trzsz")
    def test_zero_stat_size_with_nonempty_contents_is_not_acknowledged(self):
        with tempfile.TemporaryDirectory(prefix="snag-download-size-") as path:
            session = DownloadSession(Path(path), CLIENT)
            try:
                session.write(b"/send /proc/version\r")
                output = session.read_until(b"download contents:", 10)
                self.assertNotIn(b"Download completed:", output)
                session.write(b"ping\r")
                session.read_until(b"pong", 8)
                session.exit()
            finally:
                session.close()

    @unittest.skipUnless(CLIENT and shutil.which("screen"), "requires real wrapper and GNU screen")
    def test_screen_display_has_no_transfer_frames_after_download(self):
        for mode in ("off", "on"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(prefix="snag-download-screen-") as path:
                session = DownloadSession(Path(path), CLIENT, screen=mode)
                try:
                    data = os.urandom(90000)
                    (session.home / "report").write_bytes(data)
                    session.write(b"/send report\r")
                    session.read_until(b"Download completed:", 15)
                    self.assertEqual((session.downloads / "report").read_bytes(), data)
                    session.write(b"\x01d")
                    self.assertEqual(session.process.wait(timeout=5), 0)
                    attach_root = Path(path) / "reattach"
                    attach_root.mkdir()
                    again = Session(attach_root, CLIENT, command=["screen", "-r", session.screen_name],
                                    ready="› ".encode(), cwd=session.home, env=session.screen_env)
                    visible = session.hardcopy()
                    again.write(b"/exit\r")
                    self.assertEqual(again.process.wait(timeout=5), 0)
                    again.close()
                    self.assertNotIn(b"#CFG:", visible)
                    self.assertNotIn(b"#DATA:", visible)
                    self.assertNotIn(b"::TRZSZ:TRANSFER:", visible)
                finally:
                    session.close()

    @unittest.skipUnless(CLIENT and shutil.which("screen"), "requires real wrapper and GNU screen")
    def test_screen_display_has_no_transfer_frames_after_upload(self):
        for mode in ("off", "on"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(prefix="snag-upload-screen-") as path:
                session = DownloadSession(Path(path), CLIENT, screen=mode)
                try:
                    upload = session.home / "upload.bin"
                    upload.write_bytes(os.urandom(90000))
                    session.write(shlex.quote(str(upload)).encode())
                    session.read_until(b"1 unsent attachment(s)", 15)
                    session.write(b"\x01d")
                    self.assertEqual(session.process.wait(timeout=5), 0)
                    attach_root = Path(path) / "reattach"
                    attach_root.mkdir()
                    again = Session(attach_root, CLIENT, command=["screen", "-r", session.screen_name],
                                    ready="› ".encode(), cwd=session.home, env=session.screen_env)
                    visible = session.hardcopy()
                    again.write(b"/exit\r")
                    self.assertEqual(again.process.wait(timeout=5), 0)
                    again.close()
                    self.assertNotIn(b"#CFG:", visible)
                    self.assertNotIn(b"#SUCC:", visible)
                    self.assertNotIn(b"::TRZSZ:TRANSFER:", visible)
                finally:
                    session.close()

    @unittest.skipUnless(CLIENT, "set TRZSZ_CLIENT for pinned real-wrapper interoperability")
    def test_real_wrapper_binary_empty_and_existing_destination(self):
        with tempfile.TemporaryDirectory(prefix="snag-download-real-") as path:
            session = DownloadSession(Path(path), CLIENT)
            try:
                for name, data in (("binary.bin", bytes(range(256)) * 300), ("empty", b"")):
                    (session.home / name).write_bytes(data)
                    session.write(f"/send {name}\r".encode())
                    session.read_until(b"Download completed:", 15)
                    self.assertEqual((session.downloads / name).read_bytes(), data)
                session.write(b"/send binary.bin\r")
                session.read_until(b"Download completed:", 15)
                files = list(session.downloads.iterdir())
                self.assertEqual(len(files), 3)
                self.assertEqual(sum(p.read_bytes() == bytes(range(256)) * 300 for p in files), 2)
                session.exit()
            finally:
                session.close()


if __name__ == "__main__":
    unittest.main()
