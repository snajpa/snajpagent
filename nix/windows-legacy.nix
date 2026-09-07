# SPDX-License-Identifier: GPL-2.0-only
# Internal old-Windows port; not a qualified production target.
{ pkgs ? (import ./portable.nix { }).pkgs }:
let
  windows = import pkgs.path {
    localSystem = pkgs.stdenv.buildPlatform.system;
    crossSystem = {
      config = "i686-w64-mingw32";
      libc = "msvcrt";
      useLLVM = true;
    };
  };
in import ./windows.nix { inherit pkgs windows; winver = "0x0500"; }
