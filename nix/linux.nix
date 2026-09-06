# SPDX-License-Identifier: GPL-2.0-only
{ pkgs, musl }:
let
  static = musl.pkgsStatic;
  tls = static.mbedtls;
  curl = (static.curlMinimal.override {
    opensslSupport = false;
    scpSupport = false;
    gssSupport = false;
    http2Support = true;
    idnSupport = true;
    zlibSupport = true;
    brotliSupport = true;
    zstdSupport = true;
    c-aresSupport = true;
  }).overrideAttrs (old: {
    propagatedBuildInputs = old.propagatedBuildInputs ++ [ tls ];
    configureFlags = builtins.filter (flag: flag != "--without-ssl") old.configureFlags ++ [
      "--with-mbedtls=${pkgs.lib.getDev tls}"
      "--without-ca-bundle"
      "--without-ca-path"
    ];
  });
in {
  inherit static tls curl;
  application = { source, packageName, version, revision }: musl.stdenv.mkDerivation {
    pname = packageName;
    inherit version;
    src = source;
    outputs = [ "out" "debug" ];
    nativeBuildInputs = [ musl.buildPackages.pkg-config ];
    buildInputs = [ static.jansson curl ];
    enableParallelBuilding = true;
    dontStrip = true;
    preBuild = ''
      mkdir -p build
      od -An -v -t u1 ${pkgs.cacert}/etc/ssl/certs/ca-no-trust-rules-bundle.crt |
        sed -E 's/([0-9]+)/\1,/g' > build/ca_bundle.inc
      makeFlagsArray+=(
        'TARGET_OS=Linux'
        "CC=$CC" "STRIP=$STRIP" "OBJCOPY=$OBJCOPY"
        "GIT_HEAD=${revision}" "BUILD_VERSION=${version}"
        'CPPFLAGS=-D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Ibuild -DSNAJPAGENT_CA_BUNDLE=\"ca_bundle.inc\"'
        'CFLAGS=-std=c11 -Os -g -flto -ffunction-sections -fdata-sections -Wall -Wextra -Wpedantic -Werror'
        'LDFLAGS=-static-pie -flto -Wl,--gc-sections'
        "JANSSON_CFLAGS=$($PKG_CONFIG --cflags jansson)"
        "LDLIBS=$($PKG_CONFIG --static --libs jansson)"
        "CURL_CFLAGS=$($PKG_CONFIG --cflags libcurl)"
        "CURL_LIBS=$($PKG_CONFIG --static --libs libcurl)"
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
