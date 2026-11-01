#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""An unavailable saved cwd falls home once, before model work touches the old path."""
import contextlib
import errno
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

import tmux_terminal as harness
from store_history import create_legacy, journal_paths, read_events


BINARY = Path(sys.argv.pop(1)).resolve()


@contextlib.contextmanager
def mounted_project(backing, mount):
    """Use a local SFTP socket so the disconnected-mount fixture needs no SSH host."""
    left, right = socket.socketpair()
    server = None
    filesystem = None
    mounted = False
    try:
        server = subprocess.Popen(["/usr/lib/openssh/sftp-server"],
                                  stdin=left, stdout=left, stderr=subprocess.DEVNULL)
        filesystem = subprocess.Popen(
            ["sshfs", "-f", "-o", "passive,attr_timeout=0,entry_timeout=0,negative_timeout=0",
             ":" + str(backing), str(mount)],
            stdin=right, stdout=right, stderr=subprocess.DEVNULL)
        left.close()
        right.close()
        deadline = time.monotonic() + 5
        while not os.path.ismount(mount):
            if filesystem.poll() is not None:
                raise unittest.SkipTest("SSHFS mounting is unavailable")
            if time.monotonic() >= deadline:
                raise RuntimeError("SSHFS fixture did not mount")
            time.sleep(.01)
        mounted = True

        yield filesystem
    finally:
        left.close()
        right.close()
        for process in (filesystem, server):
            if process is not None:
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=5)
        if mounted or os.path.ismount(mount):
            subprocess.run(["fusermount3", "-u", str(mount)], check=True, timeout=5)


class CwdRecoveryTests(unittest.TestCase):
    def test_resume_after_cwd_disappears(self):
        for legacy in (False, True):
            for replacement in ("missing", "file", "missing-parent", "symlink-loop"):
                with self.subTest(replacement=replacement, legacy=legacy):
                    self.check_recovery(replacement, legacy)

    @unittest.skipUnless(sys.platform.startswith("linux") and Path("/dev/fuse").exists() and
                         Path("/usr/lib/openssh/sftp-server").is_file() and
                         shutil.which("sshfs") and shutil.which("fusermount3"),
                         "requires Linux FUSE, SSHFS and a local SFTP server")
    def test_resume_after_filesystem_disconnect(self):
        for legacy in (False, True):
            for failure in ("disconnected", "stalled"):
                with self.subTest(legacy=legacy, failure=failure):
                    self.check_recovery(failure, legacy)

    def test_read_only_and_disabled_project_discovery(self):
        for legacy in (False, True):
            for read_only, read_agents in ((True, True), (False, False)):
                with self.subTest(legacy=legacy, read_only=read_only, read_agents=read_agents):
                    self.check_recovery("missing", legacy, read_only, read_agents)

    def check_recovery(self, replacement, legacy, read_only=False, read_agents=True):
        provider = harness.FakeResponses()
        self.addCleanup(provider.close)
        with tempfile.TemporaryDirectory(prefix="snag-cwd-recovery-") as tmp, \
                contextlib.ExitStack() as cleanup:
            root = Path(tmp).resolve()
            home = root / "home"
            home.mkdir()
            home_instruction = home / "AGENTS.md"
            home_instruction.write_text("Home recovery instructions.\n")
            (home / "marker").write_text("home relative file\n")
            old = home / "project" / "work"
            if replacement in ("disconnected", "stalled"):
                backing = root / "backing"
                backing.mkdir()
                old.parent.mkdir()
                filesystem = cleanup.enter_context(mounted_project(backing, old.parent))
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
                f"read_agents_md={'true' if read_agents else 'false'}\n"
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
                    "get_cwd": {},
                    "read_file": {"path": "marker"},
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
            if replacement in ("disconnected", "stalled"):
                target.mkdir()
                shutil.copyfile(old / "AGENTS.md", target / "AGENTS.md")
                if replacement == "stalled":
                    filesystem.send_signal(signal.SIGSTOP)
                else:
                    filesystem.kill()
                    filesystem.wait(timeout=5)
                    with self.assertRaises(OSError) as failure:
                        old.stat()
                    self.assertEqual(failure.exception.errno, errno.ENOTCONN)
            elif replacement == "missing-parent":
                old.parent.rename(target)
                target = target / "work"
            else:
                old.rename(target)
                if replacement == "file":
                    old.write_text("A file now occupies the saved directory path.\n")
                elif replacement == "symlink-loop":
                    old.symlink_to(old.name)
            requests.clear()
            steps[:] = ["get_cwd", "read_file", "answer"]
            started = time.monotonic()
            prompt = "/ro recover" if read_only else "recover"
            second = subprocess.run([*common, "--resume", session.name, "-e", "--", prompt],
                                    cwd=root, env=env, capture_output=True, text=True, timeout=6)
            elapsed = time.monotonic() - started
            self.assertEqual(second.returncode, 0, second.stderr)
            self.assertFalse(steps)
            self.assertIn("Changed to home:", second.stderr)
            context = json.dumps(requests[0]["input"])
            if not read_only:
                self.assertIn(f"cwd={home};", context)
            self.assertEqual(str(global_file) in context, read_agents)
            self.assertIn(str(explicit), context)
            self.assertEqual(str(home_instruction) in context, read_agents)
            self.assertNotIn(str(old / "AGENTS.md"), context)
            self.assertIn("home relative file", json.dumps(requests[-1]["input"]))
            changes = [e for e in read_events(journal) if e["type"] == "work_context_changed"]
            self.assertEqual(changes[-1]["data"]["old_cwd"], str(old))
            self.assertEqual(changes[-1]["data"]["new_cwd"], str(home))
            steps[:] = ["get_cwd", "read_file", "cd", "answer"]
            requests.clear()
            started = time.monotonic()
            third = subprocess.run([*common, "--resume", session.name, "-e", "--", "continue"],
                                   cwd=root, env=env, capture_output=True, text=True, timeout=6)
            reopen_elapsed = time.monotonic() - started
            self.assertEqual(third.returncode, 0, third.stderr)
            self.assertFalse(steps)
            self.assertNotIn("Changed to home:", third.stderr)
            context = json.dumps(requests[0]["input"])
            self.assertIn(f"cwd={home};", context)
            self.assertNotIn(str(old / "AGENTS.md"), context)
            self.assertEqual(str(target / "AGENTS.md") in json.dumps(requests[-1]["input"]),
                             read_agents)
            print(f"{replacement} legacy={legacy}: fallback={elapsed:.3f}s "
                  f"reopen={reopen_elapsed:.3f}s", flush=True)
            self.assertFalse(any(e["type"] in ("goal_resumed", "goal_completed")
                                 for e in read_events(journal)))


if __name__ == "__main__":
    unittest.main()
