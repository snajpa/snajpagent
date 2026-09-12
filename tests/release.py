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
    ("FreeBSD", ["-lm"]),
    ("OpenBSD", ["-lm"]),
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

# The pinned JPEG MinGW patch left an outer #ifndef unclosed. Keep its Windows
# boolean ABI while allowing standalone and predeclared RPC/application types.
jpeg_patch = (root / "nix/jpeg-mingw-boolean.patch").read_text()
jpeg_source = "\n".join(line[1:] for line in jpeg_patch.splitlines()
                       if line.startswith((" ", "+")) and not line.startswith("+++"))
jpeg_types = jpeg_source[jpeg_source.index("#if defined(_WIN32)"):]
with tempfile.TemporaryDirectory(prefix="jpeg-boolean-", dir=root / "build") as tmp:
    tmp = Path(tmp)
    for name, prelude, expected in (
        ("native", "", "sizeof(int)"),
        ("windows", "#define _WIN32 1\n", "1"),
        ("rpc", "#define _WIN32 1\n#define __RPCNDR_H__ 1\ntypedef unsigned char boolean;\n", "1"),
        ("declared", "#define _WIN32 1\n#define HAVE_BOOLEAN 1\ntypedef unsigned char boolean;\n", "1"),
    ):
        source = tmp / (name + ".c")
        source.write_text(prelude + jpeg_types +
            '\n_Static_assert(sizeof(boolean) == ' + expected + ', "boolean ABI");\n'
            '_Static_assert(FALSE == 0 && TRUE == 1, "boolean values");\nint main(void) { return 0; }\n')
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(source),
                        "-o", str(tmp / name)], check=True)
assert 'builtins.baseNameOf patch != "mingw-boolean.patch"' in windows
assert 'old.patches ++ [ ./jpeg-mingw-boolean.patch ]' in windows
print("PASS: JPEG boolean declarations preserve native, Windows and predeclared ABI")

# Poppler rejects MinSizeRel; use the recipe's C++ compiler and native fonts.
pdf_windows = windows.split("pdf = (cmakeLibrary windows.poppler [", 1)[1].split("  av =", 1)[0]
assert 'cmakeBuildType = "Release";' in pdf_windows
assert '"-DFONT_CONFIGURATION=win32"' in pdf_windows
assert '"CXX=$CXX"' in windows
assert 'libavformat libavcodec libavutil libswresample libswscale)' in windows
print("PASS: Windows PDF selects a supported build type and static C++ interface")

png_recipe = windows.split("png = (cmakeLibrary windows.libpng [", 1)[1].split("  freetype =", 1)[0]
expression = re.search(r"sed -i '([^']+)'", png_recipe).group(1)
actual = subprocess.check_output(["sed", expression],
    input="Requires.private: zlib\nLibs.private: -lz -lm\nLibs: -L/example -lpng16\n", text=True)
assert actual == "Requires.private: zlib\nLibs.private: -lm\nLibs: -L/example -lpng16\n"
print("PASS: Windows PNG uses zlib pkg-config instead of a redundant -lz")

openjpeg_recipe = windows.split("openjpeg = ", 1)[1].split("  pdf =", 1)[0]
assert '"$out/lib/pkgconfig/libopenjp2.pc"' in openjpeg_recipe
assert "--replace-fail '-l-lpthread' '-lpthread'" in openjpeg_recipe
print("PASS: Windows OpenJPEG metadata keeps a valid pthread library name")

# Test the legacy libc++ handle-stat replacement, including reparse failure and
# large file identity/size fields, without accessing a real filesystem or SDK.
stat_patch = (root / "nix/libcxx-legacy-stat.patch").read_text()
stat_source = "\n".join(line[1:] for line in stat_patch.splitlines()
                        if line.startswith((" ", "+")) and not line.startswith("+++"))
stat_function = re.search(r"inline int stat_handle\(.*?\n}", stat_source, re.S).group(0)
with tempfile.TemporaryDirectory(prefix="libcxx-stat-", dir=root / "build") as tmp:
    tmp = Path(tmp)
    source = tmp / "stat.cpp"
    source.write_text(r"""
#include <cassert>
#include <cstdint>
#include <cstring>
using DWORD = uint32_t;
using HANDLE = void *;
using FILETIME = uint64_t;
struct BY_HANDLE_FILE_INFORMATION {
    DWORD dwFileAttributes;
    FILETIME ftLastWriteTime, ftLastAccessTime;
    DWORD nNumberOfLinks, nFileSizeHigh, nFileSizeLow, dwVolumeSerialNumber;
    DWORD nFileIndexHigh, nFileIndexLow;
};
struct StatT {
    unsigned st_mode;
    uint64_t st_mtim, st_atim, st_size, st_dev;
    uint32_t st_nlink;
    struct { unsigned char id[16]; } st_ino;
};
#define FILE_ATTRIBUTE_READONLY 1
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_REPARSE_POINT 0x400
#define _S_IFMT 0xf000
#define _S_IFDIR 0x4000
#define _S_IFREG 0x8000
#define _S_IFLNK 0xa000
#define MAXIMUM_REPARSE_DATA_BUFFER_SIZE 16384
#define FSCTL_GET_REPARSE_POINT 0x900a8
#define IO_REPARSE_TAG_SYMLINK 0xa000000cu
#define ERROR_INVALID_DATA 13
static BY_HANDLE_FILE_INFORMATION info;
static HANDLE handle = &info;
static bool fail_info, fail_reparse;
static DWORD tag, returned = 8, last_error;
static unsigned info_calls, reparse_calls;
static FILETIME filetime_to_timespec(FILETIME value) { return value; }
static void SetLastError(DWORD value) { last_error = value; }
static bool GetFileInformationByHandle(HANDLE h, BY_HANDLE_FILE_INFORMATION *out) {
    assert(h == handle); ++info_calls;
    if (fail_info) { last_error = 5; return false; }
    *out = info; return true;
}
static bool DeviceIoControl(HANDLE h, DWORD code, void *in, DWORD in_size,
                            void *out, DWORD size, DWORD *written, void *overlapped) {
    assert(h == handle && code == FSCTL_GET_REPARSE_POINT);
    assert(!in && !in_size && !overlapped && size == MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    ++reparse_calls;
    if (fail_reparse) { last_error = 6; return false; }
    std::memcpy(out, &tag, sizeof(tag)); *written = returned; return true;
}
""" + stat_function + r"""
int main() {
    info = {0, 1234567, 7654321, 3, 0xffffffffu, 0x12345678u, 42, 7, 8};
    StatT out;
    assert(stat_handle(handle, &out) == 0);
    assert(info_calls == 1 && !reparse_calls);
    assert(out.st_mode == (_S_IFREG | 0777) && out.st_nlink == 3 && out.st_dev == 42);
    assert(out.st_size == UINT64_C(0xffffffff12345678));
    assert(out.st_mtim == 1234567 && out.st_atim == 7654321);
    assert(!std::memcmp(out.st_ino.id, &info.nFileIndexHigh, 4));
    assert(!std::memcmp(out.st_ino.id + 4, &info.nFileIndexLow, 4));
    for (unsigned i = 8; i < 16; ++i) assert(out.st_ino.id[i] == 0);
    info.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY;
    assert(stat_handle(handle, &out) == 0 && out.st_mode == (_S_IFDIR | 0555));
    info.dwFileAttributes |= FILE_ATTRIBUTE_REPARSE_POINT;
    tag = 0xa0000003u; // Mount-point tags retain the directory mode.
    assert(stat_handle(handle, &out) == 0 && out.st_mode == (_S_IFDIR | 0555));
    tag = IO_REPARSE_TAG_SYMLINK;
    assert(stat_handle(handle, &out) == 0 && out.st_mode == (_S_IFLNK | 0555));
    returned = 7;
    assert(stat_handle(handle, &out) == -1 && last_error == ERROR_INVALID_DATA);
    fail_reparse = true;
    assert(stat_handle(handle, &out) == -1 && last_error == 6);
    fail_info = true; reparse_calls = 0;
    assert(stat_handle(handle, &out) == -1 && last_error == 5 && !reparse_calls);
    return 0;
}
""")
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(source),
                    "-o", str(tmp / "stat")], check=True)
    subprocess.run([str(tmp / "stat")], check=True)
assert 'patches = (old.patches or []) ++ [ ./libcxx-legacy-stat.patch ];' in (root / "nix/windows-pty.nix").read_text()
assert "GetFileInformationByHandleEx" not in stat_function
print("PASS: legacy libc++ handle stat preserves metadata and reparse errors")

# Compare the ASCII replacements with the dependency's original regex/stream
# behavior, including malformed metadata, binary strings and diagnostic count.
ascii_patch = (root / "nix/poppler-ascii-metadata.patch").read_text()
pdf_patch, base64_patch = ascii_patch.split("--- a/goo/gbase64.cc", 1)
def patch_side(text, added):
    return "\n".join(line[1:] for line in text.splitlines()
                     if line.startswith((" ", "+" if added else "-"))
                     and not line.startswith(("+++", "---")))

with tempfile.TemporaryDirectory(prefix="poppler-ascii-", dir=root / "build") as tmp:
    tmp = Path(tmp)
    source = tmp / "ascii.cpp"
    code = r"""
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
enum PDFSubtype { subtypePDFA, subtypePDFX };
enum PDFSubtypePart { subtypePartNull, subtypePart1, subtypePart2, subtypePart3,
    subtypePart4, subtypePart5, subtypePart6, subtypePart7, subtypePart8, subtypePartNone };
enum PDFSubtypeConformance { subtypeConfNull, subtypeConfA, subtypeConfB,
    subtypeConfG, subtypeConfN, subtypeConfP, subtypeConfPG, subtypeConfU, subtypeConfNone };
struct GooString : std::string {
    using std::string::string;
    static std::string toLowerCase(std::string s) {
        for (char &c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        return s;
    }
};
struct OutStream {
    std::string text;
    void put(char c) { text += c; }
    void printf(const char *fmt, const char *s = "") { text += std::string(fmt) == "%s" ? s : fmt; }
};
"""
    for added, name in ((False, "original"), (True, "revised")):
        pdf = patch_side(pdf_patch, added)
        start = pdf.index("static bool pdfAsciiAlpha(" if added else "static PDFSubtypePart pdfPartFromString(")
        end = pdf.index("    // Write data", start)
        metadata = pdf[start:end].replace("const std::regex regex(", "static const std::regex regex(")
        # Cache only regex construction in the oracle; matching is unchanged.
        hexcode = pdf[pdf.index("        const char *c = s->c_str();", end):]
        hexcode = hexcode[:hexcode.index("    } else {")]
        base64 = patch_side(base64_patch, added)
        base64 = base64[base64.index("static void b64encodeTriplet("):]
        code += "\nnamespace " + name + " {\nstatic unsigned warnings;\n"
        code += "enum { errSyntaxWarning };\nstatic void error(int, int, const char *, const char *) { ++warnings; }\n"
        code += metadata + "\n" + base64
        code += "\nstatic std::string hex(const GooString *s) { OutStream out; auto *outStr = &out;\n"
        code += hexcode + "\nreturn out.text; }\n}\n"
    code += r"""
static void check(const std::string &s) {
    for (PDFSubtype type : {subtypePDFA, subtypePDFX})
        assert(original::pdfPartFromString(type, s) == revised::pdfPartFromString(type, s));
    original::warnings = revised::warnings = 0;
    assert(original::pdfConformanceFromString(s) == revised::pdfConformanceFromString(s));
    assert(original::warnings == revised::warnings);
}
int main() {
    for (const char *family : {"A", "X", "VT", "E", "UA", "W", "a"})
        for (char part = '0'; part <= '9'; ++part)
            for (const char *conf : {"", "A", "b", "G", "n", "p", "U", "pG", "PGx", "abcd", "zz", "1", "\xff"})
                for (const char *date : {"", ":", "2001", ":2003", "2002", "123", "12345", "::2003", "AB2003"}) {
                    std::string s = std::string("prefix PDF/") + family + "-" + part + conf + date;
                    check(s); check(s + " PDF/A-2u");
                }
    for (const char *s : {"", "PDF/", "PDF/A-", "PDF/A-PDF/X-1a:2003", "PDF/A-1 PDF/X-3pG", "PDF/A-1zzz PDF/A-2u"})
        check(s);
    uint32_t seed = 12345;
    for (unsigned i = 0; i < 2000; ++i) {
        std::string s;
        for (unsigned j = 0; j < i % 80; ++j) {
            seed = seed * 1664525u + 1013904223u; s += char(seed >> 24);
        }
        if (i % 3 == 0) s.insert(s.size() / 2, "PDF/X-3PG:2003");
        check(s);
    }
    GooString bytes;
    for (unsigned n = 0; n <= 512; ++n) {
        assert(original::gbase64Encode(bytes.data(), bytes.size()) == revised::gbase64Encode(bytes.data(), bytes.size()));
        assert(original::hex(&bytes) == revised::hex(&bytes));
        bytes += char(n);
    }
    assert(revised::gbase64Encode("f", 1) == "Zg==");
    assert(revised::gbase64Encode("foo", 3) == "Zm9v");
    return 0;
}
"""
    source.write_text(code)
    subprocess.run(["c++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", str(source),
                    "-o", str(tmp / "ascii")], check=True)
    subprocess.run([str(tmp / "ascii")], check=True, timeout=30)
print("PASS: Poppler ASCII metadata, hex and base64 match original behavior")
assert 'pkgs.lib.optional legacy ./poppler-ascii-metadata.patch' in windows

# Exercise the dependency's actual RNG body with mocked Win32 APIs. Failures
# stay fatal, including a false API result with a zero last-error value.
xml_patch = (root / "nix/libxml2-legacy-windows.patch").read_text()
xml_dict_patch = xml_patch.split("+++ b/dict.c\n", 1)[1].split("--- a/CMakeLists.txt", 1)[0]
xml_dict = "\n".join(line[1:] for line in xml_dict_patch.splitlines()
                     if line.startswith((" ", "+")))
xml_random = re.search(r"void\nxmlInitRandom\(void\) \{.*?\n}", xml_dict, re.S).group(0)
with tempfile.TemporaryDirectory(prefix="xml-windows-", dir=root / "build") as tmp:
    tmp = Path(tmp)
    source = tmp / "random.c"
    source.write_text(r"""
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
typedef unsigned long DWORD;
typedef unsigned long HCRYPTPROV;
typedef int BOOL;
typedef int NTSTATUS;
#define PROV_RSA_FULL 1
#define CRYPT_VERIFYCONTEXT 0xf0000000u
#define CRYPT_SILENT 0x40u
#define BCRYPT_USE_SYSTEM_PREFERRED_RNG 2
#define BCRYPT_SUCCESS(status) ((status) == 0)
static unsigned acquires, generates, releases, bcrypts, mutexes;
static unsigned globalRngState[2];
static int xmlRngMutex, fail;
static DWORD last_error, reported_error;
static jmp_buf aborted;
static void xmlInitMutex(int *mutex) { assert(mutex == &xmlRngMutex); ++mutexes; }
static DWORD GetLastError(void) { return last_error; }
static _Noreturn void xmlAbort(const char *format, ...) {
    va_list ap; va_start(ap, format); reported_error = va_arg(ap, DWORD); va_end(ap);
    longjmp(aborted, 1);
}
#if _WIN32_WINNT < 0x0600
static BOOL CryptAcquireContextA(HCRYPTPROV *provider, const char *container,
                                const char *name, DWORD type, DWORD flags) {
    ++acquires;
    assert(!container && !name && type == PROV_RSA_FULL);
    assert(flags == (CRYPT_VERIFYCONTEXT | CRYPT_SILENT));
    if (fail == 1) { last_error = 71; return 0; }
    *provider = 123; return 1;
}
static BOOL CryptGenRandom(HCRYPTPROV provider, DWORD length, unsigned char *bytes) {
    ++generates;
    assert(provider == 123 && length == sizeof(globalRngState));
    if (fail >= 2) { last_error = fail == 2 ? 72 : 0; return 0; }
    memset(bytes, 0x5a, length); return 1;
}
static BOOL CryptReleaseContext(HCRYPTPROV provider, DWORD flags) {
    ++releases; assert(provider == 123 && flags == 0);
    last_error = 99; return 1;
}
#else
static NTSTATUS BCryptGenRandom(void *algorithm, unsigned char *bytes,
                               DWORD length, DWORD flags) {
    ++bcrypts;
    assert(!algorithm && length == sizeof(globalRngState));
    assert(flags == BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (fail) { last_error = 73; return -1; }
    memset(bytes, 0x5a, length); return 0;
}
#endif
""" + xml_random + r"""
int main(void) {
    for (fail = 0; fail < 4; ++fail) {
        acquires = generates = releases = bcrypts = mutexes = 0;
        reported_error = 999; memset(globalRngState, 0, sizeof(globalRngState));
        if (!setjmp(aborted)) { xmlInitRandom(); assert(!fail); }
        else assert(fail && reported_error != 999);
        assert(mutexes == 1);
#if _WIN32_WINNT < 0x0600
        assert(acquires == 1 && bcrypts == 0);
        assert(generates == (fail != 1) && releases == (fail != 1));
        if (fail) assert(reported_error == (fail == 1 ? 71u : fail == 2 ? 72u : 0u));
#else
        assert(bcrypts == 1 && !acquires && !generates && !releases);
        if (fail) assert(reported_error == 73);
#endif
        for (size_t i = 0; i < sizeof(globalRngState); ++i)
            assert(((unsigned char *)globalRngState)[i] == (fail ? 0 : 0x5a));
    }
    return 0;
}
""")
    for baseline in ("0x0500", "0x0502", "0x0600", "0x0a00"):
        binary = tmp / ("random-" + baseline)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-D_WIN32",
                        "-D_WIN32_WINNT=" + baseline, str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

    # The patch keeps libxml2's existing pthread once/mutex/TLS path for old
    # Windows; it leaves native Windows threading selected at newer baselines.
    header_patch = xml_patch.split("+++ b/include/private/threads.h\n", 1)[1].split("--- a/dict.c", 1)[0]
    header = "\n".join(line[1:] for line in header_patch.splitlines()
                       if line.startswith((" ", "+"))) + "\n#endif\n"
    (tmp / "libxml").mkdir()
    for name in ("libxml/threads.h", "pthread.h", "windows.h"):
        (tmp / name).write_text("")
    source.write_text(header + r"""
#if _WIN32_WINNT < 0x0600
#ifndef HAVE_POSIX_THREADS
#error legacy Windows must retain thread-safe initialization and TLS
#endif
#else
#ifndef HAVE_WIN32_THREADS
#error modern Windows must retain native threading
#endif
#endif
_Static_assert(_WIN32_WINNT == EXPECTED_WINVER, "do not raise or lower selected baseline");
int main(void) { return 0; }
""")
    for baseline in ("0x0500", "0x0502", "0x0600", "0x0a00"):
        subprocess.run(["cc", "-std=c11", "-Werror", "-D_WIN32", "-DLIBXML_THREAD_ENABLED",
                        "-D_WIN32_WINNT=" + baseline, "-DEXPECTED_WINVER=" + baseline,
                        "-I" + str(tmp), str(source), "-o", str(tmp / "threads")], check=True)
assert 'pkgs.lib.optional legacy ./libxml2-legacy-windows.patch' in windows
assert 'target_link_libraries(LibXml2 PRIVATE advapi32 pthread)' in xml_patch
assert 'target_link_libraries(LibXml2 PRIVATE bcrypt)' in xml_patch
assert '"-DENABLE_CNG=OFF"' in windows and '"-DENABLE_WIN32_XMLLITE=OFF"' in windows
assert '"-DWINDOWS_VERSION=${if legacy then "WS03" else "WIN7"}"' in windows
assert 's/^Cflags: /Cflags: -DLIBARCHIVE_STATIC /' in windows
print("PASS: Windows package readers keep static declarations, baseline threading and fatal RNG errors")

# macOS FFmpeg needs both the native generator compiler and SDK-targeted compiler.
# Keep the static file-codec profile independent of optional host frameworks.
macos = (root / "nix/macos.nix").read_text()
mac_av = macos.split("  av = ", 1)[1].split("  brotli = ", 1)[0]
for flag in ("--enable-cross-compile", "--target-os=darwin", "--enable-static",
             "--disable-shared", "--disable-autodetect", "--disable-network",
             "--disable-programs", "--enable-pthreads", "--enable-zlib"):
    assert '"' + flag + '"' in mac_av
assert '"--host-cc=${pkgs.stdenv.cc}/bin/cc"' in mac_av
assert '"--cc=${compiler} --target=${target} -isysroot ${sdk}"' in mac_av
assert '"--disable-postproc"' not in mac_av  # Removed in pinned FFmpeg 8.
assert '"AV_LIBS=$(pkg-config --static --libs libavformat libavcodec libavutil libswresample libswscale)"' in macos
assert 'buildInputs = [ jansson curl av pdf png freetype expat fontconfig jpeg openjpeg xml archive ] ++ networkLibraries;' in macos
assert "'MINIAUDIO_CFLAGS=-isystem ${pkgs.miniaudio.src}'" in macos
assert 'makeFlags = [ "ASMSTRIPFLAGS=" ];' in mac_av
# Upstream's assembler rule conditionally invokes STRIP via ASMSTRIPFLAGS.
# Reproduce that pre-link call and prove the production make override omits it.
with tempfile.TemporaryDirectory(prefix="macos-asm-strip-", dir=root / "build") as tmp:
    makefile = Path(tmp) / "Makefile"
    makefile.write_text("ASMSTRIPFLAGS=-x\nall:\n\t$(if $(ASMSTRIPFLAGS),false,true)\n")
    baseline = subprocess.run(["make", "-s", "-f", str(makefile)], capture_output=True)
    assert baseline.returncode != 0
    subprocess.run(["make", "-s", "-f", str(makefile), "ASMSTRIPFLAGS="], check=True)
assert '$(STRIP) -S -x "$$stage/$(BIN)"' in (root / "Makefile").read_text()
print("PASS: macOS media keeps intermediate NASM symbols and final executable stripping")

# dSYM lookup needs distinct deterministic archive member names for C/SIMD
# objects with the same basename. Exercise the dependency's actual archive rule.
archive_patch = (root / "nix/ffmpeg-darwin-archive-names.patch").read_text()
archive_hunk = archive_patch.split("+++ b/ffbuild/library.mak\n", 1)[1]
with tempfile.TemporaryDirectory(prefix="darwin-archive-", dir=root / "build") as tmp:
    tmp = Path(tmp)
    objects = ["lib/same.o", "lib/simd/same.o"]
    for i, name in enumerate(objects):
        obj = tmp / name
        obj.parent.mkdir(parents=True, exist_ok=True)
        source = obj.with_suffix(".c")
        source.write_text(f"int fixture_{i}(void) {{ return {i + 1}; }}\n")
        subprocess.run(["cc", "-c", str(source), "-o", str(obj)], check=True)
    for revised in (False, True):
        source = "\n".join(line[1:] for line in archive_hunk.splitlines()
                           if line.startswith((" ", "+" if revised else "-")))
        rule = source[source.index("$(SUBDIR)$(LIBNAME):"):source.index("\ninstall-headers:")]
        makefile = tmp / "Makefile"
        makefile.write_text("SUBDIR=\nLIBNAME=fixture.a\nOBJS=" + " ".join(objects) +
                            "\nAR=ar\nARFLAGS=rcs\nAR_O=$@\nRANLIB=ranlib\nRM=rm -f\n" + rule + "\n")
        subprocess.run(["make", "-s", "-B"], cwd=tmp, check=True)
        names = subprocess.check_output(["ar", "t", "fixture.a"], cwd=tmp, text=True).splitlines()
        assert len(names) == 2
        if revised:
            assert names == [name.replace("/", "_") for name in objects]
            for name, member in zip(objects, names):
                assert subprocess.check_output(["ar", "p", "fixture.a", member], cwd=tmp) == (tmp / name).read_bytes()
            assert not (tmp / "fixture.a.objects").exists()
        else:
            assert names == ["same.o", "same.o"], "baseline must reproduce ambiguous dSYM members"
assert './ffmpeg-darwin-archive-names.patch' in mac_av
print("PASS: Darwin FFmpeg archive rule preserves bytes with distinct C/SIMD member names")

# libpng's custom preprocessing command bypasses CMAKE_C_COMPILER_TARGET.
mac_png = macos.split("  png = ", 1)[1].split("  freetype = ", 1)[0]
assert 'cmakeFlagsArray+=("-DCMAKE_C_FLAGS=${cflags} --target=${target}")' in mac_png
mac_jpeg = macos.split("  jpeg = ", 1)[1].split("  openjpeg = ", 1)[0]
assert 'pkgs.nasm' in mac_jpeg and 'CMAKE_INSTALL_NAME_TOOL=${tools}/llvm-install-name-tool' in mac_jpeg
mac_pdf = macos.split("  pdf = ", 1)[1].split("  brotli = ", 1)[0]
assert 'cmakeBuildType = "Release";' in mac_pdf and './poppler-static-fonts.patch' in mac_pdf
assert '"-DFONT_CONFIGURATION=fontconfig"' in mac_pdf
assert '"PDF_LIBS=$(pkg-config --static --libs poppler libpng) -lc++"' in macos
assert "'CXX=${llvm.clang-unwrapped}/bin/clang++ --target=${target} -isysroot ${sdk}'" in macos
print("PASS: macOS PDF targets generated headers, NASM tools and static font/C++ dependencies")

freebsd = (root / "nix/freebsd.nix").read_text()
freebsd_archive = freebsd.split("  archive = ", 1)[1].split("  brotli = ", 1)[0]
assert '"-DLIBMD_LIBRARY=${sdk}/usr/lib/libmd.a"' in freebsd_archive
assert '] [ zlib iconv ];' in freebsd_archive
assert 'buildInputs = [ jansson curl av xml archive ]' in freebsd
assert '"-DLIBXML2_WITH_MODULES=OFF"' in freebsd and '"-DENABLE_TEST=OFF"' in freebsd_archive
print("PASS: FreeBSD Office readers use static SDK digests and application dependency wiring")
assert '-Dstatic_assert=_Static_assert' in freebsd
assert 'lib.optional legacy ./ffmpeg-legacy-libm.patch' in freebsd
math_patch = (root / "nix/ffmpeg-legacy-libm.patch").read_text()
math_added = "\n".join(line[1:] for line in math_patch.splitlines()
                        if line.startswith("+") and not line.startswith("+++"))
with tempfile.TemporaryDirectory(prefix="legacy-math-", dir=root / "build") as tmp:
    tmp = Path(tmp)
    source = tmp / "math.c"
    replacements = "\n".join(re.search(
        r"static av_always_inline (?:double|float) " + name + r"\(.*?\n}", math_added, re.S).group(0)
        for name in ("fmin", "fminf", "fmax", "fmaxf"))
    source.write_text(r"""
#include <assert.h>
#include <math.h>
#define av_always_inline inline
#define fmin replacement_fmin
#define fminf replacement_fminf
#define fmax replacement_fmax
#define fmaxf replacement_fmaxf
""" + replacements + r"""
int main(void) {
    const double values[] = {-INFINITY, -123.5, -1.0, -0.0, 0.0, 1.0, 123.5, INFINITY, NAN};
    for (unsigned i = 0; i < sizeof(values)/sizeof(values[0]); ++i) {
        for (unsigned j = 0; j < sizeof(values)/sizeof(values[0]); ++j) {
            double x = values[i], y = values[j];
            double low = replacement_fmin(x, y), high = replacement_fmax(x, y);
            float lowf = replacement_fminf((float)x, (float)y);
            float highf = replacement_fmaxf((float)x, (float)y);
            if (isnan(x) && isnan(y)) {
                assert(isnan(low) && isnan(high) && isnan(lowf) && isnan(highf));
            } else {
                double a = isnan(x) ? y : isnan(y) ? x : x < y ? x : y;
                double b = isnan(x) ? y : isnan(y) ? x : x > y ? x : y;
                assert(low == a && high == b && lowf == (float)a && highf == (float)b);
                if (x == 0 && y == 0) {
                    assert(!!signbit(low) == (!!signbit(x) || !!signbit(y)));
                    assert(!!signbit(high) == (!!signbit(x) && !!signbit(y)));
                    assert(!!signbit(lowf) == !!signbit(low));
                    assert(!!signbit(highf) == !!signbit(high));
                }
            }
        }
    }
    return 0;
}
""")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(source), "-lm",
                    "-o", str(tmp / "math")], check=True)
    subprocess.run([str(tmp / "math")], check=True)
    source.write_text("static_assert(sizeof(int) > 0, \"integer size\");\n")
    failed = subprocess.run(["cc", "-std=c11", "-Werror", "-fsyntax-only", str(source)], capture_output=True)
    assert failed.returncode != 0
    subprocess.run(["cc", "-std=c11", "-Werror", "-Dstatic_assert=_Static_assert",
                    "-fsyntax-only", str(source)], check=True)
    source.write_text("static_assert(sizeof(int) == 0, \"must fail\");\n")
    failed = subprocess.run(["cc", "-std=c11", "-Werror", "-Dstatic_assert=_Static_assert",
                             "-fsyntax-only", str(source)], capture_output=True)
    assert failed.returncode != 0, "the alias must preserve failed compile-time assertions"
print("PASS: legacy FFmpeg math preserves NaNs, infinities, signed zero and compile-time assertions")

# Clang can introduce unavailable exp2 symbols from otherwise portable pow calls.
assert 'lib.optionalString legacy " -fno-builtin-pow -fno-builtin-powf"' in freebsd
if shutil.which("clang"):
    source = ("extern double pow(double, double); extern float powf(float, float);\n"
              "double power(double x) { return pow(2, x); }\n"
              "float powerf(float x) { return powf(2, x); }\n"
              "float table[64];\n"
              "void generate(void) { for (int i = 0; i < 64; ++i) table[i] = pow(2.0, (i - 15) / 3.0); }\n")
    # Include FFmpeg's flags: vectorization can introduce exp2 even with
    # -fno-builtin-exp2. Disable the source builtins instead.
    for flags in (["-Os"], ["-O3", "-fno-math-errno", "-fno-signed-zeros"]):
        command = ["clang", "-x", "c", "-", "-S", "-emit-llvm", "-o", "-"] + flags
        before = subprocess.run(command, input=source, text=True, capture_output=True, check=True).stdout
        after = subprocess.run(command + ["-fno-builtin-pow", "-fno-builtin-powf"],
                               input=source, text=True, capture_output=True, check=True).stdout
        for symbol in (r"(exp2\(|llvm\.exp2\.f64\()", r"(exp2f\(|llvm\.exp2\.f32\()"):
            assert re.search(r"call[^\n]*@" + symbol, before), "baseline must reproduce libm rewriting"
        assert not re.search(r"@(llvm\.)?exp2f?[.(]", after), "legacy profile must not introduce exp2"
        for name in ("pow", "powf"):
            assert re.search(r"call[^\n]*@" + name + r"\(", after), "preserve available libm function"
    print("PASS: legacy FFmpeg flags prevent scalar/vectorized exp2/exp2f imports")
else:
    print("SKIP: Clang unavailable for legacy FFmpeg libm transformation regression")

# Exercise the actual snapshot comparison using each platform's stat layout.
media_source = (root / "src/media.c").read_text()
unchanged = re.search(r"static bool\nunchanged\(.*?\n}", media_source, re.S).group(0)
with tempfile.TemporaryDirectory(prefix="media-stat-", dir=root / "build") as tmp:
    tmp = Path(tmp)
    for platform in ("POSIX", "__APPLE__", "__FreeBSD__", "_WIN32"):
        source = tmp / "stat.c"
        named = platform in ("__APPLE__", "__FreeBSD__")
        mt, ct = ("st_mtimespec", "st_ctimespec") if named else ("st_mtim", "st_ctim")
        source.write_text("#include <assert.h>\n#include <stdbool.h>\n" +
            ("" if platform == "POSIX" else "#define " + platform + " 1\n") +
            "#define S_ISREG(mode) ((mode) == 1)\n"
            "typedef struct { unsigned st_dev, st_ino, st_size, st_mtime, st_ctime, st_mode;\n" +
            "struct { long tv_nsec; } " + mt + ", " + ct + "; } snag_file_info;\n" +
            unchanged + "\nint main(void) {\n"
            "snag_file_info a = {.st_mode=1}, b = a; assert(unchanged(&a,&b));\n" +
            "".join("b=a; ++b." + field + "; assert(!unchanged(&a,&b));\n"
                    for field in ("st_dev", "st_ino", "st_size", "st_mtime", "st_mode")) +
            ("" if platform == "_WIN32" else
             "".join("b=a; ++b." + field + "; assert(!unchanged(&a,&b));\n"
                     for field in ("st_ctime", mt + ".tv_nsec", ct + ".tv_nsec"))) +
            "return 0;}\n")
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(source),
                        "-o", str(tmp / "stat")], check=True)
        subprocess.run([str(tmp / "stat")], check=True)
print("PASS: media snapshot comparison preserves identity, size and platform nanoseconds")

# Old OSS headers lack inventory/version queries. Only report accessible default
# paths; all device opening/format negotiation stays in miniaudio's existing owner.
oss_patch = (root / "nix/miniaudio-oss3.patch").read_text()
oss_added = "\n".join(line[1:] for line in oss_patch.splitlines()
                       if line.startswith("+") and not line.startswith("+++"))
oss_info = re.search(r"static ma_result ma_context_get_device_info__oss\(.*?\n}", oss_added, re.S).group(0)
oss_enum = re.search(r"static ma_result ma_context_enumerate_devices__oss\(.*?\n}", oss_added, re.S).group(0)
with tempfile.TemporaryDirectory(prefix="oss3-default-", dir=root / "build") as tmp:
    tmp = Path(tmp)
    source = tmp / "oss.c"
    source.write_text(r"""
#include <assert.h>
#include <stddef.h>
#include <string.h>
typedef int ma_result;
typedef int ma_device_type;
typedef struct { int unused; } ma_context;
typedef struct { char oss[64]; } ma_device_id;
typedef struct { ma_device_id id; char name[64]; unsigned isDefault, nativeDataFormatCount; } ma_device_info;
typedef int (*ma_enum_devices_callback_proc)(ma_context*, ma_device_type, const ma_device_info*, void*);
#define MA_SUCCESS 0
#define MA_INVALID_ARGS -1
#define MA_NO_DEVICE -2
#define MA_TRUE 1
#define ma_device_type_playback 1
#define ma_device_type_capture 2
#define MA_OSS_DEFAULT_DEVICE_NAME "/dev/dsp"
#define MA_DEFAULT_PLAYBACK_DEVICE_NAME "Default playback"
#define MA_DEFAULT_CAPTURE_DEVICE_NAME "Default capture"
#define MA_ZERO_OBJECT(p) memset(p, 0, sizeof(*(p)))
#define R_OK 4
#define W_OK 2
static int permissions, access_calls, callbacks, stop;
static int access(const char *path, int mode) {
    assert(!strcmp(path, "/dev/dsp") && (mode == R_OK || mode == W_OK));
    ++access_calls; return (permissions & mode) ? 0 : -1;
}
static void ma_strncpy_s(char *out, size_t size, const char *text, size_t count) {
    assert(count == (size_t)-1 && strlen(text) < size); strcpy(out, text);
}
static int callback(ma_context *context, ma_device_type type, const ma_device_info *info, void *user) {
    assert(context && user == &permissions);
    assert(!strcmp(info->id.oss, "/dev/dsp") && info->isDefault && !info->nativeDataFormatCount);
    assert(!strcmp(info->name, type == ma_device_type_playback ? "Default playback" : "Default capture"));
    ++callbacks; return !stop;
}
""" + oss_info + "\n" + oss_enum + r"""
int main(void) {
    ma_context context = {0}; ma_device_info info; ma_device_id id;
    for (unsigned i = 0; i < 4; ++i) {
        permissions = ((i & 1) ? R_OK : 0) | ((i & 2) ? W_OK : 0);
        callbacks = access_calls = stop = 0;
        assert(ma_context_enumerate_devices__oss(&context, callback, &permissions) == MA_SUCCESS);
        assert(callbacks == !!(permissions & R_OK) + !!(permissions & W_OK) && access_calls == 2);
    }
    permissions = R_OK | W_OK; callbacks = access_calls = 0; stop = 1;
    assert(ma_context_enumerate_devices__oss(&context, callback, &permissions) == MA_SUCCESS);
    assert(callbacks == 1 && access_calls == 1);
    strcpy(id.oss, "/dev/dsp");
    memset(&info, 0xaa, sizeof(info));
    assert(ma_context_get_device_info__oss(&context, ma_device_type_capture, &id, &info) == MA_SUCCESS);
    assert(!info.nativeDataFormatCount && info.isDefault);
    strcpy(id.oss, "/dev/other"); access_calls = 0;
    assert(ma_context_get_device_info__oss(&context, ma_device_type_capture, &id, &info) == MA_NO_DEVICE);
    assert(!access_calls);
    assert(ma_context_get_device_info__oss(&context, 3, NULL, &info) == MA_INVALID_ARGS);
    assert(!access_calls);
    permissions = 0;
    assert(ma_context_get_device_info__oss(&context, ma_device_type_capture, NULL, &info) == MA_NO_DEVICE);
    assert(access_calls == 1);
    return 0;
}
""")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(source),
                    "-o", str(tmp / "oss")], check=True)
    subprocess.run([str(tmp / "oss")], check=True)
assert '#if defined(SNDCTL_SYSINFO) && defined(SNDCTL_AUDIOINFO)' in oss_patch
assert '#ifdef OSS_GETVERSION' in oss_patch
assert 'pContext->oss.versionMajor = 0;' in oss_added and 'pContext->oss.versionMinor = 0;' in oss_added
for name in ("fmin", "fminf", "fmax", "fmaxf"):
    assert '+' + name + '_args=2' in math_patch
assert '+#include "libm.h"' in math_patch
assert '+#if !defined(__FreeBSD__) || __FreeBSD__ >= 6' in math_patch
print("PASS: OSS3 defaults respect access, stop callbacks and unknown native format/version")

# Old BSD extension headers require the standard pthread declarations first.
openbsd = (root / "nix/openbsd.nix").read_text()
assert 'lib.optional legacy ./ffmpeg-bsd-thread-headers.patch' in openbsd
assert 'lib.optionalString legacy " -Dstatic_assert=_Static_assert"' in openbsd
thread_patch = (root / "nix/ffmpeg-bsd-thread-headers.patch").read_text().splitlines()
before, after = [], []
in_hunk = False
for line in thread_patch:
    if line.startswith("@@"):
        in_hunk = True
    elif in_hunk:
        if line.startswith((" ", "-")):
            before.append(line[1:])
        if line.startswith((" ", "+")):
            after.append(line[1:])
with tempfile.TemporaryDirectory(prefix="bsd-thread-headers-", dir=root / "build") as tmp:
    tmp = Path(tmp)
    (tmp / "pthread.h").write_text("typedef int pthread_t; typedef int stack_t;\n")
    (tmp / "pthread_np.h").write_text("void pthread_set_name_np(pthread_t, const char *);\n"
                                      "int pthread_stackseg_np(pthread_t, stack_t *);\n")
    source = tmp / "thread.c"
    flags = "#define HAVE_PTHREAD_SET_NAME_NP 1\n#define HAVE_PTHREAD_NP_H 1\n"
    command = ["cc", "-std=c11", "-Wall", "-Werror", "-I" + str(tmp), "-fsyntax-only", str(source)]
    source.write_text(flags + "\n".join(before))
    result = subprocess.run(command, capture_output=True)
    assert result.returncode != 0, "old extension header must reproduce missing pthread types"
    source.write_text(flags + "\n".join(after))
    subprocess.run(command, check=True)
print("PASS: FFmpeg BSD extension headers receive standard pthread declarations first")

# Intermediate OpenBSD audio(4) uses pause instead of AUDIO_FLUSH.
audio4_patch = (root / "nix/miniaudio-openbsd-audio4.patch").read_text()
audio4_after = "\n".join(line[1:] for line in audio4_patch.splitlines()
                         if line.startswith((" ", "+")) and not line.startswith("+++"))
audio4_functions = "\n".join(re.search(r"static ma_result " + name + r"\(.*?\n}",
                                      audio4_after, re.S).group(0) for name in (
    "ma_device_pause_fd__audio4", "ma_device_start__audio4",
    "ma_device_stop_fd__audio4", "ma_device_stop__audio4"))
assert 'fdInfo.play.pause = fdInfo.record.pause = 1;' in audio4_after
assert 'fdInfo.record.block_size = internalPeriodSizeInBytes;' in audio4_after
assert 'fdInfo.play.block_size = internalPeriodSizeInBytes;' in audio4_after
assert 'deviceType == ma_device_type_capture ? fdInfo.record.block_size : fdInfo.play.block_size' in audio4_after
assert '!defined(MA_AUDIO4_USE_NEW_API) && !defined(MA_AUDIO4_USE_PAUSE_API)' in audio4_after
with tempfile.TemporaryDirectory(prefix="audio4-pause-", dir=root / "build") as tmp:
    tmp = Path(tmp)
    source = tmp / "pause.c"
    source.write_text(r"""
#include <assert.h>
#include <errno.h>
#include <string.h>
typedef int ma_result;
typedef struct { int type; struct { int fdCapture, fdPlayback; } audio4; } ma_device;
struct audio_info { struct { unsigned char pause; } play, record; };
#define MA_AUDIO4_USE_PAUSE_API 1
#define MA_SUCCESS 0
#define MA_INVALID_ARGS -1
#define MA_LOG_LEVEL_ERROR 0
#define MA_ASSERT assert
#define ma_device_type_capture 1
#define ma_device_type_playback 2
#define ma_device_type_duplex 3
#define AUDIO_SETINFO 22
#define AUDIO_INITINFO(p) memset(p, 255, sizeof(*(p)))
static int calls, fail_call, descriptors[8], states[8];
static int ioctl(int fd, int request, const struct audio_info *info) {
    assert(request == AUDIO_SETINFO && calls < 8);
    assert(fd == 10 || fd == 11);
    assert(info->play.pause == info->record.pause && info->play.pause <= 1);
    descriptors[calls] = fd; states[calls] = info->play.pause; ++calls;
    if (calls == fail_call) { errno = EIO; return -1; }
    return 0;
}
static int ma_device_get_log(ma_device *device) { assert(device); return 0; }
static void ma_log_post(int log, int level, const char *text) { (void)log; (void)level; (void)text; }
static ma_result ma_result_from_errno(int error) { return -error; }
static void reset(int fail) { calls = 0; fail_call = fail; }
""" + audio4_functions + r"""
int main(void) {
    ma_device device = {ma_device_type_duplex, {10, 11}};
    reset(0);
    assert(ma_device_start__audio4(&device) == MA_SUCCESS && calls == 2);
    assert(descriptors[0] == 10 && descriptors[1] == 11 && states[0] == 0 && states[1] == 0);
    reset(0);
    assert(ma_device_stop__audio4(&device) == MA_SUCCESS && calls == 2);
    assert(states[0] == 1 && states[1] == 1);
    reset(1);
    assert(ma_device_start__audio4(&device) == -EIO && calls == 1);
    reset(2);
    assert(ma_device_start__audio4(&device) == -EIO && calls == 3);
    assert(descriptors[2] == 10 && states[2] == 1); /* roll back capture */
    reset(1);
    assert(ma_device_stop_fd__audio4(&device, 10) == -EIO && calls == 1);
    reset(0); device.audio4.fdPlayback = -1;
    assert(ma_device_start__audio4(&device) == MA_INVALID_ARGS && calls == 0);
    assert(ma_device_stop_fd__audio4(&device, -1) == MA_INVALID_ARGS && calls == 0);
    for (int type = ma_device_type_capture; type <= ma_device_type_playback; ++type) {
        device.type = type; device.audio4.fdPlayback = 11; reset(0);
        assert(ma_device_start__audio4(&device) == MA_SUCCESS && calls == 1);
        assert(descriptors[0] == (type == ma_device_type_capture ? 10 : 11));
        assert(states[0] == 0); reset(0);
        assert(ma_device_stop__audio4(&device) == MA_SUCCESS && calls == 1 && states[0] == 1);
    }
    return 0;
}
""")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(source),
                    "-o", str(tmp / "pause")], check=True)
    subprocess.run([str(tmp / "pause")], check=True)
print("PASS: OpenBSD audio(4) pause/start/stop preserves failures, duplex rollback and no drain")
