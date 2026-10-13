#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Native compact API contract, capability negotiation and retained failure state."""
import http.server
import json
import os
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

from store_history import journal_paths, read_events
from test_access_token import response
from test_remote_terminal import RemoteProcess


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, status, body):
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass  # The synchronized cancellation cases deliberately close the caller.

    def do_GET(self):
        assert self.path == "/v1/models", self.path
        if self.server.catalog_delay:
            self.server.catalog_entered.set()
            assert self.server.catalog_release.wait(10)
        if self.server.catalog_status != 200:
            self.reply(self.server.catalog_status, b'{}')
            return
        metadata = dict(getattr(self.server, "catalog_metadata", {"context_window": 1000000}))
        if self.server.v2:
            metadata["native_compaction_protocol"] = "responses_v2"
        body = {"data": [{"id": "gpt-fixture", "metadata": metadata},
                         {"id": "other-model", "metadata": {
                             "native_compaction_protocol": "responses_v2"}}]}
        self.reply(200, json.dumps(body).encode())

    def do_POST(self):
        raw = self.rfile.read(int(self.headers["Content-Length"]))
        body = json.loads(raw)
        assert self.headers.get("Authorization") == "Bearer fixture-secret"
        assert not self.headers.get("ChatGPT-Account-Id")
        if self.path == "/v1/responses/input_tokens":
            self.server.count_requests.append(body)
            self.reply(200, json.dumps({"object": "response.input_tokens", "input_tokens": 1024}).encode())
            return
        self.server.requests.append((self.path, body, dict(self.headers), raw))
        if self.path == "/v1/responses":
            if getattr(self.server, "reject_creates", 0):
                self.server.reject_creates -= 1
                self.reply(400, json.dumps({"error": {"code": "context_length_exceeded",
                            "type": "invalid_request_error", "message": "fixture capacity rejection"}}).encode())
                return
            body = response()
            if getattr(self.server, "response_text", None) is not None:
                body = body.replace(b"HTTPS answer once", self.server.response_text.encode())
            if getattr(self.server, "input_usage", None) is not None:
                assert b'"input_tokens": 1' in body and b'"total_tokens": 4' in body
                body = body.replace(b'"input_tokens": 1',
                                    f'"input_tokens": {self.server.input_usage}'.encode())
                body = body.replace(b'"total_tokens": 4',
                                    f'"total_tokens": {self.server.input_usage + 3}'.encode())
            if getattr(self.server, "omit_input_usage", False):
                body = body.replace(b'"input_tokens": 1', b'"input_tokens": null').replace(
                    b'"total_tokens": 4', b'"total_tokens": null')
            self.reply(200, body)
            return
        assert self.path == "/v1/responses/compact", self.path
        if self.server.failure:
            error = {"error": {"code": "server_error" if self.server.failure == 500 else "not_found",
                               "type": "server_error", "message": "fixture-native-failure"}}
            self.reply(self.server.failure, json.dumps(error).encode())
            return
        if getattr(self.server, "compact_delay", False):
            self.server.compact_entered.set()
            assert self.server.compact_release.wait(10)
        capsule = {"type": "compaction", "encrypted_content": "opaque-api-continuation"}
        output = self.server.invalid_output if self.server.invalid_output is not None else [
            {"type": "message", "role": "user", "content": [{"type": "input_text", "text": "retained native window"}]},
            capsule]
        self.reply(200, json.dumps({"object": "response.compaction", "output": output}).encode())


def check(binary):
    for v2, catalog_status in ((False, 200), (False, 404), (False, 405), (True, 200)):
        with tempfile.TemporaryDirectory(prefix="snag-native-api-") as directory:
            root = Path(directory).resolve()
            state = root / "state"
            config = root / "config.ini"
            server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
            server.requests, server.v2, server.failure = [], v2, 0
            server.count_requests = []
            server.catalog_status = catalog_status
            server.catalog_delay = False
            server.catalog_entered, server.catalog_release = threading.Event(), threading.Event()
            server.invalid_output = None
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            text = ("[agent]\nmodel=gpt-fixture\nreasoning_effort=xhigh\n"
                    "read_agents_md=false\nmax_turn_retries=0\n"
                    f"[provider fixture]\nbase_url=http://127.0.0.1:{server.server_port}/v1\n"
                    'api_key="fixture-secret"\nconnect_timeout_ms=2000\nrequest_timeout_ms=5000\n'
                    "idle_timeout_ms=3000\nexact_token_count=false\nnative_compaction=true\n"
                    "[ui]\ncolor=never\n")
            if v2:
                text = text.replace("exact_token_count=false", "exact_token_count=true")
            config.write_text(text)
            config.chmod(0o600)
            env = {**os.environ, "HOME": str(root), "NO_PROXY": "127.0.0.1",
                   "no_proxy": "127.0.0.1", "SNAJPAGENT_DOTDIR": str(state)}
            env.pop("OPENAI_API_KEY", None)
            prefix = [str(binary), "--dotdir", str(state), "--config", str(config), "-m", "fixture/gpt-fixture"]

            def run(*args, prompt="Keep the original native context.", ok=True):
                result = subprocess.run([*prefix, *args], input=prompt, text=True,
                                        capture_output=True, cwd=root, env=env, timeout=20)
                assert (result.returncode == 0) == ok, (args, result.returncode, result.stderr)
                assert "fixture-secret" not in result.stdout + result.stderr
                return result

            try:
                run("-e")
                journal, = journal_paths(state)
                sid = journal.parent.name
                foreground = server.requests[-1][1]
                server.requests.clear()
                server.count_requests.clear()
                child = RemoteProcess(root, [*prefix, "--resume", sid], wrapped=None,
                                      extra_env=env, winsize=(24, 100))
                try:
                    child.until("›".encode(), timeout=10)
                    os.write(child.master, b"/compact\r")
                    child.until(b"Compacted", timeout=10)
                    os.write(child.master, b"/exit\r")
                    child.wait(0)
                finally:
                    child.close()
                compact, = server.requests
                assert compact[0] == "/v1/responses/compact"
                assert compact[2].get("session_id") == sid
                body = compact[1]
                assert not any(i.get("type") == "compaction_trigger" for i in body["input"])
                if v2:
                    assert body["reasoning"] == foreground["reasoning"]
                    assert body["tools"] == foreground["tools"]
                    assert body["parallel_tool_calls"] == foreground["parallel_tool_calls"]
                    counted = [r for r in server.count_requests if r.get("input") == body["input"]]
                    assert counted and counted[0]["tools"] == body["tools"]
                    assert counted[0]["reasoning"] == body["reasoning"]
                    assert "prompt_cache_key" not in counted[0]
                else:
                    assert "reasoning" not in body and "tools" not in body
                    assert "parallel_tool_calls" not in body
                events = read_events(journal)
                assert events[-1]
                done = [e for e in events if e["type"] == "compaction_completed"]
                assert len(done) == 1 and done[0]["data"]["output"][-1] == {
                    "type": "compaction", "encrypted_content": "opaque-api-continuation"}
                if catalog_status == 200:
                    config.write_text(text)
                    run("--resume", sid, "-e", prompt="Keep this turn for catalogue cancellation.")
                    server.requests.clear()
                    server.catalog_delay = True
                    child = RemoteProcess(root, [*prefix, "--resume", sid], wrapped=None,
                                          extra_env=env, winsize=(24, 100))
                    try:
                        child.until("›".encode(), timeout=10)
                        os.write(child.master, b"/compact\r")
                        assert server.catalog_entered.wait(5), "optional discovery never started"
                        os.write(child.master, b"\x03")
                        child.until(b"compaction interrupted; previous context retained", timeout=10)
                        assert not server.requests
                        server.catalog_release.set()
                        os.write(child.master, b"/exit\r")
                        child.wait(0)
                    finally:
                        server.catalog_release.set()
                        child.close()
                        server.catalog_delay = False
                    assert len([e for e in read_events(journal) if e["type"] == "compaction_completed"]) == 1

                for status in (500, 404):
                    config.write_text(text)
                    server.failure = 0
                    run("--resume", sid, "-e", prompt="Preserve this complete turn: " + "a" * 10000)
                    before = read_events(journal)
                    before_done = [e for e in before if e["type"] == "compaction_completed"]
                    config.write_text(text.replace("native_compaction=true", "native_compaction=true\nauto_compact_input_tokens=1"))
                    server.requests.clear()
                    server.failure = status
                    result = run("--resume", sid, "-e", prompt="Continue with the same context.", ok=False)
                    assert "fixture-native-failure" in result.stderr, result.stderr
                    assert server.requests and all(r[0] == "/v1/responses/compact" for r in server.requests)
                    assert len({r[3] for r in server.requests}) == 1, "failure changed the source before retry"
                    after = read_events(journal)
                    assert [e for e in after if e["type"] == "compaction_completed"] == before_done
                    assert any(e["type"] == "compaction_interrupted" for e in after)
                for bad_output in (
                    [{"type": "message", "role": "assistant", "content": "plain-text"}],
                    [{"type": "compaction", "encrypted_content": "valid"}, {"type": "message"}],
                    [{"type": "compaction", "encrypted_content": "valid"},
                     {"type": "message", "role": "user", "content": [{"type": "input_text"}]}],
                    [{"type": "compaction", "encrypted_content": "valid"},
                     {"type": "compaction", "encrypted_content": ""}],
                    [{"type": "compaction", "encrypted_content": "first"},
                     {"type": "compaction", "encrypted_content": "second"}],
                ):
                    server.failure = 0
                    server.invalid_output = bad_output
                    server.requests.clear()
                    before_done = [e for e in read_events(journal) if e["type"] == "compaction_completed"]
                    result = run("--resume", sid, "-e", prompt="Keep native state intact.", ok=False)
                    assert "encrypted" in result.stderr or "compact output" in result.stderr
                    after = read_events(journal)
                    assert [e for e in after if e["type"] == "compaction_completed"] == before_done
                    assert all(r[0] == "/v1/responses/compact" for r in server.requests)
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=5)
    print("Native compact API: capability-scoped parameters, standard API, native output and retained failures: ok")


def check_context_limits(binary):
    cases = (
        ({}, 784800, 706320, 0),
        ({"input_context_window": 272000}, 272000, 244800, 1),
        ({"input_context_window": 600000}, 600000, 540000, 0),
        ({"input_context_window": 600000, "max_input_tokens": 800000}, 600000, 540000, 0),
        ({"max_input_tokens": 800000, "max_output_tokens": 128000}, 744000, 669600, 0),
        ({}, 784800, 0, 0),
    )
    for declared, budget, threshold, compacts in cases:
        with tempfile.TemporaryDirectory(prefix="snag-native-limits-") as directory:
            root = Path(directory).resolve()
            state, config = root / "state", root / "config.ini"
            server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
            server.requests, server.count_requests = [], []
            server.v2, server.failure, server.catalog_status = True, 0, 200
            server.catalog_delay, server.invalid_output, server.input_usage = False, None, 300000
            server.catalog_metadata = {"context_window": 272000, "max_context_window": 872000,
                                       **declared}
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            config.write_text("[agent]\nmodel=gpt-fixture\nreasoning_effort=xhigh\n"
                              "read_agents_md=false\nmax_turn_retries=0\n"
                              f"[provider fixture]\nbase_url=http://127.0.0.1:{server.server_port}/v1\n"
                              'api_key="fixture-secret"\nexact_token_count=false\nnative_compaction=true\n'
                              f"auto_compact_input_tokens={'auto' if threshold else '0'}\n"
                              "connect_timeout_ms=2000\nrequest_timeout_ms=5000\nidle_timeout_ms=3000\n"
                              "[ui]\ncolor=never\n")
            config.chmod(0o600)
            env = {**os.environ, "HOME": str(root), "SNAJPAGENT_DOTDIR": str(state),
                   "NO_PROXY": "127.0.0.1", "no_proxy": "127.0.0.1", "OPENAI_API_KEY": None}
            prefix = [str(binary), "--dotdir", str(state), "--config", str(config),
                      "-m", "fixture/gpt-fixture"]
            child = RemoteProcess(root, prefix, wrapped=None, extra_env=env, winsize=(24, 180))
            try:
                child.until("›".encode(), timeout=10)
                os.write(child.master, b"/model cache\r")
                child.until(b"cache updated:", timeout=10)
                os.write(child.master, b"/context max\r")
                expected = f"input-budget={budget} · compact={threshold}".encode()
                child.until(expected, timeout=10)
                os.write(child.master, b"Exercise the selected native input budget.\r")
                child.until_after(b"once", "›".encode(), timeout=10)
                if not compacts:
                    child.output.clear()
                    os.write(child.master, b"/context\r")
                    child.until_after(b"context for next turn:", "›".encode(), timeout=10)
                    advice = b"last measured input has reached an active limit" in child.output
                    assert not advice, child.output
                if not threshold:
                    # Disabling the soft policy still leaves the hard budget guard active.
                    for window, needs_compact in ((333334, False), (272000, True)):
                        child.output.clear()
                        os.write(child.master, f"/context {window}\r".encode())
                        child.until_after(b"context for next turn:", "›".encode(), timeout=10)
                        advice = b"last measured input has reached an active limit" in child.output
                        assert advice == needs_compact, (window, child.output)
                os.write(child.master, b"/exit\r")
                child.wait(0)
                journal, = journal_paths(state)
                events = read_events(journal)
                starts = [e for e in events if e["type"] == "response_started"]
                assert len(starts) == 1 and starts[0]["data"]["hard_input_tokens"] == budget
                responses = [e for e in events if e["type"] == "response_completed"]
                assert len(responses) == 1 and responses[0]["data"]["usage"]["input_tokens"] == 300000
                completed = [e for e in events if e["type"] == "compaction_completed"]
                assert len(completed) == compacts, (declared, completed)
                assert len([r for r in server.requests if r[0] == "/v1/responses/compact"]) == compacts
                assert b"fixture-secret" not in child.output
            finally:
                child.close()
                server.shutdown()
                server.server_close()
                thread.join(timeout=5)
    print("Native context limits: default, maximum, independent ceilings and automatic compaction: ok")


def check_catalog_refresh(binary):
    with tempfile.TemporaryDirectory(prefix="snag-context-refresh-") as directory:
        root = Path(directory).resolve()
        state, config = root / "state", root / "config.ini"
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.requests, server.count_requests = [], []
        server.v2, server.failure, server.catalog_status = True, 0, 200
        server.catalog_delay, server.invalid_output, server.input_usage = False, None, 300000
        server.catalog_metadata = {"context_window": 272000, "max_context_window": 872000,
                                   "input_context_window": 272000}
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        config.write_text("[agent]\nmodel=gpt-fixture\nreasoning_effort=max\n"
                          "read_agents_md=false\nmax_turn_retries=0\n"
                          f"[provider fixture]\nbase_url=http://127.0.0.1:{server.server_port}/v1\n"
                          'api_key="fixture-secret"\nexact_token_count=false\nnative_compaction=true\n'
                          "[ui]\ncolor=never\n")
        config.chmod(0o600)
        env = {**os.environ, "HOME": str(root), "SNAJPAGENT_DOTDIR": str(state),
               "NO_PROXY": "127.0.0.1", "no_proxy": "127.0.0.1", "OPENAI_API_KEY": None}
        prefix = [str(binary), "--dotdir", str(state), "--config", str(config)]
        child = RemoteProcess(root, prefix, wrapped=None, extra_env=env, winsize=(24, 180))

        def command(text, expected):
            child.output.clear()
            os.write(child.master, text.encode() + b"\r")
            initial = child.until(expected.encode(), timeout=10)
            return initial + child.until_after(expected.encode(), "›".encode(), timeout=10)

        try:
            child.until("›".encode(), timeout=10)
            command("/model cache", "cache updated:")
            server.catalog_metadata.pop("input_context_window")
            # Selection uses the saved catalog until an explicit refresh.
            for selector in ("/model fixture/gpt-fixture/max:max", "/context max"):
                shown = command(selector, "input-budget=272000")
                assert b"input_context_window_tokens=272000" in shown, shown
                assert b"/model cache" in shown, shown
                assert not server.requests
            command("/model cache", "cache updated:")
            for selector in ("/context max", "/model fixture/gpt-fixture/max:max"):
                shown = command(selector, "input-budget=784800")
                assert b"compact=706320" in shown, shown
                assert b"input_context_window_tokens=272000" not in shown
            command("Exercise refreshed maximum context", "once")
            os.write(child.master, b"/exit\r")
            child.wait(0)
            journal, = journal_paths(state)
            events = read_events(journal)
            assert not any(e["type"] == "compaction_started" for e in events)
            starts = [e["data"] for e in events if e["type"] == "response_started"]
            assert len(starts) == 1 and starts[0]["hard_input_tokens"] == 784800
            assert starts[0]["effort"] == "max"
            child.close()
            child = RemoteProcess(root, [*prefix, "--resume", journal.parent.name], wrapped=None,
                                  extra_env=env, winsize=(24, 180))
            child.until("›".encode(), timeout=10)
            shown = command("/context", "input-budget=784800")
            assert b"max (advertised maximum)" in shown, shown
            os.write(child.master, b"/exit\r")
            child.wait(0)
            assert len(server.requests) == 1
        finally:
            child.close()
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)
    print("Context maximum: stale ceiling diagnostic, explicit refresh, model suffix and resume: ok")


def check_compact_byte_ratios(binary, mode, exact=True):
    with tempfile.TemporaryDirectory(prefix="snag-compact-ratio-") as directory:
        root = Path(directory).resolve()
        state, config = root / "state", root / "config.ini"
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.requests, server.count_requests = [], []
        server.v2, server.failure, server.catalog_status = True, 0, 200
        server.catalog_delay, server.input_usage = False, 50000
        server.invalid_output = [{"type": "compaction", "encrypted_content": "opaque-context"}]
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        config.write_text("[agent]\nmodel=gpt-fixture\nreasoning_effort=max\n"
                          "read_agents_md=false\nmax_turn_retries=0\n"
                          f"[provider fixture]\nbase_url=http://127.0.0.1:{server.server_port}/v1\n"
                          'api_key="fixture-secret"\n'
                          f"exact_token_count={'true' if exact else 'false'}\n"
                          f"native_compaction={'true' if mode == 'source' else 'false'}\n"
                          "auto_compact_input_tokens=0\n"
                          "[model-limit fixture/gpt-fixture]\nmax_input_tokens=100000\n"
                          "[ui]\ncolor=never\n")
        config.chmod(0o600)
        env = {**os.environ, "HOME": str(root), "SNAJPAGENT_DOTDIR": str(state),
               "NO_PROXY": "127.0.0.1", "no_proxy": "127.0.0.1"}
        env.pop("OPENAI_API_KEY", None)
        prefix = [str(binary), "--dotdir", str(state), "--config", str(config)]
        markers = [f"complete-source-{n}" for n in range(3 if mode == "source" else 1)]
        child = None
        try:
            sid = None
            for marker in markers:
                resume = ["--resume", sid] if sid else []
                result = subprocess.run([*prefix, *resume, "-e"], cwd=root, env=env,
                                        input=marker + " " + "x" * 200000,
                                        capture_output=True, text=True, timeout=30)
                assert result.returncode == 0, result.stderr
                journal, = journal_paths(state)
                sid = journal.parent.name
            creates = len(server.requests)
            if mode == "output":
                server.response_text = "fitting-summary " + "x" * 150000
            child = RemoteProcess(root, [*prefix, "--resume", sid], wrapped=None,
                                  extra_env=env, winsize=(24, 180))
            child.until("›".encode(), timeout=10)
            if mode == "source":
                child.output.clear()
                os.write(child.master, b"/context 10000\r")
                child.until_after(b"context for next turn:", "›".encode(), timeout=10)
            child.output.clear()
            os.write(child.master, b"/compact\r")
            child.until_after(b"Compacted", "›".encode(), timeout=20)
            os.write(child.master, b"/exit\r")
            child.wait(0)
            compacts = [body for path, body, _, _ in server.requests[creates:]]
            assert len(compacts) == 1, ("encoded bytes triggered a second compact", len(compacts))
            assert all(marker in json.dumps(compacts[0]["input"]) for marker in markers), (
                "byte/token ratio trimmed a source whose exact count fits",
                [marker for marker in markers if marker in json.dumps(compacts[0]["input"])])
            if exact:
                assert any(count["input"] == compacts[0]["input"]
                           for count in server.count_requests)
            else:
                assert not server.count_requests
            completed = [e["data"] for e in read_events(journal)
                         if e["type"] == "compaction_completed"]
            assert len(completed) == 1
            assert completed[0]["output_count_method"] == ("exact" if exact else "unknown")
        finally:
            if child is not None:
                child.close()
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)
    print(f"Compact {mode}, exact={exact}: byte ratios do not select or reduce context: ok")


def check_post_turn_usage(binary):
    with tempfile.TemporaryDirectory(prefix="snag-post-turn-usage-") as directory:
        root = Path(directory).resolve()
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.requests, server.count_requests = [], []
        server.v2, server.catalog_status, server.catalog_delay = True, 200, False
        server.invalid_output = [{"type": "compaction", "encrypted_content": "measured-summary"}]
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            for exact in (False, True):
                for usage, threshold, failure in ((4999, 5000, 0), (5000, 5000, 0),
                                                  (5001, 5000, 0), (9000, 0, 0),
                                                  (5000, 5000, 404), (None, 1000, 0)):
                    case = root / f"{exact}-{usage}-{threshold}-{failure}"
                    case.mkdir(mode=0o700)
                    state, config = case / "state", case / "config.ini"
                    config.write_text("[agent]\nmodel=gpt-fixture\nreasoning_effort=max\n"
                                      "read_agents_md=false\nmax_turn_retries=0\n"
                                      f"[provider fixture]\nbase_url=http://127.0.0.1:{server.server_port}/v1\n"
                                      'api_key="fixture-secret"\nnative_compaction=true\n'
                                      f"exact_token_count={'true' if exact else 'false'}\n"
                                      f"auto_compact_input_tokens={threshold}\n"
                                      "[model-limit fixture/gpt-fixture]\nmax_input_tokens=10000\n"
                                      "[ui]\ncolor=never\n")
                    config.chmod(0o600)
                    env = {**os.environ, "HOME": str(case), "SNAJPAGENT_DOTDIR": str(state),
                           "NO_PROXY": "127.0.0.1", "no_proxy": "127.0.0.1"}
                    env.pop("OPENAI_API_KEY", None)
                    server.requests.clear()
                    server.count_requests.clear()
                    server.input_usage, server.failure = usage, failure
                    server.omit_input_usage = usage is None
                    result = subprocess.run([str(binary), "--dotdir", str(state),
                        "--config", str(config), "-e"], input="Use the completed response usage.",
                        cwd=case, env=env, capture_output=True, text=True, timeout=20)
                    assert result.returncode == 0, (case.name, result.stderr)
                    assert result.stdout.strip() == "HTTPS answer once", result.stdout
                    journal, = journal_paths(state)
                    events = read_events(journal)
                    request, = [e for e in events if e["type"] == "response_started"]
                    assert request["data"]["input_tokens_bound"] == (1024 if exact else 0)
                    assert request["data"]["count_method"] == ("exact" if exact else "unknown")
                    reply, = [e for e in events if e["type"] == "response_completed"]
                    assert reply["data"]["usage"]["input_tokens"] == usage
                    turn, = [e for e in events if e["type"] == "turn_completed"]
                    started = [e for e in events if e["type"] == "compaction_started"]
                    completed = [e for e in events if e["type"] == "compaction_completed"]
                    interrupted = [e for e in events if e["type"] == "compaction_interrupted"]
                    measured = usage if usage is not None else (1024 if exact else 0)
                    attempt = bool(threshold and measured >= threshold)
                    assert len(started) == int(attempt), (case.name, started)
                    assert len(completed) == int(attempt and not failure), (case.name, completed)
                    assert len(interrupted) == int(attempt and bool(failure))
                    if attempt:
                        assert started[0]["seq"] > turn["seq"]
                        assert started[0]["data"]["reason"] == "proactive"
                    assert not any(e["type"] in ("context_rebased", "turn_recovery", "turn_failed")
                                   for e in events)
                    assert [path for path, _, _, _ in server.requests] == (
                        ["/v1/responses"] + ["/v1/responses/compact"] * int(attempt))
                    expected_counts = 1 + len(started) + len(completed) if exact else 0
                    assert len(server.count_requests) == expected_counts
                    print(f"Post-turn usage exact={exact}, input={usage}, threshold={threshold}, "
                          f"failure={failure}: ok", flush=True)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    binary = Path(sys.argv[1]).resolve()
    check(binary)
    check_context_limits(binary)
    check_catalog_refresh(binary)
    for mode in ("source", "output"):
        for exact in (False, True):
            check_compact_byte_ratios(binary, mode, exact)
    check_post_turn_usage(binary)
