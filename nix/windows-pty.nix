# SPDX-License-Identifier: GPL-2.0-only
# Static console collection for the internal LLVM/NT5 port.
{ pkgs, windows, threads }:
let
  unwind = (windows.llvmPackages_21.libunwind.override {
    enableShared = false;
    doFakeLibgcc = false;
  }).overrideAttrs (old: {
    pname = "libunwind-windows-i686-legacy";
    prePatch = "cd ..; chmod u+w libunwind/src libunwind/src/RWMutex.hpp libunwind/src/AddressSpace.hpp";
    postPatch = "cd runtimes";
    patches = (old.patches or []) ++ [ ./libunwind-legacy-windows.patch ];
    buildInputs = (old.buildInputs or []) ++ [ threads ];
    cmakeFlags = old.cmakeFlags ++ [
      "-DCMAKE_C_FLAGS=-D_WIN32_WINNT=0x0500"
      "-DCMAKE_CXX_FLAGS=-D_WIN32_WINNT=0x0500"
    ];
  });
  cxx = (windows.llvmPackages_21.libcxx.override {
    enableShared = false;
  }).overrideAttrs (old: {
    pname = "libcxx-windows-i686-pthread";
    buildInputs = (old.buildInputs or []) ++ [ threads ];
    cmakeFlags = old.cmakeFlags ++ [
      "-DLIBCXX_HAS_PTHREAD_API=ON" "-DLIBCXX_HAS_WIN32_THREAD_API=OFF"
      "-DLIBCXXABI_HAS_PTHREAD_API=ON" "-DLIBCXXABI_HAS_WIN32_THREAD_API=OFF"
      "-DLIBCXX_ADDITIONAL_LIBRARIES=unwind;winpthread"
      "-DLIBCXXABI_ADDITIONAL_LIBRARIES=unwind;winpthread"
    ];
  });
  collector = windows.stdenv.mkDerivation {
    pname = "snajpagent-winpty-static";
    version = "0.4.3";
    src = pkgs.fetchurl {
      urls = [
        "https://github.com/rprichard/winpty/archive/refs/tags/0.4.3.tar.gz"
        "https://codeload.github.com/rprichard/winpty/tar.gz/refs/tags/0.4.3"
      ];
      sha256 = "093037c39f9c899d79b74d5e15ff74fb59a98c492c5ed621e97e1090c3442865";
    };
    patches = [ ./winpty-owned-pipes.patch ];
    buildInputs = [ cxx unwind threads ];
    dontConfigure = true;
    buildPhase = ''
      runHook preBuild
      mkdir -p objects
      result=0
      for source in src/agent/{ConsoleFont,ConsoleInput,ConsoleInputReencoding,ConsoleLine,DebugShowInput,DefaultInputMap,EventLoop,InputMap,LargeConsoleRead,NamedPipe,Scraper,Terminal,Win32Console,Win32ConsoleBuffer}.cc \
                    src/shared/{DebugClient,OwnedHandle,StringUtil,WindowsSecurity,WindowsVersion,WinptyException}.cc \
                    ${./winpty-console.cc}; do
        $CXX -std=c++11 -Os -g -flto -ffunction-sections -fdata-sections \
          -Wall -Wextra -Werror -DUNICODE -D_UNICODE -DWINPTY_AGENT_ASSERT \
          -D_WIN32_WINNT=0x0500 -DWINVER=0x0500 -Isrc -nostdinc++ \
          -isystem ${pkgs.lib.getDev cxx}/include/c++/v1 \
          -c "$source" -o "objects/$(basename "$source").o" || result=1
      done
      test "$result" = 0
      $AR rcs libsnagpty.a objects/*.o
      runHook postBuild
    '';
    installPhase = ''
      mkdir -p "$out/lib" "$out/share/licenses/winpty"
      cp libsnagpty.a "$out/lib/"
      cp LICENSE "$out/share/licenses/winpty/"
    '';
  };
in { inherit cxx unwind collector; }
