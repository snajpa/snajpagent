# SPDX-License-Identifier: GPL-2.0-only
{ buildVersion ? null, buildRevision ? null, debug ? false, updateBase ? "" }:
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
  # A published commit-suffixed build always carries runnable debug symbols.
  debugBuild = debug || (updateBase != "" && pkgs.lib.hasInfix "-" version);
  args = updateTarget: {
    inherit source packageName version revision updateBase updateTarget;
    debug = debugBuild;
  };
  mac = arch: updateTarget: (import ./macos.nix {
    inherit pkgs arch;
    sourcePkgs = static;
  }).application (args updateTarget);
  universalArm = mac "arm64" "macos-universal";
  universalIntel = mac "x86_64" "macos-universal";
in assert buildRevision == null || buildRevision == revision; rec {
  inherit pkgs static;
  inherit (x86) tls curl;
  macos-arm64 = mac "arm64" "macos-arm64";
  macos-x86_64 = mac "x86_64" "macos-x86_64";
  macos-universal = pkgs.runCommand "${packageName}-macos-universal-${version}" {
    nativeBuildInputs = [ pkgs.llvmPackages_21.llvm ];
    outputs = [ "out" "debug" ];
  } ''
    mkdir -p "$out/bin" "$debug/${packageName}.dSYM/Contents/Resources/DWARF"
    llvm-lipo -create ${universalArm}/bin/${packageName} \
      ${universalIntel}/bin/${packageName} -output "$out/bin/${packageName}"
    cp ${universalArm.debug}/${packageName}.dSYM/Contents/Info.plist \
      "$debug/${packageName}.dSYM/Contents/Info.plist"
    llvm-lipo -create \
      ${universalArm.debug}/${packageName}.dSYM/Contents/Resources/DWARF/${packageName} \
      ${universalIntel.debug}/${packageName}.dSYM/Contents/Resources/DWARF/${packageName} \
      -output "$debug/${packageName}.dSYM/Contents/Resources/DWARF/${packageName}"
    ln -s "$debug/${packageName}.dSYM" "$out/bin/${packageName}.dSYM"
  '';
  linux-x86_64 = x86.application (args "linux-x86_64");
  linux-i686 = (linux pkgs.pkgsCross.musl32).application (args "linux-i686");
  linux-i686-legacy = (import ./linux-legacy.nix { inherit pkgs; }).application (args "linux-i686-legacy");
  linux-aarch64 = (linux pkgs.pkgsCross.aarch64-multiplatform-musl).application (args "linux-aarch64");
  freebsd-amd64 = (import ./freebsd.nix {
    inherit pkgs;
    sourcePkgs = static;
  }).application (args "freebsd-amd64");
  freebsd-amd64-legacy = (import ./freebsd.nix {
    inherit pkgs;
    sourcePkgs = static;
    osVersion = "5.5";
  }).application (args "freebsd-amd64-legacy");
  windows-x86_64 = (import ./windows-legacy.nix { inherit pkgs; arch = "x86_64"; }).application (args "windows-x86_64");
  windows-arm64 = (import ./windows.nix {
    inherit pkgs;
    windows = pkgs.pkgsCross.ucrtAarch64;
  }).application (args "windows-arm64");
}
