# SPDX-License-Identifier: GPL-2.0-only
{ buildVersion ? null, buildRevision ? null }:
let
  pkgs = import (builtins.fetchTarball {
    url = "https://github.com/NixOS/nixpkgs/archive/b6018f87da91d19d0ab4cf979885689b469cdd41.tar.gz";
    sha256 = "sha256-twXPFqFsrrY5r28Zh7Homgcp2gUMBgQ6WDS98Q/3xFI=";
  }) { };
  linux = musl: import ./linux.nix { inherit pkgs musl; };
  x86 = linux pkgs.pkgsCross.musl64;
  static = x86.static;
  source = builtins.fetchGit { url = toString ../.; };
  revision = if source ? dirtyRev then
    pkgs.lib.removeSuffix "-dirty" source.dirtyRev else source.rev;
  metadata = pkgs.lib.splitString "\n" (builtins.readFile (source + "/META"));
  metaValue = key: let prefix = "override ${key} = "; in
    pkgs.lib.removePrefix prefix
      (pkgs.lib.findFirst (pkgs.lib.hasPrefix prefix) (throw "META lacks ${key}") metadata);
  packageName = metaValue "NAME";
  version = if buildVersion != null then buildVersion else
    throw "derive buildVersion from Git tags using make prod-linux-x86_64 (or the desired prod target)";
in assert buildRevision == null || buildRevision == revision; rec {
  inherit pkgs static;
  inherit (x86) tls curl;
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
  linux-x86_64 = x86.application { inherit source packageName version revision; };
  linux-aarch64 = (linux pkgs.pkgsCross.aarch64-multiplatform-musl).application {
    inherit source packageName version revision;
  };
  windows-x86_64 = (import ./windows.nix { inherit pkgs; }).application {
    inherit source packageName version revision;
  };
  windows-arm64 = (import ./windows.nix {
    inherit pkgs;
    windows = pkgs.pkgsCross.ucrtAarch64;
  }).application { inherit source packageName version revision; };
}
