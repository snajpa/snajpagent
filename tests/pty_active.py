#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import errno
import fcntl
import hashlib
import json
import os
import pty
import re
import select
import shlex
import signal
import socket
import struct
import subprocess
import sys
import termios
import time
from datetime import datetime, timezone
from pathlib import Path

BINARY = os.path.abspath(sys.argv[1])
WORKSPACE = os.path.abspath(sys.argv[2])
DOTDIR = os.environ["SNAJPAGENT_DOTDIR"]
STATE_ROOT = Path(DOTDIR) / "sessions"
LIVE_GAP = rb"(?:\r{1,2}\n){1,2}(?:[^\n]*?\r\x1b\[2K(?:\x1b\[1A\r\x1b\[2K)*)?\r\x1b\[[12]A(?:\x1b\[\d+C)?"
PROMPT = "› ".encode()
DEFAULT_MODEL = "gpt-5.5-2026-04-23"
DEFAULT_IDLE_PROMPT = f" openai/{DEFAULT_MODEL}/medium   0% › ".encode()
DEFAULT_ACCOUNTED_IDLE_PROMPT = f" openai/{DEFAULT_MODEL}/medium   ?% › ".encode()
DEFAULT_ACTIVE_PROMPT = f" openai/{DEFAULT_MODEL}/medium   ?% » ".encode()
GOAL_SET = "• Goal set".encode()
GOAL_UPDATED = "• Goal updated".encode()
GOAL_CLEARED = "• Goal cleared".encode()
COMPACTED = "• Compacted".encode()
RESUME_HEADER = \
    "• You can resume this session with the following command:".encode()


def write_config(name, text):
    config = Path(os.environ["SNAJPAGENT_TEST_ROOT"]) / "config" / name
    config.write_text(text, encoding="utf-8")
    return config


def chat_prompt(operator):
    return f"{operator}@{socket.gethostname()} : ".encode()


class Child:
    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.kill()

    def __init__(self, args, ready=None, *, term=None, cols=None, env=None):
        self.sessions_before = session_ids()
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.chdir(WORKSPACE)
            env = dict(os.environ if env is None else env)
            if term is not None:
                env["TERM"] = term
            if cols is not None:
                fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", 24, cols, 0, 0))
            os.execve(BINARY, [BINARY, "--dotdir", DOTDIR, *args], env)
        self.buf = bytearray()
        if ready is not None:
            try:
                self.wait(ready)
            except BaseException:
                self.kill()
                raise

    @classmethod
    def from_command(cls, command, *, env=None):
        child = cls.__new__(cls)
        child.sessions_before = session_ids()
        child.pid, child.fd = pty.fork()
        if child.pid == 0:
            os.chdir(WORKSPACE)
            os.execle("/bin/sh", "sh", "-c", "exec " + command,
                      os.environ if env is None else env)
        child.buf = bytearray()
        return child

    def session_id(self):
        return new_session(self.sessions_before)

    def read_once(self, timeout):
        ready, _, _ = select.select([self.fd], [], [], timeout)
        if not ready:
            return False
        try:
            chunk = os.read(self.fd, 65536)
        except OSError as exc:
            if exc.errno == errno.EIO:
                return False
            raise
        if chunk:
            self.buf.extend(chunk)
            return True
        return False

    def wait(self, needle, start=0, timeout=8.0):
        # Active/idle changes repaint only the changed label span. Full cell
        # layout and unchanged margins are covered by the renderer/tmux tests.
        if needle in (DEFAULT_IDLE_PROMPT, DEFAULT_ACCOUNTED_IDLE_PROMPT, PROMPT):
            return self.wait_idle_prompt(start, timeout)
        return self.wait_text(needle, start, timeout)

    def wait_text(self, needle, start=0, timeout=8.0):
        # Live prose can park/resume between fragments; match that exact
        # reversible detour, not arbitrary escapes, and return a raw offset.
        gap = b"(?:" + LIVE_GAP + b")*"
        pattern = re.compile(gap.join(re.escape(bytes([c])) for c in needle))
        return self.wait_pattern(pattern, start, timeout)

    def wait_pattern(self, pattern, start=0, timeout=8.0):
        end = time.monotonic() + timeout
        while True:
            match = pattern.search(self.buf, start)
            if match is not None:
                return match.end()
            remaining = end - time.monotonic()
            if remaining <= 0 or not self.read_once(remaining):
                raise AssertionError(
                    f"timeout waiting for {pattern.pattern!r}; got {bytes(self.buf)!r}"
                )

    def wait_idle_prompt(self, start=0, timeout=8.0):
        # At a narrow width only the idle marker's row may change. Require
        # either the full prompt or a cursor-positioned idle-marker repaint.
        pattern = re.compile(
            re.escape(DEFAULT_IDLE_PROMPT.rstrip()) + b"|" +
            re.escape(DEFAULT_ACCOUNTED_IDLE_PROMPT.rstrip()) +
            rb"|(?:^|[\r\n])[^\r\n]*/[^\r\n]* \xe2\x80\xba"
            rb"|\r(?:\x1b\[\d+C)?(?:[0-9? ]{0,3}% )?\xe2\x80\xba(?=\r)")
        return self.wait_pattern(pattern, start, timeout)

    def send(self, data):
        os.write(self.fd, data)

    def send_wait(self, data, needle, start=0, timeout=8.0):
        self.send(data)
        return self.wait(needle, start=start, timeout=timeout)

    def send_wait_idle(self, data, needle, start=0):
        end = self.send_wait(data, needle, start=start)
        self.wait_idle_prompt(start=end)
        return end

    def drain(self, duration=0.25):
        end = time.monotonic() + duration
        while time.monotonic() < end:
            self.read_once(max(0.0, min(0.05, end - time.monotonic())))

    def exit_cleanly(self, after):
        self.wait_idle_prompt(start=after, timeout=8.0)
        self.exit_now()

    def exit_now(self, expect_resume=True):
        self.send(b"/exit\r")
        return self.finish(expect_resume=expect_resume)

    def reap(self):
        deadline = time.monotonic() + 8.0
        while True:
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                break
            if time.monotonic() >= deadline:
                raise AssertionError(f"process did not exit: {bytes(self.buf)!r}")
            self.read_once(0.05)
        self.pid = None
        while self.read_once(0.05):
            pass
        os.close(self.fd)
        return os.waitstatus_to_exitcode(status)

    def finish(self, expected=0, expect_resume=True):
        code = self.reap()
        if code != expected:
            raise AssertionError(
                f"exit status {code}, expected {expected}; "
                f"got {bytes(self.buf)!r}"
            )
        commands = []
        lines = bytes(self.buf).splitlines()
        for index, line in enumerate(lines):
            marker = line.find(RESUME_HEADER)
            if marker >= 0:
                suffix = line[marker + len(RESUME_HEADER):]
                if suffix not in (b"", b"\x1b[0m") or index + 1 >= len(lines):
                    raise AssertionError(
                        f"resume header is not on its own line; "
                        f"output={bytes(self.buf)!r}"
                    )
                command = lines[index + 1]
                if (not command or command[:1].isspace() or
                        b"\x1b" in command):
                    raise AssertionError(
                        f"resume command is not uncolored at column zero; "
                        f"output={bytes(self.buf)!r}"
                    )
                commands.append(command.decode("ascii"))
        if expect_resume:
            if len(commands) != 1:
                raise AssertionError(
                    f"expected one resume command, got {commands!r}; "
                    f"output={bytes(self.buf)!r}"
                )
            return commands[0]
        if commands:
            raise AssertionError(
                f"unexpected resume command {commands!r}; "
                f"output={bytes(self.buf)!r}"
            )
        return None

    def kill(self):
        if self.pid is None:
            return
        os.kill(self.pid, signal.SIGKILL)
        os.waitpid(self.pid, 0)
        self.pid = None
        os.close(self.fd)


def test_resize_and_suspend_preserve_draft():
    for suspend in (False, True):
        text = b"suspend draft" if suspend else b"resize draft"
        with Child(["-vvvv"], ready=DEFAULT_IDLE_PROMPT) as child:
            typed_end = child.send_wait(text, text)
            if suspend:
                child.send(b"\x1a")
                deadline = time.monotonic() + 8.0
                while True:
                    got, status = os.waitpid(child.pid, os.WUNTRACED | os.WNOHANG)
                    if got:
                        assert os.WIFSTOPPED(status), (status, bytes(child.buf))
                        break
                    assert time.monotonic() < deadline, bytes(child.buf)
                    child.read_once(0.05)
                start = len(child.buf)
                os.kill(child.pid, signal.SIGCONT)
                child.wait(DEFAULT_IDLE_PROMPT, start=start)
                child.wait(text, start=start)
            else:
                start = len(child.buf)
                fcntl.ioctl(child.fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 40, 0, 0))
                os.kill(child.pid, signal.SIGWINCH)
                child.wait(DEFAULT_IDLE_PROMPT, start=start)
            end = child.send_wait(b"\r", b"fixture answer", start=typed_end)
            end = child.wait(b"turn_completed synced", start=end)
            idle = child.wait(DEFAULT_ACCOUNTED_IDLE_PROMPT, start=end)
            if not suspend:
                start = len(child.buf)
                fcntl.ioctl(child.fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 10, 0, 0))
                os.kill(child.pid, signal.SIGWINCH)
                child.wait(DEFAULT_ACCOUNTED_IDLE_PROMPT, start=start)
            child.exit_now()
            if not suspend:
                assert b"\x1b[?2004l" in child.buf[idle:], bytes(child.buf)
        turn = one(events(child.session_id()), "turn_started")
        assert turn["data"]["text"] == text.decode(), turn


class IRCClient:
    def __init__(self, port, nick, agent=False):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=4.0)
        self.buf = bytearray()
        role = b" snajpagent/agent" if agent else b""
        registration = (
            b"CAP LS 302\r\nCAP REQ :batch server-time draft/chathistory" +
            role + b"\r\nCAP END\r\nNICK " + nick.encode() +
            b"\r\nUSER " + nick.encode() + b" 0 * :PTY peer\r\nJOIN #lab\r\n"
        )
        self.sock.sendall(registration)
        self.sock.setblocking(False)
        self.wait(b" 366 " + nick.encode() + b" #lab ")

    def wait(self, needle, start=0, timeout=8.0):
        end = time.monotonic() + timeout
        while needle not in self.buf[start:]:
            remaining = end - time.monotonic()
            if remaining <= 0:
                raise AssertionError(
                    f"timeout waiting for IRC {needle!r}; got {bytes(self.buf)!r}"
                )
            ready, _, _ = select.select([self.sock], [], [], remaining)
            if not ready:
                continue
            try:
                chunk = self.sock.recv(65536)
            except BlockingIOError:
                continue
            if not chunk:
                raise AssertionError(
                    f"IRC socket closed waiting for {needle!r}; "
                    f"got {bytes(self.buf)!r}"
                )
            self.buf.extend(chunk)
        return self.buf.find(needle, start) + len(needle)

    def message(self, text):
        self.sock.sendall(b"PRIVMSG #lab :" + text.encode() + b"\r\n")

    def drain(self, duration=0.25):
        end = time.monotonic() + duration
        while time.monotonic() < end:
            ready, _, _ = select.select(
                [self.sock], [], [], min(0.05, end - time.monotonic())
            )
            if not ready:
                continue
            try:
                chunk = self.sock.recv(65536)
            except BlockingIOError:
                continue
            if not chunk:
                return
            self.buf.extend(chunk)

    def close(self):
        self.sock.close()


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def accept_connections(listener, count):
    accepted = []
    deadline = time.monotonic() + 8.0
    while len(accepted) < count:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise AssertionError(
                f"expected {count} outgoing connections, got {len(accepted)}"
            )
        ready, _, _ = select.select([listener], [], [], remaining)
        if ready:
            connection, _ = listener.accept()
            accepted.append(connection)
    return accepted


def command_arguments(command):
    arguments = shlex.split(command)
    assert arguments[0] == BINARY, arguments
    return arguments


def session_ids():
    if not STATE_ROOT.exists():
        return set()
    return {entry.name for entry in STATE_ROOT.iterdir() if entry.is_dir()}


def new_session(before, child=None):
    deadline = time.monotonic() + 4.0
    while True:
        created = session_ids() - before
        if child is None or (created and all(
                (STATE_ROOT / sid / "events.jsonl").is_file() for sid in created)):
            break
        assert time.monotonic() < deadline, bytes(child.buf)
        child.read_once(0.01)
    if len(created) != 1:
        raise AssertionError(f"expected one new session, got {sorted(created)!r}")
    return created.pop()


def events(session_id):
    path = STATE_ROOT / session_id / "events.jsonl"
    # A live writer may expose a partial final JSON/UTF-8 record.
    records = [json.loads(line) for line in path.read_bytes().split(b"\n")[:-1]]
    # Expose logical scheduler input to the existing behavior assertions.
    # Production reconnect tests independently verify raw journal IDs/payloads.
    payloads = {}
    for event in records:
        data = event["data"]
        if event["type"] == "irc_event" and data.get("stream"):
            identity = f"{data['stream']}:{data['sequence']}"
            payloads[identity] = (f"[IRC endpoint={data['endpoint']} room={data['room']} "
                f"event={data['kind']} sender={data['nick']} operator={str(data['op']).lower()}]\n{data['text']}\n")
        elif event["type"] in ("turn_started", "steering_added"):
            data["text"] = re.sub(r"\[IRC update id=([^ ]+)[^\n]*\n",
                lambda m: payloads.get(m[1], m[0]), data["text"])
    return records


def turn_events(items, event_type, turn_id):
    return [item for item in items if item["type"] == event_type and
            item["data"]["turn_id"] == turn_id]


def one(items, event_type):
    matches = [item for item in items if item["type"] == event_type]
    if len(matches) != 1:
        raise AssertionError(f"expected one {event_type}, got {len(matches)}")
    return matches[0]


def wait_turn_completed(child, session_id, needle, timeout=8.0):
    deadline = time.monotonic() + timeout
    while True:
        try:
            log = events(session_id)
        except (FileNotFoundError, json.JSONDecodeError):
            log = []
        turn_ids = [
            event["data"]["turn_id"] for event in log
            if event["type"] == "turn_started" and
            needle in event["data"]["text"]
        ]
        if any(event["type"] == "turn_completed" and
               event["data"]["turn_id"] in turn_ids[-1:] for event in log):
            return log
        if time.monotonic() >= deadline:
            lifecycle = [(event["type"], event["data"].get("turn_id"),
                          event["data"].get("text")) for event in log
                         if event["type"] in {"turn_started", "turn_completed",
                                              "turn_failed", "steering_added"}]
            raise AssertionError(
                f"turn containing {needle!r} did not complete; "
                f"lifecycle={lifecycle!r}; "
                f"terminal={bytes(child.buf)!r}"
            )
        child.drain(0.05)


def clear_draft_incrementally(child, prompt=DEFAULT_IDLE_PROMPT):
    start = len(child.buf)
    end = child.send_wait(b"\x15", b"\x1b[K", start=start)
    edit = bytes(child.buf[start:end])
    assert b"\x1b[2K" not in edit, edit
    assert prompt not in edit, edit


def assert_bytes_in_order(output, expected):
    offset = 0
    for byte in expected:
        offset = output.find(bytes((byte,)), offset)
        assert offset >= 0, output
        offset += 1


def wait_prompt_painted(child, prompt, start=0, timeout=8.0):
    prompt_end = child.wait(prompt, start=start, timeout=timeout)
    return child.wait(
        b"\x1b[K",
        start=prompt_end,
        timeout=timeout,
    )


def test_incremental_prompt_edit_and_utf8_cursor_column():
    with Child([]) as child:
        wait_prompt_painted(child, DEFAULT_IDLE_PROMPT)
        empty_tab_start = len(child.buf)
        end = child.send_wait(b"\t", "── chat ──".encode(), start=empty_tab_start)
        child.send_wait(b"\t", "── rollout ──".encode(), start=end)
        child.drain()
        start = len(child.buf)
        child.send(b"a")
        child.drain()
        edit = bytes(child.buf[start:])
        assert edit == b"a", edit
        assert b"\x1b[2K" not in edit, edit
        assert DEFAULT_IDLE_PROMPT not in edit, edit

        start = len(child.buf)
        child.send(b"\x1b[D")
        child.drain()
        movement = bytes(child.buf[start:])
        prompt_column = len("   HH:MM:SS") + len(DEFAULT_IDLE_PROMPT.decode())
        assert f"\r\x1b[{prompt_column}C".encode() in movement, movement
        assert f"\r\x1b[{prompt_column + 1}C".encode() not in movement, movement
        assert b"\x1b[2K" not in movement, movement
        assert DEFAULT_IDLE_PROMPT not in movement, movement


def test_incremental_active_prompt_keeps_status_stable():
    with Child([], ready=DEFAULT_IDLE_PROMPT) as child:
        child.send_wait(b"terminal_status\r", "»".encode())
        phase_start = len(child.buf)
        child.wait("◷".encode(), start=phase_start, timeout=1.0)

        start = len(child.buf)
        end = child.send_wait(b"a", b"a", start=start)
        edit = bytes(child.buf[start:end])
        assert b"\x1b[2K" not in edit, edit
        assert b"working\xe2\x80\xa6" not in edit, edit
        assert DEFAULT_ACTIVE_PROMPT not in edit, edit


def test_static_zero_width_spinner_has_no_refresh():
    config = write_config("static-spinner.ini",
        "[provider openai]\n[ui]\n"
        'prompt_spinner_goal = "\\0"\n'
        'prompt_spinner_provider = "\\0◆"\n'
        'prompt_spinner_tool = "\\0"\n'
        "prompt_spinner_per_second = 60\n")
    idle = DEFAULT_IDLE_PROMPT
    active = DEFAULT_ACTIVE_PROMPT
    with Child(["--config", str(config)], ready=idle) as child:
        first = child.send_wait(b"terminal_status\r", b"status-first-fragment ")
        wait_prompt_painted(child, active, start=first)
        assert re.search("◆ [0-9]{2}:[0-9]{2}:[0-9]{2}".encode() +
                         re.escape(active), child.buf), bytes(child.buf)
        settled = len(child.buf)
        child.drain(0.35)
        assert len(child.buf) == settled, bytes(child.buf[settled:])


def test_prompt_clock_lifetime():
    clock = "@{hour:02}:{minute:02}:{second:02}"
    config = write_config("prompt-clock.ini",
        "[provider openai]\n[ui]\nprompt = " + clock + "{chat::}"
        "{rollout-idle: {context:3}%{activity_spinner}›}"
        "{rollout-active: {context:3}%{activity_spinner}»}\n"
        'prompt_spinner_provider = " P"\n')
    child = Child(["--config", str(config), "-s", f"127.0.0.1:{free_port()}",
                   "-n", "clockagent", "-o", "clockop", "-r", "lab"])
    pattern = rb"@(\d{2}:\d{2}:\d{2})"

    def latest_clock():
        return re.findall(pattern, child.buf)[-1]

    try:
        child.wait(b": ")
        original = latest_clock()
        child.drain(1.1)
        # Empty Tab changes views, not the underlying composer capture.
        start = len(child.buf)
        child.send(b"\t")
        idle = b"@" + original + "   0% › ".encode()
        child.wait(idle, start=start)
        child.send(b"clock-draft")
        child.drain()
        start = len(child.buf)
        fcntl.ioctl(child.fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 48, 0, 0))
        child.wait(idle + b"clock-draft", start=start)
        start = len(child.buf)
        child.send_wait(b"\x12", b"reverse-i-search", start=start)
        child.drain(1.1)
        start = len(child.buf)
        child.send_wait(b"\x07", idle + b"clock-draft", start=start)
        # Cancellation retains the old displayed line and captures a new one.
        start = len(child.buf)
        cancelled = child.send_wait(b"\x03", b"^C\r\n", start=start)
        child.wait("   0% › ".encode(), start=cancelled)
        replacement = latest_clock()
        assert replacement != original, bytes(child.buf[start:])
        child.drain(1.1)
        # Blank submission retains the frozen label and captures a fresh clock.
        start = len(child.buf)
        child.send(b"\r")
        child.wait("   0% › ".encode(), start=start)
        child.drain(0.1)
        clocks = re.findall(pattern, child.buf[start:])
        assert len(clocks) >= 2 and clocks[0] == replacement and clocks[-1] != replacement, clocks
        replacement = clocks[-1]
        child.drain(1.1)
        start = len(child.buf)
        active_end = child.send_wait(b"terminal_status\r", "   ?%P» ".encode(), start=start)
        active_clock = latest_clock()
        assert active_clock != replacement, bytes(child.buf[start:])
        second = child.send_wait(b"preserved-draft", b"status-second-fragment", start=active_end)
        child.wait(b"preserved-draft", start=second)
        # Only the spinner/marker changes; clock and draft stay painted.
        # The provider can stop before the marker changes; the preceding
        # blank is already painted and need not be emitted a second time.
        settled = child.wait("›".encode(), start=second)
        assert set(re.findall(pattern, child.buf[active_end:])) == {active_clock}
        child.send_wait(b"\x03", b"^C\r\n", start=settled)
        child.drain()
        child.exit_now()
        child = None
    finally:
        if child:
            child.kill()


def test_initial_unrenderable_prompt_is_rejected_atomically():
    config = (Path(os.environ["SNAJPAGENT_TEST_ROOT"]) /
              "config" / "initial-unrenderable.ini")
    before = session_ids()

    for label in ("x" * 600, "x" * 508 + "{queue}", "{queued:({queue}) }"):
        config.write_text(
            "[provider openai]\n[ui]\n"
            "prompt = {chat:x}{rollout-idle:" + label +
            "}{rollout-active:z}\n",
            encoding="utf-8",
        )
        child = Child(["--config", str(config)])
        child.wait(
            b"configured prompt cannot be rendered with the current selection"
        )
        child.finish(expected=2, expect_resume=False)
        assert session_ids() == before


def test_incremental_multiline_delete_clears_old_tail():
    with Child([], ready=DEFAULT_IDLE_PROMPT) as child:
        child.send_wait(b"abcdef\nsecond", b"second")
        child.drain(0.05)
        child.send(b"\x1b[H" + b"\x1b[C" * 6)
        child.drain(0.05)

        start = len(child.buf)
        child.send(b"\x7f\x7f\x7f")
        child.drain(0.1)
        end = len(child.buf)
        edit = bytes(child.buf[start:end])
        assert edit.count(b"\x1b[K") == 3, edit
        assert b"second" not in edit and b"\n" not in edit, edit
        assert b"\x1b[2K" not in edit, edit
        assert DEFAULT_IDLE_PROMPT not in edit, edit


def test_incremental_wrapped_long_prompt_multiline_indent():
    model = "m" * 120
    prompt = f" openai/{model}/medium   0% › ".encode()
    with Child(["-m", model], ready=prompt) as child:
        start = len(child.buf)
        end = child.send_wait(b"x\n" * 8 + b"z", b"z", start=start)
        edit = bytes(child.buf[start:end])
        assert b"\x1b[2K" not in edit, edit
        assert prompt not in edit, edit


def test_steering():
    child = Child([], DEFAULT_IDLE_PROMPT)
    child.send_wait(b"slow\r", "»".encode())
    child.wait(b"working slowly")
    answer_end = child.send_wait(b"change course\r", b"steered: change course")
    child.exit_cleanly(answer_end)

    session_id = child.session_id()
    log = events(session_id)
    steering = one(log, "steering_added")
    interrupted = one(log, "response_interrupted")
    starts = [item for item in log if item["type"] == "response_started"]
    assert interrupted["data"]["origin"] == "steering"
    assert interrupted["data"]["reason"] == "steered"
    assert interrupted["data"]["partial_public"][0]["text"] == "working slowly\n"
    assert len(starts) == 2
    assert starts[1]["data"]["steering_ids"] == [steering["data"]["steering_id"]]


def test_repeated_steering_rearms_composer():
    child = Child([], DEFAULT_IDLE_PROMPT)
    child.send_wait(b"slow_resteer\r", b"working slowly")

    first_ack = child.send_wait(b"first steer\r", DEFAULT_ACTIVE_PROMPT + b"first steer")
    boundary = child.wait(b"first steer\r\n",
                          start=first_ack - len(b"first steer"))
    child.wait(DEFAULT_ACTIVE_PROMPT, start=boundary)
    second_ack = child.send_wait(b"second steer\r", DEFAULT_ACTIVE_PROMPT + b"second steer",
                            start=first_ack)
    boundary = child.wait(b"second steer\r\n",
                          start=second_ack - len(b"second steer"))
    child.wait(DEFAULT_ACTIVE_PROMPT, start=boundary)
    answer_end = child.wait(b"repeated steering complete")
    child.exit_cleanly(answer_end)

    log = events(child.session_id())
    steering = [item for item in log if item["type"] == "steering_added"]
    starts = [item for item in log if item["type"] == "response_started"]
    interrupted = [item for item in log
                   if item["type"] == "response_interrupted"]
    assert [item["data"]["text"] for item in steering] == [
        "first steer", "second steer"
    ]
    projected = [steering_id for item in starts
                 for steering_id in item["data"]["steering_ids"]]
    assert projected == [item["data"]["steering_id"] for item in steering]
    assert all(item["data"]["origin"] == "steering" for item in interrupted)


def test_public_index_gap():
    child = Child([], DEFAULT_IDLE_PROMPT)
    commentary_end = child.send_wait(b"public_index_gap\r", b"Checking hidden work.")
    answer_end = child.wait(b"Gap-safe final.", start=commentary_end)
    child.exit_cleanly(answer_end)

    log = events(child.session_id())
    completed = one(log, "response_completed")
    assert [item["kind"] for item in completed["data"]["items"]] == [
        "assistant", "assistant"
    ]
    assert not [item for item in log if item["type"] == "response_failed"]
    assert not [item for item in log if item["type"] == "turn_failed"]


def test_public_index_diagnostic():
    child = Child([], DEFAULT_IDLE_PROMPT)
    child.send_wait(b"public_index_decrease\r", b"index one")
    failure_end = child.wait(b"public output indexes did not increase")
    exhausted = child.wait(b"turn failed; try /retry", start=failure_end, timeout=20.0)
    child.exit_cleanly(exhausted)

    log = events(child.session_id())
    failures = [item for item in log if item["type"] == "response_failed"]
    assert len(failures) == 6
    assert len([item for item in log if item["type"] == "turn_recovery"]) == 5
    failed = failures[-1]
    assert failed["data"]["class"] == "protocol"
    assert failed["data"]["message"] == (
        "public output indexes did not increase"
    )
    assert one(log, "turn_failed")["data"]["class"] == "protocol"


def test_split_utf8_steering():
    before = session_ids()
    child = Child([], PROMPT.rstrip())
    child.send(b"slow_utf8\r")
    deadline = time.monotonic() + 4.0
    while session_ids() == before:
        assert time.monotonic() < deadline
        child.read_once(0.02)
    session = new_session(before)
    while not any(e["type"] == "response_started" for e in events(session)):
        assert time.monotonic() < deadline
        child.read_once(0.02)
    answer_end = child.send_wait(b"change\r", b"steered: change")
    child.exit_cleanly(answer_end)

    interrupted = one(events(new_session(before)), "response_interrupted")
    assert interrupted["data"]["partial_public"] == []


def test_typing_pause_and_transient_composer():
    config = write_config("typing-pause.ini", "[provider openai]\n[ui]\ntyping_pause_ms = 300\n")
    child = Child(["--config", str(config)], DEFAULT_IDLE_PROMPT)
    first_end = child.send_wait(b"typing_stream\r", b"model-output-one")

    edit_start = len(child.buf)
    child.send_wait(b"a", b"a", start=edit_start)
    time.sleep(0.1)
    edit_start = len(child.buf)
    child.send_wait(b"b", b"b", start=edit_start)
    second_start = time.monotonic()
    quiet_start = len(child.buf)
    child.drain(0.15)
    assert b"model-output-two" not in child.buf[quiet_start:]
    second_end = child.wait(b"model-output-two", start=quiet_start)
    assert time.monotonic() - second_start >= 0.20

    edit_start = len(child.buf)
    child.send_wait(b"c", b"c", start=edit_start)
    third_start = time.monotonic()
    quiet_start = len(child.buf)
    child.drain(0.15)
    assert b"model-output-three" not in child.buf[quiet_start:]
    third_end = child.wait(b"model-output-three", start=quiet_start)
    assert time.monotonic() - third_start >= 0.20
    child.wait(b"abc", start=third_end)
    child.wait_idle_prompt(start=third_end)
    child.drain(0.05)
    clear_draft_incrementally(child)
    child.send(b"/exit\r")
    assert child.reap() == 0

    completed = one(events(child.session_id()), "response_completed")
    assert completed["data"]["items"][0]["text"] == (
        "model-output-one model-output-two model-output-three"
    )


def test_armed_fifo():
    child = Child([], DEFAULT_IDLE_PROMPT)
    child.send_wait(b"slow\r", b"working slowly")
    child.send_wait(b"ping\t", b"queued (/next or /q c) " + PROMPT + b"ping")
    child.wait(b"/medium   ?% (1) \xc2\xbb ")
    child.send_wait(b"retained-draft", b"slow complete")
    answer_end = child.wait(b"pong")
    child.wait(b"retained-draft", start=answer_end)
    child.wait_idle_prompt(start=answer_end)
    child.send(b"\x15")
    child.exit_cleanly(answer_end)

    log = events(child.session_id())
    queued = one(log, "future_turn_queued")
    turns = [item for item in log if item["type"] == "turn_started"]
    assert len(turns) == 2
    assert turns[0]["data"]["input_kind"] == "direct"
    assert turns[1]["data"]["input_kind"] == "queued"
    assert turns[1]["data"]["queue_id"] == queued["data"]["queue_id"]
    assert turns[1]["data"]["queue_seq"] == queued["seq"]
    assert turns[1]["data"]["text"] == "ping"


def test_queue_prompt_counts():

    def count_repaint(count, start):
        # The retained frame may repaint just the changed digits. The tmux
        # queue case verifies the complete prompt and counts on screen.
        end = time.monotonic() + 8.0
        pattern = re.compile(re.escape(f"({count}) »".encode()) +
                             rb"|\r\x1b\[51C" + str(count).encode() + rb"(?=[)\r])")
        while time.monotonic() < end:
            match = pattern.search(child.buf, start)
            if match:
                return match.end()
            child.read_once(0.1)
        raise AssertionError(f"queue count {count} did not repaint: {bytes(child.buf)!r}")
    with Child([], ready=DEFAULT_IDLE_PROMPT) as child:
        after = child.send_wait(b"queue_slow\r", b"working slowly")
        for count in range(1, 11):
            child.send(f"entry-{count}\t".encode())
            after = count_repaint(count, after)
        for command, count in ((b"/queue pop\r", 9), (b"/queue 1 delete\r", 8)):
            child.send(command)
            after = count_repaint(count, after)
        after = child.send_wait(b"\t", b" : ", start=after)
        child.drain(0.05)
        assert b"(8)" not in child.buf[after:]
        after = child.send_wait(b"\t", b"/medium   ?% (8) \xc2\xbb ", start=after)
        after = child.send_wait(b"/queue clear\r", b"8 future turns cancelled", start=after)
        after = child.wait("»".encode(), start=after)
        after = child.send_wait(b"\x03", b"turn interrupted", start=after)
        child.exit_cleanly(after)


def test_read_only_queries():
    Path(WORKSPACE, "ro-input.txt").write_text("native text\nsecond line\n", encoding="utf-8")
    child = Child([], DEFAULT_IDLE_PROMPT)
    end = child.send_wait_idle(b"/ro\r", b"usage: /ro QUERY")
    end = child.send_wait_idle(b"/ro ro_native\r", b"native complete")
    end = child.send_wait_idle(b"/ro ro_denied\r", b"denied complete")
    end = child.send_wait_idle(b"ping\r", b"pong")
    end = child.send_wait(b"//ro ping\r", b"fixture answer", start=end)
    child.exit_cleanly(end)
    log = events(child.session_id())
    turns = [x["data"] for x in log if x["type"] == "turn_started"]
    assert [x["read_only"] for x in turns] == [True, True, False, False]
    assert turns[-1]["text"] == "/ro ping"
    results = [x["data"]["result"] for x in log if x["type"] == "tool_finished"]
    assert len(results) == 11
    assert all(x["status"] == "succeeded" for x in results[:3])
    assert "1:native text\n2:second line\n" in results[1]["model_text"]
    assert "ro-input.txt:1:native text" in results[2]["model_text"]
    assert all(x["status"] == "not_run" and "read-only" in x["model_text"]
               for x in results[3:])
    assert not any(x["type"] == "goal_started" for x in log)

    child = Child([], DEFAULT_IDLE_PROMPT)
    child.send_wait(b"slow\r", b"working slowly")
    end = child.send_wait(b"/ro ping\r", b"queued (/next or /q c) " + PROMPT + b"/ro ping")
    child.send_wait(b"/queue /ro repeat\r", b"queued (/next or /q c) " + PROMPT + b"/ro repeat", start=end)
    end = child.send_wait(b"//ro ping\t", b"slow complete")
    end = child.wait(b"pong", start=end)
    end = child.wait(b"haha", start=end)
    end = child.wait(b"fixture answer", start=end)
    child.exit_cleanly(end)
    log = events(child.session_id())
    turns = [x["data"] for x in log if x["type"] == "turn_started"]
    assert [x["read_only"] for x in turns] == [False, True, True, False]
    assert [x["text"] for x in turns] == ["slow", "ping", "repeat", "/ro ping"]
    assert not any(x["type"] in ("steering_added", "response_interrupted") for x in log)

    # Ordinary steers keep the existing read-only turn read-only.
    child = Child([], DEFAULT_IDLE_PROMPT)
    child.send_wait(b"/ro slow\r", b"working slowly")
    end = child.send_wait(b"replacement\r", b"steered: replacement")
    child.exit_cleanly(end)
    log = events(child.session_id())
    assert one(log, "turn_started")["data"]["read_only"] is True
    one(log, "steering_added")


def test_compaction_ignores_legacy_samples():
    root = Path(os.environ["SNAJPAGENT_TEST_ROOT"])
    state = root / "statistical-compact"
    state.mkdir(mode=0o700)
    config = write_config("statistical-compact.ini",
        "[agent]\nread_agents_md = false\n[provider openai]\n"
        "exact_token_count = false\nnative_compaction = false\n"
        "auto_compact_input_tokens = 1\n")
    model = {
        "id": DEFAULT_MODEL, "count_capability": "unsupported",
        "default_effort": "medium", "efforts": ["medium"],
        "observed_hard_input_tokens": 0, "observed_input_tokens": 1000,
        "observed_input_bytes": 10000,
        "limits": {name: None for name in (
            "auto_compact_input_tokens", "context_window_tokens",
            "effective_context_window_percent", "input_context_window_tokens",
            "max_context_window_tokens", "max_input_tokens", "max_output_tokens")},
    }
    model["limits"]["max_input_tokens"] = 10000
    cache = state / "models.json"
    cache.write_text(json.dumps({"schema_version": 1, "updated_at_ms": 1,
        "providers": [{"name": "openai", "protocol": "openai",
                       "base_url": "https://api.openai.com", "models": [model]}]}),
        encoding="utf-8")
    cache.chmod(0o600)
    run = subprocess.run([BINARY, "--dotdir", str(state), "--config", str(config),
                          "-e", "--", "large prior user " + "a" * 25000],
                         cwd=WORKSPACE, capture_output=True, timeout=10)
    assert run.returncode == 0, run.stderr
    logs = list((state / "sessions").glob("*/events.jsonl"))
    assert len(logs) == 1
    log = [json.loads(line) for line in logs[0].read_text().splitlines()]
    assert not any(event["type"] == "compaction_started" for event in log)
    starts = [event["data"] for event in log if event["type"] == "response_started"]
    assert starts and all(e["count_method"] == "unknown" and e["input_tokens_bound"] == 0 for e in starts)
    assert not any(event["type"] == "turn_failed" for event in log)


def test_read_only_multiline_compaction_and_chat():
    Path(WORKSPACE, "ro-input.txt").write_text("native text\nsecond line\n", encoding="utf-8")
    config = write_config("ro-compaction.ini", "[provider openai]\nauto_compact_input_tokens = 1\n")
    child = Child([], DEFAULT_IDLE_PROMPT)
    end = child.send_wait(b"ping\r", b"pong")
    child.exit_cleanly(end)
    sid = child.session_id()
    child = Child(["--config", str(config), "--resume", sid], DEFAULT_ACCOUNTED_IDLE_PROMPT)
    end = child.send_wait(b"/ro ro_native\r", b"native complete")
    child.exit_cleanly(end)
    log = events(sid)
    assert any(x["type"] == "compaction_completed" for x in log)
    assert [x for x in log if x["type"] == "turn_started"][-1]["data"]["read_only"] is True

    child = Child([], DEFAULT_IDLE_PROMPT)
    child.send_wait(b"slow\r", b"working slowly")
    end = child.send_wait(b"\x1b[200~/ro inspect\nmultiline\x1b[201~\r", b"queued (/next or /q c) " + PROMPT)
    child.send_wait(b"\x1b[200~/queue /ro another\nquery\x1b[201~\r", b"queued (/next or /q c) " + PROMPT, start=end)
    end = child.wait(b"slow complete")
    end = child.wait(b"fixture answer", start=end)
    end = child.wait(b"fixture answer", start=end)
    child.exit_cleanly(end)
    log = events(child.session_id())
    turns = [x["data"] for x in log if x["type"] == "turn_started"]
    assert [x["text"] for x in turns[1:]] == ["inspect\nmultiline", "another\nquery"]
    assert all(x["read_only"] for x in turns[1:])
    assert not any(x["type"] == "steering_added" for x in log)

    child = Child(["--no-color", "-s", f"127.0.0.1:{free_port()}",
                   "-n", "roagent", "-o", "rooperator", "-r", "lab"], chat_prompt("rooperator"))
    end = child.send_wait(b"/ro ro_native\r", b"native complete")
    child.exit_cleanly(end)
    log = events(child.session_id())
    turn = one(log, "turn_started")["data"]
    assert turn["read_only"] and turn["text"] == "ro_native"
    assert not any(x["type"] in ("irc_reply_reminder", "turn_failed") for x in log)
    assert all(x["data"]["result"]["status"] == "succeeded"
               for x in log if x["type"] == "tool_finished")


def test_queue_edit_resume_at_acknowledgement():
    with Child([], DEFAULT_IDLE_PROMPT) as child:
        child.send_wait(b"queue_slow\r", b"working slowly")
        child.send_wait(b"/queue repeat\r", b"queued (/next or /q c)")
        child.send_wait(b"/queue 1 edit\r", b"repeat", start=len(child.buf))
        child.send_wait(b"\x15ping\r", b"ping", start=len(child.buf))
        sid = child.session_id()
        deadline = time.monotonic() + 3
        while not any(e["type"] == "future_turn_edited" for e in events(sid)):
            assert time.monotonic() < deadline
            child.read_once(.02)
        child.kill()
    path = STATE_ROOT / sid / "events.jsonl"
    lines = path.read_bytes().splitlines(keepends=True)
    edit = next(e for e in map(json.loads, lines) if e["type"] == "future_turn_edited")
    assert edit["data"]["armed"] is True
    path.write_bytes(b"".join(lines[:edit["seq"]]))
    with Child(["--resume", sid], ready=None) as child:
        end = child.wait(b"pong")
        child.exit_cleanly(end)
    turns = [e["data"] for e in events(sid) if e["type"] == "turn_started"]
    assert len(turns) == 2 and turns[-1]["text"] == "ping", turns


def test_read_only_queue_replay_and_edit():
    child = Child([], DEFAULT_IDLE_PROMPT)
    child.send_wait(b"queue_slow\r", b"working slowly")
    child.send_wait(b"/ro ping\t", b"queued (/next or /q c) " + PROMPT + b"/ro ping")
    end = len(child.buf)
    end = child.send_wait(b"/queue 1 edit\r", b"/ro ping", start=end)
    end = child.send_wait(b"\x15/ro repeat\r", b"/ro repeat", start=end)
    child.wait(f"openai/{DEFAULT_MODEL}/medium".encode(), start=end)
    end = child.send_wait(b"\x03", b"turn interrupted")
    child.exit_cleanly(end)
    sid = child.session_id()
    log = events(sid)
    assert one(log, "future_turn_edited")["data"]["read_only"] is True
    child = Child(["--resume", sid])
    end = child.wait(b"queued future turns are paused")
    child.wait_idle_prompt(start=end)
    end = child.send_wait(b"/goal slow goal\r", GOAL_SET)
    # Existing explicit goal start arms retained FIFO work before the goal.
    end = child.wait(b"haha", start=end)
    end = child.wait(b"goal done", start=end, timeout=10.0)
    child.exit_cleanly(end)
    log = events(sid)
    turns = [x["data"] for x in log if x["type"] == "turn_started"]
    assert turns[1]["input_kind"] == "queued" and turns[1]["read_only"] is True
    assert turns[1]["text"] == "repeat"

    child = Child([], DEFAULT_IDLE_PROMPT)
    child.send_wait(b"/goal slow goal\r", b"working on goal")
    end = child.send_wait(b"/ro ping\t", b"queued (/next or /q c) " + PROMPT + b"/ro ping")
    child.send_wait(b"/queue 1 edit\r", b"/ro ping", start=end)
    end = child.wait(b"goal checkpoint")
    child.drain(0.1)
    assert b"goal done" not in child.buf[end:] and b"pong" not in child.buf[end:]
    end = child.send_wait(b"\x15/ro repeat\r", b"/ro repeat", start=end)
    child.wait(f"openai/{DEFAULT_MODEL}/medium".encode(), start=end)
    end = child.send_wait(b"/next\r", b"haha", start=end)
    end = child.wait(b"goal done", start=end)
    child.exit_cleanly(end)
    log = events(child.session_id())
    assert [x["data"]["input_kind"] for x in log if x["type"] == "turn_started"] == [
        "goal", "queued", "goal"
    ]


def test_managed_command_steering_and_tab_queue():
    child = Child(["-v"], DEFAULT_IDLE_PROMPT)
    tool_start = child.send_wait(b"managed_command_steer\r", b"fixture managed steering wait")
    deadline = time.monotonic() + 1.0
    while not any(frame.encode() in child.buf[tool_start:] for frame in "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏"):
        remaining = deadline - time.monotonic()
        assert remaining > 0, "tool spinner was not shown"
        child.read_once(remaining)
    steering_ack = child.send_wait(b"terminate it\r", "» terminate it\r\n".encode())
    child.wait("»".encode(), start=steering_ack)
    answer_end = child.wait(b"managed command steering complete")
    child.exit_cleanly(answer_end)

    log = events(child.session_id())
    steering = one(log, "steering_added")
    running = next(
        item for item in log
        if item["type"] == "tool_finished" and
        item["data"]["result"]["status"] == "running"
    )
    assert running["data"]["result"]["reason"] == "steering_handoff"
    assert running["seq"] > steering["seq"]
    assert running["data"]["result"]["handle"] is not None
    assert not [item for item in log if item["type"] == "process_closed"]

    child = Child(["-v"], DEFAULT_IDLE_PROMPT)
    child.send_wait(b"managed_command_queue\r", b"fixture managed queue wait")
    child.send_wait(b"ping\t", b"queued (/next or /q c) " + PROMPT + b"ping")
    command_end = child.wait(b"managed command queue complete")
    answer_end = child.wait(b"pong", start=command_end)
    child.exit_cleanly(answer_end)

    log = events(child.session_id())
    queued = one(log, "future_turn_queued")
    turns = [item for item in log if item["type"] == "turn_started"]
    first_turn_id = turns[0]["data"]["turn_id"]
    assert not [item for item in log
                if item["type"] == "steering_added" and
                item["data"]["turn_id"] == first_turn_id]
    assert not [item for item in log
                if item["type"] == "response_interrupted" and
                item["data"]["turn_id"] == first_turn_id]
    first_results = [item["data"]["result"] for item in log
                     if item["type"] == "tool_finished" and
                     item["data"]["turn_id"] == first_turn_id]
    assert first_results and all(result["status"] != "running"
                                 for result in first_results)
    assert turns[1]["data"]["input_kind"] == "queued"
    assert turns[1]["data"]["queue_id"] == queued["data"]["queue_id"]
    assert turns[1]["data"]["text"] == "ping"


def test_steering_during_pre_response_compaction():
    config = write_config("steering-compaction.ini", "[provider openai]\nauto_compact_input_tokens = 1\n")
    child = Child([], DEFAULT_IDLE_PROMPT)
    answer_end = child.send_wait(b"context_anchor_chain\r", b"context anchor complete")
    child.exit_cleanly(answer_end)
    session_id = child.session_id()

    child = Child(["--config", str(config), "--resume", session_id], DEFAULT_ACCOUNTED_IDLE_PROMPT)
    child.send_wait(b"compaction_steer\r", "»".encode())
    steer_end = child.send_wait(b"change plan\r", DEFAULT_ACTIVE_PROMPT + b"change plan")
    child.wait("»".encode(), start=steer_end)
    answer_end = child.wait(b"fixture answer", start=steer_end)
    child.exit_cleanly(answer_end)

    log = events(session_id)
    interrupted = [item for item in log
                   if item["type"] == "compaction_interrupted"]
    assert len(interrupted) == 1
    assert interrupted[0]["data"]["reason"] == "steering"
    steering = one([item for item in log
                    if item["type"] == "steering_added"],
                   "steering_added")
    turns = [item for item in log if item["type"] == "turn_started"]
    turn_id = turns[-1]["data"]["turn_id"]
    starts = turn_events(log, "response_started", turn_id)
    assert len(starts) == 1
    assert starts[0]["data"]["steering_ids"] == [
        steering["data"]["steering_id"]
    ]

    resumed = Child(["--config", str(config), "--resume", session_id], PROMPT.rstrip())
    resumed.exit_now()


def test_steering_during_capacity_recovery_compaction():
    child = Child([], DEFAULT_IDLE_PROMPT)
    answer_end = child.send_wait(b"ping\r", b"pong")
    child.exit_cleanly(answer_end)
    session_id = child.session_id()

    child = Child(["--resume", session_id], DEFAULT_ACCOUNTED_IDLE_PROMPT)
    child.send(b"capacity_recovery_steer\r")
    deadline = time.monotonic() + 4.0
    while not any(e["type"] == "compaction_started" and
                  e["data"]["reason"] == "provider_rejection"
                  for e in events(session_id)):
        assert time.monotonic() < deadline
        child.read_once(0.02)
    steer_end = child.send_wait(b"change recovery plan\r", b"\xc2\xbb change recovery plan")
    answer_end = child.wait(b"fixture answer", start=steer_end)
    child.exit_cleanly(answer_end)

    log = events(session_id)
    turn = [item for item in log if item["type"] == "turn_started"][-1]
    turn_id = turn["data"]["turn_id"]
    rejected = turn_events(log, "response_capacity_rejected", turn_id)
    interrupted = [item for item in log
                   if item["type"] == "compaction_interrupted"]
    compactions = [item for item in log
                   if item["type"] == "compaction_started" and
                   item["data"]["reason"] == "provider_rejection"]
    completed = [item for item in log if item["type"] == "compaction_completed"]
    steering = turn_events(log, "steering_added", turn_id)
    starts = turn_events(log, "response_started", turn_id)
    assert len(rejected) == 1
    assert rejected[0]["data"]["observed_hard_input_tokens"] == 89999
    assert re.fullmatch(
        r"[0-9a-f]{64}", rejected[0]["data"]["provider_source_sha256"]
    )
    assert len(interrupted) == 1
    assert interrupted[0]["data"]["reason"] == "steering"
    assert len(compactions) == 2 and len(completed) == 1
    assert len(steering) == 1 and len(starts) == 2
    assert starts[1]["data"]["steering_ids"] == [
        steering[0]["data"]["steering_id"]
    ]
    assert (rejected[0]["seq"] < compactions[0]["seq"] <
            interrupted[0]["seq"] < compactions[1]["seq"] <
            completed[0]["seq"] < starts[1]["seq"])

    resumed = Child(["--resume", session_id])
    prompt_end = resumed.wait(b"\xe2\x80\xba ")
    status_end = resumed.send_wait(b"/status\r", b"context: source=observed", start=prompt_end)
    status_end = resumed.wait(
        b"observed ceiling: hard-input=89999", start=status_end
    )
    resumed.wait(b"binding=current", start=status_end)
    resumed.exit_now()

    mismatch_config = (
        Path(os.environ["SNAJPAGENT_TEST_ROOT"]) /
        "capacity-source-mismatch.ini"
    )
    mismatch_config.write_text(
        "[provider openai]\nbase_url = https://different.example.test\n",
        encoding="utf-8",
    )
    mismatched = Child([
        "--config", str(mismatch_config), "--resume", session_id
    ])
    prompt_end = mismatched.wait(DEFAULT_IDLE_PROMPT)
    status_end = mismatched.send_wait(b"/status\r", b"context: source=unknown", start=prompt_end)
    status_end = mismatched.wait(
        b"observed ceiling: hard-input=89999", start=status_end
    )
    mismatched.wait(b"binding=source mismatch; ignored", start=status_end)
    mismatched.exit_now()


def test_agents_md_config():
    root = Path(os.environ["SNAJPAGENT_TEST_ROOT"])
    workspace = root / "agents-workspace"
    workspace.mkdir()
    agents = workspace / "AGENTS.md"
    contents = "Always answer fixture prompts normally.\n"
    agents.write_text(contents, encoding="utf-8")

    enabled_config = write_config("agents-enabled.ini", "[provider openai]\n[agent]\nread_agents_md = true\n")
    child = Child(["--config", str(enabled_config), "-C", str(workspace)], PROMPT.rstrip())
    answer_end = child.send_wait(b"ping\r", b"pong")
    child.exit_cleanly(answer_end)
    turn = one(events(child.session_id()), "turn_started")
    instructions = turn["data"]["instructions"]
    assert instructions
    assert instructions[-1] == str(agents)

    disabled_config = write_config("agents-disabled.ini",
        "[provider openai]\n[agent]\nread_agents_md = false\n")
    child = Child(["--config", str(disabled_config), "-C", str(workspace)], PROMPT.rstrip())
    answer_end = child.send_wait(b"ping\r", b"pong")
    child.exit_cleanly(answer_end)
    turn = one(events(child.session_id()), "turn_started")
    assert turn["data"]["instructions"] == []

    # Explicit roots survive disabled automatic discovery, compaction and resume.
    docs = root / 'working docs "quoted"'
    docs.mkdir()
    entry = docs / "AGENTS.md"
    entry.write_text("Private notes must not be injected.\n", encoding="utf-8")
    before = session_ids()
    options = ["--config", str(disabled_config), "-C", str(workspace),
               "-d", str(docs), "-d" + str(docs / "."), "-d", str(workspace)]
    child = Child(options, PROMPT.rstrip())
    end = child.send_wait_idle(b"ping\r", b"pong")
    end = child.send_wait_idle(b"/compact\r", COMPACTED, start=end)
    (workspace / "ro-input.txt").write_text("native docs check\n", encoding="utf-8")
    end = child.send_wait(f"/ro ro_native {entry}\r".encode(), b"native complete", start=end)
    child.exit_cleanly(end)
    session = new_session(before)
    results = [e["data"]["result"] for e in events(session) if e["type"] == "tool_finished"]
    assert results[1]["status"] == "succeeded", results
    assert "Private notes must not be injected." in results[1]["model_text"]
    for turn in [e for e in events(session) if e["type"] == "turn_started"]:
        assert turn["data"]["instructions"] == [str(entry), str(agents)]
    entry.write_text("Updated live notes.\n", encoding="utf-8")
    child = Child([*options, "--resume", session, "--", "ping"])
    end = child.wait(b"pong")
    child.exit_cleanly(end)
    turns = [e for e in events(session) if e["type"] == "turn_started"]
    assert turns[-1]["data"]["instructions"] == [str(entry), str(agents)]


def test_interrupt():
    child = Child([], PROMPT.rstrip())
    child.send_wait(b"slow\r", b"working slowly")
    interrupted_end = child.send_wait(b"\x03", b"turn interrupted")
    child.drain(0.1)
    assert os.waitpid(child.pid, os.WNOHANG) == (0, 0), bytes(child.buf)
    idle_cancel = len(child.buf)
    child.send_wait(b"x\x03", b"^C\r\n", start=idle_cancel)
    child.drain(0.2)
    assert os.waitpid(child.pid, os.WNOHANG) == (0, 0), bytes(child.buf)
    assert bytes(child.buf[idle_cancel:]).count(b"^C\r\n") == 1
    child.exit_now()

    log = events(child.session_id())
    response = one(log, "response_interrupted")
    turn = one(log, "turn_interrupted")
    assert response["data"]["origin"] == "user"
    assert response["data"]["reason"] == "cancelled"
    assert turn["data"]["origin"] == "user"
    assert turn["data"]["reason"] == "cancelled"


def test_active_ctrl_c_clears_draft():
    child = Child([], PROMPT.rstrip())
    child.send_wait(b"queue_slow\r", b"working slowly")

    edit_start = len(child.buf)
    child.send_wait(b"discard this", b"this", start=edit_start)
    clear_start = len(child.buf)
    clear_end = child.send_wait(b"\x03", b"^C\r\n", start=clear_start)
    cleared = bytes(child.buf[clear_start:clear_end])
    assert b"interrupting" not in cleared
    assert b"\x1b[2K" not in cleared

    answer_end = child.send_wait(b"replacement\r", b"steered: replacement", start=clear_end)
    child.exit_cleanly(answer_end)

    log = events(child.session_id())
    steering = one(log, "steering_added")
    response = one(log, "response_interrupted")
    assert steering["data"]["text"] == "replacement"
    assert response["data"]["origin"] == "steering"
    assert not [item for item in log if item["type"] == "turn_interrupted"]


def test_ctrl_c_cancels_partial_editor_states():
    with Child([], ready=DEFAULT_IDLE_PROMPT) as child:
        escape_start = len(child.buf)
        child.send(b"escape-draft\x1b")
        escape_end = child.send_wait(b"\x03", b"^C\r\n", start=escape_start)
        assert_bytes_in_order(bytes(child.buf[escape_start:escape_end]),
                              b"escape-draft")
        child.wait(DEFAULT_IDLE_PROMPT, start=escape_end)

        paste_start = len(child.buf)
        child.send(b"\x1b[200~paste-draft")
        paste_end = child.send_wait(b"\x03", b"^C\r\n", start=paste_start)
        assert_bytes_in_order(bytes(child.buf[paste_start:paste_end]),
                              b"paste-draft")
        child.wait(DEFAULT_IDLE_PROMPT, start=paste_end)
        child.send_wait(b"clean-after-cancel\r", b"fixture answer", start=paste_end)


def test_history_local_first_archive():
    history = Path(DOTDIR, "prompt_history")
    history.parent.mkdir(parents=True, exist_ok=True)
    saved = history.read_bytes() if history.exists() else b""
    archive = (b"archive-oldest-995\n" +
               b"".join(f"archive-entry-{i:05d}\n".encode() for i in range(1200)) +
               b"archive-match-995-global\n")
    history.write_bytes(archive)
    try:
        with Child([], DEFAULT_IDLE_PROMPT, cols=160) as local, Child([], DEFAULT_IDLE_PROMPT, cols=160) as peer:
            before = session_ids()
            local.send_wait_idle(b"archive-match-995-local\r", b"fixture answer", start=len(local.buf))
            sid = new_session(before)
            peer.send_wait_idle(b"archive-peer-995\r", b"fixture answer", start=len(peer.buf))
            assert history.read_bytes() == archive, "live submissions changed the global archive"
            local_file = Path(DOTDIR, "sessions", sid, "prompt_history")
            assert b"archive-oldest" not in local_file.read_bytes(), "global archive copied into local history"
            start = len(local.buf)
            local.send_wait(b"\x12archive-match-995", b"': archive-match-995-local", start=start)
            local.send_wait(b"\x12", b"global", start=len(local.buf))
            local.send(b"\x07")
            start = len(local.buf)
            local.send_wait(b"\x1b[A", b"archive-match-995-local", start=start)
            # Up/Down first cross draft rows; Ctrl-A puts this single-line recall at its start.
            local.send_wait(b"\x01\x1b[A", b"global", start=len(local.buf))
            local.send_wait(b"\x1b[B", b"local", start=len(local.buf))
            local.send_wait(b"\x10", b"global", start=len(local.buf))
            start = len(local.buf)
            local.send(b"\x10" * 105 + b"\x0e\r")
            local.wait(b"fixture answer", start=start)
            local.wait_idle_prompt(start=start)
            submitted = [e for e in events(sid) if e["type"] == "turn_started"]
            assert submitted[-1]["data"]["text"] == "archive-entry-01096"

            local.send_wait(b"\x12archive-oldest-995", b"': archive-oldest-995", start=len(local.buf))
            local.send(b"\x07")
            local.send_wait(b"\x12archive-peer-995", b"failed reverse-i-search", start=len(local.buf))
            local.send(b"\x07")
            peer.exit_now()
            local.send_wait(b"\x12archive-peer-995", b"': archive-peer-995", start=len(local.buf))
            local.send(b"\x07")
            local.exit_now()
        assert history.read_bytes().startswith(archive)
    finally:
        history.write_bytes(saved)


def test_history_repeated_resume_exit():
    history = Path(DOTDIR, "prompt_history")
    marker = b"history-resume-once-995"
    initial = history.read_bytes().splitlines().count(marker) if history.exists() else 0
    with Child([], DEFAULT_IDLE_PROMPT) as child:
        before = session_ids()
        child.send_wait_idle(marker + b"\r", b"fixture answer", start=len(child.buf))
        sid = new_session(before)
        child.send(b"\x04")
        child.finish()
    original = history.read_bytes()
    for _ in range(4):
        with Child(["--resume", sid], DEFAULT_ACCOUNTED_IDLE_PROMPT) as child:
            child.send_wait(b"\x1b[A", marker, start=len(child.buf))
            child.send_wait(b"\x03", b"^C\r\n", start=len(child.buf))
            child.send(b"\x04")
            child.finish()
        assert history.read_bytes() == original, "resume/exit remerged existing history"
    with Child(["--resume", sid], DEFAULT_ACCOUNTED_IDLE_PROMPT) as child:
        child.send_wait_idle(marker + b"\r", b"fixture answer", start=len(child.buf))
        child.exit_now()
    assert history.read_bytes().splitlines().count(marker) == initial + 2, "intentional repeat lost or duplicated"
    original = history.read_bytes()
    for _ in range(3):
        with Child(["--resume", sid], DEFAULT_ACCOUNTED_IDLE_PROMPT) as child:
            child.exit_now()
        assert history.read_bytes() == original + b"/exit\n", "/exit remerged older submissions"
        original = history.read_bytes()


def test_history_large_archive():
    history = Path(DOTDIR, "prompt_history")
    history.parent.mkdir(parents=True, exist_ok=True)
    saved = history.read_bytes() if history.exists() else b""
    try:
        with history.open("wb") as file:
            file.write(b"history-large-oldest-995\n")
            block = (b"old archive filler " + b"x" * 1000 + b"\n") * 1000
            for _ in range(24):
                file.write(block)
            file.write(b"history-large-newest-995\n")
        size = history.stat().st_size
        digest = hashlib.sha256(history.read_bytes()).digest()
        with Child([], DEFAULT_IDLE_PROMPT) as child:
            child.send_wait(b"\x1b[A", b"history-large-newest-995", start=len(child.buf))
            child.send_wait(b"\x03", b"^C\r\n", start=len(child.buf))
            start = len(child.buf)
            child.send(b"\x12history-large-oldest-995")
            # The renderer may change only 'new' to 'old' in the matched draft.
            child.wait_pattern(re.compile(rb"\r\x1b\[\d+C(?:old|history-large-oldest-995)"), start=start)
            child.send_wait(b"\x1b", b"history-large-oldest-995", start=len(child.buf))
            child.send_wait(b"\x03", b"^C\r\n", start=len(child.buf))
            child.send(b"\x04")
            child.finish(expect_resume=False)
        assert history.stat().st_size == size
        assert hashlib.sha256(history.read_bytes()).digest() == digest
    finally:
        history.write_bytes(saved)


def test_history_sparse_archive_cancellation():
    history = Path(DOTDIR, "prompt_history")
    saved = history.read_bytes() if history.exists() else b""
    try:
        with history.open("wb") as file:
            file.write(b"sparse-oldest-995\n")
            file.seek(2**31 + 8192)
            file.write(b"\nsparse-newest-995\n")
        size = history.stat().st_size
        with Child([], DEFAULT_IDLE_PROMPT) as child:
            # No scan of the multi-gigabyte malformed record at startup.
            child.send_wait(b"\x1b[A", b"sparse-newest-995", start=len(child.buf))
            child.send_wait(b"\x03", b"^C\r\n", start=len(child.buf))
            child.send_wait(b"\x12never-present-995", b"5': ", start=len(child.buf))
            start = len(child.buf)
            child.send_wait(b"\x07draft-still-live-995", b"draft-still-live-995", start=start, timeout=0.5)
            child.send_wait(b"\x03", b"^C\r\n", start=len(child.buf))
            child.send(b"\x04")
            child.finish(expect_resume=False)
        assert history.stat().st_size == size
    finally:
        history.write_bytes(saved)


def test_session_prompt_history_isolation():
    marker_a = b"history-isolation-first-831"
    marker_b = b"history-isolation-second-831"
    first = Child([], DEFAULT_IDLE_PROMPT)
    second = Child([], DEFAULT_IDLE_PROMPT)
    try:
        before = session_ids()
        second.send_wait_idle(marker_b + b"\r", b"fixture answer", start=len(second.buf))
        second_id = new_session(before)
        before = session_ids()
        first.send_wait_idle(marker_a + b"\r", b"fixture answer", start=len(first.buf))
        first_id = new_session(before)
        start = len(second.buf)
        second.send(b"\x1b[A")
        second.drain(0.4)  # Includes the engine/editor history snapshot handoff.
        assert marker_b in second.buf[start:], bytes(second.buf[start:])
        assert marker_a not in second.buf[start:], bytes(second.buf[start:])
        second.send_wait(b"\x03", b"^C\r\n", start=len(second.buf))
        start = len(second.buf)
        second.send(b"\x12" + marker_a)
        second.drain(0.4)
        assert b"failed reverse-i-search" in second.buf[start:], bytes(second.buf[start:])
        assert b"': " + marker_a not in second.buf[start:], bytes(second.buf[start:])
        second.send(b"\x07")
        first.exit_now()
        global_history = Path(DOTDIR, "prompt_history").read_text()
        assert marker_a.decode() in global_history and marker_b.decode() not in global_history
        start = len(second.buf)
        second.send(b"\x1b[A")
        second.drain(0.4)
        assert marker_a not in second.buf[start:], bytes(second.buf[start:])
        second.send_wait(b"\x03", b"^C\r\n", start=len(second.buf))
        second.exit_now()
    finally:
        first.kill()
        second.kill()
    for sid, own, other in ((first_id, marker_a, marker_b), (second_id, marker_b, marker_a)):
        path = Path(DOTDIR, "sessions", sid, "prompt_history")
        text = path.read_text()
        assert own.decode() in text and other.decode() not in text
        assert path.stat().st_mode & 0o777 == 0o600
        with Child(["--resume", sid], ready=DEFAULT_ACCOUNTED_IDLE_PROMPT) as resumed:
            start = len(resumed.buf)
            resumed.send(b"\x12" + other)
            resumed.wait(b"': " + other, start=start)
            resumed.send(b"\x07")
            resumed.exit_now()
    with Child([], DEFAULT_IDLE_PROMPT) as fresh:
        start = len(fresh.buf)
        fresh.send(b"\x12" + marker_b)
        fresh.wait(b"': " + marker_b, start=start)
        fresh.send(b"\x07")
        fresh.exit_now(expect_resume=False)
    records = Path(DOTDIR, "prompt_history").read_text().splitlines()
    assert records.count(marker_a.decode()) == 1
    assert records.count(marker_b.decode()) == 1


def test_session_prompt_history_exit_and_crash():
    for abrupt in (False, True):
        marker = f"history-local-{'crash' if abrupt else 'signal'}-945".encode()
        with Child([], DEFAULT_IDLE_PROMPT) as child:
            before = session_ids()
            child.send_wait_idle(marker + b"\r", b"fixture answer", start=len(child.buf))
            sid = new_session(before)
            local = Path(DOTDIR, "sessions", sid, "prompt_history")
            assert marker.decode() in local.read_text()
            if abrupt:
                child.kill()
            else:
                os.kill(child.pid, signal.SIGTERM)
                child.finish(expected=128 + signal.SIGTERM)
        merged = marker.decode() in Path(DOTDIR, "prompt_history").read_text()
        assert merged == (not abrupt), (abrupt, merged)
        with Child(["--resume", sid], ready=DEFAULT_ACCOUNTED_IDLE_PROMPT) as resumed:
            start = len(resumed.buf)
            resumed.send(b"\x12" + marker)
            resumed.wait(b"': " + marker, start=start)
            resumed.send(b"\x07")
            resumed.exit_now()
        # Resume does not replay an old local file into the global archive.
        assert (marker.decode() in Path(DOTDIR, "prompt_history").read_text()) == merged


def test_prompt_history_and_reverse_search():
    history = Path(DOTDIR) / "prompt_history"
    first = Child([], DEFAULT_IDLE_PROMPT)
    for entry in (
        b"history-repeat-old",
        b"history-repeat-new",
        "history-café-unique".encode(),
    ):
        start = len(first.buf)
        answer = first.send_wait_idle(entry + b"\r", b"fixture answer", start=start)
    first.exit_cleanly(answer)

    before_second = session_ids()
    second = Child([], DEFAULT_IDLE_PROMPT)
    assert session_ids() == before_second
    second.send(b"draft-restore")
    # The terminal may reuse the existing trailing blank instead of emitting it.
    second.send_wait(b"\x12", b"(failed reverse-i-search)`draft-restore':")
    second.send(b"\x07")
    restored = len(second.buf)
    answer = second.send_wait_idle(b"\r", b"fixture answer", start=restored)
    second_id = new_session(before_second)
    assert one(events(second_id), "turn_started")["data"]["text"] == "draft-restore"
    cancel = len(second.buf)
    cancel_end = second.send_wait(b"draft-cancel\x03", b"^C\r\n", start=cancel)

    search = len(second.buf)
    second.send_wait(b"\x12history-repeat", b"t': history-repeat-new",
                start=search)
    older = len(second.buf)
    second.send_wait(b"\x12", b"old",
                start=older)
    answer = second.send_wait_idle(b"\r", b"fixture answer", start=search)

    search = len(second.buf)
    second.send_wait(b"\x12" + "history-café-uniqueX".encode(), "failed reverse-i-search)`history-café-uniqueX':".encode(),
                start=search)
    second.send_wait(b"\x7f",
        "reverse-i-search)`history-café-unique': history-café-unique".encode(),
        start=search,
    )
    accepted = len(second.buf)
    second.send_wait(b"\x1b", DEFAULT_ACCOUNTED_IDLE_PROMPT + "history-café-unique".encode(),
                start=accepted)
    second.send_wait(b"\x03", b"^C\r\n", start=accepted)

    invalid_end = second.send_wait_idle(b"/history-invalid-command\r", b"unknown slash command")
    second.send(b"\r")
    second.send_wait(b"history-cancelled-draft\x03", b"^C\r\n", start=invalid_end)
    confirm = second.send_wait(b"/delete\r", b"delete is irreversible")
    second.wait(PROMPT.rstrip(), start=confirm)
    mismatch = second.send_wait_idle(b"history-confirmation-excluded\r", b"delete confirmation did not match", start=confirm)

    confirm = second.send_wait(b"/delete\r", b"delete is irreversible", start=mismatch)
    second.wait(PROMPT.rstrip(), start=confirm)
    cancel = len(second.buf)
    cancel_end = second.send_wait(b"confirmation-cancelled-draft\x03", b"^C\r\n", start=cancel)
    prompt_end = second.wait(DEFAULT_ACCOUNTED_IDLE_PROMPT, start=cancel_end)
    cancelled = bytes(second.buf[cancel:prompt_end])
    assert_bytes_in_order(cancelled, b"confirmation-cancelled-draft")
    assert b"delete cancelled" not in cancelled
    assert cancelled.count(DEFAULT_ACCOUNTED_IDLE_PROMPT.rstrip()) == 1, cancelled

    run = subprocess.run(
        [BINARY, "--dotdir", DOTDIR, "-e", "--",
         "history-noninteractive-excluded"],
        cwd=WORKSPACE,
        capture_output=True,
        timeout=8,
        check=False,
    )
    assert run.returncode == 0, (run.stdout, run.stderr)
    second.exit_now()

    assert history.stat().st_mode & 0o777 == 0o600
    records = history.read_text(encoding="utf-8").splitlines()
    assert records.count("history-repeat-old") == 2
    assert records.count("history-repeat-new") == 1
    assert records.count("history-café-unique") == 1
    assert records.count("/history-invalid-command") == 1
    assert "draft-restore" in records
    assert "draft-cancel" not in records
    assert "history-cancelled-draft" not in records
    assert "history-confirmation-excluded" not in records
    assert "confirmation-cancelled-draft" not in records
    assert "history-noninteractive-excluded" not in records


def test_multiline_and_paste():
    child = Child([], PROMPT.rstrip())
    first_end = child.send_wait(b"line one\nline two\r", b"fixture answer")
    child.wait(DEFAULT_ACCOUNTED_IDLE_PROMPT, start=first_end)
    answer_end = child.send_wait(b"\x1b[200~ping\x1b[201~\r", b"pong")
    child.exit_cleanly(answer_end)

    turns = [item for item in events(child.session_id())
             if item["type"] == "turn_started"]
    assert [item["data"]["text"] for item in turns] == ["line one\nline two", "ping"]


def test_network_input_recovery_boundaries():
    for cut_type in ("irc_event", "irc_admitted"):
        port = free_port()
        args = ["--listen", f"127.0.0.1:{port}", "--no-client",
                "-n", "recoveryagent", "-o", "recoveryop", "-r", "lab"]
        with Child(args, chat_prompt("recoveryop")) as child:
            peer = IRCClient(port, "peer")
            try:
                peer.message("recoveryagent: network_zero")
                child.send_wait(b"/rollout\r", b"network zero local only")
                sid = child.session_id()
                child.exit_cleanly(0)
            finally:
                peer.close()
        path = STATE_ROOT / sid / "events.jsonl"
        lines = path.read_bytes().splitlines(keepends=True)
        log = [json.loads(line) for line in lines]
        received = next(e for e in log if e["type"] == "irc_event" and
                        "recoveryagent: network_zero" in e["data"]["text"])
        admitted = next(e for e in log if e["type"] == "irc_admitted" and
                        received["seq"] in e["data"]["sequences"])
        assert admitted["data"]["input"]["text"] and received["data"]["urgent"]
        cut = received if cut_type == "irc_event" else admitted
        path.write_bytes(b"".join(lines[:cut["seq"]]))
        with Child([*args, "--resume", sid], ready=None) as resumed:
            resumed.wait(chat_prompt("recoveryop"))
            resumed.send(b"/rollout\r")
            end = resumed.wait(b"network zero local only")
            resumed.exit_cleanly(end)
        log = events(sid)
        matches = [e for e in log if e["type"] == "irc_admitted" and
                   received["seq"] in e["data"]["sequences"]]
        assert len(matches) == 1, matches
        assert len([e for e in log if e["type"] == "turn_started"]) == 1
        assert len([e for e in log if e["type"] == "turn_completed"]) == 1


def test_deferred_controls_in_admission_order():
    root = Path(os.environ["SNAJPAGENT_TEST_ROOT"])
    marker = root / "deferred-editor-started"
    editor = root / "deferred-editor"
    editor.write_text("#!/bin/sh\nprintf x >> '" + str(marker) + "'\n", encoding="utf-8")
    editor.chmod(0o700)
    with Child([], PROMPT.rstrip(), env=dict(os.environ, EDITOR=str(editor))) as child:
        child.send_wait(b"engine_blocked\r", b"engine-block-start")
        child.send(b"/model cache\r/config\r\x04")
        child.wait(RESUME_HEADER, timeout=5)
        child.finish()
        sid = child.session_id()
    assert not marker.exists()
    log = events(sid)
    requested = [e["data"]["control"] for e in log if e["type"] == "control_requested"]
    assert requested == [2, 1], requested
    with Child(["--resume", sid], ready=None, env=dict(os.environ, EDITOR=str(editor))) as child:
        child.wait(b"configuration unchanged")
        child.wait_idle_prompt()
        child.exit_now()
    log = events(sid)
    assert [e["data"]["control"] for e in log if e["type"] == "control_started"] == requested
    assert [e["data"]["control"] for e in log if e["type"] == "control_finished"] == requested
    assert marker.read_text() == "x"


def test_archive_control_completion_recovery():
    for cut_type in ("session_archived", "control_finished"):
        with Child([], PROMPT.rstrip()) as child:
            child.send_wait(b"engine_blocked\r", b"engine-block-start")
            child.send(b"/archive\r")
            child.wait(b"session archived")
            child.finish()
            sid = child.session_id()
        path = STATE_ROOT / sid / "events.jsonl"
        lines = path.read_bytes().splitlines(keepends=True)
        log = [json.loads(line) for line in lines]
        cut = one(log, cut_type)
        assert one(log, "control_requested")["data"]["control"] == 8
        # Inactive private session, cut at the actual writer's durable boundary.
        path.write_bytes(b"".join(lines[:cut["seq"]]))
        with Child(["--resume", sid]) as child:
            child.wait_idle_prompt()
            child.exit_now()
        log = events(sid)
        one(log, "session_archived")
        one(log, "session_unarchived")
        one(log, "control_finished")


def test_exit_preserves_pending_submission():
    with Child([], DEFAULT_IDLE_PROMPT) as child:
        child.send_wait(b"engine_blocked\r", b"engine-block-start")
        # The UI receives both while the engine is in the bounded stall.
        child.send(b"/queue ping\r\x04")
        child.wait(RESUME_HEADER, timeout=5)
        child.finish()
        session_id = child.session_id()
    log = events(session_id)
    assert one(log, "future_turn_queued")["data"]["text"] == "ping", log
    assert len([e for e in log if e["type"] == "turn_started"]) == 1
    with Child(["--resume", session_id], ready=None) as resumed:
        answer = resumed.wait(b"pong")
        resumed.exit_cleanly(answer)
    turns = [e["data"] for e in events(session_id) if e["type"] == "turn_started"]
    assert len(turns) == 2 and turns[1]["text"] == "ping"


def test_input_survives_preparation_failure():
    # Failure before turn_started must leave the submitted intent recoverable.
    agents = Path(WORKSPACE) / "AGENTS.md"
    assert not agents.exists()
    agents.mkdir()
    try:
        with Child([], DEFAULT_IDLE_PROMPT) as child:
            child.send_wait(b"/ro ping\r", b"must be a non-symlink regular file")
            session_id = child.session_id()
            log = events(session_id)
            accepted = [e for e in log if e["type"] == "input_received"]
            assert len(accepted) == 1, log
            assert accepted[0]["data"]["text"] == "ping"
            assert accepted[0]["data"]["read_only"] is True
            assert not any(e["type"] == "turn_started" for e in log)
            child.kill()
    finally:
        agents.rmdir()
    with Child(["--resume", session_id], ready=None) as resumed:
        answer = resumed.wait(b"pong")
        resumed.exit_cleanly(answer)
    turns = [e["data"] for e in events(session_id) if e["type"] == "turn_started"]
    assert len(turns) == 1 and turns[0]["text"] == "ping" and turns[0]["read_only"]
    with Child(["--resume", session_id], PROMPT.rstrip()) as resumed:
        resumed.exit_now()
    assert len([e for e in events(session_id) if e["type"] == "turn_started"]) == 1

def test_resume_keeps_original_instruction_paths():
    root = Path(os.environ["SNAJPAGENT_TEST_ROOT"])
    work = root / "instruction-recovery"
    work.mkdir()
    entry = work / "AGENTS.md"
    entry.write_text("Original entry point.\n", encoding="utf-8")
    with Child(["-C", str(work)], PROMPT.rstrip()) as child:
        child.send_wait(b"queue_slow\r", b"working slowly")
        sid = child.session_id()
        child.kill()
    # The recorded pointer remains context, while new work rediscovers its own
    # paths. No file contents or private reasoning are saved automatically.
    entry.rename(work / "moved-notes.md")
    with Child(["--resume", sid], ready=None) as child:
        end = child.wait(b"fixture answer")
        child.exit_cleanly(end)
    turns = [e for e in events(sid) if e["type"] == "turn_started"]
    assert len(turns) == 1 and str(entry) in turns[0]["data"]["instructions"]
    assert one(events(sid), "turn_completed")



def test_durable_queue_scheduling():
    # Armed future work continues after the original turn, including a crash
    # after /next acknowledgement. Idle additions stay paused until /next.
    for arm in (False, True):
        with Child([], DEFAULT_IDLE_PROMPT) as child:
            end = child.send_wait(b"/queue ping\r", b"queued (/next or /q c)")
            child.wait_idle_prompt(start=end)
            session_id = child.session_id()
            if arm:
                child.send_wait(b"queue_slow\r", b"working slowly", start=len(child.buf))
                child.send_wait(b"/next\r", b"queued work armed", start=len(child.buf))
            child.kill()
        with Child(["--resume", session_id], ready=None) as resumed:
            if arm:
                end = resumed.wait(b"pong")
            else:
                resumed.wait(b"queued future turns are paused")
                resumed.drain(.2)
                assert b"pong" not in resumed.buf
                end = resumed.send_wait(b"/next\r", b"pong", start=len(resumed.buf))
            resumed.exit_cleanly(end)
        log = events(session_id)
        turns = [e["data"] for e in log if e["type"] == "turn_started"]
        assert len(turns) == (2 if arm else 1), turns
        assert turns[-1]["input_kind"] == "queued" and turns[-1]["text"] == "ping"
        with Child(["--resume", session_id], PROMPT.rstrip()) as resumed:
            resumed.exit_now()
        assert len([e for e in events(session_id) if e["type"] == "turn_started"]) == len(turns)


def test_resume_preserves_armed_fifo():
    child = Child([], PROMPT.rstrip())
    child.send_wait(b"slow\r", b"working slowly")
    child.send_wait(b"/queue ping\r", b"queued (/next or /q c) " + PROMPT + b"ping")
    session_id = child.session_id()
    child.kill()

    resumed = Child(["--resume", session_id], b"1 queued armed")
    answer_end = resumed.wait(b"pong")
    resumed.wait(DEFAULT_ACCOUNTED_IDLE_PROMPT, start=answer_end)
    resumed.exit_cleanly(answer_end)

    log = events(session_id)
    turns = [item for item in log if item["type"] == "turn_started"]
    assert len(turns) == 2
    assert turns[1]["data"]["input_kind"] == "queued"
    assert one(log, "response_interrupted")["data"]["origin"] == "recovery"
    assert not [e for e in log if e["type"] == "turn_interrupted"]
    assert len([e for e in log if e["type"] == "turn_completed"]) == 2


def test_goal_quoted_reserved_wording():
    child = Child([], PROMPT.rstrip())
    error_end = child.send_wait(b"/goal pause after release\r", b"reserved /goal command has extra text")
    child.wait(PROMPT.rstrip(), start=error_end)
    answer_end = child.send_wait(b'/goal "pause after release"\r', b"goal done")
    status_end = child.send_wait(b"/goal\r", b": completed", start=answer_end)
    wording_end = child.wait(b"pause after release", start=status_end)
    child.wait(PROMPT.rstrip(), start=wording_end)
    child.exit_now()

    log = events(child.session_id())
    started = one(log, "goal_started")
    completed = one(log, "goal_completed")
    turns = [item for item in log if item["type"] == "turn_started"]
    assert started["data"]["prompt"] == "pause after release"
    assert completed["data"]["actor"] == "model"
    assert [item["data"]["input_kind"] for item in turns] == ["goal"]


def test_goal_automatic_continuation():
    child = Child([], PROMPT.rstrip())
    checkpoint_end = child.send_wait(b"/goal automatic goal\r", b"goal checkpoint")
    answer_end = child.wait(b"goal done", start=checkpoint_end)
    child.exit_cleanly(answer_end)

    log = events(child.session_id())
    turns = [item for item in log if item["type"] == "turn_started"]
    assert [item["data"]["input_kind"] for item in turns] == ["goal", "goal"]
    assert all(item["data"]["text"] ==
               "Continue the active goal from its durable state."
               for item in turns)
    assert one(log, "goal_completed")["data"]["actor"] == "model"


def test_model_created_goal_continuation():
    child = Child([], PROMPT.rstrip())
    started_end = child.send_wait(b"please create a persistent goal\r", GOAL_SET)
    checkpoint_end = child.wait(b"model-created checkpoint", start=started_end)
    cleared_end = child.wait(GOAL_CLEARED, start=checkpoint_end)
    answer_end = child.wait(b"goal done", start=cleared_end)
    child.exit_cleanly(answer_end)

    log = events(child.session_id())
    started = one(log, "goal_started")
    turns = [item for item in log if item["type"] == "turn_started"]
    assert started["data"]["prompt"] == "model-created goal"
    assert [item["data"]["input_kind"] for item in turns] == ["direct", "goal"]
    assert one(log, "goal_completed")["data"]["actor"] == "model"


def test_goal_configured_wording_limit():
    config = write_config("goal-limit.ini", "[provider openai]\n[agent]\nmax_goal_prompt_bytes = 4\n")
    child = Child(["--config", str(config)], PROMPT.rstrip())
    error_end = child.send_wait(b"/goal abcde\r", b"goal wording must contain 1..4 UTF-8 bytes")
    child.wait(PROMPT.rstrip(), start=error_end)
    answer_end = child.send_wait(b"/goal tiny\r", b"goal done")
    child.exit_cleanly(answer_end)

    log = events(child.session_id())
    assert one(log, "goal_started")["data"]["prompt"] == "tiny"
    assert len([item for item in log if item["type"] == "goal_reworded"]) == 0
    assert one(log, "goal_completed")["data"]["actor"] == "model"


def test_goal_model_rewrite_and_lock():
    child = Child([], PROMPT.rstrip())
    set_end = child.send_wait(b"/goal rewrite goal\r", GOAL_SET)
    rewritten_end = child.wait(GOAL_UPDATED, start=set_end)
    cleared_end = child.wait(GOAL_CLEARED, start=rewritten_end)
    answer_end = child.wait(b"goal done", start=cleared_end)
    child.exit_cleanly(answer_end)
    log = events(child.session_id())
    reworded = one(log, "goal_reworded")
    assert reworded["data"] == {
        "actor": "model",
        "goal_id": one(log, "goal_started")["data"]["goal_id"],
        "prompt": "rewritten goal",
    }

    child = Child([], PROMPT.rstrip())
    child.send_wait(b"/goal locked goal\r", b"preparing goal rewrite")
    lock_end = child.send_wait(b"/goal lock\r", b"Goal wording locked against model changes")
    answer_end = child.wait(b"goal done", start=lock_end)
    child.exit_cleanly(answer_end)
    log = events(child.session_id())
    assert len([item for item in log if item["type"] == "goal_reworded"]) == 0
    assert one(log, "goal_lock_changed")["data"]["locked"] is True
    assert one(log, "goal_completed")["data"]["actor"] == "model"


def test_goal_interrupt_prompt_state():
    # Final captures can hide a stale flag by overwriting it later. Check every
    # emitted idle prompt after Ctrl-C, including before the paused notice.
    config = write_config("goal-interrupt-prompt.ini",
        "[provider openai]\n[ui]\n"
        "prompt = {goal_spinner}{chat:C>}{rollout-idle:I>}{rollout-active:A>}\n")
    for term in ("xterm", "dumb"):
        with Child(["--config", str(config)], ready=b"I>", term=term) as child:
            child.send_wait(b"/goal slow goal\r", b"working on goal")
            if term != "dumb":
                # The raw composer owns draft bytes. In TERM=dumb the kernel
                # retains unsubmitted input, and Ctrl-C arrives as SIGINT.
                child.send_wait(b"keep working", b"keep working")
                cleared = child.send_wait(b"\x03", b"^C\r\n")
                child.drain(0.05)
                assert "⚑A>".encode() in child.buf[cleared:]
                assert not any(e["type"] == "goal_paused" for e in events(child.session_id()))
            start = len(child.buf)
            child.send(b"\x03")
            paused = child.wait(b"Goal paused at the current turn boundary", start=start)
            child.drain(0.15)
            output = bytes(child.buf[start:])
            assert "⚑I>".encode() not in output, output
            assert "⚑".encode() not in child.buf[paused:], bytes(child.buf[paused:])
            blank = len(child.buf)
            child.send(b"\r" * 4)
            child.drain(0.15)
            assert child.buf[blank:].count(b"\n") >= 4
            assert "⚑".encode() not in child.buf[blank:]
            child.exit_now()
        log = events(child.session_id())
        assert len([e for e in log if e["type"] == "turn_started"]) == 1
        assert len([e for e in log if e["type"] == "goal_paused"]) == 1
        assert not any(e["type"] in ("input_received", "goal_resumed") for e in log)
        assert not any(e["data"].get("text") == "Continue." for e in log)


def test_goal_pause_resume_and_queue_priority():
    child = Child([], PROMPT.rstrip())
    child.send_wait(b"/goal slow goal\r", b"working on goal")
    pause_end = child.send_wait(b"/goal pause\r", b"Goal paused at the current turn boundary")
    checkpoint_end = child.wait(b"goal checkpoint", start=pause_end)
    child.wait(PROMPT.rstrip(), start=checkpoint_end)
    child.drain(0.2)
    assert b"goal done" not in child.buf[checkpoint_end:]
    answer_end = child.send_wait(b"/goal resume\r", b"goal done", start=checkpoint_end)
    child.exit_cleanly(answer_end)
    log = events(child.session_id())
    assert one(log, "goal_paused")["data"]["reason"] == "user"
    one(log, "goal_resumed")
    turns = [item for item in log if item["type"] == "turn_started"]
    assert [item["data"]["input_kind"] for item in turns] == ["goal", "goal"]

    child = Child([], PROMPT.rstrip())
    child.send_wait(b"/goal slow goal\r", b"working on goal")
    child.send_wait(b"ping\t", b"queued (/next or /q c) " + PROMPT + b"ping")
    child.send_wait(b"/ro repeat\t", b"queued (/next or /q c) " + PROMPT + b"/ro repeat")
    checkpoint_end = child.send_wait(b"ping\t", b"goal checkpoint")
    pong_end = child.wait(b"pong", start=checkpoint_end)
    pong_end = child.wait(b"haha", start=pong_end)
    pong_end = child.wait(b"pong", start=pong_end)
    answer_end = child.wait(b"goal done", start=pong_end)
    child.exit_cleanly(answer_end)
    log = events(child.session_id())
    turns = [item for item in log if item["type"] == "turn_started"]
    assert [item["data"]["input_kind"] for item in turns] == [
        "goal", "queued", "queued", "queued", "goal"
    ]


def test_goal_clear_controls_continuation():
    for state in ("active", "paused", "blocked"):
        with Child([], ready=DEFAULT_IDLE_PROMPT) as child:
            if state == "blocked":
                end = child.send_wait_idle(b"/goal blocked goal\r", b"goal done")
            else:
                end = child.send_wait(b"/goal slow goal\r", b"working on goal")
                if state == "paused":
                    end = child.send_wait(b"/goal pause\r", b"Goal paused at the current turn boundary", start=end)
                    end = child.wait_idle_prompt(start=end)
            end = child.send_wait(b"/goal clear\r", GOAL_CLEARED, start=end)
            end = child.wait_idle_prompt(start=end)
            end = child.send_wait_idle(b"/goal status\r", b": cancelled", start=end)
            session_id = child.session_id()
            log = events(session_id)
            cancelled = one(log, "goal_cancelled")
            assert cancelled["data"]["goal_id"] == one(log, "goal_started")["data"]["goal_id"]
            assert not [e for e in log if e["type"] == "goal_reworded"]
            assert len([e for e in log if e["type"] == "turn_started"]) == 1
            end = child.send_wait_idle(b"/goal resume\r", b"only a paused or blocked goal", start=end)
            child.exit_now()
        before = len(events(session_id))
        with Child(["--resume", session_id], ready=DEFAULT_ACCOUNTED_IDLE_PROMPT) as child:
            end = child.send_wait_idle(b"/goal\r", b": cancelled")
            child.drain(0.15)
            assert not [e for e in events(session_id)[before:] if e["type"] == "turn_started"]
            end = child.send_wait_idle(b"/goal automatic goal\r", b"goal done", start=end)
            child.exit_now()
        assert len([e for e in events(session_id) if e["type"] == "goal_started"]) == 2


def test_goal_control_whitespace():
    with Child([], ready=DEFAULT_IDLE_PROMPT) as child:
        end = child.send_wait(b"/goal slow goal\r", b"working on goal")
        # Paste keeps tabs literal rather than invoking editor completion.
        end = child.send_wait(b"\x1b[200~/goal\t pause \t\x1b[201~\r",
                              b"Goal paused at the current turn boundary", start=end)
        end = child.wait_idle_prompt(start=end)
        end = child.send_wait_idle(b"/goal   status   \r", b": paused", start=end)
        end = child.send_wait_idle(b"/goal    \r", b": paused", start=end)
        end = child.send_wait_idle(b"/goal help   \r", b"clear=cancel", start=end)
        end = child.send_wait_idle(b"/goal clear extra\r", b"reserved /goal command has extra text", start=end)
        end = child.send_wait_idle(b"/goal pause extra\r", b"reserved /goal command has extra text", start=end)
        end = child.send_wait_idle(b"/goal resume extra\r", b"reserved /goal command has extra text", start=end)
        end = child.send_wait_idle(b"/goal lock   \r", b"Goal wording locked", start=end)
        end = child.send_wait_idle(b"/goal unlock   \r", b"Goal wording unlocked", start=end)
        end = child.send_wait_idle(b"\x1b[200~/goal\tresume\t \x1b[201~\r", b"goal done", start=end)
        child.exit_now()
    log = events(child.session_id())
    one(log, "goal_paused")
    one(log, "goal_resumed")
    assert [e["data"]["input_kind"] for e in log if e["type"] == "turn_started"] == ["goal", "goal"]
    assert not [e for e in log if e["type"] == "goal_reworded"]


    with Child([], ready=DEFAULT_IDLE_PROMPT) as child:
        end = child.send_wait_idle(b"/goal blocked goal\r", b"goal done")
        end = child.send_wait_idle(b"/goal set resumed goal\r", GOAL_UPDATED, start=end)
        end = child.send_wait_idle(b"/goal resume   \r", b"goal done", start=end)
        child.exit_now()
    log = events(child.session_id())
    one(log, "goal_blocked")
    one(log, "goal_resumed")
    one(log, "goal_completed")


def test_goal_clear_without_goal_and_reserved_wording():
    with Child([], ready=DEFAULT_IDLE_PROMPT) as child:
        end = child.send_wait_idle(b"/goal clear\r", b"no unfinished goal can be cancelled")
        assert session_ids() == child.sessions_before
        end = child.send_wait_idle(b"/goal blocked goal\r", b"goal done", start=end)
        end = child.send_wait_idle(b'/goal "clear the build directory"\r', GOAL_UPDATED, start=end)
        end = child.send_wait_idle(b"/goal set clear the cache\r", GOAL_UPDATED, start=end)
        end = child.send_wait_idle(b"/goal clear   \r", GOAL_CLEARED, start=end)
        end = child.send_wait_idle(b"/goal clear\r", b"no unfinished goal can be cancelled", start=end)
        child.exit_now()
    log = events(child.session_id())
    assert [e["data"]["prompt"] for e in log if e["type"] == "goal_reworded"] == [
        "clear the build directory", "clear the cache"]
    one(log, "goal_cancelled")


def test_goal_user_terminal_commands_and_unlock():
    child = Child([], PROMPT.rstrip())
    set_end = child.send_wait(b"/goal slow goal\r", GOAL_SET)
    child.wait(b"working on goal", start=set_end)
    reworded_end = child.send_wait(b"/goal set retitled goal\r", GOAL_UPDATED, start=set_end)
    child.send_wait(b"/goal lock\r", b"Goal wording locked against model changes",
               start=reworded_end)
    child.send_wait(b"/goal unlock\r", b"Goal wording unlocked for model changes")
    complete_end = child.send_wait(b"/goal complete\r", GOAL_CLEARED, start=reworded_end)
    checkpoint_end = child.wait_idle_prompt(start=complete_end)

    set_end = child.send_wait(b"/goal slow goal\r", GOAL_SET, start=checkpoint_end)
    child.wait(b"working on goal", start=set_end)
    cancel_end = child.send_wait(b"/goal cancel\r", GOAL_CLEARED, start=set_end)
    child.exit_cleanly(cancel_end)

    log = events(child.session_id())
    completed = one(log, "goal_completed")
    assert completed["data"]["actor"] == "user"
    one(log, "goal_cancelled")
    assert one(log, "goal_reworded")["data"]["prompt"] == "retitled goal"
    locks = [item["data"]["locked"] for item in log
             if item["type"] == "goal_lock_changed"]
    assert locks == [True, False]
    # Feedback may split streamed chunks; completion is a durable assertion.
    responses = [item for item in log if item["type"] == "response_completed"]
    assert [part["text"] for item in responses for part in item["data"]["items"]
            if part["provider_item_id"] == "msg_fixture_goal_checkpoint"] == [
        "goal checkpoint", "goal checkpoint"
    ]


def test_goal_refusal_failure_block_and_restart_state():
    child = Child([], PROMPT.rstrip())
    child.send_wait(b"/goal refusing goal\r", b"I cannot continue this goal.")
    paused = child.wait(b"Goal paused after model refusal")
    child.exit_cleanly(paused)
    log = events(child.session_id())
    assert one(log, "goal_paused")["data"]["reason"] == "refusal"
    assert not [item for item in log if item["type"] == "turn_recovery"]

    child = Child([], PROMPT.rstrip())
    child.send_wait(b"/goal failing goal\r", b"fixture goal provider failed")
    child.wait(b"Goal active; retrying")
    done = child.wait(b"goal done", timeout=20.0)
    child.exit_cleanly(done)
    log = events(child.session_id())
    assert not [e for e in log if e["type"] == "goal_paused"]
    assert len([e for e in log if e["type"] == "turn_recovery"]) == 4
    assert len([e for e in log if e["type"] == "turn_started"]) == 1
    one(log, "goal_completed")

    child = Child([], PROMPT.rstrip())
    child.send_wait(b"/goal blocked goal\r", b"Goal blocked by model")
    answer_end = child.wait(b"goal done")
    status_end = child.send_wait(b"/goal\r", b": blocked", start=answer_end)
    blocker_end = child.wait(b"fixture dependency is unavailable", start=status_end)
    child.wait(PROMPT.rstrip(), start=blocker_end)
    child.exit_now()
    log = events(child.session_id())
    assert one(log, "goal_blocked")["data"]["reason"] == \
        "fixture dependency is unavailable"

    child = Child([], PROMPT.rstrip())
    child.send_wait(b"/goal slow goal\r", b"working on goal")
    session_id = child.session_id()
    child.kill()
    resumed = Child(["--resume", session_id])
    status_end = resumed.wait(b": active")
    restored = resumed.wait(b"slow goal", start=status_end)
    answer = resumed.wait(b"goal done", start=restored)
    resumed.exit_cleanly(answer)
    log = events(session_id)
    goal_id = one(log, "goal_started")["data"]["goal_id"]
    assert one(log, "goal_completed")["data"]["goal_id"] == goal_id
    assert not [item for item in log if item["type"] in ("goal_paused", "goal_resumed")]
    assert [item["data"]["input_kind"] for item in log if item["type"] == "turn_started"] == ["goal", "goal"]


def test_saved_goal_restored_without_lookup():
    with Child([], ready=DEFAULT_IDLE_PROMPT) as child:
        child.send_wait(b"/goal slow goal\r", b"working on goal")
        paused = child.send_wait(b"/goal pause\r", b"Goal paused at the current turn boundary")
        checkpoint = child.wait(b"goal checkpoint", start=paused)
        child.wait_idle_prompt(start=checkpoint)
        wording = "obnovit žluťoučký plán bez opakování"
        start = len(child.buf)
        changed = child.send_wait(("/goal set " + wording + "\r").encode(), GOAL_UPDATED, start=start)
        locked = child.send_wait_idle(b"/goal lock\r", b"Goal wording locked against model changes", start=changed)
        child.exit_now()
    session_id = child.session_id()
    original = events(session_id)
    goal_id = one(original, "goal_started")["data"]["goal_id"]
    config = write_config("goal-resume.ini",
        "[ui]\nresume_history_turns = 0\n"
        "[provider openai]\nbase_url = http://127.0.0.1:1/v1\n"
        "api_key = fixture-only\n")
    for selector in ([session_id], ["--last"]):
        with Child(["--config", str(config), "--resume", *selector]) as resumed:
            status = resumed.wait(f"goal {goal_id[:8]}: paused · wording locked".encode())
            restored = resumed.wait(wording.encode(), start=status)
            assert b"revision: 2" in resumed.buf[status:restored]
            resumed.wait_idle_prompt(start=restored)
            resumed.drain(0.1)
            assert len(events(session_id)) == len(original)
            # A normal follow-up must not create a replacement or resume it.
            answer = resumed.send_wait(b"ping\r", b"pong", start=restored)
            resumed.exit_cleanly(answer)
        current = events(session_id)
        assert [e for e in current if e["type"].startswith("goal_")] == [
            e for e in original if e["type"].startswith("goal_")]
        original = current


def test_resume_preserves_inactive_and_queued_goal_states():
    for state in ("blocked", "completed", "cancelled"):
        with Child([], ready=DEFAULT_IDLE_PROMPT) as child:
            answer = child.send_wait_idle(b"/goal blocked goal\r", b"goal done")
            if state != "blocked":
                start = len(child.buf)
                cleared = child.send_wait_idle(b"/goal " + (b"complete" if state == "completed" else b"cancel") + b"\r", GOAL_CLEARED, start=start)
            child.exit_now()
        session_id = child.session_id()
        original = events(session_id)
        goal_id = one(original, "goal_started")["data"]["goal_id"]
        with Child(["--resume", session_id]) as resumed:
            restored = resumed.wait(f"goal {goal_id[:8]}: {state}".encode())
            if state == "blocked":
                restored = resumed.wait(b"blocker: fixture dependency is unavailable", start=restored)
            resumed.wait_idle_prompt(start=restored)
            resumed.drain(0.1)
            resumed.exit_now()
        assert events(session_id) == original

    with Child([], ready=DEFAULT_IDLE_PROMPT) as child:
        child.send_wait(b"/goal slow goal\r", b"working on goal")
        child.send_wait(b"ping\t", b"queued (/next or /q c) " + PROMPT + b"ping")
        session_id = child.session_id()
        child.send_wait(b"\x04", RESUME_HEADER, timeout=4.0)
        command = child.finish()
    original = events(session_id)
    with Child.from_command(command) as resumed:
        restored = resumed.wait(b": active")
        answer = resumed.wait(b"pong", start=restored)
        done = resumed.wait(b"goal done", start=answer)
        resumed.exit_cleanly(done)
    current = events(session_id)
    assert [e["data"]["input_kind"] for e in current if e["type"] == "turn_started"] == [
        "goal", "queued", "goal"]
    assert not [e for e in current if e["type"] in ("goal_paused", "goal_resumed")]


def test_queue_mutation_commands():
    child = Child([], PROMPT.rstrip())
    child.send_wait(b"queue_slow\r", b"working slowly")

    for text in (b"first", b"second", b"third"):
        child.send_wait(text + b"\t", b"queued (/next or /q c) " + PROMPT + text)

    start = len(child.buf)
    listed = child.send_wait(b"/q\r", b"3 ", start=start)
    child.wait(b"first", start=start)
    child.wait(b"second", start=start)
    child.wait(b"third", start=start)
    child.wait("»".encode(), start=listed)

    child.send_wait(b"/q p\r", b"1 future turn cancelled")
    child.send_wait(b"/q 1d\r", b"1 future turn cancelled")

    edit_start = child.send_wait(b"/q 1e\r", "edit 1 › ".encode())
    child.wait(b"second", start=edit_start)
    cancel_start = len(child.buf)
    child.send_wait(b"\x03", b"^C\r\n", start=cancel_start)
    child.wait("»".encode(), start=cancel_start)
    edit_start = child.send_wait(b"/q 1e\r", "edit 1 › ".encode(), start=cancel_start)
    child.wait(b"second", start=edit_start)
    child.send_wait(b" active\r", "› second active".encode(), start=edit_start)

    child.send_wait(b"fourth\t", b"queued (/next or /q c) " + PROMPT + b"fourth")
    interrupted_end = child.send_wait_idle(b"\x03", b"turn interrupted")

    edit_start = child.send_wait(b"/queue 1 edit\r", "edit 1 › ".encode(), start=interrupted_end)
    child.wait(b"second active", start=edit_start)
    child.send_wait(b" idle\r", "› second active idle".encode(), start=edit_start)

    child.send(b"/exit\r")
    assert child.reap() == 0

    session_id = child.session_id()
    resumed = Child(["--resume", session_id], b"2 queued paused")
    resumed.wait(b"/medium   ?% (2) \xe2\x80\xba ")
    resumed.wait(PROMPT.rstrip())
    start = len(resumed.buf)
    resumed.send_wait(b"/q\r", b"second active idle", start=start)
    resumed.wait(b"fourth", start=start)
    cleared_end = resumed.send_wait(b"/queue clear\r", b"2 future turns cancelled", start=start)
    empty_end = resumed.send_wait(b"/q\r", b"future-turn queue is empty", start=cleared_end)
    resumed.wait(PROMPT.rstrip(), start=empty_end)
    resumed.send(b"/exit\r")
    assert resumed.reap() == 0

    log = events(session_id)
    queued = [item for item in log if item["type"] == "future_turn_queued"]
    edited = [item for item in log if item["type"] == "future_turn_edited"]
    cancelled = [item for item in log
                 if item["type"] == "future_turn_cancelled"]
    assert [item["data"]["text"] for item in queued] == [
        "first", "second", "third", "fourth"
    ]
    assert [item["data"]["text"] for item in edited] == [
        "second active", "second active idle"
    ]
    assert all(item["data"]["queue_id"] == queued[1]["data"]["queue_id"]
               for item in edited)
    assert cancelled[0]["data"]["queue_ids"] == [
        queued[2]["data"]["queue_id"]
    ]
    assert cancelled[1]["data"]["queue_ids"] == [
        queued[0]["data"]["queue_id"]
    ]
    assert cancelled[2]["data"]["queue_ids"] == [
        queued[1]["data"]["queue_id"], queued[3]["data"]["queue_id"]
    ]


def test_preferences_and_verbosity():
    child = Child([], PROMPT.rstrip())

    for level in range(7):
        start = len(child.buf)
        end = child.send_wait(f"/verbose {level}  \r".encode(), f"verbosity: {level} (".encode(), start=start)
        child.wait(PROMPT.rstrip(), start=end)
    for value in ("7", "-1", "2x", "1 2", "99"):
        end = child.send_wait(f"/verbose {value}\r".encode(), b"/verbose expects one integer from 0 through 6", start=end)
        child.wait(PROMPT.rstrip(), start=end)
    end = child.send_wait(b"/verbose\r", b"verbosity: 6 (wire)", start=end)
    child.wait(PROMPT.rstrip(), start=end)

    end = child.send_wait(b"/effort quantum\r", b"effort for next turn: quantum")
    child.wait(PROMPT.rstrip(), start=end)

    end = child.send_wait(b"/verbose 4\r", b"verbosity: 4")
    child.wait(PROMPT.rstrip(), start=end)

    child.send_wait(b"ping\r", b"event \xe2\x80\xba")
    answer_end = child.wait(b"pong")
    terminal_end = child.wait(b"turn_completed synced", start=answer_end)
    child.exit_cleanly(terminal_end)

    log = events(child.session_id())
    effort = one(log, "model_selection_changed")
    turn = one(log, "turn_started")
    assert effort["data"] == {
        "old_provider": "openai", "new_provider": "openai",
        "old_model": DEFAULT_MODEL, "new_model": DEFAULT_MODEL,
        "old_effort": "default", "new_effort": "quantum"
    }
    assert turn["data"]["config"]["effort"] == "quantum"


def test_active_verbosity():
    for key in (b"\r", b"\t"):
        for initial, level in ((0, 3), (3, 0)):
            with Child(["-v"] * initial, ready=DEFAULT_IDLE_PROMPT) as child:
                child.send_wait(b"managed_command_queue\r", "⠋".encode())
                session = child.session_id()
                start = len(child.buf)
                after = child.send_wait(f"/verbose {level}".encode() + key, f"verbosity: {level} (".encode(),
                                   start=start, timeout=0.25)
                assert b"managed command queue complete" not in child.buf
                end = child.wait(b"managed command queue complete", start=after)
                child.exit_cleanly(end)
                output = b"fixture managed wait completed"
                assert (output in child.buf[after:]) == (level == 3)
                log = events(session)
                assert one(log, "turn_started")["data"]["text"] == "managed_command_queue"
                one(log, "turn_completed")
                result = one(log, "tool_finished")["data"]["result"]
                assert output.decode() in json.dumps(result)
                assert not any(e["type"] in (
                    "steering_added", "future_turn_queued", "response_interrupted"
                ) for e in log)


def test_runtime_verbosity_resume():
    for level in (0, 1, 6):
        with Child(["-vvv"], ready=DEFAULT_IDLE_PROMPT) as child:
            answered = child.send_wait_idle(b"ping\r", b"pong")
            end = child.send_wait_idle(f"/verbose {level}\r".encode(), f"verbosity: {level} (".encode())
            command = child.exit_now()
            assert shlex.split(command).count("-v") == level, command
        with Child.from_command(command) as resumed:
            resumed.wait_idle_prompt()
            end = resumed.send_wait_idle(b"/verbose\r", f"verbosity: {level} (".encode())
            resumed.exit_now()


def test_help_plain_terminal():
    with Child([], ready=DEFAULT_IDLE_PROMPT, term="dumb") as child:
        for command in (b"/help\r", b"/?\r", b"/goal help\r"):
            start = len(child.buf)
            child.send(command)
            child.wait(b"clear=cancel", start=start)
            child.drain(0.1)
            text = bytes(child.buf[start:])
            assert b"[optional]" in text and b"/goal [set] TEXT" in text
            assert b"\x1b" not in text
        child.exit_now(expect_resume=False)
        assert session_ids() == child.sessions_before


def test_command_name_completion():
    child = Child([], PROMPT.rstrip(), env=dict(os.environ, EDITOR="true"))

    start = len(child.buf)
    end = child.send_wait(b"/he\t", b"lp", start=start)
    help_end = child.send_wait(b"\r", b"/compact", start=end)
    child.wait(b"Empty Tab switch view", start=help_end)
    child.wait(b"Tab complete/indent/queue", start=help_end)
    child.drain()
    assert b"Enter submit/steer (chat: send)" in child.buf[end:]
    child.wait(PROMPT.rstrip(), start=help_end)

    start = len(child.buf)
    alias_end = child.send_wait(b"/?\r", b"/help", start=start)
    child.wait(b"/?", start=alias_end)
    alias_end = child.wait(b"/compact", start=alias_end)
    child.wait(b"Tab complete/indent/queue", start=alias_end)
    child.wait(PROMPT.rstrip(), start=alias_end)

    for prefix, command in (
        (b"/sta", b"/status"),
        (b"/hi", b"/history"),
        (b"/mo", b"/model"),
        (b"/ef", b"/effort"),
        (b"/go", b"/goal"),
        (b"/v", b"/verbose"),
        (b"/q", b"/queue"),
        (b"/ne", b"/next"),
        (b"/ar", b"/archive"),
        (b"/com", b"/compact"),
        (b"/conf", b"/config"),
        (b"/de", b"/delete"),
        (b"/ex", b"/exit"),
    ):
        start = len(child.buf)
        end = child.send_wait(prefix + b"\t", command + b" ", start=start)
        clear_draft_incrementally(child)

    for prefix in (b"/h", b"/c"):
        start = len(child.buf)
        child.send(prefix + b"\t")
        child.drain(0.08)
        assert b"/help" not in child.buf[start:] and b"/compact" not in child.buf[start:]
        child.send(b"\t")
        choices = (b"/help", b"/history") if prefix == b"/h" else (b"/compact", b"/config")
        for choice in choices:
            child.wait(choice, start=start)
        # The prefix also occurs inside choices; observe the repainted prompt.
        child.wait(PROMPT + prefix, start=child.buf.rfind(choices[-1]))
        clear_draft_incrementally(child)

    start = len(child.buf)
    end = child.send_wait(b"/mo gpt\x1b[D\x1b[D\x1b[D\x1b[D\t", b"del gpt", start=start)
    clear_draft_incrementally(child)

    start = len(child.buf)
    end = child.send_wait(b"/h\ti\t", b"/history", start=start)
    clear_draft_incrementally(child)

    start = len(child.buf)
    end = child.send_wait(b"x\t", b"x   ", start=start)
    edit = bytes(child.buf[start:end])
    assert b"\x1b[2K" not in edit, edit
    assert DEFAULT_IDLE_PROMPT not in edit, edit
    clear_draft_incrementally(child)

    child.send_wait(b"slow\r", b"working slowly")

    start = len(child.buf)
    end = child.send_wait(b"/he\t", b"lp", start=start)
    help_end = child.send_wait(b"\r", b"/compact", start=end)
    child.wait(b"Tab complete/indent/queue", start=help_end)
    child.wait("»".encode(), start=help_end)

    start = len(child.buf)
    alias_end = child.send_wait(b"/?\r", b"/help", start=start)
    child.wait(b"/?", start=alias_end)
    alias_end = child.wait(b"/compact", start=alias_end)
    child.wait(b"Tab complete/indent/queue", start=alias_end)
    child.wait("»".encode(), start=alias_end)

    start = len(child.buf)
    end = child.send_wait(b"/sta\t", b"tus", start=start)
    status_end = child.send_wait(b"\r", b"state: active", start=end)
    child.wait("»".encode(), start=status_end)
    config_end = child.send_wait(b"/config\r",
        b"/config accepted; applying at the next safe request boundary", start=status_end
    )
    answer_end = child.wait(b"fixture answer", start=config_end)
    child.exit_cleanly(answer_end)


def test_unfinished_public_resume():
    for stop in ("crash", "eof", "exit"):
        with Child([], PROMPT.rstrip()) as child:
            child.send_wait(b"queue_slow\r", b"working slowly")
            session_id = child.session_id()
            log = events(session_id)
            output = [e["data"] for e in log if e["type"] == "response_output"]
            assert "".join(e["item"]["text"] for e in output) == "working slowly\n", log
            assert not [e for e in log if e["type"] == "response_completed"]
            if stop == "crash":
                child.kill()
            else:
                child.send(b"\x04" if stop == "eof" else b"/exit\r")
                child.finish()
        with Child(["--resume", session_id]) as child:
            child.wait(b"unfinished turn")
            child.wait(b"working slowly")
            end = child.wait(b"fixture answer")
            child.wait_idle_prompt(start=end)
            start = len(child.buf)
            end = child.send_wait(b"/history 1\r", b"working slowly", start=start)
            child.wait(b"fixture answer", start=end)
            child.exit_now()
        log = events(session_id)
        turn = one(log, "turn_started")["data"]["turn_id"]
        assert one(log, "turn_completed")["data"]["turn_id"] == turn
        requests = [e["data"] for e in log if e["type"] == "response_started"]
        assert [r["cycle"] for r in requests] == [1, 2], log
        assert {r["turn_id"] for r in requests} == {turn}
        partial = one(log, "response_interrupted")["data"]["partial_public"]
        assert partial[0]["text"] == "working slowly\n", log
        assert not [e for e in log if e["type"] == "turn_interrupted"]
        # A second resume is passive after the recovered turn has completed.
        count = len(requests)
        with Child(["--resume", session_id], PROMPT.rstrip()) as child:
            child.drain(.1)
            child.exit_now()
        assert sum(e["type"] == "response_started" for e in events(session_id)) == count


def test_retry_budget_survives_response_boundary():
    config = write_config("resume-budget.ini", "[agent]\nmax_turn_retries=1\n[provider openai]\n")
    for failure_number in (0, 1):
        before = session_ids()
        result = subprocess.run([BINARY, "--dotdir", DOTDIR, "--config", str(config),
                                 "-e", "--", "empty"], cwd=WORKSPACE,
                                capture_output=True, timeout=10)
        assert result.returncode == 4, result.stderr
        sid = new_session(before)
        path = STATE_ROOT / sid / "events.jsonl"
        lines = path.read_bytes().splitlines(keepends=True)
        log = [json.loads(line) for line in lines]
        failures = [e for e in log if e["type"] == "response_completed"]
        assert len(failures) == 2, log
        assert one(log, "turn_recovery")["data"]["retry_attempts"] == 1
        path.write_bytes(b"".join(lines[:failures[failure_number]["seq"]]))
        with Child(["--config", str(config), "--resume", sid], ready=None) as child:
            child.wait(b"turn failed; try /retry")
            child.wait_idle_prompt()
            child.exit_now()
        log = events(sid)
        assert len([e for e in log if e["type"] == "response_completed"]) == 2, log
        assert len([e for e in log if e["type"] == "turn_started"]) == 1, log
        assert one(log, "turn_failed")


def test_explicit_cancel_is_not_resumed():
    with Child([], PROMPT.rstrip()) as child:
        child.send_wait(b"queue_slow\r", b"working slowly")
        session_id = child.session_id()
        end = child.send_wait(b"\x03", b"turn interrupted", start=len(child.buf))
        child.exit_cleanly(end)
    path = STATE_ROOT / session_id / "events.jsonl"
    lines = path.read_bytes().splitlines(keepends=True)
    cancellation = next(e for e in map(json.loads, lines) if e["type"] == "turn_cancel_requested")
    path.write_bytes(b"".join(lines[:cancellation["seq"]]))
    with Child(["--resume", session_id], PROMPT.rstrip()) as child:
        child.wait(b"interrupted turn")
        child.wait(b"working slowly")
        child.drain(.1)
        assert b"fixture answer" not in child.buf
        child.exit_now()
    log = events(session_id)
    assert len([e for e in log if e["type"] == "response_started"]) == 1
    assert one(log, "turn_interrupted")["data"]["origin"] == "user"


def test_recovery_at_durable_tool_boundaries():
    # These private, inactive test sessions are cut to real writer boundaries.
    # No fabricated events or hashes, live file rewriting, or repeated effects.
    cuts = [("response_completed", 0), ("tool_started", 0),
            ("tool_finished", 0), ("tool_finished", 1),
            ("response_completed", 1)]
    for kind, occurrence in cuts:
        with Child([], PROMPT.rstrip()) as child:
            end = child.send_wait(b"two_tools\r", b"two tools complete")
            child.exit_cleanly(end)
            session_id = child.session_id()
        path = STATE_ROOT / session_id / "events.jsonl"
        lines = path.read_bytes().splitlines(keepends=True)
        parsed = [json.loads(line) for line in lines]
        end = [i for i, e in enumerate(parsed) if e["type"] == kind][occurrence] + 1
        before = parsed[:end]
        path.write_bytes(b"".join(lines[:end]))
        with Child(["--resume", session_id]) as child:
            child.wait(b"two tools complete")
            child.wait_idle_prompt()
            child.exit_now()
        after = events(session_id)
        added = after[end:]
        assert not [e for e in added if e["type"] == "tool_started"], added
        one(after, "turn_started")
        one(after, "turn_completed")
        finished = {e["data"]["call_id"] for e in before if e["type"] == "tool_finished"}
        assert not [e for e in added if e["type"] == "tool_finished" and e["data"]["call_id"] in finished]
        started = {e["data"]["call_id"] for e in before if e["type"] == "tool_started"}
        for e in added:
            if e["type"] == "tool_finished":
                expected = "outcome_unknown" if e["data"]["call_id"] in started else "not_run"
                assert e["data"]["result"]["status"] == expected, e


def test_idle_compaction_crash_recovery():
    config = write_config("compact-crash.ini", "[provider openai]\nnative_compaction=false\n")
    with Child(["--config", str(config)], PROMPT.rstrip()) as child:
        end = child.send_wait(b"ping\r", b"pong")
        child.wait_idle_prompt(start=end)
        end = child.send_wait(b"/compact\r", COMPACTED, start=len(child.buf))
        child.exit_cleanly(end)
        session_id = child.session_id()
    path = STATE_ROOT / session_id / "events.jsonl"
    lines = path.read_bytes().splitlines(keepends=True)
    end = next(i for i, line in enumerate(lines) if json.loads(line)["type"] == "compaction_started") + 1
    path.write_bytes(b"".join(lines[:end]))
    with Child(["--config", str(config), "--resume", session_id]) as child:
        # The accepted idle control survives a crash during the provider request.
        end = child.wait(COMPACTED)
        child.exit_cleanly(end)
    log = events(session_id)
    one(log, "compaction_interrupted")
    one(log, "compaction_completed")


def test_active_next_turn_settings():
    # Defaults can change while the current turn retains its request identity.
    with Child([], PROMPT.rstrip()) as child:
        child.send_wait(b"queue_slow\r", b"working slowly")
        session_id = child.session_id()
        initial = one(events(session_id), "turn_started")["data"]["config"]
        start = len(child.buf)
        child.send_wait(b"/model gpt-5.6-luna/high\r",
                        b"model for next turn:", start=start)
        start = len(child.buf)
        child.send_wait(b"/effort medium\r", b"effort", start=start)
        # Force another request in the *same* turn after both default changes.
        end = child.send_wait(b"finish this turn\r", b"steered: finish this turn",
                              start=len(child.buf))
        child.wait_idle_prompt(start=end)
        end = child.send_wait(b"ping\r", b"pong", start=len(child.buf))
        child.exit_cleanly(end)
    log = events(session_id)
    turns = [e["data"] for e in log if e["type"] == "turn_started"]
    assert len(turns) == 2, turns
    assert turns[1]["config"]["model"] == "gpt-5.6-luna", turns
    assert turns[1]["config"]["effort"] == "medium", turns
    requests = [e["data"] for e in log if e["type"] == "response_started"
                and e["data"]["turn_id"] == turns[0]["turn_id"]]
    assert len(requests) == 2, requests
    for request in requests:
        assert (request["model"], request["effort"]) == (
            initial["model"], initial["effort"]), requests
    # The deferred defaults must also survive a crash before the next turn.
    with Child(["--resume", session_id], PROMPT.rstrip()) as child:
        child.send_wait(b"queue_slow\r", b"working slowly")
        child.send_wait(b"/effort high\r", b"effort", start=len(child.buf))
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            changes = [e for e in events(session_id)
                       if e["type"] == "model_selection_changed"]
            if changes[-1]["data"]["new_effort"] == "high":
                break
            child.read_once(.02)
        else:
            raise AssertionError("active effort change was not journaled")
        child.kill()
    with Child(["--resume", session_id], PROMPT.rstrip()) as child:
        child.send_wait(b"/status\r", b"gpt-5.6-luna", start=len(child.buf))
        child.exit_now()
    assert changes[-1]["data"]["new_effort"] == "high"


def cached_timestamp(cache):
    updated = cache["updated_at_ms"] / 1000
    return datetime.fromtimestamp(updated, timezone.utc).strftime(
        "cache updated: %Y-%m-%dT%H:%M:%SZ"
    ).encode()


def test_uncached_typed_model_selection():
    before = session_ids()
    cache_path = Path(DOTDIR) / "models.json"
    default_codex_cache = Path(os.environ["HOME"]) / ".codex" / "models_cache.json"
    custom_codex_home = Path(os.environ["SNAJPAGENT_TEST_ROOT"]) / "codex-home"
    custom_codex_cache = custom_codex_home / "models_cache.json"
    borrowed_catalog = json.dumps({
        "models": [{
            "slug": "borrowed-model", "visibility": "list", "priority": 1,
            "supported_reasoning_levels": [{"effort": "high"}],
            "default_reasoning_level": "high",
        }],
    })
    env = dict(os.environ)
    cache_path.unlink(missing_ok=True)
    default_codex_cache.parent.mkdir(mode=0o700, exist_ok=True)
    custom_codex_home.mkdir(mode=0o700, exist_ok=True)
    default_codex_cache.write_text(borrowed_catalog, encoding="utf-8")
    custom_codex_cache.write_text(borrowed_catalog, encoding="utf-8")
    # Even an explicitly located Codex cache is not snajpagent state.
    env["CODEX_HOME"] = str(custom_codex_home)
    child = Child([], PROMPT.rstrip(), env=env)
    end = child.send_wait(b"/model\r", b"model cache is empty; use /model cache")
    child.wait(PROMPT.rstrip(), start=end)
    assert not cache_path.exists()

    # A typed model is trusted without discovery or any cache mutation.
    end = child.send_wait(b"/model gpt-5.6-luna / high\r",
        b"model for next turn: openai / gpt-5.6-luna / high", start=end
    )
    end = child.wait(
        b"snajpagent: model is not known in the model cache; "
        b"the configured provider will still be used",
        start=end,
    )
    child.wait(PROMPT.rstrip(), start=end)
    assert not cache_path.exists()
    child.exit_now(expect_resume=False)
    assert session_ids() == before

    # The conventional ~/.codex cache is ignored as well.
    env.pop("CODEX_HOME", None)
    child = Child([], PROMPT.rstrip(), env=env)
    end = child.send_wait(b"/model list\r", b"model cache is empty; use /model cache")
    child.wait(PROMPT.rstrip(), start=end)
    assert not cache_path.exists()
    child.exit_now(expect_resume=False)
    assert session_ids() == before


def test_provider_login_and_first_run():
    root = Path(os.environ["SNAJPAGENT_TEST_ROOT"]) / "login-tests"
    root.mkdir()
    env = dict(os.environ)
    env["SNAJPAGENT_TEST_LOGIN"] = "1"
    env.pop("OPENAI_API_KEY", None)
    api = root / "api"
    command = [BINARY, "--dotdir", str(api), "-m", "openrouter/model/high", "login",
               "openrouter", "--with-api-key"]
    result = subprocess.run(command, input=b"private-test-key\n", capture_output=True,
                            env=env, timeout=10)
    assert result.returncode == 0, result.stderr
    config = api / "config.ini"
    auth = api / "auth" / "openrouter.json"
    assert "private-test-key" not in config.read_text()
    assert "auth = api_key" in config.read_text()
    assert "native_compaction = false" in config.read_text()
    assert config.stat().st_mode & 0o777 == 0o600
    assert auth.stat().st_mode & 0o777 == 0o600
    original = config.read_bytes()
    config.unlink()
    result = subprocess.run(command[:-1], capture_output=True, env=env, timeout=10)
    assert result.returncode == 0, result.stderr
    assert config.read_bytes() == original
    result = subprocess.run([BINARY, "--dotdir", str(api), "login", "status"],
                            capture_output=True, env=env, timeout=10)
    assert result.returncode == 0 and b"openrouter: api_key (stored)" in result.stdout
    assert b"private-test-key" not in result.stdout + result.stderr
    result = subprocess.run([BINARY, "--dotdir", str(api), "login", "openai",
                             "--with-api-key"], input=b"second-private-key\n",
                            capture_output=True, env=env, timeout=10)
    assert result.returncode == 0, result.stderr
    assert "provider = openrouter" in config.read_text()
    assert "model = model" in config.read_text()
    assert "reasoning_effort = high" in config.read_text()
    assert (api / "auth" / "openai.json").exists()
    result = subprocess.run([BINARY, "--dotdir", str(api), "logout", "openrouter"],
                            capture_output=True, env=env, timeout=10)
    assert result.returncode == 0 and not auth.exists()
    assert (api / "auth" / "openai.json").exists()
    # A failed config commit rolls back only this login's new credential.
    lock = os.open(api, os.O_RDONLY | os.O_DIRECTORY)
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        before_failure = config.read_bytes()
        result = subprocess.run(command, input=b"rolled-back-key\n", capture_output=True,
                                env=env, timeout=10)
        assert result.returncode != 0
        assert config.read_bytes() == before_failure
        assert not auth.exists()
        assert (api / "auth" / "openai.json").exists()
    finally:
        os.close(lock)
    missing = root / "missing"
    result = subprocess.run([BINARY, "--dotdir", str(missing), "--config",
                             str(root / "absent.ini"), "login", "openrouter",
                             "--with-api-key"], input=b"not-saved\n", capture_output=True,
                            env=env, timeout=10)
    assert result.returncode != 0 and not missing.exists()
    config.write_text("[unknown]\nvalue=1\n")
    result = subprocess.run(command, input=b"not-saved\n", capture_output=True,
                            env=env, timeout=10)
    assert result.returncode != 0 and not auth.exists()
    config.write_bytes(original)

    for cancel in (True, False):
        fresh = root / ("cancelled" if cancel else "first-run")
        with Child.from_command(shlex.join([BINARY, "--dotdir", str(fresh)]),
                                env=dict(os.environ, SNAJPAGENT_TEST_LOGIN="1")) as child:
            child.wait(b"Provider: ")
            child.send_wait(b"openrouter\n", b"Local provider name [openrouter]: ")
            child.send_wait(b"\n", b"API key (hidden;")
            end = child.send_wait(b"hidden-first-run-key\n", b"Fetch this provider's model list now?")
            assert b"hidden-first-run-key" not in child.buf
            if cancel:
                child.send(b"\x03")
                child.finish(expected=2, expect_resume=False)
                assert not (fresh / "config.ini").exists()
                assert not (fresh / "auth").exists()
            else:
                child.send_wait(b"n\n", b"Model number or exact model ID: ", start=end)
                end = child.send_wait(b"vendor/model\n", b"Default model: openrouter / vendor/model")
                child.wait(PROMPT.rstrip(), start=end)
                child.exit_now(expect_resume=False)
                assert not list((fresh / "sessions").glob("*/events.jsonl"))
                assert (fresh / "auth" / "openrouter.json").exists()
                assert b"hidden-first-run-key" not in child.buf


def test_compaction_policy_selection():
    before = session_ids()
    cache_path = Path(DOTDIR) / "models.json"
    old_cache = cache_path.read_bytes() if cache_path.exists() else None
    config = write_config("compact-auto.ini",
        "[agent]\nmodel=gpt-5.6-luna\nreasoning_effort=high\n"
        "[provider first]\nbase_url=https://example.test/backend-api/codex\n"
        "[provider second]\nauto_compact_input_tokens=30000\n"
        "[provider third]\nauto_compact_input_tokens=0\n"
        "[model-limit first/gpt-5.6-luna]\ncontext_window_tokens=872000\n"
        "[model-limit first/small]\ncontext_window_tokens=100000\n"
        "max_output_tokens=20000\n")
    original_config = config.read_bytes()
    child = Child(["--config", str(config), "--no-color"])
    try:
        child.wait(PROMPT.rstrip())
        end = child.send_wait(b"/model cache\r", b"compact=745560 (auto)")
        child.wait(PROMPT.rstrip(), start=end)
        original_cache = cache_path.read_bytes()
        for selector, expected in (
            ("first / small / high", b"compact=72000 (auto)"),
            ("first / unknown / high", b"compact=120000 (auto fallback)"),
            ("second / gpt-5.6-luna / high", b"compact=30000 (fixed)"),
            ("third / gpt-5.6-luna / high", b"compact=0 (off)"),
            ("first / gpt-5.6-luna / high", b"compact=745560 (auto)"),
        ):
            start = len(child.buf)
            end = child.send_wait(f"/model {selector}\r".encode(), b"model for next turn: " + selector.encode(), start=start)
            child.wait(PROMPT.rstrip(), start=end)
            for command in (b"/model\r", b"/status\r"):
                start = len(child.buf)
                end = child.send_wait(command, expected, start=start)
                child.wait(PROMPT.rstrip(), start=end)
        assert config.read_bytes() == original_config
        assert cache_path.read_bytes() == original_cache
        child.exit_now(expect_resume=False)
        assert session_ids() == before
    finally:
        child.kill()
        if old_cache is None:
            cache_path.unlink(missing_ok=True)
        else:
            cache_path.write_bytes(old_cache)


def test_provider_local_models(native=True):
    text = (
        "[agent]\nmodel=small\nreasoning_effort=high\n"
        "[provider codex-lb]\nbase_url=https://fixture.test/backend-api/codex\n"
        "auto_compact_input_tokens=0\nexact_token_count=true\n"
        f"native_compaction={'true' if native else 'false'}\n"
        "[provider second]\n"
        "[model-alias codex-lb/small]\nmodel=gpt-6-astra\n"
        "[model-alias codex-lb/large]\nmodel=gpt-6-astra\n"
        "[model-limit codex-lb]\ncontext_window_tokens=500000\n"
        "[model-limit codex-lb/small]\ncontext_window_tokens=128000\n"
    )
    config = write_config("provider-models.ini",text)
    before = session_ids()
    with Child(["--config", str(config)], ready=b"codex-lb/small/high") as child:
        assert session_ids() == before
        child.send_wait(b"/status\r", b"hard-input=121600")
        child.wait(b"context rule: [model-limit codex-lb/small]")
        start = len(child.buf)
        answer_end = child.send_wait(b"ping\r", b"pong", start=start)
        sid = new_session(before)
        wait_turn_completed(child, sid, "ping")
        child.wait_idle_prompt(start=answer_end)
        end = child.send_wait_idle(b"/compact\r", COMPACTED, start=start)
        child.send_wait(b"/model large/high save\r", b"model for next turn: codex-lb / large / high")
        end = child.wait(b"configuration saved:")
        child.wait_idle_prompt(start=end)
        start = len(child.buf)
        child.send_wait(b"/status\r", b"hard-input=475000", start=start)
        child.wait_idle_prompt(start=start)
        child.exit_now()
        log = events(sid)
        assert one(log, "session_created")["data"]["default_model"] == "small"
        assert one(log, "turn_started")["data"]["config"]["model"] == "small"
        assert one(log, "compaction_started")["data"]["model"] == "small"
        assert one(log, "model_selection_changed")["data"]["new_model"] == "large"
        assert "model_alias" not in json.dumps(log)
        assert "model = large" in config.read_text()
        prefix = (STATE_ROOT / sid / "events.jsonl").read_bytes()
        # A provider rename is recovered explicitly; old history stays byte-identical.
        config.write_text(config.read_text().replace("codex-lb", "renamed"))
        with Child(["--config", str(config), "--provider", "renamed", "--resume", sid], ready=b"renamed/large/high") as resumed:
            resumed.exit_now()
            assert (STATE_ROOT / sid / "events.jsonl").read_bytes().startswith(prefix)
            changes = [e["data"] for e in events(sid) if e["type"] == "model_selection_changed"]
            assert changes[-1]["new_provider"] == "renamed"
            assert changes[-1]["new_model"] == "large"


def test_model_cache_and_selection():
    cache_path = Path(DOTDIR) / "models.json"
    initial_prompt = b" first/uncached-start/low   0% \xe2\x80\xba "
    cache_path.unlink(missing_ok=True)
    config = write_config("models.ini",
        "[agent]\n"
        "model = uncached-start\n"
        "reasoning_effort = low\n"
        "[provider first]\n"
        "api_key = ${FIRST_API_KEY}\n"
        "[provider second]\n"
        "api_key = ${SECOND_API_KEY}\n")
    child = Child(["--config", str(config)], PROMPT.rstrip())

    # Explicit refresh creates the complete all-provider cache.
    start = len(child.buf)
    child.send_wait(b"/model cache\r", b"selected: first / uncached-start / low", start=start)
    child.wait(b"1. first / gpt-5.6-luna / high", start=start)
    child.wait(b"16. second / vendor/future-model / low", start=start)
    cache = json.loads(cache_path.read_text(encoding="utf-8"))
    assert cache_path.stat().st_mode & 0o777 == 0o600
    assert cache["schema_version"] == 1
    assert [provider["name"] for provider in cache["providers"]] == [
        "first", "second"
    ]
    first_model = cache["providers"][0]["models"][0]
    assert first_model["count_capability"] == "unknown"
    assert first_model["observed_input_bytes"] == 0
    assert first_model["observed_input_tokens"] == 0
    assert first_model["observed_hard_input_tokens"] == 0
    child.wait(b"count=unknown", start=start)
    child.wait(b"count=unknown", start=start)
    stamp = cached_timestamp(cache)
    end = child.wait(stamp + b"\r\n", start=start)
    end = child.wait(initial_prompt, start=end)

    # /model list is a cache-only alias and retains the stored timestamp.
    original = cache_path.read_bytes()
    original_inode = cache_path.stat().st_ino
    end = child.send_wait(b"/model list\r", stamp + b"\r\n", start=end)
    end = child.wait(initial_prompt, start=end)
    assert cache_path.read_bytes() == original
    assert cache_path.stat().st_ino == original_inode

    # A later explicit refresh atomically replaces the complete catalog.
    child.send_wait(b"/model cache\r", b"16. second / vendor/future-model / low", start=end)
    refreshed = json.loads(cache_path.read_text(encoding="utf-8"))
    assert refreshed["updated_at_ms"] >= cache["updated_at_ms"]
    assert cache_path.stat().st_ino != original_inode
    refreshed_stamp = cached_timestamp(refreshed)
    end = child.wait(refreshed_stamp + b"\r\n", start=end)
    end = child.wait(initial_prompt, start=end)

    # A bare cached model chooses the highest recognized advertised effort.
    end = child.send_wait(b"/model gpt-5.6-luna\r",
        b"model for next turn: first / gpt-5.6-luna / high", start=end
    )
    prompt_end = child.wait(PROMPT.rstrip(), start=end)
    assert b"not known in the model cache" not in child.buf[end:prompt_end]
    end = prompt_end

    # Uncached identifiers and effort names pass through without local lookup.
    for selector, expected in (
            ("definitely-new-model", "first / definitely-new-model / high"),
            ("fresh-model / quantum", "first / fresh-model / quantum"),
            ("default / literal-effort", "first / default / literal-effort"),
            ("second / future-new / cosmic", "second / future-new / cosmic")):
        end = child.send_wait(f"/model {selector}\r".encode(),
            f"model for next turn: {expected}".encode(), start=end)
        end = child.wait(b"not known in the model cache", start=end)
        child.wait(PROMPT.rstrip(), start=end)

    # Both numeric spellings select the exact flattened cached variant.
    for selector, expected in (
            ("2", "first / gpt-5.6-terra / low"),
            ("#16", "second / vendor/future-model / low"),
            ("#9", "second / gpt-5.6-luna / high")):
        end = child.send_wait(f"/model {selector}\r".encode(),
            f"model for next turn: {expected}".encode(), start=end)
        child.wait(PROMPT.rstrip(), start=end)
    answer_end = child.send_wait(b"ping\r", b"pong", start=end)
    child.exit_cleanly(answer_end)

    session_id = child.session_id()
    log = events(session_id)
    changes = [event for event in log
               if event["type"] == "model_selection_changed"]
    assert len(changes) == 8
    assert changes[-1]["data"]["new_provider"] == "second"
    assert changes[-1]["data"]["new_model"] == "gpt-5.6-luna"
    assert changes[-1]["data"]["new_effort"] == "high"
    turn = one(log, "turn_started")
    assert turn["data"]["config"]["provider"] == "second"
    assert turn["data"]["config"]["model"] == "gpt-5.6-luna"
    assert turn["data"]["config"]["effort"] == "high"

    # Provider/model/effort selection survives a process restart and resume.
    resumed = Child(["--config", str(config), "--resume", session_id], PROMPT.rstrip())
    status_end = resumed.send_wait(b"/status\r", b"provider: second")
    resumed.wait(b"model: gpt-5.6-luna", start=status_end)
    status_end = resumed.wait(b"effort: high", start=status_end)
    resumed.wait(PROMPT.rstrip(), start=status_end)
    answer_end = resumed.send_wait(b"ping\r", b"pong", start=status_end)
    resumed.exit_cleanly(answer_end)
    turns = [event for event in events(session_id)
             if event["type"] == "turn_started"]
    assert turns[-1]["data"]["config"]["provider"] == "second"
    assert turns[-1]["data"]["config"]["model"] == "gpt-5.6-luna"
    assert turns[-1]["data"]["config"]["effort"] == "high"

    # Any provider failure leaves the previous complete cache untouched.
    complete_cache = cache_path.read_bytes()
    complete_inode = cache_path.stat().st_ino
    failing = Child(["--config", str(config)], PROMPT.rstrip(),
                    env=dict(os.environ, SNAJPAGENT_FIXTURE_MODEL_FAILURE="second"))
    failed_end = failing.send_wait(b"/model cache\r",
        b"cannot refresh provider second: fixture model discovery failed"
    )
    failing.wait(initial_prompt, start=failed_end)
    failing.send(b"/exit\r")
    assert failing.reap() == 0
    assert cache_path.read_bytes() == complete_cache
    assert cache_path.stat().st_ino == complete_inode


def test_model_configuration_save():
    config = write_config("model-save.ini",
        "# unrelated comment stays byte-for-byte\n"
        "[agent]\n"
        "model = save-base\n"
        "reasoning_effort = low\n"
        "max_goal_prompt_bytes = 123456\n"
        "[provider first]\n"
        "api_key = ${FIRST_API_KEY}\n"
        "[provider second]\n"
        "api_key = ${SECOND_API_KEY}\n")
    original = config.read_bytes()
    original_mode = config.stat().st_mode & 0o777
    child = Child(["--config", str(config)], PROMPT.rstrip())

    # Selection without a suffix remains session-only.
    cached = child.send_wait(b"/model cache\r", b"16. second / vendor/future-model / low")
    child.wait(PROMPT.rstrip(), start=cached)
    end = child.send_wait(b"/model #9\r",
        b"model for next turn: second / gpt-5.6-luna / high"
    )
    child.wait(PROMPT.rstrip(), start=end)
    assert config.read_bytes() == original

    # The one-letter spelling atomically persists a numbered cache row.
    old_inode = config.stat().st_ino
    end = child.send_wait(b"/model 2 s\r",
        b"model for next turn: first / gpt-5.6-terra / low", start=end
    )
    end = child.wait(
        f"configuration saved: {config}".encode(), start=end
    )
    child.wait(PROMPT.rstrip(), start=end)
    first_save = config.read_text(encoding="utf-8")
    assert config.stat().st_ino != old_inode
    assert config.stat().st_mode & 0o777 == original_mode
    assert "# unrelated comment stays byte-for-byte\n" in first_save
    assert "max_goal_prompt_bytes = 123456\n" in first_save
    assert "provider = first\n" in first_save
    assert "model = gpt-5.6-terra\n" in first_save
    assert "reasoning_effort = low\n" in first_save

    # The full spelling persists a typed provider/model/effort selection.
    end = child.send_wait(b"/model second / durable-new / cosmic save\r",
        b"model for next turn: second / durable-new / cosmic", start=end
    )
    warning_end = child.wait(b"not known in the model cache", start=end)
    end = child.wait(
        f"configuration saved: {config}".encode(), start=warning_end
    )
    child.wait(PROMPT.rstrip(), start=end)
    saved = config.read_bytes()
    saved_text = saved.decode("utf-8")
    assert "provider = second\n" in saved_text
    assert "model = durable-new\n" in saved_text
    assert "reasoning_effort = cosmic\n" in saved_text

    # Without a preceding selector, save and s remain literal model IDs.
    end = child.send_wait(b"/model save\r", b"model for next turn: second / save / cosmic", start=end)
    end = child.wait(b"not known in the model cache", start=end)
    child.wait(PROMPT.rstrip(), start=end)
    assert config.read_bytes() == saved
    end = child.send_wait(b"/model s\r", b"model for next turn: second / s / cosmic", start=end)
    end = child.wait(b"not known in the model cache", start=end)
    child.wait(PROMPT.rstrip(), start=end)
    assert config.read_bytes() == saved

    # A write failure does not change the selected runtime model.
    config.unlink()
    config.mkdir()
    end = child.send_wait(b"/model rejected-model save\r",
        b"configuration must be a regular file no larger than 64 KiB",
        start=end,
    )
    child.wait(PROMPT.rstrip(), start=end)
    status_end = child.send_wait(b"/status\r", b"model: s", start=end)
    child.wait(PROMPT.rstrip(), start=status_end)
    config.rmdir()
    config.write_bytes(saved)
    os.chmod(config, original_mode)
    answered = child.send_wait_idle(b"ping\r", b"pong", start=status_end)
    child.exit_now()

    log = events(child.session_id())
    assert not [
        event for event in log
        if event["type"] == "model_selection_changed" and
        event["data"]["new_model"] == "rejected-model"
    ]

    # A new session consumes the saved provider and defaults from that path.
    before_new = session_ids()
    child = Child(["--config", str(config)], PROMPT.rstrip())
    end = child.send_wait(b"/status\r", b"provider: second")
    child.wait(b"model: durable-new", start=end)
    end = child.wait(b"effort: cosmic", start=end)
    child.wait(PROMPT.rstrip(), start=end)
    child.exit_now(expect_resume=False)
    assert session_ids() == before_new


def test_config_editor_reload():
    root = Path(os.environ["SNAJPAGENT_TEST_ROOT"])
    plan = root / "config" / "editor-plan"
    seen = root / "config" / "editor-seen"
    editor = root / "config" / "editor"
    config = write_config("editor.ini",
        "[agent]\nmodel = editor-base\nreasoning_effort = medium\n"
        "[provider openai]\napi_key = ${OPENAI_API_KEY}\n"
        "[ui]\n",
    )
    valid_two = write_config("editor-valid-two.ini",
        "[agent]\nmodel = ignored-default\nreasoning_effort = high\n"
        "[provider openai]\napi_key = ${OPENAI_API_KEY}\n"
        "[ui]\ntyping_pause_ms = 25\n"
        "prompt = {chat:{hour:2}:{minute:02}:{second:02}:}"
        "{rollout-idle:W{context:3}%{goal_spinner}{activity_spinner}›}"
        "{rollout-active:W{context:3}%{activity_spinner}»}\n"
        'prompt_spinner_goal = "\\0"\n'
        'prompt_spinner_provider = "\\0P"\n'
        'prompt_spinner_tool = " "\n',
    )
    valid_one = write_config("editor-valid-one.ini",
        "[agent]\nmodel = another-default\nreasoning_effort = low\n"
        "[provider openai]\napi_key = ${OPENAI_API_KEY}\n"
        "[ui]\n",
    )
    invalid = write_config("editor-invalid.ini",
        "[ui]\nprompt_spinner_tool = \"\\0\"\n"
        "prompt = {chat:{hour:002}}{rollout-idle:x}{rollout-active:y}\n")
    unrenderable = write_config("editor-unrenderable.ini",
        "[provider openai]\n[ui]\n"
        "prompt = {chat:x}{rollout-idle:" + ("x" * 600) +
        "}{rollout-active:z}\n",
    )
    network_port = free_port()
    network = write_config("editor-network.ini",
        "[agent]\nmodel = network-default\nreasoning_effort = medium\n"
        "[provider openai]\napi_key = ${OPENAI_API_KEY}\n"
        "[irc]\n"
        f"listen = 127.0.0.1:{network_port}\n"
        "model_nick = reloadagent\noperator_nick = reloadop\n"
        "room_name = lab\n",
    )
    editor.write_text(
        "#!/bin/sh\n"
        "choice=$(cat \"$SNAJPAGENT_EDITOR_PLAN\") || exit 2\n"
        "printf '%s' \"$1\" >\"$SNAJPAGENT_EDITOR_SEEN\" || exit 3\n"
        "case $choice in\n"
        "  unchanged) exit 0 ;;\n"
        "  nonzero:*) cp \"${choice#nonzero:}\" \"$1\" || exit 4; exit 7 ;;\n"
        "  *) exec cp \"$choice\" \"$1\" ;;\n"
        "esac\n",
        encoding="utf-8",
    )
    editor.chmod(0o700)
    env = dict(os.environ, EDITOR=str(editor),
               SNAJPAGENT_EDITOR_PLAN=str(plan), SNAJPAGENT_EDITOR_SEEN=str(seen))
    before = session_ids()
    child = Child(["--config", str(config)], PROMPT.rstrip(), env=env)
    assert session_ids() == before
    child.send_wait(b"/verbose 2\r", b"verbosity: 2")

    plan.write_text("unchanged", encoding="utf-8")
    end = child.send_wait(b"/config\r",
        f"configuration unchanged: {config}".encode()
    )
    child.wait(PROMPT.rstrip(), start=end)
    assert seen.read_text(encoding="utf-8") == str(config)

    plan.write_text(str(valid_two), encoding="utf-8")
    end = child.send_wait(b"/config\r", f"configuration reloaded: {config}".encode(), start=end)
    child.wait("W  0%› ".encode(), start=end)
    status_end = child.send_wait(b"/status\r", b"verbosity: 2", start=end)
    child.wait(b"model: editor-base", start=end)
    child.wait(PROMPT.rstrip(), start=status_end)

    plan.write_text(str(invalid), encoding="utf-8")
    end = child.send_wait(b"/config\r", b"invalid configuration at line 3", start=status_end)
    child.wait("W  0%› ".encode(), start=end)
    status_end = child.send_wait(b"/status\r", b"verbosity: 2", start=end)
    child.wait(b"model: editor-base", start=end)
    child.wait(PROMPT.rstrip(), start=status_end)

    plan.write_text(str(unrenderable), encoding="utf-8")
    end = child.send_wait(b"/config\r",
        b"reloaded prompt cannot be rendered with the current selection",
        start=status_end,
    )
    child.wait(PROMPT.rstrip(), start=end)
    status_end = child.send_wait(b"/status\r", b"verbosity: 2", start=end)
    child.wait(b"model: editor-base", start=end)
    child.wait(PROMPT.rstrip(), start=status_end)

    # File changes are checked and loaded even when the editor exits nonzero.
    plan.write_text(f"nonzero:{valid_one}", encoding="utf-8")
    warning_end = child.send_wait(b"/config\r",
        b"$EDITOR exited unsuccessfully after changing the configuration",
        start=status_end,
    )
    end = child.wait(
        f"configuration reloaded: {config}".encode(), start=warning_end
    )
    child.wait(PROMPT.rstrip(), start=end)
    status_end = child.send_wait(b"/status\r", b"verbosity: 2", start=end)
    child.wait(b"model: editor-base", start=end)
    child.wait(PROMPT.rstrip(), start=status_end)

    # Topology reloads preserve the selected private/public view.
    plan.write_text(str(network), encoding="utf-8")
    end = child.send_wait(b"/config\r", f"configuration reloaded: {config}".encode(), start=end)
    child.wait(PROMPT.rstrip(), start=end)
    child.send_wait(b"/chat\r", f"reloadop@{socket.gethostname()} : ".encode(), start=end)
    child.send_wait(b"session setup\r", "reloadop › session setup".encode())
    peer = IRCClient(network_port, "reloadpeer")
    peer.close()
    # Membership notifications start a turn; /config is idle-only.
    deadline = time.monotonic() + 8.0
    while True:
        if session_ids() == before:
            assert time.monotonic() < deadline, bytes(child.buf)
            child.drain(0.05)
            continue
        session_id = new_session(before)
        log = events(session_id)
        turns = [event["data"]["turn_id"] for event in log
                 if event["type"] == "turn_started" and
                 "event=quit sender=reloadpeer" in event["data"]["text"]]
        if any(event["type"] == "turn_completed" and
               event["data"]["turn_id"] in turns for event in log):
            break
        assert time.monotonic() < deadline, bytes(child.buf)
        child.drain(0.05)
    # Unrelated edits cannot resurrect a runtime-stopped configured host,
    # nor erase a runtime-added outgoing endpoint. Keep its sockets intact.
    end = child.send_wait(b"/server stop\r", b"hosting stopped; outgoing connections unchanged", start=end)
    wait_turn_completed(child, session_id, "endpoint removed")
    upstream = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    upstream.bind(("127.0.0.1", 0))
    upstream.listen(8)
    outgoing = f"127.0.0.1:{upstream.getsockname()[1]}"
    end = child.send_wait(f"/connect {outgoing}\r".encode(), b"outgoing connection added", start=end)
    links = accept_connections(upstream, 2)
    edited_network = write_config("editor-network-unrelated.ini",
        network.read_text() + "[ui]\ntyping_pause_ms = 26\n")
    try:
        plan.write_text(str(edited_network), encoding="utf-8")
        end = child.send_wait(b"/config\r", f"configuration reloaded: {config}".encode(), start=end)
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            assert probe.connect_ex(("127.0.0.1", network_port)) != 0
        ready, _, _ = select.select([upstream], [], [], 0.1)
        assert not ready, "unrelated reload restarted an outgoing owner"
        for link in links:
            link.setblocking(False)
            try:
                data = link.recv(65536)
                assert data, "unrelated reload closed the outgoing socket"
            except BlockingIOError:
                pass
        # Deliberately changing the file's listener overrides the runtime
        # removal; changing client fields is handled independently.
        replacement_port = free_port()
        edited_network.write_text(edited_network.read_text().replace(
            f"listen = 127.0.0.1:{network_port}", f"listen = 127.0.0.1:{replacement_port}"), encoding="utf-8")
        end = child.send_wait(b"/config\r", f"configuration reloaded: {config}".encode(), start=end)
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            assert probe.connect_ex(("127.0.0.1", replacement_port)) == 0
        end = child.send_wait(b"/disconnect\r", b"outgoing connections removed; hosting unchanged", start=end)
        wait_turn_completed(child, session_id, f"endpoint={outgoing} ")
    finally:
        for link in links:
            link.close()
        upstream.close()
    plan.write_text(str(valid_one), encoding="utf-8")
    end = child.send_wait(b"/config\r", f"configuration reloaded: {config}".encode(), start=end)
    child.send_wait(b"/rollout\r", PROMPT.rstrip(), start=end)
    child.exit_now()

    # The resolved default path is passed to the editor and may be created.
    default_config = Path(DOTDIR) / "config.ini"
    prior_default = default_config.read_bytes() if default_config.exists() else None
    try:
        default_config.unlink(missing_ok=True)
        plan.write_text(str(valid_one), encoding="utf-8")
        child = Child([], PROMPT.rstrip(), env=env)
        end = child.send_wait(b"/config\r",
            f"configuration reloaded: {default_config}".encode()
        )
        child.wait(PROMPT.rstrip(), start=end)
        assert seen.read_text(encoding="utf-8") == str(default_config)
        child.exit_now(expect_resume=False)
    finally:
        if prior_default is None:
            default_config.unlink(missing_ok=True)
        else:
            default_config.write_bytes(prior_default)


def test_known_context_meter():
    config = Path(os.environ["SNAJPAGENT_TEST_ROOT"]) / "config" / "models.ini"
    before = session_ids()
    child = Child(["--config", str(config)], b" first/uncached-start/low   0% \xe2\x80\xba ")
    selected = child.send_wait(b"/model gpt-5.6-luna / high\r",
        b"model for next turn: first / gpt-5.6-luna / high"
    )
    child.wait(b"gpt-5.6-luna/high   0% \xe2\x80\xba ", start=selected)
    assert session_ids() == before
    start = len(child.buf)
    child.send_wait(b"slow\r", b"working slowly", start=start)
    session_id = new_session(before)
    deadline = time.monotonic() + 8.0
    response = None
    while time.monotonic() < deadline:
        starts = [event for event in events(session_id)
                  if event["type"] == "response_started"]
        if starts:
            response = starts[-1]["data"]
            break
        child.read_once(0.02)
    assert response is not None
    hard = response["hard_input_tokens"]
    assert isinstance(hard, int) and hard > 0
    assert response["count_method"] == "unknown"
    assert response["input_tokens_bound"] == 0
    child.wait(b"?%", start=start)
    interrupted = child.send_wait(b"\x03", b"turn interrupted", start=start)
    child.exit_cleanly(interrupted)

    child = Child(["--config", str(config), "--resume", session_id])
    child.wait_idle_prompt()
    start = len(child.buf)
    answered = child.send_wait_idle(b"context_anchor_chain\r", b"context anchor complete", start=start)
    completed = [event["data"] for event in events(session_id)
                 if event["type"] == "response_completed"][-1]
    used = completed["usage"]["input_tokens"]
    percent = min(100, (used * 100 + hard - 1) // hard)
    # The idle prompt reports measured usage, and new unknown requests cannot replace it.
    child.wait(f"{percent}% ›".encode(), start=answered)
    child.exit_cleanly(answered)


def test_model_selection_stays():
    config = write_config("model-stays.ini",
        "[provider first]\n[provider second]\n[agent]\n"
        "provider = first\nmodel = original\nreasoning_effort = medium\n")
    original_config = config.read_bytes()
    with Child(["--config", str(config)], ready=b"original/medium") as child:
        end = child.send_wait_idle(b"ping\r", b"pong")
        child.exit_cleanly(end)
    sid = child.session_id()
    # Exit before the first turn: selection must already belong to the session,
    # not an unconsumed override encoded in a printed command.
    with Child(["--config", str(config), "-m", "second/changed/high", "--resume", sid],
               ready=b"second/changed/high") as child:
        child.exit_now()
    selections = [e["data"] for e in events(sid) if e["type"] == "model_selection_changed"]
    assert selections and selections[-1]["new_model"] == "changed", selections
    with Child(["--config", str(config), "--resume", sid], ready=b"second/changed/high") as child:
        end = len(child.buf)
        for _ in range(2):
            end = child.send_wait_idle(b"ping\r", b"pong", start=end)
        end = child.send_wait_idle(b"/model first/final/low\r", b"model for next turn:", start=end)
        for _ in range(2):
            end = child.send_wait_idle(b"ping\r", b"pong", start=end)
        child.exit_cleanly(end)
    with Child(["--config", str(config), "--resume", sid], ready=b"first/final/low") as child:
        end = child.send_wait_idle(b"ping\r", b"pong")
        child.exit_cleanly(end)
    turns = [e["data"]["config"] for e in events(sid) if e["type"] == "turn_started"]
    assert [(t["provider"], t["model"], t["effort"]) for t in turns] == [
        ("first", "original", "medium"), ("second", "changed", "high"),
        ("second", "changed", "high"), ("first", "final", "low"),
        ("first", "final", "low"), ("first", "final", "low")]
    assert config.read_bytes() == original_config


def test_config_and_cli_model_passthrough():
    config = write_config("model-passthrough.ini",
        "[provider openai]\n[agent]\nmodel = openai/gpt-5.6\nreasoning_effort = default\n"
        "[model-alias openai/future]\nmodel=vendor/future-model\n")
    child = Child(["--config", str(config)], b" openai/openai/gpt-5.6/medium   0% \xe2\x80\xba ")

    end = child.send_wait(b"/status\r", b"model: openai/gpt-5.6")
    child.wait(PROMPT.rstrip(), start=end)

    answer_end = child.send_wait(b"ping\r", b"pong")
    child.exit_cleanly(answer_end)

    session_id = child.session_id()
    log = events(session_id)
    turn = one(log, "turn_started")
    assert turn["data"]["config"]["model"] == "openai/gpt-5.6"
    assert turn["data"]["config"]["effort"] == "medium"

    resumed = Child([
        "--config", str(config), "-m", "openai/future",
        "--effort", "custom-effort", "--resume", session_id
    ], b" openai/future/custom-effort   ?% \xe2\x80\xba ")
    start = len(resumed.buf)
    end = resumed.send_wait(b"/status\r",
        b"model: future", start=start
    )
    resumed.wait(PROMPT.rstrip(), start=end)
    for _ in range(2):  # Repeating /effort is a durable no-op, not a model reset.
        end = resumed.send_wait(b"/effort quantum\r", b"effort for next turn: quantum", start=end)
        resumed.wait(PROMPT.rstrip(), start=end)
    resumed.send_wait(b"ping\r", "»".encode(),
                 start=end)
    answer_end = resumed.wait(b"pong", start=end)
    # Same selection stays painted: only the active/idle marker needs repaint.
    idle_end = resumed.wait_idle_prompt(start=answer_end)
    resumed.send(b"/exit\r")
    assert resumed.reap() == 0, idle_end

    resumed_turns = [event for event in events(session_id)
                     if event["type"] == "turn_started"]
    assert resumed_turns[-1]["data"]["config"]["model"] == "future"
    assert resumed_turns[-1]["data"]["config"]["effort"] == "quantum"
    selections = [e["data"] for e in events(session_id) if e["type"] == "model_selection_changed"]
    assert len(selections) == 2
    assert [(e["old_model"], e["new_model"], e["old_effort"], e["new_effort"]) for e in selections] == [
        ("openai/gpt-5.6", "future", "default", "custom-effort"),
        ("future", "future", "custom-effort", "quantum")]


def test_empty_session_lifecycle():
    for action in (b"/exit\r", b"\x04", b"\x03" * 5, b"/archive\r", b"/delete\r",
                   signal.SIGHUP, signal.SIGTERM):
        before = session_ids()
        with Child(["--no-color", "--no-listen", "--no-client"], DEFAULT_IDLE_PROMPT) as child:
            assert session_ids() == before
            child.send_wait(b"/compact\r", b"nothing to compact before the first prompt")
            child.send(b"/status\r")
            child.wait(DEFAULT_IDLE_PROMPT, start=len(child.buf))
            assert session_ids() == before
            if isinstance(action, int):
                os.kill(child.pid, action)
                child.finish(expected=128 + action, expect_resume=False)
            else:
                child.send(action)
                child.finish(expect_resume=False)
            assert session_ids() == before

    before = session_ids()
    with Child(["--no-color", "--no-listen", "--no-client"], DEFAULT_IDLE_PROMPT) as child:
        child.send_wait(b"unsent draft", b"unsent draft")
        child.send(b"\x15\x04")
        child.finish(expect_resume=False)
        assert session_ids() == before

    before = session_ids()
    with Child(["--no-color", "--no-listen", "--no-client"], DEFAULT_IDLE_PROMPT) as child:
        child.send_wait(b"/model selected-before-prompt / high\r", b"selected-before-prompt/high   0%")
        assert session_ids() == before
        child.send_wait(b"ping\r", b"pong")
        sid = new_session(before)
        command = child.exit_now()
        assert command_arguments(command)[-2:] == ["--resume", sid]
        log = events(sid)
        assert len([e for e in log if e["type"] == "session_created"]) == 1
        assert one(log, "turn_started")["data"]["config"]["model"] == "selected-before-prompt"
    saved = (STATE_ROOT / sid / "events.jsonl").read_bytes()
    with Child(["--no-color", "--no-listen", "--no-client", "--resume", sid],
               b"selected-before-prompt/high   ?%") as resumed:
        command = resumed.exit_now()
        assert command_arguments(command)[-2:] == ["--resume", sid]
        assert (STATE_ROOT / sid / "events.jsonl").read_bytes() == saved
    print("empty session lifecycle: ok")



def test_empty_network_session():
    for sent in ("none", "operator", "mention"):
        before = session_ids()
        port = free_port()
        child = Child(["--no-color", "--no-client", "--listen", f"127.0.0.1:{port}",
                       "-n", "emptyagent", "-o", "emptyop", "-r", "lab"])
        peer = None
        try:
            child.wait(chat_prompt("emptyop"))
            peer = IRCClient(port, "emptypeer")
            peer.message("background before input")
            child.wait(b"background before input")
            child.drain(0.2)  # Cross the ordinary background admission delay.
            assert session_ids() == before
            # Exercise buffered IRC rendering before a durable log exists.
            child.send_wait(b"/rollout\r", DEFAULT_IDLE_PROMPT)
            child.send(b"/chat\r")
            child.wait(chat_prompt("emptyop"), start=len(child.buf))
            if sent != "none":
                if sent == "operator":
                    child.send(b"operator first message\r")
                    peer.wait(b"operator first message")
                else:
                    peer.message("emptyagent: network_zero")
                deadline = time.monotonic() + 5.0
                while session_ids() == before:
                    assert time.monotonic() < deadline
                    child.read_once(0.02)
                sid = new_session(before)
                if sent == "mention":
                    wait_turn_completed(child, sid, "network_zero")
                child.send(b"\x04")  # Background catch-up may already be active.
                command = child.finish()
                assert command_arguments(command)[-2:] == ["--resume", sid]
                journal = (STATE_ROOT / sid / "events.jsonl").read_text()
                assert "background before input" in journal
                assert ("operator first message" if sent == "operator" else "network_zero") in journal
            else:
                child.send(b"/exit\r")
                child.finish(expect_resume=False)
                assert session_ids() == before
        finally:
            if peer:
                peer.close()
            child.kill()
    print("empty network session: ok", flush=True)


def test_exit_resume_matrix():
    for action in (b"/exit\r", b"\x04", "cancel", signal.SIGHUP, signal.SIGTERM,
                   "active", b"/archive\r", b"/delete\r", "selection"):
        before = session_ids()
        ready = DEFAULT_IDLE_PROMPT if action == "cancel" else PROMPT.rstrip()
        with Child(["--no-color"], ready) as child:
            if action == "active":
                child.send_wait(b"slow\r", b"working slowly")
                child.send_wait(b"\x04", RESUME_HEADER, timeout=1.0)
            else:
                answered = child.send_wait_idle(b"ping\r", b"pong")
            session_id = new_session(before)
            if action == "cancel":
                start = len(child.buf)
                child.send(b"\x03" * 4)
                deadline = time.monotonic() + 8.0
                while bytes(child.buf[start:]).count(b"^C\r\n") < 4:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0 or not child.read_once(remaining):
                        raise AssertionError(f"missing Ctrl-C cancellations: {child.buf!r}")
                assert os.waitpid(child.pid, os.WNOHANG) == (0, 0)
                child.send(b"\x03")
            elif isinstance(action, int):
                os.kill(child.pid, action)
            elif action == b"/delete\r":
                child.send_wait(action, b"type the displayed 8-character id prefix to confirm")
                child.send(session_id[:8].encode() + b"\r")
            elif action != "active":
                child.send(b"/exit\r" if action == "selection" else action)
            command = child.finish(expected=128 + action if isinstance(action, int) else 0,
                                   expect_resume=action != b"/delete\r")
            if action == b"/delete\r":
                assert not (STATE_ROOT / session_id).exists()
                continue
            arguments = command_arguments(command)
            assert arguments[-2:] == ["--resume", session_id], arguments
            assert arguments[arguments.index("--dotdir") + 1] == DOTDIR
            if action == b"/archive\r":
                assert one(events(session_id), "session_archived")
            if action == "active":
                log = events(session_id)
                assert not [event for event in log if event["type"] == "turn_interrupted"]
                assert one(log, "turn_recovery")
                assert not [event for event in log if event["type"] == "turn_completed"]
                assert b"slow complete" not in child.buf

    selected = Child([
        "--no-color", "-m", "openai/future", "--effort", "xhigh",
        "--resume", session_id,
    ], PROMPT.rstrip())
    selected_command = selected.exit_now()
    selected_arguments = command_arguments(selected_command)
    assert "-m" not in selected_arguments and "--effort" not in selected_arguments
    selection = [e["data"] for e in events(session_id) if e["type"] == "model_selection_changed"][-1]
    assert (selection["new_provider"], selection["new_model"], selection["new_effort"]) == ("openai", "future", "xhigh")
    with Child.from_command(selected_command) as restored:
        restored.wait(b"openai/future/xhigh")
        restored.exit_now()

    occupied = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    occupied.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    occupied.bind(("127.0.0.1", 0))
    occupied.listen()
    occupied_endpoint = f"127.0.0.1:{occupied.getsockname()[1]}"
    before = session_ids()
    failed = Child([
        "--no-color", "-s", occupied_endpoint,
        "-n", "agent", "-o", "localop", "-r", "lab",
    ])
    failed.finish(expected=3, expect_resume=False)
    assert session_ids() == before
    occupied.close()


def test_runtime_network_commands():
    endpoint = f"127.0.0.1:{free_port()}"
    upstream = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    upstream.bind(("127.0.0.1", 0))
    upstream.listen(8)
    outgoing = f"127.0.0.1:{upstream.getsockname()[1]}"
    config = write_config("runtime.ini",
        "[provider openai]\napi_key = ${OPENAI_API_KEY}\n[irc]\n"
        f"listen = {endpoint}\nclient = {outgoing}\n")
    before = session_ids()
    child = Child(["--no-color", "--config", str(config), "--no-listen", "--no-client",
                   "-n", "runtimeagent", "-o", "runtimeop", "-r", "lab"])
    links = []
    peer = None
    try:
        child.wait(PROMPT.rstrip())
        assert session_ids() == before
        end = child.send_wait(b"/help\r", b"/disconnect [ENDPOINT]")
        child.wait(PROMPT.rstrip(), start=end)
        end = child.send_wait(b"/chat\r", b"chat is offline")
        child.wait(chat_prompt("runtimeop"), start=end)
        end = child.send_wait(b"keep-unsent-draft\r", b"no IRC destination selected; use /names")
        child.wait(b"keep-unsent-draft", start=end)
        assert session_ids() == before
        end = child.send_wait(b"\x15/rollout\r", "── rollout ──".encode(), start=end)
        child.wait(PROMPT.rstrip(), start=end)
        end = child.send_wait(b"slow\r", b"working slowly", start=end)
        session_id = new_session(before)
        end = child.send_wait(f"/server start {endpoint}\r".encode(), f"hosting started on {endpoint}".encode(), start=end)
        assert not [event for event in events(session_id)
                    if event["type"] in ("turn_completed", "turn_interrupted")]
        peer = IRCClient(int(endpoint.rsplit(":", 1)[1]), "runtimepeer")
        end = child.send_wait(f"/connect {outgoing}\r".encode(), b"outgoing connection added", start=end)
        links = accept_connections(upstream, 2)
        end = child.send_wait(f"/connect {outgoing}\r".encode(), b"outgoing connection already configured", start=end)
        end = child.send_wait(b"/disconnect\r", b"outgoing connections removed; hosting unchanged", start=end)
        for connection in links:
            connection.settimeout(2.0)
            while connection.recv(65536):
                pass
        end = child.send_wait(b"/server stop\r", b"hosting stopped; outgoing connections unchanged", start=end)
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            assert probe.connect_ex(("127.0.0.1", int(endpoint.rsplit(":", 1)[1]))) != 0
        end = child.send_wait_idle(b"\x03", b"turn interrupted", start=end)
        command = child.exit_now()
        arguments = command_arguments(command)
        assert "--no-listen" in arguments and "--no-client" in arguments
        assert "--listen" not in arguments and "--client" not in arguments
        log = events(session_id)
        assert not [event for event in log if event["type"] == "steering_added"]
        with Child.from_command(command) as resumed:
            resumed.wait(b"session id " + session_id[:8].encode())
            resumed.wait(PROMPT.rstrip())
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
                assert probe.connect_ex(("127.0.0.1", int(endpoint.rsplit(":", 1)[1]))) != 0
            ready, _, _ = select.select([upstream], [], [], 0.1)
            assert not ready, "resume resurrected an outgoing config default"
            resumed.exit_now()
    finally:
        if peer is not None:
            peer.close()
        for connection in links:
            connection.close()
        upstream.close()
        child.kill()


def test_network_resume_roles():
    upstream = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    upstream.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    upstream.bind(("127.0.0.1", 0))
    upstream.listen(8)
    upstream_endpoint = f"127.0.0.1:{upstream.getsockname()[1]}"

    before = session_ids()
    client = Child([
        "--no-color", "-c", upstream_endpoint,
        "-n", "clientagent", "-o", "clientop",
    ], chat_prompt("clientop"))
    assert session_ids() == before
    first_links = accept_connections(upstream, 2)
    switched = client.send_wait_idle(b"/rollout\r", "── rollout ──".encode())
    answered = client.send_wait_idle(b"ping\r", b"pong", start=switched)
    client_id = new_session(before)
    client_command = client.exit_now()
    for connection in first_links:
        connection.close()
    client_arguments = command_arguments(client_command)
    assert "--listen" not in client_arguments
    assert client_arguments.count("--client") == 1
    assert client_arguments[client_arguments.index("--client") + 1] == \
        upstream_endpoint
    resumed_client = Child.from_command(client_command)
    resumed_client.wait(b"session id " + client_id[:8].encode())
    resumed_client.wait(chat_prompt("clientop"))
    resumed_links = accept_connections(upstream, 2)
    resumed_client.exit_now()
    for connection in resumed_links:
        connection.close()

    server_port = free_port()
    server_endpoint = f"127.0.0.1:{server_port}"
    before = session_ids()
    server = Child([
        "--no-color", "-s", server_endpoint,
        "-n", "serveragent", "-o", "serverop", "-r", "lab",
    ], chat_prompt("serverop"))
    assert session_ids() == before
    peer = IRCClient(server_port, "firstpeer")
    peer.message("retained room message")
    server.wait("firstpeer › retained room message".encode())
    server.send_wait(b"resume setup\r", "serverop › resume setup".encode())
    server_id = new_session(before, server)
    peer.close()
    server.send(b"\x04")
    server_command = server.finish()
    server_arguments = command_arguments(server_command)
    assert server_arguments[server_arguments.index("--listen") + 1] == \
        server_endpoint
    assert "--client" not in server_arguments
    assert server_arguments[server_arguments.index("--room-name") + 1] == \
        "#lab"
    resumed_server = Child.from_command(server_command)
    resumed_server.wait(b"session id " + server_id[:8].encode())
    restored = resumed_server.wait("@firstpeer › retained room message".encode())
    replayed = resumed_server.wait("── history replayed ──".encode(), start=restored)
    assert b" history @firstpeer" not in resumed_server.buf
    assert resumed_server.buf[:replayed].count("── history replayed ──".encode()) == 1
    assert resumed_server.buf.count(b"retained room message") == 1
    resumed_server.wait(chat_prompt("serverop"))
    peer = IRCClient(server_port, "secondpeer")
    peer.wait(b" PRIVMSG #lab :retained room message\r\n")
    assert peer.buf.count(b" PRIVMSG #lab :retained room message\r\n") == 1
    peer.close()
    resumed_server.send(b"\x04")
    resumed_server.finish()
    assert len([event for event in events(server_id)
                if event["type"] == "irc_event" and
                event["data"]["text"] == "retained room message"]) == 1

    combined_port = free_port()
    combined_endpoint = f"127.0.0.1:{combined_port}"
    before = session_ids()
    combined = Child([
        "--no-color", "-s", combined_endpoint,
        "-c", upstream_endpoint,
        "-n", "combinedagent", "-o", "combinedop", "-r", "lab",
    ], chat_prompt("combinedop"))
    assert session_ids() == before
    first_links = accept_connections(upstream, 2)
    peer = IRCClient(combined_port, "combinedpeer")
    combined.send_wait(b"resume setup\r", "combinedop › resume setup".encode())
    combined_id = new_session(before, combined)
    peer.close()
    combined.send(b"\x04")
    combined_command = combined.finish()
    for connection in first_links:
        connection.close()
    combined_arguments = command_arguments(combined_command)
    assert combined_arguments[combined_arguments.index("--listen") + 1] == \
        combined_endpoint
    assert combined_arguments[combined_arguments.index("--client") + 1] == \
        upstream_endpoint
    resumed_combined = Child.from_command(combined_command)
    resumed_combined.wait(b"session id " + combined_id[:8].encode())
    resumed_combined.wait(chat_prompt("combinedop"))
    resumed_links = accept_connections(upstream, 2)
    peer = IRCClient(combined_port, "resumedpeer")
    peer.close()
    resumed_combined.send(b"\x04")
    resumed_combined.finish()
    for connection in resumed_links:
        connection.close()
    upstream.close()


def test_network_collision_prompts():
    port = free_port()
    address = f"127.0.0.1:{port}"
    children = []
    peer = None
    env = dict(os.environ, USER="root")
    try:
        server = Child(["--no-color", "-vvvvvv", "-s", address, "-r", "lab"], env=env)
        children.append(server)
        server.wait(chat_prompt("root0"))
        names_end = server.send_wait(b"/names\r", b"model nick: agent0")
        server.wait(b"operator nick: root0", start=names_end)
        for suffix in (1, 2):
            client = Child(["--no-color", "-c", address], env=env)
            children.append(client)
            deadline = time.monotonic() + 8.0
            while (chat_prompt(f"root{suffix}") not in client.buf and
                   f"\x1b[16C{suffix}".encode() not in client.buf):
                remaining = deadline - time.monotonic()
                assert remaining > 0, f"collision nick was not painted: {bytes(client.buf)!r}"
                client.read_once(remaining)
            client.send_wait(b"/names\r", f"model agent{suffix} operator root{suffix}".encode())
            assert b"agent01" not in client.buf
            assert b"root01" not in client.buf
        peer = IRCClient(port, "visitor", agent=True)
        for child in children:
            child.wait(b"visitor joined")
            child.drain()
        starts = [len(child.buf) for child in children]
        peer.sock.sendall(b"NICK visitor2\r\n")
        peer.wait(b" NICK :visitor2\r\n")
        for child, start in zip(children, starts):
            child.wait("visitor is now known as · visitor2".encode(), start=start)
            child.drain()
            assert child.buf[start:].count(b"visitor is now known as") == 1
        # Escaped wire diagnostics may be four times longer than the wire.
        starts = [len(child.buf) for child in children]
        peer.message("\x02" * 8100 + "long trace payload")
        message = "visitor2 › long trace payload".encode()
        for child, start in zip(children, starts):
            child.wait(message, start=start)
            child.drain()
            assert child.buf[start:].count(message) == 1
        for suffix, child in enumerate(children):
            child.send_wait(b"resume setup\r", f"root{suffix} › resume setup".encode())
    finally:
        if peer:
            peer.close()
        for child in reversed(children):
            child.send(b"\x04")
            arguments = command_arguments(child.finish())
            assert "--model-nick" not in arguments
            assert "--operator-nick" not in arguments


def test_network_live_nick_prompt():
    upstream = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    upstream.bind(("127.0.0.1", 0))
    upstream.listen(2)
    before = session_ids()
    child = Child(["--no-color", "-c",
                   f"127.0.0.1:{upstream.getsockname()[1]}",
                   "-n", "agent", "-o", "operator"])
    links = []
    try:
        child.wait(chat_prompt("operator"))
        assert session_ids() == before
        links = accept_connections(upstream, 2)
        for link in links:
            link.settimeout(4.0)
            wire = bytearray()
            while b"USER " not in wire:
                chunk = link.recv(4096)
                if not chunk:
                    child.drain(0.05)
                    raise AssertionError(f"IRC registration closed: {bytes(child.buf)!r}")
                wire.extend(chunk)
            nick = re.search(rb"NICK (\w+)\r\n", wire)[1].decode()
            accepted = nick + "7"
            link.sendall((f":fake 001 {accepted} :welcome\r\n"
                          f":fake 005 {accepted} SAJROOM=#lab :supported\r\n"
                          f":fake 376 {accepted} :end\r\n").encode())
            wire = bytearray()
            while b"JOIN #lab\r\n" not in wire:
                chunk = link.recv(4096)
                if not chunk:
                    child.drain(0.05)
                    raise AssertionError(f"IRC join closed: {bytes(child.buf)!r}")
                wire.extend(chunk)
            link.sendall((f":{accepted}!u@fake JOIN #lab\r\n"
                          f":fake 353 {accepted} = #lab :@operator7 agent7\r\n"
                          f":fake 366 {accepted} #lab :end\r\n"
                          ":fake BATCH +h chathistory #lab\r\n"
                          ":fake BATCH -h\r\n").encode())
        child.wait(f"7@{socket.gethostname()}".encode())
        child.drain()
        start = len(child.buf)
        child.send_wait(b"@ag\t", b"@agent7 ", start=start)
        child.send(b"\x03")
        child.drain(0.03)
        # Preserve a draft and its cursor through a live rename.
        child.send(b"/stats\x1b[D")
        child.drain()
        start = len(child.buf)
        for link in links:
            link.sendall(b":operator7!u@fake NICK :operator8\r\n"
                         b":agent7!u@fake NICK :agent8\r\n")
        deadline = time.monotonic() + 8.0
        while (chat_prompt("operator8") not in child.buf[start:] and
               b"\x1b[20C8" not in child.buf[start:]):
            remaining = deadline - time.monotonic()
            assert remaining > 0, "renamed idle prompt was not painted"
            child.read_once(remaining)
        child.wait(b"/stats", start=start)
        status_end = child.send_wait(b"u\r", b"verbosity: 0", start=start)
        child.wait(chat_prompt("operator8"), start=status_end)
        child.drain()
        assert child.buf[start:].count(b"operator7 is now known as") == 1
        assert child.buf[start:].count(b"agent7 is now known as") == 1
        start = len(child.buf)
        child.send_wait(b"@ag\t", b"@agent8 ", start=start)
        child.send(b"\x03")
        child.drain(0.03)
        # Local input is attributed to the accepted operator and the model's
        # request context includes a fresh snapshot with both accepted nicks.
        start = len(child.buf)
        child.send_wait(b"network_view_stream\r", "@operator8 › network_view_stream".encode(), start=start)
        session_id = new_session(before, child)
        end = child.wait("◴".encode(), start=start)
        visible = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", child.buf[start:end])
        assert re.search("[0-9]{2}:[0-9]{2}:[0-9]{2} operator8@".encode(),
                         visible), visible
        child.send(b"/stats\x1b[D")
        child.drain(0.03)
        for link in links:
            link.sendall(b":operator8!u@fake NICK :operator9\r\n"
                         b":agent8!u@fake NICK :agent9\r\n")
        deadline = time.monotonic() + 8.0
        while (chat_prompt("operator9") not in child.buf[start:] and
               b"\x1b[20C9" not in child.buf[start:]):
            remaining = deadline - time.monotonic()
            assert remaining > 0, "renamed active prompt was not painted"
            child.read_once(remaining)
        renamed = len(child.buf)
        assert b"/stats" in child.buf[end:renamed]
        child.send_wait(b"u\r", b"verbosity: 0", start=renamed)
        deadline = time.monotonic() + 8.0
        while True:
            log = events(session_id)
            turns = [event["data"]["turn_id"] for event in log
                     if event["type"] == "turn_started" and
                     "network_view_stream" in event["data"]["text"]]
            if any(event["type"] == "turn_completed" and
                   event["data"]["turn_id"] in turns for event in log):
                break
            assert time.monotonic() < deadline, bytes(child.buf)
            child.drain(0.05)
        snapshots = [event["data"]["text"] for event in log
                     if event["type"] == "irc_snapshot" and
                     event["data"]["reason"] == "nick"]
        assert any("model nick: agent8\noperator nick: operator8\n" in text
                   for text in snapshots)
        assert any("model nick: agent9\noperator nick: operator9\n" in text
                   for text in snapshots)
        assert b"model-output-one" not in child.buf
        assert child.buf.count(b"network stream acknowledged") == 1
        # Nick notifications may start a background turn after this one.
        # EOF interrupts it and exits; /exit is an idle-only command.
        child.send(b"\x04")
        command = child.finish()
        child = None
        arguments = command_arguments(command)
        assert arguments[arguments.index("--model-nick") + 1] == "agent"
        assert arguments[arguments.index("--operator-nick") + 1] == "operator"
        assert "operator8" not in command
    finally:
        if child:
            child.kill()
        for link in links:
            link.close()
        upstream.close()


def test_prompt_identity_is_terminal_safe():
    unsafe_model = "unsafe\x1bmodel"
    unsafe_effort = "odd\u202eeffort"
    visible = b"unsafe\\x1Bmodel/odd\\u{202E}effort"
    child = Child(["-m", unsafe_model, "--effort", unsafe_effort], b" openai/" + visible + "   0% › ".encode())
    assert unsafe_model.encode() not in child.buf
    assert unsafe_effort.encode() not in child.buf
    child.send_wait(b"ping\r", "»".encode())
    answer_end = child.wait(b"pong")
    child.exit_cleanly(answer_end)

    turn = one(events(child.session_id()), "turn_started")
    assert turn["data"]["config"]["model"] == unsafe_model
    assert turn["data"]["config"]["effort"] == unsafe_effort


def test_model_message_corrections_are_private_and_specific():
    cases = [
        (
            "empty_message_recovery",
            "You tried to send an empty assistant message. "
            "Send nonempty text or take another action.",
            b"empty message recovered",
        ),
        (
            "oversized_message_recovery",
            "You tried to send an oversized assistant message. "
            "Send a shorter message or take another action.",
            b"oversized message recovered",
        ),
    ]
    for prompt, correction, recovered in cases:
        child = Child([], DEFAULT_IDLE_PROMPT)
        start = len(child.buf)
        recovered_end = child.send_wait(prompt.encode() + b"\r", recovered, start=start)
        child.wait(DEFAULT_ACCOUNTED_IDLE_PROMPT, start=recovered_end)
        visible = bytes(child.buf[start:])
        assert correction.encode() not in visible
        child.exit_cleanly(recovered_end)

        log = events(child.session_id())
        corrections = [
            event for event in log
            if event["type"] == "response_output_correction"
        ]
        assert not [event for event in log if event["type"] == "response_failed"]
        assert len(corrections) == 1
        assert corrections[0]["data"]["text"] == correction
        assert len([event for event in log
                    if event["type"] == "response_started"]) == 2
        assert len([event for event in log
                    if event["type"] == "turn_completed"]) == 1


def test_network_view_routing_and_atomic_catchup():
    before = session_ids()
    port = free_port()
    endpoint = f"127.0.0.1:{port}"
    network_workspace = (
        Path(os.environ["SNAJPAGENT_TEST_ROOT"]) / "network-routing-workspace"
    )
    network_workspace.mkdir()
    child = Child([
        "-s", endpoint, "-n", "agent", "-o", "localop",
        "-r", "lab", "-C", str(network_workspace), "--no-color",
    ])
    human = None
    peer_agent = None
    exited = False
    network_idle = f"localop@{socket.gethostname()} : ".encode()
    rollout_idle = f" openai/{DEFAULT_MODEL}/medium   ?% › ".encode()

    def queue_rollout_and_enter(trigger, first, second, switch):
        chat_start = len(child.buf)
        wire_start = len(human.buf)
        for item in ("one", "two"):
            message = f"agent: {trigger}_{item}"
            peer_agent.message(message)
            human.wait(
                f"PRIVMSG #lab :{message}\r\n".encode(),
                start=wire_start,
            )
            wait_turn_completed(child, session_id, f"{trigger}_{item}")
        child.wait(network_idle, start=chat_start)
        child.drain()
        hidden = re.sub(LIVE_GAP, b"", child.buf[chat_start:])
        assert first not in hidden
        assert second not in hidden

        switch_start = len(child.buf)
        boundary_end = child.send_wait(switch, "── rollout ──".encode(), start=switch_start)
        prompt_end = child.wait(rollout_idle, start=boundary_end)
        transition = bytes(child.buf[boundary_end:prompt_end])
        prompt_at = transition.find(rollout_idle.rstrip())
        assert prompt_at >= 0, transition
        catchup = re.sub(LIVE_GAP, b"", transition[:prompt_at])
        assert network_idle not in catchup, catchup
        assert rollout_idle not in catchup, catchup
        assert catchup.count(first) == 1, catchup
        assert catchup.count(second) == 1, catchup
        assert catchup.find(first) < catchup.find(second), catchup
        assert transition.count(rollout_idle.rstrip()) == 1, transition
        return prompt_end

    try:
        child.wait(network_idle)
        assert session_ids() == before
        child.send_wait(b"session setup\r", "localop › session setup".encode())
        session_id = new_session(before, child)
        human = IRCClient(port, "remoteop")
        peer_agent = IRCClient(port, "peerbot", agent=True)

        # Membership events also start turns. Finish setup before testing two
        # distinct messages, or the second may legitimately steer a join turn.
        for nick in ("remoteop", "peerbot"):
            wait_turn_completed(child, session_id, f"event=join sender={nick}")

        tab_first = b"tab-catchup-one"
        tab_second = b"tab-catchup-two"
        queue_rollout_and_enter(
            "network_prompt_catchup_tab", tab_first, tab_second, b"\t"
        )

        same_start = len(child.buf)
        child.send_wait(b"/rollout\r", rollout_idle, start=same_start)
        child.drain()
        same_view = bytes(child.buf[same_start:])
        assert "── rollout ──".encode() not in same_view, same_view
        assert tab_first not in same_view, same_view
        assert tab_second not in same_view, same_view
        assert same_view.count(rollout_idle + b"/rollout\r\n") == 1, same_view
        assert same_view.count(rollout_idle) == 2, same_view  # Echo plus one idle prompt.

        chat_start = len(child.buf)
        chat_boundary = child.send_wait(b"\t", "── chat ──".encode(), start=chat_start)
        child.wait(network_idle, start=chat_boundary)

        slash_first = b"slash-catchup-one"
        slash_second = b"slash-catchup-two"
        queue_rollout_and_enter(
            "network_prompt_catchup_slash",
            slash_first,
            slash_second,
            b"/rollout\r",
        )

        idle_start = len(child.buf)
        idle_wire_start = len(human.buf)
        child.send(b"network_zero\r")
        submitted_idle = rollout_idle + b"network_zero"
        child.wait(submitted_idle, start=idle_start)
        answer_end = child.wait(b"network zero local only", start=idle_start)
        child.wait(rollout_idle, start=answer_end)
        idle_output = bytes(child.buf[idle_start:])
        assert idle_output.count(submitted_idle) == 1, idle_output
        human.drain()
        assert (b"PRIVMSG #lab :network_zero\r\n" not in
                human.buf[idle_wire_start:])

        active_start = len(child.buf)
        active_wire_start = len(human.buf)
        child.send(b"slow\r")
        submitted_start = rollout_idle + b"slow"
        child.wait(submitted_start, start=active_start)
        child.wait(b"working slowly", start=active_start)
        child.wait("»".encode(), start=active_start)
        steer_start = len(child.buf)
        answer_end = child.send_wait(b"rollout active steer\r", b"steered: rollout active steer",
                                start=steer_start)
        child.wait(rollout_idle, start=answer_end)
        active_output = bytes(child.buf[active_start:])
        active_labels = re.findall(
            "(?:◴|◷|◶|◵)  [0-9]{2}:[0-9]{2}:[0-9]{2}".encode() +
            re.escape(DEFAULT_ACTIVE_PROMPT + b"rollout active steer"),
            active_output,
        )
        assert active_output.count(submitted_start) == 1, active_output
        assert len(active_labels) == 1
        human.drain()
        assert b"PRIVMSG #lab :slow\r\n" not in human.buf[active_wire_start:]
        assert (b"PRIVMSG #lab :rollout active steer\r\n" not in
                human.buf[active_wire_start:])

        backlog_start = len(child.buf)
        backlog_wire_start = len(human.buf)
        human.message("chat-route-backlog")
        human.wait(b"PRIVMSG #lab :chat-route-backlog\r\n",
                   start=backlog_wire_start)
        child.drain()
        assert b"chat-route-backlog" not in child.buf[backlog_start:], bytes(child.buf[backlog_start:])
        chat_boundary = child.send_wait(b"/chat\r", "── chat ──".encode(), start=backlog_start)
        backlog_end = child.wait(b"chat-route-backlog", start=chat_boundary)
        child.wait(network_idle, start=backlog_end)
        assert child.buf[chat_boundary:].count(b"chat-route-backlog") == 1

        chat_wire_start = len(human.buf)
        child.send(b"network_one\r")
        human.wait(b"PRIVMSG #lab :network_one\r\n", start=chat_wire_start)
        human.wait(b"PRIVMSG #lab :network one reply\r\n",
                   start=chat_wire_start)
        wait_turn_completed(child, session_id, "network_one")
        human.drain()
        assert (human.buf[chat_wire_start:].count(
                    b"PRIVMSG #lab :network_one\r\n") == 1)

        child.exit_now()
        exited = True
    finally:
        if peer_agent is not None:
            peer_agent.close()
        if human is not None:
            human.close()
        if not exited:
            child.kill()

    log = events(session_id)
    direct = [
        event for event in log
        if event["type"] == "turn_started" and
        event["data"]["text"] in ("network_zero", "slow")
    ]
    assert [event["data"]["text"] for event in direct] == [
        "network_zero", "slow"
    ]
    slow_turn = direct[1]["data"]["turn_id"]
    steering = turn_events(log, "steering_added", slow_turn)
    assert len(steering) == 1
    assert steering[0]["data"]["text"] == "rollout active steer"
    chat_turns = [
        event for event in log
        if event["type"] == "turn_started" and
        "network_one" in event["data"]["text"]
    ]
    assert len(chat_turns) == 1
    assert chat_turns[0]["data"]["text"] != "network_one"


def test_chat_mention_completion_and_steering():
    before = session_ids()
    port = free_port()
    child = Child(["-s", f"127.0.0.1:{port}", "-n", "agent", "-o", "localop",
                   "-r", "lab", "--no-color", "-v"])
    human = None
    try:
        child.wait(chat_prompt("localop"))
        assert session_ids() == before
        child.send_wait(b"session setup\r", "localop › session setup".encode())
        session_id = new_session(before, child)
        human = IRCClient(port, "remoteop")
        wait_turn_completed(child, session_id, "event=join sender=remoteop")
        for prompt, marker in (("slow", b"working slowly"),
                               ("managed_command_steer", b"fixture managed steering wait")):
            start = len(child.buf)
            boundary = child.send_wait_idle(b"/rollout\r", "── rollout ──".encode(), start=start)
            child.send_wait(prompt.encode() + b"\r", marker, start=start)
            turn = next(event for event in reversed(events(session_id))
                        if event["type"] == "turn_started")
            turn_id = turn["data"]["turn_id"]
            boundary = child.send_wait(b"/chat\r", "── chat ──".encode(), start=start)
            child.wait(chat_prompt("localop"), start=boundary)
            # A unique completion in the middle preserves punctuation and tail.
            wire_start = len(human.buf)
            child.send(b"hello @rem, tail" + b"\x1b[D" * 6 + b"\t\x05\r")
            human.wait(b"PRIVMSG #lab :hello @remoteop , tail\r\n", start=wire_start)
            human.message("ordinary operator chatter")
            human.message("@agent7 is someone else")
            human.wait(b"PRIVMSG #lab :@agent7 is someone else\r\n", start=wire_start)
            # Unmatched and ambiguous completion must not queue active drafts.
            child.send(b"@absent\t\t\x03@\t")
            choices_start = len(child.buf)
            child.send(b"\t")
            for nick in (b"@agent", b"@localop", b"@remoteop"):
                child.wait(nick, start=choices_start)
            child.send(b"\x03")
            child.drain(0.08)
            log = events(session_id)
            assert not [event for event in log if event["type"] == "queue_added"]
            assert not [event for event in log
                        if event["data"].get("turn_id") == turn_id and
                        event["type"] in ("steering_added", "response_interrupted",
                                          "turn_completed", "turn_interrupted")]
            # Completing and submitting the local agent does steer. The managed
            # fixture requires this exact instruction before terminating its handle.
            child.send(b"@ag\tterminate it\r")
            human.wait(b"PRIVMSG #lab :@agent terminate it\r\n", start=wire_start)
            wait_turn_completed(child, session_id, prompt)
            wait_turn_completed(child, session_id, "hello @remoteop , tail")
            log = events(session_id)
            admissions = [event for event in log if event["type"] == "irc_admitted"
                          and event["data"].get("steering", {}).get("turn_id") == turn_id]
            assert len(admissions) == 1, admissions
            sequences = admissions[0]["data"]["sequences"]
            received = [event["data"]["text"] for event in log
                        if event["type"] == "irc_event" and event["seq"] in sequences]
            assert any("@agent terminate it" in text for text in received), received
            assert all("ordinary operator chatter" not in text and "hello @remoteop" not in text
                       for text in received), received
            assert not [event for event in log if event["type"] == "queue_added"]
        child.send(b"\x04")
        child.finish()
        child = None
    finally:
        if human:
            human.close()
        if child:
            child.kill()


def test_network_chat_and_managed_mention():
    before = session_ids()
    port = free_port()
    endpoint = f"127.0.0.1:{port}"
    network_workspace = Path(os.environ["SNAJPAGENT_TEST_ROOT"]) / "network-workspace"
    network_workspace.mkdir()
    child = Child([
        "-s", endpoint, "-n", "agent", "-o", "localop",
        "-r", "lab", "-C", str(network_workspace), "--no-color",
    ])
    human = None
    peer_agent = None
    exited = False
    network_idle = f"localop@{socket.gethostname()} : ".encode()
    network_active = f"localop@{socket.gethostname()} : ".encode()
    network_rollout_idle = (
        f" openai/{DEFAULT_MODEL}/medium   0% › ".encode()
    )
    network_rollout_accounted_idle = (
        f" openai/{DEFAULT_MODEL}/medium   ?% › ".encode()
    )
    try:
        child.wait(network_idle)
        assert session_ids() == before

        view_start = len(child.buf)
        rollout_end = child.send_wait(b"\t", "── rollout ──".encode(), start=view_start)
        child.wait(network_rollout_idle, start=rollout_end)
        child.send(b"/rollout\r")
        child.drain()
        assert child.buf[view_start:].count("── rollout ──".encode()) == 1
        chat_end = child.send_wait(b"/chat\r", "── chat ──".encode(), start=rollout_end)
        child.wait(network_idle, start=chat_end)
        child.send(b"/chat\r")
        child.drain()
        assert child.buf[view_start:].count("── chat ──".encode()) == 1
        help_end = child.send_wait(b"/help\r", b"Empty Tab switch view", start=chat_end)
        child.wait(network_idle, start=help_end)

        draft_start = len(child.buf)
        draft_end = child.send_wait(b"x\t", b"x   ", start=draft_start)
        edit = bytes(child.buf[draft_start:draft_end])
        assert b"\x1b[2K" not in edit, edit
        assert network_idle not in edit, edit
        assert "── rollout ──".encode() not in edit
        clear_draft_incrementally(child, network_idle)

        child.send_wait(b"session setup\r", "localop › session setup".encode())
        session_id = new_session(before, child)
        human = IRCClient(port, "remoteop")
        assert (b" 332 remoteop #lab :" + str(network_workspace).encode() +
                b"\r\n") in human.buf
        peer_agent = IRCClient(port, "peerbot", agent=True)

        stream_start = len(child.buf)
        for nick in ("remoteop", "peerbot"):
            wait_turn_completed(child, session_id, f"event=join sender={nick}")
        model_wire_start = len(human.buf)
        child.send(b"network_view_stream\r")
        deadline = time.monotonic() + 4.0
        while not any(event["type"] == "turn_started" and
                      "network_view_stream" in event["data"]["text"]
                      for event in events(session_id)):
            if time.monotonic() >= deadline:
                raise AssertionError("network stream turn did not start")
            time.sleep(0.01)
        rollout_end = child.send_wait(b"\t", "── rollout ──".encode(), start=stream_start)
        child.wait(b"model-output-one", start=rollout_end)
        child.wait(network_active, start=stream_start)
        child.wait(b"model-output-two", start=rollout_end)

        chat_wire_start = len(human.buf)
        peer_agent.message("chat backlog")
        human.wait(b"PRIVMSG #lab :chat backlog\r\n", start=chat_wire_start)
        chat_end = child.send_wait(b"\t", "── chat ──".encode(), start=rollout_end)
        backlog_end = child.wait(b"chat backlog", start=chat_end)
        child.send(b"/chat\r")
        child.drain()
        assert child.buf[chat_end:].count(b"chat backlog") == 1
        human.wait(b"PRIVMSG #lab :network stream acknowledged\r\n",
                   start=model_wire_start)
        child.drain()
        assert b"model-output-three" not in child.buf[chat_end:]
        child.wait(network_idle, start=backlog_end)
        tail_end = child.send_wait(b"\t", b"model-output-three", start=chat_end)
        visible_stream = re.sub(LIVE_GAP, b"", child.buf[stream_start:tail_end])
        for fragment in (b"model-output-one", b"model-output-two",
                         b"model-output-three"):
            assert visible_stream.count(fragment) == 1, visible_stream
        chat_end = child.send_wait(b"\t", "── chat ──".encode(), start=tail_end)
        child.wait(network_idle, start=chat_end)
        assert (b"PRIVMSG #lab :model-output-one model-output-two "
                b"model-output-three\r\n" not in human.buf[model_wire_start:])

        rollout_end = child.send_wait(b"/rollout\r", "── rollout ──".encode(), start=chat_end)
        child.wait(network_rollout_accounted_idle, start=rollout_end)
        search_start = len(child.buf)
        child.send_wait(b"\x12network_view_stream",
            b"m': network_view_stream",
            start=search_start,
        )
        child.send_wait(b"\x07", network_rollout_accounted_idle, start=search_start)
        chat_end = child.send_wait(b"/chat\r", "── chat ──".encode(), start=search_start)
        child.wait(network_idle, start=chat_end)

        terminal_start = len(child.buf)
        wire_start = len(human.buf)
        child.send(b"network_zero\r")
        wait_turn_completed(child, session_id, "network_zero")
        child.wait(network_idle, start=terminal_start)
        child.drain()
        assert b"network zero local only" not in child.buf[terminal_start:]
        assert (b"PRIVMSG #lab :network zero local only\r\n" not in
                human.buf[wire_start:])

        verbose_end = child.send_wait(b"/verbose 1\r", b"verbosity: 1", start=terminal_start)
        child.wait(network_idle, start=verbose_end)
        wire_start = len(human.buf)
        child.send(b"network_one\r")
        human.wait(b"PRIVMSG #lab :network one reply\r\n", start=wire_start)
        wait_turn_completed(child, session_id, "network_one")
        verbose_end = child.wait(b"network one reply", start=verbose_end)
        child.wait(network_idle, start=verbose_end)

        wire_start = len(human.buf)
        tool_start = len(child.buf)
        child.send(b"network_tool\r")
        human.wait(b"PRIVMSG #lab :network tool complete\r\n", start=wire_start)
        wait_turn_completed(child, session_id, "network_tool")
        child.send_wait(b"/rollout\r", b"\xe2\x86\x92 exec_command", start=tool_start)
        child.wait(PROMPT.rstrip(), start=tool_start)
        child.drain()
        assert b"  arguments:" not in child.buf[tool_start:]
        assert b"fixture command succeeded" not in child.buf[tool_start:]
        child.send_wait(b"/chat\r", "── chat ──".encode(), start=tool_start)

        operator_start = len(child.buf)
        wire_start = len(human.buf)
        human.message("network_operator")
        human.wait(b"PRIVMSG #lab :network operator reply\r\n", start=wire_start)
        wait_turn_completed(child, session_id, "network_operator")
        child.wait(network_idle, start=operator_start)

        mention_start = len(child.buf)
        wire_start = len(human.buf)
        peer_agent.message("agent: network_mention")
        human.wait(b"PRIVMSG #lab :network mention reply\r\n", start=wire_start)
        wait_turn_completed(child, session_id, "network_mention")
        child.wait(network_idle, start=mention_start)

        count_start = len(child.buf)
        child.send(b"network_count_wait\r")
        deadline = time.monotonic() + 4.0
        while True:
            try:
                count_started = any(
                    event["type"] == "turn_started" and
                    "network_count_wait" in event["data"]["text"]
                    for event in events(session_id)
                )
            except json.JSONDecodeError:
                count_started = False
            if count_started:
                break
            if time.monotonic() >= deadline:
                raise AssertionError("network count turn did not start")
            time.sleep(0.01)
        wire_start = len(human.buf)
        peer_agent.message("agent: network count mention")
        human.wait(b"PRIVMSG #lab :network count mention reply\r\n",
                   start=wire_start)
        wait_turn_completed(child, session_id, "network_count_wait")
        child.wait(network_idle, start=count_start)

        reminder_start = len(child.buf)
        wire_start = len(human.buf)
        child.send(b"@agent network_reminder\r")
        human.wait(b"PRIVMSG #lab :network reminder reply\r\n", start=wire_start)
        wait_turn_completed(child, session_id, "network_reminder")
        child.wait(network_idle, start=reminder_start)

        verbose_end = child.send_wait(b"/verbose 2\r", b"verbosity: 2", start=verbose_end)
        child.wait(network_idle, start=verbose_end)
        commentary_start = len(child.buf)
        wire_start = len(human.buf)
        child.send(b"network_commentary\r")
        human.wait(b"PRIVMSG #lab :network commentary reply\r\n",
                   start=wire_start)
        wait_turn_completed(child, session_id, "network_commentary")
        child.send_wait(b"/rollout\r", b"\xe2\x80\xa2 network local planning", start=verbose_end)
        child.send_wait(b"/chat\r", "── chat ──".encode(), start=verbose_end)
        child.wait(network_idle, start=commentary_start)

        wire_start = len(human.buf)
        managed_view = child.send_wait(b"network_managed\r\t", "── rollout ──".encode(), start=verbose_end)
        child.wait(b"fixture process is still running", start=managed_view)
        peer_agent.message("agent: network managed mention")
        try:
            managed_end = human.wait(
                b"PRIVMSG #lab :network managed reaction\r\n",
                start=wire_start,
            )
        except AssertionError as exc:
            child.drain()
            raise AssertionError(
                f"{exc}; terminal={bytes(child.buf[verbose_end:])!r}"
            ) from exc
        managed_complete = child.wait(b"network managed local completion",
                                      start=verbose_end)
        wait_turn_completed(child, session_id, "network_managed")
        child.wait(PROMPT.rstrip(), start=managed_complete)
        assert managed_end > wire_start
        assert (b"PRIVMSG #lab :network managed local completion\r\n" not in
                human.buf[wire_start:])
        child.send_wait(b"/chat\r", "── chat ──".encode(), start=managed_view)

        compact_start = len(child.buf)
        compact_prompt = child.send_wait(b"/compact\r", network_idle, start=compact_start)
        child.drain()
        assert COMPACTED not in child.buf[compact_start:]
        compact_end = child.send_wait(b"/rollout\r", COMPACTED, start=compact_prompt)
        child.wait(PROMPT.rstrip(), start=compact_end)
        chat_end = child.send_wait(b"/chat\r", "── chat ──".encode(), start=compact_end)
        assert child.buf[compact_prompt:chat_end].count(COMPACTED) == 1
        child.wait(network_idle, start=chat_end)
        wire_start = len(human.buf)
        child.send(b"network_one\r")
        human.wait(b"PRIVMSG #lab :network one reply\r\n", start=wire_start)

        wait_turn_completed(child, session_id, "network_one")
        child.exit_now()
        exited = True
    finally:
        if peer_agent is not None:
            peer_agent.close()
        if human is not None:
            human.close()
        if not exited:
            child.kill()

    log = events(session_id)
    turns = [event for event in log if event["type"] == "turn_started"]
    snapshots = [event for event in log if event["type"] == "irc_snapshot"]
    join_snapshot = next(
        event for event in snapshots if event["data"]["reason"] == "join"
    )
    assert join_snapshot["seq"] < turns[0]["seq"]
    assert "room: #lab" in join_snapshot["data"]["text"]
    compact_completed = next(
        event for event in log if event["type"] == "compaction_completed"
    )
    compact_snapshot = next(
        event for event in snapshots
        if event["data"]["reason"] == "compaction" and
        event["seq"] > compact_completed["seq"]
    )
    assert compact_snapshot["seq"] == compact_completed["seq"] + 1
    assert next(
        event for event in log
        if event["type"] == "response_started" and
        event["seq"] > compact_snapshot["seq"]
    )

    count_turn = next(
        event for event in turns if "network_count_wait" in event["data"]["text"]
    )
    count_admission = next(event for event in log
        if event["type"] == "irc_admitted" and
        event["data"].get("steering", {}).get("turn_id") == count_turn["data"]["turn_id"])
    count_steering = count_admission["data"]["steering"]
    assert count_admission["data"]["sequences"]

    count_start = next(
        event for event in log
        if event["type"] == "response_started" and
        event["data"]["turn_id"] == count_turn["data"]["turn_id"]
    )
    assert count_start["data"]["steering_ids"] == [
        count_steering["steering_id"]
    ]
    reminder_turn = next(
        event for event in turns if "network_reminder" in event["data"]["text"]
    )
    reminders = turn_events(log, "irc_reply_reminder", reminder_turn["data"]["turn_id"])
    assert len(reminders) == 1
    reminder_responses = turn_events(log, "response_started", reminder_turn["data"]["turn_id"])
    assert len(reminder_responses) == 4
    failed_response = next(
        event for event in log
        if event["type"] == "response_completed" and
        event["data"]["turn_id"] == reminder_turn["data"]["turn_id"] and
        event["data"]["cycle"] == 1
    )
    failed_call = failed_response["data"]["items"][0]["call_id"]
    failed_send = next(
        event for event in log
        if event["type"] == "tool_finished" and
        event["data"]["call_id"] == failed_call
    )
    assert failed_send["data"]["result"]["status"] == "failed"
    assert (failed_send["data"]["result"]["model_text"] ==
            "text must be UTF-8 text of 1..2097152 bytes without NUL.")

    zero_turn = next(
        event for event in turns if "network_zero" in event["data"]["text"]
    )
    zero_reminders = turn_events(log, "irc_reply_reminder", zero_turn["data"]["turn_id"])
    zero_responses = turn_events(log, "response_started", zero_turn["data"]["turn_id"])
    assert not zero_reminders
    assert len(zero_responses) == 1

    managed_turn = next(
        event for event in turns if "network_managed" in event["data"]["text"]
    )
    turn_id = managed_turn["data"]["turn_id"]
    admitted = [e for e in log if e["type"] == "irc_admitted" and
                e["data"].get("steering", {}).get("turn_id") == turn_id]
    assert len(admitted) == 1 and admitted[0]["data"]["sequences"]
    source = next(e for e in log if e["type"] == "irc_event" and
                  e["seq"] in admitted[0]["data"]["sequences"] and
                  "network managed mention" in e["data"]["text"])
    assert source["data"]["urgent"]
    assert source["data"]["stream"] in admitted[0]["data"]["steering"]["text"]
    completed = turn_events(log, "response_completed", turn_id)
    assert [event["data"]["cycle"] for event in completed] == [1, 2, 3, 4]
    cycle2_call = completed[1]["data"]["items"][0]["call_id"]
    assert [item["name"] for item in completed[2]["data"]["items"]] == [
        "irc_send", "write_stdin"
    ]
    superseded = next(
        event for event in log
        if event["type"] == "tool_finished" and
        event["data"]["call_id"] == cycle2_call
    )
    assert superseded["data"]["result"]["status"] == "not_run"
    assert superseded["data"]["result"]["reason"] == "superseded_by_steering"
    cycle3_ids = [item["call_id"] for item in completed[2]["data"]["items"]]
    cycle3_finished = [
        event for event in log
        if event["type"] == "tool_finished" and
        event["data"]["call_id"] in cycle3_ids
    ]
    assert [event["data"]["call_id"] for event in cycle3_finished] == cycle3_ids
    assert all(event["data"]["result"]["status"] == "succeeded"
               for event in cycle3_finished)

def test_ctrl_d_exit():
    for prompt in (None, b"queue_slow", b"engine_blocked", b"/goal slow goal",
                   b"slow"):
        before = session_ids()
        with Child([], ready=DEFAULT_IDLE_PROMPT) as child:
            if prompt:
                child.send_wait(prompt + b"\r", b"engine-block-start" if prompt == b"engine_blocked"
                           else b"working on goal" if prompt.startswith(b"/goal")
                           else b"working slowly")
            # Nonempty Ctrl-D deletes at the cursor, but does not exit at EOL.
            start = len(child.buf)
            child.send(b"pxing\x1b[H\x1b[C\x04\x1b[F\x04")
            child.drain(0.1)
            assert os.waitpid(child.pid, os.WNOHANG) == (0, 0)
            if prompt == b"queue_slow":
                child.send_wait(b"\t", b"queued (/next or /q c) " + PROMPT + b"ping", start=start)
            else:
                child.send(b"\x7f" * 4)
            if prompt == b"slow":
                # Canonical Ctrl-D is read(0), as in the cooked fallback.
                attrs = termios.tcgetattr(child.fd)
                attrs[3] |= termios.ICANON
                termios.tcsetattr(child.fd, termios.TCSANOW, attrs)
            child.send_wait(b"\x04", RESUME_HEADER if prompt else b"\x1b[?2004l",
                       timeout=4.0 if prompt == b"engine_blocked"
                       else 1.0)
            flags = termios.tcgetattr(child.fd)[3]
            assert flags & termios.ICANON and flags & termios.ECHO
            command = child.finish(expect_resume=bool(prompt))
            if not prompt:
                assert session_ids() == before
                continue
            log = events(new_session(before))
            if prompt:
                assert not [e for e in log if e["type"] == "turn_interrupted"]
                assert len([e for e in log if e["type"] == "turn_started"]) == 1
                assert not [e for e in log if e["type"] == "turn_completed"], (prompt, log)
            if prompt == b"queue_slow":
                assert one(log, "future_turn_queued")["data"]["text"] == "ping"
            if prompt == b"/goal slow goal":
                assert not [e for e in log if e["type"] == "goal_paused"]
            if prompt in (b"queue_slow", b"/goal slow goal"):
                with Child.from_command(command) as resumed:
                    if prompt == b"/goal slow goal":
                        active = resumed.wait(b": active")
                        done = resumed.wait(b"goal done", start=active)
                        resumed.exit_cleanly(done)
                        log = events(command_arguments(command)[-1])
                        assert one(log, "goal_completed")["data"]["goal_id"] == one(log, "goal_started")["data"]["goal_id"]
                        assert not [e for e in log if e["type"] in ("goal_paused", "goal_resumed")]
                        continue
                    answer = resumed.wait(b"pong")
                    resumed.exit_cleanly(answer)
                    log = events(command_arguments(command)[-1])
                    turns = [e["data"] for e in log if e["type"] == "turn_started"]
                    assert len(turns) == 2 and turns[1]["input_kind"] == "queued"
            assert b"engine-block-end" not in child.buf


def test_goal_orderly_quit_resume():
    for mode in ("eof", "five-ctrl-c", "sigterm", "sighup", "host"):
        before = session_ids()
        args = []
        if mode == "host":
            args = ["-v", "--listen", f"localhost:{free_port()}", "--no-client",
                    "-n", "goalagent", "-o", "goalop", "-r", "lab"]
        with Child(args, ready=chat_prompt("goalop") if mode == "host" else PROMPT.rstrip()) as child:
            if mode == "host":
                switched = child.send_wait_idle(b"/rollout\r", "── rollout ──".encode())
            child.send_wait(b"/goal slow goal\r", b"working on goal")
            session_id = new_session(before)
            child.send(b"/goal lock\r")
            # The notice is queued behind the open response. Wait for the
            # durable lock, not its eventual rendering at the response boundary.
            deadline = time.monotonic() + 4.0
            while not any(e["type"] == "goal_lock_changed" for e in events(session_id)):
                assert time.monotonic() < deadline, bytes(child.buf)
                child.read_once(0.01)
            expected = 0
            if mode == "eof":
                attrs = termios.tcgetattr(child.fd)
                attrs[3] |= termios.ICANON
                termios.tcsetattr(child.fd, termios.TCSANOW, attrs)
                child.send(b"\x04")
            elif mode in ("sigterm", "sighup"):
                signum = signal.SIGTERM if mode == "sigterm" else signal.SIGHUP
                os.kill(child.pid, signum)
                expected = 128 + signum
            elif mode == "five-ctrl-c":
                # A turn-only Ctrl-C deliberately pauses first; a later exit
                # must preserve that pause, not turn it back into an active goal.
                child.send_wait(b"\x03", b"Goal paused at the current turn boundary")
                child.send(b"\x03" * 4)
            else:
                child.send(b"\x04")
            child.wait(b"Goal wording locked against model changes")
            child.wait(RESUME_HEADER, timeout=4.0)
            command = child.finish(expected=expected)
        stopped = events(session_id)
        goal = one(stopped, "goal_started")["data"]
        assert one(stopped, "goal_lock_changed")["data"]["locked"]
        assert not [e for e in stopped if e["type"] in ("goal_resumed", "goal_completed")]
        if mode == "five-ctrl-c":
            assert one(stopped, "goal_paused")["data"]["reason"] == "user"
        else:
            assert not [e for e in stopped if e["type"] == "goal_paused"]
        if mode == "five-ctrl-c":
            one(stopped, "turn_interrupted")
        else:
            assert not [e for e in stopped if e["type"] == "turn_interrupted"]
        with Child.from_command(command) as resumed:
            if mode == "five-ctrl-c":
                restored = resumed.wait(f"goal {goal['goal_id'][:8]}: paused · wording locked".encode())
                resumed.wait_idle_prompt(start=restored)
                resumed.exit_now()
                assert events(session_id) == stopped
                print("explicit Ctrl-C pause then quit/resume: ok", flush=True)
                continue
            restored = resumed.wait(f"goal {goal['goal_id'][:8]}: active · wording locked".encode())
            resumed.wait(b"slow goal", start=restored)
            deadline = time.monotonic() + 8.0
            while not any(e["type"] == "turn_completed" for e in events(session_id)[len(stopped):]):
                assert time.monotonic() < deadline, bytes(resumed.buf)
                resumed.read_once(0.05)
            if mode == "host":
                # goal_completed is a tool event; wait for the enclosing turn
                # and a fresh idle prompt before issuing an idle-only /exit.
                switched = resumed.send_wait_idle(b"/rollout\r", "── rollout ──".encode(), start=restored)
                resumed.exit_now()
            else:
                done = resumed.wait(b"goal done", start=restored)
                resumed.exit_cleanly(done)
        restored_log = events(session_id)
        assert one(restored_log, "goal_started")["data"] == goal
        assert one(restored_log, "goal_completed")["data"]["goal_id"] == goal["goal_id"]
        assert not [e for e in restored_log if e["type"] in ("goal_paused", "goal_resumed", "goal_reworded")]
        print(f"goal orderly quit/resume {mode}: ok", flush=True)


def test_five_ctrl_c_exit():
    for prompt in (None, b"slow", b"engine_blocked"):
        before = session_ids()
        child = Child([])
        child.wait_idle_prompt()
        if prompt:
            child.send_wait(prompt + b"\r", b"engine-block-start" if prompt == b"engine_blocked"
                       else b"working slowly")
        child.send(b"\x03" * 4)
        child.drain(0.1)
        assert os.waitpid(child.pid, os.WNOHANG) == (0, 0)
        child.send_wait(b"\x03", RESUME_HEADER if prompt else b"\x1b[?2004l", timeout=4.0)
        child.finish(expect_resume=bool(prompt))
        if not prompt:
            assert session_ids() == before


def test_ctrl_c_sequence_reset():
    before = session_ids()
    with Child([], ready=DEFAULT_IDLE_PROMPT) as child:
        child.send(b"\x03" * 4)
        child.drain(2.1)
        child.send(b"\x03")
        child.drain(0.05)
        assert os.waitpid(child.pid, os.WNOHANG) == (0, 0)
        child.send(b"x" + b"\x03" * 4)
        child.drain(0.05)
        assert os.waitpid(child.pid, os.WNOHANG) == (0, 0)
        child.send_wait(b"\x03", b"\x1b[?2004l")
        child.finish(expect_resume=False)
        assert session_ids() == before


def test_blank_enter_during_engine_stall():
    # Fill the engine FIFO with ordinary controls. Local blank Enter must still
    # paint immediately, without steering, history writes or fabricated work.
    for term in ("xterm", "dumb"):
        with Child([], ready=DEFAULT_IDLE_PROMPT, term=term) as child:
            child.send_wait(b"engine_blocked\r", b"engine-block-start")
            child.send_wait(b"/status\r" * 32, b"input backlog is full", timeout=1.0)
            child.drain(0.05)
            start = len(child.buf)
            child.send(b"\r" * 40)
            deadline = time.monotonic() + 0.8
            while child.buf[start:].count(b"\n") < 40:
                remaining = deadline - time.monotonic()
                assert remaining > 0, bytes(child.buf[start:])
                child.read_once(remaining)
            assert b"\a" not in child.buf[start:]
            assert b"engine-block-end" not in child.buf
            end = child.wait(b"engine-block-end", start=start)
            child.exit_cleanly(end)
        log = events(new_session(child.sessions_before))
        assert [e["data"]["text"] for e in log if e["type"] == "turn_started"] == ["engine_blocked"]
        assert not [e for e in log if e["type"] in
                    ("turn_interrupted", "steering_added", "future_turn_queued")]


def test_full_input_queue_keeps_exit_live():
    for gesture in (b"\x03" * 5, b"\x15\x04"):
        with Child([], ready=DEFAULT_IDLE_PROMPT) as child:
            child.send_wait(b"engine_blocked\r", b"engine-block-start")
            child.send_wait(b"/verbose 1\r" * 32 + b"retained-draft\r", b"input backlog is full", timeout=1.0)
            child.wait(b"\a", timeout=1.0)
            child.send_wait(gesture, RESUME_HEADER, timeout=4.0)
            child.finish()


def test_history_lock_keeps_editing_live():
    before = session_ids()
    with Child([], ready=DEFAULT_IDLE_PROMPT) as child:
        with (Path(DOTDIR) / "prompt_history").open("r+") as history:
            fcntl.lockf(history, fcntl.LOCK_EX)
            start = child.send_wait(b"\x12", b"reverse-i-search")
            child.send_wait(b"locked-history", b"y': ", start=start, timeout=0.25)
            assert_bytes_in_order(child.buf[start:], b"locked-history")
            child.send(b"\x07draft-alive")
            child.drain(0.1)
            visible = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]| \x08|\r", b"", child.buf[start:])
            assert b"draft-alive" in visible, visible
            fcntl.lockf(history, fcntl.LOCK_UN)
        child.send(b"\x03")
        child.drain(0.1)
        child.exit_now(expect_resume=False)
        assert session_ids() == before


def test_editor_during_render_flood():
    for mode in ("--markdown", "--no-markdown"):
        with Child([mode], ready=DEFAULT_IDLE_PROMPT) as child:
            start = child.send_wait(b"render_flood\r", b"row-0000")
            child.send(b"live-draft")
            deadline = time.monotonic() + 0.25
            while b"live-draft" not in re.sub(
                    rb"\x1b\[[0-?]*[ -/]*[@-~]| \x08|\r", b"", child.buf[start:]):
                remaining = deadline - time.monotonic()
                assert remaining > 0, (mode, "rendering stopped local editing",
                                       bytes(child.buf[start:]))
                child.read_once(remaining)
            end = child.wait(b"flood-end", start=start)
            child.wait_idle_prompt(start=end)
            child.send(b"\x03")
            child.drain(0.1)
            child.exit_now()
        completed = one(events(child.session_id()), "response_completed")
        text = completed["data"]["items"][0]["text"]
        expected = "| row | text |\n| --- | --- |\n" + "".join(
            f"| row-{i:04} | **bold** and `code` |\n" for i in range(2048)
        ) + "\nflood-end\n"
        assert text == expected


def test_editor_during_blocked_engine(key=b"\r"):
    child = Child([])
    failure = None
    try:
        child.wait_idle_prompt()
        after = child.send_wait(b"engine_blocked\r", b"engine-block-start")
        tasks = Path(f"/proc/{child.pid}/task")
        if tasks.exists():
            # GCC TSan adds one instrumentation worker, not an application thread.
            tsan = "libtsan" in Path(f"/proc/{child.pid}/maps").read_text()
            assert len(list(tasks.iterdir())) == 2 + int(tsan)
        child.drain(0.4)
        child.wait(b"engine-block-start\r\r\n\r\r\n")
        assert len(set(re.findall("[◴◷◶◵]", child.buf[after:].decode()))) > 1
        after = child.send_wait(b"/verbose 2" + key, b"verbosity: 2 (previews)", start=after, timeout=0.25)
        after = child.send_wait(b"/verbose 7" + key, b"/verbose expects one integer from 0 through 6",
                           start=after, timeout=0.25)
        after = child.send_wait(b"/verbose " + key, b"verbosity: 2 (previews)", start=after, timeout=0.25)
        assert b"engine-block-end" not in child.buf
        fcntl.ioctl(child.fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", 24, 48, 0, 0))
        child.send(b"responsive-draft")
        try:
            deadline = time.monotonic() + 0.25
            while b"responsive-draft" not in re.sub(
                    rb"\x1b\[[0-?]*[ -/]*[@-~]| \x08|\r", b"", child.buf[after:]):
                remaining = deadline - time.monotonic()
                assert remaining > 0, (
                    f"engine stall stopped local editing: {bytes(child.buf[after:])!r}"
                )
                child.read_once(remaining)
        except AssertionError as exc:
            failure = exc
        end = child.send_wait(b"\x03", b"engine-block-end", start=after)
        child.exit_cleanly(end)
    finally:
        child.kill()
        if failure:
            raise failure


def test_stalled_output_consumes_input():
    for level in range(7):
        child = Child(["-v"] * level + ["--no-markdown"])
        slave = None
        try:
            child.wait_idle_prompt()
            inherited = [Path(f"/proc/{child.pid}/fdinfo/{fd}").read_text()
                         for fd in (0, 1, 2)]
            assert all(not (int(re.search(r"flags:\s+(\d+)", info)[1], 8) &
                            os.O_NONBLOCK) for info in inherited)
            slave = os.open(os.readlink(f"/proc/{child.pid}/fd/0"),
                            os.O_RDONLY | os.O_NONBLOCK | os.O_NOCTTY)
            after = child.send_wait(b"render_flood\r", b"row-0000")
            # Stop draining until the terminal fills. Input must still be consumed.
            time.sleep(0.15)
            child.send(b"/co\t\t\x15/verbose 0\rpinx\x7fg")
            deadline = time.monotonic() + 0.25
            while True:
                pending = struct.unpack("i", fcntl.ioctl(slave, termios.FIONREAD,
                                                         struct.pack("i", 0)))[0]
                if not pending:
                    break
                assert time.monotonic() < deadline, "stalled output stopped input consumption"
                time.sleep(0.01)
            child.wait(b"verbosity: 0 (conversation)", start=after)
            end = child.wait(b"flood-end", start=after)
            child.wait_idle_prompt(start=end)
            child.wait(b"/compact", start=after)
            child.wait(b"/config", start=after)
            end = child.send_wait(b"\r", b"pong", start=end)
            child.exit_cleanly(end)
        finally:
            if slave is not None:
                os.close(slave)
            child.kill()
        log = events(child.session_id())
        inputs = [event["data"]["text"] for event in log if event["type"] == "turn_started"]
        assert inputs == ["render_flood", "ping"], inputs


if __name__ == "__main__":
    test_empty_session_lifecycle()
    test_empty_network_session()
    test_resize_and_suspend_preserve_draft()
    test_compaction_ignores_legacy_samples()
    test_ctrl_d_exit()
    test_goal_orderly_quit_resume()
    test_stalled_output_consumes_input()
    test_editor_during_render_flood()
    test_editor_during_blocked_engine()
    test_editor_during_blocked_engine(b"\t")
    test_active_verbosity()
    test_five_ctrl_c_exit()
    test_ctrl_c_sequence_reset()
    test_blank_enter_during_engine_stall()
    test_full_input_queue_keeps_exit_live()
    test_history_lock_keeps_editing_live()
    test_incremental_prompt_edit_and_utf8_cursor_column()
    test_incremental_active_prompt_keeps_status_stable()
    test_static_zero_width_spinner_has_no_refresh()
    test_prompt_clock_lifetime()
    test_initial_unrenderable_prompt_is_rejected_atomically()
    test_incremental_multiline_delete_clears_old_tail()
    test_incremental_wrapped_long_prompt_multiline_indent()
    test_steering()
    test_repeated_steering_rearms_composer()
    test_public_index_gap()
    test_public_index_diagnostic()
    test_split_utf8_steering()
    test_typing_pause_and_transient_composer()
    test_armed_fifo()
    test_queue_prompt_counts()
    test_read_only_queries()
    test_read_only_multiline_compaction_and_chat()
    test_queue_edit_resume_at_acknowledgement()
    test_read_only_queue_replay_and_edit()
    test_managed_command_steering_and_tab_queue()
    test_steering_during_pre_response_compaction()
    test_steering_during_capacity_recovery_compaction()
    test_agents_md_config()
    test_active_ctrl_c_clears_draft()
    test_ctrl_c_cancels_partial_editor_states()
    test_interrupt()
    test_prompt_history_and_reverse_search()
    test_history_local_first_archive()
    test_history_repeated_resume_exit()
    test_history_large_archive()
    test_history_sparse_archive_cancellation()
    test_session_prompt_history_isolation()
    test_session_prompt_history_exit_and_crash()
    test_multiline_and_paste()
    test_network_input_recovery_boundaries()
    test_deferred_controls_in_admission_order()
    test_archive_control_completion_recovery()
    test_exit_preserves_pending_submission()
    test_input_survives_preparation_failure()
    test_resume_keeps_original_instruction_paths()
    test_durable_queue_scheduling()
    test_resume_preserves_armed_fifo()
    test_goal_quoted_reserved_wording()
    test_goal_automatic_continuation()
    test_model_created_goal_continuation()
    test_goal_configured_wording_limit()
    test_goal_model_rewrite_and_lock()
    test_goal_interrupt_prompt_state()
    test_goal_pause_resume_and_queue_priority()
    test_goal_clear_controls_continuation()
    test_goal_control_whitespace()
    test_goal_clear_without_goal_and_reserved_wording()
    test_goal_user_terminal_commands_and_unlock()
    test_goal_refusal_failure_block_and_restart_state()
    test_saved_goal_restored_without_lookup()
    test_resume_preserves_inactive_and_queued_goal_states()
    test_queue_mutation_commands()
    test_unfinished_public_resume()
    test_retry_budget_survives_response_boundary()
    test_explicit_cancel_is_not_resumed()
    test_recovery_at_durable_tool_boundaries()
    test_idle_compaction_crash_recovery()
    test_active_next_turn_settings()
    test_preferences_and_verbosity()
    test_runtime_verbosity_resume()
    test_help_plain_terminal()
    test_command_name_completion()
    test_uncached_typed_model_selection()
    test_provider_login_and_first_run()
    test_compaction_policy_selection()
    test_provider_local_models()
    test_provider_local_models(False)
    test_model_cache_and_selection()
    test_model_configuration_save()
    test_config_editor_reload()
    test_known_context_meter()
    test_model_selection_stays()
    test_config_and_cli_model_passthrough()
    test_exit_resume_matrix()
    test_runtime_network_commands()
    test_network_resume_roles()
    test_network_collision_prompts()
    test_network_live_nick_prompt()
    test_prompt_identity_is_terminal_safe()
    test_model_message_corrections_are_private_and_specific()
    test_network_view_routing_and_atomic_catchup()
    test_chat_mention_completion_and_steering()
    test_network_chat_and_managed_mention()
    print("pty_active: ok")
