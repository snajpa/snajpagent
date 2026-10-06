# SPDX-License-Identifier: GPL-2.0-only
"""Command reports remain readable in short terminals through the configured pager."""

import fcntl
import json
import os
import select
import shlex
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest
from pathlib import Path

from irc_client import IRCClient
from test_upload_client import PRODUCT, Session


class CommandPagerTests(unittest.TestCase):
    def setUp(self):
        # Keep native socket paths within Darwin's sockaddr_un length.
        self.tmp = tempfile.TemporaryDirectory(prefix="snag-command-pager-",
                                               dir="/private/tmp" if sys.platform == "darwin" else None)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        self.capture = self.root / "report.txt"
        self.calls = self.root / "calls.txt"
        self.pager = self.root / "pager with spaces.sh"
        self.pager.write_text(
            '#!/bin/sh\ncat "$1" > "$REPORT_CAPTURE"\n'
            'printf "call\\n" >> "$REPORT_CALLS"\n'
            'printf "REPORT_READY\\n"\nread -r answer < /dev/tty\n'
        )
        self.pager.chmod(0o700)
        self.pager_open = False

    def start(self, pager=None, configured=None, environment=None, arguments=(), config="",
              ready="›".encode()):
        self.dotdir = self.root / "dotdir"
        self.home = self.root / "home"
        self.dotdir.mkdir(mode=0o700)
        self.home.mkdir(mode=0o700)
        if configured is not None or config:
            (self.dotdir / "config.ini").write_text(
                "[provider openai]\n" + config +
                ("[ui]\npager = " + configured + "\n" if configured is not None else ""))
        self.env = dict(os.environ, HOME=str(self.home), TERM="xterm-256color",
                        SNAJPAGENT_DOTDIR=str(self.dotdir))
        self.env.update({
            "PAGER": shlex.quote(str(self.pager)) if pager is None else pager,
            "REPORT_CAPTURE": str(self.capture), "REPORT_CALLS": str(self.calls),
        })
        self.env.update(environment or {})
        self.env = {key: value for key, value in self.env.items() if value is not None}
        self.ready = ready
        child = Session(self.root,
                        command=[str(PRODUCT), "--dotdir", str(self.dotdir), *arguments],
                        ready=ready, cwd=self.home, env=self.env)
        self.addCleanup(self.stop, child)
        fcntl.ioctl(child.master, termios.TIOCSWINSZ,
                    struct.pack("HHHH", 6, 60, 0, 0))
        return child

    def stop(self, child):
        try:
            if child.process.poll() is None:
                if self.pager_open:
                    child.write(b"q\n")
                    child.read_until(self.ready)
                child.write(b"\x03" * 5)
                child.process.wait(timeout=5)
        finally:
            self.stop_owner(self.dotdir)
            child.close()

    @staticmethod
    def stop_owner(dotdir):
        # Native owners outlive terminal loss. Match this fixture's unique store
        # as well as its binary before stopping an owner left by a failed check.
        prefix = f"{PRODUCT} --dotdir {dotdir}"
        for row in subprocess.check_output(["ps", "-axo", "pid=,args="], text=True).splitlines():
            pid, _, command = row.strip().partition(" ")
            if command.lstrip() == prefix or command.lstrip().startswith(prefix + " "):
                try:
                    os.kill(int(pid), signal.SIGKILL)
                except ProcessLookupError:
                    pass

    def report(self, child, command, *expected):
        before = len(self.calls.read_text().splitlines()) if self.calls.exists() else 0
        child.write(command.encode() + b"\r")
        output = child.read_until(b"REPORT_READY", seconds=3)
        self.pager_open = True
        text = self.capture.read_text()
        self.assertNotIn("\x1b", text)
        for value in expected:
            self.assertIn(value, text)
        self.assertEqual(len(self.calls.read_text().splitlines()), before + 1)
        child.write(b"q\n")
        self.pager_open = False
        child.read_until("›".encode())
        return text, output

    def test_status_uses_pager_in_six_line_terminal(self):
        child = self.start()
        text, output = self.report(child, "/status", "session:", "provider:", "context:")
        self.assertGreater(len(text.splitlines()), 6)
        self.assertNotIn(b"provider:", output)
        child.write(b"ping\r")
        child.read_until(b"pong")
        child.read_until("›".encode())
        child.write(b"/exit\r")
        child.read_until(b"--resume")

    def test_report_commands_and_aliases_share_the_pager(self):
        child = self.start()
        child.write(b"/model cache\r")
        child.read_until(b"cache updated:")
        child.read_until("›".encode())
        reports = (
            ("/help", "Help and settings", "/status"),
            ("/?", "Keyboard", "/send PATH"),
            ("/state", "session:", "no goal has been set", "goal actions:"),
            ("/state goal", "no goal has been set", "goal actions:"),
            ("/goal", "no goal has been set", "goal actions:"),
            ("/state goal status", "no goal has been set"),
            ("/goal help", "/state goal [set] TEXT"),
            ("/queue", "future-turn queue is empty"),
            ("/q", "future-turn queue is empty"),
            ("/session", "current session:", "running sessions:"),
            ("/s l", "current session:", "saved sessions:"),
            ("/history 0", "0 shown", "0 total"),
            ("/attachments", "0 unsent attachment(s)"),
            ("/context", "context for", "reserve="),
            ("/effort", "effort for next turn:"),
            ("/banner", "banner: none"),
            ("/nick", "model nick:", "operator nick:"),
            ("/steering", "steering for next turn:"),
            ("/server", "hosting is off"),
            ("/names", "selected destination:", "no active endpoints"),
            ("/topic", "selected destination:", "no active endpoints"),
            ("/verbose", "verbosity: 0"),
            ("/verbose  ", "verbosity: 0"),
            ("/model", "selected:", "cache updated:"),
            ("/model list", "selected:", "cache updated:"),
        )
        for command, *expected in reports:
            with self.subTest(command=command):
                self.report(child, command, *expected)

    def test_history_queue_and_session_rows_are_complete(self):
        child = self.start()
        child.write(b"ping\r")
        child.read_until(b"pong")
        child.read_until("›".encode())
        self.report(child, "/history 1", "user: ping", "assistant: pong", "1 shown")
        self.report(child, "/session list", "SESSION", "TURNS", "attached")
        for index in range(12):
            child.write(f"/queue queued item {index:02d} café 漢字\r".encode())
            child.read_until(b"queued (/next or /q c)")
            child.read_until("›".encode())
        text, _ = self.report(child, "/q", "queued item 00 café 漢字", "queued item 11 café 漢字")
        self.assertEqual(text.count("queued item"), 12)
        self.assertGreater(len(text.splitlines()), 6)
        journal = next((self.dotdir / "sessions").glob("*/events.jsonl"))
        events = [json.loads(line) for line in journal.read_text().splitlines()]
        self.assertEqual(sum(e["type"] == "turn_started" for e in events), 1)

    def test_active_turn_survives_report_and_pager_exit(self):
        child = self.start()
        child.write(b"slow\r")
        child.read_until(b"working slowly")
        child.write(b"/status\r")
        child.read_until(b"REPORT_READY")
        self.pager_open = True
        self.assertIn("state: active", self.capture.read_text())
        child.write(b"q\n")
        self.pager_open = False
        child.read_until(b" complete", seconds=12)
        child.read_until("›".encode())
        self.report(child, "/history", "user: slow", "slow complete", "1 completed")

    @staticmethod
    def available_output(child):
        output = child.pending
        child.pending = b""
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline and select.select([child.master], [], [], 0.15)[0]:
            output += os.read(child.master, 65536)
        return output

    def test_held_pager_retains_irc_until_terminal_returns(self):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        child = self.start(arguments=("-s", f"127.0.0.1:{port}", "-r", "lab",
                                      "-n", "pagerbot", "-o", "pagerop"),
                           ready=b"pagerop@")
        peer = IRCClient(port, "pagerpeer")
        self.addCleanup(peer.close)
        peer.message("pager-before-hold")
        child.read_until(b"pager-before-hold")
        child.write(b"/status\r")
        child.read_until(b"REPORT_READY")
        self.pager_open = True

        # Cross the journal's automatic checkpoint boundary while display is held.
        messages = [f"pager-held-{index:03d}" for index in range(160)]
        for message in messages:
            peer.message(message)
        # Hosted broadcasts follow the engine's durable event/UI acceptance.
        # Receiving the last echo establishes that all these events arrived.
        held_output = child.pending
        child.pending = b""
        deadline = time.monotonic() + 5
        while messages[-1].encode() not in peer.buf:
            remaining = deadline - time.monotonic()
            self.assertGreater(remaining, 0, "IRC stopped while the pager was open")
            ready, _, _ = select.select([peer.sock, child.master], [], [], remaining)
            if peer.sock in ready:
                peer.buf.extend(peer.sock.recv(65536))
            if child.master in ready:
                held_output += os.read(child.master, 65536)
        journal = next((self.dotdir / "sessions").glob("*/events.jsonl"))
        records = journal.read_text().rsplit("\n", 1)[0].splitlines()
        events = [json.loads(line) for line in records]
        self.assertTrue(any(event["type"] == "session_checkpoint" for event in events))
        retained = [event["data"]["text"] for event in events
                    if event["type"] == "irc_event" and
                    event["data"]["text"] in messages]
        self.assertEqual(retained, messages)
        held_output += self.available_output(child)
        child.write(b"q\n")
        self.pager_open = False
        resumed = child.read_until(self.ready)
        resumed += self.available_output(child)
        self.assertNotIn(messages[0].encode(), held_output,
                         "session output painted while the pager owned the terminal")
        for message in messages:
            self.assertEqual(resumed.count(message.encode()), 1, message)

    def test_provider_finishes_while_pager_remains_open(self):
        child = self.start()
        child.write(b"slow\r")
        child.read_until(b"working slowly")
        child.write(b"/status\r")
        child.read_until(b"REPORT_READY")
        self.pager_open = True
        journal = next((self.dotdir / "sessions").glob("*/events.jsonl"))
        deadline = time.monotonic() + 6
        completed = False
        while time.monotonic() < deadline:
            records = journal.read_text().rsplit("\n", 1)[0].splitlines()
            completed = any(json.loads(line)["type"] == "turn_completed"
                            for line in records)
            if completed:
                break
            time.sleep(0.025)
        held_output = self.available_output(child)
        child.write(b"q\n")
        self.pager_open = False
        resumed = child.read_until(self.ready)
        self.assertTrue(completed, "provider progress stopped for the pager's lifetime")
        self.assertNotIn(b"slow complete", held_output)
        self.assertIn(b"slow complete", resumed)

    def complete_under_pager(self, child, prompt, event):
        child.write(prompt.encode() + b"\r")
        child.read_until(b"working slowly")
        child.write(b"/status\r")
        child.read_until(b"REPORT_READY")
        self.pager_open = True
        journal = next((self.dotdir / "sessions").glob("*/events.jsonl"))
        deadline = time.monotonic() + 7
        held = b""
        records = []
        while time.monotonic() < deadline:
            held += self.available_output(child)
            records = [json.loads(line) for line in
                       journal.read_text().rsplit("\n", 1)[0].splitlines()]
            if any(row["type"] == event for row in records):
                break
        held += self.available_output(child)
        child.write(b"q\n")
        self.pager_open = False
        resumed = child.read_until(self.ready)
        self.assertTrue(any(row["type"] == event for row in records),
                        f"{event} blocked by the open pager")
        return held, resumed, records

    def test_provider_failure_is_retained_until_pager_exits(self):
        child = self.start(config="[agent]\nmax_turn_retries=0\n")
        held, resumed, _ = self.complete_under_pager(child, "slow_failure", "turn_failed")
        self.assertNotIn(b"fixture delayed provider failure", held)
        self.assertIn(b"fixture delayed provider failure", resumed)
        child.write(b"ping\r")
        child.read_until(b"pong")

    def test_nonzero_pager_exit_preserves_completed_output(self):
        self.pager.write_text(self.pager.read_text() + "exit 7\n")
        child = self.start()
        held, resumed, records = self.complete_under_pager(child, "slow", "turn_completed")
        resumed += self.available_output(child)
        self.assertNotIn(b"slow complete", held)
        self.assertEqual(resumed.count(b"slow complete"), 1)
        self.assertEqual(sum(row["type"] == "turn_completed" for row in records), 1)
        child.write(b"ping\r")
        child.read_until(b"pong")
        child.read_until(self.ready)

    def test_tool_finishes_and_retains_output_under_pager(self):
        child = self.start()
        child.write(b"/verbose 2\r")
        child.read_until(b"verbosity: 2")
        child.read_until(self.ready)
        held, resumed, records = self.complete_under_pager(child, "slow_tool", "turn_completed")
        self.assertTrue(any(row["type"] == "tool_finished" for row in records))
        self.assertNotIn(b"fixture command succeeded", held)
        self.assertIn(b"fixture command succeeded", resumed)
        self.assertIn(b"slow complete", resumed)

    def test_model_download_stays_queued_while_pager_owns_input(self):
        child = self.start()
        (self.home / "report.bin").write_bytes(b"pager transfer regression\n")
        held, resumed, records = self.complete_under_pager(
            child, "slow_download report.bin", "turn_completed")
        self.assertEqual(sum(row["type"] == "download_queued" for row in records), 1)
        self.assertFalse(any(row["type"] == "download_removed" for row in records))
        self.assertNotIn(b"\x1b[?9001;", held)
        self.assertNotIn(b"slow complete", held)
        self.assertIn(b"slow complete", resumed)

    def test_interrupting_pager_restores_composer(self):
        child = self.start()
        child.write(b"/status\r")
        child.read_until(b"REPORT_READY")
        self.pager_open = True
        child.write(b"\x03")
        child.read_until(self.ready)
        self.pager_open = False
        child.write(b"ping\r")
        child.read_until(b"pong")
        child.read_until(self.ready)

    def test_disabled_pager_retains_direct_output(self):
        child = self.start(configured="off")
        child.write(b"/status\r")
        child.read_until(b"provider:")
        child.read_until("›".encode())
        child.write(b"/history 0\r")
        child.read_until(b"0 shown")
        child.read_until("›".encode())
        self.assertFalse(self.calls.exists())

    def test_empty_pager_retains_direct_output(self):
        child = self.start(pager="")
        child.write(b"/state\r")
        child.read_until(b"goal actions:")
        child.read_until("›".encode())
        self.assertFalse(self.calls.exists())

    def less_fixture(self):
        directory = self.root / "bin"
        directory.mkdir()
        self.arguments = self.root / "arguments.txt"
        less = directory / "less"
        less.write_text(
            '#!/bin/sh\nprintf "%s\\n" "$@" > "$REPORT_ARGS"\n'
            'for file do :; done\n'
            + shlex.quote(shutil.which("cat")) + ' "$file" > "$REPORT_CAPTURE"\n'
            'printf "call\\n" >> "$REPORT_CALLS"\n'
            'printf "REPORT_READY\\n"\nread -r answer < /dev/tty\n'
        )
        less.chmod(0o700)
        return {"PATH": str(directory), "REPORT_ARGS": str(self.arguments)}

    def test_unset_pager_defaults_to_less_X_for_reports_and_files(self):
        environment = self.less_fixture()
        environment["PAGER"] = None
        child = self.start(environment=environment)
        self.report(child, "/status", "session:", "provider:")
        self.assertEqual(self.arguments.read_text().splitlines()[:-1], ["-X"])
        local_file = self.home / "notes.txt"
        local_file.write_text("local file in default pager\n")
        self.report(child, "/cat notes.txt", "local file in default pager")
        self.assertEqual(self.arguments.read_text().splitlines(), ["-X", str(local_file)])

    def test_bare_less_gets_X(self):
        child = self.start(pager="less", environment=self.less_fixture())
        self.report(child, "/status", "session:")
        self.assertEqual(self.arguments.read_text().splitlines()[:-1], ["-X"])

    def test_explicit_less_arguments_are_preserved(self):
        child = self.start(pager="less -R", environment=self.less_fixture())
        self.report(child, "/status", "session:")
        self.assertEqual(self.arguments.read_text().splitlines()[:-1], ["-R"])

    def test_unset_pager_without_less_retains_direct_output(self):
        directory = self.root / "empty-bin"
        directory.mkdir()
        child = self.start(environment={"PAGER": None, "PATH": str(directory)})
        child.write(b"/state\r")
        child.read_until(b"goal actions:")
        child.read_until("›".encode())
        self.assertFalse(self.calls.exists())

    def test_noninteractive_cli_does_not_launch_pager(self):
        env = dict(os.environ, HOME=str(self.root), PAGER=shlex.quote(str(self.pager)),
                   REPORT_CAPTURE=str(self.capture), REPORT_CALLS=str(self.calls))
        result = subprocess.run([str(PRODUCT), "--dotdir", str(self.root / "plain"), "-l"],
                                stdin=subprocess.DEVNULL, cwd=self.root, env=env,
                                capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.calls.exists())

    def test_missing_pager_falls_back_to_complete_report(self):
        child = self.start(pager=str(self.root / "absent-pager"))
        child.write(b"/state\r")
        text = child.read_until(b"goal actions:")
        self.assertIn(b"session:", text)
        self.assertIn(b"no goal has been set", text)
        child.read_until("›".encode())

    def test_configured_template_and_setting_controls(self):
        child = self.start(pager="false", configured=shlex.quote(str(self.pager)) + " %s")
        self.report(child, "/status", "session:")
        child.write(b"/verbose 2\r")
        child.read_until(b"verbosity: 2")
        child.read_until("›".encode())
        self.assertEqual(len(self.calls.read_text().splitlines()), 1)
        self.report(child, "/verbose", "verbosity: 2")
        child.write(b"/banner retained banner\r")
        child.read_until(b"banner updated")
        child.read_until("›".encode())
        self.report(child, "/banner", "retained banner")

    @unittest.skipUnless(shutil.which("less"), "less unavailable")
    def test_real_less_reaches_both_ends_after_resize(self):
        child = self.start(environment={"PAGER": None})
        child.write(b"/status\r")
        child.read_until(b"session:")
        child.write(b"G")
        child.read_until(b"no active endpoints")
        fcntl.ioctl(child.master, termios.TIOCSWINSZ, struct.pack("HHHH", 4, 40, 0, 0))
        child.write(b"g")
        child.read_until(b"session:")
        child.write(b"q")
        child.read_until("›".encode())
        child.write(b"/exit\r")
        child.read_until(b"--resume")

    @unittest.skipUnless(all(shutil.which(name) for name in ("mosh", "mosh-server", "less", "screen")),
                         "stock Mosh, less or screen unavailable")
    def test_stock_mosh_less_in_six_line_terminal(self):
        from test_remote_terminal import RemoteProcess, screen_snapshot

        home = self.root / "remote"
        home.mkdir()
        sockets = self.root / "screens"
        sockets.mkdir(mode=0o700)
        config = self.root / "screenrc"
        config.write_text("startup_message off\ndefscrollback 0\naltscreen on\n")
        dotdir = self.root / "agent"
        env = dict(os.environ, SCREENDIR=str(sockets))
        child = RemoteProcess(home, ["screen", "-U", "-c", str(config), "-S", "pager-test",
                              str(PRODUCT), "remote", "mosh", "--local", "--no-init",
                              "--predict=never", "127.0.0.1", str(PRODUCT), "--dotdir", str(dotdir)],
                              wrapped=None, winsize=(6, 60),
                              extra_env={"PAGER": "less -X", "SCREENDIR": str(sockets)})

        def visible(marker):
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if select.select([child.master], [], [], 0.05)[0]:
                    child.output.extend(os.read(child.master, 65536))
                snapshot = screen_snapshot(env, "pager-test", self.root / "visible.txt")
                if marker in snapshot:
                    return snapshot
            self.fail(f"missing {marker!r} in visible Mosh screen: {snapshot!r}")

        try:
            visible(b"openai/gpt-5.5")
            os.write(child.master, b"/status\r")
            visible(b"session:")
            os.write(child.master, b"G")
            visible(b"no active endpoints")
            os.write(child.master, b"g")
            visible(b"session:")
            os.write(child.master, b"q")
            visible(b"openai/gpt-5.5")
            os.write(child.master, b"/exit\r")
            child.until(b"[screen is terminating]", 10)
        finally:
            self.stop_owner(dotdir)
            subprocess.run(["screen", "-S", "pager-test", "-X", "quit"], env=env,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
            if child.master is not None:
                os.close(child.master)
                child.master = None
            child.close()


if __name__ == "__main__":
    unittest.main()
