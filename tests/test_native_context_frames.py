#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise native context framing through the real interactive voice owner."""
import base64
import hashlib
import http.server
import json
import os
import pty
import select
import signal
import socket
import struct
import sys
import tempfile
import threading
import time
from pathlib import Path


ANSWER = "\r\n".join([
    "v=0", "o=- 1 1 IN IP4 127.0.0.1", "s=-", "t=0 0",
    "a=group:BUNDLE 0", "a=msid-semantic: WMS",
    "m=audio 9 UDP/TLS/RTP/SAVPF 111", "c=IN IP4 0.0.0.0",
    "a=ice-ufrag:fixture", "a=ice-pwd:abcdefghijklmnopqrstuvwxyz",
    "a=fingerprint:sha-256 " + ":".join(["01"] * 32),
    "a=setup:active", "a=mid:0", "a=sendrecv", "a=rtcp-mux",
    "a=rtpmap:111 opus/48000/2", "a=fmtp:111 minptime=10;useinbandfec=1", "",
]).encode()


def exact(stream, count):
    result = bytearray()
    while len(result) < count:
        data = stream.read(count - len(result))
        if not data:
            raise EOFError()
        result.extend(data)
    return bytes(result)


def frame(stream):
    first, second = exact(stream, 2)
    length = second & 127
    if length == 126:
        length = struct.unpack("!H", exact(stream, 2))[0]
    elif length == 127:
        length = struct.unpack("!Q", exact(stream, 8))[0]
    assert length <= 2 * 1024 * 1024
    mask = exact(stream, 4) if second & 128 else None
    data = exact(stream, length)
    if mask:
        data = bytes(byte ^ mask[index % 4] for index, byte in enumerate(data))
    return first & 15, data


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_POST(self):
        assert self.path.startswith("/backend-api/codex/realtime/calls?")
        exact(self.rfile, int(self.headers["Content-Length"]))
        self.send_response(201)
        self.send_header("Content-Type", "application/sdp")
        self.send_header("Content-Length", str(len(ANSWER)))
        self.send_header("Location", "/v1/live/rtc_fixture")
        self.end_headers()
        self.wfile.write(ANSWER)
        self.wfile.flush()

    def do_GET(self):
        try:
            assert self.path == "/backend-api/codex/rtc_fixture", self.path
            key = self.headers["Sec-WebSocket-Key"]
            accept = base64.b64encode(hashlib.sha1(
                (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
            self.send_response(101)
            self.send_header("Upgrade", "websocket")
            self.send_header("Connection", "Upgrade")
            self.send_header("Sec-WebSocket-Accept", accept)
            self.end_headers()
            self.wfile.flush()
            self.connection.settimeout(8)
            while not self.server.stop.is_set():
                opcode, data = frame(self.rfile)
                if opcode == 8:
                    return
                if opcode != 1:
                    continue
                event = json.loads(data)
                if event.get("type") == "session.context.append":
                    self.server.messages.append(event)
                    text = event["content"][0]["text"]
                    assert len(text.encode()) <= 500, (
                        "oversized native context frame", len(text.encode()))
                    snapshot = "".join(item["content"][0]["text"]
                                       for item in self.server.messages)
                    if snapshot.endswith("not a new request or approval."):
                        self.server.received.set()
        except (EOFError, socket.timeout):
            pass
        except BaseException as error:
            self.server.errors.append(error)
            self.server.received.set()
        finally:
            self.close_connection = True


def run(binary):
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.messages = []
    server.errors = []
    server.stop = threading.Event()
    server.received = threading.Event()
    worker = threading.Thread(target=server.serve_forever)
    worker.start()
    with tempfile.TemporaryDirectory(prefix="snajpagent-native-context-") as directory:
        root = Path(directory).resolve()
        root.chmod(0o700)
        key = root / "key"
        key.write_text("native-context-fixture-key\n")
        key.chmod(0o600)
        config = root / "config.ini"
        config.write_text(
            "[provider fixture]\nauth=api_key\n"
            f"base_url=http://127.0.0.1:{server.server_port}/backend-api/codex\n"
            f"api_key={key}\n[agent]\nprovider=fixture\nmodel=fixture-model\n"
            "[audio]\ncapture_device=absent-context-test-device\n"
            "playback_device=absent-context-test-device\n")
        pid, terminal = pty.fork()
        if pid == 0:
            os.chdir(root)
            env = dict(os.environ, PAGER="", TERM="xterm")
            for name in ("SNAJPAGENT_TEST_OPENAI_BASE", "SNAJPAGENT_DOTDIR"):
                env.pop(name, None)
            os.execve(binary, [binary, "--dotdir", str(root / "state"), "--config", str(config),
                               "--no-listen", "--no-client"], env)
        output = bytearray()

        def collect(seconds, needle=None):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                ready, _, _ = select.select([terminal], [], [], .05)
                if ready:
                    try:
                        data = os.read(terminal, 65536)
                    except OSError:
                        break
                    if not data:
                        break
                    output.extend(data)
                    if needle and needle in output:
                        break
                if not needle and server.received.is_set():
                    break

        try:
            collect(10, "› ".encode())
            assert "› ".encode() in output, output[-1000:]
            os.write(terminal, b"/voice on\r")
            collect(12)
            if b"Native voice is unavailable in this audio-disabled build" in output:
                print("SKIP: native context owner requires the device/RTC-enabled build")
                return
            assert server.received.is_set(), output[-2000:]
            assert not server.errors, server.errors
            assert len(server.messages) > 1, server.messages
            text = "".join(event["content"][0]["text"] for event in server.messages)
            assert "Host session context:" in text and "historical data" in text, text
            assert all(event["channel"] == "commentary" for event in server.messages)
            print("PASS: native owner emits bounded ordered context frames without audio capture")
        finally:
            try:
                os.write(terminal, b"/voice off\r/exit\r")
            except OSError:
                pass
            time.sleep(.4)
            done, _ = os.waitpid(pid, os.WNOHANG)
            if not done:
                os.kill(pid, signal.SIGTERM)
                os.waitpid(pid, 0)
            os.close(terminal)
            server.stop.set()
            server.shutdown()
            server.server_close()
            worker.join()


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()))
