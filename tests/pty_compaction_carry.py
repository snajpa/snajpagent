#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""A summary's coverage survives a binding switch (portable text carry).

The 6511acd6 history showed a single model switch (deepseek-flash -> gpt-6-astra)
invalidating a compaction that covered 41,900 of ~43k events, after which the
client re-walked the whole archive for tens of minutes. The compaction builder
now keeps the coverage when the retained summary belongs to another scope and
leads the new source with the summary's text, so a switch compacts the uncovered
tail instead of the archive.

Observable here: run one turn under model A (a proactive threshold makes it
compact), then the same session under a different model in the config - a
different continuation scope - and assert the next compaction's source_seq is
beyond the first one's, i.e. coverage survived, instead of restarting at the
oldest history.

Usage: pty_compaction_carry.py <binary> <workspace> with SNAJPAGENT_DOTDIR and
SNAJPAGENT_TEST_ROOT in the environment (same convention as test_cli.sh).
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pty_active as H


def run_turn(config, session=None):
    args = ["--config", str(config)]
    if session:
        args += ["--resume", session]
    # A proactive threshold compacts on the first prompt, so the idle-prompt
    # ready condition never settles; the banner is the stable signal.
    with H.Child(args, ready=b"snajpagent 0.99", term="xterm") as child:
        child.send_wait(b"ping\r", b"pong")
        if session is None:
            session = child.session_id()
        # A proactive threshold compacts every turn; let that finish so the next
        # run does not start while the previous one is still compacting.
        try:
            child.wait_text(b"Compacted", timeout=120.0)
        except AssertionError:
            pass
        child.exit_cleanly(len(child.buf))
    return session


def last_source_seq(session):
    seqs = [event["data"].get("source_seq") for event in H.events(session)
            if event["type"] == "compaction_started"]
    return seqs[-1] if seqs else 0


def main():
    first = H.write_config("compaction-carry-a.ini",
        "[provider openai]\nexact_token_count = true\nnative_compaction = true\n"
        "auto_compact_input_tokens = 1\n")
    session = run_turn(first)
    baseline = last_source_seq(session)
    assert baseline, "first run did not compact"

    # Same session, different model in the config: the retained summary now
    # belongs to another continuation scope.
    second = H.write_config("compaction-carry-b.ini",
        "[provider openai]\nmodel = gpt-transport-alternate\n"
        "exact_token_count = true\nnative_compaction = true\n"
        "auto_compact_input_tokens = 1\n")
    run_turn(second, session)
    carried = last_source_seq(session)
    assert carried >= baseline, (baseline, carried)
    print("compaction carry across a binding switch: ok")


if __name__ == "__main__":
    main()
