# SPDX-License-Identifier: GPL-2.0-only
# Internal old-Windows port; not a qualified production target.
{ pkgs ? (import ./portable.nix { }).pkgs, arch ? "i686" }:
assert builtins.elem arch [ "i686" "x86_64" ];
let
  windows = import pkgs.path {
    localSystem = pkgs.stdenv.buildPlatform.system;
    crossSystem = {
      config = "${arch}-w64-mingw32";
      libc = "msvcrt";
      useLLVM = true;
    };
  };
in import ./windows.nix {
  inherit pkgs windows;
  winver = if arch == "i686" then "0x0500" else "0x0502";
}
