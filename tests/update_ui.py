#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise a real update through the existing terminal UI, over private TLS.

Pass an updater-enabled development fixture executable and a newer matching
production executable. Neither endpoint nor provider uses external services.
"""
import hashlib
import http.server
import json
import os
from pathlib import Path
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading

from tmux_terminal import TmuxTerminal

old, new = (Path(p).resolve() for p in sys.argv[1:])
marker = b"\nsnajpagent-update-v1\nsnajpagent\nlinux-x86_64\nhttps://agent.snajpa.net\n"
data = new.read_bytes()
assert marker in old.read_bytes() and marker in data
version = data.split(marker)[1].split(b"\n")[0].decode()
root = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix="uui-", dir=root / "build") as tmp:
    tmp = Path(tmp)
    # Keep the tmux socket comfortably below the Unix path-length limit.
    exe = tmp / "snajpagent"
    shutil.copy2(old, exe)
    cert, key = tmp / "cert.pem", tmp / "key.pem"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                    "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
                    "-keyout", key, "-out", cert], check=True, capture_output=True)
    ready, release = threading.Event(), threading.Event()
    requests = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            requests.append(self.path)
            if self.path.endswith(".json"):
                ready.set()
                if not release.wait(12):
                    self.send_error(504)
                    return
                body = json.dumps(dict(name="snajpagent", target="linux-x86_64", version=version,
                    url=f"{base}/immutable", sha256=hashlib.sha256(data).hexdigest(), size=len(data),
                    changelog="https://agent.snajpa.net/downloads.html#changelog")).encode()
            else:
                body = data
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"https://localhost:{server.server_port}"
    work = tmp / "work"
    work.mkdir()
    config = tmp / "config.ini"
    config.write_text(f"[agent]\nmodel = gpt-5.5\nread_agents_md = false\nauto_update = true\n"
                      f"update_url = {base}/snajpagent-linux-x86_64\n"
                      "[provider openai]\napi_key = ${SNAJPAGENT_TEST_UI_KEY}\n"
                      "[ui]\ncolor = never\nmarkdown = true\n")
    try:
        with TmuxTerminal(tmp / "t", exe, work, tmp / "state", config, 110, 28,
                          environment=dict(SSL_CERT_FILE=str(cert), SNAJPAGENT_TEST_UI_KEY="local-fixture")) as term:
            assert ready.wait(5), term.capture()
            # Input remains usable while discovery is held at the TLS endpoint.
            term.send_text("keep this update draft")
            term.wait("keep this update draft")
            assert exe.read_bytes() == old.read_bytes()
            release.set()
            screen = term.wait("=== snajpagent updated ===", timeout=10)
            screen = term.wait("keep this update draft")
            assert screen.count("=== snajpagent updated ===") == 1
            assert "Restart when convenient" in screen and "downloads.html#changelog" in screen
            assert exe.read_bytes() == data and not term.dead()
            term.send_key("C-u")
            term.submit("/exit")
            term.wait_until(lambda _: term.dead(), "clean exit", timeout=5)
            assert term.capture().count("=== snajpagent updated ===") == 1
        assert requests == ["/snajpagent-linux-x86_64.json", "/immutable"]
        # Persistent conversation data must not acquire a local update notice.
        for journal in (tmp / "state").rglob("*.jsonl"):
            assert b"snajpagent updated" not in journal.read_bytes()
    finally:
        release.set()
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)
print("PASS: TLS update, responsive UI and retained draft, one local banner, clean exit")
