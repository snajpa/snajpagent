#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Removed archiving leaves old sessions visible and resumable without effects."""
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import tmux_terminal as harness


def append_legacy_events(path, kinds):
    """Append obsolete ASCII records to a stopped, disposable fixture journal."""
    previous = json.loads(path.read_bytes().splitlines()[-1])
    with path.open("ab") as out:
        for kind in kinds:
            data = {"control": 8} if kind.startswith("control_") else {"origin": "user"}
            event = dict(data=data, prev_sha256=previous["event_sha256"],
                         seq=previous["seq"] + 1, session_id=previous["session_id"],
                         time_ms=previous["time_ms"] + 1, type=kind, v=previous["v"])
            if "checkpoint_offset" in previous:
                event["checkpoint_offset"] = previous["checkpoint_offset"]
            def encode():
                return json.dumps(event, sort_keys=True, separators=(",", ":")).encode()
            event["event_sha256"] = hashlib.sha256(encode()).hexdigest()
            out.write(encode() + b"\n")
            previous = event


def check_legacy_sessions(binary):
    provider = harness.FakeResponses()

    def respond(handler, request, sequence):
        provider.reply(handler, provider.response_body(sequence, "saved").encode())

    provider.runtime_handler = respond
    try:
        with tempfile.TemporaryDirectory(prefix="snag-legacy-session-") as tmp:
            root = Path(tmp).resolve()
            config = root / "config.ini"
            harness.write_irc_config(config, provider.port, "host-model")
            state = root / "state"
            env = {**os.environ, "HOME": str(root), "SNAJPAGENT_IRC_UI_KEY": "irc-ui-secret"}
            prefix = [str(binary), "--config", str(config), "--dotdir", str(state)]

            def run(*args):
                result = subprocess.run(prefix + list(args), cwd=root, env=env,
                                        capture_output=True, timeout=30)
                assert result.returncode == 0, (args, result.stdout, result.stderr)
                return result

            request = ["control_requested"]
            started = request + ["control_started"]
            effect = started + ["session_archived"]
            cases = [["session_archived"], ["session_archived", "session_unarchived"],
                     request, started, effect, effect + ["control_finished"]]
            seen = set()
            for index, kinds in enumerate(cases):
                name = f"legacy-{index}"
                run("-N", name, "-e", "--", "ping")
                journals = set((state / "sessions").glob("*/events.jsonl"))
                path, = journals - seen
                seen = journals
                append_legacy_events(path, kinds)
                original = path.read_bytes()
                listed = run("-l").stdout.decode().splitlines()
                assert listed[0].split("\t") == [
                    "SESSION", "NAME", "MODEL", "TURNS", "PROCESS", "FIRST PROMPT"]
                own = next(line.split("\t") for line in listed[1:]
                           if line.startswith(path.parent.name[:8] + "\t"))
                assert own[1] == name and own[4] == "stored", own
                assert path.read_bytes() == original, "listing changed legacy history"
                # --last must select even a formerly archived session.
                run("--resume", "--last", "-e", "--", "ping")
                suffix = path.read_bytes()[len(original):]
                new = [json.loads(line) for line in suffix.splitlines()]
                assert any(e["type"] == "turn_completed" for e in new), new
                assert not any(e["type"] in ("session_archived", "session_unarchived",
                                            "session_delete_requested") for e in new), new
                if "control_requested" in kinds:
                    all_events = [json.loads(line) for line in path.read_bytes().splitlines()]
                    assert sum(e["type"] == "control_finished" and e["data"]["control"] == 8
                               for e in all_events) == 1
                run("--resume", "-N", name, "-e", "--", "ping")
                assert path.read_bytes().startswith(original), "resume rewrote legacy history"
            assert not provider.failure, provider.failure
            print("legacy archived sessions: list, last, name, history and pending controls: ok",
                  flush=True)
    finally:
        provider.close()


if __name__ == "__main__":
    check_legacy_sessions(Path(sys.argv[1]).resolve())
