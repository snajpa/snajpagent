#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Trailing transport heartbeats preserve completed turns without recovery."""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import tmux_terminal as harness


def check_keepalives(binary):
    provider = harness.FakeResponses()
    suffix = ""
    text = "completed answer retained once"

    def respond(handler, request, sequence):
        body = provider.response_body(sequence, text) + suffix
        provider.reply(handler, body.encode())

    provider.runtime_handler = respond
    try:
        with tempfile.TemporaryDirectory(prefix="snag-response-keepalive-") as tmp:
            root = Path(tmp).resolve()
            config = root / "config.ini"
            harness.write_irc_config(config, provider.port, "host-model")
            config.write_text(config.read_text().replace("[agent]\n", "[agent]\nmax_turn_retries = 0\n"))
            env = {**os.environ, "HOME": str(root), "SNAJPAGENT_IRC_UI_KEY": "irc-ui-secret"}
            cases = [
                ("named", "event: keepalive\ndata: {\"type\":\"keepalive\",\"time_ms\":123}\n\n", None),
                ("unnamed", "data: {\"type\":\"keepalive\"}\n\n", None),
                ("relay", "event: keepalive\ndata: {\"time_ms\":123}\n\n", None),
                ("mixed", ": heartbeat\n\n"
                 "event: keepalive\ndata: {\"type\":\"keepalive\"}\n\n"
                 "data: [DONE]\n\n"
                 "event: response.subscription_usage\ndata: {}\n\n"
                 "data: {\"type\":\"keepalive\"}\n\n", None),
                ("malformed", "event: keepalive\ndata: {broken\n\n", "invalid Responses JSON"),
                ("non-object", "event: keepalive\ndata: []\n\n", "Responses event has no type"),
                ("mismatch", "event: keepalive\ndata: {\"type\":\"response.completed\"}\n\n",
                 "SSE event name and JSON type disagree"),
                ("duplicate", "data: {\"type\":\"response.completed\"}\n\n",
                 "Responses event follows terminal completion"),
                ("late-delta", "data: {\"type\":\"response.output_text.delta\",\"delta\":\"late\"}\n\n",
                 "Responses event follows terminal completion"),
            ]
            for name, suffix, error in cases:
                state = root / name
                before = len(provider.requests)
                result = subprocess.run([str(binary), "--config", str(config), "--dotdir", str(state),
                                         "-e", "--", "finish once"], cwd=root, env=env,
                                        capture_output=True, text=True, timeout=20)
                assert len(provider.requests) == before + 1, (name, result, provider.requests[before:])
                path, = (state / "sessions").glob("*/events.jsonl")
                events = [json.loads(line) for line in path.read_text().splitlines()]
                completed = [e for e in events if e["type"] == "response_completed"]
                failed = [e for e in events if e["type"] == "response_failed"]
                assert sum(e["type"] == "turn_started" for e in events) == 1, name
                if error:
                    assert result.returncode != 0 and error in result.stderr, (name, result)
                    assert len(failed) == 1 and not completed, (name, failed, completed)
                else:
                    assert result.returncode == 0, (name, result.stderr)
                    assert result.stdout.strip() == text, (name, result.stdout)
                    assert not failed and len(completed) == 1, (name, failed, completed)
                    assert sum(e["type"] == "turn_completed" for e in events) == 1, name
                    assert not any(e["type"] == "turn_recovery" for e in events), name
                    assert "Retrying" not in result.stderr and "recovering" not in result.stderr
            assert not provider.failure, provider.failure
            print("Responses keepalives: completed turns retained once; invalid trailers rejected: ok",
                  flush=True)
    finally:
        provider.close()


if __name__ == "__main__":
    check_keepalives(Path(sys.argv[1]).resolve())
