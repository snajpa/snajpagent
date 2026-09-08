# SPDX-License-Identifier: GPL-2.0-only
# Static LibreOfficeKit dependency build. Application linkage/runtime packaging
# remains unfinished; this derivation does not constitute a distribution.
{ pkgs, musl, static ? musl.pkgsStatic }:
let
  base = static.libreoffice.unwrapped.override {
    withJava = false;
    withHelp = false;
    withFonts = false;
    langs = [ "en-US" ];
  };
  headless = [
    "--disable-gui" "--disable-dbus" "--disable-cups"
    "--disable-gstreamer-1-0" "--disable-avmedia" "--disable-skia"
    "--disable-opencl" "--disable-pdfimport" "--disable-poppler"
    "--disable-gpgmepp" "--disable-curl" "--without-webdav" "--without-tls"
    "--disable-openssl" "--disable-nss" "--disable-ldap"
    "--disable-database-connectivity" "--disable-firebird-sdbc"
    "--disable-report-builder" "--disable-extensions" "--disable-scripting"
    "--without-java" "--enable-python=no" "--disable-odk"
    "--disable-online-update" "--disable-xmlhelp" "--without-help"
    "--without-doxygen" "--without-junit" "--without-export-validation"
    "--without-galleries" "--without-fonts" "--without-myspell-dicts"
    "--disable-fetch-external" "--without-buildconfig-recorded"
  ];
in base.overrideAttrs (old: {
  pname = "libreofficekit";
  nativeBuildInputs = with pkgs; [
    autoconf automake bison flex fontforge gettext gperf libtool
    perl perlPackages.ArchiveZip perlPackages.IOCompress pkg-config
    python3 unzip zip which gnumake
  ];
  buildInputs = with static; [ fontconfig freetype zlib libxml2 libxslt expat ];
  propagatedBuildInputs = [];
  # Use the source's pinned internal dependencies for the headless build;
  # the desktop recipe's GUI/Java libraries cannot supply a static closure.
  postPatch = ''
    substituteInPlace configure.ac --replace-fail distutils.sysconfig sysconfig
  '';
  env = {};
  configureFlags = headless ++ [
    "--host=${musl.stdenv.hostPlatform.config}"
    "--build=${pkgs.stdenv.buildPlatform.config}"
    "--disable-dynamic-loading" "--enable-customtarget-components"
    "--with-locales=en"
    "--with-system-fontconfig" "--with-system-freetype"
    "--with-system-zlib" "--with-system-libxml" "--with-system-expat"
    "--with-build-platform-configure-options=${pkgs.lib.concatStringsSep " " headless}"
  ];
  preConfigure = old.preConfigure + ''
    export CC_FOR_BUILD=${pkgs.stdenv.cc}/bin/cc
    export CXX_FOR_BUILD=${pkgs.stdenv.cc}/bin/c++
    export PKG_CONFIG_FOR_BUILD=${pkgs.pkg-config}/bin/pkg-config
  '';
  # Generated-document tests belong to the linked application. Desktop GUI
  # tests are not runnable in this headless dependency profile.
  doCheck = false;
  disallowedRequisites = [];
  installPhase = ''
    mkdir -p "$out/lib/libreoffice" "$out/lib/archives" "$out/include"
    cp -a instdir/. "$out/lib/libreoffice/"
    cp -a include/LibreOfficeKit "$out/include/"
    find workdir -name '*.a' -type f -exec cp --parents '{}' "$out/lib/archives/" \;
    cp config_host.mk "$out/lib/archives/"
  '';
})
