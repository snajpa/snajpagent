#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Canonical read-only history for native-default CLI fixtures."""
import json
import subprocess
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


def read_events(journal):
    journal = Path(journal)
    if journal.name == "events.jsonl":
        return [json.loads(line) for line in journal.read_bytes().split(b"\n")[:-1]
                if line.strip()]
    if journal.name != "journal.bin":
        raise ValueError(f"unsupported fixture journal: {journal}")
    result = subprocess.run(
        [str(READER), "--read-history", str(journal.parent.parent.parent),
         journal.parent.name], capture_output=True, timeout=15)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors="replace").strip())
    return [json.loads(line) for line in result.stdout.splitlines()]


def read_boundary(journal):
    """Return decoder-proved coordinates after whole-prefix verification."""
    journal = Path(journal)
    if journal.name not in ("events.jsonl", "journal.bin"):
        raise ValueError(f"unsupported fixture journal: {journal}")
    result = subprocess.run(
        [str(READER), "--read-boundary", str(journal.parent.parent.parent),
         journal.parent.name], capture_output=True, timeout=15)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors="replace").strip())
    return json.loads(result.stdout)


def create_legacy(dotdir, cwd, provider, model, effort="medium"):
    """Use the explicit released-format constructor for byte-mutation fixtures."""
    result = subprocess.run(
        [str(READER), "--create-legacy", str(dotdir), str(cwd), provider, model, effort],
        capture_output=True, text=True, check=True, timeout=15)
    return Path(dotdir) / "sessions" / result.stdout.strip() / "events.jsonl"
