/*-------------------------------------------------------------------------
 *
 * outline_plan.c
 *	  Extract hints from execution plans for outline system
 *
 * This file implements the functionality to derive hints from a PlannedStmt
 * that can be used to recreate the same execution plan later.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/outline/outline_plan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/outline_hints.h"
#include "nodes/plannodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"
#include "lib/stringinfo.h"
#include "utils/lsyscache.h"
#include "catalog/namespace.h"
#include "parser/parsetree.h"

/* Forward declarations */
static void extract_hints_from_plan(Plan *plan, StringInfo hints, List *rtable, int rtoffset);
static void extract_scan_hints(Scan *scan, StringInfo hints, List *rtable);
static void extract_join_hints(Join *join, StringInfo hints, List *rtable);
static char *get_outline_rel_name(Oid relid, List *rtable);

/*
 * Convert a PlannedStmt to a hint string
 *
 * This function walks the plan tree and extracts hints that would
 * reproduce the same plan structure.
 */
char *
plan_to_hints(PlannedStmt *pstmt, const char *query_string)
{
	StringInfoData hints;

	if (pstmt == NULL || pstmt->planTree == NULL)
		return NULL;

	initStringInfo(&hints);

	/* Extract hints from the plan tree */
	extract_hints_from_plan(pstmt->planTree, &hints, pstmt->rtable, 0);

	if (hints.len == 0)
	{
		pfree(hints.data);
		return NULL;
	}

	return hints.data;
}

/*
 * Format hints in OceanBase/Oracle outline data style
 *
 * Wraps hints in a comment block with BEGIN_OUTLINE_DATA/END_OUTLINE_DATA
 * markers, similar to OceanBase and Oracle format.
 */
char *
format_outline_data(const char *hints)
{
	StringInfoData outline;
	char	   *line;
	char	   *hints_copy;
	char	   *saveptr;

	if (hints == NULL || hints[0] == '\0')
		return NULL;

	initStringInfo(&outline);

	/* Start outline data block */
	appendStringInfoString(&outline, "/*+\n");
	appendStringInfoString(&outline, "BEGIN_OUTLINE_DATA\n");

	/* Add each hint on a separate line */
	hints_copy = pstrdup(hints);
	line = strtok_r(hints_copy, "\n", &saveptr);
	while (line != NULL)
	{
		/* Skip empty lines */
		if (line[0] != '\0')
		{
			appendStringInfo(&outline, "%s\n", line);
		}
		line = strtok_r(NULL, "\n", &saveptr);
	}
	pfree(hints_copy);

	/* End outline data block */
	appendStringInfoString(&outline, "END_OUTLINE_DATA\n");
	appendStringInfoString(&outline, "*/");

	return outline.data;
}

/*
 * Recursively extract hints from a plan node
 */
static void
extract_hints_from_plan(Plan *plan, StringInfo hints, List *rtable, int rtoffset)
{
	if (plan == NULL)
		return;

	/* Extract hints based on node type */
	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapIndexScan:
		case T_BitmapHeapScan:
		case T_TidScan:
			extract_scan_hints((Scan *) plan, hints, rtable);
			break;

		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin:
			extract_join_hints((Join *) plan, hints, rtable);
			/* Recursively process join inputs */
			extract_hints_from_plan(plan->lefttree, hints, rtable, rtoffset);
			extract_hints_from_plan(plan->righttree, hints, rtable, rtoffset);
			break;

		case T_Gather:
		case T_GatherMerge:
			{
				Gather *gather = (Gather *) plan;

				/*
				 * Extract parallel hint from Gather node
				 * Gather nodes contain the number of workers used for parallel execution
				 */
				if (gather->num_workers > 0 && plan->lefttree)
				{
					/* Try to get the relation name from the subplan */
					Plan *subplan = plan->lefttree;

					/* If the subplan is a scan, we can generate a Parallel hint */
					if (IsA(subplan, SeqScan) || IsA(subplan, IndexScan) ||
						IsA(subplan, IndexOnlyScan) || IsA(subplan, BitmapHeapScan))
					{
						Scan *scan = (Scan *) subplan;
						RangeTblEntry *rte = rt_fetch(scan->scanrelid, rtable);

						if (rte && rte->rtekind == RTE_RELATION)
						{
							char *relname = get_outline_rel_name(rte->relid, rtable);
							if (relname)
							{
								if (hints->len > 0)
									appendStringInfoChar(hints, '\n');
								appendStringInfo(hints, "Parallel(%s %d)",
											   relname, gather->num_workers);
							}
						}
					}
				}

				/* Recursively process subplan */
				extract_hints_from_plan(plan->lefttree, hints, rtable, rtoffset);
			}
			break;

		case T_Append:
		case T_MergeAppend:
		case T_BitmapAnd:
		case T_BitmapOr:
			{
				ListCell   *lc;
				Append	   *append = (Append *) plan;

				foreach(lc, append->appendplans)
				{
					Plan	   *subplan = (Plan *) lfirst(lc);

					extract_hints_from_plan(subplan, hints, rtable, rtoffset);
				}
			}
			break;

		case T_SubqueryScan:
			{
				SubqueryScan *subscan = (SubqueryScan *) plan;

				extract_hints_from_plan(subscan->subplan, hints, rtable,
									   subscan->scan.scanrelid);
			}
			break;

		default:
			/* Recursively process left and right subtrees */
			if (plan->lefttree)
				extract_hints_from_plan(plan->lefttree, hints, rtable, rtoffset);
			if (plan->righttree)
				extract_hints_from_plan(plan->righttree, hints, rtable, rtoffset);
			break;
	}
}

/*
 * Extract scan method hints from a scan node
 */
static void
extract_scan_hints(Scan *scan, StringInfo hints, List *rtable)
{
	RangeTblEntry *rte;
	char	   *relname;

	/* Get the range table entry */
	rte = rt_fetch(scan->scanrelid, rtable);
	if (rte == NULL || rte->rtekind != RTE_RELATION)
		return;

	/* Get relation name */
	relname = get_outline_rel_name(rte->relid, rtable);
	if (relname == NULL)
		return;

	/* Generate appropriate hint based on scan type */
	switch (nodeTag(scan))
	{
		case T_SeqScan:
			if (hints->len > 0)
				appendStringInfoChar(hints, '\n');
			appendStringInfo(hints, "SeqScan(%s)", relname);
			break;

		case T_IndexScan:
			{
				IndexScan  *iscan = (IndexScan *) scan;
				char	   *indexname;

				indexname = get_outline_rel_name(iscan->indexid, NULL);
				if (indexname != NULL)
				{
					if (hints->len > 0)
						appendStringInfoChar(hints, '\n');
					appendStringInfo(hints, "IndexScan(%s %s)",
									 relname, indexname);
				}
			}
			break;

		case T_IndexOnlyScan:
			{
				IndexOnlyScan *ioscan = (IndexOnlyScan *) scan;
				char	   *indexname;

				indexname = get_outline_rel_name(ioscan->indexid, NULL);
				if (indexname != NULL)
				{
					if (hints->len > 0)
						appendStringInfoChar(hints, '\n');
					appendStringInfo(hints, "IndexOnlyScan(%s %s)",
									 relname, indexname);
				}
			}
			break;

		case T_BitmapHeapScan:
			if (hints->len > 0)
				appendStringInfoChar(hints, '\n');
			appendStringInfo(hints, "BitmapScan(%s)", relname);
			break;

		case T_TidScan:
			if (hints->len > 0)
				appendStringInfoChar(hints, '\n');
			appendStringInfo(hints, "TidScan(%s)", relname);
			break;

		default:
			/* Other scan types not supported yet */
			break;
	}

	/*
	 * Optionally add row count hint if the estimated rows differ significantly
	 * from what might be expected (e.g., very small or very large).
	 * This is conservative - we only add Rows hints for notable cases.
	 */
	if (scan->plan.plan_rows > 0)
	{
		double rows = scan->plan.plan_rows;

		/*
		 * Add Rows hint for significant row counts (> 1000) or very small counts (< 10)
		 * to help reproduce the plan when statistics might differ
		 */
		if (rows >= 1000 || rows < 10)
		{
			if (hints->len > 0)
				appendStringInfoChar(hints, '\n');
			appendStringInfo(hints, "Rows(%s %.0f)", relname, rows);
		}
	}

	/*
	 * Add parallel hint if the scan uses parallel workers
	 * Note: parallel_aware flag indicates plan can be executed in parallel
	 */
	if (scan->plan.parallel_aware && scan->plan.plan_rows > 10000)
	{
		/*
		 * We can't directly get worker count from the scan node,
		 * but we can indicate that parallelism is expected.
		 * In practice, the actual worker count comes from GatherNode.
		 * For now, we add a comment that parallelism was used.
		 */
		/* Parallel hint will be added at Gather node level instead */
	}
}

/*
 * Extract join method hints from a join node
 */
static void
extract_join_hints(Join *join, StringInfo hints, List *rtable)
{
	char	   *outer_rel = NULL;
	char	   *inner_rel = NULL;
	Plan	   *outer_plan = join->plan.lefttree;
	Plan	   *inner_plan = join->plan.righttree;

	/* Try to get relation names from the join inputs */
	if (IsA(outer_plan, Scan))
	{
		Scan	   *scan = (Scan *) outer_plan;
		RangeTblEntry *rte = rt_fetch(scan->scanrelid, rtable);

		if (rte && rte->rtekind == RTE_RELATION)
			outer_rel = get_outline_rel_name(rte->relid, rtable);
	}

	if (IsA(inner_plan, Scan))
	{
		Scan	   *scan = (Scan *) inner_plan;
		RangeTblEntry *rte = rt_fetch(scan->scanrelid, rtable);

		if (rte && rte->rtekind == RTE_RELATION)
			inner_rel = get_outline_rel_name(rte->relid, rtable);
	}

	if (outer_rel == NULL || inner_rel == NULL)
		return;

	/* Generate appropriate hint based on join type */
	if (hints->len > 0)
		appendStringInfoChar(hints, '\n');

	switch (nodeTag(join))
	{
		case T_NestLoop:
			appendStringInfo(hints, "NestLoop(%s %s)", outer_rel, inner_rel);
			break;

		case T_HashJoin:
			appendStringInfo(hints, "HashJoin(%s %s)", outer_rel, inner_rel);
			break;

		case T_MergeJoin:
			appendStringInfo(hints, "MergeJoin(%s %s)", outer_rel, inner_rel);
			break;

		default:
			/* Shouldn't happen */
			break;
	}
}

/*
 * Get the name of a relation given its OID
 */
static char *
get_outline_rel_name(Oid relid, List *rtable)
{
	char	   *relname;
	char	   *nspname;
	char	   *result;

	/* Get relation and namespace names */
	relname = get_rel_name(relid);
	if (relname == NULL)
		return NULL;

	nspname = get_namespace_name(get_rel_namespace(relid));
	if (nspname == NULL)
	{
		pfree(relname);
		return NULL;
	}

	/* For now, just return the relation name without schema qualification */
	result = pstrdup(relname);

	return result;
}
