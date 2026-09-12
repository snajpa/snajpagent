#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Focused packaging/channel regressions; synthetic bytes, no publication."""
import argparse
from html.parser import HTMLParser
import importlib.util
import json
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
    # The documented tag recipe requires an operator-supplied version and must
    # not recreate a published literal tag when copied without preparation.
    policy = (root / "RELEASE.md").read_text()
    recipe = next(block for block in re.findall(r"```sh\n(.*?)```", policy, re.S)
                  if "git tag -a" in block)
    commands = tmp / "example-bin"
    commands.mkdir()
    marker = tmp / "tag-attempted"
    git_stub = commands / "git"
    git_stub.write_text("#!/bin/sh\nprintf tag > '" + str(marker) + "'\n")
    git_stub.chmod(0o700)
    environment = dict(os.environ, PATH=str(commands) + os.pathsep + os.environ["PATH"])
    environment.pop("APPROVED_VERSION", None)
    attempted = subprocess.run(["sh", "-eu", "-c", recipe], cwd=tmp,
                               env=environment, capture_output=True, text=True)
    assert attempted.returncode != 0 and not marker.exists(), attempted
    assert "operator-approved" in attempted.stderr, attempted.stderr
    print("PASS: documented release recipe requires an explicit approved version")
    if shutil.which("groff"):
        guidance = (root / "AGENTS.md").read_text()
        render_command = re.search(r"`(groff [^`]*snajpagent\.1)`", guidance).group(1)
        rendered = subprocess.run(render_command.split(), cwd=root, check=True,
                                  capture_output=True, text=True)
        assert "›" in rendered.stdout and "»" in rendered.stdout, "manual lost UTF-8 prompt glyphs"
        assert not rendered.stderr, rendered.stderr
        print("PASS: documented manual rendering preserves UTF-8 prompt glyphs")
    # Implemented standalone targets need a matching user-manual build entry.
    matrix = re.search(r"^PROD_TARGETS = (.+)$", (root / "Makefile").read_text(), re.M)
    manual = (root / "snajpagent.1").read_text()
    for target in matrix.group(1).split():
        assert target in manual, "missing manual build entry: " + target
    print("PASS: manual covers every implemented production build target")
    # Build-target mentions alone do not provide a usable first-run workflow.
    getting_started = manual.split('.SH "Getting started"', 1)[1].split('.SH ', 1)[0]
    downloads = (root / "www/downloads.html").read_text()
    for family in sorted({target.split("-")[1] for target in matrix.group(1).split()}):
        label = re.search(r'id="' + family + r'">\s*<summary>([^<]+)</summary>',
                          downloads).group(1)
        heading = '.SS "Install on ' + label + '"'
        assert heading in getting_started, "missing first-run instructions: " + label
        setup = getting_started.split(heading, 1)[1].split('.SS ', 1)[0]
        assert ".nf\n" in setup and "snajpagent" in setup, "missing example: " + label
        assert "Install on " + label in (root / "README.md").read_text(), label
    windows_setup = getting_started.split('.SS "Install on Windows"', 1)[1].split('.SS ', 1)[0]
    assert "Get\\-FileHash" in windows_setup and "certutil" in windows_setup
    assert "USERPROFILE" in windows_setup and "PowerShell" in windows_setup
    assert "cksum \\-a SHA256" in getting_started, "NetBSD legacy checksum command"
    for family in ("Linux", "macOS", "Windows", "FreeBSD", "OpenBSD", "NetBSD"):
        assert f'href="manual.html#Install_on_{family}"' in downloads
    print("PASS: first-run manual and README cover every released platform family")
    # Source-size reporting must never reject large files or emit budget warnings.
    size_tree = tmp / "source-size"
    size_tree.mkdir()
    for name in ("Makefile", "META", "config.mk"):
        shutil.copyfile(root / name, size_tree / name)
    (size_tree / "src").mkdir()
    (size_tree / "tests").mkdir()
    for prod_c, prod_h, test_c in ((65537, 2, 1), (3, 65537, 1),
                                  (3, 2, 65537), (65537, 65537, 65537),
                                  (2001, 2, 1), (3, 2, 1)):
        for name, count in (("src/report.c", prod_c), ("src/report.h", prod_h),
                            ("tests/report.c", test_c)):
            (size_tree / name).write_text("\n" * count)
        result = subprocess.run(["make", "--no-print-directory", "sizecheck",
                                 "BUILD_VERSION=fixture"], cwd=size_tree,
                                capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        lines = result.stdout.splitlines()
        assert lines[:3] == [f"production C lines: {prod_c}",
                             f"production header lines: {prod_h}",
                             f"test C lines: {test_c}"], lines
        assert len(lines) == 4 and lines[3].startswith("largest production C/header file:"), lines
        assert not any(word in result.stdout.lower() for word in
                       ("soft", "hard", "limit", "budget", "trigger", "review")), result.stdout
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

android_libs = next(line for line in android.splitlines() if '"CURL_LIBS=' in line)
assert "sed 's/-l-pthread/-pthread/g'" in android_libs
thread_flags = subprocess.check_output(["sed", "s/-l-pthread/-pthread/g"],
    input="-lcurl -l-pthread -lmbedtls -pthread -l-pthread\n", text=True)
assert thread_flags == "-lcurl -pthread -lmbedtls -pthread -pthread\n"
print("PASS: Android curl pthread flags remain compiler switches")

# Android API24 lacks nl_langinfo; select Bionic's built-in UTF-8 locale directly.
platform = (root / "src/platform.c").read_text()
locale_init = re.findall(r"bool\nsnag_text_locale_init\(void\)\n\{.*?\n}",
                         platform, re.S)[-1]
with tempfile.TemporaryDirectory(prefix="android-locale-", dir=root / "build") as tmp:
    tmp = Path(tmp)
    source = tmp / "locale.c"
    source.write_text("""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#define __ANDROID__ 1
#define LC_CTYPE 0
static bool fail;
static char *setlocale(int category, const char *name)
{
    assert(category == LC_CTYPE && !strcmp(name, "C.UTF-8"));
    return fail ? NULL : "C.UTF-8";
}
""" + locale_init + """
int main(void)
{
    assert(snag_text_locale_init());
    fail = true;
    assert(!snag_text_locale_init());
    return 0;
}
""")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(source),
                    "-o", str(tmp / "locale")], check=True)
    subprocess.run([str(tmp / "locale")], check=True)
print("PASS: Android locale initialization needs no API26 langinfo symbol")

# API25 Bionic wcwidth treats every nonzero character as one cell.
width = re.findall(r"int\nsnag_char_width\(uint32_t cp\)\n\{.*?\n}", platform, re.S)[-1]
with tempfile.TemporaryDirectory(prefix="android-width-", dir=root / "build") as tmp:
    tmp = Path(tmp)
    source = tmp / "width.c"
    source.write_text(r"""
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#define __ANDROID__ 1
#define wcwidth(cp) ((cp) > 0)
#define uc_width(cp, encoding) (assert(!strcmp(encoding, "UTF-8")), \
    (cp) == 0 ? 0 : (cp) == '\n' ? -1 : (cp) == 0x301 ? 0 : \
    (cp) == 0x4e2d || (cp) == 0x1f600 ? 2 : 1)
""" + width + r"""
int main(void)
{
    assert(snag_char_width('A') == 1);
    assert(snag_char_width('\n') == -1);
    assert(snag_char_width(0) == 0);
    assert(snag_char_width(0x301) == 0);
    assert(snag_char_width(0x4e2d) == 2);
    assert(snag_char_width(0x1f600) == 2);
    assert(snag_char_width(0xd800) == -1);
    assert(snag_char_width(0x110000) == -1);
    return 0;
}
""")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(source),
                    "-o", str(tmp / "width")], check=True)
    subprocess.run([str(tmp / "width")], check=True)
assert '-I${unistring}/include' in android
assert '-L${unistring}/lib -lunistring' in android
print("PASS: Android character widths use the static Unicode library")

# Native Bionic regex does not provide the required Unicode character classes.
assert 'regex = (import ./windows-regex.nix' in android
assert '-I${regex}/include' in android
assert '-L${regex}/lib -lsnagregex' in android
assert "--replace-fail '__REPB_PREFIX(used)' '__REPB_PREFIX(snag_used)'" in android
print("PASS: Android links the shared static Unicode regex implementation")

# The system shell is shared by command defaults and EDITOR on Android.
assert 'execl(SNAG_SYSTEM_SHELL, "sh", "-c",' in platform
shell_start = platform.index("#if defined(__ANDROID__)\n#define SNAG_SYSTEM_SHELL")
shell_end = platform.index("\nint\nsnag_hostname", shell_start)
with tempfile.TemporaryDirectory(prefix="android-shell-", dir=root / "build") as tmp:
    tmp = Path(tmp)
    source = tmp / "shell.c"
    source.write_text("#include <assert.h>\n#include <stdlib.h>\n#include <string.h>\n" +
                      platform[shell_start:shell_end] + """
int main(void)
{
    char *shell = snag_default_shell();
    assert(shell && !strcmp(shell, EXPECTED_SHELL));
    free(shell);
    return 0;
}
""")
    for target, expected in (([], "/bin/sh"), (["-D__ANDROID__"], "/system/bin/sh")):
        subprocess.run(["cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Werror",
                        *target, f'-DEXPECTED_SHELL="{expected}"', str(source),
                        "-o", str(tmp / "shell")], check=True)
        subprocess.run([str(tmp / "shell")], check=True)
print("PASS: Android command and editor shell use the native system path")

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
# listing only current stable executables, requirements and installation links.
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
families = [n for n in nodes if "os-family" in n["attrs"].get("class", "").split()]
assert {n["attrs"]["id"] for n in families} == {target.split("-")[0] for target in release.targets()}
for family in families:
    assert family["tag"] == "details" and "open" not in family["attrs"]
    summaries = [n for n in nodes if n["tag"] == "summary" and n["parents"][-1] is family]
    assert len(summaries) == 1
    assert summaries[0]["text"].strip().lower() == family["attrs"]["id"]
    assert not any(n["tag"] == "a" and summaries[0] in n["parents"] for n in nodes)
    assert "Install on " in family["text"]
    assert sum(n["tag"] == "table" and family in n["parents"] for n in nodes) == 1

channel = {meta["target"]: meta for _, meta in release.load_channel(root / "www/latest")}
versions = {meta["version"] for meta in channel.values()}
assert len(versions) == 1 and "-" not in next(iter(versions))
downloads = [n for n in nodes if n["tag"] == "a" and "download" in n["attrs"].get("class", "").split()]
assert len(downloads) == len(channel)
seen = set()
for link in downloads:
    row = next(n for n in reversed(link["parents"]) if n["tag"] == "tr")
    target = row["attrs"]["data-target"]
    assert target not in seen and target in channel
    seen.add(target)
    meta = channel[target]
    assert link["attrs"]["href"] == meta["url"]
    assert not any(word in meta["url"].split("/")[-1] for word in ("symbols", "source", "debug"))
    family = next(n for n in reversed(link["parents"]) if "os-family" in n["attrs"].get("class", "").split())
    assert target.startswith(family["attrs"]["id"] + "-")
    cells = [n for n in nodes if n["tag"] == "td" and n["parents"][-1] is row]
    assert len(cells) == 3 and all(n["text"].strip() for n in cells)
    assert any(word in cells[1]["text"] for word in ("+", "later", "ABI", "–")), cells[1]["text"]
    hashes = [n for n in nodes if "sha256" in n["attrs"].get("class", "").split() and row in n["parents"]]
    assert len(hashes) == 1 and hashes[0]["text"].strip() == meta["sha256"]
    assert any(n["tag"] == "details" and "open" not in n["attrs"] for n in hashes[0]["parents"])
    size = next(n for n in nodes if "file-size" in n["attrs"].get("class", "").split() and row in n["parents"])
    assert size["attrs"]["title"] == f'{meta["size"]:,} bytes'
assert seen == set(channel)
html = (root / "www/downloads.html").read_text()
assert 'href="https://github.com/snajpa/snajpagent/releases">Older releases</a>' in html
assert "coming-soon" not in html and "Tier 1" not in html and "latest-dev" not in html
assert "6.12" not in html
linux = next(n for n in families if n["attrs"]["id"] == "linux")
assert "Kernel baseline" in linux["text"] and "older kernels may work" in linux["text"]
for version in ("2.6.39+", "3.7+", "3.13+", "4.15+", "2.4.27+"):
    assert version in linux["text"]
mac = next(n for n in families if n["attrs"]["id"] == "macos")
assert "xattr -d com.apple.quarantine ./snajpagent" in mac["text"]
assert "xattr -r" not in mac["text"] and "spctl --master-disable" not in mac["text"]
for node in nodes:
    for target in node["attrs"].get("aria-labelledby", "").split():
        assert target in ids
print("PASS: latest-stable-only downloads match all channel assets, requirements, hashes and sizes")


# Keep Poppler's own static test consumers linked through the same ordered
# font dependencies as the application; CMake's imported archives omit them.
linux = (root / "nix/linux.nix").read_text()
assert "./poppler-static-fonts.patch" in linux
fonts = (root / "nix/poppler-static-fonts.patch").read_text()
assert "+  if(NOT BUILD_SHARED_LIBS)" in fonts
assert "+    pkg_check_modules(STATIC_FONTS REQUIRED fontconfig freetype2)" in fonts
assert "+    set_property(TARGET Fontconfig::Fontconfig APPEND PROPERTY" in fonts
assert 'INTERFACE_LINK_LIBRARIES "${STATIC_FONTS_STATIC_LDFLAGS}"' in fonts
print("PASS: static PDF consumers retain ordered font-library dependencies")

# External PDF headers contain intentionally unused parameters. Keep application
# warnings fatal while marking only pkg-config include directories as system.
pdf_flags = next(line for line in (root / "config.mk").read_text().splitlines()
                 if line.startswith("PDF_CFLAGS ?="))
pdf_flags = pdf_flags.replace("$(shell pkg-config --cflags poppler libpng)",
                              "-I/fixture/poppler -DKEEP_FLAG=1 -I/fixture/png")
actual = subprocess.check_output(["make", "-s", "--no-print-directory", "-f", "-"],
    input=pdf_flags + '\nall:\n\t@printf "%s\\n" "$(PDF_CFLAGS)"\n', text=True)
assert actual.split() == ["-isystem", "/fixture/poppler", "-DKEEP_FLAG=1", "-isystem", "/fixture/png"]
pdf_line = next(line for line in linux.splitlines() if '"PDF_CFLAGS=' in line)
expression = re.search(r"sed -E '([^']+)'", pdf_line).group(1)
actual = subprocess.check_output(["sed", "-E", expression],
    input="-I/fixture/poppler -DKEEP_FLAG=1 -I/fixture/png", text=True)
assert actual.split() == ["-isystem", "/fixture/poppler", "-DKEEP_FLAG=1", "-isystem", "/fixture/png"]
print("PASS: PDF system include paths preserve unrelated compiler flags")

# Nixpkgs defaults to a store-only fallback font directory. Portable binaries
# retain the host Fontconfig configuration and host font directory fallback.
assert 'pkgs.lib.hasPrefix "--with-default-fonts=" flag' in linux
assert '"--with-default-fonts=/usr/share/fonts,/usr/local/share/fonts"' in linux
pdf_recipe = linux.split("pdf = (static.poppler.override {", 1)[1].split("}).overrideAttrs", 1)[0]
assert "inherit fontconfig;" in pdf_recipe
print("PASS: portable PDF font fallback uses host directories")

# Nixpkgs curlMinimal disables WebSockets unless explicitly requested. Realtime
# voice uses the linked library, covered at runtime by test_provider_transport.
curl_recipe = linux.split("curl = (static.curlMinimal.override {", 1)[1].split("}).overrideAttrs", 1)[0]
assert "websocketSupport = true;" in curl_recipe
print("PASS: portable Linux curl enables realtime WebSockets")

# Static musl needs linked device clients rather than miniaudio's dlopen path.
for flag in ("MA_NO_RUNTIME_LINKING", "MA_ENABLE_ONLY_SPECIFIC_BACKENDS",
             "MA_ENABLE_ALSA", "MA_ENABLE_PULSEAUDIO"):
    assert "-D" + flag in linux
assert '"AUDIO_DEVICE_LIBS=$($PKG_CONFIG --static --libs alsa libpulse)"' in linux
assert 'propagatedBuildInputs = [ static.libsndfile ];' in linux
assert 'Requires.private: sndfile' in linux
assert '#define ALSA_CONFIG_DIR "/usr/share/alsa"' in linux
assert 'substituteInPlace include/config.h' in linux
assert '"-Ddaemon=false" "-Dclient=true"' in linux
print("PASS: portable Linux links audio clients and uses host ALSA configuration")

# The portable recipe uses TARGET_OS=Windows, while native Windows hosts may
# report Windows_NT. Neither may inherit the POSIX dlopen link flags.
audio_flags = next(line for line in (root / "config.mk").read_text().splitlines()
                   if line.startswith("AUDIO_DEVICE_LIBS ?="))
for target, expected in (
    ("Windows", ["-lole32", "-lwinmm"]),
    ("Windows_NT", ["-lole32", "-lwinmm"]),
    ("Darwin", ["-framework", "CoreFoundation", "-framework", "CoreAudio",
                "-framework", "AudioToolbox"]),
    ("Linux", ["-ldl", "-lm"]),
):
    actual = subprocess.check_output(["make", "-s", "--no-print-directory", "-f", "-",
                                      "TARGET_OS=" + target],
        input=audio_flags + '\nall:\n\t@printf "%s\\n" "$(AUDIO_DEVICE_LIBS)"\n', text=True)
    assert actual.split() == expected, (target, actual)
print("PASS: audio device link flags follow native and portable target OS names")

# zlib's Windows static archive is libzs.a, while its upstream pkg-config
# template still advertises -lz. Fix the producer, not individual consumers.
windows = (root / "nix/windows.nix").read_text()
zlib_recipe = windows.split("zlib = ", 1)[1].split("  brotli =", 1)[0]
assert '"$out/lib/pkgconfig/zlib.pc"' in zlib_recipe
expression = re.search(r"sed -i '([^']+)'", zlib_recipe).group(1)
actual = subprocess.check_output(["sed", expression],
    input="Libs: -L/fixture/lib -lz\nCflags: -I/fixture/include\n", text=True)
assert actual == "Libs: -L/fixture/lib -lzs\nCflags: -I/fixture/include\n"
assert '"CURL_LIBS=$($PKG_CONFIG --static --libs libcurl)"' in windows
print("PASS: Windows zlib metadata names its static archive for every consumer")

# Exercise the patched FFmpeg UTF conversion with mock Win32/allocation calls.
# The patch retains the full function as context; no SDK, device or download.
ffmpeg_patch = (root / "nix/ffmpeg-legacy-windows.patch").read_text()
wide_patch = ffmpeg_patch.split("+++ b/libavutil/wchar_filename.h\n", 1)[1]
wide_source = "\n".join(line[1:] for line in wide_patch.splitlines()
                        if line.startswith((" ", "+")))
wide_function = re.search(r"static inline int wchartocp\(.*?\n}", wide_source, re.S).group(0)
with tempfile.TemporaryDirectory(prefix="ffmpeg-wide-", dir=root / "build") as tmp:
    tmp = Path(tmp)
    source = tmp / "wide.c"
    source.write_text(r"""
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define wchar_t uint16_t
#define CP_UTF8 65001u
typedef unsigned long DWORD;
static unsigned calls;
static int fail_convert, fail_alloc;
#if _WIN32_WINNT >= 0x0600
#define WC_ERR_INVALID_CHARS 0x80u
#endif
static int WideCharToMultiByte(unsigned cp, DWORD flags, const wchar_t *input,
    int count, char *out, int size, const char *fallback, int *used)
{
    assert(input && count == -1 && !fallback && !used);
#if _WIN32_WINNT >= 0x0600
    assert(flags == (cp == CP_UTF8 ? WC_ERR_INVALID_CHARS : 0));
#else
    assert(flags == 0);
    (void)cp;
#endif
    ++calls;
    if (fail_convert) return 0;
    if (out) { assert(size == 3); memcpy(out, "ok", 3); }
    return 3;
}
static void *av_malloc_array(size_t n, size_t size)
{
    return fail_alloc ? NULL : malloc(n * size);
}
""" + wide_function + r"""
int main(void)
{
    const wchar_t valid[][6] = {{0}, {'a',0}, {0x4e2d,0},
        {0xd800,0xdc00,0}, {0xdbff,0xdfff,0}, {'a',0xd83d,0xde00,'b',0}};
    const wchar_t invalid[][4] = {{0xd800,0}, {0xdbff,'a',0},
        {0xdc00,0}, {0xdfff,0}, {0xd800,0xd800,0xdc00,0}, {0xdc00,0xd800,0}};
    char *out;
    for (size_t i = 0; i < sizeof(valid)/sizeof(valid[0]); ++i) {
        calls = 0; out = NULL;
        assert(wchartocp(CP_UTF8, valid[i], &out) == 0);
        assert(calls == 2 && out && !strcmp(out, "ok")); free(out);
    }
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        calls = 0; out = (char *)1;
#if _WIN32_WINNT >= 0x0600
        fail_convert = 1; /* Modern API rejects invalid UTF-16. */
#endif
        assert(wchartocp(CP_UTF8, invalid[i], &out) < 0 && !out && errno == EINVAL);
        assert(calls == (_WIN32_WINNT >= 0x0600 ? 1u : 0u));
        fail_convert = 0;
        assert(wchartocp(0, invalid[i], &out) == 0); free(out); /* ACP unchanged. */
    }
    fail_convert = 1; out = (char *)1;
    assert(wchartocp(CP_UTF8, valid[1], &out) < 0 && !out && errno == EINVAL);
    fail_convert = 0; fail_alloc = 1; out = (char *)1;
    assert(wchartocp(CP_UTF8, valid[1], &out) < 0 && !out && errno == ENOMEM);
    return 0;
}
""")
    for baseline in ("0x0500", "0x0502", "0x0600"):
        binary = tmp / ("wide-" + baseline)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-D_WIN32_WINNT=" + baseline, str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
print("PASS: FFmpeg pre-Vista UTF-16 validation preserves modern and ACP paths")

# Check CryptoAPI result propagation and cleanup without calling an OS RNG.
random_patch = ffmpeg_patch.split("+++ b/libavutil/random_seed.c\n", 1)[1]
random_source = "\n".join(line[1:] for line in random_patch.splitlines()
                          if line.startswith((" ", "+")))
random_function = re.search(r"static int win32_random_bytes\(.*?\n}", random_source, re.S).group(0)
with tempfile.TemporaryDirectory(prefix="ffmpeg-random-", dir=root / "build") as tmp:
    tmp = Path(tmp)
    source = tmp / "random.c"
    source.write_text(r"""
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef unsigned long HCRYPTPROV;
typedef uint32_t DWORD;
typedef int BOOL;
#define PROV_RSA_FULL 1
#define CRYPT_VERIFYCONTEXT 0xf0000000u
#define CRYPT_SILENT 0x40u
#define AVERROR_EXTERNAL -123
static unsigned acquire_calls, generate_calls, release_calls;
static int fail;
static BOOL CryptAcquireContextA(HCRYPTPROV *provider, const char *container,
                                 const char *name, DWORD type, DWORD flags)
{
    ++acquire_calls;
    assert(!container && !name && type == PROV_RSA_FULL);
    assert(flags == (CRYPT_VERIFYCONTEXT | CRYPT_SILENT));
    *provider = 42;
    return fail != 1;
}
static BOOL CryptGenRandom(HCRYPTPROV provider, DWORD len, uint8_t *buf)
{
    ++generate_calls;
    assert(provider == 42 && len <= 16);
    if (fail == 2) return 0;
    memset(buf, 0xa5, len);
    return 1;
}
static BOOL CryptReleaseContext(HCRYPTPROV provider, DWORD flags)
{
    ++release_calls;
    assert(provider == 42 && !flags);
    return fail != 3;
}
""" + random_function + r"""
int main(void)
{
    uint8_t buf[16];
    for (fail = 0; fail < 4; ++fail) {
        acquire_calls = generate_calls = release_calls = 0;
        assert(win32_random_bytes(buf, sizeof(buf)) == (fail ? AVERROR_EXTERNAL : 0));
        assert(acquire_calls == 1 && generate_calls == (fail == 1 ? 0u : 1u));
        assert(release_calls == generate_calls);
        if (!fail) for (size_t i = 0; i < sizeof(buf); ++i) assert(buf[i] == 0xa5);
    }
    fail = 0;
    assert(win32_random_bytes(buf, 0) == 0);
#if SIZE_MAX > UINT32_MAX
    acquire_calls = generate_calls = release_calls = 0;
    assert(win32_random_bytes(buf, (size_t)UINT32_MAX + 1) == AVERROR_EXTERNAL);
    assert(!acquire_calls && !generate_calls && !release_calls);
#endif
    return 0;
}
""")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(source), "-o", str(tmp / "random")], check=True)
    subprocess.run([str(tmp / "random")], check=True)
assert '+if ! test_cpp_condition windows.h "defined(_WIN32_WINNT) && _WIN32_WINNT < 0x0600"; then' in ffmpeg_patch
assert '+    return win32_random_bytes(buf, len);' in ffmpeg_patch
assert 'coremedia bcrypt advapi32 stdatomic"' in ffmpeg_patch
assert 'pkgs.lib.optional legacy ./ffmpeg-legacy-windows.patch' in windows
assert '"--disable-autodetect" "--disable-w32threads" "--enable-pthreads"' in windows
assert '"AV_LIBS=$($PKG_CONFIG --static --libs libavformat libavcodec libavutil libswresample libswscale)"' in windows
print("PASS: legacy FFmpeg RNG cleanup, failure and length handling; static recipe wiring")
