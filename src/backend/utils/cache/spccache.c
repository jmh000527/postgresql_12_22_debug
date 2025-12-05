/*-------------------------------------------------------------------------
 *
 * spccache.c
 *	  Tablespace cache management.
 *
 * We cache the parsed version of spcoptions for each tablespace to avoid
 * needing to reparse on every lookup.  Right now, there doesn't appear to
 * be a measurable performance gain from doing this, but that might change
 * in the future as we add more options.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/spccache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/reloptions.h"
#include "catalog/pg_tablespace.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "optimizer/optimizer.h"
#include "storage/bufmgr.h"
#include "utils/catcache.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/spccache.h"
#include "utils/syscache.h"


/* Hash table for information about each tablespace */
static HTAB *TableSpaceCacheHash = NULL;

typedef struct
{
	Oid			oid;			/* lookup key - must be first */
	TableSpaceOpts *opts;		/* options, or NULL if none */
} TableSpaceCacheEntry;


/*
 * InvalidateTableSpaceCacheCallback
 *		Flush all cache entries when pg_tablespace is updated.
 *
 * When pg_tablespace is updated, we must flush the cache entry at least
 * for that tablespace.  Currently, we just flush them all.  This is quick
 * and easy and doesn't cost much, since there shouldn't be terribly many
 * tablespaces, nor do we expect them to be frequently modified.
 */
static void
InvalidateTableSpaceCacheCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	TableSpaceCacheEntry *spc;

	hash_seq_init(&status, TableSpaceCacheHash);
	while ((spc = (TableSpaceCacheEntry *) hash_seq_search(&status)) != NULL)
	{
		if (spc->opts)
			pfree(spc->opts);
		if (hash_search(TableSpaceCacheHash,
						(void *) &spc->oid,
						HASH_REMOVE,
						NULL) == NULL)
			elog(ERROR, "hash table corrupted");
	}
}

/*
 * InitializeTableSpaceCache
 *		Initialize the tablespace cache.
 */
static void
InitializeTableSpaceCache(void)
{
	HASHCTL		ctl;

	/* Initialize the hash table. */
	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(TableSpaceCacheEntry);
	TableSpaceCacheHash =
		hash_create("TableSpace cache", 16, &ctl,
					HASH_ELEM | HASH_BLOBS);

	/* Make sure we've initialized CacheMemoryContext. */
	if (!CacheMemoryContext)
		CreateCacheMemoryContext();

	/* Watch for invalidation events. */
	CacheRegisterSyscacheCallback(TABLESPACEOID,
								  InvalidateTableSpaceCacheCallback,
								  (Datum) 0);
}

/*
 * get_tablespace
 *		Fetch TableSpaceCacheEntry structure for a specified table OID.
 *
 * Pointers returned by this function should not be stored, since a cache
 * flush will invalidate them.
 */
static TableSpaceCacheEntry *
get_tablespace(Oid spcid)
{
	TableSpaceCacheEntry *spc;
	HeapTuple	tp;
	TableSpaceOpts *opts;

	/*
	 * Since spcid is always from a pg_class tuple, InvalidOid implies the
	 * default.
	 */
	if (spcid == InvalidOid)
		spcid = MyDatabaseTableSpace;

	/* Find existing cache entry, if any. */
	if (!TableSpaceCacheHash)
		InitializeTableSpaceCache();
	spc = (TableSpaceCacheEntry *) hash_search(TableSpaceCacheHash,
											   (void *) &spcid,
											   HASH_FIND,
											   NULL);
	if (spc)
		return spc;

	/*
	 * Not found in TableSpace cache.  Check catcache.  If we don't find a
	 * valid HeapTuple, it must mean someone has managed to request tablespace
	 * details for a non-existent tablespace.  We'll just treat that case as
	 * if no options were specified.
	 */
	tp = SearchSysCache1(TABLESPACEOID, ObjectIdGetDatum(spcid));
	if (!HeapTupleIsValid(tp))
		opts = NULL;
	else
	{
		Datum		datum;
		bool		isNull;

		datum = SysCacheGetAttr(TABLESPACEOID,
								tp,
								Anum_pg_tablespace_spcoptions,
								&isNull);
		if (isNull)
			opts = NULL;
		else
		{
			bytea	   *bytea_opts = tablespace_reloptions(datum, false);

			opts = MemoryContextAlloc(CacheMemoryContext, VARSIZE(bytea_opts));
			memcpy(opts, bytea_opts, VARSIZE(bytea_opts));
		}
		ReleaseSysCache(tp);
	}

	/*
	 * Now create the cache entry.  It's important to do this only after
	 * reading the pg_tablespace entry, since doing so could cause a cache
	 * flush.
	 */
	spc = (TableSpaceCacheEntry *) hash_search(TableSpaceCacheHash,
											   (void *) &spcid,
											   HASH_ENTER,
											   NULL);
	spc->opts = opts;
	return spc;
}

/*
 * get_tablespace_page_costs
 *    获取给定表空间的随机和/或顺序页面访问成本
 *
 *    这些值不会被事务锁定，因此当使用这些值进行规划的SELECT仍在执行时，
 *    这些值可能会被更改。这意味着同一执行中的查询可能使用不同的成本估计值。
 *
 * 参数：
 *   spcid - 表空间的OID标识符
 *   spc_random_page_cost - 输出参数，用于存储表空间的随机页面访问成本
 *   spc_seq_page_cost - 输出参数，用于存储表空间的顺序页面访问成本
 *
 * 返回值：
 *   无直接返回值，结果通过输出参数返回
 */
void
get_tablespace_page_costs(Oid spcid,
                          double *spc_random_page_cost,
                          double *spc_seq_page_cost)
{
	// 从表空间缓存中获取指定OID的表空间缓存条目
	TableSpaceCacheEntry* spc = get_tablespace(spcid);

    // 断言：确保表空间缓存条目不为空
    Assert(spc != NULL);

    /* 处理随机页面访问成本 */
    if (spc_random_page_cost != NULL)  // 仅当请求随机页面成本时
    {
        // 如果表空间没有自定义选项或随机页面成本为负值，则使用系统默认值
        if (!spc->opts || spc->opts->random_page_cost < 0)
            *spc_random_page_cost = random_page_cost;  // 系统全局变量
        else
            // 否则使用表空间特定的随机页面成本值
            *spc_random_page_cost = spc->opts->random_page_cost;
    }

    /* 处理顺序页面访问成本 */
    if (spc_seq_page_cost != NULL)  // 仅当请求顺序页面成本时
    {
        // 如果表空间没有自定义选项或顺序页面成本为负值，则使用系统默认值
        if (!spc->opts || spc->opts->seq_page_cost < 0)
            *spc_seq_page_cost = seq_page_cost;  // 系统全局变量
        else
            // 否则使用表空间特定的顺序页面成本值
            *spc_seq_page_cost = spc->opts->seq_page_cost;
    }
}


/*
 * get_tablespace_io_concurrency
 *
 *		This value is not locked by the transaction, so this value may
 *		be changed while a SELECT that has used these values for planning
 *		is still executing.
 */
int
get_tablespace_io_concurrency(Oid spcid)
{
	TableSpaceCacheEntry *spc = get_tablespace(spcid);

	if (!spc->opts || spc->opts->effective_io_concurrency < 0)
		return effective_io_concurrency;
	else
		return spc->opts->effective_io_concurrency;
}
