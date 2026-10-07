# SPDX-License-Identifier: GPL-2.0-only
"""File bytes cross a real tmux server between the wrapper and agent."""
import json
import os
import random
import re
import select
import shlex
import sys
import shutil
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from test_remote_terminal import PRODUCT, RemoteProcess
from test_upload_client import FixtureChildren


@unittest.skipUnless(shutil.which("tmux"), "requires tmux")
class TmuxTransfers(unittest.TestCase):
    def test_remote_tmux_bidirectional(self):
        self.round_trip()

    def test_small_tmux_pane_with_other_output(self):
        self.round_trip(noisy=True)

    def test_cancellation_preserves_files_and_recovers(self):
        self.round_trip(cancel=True)

    @unittest.skipUnless(shutil.which("mosh") and Path("/proc/self/stat").exists(),
                         "requires stock Mosh and Linux process ownership checks")
    def test_stock_mosh_with_remote_tmux(self):
        self.round_trip(mosh=True)

    def test_model_send_file_records_delivery(self):
        self.round_trip(model=True)

    def test_ambiguous_client_refuses_then_recovers(self):
        self.round_trip(ambiguous=True)

    def test_tmux_detach_reattach_uses_new_client_terminal(self):
        self.round_trip(reattach=True)

    def test_local_tmux_contains_workstation_wrapper(self):
        self.round_trip(local=True)

    def test_bracketed_file_drop_preserves_draft(self):
        self.round_trip(drop=True)

    def wait_started(self, child, size):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            child.until(f"/{size} bytes".encode(), 10)
            counts = re.findall(rb"(\d+)/" + str(size).encode() + rb" bytes", child.output)
            if any(0 < int(count) < size for count in counts):
                return
            child.output.clear()
        self.fail("transfer did not start")

    def round_trip(self, noisy=False, cancel=False, mosh=False, model=False,
                   ambiguous=False, reattach=False, local=False, drop=False):
        with tempfile.TemporaryDirectory(prefix="snag-tmux-files-") as tmp:
            root = Path(tmp).resolve()
            client = root / "client"
            client.mkdir()
            config = root / "tmux.conf"
            config.write_text("set -g status off\nset -g history-limit 10000\n")
            sock = root / "socket"
            # The server, not its later client, supplies the pane environment.
            env = dict(os.environ, HOME=str(client), TERM="xterm-256color",
                       SHELL="/bin/sh", PAGER="")
            for key in ("STY", "TMUX", "TMUX_PANE", "OPENAI_API_KEY"):
                env.pop(key, None)
            server = subprocess.Popen(["tmux", "-D", "-S", str(sock), "-f", str(config)],
                                      env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            children = FixtureChildren(server.pid)
            child = None
            mosh_children = None
            second = None
            try:
                deadline = time.monotonic() + 5
                while not sock.exists() and server.poll() is None and time.monotonic() < deadline:
                    time.sleep(.02)
                self.assertTrue(sock.exists(), "tmux server did not start")
                agent = [str(PRODUCT), "--dotdir", str(root / "agent")]
                if local:
                    agent = [str(PRODUCT), "remote", *agent]
                command = ["tmux", "-S", str(sock), "new-session", "-s", "files",
                           "-c", str(client), "env", f"HOME={client}", *agent]
                if mosh:
                    receipt = root / "mosh-server.pid"
                    launcher = root / "start-tmux"
                    launcher.write_text("#!/bin/sh\nprintf '%s\\n' \"$PPID\" > " +
                                        shlex.quote(str(receipt)) + "\nexec " + shlex.join(command) + "\n")
                    launcher.chmod(0o700)
                    command = ["mosh", "--local", "--no-init", "--predict=never",
                               "127.0.0.1", str(launcher)]
                child = RemoteProcess(client, command, wrapped=None if local else True,
                                      server_children=children,
                                      winsize=(10, 48) if noisy else (24, 100))
                if mosh:
                    deadline = time.monotonic() + 10
                    while not receipt.exists():
                        self.assertLess(time.monotonic(), deadline, "Mosh launch receipt missing")
                        child.remember_children()
                        time.sleep(.02)
                    pid = int(receipt.read_text())
                    self.assertIn(str(launcher).encode(), Path(f"/proc/{pid}/cmdline").read_bytes())
                    parent = int(Path(f"/proc/{pid}/stat").read_text().split(") ", 1)[1].split()[1])
                    mosh_children = FixtureChildren(pid, parent=parent)
                    self.assertIn(pid, mosh_children.handles)
                child.until("›".encode(), 20)
                if noisy:
                    chatter = ("import time; n=0\nwhile True:\n "
                               "print('other pane',n,flush=True);n+=1;time.sleep(.01)")
                    subprocess.run(["tmux", "-S", str(sock), "split-window", "-v", "-l", "3",
                                    shlex.join([sys.executable, "-u", "-c", chatter])], check=True)
                    subprocess.run(["tmux", "-S", str(sock), "select-pane", "-t", "%0"], check=True)
                if reattach:
                    subprocess.run(["tmux", "-S", str(sock), "detach-client",
                                    "-t", subprocess.check_output(["tmux", "-S", str(sock), "list-clients",
                                                                   "-F", "#{client_tty}"]).decode().strip()],
                                   check=True)
                    child.wait(0)
                    child.close()
                    child = RemoteProcess(client, ["tmux", "-S", str(sock), "attach", "-t", "files"],
                                          server_children=children, winsize=(24, 100))
                    child.until("›".encode(), 10)
                source = root / "download.bin"
                source.write_bytes(random.Random(0).randbytes(512 if mosh else 65536))
                if ambiguous:
                    second = RemoteProcess(client, ["tmux", "-S", str(sock), "attach", "-t", "files"],
                                           wrapped=None, server_children=children, winsize=(24, 100))
                    second.until("›".encode(), 10)
                    os.write(child.master, f"/send {source}\r".encode())
                    child.until(b"tmux file transfers require one writable client", 10)
                    self.assertFalse((client / "Downloads/download.bin").exists())
                    subprocess.run(["tmux", "-S", str(sock), "detach-client",
                                    "-t", os.ttyname(second.slave)], check=True)
                    second.wait(0)
                    second.close()
                    second = None
                    child.output.clear()
                started = time.monotonic()
                os.write(child.master, f"/send {source}\r".encode())
                target = client / "Downloads" / source.name
                child.until(b"Client acknowledged the file digest and final EXIT.", 40)
                self.assertEqual(target.read_bytes(), source.read_bytes())
                print(f"download: {time.monotonic() - started:.3f}s", flush=True)
                started = time.monotonic()
                if drop:
                    os.write(child.master, b"ping")
                    child.until(b"ping", 10)
                else:
                    os.write(child.master, b"/receive\r")
                    child.until(b"Select local file", 10)
                upload = client / "upload 'résumé'.bin"
                upload.write_bytes(random.Random(1).randbytes(512 if mosh else 65536))
                if drop:
                    os.write(child.master, b"\x1b[200~" + str(upload).encode() + b"\x1b[201~")
                else:
                    os.write(child.master, f"{upload}\r".encode())
                # The small pane can clip the listing's leading banner.
                child.until(b"[1 attached]", 40)
                media = next((root / "agent/sessions").glob("*/media"))
                self.assertTrue(any(path.read_bytes() == upload.read_bytes() for path in media.iterdir()))
                print(f"upload: {time.monotonic() - started:.3f}s", flush=True)
                for marker in (b"#DATA:", b"#CFG:", b"::TRZSZ:TRANSFER:", b"SNAJPAGENT-SCREEN/1"):
                    self.assertNotIn(marker, child.output)
                pane = subprocess.check_output(["tmux", "-S", str(sock), "capture-pane", "-p", "-S", "-"])
                for marker in (b"#DATA:", b"#CFG:", b"::TRZSZ:TRANSFER:", b"SNAJPAGENT-SCREEN/1"):
                    self.assertNotIn(marker, pane)
                if drop:
                    os.write(child.master, b"\r")
                    child.until(b"pong", 10)
                if model:
                    child.output.clear()
                    tool = client / "tool.bin"
                    tool.write_bytes(random.Random(3).randbytes(1024))
                    os.write(child.master, f"download_tool {tool}\r".encode())
                    child.until(str(client / "Downloads/tool.bin").encode(), 20)
                    journal = next((root / "agent/sessions").glob("*/events.jsonl"))
                    deadline = time.monotonic() + 10
                    while not any(json.loads(line)["type"] == "turn_completed"
                                  for line in journal.read_text().splitlines(keepends=True)
                                  if line.endswith("\n")):
                        self.assertLess(time.monotonic(), deadline)
                        child.remember_children()
                        if select.select([child.master], [], [], .05)[0]:
                            child.output.extend(os.read(child.master, 65536))
                    self.assertTrue((client / "Downloads/tool.bin").exists(), bytes(child.output))
                    self.assertEqual((client / "Downloads/tool.bin").read_bytes(), tool.read_bytes())
                    events = [json.loads(line) for line in
                              next((root / "agent/sessions").glob("*/events.jsonl")).read_text().splitlines()]
                    self.assertEqual(sum(e["type"] == "download_queued" for e in events), 1)
                    self.assertEqual(sum(e["type"] == "download_removed" for e in events), 1)
                if cancel:
                    large = client / "cancel.bin"
                    large.write_bytes(random.Random(2).randbytes(2 * 1024 * 1024))
                    for direction in ("download", "upload"):
                        child.output.clear()
                        if direction == "download":
                            os.write(child.master, f"/send {large}\r".encode())
                        else:
                            os.write(child.master, b"/receive\r")
                            child.until(b"Select local file", 10)
                            os.write(child.master, f"{large}\r".encode())
                        self.wait_started(child, large.stat().st_size)
                        os.write(child.master, b"\x03")
                        child.until_after(b"cancelled", "›".encode(), 10)
                        self.assertEqual(list((client / "Downloads").iterdir()), [target])
                        self.assertEqual([path.read_bytes() for path in media.iterdir()],
                                         [upload.read_bytes()])
                        for marker in (b"#DATA:", b"#CFG:", b"SNAJPAGENT-SCREEN/1"):
                            self.assertNotIn(marker, child.output)
                    recovery = client / "recovery.bin"
                    recovery.write_bytes(b"after cancellation" * 128)
                    child.output.clear()
                    os.write(child.master, f"/send {recovery}\r".encode())
                    landed = client / "Downloads" / recovery.name
                    child.until(str(landed).encode(), 10)
                    self.assertEqual(landed.read_bytes(), recovery.read_bytes())
                os.write(child.master, b"ping\r")
                child.until(b"pong", 10)
                os.write(child.master, b"/exit\r")
                if noisy:
                    deadline = time.monotonic() + 5
                    while time.monotonic() < deadline:
                        panes = subprocess.check_output(
                            ["tmux", "-S", str(sock), "list-panes", "-F", "#{pane_id}"])
                        if b"%0" not in panes.splitlines():
                            break
                        time.sleep(.02)
                    self.assertNotIn(b"%0", panes.splitlines(), "agent did not exit")
                    subprocess.run(["tmux", "-S", str(sock), "kill-pane", "-t", "%1"], check=True)
                child.wait(0)
            finally:
                if second:
                    second.close()
                if child:
                    child.close()
                if mosh_children:
                    mosh_children.close()
                children.remember()
                subprocess.run(["tmux", "-S", str(sock), "kill-server"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
                children.close()
                if server.poll() is None:
                    server.terminate()
                server.wait(timeout=5)
                server.stderr.close()


if __name__ == "__main__":
    unittest.main()
