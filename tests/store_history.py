#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Canonical read-only history for native-default CLI fixtures."""
import json
import subprocess
import time
from pathlib import Path


READER = Path(__file__).resolve().with_name("test_store")


def journal_paths(dotdir):
    sessions = Path(dotdir) / "sessions"
    paths = {path.parent: path for path in sessions.glob("*/events.jsonl")}
    paths.update({path.parent: path for path in sessions.glob("*/journal.bin")})
    # Provisional native siblings are retained for recovery, not listed sessions.
    return [path for directory, path in paths.items()
            if len(directory.name) == 32 and
            all(char in "0123456789abcdef" for char in directory.name)]


def history_result(journal, operation):
    # Read-only native reconstruction reports EAGAIN when its source grows.
    # Discard that attempt's output; retry only the fixture's explicit status.
    deadline = time.monotonic() + 15
    while True:
        result = subprocess.run(
            [str(READER), operation, str(journal.parent.parent.parent),
             journal.parent.name], capture_output=True,
            timeout=max(.01, deadline - time.monotonic()))
        if result.returncode != 75 or time.monotonic() >= deadline:
            break
        time.sleep(.01)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors="replace").strip())
    return result.stdout


def read_events(journal):
    journal = Path(journal)
    if journal.name == "events.jsonl":
        return [json.loads(line) for line in journal.read_bytes().split(b"\n")[:-1]
                if line.strip()]
    if journal.name != "journal.bin":
        raise ValueError(f"unsupported fixture journal: {journal}")
    return [json.loads(line) for line in history_result(journal, "--read-history").splitlines()]


def read_boundary(journal):
    """Return decoder-proved coordinates after whole-prefix verification."""
    journal = Path(journal)
    if journal.name not in ("events.jsonl", "journal.bin"):
        raise ValueError(f"unsupported fixture journal: {journal}")
    return json.loads(history_result(journal, "--read-boundary"))


def create_legacy(dotdir, cwd, provider, model, effort="medium"):
    """Use the explicit released-format constructor for byte-mutation fixtures."""
    result = subprocess.run(
        [str(READER), "--create-legacy", str(dotdir), str(cwd), provider, model, effort],
        capture_output=True, text=True, check=True, timeout=15)
    return Path(dotdir) / "sessions" / result.stdout.strip() / "events.jsonl"
