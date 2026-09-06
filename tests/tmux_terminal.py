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
DEFAULT_IDLE_PROMPT = "    0% openai/gpt-5.5-2026-04-23/medium ›"
DEFAULT_ACCOUNTED_IDLE_PROMPT = "    ?% openai/gpt-5.5-2026-04-23/medium ›"
DEFAULT_ACTIVE_PROMPT = " ◴  ?% openai/gpt-5.5-2026-04-23/medium »"
DEFAULT_GOAL_ACTIVE_PROMPT = "⚑◴  ?% openai/gpt-5.5-2026-04-23/medium »"
MACHINE_HOSTNAME = socket.gethostname()
IRC_SECOND_MESSAGE = "integration two from twoop " + "long chat text " * 80 + "end"
EMPTY_OUTPUT_CORRECTION = (
    "You tried to send an empty assistant message. "
    "Send nonempty text or take another action."
)


def read_events(dotdir):
    paths = sorted((dotdir / "sessions").glob("*/events.jsonl"))
    if len(paths) != 1:
        raise AssertionError(f"expected one session log, got {paths!r}")
    events = []
    # A live writer can expose part of its final JSON/UTF-8 record. Frame
    # complete records before decoding, and still reject malformed full lines.
    for line in paths[0].read_bytes().split(b"\n")[:-1]:
        if line.strip():
            events.append(json.loads(line))
    return paths[0], events


def maybe_events(dotdir):
    paths = sorted((dotdir / "sessions").glob("*/events.jsonl"))
    if len(paths) != 1:
        return None, []
    try:
        return read_events(dotdir)
    except OSError:
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
        self.exit_started = threading.Event()
        self.exit_release = threading.Event()
        self.tool_workspace = None
        self.runtime_handler = None
        self.runtime_count_handler = None
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
                content = item["content"]
                ids = re.findall(r"\[IRC update id=([^ ]+)", content)
                if ids:
                    # Resolve the production scheduler's references to its
                    # separately retained room-event payloads in this request.
                    return "\n".join(str(event.get("content", ""))
                        for event in request.get("input", [])
                        if any(f" id={identity}]" in str(event.get("content", "")) for identity in ids))
                return content
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
        return self.functions_body(sequence, [(call_id, name, arguments)])

    def functions_body(self, sequence, calls):
        response_id = f"resp_irc_ui_{sequence}"
        created = {"type": "response.created", "response": {
            "id": response_id, "status": "in_progress", "output": []}}
        events = [self.event("response.created", created)]
        for index, (call_id, name, arguments) in enumerate(calls):
            item_id = f"fc_irc_ui_{sequence}_{index}"
            encoded = json.dumps(arguments, ensure_ascii=False, separators=(",", ":"))
            item = {"id": item_id, "type": "function_call", "status": "in_progress",
                    "call_id": call_id, "name": name, "arguments": ""}
            events.append(self.event("response.output_item.added", {
                "type": "response.output_item.added", "output_index": index, "item": item}))
            events.append(self.event("response.function_call_arguments.delta", {
                "type": "response.function_call_arguments.delta",
                "item_id": item_id, "output_index": index, "delta": encoded}))
            events.append(self.event("response.function_call_arguments.done", {
                "type": "response.function_call_arguments.done",
                "item_id": item_id, "output_index": index, "arguments": encoded}))
            item = dict(item, status="completed", arguments=encoded)
            events.append(self.event("response.output_item.done", {
                "type": "response.output_item.done", "output_index": index, "item": item}))
        events.append(self.event("response.completed", {
            "type": "response.completed", "response": {
                "id": response_id, "status": "completed", "output": [],
                "usage": {"input_tokens": 1, "output_tokens": 1, "total_tokens": 2}}}))
        return "".join(events)

    def handle(self, handler):
        try:
            counting = handler.path == "/v1/responses/input_tokens"
            if handler.path != "/v1/responses" and not counting:
                raise AssertionError(f"unexpected fake endpoint {handler.path!r}")
            if handler.headers.get("Authorization") != "Bearer irc-ui-secret":
                raise AssertionError("fake endpoint received the wrong credential")
            length = int(handler.headers.get("Content-Length", "-1"))
            if length < 0 or length > 32 * 1024 * 1024:
                raise AssertionError("fake endpoint request length is invalid")
            request = json.loads(handler.rfile.read(length))
            if counting:
                assert self.runtime_count_handler is not None
                self.runtime_count_handler(handler, request)
                return
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
                    "body": request,
                })
            if self.runtime_handler is not None:
                self.runtime_handler(handler, request, sequence)
                return
            if latest.startswith("exit-"):
                self.handle_exit(handler, request, sequence, latest)
                return
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
            if latest.startswith("multi-tools"):
                body = self.multi_tool_body(request, sequence, latest).encode()
            elif latest.startswith("destination-model "):
                _, destination, marker = latest.split()
                call_id = f"call_destination_{marker}"
                finished = any(item.get("type") == "function_call" and
                               item.get("name") == "irc_send" and
                               json.loads(item.get("arguments", "{}")).get("text") == marker and
                               item.get("call_id") in completed_calls
                               for item in request.get("input", []))
                if finished:
                    body = self.response_body(sequence, "destination model done").encode()
                else:
                    body = self.function_body(sequence, call_id, "irc_send", {
                        "destination": None if destination == "null" else destination,
                        "notice": False, "text": marker,
                    }).encode()
            elif latest.startswith("tool-cap "):
                body = self.output_cap_body(request, sequence, latest).encode()
            elif marker and not call_finished:
                body = self.function_body(
                    sequence, call_id, "irc_send", {
                        "notice": False,
                        "destination": None,
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

    def handle_exit(self, handler, request, sequence, mode):
        handler.close_connection = True
        if mode in ("exit-tool", "exit-managed") and not any(
                item.get("type") == "function_call_output"
                for item in request.get("input", [])):
            body = self.function_body(sequence, "call_exit", "exec_command", {
                "command": 'printf "%s" "$$" > command.pid; exec sleep 30',
                "workdir": str(self.tool_workspace), "stdin": None,
                "pty": False, "timeout_ms": None, "max_output_tokens": None,
                "yield_ms": 1 if mode == "exit-managed" else 0,
            }).encode()
        else:
            if mode == "exit-stream":
                body = self.response_body(sequence, "exit stream prefix\n")
                prefix = body.split("event: response.output_text.done", 1)[0]
                handler.send_response(200)
                handler.send_header("Content-Type", "text/event-stream")
                handler.send_header("Connection", "close")
                handler.end_headers()
                handler.wfile.write(prefix.encode())
                handler.wfile.flush()
            # Hold before headers or mid-stream until the application exits.
            self.exit_started.set()
            if not self.exit_release.wait(10.0):
                raise AssertionError("Ctrl-D did not release the held request")
            return
        handler.send_response(200)
        handler.send_header("Content-Type", "text/event-stream")
        handler.send_header("Content-Length", str(len(body)))
        handler.end_headers()
        handler.wfile.write(body)

    def output_cap_body(self, request, sequence, prompt):
        _, ceiling, selected = prompt.split()
        ceiling, selected = int(ceiling), json.loads(selected)
        effective = min(ceiling, selected if selected is not None else ceiling)
        for tool in request["tools"]:
            if tool.get("name") in ("exec_command", "write_stdin"):
                assert tool["parameters"]["properties"]["max_output_tokens"][
                    "maximum"] == ceiling
        outputs = [item["output"] for item in request["input"]
                   if item.get("type") == "function_call_output"]
        if outputs:
            assert len(outputs) == 1 and len(outputs[0].encode()) <= effective
            assert f"max_output_tokens={effective}" in outputs[0]
            return self.response_body(sequence, "tool cap confirmed")
        return self.function_body(sequence, "call_cap", "exec_command", {
            "command": "printf '%08000d' 0", "workdir": str(self.tool_workspace),
            "stdin": None, "pty": False, "timeout_ms": None, "yield_ms": 0,
            "max_output_tokens": selected,
        })

    def multi_tool_body(self, request, sequence, prompt):
        mode = prompt.split()[-1]
        assert request["parallel_tool_calls"] is (not mode.startswith("single-"))
        calls = [item for item in request["input"] if item.get("type") == "function_call"]
        outputs = {item["call_id"]: item["output"] for item in request["input"]
                   if item.get("type") == "function_call_output"}
        jobs = []
        for item in request["input"]:
            text = item.get("content", "")
            if isinstance(text, str) and "The preceding JSON describes unsettled commands" in text:
                jobs = json.loads(text.split("\n", 1)[0])
        if not calls:
            # A cannot finish until B launches: this detects actual overlap,
            # not merely several call items or a fast serial timing result.
            commands = (["sleep 0.1; printf first", "printf second"] if mode in ("serial", "single-serial") else [
                "while test ! -f peer-ready; do sleep 0.02; done; sleep 0.2; printf first",
                "touch peer-ready; printf second"])
            if mode == "failure":
                commands[1] = "touch peer-ready; printf failed-peer; exit 7"
            if mode == "full-output":
                commands[0] += "; printf '%080000d' 0; printf full-output-tail"
            if mode in ("steer", "cancel"):
                commands = ["echo $$ > a.pid; sleep 10; printf first",
                            "echo $$ > b.pid; sleep 10; printf second",
                            "touch must-not-run"]
            batch = [(f"call_multi_{i}", "exec_command", {
                "command": command, "workdir": str(self.tool_workspace),
                "stdin": None, "pty": False,
                "timeout_ms": None if mode in ("steer", "cancel") else 3000,
                "yield_ms": 1 if mode in ("yield", "single-request") else 0,
                "max_output_tokens": None,
            }) for i, command in enumerate(commands)]
            return self.functions_body(sequence, batch)
        assert all(call["call_id"] in outputs for call in calls)
        if jobs:
            assert {"exec_command", "apply_patch", "write_stdin"}.issubset(
                {tool.get("name") for tool in request["tools"]})
            return self.functions_body(sequence, [(f"poll_{sequence}_{i}", "write_stdin", {
                "handle": job["handle"], "data": "", "eof": False,
                "terminate": mode == "steer", "yield_ms": 1000, "max_output_tokens": None,
            }) for i, job in enumerate(jobs)])
        if mode == "steer":
            assert len(outputs) == 5
            assert "superseded_by_steering" in "".join(outputs.values())
            return self.response_body(sequence, "multi tools confirmed")
        assert "first" in "".join(outputs.values())
        assert ("failed-peer" if mode == "failure" else "second") in "".join(outputs.values())
        return self.response_body(sequence, "multi tools confirmed")

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
                        {"slug": "hidden", "visibility": "hide", "priority": 0},
                        {"slug": "missing-visibility", "priority": 0},
                        {"slug": "none", "visibility": "none", "priority": 0},
                        {"slug": "future", "visibility": "future", "priority": 0},
                        {"slug": "codex-late", "visibility": "list",
                         "priority": 20, "context_window": 272000,
                         "max_context_window": 872000,
                         "auto_compact_token_limit": None,
                         "supported_reasoning_levels": [
                             {"effort": "low"}, {"effort": "ultra"}, {"effort": "low"},
                         ],
                         "default_reasoning_level": "low"},
                        {"slug": "codex-fast", "visibility": "list",
                         "priority": 1,
                         "supported_reasoning_levels": [{"effort": "medium"}],
                         "default_reasoning_level": "medium"},
                        {"slug": "codex-tied", "visibility": "list",
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
        f"[provider openai]\n[agent]\nread_agents_md = {'true' if read_agents else 'false'}\n"
        f"[ui]\ntyping_pause_ms = {pause_ms}\n"
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
        terminal.wait(DEFAULT_IDLE_PROMPT, join_wrapped=True)
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


def run_paced_decode_case(binary, root, width=28, unicode=False, resize=None):
    case = root / f"decode-{width}-{unicode}-{resize}"
    workspace = case / "workspace"
    workspace.mkdir(mode=0o700, parents=True)
    config = case / "config.ini"
    write_config(config, False)
    dotdir = case / "state"
    terminal = TmuxTerminal(
        case / "terminal", binary, workspace, dotdir, config, width, 14
    )
    try:
        terminal.wait(DEFAULT_IDLE_PROMPT, join_wrapped=True)
        terminal.submit("terminal_paced_unicode" if unicode else "terminal_paced_decode")
        prefix = "Paced tokens form inter" + ("🌙" if unicode else "")
        split = prefix + ("́" if unicode else "") + "fragment"
        expected = split + " and finish finalword"
        wait_normalized(terminal, "Paced")
        wait_normalized(terminal, "Paced tokens")
        _, split_prefix_at = wait_normalized(
            terminal, prefix
        )
        if resize:
            time.sleep(0.05)
            terminal.resize(resize, 14)
        _, split_word_at = wait_normalized(
            terminal, split
        )
        if split_word_at - split_prefix_at < 0.03:
            raise AssertionError(
                "a complete split-word prefix was withheld until its suffix"
            )
        wait_normalized(terminal, "and finish")
        final_screen, final_at = wait_normalized(
            terminal, expected, timeout=0.35
        )
        if "working…" in final_screen:
            raise AssertionError(
                "activity appeared while the paced public item was open"
            )

        time.sleep(0.45)
        held_screen = terminal.capture(join_wrapped=True)
        if expected not in normalize_space(held_screen):
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
        if public != [expected, "paced complete"]:
            raise AssertionError(
                f"paced rendered text differs from durable output: {public!r}"
            )
        final = normalize_space(terminal.capture(join_wrapped=True))
        if final.count(expected) != 1:
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
        expected = str(agents)
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
        for count, text in enumerate(("first", "second", "third", "fourth"), 1):
            terminal.send_text(text)
            terminal.send_key("Tab")
            terminal.wait(f"next › {text}")
            wait_idle_prompt_at_bottom(terminal, f"/medium ({count}) »")

        terminal.submit("/q")
        wait_queue_listing(terminal, ("first", "second", "third", "fourth"))

        terminal.submit("/q p")
        wait_event_count(dotdir, "future_turn_cancelled", 1)
        terminal.wait("1 future turn cancelled")
        wait_idle_prompt_at_bottom(terminal, "/medium (3) »")
        terminal.submit("/queue pop")
        wait_event_count(dotdir, "future_turn_cancelled", 2)
        wait_idle_prompt_at_bottom(terminal, "/medium (2) »")
        terminal.submit("/queue 1 delete")
        wait_event_count(dotdir, "future_turn_cancelled", 3)
        wait_idle_prompt_at_bottom(terminal, "/medium (1) »")
        terminal.submit("/q 1e")
        terminal.wait(" ◴  ?% edit 1 › second")
        terminal.send_text(" active")
        terminal.send_key("Enter")
        wait_event_count(dotdir, "future_turn_edited", 1)
        wait_idle_prompt_at_bottom(terminal, "/medium (1) »")

        terminal.send_text("fifth")
        terminal.send_key("Tab")
        terminal.wait("next › fifth")
        terminal.submit("/queue")
        wait_queue_listing(terminal, ("second active", "fifth"))

        terminal.send_key("C-c")
        terminal.wait("turn interrupted")
        wait_idle_prompt_at_bottom(terminal, "/medium (2) ›")
        terminal.submit("/queue 1 edit")
        terminal.wait("    ?% edit 1 › second active")
        terminal.send_text(" idle")
        terminal.send_key("Enter")
        wait_event_count(dotdir, "future_turn_edited", 2)
        terminal.submit("/q c")
        wait_event_count(dotdir, "future_turn_cancelled", 4)
        terminal.wait("2 future turns cancelled")
        wait_idle_prompt_at_bottom(terminal, DEFAULT_ACCOUNTED_IDLE_PROMPT)
        terminal.submit("/q")
        empty = terminal.wait("future-turn queue is empty")
        assert_order(
            empty,
            [
                "next › first",
                "next › second",
                "next › third",
                "next › fourth",
                " ◴  ?% edit 1 › second active",
                "next › fifth",
                "    ?% edit 1 › second active idle",
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
        terminal.submit("/verbose 3")
        terminal.wait("verbosity: 3")
        terminal.submit("text_tool")
        screen = terminal.wait("fixture command succeeded", join_wrapped=True)
        terminal.wait("done", join_wrapped=True)
        assert_order(screen, [
            "→ exec_command",
            'arguments:',
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


def run_retained_composer_case(binary, root):
    case = root / "retained"
    workspace = case / "workspace"
    workspace.mkdir(mode=0o700, parents=True)
    config = case / "config.ini"
    write_config(config, False)
    with config.open("a", encoding="utf-8") as output:
        output.write("prompt = {chat:p>}{rollout-idle:p>}{rollout-active:p>}\n")
    terminal = TmuxTerminal(
        case / "terminal", binary, workspace, case / "state", config, 24, 18,
    )
    try:
        terminal.wait("p>")
        draft = "first-row-unchanged second-row-unchanged third-row"
        terminal.send_text(draft)
        terminal.wait("p> " + draft, join_wrapped=True)
        terminal.send_key("Home")
        terminal.send_text("X")
        terminal.wait("p> X" + draft, join_wrapped=True)
        terminal.send_key("DC")
        terminal.wait("p> X" + draft[1:], join_wrapped=True)
        terminal.send_key("C-u")
        terminal.send_text("short")
        screen = terminal.wait("p> short")
        if "row-unchanged" in screen or "third-row" in screen:
            raise AssertionError(f"shrinking composer left obsolete rows:\n{screen}")

        terminal.send_key("C-u")
        wide = "a" * 20 + "界é tail"
        terminal.send_text(wide)
        screen = terminal.wait("界é tail")
        if "p> " + "a" * 20 not in screen:
            raise AssertionError(f"wide boundary damaged the previous row:\n{screen}")
        cursor = terminal.run("display-message", "-p", "-t", terminal.target,
                              "#{cursor_x}").strip()
        if cursor != "8":
            raise AssertionError(f"wide/combining cursor column was {cursor}")
        terminal.send_key("BSpace")
        terminal.wait("界é tai")
        terminal.resize(25, 18)
        terminal.wait("界é tai", join_wrapped=True)
        terminal.resize(24, 18)
        terminal.wait("界é tai", join_wrapped=True)

        terminal.send_key("C-u")
        terminal.send_text("a" * 21)  # Exact right margin, including p>.
        terminal.wait("p> " + "a" * 21, join_wrapped=True)
        terminal.send_text("xyz")
        terminal.wait("p> " + "a" * 21 + "xyz", join_wrapped=True)
        for _ in range(3):
            terminal.send_key("BSpace")
        terminal.send_text("Q")
        terminal.wait("p> " + "a" * 21 + "Q", join_wrapped=True)
        terminal.send_key("BSpace")
        terminal.send_key("BSpace")
        terminal.send_text("Z")
        terminal.wait("p> " + "a" * 20 + "Z", join_wrapped=True)
        terminal.resize(30, 18)
        terminal.wait("p> " + "a" * 20 + "Z", join_wrapped=True)
        terminal.send_key("C-u")
        terminal.exit()
    finally:
        try:
            screen = terminal.last_screen or terminal.capture()
            (case / "screen.txt").write_text(screen, encoding="utf-8")
        finally:
            close_fixture_terminal(terminal)


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
    run_paced_decode_case(binary, root, width=24)
    run_paced_decode_case(binary, root, width=26, unicode=True)
    run_paced_decode_case(binary, root, width=26, unicode=True, resize=25)
    run_markdown_case(binary, root)
    run_narrow_markdown_table_case(binary, root)
    run_render_case(binary, root)
    run_queue_case(binary, root)
    run_tool_case(binary, root)
    run_retained_composer_case(binary, root)
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
        "api_key = ${SNAJPAGENT_IRC_UI_KEY}\n"
        "connect_timeout_ms = 1000\nidle_timeout_ms = 3000\n"
        "request_timeout_ms = 5000\nauto_compact_input_tokens = 0\n"
        "exact_token_count = false\nnative_compaction = false\n"
        "[ui]\ntyping_pause_ms = 50\ncolor = never\n",
        encoding="utf-8",
    )


def write_catalog_config(path, provider_port):
    path.write_text(
        "[agent]\nmodel = uncached-start\nreasoning_effort = low\n"
        "read_agents_md = false\n"
        f"[provider ordinary]\nbase_url = http://127.0.0.1:{provider_port}\n"
        "api_key = ${SNAJPAGENT_IRC_UI_KEY}\n"
        "connect_timeout_ms = 1000\nidle_timeout_ms = 3000\n"
        "request_timeout_ms = 5000\n"
        f"[provider codex]\nbase_url = http://127.0.0.1:{provider_port}"
        "/backend-api/codex/\n"
        "api_key = ${SNAJPAGENT_IRC_UI_KEY}\n"
        "connect_timeout_ms = 1000\nidle_timeout_ms = 3000\n"
        "request_timeout_ms = 5000\n"
        "[ui]\ncolor = never\n",
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
        terminal.wait("    0% ordinary/uncached-start/low ›")
        terminal.submit("/verbose 6")
        terminal.wait("verbosity: 6")
        before = provider.catalog_paths()
        terminal.submit("/model cache")
        screen = terminal.wait("5. codex / codex-late / ultra",
                               join_wrapped=True)
        for expected in (
                "1. ordinary / standard-model / medium",
                "2. codex / codex-fast / medium",
                "3. codex / codex-tied / high",
                "4. codex / codex-late / low"):
            if expected not in screen:
                raise AssertionError(
                    f"model catalog UI omitted {expected!r}:\n{screen}"
                )
        assert "> GET /backend-api/codex/models?client_version=0.146.0 HTTP/1.1" in screen
        assert "> authorization:" in screen and "<redacted:bearer>" in screen
        assert "irc-ui-secret" not in screen
        cache_path = terminal.dotdir / "models.json"
        cache = json.loads(cache_path.read_text(encoding="utf-8"))
        if cache.get("schema_version") != 1:
            raise AssertionError("model cache omitted its schema version")
        if [entry["name"] for entry in cache["providers"]] != [
                "ordinary", "codex"]:
            raise AssertionError("mixed provider cache changed provider order")
        first_model = cache["providers"][0]["models"][0]
        if (first_model.get("count_capability") != "unknown" or
                first_model.get("observed_input_bytes") != 0 or
                first_model.get("observed_input_tokens") != 0 or
                first_model.get("observed_hard_input_tokens") != 0):
            raise AssertionError("fresh model cache has invalid accounting state")
        if [model["id"] for model in cache["providers"][1]["models"]] != [
                "codex-fast", "codex-tied", "codex-late"]:
            raise AssertionError("Codex cache retained hidden or unsorted models")
        models = cache["providers"][1]["models"]
        assert [m["default_effort"] for m in models] == ["medium", "high", "low"]
        assert models[-1]["efforts"] == ["low", "ultra"]
        limits = models[-1]["limits"]
        assert limits["context_window_tokens"] == 272000
        assert limits["max_context_window_tokens"] == 872000
        assert limits["max_output_tokens"] is None
        assert limits["effective_context_window_percent"] is None
        expected_paths = [
            "/v1/models",
            "/backend-api/codex/models?client_version=0.146.0",
        ]
        if provider.catalog_paths()[len(before):] != expected_paths:
            raise AssertionError("mixed refresh used unexpected catalog endpoints")

        paths_before_list = provider.catalog_paths()
        terminal.submit("/model list")
        terminal.wait("5. codex / codex-late / ultra", join_wrapped=True)
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
    expected = (f"{operator}@{MACHINE_HOSTNAME} :" if operator else
                "    0% ordinary/uncached-start/low ›")
    timestamped = re.compile(
        rf"(?m)^   \d{{2}}:\d{{2}}:\d{{2}} {re.escape(expected)}$"
    ) if operator else None
    screen = ""
    while time.monotonic() < deadline:
        screen = terminal.capture()
        visible = screen.rstrip()
        if ((timestamped.search(visible) is not None) if operator else
                visible.endswith(expected)):
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
    pattern = (
        rf"(?m)^\d{{2}}:\d{{2}}:\d{{2}} {re.escape(marker + nick)} › "
        rf"{re.escape(text)}$"
    )
    if re.search(pattern, screen) is None:
        raise AssertionError(f"missing timestamped IRC line {nick!r}: {text!r}\n{screen}")


def validate_irc_events(dotdir):
    _, events = read_events(dotdir)
    expected_messages = [
        ("oneop", "integration one from oneop", True),
        ("twoop", IRC_SECOND_MESSAGE, True),
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


def foreground_at(styled, pattern):
    foreground = None
    text, colors = [], []
    for chunk in re.split(r"(\x1b\[[0-9;]*m)", styled):
        if chunk.startswith("\x1b["):
            for value in chunk[2:-1].split(";"):
                code = int(value or "0")
                if code in (0, 39):
                    foreground = None
                elif 30 <= code <= 37:
                    foreground = code
        else:
            text.append(chunk)
            colors.extend([foreground] * len(chunk))
    match = re.search(pattern, "".join(text))
    assert match, f"styled terminal has no {pattern!r}:\n{styled}"
    return colors[match.start(1)]


def validate_irc_styles(terminal, own):
    styled = terminal.capture_styled()
    expected = [
        (nick, color, role)
        for color, role, nicks in (
            (36, "operator cyan", ("@oneop", "@twoop")),
            (34, "agent blue", ("hostbot", "onebot", "twobot")),
        )
        for nick in nicks
    ]
    for nick, color, role in expected:
        if nick == own:
            color, role = 35, "mention magenta"
        pattern = rf"(?m)^\d{{2}}:\d{{2}}:\d{{2}} ({nick}) "
        assert foreground_at(styled, pattern) == color, f"{nick} missing {role}:\n{styled}"
        if not nick.startswith("@"):
            # Self-mentions highlight timestamps and sender nicks, never the body.
            pattern = rf"(?m)^(\d{{2}}:\d{{2}}:\d{{2}}) {nick} "
            assert foreground_at(styled, pattern) == (35 if nick == own else None)
            pattern = rf"(?m)^\d{{2}}:\d{{2}}:\d{{2}} {nick} › ({nick}) heard"
            assert foreground_at(styled, pattern) is None
            pattern = rf"(?m)^\d{{2}}:\d{{2}}:\d{{2}} {nick} (›) {nick} heard"
            assert foreground_at(styled, pattern) == (35 if nick == own else None)


def run_destination_case(binary, root, provider, environment):
    endpoints = [f"127.0.0.1:{free_loopback_port()}" for _ in range(2)]
    specs = [
        ("a", "host-model", ["-s", endpoints[0], "-n", "servera", "-o", "opa", "-r", "alpha"]),
        ("b", "one-model", ["-s", endpoints[1], "-n", "serverb", "-o", "opb", "-r", "beta"]),
        ("c", "two-model", ["-c", endpoints[0], "-c", endpoints[1], "-n", "routerbot", "-o", "routerop"]),
    ]
    terminals = {}

    def deliveries(marker, expected):
        for name in expected:
            terminals[name].wait(marker)
        wait_irc_idle(list(terminals.values()))
        for name, terminal in terminals.items():
            _, events = read_events(terminal.dotdir)
            matches = [event for event in event_list(events, "irc_event")
                       if event["data"]["kind"] == "message" and
                       event["data"]["text"] == marker]
            assert len(matches) == expected.get(name, 0), (name, marker, matches)

    try:
        for name, model, args in specs:
            case = root / ("dest-" + name)
            workspace = case / "work"
            workspace.mkdir(mode=0o700, parents=True)
            config = case / "config.ini"
            write_irc_config(config, provider.port, model)
            terminal = TmuxTerminal(case / "terminal", binary, workspace,
                case / "state", config, 120, 24, args=args, environment=environment)
            terminals[name] = terminal
            terminal.wait(f"{args[args.index('-o') + 1]}@{MACHINE_HOSTNAME} :")
        client = terminals["c"]
        terminals["a"].wait("routerop joined")
        terminals["b"].wait("routerop joined")
        wait_irc_idle(list(terminals.values()))
        client.wait("[1 #alpha]")
        client.submit("destination-plain-one")
        deliveries("destination-plain-one", {"a": 1, "c": 1})
        client.submit("/2 destination-once-two")
        deliveries("destination-once-two", {"b": 1, "c": 1})
        client.submit("destination-still-one")
        deliveries("destination-still-one", {"a": 1, "c": 1})
        client.submit("/2")
        client.wait("destination: 2")
        client.wait("[2 #beta]")
        client.submit("destination-selected-two")
        deliveries("destination-selected-two", {"b": 1, "c": 1})
        client.submit("/all destination-broadcast")
        deliveries("destination-broadcast", {"a": 1, "b": 1, "c": 2})
        client.submit("/1 /all literal-command")
        deliveries("/all literal-command", {"a": 1, "c": 1})
        client.submit("/names")
        client.wait("selected destination: 2")
        client.wait(f"destination[1]: {endpoints[0]}")
        client.wait(f"destination[2]: {endpoints[1]}")

        client.submit("/rollout")
        client.wait("fake/two-model/medium ›")
        client.submit("destination-model 1 model-to-one")
        deliveries("model-to-one", {"a": 1, "c": 1})
        client.wait("destination model done")
        client.submit("destination-model null ambiguous-model")
        wait_irc_idle(list(terminals.values()))
        requests = provider.matching_requests("destination-model null ambiguous-model")
        deadline = time.monotonic() + 10.0
        while len(requests) < 2 and time.monotonic() < deadline:
            time.sleep(0.02)
            requests = provider.matching_requests("destination-model null ambiguous-model")
        outputs = [item["output"] for request in requests
                   for item in request["body"]["input"]
                   if item.get("type") == "function_call_output"]
        assert any("Select a destination" in output for output in outputs), outputs
        deliveries("ambiguous-model", {})

        client.submit(f"/disconnect {endpoints[1]}")
        client.wait("outgoing connection removed")
        client.submit("/chat")
        client.wait("[2 unavailable]")
        client.submit("/1")
        client.wait("destination: 1")
        client.submit("/1 single-still-valid")
        deliveries("single-still-valid", {"a": 1, "c": 1})
        assert "[1 #alpha]" not in client.capture().rstrip().splitlines()[-1]
        client.submit(f"/connect {endpoints[1]}")
        client.wait("outgoing connection added")
        client.submit("/names")
        client.wait(f"destination[3]: {endpoints[1]}")
        client.submit("/2 removed-target")
        client.wait("destination 2 is unavailable; use /names")
        client.wait(": /2 removed-target")
        deliveries("removed-target", {})
        for terminal in reversed(list(terminals.values())):
            terminal.send_key("C-u")
            terminal.send_key("C-d")
            terminal.wait_dead()
        print("tmux_terminal destinations: ok", flush=True)
    finally:
        for terminal in reversed(list(terminals.values())):
            terminal.close()


def run_listener_collision_case(binary, root, provider, environment):
    endpoint = f"localhost:{free_loopback_port()}"
    terminals = []
    try:
        for number in (1, 2):
            case = root / f"listener-{number}"
            workspace = case / "work"
            workspace.mkdir(mode=0o700, parents=True)
            config = case / "config.ini"
            write_irc_config(config, provider.port, "host-model")
            terminal = TmuxTerminal(case / "terminal", binary, workspace,
                case / "state", config, 120, 24,
                args=("-s", endpoint, "-n", f"agent{number}",
                      "-o", f"operator{number}"), environment=environment)
            terminals.append(terminal)
            if number == 1:
                terminal.wait(f"operator1@{MACHINE_HOSTNAME} :")
            else:
                terminal.wait_dead(timeout=5.0)
                assert terminal.run("display-message", "-p", "-t", terminal.target,
                                    "#{pane_dead_status}").strip() != "0"
                screen = terminal.capture(join_wrapped=True)
                assert f"cannot listen on IRC endpoint {endpoint}:" in screen, screen
                assert ("Address already in use" in screen or
                        "Address in use" in screen), screen
        terminals[0].submit("/names")
        terminals[0].wait(f"members[{endpoint}]:", join_wrapped=True)
        terminals[0].exit()
        print("tmux_terminal listener collision: ok", flush=True)
    finally:
        for terminal in reversed(terminals):
            terminal.close()

def run_multi_tool_cases(binary, root, provider, environment):
    for mode in ("parallel", "serial", "yield", "failure", "single-request", "single-serial", "steer", "cancel", "full-output"):
        case = root / ("multi-" + mode)
        workspace = case / "work"
        workspace.mkdir(mode=0o700, parents=True)
        provider.tool_workspace = workspace
        config = case / "config.ini"
        write_irc_config(config, provider.port, "host-model")
        text = config.read_text()
        if mode.startswith("single-"):
            # The runtime must still handle a provider returning several calls.
            text = text.replace("[provider fake]\n", "[provider fake]\nparallel_tool_calls = false\n")
        config.write_text(text + "[tool]\nmax_parallel_commands = " +
                          ("1" if mode in ("serial", "single-serial") else "2" if mode in ("steer", "cancel") else "4") + "\n", encoding="utf-8")
        terminal = TmuxTerminal(case / "terminal", binary, workspace,
                                case / "state", config, 120, 24,
                                args=("-vvv" if mode == "full-output" else "-v",), environment=environment)
        try:
            terminal.wait("host-model/medium ›")
            terminal.submit("multi-tools " + mode)
            if mode in ("steer", "cancel"):
                deadline = time.monotonic() + 4.0
                while not all((workspace / name).exists() for name in ("a.pid", "b.pid")):
                    assert time.monotonic() < deadline, "commands did not start"
                    time.sleep(0.02)
                if mode == "cancel":
                    terminal.send_key("C-d")
                    terminal.wait_dead(timeout=1.5)
                    _, events = read_events(terminal.dotdir)
                    assert len(event_list(events, "turn_interrupted")) == 1
                    assert not (workspace / "must-not-run").exists()
                    print("tmux_terminal multi-tool cancel: ok", flush=True)
                    continue
                terminal.submit("multi-tools steer")
            terminal.wait("multi tools confirmed")
            _, events = wait_for_terminal_event(terminal.dotdir, {"turn_completed"}, 5.0)
            starts = event_list(events, "tool_started")
            finishes = event_list(events, "tool_finished")
            assert len(finishes) == len(starts) + (1 if mode == "steer" else 0)
            assert not event_list(events, "turn_failed")
            if mode not in ("serial", "single-serial"):
                assert starts[1]["seq"] < finishes[0]["seq"]
            else:
                assert finishes[0]["seq"] < starts[1]["seq"]
            if mode in ("parallel", "failure"):
                assert finishes[0]["data"]["call_id"] == starts[1]["data"]["call_id"]
            if mode in ("yield", "single-request"):
                assert any(event["data"]["result"]["status"] == "running" for event in finishes)
            chunks = event_list(events, "process_output")
            if mode == "steer":
                assert not (workspace / "must-not-run").exists()
                assert len(event_list(events, "steering_added")) == 1
            elif mode == "full-output":
                assert sum(len(event["data"]["data"]) for event in chunks) > 80000
                assert "full-output-tail" in terminal.capture(join_wrapped=True)
            else:
                assert chunks and all(event["data"]["offset"] == 0 for event in chunks)
                assert len({event["data"]["handle"] for event in chunks}) == 2
            terminal.exit()
            print(f"tmux_terminal multi-tool {mode}: ok", flush=True)
        finally:
            terminal.close()
        if provider.failure:
            raise provider.failure


def run_output_cap_cases(binary, root, provider, environment):
    for name, configured, selected in (("default", None, None),
                                       ("above", 1234, 9999),
                                       ("below", 1234, 512)):
        case = root / ("cap-" + name)
        workspace = case / "work"
        workspace.mkdir(mode=0o700, parents=True)
        provider.tool_workspace = workspace
        config = case / "config.ini"
        write_irc_config(config, provider.port, "host-model")
        config.write_text(config.read_text() + "[tool]\nmax_output_bytes = 17\n" +
                          (f"max_output_tokens = {configured}\n"
                           if configured else ""), encoding="utf-8")
        ceiling = configured or 6000
        terminal = TmuxTerminal(case / "terminal", binary, workspace,
                                case / "state", config, 120, 24,
                                args=("-v",), environment=environment)
        try:
            terminal.wait("host-model/medium ›")
            terminal.submit(f"tool-cap {ceiling} {json.dumps(selected)}")
            terminal.wait("tool cap confirmed")
            _, events = wait_for_terminal_event(terminal.dotdir, {"turn_completed"}, 5.0)
            result = event_list(events, "tool_finished")[0]["data"]["result"]
            assert result["max_output_tokens"] == min(ceiling, selected or ceiling)
            chunks = event_list(events, "process_output")
            assert "".join(event["data"]["data"] for event in chunks) == "0" * 8000
            assert result["stdout"]["original_bytes"] == 8000
            terminal.exit()
            print(f"tmux_terminal output cap {name}: ok", flush=True)
        finally:
            terminal.close()


def run_ctrl_d_cases(binary, root, provider, environment):
    for mode in ("silent", "stream", "tool", "managed"):
        case = root / ("exit-" + mode)
        workspace = case / "work"
        workspace.mkdir(mode=0o700, parents=True)
        provider.tool_workspace = workspace
        provider.exit_started.clear()
        provider.exit_release.clear()
        config = case / "config.ini"
        write_irc_config(config, provider.port, "host-model")
        terminal = TmuxTerminal(case / "terminal", binary, workspace,
                                case / "state", config, 120, 24,
                                environment=environment)
        try:
            terminal.wait("host-model/medium ›")
            terminal.submit("exit-" + mode)
            if mode in ("tool", "managed"):
                deadline = time.monotonic() + 5.0
                while not (workspace / "command.pid").exists():
                    assert time.monotonic() < deadline, "command did not start"
                    time.sleep(0.02)
            if mode != "tool":
                assert provider.exit_started.wait(5.0), "request did not start"
            if mode == "stream":
                terminal.wait("exit stream prefix")
            terminal.send_key("C-d")
            terminal.wait_dead(timeout=1.5)
            assert terminal.run("display-message", "-p", "-t", terminal.target,
                                "#{pane_dead_status}").strip() == "0"
            screen = terminal.capture(join_wrapped=True)
            assert "You can resume this session" in screen, screen
            _, events = read_events(terminal.dotdir)
            assert len(event_list(events, "turn_interrupted")) == 1
            assert not event_list(events, "turn_completed")
            assert len(event_list(events, "response_started")) == (
                2 if mode == "managed" else 1)
            if mode in ("tool", "managed"):
                pid = int((workspace / "command.pid").read_text())
                try:
                    os.kill(pid, 0)
                except ProcessLookupError:
                    pass
                else:
                    raise AssertionError(f"command {pid} survived Ctrl-D")
            print(f"tmux_terminal ctrl-d {mode}: ok", flush=True)
        finally:
            provider.exit_release.set()
            terminal.close()


def run_runtime_networking_cases(binary, root, provider, environment):
    for level in range(7):
        for view in (("chat", "rollout", "burst") if level == 0 else ("chat", "rollout")):
            case = root / f"runtime-{level}-{view}"
            workspace = case / "work"
            workspace.mkdir(mode=0o700, parents=True)
            config = case / "config.ini"
            write_irc_config(config, provider.port, "host-model")
            endpoint = f"127.0.0.1:{free_loopback_port()}"
            arrived, release = threading.Event(), threading.Event()
            requests = []
            prefix = "runtime uninterrupted prefix\n"

            def respond(handler, request, sequence):
                requests.append(request)
                first = len(requests) == 1
                body = provider.response_body(sequence,
                    prefix if first else f"runtime completion {len(requests)}").encode()
                split = body.index(b"event: response.output_text.done")
                streaming = first and level % 2 == 1
                handler.close_connection = True
                if streaming:
                    handler.send_response(200)
                    handler.send_header("Content-Type", "text/event-stream")
                    handler.send_header("Connection", "close")
                    handler.end_headers()
                    handler.wfile.write(body[:split])
                    handler.wfile.flush()
                if first:
                    arrived.set()
                    assert release.wait(15.0), "runtime commands did not finish during the request"
                if not streaming:
                    handler.send_response(200)
                    handler.send_header("Content-Type", "text/event-stream")
                    handler.send_header("Content-Length", str(len(body)))
                    handler.send_header("Connection", "close")
                    handler.end_headers()
                handler.wfile.write(body[split:] if streaming else body)

            provider.runtime_handler = respond
            terminal = TmuxTerminal(case / "terminal", binary, workspace,
                case / "state", config, 120, 24,
                args=(["-v"] * level + ["-n", "runtimeagent", "-o", "runtimeop", "-r", "lab"]),
                environment=environment)
            peer = None
            try:
                terminal.wait("host-model/medium ›")
                terminal.submit("runtime-main")
                assert arrived.wait(5.0), "provider did not receive the initial request"
                initial = json.dumps(requests[0], sort_keys=True)
                assert "irc_send" not in {tool.get("name") for tool in requests[0]["tools"]}
                if view == "chat":
                    terminal.submit("/chat")
                    terminal.wait("chat is offline")
                terminal.submit(f"/server start {endpoint}")
                terminal.wait(f"hosting started on {endpoint}", join_wrapped=True)
                peer = socket.create_connection(("127.0.0.1", int(endpoint.rsplit(":", 1)[1])))
                peer.sendall(b"NICK runtimepeer\r\nUSER runtimepeer 0 * :human\r\nJOIN #lab\r\n")
                backgrounds = ["runtime-background café € " + "long ordinary text " * 185]
                mentions = ["runtimeagent: runtime-mention-one", "@runtimeagent runtime-mention-two €"]
                if view == "burst":
                    backgrounds = [f"background-{i:03} café € " + "ordinary " * 390 for i in range(80)]
                    mentions = [f"runtimeagent: mention-{i:03} € " + "urgent " * 500 for i in range(80)]
                for message in backgrounds:
                    peer.sendall(f"PRIVMSG #lab :{message}\r\n".encode())
                for message in mentions:
                    peer.sendall(f"NOTICE #lab :{message}\r\n".encode())
                peer.sendall(b"NICK renamedpeer\r\nTOPIC #lab :runtime-topic\r\n")
                deadline = time.monotonic() + 5.0
                while True:
                    _, log = read_events(terminal.dotdir)
                    received = [event["data"] for event in event_list(log, "irc_event")]
                    if any(event["text"] == "runtime-topic" for event in received):
                        break
                    assert time.monotonic() < deadline, received
                    time.sleep(0.02)
                assert all(any(event["text"] == message for event in received) for message in backgrounds)
                assert len(requests) == 1, "IRC input interrupted a live provider response"
                terminal.submit("/server stop")
                terminal.wait("hosting stopped; outgoing connections unchanged", join_wrapped=True)
                with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
                    assert probe.connect_ex(("127.0.0.1", int(endpoint.rsplit(":", 1)[1]))) != 0
                assert json.dumps(requests[0], sort_keys=True) == initial
                _, log = read_events(terminal.dotdir)
                assert not event_list(log, "response_interrupted")
                assert not event_list(log, "turn_completed")
                release.set()
                deadline = time.monotonic() + 10.0
                expected = 4 if view == "burst" else 3
                while len(requests) < expected:
                    assert time.monotonic() < deadline, (len(requests), terminal.capture())
                    time.sleep(0.02)
                wait_irc_idle([terminal])
                second = json.dumps(requests[1], ensure_ascii=False)
                third = json.dumps(requests[-1], ensure_ascii=False)
                for marker in mentions:
                    assert marker in second, (marker, second)
                assert all(message not in second for message in backgrounds), "topology admitted ordinary chat too early"
                assert all(message in third for message in backgrounds), "final disconnect stranded background input"
                assert "runtime-topic" in third and "renamedpeer" in third
                assert endpoint in second and "sender=runtimepeer operator=true" in second
                assert "no active endpoints" in second
                assert prefix.rstrip() in second, "IRC mention truncated the provider's answer"
                assert "irc_send" not in {tool.get("name") for tool in requests[1]["tools"]}
                _, log = read_events(terminal.dotdir)
                admitted = [event["data"]["text"] for event in event_list(log, "steering_added")]
                assert len(admitted) == (2 if view == "burst" else 1)
                assert all(sum(marker in text for text in admitted) == 1 for marker in mentions)
                assert len(requests) == expected, "received input was admitted as duplicate work"
                if view == "chat":
                    assert "runtime completion" not in terminal.capture(), "private response leaked into offline chat"
                    terminal.submit("/rollout")
                    terminal.wait("runtime completion 3")
                terminal.exit()
                screen = terminal.capture(join_wrapped=True)
                resume = screen.split("You can resume this session with the following command:", 1)[1]
                assert "--no-listen" in resume and "--no-client" in resume
                print(f"tmux_terminal runtime input delivery {level}/{view}: ok", flush=True)
            finally:
                release.set()
                if peer is not None:
                    peer.close()
                terminal.close()
                provider.runtime_handler = None


def run_runtime_routing_cases(binary, root, provider, environment):
    cases = [("irc_send", change, "response") for change in ("noop", "replace", "readd", "off")]
    cases += [("irc_state", "off", "response"), ("irc_topic", "off", "response"),
              ("irc_send", "off", "count"), ("irc_send", "off", "retry")]
    for tool, change, phase in cases:
        case = root / f"route-{tool}-{change}-{phase}"
        workspace = case / "work"
        workspace.mkdir(mode=0o700, parents=True)
        config = case / "config.ini"
        write_irc_config(config, provider.port, "host-model")
        if phase == "count":
            config.write_text(config.read_text().replace("exact_token_count = false", "exact_token_count = true"))
        endpoint = f"127.0.0.1:{free_loopback_port()}"
        destination = endpoint if change == "readd" else f"127.0.0.1:{free_loopback_port()}"
        arrived, release = threading.Event(), threading.Event()
        requests = []
        counts = []
        marker = "runtime-stale-send-must-not-migrate"

        def count(handler, request):
            counts.append(request)
            if len(counts) == 1:
                arrived.set()
                assert release.wait(15.0)
            body = b'{"object":"response.input_tokens","input_tokens":20}'
            handler.send_response(200)
            handler.send_header("Content-Type", "application/json")
            handler.send_header("Content-Length", str(len(body)))
            handler.end_headers()
            handler.wfile.write(body)
            handler.close_connection = True

        def respond(handler, request, sequence):
            requests.append(request)
            if len(requests) == 1 and phase != "count":
                arrived.set()
                assert release.wait(15.0)
            if phase == "retry" and len(requests) == 1:
                handler.send_response(503)
                handler.send_header("Retry-After", "1")
                handler.send_header("Content-Length", "0")
                handler.end_headers()
                handler.close_connection = True
                return
            if len(requests) == (2 if phase == "retry" else 1):
                arguments = {"destination": None, "notice": False, "text": marker} if tool == "irc_send" else \
                    {"destination": None, "topic": marker} if tool == "irc_topic" else {}
                body = provider.function_body(sequence, "runtime-route", tool, arguments).encode()
            else:
                body = provider.response_body(sequence, "runtime routing complete").encode()
            handler.send_response(200)
            handler.send_header("Content-Type", "text/event-stream")
            handler.send_header("Content-Length", str(len(body)))
            handler.send_header("Connection", "close")
            handler.end_headers()
            handler.wfile.write(body)
            handler.close_connection = True

        provider.runtime_handler = respond
        provider.runtime_count_handler = count
        terminal = TmuxTerminal(case / "terminal", binary, workspace,
            case / "state", config, 120, 24,
            args=["-s", endpoint, "-n", "runtimeagent", "-o", "runtimeop", "-r", "lab"],
            environment=environment)
        peer = None
        try:
            terminal.wait(f"runtimeop@{MACHINE_HOSTNAME} :")
            terminal.submit("/rollout")
            terminal.wait("host-model/medium ›")
            terminal.submit("runtime-routing")
            assert arrived.wait(5.0)
            frozen = counts[0] if phase == "count" else requests[0]
            assert tool in {item.get("name") for item in frozen["tools"]}
            if change == "noop":
                terminal.submit(f"/server start {endpoint}")
                terminal.wait(f"already hosting {endpoint}")
                destination = endpoint
            else:
                terminal.submit("/server stop")
                terminal.wait("hosting stopped; outgoing connections unchanged", join_wrapped=True)
                if change != "off":
                    terminal.submit(f"/server start {destination}")
                    terminal.wait(f"hosting started on {destination}", join_wrapped=True)
            if change != "off":
                peer = socket.create_connection(("127.0.0.1", int(destination.rsplit(":", 1)[1])))
                peer.sendall(b"NICK routepeer\r\nUSER routepeer 0 * :human\r\nJOIN #lab\r\n")
                deadline = time.monotonic() + 5.0
                while True:
                    _, log = read_events(terminal.dotdir)
                    if any(event["data"]["kind"] == "join" and event["data"]["nick"] == "routepeer"
                           for event in event_list(log, "irc_event")):
                        break
                    assert time.monotonic() < deadline
                    time.sleep(0.02)
            release.set()
            terminal.wait("runtime routing complete")
            wait_irc_idle([terminal])
            assert len(requests) >= 2
            result_request = requests[2] if phase == "retry" else requests[1]
            if phase == "retry":
                assert requests[0] == requests[1], "retry rebuilt a frozen request after disconnect"
            if phase == "count":
                assert counts[0]["tools"] == requests[0]["tools"]
                assert counts[0]["input"] == requests[0]["input"]
            calls = {item["call_id"] for item in result_request["input"]
                     if item.get("type") == "function_call" and item.get("name") == tool}
            outputs = [item["output"] for item in result_request["input"]
                       if item.get("type") == "function_call_output" and item.get("call_id") in calls]
            assert len(outputs) == 1
            if tool == "irc_state":
                assert "no active endpoints" in outputs[0] and "invalid" not in outputs[0]
            elif change != "noop":
                assert "not performed" in outputs[0], outputs
            else:
                assert "destination 1: queued" in outputs[0], outputs
            _, log = read_events(terminal.dotdir)
            assert not event_list(log, "turn_failed"), log
            public = [event["data"] for event in event_list(log, "irc_event")
                      if event["data"]["text"] == marker]
            assert len(public) == (1 if change == "noop" else 0), public
            if public:
                assert public[0]["endpoint"] == endpoint
            if peer is not None:
                peer.settimeout(0.3)
                wire = b""
                try:
                    while True:
                        chunk = peer.recv(65536)
                        if not chunk:
                            break
                        wire += chunk
                except socket.timeout:
                    pass
                assert (marker.encode() in wire) == (change == "noop"), wire
            terminal.exit()
            print(f"tmux_terminal frozen routing {tool}/{change}/{phase}: ok", flush=True)
        finally:
            release.set()
            if peer is not None:
                peer.close()
            terminal.close()
            provider.runtime_handler = None
            provider.runtime_count_handler = None


def run_runtime_boundary_cases(binary, root, provider, environment):
    for boundary in ("tool", "steer", "queue", "goal"):
        case = root / f"boundary-{boundary}"
        workspace = case / "work"
        workspace.mkdir(mode=0o700, parents=True)
        config = case / "config.ini"
        write_irc_config(config, provider.port, "host-model")
        endpoint = f"127.0.0.1:{free_loopback_port()}"
        arrived, release = threading.Event(), threading.Event()
        requests = []
        pid = None

        def respond(handler, request, sequence):
            requests.append(request)
            number = len(requests)
            if boundary == "tool" and number == 1:
                body = provider.function_body(sequence, "runtime-exec", "exec_command", {
                    "command": 'printf "%s" "$$" > command.pid; IFS= read -r line; '
                               'printf "same-process:%s:%s\\n" "$$" "$line"',
                    "workdir": str(workspace), "stdin": None, "pty": False,
                    "timeout_ms": None, "max_output_tokens": None, "yield_ms": 0,
                }).encode()
            elif boundary == "tool" and number == 2:
                arrived.set()
                assert release.wait(15.0)
                _, log = read_events(case / "state")
                handle = event_list(log, "tool_finished")[0]["data"]["result"]["handle"]
                body = provider.functions_body(sequence, [("runtime-mixed", "irc_send",
                    {"destination": None, "text": "stale-mixed-send", "notice": False}), ("runtime-stdin", "write_stdin", {
                        "handle": handle, "data": "continue-same-handle\n", "eof": False,
                        "terminate": False, "yield_ms": 1000, "max_output_tokens": None,
                    })]).encode()
            elif number == 1:
                body = provider.response_body(sequence, "boundary delivered prefix\n").encode()
                split = body.index(b"event: response.output_text.done")
                handler.send_response(200)
                handler.send_header("Content-Type", "text/event-stream")
                handler.send_header("Connection", "close")
                handler.end_headers()
                handler.wfile.write(body[:split])
                handler.wfile.flush()
                arrived.set()
                assert release.wait(15.0)
                try:
                    handler.wfile.write(body[split:])
                except (BrokenPipeError, ConnectionResetError):
                    assert boundary == "steer"
                handler.close_connection = True
                return
            elif boundary == "goal" and number == 2:
                body = provider.function_body(sequence, "runtime-goal-done", "update_goal", {
                    "action": "complete", "text": None,
                }).encode()
            else:
                body = provider.response_body(sequence, f"boundary completion {number}").encode()
            handler.send_response(200)
            handler.send_header("Content-Type", "text/event-stream")
            handler.send_header("Content-Length", str(len(body)))
            handler.send_header("Connection", "close")
            handler.end_headers()
            handler.wfile.write(body)
            handler.close_connection = True

        provider.runtime_handler = respond
        terminal = TmuxTerminal(case / "terminal", binary, workspace, case / "state",
            config, 120, 24, args=["-s", endpoint, "-n", "runtimeagent", "-o", "runtimeop", "-r", "lab"],
            environment=environment)
        peer = None
        try:
            terminal.wait(f"runtimeop@{MACHINE_HOSTNAME} :")
            terminal.submit("/rollout")
            terminal.wait("host-model/medium ›")
            terminal.submit("/goal set runtime-goal" if boundary == "goal" else "runtime-boundary")
            if boundary == "tool":
                deadline = time.monotonic() + 5.0
                while not (workspace / "command.pid").exists():
                    assert time.monotonic() < deadline, terminal.capture()
                    time.sleep(0.02)
                pid = int((workspace / "command.pid").read_text())
                os.kill(pid, 0)
                terminal.submit(f"/connect 127.0.0.1:{free_loopback_port()}")
                terminal.wait("outgoing connection added")
                terminal.submit("/disconnect")
                terminal.wait("outgoing connections removed; hosting unchanged", join_wrapped=True)
                os.kill(pid, 0)
            else:
                assert arrived.wait(5.0), terminal.capture()
            peer = socket.create_connection(("127.0.0.1", int(endpoint.rsplit(":", 1)[1])))
            peer.sendall(b"NICK boundarypeer\r\nUSER boundarypeer 0 * :human\r\nJOIN #lab\r\n"
                         b"PRIVMSG #lab :boundary ordinary message\r\n")
            if boundary == "tool":
                peer.sendall(b"NOTICE #lab :runtimeagent: boundary urgent message\r\n")
                assert arrived.wait(5.0), terminal.capture()
                second = json.dumps(requests[1])
                assert "boundary urgent message" in second
                assert "boundary ordinary message" not in second
                os.kill(pid, 0)
            deadline = time.monotonic() + 5.0
            while True:
                _, log = read_events(terminal.dotdir)
                if any(event["data"].get("text") == "boundary ordinary message"
                       for event in event_list(log, "irc_event")):
                    break
                assert time.monotonic() < deadline
                time.sleep(0.02)
            terminal.submit("/server stop")
            terminal.wait("hosting stopped; outgoing connections unchanged", join_wrapped=True)
            if boundary == "steer":
                terminal.submit("boundary direct steer")
                deadline = time.monotonic() + 5.0
                while len(requests) < 2:
                    assert time.monotonic() < deadline, terminal.capture()
                    time.sleep(0.02)
                second = json.dumps(requests[1])
                assert "boundary direct steer" in second and "boundary delivered prefix" in second
                assert "boundary ordinary message" not in second
            elif boundary == "queue":
                terminal.submit("/queue boundary future input")
                terminal.wait("next › boundary future input")
                assert len(requests) == 1
            release.set()
            expected = 4 if boundary == "tool" else 3
            deadline = time.monotonic() + 10.0
            while len(requests) < expected:
                assert time.monotonic() < deadline, (len(requests), terminal.capture(), provider.failure)
                time.sleep(0.02)
            wait_irc_idle([terminal])
            assert len(requests) == expected
            _, log = read_events(terminal.dotdir)
            assert not event_list(log, "turn_failed"), event_list(log, "turn_failed")
            assert provider.failure is None, provider.failure
            assert "boundary ordinary message" in json.dumps(requests[-1])
            if boundary == "tool":
                third = json.dumps(requests[2])
                assert "not performed" in third
                assert f"same-process:{pid}:continue-same-handle" in third
                assert not any(event["data"].get("text") == "stale-mixed-send"
                               for event in event_list(log, "irc_event"))
                completed = event_list(log, "tool_finished")
                assert completed[0]["data"]["result"]["reason"] == "steering_handoff"
            elif boundary == "queue":
                assert "boundary future input" in json.dumps(requests[1])
                assert "boundary ordinary message" not in json.dumps(requests[1])
            elif boundary == "goal":
                assert "boundary ordinary message" in json.dumps(requests[1]), "goal starved background input"
            assert bool(event_list(log, "response_interrupted")) == (boundary == "steer")
            terminal.exit()
            print(f"tmux_terminal runtime boundary {boundary}: ok", flush=True)
        finally:
            release.set()
            if peer is not None:
                peer.close()
            terminal.close()
            provider.runtime_handler = None


def run_runtime_history_case(binary, root, provider, environment):
    case = root / "runtime-history"
    workspace = case / "work"
    workspace.mkdir(mode=0o700, parents=True)
    config = case / "config.ini"
    write_irc_config(config, provider.port, "host-model")
    arrived, release = threading.Event(), threading.Event()
    requests = []
    history = "agent7: historical mention café must stay historical"

    def respond(handler, request, sequence):
        requests.append(request)
        if len(requests) == 1:
            arrived.set()
            assert release.wait(15.0)
        body = provider.response_body(sequence, f"history completion {len(requests)}").encode()
        handler.send_response(200)
        handler.send_header("Content-Type", "text/event-stream")
        handler.send_header("Content-Length", str(len(body)))
        handler.end_headers()
        handler.wfile.write(body)
        handler.close_connection = True

    provider.runtime_handler = respond
    upstream = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    upstream.bind(("127.0.0.1", 0))
    upstream.listen(2)
    upstream.settimeout(4.0)
    endpoint = f"127.0.0.1:{upstream.getsockname()[1]}"
    terminal = TmuxTerminal(case / "terminal", binary, workspace, case / "state",
        config, 120, 24, args=["-n", "agent", "-o", "operator"], environment=environment)
    links = []
    try:
        terminal.wait("host-model/medium ›")
        terminal.submit("runtime history main")
        assert arrived.wait(5.0)
        terminal.submit(f"/connect {endpoint}")
        for _ in range(2):
            link, _ = upstream.accept()
            links.append(link)
            link.settimeout(3.0)
            wire = b""
            while b"USER " not in wire:
                wire += link.recv(8192)
            nick = re.search(rb"NICK (\w+)\r\n", wire)[1].decode() + "7"
            link.sendall((f":fake 001 {nick} :welcome\r\n"
                          f":fake 005 {nick} SAJROOM=#lab :supported\r\n"
                          f":fake 376 {nick} :end\r\n").encode())
            wire = b""
            while b"JOIN #lab\r\n" not in wire:
                wire += link.recv(8192)
            link.sendall((f":{nick}!u@fake JOIN #lab\r\n"
                          f":fake 353 {nick} = #lab :@operator7 agent7 peer\r\n"
                          f":fake 366 {nick} #lab :end\r\n"
                          ":fake BATCH +h chathistory #lab\r\n"
                          f"@batch=h;time=2026-09-01T12:00:00.000Z :peer!u@fake PRIVMSG #lab :{history}\r\n"
                          ":fake BATCH -h\r\n").encode())
        deadline = time.monotonic() + 5.0
        while True:
            _, log = read_events(terminal.dotdir)
            if any(history in event["data"]["text"] for event in event_list(log, "irc_snapshot")):
                break
            assert time.monotonic() < deadline, provider.failure
            time.sleep(0.02)
        assert len(requests) == 1
        historical = [event["data"] for event in event_list(log, "irc_event")
                      if event["data"]["text"] == history]
        assert len(historical) == 1 and historical[0]["historical"]
        terminal.submit("/chat")
        screen = terminal.wait("── history replayed ──", join_wrapped=True)
        assert screen.count("── history replayed ──") == 1, screen
        assert re.search(r"\d{2}:\d{2}:\d{2} peer › " + re.escape(history), screen), screen
        assert not re.search(r"\d{2}:\d{2}:\d{2} history ", screen), screen
        assert screen.index(history) < screen.index("── history replayed ──"), screen
        terminal.submit("/disconnect")
        terminal.wait("outgoing connections removed; hosting unchanged", join_wrapped=True)
        release.set()
        deadline = time.monotonic() + 8.0
        while len(requests) < 2:
            assert time.monotonic() < deadline, terminal.capture()
            time.sleep(0.02)
        wait_irc_idle([terminal])
        second = json.dumps(requests[1], ensure_ascii=False)
        assert history in second and endpoint in second and "agent7" in second
        assert "no active endpoints" in second
        _, log = read_events(terminal.dotdir)
        assert not event_list(log, "steering_added"), "historical mention became urgent input"
        assert not event_list(log, "irc_reply_reminder")
        assert not event_list(log, "turn_failed") and provider.failure is None
        assert len(requests) == 2
        terminal.exit()
        print("tmux_terminal runtime historical input: ok", flush=True)
    finally:
        release.set()
        for link in links:
            link.close()
        upstream.close()
        terminal.close()
        provider.runtime_handler = None


def run_provider_retry_input_cases(binary, root, provider, environment):
    for mode in ("steer", "chat", "mention", "queue", "command", "before", "zero", "healthy"):
        case = root / ("retry-" + mode)
        workspace = case / "work"
        workspace.mkdir(mode=0o700, parents=True)
        config = case / "config.ini"
        write_irc_config(config, provider.port, "host-model")
        endpoint = f"127.0.0.1:{free_loopback_port()}"
        arrived, release = threading.Event(), threading.Event()
        requests = []
        marker = "fresh-input-" + mode

        def respond(handler, request, sequence):
            if provider.latest_user(request) == "retry-original":
                requests.append(request)
                first = len(requests) == 1
            else:
                first = False
                if requests:
                    requests.append(request)
            fail = first and mode != "healthy"
            if first:
                arrived.set()
                assert release.wait(10.0), "retry input was not admitted"
            body = (provider.event("response.failed", {
                "type": "response.failed", "response": {"error": {
                    "code": "server_error", "message": "temporary fixture failure"}}
            }) if fail else provider.response_body(sequence, "retry input complete")).encode()
            handler.send_response(200)
            handler.send_header("Content-Type", "text/event-stream")
            handler.send_header("Retry-After", "0" if mode == "zero" else "2")
            handler.send_header("Content-Length", str(len(body)))
            handler.send_header("Connection", "close")
            handler.end_headers()
            handler.wfile.write(body)
            handler.close_connection = True

        provider.runtime_handler = respond
        terminal = TmuxTerminal(case / "term", binary, workspace, case / "state", config,
            140, 28, args=["-s", endpoint, "-n", "retrybot", "-o", "retryop", "-r", "lab"],
            environment=environment)
        peer = None
        try:
            terminal.wait(f"retryop@{MACHINE_HOSTNAME} :")
            peer = socket.create_connection(("127.0.0.1", int(endpoint.rsplit(":", 1)[1])))
            peer.sendall(b"NICK retrypeer\r\nUSER retrypeer 0 * :human\r\nJOIN #lab\r\n")
            terminal.wait("retrypeer joined")
            wait_irc_idle([terminal])
            terminal.submit("/rollout")
            terminal.wait("host-model/medium ›")
            terminal.submit("retry-original")
            assert arrived.wait(5.0)
            if mode not in ("before", "zero", "healthy"):
                release.set()
                terminal.wait("provider retry 1/2", timeout=5.0)
            if mode == "steer":
                terminal.submit(marker)
            elif mode == "queue":
                terminal.submit("/queue " + marker)
            elif mode == "command":
                terminal.submit("/status")
            else:
                text = ("retrybot: " if mode == "mention" else "") + marker
                peer.sendall(f"PRIVMSG #lab :{text}\r\n".encode())
                deadline = time.monotonic() + 5.0
                while True:
                    _, events = read_events(terminal.dotdir)
                    if any(e["data"].get("text") == text for e in event_list(events, "irc_event")):
                        break
                    assert time.monotonic() < deadline, "chat was not admitted during retry"
                    time.sleep(0.02)
                if mode in ("before", "zero", "healthy"):
                    release.set()
            wait_irc_idle([terminal])
            _, events = read_events(terminal.dotdir)
            if mode == "command":
                assert len(requests) == 2 and requests[0] == requests[1], requests
            else:
                assert len(requests) >= 1
                assert all(request != requests[0] for request in requests[1:]), "new input replayed stale request"
                if mode == "queue":
                    queued = event_list(events, "future_turn_queued")
                    assert any(e["data"]["text"] == marker for e in queued)
                    terminal.wait("retry input complete")
                    wait_irc_idle([terminal])
                assert any(marker in json.dumps(request) for request in requests[1:]), (mode, requests)
                if mode == "steer":
                    assert event_list(events, "response_interrupted")
                    assert not event_list(events, "response_failed")
                if mode == "healthy":
                    assert not event_list(events, "response_interrupted")
                    assert not event_list(events, "response_failed")
            terminal.exit()
            print(f"tmux_terminal provider retry input {mode}: ok", flush=True)
        finally:
            release.set()
            if peer is not None:
                peer.close()
            terminal.close()
            provider.runtime_handler = None


def run_provider_clarification_cases(binary, root, provider, environment):
    cases = [("success", level) for level in range(7)]
    cases += [(mode, 0) for mode in ("exhausted", "steer", "chat", "queue", "partial", "prior")]
    for mode, level in cases:
        case = root / f"clarify-{mode}-{level}"
        workspace = case / "work"
        workspace.mkdir(mode=0o700, parents=True)
        (workspace / "input.txt").write_text("ordinary application data\n")
        config = case / "config.ini"
        write_irc_config(config, provider.port, "host-model")
        endpoint = f"127.0.0.1:{free_loopback_port()}"
        arrived, release = threading.Event(), threading.Event()
        requests = []
        fresh = "clarification-fresh-" + mode
        original = "clarify-original: improve the local file reader"

        def respond(handler, request, sequence):
            active = requests or provider.latest_user(request) == original
            if active:
                requests.append(request)
            attempt = len(requests) - (1 if mode == "prior" else 0)
            changed = fresh in json.dumps(request)
            fail = active and not changed and (mode != "prior" or len(requests) > 1) and (
                mode == "exhausted" or attempt <= 3)
            if fail and attempt == 2 and mode in ("steer", "chat", "queue"):
                arrived.set()
                assert release.wait(10.0), "new clarification input did not arrive"
            if mode == "prior" and active and len(requests) == 1:
                body = provider.function_body(sequence, "prior-read", "read_file", {
                    "path": "input.txt", "start_line": 1, "end_line": 1})
            elif fail:
                body = ""
                if mode == "partial":
                    body = provider.response_body(sequence, "already delivered")
                    body = body[:body.index("event: response.completed")]
                body += provider.event("response.failed", {"type": "response.failed",
                    "response": {"error": {"code": "cyber_policy", "message": "fixture scope rejection"}}})
            else:
                body = provider.response_body(sequence, "accurate scope clarified")
            encoded = body.encode()
            handler.send_response(200)
            handler.send_header("Content-Type", "text/event-stream")
            handler.send_header("Content-Length", str(len(encoded)))
            handler.send_header("Connection", "close")
            handler.end_headers()
            try:
                handler.wfile.write(encoded)
            except (BrokenPipeError, ConnectionResetError):
                if not (mode == "steer" and attempt == 2 and release.is_set()):
                    raise
            handler.close_connection = True

        provider.runtime_handler = respond
        terminal = TmuxTerminal(case / "term", binary, workspace, case / "state", config,
            140, 28, args=["-v"] * level + ["-s", endpoint, "-n", "clarifybot", "-o", "clarifyop", "-r", "lab"],
            environment=environment)
        peer = None
        try:
            terminal.wait(f"clarifyop@{MACHINE_HOSTNAME} :")
            peer = socket.create_connection(("127.0.0.1", int(endpoint.rsplit(":", 1)[1])))
            peer.sendall(b"NICK clarifypeer\r\nUSER clarifypeer 0 * :human\r\nJOIN #lab\r\n")
            terminal.wait("clarifypeer joined")
            wait_irc_idle([terminal])
            terminal.submit("/rollout")
            terminal.wait("host-model/medium ›")
            terminal.submit(original)
            if mode in ("steer", "chat", "queue"):
                assert arrived.wait(5.0)
                terminal.wait("provider clarification 1/3")
                if mode == "chat":
                    peer.sendall(f"PRIVMSG #lab :{fresh}\r\n".encode())
                    wanted = "irc_event"
                else:
                    terminal.submit(("/queue " if mode == "queue" else "") + fresh)
                    wanted = "future_turn_queued" if mode == "queue" else "steering_added"
                deadline = time.monotonic() + 5.0
                while True:
                    _, events = read_events(terminal.dotdir)
                    if any(e["data"].get("text") == fresh for e in event_list(events, wanted)):
                        break
                    assert time.monotonic() < deadline, "new input was not retained"
                    time.sleep(0.02)
                release.set()
            terminal.wait("fixture scope rejection" if mode in ("partial", "exhausted") else
                          "provider clarification 1/3")
            wait_irc_idle([terminal])
            if mode == "queue":
                terminal.wait("accurate scope clarified")
                wait_irc_idle([terminal])
            _, events = read_events(terminal.dotdir)
            corrections = event_list(events, "response_output_correction")
            count = 0 if mode == "partial" else 1 if mode in ("steer", "chat", "queue") else 3
            assert len(corrections) == count, (mode, len(corrections))
            relevant = requests[1:] if mode == "prior" else requests
            if mode in ("success", "exhausted", "prior"):
                assert len(relevant) == 4, (mode, len(relevant))
                for attempt, request in enumerate(relevant):
                    assert any(item.get("role") == "user" and item.get("content") == original
                               for item in request["input"]), "original task was rewritten"
                    notes = [item["content"] for item in request["input"]
                             if item.get("role") == "developer" and
                             item.get("content", "").startswith("The provider rejected the preceding")]
                    assert len(notes) == attempt, (mode, attempt, notes)
                    assert all("preserving its purpose, actions, targets, and authorization" in note and
                               "Do not conceal security-relevant details" in note for note in notes)
            elif mode == "partial":
                assert len(relevant) == 1
            else:
                assert len(relevant) == 3, (mode, len(relevant))
                assert fresh in json.dumps(relevant[-1]), "new input did not reach model"
            if mode == "prior":
                assert len(event_list(events, "tool_started")) == 1, "earlier tool was replayed"
            screen = terminal.capture(join_wrapped=True)
            for attempt in range(1, count + 1):
                assert screen.count(f"provider clarification {attempt}/3 after cyber_policy") == 1, screen
            assert "provider clarification 4/3" not in screen
            if level < 4:
                assert "response_output_correction" not in screen
            if level < 5:
                assert "The provider rejected the preceding" not in screen
            terminal.exit()
            # Reopen durable state through the ordinary session listing path.
            replay = subprocess.run([binary, "--dotdir", str(terminal.dotdir), "-l"],
                                    capture_output=True, text=True, env={**os.environ, **environment})
            assert replay.returncode == 0, replay.stderr
            print(f"tmux_terminal provider clarification {mode}/{level}: ok", flush=True)
        finally:
            release.set()
            if peer is not None:
                peer.close()
            terminal.close()
            provider.runtime_handler = None


def run_manual_retry_cases(binary, root, provider, environment):
    for mode in ("queue", "read-only-resume", "chat"):
        case = root / ("manual-" + mode)
        workspace = case / "work"
        workspace.mkdir(mode=0o700, parents=True)
        (workspace / "input.txt").write_text("retained tool result\n")
        config = case / "config.ini"
        write_irc_config(config, provider.port, "host-model")
        requests = []
        arrived, release = threading.Event(), threading.Event()
        original = "manual retry original " + mode

        def respond(handler, request, sequence):
            requests.append(request)
            attempt = len(requests)
            if attempt == 1:
                arrived.set()
                assert release.wait(10.0), "active retry command was not handled"
                if mode == "read-only-resume":
                    body = provider.function_body(sequence, "retry-read", "read_file", {
                        "path": "input.txt", "start_line": 1, "end_line": 1})
                else:
                    body = provider.function_body(sequence, "retry-read", "exec_command", {
                        "command": "cat input.txt", "workdir": str(workspace),
                        "stdin": None, "pty": False, "timeout_ms": None,
                        "yield_ms": 1000, "max_output_tokens": None})
            elif attempt in (2, 3):
                body = provider.event("response.failed", {"type": "response.failed",
                    "response": {"error": {"code": "fixture_failure",
                        "message": f"manual retry failure {attempt}"}}})
            else:
                body = provider.response_body(sequence, "manual retry complete")
            encoded = body.encode()
            handler.send_response(200)
            handler.send_header("Content-Type", "text/event-stream")
            handler.send_header("Content-Length", str(len(encoded)))
            handler.send_header("Connection", "close")
            handler.end_headers()
            handler.wfile.write(encoded)
            handler.close_connection = True

        provider.runtime_handler = respond
        terminal = TmuxTerminal(case / "term", binary, workspace, case / "state", config,
            140, 28, environment=environment)
        try:
            terminal.wait("host-model/medium ›")
            terminal.submit("/retry")
            terminal.wait("no failed turn to retry")
            assert not requests
            terminal.submit(("/ro " if mode == "read-only-resume" else "") + original)
            assert arrived.wait(5.0)
            terminal.submit("/retry")
            terminal.wait("that command is unavailable while a turn is active")
            if mode == "queue":
                terminal.submit("/queue still paused")
                terminal.wait("next › still paused")
            release.set()
            terminal.wait("manual retry failure 2")
            terminal.wait("turn failed; try /retry to continue")
            wait_irc_idle([terminal])
            assert len(requests) == 2
            if mode == "read-only-resume":
                log_path, _ = read_events(terminal.dotdir)
                terminal.exit()
                terminal.close()
                terminal = TmuxTerminal(case / "resume", binary, workspace, case / "state", config,
                    140, 28, args=["--resume", log_path.parent.name], environment=environment)
                terminal.wait("host-model/medium ›")
            if mode == "chat":
                terminal.submit("/chat")
                terminal.wait("chat is offline")
            terminal.submit("/retry")
            terminal.wait("manual retry failure 3")
            wait_irc_idle([terminal])
            terminal.submit("/retry")
            if mode == "chat":
                terminal.submit("/rollout")
            terminal.wait("manual retry complete")
            wait_irc_idle([terminal])
            _, events = read_events(terminal.dotdir)
            turns = event_list(events, "turn_started")
            assert len(turns) == 3 and len(requests) == 4
            assert len(event_list(events, "turn_failed")) == 2
            assert len(event_list(events, "tool_started")) == 1, "retry replayed completed tool"
            assert all(e["data"]["read_only"] == (mode == "read-only-resume") for e in turns)
            for request in requests[2:]:
                assert sum(item.get("role") == "user" and item.get("content") == original
                           for item in request["input"]) == 1, "retry duplicated the original prompt"
                assert "retained tool result" in json.dumps(request), "retry lost tool context"
                assert "still paused" not in json.dumps(request), "retry consumed paused queue"
                names = {tool.get("name") for tool in request["tools"]}
                assert ("exec_command" not in names) == (mode == "read-only-resume")
            if mode == "queue":
                assert len(event_list(events, "future_turn_queued")) == 1
                assert all(e["data"]["input_kind"] == "direct" for e in turns)
            terminal.submit("/retry")
            wait_irc_idle([terminal])
            assert len(requests) == 4, "retry after success started stale work"
            terminal.exit()
            replay = subprocess.run([binary, "--dotdir", str(terminal.dotdir), "-l"],
                                    capture_output=True, text=True, env={**os.environ, **environment})
            assert replay.returncode == 0, replay.stderr
            print(f"tmux_terminal manual retry {mode}: ok", flush=True)
        finally:
            release.set()
            terminal.close()
            provider.runtime_handler = None


def run_incremental_history_case(binary, root):
    root.mkdir(mode=0o700, parents=True)
    provider = FakeResponses()
    captured = []
    held, release = threading.Event(), threading.Event()
    hold_once = [True]

    def respond(handler, request, sequence):
        captured.append(request)
        if request.get("model") == "one-model" and hold_once[0] and "pending at process exit" in json.dumps(request):
            hold_once[0] = False
            held.set()
            assert release.wait(10), "pending-input test did not release its request"
        body = provider.response_body(sequence, "caught up").encode()
        handler.send_response(200)
        handler.send_header("Content-Type", "text/event-stream")
        handler.send_header("Content-Length", str(len(body)))
        handler.end_headers()
        try:
            handler.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            assert held.is_set() and release.is_set()
        handler.close_connection = True

    provider.runtime_handler = respond
    endpoint = f"127.0.0.1:{free_loopback_port()}"
    environment = {"SNAJPAGENT_IRC_UI_KEY": "irc-ui-secret"}
    terminals = {}
    generation = {"host": 0, "client": 0}
    configs = {}
    for name, model in (("host", "host-model"), ("client", "one-model")):
        case = root / name
        (case / "work").mkdir(mode=0o700, parents=True)
        config = case / "config.ini"
        write_irc_config(config, provider.port, model)
        if name == "host":
            config.write_text(config.read_text() + "[irc]\nhistory_lines = 12\n")
        configs[name] = config

    def launch(name, session=None):
        generation[name] += 1
        case = root / name
        args = (["-s", endpoint, "-n", "hostbot", "-o", "hostop", "-r", "lab"]
                if name == "host" else ["-c", endpoint, "-n", "clientbot", "-o", "clientop"])
        if session:
            args += ["--resume", session]
        t = TmuxTerminal(case / f"t{generation[name]}", binary, case / "work",
                         case / "state", configs[name], 140, 28, args=args, environment=environment)
        terminals[name] = t
        t.wait(("hostop" if name == "host" else "clientop") + f"@{MACHINE_HOSTNAME} :")
        return t

    def sid(t):
        return read_events(t.dotdir)[0].parent.name

    def records(t):
        return event_list(read_events(t.dotdir)[1], "irc_event")

    def wait_record(t, text, count=1):
        deadline = time.monotonic() + 10
        while sum(e["data"]["text"] == text for e in records(t)) < count:
            assert time.monotonic() < deadline, (text, t.capture(), provider.failure)
            time.sleep(0.02)
        return [e for e in records(t) if e["data"]["text"] == text]

    def observed(text, count=1):
        deadline = time.monotonic() + 10
        while True:
            requests = [r for r in captured if r.get("model") == "one-model"]
            # Count message payloads, not older requests legitimately retaining context.
            if any(sum(text in str(item.get("content", "")) for item in r.get("input", [])) == count
                   for r in requests):
                return
            assert time.monotonic() < deadline, (text, requests[-1:] , terminals['client'].capture())
            time.sleep(0.02)

    try:
        host = launch("host")
        client = launch("client")
        wait_irc_idle([host, client])
        host.submit("one original conversation marker")
        wait_record(client, "one original conversation marker")
        observed("one original conversation marker")
        wait_irc_idle([host, client])
        host_id = sid(host)
        first = wait_record(client, "one original conversation marker")[0]["data"]
        assert first["stream"] and first["sequence"]
        host.exit(); host.close()
        client.wait("disconnected")
        host = launch("host", host_id)
        host.wait("set mode · +o clientop")
        deadline = time.monotonic() + 10
        while not any(e["data"].get("kind") == "history_ready" for e in records(client)
                      if e["seq"] > wait_record(client, "one original conversation marker")[0]["seq"]):
            assert time.monotonic() < deadline, client.capture()
            time.sleep(0.02)
        wait_irc_idle([host, client])
        assert len(wait_record(client, "one original conversation marker")) == 1
        assert client.capture().count("one original conversation marker") == 1
        assert all(sum("one original conversation marker" in str(i.get("content", "")) for i in r.get("input", [])) <= 1
                   for r in captured if r.get("model") == "one-model")
        client.submit("/disconnect")
        client.wait("outgoing connections removed")
        host.submit("identical legitimate message")
        wait_record(host, "identical legitimate message")
        host.submit("identical legitimate message")
        wait_record(host, "identical legitimate message", 2)
        client.submit("/connect " + endpoint)
        missed = wait_record(client, "identical legitimate message", 2)
        assert all(e["data"]["historical"] for e in missed)
        assert len({e["data"]["sequence"] for e in missed}) == 2
        assert all(e["data"]["stream"] == first["stream"] for e in missed)
        observed("identical legitimate message", 2)
        wait_irc_idle([host, client])
        client_id = sid(client)
        client.exit(); client.close()
        client = launch("client", client_id)
        wait_irc_idle([host, client])
        assert len(wait_record(client, "identical legitimate message", 2)) == 2
        host.submit("after client process restart")
        wait_record(client, "after client process restart")
        observed("after client process restart")
        wait_irc_idle([host, client])
        host.submit("pending at process exit")
        wait_record(client, "pending at process exit")
        assert held.wait(8), client.capture()
        cutoff = len(captured)
        client.close()  # Exact task-owned terminal; intentionally interrupt delivery.
        release.set()
        client = launch("client", client_id)
        deadline = time.monotonic() + 10
        while not any(r.get("model") == "one-model" and "pending at process exit" in json.dumps(r)
                      for r in captured[cutoff:]):
            assert time.monotonic() < deadline, client.capture()
            time.sleep(0.02)
        wait_irc_idle([host, client])
        assert len(wait_record(client, "pending at process exit")) == 1
        client.submit("/disconnect")
        client.wait("outgoing connections removed")
        for number in range(16):
            host.submit(f"retention-gap-{number}")
            wait_record(host, f"retention-gap-{number}")
        client.submit("/connect " + endpoint)
        client.wait("history gap")
        wait_record(client, "retention-gap-15")
        wait_irc_idle([host, client])
        ids = [(e["data"]["stream"], e["data"]["sequence"]) for e in records(client) if e["data"]["stream"]]
        assert len(ids) == len(set(ids)), "duplicate IDs were admitted"
        # A new server session at the same address must not reuse an old cursor.
        host.exit(); host.close()
        client.wait("disconnected")
        host = launch("host")
        host.wait("set mode · +o clientop")
        host.submit("new stream is not skipped")
        fresh = wait_record(client, "new stream is not skipped")[0]["data"]
        assert fresh["stream"] != first["stream"]
        observed("new stream is not skipped")
        wait_irc_idle([host, client])
        client.exit(); host.exit()
        print("tmux_terminal incremental reconnect history: ok", flush=True)
    finally:
        release.set()
        for name, terminal in terminals.items():
            try:
                (root / name / "screen.txt").write_text(terminal.capture())
            except Exception:
                pass
            terminal.close()
        provider.close()


def run_irc_case(binary, root):
    root.mkdir(mode=0o700, parents=True)
    provider = FakeResponses()
    environment = {"SNAJPAGENT_IRC_UI_KEY": "irc-ui-secret"}
    try:
        run_manual_retry_cases(binary, root, provider, environment)
        run_provider_retry_input_cases(binary, root, provider, environment)
        run_provider_clarification_cases(binary, root, provider, environment)
        run_runtime_networking_cases(binary, root, provider, environment)
        run_runtime_routing_cases(binary, root, provider, environment)
        run_runtime_boundary_cases(binary, root, provider, environment)
        run_runtime_history_case(binary, root, provider, environment)
        run_destination_case(binary, root, provider, environment)
        run_listener_collision_case(binary, root, provider, environment)
        run_multi_tool_cases(binary, root, provider, environment)
        run_output_cap_cases(binary, root, provider, environment)
        run_ctrl_d_cases(binary, root, provider, environment)
        run_model_catalog_case(binary, root, provider, environment)
    finally:
        provider.close()
    run_irc_chat_case(binary, root / "chat")
    run_incremental_history_case(binary, root / "catchup")


def run_irc_chat_case(binary, root):
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
            terminal.wait(f"{operator}@{MACHINE_HOSTNAME} :")

        ordered = [terminals[name] for name in ("host", "one", "two")]
        terminals["host"].wait("twoop joined")
        terminals["one"].wait("twoop joined")
        terminals["one"].wait("set mode · +o twoop")
        terminals["two"].wait("── history replayed ──")
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
        for terminal in terminals.values():
            for agent in ("hostbot", "onebot", "twobot"):
                terminal.wait(f"{agent} heard one")
        wait_irc_idle(ordered)

        terminals["two"].submit("/verbose 1")
        terminals["two"].wait("verbosity: 1")
        wait_current_prompt(terminals["two"], "twoop")
        second = IRC_SECOND_MESSAGE
        terminals["two"].submit(second)
        for terminal in ordered:
            terminal.wait(second, join_wrapped=True)
        provider.wait_models(second)
        for terminal in terminals.values():
            for agent in ("hostbot", "onebot", "twobot"):
                terminal.wait(f"{agent} heard two")
        wait_irc_idle(ordered)

        terminals["two"].submit("/topic shared integration topic")
        for terminal in ordered:
            terminal.wait("@twoop set topic · shared integration topic")
        wait_irc_idle(ordered)

        for name, _model, _own, operator, _args in specs:
            terminal = terminals[name]
            wait_current_prompt(terminal, operator)
            screen = terminal.capture(join_wrapped=True)
            assert_chat_line(screen, "oneop", first, operator=True)
            assert_chat_line(screen, "twoop", second, operator=True)
            for suffix in ("one", "two"):
                for agent in ("hostbot", "onebot", "twobot"):
                    count = screen.count(f"{agent} heard {suffix}")
                    if count != 1:
                        raise AssertionError(
                            f"{name} rendered {agent} reply {suffix} {count} times; "
                            f"expected once\n{screen}"
                        )
                    assert_chat_line(screen, agent, f"{agent} heard {suffix}")
            if "**hostbot**" in screen or "`one`" in screen or "`two`" in screen:
                raise AssertionError(f"{name} retained model Markdown markers:\n{screen}")
            if EMPTY_OUTPUT_CORRECTION in screen:
                raise AssertionError(
                    f"{name} rendered a model-facing output correction:\n{screen}"
                )
            if screen.count("@twoop set topic · shared integration topic") != 1:
                raise AssertionError(f"{name} did not render the topic change once")
            validate_irc_events(terminal.dotdir)
        for name, _model, own, _operator, _args in specs:
            validate_irc_styles(terminals[name], own)

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

        def check_event(kind, nick, text, op, label):
            for terminal in ordered:
                terminal.wait(label)
                matches = [event["data"] for event in
                           event_list(read_events(terminal.dotdir)[1], "irc_event")
                           if event["data"]["kind"] == kind and
                           event["data"]["nick"] == nick and
                           event["data"]["text"] == text and
                           not event["data"]["historical"]]
                assert len(matches) == 1, (kind, nick, text, matches)
                assert matches[0]["op"] is op and matches[0]["room"] == "#lab"
                pattern = rf"(?m)^\d{{2}}:\d{{2}}:\d{{2}} ({re.escape(label)})"
                assert foreground_at(terminal.capture_styled(), pattern) == (36 if op else 34)

        # Exercise real wire events after all viewers have joined; lifecycle
        # fields must agree before the shared renderer can produce equal colors.
        with socket.create_connection(("127.0.0.1", irc_port)) as peer:
            peer.sendall(b"NICK parityop\r\nUSER parityop 0 * :human\r\nJOIN #lab\r\n")
            check_event("join", "parityop", "", False, "· parityop joined")
            terminals["host"].wait("set mode · +o parityop")
            server_name = next(event["data"]["nick"] for event in
                               event_list(read_events(terminals["host"].dotdir)[1], "irc_event")
                               if event["data"]["kind"] == "mode" and
                               event["data"]["text"] == "+o parityop")
            assert server_name != "parityop"
            check_event("mode", server_name, "+o parityop", False,
                        f"· {server_name} set mode · +o parityop")
            peer.sendall(b"PRIVMSG #lab :parity speech\r\nNOTICE #lab :parity notice\r\n"
                         b"TOPIC #lab :parity topic\r\nNICK paritynick\r\n")
            check_event("message", "parityop", "parity speech", True, "@parityop › parity speech")
            check_event("notice", "parityop", "parity notice", True, "-@parityop - parity notice")
            check_event("topic", "parityop", "parity topic", True, "· @parityop set topic")
            check_event("nick", "parityop", "paritynick", True, "· @parityop is now known as")
            peer.sendall(b"MODE #lab -o paritynick\r\nPART #lab :parity parted\r\nJOIN #lab\r\n")
            check_event("mode", "paritynick", "-o paritynick", False,
                        "· paritynick set mode · -o paritynick")
            check_event("part", "paritynick", "parity parted", False, "· paritynick left")
            check_event("join", "paritynick", "", False, "· paritynick joined")
            check_event("mode", server_name, "+o paritynick", False,
                        f"· {server_name} set mode · +o paritynick")
        wait_irc_quits(terminals["host"], ("paritynick",))
        quit_text = next(event["data"]["text"] for event in
                         event_list(read_events(terminals["host"].dotdir)[1], "irc_event")
                         if event["data"]["kind"] == "quit" and event["data"]["nick"] == "paritynick")
        check_event("quit", "paritynick", quit_text, True, "· @paritynick quit")
        wait_irc_idle(ordered)

        with socket.create_connection(("127.0.0.1", irc_port)) as peer:
            peer.sendall(b"CAP REQ :snajpagent/agent\r\nNICK highlightpeer\r\n"
                         b"USER highlightpeer 0 * :agent\r\nCAP END\r\nJOIN #lab\r\n")
            for terminal in ordered:
                terminal.wait("highlightpeer joined")
            peer.sendall(b"PRIVMSG #lab :ordinary palette baseline\r\n")
            for terminal in ordered:
                terminal.wait("ordinary palette baseline")
                pattern = r"(?m)^\d{2}:\d{2}:\d{2} (highlightpeer) › ordinary palette baseline"
                assert foreground_at(terminal.capture_styled(), pattern) == 34
            for target, viewer in (("hostop", "host"), ("oneop", "one"),
                                   ("hostbot", "host"), ("onebot", "one")):
                if viewer == "one":
                    wait_current_prompt(terminals["one"], "oneop")
                    terminals["one"].submit("/rollout")
                    terminals["one"].wait("── rollout ──")
                ending = f"highlight {target} end"
                message = f"@{target.upper()} **highlight start** `code` " + "wrapped message " * 12 + ending
                peer.sendall(f"PRIVMSG #lab :{message}\r\n".encode())
                terminals["host"].wait(ending)
                if viewer == "one":
                    deadline = time.monotonic() + 5.0
                    while not any(event["data"].get("text") == message
                                  for event in event_list(maybe_events(terminals["one"].dotdir)[1], "irc_event")):
                        assert time.monotonic() < deadline, "client did not retain queued highlight"
                        time.sleep(0.02)
                    terminals["one"].submit("/chat")
                for name, terminal in terminals.items():
                    terminal.wait(ending)
                    styled = terminal.capture_styled()
                    # Only the addressed viewer highlights this message's
                    # timestamp and sender; its entire body remains unchanged.
                    assert foreground_at(styled, r"(highlight start)") is None, styled
                    assert foreground_at(styled, r"(code)") == 33, styled
                    prefix = r"(?m)^\d{2}:\d{2}:\d{2} (highlightpeer) › @" + target.upper()
                    assert foreground_at(styled, prefix) == (35 if name == viewer else 34), styled
                    prefix = r"(?m)^(\d{2}:\d{2}:\d{2}) highlightpeer › @" + target.upper()
                    assert foreground_at(styled, prefix) == (35 if name == viewer else None), styled
                    assert foreground_at(styled, f"({ending})") is None, styled
                wait_irc_idle(ordered)
        wait_irc_quits(terminals["host"], ("highlightpeer",))
        wait_irc_idle(ordered)
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
    expected = str(agents)
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
