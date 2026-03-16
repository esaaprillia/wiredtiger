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
 * wiredtiger_version --
 *     Return library version information.
 */
const char *
wiredtiger_version(int *majorp, int *minorp, int *patchp)
{
    if (majorp != NULL)
        *majorp = WIREDTIGER_VERSION_MAJOR;
    if (minorp != NULL)
        *minorp = WIREDTIGER_VERSION_MINOR;
    if (patchp != NULL)
        *patchp = WIREDTIGER_VERSION_PATCH;
    return (WIREDTIGER_VERSION_STRING);
}
