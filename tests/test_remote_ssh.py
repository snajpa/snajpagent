# SPDX-License-Identifier: GPL-2.0-only
"""Native remote wrapper through disposable loopback OpenSSH and GNU screen."""
import json
import os
import pwd
import select
import shlex
import shutil
import socket
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path

from test_remote_terminal import PRODUCT, RemoteProcess
from test_upload_client import FixtureChildren


class BlackholeProxy:
    """Drop bytes without closing TCP, as when an SSH network path disappears."""
    def __init__(self, destination):
        self.blocked = threading.Event()
        self.stopped = threading.Event()
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.destination = destination
        self.thread = threading.Thread(target=self.forward, daemon=True)
        self.thread.start()

    def forward(self):
        while not self.stopped.is_set():
            if select.select([self.listener], [], [], .1)[0]:
                break
        else:
            return
        with self.listener.accept()[0] as client, socket.create_connection(self.destination) as server:
            sockets = (client, server)
            while not self.stopped.is_set():
                for source in select.select(sockets, [], [], .1)[0]:
                    data = source.recv(65536)
                    if not data:
                        return
                    if not self.blocked.is_set():
                        (server if source is client else client).sendall(data)

    def close(self):
        self.stopped.set()
        self.thread.join(2)
        self.listener.close()
        assert not self.thread.is_alive(), "blackhole proxy did not stop"


@unittest.skipUnless(shutil.which("sshd") and shutil.which("ssh-keygen") and
                     shutil.which("screen") and os.getuid() == 0 and Path("/run/sshd").is_dir(),
                     "isolated sshd fixture needs root, OpenSSH, GNU screen and /run/sshd")
class RemoteSSHTests(unittest.TestCase):
    def test_first_nested_ssh_and_remote_screen(self):
        with tempfile.TemporaryDirectory(prefix="snag-remote-ssh-") as tmp:
            root = Path(tmp)
            home = root / "client"
            home.mkdir()
            for name in ("host", "client-key"):
                subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "",
                                "-f", str(root / name)], check=True, timeout=10)
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            cfg = root / "sshd.conf"
            cfg.write_text(f"Port {port}\nListenAddress 127.0.0.1\n"
                           f"HostKey {root / 'host'}\nPidFile {root / 'sshd.pid'}\n"
                           f"AuthorizedKeysFile {root / 'client-key.pub'}\n"
                           "PermitRootLogin yes\nPasswordAuthentication no\n"
                           "KbdInteractiveAuthentication no\nUsePAM no\n"
                           "StrictModes no\nAllowTcpForwarding no\nX11Forwarding no\n"
                           "ClientAliveInterval 1\nClientAliveCountMax 2\n")
            host_key = (root / "host.pub").read_text().split()
            known = root / "known_hosts"
            known.write_text(f"[127.0.0.1]:{port} {host_key[0]} {host_key[1]}\n")
            log = open(root / "sshd.log", "wb")
            server = subprocess.Popen([shutil.which("sshd"), "-D", "-e", "-f", str(cfg)],
                                      stdout=log, stderr=log)
            server_children = FixtureChildren(server.pid)
            pinned = []
            tmux_servers = []
            ssh = ["ssh", "-tt", "-p", str(port), "-i", str(root / "client-key"),
                   "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes",
                   "-o", "StrictHostKeyChecking=yes", "-o", f"UserKnownHostsFile={known}",
                   "-o", "GlobalKnownHostsFile=/dev/null",
                   f"{pwd.getpwuid(os.getuid()).pw_name}@127.0.0.1"]
            try:
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    if server.poll() is not None:
                        self.fail((root / "sshd.log").read_text())
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                            break
                    except OSError:
                        time.sleep(0.05)
                else:
                    self.fail("private sshd did not listen")
                scenarios = ["first", "screen", "nested"]
                if shutil.which("tmux"):
                    scenarios.append("tmux")
                if server_children.handles:
                    scenarios.extend(("lost-client", "blackhole"))
                for scenario in scenarios:
                    with self.subTest(scenario=scenario):
                        state = root / scenario
                        state.mkdir()
                        source = state / "report.bin"
                        source.write_bytes(bytes(range(256)) * 300)
                        dotdir = state / "agent"
                        # SSH starts a fresh environment on the server side.
                        # Transfer reports stay inline in this protocol fixture.
                        command = ["env", f"HOME={state}", "LC_ALL=C.UTF-8", "PAGER=",
                                   str(PRODUCT), "--dotdir", str(dotdir)]
                        screen_env = None
                        if scenario == "tmux":
                            tmux_socket = state / "tmux.sock"
                            tmux_config = state / "tmux.conf"
                            tmux_config.write_text("set -g status off\n")
                            tmux = subprocess.Popen(["tmux", "-D", "-S", str(tmux_socket),
                                                     "-f", str(tmux_config)], stdout=log, stderr=log)
                            tmux_servers.append(tmux)
                            if server_children.handles:
                                server_children.pin(tmux.pid, os.getpid())
                            deadline = time.monotonic() + 5
                            while not tmux_socket.exists():
                                self.assertIsNone(tmux.poll())
                                self.assertLess(time.monotonic(), deadline)
                                time.sleep(.02)
                            command = ["env", "LC_ALL=C.UTF-8", "tmux", "-u", "-S", str(tmux_socket), "new-session",
                                       *command]
                        if scenario == "screen":
                            sockets = state / "screens"
                            sockets.mkdir(mode=0o700)
                            screenrc = state / "screenrc"
                            screenrc.write_text("startup_message off\naltscreen off\n")
                            screen_env = dict(os.environ, SCREENDIR=str(sockets))
                            command = ["env", f"SCREENDIR={sockets}", "LC_ALL=C.UTF-8", "screen", "-U", "-c",
                                       str(screenrc), "-S", "ssh-native", *command]
                        remote = "cd " + shlex.quote(str(state)) + "; exec " + shlex.join(command)
                        if scenario == "nested":
                            remote = shlex.join([*ssh, remote])
                        proxy = BlackholeProxy(("127.0.0.1", port)) if scenario == "blackhole" else None
                        connection = list(ssh)
                        if proxy:
                            connection[connection.index("-p") + 1] = str(proxy.port)
                            connection[-1:-1] = ["-o", f"HostKeyAlias=[127.0.0.1]:{port}"]
                        child = RemoteProcess(home, [*connection, remote], server_children=server_children)
                        try:
                            child.until("›".encode(), 10)
                            target = home / "Downloads" / source.name
                            if target.exists():
                                target.unlink()
                            os.write(child.master, f"download_tool {source}\r".encode())
                            child.until(str(target).encode(), 15)
                            self.assertEqual(target.read_bytes(), source.read_bytes())
                            journal = next((dotdir / "sessions").glob("*/events.jsonl"))
                            events = [json.loads(line) for line in journal.read_text().splitlines()]
                            self.assertEqual(sum(e["type"] == "download_removed" for e in events), 1)
                            if scenario in ("lost-client", "blackhole"):
                                # Transfer acknowledgement precedes turn completion. Starting
                                # the next command earlier admits steering, not a second turn.
                                deadline = time.monotonic() + 10
                                while True:
                                    events = [json.loads(line) for line in
                                              journal.read_text().splitlines(keepends=True)
                                              if line.endswith("\n")]
                                    if sum(e["type"] == "turn_completed" for e in events) == 1:
                                        break
                                    self.assertLess(time.monotonic(), deadline,
                                                    "download turn did not complete")
                                    child.remember_children()
                                    if select.select([child.master], [], [], .05)[0]:
                                        child.output.extend(os.read(child.master, 65536))
                                started = state / "native-process-started"
                                finished = state / "native-process-finished"
                                os.write(child.master, b"native_attachment_process\r")
                                deadline = time.monotonic() + 10
                                while not started.exists():
                                    if time.monotonic() >= deadline:
                                        pending = [json.loads(line) for line in
                                                   journal.read_text().splitlines()]
                                        facts = [(event["type"], event["data"].get("text"),
                                                  event["data"].get("prompt"),
                                                  event["data"].get("cwd")) for event in pending]
                                        self.fail(f"remote command did not start: {facts!r}")
                                    child.remember_children()
                                    time.sleep(0.02)
                                self.assertFalse(finished.exists(),
                                                 "command finished before SSH disconnected")
                                server_children.remember()
                                pinned += [os.dup(fd) for pid, fd in server_children.handles.items()
                                          if pid != server.pid]
                                self.assertTrue(pinned, "private server descendants were not pinned")
                                if proxy:
                                    log_start = (root / "sshd.log").stat().st_size
                                    proxy.blocked.set()
                                    deadline = time.monotonic() + 10
                                    while b"Timeout, client not responding" not in (
                                            root / "sshd.log").read_bytes()[log_start:]:
                                        self.assertLess(time.monotonic(), deadline,
                                                        "dead SSH connection retained its terminal")
                                        time.sleep(.05)
                                else:
                                    child.close()
                                self.assertLess(len(select.select(pinned, [], [], 0)[0]), len(pinned),
                                                "lost client did not leave a surviving remote owner")
                                deadline = time.monotonic() + 15
                                while True:
                                    events = [json.loads(line) for line in
                                              journal.read_text().splitlines(keepends=True)
                                              if line.endswith("\n")]
                                    if sum(e["type"] == "turn_completed" for e in events) == 2:
                                        break
                                    self.assertLess(time.monotonic(), deadline,
                                                    "remote work stopped after SSH disconnected")
                                    time.sleep(0.05)
                                self.assertEqual(finished.read_text(), "completed")
                                reconnect = shlex.join([*command, "--attach", journal.parent.name])
                                attached = RemoteProcess(home, [*ssh, reconnect],
                                                         server_children=server_children)
                                try:
                                    attached.until(b"Attached session", 10)
                                    attached.until(b"native process complete", 10)
                                    os.write(attached.master, b"/exit\r")
                                    attached.wait(0)
                                finally:
                                    attached.close()
                                events = [json.loads(line) for line in
                                          journal.read_text().splitlines()]
                                calls = [item for event in events
                                         if event["type"] == "response_completed"
                                         for item in event["data"]["items"]
                                         if item["kind"] == "tool_call"]
                                self.assertEqual(sum(c["name"] == "exec_command"
                                                     for c in calls), 1)
                                self.assertEqual(sum(e["type"] == "turn_started"
                                                     for e in events), 2)
                                self.assertFalse([e for e in events if e["type"] in
                                                  ("turn_interrupted", "response_interrupted",
                                                   "turn_recovery", "turn_failed")])
                            else:
                                if scenario == "tmux":
                                    upload = home / "ssh-tmux-upload.bin"
                                    upload.write_bytes(bytes(reversed(range(256))) * 256)
                                    os.write(child.master, b"/receive\r")
                                    child.until(b"Select local file", 10)
                                    os.write(child.master, f"{upload}\r".encode())
                                    child.until(b"1 unsent attachment(s)", 15)
                                    media = journal.parent / "media"
                                    self.assertTrue(any(p.read_bytes() == upload.read_bytes()
                                                        for p in media.iterdir()))
                                os.write(child.master, b"/exit\r")
                                child.wait(0)
                        finally:
                            if screen_env:
                                subprocess.run(["screen", "-S", "ssh-native", "-X", "quit"],
                                               env=screen_env, timeout=5, stdout=subprocess.DEVNULL,
                                               stderr=subprocess.DEVNULL)
                            child.close()
                            if proxy:
                                proxy.close()
            finally:
                try:
                    server_children.close()
                    if server.poll() is None:
                        server.terminate()
                    server.wait(timeout=5)
                    self.assertEqual(len(select.select(pinned, [], [], 0)[0]), len(pinned),
                                     "private SSH descendants survived fixture shutdown")
                finally:
                    server_children.close()
                    for fd in pinned:
                        os.close(fd)
                    for tmux in tmux_servers:
                        if tmux.poll() is None:
                            tmux.terminate()
                        tmux.wait(timeout=5)
                    log.close()


if __name__ == "__main__":
    unittest.main()
