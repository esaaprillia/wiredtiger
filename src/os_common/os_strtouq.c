/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wiredtiger_config.h"
#include "wiredtiger_ext.h"
#include "wt_system.h"
#include "wt_compiler.h"
#include "wt_fwd.h"
#include "extern_noninline.h"
#ifdef _WIN32
#include "extern_win.h"
#else
#include "extern_posix.h"
#ifdef __linux__
#include "extern_linux.h"
#elif __APPLE__
#include "extern_darwin.h"
#endif
#endif

/*
 * __wt_strtouq --
 *     Convert a string to an unsigned long integer. Effectively uses `strtoull`.
 */
uint64_t
__wt_strtouq(const char *nptr, char **endptr, int base)
  WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    static_assert(
      sizeof(uint64_t) == sizeof(unsigned long long), "unsigned long long is not 64 bytes");

    return (strtoull(nptr, endptr, base));
}
