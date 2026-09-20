#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""A failed public-output delivery is an input-shaped condition, not a broken
terminal.

The presenter used to latch terminal input closed on any delivery error, so a
session whose public output tripped the render bound stopped reading keys
entirely - nothing echoed and no prompt ran again - while the engine kept
running. The fixture's cite_split answer carries more turn references than the
presenter rewrites and crosses the fixture's emission split, so one delivery
fails with EOVERFLOW exactly like the release session did; the next prompt must
still reach the composer. Usage: pty_delivery_recovery.py <binary> <workspace>
with SNAJPAGENT_DOTDIR and SNAJPAGENT_TEST_ROOT in the environment (same
convention as test_cli.sh).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pty_active as H


def main():
    with H.Child([], ready=H.DEFAULT_IDLE_PROMPT, term="xterm") as child:
        child.send_wait(b"cite_split\r", b"public output delivery failed", timeout=20.0)
        # The failed delivery must not close terminal input: the next prompt
        # still has to be consumed and echoed. A latched close ate every
        # keystroke silently, so this text never appeared at all.
        after = len(child.buf)
        child.send(b"ping\r")
        child.wait(b"ping", start=after, timeout=10.0)
    print("delivery recovery: ok")


if __name__ == "__main__":
    main()
