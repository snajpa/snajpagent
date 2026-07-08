#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Capture and validate the dynamic dependency closure of a built binary.

The release archive intentionally does not vendor libcurl or Jansson.  This
check is the machine-readable counterpart to that policy: for the executable
that will be qualified on a host, record the concrete dynamic libraries selected
by the platform loader and fail if the required provider-capable dependencies are
missing or unresolved.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
from typing import Any


TLS_MARKERS = (
    "libssl", "libcrypto", "gnutls", "mbedtls", "wolfssl", "libnss",
    "securetransport", "schannel",
)
RESOLVER_MARKERS = ("libcares", "libresolv", "libidn", "libidn2", "libpsl")
COMPRESSION_MARKERS = ("libz", "zlib", "libzstd", "libbrotli")
HTTP_MARKERS = ("libnghttp2", "libngtcp2", "libnghttp3", "libcurl")


def die(message: str) -> None:
    raise SystemExit(f"depclosurecheck: {message}")


def run(argv: list[str]) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(argv, text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, check=False)
    except OSError as exc:
        die(f"could not run {' '.join(argv)}: {exc}")


def tool_path(name: str) -> str:
    found = shutil.which(name)
    if not found:
        die(f"required tool not found in PATH: {name}")
    return found


def basename(text: str) -> str:
    return Path(text).name if "/" in text else text


def classify(deps: list[dict[str, str]], markers: tuple[str, ...]) -> list[str]:
    out: list[str] = []
    for dep in deps:
        haystack = f"{dep.get('name', '')} {dep.get('path', '')}".lower()
        if any(marker.lower() in haystack for marker in markers):
            label = dep.get("path") or dep.get("name")
            if label not in out:
                out.append(label)
    return sorted(out)


def has_dep(deps: list[dict[str, str]], needle: str) -> bool:
    needle = needle.lower()
    for dep in deps:
        if needle in dep.get("name", "").lower():
            return True
        if needle in dep.get("path", "").lower():
            return True
    return False


def parse_ldd(text: str) -> tuple[list[dict[str, str]], list[str]]:
    deps: list[dict[str, str]] = []
    unresolved: list[str] = []
    seen: set[tuple[str, str]] = set()
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if "not found" in line:
            unresolved.append(line)
            continue
        name = ""
        path = ""
        if "=>" in line:
            left, right = line.split("=>", 1)
            name = left.strip()
            token = right.strip().split()[0] if right.strip().split() else ""
            if token.startswith("/"):
                path = token
        else:
            token = line.split()[0]
            if token.startswith("/"):
                path = token
                name = basename(token)
            else:
                name = token
        if not name and path:
            name = basename(path)
        if not name:
            continue
        key = (name, path)
        if key not in seen:
            seen.add(key)
            deps.append({"name": name, "path": path})
    return deps, unresolved


def linux_closure(binary: Path) -> dict[str, Any]:
    ldd = run([tool_path("ldd"), str(binary)])
    if ldd.returncode != 0:
        die(ldd.stderr.strip() or "ldd failed")
    deps, unresolved = parse_ldd(ldd.stdout)
    readelf_needed: list[str] = []
    if shutil.which("readelf"):
        readelf = run(["readelf", "-d", str(binary)])
        if readelf.returncode == 0:
            for match in re.finditer(r"Shared library: \[(.*?)\]", readelf.stdout):
                readelf_needed.append(match.group(1))
    return {
        "platform_tool": "ldd",
        "dependencies": deps,
        "unresolved": unresolved,
        "direct_needed": sorted(readelf_needed),
    }


def macos_closure(binary: Path) -> dict[str, Any]:
    otool = run([tool_path("otool"), "-L", str(binary)])
    if otool.returncode != 0:
        die(otool.stderr.strip() or "otool -L failed")
    deps: list[dict[str, str]] = []
    for raw in otool.stdout.splitlines()[1:]:
        line = raw.strip()
        if not line:
            continue
        path = line.split(" ", 1)[0]
        deps.append({"name": basename(path), "path": path})
    return {
        "platform_tool": "otool -L",
        "dependencies": deps,
        "unresolved": [],
        "direct_needed": [],
    }


def optional_cmd(argv: list[str]) -> list[str]:
    if not shutil.which(argv[0]):
        return []
    result = run(argv)
    if result.returncode != 0:
        return []
    return [line.strip() for line in result.stdout.splitlines() if line.strip()]


def pkg_version(name: str) -> str:
    if not shutil.which("pkg-config"):
        return ""
    result = run(["pkg-config", "--modversion", name])
    if result.returncode != 0:
        return ""
    return result.stdout.strip()


def validate(report: dict[str, Any]) -> None:
    deps = report["dependencies"]
    unresolved = report["unresolved"]
    if unresolved:
        die("unresolved dynamic dependencies:\n" + "\n".join(unresolved))
    if not has_dep(deps, "libcurl"):
        die("dynamic closure does not include libcurl")
    if not has_dep(deps, "jansson"):
        die("dynamic closure does not include Jansson")
    tls = classify(deps, TLS_MARKERS)
    curl_features = set(report.get("curl_config", {}).get("features", []))
    ssl_backends = report.get("curl_config", {}).get("ssl_backends", [])
    if not tls and "SSL" not in curl_features and not ssl_backends:
        die("could not identify a TLS-capable libcurl backend in dependency closure")


def build_report(binary: Path) -> dict[str, Any]:
    system = platform.system()
    if system == "Linux":
        closure = linux_closure(binary)
    elif system == "Darwin":
        closure = macos_closure(binary)
    else:
        die(f"unsupported dependency-closure platform: {system}")
    deps = closure["dependencies"]
    report: dict[str, Any] = {
        "schema": "snajpagent.dependency_closure.v1",
        "binary": str(binary),
        "platform": {
            "system": system,
            "release": platform.release(),
            "machine": platform.machine(),
        },
        **closure,
        "classified_backends": {
            "tls": classify(deps, TLS_MARKERS),
            "resolver": classify(deps, RESOLVER_MARKERS),
            "compression": classify(deps, COMPRESSION_MARKERS),
            "http": classify(deps, HTTP_MARKERS),
        },
        "pkg_config": {
            "jansson": pkg_version("jansson"),
            "libcurl": pkg_version("libcurl"),
        },
        "curl_config": {
            "version": optional_cmd(["curl-config", "--version"]),
            "features": optional_cmd(["curl-config", "--features"]),
            "ssl_backends": optional_cmd(["curl-config", "--ssl-backends"]),
        },
    }
    validate(report)
    return report


def main() -> int:
    if len(sys.argv) not in (2, 4):
        die("usage: check_dependency_closure.py PATH_TO_BINARY [--json-out PATH]")
    binary = Path(sys.argv[1]).resolve()
    if not binary.is_file():
        die(f"binary not found: {binary}")
    json_out: Path | None = None
    if len(sys.argv) == 4:
        if sys.argv[2] != "--json-out":
            die("usage: check_dependency_closure.py PATH_TO_BINARY [--json-out PATH]")
        json_out = Path(sys.argv[3])
    report = build_report(binary)
    if json_out is None and os.environ.get("SNAJPAGENT_DEP_CLOSURE_JSON"):
        json_out = Path(os.environ["SNAJPAGENT_DEP_CLOSURE_JSON"])
    if json_out is not None:
        json_out.parent.mkdir(parents=True, exist_ok=True)
        json_out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                            encoding="utf-8")
    deps = report["dependencies"]
    classified = report["classified_backends"]
    print(
        "depclosurecheck: ok "
        f"({report['platform']['system']} {report['platform']['machine']}; "
        f"deps={len(deps)}; "
        f"curl={'yes' if has_dep(deps, 'libcurl') else 'no'}; "
        f"jansson={'yes' if has_dep(deps, 'jansson') else 'no'}; "
        f"tls={len(classified['tls']) or len(report['curl_config']['ssl_backends'])})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
