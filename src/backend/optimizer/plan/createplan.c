/*-------------------------------------------------------------------------
 *
 * createplan.c
 *	  Routines to create the desired plan for processing a query.
 *	  Planning is complete, we just need to convert the selected
 *	  Path into a Plan.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/createplan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>
#include <math.h>

#include "access/sysattr.h"
#include "catalog/pg_class.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/paramassign.h"
#include "optimizer/paths.h"
#include "optimizer/placeholder.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "parser/parse_clause.h"
#include "parser/parsetree.h"
#include "partitioning/partprune.h"
#include "tcop/tcopprot.h"
#include "utils/lsyscache.h"


/*
 * create_plan_recurse() 的 flags 参数可用的标志位，可以按位或组合使用。
 *
 * CP_EXACT_TLIST 表示生成的计划节点必须返回与路径的 pathtarget 完全一致的目标列列表（tlist），
 * 该标志优先级高于 CP_SMALL_TLIST 和 CP_LABEL_TLIST。
 * 否则，计划节点只需返回用于计算 pathtarget 的 Vars 和 PlaceHolderVars。
 *
 * CP_SMALL_TLIST 表示优先生成更窄的目标列列表。父节点如 Sort 和 Hash 需要存储返回的元组时会传递此标志。
 *
 * CP_LABEL_TLIST 表示计划节点必须返回与 pathtarget 中指定的 sortgrouprefs 匹配的列，并正确设置 ressortgroupref 标签。
 * 父节点如 Sort 和 Group 需要输入中包含这些值时会传递此标志。
 *
 * CP_IGNORE_TLIST 表示调用者会替换目标列列表，因此生成的 tlist 无需关心。
 */
#define CP_EXACT_TLIST		0x0001	/* 计划必须返回指定的目标列列表 */
#define CP_SMALL_TLIST		0x0002	/* 优先生成更窄的目标列列表 */
#define CP_LABEL_TLIST		0x0004	/* tlist 必须包含 sortgrouprefs 标签 */
#define CP_IGNORE_TLIST		0x0008	/* 调用者会替换 tlist，无需关心内容 */


static Plan *create_plan_recurse(PlannerInfo *root, Path *best_path,
								 int flags);
static Plan *create_scan_plan(PlannerInfo *root, Path *best_path,
							  int flags);
static List *build_path_tlist(PlannerInfo *root, Path *path);
static bool use_physical_tlist(PlannerInfo *root, Path *path, int flags);
static List *get_gating_quals(PlannerInfo *root, List *quals);
static Plan *create_gating_plan(PlannerInfo *root, Path *path, Plan *plan,
								List *gating_quals);
static Plan *create_join_plan(PlannerInfo *root, JoinPath *best_path);
static Plan *create_append_plan(PlannerInfo *root, AppendPath *best_path,
								int flags);
static Plan *create_merge_append_plan(PlannerInfo *root, MergeAppendPath *best_path,
									  int flags);
static Result *create_group_result_plan(PlannerInfo *root,
										GroupResultPath *best_path);
static ProjectSet *create_project_set_plan(PlannerInfo *root, ProjectSetPath *best_path);
static Material *create_material_plan(PlannerInfo *root, MaterialPath *best_path,
									  int flags);
static Plan *create_unique_plan(PlannerInfo *root, UniquePath *best_path,
								int flags);
static Gather *create_gather_plan(PlannerInfo *root, GatherPath *best_path);
static Plan *create_projection_plan(PlannerInfo *root,
									ProjectionPath *best_path,
									int flags);
static Plan *inject_projection_plan(Plan *subplan, List *tlist, bool parallel_safe);
static Sort *create_sort_plan(PlannerInfo *root, SortPath *best_path, int flags);
static Group *create_group_plan(PlannerInfo *root, GroupPath *best_path);
static Unique *create_upper_unique_plan(PlannerInfo *root, UpperUniquePath *best_path,
										int flags);
static Agg *create_agg_plan(PlannerInfo *root, AggPath *best_path);
static Plan *create_groupingsets_plan(PlannerInfo *root, GroupingSetsPath *best_path);
static Result *create_minmaxagg_plan(PlannerInfo *root, MinMaxAggPath *best_path);
static WindowAgg *create_windowagg_plan(PlannerInfo *root, WindowAggPath *best_path);
static SetOp *create_setop_plan(PlannerInfo *root, SetOpPath *best_path,
								int flags);
static RecursiveUnion *create_recursiveunion_plan(PlannerInfo *root, RecursiveUnionPath *best_path);
static LockRows *create_lockrows_plan(PlannerInfo *root, LockRowsPath *best_path,
									  int flags);
static ModifyTable *create_modifytable_plan(PlannerInfo *root, ModifyTablePath *best_path);
static Limit *create_limit_plan(PlannerInfo *root, LimitPath *best_path,
								int flags);
static SeqScan *create_seqscan_plan(PlannerInfo *root, Path *best_path,
									List *tlist, List *scan_clauses);
static SampleScan *create_samplescan_plan(PlannerInfo *root, Path *best_path,
										  List *tlist, List *scan_clauses);
static Scan *create_indexscan_plan(PlannerInfo *root, IndexPath *best_path,
								   List *tlist, List *scan_clauses, bool indexonly);
static BitmapHeapScan *create_bitmap_scan_plan(PlannerInfo *root,
											   BitmapHeapPath *best_path,
											   List *tlist, List *scan_clauses);
static Plan *create_bitmap_subplan(PlannerInfo *root, Path *bitmapqual,
								   List **qual, List **indexqual, List **indexECs);
static void bitmap_subplan_mark_shared(Plan *plan);
static TidScan *create_tidscan_plan(PlannerInfo *root, TidPath *best_path,
									List *tlist, List *scan_clauses);
static SubqueryScan *create_subqueryscan_plan(PlannerInfo *root,
											  SubqueryScanPath *best_path,
											  List *tlist, List *scan_clauses);
static FunctionScan *create_functionscan_plan(PlannerInfo *root, Path *best_path,
											  List *tlist, List *scan_clauses);
static ValuesScan *create_valuesscan_plan(PlannerInfo *root, Path *best_path,
										  List *tlist, List *scan_clauses);
static TableFuncScan *create_tablefuncscan_plan(PlannerInfo *root, Path *best_path,
												List *tlist, List *scan_clauses);
static CteScan *create_ctescan_plan(PlannerInfo *root, Path *best_path,
									List *tlist, List *scan_clauses);
static NamedTuplestoreScan *create_namedtuplestorescan_plan(PlannerInfo *root,
															Path *best_path, List *tlist, List *scan_clauses);
static Result *create_resultscan_plan(PlannerInfo *root, Path *best_path,
									  List *tlist, List *scan_clauses);
static WorkTableScan *create_worktablescan_plan(PlannerInfo *root, Path *best_path,
												List *tlist, List *scan_clauses);
static ForeignScan *create_foreignscan_plan(PlannerInfo *root, ForeignPath *best_path,
											List *tlist, List *scan_clauses);
static CustomScan *create_customscan_plan(PlannerInfo *root,
										  CustomPath *best_path,
										  List *tlist, List *scan_clauses);
static NestLoop *create_nestloop_plan(PlannerInfo *root, NestPath *best_path);
static MergeJoin *create_mergejoin_plan(PlannerInfo *root, MergePath *best_path);
static HashJoin *create_hashjoin_plan(PlannerInfo *root, HashPath *best_path);
static Node *replace_nestloop_params(PlannerInfo *root, Node *expr);
static Node *replace_nestloop_params_mutator(Node *node, PlannerInfo *root);
static void fix_indexqual_references(PlannerInfo *root, IndexPath *index_path,
									 List **stripped_indexquals_p,
									 List **fixed_indexquals_p);
static List *fix_indexorderby_references(PlannerInfo *root, IndexPath *index_path);
static Node *fix_indexqual_clause(PlannerInfo *root,
								  IndexOptInfo *index, int indexcol,
								  Node *clause, List *indexcolnos);
static Node *fix_indexqual_operand(Node *node, IndexOptInfo *index, int indexcol);
static List *get_switched_clauses(List *clauses, Relids outerrelids);
static List *order_qual_clauses(PlannerInfo *root, List *clauses);
static void copy_generic_path_info(Plan *dest, Path *src);
static void copy_plan_costsize(Plan *dest, Plan *src);
static void label_sort_with_costsize(PlannerInfo *root, Sort *plan,
									 double limit_tuples);
static SeqScan *make_seqscan(List *qptlist, List *qpqual, Index scanrelid);
static SampleScan *make_samplescan(List *qptlist, List *qpqual, Index scanrelid,
								   TableSampleClause *tsc);
static IndexScan *make_indexscan(List *qptlist, List *qpqual, Index scanrelid,
								 Oid indexid, List *indexqual, List *indexqualorig,
								 List *indexorderby, List *indexorderbyorig,
								 List *indexorderbyops,
								 ScanDirection indexscandir);
static IndexOnlyScan *make_indexonlyscan(List *qptlist, List *qpqual,
										 Index scanrelid, Oid indexid,
										 List *indexqual, List *recheckqual,
										 List *indexorderby,
										 List *indextlist,
										 ScanDirection indexscandir);
static BitmapIndexScan *make_bitmap_indexscan(Index scanrelid, Oid indexid,
											  List *indexqual,
											  List *indexqualorig);
static BitmapHeapScan *make_bitmap_heapscan(List *qptlist,
											List *qpqual,
											Plan *lefttree,
											List *bitmapqualorig,
											Index scanrelid);
static TidScan *make_tidscan(List *qptlist, List *qpqual, Index scanrelid,
							 List *tidquals);
static SubqueryScan *make_subqueryscan(List *qptlist,
									   List *qpqual,
									   Index scanrelid,
									   Plan *subplan);
static FunctionScan *make_functionscan(List *qptlist, List *qpqual,
									   Index scanrelid, List *functions, bool funcordinality);
static ValuesScan *make_valuesscan(List *qptlist, List *qpqual,
								   Index scanrelid, List *values_lists);
static TableFuncScan *make_tablefuncscan(List *qptlist, List *qpqual,
										 Index scanrelid, TableFunc *tablefunc);
static CteScan *make_ctescan(List *qptlist, List *qpqual,
							 Index scanrelid, int ctePlanId, int cteParam);
static NamedTuplestoreScan *make_namedtuplestorescan(List *qptlist, List *qpqual,
													 Index scanrelid, char *enrname);
static WorkTableScan *make_worktablescan(List *qptlist, List *qpqual,
										 Index scanrelid, int wtParam);
static RecursiveUnion *make_recursive_union(List *tlist,
											Plan *lefttree,
											Plan *righttree,
											int wtParam,
											List *distinctList,
											long numGroups);
static BitmapAnd *make_bitmap_and(List *bitmapplans);
static BitmapOr *make_bitmap_or(List *bitmapplans);
static NestLoop *make_nestloop(List *tlist,
							   List *joinclauses, List *otherclauses, List *nestParams,
							   Plan *lefttree, Plan *righttree,
							   JoinType jointype, bool inner_unique);
static HashJoin *make_hashjoin(List *tlist,
							   List *joinclauses, List *otherclauses,
							   List *hashclauses,
							   List *hashoperators, List *hashcollations,
							   List *hashkeys,
							   Plan *lefttree, Plan *righttree,
							   JoinType jointype, bool inner_unique);
static Hash *make_hash(Plan *lefttree,
					   List *hashkeys,
					   Oid skewTable,
					   AttrNumber skewColumn,
					   bool skewInherit);
static MergeJoin *make_mergejoin(List *tlist,
								 List *joinclauses, List *otherclauses,
								 List *mergeclauses,
								 Oid *mergefamilies,
								 Oid *mergecollations,
								 int *mergestrategies,
								 bool *mergenullsfirst,
								 Plan *lefttree, Plan *righttree,
								 JoinType jointype, bool inner_unique,
								 bool skip_mark_restore);
static Sort *make_sort(Plan *lefttree, int numCols,
					   AttrNumber *sortColIdx, Oid *sortOperators,
					   Oid *collations, bool *nullsFirst);
static Plan *prepare_sort_from_pathkeys(Plan *lefttree, List *pathkeys,
										Relids relids,
										const AttrNumber *reqColIdx,
										bool adjust_tlist_in_place,
										int *p_numsortkeys,
										AttrNumber **p_sortColIdx,
										Oid **p_sortOperators,
										Oid **p_collations,
										bool **p_nullsFirst);
static EquivalenceMember *find_ec_member_for_tle(EquivalenceClass *ec,
												 TargetEntry *tle,
												 Relids relids);
static Sort *make_sort_from_pathkeys(Plan *lefttree, List *pathkeys,
									 Relids relids);
static Sort *make_sort_from_groupcols(List *groupcls,
									  AttrNumber *grpColIdx,
									  Plan *lefttree);
static Material *make_material(Plan *lefttree);
static WindowAgg *make_windowagg(List *tlist, Index winref,
								 int partNumCols, AttrNumber *partColIdx, Oid *partOperators, Oid *partCollations,
								 int ordNumCols, AttrNumber *ordColIdx, Oid *ordOperators, Oid *ordCollations,
								 int frameOptions, Node *startOffset, Node *endOffset,
								 Oid startInRangeFunc, Oid endInRangeFunc,
								 Oid inRangeColl, bool inRangeAsc, bool inRangeNullsFirst,
								 Plan *lefttree);
static Group *make_group(List *tlist, List *qual, int numGroupCols,
						 AttrNumber *grpColIdx, Oid *grpOperators, Oid *grpCollations,
						 Plan *lefttree);
static Unique *make_unique_from_sortclauses(Plan *lefttree, List *distinctList);
static Unique *make_unique_from_pathkeys(Plan *lefttree,
										 List *pathkeys, int numCols);
static Gather *make_gather(List *qptlist, List *qpqual,
						   int nworkers, int rescan_param, bool single_copy, Plan *subplan);
static SetOp *make_setop(SetOpCmd cmd, SetOpStrategy strategy, Plan *lefttree,
						 List *distinctList, AttrNumber flagColIdx, int firstFlag,
						 long numGroups);
static LockRows *make_lockrows(Plan *lefttree, List *rowMarks, int epqParam);
static Result *make_result(List *tlist, Node *resconstantqual, Plan *subplan);
static ProjectSet *make_project_set(List *tlist, Plan *subplan);
static ModifyTable *make_modifytable(PlannerInfo *root,
									 CmdType operation, bool canSetTag,
									 Index nominalRelation, Index rootRelation,
									 bool partColsUpdated,
									 List *resultRelations, List *subplans, List *subroots,
									 List *withCheckOptionLists, List *returningLists,
									 List *rowMarks, OnConflictExpr *onconflict, int epqParam);
static GatherMerge *create_gather_merge_plan(PlannerInfo *root,
											 GatherMergePath *best_path);


/*
 * create_plan
 *	  创建查询的访问计划，通过递归处理以 'best_path' 为根的路径节点树。
 *	  对于每个路径节点，创建一个对应的计划节点，包含适当的 id、目标列列表和条件信息。
 *
 *	  计划树中的 tlist 和 quals 仍然是规划器格式，即 Vars 仍然对应解析器的编号。
 *	  这些将在 setrefs.c 中修正。
 *
 *	  best_path 是最佳访问路径
 *
 *	  返回一个 Plan 计划树。
 */
Plan *
create_plan(PlannerInfo *root, Path *best_path)
{
	Plan	   *plan;

	/* 当前查询级别不应使用 plan_params */
	Assert(root->plan_params == NIL);

	/* 初始化 PlannerInfo 的工作区 */
	root->curOuterRels = NULL;
	root->curOuterParams = NIL;

	/* 递归处理路径树，要求返回正确的目标列列表 */
	plan = create_plan_recurse(root, best_path, CP_EXACT_TLIST);

	/*
	 * 确保最顶层计划节点的目标列列表包含原始列名和其他修饰信息。
	 * 规划器内部生成的目标列列表不会包含这些信息，但在执行时顶层 tlist 必须有。
	 * 但是，ModifyTable 计划节点的 tlist 不匹配查询树的目标列列表。
	 */
	if (!IsA(plan, ModifyTable))
		apply_tlist_labeling(plan->targetlist, root->processed_tlist);

	/*
	 * 将本查询级别创建的所有 initPlans 附加到最顶层计划节点。
	 * （原则上 initplans 可以附加到任何引用它们的计划节点或更高节点，
	 * 但没有理由放得比顶层更低。参见 SS_finalize_plan 的注释。）
	 */
	SS_attach_initplans(root, plan);

	/* 检查所有 NestLoopParams 是否都成功分配到计划节点 */
	if (root->curOuterParams != NIL)
		elog(ERROR, "failed to assign all NestLoopParams to plan nodes");

	/*
	 * 重置 plan_params，确保 nestloop params 使用的 param ID 不会被后续复用
	 */
	root->plan_params = NIL;

	return plan;
}

/*
 * create_plan_recurse
 *	  create_plan() 的递归核心实现。
 *	  根据 best_path 的 pathtype，递归生成对应的 Plan 节点。
 */
static Plan *
create_plan_recurse(PlannerInfo *root, Path *best_path, int flags)
{
	Plan	   *plan;

	/* 防止因计划过于复杂导致栈溢出 */
	check_stack_depth();

	switch (best_path->pathtype)
	{
		/* 基础扫描节点 */
		case T_SeqScan:
		case T_SampleScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_TableFuncScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_WorkTableScan:
		case T_NamedTuplestoreScan:
		case T_ForeignScan:
		case T_CustomScan:
			plan = create_scan_plan(root, best_path, flags);
			break;
		/* 连接节点 */
		case T_HashJoin:
		case T_MergeJoin:
		case T_NestLoop:
			plan = create_join_plan(root,
									(JoinPath *) best_path);
			break;
		/* Append 相关节点 */
		case T_Append:
			plan = create_append_plan(root,
									  (AppendPath *) best_path,
									  flags);
			break;
		case T_MergeAppend:
			plan = create_merge_append_plan(root,
											(MergeAppendPath *) best_path,
											flags);
			break;
		/* Result 相关节点 */
		case T_Result:
			if (IsA(best_path, ProjectionPath))
			{
				plan = create_projection_plan(root,
											  (ProjectionPath *) best_path,
											  flags);
			}
			else if (IsA(best_path, MinMaxAggPath))
			{
				plan = (Plan *) create_minmaxagg_plan(root,
													  (MinMaxAggPath *) best_path);
			}
			else if (IsA(best_path, GroupResultPath))
			{
				plan = (Plan *) create_group_result_plan(root,
														 (GroupResultPath *) best_path);
			}
			else
			{
				/* 简单的 RTE_RESULT 基础关系 */
				Assert(IsA(best_path, Path));
				plan = create_scan_plan(root, best_path, flags);
			}
			break;
		/* SRF 投影节点 */
		case T_ProjectSet:
			plan = (Plan *) create_project_set_plan(root,
													(ProjectSetPath *) best_path);
			break;
		/* 物化节点 */
		case T_Material:
			plan = (Plan *) create_material_plan(root,
												 (MaterialPath *) best_path,
												 flags);
			break;
		/* 唯一化节点 */
		case T_Unique:
			if (IsA(best_path, UpperUniquePath))
			{
				plan = (Plan *) create_upper_unique_plan(root,
														 (UpperUniquePath *) best_path,
														 flags);
			}
			else
			{
				Assert(IsA(best_path, UniquePath));
				plan = create_unique_plan(root,
										  (UniquePath *) best_path,
										  flags);
			}
			break;
		/* 并行收集节点 */
		case T_Gather:
			plan = (Plan *) create_gather_plan(root,
											   (GatherPath *) best_path);
			break;
		/* 排序节点 */
		case T_Sort:
			plan = (Plan *) create_sort_plan(root,
											 (SortPath *) best_path,
											 flags);
			break;
		/* 分组节点 */
		case T_Group:
			plan = (Plan *) create_group_plan(root,
											  (GroupPath *) best_path);
			break;
		/* 聚合节点 */
		case T_Agg:
			if (IsA(best_path, GroupingSetsPath))
				plan = create_groupingsets_plan(root,
												(GroupingSetsPath *) best_path);
			else
			{
				Assert(IsA(best_path, AggPath));
				plan = (Plan *) create_agg_plan(root,
												(AggPath *) best_path);
			}
			break;
		/* 窗口聚合节点 */
		case T_WindowAgg:
			plan = (Plan *) create_windowagg_plan(root,
												  (WindowAggPath *) best_path);
			break;
		/* 集合操作节点 */
		case T_SetOp:
			plan = (Plan *) create_setop_plan(root,
											  (SetOpPath *) best_path,
											  flags);
			break;
		/* 递归 UNION 节点 */
		case T_RecursiveUnion:
			plan = (Plan *) create_recursiveunion_plan(root,
													   (RecursiveUnionPath *) best_path);
			break;
		/* 行锁节点 */
		case T_LockRows:
			plan = (Plan *) create_lockrows_plan(root,
												 (LockRowsPath *) best_path,
												 flags);
			break;
		/* DML 节点 */
		case T_ModifyTable:
			plan = (Plan *) create_modifytable_plan(root,
													(ModifyTablePath *) best_path);
			break;
		/* Limit 节点 */
		case T_Limit:
			plan = (Plan *) create_limit_plan(root,
											  (LimitPath *) best_path,
											  flags);
			break;
		/* 并行归并收集节点 */
		case T_GatherMerge:
			plan = (Plan *) create_gather_merge_plan(root,
													 (GatherMergePath *) best_path);
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) best_path->pathtype);
			plan = NULL;		/* 防止编译器警告 */
			break;
	}

	return plan;
}

/*
 * create_scan_plan
 *	  为 'best_path' 的父关系创建一个扫描计划节点。
 */
static Plan *
create_scan_plan(PlannerInfo *root, Path *best_path, int flags)
{
	RelOptInfo *rel = best_path->parent;
	List	   *scan_clauses;
	List	   *gating_clauses;
	List	   *tlist;
	Plan	   *plan;

	/*
	 * 提取父关系的限制条件（baserestrictinfo）。执行器必须在扫描期间应用所有这些限制，
	 * 除了伪常量（pseudoconstant）条件，后面会单独处理。
	 *
	 * 如果是普通的索引扫描或索引仅扫描，则只考虑索引谓词未隐含的限制条件（indrestrictinfo），
	 * 而不是 baserestrictinfo。对于位图索引扫描则不能这样做，但 create_bitmap_scan_plan
	 * 会自动处理这些条件。
	 */
	switch (best_path->pathtype)
	{
		case T_IndexScan:
		case T_IndexOnlyScan:
			scan_clauses = castNode(IndexPath, best_path)->indexinfo->indrestrictinfo;
			break;
		default:
			scan_clauses = rel->baserestrictinfo;
			break;
	}

	/*
	 * 如果是参数化扫描，还需要添加外部关系的连接条件（ppi_clauses）。
	 * 为安全起见，不要修改原始 baserestrictinfo 列表。
	 */
	if (best_path->param_info)
		scan_clauses = list_concat(list_copy(scan_clauses),
								   best_path->param_info->ppi_clauses);

	/*
	 * 检查是否有伪常量条件需要处理。如果需要插入 gating Result 节点，则该节点可以做投影，
	 * 所以对子计划的 tlist 没有特殊要求。
	 */
	gating_clauses = get_gating_quals(root, scan_clauses);
	if (gating_clauses)
		flags = 0;

	/*
	 * 对于表扫描，优先生成包含所有物理列的 tlist（物理顺序），这样执行器可以优化投影过程。
	 * 如果调用者会忽略 tlist，则直接设为 NULL。只有当 flags 仅为 CP_IGNORE_TLIST 时才这样做。
	 */
	if (flags == CP_IGNORE_TLIST)
	{
		tlist = NULL;
	}
	else if (use_physical_tlist(root, best_path, flags))
	{
		if (best_path->pathtype == T_IndexOnlyScan)
		{
			/* 对于索引仅扫描，优先使用索引的 indextlist */
			tlist = copyObject(((IndexPath *) best_path)->indexinfo->indextlist);

			/* 如果需要，转移 sortgroupref 信息到新 tlist（use_physical_tlist 已检查可行性） */
			if (flags & CP_LABEL_TLIST)
				apply_pathtarget_labeling_to_tlist(tlist, best_path->pathtarget);
		}
		else
		{
			tlist = build_physical_tlist(root, rel);
			if (tlist == NIL)
			{
				/* 如果物理 tlist 构建失败（如有被删除的列），则用常规方法 */
				tlist = build_path_tlist(root, best_path);
			}
			else
			{
				/* 同样转移 sortgroupref 信息 */
				if (flags & CP_LABEL_TLIST)
					apply_pathtarget_labeling_to_tlist(tlist, best_path->pathtarget);
			}
		}
	}
	else
	{
		tlist = build_path_tlist(root, best_path);
	}

	/*
	 * 根据 best_path 的类型，调用对应的扫描计划构建函数。
	 */
	switch (best_path->pathtype)
	{
		case T_SeqScan:
			plan = (Plan *) create_seqscan_plan(root,
												best_path,
												tlist,
												scan_clauses);
			break;

		case T_SampleScan:
			plan = (Plan *) create_samplescan_plan(root,
												   best_path,
												   tlist,
												   scan_clauses);
			break;

		case T_IndexScan:
			plan = (Plan *) create_indexscan_plan(root,
												  (IndexPath *) best_path,
												  tlist,
												  scan_clauses,
												  false);
			break;

		case T_IndexOnlyScan:
			plan = (Plan *) create_indexscan_plan(root,
												  (IndexPath *) best_path,
												  tlist,
												  scan_clauses,
												  true);
			break;

		case T_BitmapHeapScan:
			plan = (Plan *) create_bitmap_scan_plan(root,
													(BitmapHeapPath *) best_path,
													tlist,
													scan_clauses);
			break;

		case T_TidScan:
			plan = (Plan *) create_tidscan_plan(root,
												(TidPath *) best_path,
												tlist,
												scan_clauses);
			break;

		case T_SubqueryScan:
			plan = (Plan *) create_subqueryscan_plan(root,
													 (SubqueryScanPath *) best_path,
													 tlist,
													 scan_clauses);
			break;

		case T_FunctionScan:
			plan = (Plan *) create_functionscan_plan(root,
													 best_path,
													 tlist,
													 scan_clauses);
			break;

		case T_TableFuncScan:
			plan = (Plan *) create_tablefuncscan_plan(root,
													  best_path,
													  tlist,
													  scan_clauses);
			break;

		case T_ValuesScan:
			plan = (Plan *) create_valuesscan_plan(root,
												   best_path,
												   tlist,
												   scan_clauses);
			break;

		case T_CteScan:
			plan = (Plan *) create_ctescan_plan(root,
												best_path,
												tlist,
												scan_clauses);
			break;

		case T_NamedTuplestoreScan:
			plan = (Plan *) create_namedtuplestorescan_plan(root,
															best_path,
															tlist,
															scan_clauses);
			break;

		case T_Result:
			plan = (Plan *) create_resultscan_plan(root,
												   best_path,
												   tlist,
												   scan_clauses);
			break;

		case T_WorkTableScan:
			plan = (Plan *) create_worktablescan_plan(root,
													  best_path,
													  tlist,
													  scan_clauses);
			break;

		case T_ForeignScan:
			plan = (Plan *) create_foreignscan_plan(root,
													(ForeignPath *) best_path,
													tlist,
													scan_clauses);
			break;

		case T_CustomScan:
			plan = (Plan *) create_customscan_plan(root,
												   (CustomPath *) best_path,
												   tlist,
												   scan_clauses);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) best_path->pathtype);
			plan = NULL;		/* 防止编译器警告 */
			break;
	}

	/*
	 * 如果有伪常量条件，则在计划节点上方插入 gating Result 节点，
	 * 用于一次性评估这些条件。
	 */
	if (gating_clauses)
		plan = create_gating_plan(root, best_path, plan, gating_clauses);

	return plan;
}
/*
 * 构建路径节点的目标列列表（TargetEntry 列表）。
 *
 * 基本等价于 make_tlist_from_pathtarget()，但需要处理 nestloop 参数替换。
 */
static List *
build_path_tlist(PlannerInfo *root, Path *path)
{
	List	   *tlist = NIL;
	Index	   *sortgrouprefs = path->pathtarget->sortgrouprefs;
	int			resno = 1;
	ListCell   *v;

	foreach(v, path->pathtarget->exprs)
	{
		Node	   *node = (Node *) lfirst(v);
		TargetEntry *tle;

		/*
		 * 如果是参数化路径，tlist 可能包含外部引用，需要替换为 nestloop 参数。
		 * 无需重新构造 TargetEntry，只需对每个表达式单独处理即可。
		 */
		if (path->param_info)
			node = replace_nestloop_params(root, node);

		tle = makeTargetEntry((Expr *) node,
							  resno,
							  NULL,
							  false);
		if (sortgrouprefs)
			tle->ressortgroupref = sortgrouprefs[resno - 1];

		tlist = lappend(tlist, tle);
		resno++;
	}
	return tlist;
}

/*
 * use_physical_tlist
 *		判断是否可以使用与物理表结构一致的目标列列表（tlist），
 *		而不是仅包含实际引用的 Vars。
 *
 *		返回 true 表示可以直接使用物理 tlist，无需额外投影。
 */
static bool
use_physical_tlist(PlannerInfo *root, Path *path, int flags)
{
	RelOptInfo *rel = path->parent;
	int			i;
	ListCell   *lc;

	/*
	 * 如果要求精确 tlist 或更窄 tlist，则不能使用物理 tlist。
	 */
	if (flags & (CP_EXACT_TLIST | CP_SMALL_TLIST))
		return false;

	/*
	 * 仅对基础关系扫描、子查询扫描、函数扫描、TableFunc 扫描、Values 扫描和 CTE 扫描有效，
	 * 连接等其他类型不能直接用物理 tlist。
	 */
	if (rel->rtekind != RTE_RELATION &&
		rel->rtekind != RTE_SUBQUERY &&
		rel->rtekind != RTE_FUNCTION &&
		rel->rtekind != RTE_TABLEFUNC &&
		rel->rtekind != RTE_VALUES &&
		rel->rtekind != RTE_CTE)
		return false;

	/*
	 * 继承场景下不能用物理 tlist（主要因为 Append 节点不做投影）。
	 */
	if (rel->reloptkind != RELOPT_BASEREL)
		return false;

	/*
	 * 对于自定义路径（CustomPath），不使用物理 tlist。
	 */
	if (IsA(path, CustomPath))
		return false;

	/*
	 * 如果 BitmapHeapPath 的 tlist 为空，则保持原样，不用物理 tlist。
	 */
	if (IsA(path, BitmapHeapPath) &&
		path->pathtarget->exprs == NIL)
		return false;

	/*
	 * 如果请求了系统列或 whole-row Vars，则不能用物理 tlist。
	 */
	for (i = rel->min_attr; i <= 0; i++)
	{
		if (!bms_is_empty(rel->attr_needed[i - rel->min_attr]))
			return false;
	}

	/*
	 * 如果需要输出任何 placeholder 表达式，也不能用物理 tlist。
	 */
	foreach(lc, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(lc);

		if (bms_nonempty_difference(phinfo->ph_needed, rel->relids) &&
			bms_is_subset(phinfo->ph_eval_at, rel->relids))
			return false;
	}

	/*
	 * 对于索引仅扫描，只有所有索引列都可返回时才能用 indextlist 作为物理 tlist。
	 */
	if (path->pathtype == T_IndexOnlyScan)
	{
		IndexOptInfo *indexinfo = ((IndexPath *) path)->indexinfo;

		for (i = 0; i < indexinfo->ncolumns; i++)
		{
			if (!indexinfo->canreturn[i])
				return false;
		}
	}

	/*
	 * 如果要求 CP_LABEL_TLIST，且 sort/group 列不是简单 Var，或有重复 Var，则不能用物理 tlist。
	 */
	if ((flags & CP_LABEL_TLIST) && path->pathtarget->sortgrouprefs)
	{
		Bitmapset  *sortgroupatts = NULL;

		i = 0;
		foreach(lc, path->pathtarget->exprs)
		{
			Expr	   *expr = (Expr *) lfirst(lc);

			if (path->pathtarget->sortgrouprefs[i])
			{
				if (expr && IsA(expr, Var))
				{
					int			attno = ((Var *) expr)->varattno;

					attno -= FirstLowInvalidHeapAttributeNumber;
					if (bms_is_member(attno, sortgroupatts))
						return false;
					sortgroupatts = bms_add_member(sortgroupatts, attno);
				}
				else
					return false;
			}
			i++;
		}
	}

	return true;
}

/*
 * get_gating_quals
 *	  See if there are pseudoconstant quals in a node's quals list
 *
 * If the node's quals list includes any pseudoconstant quals,
 * return just those quals.
 */
static List *
get_gating_quals(PlannerInfo *root, List *quals)
{
	/* No need to look if we know there are no pseudoconstants */
	if (!root->hasPseudoConstantQuals)
		return NIL;

	/* Sort into desirable execution order while still in RestrictInfo form */
	quals = order_qual_clauses(root, quals);

	/* Pull out any pseudoconstant quals from the RestrictInfo list */
	return extract_actual_clauses(quals, true);
}

/*
 * create_gating_plan
 *	  Deal with pseudoconstant qual clauses
 *
 * Add a gating Result node atop the already-built plan.
 */
static Plan *
create_gating_plan(PlannerInfo *root, Path *path, Plan *plan,
				   List *gating_quals)
{
	Plan	   *gplan;
	Plan	   *splan;

	Assert(gating_quals);

	/*
	 * We might have a trivial Result plan already.  Stacking one Result atop
	 * another is silly, so if that applies, just discard the input plan.
	 * (We're assuming its targetlist is uninteresting; it should be either
	 * the same as the result of build_path_tlist, or a simplified version.)
	 */
	splan = plan;
	if (IsA(plan, Result))
	{
		Result	   *rplan = (Result *) plan;

		if (rplan->plan.lefttree == NULL &&
			rplan->resconstantqual == NULL)
			splan = NULL;
	}

	/*
	 * Since we need a Result node anyway, always return the path's requested
	 * tlist; that's never a wrong choice, even if the parent node didn't ask
	 * for CP_EXACT_TLIST.
	 */
	gplan = (Plan *) make_result(build_path_tlist(root, path),
								 (Node *) gating_quals,
								 splan);

	/*
	 * Notice that we don't change cost or size estimates when doing gating.
	 * The costs of qual eval were already included in the subplan's cost.
	 * Leaving the size alone amounts to assuming that the gating qual will
	 * succeed, which is the conservative estimate for planning upper queries.
	 * We certainly don't want to assume the output size is zero (unless the
	 * gating qual is actually constant FALSE, and that case is dealt with in
	 * clausesel.c).  Interpolating between the two cases is silly, because it
	 * doesn't reflect what will really happen at runtime, and besides which
	 * in most cases we have only a very bad idea of the probability of the
	 * gating qual being true.
	 */
	copy_plan_costsize(gplan, plan);

	/* Gating quals could be unsafe, so better use the Path's safety flag */
	gplan->parallel_safe = path->parallel_safe;

	return gplan;
}
/*
 * create_join_plan
 *	  为 'best_path' 创建一个连接计划节点，并递归处理其内外路径。
 */
static Plan *
create_join_plan(PlannerInfo *root, JoinPath *best_path)
{
	Plan	   *plan;
	List	   *gating_clauses;

	switch (best_path->path.pathtype)
	{
		case T_MergeJoin:
			plan = (Plan *) create_mergejoin_plan(root,
												  (MergePath *) best_path);
			break;
		case T_HashJoin:
			plan = (Plan *) create_hashjoin_plan(root,
												 (HashPath *) best_path);
			break;
		case T_NestLoop:
			plan = (Plan *) create_nestloop_plan(root,
												 (NestPath *) best_path);
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) best_path->path.pathtype);
			plan = NULL;		/* 防止编译器警告 */
			break;
	}

	/*
	 * 如果该节点有伪常量条件，则在计划节点上方插入 gating Result 节点，
	 * 用于一次性评估这些条件。
	 */
	gating_clauses = get_gating_quals(root, best_path->joinrestrictinfo);
	if (gating_clauses)
		plan = create_gating_plan(root, (Path *) best_path, plan,
								  gating_clauses);

#ifdef NOT_USED
	/*
	 * 代价高的函数上拉可能会将本地谓词拉到该路径节点，
	 * 需要将其放入计划节点的 qpqual 中。
	 * JMH, 6/15/92
	 */
	if (get_loc_restrictinfo(best_path) != NIL)
		set_qpqual((Plan) plan,
				   list_concat(get_qpqual((Plan) plan),
							   get_actual_clauses(get_loc_restrictinfo(best_path))));
#endif

	return plan;
}

/*
 * create_append_plan
 *	  Create an Append plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 *
 *	  Returns a Plan node.
 */
static Plan *
create_append_plan(PlannerInfo *root, AppendPath *best_path, int flags)
{
	Append	   *plan;
	List	   *tlist = build_path_tlist(root, &best_path->path);
	int			orig_tlist_length = list_length(tlist);
	bool		tlist_was_changed = false;
	List	   *pathkeys = best_path->path.pathkeys;
	List	   *subplans = NIL;
	ListCell   *subpaths;
	RelOptInfo *rel = best_path->path.parent;
	PartitionPruneInfo *partpruneinfo = NULL;
	int			nodenumsortkeys = 0;
	AttrNumber *nodeSortColIdx = NULL;
	Oid		   *nodeSortOperators = NULL;
	Oid		   *nodeCollations = NULL;
	bool	   *nodeNullsFirst = NULL;

	/*
	 * The subpaths list could be empty, if every child was proven empty by
	 * constraint exclusion.  In that case generate a dummy plan that returns
	 * no rows.
	 *
	 * Note that an AppendPath with no members is also generated in certain
	 * cases where there was no appending construct at all, but we know the
	 * relation is empty (see set_dummy_rel_pathlist and mark_dummy_rel).
	 */
	if (best_path->subpaths == NIL)
	{
		/* Generate a Result plan with constant-FALSE gating qual */
		Plan	   *plan;

		plan = (Plan *) make_result(tlist,
									(Node *) list_make1(makeBoolConst(false,
																	  false)),
									NULL);

		copy_generic_path_info(plan, (Path *) best_path);

		return plan;
	}

	/*
	 * Otherwise build an Append plan.  Note that if there's just one child,
	 * the Append is pretty useless; but we wait till setrefs.c to get rid of
	 * it.  Doing so here doesn't work because the varno of the child scan
	 * plan won't match the parent-rel Vars it'll be asked to emit.
	 *
	 * We don't have the actual creation of the Append node split out into a
	 * separate make_xxx function.  This is because we want to run
	 * prepare_sort_from_pathkeys on it before we do so on the individual
	 * child plans, to make cross-checking the sort info easier.
	 */
	plan = makeNode(Append);
	plan->plan.targetlist = tlist;
	plan->plan.qual = NIL;
	plan->plan.lefttree = NULL;
	plan->plan.righttree = NULL;

	if (pathkeys != NIL)
	{
		/*
		 * Compute sort column info, and adjust the Append's tlist as needed.
		 * Because we pass adjust_tlist_in_place = true, we may ignore the
		 * function result; it must be the same plan node.  However, we then
		 * need to detect whether any tlist entries were added.
		 */
		(void) prepare_sort_from_pathkeys((Plan *) plan, pathkeys,
										  best_path->path.parent->relids,
										  NULL,
										  true,
										  &nodenumsortkeys,
										  &nodeSortColIdx,
										  &nodeSortOperators,
										  &nodeCollations,
										  &nodeNullsFirst);
		tlist_was_changed = (orig_tlist_length != list_length(plan->plan.targetlist));
	}

	/* Build the plan for each child */
	foreach(subpaths, best_path->subpaths)
	{
		Path	   *subpath = (Path *) lfirst(subpaths);
		Plan	   *subplan;

		/* Must insist that all children return the same tlist */
		subplan = create_plan_recurse(root, subpath, CP_EXACT_TLIST);

		/*
		 * For ordered Appends, we must insert a Sort node if subplan isn't
		 * sufficiently ordered.
		 */
		if (pathkeys != NIL)
		{
			int			numsortkeys;
			AttrNumber *sortColIdx;
			Oid		   *sortOperators;
			Oid		   *collations;
			bool	   *nullsFirst;

			/*
			 * Compute sort column info, and adjust subplan's tlist as needed.
			 * We must apply prepare_sort_from_pathkeys even to subplans that
			 * don't need an explicit sort, to make sure they are returning
			 * the same sort key columns the Append expects.
			 */
			subplan = prepare_sort_from_pathkeys(subplan, pathkeys,
												 subpath->parent->relids,
												 nodeSortColIdx,
												 false,
												 &numsortkeys,
												 &sortColIdx,
												 &sortOperators,
												 &collations,
												 &nullsFirst);

			/*
			 * Check that we got the same sort key information.  We just
			 * Assert that the sortops match, since those depend only on the
			 * pathkeys; but it seems like a good idea to check the sort
			 * column numbers explicitly, to ensure the tlists match up.
			 */
			Assert(numsortkeys == nodenumsortkeys);
			if (memcmp(sortColIdx, nodeSortColIdx,
					   numsortkeys * sizeof(AttrNumber)) != 0)
				elog(ERROR, "Append child's targetlist doesn't match Append");
			Assert(memcmp(sortOperators, nodeSortOperators,
						  numsortkeys * sizeof(Oid)) == 0);
			Assert(memcmp(collations, nodeCollations,
						  numsortkeys * sizeof(Oid)) == 0);
			Assert(memcmp(nullsFirst, nodeNullsFirst,
						  numsortkeys * sizeof(bool)) == 0);

			/* Now, insert a Sort node if subplan isn't sufficiently ordered */
			if (!pathkeys_contained_in(pathkeys, subpath->pathkeys))
			{
				Sort	   *sort = make_sort(subplan, numsortkeys,
											 sortColIdx, sortOperators,
											 collations, nullsFirst);

				label_sort_with_costsize(root, sort, best_path->limit_tuples);
				subplan = (Plan *) sort;
			}
		}

		subplans = lappend(subplans, subplan);
	}

	/*
	 * If any quals exist, they may be useful to perform further partition
	 * pruning during execution.  Gather information needed by the executor to
	 * do partition pruning.
	 */
	if (enable_partition_pruning &&
		rel->reloptkind == RELOPT_BASEREL &&
		best_path->partitioned_rels != NIL)
	{
		List	   *prunequal;

		prunequal = extract_actual_clauses(rel->baserestrictinfo, false);

		if (best_path->path.param_info)
		{
			List	   *prmquals = best_path->path.param_info->ppi_clauses;

			prmquals = extract_actual_clauses(prmquals, false);
			prmquals = (List *) replace_nestloop_params(root,
														(Node *) prmquals);

			prunequal = list_concat(prunequal, prmquals);
		}

		if (prunequal != NIL)
			partpruneinfo =
				make_partition_pruneinfo(root, rel,
										 best_path->subpaths,
										 best_path->partitioned_rels,
										 prunequal);
	}

	plan->appendplans = subplans;
	plan->first_partial_plan = best_path->first_partial_path;
	plan->part_prune_info = partpruneinfo;

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	/*
	 * If prepare_sort_from_pathkeys added sort columns, but we were told to
	 * produce either the exact tlist or a narrow tlist, we should get rid of
	 * the sort columns again.  We must inject a projection node to do so.
	 */
	if (tlist_was_changed && (flags & (CP_EXACT_TLIST | CP_SMALL_TLIST)))
	{
		tlist = list_truncate(list_copy(plan->plan.targetlist),
							  orig_tlist_length);
		return inject_projection_plan((Plan *) plan, tlist,
									  plan->plan.parallel_safe);
	}
	else
		return (Plan *) plan;
}

/*
 * create_merge_append_plan
 *	  Create a MergeAppend plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 *
 *	  Returns a Plan node.
 */
static Plan *
create_merge_append_plan(PlannerInfo *root, MergeAppendPath *best_path,
						 int flags)
{
	MergeAppend *node = makeNode(MergeAppend);
	Plan	   *plan = &node->plan;
	List	   *tlist = build_path_tlist(root, &best_path->path);
	int			orig_tlist_length = list_length(tlist);
	bool		tlist_was_changed;
	List	   *pathkeys = best_path->path.pathkeys;
	List	   *subplans = NIL;
	ListCell   *subpaths;
	RelOptInfo *rel = best_path->path.parent;
	PartitionPruneInfo *partpruneinfo = NULL;

	/*
	 * We don't have the actual creation of the MergeAppend node split out
	 * into a separate make_xxx function.  This is because we want to run
	 * prepare_sort_from_pathkeys on it before we do so on the individual
	 * child plans, to make cross-checking the sort info easier.
	 */
	copy_generic_path_info(plan, (Path *) best_path);
	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;

	/*
	 * Compute sort column info, and adjust MergeAppend's tlist as needed.
	 * Because we pass adjust_tlist_in_place = true, we may ignore the
	 * function result; it must be the same plan node.  However, we then need
	 * to detect whether any tlist entries were added.
	 */
	(void) prepare_sort_from_pathkeys(plan, pathkeys,
									  best_path->path.parent->relids,
									  NULL,
									  true,
									  &node->numCols,
									  &node->sortColIdx,
									  &node->sortOperators,
									  &node->collations,
									  &node->nullsFirst);
	tlist_was_changed = (orig_tlist_length != list_length(plan->targetlist));

	/*
	 * Now prepare the child plans.  We must apply prepare_sort_from_pathkeys
	 * even to subplans that don't need an explicit sort, to make sure they
	 * are returning the same sort key columns the MergeAppend expects.
	 */
	foreach(subpaths, best_path->subpaths)
	{
		Path	   *subpath = (Path *) lfirst(subpaths);
		Plan	   *subplan;
		int			numsortkeys;
		AttrNumber *sortColIdx;
		Oid		   *sortOperators;
		Oid		   *collations;
		bool	   *nullsFirst;

		/* Build the child plan */
		/* Must insist that all children return the same tlist */
		subplan = create_plan_recurse(root, subpath, CP_EXACT_TLIST);

		/* Compute sort column info, and adjust subplan's tlist as needed */
		subplan = prepare_sort_from_pathkeys(subplan, pathkeys,
											 subpath->parent->relids,
											 node->sortColIdx,
											 false,
											 &numsortkeys,
											 &sortColIdx,
											 &sortOperators,
											 &collations,
											 &nullsFirst);

		/*
		 * Check that we got the same sort key information.  We just Assert
		 * that the sortops match, since those depend only on the pathkeys;
		 * but it seems like a good idea to check the sort column numbers
		 * explicitly, to ensure the tlists really do match up.
		 */
		Assert(numsortkeys == node->numCols);
		if (memcmp(sortColIdx, node->sortColIdx,
				   numsortkeys * sizeof(AttrNumber)) != 0)
			elog(ERROR, "MergeAppend child's targetlist doesn't match MergeAppend");
		Assert(memcmp(sortOperators, node->sortOperators,
					  numsortkeys * sizeof(Oid)) == 0);
		Assert(memcmp(collations, node->collations,
					  numsortkeys * sizeof(Oid)) == 0);
		Assert(memcmp(nullsFirst, node->nullsFirst,
					  numsortkeys * sizeof(bool)) == 0);

		/* Now, insert a Sort node if subplan isn't sufficiently ordered */
		if (!pathkeys_contained_in(pathkeys, subpath->pathkeys))
		{
			Sort	   *sort = make_sort(subplan, numsortkeys,
										 sortColIdx, sortOperators,
										 collations, nullsFirst);

			label_sort_with_costsize(root, sort, best_path->limit_tuples);
			subplan = (Plan *) sort;
		}

		subplans = lappend(subplans, subplan);
	}

	/*
	 * If any quals exist, they may be useful to perform further partition
	 * pruning during execution.  Gather information needed by the executor to
	 * do partition pruning.
	 */
	if (enable_partition_pruning &&
		rel->reloptkind == RELOPT_BASEREL &&
		best_path->partitioned_rels != NIL)
	{
		List	   *prunequal;

		prunequal = extract_actual_clauses(rel->baserestrictinfo, false);

		if (best_path->path.param_info)
		{
			List	   *prmquals = best_path->path.param_info->ppi_clauses;

			prmquals = extract_actual_clauses(prmquals, false);
			prmquals = (List *) replace_nestloop_params(root,
														(Node *) prmquals);

			prunequal = list_concat(prunequal, prmquals);
		}

		if (prunequal != NIL)
			partpruneinfo = make_partition_pruneinfo(root, rel,
													 best_path->subpaths,
													 best_path->partitioned_rels,
													 prunequal);
	}

	node->mergeplans = subplans;
	node->part_prune_info = partpruneinfo;

	/*
	 * If prepare_sort_from_pathkeys added sort columns, but we were told to
	 * produce either the exact tlist or a narrow tlist, we should get rid of
	 * the sort columns again.  We must inject a projection node to do so.
	 */
	if (tlist_was_changed && (flags & (CP_EXACT_TLIST | CP_SMALL_TLIST)))
	{
		tlist = list_truncate(list_copy(plan->targetlist), orig_tlist_length);
		return inject_projection_plan(plan, tlist, plan->parallel_safe);
	}
	else
		return plan;
}

/*
 * create_group_result_plan
 *	  Create a Result plan for 'best_path'.
 *	  This is only used for degenerate grouping cases.
 *
 *	  Returns a Plan node.
 */
static Result *
create_group_result_plan(PlannerInfo *root, GroupResultPath *best_path)
{
	Result	   *plan;
	List	   *tlist;
	List	   *quals;

	tlist = build_path_tlist(root, &best_path->path);

	/* best_path->quals is just bare clauses */
	quals = order_qual_clauses(root, best_path->quals);

	plan = make_result(tlist, (Node *) quals, NULL);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_project_set_plan
 *	  Create a ProjectSet plan for 'best_path'.
 *
 *	  Returns a Plan node.
 */
static ProjectSet *
create_project_set_plan(PlannerInfo *root, ProjectSetPath *best_path)
{
	ProjectSet *plan;
	Plan	   *subplan;
	List	   *tlist;

	/* Since we intend to project, we don't need to constrain child tlist */
	subplan = create_plan_recurse(root, best_path->subpath, 0);

	tlist = build_path_tlist(root, &best_path->path);

	plan = make_project_set(tlist, subplan);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_material_plan - 创建物化(Material)计划节点
 *
 * 该函数负责为给定的MaterialPath创建对应的Material计划节点，
 * 并递归地为其子路径创建计划节点。物化操作用于缓存中间结果集，
 * 避免重复计算或多次扫描同一数据源，特别是在需要多次访问同一结果集的场景中
 * (如子查询、窗口函数或连接操作中)。
 *
 * 参数说明：
 *   root: 查询规划器的根节点，包含整个查询的规划信息
 *   best_path: 要转换为计划节点的MaterialPath路径
 *   flags: 控制计划创建行为的标志位
 *
 * 返回值：
 *   创建好的Material计划节点
 */
static Material *
create_material_plan(PlannerInfo *root, MaterialPath *best_path, int flags)
{
	Material   *plan;      /* 最终创建的物化计划节点 */
	Plan	   *subplan;   /* 子路径对应的计划节点 */

	/*
	 * 性能优化：由于物化操作会将整个结果集缓存到内存或磁盘中，
	 * 我们希望尽可能减少物化元组中的列数，以节省存储空间和IO开销。
	 * 因此，通过设置CP_SMALL_TLIST标志，请求子路径生成最小化的目标列表，禁止添加额外的列。
	 * 
	 * 注意：Material节点本身不执行投影操作(不能修改目标列表)，
	 * 所以目标列表的要求会直接传递给子路径。
	 */
	subplan = create_plan_recurse(root, best_path->subpath,
					  flags | CP_SMALL_TLIST);

	/* 创建物化计划节点，将子计划作为其输入 */
	plan = make_material(subplan);

	/* 从路径节点复制通用的路径信息到计划节点，包括成本、行数估计等 */
	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}


/*
 * create_unique_plan
 *	  为 'best_path' 创建一个 Unique 计划节点，并递归处理其子路径。
 *
 *	  返回一个 Plan 节点。
 */
static Plan *
create_unique_plan(PlannerInfo *root, UniquePath *best_path, int flags)
{
	Plan	   *plan;
	Plan	   *subplan;
	List	   *in_operators;
	List	   *uniq_exprs;
	List	   *newtlist;
	int			nextresno;
	bool		newitems;
	int			numGroupCols;
	AttrNumber *groupColIdx;
	Oid		   *groupCollations;
	int			groupColPos;
	ListCell   *l;

	/* Unique 节点不做投影，tlist 要求直接传递 */
	subplan = create_plan_recurse(root, best_path->subpath, flags);

	/* 如果不需要实际去重，直接返回子计划 */
	if (best_path->umethod == UNIQUE_PATH_NOOP)
		return subplan;

	/*
	 * 子计划的 tlist 只包含本层和上层需要的 Vars。我们需要对这些变量做唯一化处理，
	 * 但唯一化的表达式可能是这些变量的组合表达式，因此需要将这些表达式加入到 tlist。
	 *
	 * 如果子计划是简单扫描，可能有 "物理" tlist。如果需要排序，则应缩减为常规 tlist，
	 * 以避免排序多余数据。对于哈希去重，如果需要添加表达式，则运行时也需要投影，
	 * 所以也可以去掉不需要的项。因此 newtlist 从 build_path_tlist() 开始，
	 * 只有排序或需要添加表达式时才安装到 subplan。
	 */
	in_operators = best_path->in_operators;
	uniq_exprs = best_path->uniq_exprs;

	/* 初始化新的 tlist，仅包含“必须”的变量 */
	newtlist = build_path_tlist(root, &best_path->path);
	nextresno = list_length(newtlist) + 1;
	newitems = false;

	/* 将唯一化表达式加入 tlist */
	foreach(l, uniq_exprs)
	{
		Expr	   *uniqexpr = lfirst(l);
		TargetEntry *tle;

		tle = tlist_member(uniqexpr, newtlist);
		if (!tle)
		{
			tle = makeTargetEntry((Expr *) uniqexpr,
								  nextresno,
								  NULL,
								  false);
			newtlist = lappend(newtlist, tle);
			nextresno++;
			newitems = true;
		}
	}

	/* 如果需要插入 Result 节点，则用 change_plan_targetlist 包装 */
	if (newitems || best_path->umethod == UNIQUE_PATH_SORT)
		subplan = change_plan_targetlist(subplan, newtlist,
										 best_path->path.parallel_safe);

	/*
	 * 构建分组控制信息，指示哪些输出列需要分组。不能和上面循环合并，
	 * 因为此时才确定最终使用的 tlist。
	 */
	newtlist = subplan->targetlist;
	numGroupCols = list_length(uniq_exprs);
	groupColIdx = (AttrNumber *) palloc(numGroupCols * sizeof(AttrNumber));
	groupCollations = (Oid *) palloc(numGroupCols * sizeof(Oid));

	groupColPos = 0;
	foreach(l, uniq_exprs)
	{
		Expr	   *uniqexpr = lfirst(l);
		TargetEntry *tle;

		tle = tlist_member(uniqexpr, newtlist);
		if (!tle)				/* 不应该发生 */
			elog(ERROR, "failed to find unique expression in subplan tlist");
		groupColIdx[groupColPos] = tle->resno;
		groupCollations[groupColPos] = exprCollation((Node *) tle->expr);
		groupColPos++;
	}

	if (best_path->umethod == UNIQUE_PATH_HASH)
	{
		Oid		   *groupOperators;

		/*
		 * 获取 Agg 节点需要的哈希等值操作符。通常与 IN 操作符一致，
		 * 但如果是跨类型操作符，则等值操作符为 IN 操作符右侧类型的等值操作符。
		 */
		groupOperators = (Oid *) palloc(numGroupCols * sizeof(Oid));
		groupColPos = 0;
		foreach(l, in_operators)
		{
			Oid			in_oper = lfirst_oid(l);
			Oid			eq_oper;

			if (!get_compatible_hash_operators(in_oper, NULL, &eq_oper))
				elog(ERROR, "could not find compatible hash operator for operator %u",
					 in_oper);
			groupOperators[groupColPos++] = eq_oper;
		}

		/*
		 * Agg 节点会做投影，因此可以给它最小输出 tlist，不需要加到 subplan tlist 的额外项。
		 */
		plan = (Plan *) make_agg(build_path_tlist(root, &best_path->path),
								 NIL,
								 AGG_HASHED,
								 AGGSPLIT_SIMPLE,
								 numGroupCols,
								 groupColIdx,
								 groupOperators,
								 groupCollations,
								 NIL,
								 NIL,
								 best_path->path.rows,
								 subplan);
	}
	else
	{
		List	   *sortList = NIL;
		Sort	   *sort;

		/* 构建 ORDER BY 列表，使输入排序兼容 */
		groupColPos = 0;
		foreach(l, in_operators)
		{
			Oid			in_oper = lfirst_oid(l);
			Oid			sortop;
			Oid			eqop;
			TargetEntry *tle;
			SortGroupClause *sortcl;

			sortop = get_ordering_op_for_equality_op(in_oper, false);
			if (!OidIsValid(sortop))	/* 不应该发生 */
				elog(ERROR, "could not find ordering operator for equality operator %u",
					 in_oper);

			/*
			 * Unique 节点需要等值操作符。通常与 IN 操作符一致，
			 * 但如果是跨类型操作符，则等值操作符为排序操作符的等值操作符。
			 */
			eqop = get_equality_op_for_ordering_op(sortop, NULL);
			if (!OidIsValid(eqop))	/* 不应该发生 */
				elog(ERROR, "could not find equality operator for ordering operator %u",
					 sortop);

			tle = get_tle_by_resno(subplan->targetlist,
								   groupColIdx[groupColPos]);
			Assert(tle != NULL);

			sortcl = makeNode(SortGroupClause);
			sortcl->tleSortGroupRef = assignSortGroupRef(tle,
														 subplan->targetlist);
			sortcl->eqop = eqop;
			sortcl->sortop = sortop;
			sortcl->nulls_first = false;
			sortcl->hashable = false;	/* 无需准确 */
			sortList = lappend(sortList, sortcl);
			groupColPos++;
		}
		sort = make_sort_from_sortclauses(sortList, subplan);
		label_sort_with_costsize(root, sort, -1.0);
		plan = (Plan *) make_unique_from_sortclauses((Plan *) sort, sortList);
	}

	/* 从 Path 复制成本信息到 Plan */
	copy_generic_path_info(plan, &best_path->path);

	return plan;
}

/*
 * create_gather_plan
 *
 *	  Create a Gather plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static Gather *
create_gather_plan(PlannerInfo *root, GatherPath *best_path)
{
	Gather	   *gather_plan;
	Plan	   *subplan;
	List	   *tlist;

	/*
	 * Although the Gather node can project, we prefer to push down such work
	 * to its child node, so demand an exact tlist from the child.
	 */
	subplan = create_plan_recurse(root, best_path->subpath, CP_EXACT_TLIST);

	tlist = build_path_tlist(root, &best_path->path);

	gather_plan = make_gather(tlist,
							  NIL,
							  best_path->num_workers,
							  assign_special_exec_param(root),
							  best_path->single_copy,
							  subplan);

	copy_generic_path_info(&gather_plan->plan, &best_path->path);

	/* use parallel mode for parallel plans. */
	root->glob->parallelModeNeeded = true;

	return gather_plan;
}

/*
 * create_gather_merge_plan
 *
 *	  Create a Gather Merge plan for 'best_path' and (recursively)
 *	  plans for its subpaths.
 */
static GatherMerge *
create_gather_merge_plan(PlannerInfo *root, GatherMergePath *best_path)
{
	GatherMerge *gm_plan;
	Plan	   *subplan;
	List	   *pathkeys = best_path->path.pathkeys;
	List	   *tlist = build_path_tlist(root, &best_path->path);

	/* As with Gather, it's best to project away columns in the workers. */
	subplan = create_plan_recurse(root, best_path->subpath, CP_EXACT_TLIST);

	/* Create a shell for a GatherMerge plan. */
	gm_plan = makeNode(GatherMerge);
	gm_plan->plan.targetlist = tlist;
	gm_plan->num_workers = best_path->num_workers;
	copy_generic_path_info(&gm_plan->plan, &best_path->path);

	/* Assign the rescan Param. */
	gm_plan->rescan_param = assign_special_exec_param(root);

	/* Gather Merge is pointless with no pathkeys; use Gather instead. */
	Assert(pathkeys != NIL);

	/* Compute sort column info, and adjust subplan's tlist as needed */
	subplan = prepare_sort_from_pathkeys(subplan, pathkeys,
										 best_path->subpath->parent->relids,
										 gm_plan->sortColIdx,
										 false,
										 &gm_plan->numCols,
										 &gm_plan->sortColIdx,
										 &gm_plan->sortOperators,
										 &gm_plan->collations,
										 &gm_plan->nullsFirst);


	/* Now, insert a Sort node if subplan isn't sufficiently ordered */
	if (!pathkeys_contained_in(pathkeys, best_path->subpath->pathkeys))
		subplan = (Plan *) make_sort(subplan, gm_plan->numCols,
									 gm_plan->sortColIdx,
									 gm_plan->sortOperators,
									 gm_plan->collations,
									 gm_plan->nullsFirst);

	/* Now insert the subplan under GatherMerge. */
	gm_plan->plan.lefttree = subplan;

	/* use parallel mode for parallel plans. */
	root->glob->parallelModeNeeded = true;

	return gm_plan;
}

/*
 * create_projection_plan
 *
 *	  Create a plan tree to do a projection step and (recursively) plans
 *	  for its subpaths.  We may need a Result node for the projection,
 *	  but sometimes we can just let the subplan do the work.
 */
static Plan *
create_projection_plan(PlannerInfo *root, ProjectionPath *best_path, int flags)
{
	Plan	   *plan;
	Plan	   *subplan;
	List	   *tlist;
	bool		needs_result_node = false;

	/*
	 * Convert our subpath to a Plan and determine whether we need a Result
	 * node.
	 *
	 * In most cases where we don't need to project, create_projection_path
	 * will have set dummypp, but not always.  First, some createplan.c
	 * routines change the tlists of their nodes.  (An example is that
	 * create_merge_append_plan might add resjunk sort columns to a
	 * MergeAppend.)  Second, create_projection_path has no way of knowing
	 * what path node will be placed on top of the projection path and
	 * therefore can't predict whether it will require an exact tlist. For
	 * both of these reasons, we have to recheck here.
	 */
	if (use_physical_tlist(root, &best_path->path, flags))
	{
		/*
		 * Our caller doesn't really care what tlist we return, so we don't
		 * actually need to project.  However, we may still need to ensure
		 * proper sortgroupref labels, if the caller cares about those.
		 */
		subplan = create_plan_recurse(root, best_path->subpath, 0);
		tlist = subplan->targetlist;
		if (flags & CP_LABEL_TLIST)
			apply_pathtarget_labeling_to_tlist(tlist,
											   best_path->path.pathtarget);
	}
	else if (is_projection_capable_path(best_path->subpath))
	{
		/*
		 * Our caller requires that we return the exact tlist, but no separate
		 * result node is needed because the subpath is projection-capable.
		 * Tell create_plan_recurse that we're going to ignore the tlist it
		 * produces.
		 */
		subplan = create_plan_recurse(root, best_path->subpath,
									  CP_IGNORE_TLIST);
		Assert(is_projection_capable_plan(subplan));
		tlist = build_path_tlist(root, &best_path->path);
	}
	else
	{
		/*
		 * It looks like we need a result node, unless by good fortune the
		 * requested tlist is exactly the one the child wants to produce.
		 */
		subplan = create_plan_recurse(root, best_path->subpath, 0);
		tlist = build_path_tlist(root, &best_path->path);
		needs_result_node = !tlist_same_exprs(tlist, subplan->targetlist);
	}

	/*
	 * If we make a different decision about whether to include a Result node
	 * than create_projection_path did, we'll have made slightly wrong cost
	 * estimates; but label the plan with the cost estimates we actually used,
	 * not "corrected" ones.  (XXX this could be cleaned up if we moved more
	 * of the sortcolumn setup logic into Path creation, but that would add
	 * expense to creating Paths we might end up not using.)
	 */
	if (!needs_result_node)
	{
		/* Don't need a separate Result, just assign tlist to subplan */
		plan = subplan;
		plan->targetlist = tlist;

		/* Label plan with the estimated costs we actually used */
		plan->startup_cost = best_path->path.startup_cost;
		plan->total_cost = best_path->path.total_cost;
		plan->plan_rows = best_path->path.rows;
		plan->plan_width = best_path->path.pathtarget->width;
		plan->parallel_safe = best_path->path.parallel_safe;
		/* ... but don't change subplan's parallel_aware flag */
	}
	else
	{
		/* We need a Result node */
		plan = (Plan *) make_result(tlist, NULL, subplan);

		copy_generic_path_info(plan, (Path *) best_path);
	}

	return plan;
}

/*
 * inject_projection_plan
 *	  Insert a Result node to do a projection step.
 *
 * This is used in a few places where we decide on-the-fly that we need a
 * projection step as part of the tree generated for some Path node.
 * We should try to get rid of this in favor of doing it more honestly.
 *
 * One reason it's ugly is we have to be told the right parallel_safe marking
 * to apply (since the tlist might be unsafe even if the child plan is safe).
 */
static Plan *
inject_projection_plan(Plan *subplan, List *tlist, bool parallel_safe)
{
	Plan	   *plan;

	plan = (Plan *) make_result(tlist, NULL, subplan);

	/*
	 * In principle, we should charge tlist eval cost plus cpu_per_tuple per
	 * row for the Result node.  But the former has probably been factored in
	 * already and the latter was not accounted for during Path construction,
	 * so being formally correct might just make the EXPLAIN output look less
	 * consistent not more so.  Hence, just copy the subplan's cost.
	 */
	copy_plan_costsize(plan, subplan);
	plan->parallel_safe = parallel_safe;

	return plan;
}

/*
 * change_plan_targetlist
 *	  Externally available wrapper for inject_projection_plan.
 *
 * This is meant for use by FDW plan-generation functions, which might
 * want to adjust the tlist computed by some subplan tree.  In general,
 * a Result node is needed to compute the new tlist, but we can optimize
 * some cases.
 *
 * In most cases, tlist_parallel_safe can just be passed as the parallel_safe
 * flag of the FDW's own Path node.
 */
Plan *
change_plan_targetlist(Plan *subplan, List *tlist, bool tlist_parallel_safe)
{
	/*
	 * If the top plan node can't do projections and its existing target list
	 * isn't already what we need, we need to add a Result node to help it
	 * along.
	 */
	if (!is_projection_capable_plan(subplan) &&
		!tlist_same_exprs(tlist, subplan->targetlist))
		subplan = inject_projection_plan(subplan, tlist,
										 subplan->parallel_safe &&
										 tlist_parallel_safe);
	else
	{
		/* Else we can just replace the plan node's tlist */
		subplan->targetlist = tlist;
		subplan->parallel_safe &= tlist_parallel_safe;
	}
	return subplan;
}

/*
 * create_sort_plan
 *
 *	  Create a Sort plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static Sort *
create_sort_plan(PlannerInfo *root, SortPath *best_path, int flags)
{
	Sort	   *plan;
	Plan	   *subplan;

	/*
	 * We don't want any excess columns in the sorted tuples, so request a
	 * smaller tlist.  Otherwise, since Sort doesn't project, tlist
	 * requirements pass through.
	 */
	subplan = create_plan_recurse(root, best_path->subpath,
								  flags | CP_SMALL_TLIST);

	/*
	 * make_sort_from_pathkeys() indirectly calls find_ec_member_for_tle(),
	 * which will ignore any child EC members that don't belong to the given
	 * relids. Thus, if this sort path is based on a child relation, we must
	 * pass its relids.
	 */
	plan = make_sort_from_pathkeys(subplan, best_path->path.pathkeys,
								   IS_OTHER_REL(best_path->subpath->parent) ?
								   best_path->path.parent->relids : NULL);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_group_plan
 *
 *	  Create a Group plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static Group *
create_group_plan(PlannerInfo *root, GroupPath *best_path)
{
	Group	   *plan;
	Plan	   *subplan;
	List	   *tlist;
	List	   *quals;

	/*
	 * Group can project, so no need to be terribly picky about child tlist,
	 * but we do need grouping columns to be available
	 */
	subplan = create_plan_recurse(root, best_path->subpath, CP_LABEL_TLIST);

	tlist = build_path_tlist(root, &best_path->path);

	quals = order_qual_clauses(root, best_path->qual);

	plan = make_group(tlist,
					  quals,
					  list_length(best_path->groupClause),
					  extract_grouping_cols(best_path->groupClause,
											subplan->targetlist),
					  extract_grouping_ops(best_path->groupClause),
					  extract_grouping_collations(best_path->groupClause,
												  subplan->targetlist),
					  subplan);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_upper_unique_plan
 *
 *	  Create a Unique plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static Unique *
create_upper_unique_plan(PlannerInfo *root, UpperUniquePath *best_path, int flags)
{
	Unique	   *plan;
	Plan	   *subplan;

	/*
	 * Unique doesn't project, so tlist requirements pass through; moreover we
	 * need grouping columns to be labeled.
	 */
	subplan = create_plan_recurse(root, best_path->subpath,
								  flags | CP_LABEL_TLIST);

	plan = make_unique_from_pathkeys(subplan,
									 best_path->path.pathkeys,
									 best_path->numkeys);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_agg_plan
 *	  为 'best_path' 创建一个 Agg 计划节点，并递归处理其子路径。
 *
 *	  返回一个 Agg 节点。
 */
static Agg *
create_agg_plan(PlannerInfo *root, AggPath *best_path)
{
	Agg		   *plan;
	Plan	   *subplan;
	List	   *tlist;
	List	   *quals;

	/*
	 * Agg 节点可以做投影，因此对子计划的 tlist 不需要严格要求，
	 * 但需要保证分组列可用。
	 */
	subplan = create_plan_recurse(root, best_path->subpath, CP_LABEL_TLIST);

	/* 构建目标列列表 */
	tlist = build_path_tlist(root, &best_path->path);

	/* 对聚合条件进行排序优化 */
	quals = order_qual_clauses(root, best_path->qual);

	/* 构建 Agg 计划节点 */
	plan = make_agg(tlist, quals,
					best_path->aggstrategy,
					best_path->aggsplit,
					list_length(best_path->groupClause),
					extract_grouping_cols(best_path->groupClause,
										  subplan->targetlist),
					extract_grouping_ops(best_path->groupClause),
					extract_grouping_collations(best_path->groupClause,
												subplan->targetlist),
					NIL,
					NIL,
					best_path->numGroups,
					subplan);

	/* 从 Path 复制成本信息到 Plan */
	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * Given a groupclause for a collection of grouping sets, produce the
 * corresponding groupColIdx.
 *
 * root->grouping_map maps the tleSortGroupRef to the actual column position in
 * the input tuple. So we get the ref from the entries in the groupclause and
 * look them up there.
 */
static AttrNumber *
remap_groupColIdx(PlannerInfo *root, List *groupClause)
{
	AttrNumber *grouping_map = root->grouping_map;
	AttrNumber *new_grpColIdx;
	ListCell   *lc;
	int			i;

	Assert(grouping_map);

	new_grpColIdx = palloc0(sizeof(AttrNumber) * list_length(groupClause));

	i = 0;
	foreach(lc, groupClause)
	{
		SortGroupClause *clause = lfirst(lc);

		new_grpColIdx[i++] = grouping_map[clause->tleSortGroupRef];
	}

	return new_grpColIdx;
}

/*
 * create_groupingsets_plan
 *	  Create a plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 *
 *	  What we emit is an Agg plan with some vestigial Agg and Sort nodes
 *	  hanging off the side.  The top Agg implements the last grouping set
 *	  specified in the GroupingSetsPath, and any additional grouping sets
 *	  each give rise to a subsidiary Agg and Sort node in the top Agg's
 *	  "chain" list.  These nodes don't participate in the plan directly,
 *	  but they are a convenient way to represent the required data for
 *	  the extra steps.
 *
 *	  Returns a Plan node.
 */
static Plan *
create_groupingsets_plan(PlannerInfo *root, GroupingSetsPath *best_path)
{
	Agg		   *plan;
	Plan	   *subplan;
	List	   *rollups = best_path->rollups;
	AttrNumber *grouping_map;
	int			maxref;
	List	   *chain;
	ListCell   *lc;

	/* Shouldn't get here without grouping sets */
	Assert(root->parse->groupingSets);
	Assert(rollups != NIL);

	/*
	 * Agg can project, so no need to be terribly picky about child tlist, but
	 * we do need grouping columns to be available
	 */
	subplan = create_plan_recurse(root, best_path->subpath, CP_LABEL_TLIST);

	/*
	 * Compute the mapping from tleSortGroupRef to column index in the child's
	 * tlist.  First, identify max SortGroupRef in groupClause, for array
	 * sizing.
	 */
	maxref = 0;
	foreach(lc, root->parse->groupClause)
	{
		SortGroupClause *gc = (SortGroupClause *) lfirst(lc);

		if (gc->tleSortGroupRef > maxref)
			maxref = gc->tleSortGroupRef;
	}

	grouping_map = (AttrNumber *) palloc0((maxref + 1) * sizeof(AttrNumber));

	/* Now look up the column numbers in the child's tlist */
	foreach(lc, root->parse->groupClause)
	{
		SortGroupClause *gc = (SortGroupClause *) lfirst(lc);
		TargetEntry *tle = get_sortgroupclause_tle(gc, subplan->targetlist);

		grouping_map[gc->tleSortGroupRef] = tle->resno;
	}

	/*
	 * During setrefs.c, we'll need the grouping_map to fix up the cols lists
	 * in GroupingFunc nodes.  Save it for setrefs.c to use.
	 *
	 * This doesn't work if we're in an inheritance subtree (see notes in
	 * create_modifytable_plan).  Fortunately we can't be because there would
	 * never be grouping in an UPDATE/DELETE; but let's Assert that.
	 */
	Assert(root->inhTargetKind == INHKIND_NONE);
	Assert(root->grouping_map == NULL);
	root->grouping_map = grouping_map;

	/*
	 * Generate the side nodes that describe the other sort and group
	 * operations besides the top one.  Note that we don't worry about putting
	 * accurate cost estimates in the side nodes; only the topmost Agg node's
	 * costs will be shown by EXPLAIN.
	 */
	chain = NIL;
	if (list_length(rollups) > 1)
	{
		ListCell   *lc2 = lnext(list_head(rollups));
		bool		is_first_sort = ((RollupData *) linitial(rollups))->is_hashed;

		for_each_cell(lc, lc2)
		{
			RollupData *rollup = lfirst(lc);
			AttrNumber *new_grpColIdx;
			Plan	   *sort_plan = NULL;
			Plan	   *agg_plan;
			AggStrategy strat;

			new_grpColIdx = remap_groupColIdx(root, rollup->groupClause);

			if (!rollup->is_hashed && !is_first_sort)
			{
				sort_plan = (Plan *)
					make_sort_from_groupcols(rollup->groupClause,
											 new_grpColIdx,
											 subplan);
			}

			if (!rollup->is_hashed)
				is_first_sort = false;

			if (rollup->is_hashed)
				strat = AGG_HASHED;
			else if (list_length(linitial(rollup->gsets)) == 0)
				strat = AGG_PLAIN;
			else
				strat = AGG_SORTED;

			agg_plan = (Plan *) make_agg(NIL,
										 NIL,
										 strat,
										 AGGSPLIT_SIMPLE,
										 list_length((List *) linitial(rollup->gsets)),
										 new_grpColIdx,
										 extract_grouping_ops(rollup->groupClause),
										 extract_grouping_collations(rollup->groupClause, subplan->targetlist),
										 rollup->gsets,
										 NIL,
										 rollup->numGroups,
										 sort_plan);

			/*
			 * Remove stuff we don't need to avoid bloating debug output.
			 */
			if (sort_plan)
			{
				sort_plan->targetlist = NIL;
				sort_plan->lefttree = NULL;
			}

			chain = lappend(chain, agg_plan);
		}
	}

	/*
	 * Now make the real Agg node
	 */
	{
		RollupData *rollup = linitial(rollups);
		AttrNumber *top_grpColIdx;
		int			numGroupCols;

		top_grpColIdx = remap_groupColIdx(root, rollup->groupClause);

		numGroupCols = list_length((List *) linitial(rollup->gsets));

		plan = make_agg(build_path_tlist(root, &best_path->path),
						best_path->qual,
						best_path->aggstrategy,
						AGGSPLIT_SIMPLE,
						numGroupCols,
						top_grpColIdx,
						extract_grouping_ops(rollup->groupClause),
						extract_grouping_collations(rollup->groupClause, subplan->targetlist),
						rollup->gsets,
						chain,
						rollup->numGroups,
						subplan);

		/* Copy cost data from Path to Plan */
		copy_generic_path_info(&plan->plan, &best_path->path);
	}

	return (Plan *) plan;
}

/*
 * create_minmaxagg_plan
 *
 *	  为 'best_path' 创建一个 Result 计划节点，并递归处理其子路径。
 *	  该函数用于处理 MIN/MAX 聚合优化路径。
 */
static Result *
create_minmaxagg_plan(PlannerInfo *root, MinMaxAggPath *best_path)
{
	Result	   *plan;
	List	   *tlist;
	ListCell   *lc;

	/* 为每个聚合的子查询准备一个 InitPlan。 */
	foreach(lc, best_path->mmaggregates)
	{
		MinMaxAggInfo *mminfo = (MinMaxAggInfo *) lfirst(lc);
		PlannerInfo *subroot = mminfo->subroot;
		Query	   *subparse = subroot->parse;
		Plan	   *plan;

		/*
		 * 生成子查询的计划。我们已经有了 Path，但需要将其转换为 Plan，并在其上方添加 LIMIT 节点。
		 * 由于进入了不同的 planner 上下文（subroot），需要递归调用 create_plan 而不是 create_plan_recurse。
		 */
		plan = create_plan(subroot, mminfo->path);

		plan = (Plan *) make_limit(plan,
								   subparse->limitOffset,
								   subparse->limitCount);

		/* 必须为 Limit 节点应用正确的成本和宽度信息 */
		plan->startup_cost = mminfo->path->startup_cost;
		plan->total_cost = mminfo->pathcost;
		plan->plan_rows = 1;
		plan->plan_width = mminfo->path->pathtarget->width;
		plan->parallel_aware = false;
		plan->parallel_safe = mminfo->path->parallel_safe;

		/* 将该计划转换为外层查询的 InitPlan。 */
		SS_make_initplan_from_plan(root, subroot, plan, mminfo->param);
	}

	/* 生成输出计划 —— 基本上只是一个 Result 节点 */
	tlist = build_path_tlist(root, &best_path->path);

	plan = make_result(tlist, (Node *) best_path->quals, NULL);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	/*
	 * 在 setrefs.c 阶段，需要将对 Agg 节点的引用替换为 InitPlan 的输出参数。
	 * （不能仅在 MinMaxAgg 节点本地替换，因为上层路径节点也可能有 Agg 引用。）
	 * 保存 mmaggregates 列表以便 setrefs.c 处理。
	 *
	 * 如果处于继承子树中则不适用（见 create_modifytable_plan 的注释）。
	 * 幸运的是，UPDATE/DELETE 不会有聚合，因此这里可以断言。
	 */
	Assert(root->inhTargetKind == INHKIND_NONE);
	Assert(root->minmax_aggs == NIL);
	root->minmax_aggs = best_path->mmaggregates;

	return plan;
}

/*
 * create_windowagg_plan
 *
 *	  Create a WindowAgg plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static WindowAgg *
create_windowagg_plan(PlannerInfo *root, WindowAggPath *best_path)
{
	WindowAgg  *plan;
	WindowClause *wc = best_path->winclause;
	int			numPart = list_length(wc->partitionClause);
	int			numOrder = list_length(wc->orderClause);
	Plan	   *subplan;
	List	   *tlist;
	int			partNumCols;
	AttrNumber *partColIdx;
	Oid		   *partOperators;
	Oid		   *partCollations;
	int			ordNumCols;
	AttrNumber *ordColIdx;
	Oid		   *ordOperators;
	Oid		   *ordCollations;
	ListCell   *lc;

	/*
	 * Choice of tlist here is motivated by the fact that WindowAgg will be
	 * storing the input rows of window frames in a tuplestore; it therefore
	 * behooves us to request a small tlist to avoid wasting space. We do of
	 * course need grouping columns to be available.
	 */
	subplan = create_plan_recurse(root, best_path->subpath,
								  CP_LABEL_TLIST | CP_SMALL_TLIST);

	tlist = build_path_tlist(root, &best_path->path);

	/*
	 * Convert SortGroupClause lists into arrays of attr indexes and equality
	 * operators, as wanted by executor.  (Note: in principle, it's possible
	 * to drop some of the sort columns, if they were proved redundant by
	 * pathkey logic.  However, it doesn't seem worth going out of our way to
	 * optimize such cases.  In any case, we must *not* remove the ordering
	 * column for RANGE OFFSET cases, as the executor needs that for in_range
	 * tests even if it's known to be equal to some partitioning column.)
	 */
	partColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numPart);
	partOperators = (Oid *) palloc(sizeof(Oid) * numPart);
	partCollations = (Oid *) palloc(sizeof(Oid) * numPart);

	partNumCols = 0;
	foreach(lc, wc->partitionClause)
	{
		SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
		TargetEntry *tle = get_sortgroupclause_tle(sgc, subplan->targetlist);

		Assert(OidIsValid(sgc->eqop));
		partColIdx[partNumCols] = tle->resno;
		partOperators[partNumCols] = sgc->eqop;
		partCollations[partNumCols] = exprCollation((Node *) tle->expr);
		partNumCols++;
	}

	ordColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numOrder);
	ordOperators = (Oid *) palloc(sizeof(Oid) * numOrder);
	ordCollations = (Oid *) palloc(sizeof(Oid) * numOrder);

	ordNumCols = 0;
	foreach(lc, wc->orderClause)
	{
		SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
		TargetEntry *tle = get_sortgroupclause_tle(sgc, subplan->targetlist);

		Assert(OidIsValid(sgc->eqop));
		ordColIdx[ordNumCols] = tle->resno;
		ordOperators[ordNumCols] = sgc->eqop;
		ordCollations[ordNumCols] = exprCollation((Node *) tle->expr);
		ordNumCols++;
	}

	/* And finally we can make the WindowAgg node */
	plan = make_windowagg(tlist,
						  wc->winref,
						  partNumCols,
						  partColIdx,
						  partOperators,
						  partCollations,
						  ordNumCols,
						  ordColIdx,
						  ordOperators,
						  ordCollations,
						  wc->frameOptions,
						  wc->startOffset,
						  wc->endOffset,
						  wc->startInRangeFunc,
						  wc->endInRangeFunc,
						  wc->inRangeColl,
						  wc->inRangeAsc,
						  wc->inRangeNullsFirst,
						  subplan);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_setop_plan
 *
 *	  Create a SetOp plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static SetOp *
create_setop_plan(PlannerInfo *root, SetOpPath *best_path, int flags)
{
	SetOp	   *plan;
	Plan	   *subplan;
	long		numGroups;

	/*
	 * SetOp doesn't project, so tlist requirements pass through; moreover we
	 * need grouping columns to be labeled.
	 */
	subplan = create_plan_recurse(root, best_path->subpath,
								  flags | CP_LABEL_TLIST);

	/* Convert numGroups to long int --- but 'ware overflow! */
	numGroups = (long) Min(best_path->numGroups, (double) LONG_MAX);

	plan = make_setop(best_path->cmd,
					  best_path->strategy,
					  subplan,
					  best_path->distinctList,
					  best_path->flagColIdx,
					  best_path->firstFlag,
					  numGroups);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_recursiveunion_plan
 *
 *	  Create a RecursiveUnion plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static RecursiveUnion *
create_recursiveunion_plan(PlannerInfo *root, RecursiveUnionPath *best_path)
{
	RecursiveUnion *plan;
	Plan	   *leftplan;
	Plan	   *rightplan;
	List	   *tlist;
	long		numGroups;

	/* Need both children to produce same tlist, so force it */
	leftplan = create_plan_recurse(root, best_path->leftpath, CP_EXACT_TLIST);
	rightplan = create_plan_recurse(root, best_path->rightpath, CP_EXACT_TLIST);

	tlist = build_path_tlist(root, &best_path->path);

	/* Convert numGroups to long int --- but 'ware overflow! */
	numGroups = (long) Min(best_path->numGroups, (double) LONG_MAX);

	plan = make_recursive_union(tlist,
								leftplan,
								rightplan,
								best_path->wtParam,
								best_path->distinctList,
								numGroups);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_lockrows_plan
 *
 *	  Create a LockRows plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static LockRows *
create_lockrows_plan(PlannerInfo *root, LockRowsPath *best_path,
					 int flags)
{
	LockRows   *plan;
	Plan	   *subplan;

	/* LockRows doesn't project, so tlist requirements pass through */
	subplan = create_plan_recurse(root, best_path->subpath, flags);

	plan = make_lockrows(subplan, best_path->rowMarks, best_path->epqParam);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_modifytable_plan
 *	  Create a ModifyTable plan for 'best_path'.
 *
 *	  Returns a Plan node.
 */
static ModifyTable *
create_modifytable_plan(PlannerInfo *root, ModifyTablePath *best_path)
{
	ModifyTable *plan;
	List	   *subplans = NIL;
	ListCell   *subpaths,
			   *subroots;

	/* Build the plan for each input path */
	forboth(subpaths, best_path->subpaths,
			subroots, best_path->subroots)
	{
		Path	   *subpath = (Path *) lfirst(subpaths);
		PlannerInfo *subroot = (PlannerInfo *) lfirst(subroots);
		Plan	   *subplan;

		/*
		 * In an inherited UPDATE/DELETE, reference the per-child modified
		 * subroot while creating Plans from Paths for the child rel.  This is
		 * a kluge, but otherwise it's too hard to ensure that Plan creation
		 * functions (particularly in FDWs) don't depend on the contents of
		 * "root" matching what they saw at Path creation time.  The main
		 * downside is that creation functions for Plans that might appear
		 * below a ModifyTable cannot expect to modify the contents of "root"
		 * and have it "stick" for subsequent processing such as setrefs.c.
		 * That's not great, but it seems better than the alternative.
		 */
		subplan = create_plan_recurse(subroot, subpath, CP_EXACT_TLIST);

		/* Transfer resname/resjunk labeling, too, to keep executor happy */
		apply_tlist_labeling(subplan->targetlist, subroot->processed_tlist);

		subplans = lappend(subplans, subplan);
	}

	plan = make_modifytable(root,
							best_path->operation,
							best_path->canSetTag,
							best_path->nominalRelation,
							best_path->rootRelation,
							best_path->partColsUpdated,
							best_path->resultRelations,
							subplans,
							best_path->subroots,
							best_path->withCheckOptionLists,
							best_path->returningLists,
							best_path->rowMarks,
							best_path->onconflict,
							best_path->epqParam);

	copy_generic_path_info(&plan->plan, &best_path->path);

	return plan;
}

/*
 * create_limit_plan
 *
 *	  Create a Limit plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static Limit *
create_limit_plan(PlannerInfo *root, LimitPath *best_path, int flags)
{
	Limit	   *plan;
	Plan	   *subplan;

	/* Limit doesn't project, so tlist requirements pass through */
	subplan = create_plan_recurse(root, best_path->subpath, flags);

	plan = make_limit(subplan,
					  best_path->limitOffset,
					  best_path->limitCount);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}


/*****************************************************************************
 *
 *	BASE-RELATION SCAN METHODS
 *
 *****************************************************************************/

/*
 * create_seqscan_plan
 *	 为 'best_path' 扫描的基础关系生成一个顺序扫描计划节点，
 *	 使用限制条件 'scan_clauses' 和目标列列表 'tlist'。
 */
static SeqScan *
create_seqscan_plan(PlannerInfo *root, Path *best_path,
					List *tlist, List *scan_clauses)
{
	SeqScan    *scan_plan;
	Index		scan_relid = best_path->parent->relid;

	/* 应该是一个基础关系 */
	Assert(scan_relid > 0);
	Assert(best_path->parent->rtekind == RTE_RELATION);

	/* 将限制条件按最佳执行顺序排序 */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* 将 RestrictInfo 列表简化为表达式列表，忽略伪常量 */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/*
	 * 如果有参数化路径，则替换外部关系变量为 nestloop 参数
	 * Var、PlaceHolderVar等类型变换。
	 */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
	}

	/* 构建顺序扫描计划节点 */
	scan_plan = make_seqscan(tlist,
							 scan_clauses,
							 scan_relid);

	/* 从 Path 复制成本信息到 Plan */
	copy_generic_path_info(&scan_plan->plan, best_path);

	return scan_plan;
}

/*
 * create_samplescan_plan
 *	 为 'best_path' 扫描的基础关系生成一个采样扫描计划节点，
 *	 使用限制条件 'scan_clauses' 和目标列列表 'tlist'。
 */
static SampleScan *
create_samplescan_plan(PlannerInfo *root, Path *best_path,
					   List *tlist, List *scan_clauses)
{
	SampleScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;
	TableSampleClause *tsc;

	/* 应该是一个带有采样子句的基础关系 */
	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_RELATION);
	tsc = rte->tablesample;
	Assert(tsc != NULL);

	/* 将限制条件按最佳执行顺序排序 */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* 将 RestrictInfo 列表简化为表达式列表，忽略伪常量 */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* 如果有参数化路径，则替换外部关系变量为 nestloop 参数 */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
		tsc = (TableSampleClause *)
			replace_nestloop_params(root, (Node *) tsc);
	}

	/* 构建采样扫描计划节点 */
	scan_plan = make_samplescan(tlist,
								scan_clauses,
								scan_relid,
								tsc);

	/* 从 Path 复制成本信息到 Plan */
	copy_generic_path_info(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_indexscan_plan
 *    为best_path扫描的基础关系创建一个索引扫描计划，
 *    使用限制条件scan_clauses和目标列表tlist。
 *
 * 我们使用此函数来构建普通索引扫描(IndexScan)和仅索引扫描(IndexOnlyScan)，
 * 因为两者的条件预处理工作是相同的。注意，由调用者告诉我们要构建哪一种，
 * 我们不查看best_path->path.pathtype，因为create_bitmap_subplan需要能够覆盖先前的决定。
 */
static Scan *
create_indexscan_plan(PlannerInfo *root,          /* 查询规划器的全局信息 */
                      IndexPath *best_path,       /* 选择的索引路径 */
                      List *tlist,                /* 目标列表（要检索的列） */
                      List *scan_clauses,         /* 应用于此关系的限制条件列表 */
                      bool indexonly)             /* 是否创建仅索引扫描 */
{
    Scan       *scan_plan;                      /* 最终生成的扫描计划节点 */
    List       *indexclauses = best_path->indexclauses; /* 可用于索引扫描的条件 */
    List       *indexorderbys = best_path->indexorderbys; /* 用于索引排序的表达式 */
    Index      baserelid = best_path->path.parent->relid; /* 基表的关系ID */
    IndexOptInfo *indexinfo = best_path->indexinfo; /* 索引的优化信息 */
    Oid        indexoid = indexinfo->indexoid;  /* 索引的OID */
    List       *qpqual;                         /* 传给执行器的查询谓词 */
    List       *stripped_indexquals;            /* 去除RestrictInfo后的索引条件 */
    List       *fixed_indexquals;               /* 替换为索引Var后的索引条件 */
    List       *fixed_indexorderbys;            /* 修复后的索引ORDER BY表达式 */
    List       *indexorderbyops = NIL;          /* ORDER BY表达式的排序操作符 */
    ListCell   *l;                              /* 用于遍历列表的临时变量 */

    /* 验证这是一个基础关系... */
    Assert(baserelid > 0);
    Assert(best_path->path.parent->rtekind == RTE_RELATION);

    /*
     * 从IndexClauses列表中提取索引条件表达式（去除RestrictInfo包装），
     * 并准备一个将表Var替换为索引Var的副本。
     * （此步骤还在fixed_indexquals上执行replace_nestloop_params）
     */
    fix_indexqual_references(root, best_path,
                            &stripped_indexquals,
                            &fixed_indexquals);

    /*
     * 同样修复ORDER BY表达式中的索引属性引用
     */
    fixed_indexorderbys = fix_indexorderby_references(root, best_path);

    /*
     * qpqual列表必须包含所有不能由索引自动处理的限制条件，
     * 除了伪常量条件，后者将由单独的门控计划节点处理。
     * indexquals中的所有谓词都将被检查（由索引本身或nodeIndexscan.c），
     * 但如果涉及任何"特殊"操作符，则必须将它们包含在qpqual中。
     * 结果是qpqual必须包含scan_clauses中不在indexquals中的部分。
     *
     * is_redundant_with_indexclauses()检测扫描条件出现在indexclauses列表中，
     * 或者是从与某些索引条件相同的等价类(EquivalenceClass)生成的情况，
     * 因此尽管不相等，但与indexclauses冗余。
     * （后者发生在indxpath.c偏好不同于generate_join_implied_equalities为参数化扫描的
     * ppi_clauses选择的派生等式时。）注意，它不会匹配有损索引条件，
     * 这很关键，因为在那种情况下我们必须在qpqual中包含原始条件。
     *
     * 在某些情况下（特别是带有OR的索引条件），我们可能有不等于但在逻辑上
     * 被索引条件隐含的scan_clauses；因此我们也尝试使用predicate_implied_by()
     * 检查，看看是否可以通过这种方式丢弃条件。（predicate_implied_by假设其
     * 第一个输入只包含不可变函数，所以我们必须检查这一点。）
     *
     * 注意：如果你更改这段代码，你也应该查看costsize.c中的extract_nonindex_conditions()。
     */
    qpqual = NIL;
    foreach(l, scan_clauses)
    {
        RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

        if (rinfo->pseudoconstant)
			continue;           /* 我们可以在这里丢弃伪常量 */
		
		if (is_redundant_with_indexclauses(rinfo, indexclauses))
			continue;           /* 重复的或源自相同等价类 */
		
		if (!contain_mutable_functions((Node*)rinfo->clause) &&
            predicate_implied_by(list_make1(rinfo->clause), stripped_indexquals, false))
			continue;           /* 被索引条件逻辑隐含 */
		
		qpqual = lappend(qpqual, rinfo);
    }

    /* 将条件按最佳执行顺序排序 */
    qpqual = order_qual_clauses(root, qpqual);

    /* 将RestrictInfo列表简化为裸表达式；忽略伪常量 */
    qpqual = extract_actual_clauses(qpqual, false);

    /*
     * 我们必须在indexqualorig、qpqual和indexorderbyorig表达式中
     * 将外部关系变量替换为嵌套循环参数。这有点麻烦，需要与fix_indexqual_references
     * 中的处理分开进行——在泛化内部索引扫描支持时重新考虑这一点。
     * 但请注意，我们不能真的更早地执行此操作，因为这会破坏上面与谓词的比较...
     * （或者不会？那些不会有外部引用）
     */
    if (best_path->path.param_info)
    {
        stripped_indexquals = (List *)
            replace_nestloop_params(root, (Node *) stripped_indexquals);
        qpqual = (List *)
            replace_nestloop_params(root, (Node *) qpqual);
        indexorderbys = (List *)
            replace_nestloop_params(root, (Node *) indexorderbys);
    }

    /*
     * 如果有ORDER BY表达式，查找它们结果数据类型的排序操作符
     */
    if (indexorderbys)
    {
        ListCell   *pathkeyCell,
                   *exprCell;

        /*
         * PathKey包含我们排序所依据的btree操作符族的OID，
         * 但这还不够，因为我们需要表达式的数据类型来查找操作符族中的排序操作符
         */
        Assert(list_length(best_path->path.pathkeys) == list_length(indexorderbys));
        forboth(pathkeyCell, best_path->path.pathkeys, exprCell, indexorderbys)
        {
            PathKey    *pathkey = (PathKey *) lfirst(pathkeyCell);
            Node       *expr = (Node *) lfirst(exprCell);
            Oid         exprtype = exprType(expr);
            Oid         sortop;

            /* 从操作符族获取排序操作符 */
            sortop = get_opfamily_member(pathkey->pk_opfamily,
                                        exprtype,
                                        exprtype,
                                        pathkey->pk_strategy);
            if (!OidIsValid(sortop))
                elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
                     pathkey->pk_strategy, exprtype, exprtype, pathkey->pk_opfamily);
            indexorderbyops = lappend_oid(indexorderbyops, sortop);
        }
    }

    /*
     * 对于仅索引扫描，我们必须将索引目标列表中索引AM无法返回的列标记为resjunk；
     * 这提示setrefs.c不要生成对这些列的引用
     */
    if (indexonly)
    {
        int         i = 0;

        foreach(l, indexinfo->indextlist)
        {
            TargetEntry *indextle = (TargetEntry *) lfirst(l);

            indextle->resjunk = !indexinfo->canreturn[i];
            i++;
        }
    }

    /* 最终准备构建计划节点 */
    if (indexonly)
        scan_plan = (Scan *) make_indexonlyscan(tlist,
                                             qpqual,
                                             baserelid,
                                             indexoid,
                                             fixed_indexquals,
                                             stripped_indexquals,
                                             fixed_indexorderbys,
                                             indexinfo->indextlist,
                                             best_path->indexscandir);
    else
        scan_plan = (Scan *) make_indexscan(tlist,
                                         qpqual,
                                         baserelid,
                                         indexoid,
                                         fixed_indexquals,
                                         stripped_indexquals,
                                         fixed_indexorderbys,
                                         indexorderbys,
                                         indexorderbyops,
                                         best_path->indexscandir);

    /* 复制通用路径信息到计划节点 */
    copy_generic_path_info(&scan_plan->plan, &best_path->path);

    return scan_plan;
}


/*
 * create_bitmap_scan_plan
 *	  Returns a bitmap scan plan for the base relation scanned by 'best_path'
 *	  with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static BitmapHeapScan *
create_bitmap_scan_plan(PlannerInfo *root,
						BitmapHeapPath *best_path,
						List *tlist,
						List *scan_clauses)
{
	Index		baserelid = best_path->path.parent->relid;
	Plan	   *bitmapqualplan;
	List	   *bitmapqualorig;
	List	   *indexquals;
	List	   *indexECs;
	List	   *qpqual;
	ListCell   *l;
	BitmapHeapScan *scan_plan;

	/* it should be a base rel... */
	Assert(baserelid > 0);
	Assert(best_path->path.parent->rtekind == RTE_RELATION);

	/* Process the bitmapqual tree into a Plan tree and qual lists */
	bitmapqualplan = create_bitmap_subplan(root, best_path->bitmapqual,
										   &bitmapqualorig, &indexquals,
										   &indexECs);

	if (best_path->path.parallel_aware)
		bitmap_subplan_mark_shared(bitmapqualplan);

	/*
	 * The qpqual list must contain all restrictions not automatically handled
	 * by the index, other than pseudoconstant clauses which will be handled
	 * by a separate gating plan node.  All the predicates in the indexquals
	 * will be checked (either by the index itself, or by
	 * nodeBitmapHeapscan.c), but if there are any "special" operators
	 * involved then they must be added to qpqual.  The upshot is that qpqual
	 * must contain scan_clauses minus whatever appears in indexquals.
	 *
	 * This loop is similar to the comparable code in create_indexscan_plan(),
	 * but with some differences because it has to compare the scan clauses to
	 * stripped (no RestrictInfos) indexquals.  See comments there for more
	 * info.
	 *
	 * In normal cases simple equal() checks will be enough to spot duplicate
	 * clauses, so we try that first.  We next see if the scan clause is
	 * redundant with any top-level indexqual by virtue of being generated
	 * from the same EC.  After that, try predicate_implied_by().
	 *
	 * Unlike create_indexscan_plan(), the predicate_implied_by() test here is
	 * useful for getting rid of qpquals that are implied by index predicates,
	 * because the predicate conditions are included in the "indexquals"
	 * returned by create_bitmap_subplan().  Bitmap scans have to do it that
	 * way because predicate conditions need to be rechecked if the scan
	 * becomes lossy, so they have to be included in bitmapqualorig.
	 */
	qpqual = NIL;
	foreach(l, scan_clauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);
		Node	   *clause = (Node *) rinfo->clause;

		if (rinfo->pseudoconstant)
			continue;			/* we may drop pseudoconstants here */
		if (list_member(indexquals, clause))
			continue;			/* simple duplicate */
		if (rinfo->parent_ec && list_member_ptr(indexECs, rinfo->parent_ec))
			continue;			/* derived from same EquivalenceClass */
		if (!contain_mutable_functions(clause) &&
			predicate_implied_by(list_make1(clause), indexquals, false))
			continue;			/* provably implied by indexquals */
		qpqual = lappend(qpqual, rinfo);
	}

	/* Sort clauses into best execution order */
	qpqual = order_qual_clauses(root, qpqual);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	qpqual = extract_actual_clauses(qpqual, false);

	/*
	 * When dealing with special operators, we will at this point have
	 * duplicate clauses in qpqual and bitmapqualorig.  We may as well drop
	 * 'em from bitmapqualorig, since there's no point in making the tests
	 * twice.
	 */
	bitmapqualorig = list_difference_ptr(bitmapqualorig, qpqual);

	/*
	 * We have to replace any outer-relation variables with nestloop params in
	 * the qpqual and bitmapqualorig expressions.  (This was already done for
	 * expressions attached to plan nodes in the bitmapqualplan tree.)
	 */
	if (best_path->path.param_info)
	{
		qpqual = (List *)
			replace_nestloop_params(root, (Node *) qpqual);
		bitmapqualorig = (List *)
			replace_nestloop_params(root, (Node *) bitmapqualorig);
	}

	/* Finally ready to build the plan node */
	scan_plan = make_bitmap_heapscan(tlist,
									 qpqual,
									 bitmapqualplan,
									 bitmapqualorig,
									 baserelid);

	copy_generic_path_info(&scan_plan->scan.plan, &best_path->path);

	return scan_plan;
}

/*
 * Given a bitmapqual tree, generate the Plan tree that implements it
 *
 * As byproducts, we also return in *qual and *indexqual the qual lists
 * (in implicit-AND form, without RestrictInfos) describing the original index
 * conditions and the generated indexqual conditions.  (These are the same in
 * simple cases, but when special index operators are involved, the former
 * list includes the special conditions while the latter includes the actual
 * indexable conditions derived from them.)  Both lists include partial-index
 * predicates, because we have to recheck predicates as well as index
 * conditions if the bitmap scan becomes lossy.
 *
 * In addition, we return a list of EquivalenceClass pointers for all the
 * top-level indexquals that were possibly-redundantly derived from ECs.
 * This allows removal of scan_clauses that are redundant with such quals.
 * (We do not attempt to detect such redundancies for quals that are within
 * OR subtrees.  This could be done in a less hacky way if we returned the
 * indexquals in RestrictInfo form, but that would be slower and still pretty
 * messy, since we'd have to build new RestrictInfos in many cases.)
 */
static Plan *
create_bitmap_subplan(PlannerInfo *root, Path *bitmapqual,
					  List **qual, List **indexqual, List **indexECs)
{
	Plan	   *plan;

	if (IsA(bitmapqual, BitmapAndPath))
	{
		BitmapAndPath *apath = (BitmapAndPath *) bitmapqual;
		List	   *subplans = NIL;
		List	   *subquals = NIL;
		List	   *subindexquals = NIL;
		List	   *subindexECs = NIL;
		ListCell   *l;

		/*
		 * There may well be redundant quals among the subplans, since a
		 * top-level WHERE qual might have gotten used to form several
		 * different index quals.  We don't try exceedingly hard to eliminate
		 * redundancies, but we do eliminate obvious duplicates by using
		 * list_concat_unique.
		 */
		foreach(l, apath->bitmapquals)
		{
			Plan	   *subplan;
			List	   *subqual;
			List	   *subindexqual;
			List	   *subindexEC;

			subplan = create_bitmap_subplan(root, (Path *) lfirst(l),
											&subqual, &subindexqual,
											&subindexEC);
			subplans = lappend(subplans, subplan);
			subquals = list_concat_unique(subquals, subqual);
			subindexquals = list_concat_unique(subindexquals, subindexqual);
			/* Duplicates in indexECs aren't worth getting rid of */
			subindexECs = list_concat(subindexECs, subindexEC);
		}
		plan = (Plan *) make_bitmap_and(subplans);
		plan->startup_cost = apath->path.startup_cost;
		plan->total_cost = apath->path.total_cost;
		plan->plan_rows =
			clamp_row_est(apath->bitmapselectivity * apath->path.parent->tuples);
		plan->plan_width = 0;	/* meaningless */
		plan->parallel_aware = false;
		plan->parallel_safe = apath->path.parallel_safe;
		*qual = subquals;
		*indexqual = subindexquals;
		*indexECs = subindexECs;
	}
	else if (IsA(bitmapqual, BitmapOrPath))
	{
		BitmapOrPath *opath = (BitmapOrPath *) bitmapqual;
		List	   *subplans = NIL;
		List	   *subquals = NIL;
		List	   *subindexquals = NIL;
		bool		const_true_subqual = false;
		bool		const_true_subindexqual = false;
		ListCell   *l;

		/*
		 * Here, we only detect qual-free subplans.  A qual-free subplan would
		 * cause us to generate "... OR true ..."  which we may as well reduce
		 * to just "true".  We do not try to eliminate redundant subclauses
		 * because (a) it's not as likely as in the AND case, and (b) we might
		 * well be working with hundreds or even thousands of OR conditions,
		 * perhaps from a long IN list.  The performance of list_append_unique
		 * would be unacceptable.
		 */
		foreach(l, opath->bitmapquals)
		{
			Plan	   *subplan;
			List	   *subqual;
			List	   *subindexqual;
			List	   *subindexEC;

			subplan = create_bitmap_subplan(root, (Path *) lfirst(l),
											&subqual, &subindexqual,
											&subindexEC);
			subplans = lappend(subplans, subplan);
			if (subqual == NIL)
				const_true_subqual = true;
			else if (!const_true_subqual)
				subquals = lappend(subquals,
								   make_ands_explicit(subqual));
			if (subindexqual == NIL)
				const_true_subindexqual = true;
			else if (!const_true_subindexqual)
				subindexquals = lappend(subindexquals,
										make_ands_explicit(subindexqual));
		}

		/*
		 * In the presence of ScalarArrayOpExpr quals, we might have built
		 * BitmapOrPaths with just one subpath; don't add an OR step.
		 */
		if (list_length(subplans) == 1)
		{
			plan = (Plan *) linitial(subplans);
		}
		else
		{
			plan = (Plan *) make_bitmap_or(subplans);
			plan->startup_cost = opath->path.startup_cost;
			plan->total_cost = opath->path.total_cost;
			plan->plan_rows =
				clamp_row_est(opath->bitmapselectivity * opath->path.parent->tuples);
			plan->plan_width = 0;	/* meaningless */
			plan->parallel_aware = false;
			plan->parallel_safe = opath->path.parallel_safe;
		}

		/*
		 * If there were constant-TRUE subquals, the OR reduces to constant
		 * TRUE.  Also, avoid generating one-element ORs, which could happen
		 * due to redundancy elimination or ScalarArrayOpExpr quals.
		 */
		if (const_true_subqual)
			*qual = NIL;
		else if (list_length(subquals) <= 1)
			*qual = subquals;
		else
			*qual = list_make1(make_orclause(subquals));
		if (const_true_subindexqual)
			*indexqual = NIL;
		else if (list_length(subindexquals) <= 1)
			*indexqual = subindexquals;
		else
			*indexqual = list_make1(make_orclause(subindexquals));
		*indexECs = NIL;
	}
	else if (IsA(bitmapqual, IndexPath))
	{
		IndexPath  *ipath = (IndexPath *) bitmapqual;
		IndexScan  *iscan;
		List	   *subquals;
		List	   *subindexquals;
		List	   *subindexECs;
		ListCell   *l;

		/* Use the regular indexscan plan build machinery... */
		iscan = castNode(IndexScan,
						 create_indexscan_plan(root, ipath,
											   NIL, NIL, false));
		/* then convert to a bitmap indexscan */
		plan = (Plan *) make_bitmap_indexscan(iscan->scan.scanrelid,
											  iscan->indexid,
											  iscan->indexqual,
											  iscan->indexqualorig);
		/* and set its cost/width fields appropriately */
		plan->startup_cost = 0.0;
		plan->total_cost = ipath->indextotalcost;
		plan->plan_rows =
			clamp_row_est(ipath->indexselectivity * ipath->path.parent->tuples);
		plan->plan_width = 0;	/* meaningless */
		plan->parallel_aware = false;
		plan->parallel_safe = ipath->path.parallel_safe;
		/* Extract original index clauses, actual index quals, relevant ECs */
		subquals = NIL;
		subindexquals = NIL;
		subindexECs = NIL;
		foreach(l, ipath->indexclauses)
		{
			IndexClause *iclause = (IndexClause *) lfirst(l);
			RestrictInfo *rinfo = iclause->rinfo;

			Assert(!rinfo->pseudoconstant);
			subquals = lappend(subquals, rinfo->clause);
			subindexquals = list_concat(subindexquals,
										get_actual_clauses(iclause->indexquals));
			if (rinfo->parent_ec)
				subindexECs = lappend(subindexECs, rinfo->parent_ec);
		}
		/* We can add any index predicate conditions, too */
		foreach(l, ipath->indexinfo->indpred)
		{
			Expr	   *pred = (Expr *) lfirst(l);

			/*
			 * We know that the index predicate must have been implied by the
			 * query condition as a whole, but it may or may not be implied by
			 * the conditions that got pushed into the bitmapqual.  Avoid
			 * generating redundant conditions.
			 */
			if (!predicate_implied_by(list_make1(pred), subquals, false))
			{
				subquals = lappend(subquals, pred);
				subindexquals = lappend(subindexquals, pred);
			}
		}
		*qual = subquals;
		*indexqual = subindexquals;
		*indexECs = subindexECs;
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d", nodeTag(bitmapqual));
		plan = NULL;			/* keep compiler quiet */
	}

	return plan;
}

/*
 * create_tidscan_plan
 *	 Returns a tidscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static TidScan *
create_tidscan_plan(PlannerInfo *root, TidPath *best_path,
					List *tlist, List *scan_clauses)
{
	TidScan    *scan_plan;
	Index		scan_relid = best_path->path.parent->relid;
	List	   *tidquals = best_path->tidquals;

	/* it should be a base rel... */
	Assert(scan_relid > 0);
	Assert(best_path->path.parent->rtekind == RTE_RELATION);

	/*
	 * The qpqual list must contain all restrictions not enforced by the
	 * tidquals list.  Since tidquals has OR semantics, we have to be careful
	 * about matching it up to scan_clauses.  It's convenient to handle the
	 * single-tidqual case separately from the multiple-tidqual case.  In the
	 * single-tidqual case, we look through the scan_clauses while they are
	 * still in RestrictInfo form, and drop any that are redundant with the
	 * tidqual.
	 *
	 * In normal cases simple pointer equality checks will be enough to spot
	 * duplicate RestrictInfos, so we try that first.
	 *
	 * Another common case is that a scan_clauses entry is generated from the
	 * same EquivalenceClass as some tidqual, and is therefore redundant with
	 * it, though not equal.
	 *
	 * Unlike indexpaths, we don't bother with predicate_implied_by(); the
	 * number of cases where it could win are pretty small.
	 */
	if (list_length(tidquals) == 1)
	{
		List	   *qpqual = NIL;
		ListCell   *l;

		foreach(l, scan_clauses)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

			if (rinfo->pseudoconstant)
				continue;		/* we may drop pseudoconstants here */
			if (list_member_ptr(tidquals, rinfo))
				continue;		/* simple duplicate */
			if (is_redundant_derived_clause(rinfo, tidquals))
				continue;		/* derived from same EquivalenceClass */
			qpqual = lappend(qpqual, rinfo);
		}
		scan_clauses = qpqual;
	}

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo lists to bare expressions; ignore pseudoconstants */
	tidquals = extract_actual_clauses(tidquals, false);
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/*
	 * If we have multiple tidquals, it's more convenient to remove duplicate
	 * scan_clauses after stripping the RestrictInfos.  In this situation,
	 * because the tidquals represent OR sub-clauses, they could not have come
	 * from EquivalenceClasses so we don't have to worry about matching up
	 * non-identical clauses.  On the other hand, because tidpath.c will have
	 * extracted those sub-clauses from some OR clause and built its own list,
	 * we will certainly not have pointer equality to any scan clause.  So
	 * convert the tidquals list to an explicit OR clause and see if we can
	 * match it via equal() to any scan clause.
	 */
	if (list_length(tidquals) > 1)
		scan_clauses = list_difference(scan_clauses,
									   list_make1(make_orclause(tidquals)));

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->path.param_info)
	{
		tidquals = (List *)
			replace_nestloop_params(root, (Node *) tidquals);
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
	}

	scan_plan = make_tidscan(tlist,
							 scan_clauses,
							 scan_relid,
							 tidquals);

	copy_generic_path_info(&scan_plan->scan.plan, &best_path->path);

	return scan_plan;
}

/*
 * create_subqueryscan_plan
 *	 Returns a subqueryscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static SubqueryScan *
create_subqueryscan_plan(PlannerInfo *root, SubqueryScanPath *best_path,
						 List *tlist, List *scan_clauses)
{
	SubqueryScan *scan_plan;
	RelOptInfo *rel = best_path->path.parent;
	Index		scan_relid = rel->relid;
	Plan	   *subplan;

	/* it should be a subquery base rel... */
	Assert(scan_relid > 0);
	Assert(rel->rtekind == RTE_SUBQUERY);

	/*
	 * Recursively create Plan from Path for subquery.  Since we are entering
	 * a different planner context (subroot), recurse to create_plan not
	 * create_plan_recurse.
	 */
	subplan = create_plan(rel->subroot, best_path->subpath);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->path.param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
		process_subquery_nestloop_params(root,
										 rel->subplan_params);
	}

	scan_plan = make_subqueryscan(tlist,
								  scan_clauses,
								  scan_relid,
								  subplan);

	copy_generic_path_info(&scan_plan->scan.plan, &best_path->path);

	return scan_plan;
}

/*
 * create_functionscan_plan
 *	 Returns a functionscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static FunctionScan *
create_functionscan_plan(PlannerInfo *root, Path *best_path,
						 List *tlist, List *scan_clauses)
{
	FunctionScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;
	List	   *functions;

	/* it should be a function base rel... */
	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_FUNCTION);
	functions = rte->functions;

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
		/* The function expressions could contain nestloop params, too */
		functions = (List *) replace_nestloop_params(root, (Node *) functions);
	}

	scan_plan = make_functionscan(tlist, scan_clauses, scan_relid,
								  functions, rte->funcordinality);

	copy_generic_path_info(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_tablefuncscan_plan
 *	 Returns a tablefuncscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static TableFuncScan *
create_tablefuncscan_plan(PlannerInfo *root, Path *best_path,
						  List *tlist, List *scan_clauses)
{
	TableFuncScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;
	TableFunc  *tablefunc;

	/* it should be a function base rel... */
	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_TABLEFUNC);
	tablefunc = rte->tablefunc;

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
		/* The function expressions could contain nestloop params, too */
		tablefunc = (TableFunc *) replace_nestloop_params(root, (Node *) tablefunc);
	}

	scan_plan = make_tablefuncscan(tlist, scan_clauses, scan_relid,
								   tablefunc);

	copy_generic_path_info(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_valuesscan_plan
 *	 Returns a valuesscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static ValuesScan *
create_valuesscan_plan(PlannerInfo *root, Path *best_path,
					   List *tlist, List *scan_clauses)
{
	ValuesScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;
	List	   *values_lists;

	/* it should be a values base rel... */
	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_VALUES);
	values_lists = rte->values_lists;

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
		/* The values lists could contain nestloop params, too */
		values_lists = (List *)
			replace_nestloop_params(root, (Node *) values_lists);
	}

	scan_plan = make_valuesscan(tlist, scan_clauses, scan_relid,
								values_lists);

	copy_generic_path_info(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_ctescan_plan
 *	 Returns a ctescan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static CteScan *
create_ctescan_plan(PlannerInfo *root, Path *best_path,
					List *tlist, List *scan_clauses)
{
	CteScan    *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;
	SubPlan    *ctesplan = NULL;
	int			plan_id;
	int			cte_param_id;
	PlannerInfo *cteroot;
	Index		levelsup;
	int			ndx;
	ListCell   *lc;

	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_CTE);
	Assert(!rte->self_reference);

	/*
	 * Find the referenced CTE, and locate the SubPlan previously made for it.
	 */
	levelsup = rte->ctelevelsup;
	cteroot = root;
	while (levelsup-- > 0)
	{
		cteroot = cteroot->parent_root;
		if (!cteroot)			/* shouldn't happen */
			elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
	}

	/*
	 * Note: cte_plan_ids can be shorter than cteList, if we are still working
	 * on planning the CTEs (ie, this is a side-reference from another CTE).
	 * So we mustn't use forboth here.
	 */
	ndx = 0;
	foreach(lc, cteroot->parse->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

		if (strcmp(cte->ctename, rte->ctename) == 0)
			break;
		ndx++;
	}
	if (lc == NULL)				/* shouldn't happen */
		elog(ERROR, "could not find CTE \"%s\"", rte->ctename);
	if (ndx >= list_length(cteroot->cte_plan_ids))
		elog(ERROR, "could not find plan for CTE \"%s\"", rte->ctename);
	plan_id = list_nth_int(cteroot->cte_plan_ids, ndx);
	if (plan_id <= 0)
		elog(ERROR, "no plan was made for CTE \"%s\"", rte->ctename);
	foreach(lc, cteroot->init_plans)
	{
		ctesplan = (SubPlan *) lfirst(lc);
		if (ctesplan->plan_id == plan_id)
			break;
	}
	if (lc == NULL)				/* shouldn't happen */
		elog(ERROR, "could not find plan for CTE \"%s\"", rte->ctename);

	/*
	 * We need the CTE param ID, which is the sole member of the SubPlan's
	 * setParam list.
	 */
	cte_param_id = linitial_int(ctesplan->setParam);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
	}

	scan_plan = make_ctescan(tlist, scan_clauses, scan_relid,
							 plan_id, cte_param_id);

	copy_generic_path_info(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_namedtuplestorescan_plan
 *	 Returns a tuplestorescan plan for the base relation scanned by
 *	'best_path' with restriction clauses 'scan_clauses' and targetlist
 *	'tlist'.
 */
static NamedTuplestoreScan *
create_namedtuplestorescan_plan(PlannerInfo *root, Path *best_path,
								List *tlist, List *scan_clauses)
{
	NamedTuplestoreScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;

	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_NAMEDTUPLESTORE);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
	}

	scan_plan = make_namedtuplestorescan(tlist, scan_clauses, scan_relid,
										 rte->enrname);

	copy_generic_path_info(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_resultscan_plan
 *	 Returns a Result plan for the RTE_RESULT base relation scanned by
 *	'best_path' with restriction clauses 'scan_clauses' and targetlist
 *	'tlist'.
 */
static Result *
create_resultscan_plan(PlannerInfo *root, Path *best_path,
					   List *tlist, List *scan_clauses)
{
	Result	   *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte PG_USED_FOR_ASSERTS_ONLY;

	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_RESULT);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
	}

	scan_plan = make_result(tlist, (Node *) scan_clauses, NULL);

	copy_generic_path_info(&scan_plan->plan, best_path);

	return scan_plan;
}

/*
 * create_worktablescan_plan
 *	 Returns a worktablescan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static WorkTableScan *
create_worktablescan_plan(PlannerInfo *root, Path *best_path,
						  List *tlist, List *scan_clauses)
{
	WorkTableScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;
	Index		levelsup;
	PlannerInfo *cteroot;

	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_CTE);
	Assert(rte->self_reference);

	/*
	 * We need to find the worktable param ID, which is in the plan level
	 * that's processing the recursive UNION, which is one level *below* where
	 * the CTE comes from.
	 */
	levelsup = rte->ctelevelsup;
	if (levelsup == 0)			/* shouldn't happen */
		elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
	levelsup--;
	cteroot = root;
	while (levelsup-- > 0)
	{
		cteroot = cteroot->parent_root;
		if (!cteroot)			/* shouldn't happen */
			elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
	}
	if (cteroot->wt_param_id < 0)	/* shouldn't happen */
		elog(ERROR, "could not find param ID for CTE \"%s\"", rte->ctename);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
	}

	scan_plan = make_worktablescan(tlist, scan_clauses, scan_relid,
								   cteroot->wt_param_id);

	copy_generic_path_info(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_foreignscan_plan
 *	 Returns a foreignscan plan for the relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static ForeignScan *
create_foreignscan_plan(PlannerInfo *root, ForeignPath *best_path,
						List *tlist, List *scan_clauses)
{
	ForeignScan *scan_plan;
	RelOptInfo *rel = best_path->path.parent;
	Index		scan_relid = rel->relid;
	Oid			rel_oid = InvalidOid;
	Plan	   *outer_plan = NULL;

	Assert(rel->fdwroutine != NULL);

	/* transform the child path if any */
	if (best_path->fdw_outerpath)
		outer_plan = create_plan_recurse(root, best_path->fdw_outerpath,
										 CP_EXACT_TLIST);

	/*
	 * If we're scanning a base relation, fetch its OID.  (Irrelevant if
	 * scanning a join relation.)
	 */
	if (scan_relid > 0)
	{
		RangeTblEntry *rte;

		Assert(rel->rtekind == RTE_RELATION);
		rte = planner_rt_fetch(scan_relid, root);
		Assert(rte->rtekind == RTE_RELATION);
		rel_oid = rte->relid;
	}

	/*
	 * Sort clauses into best execution order.  We do this first since the FDW
	 * might have more info than we do and wish to adjust the ordering.
	 */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/*
	 * Let the FDW perform its processing on the restriction clauses and
	 * generate the plan node.  Note that the FDW might remove restriction
	 * clauses that it intends to execute remotely, or even add more (if it
	 * has selected some join clauses for remote use but also wants them
	 * rechecked locally).
	 */
	scan_plan = rel->fdwroutine->GetForeignPlan(root, rel, rel_oid,
												best_path,
												tlist, scan_clauses,
												outer_plan);

	/* Copy cost data from Path to Plan; no need to make FDW do this */
	copy_generic_path_info(&scan_plan->scan.plan, &best_path->path);

	/* Copy foreign server OID; likewise, no need to make FDW do this */
	scan_plan->fs_server = rel->serverid;

	/*
	 * Likewise, copy the relids that are represented by this foreign scan. An
	 * upper rel doesn't have relids set, but it covers all the base relations
	 * participating in the underlying scan, so use root's all_baserels.
	 */
	if (rel->reloptkind == RELOPT_UPPER_REL)
		scan_plan->fs_relids = root->all_baserels;
	else
		scan_plan->fs_relids = best_path->path.parent->relids;

	/*
	 * If this is a foreign join, and to make it valid to push down we had to
	 * assume that the current user is the same as some user explicitly named
	 * in the query, mark the finished plan as depending on the current user.
	 */
	if (rel->useridiscurrent)
		root->glob->dependsOnRole = true;

	/*
	 * Replace any outer-relation variables with nestloop params in the qual,
	 * fdw_exprs and fdw_recheck_quals expressions.  We do this last so that
	 * the FDW doesn't have to be involved.  (Note that parts of fdw_exprs or
	 * fdw_recheck_quals could have come from join clauses, so doing this
	 * beforehand on the scan_clauses wouldn't work.)  We assume
	 * fdw_scan_tlist contains no such variables.
	 */
	if (best_path->path.param_info)
	{
		scan_plan->scan.plan.qual = (List *)
			replace_nestloop_params(root, (Node *) scan_plan->scan.plan.qual);
		scan_plan->fdw_exprs = (List *)
			replace_nestloop_params(root, (Node *) scan_plan->fdw_exprs);
		scan_plan->fdw_recheck_quals = (List *)
			replace_nestloop_params(root,
									(Node *) scan_plan->fdw_recheck_quals);
	}

	/*
	 * If rel is a base relation, detect whether any system columns are
	 * requested from the rel.  (If rel is a join relation, rel->relid will be
	 * 0, but there can be no Var with relid 0 in the rel's targetlist or the
	 * restriction clauses, so we skip this in that case.  Note that any such
	 * columns in base relations that were joined are assumed to be contained
	 * in fdw_scan_tlist.)	This is a bit of a kluge and might go away
	 * someday, so we intentionally leave it out of the API presented to FDWs.
	 */
	scan_plan->fsSystemCol = false;
	if (scan_relid > 0)
	{
		Bitmapset  *attrs_used = NULL;
		ListCell   *lc;
		int			i;

		/*
		 * First, examine all the attributes needed for joins or final output.
		 * Note: we must look at rel's targetlist, not the attr_needed data,
		 * because attr_needed isn't computed for inheritance child rels.
		 */
		pull_varattnos((Node *) rel->reltarget->exprs, scan_relid, &attrs_used);

		/* Add all the attributes used by restriction clauses. */
		foreach(lc, rel->baserestrictinfo)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

			pull_varattnos((Node *) rinfo->clause, scan_relid, &attrs_used);
		}

		/* Now, are any system columns requested from rel? */
		for (i = FirstLowInvalidHeapAttributeNumber + 1; i < 0; i++)
		{
			if (bms_is_member(i - FirstLowInvalidHeapAttributeNumber, attrs_used))
			{
				scan_plan->fsSystemCol = true;
				break;
			}
		}

		bms_free(attrs_used);
	}

	return scan_plan;
}

/*
 * create_customscan_plan
 *
 * Transform a CustomPath into a Plan.
 */
static CustomScan *
create_customscan_plan(PlannerInfo *root, CustomPath *best_path,
					   List *tlist, List *scan_clauses)
{
	CustomScan *cplan;
	RelOptInfo *rel = best_path->path.parent;
	List	   *custom_plans = NIL;
	ListCell   *lc;

	/* Recursively transform child paths. */
	foreach(lc, best_path->custom_paths)
	{
		Plan	   *plan = create_plan_recurse(root, (Path *) lfirst(lc),
											   CP_EXACT_TLIST);

		custom_plans = lappend(custom_plans, plan);
	}

	/*
	 * Sort clauses into the best execution order, although custom-scan
	 * provider can reorder them again.
	 */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/*
	 * Invoke custom plan provider to create the Plan node represented by the
	 * CustomPath.
	 */
	cplan = castNode(CustomScan,
					 best_path->methods->PlanCustomPath(root,
														rel,
														best_path,
														tlist,
														scan_clauses,
														custom_plans));

	/*
	 * Copy cost data from Path to Plan; no need to make custom-plan providers
	 * do this
	 */
	copy_generic_path_info(&cplan->scan.plan, &best_path->path);

	/* Likewise, copy the relids that are represented by this custom scan */
	cplan->custom_relids = best_path->path.parent->relids;

	/*
	 * Replace any outer-relation variables with nestloop params in the qual
	 * and custom_exprs expressions.  We do this last so that the custom-plan
	 * provider doesn't have to be involved.  (Note that parts of custom_exprs
	 * could have come from join clauses, so doing this beforehand on the
	 * scan_clauses wouldn't work.)  We assume custom_scan_tlist contains no
	 * such variables.
	 */
	if (best_path->path.param_info)
	{
		cplan->scan.plan.qual = (List *)
			replace_nestloop_params(root, (Node *) cplan->scan.plan.qual);
		cplan->custom_exprs = (List *)
			replace_nestloop_params(root, (Node *) cplan->custom_exprs);
	}

	return cplan;
}


/*****************************************************************************
 *
 *	JOIN METHODS
 *
 *****************************************************************************/

static NestLoop *
create_nestloop_plan(PlannerInfo *root,
					 NestPath *best_path)
{
	NestLoop   *join_plan;
	Plan	   *outer_plan;
	Plan	   *inner_plan;
	List	   *tlist = build_path_tlist(root, &best_path->path);
	List	   *joinrestrictclauses = best_path->joinrestrictinfo;
	List	   *joinclauses;
	List	   *otherclauses;
	Relids		outerrelids;
	List	   *nestParams;
	Relids		saveOuterRels = root->curOuterRels;

	/* NestLoop can project, so no need to be picky about child tlists */
	outer_plan = create_plan_recurse(root, best_path->outerjoinpath, 0);

	/* For a nestloop, include outer relids in curOuterRels for inner side */
	root->curOuterRels = bms_union(root->curOuterRels,
								   best_path->outerjoinpath->parent->relids);

	inner_plan = create_plan_recurse(root, best_path->innerjoinpath, 0);

	/* Restore curOuterRels */
	bms_free(root->curOuterRels);
	root->curOuterRels = saveOuterRels;

	/* Sort join qual clauses into best execution order */
	joinrestrictclauses = order_qual_clauses(root, joinrestrictclauses);

	/* Get the join qual clauses (in plain expression form) */
	/* Any pseudoconstant clauses are ignored here */
	if (IS_OUTER_JOIN(best_path->jointype))
	{
		extract_actual_join_clauses(joinrestrictclauses,
									best_path->path.parent->relids,
									&joinclauses, &otherclauses);
	}
	else
	{
		/* We can treat all clauses alike for an inner join */
		joinclauses = extract_actual_clauses(joinrestrictclauses, false);
		otherclauses = NIL;
	}

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->path.param_info)
	{
		joinclauses = (List *)
			replace_nestloop_params(root, (Node *) joinclauses);
		otherclauses = (List *)
			replace_nestloop_params(root, (Node *) otherclauses);
	}

	/*
	 * Identify any nestloop parameters that should be supplied by this join
	 * node, and remove them from root->curOuterParams.
	 */
	outerrelids = best_path->outerjoinpath->parent->relids;
	nestParams = identify_current_nestloop_params(root, outerrelids);

	join_plan = make_nestloop(tlist,
							  joinclauses,
							  otherclauses,
							  nestParams,
							  outer_plan,
							  inner_plan,
							  best_path->jointype,
							  best_path->inner_unique);

	copy_generic_path_info(&join_plan->join.plan, &best_path->path);

	return join_plan;
}
/*
 * create_mergejoin_plan
 *    为 'best_path' 创建一个 MergeJoin 计划节点，并递归处理其内外路径。
 *
 * 参数:
 * - root: 包含查询规划信息的根节点
 * - best_path: 要转换为计划节点的最佳MergeJoin路径
 *
 * 返回值:
 * - 指向新创建的MergeJoin计划节点的指针
 */
static MergeJoin *
create_mergejoin_plan(PlannerInfo *root,
			  MergePath *best_path)
{
	// 声明变量
	MergeJoin  *join_plan;        // 最终构建的MergeJoin计划节点
	Plan	   *outer_plan;      // 外连接子计划
	Plan	   *inner_plan;      // 内连接子计划
	// 构建目标列表(target list)，描述查询需要输出的列
	List	   *tlist = build_path_tlist(root, &best_path->jpath.path);
	// 连接条件相关变量
	List	   *joinclauses;     // 普通连接条件
	List	   *otherclauses;    // 外连接时的其他条件
	List	   *mergeclauses;    // 用于执行合并连接的等式条件
	// 排序键相关变量
	List	   *outerpathkeys;   // 外部路径的排序键
	List	   *innerpathkeys;   // 内部路径的排序键
	// 合并连接执行所需的信息
	int			nClauses;         // 合并条件数量
	Oid		   *mergefamilies;   // 合并操作符族OID数组
	Oid		   *mergecollations; // 排序规则OID数组
	int		   *mergestrategies; // 合并策略数组
	bool	   *mergenullsfirst; // NULL值排序策略数组
	// 处理等价类和路径键的临时变量
	PathKey    *opathkey;        // 当前处理的外部路径键
	EquivalenceClass *opeclass;  // 当前处理的外部等价类
	int			i;                // 循环计数器
	ListCell   *lc;              // 用于遍历合并条件列表的指针
	ListCell   *lop;             // 用于遍历外部路径键列表的指针
	ListCell   *lip;             // 用于遍历内部路径键列表的指针
	// 缓存外部和内部路径的引用
	Path	   *outer_path = best_path->jpath.outerjoinpath;
	Path	   *inner_path = best_path->jpath.innerjoinpath;

	/*
	 * MergeJoin 节点可以做投影，因此对子计划的 tlist 不需要严格要求。
	 * 但如果需要对子计划排序，建议使用更窄的 tlist，避免排序多余数据。
	 */
	
	/* 递归创建外连接计划节点，根据是否需要排序选择小 tlist */
	outer_plan = create_plan_recurse(root, best_path->jpath.outerjoinpath,
					 (best_path->outersortkeys != NIL) ? CP_SMALL_TLIST : 0);
	/* 递归创建内连接计划节点，根据是否需要排序选择小 tlist */
	inner_plan = create_plan_recurse(root, best_path->jpath.innerjoinpath,
					 (best_path->innersortkeys != NIL) ? CP_SMALL_TLIST : 0);

	/* 对连接条件进行排序优化（mergeclauses 不可重排） */
	joinclauses = order_qual_clauses(root, best_path->jpath.joinrestrictinfo);

	/* 获取连接条件（普通表达式形式），忽略伪常量条件 */
	if (IS_OUTER_JOIN(best_path->jpath.jointype))
	{
		// 外连接时需要区分原始连接条件和下推条件
		extract_actual_join_clauses(joinclauses,
							best_path->jpath.path.parent->relids,
							&joinclauses, &otherclauses);
	}
	else
	{
		// 内连接时所有条件都一样处理
		joinclauses = extract_actual_clauses(joinclauses, false);
		otherclauses = NIL;
	}

	/*
	 * 从连接条件中移除 mergeclauses，剩下的为需要检查的 qpqual 条件。
	 */
	// 获取用于合并连接的条件
	mergeclauses = get_actual_clauses(best_path->path_mergeclauses);
	// 从普通连接条件中移除已用于合并连接的条件
	joinclauses = list_difference(joinclauses, mergeclauses);

	/*
	 * 如果是参数化路径，则替换外部关系变量为 nestloop 参数。
	 * mergeclauses 不应包含外部参数。
	 */
	if (best_path->jpath.path.param_info)
	{
		// 替换普通连接条件中的外部关系变量
		joinclauses = (List *)
			replace_nestloop_params(root, (Node *) joinclauses);
		// 替换外连接其他条件中的外部关系变量
		otherclauses = (List *)
			replace_nestloop_params(root, (Node *) otherclauses);
	}

	/*
	 * 调整 mergeclauses，使外部变量始终在左侧，并标记 outer_is_left 状态。
	 */
	mergeclauses = get_switched_clauses(best_path->path_mergeclauses,
							best_path->jpath.outerjoinpath->parent->relids);

	/*
	 * 如有需要，为外部和内部路径添加显式排序节点。
	 */
	if (best_path->outersortkeys)
	{
		// 获取外部关系ID集合
		Relids		outer_relids = outer_path->parent->relids;
		// 创建排序节点
		Sort	   *sort = make_sort_from_pathkeys(outer_plan,
						   best_path->outersortkeys,
						   outer_relids);

		// 标记排序节点的成本和规模
		label_sort_with_costsize(root, sort, -1.0);
		// 更新外部计划为排序节点
		outer_plan = (Plan *) sort;
		outerpathkeys = best_path->outersortkeys;
	}
	else
		// 使用已有外部路径的排序键
		outerpathkeys = best_path->jpath.outerjoinpath->pathkeys;

	if (best_path->innersortkeys)
	{
		// 获取内部关系ID集合
		Relids		inner_relids = inner_path->parent->relids;
		// 创建排序节点
		Sort	   *sort = make_sort_from_pathkeys(inner_plan,
						   best_path->innersortkeys,
						   inner_relids);

		// 标记排序节点的成本和规模
		label_sort_with_costsize(root, sort, -1.0);
		// 更新内部计划为排序节点
		inner_plan = (Plan *) sort;
		innerpathkeys = best_path->innersortkeys;
	}
	else
		// 使用已有内部路径的排序键
		innerpathkeys = best_path->jpath.innerjoinpath->pathkeys;

	/*
	 * 如有需要，为内部计划添加物化节点，避免 mark/restore 操作。
	 */
	if (best_path->materialize_inner)
	{
		// 创建物化节点
		Plan	   *matplan = (Plan *) make_material(inner_plan);

		// 估算物化成本，假设不会落盘，仅每行 cpu_operator_cost
		copy_plan_costsize(matplan, inner_plan);
		matplan->total_cost += cpu_operator_cost * matplan->plan_rows;

		// 更新内部计划为物化节点
		inner_plan = matplan;
	}

	/*
	 * 构建执行器需要的 opfamily/collation/strategy/nullsfirst 数组。
	 * 信息来自于输入的 pathkeys，但需注意 mergeclauses 与 pathkeys 的顺序和冗余。
	 */
	// 获取合并条件数量
	nClauses = list_length(mergeclauses);
	Assert(nClauses == list_length(best_path->path_mergeclauses));
	// 为合并操作分配必要的数据结构
	mergefamilies = (Oid *) palloc(nClauses * sizeof(Oid));
	mergecollations = (Oid *) palloc(nClauses * sizeof(Oid));
	mergestrategies = (int *) palloc(nClauses * sizeof(int));
	mergenullsfirst = (bool *) palloc(nClauses * sizeof(bool));

	// 初始化变量
	opathkey = NULL;
	opeclass = NULL;
	lop = list_head(outerpathkeys);
	lip = list_head(innerpathkeys);
	i = 0;
	// 遍历所有合并条件，填充执行器所需信息
	foreach(lc, best_path->path_mergeclauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		EquivalenceClass *oeclass; // 外部等价类
		EquivalenceClass *ieclass; // 内部等价类
		PathKey    *ipathkey = NULL; // 当前处理的内部路径键
		EquivalenceClass *ipeclass = NULL; // 当前处理的内部等价类
		bool		first_inner_match = false; // 是否首次匹配内部路径键

		// 获取 mergeclause 的外部/内部等价类
		if (rinfo->outer_is_left)
		{
			oeclass = rinfo->left_ec;
			ieclass = rinfo->right_ec;
		}
		else
		{
			oeclass = rinfo->right_ec;
			ieclass = rinfo->left_ec;
		}
		Assert(oeclass != NULL);
		Assert(ieclass != NULL);

		/*
		 * 通过等价类匹配 pathkey 元素，确保顺序一致。
		 * 外部 pathkeys 顺序应与 mergeclauses 一致，内部 pathkeys 可能有冗余。
		 */
		// 处理外部路径键
		if (oeclass != opeclass)
		{
			if (lop == NULL)
				elog(ERROR, "outer pathkeys do not match mergeclauses");
			opathkey = (PathKey *) lfirst(lop);
			opeclass = opathkey->pk_eclass;
			lop = lnext(lop);
			if (oeclass != opeclass)
				elog(ERROR, "outer pathkeys do not match mergeclauses");
		}

		// 处理内部路径键
		if (lip)
		{
			ipathkey = (PathKey *) lfirst(lip);
			ipeclass = ipathkey->pk_eclass;
			if (ieclass == ipeclass)
			{
				// 第一次匹配该内部 pathkey
				lip = lnext(lip);
				first_inner_match = true;
			}
		}
		if (!first_inner_match)
		{
			// 冗余条件，需在 lip 之前查找匹配的内部 pathkey
			ListCell   *l2;

			foreach(l2, innerpathkeys)
			{
				if (l2 == lip)
					break;
				ipathkey = (PathKey *) lfirst(l2);
				ipeclass = ipathkey->pk_eclass;
				if (ieclass == ipeclass)
					break;
			}
			if (ieclass != ipeclass)
				elog(ERROR, "inner pathkeys do not match mergeclauses");
		}

		/*
		 * pathkeys 在 opfamily/collation 上必须一致，冗余内部 pathkey 的排序可忽略。
		 * 非冗余内部 pathkey 排序必须与外部一致。
		 */
		// 验证操作符族和排序规则一致性
		if (opathkey->pk_opfamily != ipathkey->pk_opfamily ||
			opathkey->pk_eclass->ec_collation != ipathkey->pk_eclass->ec_collation)
			elog(ERROR, "left and right pathkeys do not match in mergejoin");
		// 对于非冗余内部路径键，验证排序策略一致性
		if (first_inner_match &&
			(opathkey->pk_strategy != ipathkey->pk_strategy ||
			 opathkey->pk_nulls_first != ipathkey->pk_nulls_first))
			elog(ERROR, "left and right pathkeys do not match in mergejoin");

		// 保存执行器需要的信息
		mergefamilies[i] = opathkey->pk_opfamily;
		mergecollations[i] = opathkey->pk_eclass->ec_collation;
		mergestrategies[i] = opathkey->pk_strategy;
		mergenullsfirst[i] = opathkey->pk_nulls_first;
		i++;
	}

	/*
	 * 注意：如果 pathkeys 有多余元素（lop 或 lip 非 NULL），不是错误。
	 * 输入路径可能比当前 mergejoin 需要的排序更好。
	 */

	/*
	 * 构建 MergeJoin 节点。
	 */
	join_plan = make_mergejoin(tlist,           // 目标列表
				   joinclauses,      // 普通连接条件
				   otherclauses,     // 外连接时的其他条件
				   mergeclauses,     // 合并条件
				   mergefamilies,    // 操作符族数组
				   mergecollations,  // 排序规则数组
				   mergestrategies,  // 合并策略数组
				   mergenullsfirst,  // NULL值排序策略数组
				   outer_plan,       // 外部子计划
				   inner_plan,       // 内部子计划
				   best_path->jpath.jointype,         // 连接类型
				   best_path->jpath.inner_unique,     // 内部表是否唯一
				   best_path->skip_mark_restore);     // 是否跳过mark/restore操作

	// 排序和物化的成本已包含在 path 的成本中
	// 复制路径信息到计划节点
	copy_generic_path_info(&join_plan->join.plan, &best_path->jpath.path);

	return join_plan; // 返回构建好的MergeJoin计划节点
}

static HashJoin *
create_hashjoin_plan(PlannerInfo *root,
					 HashPath *best_path)
{
	HashJoin   *join_plan;
	Hash	   *hash_plan;
	Plan	   *outer_plan;
	Plan	   *inner_plan;
	List	   *tlist = build_path_tlist(root, &best_path->jpath.path);
	List	   *joinclauses;
	List	   *otherclauses;
	List	   *hashclauses;
	List	   *hashoperators = NIL;
	List	   *hashcollations = NIL;
	List	   *inner_hashkeys = NIL;
	List	   *outer_hashkeys = NIL;
	Oid			skewTable = InvalidOid;
	AttrNumber	skewColumn = InvalidAttrNumber;
	bool		skewInherit = false;
	ListCell   *lc;

	/*
	 * HashJoin can project, so we don't have to demand exact tlists from the
	 * inputs.  However, it's best to request a small tlist from the inner
	 * side, so that we aren't storing more data than necessary.  Likewise, if
	 * we anticipate batching, request a small tlist from the outer side so
	 * that we don't put extra data in the outer batch files.
	 */
	outer_plan = create_plan_recurse(root, best_path->jpath.outerjoinpath,
									 (best_path->num_batches > 1) ? CP_SMALL_TLIST : 0);

	inner_plan = create_plan_recurse(root, best_path->jpath.innerjoinpath,
									 CP_SMALL_TLIST);

	/* Sort join qual clauses into best execution order */
	joinclauses = order_qual_clauses(root, best_path->jpath.joinrestrictinfo);
	/* There's no point in sorting the hash clauses ... */

	/* Get the join qual clauses (in plain expression form) */
	/* Any pseudoconstant clauses are ignored here */
	if (IS_OUTER_JOIN(best_path->jpath.jointype))
	{
		extract_actual_join_clauses(joinclauses,
									best_path->jpath.path.parent->relids,
									&joinclauses, &otherclauses);
	}
	else
	{
		/* We can treat all clauses alike for an inner join */
		joinclauses = extract_actual_clauses(joinclauses, false);
		otherclauses = NIL;
	}

	/*
	 * Remove the hashclauses from the list of join qual clauses, leaving the
	 * list of quals that must be checked as qpquals.
	 */
	hashclauses = get_actual_clauses(best_path->path_hashclauses);
	joinclauses = list_difference(joinclauses, hashclauses);

	/*
	 * Replace any outer-relation variables with nestloop params.  There
	 * should not be any in the hashclauses.
	 */
	if (best_path->jpath.path.param_info)
	{
		joinclauses = (List *)
			replace_nestloop_params(root, (Node *) joinclauses);
		otherclauses = (List *)
			replace_nestloop_params(root, (Node *) otherclauses);
	}

	/*
	 * Rearrange hashclauses, if needed, so that the outer variable is always
	 * on the left.
	 */
	hashclauses = get_switched_clauses(best_path->path_hashclauses,
									   best_path->jpath.outerjoinpath->parent->relids);

	/*
	 * If there is a single join clause and we can identify the outer variable
	 * as a simple column reference, supply its identity for possible use in
	 * skew optimization.  (Note: in principle we could do skew optimization
	 * with multiple join clauses, but we'd have to be able to determine the
	 * most common combinations of outer values, which we don't currently have
	 * enough stats for.)
	 */
	if (list_length(hashclauses) == 1)
	{
		OpExpr	   *clause = (OpExpr *) linitial(hashclauses);
		Node	   *node;

		Assert(is_opclause(clause));
		node = (Node *) linitial(clause->args);
		if (IsA(node, RelabelType))
			node = (Node *) ((RelabelType *) node)->arg;
		if (IsA(node, Var))
		{
			Var		   *var = (Var *) node;
			RangeTblEntry *rte;

			rte = root->simple_rte_array[var->varno];
			if (rte->rtekind == RTE_RELATION)
			{
				skewTable = rte->relid;
				skewColumn = var->varattno;
				skewInherit = rte->inh;
			}
		}
	}

	/*
	 * Collect hash related information. The hashed expressions are
	 * deconstructed into outer/inner expressions, so they can be computed
	 * separately (inner expressions are used to build the hashtable via Hash,
	 * outer expressions to perform lookups of tuples from HashJoin's outer
	 * plan in the hashtable). Also collect operator information necessary to
	 * build the hashtable.
	 */
	foreach(lc, hashclauses)
	{
		OpExpr	   *hclause = lfirst_node(OpExpr, lc);

		hashoperators = lappend_oid(hashoperators, hclause->opno);
		hashcollations = lappend_oid(hashcollations, hclause->inputcollid);
		outer_hashkeys = lappend(outer_hashkeys, linitial(hclause->args));
		inner_hashkeys = lappend(inner_hashkeys, lsecond(hclause->args));
	}

	/*
	 * Build the hash node and hash join node.
	 */
	hash_plan = make_hash(inner_plan,
						  inner_hashkeys,
						  skewTable,
						  skewColumn,
						  skewInherit);

	/*
	 * Set Hash node's startup & total costs equal to total cost of input
	 * plan; this only affects EXPLAIN display not decisions.
	 */
	copy_plan_costsize(&hash_plan->plan, inner_plan);
	hash_plan->plan.startup_cost = hash_plan->plan.total_cost;

	/*
	 * If parallel-aware, the executor will also need an estimate of the total
	 * number of rows expected from all participants so that it can size the
	 * shared hash table.
	 */
	if (best_path->jpath.path.parallel_aware)
	{
		hash_plan->plan.parallel_aware = true;
		hash_plan->rows_total = best_path->inner_rows_total;
	}

	join_plan = make_hashjoin(tlist,
							  joinclauses,
							  otherclauses,
							  hashclauses,
							  hashoperators,
							  hashcollations,
							  outer_hashkeys,
							  outer_plan,
							  (Plan *) hash_plan,
							  best_path->jpath.jointype,
							  best_path->jpath.inner_unique);

	copy_generic_path_info(&join_plan->join.plan, &best_path->jpath.path);

	return join_plan;
}


/*****************************************************************************
 *
 *	SUPPORTING ROUTINES
 *
 *****************************************************************************/

/*
 * replace_nestloop_params
 *	  将表达式中的外部关系 Vars 和 PlaceHolderVars 替换为 nestloop Params。
 *
 * 所有属于 root->curOuterRels 标识的关系的 Vars 和 PlaceHolderVars 都会被替换为 Param，
 * 并且如果尚未存在，则会将条目添加到 root->curOuterParams。
 */
static Node *
replace_nestloop_params(PlannerInfo *root, Node *expr)
{
	/* 不需要额外设置，直接递归处理表达式树 */
	return replace_nestloop_params_mutator(expr, root);
}

static Node *
replace_nestloop_params_mutator(Node *node, PlannerInfo *root)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		/* Upper-level Vars should be long gone at this point */
		Assert(var->varlevelsup == 0);
		/* If not to be replaced, we can just return the Var unmodified */
		if (!bms_is_member(var->varno, root->curOuterRels))
			return node;
		/* Replace the Var with a nestloop Param */
		return (Node *) replace_nestloop_param_var(root, var);
	}
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		/* Upper-level PlaceHolderVars should be long gone at this point */
		Assert(phv->phlevelsup == 0);

		/*
		 * Check whether we need to replace the PHV.  We use bms_overlap as a
		 * cheap/quick test to see if the PHV might be evaluated in the outer
		 * rels, and then grab its PlaceHolderInfo to tell for sure.
		 */
		if (!bms_overlap(phv->phrels, root->curOuterRels) ||
			!bms_is_subset(find_placeholder_info(root, phv, false)->ph_eval_at,
						   root->curOuterRels))
		{
			/*
			 * We can't replace the whole PHV, but we might still need to
			 * replace Vars or PHVs within its expression, in case it ends up
			 * actually getting evaluated here.  (It might get evaluated in
			 * this plan node, or some child node; in the latter case we don't
			 * really need to process the expression here, but we haven't got
			 * enough info to tell if that's the case.)  Flat-copy the PHV
			 * node and then recurse on its expression.
			 *
			 * Note that after doing this, we might have different
			 * representations of the contents of the same PHV in different
			 * parts of the plan tree.  This is OK because equal() will just
			 * match on phid/phlevelsup, so setrefs.c will still recognize an
			 * upper-level reference to a lower-level copy of the same PHV.
			 */
			PlaceHolderVar *newphv = makeNode(PlaceHolderVar);

			memcpy(newphv, phv, sizeof(PlaceHolderVar));
			newphv->phexpr = (Expr *)
				replace_nestloop_params_mutator((Node *) phv->phexpr,
												root);
			return (Node *) newphv;
		}
		/* Replace the PlaceHolderVar with a nestloop Param */
		return (Node *) replace_nestloop_param_placeholdervar(root, phv);
	}
	return expression_tree_mutator(node,
								   replace_nestloop_params_mutator,
								   (void *) root);
}

/*
 * fix_indexqual_references
 *    修正索引条件表达式，使其符合执行器对 indexqual 的要求。
 *
 * 主要任务：
 * 1. 从输入的 IndexClause 列表中提取实际的条件表达式，并去除 RestrictInfo 包装。
 * 2. 将外部关系的 Var 或 PlaceHolderVar 替换为 nestloop Param（嵌套循环参数）。
 * 3. 将索引键表达式转换为以索引列号为 varattno 的 Var 节点，而不是原始表的属性号。
 *
 * 输出参数：
 *   *stripped_indexquals_p 返回实际的条件表达式列表（去除 RestrictInfo）。
 *   *fixed_indexquals_p 返回修正后的条件表达式列表（深拷贝，避免子计划树共享）。
 */
static void
fix_indexqual_references(PlannerInfo *root, IndexPath *index_path,
						 List **stripped_indexquals_p, List **fixed_indexquals_p)
{
	IndexOptInfo *index = index_path->indexinfo;
	List *stripped_indexquals = NIL;
	List *fixed_indexquals = NIL;
	ListCell *lc;

	foreach(lc, index_path->indexclauses)
	{
		IndexClause *iclause = lfirst_node(IndexClause, lc);
		int indexcol = iclause->indexcol;
		ListCell *lc2;

		foreach(lc2, iclause->indexquals)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);
			Node *clause = (Node *) rinfo->clause;

			/* 添加原始条件（去除 RestrictInfo） */
			stripped_indexquals = lappend(stripped_indexquals, clause);

			/* 修正条件表达式（替换参数、索引键等） */
			clause = fix_indexqual_clause(root, index, indexcol,
										  clause, iclause->indexcols);
			fixed_indexquals = lappend(fixed_indexquals, clause);
		}
	}

	*stripped_indexquals_p = stripped_indexquals;
	*fixed_indexquals_p = fixed_indexquals;
}

/*
 * fix_indexorderby_references
 *	  Adjust indexorderby clauses to the form the executor's index
 *	  machinery needs.
 *
 * This is a simplified version of fix_indexqual_references.  The input is
 * bare clauses and a separate indexcol list, instead of IndexClauses.
 */
static List *
fix_indexorderby_references(PlannerInfo *root, IndexPath *index_path)
{
	IndexOptInfo *index = index_path->indexinfo;
	List	   *fixed_indexorderbys;
	ListCell   *lcc,
			   *lci;

	fixed_indexorderbys = NIL;

	forboth(lcc, index_path->indexorderbys, lci, index_path->indexorderbycols)
	{
		Node	   *clause = (Node *) lfirst(lcc);
		int			indexcol = lfirst_int(lci);

		clause = fix_indexqual_clause(root, index, indexcol, clause, NIL);
		fixed_indexorderbys = lappend(fixed_indexorderbys, clause);
	}

	return fixed_indexorderbys;
}

/*
 * fix_indexqual_clause
 *	  Convert a single indexqual clause to the form needed by the executor.
 *
 * We replace nestloop params here, and replace the index key variables
 * or expressions by index Var nodes.
 */
static Node *
fix_indexqual_clause(PlannerInfo *root, IndexOptInfo *index, int indexcol,
					 Node *clause, List *indexcolnos)
{
	/*
	 * Replace any outer-relation variables with nestloop params.
	 *
	 * This also makes a copy of the clause, so it's safe to modify it
	 * in-place below.
	 */
	clause = replace_nestloop_params(root, clause);

	if (IsA(clause, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) clause;

		/* Replace the indexkey expression with an index Var. */
		linitial(op->args) = fix_indexqual_operand(linitial(op->args),
												   index,
												   indexcol);
	}
	else if (IsA(clause, RowCompareExpr))
	{
		RowCompareExpr *rc = (RowCompareExpr *) clause;
		ListCell   *lca,
				   *lcai;

		/* Replace the indexkey expressions with index Vars. */
		Assert(list_length(rc->largs) == list_length(indexcolnos));
		forboth(lca, rc->largs, lcai, indexcolnos)
		{
			lfirst(lca) = fix_indexqual_operand(lfirst(lca),
												index,
												lfirst_int(lcai));
		}
	}
	else if (IsA(clause, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;

		/* Replace the indexkey expression with an index Var. */
		linitial(saop->args) = fix_indexqual_operand(linitial(saop->args),
													 index,
													 indexcol);
	}
	else if (IsA(clause, NullTest))
	{
		NullTest   *nt = (NullTest *) clause;

		/* Replace the indexkey expression with an index Var. */
		nt->arg = (Expr *) fix_indexqual_operand((Node *) nt->arg,
												 index,
												 indexcol);
	}
	else
		elog(ERROR, "unsupported indexqual type: %d",
			 (int) nodeTag(clause));

	return clause;
}

/*
 * fix_indexqual_operand
 *    将索引条件表达式转换为引用索引列的Var节点。
 *
 * 我们通过Var节点表示索引键，其中varno == INDEX_VAR，varattno等于
 * 索引的属性编号（索引列位置）。
 *
 * 这里的大部分代码用于进行完整性交叉检查，确保给定的表达式
 * 确实与其声明的索引列匹配。
 */
static Node *
fix_indexqual_operand(Node *node, IndexOptInfo *index, int indexcol)
{
    Var       *result;       /* 存储最终的Var节点结果 */
    int        pos;          /* 用于遍历索引列的循环变量 */
    ListCell  *indexpr_item; /* 指向索引表达式列表中项目的指针 */

    /*
     * 移除索引键上任何二进制兼容的类型转换（RelabelType）
     * 这确保了我们能够正确比较底层表达式，而不会被类型标签干扰
     */
    if (IsA(node, RelabelType))
        node = (Node *) ((RelabelType *) node)->arg;

    /* 断言：确保索引列索引在有效范围内 */
    Assert(indexcol >= 0 && indexcol < index->ncolumns);

    /*
     * 处理简单索引列的情况
     * 当indexkeys[indexcol]不为0时，表示这是一个普通的表列索引
     */
    if (index->indexkeys[indexcol] != 0)
    {
        /* 这是一个简单索引列 */
        if (IsA(node, Var) &&
            ((Var *) node)->varno == index->rel->relid &&
            ((Var *) node)->varattno == index->indexkeys[indexcol])
        {
            /* 复制原始Var节点并修改其属性以引用索引列 */
            result = (Var *) copyObject(node);
            result->varno = INDEX_VAR;      /* 将表变量号设置为特殊的INDEX_VAR值 */
            result->varattno = indexcol + 1; /* 将属性号设置为索引中的列位置(+1因为从1开始) */
            return (Node *) result;
        }
        else
            /* 如果条件不匹配，输出错误信息 */
            elog(ERROR, "index key does not match expected index column");
    }

    /*
     * 处理索引表达式的情况
     * 当indexkeys[indexcol]为0时，表示这是一个表达式索引
     * 需要在indexprs列表中查找并交叉检查表达式
     */
    indexpr_item = list_head(index->indexprs); /* 获取索引表达式列表的头部 */
    
    /* 遍历所有索引列 */
    for (pos = 0; pos < index->ncolumns; pos++)
    {
        /* 找到表达式索引列 */
        if (index->indexkeys[pos] == 0)
        {
            /* 检查索引表达式列表是否有足够的项 */
            if (indexpr_item == NULL)
                elog(ERROR, "too few entries in indexprs list");
                
            /* 当找到目标索引列时 */
            if (pos == indexcol)
            {
                Node   *indexkey; /* 当前索引表达式 */

                /* 获取索引表达式列表中的当前项 */
                indexkey = (Node *) lfirst(indexpr_item);
                
                /* 同样移除任何二进制兼容的类型转换 */
                if (indexkey && IsA(indexkey, RelabelType))
                    indexkey = (Node *) ((RelabelType *) indexkey)->arg;
                    
                /* 比较输入的节点是否与索引表达式匹配 */
                if (equal(node, indexkey))
                {
                    /* 如果匹配，则创建一个新的Var节点引用索引列 */
                    result = makeVar(INDEX_VAR, indexcol + 1,
                                     exprType(lfirst(indexpr_item)), -1,
                                     exprCollation(lfirst(indexpr_item)),
                                     0);
                    return (Node *) result;
                }
                else
                    /* 如果不匹配，输出错误信息 */
                    elog(ERROR, "index key does not match expected index column");
            }
            /* 移动到索引表达式列表中的下一项 */
            indexpr_item = lnext(indexpr_item);
        }
    }

    /* 如果执行到这里，表示找不到匹配的索引列，输出错误信息 */
    elog(ERROR, "index key does not match expected index column");
    return NULL;              /* 保留此返回值以避免编译器警告 */
}


/*
 * get_switched_clauses
 *	  Given a list of merge or hash joinclauses (as RestrictInfo nodes),
 *	  extract the bare clauses, and rearrange the elements within the
 *	  clauses, if needed, so the outer join variable is on the left and
 *	  the inner is on the right.  The original clause data structure is not
 *	  touched; a modified list is returned.  We do, however, set the transient
 *	  outer_is_left field in each RestrictInfo to show which side was which.
 */
static List *
get_switched_clauses(List *clauses, Relids outerrelids)
{
	List	   *t_list = NIL;
	ListCell   *l;

	foreach(l, clauses)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(l);
		OpExpr	   *clause = (OpExpr *) restrictinfo->clause;

		Assert(is_opclause(clause));
		if (bms_is_subset(restrictinfo->right_relids, outerrelids))
		{
			/*
			 * Duplicate just enough of the structure to allow commuting the
			 * clause without changing the original list.  Could use
			 * copyObject, but a complete deep copy is overkill.
			 */
			OpExpr	   *temp = makeNode(OpExpr);

			temp->opno = clause->opno;
			temp->opfuncid = InvalidOid;
			temp->opresulttype = clause->opresulttype;
			temp->opretset = clause->opretset;
			temp->opcollid = clause->opcollid;
			temp->inputcollid = clause->inputcollid;
			temp->args = list_copy(clause->args);
			temp->location = clause->location;
			/* Commute it --- note this modifies the temp node in-place. */
			CommuteOpExpr(temp);
			t_list = lappend(t_list, temp);
			restrictinfo->outer_is_left = false;
		}
		else
		{
			Assert(bms_is_subset(restrictinfo->left_relids, outerrelids));
			t_list = lappend(t_list, clause);
			restrictinfo->outer_is_left = true;
		}
	}
	return t_list;
}
/*
 * order_qual_clauses
 *		给定一个将在同一计划节点评估的条件列表，对其进行排序，
 *		以优化运行时的条件检查顺序。
 *
 * 当查询中使用安全屏障条件（security barrier quals）时，列表中可能包含不同安全级别的条件。
 * 较低 security_level 的条件必须排在较高 security_level 的条件之前，
 * 除非条件是 leakproof（不会泄漏数据），此时可以适当提前。
 * 如果安全级别不决定顺序，则优先按估算的执行成本排序，越便宜越靠前。
 *
 * 理想情况下，排序应同时考虑执行成本和选择性，但实际估算不准确，
 * 所以这里只按安全级别和每元组成本排序，且当估算相同时保持原有顺序。
 *
 * 虽然本函数可处理裸条件或 RestrictInfo，但建议传入 RestrictInfo，
 * 因为可复用其中缓存的成本信息。裸条件时无法应用安全级别排序，但目前不会影响 barrier quals。
 *
 * 注意：有些调用者会传入包含后续会被移除的条目，仅为让本函数能看到 RestrictInfo，
 * 所以不建议在排序时考虑选择性，否则可能做出错误决策。
 */
static List *
order_qual_clauses(PlannerInfo *root, List *clauses)
{
	typedef struct
	{
		Node	   *clause;			/* 条件表达式 */
		Cost		cost;			/* 每元组执行成本 */
		Index		security_level;	/* 安全级别 */
	} QualItem;
	int			nitems = list_length(clauses);
	QualItem   *items;
	ListCell   *lc;
	int			i;
	List	   *result;

	/* 0 或 1 个条件无需排序 */
	if (nitems <= 1)
		return clauses;

	/*
	 * 收集所有条件及其成本到数组，避免重复计算成本（尤其是裸条件）。
	 * 同时获取安全级别，以便后续排序使用。
	 */
	items = (QualItem *) palloc(nitems * sizeof(QualItem));
	i = 0;
	foreach(lc, clauses)
	{
		Node	   *clause = (Node *) lfirst(lc);
		QualCost	qcost;

		cost_qual_eval_node(&qcost, clause, root);
		items[i].clause = clause;
		items[i].cost = qcost.per_tuple;
		if (IsA(clause, RestrictInfo))
		{
			RestrictInfo *rinfo = (RestrictInfo *) clause;

			/*
			 * 如果条件是 leakproof 且成本较低（小于 10 倍 cpu_operator_cost），
			 * 则将其安全级别视为 0，可提前执行。
			 * 否则按原有安全级别排序。
			 */
			if (rinfo->leakproof && items[i].cost < 10 * cpu_operator_cost)
				items[i].security_level = 0;
			else
				items[i].security_level = rinfo->security_level;
		}
		else
			items[i].security_level = 0;
		i++;
	}

	/*
	 * 插入排序（不用 qsort，保证稳定性），按安全级别和成本升序排列。
	 * 安全级别较低的条件优先；若相同，则成本较低的优先。
	 * 成本相同时保持原有顺序。
	 */
	for (i = 1; i < nitems; i++)
	{
		QualItem	newitem = items[i];
		int			j;

		/* 将 newitem 插入已排序子数组 */
		for (j = i; j > 0; j--)
		{
			QualItem   *olditem = &items[j - 1];

			if (newitem.security_level > olditem->security_level ||
				(newitem.security_level == olditem->security_level &&
				 newitem.cost >= olditem->cost))
				break;
			items[j] = *olditem;
		}
		items[j] = newitem;
	}

	/* 转换回 List */
	result = NIL;
	for (i = 0; i < nitems; i++)
		result = lappend(result, items[i].clause);

	return result;
}

/*
 * 从 Path 节点复制成本和宽度等信息到 Plan 节点。
 * 执行器通常不会用这些信息，但 EXPLAIN 需要。
 * 同时复制并行相关标志，执行器会用到。
 */
static void
copy_generic_path_info(Plan *dest, Path *src)
{
	dest->startup_cost = src->startup_cost;
	dest->total_cost = src->total_cost;
	dest->plan_rows = src->rows;
	dest->plan_width = src->pathtarget->width;
	dest->parallel_aware = src->parallel_aware;
	dest->parallel_safe = src->parallel_safe;
}

/*
 * Copy cost and size info from a lower plan node to an inserted node.
 * (Most callers alter the info after copying it.)
 */
static void
copy_plan_costsize(Plan *dest, Plan *src)
{
	dest->startup_cost = src->startup_cost;
	dest->total_cost = src->total_cost;
	dest->plan_rows = src->plan_rows;
	dest->plan_width = src->plan_width;
	/* Assume the inserted node is not parallel-aware. */
	dest->parallel_aware = false;
	/* Assume the inserted node is parallel-safe, if child plan is. */
	dest->parallel_safe = src->parallel_safe;
}

/*
 * Some places in this file build Sort nodes that don't have a directly
 * corresponding Path node.  The cost of the sort is, or should have been,
 * included in the cost of the Path node we're working from, but since it's
 * not split out, we have to re-figure it using cost_sort().  This is just
 * to label the Sort node nicely for EXPLAIN.
 *
 * limit_tuples is as for cost_sort (in particular, pass -1 if no limit)
 */
static void
label_sort_with_costsize(PlannerInfo *root, Sort *plan, double limit_tuples)
{
	Plan	   *lefttree = plan->plan.lefttree;
	Path		sort_path;		/* dummy for result of cost_sort */

	cost_sort(&sort_path, root, NIL,
			  lefttree->total_cost,
			  lefttree->plan_rows,
			  lefttree->plan_width,
			  0.0,
			  work_mem,
			  limit_tuples);
	plan->plan.startup_cost = sort_path.startup_cost;
	plan->plan.total_cost = sort_path.total_cost;
	plan->plan.plan_rows = lefttree->plan_rows;
	plan->plan.plan_width = lefttree->plan_width;
	plan->plan.parallel_aware = false;
	plan->plan.parallel_safe = lefttree->parallel_safe;
}

/*
 * bitmap_subplan_mark_shared
 *	 Set isshared flag in bitmap subplan so that it will be created in
 *	 shared memory.
 */
static void
bitmap_subplan_mark_shared(Plan *plan)
{
	if (IsA(plan, BitmapAnd))
		bitmap_subplan_mark_shared(
								   linitial(((BitmapAnd *) plan)->bitmapplans));
	else if (IsA(plan, BitmapOr))
	{
		((BitmapOr *) plan)->isshared = true;
		bitmap_subplan_mark_shared(
								   linitial(((BitmapOr *) plan)->bitmapplans));
	}
	else if (IsA(plan, BitmapIndexScan))
		((BitmapIndexScan *) plan)->isshared = true;
	else
		elog(ERROR, "unrecognized node type: %d", nodeTag(plan));
}

/*****************************************************************************
 *
 *	PLAN NODE BUILDING ROUTINES
 *
 * 通常，这些函数不会传入原始 Path，因此由调用者负责通过 copy_generic_path_info()
 * 从 Path 填充成本和宽度字段。这样做是历史原因，但支持上面某些场景，
 * 比如构建没有完全对应 Path 节点的计划节点。绝不能在这些函数中自行计算成本，
 * 否则会和 Path 构建时的计算重复。
 *
 *****************************************************************************/

/*
 * make_seqscan
 *	 构建一个顺序扫描计划节点
 *
 * 参数：
 *	 qptlist    - 目标列列表
 *	 qpqual     - 限制条件列表
 *	 scanrelid  - 扫描的关系 ID
 *
 * 返回：
 *	 构建好的 SeqScan 节点
 */
static SeqScan *
make_seqscan(List *qptlist,
			 List *qpqual,
			 Index scanrelid)
{
	SeqScan    *node = makeNode(SeqScan);
	Plan	   *plan = &node->plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scanrelid = scanrelid;

	return node;
}

static SampleScan *
make_samplescan(List *qptlist,
				List *qpqual,
				Index scanrelid,
				TableSampleClause *tsc)
{
	SampleScan *node = makeNode(SampleScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->tablesample = tsc;

	return node;
}

static IndexScan *
make_indexscan(List *qptlist,
			   List *qpqual,
			   Index scanrelid,
			   Oid indexid,
			   List *indexqual,
			   List *indexqualorig,
			   List *indexorderby,
			   List *indexorderbyorig,
			   List *indexorderbyops,
			   ScanDirection indexscandir)
{
	IndexScan  *node = makeNode(IndexScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->indexid = indexid;
	node->indexqual = indexqual;
	node->indexqualorig = indexqualorig;
	node->indexorderby = indexorderby;
	node->indexorderbyorig = indexorderbyorig;
	node->indexorderbyops = indexorderbyops;
	node->indexorderdir = indexscandir;

	return node;
}

static IndexOnlyScan *
make_indexonlyscan(List *qptlist,
				   List *qpqual,
				   Index scanrelid,
				   Oid indexid,
				   List *indexqual,
				   List *recheckqual,
				   List *indexorderby,
				   List *indextlist,
				   ScanDirection indexscandir)
{
	IndexOnlyScan *node = makeNode(IndexOnlyScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->indexid = indexid;
	node->indexqual = indexqual;
	node->recheckqual = recheckqual;
	node->indexorderby = indexorderby;
	node->indextlist = indextlist;
	node->indexorderdir = indexscandir;

	return node;
}

static BitmapIndexScan *
make_bitmap_indexscan(Index scanrelid,
					  Oid indexid,
					  List *indexqual,
					  List *indexqualorig)
{
	BitmapIndexScan *node = makeNode(BitmapIndexScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = NIL;		/* not used */
	plan->qual = NIL;			/* not used */
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->indexid = indexid;
	node->indexqual = indexqual;
	node->indexqualorig = indexqualorig;

	return node;
}

static BitmapHeapScan *
make_bitmap_heapscan(List *qptlist,
					 List *qpqual,
					 Plan *lefttree,
					 List *bitmapqualorig,
					 Index scanrelid)
{
	BitmapHeapScan *node = makeNode(BitmapHeapScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->bitmapqualorig = bitmapqualorig;

	return node;
}

static TidScan *
make_tidscan(List *qptlist,
			 List *qpqual,
			 Index scanrelid,
			 List *tidquals)
{
	TidScan    *node = makeNode(TidScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->tidquals = tidquals;

	return node;
}

static SubqueryScan *
make_subqueryscan(List *qptlist,
				  List *qpqual,
				  Index scanrelid,
				  Plan *subplan)
{
	SubqueryScan *node = makeNode(SubqueryScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->subplan = subplan;

	return node;
}

static FunctionScan *
make_functionscan(List *qptlist,
				  List *qpqual,
				  Index scanrelid,
				  List *functions,
				  bool funcordinality)
{
	FunctionScan *node = makeNode(FunctionScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->functions = functions;
	node->funcordinality = funcordinality;

	return node;
}

static TableFuncScan *
make_tablefuncscan(List *qptlist,
				   List *qpqual,
				   Index scanrelid,
				   TableFunc *tablefunc)
{
	TableFuncScan *node = makeNode(TableFuncScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->tablefunc = tablefunc;

	return node;
}

static ValuesScan *
make_valuesscan(List *qptlist,
				List *qpqual,
				Index scanrelid,
				List *values_lists)
{
	ValuesScan *node = makeNode(ValuesScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->values_lists = values_lists;

	return node;
}

static CteScan *
make_ctescan(List *qptlist,
			 List *qpqual,
			 Index scanrelid,
			 int ctePlanId,
			 int cteParam)
{
	CteScan    *node = makeNode(CteScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->ctePlanId = ctePlanId;
	node->cteParam = cteParam;

	return node;
}

static NamedTuplestoreScan *
make_namedtuplestorescan(List *qptlist,
						 List *qpqual,
						 Index scanrelid,
						 char *enrname)
{
	NamedTuplestoreScan *node = makeNode(NamedTuplestoreScan);
	Plan	   *plan = &node->scan.plan;

	/* cost should be inserted by caller */
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->enrname = enrname;

	return node;
}

static WorkTableScan *
make_worktablescan(List *qptlist,
				   List *qpqual,
				   Index scanrelid,
				   int wtParam)
{
	WorkTableScan *node = makeNode(WorkTableScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->wtParam = wtParam;

	return node;
}

ForeignScan *
make_foreignscan(List *qptlist,
				 List *qpqual,
				 Index scanrelid,
				 List *fdw_exprs,
				 List *fdw_private,
				 List *fdw_scan_tlist,
				 List *fdw_recheck_quals,
				 Plan *outer_plan)
{
	ForeignScan *node = makeNode(ForeignScan);
	Plan	   *plan = &node->scan.plan;

	/* cost will be filled in by create_foreignscan_plan */
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = outer_plan;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->operation = CMD_SELECT;
	/* fs_server will be filled in by create_foreignscan_plan */
	node->fs_server = InvalidOid;
	node->fdw_exprs = fdw_exprs;
	node->fdw_private = fdw_private;
	node->fdw_scan_tlist = fdw_scan_tlist;
	node->fdw_recheck_quals = fdw_recheck_quals;
	/* fs_relids will be filled in by create_foreignscan_plan */
	node->fs_relids = NULL;
	/* fsSystemCol will be filled in by create_foreignscan_plan */
	node->fsSystemCol = false;

	return node;
}

static RecursiveUnion *
make_recursive_union(List *tlist,
					 Plan *lefttree,
					 Plan *righttree,
					 int wtParam,
					 List *distinctList,
					 long numGroups)
{
	RecursiveUnion *node = makeNode(RecursiveUnion);
	Plan	   *plan = &node->plan;
	int			numCols = list_length(distinctList);

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->wtParam = wtParam;

	/*
	 * convert SortGroupClause list into arrays of attr indexes and equality
	 * operators, as wanted by executor
	 */
	node->numCols = numCols;
	if (numCols > 0)
	{
		int			keyno = 0;
		AttrNumber *dupColIdx;
		Oid		   *dupOperators;
		Oid		   *dupCollations;
		ListCell   *slitem;

		dupColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);
		dupOperators = (Oid *) palloc(sizeof(Oid) * numCols);
		dupCollations = (Oid *) palloc(sizeof(Oid) * numCols);

		foreach(slitem, distinctList)
		{
			SortGroupClause *sortcl = (SortGroupClause *) lfirst(slitem);
			TargetEntry *tle = get_sortgroupclause_tle(sortcl,
													   plan->targetlist);

			dupColIdx[keyno] = tle->resno;
			dupOperators[keyno] = sortcl->eqop;
			dupCollations[keyno] = exprCollation((Node *) tle->expr);
			Assert(OidIsValid(dupOperators[keyno]));
			keyno++;
		}
		node->dupColIdx = dupColIdx;
		node->dupOperators = dupOperators;
		node->dupCollations = dupCollations;
	}
	node->numGroups = numGroups;

	return node;
}

static BitmapAnd *
make_bitmap_and(List *bitmapplans)
{
	BitmapAnd  *node = makeNode(BitmapAnd);
	Plan	   *plan = &node->plan;

	plan->targetlist = NIL;
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->bitmapplans = bitmapplans;

	return node;
}

static BitmapOr *
make_bitmap_or(List *bitmapplans)
{
	BitmapOr   *node = makeNode(BitmapOr);
	Plan	   *plan = &node->plan;

	plan->targetlist = NIL;
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->bitmapplans = bitmapplans;

	return node;
}

static NestLoop *
make_nestloop(List *tlist,
			  List *joinclauses,
			  List *otherclauses,
			  List *nestParams,
			  Plan *lefttree,
			  Plan *righttree,
			  JoinType jointype,
			  bool inner_unique)
{
	NestLoop   *node = makeNode(NestLoop);
	Plan	   *plan = &node->join.plan;

	plan->targetlist = tlist;
	plan->qual = otherclauses;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->join.jointype = jointype;
	node->join.inner_unique = inner_unique;
	node->join.joinqual = joinclauses;
	node->nestParams = nestParams;

	return node;
}

static HashJoin *
make_hashjoin(List *tlist,
			  List *joinclauses,
			  List *otherclauses,
			  List *hashclauses,
			  List *hashoperators,
			  List *hashcollations,
			  List *hashkeys,
			  Plan *lefttree,
			  Plan *righttree,
			  JoinType jointype,
			  bool inner_unique)
{
	HashJoin   *node = makeNode(HashJoin);
	Plan	   *plan = &node->join.plan;

	plan->targetlist = tlist;
	plan->qual = otherclauses;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->hashclauses = hashclauses;
	node->hashoperators = hashoperators;
	node->hashcollations = hashcollations;
	node->hashkeys = hashkeys;
	node->join.jointype = jointype;
	node->join.inner_unique = inner_unique;
	node->join.joinqual = joinclauses;

	return node;
}

static Hash *
make_hash(Plan *lefttree,
		  List *hashkeys,
		  Oid skewTable,
		  AttrNumber skewColumn,
		  bool skewInherit)
{
	Hash	   *node = makeNode(Hash);
	Plan	   *plan = &node->plan;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	node->hashkeys = hashkeys;
	node->skewTable = skewTable;
	node->skewColumn = skewColumn;
	node->skewInherit = skewInherit;

	return node;
}

static MergeJoin *
make_mergejoin(List *tlist,
			   List *joinclauses,
			   List *otherclauses,
			   List *mergeclauses,
			   Oid *mergefamilies,
			   Oid *mergecollations,
			   int *mergestrategies,
			   bool *mergenullsfirst,
			   Plan *lefttree,
			   Plan *righttree,
			   JoinType jointype,
			   bool inner_unique,
			   bool skip_mark_restore)
{
	MergeJoin  *node = makeNode(MergeJoin);
	Plan	   *plan = &node->join.plan;

	plan->targetlist = tlist;
	plan->qual = otherclauses;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->skip_mark_restore = skip_mark_restore;
	node->mergeclauses = mergeclauses;
	node->mergeFamilies = mergefamilies;
	node->mergeCollations = mergecollations;
	node->mergeStrategies = mergestrategies;
	node->mergeNullsFirst = mergenullsfirst;
	node->join.jointype = jointype;
	node->join.inner_unique = inner_unique;
	node->join.joinqual = joinclauses;

	return node;
}

/*
 * make_sort --- 构建一个 Sort 计划节点的基本函数
 *
 * 调用者必须已经构建好 sortColIdx、sortOperators、collations 和 nullsFirst 数组。
 */
static Sort *
make_sort(Plan *lefttree, int numCols,
		  AttrNumber *sortColIdx, Oid *sortOperators,
		  Oid *collations, bool *nullsFirst)
{
	Sort	   *node = makeNode(Sort);
	Plan	   *plan = &node->plan;

	/* 设置目标列列表为输入计划的 targetlist */
	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;						/* Sort 节点没有 qual 条件 */
	plan->lefttree = lefttree;				/* 输入计划节点 */
	plan->righttree = NULL;					/* Sort 节点没有右子树 */
	node->numCols = numCols;				/* 排序列数量 */
	node->sortColIdx = sortColIdx;			/* 排序列索引数组 */
	node->sortOperators = sortOperators;	/* 排序操作符数组 */
	node->collations = collations;			/* 排序用的排序规则数组 */
	node->nullsFirst = nullsFirst;			/* NULL 排序方式数组 */

	return node;
}

/*
 * prepare_sort_from_pathkeys
 *	  Prepare to sort according to given pathkeys
 *
 * This is used to set up for Sort, MergeAppend, and Gather Merge nodes.  It
 * calculates the executor's representation of the sort key information, and
 * adjusts the plan targetlist if needed to add resjunk sort columns.
 *
 * Input parameters:
 *	  'lefttree' is the plan node which yields input tuples
 *	  'pathkeys' is the list of pathkeys by which the result is to be sorted
 *	  'relids' identifies the child relation being sorted, if any
 *	  'reqColIdx' is NULL or an array of required sort key column numbers
 *	  'adjust_tlist_in_place' is true if lefttree must be modified in-place
 *
 * We must convert the pathkey information into arrays of sort key column
 * numbers, sort operator OIDs, collation OIDs, and nulls-first flags,
 * which is the representation the executor wants.  These are returned into
 * the output parameters *p_numsortkeys etc.
 *
 * When looking for matches to an EquivalenceClass's members, we will only
 * consider child EC members if they belong to given 'relids'.  This protects
 * against possible incorrect matches to child expressions that contain no
 * Vars.
 *
 * If reqColIdx isn't NULL then it contains sort key column numbers that
 * we should match.  This is used when making child plans for a MergeAppend;
 * it's an error if we can't match the columns.
 *
 * If the pathkeys include expressions that aren't simple Vars, we will
 * usually need to add resjunk items to the input plan's targetlist to
 * compute these expressions, since a Sort or MergeAppend node itself won't
 * do any such calculations.  If the input plan type isn't one that can do
 * projections, this means adding a Result node just to do the projection.
 * However, the caller can pass adjust_tlist_in_place = true to force the
 * lefttree tlist to be modified in-place regardless of whether the node type
 * can project --- we use this for fixing the tlist of MergeAppend itself.
 *
 * Returns the node which is to be the input to the Sort (either lefttree,
 * or a Result stacked atop lefttree).
 */
static Plan *
prepare_sort_from_pathkeys(Plan *lefttree, List *pathkeys,
						   Relids relids,
						   const AttrNumber *reqColIdx,
						   bool adjust_tlist_in_place,
						   int *p_numsortkeys,
						   AttrNumber **p_sortColIdx,
						   Oid **p_sortOperators,
						   Oid **p_collations,
						   bool **p_nullsFirst)
{
	List	   *tlist = lefttree->targetlist;
	ListCell   *i;
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;

	/*
	 * We will need at most list_length(pathkeys) sort columns; possibly less
	 */
	numsortkeys = list_length(pathkeys);
	sortColIdx = (AttrNumber *) palloc(numsortkeys * sizeof(AttrNumber));
	sortOperators = (Oid *) palloc(numsortkeys * sizeof(Oid));
	collations = (Oid *) palloc(numsortkeys * sizeof(Oid));
	nullsFirst = (bool *) palloc(numsortkeys * sizeof(bool));

	numsortkeys = 0;

	foreach(i, pathkeys)
	{
		PathKey    *pathkey = (PathKey *) lfirst(i);
		EquivalenceClass *ec = pathkey->pk_eclass;
		EquivalenceMember *em;
		TargetEntry *tle = NULL;
		Oid			pk_datatype = InvalidOid;
		Oid			sortop;
		ListCell   *j;

		if (ec->ec_has_volatile)
		{
			/*
			 * If the pathkey's EquivalenceClass is volatile, then it must
			 * have come from an ORDER BY clause, and we have to match it to
			 * that same targetlist entry.
			 */
			if (ec->ec_sortref == 0)	/* can't happen */
				elog(ERROR, "volatile EquivalenceClass has no sortref");
			tle = get_sortgroupref_tle(ec->ec_sortref, tlist);
			Assert(tle);
			Assert(list_length(ec->ec_members) == 1);
			pk_datatype = ((EquivalenceMember *) linitial(ec->ec_members))->em_datatype;
		}
		else if (reqColIdx != NULL)
		{
			/*
			 * If we are given a sort column number to match, only consider
			 * the single TLE at that position.  It's possible that there is
			 * no such TLE, in which case fall through and generate a resjunk
			 * targetentry (we assume this must have happened in the parent
			 * plan as well).  If there is a TLE but it doesn't match the
			 * pathkey's EC, we do the same, which is probably the wrong thing
			 * but we'll leave it to caller to complain about the mismatch.
			 */
			tle = get_tle_by_resno(tlist, reqColIdx[numsortkeys]);
			if (tle)
			{
				em = find_ec_member_for_tle(ec, tle, relids);
				if (em)
				{
					/* found expr at right place in tlist */
					pk_datatype = em->em_datatype;
				}
				else
					tle = NULL;
			}
		}
		else
		{
			/*
			 * Otherwise, we can sort by any non-constant expression listed in
			 * the pathkey's EquivalenceClass.  For now, we take the first
			 * tlist item found in the EC. If there's no match, we'll generate
			 * a resjunk entry using the first EC member that is an expression
			 * in the input's vars.  (The non-const restriction only matters
			 * if the EC is below_outer_join; but if it isn't, it won't
			 * contain consts anyway, else we'd have discarded the pathkey as
			 * redundant.)
			 *
			 * XXX if we have a choice, is there any way of figuring out which
			 * might be cheapest to execute?  (For example, int4lt is likely
			 * much cheaper to execute than numericlt, but both might appear
			 * in the same equivalence class...)  Not clear that we ever will
			 * have an interesting choice in practice, so it may not matter.
			 */
			foreach(j, tlist)
			{
				tle = (TargetEntry *) lfirst(j);
				em = find_ec_member_for_tle(ec, tle, relids);
				if (em)
				{
					/* found expr already in tlist */
					pk_datatype = em->em_datatype;
					break;
				}
				tle = NULL;
			}
		}

		if (!tle)
		{
			/*
			 * No matching tlist item; look for a computable expression. Note
			 * that we treat Aggrefs as if they were variables; this is
			 * necessary when attempting to sort the output from an Agg node
			 * for use in a WindowFunc (since grouping_planner will have
			 * treated the Aggrefs as variables, too).  Likewise, if we find a
			 * WindowFunc in a sort expression, treat it as a variable.
			 */
			Expr	   *sortexpr = NULL;

			foreach(j, ec->ec_members)
			{
				EquivalenceMember *em = (EquivalenceMember *) lfirst(j);
				List	   *exprvars;
				ListCell   *k;

				/*
				 * We shouldn't be trying to sort by an equivalence class that
				 * contains a constant, so no need to consider such cases any
				 * further.
				 */
				if (em->em_is_const)
					continue;

				/*
				 * Ignore child members unless they belong to the rel being
				 * sorted.
				 */
				if (em->em_is_child &&
					!bms_is_subset(em->em_relids, relids))
					continue;

				sortexpr = em->em_expr;
				exprvars = pull_var_clause((Node *) sortexpr,
										   PVC_INCLUDE_AGGREGATES |
										   PVC_INCLUDE_WINDOWFUNCS |
										   PVC_INCLUDE_PLACEHOLDERS);
				foreach(k, exprvars)
				{
					if (!tlist_member_ignore_relabel(lfirst(k), tlist))
						break;
				}
				list_free(exprvars);
				if (!k)
				{
					pk_datatype = em->em_datatype;
					break;		/* found usable expression */
				}
			}
			if (!j)
				elog(ERROR, "could not find pathkey item to sort");

			/*
			 * Do we need to insert a Result node?
			 */
			if (!adjust_tlist_in_place &&
				!is_projection_capable_plan(lefttree))
			{
				/* copy needed so we don't modify input's tlist below */
				tlist = copyObject(tlist);
				lefttree = inject_projection_plan(lefttree, tlist,
												  lefttree->parallel_safe);
			}

			/* Don't bother testing is_projection_capable_plan again */
			adjust_tlist_in_place = true;

			/*
			 * Add resjunk entry to input's tlist
			 */
			tle = makeTargetEntry(sortexpr,
								  list_length(tlist) + 1,
								  NULL,
								  true);
			tlist = lappend(tlist, tle);
			lefttree->targetlist = tlist;	/* just in case NIL before */
		}

		/*
		 * Look up the correct sort operator from the PathKey's slightly
		 * abstracted representation.
		 */
		sortop = get_opfamily_member(pathkey->pk_opfamily,
									 pk_datatype,
									 pk_datatype,
									 pathkey->pk_strategy);
		if (!OidIsValid(sortop))	/* should not happen */
			elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
				 pathkey->pk_strategy, pk_datatype, pk_datatype,
				 pathkey->pk_opfamily);

		/* Add the column to the sort arrays */
		sortColIdx[numsortkeys] = tle->resno;
		sortOperators[numsortkeys] = sortop;
		collations[numsortkeys] = ec->ec_collation;
		nullsFirst[numsortkeys] = pathkey->pk_nulls_first;
		numsortkeys++;
	}

	/* Return results */
	*p_numsortkeys = numsortkeys;
	*p_sortColIdx = sortColIdx;
	*p_sortOperators = sortOperators;
	*p_collations = collations;
	*p_nullsFirst = nullsFirst;

	return lefttree;
}

/*
 * find_ec_member_for_tle
 *		Locate an EquivalenceClass member matching the given TLE, if any
 *
 * Child EC members are ignored unless they belong to given 'relids'.
 */
static EquivalenceMember *
find_ec_member_for_tle(EquivalenceClass *ec,
					   TargetEntry *tle,
					   Relids relids)
{
	Expr	   *tlexpr;
	ListCell   *lc;

	/* We ignore binary-compatible relabeling on both ends */
	tlexpr = tle->expr;
	while (tlexpr && IsA(tlexpr, RelabelType))
		tlexpr = ((RelabelType *) tlexpr)->arg;

	foreach(lc, ec->ec_members)
	{
		EquivalenceMember *em = (EquivalenceMember *) lfirst(lc);
		Expr	   *emexpr;

		/*
		 * We shouldn't be trying to sort by an equivalence class that
		 * contains a constant, so no need to consider such cases any further.
		 */
		if (em->em_is_const)
			continue;

		/*
		 * Ignore child members unless they belong to the rel being sorted.
		 */
		if (em->em_is_child &&
			!bms_is_subset(em->em_relids, relids))
			continue;

		/* Match if same expression (after stripping relabel) */
		emexpr = em->em_expr;
		while (emexpr && IsA(emexpr, RelabelType))
			emexpr = ((RelabelType *) emexpr)->arg;

		if (equal(emexpr, tlexpr))
			return em;
	}

	return NULL;
}

/*
 * make_sort_from_pathkeys
 *	  Create sort plan to sort according to given pathkeys
 *
 *	  'lefttree' is the node which yields input tuples
 *	  'pathkeys' is the list of pathkeys by which the result is to be sorted
 *	  'relids' is the set of relations required by prepare_sort_from_pathkeys()
 */
static Sort *
make_sort_from_pathkeys(Plan *lefttree, List *pathkeys, Relids relids)
{
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;

	/* Compute sort column info, and adjust lefttree as needed */
	lefttree = prepare_sort_from_pathkeys(lefttree, pathkeys,
										  relids,
										  NULL,
										  false,
										  &numsortkeys,
										  &sortColIdx,
										  &sortOperators,
										  &collations,
										  &nullsFirst);

	/* Now build the Sort node */
	return make_sort(lefttree, numsortkeys,
					 sortColIdx, sortOperators,
					 collations, nullsFirst);
}

/*
 * make_sort_from_sortclauses
 *	  Create sort plan to sort according to given sortclauses
 *
 *	  'sortcls' is a list of SortGroupClauses
 *	  'lefttree' is the node which yields input tuples
 */
Sort *
make_sort_from_sortclauses(List *sortcls, Plan *lefttree)
{
	List	   *sub_tlist = lefttree->targetlist;
	ListCell   *l;
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;

	/* Convert list-ish representation to arrays wanted by executor */
	numsortkeys = list_length(sortcls);
	sortColIdx = (AttrNumber *) palloc(numsortkeys * sizeof(AttrNumber));
	sortOperators = (Oid *) palloc(numsortkeys * sizeof(Oid));
	collations = (Oid *) palloc(numsortkeys * sizeof(Oid));
	nullsFirst = (bool *) palloc(numsortkeys * sizeof(bool));

	numsortkeys = 0;
	foreach(l, sortcls)
	{
		SortGroupClause *sortcl = (SortGroupClause *) lfirst(l);
		TargetEntry *tle = get_sortgroupclause_tle(sortcl, sub_tlist);

		sortColIdx[numsortkeys] = tle->resno;
		sortOperators[numsortkeys] = sortcl->sortop;
		collations[numsortkeys] = exprCollation((Node *) tle->expr);
		nullsFirst[numsortkeys] = sortcl->nulls_first;
		numsortkeys++;
	}

	return make_sort(lefttree, numsortkeys,
					 sortColIdx, sortOperators,
					 collations, nullsFirst);
}

/*
 * make_sort_from_groupcols
 *	  Create sort plan to sort based on grouping columns
 *
 * 'groupcls' is the list of SortGroupClauses
 * 'grpColIdx' gives the column numbers to use
 *
 * This might look like it could be merged with make_sort_from_sortclauses,
 * but presently we *must* use the grpColIdx[] array to locate sort columns,
 * because the child plan's tlist is not marked with ressortgroupref info
 * appropriate to the grouping node.  So, only the sort ordering info
 * is used from the SortGroupClause entries.
 */
static Sort *
make_sort_from_groupcols(List *groupcls,
						 AttrNumber *grpColIdx,
						 Plan *lefttree)
{
	List	   *sub_tlist = lefttree->targetlist;
	ListCell   *l;
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;

	/* Convert list-ish representation to arrays wanted by executor */
	numsortkeys = list_length(groupcls);
	sortColIdx = (AttrNumber *) palloc(numsortkeys * sizeof(AttrNumber));
	sortOperators = (Oid *) palloc(numsortkeys * sizeof(Oid));
	collations = (Oid *) palloc(numsortkeys * sizeof(Oid));
	nullsFirst = (bool *) palloc(numsortkeys * sizeof(bool));

	numsortkeys = 0;
	foreach(l, groupcls)
	{
		SortGroupClause *grpcl = (SortGroupClause *) lfirst(l);
		TargetEntry *tle = get_tle_by_resno(sub_tlist, grpColIdx[numsortkeys]);

		if (!tle)
			elog(ERROR, "could not retrieve tle for sort-from-groupcols");

		sortColIdx[numsortkeys] = tle->resno;
		sortOperators[numsortkeys] = grpcl->sortop;
		collations[numsortkeys] = exprCollation((Node *) tle->expr);
		nullsFirst[numsortkeys] = grpcl->nulls_first;
		numsortkeys++;
	}

	return make_sort(lefttree, numsortkeys,
					 sortColIdx, sortOperators,
					 collations, nullsFirst);
}

static Material *
make_material(Plan *lefttree)
{
	Material   *node = makeNode(Material);
	Plan	   *plan = &node->plan;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	return node;
}

/*
 * materialize_finished_plan: stick a Material node atop a completed plan
 *
 * There are a couple of places where we want to attach a Material node
 * after completion of create_plan(), without any MaterialPath path.
 * Those places should probably be refactored someday to do this on the
 * Path representation, but it's not worth the trouble yet.
 */
Plan *
materialize_finished_plan(Plan *subplan)
{
	Plan	   *matplan;
	Path		matpath;		/* dummy for result of cost_material */

	matplan = (Plan *) make_material(subplan);

	/*
	 * XXX horrid kluge: if there are any initPlans attached to the subplan,
	 * move them up to the Material node, which is now effectively the top
	 * plan node in its query level.  This prevents failure in
	 * SS_finalize_plan(), which see for comments.  We don't bother adjusting
	 * the subplan's cost estimate for this.
	 */
	matplan->initPlan = subplan->initPlan;
	subplan->initPlan = NIL;

	/* Set cost data */
	cost_material(&matpath,
				  subplan->startup_cost,
				  subplan->total_cost,
				  subplan->plan_rows,
				  subplan->plan_width);
	matplan->startup_cost = matpath.startup_cost;
	matplan->total_cost = matpath.total_cost;
	matplan->plan_rows = subplan->plan_rows;
	matplan->plan_width = subplan->plan_width;
	matplan->parallel_aware = false;
	matplan->parallel_safe = subplan->parallel_safe;

	return matplan;
}

Agg *
make_agg(List *tlist, List *qual,
		 AggStrategy aggstrategy, AggSplit aggsplit,
		 int numGroupCols, AttrNumber *grpColIdx, Oid *grpOperators, Oid *grpCollations,
		 List *groupingSets, List *chain,
		 double dNumGroups, Plan *lefttree)
{
	Agg		   *node = makeNode(Agg);
	Plan	   *plan = &node->plan;
	long		numGroups;

	/* Reduce to long, but 'ware overflow! */
	numGroups = (long) Min(dNumGroups, (double) LONG_MAX);

	node->aggstrategy = aggstrategy;
	node->aggsplit = aggsplit;
	node->numCols = numGroupCols;
	node->grpColIdx = grpColIdx;
	node->grpOperators = grpOperators;
	node->grpCollations = grpCollations;
	node->numGroups = numGroups;
	node->aggParams = NULL;		/* SS_finalize_plan() will fill this */
	node->groupingSets = groupingSets;
	node->chain = chain;

	plan->qual = qual;
	plan->targetlist = tlist;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	return node;
}

static WindowAgg *
make_windowagg(List *tlist, Index winref,
			   int partNumCols, AttrNumber *partColIdx, Oid *partOperators, Oid *partCollations,
			   int ordNumCols, AttrNumber *ordColIdx, Oid *ordOperators, Oid *ordCollations,
			   int frameOptions, Node *startOffset, Node *endOffset,
			   Oid startInRangeFunc, Oid endInRangeFunc,
			   Oid inRangeColl, bool inRangeAsc, bool inRangeNullsFirst,
			   Plan *lefttree)
{
	WindowAgg  *node = makeNode(WindowAgg);
	Plan	   *plan = &node->plan;

	node->winref = winref;
	node->partNumCols = partNumCols;
	node->partColIdx = partColIdx;
	node->partOperators = partOperators;
	node->partCollations = partCollations;
	node->ordNumCols = ordNumCols;
	node->ordColIdx = ordColIdx;
	node->ordOperators = ordOperators;
	node->ordCollations = ordCollations;
	node->frameOptions = frameOptions;
	node->startOffset = startOffset;
	node->endOffset = endOffset;
	node->startInRangeFunc = startInRangeFunc;
	node->endInRangeFunc = endInRangeFunc;
	node->inRangeColl = inRangeColl;
	node->inRangeAsc = inRangeAsc;
	node->inRangeNullsFirst = inRangeNullsFirst;

	plan->targetlist = tlist;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	/* WindowAgg nodes never have a qual clause */
	plan->qual = NIL;

	return node;
}

static Group *
make_group(List *tlist,
		   List *qual,
		   int numGroupCols,
		   AttrNumber *grpColIdx,
		   Oid *grpOperators,
		   Oid *grpCollations,
		   Plan *lefttree)
{
	Group	   *node = makeNode(Group);
	Plan	   *plan = &node->plan;

	node->numCols = numGroupCols;
	node->grpColIdx = grpColIdx;
	node->grpOperators = grpOperators;
	node->grpCollations = grpCollations;

	plan->qual = qual;
	plan->targetlist = tlist;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	return node;
}

/*
 * distinctList is a list of SortGroupClauses, identifying the targetlist items
 * that should be considered by the Unique filter.  The input path must
 * already be sorted accordingly.
 */
static Unique *
make_unique_from_sortclauses(Plan *lefttree, List *distinctList)
{
	Unique	   *node = makeNode(Unique);
	Plan	   *plan = &node->plan;
	int			numCols = list_length(distinctList);
	int			keyno = 0;
	AttrNumber *uniqColIdx;
	Oid		   *uniqOperators;
	Oid		   *uniqCollations;
	ListCell   *slitem;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	/*
	 * convert SortGroupClause list into arrays of attr indexes and equality
	 * operators, as wanted by executor
	 */
	Assert(numCols > 0);
	uniqColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);
	uniqOperators = (Oid *) palloc(sizeof(Oid) * numCols);
	uniqCollations = (Oid *) palloc(sizeof(Oid) * numCols);

	foreach(slitem, distinctList)
	{
		SortGroupClause *sortcl = (SortGroupClause *) lfirst(slitem);
		TargetEntry *tle = get_sortgroupclause_tle(sortcl, plan->targetlist);

		uniqColIdx[keyno] = tle->resno;
		uniqOperators[keyno] = sortcl->eqop;
		uniqCollations[keyno] = exprCollation((Node *) tle->expr);
		Assert(OidIsValid(uniqOperators[keyno]));
		keyno++;
	}

	node->numCols = numCols;
	node->uniqColIdx = uniqColIdx;
	node->uniqOperators = uniqOperators;
	node->uniqCollations = uniqCollations;

	return node;
}

/*
 * as above, but use pathkeys to identify the sort columns and semantics
 */
static Unique *
make_unique_from_pathkeys(Plan *lefttree, List *pathkeys, int numCols)
{
	Unique	   *node = makeNode(Unique);
	Plan	   *plan = &node->plan;
	int			keyno = 0;
	AttrNumber *uniqColIdx;
	Oid		   *uniqOperators;
	Oid		   *uniqCollations;
	ListCell   *lc;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	/*
	 * Convert pathkeys list into arrays of attr indexes and equality
	 * operators, as wanted by executor.  This has a lot in common with
	 * prepare_sort_from_pathkeys ... maybe unify sometime?
	 */
	Assert(numCols >= 0 && numCols <= list_length(pathkeys));
	uniqColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);
	uniqOperators = (Oid *) palloc(sizeof(Oid) * numCols);
	uniqCollations = (Oid *) palloc(sizeof(Oid) * numCols);

	foreach(lc, pathkeys)
	{
		PathKey    *pathkey = (PathKey *) lfirst(lc);
		EquivalenceClass *ec = pathkey->pk_eclass;
		EquivalenceMember *em;
		TargetEntry *tle = NULL;
		Oid			pk_datatype = InvalidOid;
		Oid			eqop;
		ListCell   *j;

		/* Ignore pathkeys beyond the specified number of columns */
		if (keyno >= numCols)
			break;

		if (ec->ec_has_volatile)
		{
			/*
			 * If the pathkey's EquivalenceClass is volatile, then it must
			 * have come from an ORDER BY clause, and we have to match it to
			 * that same targetlist entry.
			 */
			if (ec->ec_sortref == 0)	/* can't happen */
				elog(ERROR, "volatile EquivalenceClass has no sortref");
			tle = get_sortgroupref_tle(ec->ec_sortref, plan->targetlist);
			Assert(tle);
			Assert(list_length(ec->ec_members) == 1);
			pk_datatype = ((EquivalenceMember *) linitial(ec->ec_members))->em_datatype;
		}
		else
		{
			/*
			 * Otherwise, we can use any non-constant expression listed in the
			 * pathkey's EquivalenceClass.  For now, we take the first tlist
			 * item found in the EC.
			 */
			foreach(j, plan->targetlist)
			{
				tle = (TargetEntry *) lfirst(j);
				em = find_ec_member_for_tle(ec, tle, NULL);
				if (em)
				{
					/* found expr already in tlist */
					pk_datatype = em->em_datatype;
					break;
				}
				tle = NULL;
			}
		}

		if (!tle)
			elog(ERROR, "could not find pathkey item to sort");

		/*
		 * Look up the correct equality operator from the PathKey's slightly
		 * abstracted representation.
		 */
		eqop = get_opfamily_member(pathkey->pk_opfamily,
								   pk_datatype,
								   pk_datatype,
								   BTEqualStrategyNumber);
		if (!OidIsValid(eqop))	/* should not happen */
			elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
				 BTEqualStrategyNumber, pk_datatype, pk_datatype,
				 pathkey->pk_opfamily);

		uniqColIdx[keyno] = tle->resno;
		uniqOperators[keyno] = eqop;
		uniqCollations[keyno] = ec->ec_collation;

		keyno++;
	}

	node->numCols = numCols;
	node->uniqColIdx = uniqColIdx;
	node->uniqOperators = uniqOperators;
	node->uniqCollations = uniqCollations;

	return node;
}

static Gather *
make_gather(List *qptlist,
			List *qpqual,
			int nworkers,
			int rescan_param,
			bool single_copy,
			Plan *subplan)
{
	Gather	   *node = makeNode(Gather);
	Plan	   *plan = &node->plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = subplan;
	plan->righttree = NULL;
	node->num_workers = nworkers;
	node->rescan_param = rescan_param;
	node->single_copy = single_copy;
	node->invisible = false;
	node->initParam = NULL;

	return node;
}

/*
 * distinctList is a list of SortGroupClauses, identifying the targetlist
 * items that should be considered by the SetOp filter.  The input path must
 * already be sorted accordingly.
 */
static SetOp *
make_setop(SetOpCmd cmd, SetOpStrategy strategy, Plan *lefttree,
		   List *distinctList, AttrNumber flagColIdx, int firstFlag,
		   long numGroups)
{
	SetOp	   *node = makeNode(SetOp);
	Plan	   *plan = &node->plan;
	int			numCols = list_length(distinctList);
	int			keyno = 0;
	AttrNumber *dupColIdx;
	Oid		   *dupOperators;
	Oid		   *dupCollations;
	ListCell   *slitem;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	/*
	 * convert SortGroupClause list into arrays of attr indexes and equality
	 * operators, as wanted by executor
	 */
	dupColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);
	dupOperators = (Oid *) palloc(sizeof(Oid) * numCols);
	dupCollations = (Oid *) palloc(sizeof(Oid) * numCols);

	foreach(slitem, distinctList)
	{
		SortGroupClause *sortcl = (SortGroupClause *) lfirst(slitem);
		TargetEntry *tle = get_sortgroupclause_tle(sortcl, plan->targetlist);

		dupColIdx[keyno] = tle->resno;
		dupOperators[keyno] = sortcl->eqop;
		dupCollations[keyno] = exprCollation((Node *) tle->expr);
		Assert(OidIsValid(dupOperators[keyno]));
		keyno++;
	}

	node->cmd = cmd;
	node->strategy = strategy;
	node->numCols = numCols;
	node->dupColIdx = dupColIdx;
	node->dupOperators = dupOperators;
	node->dupCollations = dupCollations;
	node->flagColIdx = flagColIdx;
	node->firstFlag = firstFlag;
	node->numGroups = numGroups;

	return node;
}

/*
 * make_lockrows
 *	  Build a LockRows plan node
 */
static LockRows *
make_lockrows(Plan *lefttree, List *rowMarks, int epqParam)
{
	LockRows   *node = makeNode(LockRows);
	Plan	   *plan = &node->plan;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	node->rowMarks = rowMarks;
	node->epqParam = epqParam;

	return node;
}

/*
 * make_limit
 *	  Build a Limit plan node
 */
Limit *
make_limit(Plan *lefttree, Node *limitOffset, Node *limitCount)
{
	Limit	   *node = makeNode(Limit);
	Plan	   *plan = &node->plan;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	node->limitOffset = limitOffset;
	node->limitCount = limitCount;

	return node;
}

/*
 * make_result
 *	  Build a Result plan node
 */
static Result *
make_result(List *tlist,
			Node *resconstantqual,
			Plan *subplan)
{
	Result	   *node = makeNode(Result);
	Plan	   *plan = &node->plan;

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = subplan;
	plan->righttree = NULL;
	node->resconstantqual = resconstantqual;

	return node;
}

/*
 * make_project_set
 *	  Build a ProjectSet plan node
 */
static ProjectSet *
make_project_set(List *tlist,
				 Plan *subplan)
{
	ProjectSet *node = makeNode(ProjectSet);
	Plan	   *plan = &node->plan;

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = subplan;
	plan->righttree = NULL;

	return node;
}

/*
 * make_modifytable
 *	  Build a ModifyTable plan node
 */
static ModifyTable *
make_modifytable(PlannerInfo *root,
				 CmdType operation, bool canSetTag,
				 Index nominalRelation, Index rootRelation,
				 bool partColsUpdated,
				 List *resultRelations, List *subplans, List *subroots,
				 List *withCheckOptionLists, List *returningLists,
				 List *rowMarks, OnConflictExpr *onconflict, int epqParam)
{
	ModifyTable *node = makeNode(ModifyTable);
	List	   *fdw_private_list;
	Bitmapset  *direct_modify_plans;
	ListCell   *lc;
	ListCell   *lc2;
	int			i;

	Assert(list_length(resultRelations) == list_length(subplans));
	Assert(list_length(resultRelations) == list_length(subroots));
	Assert(withCheckOptionLists == NIL ||
		   list_length(resultRelations) == list_length(withCheckOptionLists));
	Assert(returningLists == NIL ||
		   list_length(resultRelations) == list_length(returningLists));

	node->plan.lefttree = NULL;
	node->plan.righttree = NULL;
	node->plan.qual = NIL;
	/* setrefs.c will fill in the targetlist, if needed */
	node->plan.targetlist = NIL;

	node->operation = operation;
	node->canSetTag = canSetTag;
	node->nominalRelation = nominalRelation;
	node->rootRelation = rootRelation;
	node->partColsUpdated = partColsUpdated;
	node->resultRelations = resultRelations;
	node->resultRelIndex = -1;	/* will be set correctly in setrefs.c */
	node->rootResultRelIndex = -1;	/* will be set correctly in setrefs.c */
	node->plans = subplans;
	if (!onconflict)
	{
		node->onConflictAction = ONCONFLICT_NONE;
		node->onConflictSet = NIL;
		node->onConflictWhere = NULL;
		node->arbiterIndexes = NIL;
		node->exclRelRTI = 0;
		node->exclRelTlist = NIL;
	}
	else
	{
		node->onConflictAction = onconflict->action;
		node->onConflictSet = onconflict->onConflictSet;
		node->onConflictWhere = onconflict->onConflictWhere;

		/*
		 * If a set of unique index inference elements was provided (an
		 * INSERT...ON CONFLICT "inference specification"), then infer
		 * appropriate unique indexes (or throw an error if none are
		 * available).
		 */
		node->arbiterIndexes = infer_arbiter_indexes(root);

		node->exclRelRTI = onconflict->exclRelIndex;
		node->exclRelTlist = onconflict->exclRelTlist;
	}
	node->withCheckOptionLists = withCheckOptionLists;
	node->returningLists = returningLists;
	node->rowMarks = rowMarks;
	node->epqParam = epqParam;

	/*
	 * For each result relation that is a foreign table, allow the FDW to
	 * construct private plan data, and accumulate it all into a list.
	 */
	fdw_private_list = NIL;
	direct_modify_plans = NULL;
	i = 0;
	forboth(lc, resultRelations, lc2, subroots)
	{
		Index		rti = lfirst_int(lc);
		PlannerInfo *subroot = lfirst_node(PlannerInfo, lc2);
		FdwRoutine *fdwroutine;
		List	   *fdw_private;
		bool		direct_modify;

		/*
		 * If possible, we want to get the FdwRoutine from our RelOptInfo for
		 * the table.  But sometimes we don't have a RelOptInfo and must get
		 * it the hard way.  (In INSERT, the target relation is not scanned,
		 * so it's not a baserel; and there are also corner cases for
		 * updatable views where the target rel isn't a baserel.)
		 */
		if (rti < subroot->simple_rel_array_size &&
			subroot->simple_rel_array[rti] != NULL)
		{
			RelOptInfo *resultRel = subroot->simple_rel_array[rti];

			fdwroutine = resultRel->fdwroutine;
		}
		else
		{
			RangeTblEntry *rte = planner_rt_fetch(rti, subroot);

			Assert(rte->rtekind == RTE_RELATION);
			if (rte->relkind == RELKIND_FOREIGN_TABLE)
			{
				/* Check if the access to foreign tables is restricted */
				if (unlikely((restrict_nonsystem_relation_kind & RESTRICT_RELKIND_FOREIGN_TABLE) != 0))
				{
					/* there must not be built-in foreign tables */
					Assert(rte->relid >= FirstNormalObjectId);
					ereport(ERROR,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("access to non-system foreign table is restricted")));
				}

				fdwroutine = GetFdwRoutineByRelId(rte->relid);
			}
			else
				fdwroutine = NULL;
		}

		/*
		 * Try to modify the foreign table directly if (1) the FDW provides
		 * callback functions needed for that and (2) there are no local
		 * structures that need to be run for each modified row: row-level
		 * triggers on the foreign table, stored generated columns, WITH CHECK
		 * OPTIONs from parent views.
		 */
		direct_modify = false;
		if (fdwroutine != NULL &&
			fdwroutine->PlanDirectModify != NULL &&
			fdwroutine->BeginDirectModify != NULL &&
			fdwroutine->IterateDirectModify != NULL &&
			fdwroutine->EndDirectModify != NULL &&
			withCheckOptionLists == NIL &&
			!has_row_triggers(subroot, rti, operation) &&
			!has_stored_generated_columns(subroot, rti))
			direct_modify = fdwroutine->PlanDirectModify(subroot, node, rti, i);
		if (direct_modify)
			direct_modify_plans = bms_add_member(direct_modify_plans, i);

		if (!direct_modify &&
			fdwroutine != NULL &&
			fdwroutine->PlanForeignModify != NULL)
			fdw_private = fdwroutine->PlanForeignModify(subroot, node, rti, i);
		else
			fdw_private = NIL;
		fdw_private_list = lappend(fdw_private_list, fdw_private);
		i++;
	}
	node->fdwPrivLists = fdw_private_list;
	node->fdwDirectModifyPlans = direct_modify_plans;

	return node;
}

/*
 * is_projection_capable_path
 *      功能：检查给定的Path节点是否能够执行投影操作
 *
 * 参数：
 *      path - 要检查的路径节点指针
 *
 * 返回值：
 *      true  - 路径节点能够执行投影操作
 *      false - 路径节点不能执行投影操作
 *
 * 投影操作(Projection)：
 *      在查询执行过程中，将输入元组转换为输出元组的过程，包括表达式计算、列重命名、
 *      列选择等操作。能够执行投影的路径可以直接生成所需的输出格式，无需额外的Result节点。
 *
 * 实现策略：
 *      采用白名单策略，默认大多数路径类型都支持投影，只需明确列出不支持投影的路径类型。
 */
bool
is_projection_capable_path(Path *path)
{
    /* 大多数路径类型都能执行投影，因此只需列出不支持投影的类型 */
    switch (path->pathtype)
    {
        case T_Hash:        /* 哈希连接路径 */
        case T_Material:    /* 物化路径 */
        case T_Sort:        /* 排序路径 */
        case T_Unique:      /* 去重路径 */
        case T_SetOp:       /* 集合操作路径(UNION, INTERSECT等) */
        case T_LockRows:    /* 行锁定路径 */
        case T_Limit:       /* 限制路径 */
        case T_ModifyTable: /* 表修改路径 */
        case T_MergeAppend: /* 合并追加路径 */
        case T_RecursiveUnion: /* 递归联合路径 */
            return false;   /* 这些路径类型不能执行投影 */
            
        case T_Append:      /* 追加路径 */
            /*
             * Append通常不能执行投影，但如果AppendPath用于表示一个虚拟路径(dummy path)，
             * 实际生成的将是一个能够执行投影的Result节点。
             */
            return IS_DUMMY_APPEND(path);
            
        case T_ProjectSet:  /* 投影集合路径 */
            /*
             * 虽然ProjectSet肯定会执行投影，但我们仍然返回false，因为我们不希望
             * 规划器随意替换它的目标列表(target list)。集合返回函数(SRFs)必须保持在顶层。
             * 未来可能会放宽这一限制。
             */
            return false;
            
        default:            /* 其他所有路径类型 */
            break;
    }
    
    /* 默认情况下，其他所有路径类型都支持投影 */
    return true;
}


/*
 * is_projection_capable_plan
 *		Check whether a given Plan node is able to do projection.
 */
bool
is_projection_capable_plan(Plan *plan)
{
	/* Most plan types can project, so just list the ones that can't */
	switch (nodeTag(plan))
	{
		case T_Hash:
		case T_Material:
		case T_Sort:
		case T_Unique:
		case T_SetOp:
		case T_LockRows:
		case T_Limit:
		case T_ModifyTable:
		case T_Append:
		case T_MergeAppend:
		case T_RecursiveUnion:
			return false;
		case T_ProjectSet:

			/*
			 * Although ProjectSet certainly projects, say "no" because we
			 * don't want the planner to randomly replace its tlist with
			 * something else; the SRFs have to stay at top level.  This might
			 * get relaxed later.
			 */
			return false;
		default:
			break;
	}
	return true;
}
