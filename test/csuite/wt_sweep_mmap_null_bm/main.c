/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Regression test for a NULL dereference in __wt_page_out() when the sweep
 * server discards pages from a btree whose block manager (bm) has already
 * been set to NULL by a prior __wt_btree_close() call.
 *
 * Root cause
 * ----------
 * GDB analysis of core dumps from the kek_initialization_with_kmip MongoDB
 * JS test on aarch64 Linux (Amazon Linux 2023) revealed:
 *
 *   Crash site:  src/btree/bt_discard.c:125
 *     (void)S2BT(session)->bm->map_discard(...)
 *   S2BT(session)->bm == 0x0  (NULL)
 *   page->flags_atomic == 0x9  (WT_PAGE_BUILD_KEYS | WT_PAGE_DISK_MAPPED)
 *   btree->flags       == 0x6000  (WT_BTREE_CLOSED | WT_BTREE_DISAGGREGATED)
 *   dhandle->name      == "file:WiredTigerShared.wt_stable"
 *
 * The crash occurs when the sweep server closes a dead dhandle in two passes:
 *
 *   Pass 1 - __sweep_expire_one
 *     -> __wt_conn_dhandle_close(mark_dead=true)
 *     -> For a disaggregated btree: bm->is_mapped() returns false
 *        (disagg block manager always returns false from is_mapped()),
 *        so the handle is marked WT_DHANDLE_DEAD and __wt_btree_close()
 *        is called, setting btree->bm = NULL.
 *
 *   Pass 2 - __sweep_discard_trees
 *     -> __wt_conn_dhandle_close(mark_dead=false, discard=true)
 *     -> bm is now NULL, is_mapped() check is skipped
 *     -> __wt_evict_file(WT_SYNC_DISCARD)
 *        -> __wt_ref_out() -> __wt_page_out()
 *           -> WT_PAGE_DISK_MAPPED is set on cached pages
 *           -> S2BT(session)->bm->map_discard(...)  <- NULL deref, SIGSEGV
 *
 * WT_PAGE_DISK_MAPPED is set when mmap=true and pages are read through a
 * checkpoint (read-only) cursor: __bm_checkpoint_load sets bm->map via
 * __wti_blkcache_map(), and __wti_blkcache_map_read() sets WT_PAGE_DISK_MAPPED
 * on each page loaded from the mmap region.
 *
 * How this test reproduces the bug
 * ---------------------------------
 * The exact production scenario requires MongoDB's disaggregated storage
 * stack. This test replicates the essential kernel of the bug by directly
 * manipulating WiredTiger internals:
 *
 *   1. Open connection with mmap=true.
 *   2. Write rows and checkpoint.
 *   3. Open a checkpoint cursor and read all rows, populating the cache
 *      with WT_PAGE_DISK_MAPPED pages.
 *   4. Simulate Pass 1 of sweep: null out btree->bm (mimicking the effect
 *      of __wt_btree_close() after the handle is marked dead on a btree
 *      where bm->is_mapped() returns false).
 *   5. Call __wt_evict_file(WT_SYNC_DISCARD) directly, which walks the
 *      in-cache pages and calls __wt_page_out() on each.
 *   6. Without the fix: __wt_page_out() dereferences the NULL bm pointer
 *      for any page with WT_PAGE_DISK_MAPPED -> SIGSEGV.
 *      With the fix: the NULL check prevents the dereference -> clean exit.
 *
 * Fix
 * ---
 * In __wt_page_out() (src/btree/bt_discard.c):
 *
 *     if (F_ISSET_ATOMIC_16(page, WT_PAGE_DISK_MAPPED)) {
 *         WT_BM *bm = S2BT(session)->bm;
 *         if (bm != NULL)
 *             (void)bm->map_discard(bm, session, dsk, (size_t)dsk->mem_size);
 *     }
 *
 * Safe: bm->close() in Pass 1 already released the mmap region; there
 * is nothing left to unmap in Pass 2.
 */

#include "wt_internal.h"
#include "test_util.h"

#define TABLE_URI "table:sweep_mmap_test"
#define CHECKPOINT_NAME "WiredTigerCheckpoint"

/* Enough rows to populate several leaf pages and keep them in cache. */
#define NUM_ROWS 5000
#define VALUE_LEN 128

/*
 * mmap=true           - enables whole-file mapping for checkpoint handles (WT_PAGE_DISK_MAPPED)
 * eviction_trigger=99 - suppress background eviction so pages stay mapped in cache
 * cache_size=500MB    - large enough that eviction is not needed during the test
 */
#define CONN_CONFIG "create,mmap=true,eviction_trigger=99,eviction_target=95,cache_size=500MB"

int
main(int argc, char *argv[])
{
    WT_BM *saved_bm;
    WT_BTREE *btree;
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_DATA_HANDLE *dhandle;
    WT_SESSION *pub_session;
    WT_SESSION_IMPL *session;
    char home[512];
    char key_buf[32];
    char value_buf[VALUE_LEN + 1];
    uint64_t i;

    (void)argc;
    (void)argv;

    testutil_work_dir_from_path(home, sizeof(home), "WT_TEST_sweep_mmap_null_bm");
    testutil_recreate_dir(home);

    printf("Opening connection (mmap=true, eviction suppressed) ...\n");
    testutil_check(wiredtiger_open(home, NULL, CONN_CONFIG, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &pub_session));

    testutil_check(
      pub_session->create(pub_session, TABLE_URI, "key_format=S,value_format=S,leaf_page_max=4KB"));

    printf("Inserting %d rows ...\n", NUM_ROWS);
    testutil_check(pub_session->open_cursor(pub_session, TABLE_URI, NULL, NULL, &cursor));
    memset(value_buf, 'v', VALUE_LEN);
    value_buf[VALUE_LEN] = '\0';
    for (i = 0; i < NUM_ROWS; ++i) {
        testutil_snprintf(key_buf, sizeof(key_buf), "key%09" PRIu64, i);
        cursor->set_key(cursor, key_buf);
        cursor->set_value(cursor, value_buf);
        testutil_check(cursor->insert(cursor));
    }
    testutil_check(cursor->close(cursor));

    printf("Checkpointing ...\n");
    testutil_check(pub_session->checkpoint(pub_session, NULL));

    /*
     * Open a checkpoint cursor. This triggers __bm_checkpoint_load which calls
     * __wti_blkcache_map(), setting bm->map to the whole-file mmap region.
     * Subsequent reads via __wti_blkcache_map_read() set WT_PAGE_DISK_MAPPED
     * on each page brought into cache.
     */
    printf("Opening checkpoint cursor (loads WT_PAGE_DISK_MAPPED pages) ...\n");
    testutil_check(pub_session->open_cursor(
      pub_session, TABLE_URI, NULL, "checkpoint=" CHECKPOINT_NAME, &cursor));
    while (cursor->next(cursor) == 0)
        ;

    /*
     * Verify that the checkpoint cursor actually established a mmap region (bm->map != NULL).
     * If mmap is not supported on this filesystem, the map pointer stays NULL and no pages
     * will have WT_PAGE_DISK_MAPPED set, so the test is not applicable -- skip it.
     */
    {
        WT_SESSION_IMPL *si = (WT_SESSION_IMPL *)pub_session;
        WT_BTREE *ckpt_btree;
        WT_DATA_HANDLE *dh;
        WT_CONNECTION_IMPL *conn_impl = S2C(si);
        bool found_map = false;

        TAILQ_FOREACH (dh, &conn_impl->dhqh, q) {
            if (dh->checkpoint != NULL && strstr(dh->name, "sweep_mmap_test") != NULL) {
                ckpt_btree = (WT_BTREE *)dh->handle;
                if (ckpt_btree->bm != NULL && ckpt_btree->bm->map != NULL)
                    found_map = true;
                break;
            }
        }

        if (!found_map) {
            printf(
              "SKIP: checkpoint btree has no mmap region (bm->map == NULL);"
              " mmap may not be supported on this filesystem.\n");
            testutil_check(cursor->close(cursor));
            testutil_check(pub_session->close(pub_session, NULL));
            testutil_check(conn->close(conn, NULL));
            testutil_remove(home);
            return (EXIT_SUCCESS);
        }
        printf("  Confirmed: checkpoint btree has bm->map != NULL (mmap region active).\n");
    }

    /*
     * Get the internal session and the btree for the checkpoint dhandle so we
     * can manipulate internals directly.
     */
    session = (WT_SESSION_IMPL *)pub_session;

    /*
     * Acquire the checkpoint dhandle.  The cursor is still open on it, so the
     * dhandle is pinned.  Walk the open dhandles to find the checkpoint one.
     */
    dhandle = NULL;
    {
        WT_CONNECTION_IMPL *conn_impl = S2C(session);
        WT_DATA_HANDLE *dh;
        TAILQ_FOREACH (dh, &conn_impl->dhqh, q) {
            /* Checkpoint dhandles have a non-NULL checkpoint field. */
            if (dh->checkpoint != NULL && strstr(dh->name, "sweep_mmap_test") != NULL) {
                dhandle = dh;
                break;
            }
        }
    }

    if (dhandle == NULL) {
        fprintf(stderr, "ERROR: could not find checkpoint dhandle for %s\n", TABLE_URI);
        return (EXIT_FAILURE);
    }

    btree = (WT_BTREE *)dhandle->handle;
    printf("  Found checkpoint dhandle: %s\n", dhandle->name);
    printf("  btree->bm = %p (should be non-NULL)\n", (void *)btree->bm);

    if (btree->bm == NULL) {
        fprintf(stderr, "ERROR: btree->bm is already NULL before test setup\n");
        return (EXIT_FAILURE);
    }

    /*
     * Close the cursor so the dhandle's session_inuse drops to zero. We still
     * hold a reference via dhandle directly for the manipulation below.
     */
    testutil_check(cursor->close(cursor));

    /*
     * Simulate Pass 1 of the sweep server:
     *
     *   In the real crash scenario (disaggregated btree):
     *     __sweep_expire_one -> __wt_conn_dhandle_close(mark_dead=true)
     *       -> bm->is_mapped() returns false (disagg always returns false)
     *       -> __wt_btree_close() is called, which sets btree->bm = NULL
     *       -> handle flagged WT_DHANDLE_DEAD
     *
     *   Here we replicate the key effect: btree->bm = NULL with pages
     *   still in cache marked WT_PAGE_DISK_MAPPED.
     */
    printf("Simulating Pass 1: nulling btree->bm (mimics __wt_btree_close on a dead handle) ...\n");
    saved_bm = btree->bm;
    btree->bm = NULL;

    /*
     * Mark the dhandle dead so __wt_evict_file's assertion is satisfied.
     * (The assertion at evict_file.c checks that either btree->evict_disabled > 0
     * or WT_DHANDLE_DEAD is set when dhandle is still open.)
     */
    F_SET(dhandle, WT_DHANDLE_DEAD);

    /*
     * Simulate Pass 2 of the sweep server:
     *
     *   __sweep_discard_trees -> __wt_conn_dhandle_close(mark_dead=false)
     *     -> discard=true (because WT_DHANDLE_DEAD is set)
     *     -> bm is NULL, is_mapped check returns false
     *     -> __wt_evict_file(WT_SYNC_DISCARD)
     *        -> walks all pages in the btree cache
     *        -> __wt_ref_out() -> __wt_page_out()
     *           -> for pages with WT_PAGE_DISK_MAPPED:
     *              WITHOUT fix: S2BT(session)->bm->map_discard(...)  SIGSEGV
     *              WITH fix:    bm == NULL check prevents the call
     */
    printf(
      "Simulating Pass 2: calling __wt_evict_file(WT_SYNC_DISCARD) with bm=NULL ...\n"
      "  (Without fix: SIGSEGV here; with fix: clean return)\n");

    WT_WITH_DHANDLE(session, dhandle, testutil_check(__wt_evict_file_exclusive_on(session)));
    WT_WITH_DHANDLE(session, dhandle, testutil_check(__wt_evict_file(session, WT_SYNC_DISCARD)));
    WT_WITH_DHANDLE(session, dhandle, __wt_evict_file_exclusive_off(session));

    printf("  __wt_evict_file returned cleanly -- bm=NULL was handled correctly.\n");

    /* Restore bm so the dhandle can be properly cleaned up. */
    btree->bm = saved_bm;
    F_CLR(dhandle, WT_DHANDLE_DEAD);

    printf("Closing session and connection ...\n");
    testutil_check(pub_session->close(pub_session, NULL));
    testutil_check(conn->close(conn, NULL));

    testutil_remove(home);
    printf("TEST PASSED.\n");
    return (EXIT_SUCCESS);
}
