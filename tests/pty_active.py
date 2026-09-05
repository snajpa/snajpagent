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
PROMPT = "› ".encode()
DEFAULT_MODEL = "gpt-5.5-2026-04-23"
DEFAULT_IDLE_PROMPT = f"default/{DEFAULT_MODEL}/medium   0%   › ".encode()
DEFAULT_ACCOUNTED_IDLE_PROMPT = f"default/{DEFAULT_MODEL}/medium   ?%   › ".encode()
DEFAULT_ACTIVE_PROMPT = f"default/{DEFAULT_MODEL}/medium   ?% ◴ » ".encode()
DEFAULT_TOOL_PROMPT = f"default/{DEFAULT_MODEL}/medium   ?%  ⠋» ".encode()
QUEUE_EDIT_ACTIVE_PROMPT = "edit 1   ?% ◴ › ".encode()
QUEUE_EDIT_IDLE_PROMPT = "edit 1   ?%   › ".encode()
GOAL_SET = "• Goal set".encode()
GOAL_CLEARED = "• Goal cleared".encode()
COMPACTED = "• Compacted".encode()
RESUME_HEADER = \
    "• You can resume this session with the following command:".encode()


def chat_prompt(operator):
    return f"{operator}@{socket.gethostname()}   : ".encode()


def chat_active_prompt(operator):
    return f"{operator}@{socket.gethostname()} ◴ : ".encode()


class Child:
    def __init__(self, args):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.chdir(WORKSPACE)
            os.execv(BINARY, [BINARY, "--dotdir", DOTDIR, *args])
        self.buf = bytearray()

    @classmethod
    def from_command(cls, command):
        child = cls.__new__(cls)
        child.pid, child.fd = pty.fork()
        if child.pid == 0:
            os.chdir(WORKSPACE)
            os.execl("/bin/sh", "sh", "-c", "exec " + command)
        child.buf = bytearray()
        return child

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
        end = time.monotonic() + timeout
        while needle not in self.buf[start:]:
            remaining = end - time.monotonic()
            if remaining <= 0 or not self.read_once(remaining):
                raise AssertionError(
                    f"timeout waiting for {needle!r}; got {bytes(self.buf)!r}"
                )
        return self.buf.find(needle, start) + len(needle)

    def wait_idle_prompt(self, start=0, timeout=8.0):
        pattern = re.compile(rb"(?:^|[\r\n])[^\r\n]*/[^\r\n]* \xe2\x80\xba ")
        end = time.monotonic() + timeout
        while True:
            match = pattern.search(self.buf, start)
            if match is not None:
                return match.end()
            remaining = end - time.monotonic()
            if remaining <= 0 or not self.read_once(remaining):
                raise AssertionError(
                    f"timeout waiting for idle prompt; got {bytes(self.buf)!r}"
                )

    def send(self, data):
        os.write(self.fd, data)

    def drain(self, duration=0.25):
        end = time.monotonic() + duration
        while time.monotonic() < end:
            self.read_once(min(0.05, end - time.monotonic()))

    def exit_cleanly(self, after):
        self.wait_idle_prompt(start=after, timeout=8.0)
        self.exit_now()

    def exit_now(self):
        self.send(b"/exit\r")
        return self.finish()

    def finish(self, expected=0, expect_resume=True):
        _, status = os.waitpid(self.pid, 0)
        self.pid = None
        while self.read_once(0.05):
            pass
        os.close(self.fd)
        code = os.waitstatus_to_exitcode(status)
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


def new_session(before):
    created = session_ids() - before
    if len(created) != 1:
        raise AssertionError(f"expected one new session, got {sorted(created)!r}")
    return created.pop()


def events(session_id):
    path = STATE_ROOT / session_id / "events.jsonl"
    with path.open(encoding="utf-8") as source:
        return [json.loads(line) for line in source]


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
    child.send(b"\x15")
    end = child.wait(b"\x1b[K", start=start)
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
    columns = len(prompt.decode())
    return child.wait(
        f"\x1b[K\r\x1b[{columns}C".encode(),
        start=prompt_end,
        timeout=timeout,
    )


def test_incremental_prompt_edit_and_utf8_cursor_column():
    child = Child([])
    try:
        wait_prompt_painted(child, DEFAULT_IDLE_PROMPT)
        empty_tab_start = len(child.buf)
        child.send(b"\t")
        child.drain()
        assert child.buf[empty_tab_start:] == b"", bytes(
            child.buf[empty_tab_start:]
        )
        start = len(child.buf)
        child.send(b"a")
        child.drain()
        edit = bytes(child.buf[start:])
        assert b"a\x1b[K" in edit, edit
        assert b"\x1b[2K" not in edit, edit
        assert DEFAULT_IDLE_PROMPT not in edit, edit

        start = len(child.buf)
        child.send(b"\x1b[D")
        child.drain()
        movement = bytes(child.buf[start:])
        prompt_column = len(DEFAULT_IDLE_PROMPT.decode())
        assert f"\r\x1b[{prompt_column}C".encode() in movement, movement
        assert f"\r\x1b[{prompt_column + 1}C".encode() not in movement, movement
        assert b"\x1b[2K" not in movement, movement
        assert DEFAULT_IDLE_PROMPT not in movement, movement
    finally:
        child.kill()


def test_incremental_active_prompt_keeps_status_stable():
    child = Child([])
    try:
        child.wait(DEFAULT_IDLE_PROMPT)
        child.send(b"terminal_status\r")
        child.wait(DEFAULT_ACTIVE_PROMPT)
        phase_start = len(child.buf)
        child.wait("◷".encode(), start=phase_start, timeout=1.0)

        start = len(child.buf)
        child.send(b"a")
        end = child.wait(b"a\x1b[K", start=start)
        edit = bytes(child.buf[start:end])
        assert b"\x1b[2K" not in edit, edit
        assert b"working\xe2\x80\xa6" not in edit, edit
        assert DEFAULT_ACTIVE_PROMPT not in edit, edit
    finally:
        child.kill()


def test_static_zero_width_spinner_has_no_refresh():
    config = (Path(os.environ["SNAJPAGENT_TEST_ROOT"]) /
              "config" / "static-spinner.ini")
    config.write_text(
        "[ui]\n"
        'prompt_spinner_goal = "\\0"\n'
        'prompt_spinner_provider = "\\0◆"\n'
        'prompt_spinner_tool = "\\0"\n'
        "prompt_spinner_per_second = 60\n",
        encoding="utf-8",
    )
    idle = f"default/{DEFAULT_MODEL}/medium   0%› ".encode()
    active = f"default/{DEFAULT_MODEL}/medium   ?%◆» ".encode()
    child = Child(["--config", str(config)])
    try:
        child.wait(idle)
        child.send(b"terminal_status\r")
        wait_prompt_painted(child, active)
        settled = len(child.buf)
        child.drain(0.35)
        assert len(child.buf) == settled, bytes(child.buf[settled:])
    finally:
        child.kill()


def test_prompt_clock_lifetime():
    config = (Path(os.environ["SNAJPAGENT_TEST_ROOT"]) /
              "config" / "prompt-clock.ini")
    clock = "@{hour:02}:{minute:02}:{second:02}"
    config.write_text(
        "[ui]\nprompt = {chat:" + clock + ":}"
        "{rollout-idle:" + clock + " {context:4}{provider_spinner}›}"
        "{rollout-active:" + clock + " {context:4}{provider_spinner}»}\n"
        'prompt_spinner_provider = " P"\n', encoding="utf-8")
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
        child.send(b"\x12")
        child.wait(b"reverse-i-search", start=start)
        child.drain(1.1)
        start = len(child.buf)
        child.send(b"\x07")
        child.wait(idle + b"clock-draft", start=start)
        # Cancellation retains the old displayed line and captures a new one.
        start = len(child.buf)
        child.send(b"\x03")
        cancelled = child.wait(b"^C\r\n", start=start)
        child.wait("   0% › ".encode(), start=cancelled)
        replacement = latest_clock()
        assert replacement != original, bytes(child.buf[start:])
        child.drain(1.1)
        start = len(child.buf)
        child.send(b"terminal_status\r")
        active_end = child.wait("   ?%P» ".encode(), start=start)
        active_clock = latest_clock()
        assert active_clock != replacement, bytes(child.buf[start:])
        child.send(b"preserved-draft")
        child.wait(b"status-second-fragment", start=active_end)
        settled = child.wait(b"@" + active_clock + "   ?% › ".encode(),
                             start=active_end)
        child.wait(b"preserved-draft", start=settled)
        assert set(re.findall(pattern, child.buf[active_end:])) == {active_clock}
        child.send(b"\x03")
        child.wait(b"^C\r\n", start=settled)
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

    config.write_text(
        "[ui]\n"
        "prompt = {chat:x}{rollout-idle:" + ("x" * 600) +
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
    child = Child([])
    try:
        child.wait(DEFAULT_IDLE_PROMPT)
        child.send(b"abcdef\nsecond")
        child.wait(b"d\x1b[K")
        child.drain(0.05)
        child.send(b"\x1b[H" + b"\x1b[C" * 6)
        child.drain(0.05)

        start = len(child.buf)
        child.send(b"\x7f\x7f\x7f")
        end = child.wait(b"second\x1b[K", start=start)
        edit = bytes(child.buf[start:end])
        indent = b" " * len(DEFAULT_IDLE_PROMPT.decode())
        assert b"\r\n" + indent + b"second" in edit, edit
        assert b"\x1b[2K" not in edit, edit
        assert DEFAULT_IDLE_PROMPT not in edit, edit
    finally:
        child.kill()


def test_incremental_wrapped_long_prompt_multiline_indent():
    model = "m" * 120
    prompt = f"default/{model}/medium   0%   › ".encode()
    child = Child(["-m", model])
    try:
        child.wait(prompt)
        start = len(child.buf)
        child.send(b"x\n" * 8 + b"z")
        end = child.wait(b"z\x1b[K", start=start)
        edit = bytes(child.buf[start:end])
        assert b"\x1b[2K" not in edit, edit
        assert prompt not in edit, edit
    finally:
        child.kill()


def test_steering():
    before = session_ids()
    child = Child([])
    child.wait(DEFAULT_IDLE_PROMPT)
    child.send(b"slow\r")
    child.wait(DEFAULT_ACTIVE_PROMPT)
    child.wait(b"working slowly")
    child.send(b"change course\r")
    answer_end = child.wait(b"steered: change course")
    child.exit_cleanly(answer_end)

    session_id = new_session(before)
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
    before = session_ids()
    child = Child([])
    child.wait(DEFAULT_IDLE_PROMPT)
    child.send(b"slow_resteer\r")
    child.wait(b"working slowly")

    child.send(b"first steer\r")
    first_ack = child.wait(DEFAULT_ACTIVE_PROMPT + b"first steer")
    child.wait(b"first steer\r\n\r\n" + DEFAULT_ACTIVE_PROMPT,
               start=first_ack - len(b"first steer"))
    child.send(b"second steer\r")
    second_ack = child.wait(DEFAULT_ACTIVE_PROMPT + b"second steer",
                            start=first_ack)
    child.wait(b"second steer\r\n\r\n" + DEFAULT_ACTIVE_PROMPT,
               start=second_ack - len(b"second steer"))
    answer_end = child.wait(b"repeated steering complete")
    child.exit_cleanly(answer_end)

    log = events(new_session(before))
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
    before = session_ids()
    child = Child([])
    child.wait(DEFAULT_IDLE_PROMPT)
    child.send(b"public_index_gap\r")
    commentary_end = child.wait(b"Checking hidden work.")
    answer_end = child.wait(b"Gap-safe final.", start=commentary_end)
    child.exit_cleanly(answer_end)

    log = events(new_session(before))
    completed = one(log, "response_completed")
    assert [item["kind"] for item in completed["data"]["items"]] == [
        "assistant", "opaque", "assistant"
    ]
    assert not [item for item in log if item["type"] == "response_failed"]
    assert not [item for item in log if item["type"] == "turn_failed"]


def test_public_index_diagnostic():
    before = session_ids()
    child = Child([])
    child.wait(DEFAULT_IDLE_PROMPT)
    child.send(b"public_index_decrease\r")
    child.wait(b"index one")
    failure_end = child.wait(b"public output indexes did not increase")
    child.exit_cleanly(failure_end)

    log = events(new_session(before))
    failed = one(log, "response_failed")
    assert failed["data"]["class"] == "protocol"
    assert failed["data"]["message"] == (
        "public output indexes did not increase"
    )
    assert one(log, "turn_failed")["data"]["class"] == "protocol"


def test_split_utf8_steering():
    before = session_ids()
    child = Child([])
    child.wait(PROMPT)
    child.send(b"slow_utf8\r")
    session = new_session(before)
    deadline = time.monotonic() + 4.0
    while not any(e["type"] == "response_started" for e in events(session)):
        assert time.monotonic() < deadline
        child.read_once(0.02)
    child.send(b"change\r")
    answer_end = child.wait(b"steered: change")
    child.exit_cleanly(answer_end)

    interrupted = one(events(new_session(before)), "response_interrupted")
    assert interrupted["data"]["partial_public"] == []


def test_typing_pause_and_stream_snapshots():
    config = Path(os.environ["SNAJPAGENT_TEST_ROOT"]) / "config" / \
        "typing-pause.ini"
    config.write_text("[ui]\ntyping_pause_ms = 300\n", encoding="utf-8")
    before = session_ids()
    child = Child(["--config", str(config)])
    child.wait(DEFAULT_IDLE_PROMPT)
    child.send(b"typing_stream\r")
    first_end = child.wait(b"model-output-one")

    child.send(b"a")
    child.wait(DEFAULT_ACTIVE_PROMPT + b"a", start=first_end)
    time.sleep(0.1)
    edit_start = len(child.buf)
    child.send(b"b")
    child.wait(b"b\x1b[K", start=edit_start)
    second_start = time.monotonic()
    quiet_start = len(child.buf)
    child.drain(0.15)
    assert b"model-output-two" not in child.buf[quiet_start:]
    second_end = child.wait(b"model-output-two", start=quiet_start)
    assert time.monotonic() - second_start >= 0.20

    edit_start = len(child.buf)
    child.send(b"c")
    child.wait(b"c\x1b[K", start=edit_start)
    third_start = time.monotonic()
    quiet_start = len(child.buf)
    child.drain(0.15)
    assert b"model-output-three" not in child.buf[quiet_start:]
    third_end = child.wait(b"model-output-three", start=quiet_start)
    assert time.monotonic() - third_start >= 0.20
    child.wait(PROMPT + b"abc", start=third_end)
    child.drain(0.05)
    clear_draft_incrementally(child)
    child.send(b"/exit\r")
    _, status = os.waitpid(child.pid, 0)
    os.close(child.fd)
    assert os.waitstatus_to_exitcode(status) == 0

    completed = one(events(new_session(before)), "response_completed")
    assert completed["data"]["items"][0]["text"] == (
        "model-output-one model-output-two model-output-three"
    )


def test_armed_fifo():
    before = session_ids()
    child = Child([])
    child.wait(DEFAULT_IDLE_PROMPT)
    child.send(b"slow\r")
    child.wait(b"working slowly")
    child.send(b"ping\t")
    child.wait(b"next " + PROMPT + b"ping")
    child.wait(b"slow complete")
    answer_end = child.wait(b"pong")
    child.exit_cleanly(answer_end)

    log = events(new_session(before))
    queued = one(log, "future_turn_queued")
    turns = [item for item in log if item["type"] == "turn_started"]
    assert len(turns) == 2
    assert turns[0]["data"]["input_kind"] == "direct"
    assert turns[1]["data"]["input_kind"] == "queued"
    assert turns[1]["data"]["queue_id"] == queued["data"]["queue_id"]
    assert turns[1]["data"]["queue_seq"] == queued["seq"]
    assert turns[1]["data"]["text"] == "ping"


def test_read_only_queries():
    Path(WORKSPACE, "ro-input.txt").write_text("native text\nsecond line\n", encoding="utf-8")
    before = session_ids()
    child = Child([])
    child.wait(DEFAULT_IDLE_PROMPT)
    child.send(b"/ro\r")
    end = child.wait(b"usage: /ro QUERY")
    child.wait_idle_prompt(start=end)
    child.send(b"/ro ro_native\r")
    end = child.wait(b"native complete")
    child.wait_idle_prompt(start=end)
    child.send(b"/ro ro_denied\r")
    end = child.wait(b"denied complete")
    child.wait_idle_prompt(start=end)
    child.send(b"ping\r")
    end = child.wait(b"pong")
    child.wait_idle_prompt(start=end)
    child.send(b"//ro ping\r")
    end = child.wait(b"fixture answer", start=end)
    child.exit_cleanly(end)
    log = events(new_session(before))
    turns = [x["data"] for x in log if x["type"] == "turn_started"]
    assert [x["read_only"] for x in turns] == [True, True, False, False]
    assert turns[-1]["text"] == "/ro ping"
    results = [x["data"]["result"] for x in log if x["type"] == "tool_finished"]
    assert len(results) == 11
    assert all(x["status"] == "succeeded" for x in results[:3])
    assert "1:native text\n2:second line\n" in results[1]["model_text"]
    assert "ro-input.txt:1:native text" in results[2]["model_text"]
    assert all(x["status"] == "failed" and "read-only" in x["model_text"]
               for x in results[3:])
    assert not any(x["type"] == "goal_started" for x in log)

    before = session_ids()
    child = Child([])
    child.wait(DEFAULT_IDLE_PROMPT)
    child.send(b"slow\r")
    child.wait(b"working slowly")
    child.send(b"/ro ping\r")
    end = child.wait(b"/ro cannot steer an active turn")
    child.wait(b"/ro ping", start=end)
    child.send(b"\t")
    child.wait(b"next " + PROMPT + b"/ro ping", start=end)
    child.send(b"/queue /ro repeat\r")
    child.wait(b"next " + PROMPT + b"/ro repeat", start=end)
    child.send(b"//ro ping\t")
    end = child.wait(b"slow complete")
    end = child.wait(b"pong", start=end)
    end = child.wait(b"haha", start=end)
    end = child.wait(b"fixture answer", start=end)
    child.exit_cleanly(end)
    log = events(new_session(before))
    turns = [x["data"] for x in log if x["type"] == "turn_started"]
    assert [x["read_only"] for x in turns] == [False, True, True, False]
    assert [x["text"] for x in turns] == ["slow", "ping", "repeat", "/ro ping"]
    assert not any(x["type"] in ("steering_added", "response_interrupted") for x in log)

    # Ordinary steers keep the existing read-only turn read-only.
    before = session_ids()
    child = Child([])
    child.wait(DEFAULT_IDLE_PROMPT)
    child.send(b"/ro slow\r")
    child.wait(b"working slowly")
    child.send(b"replacement\r")
    end = child.wait(b"steered: replacement")
    child.exit_cleanly(end)
    log = events(new_session(before))
    assert one(log, "turn_started")["data"]["read_only"] is True
    one(log, "steering_added")


def test_read_only_multiline_compaction_and_chat():
    root = Path(os.environ["SNAJPAGENT_TEST_ROOT"])
    config = root / "config" / "ro-compaction.ini"
    config.write_text("[provider]\nauto_compact_input_tokens = 1\n", encoding="utf-8")
    before = session_ids()
    child = Child([])
    child.wait(DEFAULT_IDLE_PROMPT)
    child.send(b"ping\r")
    end = child.wait(b"pong")
    child.exit_cleanly(end)
    sid = new_session(before)
    child = Child(["--config", str(config), "--resume", sid])
    child.wait(DEFAULT_ACCOUNTED_IDLE_PROMPT)
    child.send(b"/ro ro_native\r")
    end = child.wait(b"native complete")
    child.exit_cleanly(end)
    log = events(sid)
    assert any(x["type"] == "compaction_completed" for x in log)
    assert [x for x in log if x["type"] == "turn_started"][-1]["data"]["read_only"] is True

    before = session_ids()
    child = Child([])
    child.wait(DEFAULT_IDLE_PROMPT)
    child.send(b"slow\r")
    child.wait(b"working slowly")
    child.send(b"\x1b[200~/ro inspect\nmultiline\x1b[201~\r")
    end = child.wait(b"/ro cannot steer an active turn")
    child.wait(b"multiline", start=end)
    child.send(b"\t")
    end = child.wait(b"next " + PROMPT, start=end)
    child.send(b"\x1b[200~/queue /ro another\nquery\x1b[201~\r")
    child.wait(b"next " + PROMPT, start=end)
    end = child.wait(b"slow complete")
    end = child.wait(b"fixture answer", start=end)
    end = child.wait(b"fixture answer", start=end)
    child.exit_cleanly(end)
    log = events(new_session(before))
    turns = [x["data"] for x in log if x["type"] == "turn_started"]
    assert [x["text"] for x in turns[1:]] == ["inspect\nmultiline", "another\nquery"]
    assert all(x["read_only"] for x in turns[1:])
    assert not any(x["type"] == "steering_added" for x in log)

    before = session_ids()
    child = Child(["--no-color", "-s", f"127.0.0.1:{free_port()}",
                   "-n", "roagent", "-o", "rooperator", "-r", "lab"])
    child.wait(chat_prompt("rooperator"))
    child.send(b"/ro ro_native\r")
    end = child.wait(b"native complete")
    child.exit_cleanly(end)
    log = events(new_session(before))
    turn = one(log, "turn_started")["data"]
    assert turn["read_only"] and turn["text"] == "ro_native"
    assert not any(x["type"] in ("irc_reply_reminder", "turn_failed") for x in log)
    assert all(x["data"]["result"]["status"] == "succeeded"
               for x in log if x["type"] == "tool_finished")


def test_read_only_queue_replay_and_edit():
    before = session_ids()
    child = Child([])
    child.wait(DEFAULT_IDLE_PROMPT)
    child.send(b"queue_slow\r")
    child.wait(b"working slowly")
    child.send(b"/ro ping\t")
    child.wait(b"next " + PROMPT + b"/ro ping")
    end = len(child.buf)
    child.send(b"/queue 1 edit\r")
    end = child.wait(b"/ro ping", start=end)
    child.send(b"\x15/ro repeat\r")
    child.wait(b"/ro repeat", start=end)
    child.send(b"\x03")
    end = child.wait(b"turn interrupted")
    child.exit_cleanly(end)
    sid = new_session(before)
    log = events(sid)
    assert one(log, "future_turn_edited")["data"]["read_only"] is True
    child = Child(["--resume", sid])
    end = child.wait(b"queued future turns are paused")
    child.wait_idle_prompt(start=end)
    child.send(b"/goal slow goal\r")
    end = child.wait(GOAL_SET)
    # Existing explicit goal start arms retained FIFO work before the goal.
    end = child.wait(b"haha", start=end)
    end = child.wait(b"goal done", start=end, timeout=10.0)
    child.exit_cleanly(end)
    log = events(sid)
    turns = [x["data"] for x in log if x["type"] == "turn_started"]
    assert turns[1]["input_kind"] == "queued" and turns[1]["read_only"] is True
    assert turns[1]["text"] == "repeat"

    before = session_ids()
    child = Child([])
    child.wait(DEFAULT_IDLE_PROMPT)
    child.send(b"/goal slow goal\r")
    child.wait(b"working on goal")
    child.send(b"/ro ping\t")
    end = child.wait(b"next " + PROMPT + b"/ro ping")
    child.send(b"/queue 1 edit\r")
    child.wait(b"/ro ping", start=end)
    end = child.wait(b"goal checkpoint")
    child.drain(0.1)
    assert b"goal done" not in child.buf[end:] and b"pong" not in child.buf[end:]
    child.send(b"\x15/ro repeat\r")
    end = child.wait(b"/ro repeat", start=end)
    child.send(b"/next\r")
    end = child.wait(b"haha", start=end)
    end = child.wait(b"goal done", start=end)
    child.exit_cleanly(end)
    log = events(new_session(before))
    assert [x["data"]["input_kind"] for x in log if x["type"] == "turn_started"] == [
        "goal", "queued", "goal"
    ]


def test_managed_command_steering_and_tab_queue():
    before = session_ids()
    child = Child(["-v"])
    child.wait(DEFAULT_IDLE_PROMPT)
    child.send(b"managed_command_steer\r")
    child.wait(b"fixture managed steering wait")
    child.wait(DEFAULT_TOOL_PROMPT)
    child.send(b"terminate it\r")
    steering_ack = child.wait("» terminate it\r\n".encode())
    child.wait(DEFAULT_TOOL_PROMPT, start=steering_ack)
    answer_end = child.wait(b"managed command steering complete")
    child.exit_cleanly(answer_end)

    log = events(new_session(before))
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

    before = session_ids()
    child = Child(["-v"])
    child.wait(DEFAULT_IDLE_PROMPT)
    child.send(b"managed_command_queue\r")
    child.wait(b"fixture managed queue wait")
    child.send(b"ping\t")
    child.wait(b"next " + PROMPT + b"ping")
    command_end = child.wait(b"managed command queue complete")
    answer_end = child.wait(b"pong", start=command_end)
    child.exit_cleanly(answer_end)

    log = events(new_session(before))
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
    root = Path(os.environ["SNAJPAGENT_TEST_ROOT"])
    config = root / "config" / "steering-compaction.ini"
    config.write_text(
        "[provider]\nauto_compact_input_tokens = 1\n",
        encoding="utf-8",
    )
    before = session_ids()
    child = Child([])
    child.wait(DEFAULT_IDLE_PROMPT)
    child.send(b"ping\r")
    answer_end = child.wait(b"pong")
    child.exit_cleanly(answer_end)
    session_id = new_session(before)

    child = Child(["--config", str(config), "--resume", session_id])
    child.wait(DEFAULT_ACCOUNTED_IDLE_PROMPT)
    child.send(b"compaction_steer\r")
    child.wait(DEFAULT_ACTIVE_PROMPT)
    child.send(b"change plan\r")
    steer_end = child.wait(DEFAULT_ACTIVE_PROMPT + b"change plan")
    child.wait(DEFAULT_ACTIVE_PROMPT, start=steer_end)
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
    starts = [item for item in log
              if item["type"] == "response_started" and
              item["data"]["turn_id"] == turn_id]
    assert len(starts) == 1
    assert starts[0]["data"]["steering_ids"] == [
        steering["data"]["steering_id"]
    ]

    resumed = Child(["--config", str(config), "--resume", session_id])
    resumed.wait(PROMPT)
    resumed.exit_now()


def test_steering_during_capacity_recovery_compaction():
    before = session_ids()
    child = Child([])
    child.wait(DEFAULT_IDLE_PROMPT)
    child.send(b"ping\r")
    answer_end = child.wait(b"pong")
    child.exit_cleanly(answer_end)
    session_id = new_session(before)

    child = Child(["--resume", session_id])
    child.wait(DEFAULT_ACCOUNTED_IDLE_PROMPT)
    child.send(b"capacity_recovery_steer\r")
    deadline = time.monotonic() + 4.0
    while not any(e["type"] == "compaction_started" and
                  e["data"]["reason"] == "provider_rejection"
                  for e in events(session_id)):
        assert time.monotonic() < deadline
        child.read_once(0.02)
    child.send(b"change recovery plan\r")
    steer_end = child.wait(b"\xc2\xbb change recovery plan")
    answer_end = child.wait(b"fixture answer", start=steer_end)
    child.exit_cleanly(answer_end)

    log = events(session_id)
    turn = [item for item in log if item["type"] == "turn_started"][-1]
    turn_id = turn["data"]["turn_id"]
    rejected = [item for item in log
                if item["type"] == "response_capacity_rejected" and
                item["data"]["turn_id"] == turn_id]
    interrupted = [item for item in log
                   if item["type"] == "compaction_interrupted"]
    compactions = [item for item in log
                   if item["type"] == "compaction_started" and
                   item["data"]["reason"] == "provider_rejection"]
    completed = [item for item in log if item["type"] == "compaction_completed"]
    steering = [item for item in log
                if item["type"] == "steering_added" and
                item["data"]["turn_id"] == turn_id]
    starts = [item for item in log
              if item["type"] == "response_started" and
              item["data"]["turn_id"] == turn_id]
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
    resumed.send(b"/status\r")
    status_end = resumed.wait(b"context: source=observed", start=prompt_end)
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
        "[provider]\nbase_url = https://different.example.test\n",
        encoding="utf-8",
    )
    mismatched = Child([
        "--config", str(mismatch_config), "--resume", session_id
    ])
    prompt_end = mismatched.wait(b"   0%   \xe2\x80\xba ")
    mismatched.send(b"/status\r")
    status_end = mismatched.wait(b"context: source=unknown", start=prompt_end)
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

    enabled_config = root / "config" / "agents-enabled.ini"
    enabled_config.write_text(
        "[agent]\nread_agents_md = true\n",
        encoding="utf-8",
    )
    before = session_ids()
    child = Child(["--config", str(enabled_config), "-C", str(workspace)])
    child.wait(PROMPT)
    child.send(b"ping\r")
    answer_end = child.wait(b"pong")
    child.exit_cleanly(answer_end)
    turn = one(events(new_session(before)), "turn_started")
    instructions = turn["data"]["instructions"]
    assert instructions
    assert instructions[-1] == {
        "path": str(agents),
        "bytes": len(contents.encode()),
        "sha256": hashlib.sha256(contents.encode()).hexdigest(),
    }

    disabled_config = root / "config" / "agents-disabled.ini"
    disabled_config.write_text(
        "[agent]\nread_agents_md = false\n",
        encoding="utf-8",
    )
    before = session_ids()
    child = Child(["--config", str(disabled_config), "-C", str(workspace)])
    child.wait(PROMPT)
    child.send(b"ping\r")
    answer_end = child.wait(b"pong")
    child.exit_cleanly(answer_end)
    turn = one(events(new_session(before)), "turn_started")
    assert turn["data"]["instructions"] == []


def test_interrupt():
    before = session_ids()
    child = Child([])
    child.wait(PROMPT)
    child.send(b"slow\r")
    child.wait(b"working slowly")
    child.send(b"\x03")
    interrupted_end = child.wait(b"turn interrupted")
    child.drain(0.1)
    assert os.waitpid(child.pid, os.WNOHANG) == (0, 0), bytes(child.buf)
    idle_cancel = len(child.buf)
    child.send(b"x\x03")
    child.wait(b"^C\r\n", start=idle_cancel)
    child.drain(0.2)
    assert os.waitpid(child.pid, os.WNOHANG) == (0, 0), bytes(child.buf)
    assert bytes(child.buf[idle_cancel:]).count(b"^C\r\n") == 1
    child.exit_now()

    log = events(new_session(before))
    response = one(log, "response_interrupted")
    turn = one(log, "turn_interrupted")
    assert response["data"]["origin"] == "user"
    assert response["data"]["reason"] == "cancelled"
    assert turn["data"]["origin"] == "user"
    assert turn["data"]["reason"] == "cancelled"


def test_active_ctrl_c_clears_draft():
    before = session_ids()
    child = Child([])
    child.wait(PROMPT)
    child.send(b"queue_slow\r")
    child.wait(b"working slowly")

    edit_start = len(child.buf)
    child.send(b"discard this")
    child.wait(b"t\x1b[Kh\x1b[Ki\x1b[Ks\x1b[K", start=edit_start)
    clear_start = len(child.buf)
    child.send(b"\x03")
    clear_end = child.wait(b"^C\r\n", start=clear_start)
    cleared = bytes(child.buf[clear_start:clear_end])
    assert b"interrupting" not in cleared
    assert b"\x1b[2K" not in cleared

    child.send(b"replacement\r")
    answer_end = child.wait(b"steered: replacement", start=clear_end)
    child.exit_cleanly(answer_end)

    log = events(new_session(before))
    steering = one(log, "steering_added")
    response = one(log, "response_interrupted")
    assert steering["data"]["text"] == "replacement"
    assert response["data"]["origin"] == "steering"
    assert not [item for item in log if item["type"] == "turn_interrupted"]


def test_ctrl_c_cancels_partial_editor_states():
    child = Child([])
    try:
        child.wait(DEFAULT_IDLE_PROMPT)
        escape_start = len(child.buf)
        child.send(b"escape-draft\x1b")
        child.send(b"\x03")
        escape_end = child.wait(b"^C\r\n", start=escape_start)
        assert_bytes_in_order(bytes(child.buf[escape_start:escape_end]),
                              b"escape-draft")
        child.wait(DEFAULT_IDLE_PROMPT, start=escape_end)

        paste_start = len(child.buf)
        child.send(b"\x1b[200~paste-draft")
        child.send(b"\x03")
        paste_end = child.wait(b"^C\r\n", start=paste_start)
        assert_bytes_in_order(bytes(child.buf[paste_start:paste_end]),
                              b"paste-draft")
        child.wait(DEFAULT_IDLE_PROMPT, start=paste_end)
        child.send(b"clean-after-cancel\r")
        child.wait(b"fixture answer", start=paste_end)
    finally:
        child.kill()


def test_prompt_history_and_reverse_search():
    history = Path(DOTDIR) / "prompt_history"
    second = Child([])
    second.wait(DEFAULT_IDLE_PROMPT)
    first = Child([])
    first.wait(DEFAULT_IDLE_PROMPT)
    for entry in (
        b"history-repeat-old",
        b"history-repeat-new",
        "history-café-unique".encode(),
    ):
        start = len(first.buf)
        first.send(entry + b"\r")
        answer = first.wait(b"fixture answer", start=start)
        first.wait_idle_prompt(start=answer)
    first.exit_cleanly(answer)

    second.send(b"draft-restore")
    second.send(b"\x12")
    second.wait(b"(failed reverse-i-search)`draft-restore': ")
    second.send(b"\x07")
    cancel = len(second.buf)
    second.send(b"\x03")
    cancel_end = second.wait(b"^C\r\n", start=cancel)
    cancelled = bytes(second.buf[cancel:cancel_end])
    assert b"draft-restore" in cancelled

    search = len(second.buf)
    second.send(b"\x12history-repeat")
    second.wait(b"(reverse-i-search)`history-repeat': history-repeat-new",
                start=search)
    older = len(second.buf)
    second.send(b"\x12")
    second.wait(b"(reverse-i-search)`history-repeat': history-repeat-old",
                start=older)
    second.send(b"\r")
    answer = second.wait(b"fixture answer", start=search)
    second.wait_idle_prompt(start=answer)

    search = len(second.buf)
    second.send(b"\x12" + "history-café-uniqueX".encode())
    second.wait("(failed reverse-i-search)`history-café-uniqueX': ".encode(),
                start=search)
    second.send(b"\x7f")
    second.wait(
        "(reverse-i-search)`history-café-unique': history-café-unique".encode(),
        start=search,
    )
    accepted = len(second.buf)
    second.send(b"\x1b")
    second.wait(DEFAULT_ACCOUNTED_IDLE_PROMPT + "history-café-unique".encode(),
                start=accepted)
    second.send(b"\x03")
    second.wait(b"^C\r\n", start=accepted)

    second.send(b"/history-invalid-command\r")
    invalid_end = second.wait(b"unknown slash command")
    second.wait_idle_prompt(start=invalid_end)
    second.send(b"\r")
    second.send(b"history-cancelled-draft\x03")
    second.wait(b"^C\r\n", start=invalid_end)
    second.send(b"/delete\r")
    confirm = second.wait(b"delete is irreversible")
    second.wait(PROMPT, start=confirm)
    second.send(b"history-confirmation-excluded\r")
    mismatch = second.wait(b"delete confirmation did not match", start=confirm)
    second.wait_idle_prompt(start=mismatch)

    second.send(b"/delete\r")
    confirm = second.wait(b"delete is irreversible", start=mismatch)
    second.wait(PROMPT, start=confirm)
    cancel = len(second.buf)
    second.send(b"confirmation-cancelled-draft\x03")
    cancel_end = second.wait(b"^C\r\n", start=cancel)
    prompt_end = second.wait(DEFAULT_ACCOUNTED_IDLE_PROMPT, start=cancel_end)
    cancelled = bytes(second.buf[cancel:prompt_end])
    assert_bytes_in_order(cancelled, b"confirmation-cancelled-draft")
    assert b"delete cancelled" not in cancelled
    assert cancelled.count(DEFAULT_ACCOUNTED_IDLE_PROMPT) == 1

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
    assert "draft-restore" not in records
    assert "history-cancelled-draft" not in records
    assert "history-confirmation-excluded" not in records
    assert "confirmation-cancelled-draft" not in records
    assert "history-noninteractive-excluded" not in records


def test_multiline_and_paste():
    before = session_ids()
    child = Child([])
    child.wait(PROMPT)
    child.send(b"line one\nline two\r")
    first_end = child.wait(b"fixture answer")
    child.wait(DEFAULT_ACCOUNTED_IDLE_PROMPT, start=first_end)
    child.send(b"\x1b[200~ping\x1b[201~\r")
    answer_end = child.wait(b"pong")
    child.exit_cleanly(answer_end)

    turns = [item for item in events(new_session(before))
             if item["type"] == "turn_started"]
    assert [item["data"]["text"] for item in turns] == ["line one\nline two", "ping"]


def test_resume_pauses_fifo():
    before = session_ids()
    child = Child([])
    child.wait(PROMPT)
    child.send(b"slow\r")
    child.wait(b"working slowly")
    child.send(b"/queue ping\r")
    child.wait(b"next " + PROMPT + b"ping")
    session_id = new_session(before)
    child.kill()

    resumed = Child(["--resume", session_id])
    resumed.wait(b"1 queued paused")
    resumed.wait(b"queued future turns are paused; use /next")
    resumed.wait(PROMPT)
    resumed.drain(0.3)
    assert b"pong" not in resumed.buf
    resumed.send(b"/next\r")
    answer_end = resumed.wait(b"pong")
    resumed.exit_cleanly(answer_end)

    log = events(session_id)
    turns = [item for item in log if item["type"] == "turn_started"]
    assert len(turns) == 2
    assert turns[1]["data"]["input_kind"] == "queued"
    assert one(log, "response_interrupted")["data"]["origin"] == "recovery"
    assert one(log, "turn_interrupted")["data"]["origin"] == "recovery"


def test_goal_quoted_reserved_wording():
    before = session_ids()
    child = Child([])
    child.wait(PROMPT)
    child.send(b"/goal pause after release\r")
    error_end = child.wait(b"reserved /goal command has extra text")
    child.wait(PROMPT, start=error_end)
    child.send(b'/goal "pause after release"\r')
    answer_end = child.wait(b"goal done")
    child.send(b"/goal\r")
    status_end = child.wait(b": completed", start=answer_end)
    wording_end = child.wait(b"pause after release", start=status_end)
    child.wait(PROMPT, start=wording_end)
    child.exit_now()

    log = events(new_session(before))
    started = one(log, "goal_started")
    completed = one(log, "goal_completed")
    turns = [item for item in log if item["type"] == "turn_started"]
    assert started["data"]["prompt"] == "pause after release"
    assert completed["data"]["actor"] == "model"
    assert [item["data"]["input_kind"] for item in turns] == ["goal"]


def test_goal_automatic_continuation():
    before = session_ids()
    child = Child([])
    child.wait(PROMPT)
    child.send(b"/goal automatic goal\r")
    checkpoint_end = child.wait(b"goal checkpoint")
    answer_end = child.wait(b"goal done", start=checkpoint_end)
    child.exit_cleanly(answer_end)

    log = events(new_session(before))
    turns = [item for item in log if item["type"] == "turn_started"]
    assert [item["data"]["input_kind"] for item in turns] == ["goal", "goal"]
    assert all(item["data"]["text"] ==
               "Continue the active goal from its durable state."
               for item in turns)
    assert one(log, "goal_completed")["data"]["actor"] == "model"


def test_model_created_goal_continuation():
    before = session_ids()
    child = Child([])
    child.wait(PROMPT)
    child.send(b"please create a persistent goal\r")
    started_end = child.wait(GOAL_SET)
    checkpoint_end = child.wait(b"model-created checkpoint", start=started_end)
    cleared_end = child.wait(GOAL_CLEARED, start=checkpoint_end)
    answer_end = child.wait(b"goal done", start=cleared_end)
    child.exit_cleanly(answer_end)

    log = events(new_session(before))
    started = one(log, "goal_started")
    turns = [item for item in log if item["type"] == "turn_started"]
    assert started["data"]["prompt"] == "model-created goal"
    assert [item["data"]["input_kind"] for item in turns] == ["direct", "goal"]
    assert one(log, "goal_completed")["data"]["actor"] == "model"


def test_goal_configured_wording_limit():
    config = Path(os.environ["SNAJPAGENT_TEST_ROOT"]) / "config" / \
        "goal-limit.ini"
    config.write_text(
        "[agent]\nmax_goal_prompt_bytes = 4\n",
        encoding="utf-8",
    )
    before = session_ids()
    child = Child(["--config", str(config)])
    child.wait(PROMPT)
    child.send(b"/goal abcde\r")
    error_end = child.wait(b"goal wording must contain 1..4 UTF-8 bytes")
    child.wait(PROMPT, start=error_end)
    child.send(b"/goal tiny\r")
    answer_end = child.wait(b"goal done")
    child.exit_cleanly(answer_end)

    log = events(new_session(before))
    assert one(log, "goal_started")["data"]["prompt"] == "tiny"
    assert len([item for item in log if item["type"] == "goal_reworded"]) == 0
    assert one(log, "goal_completed")["data"]["actor"] == "model"


def test_goal_model_rewrite_and_lock():
    before = session_ids()
    child = Child([])
    child.wait(PROMPT)
    child.send(b"/goal rewrite goal\r")
    set_end = child.wait(GOAL_SET)
    rewritten_end = child.wait(GOAL_SET, start=set_end)
    cleared_end = child.wait(GOAL_CLEARED, start=rewritten_end)
    answer_end = child.wait(b"goal done", start=cleared_end)
    child.exit_cleanly(answer_end)
    log = events(new_session(before))
    reworded = one(log, "goal_reworded")
    assert reworded["data"] == {
        "actor": "model",
        "goal_id": one(log, "goal_started")["data"]["goal_id"],
        "prompt": "rewritten goal",
    }

    before = session_ids()
    child = Child([])
    child.wait(PROMPT)
    child.send(b"/goal locked goal\r")
    child.wait(b"preparing goal rewrite")
    child.send(b"/goal lock\r")
    lock_end = child.wait(b"goal wording locked against model changes")
    answer_end = child.wait(b"goal done", start=lock_end)
    child.exit_cleanly(answer_end)
    log = events(new_session(before))
    assert len([item for item in log if item["type"] == "goal_reworded"]) == 0
    assert one(log, "goal_lock_changed")["data"]["locked"] is True
    assert one(log, "goal_completed")["data"]["actor"] == "model"


def test_goal_pause_resume_and_queue_priority():
    before = session_ids()
    child = Child([])
    child.wait(PROMPT)
    child.send(b"/goal slow goal\r")
    child.wait(b"working on goal")
    child.send(b"/goal pause\r")
    pause_end = child.wait(b"goal paused at the current turn boundary")
    checkpoint_end = child.wait(b"goal checkpoint", start=pause_end)
    child.wait(PROMPT, start=checkpoint_end)
    child.drain(0.2)
    assert b"goal done" not in child.buf[checkpoint_end:]
    child.send(b"/goal resume\r")
    answer_end = child.wait(b"goal done", start=checkpoint_end)
    child.exit_cleanly(answer_end)
    log = events(new_session(before))
    assert one(log, "goal_paused")["data"]["reason"] == "user"
    one(log, "goal_resumed")
    turns = [item for item in log if item["type"] == "turn_started"]
    assert [item["data"]["input_kind"] for item in turns] == ["goal", "goal"]

    before = session_ids()
    child = Child([])
    child.wait(PROMPT)
    child.send(b"/goal slow goal\r")
    child.wait(b"working on goal")
    child.send(b"ping\t")
    child.wait(b"next " + PROMPT + b"ping")
    child.send(b"/ro repeat\t")
    child.wait(b"next " + PROMPT + b"/ro repeat")
    child.send(b"ping\t")
    checkpoint_end = child.wait(b"goal checkpoint")
    pong_end = child.wait(b"pong", start=checkpoint_end)
    pong_end = child.wait(b"haha", start=pong_end)
    pong_end = child.wait(b"pong", start=pong_end)
    answer_end = child.wait(b"goal done", start=pong_end)
    child.exit_cleanly(answer_end)
    log = events(new_session(before))
    turns = [item for item in log if item["type"] == "turn_started"]
    assert [item["data"]["input_kind"] for item in turns] == [
        "goal", "queued", "queued", "queued", "goal"
    ]


def test_goal_user_terminal_commands_and_unlock():
    before = session_ids()
    child = Child([])
    child.wait(PROMPT)
    child.send(b"/goal slow goal\r")
    set_end = child.wait(GOAL_SET)
    child.wait(b"working on goal", start=set_end)
    child.send(b"/goal set retitled goal\r")
    reworded_end = child.wait(GOAL_SET, start=set_end)
    child.send(b"/goal lock\r")
    child.wait(b"goal wording locked against model changes",
               start=reworded_end)
    child.send(b"/goal unlock\r")
    child.wait(b"goal wording unlocked for model changes")
    child.send(b"/goal complete\r")
    complete_end = child.wait(GOAL_CLEARED, start=reworded_end)
    checkpoint_end = child.wait(b"goal checkpoint", start=complete_end)
    child.wait(PROMPT, start=checkpoint_end)

    child.send(b"/goal slow goal\r")
    set_end = child.wait(GOAL_SET, start=checkpoint_end)
    child.wait(b"working on goal", start=set_end)
    child.send(b"/goal cancel\r")
    cancel_end = child.wait(GOAL_CLEARED, start=set_end)
    checkpoint_end = child.wait(b"goal checkpoint", start=cancel_end)
    child.exit_cleanly(checkpoint_end)

    log = events(new_session(before))
    completed = one(log, "goal_completed")
    assert completed["data"]["actor"] == "user"
    one(log, "goal_cancelled")
    assert one(log, "goal_reworded")["data"]["prompt"] == "retitled goal"
    locks = [item["data"]["locked"] for item in log
             if item["type"] == "goal_lock_changed"]
    assert locks == [True, False]


def test_goal_refusal_failure_block_and_restart_pause():
    before = session_ids()
    child = Child([])
    child.wait(PROMPT)
    child.send(b"/goal refusing goal\r")
    child.wait(b"I cannot continue this goal")
    paused_end = child.wait(b"goal paused after model refusal")
    child.exit_cleanly(paused_end)
    log = events(new_session(before))
    assert one(log, "goal_paused")["data"]["reason"] == "refusal"

    before = session_ids()
    child = Child([])
    child.wait(PROMPT)
    child.send(b"/goal failing goal\r")
    child.wait(b"fixture goal provider failed")
    paused_end = child.wait(b"goal paused after the turn stopped")
    child.exit_cleanly(paused_end)
    log = events(new_session(before))
    assert one(log, "goal_paused")["data"]["reason"] == "turn_stopped"
    one(log, "turn_failed")

    before = session_ids()
    child = Child([])
    child.wait(PROMPT)
    child.send(b"/goal blocked goal\r")
    child.wait(b"goal blocked by model")
    answer_end = child.wait(b"goal done")
    child.send(b"/goal\r")
    status_end = child.wait(b": blocked", start=answer_end)
    blocker_end = child.wait(b"fixture dependency is unavailable", start=status_end)
    child.wait(PROMPT, start=blocker_end)
    child.exit_now()
    log = events(new_session(before))
    assert one(log, "goal_blocked")["data"]["reason"] == \
        "fixture dependency is unavailable"

    before = session_ids()
    child = Child([])
    child.wait(PROMPT)
    child.send(b"/goal slow goal\r")
    child.wait(b"working on goal")
    session_id = new_session(before)
    child.kill()
    resumed = Child(["--resume", session_id])
    paused_end = resumed.wait(
        b"active goal was paused on resume; use /goal resume to continue"
    )
    resumed.send(b"/goal\r")
    status_end = resumed.wait(b": paused", start=paused_end)
    resumed.wait(PROMPT, start=status_end)
    resumed.exit_now()
    log = events(session_id)
    pauses = [item for item in log if item["type"] == "goal_paused"]
    assert pauses[-1]["data"]["reason"] == "session_resumed"


def test_queue_mutation_commands():
    before = session_ids()
    child = Child([])
    child.wait(PROMPT)
    child.send(b"queue_slow\r")
    child.wait(b"working slowly")

    for text in (b"first", b"second", b"third"):
        child.send(text + b"\t")
        child.wait(b"next " + PROMPT + text)

    start = len(child.buf)
    child.send(b"/q\r")
    listed = child.wait(b"3 ", start=start)
    child.wait(b"first", start=start)
    child.wait(b"second", start=start)
    child.wait(b"third", start=start)
    child.wait(DEFAULT_ACTIVE_PROMPT, start=listed)

    child.send(b"/q p\r")
    child.wait(b"1 future turn cancelled")
    child.send(b"/q 1d\r")
    child.wait(b"1 future turn cancelled")

    child.send(b"/q 1e\r")
    child.wait(QUEUE_EDIT_ACTIVE_PROMPT + b"second")
    cancel_start = len(child.buf)
    child.send(b"\x03")
    child.wait(b"^C\r\n", start=cancel_start)
    child.wait(DEFAULT_ACTIVE_PROMPT, start=cancel_start)
    child.send(b"/q 1e\r")
    child.wait(QUEUE_EDIT_ACTIVE_PROMPT + b"second", start=cancel_start)
    child.send(b" active\r")
    child.wait(QUEUE_EDIT_ACTIVE_PROMPT + b"second active")

    child.send(b"fourth\t")
    child.wait(b"next " + PROMPT + b"fourth")
    child.send(b"\x03")
    interrupted_end = child.wait(b"turn interrupted")
    child.wait(DEFAULT_ACCOUNTED_IDLE_PROMPT, start=interrupted_end)

    child.send(b"/queue 1 edit\r")
    child.wait(QUEUE_EDIT_IDLE_PROMPT + b"second active")
    child.send(b" idle\r")
    child.wait(QUEUE_EDIT_IDLE_PROMPT + b"second active idle")

    child.send(b"/exit\r")
    _, status = os.waitpid(child.pid, 0)
    os.close(child.fd)
    assert os.waitstatus_to_exitcode(status) == 0

    session_id = new_session(before)
    resumed = Child(["--resume", session_id])
    resumed.wait(b"2 queued paused")
    resumed.wait(PROMPT)
    start = len(resumed.buf)
    resumed.send(b"/q\r")
    resumed.wait(b"second active idle", start=start)
    resumed.wait(b"fourth", start=start)
    resumed.send(b"/queue clear\r")
    cleared_end = resumed.wait(b"2 future turns cancelled", start=start)
    resumed.send(b"/q\r")
    empty_end = resumed.wait(b"future-turn queue is empty", start=cleared_end)
    resumed.wait(PROMPT, start=empty_end)
    resumed.send(b"/exit\r")
    _, status = os.waitpid(resumed.pid, 0)
    os.close(resumed.fd)
    assert os.waitstatus_to_exitcode(status) == 0

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
    before = session_ids()
    child = Child([])
    child.wait(PROMPT)

    child.send(b"/effort quantum\r")
    end = child.wait(b"effort for next turn: quantum")
    child.wait(PROMPT, start=end)

    child.send(b"/verbose 4\r")
    end = child.wait(b"verbosity: 4")
    child.wait(PROMPT, start=end)

    child.send(b"ping\r")
    child.wait(b"event \xe2\x80\xba")
    answer_end = child.wait(b"pong")
    terminal_end = child.wait(b"turn_completed synced", start=answer_end)
    child.exit_cleanly(terminal_end)

    log = events(new_session(before))
    effort = one(log, "effort_changed")
    turn = one(log, "turn_started")
    assert effort["data"] == {
        "old_effort": "default", "new_effort": "quantum"
    }
    assert turn["data"]["config"]["effort"] == "quantum"


def test_command_name_completion():
    child = Child([])
    child.wait(PROMPT)

    start = len(child.buf)
    child.send(b"/he\t")
    end = child.wait(PROMPT + b"/help", start=start)
    child.send(b"\r")
    help_end = child.wait(b"/compact", start=end)
    child.wait(b"Empty Tab no-op", start=help_end)
    child.wait(b"Tab complete/indent/queue", start=help_end)
    child.drain()
    assert b"steer" not in child.buf[end:]
    child.wait(PROMPT, start=help_end)

    start = len(child.buf)
    child.send(b"/?\r")
    alias_end = child.wait(b"/help", start=start)
    child.wait(b"/?", start=alias_end)
    alias_end = child.wait(b"/compact", start=alias_end)
    child.wait(b"Tab complete/indent/queue", start=alias_end)
    child.wait(PROMPT, start=alias_end)

    for prefix, command in (
        (b"/sta", b"/status"),
        (b"/hi", b"/history"),
        (b"/mo", b"/model"),
        (b"/ef", b"/effort"),
        (b"/go", b"/goal"),
        (b"/v", b"/verbose"),
        (b"/q", b"/queue"),
        (b"/n", b"/next"),
        (b"/a", b"/archive"),
        (b"/com", b"/compact"),
        (b"/con", b"/config"),
        (b"/d", b"/delete"),
        (b"/ex", b"/exit"),
    ):
        start = len(child.buf)
        child.send(prefix + b"\t")
        end = child.wait(PROMPT + command, start=start)
        clear_draft_incrementally(child)

    start = len(child.buf)
    child.send(b"/mo gpt\x1b[D\x1b[D\x1b[D\x1b[D\t")
    end = child.wait(PROMPT + b"/model gpt", start=start)
    clear_draft_incrementally(child)

    start = len(child.buf)
    child.send(b"/h\ti\t")
    end = child.wait(PROMPT + b"/history", start=start)
    clear_draft_incrementally(child)

    start = len(child.buf)
    child.send(b"x\t")
    end = child.wait(b"x\x1b[K   \x1b[K", start=start)
    edit = bytes(child.buf[start:end])
    assert b"\x1b[2K" not in edit, edit
    assert DEFAULT_IDLE_PROMPT not in edit, edit
    clear_draft_incrementally(child)

    child.send(b"slow\r")
    child.wait(b"working slowly")

    start = len(child.buf)
    child.send(b"/he\t")
    end = child.wait(DEFAULT_ACTIVE_PROMPT + b"/help", start=start)
    child.send(b"\r")
    help_end = child.wait(b"/compact", start=end)
    child.wait(b"Tab complete/indent/queue", start=help_end)
    child.wait(DEFAULT_ACTIVE_PROMPT, start=help_end)

    start = len(child.buf)
    child.send(b"/?\r")
    alias_end = child.wait(b"/help", start=start)
    child.wait(b"/?", start=alias_end)
    alias_end = child.wait(b"/compact", start=alias_end)
    child.wait(b"Tab complete/indent/queue", start=alias_end)
    child.wait(DEFAULT_ACTIVE_PROMPT, start=alias_end)

    start = len(child.buf)
    child.send(b"/sta\t")
    end = child.wait(DEFAULT_ACTIVE_PROMPT + b"/status", start=start)
    child.send(b"\r")
    status_end = child.wait(b"state: active", start=end)
    child.wait(DEFAULT_ACTIVE_PROMPT, start=status_end)
    child.send(b"/config\r")
    config_end = child.wait(
        b"/config is idle-only; interrupt or wait", start=status_end
    )
    answer_end = child.wait(b"slow complete", start=config_end)
    child.exit_cleanly(answer_end)


def cached_timestamp(cache):
    updated = cache["updated_at_ms"] / 1000
    return datetime.fromtimestamp(updated, timezone.utc).strftime(
        "cache updated: %Y-%m-%dT%H:%M:%SZ"
    ).encode()


def test_uncached_typed_model_selection():
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
    previous_codex_home = os.environ.get("CODEX_HOME")
    cache_path.unlink(missing_ok=True)
    default_codex_cache.parent.mkdir(mode=0o700, exist_ok=True)
    custom_codex_home.mkdir(mode=0o700, exist_ok=True)
    default_codex_cache.write_text(borrowed_catalog, encoding="utf-8")
    custom_codex_cache.write_text(borrowed_catalog, encoding="utf-8")
    try:
        # Even an explicitly located Codex cache is not snajpagent state.
        os.environ["CODEX_HOME"] = str(custom_codex_home)
        child = Child([])
        child.wait(PROMPT)
        child.send(b"/model\r")
        end = child.wait(b"model cache is empty; use /model cache while idle")
        child.wait(PROMPT, start=end)
        assert not cache_path.exists()

        # A typed model is trusted without discovery or any cache mutation.
        child.send(b"/model gpt-5.6-luna / high\r")
        end = child.wait(
            b"model for next turn: default / gpt-5.6-luna / high", start=end
        )
        end = child.wait(
            b"snajpagent: model is not known in the model cache; "
            b"it will still be sent unchanged",
            start=end,
        )
        child.wait(PROMPT, start=end)
        assert not cache_path.exists()
        child.exit_now()

        # The conventional ~/.codex cache is ignored as well.
        os.environ.pop("CODEX_HOME", None)
        child = Child([])
        child.wait(PROMPT)
        child.send(b"/model list\r")
        end = child.wait(b"model cache is empty; use /model cache while idle")
        child.wait(PROMPT, start=end)
        assert not cache_path.exists()
        child.exit_now()
    finally:
        if previous_codex_home is None:
            os.environ.pop("CODEX_HOME", None)
        else:
            os.environ["CODEX_HOME"] = previous_codex_home


def test_model_cache_and_selection():
    cache_path = Path(DOTDIR) / "models.json"
    initial_prompt = b"first/uncached-start/low   0%   \xe2\x80\xba "
    cache_path.unlink(missing_ok=True)
    config = Path(os.environ["SNAJPAGENT_TEST_ROOT"]) / "config" / "models.ini"
    config.write_text(
        "[agent]\n"
        "model = uncached-start\n"
        "reasoning_effort = low\n"
        "[provider first]\n"
        "api_key_env = FIRST_API_KEY\n"
        "[provider second]\n"
        "api_key_env = SECOND_API_KEY\n",
        encoding="utf-8",
    )
    before = session_ids()
    child = Child(["--config", str(config)])
    child.wait(PROMPT)

    # Explicit refresh creates the complete all-provider cache.
    start = len(child.buf)
    child.send(b"/model cache\r")
    child.wait(b"selected: first / uncached-start / low", start=start)
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
    assert first_model["observed_model_input_bytes"] == 0
    assert first_model["observed_input_tokens"] == 0
    assert first_model["observed_hard_input_tokens"] == 0
    child.wait(b"count=unknown", start=start)
    child.wait(b"estimate=none", start=start)
    stamp = cached_timestamp(cache)
    end = child.wait(stamp + b"\r\n" + initial_prompt, start=start)

    # /model list is a cache-only alias and retains the stored timestamp.
    original = cache_path.read_bytes()
    original_inode = cache_path.stat().st_ino
    child.send(b"/model list\r")
    end = child.wait(stamp + b"\r\n" + initial_prompt, start=end)
    assert cache_path.read_bytes() == original
    assert cache_path.stat().st_ino == original_inode

    # A later explicit refresh atomically replaces the complete catalog.
    child.send(b"/model cache\r")
    child.wait(b"16. second / vendor/future-model / low", start=end)
    refreshed = json.loads(cache_path.read_text(encoding="utf-8"))
    assert refreshed["updated_at_ms"] >= cache["updated_at_ms"]
    assert cache_path.stat().st_ino != original_inode
    refreshed_stamp = cached_timestamp(refreshed)
    end = child.wait(refreshed_stamp + b"\r\n" + initial_prompt, start=end)

    # A bare cached model chooses the highest recognized advertised effort.
    child.send(b"/model gpt-5.6-luna\r")
    end = child.wait(
        b"model for next turn: first / gpt-5.6-luna / high", start=end
    )
    prompt_end = child.wait(PROMPT, start=end)
    assert b"not known in the model cache" not in child.buf[end:prompt_end]
    end = prompt_end

    # Uncached identifiers and effort names pass through without local lookup.
    child.send(b"/model definitely-new-model\r")
    end = child.wait(
        b"model for next turn: first / definitely-new-model / high", start=end
    )
    end = child.wait(b"not known in the model cache", start=end)
    child.wait(PROMPT, start=end)
    child.send(b"/model fresh-model / quantum\r")
    end = child.wait(
        b"model for next turn: first / fresh-model / quantum", start=end
    )
    end = child.wait(b"not known in the model cache", start=end)
    child.wait(PROMPT, start=end)
    child.send(b"/model default / literal-effort\r")
    end = child.wait(
        b"model for next turn: first / default / literal-effort", start=end
    )
    end = child.wait(b"not known in the model cache", start=end)
    child.wait(PROMPT, start=end)
    child.send(b"/model second / future-new / cosmic\r")
    end = child.wait(
        b"model for next turn: second / future-new / cosmic", start=end
    )
    end = child.wait(b"not known in the model cache", start=end)
    child.wait(PROMPT, start=end)

    # Both numeric spellings select the exact flattened cached variant.
    child.send(b"/model 2\r")
    end = child.wait(
        b"model for next turn: first / gpt-5.6-terra / low", start=end
    )
    child.wait(PROMPT, start=end)
    child.send(b"/model #16\r")
    end = child.wait(
        b"model for next turn: second / vendor/future-model / low", start=end
    )
    child.wait(PROMPT, start=end)
    child.send(b"/model #9\r")
    end = child.wait(
        b"model for next turn: second / gpt-5.6-luna / high", start=end
    )
    child.wait(PROMPT, start=end)
    child.send(b"ping\r")
    answer_end = child.wait(b"pong", start=end)
    child.exit_cleanly(answer_end)

    session_id = new_session(before)
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
    resumed = Child(["--config", str(config), "--resume", session_id])
    resumed.wait(PROMPT)
    resumed.send(b"/status\r")
    status_end = resumed.wait(b"provider: second")
    resumed.wait(b"model: gpt-5.6-luna", start=status_end)
    status_end = resumed.wait(b"effort: high", start=status_end)
    resumed.wait(PROMPT, start=status_end)
    resumed.send(b"ping\r")
    answer_end = resumed.wait(b"pong", start=status_end)
    resumed.exit_cleanly(answer_end)
    turns = [event for event in events(session_id)
             if event["type"] == "turn_started"]
    assert turns[-1]["data"]["config"]["provider"] == "second"
    assert turns[-1]["data"]["config"]["model"] == "gpt-5.6-luna"
    assert turns[-1]["data"]["config"]["effort"] == "high"

    # Any provider failure leaves the previous complete cache untouched.
    complete_cache = cache_path.read_bytes()
    complete_inode = cache_path.stat().st_ino
    os.environ["SNAJPAGENT_FIXTURE_MODEL_FAILURE"] = "second"
    try:
        failing = Child(["--config", str(config)])
        failing.wait(PROMPT)
        failing.send(b"/model cache\r")
        failed_end = failing.wait(
            b"cannot refresh provider second: fixture model discovery failed"
        )
        failing.wait(b"\r\n" + initial_prompt, start=failed_end)
        failing.send(b"/exit\r")
        _, status = os.waitpid(failing.pid, 0)
        os.close(failing.fd)
        assert os.waitstatus_to_exitcode(status) == 0
    finally:
        os.environ.pop("SNAJPAGENT_FIXTURE_MODEL_FAILURE", None)
    assert cache_path.read_bytes() == complete_cache
    assert cache_path.stat().st_ino == complete_inode


def test_model_configuration_save():
    root = Path(os.environ["SNAJPAGENT_TEST_ROOT"])
    config = root / "config" / "model-save.ini"
    config.write_text(
        "# unrelated comment stays byte-for-byte\n"
        "[agent]\n"
        "model = save-base\n"
        "reasoning_effort = low\n"
        "max_goal_prompt_bytes = 123456\n"
        "[provider first]\n"
        "api_key_env = FIRST_API_KEY\n"
        "[provider second]\n"
        "api_key_env = SECOND_API_KEY\n",
        encoding="utf-8",
    )
    original = config.read_bytes()
    original_mode = config.stat().st_mode & 0o777
    before = session_ids()
    child = Child(["--config", str(config)])
    child.wait(PROMPT)

    # Selection without a suffix remains session-only.
    child.send(b"/model #9\r")
    end = child.wait(
        b"model for next turn: second / gpt-5.6-luna / high"
    )
    child.wait(PROMPT, start=end)
    assert config.read_bytes() == original

    # The one-letter spelling atomically persists a numbered cache row.
    old_inode = config.stat().st_ino
    child.send(b"/model 2 s\r")
    end = child.wait(
        b"model for next turn: first / gpt-5.6-terra / low", start=end
    )
    end = child.wait(
        f"configuration saved: {config}".encode(), start=end
    )
    child.wait(PROMPT, start=end)
    first_save = config.read_text(encoding="utf-8")
    assert config.stat().st_ino != old_inode
    assert config.stat().st_mode & 0o777 == original_mode
    assert "# unrelated comment stays byte-for-byte\n" in first_save
    assert "max_goal_prompt_bytes = 123456\n" in first_save
    assert "provider = first\n" in first_save
    assert "model = gpt-5.6-terra\n" in first_save
    assert "reasoning_effort = low\n" in first_save

    # The full spelling persists a typed provider/model/effort selection.
    child.send(b"/model second / durable-new / cosmic save\r")
    end = child.wait(
        b"model for next turn: second / durable-new / cosmic", start=end
    )
    warning_end = child.wait(b"not known in the model cache", start=end)
    end = child.wait(
        f"configuration saved: {config}".encode(), start=warning_end
    )
    child.wait(PROMPT, start=end)
    saved = config.read_bytes()
    saved_text = saved.decode("utf-8")
    assert "provider = second\n" in saved_text
    assert "model = durable-new\n" in saved_text
    assert "reasoning_effort = cosmic\n" in saved_text

    # Without a preceding selector, save and s remain literal model IDs.
    child.send(b"/model save\r")
    end = child.wait(b"model for next turn: first / save / cosmic", start=end)
    end = child.wait(b"not known in the model cache", start=end)
    child.wait(PROMPT, start=end)
    assert config.read_bytes() == saved
    child.send(b"/model s\r")
    end = child.wait(b"model for next turn: first / s / cosmic", start=end)
    end = child.wait(b"not known in the model cache", start=end)
    child.wait(PROMPT, start=end)
    assert config.read_bytes() == saved

    # A write failure does not change the selected runtime model.
    config.unlink()
    config.mkdir()
    child.send(b"/model rejected-model save\r")
    end = child.wait(
        b"configuration must be a regular file no larger than 64 KiB",
        start=end,
    )
    child.wait(PROMPT, start=end)
    child.send(b"/status\r")
    status_end = child.wait(b"model: s", start=end)
    child.wait(PROMPT, start=status_end)
    config.rmdir()
    config.write_bytes(saved)
    os.chmod(config, original_mode)
    child.exit_now()

    log = events(new_session(before))
    assert not [
        event for event in log
        if event["type"] == "model_selection_changed" and
        event["data"]["new_model"] == "rejected-model"
    ]

    # A new session consumes the saved provider and defaults from that path.
    child = Child(["--config", str(config)])
    child.wait(PROMPT)
    child.send(b"/status\r")
    end = child.wait(b"provider: second")
    child.wait(b"model: durable-new", start=end)
    end = child.wait(b"effort: cosmic", start=end)
    child.wait(PROMPT, start=end)
    child.exit_now()


def test_config_editor_reload():
    root = Path(os.environ["SNAJPAGENT_TEST_ROOT"])
    config = root / "config" / "editor.ini"
    valid_two = root / "config" / "editor-valid-two.ini"
    valid_one = root / "config" / "editor-valid-one.ini"
    invalid = root / "config" / "editor-invalid.ini"
    unrenderable = root / "config" / "editor-unrenderable.ini"
    network = root / "config" / "editor-network.ini"
    plan = root / "config" / "editor-plan"
    seen = root / "config" / "editor-seen"
    editor = root / "config" / "editor"
    config.write_text(
        "[agent]\nmodel = editor-base\nreasoning_effort = medium\n"
        "[provider]\napi_key_env = OPENAI_API_KEY\n"
        "[ui]\nverbosity = 0\n",
        encoding="utf-8",
    )
    valid_two.write_text(
        "[agent]\nmodel = ignored-default\nreasoning_effort = high\n"
        "[provider]\napi_key_env = OPENAI_API_KEY\n"
        "[ui]\nverbosity = 2\ntyping_pause_ms = 25\n"
        "prompt = {chat:{hour:2}:{minute:02}:{second:02}:}"
        "{rollout-idle:W{context:4}{goal_spinner}{provider_spinner}{tool_spinner}›}"
        "{rollout-active:W{context:4}{provider_spinner}»}\n"
        'prompt_spinner_goal = "\\0"\n'
        'prompt_spinner_provider = "\\0P"\n'
        'prompt_spinner_tool = " "\n',
        encoding="utf-8",
    )
    valid_one.write_text(
        "[agent]\nmodel = another-default\nreasoning_effort = low\n"
        "[provider]\napi_key_env = OPENAI_API_KEY\n"
        "[ui]\nverbosity = 1\n",
        encoding="utf-8",
    )
    invalid.write_text(
        "[ui]\nverbosity = 5\nprompt_spinner_tool = \"\\0\"\n"
        "prompt = {chat:{hour:002}}{rollout-idle:x}{rollout-active:y}\n",
        encoding="utf-8")
    unrenderable.write_text(
        "[ui]\nverbosity = 5\n"
        "prompt = {chat:x}{rollout-idle:" + ("x" * 600) +
        "}{rollout-active:z}\n",
        encoding="utf-8",
    )
    network_port = free_port()
    network.write_text(
        "[agent]\nmodel = network-default\nreasoning_effort = medium\n"
        "[provider]\napi_key_env = OPENAI_API_KEY\n"
        "[irc]\n"
        f"listen = 127.0.0.1:{network_port}\n"
        "model_nick = reloadagent\noperator_nick = reloadop\n"
        "room_name = lab\n",
        encoding="utf-8",
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
    old_editor = os.environ.get("EDITOR")
    old_plan = os.environ.get("SNAJPAGENT_EDITOR_PLAN")
    old_seen = os.environ.get("SNAJPAGENT_EDITOR_SEEN")
    os.environ["EDITOR"] = str(editor)
    os.environ["SNAJPAGENT_EDITOR_PLAN"] = str(plan)
    os.environ["SNAJPAGENT_EDITOR_SEEN"] = str(seen)
    try:
        before = session_ids()
        child = Child(["--config", str(config)])
        child.wait(PROMPT)
        session_id = new_session(before)

        plan.write_text("unchanged", encoding="utf-8")
        child.send(b"/config\r")
        end = child.wait(
            f"configuration unchanged: {config}".encode()
        )
        child.wait(PROMPT, start=end)
        assert seen.read_text(encoding="utf-8") == str(config)

        plan.write_text(str(valid_two), encoding="utf-8")
        child.send(b"/config\r")
        end = child.wait(f"configuration reloaded: {config}".encode(), start=end)
        child.wait("W  0% › ".encode(), start=end)
        child.send(b"/status\r")
        status_end = child.wait(b"verbosity: 2", start=end)
        child.wait(b"model: editor-base", start=end)
        child.wait(PROMPT, start=status_end)

        plan.write_text(str(invalid), encoding="utf-8")
        child.send(b"/config\r")
        end = child.wait(b"invalid configuration at line 4", start=status_end)
        child.wait("W  0% › ".encode(), start=end)
        child.send(b"/status\r")
        status_end = child.wait(b"verbosity: 2", start=end)
        child.wait(b"model: editor-base", start=end)
        child.wait(PROMPT, start=status_end)

        plan.write_text(str(unrenderable), encoding="utf-8")
        child.send(b"/config\r")
        end = child.wait(
            b"reloaded prompt cannot be rendered with the current selection",
            start=status_end,
        )
        child.wait(PROMPT, start=end)
        child.send(b"/status\r")
        status_end = child.wait(b"verbosity: 2", start=end)
        child.wait(b"model: editor-base", start=end)
        child.wait(PROMPT, start=status_end)

        # File changes are checked and loaded even when the editor exits nonzero.
        plan.write_text(f"nonzero:{valid_one}", encoding="utf-8")
        child.send(b"/config\r")
        warning_end = child.wait(
            b"$EDITOR exited unsuccessfully after changing the configuration",
            start=status_end,
        )
        end = child.wait(
            f"configuration reloaded: {config}".encode(), start=warning_end
        )
        child.wait(PROMPT, start=end)
        child.send(b"/status\r")
        status_end = child.wait(b"verbosity: 1", start=end)
        child.wait(b"model: editor-base", start=end)
        child.wait(PROMPT, start=status_end)

        # Process topology reloads too: enter and leave configured IRC mode.
        plan.write_text(str(network), encoding="utf-8")
        child.send(b"/config\r")
        end = child.wait(f"configuration reloaded: {config}".encode(), start=end)
        child.wait(f"reloadop@{socket.gethostname()}   : ".encode(), start=end)
        peer = IRCClient(network_port, "reloadpeer")
        peer.close()
        # Membership notifications start a turn; /config is idle-only.
        deadline = time.monotonic() + 8.0
        while True:
            log = events(session_id)
            turns = [event["data"]["turn_id"] for event in log
                     if event["type"] == "turn_started" and
                     "event=quit sender=reloadpeer" in event["data"]["text"]]
            if any(event["type"] == "turn_completed" and
                   event["data"]["turn_id"] in turns for event in log):
                break
            assert time.monotonic() < deadline, bytes(child.buf)
            child.drain(0.05)
        plan.write_text(str(valid_one), encoding="utf-8")
        child.send(b"/config\r")
        end = child.wait(f"configuration reloaded: {config}".encode(), start=end)
        child.wait(PROMPT, start=end)
        child.exit_now()

        # The resolved default path is passed to the editor and may be created.
        default_config = Path(DOTDIR) / "config.ini"
        prior_default = default_config.read_bytes() if default_config.exists() else None
        try:
            default_config.unlink(missing_ok=True)
            plan.write_text(str(valid_one), encoding="utf-8")
            child = Child([])
            child.wait(PROMPT)
            child.send(b"/config\r")
            end = child.wait(
                f"configuration reloaded: {default_config}".encode()
            )
            child.wait(PROMPT, start=end)
            assert seen.read_text(encoding="utf-8") == str(default_config)
            child.exit_now()
        finally:
            if prior_default is None:
                default_config.unlink(missing_ok=True)
            else:
                default_config.write_bytes(prior_default)
    finally:
        if old_editor is None:
            os.environ.pop("EDITOR", None)
        else:
            os.environ["EDITOR"] = old_editor
        if old_plan is None:
            os.environ.pop("SNAJPAGENT_EDITOR_PLAN", None)
        else:
            os.environ["SNAJPAGENT_EDITOR_PLAN"] = old_plan
        if old_seen is None:
            os.environ.pop("SNAJPAGENT_EDITOR_SEEN", None)
        else:
            os.environ["SNAJPAGENT_EDITOR_SEEN"] = old_seen


def test_known_context_meter():
    config = Path(os.environ["SNAJPAGENT_TEST_ROOT"]) / "config" / "models.ini"
    before = session_ids()
    child = Child(["--config", str(config)])
    child.wait(b"first/uncached-start/low   0%   \xe2\x80\xba ")
    child.send(b"/model gpt-5.6-luna / high\r")
    selected = child.wait(
        b"model for next turn: first / gpt-5.6-luna / high"
    )
    child.wait(b"first/gpt-5.6-luna/high   0%   \xe2\x80\xba ", start=selected)
    session_id = new_session(before)
    start = len(child.buf)
    child.send(b"slow\r")
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
    used = response["input_tokens_bound"]
    assert isinstance(hard, int) and hard > 0
    assert isinstance(used, int) and used > 0
    percent = min(100, (used * 100 + hard - 1) // hard)
    assert percent > 0
    expected = f"first/gpt-5.6-luna/high {str(percent) + '%':>4} ◴ » ".encode()
    child.wait(expected, start=start)
    child.send(b"\x03")
    interrupted = child.wait(b"turn interrupted", start=start)
    child.exit_cleanly(interrupted)


def test_config_and_cli_model_passthrough():
    config = Path(os.environ["SNAJPAGENT_TEST_ROOT"]) / "config" / "model-passthrough.ini"
    config.write_text(
        "[agent]\nmodel = openai/gpt-5.6\nreasoning_effort = default\n",
        encoding="utf-8",
    )
    before = session_ids()
    child = Child(["--config", str(config)])
    child.wait(b"default/openai/gpt-5.6/medium   0%   \xe2\x80\xba ")

    child.send(b"/status\r")
    end = child.wait(b"model: openai/gpt-5.6")
    child.wait(PROMPT, start=end)

    child.send(b"ping\r")
    answer_end = child.wait(b"pong")
    child.exit_cleanly(answer_end)

    session_id = new_session(before)
    log = events(session_id)
    turn = one(log, "turn_started")
    assert turn["data"]["config"]["model"] == "openai/gpt-5.6"
    assert turn["data"]["config"]["effort"] == "medium"

    resumed = Child([
        "--config", str(config), "-m", "vendor/future-model",
        "--effort", "custom-effort", "--resume", session_id
    ])
    resumed.wait(b"default/vendor/future-model/custom-effort   0%   \xe2\x80\xba ")
    start = len(resumed.buf)
    resumed.send(b"/status\r")
    end = resumed.wait(
        b"model: vendor/future-model (staged once)", start=start
    )
    resumed.wait(PROMPT, start=end)
    resumed.send(b"ping\r")
    resumed.wait(b"default/vendor/future-model/custom-effort   ?% \xe2\x97\xb4 \xc2\xbb ",
                 start=end)
    answer_end = resumed.wait(b"pong", start=end)
    idle_end = resumed.wait(
        b"default/openai/gpt-5.6/medium   0%   \xe2\x80\xba ", start=answer_end
    )
    resumed.send(b"/exit\r")
    _, status = os.waitpid(resumed.pid, 0)
    os.close(resumed.fd)
    assert os.waitstatus_to_exitcode(status) == 0, idle_end

    resumed_turns = [event for event in events(session_id)
                     if event["type"] == "turn_started"]
    assert resumed_turns[-1]["data"]["config"]["model"] == "vendor/future-model"
    assert resumed_turns[-1]["data"]["config"]["effort"] == "custom-effort"


def test_exit_resume_matrix():
    for exit_input in (b"/exit\r", b"\x04"):
        before = session_ids()
        child = Child(["--no-color"])
        child.wait(PROMPT)
        session_id = new_session(before)
        child.send(exit_input)
        command = child.finish()
        arguments = command_arguments(command)
        assert arguments[-2:] == ["--resume", session_id], arguments
        assert arguments[arguments.index("--dotdir") + 1] == DOTDIR

    before = session_ids()
    cancelled = Child(["--no-color"])
    cancelled.wait(DEFAULT_IDLE_PROMPT)
    cancelled_id = new_session(before)
    start = len(cancelled.buf)
    cancelled.send(b"\x03" * 4)
    deadline = time.monotonic() + 8.0
    while bytes(cancelled.buf[start:]).count(b"^C\r\n") < 4:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not cancelled.read_once(remaining):
            raise AssertionError(f"missing Ctrl-C cancellations: {cancelled.buf!r}")
    assert os.waitpid(cancelled.pid, os.WNOHANG) == (0, 0)
    cancelled.send(b"\x03")
    command = cancelled.finish()
    assert command_arguments(command)[-2:] == ["--resume", cancelled_id]

    for signal_number in (signal.SIGHUP, signal.SIGTERM):
        before = session_ids()
        child = Child(["--no-color"])
        child.wait(PROMPT)
        session_id = new_session(before)
        os.kill(child.pid, signal_number)
        command = child.finish(expected=128 + signal_number)
        assert command_arguments(command)[-2:] == ["--resume", session_id]

    before = session_ids()
    active_eof = Child(["--no-color"])
    active_eof.wait(PROMPT)
    active_eof.send(b"slow\r")
    active_eof.wait(b"working slowly")
    active_eof.send(b"\x04")
    active_eof_command = active_eof.finish()
    active_eof_id = new_session(before)
    assert command_arguments(active_eof_command)[-2:] == [
        "--resume", active_eof_id
    ]
    assert one(events(active_eof_id), "turn_completed")

    before = session_ids()
    archived = Child(["--no-color"])
    archived.wait(PROMPT)
    archived_id = new_session(before)
    archived.send(b"/archive\r")
    archived_command = archived.finish()
    assert command_arguments(archived_command)[-2:] == [
        "--resume", archived_id
    ]
    assert one(events(archived_id), "session_archived")

    before = session_ids()
    deleted = Child(["--no-color"])
    deleted.wait(PROMPT)
    deleted_id = new_session(before)
    deleted.send(b"/delete\r")
    deleted.wait(b"type the displayed 8-character id prefix to confirm")
    deleted.send(deleted_id[:8].encode() + b"\r")
    deleted.finish(expect_resume=False)
    assert not (STATE_ROOT / deleted_id).exists()

    before = session_ids()
    original = Child(["--no-color"])
    original.wait(PROMPT)
    staged_id = new_session(before)
    original_command = original.exit_now()
    assert command_arguments(original_command)[-2:] == ["--resume", staged_id]
    staged = Child([
        "--no-color", "-m", "future/model", "--effort", "xhigh",
        "--resume", staged_id,
    ])
    staged.wait(PROMPT)
    staged_command = staged.exit_now()
    staged_arguments = command_arguments(staged_command)
    assert staged_arguments[staged_arguments.index("-m") + 1] == "future/model"
    assert staged_arguments[staged_arguments.index("--effort") + 1] == "xhigh"

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
    failed_command = failed.finish(expected=3)
    failed_arguments = command_arguments(failed_command)
    failed_id = new_session(before)
    assert failed_arguments[-2:] == ["--resume", failed_id]
    assert failed_arguments[failed_arguments.index("--listen") + 1] == \
        occupied_endpoint
    occupied.close()


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
    ])
    client.wait(chat_prompt("clientop"))
    client_id = new_session(before)
    first_links = accept_connections(upstream, 2)
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
    ])
    server.wait(chat_prompt("serverop"))
    server_id = new_session(before)
    peer = IRCClient(server_port, "firstpeer")
    peer.message("retained room message")
    server.wait("firstpeer › retained room message".encode())
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
    resumed_server.wait("history @firstpeer › retained room message".encode())
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
    ])
    combined.wait(chat_prompt("combinedop"))
    combined_id = new_session(before)
    first_links = accept_connections(upstream, 2)
    peer = IRCClient(combined_port, "combinedpeer")
    peer.close()
    combined_command = combined.exit_now()
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
    resumed_combined.exit_now()
    for connection in resumed_links:
        connection.close()
    upstream.close()


def test_network_collision_prompts():
    port = free_port()
    address = f"127.0.0.1:{port}"
    children = []
    peer = None
    old_user = os.environ.get("USER")
    os.environ["USER"] = "root"
    try:
        server = Child(["--no-color", "-vvvvvv", "-s", address, "-r", "lab"])
        children.append(server)
        server.wait(chat_prompt("root0"))
        server.send(b"/names\r")
        names_end = server.wait(b"model nick: agent0")
        server.wait(b"operator nick: root0", start=names_end)
        for suffix in (1, 2):
            client = Child(["--no-color", "-c", address])
            children.append(client)
            client.wait(chat_prompt(f"root{suffix}"))
            client.send(b"/names\r")
            client.wait(f"model agent{suffix} operator root{suffix}".encode())
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
    finally:
        try:
            if peer:
                peer.close()
            for child in reversed(children):
                child.send(b"\x04")
                arguments = command_arguments(child.finish())
                assert "--model-nick" not in arguments
                assert "--operator-nick" not in arguments
        finally:
            if old_user is None:
                os.environ.pop("USER", None)
            else:
                os.environ["USER"] = old_user


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
        session_id = new_session(before)
        links = accept_connections(upstream, 2)
        for link in links:
            link.settimeout(4.0)
            wire = bytearray()
            while b"USER " not in wire:
                wire.extend(link.recv(4096))
            nick = re.search(rb"NICK (\w+)\r\n", wire)[1].decode()
            accepted = nick + "7"
            link.sendall((f":fake 001 {accepted} :welcome\r\n"
                          f":fake 005 {accepted} SAJROOM=#lab :supported\r\n"
                          f":fake 376 {accepted} :end\r\n").encode())
            wire = bytearray()
            while b"JOIN #lab\r\n" not in wire:
                wire.extend(link.recv(4096))
            link.sendall((f":{accepted}!u@fake JOIN #lab\r\n"
                          f":fake 353 {accepted} = #lab :@operator7 agent7\r\n"
                          f":fake 366 {accepted} #lab :end\r\n"
                          ":fake BATCH +h chathistory #lab\r\n"
                          ":fake BATCH -h\r\n").encode())
        child.wait(f"operator7@{socket.gethostname()}".encode())
        child.drain()
        # Preserve a draft and its cursor through a live rename.
        child.send(b"/stats\x1b[D")
        child.drain()
        start = len(child.buf)
        for link in links:
            link.sendall(b":operator7!u@fake NICK :operator8\r\n"
                         b":agent7!u@fake NICK :agent8\r\n")
        child.wait(f"operator8@{socket.gethostname()}".encode(), start=start)
        child.wait(b"/stats", start=start)
        child.send(b"u\r")
        status_end = child.wait(b"verbosity: 0", start=start)
        child.wait(chat_prompt("operator8"), start=status_end)
        child.drain()
        assert child.buf[start:].count(b"operator7 is now known as") == 1
        assert child.buf[start:].count(b"agent7 is now known as") == 1
        # Local input is attributed to the accepted operator and the model's
        # request context includes a fresh snapshot with both accepted nicks.
        start = len(child.buf)
        child.send(b"network_view_stream\r")
        child.wait("@operator8 › network_view_stream".encode(), start=start)
        child.wait(chat_active_prompt("operator8"), start=start)
        child.send(b"/stats\x1b[D")
        child.drain(0.03)
        for link in links:
            link.sendall(b":operator8!u@fake NICK :operator9\r\n"
                         b":agent8!u@fake NICK :agent9\r\n")
        renamed = child.wait(f"operator9@{socket.gethostname()}".encode(),
                             start=start)
        child.wait(b"/stats", start=renamed)
        child.send(b"u\r")
        child.wait(b"verbosity: 0", start=renamed)
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
        # EOF gracefully finishes it; /exit is an idle-only command.
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
    before = session_ids()
    child = Child(["-m", unsafe_model, "--effort", unsafe_effort])
    child.wait(b"default/" + visible + b"   0%   \xe2\x80\xba ")
    assert unsafe_model.encode() not in child.buf
    assert unsafe_effort.encode() not in child.buf
    child.send(b"ping\r")
    child.wait(b"default/" + visible + b"   ?% \xe2\x97\xb4 \xc2\xbb ")
    answer_end = child.wait(b"pong")
    child.exit_cleanly(answer_end)

    turn = one(events(new_session(before)), "turn_started")
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
        before = session_ids()
        child = Child([])
        child.wait(DEFAULT_IDLE_PROMPT)
        start = len(child.buf)
        child.send(prompt.encode() + b"\r")
        recovered_end = child.wait(recovered, start=start)
        child.wait(DEFAULT_ACCOUNTED_IDLE_PROMPT, start=recovered_end)
        visible = bytes(child.buf[start:])
        assert correction.encode() not in visible
        child.exit_cleanly(recovered_end)

        log = events(new_session(before))
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
    network_idle = f"localop@{socket.gethostname()}   : ".encode()
    rollout_idle = f"default/{DEFAULT_MODEL}/medium   ?%   › ".encode()

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
        assert first not in child.buf[chat_start:]
        assert second not in child.buf[chat_start:]

        switch_start = len(child.buf)
        child.send(switch)
        boundary_end = child.wait("── rollout ──".encode(), start=switch_start)
        prompt_end = child.wait(rollout_idle, start=boundary_end)
        transition = bytes(child.buf[boundary_end:prompt_end])
        prompt_at = transition.find(rollout_idle)
        assert prompt_at >= 0, transition
        catchup = transition[:prompt_at]
        assert network_idle not in catchup, catchup
        assert rollout_idle not in catchup, catchup
        assert catchup.count(first) == 1, catchup
        assert catchup.count(second) == 1, catchup
        assert catchup.find(first) < catchup.find(second), catchup
        assert transition.count(rollout_idle) == 1, transition
        return prompt_end

    try:
        child.wait(network_idle)
        session_id = new_session(before)
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
        child.send(b"/rollout\r")
        child.wait(rollout_idle, start=same_start)
        child.drain()
        same_view = bytes(child.buf[same_start:])
        assert "── rollout ──".encode() not in same_view, same_view
        assert tab_first not in same_view, same_view
        assert tab_second not in same_view, same_view
        assert same_view.count(rollout_idle) == 1, same_view

        chat_start = len(child.buf)
        child.send(b"\t")
        chat_boundary = child.wait("── chat ──".encode(), start=chat_start)
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
        child.wait(DEFAULT_ACTIVE_PROMPT, start=active_start)
        steer_start = len(child.buf)
        child.send(b"rollout active steer\r")
        answer_end = child.wait(b"steered: rollout active steer",
                                start=steer_start)
        child.wait(rollout_idle, start=answer_end)
        active_output = bytes(child.buf[active_start:])
        frames = ["◴", "◷", "◶", "◵"]
        active_labels = [
            f"default/{DEFAULT_MODEL}/medium   ?% {frame} » ".encode() +
            b"rollout active steer" for frame in frames
        ]
        assert active_output.count(submitted_start) == 1, active_output
        assert sum(active_output.count(label) for label in active_labels) == 1
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
        child.send(b"/chat\r")
        chat_boundary = child.wait("── chat ──".encode(), start=backlog_start)
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
    steering = [
        event for event in log
        if event["type"] == "steering_added" and
        event["data"]["turn_id"] == slow_turn
    ]
    assert len(steering) == 1
    assert steering[0]["data"]["text"] == "rollout active steer"
    chat_turns = [
        event for event in log
        if event["type"] == "turn_started" and
        "network_one" in event["data"]["text"]
    ]
    assert len(chat_turns) == 1
    assert chat_turns[0]["data"]["text"] != "network_one"


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
    network_idle = f"localop@{socket.gethostname()}   : ".encode()
    network_active = f"localop@{socket.gethostname()} ◴ : ".encode()
    network_rollout_idle = (
        f"default/{DEFAULT_MODEL}/medium   0%   › ".encode()
    )
    network_rollout_accounted_idle = (
        f"default/{DEFAULT_MODEL}/medium   ?%   › ".encode()
    )
    try:
        child.wait(network_idle)
        session_id = new_session(before)

        view_start = len(child.buf)
        child.send(b"\t")
        rollout_end = child.wait("── rollout ──".encode(), start=view_start)
        child.wait(network_rollout_idle, start=rollout_end)
        child.send(b"/rollout\r")
        child.drain()
        assert child.buf[view_start:].count("── rollout ──".encode()) == 1
        child.send(b"/chat\r")
        chat_end = child.wait("── chat ──".encode(), start=rollout_end)
        child.wait(network_idle, start=chat_end)
        child.send(b"/chat\r")
        child.drain()
        assert child.buf[view_start:].count("── chat ──".encode()) == 1
        child.send(b"/help\r")
        help_end = child.wait(b"Empty Tab switch view", start=chat_end)
        child.wait(network_idle, start=help_end)

        draft_start = len(child.buf)
        child.send(b"x\t")
        draft_end = child.wait(b"x\x1b[K   \x1b[K", start=draft_start)
        edit = bytes(child.buf[draft_start:draft_end])
        assert b"\x1b[2K" not in edit, edit
        assert network_idle not in edit, edit
        assert "── rollout ──".encode() not in edit
        clear_draft_incrementally(child, network_idle)

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
        child.send(b"\t")
        rollout_end = child.wait("── rollout ──".encode(), start=stream_start)
        child.wait(b"model-output-one", start=rollout_end)
        child.wait(network_active, start=stream_start)
        child.wait(b"model-output-two", start=rollout_end)

        chat_wire_start = len(human.buf)
        peer_agent.message("chat backlog")
        human.wait(b"PRIVMSG #lab :chat backlog\r\n", start=chat_wire_start)
        child.send(b"\t")
        chat_end = child.wait("── chat ──".encode(), start=rollout_end)
        backlog_end = child.wait(b"chat backlog", start=chat_end)
        child.send(b"/chat\r")
        child.drain()
        assert child.buf[chat_end:].count(b"chat backlog") == 1
        human.wait(b"PRIVMSG #lab :network stream acknowledged\r\n",
                   start=model_wire_start)
        child.drain()
        assert b"model-output-three" not in child.buf[chat_end:]
        child.wait(network_idle, start=backlog_end)
        child.send(b"\t")
        tail_end = child.wait(b"model-output-three", start=chat_end)
        visible_stream = bytes(child.buf[stream_start:tail_end])
        for fragment in (b"model-output-one", b"model-output-two",
                         b"model-output-three"):
            assert visible_stream.count(fragment) == 1, visible_stream
        child.send(b"\t")
        chat_end = child.wait("── chat ──".encode(), start=tail_end)
        child.wait(network_idle, start=chat_end)
        assert (b"PRIVMSG #lab :model-output-one model-output-two "
                b"model-output-three\r\n" not in human.buf[model_wire_start:])

        child.send(b"/rollout\r")
        rollout_end = child.wait("── rollout ──".encode(), start=chat_end)
        child.wait(network_rollout_accounted_idle, start=rollout_end)
        search_start = len(child.buf)
        child.send(b"\x12network_view_stream")
        child.wait(
            b"(reverse-i-search)`network_view_stream': network_view_stream",
            start=search_start,
        )
        child.send(b"\x07")
        child.wait(network_rollout_accounted_idle, start=search_start)
        child.send(b"/chat\r")
        chat_end = child.wait("── chat ──".encode(), start=search_start)
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

        child.send(b"/verbose 1\r")
        verbose_end = child.wait(b"verbosity: 1", start=terminal_start)
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
        child.send(b"/rollout\r")
        child.wait(b"\xe2\x86\x92 exec  timeout=1000ms  'fixture ok'",
                   start=tool_start)
        child.wait(b"arguments: {\"command\":\"fixture ok\"", start=tool_start)
        child.wait(b"fixture command succeeded", start=tool_start)
        child.wait(PROMPT, start=tool_start)
        child.send(b"/chat\r")
        child.wait("── chat ──".encode(), start=tool_start)

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
        child.send(b"network_reminder\r")
        human.wait(b"PRIVMSG #lab :network reminder reply\r\n", start=wire_start)
        wait_turn_completed(child, session_id, "network_reminder")
        child.wait(network_idle, start=reminder_start)

        child.send(b"/verbose 2\r")
        verbose_end = child.wait(b"verbosity: 2", start=verbose_end)
        child.wait(network_idle, start=verbose_end)
        commentary_start = len(child.buf)
        wire_start = len(human.buf)
        child.send(b"network_commentary\r")
        human.wait(b"PRIVMSG #lab :network commentary reply\r\n",
                   start=wire_start)
        wait_turn_completed(child, session_id, "network_commentary")
        child.send(b"/rollout\r")
        child.wait(b"\xe2\x80\xa2 network local planning", start=verbose_end)
        child.send(b"/chat\r")
        child.wait("── chat ──".encode(), start=verbose_end)
        child.wait(network_idle, start=commentary_start)

        wire_start = len(human.buf)
        child.send(b"network_managed\r\t")
        managed_view = child.wait("── rollout ──".encode(), start=verbose_end)
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
        child.wait(PROMPT, start=managed_complete)
        assert managed_end > wire_start
        assert (b"PRIVMSG #lab :network managed local completion\r\n" not in
                human.buf[wire_start:])
        child.send(b"/chat\r")
        child.wait("── chat ──".encode(), start=managed_view)

        compact_start = len(child.buf)
        child.send(b"/compact\r")
        compact_prompt = child.wait(network_idle, start=compact_start)
        child.drain()
        assert COMPACTED not in child.buf[compact_start:]
        child.send(b"/rollout\r")
        compact_end = child.wait(COMPACTED, start=compact_prompt)
        child.wait(PROMPT, start=compact_end)
        child.send(b"/chat\r")
        chat_end = child.wait("── chat ──".encode(), start=compact_end)
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
    count_steering = next(
        event for event in log
        if event["type"] == "steering_added" and
        event["data"]["turn_id"] == count_turn["data"]["turn_id"]
    )
    count_start = next(
        event for event in log
        if event["type"] == "response_started" and
        event["data"]["turn_id"] == count_turn["data"]["turn_id"]
    )
    assert count_start["data"]["steering_ids"] == [
        count_steering["data"]["steering_id"]
    ]
    reminder_turn = next(
        event for event in turns if "network_reminder" in event["data"]["text"]
    )
    reminders = [
        event for event in log
        if event["type"] == "irc_reply_reminder" and
        event["data"]["turn_id"] == reminder_turn["data"]["turn_id"]
    ]
    assert len(reminders) == 1
    reminder_responses = [
        event for event in log
        if event["type"] == "response_started" and
        event["data"]["turn_id"] == reminder_turn["data"]["turn_id"]
    ]
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
            "irc_send arguments are invalid")

    zero_turn = next(
        event for event in turns if "network_zero" in event["data"]["text"]
    )
    zero_reminders = [
        event for event in log
        if event["type"] == "irc_reply_reminder" and
        event["data"]["turn_id"] == zero_turn["data"]["turn_id"]
    ]
    zero_responses = [
        event for event in log
        if event["type"] == "response_started" and
        event["data"]["turn_id"] == zero_turn["data"]["turn_id"]
    ]
    assert len(zero_reminders) == 1
    assert len(zero_responses) == 2

    managed_turn = next(
        event for event in turns if "network_managed" in event["data"]["text"]
    )
    turn_id = managed_turn["data"]["turn_id"]
    steering = [
        event for event in log
        if event["type"] == "steering_added" and
        event["data"]["turn_id"] == turn_id
    ]
    assert len(steering) == 1
    assert "network managed mention" in steering[0]["data"]["text"]
    completed = [
        event for event in log
        if event["type"] == "response_completed" and
        event["data"]["turn_id"] == turn_id
    ]
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

def test_five_ctrl_c_exit():
    for prompt in (None, b"slow", b"engine_blocked"):
        child = Child([])
        child.wait_idle_prompt()
        if prompt:
            child.send(prompt + b"\r")
            child.wait(b"engine-block-start" if prompt == b"engine_blocked"
                       else b"working slowly")
        child.send(b"\x03" * 4)
        child.drain(0.1)
        assert os.waitpid(child.pid, os.WNOHANG) == (0, 0)
        child.send(b"\x03")
        child.wait(RESUME_HEADER, timeout=4.0)
        child.finish()


def test_ctrl_c_sequence_reset():
    child = Child([])
    try:
        child.wait_idle_prompt()
        child.send(b"\x03" * 4)
        child.drain(2.1)
        child.send(b"\x03")
        child.drain(0.05)
        assert os.waitpid(child.pid, os.WNOHANG) == (0, 0)
        child.send(b"x" + b"\x03" * 4)
        child.drain(0.05)
        assert os.waitpid(child.pid, os.WNOHANG) == (0, 0)
        child.send(b"\x03")
        child.wait(RESUME_HEADER)
        child.finish()
    finally:
        child.kill()


def test_full_input_queue_keeps_exit_live():
    child = Child([])
    try:
        child.wait_idle_prompt()
        child.send(b"engine_blocked\r")
        child.wait(b"engine-block-start")
        child.send(b"/verbose 1\r" * 32 + b"retained-draft\r")
        child.wait(b"input backlog is full", timeout=1.0)
        child.wait(b"\a", timeout=1.0)
        child.send(b"\x03" * 5)
        child.wait(RESUME_HEADER, timeout=4.0)
        child.finish()
    finally:
        child.kill()


def test_history_lock_keeps_editing_live():
    child = Child([])
    try:
        child.wait_idle_prompt()
        with (Path(DOTDIR) / "prompt_history").open("r+") as history:
            fcntl.lockf(history, fcntl.LOCK_EX)
            child.send(b"\x12")
            start = child.wait(b"reverse-i-search")
            child.send(b"locked-history")
            child.wait(b"locked-history", start=start, timeout=0.25)
            child.send(b"\x07draft-alive")
            child.drain(0.1)
            visible = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]|\r", b"", child.buf[start:])
            assert b"draft-alive" in visible, visible
            fcntl.lockf(history, fcntl.LOCK_UN)
        child.send(b"\x03")
        child.drain(0.1)
        child.exit_now()
    finally:
        child.kill()


def test_editor_during_render_flood():
    for mode in ("--markdown", "--no-markdown"):
        before = session_ids()
        child = Child([mode])
        try:
            child.wait_idle_prompt()
            child.send(b"render_flood\r")
            start = child.wait(b"row-0000")
            child.send(b"live-draft")
            deadline = time.monotonic() + 0.25
            while b"live-draft" not in re.sub(
                    rb"\x1b\[[0-?]*[ -/]*[@-~]|\r", b"", child.buf[start:]):
                remaining = deadline - time.monotonic()
                assert remaining > 0, "rendering stopped local editing"
                child.read_once(remaining)
            end = child.wait(b"flood-end", start=start)
            child.wait_idle_prompt(start=end)
            child.send(b"\x03")
            child.drain(0.1)
            child.exit_now()
        finally:
            child.kill()
        completed = one(events(new_session(before)), "response_completed")
        text = completed["data"]["items"][0]["text"]
        expected = "| row | text |\n| --- | --- |\n" + "".join(
            f"| row-{i:04} | **bold** and `code` |\n" for i in range(2048)
        ) + "\nflood-end\n"
        assert text == expected


def test_editor_during_blocked_engine():
    child = Child([])
    failure = None
    try:
        child.wait_idle_prompt()
        child.send(b"engine_blocked\r")
        after = child.wait(b"engine-block-start")
        tasks = Path(f"/proc/{child.pid}/task")
        if tasks.exists():
            # GCC TSan adds one instrumentation worker, not an application thread.
            tsan = "libtsan" in Path(f"/proc/{child.pid}/maps").read_text()
            assert len(list(tasks.iterdir())) == 2 + int(tsan)
        child.drain(0.4)
        assert b"engine-block-start \n" in child.buf.replace(b"\r", b"")
        assert len(set(re.findall("[◴◷◶◵]", child.buf[after:].decode()))) > 1
        fcntl.ioctl(child.fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", 24, 48, 0, 0))
        child.send(b"responsive-draft")
        try:
            deadline = time.monotonic() + 0.25
            while b"responsive-draft" not in re.sub(
                    rb"\x1b\[[0-?]*[ -/]*[@-~]|\r", b"", child.buf[after:]):
                remaining = deadline - time.monotonic()
                assert remaining > 0, (
                    f"engine stall stopped local editing: {bytes(child.buf[after:])!r}"
                )
                child.read_once(remaining)
        except AssertionError as exc:
            failure = exc
        child.send(b"\x03")
        end = child.wait(b"engine-block-end", start=after)
        child.exit_cleanly(end)
    finally:
        child.kill()
        if failure:
            raise failure


if __name__ == "__main__":
    test_editor_during_render_flood()
    test_editor_during_blocked_engine()
    test_five_ctrl_c_exit()
    test_ctrl_c_sequence_reset()
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
    test_typing_pause_and_stream_snapshots()
    test_armed_fifo()
    test_read_only_queries()
    test_read_only_multiline_compaction_and_chat()
    test_read_only_queue_replay_and_edit()
    test_managed_command_steering_and_tab_queue()
    test_steering_during_pre_response_compaction()
    test_steering_during_capacity_recovery_compaction()
    test_agents_md_config()
    test_active_ctrl_c_clears_draft()
    test_ctrl_c_cancels_partial_editor_states()
    test_interrupt()
    test_prompt_history_and_reverse_search()
    test_multiline_and_paste()
    test_resume_pauses_fifo()
    test_goal_quoted_reserved_wording()
    test_goal_automatic_continuation()
    test_model_created_goal_continuation()
    test_goal_configured_wording_limit()
    test_goal_model_rewrite_and_lock()
    test_goal_pause_resume_and_queue_priority()
    test_goal_user_terminal_commands_and_unlock()
    test_goal_refusal_failure_block_and_restart_pause()
    test_queue_mutation_commands()
    test_preferences_and_verbosity()
    test_command_name_completion()
    test_uncached_typed_model_selection()
    test_model_cache_and_selection()
    test_model_configuration_save()
    test_config_editor_reload()
    test_known_context_meter()
    test_config_and_cli_model_passthrough()
    test_exit_resume_matrix()
    test_network_resume_roles()
    test_network_collision_prompts()
    test_network_live_nick_prompt()
    test_prompt_identity_is_terminal_safe()
    test_model_message_corrections_are_private_and_specific()
    test_network_view_routing_and_atomic_catchup()
    test_network_chat_and_managed_mention()
    print("pty_active: ok")
