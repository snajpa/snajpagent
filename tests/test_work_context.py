#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Temporary project/harness roles exercise the real session and provider path."""
import json
from pathlib import Path
import unittest

import skills


class WorkContext(skills.SkillsCase):
    configured_provider = skills.RuntimeSkills.configured_provider
    execute = skills.RuntimeSkills.execute
    runtime_events = skills.RuntimeSkills.runtime_events

    def project(self, name):
        root = self.root / name
        root.mkdir()
        (root / ".git").mkdir()
        (root / "AGENTS.md").write_text(f"Read role instructions for {name}.\n")
        self.skill(root / ".agents/skills", name, body=f"{name.upper()}-GUIDANCE")
        return root

    def plan(self, provider, calls, final="finished"):
        requests = []

        def respond(handler, request, sequence):
            requests.append(request)
            index = len(requests) - 1
            if index < len(calls):
                name, arguments = calls[index]
                body = provider.function_body(sequence, f"step-{index}", name, arguments)
            else:
                body = provider.response_body(sequence, final)
            provider.reply(handler, body.encode(), close_header=True)

        provider.runtime_handler = respond
        return requests

    @staticmethod
    def guidance(request):
        return json.dumps([message for message in request["input"]
            if message.get("role") in ("system", "developer")])

    def test_role_navigation_and_return_without_goal(self):
        provider = self.configured_provider()
        role = self.project("architect")
        target = self.project("product")
        self.skill(self.dotdir / "skills", "parent", body="PARENT-GUIDANCE")
        requests = self.plan(provider, [
            ("skills", {"action": "activate", "id": "parent"}),
            ("work_context", {"action": "enter", "path": str(role),
                "task": "Act as architect for this product; return with a design."}),
            ("skills", {"action": "activate", "id": "architect"}),
            ("cd", {"path": str(target)}),
            ("skills", {"action": "list"}),
            ("work_context", {"action": "leave", "summary": "Design recorded in project notes."}),
        ])
        result = self.execute("Design within the existing assignment; preserve parent notes.")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(requests), 7)
        entered = self.guidance(requests[2])
        self.assertIn("Current temporary work_context", entered)
        self.assertIn("without requiring a native goal", entered)
        self.assertIn("Act as architect", entered)
        self.assertIn(str(role / "AGENTS.md"), entered)
        self.assertIn("PARENT-GUIDANCE", entered)
        navigated = self.guidance(requests[4])
        self.assertIn(str(target / "AGENTS.md"), navigated)
        self.assertIn(str(role / "AGENTS.md"), navigated)
        self.assertIn("ARCHITECT-GUIDANCE", navigated)
        self.assertEqual(json.dumps(requests[4]["input"]).count("ARCHITECT-GUIDANCE"), 1)
        returned = self.guidance(requests[6])
        self.assertIn("No temporary work_context", returned)
        self.assertIn("PARENT-GUIDANCE", returned)
        self.assertNotIn("ARCHITECT-GUIDANCE", returned)
        self.assertNotIn(str(role / "AGENTS.md"), returned)
        events = self.runtime_events()
        changes = [event["data"] for event in events
            if event["type"] == "work_context_changed"]
        self.assertEqual(len(changes), 3)
        self.assertEqual(changes[0]["work_context"]["return_cwd"], str(self.home))
        self.assertEqual(changes[1]["work_context"], changes[0]["work_context"])
        self.assertIsNone(changes[2]["work_context"])
        self.assertEqual(changes[2]["new_cwd"], str(self.home))
        self.assertFalse(any(event["type"] == "goal_started" for event in events))
        self.assertTrue(all(request["model"] == "host-model" for request in requests))

    def test_direct_switch_retires_role_skills_and_keeps_parent_return(self):
        provider = self.configured_provider()
        first, second = self.project("engineer"), self.project("debugger")
        requests = self.plan(provider, [
            ("work_context", {"action": "enter", "path": str(first), "task": "Implement"}),
            ("skills", {"action": "activate", "id": "engineer"}),
            ("work_context", {"action": "enter", "path": str(second),
                "task": "Investigate using disposable evidence only"}),
            ("work_context", {"action": "status"}),
            ("work_context", {"action": "leave"}),
        ])
        result = self.execute("Use temporary roles sequentially")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("ENGINEER-GUIDANCE", self.guidance(requests[2]))
        self.assertNotIn("ENGINEER-GUIDANCE", self.guidance(requests[3]))
        self.assertIn(str(second / "AGENTS.md"), self.guidance(requests[3]))
        changes = [event["data"] for event in self.runtime_events()
            if event["type"] == "work_context_changed"]
        self.assertNotEqual(changes[0]["work_context"]["id"],
            changes[1]["work_context"]["id"])
        self.assertEqual(changes[1]["work_context"]["return_cwd"], str(self.home))
        self.assertEqual(changes[2]["new_cwd"], str(self.home))

    def test_failed_preparation_and_missing_return_leave_state_unchanged(self):
        provider = self.configured_provider()
        role = self.project("engineer")
        broken = self.root / "broken"
        broken.mkdir()
        (broken / "AGENTS.md").mkdir()
        requests = self.plan(provider, [
            ("work_context", {"action": "enter", "path": str(role), "task": "Implement"}),
            ("work_context", {"action": "enter", "path": str(broken), "task": "Fail"}),
            ("cd", {"path": str(self.root / "absent")}),
            ("work_context", {"action": "status"}),
            ("work_context", {"action": "leave", "path": str(self.workspace)}),
        ])
        result = self.execute("Exercise atomic preparation")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("must resolve to a regular file", json.dumps(requests[2]["input"]))
        self.assertIn(str(role / "AGENTS.md"), self.guidance(requests[4]))
        changes = [event["data"] for event in self.runtime_events()
            if event["type"] == "work_context_changed"]
        self.assertEqual(len(changes), 2)
        self.assertEqual(changes[-1]["new_cwd"], str(self.workspace))

    def test_readonly_status_does_not_allow_entry(self):
        provider = self.configured_provider()
        role = self.project("debugger")
        requests = self.plan(provider, [
            ("work_context", {"action": "status"}),
            ("work_context", {"action": "enter", "path": str(role), "task": "Investigate"}),
            ("cd", {"path": str(role)}),
        ])
        result = self.execute("/ro inspect the temporary context")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("read-only", json.dumps(requests[2]["input"]))
        self.assertFalse(any(event["type"] == "work_context_changed"
            for event in self.runtime_events()))

    def test_resume_keeps_role_and_skill_after_source_deletion(self):
        provider = self.configured_provider()
        role = self.project("architect")
        requests = []

        def stop(handler, request, sequence):
            requests.append(request)
            index = len(requests)
            if index == 1:
                body = provider.function_body(sequence, "enter", "work_context",
                    {"action": "enter", "path": str(role), "task": "Design a small fix"})
            elif index == 2:
                body = provider.function_body(sequence, "activate", "skills",
                    {"action": "activate", "id": "architect"})
            else:
                provider.reply(handler, b'{"error":{"message":"fixture stopped"}}',
                    status=503, close_header=True)
                return
            provider.reply(handler, body.encode(), close_header=True)

        provider.runtime_handler = stop
        result = self.execute("Start temporary design work")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ARCHITECT-GUIDANCE", self.guidance(requests[-1]))
        directory = max((self.dotdir / "sessions").iterdir(),
            key=lambda path: path.stat().st_mtime_ns)
        (role / ".agents/skills/architect/SKILL.md").unlink()
        requests = self.plan(provider, [("work_context", {"action": "leave"})])
        result = self.execute("/retry", session=directory.name)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("ARCHITECT-GUIDANCE", self.guidance(requests[0]))
        self.assertIn("Design a small fix", self.guidance(requests[0]))
        self.assertNotIn("ARCHITECT-GUIDANCE", self.guidance(requests[1]))

    def test_native_goal_and_parent_goal_guidance_survive_visit(self):
        provider = self.configured_provider()
        role = self.project("engineer")
        self.skill(self.dotdir / "skills", "parent", body="PARENT-GOAL-GUIDANCE")
        requests = self.plan(provider, [
            ("create_goal", {"objective": "Complete the existing parser project"}),
            ("skills", {"action": "activate", "id": "parent"}),
            ("work_context", {"action": "enter", "path": str(role), "task": "Implement"}),
            ("skills", {"action": "activate", "id": "engineer"}),
            ("skills", {"action": "activate", "id": "engineer"}),
            ("work_context", {"action": "leave"}),
            ("update_goal", {"action": "complete"}),
        ])
        result = self.execute("Create a persistent goal for the parser project, use the role, "
            "and complete the goal when the project is finished")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("PARENT-GOAL-GUIDANCE", self.guidance(requests[6]))
        self.assertNotIn("ENGINEER-GUIDANCE", self.guidance(requests[6]))
        events = self.runtime_events()
        self.assertEqual(sum(event["type"] == "goal_started" for event in events), 1)
        self.assertEqual(sum(event["type"] == "goal_completed" for event in events), 1)
        self.assertEqual(sum(event["type"] == "skills_updated" for event in events), 2)

    def test_deleted_parent_return_requires_explicit_recovery_path(self):
        provider = self.configured_provider()
        role = self.project("debugger")
        parent = self.root / "parent"
        parent.mkdir()
        session_requests = self.plan(provider, [
            ("cd", {"path": str(parent)}),
            ("work_context", {"action": "enter", "path": str(role), "task": "Investigate"}),
        ])
        result = self.execute("Begin the visit")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(session_requests), 3)
        parent.rmdir()
        requests = self.plan(provider, [
            ("work_context", {"action": "leave"}),
            ("work_context", {"action": "status"}),
            ("work_context", {"action": "leave", "path": str(self.workspace)}),
        ])
        result = self.execute("Recover the missing return directory", session=self.saved_session())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Investigate", self.guidance(requests[2]))
        self.assertIn("No temporary work_context", self.guidance(requests[3]))
        self.assertEqual(sum(event["type"] == "work_context_changed"
            for event in self.runtime_events()), 3)

    def test_invalid_actions_do_not_change_context(self):
        provider = self.configured_provider()
        role = self.project("engineer")
        requests = self.plan(provider, [
            ("work_context", {"action": "enter", "path": str(role)}),
            ("work_context", {"action": "status", "task": "unexpected"}),
            ("work_context", {"action": "leave"}),
            ("work_context", {"action": "status"}),
        ])
        result = self.execute("Check invalid context requests")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(requests), 5)
        self.assertFalse(any(event["type"] == "work_context_changed"
            for event in self.runtime_events()))

    def saved_session(self):
        return max((self.dotdir / "sessions").iterdir(),
            key=lambda path: path.stat().st_mtime_ns).name

    def test_completed_turn_and_compaction_retain_visit_and_parent_guidance(self):
        import tmux_terminal as harness
        provider = self.configured_provider()
        role = self.project("architect")
        self.skill(self.dotdir / "skills", "parent", body="PARENT-GUIDANCE")
        self.plan(provider, [
            ("skills", {"action": "activate", "id": "parent"}),
            ("work_context", {"action": "enter", "path": str(role),
                "task": "Design beneath the existing document-based assignment"}),
            ("skills", {"action": "activate", "id": "architect"}),
        ])
        result = self.execute("Start a temporary design visit")
        self.assertEqual(result.returncode, 0, result.stderr)
        session = self.saved_session()
        summary_requests = []

        def summarize(handler, request, sequence):
            self.assertTrue(harness.is_summary_request(request))
            summary_requests.append(request)
            provider.reply(handler, provider.response_body(sequence,
                "Earlier conversation summarized; see project notes.").encode(),
                close_header=True)

        provider.runtime_handler = summarize
        config = self.dotdir / "config.ini"
        config.write_text(config.read_text().replace("exact_token_count=false",
            "exact_token_count=false\nnative_compaction=false"))
        with harness.TmuxTerminal(self.root / "compact-terminal", skills.BINARY,
                self.workspace, self.dotdir, config, 100, 26,
                args=("--resume", session, "--no-listen", "--no-client"),
                environment=self.environment) as terminal:
            terminal.wait("fixture/host-model/medium")
            terminal.submit_wait("/compact", "• Compacted")
            terminal.exit()
        self.assertTrue(summary_requests)
        requests = self.plan(provider, [("work_context", {"action": "leave"})])
        result = self.execute("Continue the existing visit and return", session=session)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("ARCHITECT-GUIDANCE", self.guidance(requests[0]))
        self.assertIn("PARENT-GUIDANCE", self.guidance(requests[0]))
        self.assertIn("Design beneath", self.guidance(requests[0]))
        self.assertNotIn("ARCHITECT-GUIDANCE", self.guidance(requests[1]))
        self.assertIn("PARENT-GUIDANCE", self.guidance(requests[1]))
        events = self.runtime_events()
        self.assertTrue(any(event["type"] == "compaction_completed" for event in events))
        self.assertFalse(any(event["type"] == "goal_started" for event in events))

    def test_running_command_keeps_spawn_cwd_across_role_switch(self):
        import re
        provider = self.configured_provider()
        role = self.project("engineer")
        requests = []
        handle = None

        def respond(handler, request, sequence):
            nonlocal handle
            requests.append(request)
            index = len(requests)
            if index == 1:
                name, arguments = "exec_command", {"command":
                    "sleep 0.3; pwd; printf command-complete", "yield_ms": 0}
            elif index == 2:
                matches = re.findall(r'"handle"\s*:\s*"([a-f0-9]{32})"',
                    json.dumps(request["input"]).replace('\\"', '"'))
                self.assertTrue(matches)
                handle = matches[-1]
                name, arguments = "work_context", {"action": "enter", "path": str(role),
                    "task": "Implement while an existing command continues"}
            elif index == 3:
                name, arguments = "write_stdin", {"handle": handle, "yield_ms": 1000}
            elif index == 4:
                text = json.dumps(request["input"])
                self.assertIn("command-complete", text)
                self.assertIn(str(self.home) + "\\ncommand-complete", text)
                name, arguments = "work_context", {"action": "leave"}
            else:
                provider.reply(handler, provider.response_body(sequence, "finished").encode(),
                    close_header=True)
                return
            provider.reply(handler, provider.function_body(sequence, f"run-{index}",
                name, arguments).encode(), close_header=True)

        provider.runtime_handler = respond
        result = self.execute("Keep the command and temporary role independent")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(requests), 5)
        events = self.runtime_events()
        starts = [event["data"] for event in events if event["type"] == "tool_started"
            and event["data"].get("resolved_workdir") == str(self.home)]
        self.assertTrue(starts)
        self.assertTrue(handle)


if __name__ == "__main__":
    unittest.main()
