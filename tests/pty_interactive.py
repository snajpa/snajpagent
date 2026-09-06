#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import os
import pty
import select
import sys
import time

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
    while needle not in buf:
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

fresh_prompt = b"    ?% openai/gpt-5.5-2026-04-23/medium \xe2\x80\xba "
accounted_prompt = b"    ?% openai/gpt-5.5-2026-04-23/medium \xe2\x80\xba "
read_until(fresh_prompt)
os.write(fd, b"ping\r")
read_until(b"pong")
# A prompt redraw can occur while a response is still active.  The durable
# terminal event is the unambiguous point at which /exit is an idle command.
read_until(b"turn_completed synced")
terminal_end = buf.find(b"turn_completed synced") + len(b"turn_completed synced")
end = time.monotonic() + 5.0
while accounted_prompt not in buf[terminal_end:]:
    remaining = end - time.monotonic()
    if remaining <= 0:
        raise SystemExit(f"no idle composer: {bytes(buf)!r}")
    ready, _, _ = select.select([fd], [], [], remaining)
    if ready:
        chunk = os.read(fd, 65536)
        if not chunk:
            raise SystemExit(f"unexpected EOF: {bytes(buf)!r}")
        buf.extend(chunk)
os.write(fd, b"slow\r")
read_until(b"working slowly")
os.write(fd, b"\x03")
read_until(b"turn interrupted")
interrupt_end = buf.find(b"turn interrupted") + len(b"turn interrupted")
while accounted_prompt not in buf[interrupt_end:]:
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
if os.environ.get("TERM") == "dumb" and b"\x1b" in buf:
    raise SystemExit(f"TERM=dumb received ANSI: {bytes(buf)!r}")
