#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""A missing saved cwd must leave the model able to select an existing one."""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import tmux_terminal as harness
from store_history import create_legacy, journal_paths, read_events


BINARY = Path(sys.argv.pop(1)).resolve()


class CwdRecoveryTests(unittest.TestCase):
    def test_resume_after_cwd_disappears(self):
        for legacy in (False, True):
            for replacement in ("missing", "file", "missing-parent"):
                with self.subTest(replacement=replacement, legacy=legacy):
                    self.check_recovery(replacement, legacy)

    def check_recovery(self, replacement, legacy):
        provider = harness.FakeResponses()
        self.addCleanup(provider.close)
        with tempfile.TemporaryDirectory(prefix="snag-cwd-recovery-") as tmp:
            root = Path(tmp).resolve()
            home = root / "home"
            home.mkdir()
            old = home / "project" / "work"
            old.mkdir(parents=True)
            (old / "AGENTS.md").write_text("Project instructions before the move.\n")
            docs = root / "docs"
            docs.mkdir()
            explicit = docs / "AGENTS.md"
            explicit.write_text("Explicit recovery guidance.\n")
            global_root = home / ".config" / "snajpagent"
            global_root.mkdir(parents=True)
            global_file = global_root / "AGENTS.md"
            global_file.write_text("Global recovery guidance.\n")
            state = root / "state"
            state.mkdir(mode=0o700)
            config = state / "config.ini"
            config.write_text(
                "[agent]\nprovider=fixture\nmodel=host-model\nmax_turn_retries=0\n"
                "read_agents_md=true\n"
                f"[provider fixture]\nbase_url=http://127.0.0.1:{provider.port}\n"
                'api_key="irc-ui-secret"\nexact_token_count=false\n'
                "[ui]\ncolor=never\nmarkdown=false\npager=off\n")
            config.chmod(0o600)
            env = {**os.environ, "HOME": str(home), "XDG_CONFIG_HOME": str(home / ".config")}
            target = old
            steps = ["cd", "create_goal", "update_goal", "answer"]
            requests = []

            def respond(handler, request, sequence):
                requests.append(request)
                step = steps.pop(0)
                arguments = {
                    "cd": {"path": str(target)},
                    "create_goal": {"objective": "Wait for the synthetic dependency."},
                    "update_goal": {"action": "block", "text": "Synthetic input is missing.",
                                    "wait_for": "external: synthetic input"},
                }
                body = (provider.response_body(sequence, "cwd recovery complete")
                        if step == "answer" else
                        provider.function_body(sequence, f"cwd-{sequence}", step, arguments[step]))
                provider.reply(handler, body.encode(), close_header=True)

            provider.runtime_handler = respond
            common = [str(BINARY), "--dotdir", str(state), "--no-listen", "--no-client"]
            start = []
            if legacy:
                journal = create_legacy(state, home, "fixture", "host-model")
                start = ["--resume", journal.parent.name]
            first = subprocess.run([*common, *start, "-d", str(docs), "-e", "--", "enter project"],
                                   cwd=root, env=env, capture_output=True, text=True, timeout=20)
            self.assertEqual(first.returncode, 0, first.stderr)
            self.assertFalse(steps)
            session, = (state / "sessions").iterdir()
            journal, = journal_paths(state)
            self.assertEqual(sum(e["type"] == "goal_blocked" for e in read_events(journal)), 1)
            target = root / "moved"
            if replacement == "missing-parent":
                old.parent.rename(target)
                target = target / "work"
            else:
                old.rename(target)
                if replacement == "file":
                    old.write_text("A file now occupies the saved directory path.\n")
            requests.clear()
            steps[:] = ["cd", "answer"]
            second = subprocess.run([*common, "--resume", session.name, "-e", "--", "recover"],
                                    cwd=root, env=env, capture_output=True, text=True, timeout=20)
            self.assertEqual(second.returncode, 0, second.stderr)
            self.assertFalse(steps)
            context = json.dumps(requests[0]["input"])
            self.assertIn("Saved working directory is unavailable", context)
            self.assertIn(str(old), context)
            self.assertIn(str(global_file), context)
            self.assertIn(str(explicit), context)
            self.assertNotIn(str(old / "AGENTS.md"), context)
            self.assertIn(f"cwd={target};", json.dumps(requests[-1]["input"]))
            steps[:] = ["answer"]
            requests.clear()
            third = subprocess.run([*common, "--resume", session.name, "-e", "--", "continue"],
                                   cwd=root, env=env, capture_output=True, text=True, timeout=20)
            self.assertEqual(third.returncode, 0, third.stderr)
            self.assertFalse(steps)
            context = json.dumps(requests[0]["input"])
            self.assertNotIn("Saved working directory is unavailable", context)
            self.assertIn(str(target / "AGENTS.md"), context)
            self.assertFalse(any(e["type"] in ("goal_resumed", "goal_completed")
                                 for e in read_events(journal)))


if __name__ == "__main__":
    unittest.main()
