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

from tmux_terminal import TmuxTerminal, wait_normalized, normalize_space

old, new = (Path(p).resolve() for p in sys.argv[1:3])
width = int(sys.argv[3]) if len(sys.argv) > 3 else 110
color = sys.argv[4] if len(sys.argv) > 4 else "never"
marker = b"\nsnajpagent-update-v1\nsnajpagent\nlinux-x86_64\nhttps://agent.snajpa.net\n"
data = new.read_bytes()
assert marker in old.read_bytes() and marker in data
version = subprocess.check_output([new, "-V"], text=True).strip().split()[-1]
assert marker + version.encode() + b"\n" in data
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
                      f"[ui]\ncolor = {color}\nmarkdown = true\n")
    try:
        with TmuxTerminal(tmp / "t", exe, work, tmp / "state", config, width, 28,
                          environment=dict(SSL_CERT_FILE=str(cert), SNAJPAGENT_TEST_UI_KEY="local-fixture")) as term:
            assert ready.wait(5), term.capture()
            # Input remains usable while discovery is held at the TLS endpoint.
            term.send_text("keep this update draft")
            wait_normalized(term, "keep this update draft", timeout=10)
            assert exe.read_bytes() == old.read_bytes()
            release.set()
            wait_normalized(term, "=== snajpagent updated ===", timeout=10)
            screen = normalize_space(wait_normalized(term, "Restart when convenient to use it.", timeout=10)[0])
            wait_normalized(term, "keep this update draft", timeout=10)
            physical = term.capture()
            assert "convenient" in physical, physical
            if width >= 28: assert "snajpagent updated" in physical, physical
            (tmp / "screen.txt").write_text(physical)
            assert screen.count("=== snajpagent updated ===") == 1
            assert "Restart when convenient" in screen
            assert "downloads.html#changelog" in term.capture(join_wrapped=True)
            assert exe.read_bytes() == data and not term.dead()
            term.send_key("C-u")
            term.submit("/exit")
            term.wait_until(lambda _: term.dead(), "clean exit", timeout=5)
            assert normalize_space(term.capture(join_wrapped=True)).count("=== snajpagent updated ===") == 1
        assert requests == ["/snajpagent-linux-x86_64.json", "/immutable"]
        # Persistent conversation data must not acquire a local update notice.
        for journal in (tmp / "state").rglob("*.jsonl"):
            assert b"snajpagent updated" not in journal.read_bytes()
    finally:
        release.set()
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)
print(f"PASS: TLS update {width}/{color}, responsive UI and retained draft, one local banner, clean exit")
