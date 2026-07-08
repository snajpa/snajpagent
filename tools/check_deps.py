#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Validate the source dependency/vendoring policy."""

from __future__ import annotations

import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
FORBIDDEN_VENDOR_DIRS = {
    "vendor",
    "vendors",
    "third_party",
    "third-party",
    "external",
    "submodules",
}
ALLOWED_JANSSON_INCLUDE = '#include "snj_jansson.h"'
WRAPPER = ROOT / "src" / "snj_jansson.h"
ABI = ROOT / "src" / "snj_jansson_abi.h"
DOC = ROOT / "DEPENDENCIES.md"


def rel(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


def fail(message: str) -> None:
    raise SystemExit(f"depscheck: {message}")


def source_files() -> list[Path]:
    files: list[Path] = []
    for base in (ROOT / "src", ROOT / "tests"):
        for path in base.rglob("*"):
            if path.is_file() and path.suffix in {".c", ".h"}:
                files.append(path)
    return sorted(files)


def check_no_vendor_dirs() -> None:
    for current, dirs, _files in os.walk(ROOT):
        current_path = Path(current)
        if any(part in {".git", "__pycache__", ".pytest_cache"} for part in current_path.parts):
            dirs[:] = []
            continue
        for name in list(dirs):
            if name in FORBIDDEN_VENDOR_DIRS:
                fail(f"forbidden vendored-dependency directory present: {rel(current_path / name)}")


def check_jansson_policy() -> None:
    if (ROOT / "src" / "jansson.h").exists():
        fail("src/jansson.h would shadow a system <jansson.h>; use snj_jansson.h wrapper")
    if not WRAPPER.is_file():
        fail("missing src/snj_jansson.h wrapper")
    if not ABI.is_file():
        fail("missing src/snj_jansson_abi.h ABI declaration shim")
    abi_text = ABI.read_text(encoding="utf-8")
    if "First-party declaration shim" not in abi_text or "contains no implementation" not in abi_text:
        fail("Jansson ABI shim is not explicitly inventoried as declarations-only")
    wrapper_text = WRAPPER.read_text(encoding="utf-8")
    if '#include "snj_jansson_abi.h"' not in wrapper_text:
        fail("Jansson wrapper does not contain the local ABI fallback")
    for path in source_files():
        text = path.read_text(encoding="utf-8", errors="surrogateescape")
        if path == WRAPPER:
            continue
        if re.search(r"#\s*include\s*<jansson\.h>", text):
            fail(f"raw <jansson.h> include outside wrapper: {rel(path)}")
        if "snj_jansson_abi.h" in text and path != ABI:
            fail(f"direct ABI shim include outside wrapper: {rel(path)}")


def check_curl_policy() -> None:
    for path in source_files():
        text = path.read_text(encoding="utf-8", errors="surrogateescape")
        if re.search(r"#\s*include\s*<curl/", text) and rel(path) != "src/provider.c":
            fail(f"libcurl must stay behind provider.c boundary: {rel(path)}")


def check_doc() -> None:
    if not DOC.is_file():
        fail("missing DEPENDENCIES.md")
    text = DOC.read_text(encoding="utf-8")
    required = [
        "Vendored third-party implementation source: none",
        "Vendored third-party header source: none",
        "src/snj_jansson_abi.h",
        "system libcurl",
        "system Jansson",
        "make depscheck",
        "make depclosurecheck",
    ]
    for marker in required:
        if marker not in text:
            fail(f"DEPENDENCIES.md missing required marker: {marker}")


def main() -> int:
    check_no_vendor_dirs()
    check_jansson_policy()
    check_curl_policy()
    check_doc()
    print("depscheck: ok (no vendored third-party source; system libcurl/Jansson policy inventoried)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
