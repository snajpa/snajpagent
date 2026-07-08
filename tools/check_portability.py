#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Static portability guard for advertised Linux/macOS terminal/process surfaces."""

from __future__ import annotations

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


def die(message: str) -> None:
    raise SystemExit(f"portabilitycheck: {message}")


def require(condition: bool, message: str) -> None:
    if not condition:
        die(message)


def main() -> int:
    tools = (ROOT / "src" / "tools.c").read_text(encoding="utf-8")
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
    require(tools.count("#if defined(SNAJPAGENT_HAVE_PTY)\n            exec_pty_child") == 2,
            "both immediate and yielded PTY child paths must use the capability guard")
    require(tools.count("#if defined(__linux__)") == 1,
            "Linux-only preprocessor guards must not wrap PTY behavior outside the include/capability branch")
    require("-D_POSIX_C_SOURCE=200809L" in config and "-D_XOPEN_SOURCE=700" in config,
            "POSIX/XOPEN feature macros must remain enabled")
    require("Linux" in status and "macOS" in status,
            "implementation status must name both advertised platforms")
    require("External evidence still required" in qualification,
            "qualification ledger must keep external evidence gap visible")

    print("portabilitycheck: ok (PTY capability covers Linux and macOS; external evidence still explicit)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
