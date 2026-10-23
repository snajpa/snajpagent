# SPDX-License-Identifier: GPL-2.0-only
{ pkgs }:
{
  pname = "snajpagent-vterm";
  version = "0.3.3";
  src = pkgs.fetchurl {
    url = "https://www.leonerd.org.uk/code/libvterm/libvterm-0.3.3.tar.gz";
    sha256 = "09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0";
  };
  patches = [ ./libvterm-frontend.patch ];
}
