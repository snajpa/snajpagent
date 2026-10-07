#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Enterprise PAT login, storage and real Codex request routing with local HTTP."""
import http.server
import json
import os
import subprocess
import sys
import tempfile
import threading
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

from store_history import journal_paths
from test_provider_https import response

TOKEN = "at-fixture-private-token"
ACCOUNT = "fixture-workspace"


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, status, body):
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        if status == 302:
            self.send_header("Location", "/unexpected-redirect")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        self.server.requests.append(("GET", self.path, dict(self.headers), None))
        assert self.headers.get("Authorization") == "Bearer " + self.server.token
        url = urlsplit(self.path)
        if url.path == "/models":
            assert self.headers.get("ChatGPT-Account-Id") == self.server.account
            version = tuple(int(part) for part in parse_qs(url.query)["client_version"][0].split("."))
            names = ["gpt-5.6-sol"]
            if version >= (0, 159, 2):
                names += ["gpt-6.1-sol", "gpt-6-astra"]
            models = [{"slug": name, "visibility": "list", "priority": i,
                       "default_reasoning_level": "high", "context_window": 272000,
                       "supported_reasoning_levels": [{"effort": "high"}, {"effort": "xhigh"}]}
                      for i, name in enumerate(names)]
            self.reply(200, json.dumps({"models": models}).encode())
            return
        assert self.path == "/api/accounts/v1/user-auth-credential/whoami", self.path
        self.reply(self.server.status, json.dumps(self.server.metadata).encode())

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.server.requests.append(("POST", self.path, dict(self.headers), body))
        assert self.headers.get("Authorization") == "Bearer " + self.server.token
        assert self.headers.get("ChatGPT-Account-Id") == self.server.account
        if self.path == "/responses/compact":
            self.reply(404, b'{}')
            return
        assert self.path == "/responses", self.path
        assert body["store"] is False and body["stream"] is True
        assert "instructions" in body and "max_output_tokens" not in body
        self.reply(self.server.provider_status, response() if self.server.provider_status == 200
                   else json.dumps({"error": {"message": self.server.token}}).encode())


def check(binary):
    with tempfile.TemporaryDirectory(prefix="snag-access-token-") as directory:
        root = Path(directory)
        root.chmod(0o700)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.requests, server.status, server.provider_status = [], 200, 200
        server.token, server.account = TOKEN, ACCOUNT
        metadata = {"chatgpt_account_id": ACCOUNT, "chatgpt_user_id": "fixture-user",
                    "chatgpt_plan_type": "enterprise", "chatgpt_account_is_fedramp": False}
        server.metadata = metadata
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        base = f"http://127.0.0.1:{server.server_port}"
        env = {**os.environ, "HOME": str(root), "SNAJPAGENT_TEST_AUTH_BASE": base,
               "SNAJPAGENT_TEST_OPENAI_BASE": base, "NO_PROXY": "127.0.0.1",
               "no_proxy": "127.0.0.1"}
        env.pop("OPENAI_API_KEY", None)
        state = root / "state"

        def run(*args, secret=None, ok=True):
            result = subprocess.run([str(binary), "--dotdir", str(state), *args],
                                    input=secret, text=True, capture_output=True,
                                    cwd=root, env=env, timeout=15)
            assert TOKEN not in result.stdout + result.stderr, "credential leaked"
            assert (result.returncode == 0) == ok, (args, result.returncode, result.stderr)
            return result

        try:
            run("login", "codex", "--with-access-token", secret=TOKEN, ok=False)
            assert not server.requests and not state.exists()
            run("-m", "gpt-fixture", "login", "codex", "--with-access-token", secret=TOKEN + "\n")
            auth = state / "auth/codex.json"
            saved = json.loads(auth.read_text())
            assert saved["kind"] == "codex_token" and saved["account_id"] == ACCOUNT
            assert saved["access_token"] == TOKEN and saved["refresh_token"] == ""
            assert saved["expires_at_ms"] == 0 and auth.stat().st_mode & 0o777 == 0o600
            config = state / "config.ini"
            assert "codex_token" in config.read_text() and TOKEN not in config.read_text()
            assert "codex_token (stored)" in run("login", "status", "codex").stdout
            before = auth.read_bytes(), config.read_bytes()
            # The native catalog filters current models by client compatibility version.
            run("--update-model-cache", "-l", "0")
            providers = json.loads((state / "models.json").read_text())["providers"]
            catalog = next(provider for provider in providers if provider["name"] == "codex")
            models = {model["id"]: model for model in catalog["models"]}
            for name in ("gpt-6.1-sol", "gpt-6-astra"):
                assert name in models, ("current model missing from catalog", name, list(models))
                assert models[name]["efforts"] == ["high", "xhigh"]
                assert models[name]["limits"]["context_window_tokens"] == 272000
            run("login", "codex")
            assert (auth.read_bytes(), config.read_bytes()) == before
            result = run("login", "codex", "--openai-device-auth", ok=False)
            assert "logout codex" in result.stderr
            for status, invalid in ((401, {"message": TOKEN}), (403, {}), (500, {}),
                                    (302, {}), (200, {}), (200, []),
                                    (200, dict(metadata, chatgpt_account_id="bad\r\nheader")),
                                    (200, dict(metadata, chatgpt_account_id="")),
                                    (200, dict(metadata, chatgpt_account_is_fedramp=True))):
                server.status, server.metadata = status, invalid
                run("login", "codex", "--with-access-token", secret=TOKEN + "\n", ok=False)
                assert (auth.read_bytes(), config.read_bytes()) == before
            server.status, server.metadata = 200, metadata
            for options in (("--with-api-key",), ("--openai-device-auth",),
                            ("--meta-device-auth",), ("--with-access-token",)):
                run("login", "codex", "--with-access-token", *options, ok=False)
            count = len(server.requests)
            run("login", "openai", "--with-access-token", secret=TOKEN, ok=False)
            run("login", "codex", "--with-access-token", secret="not-a-PAT\n", ok=False)
            assert len(server.requests) == count
            for field, invalid in (("account_id", ""), ("refresh_token", "unexpected"),
                                   ("expires_at_ms", 1)):
                auth.write_text(json.dumps(dict(saved, **{field: invalid})))
                run("login", "status", "codex", ok=False)
            auth.write_bytes(before[0])
            for invalid in (before[1].replace(b"https://chatgpt.com/backend-api/codex",
                                             b"https://other.invalid"),
                            before[1].replace(b"auth = codex_token",
                                             b'auth = codex_token\napi_key = "unexpected"')):
                config.write_bytes(invalid)
                run("login", "status", "codex", ok=False)
            config.write_bytes(before[1])
            server.token = TOKEN + "-rotated"
            server.account = ACCOUNT + "-rotated"
            server.metadata = dict(metadata, chatgpt_account_id=server.account)
            run("login", "codex", "--with-access-token", secret=server.token + "\n")
            assert json.loads(auth.read_text())["access_token"] == server.token
            assert json.loads(auth.read_text())["account_id"] == server.account
            # Disable outer turn retries so one unauthorized provider response stays one request.
            config.write_text(config.read_text().replace("[agent]\n", "[agent]\n"
                              "read_agents_md=false\nmax_turn_retries=0\n"))
            for status in (200, 401):
                server.provider_status = status
                server.requests.clear()
                result = run("-e", "--", "answer", ok=status == 200)
                assert len(server.requests) == 1 and server.requests[0][:2] == ("POST", "/responses")
                if status == 200:
                    assert result.stdout.strip() == "HTTPS answer once"
            for path in journal_paths(state):
                assert TOKEN.encode() not in path.read_bytes(), "credential leaked into session"
            run("logout", "codex")
            assert not auth.exists()
            print("Enterprise access token: login, rejection, rotation, Codex routing, redaction: ok")
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    check(Path(sys.argv[1]).resolve())
