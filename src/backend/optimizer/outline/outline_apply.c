/*-------------------------------------------------------------------------
 *
 * outline_apply.c
 *	  Apply outline hints to query optimization
 *
 * This file implements the hook functions that apply hints to the query
 * optimization process.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/outline/outline_apply.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/outline_hints.h"
#include "optimizer/paths.h"
#include "optimizer/pathnode.h"
#include "optimizer/cost.h"
#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"
#include "utils/lsyscache.h"
#include "utils/guc.h"

/* Module-level state */
static HintState *current_hint_state = NULL;

/*
 * Initialize outline hint system
 */
void
outline_hints_init(void)
{
	current_hint_state = NULL;
}

/*
 * Set the current hint state for a query
 */
void
outline_set_hint_state(HintState *hstate)
{
	current_hint_state = hstate;
}

/*
 * Get the current hint state
 */
HintState *
outline_get_hint_state(void)
{
	return current_hint_state;
}

/*
 * Clear the current hint state
 */
void
outline_clear_hint_state(void)
{
	if (current_hint_state != NULL)
	{
		free_hint_state(current_hint_state);
		current_hint_state = NULL;
	}
}

/*
 * Check if a scan hint applies to this relation
 */
static ScanHint *
find_scan_hint(HintState *hstate, Index relid, RangeTblEntry *rte)
{
	ListCell   *lc;
	char	   *relname;

	if (hstate == NULL || !hstate->enabled)
		return NULL;

	if (rte->rtekind != RTE_RELATION)
		return NULL;

	relname = get_rel_name(rte->relid);
	if (relname == NULL)
		return NULL;

	foreach(lc, hstate->hints)
	{
		Hint	   *hint = (Hint *) lfirst(lc);

		if (hint->type == HINT_TYPE_SCAN_METHOD)
		{
			ScanHint   *scan_hint = &hint->hint.scan;

			if (pg_strcasecmp(scan_hint->relname, relname) == 0)
				return scan_hint;
		}
	}

	return NULL;
}

/*
 * Check if a join hint applies to this join
 */
static JoinHint *
find_join_hint(HintState *hstate, RelOptInfo *outerrel, RelOptInfo *innerrel)
{
	ListCell   *lc;

	if (hstate == NULL || !hstate->enabled)
		return NULL;

	/* For now, we don't implement join hint matching */
	/* This would require tracking relation names through the join tree */

	foreach(lc, hstate->hints)
	{
		Hint	   *hint = (Hint *) lfirst(lc);

		if (hint->type == HINT_TYPE_JOIN_METHOD)
		{
			/* TODO: Match join hint with actual join relations */
		}
	}

	return NULL;
}

/*
 * Check if a Rows hint applies to this relation or join
 */
static RowsHint *
find_rows_hint(HintState *hstate, RelOptInfo *rel, PlannerInfo *root)
{
	ListCell   *lc;

	if (hstate == NULL || !hstate->enabled)
		return NULL;

	foreach(lc, hstate->hints)
	{
		Hint	   *hint = (Hint *) lfirst(lc);

		if (hint->type == HINT_TYPE_ROWS)
		{
			RowsHint   *rows_hint = &hint->hint.rows;
			int			nrelnames = list_length(rows_hint->relnames);

			/* For base relations, match single relation name */
			if (nrelnames == 1 && rel->reloptkind == RELOPT_BASEREL)
			{
				char *relname = (char *) linitial(rows_hint->relnames);
				RangeTblEntry *rte = root->simple_rte_array[rel->relid];

				if (rte && rte->rtekind == RTE_RELATION)
				{
					char *actual_relname = get_rel_name(rte->relid);
					if (actual_relname && pg_strcasecmp(relname, actual_relname) == 0)
						return rows_hint;
				}
			}
			/* For join relations, would need more complex matching */
			/* TODO: Implement join relation matching */
		}
	}

	return NULL;
}

/*
 * Check if a Parallel hint applies to this relation
 */
static ParallelHint *
find_parallel_hint(HintState *hstate, Index relid, RangeTblEntry *rte)
{
	ListCell   *lc;
	char	   *relname;

	if (hstate == NULL || !hstate->enabled)
		return NULL;

	if (rte->rtekind != RTE_RELATION)
		return NULL;

	relname = get_rel_name(rte->relid);
	if (relname == NULL)
		return NULL;

	foreach(lc, hstate->hints)
	{
		Hint	   *hint = (Hint *) lfirst(lc);

		if (hint->type == HINT_TYPE_PARALLEL)
		{
			ParallelHint   *parallel_hint = &hint->hint.parallel;

			if (pg_strcasecmp(parallel_hint->relname, relname) == 0)
				return parallel_hint;
		}
	}

	return NULL;
}

/*
 * Apply Set hints (GUC parameter overrides)
 */
static void
apply_set_hints(HintState *hstate)
{
	ListCell   *lc;

	if (hstate == NULL || !hstate->enabled)
		return;

	foreach(lc, hstate->hints)
	{
		Hint	   *hint = (Hint *) lfirst(lc);

		if (hint->type == HINT_TYPE_SET)
		{
			SetHint *set_hint = &hint->hint.set;

			/* Apply the GUC setting for this query */
			(void) set_config_option(set_hint->name, set_hint->value,
									 PGC_USERSET, PGC_S_SESSION,
									 GUC_ACTION_SAVE, true, 0, false);
		}
	}
}

/*
 * Hook function for set_rel_pathlist
 *
 * This function is called when paths are being generated for a relation.
 * We use it to enforce scan method hints.
 */
void
outline_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
						Index rti, RangeTblEntry *rte)
{
	ScanHint   *hint;
	RowsHint   *rows_hint;
	ParallelHint *parallel_hint;
	ListCell   *lc;
	List	   *paths_to_keep = NIL;

	/* Apply Set hints if we haven't already */
	static bool set_hints_applied = false;
	if (!set_hints_applied && current_hint_state != NULL)
	{
		apply_set_hints(current_hint_state);
		set_hints_applied = true;
	}

	/* Check if there's a Rows hint for this relation */
	rows_hint = find_rows_hint(current_hint_state, rel, root);
	if (rows_hint != NULL)
	{
		/* Override the estimated row count */
		rel->rows = rows_hint->rows;
		/* Recalculate tuple fraction if needed */
		rel->tuples = rows_hint->rows;
	}

	/* Check if there's a Parallel hint for this relation */
	parallel_hint = find_parallel_hint(current_hint_state, rti, rte);
	if (parallel_hint != NULL)
	{
		if (parallel_hint->force_parallel && parallel_hint->nworkers > 0)
		{
			/* Force parallel execution with specified number of workers */
			rel->consider_parallel = true;
			rel->rel_parallel_workers = parallel_hint->nworkers;
		}
		else if (!parallel_hint->force_parallel)
		{
			/* Disable parallel execution */
			rel->consider_parallel = false;
		}
	}

	/* Check if there's a scan hint for this relation */
	hint = find_scan_hint(current_hint_state, rti, rte);
	if (hint == NULL)
		return;

	/* Apply the hint by filtering paths */
	switch (hint->method)
	{
		case SCAN_HINT_SEQSCAN:
			/* Keep only sequential scan paths */
			foreach(lc, rel->pathlist)
			{
				Path	   *path = (Path *) lfirst(lc);

				if (IsA(path, Path) && path->pathtype == T_SeqScan)
					paths_to_keep = lappend(paths_to_keep, path);
			}
			if (paths_to_keep != NIL)
				rel->pathlist = paths_to_keep;
			break;

		case SCAN_HINT_INDEXSCAN:
			/* Keep only index scan paths */
			foreach(lc, rel->pathlist)
			{
				Path	   *path = (Path *) lfirst(lc);

				if (IsA(path, IndexPath))
					paths_to_keep = lappend(paths_to_keep, path);
			}
			if (paths_to_keep != NIL)
				rel->pathlist = paths_to_keep;
			break;

		case SCAN_HINT_INDEXONLYSCAN:
			/* Keep only index-only scan paths */
			foreach(lc, rel->pathlist)
			{
				Path	   *path = (Path *) lfirst(lc);

				if (IsA(path, IndexPath) &&
					((IndexPath *) path)->indexinfo->canreturn)
					paths_to_keep = lappend(paths_to_keep, path);
			}
			if (paths_to_keep != NIL)
				rel->pathlist = paths_to_keep;
			break;

		case SCAN_HINT_NOSEQSCAN:
			/* Remove sequential scan paths */
			foreach(lc, rel->pathlist)
			{
				Path	   *path = (Path *) lfirst(lc);

				if (!(IsA(path, Path) && path->pathtype == T_SeqScan))
					paths_to_keep = lappend(paths_to_keep, path);
			}
			if (paths_to_keep != NIL)
				rel->pathlist = paths_to_keep;
			break;

		case SCAN_HINT_NOINDEXSCAN:
			/* Remove index scan paths */
			foreach(lc, rel->pathlist)
			{
				Path	   *path = (Path *) lfirst(lc);

				if (!IsA(path, IndexPath))
					paths_to_keep = lappend(paths_to_keep, path);
			}
			if (paths_to_keep != NIL)
				rel->pathlist = paths_to_keep;
			break;

		default:
			/* Other hints not implemented yet */
			break;
	}
}

/*
 * Hook function for set_join_pathlist
 *
 * This function is called when join paths are being generated.
 * We use it to enforce join method hints.
 */
void
outline_set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel,
						 RelOptInfo *outerrel, RelOptInfo *innerrel,
						 JoinType jointype, JoinPathExtraData *extra)
{
	JoinHint   *hint;
	ListCell   *lc;
	List	   *paths_to_keep = NIL;

	/* Check if there's a join hint for this join */
	hint = find_join_hint(current_hint_state, outerrel, innerrel);
	if (hint == NULL)
		return;

	/* Apply the hint by filtering join paths */
	switch (hint->method)
	{
		case JOIN_HINT_NESTLOOP:
			/* Keep only nested loop join paths */
			foreach(lc, joinrel->pathlist)
			{
				Path	   *path = (Path *) lfirst(lc);

				if (IsA(path, NestPath))
					paths_to_keep = lappend(paths_to_keep, path);
			}
			if (paths_to_keep != NIL)
				joinrel->pathlist = paths_to_keep;
			break;

		case JOIN_HINT_HASHJOIN:
			/* Keep only hash join paths */
			foreach(lc, joinrel->pathlist)
			{
				Path	   *path = (Path *) lfirst(lc);

				if (IsA(path, HashPath))
					paths_to_keep = lappend(paths_to_keep, path);
			}
			if (paths_to_keep != NIL)
				joinrel->pathlist = paths_to_keep;
			break;

		case JOIN_HINT_MERGEJOIN:
			/* Keep only merge join paths */
			foreach(lc, joinrel->pathlist)
			{
				Path	   *path = (Path *) lfirst(lc);

				if (IsA(path, MergePath))
					paths_to_keep = lappend(paths_to_keep, path);
			}
			if (paths_to_keep != NIL)
				joinrel->pathlist = paths_to_keep;
			break;

		default:
			/* Other hints not implemented yet */
			break;
	}
}
