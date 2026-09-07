# SPDX-License-Identifier: GPL-2.0-only
{ pkgs, sourcePkgs }:
let
  lib = pkgs.lib;
  llvm = pkgs.llvmPackages_21;
  target = "x86_64-unknown-freebsd8.4";
  compiler = "${llvm.clang-unwrapped}/bin/clang";
  tools = "${llvm.llvm}/bin";
  sdk = pkgs.stdenvNoCC.mkDerivation {
    pname = "freebsd-amd64-sysroot";
    version = "8.4";
    src = pkgs.fetchurl {
      name = "disc1.iso";
      url = "https://archive.freebsd.org/old-releases/amd64/ISO-IMAGES/8.4/FreeBSD-8.4-RELEASE-amd64-disc1.iso";
      sha256 = "2fb17d77d4eba34736eb98c142c56546dd73a4e7ac38895bb6c8517949282438";
    };
    nativeBuildInputs = [ pkgs.libarchive pkgs.python3 ];
    unpackPhase = ''bsdtar -xf "$src" 8.4-RELEASE/base'';
    dontConfigure = true;
    dontBuild = true;
    dontFixup = true;
    installPhase = ''
      mkdir -p "$out"
      cat 8.4-RELEASE/base/base.[a-z][a-z] | bsdtar -xf - -C "$out"
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
    '';
  };
  cflags = "-Os -g -fstack-protector-strong -D__BSD_VISIBLE=1";
  ldflags = "--ld-path=${llvm.lld}/bin/ld.lld -static";
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
        "-DCMAKE_SYSTEM_NAME=FreeBSD" "-DCMAKE_SYSTEM_VERSION=8.4"
        "-DCMAKE_SYSTEM_PROCESSOR=amd64" "-DCMAKE_SYSROOT=${sdk}"
        "-DCMAKE_C_COMPILER=${compiler}" "-DCMAKE_C_COMPILER_TARGET=${target}"
        "-DCMAKE_CXX_COMPILER=${llvm.clang-unwrapped}/bin/clang++"
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
        export CXX="${llvm.clang-unwrapped}/bin/clang++ --target=${target} --sysroot=${sdk}"
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
  brotli = cmakeLibrary sourcePkgs.brotli [ "-DBROTLI_DISABLE_TESTS=ON" ] [];
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
  networkLibraries = [ tls zlib brotli zstd cares nghttp2 iconv unistring idn2 ];
  curl = cmakeLibrary sourcePkgs.curlMinimal [
    "-DBUILD_STATIC_LIBS=ON" "-DBUILD_CURL_EXE=OFF" "-DCURL_BUILD_EVERYTHING=OFF"
    "-DCURL_USE_MBEDTLS=ON" "-DCURL_USE_OPENSSL=OFF" "-DCURL_DEFAULT_SSL_BACKEND=mbedtls"
    "-DENABLE_ARES=ON" "-DUSE_NGHTTP2=ON" "-DUSE_LIBIDN2=ON"
    "-DCURL_ZLIB=ON" "-DCURL_BROTLI=ON" "-DCURL_ZSTD=ON"
    "-DCURL_USE_LIBPSL=OFF" "-DCURL_USE_LIBSSH2=OFF" "-DCURL_USE_LIBSSH=OFF"
    "-DCURL_DISABLE_LDAP=ON" "-DCURL_DISABLE_LDAPS=ON"
    "-DCURL_CA_BUNDLE=none" "-DCURL_CA_PATH=none"
  ] networkLibraries;
in {
  inherit sdk target compiler tools cflags ldflags jansson tls curl;
  application = { source, packageName, version, revision }:
    pkgs.stdenvNoCC.mkDerivation {
      pname = "${packageName}-freebsd-amd64";
      inherit version;
      src = source;
      outputs = [ "out" "debug" ];
      nativeBuildInputs = [ pkgs.pkg-config ];
      buildInputs = [ jansson curl ] ++ networkLibraries;
      enableParallelBuilding = true;
      dontStrip = true;
      preBuild = ''
        mkdir -p build
        ${pkgs.zstd}/bin/zstd -q -19 \
          ${pkgs.cacert}/etc/ssl/certs/ca-no-trust-rules-bundle.crt -o build/ca_bundle.zst
        od -An -v -t u1 build/ca_bundle.zst |
          sed -E 's/([0-9]+)/\1,/g' > build/ca_bundle.inc
        makeFlagsArray+=(
          'TARGET_OS=FreeBSD'
          'CC=${compiler} --target=${target} --sysroot=${sdk}'
          'STRIP=${tools}/llvm-strip' 'OBJCOPY=${tools}/llvm-objcopy'
          'GIT_HEAD=${revision}' 'BUILD_VERSION=${version}'
          'CPPFLAGS=-D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_FILE_OFFSET_BITS=64 -Ibuild -DSNAJPAGENT_CA_BUNDLE=\"ca_bundle.inc\"'
          'CFLAGS=-std=c11 ${cflags} -flto -ffunction-sections -fdata-sections -Wall -Wextra -Wpedantic -Werror'
          'LDFLAGS=${ldflags} -flto -Wl,--gc-sections'
          "JANSSON_CFLAGS=$(pkg-config --cflags jansson)"
          "LDLIBS=$(pkg-config --static --libs jansson)"
          "CURL_CFLAGS=$(pkg-config --cflags libcurl)"
          "CURL_LIBS=$(pkg-config --static --libs libcurl) -lutil"
        )
      '';
      installPhase = ''
        runHook preInstall
        mkdir -p "$out/bin" "$debug"
        cp ${packageName} "$out/bin/"
        cp debug-${packageName} "$debug/"
        ln -s "$debug" "$out/bin/.debug"
        runHook postInstall
      '';
    };
}
