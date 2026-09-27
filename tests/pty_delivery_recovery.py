#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Buffered citation fragments survive renderer and UI delivery boundaries.

Both a large unrecognized citation and a small recognized citation with its
terminator delivered separately must finish without retries or a false input
overflow. The next ordinary prompt must still work.
Usage: pty_delivery_recovery.py <binary> <workspace>
with SNAJPAGENT_DOTDIR and SNAJPAGENT_TEST_ROOT in the environment (same
convention as test_cli.sh).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pty_active as H


def main():
    with H.Child([], ready=H.DEFAULT_IDLE_PROMPT, term="xterm") as child:
        end = child.send_wait(b"cite_split\r", b"yyyyyyyy", timeout=20.0)
        child.wait_idle_prompt(start=end)
        end = child.send_wait(b"cite_tail\r", b"[cite: turn 2]", timeout=20.0)
        child.wait_idle_prompt(start=end)
        end = child.send_wait(b"ping\r", b"pong", timeout=10.0)
        child.exit_cleanly(end)
        assert b"public output delivery failed" not in child.buf
        assert b"submission exceeds" not in child.buf
        assert b"prompt exceeds" not in child.buf
        log = H.events(child.session_id())
        assert not [e for e in log if e["type"] in ("response_failed", "turn_recovery")]
        assert len([e for e in log if e["type"] == "response_completed"]) == 3
    print("fragmented citation delivery: ok")


if __name__ == "__main__":
    main()
