# SPDX-License-Identifier: GPL-2.0-only
{ pkgs, sourcePkgs }:
let
  inherit (pkgs) lib;
  osVersion = "5.2.3";
  target = "x86_64-unknown-netbsd5.2";
  llvm = pkgs.llvmPackages_21;
  tools = "${llvm.llvm}/bin";
  sdk = pkgs.stdenvNoCC.mkDerivation {
    pname = "netbsd-amd64-sysroot";
    version = osVersion;
    src = pkgs.fetchurl {
      url = "https://archive.netbsd.org/pub/NetBSD-archive/NetBSD-${osVersion}/amd64/binary/sets/base.tgz";
      sha256 = "693a8018ec6271cfc8a267ee8364de6a125d1483f47e9bc6bcaec507c2c0f784";
    };
    compilerSet = pkgs.fetchurl {
      url = "https://archive.netbsd.org/pub/NetBSD-archive/NetBSD-${osVersion}/amd64/binary/sets/comp.tgz";
      sha256 = "90be70dec28c68db354564d810613f48a7ed2d17d9cff2b381e3f9db4f94f54e";
    };
    nativeBuildInputs = [ pkgs.libarchive pkgs.python3 ];
    dontUnpack = true;
    dontConfigure = true;
    dontBuild = true;
    dontFixup = true;
    installPhase = ''
      mkdir -p "$out"
      bsdtar -xf "$src" -C "$out"
      bsdtar -xf "$compilerSet" -C "$out"
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
      PYSDK
    '';
  };
  compilerWrapper = pkgs.runCommand "netbsd-${osVersion}-clang" {} ''
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
    # Native signal.h uses GNU89 extern-inline semantics, not C99 semantics.
    inlineFlags=(-fgnu89-inline)
    case "$0" in *++) cc="$cc++"; inlineFlags=(); extra=(-lstdc++ -lm -lgcc_s);; esac
    if [ "$link" = 0 ]; then exec "$cc" "''${inlineFlags[@]}" "$@"; fi
    start=(${sdk}/usr/lib/crt0.o ${sdk}/usr/lib/crti.o ${sdk}/usr/lib/crtbegin.o)
    end=(${sdk}/usr/lib/crtend.o ${sdk}/usr/lib/crtn.o)
    flags=(-Wl,-no-pie,-e,_start,--dynamic-linker=/libexec/ld.elf_so)
    if [ "$shared" = 1 ]; then
      start=(${sdk}/usr/lib/crti.o ${sdk}/usr/lib/crtbeginS.o)
      end=(${sdk}/usr/lib/crtendS.o ${sdk}/usr/lib/crtn.o)
      flags=()
    fi
    exec "$cc" "''${inlineFlags[@]}" -nostdlib "''${flags[@]}" "''${start[@]}" "$@" \
      -Wl,-Bdynamic "''${extra[@]}" -lpthread -lc ${sdk}/usr/lib/libgcc.a "''${end[@]}"
    SH
    chmod +x "$out/bin/clang"
    ln -s clang "$out/bin/clang++"
  '';
  compiler = "${compilerWrapper}/bin/clang";
  cxxCompiler = "${compilerWrapper}/bin/clang++";
  cflags = "-Os -g -D_NETBSD_SOURCE -fPIC -fstack-protector-strong";
  ldflags = "--ld-path=${llvm.lld}/bin/ld.lld";
  cmakeLibrary = package: flags: dependencies:
    pkgs.stdenvNoCC.mkDerivation {
      pname = "${package.pname}-netbsd-amd64";
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
        "-DCMAKE_SYSTEM_NAME=NetBSD" "-DCMAKE_SYSTEM_VERSION=${osVersion}"
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
      pname = "${package.pname}-netbsd-amd64";
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
      # The RSA self-test uses the word-sized API provided by this native libc.
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
  brotli = cmakeLibrary sourcePkgs.brotli [ "-DBROTLI_DISABLE_TESTS=ON" ] [];
  zstd = (cmakeLibrary sourcePkgs.zstd [
    "-DZSTD_BUILD_SHARED=OFF" "-DZSTD_BUILD_STATIC=ON"
    "-DZSTD_BUILD_PROGRAMS=OFF" "-DZSTD_BUILD_TESTS=OFF"
  ] []).overrideAttrs (_: { cmakeDir = "../build/cmake"; });
  cares = cmakeLibrary sourcePkgs.c-ares [
    "-DCARES_SHARED=OFF" "-DCARES_STATIC=ON" "-DCARES_STATIC_PIC=ON"
    "-DCARES_BUILD_TOOLS=OFF" "-DCARES_BUILD_TESTS=OFF"
  ] [];
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
    postPatch = ''
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
  ] networkLibraries);
in {
  inherit sdk target compiler tools cflags ldflags jansson tls curl regex unistring;
  application = { source, packageName, version, revision, debug ? false,
                  updateBase ? "", updateTarget ? "" }:
    pkgs.stdenvNoCC.mkDerivation {
      pname = "${packageName}-netbsd-amd64";
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
          'TARGET_OS=NetBSD'
          'CC=${compiler} --target=${target} --sysroot=${sdk}'
          'STRIP=${tools}/llvm-strip' 'OBJCOPY=${tools}/llvm-objcopy'
          'GIT_HEAD=${revision}' 'BUILD_VERSION=${version}'
          'CPPFLAGS=-D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_FILE_OFFSET_BITS=64 -Ibuild -DSNAJPAGENT_CA_BUNDLE=\"ca_bundle.inc\" -DSNAJPAGENT_STATIC_UTF8 -I${regex}/include -I${unistring}/include' 
          'CFLAGS=-std=c11 ${cflags} ${if debug then "-Og -fno-omit-frame-pointer" else "-flto -ffunction-sections -fdata-sections"} -Wall -Wextra -Wpedantic -Werror'
          'LDFLAGS=--ld-path=${llvm.lld}/bin/ld.lld ${pkgs.lib.optionalString (!debug) "-flto"} -Wl,--gc-sections,--as-needed,-Bstatic'
          "JANSSON_CFLAGS=$(pkg-config --cflags jansson)"
          "LDLIBS=-Wl,-Bstatic $(pkg-config --static --libs jansson) -L${regex}/lib -lsnagregex -L${unistring}/lib -lunistring"
          "CURL_CFLAGS=$(pkg-config --cflags libcurl)"
          "CURL_LIBS=$(pkg-config --static --libs libcurl | sed -E 's/-l(-l)?pthread//g') -lutil -Wl,-Bdynamic -lpthread"
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
