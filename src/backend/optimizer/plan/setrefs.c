/*-------------------------------------------------------------------------
 *
 * setrefs.c
 *	  Post-processing of a completed plan tree: fix references to subplan
 *	  vars, compute regproc values for operators, etc
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/setrefs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/transam.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/tlist.h"
#include "tcop/utility.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


typedef struct
{
	Index		varno;			/* RT index of Var */
	AttrNumber	varattno;		/* attr number of Var */
	AttrNumber	resno;			/* TLE position of Var */
} tlist_vinfo;

typedef struct
{
	List	   *tlist;			/* underlying target list */
	int			num_vars;		/* number of plain Var tlist entries */
	bool		has_ph_vars;	/* are there PlaceHolderVar entries? */
	bool		has_non_vars;	/* are there other entries? */
	tlist_vinfo vars[FLEXIBLE_ARRAY_MEMBER];	/* has num_vars entries */
} indexed_tlist;

typedef struct
{
	PlannerInfo *root;
	int			rtoffset;
} fix_scan_expr_context;

typedef struct
{
	PlannerInfo *root;
	indexed_tlist *outer_itlist;
	indexed_tlist *inner_itlist;
	Index		acceptable_rel;
	int			rtoffset;
} fix_join_expr_context;

typedef struct
{
	PlannerInfo *root;
	indexed_tlist *subplan_itlist;
	Index		newvarno;
	int			rtoffset;
} fix_upper_expr_context;

/*
 * Check if a Const node is a regclass value.  We accept plain OID too,
 * since a regclass Const will get folded to that type if it's an argument
 * to oideq or similar operators.  (This might result in some extraneous
 * values in a plan's list of relation dependencies, but the worst result
 * would be occasional useless replans.)
 */
#define ISREGCLASSCONST(con) \
	(((con)->consttype == REGCLASSOID || (con)->consttype == OIDOID) && \
	 !(con)->constisnull)

#define fix_scan_list(root, lst, rtoffset) \
	((List *) fix_scan_expr(root, (Node *) (lst), rtoffset))

static void add_rtes_to_flat_rtable(PlannerInfo *root, bool recursing);
static void flatten_unplanned_rtes(PlannerGlobal *glob, RangeTblEntry *rte);
static bool flatten_rtes_walker(Node *node, PlannerGlobal *glob);
static void add_rte_to_flat_rtable(PlannerGlobal *glob, RangeTblEntry *rte);
static Plan *set_plan_refs(PlannerInfo *root, Plan *plan, int rtoffset);
static Plan *set_indexonlyscan_references(PlannerInfo *root,
										  IndexOnlyScan *plan,
										  int rtoffset);
static Plan *set_subqueryscan_references(PlannerInfo *root,
										 SubqueryScan *plan,
										 int rtoffset);
static bool trivial_subqueryscan(SubqueryScan *plan);
static Plan *clean_up_removed_plan_level(Plan *parent, Plan *child);
static void set_foreignscan_references(PlannerInfo *root,
									   ForeignScan *fscan,
									   int rtoffset);
static void set_customscan_references(PlannerInfo *root,
									  CustomScan *cscan,
									  int rtoffset);
static Plan *set_append_references(PlannerInfo *root,
								   Append *aplan,
								   int rtoffset);
static Plan *set_mergeappend_references(PlannerInfo *root,
										MergeAppend *mplan,
										int rtoffset);
static void set_hash_references(PlannerInfo *root, Plan *plan, int rtoffset);
static Node *fix_scan_expr(PlannerInfo *root, Node *node, int rtoffset);
static Node *fix_scan_expr_mutator(Node *node, fix_scan_expr_context *context);
static bool fix_scan_expr_walker(Node *node, fix_scan_expr_context *context);
static void set_join_references(PlannerInfo *root, Join *join, int rtoffset);
static void set_upper_references(PlannerInfo *root, Plan *plan, int rtoffset);
static void set_param_references(PlannerInfo *root, Plan *plan);
static Node *convert_combining_aggrefs(Node *node, void *context);
static void set_dummy_tlist_references(Plan *plan, int rtoffset);
static indexed_tlist *build_tlist_index(List *tlist);
static Var *search_indexed_tlist_for_var(Var *var,
										 indexed_tlist *itlist,
										 Index newvarno,
										 int rtoffset);
static Var *search_indexed_tlist_for_non_var(Expr *node,
											 indexed_tlist *itlist,
											 Index newvarno);
static Var *search_indexed_tlist_for_sortgroupref(Expr *node,
												  Index sortgroupref,
												  indexed_tlist *itlist,
												  Index newvarno);
static List *fix_join_expr(PlannerInfo *root,
						   List *clauses,
						   indexed_tlist *outer_itlist,
						   indexed_tlist *inner_itlist,
						   Index acceptable_rel, int rtoffset);
static Node *fix_join_expr_mutator(Node *node,
								   fix_join_expr_context *context);
static Node *fix_upper_expr(PlannerInfo *root,
							Node *node,
							indexed_tlist *subplan_itlist,
							Index newvarno,
							int rtoffset);
static Node *fix_upper_expr_mutator(Node *node,
									fix_upper_expr_context *context);
static List *set_returning_clause_references(PlannerInfo *root,
											 List *rlist,
											 Plan *topplan,
											 Index resultRelation,
											 int rtoffset);


/*****************************************************************************
 *
 *		SUBPLAN REFERENCES
 *
 *****************************************************************************/

/*
 * set_plan_references
 *
 * This is the final processing pass of the planner/optimizer.  The plan
 * tree is complete; we just have to adjust some representational details
 * for the convenience of the executor:
 *
 * 1. We flatten the various subquery rangetables into a single list, and
 * zero out RangeTblEntry fields that are not useful to the executor.
 *
 * 2. We adjust Vars in scan nodes to be consistent with the flat rangetable.
 *
 * 3. We adjust Vars in upper plan nodes to refer to the outputs of their
 * subplans.
 *
 * 4. Aggrefs in Agg plan nodes need to be adjusted in some cases involving
 * partial aggregation or minmax aggregate optimization.
 *
 * 5. PARAM_MULTIEXPR Params are replaced by regular PARAM_EXEC Params,
 * now that we have finished planning all MULTIEXPR subplans.
 *
 * 6. We compute regproc OIDs for operators (ie, we look up the function
 * that implements each op).
 *
 * 7. We create lists of specific objects that the plan depends on.
 * This will be used by plancache.c to drive invalidation of cached plans.
 * Relation dependencies are represented by OIDs, and everything else by
 * PlanInvalItems (this distinction is motivated by the shared-inval APIs).
 * Currently, relations, user-defined functions, and domains are the only
 * types of objects that are explicitly tracked this way.
 *
 * 8. We assign every plan node in the tree a unique ID.
 *
 * We also perform one final optimization step, which is to delete
 * SubqueryScan, Append, and MergeAppend plan nodes that aren't doing
 * anything useful.  The reason for doing this last is that
 * it can't readily be done before set_plan_references, because it would
 * break set_upper_references: the Vars in the child plan's top tlist
 * wouldn't match up with the Vars in the outer plan tree.  A SubqueryScan
 * serves a necessary function as a buffer between outer query and subquery
 * variable numbering ... but after we've flattened the rangetable this is
 * no longer a problem, since then there's only one rtindex namespace.
 * Likewise, Append and MergeAppend buffer between the parent and child vars
 * of an appendrel, but we don't need to worry about that once we've done
 * set_plan_references.
 *
 * set_plan_references recursively traverses the whole plan tree.
 *
 * The return value is normally the same Plan node passed in, but can be
 * different when the passed-in Plan is a node we decide isn't needed.
 *
 * The flattened rangetable entries are appended to root->glob->finalrtable.
 * Also, rowmarks entries are appended to root->glob->finalrowmarks, and the
 * RT indexes of ModifyTable result relations to root->glob->resultRelations.
 * Plan dependencies are appended to root->glob->relationOids (for relations)
 * and root->glob->invalItems (for everything else).
 *
 * Notice that we modify Plan nodes in-place, but use expression_tree_mutator
 * to process targetlist and qual expressions.  We can assume that the Plan
 * nodes were just built by the planner and are not multiply referenced, but
 * it's not so safe to assume that for expression tree nodes.
 */
Plan *
set_plan_references(PlannerInfo *root, Plan *plan)
{
	PlannerGlobal *glob = root->glob;
	int			rtoffset = list_length(glob->finalrtable);
	ListCell   *lc;

	/*
	 * Add all the query's RTEs to the flattened rangetable.  The live ones
	 * will have their rangetable indexes increased by rtoffset.  (Additional
	 * RTEs, not referenced by the Plan tree, might get added after those.)
	 */
	add_rtes_to_flat_rtable(root, false);

	/*
	 * Adjust RT indexes of PlanRowMarks and add to final rowmarks list
	 */
	foreach(lc, root->rowMarks)
	{
		PlanRowMark *rc = lfirst_node(PlanRowMark, lc);
		PlanRowMark *newrc;

		/* flat copy is enough since all fields are scalars */
		newrc = (PlanRowMark *) palloc(sizeof(PlanRowMark));
		memcpy(newrc, rc, sizeof(PlanRowMark));

		/* adjust indexes ... but *not* the rowmarkId */
		newrc->rti += rtoffset;
		newrc->prti += rtoffset;

		glob->finalrowmarks = lappend(glob->finalrowmarks, newrc);
	}

	/* Now fix the Plan tree */
	return set_plan_refs(root, plan, rtoffset);
}

/*
 * Extract RangeTblEntries from the plan's rangetable, and add to flat rtable
 *
 * This can recurse into subquery plans; "recursing" is true if so.
 */
static void
add_rtes_to_flat_rtable(PlannerInfo *root, bool recursing)
{
	PlannerGlobal *glob = root->glob;
	Index		rti;
	ListCell   *lc;

	/*
	 * Add the query's own RTEs to the flattened rangetable.
	 *
	 * At top level, we must add all RTEs so that their indexes in the
	 * flattened rangetable match up with their original indexes.  When
	 * recursing, we only care about extracting relation RTEs.
	 */
	foreach(lc, root->parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		if (!recursing || rte->rtekind == RTE_RELATION)
			add_rte_to_flat_rtable(glob, rte);
	}

	/*
	 * If there are any dead subqueries, they are not referenced in the Plan
	 * tree, so we must add RTEs contained in them to the flattened rtable
	 * separately.  (If we failed to do this, the executor would not perform
	 * expected permission checks for tables mentioned in such subqueries.)
	 *
	 * Note: this pass over the rangetable can't be combined with the previous
	 * one, because that would mess up the numbering of the live RTEs in the
	 * flattened rangetable.
	 */
	rti = 1;
	foreach(lc, root->parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		/*
		 * We should ignore inheritance-parent RTEs: their contents have been
		 * pulled up into our rangetable already.  Also ignore any subquery
		 * RTEs without matching RelOptInfos, as they likewise have been
		 * pulled up.
		 */
		if (rte->rtekind == RTE_SUBQUERY && !rte->inh &&
			rti < root->simple_rel_array_size)
		{
			RelOptInfo *rel = root->simple_rel_array[rti];

			if (rel != NULL)
			{
				Assert(rel->relid == rti);	/* sanity check on array */

				/*
				 * The subquery might never have been planned at all, if it
				 * was excluded on the basis of self-contradictory constraints
				 * in our query level.  In this case apply
				 * flatten_unplanned_rtes.
				 *
				 * If it was planned but the result rel is dummy, we assume
				 * that it has been omitted from our plan tree (see
				 * set_subquery_pathlist), and recurse to pull up its RTEs.
				 *
				 * Otherwise, it should be represented by a SubqueryScan node
				 * somewhere in our plan tree, and we'll pull up its RTEs when
				 * we process that plan node.
				 *
				 * However, if we're recursing, then we should pull up RTEs
				 * whether the subquery is dummy or not, because we've found
				 * that some upper query level is treating this one as dummy,
				 * and so we won't scan this level's plan tree at all.
				 */
				if (rel->subroot == NULL)
					flatten_unplanned_rtes(glob, rte);
				else if (recursing ||
						 IS_DUMMY_REL(fetch_upper_rel(rel->subroot,
													  UPPERREL_FINAL, NULL)))
					add_rtes_to_flat_rtable(rel->subroot, true);
			}
		}
		rti++;
	}
}

/*
 * Extract RangeTblEntries from a subquery that was never planned at all
 */
static void
flatten_unplanned_rtes(PlannerGlobal *glob, RangeTblEntry *rte)
{
	/* Use query_tree_walker to find all RTEs in the parse tree */
	(void) query_tree_walker(rte->subquery,
							 flatten_rtes_walker,
							 (void *) glob,
							 QTW_EXAMINE_RTES_BEFORE);
}

static bool
flatten_rtes_walker(Node *node, PlannerGlobal *glob)
{
	if (node == NULL)
		return false;
	if (IsA(node, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) node;

		/* As above, we need only save relation RTEs */
		if (rte->rtekind == RTE_RELATION)
			add_rte_to_flat_rtable(glob, rte);
		return false;
	}
	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		return query_tree_walker((Query *) node,
								 flatten_rtes_walker,
								 (void *) glob,
								 QTW_EXAMINE_RTES_BEFORE);
	}
	return expression_tree_walker(node, flatten_rtes_walker,
								  (void *) glob);
}

/*
 * Add (a copy of) the given RTE to the final rangetable
 *
 * In the flat rangetable, we zero out substructure pointers that are not
 * needed by the executor; this reduces the storage space and copying cost
 * for cached plans.  We keep only the ctename, alias and eref Alias fields,
 * which are needed by EXPLAIN, and the selectedCols, insertedCols,
 * updatedCols, and extraUpdatedCols bitmaps, which are needed for
 * executor-startup permissions checking and for trigger event checking.
 */
static void
add_rte_to_flat_rtable(PlannerGlobal *glob, RangeTblEntry *rte)
{
	RangeTblEntry *newrte;

	/* flat copy to duplicate all the scalar fields */
	newrte = (RangeTblEntry *) palloc(sizeof(RangeTblEntry));
	memcpy(newrte, rte, sizeof(RangeTblEntry));

	/* zap unneeded sub-structure */
	newrte->tablesample = NULL;
	newrte->subquery = NULL;
	newrte->joinaliasvars = NIL;
	newrte->functions = NIL;
	newrte->tablefunc = NULL;
	newrte->values_lists = NIL;
	newrte->coltypes = NIL;
	newrte->coltypmods = NIL;
	newrte->colcollations = NIL;
	newrte->securityQuals = NIL;

	glob->finalrtable = lappend(glob->finalrtable, newrte);

	/*
	 * Check for RT index overflow; it's very unlikely, but if it did happen,
	 * the executor would get confused by varnos that match the special varno
	 * values.
	 */
	if (IS_SPECIAL_VARNO(list_length(glob->finalrtable)))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("too many range table entries")));

	/*
	 * If it's a plain relation RTE, add the table to relationOids.
	 *
	 * We do this even though the RTE might be unreferenced in the plan tree;
	 * this would correspond to cases such as views that were expanded, child
	 * tables that were eliminated by constraint exclusion, etc. Schema
	 * invalidation on such a rel must still force rebuilding of the plan.
	 *
	 * Note we don't bother to avoid making duplicate list entries.  We could,
	 * but it would probably cost more cycles than it would save.
	 */
	if (newrte->rtekind == RTE_RELATION)
		glob->relationOids = lappend_oid(glob->relationOids, newrte->relid);
}

/*
 * set_plan_refs: 递归处理单个子查询级别的Plan节点
 *
 * 参数：
 *   root - 规划器全局信息指针，包含查询的所有上下文信息
 *   plan - 需要处理的计划节点指针
 *   rtoffset - 关系表编号偏移量，用于调整Var节点中的关系引用
 *
 * 返回值：
 *   返回处理后的计划节点指针
 *
 * 功能：
 *   该函数是查询优化器中的关键组件，负责为执行计划中的节点设置正确的引用关系，
 *   特别是调整Var节点中的关系编号，确保在查询执行时能正确引用到对应的表和列。
 *   函数通过递归方式处理整个计划树，并根据不同的节点类型采用不同的处理策略。
 */
static Plan *
set_plan_refs(PlannerInfo *root, Plan *plan, int rtoffset)
{
	ListCell   *l;

	/* 处理空计划节点情况 */
	if (plan == NULL)
		return NULL;

	/* 为该计划节点分配唯一ID */
	plan->plan_node_id = root->glob->lastPlanNodeId++;

	/*
	 * 根据不同的计划节点类型执行特定的修复操作
	 */
	switch (nodeTag(plan))
	{
		case T_SeqScan: /* 顺序扫描节点处理 */
			{
				SeqScan    *splan = (SeqScan *) plan;

				/* 调整扫描关系编号，应用偏移量 */
				splan->scanrelid += rtoffset;
				/* 修复目标列列表中的引用 */
				splan->plan.targetlist = 
					fix_scan_list(root, splan->plan.targetlist, rtoffset);
				/* 修复过滤条件中的引用 */
				splan->plan.qual = 
					fix_scan_list(root, splan->plan.qual, rtoffset);
			}
			break;
		case T_SampleScan: /* 表采样扫描节点处理 */
			{
				SampleScan *splan = (SampleScan *) plan;

				/* 调整扫描关系编号 */
				splan->scan.scanrelid += rtoffset;
				/* 修复目标列和过滤条件 */
				splan->scan.plan.targetlist = 
					fix_scan_list(root, splan->scan.plan.targetlist, rtoffset);
				splan->scan.plan.qual = 
					fix_scan_list(root, splan->scan.plan.qual, rtoffset);
				/* 修复表采样子句 */
				splan->tablesample = (TableSampleClause *)
					fix_scan_expr(root, (Node *) splan->tablesample, rtoffset);
			}
			break;
		case T_IndexScan: /* 索引扫描节点处理 */
			{
				IndexScan  *splan = (IndexScan *) plan;

				/* 调整扫描关系编号 */
				splan->scan.scanrelid += rtoffset;
				/* 修复目标列和过滤条件 */
				splan->scan.plan.targetlist = 
					fix_scan_list(root, splan->scan.plan.targetlist, rtoffset);
				splan->scan.plan.qual = 
					fix_scan_list(root, splan->scan.plan.qual, rtoffset);
				/* 修复索引条件相关表达式 */
				splan->indexqual = 
					fix_scan_list(root, splan->indexqual, rtoffset);
				splan->indexqualorig = 
					fix_scan_list(root, splan->indexqualorig, rtoffset);
				splan->indexorderby = 
					fix_scan_list(root, splan->indexorderby, rtoffset);
				splan->indexorderbyorig = 
					fix_scan_list(root, splan->indexorderbyorig, rtoffset);
			}
			break;
		case T_IndexOnlyScan: /* 仅索引扫描节点处理 */
			{
				IndexOnlyScan *splan = (IndexOnlyScan *) plan;

				/* 使用专门的函数处理仅索引扫描 */
				return set_indexonlyscan_references(root, splan, rtoffset);
			}
			break;
		case T_BitmapIndexScan: /* 位图索引扫描节点处理 */
			{
				BitmapIndexScan *splan = (BitmapIndexScan *) plan;

				/* 调整扫描关系编号 */
				splan->scan.scanrelid += rtoffset;
				/* 位图索引扫描不应该有目标列和过滤条件 */
				Assert(splan->scan.plan.targetlist == NIL);
				Assert(splan->scan.plan.qual == NIL);
				/* 修复索引条件 */
				splan->indexqual = 
					fix_scan_list(root, splan->indexqual, rtoffset);
				splan->indexqualorig = 
					fix_scan_list(root, splan->indexqualorig, rtoffset);
			}
			break;
		case T_BitmapHeapScan: /* 位图堆扫描节点处理 */
			{
				BitmapHeapScan *splan = (BitmapHeapScan *) plan;

				/* 调整扫描关系编号 */
				splan->scan.scanrelid += rtoffset;
				/* 修复目标列和过滤条件 */
				splan->scan.plan.targetlist = 
					fix_scan_list(root, splan->scan.plan.targetlist, rtoffset);
				splan->scan.plan.qual = 
					fix_scan_list(root, splan->scan.plan.qual, rtoffset);
				/* 修复位图条件 */
				splan->bitmapqualorig = 
					fix_scan_list(root, splan->bitmapqualorig, rtoffset);
			}
			break;
		case T_TidScan: /* TID扫描节点处理 */
			{
				TidScan    *splan = (TidScan *) plan;

				/* 调整扫描关系编号 */
				splan->scan.scanrelid += rtoffset;
				/* 修复目标列和过滤条件 */
				splan->scan.plan.targetlist = 
					fix_scan_list(root, splan->scan.plan.targetlist, rtoffset);
				splan->scan.plan.qual = 
					fix_scan_list(root, splan->scan.plan.qual, rtoffset);
				/* 修复TID条件 */
				splan->tidquals = 
					fix_scan_list(root, splan->tidquals, rtoffset);
			}
			break;
		case T_SubqueryScan: /* 子查询扫描节点处理 */
			/* 需要特殊处理，见相关函数注释 */
			return set_subqueryscan_references(root, 
									  (SubqueryScan *) plan, 
									  rtoffset);
		case T_FunctionScan: /* 函数扫描节点处理 */
			{
				FunctionScan *splan = (FunctionScan *) plan;

				/* 调整扫描关系编号 */
				splan->scan.scanrelid += rtoffset;
				/* 修复目标列和过滤条件 */
				splan->scan.plan.targetlist = 
					fix_scan_list(root, splan->scan.plan.targetlist, rtoffset);
				splan->scan.plan.qual = 
					fix_scan_list(root, splan->scan.plan.qual, rtoffset);
				/* 修复函数表达式列表 */
				splan->functions = 
					fix_scan_list(root, splan->functions, rtoffset);
			}
			break;
		case T_TableFuncScan: /* 表函数扫描节点处理 */
			{
				TableFuncScan *splan = (TableFuncScan *) plan;

				/* 调整扫描关系编号 */
				splan->scan.scanrelid += rtoffset;
				/* 修复目标列和过滤条件 */
				splan->scan.plan.targetlist = 
					fix_scan_list(root, splan->scan.plan.targetlist, rtoffset);
				splan->scan.plan.qual = 
					fix_scan_list(root, splan->scan.plan.qual, rtoffset);
				/* 修复表函数表达式 */
				splan->tablefunc = (TableFunc *)
					fix_scan_expr(root, (Node *) splan->tablefunc, rtoffset);
			}
			break;
		case T_ValuesScan: /* 值扫描节点处理 */
			{
				ValuesScan *splan = (ValuesScan *) plan;

				/* 调整扫描关系编号 */
				splan->scan.scanrelid += rtoffset;
				/* 修复目标列和过滤条件 */
				splan->scan.plan.targetlist = 
					fix_scan_list(root, splan->scan.plan.targetlist, rtoffset);
				splan->scan.plan.qual = 
					fix_scan_list(root, splan->scan.plan.qual, rtoffset);
				/* 修复值列表 */
				splan->values_lists = 
					fix_scan_list(root, splan->values_lists, rtoffset);
			}
			break;
		case T_CteScan: /* CTE扫描节点处理 */
			{
				CteScan    *splan = (CteScan *) plan;

				/* 调整扫描关系编号 */
				splan->scan.scanrelid += rtoffset;
				/* 修复目标列和过滤条件 */
				splan->scan.plan.targetlist = 
					fix_scan_list(root, splan->scan.plan.targetlist, rtoffset);
				splan->scan.plan.qual = 
					fix_scan_list(root, splan->scan.plan.qual, rtoffset);
			}
			break;
		case T_NamedTuplestoreScan: /* 命名元组存储扫描节点处理 */
			{
				NamedTuplestoreScan *splan = (NamedTuplestoreScan *) plan;

				/* 调整扫描关系编号 */
				splan->scan.scanrelid += rtoffset;
				/* 修复目标列和过滤条件 */
				splan->scan.plan.targetlist = 
					fix_scan_list(root, splan->scan.plan.targetlist, rtoffset);
				splan->scan.plan.qual = 
					fix_scan_list(root, splan->scan.plan.qual, rtoffset);
			}
			break;
		case T_WorkTableScan: /* 工作表扫描节点处理 */
			{
				WorkTableScan *splan = (WorkTableScan *) plan;

				/* 调整扫描关系编号 */
				splan->scan.scanrelid += rtoffset;
				/* 修复目标列和过滤条件 */
				splan->scan.plan.targetlist = 
					fix_scan_list(root, splan->scan.plan.targetlist, rtoffset);
				splan->scan.plan.qual = 
					fix_scan_list(root, splan->scan.plan.qual, rtoffset);
			}
			break;
		case T_ForeignScan: /* 外部表扫描节点处理 */
			set_foreignscan_references(root, (ForeignScan *) plan, rtoffset);
			break;
		case T_CustomScan: /* 自定义扫描节点处理 */
			set_customscan_references(root, (CustomScan *) plan, rtoffset);
			break;

		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin: /* 连接节点处理 */
			set_join_references(root, (Join *) plan, rtoffset);
			break;

		case T_Gather:
		case T_GatherMerge: /* 并行执行节点处理 */
			{
				/* 设置上层节点引用 */
				set_upper_references(root, plan, rtoffset);
				/* 设置参数引用 */
				set_param_references(root, plan);
			}
			break;

		case T_Hash: /* 哈希节点处理 */
			set_hash_references(root, plan, rtoffset);
			break;

		case T_Material: /* 物化节点处理 */
		case T_Sort: /* 排序节点处理 */
		case T_Unique: /* 去重节点处理 */
		case T_SetOp: /* 集合操作节点处理 */

			/*
			 * 这些计划类型实际上不会计算它们的目标列列表，因为它们只是返回未修改的输入元组。
			 * 即使目标列列表不会被执行器使用，我们也会修复它以供EXPLAIN使用
			 * （更不用说调试的便利性 - 错误的varnos非常令人困惑）。
			 */
			set_dummy_tlist_references(plan, rtoffset);

			/*
			 * 由于这些计划类型也不检查条件，所以不应该有任何条件表达式附加到它们。
			 */
			Assert(plan->qual == NIL);
			break;
		case T_LockRows: /* 行锁节点处理 */
			{
				LockRows   *splan = (LockRows *) plan;

				/*
				 * 与上面的计划类型类似，LockRows不计算其目标列或条件。
				 * 但我们必须修复其行标记中的RT索引。
				 */
				set_dummy_tlist_references(plan, rtoffset);
				Assert(splan->plan.qual == NIL);

				/* 修复行标记中的关系索引 */
				foreach(l, splan->rowMarks)
				{
					PlanRowMark *rc = (PlanRowMark *) lfirst(l);

					rc->rti += rtoffset;
					rc->prti += rtoffset;
				}
			}
			break;
		case T_Limit: /* 限制节点处理 */
			{
				Limit	   *splan = (Limit *) plan;

				/*
				 * 与上面的计划类型类似，Limit不计算其目标列或条件。
				 * 但是它确实有用于limit/offset的实际表达式；并且这些表达式不能包含子计划变量引用，
				 * 所以fix_scan_expr适用于它们。
				 */
				set_dummy_tlist_references(plan, rtoffset);
				Assert(splan->plan.qual == NIL);

				/* 修复limit和offset表达式 */
				splan->limitOffset = 
					fix_scan_expr(root, splan->limitOffset, rtoffset);
				splan->limitCount = 
					fix_scan_expr(root, splan->limitCount, rtoffset);
			}
			break;
		case T_Agg: /* 聚合节点处理 */
			{
				Agg		   *agg = (Agg *) plan;

				/*
				 * 如果此节点正在合并 Partial Aggregate 结果，我们必须转换其Aggrefs以包含对
				 * 将从子计划节点获得的部分聚合子表达式的引用。
				 */
				if (DO_AGGSPLIT_COMBINE(agg->aggsplit))
				{
					plan->targetlist = (List *)
						convert_combining_aggrefs((Node *) plan->targetlist,
										NULL);
					plan->qual = (List *)
						convert_combining_aggrefs((Node *) plan->qual,
										NULL);
				}

				/* 设置上层节点引用 */
				set_upper_references(root, plan, rtoffset);
			}
			break;
		case T_Group: /* 分组节点处理 */
			set_upper_references(root, plan, rtoffset);
			break;
		case T_WindowAgg: /* 窗口聚合节点处理 */
			{
				WindowAgg  *wplan = (WindowAgg *) plan;

				/* 设置上层节点引用 */
				set_upper_references(root, plan, rtoffset);

				/*
				 * 类似于Limit节点的limit/offset表达式，WindowAgg有框架偏移表达式，
				 * 这些表达式不能包含子计划变量引用，所以fix_scan_expr适用于它们。
				 */
				wplan->startOffset = 
					fix_scan_expr(root, wplan->startOffset, rtoffset);
				wplan->endOffset = 
					fix_scan_expr(root, wplan->endOffset, rtoffset);
			}
			break;
		case T_Result: /* 结果节点处理 */
			{
				Result	   *splan = (Result *) plan;

				/*
				 * Result节点可能有也可能没有子计划；如果没有，则更像是扫描节点而不是上层节点。
				 */
				if (splan->plan.lefttree != NULL)
					set_upper_references(root, plan, rtoffset);
				else
				{
					/* 没有子计划时，修复目标列和条件 */
					splan->plan.targetlist = 
						fix_scan_list(root, splan->plan.targetlist, rtoffset);
					splan->plan.qual = 
						fix_scan_list(root, splan->plan.qual, rtoffset);
				}
				/* resconstantqual不能包含任何子计划变量引用 */
				splan->resconstantqual = 
					fix_scan_expr(root, splan->resconstantqual, rtoffset);
			}
			break;
		case T_ProjectSet: /* 投影集节点处理 */
			set_upper_references(root, plan, rtoffset);
			break;
		case T_ModifyTable: /* 修改表节点处理 */
			{
				ModifyTable *splan = (ModifyTable *) plan;

				/* 修改表节点不应该有目标列和条件 */
				Assert(splan->plan.targetlist == NIL);
				Assert(splan->plan.qual == NIL);

				/* 修复CHECK OPTION条件列表 */
				splan->withCheckOptionLists = 
					fix_scan_list(root, splan->withCheckOptionLists, rtoffset);

				/* 处理RETURNING子句 */
				if (splan->returningLists)
				{
					List	   *newRL = NIL;
					ListCell   *lcrl,
							   *lcrr,
							   *lcp;

					/*
					 * 为每个子计划的returningList调用set_returning_clause_references()
					 */
					Assert(list_length(splan->returningLists) == list_length(splan->resultRelations));
					Assert(list_length(splan->returningLists) == list_length(splan->plans));
					/* 同时遍历三个列表：returningLists、resultRelations和plans */
					forthree(lcrl, splan->returningLists,
						 lcrr, splan->resultRelations,
						 lcp, splan->plans)
					{
						List	   *rlist = (List *) lfirst(lcrl);
						Index		resultrel = lfirst_int(lcrr);
						Plan	   *subplan = (Plan *) lfirst(lcp);

						rlist = set_returning_clause_references(root,
											 rlist,
											 subplan,
											 resultrel,
											 rtoffset);
						newRL = lappend(newRL, rlist);
					}
					splan->returningLists = newRL;

					/*
					 * 将可见的计划目标列列表设置为与第一个RETURNING列表相同。
					 * 这是供EXPLAIN使用的；执行器不会关注目标列列表。
					 * 我们将此步骤推迟到这里，以便不必对相同的目标列列表执行两次set_returning_clause_references()
					 */
					splan->plan.targetlist = copyObject(linitial(newRL));
				}

				/*
				 * 我们将带有ON CONFLICT的ModifyTable视为一种'伪连接'，其中内部是EXCLUDED元组。
				 * 因此，使用fix_join_expr将相关变量设置为INNER_VAR。
				 * 我们明确不创建任何OUTER_VAR，因为它们已经被RETURNING使用，避免冲突更好。
				 */
				if (splan->onConflictSet)
				{
					indexed_tlist *itlist;

					itlist = build_tlist_index(splan->exclRelTlist);

					splan->onConflictSet = 
						fix_join_expr(root, splan->onConflictSet,
							   NULL, itlist,
							   linitial_int(splan->resultRelations),
							   rtoffset);

					splan->onConflictWhere = (Node *)
						fix_join_expr(root, (List *) splan->onConflictWhere,
							   NULL, itlist,
							   linitial_int(splan->resultRelations),
							   rtoffset);

					pfree(itlist);

					splan->exclRelTlist = 
						fix_scan_list(root, splan->exclRelTlist, rtoffset);
				}

				/* 调整各种关系引用 */
				splan->nominalRelation += rtoffset;
				if (splan->rootRelation)
					splan->rootRelation += rtoffset;
				splan->exclRelRTI += rtoffset;

				/* 调整结果关系索引列表 */
				foreach(l, splan->resultRelations)
				{
					lfirst_int(l) += rtoffset;
				}
				/* 调整行标记中的关系索引 */
				foreach(l, splan->rowMarks)
				{
					PlanRowMark *rc = (PlanRowMark *) lfirst(l);

					rc->rti += rtoffset;
					rc->prti += rtoffset;
				}
				/* 递归处理子计划 */
				foreach(l, splan->plans)
				{
					lfirst(l) = set_plan_refs(root,
							   (Plan *) lfirst(l),
							   rtoffset);
				}

				/*
				 * 将此ModifyTable节点的最终结果关系RT索引附加到计划的全局列表中，
				 * 并设置其resultRelIndex以反映它们在全局列表中的起始位置。
				 */
				splan->resultRelIndex = list_length(root->glob->resultRelations);
				root->glob->resultRelations = 
					list_concat(root->glob->resultRelations,
						   list_copy(splan->resultRelations));

				/*
				 * 如果主目标关系是分区表，还将分区根的RT索引添加到rootResultRelations，
				 * 并在该列表中记住其索引在rootResultRelIndex中。
				 */
				if (splan->rootRelation)
				{
					splan->rootResultRelIndex = 
						list_length(root->glob->rootResultRelations);
					root->glob->rootResultRelations = 
						lappend_int(root->glob->rootResultRelations,
							   splan->rootRelation);
				}
			}
			break;
		case T_Append: /* 追加节点处理 */
			/* 需要特殊处理，见相关函数注释 */
			return set_append_references(root,
							   (Append *) plan,
							   rtoffset);
		case T_MergeAppend: /* 合并追加节点处理 */
			/* 需要特殊处理，见相关函数注释 */
			return set_mergeappend_references(root,
							      (MergeAppend *) plan,
							      rtoffset);
		case T_RecursiveUnion: /* 递归联合节点处理 */
			/* 这个节点也不计算目标列或检查条件 */
			set_dummy_tlist_references(plan, rtoffset);
			Assert(plan->qual == NIL);
			break;
		case T_BitmapAnd: /* 位图AND节点处理 */
			{
				BitmapAnd  *splan = (BitmapAnd *) plan;

				/* BitmapAnd的工作方式类似于Append，但没有目标列列表 */
				Assert(splan->plan.targetlist == NIL);
				Assert(splan->plan.qual == NIL);
				/* 递归处理所有位图子计划 */
				foreach(l, splan->bitmapplans)
				{
					lfirst(l) = set_plan_refs(root,
							   (Plan *) lfirst(l),
							   rtoffset);
				}
			}
			break;
		case T_BitmapOr: /* 位图OR节点处理 */
			{
				BitmapOr   *splan = (BitmapOr *) plan;

				/* BitmapOr的工作方式类似于Append，但没有目标列列表 */
				Assert(splan->plan.targetlist == NIL);
				Assert(splan->plan.qual == NIL);
				/* 递归处理所有位图子计划 */
				foreach(l, splan->bitmapplans)
				{
					lfirst(l) = set_plan_refs(root,
							   (Plan *) lfirst(l),
							   rtoffset);
				}
			}
			break;
		default:
			/* 处理未识别的节点类型 */
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(plan));
			break;
	}

	/*
	 * 现在递归处理子计划（如果有）
	 *
	 * 注意：至关重要的是，我们必须在设置此计划的目标列和条件中的子计划引用之后
	 * 再递归到子计划。如果我们自下而上进行引用调整，那么我们将无法匹配此计划的
	 * var节点与已修改的子节点。
	 */
	plan->lefttree = set_plan_refs(root, plan->lefttree, rtoffset);
	plan->righttree = set_plan_refs(root, plan->righttree, rtoffset);

	return plan;
}


/*
 * set_indexonlyscan_references
 *		Do set_plan_references processing on an IndexOnlyScan
 *
 * This is unlike the handling of a plain IndexScan because we have to
 * convert Vars referencing the heap into Vars referencing the index.
 * We can use the fix_upper_expr machinery for that, by working from a
 * targetlist describing the index columns.
 */
static Plan *
set_indexonlyscan_references(PlannerInfo *root,
							 IndexOnlyScan *plan,
							 int rtoffset)
{
	indexed_tlist *index_itlist;
	List	   *stripped_indextlist;
	ListCell   *lc;

	/*
	 * Vars in the plan node's targetlist, qual, and recheckqual must only
	 * reference columns that the index AM can actually return.  To ensure
	 * this, remove non-returnable columns (which are marked as resjunk) from
	 * the indexed tlist.  We can just drop them because the indexed_tlist
	 * machinery pays attention to TLE resnos, not physical list position.
	 */
	stripped_indextlist = NIL;
	foreach(lc, plan->indextlist)
	{
		TargetEntry *indextle = (TargetEntry *) lfirst(lc);

		if (!indextle->resjunk)
			stripped_indextlist = lappend(stripped_indextlist, indextle);
	}

	index_itlist = build_tlist_index(stripped_indextlist);

	plan->scan.scanrelid += rtoffset;
	plan->scan.plan.targetlist = (List *)
		fix_upper_expr(root,
					   (Node *) plan->scan.plan.targetlist,
					   index_itlist,
					   INDEX_VAR,
					   rtoffset);
	plan->scan.plan.qual = (List *)
		fix_upper_expr(root,
					   (Node *) plan->scan.plan.qual,
					   index_itlist,
					   INDEX_VAR,
					   rtoffset);
	plan->recheckqual = (List *)
		fix_upper_expr(root,
					   (Node *) plan->recheckqual,
					   index_itlist,
					   INDEX_VAR,
					   rtoffset);
	/* indexqual is already transformed to reference index columns */
	plan->indexqual = fix_scan_list(root, plan->indexqual, rtoffset);
	/* indexorderby is already transformed to reference index columns */
	plan->indexorderby = fix_scan_list(root, plan->indexorderby, rtoffset);
	/* indextlist must NOT be transformed to reference index columns */
	plan->indextlist = fix_scan_list(root, plan->indextlist, rtoffset);

	pfree(index_itlist);

	return (Plan *) plan;
}

/*
 * set_subqueryscan_references
 *		Do set_plan_references processing on a SubqueryScan
 *
 * We try to strip out the SubqueryScan entirely; if we can't, we have
 * to do the normal processing on it.
 */
static Plan *
set_subqueryscan_references(PlannerInfo *root,
							SubqueryScan *plan,
							int rtoffset)
{
	RelOptInfo *rel;
	Plan	   *result;

	/* Need to look up the subquery's RelOptInfo, since we need its subroot */
	rel = find_base_rel(root, plan->scan.scanrelid);

	/* Recursively process the subplan */
	plan->subplan = set_plan_references(rel->subroot, plan->subplan);

	if (trivial_subqueryscan(plan))
	{
		/*
		 * We can omit the SubqueryScan node and just pull up the subplan.
		 */
		result = clean_up_removed_plan_level((Plan *) plan, plan->subplan);
	}
	else
	{
		/*
		 * Keep the SubqueryScan node.  We have to do the processing that
		 * set_plan_references would otherwise have done on it.  Notice we do
		 * not do set_upper_references() here, because a SubqueryScan will
		 * always have been created with correct references to its subplan's
		 * outputs to begin with.
		 */
		plan->scan.scanrelid += rtoffset;
		plan->scan.plan.targetlist =
			fix_scan_list(root, plan->scan.plan.targetlist, rtoffset);
		plan->scan.plan.qual =
			fix_scan_list(root, plan->scan.plan.qual, rtoffset);

		result = (Plan *) plan;
	}

	return result;
}

/*
 * trivial_subqueryscan
 *		Detect whether a SubqueryScan can be deleted from the plan tree.
 *
 * We can delete it if it has no qual to check and the targetlist just
 * regurgitates the output of the child plan.
 */
static bool
trivial_subqueryscan(SubqueryScan *plan)
{
	int			attrno;
	ListCell   *lp,
			   *lc;

	if (plan->scan.plan.qual != NIL)
		return false;

	if (list_length(plan->scan.plan.targetlist) !=
		list_length(plan->subplan->targetlist))
		return false;			/* tlists not same length */

	attrno = 1;
	forboth(lp, plan->scan.plan.targetlist, lc, plan->subplan->targetlist)
	{
		TargetEntry *ptle = (TargetEntry *) lfirst(lp);
		TargetEntry *ctle = (TargetEntry *) lfirst(lc);

		if (ptle->resjunk != ctle->resjunk)
			return false;		/* tlist doesn't match junk status */

		/*
		 * We accept either a Var referencing the corresponding element of the
		 * subplan tlist, or a Const equaling the subplan element. See
		 * generate_setop_tlist() for motivation.
		 */
		if (ptle->expr && IsA(ptle->expr, Var))
		{
			Var		   *var = (Var *) ptle->expr;

			Assert(var->varno == plan->scan.scanrelid);
			Assert(var->varlevelsup == 0);
			if (var->varattno != attrno)
				return false;	/* out of order */
		}
		else if (ptle->expr && IsA(ptle->expr, Const))
		{
			if (!equal(ptle->expr, ctle->expr))
				return false;
		}
		else
			return false;

		attrno++;
	}

	return true;
}

/*
 * clean_up_removed_plan_level
 *		Do necessary cleanup when we strip out a SubqueryScan, Append, etc
 *
 * We are dropping the "parent" plan in favor of returning just its "child".
 * A few small tweaks are needed.
 */
static Plan *
clean_up_removed_plan_level(Plan *parent, Plan *child)
{
	/*
	 * We have to be sure we don't lose any initplans, so move any that were
	 * attached to the parent plan to the child.  If we do move any, the child
	 * is no longer parallel-safe.
	 */
	if (parent->initPlan)
		child->parallel_safe = false;

	/*
	 * Attach plans this way so that parent's initplans are processed before
	 * any pre-existing initplans of the child.  Probably doesn't matter, but
	 * let's preserve the ordering just in case.
	 */
	child->initPlan = list_concat(parent->initPlan,
								  child->initPlan);

	/*
	 * We also have to transfer the parent's column labeling info into the
	 * child, else columns sent to client will be improperly labeled if this
	 * is the topmost plan level.  resjunk and so on may be important too.
	 */
	apply_tlist_labeling(child->targetlist, parent->targetlist);

	return child;
}

/*
 * set_foreignscan_references
 *	   Do set_plan_references processing on a ForeignScan
 */
static void
set_foreignscan_references(PlannerInfo *root,
						   ForeignScan *fscan,
						   int rtoffset)
{
	/* Adjust scanrelid if it's valid */
	if (fscan->scan.scanrelid > 0)
		fscan->scan.scanrelid += rtoffset;

	if (fscan->fdw_scan_tlist != NIL || fscan->scan.scanrelid == 0)
	{
		/*
		 * Adjust tlist, qual, fdw_exprs, fdw_recheck_quals to reference
		 * foreign scan tuple
		 */
		indexed_tlist *itlist = build_tlist_index(fscan->fdw_scan_tlist);

		fscan->scan.plan.targetlist = (List *)
			fix_upper_expr(root,
						   (Node *) fscan->scan.plan.targetlist,
						   itlist,
						   INDEX_VAR,
						   rtoffset);
		fscan->scan.plan.qual = (List *)
			fix_upper_expr(root,
						   (Node *) fscan->scan.plan.qual,
						   itlist,
						   INDEX_VAR,
						   rtoffset);
		fscan->fdw_exprs = (List *)
			fix_upper_expr(root,
						   (Node *) fscan->fdw_exprs,
						   itlist,
						   INDEX_VAR,
						   rtoffset);
		fscan->fdw_recheck_quals = (List *)
			fix_upper_expr(root,
						   (Node *) fscan->fdw_recheck_quals,
						   itlist,
						   INDEX_VAR,
						   rtoffset);
		pfree(itlist);
		/* fdw_scan_tlist itself just needs fix_scan_list() adjustments */
		fscan->fdw_scan_tlist =
			fix_scan_list(root, fscan->fdw_scan_tlist, rtoffset);
	}
	else
	{
		/*
		 * Adjust tlist, qual, fdw_exprs, fdw_recheck_quals in the standard
		 * way
		 */
		fscan->scan.plan.targetlist =
			fix_scan_list(root, fscan->scan.plan.targetlist, rtoffset);
		fscan->scan.plan.qual =
			fix_scan_list(root, fscan->scan.plan.qual, rtoffset);
		fscan->fdw_exprs =
			fix_scan_list(root, fscan->fdw_exprs, rtoffset);
		fscan->fdw_recheck_quals =
			fix_scan_list(root, fscan->fdw_recheck_quals, rtoffset);
	}

	/* Adjust fs_relids if needed */
	if (rtoffset > 0)
	{
		Bitmapset  *tempset = NULL;
		int			x = -1;

		while ((x = bms_next_member(fscan->fs_relids, x)) >= 0)
			tempset = bms_add_member(tempset, x + rtoffset);
		fscan->fs_relids = tempset;
	}
}

/*
 * set_customscan_references
 *	   Do set_plan_references processing on a CustomScan
 */
static void
set_customscan_references(PlannerInfo *root,
						  CustomScan *cscan,
						  int rtoffset)
{
	ListCell   *lc;

	/* Adjust scanrelid if it's valid */
	if (cscan->scan.scanrelid > 0)
		cscan->scan.scanrelid += rtoffset;

	if (cscan->custom_scan_tlist != NIL || cscan->scan.scanrelid == 0)
	{
		/* Adjust tlist, qual, custom_exprs to reference custom scan tuple */
		indexed_tlist *itlist = build_tlist_index(cscan->custom_scan_tlist);

		cscan->scan.plan.targetlist = (List *)
			fix_upper_expr(root,
						   (Node *) cscan->scan.plan.targetlist,
						   itlist,
						   INDEX_VAR,
						   rtoffset);
		cscan->scan.plan.qual = (List *)
			fix_upper_expr(root,
						   (Node *) cscan->scan.plan.qual,
						   itlist,
						   INDEX_VAR,
						   rtoffset);
		cscan->custom_exprs = (List *)
			fix_upper_expr(root,
						   (Node *) cscan->custom_exprs,
						   itlist,
						   INDEX_VAR,
						   rtoffset);
		pfree(itlist);
		/* custom_scan_tlist itself just needs fix_scan_list() adjustments */
		cscan->custom_scan_tlist =
			fix_scan_list(root, cscan->custom_scan_tlist, rtoffset);
	}
	else
	{
		/* Adjust tlist, qual, custom_exprs in the standard way */
		cscan->scan.plan.targetlist =
			fix_scan_list(root, cscan->scan.plan.targetlist, rtoffset);
		cscan->scan.plan.qual =
			fix_scan_list(root, cscan->scan.plan.qual, rtoffset);
		cscan->custom_exprs =
			fix_scan_list(root, cscan->custom_exprs, rtoffset);
	}

	/* Adjust child plan-nodes recursively, if needed */
	foreach(lc, cscan->custom_plans)
	{
		lfirst(lc) = set_plan_refs(root, (Plan *) lfirst(lc), rtoffset);
	}

	/* Adjust custom_relids if needed */
	if (rtoffset > 0)
	{
		Bitmapset  *tempset = NULL;
		int			x = -1;

		while ((x = bms_next_member(cscan->custom_relids, x)) >= 0)
			tempset = bms_add_member(tempset, x + rtoffset);
		cscan->custom_relids = tempset;
	}
}

/*
 * set_append_references
 *		Do set_plan_references processing on an Append
 *
 * We try to strip out the Append entirely; if we can't, we have
 * to do the normal processing on it.
 */
static Plan *
set_append_references(PlannerInfo *root,
					  Append *aplan,
					  int rtoffset)
{
	ListCell   *l;

	/*
	 * Append, like Sort et al, doesn't actually evaluate its targetlist or
	 * check quals.  If it's got exactly one child plan, then it's not doing
	 * anything useful at all, and we can strip it out.
	 */
	Assert(aplan->plan.qual == NIL);

	/* First, we gotta recurse on the children */
	foreach(l, aplan->appendplans)
	{
		lfirst(l) = set_plan_refs(root, (Plan *) lfirst(l), rtoffset);
	}

	/*
	 * See if it's safe to get rid of the Append entirely.  For this to be
	 * safe, there must be only one child plan and that child plan's parallel
	 * awareness must match that of the Append's.  The reason for the latter
	 * is that the if the Append is parallel aware and the child is not then
	 * the calling plan may execute the non-parallel aware child multiple
	 * times.
	 */
	if (list_length(aplan->appendplans) == 1 &&
		((Plan *) linitial(aplan->appendplans))->parallel_aware == aplan->plan.parallel_aware)
		return clean_up_removed_plan_level((Plan *) aplan,
										   (Plan *) linitial(aplan->appendplans));

	/*
	 * Otherwise, clean up the Append as needed.  It's okay to do this after
	 * recursing to the children, because set_dummy_tlist_references doesn't
	 * look at those.
	 */
	set_dummy_tlist_references((Plan *) aplan, rtoffset);

	if (aplan->part_prune_info)
	{
		foreach(l, aplan->part_prune_info->prune_infos)
		{
			List	   *prune_infos = lfirst(l);
			ListCell   *l2;

			foreach(l2, prune_infos)
			{
				PartitionedRelPruneInfo *pinfo = lfirst(l2);

				pinfo->rtindex += rtoffset;
			}
		}
	}

	/* We don't need to recurse to lefttree or righttree ... */
	Assert(aplan->plan.lefttree == NULL);
	Assert(aplan->plan.righttree == NULL);

	return (Plan *) aplan;
}

/*
 * set_mergeappend_references
 *		Do set_plan_references processing on a MergeAppend
 *
 * We try to strip out the MergeAppend entirely; if we can't, we have
 * to do the normal processing on it.
 */
static Plan *
set_mergeappend_references(PlannerInfo *root,
						   MergeAppend *mplan,
						   int rtoffset)
{
	ListCell   *l;

	/*
	 * MergeAppend, like Sort et al, doesn't actually evaluate its targetlist
	 * or check quals.  If it's got exactly one child plan, then it's not
	 * doing anything useful at all, and we can strip it out.
	 */
	Assert(mplan->plan.qual == NIL);

	/* First, we gotta recurse on the children */
	foreach(l, mplan->mergeplans)
	{
		lfirst(l) = set_plan_refs(root, (Plan *) lfirst(l), rtoffset);
	}

	/*
	 * See if it's safe to get rid of the MergeAppend entirely.  For this to
	 * be safe, there must be only one child plan and that child plan's
	 * parallel awareness must match that of the MergeAppend's.  The reason
	 * for the latter is that the if the MergeAppend is parallel aware and the
	 * child is not then the calling plan may execute the non-parallel aware
	 * child multiple times.
	 */
	if (list_length(mplan->mergeplans) == 1 &&
		((Plan *) linitial(mplan->mergeplans))->parallel_aware == mplan->plan.parallel_aware)
		return clean_up_removed_plan_level((Plan *) mplan,
										   (Plan *) linitial(mplan->mergeplans));

	/*
	 * Otherwise, clean up the MergeAppend as needed.  It's okay to do this
	 * after recursing to the children, because set_dummy_tlist_references
	 * doesn't look at those.
	 */
	set_dummy_tlist_references((Plan *) mplan, rtoffset);

	if (mplan->part_prune_info)
	{
		foreach(l, mplan->part_prune_info->prune_infos)
		{
			List	   *prune_infos = lfirst(l);
			ListCell   *l2;

			foreach(l2, prune_infos)
			{
				PartitionedRelPruneInfo *pinfo = lfirst(l2);

				pinfo->rtindex += rtoffset;
			}
		}
	}

	/* We don't need to recurse to lefttree or righttree ... */
	Assert(mplan->plan.lefttree == NULL);
	Assert(mplan->plan.righttree == NULL);

	return (Plan *) mplan;
}

/*
 * set_hash_references
 *	   Do set_plan_references processing on a Hash node
 */
static void
set_hash_references(PlannerInfo *root, Plan *plan, int rtoffset)
{
	Hash	   *hplan = (Hash *) plan;
	Plan	   *outer_plan = plan->lefttree;
	indexed_tlist *outer_itlist;

	/*
	 * Hash's hashkeys are used when feeding tuples into the hashtable,
	 * therefore have them reference Hash's outer plan (which itself is the
	 * inner plan of the HashJoin).
	 */
	outer_itlist = build_tlist_index(outer_plan->targetlist);
	hplan->hashkeys = (List *)
		fix_upper_expr(root,
					   (Node *) hplan->hashkeys,
					   outer_itlist,
					   OUTER_VAR,
					   rtoffset);

	/* Hash doesn't project */
	set_dummy_tlist_references(plan, rtoffset);

	/* Hash nodes don't have their own quals */
	Assert(plan->qual == NIL);
}

/*
 * copyVar
 *		Copy a Var node.
 *
 * fix_scan_expr and friends do this enough times that it's worth having
 * a bespoke routine instead of using the generic copyObject() function.
 */
static inline Var *
copyVar(Var *var)
{
	Var		   *newvar = (Var *) palloc(sizeof(Var));

	*newvar = *var;
	return newvar;
}

/*
 * fix_expr_common
 *		Do generic set_plan_references processing on an expression node
 *
 * This is code that is common to all variants of expression-fixing.
 * We must look up operator opcode info for OpExpr and related nodes,
 * add OIDs from regclass Const nodes into root->glob->relationOids, and
 * add PlanInvalItems for user-defined functions into root->glob->invalItems.
 * We also fill in column index lists for GROUPING() expressions.
 *
 * We assume it's okay to update opcode info in-place.  So this could possibly
 * scribble on the planner's input data structures, but it's OK.
 */
static void
fix_expr_common(PlannerInfo *root, Node *node)
{
	/* We assume callers won't call us on a NULL pointer */
	if (IsA(node, Aggref))
	{
		record_plan_function_dependency(root,
										((Aggref *) node)->aggfnoid);
	}
	else if (IsA(node, WindowFunc))
	{
		record_plan_function_dependency(root,
										((WindowFunc *) node)->winfnoid);
	}
	else if (IsA(node, FuncExpr))
	{
		record_plan_function_dependency(root,
										((FuncExpr *) node)->funcid);
	}
	else if (IsA(node, OpExpr))
	{
		set_opfuncid((OpExpr *) node);
		record_plan_function_dependency(root,
										((OpExpr *) node)->opfuncid);
	}
	else if (IsA(node, DistinctExpr))
	{
		set_opfuncid((OpExpr *) node);	/* rely on struct equivalence */
		record_plan_function_dependency(root,
										((DistinctExpr *) node)->opfuncid);
	}
	else if (IsA(node, NullIfExpr))
	{
		set_opfuncid((OpExpr *) node);	/* rely on struct equivalence */
		record_plan_function_dependency(root,
										((NullIfExpr *) node)->opfuncid);
	}
	else if (IsA(node, ScalarArrayOpExpr))
	{
		set_sa_opfuncid((ScalarArrayOpExpr *) node);
		record_plan_function_dependency(root,
										((ScalarArrayOpExpr *) node)->opfuncid);
	}
	else if (IsA(node, Const))
	{
		Const	   *con = (Const *) node;

		/* Check for regclass reference */
		if (ISREGCLASSCONST(con))
			root->glob->relationOids =
				lappend_oid(root->glob->relationOids,
							DatumGetObjectId(con->constvalue));
	}
	else if (IsA(node, GroupingFunc))
	{
		GroupingFunc *g = (GroupingFunc *) node;
		AttrNumber *grouping_map = root->grouping_map;

		/* If there are no grouping sets, we don't need this. */

		Assert(grouping_map || g->cols == NIL);

		if (grouping_map)
		{
			ListCell   *lc;
			List	   *cols = NIL;

			foreach(lc, g->refs)
			{
				cols = lappend_int(cols, grouping_map[lfirst_int(lc)]);
			}

			Assert(!g->cols || equal(cols, g->cols));

			if (!g->cols)
				g->cols = cols;
		}
	}
}

/*
 * fix_param_node
 *		Do set_plan_references processing on a Param
 *
 * If it's a PARAM_MULTIEXPR, replace it with the appropriate Param from
 * root->multiexpr_params; otherwise no change is needed.
 * Just for paranoia's sake, we make a copy of the node in either case.
 */
static Node *
fix_param_node(PlannerInfo *root, Param *p)
{
	if (p->paramkind == PARAM_MULTIEXPR)
	{
		int			subqueryid = p->paramid >> 16;
		int			colno = p->paramid & 0xFFFF;
		List	   *params;

		if (subqueryid <= 0 ||
			subqueryid > list_length(root->multiexpr_params))
			elog(ERROR, "unexpected PARAM_MULTIEXPR ID: %d", p->paramid);
		params = (List *) list_nth(root->multiexpr_params, subqueryid - 1);
		if (colno <= 0 || colno > list_length(params))
			elog(ERROR, "unexpected PARAM_MULTIEXPR ID: %d", p->paramid);
		return copyObject(list_nth(params, colno - 1));
	}
	return (Node *) copyObject(p);
}

/*
 * fix_scan_expr
 *    对扫描级别表达式执行set_plan_references处理
 *
 * 该函数执行的操作包括：
 * - 将所有Var节点的varnos（关系编号）增加rtoffset
 * - 替换PARAM_MULTIEXPR类型的Param参数
 * - 展开PlaceHolderVars（占位符变量）
 * - 替换应被initplan输出参数替代的Aggref节点
 * - 为OpExpr及相关节点查找运算符opcode信息
 * - 将regclass类型常量节点中的OID添加到root->glob->relationOids中
 */
static Node *
fix_scan_expr(PlannerInfo *root, Node *node, int rtoffset)
{
    fix_scan_expr_context context; /* 处理上下文，包含必要的参数和状态 */

    /* 初始化上下文 */
    context.root = root;      /* 查询规划器信息 */
    context.rtoffset = rtoffset; /* 关系编号偏移量 */

    /*
     * 判断是否需要进行完整的表达式复制和转换
     * 当以下任一条件满足时需要完整转换：
     * 1. rtoffset != 0 - 需要调整关系编号
     * 2. 存在多表达式参数 - 需要替换参数
     * 3. 存在占位符 - 需要展开占位符
     * 4. 存在minmax聚合 - 需要替换聚合节点
     */
    if (rtoffset != 0 ||
        root->multiexpr_params != NIL ||
        root->glob->lastPHId != 0 ||
        root->minmax_aggs != NIL)
    {
        /* 需要完整转换，调用mutator函数创建新的表达式树 */
        return fix_scan_expr_mutator(node, &context);
    }
    else
    {
        /*
         * 优化路径：如果rtoffset == 0，我们不需要更改任何Var节点，
         * 如果没有MULTIEXPR子查询，则不需要替换PARAM_MULTIEXPR参数，
         * 如果没有占位符，则不需要移除它们，
         * 如果没有minmax聚合，则不需要替换它们。
         * 
         * 在这种情况下，可以直接在输入节点树上进行修改（因为唯一的更改是
         * 填充未设置的opfuncid字段，这是无害的）。这可以节省足够的CPU周期，
         * 在简单查询上效果显著。
         */
        (void) fix_scan_expr_walker(node, &context);
        return node;  /* 直接返回修改后的原节点 */
    }
}


/*
 * fix_scan_expr_mutator - 递归处理扫描级别表达式中的节点，修复引用关系
 * 
 * 参数：
 *   node - 待处理的表达式节点
 *   context - 包含处理上下文信息的结构体，包括root(规划器状态)和rtoffset(关系编号偏移量)
 * 
 * 返回值：
 *   返回处理后的新表达式节点
 * 
 * 功能：
 *   此函数作为mutator函数，在表达式树遍历过程中修复各类节点中的引用关系，
 *   特别是调整Var节点的关系编号、处理参数和聚合函数引用等
 */
static Node *
fix_scan_expr_mutator(Node *node, fix_scan_expr_context *context)
{
	/* 处理NULL节点情况 */
	if (node == NULL)
		return NULL;
	
	/* 处理Var节点 - 表列引用 */
	if (IsA(node, Var))
	{
		Var		   *var = copyVar((Var *) node);

		/* 断言：扫描级别的变量不应有嵌套层级 */
		Assert(var->varlevelsup == 0);

		/*
		 * 断言：不应该看到标记为INNER_VAR或OUTER_VAR的Var。
		 * 但索引条件表达式可能包含INDEX_VAR类型的Var。
		 */
		Assert(var->varno != INNER_VAR);
		Assert(var->varno != OUTER_VAR);
		
		/* 调整非特殊关系编号，应用偏移量 */
		if (!IS_SPECIAL_VARNO(var->varno))
			var->varno += context->rtoffset;
		/* 同样调整varnoold（原始关系编号） */
		if (var->varnoold > 0)
			var->varnoold += context->rtoffset;
		return (Node *) var;
	}
	
	/* 处理Param节点 - 参数引用 */
	if (IsA(node, Param))
		return fix_param_node(context->root, (Param *) node);
	
	/* 处理Aggref节点 - 聚合函数引用 */
	if (IsA(node, Aggref))
	{
		Aggref	   *aggref = (Aggref *) node;
		Param	   *aggparam;

		/* 检查是否应该用Param替换Aggref（通常用于min/max聚合优化） */
		aggparam = find_minmax_agg_replacement_param(context->root, aggref);
		if (aggparam != NULL)
		{
			/* 安全起见，复制Param节点 */
			return (Node *) copyObject(aggparam);
		}
		/* 无匹配项时，继续正常处理 */
	}
	
	/* 处理CurrentOfExpr节点 - 当前游标的表达式引用 */
	if (IsA(node, CurrentOfExpr))
	{
		CurrentOfExpr *cexpr = (CurrentOfExpr *) copyObject(node);

		/* 断言：不应引用内部或外部关系 */
		Assert(cexpr->cvarno != INNER_VAR);
		Assert(cexpr->cvarno != OUTER_VAR);
		/* 调整非特殊关系编号 */
		if (!IS_SPECIAL_VARNO(cexpr->cvarno))
			cexpr->cvarno += context->rtoffset;
		return (Node *) cexpr;
	}
	
	/* 处理PlaceHolderVar节点 - 占位变量 */
	if (IsA(node, PlaceHolderVar))
	{
		/* 在扫描级别，我们应该直接评估包含的表达式 */
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		/* 递归处理占位变量包含的表达式 */
		return fix_scan_expr_mutator((Node *) phv->phexpr, context);
	}
	
	/* 对所有其他节点类型应用通用修复 */
	fix_expr_common(context->root, node);
	/* 递归处理节点的子表达式 */
	return expression_tree_mutator(node, fix_scan_expr_mutator,
					   (void *) context);
}

/*
 * fix_scan_expr_walker - 遍历扫描级别表达式，执行原地修改
 * 
 * 参数：
 *   node - 待处理的表达式节点
 *   context - 包含处理上下文信息的结构体
 * 
 * 返回值：
 *   布尔值，表示是否需要继续遍历
 * 
 * 功能：
 *   作为walker函数，对表达式树执行原地修改操作，
 *   适用于不需要创建新节点树的场景，主要调用fix_expr_common处理节点
 */
static bool
fix_scan_expr_walker(Node *node, fix_scan_expr_context *context)
{
	/* 处理NULL节点情况 */
	if (node == NULL)
		return false;
	/* 断言：此函数不处理PlaceHolderVar节点（由mutator版本特殊处理） */
	Assert(!IsA(node, PlaceHolderVar));
	/* 对节点应用通用修复 */
	fix_expr_common(context->root, node);
	/* 递归遍历子表达式 */
	return expression_tree_walker(node, fix_scan_expr_walker,
					  (void *) context);
}


/*
 * set_join_references
 *	  Modify the target list and quals of a join node to reference its
 *	  subplans, by setting the varnos to OUTER_VAR or INNER_VAR and setting
 *	  attno values to the result domain number of either the corresponding
 *	  outer or inner join tuple item.  Also perform opcode lookup for these
 *	  expressions, and add regclass OIDs to root->glob->relationOids.
 */
static void
set_join_references(PlannerInfo *root, Join *join, int rtoffset)
{
	Plan	   *outer_plan = join->plan.lefttree;
	Plan	   *inner_plan = join->plan.righttree;
	indexed_tlist *outer_itlist;
	indexed_tlist *inner_itlist;

	outer_itlist = build_tlist_index(outer_plan->targetlist);
	inner_itlist = build_tlist_index(inner_plan->targetlist);

	/*
	 * First process the joinquals (including merge or hash clauses).  These
	 * are logically below the join so they can always use all values
	 * available from the input tlists.  It's okay to also handle
	 * NestLoopParams now, because those couldn't refer to nullable
	 * subexpressions.
	 */
	join->joinqual = fix_join_expr(root,
								   join->joinqual,
								   outer_itlist,
								   inner_itlist,
								   (Index) 0,
								   rtoffset);

	/* Now do join-type-specific stuff */
	if (IsA(join, NestLoop))
	{
		NestLoop   *nl = (NestLoop *) join;
		ListCell   *lc;

		foreach(lc, nl->nestParams)
		{
			NestLoopParam *nlp = (NestLoopParam *) lfirst(lc);

			nlp->paramval = (Var *) fix_upper_expr(root,
												   (Node *) nlp->paramval,
												   outer_itlist,
												   OUTER_VAR,
												   rtoffset);
			/* Check we replaced any PlaceHolderVar with simple Var */
			if (!(IsA(nlp->paramval, Var) &&
				  nlp->paramval->varno == OUTER_VAR))
				elog(ERROR, "NestLoopParam was not reduced to a simple Var");
		}
	}
	else if (IsA(join, MergeJoin))
	{
		MergeJoin  *mj = (MergeJoin *) join;

		mj->mergeclauses = fix_join_expr(root,
										 mj->mergeclauses,
										 outer_itlist,
										 inner_itlist,
										 (Index) 0,
										 rtoffset);
	}
	else if (IsA(join, HashJoin))
	{
		HashJoin   *hj = (HashJoin *) join;

		hj->hashclauses = fix_join_expr(root,
										hj->hashclauses,
										outer_itlist,
										inner_itlist,
										(Index) 0,
										rtoffset);

		/*
		 * HashJoin's hashkeys are used to look for matching tuples from its
		 * outer plan (not the Hash node!) in the hashtable.
		 */
		hj->hashkeys = (List *) fix_upper_expr(root,
											   (Node *) hj->hashkeys,
											   outer_itlist,
											   OUTER_VAR,
											   rtoffset);
	}

	/*
	 * Now we need to fix up the targetlist and qpqual, which are logically
	 * above the join.  This means they should not re-use any input expression
	 * that was computed in the nullable side of an outer join.  Vars and
	 * PlaceHolderVars are fine, so we can implement this restriction just by
	 * clearing has_non_vars in the indexed_tlist structs.
	 *
	 * XXX This is a grotty workaround for the fact that we don't clearly
	 * distinguish between a Var appearing below an outer join and the "same"
	 * Var appearing above it.  If we did, we'd not need to hack the matching
	 * rules this way.
	 */
	switch (join->jointype)
	{
		case JOIN_LEFT:
		case JOIN_SEMI:
		case JOIN_ANTI:
			inner_itlist->has_non_vars = false;
			break;
		case JOIN_RIGHT:
			outer_itlist->has_non_vars = false;
			break;
		case JOIN_FULL:
			outer_itlist->has_non_vars = false;
			inner_itlist->has_non_vars = false;
			break;
		default:
			break;
	}

	join->plan.targetlist = fix_join_expr(root,
										  join->plan.targetlist,
										  outer_itlist,
										  inner_itlist,
										  (Index) 0,
										  rtoffset);
	join->plan.qual = fix_join_expr(root,
									join->plan.qual,
									outer_itlist,
									inner_itlist,
									(Index) 0,
									rtoffset);

	pfree(outer_itlist);
	pfree(inner_itlist);
}

/*
 * set_upper_references
 *	  Update the targetlist and quals of an upper-level plan node
 *	  to refer to the tuples returned by its lefttree subplan.
 *	  Also perform opcode lookup for these expressions, and
 *	  add regclass OIDs to root->glob->relationOids.
 *
 * This is used for single-input plan types like Agg, Group, Result.
 *
 * In most cases, we have to match up individual Vars in the tlist and
 * qual expressions with elements of the subplan's tlist (which was
 * generated by flattening these selfsame expressions, so it should have all
 * the required variables).  There is an important exception, however:
 * depending on where we are in the plan tree, sort/group columns may have
 * been pushed into the subplan tlist unflattened.  If these values are also
 * needed in the output then we want to reference the subplan tlist element
 * rather than recomputing the expression.
 */
/*
 * 更新上层计划节点的目标列表和条件表达式，使其引用左子树子计划返回的元组
 * 
 * 此函数在查询优化器的计划生成阶段扮演关键角色，确保计划树中上层节点能够正确引用下层节点的输出
 * 主要用于处理单输入计划类型，如聚合(Agg)、分组(Group)和结果(Result)节点
 * 
 * 参数:
 *   root - 规划器信息结构
 *   plan - 需要更新引用的上层计划节点
 *   rtoffset - 关系表编号偏移量，用于调整Var节点的varno属性
 */
static void
set_upper_references(PlannerInfo *root, Plan *plan, int rtoffset)
{
	Plan *subplan = plan->lefttree;       /* 获取左子树子计划 */
	indexed_tlist *subplan_itlist;        /* 子计划目标列表的索引结构 */
	List *output_targetlist;              /* 构建新的输出目标列表 */
	ListCell *l;                          /* 列表遍历指针 */

	/* 为子计划的目标列表构建索引数据结构，优化后续查找性能 */
	/* 这个索引结构包含对Var节点的快速访问以及非Var表达式的信息 */
	subplan_itlist = build_tlist_index(subplan->targetlist);

	/* 初始化输出目标列表 */
	output_targetlist = NIL;
	
	/* 遍历上层计划节点的目标列表中的每个TargetEntry */
	foreach(l, plan->targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);  /* 当前处理的目标项 */
		Node *newexpr;                                 /* 处理后的新表达式 */

		/* 如果是排序/分组项，首先尝试通过sortgroupref进行匹配 */
		/* 这确保在有多个文本相等但易变的排序表达式时，我们选择正确的子计划TLE */
		if (tle->ressortgroupref != 0)
		{
			/* 尝试通过sortgroupref查找匹配的子计划目标项 */
			newexpr = (Node *)
				search_indexed_tlist_for_sortgroupref(tle->expr,
									   tle->ressortgroupref,
									   subplan_itlist,
									   OUTER_VAR);
			/* 如果未找到匹配项，则使用通用表达式修复函数 */
			if (!newexpr)
				newexpr = fix_upper_expr(root,
						  (Node *) tle->expr,
						  subplan_itlist,
						  OUTER_VAR,
						  rtoffset);
		}
		else
			/* 非排序/分组项，直接使用通用表达式修复函数 */
			newexpr = fix_upper_expr(root,
					  (Node *) tle->expr,
					  subplan_itlist,
					  OUTER_VAR,
					  rtoffset);
		
		/* 复制目标项并更新其表达式为修复后的版本 */
		tle = flatCopyTargetEntry(tle);
		tle->expr = (Expr *) newexpr;
		/* 将更新后的目标项添加到输出目标列表 */
		output_targetlist = lappend(output_targetlist, tle);
	}
	
	/* 用新构建的目标列表替换计划节点的原始目标列表 */
	plan->targetlist = output_targetlist;

	/* 处理计划节点的过滤条件表达式 */
	plan->qual = (List *)
		fix_upper_expr(root,
			  (Node *) plan->qual,
			  subplan_itlist,
			  OUTER_VAR,
			  rtoffset);

	/* 释放临时创建的索引结构 */
	pfree(subplan_itlist);
}


/*
 * set_param_references
 *	  Initialize the initParam list in Gather or Gather merge node such that
 *	  it contains reference of all the params that needs to be evaluated
 *	  before execution of the node.  It contains the initplan params that are
 *	  being passed to the plan nodes below it.
 */
static void
set_param_references(PlannerInfo *root, Plan *plan)
{
	Assert(IsA(plan, Gather) ||IsA(plan, GatherMerge));

	if (plan->lefttree->extParam)
	{
		PlannerInfo *proot;
		Bitmapset  *initSetParam = NULL;
		ListCell   *l;

		for (proot = root; proot != NULL; proot = proot->parent_root)
		{
			foreach(l, proot->init_plans)
			{
				SubPlan    *initsubplan = (SubPlan *) lfirst(l);
				ListCell   *l2;

				foreach(l2, initsubplan->setParam)
				{
					initSetParam = bms_add_member(initSetParam, lfirst_int(l2));
				}
			}
		}

		/*
		 * Remember the list of all external initplan params that are used by
		 * the children of Gather or Gather merge node.
		 */
		if (IsA(plan, Gather))
			((Gather *) plan)->initParam =
				bms_intersect(plan->lefttree->extParam, initSetParam);
		else
			((GatherMerge *) plan)->initParam =
				bms_intersect(plan->lefttree->extParam, initSetParam);
	}
}

/*
 * Recursively scan an expression tree and convert Aggrefs to the proper
 * intermediate form for combining aggregates.  This means (1) replacing each
 * one's argument list with a single argument that is the original Aggref
 * modified to show partial aggregation and (2) changing the upper Aggref to
 * show combining aggregation.
 *
 * After this step, set_upper_references will replace the partial Aggrefs
 * with Vars referencing the lower Agg plan node's outputs, so that the final
 * form seen by the executor is a combining Aggref with a Var as input.
 *
 * It's rather messy to postpone this step until setrefs.c; ideally it'd be
 * done in createplan.c.  The difficulty is that once we modify the Aggref
 * expressions, they will no longer be equal() to their original form and
 * so cross-plan-node-level matches will fail.  So this has to happen after
 * the plan node above the Agg has resolved its subplan references.
 */
/*
 * 递归扫描表达式树，将聚合引用(Aggrefs)转换为用于组合聚合的适当中间形式
 * 
 * 这个函数是PostgreSQL查询优化器中聚合拆分(Aggregation Splitting)功能的关键组件，
 * 它实现了将单个聚合表达式转换为两个阶段的聚合处理：部分聚合和最终合并。
 * 
 * 参数:
 *   node - 待处理的表达式树节点
 *   context - 上下文指针(此函数中未使用)
 * 返回值:
 *   转换后的表达式节点树
 */
static Node *
convert_combining_aggrefs(Node *node, void *context)
{
	/* 处理空节点的边界情况 */
	if (node == NULL)
		return NULL;
	
	/* 如果节点是聚合引用(Aggref)，则进行转换处理 */
	if (IsA(node, Aggref))
	{
		Aggref *orig_agg = (Aggref *) node; /* 原始聚合引用节点 */
		Aggref *child_agg;  /* 子聚合引用(用于第一阶段部分聚合) */
		Aggref *parent_agg; /* 父聚合引用(用于第二阶段合并聚合) */

		/* 断言确保我们没有选择对不支持的情况进行部分聚合处理 */
		Assert(orig_agg->aggorder == NIL);    /* 不支持有序聚合 */
		Assert(orig_agg->aggdistinct == NIL); /* 不支持DISTINCT聚合 */

		/*
		 * 由于聚合调用不能嵌套，我们不需要递归到参数中。
		 * 但为安全起见，我们复制Aggref节点本身，而不是就地修改。
		 */
		child_agg = makeNode(Aggref);          /* 创建新的Aggref节点 */
		memcpy(child_agg, orig_agg, sizeof(Aggref)); /* 复制原始节点的所有字段 */

		/*
		 * 对于父聚合引用，我们希望复制原始聚合的所有字段，除了:
		 * 1. 参数列表(args)，将在下面替换
		 * 2. 聚合过滤器(aggfilter)，应该只由子聚合应用
		 * 为避免在这里显式了解所有其他字段，我们临时修改child_agg作为copyObject的合适源
		 */
		child_agg->args = NIL;        /* 临时清空参数列表 */
		child_agg->aggfilter = NULL;  /* 临时清空聚合过滤器 */
		parent_agg = copyObject(child_agg); /* 复制修改后的child_agg到parent_agg */
		child_agg->args = orig_agg->args;        /* 恢复child_agg的原始参数列表 */
		child_agg->aggfilter = orig_agg->aggfilter; /* 恢复child_agg的原始过滤器 */

		/*
		 * 现在，设置child_agg表示部分聚合的第一阶段。
		 * 对于初始阶段，我们假设需要序列化。
		 * 这对应于在数据分片上执行的部分聚合操作。
		 */
		mark_partial_aggref(child_agg, AGGSPLIT_INITIAL_SERIAL);

		/*
		 * 设置parent_agg表示第二阶段。
		 * 父聚合引用的参数列表只包含一个TargetEntry，其表达式为child_agg
		 * 这将在后续阶段由set_upper_references替换为引用子Agg计划节点输出的Var
		 */
		parent_agg->args = list_make1(makeTargetEntry((Expr *) child_agg,
							  1, NULL, false));
		mark_partial_aggref(parent_agg, AGGSPLIT_FINAL_DESERIAL);

		return (Node *) parent_agg; /* 返回转换后的父聚合引用节点 */
	}
	
	/* 对于非Aggref节点，递归处理其子节点 */
	return expression_tree_mutator(node, convert_combining_aggrefs,
				   (void *) context);
}


/*
 * set_dummy_tlist_references
 *	  Replace the targetlist of an upper-level plan node with a simple
 *	  list of OUTER_VAR references to its child.
 *
 * This is used for plan types like Sort and Append that don't evaluate
 * their targetlists.  Although the executor doesn't care at all what's in
 * the tlist, EXPLAIN needs it to be realistic.
 *
 * Note: we could almost use set_upper_references() here, but it fails for
 * Append for lack of a lefttree subplan.  Single-purpose code is faster
 * anyway.
 */
static void
set_dummy_tlist_references(Plan *plan, int rtoffset)
{
	List	   *output_targetlist;
	ListCell   *l;

	output_targetlist = NIL;
	foreach(l, plan->targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);
		Var		   *oldvar = (Var *) tle->expr;
		Var		   *newvar;

		/*
		 * As in search_indexed_tlist_for_non_var(), we prefer to keep Consts
		 * as Consts, not Vars referencing Consts.  Here, there's no speed
		 * advantage to be had, but it makes EXPLAIN output look cleaner, and
		 * again it avoids confusing the executor.
		 */
		if (IsA(oldvar, Const))
		{
			/* just reuse the existing TLE node */
			output_targetlist = lappend(output_targetlist, tle);
			continue;
		}

		newvar = makeVar(OUTER_VAR,
						 tle->resno,
						 exprType((Node *) oldvar),
						 exprTypmod((Node *) oldvar),
						 exprCollation((Node *) oldvar),
						 0);
		if (IsA(oldvar, Var))
		{
			newvar->varnoold = oldvar->varno + rtoffset;
			newvar->varoattno = oldvar->varattno;
		}
		else
		{
			newvar->varnoold = 0;	/* wasn't ever a plain Var */
			newvar->varoattno = 0;
		}

		tle = flatCopyTargetEntry(tle);
		tle->expr = (Expr *) newvar;
		output_targetlist = lappend(output_targetlist, tle);
	}
	plan->targetlist = output_targetlist;

	/* We don't touch plan->qual here */
}


/*
 * build_tlist_index --- build an index data structure for a child tlist
 *
 * In most cases, subplan tlists will be "flat" tlists with only Vars,
 * so we try to optimize that case by extracting information about Vars
 * in advance.  Matching a parent tlist to a child is still an O(N^2)
 * operation, but at least with a much smaller constant factor than plain
 * tlist_member() searches.
 *
 * The result of this function is an indexed_tlist struct to pass to
 * search_indexed_tlist_for_var() or search_indexed_tlist_for_non_var().
 * When done, the indexed_tlist may be freed with a single pfree().
 */
static indexed_tlist *
build_tlist_index(List *tlist)
{
	indexed_tlist *itlist;
	tlist_vinfo *vinfo;
	ListCell   *l;

	/* Create data structure with enough slots for all tlist entries */
	itlist = (indexed_tlist *)
		palloc(offsetof(indexed_tlist, vars) +
			   list_length(tlist) * sizeof(tlist_vinfo));

	itlist->tlist = tlist;
	itlist->has_ph_vars = false;
	itlist->has_non_vars = false;

	/* Find the Vars and fill in the index array */
	vinfo = itlist->vars;
	foreach(l, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->expr && IsA(tle->expr, Var))
		{
			Var		   *var = (Var *) tle->expr;

			vinfo->varno = var->varno;
			vinfo->varattno = var->varattno;
			vinfo->resno = tle->resno;
			vinfo++;
		}
		else if (tle->expr && IsA(tle->expr, PlaceHolderVar))
			itlist->has_ph_vars = true;
		else
			itlist->has_non_vars = true;
	}

	itlist->num_vars = (vinfo - itlist->vars);

	return itlist;
}

/*
 * build_tlist_index_other_vars --- build a restricted tlist index
 *
 * This is like build_tlist_index, but we only index tlist entries that
 * are Vars belonging to some rel other than the one specified.  We will set
 * has_ph_vars (allowing PlaceHolderVars to be matched), but not has_non_vars
 * (so nothing other than Vars and PlaceHolderVars can be matched).
 */
static indexed_tlist *
build_tlist_index_other_vars(List *tlist, Index ignore_rel)
{
	indexed_tlist *itlist;
	tlist_vinfo *vinfo;
	ListCell   *l;

	/* Create data structure with enough slots for all tlist entries */
	itlist = (indexed_tlist *)
		palloc(offsetof(indexed_tlist, vars) +
			   list_length(tlist) * sizeof(tlist_vinfo));

	itlist->tlist = tlist;
	itlist->has_ph_vars = false;
	itlist->has_non_vars = false;

	/* Find the desired Vars and fill in the index array */
	vinfo = itlist->vars;
	foreach(l, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->expr && IsA(tle->expr, Var))
		{
			Var		   *var = (Var *) tle->expr;

			if (var->varno != ignore_rel)
			{
				vinfo->varno = var->varno;
				vinfo->varattno = var->varattno;
				vinfo->resno = tle->resno;
				vinfo++;
			}
		}
		else if (tle->expr && IsA(tle->expr, PlaceHolderVar))
			itlist->has_ph_vars = true;
	}

	itlist->num_vars = (vinfo - itlist->vars);

	return itlist;
}

/*
 * search_indexed_tlist_for_var --- find a Var in an indexed tlist
 *
 * If a match is found, return a copy of the given Var with suitably
 * modified varno/varattno (to wit, newvarno and the resno of the TLE entry).
 * Also ensure that varnoold is incremented by rtoffset.
 * If no match, return NULL.
 */
static Var *
search_indexed_tlist_for_var(Var *var, indexed_tlist *itlist,
							 Index newvarno, int rtoffset)
{
	Index		varno = var->varno;
	AttrNumber	varattno = var->varattno;
	tlist_vinfo *vinfo;
	int			i;

	vinfo = itlist->vars;
	i = itlist->num_vars;
	while (i-- > 0)
	{
		if (vinfo->varno == varno && vinfo->varattno == varattno)
		{
			/* Found a match */
			Var		   *newvar = copyVar(var);

			newvar->varno = newvarno;
			newvar->varattno = vinfo->resno;
			if (newvar->varnoold > 0)
				newvar->varnoold += rtoffset;
			return newvar;
		}
		vinfo++;
	}
	return NULL;				/* no match */
}

/*
 * search_indexed_tlist_for_non_var --- find a non-Var in an indexed tlist
 *
 * If a match is found, return a Var constructed to reference the tlist item.
 * If no match, return NULL.
 *
 * NOTE: it is a waste of time to call this unless itlist->has_ph_vars or
 * itlist->has_non_vars.  Furthermore, set_join_references() relies on being
 * able to prevent matching of non-Vars by clearing itlist->has_non_vars,
 * so there's a correctness reason not to call it unless that's set.
 */
static Var *
search_indexed_tlist_for_non_var(Expr *node,
								 indexed_tlist *itlist, Index newvarno)
{
	TargetEntry *tle;

	/*
	 * If it's a simple Const, replacing it with a Var is silly, even if there
	 * happens to be an identical Const below; a Var is more expensive to
	 * execute than a Const.  What's more, replacing it could confuse some
	 * places in the executor that expect to see simple Consts for, eg,
	 * dropped columns.
	 */
	if (IsA(node, Const))
		return NULL;

	tle = tlist_member(node, itlist->tlist);
	if (tle)
	{
		/* Found a matching subplan output expression */
		Var		   *newvar;

		newvar = makeVarFromTargetEntry(newvarno, tle);
		newvar->varnoold = 0;	/* wasn't ever a plain Var */
		newvar->varoattno = 0;
		return newvar;
	}
	return NULL;				/* no match */
}

/*
 * search_indexed_tlist_for_sortgroupref --- find a sort/group expression
 *
 * If a match is found, return a Var constructed to reference the tlist item.
 * If no match, return NULL.
 *
 * This is needed to ensure that we select the right subplan TLE in cases
 * where there are multiple textually-equal()-but-volatile sort expressions.
 * And it's also faster than search_indexed_tlist_for_non_var.
 */
static Var *
search_indexed_tlist_for_sortgroupref(Expr *node,
									  Index sortgroupref,
									  indexed_tlist *itlist,
									  Index newvarno)
{
	ListCell   *lc;

	foreach(lc, itlist->tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		/* The equal() check should be redundant, but let's be paranoid */
		if (tle->ressortgroupref == sortgroupref &&
			equal(node, tle->expr))
		{
			/* Found a matching subplan output expression */
			Var		   *newvar;

			newvar = makeVarFromTargetEntry(newvarno, tle);
			newvar->varnoold = 0;	/* wasn't ever a plain Var */
			newvar->varoattno = 0;
			return newvar;
		}
	}
	return NULL;				/* no match */
}

/*
 * fix_join_expr
 *	   Create a new set of targetlist entries or join qual clauses by
 *	   changing the varno/varattno values of variables in the clauses
 *	   to reference target list values from the outer and inner join
 *	   relation target lists.  Also perform opcode lookup and add
 *	   regclass OIDs to root->glob->relationOids.
 *
 * This is used in three different scenarios:
 * 1) a normal join clause, where all the Vars in the clause *must* be
 *	  replaced by OUTER_VAR or INNER_VAR references.  In this case
 *	  acceptable_rel should be zero so that any failure to match a Var will be
 *	  reported as an error.
 * 2) RETURNING clauses, which may contain both Vars of the target relation
 *	  and Vars of other relations. In this case we want to replace the
 *	  other-relation Vars by OUTER_VAR references, while leaving target Vars
 *	  alone. Thus inner_itlist = NULL and acceptable_rel = the ID of the
 *	  target relation should be passed.
 * 3) ON CONFLICT UPDATE SET/WHERE clauses.  Here references to EXCLUDED are
 *	  to be replaced with INNER_VAR references, while leaving target Vars (the
 *	  to-be-updated relation) alone. Correspondingly inner_itlist is to be
 *	  EXCLUDED elements, outer_itlist = NULL and acceptable_rel the target
 *	  relation.
 *
 * 'clauses' is the targetlist or list of join clauses
 * 'outer_itlist' is the indexed target list of the outer join relation,
 *		or NULL
 * 'inner_itlist' is the indexed target list of the inner join relation,
 *		or NULL
 * 'acceptable_rel' is either zero or the rangetable index of a relation
 *		whose Vars may appear in the clause without provoking an error
 * 'rtoffset': how much to increment varnoold by
 *
 * Returns the new expression tree.  The original clause structure is
 * not modified.
 */
static List *
fix_join_expr(PlannerInfo *root,
			  List *clauses,
			  indexed_tlist *outer_itlist,
			  indexed_tlist *inner_itlist,
			  Index acceptable_rel,
			  int rtoffset)
{
	fix_join_expr_context context;

	context.root = root;
	context.outer_itlist = outer_itlist;
	context.inner_itlist = inner_itlist;
	context.acceptable_rel = acceptable_rel;
	context.rtoffset = rtoffset;
	return (List *) fix_join_expr_mutator((Node *) clauses, &context);
}

static Node *
fix_join_expr_mutator(Node *node, fix_join_expr_context *context)
{
	Var		   *newvar;

	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		/* Look for the var in the input tlists, first in the outer */
		if (context->outer_itlist)
		{
			newvar = search_indexed_tlist_for_var(var,
												  context->outer_itlist,
												  OUTER_VAR,
												  context->rtoffset);
			if (newvar)
				return (Node *) newvar;
		}

		/* then in the inner. */
		if (context->inner_itlist)
		{
			newvar = search_indexed_tlist_for_var(var,
												  context->inner_itlist,
												  INNER_VAR,
												  context->rtoffset);
			if (newvar)
				return (Node *) newvar;
		}

		/* If it's for acceptable_rel, adjust and return it */
		if (var->varno == context->acceptable_rel)
		{
			var = copyVar(var);
			var->varno += context->rtoffset;
			if (var->varnoold > 0)
				var->varnoold += context->rtoffset;
			return (Node *) var;
		}

		/* No referent found for Var */
		elog(ERROR, "variable not found in subplan target lists");
	}
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		/* See if the PlaceHolderVar has bubbled up from a lower plan node */
		if (context->outer_itlist && context->outer_itlist->has_ph_vars)
		{
			newvar = search_indexed_tlist_for_non_var((Expr *) phv,
													  context->outer_itlist,
													  OUTER_VAR);
			if (newvar)
				return (Node *) newvar;
		}
		if (context->inner_itlist && context->inner_itlist->has_ph_vars)
		{
			newvar = search_indexed_tlist_for_non_var((Expr *) phv,
													  context->inner_itlist,
													  INNER_VAR);
			if (newvar)
				return (Node *) newvar;
		}

		/* If not supplied by input plans, evaluate the contained expr */
		return fix_join_expr_mutator((Node *) phv->phexpr, context);
	}
	/* Try matching more complex expressions too, if tlists have any */
	if (context->outer_itlist && context->outer_itlist->has_non_vars)
	{
		newvar = search_indexed_tlist_for_non_var((Expr *) node,
												  context->outer_itlist,
												  OUTER_VAR);
		if (newvar)
			return (Node *) newvar;
	}
	if (context->inner_itlist && context->inner_itlist->has_non_vars)
	{
		newvar = search_indexed_tlist_for_non_var((Expr *) node,
												  context->inner_itlist,
												  INNER_VAR);
		if (newvar)
			return (Node *) newvar;
	}
	/* Special cases (apply only AFTER failing to match to lower tlist) */
	if (IsA(node, Param))
		return fix_param_node(context->root, (Param *) node);
	fix_expr_common(context->root, node);
	return expression_tree_mutator(node,
								   fix_join_expr_mutator,
								   (void *) context);
}

/*
 * fix_upper_expr
 *		Modifies an expression tree so that all Var nodes reference outputs
 *		of a subplan.  Also looks for Aggref nodes that should be replaced
 *		by initplan output Params.  Also performs opcode lookup, and adds
 *		regclass OIDs to root->glob->relationOids.
 *
 * This is used to fix up target and qual expressions of non-join upper-level
 * plan nodes, as well as index-only scan nodes.
 *
 * An error is raised if no matching var can be found in the subplan tlist
 * --- so this routine should only be applied to nodes whose subplans'
 * targetlists were generated by flattening the expressions used in the
 * parent node.
 *
 * If itlist->has_non_vars is true, then we try to match whole subexpressions
 * against elements of the subplan tlist, so that we can avoid recomputing
 * expressions that were already computed by the subplan.  (This is relatively
 * expensive, so we don't want to try it in the common case where the
 * subplan tlist is just a flattened list of Vars.)
 *
 * 'node': the tree to be fixed (a target item or qual)
 * 'subplan_itlist': indexed target list for subplan (or index)
 * 'newvarno': varno to use for Vars referencing tlist elements
 * 'rtoffset': how much to increment varnoold by
 *
 * The resulting tree is a copy of the original in which all Var nodes have
 * varno = newvarno, varattno = resno of corresponding targetlist element.
 * The original tree is not modified.
 */
static Node *
fix_upper_expr(PlannerInfo *root,
			   Node *node,
			   indexed_tlist *subplan_itlist,
			   Index newvarno,
			   int rtoffset)
{
	fix_upper_expr_context context;

	context.root = root;
	context.subplan_itlist = subplan_itlist;
	context.newvarno = newvarno;
	context.rtoffset = rtoffset;
	return fix_upper_expr_mutator(node, &context);
}

static Node *
fix_upper_expr_mutator(Node *node, fix_upper_expr_context *context)
{
	Var		   *newvar;

	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		newvar = search_indexed_tlist_for_var(var,
											  context->subplan_itlist,
											  context->newvarno,
											  context->rtoffset);
		if (!newvar)
			elog(ERROR, "variable not found in subplan target list");
		return (Node *) newvar;
	}
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		/* See if the PlaceHolderVar has bubbled up from a lower plan node */
		if (context->subplan_itlist->has_ph_vars)
		{
			newvar = search_indexed_tlist_for_non_var((Expr *) phv,
													  context->subplan_itlist,
													  context->newvarno);
			if (newvar)
				return (Node *) newvar;
		}
		/* If not supplied by input plan, evaluate the contained expr */
		return fix_upper_expr_mutator((Node *) phv->phexpr, context);
	}
	/* Try matching more complex expressions too, if tlist has any */
	if (context->subplan_itlist->has_non_vars)
	{
		newvar = search_indexed_tlist_for_non_var((Expr *) node,
												  context->subplan_itlist,
												  context->newvarno);
		if (newvar)
			return (Node *) newvar;
	}
	/* Special cases (apply only AFTER failing to match to lower tlist) */
	if (IsA(node, Param))
		return fix_param_node(context->root, (Param *) node);
	if (IsA(node, Aggref))
	{
		Aggref	   *aggref = (Aggref *) node;
		Param	   *aggparam;

		/* See if the Aggref should be replaced by a Param */
		aggparam = find_minmax_agg_replacement_param(context->root, aggref);
		if (aggparam != NULL)
		{
			/* Make a copy of the Param for paranoia's sake */
			return (Node *) copyObject(aggparam);
		}
		/* If no match, just fall through to process it normally */
	}
	fix_expr_common(context->root, node);
	return expression_tree_mutator(node,
								   fix_upper_expr_mutator,
								   (void *) context);
}

/*
 * set_returning_clause_references
 *		Perform setrefs.c's work on a RETURNING targetlist
 *
 * If the query involves more than just the result table, we have to
 * adjust any Vars that refer to other tables to reference junk tlist
 * entries in the top subplan's targetlist.  Vars referencing the result
 * table should be left alone, however (the executor will evaluate them
 * using the actual heap tuple, after firing triggers if any).  In the
 * adjusted RETURNING list, result-table Vars will have their original
 * varno (plus rtoffset), but Vars for other rels will have varno OUTER_VAR.
 *
 * We also must perform opcode lookup and add regclass OIDs to
 * root->glob->relationOids.
 *
 * 'rlist': the RETURNING targetlist to be fixed
 * 'topplan': the top subplan node that will be just below the ModifyTable
 *		node (note it's not yet passed through set_plan_refs)
 * 'resultRelation': RT index of the associated result relation
 * 'rtoffset': how much to increment varnos by
 *
 * Note: the given 'root' is for the parent query level, not the 'topplan'.
 * This does not matter currently since we only access the dependency-item
 * lists in root->glob, but it would need some hacking if we wanted a root
 * that actually matches the subplan.
 *
 * Note: resultRelation is not yet adjusted by rtoffset.
 */
static List *
set_returning_clause_references(PlannerInfo *root,
								List *rlist,
								Plan *topplan,
								Index resultRelation,
								int rtoffset)
{
	indexed_tlist *itlist;

	/*
	 * We can perform the desired Var fixup by abusing the fix_join_expr
	 * machinery that formerly handled inner indexscan fixup.  We search the
	 * top plan's targetlist for Vars of non-result relations, and use
	 * fix_join_expr to convert RETURNING Vars into references to those tlist
	 * entries, while leaving result-rel Vars as-is.
	 *
	 * PlaceHolderVars will also be sought in the targetlist, but no
	 * more-complex expressions will be.  Note that it is not possible for a
	 * PlaceHolderVar to refer to the result relation, since the result is
	 * never below an outer join.  If that case could happen, we'd have to be
	 * prepared to pick apart the PlaceHolderVar and evaluate its contained
	 * expression instead.
	 */
	itlist = build_tlist_index_other_vars(topplan->targetlist, resultRelation);

	rlist = fix_join_expr(root,
						  rlist,
						  itlist,
						  NULL,
						  resultRelation,
						  rtoffset);

	pfree(itlist);

	return rlist;
}


/*
 * find_minmax_agg_replacement_param
 *		If the given Aggref is one that we are optimizing into a subquery
 *		(cf. planagg.c), then return the Param that should replace it.
 *		Else return NULL.
 *
 * This is exported so that SS_finalize_plan can use it before setrefs.c runs.
 * Note that it will not find anything until we have built a Plan from a
 * MinMaxAggPath, as root->minmax_aggs will never be filled otherwise.
 */
Param *
find_minmax_agg_replacement_param(PlannerInfo *root, Aggref *aggref)
{
	if (root->minmax_aggs != NIL &&
		list_length(aggref->args) == 1)
	{
		TargetEntry *curTarget = (TargetEntry *) linitial(aggref->args);
		ListCell   *lc;

		foreach(lc, root->minmax_aggs)
		{
			MinMaxAggInfo *mminfo = (MinMaxAggInfo *) lfirst(lc);

			if (mminfo->aggfnoid == aggref->aggfnoid &&
				equal(mminfo->target, curTarget->expr))
				return mminfo->param;
		}
	}
	return NULL;
}


/*****************************************************************************
 *					QUERY DEPENDENCY MANAGEMENT
 *****************************************************************************/

/*
 * record_plan_function_dependency
 *		Mark the current plan as depending on a particular function.
 *
 * This is exported so that the function-inlining code can record a
 * dependency on a function that it's removed from the plan tree.
 */
void
record_plan_function_dependency(PlannerInfo *root, Oid funcid)
{
	/*
	 * For performance reasons, we don't bother to track built-in functions;
	 * we just assume they'll never change (or at least not in ways that'd
	 * invalidate plans using them).  For this purpose we can consider a
	 * built-in function to be one with OID less than FirstBootstrapObjectId.
	 * Note that the OID generator guarantees never to generate such an OID
	 * after startup, even at OID wraparound.
	 */
	if (funcid >= (Oid) FirstBootstrapObjectId)
	{
		PlanInvalItem *inval_item = makeNode(PlanInvalItem);

		/*
		 * It would work to use any syscache on pg_proc, but the easiest is
		 * PROCOID since we already have the function's OID at hand.  Note
		 * that plancache.c knows we use PROCOID.
		 */
		inval_item->cacheId = PROCOID;
		inval_item->hashValue = GetSysCacheHashValue1(PROCOID,
													  ObjectIdGetDatum(funcid));

		root->glob->invalItems = lappend(root->glob->invalItems, inval_item);
	}
}

/*
 * record_plan_type_dependency
 *		Mark the current plan as depending on a particular type.
 *
 * This is exported so that eval_const_expressions can record a
 * dependency on a domain that it's removed a CoerceToDomain node for.
 *
 * We don't currently need to record dependencies on domains that the
 * plan contains CoerceToDomain nodes for, though that might change in
 * future.  Hence, this isn't actually called in this module, though
 * someday fix_expr_common might call it.
 */
void
record_plan_type_dependency(PlannerInfo *root, Oid typid)
{
	/*
	 * As in record_plan_function_dependency, ignore the possibility that
	 * someone would change a built-in domain.
	 */
	if (typid >= (Oid) FirstBootstrapObjectId)
	{
		PlanInvalItem *inval_item = makeNode(PlanInvalItem);

		/*
		 * It would work to use any syscache on pg_type, but the easiest is
		 * TYPEOID since we already have the type's OID at hand.  Note that
		 * plancache.c knows we use TYPEOID.
		 */
		inval_item->cacheId = TYPEOID;
		inval_item->hashValue = GetSysCacheHashValue1(TYPEOID,
													  ObjectIdGetDatum(typid));

		root->glob->invalItems = lappend(root->glob->invalItems, inval_item);
	}
}

/*
 * extract_query_dependencies
 *		Given a rewritten, but not yet planned, query or queries
 *		(i.e. a Query node or list of Query nodes), extract dependencies
 *		just as set_plan_references would do.  Also detect whether any
 *		rewrite steps were affected by RLS.
 *
 * This is needed by plancache.c to handle invalidation of cached unplanned
 * queries.
 *
 * Note: this does not go through eval_const_expressions, and hence doesn't
 * reflect its additions of inlined functions and elided CoerceToDomain nodes
 * to the invalItems list.  This is obviously OK for functions, since we'll
 * see them in the original query tree anyway.  For domains, it's OK because
 * we don't care about domains unless they get elided.  That is, a plan might
 * have domain dependencies that the query tree doesn't.
 */
void
extract_query_dependencies(Node *query,
						   List **relationOids,
						   List **invalItems,
						   bool *hasRowSecurity)
{
	PlannerGlobal glob;
	PlannerInfo root;

	/* Make up dummy planner state so we can use this module's machinery */
	MemSet(&glob, 0, sizeof(glob));
	glob.type = T_PlannerGlobal;
	glob.relationOids = NIL;
	glob.invalItems = NIL;
	/* Hack: we use glob.dependsOnRole to collect hasRowSecurity flags */
	glob.dependsOnRole = false;

	MemSet(&root, 0, sizeof(root));
	root.type = T_PlannerInfo;
	root.glob = &glob;

	(void) extract_query_dependencies_walker(query, &root);

	*relationOids = glob.relationOids;
	*invalItems = glob.invalItems;
	*hasRowSecurity = glob.dependsOnRole;
}

/*
 * Tree walker for extract_query_dependencies.
 *
 * This is exported so that expression_planner_with_deps can call it on
 * simple expressions (post-planning, not before planning, in that case).
 * In that usage, glob.dependsOnRole isn't meaningful, but the relationOids
 * and invalItems lists are added to as needed.
 */
bool
extract_query_dependencies_walker(Node *node, PlannerInfo *context)
{
	if (node == NULL)
		return false;
	Assert(!IsA(node, PlaceHolderVar));
	if (IsA(node, Query))
	{
		Query	   *query = (Query *) node;
		ListCell   *lc;

		if (query->commandType == CMD_UTILITY)
		{
			/*
			 * This logic must handle any utility command for which parse
			 * analysis was nontrivial (cf. stmt_requires_parse_analysis).
			 *
			 * Notably, CALL requires its own processing.
			 */
			if (IsA(query->utilityStmt, CallStmt))
			{
				CallStmt   *callstmt = (CallStmt *) query->utilityStmt;

				/* We need not examine funccall, just the transformed exprs */
				(void) extract_query_dependencies_walker((Node *) callstmt->funcexpr,
														 context);
				return false;
			}

			/*
			 * Ignore other utility statements, except those (such as EXPLAIN)
			 * that contain a parsed-but-not-planned query.  For those, we
			 * just need to transfer our attention to the contained query.
			 */
			query = UtilityContainsQuery(query->utilityStmt);
			if (query == NULL)
				return false;
		}

		/* Remember if any Query has RLS quals applied by rewriter */
		if (query->hasRowSecurity)
			context->glob->dependsOnRole = true;

		/* Collect relation OIDs in this Query's rtable */
		foreach(lc, query->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

			if (rte->rtekind == RTE_RELATION)
				context->glob->relationOids =
					lappend_oid(context->glob->relationOids, rte->relid);
			else if (rte->rtekind == RTE_NAMEDTUPLESTORE &&
					 OidIsValid(rte->relid))
				context->glob->relationOids =
					lappend_oid(context->glob->relationOids,
								rte->relid);
		}

		/* And recurse into the query's subexpressions */
		return query_tree_walker(query, extract_query_dependencies_walker,
								 (void *) context, 0);
	}
	/* Extract function dependencies and check for regclass Consts */
	fix_expr_common(context, node);
	return expression_tree_walker(node, extract_query_dependencies_walker,
								  (void *) context);
}
