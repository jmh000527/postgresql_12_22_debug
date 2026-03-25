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
#include "nodes/parsenodes.h"
#include "utils/lsyscache.h"
#include "utils/guc.h"
#include "catalog/pg_class.h"

/* Module-level state */
static HintState *current_hint_state = NULL;

/* Forward declarations */
static RelOptInfo *outline_join_search(PlannerInfo *root, int levels_needed,
									   List *initial_rels);

/*
 * Initialize outline hint system
 */
void
outline_hints_init(void)
{
	current_hint_state = NULL;

	/* Register our custom join search hook for Leading hint support */
	join_search_hook = outline_join_search;
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
 * Check if a Leading hint applies to this query
 */
static LeadingHint *
find_leading_hint(HintState *hstate)
{
	ListCell   *lc;

	if (hstate == NULL || !hstate->enabled)
		return NULL;

	foreach(lc, hstate->hints)
	{
		Hint	   *hint = (Hint *) lfirst(lc);

		if (hint->type == HINT_TYPE_LEADING)
		{
			return &hint->hint.leading;
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

/*
 * find_rel_by_relname - Find a RelOptInfo in the initial_rels list by relation name
 */
static RelOptInfo *
find_rel_by_relname(List *initial_rels, const char *relname, PlannerInfo *root)
{
	ListCell   *lc;

	foreach(lc, initial_rels)
	{
		RelOptInfo *rel = (RelOptInfo *) lfirst(lc);
		RangeTblEntry *rte;
		char	   *rel_relname;

		/* Get the RTE for this relation */
		if (rel->relid == 0 || rel->relid > list_length(root->parse->rtable))
			continue;

		rte = rt_fetch(rel->relid, root->parse->rtable);
		if (rte->rtekind != RTE_RELATION)
			continue;

		rel_relname = get_rel_name(rte->relid);
		if (rel_relname && pg_strcasecmp(rel_relname, relname) == 0)
			return rel;
	}

	return NULL;
}

/*
 * join_two_rels - Join two RelOptInfo structures
 */
static RelOptInfo *
join_two_rels(PlannerInfo *root, RelOptInfo *outer_rel, RelOptInfo *inner_rel)
{
	RelOptInfo *joinrel;

	/* Try to make the join relation */
	joinrel = make_join_rel(root, outer_rel, inner_rel);

	if (joinrel == NULL)
	{
		/* Join is not legal, this can happen with certain join orders */
		return NULL;
	}

	return joinrel;
}

/*
 * outline_join_search_simple - Implement simple Leading hint (left-to-right join order)
 *
 * For a simple Leading hint like Leading(t1 t2 t3), we join tables from left to right:
 * First join t1 and t2, then join the result with t3, and so on.
 */
static RelOptInfo *
outline_join_search_simple(PlannerInfo *root, LeadingHint *leading_hint,
						   List *initial_rels)
{
	ListCell   *lc;
	RelOptInfo *result_rel = NULL;
	List	   *remaining_rels = list_copy(initial_rels);

	/* Process each relation in the Leading hint order */
	foreach(lc, leading_hint->relnames)
	{
		char	   *relname = (char *) lfirst(lc);
		RelOptInfo *next_rel;

		/* Skip parentheses markers - they're for nested syntax */
		if (strcmp(relname, "(") == 0 || strcmp(relname, ")") == 0)
			continue;

		/* Find this relation in the remaining relations */
		next_rel = find_rel_by_relname(remaining_rels, relname, root);
		if (next_rel == NULL)
		{
			/* Relation not found, hint doesn't match query */
			ereport(DEBUG1,
					(errmsg("Leading hint relation \"%s\" not found in query", relname)));
			return NULL;
		}

		/* Remove from remaining relations */
		remaining_rels = list_delete_ptr(remaining_rels, next_rel);

		if (result_rel == NULL)
		{
			/* First relation becomes the starting point */
			result_rel = next_rel;
		}
		else
		{
			/* Join the accumulated result with the next relation */
			result_rel = join_two_rels(root, result_rel, next_rel);
			if (result_rel == NULL)
			{
				/* Join failed, can't continue with this order */
				ereport(DEBUG1,
						(errmsg("Leading hint join order failed for relation \"%s\"", relname)));
				return NULL;
			}
		}
	}

	/* Join any remaining relations that weren't in the Leading hint */
	foreach(lc, remaining_rels)
	{
		RelOptInfo *next_rel = (RelOptInfo *) lfirst(lc);

		if (result_rel == NULL)
			result_rel = next_rel;
		else
		{
			result_rel = join_two_rels(root, result_rel, next_rel);
			if (result_rel == NULL)
			{
				/* Join failed */
				return NULL;
			}
		}
	}

	return result_rel;
}

/*
 * outline_join_search - Custom join search function with Leading hint support
 *
 * This function is registered as the join_search_hook. It checks if a Leading hint
 * is active, and if so, uses the hinted join order. Otherwise, it falls back to
 * the standard join search algorithm.
 */
static RelOptInfo *
outline_join_search(PlannerInfo *root, int levels_needed, List *initial_rels)
{
	LeadingHint *leading_hint;
	RelOptInfo *result_rel;

	/* Check if we have a Leading hint */
	leading_hint = find_leading_hint(current_hint_state);

	if (leading_hint != NULL && leading_hint->relnames != NIL)
	{
		ereport(DEBUG1,
				(errmsg("Applying Leading hint with %d relations",
						list_length(leading_hint->relnames))));

		/* Try to use the Leading hint to control join order */
		result_rel = outline_join_search_simple(root, leading_hint, initial_rels);

		if (result_rel != NULL)
		{
			/* Successfully applied Leading hint */
			ereport(DEBUG1,
					(errmsg("Leading hint successfully applied")));
			return result_rel;
		}

		/* Leading hint failed, fall through to standard search */
		ereport(DEBUG1,
				(errmsg("Leading hint failed, falling back to standard join search")));
	}

	/* No Leading hint or hint failed, use standard join search */
	return standard_join_search(root, levels_needed, initial_rels);
}

/*
 * Leading Hint Implementation Notes
 * ==================================
 *
 * The Leading hint is designed to control join order, which is one of the most
 * complex aspects of query optimization. Full implementation requires deep
 * integration with PostgreSQL's join enumeration algorithm.
 *
 * Current Status:
 * ---------------
 * - Parsing: IMPLEMENTED (see outline_hints.c:parse_leading_hint_args)
 *   The parser correctly handles both simple and nested Leading hint syntax:
 *   - Simple: Leading(t1 t2 t3) - tables joined left-to-right
 *   - Nested: Leading((t1 t2) t3) - explicit join tree structure
 *
 * - Application: IMPLEMENTED - Simple join order control
 *   The outline_join_search() function enforces Leading hints for simple cases.
 *   For simple syntax (no nested parentheses), tables are joined left-to-right.
 *   Nested syntax with parentheses is parsed but uses simplified join logic.
 *
 * Implementation Details:
 * -----------------------
 * 1. join_search_hook registered in outline_hints_init()
 *    - outline_join_search() replaces standard_join_search() when hints active
 *    - Falls back to standard search if no Leading hint or if hint fails
 *
 * 2. Simple Join Order Implementation:
 *    - outline_join_search_simple() processes relations left-to-right
 *    - Uses make_join_rel() to create joins in the specified order
 *    - Handles cases where hinted join order is not feasible
 *
 * 3. Limitations:
 *    - Nested parentheses syntax is simplified (joins left-to-right)
 *    - Full nested join tree building not yet implemented
 *    - Outer join constraints may restrict applicable join orders
 *
 * Future Enhancements:
 * --------------------
 * - Implement proper nested join tree building for complex syntax
 * - Better handling of outer joins and join constraints
 * - Validation of join order feasibility before attempting
 * - Integration with bushy join tree algorithms
 *
 * References:
 * -----------
 * - pg_hint_plan extension: See how it implements Leading hints
 * - PostgreSQL src/backend/optimizer/path/joinrels.c: Join enumeration logic
 * - PostgreSQL src/backend/optimizer/path/allpaths.c: standard_join_search()
 * - PostgreSQL src/include/optimizer/paths.h: join_search_hook definition
 */
