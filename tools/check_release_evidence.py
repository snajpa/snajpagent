#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Validate a snajpagent release-evidence bundle.

The checker accepts evidence generated on the current host or copied from an
external release host.  Use --require-live and --require-terminal when a final
release gate must prove those pieces were captured for the platform bundle.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys
import tempfile
from typing import Any

HEX64 = re.compile(r"^[0-9a-f]{64}$")


def die(message: str) -> None:
    raise SystemExit(f"evidencecheck: {message}")


def load_json(path: Path) -> dict[str, Any]:
    if not path.is_file():
        die(f"missing evidence file: {path}")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        die(f"invalid JSON in {path}: {exc}")
    if not isinstance(data, dict):
        die(f"top-level JSON object required: {path}")
    return data


def record_path(root: Path, value: object, label: str) -> Path:
    if not isinstance(value, str) or not value:
        die(f"release evidence must reference {label}")
    if value.startswith(("/", "./")) or "\\" in value or "//" in value:
        die(f"{label} path is not canonical relative evidence path: {value!r}")
    parts = value.split("/")
    if any(part in ("", ".", "..") for part in parts):
        die(f"{label} path is unsafe: {value!r}")
    candidate = (root / value).resolve(strict=False)
    root_resolved = root.resolve(strict=True)
    try:
        candidate.relative_to(root_resolved)
    except ValueError:
        die(f"{label} path escapes evidence directory: {value!r}")
    return candidate


def require_schema(data: dict[str, Any], expected: str, label: str) -> None:
    if data.get("schema") != expected:
        die(f"{label} schema is {data.get('schema')!r}, expected {expected!r}")


def require_nonempty_string(data: dict[str, Any], key: str, label: str) -> None:
    if not isinstance(data.get(key), str) or not data[key]:
        die(f"{label} missing nonempty {key}")


def require_hex(value: object, label: str) -> None:
    if not isinstance(value, str) or not HEX64.fullmatch(value):
        die(f"{label} is not lowercase sha256 hex")


def commands_ok(commands: object, label: str) -> None:
    if not isinstance(commands, list) or not commands:
        die(f"{label} has no command records")
    for index, command in enumerate(commands):
        if not isinstance(command, dict):
            die(f"{label} command {index} is not an object")
        require_nonempty_string(command, "name", f"{label} command {index}")
        if command.get("returncode") != 0:
            die(f"{label} command {command.get('name')} did not pass")


def validate_source_audit(path: Path) -> None:
    audit = load_json(path)
    require_schema(audit, "snajpagent.source_audit_evidence.v1", "source audit")
    commands_ok(audit.get("commands"), "source audit")
    required = {"statuscheck", "depscheck", "portabilitycheck", "sizecheck"}
    seen = {cmd.get("name") for cmd in audit.get("commands", []) if isinstance(cmd, dict)}
    missing = sorted(required - seen)
    if missing:
        die("source audit missing commands: " + ", ".join(missing))


def has_dep(deps: list[dict[str, Any]], needle: str) -> bool:
    needle = needle.lower()
    for dep in deps:
        if needle in str(dep.get("name", "")).lower() or needle in str(dep.get("path", "")).lower():
            return True
    return False


def validate_dependency(path: Path) -> None:
    dep = load_json(path)
    require_schema(dep, "snajpagent.dependency_closure.v1", "dependency closure")
    deps = dep.get("dependencies")
    if not isinstance(deps, list) or not deps:
        die("dependency closure contains no dependencies")
    if dep.get("unresolved"):
        die("dependency closure has unresolved entries")
    if not has_dep(deps, "libcurl"):
        die("dependency closure lacks libcurl")
    if not has_dep(deps, "jansson"):
        die("dependency closure lacks Jansson")
    classified = dep.get("classified_backends", {})
    curl_config = dep.get("curl_config", {})
    if not isinstance(classified, dict) or not isinstance(curl_config, dict):
        die("dependency closure lacks backend classification")
    tls = classified.get("tls")
    ssl_backends = curl_config.get("ssl_backends")
    features = curl_config.get("features")
    if not tls and not ssl_backends and (not isinstance(features, list) or "SSL" not in features):
        die("dependency closure lacks TLS backend evidence")


def validate_terminal(path: Path) -> None:
    term = load_json(path)
    require_schema(term, "snajpagent.terminal_evidence.v1", "terminal evidence")
    commands_ok(term.get("commands"), "terminal evidence")
    required = {"pty_interactive_xterm", "pty_interactive_dumb", "pty_resize", "pty_suspend", "pty_terminal_matrix", "pty_active"}
    seen = {cmd.get("name") for cmd in term.get("commands", []) if isinstance(cmd, dict)}
    missing = sorted(required - seen)
    if missing:
        die("terminal evidence missing commands: " + ", ".join(missing))


def validate_live(path: Path) -> None:
    live = load_json(path)
    require_schema(live, "snajpagent.live_provider_evidence.v1", "live provider evidence")
    command = live.get("command")
    if not isinstance(command, dict) or command.get("returncode") != 0:
        die("live provider command did not pass")
    if live.get("api_key_present") is not True:
        die("live provider evidence did not record API-key presence")


def command_record(name: str) -> dict[str, Any]:
    return {"name": name, "returncode": 0, "stdout": "", "stderr": ""}


def write_json(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def write_fake_bundle(root: Path, *, source_ref: str = "source_audit.json",
                      terminal: bool = True, live: bool = True) -> Path:
    source_commands = [
        command_record("statuscheck"),
        command_record("depscheck"),
        command_record("portabilitycheck"),
        command_record("sizecheck"),
    ]
    terminal_commands = [
        command_record("pty_interactive_xterm"),
        command_record("pty_interactive_dumb"),
        command_record("pty_resize"),
        command_record("pty_suspend"),
        command_record("pty_terminal_matrix"),
        command_record("pty_active"),
    ]
    write_json(root / "source_audit.json", {
        "schema": "snajpagent.source_audit_evidence.v1",
        "commands": source_commands,
    })
    write_json(root / "dependency_closure.json", {
        "schema": "snajpagent.dependency_closure.v1",
        "dependencies": [
            {"name": "libcurl.so.4", "path": "/usr/lib/libcurl.so.4"},
            {"name": "libjansson.so.4", "path": "/usr/lib/libjansson.so.4"},
            {"name": "libssl.so.3", "path": "/usr/lib/libssl.so.3"},
        ],
        "unresolved": [],
        "classified_backends": {"tls": ["/usr/lib/libssl.so.3"]},
        "curl_config": {"features": ["SSL"], "ssl_backends": ["OpenSSL"]},
    })
    records: dict[str, str | None] = {
        "source_audit": source_ref,
        "dependency_closure": "dependency_closure.json",
        "terminal_evidence": "terminal_evidence.json" if terminal else None,
        "live_provider_evidence": "live_provider_evidence.json" if live else None,
    }
    commands = [command_record("source"), command_record("dependency")]
    if terminal:
        write_json(root / "terminal_evidence.json", {
            "schema": "snajpagent.terminal_evidence.v1",
            "commands": terminal_commands,
        })
        commands.append(command_record("terminal"))
    if live:
        write_json(root / "live_provider_evidence.json", {
            "schema": "snajpagent.live_provider_evidence.v1",
            "api_key_present": True,
            "command": command_record("live_provider_check"),
        })
        commands.append(command_record("live"))
    write_json(root / "release_evidence.json", {
        "schema": "snajpagent.release_evidence.v1",
        "version": "0.9.0-wip",
        "platform_id": "linux-x86_64",
        "binary": {"sha256": "0" * 64},
        "records": records,
        "commands": commands,
    })
    return root


def expect_failure(argv: list[str], needle: str) -> None:
    try:
        main(argv)
    except SystemExit as exc:
        message = str(exc)
        if needle not in message:
            die(f"self-test expected {needle!r}, got {message!r}")
        return
    die(f"self-test expected failure containing {needle!r}")


def self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="snajpagent-evidence-one-") as tmp:
        root = Path(tmp)
        good = write_fake_bundle(root / "good")
        if main([str(good), "--require-terminal", "--require-live"]) != 0:
            die("self-test positive case did not pass")

        escape = write_fake_bundle(root / "escape", source_ref="../source_audit.json")
        expect_failure([str(escape)], "source_audit path")

        absolute = write_fake_bundle(root / "absolute", source_ref="/tmp/source_audit.json")
        expect_failure([str(absolute)], "source_audit path")

        no_live = write_fake_bundle(root / "no_live", live=False)
        expect_failure([str(no_live), "--require-live"], "live provider evidence is required")

        no_terminal = write_fake_bundle(root / "no_terminal", terminal=False)
        expect_failure([str(no_terminal), "--require-terminal"], "terminal evidence is required")
    print("evidencecheck: self-test ok")
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence_dir", nargs="?", help="directory containing release_evidence.json")
    parser.add_argument("--require-terminal", action="store_true", help="require terminal_evidence.json")
    parser.add_argument("--require-live", action="store_true", help="require live_provider_evidence.json")
    parser.add_argument("--require-platform", action="append", default=[], help="required platform_id; may be repeated")
    parser.add_argument("--self-test", action="store_true", help="run built-in evidence-check regression tests")
    return parser.parse_args(argv)


def validate_bundle(evidence_dir: Path, *, require_terminal: bool = False,
                    require_live: bool = False,
                    require_platforms: list[str] | None = None) -> dict[str, Any]:
    root = evidence_dir.resolve()
    summary = load_json(root / "release_evidence.json")
    require_schema(summary, "snajpagent.release_evidence.v1", "release evidence")
    require_nonempty_string(summary, "version", "release evidence")
    require_nonempty_string(summary, "platform_id", "release evidence")
    required_platforms = require_platforms or []
    if required_platforms and summary["platform_id"] not in required_platforms:
        die(f"platform_id {summary['platform_id']!r} is not in required set {required_platforms!r}")
    binary = summary.get("binary")
    if not isinstance(binary, dict):
        die("release evidence missing binary object")
    require_hex(binary.get("sha256"), "binary sha256")
    records = summary.get("records")
    if not isinstance(records, dict):
        die("release evidence missing records object")
    commands_ok(summary.get("commands"), "release evidence")

    source_path = record_path(root, records.get("source_audit"), "source_audit")
    dep_path = record_path(root, records.get("dependency_closure"), "dependency_closure")
    validate_source_audit(source_path)
    validate_dependency(dep_path)

    terminal_rel = records.get("terminal_evidence")
    if require_terminal and not isinstance(terminal_rel, str):
        die("terminal evidence is required but absent")
    if isinstance(terminal_rel, str):
        validate_terminal(record_path(root, terminal_rel, "terminal_evidence"))

    live_rel = records.get("live_provider_evidence")
    if require_live and not isinstance(live_rel, str):
        die("live provider evidence is required but absent")
    if isinstance(live_rel, str):
        validate_live(record_path(root, live_rel, "live_provider_evidence"))
    return summary


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.evidence_dir:
        die("evidence_dir is required unless --self-test is used")
    summary = validate_bundle(
        Path(args.evidence_dir),
        require_terminal=args.require_terminal,
        require_live=args.require_live,
        require_platforms=args.require_platform,
    )
    records = summary.get("records") if isinstance(summary.get("records"), dict) else {}
    live_rel = records.get("live_provider_evidence")
    print(f"evidencecheck: ok ({summary['platform_id']}; live={'yes' if isinstance(live_rel, str) else 'no'})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
