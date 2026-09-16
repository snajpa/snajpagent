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
  legacyRt = pkgs.runCommand "legacyrt-openbsd-${osVersion}" {} ''
    mkdir -p $out/lib
    ${compiler} --target=${target} --sysroot=${sdk} -Os -fno-stack-protector \
      -c ${./legacy-rt-shim.c} -o legacyrt.o
    ${tools}/llvm-ar rcs $out/lib/liblegacyrt.a legacyrt.o
  '';
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
  cxx = import ./bsd-cxx.nix {
    inherit pkgs autotoolsLibrary cflags llvm osVersion;
    os = "openbsd";
  };
  voiceRtc = import ./voice-rtc-cross.nix {
    inherit pkgs cmakeLibrary tls; sourcePkgs = sourcePkgs;
    # OpenBSD 3.5's inttypes.h omits the C99 format macros (PRIx64).
    srtpPatches = lib.optional early ./libsrtp-openbsd35-inttypes.patch;
    # 3.5's netinet/in.h needs sys/types.h first; seed the checked header.
    srtpFlags = lib.optional early "-DHAVE_NETINET_IN_H=1";
    # OpenBSD 7.x dropped struct route_in6 from the userland headers; the
    # legacy SDKs still provide it, so only the modern targets take the patch.
    sctpPatches = [ ./usrsctp-bsd-tailq-safe.patch ] ++ lib.optional early ./usrsctp-openbsd35-systypes.patch ++ lib.optional early ./usrsctp-legacy-compat.patch ++ lib.optional early ./usrsctp-legacy-timingsafe.patch ++ lib.optional (!legacy) ./usrsctp-openbsd-route-in6.patch;
    # OpenBSD 5.9/3.5 net/if.h needs struct sockaddr complete first (7.9 does not).
    rtcPatches = lib.optional legacy ./libdatachannel-bsd-sockaddr.patch;
    # Keep the C++ runtime consistent with the variant's application flags and
    # the RTC link line: legacy/early use the built libstdc++, non-legacy the
    # SDK's libc++ (forcing libstdc++ here pulls the unbuilt 7.9 runtime).
    cxxFlags = "${cflags} ${if legacy then "-stdlib=libstdc++" else "-stdlib=libc++"} -pthread${lib.optionalString early " -fno-use-cxa-atexit -fno-builtin-pow -fno-builtin-powf -include ${./bsd-legacy-cxx.h}"}${lib.optionalString legacy " -nostdinc++ -isystem ${cxx}/include/c++ -isystem ${cxx}/include/c++/${target}"}";
    cxxLibraries = "${ldflags}${lib.optionalString legacy " -L${cxx}/lib"}";
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
      # libdatachannel uses the DTLS-SRTP API; enable it (PROTO_DTLS is on).
      perl scripts/config.pl set MBEDTLS_SSL_DTLS_SRTP
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
  miniaudio = pkgs.runCommand "miniaudio-openbsd-audio4" { nativeBuildInputs = [ pkgs.patch ]; } ''
    mkdir -p "$out"
    cp ${pkgs.miniaudio.src}/miniaudio.h "$out/"
    chmod u+w "$out/miniaudio.h"
    patch -d "$out" -p1 < ${./miniaudio-openbsd-audio4.patch}
    patch -d "$out" -p1 < ${./miniaudio-openbsd35-headers.patch}
    patch -d "$out" -p1 < ${./miniaudio-sndio-transfers.patch}
  '';
  av = pkgs.stdenvNoCC.mkDerivation {
    pname = "ffmpeg-headless-openbsd-${osVersion}";
    inherit (sourcePkgs.ffmpeg_8) version src;
    patches = sourcePkgs.ffmpeg_8.patches
      ++ lib.optional legacy ./ffmpeg-bsd-thread-headers.patch
      ++ lib.optionals early [ ./ffmpeg-openbsd35-inttypes.patch ./ffmpeg-openbsd35-hls.patch
                               ./ffmpeg-legacy-libm.patch ];
    postPatch = lib.optionalString early ''
      # Reuse the application's exported errno owner without its other wrappers.
      mkdir -p compat/errno
      cp ${regex}/include/errno.h compat/errno/
    '';
    nativeBuildInputs = [ pkgs.pkg-config pkgs.perl pkgs.nasm llvm.llvm ];
    buildInputs = [ zlib ];
    strictDeps = true;
    enableParallelBuilding = true;
    dontStrip = true;
    configurePlatforms = [];
    configureFlags = [
      "--enable-cross-compile" "--target-os=openbsd" "--arch=x86_64"
      "--enable-static" "--disable-shared" "--enable-pic"
      "--disable-autodetect" "--disable-network" "--disable-programs" "--disable-doc"
      "--disable-avdevice" "--disable-avfilter"
      "--enable-avcodec" "--enable-avformat" "--enable-avutil"
      "--enable-swresample" "--enable-swscale" "--enable-zlib"
      "--enable-pthreads" "--enable-safe-bitstream-reader" "--enable-pixelutils"
      "--disable-gpl" "--disable-version3" "--pkg-config-flags=--static"
    ];
    preConfigure = ''
      export PKG_CONFIG_PATH=
      export PKG_CONFIG_LIBDIR=${zlib}/lib/pkgconfig
      configureFlagsArray+=(
        "--host-cc=${pkgs.stdenv.cc}/bin/cc"
        "--cc=${compiler} --target=${target} --sysroot=${sdk}"
        "--cxx=${cxxCompiler} --target=${target} --sysroot=${sdk}"
        "--ar=${tools}/llvm-ar" "--ranlib=${tools}/llvm-ranlib"
        "--nm=${tools}/llvm-nm" "--strip=${tools}/llvm-strip"
        "--extra-cflags=${cflags}${lib.optionalString legacy " -Dstatic_assert=_Static_assert"}${lib.optionalString early " -fno-builtin-pow -fno-builtin-powf -I$PWD/compat/errno"}"
        "--extra-ldflags=${ldflags}"
      )
    '';
  };
  png = (cmakeLibrary sourcePkgs.libpng [
    "-DPNG_SHARED=OFF" "-DPNG_STATIC=ON" "-DPNG_TESTS=OFF" "-DPNG_TOOLS=OFF"
  ] [ zlib ]).overrideAttrs (old: {
    # libpng's header generator invokes Clang directly, outside CMake's target rule.
    preConfigure = old.preConfigure + ''
      cmakeFlagsArray+=("-DCMAKE_C_FLAGS=${cflags} --target=${target} --sysroot=${sdk}")
    '';
  });
  freetype = cmakeLibrary sourcePkgs.freetype [
    "-DFT_DISABLE_BZIP2=ON" "-DFT_DISABLE_BROTLI=ON" "-DFT_DISABLE_HARFBUZZ=ON"
    "-DFT_REQUIRE_ZLIB=ON" "-DFT_REQUIRE_PNG=ON"
  ] [ zlib png ];
  expat = cmakeLibrary sourcePkgs.expat ([
    "-DEXPAT_SHARED_LIBS=OFF" "-DEXPAT_BUILD_TOOLS=OFF"
    "-DEXPAT_BUILD_EXAMPLES=OFF" "-DEXPAT_BUILD_TESTS=OFF" "-DEXPAT_BUILD_DOCS=OFF"
  ] ++ lib.optionals early [
    "-DEXPAT_DEV_URANDOM=OFF" "-DEXPAT_WITH_ARC4RANDOM=ON"
  ]) [];
  fontconfig = (autotoolsLibrary sourcePkgs.fontconfig [
    "--disable-docs" "--disable-docbook" "--disable-cache-build" "--disable-nls"
    "--sysconfdir=/etc" "--with-cache-dir=/var/cache/fontconfig"
    "--with-default-fonts=/usr/X11R6/lib/X11/fonts"
    "--with-add-fonts=/usr/local/share/fonts"
  ] [ expat freetype png zlib ]).overrideAttrs (old: {
    nativeBuildInputs = old.nativeBuildInputs ++ [ pkgs.gperf pkgs.python3 ];
    postPatch = lib.optionalString early ''
      substituteInPlace src/fcatomic.c --replace-fail 'errno == ENOTSUP' 'errno == EOPNOTSUPP'
    '';
    preConfigure = old.preConfigure + ''
      # Include PNG's private math dependency through FreeType's static metadata.
      export FREETYPE_LIBS="$(pkg-config --static --libs freetype2)"
    '';
    installFlags = [ "sysconfdir=$(out)/etc" "RUN_FC_CACHE_TEST=false" "fc_cachedir=$(TMPDIR)/fontconfig-cache" ];
  });
  jpeg = (cmakeLibrary sourcePkgs.libjpeg [
    "-DENABLE_SHARED=OFF" "-DENABLE_STATIC=ON" "-DWITH_TURBOJPEG=OFF"
  ] []).overrideAttrs (old: {
    nativeBuildInputs = old.nativeBuildInputs ++ [ pkgs.nasm ];
  });
  openjpeg = (cmakeLibrary sourcePkgs.openjpeg [ "-DBUILD_CODEC=OFF" ] []).overrideAttrs (_: {
    postPatch = lib.optionalString early ''
      substituteInPlace src/lib/openjp2/opj_includes.h \
        --replace-fail '#include <inttypes.h>' '#include <inttypes.h>
      #ifndef PRId64
      #define PRId64 __INT64_FMTd__
      #define PRIi64 __INT64_FMTi__
      #define PRIu32 __UINT32_FMTu__
      #endif' \
        --replace-fail 'return lrintf(f);' 'return __builtin_lrintf(f);'
    '';
  });
  pdf = (cmakeLibrary sourcePkgs.poppler [
    "-DENABLE_UNSTABLE_API_ABI_HEADERS=ON" "-DFONT_CONFIGURATION=fontconfig"
    "-DENABLE_UTILS=OFF" "-DENABLE_CPP=OFF" "-DENABLE_GLIB=OFF"
    "-DENABLE_GOBJECT_INTROSPECTION=OFF" "-DENABLE_QT5=OFF" "-DENABLE_QT6=OFF"
    "-DBUILD_QT5_TESTS=OFF" "-DBUILD_QT6_TESTS=OFF" "-DBUILD_CPP_TESTS=OFF"
    "-DBUILD_MANUAL_TESTS=OFF" "-DENABLE_LCMS=OFF" "-DENABLE_LIBCURL=OFF"
    "-DENABLE_LIBTIFF=OFF" "-DENABLE_NSS3=OFF" "-DENABLE_GPGME=OFF"
  ] ([ zlib png freetype expat fontconfig jpeg openjpeg pkgs.boost ] ++ lib.optional legacy cxx)).overrideAttrs (old: {
    cmakeBuildType = "Release";
    patches = old.patches ++ [ ./poppler-static-fonts.patch ];
    postPatch = lib.optionalString early (import ./poppler-legacy-math.nix);
    preConfigure = old.preConfigure + lib.optionalString legacy ''
      cmakeFlagsArray+=(
        "-DCMAKE_CXX_FLAGS=${cflags} -stdlib=libstdc++ -pthread${lib.optionalString early " -fno-use-cxa-atexit -fno-builtin-pow -fno-builtin-powf -include ${./bsd-legacy-cxx.h}"} -nostdinc++ -isystem ${cxx}/include/c++ -isystem ${cxx}/include/c++/${target}"
        "-DCMAKE_EXE_LINKER_FLAGS=${ldflags} -L${cxx}/lib"
      )
    '';
  });
  xml = cmakeLibrary sourcePkgs.libxml2 [
    "-DLIBXML2_WITH_PROGRAMS=OFF" "-DLIBXML2_WITH_TESTS=OFF"
    "-DLIBXML2_WITH_PYTHON=OFF" "-DLIBXML2_WITH_MODULES=OFF"
    "-DLIBXML2_WITH_ICONV=ON"
  ] [ iconv ];
  archive = (cmakeLibrary sourcePkgs.libarchive [
    "-DENABLE_TAR=OFF" "-DENABLE_CPIO=OFF" "-DENABLE_CAT=OFF"
    "-DENABLE_UNZIP=OFF" "-DENABLE_TEST=OFF" "-DENABLE_INSTALL=ON"
    "-DENABLE_OPENSSL=OFF" "-DENABLE_MBEDTLS=OFF" "-DENABLE_NETTLE=OFF"
    "-DENABLE_CNG=OFF" "-DENABLE_LIBB2=OFF" "-DENABLE_LZ4=OFF"
    "-DENABLE_LZO=OFF" "-DENABLE_LZMA=OFF" "-DENABLE_ZSTD=OFF"
    "-DENABLE_BZip2=OFF" "-DENABLE_LIBXML2=OFF" "-DENABLE_EXPAT=OFF"
    "-DENABLE_WIN32_XMLLITE=OFF" "-DENABLE_PCREPOSIX=OFF"
    "-DENABLE_PCRE2POSIX=OFF" "-DENABLE_ZLIB=ON" "-DENABLE_ICONV=ON"
  ] [ zlib iconv ]).overrideAttrs (old: {
    patches = old.patches ++ lib.optional early ./libarchive-wide-fallbacks.patch;
  });
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
  inherit sdk target compiler tools cflags ldflags jansson tls curl av pdf png freetype expat fontconfig jpeg openjpeg miniaudio regex unistring xml archive iconv zlib cxx;
  application = { source, packageName, version, revision, debug ? false,
                  updateBase ? "", updateTarget ? "" }:
    pkgs.stdenvNoCC.mkDerivation {
      pname = "${packageName}-openbsd-amd64";
      inherit version;
      src = source;
      outputs = [ "out" "debug" ];
      nativeBuildInputs = [ pkgs.pkg-config ];
      buildInputs = [ jansson curl av xml archive ] ++ voiceRtc.dependencies ++ networkLibraries ++ [ regex ]
        ++ [ pdf png freetype expat fontconfig jpeg openjpeg ] ++ lib.optional legacy cxx;
      enableParallelBuilding = true;
      dontConfigure = true;
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
          'WITH_OFFICE=0' 'WITH_OFFICE_COMMANDS=1'
          'CC=${compiler} --target=${target} --sysroot=${sdk}'
          'STRIP=${tools}/llvm-strip' 'OBJCOPY=${tools}/llvm-objcopy'
          'GIT_HEAD=${revision}' 'BUILD_VERSION=${version}'
          'CPPFLAGS=${lib.optionalString (!early) "-D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 "}-D_FILE_OFFSET_BITS=64 -Ibuild -DSNAJPAGENT_CA_BUNDLE=\"ca_bundle.inc\" -DSNAJPAGENT_STATIC_UTF8 -I${regex}/include -I${unistring}/include'
          'CFLAGS=-std=c11 ${cflags}${lib.optionalString early " -Dsnprintf=rpl_snprintf -Dvsnprintf=rpl_vsnprintf -DSNAJPAGENT_LEGACY_PRINTF -Dprintf=snag_legacy_printf -Dfprintf=snag_legacy_fprintf"} ${if debug then "-Og -fno-omit-frame-pointer" else "-flto -ffunction-sections -fdata-sections"} -Wall -Wextra -Wpedantic -Werror'
          'LDFLAGS=--ld-path=${llvm.lld}/bin/ld.lld ${pkgs.lib.optionalString (!debug) "-flto"} -Wl,--gc-sections,--as-needed,-Bstatic'
          "JANSSON_CFLAGS=$(pkg-config --cflags jansson)"
          "LDLIBS=-Wl,-Bstatic $(pkg-config --static --libs jansson) -L${regex}/lib -lsnagregex -L${unistring}/lib -lunistring"
          "AV_CFLAGS=$(pkg-config --cflags libavformat libavcodec libavutil libswresample libswscale)"
          "AV_LIBS=$(pkg-config --static --libs libavformat libavcodec libavutil libswresample libswscale | sed -E 's/-l?(-l?)?pthread//g')"
          "RTC_CFLAGS=${voiceRtc.cflags}"
          "RTC_LIBS=${voiceRtc.libs} ${if legacy then "${cxx}/lib/libstdc++.a -Wl,-Bdynamic" else "-Wl,-Bdynamic -lc++ -lc++abi"} -lm -Wl,-Bstatic"
          'MINIAUDIO_CFLAGS=-isystem ${miniaudio}'
          'CXX=${cxxCompiler} --target=${target} --sysroot=${sdk}'
          'CXXFLAGS=-std=c++20 ${cflags} ${if legacy then "-nostdinc++ -isystem ${cxx}/include/c++ -isystem ${cxx}/include/c++/${target}" else "-stdlib=libc++"}${lib.optionalString early " -fno-use-cxa-atexit -include ${./bsd-legacy-cxx.h}"} ${if debug then "-Og -fno-omit-frame-pointer" else "-flto -ffunction-sections -fdata-sections"} -Wall -Wextra -Wpedantic -Werror'
          "PDF_CFLAGS=$(pkg-config --cflags poppler libpng | sed -E 's/(^| )-I/\1-isystem /g')"
          "PDF_LIBS=$(pkg-config --static --libs poppler libpng | sed -E 's/-l?(-l?)?pthread//g') ${if legacy then "${cxx}/lib/libstdc++.a -Wl,-Bdynamic" else "-Wl,-Bdynamic -lc++ -lc++abi"} -lm -Wl,-Bstatic"
          "CURL_CFLAGS=$(pkg-config --cflags libcurl)"
          "CURL_LIBS=$(pkg-config --static --libs libcurl | sed -E 's/-l?(-l?)?pthread//g') -lutil -Wl,-Bdynamic ${if legacy then "-l:libpthread.so.${threadVersion}" else "-lpthread"}${lib.optionalString early " ${legacyRt}/lib/liblegacyrt.a"}"
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
