/*-
 * Copyright (c) 2014-2020 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *  All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __help_bucket --
 *     Block cache verbose logging.
 */
static WT_INLINE uint64_t
__help_bucket(uint64_t hash, u_int hash_size)
{
    /* simulate collision on buckets, index 0-50% are 1:1 mapped to their buckets, index 50%-90% are
     * 5:1 mapped to their buckets index 90%-100% are 20 : 1 mapped to their buckets.*/
    uint64_t bucket = hash % hash_size;
    u_int t50 = hash_size / 2;
    u_int t90 = hash_size / 10 * 9;
    /* this simulates collision, the first 50% of buckets are mapped to first 50% slots, the next
     * 40% of buckets are mapped to the next 40%/5 which is 8% of slots, the last 10% of buckets are
     * mapped to 0.5% of slots.*/
    if (bucket <= t50) {
        return (bucket);
    } else if (bucket <= t90) {
        return (bucket - t50) / 5 + t50;
    } else {
        return (bucket - t90) / 20 + t90;
    }
}

/*
 * __page_cache_verbose --
 *     Block cache verbose logging.
 */
static WT_INLINE void
__page_cache_verbose(WT_SESSION_IMPL *session, WT_VERBOSE_LEVEL level, const char *tag,
  uint64_t hash, const uint8_t *addr, size_t addr_size)
{
    WT_DECL_ITEM(tmp);
    const char *addr_string;

    if (!WT_VERBOSE_LEVEL_ISSET(session, WT_VERB_BLKCACHE, level))
        return;

    /*
     * Complicate the error handling so we don't have to return a value from this function, it
     * simplifies error handling in our callers.
     */
    addr_string = __wt_scr_alloc(session, 0, &tmp) == 0 ?
      __wt_addr_string(session, addr, addr_size, tmp) :
      "[unable to format addr]";
    __wt_verbose_level(
      session, WT_VERB_BLKCACHE, level, "%s: %s, hash=%" PRIu64, tag, addr_string, hash);
    __wt_scr_free(session, &tmp);
}

/*
 * __wt_page_cache_get --
 *     Get a block from the cache.
 */
void
__wt_page_cache_get(WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size,
  WT_PAGE_CACHE_ITEM **page_cache_retp, bool *foundp)
{
    WT_PAGE_CACHE *page_cache;
    WT_PAGE_CACHE_ITEM *page_cache_item;
    uint64_t bucket, hash;
    uint64_t time_diff, time_start, time_stop;

    time_start = __wt_clock(session);

    *foundp = false;
    *page_cache_retp = NULL;

    page_cache = &S2C(session)->cache->page_cache;

    WT_STAT_CONN_INCR(session, block_cache_lookups);

    hash = __wt_hash_city64(addr, addr_size);
    bucket = __help_bucket(hash, page_cache->hash_size);
    __wt_spin_lock(session, &page_cache->hash_locks[bucket]);
    TAILQ_FOREACH (page_cache_item, &page_cache->hash[bucket], hashq) {
        if (page_cache_item->addr_size == addr_size && page_cache_item->fid == S2BT(session)->id &&
          memcmp(page_cache_item->addr, addr, addr_size) == 0) {
            ++page_cache_item->ref_count;
            break;
        }
    }
    __wt_spin_unlock(session, &page_cache->hash_locks[bucket]);

    if (page_cache_item != NULL) {
        *page_cache_retp = page_cache_item;
        *foundp = true;
        if (page_cache->max_ref_count < page_cache_item->ref_count) {
            page_cache->max_ref_count = page_cache_item->ref_count;
            WT_STAT_CONN_SET(session, page_cache_max_page_ref_count, page_cache->max_ref_count);
        }
        WT_STAT_CONN_INCR(session, page_cache_total_page_count);
        WT_STAT_CONN_INCR(session, page_cache_hit);
        __page_cache_verbose(
          session, WT_VERBOSE_DEBUG_2, "page found in cache", hash, addr, addr_size);
    } else {
        WT_STAT_CONN_INCR(session, page_cache_miss);
        __page_cache_verbose(
          session, WT_VERBOSE_DEBUG_2, "page not found in cache", hash, addr, addr_size);
    }

    time_stop = __wt_clock(session);
    time_diff = WT_CLOCKDIFF_US(time_stop, time_start);
    if (page_cache->max_get_time < time_diff) {
        page_cache->max_get_time = time_diff;
        WT_STAT_CONN_SET(session, page_cache_max_get_time, time_diff);
    }
    WT_STAT_CONN_INCRV(session, page_cache_total_op_time, time_diff);
    WT_STAT_CONN_INCR(session, page_cache_total_op);
}

/*
 * __wt_page_cache_put --
 *     Put a block into the cache.
 */
int
__wt_page_cache_put(WT_SESSION_IMPL *session, const void *data, size_t data_size,
  WT_PAGE_BLOCK_META *block_meta, const uint8_t *addr, size_t addr_size,
  WT_PAGE_CACHE_ITEM **page_cache_retp)
{
    WT_DECL_RET;
    WT_PAGE_BLOCK_META *block_meta_ptr;
    WT_PAGE_CACHE *page_cache;
    WT_PAGE_CACHE_ITEM *page_cache_item, *page_cache_store;
    uint64_t bucket, hash;
    void *data_ptr;
    uint32_t bucket_size = 0;
    uint64_t time_diff, time_start, time_stop;
    bool cache_inserted = false;

    time_start = __wt_clock(session);

    page_cache = &S2C(session)->cache->page_cache;
    block_meta_ptr = NULL;
    data_ptr = NULL;
    *page_cache_retp = NULL;

    /*
     * Allocate and initialize space in the cache outside of the critical section. In the unlikely
     * event that we fail an allocation, free the space. NVRAM allocations can fail if there's no
     * available memory, treat it as a cache-full failure.
     */
    WT_RET(__wt_malloc(session, data_size, &data_ptr));
    if (data_ptr == NULL)
        goto err;
    memcpy(data_ptr, data, data_size);

    WT_ERR(__wt_calloc(session, 1, sizeof(*page_cache_store) + addr_size, &page_cache_store));
    page_cache_store->data = data_ptr;
    page_cache_store->data_size = WT_STORE_SIZE(data_size);

    if (block_meta != NULL) {
        WT_ERR(__wt_calloc(session, 1, sizeof(*block_meta_ptr), &block_meta_ptr));
        *block_meta_ptr = *block_meta;
        page_cache_store->block_meta = block_meta_ptr;
    }

    page_cache_store->fid = S2BT(session)->id;
    page_cache_store->addr_size = (uint8_t)addr_size;
    page_cache_store->ref_count = 1;
    memcpy(page_cache_store->addr, addr, addr_size);

    hash = __wt_hash_city64(addr, addr_size);
    bucket = __help_bucket(hash, page_cache->hash_size);
    __wt_spin_lock(session, &page_cache->hash_locks[bucket]);

    /*
     * In the case of a read, check if the block is already in the cache: it's possible because two
     * readers can attempt to cache the same overflow block because overflow blocks aren't cached at
     * the btree level. Collisions are relatively unlikely because other page types are cached at
     * higher levels and reads of those tree pages are single-threaded so the page can be converted
     * to its in-memory form before reader access. In summary, because collisions are unlikely, the
     * allocation and copying remains outside of the bucket lock and collision check. Writing a
     * block is single-threaded at a higher level, and as there should never be a collision, only
     * check in diagnostic mode.
     */

    TAILQ_FOREACH (page_cache_item, &page_cache->hash[bucket], hashq) {
        ++bucket_size;
        if (page_cache_item->addr_size == addr_size && page_cache_item->fid == S2BT(session)->id &&
          memcmp(page_cache_item->addr, addr, addr_size) == 0) {
            ++page_cache_item->ref_count;
            __wt_spin_unlock(session, &page_cache->hash_locks[bucket]);

            *page_cache_retp = page_cache_item;
            WT_STAT_CONN_INCR(session, block_cache_blocks_update);
            __page_cache_verbose(
              session, WT_VERBOSE_DEBUG_2, "block already in cache", hash, addr, addr_size);
            goto done;
        }
    }
    if (page_cache->max_bucket_size < bucket_size) {
        page_cache->max_bucket_size = bucket_size;
        WT_STAT_CONN_SET(session, page_cache_max_bucket_size, bucket_size);
    }

    TAILQ_INSERT_HEAD(&page_cache->hash[bucket], page_cache_store, hashq);

    __wt_spin_unlock(session, &page_cache->hash_locks[bucket]);

    *page_cache_retp = page_cache_store;
    cache_inserted = true;
    __page_cache_verbose(
      session, WT_VERBOSE_DEBUG_1, "block inserted in cache", hash, addr, addr_size);
done:
    time_stop = __wt_clock(session);
    time_diff = WT_CLOCKDIFF_US(time_stop, time_start);
    if (page_cache->max_put_time < time_diff) {
        page_cache->max_put_time = time_diff;
        WT_STAT_CONN_SET(session, page_cache_max_put_time, time_diff);
    }
    WT_STAT_CONN_INCR(session, page_cache_total_page_count);
    WT_STAT_CONN_INCRV(session, page_cache_total_op_time, time_diff);
    WT_STAT_CONN_INCR(session, page_cache_total_op);
    if (cache_inserted)
        WT_STAT_CONN_INCR(session, page_cache_total_entry_count);
err:
    if (!cache_inserted) {
        __wt_free(session, data_ptr);
        __wt_free(session, block_meta_ptr);
        __wt_free(session, page_cache_store);
    }
    return (ret);
}

/*
 * __wt_page_cache_release --
 *     Remove a block from the cache.
 */
void
__wt_page_cache_release(WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size,
  WT_PAGE_CACHE_ITEM *page_cache_item)
{
    if (page_cache_item == NULL)
        return;
    WT_PAGE_CACHE *page_cache;
    uint64_t bucket, hash;
    uint64_t time_diff, time_start, time_stop;

    time_start = __wt_clock(session);

    page_cache = &S2C(session)->cache->page_cache;
    hash = __wt_hash_city64(addr, addr_size);
    bucket = __help_bucket(hash, page_cache->hash_size);

    __wt_spin_lock(session, &page_cache->hash_locks[bucket]);
    /* Remove the page cache when ref count is reduced to 0.*/
    if (--page_cache_item->ref_count == 0) {
        TAILQ_REMOVE(&page_cache->hash[bucket], page_cache_item, hashq);
        __wt_spin_unlock(session, &page_cache->hash_locks[bucket]);

        __wt_free(session, page_cache_item->block_meta);
        __wt_free(session, page_cache_item->data);
        __wt_free(session, page_cache_item);

        WT_STAT_CONN_DECR(session, page_cache_total_entry_count);
        time_stop = __wt_clock(session);
        time_diff = WT_CLOCKDIFF_US(time_stop, time_start);
        if (page_cache->max_release_time < time_diff) {
            page_cache->max_release_time = time_diff;
            WT_STAT_CONN_SET(session, page_cache_max_release_time, time_diff);
        }
        WT_STAT_CONN_INCRV(session, page_cache_total_op_time, time_diff);
        WT_STAT_CONN_INCR(session, page_cache_total_op);
        /* __page_cache_verbose(
           session, WT_VERBOSE_DEBUG_1, "page removed from cache", hash, addr, addr_size); */
    } else {
        __wt_spin_unlock(session, &page_cache->hash_locks[bucket]);
    }
    WT_STAT_CONN_DECR(session, page_cache_total_page_count);
    /* A page being released must always be present in the hash table. */
    /* Can we verbose an error log here or return some error code? */
}

/*
 * __wti_page_cache_init --
 *     Initialize the page cache.
 */
int
__wti_page_cache_init(WT_SESSION_IMPL *session, u_int hash_size)
{
    WT_PAGE_CACHE *page_cache;
    uint64_t i;

    if (hash_size == 0)
        hash_size = 10;
    if (hash_size > WT_MILLION)
        hash_size = WT_MILLION;
    page_cache = &S2C(session)->cache->page_cache;
    page_cache->hash_size = hash_size;
    page_cache->max_bucket_size = 0;
    page_cache->max_release_time = 0;
    page_cache->max_get_time = 0;
    page_cache->max_put_time = 0;
    page_cache->total_op_time = 0;
    page_cache->total_op = 0;
    page_cache->max_ref_count = 0;

    WT_RET(__wt_calloc_def(session, page_cache->hash_size, &page_cache->hash));
    WT_RET(__wt_calloc_def(session, page_cache->hash_size, &page_cache->hash_locks));

    for (i = 0; i < page_cache->hash_size; i++) {
        TAILQ_INIT(&page_cache->hash[i]); /* Block cache hash lists */
        WT_RET(__wt_spin_init(session, &page_cache->hash_locks[i], "page cache bucket locks"));
    }

    return (0);
}

/*
 * __wti_page_cache_destroy --
 *     Destroy the block cache and free all memory.
 */
void
__wti_page_cache_destroy(WT_SESSION_IMPL *session)
{
    /* fix me should release remaining cached pages? */
    WT_PAGE_CACHE *page_cache = &S2C(session)->cache->page_cache;
    WT_PAGE_CACHE_ITEM *page_cache_item;

    if (page_cache->hash == NULL || page_cache->hash_locks == NULL)
        return;

    for (uint64_t i = 0; i < page_cache->hash_size; i++) {
        __wt_spin_lock(session, &page_cache->hash_locks[i]);
        while (!TAILQ_EMPTY(&page_cache->hash[i])) {
            page_cache_item = TAILQ_FIRST(&page_cache->hash[i]);
            TAILQ_REMOVE(&page_cache->hash[i], page_cache_item, hashq);
            __wt_free(session, page_cache_item->block_meta);
            __wt_free(session, page_cache_item->data);
            __wt_free(session, page_cache_item);
        }
        __wt_spin_unlock(session, &page_cache->hash_locks[i]);
        __wt_spin_destroy(session, &page_cache->hash_locks[i]);
    }

    __wt_free(session, page_cache->hash);
    __wt_free(session, page_cache->hash_locks);
}
