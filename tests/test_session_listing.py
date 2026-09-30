#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Session tables keep saved text on one bounded, terminal-safe row."""
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import tmux_terminal as harness


def check_listing(binary):
    provider = harness.FakeResponses()

    def respond(handler, request, sequence):
        provider.reply(handler, provider.response_body(sequence, "saved").encode())

    provider.runtime_handler = respond
    try:
        with tempfile.TemporaryDirectory(prefix="snag-list-") as tmp:
            root = Path(tmp).resolve()
            config = root / "config.ini"
            harness.write_irc_config(config, provider.port, "host-model")
            state = root / "state"
            env = {**os.environ, "HOME": str(root), "SNAJPAGENT_IRC_UI_KEY": "irc-ui-secret"}
            prefix = [str(binary), "--config", str(config), "--dotdir", str(state)]
            cases = [
                ("IRC history", "[IRC endpoint=localhost:6667 room=#lab event=history_ready]\n" +
                 "[IRC update id=fixture:1 event=join sender=peer]\n" * 30 + "replayed"),
                ("multiline", "first line\nsecond\tcolumn\rreturn\vvertical\fform feed"),
                ("controls", "escape\x1b[31m red\x1b[0m\x07 bell\u009b31m\u2028line\u2029paragraph"),
                ("Unicode", "a" * 79 + "界😀žluťoučký"),
                ("large", "ž" * 40000),
                ("name\u009b\u2028control", "ordinary prompt"),
            ]
            journals = {}
            for name, prompt in cases:
                result = subprocess.run(prefix + ["-N", name, "-e", "--", prompt],
                                        cwd=root, env=env, capture_output=True, timeout=20)
                assert result.returncode == 0, result.stderr
                paths = set((state / "sessions").glob("*/events.jsonl")) - set(journals)
                assert len(paths) == 1, paths
                journal = paths.pop()
                events = [json.loads(line) for line in journal.read_text().split("\n") if line]
                assert next(e["data"]["text"] for e in events if e["type"] == "turn_started") == prompt
                journals[journal] = hashlib.sha256(journal.read_bytes()).hexdigest()
                result = subprocess.run(prefix + ["-l"], cwd=root, env=env,
                                        capture_output=True, timeout=10)
                assert result.returncode == 0, result.stderr
                assert not result.stderr, result.stderr
                lines = result.stdout.decode().splitlines()
                assert lines[0].split("\t") == [
                    "SESSION", "NAME", "MODEL", "TURNS", "STATUS", "PROCESS", "FIRST PROMPT"]
                assert len(lines) == len(journals) + 1, (name, len(lines), len(journals))
                for line in lines[1:]:
                    fields = line.split("\t")
                    assert len(fields) == 7, fields
                    for field in (fields[1], fields[2], fields[6]):
                        assert len(field) <= 81, field
                        assert not any(ord(c) < 32 or 127 <= ord(c) <= 159 or
                                       c in "\u2028\u2029" for c in field), repr(field)
                own = next(line.split("\t") for line in lines if line.startswith(journal.parent.name[:8]))
                if name == "multiline":
                    assert own[6] == "first line second column return vertical form feed", own
                if name == "Unicode":
                    assert own[6] == "a" * 79 + "界…", own
                if name in ("IRC history", "large"):
                    assert own[6].endswith("…"), own
                assert all(hashlib.sha256(path.read_bytes()).hexdigest() == digest
                           for path, digest in journals.items()), "listing changed saved history"
            assert not provider.failure, provider.failure
            print("session listing: multiline IRC, controls, long Unicode and read-only rows: ok", flush=True)
    finally:
        provider.close()


if __name__ == "__main__":
    check_listing(Path(sys.argv[1]).resolve())
