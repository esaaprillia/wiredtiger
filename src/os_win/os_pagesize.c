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
#include "extern_win.h"

/*
 * __wt_get_vm_pagesize --
 *     Return the default page size of a virtual memory page.
 */
int
__wt_get_vm_pagesize(void)
{
    SYSTEM_INFO system_info;

    GetSystemInfo(&system_info);

    return (system_info.dwPageSize);
}
