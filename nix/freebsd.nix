# SPDX-License-Identifier: GPL-2.0-only
{ pkgs, sourcePkgs, osVersion ? "8.4" }:
let
  lib = pkgs.lib;
  llvm = pkgs.llvmPackages_21;
  legacy = lib.versionOlder osVersion "7.0";
  early = lib.versionOlder osVersion "5.3";
  threads = if early then "c_r" else "pthread";
  mediaRoot = lib.optionalString (!early) "${osVersion}-RELEASE/";
  target = "x86_64-unknown-freebsd${osVersion}";
  compiler = if legacy then "${oldCompiler}/bin/clang" else "${llvm.clang-unwrapped}/bin/clang";
  cxxCompiler = if legacy then "${oldCompiler}/bin/clang++" else "${llvm.clang-unwrapped}/bin/clang++";
  tools = "${llvm.llvm}/bin";
  sdk = pkgs.stdenvNoCC.mkDerivation {
    pname = "freebsd-amd64-sysroot";
    version = osVersion;
    src = pkgs.fetchurl {
      name = if early then "miniinst.iso" else "disc1.iso";
      url = "https://archive.freebsd.org/old-releases/amd64/ISO-IMAGES/${osVersion}/${lib.optionalString (!legacy) "FreeBSD-"}${osVersion}-RELEASE-amd64-${if early then "miniinst" else "disc1"}.iso";
      sha256 = {
        "8.4" = "2fb17d77d4eba34736eb98c142c56546dd73a4e7ac38895bb6c8517949282438";
        # Official HTTPS and matching publisher MD5; SHA256 computed locally.
        "5.1" = "701dceb84e046858ec52ed2e1fa05daa880523766b254941dbf0abbccc9248dc";
        "5.5" = "f71eedf18ab24d973c938b473ca127018eb87ab1d1b4c96a5d8d1e9cd8f261d3";
      }.${osVersion};
    };
    nativeBuildInputs = [ pkgs.libarchive pkgs.python3 ];
    unpackPhase = ''bsdtar -xf "$src" ${mediaRoot}base'';
    dontConfigure = true;
    dontBuild = true;
    dontFixup = true;
    installPhase = ''
      mkdir -p "$out"
      cat ${mediaRoot}base/base.[a-z][a-z] | bsdtar -xf - -C "$out"
      python3 - "$out" <<'PY'
      import os, sys
      root = sys.argv[1]
      for directory, dirs, files in os.walk(root):
          for name in dirs + files:
              path = os.path.join(directory, name)
              if os.path.islink(path):
                  dest = os.readlink(path)
                  if dest.startswith('/'):
                      os.unlink(path)
                      os.symlink(os.path.relpath(root + dest, directory), path)
      PY
    '' + lib.optionalString early ''
      # 5.1 recognizes GCC 3 exactly; Clang implements these attributes too.
      substituteInPlace "$out/usr/include/sys/cdefs.h" \
        --replace-fail '|| __GNUC__ == 3' '|| __GNUC__ >= 3'
    '';
  };
  # GCC 3.4's base runtime predates crtbeginT.o and the split libgcc_eh.
  # Supply the actual old startup/runtime ordering rather than fake archives.
  oldCompiler = pkgs.runCommand "freebsd-${osVersion}-clang" {} ''
    mkdir -p "$out/bin"
    cat > "$out/bin/clang" <<'SH'
    #!${pkgs.runtimeShell}
    link=1
    shared=0
    cxx=0
    case "$0" in *++) cxx=1;; esac
    for arg in "$@"; do
      case "$arg" in
        -c|-S|-E|-M|-MM|-fsyntax-only|--version|-dump*|-print*) link=0;;
        -shared) shared=1;;
      esac
    done
    cc=${llvm.clang-unwrapped}/bin/clang
    extra=()
    if [ "$cxx" = 1 ]; then
      cc="$cc++"
      extra=(-lstdc++)
    fi
    if [ "$link" = 0 ]; then
      exec "$cc" "$@"
    fi
    start=(${sdk}/usr/lib/crt1.o ${sdk}/usr/lib/crti.o ${sdk}/usr/lib/crtbegin.o)
    end=(${sdk}/usr/lib/crtend.o ${sdk}/usr/lib/crtn.o)
    if [ "$shared" = 1 ]; then
      start=(${sdk}/usr/lib/crti.o ${sdk}/usr/lib/crtbeginS.o)
      end=(${sdk}/usr/lib/crtendS.o ${sdk}/usr/lib/crtn.o)
    fi
    exec "$cc" ${lib.optionalString early "-Wl,--dynamic-linker=/usr/libexec/ld-elf.so.1"} -nostdlib "''${start[@]}" "$@" -Wl,--start-group \
      "''${extra[@]}" -l${threads} -lc -lgcc -Wl,--end-group "''${end[@]}"
    SH
    chmod +x "$out/bin/clang"
    ln -s clang "$out/bin/clang++"
  '';
  # Early libc_r enters user thread stacks eight bytes off the amd64 ABI.
  # Realign compiled entry points, including library callbacks and workers.
  cflags = "-Os -g -D__BSD_VISIBLE=1"
    + lib.optionalString early " -mstackrealign"
    + lib.optionalString (!legacy) " -fstack-protector-strong";
  ldflags = "--ld-path=${llvm.lld}/bin/ld.lld -static";
  compilerBuiltins = pkgs.runCommand "compiler-rt-freebsd-${osVersion}" {} ''
    mkdir -p "$out/lib"
    for file in udivti3.c udivmodti4.c; do
      ${compiler} --target=${target} --sysroot=${sdk} -Os -g -fno-stack-protector \
        -c ${llvm.compiler-rt.src}/compiler-rt/lib/builtins/"$file" \
        -o "$file.o"
    done
    ${tools}/llvm-ar rcs "$out/lib/libclang_rt.builtins.a" ./*.o
  '';
  cmakeLibrary = package: flags: dependencies:
    pkgs.stdenvNoCC.mkDerivation {
      pname = "${package.pname}-freebsd-amd64";
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
          "-DCMAKE_CXX_FLAGS=${cflags} -stdlib=libstdc++"
          "-DCMAKE_EXE_LINKER_FLAGS=${ldflags}"
        )
      '';
      cmakeFlags = [
        "-DCMAKE_SYSTEM_NAME=FreeBSD" "-DCMAKE_SYSTEM_VERSION=${osVersion}"
        "-DCMAKE_SYSTEM_PROCESSOR=amd64" "-DCMAKE_SYSROOT=${sdk}"
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
      pname = "${package.pname}-freebsd-amd64";
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
  jansson = cmakeLibrary sourcePkgs.jansson [
    "-DJANSSON_BUILD_SHARED_LIBS=OFF" "-DJANSSON_BUILD_DOCS=OFF"
    "-DJANSSON_WITHOUT_TESTS=ON" "-DJANSSON_EXAMPLES=OFF"
  ] [];
  tls = (cmakeLibrary sourcePkgs.mbedtls [
    "-DUSE_SHARED_MBEDTLS_LIBRARY=OFF" "-DENABLE_PROGRAMS=OFF"
    "-DENABLE_TESTING=OFF" "-DGEN_FILES=OFF"
  ] []).overrideAttrs (_: {
    postPatch = ''
      perl scripts/config.pl set MBEDTLS_THREADING_C
      perl scripts/config.pl set MBEDTLS_THREADING_PTHREAD
      substituteInPlace library/net_sockets.c \
        --replace-fail 'fd >= FD_SETSIZE' '(unsigned int) fd >= FD_SETSIZE'
    '';
  });
  zlib = cmakeLibrary sourcePkgs.zlib [
    "-DZLIB_BUILD_SHARED=OFF" "-DZLIB_BUILD_STATIC=ON" "-DZLIB_BUILD_TESTING=OFF"
  ] [];
  brotli = (cmakeLibrary sourcePkgs.brotli [ "-DBROTLI_DISABLE_TESTS=ON" ] []).overrideAttrs (_: {
    postPatch = lib.optionalString legacy ''
      # This libm has log but not log2; brotli's fallback still requires -lm.
      substituteInPlace CMakeLists.txt \
        --replace-fail 'add_definitions(-DBROTLI_HAVE_LOG2=0)' \
          'set(LIBM_LIBRARY "m")
    add_definitions(-DBROTLI_HAVE_LOG2=0)'
    '';
  });
  zstd = (cmakeLibrary sourcePkgs.zstd [
    "-DZSTD_BUILD_SHARED=OFF" "-DZSTD_BUILD_STATIC=ON"
    "-DZSTD_BUILD_PROGRAMS=OFF" "-DZSTD_BUILD_TESTS=OFF"
  ] []).overrideAttrs (_: { cmakeDir = "../build/cmake"; });
  cares = (cmakeLibrary sourcePkgs.c-ares [
    "-DCARES_SHARED=OFF" "-DCARES_STATIC=ON" "-DCARES_STATIC_PIC=ON"
    "-DCARES_BUILD_TOOLS=OFF" "-DCARES_BUILD_TESTS=OFF"
  ] []).overrideAttrs (_: {
    postPatch = ''
      # Old BSD net/if.h needs sockaddr declared before configure's type probes.
      substituteInPlace CMakeLists.txt \
        --replace-fail 'CARES_EXTRAINCLUDE_IFSET (HAVE_NET_IF_H       net/if.h)' \
          'CARES_EXTRAINCLUDE_IFSET (HAVE_NET_IF_H       "sys/socket.h;net/if.h")'
    '';
  });
  nghttp2 = cmakeLibrary sourcePkgs.nghttp2 [
    "-DENABLE_LIB_ONLY=ON" "-DBUILD_STATIC_LIBS=ON" "-DENABLE_DOC=OFF"
  ] [];
  iconv = autotoolsLibrary pkgs.libiconvReal [] [];
  unistring = autotoolsLibrary sourcePkgs.libunistring
    [ "--with-libiconv-prefix=${iconv}" ] [ iconv ];
  idn2 = (autotoolsLibrary sourcePkgs.libidn2 [
    "--disable-doc" "--with-libiconv-prefix=${iconv}"
    "--with-libunistring-prefix=${unistring}"
  ] [ iconv unistring ]).overrideAttrs (_: {
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
  regex = import ./windows-regex.nix {
    inherit pkgs unistring;
    cross = { inherit compiler cxxCompiler target sdk tools cflags ldflags; };
  };
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
    postInstall = lib.optionalString (!legacy) ''
      # curl prefixes the imported Threads target's -lpthread flag twice.
      substituteInPlace "$out/lib/pkgconfig/libcurl.pc" \
        --replace-fail '-l-lpthread' '-lpthread'
    '';
  });
in {
  inherit sdk target compiler tools cflags ldflags jansson tls curl regex unistring;
  application = { source, packageName, version, revision, debug ? false,
                  updateBase ? "", updateTarget ? "" }:
    pkgs.stdenvNoCC.mkDerivation {
      pname = "${packageName}-freebsd-amd64";
      inherit version;
      src = source;
      outputs = [ "out" "debug" ];
      nativeBuildInputs = [ pkgs.pkg-config ];
      buildInputs = [ jansson curl ] ++ networkLibraries ++ lib.optional early regex;
      enableParallelBuilding = true;
      dontStrip = true;
      preBuild = ''
        mkdir -p build
        ${pkgs.zstd}/bin/zstd -q -19 \
          ${pkgs.cacert}/etc/ssl/certs/ca-no-trust-rules-bundle.crt -o build/ca_bundle.zst
        od -An -v -t u1 build/ca_bundle.zst |
          sed -E 's/([0-9]+)/\1,/g' > build/ca_bundle.inc
        # Keep application/Unicode libraries and libutil static; use one
        # native threading runtime with libc.
        # c-ares' pkg-config pthread flags must not pull in old static libthr.
        makeFlagsArray+=(
          'DEBUG=${if debug then "1" else "0"}'
          ${pkgs.lib.optionalString (updateBase != "") "'UPDATE_BASE_URL=${updateBase}' 'UPDATE_TARGET=${updateTarget}'"}
          'TARGET_OS=FreeBSD'
          'CC=${compiler} --target=${target} --sysroot=${sdk}'
          'STRIP=${tools}/llvm-strip' 'OBJCOPY=${tools}/llvm-objcopy'
          'GIT_HEAD=${revision}' 'BUILD_VERSION=${version}'
          'CPPFLAGS=-D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_FILE_OFFSET_BITS=64 -Ibuild -DSNAJPAGENT_CA_BUNDLE=\"ca_bundle.inc\"${lib.optionalString early " -DSNAJPAGENT_LEGACY_BSD_JOIN -DSNAJPAGENT_STATIC_UTF8 -I${regex}/include -I${unistring}/include"}'
          'CFLAGS=-std=c11 ${cflags} ${if debug then "-Og -fno-omit-frame-pointer" else "-flto -ffunction-sections -fdata-sections"} -Wall -Wextra -Wpedantic -Werror'
          'LDFLAGS=--ld-path=${llvm.lld}/bin/ld.lld ${pkgs.lib.optionalString (!debug) "-flto"} -Wl,--gc-sections,--as-needed,-Bstatic${lib.optionalString early " -Wl,--wrap=pthread_join"}'
          "JANSSON_CFLAGS=$(pkg-config --cflags jansson)"
          "LDLIBS=-Wl,-Bstatic $(pkg-config --static --libs jansson)${lib.optionalString early " -L${regex}/lib -lsnagregex -L${unistring}/lib -lunistring"}"
          "CURL_CFLAGS=$(pkg-config --cflags libcurl)"
          "CURL_LIBS=$(pkg-config --static --libs libcurl | sed -E 's/-l(-l)?pthread//g') -lutil${lib.optionalString early " ${compilerBuiltins}/lib/libclang_rt.builtins.a"} -Wl,-Bdynamic -l${threads}"
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
