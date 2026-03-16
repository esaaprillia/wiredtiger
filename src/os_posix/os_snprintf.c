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
#include "error.h"
#include "extern_noninline.h"
#include "extern_posix.h"

/*
 * __wt_vsnprintf_len_incr --
 *     POSIX vsnprintf convenience function, incrementing the returned size.
 */
int
__wt_vsnprintf_len_incr(char *buf, size_t size, size_t *retsizep, const char *fmt, va_list ap)
  WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    WT_DECL_RET;

    if ((ret = vsnprintf(buf, size, fmt, ap)) >= 0) {
        *retsizep += (size_t)ret;
        return (0);
    }
    return (__wt_errno());
}
