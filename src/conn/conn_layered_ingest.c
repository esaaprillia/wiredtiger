/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __layered_last_checkpoint_order(
  WT_SESSION_IMPL *session, const char *shared_uri, int64_t *ckpt_order);

/*
 * __layered_assert_tombstone_has_value_on_stable_btree --
 *     Assert that a value exists on the stable btree before moving a tombstone intended to delete
 *     it.
 */
static WT_INLINE void
__layered_assert_tombstone_has_value_on_stable_btree(
  WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_UPDATE *last_upd)
{
    bool has_value;

    if (last_upd->type != WT_UPDATE_TOMBSTONE)
        return;

    /*
     * If the last update is a tombstone, ensure that there is a corresponding value on the stable
     * table that it deletes.
     */
    if (cbt->compare != 0)
        /* No on-page value to check; rely solely on visibility. */
        has_value = false;
    else {
        WT_ASSERT_ALWAYS(session, cbt->ins == NULL,
          "The stable btree should not contain inserts prior to draining");
        WT_UPDATE *upd = NULL;
        if (cbt->ref->page->modify != NULL && cbt->ref->page->modify->mod_row_update != NULL)
            upd = cbt->ref->page->modify->mod_row_update[cbt->slot];

        if (upd != NULL) {
            WT_ASSERT_ALWAYS(session, upd->txnid != WT_TXN_ABORTED,
              "The stable btree should not contain aborted updates prior to draining");
            has_value = upd->type != WT_UPDATE_TOMBSTONE;
        } else {
            WT_TIME_WINDOW tw;
            bool tw_found = __wt_read_cell_time_window(cbt, &tw);
            has_value = tw_found && !WT_TIME_WINDOW_HAS_STOP(&tw);
        }
    }

    /*
     * If a globally visible tombstone is observed at the end, the update it deletes may have been
     * removed during the obsolete check.
     */
    WT_ASSERT_ALWAYS(session, has_value || __wt_txn_upd_visible_all(session, last_upd),
      "No corresponding value exists on the stable table to delete");
}

/*
 * __layered_move_updates --
 *     Move the updates of a key to the stable table. Any unresolved prepared update on the stable
 *     table should now have been resolved.
 */
static int
__layered_move_updates(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_ITEM *key,
  WT_UPDATE *upds, WT_UPDATE *last_upd)
{
    WT_DECL_RET;

    /*
     * Disable bulk load if the btree is empty. Otherwise, checkpoint may skip this btree if it has
     * never been checkpointed.
     */
    __wt_btree_disable_bulk(session);

    /* Search the page. */
    WT_WITH_PAGE_INDEX(session, ret = __wt_row_search(cbt, key, true, NULL, false, NULL));
    WT_ERR(ret);

    __layered_assert_tombstone_has_value_on_stable_btree(session, cbt, last_upd);

    /* Apply the modification. */
    WT_ERR(__wt_row_modify(cbt, key, NULL, &upds, WT_UPDATE_INVALID, false, false));

err:
    WT_TRET(__wt_btcur_reset(cbt));
    return (ret);
}

/*
 * __layered_clear_ingest_table --
 *     After ingest content has been drained to the stable table, clear out the ingest table.
 */
static int
__layered_clear_ingest_table(WT_SESSION_IMPL *session, const char *uri)
{
    WT_ASSERT(session, WT_SUFFIX_MATCH(uri, ".wt_ingest"));

    /*
     * Truncate needs a running txn. We should probably do something more like the history store and
     * make this non-transactional -- this happens during step-up, so we know there are no other
     * transactions running, so it's safe.
     */
    WT_RET(__wt_txn_begin(session, NULL));

    /*
     * No other transactions are running, we're only doing this truncate, and it should become
     * immediately visible. So this transaction doesn't have to care about timestamps.
     */
    F_SET(session->txn, WT_TXN_TS_NOT_SET);

    WT_RET(session->iface.truncate(&session->iface, uri, NULL, NULL, NULL));

    WT_RET(__wt_txn_commit(session, NULL));

    return (0);
}

/*
 * __layered_reset_ingest_table_prune_timestamp --
 *     Reset the prune timestamp for the ingest table.
 *
 * This is used when connection steps up from follower to leader. Resetting the prune timestamp to
 *     WT_TS_NONE will allow immediate eviction of dirty ingest pages. These dirty pages are not
 *     needed any more since the new leader just drained all the ingest content to the stable table.
 */
static int
__layered_reset_ingest_table_prune_timestamp(WT_SESSION_IMPL *session, const char *ingest_uri)
{
    WT_BTREE *btree = NULL;
    WT_DECL_RET;
    wt_timestamp_t btree_prune_timestamp;

    WT_ERR_NOTFOUND_OK(__wt_session_get_dhandle(session, ingest_uri, NULL, NULL, 0), true);
    if (ret == WT_NOTFOUND) {
        __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5,
          "Handle not found for ingest table uri: %s", ingest_uri);
        ret = 0;
        goto err;
    }

    btree = (WT_BTREE *)session->dhandle->handle;
    btree_prune_timestamp = __wt_atomic_load_uint64_relaxed(&btree->prune_timestamp);

    __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5,
      "Reset prune timestamp from %" PRIu64 " to WT_TS_NONE(%d)", btree_prune_timestamp,
      WT_TS_NONE);

    __wt_atomic_store_uint64_relaxed(&btree->prune_timestamp, WT_TS_NONE);

    WT_ERR(__wt_session_release_dhandle(session));

err:
    return (ret);
}

/*
 * __layered_table_get_constituent_cursor --
 *     Retrieve or open a constituent cursor for a layered tree.
 */
static int
__layered_table_get_constituent_cursor(
  WT_SESSION_IMPL *session, uint32_t ingest_id, WT_CURSOR **cursorp)
{
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR *stable_cursor;
    WT_LAYERED_TABLE_MANAGER_ENTRY *entry;

    const char *cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), "overwrite", NULL, NULL};

    conn = S2C(session);
    entry = conn->layered_table_manager.entries[ingest_id];

    *cursorp = NULL;

    if (entry == NULL)
        return (0);

    /* Open the cursor and keep a reference in the manager entry and our caller */
    WT_RET(__wt_open_cursor(session, entry->stable_uri, NULL, cfg, &stable_cursor));
    *cursorp = stable_cursor;

    return (0);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __layered_assert_ingest_table_empty --
 *     Verify that the ingest table has no records. Called after truncation as a post-condition
 *     check.
 */
static int
__layered_assert_ingest_table_empty(WT_SESSION_IMPL *session, const char *uri)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    const char *cursor_config[] = {
      WT_CONFIG_BASE(session, WT_SESSION_open_cursor), "readonly", NULL, NULL};

    WT_RET(__wt_open_cursor(session, uri, NULL, cursor_config, &cursor));
    ret = cursor->next(cursor);
    WT_ASSERT(session, ret == WT_NOTFOUND);
    WT_TRET(cursor->close(cursor));

    return (ret == WT_NOTFOUND ? 0 : ret);
}
#endif

/*
 * __layered_fix_prepared_transaction_callback --
 *     Callback for session walk to fix prepared transactions that may be active during the ingest
 *     btree drain.
 */
static int
__layered_fix_prepared_transaction_callback(
  WT_SESSION_IMPL *session, WT_SESSION_IMPL *array_session, bool *exit_walkp, void *cookiep)
{
    WT_FIX_PREPARED_COOKIE *cookie;
    WT_TXN *txn;

    cookie = (WT_FIX_PREPARED_COOKIE *)cookiep;
    txn = array_session->txn;
    *exit_walkp = false;

    if (txn->time_point.id != cookie->txnid)
        return (0);

    for (size_t i = 0; i < txn->mod_count; i++) {
        WT_TXN_OP *op = &txn->mod[i];

        if (op->type == WT_TXN_OP_NONE)
            continue;

        if (op->btree != cookie->ingest_btree)
            continue;

        int cmp;
        WT_RET(__wt_compare(session, op->btree->collator, &op->u.op_row.key, cookie->key, &cmp));

        if (cmp < 0)
            continue;

        /*
         * The operation keys in a prepared transaction are sorted. We have passed the key we're
         * looking for.
         */
        if (cmp > 0)
            break;

        /*
         * Mark the original update on the ingest btree as aborted. Otherwise, we may get a
         * WT_ROLLBACK error when we try to truncate the ingest btree.
         */
        op->u.op_upd->txnid = WT_TXN_ABORTED;
        /* Point the operation to the stable btree. */
        op->btree = cookie->stable_btree;

        /*
         * Transfer the session_inuse reference from the ingest btree to the stable btree. The
         * ingest btree's session_inuse was incremented when this operation was recorded in the
         * transaction, and op->btree's (now the stable btree) session_inuse will be decremented
         * when the operation is freed. Adjust both counts to keep them balanced.
         */
        (void)__wt_atomic_sub_int32(&cookie->ingest_btree->dhandle->session_inuse, 1);
        (void)__wt_atomic_add_int32(&cookie->stable_btree->dhandle->session_inuse, 1);
    }

    *exit_walkp = true;
    return (0);
}

/*
 * __layered_fix_prepared_transaction --
 *     During ingest drain, a key that was prepared on the ingest btree is being moved to the stable
 *     btree. If the owning transaction is still in-flight (not yet committed or rolled back), its
 *     WT_TXN_OP entries still reference the ingest btree and the in-memory update on it. This
 *     function patches those entries so that commit/rollback will operate on the stable btree
 *     instead. For each matching operation it: (1) aborts the original in-memory update on the
 *     ingest btree so that a subsequent truncate of the ingest table does not trip over a live
 *     prepared update, (2) redirects op->btree to the stable btree, and (3) transfers the
 *     session_inuse reference from the ingest dhandle to the stable dhandle to keep reference
 *     counts balanced.
 *
 * This is a temporary solution. It assumes no concurrent commit/rollback of the prepared
 *     transaction and no prepared fast-truncate operations.
 */
static int
__layered_fix_prepared_transaction(WT_SESSION_IMPL *session, WT_ITEM *key, WT_BTREE *ingest_btree,
  WT_BTREE *stable_btree, uint64_t txnid)
{
    WT_FIX_PREPARED_COOKIE cookie;

    cookie.key = key;
    cookie.ingest_btree = ingest_btree;
    cookie.stable_btree = stable_btree;
    cookie.txnid = txnid;

    return (
      __wt_session_array_walk(session, __layered_fix_prepared_transaction_callback, true, &cookie));
}

/*
 * Cookie passed to the cache-warming worker thread spawned alongside drain.
 */
struct __wt_drain_warm_cookie {
    WT_SESSION_IMPL *session; /* Internal session owned by the warm thread for the duration. */
    const char *stable_uri;
    uint64_t usec; /* Output: wall time the walk took. */
    int ret;       /* Output: cursor open/walk error, if any. */
};

/*
 * __layered_drain_warm_skip --
 *     Tree-walk skip callback used by the warm thread. For leaf refs that are on disk, queue
 *     them for asynchronous prefetch and tell the walker to skip the synchronous page load —
 *     the prefetch threads will do the I/O. For internal refs, return skip=false so the walker
 *     descends into them; that's how we discover the leaves below. The result is a tree walk
 *     that touches only internal pages (small, mostly cached) and dispatches all leaf I/O to
 *     the prefetch worker pool. Errors from queue_push are non-fatal: EBUSY (queue full) just
 *     drops this leaf from prefetching; drain may have to load it cold but correctness is
 *     unaffected.
 */
static int
__layered_drain_warm_skip(WT_SESSION_IMPL *session, WT_REF *ref, void *cookie, bool visible_all,
  bool *skipp)
{
    WT_DECL_RET;

    WT_UNUSED(cookie);
    WT_UNUSED(visible_all);

    if (F_ISSET(ref, WT_REF_FLAG_LEAF)) {
        if (WT_REF_GET_STATE(ref) == WT_REF_DISK && ref->page_del == NULL &&
          !F_ISSET_ATOMIC_8(ref, WT_REF_FLAG_PREFETCH)) {
            ret = __wt_conn_prefetch_queue_push(session, ref);
            if (ret != 0 && ret != EBUSY)
                return (ret);
        }
        *skipp = true;
    } else
        *skipp = false;
    return (0);
}

/*
 * __layered_drain_warm_thread --
 *     Walk the stable btree at internal-page granularity, queueing every leaf for asynchronous
 *     prefetch by the connection's prefetch worker pool. The walker thread itself never
 *     page-faults on a leaf: the skip callback diverts each leaf to the queue and returns
 *     skip=true so __tree_walk_internal moves on without calling __wt_page_swap. This converts
 *     the warming walk from "sequential per-leaf cursor advance" into "metadata-only descent
 *     plus queue-push", which is CPU-bound and fast — leaf I/O fans out across the prefetch
 *     threads in parallel with drain.
 *
 *     Runs on its own session because WT sessions are not designed to be touched concurrently
 *     from multiple threads. Errors here are non-fatal for drain correctness — a failed warming
 *     just means drain runs cold — so the caller logs and continues rather than propagating.
 */
static WT_THREAD_RET
__layered_drain_warm_thread(void *arg)
{
    WT_DECL_RET;
    WT_REF *ref;
    WT_SESSION_IMPL *session;
    struct __wt_drain_warm_cookie *cookie;
    uint64_t t0, t1;
    bool dhandle_acquired;

    cookie = arg;
    session = cookie->session;
    dhandle_acquired = false;

    F_SET(session, WT_SESSION_PREFETCH_ENABLED);

    if ((ret = __wt_session_get_dhandle(session, cookie->stable_uri, NULL, NULL, 0)) != 0)
        goto done;
    dhandle_acquired = true;

    t0 = __wt_clock(session);

    ref = NULL;
    while ((ret = __wt_tree_walk_custom_skip(
              session, &ref, __layered_drain_warm_skip, NULL, 0)) == 0 &&
      ref != NULL)
        ;
    if (ret == WT_NOTFOUND)
        ret = 0;

    t1 = __wt_clock(session);
    cookie->usec = WT_CLOCKDIFF_US(t1, t0);

done:
    if (dhandle_acquired)
        WT_TRET(__wt_session_release_dhandle(session));
    cookie->ret = ret;
    return (WT_THREAD_RET_VALUE);
}

/*
 * __layered_copy_ingest_table --
 *     Moving all the data from a single ingest table to the corresponding stable table
 */
static int
__layered_copy_ingest_table(WT_SESSION_IMPL *session, WT_LAYERED_TABLE_MANAGER_ENTRY *entry)
{
    WT_BTREE *ingest_btree, *stable_btree;
    WT_CURSOR *ingest_btree_cursor, *ingest_version_cursor, *prepare_cursor, *stable_cursor;
    WT_CURSOR_BTREE *cbt;
    WT_DECL_ITEM(key);
    WT_DECL_ITEM(tmp_key);
    WT_DECL_ITEM(value);
    WT_DECL_RET;
    WT_SESSION_IMPL *warm_session;
    WT_UPDATE *last_upd, *prev_upd, *upd, *upds;
    struct __wt_drain_warm_cookie warm_cookie;
    wt_thread_t warm_tid;
    wt_timestamp_t last_checkpoint_timestamp;
    wt_timestamp_t durable_start_ts, durable_stop_ts, start_prepare_ts, start_ts, stop_prepare_ts,
      stop_ts;
    uint64_t drain_progress_now, drain_progress_start, drain_progress_last_log;
    uint64_t local_usec_cursor_next, local_usec_lookahead, local_usec_move_updates,
      local_usec_prepare_work;
    uint64_t local_version_rows, local_keys_flushed, local_updates_chained;
    uint64_t start_prepared_id, start_txn, stop_prepared_id, stop_txn;
    uint8_t flags, location, prepare, type;
    int cmp;
    char buf[256], buf2[64];
    const char *cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), NULL, NULL, NULL};
    bool is_prepare_rollback, prepare_resolved, preserve_prepared, prepare_txn_fixed,
      warm_started;

    ingest_version_cursor = prepare_cursor = stable_cursor = NULL;
    warm_session = NULL;
    warm_started = false;
    WT_CLEAR(warm_cookie);
    WT_CLEAR(warm_tid);
    last_upd = prev_upd = upd = upds = NULL;
    prepare_resolved = prepare_txn_fixed = false;
    local_usec_cursor_next = local_usec_lookahead = local_usec_move_updates =
      local_usec_prepare_work = 0;
    local_version_rows = local_keys_flushed = local_updates_chained = 0;
    preserve_prepared = F_ISSET(S2C(session), WT_CONN_PRESERVE_PREPARED);

    last_checkpoint_timestamp = __wt_atomic_load_uint64_acquire(
      &S2C(session)->disaggregated_storage.last_checkpoint_timestamp);
    WT_RET(__layered_table_get_constituent_cursor(session, entry->ingest_id, &stable_cursor));
    cbt = (WT_CURSOR_BTREE *)stable_cursor;
    stable_btree = CUR2BT(cbt);
    if (last_checkpoint_timestamp != WT_TS_NONE)
        WT_ERR(__wt_snprintf(
          buf2, sizeof(buf2), "start_timestamp=%" PRIx64 "", last_checkpoint_timestamp));
    else
        buf2[0] = '\0';
    WT_ERR(__wt_snprintf(buf, sizeof(buf),
      "debug=(dump_version=(enabled=true,raw_key_value=true,timestamp_order=true,cross_key=true,"
      "show_prepared_rollback=%s,%s))",
      preserve_prepared ? "true" : "false", buf2));
    cfg[1] = buf;
    WT_ERR(__wt_open_cursor(session, entry->ingest_uri, NULL, cfg, &ingest_version_cursor));
    ingest_btree_cursor = ((WT_CURSOR_VERSION *)ingest_version_cursor)->file_cursor;
    ingest_btree = CUR2BT(ingest_btree_cursor);

    WT_ERR(__wt_scr_alloc(session, 0, &key));
    WT_ERR(__wt_scr_alloc(session, 0, &tmp_key));
    WT_ERR(__wt_scr_alloc(session, 0, &value));

    /*
     * Spawn a worker thread to pre-warm the stable table cache by walking it end-to-end with
     * prefetch enabled. The tree-walk inside cursor->next fires __wti_btree_prefetch, which
     * queues nearby leaf pages for asynchronous load by the prefetch threads. By the time the
     * row_search calls inside __layered_move_updates touch stable leaves, they hit warm cache.
     * Running the walk concurrently with drain on a separate thread overlaps the warming wall
     * time with drain's own work — total time is max(warm, drain) rather than warm + drain. The
     * warm thread runs on its own internal session both because WT sessions are not designed to
     * be touched concurrently from multiple threads and because the session's prefetch flag and
     * cursor state must not collide with the drain session.
     *
     * Failures in the warm thread are non-fatal for drain correctness — a failed warming just
     * means drain runs cold — so we log them on join and otherwise continue.
     *
     * HACK: scope the warming to MongoDB index tables only. Indexes have small keys, dense
     * leaves, and predictable sequential access patterns, so prefetch is cleanly profitable.
     * Collections have larger values and sparser leaves where prefetch can cause cache thrash on
     * cold step-ups. Detecting via the URI substring "index-" matches MongoDB's naming
     * convention (file:index-<id>-<hash>.wt_stable) and skips collection-* tables. This is a
     * layering violation; a proper fix would be a per-table config flag set by the layered
     * table create path, but the URI heuristic is contained and correctness-safe.
     */
    if (entry->stable_uri != NULL && strstr(entry->stable_uri, "index-") != NULL) {
        WT_ERR(
          __wt_open_internal_session(S2C(session), "drain-warm", false, 0, 0, &warm_session));
        warm_cookie.session = warm_session;
        warm_cookie.stable_uri = entry->stable_uri;
        WT_ERR(__wt_thread_create(session, &warm_tid, __layered_drain_warm_thread, &warm_cookie));
        warm_started = true;
        WT_STAT_CONN_INCR(session, layered_drain_ingest_prefetch_active);
    }

    drain_progress_start = drain_progress_last_log = __wt_clock(session);
    __wt_verbose_info(session, WT_VERB_DISAGGREGATED_STORAGE,
      "Draining ingest table \"%s\" into stable table \"%s\"", entry->ingest_uri,
      entry->stable_uri);

    for (;;) {
        drain_progress_now = __wt_clock(session);
        if (WT_CLOCKDIFF_SEC(drain_progress_now, drain_progress_last_log) >= 10) {
            __wt_verbose_info(session, WT_VERB_DISAGGREGATED_STORAGE,
              "Still draining ingest table \"%s\" into stable \"%s\" (%" PRIu64
              "s elapsed): %" PRIu64 " version rows, %" PRIu64 " keys flushed, %" PRIu64
              " updates chained; ms: cursor_next=%" PRIu64 " lookahead=%" PRIu64
              " move_updates=%" PRIu64 " prepare=%" PRIu64,
              entry->ingest_uri, entry->stable_uri,
              WT_CLOCKDIFF_SEC(drain_progress_now, drain_progress_start), local_version_rows,
              local_keys_flushed, local_updates_chained, local_usec_cursor_next / 1000,
              local_usec_lookahead / 1000, local_usec_move_updates / 1000,
              local_usec_prepare_work / 1000);
            drain_progress_last_log = drain_progress_now;
        }

        upd = NULL;
        {
            uint64_t tn0, tn1;

            tn0 = __wt_clock(session);
            ret = ingest_version_cursor->next(ingest_version_cursor);
            tn1 = __wt_clock(session);
            local_usec_cursor_next += WT_CLOCKDIFF_US(tn1, tn0);
        }
        if (ret != 0 && ret != WT_NOTFOUND)
            WT_ERR(ret);
        if (ret == WT_NOTFOUND) {
            if (key->size > 0 && upds != NULL) {
                uint64_t tm0, tm1;

                WT_WITH_DHANDLE(session, cbt->dhandle, {
                    tm0 = __wt_clock(session);
                    ret = __layered_move_updates(session, cbt, key, upds, last_upd);
                    tm1 = __wt_clock(session);
                });
                WT_ERR(ret);
                local_usec_move_updates += WT_CLOCKDIFF_US(tm1, tm0);
                ++local_keys_flushed;
                upds = NULL;
            } else
                ret = 0;
            break;
        }

        ++local_version_rows;

        WT_ERR(ingest_version_cursor->get_key(ingest_version_cursor, tmp_key));
        WT_ERR(__wt_compare(session, stable_btree->collator, key, tmp_key, &cmp));
        if (cmp != 0) {
            /*
             * Ensure keys returned are in correctly sorted order. Only perform this check when key
             * has been initialized.
             */
            WT_ASSERT(session, key->size == 0 || cmp <= 0);

            if (upds != NULL) {
                uint64_t tm0, tm1;

                WT_WITH_DHANDLE(session, cbt->dhandle, {
                    tm0 = __wt_clock(session);
                    ret = __layered_move_updates(session, cbt, key, upds, last_upd);
                    tm1 = __wt_clock(session);
                });
                WT_ERR(ret);
                local_usec_move_updates += WT_CLOCKDIFF_US(tm1, tm0);
                ++local_keys_flushed;
            }

            upds = NULL;
            prev_upd = NULL;
            prepare_txn_fixed = false;
            prepare_resolved = false;
            WT_ERR(__wt_buf_set(session, key, tmp_key->data, tmp_key->size));
        }

        WT_ERR(ingest_version_cursor->get_value(ingest_version_cursor, &start_txn, &start_ts,
          &durable_start_ts, &start_prepare_ts, &start_prepared_id, &stop_txn, &stop_ts,
          &durable_stop_ts, &stop_prepare_ts, &stop_prepared_id, &type, &prepare, &flags, &location,
          value));

        is_prepare_rollback = start_txn == WT_TXN_ABORTED;
        /*
         * It is possible to see a full value that is smaller than or equal to the last checkpoint
         * timestamp with a stop timestamp that is larger than the last checkpoint timestamp. Ignore
         * the update in this case.
         */
        if (prepare || durable_start_ts > last_checkpoint_timestamp) {
            /*
             * If the "preserve prepared" option is enabled and the ingest btree contains a resolved
             * prepared update for this key whose prepared timestamp is less than or equal to the
             * last checkpoint timestamp, the stable btree must still contain an unresolved prepared
             * cell from a previous checkpoint. To ensure data consistency, resolve the unresolved
             * prepared cell before applying the ingest updates.
             */
            if (preserve_prepared && start_prepared_id != WT_PREPARED_ID_NONE &&
              start_prepare_ts <= last_checkpoint_timestamp) {
                if (prepare) {
                    if (!prepare_txn_fixed) {
                        uint64_t tp0, tp1;

                        WT_ASSERT(session, upds == NULL);
                        tp0 = __wt_clock(session);
                        ret = __layered_fix_prepared_transaction(
                          session, key, ingest_btree, stable_btree, start_txn);
                        tp1 = __wt_clock(session);
                        local_usec_prepare_work += WT_CLOCKDIFF_US(tp1, tp0);
                        WT_ERR(ret);
                        prepare_txn_fixed = true;
                    }
                } else if (!prepare_resolved) {
                    /* Only resolve the updates from the same prepared transaction once. */
                    if (is_prepare_rollback) {
                        /*
                         * The original transaction id is stored in start timestamp and the rollback
                         * timestamp is stored in durable timestamp.
                         */
                        WT_TXN_TIME_POINT txn_time_point;
                        uint64_t tp0, tp1;

                        txn_time_point.id = start_ts;
                        txn_time_point.prepared_id = start_prepared_id;
                        txn_time_point.prepare_timestamp = start_prepare_ts;
                        txn_time_point.rollback_timestamp = durable_start_ts;
                        tp0 = __wt_clock(session);
                        ret = __wt_txn_resolve_prepared_op(session, stable_btree, &txn_time_point,
                          key, WT_RECNO_OOB, false, &prepare_cursor);
                        tp1 = __wt_clock(session);
                        local_usec_prepare_work += WT_CLOCKDIFF_US(tp1, tp0);
                        WT_ERR(ret);
                    } else {
                        WT_TXN_TIME_POINT txn_time_point;
                        uint64_t tp0, tp1;

                        txn_time_point.id = start_txn;
                        txn_time_point.prepared_id = start_prepared_id;
                        txn_time_point.prepare_timestamp = start_prepare_ts;
                        txn_time_point.commit_timestamp = start_ts;
                        txn_time_point.durable_timestamp = durable_start_ts;
                        tp0 = __wt_clock(session);
                        ret = __wt_txn_resolve_prepared_op(session, stable_btree, &txn_time_point,
                          key, WT_RECNO_OOB, true, &prepare_cursor);
                        tp1 = __wt_clock(session);
                        local_usec_prepare_work += WT_CLOCKDIFF_US(tp1, tp0);
                        WT_ERR(ret);
                    }
                    prepare_resolved = true;
                }
            } else {
                /*
                 * If the update is not a prepared update or a resolved prepared update that has
                 * never been written to the checkpoint as a prepared update, move it to the stable
                 * table directly.
                 */
                /*
                 * FIXME-WT-14732: this is an ugly layering violation. But I can't think of a better
                 * way now.
                 */
                if (__wt_clayered_deleted(value))
                    WT_ERR(__wt_upd_alloc_tombstone(session, &upd, NULL));
                else
                    WT_ERR(__wt_upd_alloc(session, value, WT_UPDATE_STANDARD, &upd, NULL));
                /*
                 * If the prepared update is aborted, move the aborted update to the stable table
                 * because we may write a prepared update to the disk in a future reconciliation.
                 */
                if (is_prepare_rollback) {
                    /* Prepared transactions must have a prepared id in disagg. */
                    WT_ASSERT(session,
                      !prepare && preserve_prepared && start_prepared_id != WT_PREPARED_ID_NONE);
                    /*
                     * The original transaction id is stored in start timestamp and the rollback
                     * timestamp is stored in durable timestamp.
                     */
                    upd->txnid = WT_TXN_ABORTED;
                    upd->prepare_state = WT_PREPARE_INPROGRESS;
                    upd->prepare_ts = start_prepare_ts;
                    upd->prepared_id = start_prepared_id;
                    upd->upd_saved_txnid = start_ts;
                    upd->upd_rollback_ts = durable_start_ts;
                } else {
                    WT_ASSERT(session, !prepare || durable_start_ts == WT_TS_NONE);
                    upd->txnid = start_txn;
                    if (prepare)
                        upd->prepare_state = WT_PREPARE_INPROGRESS;
                    else if (start_prepared_id != WT_PREPARED_ID_NONE)
                        upd->prepare_state = WT_PREPARE_RESOLVED;
                    upd->prepare_ts = start_prepare_ts;
                    upd->prepared_id = start_prepared_id;
                    upd->upd_start_ts = start_ts;
                    upd->upd_durable_ts = durable_start_ts;
                }
                /* This is for debugging purpose and it is not checked in the code. */
                F_SET(upd, WT_UPDATE_RESTORED_FROM_INGEST);
                last_upd = upd;

                if (prepare && !prepare_txn_fixed) {
                    uint64_t tp0, tp1;

                    WT_ASSERT(session, upds == NULL);
                    tp0 = __wt_clock(session);
                    ret = __layered_fix_prepared_transaction(
                      session, key, ingest_btree, stable_btree, start_txn);
                    tp1 = __wt_clock(session);
                    local_usec_prepare_work += WT_CLOCKDIFF_US(tp1, tp0);
                    WT_ERR(ret);
                    prepare_txn_fixed = true;
                }
            }
        }

        if (upd != NULL) {
            ++local_updates_chained;
            /* If a prepared update is resolved, it must be the final update to be drained. */
            WT_ASSERT(session, !prepare_resolved);
            if (prev_upd != NULL)
                prev_upd->next = upd;
            else
                upds = upd;

            prev_upd = upd;
        }
    }

    {
        uint64_t wall_us;

        wall_us = WT_CLOCKDIFF_US(__wt_clock(session), drain_progress_start);

        /*
         * Flush per-table accumulators into connection stats once at end of drain. We accumulate
         * locally in microseconds for precision (sub-millisecond per-call ops would round to zero
         * if incremented in milliseconds) and convert here before bumping the millisecond stats.
         */
        WT_STAT_CONN_INCRV(
          session, layered_drain_ingest_msec_cursor_next, local_usec_cursor_next / 1000);
        WT_STAT_CONN_INCRV(
          session, layered_drain_ingest_msec_lookahead, local_usec_lookahead / 1000);
        WT_STAT_CONN_INCRV(
          session, layered_drain_ingest_msec_move_updates, local_usec_move_updates / 1000);
        WT_STAT_CONN_INCRV(
          session, layered_drain_ingest_msec_prepare_work, local_usec_prepare_work / 1000);
        WT_STAT_CONN_INCRV(session, layered_drain_ingest_msec_total, wall_us / 1000);
        WT_STAT_CONN_INCRV(session, layered_drain_ingest_keys_flushed, local_keys_flushed);
        WT_STAT_CONN_INCRV(session, layered_drain_ingest_updates_chained, local_updates_chained);
        WT_STAT_CONN_INCRV(session, layered_drain_ingest_version_rows, local_version_rows);

        __wt_verbose_info(session, WT_VERB_DISAGGREGATED_STORAGE,
          "Finished draining ingest table \"%s\" into stable \"%s\": %" PRIu64 " version rows, %" PRIu64
          " keys flushed, %" PRIu64 " updates chained; ms cursor_next=%" PRIu64
          " lookahead=%" PRIu64 " move_updates=%" PRIu64 " prepare=%" PRIu64 " wall=%" PRIu64,
          entry->ingest_uri, entry->stable_uri, local_version_rows, local_keys_flushed,
          local_updates_chained, local_usec_cursor_next / 1000, local_usec_lookahead / 1000,
          local_usec_move_updates / 1000, local_usec_prepare_work / 1000, wall_us / 1000);
    }

err:
    if (upd != NULL)
        __wt_free(session, upd);
    if (upds != NULL)
        __wt_free_update_list(session, &upds);
    __wt_scr_free(session, &key);
    __wt_scr_free(session, &tmp_key);
    __wt_scr_free(session, &value);
    /*
     * Join the warming thread before closing cursors and the warm session — the warm thread
     * holds a cursor on the stable dhandle and references the warm session. Warming errors are
     * non-fatal for drain: log them and proceed.
     */
    if (warm_started) {
        WT_TRET(__wt_thread_join(session, &warm_tid));
        if (warm_cookie.ret != 0)
            __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_WARNING,
              "Drain cache-warming thread failed for \"%s\": %d", entry->stable_uri,
              warm_cookie.ret);
        local_usec_lookahead = warm_cookie.usec;
    }
    if (ingest_version_cursor != NULL)
        WT_TRET(ingest_version_cursor->close(ingest_version_cursor));
    if (prepare_cursor != NULL)
        WT_TRET(prepare_cursor->close(prepare_cursor));
    if (stable_cursor != NULL)
        WT_TRET(stable_cursor->close(stable_cursor));
    if (warm_session != NULL)
        WT_TRET(__wt_session_close_internal(warm_session));
    return (ret);
}

/*
 * __layered_drain_worker_run --
 *     Run function for drain workers.
 */
static int
__layered_drain_worker_run(WT_SESSION_IMPL *session, WT_THREAD *ctx)
{
    WT_DECL_RET;
    WT_CONNECTION_IMPL *conn = S2C(session);
    WT_UNUSED(ctx);
    __wt_spin_lock(session, &conn->layered_drain_data.queue_lock);
    /* If the queue is empty we are done. */
    if (TAILQ_EMPTY(&conn->layered_drain_data.work_queue)) {
        __wt_spin_unlock(session, &conn->layered_drain_data.queue_lock);
        return (0);
    }

    WT_LAYERED_DRAIN_ENTRY *work_item = TAILQ_FIRST(&conn->layered_drain_data.work_queue);
    WT_ASSERT(session, work_item != NULL);
    TAILQ_REMOVE(&conn->layered_drain_data.work_queue, work_item, q);
    __wt_spin_unlock(session, &conn->layered_drain_data.queue_lock);
    WT_ERR_MSG_CHK(session, __layered_copy_ingest_table(session, work_item->entry),
      "Failed to copy ingest table \"%s\" to stable table \"%s\"", work_item->entry->ingest_uri,
      work_item->entry->stable_uri);
    {
        uint64_t tt0, tt1, tt_us;

        tt0 = __wt_clock(session);
        ret = __layered_clear_ingest_table(session, work_item->entry->ingest_uri);
        tt1 = __wt_clock(session);
        tt_us = WT_CLOCKDIFF_US(tt1, tt0);
        WT_STAT_CONN_INCRV(session, layered_drain_ingest_msec_truncate, tt_us / 1000);
        __wt_verbose_info(session, WT_VERB_DISAGGREGATED_STORAGE,
          "Truncated ingest table \"%s\" in %" PRIu64 " ms", work_item->entry->ingest_uri,
          tt_us / 1000);
        WT_ERR_MSG_CHK(session, ret, "Failed to clear ingest table \"%s\"",
          work_item->entry->ingest_uri);
    }

#ifdef HAVE_DIAGNOSTIC
    WT_ERR(__layered_assert_ingest_table_empty(session, work_item->entry->ingest_uri));
#endif

    WT_ERR_MSG_CHK(session,
      __layered_reset_ingest_table_prune_timestamp(session, work_item->entry->ingest_uri),
      "Failed to reset ingest table prune timestamp \"%s\"", work_item->entry->ingest_uri);

    WT_ASSERT(session, work_item->entry->pinned_dhandle != NULL);
    WT_WITH_DHANDLE(session, work_item->entry->pinned_dhandle, {
        work_item->entry->pinned_dhandle = NULL;
        __wt_cursor_dhandle_decr_use(session);
    });

err:
    __wt_free(session, work_item);
    return (ret);
}

/*
 * __layered_drain_worker_check --
 *     Check function for drain workers.
 */
static bool
__layered_drain_worker_check(WT_SESSION_IMPL *session)
{
    return (__wt_atomic_load_bool_relaxed(&S2C(session)->layered_drain_data.running));
}

/*
 * __layered_drain_clear_work_queue --
 *     Clear the work queue for ingest table drain.
 */
static void
__layered_drain_clear_work_queue(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn = S2C(session);
    __wt_spin_lock(session, &conn->layered_drain_data.queue_lock);
    if (!TAILQ_EMPTY(&conn->layered_drain_data.work_queue)) {
        WT_LAYERED_DRAIN_ENTRY *work_item = NULL, *work_item_tmp = NULL;
        TAILQ_FOREACH_SAFE(work_item, &conn->layered_drain_data.work_queue, q, work_item_tmp)
        {
            TAILQ_REMOVE(&conn->layered_drain_data.work_queue, work_item, q);
            __wt_free(session, work_item);
        }
    }
    WT_ASSERT_ALWAYS(session, TAILQ_EMPTY(&conn->layered_drain_data.work_queue),
      "Layered drain work queue failed to drain");
    __wt_spin_unlock(session, &conn->layered_drain_data.queue_lock);
    __wt_spin_destroy(session, &conn->layered_drain_data.queue_lock);
}

/*
 * __wti_layered_drain_ingest_tables --
 *     Moving all the data from the ingest tables to the stable tables
 */
int
__wti_layered_drain_ingest_tables(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_LAYERED_TABLE_MANAGER *manager;
    WT_LAYERED_TABLE_MANAGER_ENTRY *entry;

    size_t i, table_count;
    bool empty, group_created;

    conn = S2C(session);
    manager = &conn->layered_table_manager;
    group_created = false;

    __wt_spin_lock(session, &manager->layered_table_lock);

    table_count = manager->open_layered_table_count;

    /*
     * FIXME-WT-14734: shouldn't we hold this lock longer, e.g. manager->entries could get
     * reallocated, or individual entries could get removed or freed.
     */
    __wt_spin_unlock(session, &manager->layered_table_lock);
    /* Initialize the work queue. */
    TAILQ_INIT(&conn->layered_drain_data.work_queue);
    WT_RET(__wt_spin_init(
      session, &conn->layered_drain_data.queue_lock, "layered drain work queue lock"));

    __wt_atomic_store_bool(&conn->layered_drain_data.running, true);

    bool multithreaded = conn->layered_drain_data.thread_count > 1;

    /*
     * Create the thread group. The application thread is also a drain thread so the configured
     * thread count needs to be greater than 1 for this to be meaningful. We still lock and queue
     * work for single threaded mode, as such single threaded is only recommended for testing.
     */
    if (multithreaded) {
        WT_ERR(__wt_thread_group_create(session, &conn->layered_drain_data.threads, "disagg-drain",
          conn->layered_drain_data.thread_count - 1, conn->layered_drain_data.thread_count - 1,
          WT_THREAD_CAN_WAIT | WT_THREAD_PANIC_FAIL, __layered_drain_worker_check,
          __layered_drain_worker_run, NULL));
        group_created = true;
    }

    /* FIXME-WT-14735: skip empty ingest tables. */
    for (i = 0; i < table_count; i++) {
        if ((entry = manager->entries[i]) != NULL) {
            /*
             * Mark the layered table in use, we don't want it to be closed between now and when the
             * drain takes place, otherwise this entry would be freed.
             */
            WT_ERR(__wt_cursor_uri_incr_use(session, entry->layered_uri, &entry->pinned_dhandle));

            WT_LAYERED_DRAIN_ENTRY *work_item;
            WT_ERR(__wt_calloc_one(session, &work_item));
            work_item->entry = entry;
            __wt_spin_lock(session, &conn->layered_drain_data.queue_lock);
            TAILQ_INSERT_HEAD(&conn->layered_drain_data.work_queue, work_item, q);
            __wt_spin_unlock(session, &conn->layered_drain_data.queue_lock);
        }
    }

    /*
     * We can be lazy here and use the current thread as a worker thread. Then once this loop exits
     * we can kill our thread group.
     */
    while (true) {
        __wt_spin_lock(session, &conn->layered_drain_data.queue_lock);
        empty = TAILQ_EMPTY(&conn->layered_drain_data.work_queue);
        __wt_spin_unlock(session, &conn->layered_drain_data.queue_lock);
        if (empty) {
            /*
             * Notify the other threads to exit. Relaxed is okay here as the worker threads will
             * observe this change eventually.
             */
            __wt_atomic_store_bool_relaxed(&conn->layered_drain_data.running, false);
            break;
        }
        WT_ERR(__layered_drain_worker_run(session, NULL));
    }

err:
    /* Let any running threads finish up. */
    if (group_created) {
        __wt_cond_signal(session, conn->layered_drain_data.threads.wait_cond);
        __wt_writelock(session, &conn->layered_drain_data.threads.lock);
        WT_TRET(__wt_thread_group_destroy(session, &conn->layered_drain_data.threads));
    }
    /* Cleanup and release resources. */
    __layered_drain_clear_work_queue(session);
    return (ret);
}

/*
 * __layered_update_ingest_table_prune_timestamp --
 *     Update the prune timestamp of the specified ingest table.
 *
 * We want to see what is the oldest checkpoint on the provided table that is in use by any open
 *     cursor. Even if there are no open cursors on it, the most recent checkpoint on the table is
 *     always considered in use. The basic plan is to start with the last checkpoint in use that we
 *     knew about, and check it again. If it's no longer in use, we go to the next one, etc. This
 *     gives us a list (possibly zero length), of checkpoints that are no longer in use by cursors
 *     on this table. Thus, the timestamp associated with the newest such checkpoint can be used for
 *     garbage collection pruning. Any item in the ingest table older than that timestamp must be
 *     including in one of the checkpoints we're saving, and thus can be removed.
 *
 * The `uri_at_checkpoint_buf` argument is used only to avoid extra allocations between consecutive
 *     calls.
 */
static int
__layered_update_ingest_table_prune_timestamp(WT_SESSION_IMPL *session, const char *layered_uri,
  wt_timestamp_t checkpoint_timestamp, WT_ITEM *uri_at_checkpoint_buf)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_LAYERED_TABLE *layered_table;
    wt_timestamp_t btree_prune_timestamp, prune_timestamp;
    int64_t ckpt_inuse, last_ckpt;
    int32_t layered_dhandle_inuse, stable_dhandle_inuse;

    layered_table = NULL;
    prune_timestamp = WT_TS_NONE;

    /*
     * Get the layered table from the provided URI. We don't hold any global locks so that's
     * possible that it was already removed.
     */
    WT_ERR_NOTFOUND_OK(__wt_session_get_dhandle(session, layered_uri, NULL, NULL, 0), true);
    if (ret == WT_NOTFOUND) {
        __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5,
          "GC %s: Layered table was not found.", layered_uri);
        return (0);
    }
    layered_table = (WT_LAYERED_TABLE *)session->dhandle;

    /*
     * Get the last existing checkpoint. If we've never seen a checkpoint, then there's nothing in
     * the ingest table we can remove. Move on.
     */
    WT_ERR_NOTFOUND_OK(
      __layered_last_checkpoint_order(session, layered_table->stable_uri, &last_ckpt), true);
    if (ret == WT_NOTFOUND) {
        __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5,
          "GC %s: Layered table checkpoint does not exist: %s", layered_table->iface.name,
          layered_table->stable_uri);
        ret = 0;
        goto err;
    }

    /*
     * If we are setting a prune timestamp the first time, the previous checkpoint could still be in
     * use, so start from it.
     */
    ckpt_inuse = layered_table->last_ckpt_inuse;
    if (ckpt_inuse == 0)
        ckpt_inuse = (last_ckpt > 1) ? last_ckpt - 1 : last_ckpt;

    /* Find the last checkpoint which is still in use. */
    while (ckpt_inuse < last_ckpt) {
        stable_dhandle_inuse = 0;
        WT_ERR(__wt_buf_fmt(session, uri_at_checkpoint_buf, "%s/%s.%" PRId64,
          layered_table->stable_uri, WT_CHECKPOINT, ckpt_inuse));

        /* If it's in use, then it must be in the connection cache. */
        WT_WITH_HANDLE_LIST_READ_LOCK(session,
          if ((ret = __wt_conn_dhandle_find(session, uri_at_checkpoint_buf->data, NULL)) == 0)
            WT_DHANDLE_ACQUIRE(session->dhandle));

        /* If one exists, read all the required info, then release. */
        if (ret == 0) {
            stable_dhandle_inuse = __wt_atomic_load_int32_acquire(&session->dhandle->session_inuse);
            WT_ASSERT(session, prune_timestamp <= S2BT(session)->checkpoint_timestamp);
            prune_timestamp = S2BT(session)->checkpoint_timestamp;
            WT_DHANDLE_RELEASE(session->dhandle);
        }

        WT_ERR_NOTFOUND_OK(ret, false);

        /* If it's in use by any session, then we're done. */
        if (stable_dhandle_inuse > 0)
            break;

        ++ckpt_inuse;
    }

    layered_dhandle_inuse =
      __wt_atomic_load_int32_acquire(&((WT_DATA_HANDLE *)layered_table)->session_inuse);
    if (ckpt_inuse == last_ckpt && (last_ckpt != 1 || layered_dhandle_inuse == 0))
        prune_timestamp = checkpoint_timestamp;

    if (ckpt_inuse == layered_table->last_ckpt_inuse) {
        __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5,
          "GC %s: Nothing to update - the last checkpoint is still in use %" PRId64,
          layered_table->iface.name, ckpt_inuse);
        ret = 0;
        goto err;
    }

    if (prune_timestamp == WT_TS_NONE) {
        __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5,
          "GC %s: No checkpoint is eligible for pruning. The last checkpoint in use is %" PRId64,
          layered_table->iface.name, ckpt_inuse);
        ret = 0;
        goto err;
    }

    /*
     * Set the prune timestamp in the btree if it is open, typically it is. However, it's possible
     * that it hasn't been opened yet. In that case, we need to skip updating its timestamp for
     * pruning, and we'll get another chance to update the prune timestamp at the next checkpoint.
     */
    WT_ERR_NOTFOUND_OK(
      __wt_session_get_dhandle(session, layered_table->ingest_uri, NULL, NULL, 0), true);
    if (ret == WT_NOTFOUND) {
        __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5,
          "GC %s: Handle not found for ingest table uri: %s", layered_table->iface.name,
          layered_table->ingest_uri);
        ret = 0;
        goto err;
    }

    btree = (WT_BTREE *)session->dhandle->handle;

    btree_prune_timestamp = __wt_atomic_load_uint64_relaxed(&btree->prune_timestamp);
    WT_ASSERT(session, prune_timestamp >= btree_prune_timestamp);

    __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5,
      "GC %s: update prune timestamp from %" PRIu64 " to %" PRIu64
      " and checkpoint in use from %" PRId64 " to %" PRId64,
      layered_table->iface.name, btree_prune_timestamp, prune_timestamp,
      layered_table->last_ckpt_inuse, ckpt_inuse);

    /*
     * The prune timestamp should be monotonically increasing. It is fine for the user to read the
     * obsolete value. Therefore, no synchronization is required.
     */
    __wt_atomic_store_uint64_relaxed(&btree->prune_timestamp, prune_timestamp);
    layered_table->last_ckpt_inuse = ckpt_inuse;

    WT_ERR(__wt_session_release_dhandle(session));

err:
    WT_ASSERT(session, layered_table != NULL);
    session->dhandle = (WT_DATA_HANDLE *)layered_table;
    WT_TRET(__wt_session_release_dhandle(session));

    return (ret);
}

/*
 * __wti_layered_iterate_ingest_tables_for_gc_pruning --
 *     Iterate over all ingest tables and check whether their prune timestamps could be updated.
 */
int
__wti_layered_iterate_ingest_tables_for_gc_pruning(
  WT_SESSION_IMPL *session, wt_timestamp_t checkpoint_timestamp)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(layered_table_uri_buf);
    WT_DECL_ITEM(uri_at_checkpoint_buf);
    WT_DECL_RET;
    WT_LAYERED_TABLE_MANAGER *manager;
    WT_LAYERED_TABLE_MANAGER_ENTRY *entry;
    size_t i;

    conn = S2C(session);
    manager = &conn->layered_table_manager;
    WT_RET(__wt_scr_alloc(session, 0, &layered_table_uri_buf));
    WT_RET(__wt_scr_alloc(session, 0, &uri_at_checkpoint_buf));

    WT_ASSERT(session, manager->init);

    __wt_spin_lock(session, &manager->layered_table_lock);
    for (i = 0; i < manager->open_layered_table_count; i++) {
        if ((entry = manager->entries[i]) == NULL)
            continue;
        ret = __wt_buf_setstr(session, layered_table_uri_buf, entry->layered_uri);

        /*
         * Unlock the mutex while handling a table since while updating the prune timestamp we get a
         * dhandle lock which could cause a deadlock.
         *
         * Releasing the mutex may allow the table to grow, shrink or be modified during this
         * operation. It's okay to prune an element twice in a loop (the second pruning will
         * probably do nothing), or miss an element to prune (it will be visited next time).
         */
        __wt_spin_unlock(session, &manager->layered_table_lock);

        /* Check the buffer-copy result here to avoid returning with the mutex held. */
        WT_ERR(ret);

        WT_ERR(__layered_update_ingest_table_prune_timestamp(
          session, layered_table_uri_buf->data, checkpoint_timestamp, uri_at_checkpoint_buf));

        __wt_spin_lock(session, &manager->layered_table_lock);
    }
    __wt_spin_unlock(session, &manager->layered_table_lock);

err:
    if (ret != 0)
        __wt_verbose_level(
          session, WT_VERB_LAYERED, WT_VERBOSE_ERROR, "GC ingest tables prune failed by: %d", ret);

    __wt_scr_free(session, &layered_table_uri_buf);
    __wt_scr_free(session, &uri_at_checkpoint_buf);
    return (ret);
}

/*
 * __layered_last_checkpoint_order --
 *     For a URI, get the order number for the most recent checkpoint.
 */
static int
__layered_last_checkpoint_order(
  WT_SESSION_IMPL *session, const char *shared_uri, int64_t *ckpt_order)
{
    int scanf_ret;

    const char *checkpoint_name;
    int64_t order_from_name;

    *ckpt_order = 0;

    /* Pull up the last checkpoint for this URI. It could return WT_NOTFOUND. */
    WT_RET(__wt_meta_checkpoint_last_name(session, shared_uri, &checkpoint_name, ckpt_order, NULL));

    /* Sanity check: we make sure that the name returned matches the order number. */
    scanf_ret = sscanf(checkpoint_name, WT_CHECKPOINT ".%" PRId64, &order_from_name);
    __wt_free(session, checkpoint_name);

    if (scanf_ret != 1)
        WT_RET_MSG(session, EINVAL,
          "shared metadata checkpoint unknown format: %s, scan returns %d", checkpoint_name,
          scanf_ret);

    /* These should always be the same. */
    WT_ASSERT(session, *ckpt_order == order_from_name);

    return (0);
}
