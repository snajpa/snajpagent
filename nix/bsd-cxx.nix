# SPDX-License-Identifier: GPL-2.0-only
{ pkgs, autotoolsLibrary, cflags, llvm, os, osVersion }:
(autotoolsLibrary pkgs.gcc14.cc [
  "--disable-multilib" "--disable-nls" "--disable-libstdcxx-pch" "--with-target-subdir=."
  "--disable-symvers" # No shared library or shared-only version-compatibility objects.
  "--enable-clocale=generic" "--enable-libstdcxx-threads"
  "--enable-libstdcxx-time=yes" "--disable-libstdcxx-backtrace"
  "--with-gxx-include-dir=${placeholder "out"}/include/c++"
] []).overrideAttrs (old: {
  pname = "libstdcxx-${os}-${osVersion}";
  sourceRoot = "gcc-${pkgs.gcc14.cc.version}/libstdc++-v3";
  # config-ml.in replays --disable-static but deliberately ignores a later
  # --enable-static. Do not let stdenv prepend the conflicting default.
  dontDisableStatic = true;
  postPatch = ''
    # Clang rejects the old FreeBSD shim's unconditional noexcept redeclarations
    # of functions already declared by libc. Keep native declarations when
    # visible and the existing dynamic fallback when strict headers hide them.
    substituteInPlace config/os/bsd/freebsd/os_defines.h \
      --replace-fail '#define _GLIBCXX_USE_C99_CHECK 1' '#define _GLIBCXX_USE_C99_CHECK 0' \
      --replace-fail '#define _GLIBCXX_USE_C99_LONG_LONG_CHECK 1' '#define _GLIBCXX_USE_C99_LONG_LONG_CHECK 0'
    # Make the mandated zero initialization explicit for Clang's constinit
    # check when configure selects the pthread-key exception-state path.
    substituteInPlace libsupc++/eh_globals.cc \
      --replace-fail '__constinit __cxa_eh_globals eh_globals;' \
                     '__constinit __cxa_eh_globals eh_globals = {};'
    # Match the public declaration's GNU attribute spelling under Clang.
    substituteInPlace src/c++11/assert_fail.cc \
      --replace-fail '[[__noreturn__]]' '__attribute__((__noreturn__))'
    substituteInPlace src/c++11/random.cc \
      --replace-fail '__builtin_ia32_rdseed_si_step' '__builtin_ia32_rdseed32_step'
    # Clang emits the bridge's DWARF with its ordinary assembly output; it
    # does not accept GCC's assembler-location capability override.
    substituteInPlace src/c++11/Makefile.am src/c++11/Makefile.in \
      --replace-fail ' -gno-as-loc-support' ""
    # Native FreeBSD headers already define the four wide-character
    # classification methods inline. Keep those native methods and the
    # generic locale's remaining transformation/initialization methods.
    substituteInPlace config/locale/generic/ctype_members.cc \
      --replace-fail '  bool
      ctype<wchar_t>::
      do_is(mask' '#ifndef __FreeBSD__
      bool
      ctype<wchar_t>::
      do_is(mask' \
      --replace-fail '  wchar_t
      ctype<wchar_t>::
      do_widen(char' '#endif
      wchar_t
      ctype<wchar_t>::
      do_widen(char'
  '';
  preConfigure = old.preConfigure + ''
    # A standalone libstdc++ build has no libgcc configure step to select
    # the target's gthread header. Supply the real POSIX implementation so
    # feature probes discover mutexes, condition variables and std::thread.
    ln -s gthr-posix.h ../libgcc/gthr-default.h
    # These SDKs hide long-long declarations in strict C++ even though the
    # compiler supports them. Expose the real functions to configure probes.
    export CXXFLAGS="${cflags} -fPIC -stdlib=libstdc++ -D__LONG_LONG_SUPPORTED="
    export CFLAGS="${cflags} -fPIC"
    export LDFLAGS="--ld-path=${llvm.lld}/bin/ld.lld"
  '';
  doCheck = false; # Cross-built runtime; target execution is separate.
})
