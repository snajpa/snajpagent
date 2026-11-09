# SPDX-License-Identifier: GPL-2.0-only
final: previous:
let
  mirrors = package: urls: package.overrideAttrs (old: {
    src = if (old.src or null) != null && old.src ? overrideAttrs then old.src.overrideAttrs (source: {
      urls = previous.lib.unique (source.urls ++ urls);
    }) else old.src or null;
  });
in {
  brotli = mirrors previous.brotli [
    "https://codeload.github.com/google/brotli/tar.gz/v${previous.brotli.version}"
  ];
  jansson = mirrors previous.jansson [
    "https://codeload.github.com/akheron/jansson/tar.gz/v${previous.jansson.version}"
  ];
  zstd = mirrors previous.zstd [
    "https://codeload.github.com/facebook/zstd/tar.gz/v${previous.zstd.version}"
  ];
  c-ares = mirrors previous.c-ares [
    "https://sources.buildroot.net/c-ares/c-ares-${previous.c-ares.version}.tar.gz"
    "https://distfiles.macports.org/c-ares/c-ares-${previous.c-ares.version}.tar.gz"
  ];
  curl = mirrors previous.curl [
    "https://curl.se/download/curl-${previous.curl.version}.tar.xz"
  ];
  curlMinimal = mirrors previous.curlMinimal [
    "https://curl.se/download/curl-${previous.curlMinimal.version}.tar.xz"
  ];
  libidn2 = mirrors previous.libidn2 [
    "mirror://gnu/libidn/libidn2-${previous.libidn2.version}.tar.gz"
  ];
  uclibc-ng = mirrors previous.uclibc-ng [
    "https://sources.buildroot.net/uclibc/uClibc-ng-${previous.uclibc-ng.version}.tar.xz"
  ];
}
