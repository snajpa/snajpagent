#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise real skill discovery, validation, installation and invocation."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

BINARY = str(Path(os.environ.get("SNAJPAGENT_TEST_BIN", "./snajpagent")).resolve())

class SkillsCase(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="snag-skills-cli-")
        self.root = Path(self.temporary.name).resolve()
        self.home = self.root / "home"
        self.home.mkdir()
        self.dotdir = self.root / "state"
        self.dotdir.mkdir(mode=0o700)
        self.workspace = self.root / "workspace"
        self.workspace.mkdir()
        self.environment = dict(os.environ, HOME=str(self.home))

    def tearDown(self):
        self.temporary.cleanup()

    def run_cli(self, *arguments, ok=True):
        result = subprocess.run(
            [BINARY, "skills", "--dotdir", str(self.dotdir),
             "--cwd", str(self.workspace), "--json", *map(str, arguments)],
            env=self.environment, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode == 0, ok, result.stderr)
        return json.loads(result.stdout) if ok else result.stderr

    def skill(self, parent, name="sample", description="A sample task", extra="", body="Body"):
        package = parent / name
        package.mkdir(parents=True)
        (package / "SKILL.md").write_text(
            f"---\nname: {name}\ndescription: {description}\n{extra}---\n{body}\n")
        return package

class Skills(SkillsCase):

    def test_default_cli_reports_are_readable_and_json_is_explicit(self):
        package = self.skill(self.dotdir / "skills", body="VISIBLE-SOURCE-BODY")
        commands = [("list",), ("show", "sample"), ("paths",), ("validate", package),
            ("create", self.workspace / "created"), ("install", package),
            ("update", "sample"), ("remove", "sample")]
        titles = ["Available skills (1 of 1):", "Skill: sample", "Skill paths:",
            "Valid skill: sample", "Created skill: created", "Installed skill: sample",
            "Updated skill: sample", "Removed skill: sample"]
        for arguments, title in zip(commands, titles):
            with self.subTest(command=arguments[0]):
                result = subprocess.run([BINARY, "skills", "--dotdir", str(self.dotdir),
                    "--cwd", str(self.workspace), *map(str, arguments)], env=self.environment,
                    capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(result.stdout.startswith(title), result.stdout)
                self.assertNotIn('"entries":', result.stdout)
                self.assertNotIn('"digest":', result.stdout)
                if arguments[0] == "show":
                    self.assertIn("VISIBLE-SOURCE-BODY", result.stdout)
                else:
                    self.assertNotIn("VISIBLE-SOURCE-BODY", result.stdout)
        self.assertIn("entries", self.run_cli("list"))

    def test_yaml_validation_and_isolation(self):
        roots = self.dotdir / "skills"
        package = self.skill(roots, description="|\n  Unicode café and bundled resources")
        self.assertEqual(self.run_cli("validate", package)["name"], "sample")
        bad = self.skill(roots, "broken", extra="name: duplicate\n")
        self.assertIn("duplicate", self.run_cli("validate", bad, ok=False))
        result = self.run_cli("list")
        self.assertEqual([x["name"] for x in result["entries"]], ["sample"])
        self.assertEqual(len(result["diagnostics"]), 1)
        self.assertNotIn("body", result["entries"][0])
        self.assertIn("Body", self.run_cli("show", "sample")["body"])

    def test_collection_collisions_and_package_boundary(self):
        root = self.dotdir / "skills"
        first = self.skill(root / "a")
        self.skill(root / "b")
        self.skill(first / "references", "nested-example")
        result = self.run_cli("list")
        self.assertEqual(result["total"], 2)
        self.assertIn("ambiguous", self.run_cli("show", "sample", ok=False))
        for entry in result["entries"]:
            self.assertEqual(self.run_cli("show", entry["id"])["name"], "sample")

    def test_discovery_never_executes(self):
        marker = self.root / "executed"
        self.skill(self.dotdir / "skills", body=f"!`touch {marker}`")
        self.run_cli("list")
        skill = self.run_cli("show", "sample")
        self.assertTrue(skill["unsupported_execution"])
        self.assertFalse(marker.exists())

    def test_standard_types_unicode_lengths_and_instruction_origins(self):
        package = self.skill(self.dotdir / "skills", description='"' + "é" * 1024 + '"')
        self.run_cli("validate", package)
        skill_path = package / "SKILL.md"
        for field in ("allowed-tools: [exec_command]", "metadata: {author: [bad]}",
                "compatibility: " + "x" * 501, "user-invocable: maybe"):
            skill_path.write_text(
                f"---\nname: sample\ndescription: Task\n{field}\n---\nBody\n")
            self.run_cli("validate", package, ok=False)
        external = self.root / "external-instructions"
        external.write_text("---\nname: sample\ndescription: Task\n---\nBody\n")
        skill_path.unlink()
        skill_path.symlink_to(external)
        self.assertIn("inside its skill package", self.run_cli("validate", package, ok=False))

    def test_settings_require_private_owner_and_checked_types(self):
        settings = self.dotdir / "skills.json"
        settings.write_text('{"roots": []}')
        settings.chmod(0o644)
        self.assertIn("private", self.run_cli("list", ok=False))
        settings.chmod(0o600)
        for invalid in ({"version": 99}, {"installed": []}):
            settings.write_text(json.dumps(invalid))
            self.run_cli("list", ok=False)

    def test_validate_checks_resource_links_and_vendor_icons(self):
        package = self.skill(self.dotdir / "skills", body="[Guide](references/guide.md#usage)")
        self.assertIn("resource", self.run_cli("validate", package, ok=False))
        (package / "references").mkdir()
        (package / "references/guide.md").write_text("# Usage\n")
        self.run_cli("validate", package)
        (package / "SKILL.md").write_text(
            "---\nname: sample\ndescription: Sample\n---\n"
            "[remote](https://example.invalid/doc) [anchor](#usage)\n"
            "```markdown\n[example](not-installed.md)\n```\n"
            "`[inline example](not-installed.md)`\n")
        self.run_cli("validate", package)
        agents = package / "agents"
        agents.mkdir()
        vendor = agents / "openai.yaml"
        vendor.write_text('interface:\n  icon_small: "./assets/missing.svg"\n')
        self.assertIn("resource", self.run_cli("validate", package, ok=False))
        vendor.write_text('interface:\n  icon_small: "../outside.svg"\n')
        self.assertIn("resource", self.run_cli("validate", package, ok=False))
        (package / "assets").mkdir()
        (package / "assets/icon.svg").write_text("<svg/>\n")
        vendor.write_text('interface:\n  icon_small: "./assets/icon.svg"\n')
        self.run_cli("validate", package)

    def test_create_never_overwrites(self):
        destination = self.root / "new-skill"
        result = self.run_cli("create", destination)
        self.assertEqual(result["name"], "new-skill")
        original = (destination / "SKILL.md").read_bytes()
        self.run_cli("create", destination, ok=False)
        self.assertEqual((destination / "SKILL.md").read_bytes(), original)

    def test_managed_install_update_remove_and_old_generation(self):
        source = self.skill(self.root / "source", body="Version one")
        installed = self.run_cli("install", source)
        first = Path(installed["root"])
        self.assertIn("Version one", (first / "SKILL.md").read_text())
        entry, = self.run_cli("list")["entries"]
        self.assertEqual(entry["state"], "available")
        self.run_cli("enable", entry["id"], ok=False)
        (source / "SKILL.md").write_text(
            "---\nname: sample\ndescription: New metadata\n---\nVersion two\n")
        updated = self.run_cli("update", "sample")
        self.assertNotEqual(first, Path(updated["root"]))
        current, = self.run_cli("list")["entries"]
        self.assertEqual(current["id"], entry["id"])
        self.assertEqual(current["state"], "available")
        self.assertIn("Version one", (first / "SKILL.md").read_text())
        self.run_cli("remove", "sample")
        self.assertEqual(self.run_cli("list")["total"], 0)
        self.assertTrue(first.is_dir())
        self.assertTrue(source.is_dir())

    def test_managed_copy_refuses_escape_and_special_file(self):
        source = self.skill(self.root / "source")
        (source / "escape").symlink_to(self.root / "outside")
        self.assertIn("package", self.run_cli("install", source, ok=False))
        self.assertEqual(self.run_cli("list")["total"], 0)
        (source / "escape").unlink()
        os.mkfifo(source / "pipe")
        self.assertIn("special", self.run_cli("install", source, ok=False))
        self.assertEqual(self.run_cli("list")["total"], 0)

    def test_managed_git_pins_commit_and_no_hooks(self):
        checkout = self.root / "checkout"
        package = self.skill(checkout / "collection")
        subprocess.run(["git", "init", "-q", str(checkout)], check=True)
        subprocess.run(["git", "-C", str(checkout), "add", "."], check=True)
        subprocess.run(["git", "-C", str(checkout), "-c", "user.name=Fixture",
            "-c", "user.email=fixture@example.test", "commit", "-qm", "fixture"], check=True)
        commit = subprocess.check_output(["git", "-C", str(checkout),
            "rev-parse", "HEAD"], text=True).strip()
        installed = self.run_cli("install", "--git", checkout,
            "--subdir", "collection/sample", "--ref", commit)
        self.assertEqual(installed["commit"], commit)
        self.assertEqual(installed["subdir"], "collection/sample")
        self.assertTrue((Path(installed["root"]) / "SKILL.md").is_file())
        self.assertFalse((Path(installed["root"]) / ".git").exists())
        self.assertTrue(package.exists())

    def test_failed_update_retains_published_package(self):
        source = self.skill(self.root / "source")
        first = self.run_cli("install", source)
        (source / "SKILL.md").write_text("invalid")
        self.run_cli("update", "sample", ok=False)
        entry, = self.run_cli("list")["entries"]
        self.assertEqual(entry["root"], first["root"])

class ProjectDiscovery(SkillsCase):
    def test_project_precedence_and_canonical_aliases(self):
        (self.workspace / ".git").mkdir()
        package = self.skill(self.workspace / ".claude/skills", body="Project body")
        (self.workspace / ".agents").mkdir()
        (self.workspace / ".agents/skills").symlink_to(
            self.workspace / ".claude/skills", target_is_directory=True)
        self.skill(self.dotdir / "skills", body="Global body")
        entries = self.run_cli("list")["entries"]
        self.assertEqual(len(entries), 2)
        selected = [entry for entry in entries if entry["selected"]]
        self.assertEqual(len(selected), 1)
        self.assertEqual(selected[0]["root"], str(package))
        self.assertIn("Project body", self.run_cli("show", "sample")["body"])
        self.assertTrue(all("body" not in entry and "metadata" not in entry
            for entry in entries))

    def test_nearest_project_skills_win_and_stop_at_repository_root(self):
        (self.workspace / ".git").mkdir()
        self.skill(self.workspace / ".snajpagent/skills", body="Root body")
        child = self.workspace / "child"
        child.mkdir()
        nearest = self.skill(child / ".agents/skills", body="Nearest body")
        self.skill(self.root / ".agents/skills", "outside")
        self.workspace = child
        self.assertEqual(self.run_cli("show", "sample")["root"], str(nearest))
        self.assertNotIn("outside", [entry["name"] for entry in
            self.run_cli("list")["entries"]])

    def test_collection_links_and_cycles_need_no_admission(self):
        root = self.dotdir / "skills"
        local = self.skill(root)
        external = self.root / "external"
        outside = self.skill(external, "outside")
        (root / "cycle").symlink_to(root, target_is_directory=True)
        (root / "collection").symlink_to(external, target_is_directory=True)
        (root / "duplicate").symlink_to(outside, target_is_directory=True)
        result = self.run_cli("list")
        self.assertEqual(sorted(entry["name"] for entry in result["entries"]),
            ["outside", "sample"])
        self.assertEqual(self.run_cli("show", "outside")["root"], str(outside))
        self.assertTrue(local.is_dir())

    def test_legacy_curation_records_are_inert_and_commands_removed(self):
        self.skill(self.dotdir / "skills")
        policy = self.dotdir / "skills.json"
        policy.write_text(json.dumps({"version": 1, "banned": ["sample"],
            "disabled": ["sample"], "approvals": [], "reviews": "obsolete"}))
        policy.chmod(0o600)
        self.assertEqual(self.run_cli("list")["total"], 1)
        self.assertIn("Body", self.run_cli("show", "sample")["body"])
        for command in ("enable", "disable", "ban", "unban", "approve", "review"):
            self.run_cli(command, "sample", ok=False)
        self.run_cli("list", "--all-skills", ok=False)
        self.assertEqual(json.loads(policy.read_text())["banned"], ["sample"])

    def test_configured_roots_replace_global_not_project_discovery(self):
        self.skill(self.dotdir / "skills", "global")
        self.skill(self.dotdir / "extra", "configured")
        self.skill(self.workspace / ".agents/skills", "project")
        config = self.dotdir / "config.ini"
        config.write_text('[skills]\nroots = ["extra"]\n')
        config.chmod(0o600)
        self.assertEqual([entry["name"] for entry in self.run_cli("list")["entries"]],
            ["project", "configured"])
        config.write_text('[skills]\nroots = []\n')
        self.assertEqual([entry["name"] for entry in self.run_cli("list")["entries"]],
            ["project"])

    def test_unavailable_project_path_keeps_global_skills(self):
        self.skill(self.dotdir / "skills", "global")
        original = self.workspace
        for replacement in ("missing", "file", "file/child"):
            self.workspace = self.root / replacement
            if replacement == "file":
                self.workspace.write_text("Saved directory was replaced.\n")
            with self.subTest(replacement=replacement):
                self.assertEqual([entry["name"] for entry in self.run_cli("list")["entries"]],
                    ["global"])
        self.workspace = original

    def test_discovery_is_lazy_but_explicit_validation_checks_resources(self):
        package = self.skill(self.dotdir / "skills")
        os.mkfifo(package / "pipe")
        self.assertEqual(self.run_cli("list")["total"], 1)
        self.assertIn("Body", self.run_cli("show", "sample")["body"])
        self.assertIn("special", self.run_cli("validate", package, ok=False))

class RuntimeSkills(SkillsCase):
    def runtime_events(self):
        from tmux_terminal import journal_events, journal_paths
        path = max(journal_paths(self.dotdir), key=lambda item: item.stat().st_mtime_ns)
        return journal_events(path)

    def configured_provider(self):
        import tmux_terminal as harness
        provider = harness.FakeResponses()
        self.addCleanup(provider.close)
        config = self.dotdir / "config.ini"
        config.write_text(
            "[agent]\nprovider=fixture\nmodel=host-model\nmax_turn_retries=0\n"
            "[provider fixture]\n"
            f"base_url=http://127.0.0.1:{provider.port}\n"
            'api_key="irc-ui-secret"\nexact_token_count=false\n'
            "[ui]\ncolor=never\nmarkdown=false\npager=off\n")
        config.chmod(0o600)
        return provider

    def execute(self, prompt=None, session=None):
        arguments = [BINARY, "--dotdir", str(self.dotdir), "--no-listen", "--no-client"]
        if session:
            arguments.extend(["--resume", session])
        arguments.append("-e")
        if prompt is not None:
            arguments.extend(["--", prompt])
        return subprocess.run(arguments, cwd=self.workspace, env=self.environment,
            input="", capture_output=True, text=True, timeout=30)

    def test_prices_variables_and_unknown_dollar_words_stay_literal(self):
        provider = self.configured_provider()
        requests = []
        prompt = "Check $5k and $500, $HOME, $missing, ${PATH}, `code $sample`."
        def respond(handler, request, sequence):
            requests.append(request)
            provider.reply(handler, provider.response_body(sequence, "literal request done").encode(),
                close_header=True)
        provider.runtime_handler = respond
        result = self.execute(prompt)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(requests), 1)
        self.assertIn(prompt, json.dumps(requests[0]["input"]))

    def test_human_skills_empty_state_ignores_catalog_inspection(self):
        from tmux_terminal import TmuxTerminal
        provider = self.configured_provider()
        self.skill(self.dotdir / "skills", "pending-package", body="UNLOADED-BODY")
        with TmuxTerminal(self.root / "summary", BINARY,
                self.workspace, self.dotdir, self.dotdir / "config.ini", 140, 42,
                args=("--no-listen", "--no-client"), environment=self.environment) as terminal:
            terminal.wait("host-model")
            screen = terminal.submit_wait("/skills   ", "No skills loaded.")
            self.assertIn("/skills list", screen)
            self.assertNotIn("pending-package", screen)
            self.assertNotIn('"entries"', screen)
        self.assertEqual(provider.requests, [])

    def test_human_skills_lists_loaded_bindings_and_clears_explicitly(self):
        import shutil
        from tmux_terminal import TmuxTerminal
        provider = self.configured_provider()
        package = self.skill(self.dotdir / "skills", body="PRIVATE-PINNED-GUIDANCE")
        entry = self.run_cli("list")["entries"][0]
        self.skill(self.dotdir / "skills", "unloaded-package", body="UNLOADED-BODY")

        def respond(handler, request, sequence):
            provider.reply(handler, provider.response_body(sequence, "GUIDANCE_DONE").encode(),
                close_header=True)

        provider.runtime_handler = respond
        with TmuxTerminal(self.root / "loaded-summary", BINARY, self.workspace, self.dotdir,
                self.dotdir / "config.ini", 140, 42,
                args=("--no-listen", "--no-client"),
                environment=self.environment) as terminal:
            terminal.wait("host-model")
            terminal.submit_wait("Use $sample to answer.", "GUIDANCE_DONE")
            shutil.rmtree(package)
            self.assertEqual(self.run_cli("list")["total"], 1)
            screen = terminal.submit_wait("/skills", "Loaded skills (1):", join_wrapped=True)
            summary = screen.rsplit("Loaded skills (1):", 1)[1]
            self.assertIn("sample", summary)
            self.assertIn(entry["id"], summary)
            self.assertNotIn("unloaded-package", summary)
            self.assertNotIn("PRIVATE-PINNED-GUIDANCE", summary)
            self.assertNotIn('"entries"', summary)
            screen = terminal.submit_wait("/skills clear sample", "• Skill unloaded: sample")
            self.assertEqual(screen.count("• Skill unloaded: sample"), 1)
            self.assertNotIn('"action": "clear"', screen)
            terminal.submit_wait("/skills clear sample", "• No skills unloaded.")
            terminal.submit("/skills")
            terminal.wait_until(lambda text: text.count("No skills loaded.") == 1,
                "empty loaded-skills summary after explicit clear")
            self.assertEqual(len(provider.requests), 1)

    def test_skill_unload_by_id_and_all_has_colored_bullets(self):
        from tmux_terminal import TmuxTerminal
        provider = self.configured_provider()
        self.skill(self.dotdir / "skills", body="SAMPLE-GUIDANCE")
        self.skill(self.dotdir / "skills", "sibling", body="SIBLING-GUIDANCE")
        entry = next(item for item in self.run_cli("list")["entries"]
            if item["name"] == "sample")
        requests = []

        def respond(handler, request, sequence):
            requests.append(request)
            provider.reply(handler, provider.response_body(sequence,
                f"GUIDANCE_DONE_{len(requests)}").encode(), close_header=True)

        provider.runtime_handler = respond
        for view in ("chat", "rollout"):
            with self.subTest(view=view), TmuxTerminal(self.root / view, BINARY,
                    self.workspace, self.dotdir, self.dotdir / "config.ini", 160, 48,
                    args=("--no-listen", "--no-client", "--color=always", "--view", "rollout"),
                    environment=self.environment) as terminal:
                terminal.wait("session id")
                terminal.submit_wait("Use $sample and $sibling.",
                    f"GUIDANCE_DONE_{len(requests) + 1}")
                terminal.submit("/" + view)
                notice = "• Skill unloaded: " + entry["id"]
                terminal.submit("/skills clear " + entry["id"])
                if view == "chat":
                    terminal.submit("/rollout")
                screen = terminal.wait(notice, join_wrapped=True)
                self.assertEqual(screen.count(notice), 1)
                self.assertRegex(terminal.capture_styled(), r"\x1b\[[0-9;]*32m•")
                screen = terminal.submit_wait("/skills", "Loaded skills (1):", join_wrapped=True)
                summary = screen.rsplit("Loaded skills (1):", 1)[1]
                self.assertIn("sibling", summary)
                self.assertNotIn(entry["id"], summary)
                screen = terminal.submit_wait("/skills clear", "• Skills unloaded.")
                self.assertEqual(screen.count("• Skills unloaded."), 1)
                terminal.submit_wait("/skills clear", "• No skills unloaded.")
                terminal.submit("/rollout")
                terminal.submit_wait("Continue without guidance.",
                    f"GUIDANCE_DONE_{len(requests) + 1}")
                guidance = [message for message in requests[-1]["input"]
                    if message.get("role") in ("system", "developer")]
                self.assertNotIn("SAMPLE-GUIDANCE", json.dumps(guidance))
                self.assertNotIn("SIBLING-GUIDANCE", json.dumps(guidance))

    def test_interactive_skill_inspection_and_management_are_readable(self):
        from tmux_terminal import TmuxTerminal
        provider = self.configured_provider()
        package = self.skill(self.dotdir / "skills", body="VISIBLE-SOURCE-BODY")
        for view in ("chat", "rollout"):
            with self.subTest(view=view), TmuxTerminal(self.root / ("inspect-" + view), BINARY,
                    self.workspace, self.dotdir, self.dotdir / "config.ini", 160, 48,
                    args=("--no-listen", "--no-client", "--view", view),
                    environment=self.environment) as terminal:
                terminal.wait("session id")
                for command, expected in [
                    ("/skills list", "Available skills (1 of 1):"),
                    ("/skills show sample", "VISIBLE-SOURCE-BODY"),
                    ("/skills refresh", "Available skills (1 of 1):"),
                    ("/skills paths", "Skill paths:"),
                    (f"/skills validate {package}", "Valid skill: sample"),
                    (f"/skills install {package}", "Installed skill: sample"),
                    ("/skills update sample", "Updated skill: sample"),
                    ("/skills remove sample", "Removed skill: sample"),
                ]:
                    screen = terminal.submit_wait(command, expected)
                    self.assertNotIn('"entries":', screen)
                    self.assertNotIn('"digest":', screen)
                    self.assertNotIn('"generations_retained":', screen)
        self.assertEqual(provider.requests, [])

    def test_model_can_load_unload_and_reload_skill_guidance(self):
        provider = self.configured_provider()
        self.skill(self.dotdir / "skills", body="LIFECYCLE-GUIDANCE")
        entry = self.run_cli("list")["entries"][0]
        for selector in ("sample", entry["id"]):
            requests = []

            def respond(handler, request, sequence):
                requests.append(request)
                actions = [
                    {"action": "activate", "id": "sample"},
                    {"action": "deactivate"},
                    {"action": "deactivate", "id": selector},
                    {"action": "deactivate", "id": selector},
                    {"action": "activate", "id": "sample"},
                ]
                if len(requests) <= len(actions):
                    body = provider.function_body(sequence, f"skill-{len(requests)}", "skills",
                        actions[len(requests) - 1])
                else:
                    body = provider.response_body(sequence, "lifecycle complete")
                provider.reply(handler, body.encode(), close_header=True)

            provider.runtime_handler = respond
            result = self.execute("Manage skill guidance with tools.")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(len(requests), 6, result.stderr)
            schema = next(tool for tool in requests[0]["tools"] if tool.get("name") == "skills")
            self.assertIn("deactivate", schema["parameters"]["properties"]["action"]["enum"])
            for index, expected in [(1, True), (2, True), (3, False), (4, False), (5, True)]:
                guidance = [message for message in requests[index]["input"]
                    if message.get("role") in ("system", "developer")]
                self.assertEqual("LIFECYCLE-GUIDANCE" in json.dumps(guidance), expected)
            for index, removed in [(3, 1), (4, 0)]:
                outputs = [item["output"] for item in requests[index]["input"]
                    if item.get("type") == "function_call_output"]
                self.assertEqual(json.loads(outputs[-1])["removed"], removed)
            self.assertEqual(result.stderr.count("• Skill loaded: sample"), 2)
            self.assertEqual(result.stderr.count("• Skill unloaded: " + selector), 1)
            self.assertEqual(result.stderr.count("• No skills unloaded."), 1)

    def test_price_alongside_available_skill_preserves_activation(self):
        provider = self.configured_provider()
        self.skill(self.dotdir / "skills", body="APPROVED-PRICE-GUIDANCE")
        requests = []
        def respond(handler, request, sequence):
            requests.append(request)
            provider.reply(handler, provider.response_body(sequence, "approved request done").encode(),
                close_header=True)
        provider.runtime_handler = respond
        result = self.execute("Use $sample and $sample to check $5k with $HOME.")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(requests), 1)
        self.assertIn("APPROVED-PRICE-GUIDANCE", json.dumps(requests[0]["input"]))
        self.assertEqual(result.stderr.count("• Skill loaded: sample"), 1)
        selected = self.execute("/skills use sample -- Check $5k.")
        self.assertEqual(selected.returncode, 0, selected.stderr)
        self.assertEqual(selected.stderr.count("• Skill loaded: sample"), 1)
        self.assertEqual(len(requests), 2)

    def test_explicit_missing_selection_is_recoverable_before_provider(self):
        from tmux_terminal import TmuxTerminal
        provider = self.configured_provider()
        requests = []
        def respond(handler, request, sequence):
            requests.append(request)
            provider.reply(handler, provider.response_body(sequence, "session still working").encode(),
                close_header=True)
        provider.runtime_handler = respond
        with TmuxTerminal(self.root / "missing-terminal", BINARY, self.workspace, self.dotdir,
                self.dotdir / "config.ini", 140, 42, args=("--no-listen", "--no-client"),
                environment=self.environment) as terminal:
            terminal.wait("host-model")
            terminal.submit_wait("/skills use missing -- request", "skill unavailable")
            self.assertEqual(len(requests), 0)
            terminal.submit_wait("Continue with the $5k price.", "session still working")
            self.assertEqual(len(requests), 1)

    def test_refused_selection_then_resume_accepts_price(self):
        provider = self.configured_provider()
        requests = []
        def respond(handler, request, sequence):
            requests.append(request)
            provider.reply(handler, provider.response_body(sequence, "resumed price done").encode(),
                close_header=True)
        provider.runtime_handler = respond
        refused = self.execute("/skills use missing -- request")
        self.assertEqual(refused.returncode, 4, refused.stderr)
        self.assertIn("skill unavailable", refused.stderr)
        self.assertNotIn("Skill loaded:", refused.stderr)
        self.assertEqual(len(requests), 0)
        session, = (self.dotdir / "sessions").iterdir()
        resumed = self.execute("Continue with $5k and $HOME.", session=session.name)
        self.assertEqual(resumed.returncode, 0, resumed.stderr)
        self.assertEqual(len(requests), 1)

    def test_skill_guidance_does_not_widen_readonly_tools(self):
        provider = self.configured_provider()
        self.skill(self.dotdir / "skills", body="READONLY-APPROVED-GUIDANCE",
            extra="allowed-tools: write_file exec_command\n")
        marker = self.workspace / "forbidden"
        requests = []
        def respond(handler, request, sequence):
            requests.append(request)
            if len(requests) == 1:
                body = provider.function_body(sequence, "denied-write", "write_file",
                    {"path": str(marker), "content": "not permitted"})
            else:
                body = provider.response_body(sequence, "readonly restriction observed")
            provider.reply(handler, body.encode(), close_header=True)
        provider.runtime_handler = respond
        result = self.execute("/ro inspect $sample")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(requests), 2)
        self.assertIn("READONLY-APPROVED-GUIDANCE", json.dumps(requests[0]["input"]))
        self.assertIn("read-only", json.dumps(requests[1]["input"]))
        self.assertFalse(marker.exists())

    def test_dependencies_use_actual_mcp_state_with_actual_authentication(self):
        import sys
        from unittest.mock import patch
        with patch.object(sys, "argv", [sys.argv[0]]):
            from test_mcp import MCPServer
        available, locked = MCPServer(), MCPServer()
        locked.oauth = True
        self.addCleanup(available.close)
        self.addCleanup(locked.close)
        provider = self.configured_provider()
        config = self.dotdir / "config.ini"
        with config.open("a") as stream:
            stream.write(f"[mcp available]\nurl={available.url}/mcp\ntimeout_ms=1000\n"
                f"[mcp locked]\nurl={locked.url}/mcp\nissuer={locked.url}\n"
                "client_id=fixture-client\ntimeout_ms=1000\n")
        package = self.skill(self.dotdir / "skills", extra="allowed-tools: send.message\n")
        (package / "agents").mkdir()
        (package / "agents/openai.yaml").write_text("dependencies:\n  tools:\n"
            f"    - {{type: mcp, value: available, url: '{available.url}/mcp'}}\n"
            f"    - {{type: mcp, value: locked, url: '{locked.url}/mcp'}}\n"
            "    - {type: mcp, value: absent}\n"
            "    - {type: mcp, value: available, url: 'https://invalid.example/mcp'}\n"
            "    - {type: mcp, value: available, transport: stdio}\n")
        requests = []
        def respond(handler, request, sequence):
            requests.append(request)
            if len(requests) == 1:
                body = provider.function_body(sequence, "activate", "skills",
                    {"action": "activate", "id": "sample"})
            elif len(requests) == 2:
                tools = [item for item in request["tools"] if item.get("name", "").startswith("mcp_")]
                self.assertEqual(len(tools), 1)
                body = provider.function_body(sequence, "unauthorized-effect", tools[0]["name"],
                    {"destination": "channel-42", "message": "must not send"})
            else:
                body = provider.response_body(sequence, "MCP boundary observed")
            provider.reply(handler, body.encode(), close_header=True)
        provider.runtime_handler = respond
        result = self.execute("inspect approved dependency declarations")
        self.assertEqual(result.returncode, 0, result.stderr)
        outputs = [item["output"] for item in requests[1]["input"]
            if item.get("type") == "function_call_output"]
        receipt = json.loads(outputs[-1])
        self.assertEqual([item["state"] for item in receipt["dependencies"]],
            ["available", "unauthorized", "missing", "endpoint_mismatch", "unsupported"])
        self.assertIn("requires exact operator approval", json.dumps(requests[2]["input"]))
        from tmux_terminal import event_list
        result = event_list(self.runtime_events(), "tool_finished")[-1]["data"]["result"]
        self.assertEqual(result["reason"], "mcp_approval_required")
        self.assertEqual(available.calls, [])
        self.assertEqual(locked.calls, [])
        self.assertEqual(locked.tokens, [])

    def test_read_only_turn_preserves_existing_bindings(self):
        from tmux_terminal import TmuxTerminal, event_list
        provider = self.configured_provider()
        self.skill(self.dotdir / "skills", body="ONGOING-SKILL-BODY")
        requests = []
        def respond(handler, request, sequence):
            requests.append(request)
            if len(requests) == 1:
                body = provider.function_body(sequence, "activate", "skills",
                    {"action": "activate", "id": "sample"})
            else:
                body = provider.response_body(sequence, f"response-{len(requests)}")
            provider.reply(handler, body.encode(), close_header=True)
        provider.runtime_handler = respond
        with TmuxTerminal(self.root / "readonly-terminal", BINARY, self.workspace, self.dotdir,
                self.dotdir / "config.ini", 140, 42, args=("--no-listen", "--no-client"),
                environment=self.environment) as terminal:
            terminal.wait("host-model")
            terminal.submit_wait("use approved sample", "response-2")
            before = self.runtime_events()
            active = event_list(before, "skills_updated")[-1]["data"]["active"]
            self.assertEqual(len(active), 1)
            terminal.submit_wait("/ro inspect state only", "response-3")
            after = self.runtime_events()
            self.assertEqual(event_list(after, "skills_updated")[-1]["data"]["active"], active)
            terminal.submit_wait("new ordinary task", "response-4")
            after = self.runtime_events()
            self.assertEqual(event_list(after, "skills_updated")[-1]["data"]["active"], [])

    def test_resume_pins_exact_instructions_after_source_deletion(self):
        provider = self.configured_provider()
        package = self.skill(self.dotdir / "skills", body="EXACT-PIN $ARGUMENTS")
        requests = []
        def fail_after_activation(handler, request, sequence):
            requests.append(request)
            if len(requests) == 1:
                body = provider.function_body(sequence, "activate", "skills",
                    {"action": "activate", "id": "sample", "arguments": "literal"})
                provider.reply(handler, body.encode(), close_header=True)
            else:
                provider.reply(handler, b'{"error":{"message":"fixture unavailable"}}',
                    status=503, close_header=True)
        provider.runtime_handler = fail_after_activation
        failed = self.execute("use sample")
        self.assertNotEqual(failed.returncode, 0)
        self.assertGreaterEqual(len(requests), 2, failed.stderr)
        self.assertIn("EXACT-PIN literal", json.dumps(requests[1]["input"]))
        sessions = list((self.dotdir / "sessions").iterdir())
        session = max(sessions, key=lambda path: path.stat().st_mtime_ns)
        (package / "SKILL.md").unlink()
        package.rmdir()
        requests.clear()
        def resumed(handler, request, sequence):
            requests.append(request)
            provider.reply(handler, provider.response_body(sequence, "recovered").encode(),
                close_header=True)
        provider.runtime_handler = resumed
        result = self.execute("/retry", session=session.name)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(requests, (result.stdout, result.stderr))
        pins = [m for m in requests[0]["input"] if
            "Active skill snapshot:" in json.dumps(m)]
        self.assertEqual(len(pins), 1, json.dumps(requests[0]["input"]) + result.stderr)
        self.assertIn("EXACT-PIN literal", json.dumps(pins))
        # Resume retains completed tool history; it must not need a new activation.
        self.assertEqual(len(requests), 1)
        calls = [m for m in requests[0]["input"] if
            m.get("type") == "function_call" and m.get("name") == "skills"]
        self.assertEqual(len(calls), 1)

    def test_real_provider_activation_and_next_task_retirement(self):
        import tmux_terminal as harness
        provider = harness.FakeResponses()
        self.addCleanup(provider.close)
        package = self.skill(self.dotdir / "skills", body="PINNED-SKILL-BODY $ARGUMENTS")
        config = self.dotdir / "config.ini"
        config.write_text(
            "[agent]\nprovider=fixture\nmodel=host-model\nmax_turn_retries=0\n"
            "[provider fixture]\n"
            f"base_url=http://127.0.0.1:{provider.port}\n"
            'api_key="irc-ui-secret"\nexact_token_count=false\n'
            "[ui]\ncolor=never\nmarkdown=false\npager=off\n")
        config.chmod(0o600)
        requests = []

        def respond(handler, request, sequence):
            requests.append(request)
            if len(requests) == 1:
                body = provider.function_body(sequence, "activate-skill", "skills",
                    {"action": "activate", "id": "sample", "arguments": "literal $0"})
            else:
                body = provider.response_body(sequence, "skill workflow complete")
            provider.reply(handler, body.encode(), close_header=True)

        provider.runtime_handler = respond
        common = [BINARY, "--dotdir", str(self.dotdir), "--no-listen", "--no-client"]
        first = subprocess.run([*common, "-e", "--", "use the sample task"],
            cwd=self.workspace, env=self.environment, capture_output=True, text=True, timeout=30)
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(first.stderr.count("• Skill loaded: sample"), 1)
        self.assertEqual(len(requests), 2)
        initial = json.dumps(requests[0]["input"])
        self.assertIn("Available task skills", initial)
        self.assertNotIn("PINNED-SKILL-BODY", initial)
        self.assertIn("PINNED-SKILL-BODY literal $0", json.dumps(requests[1]["input"]))
        sessions = list((self.dotdir / "sessions").iterdir())
        session = max(sessions, key=lambda path: path.stat().st_mtime_ns)
        (package / "SKILL.md").unlink()
        package.rmdir()
        requests.clear()
        def final_only(handler, request, sequence):
            requests.append(request)
            provider.reply(handler, provider.response_body(sequence, "new task complete").encode(),
                close_header=True)
        provider.runtime_handler = final_only
        second = subprocess.run([*common, "--resume", session.name, "-e", "--", "new task"],
            cwd=self.workspace, env=self.environment, capture_output=True, text=True, timeout=30)
        self.assertEqual(second.returncode, 0, second.stderr)
        active = [message for message in requests[0]["input"]
            if message.get("role") in ("system", "developer") and
            "Active skill snapshot:" in json.dumps(message)]
        self.assertFalse(active, active)
        # Completed tool history remains, but does not become an active snapshot.

if __name__ == "__main__":
    unittest.main()
