#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Static portability guard for advertised Linux/macOS terminal/process surfaces."""

from __future__ import annotations

from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def die(message: str) -> None:
    raise SystemExit(f"portabilitycheck: {message}")


def require(condition: bool, message: str) -> None:
    if not condition:
        die(message)


def check_configure() -> None:
    """The host entry point is POSIX sh; keep it that way on every platform."""
    script = ROOT / "configure"
    if not script.exists():
        return
    text = script.read_text(encoding="utf-8")
    require(text.startswith("#!/bin/sh\n"),
            "configure must name the POSIX shell in its shebang")
    for number, line in enumerate(text.splitlines(), 1):
        if line.lstrip().startswith("#"):
            continue
        for token in ("[[", "<<<", "pipefail", "$RANDOM", "mapfile", "declare ", "function "):
            require(token not in line,
                    f"configure:{number} uses the non-POSIX construct {token!r}")
    for shell in ("sh", "dash"):
        binary = shutil.which(shell)
        if binary is None:
            continue
        result = subprocess.run([binary, "-n", str(script)],
                                capture_output=True, text=True, check=False)
        require(result.returncode == 0,
                f"configure must parse under {shell}: {result.stderr.strip()}")


def main() -> int:
    tools = (ROOT / "src" / "process_host.c").read_text(encoding="utf-8")
    config = (ROOT / "config.mk").read_text(encoding="utf-8")
    status = (ROOT / "IMPLEMENTATION_STATUS.md").read_text(encoding="utf-8")
    qualification = (ROOT / "QUALIFICATION.md").read_text(encoding="utf-8")

    require("#define SNAJPAGENT_HAVE_PTY 1" in tools,
            "PTY capability macro is missing")
    require(re.search(r"#if defined\(__linux__\).*?#define SNAJPAGENT_HAVE_PTY 1.*?#include <pty\.h>",
                      tools, re.DOTALL) is not None,
            "Linux PTY branch must define capability and include <pty.h>")
    require(re.search(r"#elif defined\(__APPLE__\).*?#define SNAJPAGENT_HAVE_PTY 1.*?#include <sys/ioctl\.h>.*?#include <util\.h>",
                      tools, re.DOTALL) is not None,
            "macOS PTY branch must define capability and include <util.h>")
    require("#if defined(SNAJPAGENT_HAVE_PTY)\nstatic void\nhost_winsize" in tools,
            "PTY helper implementation must be guarded by capability, not by one OS")
    require(tools.count("#if defined(SNAJPAGENT_HAVE_PTY)\n            exec_pty_child") == 1,
            "the unified managed PTY child path must use the capability guard")
    require(tools.count("#if defined(__linux__)") == 1,
            "Linux-only preprocessor guards must not wrap PTY behavior outside the include/capability branch")
    require("-D_POSIX_C_SOURCE=200809L" in config and "-D_XOPEN_SOURCE=700" in config,
            "POSIX/XOPEN feature macros must remain enabled")
    require("Linux" in status and "macOS" in status,
            "implementation status must name both advertised platforms")
    require("External evidence still required" in qualification,
            "qualification ledger must keep external evidence gap visible")

    check_configure()

    print("portabilitycheck: ok (PTY capability covers Linux and macOS; external evidence still explicit)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
