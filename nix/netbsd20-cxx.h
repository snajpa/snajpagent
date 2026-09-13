/* SPDX-License-Identifier: GPL-2.0-only */
/* The SDK's C type macro would redeclare the native C++ wchar_t keyword. */
#ifdef __cplusplus
#include <machine/ansi.h>
#undef _BSD_WCHAR_T_
#endif
