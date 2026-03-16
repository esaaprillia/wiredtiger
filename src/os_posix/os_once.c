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
 * __wt_once --
 *     One-time initialization per process.
 */
int
__wt_once(void (*init_routine)(void))
{
    static pthread_once_t once_control = PTHREAD_ONCE_INIT;

    return (pthread_once(&once_control, init_routine));
}
