#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""End-to-end tests for native exploration and modification tools.

Runs the real product binary headlessly against the shared fixture provider and
asserts durable journal events plus real filesystem effects. Deterministic: no
terminal, no timing races.
"""
import argparse
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
             prepare=None):
    case = root / name
    case.mkdir(parents=True)
    if prepare is not None:
        prepare(case)
    config = case / "config.ini"
    harness.write_irc_config(config, provider.port, "host-model")
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
    _, events = harness.read_events(state)
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


def case_model_switch(binary, provider, root):
    models = []

    def observed(outs, request):
        del outs
        models.append(request["model"])

    case, run, events = run_case(binary, provider, root, "model-switch",
        "switch the model and retain results", responder(provider, [
            ("select_model", {"selector": "one-model"}),
            ("get_cwd", {}),
        ], "model switch complete", inspect=observed))
    assert run.returncode == 0, (run.stdout, run.stderr)
    assert models[:3] == ["host-model", "one-model", "one-model"], models
    assert sum(event["type"] == "turn_model_changed" for event in events) == 1
    assert sum(event["type"] == "model_selection_changed" for event in events) == 1
    results = [entry["data"]["result"] for entry in finished(events)]
    assert len(results) == 2 and all(item["status"] == "succeeded" for item in results), results
    assert str(case) in results[1]["model_text"]
    print("tools e2e same-turn model switch: ok", flush=True)


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


CASES = (
    case_home_default,
    case_model_switch,
    case_exploration,
    case_write_and_edit,
    case_explicit_file_paths,
    case_read_only_refuses_writes,
    case_rule_denies_write,
    case_wide_call_batch,
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
