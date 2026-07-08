#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import errno
import json
import os
import pty
import select
import signal
import sys
import time
from pathlib import Path

binary = os.path.abspath(sys.argv[1])
workspace = os.path.abspath(sys.argv[2])
state_root = Path(os.environ["XDG_STATE_HOME"]) / "snajpagent" / "sessions"
prompt = "› ".encode()
text = b"suspend draft"


def session_ids():
    if not state_root.exists():
        return set()
    return {entry.name for entry in state_root.iterdir() if entry.is_dir()}


def events(session_id):
    with (state_root / session_id / "events.jsonl").open(encoding="utf-8") as source:
        return [json.loads(line) for line in source]


before = session_ids()
pid, fd = pty.fork()
if pid == 0:
    os.chdir(workspace)
    os.execv(binary, [binary, "-vvvv"])

buf = bytearray()


def read_once(timeout):
    ready, _, _ = select.select([fd], [], [], timeout)
    if not ready:
        return False
    try:
        chunk = os.read(fd, 65536)
    except OSError as exc:
        if exc.errno == errno.EIO:
            return False
        raise
    if chunk:
        buf.extend(chunk)
        return True
    return False


def wait(needle, start=0, timeout=8.0):
    end = time.monotonic() + timeout
    while needle not in buf[start:]:
        remaining = end - time.monotonic()
        if remaining <= 0 or not read_once(remaining):
            raise AssertionError(f"timeout waiting for {needle!r}; got {bytes(buf)!r}")
    return buf.find(needle, start) + len(needle)


def wait_stopped(timeout=8.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        got_pid, status = os.waitpid(pid, os.WUNTRACED | os.WNOHANG)
        if got_pid == pid:
            if not os.WIFSTOPPED(status):
                raise AssertionError(f"child did not stop; status={status}; buf={bytes(buf)!r}")
            return
        read_once(0.05)
    raise AssertionError(f"timeout waiting for suspended child; got {bytes(buf)!r}")


try:
    wait(prompt)
    os.write(fd, text)
    typed_end = wait(text)
    os.write(fd, b"\x1a")
    wait_stopped()
    resume_start = len(buf)
    os.kill(pid, signal.SIGCONT)
    wait(prompt, start=resume_start)
    wait(text, start=resume_start)
    os.write(fd, b"\r")
    answer_end = wait(b"fixture answer", start=typed_end)
    terminal_end = wait(b"turn_completed synced", start=answer_end)
    wait(b"\r" + prompt, start=terminal_end)
    os.write(fd, b"/exit\r")
    _, status = os.waitpid(pid, 0)
    code = os.waitstatus_to_exitcode(status)
    if code != 0:
        raise AssertionError(f"exit status {code}; got {bytes(buf)!r}")
finally:
    try:
        os.close(fd)
    except OSError:
        pass

created = session_ids() - before
if len(created) != 1:
    raise AssertionError(f"expected one new session, got {sorted(created)!r}")
turns = [event for event in events(created.pop()) if event["type"] == "turn_started"]
if len(turns) != 1 or turns[0]["data"]["text"] != text.decode():
    raise AssertionError(f"suspend changed submitted draft: {turns!r}")
print("pty_suspend: ok")
