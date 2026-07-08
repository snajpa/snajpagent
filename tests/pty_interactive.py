#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import os
import pty
import select
import sys
import time

binary = sys.argv[1]
workspace = sys.argv[2]
pid, fd = pty.fork()
if pid == 0:
    os.chdir(workspace)
    os.execv(binary, [binary, "-vvvv"])

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

read_until(b"\xe2\x80\xba ")
os.write(fd, b"ping\r")
read_until(b"pong")
# A prompt redraw can occur while a response is still active.  The durable
# terminal event is the unambiguous point at which /exit is an idle command.
read_until(b"turn_completed synced")
terminal_end = buf.find(b"turn_completed synced") + len(b"turn_completed synced")
end = time.monotonic() + 5.0
while b"\xe2\x80\xba " not in buf[terminal_end:]:
    remaining = end - time.monotonic()
    if remaining <= 0:
        raise SystemExit(f"no idle composer: {bytes(buf)!r}")
    ready, _, _ = select.select([fd], [], [], remaining)
    if ready:
        chunk = os.read(fd, 65536)
        if not chunk:
            raise SystemExit(f"unexpected EOF: {bytes(buf)!r}")
        buf.extend(chunk)
os.write(fd, b"/exit\r")
_, status = os.waitpid(pid, 0)
if status != 0:
    raise SystemExit(f"interactive exit status {status}: {bytes(buf)!r}")
if os.environ.get("TERM") == "dumb" and b"\x1b" in buf:
    raise SystemExit(f"TERM=dumb received ANSI: {bytes(buf)!r}")
