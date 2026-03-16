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
#include "extern_posix.h"

/*
 * __wt_has_priv --
 *     Return if the process has special privileges, defined as having different effective and read
 *     UIDs or GIDs.
 */
bool
__wt_has_priv(void)
{
    return (getuid() != geteuid() || getgid() != getegid());
}
