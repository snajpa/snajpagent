#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Canonical native fixture reads preserve bytes and reject corrupt sources."""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import tmux_terminal as harness
from store_history import READER, create_legacy, journal_paths, read_boundary, read_events


BINARY = Path(sys.argv[1] if len(sys.argv) > 1 else "./snajpagent").resolve()


class HistoryTests(unittest.TestCase):
    def test_only_published_session_names(self):
        with tempfile.TemporaryDirectory(prefix="snag-history-names-") as tmp:
            root = Path(tmp).resolve()
            session = root / "sessions" / ("a" * 32)
            session.mkdir(parents=True)
            legacy = session / "events.jsonl"
            legacy.touch()
            self.assertEqual(journal_paths(root), [legacy])
            native = session / "journal.bin"
            native.touch()
            provisional = root / "sessions" / (".creating-" + "b" * 32)
            provisional.mkdir()
            (provisional / "journal.bin").touch()
            for name in ("B" * 32, "g" * 32, "c" * 31):
                invalid = root / "sessions" / name
                invalid.mkdir()
                (invalid / "journal.bin").touch()
            self.assertEqual(journal_paths(root), [native])

    def test_explicit_legacy_fixture(self):
        with tempfile.TemporaryDirectory(prefix="snag-history-legacy-") as tmp:
            root = Path(tmp).resolve()
            journal = create_legacy(root / "state", root, "openai", "host-model")
            self.assertEqual(journal_paths(root / "state"), [journal])
            self.assertFalse(journal.with_name("journal.bin").exists())
            events = read_events(journal)
            self.assertEqual(events[0]["type"], "session_created")
            saved = journal.read_bytes()
            last = json.loads(saved.splitlines()[-1])
            self.assertEqual(read_boundary(journal), {"seq": last["seq"],
                             "end": len(saved), "sha256": last["event_sha256"]})
            journal.write_bytes(saved + b'{"partial')
            self.assertEqual(read_events(journal), events)
            self.assertEqual(journal.read_bytes(), saved + b'{"partial')
            journal.write_bytes(saved + b"invalid complete record\n")
            with self.assertRaises(ValueError):
                read_events(journal)

    def test_native_read_only_prefix(self):
        provider = harness.FakeResponses()
        provider.runtime_handler = lambda handler, request, sequence: provider.reply(
            handler, provider.response_body(sequence, "history answer").encode(),
            close_header=True)
        try:
            with tempfile.TemporaryDirectory(prefix="snag-history-") as tmp:
                root = Path(tmp).resolve()
                config = root / "config.ini"
                harness.write_irc_config(config, provider.port, "host-model")
                dotdir = root / "state"
                result = subprocess.run(
                    [str(BINARY), "--config", str(config), "--dotdir", str(dotdir),
                     "-e", "--", "history fixture"], cwd=root,
                    env={**os.environ, "HOME": str(root),
                         "SNAJPAGENT_IRC_UI_KEY": "irc-ui-secret"},
                    capture_output=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stderr)
                journal, = journal_paths(dotdir)
                self.assertEqual(journal.name, "journal.bin")
                before = {p.name: p.read_bytes() for p in journal.parent.iterdir()
                          if p.is_file()}
                events = read_events(journal)
                boundary = read_boundary(journal)
                self.assertEqual(boundary["end"], len(before["journal.bin"]))
                self.assertGreaterEqual(boundary["seq"], events[-1]["seq"])
                self.assertRegex(boundary["sha256"], r"^[0-9a-f]{64}$")
                self.assertEqual([e["data"]["text"] for e in events
                                  if e["type"] == "turn_started"], ["history fixture"])
                self.assertEqual(before, {p.name: p.read_bytes()
                                          for p in journal.parent.iterdir() if p.is_file()})
                with journal.open("ab") as file:
                    file.write(b"partial")
                torn = journal.read_bytes()
                self.assertEqual(read_events(journal), events)
                self.assertEqual(read_boundary(journal), boundary)
                self.assertEqual(journal.read_bytes(), torn)
                journal.write_bytes(bytes([torn[0] ^ 1]) + torn[1:])
                corrupt = journal.read_bytes()
                result = subprocess.run(
                    [str(READER), "--read-history", str(dotdir), journal.parent.name],
                    capture_output=True, timeout=15)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b"")
                self.assertEqual(journal.read_bytes(), corrupt)
        finally:
            provider.close()


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
