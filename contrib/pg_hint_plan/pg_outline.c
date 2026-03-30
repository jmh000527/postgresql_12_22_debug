#define PG_HINT_PLAN_NORMALIZE_API
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"
#include "parser/analyze.h"
#include "parser/parser.h"
#include "parser/scansup.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"
#include "utils/varlena.h"

#include "normalize_query.h"
#include "pg_outline.h"

/* --- GUCs --- */
static bool	pg_outline_enable = true;

/* --- Internal bookkeeping --- */
typedef struct QueryNameEntry
{
	Query	   *query;		/* hash key */
	char		name[NAMEDATALEN];
} QueryNameEntry;

typedef struct QueryHintEntry
{
	Query	   *query;		/* hash key */
	char	   *hints;		/* allocated in outline_mcxt */
} QueryHintEntry;

typedef struct QueryNamingContext
{
	int			subquery_index;
	int			sublink_index;
	HTAB	   *name_map;
} QueryNamingContext;

static MemoryContext outline_mcxt = NULL;
static HTAB *outline_name_map = NULL;
static HTAB *outline_hint_map = NULL;
static Query *current_stmt_root = NULL;
static char *current_normalized_query = NULL;
static bool outline_loading = false;

static void assign_query_names(Query *query, QueryNamingContext *context);
static bool assign_query_names_sublink_walker(Node *node, QueryNamingContext *context);
static void reset_outline_state(void);
static void ensure_state(void);
static char *normalize_sql(const char *sql);
static void load_outline_hints(void);

/* SQL-callable helpers */
Datum pg_outline_create(PG_FUNCTION_ARGS);
Datum pg_outline_delete(PG_FUNCTION_ARGS);
Datum pg_outline_list(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_outline_create);
PG_FUNCTION_INFO_V1(pg_outline_delete);
PG_FUNCTION_INFO_V1(pg_outline_list);

void pg_outline_init_hooks(void);
void pg_outline_fini_hooks(void);

void
pg_outline_init_hooks(void)
{
	DefineCustomBoolVariable("pg_outline.enable",
							 "Enable pg_outline hint injection.",
							 NULL,
							 &pg_outline_enable,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	/* pg_hint_plan owns post_parse_analyze_hook; we keep only our state */
}

void
pg_outline_fini_hooks(void)
{
	/* nothing to do */
}

static void
ensure_state(void)
{
	HASHCTL		ctl;

	if (outline_mcxt == NULL)
	{
		outline_mcxt = AllocSetContextCreate(TopMemoryContext,
											 "pg_outline context",
											 ALLOCSET_SMALL_SIZES);
	}

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Query *);
	ctl.entrysize = sizeof(QueryNameEntry);
	ctl.hcxt = outline_mcxt;
	if (outline_name_map == NULL)
		outline_name_map = hash_create("pg_outline query names",
									   128, &ctl,
									   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	ctl.entrysize = sizeof(QueryHintEntry);
	if (outline_hint_map == NULL)
		outline_hint_map = hash_create("pg_outline query hints",
									   128, &ctl,
									   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static void
reset_outline_state(void)
{
	if (!outline_mcxt)
		return;

	hash_destroy(outline_name_map);
	hash_destroy(outline_hint_map);
	outline_name_map = outline_hint_map = NULL;

	if (current_normalized_query)
		pfree(current_normalized_query);
	current_normalized_query = NULL;

	MemoryContextReset(outline_mcxt);
}

/*
 * Helper: assign name to a Query and store it.
 */
static void
store_query_name(Query *query, const char *name, HTAB *map)
{
	bool		found;
	QueryNameEntry *entry;

	entry = (QueryNameEntry *) hash_search(map, &query, HASH_ENTER, &found);
	strlcpy(entry->name, name, sizeof(entry->name));
}

/*
 * Expression walker to capture SubLink subqueries.
 */
static bool
assign_query_names_sublink_walker(Node *node, QueryNamingContext *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, SubLink))
	{
		SubLink    *sublink = (SubLink *) node;
		Query	   *q = castNode(Query, sublink->subselect);
		char		namebuf[NAMEDATALEN];

		snprintf(namebuf, sizeof(namebuf), "sublink_%d",
				 context->sublink_index++);
		assign_query_names(q, context);
		store_query_name(q, namebuf, context->name_map);
	}
	return expression_tree_walker(node,
								  assign_query_names_sublink_walker,
								  (void *) context);
}

/*
 * Recursively assign names to queries (main, subquery_N, cte_*, sublink_N).
 */
static void
assign_query_names(Query *query, QueryNamingContext *context)
{
	ListCell   *lc;
	int			sub_idx = 0;

	if (query == NULL)
		return;

	/* CTEs */
	foreach(lc, query->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);
		Query	   *ctequery = castNode(Query, cte->ctequery);
		char		namebuf[NAMEDATALEN];

		snprintf(namebuf, sizeof(namebuf), "cte_%s", cte->ctename);
		assign_query_names(ctequery, context);
		store_query_name(ctequery, namebuf, context->name_map);
	}

	/* RTE subqueries */
	foreach(lc, query->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		if (rte->rtekind == RTE_SUBQUERY)
		{
			Query	   *subq = rte->subquery;
			char		namebuf[NAMEDATALEN];

			snprintf(namebuf, sizeof(namebuf), "subquery_%d", sub_idx++);
			assign_query_names(subq, context);
			store_query_name(subq, namebuf, context->name_map);
		}
	}

	/* SubLinks in expressions */
	assign_query_names_sublink_walker((Node *) query, context);
}

/*
 * Build normalized query string from raw SQL text.
 */
static char *
normalize_sql(const char *sql)
{
	pgssJumbleState jstate;
	int			len;

	jstate.jumble = NULL;
	jstate.jumble_len = 0;
	jstate.clocations = NULL;
	jstate.clocations_buf_size = 0;
	jstate.clocations_count = 0;
	jstate.highest_extern_param_id = 0;

	len = strlen(sql) + 1;
	return generate_normalized_query(&jstate, sql, 0, &len,
									 GetDatabaseEncoding());
}

/*
 * Load hints for the current statement into the hint map.
 */
static void
load_outline_hints(void)
{
	int			ret;
	bool		isnull;
	uint64		proc = 0;
	Oid			relid;
	Oid			nspid;

	if (!current_normalized_query)
		return;

	/* Do nothing until the catalog table exists (e.g., during extension install). */
	nspid = get_namespace_oid("pg_catalog", true);
	if (!OidIsValid(nspid))
		return;

	relid = get_relname_relid("pg_outline_query_hints", nspid);
	if (!OidIsValid(relid))
		return;

	/* Guard against recursion when we run SPI inside the post_parse hook. */
	if (outline_loading)
		return;

	outline_loading = true;

	PG_TRY();
	{
		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "pg_outline: SPI_connect failed");

		ret = SPI_execute_with_args(
									"SELECT query_name, hints "
									"FROM pg_catalog.pg_outline_query_hints "
									"WHERE normalized_query = $1",
									1,
									(Oid[]) {TEXTOID},
									(Datum[]) {CStringGetTextDatum(current_normalized_query)},
									NULL,
									true,
									0);
		if (ret != SPI_OK_SELECT)
			elog(ERROR, "pg_outline: failed to read outline table");

		for (proc = 0; proc < SPI_processed; proc++)
		{
			HeapTuple	tuple = SPI_tuptable->vals[proc];
			TupleDesc	tupdesc = SPI_tuptable->tupdesc;
			char	   *qname = TextDatumGetCString(SPI_getbinval(tuple, tupdesc, 1, &isnull));
			char	   *hints = TextDatumGetCString(SPI_getbinval(tuple, tupdesc, 2, &isnull));
			HASH_SEQ_STATUS status;
			QueryNameEntry *entry;

			/* Find the matching Query by name */
			hash_seq_init(&status, outline_name_map);
			while ((entry = (QueryNameEntry *) hash_seq_search(&status)) != NULL)
			{
				if (strcmp(entry->name, qname) == 0)
				{
					bool		found;
					QueryHintEntry *hentry;

					hentry = (QueryHintEntry *) hash_search(outline_hint_map,
															&entry->query,
															HASH_ENTER,
															&found);
					if (!found || hentry->hints == NULL)
						hentry->hints = MemoryContextStrdup(outline_mcxt, hints);
					break;
				}
			}

			pfree(qname);
			pfree(hints);
		}

		SPI_finish();
	}
	PG_CATCH();
	{
		outline_loading = false;
		PG_RE_THROW();
	}
	PG_END_TRY();

	outline_loading = false;
}

void
pg_outline_post_parse(ParseState *pstate, Query *query)
{
	QueryNamingContext ctx;

	if (!pg_outline_enable || outline_loading || query == NULL)
		return;

	/* We only care about plannable statements, skip utilities like CREATE EXTENSION. */
	if (query->commandType == CMD_UTILITY)
		return;

	reset_outline_state();
	ensure_state();

	current_stmt_root = query;

	/* Capture raw SQL text for normalization */
	if (pstate && pstate->p_sourcetext)
		current_normalized_query = normalize_sql(pstate->p_sourcetext);
	else if (debug_query_string)
		current_normalized_query = normalize_sql(debug_query_string);

	/* Build name map */
	store_query_name(query, "main", outline_name_map);
	ctx.subquery_index = 0;
	ctx.sublink_index = 0;
	ctx.name_map = outline_name_map;
	assign_query_names(query, &ctx);

	/* Load hints and map them */
	load_outline_hints();
}

/*
 * Lookup hint string for a Query pointer.
 */
const char *
pg_outline_get_hint(Query *query)
{
	QueryHintEntry *entry;

	if (!pg_outline_enable || outline_hint_map == NULL)
		return NULL;

	entry = (QueryHintEntry *) hash_search(outline_hint_map,
										   &query,
										   HASH_FIND,
										   NULL);
	if (entry == NULL)
		return NULL;
	return entry->hints;
}

const char *
pg_outline_get_query_name(Query *query)
{
	QueryNameEntry *entry;

	if (!outline_name_map)
		return NULL;

	entry = (QueryNameEntry *) hash_search(outline_name_map,
										   &query,
										   HASH_FIND,
										   NULL);
	if (entry == NULL)
		return NULL;
	return entry->name;
}

/*
 * SQL: create outline.
 */
Datum
pg_outline_create(PG_FUNCTION_ARGS)
{
	text	   *out_name = PG_GETARG_TEXT_PP(0);
	text	   *sql = PG_GETARG_TEXT_PP(1);
	text	   *query_name = PG_GETARG_TEXT_PP(2);
	text	   *hints = PG_GETARG_TEXT_PP(3);
	char	   *norm;
	Oid			argtypes[4] = {TEXTOID, TEXTOID, TEXTOID, TEXTOID};
	Datum		values[4];
	const char *cmd =
		"INSERT INTO pg_catalog.pg_outline_query_hints "
		"(outline_name, normalized_query, query_name, hints) "
		"VALUES ($1, $2, $3, $4) "
		"ON CONFLICT (outline_name, query_name) DO UPDATE "
		"SET hints = EXCLUDED.hints, normalized_query = EXCLUDED.normalized_query";

	norm = normalize_sql(text_to_cstring(sql));
	values[0] = PointerGetDatum(out_name);
	values[1] = CStringGetTextDatum(norm);
	values[2] = PointerGetDatum(query_name);
	values[3] = PointerGetDatum(hints);

	outline_loading = true;
	PG_TRY();
	{
		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "pg_outline: SPI_connect failed");

		if (SPI_execute_with_args(cmd, 4, argtypes, values, NULL, false, 0) != SPI_OK_INSERT)
			elog(ERROR, "pg_outline: failed to insert outline");

		SPI_finish();
	}
	PG_CATCH();
	{
		outline_loading = false;
		PG_RE_THROW();
	}
	PG_END_TRY();
	outline_loading = false;
	pfree(norm);

	PG_RETURN_VOID();
}

Datum
pg_outline_delete(PG_FUNCTION_ARGS)
{
	text	   *out_name = PG_GETARG_TEXT_PP(0);
	Oid			argtypes[1] = {TEXTOID};
	Datum		values[1];

	values[0] = PointerGetDatum(out_name);

	outline_loading = true;
	PG_TRY();
	{
		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "pg_outline: SPI_connect failed");

		if (SPI_execute_with_args("DELETE FROM pg_catalog.pg_outline_query_hints WHERE outline_name = $1",
								  1, argtypes, values, NULL, false, 0) < 0)
			elog(ERROR, "pg_outline: delete failed");

		SPI_finish();
	}
	PG_CATCH();
	{
		outline_loading = false;
		PG_RE_THROW();
	}
	PG_END_TRY();
	outline_loading = false;
	PG_RETURN_VOID();
}

Datum
pg_outline_list(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	TupleDesc	tupdesc;
	SPITupleTable *tuptable;
	int			ret;
	uint64		i;

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));

	if (rsinfo->allowedModes & SFRM_Materialize)
		rsinfo->returnMode = SFRM_Materialize;
	else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupdesc = CreateTemplateTupleDesc(5);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "outline_name", TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "query_name", TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "normalized_query", TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "hints", TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "created_at", TIMESTAMPTZOID, -1, 0);
	rsinfo->setDesc = tupdesc;
	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->setResult = tupstore;

	outline_loading = true;
	PG_TRY();
	{
		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "pg_outline: SPI_connect failed");

		ret = SPI_execute("SELECT outline_name, query_name, normalized_query, hints, created_at "
						  "FROM pg_catalog.pg_outline_query_hints ORDER BY outline_name, query_name",
						  true, 0);
		if (ret != SPI_OK_SELECT)
			elog(ERROR, "pg_outline: list failed");

		tuptable = SPI_tuptable;
		for (i = 0; i < SPI_processed; i++)
		{
			Datum		values[5];
			bool		nulls[5] = {false, false, false, false, false};
			HeapTuple	tuple;

			tuple = tuptable->vals[i];
			values[0] = SPI_getbinval(tuple, tuptable->tupdesc, 1, &nulls[0]);
			values[1] = SPI_getbinval(tuple, tuptable->tupdesc, 2, &nulls[1]);
			values[2] = SPI_getbinval(tuple, tuptable->tupdesc, 3, &nulls[2]);
			values[3] = SPI_getbinval(tuple, tuptable->tupdesc, 4, &nulls[3]);
			values[4] = SPI_getbinval(tuple, tuptable->tupdesc, 5, &nulls[4]);

			tuplestore_putvalues(tupstore, tupdesc, values, nulls);
		}

		SPI_finish();
	}
	PG_CATCH();
	{
		outline_loading = false;
		PG_RE_THROW();
	}
	PG_END_TRY();
	outline_loading = false;
	MemoryContextSwitchTo(oldcontext);

	tuplestore_donestoring(tupstore);
	return (Datum) 0;
}
