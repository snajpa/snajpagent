#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Native Codex compaction: real owner, wire hash, durable capsule and resumed input."""
import hashlib
import http.server
import json
import os
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

from store_history import create_legacy, journal_paths, read_events
from test_access_token import ACCOUNT, TOKEN, Handler
from test_remote_terminal import RemoteProcess


class MeasuredHandler(Handler):
    def reply(self, status, body):
        if getattr(self.server, "measure_requests", False) and b'"usage"' in body:
            request = self.server.requests[-1][3]
            tokens = len(json.dumps(request).encode()) // 4
            lines = body.splitlines(keepends=True)
            for i, line in enumerate(lines):
                if not line.startswith(b"data: "):
                    continue
                item = json.loads(line[6:])
                if item["type"] == "response.completed":
                    item["response"]["usage"] = {
                        "input_tokens": tokens, "output_tokens": 3, "total_tokens": tokens + 3}
                    lines[i] = ("data: " + json.dumps(item) + "\n").encode()
            body = b"".join(lines)
        super().reply(status, body)


def check(binary):
    with tempfile.TemporaryDirectory(prefix="snag-codex-compact-") as directory:
        root = Path(directory).resolve()
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), MeasuredHandler)
        server.requests, server.status, server.provider_status = [], 200, 200
        server.raw_requests = []
        server.token, server.account = TOKEN, ACCOUNT
        server.metadata = {"chatgpt_account_id": ACCOUNT, "chatgpt_user_id": "fixture-user",
                           "chatgpt_plan_type": "enterprise", "chatgpt_account_is_fedramp": False}
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        endpoint = f"http://127.0.0.1:{server.server_port}"
        env = {**os.environ, "HOME": str(root), "SNAJPAGENT_TEST_AUTH_BASE": endpoint,
               "SNAJPAGENT_TEST_OPENAI_BASE": endpoint, "NO_PROXY": "127.0.0.1",
               "no_proxy": "127.0.0.1", "SNAJPAGENT_DOTDIR": str(root / "state")}
        env.pop("OPENAI_API_KEY", None)
        state = root / "state"
        prefix = [str(binary), "--dotdir", str(state)]

        def run(*args, secret=None):
            result = subprocess.run([*prefix, *args], input=secret, text=True,
                                    capture_output=True, env=env, cwd=root, timeout=15)
            assert not result.returncode, (args, result.stderr)
            assert TOKEN not in result.stdout + result.stderr
            return result

        try:
            run("-m", "gpt-fixture", "login", "codex", "--with-access-token", secret=TOKEN + "\n")
            config = state / "config.ini"
            assert "native_compaction = true" in config.read_text()
            config.write_text(config.read_text().replace("[agent]\n",
                "[agent]\nread_agents_md=false\nmax_turn_retries=0\n"))
            for legacy in (False, True):
                if legacy:
                    journal = create_legacy(state, root, "codex", "gpt-fixture")
                    seed_args = ("--resume", journal.parent.name)
                else:
                    seed_args = ()
                run(*seed_args, "-e", "--", "Remember the fixture color: violet.")
                if not legacy:
                    journal, = journal_paths(state)
                sid = journal.parent.name
                server.requests.clear()
                server.raw_requests.clear()
                child = RemoteProcess(root, [*prefix, "--resume", sid], wrapped=None,
                                      extra_env=env, winsize=(24, 100))
                try:
                    child.until("›".encode(), timeout=10)
                    os.write(child.master, b"/compact\r")
                    child.until(b"Compacted", timeout=10)
                    assert b"compacting through Responses" not in child.output
                    os.write(child.master, b"/exit\r")
                    child.wait(0)
                finally:
                    child.close()
                posts = [r for r in server.requests if r[0] == "POST"]
                assert len(posts) == 1 and posts[0][1] == "/responses", posts
                request = posts[0][3]
                assert request["input"][-1] == {"type": "compaction_trigger"}
                raw, = server.raw_requests
                digest = hashlib.sha256(raw).hexdigest()
                events = read_events(journal)
                start = next(e for e in reversed(events) if e["type"] == "compaction_started")
                done = next(e for e in reversed(events) if e["type"] == "compaction_completed")
                assert start["data"]["request_sha256"] == digest
                window = done["data"]["output"]
                users = [dict(i, type="message") for i in request["input"]
                         if i.get("role") == "user"]
                assert users and window[:-1] == users
                capsule = window[-1:]
                assert capsule == [{"id": "compact-fixture", "type": "compaction",
                                    "encrypted_content": "opaque-fixture-continuation"}]
                server.requests.clear()
                run("--resume", sid, "-e", "--", "Continue the fixture.")
                request, = [r[3] for r in server.requests if r[0] == "POST"]
                assert capsule[0] in request["input"]
                assert not any(i.get("type") == "compaction_trigger" for i in request["input"])
            config.write_text(config.read_text().replace("native_compaction = true",
                "native_compaction = true\nauto_compact_input_tokens = 100000") +
                "\n[model-limit codex/gpt-fixture]\ncontext_window_tokens = 1000000\n")
            server.measure_requests = True
            for legacy in (False, True):
                journal = create_legacy(state, root, "codex", "gpt-fixture") if legacy else None
                prior = set(journal_paths(state))
                compacts = 0
                for turn in range(10):
                    marker = f"retained fixture request {turn}: "
                    prompt = marker + chr(ord("a") + turn) * 90000
                    args = ("--resume", journal.parent.name) if journal else ()
                    run(*args, "-e", secret=prompt)
                    if journal is None:
                        journal, = set(journal_paths(state)) - prior
                    events = read_events(journal)
                    completed = [e for e in events if e["type"] == "compaction_completed"]
                    if len(completed) == compacts:
                        continue
                    assert len(completed) == compacts + 1, "automatic compaction storm"
                    compacts += 1
                    output = completed[-1]["data"]["output"]
                    retained = output[:-1]
                    retained_bytes = sum(len(json.dumps(i, ensure_ascii=False,
                        separators=(",", ":")).encode()) for i in retained)
                    assert retained_bytes <= 256000, (
                        "native compaction retained an unbounded raw prefix", retained_bytes)
                    assert marker in json.dumps(retained), "newest user input disappeared"
                    assert "retained fixture request 0:" not in json.dumps(retained)
                    assert output[-1]["encrypted_content"] == "opaque-fixture-continuation"
                assert 2 <= compacts <= 4, compacts
                server.requests.clear()
                run("--resume", journal.parent.name, "-e", "--", "Continue after compaction.")
                requests = [r[3] for r in server.requests if r[0] == "POST"]
                assert len(requests) == 1, "resume immediately compacted again"
                assert output[-1] in requests[0]["input"]
                assert len(json.dumps(requests[0]).encode()) < 350000
                events = read_events(journal)
                assert sum(e["type"] == "turn_completed" for e in events) == 11
                assert not any(e["type"] in ("turn_recovery", "response_failed") for e in events)
                oversized = "oversized retained fixture: " + '\\"' * 90000
                run("--resume", journal.parent.name, "-e", secret=oversized)
                events = read_events(journal)
                latest = [e for e in events if e["type"] == "compaction_completed"][-1]
                assert len([e for e in events if e["type"] == "compaction_completed"]) > compacts
                assert "oversized retained fixture:" not in json.dumps(latest["data"]["output"])
                assert latest["data"]["output"][-1] == output[-1]
                assert any(e["type"] == "input_received" and e["data"].get("text") == oversized
                           for e in events), "oversized original input was lost from history"
            print("Codex compaction: native/legacy capsule, bounded repeat and resume: ok")
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    check(Path(sys.argv[1]).resolve())
