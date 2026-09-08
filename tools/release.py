#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Stage release executables or materialize the two static Pages channels."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]
MAX_BINARY = 256 * 1024 * 1024


def targets(revision=None):
    makefile = git_text(revision, "Makefile") if revision else (ROOT / "Makefile").read_text()
    line = re.search(r"^PROD_TARGETS = (.+)$", makefile, re.M)
    return [target.removeprefix("prod-") for target in line[1].split()]


def git_text(revision, name):
    return subprocess.check_output(["git", "show", f"{revision}:{name}"], cwd=ROOT, text=True)


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def https(url):
    parsed = urlsplit(url)
    if parsed.scheme != "https" or not parsed.hostname or parsed.username or parsed.password:
        raise ValueError(f"an unauthenticated HTTPS URL is required: {url!r}")
    return url


def load_channel(directory, expected=None):
    entries = []
    for path in sorted(directory.glob("*.json")):
        meta = json.loads(path.read_text())
        target = meta["target"]
        suffix = ".exe" if target.startswith("windows-") else ""
        if (meta["name"] != "snajpagent" or not re.fullmatch(r"[a-z0-9_]+(?:-[a-z0-9_]+)+", target)
                or path.name != f"snajpagent-{target}{suffix}.json"
                or not re.fullmatch(r"\d+\.\d+\.\d+(?:-[0-9a-f]{7,40})?", meta["version"])
                or not re.fullmatch(r"[0-9a-f]{64}", meta["sha256"])
                or type(meta["size"]) is not int or not 0 < meta["size"] <= MAX_BINARY):
            raise ValueError(f"invalid update descriptor: {path}")
        https(meta["url"]); https(meta["changelog"])
        if (directory.name == "latest-dev") != ("-" in meta["version"]):
            raise ValueError(f"version/channel mismatch: {path}")
        entries.append((path, meta))
    if not entries or (expected is not None and {meta["target"] for _, meta in entries} != set(expected)):
        raise ValueError(f"incomplete executable matrix in {directory}")
    if len({meta["version"] for _, meta in entries}) != 1:
        raise ValueError(f"mixed release versions in {directory}")
    return entries


def stage(args):
    revision = subprocess.check_output(["git", "rev-parse", "--verify", args.revision + "^{commit}"],
                                       cwd=ROOT, text=True).strip()
    version = args.version
    if version is None:
        version = subprocess.check_output(
            ["git", "describe", "--tags", "--exact-match", revision], cwd=ROOT, text=True).strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+(?:-[0-9a-f]{7,40})?", version):
        raise ValueError("version must be a stable version or version-commit")
    if "-" in version and not revision.startswith(version.split("-", 1)[1]):
        raise ValueError("development suffix must identify the archived source revision")
    base = https(args.publisher).rstrip("/")
    release = https(args.release or f"https://github.com/snajpa/snajpagent/releases/download/{version}").rstrip("/")
    log = https(args.changelog)
    channel = "latest-dev" if "-" in version else "latest"
    # Refuse existing output rather than modifying an immutable release stage.
    args.output.mkdir(parents=True, exist_ok=False)
    descriptions = args.output / channel
    descriptions.mkdir()
    required = targets(revision)
    for target in required:
        suffix = ".exe" if target.startswith("windows-") else ""
        source = args.matrix / target / "bin" / ("snajpagent" + suffix)
        data = source.read_bytes()
        marker = f"\nsnajpagent-update-v1\nsnajpagent\n{target}\n{base}\n{version}\n".encode()
        if marker not in data or not 0 < len(data) <= MAX_BINARY:
            raise ValueError(f"wrong publisher/target/version or invalid size: {source}")
        if channel == "latest-dev" and not target.startswith("macos-"):
            if b".debug_info" not in data or b".gnu_debuglink" in data:
                raise ValueError(f"development executable lacks embedded debug information: {source}")
        name = f"snajpagent-{version}-{target}{suffix}"
        dest = args.output / name
        shutil.copyfile(source, dest)
        dest.chmod(0o755)
        meta = dict(name="snajpagent", target=target, version=version,
                    url=f"{release}/{name}", sha256=digest(dest), size=len(data), changelog=log)
        (descriptions / f"snajpagent-{target}{suffix}.json").write_text(json.dumps(meta, indent=2) + "\n")
        symbols = source.parent / ("snajpagent.dSYM" if target.startswith("macos-") else ".debug")
        if not symbols.exists():
            raise ValueError(f"matching symbols missing: {symbols}")
        with tarfile.open(args.output / f"snajpagent-{version}-{target}-symbols.tar.gz", "w:gz",
                          dereference=True) as archive:
            archive.add(symbols, arcname=symbols.name)
    load_channel(descriptions, required)
    for name in ("COPYING", "LICENSE_SCOPE", "snajpagent.1", "RELEASE.md", "RELEASE-NOTES.md", "DEPENDENCIES.md"):
        (args.output / name).write_text(git_text(revision, name))
    with (args.output / f"snajpagent-{version}-source.tar.gz").open("wb") as output:
        subprocess.run(["git", "archive", "--format=tar.gz", f"--prefix=snajpagent-{version}/", revision],
                       cwd=ROOT, stdout=output, check=True)
    print(f"Staged {len(required)} executables and matching symbols in {args.output}")
    print("Add corresponding dependency sources/notices and SHA256SUMS before publishing.")


def pages(args):
    # Called only by the manual Pages deployment; no Git or release mutation.
    for channel in ("latest", "latest-dev"):
        directory = args.web / channel
        if not directory.exists():
            continue
        entries = load_channel(directory)
        for descriptor, meta in entries:
            dest = descriptor.with_suffix("")
            temporary = dest.with_name(dest.name + ".download")
            try:
                subprocess.run(["curl", "--fail", "--location", "--proto", "=https", "--proto-redir", "=https",
                                "--max-time", "120", "--max-filesize", str(meta["size"]),
                                "--output", str(temporary), meta["url"]], check=True)
                if temporary.stat().st_size != meta["size"] or digest(temporary) != meta["sha256"]:
                    raise ValueError(f"release asset mismatch: {descriptor}")
                temporary.replace(dest)
            finally:
                temporary.unlink(missing_ok=True)
        print(f"Materialized {channel}: {len(entries)} verified executables")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prepare = commands.add_parser("stage")
    prepare.add_argument("--version", help="manual override; normally derived from the exact Git tag")
    prepare.add_argument("--revision", default="HEAD", help="exact source revision used for this matrix")
    prepare.add_argument("--matrix", type=Path, default=ROOT / "build/matrix")
    prepare.add_argument("--output", type=Path, required=True)
    prepare.add_argument("--publisher", default="https://agent.snajpa.net")
    prepare.add_argument("--release", help="immutable URL prefix; defaults to the tagged GitHub release")
    prepare.add_argument("--changelog", default="https://agent.snajpa.net/downloads.html#changelog")
    prepare.set_defaults(run=stage)
    deploy = commands.add_parser("pages")
    deploy.add_argument("--web", type=Path, default=ROOT / "www")
    deploy.set_defaults(run=pages)
    options = parser.parse_args()
    options.run(options)
