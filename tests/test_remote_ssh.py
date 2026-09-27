# SPDX-License-Identifier: GPL-2.0-only
"""Native remote wrapper through disposable loopback OpenSSH and GNU screen."""
import json
import os
import pwd
import shlex
import shutil
import socket
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from test_remote_terminal import PRODUCT, RemoteProcess


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
                           "StrictModes no\nAllowTcpForwarding no\nX11Forwarding no\n")
            host_key = (root / "host.pub").read_text().split()
            known = root / "known_hosts"
            known.write_text(f"[127.0.0.1]:{port} {host_key[0]} {host_key[1]}\n")
            log = open(root / "sshd.log", "wb")
            server = subprocess.Popen([shutil.which("sshd"), "-D", "-e", "-f", str(cfg)],
                                      stdout=log, stderr=log)
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
                for scenario in ("first", "screen", "nested"):
                    with self.subTest(scenario=scenario):
                        state = root / scenario
                        state.mkdir()
                        source = state / "report.bin"
                        source.write_bytes(bytes(range(256)) * 300)
                        dotdir = state / "agent"
                        command = ["env", "LC_ALL=C.UTF-8", str(PRODUCT), "--dotdir", str(dotdir)]
                        screen_env = None
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
                        child = RemoteProcess(home, [*ssh, remote])
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
                            os.write(child.master, b"/exit\r")
                            child.wait(0)
                        finally:
                            if screen_env:
                                subprocess.run(["screen", "-S", "ssh-native", "-X", "quit"],
                                               env=screen_env, timeout=5, stdout=subprocess.DEVNULL,
                                               stderr=subprocess.DEVNULL)
                            child.close()
            finally:
                server.terminate()
                server.wait(timeout=5)
                log.close()


if __name__ == "__main__":
    unittest.main()
