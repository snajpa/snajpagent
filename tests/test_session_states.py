#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Production PTYs: attachment states and the displayed/short session selectors."""
import hashlib
import os
import select
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import tmux_terminal as harness
from store_history import journal_paths, read_events
from test_remote_terminal import RemoteProcess


def check(binary, previous=None):
    provider = harness.FakeResponses()
    provider.runtime_handler = lambda handler, request, sequence: provider.reply(
        handler, provider.response_body(sequence, "session state answer").encode(), close_header=True)
    children = []
    owners = {}

    def identity(pid):
        return subprocess.run(["ps", "-p", str(pid), "-o", "lstart=,command="],
                              text=True, capture_output=True).stdout.strip()

    try:
        with tempfile.TemporaryDirectory(prefix="snag-states-") as tmp:
            root = Path(tmp).resolve()
            config = root / "config.ini"
            harness.write_irc_config(config, provider.port, "host-model")
            prefix = [str(binary), "--config", str(config), "--dotdir", str(root / "state")]
            env = {"SNAJPAGENT_IRC_UI_KEY": "irc-ui-secret"}

            def start(args, marker, executable=None):
                command = [prefix[0], *prefix[3:]] if any(
                    arg in ("-A", "--attach") for arg in args) else prefix
                if executable:
                    command = [str(executable), *command[1:]]
                child = RemoteProcess(root, command + args, wrapped=None, extra_env=env)
                children.append(child)
                child.until(marker, 15)
                # Track only fixture owners while still parented by our frontend.
                for row in subprocess.check_output(
                        ["ps", "-axo", "pid=,ppid=,command="], text=True).splitlines():
                    fields = row.split(None, 2)
                    if len(fields) == 3 and fields[1] == str(child.process.pid) and str(config) in row:
                        pid = int(fields[0])
                        owners[pid] = identity(pid)
                return child

            def finish(child, command):
                os.write(child.master, command + b"\r")
                deadline = time.monotonic() + 10
                while child.process.poll() is None and time.monotonic() < deadline:
                    if select.select([child.master], [], [], .05)[0]:
                        child.output.extend(os.read(child.master, 65536))
                assert child.process.poll() == 0, bytes(child.output)

            def journals():
                return journal_paths(root / "state")

            def state(sid, expected):
                saved = {p: hashlib.sha256(p.read_bytes()).digest() for p in journals()}
                result = subprocess.run(prefix + ["-l"], cwd=root, env={**os.environ, **env},
                                        capture_output=True, text=True, timeout=10)
                assert result.returncode == 0 and not result.stderr, result.stderr
                rows = [line.split("\t") for line in result.stdout.splitlines()[1:]]
                row = next(row for row in rows if sid.startswith(row[0]))
                assert row[4] == expected, row
                assert all(hashlib.sha256(p.read_bytes()).digest() == digest
                           for p, digest in saved.items()), "listing changed a journal"
                return row[0]

            first = start([], "›".encode(), previous)
            sid = journals()[0].parent.name
            displayed = state(sid, "attached")
            # Listing inside the owner must not deadlock or drop its writer lock.
            os.write(first.master, b"/s\r")
            first.until(b"running sessions:" if previous else b"attached", 10)
            finish(first, b"/s d")
            state(sid, "detached")
            for args in (["-A", displayed], ["--attach", sid[:1]], ["--resume", sid[:4]]):
                child = start(args, b"Attached session")
                state(sid, "attached")
                finish(child, b"/s d")
                state(sid, "detached")
            for option in ("--attach", "--resume"):
                child = start([option], "session › ".encode())
                os.write(child.master, sid[:4].encode() + b"\r")
                child.until(b"Attached session", 10)
                state(sid, "attached")
                finish(child, b"/s d")

            second = start([], "›".encode())
            other = next(p.parent.name for p in journals() if p.parent.name != sid)
            os.write(second.master, f"/s a {sid[:4]}\r".encode())
            second.until(b"Attached session", 10)
            state(sid, "attached")
            state(other, "detached")
            finish(second, b"/exit")
            state(sid, "stored")
            child = start(["-A", other[:8]], b"Attached session")
            finish(child, b"/exit")
            state(other, "stored")
            result = subprocess.run(prefix + ["--resume", sid[:4], "-e", "--", "ping"],
                                    cwd=root, env={**os.environ, **env}, capture_output=True, timeout=20)
            assert result.returncode == 0, result.stderr
            records = read_events(next(p for p in journals() if p.parent.name == sid))
            assert [e["data"]["text"] for e in records if e["type"] == "turn_started"] == ["ping"]

            # Interleave live states, then create newer stored sessions. Running
            # sessions must all stay ahead of even the most recently saved work.
            statuses = {p.parent.name: "stored" for p in journals()}
            attached = []
            for name, status in (("attached-old", "attached"), ("detached-old", "detached"),
                                 ("attached-new", "attached"), ("detached-new", "detached")):
                child = start(["-N", name], "›".encode())
                live_id = next(p.parent.name for p in journals() if p.parent.name not in statuses)
                if status == "detached":
                    finish(child, b"/s d")
                else:
                    attached.append(child)
                statuses[live_id] = status
            for number in range(12):
                result = subprocess.run(prefix + ["-N", f"stored-{number}", "-e", "--", "ping"],
                                        cwd=root, env={**os.environ, **env},
                                        capture_output=True, timeout=20)
                assert result.returncode == 0, result.stderr
                new_id = next(p.parent.name for p in journals() if p.parent.name not in statuses)
                statuses[new_id] = "stored"
            # An old session with new activity belongs at the top of stored rows.
            result = subprocess.run(prefix + ["--resume", sid, "-e", "--", "recent activity"],
                                    cwd=root, env={**os.environ, **env},
                                    capture_output=True, timeout=20)
            assert result.returncode == 0, result.stderr
            saved = {p: p.read_bytes() for p in journals()}
            times = {p.parent.name: read_events(p)[-1]["time_ms"] for p in saved}
            ordered = {status: sorted((key for key in statuses if statuses[key] == status),
                                     key=lambda key: (times[key], key), reverse=True)
                       for status in ("attached", "detached", "stored")}
            assert ordered["stored"][0] == sid

            def listing(args, limit):
                result = subprocess.run(prefix + args, cwd=root, env={**os.environ, **env},
                                        capture_output=True, text=True, timeout=10)
                assert result.returncode == 0 and not result.stderr, result.stderr
                lines = result.stdout.splitlines()
                rows = [line.split("\t") for line in lines[1:]]
                expected = ordered["attached"] + ordered["detached"] + ordered["stored"][:limit]
                actual = [next(key for key in statuses if key.startswith(row[0])) for row in rows]
                assert actual == expected, (args, actual, expected)
                assert lines[0].split("\t")[4] == "STATUS", lines[0]
                assert [row[4] for row in rows] == [statuses[key] for key in expected]

            listing(["-l"], 10)
            listing(["-l", "0"], 0)
            listing(["-l", "1"], 1)
            listing(["-l", "3", "--no-color"], 3)
            listing(["-l3"], 3)
            listing(["-l", "14"], 14)
            listing(["-l", "18446744073709551615"], 14)
            for args in (["-l", "nope"], ["-l", "-1"], ["-l", "1.5"], ["-l", ""],
                         ["-l", "1", "2"], ["-l", "3", "-l"], ["-l", "3", "-e"]):
                result = subprocess.run(prefix + args, cwd=root, env={**os.environ, **env},
                                        capture_output=True, timeout=10)
                assert result.returncode != 0 and result.stderr, args
            assert all(p.read_bytes() == data for p, data in saved.items()), "listing changed history"

            # The complete saved-session picker and slash list retain older rows.
            picker = start(["--resume"], "session › ".encode())
            for key in statuses:
                assert key[:8].encode() in picker.output, (key, bytes(picker.output))
            picker.close()
            attached[0].output.clear()
            os.write(attached[0].master, b"/s l\r")
            for key in statuses:
                attached[0].until(key[:8].encode(), 10)
            for child in attached:
                finish(child, b"/exit")
            assert not provider.failure, provider.failure
            print("session states, CLI/picker/slash short IDs and read-only listing: ok", flush=True)
            print("session list status order, recent stored counts and argument validation: ok", flush=True)
    finally:
        for pid, saved in owners.items():
            if identity(pid) == saved:
                os.kill(pid, signal.SIGTERM)
        try:
            for child in children:
                child.close()
        finally:
            provider.close()


if __name__ == "__main__":
    check(Path(sys.argv[1] if len(sys.argv) > 1 else "./snajpagent").resolve(),
          Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None)
