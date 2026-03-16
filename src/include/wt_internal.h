/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 * 	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#ifndef __WT_INTERNAL_H
#define __WT_INTERNAL_H

#if defined(__cplusplus)
extern "C" {
#endif

/*******************************************
 * WiredTiger public include file, and configuration control.
 *******************************************/
#include "wiredtiger_config.h"
#include "wiredtiger_ext.h"

/*******************************************
 * WiredTiger system include files.
 *******************************************/
#include "wt_system.h"

/*******************************************
 * Forward type declarations for all internal types.
 *******************************************/
#include "wt_fwd.h"

/*******************************************
 * Compiler detection and attributes.
 *******************************************/
#include "wt_compiler.h"

#include "hardware.h"
#include "swap.h"

#include "queue.h"

#ifdef _WIN32
#include "os_windows.h"
#else
#include "posix.h"
#endif

#include "misc.h"
#include "tsan_suppress.h"
#include "mutex.h"

#include "stat.h"         /* required by dhandle.h */
#include "dhandle.h"      /* required by btree.h, connection.h */
#include "timestamp.h"    /* required by reconcile.h */
#include "thread_group.h" /* required by rollback_to_stable.h */
#include "verbose.h"      /* required by rollback_to_stable.h */
#include "error.h"        /* required by api.h */

#include "api.h"
#include "bitstring.h"
#include "block.h"
#include "block_cache.h"
#include "block_chunkcache.h"
#include "btmem.h"
#include "btree.h"
#include "cache.h"
#include "../evict/evict.h"
#include "capacity.h"
#include "cell.h"
#include "cursor.h" /* required by checkpoint */
#include "../checkpoint/checkpoint.h"
#include "compact.h"
#include "conf_keys.h" /* required by conf.h */
#include "conf.h"
#include "config.h"
#include "crypt_header.h"
#include "dlh.h"
#include "futex.h"
#include "generation.h"
#include "hash_map.h"
#include "hazard.h"
#include "json.h"
#include "../live_restore/live_restore.h"
#include "../log/log.h"
#include "meta.h" /* required by block.h */
#include "optrack.h"
#include "os.h"
#include "../reconcile/reconcile.h"
#include "rollback_to_stable.h"
#include "schema.h"
#include "tiered.h"
#include "truncate.h"
#include "txn.h"

#include "session.h" /* required by connection.h */
#include "version.h" /* required by connection.h */
#include "connection.h"

#include "extern.h"
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
#include "verify_build.h"

#include "cache_inline.h"
#include "../evict/evict_inline.h" /* required by misc_inline.h */
#include "ctype_inline.h"          /* required by packing_inline.h */
#include "intpack_inline.h"        /* required by cell_inline.h, packing_inline.h */
#include "int4bitpack_inline.h"
#include "misc_inline.h" /* required by mutex_inline.h */

#include "generation_inline.h" /* required by txn_inline.h */
#include "buf_inline.h"        /* required by cell_inline.h */
#include "ref_inline.h"        /* required by btree_inline.h */
#include "timestamp_inline.h"  /* required by btree_inline.h */
#include "cell_inline.h"       /* required by btree_inline.h */
#include "mutex_inline.h"      /* required by btree_inline.h */
#include "txn_inline.h"        /* required by btree_inline.h */

#include "bitstring_inline.h"
#include "block_inline.h"
#include "btree_inline.h" /* required by cursor_inline.h */
#include "btree_cmp_inline.h"
#include "column_inline.h"
#include "conf_inline.h"
#include "cursor_inline.h"
#include "../log/log_inline.h"
#include "modify_inline.h"
#include "os_fhandle_inline.h"
#include "os_fs_inline.h"
#include "os_fstream_inline.h"
#include "packing_inline.h"
#include "serial_inline.h"
#include "str_inline.h"
#include "time_inline.h"

#if defined(__cplusplus)
}
#endif
#endif /* !__WT_INTERNAL_H */
