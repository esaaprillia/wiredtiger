#!/usr/bin/env python
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
# of the public at large and to the use or other dealings in the software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

# test_hs_metadata_lookup.py
#
# Regression test for WT-16587:
#   Assert fired in __curhs_btree_id_to_hs_id (btree_id != 0)
#
# Root cause: background compaction (__background_compact_find_next_uri) and
# checkpoint-cleanup (__checkpoint_cleanup_get_uri) iterate the metadata file
# via cursor->next() without WT_ISO_READ_UNCOMMITTED. Under snapshot isolation,
# a recently-written metadata entry whose on-disk start_txn is not yet visible
# in the session's snapshot causes __wt_txn_read to fall through to the history
# store lookup path. Since the metadata btree has id=0 (reserved, never has HS
# entries), __wt_curhs_open hits the assertion btree_id != 0 and aborts.
#
# Fix: two changes were made:
#   1. txn_inline.h: add !WT_IS_METADATA(session->dhandle) to the HS lookup
#      guard — metadata never has HS entries so HS lookup is always wrong.
#   2. bt_sync_obsolete.c / conn_compact.c: wrap all raw metadata cursor reads
#      (search_near, next) with WT_WITH_TXN_ISOLATION(WT_ISO_READ_UNCOMMITTED).
#
# The test creates the conditions that trigger the race:
#   1. Many tables → large metadata file, many entries updated per checkpoint
#   2. Timestamps → checkpoint writes metadata with fresh txn IDs each pass
#   3. Tight cache → forces metadata page eviction to disk
#   4. Background compaction → iterates metadata cursor to find eligible tables
#   5. Checkpoint-cleanup (interval=1s) → iterates metadata cursor the same way
#   6. Rapid checkpoint thread → keeps generating new metadata txn IDs
#   7. Long-running transaction pins oldest_id, widening the window where a
#      checkpoint txn ID is committed but not globally visible
#
# Without the fix the process aborts with SIGABRT inside __curhs_btree_id_to_hs_id.
# With the fix the test completes cleanly.
#
# Note: the race window is narrow on macOS. The test is most effective on
# multi-core Linux where the checkpoint and cleanup threads truly run in
# parallel.

import threading, time, wttest
from compact_util import compact_util
from wtthread import checkpoint_thread


class test_hs_metadata_lookup(compact_util):
    conn_config = (
        'cache_size=50MB,'
        'statistics=(all),'
        'checkpoint_cleanup=(wait=1,file_wait_ms=0),'
        'transaction_sync=(enabled=false)'
    )

    n_tables = 20
    n_rows   = 5000

    uri_prefix = 'table:test_hs_metadata_lookup'
    create_params = 'key_format=i,value_format=S'

    def test_hs_metadata_lookup_bg_compact(self):
        """
        Test that background compaction iterating the metadata cursor
        does not trigger a history store lookup on the metadata btree
        (WT-16587 / __background_compact_find_next_uri path).
        """
        if self.runningHook('tiered'):
            self.skipTest('Tiered tables do not support compaction')

        self._run_workload(enable_bg_compact=True)

    def test_hs_metadata_lookup_checkpoint_cleanup(self):
        """
        Test that checkpoint-cleanup iterating the metadata cursor does
        not trigger a history store lookup on the metadata btree
        (WT-16587 / __checkpoint_cleanup_get_uri path).
        """
        if self.runningHook('tiered'):
            self.skipTest('Tiered tables do not support compaction')

        self._run_workload(enable_bg_compact=False)

    def _run_workload(self, enable_bg_compact):
        # Phase 1: create and populate tables with timestamps.
        uris = []
        for i in range(self.n_tables):
            uri = f'{self.uri_prefix}_{i}'
            uris.append(uri)
            self.session.create(uri, self.create_params)

        ts = 1
        for uri in uris:
            c = self.session.open_cursor(uri)
            for k in range(self.n_rows):
                self.session.begin_transaction()
                c[k] = 'x' * 128
                self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(ts)}')
                ts += 1
            c.close()

        self.conn.set_timestamp(
            f'stable_timestamp={self.timestamp_str(ts - 1)},'
            f'oldest_timestamp={self.timestamp_str(1)}'
        )
        self.session.checkpoint()

        # Phase 2: delete most rows so background compact sees eligible tables.
        for uri in uris:
            c = self.session.open_cursor(uri)
            for k in range(self.n_rows * 4 // 5):
                self.session.begin_transaction()
                c.set_key(k)
                c.remove()
                self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(ts)}')
                ts += 1
            c.close()

        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(ts - 1)}')
        self.session.checkpoint()

        # Phase 3: run concurrent workload.
        done = threading.Event()
        ckpt = checkpoint_thread(self.conn, done)
        ckpt.start()

        # Long-transaction thread: pins oldest_id, widening the window where
        # T_ckpt is committed but not globally visible.
        def _long_txn_worker():
            sess = self.conn.open_session()
            c = sess.open_cursor(uris[0])
            while not done.is_set():
                sess.begin_transaction('isolation=snapshot')
                c.set_key(self.n_rows - 1)
                c.search()
                time.sleep(0.2)
                sess.rollback_transaction()
            c.close()
            sess.close()

        long_txn = threading.Thread(target=_long_txn_worker, daemon=True)
        long_txn.start()

        if enable_bg_compact:
            self.turn_on_bg_compact('free_space_target=1MB')

        try:
            time.sleep(30)
        finally:
            done.set()
            ckpt.join()
            long_txn.join(timeout=2)
            if enable_bg_compact:
                self.turn_off_bg_compact()
