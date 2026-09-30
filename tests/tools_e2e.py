#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""End-to-end tests for native exploration and modification tools.

Runs the real product binary headlessly against the shared fixture provider and
asserts durable journal events plus real filesystem effects. Deterministic: no
terminal, no timing races.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import tmux_terminal as harness  # noqa: E402

SECRET = "irc-ui-secret"
DENY_WRITE = (
    "[rule deny-write]\n"
    'match = {"/tool":"^(write_file|edit_file)$"}\n'
    "action = deny\n"
    'message = "File modification is disabled here."\n'
)


def run_case(binary, provider, root, name, prompt, respond, rules="", read_only=False,
             prepare=None, agent_settings="", read_agents=False):
    case = root / name
    case.mkdir(parents=True)
    if prepare is not None:
        prepare(case)
    config = case / "config.ini"
    harness.write_irc_config(config, provider.port, "host-model")
    if read_agents:
        config.write_text(config.read_text().replace("read_agents_md = false",
                                                   "read_agents_md = true"))
    if agent_settings:
        config.write_text(config.read_text(encoding="utf-8").replace(
            "[agent]\n", "[agent]\n" + agent_settings, 1), encoding="utf-8")
    if rules:
        with config.open("a", encoding="utf-8") as out:
            out.write(rules)
    state = case / "state"
    provider.runtime_handler = respond
    try:
        result = subprocess.run(
            [str(binary), "--config", str(config), "--dotdir", str(state),
             "-e", "--", ("/ro " if read_only else "") + prompt],
            cwd=case, env={**os.environ, "HOME": str(case), "SNAJPAGENT_IRC_UI_KEY": SECRET},
            capture_output=True, text=True, timeout=60)
    finally:
        provider.runtime_handler = None
    try:
        _, events = harness.read_events(state)
    except AssertionError as error:
        raise AssertionError((str(error), result.returncode, result.stdout, result.stderr)) from error
    return case, result, events


def responder(provider, calls, final, inspect=None):
    def respond(handler, request, sequence):
        outs = [i for i in request["input"] if i.get("type") == "function_call_output"]
        if inspect is not None:
            inspect(outs, request)
        if len(outs) < len(calls):
            name, arguments = calls[len(outs)]
            provider.reply(handler, provider.function_body(
                sequence, f"tool-call-{len(outs)}", name, arguments).encode())
            return
        provider.reply(handler, provider.response_body(sequence, final).encode())
    return respond


def finished(events):
    return harness.event_list(events, "tool_finished")


def case_exploration(binary, provider, root):
    case, result, events = run_case(
        binary, provider, root, "exploration", "inspect the workspace",
        responder(provider, [
            ("list_files", {"path": "."}),
            ("read_file", {"path": "probe.txt"}),
            ("grep", {"path": ".", "pattern": "hello"}),
        ], "exploration done"),
        prepare=lambda case: (case / "probe.txt").write_text("hello world\n"))
    assert result.returncode == 0, (result.stdout, result.stderr)
    entries = finished(events)
    assert len(entries) == 3, entries
    results = [entry["data"]["result"] for entry in entries]
    assert all(item["status"] == "succeeded" for item in results), results
    assert "hello world" in results[1]["model_text"], results[1]
    assert "probe.txt" in results[0]["model_text"], results[0]
    print("tools e2e exploration: ok", flush=True)


def case_write_and_edit(binary, provider, root):
    case, result, events = run_case(
        binary, provider, root, "write-edit", "create then edit a file",
        responder(provider, [
            ("write_file", {"path": "created.txt", "content": "hello\n"}),
            ("edit_file", {"path": "created.txt", "old": "hello", "new": "bye"}),
        ], "write and edit done"))
    assert result.returncode == 0, (result.stdout, result.stderr)
    entries = finished(events)
    assert len(entries) == 2, entries
    results = [entry["data"]["result"] for entry in entries]
    assert all(item["status"] == "succeeded" for item in results), results
    assert (case / "created.txt").read_text() == "bye\n"
    print("tools e2e write/edit: ok", flush=True)


def case_explicit_file_paths(binary, provider, root):
    outside = root / "outside-working-directory"
    outside.mkdir()
    outside_file = outside / "outside.txt"
    patch_file = outside / "patched.txt"
    case_path = root / "explicit-file-paths"
    duplicate = case_path / "duplicate.txt"
    calls = [
        ("get_cwd", {}),
        ("write_file", {"path": "./own.txt", "content": "inside\n"}),
        ("write_file", {"path": str(outside_file), "content": "outside\n"}),
        ("read_file", {"path": str(outside_file)}),
        ("edit_file", {"path": str(outside_file), "old": "outside", "new": "changed"}),
        ("apply_patch", {"patch": "*** Begin Patch\n*** Add File: " + str(patch_file) +
                         "\n+patched\n*** End Patch\n", "workdir": "./subdir"}),
        ("exec_command", {"command": "pwd", "workdir": "./subdir"}),
        ("cd", {"path": "./subdir"}),
        ("get_cwd", {}),
        ("exec_command", {"command": "pwd"}),
        ("apply_patch", {"patch": "*** Begin Patch\n*** Add File: ./duplicate.txt\n+first\n"
                         "*** Add File: " + str(duplicate) + "\n+second\n*** End Patch\n",
                         "workdir": str(case_path)}),
    ]
    case, result, events = run_case(binary, provider, root, "explicit-file-paths",
                                     "use explicit local file paths",
                                     responder(provider, calls, "file paths done"),
                                     prepare=lambda case: (case / "subdir").mkdir())
    assert result.returncode == 0, (result.stdout, result.stderr)
    entries = finished(events)
    assert len(entries) == len(calls), entries
    results = [entry["data"]["result"] for entry in entries]
    assert all(item["status"] == "succeeded" for item in results[:-1]), results
    assert results[-1]["status"] == "patch_rejected" and "duplicate" in results[-1]["model_text"]
    assert str(case_path) in results[0]["model_text"]
    assert (case / "own.txt").read_text() == "inside\n"
    assert outside_file.read_text() == "changed\n"
    assert patch_file.read_text() == "patched\n"
    assert str(case / "subdir") in results[6]["model_text"]
    assert results[8]["model_text"] == str(case / "subdir")
    assert str(case / "subdir") in results[9]["model_text"]
    assert not duplicate.exists()
    assert any(entry["type"] == "cwd_changed" for entry in events)
    print("tools e2e explicit paths + cwd: ok", flush=True)


def case_home_default(binary, provider, root):
    home = root / "home"
    (home / "home-marker.txt").write_text("at home\n")
    case = root / "home-default"
    case.mkdir()
    config = case / "config.ini"
    harness.write_irc_config(config, provider.port, "host-model")
    state = case / "state"
    provider.runtime_handler = responder(provider, [
        ("get_cwd", {}),
        ("read_file", {"path": "./home-marker.txt"}),
        ("exec_command", {"command": "pwd"}),
    ], "home default done")
    try:
        run = subprocess.run([str(binary), "--config", str(config), "--dotdir", str(state),
                              "-e", "--", "verify default cwd"], cwd=case,
                             env={**os.environ, "SNAJPAGENT_IRC_UI_KEY": SECRET},
                             capture_output=True, text=True, timeout=60)
    finally:
        provider.runtime_handler = None
    assert run.returncode == 0, (run.stdout, run.stderr)
    _, events = harness.read_events(state)
    results = [entry["data"]["result"] for entry in finished(events)]
    assert len(results) == 3, results
    assert all(item["status"] == "succeeded" for item in results), results
    assert results[0]["model_text"] == str(home), results[0]
    assert "at home" in results[1]["model_text"], results[1]
    assert str(home) in results[2]["model_text"], results[2]
    print("tools e2e home default: ok", flush=True)


def case_model_switch_disabled(binary, provider, root):
    for name, setting in (("default", ""), ("explicit", "allow_model_change=false\n")):
        requests = []
        case, run, events = run_case(
            binary, provider, root, f"model-switch-disabled-{name}", "keep this model",
            responder(provider, [
                ("select_model", {"selector": "cache"}),
                ("select_model", {"selector": "standard-model/ultra"}),
                ("get_cwd", {}),
            ], "model selection unchanged", inspect=lambda outs, request: requests.append(request)),
            agent_settings=setting)
        assert run.returncode == 0, (run.stdout, run.stderr)
        assert requests and all("select_model" not in {
            tool.get("name") for tool in request["tools"]} for request in requests), \
            "disabled select_model was advertised"
        results = [entry["data"]["result"] for entry in finished(events)]
        assert [item["status"] for item in results] == ["failed", "failed", "succeeded"], results
        assert all("allow_model_change" in item["model_text"] for item in results[:2]), results
        assert all(request["model"] == "host-model" for request in requests)
        assert not any(event["type"] in ("turn_model_changed", "model_selection_changed")
                       for event in events)
        assert not (case / "state" / "models.json").exists()
    print("tools e2e model change default/explicit off: ok", flush=True)


def case_model_switch(binary, provider, root):
    models = []
    cache_path = root / "model-switch" / "state" / "models.json"
    cache_snapshot = []

    def observed(outs, request):
        assert "select_model" in {tool.get("name") for tool in request["tools"]}
        models.append(request["model"])
        if len(outs) == 2:
            cache_snapshot.append((cache_path.read_bytes(), cache_path.stat().st_ino))
        if len(outs) == 4:
            with provider.lock:
                provider.catalog_failure = "/v1/models"
        if len(outs) == 5:
            assert (cache_path.read_bytes(), cache_path.stat().st_ino) == cache_snapshot[0]
            with provider.lock:
                provider.catalog_failure = None

    previous_agents = provider.AGENTS
    provider.AGENTS = {**previous_agents, "standard-model": "standardbot"}
    try:
        case, run, events = run_case(binary, provider, root, "model-switch",
            "switch the model and retain results", responder(provider, [
                ("select_model", {"selector": "uncached-model"}),
                ("select_model", {"selector": "cache"}),
                ("select_model", {"selector": "ghost-model/medium"}),
                ("select_model", {"selector": "standard-model/ultra"}),
                ("select_model", {"selector": "cache"}),
                ("select_model", {"selector": "#1"}),
                ("get_cwd", {}),
            ], "model switch complete", inspect=observed),
            agent_settings="allow_model_change=true\n")
    finally:
        provider.AGENTS = previous_agents
    assert run.returncode == 0, (run.stdout, run.stderr)
    assert models[:6] == ["host-model"] * 6, models
    assert models[-2:] == ["standard-model"] * 2, models
    assert sum(event["type"] == "turn_model_changed" for event in events) == 1
    assert sum(event["type"] == "model_selection_changed" for event in events) == 1
    results = [entry["data"]["result"] for entry in finished(events)]
    assert [item["status"] for item in results] == [
        "failed", "succeeded", "failed", "failed", "failed", "succeeded", "succeeded"], results
    assert "cache" in results[0]["model_text"]
    assert "standard-model" in results[1]["model_text"]
    assert "cached" in results[2]["model_text"]
    assert "effort" in results[3]["model_text"] or "cached" in results[3]["model_text"]
    assert "catalog rejected" in results[4]["model_text"]
    assert str(case) in results[6]["model_text"]
    assert cache_path.is_file() and len(cache_snapshot) == 1
    print("tools e2e current-cache selection and refresh: ok", flush=True)


def case_read_only_refuses_writes(binary, provider, root):
    seen = []
    case, result, events = run_case(
        binary, provider, root, "read-only", "inspect only",
        responder(provider, [
            ("read_file", {"path": "probe.txt"}),
            ("write_file", {"path": "nope.txt", "content": "x"}),
            ("exec_command", {"command": "printf x >> marker"}),
        ], "read-only done", inspect=lambda outs, request: seen.append(request)),
        read_only=True, prepare=lambda case: (case / "probe.txt").write_text("hello world\n"))
    assert result.returncode == 0, (result.stdout, result.stderr)
    entries = finished(events)
    assert len(entries) == 3, entries
    results = [entry["data"]["result"] for entry in entries]
    assert results[0]["status"] == "succeeded", results[0]
    for item in results[1:]:
        assert item["status"] != "succeeded", item
        assert "read-only" in item["model_text"], item
    assert not (case / "nope.txt").exists()
    assert not (case / "marker").exists()
    print("tools e2e read-only-refusal: ok", flush=True)


def case_rule_denies_write(binary, provider, root):
    case, result, events = run_case(
        binary, provider, root, "rule-deny-write", "attempt a write",
        responder(provider, [
            ("write_file", {"path": "denied.txt", "content": "x"}),
        ], "rule denial done"), rules=DENY_WRITE)
    assert result.returncode == 0, (result.stdout, result.stderr)
    entries = finished(events)
    assert len(entries) == 1, entries
    item = entries[0]["data"]["result"]
    assert item["status"] == "not_run" and item["reason"] == "rule_rejected", item
    assert "File modification is disabled here." in item["model_text"], item
    assert not (case / "denied.txt").exists()
    print("tools e2e rule-denies-write: ok", flush=True)


def case_wide_call_batch(binary, provider, root):
    calls = [("read_file", {"path": "probe.txt"}) for _ in range(120)]
    seen = []

    def respond(handler, request, sequence):
        outs = [i for i in request["input"] if i.get("type") == "function_call_output"]
        if not outs:
            provider.reply(handler, provider.functions_body(
                sequence, [(f"wide-{i}", name, arguments)
                           for i, (name, arguments) in enumerate(calls)]).encode())
            return
        seen.append(len(outs))
        provider.reply(handler, provider.response_body(sequence, "wide batch done").encode())

    case, result, events = run_case(
        binary, provider, root, "wide-batch", "read the probe many times", respond,
        prepare=lambda case: (case / "probe.txt").write_text("hello world\n"))
    assert result.returncode == 0, (result.stdout, result.stderr)
    entries = finished(events)
    assert len(entries) == len(calls), len(entries)
    results = [entry["data"]["result"] for entry in entries]
    assert all(item["status"] == "succeeded" for item in results), results
    assert seen == [len(calls)], seen
    print("tools e2e wide-call-batch: ok", flush=True)


def case_cd_call_batch(binary, provider, root):
    calls = [
        ("cd", {"path": "./subdir"}),
        ("get_cwd", {}),
        ("write_file", {"path": "./created.txt", "content": "in subdir"}),
        ("exec_command", {"command": "pwd"}),
        ("cd", {"path": "./missing"}),
        ("read_file", {"path": "./created.txt"}),
        ("cd", {"path": ".."}),
        ("get_cwd", {}),
    ]

    def respond(handler, request, sequence):
        outs = [i for i in request["input"] if i.get("type") == "function_call_output"]
        if not outs:
            provider.reply(handler, provider.functions_body(
                sequence, [(f"cd-batch-{i}", name, arguments)
                           for i, (name, arguments) in enumerate(calls)]).encode())
            return
        assert len(outs) == len(calls), outs
        provider.reply(handler, provider.response_body(sequence, "cd batch done").encode())

    case, result, events = run_case(
        binary, provider, root, "cd-batch", "change directory within one batch", respond,
        prepare=lambda case: (case / "subdir").mkdir())
    assert result.returncode == 0, (result.stdout, result.stderr)
    starts = harness.event_list(events, "tool_started")
    outcomes = {entry["data"]["call_id"]: entry["data"]["result"] for entry in finished(events)}
    results = [outcomes[entry["data"]["call_id"]] for entry in starts]
    assert len(results) == len(calls), results
    assert [item["status"] for item in results] == [
        "succeeded", "succeeded", "succeeded", "succeeded",
        "failed", "succeeded", "succeeded", "succeeded"], results
    assert results[1]["model_text"] == str(case / "subdir"), results[1]
    assert str(case / "subdir") in results[3]["model_text"], results[3]
    assert "in subdir" in results[5]["model_text"], results[5]
    assert results[7]["model_text"] == str(case), results[7]
    assert (case / "subdir" / "created.txt").read_text() == "in subdir"
    assert not (case / "created.txt").exists()
    for index, entry in enumerate(starts):
        name, arguments = calls[index]
        action = dict(arguments=arguments, name=name, resolved_workdir=str(case))
        digest = hashlib.sha256(json.dumps(
            action, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
        assert entry["data"]["action_sha256"] == digest, entry
        expected = case if index in (0, 7) else case / "subdir"
        assert entry["data"]["resolved_workdir"] == str(expected), entry

    # Resume must accept the complete journal and retain the tools' outcomes.
    provider.runtime_handler = respond
    try:
        resumed = subprocess.run(
            [str(binary), "--config", str(case / "config.ini"),
             "--dotdir", str(case / "state"), "--resume", "--last", "-e", "--", "verify"],
            cwd=case, env={**os.environ, "HOME": str(case), "SNAJPAGENT_IRC_UI_KEY": SECRET},
            capture_output=True, text=True, timeout=60)
    finally:
        provider.runtime_handler = None
    assert resumed.returncode == 0, (resumed.stdout, resumed.stderr)
    _, replay = harness.read_events(case / "state")
    assert len(finished(replay)) == len(calls), replay
    print("tools e2e cd-call-batch: ok", flush=True)


def case_home_instruction_symlink(binary, provider, root):
    def prepare(case):
        (case / "guidance.md").write_text("Use the linked home instructions.\n")
        (case / "AGENTS.md").symlink_to("guidance.md")

    case, result, events = run_case(
        binary, provider, root, "home-instruction-symlink", "read the home instructions",
        responder(provider, [("read_file", {"path": "./guidance.md"})], "instructions read"),
        prepare=prepare, read_agents=True)
    assert result.returncode == 0, (result.stdout, result.stderr)
    turn = harness.event_list(events, "turn_started")[0]
    assert str(case / "guidance.md") in turn["data"]["instructions"], turn
    output = finished(events)[0]["data"]["result"]
    assert output["status"] == "succeeded", output
    assert "linked home instructions" in output["model_text"], output
    print("tools e2e home-instruction-symlink: ok", flush=True)


def case_incomplete_stream_recovery(binary, provider, root):
    requests = []

    def respond(handler, request, sequence):
        requests.append(request)
        count = len(requests)
        arguments = {"command": "printf once >> completed-command", "workdir": None,
                     "stdin": None, "pty": False, "timeout_ms": 10000,
                     "yield_ms": 1000, "max_output_tokens": 1000}
        if count == 1:
            body = provider.function_body(sequence, "completed-before-drop",
                                          "exec_command", arguments)
        elif count == 2:
            # A complete local proposal without response.completed cannot execute.
            partial = provider.response_body(sequence, "partial provider text")
            partial = partial.split("event: response.completed", 1)[0]
            arguments["command"] = "printf forbidden > unfinished-command"
            proposal = provider.function_body(sequence, "unfinished-at-drop",
                                               "exec_command", arguments)
            proposal = proposal.split("event: response.output_item.added", 1)[1]
            proposal = "event: response.output_item.added" + proposal
            # Keep distinct item slots and identities within this response.
            proposal = proposal.replace('"output_index":0', '"output_index":1')
            body = partial + proposal.split("event: response.completed", 1)[0]
        else:
            assert count == 3, count
            inputs = json.dumps(request["input"])
            assert "partial provider text" in inputs, inputs
            assert "completed-before-drop" in inputs, inputs
            assert "unfinished-at-drop" not in inputs, inputs
            body = provider.response_body(sequence, "stream recovered")
        provider.reply(handler, body.encode(), close_header=True)

    case, result, events = run_case(binary, provider, root, "incomplete-stream",
                                   "continue through a provider disconnect", respond)
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert "stream recovered" in result.stdout, result
    assert (case / "completed-command").read_text() == "once"
    assert not (case / "unfinished-command").exists()
    failed, = harness.event_list(events, "response_failed")
    assert failed["data"]["message"] == "provider stream ended before response.completed", failed
    assert len(harness.event_list(events, "turn_recovery")) == 1
    assert len(harness.event_list(events, "turn_completed")) == 1
    assert "recovering automatically" in result.stderr, result.stderr
    print("tools e2e incomplete stream recovery without duplicate effects: ok", flush=True)


CASES = (
    case_home_default,
    case_model_switch_disabled,
    case_model_switch,
    case_exploration,
    case_write_and_edit,
    case_explicit_file_paths,
    case_read_only_refuses_writes,
    case_rule_denies_write,
    case_wide_call_batch,
    case_cd_call_batch,
    case_home_instruction_symlink,
    case_incomplete_stream_recovery,
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    args = parser.parse_args()
    root = Path(tempfile.mkdtemp(prefix="snajpagent-tools-e2e-"))
    os.environ["HOME"] = str(root / "home")
    (root / "home").mkdir(mode=0o700)
    provider = harness.FakeResponses()
    binary = Path(args.binary).resolve()
    try:
        assert SECRET
        for case in CASES:
            case(binary, provider, root)
        assert not provider.failure, provider.failure
        print("tools_e2e: ok", flush=True)
    finally:
        provider.close()
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
