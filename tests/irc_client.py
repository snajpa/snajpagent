# SPDX-License-Identifier: GPL-2.0-only
"""Minimal shared IRC peer for terminal integration fixtures."""
import select
import socket
import time

MIN_WAIT_S = 30.0


class IRCClient:
    def __init__(self, port, nick, agent=False):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=4.0)
        self.buf = bytearray()
        role = b" snajpagent/agent" if agent else b""
        registration = (
            b"CAP LS 302\r\nCAP REQ :batch server-time draft/chathistory" +
            role + b"\r\nCAP END\r\nNICK " + nick.encode() +
            b"\r\nUSER " + nick.encode() + b" 0 * :PTY peer\r\nJOIN #lab\r\n"
        )
        self.sock.sendall(registration)
        self.sock.setblocking(False)
        self.wait(b" 366 " + nick.encode() + b" #lab ")

    def wait(self, needle, start=0, timeout=MIN_WAIT_S):
        end = time.monotonic() + timeout
        while needle not in self.buf[start:]:
            remaining = end - time.monotonic()
            if remaining <= 0:
                raise AssertionError(
                    f"timeout waiting for IRC {needle!r}; got {bytes(self.buf)!r}"
                )
            ready, _, _ = select.select([self.sock], [], [], remaining)
            if not ready:
                continue
            try:
                chunk = self.sock.recv(65536)
            except BlockingIOError:
                continue
            if not chunk:
                raise AssertionError(
                    f"IRC socket closed waiting for {needle!r}; "
                    f"got {bytes(self.buf)!r}"
                )
            self.buf.extend(chunk)
        return self.buf.find(needle, start) + len(needle)

    def message(self, text):
        self.sock.sendall(b"PRIVMSG #lab :" + text.encode() + b"\r\n")

    def drain(self, duration=0.25):
        end = time.monotonic() + duration
        while time.monotonic() < end:
            ready, _, _ = select.select(
                [self.sock], [], [], min(0.05, end - time.monotonic())
            )
            if not ready:
                continue
            try:
                chunk = self.sock.recv(65536)
            except BlockingIOError:
                continue
            if not chunk:
                return
            self.buf.extend(chunk)

    def close(self):
        self.sock.close()
