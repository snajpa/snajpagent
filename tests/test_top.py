# SPDX-License-Identifier: GPL-2.0-only
"""The non-interactive top view remains usable in redirected output."""

import subprocess
import sys
import tempfile
from pathlib import Path



def main():
    product = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="snag-top-") as temporary:
        dotdir = Path(temporary).resolve() / "dotdir"
        result = subprocess.run(
            [str(product), "top", "--dotdir", str(dotdir), "--delay", "1", "--color", "never"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=10,
            check=False,
        )
    assert result.returncode == 0, result.stderr.decode(errors="replace")
    assert b"\0" not in result.stdout
    output = result.stdout.decode()
    assert "snajpagent top" in output
    assert "SESSION  STATE" in output
    assert "TOTAL sessions=0 turns=0 process=0 session=0" in output
    assert result.stderr == b"", result.stderr.decode(errors="replace")


if __name__ == "__main__":
    main()
