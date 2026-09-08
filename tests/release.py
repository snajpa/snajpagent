#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Focused packaging/channel regressions; synthetic bytes, no publication."""
import argparse
import importlib.util
import json
import re
import shutil
from pathlib import Path
import subprocess
import tempfile
from unittest.mock import patch

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("release", root / "tools/release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


def rejected(function):
    try:
        function()
    except ValueError:
        return
    raise AssertionError("invalid release accepted")


with tempfile.TemporaryDirectory(prefix="release-", dir=root / "build") as tmp:
    tmp = Path(tmp)
    matrix = tmp / "matrix"
    binary = matrix / "linux-x86_64/bin/snajpagent"
    binary.parent.mkdir(parents=True)
    symbols = binary.parent / ".debug"
    symbols.mkdir()
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    version = f"0.99.2-{revision[:7]}"
    data = (f"\nsnajpagent-update-v1\nsnajpagent\nlinux-x86_64\nhttps://publisher.test\n{version}\n"
            ".debug_info").encode()
    binary.write_bytes(data)
    (symbols / "snajpagent").write_bytes(data)
    args = argparse.Namespace(version=version, revision=revision, publisher="https://publisher.test",
                             release=f"https://publisher.test/{version}", changelog="https://publisher.test/#log",
                             output=tmp / "stage", matrix=matrix)
    with patch.object(release, "targets", return_value=["linux-x86_64"]):
        release.stage(args)
        staged = args.output / f"snajpagent-{version}-linux-x86_64"
        assert staged.read_bytes() == data
        channel = args.output / "latest-dev"
        assert len(release.load_channel(channel)) == 1
        rejected(lambda: release.load_channel(channel, ["linux-x86_64", "freebsd-amd64"]))
        # Adding a later target must not invalidate an older published channel.
        with patch.object(release, "targets", return_value=["linux-x86_64", "freebsd-amd64"]):
            assert len(release.load_channel(channel)) == 1
        descriptor = next(channel.glob("*.json"))
        meta = json.loads(descriptor.read_text())
        meta["target"] = "../escape"
        descriptor.write_text(json.dumps(meta))
        rejected(lambda: release.load_channel(channel))
        meta["target"] = "linux-x86_64"
        descriptor.write_text(json.dumps(meta))
        def download(command, **kwargs):
            Path(command[command.index("--output") + 1]).write_bytes(data)
        with patch.object(release.subprocess, "run", side_effect=download):
            release.pages(argparse.Namespace(web=args.output))
            assert descriptor.with_suffix("").read_bytes() == data
            meta["sha256"] = "0" * 64
            descriptor.write_text(json.dumps(meta))
            rejected(lambda: release.pages(argparse.Namespace(web=args.output)))
            assert not list(channel.glob("*.download"))
        args.version = "0.99.2-0000000"
        rejected(lambda: release.stage(args))

# An annotated tag supplies the canonical native/matrix and staging identity.
with tempfile.TemporaryDirectory(prefix="release-tag-", dir=root / "build") as tmp:
    tmp = Path(tmp)
    for name in ("Makefile", "config.mk", "META", "COPYING", "LICENSE_SCOPE", "snajpagent.1",
                 "RELEASE.md", "RELEASE-NOTES.md", "DEPENDENCIES.md"):
        shutil.copyfile(root / name, tmp / name)
    def git(*args):
        return subprocess.check_output(["git", "-C", str(tmp), *args], text=True).strip()
    git("init", "-q", "--initial-branch=master")
    git("config", "user.name", "Release Test")
    git("config", "user.email", "release@example.test")
    git("add", ".")
    git("commit", "-qm", "release source")
    git("tag", "-a", "0.99.3", "-m", "approved fixture release")
    probe = "version-test:;@printf '%s\\n' '$(BUILD_VERSION)'"
    command = ["make", "-s", "--eval", probe, "version-test"]
    assert subprocess.check_output(command, cwd=tmp, text=True).strip() == "0.99.3"
    matrix_plan = subprocess.check_output(["make", "-n", "prod-linux-x86_64",
        "UPDATE_BASE_URL=https://publisher.test"], cwd=tmp, text=True)
    assert "--argstr buildVersion '0.99.3'" in matrix_plan
    # Keep the existing manual override available independently of the tag.
    assert subprocess.check_output(command + ["BUILD_VERSION=7.8.9"], cwd=tmp, text=True).strip() == "7.8.9"
    args = argparse.Namespace(version=None, revision="0.99.3", publisher="https://publisher.test",
        release=None, changelog="https://publisher.test/#log", output=tmp / "stage", matrix=tmp / "matrix")
    binary = args.matrix / "linux-x86_64/bin/snajpagent"
    binary.parent.mkdir(parents=True)
    data = b"\nsnajpagent-update-v1\nsnajpagent\nlinux-x86_64\nhttps://publisher.test\n0.99.3\n"
    binary.write_bytes(data)
    (binary.parent / ".debug").mkdir()
    (binary.parent / ".debug/snajpagent").write_bytes(data)
    with patch.object(release, "ROOT", tmp), patch.object(release, "targets", return_value=["linux-x86_64"]):
        release.stage(args)
    meta = json.loads((args.output / "latest/snajpagent-linux-x86_64.json").read_text())
    assert meta["version"] == "0.99.3"
    assert meta["url"].endswith("/0.99.3/snajpagent-0.99.3-linux-x86_64")
    assert (args.output / "snajpagent-0.99.3-source.tar.gz").exists()

# Inspect the native build plan: publisher opt-in forces dev debug even with DEBUG=0.
plan = subprocess.run(["make", "-n", "DEBUG=0", "BUILD_VERSION=0.99.2-abcdef0",
                       "UPDATE_BASE_URL=https://publisher.test", "UPDATE_TARGET=linux-x86_64"],
                      cwd=root, capture_output=True, text=True, check=True).stdout
assert "DEBUG=1" in plan and "-fno-omit-frame-pointer" in plan
assert "latest-dev/snajpagent-linux-x86_64" in plan
mixed = subprocess.run(["make", "-n", "all", "prod-linux-x86_64",
                        "UPDATE_BASE_URL=https://publisher.test"], cwd=root, capture_output=True, text=True)
assert mixed.returncode and "UPDATE_TARGET is required" in mixed.stderr
print("PASS: staging, immutable source selection, channel growth, hash failure, native dev profile")

# Native BSD pthread DSOs must follow the static application libraries. Curl
# can prefix the imported Threads flag twice; stripping must leave no bare -l
# that would consume the following library name.
for recipe in ("freebsd.nix", "openbsd.nix", "netbsd.nix"):
    line = next(line for line in (root / "nix" / recipe).read_text().splitlines()
                if '"CURL_LIBS=' in line)
    expression = re.search(r"sed -E '([^']+)'", line).group(1)
    for flags in ("-lpthread -lidn2", "-l-lpthread -lidn2", "-lidn2 -lpthread",
                  "-l-lpthread -lpthread -lidn2", "-l-pthread -lidn2", "-pthread -lidn2",
                  "-l-pthread -pthread -lpthread -lidn2"):
        actual = subprocess.run(["sed", "-E", expression], input=flags,
                                capture_output=True, text=True, check=True).stdout
        assert actual.split() == ["-lidn2"], (recipe, flags, actual)
print("PASS: BSD static dependency flags preserve the following library")

# ELF TLS lowering happens again during LTO. NetBSD 5 needs emulation at
# compile and final link; a compile-only flag leaves a crashing native TLS load.
netbsd = (root / "nix/netbsd.nix").read_text().splitlines()
for variable in ("cflags", "ldflags"):
    assignment = next(line for line in netbsd if line.startswith(f"  {variable} ="))
    assert "-femulated-tls" in assignment, (variable, assignment)
application_link = next(line for line in netbsd if "'LDFLAGS=" in line)
assert "${ldflags}" in application_link, application_link
assert "-Wl,-mllvm,-emulated-tls" in application_link, application_link
print("PASS: NetBSD emulated TLS reaches the LTO linker")

# Feature-selection macros follow the requested target, including cross-builds.
for target in ("Linux", "Darwin", "FreeBSD", "OpenBSD", "NetBSD", "Windows_NT"):
    for extra in ([], ["CPPFLAGS=-D_POSIX_C_SOURCE=200809L"]):
        plan = subprocess.run(["make", "-Bn", "src/platform.o",
                               f"TARGET_OS={target}", *extra], cwd=root,
                              capture_output=True, text=True, check=True).stdout
        compile_line = next(line for line in plan.splitlines()
                            if "-c src/platform.c" in line)
        assert ("-D_DARWIN_C_SOURCE" in compile_line) == (target == "Darwin"), target
print("PASS: Darwin feature selection is target-specific")

# Keep the two-load-segment ABI constraint on old NetBSD only. Modern builds
# use the native shared CRT endpoints with PIE, RELRO and immediate binding.
netbsd_text = "\n".join(netbsd)
assert 'if legacy then "crtbegin.o" else "crtbeginS.o"' in netbsd_text
assert 'if legacy then "crtend.o" else "crtendS.o"' in netbsd_text
assert 'if legacy then "-no-pie,--no-rosegment,-z,norelro," else "-pie,-z,relro,-z,now,"' in netbsd_text
print("PASS: NetBSD hardening follows the native loader ABI")

# Brotli's log2 fallback calls log, which still requires libm on old BSDs.
for recipe in ("freebsd.nix", "netbsd.nix"):
    text = (root / "nix" / recipe).read_text()
    brotli = text.split("  brotli =", 1)[1].split("  zstd =", 1)[0]
    assert 'set(LIBM_LIBRARY "m")' in brotli, recipe
    assert 'add_definitions(-DBROTLI_HAVE_LOG2=0)' in brotli, recipe
print("PASS: BSD Brotli log2 fallback retains its libm dependency")

# GCC's 32-bit ARM specs can accept -static-pie but still choose a dynamic
# loader/libc. Keep static library selection and omit PT_INTERP explicitly.
linux = (root / "nix/linux.nix").read_text()
link = next(line for line in linux.splitlines() if "'LDFLAGS=" in line)
assert '-static-pie' in link
assert 'musl.stdenv.hostPlatform.isAarch32' in link
assert '" -Wl,-Bstatic,--no-dynamic-linker,-z,text"' in link
print("PASS: 32-bit ARM static PIE explicitly omits the dynamic runtime")
