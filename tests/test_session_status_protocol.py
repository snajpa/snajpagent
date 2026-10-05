#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The production list queries old owners without reserving their terminals."""
import contextlib
import fcntl
import hashlib
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import tmux_terminal as harness


STATUS = {2: 14, 3: 15, 4: 19, 5: 19}
BUSY = b"session already has a terminal or attachment reservation"


class Owner:
    def __init__(self, directory, version, reply):
        self.version = version
        self.reply = reply
        self.requests = []
        self.errors = []
        self.stop = threading.Event()
        self.lock = (directory / "lock").open("r+b")
        fcntl.lockf(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.listener = socket.socket(socket.AF_UNIX)
        # Relative bind also exercises session paths longer than sun_path.
        previous = Path.cwd()
        try:
            os.chdir(directory)
            self.listener.bind("terminal.sock")
            os.chmod("terminal.sock", 0o600)
        finally:
            os.chdir(previous)
        self.listener.listen(8)
        self.listener.settimeout(.1)
        self.thread = threading.Thread(target=self.serve)
        self.thread.start()

    def serve(self):
        try:
            while not self.stop.is_set():
                try:
                    peer, _ = self.listener.accept()
                except socket.timeout:
                    continue
                with peer:
                    peer.settimeout(2)
                    request = b""
                    while len(request) < 8:
                        chunk = peer.recv(8 - len(request))
                        if not chunk:
                            break
                        request += chunk
                    self.requests.append(request)
                    assert len(request) == 8 and request[:2] == b"SA", request
                    version, kind = request[2:4]
                    assert kind == STATUS[version] and request[4:] == b"\0" * 4, request
                    if version != self.version:
                        continue
                    if self.reply == "timeout":
                        self.stop.wait(2)
                        continue
                    if self.reply == "eof":
                        continue
                    kind = 10 if self.reply == "busy" else STATUS[version]
                    payload = BUSY if self.reply == "busy" else self.reply
                    frame = b"SA" + bytes([version, kind]) + struct.pack("<I", len(payload))
                    # Stream fragmentation must also work during legacy fallback.
                    try:
                        for byte in frame + payload:
                            peer.sendall(bytes([byte]))
                            time.sleep(.001)
                    except (BrokenPipeError, ConnectionResetError):
                        pass
        except Exception as error:
            self.errors.append(error)

    def close(self):
        self.stop.set()
        self.thread.join(3)
        self.listener.close()
        self.lock.close()
        assert not self.thread.is_alive(), "status fixture did not stop"
        assert not self.errors, self.errors


def check(binary):
    provider = harness.FakeResponses()
    provider.runtime_handler = lambda handler, request, sequence: provider.reply(
        handler, provider.response_body(sequence, "saved").encode())
    try:
        with tempfile.TemporaryDirectory(prefix="snag-status-") as tmp, contextlib.ExitStack() as stack:
            root = Path(tmp).resolve()
            config = root / "config.ini"
            harness.write_irc_config(config, provider.port, "host-model")
            state = root / "state"
            env = {**os.environ, "HOME": str(root), "SNAJPAGENT_IRC_UI_KEY": "irc-ui-secret"}
            prefix = [str(binary), "--config", str(config), "--dotdir", str(state)]
            cases = [(f"v{v}-{s}", v, bytes([s == "attached"]), s)
                     for v in STATUS for s in ("attached", "detached")]
            cases += [("old-busy", 2, "busy", "attached"),
                      ("invalid", 4, b"\x02", "running"),
                      ("empty", 4, b"", "running"),
                      ("closed", 4, "eof", "running"),
                      ("stalled", 4, "timeout", "running"),
                      ("stored", None, None, "stored")]
            expected = {}
            owners = []
            journals = {}
            for name, version, reply, status in cases:
                result = subprocess.run(prefix + ["-N", name, "-e", "--", "hello"],
                                        cwd=root, env=env, capture_output=True, timeout=20)
                assert result.returncode == 0, result.stderr
                new = set((state / "sessions").glob("*/events.jsonl")) - journals.keys()
                assert len(new) == 1, new
                journal = new.pop()
                journals[journal] = hashlib.sha256(journal.read_bytes()).digest()
                expected[name] = status
                if version:
                    owner = Owner(journal.parent, version, reply)
                    owners.append(owner)
                    stack.callback(owner.close)
            for count in (1, 0):
                started = time.monotonic()
                result = subprocess.run(prefix + ["-l", str(count)], cwd=root, env=env,
                                        text=True, capture_output=True, timeout=10)
                assert result.returncode == 0 and not result.stderr, result.stderr
                rows = [row.split("\t") for row in result.stdout.splitlines()[1:]]
                actual = {row[1]: row[4] for row in rows}
                wanted = {name: status for name, status in expected.items()
                          if count or status != "stored"}
                assert actual == wanted, (actual, wanted)
                ranks = [dict(attached=0, detached=1, running=2, stored=3)[row[4]] for row in rows]
                assert ranks == sorted(ranks), rows
                assert time.monotonic() - started < 5, "status retries multiplied the deadline"
            for owner in owners:
                assert owner.requests and not owner.errors, (owner.requests, owner.errors)
            assert all(hashlib.sha256(path.read_bytes()).digest() == digest
                       for path, digest in journals.items()), "listing changed saved history"
            print("session status: legacy/current, failures, grouping, count, read-only: ok")
    finally:
        provider.close()


if __name__ == "__main__":
    check(Path(sys.argv[1] if len(sys.argv) > 1 else "./snajpagent").resolve())
