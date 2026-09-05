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
    "alpha beta gamma delta-extraordinary zeta eta theta\n"
    "explicit café € line\n"
    "supercalifragilisticexpialidocious0123456789ABCDEFGHIJ "
    "tail control:\x1b[31m"
)
PACED_TEXT = "Paced tokens form interfragment and finish finalword"
MARKDOWN_TEXT = (
    "# Stream **ready**\n"
    "- split `code` and [docs](https://example.test)\n"
    "```c\nint value = 1;\n```\n\n"
    "First prose line\ncontinued prose\n\n"
    "| Item | State | Count |\n"
    "| :--- | :---: | ---: |\n"
    "| alpha | `ready` | 7 |\n\n"
    "second paragraph\n\n"
    "> final quoted boundary"
)
DEFAULT_IDLE_PROMPT = "default/gpt-5.5-2026-04-23/medium 0%   ›"
DEFAULT_ACCOUNTED_IDLE_PROMPT = "default/gpt-5.5-2026-04-23/medium ?%   ›"
DEFAULT_ACTIVE_PROMPT = "default/gpt-5.5-2026-04-23/medium ?% ◴ »"
DEFAULT_GOAL_ACTIVE_PROMPT = "default/gpt-5.5-2026-04-23/medium 0%◆◴ »"
MACHINE_HOSTNAME = socket.gethostname()
EMPTY_OUTPUT_CORRECTION = (
    "You tried to send an empty assistant message. "
    "Send nonempty text or take another action."
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


class FakeResponses:
    AGENTS = {
        "host-model": "hostbot",
        "one-model": "onebot",
        "two-model": "twobot",
    }

    def __init__(self):
        self.lock = threading.Lock()
        self.requests = []
        self.catalog_requests = []
        self.catalog_failure = None
        self.failure = None
        self.sequence = 0
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_POST(self):
                owner.handle(self)

            def do_GET(self):
                owner.handle_catalog(self)

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
    def has_output_correction(request):
        return any(
            item.get("role") == "developer" and
            item.get("content") == EMPTY_OUTPUT_CORRECTION
            for item in request.get("input", [])
        )

    @staticmethod
    def event(kind, data):
        return (
            f"event: {kind}\n"
            f"data: {json.dumps(data, ensure_ascii=False, separators=(',', ':'))}\n\n"
        )

    def response_body(self, sequence, text, explicit_empty=False):
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
        if not text and not explicit_empty:
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
        ]
        if text:
            events.append(("response.output_text.delta", {
                "type": "response.output_text.delta", "item_id": item_id,
                "output_index": 0, "content_index": 0, "delta": text,
            }))
        events.extend([
            ("response.output_text.done", {
                "type": "response.output_text.done", "item_id": item_id,
                "output_index": 0, "content_index": 0, "text": text,
            }),
            ("response.output_item.done", {
                "type": "response.output_item.done", "output_index": 0,
                "item": done_item,
            }),
            ("response.completed", completed),
        ])
        return body + "".join(self.event(kind, data) for kind, data in events)

    def function_body(self, sequence, call_id, name, arguments):
        response_id = f"resp_irc_ui_{sequence}"
        item_id = f"fc_irc_ui_{sequence}"
        encoded = json.dumps(arguments, ensure_ascii=False, separators=(",", ":"))
        created = {
            "type": "response.created",
            "response": {"id": response_id, "status": "in_progress", "output": []},
        }
        added_item = {
            "id": item_id, "type": "function_call", "status": "in_progress",
            "call_id": call_id, "name": name, "arguments": "",
        }
        done_item = dict(added_item)
        done_item["status"] = "completed"
        done_item["arguments"] = encoded
        completed = {
            "type": "response.completed",
            "response": {
                "id": response_id, "status": "completed",
                "usage": {"input_tokens": 1, "output_tokens": 1,
                          "total_tokens": 2},
                "output": [],
            },
        }
        events = [
            ("response.output_item.added", {
                "type": "response.output_item.added", "output_index": 0,
                "item": added_item,
            }),
            ("response.function_call_arguments.delta", {
                "type": "response.function_call_arguments.delta",
                "item_id": item_id, "output_index": 0, "delta": encoded,
            }),
            ("response.function_call_arguments.done", {
                "type": "response.function_call_arguments.done",
                "item_id": item_id, "output_index": 0, "arguments": encoded,
            }),
            ("response.output_item.done", {
                "type": "response.output_item.done", "output_index": 0,
                "item": done_item,
            }),
            ("response.completed", completed),
        ]
        return (self.event("response.created", created) +
                "".join(self.event(kind, data) for kind, data in events))

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
            corrected = self.has_output_correction(request)
            if model not in self.AGENTS:
                raise AssertionError(f"unexpected fake model {model!r}")
            with self.lock:
                self.sequence += 1
                sequence = self.sequence
                self.requests.append({
                    "corrected": corrected,
                    "model": model,
                    "latest": latest,
                })
            marker = None
            if "integration one from oneop" in latest:
                marker = "one"
            elif "integration two from twoop" in latest:
                marker = "two"
            call_id = f"call_irc_ui_{model}_{marker}"
            sent_text = f"**{self.AGENTS[model]}** heard `{marker}`"
            completed_calls = {
                item.get("call_id")
                for item in request.get("input", [])
                if item.get("type") == "function_call_output"
            }
            call_finished = any(
                item.get("type") == "function_call" and
                item.get("name") == "irc_send" and
                json.loads(item.get("arguments", "{}")).get("text") == sent_text and
                item.get("call_id") in completed_calls
                for item in request.get("input", [])
            )
            if marker and not call_finished:
                body = self.function_body(
                    sequence, call_id, "irc_send", {
                        "notice": False,
                        "text": sent_text,
                    },
                ).encode()
            elif not marker:
                body = self.response_body(
                    sequence, "", explicit_empty=not corrected
                ).encode()
            else:
                text = f"{self.AGENTS[model]} local completion {marker}"
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

    def handle_catalog(self, handler):
        try:
            if handler.headers.get("Authorization") != "Bearer irc-ui-secret":
                raise AssertionError("catalog endpoint received the wrong credential")
            with self.lock:
                self.catalog_requests.append(handler.path)
                reject = self.catalog_failure == handler.path
            if reject:
                status = 400
                response = {"error": {"message": "catalog rejected"}}
            elif handler.path == "/v1/models":
                status = 200
                response = {
                    "data": [{
                        "id": "standard-model",
                        "metadata": {
                            "supported_reasoning_levels": ["medium"],
                            "default_reasoning_level": "medium",
                        },
                    }],
                }
            elif handler.path == (
                    "/backend-api/codex/models?client_version=0.146.0"):
                status = 200
                response = {
                    "models": [
                        {"slug": "hidden", "visibility": "hide",
                         "priority": 0},
                        {"slug": "codex-late", "visibility": "list",
                         "priority": 20,
                         "supported_reasoning_levels": [
                             {"effort": "low"}, {"effort": "ultra"},
                         ],
                         "default_reasoning_level": "low"},
                        {"slug": "codex-fast", "visibility": "list",
                         "priority": 1,
                         "supported_reasoning_levels": [{"effort": "high"}],
                         "default_reasoning_level": "high"},
                    ],
                }
            else:
                raise AssertionError(
                    f"unexpected fake catalog endpoint {handler.path!r}"
                )
            body = json.dumps(response, separators=(",", ":")).encode()
            handler.send_response(status)
            handler.send_header("Content-Type", "application/json")
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

    def catalog_paths(self):
        with self.lock:
            if self.failure:
                raise AssertionError(f"fake endpoint failed: {self.failure}")
            return list(self.catalog_requests)

    def matching_requests(self, marker):
        with self.lock:
            if self.failure:
                raise AssertionError(f"fake endpoint failed: {self.failure}")
            return [request for request in self.requests
                    if marker in request["latest"]]

    def corrected_requests(self):
        with self.lock:
            if self.failure:
                raise AssertionError(f"fake endpoint failed: {self.failure}")
            return [request for request in self.requests
                    if request["corrected"]]

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


def write_config(path, read_agents, pause_ms=300, markdown=None):
    markdown_line = "" if markdown is None else (
        f"markdown = {'true' if markdown else 'false'}\n"
    )
    path.write_text(
        f"[agent]\nread_agents_md = {'true' if read_agents else 'false'}\n"
        f"[ui]\ntyping_pause_ms = {pause_ms}\nverbosity = 0\n"
        'prompt_spinner_provider = " ◴"\n'
        'prompt_spinner_tool = " ⠋"\n'
        f"{markdown_line}",
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


def wrapped_fragment_pattern(fragment):
    if not fragment or "\n" in fragment:
        raise ValueError("wrapped fragment must be nonempty and single-line")
    return re.compile(r"\n?".join(re.escape(char) for char in fragment))


def wait_wrapped_fragment(terminal, fragment, timeout=10.0):
    pattern = wrapped_fragment_pattern(fragment)
    deadline = time.monotonic() + timeout
    screen = ""
    while time.monotonic() < deadline:
        screen = terminal.capture(join_wrapped=True)
        if pattern.search(screen):
            return screen
        if terminal.dead():
            raise AssertionError(
                f"pane exited while waiting for wrapped {fragment!r}:\n{screen}"
            )
        time.sleep(0.02)
    raise AssertionError(f"timeout waiting for wrapped {fragment!r}:\n{screen}")


def assert_wrapped_order(screen, fragments):
    offset = 0
    for fragment in fragments:
        match = wrapped_fragment_pattern(fragment).search(screen, offset)
        if match is None:
            raise AssertionError(
                f"wrapped fragment {fragment!r} is missing or reordered:\n{screen}"
            )
        offset = match.end()


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
        terminal.wait(DEFAULT_IDLE_PROMPT)
        terminal.submit("terminal_status")
        terminal.wait(DEFAULT_ACTIVE_PROMPT, timeout=3.0,
                      join_wrapped=True)
        terminal.wait("status-first-fragment", timeout=3.0)
        time.sleep(0.85)
        middle = terminal.capture(join_wrapped=True)
        if "status-first-fragment" not in middle:
            raise AssertionError(f"prompt redraw erased streamed text:\n{middle}")
        if "working…" in middle:
            raise AssertionError(f"removed activity row reappeared:\n{middle}")
        final = terminal.wait("status-second-\n  fragment", timeout=3.0,
                              join_wrapped=True)
        assert_order(final, ["status-first-fragment", "status-second-",
                             "fragment"])
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
        terminal.wait(DEFAULT_IDLE_PROMPT, join_wrapped=True)
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

        terminal.wait("paced complete", timeout=3.0, join_wrapped=True)
        if time.monotonic() - final_at < 0.8:
            raise AssertionError("the fixture's post-delta pause was lost")

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


def run_markdown_case(binary, root):
    cases = (
        ("default", None, ("--color=always",), True),
        ("disabled", None, ("--no-markdown", "--color=always"), False),
        ("config-disabled", False, ("--color=always",), False),
        ("override", False, ("--markdown", "--color=always"), True),
    )
    for name, configured, args, rendered in cases:
        case = root / f"markdown-{name}"
        workspace = case / "workspace"
        workspace.mkdir(mode=0o700, parents=True)
        config = case / "config.ini"
        write_config(config, False, markdown=configured)
        dotdir = case / "state"
        terminal = TmuxTerminal(
            case / "terminal", binary, workspace, dotdir, config, 64, 16,
            args=args,
        )
        try:
            terminal.wait(DEFAULT_IDLE_PROMPT)
            terminal.submit("terminal_markdown")
            if rendered:
                terminal.wait("Stream rea", timeout=2.0, join_wrapped=True)
                terminal.wait("Stream ready", timeout=2.0, join_wrapped=True)
                terminal.wait("• split code and [docs] <https://example.test>",
                              timeout=2.0, join_wrapped=True)
                held = terminal.wait("│ int value = 1;", timeout=2.0,
                                     join_wrapped=True)
                if any(marker in held for marker in ("**ready**", "```", "](")):
                    raise AssertionError(
                        f"rendered Markdown retained syntax markers:\n{held}"
                    )
                _, pending_events = maybe_events(dotdir)
                if event_list(pending_events, "response_completed"):
                    raise AssertionError(
                        "Markdown did not become visible during the provider pause"
                    )
                terminal.wait("│ alpha │ ready │     7 │", timeout=2.0,
                              join_wrapped=True)
                terminal.wait("• second paragraph", timeout=2.0,
                              join_wrapped=True)
                terminal.wait("│ final quoted boundary", timeout=2.0,
                              join_wrapped=True)
                styled = terminal.capture_styled()
                if re.search(
                        r"(?m)^(?:\x1b\[[0-9;]*m)+Stream", styled) is None:
                    raise AssertionError("tmux Markdown heading style is missing")
            else:
                terminal.wait("# Stream **ready**", timeout=2.0,
                              join_wrapped=True)
                held = terminal.wait(
                    "- split `code` and [docs](https://example.test)",
                    timeout=2.0, join_wrapped=True,
                )
            _, events = wait_for_terminal_event(dotdir, {"turn_completed"}, 5.0)
            completed = event_list(events, "response_completed")
            if (len(completed) != 1 or
                    completed[0]["data"]["items"][0]["text"] != MARKDOWN_TEXT):
                raise AssertionError("Markdown rendering changed durable model text")
            screen = terminal.capture(join_wrapped=True)
            if rendered:
                if "└─" not in screen:
                    raise AssertionError("completed Markdown fence was not closed")
                assert_order(screen, [
                    "Stream ready",
                    "• split code and [docs] <https://example.test>",
                    "┌─ c",
                    "│ int value = 1;",
                    "└─",
                    "• First prose line",
                    "continued prose",
                    "┌───────┬───────┬───────┐",
                    "│ Item  │ State │ Count │",
                    "│ alpha │ ready │     7 │",
                    "└───────┴───────┴───────┘",
                    "• second paragraph",
                    "│ final quoted boundary",
                ])
                raw = terminal.capture()
                if ("• First prose line\ncontinued prose\n\n┌" not in raw or
                        "┘\n\n• second paragraph" not in raw):
                    raise AssertionError(
                        f"prose bullets or paragraph spacing are wrong:\n{raw}"
                    )
                first_model = "Stream ready"
                last_model = "│ final quoted boundary"
            else:
                first_model = "# Stream **ready**"
                last_model = "> final quoted boundary"
            submitted = f"{DEFAULT_IDLE_PROMPT} terminal_markdown"
            if f"{submitted}\n\n{first_model}" not in screen:
                raise AssertionError(
                    f"submitted input and model output lack one empty row:\n{screen}"
                )
            if f"{submitted}\n\n\n{first_model}" in screen:
                raise AssertionError(
                    f"submitted input and model output have an extra empty row:\n{screen}"
                )
            after_model = screen.rsplit(last_model, 1)[1]
            if after_model.startswith("\n\n\n"):
                raise AssertionError(
                    f"model block and next visible block have an extra empty row:\n{screen}"
                )
            if not (after_model.startswith("\n\nworking\u2026") or
                    after_model.startswith(
                        f"\n\n{DEFAULT_ACCOUNTED_IDLE_PROMPT}")):
                raise AssertionError(
                    f"model block and next visible block lack one empty row:\n{screen}"
                )
            if any(len(line) > terminal.cols for line in terminal.capture().splitlines()):
                raise AssertionError("Markdown rendering exceeded the tmux width")
            terminal.exit()
        finally:
            try:
                screen = terminal.last_screen or terminal.capture()
                (case / "screen.txt").write_text(screen, encoding="utf-8")
            finally:
                close_fixture_terminal(terminal)


def run_narrow_markdown_table_case(binary, root):
    case = root / "markdown-narrow-table"
    workspace = case / "workspace"
    workspace.mkdir(mode=0o700, parents=True)
    config = case / "config.ini"
    write_config(config, False, markdown=True)
    dotdir = case / "state"
    terminal = TmuxTerminal(
        case / "terminal", binary, workspace, dotdir, config, 22, 24,
        args=("--color=never",),
    )
    try:
        terminal.wait(DEFAULT_IDLE_PROMPT, join_wrapped=True)
        terminal.submit("terminal_markdown")
        terminal.wait("┌─ table", timeout=3.0, join_wrapped=True)
        terminal.wait("│ Item: alpha", timeout=3.0, join_wrapped=True)
        terminal.wait("│ State: ready", timeout=3.0, join_wrapped=True)
        terminal.wait("│ Count: 7", timeout=3.0, join_wrapped=True)
        _, events = wait_for_terminal_event(dotdir, {"turn_completed"}, 5.0)
        completed = event_list(events, "response_completed")
        if (len(completed) != 1 or
                completed[0]["data"]["items"][0]["text"] != MARKDOWN_TEXT):
            raise AssertionError("narrow table rendering changed durable text")
        screen = terminal.capture(join_wrapped=True)
        if "| :--- | :---: | ---: |" in screen:
            raise AssertionError(f"narrow table retained delimiter syntax:\n{screen}")
        if any(len(line) > terminal.cols for line in terminal.capture().splitlines()):
            raise AssertionError("narrow Markdown table exceeded terminal width")
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
        terminal.wait(DEFAULT_IDLE_PROMPT, join_wrapped=True)
        terminal.submit("terminal_render")
        terminal.wait("alpha beta gamma delta-")
        terminal.send_text("draft")
        first = wait_wrapped_fragment(
            terminal, f"{DEFAULT_ACTIVE_PROMPT} draft"
        )
        assert_wrapped_order(first, [
            "alpha beta gamma delta-", "extraordinary",
            f"{DEFAULT_ACTIVE_PROMPT} draft",
        ])
        if re.search(r"(?m)^• alpha beta gamma", first) is None:
            raise AssertionError(f"model prose did not begin with a bullet:\n{first}")
        if "• alpha beta gamma delta-\n  extraordinary" not in first:
            raise AssertionError(f"hyphen wrapped on its left side:\n{first}")

        time.sleep(0.1)
        pause_started = time.monotonic()
        terminal.send_text(" plus")
        wait_wrapped_fragment(terminal, f"{DEFAULT_ACTIVE_PROMPT} draft plus")
        time.sleep(1.1)
        paused = terminal.capture(join_wrapped=True)
        if "explicit café € line" in paused:
            raise AssertionError(
                f"model output ignored the configured typing pause:\n{paused}"
            )
        second = terminal.wait("explicit café € line", timeout=4.0,
                               join_wrapped=True)
        if time.monotonic() - pause_started < 1.2:
            raise AssertionError("model output resumed before the typing pause")
        assert_wrapped_order(second, [
            f"{DEFAULT_ACTIVE_PROMPT} draft plus", "explicit café € line",
        ])
        prompt_pattern = wrapped_fragment_pattern(
            f"{DEFAULT_ACTIVE_PROMPT} draft plus"
        ).pattern
        if re.search(prompt_pattern + r"\n\nzeta eta theta", second):
            raise AssertionError(f"output resumed with a spurious blank line:\n{second}")
        if re.search(prompt_pattern + r"\nzeta eta theta", second) is None:
            raise AssertionError(f"output did not resume directly below the draft:\n{second}")
        if re.search(r"(?m)^zeta eta theta$", second) is None:
            raise AssertionError(f"wrapped prose gained a hanging indent:\n{second}")

        repeat_pause_started = time.monotonic()
        terminal.send_text(" again with long resize text")
        wait_wrapped_fragment(
            terminal, f"{DEFAULT_ACTIVE_PROMPT} draft plus again"
        )
        exact_margin = (
            f"{DEFAULT_ACTIVE_PROMPT} draft plus again with long resize text"
        )
        terminal.resize(len(exact_margin), 18)
        resized = terminal.wait(exact_margin, join_wrapped=True)
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
        assert_wrapped_order(
            joined,
            [
                "alpha beta gamma delta-",
                "extraordinary",
                f"{DEFAULT_ACTIVE_PROMPT} draft plus",
                "explicit café € line",
                exact_margin,
                "supercalifragilisticexpialidocious0123456789ABCDEFGHIJ",
                "control:",
                "\\x1B[31m",
            ],
        )
        if "alpha beta gamma delta-extraordinary" in final:
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
        steering_screen = terminal.wait(
            f"{DEFAULT_ACTIVE_PROMPT} change course", join_wrapped=True
        )
        assert_order(steering_screen, ["working slowly",
                                      f"{DEFAULT_ACTIVE_PROMPT} change course"])
        terminal.send_key("Enter")
        steered_screen = terminal.wait("steered: change course")
        submitted_steer = f"{DEFAULT_ACTIVE_PROMPT} change course"
        if f"{submitted_steer}\n\n• steered: change course" not in steered_screen:
            raise AssertionError(
                f"submitted steer and model output lack one empty row:\n{steered_screen}"
            )
        if f"{submitted_steer}\n\n\n• steered: change course" in steered_screen:
            raise AssertionError(
                f"submitted steer and model output have an extra empty row:\n{steered_screen}"
            )
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
        terminal.wait(DEFAULT_IDLE_PROMPT)
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
        terminal.wait("edit 1 ?% ◴ › second")
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
        terminal.wait(DEFAULT_ACCOUNTED_IDLE_PROMPT)
        terminal.submit("/queue 1 edit")
        terminal.wait("edit 1 ?%   › second active")
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
                "edit 1 ?% ◴ › second active",
                "next › fifth",
                "edit 1 ?%   › second active idle",
                "future-turn queue is empty",
            ],
        )
        terminal.exit()

        _, events = read_events(dotdir)
        queued = event_list(events, "future_turn_queued")
        edited = event_list(events, "future_turn_edited")
        cancelled = event_list(events, "future_turn_cancelled")
        queue_turn = event_list(events, "turn_started")[0]["data"]["turn_id"]
        if [event for event in event_list(events, "steering_added")
                if event["data"]["turn_id"] == queue_turn]:
            raise AssertionError("Tab queueing was admitted as steering")
        if [event for event in event_list(events, "response_interrupted")
                if event["data"]["turn_id"] == queue_turn and
                event["data"]["origin"] == "steering"]:
            raise AssertionError("Tab queueing interrupted the active response")
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


def run_tool_case(binary, root):
    case = root / "tools"
    workspace = case / "workspace"
    workspace.mkdir(mode=0o700, parents=True)
    config = case / "config.ini"
    write_config(config, False)
    dotdir = case / "state"
    terminal = TmuxTerminal(
        case / "terminal", binary, workspace, dotdir, config, 52, 18
    )
    try:
        terminal.wait(DEFAULT_IDLE_PROMPT)
        terminal.submit("/verbose 1")
        terminal.wait("verbosity: 1")
        terminal.submit("text_tool")
        screen = terminal.wait("fixture command succeeded", join_wrapped=True)
        terminal.wait("done", join_wrapped=True)
        assert_order(screen, [
            "→ exec  timeout=1000ms  'fixture ok'",
            'arguments: {"command":"fixture ok"',
            "fixture command succeeded",
        ])
        _, events = wait_for_terminal_event(dotdir, {"turn_completed"}, 5.0)
        finished = event_list(events, "tool_finished")
        if (len(finished) != 1 or
                finished[0]["data"]["result"]["model_text"] !=
                "fixture command succeeded"):
            raise AssertionError("tool display changed the model-visible result")
        terminal.exit()
    finally:
        try:
            screen = terminal.last_screen or terminal.capture()
            (case / "screen.txt").write_text(screen, encoding="utf-8")
        finally:
            close_fixture_terminal(terminal)


def wait_idle_prompt_at_bottom(terminal, prompt, timeout=5.0):
    deadline = time.monotonic() + timeout
    screen = ""
    while time.monotonic() < deadline:
        screen = terminal.capture()
        if screen.rstrip().endswith(prompt.rstrip()):
            return screen
        if terminal.dead():
            raise AssertionError(
                f"pane exited while waiting for the idle prompt:\n{screen}"
            )
        time.sleep(0.02)
    raise AssertionError(f"idle prompt is not at the bottom:\n{screen}")


def run_lifecycle_case(binary, root):
    case = root / "lifecycle"
    workspace = case / "workspace"
    workspace.mkdir(mode=0o700, parents=True)
    config = case / "config.ini"
    write_config(config, False)
    dotdir = case / "state"
    terminal = TmuxTerminal(
        case / "terminal", binary, workspace, dotdir, config, 60, 18,
        args=("--color=always",),
    )
    try:
        terminal.wait(DEFAULT_IDLE_PROMPT)
        terminal.submit("/goal slow goal")
        terminal.wait("• Goal set")
        terminal.wait("working on goal")
        terminal.send_text("/goal cancel")
        terminal.wait(f"{DEFAULT_GOAL_ACTIVE_PROMPT} /goal cancel")
        terminal.send_key("Enter")
        terminal.wait("• Goal cleared")
        terminal.wait("goal checkpoint")
        wait_for_terminal_event(dotdir, {"turn_completed"}, 5.0)
        wait_idle_prompt_at_bottom(terminal, DEFAULT_ACCOUNTED_IDLE_PROMPT)

        terminal.submit("/compact")
        terminal.wait("• Compacted")
        wait_idle_prompt_at_bottom(terminal, DEFAULT_IDLE_PROMPT)
        screen = terminal.capture(join_wrapped=True)
        assert_order(screen, ["• Goal set", "• Goal cleared", "• Compacted"])
        for notice in ("• Goal set", "• Goal cleared", "• Compacted"):
            if screen.count(notice) != 1:
                raise AssertionError(
                    f"lifecycle notice was missing or duplicated: {notice!r}\n"
                    f"{screen}"
                )
        for obsolete in (
            "goal started", "goal cancelled",
            "compaction completed and installed for future turns",
        ):
            if obsolete in screen:
                raise AssertionError(
                    f"obsolete lifecycle detail remained visible: {obsolete!r}\n"
                    f"{screen}"
                )
        styled = terminal.capture_styled()
        for notice in ("• Goal set", "• Goal cleared", "• Compacted"):
            pattern = (r"\x1b\[[0-9;]*32m(?:\x1b\[[0-9;]*m)*" +
                       re.escape(notice))
            if re.search(pattern, styled) is None:
                raise AssertionError(
                    f"lifecycle notice lacks its green role: {notice!r}"
                )
        _, events = read_events(dotdir)
        if len(event_list(events, "goal_started")) != 1 or \
                len(event_list(events, "goal_cancelled")) != 1 or \
                len(event_list(events, "compaction_completed")) != 1:
            raise AssertionError("lifecycle presentation changed durable events")
        terminal.exit()
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
    run_markdown_case(binary, root)
    run_narrow_markdown_table_case(binary, root)
    run_render_case(binary, root)
    run_queue_case(binary, root)
    run_tool_case(binary, root)
    run_lifecycle_case(binary, root)
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


def write_catalog_config(path, provider_port):
    path.write_text(
        "[agent]\nmodel = uncached-start\nreasoning_effort = low\n"
        "read_agents_md = false\n"
        f"[provider ordinary]\nbase_url = http://127.0.0.1:{provider_port}\n"
        "api_key_env = SNAJPAGENT_IRC_UI_KEY\n"
        "connect_timeout_ms = 1000\nidle_timeout_ms = 3000\n"
        "request_timeout_ms = 5000\n"
        f"[provider codex]\nbase_url = http://127.0.0.1:{provider_port}"
        "/backend-api/codex/\n"
        "api_key_env = SNAJPAGENT_IRC_UI_KEY\n"
        "connect_timeout_ms = 1000\nidle_timeout_ms = 3000\n"
        "request_timeout_ms = 5000\n"
        "[ui]\nverbosity = 0\ncolor = never\n",
        encoding="utf-8",
    )


def run_model_catalog_case(binary, root, provider, environment):
    case = root / "model-catalog"
    workspace = case / "workspace"
    workspace.mkdir(mode=0o700, parents=True)
    config = case / "config.ini"
    write_catalog_config(config, provider.port)
    terminal = TmuxTerminal(
        case / "terminal", binary, workspace, case / "state", config,
        100, 24, environment=environment,
    )
    try:
        terminal.wait("ordinary/uncached-start/low 0%   ›")
        before = provider.catalog_paths()
        terminal.submit("/model cache")
        screen = terminal.wait("4. codex / codex-late / ultra",
                               join_wrapped=True)
        for expected in (
                "1. ordinary / standard-model / medium",
                "2. codex / codex-fast / high",
                "3. codex / codex-late / low"):
            if expected not in screen:
                raise AssertionError(
                    f"model catalog UI omitted {expected!r}:\n{screen}"
                )
        cache_path = terminal.dotdir / "models.json"
        cache = json.loads(cache_path.read_text(encoding="utf-8"))
        if cache.get("schema_version") != 1:
            raise AssertionError("model cache omitted its schema version")
        if [entry["name"] for entry in cache["providers"]] != [
                "ordinary", "codex"]:
            raise AssertionError("mixed provider cache changed provider order")
        first_model = cache["providers"][0]["models"][0]
        if (first_model.get("count_capability") != "unknown" or
                first_model.get("observed_model_input_bytes") != 0 or
                first_model.get("observed_input_tokens") != 0 or
                first_model.get("observed_hard_input_tokens") != 0):
            raise AssertionError("fresh model cache has invalid accounting state")
        if [model["id"] for model in cache["providers"][1]["models"]] != [
                "codex-fast", "codex-late"]:
            raise AssertionError("Codex cache retained hidden or unsorted models")
        expected_paths = [
            "/v1/models",
            "/backend-api/codex/models?client_version=0.146.0",
        ]
        if provider.catalog_paths()[len(before):] != expected_paths:
            raise AssertionError("mixed refresh used unexpected catalog endpoints")

        paths_before_list = provider.catalog_paths()
        terminal.submit("/model list")
        terminal.wait("4. codex / codex-late / ultra", join_wrapped=True)
        wait_current_prompt(terminal, None, timeout=5.0)
        if provider.catalog_paths() != paths_before_list:
            raise AssertionError("offline model list contacted a provider")

        old_cache = cache_path.read_bytes()
        old_inode = cache_path.stat().st_ino
        with provider.lock:
            provider.catalog_failure = expected_paths[1]
        failure_start = len(provider.catalog_paths())
        terminal.submit("/model cache")
        terminal.wait("cannot refresh provider codex:", join_wrapped=True)
        wait_current_prompt(terminal, None, timeout=5.0)
        with provider.lock:
            provider.catalog_failure = None
        if provider.catalog_paths()[failure_start:] != expected_paths:
            raise AssertionError("failed Codex refresh fell back to another endpoint")
        if (cache_path.read_bytes() != old_cache or
                cache_path.stat().st_ino != old_inode):
            raise AssertionError("failed mixed refresh replaced the complete cache")
        terminal.exit()
    finally:
        try:
            screen = terminal.last_screen or terminal.capture()
            (case / "screen.txt").write_text(screen, encoding="utf-8")
        finally:
            terminal.close()
            if os.path.lexists(terminal.socket):
                raise AssertionError(
                    f"tmux socket survived cleanup: {terminal.socket}"
                )


def wait_current_prompt(terminal, operator, timeout=10.0):
    deadline = time.monotonic() + timeout
    expected = (f"{operator}@{MACHINE_HOSTNAME}   :" if operator else
                "ordinary/uncached-start/low 0%   ›")
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
    bullet = "" if operator else "• "
    pattern = (
        rf"(?m)^\d{{2}}:\d{{2}}:\d{{2}} {re.escape(marker + nick)} › "
        rf"{re.escape(bullet + text)}$"
    )
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
            (nick, f"**{nick}** heard `{suffix}`", False)
            for nick in ("hostbot", "onebot", "twobot")
        )
    messages = [event for event in event_list(events, "irc_event")
                if event["data"]["kind"] == "message"]
    if any("local completion" in event["data"]["text"] for event in messages):
        raise AssertionError("local assistant completion leaked into IRC events")
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
    explicit_calls = [
        item
        for response in event_list(events, "response_completed")
        for item in response["data"]["items"]
        if item.get("name") == "irc_send"
    ]
    if len(explicit_calls) != 2:
        raise AssertionError(
            f"expected two explicit IRC sends, got {len(explicit_calls)}"
        )
    if event_list(events, "response_failed"):
        raise AssertionError("IRC output correction became a failed response")
    corrections = event_list(events, "response_output_correction")
    if not corrections or any(
            event["data"]["text"] != EMPTY_OUTPUT_CORRECTION
            for event in corrections):
        raise AssertionError("IRC empty output was not corrected exactly")
    starts = event_list(events, "response_started")
    for correction in corrections:
        correction_id = correction["data"]["correction_id"]
        if sum(correction_id in event["data"]["steering_ids"]
               for event in starts) != 1:
            raise AssertionError(
                "IRC output correction was not consumed by one next response"
            )
    quiet = [event for event in event_list(events, "turn_completed_silent")
             if event["data"]["reason"] == "room_update_quiet"]
    if not quiet:
        raise AssertionError("IRC peer chatter did not complete silently")
    responses = {event["data"]["response_id"]: event
                 for event in event_list(events, "response_completed")}
    for event in quiet:
        response = responses.get(event["data"]["response_id"])
        if response is None or response["data"]["items"]:
            raise AssertionError(
                "quiet IRC turn did not retain an empty completed response"
            )


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
         ["-s", endpoint, "-n", "hostbot", "-o", "hostop",
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
        run_model_catalog_case(binary, root, provider, environment)
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
            terminal.wait(f"{operator}@{MACHINE_HOSTNAME}   :")

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
            if "**hostbot**" in screen or "`one`" in screen or "`two`" in screen:
                raise AssertionError(f"{name} retained model Markdown markers:\n{screen}")
            if EMPTY_OUTPUT_CORRECTION in screen:
                raise AssertionError(
                    f"{name} rendered a model-facing output correction:\n{screen}"
                )
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
            if any(count != 2 for count in counts.values()):
                raise AssertionError(
                    f"operator message did not run send and final cycles: {counts!r}"
                )
        corrected_models = {
            request["model"] for request in provider.corrected_requests()
        }
        if corrected_models != set(FakeResponses.AGENTS):
            raise AssertionError(
                "output correction did not reach every model as developer input: "
                f"{corrected_models!r}"
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
        terminal.wait(DEFAULT_IDLE_PROMPT)
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

        terminal.wait(DEFAULT_ACCOUNTED_IDLE_PROMPT, timeout=10.0)
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
