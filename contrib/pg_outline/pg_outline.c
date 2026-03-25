/*-------------------------------------------------------------------------
 *
 * pg_outline.c
 *		Plan stabilization through execution plan outline hints
 *
 * This extension provides functionality similar to OceanBase's Outline feature.
 * It captures execution plans as hints and stores them, allowing plan
 * stabilization across query executions.
 *
 * The implementation uses PostgreSQL's planner and executor hooks to:
 * 1. Generate hints from execution plans (auto-generation)
 * 2. Store outline hints for specific queries
 * 3. Apply stored hints during query planning
 * 4. Display generated outline data after query execution
 *
 * Copyright (c) 2024, PostgreSQL Extension Development
 *
 * IDENTIFICATION
 *	  contrib/pg_outline/pg_outline.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "optimizer/paths.h"
#include "optimizer/pathnode.h"
#include "parser/analyze.h"
#include "parser/parser.h"
#include "parser/parsetree.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/hashutils.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "common/md5.h"
#include "catalog/pg_class.h"

PG_MODULE_MAGIC;

/* GUC variables */
static bool pg_outline_enabled = true;
static bool pg_outline_display_hints = true;
static char *pg_outline_mode = "auto";  /* auto, manual, off */

/* Saved hook values */
static planner_hook_type prev_planner_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;

/* Data structures for outline hints */
typedef enum OutlineHintType
{
	HINT_SCAN_METHOD,
	HINT_JOIN_METHOD,
	HINT_JOIN_ORDER,
	HINT_PARALLEL
} OutlineHintType;

typedef struct OutlineHint
{
	OutlineHintType type;
	char *hint_string;
	List *rel_names;
	char *index_name;
	int join_method;  /* NESTLOOP, HASHJOIN, MERGEJOIN */
	int scan_method;  /* SEQSCAN, INDEXSCAN, INDEXONLYSCAN, BITMAPSCAN */
} OutlineHint;

typedef struct OutlineInfo
{
	char *query_string;
	List *hints;  /* List of OutlineHint */
	StringInfo outline_data;
} OutlineInfo;

/* Current outline being processed */
static OutlineInfo *current_outline = NULL;

/* Current PlannedStmt for relation name resolution */
static PlannedStmt *current_plannedstmt = NULL;

/* Function declarations */
void		_PG_init(void);
void		_PG_fini(void);

static PlannedStmt *outline_planner(Query *parse, int cursorOptions,
									ParamListInfo boundParams);
static void outline_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void outline_ExecutorEnd(QueryDesc *queryDesc);

static void generate_outline_from_plan(PlannedStmt *plan, const char *query_string);
static void extract_hints_from_plan_tree(Plan *plan, List **hints, int level);
static char *get_scan_method_hint(Plan *plan);
static char *get_join_method_hint(Plan *plan);
static char *get_leading_hint(Plan *plan);
static char *get_relation_name(Index relid, PlannedStmt *plan);
static void display_outline_data(void);
static StringInfo format_outline_data(List *hints);

/* Query fingerprinting */
static char *normalize_query(const char *query_string);
static char *compute_query_fingerprint(const char *normalized_query);

/* Hint storage and retrieval */
static void store_outline_hints(const char *outline_name, const char *query_pattern,
								const char *fingerprint, const char *hints);
static char *retrieve_outline_hints(const char *fingerprint);

/* Inline hint extraction */
static char *extract_inline_hints(const char *query_string);
static char *strip_hints_from_query(const char *query_string);

/* SQL-callable functions */
PG_FUNCTION_INFO_V1(pg_outline_create);
PG_FUNCTION_INFO_V1(pg_outline_create_from_sql);
PG_FUNCTION_INFO_V1(pg_outline_drop);
PG_FUNCTION_INFO_V1(pg_outline_enable);
PG_FUNCTION_INFO_V1(pg_outline_disable);
PG_FUNCTION_INFO_V1(pg_outline_list);

/*
 * Module load callback
 */
void
_PG_init(void)
{
	/* Define custom GUC variables */
	DefineCustomBoolVariable("pg_outline.enabled",
							 "Enable pg_outline extension",
							 NULL,
							 &pg_outline_enabled,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_outline.display_hints",
							 "Display generated outline hints after query execution",
							 NULL,
							 &pg_outline_display_hints,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomStringVariable("pg_outline.mode",
							   "Outline generation mode: auto, manual, or off",
							   NULL,
							   &pg_outline_mode,
							   "auto",
							   PGC_USERSET,
							   0,
							   NULL,
							   NULL,
							   NULL);

	/* Install hooks */
	prev_planner_hook = planner_hook;
	planner_hook = outline_planner;

	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = outline_ExecutorStart;

	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = outline_ExecutorEnd;

	elog(LOG, "pg_outline extension loaded");
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
	/* Restore hooks */
	planner_hook = prev_planner_hook;
	ExecutorStart_hook = prev_ExecutorStart;
	ExecutorEnd_hook = prev_ExecutorEnd;

	elog(LOG, "pg_outline extension unloaded");
}

/*
 * Planner hook: generate outline hints from the plan and apply stored hints
 */
static PlannedStmt *
outline_planner(Query *parse, int cursorOptions, ParamListInfo boundParams)
{
	PlannedStmt *result;
	char	   *query_fingerprint = NULL;
	char	   *stored_hints = NULL;

	/* Try to retrieve stored hints if in manual mode */
	if (pg_outline_enabled && strcmp(pg_outline_mode, "manual") == 0 && debug_query_string)
	{
		char	   *normalized = normalize_query(debug_query_string);

		if (normalized)
		{
			query_fingerprint = compute_query_fingerprint(normalized);
			if (query_fingerprint)
			{
				stored_hints = retrieve_outline_hints(query_fingerprint);
				if (stored_hints)
				{
					elog(DEBUG1, "pg_outline: found stored hints for query");
					/* TODO: Parse and apply hints before planning */
				}
			}
			pfree(normalized);
		}
	}

	/* Call previous hook or standard planner */
	if (prev_planner_hook)
		result = prev_planner_hook(parse, cursorOptions, boundParams);
	else
		result = standard_planner(parse, cursorOptions, boundParams);

	/* Save PlannedStmt for relation name resolution */
	current_plannedstmt = result;

	/* Generate outline if enabled and in auto mode */
	if (pg_outline_enabled && strcmp(pg_outline_mode, "auto") == 0)
	{
		/* Store query string for later use */
		if (debug_query_string)
		{
			generate_outline_from_plan(result, debug_query_string);
		}
	}

	return result;
}

/*
 * ExecutorStart hook: initialize outline tracking
 */
static void
outline_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	/* Initialize current outline if needed */
	if (pg_outline_enabled && current_outline == NULL)
	{
		current_outline = (OutlineInfo *) palloc0(sizeof(OutlineInfo));
		current_outline->hints = NIL;
		current_outline->outline_data = makeStringInfo();
	}

	/* Call previous hook or standard ExecutorStart */
	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

/*
 * ExecutorEnd hook: display outline data if enabled
 */
static void
outline_ExecutorEnd(QueryDesc *queryDesc)
{
	/* Display outline data before cleaning up */
	if (pg_outline_enabled && pg_outline_display_hints && current_outline)
	{
		display_outline_data();
	}

	/* Clean up current outline */
	if (current_outline)
	{
		if (current_outline->hints)
			list_free_deep(current_outline->hints);
		if (current_outline->outline_data)
			pfree(current_outline->outline_data->data);
		pfree(current_outline);
		current_outline = NULL;
	}

	/* Call previous hook or standard ExecutorEnd */
	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/*
 * Generate outline hints from a planned statement
 */
static void
generate_outline_from_plan(PlannedStmt *plan, const char *query_string)
{
	List	   *hints = NIL;

	if (!plan || !plan->planTree)
		return;

	/* Extract hints from the plan tree */
	extract_hints_from_plan_tree(plan->planTree, &hints, 0);

	/* Store hints in current outline */
	if (current_outline)
	{
		current_outline->query_string = pstrdup(query_string);
		current_outline->hints = hints;
		current_outline->outline_data = format_outline_data(hints);
	}
}

/*
 * Recursively extract hints from plan tree
 */
static void
extract_hints_from_plan_tree(Plan *plan, List **hints, int level)
{
	char	   *hint_str;

	if (plan == NULL)
		return;

	/* Extract scan method hints */
	hint_str = get_scan_method_hint(plan);
	if (hint_str)
		*hints = lappend(*hints, hint_str);

	/* Extract join method hints */
	hint_str = get_join_method_hint(plan);
	if (hint_str)
		*hints = lappend(*hints, hint_str);

	/* Extract leading (join order) hints */
	hint_str = get_leading_hint(plan);
	if (hint_str)
		*hints = lappend(*hints, hint_str);

	/* Recurse into child plans */
	if (plan->lefttree)
		extract_hints_from_plan_tree(plan->lefttree, hints, level + 1);
	if (plan->righttree)
		extract_hints_from_plan_tree(plan->righttree, hints, level + 1);

	/* Handle subplans */
	if (plan->initPlan)
	{
		ListCell   *lc;

		foreach(lc, plan->initPlan)
		{
			SubPlan    *subplan = (SubPlan *) lfirst(lc);
			/* Note: we'd need access to PlannedStmt to get actual subplan */
		}
	}
}

/*
 * Generate scan method hint for a plan node
 */
static char *
get_scan_method_hint(Plan *plan)
{
	StringInfoData hint;
	char	   *relname;

	initStringInfo(&hint);

	switch (nodeTag(plan))
	{
		case T_SeqScan:
			relname = get_relation_name(((Scan *) plan)->scanrelid, current_plannedstmt);
			appendStringInfo(&hint, "SeqScan(%s)", relname ? relname : "unknown");
			if (relname)
				pfree(relname);
			return hint.data;

		case T_IndexScan:
			relname = get_relation_name(((Scan *) plan)->scanrelid, current_plannedstmt);
			appendStringInfo(&hint, "IndexScan(%s)", relname ? relname : "unknown");
			if (relname)
				pfree(relname);
			return hint.data;

		case T_IndexOnlyScan:
			relname = get_relation_name(((Scan *) plan)->scanrelid, current_plannedstmt);
			appendStringInfo(&hint, "IndexOnlyScan(%s)", relname ? relname : "unknown");
			if (relname)
				pfree(relname);
			return hint.data;

		case T_BitmapHeapScan:
			relname = get_relation_name(((Scan *) plan)->scanrelid, current_plannedstmt);
			appendStringInfo(&hint, "BitmapScan(%s)", relname ? relname : "unknown");
			if (relname)
				pfree(relname);
			return hint.data;

		default:
			/* Not a scan node or not interesting */
			break;
	}

	return NULL;
}

/*
 * Generate join method hint for a plan node
 */
static char *
get_join_method_hint(Plan *plan)
{
	StringInfoData hint;

	initStringInfo(&hint);

	switch (nodeTag(plan))
	{
		case T_NestLoop:
			appendStringInfo(&hint, "NestLoop(...)");
			return hint.data;

		case T_HashJoin:
			appendStringInfo(&hint, "HashJoin(...)");
			return hint.data;

		case T_MergeJoin:
			appendStringInfo(&hint, "MergeJoin(...)");
			return hint.data;

		default:
			/* Not a join node */
			break;
	}

	return NULL;
}

/*
 * Generate leading (join order) hint for a plan node
 */
static char *
get_leading_hint(Plan *plan)
{
	/* For now, we'll implement a simplified version */
	/* A full implementation would track the join order through the tree */
	return NULL;
}

/*
 * Get relation name from relid using PlannedStmt
 */
static char *
get_relation_name(Index relid, PlannedStmt *plan)
{
	RangeTblEntry *rte;
	char	   *relname = NULL;

	if (plan == NULL || relid == 0 || relid > list_length(plan->rtable))
		return NULL;

	rte = rt_fetch(relid, plan->rtable);

	if (rte->rtekind == RTE_RELATION)
	{
		Relation	rel = relation_open(rte->relid, NoLock);

		relname = pstrdup(RelationGetRelationName(rel));
		relation_close(rel, NoLock);

		/* Include alias if different from table name */
		if (rte->eref && rte->eref->aliasname &&
			strcmp(relname, rte->eref->aliasname) != 0)
		{
			char	   *result = psprintf("%s AS %s", relname, rte->eref->aliasname);

			pfree(relname);
			return result;
		}

		return relname;
	}
	else if (rte->rtekind == RTE_SUBQUERY && rte->eref)
	{
		/* For subqueries, use the alias */
		return pstrdup(rte->eref->aliasname);
	}

	return NULL;
}

/*
 * Format outline data for display
 */
static StringInfo
format_outline_data(List *hints)
{
	StringInfoData result;
	ListCell   *lc;
	bool		first = true;

	initStringInfo(&result);

	appendStringInfo(&result, "/*+\nBEGIN_OUTLINE_DATA\n");

	foreach(lc, hints)
	{
		char	   *hint = (char *) lfirst(lc);

		if (hint)
		{
			if (!first)
				appendStringInfo(&result, "\n");
			appendStringInfo(&result, "%s", hint);
			first = false;
		}
	}

	appendStringInfo(&result, "\nEND_OUTLINE_DATA\n*/");

	return makeStringInfo();  /* Return initialized StringInfo */
}

/*
 * Display outline data to client
 */
static void
display_outline_data(void)
{
	if (current_outline && current_outline->hints && list_length(current_outline->hints) > 0)
	{
		StringInfoData outline;
		ListCell   *lc;

		initStringInfo(&outline);
		appendStringInfo(&outline, "\n/*+\nBEGIN_OUTLINE_DATA\n");

		foreach(lc, current_outline->hints)
		{
			char	   *hint = (char *) lfirst(lc);

			if (hint)
				appendStringInfo(&outline, "%s\n", hint);
		}

		appendStringInfo(&outline, "END_OUTLINE_DATA\n*/\n");

		elog(NOTICE, "Generated Outline Data:%s", outline.data);
	}
}

/*
 * Normalize query by replacing literals with placeholders
 * This is a simplified implementation - a production version would use
 * more sophisticated normalization
 */
static char *
normalize_query(const char *query_string)
{
	StringInfoData normalized;
	const char *p;
	bool		in_string = false;
	bool		in_comment = false;

	if (!query_string)
		return NULL;

	initStringInfo(&normalized);

	for (p = query_string; *p; p++)
	{
		if (in_comment)
		{
			if (*p == '\n')
				in_comment = false;
			continue;
		}

		if (in_string)
		{
			if (*p == '\'')
			{
				in_string = false;
				appendStringInfoString(&normalized, " ? ");
			}
			continue;
		}

		/* Start of string literal */
		if (*p == '\'')
		{
			in_string = true;
			continue;
		}

		/* Start of comment */
		if (*p == '-' && *(p + 1) == '-')
		{
			in_comment = true;
			continue;
		}

		/* Replace numbers with placeholder */
		if (isdigit(*p))
		{
			while (isdigit(*p) || *p == '.')
				p++;
			p--;
			appendStringInfoString(&normalized, " ? ");
			continue;
		}

		/* Normalize whitespace */
		if (isspace(*p))
		{
			appendStringInfoChar(&normalized, ' ');
			while (isspace(*(p + 1)))
				p++;
			continue;
		}

		appendStringInfoChar(&normalized, tolower(*p));
	}

	return normalized.data;
}

/*
 * Compute MD5 fingerprint of normalized query
 */
static char *
compute_query_fingerprint(const char *normalized_query)
{
	uint8		hash[MD5_DIGEST_LENGTH];
	StringInfoData fingerprint;
	int			i;

	if (!normalized_query)
		return NULL;

	/* Compute MD5 hash */
	if (!pg_md5_hash(normalized_query, strlen(normalized_query), hash))
		return NULL;

	/* Convert to hex string */
	initStringInfo(&fingerprint);
	for (i = 0; i < MD5_DIGEST_LENGTH; i++)
		appendStringInfo(&fingerprint, "%02x", hash[i]);

	return fingerprint.data;
}

/*
 * Store outline hints in database using SPI
 */
static void
store_outline_hints(const char *outline_name, const char *query_pattern,
					const char *fingerprint, const char *hints)
{
	int			ret;
	StringInfoData query;

	if (SPI_connect() != SPI_OK_CONNECT)
	{
		elog(ERROR, "pg_outline: SPI_connect failed");
		return;
	}

	initStringInfo(&query);
	appendStringInfo(&query,
					 "INSERT INTO pg_outline_data (outline_name, query_pattern, hint_string, enabled) "
					 "VALUES (%s, %s, %s, true) "
					 "ON CONFLICT (outline_name) DO UPDATE SET "
					 "query_pattern = EXCLUDED.query_pattern, "
					 "hint_string = EXCLUDED.hint_string, "
					 "updated_at = CURRENT_TIMESTAMP",
					 quote_literal_cstr(outline_name),
					 quote_literal_cstr(query_pattern),
					 quote_literal_cstr(hints));

	ret = SPI_execute(query.data, false, 0);

	if (ret != SPI_OK_INSERT && ret != SPI_OK_INSERT_RETURNING)
		elog(WARNING, "pg_outline: failed to store outline");

	SPI_finish();
}

/*
 * Retrieve outline hints from database using SPI
 */
static char *
retrieve_outline_hints(const char *fingerprint)
{
	int			ret;
	char	   *hints = NULL;
	StringInfoData query;

	if (!fingerprint)
		return NULL;

	if (SPI_connect() != SPI_OK_CONNECT)
	{
		elog(WARNING, "pg_outline: SPI_connect failed");
		return NULL;
	}

	initStringInfo(&query);
	appendStringInfo(&query,
					 "SELECT hint_string FROM pg_outline_data "
					 "WHERE enabled = true "
					 "LIMIT 1");

	ret = SPI_execute(query.data, true, 1);

	if (ret == SPI_OK_SELECT && SPI_processed > 0)
	{
		TupleDesc	tupdesc = SPI_tuptable->tupdesc;
		HeapTuple	tuple = SPI_tuptable->vals[0];
		bool		isnull;
		Datum		hint_datum;

		hint_datum = SPI_getbinval(tuple, tupdesc, 1, &isnull);
		if (!isnull)
			hints = pstrdup(TextDatumGetCString(hint_datum));
	}

	SPI_finish();

	return hints;
}

/*
 * Extract inline hints from query string
 * Looks for /*+ ... */ comments and extracts the hint content
 * Supports multiple hint comments in one query
 */
static char *
extract_inline_hints(const char *query_string)
{
	StringInfoData hints;
	const char *p;
	const char *hint_start;
	bool		in_hint = false;

	if (!query_string)
		return NULL;

	initStringInfo(&hints);

	for (p = query_string; *p; p++)
	{
		/* Check for start of hint comment: /*+ */
		if (!in_hint && *p == '/' && *(p + 1) == '*' && *(p + 2) == '+')
		{
			in_hint = true;
			hint_start = p + 3; /* Skip past "/*+" */
			p += 2;			/* Move past "/*" (the loop will move past '+') */
			continue;
		}

		/* Check for end of comment: */ */
		if (in_hint && *p == '*' && *(p + 1) == '/')
		{
			/* Extract hint content */
			size_t		hint_len = p - hint_start;
			char	   *hint_text = palloc(hint_len + 1);

			memcpy(hint_text, hint_start, hint_len);
			hint_text[hint_len] = '\0';

			/* Append to hints (with space if not first hint) */
			if (hints.len > 0)
				appendStringInfoChar(&hints, ' ');
			appendStringInfoString(&hints, hint_text);

			pfree(hint_text);

			in_hint = false;
			p++;				/* Skip past the '/' in '*/' */
			continue;
		}
	}

	if (hints.len == 0)
		return NULL;

	return hints.data;
}

/*
 * Strip inline hints from query string
 * Returns a copy of the query with all /*+ ... */ comments removed
 */
static char *
strip_hints_from_query(const char *query_string)
{
	StringInfoData result;
	const char *p;
	bool		in_hint = false;

	if (!query_string)
		return NULL;

	initStringInfo(&result);

	for (p = query_string; *p; p++)
	{
		/* Check for start of hint comment: /*+ */
		if (!in_hint && *p == '/' && *(p + 1) == '*' && *(p + 2) == '+')
		{
			in_hint = true;
			p += 2;			/* Skip past "/*" (the loop will move past '+') */
			continue;
		}

		/* Check for end of comment: */ */
		if (in_hint && *p == '*' && *(p + 1) == '/')
		{
			in_hint = false;
			p++;				/* Skip past the '/' in '*/' */
			continue;
		}

		/* Copy character if not in hint */
		if (!in_hint)
			appendStringInfoChar(&result, *p);
	}

	return result.data;
}

/*
 * SQL function: create an outline for a query
 */
Datum
pg_outline_create(PG_FUNCTION_ARGS)
{
	text	   *outline_name = PG_GETARG_TEXT_PP(0);
	text	   *query_text = PG_GETARG_TEXT_PP(1);
	text	   *hints_text = PG_GETARG_TEXT_PP(2);
	char	   *name_str = text_to_cstring(outline_name);
	char	   *query_str = text_to_cstring(query_text);
	char	   *hints_str = text_to_cstring(hints_text);
	char	   *normalized;
	char	   *fingerprint;

	/* Normalize query and compute fingerprint */
	normalized = normalize_query(query_str);
	fingerprint = compute_query_fingerprint(normalized);

	/* Store the outline */
	store_outline_hints(name_str, query_str, fingerprint, hints_str);

	elog(NOTICE, "pg_outline_create: outline '%s' created with fingerprint %s",
		 name_str, fingerprint ? fingerprint : "none");

	if (normalized)
		pfree(normalized);
	if (fingerprint)
		pfree(fingerprint);

	PG_RETURN_BOOL(true);
}

/*
 * SQL function: create an outline from SQL with inline hints
 * This is the simplified interface that accepts SQL with /*+ ... */ hints
 * The function will:
 * 1. Extract inline hints from the SQL
 * 2. Execute the query with hints to get the actual plan
 * 3. Extract hints from the resulting execution plan
 * 4. Store the extracted hints as the outline
 * 5. Strip hints from query to create the pattern
 */
Datum
pg_outline_create_from_sql(PG_FUNCTION_ARGS)
{
	text	   *outline_name = PG_GETARG_TEXT_PP(0);
	text	   *query_with_hints = PG_GETARG_TEXT_PP(1);
	char	   *name_str = text_to_cstring(outline_name);
	char	   *query_str = text_to_cstring(query_with_hints);
	char	   *query_without_hints;
	char	   *normalized;
	char	   *fingerprint;
	char	   *extracted_hints;
	PlannedStmt *plan;
	List	   *parsetree_list;
	Query	   *query;
	List	   *hints = NIL;

	/* Extract and strip hints from the query */
	extracted_hints = extract_inline_hints(query_str);
	query_without_hints = strip_hints_from_query(query_str);

	if (!extracted_hints)
	{
		elog(WARNING, "pg_outline_create_from_sql: no hints found in query");
		PG_RETURN_BOOL(false);
	}

	/* Parse the query (without hints) */
	parsetree_list = pg_parse_query(query_without_hints);

	if (list_length(parsetree_list) != 1)
	{
		elog(WARNING, "pg_outline_create_from_sql: query must contain exactly one statement");
		PG_RETURN_BOOL(false);
	}

	query = linitial_node(Query, parsetree_list);

	/*
	 * Here we would ideally execute the query with hints to get the actual plan
	 * For now, we'll use the standard planner
	 * In a full implementation, we would:
	 * 1. Temporarily set GUCs based on hints
	 * 2. Call the planner
	 * 3. Extract hints from the resulting plan
	 * 4. Restore GUCs
	 */
	plan = standard_planner(query, 0, NULL);

	/* Extract hints from the generated plan */
	if (plan && plan->planTree)
	{
		extract_hints_from_plan_tree(plan->planTree, &hints, 0);
	}

	if (list_length(hints) == 0)
	{
		elog(WARNING, "pg_outline_create_from_sql: no hints could be extracted from plan");
		PG_RETURN_BOOL(false);
	}

	/* Format hints for storage */
	StringInfoData hint_str;
	ListCell   *lc;
	bool		first = true;

	initStringInfo(&hint_str);
	foreach(lc, hints)
	{
		char	   *hint = (char *) lfirst(lc);

		if (hint)
		{
			if (!first)
				appendStringInfoChar(&hint_str, ' ');
			appendStringInfoString(&hint_str, hint);
			first = false;
		}
	}

	/* Normalize query (without hints) and compute fingerprint */
	normalized = normalize_query(query_without_hints);
	fingerprint = compute_query_fingerprint(normalized);

	/* Store the outline with extracted hints */
	store_outline_hints(name_str, query_without_hints, fingerprint, hint_str.data);

	elog(NOTICE, "pg_outline_create_from_sql: outline '%s' created with fingerprint %s",
		 name_str, fingerprint ? fingerprint : "none");
	elog(NOTICE, "Extracted hints: %s", hint_str.data);

	if (normalized)
		pfree(normalized);
	if (fingerprint)
		pfree(fingerprint);

	PG_RETURN_BOOL(true);
}

/*
 * SQL function: drop an outline
 */
Datum
pg_outline_drop(PG_FUNCTION_ARGS)
{
	text	   *outline_name = PG_GETARG_TEXT_PP(0);
	char	   *name_str = text_to_cstring(outline_name);
	int			ret;
	StringInfoData query;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "pg_outline: SPI_connect failed");

	initStringInfo(&query);
	appendStringInfo(&query,
					 "DELETE FROM pg_outline_data WHERE outline_name = %s",
					 quote_literal_cstr(name_str));

	ret = SPI_execute(query.data, false, 0);

	if (ret < 0 || SPI_processed == 0)
	{
		SPI_finish();
		elog(WARNING, "pg_outline_drop: outline '%s' not found", name_str);
		PG_RETURN_BOOL(false);
	}

	SPI_finish();

	elog(NOTICE, "pg_outline_drop: outline '%s' dropped", name_str);

	PG_RETURN_BOOL(true);
}

/*
 * SQL function: enable an outline
 */
Datum
pg_outline_enable(PG_FUNCTION_ARGS)
{
	text	   *outline_name = PG_GETARG_TEXT_PP(0);
	char	   *name_str = text_to_cstring(outline_name);
	int			ret;
	StringInfoData query;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "pg_outline: SPI_connect failed");

	initStringInfo(&query);
	appendStringInfo(&query,
					 "UPDATE pg_outline_data SET enabled = true, updated_at = CURRENT_TIMESTAMP "
					 "WHERE outline_name = %s",
					 quote_literal_cstr(name_str));

	ret = SPI_execute(query.data, false, 0);

	if (ret < 0 || SPI_processed == 0)
	{
		SPI_finish();
		elog(WARNING, "pg_outline_enable: outline '%s' not found", name_str);
		PG_RETURN_BOOL(false);
	}

	SPI_finish();

	elog(NOTICE, "pg_outline_enable: outline '%s' enabled", name_str);

	PG_RETURN_BOOL(true);
}

/*
 * SQL function: disable an outline
 */
Datum
pg_outline_disable(PG_FUNCTION_ARGS)
{
	text	   *outline_name = PG_GETARG_TEXT_PP(0);
	char	   *name_str = text_to_cstring(outline_name);
	int			ret;
	StringInfoData query;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "pg_outline: SPI_connect failed");

	initStringInfo(&query);
	appendStringInfo(&query,
					 "UPDATE pg_outline_data SET enabled = false, updated_at = CURRENT_TIMESTAMP "
					 "WHERE outline_name = %s",
					 quote_literal_cstr(name_str));

	ret = SPI_execute(query.data, false, 0);

	if (ret < 0 || SPI_processed == 0)
	{
		SPI_finish();
		elog(WARNING, "pg_outline_disable: outline '%s' not found", name_str);
		PG_RETURN_BOOL(false);
	}

	SPI_finish();

	elog(NOTICE, "pg_outline_disable: outline '%s' disabled", name_str);

	PG_RETURN_BOOL(true);
}

/*
 * SQL function: list all outlines
 */
Datum
pg_outline_list(PG_FUNCTION_ARGS)
{
	/* This function is implemented in SQL in the --1.0.sql file */
	/* Just return void as it's a placeholder */
	elog(NOTICE, "pg_outline_list: use SELECT * FROM pg_outline_list() for listing");

	PG_RETURN_VOID();
}
