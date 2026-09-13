/* SPDX-License-Identifier: GPL-2.0-only */
/* The SDK's C type macro would redeclare the native C++ wchar_t keyword. */
#ifdef __cplusplus
#ifdef __NetBSD__
#include <machine/ansi.h>
#undef _BSD_WCHAR_T_
#endif
#ifdef __clang__
/* The old math header uses GCC's type-specific predicate spellings. */
#define __builtin_isnanf __builtin_isnan
#define __builtin_isnanl __builtin_isnan
#if defined(__OpenBSD__) && !defined(FP_NAN)
/* This SDK has no C99 category labels. The builtin receives these labels. */
#define FP_INFINITE 0
#define FP_NAN 1
#define FP_NORMAL 2
#define FP_SUBNORMAL 3
#define FP_ZERO 4
#endif
#endif
#endif
