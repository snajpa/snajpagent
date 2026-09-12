#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""End-to-end tests for model tool-call rules.

Runs the real product binary headlessly (-e) against the shared fixture HTTP
provider with [rule NAME] configuration, and asserts durable journal events,
real filesystem effects and resume behavior. Deterministic: no terminal, no
timing races, no tmux.
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

DENY_RULES = (
    "[rule deny-exec]\n"
    "chain = out\n"
    'match = {"/kind":"^tool_call$","/tool":"^exec_command$"}\n'
    "action = reject\n"
    'text = "Blocked %{/tool}: %{/value/command} (100%%)"\n'
)
ARG_RULES = (
    "[rule deny-rm]\n"
    "chain = out\n"
    'match = {"/tool":"^exec_command$","/value/command":"^rm "}\n'
    "action = reject\n"
    'text = "rm is disabled here."\n'
)
ALLOWLIST_RULES = (
    "[rule allow-exec]\n"
    "chain = out\n"
    'match = {"/tool":"^exec_command$"}\n'
    "action = accept\n"
    "[rule deny-everything-else]\n"
    "chain = out\n"
    "action = reject\n"
    'text = "Only the audited command tool is permitted."\n'
)
PASS_THEN_DENY_RULES = (
    "[rule observe]\n"
    "chain = out\n"
    'match = {"/kind":"^tool_call$"}\n'
    "action = pass\n"
    "[rule deny-exec]\n"
    "chain = out\n"
    'match = {"/tool":"^exec_command$"}\n'
    "action = reject\n"
    'text = "pass does not exempt a call."\n'
)
JUMP_RULES = (
    "[rule enter-policy]\n"
    "chain = out\n"
    'match = {"/kind":"^tool_call$"}\n'
    "action = jump\n"
    "target = host-policy\n"
    "[rule no-shell]\n"
    "chain = host-policy\n"
    'match = {"/tool":"^exec_command$"}\n'
    "action = reject\n"
    'text = "Reusable policy denied %{/tool}."\n'
)
LOG_RULES = (
    "[rule audit]\n"
    "chain = out\n"
    'match = {"/kind":"^tool_call$"}\n'
    "action = pass\n"
    'log = "tool=%{/tool} args=%{/text}"\n'
)
STICKY_RULES = (
    "[rule deny-exec]\n"
    "chain = out\n"
    'match = {"/tool":"^exec_command$"}\n'
    "action = reject\n"
    'text = "denied %{/tool}"\n'
    "[rule audit]\n"
    "chain = out\n"
    'match = {"/kind":"^tool_call$"}\n'
    "action = pass\n"
    'log = "audit %{/tool}"\n'
)
THRESHOLD_RULES = (
    "[rule big-budget]\n"
    "chain = out\n"
    'at_least = {"/value/max_output_tokens":1000000}\n'
    "action = reject\n"
    'text = "Requested output budget exceeds policy."\n'
)
RETURN_RULES = (
    "[rule stop-exec]\n"
    "chain = out\n"
    'match = {"/tool":"^exec_command$"}\n'
    "action = return\n"
    "[rule unreachable-deny]\n"
    "chain = out\n"
    "action = reject\n"
    'text = "unreachable for exec_command"\n'
)


class RulesCase:
    def __init__(self, binary, provider, root, name, rules):
        self.binary, self.provider = binary, provider
        self.case = root / name
        self.case.mkdir(parents=True)
        self.config = self.case / "config.ini"
        harness.write_irc_config(self.config, provider.port, "host-model")
        if rules:
            with self.config.open("a", encoding="utf-8") as out:
                out.write(rules)
        self.state = self.case / "state"
        self.respond = None
        self.events = []
        self.result = None

    def run(self, args):
        self.provider.runtime_handler = self.respond
        try:
            return subprocess.run(
                [str(self.binary), "--config", str(self.config),
                 "--dotdir", str(self.state), *args],
                cwd=self.case,
                env={**os.environ, "SNAJPAGENT_IRC_UI_KEY": SECRET},
                capture_output=True, text=True, timeout=60)
        finally:
            self.provider.runtime_handler = None

    def record(self):
        _, self.events = harness.read_events(self.state)
        self.result = self.result
        return self.events

    def finish(self, args):
        self.result = self.run(args)
        self.record()
        return self.result

    def path(self, name):
        return self.case / name


def tool_outputs(request):
    return [item for item in request["input"]
            if item.get("type") == "function_call_output"]


def responder(provider, calls, final, inspect=None):
    """Propose calls[len(result) ] after each result; finish after the last."""
    def respond(handler, request, sequence):
        outs = tool_outputs(request)
        if inspect is not None:
            inspect(len(outs), outs, request)
        if len(outs) < len(calls):
            name, arguments = calls[len(outs)]
            provider.reply(handler, provider.function_body(
                sequence, f"rule-call-{len(outs)}", name, arguments).encode())
            return
        provider.reply(handler, provider.response_body(sequence, final).encode())
    return respond


def batch_responder(provider, calls, final):
    """Propose every call in one response, then finish after the results."""
    def respond(handler, request, sequence):
        if not tool_outputs(request):
            provider.reply(handler, provider.functions_body(
                sequence, [(f"rule-batch-{i}", name, arguments)
                           for i, (name, arguments) in enumerate(calls)]).encode())
            return
        provider.reply(handler, provider.response_body(sequence, final).encode())
    return respond


def finished(case):
    return harness.event_list(case.events, "tool_finished")


def started(case):
    return harness.event_list(case.events, "tool_started")


def logs(case):
    return harness.event_list(case.events, "rule_log")


def single_result(case):
    entries = finished(case)
    assert len(entries) == 1, entries
    return entries[0]["data"]["result"]


def case_deny(binary, provider, root):
    case = RulesCase(binary, provider, root, "deny", DENY_RULES)
    seen = []
    case.respond = responder(
        provider, [("exec_command", {"command": "printf x >> marker"})],
        "deny case finished",
        lambda step, outs, request: seen.extend(outs))
    result = case.finish(["-e", "--", "attempt a command"])
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert not started(case), started(case)
    res = single_result(case)
    assert res["status"] == "not_run" and res["reason"] == "rule_rejected", res
    text = res["model_text"]
    assert text.startswith("Blocked exec_command:"), text
    assert not text.startswith('"'), text
    assert "printf x >> marker" in text, text
    assert "(100%)" in text, text
    assert not case.path("marker").exists(), "denied command must not run"
    assert seen and "Blocked exec_command:" in seen[-1]["output"], seen
    assert "deny case finished" in result.stdout, result.stdout
    assert not logs(case), logs(case)
    print("rules e2e deny: ok", flush=True)


def case_allow_nonmatching(binary, provider, root):
    case = RulesCase(binary, provider, root, "allow-nonmatching", ARG_RULES)
    case.respond = responder(
        provider, [("exec_command", {"command": "printf x >> marker"})],
        "allow case finished")
    result = case.finish(["-e", "--", "run a benign command"])
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert len(started(case)) == 1, started(case)
    res = single_result(case)
    assert res["status"] == "succeeded", res
    assert case.path("marker").read_text() == "x"
    assert not logs(case), logs(case)
    print("rules e2e allow-nonmatching: ok", flush=True)


def case_deny_by_argument(binary, provider, root):
    case = RulesCase(binary, provider, root, "deny-by-argument", ARG_RULES)
    case.path("victim").write_text("keep me\n")
    case.respond = responder(
        provider, [("exec_command", {"command": "rm -rf victim"})],
        "argument case finished")
    result = case.finish(["-e", "--", "try a destructive command"])
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert not started(case), started(case)
    res = single_result(case)
    assert res["status"] == "not_run" and res["reason"] == "rule_rejected", res
    assert "rm is disabled here." in res["model_text"], res
    assert case.path("victim").read_text() == "keep me\n", "rm must not run"
    print("rules e2e deny-by-argument: ok", flush=True)


def case_allowlist(binary, provider, root):
    case = RulesCase(binary, provider, root, "allowlist", ALLOWLIST_RULES)
    case.respond = responder(
        provider,
        [("exec_command", {"command": "printf ok >> marker"}),
         ("apply_patch", {"patch": "*** Begin Patch\n*** Add File: forbidden.txt\n+x\n*** End Patch\n"})],
        "allowlist case finished")
    result = case.finish(["-e", "--", "run the audited tool then try a write"])
    assert result.returncode == 0, (result.stdout, result.stderr)
    entries = finished(case)
    assert len(entries) == 2, entries
    allowed, denied = entries[0]["data"]["result"], entries[1]["data"]["result"]
    assert allowed["status"] == "succeeded", allowed
    assert case.path("marker").read_text() == "ok"
    assert denied["status"] == "not_run" and denied["reason"] == "rule_rejected", denied
    assert "Only the audited command tool is permitted." in denied["model_text"], denied
    assert len(started(case)) == 1, started(case)
    assert not case.path("forbidden.txt").exists(), "denied write must not run"
    print("rules e2e allowlist: ok", flush=True)


def case_jump_chain(binary, provider, root):
    case = RulesCase(binary, provider, root, "jump-chain", JUMP_RULES)
    case.respond = responder(
        provider, [("exec_command", {"command": "printf x >> marker"})],
        "jump case finished")
    result = case.finish(["-e", "--", "try through the shared policy"])
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert not started(case), started(case)
    res = single_result(case)
    assert res["status"] == "not_run" and res["reason"] == "rule_rejected", res
    assert "Reusable policy denied exec_command." in res["model_text"], res
    assert not case.path("marker").exists()
    print("rules e2e jump-chain: ok", flush=True)


def case_log(binary, provider, root):
    case = RulesCase(binary, provider, root, "log", LOG_RULES)
    case.respond = responder(
        provider, [("exec_command", {"command": "printf x >> marker"})],
        "log case finished")
    result = case.finish(["-e", "--", "audit a command"])
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert len(started(case)) == 1, started(case)
    assert single_result(case)["status"] == "succeeded"
    assert case.path("marker").read_text() == "x", "logging must not block the call"
    entries = logs(case)
    assert len(entries) == 1, entries
    data = entries[0]["data"]
    assert data["rule"] == "audit" and data["chain"] == "out", data
    assert data["message"].startswith("tool=exec_command"), data
    assert "printf x >> marker" in data["message"], data
    print("rules e2e log: ok", flush=True)


def case_sticky_reject_then_log(binary, provider, root):
    case = RulesCase(binary, provider, root, "sticky", STICKY_RULES)
    case.respond = responder(
        provider, [("exec_command", {"command": "printf x >> marker"})],
        "sticky case finished")
    result = case.finish(["-e", "--", "attempt a command"])
    assert result.returncode == 0, (result.stdout, result.stderr)
    res = single_result(case)
    assert res["status"] == "not_run" and res["reason"] == "rule_rejected", res
    assert not case.path("marker").exists()
    entries = logs(case)
    assert len(entries) == 1, ("later log rule must still run after a reject", entries)
    assert entries[0]["data"]["message"] == "audit exec_command", entries[0]
    print("rules e2e sticky-reject-then-log: ok", flush=True)


def case_pass_does_not_exempt(binary, provider, root):
    case = RulesCase(binary, provider, root, "pass-then-deny", PASS_THEN_DENY_RULES)
    case.respond = responder(
        provider, [("exec_command", {"command": "printf x >> marker"})],
        "pass-then-deny finished")
    result = case.finish(["-e", "--", "a pass rule must not exempt this call"])
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert not started(case), started(case)
    res = single_result(case)
    assert res["status"] == "not_run" and res["reason"] == "rule_rejected", res
    assert "pass does not exempt a call." in res["model_text"], res
    assert not case.path("marker").exists()
    print("rules e2e pass-does-not-exempt: ok", flush=True)


def case_threshold(binary, provider, root):
    case = RulesCase(binary, provider, root, "threshold", THRESHOLD_RULES)
    case.respond = responder(
        provider,
        [("exec_command", {"command": "printf x >> marker",
                           "max_output_tokens": 1000000})],
        "threshold case finished")
    result = case.finish(["-e", "--", "request a huge budget"])
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert not started(case), started(case)
    res = single_result(case)
    assert res["status"] == "not_run" and res["reason"] == "rule_rejected", res
    assert "Requested output budget exceeds policy." in res["model_text"], res
    print("rules e2e threshold: ok", flush=True)


def case_threshold_under(binary, provider, root):
    case = RulesCase(binary, provider, root, "threshold-under", THRESHOLD_RULES)
    case.respond = responder(
        provider,
        [("exec_command", {"command": "printf x >> marker",
                           "max_output_tokens": 100})],
        "threshold under case finished")
    result = case.finish(["-e", "--", "request a small budget"])
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert len(started(case)) == 1, started(case)
    assert single_result(case)["status"] == "succeeded"
    assert case.path("marker").read_text() == "x"
    print("rules e2e threshold-under: ok", flush=True)


def case_return_stops(binary, provider, root):
    case = RulesCase(binary, provider, root, "return", RETURN_RULES)
    case.respond = responder(
        provider, [("exec_command", {"command": "printf x >> marker"})],
        "return case finished")
    result = case.finish(["-e", "--", "return stops the policy"])
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert len(started(case)) == 1, ("return at the entry chain must stop before the reject rule",
                                     started(case))
    assert single_result(case)["status"] == "succeeded"
    assert case.path("marker").read_text() == "x"
    print("rules e2e return: ok", flush=True)


def case_multi_call_mixed(binary, provider, root):
    case = RulesCase(binary, provider, root, "multi-call", ARG_RULES)
    case.path("victim").write_text("keep me\n")
    case.respond = batch_responder(
        provider,
        [("exec_command", {"command": "printf ok >> marker"}),
         ("exec_command", {"command": "rm -rf victim"})],
        "multi call case finished")
    result = case.finish(["-e", "--", "run two independent commands"])
    assert result.returncode == 0, (result.stdout, result.stderr)
    entries = finished(case)
    assert len(entries) == 2, entries
    statuses = sorted(entry["data"]["result"]["status"] for entry in entries)
    assert statuses == ["not_run", "succeeded"], statuses
    assert len(started(case)) == 1, started(case)
    assert case.path("marker").read_text() == "ok"
    assert case.path("victim").read_text() == "keep me\n", "denied call must not run"
    print("rules e2e multi-call: ok", flush=True)


def case_durability_resume(binary, provider, root):
    case = RulesCase(binary, provider, root, "durability", STICKY_RULES)
    case.respond = responder(
        provider, [("exec_command", {"command": "printf x >> marker"})],
        "first turn finished")
    result = case.finish(["-e", "--", "first turn"])
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert single_result(case)["status"] == "not_run"
    assert len(logs(case)) == 1, logs(case)
    log_path, _ = harness.read_events(case.state)
    session = log_path.parent.name

    seen = []
    case.respond = responder(
        provider, [], "second turn finished",
        lambda step, outs, request: seen.append(request))
    result = case.finish(["-e", "--resume", session, "--", "second turn"])
    assert result.returncode == 0, (
        "a session that logged a rule must still resume", result.stdout, result.stderr)
    assert seen, "resumed turn must reach the provider"
    prior = [item for item in seen[0]["input"]
             if item.get("type") == "function_call_output"]
    assert any("denied exec_command" in (item.get("output") or "") for item in prior), prior
    assert len(logs(case)) == 1, ("resume must not drop or duplicate the rule log", logs(case))
    print("rules e2e durability-resume: ok", flush=True)


def case_no_rules_baseline(binary, provider, root):
    case = RulesCase(binary, provider, root, "no-rules", None)
    case.respond = responder(
        provider, [("exec_command", {"command": "printf x >> marker"})],
        "baseline case finished")
    result = case.finish(["-e", "--", "run without policy"])
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert len(started(case)) == 1, started(case)
    assert single_result(case)["status"] == "succeeded"
    assert case.path("marker").read_text() == "x"
    assert not logs(case), logs(case)
    print("rules e2e no-rules-baseline: ok", flush=True)


def assert_startup_refused(binary, provider, root, name, rules):
    before = len(provider.requests)
    case = RulesCase(binary, provider, root, name, rules)
    result = case.run(["-e", "--", "should never run"])
    assert result.returncode != 0, (name, result.stdout, result.stderr)
    message = result.stdout + result.stderr
    assert "snajpagent:" in message, (name, message)
    assert len(provider.requests) == before, (name, "invalid config reached the provider")
    print(f"rules e2e {name}: ok", flush=True)


def case_invalid_configs(binary, provider, root):
    assert_startup_refused(
        binary, provider, root, "invalid-jump",
        "[rule bad-jump]\nchain = out\naction = jump\ntarget = nowhere\n")
    assert_startup_refused(
        binary, provider, root, "invalid-regex",
        '[rule bad-regex]\nchain = out\nmatch = {"/tool":"("}\naction = reject\n'
        'text = "x"\n')
    assert_startup_refused(
        binary, provider, root, "invalid-entry-chain",
        "[rule in-chain]\nchain = in\naction = reject\ntext = \"x\"\n")
    assert_startup_refused(
        binary, provider, root, "invalid-key",
        "[rule unknown-key]\nchain = out\naction = pass\nbogus = 1\n")
    assert_startup_refused(
        binary, provider, root, "invalid-action",
        "[rule bad-action]\nchain = out\naction = explode\n")
    assert_startup_refused(
        binary, provider, root, "invalid-duplicate",
        "[rule dup]\nchain = out\naction = pass\n"
        "[rule dup]\nchain = out\naction = pass\n")


CASES = (
    case_deny,
    case_allow_nonmatching,
    case_deny_by_argument,
    case_allowlist,
    case_jump_chain,
    case_log,
    case_sticky_reject_then_log,
    case_pass_does_not_exempt,
    case_threshold,
    case_threshold_under,
    case_return_stops,
    case_multi_call_mixed,
    case_durability_resume,
    case_no_rules_baseline,
    case_invalid_configs,
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    args = parser.parse_args()
    root = Path(tempfile.mkdtemp(prefix="snajpagent-rules-e2e-"))
    os.environ["HOME"] = str(root / "home")
    (root / "home").mkdir(mode=0o700)
    provider = harness.FakeResponses()
    binary = Path(args.binary).resolve()
    try:
        for case in CASES:
            case(binary, provider, root)
        assert not provider.failure, provider.failure
        print("rules_e2e: ok", flush=True)
    finally:
        provider.close()
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
