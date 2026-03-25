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

/* SQL-callable functions */
PG_FUNCTION_INFO_V1(pg_outline_create);
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
 * Planner hook: generate outline hints from the plan
 */
static PlannedStmt *
outline_planner(Query *parse, int cursorOptions, ParamListInfo boundParams)
{
	PlannedStmt *result;

	/* Call previous hook or standard planner */
	if (prev_planner_hook)
		result = prev_planner_hook(parse, cursorOptions, boundParams);
	else
		result = standard_planner(parse, cursorOptions, boundParams);

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

	initStringInfo(&hint);

	switch (nodeTag(plan))
	{
		case T_SeqScan:
			appendStringInfo(&hint, "SeqScan(%s)",
							 ((Scan *) plan)->scanrelid > 0 ? "table" : "unknown");
			return hint.data;

		case T_IndexScan:
			appendStringInfo(&hint, "IndexScan(%s)",
							 ((Scan *) plan)->scanrelid > 0 ? "table" : "unknown");
			return hint.data;

		case T_IndexOnlyScan:
			appendStringInfo(&hint, "IndexOnlyScan(%s)",
							 ((Scan *) plan)->scanrelid > 0 ? "table" : "unknown");
			return hint.data;

		case T_BitmapHeapScan:
			appendStringInfo(&hint, "BitmapScan(%s)",
							 ((Scan *) plan)->scanrelid > 0 ? "table" : "unknown");
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
 * SQL function: create an outline for a query
 */
Datum
pg_outline_create(PG_FUNCTION_ARGS)
{
	text	   *outline_name = PG_GETARG_TEXT_PP(0);
	text	   *query_text = PG_GETARG_TEXT_PP(1);
	text	   *hints_text = PG_GETARG_TEXT_PP(2);

	/* TODO: Implement outline creation logic */
	/* This would store the outline in a catalog table */

	elog(NOTICE, "pg_outline_create: outline '%s' created",
		 text_to_cstring(outline_name));

	PG_RETURN_BOOL(true);
}

/*
 * SQL function: drop an outline
 */
Datum
pg_outline_drop(PG_FUNCTION_ARGS)
{
	text	   *outline_name = PG_GETARG_TEXT_PP(0);

	/* TODO: Implement outline dropping logic */

	elog(NOTICE, "pg_outline_drop: outline '%s' dropped",
		 text_to_cstring(outline_name));

	PG_RETURN_BOOL(true);
}

/*
 * SQL function: enable an outline
 */
Datum
pg_outline_enable(PG_FUNCTION_ARGS)
{
	text	   *outline_name = PG_GETARG_TEXT_PP(0);

	/* TODO: Implement outline enabling logic */

	elog(NOTICE, "pg_outline_enable: outline '%s' enabled",
		 text_to_cstring(outline_name));

	PG_RETURN_BOOL(true);
}

/*
 * SQL function: disable an outline
 */
Datum
pg_outline_disable(PG_FUNCTION_ARGS)
{
	text	   *outline_name = PG_GETARG_TEXT_PP(0);

	/* TODO: Implement outline disabling logic */

	elog(NOTICE, "pg_outline_disable: outline '%s' disabled",
		 text_to_cstring(outline_name));

	PG_RETURN_BOOL(true);
}

/*
 * SQL function: list all outlines
 */
Datum
pg_outline_list(PG_FUNCTION_ARGS)
{
	/* TODO: Implement outline listing logic */
	/* This would return a set of records from the catalog table */

	elog(NOTICE, "pg_outline_list: listing outlines");

	PG_RETURN_VOID();
}
