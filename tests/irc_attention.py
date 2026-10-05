# SPDX-License-Identifier: GPL-2.0-only
"""Real IRC, Responses HTTP, and rendered terminal coverage for IRC attention."""
import argparse
import json
import threading
import time
from pathlib import Path

from irc_client import IRCClient
from tmux_terminal import (
    FakeResponses, TmuxTerminal, event_list, fixture_terminal, free_loopback_port,
    irc_workspace, read_events, wait_event_count,
)


def run_irc_attention_case(binary, root):
    root.mkdir(parents=True, exist_ok=True)
    provider = FakeResponses()
    workspace, config = irc_workspace(root / "workspace", provider.port, "host-model")
    config.write_text(config.read_text().replace("idle_timeout_ms = 3000",
        "idle_timeout_ms = 20000").replace("request_timeout_ms = 5000",
        "request_timeout_ms = 25000"))
    state = root / "state"
    port = free_loopback_port()
    release, started = threading.Event(), threading.Event()
    seen, branches, issued = [], [], set()
    branch_failure = False
    commands = {
        "sleep-count": ("irc_sleep", {"delay_ms": 60000, "wake_after_messages": 2}),
        "sleep-mention": ("irc_sleep", {"delay_ms": 60000, "wake_after_messages": 20}),
        "sleep-timeout": ("irc_sleep", {"delay_ms": 1200, "wake_after_messages": 20}),
        "sleep-resume": ("irc_sleep", {"delay_ms": 60000, "wake_after_messages": 20}),
        "compact-enable": ("irc_compact", {"after_updates": 1,
                                           "instruction": "retain decisions"}),
        "compact-off": ("irc_compact", {"after_updates": 0}),
        "compact-fail": ("irc_compact", {"after_updates": 1}),
        "compact-reload": ("irc_compact", {"after_updates": 1}),
        "compact-stale": ("irc_compact", {"after_updates": 1}),
        "rename-model": ("irc_nick", {"destination": None, "nick": "newbot"}),
        "sleep-renamed": ("irc_sleep", {"delay_ms": 60000, "wake_after_messages": 20}),
        "restore-model": ("irc_nick", {"destination": None, "nick": "hostbot"}),
    }

    def wait_for(predicate, description):
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            if provider.failure:
                raise AssertionError(provider.failure)
            if predicate():
                return
            time.sleep(0.025)
        raise AssertionError(description)

    def respond(handler, request, sequence):
        if not request.get("tools") and "IRC context compaction branch" in json.dumps(request):
            branches.append(request)
            started.set()
            assert release.wait(20), "summary branch was not released"
            if branch_failure:
                provider.reply(handler, b'{"error":{"message":"summary rejected"}}',
                               "application/json", status=400, close_header=True)
            else:
                provider.reply(handler, provider.response_body(sequence,
                    "IRC decisions condensed fixture").encode(), close_header=True)
        elif not request.get("tools"):
            provider.reply(handler, provider.response_body(sequence,
                "Whole conversation compacted fixture").encode(), close_header=True)
        else:
            seen.append(request)
            prompt = provider.latest_user(request)
            if prompt in commands and prompt not in issued:
                issued.add(prompt)
                name, arguments = commands[prompt]
                wire = provider.function_body(sequence, "attn_" + prompt, name, arguments)
            else:
                wire = provider.response_body(sequence, f"attention done {sequence}")
            provider.reply(handler, wire.encode(), close_header=True)
        handler.close_connection = True

    def tool(terminal, prompt, event):
        before = len(event_list(read_events(state)[1], event))
        terminal.submit(prompt)
        wait_event_count(state, event, before + 1)
        wait_for(lambda: any(
            any(i.get("type") == "function_call_output" and
                i.get("call_id") == "attn_" + prompt for i in r["input"]) for r in seen),
            "tool result was not sent back to the model")
        wait_for(lambda: not read_events(state)[1][-1]["type"] == "response_started",
                 "tool continuation did not finish")

    def contains(request, text):
        return text in json.dumps(request.get("input", []))

    provider.runtime_handler = respond
    peer = None
    environment = {"SNAJPAGENT_IRC_UI_KEY": "irc-ui-secret"}
    args = ("-s", f"127.0.0.1:{port}", "-n", "hostbot", "-o", "hostop", "-r", "lab")
    try:
        with fixture_terminal(TmuxTerminal(root / "terminal", binary, workspace, state,
                config, 150, 24, args=args, environment=environment), root / "screen.txt") as term:
            term.wait("hostop@")
            term.submit("/rollout")
            term.wait("host-model/medium")
            term.submit("operator-task-kept")
            wait_event_count(state, "turn_completed", 1)
            peer = IRCClient(port, "attnpeer")
            wait_for(lambda: any(e["data"].get("nick") == "attnpeer"
                for e in event_list(read_events(state)[1], "irc_event")), "peer did not join")
            tool(term, "sleep-count", "irc_sleep_set")
            first = len(seen)
            peer.message("held-count-one")
            term.submit("/chat")
            term.wait("held-count-one")
            term.submit("/rollout")
            time.sleep(0.2)
            assert not any(contains(r, "held-count-one") for r in seen[first:])
            peer.message("held-count-two")
            wait_for(lambda: any(contains(r, "held-count-one") and
                     contains(r, "held-count-two") for r in seen[first:]), "count did not wake")
            tool(term, "sleep-mention", "irc_sleep_set")
            first = len(seen)
            peer.message("quiet-before-mention")
            term.submit("/chat")
            term.wait("quiet-before-mention")
            term.submit("/rollout")
            time.sleep(0.15)
            assert not any(contains(r, "quiet-before-mention") for r in seen[first:])
            peer.message("hostbot: urgent attention")
            wait_for(lambda: any(contains(r, "urgent attention") for r in seen[first:]),
                     "accepted model nick did not wake")
            tool(term, "sleep-timeout", "irc_sleep_set")
            first = len(seen)
            peer.message("held-until-deadline")
            term.submit("/chat")
            term.wait("held-until-deadline")
            term.submit("/rollout")
            wait_for(lambda: any(contains(r, "held-until-deadline") for r in seen[first:]),
                     "deadline did not wake")
            reasons = {e["data"]["reason"] for e in event_list(read_events(state)[1],
                                                               "irc_sleep_woke")}
            assert reasons == {"messages", "mention", "timeout"}, reasons

            peer.message("original-IRC-detail")
            wait_for(lambda: any(contains(r, "original-IRC-detail") for r in seen),
                     "baseline IRC was not delivered")
            tool(term, "compact-enable", "irc_compact_configured")
            assert started.wait(10), term.capture()
            branch = branches[0]
            assert branch["model"] == "host-model" and branch["reasoning"]["effort"] == "medium"
            assert contains(branch, "operator-task-kept") and contains(branch, "original-IRC-detail")
            assert any(r["input"] == branch["input"][:-1] and
                       r.get("prompt_cache_key") == branch.get("prompt_cache_key") for r in seen)
            term.submit("foreground-while-summary-held")
            wait_for(lambda: any(provider.latest_user(r) == "foreground-while-summary-held"
                                 for r in seen), "summary blocked foreground work")
            peer.message("tail-during-summary")
            term.submit("/chat")
            term.wait("tail-during-summary")
            term.submit("/rollout")
            wait_for(lambda: any(contains(r, "tail-during-summary") for r in seen), "tail not admitted")
            release.set()
            wait_event_count(state, "irc_compacted", 1)
            term.submit("verify-summary-projection")
            wait_for(lambda: any(provider.latest_user(r) == "verify-summary-projection"
                                 for r in seen), "post-summary request missing")
            checked = next(r for r in seen if provider.latest_user(r) == "verify-summary-projection")
            assert contains(checked, "IRC decisions condensed fixture")
            assert not contains(checked, "original-IRC-detail"), checked
            assert contains(checked, "tail-during-summary") and contains(checked, "operator-task-kept")
            assert any(i.get("type") == "function_call_output" for i in checked["input"])
            term.submit("/chat")
            term.wait("tail-during-summary")
            screen = term.capture(join_wrapped=True)
            assert "original-IRC-detail" in screen and "tail-during-summary" in screen
            term.submit("/rollout")
            tool(term, "compact-off", "irc_compact_configured")

            # Failed summaries leave the exact raw source available to subsequent requests.
            peer.message("keep-after-summary-failure")
            wait_for(lambda: any(contains(r, "keep-after-summary-failure") for r in seen),
                     "failure source missing")
            branch_failure = True
            started.clear()
            tool(term, "compact-fail", "irc_compact_configured")
            assert started.wait(10)
            term.wait("IRC context compaction failed")
            term.submit("verify-failure-kept")
            wait_for(lambda: any(provider.latest_user(r) == "verify-failure-kept" for r in seen),
                     "failure follow-up missing")
            checked = next(r for r in seen if provider.latest_user(r) == "verify-failure-kept")
            assert contains(checked, "keep-after-summary-failure")

            # A whole-conversation compaction must not adopt an older branch afterwards.
            branch_failure = False
            release.clear()
            started.clear()
            tool(term, "compact-stale", "irc_compact_configured")
            assert started.wait(10)
            before = len(event_list(read_events(state)[1], "irc_compacted"))
            term.submit("/compact")
            wait_event_count(state, "compaction_completed", 1)
            release.set()
            term.submit("verify-stale-discard")
            wait_for(lambda: any(provider.latest_user(r) == "verify-stale-discard" for r in seen),
                     "ordinary compaction follow-up missing")
            assert len(event_list(read_events(state)[1], "irc_compacted")) == before

            # Reload cancels an owned branch before freeing its credential snapshot.
            release.clear()
            started.clear()
            tool(term, "compact-reload", "irc_compact_configured")
            assert started.wait(10)
            term.submit_wait("/configure", "configuration reloaded:")
            release.set()
            # Wake matching follows the accepted model nick, not its startup preference.
            tool(term, "rename-model", "irc_snapshot")
            tool(term, "sleep-renamed", "irc_sleep_set")
            first = len(seen)
            peer.message("hostbot: former nick stays asleep")
            time.sleep(0.25)
            assert not any(contains(r, "former nick stays asleep") for r in seen[first:])
            peer.message("newbot: accepted nick wakes")
            wait_for(lambda: any(contains(r, "accepted nick wakes") for r in seen[first:]),
                     "renamed model mention did not wake")
            tool(term, "restore-model", "irc_snapshot")
            # A fresh setup restores sleep after the background branch.
            tool(term, "sleep-resume", "irc_sleep_set")
            session = read_events(state)[0].parent.name
            peer.close()
            peer = None
            term.exit()

        with fixture_terminal(TmuxTerminal(root / "resumed", binary, workspace, state, config,
                150, 24, args=("--resume", session), environment=environment),
                root / "resumed-screen.txt") as term:
            term.wait("hostop@")
            term.submit("/rollout")
            term.wait("host-model/medium")
            peer = IRCClient(port, "resumedpeer")
            peer.message("held-across-resume")
            term.submit("/chat")
            term.wait("held-across-resume")
            term.submit("/rollout")
            before = len(seen)
            time.sleep(0.25)
            assert not any(contains(r, "held-across-resume") for r in seen[before:])
            peer.message("hostbot: wake resumed")
            wait_for(lambda: any(contains(r, "held-across-resume") for r in seen[before:]),
                     "resumed sleep failed to wake")
            assert any(e["data"].get("text") == "original-IRC-detail"
                       for e in event_list(read_events(state)[1], "irc_event"))
            term.exit()
    finally:
        release.set()
        if peer:
            peer.close()
        provider.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("root", type=Path)
    options = parser.parse_args()
    run_irc_attention_case(options.binary.resolve(), options.root.resolve())
    print("IRC attention integration passed")
