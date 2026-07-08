#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Live OpenAI-compatible provider smoke check for a built snajpagent binary.

This is intentionally outside `make check` and `make releasecheck`: it requires a
real provider API key, network access, and provider quota. It creates a private
throwaway HOME/XDG_STATE_HOME/workspace, runs one direct turn, and verifies the
durable evidence claims appropriate to the selected live profile.

Set SNAJPAGENT_LIVE_BASE_URL and SNAJPAGENT_LIVE_API_KEY_ENV to target an
OpenAI-compatible proxy. They default to https://api.openai.com and
OPENAI_API_KEY. Set SNAJPAGENT_LIVE_PROFILE=codex-lb for a proxy that supports
the main Responses endpoint but not auxiliary count/compact endpoints.
"""

import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile


def die(message: str, code: int = 1) -> None:
    print(f"live_provider_check: {message}", file=sys.stderr)
    raise SystemExit(code)


def load_events(state: pathlib.Path) -> list[dict]:
    sessions = list((state / "snajpagent" / "sessions").glob("*/events.jsonl"))
    if len(sessions) != 1:
        die(f"expected exactly one session log, found {len(sessions)}")
    events: list[dict] = []
    with sessions[0].open("r", encoding="utf-8") as handle:
        for line in handle:
            events.append(json.loads(line))
    return events


def require_hex(value: object, label: str, length: int = 64) -> None:
    if not isinstance(value, str) or len(value) != length:
        die(f"{label} is not a {length}-hex string")
    if any(ch not in "0123456789abcdef" for ch in value):
        die(f"{label} contains non-lowercase-hex characters")


def main() -> int:
    if len(sys.argv) != 2:
        die("usage: live_provider_check.py PATH_TO_SNAJPAGENT", 2)
    binary = pathlib.Path(sys.argv[1]).resolve()
    if not binary.is_file():
        die(f"binary not found: {binary}", 2)
    base_url = os.environ.get("SNAJPAGENT_LIVE_BASE_URL", "https://api.openai.com")
    api_key_env = os.environ.get("SNAJPAGENT_LIVE_API_KEY_ENV", "OPENAI_API_KEY")
    profile = os.environ.get("SNAJPAGENT_LIVE_PROFILE", "openai")
    if profile not in ("openai", "codex-lb"):
        die("SNAJPAGENT_LIVE_PROFILE must be openai or codex-lb", 2)
    model = os.environ.get(
        "SNAJPAGENT_LIVE_MODEL",
        "gpt-5.5" if profile == "codex-lb" else "default",
    )
    if not os.environ.get(api_key_env):
        die(f"{api_key_env} is required for live provider evidence", 2)

    root = pathlib.Path(tempfile.mkdtemp(prefix="snajpagent-live-"))
    try:
        state = root / "state"
        work = root / "work"
        home = root / "home"
        state.mkdir(mode=0o700)
        work.mkdir(mode=0o700)
        home.mkdir(mode=0o700)
        config = root / "config.ini"
        lines = [
            "[agent]\n",
            f"model = {model}\n",
            "\n[provider]\n",
            f"base_url = {base_url}\n",
            f"api_key_env = {api_key_env}\n",
        ]
        if profile == "codex-lb":
            lines.extend([
                "auto_compact_input_tokens = 0\n",
                "exact_token_count = false\n",
                "native_compaction = false\n",
            ])
        else:
            lines.append("auto_compact_input_tokens = 1\n")
        config.write_text("".join(lines), encoding="utf-8")
        env = os.environ.copy()
        env.update({
            "HOME": str(home),
            "XDG_STATE_HOME": str(state),
            "LC_ALL": env.get("LC_ALL", "C.UTF-8"),
        })
        result = subprocess.run(
            [str(binary), "-c", str(config), "-e", "--", "ping"],
            cwd=work,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=180,
            check=False,
        )
        if result.returncode != 0:
            sys.stderr.write(result.stderr)
            die(f"snajpagent exited with status {result.returncode}")
        if not result.stdout.strip():
            die("live provider returned no final stdout")
        events = load_events(state)
        responses = [event for event in events if event.get("type") == "response_started"]
        if len(responses) != 1:
            die(f"expected one response_started event, found {len(responses)}")
        response = responses[0]["data"]
        expected_count_method = (
            "qualified_upper_bound" if profile == "codex-lb" else "exact"
        )
        if response.get("count_method") != expected_count_method:
            die(f"response_started did not record {expected_count_method} input-token counting")
        for key in ("profile_id", "capability_version", "model"):
            if not isinstance(response.get(key), str) or not response[key]:
                die(f"response_started missing {key}")
        require_hex(response.get("request_sha256"), "response request_sha256")
        require_hex(response.get("count_request_sha256"), "response count_request_sha256")

        completed = [event for event in events if event.get("type") == "turn_completed"]
        if len(completed) != 1:
            die(f"expected one turn_completed event, found {len(completed)}")

        compact_starts = [event for event in events if event.get("type") == "compaction_started"]
        compact_done = [event for event in events if event.get("type") == "compaction_completed"]
        if profile == "codex-lb":
            if compact_starts or compact_done:
                die("codex-lb profile unexpectedly ran native compaction")
            print("live_provider_check: ok")
            return 0
        if len(compact_starts) != 1 or len(compact_done) != 1:
            die("automatic native compaction did not complete exactly once")
        start = compact_starts[0]["data"]
        done = compact_done[0]["data"]
        if start.get("reason") != "automatic":
            die("live compaction reason was not automatic")
        if start.get("count_method") != "exact" or done.get("count_method") != "exact":
            die("live compaction input count was not exact")
        if done.get("output_count_method") != "exact":
            die("live compaction output count was not exact")
        require_hex(start.get("request_sha256"), "compact request_sha256")
        require_hex(start.get("count_request_sha256"), "compact count_request_sha256")
        require_hex(done.get("output_count_request_sha256"), "compact output_count_request_sha256")
        print("live_provider_check: ok")
        return 0
    finally:
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
