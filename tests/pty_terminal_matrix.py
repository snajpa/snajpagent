#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import errno
import json
import os
import pty
import select
import struct
import sys
import termios
import time
from pathlib import Path

binary = os.path.abspath(sys.argv[1])
workspace = os.path.abspath(sys.argv[2])
state_root = Path(os.environ["XDG_STATE_HOME"]) / "snajpagent" / "sessions"
prompt = "› ".encode()


def session_ids():
    if not state_root.exists():
        return set()
    return {entry.name for entry in state_root.iterdir() if entry.is_dir()}


def read_events(session_id):
    with (state_root / session_id / "events.jsonl").open(encoding="utf-8") as source:
        return [json.loads(line) for line in source]


def set_winsize(fd, rows, cols):
    fcntl_ioctl = __import__("fcntl").ioctl
    fcntl_ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def run_case(term, cols, expect_ansi, expected_text):
    before = session_ids()
    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(workspace)
        os.environ["TERM"] = term
        set_winsize(0, 24, cols)
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
                raise AssertionError(
                    f"{term}/{cols}: timeout waiting for {needle!r}; got {bytes(buf)!r}"
                )
        return buf.find(needle, start) + len(needle)

    def wait_idle_prompt(start=0, timeout=8.0):
        end = time.monotonic() + timeout
        needles = (b"\r" + prompt, b"\n" + prompt)
        while not any(needle in buf[start:] for needle in needles):
            remaining = end - time.monotonic()
            if remaining <= 0 or not read_once(remaining):
                raise AssertionError(
                    f"{term}/{cols}: timeout waiting for idle prompt; got {bytes(buf)!r}"
                )
        positions = [buf.find(needle, start) + len(needle)
                     for needle in needles if needle in buf[start:]]
        return min(positions)

    def wait_child(timeout=8.0):
        end = time.monotonic() + timeout
        while True:
            got, status = os.waitpid(pid, os.WNOHANG)
            if got == pid:
                return status
            remaining = end - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f"{term}/{cols}: child did not exit; got {bytes(buf)!r}")
            read_once(min(0.05, remaining))

    try:
        wait(prompt)
        os.write(fd, expected_text.encode() + b"\r")
        answer_end = wait(b"fixture answer")
        terminal_end = wait(b"turn_completed synced", start=answer_end)
        wait_idle_prompt(start=terminal_end)
        os.write(fd, b"/exit\r")
        status = wait_child()
        while read_once(0.05):
            pass
        code = os.waitstatus_to_exitcode(status)
        if code != 0:
            raise AssertionError(f"{term}/{cols}: exit status {code}; got {bytes(buf)!r}")
    except Exception:
        try:
            os.kill(pid, 9)
        except OSError:
            pass
        try:
            os.waitpid(pid, 0)
        except OSError:
            pass
        raise
    finally:
        try:
            os.close(fd)
        except OSError:
            pass

    has_ansi = b"\x1b" in buf
    if expect_ansi and not has_ansi:
        raise AssertionError(f"{term}/{cols}: expected terminal control output; got {bytes(buf)!r}")
    if not expect_ansi and has_ansi:
        raise AssertionError(f"{term}/{cols}: unexpected ANSI/control output; got {bytes(buf)!r}")

    created = session_ids() - before
    if len(created) != 1:
        raise AssertionError(f"{term}/{cols}: expected one session, got {sorted(created)!r}")
    turns = [event for event in read_events(created.pop()) if event["type"] == "turn_started"]
    if len(turns) != 1 or turns[0]["data"]["text"] != expected_text:
        raise AssertionError(f"{term}/{cols}: submitted draft mismatch: {turns!r}")


run_case("xterm", 80, True, "matrix xterm")
run_case("xterm-256color", 100, True, "matrix 256")
run_case("vt100", 80, True, "matrix vt100")
run_case("dumb", 80, False, "matrix dumb")
run_case("xterm", 10, False, "matrix narrow")
print("pty_terminal_matrix: ok")
