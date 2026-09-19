#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Terminal-only wait notices: provider/tool waits announce what/bound.

Interactive terminals (ui.opened) get one-shot wait lines; one-shot `-e`
stderr stays frozen, so this runs under a pty like the other pty_*.py suites.
Turns synchronize on the journal-backed `turn_completed synced` event line
(idle-marker repaints can flicker mid-turn). Usage:
pty_wait_notices.py <binary> <workspace> with SNAJPAGENT_DOTDIR and
SNAJPAGENT_TEST_ROOT in the environment (same convention as test_cli.sh).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pty_active as H


def run_turn(child, prompt, *needles):
    child.send(prompt + b"\r")
    for needle in needles:
        child.wait(needle, timeout=100.0)
    child.wait_text(b"turn_completed synced", timeout=100.0)


def main():
    with H.Child(["-vvvv"], ready=H.DEFAULT_IDLE_PROMPT, term="xterm") as child:
        child.send(b"\r")
        child.drain(0.4)
        run_turn(child, b"ping", b"waiting for provider response (model ")
        run_turn(child, b"tool_only",
                 b"waiting for provider response (model ",
                 b"tool exec_command running (wait cap ")
    print("wait notices: ok")


if __name__ == "__main__":
    main()
