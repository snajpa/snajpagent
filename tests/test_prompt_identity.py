#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Optional prompt identities follow saved names and accepted IRC endpoints."""
import json
import os
import re
import socket
import sys
import tempfile
import threading
from pathlib import Path

from store_history import journal_paths, read_events
from test_remote_terminal import RemoteProcess
from tmux_terminal import FakeResponses, write_irc_config


class Fixture:
    def __init__(self, binary, root):
        self.binary, self.root = str(binary), root
        self.children = []
        self.provider = FakeResponses()
        self.release = threading.Event()
        self.release.set()

        def respond(handler, request, sequence):
            assert self.release.wait(15), "prompt check did not release provider"
            body = self.provider.response_body(sequence, "identity answer").encode()
            self.provider.reply(handler, body, close_header=True)

        self.provider.runtime_handler = respond
        self.config = root / "config.ini"
        write_irc_config(self.config, self.provider.port, "host-model")
        self.template = ("{operator}|{model_nick}|[{session_name}]|{model}"
                         "{chat: C>}{rollout-idle: I>}{rollout-active: A>}")
        with self.config.open("a") as out:
            out.write("prompt = " + self.template + "\n")

    def start(self, state, *args):
        child = RemoteProcess(self.root,
            [self.binary, "--dotdir", str(state), "--config", str(self.config), *args],
            wrapped=None, extra_env={"SNAJPAGENT_IRC_UI_KEY": "irc-ui-secret"},
            winsize=(30, 240))
        self.children.append(child)
        return child

    def send(self, child, text, marker):
        child.output.clear()
        os.write(child.master, text.encode() + b"\r")
        return child.until(marker.encode(), 10)

    def prompt(self, child, text):
        child.output.clear()
        os.write(child.master, b"\x0c")
        return child.until(text.encode(), 10)

    def finish(self, child):
        self.send(child, "/exit", "--resume")
        child.wait(0)
        child.close()
        self.children.remove(child)

    def close(self):
        self.release.set()
        for child in self.children:
            child.close()
        self.provider.close()


def check_sessions(fixture):
    state = fixture.root / "sessions-case"
    original, renamed = "build {literal} α", "renamed β"
    child = fixture.start(state, "--no-listen", "--no-client", "-o", "human",
                          "-n", "agent", "-N", original)
    child.until(f"human|agent|[{original}]|host-model I>".encode(), 10)
    sid = next((state / "sessions").iterdir()).name
    fixture.release.clear()
    fixture.send(child, "ping", f"human|agent|[{original}]|host-model A>")
    fixture.send(child, "/session name " + renamed, "session name: " + renamed)
    fixture.prompt(child, f"human|agent|[{renamed}]|host-model A>")
    fixture.release.set()
    child.until(b" answer", 10)
    fixture.send(child, "/session name " + "x" * 600,
                 "session name does not fit the configured prompt")
    fixture.prompt(child, f"human|agent|[{renamed}]|host-model I>")
    fixture.finish(child)
    journal = next(p for p in journal_paths(state) if p.parent.name == sid)
    records = read_events(journal)
    names = [r["data"]["name"] for r in records if r["type"] == "session_named"]
    assert names == [original, renamed]
    child = fixture.start(state, "--resume", sid)
    child.until(f"human|agent|[{renamed}]|host-model I>".encode(), 10)
    fixture.finish(child)
    child = fixture.start(fixture.root / "unnamed", "--no-listen", "--no-client",
                          "-o", "human", "-n", "agent")
    child.until(b"human|agent|[]|host-model I>", 10)
    fixture.finish(child)
    # A large stored name must remain valid when the template does not use it.
    text = fixture.config.read_text()
    fixture.config.write_text(text.replace("[{session_name}]", "unused"))
    child = fixture.start(fixture.root / "long-name", "--no-listen", "--no-client",
                          "-o", "human", "-n", "agent", "-N", "x" * 600)
    child.until(b"human|agent|unused|host-model I>", 10)
    fixture.finish(child)
    fixture.config.write_text(text)
    print("prompt saved/renamed/resumed/unnamed identities and label limits: ok", flush=True)


def register(listener, suffix, links):
    for _ in range(2):
        link, _ = listener.accept()
        links.append(link)
        link.settimeout(5)
        wire = bytearray()
        while b"USER " not in wire:
            part = link.recv(4096)
            assert part, "registration closed"
            wire.extend(part)
        nick = re.search(rb"NICK (\w+)\r\n", wire)[1].decode()
        accepted = nick + str(suffix)
        link.sendall((f":fake 001 {accepted} :welcome\r\n"
                      f":fake 005 {accepted} SAJROOM=#lab :supported\r\n"
                      f":fake 376 {accepted} :end\r\n").encode())
        wire.clear()
        while b"JOIN #lab\r\n" not in wire:
            part = link.recv(4096)
            assert part, "join closed"
            wire.extend(part)
        link.sendall((f":{accepted}!u@fake JOIN #lab\r\n"
                      f":fake 353 {accepted} = #lab :@human{suffix} agent{suffix}\r\n"
                      f":fake 366 {accepted} #lab :end\r\n"
                      ":fake BATCH +h chathistory #lab\r\n:fake BATCH -h\r\n").encode())


def check_network(fixture):
    listeners, links = [], []
    try:
        for _ in range(2):
            listener = socket.socket()
            listener.bind(("127.0.0.1", 0))
            listener.listen(2)
            listener.settimeout(10)
            listeners.append(listener)
        endpoints = [f"127.0.0.1:{s.getsockname()[1]}" for s in listeners]
        child = fixture.start(fixture.root / "network", "--no-listen", "-c", endpoints[0],
                              "-c", endpoints[1], "-o", "human", "-n", "agent", "-N", "network")
        child.until(b"|[network]|host-model C>", 10)
        for i, listener in enumerate(listeners, 1):
            register(listener, i, links)
        fixture.send(child, "/names", "model agent2 operator human2")
        fixture.send(child, "/1", "human1|agent1|[network]|host-model C>")
        fixture.send(child, "/2", "human2|agent2|[network]|host-model C>")
        os.write(child.master, b"draft")
        for link in links[2:]:
            link.sendall(b":human2!u@fake NICK :human3\r\n:agent2!u@fake NICK :agent3\r\n")
        child.until(b"agent2 is now known as", 10)
        fixture.prompt(child, "human3|agent3|[network]|host-model C> draft")
        os.write(child.master, b"\x03")
        # IRC rename events can admit a turn; either rollout activity state
        # must use the primary endpoint's identities.
        fixture.send(child, "/rollout", "human1|agent1|[network]|host-model ")
        fixture.send(child, "/chat", "human3|agent3|[network]|host-model C>")
        fixture.finish(child)
        print("prompt accepted nicks, selected endpoints, live rename and draft: ok", flush=True)
    finally:
        for link in links:
            link.close()
        for listener in listeners:
            listener.close()


if __name__ == "__main__":
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="snag-prompt-identity-") as tmp:
        fixture = Fixture(binary, Path(tmp).resolve())
        try:
            check_sessions(fixture)
            check_network(fixture)
        finally:
            fixture.close()
