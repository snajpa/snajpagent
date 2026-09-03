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
from datetime import datetime, timezone
from pathlib import Path

BINARY = os.path.abspath(sys.argv[1])
WORKSPACE = os.path.abspath(sys.argv[2])
DOTDIR = os.environ["SNAJPAGENT_DOTDIR"]
STATE_ROOT = Path(DOTDIR) / "sessions"
PROMPT = "› ".encode()


class Child:
    def __init__(self, args):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.chdir(WORKSPACE)
            os.execv(BINARY, [BINARY, "-d", DOTDIR, *args])
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
        self.exit_now()

    def exit_now(self):
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


def test_goal_configured_wording_limit():
    config = Path(os.environ["SNAJPAGENT_TEST_ROOT"]) / "config" / \
        "goal-limit.ini"
    config.write_text(
        "[agent]\nmax_goal_prompt_bytes = 4\n",
        encoding="utf-8",
    )
    before = session_ids()
    child = Child(["-c", str(config)])
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
    child.wait(b"goal wording updated by model")
    answer_end = child.wait(b"goal done")
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
    checkpoint_end = child.wait(b"goal checkpoint")
    pong_end = child.wait(b"pong", start=checkpoint_end)
    answer_end = child.wait(b"goal done", start=pong_end)
    child.exit_cleanly(answer_end)
    log = events(new_session(before))
    turns = [item for item in log if item["type"] == "turn_started"]
    assert [item["data"]["input_kind"] for item in turns] == [
        "goal", "queued", "goal"
    ]


def test_goal_user_terminal_commands_and_unlock():
    before = session_ids()
    child = Child([])
    child.wait(PROMPT)
    child.send(b"/goal slow goal\r")
    child.wait(b"working on goal")
    child.send(b"/goal lock\r")
    child.wait(b"goal wording locked against model changes")
    child.send(b"/goal unlock\r")
    child.wait(b"goal wording unlocked for model changes")
    child.send(b"/goal complete\r")
    complete_end = child.wait(b"goal completed by user")
    checkpoint_end = child.wait(b"goal checkpoint", start=complete_end)
    child.wait(PROMPT, start=checkpoint_end)

    child.send(b"/goal slow goal\r")
    child.wait(b"working on goal", start=checkpoint_end)
    child.send(b"/goal cancel\r")
    cancel_end = child.wait(b"goal cancelled", start=checkpoint_end)
    checkpoint_end = child.wait(b"goal checkpoint", start=cancel_end)
    child.exit_cleanly(checkpoint_end)

    log = events(new_session(before))
    completed = one(log, "goal_completed")
    assert completed["data"]["actor"] == "user"
    one(log, "goal_cancelled")
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
    resumed = Child(["-r", session_id])
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
        (b"/go", b"/goal"),
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


def cached_timestamp(cache):
    updated = cache["updated_at_ms"] / 1000
    return datetime.fromtimestamp(updated, timezone.utc).strftime(
        "cache updated: %Y-%m-%dT%H:%M:%SZ"
    ).encode()


def test_model_cache_and_selection():
    cache_path = Path(DOTDIR) / "models.json"
    cache_path.unlink(missing_ok=True)
    codex_home = Path(os.environ["SNAJPAGENT_TEST_ROOT"]) / "codex-home"
    codex_home.mkdir(mode=0o700)
    (codex_home / "models_cache.json").write_text(
        json.dumps(
            {
                "models": [
                    {
                        "slug": model,
                        "supported_reasoning_levels": [
                            {"effort": effort}
                            for effort in (
                                "low", "medium", "high", "xhigh", "max", "ultra"
                            )
                        ],
                        "default_reasoning_level": "medium",
                    }
                    for model in ("gpt-5.6-sol", "gpt-5.6-terra")
                ]
                + [{"slug": "vendor/future-model"}]
            }
        ),
        encoding="utf-8",
    )
    previous_codex_home = os.environ.get("CODEX_HOME")
    os.environ["CODEX_HOME"] = str(codex_home)
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
    child = Child(["-c", str(config)])
    child.wait(PROMPT)

    # A first plain listing imports the local Codex cache without provider work.
    start = len(child.buf)
    child.send(b"/model\r")
    child.wait(b"selected: first / uncached-start / low", start=start)
    child.wait(b"1. first / gpt-5.6-sol / low", start=start)
    child.wait(b"13. first / vendor/future-model / low", start=start)
    cache = json.loads(cache_path.read_text(encoding="utf-8"))
    assert cache_path.stat().st_mode & 0o777 == 0o600
    assert [provider["name"] for provider in cache["providers"]] == ["first"]
    stamp = cached_timestamp(cache)
    end = child.wait(stamp + b"\r\n" + PROMPT, start=start)

    # /model list is a cache-only alias and retains the stored timestamp.
    original = cache_path.read_bytes()
    original_inode = cache_path.stat().st_ino
    child.send(b"/model list\r")
    end = child.wait(stamp + b"\r\n" + PROMPT, start=end)
    assert cache_path.read_bytes() == original
    assert cache_path.stat().st_ino == original_inode

    # Explicit refresh replaces the complete catalog and prints its timestamp.
    child.send(b"/model cache\r")
    child.wait(b"26. second / vendor/future-model / low", start=end)
    refreshed = json.loads(cache_path.read_text(encoding="utf-8"))
    assert refreshed["updated_at_ms"] >= cache["updated_at_ms"]
    assert cache_path.stat().st_ino != original_inode
    refreshed_stamp = cached_timestamp(refreshed)
    end = child.wait(refreshed_stamp + b"\r\n" + PROMPT, start=end)

    # A bare cached model chooses the highest recognized advertised effort.
    child.send(b"/model gpt-5.6-sol\r")
    end = child.wait(
        b"model for next turn: first / gpt-5.6-sol / ultra", start=end
    )
    child.wait(PROMPT, start=end)

    # Uncached identifiers and effort names pass through without local lookup.
    child.send(b"/model definitely-new-model\r")
    end = child.wait(
        b"model for next turn: first / definitely-new-model / ultra", start=end
    )
    child.wait(PROMPT, start=end)
    child.send(b"/model fresh-model / quantum\r")
    end = child.wait(
        b"model for next turn: first / fresh-model / quantum", start=end
    )
    child.wait(PROMPT, start=end)
    child.send(b"/model default / literal-effort\r")
    end = child.wait(
        b"model for next turn: first / default / literal-effort", start=end
    )
    child.wait(PROMPT, start=end)
    child.send(b"/model second / future-new / cosmic\r")
    end = child.wait(
        b"model for next turn: second / future-new / cosmic", start=end
    )
    child.wait(PROMPT, start=end)

    # Both numeric spellings select the exact flattened cached variant.
    child.send(b"/model 7\r")
    end = child.wait(
        b"model for next turn: first / gpt-5.6-terra / low", start=end
    )
    child.wait(PROMPT, start=end)
    child.send(b"/model #26\r")
    end = child.wait(
        b"model for next turn: second / vendor/future-model / low", start=end
    )
    child.wait(PROMPT, start=end)
    child.send(b"/model #18\r")
    end = child.wait(
        b"model for next turn: second / gpt-5.6-sol / max", start=end
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
    assert changes[-1]["data"]["new_model"] == "gpt-5.6-sol"
    assert changes[-1]["data"]["new_effort"] == "max"
    turn = one(log, "turn_started")
    assert turn["data"]["config"]["provider"] == "second"
    assert turn["data"]["config"]["model"] == "gpt-5.6-sol"
    assert turn["data"]["config"]["effort"] == "max"

    # Provider/model/effort selection survives a process restart and resume.
    resumed = Child(["-c", str(config), "-r", session_id])
    resumed.wait(PROMPT)
    resumed.send(b"/status\r")
    status_end = resumed.wait(b"provider: second")
    resumed.wait(b"model: gpt-5.6-sol", start=status_end)
    status_end = resumed.wait(b"effort: max", start=status_end)
    resumed.wait(PROMPT, start=status_end)
    resumed.send(b"ping\r")
    answer_end = resumed.wait(b"pong", start=status_end)
    resumed.exit_cleanly(answer_end)
    turns = [event for event in events(session_id)
             if event["type"] == "turn_started"]
    assert turns[-1]["data"]["config"]["provider"] == "second"
    assert turns[-1]["data"]["config"]["model"] == "gpt-5.6-sol"
    assert turns[-1]["data"]["config"]["effort"] == "max"

    # Any provider failure leaves the previous complete cache untouched.
    complete_cache = cache_path.read_bytes()
    complete_inode = cache_path.stat().st_ino
    os.environ["SNAJPAGENT_FIXTURE_MODEL_FAILURE"] = "second"
    try:
        failing = Child(["-c", str(config)])
        failing.wait(PROMPT)
        failing.send(b"/model cache\r")
        failed_end = failing.wait(
            b"cannot refresh provider second: fixture model discovery failed"
        )
        failing.wait(b"\r\n" + PROMPT, start=failed_end)
        failing.send(b"/exit\r")
        _, status = os.waitpid(failing.pid, 0)
        os.close(failing.fd)
        assert os.waitstatus_to_exitcode(status) == 0
    finally:
        os.environ.pop("SNAJPAGENT_FIXTURE_MODEL_FAILURE", None)
    assert cache_path.read_bytes() == complete_cache
    assert cache_path.stat().st_ino == complete_inode
    if previous_codex_home is None:
        os.environ.pop("CODEX_HOME")
    else:
        os.environ["CODEX_HOME"] = previous_codex_home

def test_config_and_cli_model_passthrough():
    config = Path(os.environ["SNAJPAGENT_TEST_ROOT"]) / "config" / "model-passthrough.ini"
    config.write_text(
        "[agent]\nmodel = openai/gpt-5.6\nreasoning_effort = default\n",
        encoding="utf-8",
    )
    before = session_ids()
    child = Child(["-c", str(config)])
    child.wait(PROMPT)

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
        "-c", str(config), "-m", "vendor/future-model", "-o", "custom-effort",
        "-r", session_id
    ])
    resumed.wait(PROMPT)
    start = len(resumed.buf)
    resumed.send(b"/status\r")
    end = resumed.wait(
        b"model: vendor/future-model (staged once)", start=start
    )
    resumed.wait(PROMPT, start=end)
    resumed.send(b"ping\r")
    answer_end = resumed.wait(b"pong", start=end)
    resumed.exit_cleanly(answer_end)

    resumed_turns = [event for event in events(session_id)
                     if event["type"] == "turn_started"]
    assert resumed_turns[-1]["data"]["config"]["model"] == "vendor/future-model"
    assert resumed_turns[-1]["data"]["config"]["effort"] == "custom-effort"

test_utf8_prompt_cursor_column()
test_steering()
test_split_utf8_steering()
test_armed_fifo()
test_interrupt()
test_multiline_and_paste()
test_resume_pauses_fifo()
test_goal_quoted_reserved_wording()
test_goal_automatic_continuation()
test_goal_configured_wording_limit()
test_goal_model_rewrite_and_lock()
test_goal_pause_resume_and_queue_priority()
test_goal_user_terminal_commands_and_unlock()
test_goal_refusal_failure_block_and_restart_pause()
test_preferences_and_verbosity()
test_command_name_completion()
test_model_cache_and_selection()
test_config_and_cli_model_passthrough()
print("pty_active: ok")
