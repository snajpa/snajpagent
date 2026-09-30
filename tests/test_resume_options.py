#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Resume needs only a selector and a nondefault state directory."""
import json
import os
import shlex
import select
import signal
import socket
import time
import subprocess
import sys
import tempfile
from pathlib import Path

import tmux_terminal as harness
from test_remote_terminal import RemoteProcess


def check(binary):
    provider = harness.FakeResponses()
    provider.runtime_handler = lambda handler, request, sequence: provider.reply(
        handler, provider.response_body(sequence, "resume options answer").encode(), close_header=True)
    try:
        with tempfile.TemporaryDirectory(prefix="snag-resume-options-") as tmp:
            root = Path(tmp).resolve()
            home = root / "home"
            home.mkdir()
            docs = root / "docs 'quoted"
            docs.mkdir()
            (docs / "AGENTS.md").write_text("Saved resume instruction marker.\n")
            config = root / "config 'quoted.ini"
            harness.write_irc_config(config, provider.port, "host-model")
            env = {**os.environ, "HOME": str(home), "SNAJPAGENT_IRC_UI_KEY": "irc-ui-secret"}
            for default in (False, True):
                state = home / ".snajpagent" if default else root / "state 'quoted"
                args = [str(binary), "--dotdir", str(state) + ("/." if default else "/"),
                        "--config", str(config),
                        "--no-color", "--no-markdown", "-vv", "-d", str(docs), "-e", "--", "ping"]
                result = subprocess.run(args, cwd=root, env=env, capture_output=True,
                                        text=True, timeout=20)
                assert result.returncode == 0, result.stderr
                sid = next((state / "sessions").iterdir()).name
                command = shlex.split(result.stderr.splitlines()[-1])
                expected = [str(binary)] + ([] if default else ["--dotdir", str(state)])
                assert command == expected + ["--resume", sid], command
                journal = state / "sessions" / sid / "events.jsonl"
                before = journal.read_bytes()
                assert b"irc-ui-secret" not in before
                result = subprocess.run([*command, "-e", "--", "again"], cwd=home, env=env,
                                        capture_output=True, text=True, timeout=20)
                assert result.returncode == 0, result.stderr
                assert shlex.split(result.stderr.splitlines()[-1]) == command
                turns = [e["data"] for e in map(json.loads, journal.read_text().splitlines())
                         if e["type"] == "turn_started"]
                assert len(turns) == 2, turns
                assert str(docs / "AGENTS.md") in turns[-1]["instructions"], turns[-1]
                assert journal.read_bytes().startswith(before)
                if not default:
                    for _ in range(20):
                        records = list(map(json.loads, journal.read_text().splitlines()))
                        if any(e["type"] == "session_checkpoint" for e in records):
                            break
                        result = subprocess.run([*command, "-e", "--", "checkpoint"], cwd=home,
                                                env=env, capture_output=True, text=True, timeout=20)
                        assert result.returncode == 0, result.stderr
                    else:
                        raise AssertionError("fixture never created a checkpoint")
                    result = subprocess.run([*command, "-e", "--", "after checkpoint"], cwd=home,
                                            env=env, capture_output=True, text=True, timeout=20)
                    assert result.returncode == 0, result.stderr
                    records = list(map(json.loads, journal.read_text().splitlines()))
                    turn = [e["data"] for e in records if e["type"] == "turn_started"][-1]
                    assert str(docs / "AGENTS.md") in turn["instructions"], turn
            replacement = root / "replacement docs"
            replacement.mkdir()
            (replacement / "AGENTS.md").write_text("Replacement resume instruction marker.\n")
            (docs / "AGENTS.md").unlink()
            for options in (["-d", str(replacement)], []):
                result = subprocess.run([*command, *options, "-e", "--", "replaced docs"],
                                        cwd=home, env=env, capture_output=True, text=True, timeout=20)
                assert result.returncode == 0, result.stderr
                records = list(map(json.loads, journal.read_text().splitlines()))
                turn = [e["data"] for e in records if e["type"] == "turn_started"][-1]
                assert str(replacement / "AGENTS.md") in turn["instructions"], turn
                assert str(docs / "AGENTS.md") not in turn["instructions"], turn
            assert not provider.failure, provider.failure
            print("resume config/instructions/preferences and minimal default/custom-dotdir hints: ok")
    finally:
        provider.close()


def check_network(binary, previous=None):
    provider = harness.FakeResponses()
    children = []
    owners = {}
    sockets = []

    def identity(pid):
        return subprocess.run(["ps", "-p", str(pid), "-o", "lstart=,command="],
                              text=True, capture_output=True).stdout.strip()

    def endpoint():
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        sock.listen(16)
        sockets.append(sock)
        return f"127.0.0.1:{sock.getsockname()[1]}"

    try:
        with tempfile.TemporaryDirectory(prefix="snag-resume-irc-") as tmp:
            root = Path(tmp).resolve()
            config = root / "custom.ini"
            harness.write_irc_config(config, provider.port, "host-model")
            state = root / "state"
            base = [str(binary), "--dotdir", str(state)]
            env = {"SNAJPAGENT_IRC_UI_KEY": "irc-ui-secret"}
            clients = [endpoint(), endpoint(), endpoint()]
            hosted = endpoint()
            sockets.pop().close()

            def start(args, marker, executable=None):
                command = [str(executable or binary), *base[1:], *args]
                child = RemoteProcess(root, command, wrapped=None, extra_env=env)
                children.append(child)
                child.until(marker, 15)
                for row in subprocess.check_output(
                        ["ps", "-axo", "pid=,ppid=,command="], text=True).splitlines():
                    fields = row.split(None, 2)
                    if len(fields) == 3 and fields[1] == str(child.process.pid) and str(state) in row:
                        pid = int(fields[0])
                        owners[pid] = identity(pid)
                return child

            def finish(child):
                os.write(child.master, b"/exit\r")
                deadline = time.monotonic() + 10
                while child.process.poll() is None and time.monotonic() < deadline:
                    if select.select([child.master], [], [], .05)[0]:
                        child.output.extend(os.read(child.master, 65536))
                assert child.process.poll() == 0, bytes(child.output)

            def topology(expected):
                result = subprocess.run([*base, "-l"], cwd=root, env={**os.environ, **env},
                                        text=True, capture_output=True, timeout=10)
                assert result.returncode == 0, result.stderr
                row = result.stdout.splitlines()[1].split("\t")
                assert row[6] == expected, row

            first = start(["--config", str(config), "-s", hosted, "-c", clients[0],
                           "-c", clients[1], "-n", "saved", "-o", "resumeop",
                           "-r", "savedroom", "--no-color"], b"resumeop@", previous)
            sid = next((state / "sessions").iterdir()).name
            original = f"s/saved@{hosted},c/saved@{clients[0]},c/saved@{clients[1]}"
            topology(original)
            finish(first)
            # Keeping --config here isolates the legacy IRC bug; later resumes
            # omit it too, including for sessions created by an older executable.
            resumed = start(["--resume", sid, "--config", str(config)], b"session id")
            topology(original)
            resumed.until(b"resumeop@", 10)
            finish(resumed)
            resumed = start(["--resume", sid, "-c", clients[2], "-o", "changedop"], b"changedop@")
            topology(f"s/saved@{hosted},c/saved@{clients[2]}")
            finish(resumed)
            resumed = start(["--resume", sid], b"changedop@")
            topology(f"s/saved@{hosted},c/saved@{clients[2]}")
            finish(resumed)
            stopped = start(["--resume", sid, "--no-listen", "--no-client"], "›".encode())
            topology("-")
            finish(stopped)
            # Saved disabled roles must win over newly enabled config defaults.
            with config.open("a") as out:
                out.write(f"\n[irc]\nlisten={hosted}\nclient={clients[0]}\n")
            stopped = start(["--resume", sid], "›".encode())
            topology("-")
            finish(stopped)
            print("resume IRC inheritance, per-role overrides, disabled roles and legacy topology: ok")
    finally:
        for pid, saved in owners.items():
            if identity(pid) == saved:
                os.kill(pid, signal.SIGTERM)
        try:
            for child in children:
                if child.slave is not None:
                    os.close(child.slave)
                    child.slave = None
                child.close()
        finally:
            for sock in sockets:
                sock.close()
            provider.close()


if __name__ == "__main__":
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else "./snajpagent").resolve()
    check(binary)
    check_network(binary, Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None)
