#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Consecutive failed compactions are bounded; new input clears the bound.

The release-session report (782e7a68) showed a compaction storm: the provider
kept aborting the summary request and every turn retry re-ran it (13 attempts
over 1.5 hours). A session now stops attempting after eight consecutive
failures and reports it; a completed compaction, fresh operator input or a
manual /compact clears the count. The fixture fails native compaction for the
`compact_fail` prompt, and a proactive threshold makes the pre-response path
run it.

Usage: pty_compaction_bound.py <binary> <workspace> with SNAJPAGENT_DOTDIR and
SNAJPAGENT_TEST_ROOT in the environment (same convention as test_cli.sh).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pty_active as H


def main():
    config = H.write_config("compaction-bound.ini",
        "[agent]\nmax_turn_retries = 12\n[provider openai]\n"
        "exact_token_count = true\nnative_compaction = true\n"
        "auto_compact_input_tokens = 1\n")

    # Seed one completed turn so a compaction source exists.
    with H.Child([], ready=H.DEFAULT_IDLE_PROMPT, term="xterm") as seed:
        end = seed.send_wait(b"ping\r", b"pong")
        seed.exit_cleanly(end)
        session = seed.session_id()

    with H.Child(["--config", str(config), "--resume", session],
                 ready=H.DEFAULT_IDLE_PROMPT, term="xterm") as child:
        # One turn retries until the bound stops the storm, then fails once.
        child.send(b"compact_fail\r")
        child.wait_text(b"compaction failed 8 times in a row", timeout=180.0)
        child.wait_text(b"turn failed; try /retry to continue", timeout=30.0)
        # Fresh operator input clears the bound, and the next compaction works.
        after = len(child.buf)
        child.send(b"ping\r")
        child.wait(b"pong", start=after, timeout=30.0)
        child.exit_cleanly(len(child.buf) - len(b"pong"))

    log = H.events(session)
    started = [event for event in log if event["type"] == "compaction_started"]
    completed = [event for event in log if event["type"] == "compaction_completed"]
    # Eight bounded failures, then at least one attempt after new input.
    assert len(started) >= 9, [event["data"].get("compact_id") for event in started]
    assert completed, "compaction did not complete after the bound was cleared"
    print("compaction failure bound: ok")


if __name__ == "__main__":
    main()
