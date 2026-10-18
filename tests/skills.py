#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the real skills CLI and its filesystem/trust boundaries."""
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

    def operator_approve(self, review, *, digest=None, accepted=True):
        from tmux_terminal import TmuxTerminal
        with TmuxTerminal(self.root / f"terminal-{time.monotonic_ns()}", BINARY,
                self.workspace, self.dotdir, self.dotdir / "config.ini", 140, 42,
                args=("--no-listen", "--no-client"), environment=self.environment) as terminal:
            terminal.wait("host-model")
            selected = digest or review["digest"]
            terminal.send_text(f'/skills approve {review["id"]} --digest {selected}')
            terminal.send_key("Enter")
            if not accepted:
                terminal.wait("approval refused")
                return
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                policy = json.loads((self.dotdir / "skills.json").read_text())
                if policy.get("approvals", {}).get(review["id"], {}).get("digest") == selected:
                    return
                time.sleep(0.025)
            self.fail("operator approval did not become durable: " + terminal.capture())


class Skills(SkillsCase):

    def test_yaml_validation_and_isolation(self):
        roots = self.dotdir / "skills"
        package = self.skill(roots, description="|\n  Unicode café and bundled resources")
        self.assertEqual(self.run_cli("validate", package)["name"], "sample")
        bad = self.skill(roots, "broken", extra="name: duplicate\n")
        self.assertIn("duplicate", self.run_cli("validate", bad, ok=False))
        result = self.run_cli("list", "--all-skills")
        self.assertEqual([x["name"] for x in result["entries"]], ["sample"])
        self.assertEqual(len(result["diagnostics"]), 1)
        self.assertNotIn("body", result["entries"][0])
        self.assertIn("Body", self.run_cli("show", "--all-skills", "sample")["body"])

    def test_configured_candidate_pagination_never_grants_trust(self):
        self.skill(self.dotdir / "skills", description="Global")
        self.skill(self.dotdir / "extra", description="Configured")
        config = self.dotdir / "config.ini"
        config.write_text('[skills]\nroots = ["extra", "skills"]\n')
        config.chmod(0o600)
        result = self.run_cli("list", "--all-skills", "--limit", "1")
        self.assertEqual((result["total"], result["remaining"]), (2, 1))
        self.assertFalse(result["entries"][0]["trusted"])
        self.assertFalse(result["entries"][0]["approved"])
        self.assertEqual(self.run_cli("list")["total"], 0)
        self.run_cli("trust", ok=False)
        self.assertEqual(self.run_cli("list")["total"], 0)

    def test_collection_collisions_and_package_boundary(self):
        root = self.dotdir / "skills"
        first = self.skill(root / "a")
        self.skill(root / "b")
        self.skill(first / "references", "nested-example")
        result = self.run_cli("list", "--all-skills")
        self.assertEqual(result["total"], 2)
        self.assertIn("ambiguous", self.run_cli("show", "--all-skills", "sample", ok=False))
        for entry in result["entries"]:
            self.assertEqual(self.run_cli("show", "--all-skills", entry["id"])["name"], "sample")

    def test_link_cycles_and_external_origins(self):
        root = self.dotdir / "skills"
        first = self.skill(root)
        (root / "cycle").symlink_to(root, target_is_directory=True)
        outside = self.skill(self.root / "external", "outside")
        (root / "linked").symlink_to(outside, target_is_directory=True)
        result = self.run_cli("list", "--all-skills")
        self.assertEqual(result["total"], 2)
        entry = next(x for x in result["entries"] if x["name"] == "outside")
        self.assertFalse(entry["origin_admitted"])
        self.run_cli("admit-origin", outside, ok=False)
        result = self.run_cli("list", "--all-skills")
        entry = next(x for x in result["entries"] if x["name"] == "outside")
        self.assertFalse(entry["origin_admitted"])
        self.assertEqual(self.run_cli("list")["total"], 0)
        self.assertTrue(first.is_dir())

    def test_discovery_never_executes(self):
        marker = self.root / "executed"
        self.skill(self.dotdir / "skills", body=f"!`touch {marker}`")
        self.run_cli("list", "--all-skills")
        skill = self.run_cli("show", "--all-skills", "sample")
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
        self.assertIn("escapes", self.run_cli("validate", package, ok=False))

    def test_external_collection_requires_admission_before_recursion(self):
        external = self.root / "outside"
        self.skill(external / "deep")
        root = self.dotdir / "skills"
        root.mkdir()
        (root / "external").symlink_to(external, target_is_directory=True)
        result = self.run_cli("list", "--all-skills")
        self.assertEqual(result["total"], 0)
        self.assertTrue(result["diagnostics"])
        config = self.dotdir / "config.ini"
        config.write_text('[skills]\nroots = ' + json.dumps([str(external)]) + '\n')
        config.chmod(0o600)
        result = self.run_cli("list", "--all-skills")
        self.assertEqual(result["total"], 1)
        self.assertTrue(result["entries"][0]["origin_admitted"])

    def test_settings_require_private_owner_and_checked_types(self):
        settings = self.dotdir / "skills.json"
        settings.write_text('{"roots": []}')
        settings.chmod(0o644)
        self.assertIn("private", self.run_cli("list", ok=False))
        settings.chmod(0o600)
        for invalid in ({"banned": "arbitrary"}, {"installed": []}, {"disabled": [3]}):
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
        entry, = self.run_cli("list", "--all-skills")["entries"]
        self.assertFalse(entry["approved"])
        self.assertEqual(entry["state"], "pending")
        self.run_cli("enable", entry["id"], ok=False)
        (source / "SKILL.md").write_text(
            "---\nname: sample\ndescription: New metadata\n---\nVersion two\n")
        updated = self.run_cli("update", "sample")
        self.assertNotEqual(first, Path(updated["root"]))
        current, = self.run_cli("list", "--all-skills")["entries"]
        self.assertEqual(current["id"], entry["id"])
        self.assertFalse(current["approved"], "updates need exact package approval")
        self.assertIn("Version one", (first / "SKILL.md").read_text())
        self.run_cli("remove", "sample")
        self.assertEqual(self.run_cli("list", "--all-skills")["total"], 0)
        self.assertTrue(first.is_dir())
        self.assertTrue(source.is_dir())

    def test_managed_copy_refuses_escape_and_special_file(self):
        source = self.skill(self.root / "source")
        (source / "escape").symlink_to(self.root / "outside")
        self.assertIn("package", self.run_cli("install", source, ok=False))
        self.assertEqual(self.run_cli("list", "--all-skills")["total"], 0)
        (source / "escape").unlink()
        os.mkfifo(source / "pipe")
        self.assertIn("special", self.run_cli("install", source, ok=False))
        self.assertEqual(self.run_cli("list", "--all-skills")["total"], 0)

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
        entry, = self.run_cli("list", "--all-skills")["entries"]
        self.assertEqual(entry["root"], first["root"])


class CuratedPolicy(SkillsCase):
    def test_default_roots_are_dotdir_only(self):
        self.skill(self.dotdir / "skills", "local")
        self.skill(self.workspace / ".agents/skills", "workspace-only")
        self.skill(self.home / ".agents/skills", "home-only")
        entries = self.run_cli("list", "--all-skills")["entries"]
        self.assertEqual([entry["name"] for entry in entries], ["local"])
        paths = self.run_cli("paths")["roots"]
        self.assertEqual([entry["path"] for entry in paths],
            [str(self.dotdir / "skills")])
        self.assertTrue(paths[0]["default"])

    def test_pending_contents_are_hidden(self):
        self.skill(self.dotdir / "skills", description="HOSTILE-METADATA")
        self.assertEqual(self.run_cli("list")["total"], 0)
        self.assertIn("unavailable", self.run_cli("show", "sample", ok=False))
        pending, = self.run_cli("list", "--all-skills")["entries"]
        self.assertEqual(pending["state"], "pending")
        self.assertFalse(pending["approved"])
        self.assertEqual(self.run_cli("list")["total"], 0)

    def test_configured_paths_and_empty_roots(self):
        self.skill(self.dotdir / "skills", "unselected")
        self.skill(self.dotdir / "extra", "configured")
        config = self.dotdir / "config.ini"
        config.write_text('[skills]\nroots = ["extra"]\n')
        config.chmod(0o600)
        paths = self.run_cli("paths")["roots"]
        self.assertEqual([entry["path"] for entry in paths], [str(self.dotdir / "extra")])
        self.assertFalse(paths[0]["default"])
        self.assertEqual([entry["name"] for entry in
            self.run_cli("list", "--all-skills")["entries"]], ["configured"])
        config.write_text('[skills]\nroots = []\n')
        self.assertEqual(self.run_cli("paths")["roots"], [])
        self.assertEqual(self.run_cli("list", "--all-skills")["total"], 0)

    def test_paths_enumerate_linked_origins_and_retained_storage(self):
        root = self.dotdir / "skills"
        root.mkdir()
        outside = self.skill(self.root / "outside")
        (root / "linked").symlink_to(outside, target_is_directory=True)
        collection = self.root / "collection"
        self.skill(collection, "other")
        (root / "collection").symlink_to(collection, target_is_directory=True)
        receipt = self.run_cli("install", outside)
        paths = self.run_cli("paths")
        self.assertEqual(paths["managed"], [receipt])
        self.assertEqual(paths["excluded_retained_storage"], str(root / ".store"))
        linked = {item["canonical"] for item in paths["linked_origins"]}
        self.assertEqual(linked, {str(outside), str(collection)})
        self.assertTrue(all(item["present"] for item in paths["linked_origins"]))
        self.assertEqual(self.run_cli("list")["total"], 0)

    def test_private_dotdir_rejects_symlinked_parent(self):
        alias = self.root / "alias"
        alias.symlink_to(self.dotdir, target_is_directory=True)
        result = subprocess.run([BINARY, "skills", "--dotdir", str(alias), "paths", "--json"],
            env=self.environment, capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot inspect skills settings directory", result.stderr)
        self.assertEqual(self.run_cli("paths")["roots"][0]["path"],
            str(self.dotdir / "skills"))

    def test_relative_dotdir_reports_absolute_lookup_paths(self):
        result = subprocess.run([BINARY, "skills", "--dotdir", "state", "paths", "--json"],
            cwd=self.root, env=self.environment, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        paths = json.loads(result.stdout)
        self.assertEqual(paths["roots"][0]["path"], str(self.dotdir / "skills"))

    def test_name_ban_survives_absence_and_unban_does_not_approve(self):
        self.run_cli("ban", "sample")
        self.skill(self.dotdir / "skills")
        self.assertEqual(self.run_cli("list")["total"], 0)
        entry, = self.run_cli("list", "--all-skills")["entries"]
        self.assertEqual(entry["state"], "banned")
        self.run_cli("unban", entry["id"])
        entry, = self.run_cli("list", "--all-skills")["entries"]
        self.assertEqual(entry["state"], "pending")
        self.assertEqual(self.run_cli("list")["total"], 0)
        self.run_cli("ban", entry["id"])
        settings = json.loads((self.dotdir / "skills.json").read_text())
        self.assertIn("sample", settings["banned"])

    def test_review_covers_binary_resources_and_executable_mode(self):
        package = self.skill(self.dotdir / "skills")
        asset = package / "data.bin"
        asset.write_bytes(b"\x00\xfffirst")
        first = self.run_cli("review", "sample")
        self.assertTrue(first["adversarial"])
        self.assertEqual(self.run_cli("list")["total"], 0)
        resources = first["manifest"]["entries"]
        data, = [entry for entry in resources if entry["path"] == "data.bin"]
        self.assertEqual(data["size"], 7)
        asset.write_bytes(b"\x00\xffother")
        second = self.run_cli("review", "sample")
        self.assertNotEqual(first["digest"], second["digest"])
        snapshot = Path(first["root"]) / "data.bin"
        self.assertEqual(snapshot.read_bytes(), b"\x00\xfffirst")
        asset.chmod(0o700)
        third = self.run_cli("review", "sample")
        self.assertNotEqual(second["digest"], third["digest"])
        self.run_cli("approve", "sample", "--digest", third["digest"], ok=False)
        self.assertEqual(self.run_cli("list")["total"], 0)

    def test_policy_records_are_typed_and_snapshot_scoped(self):
        path = self.dotdir / "skills.json"
        for policy in ({"version": 99}, {"banned": [7]}, {"approvals": []},
                {"approvals": {"sample@origin": "approval"}},
                {"approvals": {"sample@origin": {"name": "sample",
                    "digest": "0" * 64, "root": str(self.root), "source": str(self.root)}}}):
            path.write_text(json.dumps(policy))
            path.chmod(0o600)
            self.run_cli("list", ok=False)

    def test_enable_and_cli_cannot_grant_approval(self):
        self.skill(self.dotdir / "skills")
        self.assertIn("approval", self.run_cli("enable", "sample", ok=False))
        self.run_cli("approve", "sample", ok=False)
        self.assertEqual(self.run_cli("list")["total"], 0)


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

    def execute(self, prompt=None, session=None, all_skills=False):
        arguments = [BINARY, "--dotdir", str(self.dotdir), "--no-listen", "--no-client"]
        if all_skills:
            arguments.append("--all-skills")
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

    def test_price_alongside_approved_skill_preserves_activation(self):
        provider = self.configured_provider()
        self.skill(self.dotdir / "skills", body="APPROVED-PRICE-GUIDANCE")
        self.operator_approve(self.run_cli("review", "sample"))
        requests = []
        def respond(handler, request, sequence):
            requests.append(request)
            provider.reply(handler, provider.response_body(sequence, "approved request done").encode(),
                close_header=True)
        provider.runtime_handler = respond
        result = self.execute("Use $sample to check $5k with $HOME.")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(requests), 1)
        self.assertIn("APPROVED-PRICE-GUIDANCE", json.dumps(requests[0]["input"]))

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
        self.assertEqual(len(requests), 0)
        session, = (self.dotdir / "sessions").iterdir()
        resumed = self.execute("Continue with $5k and $HOME.", session=session.name)
        self.assertEqual(resumed.returncode, 0, resumed.stderr)
        self.assertEqual(len(requests), 1)

    def test_pending_or_banned_dollar_words_do_not_admit_guidance(self):
        provider = self.configured_provider()
        self.skill(self.dotdir / "skills", "pending", description="HOSTILE-PENDING",
            body="HOSTILE-PENDING-BODY")
        self.skill(self.dotdir / "skills", "banned", body="HOSTILE-BANNED-BODY")
        self.run_cli("ban", "banned")
        requests = []
        def respond(handler, request, sequence):
            requests.append(request)
            provider.reply(handler, provider.response_body(sequence, "literal names done").encode(),
                close_header=True)
        provider.runtime_handler = respond
        result = self.execute("Discuss $pending and $banned at $5k.")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(requests), 1)
        self.assertNotIn("HOSTILE-", json.dumps(requests))

    def test_completion_filters_pending_and_banned_packages(self):
        from tmux_terminal import TmuxTerminal
        self.configured_provider()
        self.skill(self.dotdir / "skills", "approved-sample")
        self.skill(self.dotdir / "skills", "hostile-pending", description="HOSTILE-METADATA")
        self.skill(self.dotdir / "skills", "hostile-banned", description="BANNED-METADATA")
        self.operator_approve(self.run_cli("review", "approved-sample"))
        self.run_cli("ban", "hostile-banned")
        with TmuxTerminal(self.root / "completion-terminal", BINARY, self.workspace, self.dotdir,
                self.dotdir / "config.ini", 140, 42, args=("--no-listen", "--no-client",
                "--all-skills"), environment=self.environment) as terminal:
            terminal.wait("host-model")
            terminal.send_text("$approved-s")
            terminal.send_key("Tab")
            terminal.wait("$approved-sample")
            terminal.send_key("C-c")
            terminal.send_text("$hostile-")
            terminal.send_key("Tab")
            time.sleep(0.1)
            screen = terminal.capture()
            self.assertNotIn("hostile-pending", screen)
            self.assertNotIn("hostile-banned", screen)
            self.assertNotIn("HOSTILE-METADATA", screen)
            self.assertNotIn("BANNED-METADATA", screen)

    def test_model_switch_and_detached_workspace_preserve_curated_state(self):
        import sys
        from unittest.mock import patch
        from tmux_terminal import TmuxTerminal, event_list
        # This existing framing harness accepts its binary as a script argument.
        with patch.object(sys, "argv", ["skills.py"]):
            from test_session_view import View
        provider = self.configured_provider()
        self.skill(self.dotdir / "skills", body="DETACHED-EXACT-GUIDANCE")
        self.skill(self.dotdir / "skills", "pending", description="HOSTILE-PENDING")
        self.skill(self.dotdir / "skills", "banned", description="HOSTILE-BANNED")
        review = self.run_cli("review", "sample")
        self.operator_approve(review)
        self.run_cli("ban", "banned")
        requests = []
        def respond(handler, request, sequence):
            requests.append(request)
            if len(requests) == 1:
                body = provider.function_body(sequence, "activate", "skills",
                    {"action": "activate", "id": "sample"})
                provider.reply(handler, body.encode(), close_header=True)
            else:
                provider.reply(handler, b'{"error":{"message":"fixture stopped"}}',
                    status=503, close_header=True)
        provider.runtime_handler = respond
        with TmuxTerminal(self.root / "detach-terminal", BINARY, self.workspace, self.dotdir,
                self.dotdir / "config.ini", 140, 42, args=("--no-listen", "--no-client"),
                environment=self.environment) as terminal:
            terminal.wait("host-model")
            terminal.submit_wait("exercise approved sample", "fixture stopped")
            active = event_list(self.runtime_events(), "skills_updated")[-1]["data"]["active"]
            self.assertEqual(len(active), 1)
            self.assertEqual(active[0]["manifest_digest"], review["digest"])
            terminal.submit_wait("/session detach", "You can resume this session")
            directory = max((self.dotdir / "sessions").iterdir(),
                key=lambda path: path.stat().st_mtime_ns)
            view = View(directory)
            try:
                view.bind()
                if not view.states:
                    view.until("state")
                result = view.result(view.command("/model fixture/two-model/high:100000"))
                self.assertEqual(result["status"], "completed", result)
                state = view.states[-1]
                if state["model"] != "two-model":
                    state = view.until("state", lambda message:
                        message["state"]["model"] == "two-model")["state"]
                self.assertEqual(state["model"], "two-model")
                self.assertEqual(state["active_skills"], active)
                self.assertEqual([entry["name"] for entry in
                    state["skills_catalog"]["entries"]], ["sample"])
                self.assertNotIn("HOSTILE-", json.dumps(state))
                self.run_cli("ban", "sample")
                result = view.result(view.command("/skills refresh"))
                self.assertEqual(result["status"], "completed", result)
                state = view.states[-1]
                if state.get("active_skills"):
                    state = view.until("state", lambda message:
                        not message["state"].get("active_skills"))["state"]
                self.assertEqual(state["skills_catalog"]["entries"], [])
                self.assertNotIn("DETACHED-EXACT-GUIDANCE", json.dumps(state))
            finally:
                view.send(type="quit", generation=view.generation)
                self.assertEqual(view.until("exit")["status"], 0)
                view.peer.close()

    def test_queued_explicit_selection_checks_ban_at_admission(self):
        import threading
        from tmux_terminal import TmuxTerminal, event_list
        provider = self.configured_provider()
        self.skill(self.dotdir / "skills", body="QUEUED-REVOKED-BODY")
        self.operator_approve(self.run_cli("review", "sample"))
        started, release = threading.Event(), threading.Event()
        self.addCleanup(release.set)
        requests = []
        def respond(handler, request, sequence):
            requests.append(request)
            started.set()
            if not release.wait(10):
                raise AssertionError("queued fixture was not released")
            provider.reply(handler, provider.response_body(sequence, "held response done").encode(),
                close_header=True)
        provider.runtime_handler = respond
        with TmuxTerminal(self.root / "queue-terminal", BINARY, self.workspace, self.dotdir,
                self.dotdir / "config.ini", 140, 42, args=("--no-listen", "--no-client"),
                environment=self.environment) as terminal:
            terminal.wait("host-model")
            terminal.submit("hold first response")
            self.assertTrue(started.wait(5))
            terminal.send_text("/queue //skills use sample -- after this response")
            terminal.send_key("Enter")
            deadline = time.monotonic() + 5
            while not event_list(self.runtime_events(), "future_turn_queued"):
                self.assertLess(time.monotonic(), deadline, terminal.capture())
                time.sleep(0.025)
            self.run_cli("ban", "sample")
            release.set()
            terminal.wait("skill unavailable")
            self.assertEqual(len(requests), 1)
            pins = event_list(self.runtime_events(), "skills_updated")
            self.assertTrue(all(not item["data"]["active"] for item in pins))
            self.assertNotIn("QUEUED-REVOKED-BODY", json.dumps(requests))

    def test_approved_guidance_does_not_widen_readonly_tools(self):
        provider = self.configured_provider()
        self.skill(self.dotdir / "skills", body="READONLY-APPROVED-GUIDANCE",
            extra="allowed-tools: write_file exec_command\n")
        self.operator_approve(self.run_cli("review", "sample"))
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

    def test_dependencies_use_actual_mcp_state_without_granting_approval(self):
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
        self.operator_approve(self.run_cli("review", "sample"))
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

    def test_all_skills_never_reaches_model_or_grants_tool_approval(self):
        provider = self.configured_provider()
        self.skill(self.dotdir / "skills", description="HOSTILE-DESCRIPTION",
            body="HOSTILE-INSTRUCTIONS")
        requests = []
        def respond(handler, request, sequence):
            requests.append(request)
            if len(requests) == 1:
                body = provider.function_body(sequence, "list", "skills", {"action": "list"})
            elif len(requests) == 2:
                body = provider.function_body(sequence, "approve", "skills",
                    {"action": "approve", "id": "sample"})
            else:
                body = provider.response_body(sequence, "policy observed")
            provider.reply(handler, body.encode(), close_header=True)
        provider.runtime_handler = respond
        result = self.execute("inspect eligible skills", all_skills=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(requests), 3)
        for request in requests:
            self.assertNotIn("HOSTILE-DESCRIPTION", json.dumps(request))
            self.assertNotIn("HOSTILE-INSTRUCTIONS", json.dumps(request))
        policy = self.dotdir / "skills.json"
        self.assertFalse(policy.exists() and json.loads(policy.read_text()).get("approvals"))

    def test_operator_approval_digest_and_ban_revocation(self):
        provider = self.configured_provider()
        package = self.skill(self.dotdir / "skills", body="APPROVED-EXACT-BODY",
            extra="allowed-tools: exec_command made_up\n")
        (package / "reference.bin").write_bytes(b"\0binary")
        review = self.run_cli("review", "sample")
        self.operator_approve(review, digest="0" * 64, accepted=False)
        self.assertEqual(self.run_cli("list")["total"], 0)
        self.operator_approve(review)
        entry, = self.run_cli("list")["entries"]
        self.assertTrue(entry["approved"])
        self.assertEqual(entry["root"], review["root"])
        self.assertEqual(entry["path"], str(Path(review["root"]) / "SKILL.md"))
        self.assertEqual(entry["source_root"], str(package))
        requests = []
        def respond(handler, request, sequence):
            requests.append(request)
            if len(requests) == 1:
                body = provider.function_body(sequence, "activate", "skills",
                    {"action": "activate", "id": "sample"})
            elif len(requests) == 2:
                self.run_cli("ban", "sample")
                body = provider.function_body(sequence, "check", "get_cwd", {})
            else:
                body = provider.response_body(sequence, "revocation observed")
            provider.reply(handler, body.encode(), close_header=True)
        provider.runtime_handler = respond
        result = self.execute("exercise approved skill")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertGreaterEqual(len(requests), 3)
        self.assertIn(entry["path"], json.dumps(requests[0]["input"]))
        self.assertIn("APPROVED-EXACT-BODY", json.dumps(requests[1]["input"]))
        self.assertNotEqual(requests[0]["prompt_cache_key"], requests[1]["prompt_cache_key"])
        self.assertNotEqual(requests[1]["prompt_cache_key"], requests[2]["prompt_cache_key"])
        outputs = [item["output"] for item in requests[1]["input"]
            if item.get("type") == "function_call_output"]
        receipt = json.loads(outputs[-1])
        hints = {item["name"]: item for item in receipt["allowed_tools"]}
        self.assertEqual(hints["exec_command"]["state"], "available")
        self.assertEqual(hints["made_up"]["state"], "unavailable")
        self.assertFalse(hints["exec_command"]["grants_permission"])
        self.assertNotIn("APPROVED-EXACT-BODY", json.dumps(requests[2]["input"]))
        self.assertEqual(self.run_cli("list")["total"], 0)
        self.run_cli("unban", "sample")
        self.assertEqual(self.run_cli("list")["total"], 0)
        self.operator_approve(review, accepted=False)
        self.operator_approve(self.run_cli("review", "sample"))
        self.assertEqual(self.run_cli("list")["total"], 1)
        (package / "reference.bin").write_bytes(b"changed")
        self.assertEqual(self.run_cli("list")["total"], 0)
        entry, = self.run_cli("list", "--all-skills")["entries"]
        self.assertEqual(entry["state"], "changed")

    def test_compacted_guidance_is_discarded_after_ban(self):
        from tmux_terminal import TmuxTerminal, is_summary_request, event_list
        provider = self.configured_provider()
        config = self.dotdir / "config.ini"
        config.write_text(config.read_text().replace("exact_token_count=false",
            "exact_token_count=false\nnative_compaction=false"))
        self.skill(self.dotdir / "skills", body="COMPACTABLE-SKILL-BODY")
        self.operator_approve(self.run_cli("review", "sample"))
        requests, summaries = [], []
        def respond(handler, request, sequence):
            if is_summary_request(request):
                summaries.append(request)
                body = provider.response_body(sequence, "COMPACTED-REVOKED-GUIDANCE")
            else:
                requests.append(request)
                if len(requests) == 1:
                    body = provider.function_body(sequence, "activate", "skills",
                        {"action": "activate", "id": "sample"})
                else:
                    body = provider.response_body(sequence, f"response-{len(requests)}")
            provider.reply(handler, body.encode(), close_header=True)
        provider.runtime_handler = respond
        with TmuxTerminal(self.root / "compact-terminal", BINARY, self.workspace, self.dotdir,
                config, 140, 42, args=("--no-listen", "--no-client"),
                environment=self.environment) as terminal:
            terminal.wait("host-model")
            terminal.submit_wait("use approved sample", "response-2")
            terminal.submit_wait("/compact", "Compacted")
            self.assertEqual(len(summaries), 1)
            self.assertIn("COMPACTABLE-SKILL-BODY", json.dumps(summaries))
            self.run_cli("ban", "sample")
            terminal.submit_wait("continue after revocation", "response-3")
            projected = json.dumps(requests[-1]["input"])
            self.assertNotIn("COMPACTABLE-SKILL-BODY", projected)
            self.assertNotIn("COMPACTED-REVOKED-GUIDANCE", projected)
            events = self.runtime_events()
            self.assertTrue(event_list(events, "compaction_completed"))

    def test_read_only_turn_preserves_existing_bindings(self):
        from tmux_terminal import TmuxTerminal, event_list
        provider = self.configured_provider()
        self.skill(self.dotdir / "skills", body="ONGOING-SKILL-BODY")
        self.operator_approve(self.run_cli("review", "sample"))
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

    def test_goal_bindings_survive_readonly_and_revoke_before_resume(self):
        from tmux_terminal import TmuxTerminal, event_list
        provider = self.configured_provider()
        self.skill(self.dotdir / "skills", body="GOAL-SKILL-GUIDANCE")
        self.operator_approve(self.run_cli("review", "sample"))
        requests = []
        def respond(handler, request, sequence):
            requests.append(request)
            number = len(requests)
            if number == 1:
                body = provider.function_body(sequence, "activate", "skills",
                    {"action": "activate", "id": "sample"})
            elif number == 2:
                body = provider.function_body(sequence, "block", "update_goal",
                    {"action": "block", "text": "fixture awaiting operator", "wait_for": "operator"})
            elif number == 5:
                body = provider.function_body(sequence, "complete", "update_goal",
                    {"action": "complete"})
            else:
                body = provider.response_body(sequence, f"goal-response-{number}")
            provider.reply(handler, body.encode(), close_header=True)
        provider.runtime_handler = respond
        with TmuxTerminal(self.root / "goal-terminal", BINARY, self.workspace, self.dotdir,
                self.dotdir / "config.ini", 140, 42, args=("--no-listen", "--no-client"),
                environment=self.environment) as terminal:
            terminal.wait("host-model")
            terminal.submit_wait("/goal exercise approved sample until finished", "goal-response-3")
            active = event_list(self.runtime_events(), "skills_updated")[-1]["data"]["active"]
            self.assertEqual(len(active), 1)
            self.assertTrue(active[0]["goal_id"])
            terminal.submit_wait("/ro inspect without replacing the goal", "goal-response-4")
            self.assertEqual(event_list(self.runtime_events(), "skills_updated")[-1]["data"]["active"],
                active)
            self.assertIn("GOAL-SKILL-GUIDANCE", json.dumps(requests[-1]["input"]))
            self.run_cli("ban", "sample")
            terminal.submit_wait("/goal resume", "goal-response-6")
            for request in requests[4:]:
                self.assertNotIn("GOAL-SKILL-GUIDANCE", json.dumps(request["input"]))
            self.assertTrue(event_list(self.runtime_events(), "goal_completed"))

    def test_resume_rechecks_bans_policy_and_snapshot_resources(self):
        provider = self.configured_provider()
        package = self.skill(self.dotdir / "skills", body="REVOKABLE-GUIDANCE")
        (package / "asset.bin").write_bytes(b"original")
        for revocation in ("ban", "approval", "resource"):
            with self.subTest(revocation=revocation):
                self.run_cli("unban", "sample")
                review = self.run_cli("review", "sample")
                self.operator_approve(review)
                requests = []
                def fail_after_activation(handler, request, sequence):
                    requests.append(request)
                    if len(requests) == 1:
                        body = provider.function_body(sequence, "activate", "skills",
                            {"action": "activate", "id": "sample"})
                        provider.reply(handler, body.encode(), close_header=True)
                    else:
                        provider.reply(handler, b'{"error":{"message":"fixture stopped"}}',
                            status=503, close_header=True)
                provider.runtime_handler = fail_after_activation
                failed = self.execute("exercise revocable skill")
                self.assertNotEqual(failed.returncode, 0)
                self.assertIn("REVOKABLE-GUIDANCE", json.dumps(requests[1]["input"]))
                session = max((self.dotdir / "sessions").iterdir(),
                    key=lambda path: path.stat().st_mtime_ns)
                if revocation == "ban":
                    self.run_cli("ban", "sample")
                elif revocation == "approval":
                    path = self.dotdir / "skills.json"
                    policy = json.loads(path.read_text())
                    policy["approvals"].clear()
                    path.write_text(json.dumps(policy))
                else:
                    (Path(review["root"]) / "asset.bin").write_bytes(b"tampered")
                requests.clear()
                def resumed(handler, request, sequence):
                    requests.append(request)
                    provider.reply(handler, provider.response_body(sequence, "resumed").encode(),
                        close_header=True)
                provider.runtime_handler = resumed
                result = self.execute("/retry", session=session.name)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(requests)
                self.assertNotIn("REVOKABLE-GUIDANCE", json.dumps(requests[0]["input"]))

    def test_resume_pins_exact_instructions_after_source_deletion(self):
        provider = self.configured_provider()
        package = self.skill(self.dotdir / "skills", body="EXACT-PIN $ARGUMENTS")
        self.operator_approve(self.run_cli("review", "sample"))
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

    def test_explicit_only_policy_and_literal_prompt(self):
        provider = self.configured_provider()
        package = self.skill(self.dotdir / "skills", body="EXPLICIT $ARGUMENTS")
        (package / "agents").mkdir()
        (package / "agents/openai.yaml").write_text(
            "policy:\n  allow_implicit_invocation: false\n")
        self.operator_approve(self.run_cli("review", "sample"))
        requests = []
        def respond(handler, request, sequence):
            requests.append(request)
            if len(requests) == 1:
                body = provider.function_body(sequence, "forbidden", "skills",
                    {"action": "activate", "id": "sample"})
            else:
                body = provider.response_body(sequence, "refusal observed")
            provider.reply(handler, body.encode(), close_header=True)
        provider.runtime_handler = respond
        result = self.execute("choose relevant skills")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("requires explicit", json.dumps(requests[1]["input"]))
        self.assertNotIn("Active skill snapshot:", json.dumps(requests[1]["input"]))
        requests.clear()
        provider.runtime_handler = lambda handler, request, sequence: (
            requests.append(request), provider.reply(handler,
                provider.response_body(sequence, "explicit task complete").encode(), close_header=True))
        result = self.execute("/skills use sample -- literal $0 $(touch never)")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("EXPLICIT literal $0 $(touch never)", json.dumps(requests[0]["input"]))
        self.assertFalse((self.workspace / "never").exists())

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
        self.operator_approve(self.run_cli("review", "sample"))
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
        # A stale native skill result cannot restore guidance through history.
        self.assertNotIn("PINNED-SKILL-BODY", json.dumps(requests[0]["input"]))


if __name__ == "__main__":
    unittest.main()
