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

from helper import copy_wiredtiger_home
import wiredtiger, wttest
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_hs01.py
# Test that update and modify operations are durable across crash and recovery.
# Additionally test that checkpoint inserts content into the history store.
class test_hs01(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=200MB,statistics=(all)'
    format_values = [
        ('column', dict(key_format='r')),
        # ('row_integer', dict(key_format='i')),
        # ('row_string', dict(key_format='S'))
    ]
    value_format='u'
    scenarios = make_scenarios(format_values)

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def large_updates(self, session, uri, value, ds, nrows, timestamp=False):
        cursor = session.open_cursor(uri)
        for i in range(1, nrows):
            if timestamp == True:
                session.begin_transaction()
            cursor.set_key(ds.key(i))
            cursor.set_value(value)
            self.assertEqual(cursor.update(), 0)
            if timestamp == True:
                session.commit_transaction('commit_timestamp=' + self.timestamp_str(i + 1))
        cursor.close()

    def large_modifies(self, session, uri, offset, ds, nrows, timestamp=False):
        cursor = session.open_cursor(uri)
        for i in range(1, nrows):
            # Unlike inserts and updates, modify operations do not implicitly start/commit a transaction.
            # Hence, we begin/commit transaction manually.
            session.begin_transaction()
            cursor.set_key(ds.key(i))

            mods = []
            mod = wiredtiger.Modify('A', offset, 1)
            mods.append(mod)
            self.assertEqual(cursor.modify(mods), 0)

            if timestamp == True:
                session.commit_transaction('commit_timestamp=' + self.timestamp_str(i + 1))
            else:
                session.commit_transaction()
        cursor.close()

    def durable_check(self, check_value, uri, ds):
        # Simulating recovery.
        newdir = "BACKUP"
        copy_wiredtiger_home(self, '.', newdir, True)
        conn = self.setUpConnectionOpen(newdir)
        session = self.setUpSessionOpen(conn)
        cursor = session.open_cursor(uri, None)

        cursor.next()
        self.assertTrue(check_value == cursor.get_value(),
            "for key " + str(1) + ", expected " + str(check_value) +
            ", got " + str(cursor.get_value()))
        cursor.close()
        session.close()
        conn.close()

    def test_hs(self):
        # "checkpoint=(WiredTigerCheckpoint.1=(run_write_gen=20247))lcheckpoint_backup_info=,"
        # x = "access_pattern_hint=none,allocation_size=4KB,app_metadata=(formatVersion=8),assert=(write_timestamp=off),block_allocation=best," + "block_compressor=,cache_resident=false,checkpoint=(WiredTigerCheckpoint.1=(addr=\"018181e49834da0d8281e41546bd168381e4fc20a8a5808080e22fc0cfc0\",order=1,time=1768917027,size=8192,newest_start_durable_ts=0,oldest_start_ts=0,newest_txn=0,newest_stop_durable_ts=0,newest_stop_ts=-1,newest_stop_txn=-11,prepare=0,write_gen=20249,run_write_gen=20247))lcheckpoint_backup_info=,checkpoint_lsn=(2,58901120),checksum=onlcollator=,columns=,dictionary=0lencryption=(keyid=,name=),format=btree,huffman_key=,huffman_value=,id=24034,ignore_in_memory_cache_size=false,internal_item_max=0,internal_key_max=0,internal_key_truncate=true,internal_page_max=16k,key_format=u,key_gap=10,leaf_item_max=0,leaf_key_max=0,leaf_page_max=16k,leaf_value_max=0llog=(enabled=false),memory_page_image_max=0,memory_page_max=5MB,os_cache_dirty_max=0,os_cache_max=0,prefix_compression=true,prefix_compression_min=4,readonly=false,split_deepen_min_child=0,split_deepen_per_child=0,split_pct=90,tiered_object=false,tiered_storage=(auth_token=,bucket=,bucket_prefix=,cache_directory=,local_retention=300,name=,object_target_size=0),value_format=u,verbose=[write_timestamp],version=(major=1,minor=1),write_timestamp_usage=none";
        # for i in range(len(x)):
        #     if i <= 480 and i >= 460:
        #         self.prout(x[i])
        #     if i == 471:
        #         self.prout("hahah: " + x[i])
        # return
        # Create a small table.
        uri = "table:test_hs01"
        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        bigvalue = b"aaaaa" * 100
        bigvalue2 = b"ccccc" * 100
        bigvalue3 = b"ccccc" * 100
        bigvalue3 = b'AA' + bigvalue3[2:]
        bigvalue4 = b"ddddd" * 100

        # Initially insert a lot of data.
        nrows = 10000
        cursor = self.session.open_cursor(uri)
        for i in range(1, nrows):
            cursor.set_key(ds.key(i))
            cursor.set_value(bigvalue)
            self.assertEqual(cursor.insert(), 0)
        cursor.close()
        self.session.checkpoint()

        # Scenario: 1
        # Check to see if the history store is working with the old reader.
        # Open session 2.
        session2 = self.conn.open_session()
        session2.begin_transaction()
        # Large updates with session 1.
        self.large_updates(self.session, uri, bigvalue2, ds, nrows)

        # Checkpoint and then assert that the (nrows-1) insertions were moved to history store from data store.
        self.session.checkpoint()
        hs_writes = self.get_stat(stat.conn.cache_hs_insert)
        cache_hs_key_processed = self.get_stat(stat.conn.cache_hs_key_processed)
        cache_hs_update_processed = self.get_stat(stat.conn.cache_hs_update_processed)
        self.assertEqual(hs_writes, nrows-1)
        self.assertEqual(cache_hs_key_processed, nrows - 1)
        self.assertEqual(cache_hs_update_processed, nrows - 1)

        # Check to see the latest updated value after recovery.
        self.durable_check(bigvalue2, uri, ds)
        session2.rollback_transaction()
        session2.close()

        # Scenario: 2
        # Check to see the history store working with modify operations.
        # Open session 2.
        session2 = self.conn.open_session()
        session2.begin_transaction()
        # Apply two modify operations (session1)- replacing the first two letters with 'A'.
        self.large_modifies(self.session, uri, 0, ds, nrows)
        self.large_modifies(self.session, uri, 1, ds, nrows)

        # Checkpoint and then assert if updates (nrows-1) and first large modifies (nrows-1) were moved to history store.
        self.session.checkpoint()
        hs_writes = self.get_stat(stat.conn.cache_hs_insert)
        # The updates in data store: nrows-1
        # The first modifies in cache: nrows-1
        # The stats was already set at: nrows-1 (previous hs stats)
        # Total: (nrows-1)*3
        self.assertEqual(hs_writes, (nrows-1) * 3)

        # Check to see the modified value after recovery.
        self.durable_check(bigvalue3, uri, ds)
        session2.rollback_transaction()
        session2.close()

        # Scenario: 3
        # Check to see if the history store is working with the old timestamp.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.large_updates(self.session, uri, bigvalue4, ds, nrows, timestamp=True)

        self.session.checkpoint()
        # Check if the (nrows-1) modifications were moved to history store from data store.
        # The stats was already set at: (nrows-1)*3 (previous hs stats)
        # Total: (nrows-1)*4
        hs_writes = self.get_stat(stat.conn.cache_hs_insert)
        self.assertEqual(hs_writes, (nrows-1) * 4)

        # Check to see data can be see only till the stable_timestamp.
        self.durable_check(bigvalue3, uri, ds)

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(i + 1))
        # No need to check history store stats here as nothing will be moved.
        self.session.checkpoint()
        # Check that the latest data can be seen.
        self.durable_check(bigvalue4, uri, ds)
