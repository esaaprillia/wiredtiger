#!/usr/bin/env python3
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#
# [TEST_TAGS]
# checkpoint:checkpoint_cleanup
# [END_TAGS]

import time, threading, wttest
from test_cc01 import test_cc_base
from compact_util import compact_util
from wiredtiger import stat
from wtscenario import make_scenarios

# test_cc12_wt16587.py
#
# Regression test for WT-16587:
# "Assert fires in __curhs_btree_id_to_hs_id when checkpoint-cleanup or
#  background-compaction iterates the metadata file."
#
# --- Bug description ---
#
# When checkpoint-cleanup or background-compaction iterates the metadata btree
# (file:WiredTiger.wt) to find the next eligible table, they call
# cursor->next() on a raw metadata cursor without using
# WT_ISO_READ_UNCOMMITTED.  Under snapshot isolation the cursor reads the
# on-disk image of a metadata entry whose start_txn is *newer* than the
# cleanup session's snapshot.  Because the entry is not visible,
# __wt_txn_read() falls through to the history-store lookup path:
#
#   if (!F_ISSET(S2BT(session), WT_BTREE_IN_MEMORY) &&
#       F_ISSET_ATOMIC_32(S2C(session), WT_CONN_HS_OPEN) &&
#       !F_ISSET(session->dhandle, WT_DHANDLE_HS))        /* <-- missing metadata guard */
#       __wt_hs_find_upd(...)
#
# __wt_hs_find_upd() calls __wt_curhs_open(session, btree_id=0, ...) where
# btree_id=0 is the metadata btree's reserved ID.
# __curhs_btree_id_to_hs_id() immediately fires:
#   WT_ASSERT(session, btree_id != 0);
# causing a SIGABRT.
#
# --- Required conditions ---
#
#   1. A metadata entry whose start_txn > the cleanup session's snap_max
#      (i.e. written *after* the cleanup session took its snapshot).
#   2. That metadata page has been evicted to disk (dsk != NULL, empty
#      in-memory update chain).
#   3. The cleanup session calls cursor->next() under snapshot isolation
#      and encounters that entry.
#
# --- Two complementary fixes ---
#
# Fix 1 (src/include/txn_inline.h):
#   Added !WT_IS_METADATA(session->dhandle) to the HS-lookup guard so the
#   metadata btree never reaches __wt_hs_find_upd.
#
# Fix 2 (src/btree/bt_sync_obsolete.c, src/conn/conn_compact.c):
#   Wrapped all cursor->next() / cursor->search_near() calls in
#   __checkpoint_cleanup_get_uri and __background_compact_find_next_uri
#   with WT_WITH_TXN_ISOLATION(session, WT_ISO_READ_UNCOMMITTED, ...).
#   Under WT_ISO_READ_UNCOMMITTED all metadata entries are unconditionally
#   visible, so the HS path is never reached.
#
# --- Test strategy ---
#
# The test creates the race condition by:
#   - Holding an old snapshot transaction on a dedicated session while
#     many new metadata entries are written to the metadata btree.
#   - Triggering checkpoint to flush those entries to disk (clearing the
#     in-memory update chain so the on-disk image is the only copy).
#   - Iterating file:WiredTiger.wt directly from the snapshot session
#     to hit the path that would crash without the fixes.
#
# With both fixes applied the test must complete without any crash.
# Without Fix 1 and Fix 2, on Linux the test reliably triggers SIGABRT.
# (On macOS the crash may not be observed because the metadata page's
# disk-image pointer can remain NULL in certain memory configurations,
# causing an earlier return in __wt_txn_read before the HS guard.)
#
@wttest.skip_for_hook("tiered", "Checkpoint cleanup does not support tiered tables")
class test_cc12_wt16587(compact_util, test_cc_base):
    """
    Regression test for WT-16587.

    Exercises both the checkpoint-cleanup path and the background-compaction
    path — each is an independent crash site.
    """

    trigger_variants = [
        ('checkpoint_cleanup', dict(use_bg_compact=False)),
        ('background_compact', dict(use_bg_compact=True)),
    ]
    scenarios = make_scenarios(trigger_variants)

    conn_config = (
        'cache_size=100MB,'
        'statistics=(all),'
        'checkpoint_cleanup=[method=reclaim_space]'
    )

    num_initial_tables = 10
    num_late_tables = 100   # written AFTER snapshot is taken
    nrows = 1000

    # ------------------------------------------------------------------ helpers

    def _populate_table(self, uri, ts):
        """Create a table and write nrows rows at the given commit timestamp."""
        self.session.create(uri, 'key_format=i,value_format=S')
        c = self.session.open_cursor(uri, None)
        for i in range(self.nrows):
            self.session.begin_transaction()
            c[i] = 'x' * 128
            self.session.commit_transaction(
                'commit_timestamp=' + self.timestamp_str(ts))
        c.close()

    def _scan_metadata_with_snapshot(self, snap_session):
        """
        Iterate file:WiredTiger.wt from snap_session (which holds a stale
        snapshot).

        Without Fix 1: entries whose start_txn > snap_max reach the HS
        guard with btree_id=0 → WT_ASSERT fires → SIGABRT.
        Without Fix 2: checkpoint-cleanup's own cursor->next() hits the
        same path.

        With both fixes applied this call completes cleanly.
        """
        cur = snap_session.open_cursor('file:WiredTiger.wt', None, None)
        count = 0
        ret = cur.next()
        while ret == 0:
            count += 1
            ret = cur.next()
        cur.close()
        return count

    # ------------------------------------------------------------------ test

    def test_cc12_wt16587(self):
        """
        Reproduce the WT-16587 race condition and verify the fixes prevent
        the assertion in __curhs_btree_id_to_hs_id.
        """

        # ---- Phase 1: build baseline state --------------------------------
        base_ts = 10
        self.conn.set_timestamp(
            f'oldest_timestamp={self.timestamp_str(1)},'
            f'stable_timestamp={self.timestamp_str(1)}'
        )

        for i in range(self.num_initial_tables):
            self._populate_table(f'table:cc12_base{i}', base_ts + i)

        self.conn.set_timestamp(
            f'stable_timestamp={self.timestamp_str(base_ts + self.num_initial_tables)}'
        )
        self.session.checkpoint()

        # ---- Phase 2: take a snapshot BEFORE new metadata entries ---------
        # snap_session's snapshot (snap_max) is fixed at begin_transaction.
        # Any metadata entry written after this point has start_txn > snap_max
        # and will not be visible under this snapshot.
        snap_session = self.conn.open_session()
        snap_session.begin_transaction('isolation=snapshot')

        # ---- Phase 3: write many new metadata entries AFTER snapshot ------
        # Each create() inserts a row into file:WiredTiger.wt with the
        # schema session's current transaction ID, which is > snap_max.
        new_ts_base = base_ts + self.num_initial_tables + 1
        for i in range(self.num_late_tables):
            self._populate_table(f'table:cc12_late{i}', new_ts_base + i)

        # ---- Phase 4: checkpoint to flush metadata to disk ----------------
        # After reconciliation the metadata page's update chain is cleared.
        # The on-disk image now contains the new entries with their original
        # start_txn values (> snap_max of snap_session).
        self.conn.set_timestamp(
            f'stable_timestamp={self.timestamp_str(new_ts_base + self.num_late_tables)}'
        )
        self.session.checkpoint()

        # ---- Phase 5: trigger the crash path ------------------------------
        if self.use_bg_compact:
            # Background-compaction path:
            # __background_compact_find_next_uri iterates the metadata cursor
            # under snapshot isolation (before Fix 2).
            self.turn_on_bg_compact('free_space_target=1MB')
            time.sleep(1)
            self.turn_off_bg_compact()
        else:
            # Checkpoint-cleanup path:
            # __checkpoint_cleanup_get_uri iterates the metadata cursor under
            # snapshot isolation (before Fix 2).
            self.wait_for_cc_to_run()

        # ---- Phase 6: direct simulation of the crash path -----------------
        # Simulate what the buggy code does: iterate file:WiredTiger.wt from
        # a session holding an old snapshot.  Without Fix 1 this would reach
        # __wt_hs_find_upd(btree_id=0) and crash.
        entries = self._scan_metadata_with_snapshot(snap_session)
        snap_session.rollback_transaction()
        snap_session.close()

        self.assertGreater(entries, 0,
            'Expected at least one visible metadata entry')

        # If we reach here without SIGABRT, the fixes are working.

if __name__ == '__main__':
    wttest.run()
