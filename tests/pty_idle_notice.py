#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Idle-state notice: a turn that leaves a paused/blocked goal with nothing
pending prints one terminal line naming the state, so paused no longer reads
as stuck. Interactive terminals only (ui.opened); one-shot `-e` stderr stays
frozen. Usage: pty_idle_notice.py <binary> <workspace> with
SNAJPAGENT_DOTDIR and SNAJPAGENT_TEST_ROOT in the environment (same
convention as test_cli.sh).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pty_active as H


def run_turn(child, prompt, *needles):
    child.send(prompt + b"\r")
    for needle in needles:
        child.wait(needle, timeout=100.0)
    child.wait_idle_prompt(timeout=100.0)


def main():
    with H.Child([], ready=H.DEFAULT_IDLE_PROMPT, term="xterm") as child:
        child.send(b"\r")
        child.drain(0.4)
        set_end = child.send_wait(b"/goal slow goal\r", b"Goal set")
        child.wait(b"working on goal", start=set_end)
        run_turn(child, b"/goal pause", b"Goal paused")
        run_turn(child, b"ping", b"idle: goal paused, awaiting operator")
    with H.Child([], ready=H.DEFAULT_IDLE_PROMPT, term="xterm") as child:
        child.send(b"/goal timer slow blocked goal\r")
        child.wait(b"Goal blocked by model")
        child.wait(b"timer scheduled")
        child.wait(b"idle: goal blocked, timer scheduled")
        child.wait_idle_prompt()
        assert b"idle: goal blocked, timer scheduled" in child.buf
        assert b"idle: goal blocked, awaiting operator" not in child.buf
    print("idle notice: ok")


if __name__ == "__main__":
    main()
