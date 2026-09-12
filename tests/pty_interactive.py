#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import json
import os
import base64
import time
from pathlib import Path
from pty_active import Child, DEFAULT_IDLE_PROMPT, DOTDIR, WORKSPACE

child = Child(["-vvvv"])
buf = child.buf
child.wait_text(DEFAULT_IDLE_PROMPT, timeout=5.0)
blank_start = len(buf)
before_logs = set(Path(DOTDIR, "sessions").glob("*/events.jsonl"))
child.send(b"\r" * 3)
child.drain(0.4)
assert set(Path(DOTDIR, "sessions").glob("*/events.jsonl")) == before_logs
assert bytes(buf[blank_start:]).count(b"\n") >= 3, bytes(buf[blank_start:])
# Exercise attachment staging/removal and durable submission with the existing
# terminal fixture, including paths containing spaces. No provider perception
# is claimed by this fixture.
image = Path(WORKSPACE) / "attachment test.png"
image.write_bytes(base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aKz8AAAAASUVORK5CYII="))
child.send(("/attach " + str(image) + "\r").encode())
child.wait_text(b"1 unsent attachment(s)")
child.send(b"/detach all\r")
child.wait_text(b"0 unsent attachment(s)")
buf.clear()
child.send(("/attach " + str(image) + "\r").encode())
child.wait_text(b"1 unsent attachment(s)")
child.send(b"ping\r")
child.wait_text(b"pong")
# A prompt redraw can occur while a response is still active.  The durable
# terminal event is the unambiguous point at which /exit is an idle command.
child.wait_text(b"turn_completed synced")
events = [json.loads(line) for path in Path(DOTDIR).glob("sessions/*/events.jsonl")
          for line in path.read_text().splitlines()]
turns = [event["data"] for event in events if event["type"] == "turn_started"
         and event["data"].get("text") == "ping" and event["data"].get("content")]
assert turns, "attachment was not journalled with the ping turn"
pending = [event["data"] for event in events if event["type"] == "input_received"
           and event["data"].get("text") == "ping"]
assert pending and pending[-1]["content"] == turns[-1]["content"]
part = turns[-1]["content"][0]
assert part["type"] == "input_image" and part["asset"]["mime_type"] == "image/png"
assert part["source"]["mime_type"] == "image/png" and "frame 0 only" in part["note"]
assert "image_url" not in part, "base64 should not be duplicated in the journal"
image.unlink()
terminal_end = buf.find(b"turn_completed synced") + len(b"turn_completed synced")
# The always-visible composer changes active/idle in place. Its leftmost
# unchanged cells and final space need not be emitted again.
child.wait_idle_prompt(start=terminal_end, timeout=5.0)
# Blank Enter stays local after attachment admission and completion too.
blank_start = len(buf)
before_events = [path.read_bytes() for path in sorted(Path(DOTDIR, "sessions").glob("*/events.jsonl"))]
child.send(b"\r")
child.drain(0.4)
assert [path.read_bytes() for path in sorted(Path(DOTDIR, "sessions").glob("*/events.jsonl"))] == before_events
assert b"fixture answer" not in buf[blank_start:]
child.send(b"slow\r")
child.wait_text(b"working slowly", timeout=5.0)
child.send(b"\x03")
child.wait_text(b"turn interrupted", timeout=5.0)
interrupt_end = buf.find(b"turn interrupted") + len(b"turn interrupted")
child.wait_idle_prompt(start=interrupt_end, timeout=5.0)
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
