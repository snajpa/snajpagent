#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import argparse
import fcntl
import hashlib
import http.server
import json
import os
import re
import shlex
import shutil
import socket
import subprocess
import sys
import threading
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
PACED_TEXT = "Paced tokens form interfragment and finish finalword"


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


class FakeResponses:
    AGENTS = {
        "host-model": "hostbot",
        "one-model": "onebot",
        "two-model": "twobot",
    }

    def __init__(self):
        self.lock = threading.Lock()
        self.requests = []
        self.failure = None
        self.sequence = 0
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_POST(self):
                owner.handle(self)

            def log_message(self, _format, *_args):
                return

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()

    @property
    def port(self):
        return self.server.server_address[1]

    @staticmethod
    def latest_user(request):
        for item in reversed(request.get("input", [])):
            if item.get("role") == "user" and isinstance(item.get("content"), str):
                return item["content"]
        return ""

    @staticmethod
    def event(kind, data):
        return (
            f"event: {kind}\n"
            f"data: {json.dumps(data, ensure_ascii=False, separators=(',', ':'))}\n\n"
        )

    def response_body(self, sequence, text):
        response_id = f"resp_irc_ui_{sequence}"
        created = {
            "type": "response.created",
            "response": {"id": response_id, "status": "in_progress", "output": []},
        }
        completed = {
            "type": "response.completed",
            "response": {
                "id": response_id,
                "status": "completed",
                "usage": {"input_tokens": 1, "output_tokens": 1,
                          "total_tokens": 2},
                "output": [],
            },
        }
        body = self.event("response.created", created)
        if not text:
            return body + self.event("response.completed", completed)
        item_id = f"msg_irc_ui_{sequence}"
        added_item = {
            "id": item_id,
            "type": "message",
            "status": "in_progress",
            "role": "assistant",
            "phase": "final_answer",
            "content": [],
        }
        done_item = dict(added_item)
        done_item["status"] = "completed"
        done_item["content"] = [{"type": "output_text", "text": text,
                                  "annotations": []}]
        events = [
            ("response.output_item.added", {
                "type": "response.output_item.added", "output_index": 0,
                "item": added_item,
            }),
            ("response.content_part.added", {
                "type": "response.content_part.added", "item_id": item_id,
                "output_index": 0, "content_index": 0,
                "part": {"type": "output_text", "text": "",
                         "annotations": []},
            }),
            ("response.output_text.delta", {
                "type": "response.output_text.delta", "item_id": item_id,
                "output_index": 0, "content_index": 0, "delta": text,
            }),
            ("response.output_text.done", {
                "type": "response.output_text.done", "item_id": item_id,
                "output_index": 0, "content_index": 0, "text": text,
            }),
            ("response.output_item.done", {
                "type": "response.output_item.done", "output_index": 0,
                "item": done_item,
            }),
            ("response.completed", completed),
        ]
        return body + "".join(self.event(kind, data) for kind, data in events)

    def handle(self, handler):
        try:
            if handler.path != "/v1/responses":
                raise AssertionError(f"unexpected fake endpoint {handler.path!r}")
            if handler.headers.get("Authorization") != "Bearer irc-ui-secret":
                raise AssertionError("fake endpoint received the wrong credential")
            length = int(handler.headers.get("Content-Length", "-1"))
            if length < 0 or length > 32 * 1024 * 1024:
                raise AssertionError("fake endpoint request length is invalid")
            request = json.loads(handler.rfile.read(length))
            model = request.get("model")
            latest = self.latest_user(request)
            if model not in self.AGENTS:
                raise AssertionError(f"unexpected fake model {model!r}")
            with self.lock:
                self.sequence += 1
                sequence = self.sequence
                self.requests.append({"model": model, "latest": latest})
            marker = None
            if "integration one from oneop" in latest:
                marker = "one"
            elif "integration two from twoop" in latest:
                marker = "two"
            text = f"{self.AGENTS[model]} heard {marker}" if marker else ""
            body = self.response_body(sequence, text).encode()
            handler.send_response(200)
            handler.send_header("Content-Type", "text/event-stream")
            handler.send_header("Content-Length", str(len(body)))
            handler.send_header("Connection", "close")
            handler.end_headers()
            handler.wfile.write(body)
            handler.close_connection = True
        except Exception as exc:
            with self.lock:
                if self.failure is None:
                    self.failure = repr(exc)
            try:
                handler.send_error(500)
            except OSError:
                pass

    def matching_requests(self, marker):
        with self.lock:
            if self.failure:
                raise AssertionError(f"fake endpoint failed: {self.failure}")
            return [request for request in self.requests
                    if marker in request["latest"]]

    def wait_models(self, marker, timeout=10.0):
        deadline = time.monotonic() + timeout
        expected = set(self.AGENTS)
        while time.monotonic() < deadline:
            requests = self.matching_requests(marker)
            if {request["model"] for request in requests} == expected:
                return requests
            time.sleep(0.02)
        raise AssertionError(
            f"fake endpoint did not receive {marker!r} from every model: "
            f"{self.matching_requests(marker)!r}"
        )

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5.0)
        if self.thread.is_alive():
            raise AssertionError("fake endpoint thread did not stop")
        if self.failure:
            raise AssertionError(f"fake endpoint failed: {self.failure}")


class TmuxTerminal:
    def __init__(self, root, binary, workspace, dotdir, config, cols, rows,
                 args=(), environment=None):
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
        command = [self.binary, "--dotdir", str(dotdir)]
        if config is not None:
            command.extend(["--config", str(config)])
        command.extend(args)
        env = os.environ.copy()
        env.pop("TMUX", None)
        env["LC_ALL"] = "C.utf8"
        if environment:
            env.update(environment)
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

    def capture_styled(self):
        return self.run(
            "capture-pane", "-p", "-e", "-t", self.target, "-S", "-"
        )

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


def wait_normalized(terminal, needle, timeout=1.0):
    deadline = time.monotonic() + timeout
    screen = ""
    while time.monotonic() < deadline:
        screen = terminal.capture(join_wrapped=True)
        if needle in normalize_space(screen):
            return screen, time.monotonic()
        if terminal.dead():
            raise AssertionError(
                f"pane exited while waiting for {needle!r}:\n{screen}"
            )
        time.sleep(0.01)
    raise AssertionError(f"timeout waiting for {needle!r}:\n{screen}")


def run_paced_decode_case(binary, root):
    case = root / "decode"
    workspace = case / "workspace"
    workspace.mkdir(mode=0o700, parents=True)
    config = case / "config.ini"
    write_config(config, False)
    dotdir = case / "state"
    terminal = TmuxTerminal(
        case / "terminal", binary, workspace, dotdir, config, 28, 14
    )
    try:
        terminal.wait("\n›")
        terminal.submit("terminal_paced_decode")
        wait_normalized(terminal, "Paced")
        wait_normalized(terminal, "Paced tokens")
        _, split_prefix_at = wait_normalized(
            terminal, "Paced tokens form inter"
        )
        _, split_word_at = wait_normalized(
            terminal, "Paced tokens form interfragment"
        )
        if split_word_at - split_prefix_at < 0.03:
            raise AssertionError(
                "a complete split-word prefix was withheld until its suffix"
            )
        wait_normalized(terminal, "and finish")
        final_screen, final_at = wait_normalized(
            terminal, PACED_TEXT, timeout=0.35
        )
        if "working…" in final_screen:
            raise AssertionError(
                "activity appeared while the paced public item was open"
            )

        time.sleep(0.45)
        held_screen = terminal.capture(join_wrapped=True)
        if PACED_TEXT not in normalize_space(held_screen):
            raise AssertionError("the visible final fragment was erased")
        if "working…" in held_screen:
            raise AssertionError(
                "activity interrupted the provider's post-delta pause"
            )

        activity = terminal.wait("working…", timeout=3.0, join_wrapped=True)
        if time.monotonic() - final_at < 0.8:
            raise AssertionError(
                "activity did not follow the fixture's post-delta pause"
            )
        if not re.search(r"finalword[ \t]*\nworking…", activity):
            raise AssertionError(
                f"activity did not start on the line after visible text:\n{activity}"
            )

        _, events = wait_for_terminal_event(dotdir, {"turn_completed"}, 6.0)
        completed = event_list(events, "response_completed")
        public = [
            item["text"]
            for response in completed
            for item in response["data"]["items"]
            if item["kind"] in {"assistant", "refusal"} and item.get("text")
        ]
        if public != [PACED_TEXT, "paced complete"]:
            raise AssertionError(
                f"paced rendered text differs from durable output: {public!r}"
            )
        final = normalize_space(terminal.capture(join_wrapped=True))
        if final.count(PACED_TEXT) != 1:
            raise AssertionError(
                "paced text was missing, duplicated, or reordered in tmux history"
            )
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
    run_paced_decode_case(binary, root)
    run_render_case(binary, root)
    run_queue_case(binary, root)
    print("tmux_terminal fixture: ok")


def free_loopback_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def write_irc_config(path, provider_port, model):
    path.write_text(
        f"[agent]\nmodel = {model}\nread_agents_md = false\n"
        f"[provider fake]\nbase_url = http://127.0.0.1:{provider_port}/v1\n"
        "api_key_env = SNAJPAGENT_IRC_UI_KEY\n"
        "connect_timeout_ms = 1000\nidle_timeout_ms = 3000\n"
        "request_timeout_ms = 5000\nauto_compact_input_tokens = 0\n"
        "exact_token_count = false\nnative_compaction = false\n"
        "[ui]\ntyping_pause_ms = 50\nverbosity = 0\ncolor = never\n",
        encoding="utf-8",
    )


def wait_current_prompt(terminal, operator, timeout=10.0):
    deadline = time.monotonic() + timeout
    expected = f"{operator} ›"
    screen = ""
    while time.monotonic() < deadline:
        screen = terminal.capture()
        if screen.rstrip().endswith(expected):
            return screen
        if terminal.dead():
            raise AssertionError(
                f"pane exited while waiting for prompt {expected!r}:\n{screen}"
            )
        time.sleep(0.02)
    raise AssertionError(f"current prompt {expected!r} is missing:\n{screen}")


def active_turns(dotdir):
    _, events = maybe_events(dotdir)
    active = set()
    terminal = {
        "turn_completed", "turn_completed_silent", "turn_failed",
        "turn_interrupted", "turn_refused",
    }
    for event in events:
        turn_id = event.get("data", {}).get("turn_id")
        if event["type"] == "turn_started" and turn_id:
            active.add(turn_id)
        elif event["type"] in terminal and turn_id:
            active.discard(turn_id)
    return active


def wait_irc_idle(terminals, timeout=15.0):
    deadline = time.monotonic() + timeout
    stable_since = None
    while time.monotonic() < deadline:
        if all(not active_turns(terminal.dotdir) for terminal in terminals):
            if stable_since is None:
                stable_since = time.monotonic()
            elif time.monotonic() - stable_since >= 0.3:
                return
        else:
            stable_since = None
        if any(terminal.dead() for terminal in terminals):
            raise AssertionError("an IRC integration pane exited while active")
        time.sleep(0.02)
    raise AssertionError(
        f"IRC agents did not become idle: "
        f"{[sorted(active_turns(terminal.dotdir)) for terminal in terminals]!r}"
    )


def wait_irc_quits(terminal, nicks, timeout=15.0):
    deadline = time.monotonic() + timeout
    expected = set(nicks)
    while time.monotonic() < deadline:
        _, events = maybe_events(terminal.dotdir)
        quitters = {event["data"]["nick"] for event in event_list(events, "irc_event")
                   if event["data"]["kind"] == "quit"}
        if expected <= quitters:
            return
        if terminal.dead():
            raise AssertionError("IRC pane exited before peer quit traffic arrived")
        time.sleep(0.02)
    raise AssertionError(f"missing durable IRC quits: {expected - quitters!r}")


def assert_chat_line(screen, nick, text, operator=False):
    marker = "@" if operator else ""
    pattern = rf"(?m)^\d{{2}}:\d{{2}} {re.escape(marker + nick)} › {re.escape(text)}$"
    if re.search(pattern, screen) is None:
        raise AssertionError(f"missing timestamped IRC line {nick!r}: {text!r}\n{screen}")


def validate_irc_events(dotdir):
    _, events = read_events(dotdir)
    expected_messages = [
        ("oneop", "integration one from oneop", True),
        ("twoop", "integration two from twoop", True),
    ]
    for suffix in ("one", "two"):
        expected_messages.extend(
            (nick, f"{nick} heard {suffix}", False)
            for nick in ("hostbot", "onebot", "twobot")
        )
    messages = [event for event in event_list(events, "irc_event")
                if event["data"]["kind"] == "message"]
    for nick, text, operator in expected_messages:
        matches = [event for event in messages
                   if event["data"]["nick"] == nick and
                   event["data"]["text"] == text]
        if len(matches) != 1 or matches[0]["data"]["op"] is not operator:
            raise AssertionError(
                f"IRC message attribution mismatch for {nick!r}: {text!r}"
            )
    joins = {event["data"]["nick"] for event in event_list(events, "irc_event")
             if event["data"]["kind"] == "join"}
    membership = " ".join(joins) + "\n" + "\n".join(
        event["data"]["text"] for event in event_list(events, "irc_snapshot")
    )
    expected_joins = {"hostbot", "hostop", "onebot", "oneop",
                      "twobot", "twoop"}
    missing = {nick for nick in expected_joins if nick not in membership}
    if missing:
        raise AssertionError(f"missing durable IRC membership: {missing!r}")
    topics = [event for event in event_list(events, "irc_event")
              if event["data"]["kind"] == "topic" and
              event["data"]["nick"] == "twoop" and
              event["data"]["text"] == "shared integration topic"]
    if len(topics) != 1 or not topics[0]["data"]["op"]:
        raise AssertionError("operator topic change was not durably attributed")
    turns = event_list(events, "turn_started")
    for marker in ("integration one from oneop", "integration two from twoop"):
        matching = [event for event in turns if marker in event["data"]["text"]]
        if len(matching) != 1 or "operator=true" not in matching[0]["data"]["text"]:
            raise AssertionError(f"operator input was not admitted once: {marker!r}")
    failures = event_list(events, "turn_failed")
    if failures:
        raise AssertionError(f"IRC integration turn failed: {failures[-1]!r}")


def validate_irc_styles(terminal, remote_agent, local_agent=None):
    styled = terminal.capture_styled()
    expected = [
        (r"\x1b\[[0-9;]*35m@twoop", "operator magenta"),
        (rf"\x1b\[[0-9;]*34m{remote_agent}", "remote-agent blue"),
    ]
    if local_agent:
        expected.append(
            (rf"\x1b\[[0-9;]*36m{local_agent}", "local-agent cyan")
        )
    for pattern, role in expected:
        if re.search(pattern, styled) is None:
            raise AssertionError(f"network UI is missing {role} styling")


def run_irc_case(binary, root):
    root.mkdir(mode=0o700, parents=True)
    provider = FakeResponses()
    irc_port = free_loopback_port()
    endpoint = f"127.0.0.1:{irc_port}"
    environment = {"SNAJPAGENT_IRC_UI_KEY": "irc-ui-secret"}
    specs = [
        ("host", "host-model", "hostbot", "hostop",
         ["-d", "-s", endpoint, "-n", "hostbot", "-o", "hostop",
          "-r", "lab", "--color=always"]),
        ("one", "one-model", "onebot", "oneop",
         ["-c", endpoint, "-n", "onebot", "-o", "oneop",
          "--color=always"]),
        ("two", "two-model", "twobot", "twoop",
         ["-c", endpoint, "-n", "twobot", "-o", "twoop",
          "--color=always"]),
    ]
    terminals = {}
    try:
        for name, model, _agent, operator, args in specs:
            case = root / name
            workspace = case / "workspace"
            workspace.mkdir(mode=0o700, parents=True)
            config = case / "config.ini"
            write_irc_config(config, provider.port, model)
            terminal = TmuxTerminal(
                case / "terminal", binary, workspace, case / "state", config,
                100, 24, args=args, environment=environment,
            )
            terminals[name] = terminal
            terminal.wait(f"{operator} ›")

        ordered = [terminals[name] for name in ("host", "one", "two")]
        terminals["host"].wait("@twoop  joined")
        terminals["one"].wait("twoop  joined")
        terminals["one"].wait("set mode · +o twoop")
        terminals["two"].wait("history synchronized")
        wait_irc_idle(ordered)
        for terminal, operator in zip(ordered, ("hostop", "oneop", "twoop")):
            wait_current_prompt(terminal, operator)
        terminals["two"].submit("/names")
        names = terminals["two"].wait(
            f"members[{endpoint}]:", join_wrapped=True
        )
        for nick in ("hostbot", "@hostop", "onebot", "@oneop",
                     "twobot", "@twoop"):
            if nick not in names:
                raise AssertionError(f"/names omitted {nick!r}:\n{names}")
        wait_current_prompt(terminals["two"], "twoop")

        first = "integration one from oneop"
        terminals["one"].submit(first)
        for terminal in ordered:
            terminal.wait(first)
        provider.wait_models(first)
        for name, terminal in terminals.items():
            own = {"host": "hostbot", "one": "onebot", "two": "twobot"}[name]
            for agent in ("hostbot", "onebot", "twobot"):
                if agent != own:
                    terminal.wait(f"{agent} heard one")
        wait_irc_idle(ordered)

        terminals["two"].submit("/verbose 1")
        terminals["two"].wait("verbosity: 1")
        wait_current_prompt(terminals["two"], "twoop")
        second = "integration two from twoop"
        terminals["two"].submit(second)
        for terminal in ordered:
            terminal.wait(second)
        provider.wait_models(second)
        for name, terminal in terminals.items():
            own = {"host": "hostbot", "one": "onebot", "two": "twobot"}[name]
            for agent in ("hostbot", "onebot", "twobot"):
                if agent != own or name == "two":
                    terminal.wait(f"{agent} heard two")
        wait_irc_idle(ordered)

        terminals["two"].submit("/topic shared integration topic")
        for terminal in ordered:
            terminal.wait("@twoop  set topic · shared integration topic")
        wait_irc_idle(ordered)

        for name, _model, own, operator, _args in specs:
            terminal = terminals[name]
            screen = wait_current_prompt(terminal, operator)
            assert_chat_line(screen, "oneop", first, operator=True)
            assert_chat_line(screen, "twoop", second, operator=True)
            for suffix in ("one", "two"):
                for agent in ("hostbot", "onebot", "twobot"):
                    count = screen.count(f"{agent} heard {suffix}")
                    expected = 1 if agent != own or (name == "two" and suffix == "two") else 0
                    if count != expected:
                        raise AssertionError(
                            f"{name} rendered {agent} reply {suffix} {count} times; "
                            f"expected {expected}\n{screen}"
                        )
                    if expected:
                        assert_chat_line(screen, agent, f"{agent} heard {suffix}")
            if screen.count("@twoop  set topic · shared integration topic") != 1:
                raise AssertionError(f"{name} did not render the topic change once")
            validate_irc_events(terminal.dotdir)
        validate_irc_styles(terminals["host"], "onebot")
        validate_irc_styles(terminals["one"], "hostbot")
        validate_irc_styles(terminals["two"], "hostbot", "twobot")

        for marker in (first, second):
            requests = provider.matching_requests(marker)
            counts = {model: sum(request["model"] == model for request in requests)
                      for model in FakeResponses.AGENTS}
            if any(count != 1 for count in counts.values()):
                raise AssertionError(
                    f"operator message was not modeled once per agent: {counts!r}"
                )

        terminals["two"].exit()
        wait_irc_quits(terminals["host"], ("twobot", "twoop"))
        wait_irc_quits(terminals["one"], ("twobot", "twoop"))
        wait_irc_idle([terminals["host"], terminals["one"]])
        terminals["one"].exit()
        wait_irc_quits(terminals["host"], ("onebot", "oneop"))
        wait_irc_idle([terminals["host"]])
        terminals["host"].exit()
        print("tmux_terminal irc: ok")
    finally:
        for name, terminal in terminals.items():
            try:
                screen = terminal.capture()
                (root / name / "screen.txt").write_text(screen, encoding="utf-8")
            except Exception:
                pass
            terminal.close()
            if os.path.lexists(terminal.socket):
                raise AssertionError(f"tmux socket survived cleanup: {terminal.socket}")
        provider.close()


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
    irc = subparsers.add_parser("irc")
    irc.add_argument("binary")
    irc.add_argument("root", type=Path)
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
    elif args.mode == "irc":
        run_irc_case(args.binary, args.root)
    else:
        run_live(args.binary, args.workspace, args.config, args.root)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"tmux_terminal: {exc}", file=sys.stderr)
        raise
