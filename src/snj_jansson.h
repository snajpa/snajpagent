/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_JANSSON_H
#define SNAJPAGENT_JANSSON_H

/*
 * Prefer a system Jansson development header.  Some minimal build roots used
 * for source qualification carry the runtime library without jansson.h; in
 * that case fall back to the first-party ABI declaration shim.  The shim is
 * declarations only and is inventoried by make depscheck so it cannot masquerade
 * as a vendored Jansson implementation or shadow a system <jansson.h>.
 */
#if defined(SNAJPAGENT_FORCE_LOCAL_JANSSON_ABI)
#include "snj_jansson_abi.h"
#elif defined(__has_include)
#if __has_include(<jansson.h>)
#include <jansson.h>
#else
#include "snj_jansson_abi.h"
#endif
#else
#include <jansson.h>
#endif

#endif
