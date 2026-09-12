#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Citation blocks stay byte-exact on redirected output and in durable events.

Terminal presentation of the same text is covered by the tmux fixture case.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

BINARY = os.path.abspath(sys.argv[1])
OPEN, CLOSE, SEP = "\ue200", "\ue201", "\ue202"
PROMPT = "citation_markers"
RAW = f"citations: {OPEN}cite{SEP}turn2view0{SEP}turn0view3{CLOSE} tail"


def main():
    with tempfile.TemporaryDirectory(prefix="snajpagent-citations-") as root:
        root = Path(root)
        (root / "home").mkdir(mode=0o700)
        (root / "work").mkdir()
        dotdir = root / "state"
        environment = dict(os.environ, HOME=str(root / "home"),
                           LC_ALL="C.utf8", NO_COLOR="1")
        environment.pop("OPENAI_API_KEY", None)
        run = subprocess.run([BINARY, "--dotdir", str(dotdir), "-e", "--", PROMPT],
                             cwd=root / "work", env=environment,
                             capture_output=True, text=True, timeout=60)
        assert run.returncode == 0, run.stderr
        assert run.stdout.strip() == RAW, repr(run.stdout)
        logs = list(dotdir.glob("sessions/*/events.jsonl"))
        assert len(logs) == 1, logs
        assert OPEN in logs[0].read_text(encoding="utf-8")
        print("test_citations: ok")


if __name__ == "__main__":
    main()
