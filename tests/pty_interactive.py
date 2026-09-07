#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import json
import os
import pty
import re
import select
import sys
import time
from pathlib import Path

binary = sys.argv[1]
workspace = sys.argv[2]
dotdir = os.environ["SNAJPAGENT_DOTDIR"]
pid, fd = pty.fork()
if pid == 0:
    os.chdir(workspace)
    os.execv(binary, [binary, "--dotdir", dotdir, "-vvvv"])

buf = bytearray()
def read_until(needle: bytes, timeout: float = 5.0) -> None:
    end = time.monotonic() + timeout
    # Screen correctness is asserted by tmux; allow only the live gap detour.
    gap = rb"(?:(?:\r{1,2}\n){1,2}(?:[^\n]*?\r\x1b\[2K(?:\x1b\[1A\r\x1b\[2K)*)?\r\x1b\[[12]A(?:\x1b\[\d+C)?)*"
    pattern = re.compile(gap.join(re.escape(bytes([c])) for c in needle))
    while pattern.search(buf) is None:
        remaining = end - time.monotonic()
        if remaining <= 0:
            raise SystemExit(f"timeout waiting for {needle!r}; got {bytes(buf)!r}")
        ready, _, _ = select.select([fd], [], [], remaining)
        if not ready:
            continue
        chunk = os.read(fd, 65536)
        if not chunk:
            raise SystemExit(f"unexpected EOF; got {bytes(buf)!r}")
        buf.extend(chunk)

fresh_prompt = b" openai/gpt-5.5-2026-04-23/medium   0% \xe2\x80\xba "
accounted_prompt = b" openai/gpt-5.5-2026-04-23/medium   ?% \xe2\x80\xba "
read_until(fresh_prompt)
os.write(fd, b"\r")
read_until(b"fixture answer")
# A prompt redraw can occur while a response is still active.  The durable
# terminal event is the unambiguous point at which /exit is an idle command.
read_until(b"turn_completed synced")
terminal_end = buf.find(b"turn_completed synced") + len(b"turn_completed synced")
# The always-visible composer changes active/idle in place. Its leftmost
# unchanged cells and final space need not be emitted again.
def idle_at(start):
    return re.search(rb"(?:^|[\r\n])[^\r\n]*/[^\r\n]* \xe2\x80\xba", buf[start:])

end = time.monotonic() + 5.0
while not idle_at(terminal_end):
    remaining = end - time.monotonic()
    if remaining <= 0:
        raise SystemExit(f"no idle composer: {bytes(buf)!r}")
    ready, _, _ = select.select([fd], [], [], remaining)
    if ready:
        buf.extend(os.read(fd, 65536))
os.write(fd, b"slow\r")
read_until(b"working slowly")
os.write(fd, b"\x03")
read_until(b"turn interrupted")
interrupt_end = buf.find(b"turn interrupted") + len(b"turn interrupted")
while not idle_at(interrupt_end):
    ready, _, _ = select.select([fd], [], [], 5.0)
    if not ready:
        raise SystemExit(f"no post-interrupt prompt: {bytes(buf)!r}")
    buf.extend(os.read(fd, 65536))
os.write(fd, b"/verbose 4\r")
read_until(b"verbosity: 4")
cancel_start = len(buf)
for count in range(1, 5):
    os.write(fd, b"\x03")
    end = time.monotonic() + 5.0
    while bytes(buf[cancel_start:]).count(b"^C\r\n") < count:
        remaining = end - time.monotonic()
        if remaining <= 0:
            raise SystemExit(f"missing Ctrl-C cancellations: {bytes(buf)!r}")
        ready, _, _ = select.select([fd], [], [], remaining)
        if ready:
            buf.extend(os.read(fd, 65536))
if os.waitpid(pid, os.WNOHANG) != (0, 0):
    raise SystemExit(f"Ctrl-C exited the process: {bytes(buf)!r}")
os.write(fd, b"\x03")
read_until(b"You can resume this session")
_, status = os.waitpid(pid, 0)
if os.waitstatus_to_exitcode(status) != 0:
    raise SystemExit(f"explicit exit status {status}: {bytes(buf)!r}")
# Empty Enter must admit a direct continuation and preserve ordinary turn history.
logs = list(Path(dotdir, "sessions").glob("*/events.jsonl"))
assert any(json.loads(line).get("type") == "turn_started" and
           json.loads(line)["data"]["text"] == "Continue."
           for path in logs for line in path.read_text().splitlines())
if os.environ.get("TERM") == "dumb" and b"\x1b" in buf:
    raise SystemExit(f"TERM=dumb received ANSI: {bytes(buf)!r}")
