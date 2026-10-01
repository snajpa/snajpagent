#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Provider TLS chooses HTTP/1.1 when HTTP/2 streams are reset by the gateway."""
import http.server
import json
import os
import shutil
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
from pathlib import Path


def event(kind, **data):
    return ("data: " + json.dumps({"type": kind, **data}) + "\n\n").encode()


def response(partial=False):
    item = {"id": "message", "type": "message", "status": "in_progress",
            "role": "assistant", "phase": "final_answer", "content": []}
    position = {"item_id": "message", "output_index": 0, "content_index": 0}
    body = event("response.created", response={"id": "response", "status": "in_progress",
                                              "output": []})
    body += event("response.output_item.added", output_index=0, item=item)
    body += event("response.content_part.added", **position,
                  part={"type": "output_text", "text": "", "annotations": []})
    text = "partial answer" if partial else "HTTPS answer once"
    body += event("response.output_text.delta", **position, delta=text)
    if partial:
        return body
    body += event("response.output_text.done", **position, text=text)
    item = dict(item, status="completed",
                content=[{"type": "output_text", "text": text, "annotations": []}])
    body += event("response.output_item.done", output_index=0, item=item)
    body += event("response.completed", response={"id": "response", "status": "completed",
        "output": [], "usage": {"input_tokens": 1, "output_tokens": 3, "total_tokens": 4}})
    return body


def frame(kind, flags, stream, payload=b""):
    return len(payload).to_bytes(3, "big") + bytes([kind, flags]) + struct.pack("!I", stream) + payload


def read_exact(stream, size):
    data = bytearray()
    while len(data) < size:
        part = stream.read(size - len(data))
        assert part, "HTTP/2 fixture peer closed early"
        data.extend(part)
    return bytes(data)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def handle(self):
        negotiated = self.connection.selected_alpn_protocol()
        self.server.protocols.append(negotiated)
        self.connection.settimeout(8)
        if negotiated != "h2":
            return super().handle()
        # No HTTP/2 dependency: accept one request, then reset its stream with
        # INTERNAL_ERROR, matching a broken gateway's actual framing failure.
        assert read_exact(self.rfile, 24) == b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
        self.connection.sendall(frame(4, 0, 0))  # SETTINGS
        while True:
            head = read_exact(self.rfile, 9)
            size, kind, flags = int.from_bytes(head[:3], "big"), head[3], head[4]
            stream = int.from_bytes(head[5:], "big") & 0x7fffffff
            read_exact(self.rfile, size)
            if kind == 4 and not flags & 1:
                self.connection.sendall(frame(4, 1, 0))
            if kind in (0, 1) and flags & 1:
                # HPACK indexed :status 200 followed by RST_STREAM.
                self.connection.sendall(frame(1, 4, stream, b"\x88") +
                                        frame(3, 0, stream, struct.pack("!I", 2)))
                self.server.resets += 1
                return

    def reply(self, status, body, content_type="application/json", truncated=False):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body) + (100 if truncated else 0)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()
        self.close_connection = True

    def do_GET(self):
        self.server.requests.append((self.command, self.path, self.request_version))
        self.reply(200, b'{"data":[{"id":"tls-model"}]}')

    def do_POST(self):
        size = int(self.headers.get("Content-Length", "0"))
        assert len(self.rfile.read(size)) == size
        self.server.requests.append((self.command, self.path, self.request_version))
        assert self.path == "/v1/responses", self.path
        partial = self.server.partial
        self.reply(200, response(partial), "text/event-stream", truncated=partial)


def check(binary):
    if not ssl.HAS_ALPN or not shutil.which("openssl"):
        print("provider HTTPS: skipped (ALPN or openssl unavailable)", flush=True)
        return
    with tempfile.TemporaryDirectory(prefix="snag-provider-https-") as tmp:
        root = Path(tmp).resolve()
        cert, key = root / "cert.pem", root / "key.pem"
        conf = root / "openssl.cnf"
        conf.write_text("[req]\nprompt=no\ndistinguished_name=dn\nx509_extensions=ext\n"
                        "[dn]\nCN=localhost\n[ext]\nsubjectAltName=DNS:localhost,IP:127.0.0.1\n"
                        "basicConstraints=critical,CA:TRUE\n"
                        "keyUsage=critical,digitalSignature,keyEncipherment,keyCertSign\n"
                        "extendedKeyUsage=serverAuth\n")
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-days", "1", "-keyout", str(key), "-out", str(cert),
                        "-config", str(conf)], check=True, capture_output=True)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        context.set_alpn_protocols(["h2", "http/1.1"])
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        server.protocols, server.requests, server.resets, server.partial = [], [], 0, False
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        config = root / "config.ini"
        config.write_text("[agent]\nmodel=tls-model\nread_agents_md=false\nmax_turn_retries=0\n"
            f"[provider fixture]\nbase_url=https://127.0.0.1:{server.server_port}/v1\n"
            'api_key="fixture-secret"\nconnect_timeout_ms=2000\nrequest_timeout_ms=5000\n'
            "idle_timeout_ms=3000\nexact_token_count=false\nnative_compaction=false\n"
            "[ui]\ncolor=never\n")
        config.chmod(0o600)
        env = {**os.environ, "HOME": str(root), "SSL_CERT_FILE": str(cert),
               "NO_PROXY": "127.0.0.1", "no_proxy": "127.0.0.1"}
        try:
            for name in ("complete", "partial", "untrusted"):
                state = root / name
                server.partial = name == "partial"
                before = len(server.requests)
                case_env = dict(env)
                if name == "untrusted":
                    case_env.pop("SSL_CERT_FILE")
                result = subprocess.run([str(binary), "--config", str(config), "--dotdir",
                    str(state), "-e", "--", "answer"], env=case_env, cwd=root,
                    capture_output=True, text=True, timeout=15)
                requests = [r for r in server.requests[before:] if r[0] == "POST"]
                if name == "complete":
                    assert result.returncode == 0, (result.stderr, server.protocols, server.resets)
                    assert result.stdout.strip() == "HTTPS answer once", result.stdout
                    assert len(requests) == 1, requests
                elif name == "partial":
                    assert result.returncode != 0, result
                    assert "provider transport failed" in result.stderr, result.stderr
                    assert len(requests) == 1, requests
                    path, = (state / "sessions").glob("*/events.jsonl")
                    events = [json.loads(line) for line in path.read_text().splitlines()]
                    failure, = [e["data"] for e in events if e["type"] == "response_failed"]
                    assert failure["retry_count"] == 0, failure
                    partial, = failure["partial_public"]
                    assert partial["text"] == "partial answer", partial
                    assert not any(e["type"] == "response_completed" for e in events)
                else:
                    assert result.returncode != 0 and not requests, (result, requests)
                    assert "certificate" in result.stderr.lower(), result.stderr
                assert server.resets == 0 and set(server.protocols) == {"http/1.1"}
                assert all(r[2] == "HTTP/1.1" for r in server.requests)
            print("provider HTTPS: HTTP/1.1 chosen over reset-prone h2; completed once, "
                  "partial stream not replayed, TLS trust enforced: ok", flush=True)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)
            assert not thread.is_alive()


if __name__ == "__main__":
    check(Path(sys.argv[1]).resolve())
