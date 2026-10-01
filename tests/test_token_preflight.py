#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise API-key token preflight through the production CLI without tmux."""

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from tmux_terminal import FakeResponses, event_list, read_events


def run_case(binary, root, rejected):
    provider = FakeResponses()
    counts = []
    creates = []

    def count(handler, request):
        counts.append(request)
        if rejected:
            payload = {"error": {"type": "invalid_request_error",
                                 "code": "invalid_api_key",
                                 "message": "fixture count authentication failed"}}
            status = 401
        else:
            payload = {"object": "response.input_tokens", "input_tokens": 42}
            status = 200
        provider.reply(handler, json.dumps(payload).encode(),
                       "application/json", status)

    def respond(handler, request, sequence):
        creates.append(request)
        assert request["include"] == ["reasoning.encrypted_content"]
        assert request["prompt_cache_key"]
        assert request["parallel_tool_calls"] is True
        if len(creates) == 1:
            body = provider.function_body(sequence, "call_preflight", "get_cwd", {})
        else:
            assert any(item.get("type") == "function_call_output" and
                       item.get("call_id") == "call_preflight"
                       for item in request["input"])
            body = provider.response_body(sequence, "OK")
        provider.reply(handler, body.encode())

    provider.runtime_count_handler = count
    provider.runtime_handler = respond
    dotdir = root / "state"
    config = root / "config.ini"
    config.write_text(
        "[provider openai]\nauth = api_key\n"
        f"base_url = http://127.0.0.1:{provider.port}\n"
        'api_key = "irc-ui-secret"\n'
        "exact_token_count = true\nnative_compaction = true\n"
        "parallel_tool_calls = true\nauto_compact_input_tokens = 0\n"
        "[agent]\nprovider = openai\nmodel = host-model\n"
        "reasoning_effort = default\nread_agents_md = false\n",
        encoding="utf-8",
    )
    config.chmod(0o600)
    environment = os.environ.copy()
    environment.pop("OPENAI_API_KEY", None)
    environment["HOME"] = str(root)
    command = [binary, "--dotdir", str(dotdir), "--config", str(config),
               "--no-listen", "--no-client", "--no-color", "--no-markdown", "-e"]
    try:
        result = subprocess.run(command + ["--", "ping"], cwd=root,
                                env=environment, capture_output=True,
                                text=True, timeout=30)
        assert provider.failure is None, provider.failure
        assert counts, result.stderr
        log, events = read_events(dotdir)
        if rejected:
            assert result.returncode != 0, result.stdout
            assert "fixture count authentication failed" in result.stderr, result.stderr
            assert not creates, creates
            assert event_list(events, "turn_failed"), events
            assert not event_list(events, "turn_completed"), events
            return

        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == "OK", result.stdout
        assert len(creates) == 2, creates
        assert len(event_list(events, "turn_completed")) == 1
        assert event_list(events, "response_started")[0]["data"]["count_method"] == "exact"
        assert all(request["model"] == "host-model" and request["input"]
                   for request in counts)
        assert any(request.get("tools") for request in counts)

        resumed = subprocess.run(command + ["--resume", log.parent.name, "--", "again"],
                                 cwd=root, env=environment, capture_output=True,
                                 text=True, timeout=30)
        assert provider.failure is None, provider.failure
        assert resumed.returncode == 0, resumed.stderr
        assert resumed.stdout.strip() == "OK", resumed.stdout
        assert len(creates) == 3, creates
        _, events = read_events(dotdir)
        assert len(event_list(events, "turn_completed")) == 2
        assert not event_list(events, "turn_failed"), events
    finally:
        provider.close()


def main():
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="snajpagent-preflight-") as directory:
        root = Path(directory).resolve()
        for name, rejected in (("success", False), ("authentication", True)):
            case = root / name
            case.mkdir(mode=0o700)
            run_case(binary, case, rejected)
            print(f"test_token_preflight: {name}: ok", flush=True)


if __name__ == "__main__":
    main()
