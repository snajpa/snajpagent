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
from store_history import create_legacy, journal_paths, read_events
from test_remote_terminal import RemoteProcess
from test_session_listing import append_event


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
                journal, = journal_paths(state)
                before = journal.read_bytes()
                assert b"irc-ui-secret" not in before
                result = subprocess.run([*command, "-e", "--", "again"], cwd=home, env=env,
                                        capture_output=True, text=True, timeout=20)
                assert result.returncode == 0, result.stderr
                assert shlex.split(result.stderr.splitlines()[-1]) == command
                turns = [e["data"] for e in read_events(journal)
                         if e["type"] == "turn_started"]
                assert len(turns) == 2, turns
                assert str(docs / "AGENTS.md") in turns[-1]["instructions"], turns[-1]
                assert journal.read_bytes().startswith(before)
                if not default:
                    for _ in range(20):
                        records = read_events(journal)
                        checkpoint = ((journal.parent / "checkpoint.1").stat().st_size > 0
                                      if journal.name == "journal.bin" else
                                      any(e["type"] == "session_checkpoint" for e in records))
                        if checkpoint:
                            break
                        result = subprocess.run([*command, "-e", "--", "checkpoint"], cwd=home,
                                                env=env, capture_output=True, text=True, timeout=20)
                        assert result.returncode == 0, result.stderr
                    else:
                        raise AssertionError("fixture never created a checkpoint")
                    result = subprocess.run([*command, "-e", "--", "after checkpoint"], cwd=home,
                                            env=env, capture_output=True, text=True, timeout=20)
                    assert result.returncode == 0, result.stderr
                    records = read_events(journal)
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
                records = read_events(journal)
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


def check_hosted_rename_resume(binary):
    provider = harness.FakeResponses()

    def respond(handler, request, sequence):
        renamed = any(i.get("type") == "function_call_output" and
                      i.get("call_id") == "rename-model" for i in request["input"])
        if provider.latest_user(request) == "rename model" and not renamed:
            wire = provider.function_body(sequence, "rename-model", "irc_nick",
                                          {"destination": None, "nick": "after"})
        else:
            wire = provider.response_body(sequence, "rename observed")
        provider.reply(handler, wire.encode(), close_header=True)

    provider.runtime_handler = respond
    try:
        with tempfile.TemporaryDirectory(prefix="snag-resume-nick-") as tmp:
            root = Path(tmp).resolve()
            config = root / "config.ini"
            harness.write_irc_config(config, provider.port, "host-model")
            with config.open("a") as out:
                out.write("prompt = {chat:READY>}{rollout-idle:READY>}{rollout-active:WORK>}\n")
            state = root / "state"
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                hosted = f"127.0.0.1:{sock.getsockname()[1]}"
            base = [str(binary), "--dotdir", str(state)]
            env = {"SNAJPAGENT_IRC_UI_KEY": "irc-ui-secret"}
            # The invalid-nickname case appends a correctly hashed JSONL envelope.
            journal = create_legacy(state, root, "fake", "host-model")
            sid = journal.parent.name
            before = b""
            for options in (None, [], ["--no-listen", "--no-client"]):
                args = (["--resume", sid, "--config", str(config), "-s", hosted, "--no-client",
                         "-n", "before", "-o", "renameop", "-r", "lab", "--no-color"]
                        if options is None else ["--resume", sid, *options])
                child = RemoteProcess(root, [*base, *args], wrapped=None, extra_env=env)
                try:
                    child.until(b"READY>", 10)
                    sid = next((state / "sessions").iterdir()).name
                    journal, = journal_paths(state)
                    assert journal.read_bytes().startswith(before)
                    if options is None:
                        os.write(child.master, b"/nick operatorafter\r/nick\r")
                        child.until(b"operator nick: operatorafter", 10)
                        assert b"model nick: before" in child.output
                        child.output.clear()
                        os.write(child.master, b"/rollout\rrename model\r")
                        child.until(b"observed", 10)
                    child.output.clear()
                    os.write(child.master, b"/nick\r")
                    child.until(b"model nick: after", 10)
                    child.until(b"operator nick: operatorafter", 10)
                    child.output.clear()
                    os.write(child.master, b"/exit\r")
                    child.until(b"--resume", 10)
                    child.wait(0)
                    before = journal.read_bytes()
                    renames = [e["data"] for e in read_events(journal)
                               if e["type"] in ("irc_event", "irc_event_v2") and
                               e["data"]["kind"] == "nick"]
                    assert any(e["local"] and e["nick"] == "before" and e["text"] == "after"
                               for e in renames), renames
                    assert any(e["local"] and e["nick"] == "renameop" and
                               e["text"] == "operatorafter" for e in renames), renames
                finally:
                    child.close()
            # A correctly hashed envelope still needs IRC shape validation.
            invalid = dict(renames[-1], text="invalid nick")
            seq = read_events(journal)[-1]["seq"] + 1
            append_event(journal, "irc_event_v2", invalid)
            child = RemoteProcess(root, [*base, "--resume", sid], wrapped=None, extra_env=env)
            try:
                child.until(f"invalid irc_event_v2 transition at sequence {seq}".encode(), 10)
                child.until(b"--resume", 10)
                child.wait(3)
            finally:
                child.close()
            assert not provider.failure, provider.failure
            print("resume hosted nickname history with active and disabled networking: ok")
    finally:
        provider.close()


def check_hosted_peer_reuse(binary):
    provider = harness.FakeResponses()
    try:
        with tempfile.TemporaryDirectory(prefix="snag-resume-peer-") as tmp:
            root = Path(tmp).resolve()
            config = root / "config.ini"
            harness.write_irc_config(config, provider.port, "host-model")
            with config.open("a") as out:
                out.write("prompt = {chat:READY>}{rollout-idle:READY>}{rollout-active:WORK>}\n")
            state = root / "state"
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            base = [str(binary), "--dotdir", str(state)]
            sid = None
            for attempt in range(3):
                args = (["--config", str(config), "-s", f"127.0.0.1:{port}",
                         "--no-client", "-n", "host", "-o", "operator", "-r", "lab"]
                        if sid is None else ["--resume", sid])
                child = RemoteProcess(root, [*base, *args], wrapped=None,
                                      extra_env={"SNAJPAGENT_IRC_UI_KEY": "irc-ui-secret"})
                peer = None
                try:
                    child.until(b"READY>", 10)
                    sid = next((state / "sessions").iterdir()).name
                    if attempt < 2:
                        peer = socket.create_connection(("127.0.0.1", port), timeout=5)
                        name = "previous" if attempt == 0 else "newpeer"
                        peer.sendall(f"NICK {name}\r\nUSER peer 0 * :fixture\r\n"
                                     "JOIN #lab\r\n".encode())
                        wire = b""
                        while b" 366 " not in wire:
                            wire += peer.recv(8192)
                        if attempt == 1:
                            peer.sendall(b"NICK previous\r\nPING :renamed\r\n")
                            wire = b""
                            while b"PONG" not in wire:
                                wire += peer.recv(8192)
                            assert b" 433 " not in wire, wire
                    child.output.clear()
                    # Keep the peer connected until host exit: the next owner
                    # must clear its stale membership at the connection boundary.
                    os.write(child.master, b"/exit\r")
                    child.until(b"--resume", 10)
                    child.wait(0)
                finally:
                    if peer is not None:
                        peer.close()
                    child.close()
            records = harness.read_events(state)[1]
            assert any(e["type"] == "irc_event_v2" and
                       e["data"]["kind"] == "nick" and
                       e["data"]["nick"] == "newpeer" and
                       e["data"]["text"] == "previous" for e in records)
            assert not provider.failure, provider.failure
            print("resume after hosted peer nickname reuse: ok")
    finally:
        provider.close()


def check_display_preferences(binary):
    provider = harness.FakeResponses()
    provider.runtime_handler = lambda handler, request, sequence: provider.reply(
        handler, provider.response_body(sequence, "render **bold marker** final-marker").encode(),
        close_header=True)
    try:
        with tempfile.TemporaryDirectory(prefix="snag-resume-display-") as tmp:
            root = Path(tmp).resolve()
            config = root / "config.ini"
            harness.write_irc_config(config, provider.port, "host-model")
            with config.open("a") as out:
                out.write("markdown = true\n"
                          "prompt = {chat::}{rollout-idle:READY>}{rollout-active:WORK>}\n")
            state = root / "state"
            base = [str(binary), "--config", str(config), "--dotdir", str(state)]
            env = {"SNAJPAGENT_IRC_UI_KEY": "irc-ui-secret", "NO_COLOR": None}
            sid = None
            cases = ((["--color=always", "--no-markdown", "-vv"], True, False, 2),
                     ([], True, False, 2),
                     (["--color=never", "--markdown", "-v"], False, True, 1),
                     ([], False, True, 1))
            for index, (options, colored, markdown, verbosity) in enumerate(cases):
                if index == 3:
                    # Saved choices must still win after the defaults change.
                    config.write_text(config.read_text().replace("color = never", "color = always")
                                      .replace("markdown = true", "markdown = false"))
                args = base + (["--resume", sid] if sid else []) + options
                child = RemoteProcess(root, args, wrapped=None, extra_env=env,
                                      winsize=(24, 120))
                try:
                    child.until(b"READY>", 10)
                    child.output.clear()
                    os.write(child.master, b"ping\r")
                    output = child.until(b"final-marker", 10)
                    child.until_after(b"final-marker", b"READY>", 10)
                    records = harness.read_events(state)[1]
                    assert len(harness.event_list(records, "turn_completed")) == index + 1
                    assert (b"**bold marker**" in output) == (not markdown), output
                    child.output.clear()
                    os.write(child.master, b"/verbose\r")
                    child.until(f"verbosity: {verbosity} (".encode(), 10)
                    child.until(b"READY>", 10)
                    child.output.clear()
                    os.write(child.master, b"/exit\r")
                    output = child.until(b"--resume", 10)
                    header = "• You can resume this session with the following command:".encode()
                    assert (b"\x1b[1;32m" + header + b"\x1b[0m" in output) == colored, output
                    child.wait(0)
                    sid = next((state / "sessions").iterdir()).name
                finally:
                    child.close()
            assert not provider.failure, provider.failure
            print("resume color, Markdown and verbosity: saved, overridden and inherited: ok")
    finally:
        provider.close()


if __name__ == "__main__":
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else "./snajpagent").resolve()
    check(binary)
    check_network(binary, Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None)
    check_hosted_rename_resume(binary)
    check_hosted_peer_reuse(binary)
    check_display_preferences(binary)
