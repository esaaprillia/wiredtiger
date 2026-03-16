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
#include "misc.h"
#include "stat.h"
#include "btmem.h"
#include "session.h"
#include "connection.h"
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
 * __wt_session_prefetch_check --
 *     Check if pre-fetching work should be performed for a given ref.
 */
bool
__wt_session_prefetch_check(WT_SESSION_IMPL *session, WT_REF *ref)
{
    /*
     * Check if pre-fetching is enabled for this particular session. We don't perform pre-fetching
     * on internal threads or internal pages (finding the right content to preload based on internal
     * pages is hard), so check for that too. We also want to pre-fetch sessions that have read at
     * least one page from disk. The result of this function will subsequently be checked by cursor
     * logic to determine if pre-fetching will be performed.
     */
    if (!F_ISSET(session, WT_SESSION_PREFETCH_ENABLED)) {
        WT_STAT_CONN_INCR(session, prefetch_skipped);
        return (false);
    }

    /* Disable pre-fetch work on tiered tables. */
    if (__wt_atomic_load_enum_relaxed(&session->dhandle->type) == WT_DHANDLE_TYPE_TIERED ||
      __wt_atomic_load_enum_relaxed(&session->dhandle->type) == WT_DHANDLE_TYPE_TIERED_TREE)
        return (false);

    if (S2C(session)->prefetch_queue_count > WT_MAX_PREFETCH_QUEUE)
        return (false);

    if (F_ISSET(session, WT_SESSION_INTERNAL)) {
        WT_STAT_CONN_INCR(session, prefetch_skipped_internal_session);
        WT_STAT_CONN_INCR(session, prefetch_skipped);
        return (false);
    }

    if (F_ISSET(ref, WT_REF_FLAG_INTERNAL)) {
        WT_STAT_CONN_INCR(session, prefetch_skipped_internal_page);
        WT_STAT_CONN_INCR(session, prefetch_skipped);
        return (false);
    }

    if (F_ISSET(S2BT(session), WT_BTREE_SPECIAL_FLAGS) &&
      !F_ISSET(S2BT(session), WT_BTREE_VERIFY)) {
        WT_STAT_CONN_INCR(session, prefetch_skipped_special_handle);
        WT_STAT_CONN_INCR(session, prefetch_skipped);
        return (false);
    }

    if (session->pf.prefetch_disk_read_count == 1)
        WT_STAT_CONN_INCR(session, prefetch_disk_one);

    if (session->pf.prefetch_disk_read_count < 2) {
        WT_STAT_CONN_INCR(session, prefetch_skipped_disk_read_count);
        WT_STAT_CONN_INCR(session, prefetch_skipped);
        return (false);
    }

    WT_STAT_CONN_INCR(session, prefetch_attempts);

    return (true);
}
