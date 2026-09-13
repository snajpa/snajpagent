# SPDX-License-Identifier: GPL-2.0-only
''
  substituteInPlace poppler/TextOutputDev.cc \
    --replace-fail 'fmin(' '__builtin_fmin(' \
    --replace-fail 'fmax(' '__builtin_fmax('
  # Early BSD math headers omit std predicates or define function-like macros.
  substituteInPlace fofi/FoFiType1C.cc \
    --replace-fail 'std::isinf(' '__builtin_isinf('
  substituteInPlace poppler/Function.cc poppler/MarkedContentOutputDev.cc \
    poppler/TextOutputDev.cc splash/SplashXPathScanner.cc \
    --replace-fail 'std::isnan(' '__builtin_isnan('
  substituteInPlace poppler/Gfx.cc poppler/SplashOutputDev.cc poppler/CairoOutputDev.cc \
    --replace-fail 'std::isfinite(' '__builtin_isfinite('
''
