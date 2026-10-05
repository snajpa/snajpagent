# SPDX-License-Identifier: GPL-2.0-only
"""A completed paste renders without a subsequent key or provider output."""
import os
import re
import select
import time
import tempfile
import unittest
from pathlib import Path

import test_remote_terminal as remote

PRODUCT = Path(os.environ.get("SNAJPAGENT_PASTE_BINARY", str(remote.PRODUCT))).resolve()
remote.PRODUCT = PRODUCT


class PasteDisplay(unittest.TestCase):
    def test_completed_paste_is_visible_without_another_key(self):
        cases = (b"paste appears immediately", "first line\nžluťoučký paste finished".encode(),
                 b"long paste " * 1000 + b"PASTE_COMPLETE_SENTINEL")
        for wrapped in (False, True):
            for payload in cases:
                with self.subTest(wrapped=wrapped, size=len(payload)), \
                        tempfile.TemporaryDirectory(prefix="snag-paste-display-") as tmp:
                    root = Path(tmp).resolve()
                    config = root / "config.ini"
                    config.write_text(
                        "[agent]\nmodel = fake/test\nread_agents_md = false\n"
                        "[provider fake]\nbase_url = http://127.0.0.1:1/v1\n"
                        "api_key = ${SNAJPAGENT_PASTE_KEY}\nexact_token_count = false\n"
                        "native_compaction = false\n[ui]\ncolor = never\n")
                    arguments = ["--config", str(config), "--dotdir", str(root / "agent")]
                    if wrapped:
                        arguments.insert(0, str(PRODUCT))
                    child = remote.RemoteProcess(root, arguments, wrapped=wrapped,
                                                 extra_env={"SNAJPAGENT_PASTE_KEY": "fixture"},
                                                 winsize=(24, 100))
                    try:
                        child.until("›".encode(), 8)
                        child.output.clear()
                        # One write includes the terminator. Send nothing else
                        # until the final pasted words have appeared.
                        os.write(child.master, b"\x1b[200~" + payload + b"\x1b[201~")
                        marker = payload.split(b"\n")[-1][-20:]
                        deadline = time.monotonic() + 3
                        while marker not in re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", child.output):
                            self.assertLess(time.monotonic(), deadline, bytes(child.output[-500:]))
                            child.remember_children()
                            if select.select([child.master], [], [], .05)[0]:
                                child.output.extend(os.read(child.master, 65536))
                        os.write(child.master, b"\x15/exit\r")
                        child.until(b"--resume", 8)
                        child.wait(0)
                    finally:
                        child.close()


if __name__ == "__main__":
    unittest.main()
