#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import errno
import json
import os
import pty
import select
import signal
import sys
import time
from pathlib import Path

BINARY = os.path.abspath(sys.argv[1])
WORKSPACE = os.path.abspath(sys.argv[2])
STATE_ROOT = Path(os.environ["XDG_STATE_HOME"]) / "snajpagent" / "sessions"
PROMPT = "› ".encode()


class Child:
    def __init__(self, args):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.chdir(WORKSPACE)
            os.execv(BINARY, [BINARY, *args])
        self.buf = bytearray()

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

    def send(self, data):
        os.write(self.fd, data)

    def drain(self, duration=0.25):
        end = time.monotonic() + duration
        while time.monotonic() < end:
            self.read_once(min(0.05, end - time.monotonic()))

    def exit_cleanly(self, after):
        self.wait(b"\r" + PROMPT, start=after, timeout=8.0)
        self.send(b"/exit\r")
        _, status = os.waitpid(self.pid, 0)
        os.close(self.fd)
        code = os.waitstatus_to_exitcode(status)
        if code != 0:
            raise AssertionError(f"exit status {code}; got {bytes(self.buf)!r}")

    def kill(self):
        os.kill(self.pid, signal.SIGKILL)
        os.waitpid(self.pid, 0)
        os.close(self.fd)


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


def test_utf8_prompt_cursor_column():
    child = Child([])
    try:
        child.wait(PROMPT)
        start = len(child.buf)
        child.send(b"a")
        child.drain()
        redraw = bytes(child.buf[start:])
        assert b"\r\x1b[3C" in redraw, redraw
        assert b"\r\x1b[5C" not in redraw, redraw
    finally:
        child.kill()


def test_steering():
    before = session_ids()
    child = Child([])
    child.wait(PROMPT)
    child.send(b"slow\r")
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


def test_split_utf8_steering():
    before = session_ids()
    child = Child([])
    child.wait(PROMPT)
    child.send(b"slow_utf8\rchange\r")
    answer_end = child.wait(b"steered: change")
    child.exit_cleanly(answer_end)

    interrupted = one(events(new_session(before)), "response_interrupted")
    assert interrupted["data"]["partial_public"] == []


def test_armed_fifo():
    before = session_ids()
    child = Child([])
    child.wait(PROMPT)
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


def test_interrupt():
    before = session_ids()
    child = Child([])
    child.wait(PROMPT)
    child.send(b"slow\r")
    child.wait(b"working slowly")
    child.send(b"\x03")
    interrupted_end = child.wait(b"turn interrupted")
    child.exit_cleanly(interrupted_end)

    log = events(new_session(before))
    response = one(log, "response_interrupted")
    turn = one(log, "turn_interrupted")
    assert response["data"]["origin"] == "user"
    assert response["data"]["reason"] == "cancelled"
    assert turn["data"]["origin"] == "user"
    assert turn["data"]["reason"] == "cancelled"


def test_multiline_and_paste():
    before = session_ids()
    child = Child([])
    child.wait(PROMPT)
    child.send(b"line one\nline two\r")
    first_end = child.wait(b"fixture answer")
    child.wait(b"\r" + PROMPT, start=first_end)
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

    resumed = Child(["-r", session_id])
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



def test_preferences_and_verbosity():
    before = session_ids()
    child = Child([])
    child.wait(PROMPT)

    child.send(b"/effort high\r")
    end = child.wait(b"effort for next turn: high")
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
    assert effort["data"] == {"old_effort": "default", "new_effort": "high"}
    assert turn["data"]["config"]["effort"] == "high"


def test_command_name_completion():
    child = Child([])
    child.wait(PROMPT)

    start = len(child.buf)
    child.send(b"/he\t")
    end = child.wait(PROMPT + b"/help", start=start)
    child.send(b"\r")
    help_end = child.wait(b"/compact", start=end)
    child.wait(b"Tab complete/indent/queue", start=help_end)
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
        (b"/v", b"/verbose"),
        (b"/q", b"/queue"),
        (b"/u", b"/unqueue"),
        (b"/n", b"/next"),
        (b"/a", b"/archive"),
        (b"/c", b"/compact"),
        (b"/d", b"/delete"),
        (b"/ex", b"/exit"),
    ):
        start = len(child.buf)
        child.send(prefix + b"\t")
        end = child.wait(PROMPT + command, start=start)
        child.send(b"\x15")
        child.wait(PROMPT, start=end)

    start = len(child.buf)
    child.send(b"/mo gpt\x1b[D\x1b[D\x1b[D\x1b[D\t")
    end = child.wait(PROMPT + b"/model gpt", start=start)
    child.send(b"\x15")
    child.wait(PROMPT, start=end)

    start = len(child.buf)
    child.send(b"/h\ti\t")
    end = child.wait(PROMPT + b"/history", start=start)
    child.send(b"\x15")
    child.wait(PROMPT, start=end)

    start = len(child.buf)
    child.send(b"x\t")
    end = child.wait(PROMPT + b"x   ", start=start)
    child.send(b"\x15")
    child.wait(PROMPT, start=end)

    child.send(b"slow\r")
    child.wait(b"working slowly")

    start = len(child.buf)
    child.send(b"/he\t")
    end = child.wait(b"steer " + PROMPT + b"/help", start=start)
    child.send(b"\r")
    help_end = child.wait(b"/compact", start=end)
    child.wait(b"Tab complete/indent/queue", start=help_end)
    child.wait(b"steer " + PROMPT, start=help_end)

    start = len(child.buf)
    child.send(b"/?\r")
    alias_end = child.wait(b"/help", start=start)
    child.wait(b"/?", start=alias_end)
    alias_end = child.wait(b"/compact", start=alias_end)
    child.wait(b"Tab complete/indent/queue", start=alias_end)
    child.wait(b"steer " + PROMPT, start=alias_end)

    start = len(child.buf)
    child.send(b"/sta\t")
    end = child.wait(b"steer " + PROMPT + b"/status", start=start)
    child.send(b"\r")
    status_end = child.wait(b"state: active", start=end)
    answer_end = child.wait(b"slow complete", start=status_end)
    child.exit_cleanly(answer_end)


def test_arbitrary_model_selection():
    before = session_ids()
    child = Child([])
    child.wait(PROMPT)

    child.send(b"/model\r")
    end = child.wait(b"model for next turn: gpt-5.5-2026-04-23")
    child.wait(PROMPT, start=end)

    child.send(b"/model gpt-5.6-sol\r")
    end = child.wait(b"model for next turn: gpt-5.6-sol")
    child.wait(PROMPT, start=end)

    child.send(b"ping\r")
    answer_end = child.wait(b"pong")
    child.exit_cleanly(answer_end)

    log = events(new_session(before))
    changed = one(log, "model_changed")
    turn = one(log, "turn_started")
    assert changed["data"] == {
        "old_model": "gpt-5.5-2026-04-23",
        "new_model": "gpt-5.6-sol",
    }
    assert turn["data"]["config"]["model"] == "gpt-5.6-sol"
    assert turn["data"]["config"]["effort"] == "medium"

def test_config_and_cli_model_passthrough():
    config = Path(os.environ["XDG_CONFIG_HOME"]) / "model-passthrough.ini"
    config.write_text(
        "[agent]\nmodel = openai/gpt-5.6\nreasoning_effort = default\n",
        encoding="utf-8",
    )
    before = session_ids()
    child = Child(["-c", str(config)])
    child.wait(PROMPT)

    child.send(b"/model\r")
    end = child.wait(b"model for next turn: openai/gpt-5.6")
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
        "-c", str(config), "-m", "vendor/future-model", "-o", "low",
        "-r", session_id
    ])
    resumed.wait(PROMPT)
    start = len(resumed.buf)
    resumed.send(b"/model\r")
    end = resumed.wait(
        b"model for next turn: vendor/future-model (staged once)", start=start
    )
    resumed.wait(PROMPT, start=end)
    resumed.send(b"ping\r")
    answer_end = resumed.wait(b"pong", start=end)
    resumed.exit_cleanly(answer_end)

    resumed_turns = [event for event in events(session_id)
                     if event["type"] == "turn_started"]
    assert resumed_turns[-1]["data"]["config"]["model"] == "vendor/future-model"
    assert resumed_turns[-1]["data"]["config"]["effort"] == "low"

test_utf8_prompt_cursor_column()
test_steering()
test_split_utf8_steering()
test_armed_fifo()
test_interrupt()
test_multiline_and_paste()
test_resume_pauses_fifo()
test_preferences_and_verbosity()
test_command_name_completion()
test_arbitrary_model_selection()
test_config_and_cli_model_passthrough()
print("pty_active: ok")
