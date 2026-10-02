#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Session tables keep saved text on one bounded, terminal-safe row."""
import hashlib
import json
import os
import fcntl
import pty
import select
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unicodedata
from pathlib import Path

import tmux_terminal as harness


HEADER = ["SESSION", "NAME", "MODEL", "TURNS", "STATUS", "LAST PROMPT", "IRC"]


def canonical(value):
    if isinstance(value, str):
        return '"' + "".join(f"\\u{ord(c):04x}" if ord(c) < 32 else
                              "\\" + c if c in '\\"' else c for c in value) + '"'
    if isinstance(value, dict):
        return "{" + ",".join(canonical(k) + ":" + canonical(value[k])
                              for k in sorted(value)) + "}"
    if isinstance(value, list):
        return "[" + ",".join(map(canonical, value)) + "]"
    return json.dumps(value, separators=(",", ":"))


def append_event(path, kind, data):
    previous = json.loads(path.read_bytes().splitlines()[-1])
    event = dict(data=data, prev_sha256=previous["event_sha256"], seq=previous["seq"] + 1,
                 session_id=previous["session_id"], time_ms=previous["time_ms"] + 1,
                 type=kind, v=previous["v"])
    if "checkpoint_offset" in previous:
        event["checkpoint_offset"] = previous["checkpoint_offset"]
    def encode():
        return canonical(event).encode()
    event["event_sha256"] = hashlib.sha256(encode()).hexdigest()
    with path.open("ab") as out:
        out.write(encode() + b"\n")


def terminal_list(prefix, root, env, columns):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, columns, 0, 0))
    child = subprocess.Popen(prefix + ["-l"], cwd=root, env=env, stdin=subprocess.DEVNULL,
                             stdout=slave, stderr=slave)
    os.close(slave)
    output = bytearray()
    deadline = time.monotonic() + 15
    try:
        while time.monotonic() < deadline:
            if not select.select([master], [], [], 0.1)[0]:
                if child.poll() is not None:
                    break
                continue
            try:
                chunk = os.read(master, 65536)
            except OSError:
                break
            if not chunk:
                break
            output.extend(chunk)
        assert child.wait(timeout=2) == 0, output
    finally:
        if child.poll() is None:
            child.kill()
            child.wait()
        os.close(master)
    return output.decode().splitlines()


def cells(text):
    return sum(0 if unicodedata.combining(c) else
               2 if unicodedata.east_asian_width(c) in "WF" else 1 for c in text)


def check_listing(binary):
    provider = harness.FakeResponses()

    def respond(handler, request, sequence):
        provider.reply(handler, provider.response_body(sequence, "saved").encode())

    provider.runtime_handler = respond
    try:
        with tempfile.TemporaryDirectory(prefix="snag-list-") as tmp:
            root = Path(tmp).resolve()
            config = root / "config.ini"
            harness.write_irc_config(config, provider.port, "host-model")
            state = root / "state"
            env = {**os.environ, "HOME": str(root), "SNAJPAGENT_IRC_UI_KEY": "irc-ui-secret"}
            prefix = [str(binary), "--config", str(config), "--dotdir", str(state)]
            cases = [
                ("IRC history", "[IRC endpoint=localhost:6667 room=#lab event=history_ready]\n" +
                 "[IRC endpoint=localhost:6667 room=#lab event=join sender=peer]\n" * 30 + "replayed"),
                ("multiline", "first line\nsecond\tcolumn\rreturn\vvertical\fform feed"),
                ("controls", "escape\x1b[31m red\x1b[0m\x07 bell\u009b31m\u2028line\u2029paragraph"),
                ("Unicode", "a" * 79 + "界😀žluťoučký"),
                ("large", "ž" * 40000),
                ("name\u009b\u2028control", "ordinary prompt"),
            ]
            journals = {}
            for name, prompt in cases:
                result = subprocess.run(prefix + ["-N", name, "-e", "--", prompt],
                                        cwd=root, env=env, capture_output=True, timeout=20)
                assert result.returncode == 0, result.stderr
                paths = set((state / "sessions").glob("*/events.jsonl")) - set(journals)
                assert len(paths) == 1, paths
                journal = paths.pop()
                events = [json.loads(line) for line in journal.read_text().split("\n") if line]
                assert next(e["data"]["text"] for e in events if e["type"] == "turn_started") == prompt
                journals[journal] = hashlib.sha256(journal.read_bytes()).hexdigest()
                result = subprocess.run(prefix + ["-l"], cwd=root, env=env,
                                        capture_output=True, timeout=10)
                assert result.returncode == 0, result.stderr
                assert not result.stderr, result.stderr
                lines = result.stdout.decode().splitlines()
                assert lines[0].split("\t") == HEADER
                assert len(lines) == len(journals) + 1, (name, len(lines), len(journals))
                for line in lines[1:]:
                    fields = line.split("\t")
                    assert len(fields) == 7, fields
                    for field in (fields[1], fields[2], fields[5], fields[6]):
                        assert cells(field) <= 80, field
                        assert not any(ord(c) < 32 or 127 <= ord(c) <= 159 or
                                       c in "\u2028\u2029" for c in field), repr(field)
                own = next(line.split("\t") for line in lines if line.startswith(journal.parent.name[:8]))
                if name == "multiline":
                    assert own[5] == "first line second column return vertical form feed", own
                if name == "Unicode":
                    assert own[5] == "a" * 79 + "…", own
                if name == "IRC history":
                    assert own[5] == "localhost:6667: replayed", own
                if name == "large":
                    assert own[5].endswith("…"), own
                assert all(hashlib.sha256(path.read_bytes()).hexdigest() == digest
                           for path, digest in journals.items()), "listing changed saved history"

            # Use a real completed session, then append valid old-format IRC records.
            # The list must resolve the exact referenced input, not newer unrelated chat.
            path = next(p for p in journals if any(e.get("data", {}).get("name") == "multiline"
                        for e in map(json.loads, p.read_text().splitlines())))
            stream = "a" * 32
            def event(sequence, message):
                return dict(endpoint="localhost:6667", historical=False, kind="message",
                            local=False, nick="peer", op=False, room="#lab", text=message,
                            timestamp_ms=1, stream=stream, sequence=sequence, input=True)
            for sequence, message in [(1, "earlier message"), (2, "latest actual message")]:
                append_event(path, "irc_event", event(sequence, message))
            topology = ("[IRC room snapshot; @ marks a channel operator]\n"
                        "model nick: agent\noperator nick: op\nhosted: localhost:6667\n"
                        "destination[2]: client.example:7777\n"
                        "aliases[client.example:7777]: model side operator sideop\n"
                        "destination[1]: localhost:6667\n"
                        "destination[3]: other.example:8888\n")
            append_event(path, "irc_snapshot", dict(reason="topology", text=topology, timestamp_ms=1))
            prompt = "".join(f"[IRC update id={stream}:{i} endpoint=localhost:6667 "
                             "room=#lab event=message sender=peer]\n" for i in (1, 2))
            result = subprocess.run(prefix + ["--resume", path.parent.name, "-e", "--", prompt],
                                    cwd=root, env=env, capture_output=True, timeout=20)
            assert result.returncode == 0, result.stderr
            append_event(path, "irc_event", event(3, "unrelated later event"))
            journals[path] = hashlib.sha256(path.read_bytes()).hexdigest()
            result = subprocess.run(prefix + ["-l"], cwd=root, env=env, capture_output=True, timeout=10)
            rows = [line.split("\t") for line in result.stdout.decode().splitlines()[1:]]
            own = next(row for row in rows if row[0] == path.parent.name[:8])
            assert own[5] == "localhost:6667: latest actual message", own
            expected = "s/agent@localhost:6667,c/side@client.example:7777,c/agent@other.example:8888"
            assert own[6] == expected, own
            for width in (32, 40, 60, 80, 120, 200, 240):
                lines = terminal_list(prefix, root, env, width)
                assert len(lines) == len(journals) + 1, lines
                assert all(cells(line) <= width and "\t" not in line for line in lines), lines
                assert all(len(line.split()[0]) >= 8 and
                           all(c in "0123456789abcdef" for c in line.split()[0])
                           for line in lines[1:]), lines
                if width < 60:
                    continue
                prompt_start = lines[0].index("LAST PROMPT")
                irc_start = lines[0].index("IRC")
                assert abs((irc_start - prompt_start - 2) - (width - irc_start)) <= 1, lines[0]
                if width == 240:
                    own = next(line for line in lines[1:] if line.startswith(path.parent.name[:8]))
                    assert "localhost:6667: latest actual message" in own, own
                    assert expected in own, own
            assert all(hashlib.sha256(p.read_bytes()).hexdigest() == digest
                       for p, digest in journals.items()), "terminal listing changed history"
            # Prefer endpoint aliases, retain legacy nicks, and never read history
            # as identity/topology metadata. An unaccepted alias stays unknown.
            snapshots = [
                ("model nick: legacy\noperator nick: op\nhosted: no\n"
                 "endpoint[[::1]:6667]: joined #lab\n"
                 "aliases[[::1]:6667]: model accepted operator op\n",
                 "c/accepted@[::1]:6667"),
                ("model nick: first\nhosted: host:7\n"
                 "destination[1]: client:8\n"
                 "aliases[client:8]: model first operator op\n"
                 "destination[2]: host:7\n"
                 "aliases[host:7]: model hosted operator hostop\n",
                 "s/hosted@host:7,c/first@client:8"),
                ("model nick: first\nhosted: no\ndestination[1]: pending:7\n"
                 "aliases[pending:7]: model  operator \n",
                 "c/pending:7"),
                ("hosted: no\nendpoint[old:7]: joined #lab\nhistory:\n"
                 "model nick: spoofed\nhosted: fake:9\ndestination[1]: fake:9\n"
                 "aliases[old:7]: model spoofed operator op\n",
                 "c/old:7"),
                ("model nick: oldnick\nhosted: no\ndestination[1]: one:7\n",
                 "c/oldnick@one:7"),
                ("model nick: renamed\nhosted: no\ndestination[1]: one:7\n",
                 "c/renamed@one:7"),
                ("model nick: saved\nhosted: no\nno active endpoints\n", "-"),
            ]
            for metadata, expected in snapshots:
                append_event(path, "irc_snapshot", dict(reason="nick", timestamp_ms=2,
                             text="[IRC room snapshot; @ marks a channel operator]\n" + metadata))
                saved = path.read_bytes()
                listed = subprocess.check_output(prefix + ["-l"], cwd=root, env=env).decode()
                own = next(line.split("\t") for line in listed.splitlines()
                           if line.startswith(path.parent.name[:8]))
                assert own[6] == expected, own
                assert path.read_bytes() == saved, "nick listing changed history"
            append_event(path, "irc_snapshot", dict(reason="topology", timestamp_ms=2,
                         text="[IRC room snapshot; @ marks a channel operator]\nhosted: no\n"
                              "destination[2]: client.example:7777\n"))
            listed = subprocess.check_output(prefix + ["-l"], cwd=root, env=env).decode()
            own = next(line.split("\t") for line in listed.splitlines()
                       if line.startswith(path.parent.name[:8]))
            assert own[6] == "c/client.example:7777", own

            # Old checkpoints already carry verified recent IRC events. Preserve
            # those previews even when the historical prefix is unreadable.
            for _ in range(20):
                result = subprocess.run(prefix + ["--resume", path.parent.name, "-e", "--", prompt],
                                        cwd=root, env=env, capture_output=True, timeout=20)
                assert result.returncode == 0, result.stderr
                records = list(map(json.loads, path.read_text().splitlines()))
                checkpoints = [i for i, e in enumerate(records) if e["type"] == "session_checkpoint"]
                if checkpoints:
                    index = checkpoints[-1]
                    context = records[index]["data"]["context"]
                    if context and any(e["type"] == "irc_event" and e["data"]["sequence"] == 2
                                       for e in context["recent"]):
                        break
            else:
                raise AssertionError("provider did not write a checkpoint with recent IRC input")
            records[index]["data"]["state"]["strings"].pop("irc_snapshot", None)
            previous = records[index - 1]["event_sha256"]
            for record in records[index:]:
                record["prev_sha256"] = previous
                del record["event_sha256"]
                record["event_sha256"] = hashlib.sha256(canonical(record).encode()).hexdigest()
                previous = record["event_sha256"]
            encoded = [canonical(record).encode() + b"\n" for record in records]
            bad = next(i for i, e in enumerate(records[:index]) if e["type"] == "irc_event"
                       and e["data"]["sequence"] == 2)
            encoded[bad] = encoded[bad].replace(b"latest actual message", b"broken actual message")
            path.write_bytes(b"".join(encoded))
            saved = path.read_bytes()
            listed = subprocess.check_output(prefix + ["-l"], cwd=root, env=env).decode()
            own = next(line.split("\t") for line in listed.splitlines()
                       if line.startswith(path.parent.name[:8]))
            assert own[5] == "localhost:6667: latest actual message", own
            assert own[6] == "c/client.example:7777", own
            assert path.read_bytes() == saved
            assert not provider.failure, provider.failure
            print("session listing: multiline IRC, controls, long Unicode and read-only rows: ok", flush=True)
    finally:
        provider.close()


def check_renamed_nicks(binary):
    provider = harness.FakeResponses()
    provider.runtime_handler = lambda handler, request, sequence: provider.reply(
        handler, provider.response_body(sequence, "saved").encode())
    try:
        with tempfile.TemporaryDirectory(prefix="snag-list-rename-") as tmp:
            root = Path(tmp).resolve()
            config = root / "config.ini"
            harness.write_irc_config(config, provider.port, "host-model")
            state = root / "state"
            env = {**os.environ, "HOME": str(root), "SNAJPAGENT_IRC_UI_KEY": "irc-ui-secret"}
            prefix = [str(binary), "--config", str(config), "--dotdir", str(state)]
            subprocess.run(prefix + ["-e", "--", "ping"], cwd=root, env=env,
                           capture_output=True, check=True, timeout=20)
            path = next((state / "sessions").glob("*/events.jsonl"))
            snapshot = ("[IRC room snapshot; @ marks a channel operator]\n"
                        "model nick: minion4\noperator nick: op\nhosted: no\n"
                        "destination[1]: one:7\n"
                        "aliases[one:7]: model minion4 operator op\n"
                        "destination[2]: two:7\n"
                        "aliases[two:7]: model minion4 operator op\n")
            append_event(path, "irc_snapshot", dict(reason="join", text=snapshot, timestamp_ms=1))

            def rename(old, new, sequence, endpoint="one:7", historical=False):
                append_event(path, "irc_event", dict(endpoint=endpoint, historical=historical,
                             kind="nick", local=False, nick=old, op=False, room="#lab", text=new,
                             timestamp_ms=1, stream="b" * 32, sequence=sequence, input=False))

            def listed(expected):
                saved = path.read_bytes()
                result = subprocess.run(prefix + ["-l"], cwd=root, env=env,
                                        capture_output=True, check=True, timeout=10)
                row = result.stdout.decode().splitlines()[1].split("\t")
                assert row[6] == expected, row
                assert path.read_bytes() == saved

            rename("minion4", "docsowner", 1)
            rename("stranger", "unrelated", 2)
            rename("docsowner", "outdated", 3, historical=True)
            listed("c/docsowner@one:7,c/minion4@two:7")
            # The old owner checkpoints the stale snapshot after the rename.
            for _ in range(20):
                subprocess.run(prefix + ["--resume", path.parent.name, "-e", "--", "ping"],
                               cwd=root, env=env, capture_output=True, check=True, timeout=20)
                records = list(map(json.loads, path.read_text().splitlines()))
                checkpoints = [r for r in records if r["type"] == "session_checkpoint"]
                if checkpoints:
                    assert checkpoints[-1]["data"]["state"]["strings"]["irc_snapshot"] == snapshot
                    break
            else:
                raise AssertionError("no checkpoint created")
            listed("c/docsowner@one:7,c/minion4@two:7")
            rename("docsowner", "[review]", 4)
            rename("{REVIEW}", "minion4", 5)
            rename("minion4", "docsreviewer", 6)
            listed("c/docsreviewer@one:7,c/minion4@two:7")
            # Recent checkpoint data remains useful with a damaged old prefix.
            lines = path.read_bytes().splitlines(keepends=True)
            for i, line in enumerate(lines):
                record = json.loads(line)
                if record["type"] == "irc_event" and record["data"]["sequence"] == 1:
                    lines[i] = line.replace(b"docsowner", b"docxowner")
                    break
            path.write_bytes(b"".join(lines))
            listed("c/docsreviewer@one:7,c/minion4@two:7")
            assert not provider.failure, provider.failure
            print("session listing: later nick events, stale checkpoints and rename chains: ok")
    finally:
        provider.close()


if __name__ == "__main__":
    check_listing(Path(sys.argv[1]).resolve())
    check_renamed_nicks(Path(sys.argv[1]).resolve())
