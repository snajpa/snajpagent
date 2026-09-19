#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Terminal-only wait notices: provider/tool waits announce what/bound.

Interactive terminals show wait lines from verbosity 1 upward. Verbosity 0 and
one-shot `-e` stay quiet, so this runs under a pty like the other pty_*.py suites.
Turns synchronize on the journal-backed `turn_completed synced` event line
(idle-marker repaints can flicker mid-turn). Usage:
pty_wait_notices.py <binary> <workspace> with SNAJPAGENT_DOTDIR and
SNAJPAGENT_TEST_ROOT in the environment (same convention as test_cli.sh).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pty_active as H


def run_turn(child, prompt, answer, *needles):
    start = len(child.buf)
    child.send(prompt + b"\r")
    for needle in needles:
        child.wait(needle, timeout=100.0)
    child.wait(answer, start=start, timeout=100.0)
    return bytes(child.buf[start:])


def main():
    with H.Child([], ready=H.DEFAULT_IDLE_PROMPT, term="xterm") as child:
        quiet = run_turn(child, b"tool_only", b"tool complete")
        assert b"waiting for provider response" not in quiet
        assert b"tool exec_command running" not in quiet
    with H.Child(["-v"], ready=H.DEFAULT_IDLE_PROMPT, term="xterm") as child:
        child.send(b"\r")
        child.drain(0.4)
        visible = run_turn(child, b"ping", b"pong", b"waiting for provider response (model ")
        assert b"tool " not in visible
        run_turn(child, b"tool_only", b"tool complete",
                 b"waiting for provider response (model ",
                 b"tool exec_command running (wait cap ")
    print("wait notices: ok")


if __name__ == "__main__":
    main()
