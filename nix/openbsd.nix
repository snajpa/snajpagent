# SPDX-License-Identifier: GPL-2.0-only
{ pkgs, sourcePkgs, osVersion ? "7.9" }:
let
  inherit (pkgs) lib;
  legacy = lib.versionOlder osVersion "6.0";
  early = lib.versionOlder osVersion "4.0";
  libcVersion = if early then "30.3" else "84.2";
  threadVersion = if early then "2.4" else "20.1";
  gccRuntime = if early then "x86_64-unknown-openbsd3.5/3.3.2/fpic"
    else "amd64-unknown-openbsd5.9/4.2.1";
  release = lib.replaceStrings [ "." ] [ "" ] osVersion;
  target = "x86_64-unknown-openbsd${osVersion}";
  llvm = pkgs.llvmPackages_21;
  tools = "${llvm.llvm}/bin";
  mirrors = (lib.optionals (!legacy) [
    "https://cdn.openbsd.org/pub/OpenBSD"
    "https://ftp.hostserver.de/pub/OpenBSD"
    "https://ftp.fau.de/pub/OpenBSD"
  ]) ++ [
    "https://ftp.eu.openbsd.org/pub/OpenBSD"
    "https://ftp.lysator.liu.se/pub/OpenBSD"
    "https://mirror.yandex.ru/pub/OpenBSD"
  ];
  urls = file: map (mirror: "${mirror}/${osVersion}/amd64/${file}") mirrors;
  sdk = pkgs.stdenvNoCC.mkDerivation {
    pname = "openbsd-amd64-sysroot";
    version = osVersion;
    src = if early then pkgs.fetchurl {
      urls = urls "base35.tgz";
      sha256 = "1041bed06a9357692ee1cdeab09fe12c9d32862af35b8ca49c70a489f49e3dcb";
    } else pkgs.fetchurl {
      urls = urls "install${release}.iso";
      sha256 = {
        "7.9" = "7a4a92e953618035097c796a90b54424a0f3ae775552e1e7d102cf8a5130449f";
        "5.9" = "685262fc665425c61a2952b2820389a2d331ac5558217080e6d564d2ce88eecb";
      }.${osVersion};
    };
    compilerSet = lib.optionalString early (pkgs.fetchurl {
      urls = urls "comp35.tgz";
      sha256 = "d75980c7d961cab17edbc63e13a6b5a33d206917464e07a980ff1bfd3496da27";
    });
    nativeBuildInputs = [ pkgs.libarchive pkgs.python3 ];
    unpackPhase = if early then ''
      mkdir -p 3.5/amd64
      cp "$src" 3.5/amd64/base35.tgz
      cp "$compilerSet" 3.5/amd64/comp35.tgz
    '' else ''bsdtar -xf "$src" ${osVersion}/amd64/base${release}.tgz ${osVersion}/amd64/comp${release}.tgz'';
    dontConfigure = true;
    dontBuild = true;
    dontFixup = true;
    installPhase = ''
      mkdir -p "$out"
      for set in ${osVersion}/amd64/base${release}.tgz ${osVersion}/amd64/comp${release}.tgz; do
        bsdtar -xf "$set" -C "$out"
      done
      python3 - "$out" <<'PYSDK'
      import os, sys
      root = sys.argv[1]
      for directory, dirs, files in os.walk(root):
          for name in dirs + files:
              path = os.path.join(directory, name)
              if os.path.islink(path):
                  dest = os.readlink(path)
                  if dest.startswith("/"):
                      os.unlink(path)
                      os.symlink(os.path.relpath(root + dest, directory), path)
      # Upstream lld needs unversioned link-time names; native SONAMEs remain
      # embedded in the resulting executable. These aliases stay in the SDK.
      from pathlib import Path
      for path in Path(root, "usr/lib").glob("*.so.*"):
          alias = path.with_name(path.name.split(".so.")[0] + ".so")
          if not alias.exists():
              alias.symlink_to(path.name)
      PYSDK
    '';
  };
  compilerBuiltins = pkgs.runCommand "compiler-rt-openbsd-${osVersion}" {} ''
    mkdir -p "$out/lib"
    for file in emutls.c udivti3.c udivmodti4.c; do
      ${llvm.clang-unwrapped}/bin/clang --target=${target} --sysroot=${sdk} \
        -Os -g -fPIC -D_BSD_SOURCE \
        -c ${llvm.compiler-rt.src}/compiler-rt/lib/builtins/"$file" -o "$file.o"
    done
    ${tools}/llvm-ar rcs "$out/lib/libclang_rt.builtins.a" ./*.o
  '';
  # Supply the release's native startup objects and compiler runtime. Clang's
  # Linux installation otherwise injects its own host search/startup paths.
  compilerWrapper = pkgs.runCommand "openbsd-${osVersion}-clang" {} ''
    mkdir -p "$out/bin"
    cat > "$out/bin/clang" <<'SH'
    #!${pkgs.runtimeShell}
    link=1
    shared=0
    for arg in "$@"; do
      case "$arg" in
        -c|-S|-E|-M|-MM|-fsyntax-only|--version|-dump*|-print*) link=0;;
        -shared) shared=1;;
      esac
    done
    cc=${llvm.clang-unwrapped}/bin/clang
    extra=()
    # The 3.5 C headers use GNU89 extern-inline semantics. C++ is unchanged.
    inlineFlags=(${lib.optionalString early "-fgnu89-inline"})
    case "$0" in *++) cc="$cc++"; inlineFlags=(); extra=(${if early then "-l:libstdc++.so.32.0 -l:libm.so.1.0" else if legacy then "-l:libstdc++.so.57.0" else "-lc++ -lc++abi"});; esac
    if [ "$link" = 0 ]; then exec "$cc" "''${inlineFlags[@]}" "$@"; fi
    start=(${sdk}/usr/lib/crt0.o ${sdk}/usr/lib/crtbegin.o)
    end=(${sdk}/usr/lib/crtend.o)
    flags=(${if early then "-Wl,-no-pie" else "-pie"} -Wl,-e,__start,--dynamic-linker=/usr/libexec/ld.so)
    if [ "$shared" = 1 ]; then
      start=(${sdk}/usr/lib/crtbeginS.o)
      end=(${sdk}/usr/lib/crtendS.o)
      flags=()
    fi
    exec "$cc" "''${inlineFlags[@]}" -nostdlib "''${flags[@]}" "''${start[@]}" "$@" \
      -Wl,-Bdynamic "''${extra[@]}" ${if legacy then "-l:libpthread.so.${threadVersion} -l:libc.so.${libcVersion} ${compilerBuiltins}/lib/libclang_rt.builtins.a ${sdk}/usr/lib/gcc-lib/${gccRuntime}/libgcc.a" else "-lpthread -lc -lcompiler_rt"} "''${end[@]}"
    SH
    chmod +x "$out/bin/clang"
    ln -s clang "$out/bin/clang++"
  '';
  compiler = "${compilerWrapper}/bin/clang";
  cxxCompiler = "${compilerWrapper}/bin/clang++";
  cflags = "-Os -g -D_BSD_SOURCE -fPIC"
    + (if early then " -fno-stack-protector -fno-builtin-wcslen" else " -fstack-protector-strong")
    # 5.9's endian statement macros predate Clang's token-context diagnostic.
    + lib.optionalString legacy " -Wno-compound-token-split-by-macro";
  ldflags = "--ld-path=${llvm.lld}/bin/ld.lld";
  cmakeLibrary = package: flags: dependencies:
    pkgs.stdenvNoCC.mkDerivation {
      pname = "${package.pname}-openbsd-amd64";
      inherit (package) version src;
      patches = package.patches or [];
      nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config pkgs.perl pkgs.python3 ];
      buildInputs = dependencies;
      strictDeps = true;
      enableParallelBuilding = true;
      dontStrip = true;
      cmakeBuildType = "MinSizeRel";
      preConfigure = ''
        export PKG_CONFIG_PATH=
        export PKG_CONFIG_LIBDIR=${lib.escapeShellArg
          (lib.concatMapStringsSep ":" (dep: "${dep}/lib/pkgconfig") dependencies)}
        cmakeFlagsArray+=(
          "-DCMAKE_C_FLAGS=${cflags}"
          "-DCMAKE_CXX_FLAGS=${cflags} -stdlib=${if legacy then "libstdc++" else "libc++"}"
          "-DCMAKE_EXE_LINKER_FLAGS=${ldflags}"
        )
      '';
      cmakeFlags = [
        "-DCMAKE_SYSTEM_NAME=OpenBSD" "-DCMAKE_SYSTEM_VERSION=${osVersion}"
        "-DCMAKE_SYSTEM_PROCESSOR=amd64" "-DCMAKE_SYSROOT=${sdk}"
        "-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER"
        "-DCMAKE_C_COMPILER=${compiler}" "-DCMAKE_C_COMPILER_TARGET=${target}"
        "-DCMAKE_CXX_COMPILER=${cxxCompiler}"
        "-DCMAKE_CXX_COMPILER_TARGET=${target}"
        "-DCMAKE_AR=${tools}/llvm-ar" "-DCMAKE_RANLIB=${tools}/llvm-ranlib"
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.10"
        "-DCMAKE_PREFIX_PATH=${lib.concatStringsSep ";" dependencies}"
        "-DCMAKE_INSTALL_LIBDIR=lib" "-DCMAKE_INSTALL_INCLUDEDIR=include"
        "-DCMAKE_INSTALL_BINDIR=bin" "-DBUILD_SHARED_LIBS=OFF" "-DBUILD_TESTING=OFF"
      ] ++ flags;
    };
  autotoolsLibrary = package: flags: dependencies:
    pkgs.stdenvNoCC.mkDerivation {
      pname = "${package.pname}-openbsd-amd64";
      inherit (package) version src;
      nativeBuildInputs = [ pkgs.pkg-config pkgs.perl pkgs.texinfo llvm.llvm ];
      buildInputs = dependencies;
      strictDeps = true;
      enableParallelBuilding = true;
      dontStrip = true;
      configurePlatforms = [];
      configureFlags = [
        "--build=${pkgs.stdenv.buildPlatform.config}" "--host=${target}"
        "--disable-shared" "--enable-static" "--with-pic" "--disable-dependency-tracking"
      ] ++ flags;
      preConfigure = ''
        export CC="${compiler} --target=${target} --sysroot=${sdk}"
        export CXX="${cxxCompiler} --target=${target} --sysroot=${sdk}"
        export AR=${tools}/llvm-ar RANLIB=${tools}/llvm-ranlib NM=${tools}/llvm-nm
        export STRIP=${tools}/llvm-strip LD=${llvm.lld}/bin/ld.lld
        export CFLAGS='${cflags}' CXXFLAGS='${cflags}' LDFLAGS='${ldflags}'
        export PKG_CONFIG_PATH=
        export PKG_CONFIG_LIBDIR=${lib.escapeShellArg
          (lib.concatMapStringsSep ":" (dep: "${dep}/lib/pkgconfig") dependencies)}
      '';
    };
  jansson = (cmakeLibrary sourcePkgs.jansson [
    "-DJANSSON_BUILD_SHARED_LIBS=OFF" "-DJANSSON_BUILD_DOCS=OFF"
    "-DJANSSON_WITHOUT_TESTS=ON" "-DJANSSON_EXAMPLES=OFF"
  ] []).overrideAttrs (_: {
    postPatch = lib.optionalString early ''
      # Preserve format checking when the application's legacy printf macro
      # is active; GCC/Clang accept the reserved spelling for the archetype.
      substituteInPlace src/jansson.h \
        --replace-fail 'format(printf,' 'format(__printf__,'
      # 3.5 declares these as libc functions rather than math.h macros.
      substituteInPlace src/value.c \
        --replace-fail '#ifndef isnan' '#if !defined(isnan) && !defined(__OpenBSD__)' \
        --replace-fail '#ifndef isinf' '#if !defined(isinf) && !defined(__OpenBSD__)'
    '';
  });
  tls = (cmakeLibrary sourcePkgs.mbedtls [
    "-DUSE_SHARED_MBEDTLS_LIBRARY=OFF" "-DENABLE_PROGRAMS=OFF"
    "-DENABLE_TESTING=OFF" "-DGEN_FILES=OFF"
  ] []).overrideAttrs (_: {
    postPatch = ''
      perl scripts/config.pl set MBEDTLS_THREADING_C
      perl scripts/config.pl set MBEDTLS_THREADING_PTHREAD
      substituteInPlace library/net_sockets.c \
        --replace-fail 'fd >= FD_SETSIZE' '(unsigned int) fd >= FD_SETSIZE'
    '' + lib.optionalString early ''
      # 3.5's disabled /dev/random returns EIO; use the native urandom
      # device, also used by the application's OS entropy path.
      perl scripts/config.pl set MBEDTLS_PLATFORM_DEV_RANDOM '"/dev/urandom"'
      # 3.5 hides fd_set/select behind these newer feature requests.
      substituteInPlace library/net_sockets.c \
        --replace-fail '#define _POSIX_C_SOURCE 200112L' '/* Native BSD declarations. */' \
        --replace-fail '#define _XOPEN_SOURCE 600' '/* Native BSD declarations. */' \
        --replace-fail 'defined(__socklen_t_defined)' 'defined(__OpenBSD__) || defined(__socklen_t_defined)'
      # Keep the library's volatile zeroizer when libc has no explicit_bzero.
      # 3.5 has native clocks in sys/time.h and an empty POSIX threads macro.
      substituteInPlace library/platform_util.c \
        --replace-fail '|| defined(__OpenBSD__)' "" \
        --replace-fail '_POSIX_THREAD_SAFE_FUNCTIONS >= 200112L' '(_POSIX_THREAD_SAFE_FUNCTIONS + 0) >= 200112L' \
        --replace-fail '#include <time.h>' '#include <time.h>
      #include <sys/time.h>' \
        --replace-fail '|| defined(__HAIKU__)' '|| defined(__HAIKU__) || defined(__OpenBSD__)'
      substituteInPlace library/threading.c \
        --replace-fail '_POSIX_THREAD_SAFE_FUNCTIONS >= 200112L' '(_POSIX_THREAD_SAFE_FUNCTIONS + 0) >= 200112L'
      # The RSA self-test needs only the native word-sized random API here.
      substituteInPlace library/rsa.c \
        --replace-fail '#include <string.h>' '#include <string.h>
      #include <stdlib.h>' \
        --replace-fail 'arc4random_buf(output, len);' \
          'for (size_t i = 0; i < len; ++i) output[i] = (unsigned char) arc4random();'
    '';
  });
  zlib = cmakeLibrary sourcePkgs.zlib [
    "-DZLIB_BUILD_SHARED=OFF" "-DZLIB_BUILD_STATIC=ON" "-DZLIB_BUILD_TESTING=OFF"
  ] [];
  brotli = (cmakeLibrary sourcePkgs.brotli [ "-DBROTLI_DISABLE_TESTS=ON" ] []).overrideAttrs (_: {
    postPatch = lib.optionalString early ''
      # The log2 fallback calls log, which is in the native math library.
      substituteInPlace CMakeLists.txt \
        --replace-fail 'add_definitions(-DBROTLI_HAVE_LOG2=0)' \
          'set(LIBM_LIBRARY "m")
      add_definitions(-DBROTLI_HAVE_LOG2=0)'
    '';
  });
  zstd = (cmakeLibrary sourcePkgs.zstd [
    "-DZSTD_BUILD_SHARED=OFF" "-DZSTD_BUILD_STATIC=ON"
    "-DZSTD_BUILD_PROGRAMS=OFF" "-DZSTD_BUILD_TESTS=OFF"
  ] []).overrideAttrs (_: {
    cmakeDir = "../build/cmake";
    postPatch = lib.optionalString early ''
      # The 3.5 loader diagnoses undefined optional trace hooks even when
      # they are weak. Keep zstd's normal compression/decompression only.
      substituteInPlace lib/common/zstd_trace.h \
        --replace-fail '#  define ZSTD_TRACE ZSTD_HAVE_WEAK_SYMBOLS' '#  define ZSTD_TRACE 0'
    '';
  });
  cares = (cmakeLibrary sourcePkgs.c-ares [
    "-DCARES_SHARED=OFF" "-DCARES_STATIC=ON" "-DCARES_STATIC_PIC=ON"
    "-DCARES_BUILD_TOOLS=OFF" "-DCARES_BUILD_TESTS=OFF"
  ] []).overrideAttrs (_: {
    postPatch = ''
      # Old BSD net/if.h needs sockaddr declared before configure's type probes.
      substituteInPlace CMakeLists.txt \
        --replace-fail 'CARES_EXTRAINCLUDE_IFSET (HAVE_NET_IF_H       net/if.h)' \
          'CARES_EXTRAINCLUDE_IFSET (HAVE_NET_IF_H       "sys/socket.h;net/if.h")'
    '' + lib.optionalString early ''
      # This release's socket headers require sys/types.h first.
      substituteInPlace CMakeLists.txt \
        --replace-fail '"sys/socket.h;net/if.h"' '"sys/types.h;sys/socket.h;net/if.h"' \
        --replace-fail 'CHECK_INCLUDE_FILES (sys/socket.h' 'CHECK_INCLUDE_FILES ("sys/types.h;sys/socket.h"' \
        --replace-fail 'CARES_EXTRAINCLUDE_IFSET (HAVE_SYS_SOCKET_H   sys/socket.h)' 'CARES_EXTRAINCLUDE_IFSET (HAVE_SYS_SOCKET_H  "sys/types.h;sys/socket.h")' \
        --replace-fail 'CHECK_INCLUDE_FILES (netinet/in.h' 'CHECK_INCLUDE_FILES ("sys/types.h;sys/socket.h;netinet/in.h"' \
        --replace-fail 'CARES_EXTRAINCLUDE_IFSET (HAVE_NETINET_IN_H   netinet/in.h)' 'CARES_EXTRAINCLUDE_IFSET (HAVE_NETINET_IN_H   "sys/types.h;sys/socket.h;netinet/in.h")'
    '';
  });
  nghttp2 = (cmakeLibrary sourcePkgs.nghttp2 [
    "-DENABLE_LIB_ONLY=ON" "-DBUILD_STATIC_LIBS=ON" "-DENABLE_DOC=OFF"
  ] []).overrideAttrs (_: {
    postPatch = lib.optionalString early ''
      # Old inttypes.h has types but no C99 limits; Clang supplies stdint.h.
      substituteInPlace lib/includes/nghttp2/nghttp2.h \
        --replace-fail '#  include <inttypes.h>' '#  include <inttypes.h>
      #  include <stdint.h>'
    '';
  });
  iconv = autotoolsLibrary pkgs.libiconvReal [] [];
  unistring = (autotoolsLibrary sourcePkgs.libunistring
    [ "--with-libiconv-prefix=${iconv}" ] [ iconv ]).overrideAttrs (_: lib.optionalAttrs early {
    # Build the library, without cross-building Gnulib's host test programs.
    buildPhase = ''runHook preBuild; make -j"$NIX_BUILD_CORES" -C lib; runHook postBuild'';
    installPhase = ''runHook preInstall; make -C lib install; runHook postInstall'';
  });
  idn2 = (autotoolsLibrary sourcePkgs.libidn2 [
    "--disable-doc" "--with-libiconv-prefix=${iconv}"
    "--with-libunistring-prefix=${unistring}"
  ] [ iconv unistring ]).overrideAttrs (_: {
    postPatch = ''
      # Bundled older libtool assumes native ranlib's timestamp-only option.
      # LLVM rebuilds the deterministic archive index instead.
      substituteInPlace configure --replace-fail 'RANLIB -t' 'RANLIB'
    '';
    buildPhase = ''
      runHook preBuild
      make -j"$NIX_BUILD_CORES" -C gl
      make -j"$NIX_BUILD_CORES" -C unistring
      make -j"$NIX_BUILD_CORES" -C lib
      runHook postBuild
    '';
    installPhase = ''
      runHook preInstall
      make -C lib install
      install -Dm644 libidn2.pc "$out/lib/pkgconfig/libidn2.pc"
      runHook postInstall
    '';
  });
  regex = (import ./windows-regex.nix {
    inherit pkgs unistring;
    cross = { inherit compiler cxxCompiler target sdk tools cflags ldflags; };
  }).overrideAttrs (old: {
    preConfigure = lib.optionalString early ''
      export gl_cv_func_printf_sizes_c99=no
    '' + ''
      # Native C-locale char32 encoding rejects non-ASCII even when the
      # replacement decoder accepts UTF-8; select the matching encoder too.
      export gl_cv_func_mbrtoc32_sanitycheck=no
      export gl_cv_func_c32rtomb_sanitycheck=no
    '' + (if early then lib.replaceStrings
      [ "--m4-base=m4 regex" "autoreconf -fiv" ]
      [ "--m4-base=m4 regex errno snprintf vsnprintf" ''
        # Keep the LGPLv2-compatible formatting modules. Their basic checks
        # omit C99 length modifiers, which the native guest proved missing.
        substituteInPlace m4/snprintf.m4 m4/vsnprintf.m4 \
          --replace-fail '_usable = no; then' '_usable = no || test $gl_cv_func_printf_sizes_c99 = no; then'
        autoreconf -fiv
      '' ]
      old.preConfigure
      else old.preConfigure);
    postInstall = ''
      # OpenBSD sys/cdefs.h defines __used as an attribute. Rename only the
      # public header's private struct member; its layout and library ABI stay.
      substituteInPlace "$out/include/snajpagent-gnulib-regex.h" \
        --replace-fail '__REPB_PREFIX(used)' '__REPB_PREFIX(snag_used)'
    '' + lib.optionalString early ''
      # Export the same missing errno values used inside the Unicode library.
      cp lib/errno.h "$out/include/errno.h"
    '';
  });
  networkLibraries = [ tls zlib brotli zstd cares nghttp2 iconv unistring idn2 ];
  curl = (cmakeLibrary sourcePkgs.curlMinimal [
    "-DBUILD_STATIC_LIBS=ON" "-DBUILD_CURL_EXE=OFF" "-DCURL_BUILD_EVERYTHING=OFF"
    "-DCURL_USE_MBEDTLS=ON" "-DCURL_USE_OPENSSL=OFF" "-DCURL_DEFAULT_SSL_BACKEND=mbedtls"
    "-DENABLE_ARES=ON" "-DUSE_NGHTTP2=ON" "-DUSE_LIBIDN2=ON"
    "-DCURL_ZLIB=ON" "-DCURL_BROTLI=ON" "-DCURL_ZSTD=ON"
    "-DCURL_USE_LIBPSL=OFF" "-DCURL_USE_LIBSSH2=OFF" "-DCURL_USE_LIBSSH=OFF"
    "-DCURL_DISABLE_LDAP=ON" "-DCURL_DISABLE_LDAPS=ON"
    "-DCURL_CA_BUNDLE=none" "-DCURL_CA_PATH=none"
  ] networkLibraries).overrideAttrs (_: {
    postPatch = lib.optionalString early ''
      # The native socket/time headers require sys/types.h first. Let curl
      # probe the real headers instead of marking available functions absent.
      substituteInPlace include/curl/mprintf.h \
        --replace-fail 'format(printf,' 'format(__printf__,'
      substituteInPlace CMakeLists.txt \
        --replace-fail 'list(APPEND CURL_INCLUDES "sys/socket.h")' \
          'list(APPEND CURL_INCLUDES "sys/types.h" "sys/socket.h")'
    '';
  });
in {
  inherit sdk target compiler tools cflags ldflags jansson tls curl regex unistring;
  application = { source, packageName, version, revision, debug ? false,
                  updateBase ? "", updateTarget ? "" }:
    pkgs.stdenvNoCC.mkDerivation {
      pname = "${packageName}-openbsd-amd64";
      inherit version;
      src = source;
      outputs = [ "out" "debug" ];
      nativeBuildInputs = [ pkgs.pkg-config ];
      buildInputs = [ jansson curl ] ++ networkLibraries ++ [ regex ];
      enableParallelBuilding = true;
      dontStrip = true;
      preBuild = ''
        mkdir -p build
        ${pkgs.zstd}/bin/zstd -q -19 \
          ${pkgs.cacert}/etc/ssl/certs/ca-no-trust-rules-bundle.crt -o build/ca_bundle.zst
        od -An -v -t u1 build/ca_bundle.zst |
          sed -E 's/([0-9]+)/\1,/g' > build/ca_bundle.inc
        # Keep application/Unicode libraries static with native OS runtimes.
        makeFlagsArray+=(
          'DEBUG=${if debug then "1" else "0"}'
          ${pkgs.lib.optionalString (updateBase != "") "'UPDATE_BASE_URL=${updateBase}' 'UPDATE_TARGET=${updateTarget}'"}
          'TARGET_OS=OpenBSD'
          'CC=${compiler} --target=${target} --sysroot=${sdk}'
          'STRIP=${tools}/llvm-strip' 'OBJCOPY=${tools}/llvm-objcopy'
          'GIT_HEAD=${revision}' 'BUILD_VERSION=${version}'
          'CPPFLAGS=${lib.optionalString (!early) "-D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 "}-D_FILE_OFFSET_BITS=64 ${lib.optionalString early "-Dsnprintf=rpl_snprintf -Dvsnprintf=rpl_vsnprintf -DSNAJPAGENT_LEGACY_PRINTF -Dprintf=snag_legacy_printf -Dfprintf=snag_legacy_fprintf "}-Ibuild -DSNAJPAGENT_CA_BUNDLE=\"ca_bundle.inc\" -DSNAJPAGENT_STATIC_UTF8 -I${regex}/include -I${unistring}/include'
          'CFLAGS=-std=c11 ${cflags} ${if debug then "-Og -fno-omit-frame-pointer" else "-flto -ffunction-sections -fdata-sections"} -Wall -Wextra -Wpedantic -Werror'
          'LDFLAGS=--ld-path=${llvm.lld}/bin/ld.lld ${pkgs.lib.optionalString (!debug) "-flto"} -Wl,--gc-sections,--as-needed,-Bstatic'
          "JANSSON_CFLAGS=$(pkg-config --cflags jansson)"
          "LDLIBS=-Wl,-Bstatic $(pkg-config --static --libs jansson) -L${regex}/lib -lsnagregex -L${unistring}/lib -lunistring"
          "CURL_CFLAGS=$(pkg-config --cflags libcurl)"
          "CURL_LIBS=$(pkg-config --static --libs libcurl | sed -E 's/-l?(-l?)?pthread//g') -lutil -Wl,-Bdynamic ${if legacy then "-l:libpthread.so.${threadVersion}" else "-lpthread"}"
        )
      '';
      installPhase = ''
        runHook preInstall
        mkdir -p "$out/bin" "$debug"
        cp ${packageName} "$out/bin/"
        cp ${if debug then "${packageName}" else "debug-${packageName}"} "$debug/"
        ln -s "$debug" "$out/bin/.debug"
        runHook postInstall
      '';
    };
}
