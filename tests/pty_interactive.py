#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import json
import os
import re
import time
from pathlib import Path
from pty_active import Child, DEFAULT_IDLE_PROMPT, DOTDIR

child = Child(["-vvvv"])
buf = child.buf
child.wait_text(DEFAULT_IDLE_PROMPT, timeout=5.0)
blank_start = len(buf)
before_logs = set(Path(DOTDIR, "sessions").glob("*/events.jsonl"))
child.send(b"\r" * 3)
child.drain(0.4)
assert set(Path(DOTDIR, "sessions").glob("*/events.jsonl")) == before_logs
assert bytes(buf[blank_start:]).count(b"\n") >= 3, bytes(buf[blank_start:])
child.send(b"ping\r")
child.wait_text(b"pong", timeout=5.0)
# A prompt redraw can occur while a response is still active.  The durable
# terminal event is the unambiguous point at which /exit is an idle command.
child.wait_text(b"turn_completed synced", timeout=5.0)
terminal_end = buf.find(b"turn_completed synced") + len(b"turn_completed synced")
# The always-visible composer changes active/idle in place. Its leftmost
# unchanged cells and final space need not be emitted again.
def idle_at(start):
    return re.search(rb"(?:^|[\r\n])[^\r\n]*/[^\r\n]* \xe2\x80\xba", buf[start:])

child.wait_pattern(re.compile(rb"(?:^|[\r\n])[^\r\n]*/[^\r\n]* \xe2\x80\xba"),
                   start=terminal_end, timeout=5.0)
child.send(b"slow\r")
child.wait_text(b"working slowly", timeout=5.0)
child.send(b"\x03")
child.wait_text(b"turn interrupted", timeout=5.0)
interrupt_end = buf.find(b"turn interrupted") + len(b"turn interrupted")
while not idle_at(interrupt_end):
    if not child.read_once(5.0):
        raise SystemExit(f"no post-interrupt prompt: {bytes(buf)!r}")
child.send(b"/verbose 4\r")
child.wait_text(b"verbosity: 4", timeout=5.0)
cancel_start = len(buf)
for count in range(1, 5):
    child.send(b"\x03")
    end = time.monotonic() + 5.0
    while bytes(buf[cancel_start:]).count(b"^C\r\n") < count:
        remaining = end - time.monotonic()
        if remaining <= 0:
            raise SystemExit(f"missing Ctrl-C cancellations: {bytes(buf)!r}")
        child.read_once(remaining)
if os.waitpid(child.pid, os.WNOHANG) != (0, 0):
    raise SystemExit(f"Ctrl-C exited the process: {bytes(buf)!r}")
child.send(b"\x03")
child.wait_text(b"You can resume this session", timeout=5.0)
_, status = os.waitpid(child.pid, 0)
child.pid = None
os.close(child.fd)
if os.waitstatus_to_exitcode(status) != 0:
    raise SystemExit(f"explicit exit status {status}: {bytes(buf)!r}")
# Only explicit text starts work; blank Enter never manufactures model input.
logs = list(Path(DOTDIR, "sessions").glob("*/events.jsonl"))
assert not any(json.loads(line).get("type") == "turn_started" and
               json.loads(line)["data"]["text"] == "Continue."
           for path in logs for line in path.read_text().splitlines())
if os.environ.get("TERM") == "dumb" and b"\x1b" in buf:
    raise SystemExit(f"TERM=dumb received ANSI: {bytes(buf)!r}")
