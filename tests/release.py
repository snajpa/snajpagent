#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Focused packaging/channel regressions; synthetic bytes, no publication."""
import argparse
from html.parser import HTMLParser
import importlib.util
import json
import os
import os
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
    command = ["make", "--no-print-directory", "-s", "--eval", probe, "version-test"]
    assert subprocess.check_output(command, cwd=tmp, text=True).strip() == "0.99.3"
    # Recursive make exports -w even when the probe asks for silent recipes.
    # Directory banners are build chatter, not part of the version value.
    nested = dict(os.environ, MAKEFLAGS="w", MAKELEVEL="2")
    assert subprocess.check_output(command, cwd=tmp, env=nested, text=True) == "0.99.3\n"
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

# PowerPC Linux encodes the write direction in bit31. musl ioctl takes int,
# whereas BSD/glibc take unsigned long: preserve the request bits with either ABI.
process_host = (root / "src/process_host.c").read_text()
resize = re.search(r"static void\npty_apply_current_size\(.*?\n}\n",
                   process_host, re.S).group()
# GCC14 diagnoses the implicit conversion; GCC13 may silently accept it.
assert "ioctl(fd, (unsigned int)TIOCSWINSZ, &ws)" in resize
with tempfile.TemporaryDirectory(prefix="release-ioctl-", dir=root / "build") as tmp:
    tmp = Path(tmp)
    for request_type in ("int", "unsigned long"):
        source = tmp / "resize.c"
        source.write_text("""
#include <assert.h>
#include <string.h>
#include <sys/ioctl.h>
#undef TIOCSWINSZ
#define TIOCSWINSZ 0x80087467UL
#define ioctl checked_ioctl
static int calls, fail;
static void host_winsize(unsigned short *rows, unsigned short *cols)
{ *rows = 37; *cols = 101; }
static int checked_ioctl(int fd, REQUEST_TYPE request, struct winsize *ws)
{
    assert(fd == 7);
    assert((unsigned long)request == (unsigned long)(REQUEST_TYPE)TIOCSWINSZ);
    assert(ws->ws_row == 37 && ws->ws_col == 101);
    ++calls;
    return fail ? -1 : 0;
}
""".replace("REQUEST_TYPE", request_type) + resize + """
int main(void)
{
    unsigned short rows = 24, cols = 80;
    pty_apply_current_size(-1, &rows, &cols);
    assert(calls == 0 && rows == 24 && cols == 80);
    fail = 1;
    pty_apply_current_size(7, &rows, &cols);
    assert(calls == 1 && rows == 24 && cols == 80);
    fail = 0;
    pty_apply_current_size(7, &rows, &cols);
    assert(calls == 2 && rows == 37 && cols == 101);
    pty_apply_current_size(7, &rows, &cols);
    assert(calls == 2);
    return 0;
}
""")
        binary = tmp / "resize"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
print("PASS: PTY resize preserves ioctl request bits for signed and wide ABIs")

# stdenvNoCC exports empty AR/RANLIB; Android archive commands must name NDK tools.
android = (root / "nix/android.nix").read_text()
assert '"-DCMAKE_AR=${tools}/llvm-ar"' in android
assert '"-DCMAKE_RANLIB=${tools}/llvm-ranlib"' in android
assert '"-DCMAKE_SYSTEM_NAME=Android"' in android
assert '"-DCMAKE_ANDROID_NDK=${ndk}"' in android
assert '"-DCMAKE_TOOLCHAIN_FILE=${ndk}/build/cmake/android.toolchain.cmake"' in android
assert '"-DANDROID_PLATFORM=android-${api}"' in android
assert '"-DCMAKE_FIND_ROOT_PATH=${lib.concatStringsSep ";" dependencies}"' in android
print("PASS: Android static libraries use NDK archiving tools and Bionic target")

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

# PowerPC32 needs the compiler's static implementation for 64-bit C11 atomics.
assert "atomicFallback = musl.stdenv.hostPlatform.isPower && musl.stdenv.hostPlatform.is32bit;" in linux
libraries = next(line for line in linux.splitlines() if '"LDLIBS=' in line)
assert '${pkgs.lib.optionalString atomicFallback " -latomic"}' in libraries
print("PASS: PowerPC32 links its 64-bit atomics statically after application objects")

# RISC-V can leave exported archive callbacks as R_RISCV_64 in static PIE.
# musl rcrt1 processes relative relocations only; Jansson's malloc then stays
# null. Local archive symbols force relative callbacks without a loader.
assert ('musl.stdenv.hostPlatform.isRiscV '
        '" -Wl,--exclude-libs,ALL"') in link
print("PASS: RISC-V static archive callbacks use local relocation binding")


# One baseline artifact serves both 32-bit ARM generations. Its channel target
# must come from the actual recipe, not an ARMv7 alias with a higher ISA floor.
assert "linux-armv6" in release.targets()
portable = (root / "nix/portable.nix").read_text()
assert ('linux-armv6 = (linux pkgs.pkgsCross.muslpi).application '
        '(args "linux-armv6");') in portable
assert "linux-armv7" not in release.targets()
print("PASS: shared 32-bit ARM target retains its ARMv6 toolchain and identity")

assert "linux-riscv64" in release.targets()
assert ('linux-riscv64 = (linux pkgs.pkgsCross.riscv64-musl).application '
        '(args "linux-riscv64");') in portable
print("PASS: RISC-V matrix target uses the static musl recipe and its own identity")

assert "linux-ppc64le" in release.targets()
assert ('linux-ppc64le = (linux pkgs.pkgsCross.musl-power).application '
        '(args "linux-ppc64le");') in portable
print("PASS: POWER8 Linux matrix target uses static musl and its own identity")

assert "linux-ppc32" in release.targets()
assert 'linux-ppc32 = (linux (import pkgs.path {' in portable
assert 'crossSystem.config = "powerpc-unknown-linux-musl";' in portable
assert '})).application (args "linux-ppc32");' in portable
print("PASS: big-endian PowerPC32 matrix target selects static musl")



# Bootstrap download failure must try the next pinned URL before nix-build.
# Stub only Nix commands, exercising the actual production Make recipe offline.
with tempfile.TemporaryDirectory(prefix="release-fetch-", dir=root / "build") as tmp:
    tmp = Path(tmp)
    for name in ("Makefile", "META", "config.mk"):
        shutil.copyfile(root / name, tmp / name)
    commands = tmp / "bin"
    commands.mkdir()
    for name, body in {
        "nix-instantiate": '''case "$*" in
            *sha256*) echo '"sha256-fixture"';;
            *) echo '"https://first.test/source https://second.test/source"';;
        esac''',
        "nix-prefetch-url": '''printf '%s\\n' "$*" >> "$FETCH_LOG"
        case "$FETCH_MODE:$*" in
            cached:*) exit 0;;
            fallback:*second.test*) exit 0;;
        esac
        exit 1''',
        "nix-build": 'echo build >> "$FETCH_LOG"',
    }.items():
        path = commands / name
        path.write_text("#!/bin/sh\n" + body + "\n")
        path.chmod(0o755)
    log = tmp / "fetch.log"
    env = dict(os.environ, PATH=str(commands) + os.pathsep + os.environ["PATH"],
               FETCH_LOG=str(log))
    for mode, expected, success in (("fallback", 2, True), ("cached", 1, True),
                                    ("failed", 2, False)):
        log.write_text("")
        result = subprocess.run(["make", "-s", "prod-linux-x86_64", "BUILD_VERSION=fixture"],
                                cwd=tmp, env=dict(env, FETCH_MODE=mode),
                                capture_output=True, text=True)
        lines = log.read_text().splitlines()
        fetches = [line for line in lines if line != "build"]
        assert len(fetches) == expected, (mode, result, lines)
        assert all("--unpack --name source" in line and "sha256-fixture" in line
                   for line in fetches), lines
        assert (result.returncode == 0) == success, (mode, result)
        assert (lines[-1] == "build") == success, (mode, lines)
        if mode != "cached":
            assert "first.test" in fetches[0] and "second.test" in fetches[1], lines
print("PASS: production bootstrap fallbacks preserve the hash and fail closed")

# Download-page behavior is native HTML: families disclose release tables,
# including runnable debug files, checksums and installation instructions.
class Downloads(HTMLParser):
    def __init__(self):
        super().__init__()
        self.stack = []
        self.nodes = []

    def handle_starttag(self, tag, attrs):
        node = {"tag": tag, "attrs": dict(attrs), "parents": tuple(self.stack), "text": ""}
        self.nodes.append(node)
        if tag not in ("meta", "link", "br", "hr", "img", "input", "source", "wbr"):
            self.stack.append(node)

    def handle_endtag(self, tag):
        assert self.stack and self.stack[-1]["tag"] == tag, (tag, self.stack[-1]["tag"])
        self.stack.pop()

    def handle_data(self, data):
        for node in self.stack:
            node["text"] += data


page = Downloads()
page.feed((root / "www/downloads.html").read_text())
assert not page.stack
nodes = page.nodes
ids = [n["attrs"]["id"] for n in nodes if "id" in n["attrs"]]
assert len(ids) == len(set(ids)), "duplicate download-page anchor"
assert not any(n["tag"] == "script" for n in nodes)
families = [n for n in nodes if n["tag"] == "details"]
assert {n["attrs"]["id"] for n in families} == {"linux", "macos", "windows", "freebsd", "openbsd", "netbsd"}
for family in families:
    assert "open" not in family["attrs"]
    summaries = [n for n in nodes if n["tag"] == "summary" and n["parents"][-1] is family]
    assert len(summaries) == 1
    assert summaries[0]["text"].strip().lower() == family["attrs"]["id"]
    assert not any(n["tag"] == "a" and summaries[0] in n["parents"] for n in nodes)
    assert "Install on " in family["text"]
    tier = next(n for n in reversed(family["parents"]) if n["tag"] == "section")
    expected = "tier-1" if family["attrs"]["id"] in ("linux", "macos", "windows") else "tier-2"
    assert tier["attrs"]["aria-labelledby"] == expected

downloads = [n for n in nodes if n["tag"] == "a" and "download" in n["attrs"].get("class", "").split()]
assert downloads
counts = {}
for link in downloads:
    href = link["attrs"]["href"]
    assert href.startswith("https://github.com/snajpa/snajpagent/releases/download/")
    version, filename = href.split("/download/", 1)[1].split("/", 1)
    counts[version] = counts.get(version, 0) + 1
    assert not any(word in filename for word in ("symbols", "source", "dependencies"))
    family = next(n for n in reversed(link["parents"]) if n["tag"] == "details")
    release_section = next(n for n in reversed(link["parents"]) if n["tag"] == "section")
    assert family in release_section["parents"]
    assert version in release_section["text"]
    row = next(n for n in reversed(link["parents"]) if n["tag"] == "tr")
    cells = [n for n in nodes if n["tag"] == "td" and n["parents"][-1] is row]
    assert len(cells) == 3 and all(n["text"].strip() for n in cells)
    hashes = [n for n in nodes if "sha256" in n["attrs"].get("class", "").split() and row in n["parents"]]
    assert len(hashes) == 1 and re.fullmatch(r"[0-9a-f]{64}", hashes[0]["text"].strip())
    if "debug" in filename or "-" in version:
        position = nodes.index(link)
        assert any(n["tag"] == "h4" and "Debug" in n["text"] and release_section in n["parents"]
                   for n in nodes[:position])
assert counts == {"0.99.3": 16, "0.99.2": 11, "0.99.2-9d98036": 11, "0.99.1": 16}
for node in nodes:
    if "coming-soon" in node["attrs"].get("class", "").split():
        assert node["tag"] == "p" and "(coming soon)" in node["text"]
        assert not any(n["tag"] in ("a", "details") and node in n["parents"] for n in nodes)
assert "esp32" not in (root / "www/downloads.html").read_text().lower()
mac = next(n for n in families if n["attrs"]["id"] == "macos")
assert "xattr -d com.apple.quarantine ./snajpagent" in mac["text"]
assert "shasum -a 256 FILE.tar.gz" in mac["text"]
assert "xattr -r" not in mac["text"] and "spctl --master-disable" not in mac["text"]
for node in nodes:
    for target in node["attrs"].get("aria-labelledby", "").split():
        assert target in ids
print("PASS: collapsed OS-family downloads, release/debug rows, checksums and setup")
