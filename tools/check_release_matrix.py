#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Validate a matrix of per-platform snajpagent release-evidence bundles.

`check_release_evidence.py` validates one concrete host bundle.  This helper is
for the release aggregation step: it proves the supplied set has unique platform
ids, a consistent snajpagent version, the required platform coverage, and the
required terminal/live-provider evidence in every bundle.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
CHECK_ONE = ROOT / "tools" / "check_release_evidence.py"
_CHECK_ONE_MODULE: Any | None = None


def check_one_module() -> Any:
    global _CHECK_ONE_MODULE
    if _CHECK_ONE_MODULE is not None:
        return _CHECK_ONE_MODULE
    spec = importlib.util.spec_from_file_location("snajpagent_check_release_evidence", CHECK_ONE)
    if spec is None or spec.loader is None:
        raise MatrixError(f"cannot load single-bundle checker: {CHECK_ONE}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    _CHECK_ONE_MODULE = module
    return module


class MatrixError(Exception):
    pass


def load_json(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise MatrixError(f"missing evidence file: {path}")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise MatrixError(f"invalid JSON in {path}: {exc}") from exc
    if not isinstance(data, dict):
        raise MatrixError(f"top-level JSON object required: {path}")
    return data


def validate_one(path: Path, *, require_terminal: bool, require_live: bool) -> dict[str, Any]:
    summary = load_json(path / "release_evidence.json")
    platform_id = summary.get("platform_id")
    version = summary.get("version")
    if not isinstance(platform_id, str) or not platform_id:
        raise MatrixError(f"{path}: missing nonempty platform_id")
    if not isinstance(version, str) or not version:
        raise MatrixError(f"{path}: missing nonempty version")

    try:
        return check_one_module().validate_bundle(
            path,
            require_terminal=require_terminal,
            require_live=require_live,
        )
    except SystemExit as exc:
        detail = str(exc) or "no output"
        raise MatrixError(f"{path}: bundle validation failed: {detail}") from exc


def validate_matrix(paths: list[Path], *, require_platforms: list[str],
                    require_terminal: bool, require_live: bool,
                    allow_extra_platforms: bool, allow_version_mismatch: bool) -> tuple[str, dict[str, Path]]:
    if not paths:
        raise MatrixError("at least one evidence directory is required")
    required = set(require_platforms)
    if len(required) != len(require_platforms):
        raise MatrixError("duplicate --require-platform value")

    seen: dict[str, Path] = {}
    version = ""
    for path in paths:
        summary = validate_one(path.resolve(), require_terminal=require_terminal,
                               require_live=require_live)
        platform_id = str(summary["platform_id"])
        bundle_version = str(summary["version"])
        previous = seen.get(platform_id)
        if previous is not None:
            raise MatrixError(f"duplicate platform_id {platform_id!r}: {previous} and {path}")
        if version and bundle_version != version and not allow_version_mismatch:
            raise MatrixError(
                f"version mismatch: {path} has {bundle_version!r}, expected {version!r}"
            )
        if not version:
            version = bundle_version
        seen[platform_id] = path

    missing = sorted(required - set(seen))
    if missing:
        raise MatrixError("missing required platform evidence: " + ", ".join(missing))
    extras = sorted(set(seen) - required)
    if required and extras and not allow_extra_platforms:
        raise MatrixError("unexpected platform evidence: " + ", ".join(extras))
    return version, seen


def command_record(name: str) -> dict[str, Any]:
    return {"name": name, "returncode": 0, "stdout": "", "stderr": ""}


def write_json(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def write_fake_bundle(root: Path, platform_id: str, version: str = "0.9.0-wip") -> Path:
    out = root / platform_id
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
    write_json(out / "source_audit.json", {
        "schema": "snajpagent.source_audit_evidence.v1",
        "commands": source_commands,
    })
    write_json(out / "dependency_closure.json", {
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
    write_json(out / "terminal_evidence.json", {
        "schema": "snajpagent.terminal_evidence.v1",
        "commands": terminal_commands,
    })
    write_json(out / "live_provider_evidence.json", {
        "schema": "snajpagent.live_provider_evidence.v1",
        "api_key_present": True,
        "command": command_record("live_provider_check"),
    })
    write_json(out / "release_evidence.json", {
        "schema": "snajpagent.release_evidence.v1",
        "version": version,
        "platform_id": platform_id,
        "binary": {"sha256": "0" * 64},
        "records": {
            "source_audit": "source_audit.json",
            "dependency_closure": "dependency_closure.json",
            "terminal_evidence": "terminal_evidence.json",
            "live_provider_evidence": "live_provider_evidence.json",
        },
        "commands": [command_record("source"), command_record("dependency"),
                     command_record("terminal"), command_record("live")],
    })
    return out


def expect_matrix_failure(paths: list[Path], needle: str, **kwargs: Any) -> None:
    try:
        validate_matrix(paths, **kwargs)
    except MatrixError as exc:
        if needle not in str(exc):
            raise MatrixError(
                f"self-test expected {needle!r}, got {str(exc)!r}"
            ) from exc
        return
    raise MatrixError(f"self-test expected failure containing {needle!r}")


def self_test() -> int:
    base_kwargs = {
        "require_platforms": ["linux-x86_64", "macos-arm64"],
        "require_terminal": True,
        "require_live": True,
        "allow_extra_platforms": False,
        "allow_version_mismatch": False,
    }
    with tempfile.TemporaryDirectory(prefix="snajpagent-evidence-matrix-") as tmp:
        root = Path(tmp)
        linux = write_fake_bundle(root, "linux-x86_64")
        macos = write_fake_bundle(root, "macos-arm64")
        version, seen = validate_matrix([linux, macos], **base_kwargs)
        if version != "0.9.0-wip" or set(seen) != {"linux-x86_64", "macos-arm64"}:
            raise MatrixError("self-test positive case produced wrong coverage")

        expect_matrix_failure([linux], "macos-arm64", **base_kwargs)

        duplicate = write_fake_bundle(root / "duplicate", "linux-x86_64")
        expect_matrix_failure([linux, duplicate, macos], "duplicate platform_id", **base_kwargs)

        mismatch = write_fake_bundle(root / "mismatch", "macos-arm64", "9.9.9-test")
        expect_matrix_failure([linux, mismatch], "version mismatch", **base_kwargs)

        extra = write_fake_bundle(root / "extra", "linux-riscv64")
        expect_matrix_failure([linux, macos, extra], "unexpected platform evidence", **base_kwargs)

        missing_live = write_fake_bundle(root / "missing-live", "macos-arm64")
        (missing_live / "live_provider_evidence.json").unlink()
        expect_matrix_failure([linux, missing_live], "live_provider_evidence", **base_kwargs)
    print("evidencematrixcheck: self-test ok")
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence_dirs", nargs="*", help="per-platform evidence directories")
    parser.add_argument("--require-platform", action="append", default=[],
                        help="required platform_id; may be repeated")
    parser.add_argument("--require-terminal", action="store_true",
                        help="require terminal_evidence.json in every bundle")
    parser.add_argument("--require-live", action="store_true",
                        help="require live_provider_evidence.json in every bundle")
    parser.add_argument("--allow-extra-platforms", action="store_true",
                        help="allow supplied bundles whose platform_id was not required")
    parser.add_argument("--allow-version-mismatch", action="store_true",
                        help="allow bundles from different snajpagent versions")
    parser.add_argument("--self-test", action="store_true", help="run built-in regression checks")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        if args.self_test:
            return self_test()
        version, seen = validate_matrix(
            [Path(path) for path in args.evidence_dirs],
            require_platforms=args.require_platform,
            require_terminal=args.require_terminal,
            require_live=args.require_live,
            allow_extra_platforms=args.allow_extra_platforms,
            allow_version_mismatch=args.allow_version_mismatch,
        )
    except MatrixError as exc:
        raise SystemExit(f"evidencematrixcheck: {exc}") from exc
    platforms = ", ".join(sorted(seen))
    live = "required" if args.require_live else "optional"
    terminal = "required" if args.require_terminal else "optional"
    print(f"evidencematrixcheck: ok (version={version}; platforms={platforms}; terminal={terminal}; live={live})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
