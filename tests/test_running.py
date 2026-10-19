#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise pinned PTY commands through the native owner and local provider."""
import base64
import json
import os
import re
import threading
from pathlib import Path
import shlex
import time
import unittest

import skills

BINARY = skills.BINARY
from tmux_terminal import TmuxTerminal


class RunningCommands(unittest.TestCase):
    setUp = skills.SkillsCase.setUp
    tearDown = skills.SkillsCase.tearDown
    configured_provider = skills.RuntimeSkills.configured_provider
    execute = skills.RuntimeSkills.execute
    runtime_events = skills.RuntimeSkills.runtime_events

    def plan(self, provider, calls, before=None):
        requests = []

        def respond(handler, request, sequence):
            index = len(requests)
            requests.append(request)
            if before:
                before(index)
            if index < len(calls):
                call = calls[index]
                tool, arguments = call if isinstance(call, tuple) else ("running_commands", call)
                body = provider.function_body(sequence, f"running-{index}", tool, arguments)
            else:
                body = provider.response_body(sequence, "RUNNING_DONE")
            provider.reply(handler, body.encode(), close_header=True)

        provider.runtime_handler = respond
        return requests

    def wait_file(self, path, text):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if path.exists() and text in path.read_text():
                return
            time.sleep(.01)
        self.fail(f"missing consumer marker {text}: {path}")

    @staticmethod
    def outputs(request):
        return [item["output"] for item in request["input"]
            if item.get("type") == "function_call_output"]

    def test_real_controlling_tty_and_queued_input(self):
        provider = self.configured_provider()
        marker = self.workspace / "received"
        command = "test -t 0 && test -t 1 && test -t 2 && " \
            "stty size && printf 'PTY_READY\\n'; read line; printf '%s' \"$line\" > " + \
            shlex.quote(str(marker)) + "; printf 'INPUT:%s\\n' \"$line\"; sleep 20"
        calls = [
            {"action": "start", "name": "reader", "command": command},
            {"action": "write", "id": "reader", "data": "hello tty\n"},
            {"action": "show", "id": "reader"},
            {"action": "stop", "id": "reader"},
        ]
        requests = self.plan(provider, calls,
            lambda index: self.wait_file(marker, "hello tty") if index == 2 else None)
        result = self.execute("Start and inspect the explicit PTY job.")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(marker.read_text(), "hello tty")
        outputs = self.outputs(requests[3])
        shown = json.loads(outputs[-1])
        captured = base64.b64decode(shown["output_base64"])
        self.assertIn(b"PTY_READY", captured)
        self.assertIn("Pinned commands are host data", json.dumps(requests[2]["input"]))
        self.assertNotIn("PTY_READY", result.stdout)

    def test_explicit_resume_hook_runs_per_owner_and_stop_disables_it(self):
        provider = self.configured_provider()
        marker = self.workspace / "starts"
        target = shlex.quote(str(marker))
        calls = [{"action": "start", "name": "watcher",
            "command": f"echo initial >> {target}; sleep 20",
            "on_resume": f"echo resumed >> {target}; sleep 20"}]
        self.plan(provider, calls,
            lambda index: self.wait_file(marker, "initial") if index == 1 else None)
        first = self.execute("Keep this named job for future owner resume.")
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(marker.read_text(), "initial\n")
        from store_history import journal_paths
        session = journal_paths(self.dotdir)[0].parent.name
        requests = self.plan(provider, [
            {"action": "list"}, {"action": "stop", "id": "all"}],
            lambda index: self.wait_file(marker, "resumed") if index == 0 else None)
        resumed = self.execute("Inspect and explicitly stop saved jobs.", session=session)
        self.assertEqual(resumed.returncode, 0, resumed.stderr)
        self.assertEqual(marker.read_text(), "initial\nresumed\n")
        listed = json.loads(self.outputs(requests[1])[-1])
        self.assertEqual(listed[0]["name"], "watcher")
        self.assertTrue(listed[0]["resume"])
        self.plan(provider, [{"action": "list"}])
        stopped = self.execute("Inspect stopped job without recreating it.", session=session)
        self.assertEqual(stopped.returncode, 0, stopped.stderr)
        self.assertEqual(marker.read_text(), "initial\nresumed\n")

    def test_compact_operator_report_and_all_kill_aliases(self):
        provider = self.configured_provider()
        self.plan(provider, [{"action": "start", "name": "quiet-job",
            "command": "printf PRIVATE_JOB_OUTPUT; sleep 20",
            "on_resume": "printf PRIVATE_RESUME_HOOK; sleep 20"}])
        with TmuxTerminal(self.root / "tty", BINARY, self.workspace, self.dotdir,
                self.dotdir / "config.ini", 130, 35,
                args=("--no-listen", "--no-client"),
                environment=self.environment) as terminal:
            terminal.wait("host-model")
            terminal.submit_wait("Start the named job.", "RUNNING_DONE")
            report = terminal.submit_wait("/r", "resume:yes")
            self.assertIn("quiet-job", report)
            self.assertNotIn("PRIVATE_JOB_OUTPUT", report)
            self.assertNotIn("PRIVATE_RESUME_HOOK", report)
            terminal.submit_wait("/r k --all", "Commands stopping")
            for command in ("/r k -a", "/r kill -a", "/r kill --all"):
                terminal.submit_wait(command, "Commands stopping")
            stopped = terminal.submit_wait("/r", "resume:no")
            self.assertIn("quiet-job", stopped)

    def test_read_only_refuses_start_input_stop_but_allows_inspection(self):
        provider = self.configured_provider()
        marker = self.workspace / "readonly-must-not-run"
        requests = self.plan(provider, [
            {"action": "start", "name": "forbidden", "command": "touch " + shlex.quote(str(marker))},
            {"action": "write", "id": "forbidden", "data": "no"},
            {"action": "stop", "id": "all"}, {"action": "list"},
        ])
        result = self.execute("/ro inspect jobs only")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(marker.exists())
        for request in requests[1:4]:
            self.assertIn("read-only", self.outputs(request)[-1])
        self.assertEqual(json.loads(self.outputs(requests[4])[-1]), [])

    def test_natural_failure_is_reported_without_restart(self):
        provider = self.configured_provider()
        marker = self.workspace / "one-shot"
        requests = self.plan(provider, [
            {"action": "start", "name": "finite", "command":
                "echo once >> " + shlex.quote(str(marker)) + "; exit 7"},
            {"action": "show", "id": "finite"}, {"action": "list"},
        ], lambda index: (self.wait_file(marker, "once"), time.sleep(.1)) if index == 1 else None)
        result = self.execute("Inspect the finite job, without retrying it.")
        self.assertEqual(result.returncode, 0, result.stderr)
        state = json.loads(self.outputs(requests[2])[-1])
        self.assertEqual(state["state"], "failed")
        self.assertEqual(state["exit_code"], 7)
        self.assertEqual(marker.read_text(), "once\n")
        self.assertEqual(json.loads(self.outputs(requests[3])[-1])[0]["state"], "failed")

    def test_concurrency_start_failure_and_ambiguous_name(self):
        provider = self.configured_provider()
        config = self.dotdir / "config.ini"
        with config.open("a") as stream:
            stream.write("\n[tool]\nmax_parallel_commands=1\n")
        requests = self.plan(provider, [
            {"action": "start", "name": "duplicate", "command": "sleep 30"},
            {"action": "start", "name": "duplicate", "command": "sleep 30"},
            {"action": "stop", "id": "duplicate"}, {"action": "list"},
            {"action": "stop", "id": "all"},
        ])
        result = self.execute("Exercise the saved job limit and ambiguous names.")
        self.assertEqual(result.returncode, 0, result.stderr)
        failed = json.loads(self.outputs(requests[2])[-1])
        self.assertEqual(failed["state"], "start_failed")
        self.assertIn("limit reached", failed["error"])
        self.assertIn("ambiguous", self.outputs(requests[3])[-1])
        listed = json.loads(self.outputs(requests[4])[-1])
        self.assertEqual(len(listed), 2)
        self.assertEqual({item["state"] for item in listed}, {"running", "start_failed"})

    def test_resume_output_symlink_is_refused_without_running_hook(self):
        from store_history import journal_paths
        provider = self.configured_provider()
        marker = self.workspace / "hook-must-not-run"
        self.plan(provider, [{"action": "start", "name": "guarded",
            "command": "sleep 20", "on_resume": f"touch {marker}; sleep 20"}])
        result = self.execute("Save a job for a later owner.")
        self.assertEqual(result.returncode, 0, result.stderr)
        journal = journal_paths(self.dotdir)[0]
        event = next(event for event in self.runtime_events()
            if event["type"] == "pinned_commands_updated")
        job = event["data"]["commands"][0]
        output = journal.parent / f"running-{job['id']}-0.log"
        output.unlink()
        protected = self.workspace / "protected"
        protected.write_text("unchanged")
        output.symlink_to(protected)
        requests = self.plan(provider, [{"action": "list"},
            {"action": "stop", "id": "guarded"}])
        resumed = self.execute("Inspect the failed saved hook.", session=journal.parent.name)
        self.assertEqual(resumed.returncode, 0, resumed.stderr)
        state = json.loads(self.outputs(requests[1])[-1])[0]
        self.assertEqual(state["state"], "start_failed")
        self.assertIn("error", state)
        self.assertFalse(marker.exists())
        self.assertEqual(protected.read_text(), "unchanged")
        output.unlink()
        output.write_text("not private")
        output.chmod(0o644)
        # The previous explicit stop disabled this job. A fresh saved enabled
        # definition is unnecessary: show must apply the same privacy check.
        requests = self.plan(provider, [{"action": "show", "id": "guarded"}])
        inspected = self.execute("Inspect the non-private capture.", session=journal.parent.name)
        self.assertEqual(inspected.returncode, 0, inspected.stderr)
        receipt = self.outputs(requests[1])[-1]
        self.assertNotIn("output_base64", receipt)
        self.assertNotIn("not private", receipt)
        self.assertFalse(marker.exists())


    def test_invalid_start_and_missing_input_do_not_claim_success(self):
        provider = self.configured_provider()
        marker = self.workspace / "must-not-run"
        requests = self.plan(provider, [
            {"action": "start", "name": "bad\nname", "command": f"touch {marker}"},
            {"action": "write", "id": "missing", "data": "never delivered"},
            {"action": "list"},
        ])
        result = self.execute("Exercise refused command operations.")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(marker.exists())
        self.assertIn("control characters", self.outputs(requests[1])[-1])
        self.assertIn("unknown pinned command", self.outputs(requests[2])[-1])
        self.assertEqual(json.loads(self.outputs(requests[3])[-1]), [])

    def test_output_tail_is_bounded_and_redacted(self):
        provider = self.configured_provider()
        marker = self.workspace / "drained"
        calls = [
            {"action": "start", "name": "noisy", "command":
                "head -c 65536 /dev/zero; printf 'irc-ui-secret\\033[2JTAIL'; "
                "head -c 256 /dev/zero; "
                f"touch {marker}; sleep 20"},
            {"action": "show", "id": "noisy"},
            {"action": "stop", "id": "noisy"},
        ]
        requests = self.plan(provider, calls,
            lambda index: self.wait_file(marker, "") if index == 1 else None)
        result = self.execute("Inspect a bounded redacted tail.")
        self.assertEqual(result.returncode, 0, result.stderr)
        state = json.loads(self.outputs(requests[2])[-1])
        output = base64.b64decode(state["output_base64"])
        self.assertLessEqual(len(output), 8192)
        self.assertTrue(b"TAIL" in output, f"tail not flushed: {output[-96:]!r}")
        self.assertNotIn(b"irc-ui-secret", output)
        self.assertGreater(state["stdout_bytes"], 8192)

    def test_compaction_keeps_live_child_without_running_resume_hook(self):
        import tmux_terminal as harness
        provider = self.configured_provider()
        marker = self.workspace / "starts"
        self.plan(provider, [{"action": "start", "name": "persistent",
            "command": f"echo initial >> {marker}; sleep 20",
            "on_resume": f"echo hook >> {marker}; sleep 20"}])
        config = self.dotdir / "config.ini"
        config.write_text(config.read_text().replace("exact_token_count=false",
            "exact_token_count=false\nnative_compaction=false"))
        with TmuxTerminal(self.root / "compact", BINARY, self.workspace, self.dotdir,
                config, 130, 35, args=("--no-listen", "--no-client"),
                environment=self.environment) as terminal:
            terminal.wait("host-model")
            terminal.submit_wait("Start the persistent job.", "RUNNING_DONE")
            self.wait_file(marker, "initial")
            summaries = []

            def summarize(handler, request, sequence):
                summaries.append(request)
                self.assertTrue(harness.is_summary_request(request))
                provider.reply(handler, provider.response_body(sequence,
                    "The named PTY job remains running.").encode(), close_header=True)

            provider.runtime_handler = summarize
            terminal.submit_wait("/compact", "• Compacted")
            terminal.submit_wait("/r", "resume:yes")
            requests = self.plan(provider, [{"action": "list"},
                {"action": "stop", "id": "persistent"}])
            terminal.submit_wait("Inspect the same live child.", "RUNNING_DONE")
            terminal.wait("resume:yes")
            deadline = time.monotonic() + 5
            while len(requests) < 3 and time.monotonic() < deadline:
                time.sleep(.01)
            self.assertGreaterEqual(len(requests), 3)
            state = json.loads(self.outputs(requests[1])[-1])[0]
            self.assertEqual(state["state"], "running")
            self.assertIn("Pinned commands are host data", json.dumps(requests[0]))
        self.assertTrue(summaries)
        self.assertEqual(marker.read_text(), "initial\n")
        self.assertTrue(any(event["type"] == "compaction_completed"
            for event in self.runtime_events()))

    def test_live_attachment_does_not_run_saved_hook(self):
        from store_history import journal_paths
        provider = self.configured_provider()
        marker = self.workspace / "attached-starts"
        self.plan(provider, [{"action": "start", "name": "attached",
            "command": f"echo initial >> {marker}; sleep 20",
            "on_resume": f"echo hook >> {marker}; sleep 20"}])
        with TmuxTerminal(self.root / "owner", BINARY, self.workspace, self.dotdir,
                self.dotdir / "config.ini", 130, 35, args=("--no-listen", "--no-client"),
                environment=self.environment) as owner:
            owner.wait("host-model")
            owner.submit_wait("Start the attached owner's job.", "RUNNING_DONE")
            self.wait_file(marker, "initial")
            session = journal_paths(self.dotdir)[0].parent.name
            owner.submit("/s d")
            owner.wait_dead()
            self.assertIn("You can resume this session", owner.capture())
            with TmuxTerminal(self.root / "attached", BINARY, self.workspace, self.dotdir,
                    None, 130, 35,
                    args=("--attach", session), environment=self.environment) as attached:
                attached.wait("Attached session")
                self.assertEqual(marker.read_text(), "initial\n")
                attached.submit_wait("/r k attached", "Command stopping")
                attached.submit_wait("/r", "resume:no")
        self.assertEqual(marker.read_text(), "initial\n")

    def test_active_tools_precede_pins_and_bulk_stop_collects_normally(self):
        provider = self.configured_provider()
        waiting = threading.Event()
        release = threading.Event()
        self.addCleanup(release.set)
        calls = [
            {"action": "start", "name": "background", "command": "sleep 20"},
            ("exec_command", {"command": "sleep 20", "yield_ms": 1}),
            ("write_stdin", {}),
        ]
        requests = []

        def before(index):
            if index == 2:
                output = self.outputs(requests[-1])[-1]
                handle = re.search(r'"handle"\s*:\s*"([a-f0-9]{32})"', output)
                self.assertIsNotNone(handle, output)
                calls[2][1].update(handle=handle[1], yield_ms=1000)
                waiting.set()
                self.assertTrue(release.wait(10), "operator inspection did not complete")

        requests = self.plan(provider, calls, before)
        with TmuxTerminal(self.root / "ordering", BINARY, self.workspace, self.dotdir,
                self.dotdir / "config.ini", 130, 35, args=("--no-listen", "--no-client"),
                environment=self.environment) as terminal:
            terminal.wait("host-model")
            terminal.submit("Start a pinned and ordinary command.")
            self.assertTrue(waiting.wait(10))
            screen = terminal.submit_wait("/r", "background")
            rows = [line for line in screen.splitlines()
                if re.search(r"[a-f0-9]{8}  (tool|pin)", line)]
            self.assertGreaterEqual(len(rows), 2, screen)
            self.assertIn("tool", rows[0])
            self.assertIn("pin", rows[1])
            terminal.submit_wait("/r kill --all", "Commands stopping")
            release.set()
            terminal.wait("RUNNING_DONE")


if __name__ == "__main__":
    unittest.main()
