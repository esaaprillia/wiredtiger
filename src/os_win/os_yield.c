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
#include "hardware.h"
#include "extern_noninline.h"
#include "extern_win.h"

/*
 * __wt_yield --
 *     Yield the thread of control.
 */
void
__wt_yield(void)
{
    /*
     * Yielding the processor isn't documented as a memory barrier, and it's a reasonable
     * expectation to have. There's no reason not to explicitly include a barrier since we're giving
     * up the CPU, and ensures callers aren't ever surprised.
     */
    WT_FULL_BARRIER();

    SwitchToThread();
}
