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
#include "common/md5.h"
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
#include "portability/instr_time.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "tcop/tcopprot.h"
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
static ExplainOneQuery_hook_type prev_ExplainOneQuery_hook = NULL;

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
	bool displayed;  /* Whether outline data has been displayed */
} OutlineInfo;

/* Structure to hold hint-to-Query position mapping */
typedef struct QueryHintPosition
{
	int			hint_location;	/* Location in source where this hint begins */
	int			hint_end;		/* Location where this hint ends */
	char	   *hint_text;		/* Hint text extracted from comment */
} QueryHintPosition;

/* Memory context for pg_outline data that persists across queries */
static MemoryContext OutlineContext = NULL;

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
static void outline_ExplainOneQuery(Query *query, int cursorOptions,
									IntoClause *into, ExplainState *es,
									const char *queryString, ParamListInfo params,
									QueryEnvironment *queryEnv);

static void generate_outline_from_plan(PlannedStmt *plan, const char *query_string);
static void extract_hints_from_plan_tree(Plan *plan, List **hints, int level);
static char *get_scan_method_hint(Plan *plan);
static char *get_join_method_hint(Plan *plan);
static char *get_leading_hint(Plan *plan);
static char *get_leading_hint_from_join(Plan *plan);
static char *extract_relation_names_from_plan(Plan *plan);
static char *get_relation_name(Index relid, PlannedStmt *plan);
static void display_outline_data(void);
static StringInfo format_outline_data(List *hints);

/* Query fingerprinting */
static char *normalize_query(const char *query_string);
static char *compute_query_fingerprint(const char *normalized_query);

/* Hint storage and retrieval */
static void store_outline_hints(const char *outline_name, const char *query_pattern,
								const char *fingerprint, const char *hints);
static void store_outline_with_query_hints(const char *outline_name, const char *query_pattern,
										   const char *fingerprint, Query *query);
static char *retrieve_outline_hints(const char *fingerprint);
static bool outline_exists(const char *outline_name);

/* Inline hint extraction */
static List *extract_inline_hints_with_positions(const char *query_string);
static char *strip_hints_from_query(const char *query_string);
static void assign_hints_to_queries(Query *query, List *hint_positions, const char *source_text);
static char *find_hint_for_query_location(List *hint_positions, int stmt_location);

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

	prev_ExplainOneQuery_hook = ExplainOneQuery_hook;
	ExplainOneQuery_hook = outline_ExplainOneQuery;

	/* Create memory context for outline data */
	OutlineContext = AllocSetContextCreate(TopMemoryContext,
										   "OutlineContext",
										   ALLOCSET_DEFAULT_SIZES);

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
	ExplainOneQuery_hook = prev_ExplainOneQuery_hook;

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

	/* Note: In auto mode, outline generation is only done for EXPLAIN statements
	 * in the ExplainOneQuery hook. Normal queries don't generate outlines. */

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
 * ExecutorEnd hook: no cleanup needed
 */
static void
outline_ExecutorEnd(QueryDesc *queryDesc)
{
	/* Note: We do NOT clean up current_outline here.
	 * For EXPLAIN statements, the ExplainOneQuery hook will clean up after displaying.
	 * For normal queries, memory will be freed at transaction end since it's in TopMemoryContext.
	 * This avoids the issue where ExecutorEnd is called before ExplainOneQuery can display. */

	/* Call previous hook or standard ExecutorEnd */
	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/*
 * ExplainOneQuery hook: display and store outline data for EXPLAIN statements
 */
static void
outline_ExplainOneQuery(Query *query, int cursorOptions, IntoClause *into,
						ExplainState *es, const char *queryString,
						ParamListInfo params, QueryEnvironment *queryEnv)
{
	PlannedStmt *plan = NULL;

	/* Call the previous hook or default behavior first */
	if (prev_ExplainOneQuery_hook)
		(*prev_ExplainOneQuery_hook)(query, cursorOptions, into, es,
									 queryString, params, queryEnv);
	else
	{
		/* Default EXPLAIN behavior */
		instr_time	planstart,
					planduration;

		INSTR_TIME_SET_CURRENT(planstart);

		/* plan the query */
		plan = pg_plan_query(query, cursorOptions, params);

		INSTR_TIME_SET_CURRENT(planduration);
		INSTR_TIME_SUBTRACT(planduration, planstart);

		/* run it (if needed) and produce output */
		ExplainOnePlan(plan, into, es, queryString, params, queryEnv,
					   &planduration);
	}

	/* In auto mode, generate and display outline for EXPLAIN statements */
	if (pg_outline_enabled && strcmp(pg_outline_mode, "auto") == 0 && queryString)
	{
		/* Get the plan if we don't have it yet */
		if (plan == NULL)
			plan = current_plannedstmt;

		if (plan)
		{
			MemoryContext oldcontext;

			/* Initialize current outline if needed */
			if (current_outline == NULL)
			{
				oldcontext = MemoryContextSwitchTo(OutlineContext);
				current_outline = (OutlineInfo *) palloc0(sizeof(OutlineInfo));
				current_outline->hints = NIL;
				MemoryContextSwitchTo(oldcontext);
			}

			/* Generate outline from the plan - must be in OutlineContext
			 * so hint strings survive across memory context resets */
			oldcontext = MemoryContextSwitchTo(OutlineContext);
			generate_outline_from_plan(plan, queryString);
			MemoryContextSwitchTo(oldcontext);

			if (current_outline && current_outline->hints && list_length(current_outline->hints) > 0)
			{
				/* Display outline data if enabled */
				if (pg_outline_display_hints)
				{
					char *normalized;
					char *fingerprint;
					StringInfoData outline_name;
					StringInfoData hints_buf;
					ListCell *lc;
					bool first = true;

					display_outline_data();

					/* Generate outline creation command for user to execute */
					normalized = normalize_query(queryString);
					fingerprint = compute_query_fingerprint(normalized);

					if (fingerprint)
					{
						/* Generate outline name using fingerprint */
						initStringInfo(&outline_name);
						appendStringInfo(&outline_name, "auto_outline_%s", fingerprint);

						/* Format hints as a single string */
						initStringInfo(&hints_buf);
						foreach(lc, current_outline->hints)
						{
							char *hint = (char *) lfirst(lc);
							if (hint)
							{
								if (!first)
									appendStringInfoChar(&hints_buf, '\n');
								appendStringInfoString(&hints_buf, hint);
								first = false;
							}
						}

						/* Check if outline already exists */
						if (outline_exists(outline_name.data))
						{
							/* Outline already exists - notify user */
							elog(NOTICE, "An outline named '%s' already exists for this query.\n"
								 "The existing outline will be used when the query is executed.",
								 outline_name.data);
						}
						else
						{
							/* Display the command to store this outline */
							elog(NOTICE, "To store this outline, execute:\n"
								 "SELECT pg_outline_create('%s', %s, %s);",
								 outline_name.data,
								 quote_literal_cstr(normalized),
								 quote_literal_cstr(hints_buf.data));
						}

					/* Free the StringInfoData buffers */
					pfree(outline_name.data);
					pfree(hints_buf.data);
					}

					if (normalized)
						pfree(normalized);
					if (fingerprint)
						pfree(fingerprint);
				}
			}
		}
	}
	else if (pg_outline_display_hints && current_outline)
	{
		/* Not in auto mode but display is enabled - just display without storing */
		display_outline_data();
	}

	/* Reset outline context for next query - this frees all memory at once */
	if (OutlineContext)
	{
		MemoryContextReset(OutlineContext);
		current_outline = NULL;  /* Pointer is now invalid after reset */
	}
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

	/* Extract leading (join order) hints - only at top level (level 0) */
	if (level == 0)
	{
		hint_str = get_leading_hint(plan);
		if (hint_str)
			*hints = lappend(*hints, hint_str);
	}

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
			/* Note: we'd need access to PlannedStmt to get actual subplan */
			(void) lc; /* unused in current implementation */
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
 * Extract relation names from a plan tree recursively
 * Returns a string representation suitable for Leading hint
 * For join nodes, returns nested format: (left right)
 * For scan nodes, returns the relation name
 */
static char *
extract_relation_names_from_plan(Plan *plan)
{
	StringInfoData result;
	char	   *left_str;
	char	   *right_str;
	char	   *relname;

	if (plan == NULL)
		return NULL;

	initStringInfo(&result);

	/* Check if this is a join node */
	switch (nodeTag(plan))
	{
		case T_NestLoop:
		case T_HashJoin:
		case T_MergeJoin:
			/* Recursively get relation names from left and right subtrees */
			left_str = extract_relation_names_from_plan(plan->lefttree);
			right_str = extract_relation_names_from_plan(plan->righttree);

			if (left_str && right_str)
			{
				/* Build nested format: (left right) */
				appendStringInfo(&result, "(%s %s)", left_str, right_str);
				pfree(left_str);
				pfree(right_str);
				return result.data;
			}
			else
			{
				/* Cleanup and return NULL if either side is missing */
				if (left_str)
					pfree(left_str);
				if (right_str)
					pfree(right_str);
				return NULL;
			}

		case T_SeqScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapHeapScan:
			/* Extract relation name from scan node */
			relname = get_relation_name(((Scan *) plan)->scanrelid, current_plannedstmt);
			if (relname)
			{
				/* Use just the base relation name (without AS alias part) */
				char	   *space_pos = strchr(relname, ' ');

				if (space_pos)
				{
					/* Extract just the base name before " AS " */
					size_t		len = space_pos - relname;
					char	   *base_name = palloc(len + 1);

					memcpy(base_name, relname, len);
					base_name[len] = '\0';
					pfree(relname);
					return base_name;
				}
				return relname;
			}
			return NULL;

		default:
			/* For other node types, try to recurse into child plans */
			if (plan->lefttree)
			{
				left_str = extract_relation_names_from_plan(plan->lefttree);
				if (left_str)
					return left_str;
			}
			if (plan->righttree)
			{
				right_str = extract_relation_names_from_plan(plan->righttree);
				if (right_str)
					return right_str;
			}
			return NULL;
	}
}

/*
 * Generate Leading hint from a join plan tree
 * Returns nested parentheses format like: Leading((t1 t2) (t3 t4))
 */
static char *
get_leading_hint_from_join(Plan *plan)
{
	StringInfoData hint;
	char	   *relation_names;

	if (plan == NULL)
		return NULL;

	/* Only generate Leading hint for join nodes */
	switch (nodeTag(plan))
	{
		case T_NestLoop:
		case T_HashJoin:
		case T_MergeJoin:
			/* Extract nested relation names from the join tree */
			relation_names = extract_relation_names_from_plan(plan);
			if (relation_names)
			{
				initStringInfo(&hint);
				appendStringInfo(&hint, "Leading%s", relation_names);
				pfree(relation_names);
				return hint.data;
			}
			return NULL;

		default:
			return NULL;
	}
}

/*
 * Generate leading (join order) hint for a plan node
 */
static char *
get_leading_hint(Plan *plan)
{
	/* Generate Leading hint only for the top-level join */
	/* This will be called from extract_hints_from_plan_tree */
	return get_leading_hint_from_join(plan);
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
	StringInfo ret;
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

	/* Allocate a StringInfo structure and copy the result */
	ret = makeStringInfo();
	appendStringInfoString(ret, result.data);
	pfree(result.data);  /* Free the local StringInfoData's buffer */
	return ret;
}

/*
 * Display outline data to client
 */
static void
display_outline_data(void)
{
	if (current_outline && current_outline->hints && list_length(current_outline->hints) > 0)
	{
		/* Only display if not already displayed */
		if (!current_outline->displayed)
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
			/* Note: outline.data will be freed when memory context is reset */
			current_outline->displayed = true;
		}
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
 * Returns first 12 characters of MD5 hash for shorter outline names
 */
static char *
compute_query_fingerprint(const char *normalized_query)
{
	char		hexsum[33];  /* MD5 hex string: 32 chars + null terminator */
	char		*result;

	if (!normalized_query)
		return NULL;

	/* Compute MD5 hash - pg_md5_hash outputs hexadecimal string directly */
	if (!pg_md5_hash(normalized_query, strlen(normalized_query), hexsum))
		return NULL;

	/* Use only first 12 characters for shorter outline names */
	hexsum[12] = '\0';
	result = pstrdup(hexsum);

	return result;
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
 * Check if an outline with the given name exists in the database
 */
static bool
outline_exists(const char *outline_name)
{
	int			ret;
	bool		exists = false;
	StringInfoData query;

	if (!outline_name)
		return false;

	if (SPI_connect() != SPI_OK_CONNECT)
	{
		elog(WARNING, "pg_outline: SPI_connect failed");
		return false;
	}

	initStringInfo(&query);
	appendStringInfo(&query,
					 "SELECT 1 FROM pg_outline_data "
					 "WHERE outline_name = %s "
					 "LIMIT 1",
					 quote_literal_cstr(outline_name));

	ret = SPI_execute(query.data, true, 1);

	if (ret == SPI_OK_SELECT && SPI_processed > 0)
		exists = true;

	pfree(query.data);
	SPI_finish();

	return exists;
}

/*
 * Helper structure to collect hints from Query tree
 */
typedef struct QueryHintCollector
{
	List	   *hints;		/* List of strings: each Query's hints */
	int			query_index; /* Current Query index */
} QueryHintCollector;

/* Forward declaration */
static void collect_query_hints_recursive(Query *query, QueryHintCollector *collector);

/*
 * Context structure for SubLink collector walker
 */
typedef struct SubLinkCollectorContext
{
	QueryHintCollector *collector;
} SubLinkCollectorContext;

/*
 * Walker function to collect hints from SubLink nodes in expressions
 */
static bool
collect_hints_sublink_walker(Node *node, SubLinkCollectorContext *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, SubLink))
	{
		SubLink    *sublink = (SubLink *) node;

		/* If the subselect is a Query, process it recursively */
		if (IsA(sublink->subselect, Query))
		{
			Query	   *subquery = (Query *) sublink->subselect;

			collect_query_hints_recursive(subquery, context->collector);
		}

		/* Continue walking the testexpr and other parts of the SubLink */
		return false;
	}

	/* For Query nodes, don't recurse here - they're handled separately */
	if (IsA(node, Query))
		return false;

	/* Continue walking the expression tree */
	return expression_tree_walker(node, collect_hints_sublink_walker, (void *) context);
}

/*
 * Process SubLinks in a Query's expression trees during hint collection
 */
static void
process_query_sublinks_collection(Query *query, QueryHintCollector *collector)
{
	SubLinkCollectorContext context;

	if (!query)
		return;

	context.collector = collector;

	/* Walk targetList (SELECT clause) */
	collect_hints_sublink_walker((Node *) query->targetList, &context);

	/* Walk jointree quals (WHERE clause) */
	if (query->jointree)
		collect_hints_sublink_walker((Node *) query->jointree->quals, &context);

	/* Walk havingQual (HAVING clause) */
	collect_hints_sublink_walker(query->havingQual, &context);

	/* Walk other expression fields that might contain SubLinks */
	collect_hints_sublink_walker(query->limitOffset, &context);
	collect_hints_sublink_walker(query->limitCount, &context);
}

/*
 * Recursively collect hints from Query tree
 * Assigns an index to each Query in depth-first order
 */
static void
collect_query_hints_recursive(Query *query, QueryHintCollector *collector)
{
	ListCell   *lc;

	if (!query)
		return;

	/* Store this Query's hint with its index */
	if (query->query_hints)
	{
		char	   *indexed_hint = psprintf("%d:%s", collector->query_index, query->query_hints);

		collector->hints = lappend(collector->hints, indexed_hint);
		elog(DEBUG1, "Collected hint for Query #%d: %s", collector->query_index, query->query_hints);
	}

	collector->query_index++;

	/* Process subqueries in RTEs */
	foreach(lc, query->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		if (rte->rtekind == RTE_SUBQUERY && rte->subquery)
		{
			collect_query_hints_recursive(rte->subquery, collector);
		}
	}

	/* Process CTEs */
	foreach(lc, query->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

		if (cte->ctequery)
		{
			Query	   *ctequery = castNode(Query, cte->ctequery);

			collect_query_hints_recursive(ctequery, collector);
		}
	}

	/* Process SubLinks in expressions (WHERE, SELECT, HAVING, etc.) */
	process_query_sublinks_collection(query, collector);
}

/*
 * Store outline with per-Query hints
 * This function stores hints separately for each Query in the Query tree
 */
static void
store_outline_with_query_hints(const char *outline_name, const char *query_pattern,
							   const char *fingerprint, Query *query)
{
	int			ret;
	StringInfoData sql;
	QueryHintCollector collector;
	ListCell   *lc;
	int			outline_id;

	/* Initialize collector */
	collector.hints = NIL;
	collector.query_index = 0;

	/* Collect all hints from the Query tree */
	collect_query_hints_recursive(query, &collector);

	if (list_length(collector.hints) == 0)
	{
		elog(WARNING, "pg_outline: no hints found in Query tree");
		return;
	}

	if (SPI_connect() != SPI_OK_CONNECT)
	{
		elog(ERROR, "pg_outline: SPI_connect failed");
		return;
	}

	/* First, insert/update the outline record */
	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "INSERT INTO pg_outline_data (outline_name, query_pattern, hint_string, enabled) "
					 "VALUES (%s, %s, '', true) "
					 "ON CONFLICT (outline_name) DO UPDATE SET "
					 "query_pattern = EXCLUDED.query_pattern, "
					 "updated_at = CURRENT_TIMESTAMP "
					 "RETURNING outline_id",
					 quote_literal_cstr(outline_name),
					 quote_literal_cstr(query_pattern));

	ret = SPI_execute(sql.data, false, 0);

	if (ret != SPI_OK_INSERT_RETURNING && ret != SPI_OK_UPDATE_RETURNING)
	{
		elog(ERROR, "pg_outline: failed to store outline");
		SPI_finish();
		return;
	}

	/* Get the outline_id */
	if (SPI_processed > 0)
	{
		TupleDesc	tupdesc = SPI_tuptable->tupdesc;
		HeapTuple	tuple = SPI_tuptable->vals[0];
		bool		isnull;
		Datum		id_datum;

		id_datum = SPI_getbinval(tuple, tupdesc, 1, &isnull);
		if (isnull)
		{
			elog(ERROR, "pg_outline: outline_id is NULL");
			SPI_finish();
			return;
		}
		outline_id = DatumGetInt32(id_datum);
	}
	else
	{
		elog(ERROR, "pg_outline: no outline_id returned");
		SPI_finish();
		return;
	}

	/* Delete existing per-Query hints for this outline */
	resetStringInfo(&sql);
	appendStringInfo(&sql,
					 "DELETE FROM pg_outline_query_hints WHERE outline_id = %d",
					 outline_id);
	SPI_execute(sql.data, false, 0);

	/* Insert per-Query hints */
	foreach(lc, collector.hints)
	{
		char	   *indexed_hint = (char *) lfirst(lc);
		int			qindex;
		char	   *hint_text;
		char	   *colon_pos;

		/* Parse "index:hint" format */
		colon_pos = strchr(indexed_hint, ':');
		if (!colon_pos)
			continue;

		*colon_pos = '\0';
		qindex = atoi(indexed_hint);
		hint_text = colon_pos + 1;

		resetStringInfo(&sql);
		appendStringInfo(&sql,
						 "INSERT INTO pg_outline_query_hints (outline_id, query_index, hint_string) "
						 "VALUES (%d, %d, %s)",
						 outline_id, qindex, quote_literal_cstr(hint_text));

		ret = SPI_execute(sql.data, false, 0);
		if (ret != SPI_OK_INSERT)
			elog(WARNING, "pg_outline: failed to store hint for Query #%d", qindex);

		*colon_pos = ':'; /* Restore the string */
	}

	SPI_finish();

	elog(NOTICE, "pg_outline: stored %d hint(s) for outline '%s'",
		 list_length(collector.hints), outline_name);
}

/*
 * Extract inline hints from query string with position information
 * Looks for slash-star-plus ... star-slash comments and records both the hint content and its location
 * Returns a list of QueryHintPosition structures
 */
static List *
extract_inline_hints_with_positions(const char *query_string)
{
	List	   *hint_positions = NIL;
	const char *p;
	const char *hint_start;
	bool		in_hint = false;
	int			hint_begin_loc = 0;

	if (!query_string)
		return NIL;

	for (p = query_string; *p; p++)
	{
		/* Check for start of hint comment: slash-star-plus */
		if (!in_hint && *p == '/' && *(p + 1) == '*' && *(p + 2) == '+')
		{
			in_hint = true;
			hint_begin_loc = p - query_string;
			hint_start = p + 3; /* Skip past the opening */
			p += 2;			/* Move past slash-star (the loop will move past plus) */
			continue;
		}

		/* Check for end of comment: star-slash */
		if (in_hint && *p == '*' && *(p + 1) == '/')
		{
			/* Extract hint content */
			size_t		hint_len = p - hint_start;
			char	   *hint_text = palloc(hint_len + 1);
			QueryHintPosition *pos = palloc(sizeof(QueryHintPosition));

			memcpy(hint_text, hint_start, hint_len);
			hint_text[hint_len] = '\0';

			pos->hint_location = hint_begin_loc;
			pos->hint_end = (p - query_string) + 2; /* Include closing */
			pos->hint_text = hint_text;

			hint_positions = lappend(hint_positions, pos);

			in_hint = false;
			p++;				/* Skip past the slash in star-slash */
			continue;
		}
	}

	return hint_positions;
}

/*
 * Strip inline hints from query string
 * Returns a copy of the query with all hint comments removed
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
		/* Check for start of hint comment: slash-star-plus */
		if (!in_hint && *p == '/' && *(p + 1) == '*' && *(p + 2) == '+')
		{
			in_hint = true;
			p += 2;			/* Skip past slash-star (the loop will move past plus) */
			continue;
		}

		/* Check for end of comment: star-slash */
		if (in_hint && *p == '*' && *(p + 1) == '/')
		{
			in_hint = false;
			p++;				/* Skip past the slash in star-slash */
			continue;
		}

		/* Copy character if not in hint */
		if (!in_hint)
			appendStringInfoChar(&result, *p);
	}

	return result.data;
}

/*
 * Find the hint that corresponds to a Query at a specific location
 * Looks for a hint comment that appears just before the SELECT keyword
 * at or near the Query's stmt_location
 */
static char *
find_hint_for_query_location(List *hint_positions, int stmt_location)
{
	ListCell   *lc;

	if (stmt_location < 0)
		return NULL;

	foreach(lc, hint_positions)
	{
		QueryHintPosition *pos = (QueryHintPosition *) lfirst(lc);

		/*
		 * A hint applies to a Query if it appears shortly before the Query's
		 * location. We allow for some whitespace (up to 100 characters).
		 */
		if (pos->hint_end <= stmt_location &&
			stmt_location - pos->hint_end < 100)
		{
			return pos->hint_text;
		}
	}

	return NULL;
}

/*
 * Context structure for SubLink walker
 */
typedef struct SubLinkWalkerContext
{
	List	   *hint_positions;
	const char *source_text;
} SubLinkWalkerContext;

/*
 * Walker function to process SubLink nodes in expressions
 * This is called by expression_tree_walker for each node in the expression tree
 */
static bool
assign_hints_sublink_walker(Node *node, SubLinkWalkerContext *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, SubLink))
	{
		SubLink    *sublink = (SubLink *) node;

		/* If the subselect is a Query, process it recursively */
		if (IsA(sublink->subselect, Query))
		{
			Query	   *subquery = (Query *) sublink->subselect;

			assign_hints_to_queries(subquery, context->hint_positions, context->source_text);
		}

		/* Continue walking the testexpr and other parts of the SubLink */
		return false;
	}

	/* For Query nodes, don't recurse here - they're handled separately */
	if (IsA(node, Query))
		return false;

	/* Continue walking the expression tree */
	return expression_tree_walker(node, assign_hints_sublink_walker, (void *) context);
}

/*
 * Process SubLinks in a Query's expression trees
 * This walks all expressions in the Query looking for SubLink nodes
 */
static void
process_query_sublinks(Query *query, List *hint_positions, const char *source_text)
{
	SubLinkWalkerContext context;

	if (!query)
		return;

	context.hint_positions = hint_positions;
	context.source_text = source_text;

	/* Walk targetList (SELECT clause) */
	assign_hints_sublink_walker((Node *) query->targetList, &context);

	/* Walk jointree quals (WHERE clause) */
	if (query->jointree)
		assign_hints_sublink_walker((Node *) query->jointree->quals, &context);

	/* Walk havingQual (HAVING clause) */
	assign_hints_sublink_walker(query->havingQual, &context);

	/* Walk other expression fields that might contain SubLinks */
	assign_hints_sublink_walker(query->limitOffset, &context);
	assign_hints_sublink_walker(query->limitCount, &context);
}

/*
 * Recursively assign hints to Query structures based on their location
 * This walks the Query tree and associates each Query with its corresponding hint
 */
static void
assign_hints_to_queries(Query *query, List *hint_positions, const char *source_text)
{
	ListCell   *lc;
	char	   *hint;

	if (!query)
		return;

	/* Find and assign hint for this Query */
	hint = find_hint_for_query_location(hint_positions, query->stmt_location);
	if (hint)
	{
		query->query_hints = pstrdup(hint);
		elog(DEBUG1, "Assigned hint '%s' to Query at location %d",
			 hint, query->stmt_location);
	}

	/* Process subqueries in RTEs */
	foreach(lc, query->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		if (rte->rtekind == RTE_SUBQUERY && rte->subquery)
		{
			assign_hints_to_queries(rte->subquery, hint_positions, source_text);
		}
	}

	/* Process CTEs */
	foreach(lc, query->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

		if (cte->ctequery)
		{
			Query *ctequery = castNode(Query, cte->ctequery);
			assign_hints_to_queries(ctequery, hint_positions, source_text);
		}
	}

	/* Process SubLinks in expressions (WHERE, SELECT, HAVING, etc.) */
	process_query_sublinks(query, hint_positions, source_text);
}

/*
 * SQL function: create an outline for a query
 */
Datum
pg_outline_create(PG_FUNCTION_ARGS)
{
	text	   *outline_name = PG_GETARG_TEXT_PP(0);
	text	   *query_text = PG_GETARG_TEXT_PP(1);
	char	   *name_str;
	char	   *query_str;
	char	   *hints_str;
	char	   *normalized;
	char	   *fingerprint;

	/* Check for NULL arguments */
	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();

	name_str = text_to_cstring(outline_name);
	query_str = text_to_cstring(query_text);

	/* Handle optional hints parameter */
	if (PG_ARGISNULL(2))
		hints_str = pstrdup("");  /* Use empty string for NULL hints */
	else
		hints_str = text_to_cstring(PG_GETARG_TEXT_PP(2));

	/* Normalize query and compute fingerprint */
	normalized = normalize_query(query_str);
	fingerprint = compute_query_fingerprint(normalized);

	/* Store the outline with normalized query as pattern */
	store_outline_hints(name_str, normalized, fingerprint, hints_str);

	elog(NOTICE, "pg_outline_create: outline '%s' created with fingerprint %s",
		 name_str, fingerprint ? fingerprint : "none");

	if (normalized)
		pfree(normalized);
	if (fingerprint)
		pfree(fingerprint);
	pfree(hints_str);

	PG_RETURN_BOOL(true);
}

/*
 * SQL function: create an outline from SQL with inline hints
 * This is the simplified interface that accepts SQL with hint comments
 * The function will:
 * 1. Extract inline hints with their positions from the SQL
 * 2. Parse the query (with hints still in source for location tracking)
 * 3. Assign each hint to its corresponding Query structure
 * 4. Plan the query with hints attached to Query structures
 * 5. Extract hints from the resulting execution plan (per Query)
 * 6. Store the extracted hints separately for each Query
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
	List	   *hint_positions;
	PlannedStmt *plan;
	Query	   *query;
	RawStmt	   *raw_stmt;
	List	   *raw_parsetree_list;
	List	   *hints = NIL;
	StringInfoData hint_str;
	ListCell   *lc;
	bool		first = true;

	/* Extract hint positions from the query */
	hint_positions = extract_inline_hints_with_positions(query_str);

	if (list_length(hint_positions) == 0)
	{
		elog(WARNING, "pg_outline_create_from_sql: no hints found in query");
		PG_RETURN_BOOL(false);
	}

	elog(NOTICE, "Found %d hint(s) in query", list_length(hint_positions));

	/* Parse the raw query to get RawStmt with location info */
	raw_parsetree_list = pg_parse_query(query_str);

	if (list_length(raw_parsetree_list) != 1)
	{
		elog(WARNING, "pg_outline_create_from_sql: query must contain exactly one statement");
		PG_RETURN_BOOL(false);
	}

	raw_stmt = linitial_node(RawStmt, raw_parsetree_list);

	/* Analyze the query to get Query tree */
	query = parse_analyze(raw_stmt, query_str, NULL, 0, NULL);

	/* Assign hints to Query structures based on their locations */
	assign_hints_to_queries(query, hint_positions, query_str);

	/*
	 * Count how many Query structures have hints assigned
	 */
	/* TODO: Walk the Query tree and count queries with hints */

	/*
	 * Plan the query - the hints are now attached to Query structures
	 * The planner can access query->query_hints for each Query
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

	/* Strip hints and normalize query for fingerprint */
	query_without_hints = strip_hints_from_query(query_str);
	normalized = normalize_query(query_without_hints);
	fingerprint = compute_query_fingerprint(normalized);

	/*
	 * Store the outline with per-Query hints
	 * Use normalized query as the pattern for consistent matching
	 */
	store_outline_with_query_hints(name_str, normalized, fingerprint, query);

	elog(NOTICE, "pg_outline_create_from_sql: outline '%s' created with fingerprint %s",
		 name_str, fingerprint ? fingerprint : "none");

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
