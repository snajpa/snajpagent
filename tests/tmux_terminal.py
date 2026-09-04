#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import argparse
import fcntl
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path


LIVE_PROMPT = (
    "great... now please gather the complete state of livepatch status for "
    "vpsadminos kernel 6.12.95"
)
RENDER_TEXT = (
    "alpha beta gamma delta epsilon zeta eta theta\n"
    "explicit café € line\n"
    "supercalifragilisticexpialidocious0123456789ABCDEFGHIJ "
    "tail control:\x1b[31m"
)


def read_events(dotdir):
    paths = sorted((dotdir / "sessions").glob("*/events.jsonl"))
    if len(paths) != 1:
        raise AssertionError(f"expected one session log, got {paths!r}")
    events = []
    with paths[0].open(encoding="utf-8") as source:
        for line in source:
            if line.strip():
                events.append(json.loads(line))
    return paths[0], events


def maybe_events(dotdir):
    paths = sorted((dotdir / "sessions").glob("*/events.jsonl"))
    if len(paths) != 1:
        return None, []
    try:
        return read_events(dotdir)
    except (json.JSONDecodeError, OSError):
        return paths[0], []


def event_list(events, kind):
    return [event for event in events if event["type"] == kind]


def normalize_space(text):
    return " ".join(text.split())


class TmuxTerminal:
    def __init__(self, root, binary, workspace, dotdir, config, cols, rows):
        self.root = root
        self.binary = os.path.abspath(binary)
        self.workspace = os.path.abspath(workspace)
        self.dotdir = dotdir
        self.cols = cols
        self.rows = rows
        self.socket = root / "tmux.sock"
        self.session = "snajpagent-terminal"
        self.target = f"{self.session}:0.0"
        self.last_screen = ""
        self.started = False
        self.root.mkdir(mode=0o700, parents=True)
        if self.socket.exists():
            raise AssertionError(f"refusing existing tmux socket {self.socket}")
        if len(str(self.socket).encode()) >= 100:
            raise AssertionError(f"tmux socket path is too long: {self.socket}")
        tmux_conf = root / "tmux.conf"
        tmux_conf.write_text(
            "set -g status off\n"
            "set -g history-limit 100000\n"
            "set -g remain-on-exit on\n"
            "set -g default-terminal screen-256color\n",
            encoding="utf-8",
        )
        command = [self.binary, "-d", str(dotdir)]
        if config is not None:
            command.extend(["-c", str(config)])
        env = os.environ.copy()
        env.pop("TMUX", None)
        env["LC_ALL"] = "C.utf8"
        try:
            subprocess.run(
                [
                    "tmux", "-S", str(self.socket), "-f", str(tmux_conf),
                    "new-session", "-d", "-x", str(cols), "-y", str(rows),
                    "-s", self.session, "-c", self.workspace,
                    shlex.join(command),
                ],
                env=env,
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            self.started = True
            size = self.run(
                "display-message", "-p", "-t", self.target,
                "#{pane_width}x#{pane_height}",
            ).strip()
            if size != f"{cols}x{rows}":
                raise AssertionError(f"unexpected tmux pane size {size!r}")
        except BaseException:
            self.close()
            raise

    def run(self, *args, check=True):
        env = os.environ.copy()
        env.pop("TMUX", None)
        result = subprocess.run(
            ["tmux", "-S", str(self.socket), *args],
            env=env,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if check and result.returncode != 0:
            raise AssertionError(
                f"tmux {' '.join(args)} failed ({result.returncode}): "
                f"{result.stderr.strip()}"
            )
        return result.stdout

    def capture(self, join_wrapped=False):
        args = ["capture-pane", "-p"]
        if join_wrapped:
            args.append("-J")
        args.extend(["-t", self.target, "-S", "-"])
        self.last_screen = self.run(*args)
        if "\x1b" in self.last_screen:
            raise AssertionError("tmux rendered pane contains a raw escape byte")
        return self.last_screen

    def wait(self, needle, timeout=10.0, join_wrapped=False):
        deadline = time.monotonic() + timeout
        screen = ""
        while time.monotonic() < deadline:
            screen = self.capture(join_wrapped=join_wrapped)
            if needle in screen:
                return screen
            if self.dead():
                raise AssertionError(
                    f"pane exited while waiting for {needle!r}:\n{screen}"
                )
            time.sleep(0.02)
        raise AssertionError(f"timeout waiting for {needle!r}:\n{screen}")

    def send_text(self, text):
        self.run("send-keys", "-t", self.target, "-l", "--", text)

    def send_key(self, key):
        self.run("send-keys", "-t", self.target, key)

    def submit(self, text):
        self.send_text(text)
        self.send_key("Enter")

    def resize(self, cols, rows):
        self.run(
            "resize-window", "-t", f"{self.session}:0",
            "-x", str(cols), "-y", str(rows),
        )
        self.cols = cols
        self.rows = rows
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            size = self.run(
                "display-message", "-p", "-t", self.target,
                "#{pane_width}x#{pane_height}",
            ).strip()
            if size == f"{cols}x{rows}":
                return
            time.sleep(0.02)
        raise AssertionError(f"tmux did not resize to {cols}x{rows}")

    def dead(self):
        value = self.run(
            "display-message", "-p", "-t", self.target, "#{pane_dead}",
            check=False,
        ).strip()
        return value == "1"

    def wait_dead(self, timeout=10.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.dead():
                return
            time.sleep(0.02)
        raise AssertionError("snajpagent did not exit")

    def exit(self):
        self.submit("/exit")
        self.wait_dead()

    def close(self):
        try:
            if self.started:
                try:
                    self.last_screen = self.capture()
                except Exception:
                    pass
            if os.path.lexists(self.socket):
                self.run("kill-server", check=False)
        finally:
            self.started = False
            try:
                self.socket.unlink()
            except FileNotFoundError:
                pass


def close_fixture_terminal(terminal):
    terminal.close()
    if os.path.lexists(terminal.socket):
        raise AssertionError(f"tmux socket survived cleanup: {terminal.socket}")


def write_config(path, read_agents, pause_ms=300):
    path.write_text(
        f"[agent]\nread_agents_md = {'true' if read_agents else 'false'}\n"
        f"[ui]\ntyping_pause_ms = {pause_ms}\nverbosity = 0\n",
        encoding="utf-8",
    )


def assert_order(screen, fragments):
    offset = 0
    for fragment in fragments:
        position = screen.find(fragment, offset)
        if position < 0:
            raise AssertionError(
                f"rendered fragment {fragment!r} is missing or reordered:\n{screen}"
            )
        offset = position + len(fragment)


def run_status_case(binary, root):
    case = root / "status"
    workspace = case / "workspace"
    workspace.mkdir(mode=0o700, parents=True)
    config = case / "config.ini"
    write_config(config, False)
    dotdir = case / "state"
    terminal = TmuxTerminal(
        case / "terminal", binary, workspace, dotdir, config, 40, 14
    )
    try:
        terminal.wait("\n›")
        terminal.submit("terminal_status")
        terminal.wait("working…", timeout=3.0)
        terminal.wait("status-first-fragment", timeout=3.0)
        time.sleep(0.85)
        middle = terminal.capture(join_wrapped=True)
        if "status-first-fragment" not in middle:
            raise AssertionError(f"activity redraw erased streamed text:\n{middle}")
        if "working…" in middle:
            raise AssertionError(f"activity status interrupted a public item:\n{middle}")
        final = terminal.wait("status-second-fragment", timeout=3.0,
                              join_wrapped=True)
        if ("status-first-fragment status-second-fragment" not in
                normalize_space(final)):
            raise AssertionError(f"streamed status output was mangled:\n{final}")
        _, events = wait_for_terminal_event(dotdir, {"turn_completed"}, 5.0)
        completed = event_list(events, "response_completed")
        expected = "status-first-fragment status-second-fragment"
        if len(completed) != 1 or completed[0]["data"]["items"][0]["text"] != expected:
            raise AssertionError("status scenario changed durable assistant text")
        terminal.exit()
    finally:
        try:
            screen = terminal.last_screen or terminal.capture()
            (case / "screen.txt").write_text(screen, encoding="utf-8")
        finally:
            close_fixture_terminal(terminal)


def run_render_case(binary, root):
    case = root / "render"
    workspace = case / "workspace"
    workspace.mkdir(mode=0o700, parents=True)
    agents = workspace / "AGENTS.md"
    agents_text = "Fixture terminal instructions.\n"
    agents.write_text(agents_text, encoding="utf-8")
    config = case / "config.ini"
    write_config(config, True, pause_ms=1500)
    dotdir = case / "state"
    terminal = TmuxTerminal(
        case / "terminal", binary, workspace, dotdir, config, 32, 18
    )
    try:
        terminal.wait("\n›")
        terminal.submit("terminal_render")
        terminal.wait("alpha beta gamma delta epsilon")
        terminal.send_text("draft")
        first = terminal.wait("steer › draft")
        assert_order(first, ["alpha beta gamma delta epsilon", "steer › draft"])

        time.sleep(0.1)
        pause_started = time.monotonic()
        terminal.send_text(" plus")
        terminal.wait("steer › draft plus")
        time.sleep(1.1)
        paused = terminal.capture(join_wrapped=True)
        if "explicit café € line" in paused:
            raise AssertionError(
                f"model output ignored the configured typing pause:\n{paused}"
            )
        second = terminal.wait("explicit café € line", timeout=4.0)
        if time.monotonic() - pause_started < 1.2:
            raise AssertionError("model output resumed before the typing pause")
        assert_order(second, ["steer › draft plus", "explicit café € line"])
        if "steer › draft plus\n\nzeta eta theta" in second:
            raise AssertionError(f"output resumed with a spurious blank line:\n{second}")
        if "steer › draft plus\nzeta eta theta" not in second:
            raise AssertionError(f"output did not resume directly below the draft:\n{second}")

        repeat_pause_started = time.monotonic()
        terminal.send_text(" again with long resize text")
        terminal.wait("steer › draft plus again", join_wrapped=True)
        terminal.resize(46, 18)
        resized = terminal.wait(
            "steer › draft plus again with long resize text",
            join_wrapped=True,
        )
        exact_margin = "steer › draft plus again with long resize text"
        if len(exact_margin) != 46:
            raise AssertionError("right-margin fixture is not exactly 46 columns")
        if resized.count(exact_margin) != 1:
            raise AssertionError(f"resized composer was duplicated:\n{resized}")
        time.sleep(0.75)
        paused_again = terminal.capture(join_wrapped=True)
        if "supercalifragilisticexpialidocious" in paused_again:
            raise AssertionError(
                f"repeated editing did not restart the typing pause:\n{paused_again}"
            )

        final = terminal.wait("control:\\x1B[31m", timeout=5.0)
        if time.monotonic() - repeat_pause_started < 1.2:
            raise AssertionError("repeated typing pause ended too early")
        if f"{exact_margin}\n\nsupercalifragilisticexpialidocious" in final:
            raise AssertionError(
                f"exact-margin output resumed after a blank row:\n{final}"
            )
        if f"{exact_margin}\nsupercalifragilisticexpialidocious" not in final:
            raise AssertionError(
                f"exact-margin output did not resume on the next row:\n{final}"
            )
        _, events = wait_for_terminal_event(dotdir, {"turn_completed"}, 5.0)
        joined = terminal.capture(join_wrapped=True)
        assert_order(
            joined,
            [
                "alpha beta gamma delta epsilon",
                "steer › draft plus",
                "explicit café € line",
                "steer › draft plus again with long resize text",
                "supercalifragilisticexpialidocious0123456789ABCDEFGHIJ",
                "control:\\x1B[31m",
            ],
        )
        if "alpha beta gamma delta epsilon zeta" in final:
            raise AssertionError("model output was hard-wrapped instead of word-wrapped")
        completed = event_list(events, "response_completed")
        if len(completed) != 1 or completed[0]["data"]["items"][0]["text"] != RENDER_TEXT:
            raise AssertionError("rendering changed durable assistant text")
        turn = event_list(events, "turn_started")
        if len(turn) != 1:
            raise AssertionError("expected one durable turn")
        instructions = turn[0]["data"]["instructions"]
        expected_sha = hashlib.sha256(agents_text.encode()).hexdigest()
        expected = {
            "bytes": len(agents_text.encode()),
            "path": str(agents),
            "sha256": expected_sha,
        }
        if not instructions or instructions[-1] != expected:
            raise AssertionError(f"unexpected AGENTS.md metadata {instructions!r}")

        terminal.send_key("C-u")
        terminal.submit("slow")
        terminal.wait("working slowly")
        terminal.send_text("change course")
        steering_screen = terminal.wait("steer › change course")
        assert_order(steering_screen, ["working slowly", "steer › change course"])
        terminal.send_key("Enter")
        terminal.wait("steered: change course")
        events = wait_event_count(dotdir, "turn_completed", 2)
        steering = event_list(events, "steering_added")
        interrupted = event_list(events, "response_interrupted")
        if len(steering) != 1 or steering[0]["data"]["text"] != "change course":
            raise AssertionError("rendered steering was not durably recorded")
        if len(interrupted) != 1 or interrupted[0]["data"]["origin"] != "steering":
            raise AssertionError("rendered steering did not interrupt the response")
        terminal.exit()
    finally:
        try:
            screen = terminal.last_screen or terminal.capture()
            (case / "screen.txt").write_text(screen, encoding="utf-8")
        finally:
            close_fixture_terminal(terminal)


def queue_listing(screen, number, text):
    return re.search(
        rf"(?m)^{number} [0-9a-f]{{8}} › {re.escape(text)}$", screen
    ) is not None


def wait_queue_listing(terminal, entries, timeout=5.0):
    deadline = time.monotonic() + timeout
    screen = ""
    while time.monotonic() < deadline:
        screen = terminal.capture()
        if all(queue_listing(screen, number, text)
               for number, text in enumerate(entries, 1)):
            return screen
        if terminal.dead():
            raise AssertionError(f"pane exited while waiting for queue:\n{screen}")
        time.sleep(0.02)
    raise AssertionError(f"timeout waiting for rendered queue {entries!r}:\n{screen}")


def wait_event_count(dotdir, kind, count, timeout=5.0):
    deadline = time.monotonic() + timeout
    events = []
    while time.monotonic() < deadline:
        _, events = maybe_events(dotdir)
        if len(event_list(events, kind)) >= count:
            return events
        time.sleep(0.02)
    raise AssertionError(
        f"timeout waiting for {count} {kind} events; got "
        f"{len(event_list(events, kind))}"
    )


def run_queue_case(binary, root):
    case = root / "queue"
    workspace = case / "workspace"
    workspace.mkdir(mode=0o700, parents=True)
    (workspace / "AGENTS.md").write_text(
        "These instructions must be disabled.\n", encoding="utf-8"
    )
    config = case / "config.ini"
    write_config(config, False, pause_ms=150)
    dotdir = case / "state"
    terminal = TmuxTerminal(
        case / "terminal", binary, workspace, dotdir, config, 48, 20
    )
    try:
        terminal.wait("\n›")
        terminal.submit("queue_slow")
        terminal.wait("working slowly")
        for text in ("first", "second", "third", "fourth"):
            terminal.send_text(text)
            terminal.send_key("Tab")
            terminal.wait(f"next › {text}")

        terminal.submit("/q")
        wait_queue_listing(terminal, ("first", "second", "third", "fourth"))

        terminal.submit("/q p")
        wait_event_count(dotdir, "future_turn_cancelled", 1)
        terminal.wait("1 future turn cancelled")
        terminal.submit("/queue pop")
        wait_event_count(dotdir, "future_turn_cancelled", 2)
        terminal.submit("/queue 1 delete")
        wait_event_count(dotdir, "future_turn_cancelled", 3)
        terminal.submit("/q 1e")
        terminal.wait("edit 1 › second")
        terminal.send_text(" active")
        terminal.send_key("Enter")
        wait_event_count(dotdir, "future_turn_edited", 1)

        terminal.send_text("fifth")
        terminal.send_key("Tab")
        terminal.wait("next › fifth")
        terminal.submit("/queue")
        wait_queue_listing(terminal, ("second active", "fifth"))

        terminal.send_key("C-c")
        terminal.wait("turn interrupted")
        terminal.wait("\n›")
        terminal.submit("/queue 1 edit")
        terminal.wait("edit 1 › second active")
        terminal.send_text(" idle")
        terminal.send_key("Enter")
        wait_event_count(dotdir, "future_turn_edited", 2)
        terminal.submit("/q c")
        wait_event_count(dotdir, "future_turn_cancelled", 4)
        terminal.wait("2 future turns cancelled")
        terminal.submit("/q")
        empty = terminal.wait("future-turn queue is empty")
        assert_order(
            empty,
            [
                "next › first",
                "next › second",
                "next › third",
                "next › fourth",
                "edit 1 › second active",
                "next › fifth",
                "edit 1 › second active idle",
                "future-turn queue is empty",
            ],
        )
        terminal.exit()

        _, events = read_events(dotdir)
        queued = event_list(events, "future_turn_queued")
        edited = event_list(events, "future_turn_edited")
        cancelled = event_list(events, "future_turn_cancelled")
        if [event["data"]["text"] for event in queued] != [
            "first", "second", "third", "fourth", "fifth"
        ]:
            raise AssertionError("durable queued texts do not match the screen")
        if [event["data"]["text"] for event in edited] != [
            "second active", "second active idle"
        ]:
            raise AssertionError("durable edited texts do not match the screen")
        if len(cancelled) != 4:
            raise AssertionError(
                "expected short/long pop, delete, and clear cancellations"
            )
        expected_cancelled = [
            [queued[3]["data"]["queue_id"]],
            [queued[2]["data"]["queue_id"]],
            [queued[0]["data"]["queue_id"]],
            [
                queued[1]["data"]["queue_id"],
                queued[4]["data"]["queue_id"],
            ],
        ]
        if [event["data"]["queue_ids"] for event in cancelled] != expected_cancelled:
            raise AssertionError("queue mutations targeted the wrong rendered items")
        if any(
                event["data"]["queue_id"] != queued[1]["data"]["queue_id"]
                for event in edited):
            raise AssertionError("queue edits did not preserve the rendered item ID")
        turn = event_list(events, "turn_started")
        if len(turn) != 1 or turn[0]["data"]["instructions"] != []:
            raise AssertionError("disabled AGENTS.md discovery was not honored")
    finally:
        try:
            screen = terminal.last_screen or terminal.capture()
            (case / "screen.txt").write_text(screen, encoding="utf-8")
        finally:
            close_fixture_terminal(terminal)


def wait_for_terminal_event(dotdir, terminal_types, timeout):
    deadline = time.monotonic() + timeout
    path = None
    events = []
    while time.monotonic() < deadline:
        path, events = maybe_events(dotdir)
        if any(event["type"] in terminal_types for event in events):
            return path, events
        time.sleep(0.05)
    raise AssertionError(
        f"timeout waiting for {sorted(terminal_types)!r}; last events: "
        f"{[event['type'] for event in events]!r}"
    )


def run_fixture(binary, workspace, root):
    del workspace
    root.mkdir(mode=0o700, parents=True)
    run_status_case(binary, root)
    run_render_case(binary, root)
    run_queue_case(binary, root)
    print("tmux_terminal fixture: ok")


def validate_live_screen(screen, events, workspace):
    turns = event_list(events, "turn_started")
    if len(turns) != 1 or turns[0]["data"]["text"] != LIVE_PROMPT:
        raise AssertionError("live run did not durably admit the exact prompt once")
    instructions = turns[0]["data"]["instructions"]
    agents = Path(workspace) / "AGENTS.md"
    contents = agents.read_bytes()
    expected = {
        "bytes": len(contents),
        "path": str(agents),
        "sha256": hashlib.sha256(contents).hexdigest(),
    }
    if expected not in instructions:
        raise AssertionError(f"live run did not admit {agents}: {instructions!r}")

    items = []
    for response in event_list(events, "response_completed"):
        for item in response["data"]["items"]:
            if item["kind"] in {"assistant", "refusal"} and item.get("text"):
                items.append(item["text"])
    if not items:
        raise AssertionError("live run completed without public model text")
    normalized_screen = normalize_space(screen)
    offset = 0
    for item in items:
        rendered = normalize_space(item)
        position = normalized_screen.find(rendered, offset)
        if position < 0:
            raise AssertionError(
                "durable public item is missing or reordered in rendered tmux "
                f"history: {rendered[:200]!r}"
            )
        if (len(rendered) >= 80 and
                normalized_screen.count(rendered) != items.count(item)):
            raise AssertionError(
                "durable public item was duplicated in rendered tmux history: "
                f"{rendered[:200]!r}"
            )
        offset = position + len(rendered)
    failures = event_list(events, "turn_failed")
    if failures:
        raise AssertionError(f"live turn failed: {failures[-1]!r}")
    if len(event_list(events, "turn_completed")) != 1:
        raise AssertionError("live turn did not complete exactly once")


def run_live(binary, workspace, config, root):
    lock = config.open("rb")
    try:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as exc:
        lock.close()
        raise AssertionError(
            f"another live terminal check is using {config}"
        ) from exc
    terminal = None
    try:
        if root.exists():
            raise AssertionError(f"live result root already exists: {root}")
        root.mkdir(mode=0o700, parents=True)
        dotdir = root / "state"
        terminal = TmuxTerminal(
            root / "terminal", binary, workspace, dotdir, config, 52, 18
        )
        started = time.monotonic()
        last_report = started
        terminal.wait("\n›")
        terminal.submit(LIVE_PROMPT)
        while True:
            _, events = maybe_events(dotdir)
            terminal_types = {event["type"] for event in events}
            if "turn_completed" in terminal_types or "turn_failed" in terminal_types:
                break
            if terminal.dead():
                raise AssertionError("live snajpagent exited before a terminal turn event")
            now = time.monotonic()
            if now - started > 1800.0:
                raise AssertionError("live terminal check exceeded 30 minutes")
            if now - last_report >= 10.0:
                print(
                    "tmux_terminal live: waiting; events="
                    f"{len(events)} responses="
                    f"{len(event_list(events, 'response_completed'))}",
                    flush=True,
                )
                last_report = now
            time.sleep(0.1)

        terminal.wait("\n›", timeout=10.0)
        screen = terminal.capture()
        joined_screen = terminal.capture(join_wrapped=True)
        _, events = read_events(dotdir)
        (root / "screen.txt").write_text(screen, encoding="utf-8")
        (root / "screen-joined.txt").write_text(
            joined_screen, encoding="utf-8"
        )
        (root / "events.json").write_text(
            json.dumps(events, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        if any(len(line) > terminal.cols for line in screen.splitlines()):
            raise AssertionError("raw tmux capture contains an over-width row")
        validate_live_screen(joined_screen, events, workspace)
        terminal.exit()
        print(f"tmux_terminal live: ok; results={root}")
    finally:
        try:
            if terminal is not None:
                try:
                    screen = terminal.capture()
                    joined_screen = terminal.capture(join_wrapped=True)
                    (root / "screen.txt").write_text(screen, encoding="utf-8")
                    (root / "screen-joined.txt").write_text(
                        joined_screen, encoding="utf-8"
                    )
                except Exception:
                    pass
        finally:
            try:
                if terminal is not None:
                    terminal.close()
            finally:
                lock.close()


def main():
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="mode", required=True)
    fixture = subparsers.add_parser("fixture")
    fixture.add_argument("binary")
    fixture.add_argument("workspace")
    fixture.add_argument("root", type=Path)
    live = subparsers.add_parser("live")
    live.add_argument("binary")
    live.add_argument("workspace")
    live.add_argument("config", type=Path)
    live.add_argument("root", type=Path)
    args = parser.parse_args()

    if shutil.which("tmux") is None:
        parser.error("tmux is required")
    if args.mode == "fixture":
        run_fixture(args.binary, args.workspace, args.root)
    else:
        run_live(args.binary, args.workspace, args.config, args.root)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"tmux_terminal: {exc}", file=sys.stderr)
        raise
