#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import json
from pty_active import Child, DEFAULT_IDLE_PROMPT, STATE_ROOT


def run_case(term, cols, expect_ansi, expected_text):
    with Child(["-vvvv"], term=term, cols=cols) as child:
        child.wait_text(DEFAULT_IDLE_PROMPT)
        child.send(expected_text.encode() + b"\r")
        answer_end = child.wait_text(b"fixture answer")
        terminal_end = child.wait_text(b"turn_completed synced", start=answer_end)
        child.wait_idle_prompt(start=terminal_end)
        child.send(b"/exit\r")
        code = child.reap()
        if code != 0:
            raise AssertionError(f"{term}/{cols}: exit status {code}; got {bytes(child.buf)!r}")
    buf = child.buf
    has_ansi = b"\x1b" in buf
    if expect_ansi and not has_ansi:
        raise AssertionError(f"{term}/{cols}: expected terminal control output; got {bytes(buf)!r}")
    if not expect_ansi and has_ansi:
        raise AssertionError(f"{term}/{cols}: unexpected ANSI/control output; got {bytes(buf)!r}")

    with (STATE_ROOT / child.session_id() / "events.jsonl").open(encoding="utf-8") as source:
        turns = [event for event in map(json.loads, source) if event["type"] == "turn_started"]
    if len(turns) != 1 or turns[0]["data"]["text"] != expected_text:
        raise AssertionError(f"{term}/{cols}: submitted draft mismatch: {turns!r}")


run_case("xterm", 80, True, "matrix xterm")
run_case("xterm-256color", 100, True, "matrix 256")
run_case("vt100", 80, True, "matrix vt100")
run_case("dumb", 80, False, "matrix dumb")
run_case("xterm", 10, False, "matrix narrow")
print("pty_terminal_matrix: ok")
