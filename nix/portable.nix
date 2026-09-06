# SPDX-License-Identifier: GPL-2.0-only
{ buildVersion ? null, buildRevision ? null }:
let
  pkgs = import (builtins.fetchTarball {
    url = "https://github.com/NixOS/nixpkgs/archive/b6018f87da91d19d0ab4cf979885689b469cdd41.tar.gz";
    sha256 = "sha256-twXPFqFsrrY5r28Zh7Homgcp2gUMBgQ6WDS98Q/3xFI=";
  }) { };
  musl = pkgs.pkgsCross.musl64;
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
  source = builtins.fetchGit { url = toString ../.; };
  revision = if source ? dirtyRev then
    pkgs.lib.removeSuffix "-dirty" source.dirtyRev else source.rev;
  metadata = pkgs.lib.splitString "\n" (builtins.readFile (source + "/META"));
  metaValue = key: let prefix = "override ${key} = "; in
    pkgs.lib.removePrefix prefix
      (pkgs.lib.findFirst (pkgs.lib.hasPrefix prefix) (throw "META lacks ${key}") metadata);
  packageName = metaValue "NAME";
  version = if buildVersion != null then buildVersion else
    "${metaValue "VERSION"}-${source.dirtyShortRev or source.shortRev}";
in assert buildRevision == null || buildRevision == revision; rec {
  inherit pkgs static tls curl;
  macos-arm64 = (import ./macos.nix {
    inherit pkgs;
    sourcePkgs = static;
    arch = "arm64";
  }).application { inherit source packageName version revision; };
  macos-x86_64 = (import ./macos.nix {
    inherit pkgs;
    sourcePkgs = static;
    arch = "x86_64";
  }).application { inherit source packageName version revision; };
  macos-universal = pkgs.runCommand "${packageName}-macos-universal-${version}" {
    nativeBuildInputs = [ pkgs.llvmPackages_21.llvm ];
    outputs = [ "out" "debug" ];
  } ''
    mkdir -p "$out/bin" "$debug/${packageName}.dSYM/Contents/Resources/DWARF"
    llvm-lipo -create ${macos-arm64}/bin/${packageName} \
      ${macos-x86_64}/bin/${packageName} -output "$out/bin/${packageName}"
    cp ${macos-arm64.debug}/${packageName}.dSYM/Contents/Info.plist \
      "$debug/${packageName}.dSYM/Contents/Info.plist"
    llvm-lipo -create \
      ${macos-arm64.debug}/${packageName}.dSYM/Contents/Resources/DWARF/${packageName} \
      ${macos-x86_64.debug}/${packageName}.dSYM/Contents/Resources/DWARF/${packageName} \
      -output "$debug/${packageName}.dSYM/Contents/Resources/DWARF/${packageName}"
    ln -s "$debug/${packageName}.dSYM" "$out/bin/${packageName}.dSYM"
  '';
  linux-x86_64 = musl.stdenv.mkDerivation {
    pname = packageName;
    inherit version;
    src = source;
    outputs = [ "out" "debug" ];
    nativeBuildInputs = [ pkgs.pkg-config ];
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
        "JANSSON_CFLAGS=$(pkg-config --cflags jansson)"
        "LDLIBS=$(pkg-config --static --libs jansson)"
        "CURL_CFLAGS=$(pkg-config --cflags libcurl)"
        "CURL_LIBS=$(pkg-config --static --libs libcurl)"
      )
    '';
    installPhase = ''
      runHook preInstall
      mkdir -p "$out/bin" "$debug"
      cp ${packageName} "$out/bin/"
      cp ${packageName}.debug "$debug/"
      ln -s "$debug" "$out/bin/.debug"
      runHook postInstall
    '';
  };
}
