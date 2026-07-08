#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Collect a machine-readable release-evidence bundle for one host.

This helper does not make external claims by itself.  It records the concrete
checks that were actually run for the supplied binary and leaves live-provider
and platform-matrix completeness to `check_release_evidence.py` options and the
release process.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import time
from typing import Any
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[1]


def die(message: str, code: int = 1) -> None:
    raise SystemExit(f"collect_release_evidence: {message}")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_version() -> str:
    version_path = ROOT / "VERSION"
    if not version_path.is_file():
        return ""
    return version_path.read_text(encoding="utf-8").strip()


def command_record(name: str, argv: list[str], *, cwd: Path = ROOT,
                   env: dict[str, str] | None = None,
                   timeout: int = 240) -> dict[str, Any]:
    started = time.monotonic()
    record: dict[str, Any] = {
        "name": name,
        "argv": argv,
        "cwd": str(cwd),
        "timeout_seconds": timeout,
    }
    try:
        result = subprocess.run(argv, cwd=cwd, env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                timeout=timeout, check=False)
        record.update({
            "returncode": result.returncode,
            "stdout": result.stdout,
            "stderr": result.stderr,
        })
    except subprocess.TimeoutExpired as exc:
        record.update({
            "returncode": None,
            "timed_out": True,
            "stdout": exc.stdout if isinstance(exc.stdout, str) else "",
            "stderr": exc.stderr if isinstance(exc.stderr, str) else "",
        })
    record["duration_seconds"] = round(time.monotonic() - started, 3)
    return record


def require_ok(record: dict[str, Any]) -> None:
    if record.get("returncode") != 0:
        stdout = record.get("stdout", "")
        stderr = record.get("stderr", "")
        detail = stderr or stdout or "no output"
        die(f"{record['name']} failed: {detail.strip()}")


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def collect_source_audits(outdir: Path) -> list[dict[str, Any]]:
    commands = [
        ("statuscheck", ["python3", "./tools/check_status.py"]),
        ("depscheck", ["python3", "./tools/check_deps.py"]),
        ("portabilitycheck", ["python3", "./tools/check_portability.py"]),
        ("sizecheck", ["make", "sizecheck"]),
    ]
    records: list[dict[str, Any]] = []
    for name, argv in commands:
        record = command_record(name, argv, timeout=120)
        require_ok(record)
        records.append(record)
    write_json(outdir / "source_audit.json", {
        "schema": "snajpagent.source_audit_evidence.v1",
        "commands": records,
    })
    return records


def collect_dependency(binary: Path, outdir: Path) -> dict[str, Any]:
    json_out = outdir / "dependency_closure.json"
    record = command_record(
        "depclosurecheck",
        ["python3", "./tools/check_dependency_closure.py", str(binary), "--json-out", str(json_out)],
        timeout=120,
    )
    require_ok(record)
    return record


def collect_terminal(binary: Path, fixture: Path, outdir: Path) -> dict[str, Any]:
    root = Path(tempfile.mkdtemp(prefix="snajpagent-evidence-"))
    try:
        state = root / "state"
        work = root / "work"
        state.mkdir(mode=0o700)
        work.mkdir(mode=0o700)
        env = os.environ.copy()
        env.update({
            "XDG_STATE_HOME": str(state),
            "LC_ALL": env.get("LC_ALL", "C.UTF-8"),
            "TERM": env.get("TERM", "xterm"),
        })
        tests = [
            ("pty_interactive_xterm", ["python3", str(ROOT / "tests" / "pty_interactive.py"), str(fixture), str(work)], {"TERM": "xterm"}),
            ("pty_interactive_dumb", ["python3", str(ROOT / "tests" / "pty_interactive.py"), str(fixture), str(work)], {"TERM": "dumb"}),
            ("pty_resize", ["python3", str(ROOT / "tests" / "pty_resize.py"), str(fixture), str(work)], {"TERM": "xterm"}),
            ("pty_suspend", ["python3", str(ROOT / "tests" / "pty_suspend.py"), str(fixture), str(work)], {"TERM": "xterm"}),
            ("pty_terminal_matrix", ["python3", str(ROOT / "tests" / "pty_terminal_matrix.py"), str(fixture), str(work)], {"TERM": "xterm"}),
            ("pty_active", ["python3", str(ROOT / "tests" / "pty_active.py"), str(fixture), str(work)], {"TERM": "xterm"}),
        ]
        records: list[dict[str, Any]] = []
        for name, argv, updates in tests:
            test_env = env.copy()
            test_env.update(updates)
            record = command_record(name, argv, env=test_env, timeout=180)
            require_ok(record)
            records.append(record)
        payload = {
            "schema": "snajpagent.terminal_evidence.v1",
            "binary": str(binary),
            "fixture": str(fixture),
            "platform": platform_payload(),
            "commands": records,
        }
        write_json(outdir / "terminal_evidence.json", payload)
        return {"name": "terminal_evidence", "returncode": 0, "commands": records}
    finally:
        shutil.rmtree(root, ignore_errors=True)


def collect_live(binary: Path, outdir: Path, require_live: bool, skip_live: bool) -> dict[str, Any] | None:
    if skip_live:
        return None
    api_key_env = os.environ.get("SNAJPAGENT_LIVE_API_KEY_ENV", "OPENAI_API_KEY")
    if not os.environ.get(api_key_env):
        if require_live:
            die(f"{api_key_env} is required when --require-live is set")
        return None
    record = command_record(
        "live_provider_check",
        ["python3", "./tools/live_provider_check.py", str(binary)],
        timeout=240,
    )
    require_ok(record)
    write_json(outdir / "live_provider_evidence.json", {
        "schema": "snajpagent.live_provider_evidence.v1",
        "binary": str(binary),
        "platform": platform_payload(),
        "command": record,
        "api_key_present": True,
        "api_key_env": api_key_env,
        "base_url": os.environ.get("SNAJPAGENT_LIVE_BASE_URL",
                                   "https://api.openai.com"),
    })
    return record


def platform_payload() -> dict[str, str]:
    return {
        "system": platform.system(),
        "release": platform.release(),
        "machine": platform.machine(),
        "python": platform.python_version(),
    }


def default_platform_id() -> str:
    system = platform.system().lower()
    if system == "darwin":
        system = "macos"
    machine = platform.machine().lower()
    if machine in {"amd64", "x86_64"}:
        machine = "x86_64"
    elif machine in {"aarch64", "arm64"}:
        machine = "arm64" if system == "macos" else "aarch64"
    return f"{system}-{machine}"


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", help="path to the built snajpagent executable")
    parser.add_argument("outdir", help="directory to receive evidence JSON")
    parser.add_argument("--fixture", help="path to tests/snajpagent-fixture for PTY evidence")
    parser.add_argument("--skip-live", action="store_true", help="do not attempt live-provider evidence even if a live API key is set")
    parser.add_argument("--require-live", action="store_true", help="fail unless live-provider evidence is collected")
    parser.add_argument("--platform-id", help="stable release platform id, for example linux-x86_64")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    binary = Path(args.binary).resolve()
    if not binary.is_file():
        die(f"binary not found: {binary}", 2)
    outdir = Path(args.outdir).resolve()
    outdir.mkdir(parents=True, exist_ok=True)
    fixture = Path(args.fixture).resolve() if args.fixture else None
    if fixture is not None and not fixture.is_file():
        die(f"fixture not found: {fixture}", 2)

    command_records: list[dict[str, Any]] = []
    command_records.extend(collect_source_audits(outdir))
    command_records.append(collect_dependency(binary, outdir))
    terminal_collected = False
    if fixture is not None:
        terminal = collect_terminal(binary, fixture, outdir)
        command_records.append(terminal)
        terminal_collected = True
    live = collect_live(binary, outdir, args.require_live, args.skip_live)
    live_collected = live is not None
    if live is not None:
        command_records.append(live)

    summary = {
        "schema": "snajpagent.release_evidence.v1",
        "generated_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "version": read_version(),
        "platform_id": args.platform_id or default_platform_id(),
        "platform": platform_payload(),
        "binary": {
            "path": str(binary),
            "sha256": sha256(binary),
        },
        "records": {
            "source_audit": "source_audit.json",
            "dependency_closure": "dependency_closure.json",
            "terminal_evidence": "terminal_evidence.json" if terminal_collected else None,
            "live_provider_evidence": "live_provider_evidence.json" if live_collected else None,
        },
        "commands": command_records,
    }
    write_json(outdir / "release_evidence.json", summary)
    print(f"collect_release_evidence: ok ({outdir})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
