#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Focused real-executable updater tests; private loopback server only."""
import hashlib
import http.server
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time

old, new, local, stable, aside = (Path(p).resolve() for p in sys.argv[1:])
marker = b"\nsnajpagent-update-v1\nsnajpagent\nlinux-x86_64\nhttps://publisher.test\n"
assert marker in old.read_bytes() and marker in new.read_bytes()
assert marker not in local.read_bytes()
assert "0.99.2-aaaaaaa 0 https://publisher.test/latest-dev/" in subprocess.check_output([old, "--defaults"], text=True)
assert "0.99.2 1 https://publisher.test/latest/" in subprocess.check_output([stable, "--defaults"], text=True)
assert " 0 " in subprocess.check_output([local, "--defaults"], text=True)

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass
    def do_GET(self):
        server = self.server
        with server.guard:
            server.paths.append(self.path)
        server.requested.set()
        if self.path.endswith(".json"):
            data = json.dumps(server.meta).encode()
        else:
            data = server.data
            server.binary_requested.set()
        try:
            time.sleep(server.delay)
            self.send_response(200)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            if server.chunk_delay and not self.path.endswith(".json"):
                self.wfile.write(data[:1024]); self.wfile.flush()
                time.sleep(server.chunk_delay)
                self.wfile.write(data[1024:])
            else:
                self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
server.daemon_threads = True
server.guard = threading.Lock()
server.requested = threading.Event()
server.binary_requested = threading.Event()
server.paths = []
url = f"http://127.0.0.1:{server.server_port}/latest-dev/snajpagent-linux-x86_64"
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()

with tempfile.TemporaryDirectory(prefix="update-", dir=os.environ["TMPDIR"]) as root:
    root = Path(root)
    def reset(label, data=None, **changes):
        home = root / label
        home.mkdir(mode=0o700)
        executable = home / "snajpagent"
        shutil.copyfile(old, executable)
        executable.chmod(0o750)
        server.data = new.read_bytes() if data is None else data
        server.meta = dict(name="snajpagent", target="linux-x86_64", version="0.99.2-bbbbbbb",
                           url=url, sha256=hashlib.sha256(server.data).hexdigest(),
                           size=len(server.data), changelog="https://publisher.test/downloads.html#changelog")
        server.meta.update(changes)
        server.delay = server.chunk_delay = 0
        server.paths = []
        server.requested.clear(); server.binary_requested.clear()
        return executable
    def run(exe, timeout="10000"):
        result = subprocess.run([exe, url, timeout], capture_output=True, text=True, timeout=15)
        assert result.returncode == 0, result.stderr
        return result
    def unchanged(exe, result):
        assert exe.read_bytes() == old.read_bytes()
        assert "updated ===" not in result.stderr
        assert not (exe.parent / ".snajpagent.update-new").exists()
    exe = reset("success")
    server.delay = 0.4
    p = subprocess.Popen([exe, url, "10000"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    before = time.monotonic()
    assert p.stdout.readline().strip() == "running 0.99.2-aaaaaaa"
    assert time.monotonic() - before < 0.3
    out, err = p.communicate(timeout=15)
    assert p.returncode == 0 and "old process still running 0.99.2-aaaaaaa" in out
    assert err.count("=== snajpagent updated ===") == 1 and "#changelog" in err
    assert exe.read_bytes() == new.read_bytes() and exe.stat().st_mode & 0o777 == 0o750
    assert subprocess.check_output([exe, "-V"], text=True).strip() == "0.99.2-bbbbbbb"
    result = run(exe)
    assert not result.stderr and server.paths.count(url.split(str(server.server_port), 1)[1]) == 1
    print("PASS: background startup, real self-replacement, old process survives, one banner, permissions, next launch")
    for label, changes in [
        ("hash", dict(sha256="0" * 64)), ("size", dict(size=len(new.read_bytes()) + 1)),
        ("target", dict(target="windows-arm64")), ("name", dict(name="other")),
        ("old-version", dict(version="0.99.1")), ("same-version", dict(version="0.99.2-aaaaaaa")),
        ("version-invalid", dict(version="broken")), ("insecure", dict(url="http://example.com/update")),
    ]:
        exe = reset(label, **changes); unchanged(exe, run(exe))
    for index, bad in enumerate(["+0.99.2", "-1.99.2", "0.99.+2", "00.99.2", "0.99.2-a",
                                 "4294967296.99.2", "9" * 80 + ".1.2", "0.99.2-deadbeef-dirty"]):
        exe = reset(f"bad-version-{index}", version=bad); unchanged(exe, run(exe))
    for label, data in [("identity", new.read_bytes().replace(b"linux-x86_64", b"wrong-target")),
                        ("publisher", new.read_bytes().replace(b"https://publisher.test", b"https://wrongpubr.test"))]:
        exe = reset(label, data=data); unchanged(exe, run(exe))
    print("PASS: integrity, size, publisher/target, version and HTTPS failures leave executable intact")
    exe = reset("cancel"); server.delay = 1
    unchanged(exe, run(exe, "30"))
    time.sleep(1.1)
    exe = reset("concurrent"); server.delay = 0.2
    p = subprocess.Popen([exe, url, "10000"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert server.requested.wait(3)
    q = subprocess.Popen([exe, url, "10000"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    po, pe = p.communicate(timeout=15); qo, qe = q.communicate(timeout=15)
    assert p.returncode == q.returncode == 0 and (pe + qe).count("updated ===") == 1
    assert len(server.paths) == 2
    exe = reset("killed"); server.chunk_delay = 1
    p = subprocess.Popen([exe, url, "10000"], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    assert server.binary_requested.wait(3)
    p.kill(); p.communicate(timeout=3)
    assert exe.read_bytes() == old.read_bytes()
    server.chunk_delay = 0
    result = run(exe)
    assert "updated ===" in result.stderr and exe.read_bytes() == new.read_bytes()
    print("PASS: cancellation, concurrent instances and recovery after interrupted download")
    exe = reset("aside")
    shutil.copyfile(aside, exe)
    result = run(exe)
    assert "updated ===" in result.stderr and exe.read_bytes() == new.read_bytes()
    backup = exe.parent / ".snajpagent.update-old.exe"
    assert backup.read_bytes() == aside.read_bytes()
    exe = reset("aside-crash")
    shutil.copyfile(aside, exe)
    result = subprocess.run([exe, url, "10000"], capture_output=True,
                            env=dict(os.environ, SNAJPAGENT_TEST_RENAME_CRASH="1"), timeout=10)
    assert result.returncode == 79 and not exe.exists()
    backup = exe.parent / ".snajpagent.update-old.exe"
    assert backup.read_bytes() == aside.read_bytes()
    result = run(backup)
    assert not result.stderr and exe.read_bytes() == aside.read_bytes()
    result = run(exe)
    assert "updated ===" in result.stderr and exe.read_bytes() == new.read_bytes()
    print("PASS: rename-aside replacement and recovery after interrupted installation")
    exe = reset("local")
    shutil.copyfile(local, exe)
    result = run(exe)
    assert not result.stderr and not server.paths
    print("PASS: ordinary local build has no updater traffic")
server.shutdown(); server.server_close(); thread.join()
