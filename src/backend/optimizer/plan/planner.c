/*-------------------------------------------------------------------------
 *
 * planner.c
 *	  The query optimizer external interface.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/planner.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>
#include <math.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "jit/jit.h"
#include "lib/bipartite_match.h"
#include "lib/knapsack.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#ifdef OPTIMIZER_DEBUG
#include "nodes/print.h"
#endif
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/inherit.h"
#include "optimizer/optimizer.h"
#include "optimizer/paramassign.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "parser/parse_agg.h"
#include "partitioning/partdesc.h"
#include "rewrite/rewriteManip.h"
#include "storage/dsm_impl.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/* GUC parameters */
double		cursor_tuple_fraction = DEFAULT_CURSOR_TUPLE_FRACTION;
int			force_parallel_mode = FORCE_PARALLEL_OFF;
bool		parallel_leader_participation = true;

/* Hook for plugins to get control in planner() */
planner_hook_type planner_hook = NULL;

/* Hook for plugins to get control when grouping_planner() plans upper rels */
create_upper_paths_hook_type create_upper_paths_hook = NULL;


/* Expression kind codes for preprocess_expression */
#define EXPRKIND_QUAL				0
#define EXPRKIND_TARGET				1
#define EXPRKIND_RTFUNC				2
#define EXPRKIND_RTFUNC_LATERAL		3
#define EXPRKIND_VALUES				4
#define EXPRKIND_VALUES_LATERAL		5
#define EXPRKIND_LIMIT				6
#define EXPRKIND_APPINFO			7
#define EXPRKIND_PHV				8
#define EXPRKIND_TABLESAMPLE		9
#define EXPRKIND_ARBITER_ELEM		10
#define EXPRKIND_TABLEFUNC			11
#define EXPRKIND_TABLEFUNC_LATERAL	12

/* Passthrough data for standard_qp_callback */
typedef struct
{
	List	   *activeWindows;	/* active windows, if any */
	List	   *groupClause;	/* overrides parse->groupClause */
} standard_qp_extra;

/*
 * Data specific to grouping sets
 */

typedef struct
{
	List	   *rollups;
	List	   *hash_sets_idx;
	double		dNumHashGroups;
	bool		any_hashable;
	Bitmapset  *unsortable_refs;
	Bitmapset  *unhashable_refs;
	List	   *unsortable_sets;
	int		   *tleref_to_colnum_map;
} grouping_sets_data;

/*
 * Temporary structure for use during WindowClause reordering in order to be
 * able to sort WindowClauses on partitioning/ordering prefix.
 */
typedef struct
{
	WindowClause *wc;
	List	   *uniqueOrder;	/* A List of unique ordering/partitioning
								 * clauses per Window */
} WindowClauseSortData;

/* Local functions */
static Node *preprocess_expression(PlannerInfo *root, Node *expr, int kind);
static void preprocess_qual_conditions(PlannerInfo *root, Node *jtnode);
static void inheritance_planner(PlannerInfo *root);
static void grouping_planner(PlannerInfo *root, bool inheritance_update,
							 double tuple_fraction);
static grouping_sets_data *preprocess_grouping_sets(PlannerInfo *root);
static List *remap_to_groupclause_idx(List *groupClause, List *gsets,
									  int *tleref_to_colnum_map);
static void preprocess_rowmarks(PlannerInfo *root);
static double preprocess_limit(PlannerInfo *root,
							   double tuple_fraction,
							   int64 *offset_est, int64 *count_est);
static void remove_useless_groupby_columns(PlannerInfo *root);
static List *preprocess_groupclause(PlannerInfo *root, List *force);
static List *extract_rollup_sets(List *groupingSets);
static List *reorder_grouping_sets(List *groupingSets, List *sortclause);
static void standard_qp_callback(PlannerInfo *root, void *extra);
static double get_number_of_groups(PlannerInfo *root,
								   double path_rows,
								   grouping_sets_data *gd,
								   List *target_list);
static RelOptInfo *create_grouping_paths(PlannerInfo *root,
										 RelOptInfo *input_rel,
										 PathTarget *target,
										 bool target_parallel_safe,
										 const AggClauseCosts *agg_costs,
										 grouping_sets_data *gd);
static bool is_degenerate_grouping(PlannerInfo *root);
static void create_degenerate_grouping_paths(PlannerInfo *root,
											 RelOptInfo *input_rel,
											 RelOptInfo *grouped_rel);
static RelOptInfo *make_grouping_rel(PlannerInfo *root, RelOptInfo *input_rel,
									 PathTarget *target, bool target_parallel_safe,
									 Node *havingQual);
static void create_ordinary_grouping_paths(PlannerInfo *root,
										   RelOptInfo *input_rel,
										   RelOptInfo *grouped_rel,
										   const AggClauseCosts *agg_costs,
										   grouping_sets_data *gd,
										   GroupPathExtraData *extra,
										   RelOptInfo **partially_grouped_rel_p);
static void consider_groupingsets_paths(PlannerInfo *root,
										RelOptInfo *grouped_rel,
										Path *path,
										bool is_sorted,
										bool can_hash,
										grouping_sets_data *gd,
										const AggClauseCosts *agg_costs,
										double dNumGroups);
static RelOptInfo *create_window_paths(PlannerInfo *root,
									   RelOptInfo *input_rel,
									   PathTarget *input_target,
									   PathTarget *output_target,
									   bool output_target_parallel_safe,
									   WindowFuncLists *wflists,
									   List *activeWindows);
static void create_one_window_path(PlannerInfo *root,
								   RelOptInfo *window_rel,
								   Path *path,
								   PathTarget *input_target,
								   PathTarget *output_target,
								   WindowFuncLists *wflists,
								   List *activeWindows);
static RelOptInfo *create_distinct_paths(PlannerInfo *root,
										 RelOptInfo *input_rel);
static RelOptInfo *create_ordered_paths(PlannerInfo *root,
										RelOptInfo *input_rel,
										PathTarget *target,
										bool target_parallel_safe,
										double limit_tuples);
static PathTarget *make_group_input_target(PlannerInfo *root,
										   PathTarget *final_target);
static PathTarget *make_partial_grouping_target(PlannerInfo *root,
												PathTarget *grouping_target,
												Node *havingQual);
static List *postprocess_setop_tlist(List *new_tlist, List *orig_tlist);
static List *select_active_windows(PlannerInfo *root, WindowFuncLists *wflists);
static PathTarget *make_window_input_target(PlannerInfo *root,
											PathTarget *final_target,
											List *activeWindows);
static List *make_pathkeys_for_window(PlannerInfo *root, WindowClause *wc,
									  List *tlist);
static PathTarget *make_sort_input_target(PlannerInfo *root,
										  PathTarget *final_target,
										  bool *have_postponed_srfs);
static void adjust_paths_for_srfs(PlannerInfo *root, RelOptInfo *rel,
								  List *targets, List *targets_contain_srfs);
static void add_paths_to_grouping_rel(PlannerInfo *root, RelOptInfo *input_rel,
									  RelOptInfo *grouped_rel,
									  RelOptInfo *partially_grouped_rel,
									  const AggClauseCosts *agg_costs,
									  grouping_sets_data *gd,
									  double dNumGroups,
									  GroupPathExtraData *extra);
static RelOptInfo *create_partial_grouping_paths(PlannerInfo *root,
												 RelOptInfo *grouped_rel,
												 RelOptInfo *input_rel,
												 grouping_sets_data *gd,
												 GroupPathExtraData *extra,
												 bool force_rel_creation);
static void gather_grouping_paths(PlannerInfo *root, RelOptInfo *rel);
static bool can_partial_agg(PlannerInfo *root,
							const AggClauseCosts *agg_costs);
static void apply_scanjoin_target_to_paths(PlannerInfo *root,
										   RelOptInfo *rel,
										   List *scanjoin_targets,
										   List *scanjoin_targets_contain_srfs,
										   bool scanjoin_target_parallel_safe,
										   bool tlist_same_exprs);
static void create_partitionwise_grouping_paths(PlannerInfo *root,
												RelOptInfo *input_rel,
												RelOptInfo *grouped_rel,
												RelOptInfo *partially_grouped_rel,
												const AggClauseCosts *agg_costs,
												grouping_sets_data *gd,
												PartitionwiseAggregateType patype,
												GroupPathExtraData *extra);
static bool group_by_has_partkey(RelOptInfo *input_rel,
								 List *targetList,
								 List *groupClause);
static int	common_prefix_cmp(const void *a, const void *b);


/*****************************************************************************
 *
 *	   Query optimizer entry point
 *
 * To support loadable plugins that monitor or modify planner behavior,
 * we provide a hook variable that lets a plugin get control before and
 * after the standard planning process.  The plugin would normally call
 * standard_planner().
 *
 * Note to plugin authors: standard_planner() scribbles on its Query input,
 * so you'd better copy that data structure if you want to plan more than once.
 *
 *****************************************************************************/
PlannedStmt *
planner(Query *parse, int cursorOptions, ParamListInfo boundParams)
{
	PlannedStmt *result;

	if (planner_hook)
		result = (*planner_hook) (parse, cursorOptions, boundParams);
	else
		result = standard_planner(parse, cursorOptions, boundParams);
	return result;
}

PlannedStmt*
standard_planner(Query* parse, int cursorOptions, ParamListInfo boundParams)
{
	/* 声明主要变量：
 	 * - result: 存储最终的规划语句
 	 * - glob: 规划器全局状态信息
 	 * - tuple_fraction: 估计需要扫描的元组比例
 	 * - root: 规划器信息结构
 	 * - final_rel: 最终关系优化信息
 	 * - best_path: 最优访问路径
 	 * - top_plan: 顶层执行计划节点
 	 * - lp, lr: 列表遍历指针
 	 */
	PlannedStmt* result;
	PlannerGlobal* glob;
	double		tuple_fraction;
	PlannerInfo* root;
	RelOptInfo* final_rel;
	Path* best_path;
	Plan* top_plan;
	ListCell* lp,
		* lr;

	/*
	 * 阶段1：初始化规划器全局状态
	 * 这些数据在整个查询（包括可能存在的所有子查询级别）中都需要使用，
	 * 因此维护在单独的结构体中，每个查询的PlannerInfo都会链接到这个全局结构体
	 */
	glob = makeNode(PlannerGlobal); /* 创建新的全局规划状态 */

	/* 初始化全局状态的各个字段 */
	glob->boundParams = boundParams;       /* 绑定的参数信息 */
	glob->subplans = NIL;                 /* 子计划列表初始化为空 */
	glob->subroots = NIL;                 /* 子查询的PlannerInfo列表 */
	glob->rewindPlanIDs = NULL;           /* 需要倒回的计划节点ID */
	glob->finalrtable = NIL;              /* 最终的范围表 */
	glob->finalrowmarks = NIL;            /* 最终的行标记列表 */
	glob->resultRelations = NIL;          /* 结果关系列表 */
	glob->rootResultRelations = NIL;      /* 根结果关系列表 */
	glob->relationOids = NIL;             /* 查询涉及的关系OID列表 */
	glob->invalItems = NIL;               /* 失效项目列表 */
	glob->paramExecTypes = NIL;           /* 参数执行类型列表 */
	glob->lastPHId = 0;                   /* 最后分配的PHV（PlanRowMark）ID */
	glob->lastRowMarkId = 0;              /* 最后分配的行标记ID */
	glob->lastPlanNodeId = 0;             /* 最后分配的计划节点ID */
	glob->transientPlan = false;          /* 标记是否为临时计划 */
	glob->dependsOnRole = false;          /* 标记计划是否依赖于当前角色 */

	/*
	 * 阶段2：评估是否可以使用并行模式执行查询
	 * 以下情况不允许使用并行模式：
	 * - 在独立后端进程中运行（非Postmaster管理）
	 * - 命令会修改数据
	 * - 是游标操作（除非指定PARALLEL_OK）
	 * - GUC参数设置不允许并行
	 * - 查询树中存在并行不安全函数
	 *
	 * 注意：CREATE TABLE AS、SELECT INTO和CREATE MATERIALIZED VIEW允许使用并行计划，
	 * 因为它们写入的是全新的表，工作进程无法看到这些表。如果工作进程能看到表，
	 * 由于组锁机制会导致它们忽略领导者的重量级关系扩展锁和GIN页面锁，这将不安全。
	 *
	 * 目前，如果已经在并行工作进程中运行，则不尝试使用并行模式。未来可能会放宽此限制，
	 * 但现在最好避免并行工作进程创建自己的并行工作进程。
	 */
	if ((cursorOptions & CURSOR_OPT_PARALLEL_OK) != 0 &&  /* 游标允许并行 */
		IsUnderPostmaster &&                             /* 在Postmaster管理下运行 */
		parse->commandType == CMD_SELECT &&              /* 是SELECT命令 */
		!parse->hasModifyingCTE &&                       /* 不包含修改数据的CTE */
		max_parallel_workers_per_gather > 0 &&           /* 允许每个Gather的并行工作进程数>0 */
		!IsParallelWorker())                             /* 不在并行工作进程中 */
	{
		/* 快速检查通过，现在需要扫描整个查询树评估并行安全性 */
		glob->maxParallelHazard = max_parallel_hazard(parse);
		/* 只有当没有并行不安全操作时才允许并行模式 */
		glob->parallelModeOK = (glob->maxParallelHazard != PROPARALLEL_UNSAFE);
	}
	else 
	{
		/* 跳过查询树扫描，直接假设不安全 */
		glob->maxParallelHazard = PROPARALLEL_UNSAFE;
		glob->parallelModeOK = false;
	}

	/*
	 * 阶段3：确定是否需要并行模式
	 * 通常，glob->parallelModeNeeded在这里设置为false，只有在实际创建Gather或Gather Merge计划时才会更改为true
	 *
	 * 但是，如果force_parallel_mode设置为on或regress，只要安全，我们就会强制使用并行模式，
	 * 即使最终计划不使用并行性。如果查询包含任何并行不安全的内容，这样做是不安全的；
	 * 在这种情况下，parallelModeOK将为false。
	 *
	 * 否则，查询中的所有内容都是并行安全或并行受限的，在这两种情况下，强制并行模式限制应该是安全的。
	 * 如果这导致问题，则要么用户在查询中包含的某个函数被错误标记为并行安全或并行受限（实际上它是并行不安全的），
	 * 要么查询规划器本身存在错误。
	 */
	glob->parallelModeNeeded = glob->parallelModeOK &&
		(force_parallel_mode != FORCE_PARALLEL_OFF);

	/*
	 * 阶段4：确定计划可能扫描的元组比例
	 * 这会影响查询规划器对不同访问路径的成本估计，尤其是对于游标操作
	 */
	if (cursorOptions & CURSOR_OPT_FAST_PLAN) 
	{
		/*
		 * 对于游标，我们无法确切知道用户最终会FETCH多少元组，
		 * 但通常用户不需要所有元组，或者希望有一个快速启动的计划以便更快地处理部分元组
		 * 使用GUC参数cursor_tuple_fraction来决定优化的比例
		 */
		tuple_fraction = cursor_tuple_fraction;

		/*
		 * cursor_tuple_fraction文档说明它只是一个分数，这意味着边界情况0和1需要特殊处理
		 * - 将1转换为0（表示"所有元组"）
		 * - 将0转换为一个很小的分数（表示只需要少量元组）
		 */
		if (tuple_fraction >= 1.0)
			tuple_fraction = 0.0;
		else if (tuple_fraction <= 0.0)
			tuple_fraction = 1e-10;  /* 一个很小的非零值 */
	}
	else 
	{
		/* 默认假设需要所有元组 */
		tuple_fraction = 0.0;
	}


	/* 主要规划入口点（可能递归处理子查询） */
	root = subquery_planner(glob, parse, NULL,
		false, tuple_fraction);

	/* 选择最佳 Path 并将其转换为 Plan */
	final_rel = fetch_upper_rel(root, UPPERREL_FINAL, NULL);
	best_path = get_cheapest_fractional_path(final_rel, tuple_fraction);

	top_plan = create_plan(root, best_path);

	/*
	 * If creating a plan for a scrollable cursor, make sure it can run
	 * backwards on demand.  Add a Material node at the top at need.
	 */
	if (cursorOptions & CURSOR_OPT_SCROLL) 
	{
		if (!ExecSupportsBackwardScan(top_plan))
			top_plan = materialize_finished_plan(top_plan);
	}

	/*
	 * Optionally add a Gather node for testing purposes, provided this is
	 * actually a safe thing to do.
	 */
	if (force_parallel_mode != FORCE_PARALLEL_OFF && top_plan->parallel_safe)
	{
		Gather	   *gather = makeNode(Gather);

		/*
		 * Top plan must not have any initPlans, else it shouldn't have been
		 * marked parallel-safe.
		 */
		Assert(top_plan->initPlan == NIL);

		gather->plan.targetlist = top_plan->targetlist;
		gather->plan.qual = NIL;
		gather->plan.lefttree = top_plan;
		gather->plan.righttree = NULL;
		gather->num_workers = 1;
		gather->single_copy = true;
		gather->invisible = (force_parallel_mode == FORCE_PARALLEL_REGRESS);

		/*
		 * Since this Gather has no parallel-aware descendants to signal to,
		 * we don't need a rescan Param.
		 */
		gather->rescan_param = -1;

		/*
		 * Ideally we'd use cost_gather here, but setting up dummy path data
		 * to satisfy it doesn't seem much cleaner than knowing what it does.
		 */
		gather->plan.startup_cost = top_plan->startup_cost +
			parallel_setup_cost;
		gather->plan.total_cost = top_plan->total_cost +
			parallel_setup_cost + parallel_tuple_cost * top_plan->plan_rows;
		gather->plan.plan_rows = top_plan->plan_rows;
		gather->plan.plan_width = top_plan->plan_width;
		gather->plan.parallel_aware = false;
		gather->plan.parallel_safe = false;

		/* use parallel mode for parallel plans. */
		root->glob->parallelModeNeeded = true;

		top_plan = &gather->plan;
	}

	/*
	 * If any Params were generated, run through the plan tree and compute
	 * each plan node's extParam/allParam sets.  Ideally we'd merge this into
	 * set_plan_references' tree traversal, but for now it has to be separate
	 * because we need to visit subplans before not after main plan.
	 */
	if (glob->paramExecTypes != NIL)
	{
		Assert(list_length(glob->subplans) == list_length(glob->subroots));
		forboth(lp, glob->subplans, lr, glob->subroots)
		{
			Plan	   *subplan = (Plan *) lfirst(lp);
			PlannerInfo *subroot = lfirst_node(PlannerInfo, lr);

			SS_finalize_plan(subroot, subplan);
		}
		SS_finalize_plan(root, top_plan);
	}

	/* final cleanup of the plan */
	Assert(glob->finalrtable == NIL);
	Assert(glob->finalrowmarks == NIL);
	Assert(glob->resultRelations == NIL);
	Assert(glob->rootResultRelations == NIL);
	top_plan = set_plan_references(root, top_plan);
	/* ... and the subplans (both regular subplans and initplans) */
	Assert(list_length(glob->subplans) == list_length(glob->subroots));
	forboth(lp, glob->subplans, lr, glob->subroots)
	{
		Plan	   *subplan = (Plan *) lfirst(lp);
		PlannerInfo *subroot = lfirst_node(PlannerInfo, lr);

		lfirst(lp) = set_plan_references(subroot, subplan);
	}

	/* build the PlannedStmt result */
	result = makeNode(PlannedStmt);

	result->commandType = parse->commandType;
	result->queryId = parse->queryId;
	result->hasReturning = (parse->returningList != NIL);
	result->hasModifyingCTE = parse->hasModifyingCTE;
	result->canSetTag = parse->canSetTag;
	result->transientPlan = glob->transientPlan;
	result->dependsOnRole = glob->dependsOnRole;
	result->parallelModeNeeded = glob->parallelModeNeeded;
	result->planTree = top_plan;
	result->rtable = glob->finalrtable;
	result->resultRelations = glob->resultRelations;
	result->rootResultRelations = glob->rootResultRelations;
	result->subplans = glob->subplans;
	result->rewindPlanIDs = glob->rewindPlanIDs;
	result->rowMarks = glob->finalrowmarks;
	result->relationOids = glob->relationOids;
	result->invalItems = glob->invalItems;
	result->paramExecTypes = glob->paramExecTypes;
	/* utilityStmt should be null, but we might as well copy it */
	result->utilityStmt = parse->utilityStmt;
	result->stmt_location = parse->stmt_location;
	result->stmt_len = parse->stmt_len;

	result->jitFlags = PGJIT_NONE;
	if (jit_enabled && jit_above_cost >= 0 &&
		top_plan->total_cost > jit_above_cost)
	{
		result->jitFlags |= PGJIT_PERFORM;

		/*
		 * Decide how much effort should be put into generating better code.
		 */
		if (jit_optimize_above_cost >= 0 &&
			top_plan->total_cost > jit_optimize_above_cost)
			result->jitFlags |= PGJIT_OPT3;
		if (jit_inline_above_cost >= 0 &&
			top_plan->total_cost > jit_inline_above_cost)
			result->jitFlags |= PGJIT_INLINE;

		/*
		 * Decide which operations should be JITed.
		 */
		if (jit_expressions)
			result->jitFlags |= PGJIT_EXPR;
		if (jit_tuple_deforming)
			result->jitFlags |= PGJIT_DEFORM;
	}

	if (glob->partition_directory != NULL)
		DestroyPartitionDirectory(glob->partition_directory);

	return result;
}

/*--------------------
 * subquery_planner
 *      对子查询调用规划器。对于查询树中的每个子SELECT，我们都会递归调用此函数。
 *
 * 参数说明：
 * - glob: 当前规划器运行的全局状态
 * - parse: 由解析器和重写器生成的查询树
 * - parent_root: 直接父查询的信息（顶级查询时为NULL）
 * - hasRecursion: 如果是递归WITH查询则为true
 * - tuple_fraction: 期望检索的元组比例（用于成本计算优化）
 *
 * 主要功能：
 * 1. 执行每个Query对象只需执行一次的操作
 * 2. 调用grouping_planner进行后续规划
 * 3. 递归处理查询表达式和范围表中的子Query节点
 *
 * 返回值：
 * 返回包含规划子查询时生成的所有数据的PlannerInfo结构体("root")。特别是，
 * 附加到(UPPERREL_FINAL, NULL) upperrel的Path表示我们关于实现查询的最经济方法的结论。
 * 顶层将选择最佳Path并通过createplan.c生成最终Plan。
 *--------------------
 */
PlannerInfo *
subquery_planner(PlannerGlobal *glob, Query *parse,
                 PlannerInfo *parent_root,
                 bool hasRecursion, double tuple_fraction)
{
    PlannerInfo *root;       /* 规划器信息结构，存储所有规划相关信息 */
    List       *newWithCheckOptions; /* 处理后的WITH检查选项列表 */
    List       *newHaving;   /* 处理后的HAVING子句列表 */
    bool        hasOuterJoins; /* 是否包含外部连接 */
    bool        hasResultRTEs; /* 是否包含结果RTE */
    RelOptInfo *final_rel;   /* 最终关系的优化信息 */
    ListCell   *l;           /* 用于遍历列表的单元格指针 */

    /* 创建此子查询的PlannerInfo数据结构 */
    root = makeNode(PlannerInfo); /* 使用PostgreSQL节点创建宏 */
    root->parse = parse;           /* 设置解析树 */
    root->glob = glob;             /* 设置全局状态 */
    /* 计算查询级别：如果有父查询则+1，否则为顶层查询(级别1) */
    root->query_level = parent_root ? parent_root->query_level + 1 : 1;
    root->parent_root = parent_root; /* 设置父查询信息指针 */
    root->plan_params = NIL;       /* 规划时的参数列表（初始化为空） */
    root->outer_params = NULL;     /* 外部参数位图（初始化为空） */
    root->planner_cxt = CurrentMemoryContext; /* 设置内存上下文 */
    root->init_plans = NIL;        /* 初始计划列表（用于WITH子查询等） */
    root->cte_plan_ids = NIL;      /* CTE计划ID列表 */
    root->multiexpr_params = NIL;  /* 多表达式参数列表 */
    root->eq_classes = NIL;        /* 等价类列表（用于谓词重写和优化） */
    root->append_rel_list = NIL;   /* 追加关系列表（用于分区表处理） */
    root->rowMarks = NIL;          /* 行标记信息（用于FOR UPDATE等） */
    /* 初始化上层关系和目标数组（使用memset清零） */
    memset(root->upper_rels, 0, sizeof(root->upper_rels));
    memset(root->upper_targets, 0, sizeof(root->upper_targets));
    root->processed_tlist = NIL;   /* 已处理的目标列表 */
    root->grouping_map = NULL;     /* 分组映射（用于分组优化） */
    root->minmax_aggs = NIL;       /* 最小/最大聚合信息 */
    root->qual_security_level = 0; /* 谓词安全级别（初始为最低级别） */
    root->inhTargetKind = INHKIND_NONE; /* 继承目标类型（初始无继承） */
    root->hasRecursion = hasRecursion; /* 设置递归标志 */
    /* 为递归查询分配特殊执行参数ID */
    if (hasRecursion)
        root->wt_param_id = assign_special_exec_param(root);
    else
        root->wt_param_id = -1;
    root->non_recursive_path = NULL; /* 非递归路径（用于递归CTE） */
    root->partColsUpdated = false;   /* 分区列是否被更新（初始为否） */

    /*
     * 如果有WITH列表，处理每个WITH查询，将其转换为RTE_SUBQUERY或
     * 构建initplan SubPlan结构。
     */
    if (parse->cteList)
        SS_process_ctes(root);

    /*
     * 如果FROM子句为空，将其替换为虚拟RTE_RESULT条目，
     * 这样我们就不需要处理太多特殊情况。
     */
    replace_empty_jointree(parse);

    /*
     * 在WHERE和JOIN/ON子句中查找ANY和EXISTS类型的SubLink，
     * 并尝试将其上拉转换为连接（join）。
     * 注意：此步骤不会递归处理子查询；如果后续拉升了子查询，
     * 其内部的SubLink会在拉升前被处理。
     */
    if (parse->hasSubLinks)
        pull_up_sublinks(root);

    /*
     * 扫描范围表以查找集合返回函数，并在可能的情况下内联它们
     * （生成可能在下一步被上拉的子查询）。递归问题的处理方式与SubLinks相同。
     */
    inline_set_returning_functions(root);

    /*
     * 检查连接树中的子查询是否可以合并到此查询中。
     * 这一步尝试将相关子查询提升到主查询级别以优化执行。
     */
    pull_up_subqueries(root);

    /*
     * 如果是简单的UNION ALL查询，将其展平为appendrel。
     * 我们现在执行此操作是因为它需要对UNION ALL的叶查询应用pull_up_subqueries，
     * 这些叶查询之前没有被处理，因为它们没有被连接树引用（在我们执行此操作后会被引用）。
     */
    if (parse->setOperations)
        flatten_simple_union_all(root);

    /*
     * 调查范围表中存在的条目类型。如果未使用相关SQL功能，我们可以跳过一些后续处理；
     * 例如，如果没有JOIN RTE，我们可以避免执行flatten_join_alias_vars()的开销。
     * 当然，这必须在我们完成添加范围表条目后进行。
     * （注意：实际上，处理继承或分区关系可能导致稍后添加其子表的RTE；
     * 但这些都必须是RTE_RELATION条目，因此不会使这里得出的结论无效。）
     */
    root->hasJoinRTEs = false;    /* 是否有连接RTE */
    root->hasLateralRTEs = false; /* 是否有LATERAL RTE */
    hasOuterJoins = false;        /* 是否有外部连接 */
    hasResultRTEs = false;        /* 是否有结果RTE */
    foreach(l, parse->rtable)     /* 遍历范围表中的所有条目 */
    {
        RangeTblEntry *rte = lfirst_node(RangeTblEntry, l);

        switch (rte->rtekind)     /* 根据RTE类型进行不同处理 */
        {
            case RTE_RELATION:    /* 普通关系表 */
                if (rte->inh)     /* 检查是否标记为继承 */
                {
                    /*
                     * 检查该关系是否实际有任何子表；如果没有，清除inh标志，
                     * 这样我们就可以将其视为普通基本关系。
                     *
                     * 注意：如果该关系曾经有子表但现在没有，则可能会给出假阳性结果。
                     * 我们过去能够在发现时清除rte->inh，但现在不再可以；
                     * 我们必须将此类情况视为完整的继承关系。
                     */
                    rte->inh = has_subclass(rte->relid);
                }
                break;
            case RTE_JOIN:        /* 连接关系 */
                root->hasJoinRTEs = true;
                if (IS_OUTER_JOIN(rte->jointype))
                    hasOuterJoins = true;
                break;
            case RTE_RESULT:      /* 结果关系 */
                hasResultRTEs = true;
                break;
            default:
                /* 其他RTE类型无需在此处理 */
                break;
        }

        /* 检查是否为LATERAL RTE */
        if (rte->lateral)
            root->hasLateralRTEs = true;

        /*
         * 我们还可以现在确定任何securityQuals所需的最大安全级别。
         * 添加继承子RTE不会影响这一点，因为子表没有自己的securityQuals；
         * 请参阅expand_single_inheritance_child()。
         */
        if (rte->securityQuals)
            root->qual_security_level = Max(root->qual_security_level,
                                          list_length(rte->securityQuals));
    }

    /*
     * 预处理RowMark信息。我们需要在子查询上拉后执行此操作，
     * 以确保所有基本关系都已存在。
     * RowMark用于处理FOR UPDATE等锁定操作。
     */
    preprocess_rowmarks(root);

    /*
     * 设置hasHavingQual以记住是否存在HAVING子句。
     * 这是必要的，因为preprocess_expression会将常量为true的条件简化为空谓词列表，
     * 但"HAVING TRUE"在语义上不是无操作。
     */
    root->hasHavingQual = (parse->havingQual != NULL);

    /* 清除此标志；可能会在distribute_qual_to_rels中设置 */
    root->hasPseudoConstantQuals = false;

    /*
     * 对目标列表和谓词以及查询树中的其他表达式进行预处理。
     * 注意，我们不需要显式处理排序/分组表达式，因为它们实际上是目标列表的一部分。
     */
    parse->targetList = (List *)
        preprocess_expression(root, (Node *) parse->targetList,
                             EXPRKIND_TARGET);

    /* 常量折叠可能已删除所有集合返回函数 */
    if (parse->hasTargetSRFs)
        parse->hasTargetSRFs = expression_returns_set((Node *) parse->targetList);

    /* 预处理WITH检查选项 */
    newWithCheckOptions = NIL;
    foreach(l, parse->withCheckOptions)
    {
        WithCheckOption *wco = lfirst_node(WithCheckOption, l);

        wco->qual = preprocess_expression(root, wco->qual,
                                         EXPRKIND_QUAL);
        if (wco->qual != NULL)
            newWithCheckOptions = lappend(newWithCheckOptions, wco);
    }
    parse->withCheckOptions = newWithCheckOptions;

    /* 预处理RETURNING列表 */
    parse->returningList = (List *)
        preprocess_expression(root, (Node *) parse->returningList,
                             EXPRKIND_TARGET);

    /* 预处理连接树中的谓词条件 */
    preprocess_qual_conditions(root, (Node *) parse->jointree);

    /* 预处理HAVING子句 */
    parse->havingQual = preprocess_expression(root, parse->havingQual,
                                            EXPRKIND_QUAL);

    /* 预处理窗口子句中的偏移表达式 */
    foreach(l, parse->windowClause)
    {
        WindowClause *wc = lfirst_node(WindowClause, l);

        /* partitionClause/orderClause是排序/分组表达式 */
        wc->startOffset = preprocess_expression(root, wc->startOffset,
                                              EXPRKIND_LIMIT);
        wc->endOffset = preprocess_expression(root, wc->endOffset,
                                            EXPRKIND_LIMIT);
    }

    /* 预处理LIMIT子句 */
    parse->limitOffset = preprocess_expression(root, parse->limitOffset,
                                             EXPRKIND_LIMIT);
    parse->limitCount = preprocess_expression(root, parse->limitCount,
                                            EXPRKIND_LIMIT);

    /* 预处理ON CONFLICT子句（用于INSERT ON CONFLICT语句） */
    if (parse->onConflict)
    {
        parse->onConflict->arbiterElems = (List *)
            preprocess_expression(root,
                                 (Node *) parse->onConflict->arbiterElems,
                                 EXPRKIND_ARBITER_ELEM);
        parse->onConflict->arbiterWhere =
            preprocess_expression(root,
                                 parse->onConflict->arbiterWhere,
                                 EXPRKIND_QUAL);
        parse->onConflict->onConflictSet = (List *)
            preprocess_expression(root,
                                 (Node *) parse->onConflict->onConflictSet,
                                 EXPRKIND_TARGET);
        parse->onConflict->onConflictWhere =
            preprocess_expression(root,
                                 parse->onConflict->onConflictWhere,
                                 EXPRKIND_QUAL);
        /* exclRelTlist仅包含Vars，因此不需要预处理 */
    }

    /* 预处理append_rel_list（追加关系列表） */
    root->append_rel_list = (List *)
        preprocess_expression(root, (Node *) root->append_rel_list,
                             EXPRKIND_APPINFO);

    /* 还需要预处理RTE内部的表达式 */
    foreach(l, parse->rtable)
    {
        RangeTblEntry *rte = lfirst_node(RangeTblEntry, l);
        int           kind;        /* 表达式类型 */
        ListCell     *lcsq;        /* 用于遍历securityQuals的单元格 */

        switch (rte->rtekind)
        {
            case RTE_RELATION:    /* 普通关系表 */
                if (rte->tablesample)
                    rte->tablesample = (TableSampleClause *)
                        preprocess_expression(root,
                                             (Node *) rte->tablesample,
                                             EXPRKIND_TABLESAMPLE);
                break;
            case RTE_SUBQUERY:    /* 子查询 */
                /*
                 * 我们不想现在就对子查询的表达式进行所有预处理，因为这会在规划它时发生。
                 * 但是，如果它包含我们级别的任何连接别名，这些必须现在展开，
                 * 因为子查询的规划不会这样做。这只有在子查询是LATERAL时才可能发生。
                 */
                if (rte->lateral && root->hasJoinRTEs)
                    rte->subquery = (Query *)
                        flatten_join_alias_vars(root->parse,
                                              (Node *) rte->subquery);
                break;
            case RTE_FUNCTION:    /* 函数 */
                /* 完全预处理函数表达式 */
                kind = rte->lateral ? EXPRKIND_RTFUNC_LATERAL : EXPRKIND_RTFUNC;
                rte->functions = (List *)
                    preprocess_expression(root, (Node *) rte->functions, kind);
                break;
            case RTE_TABLEFUNC:   /* 表函数 */
                /* 完全预处理函数表达式 */
                kind = rte->lateral ? EXPRKIND_TABLEFUNC_LATERAL : EXPRKIND_TABLEFUNC;
                rte->tablefunc = (TableFunc *)
                    preprocess_expression(root, (Node *) rte->tablefunc, kind);
                break;
            case RTE_VALUES:      /* VALUES列表 */
                /* 完全预处理VALUES列表 */
                kind = rte->lateral ? EXPRKIND_VALUES_LATERAL : EXPRKIND_VALUES;
                rte->values_lists = (List *)
                    preprocess_expression(root, (Node *) rte->values_lists, kind);
                break;
        }

        /*
         * 处理securityQuals列表中的每个元素，就好像它是一个单独的谓词表达式一样（实际上它就是）。
         * 我们需要这样做以获得AND/OR结构的正确规范化。
         * 注意，这会将每个元素转换为隐式AND子列表。
         */
        foreach(lcsq, rte->securityQuals)
        {
            lfirst(lcsq) = preprocess_expression(root,
                                               (Node *) lfirst(lcsq),
                                               EXPRKIND_QUAL);
        }
    }

    /*
     * 既然我们已经完成了表达式预处理，特别是完成了连接别名变量的展开，
     * 就可以去掉joinaliasvars列表了。它们不再与树的其余部分中的表达式匹配，
     * 因为我们没有预处理这些列表中的表达式（也不希望这样做；例如，在那里展开SubLink
     * 会导致一个无用的未引用子计划）。保留它们只会为树的后续扫描创造危险。
     * 我们可以尝试通过在此时之后进行的每个树扫描中使用QTW_IGNORE_JOINALIASES来防止这种情况，
     * 但这听起来不太可靠。
     */
    if (root->hasJoinRTEs)
    {
        foreach(l, parse->rtable)
        {
            RangeTblEntry *rte = lfirst_node(RangeTblEntry, l);

            rte->joinaliasvars = NIL;
        }
    }

    /*
     * 在某些情况下，我们可能希望将HAVING子句转移到WHERE中。
     * 如果HAVING子句包含聚合（显然）或易失性函数（因为HAVING子句应该只对每个组执行一次），
     * 我们不能这样做。如果有任何非空分组集，我们也不能这样做；
     * 将这样的子句移动到WHERE中可能会改变结果，如果任何引用的列不存在于所有分组集中。
     * （如果只有空分组集，那么HAVING子句必须是如下所述的退化形式。）
     *
     * 此外，可能该子句执行起来非常昂贵，我们最好每个组只执行一次，尽管会失去选择性。
     * 除非进行整个规划过程两次，否则很难估计，因此我们使用启发式方法：
     * 包含子计划的子句保留在HAVING中。否则，我们将HAVING子句移动或复制到WHERE中，
     * 希望在聚合之前而不是之后消除元组。
     *
     * 如果查询有显式分组，我们可以简单地将这样的子句移动到WHERE中；
     * 任何失败该子句的组都不会出现在输出中，因为它的元组都不会到达分组或聚合阶段。
     * 否则，我们必须有一个退化的（无变量）HAVING子句，
     * 我们将其放在WHERE中，以便query_planner()可以在一个门控Result节点中使用它，
     * 但也保留在HAVING中以确保我们不会发出虚假的聚合行。
     * （这可以做得更好，但似乎不值得优化。）
     *
     * 注意，此时havingQual和parse->jointree->quals都采用隐式AND列表形式，
     * 尽管它们被声明为Node *。
     */
    newHaving = NIL;
    foreach(l, (List *) parse->havingQual)
    {
        Node       *havingclause = (Node *) lfirst(l);

        /* 条件判断：哪些HAVING子句需要保留，哪些可以移动 */
        if ((parse->groupClause && parse->groupingSets) ||
            contain_agg_clause(havingclause) ||
            contain_volatile_functions(havingclause) ||
            contain_subplans(havingclause))
        {
            /* 保留在HAVING中 */
            newHaving = lappend(newHaving, havingclause);
        }
        else if (parse->groupClause && !parse->groupingSets)
        {
            /* 移动到WHERE中 */
            parse->jointree->quals = (Node *)
                lappend((List *) parse->jointree->quals, havingclause);
        }
        else
        {
            /* 复制到WHERE中，同时保留在HAVING中 */
            parse->jointree->quals = (Node *)
                lappend((List *) parse->jointree->quals,
                       copyObject(havingclause));
            newHaving = lappend(newHaving, havingclause);
        }
    }
    parse->havingQual = (Node *) newHaving; /* 更新处理后的HAVING子句 */

    /* 删除任何冗余的GROUP BY列 */
    remove_useless_groupby_columns(root);

    /*
     * 如果有任何外部连接，尝试将它们减少为普通内部连接。
     * 此步骤在完成表达式预处理后最容易完成。
     */
    if (hasOuterJoins)
        reduce_outer_joins(root);

    /*
     * 如果有任何RTE_RESULT关系，检查它们是否可以从连接树中删除。
     * 此步骤在完成表达式预处理和外部连接减少后最有效地执行。
     */
    if (hasResultRTEs)
        remove_useless_result_rtes(root);

    /*
     * 执行主要规划。如果我们有一个继承的目标关系，需要特殊处理，
     * 否则直接转到grouping_planner。
     */
    if (parse->resultRelation &&
        rt_fetch(parse->resultRelation, parse->rtable)->inh)
        inheritance_planner(root); /* 处理继承关系的特殊规划 */
    else
        grouping_planner(root, false, tuple_fraction); /* 常规分组规划 */

    /*
     * 捕获我们可以访问的外层参数ID集，供以后的extParam/allParam计算使用。
     */
    SS_identify_outer_params(root);

    /*
     * 如果在此查询级别创建了任何initPlans，调整幸存路径的成本和并行安全标志以考虑它们。
     * initPlans实际上直到create_plan()运行时才会附加到计划树，但我们现在必须包含它们的影响。
     */
    final_rel = fetch_upper_rel(root, UPPERREL_FINAL, NULL);
    SS_charge_for_initplans(root, final_rel);

    /*
     * 确保我们已为最终关系确定了最便宜的路径。
     * （通过在此处而不是在grouping_planner中执行此操作，
     * 我们在决策中包含了initPlan成本，尽管这不太可能改变任何内容。）
     */
    set_cheapest(final_rel);

    return root; /* 返回完整的规划器信息结构 */
}


/*
 * preprocess_expression
 *		Do subquery_planner's preprocessing work for an expression,
 *		which can be a targetlist, a WHERE clause (including JOIN/ON
 *		conditions), a HAVING clause, or a few other things.
 */
static Node *
preprocess_expression(PlannerInfo *root, Node *expr, int kind)
{
	/*
	 * Fall out quickly if expression is empty.  This occurs often enough to
	 * be worth checking.  Note that null->null is the correct conversion for
	 * implicit-AND result format, too.
	 */
	if (expr == NULL)
		return NULL;

	/*
	 * If the query has any join RTEs, replace join alias variables with
	 * base-relation variables.  We must do this first, since any expressions
	 * we may extract from the joinaliasvars lists have not been preprocessed.
	 * For example, if we did this after sublink processing, sublinks expanded
	 * out from join aliases would not get processed.  But we can skip this in
	 * non-lateral RTE functions, VALUES lists, and TABLESAMPLE clauses, since
	 * they can't contain any Vars of the current query level.
	 */
	if (root->hasJoinRTEs &&
		!(kind == EXPRKIND_RTFUNC ||
		  kind == EXPRKIND_VALUES ||
		  kind == EXPRKIND_TABLESAMPLE ||
		  kind == EXPRKIND_TABLEFUNC))
		expr = flatten_join_alias_vars(root->parse, expr);

	/*
	 * Simplify constant expressions.
	 *
	 * Note: an essential effect of this is to convert named-argument function
	 * calls to positional notation and insert the current actual values of
	 * any default arguments for functions.  To ensure that happens, we *must*
	 * process all expressions here.  Previous PG versions sometimes skipped
	 * const-simplification if it didn't seem worth the trouble, but we can't
	 * do that anymore.
	 *
	 * Note: this also flattens nested AND and OR expressions into N-argument
	 * form.  All processing of a qual expression after this point must be
	 * careful to maintain AND/OR flatness --- that is, do not generate a tree
	 * with AND directly under AND, nor OR directly under OR.
	 */
	expr = eval_const_expressions(root, expr);

	/*
	 * If it's a qual or havingQual, canonicalize it.
	 */
	if (kind == EXPRKIND_QUAL)
	{
		expr = (Node *) canonicalize_qual((Expr *) expr, false);

#ifdef OPTIMIZER_DEBUG
		printf("After canonicalize_qual()\n");
		pprint(expr);
#endif
	}

	/* Expand SubLinks to SubPlans */
	if (root->parse->hasSubLinks)
		expr = SS_process_sublinks(root, expr, (kind == EXPRKIND_QUAL));

	/*
	 * XXX do not insert anything here unless you have grokked the comments in
	 * SS_replace_correlation_vars ...
	 */

	/* Replace uplevel vars with Param nodes (this IS possible in VALUES) */
	if (root->query_level > 1)
		expr = SS_replace_correlation_vars(root, expr);

	/*
	 * If it's a qual or havingQual, convert it to implicit-AND format. (We
	 * don't want to do this before eval_const_expressions, since the latter
	 * would be unable to simplify a top-level AND correctly. Also,
	 * SS_process_sublinks expects explicit-AND format.)
	 */
	if (kind == EXPRKIND_QUAL)
		expr = (Node *) make_ands_implicit((Expr *) expr);

	return expr;
}

/*
 * preprocess_qual_conditions
 *		Recursively scan the query's jointree and do subquery_planner's
 *		preprocessing work on each qual condition found therein.
 */
static void
preprocess_qual_conditions(PlannerInfo *root, Node *jtnode)
{
	if (jtnode == NULL)
		return;
	if (IsA(jtnode, RangeTblRef))
	{
		/* nothing to do here */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
			preprocess_qual_conditions(root, lfirst(l));

		f->quals = preprocess_expression(root, f->quals, EXPRKIND_QUAL);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		preprocess_qual_conditions(root, j->larg);
		preprocess_qual_conditions(root, j->rarg);

		j->quals = preprocess_expression(root, j->quals, EXPRKIND_QUAL);
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}

/*
 * preprocess_phv_expression
 *	  Do preprocessing on a PlaceHolderVar expression that's been pulled up.
 *
 * If a LATERAL subquery references an output of another subquery, and that
 * output must be wrapped in a PlaceHolderVar because of an intermediate outer
 * join, then we'll push the PlaceHolderVar expression down into the subquery
 * and later pull it back up during find_lateral_references, which runs after
 * subquery_planner has preprocessed all the expressions that were in the
 * current query level to start with.  So we need to preprocess it then.
 */
Expr *
preprocess_phv_expression(PlannerInfo *root, Expr *expr)
{
	return (Expr *) preprocess_expression(root, (Node *) expr, EXPRKIND_PHV);
}

/*
 * inheritance_planner
 *	  Generate Paths in the case where the result relation is an
 *	  inheritance set.
 *
 * We have to handle this case differently from cases where a source relation
 * is an inheritance set. Source inheritance is expanded at the bottom of the
 * plan tree (see allpaths.c), but target inheritance has to be expanded at
 * the top.  The reason is that for UPDATE, each target relation needs a
 * different targetlist matching its own column set.  Fortunately,
 * the UPDATE/DELETE target can never be the nullable side of an outer join,
 * so it's OK to generate the plan this way.
 *
 * Returns nothing; the useful output is in the Paths we attach to
 * the (UPPERREL_FINAL, NULL) upperrel stored in *root.
 *
 * Note that we have not done set_cheapest() on the final rel; it's convenient
 * to leave this to the caller.
 */
static void
inheritance_planner(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	int			top_parentRTindex = parse->resultRelation;
	List	   *select_rtable;
	List	   *select_appinfos;
	List	   *child_appinfos;
	List	   *old_child_rtis;
	List	   *new_child_rtis;
	Bitmapset  *subqueryRTindexes;
	Index		next_subquery_rti;
	int			nominalRelation = -1;
	Index		rootRelation = 0;
	List	   *final_rtable = NIL;
	List	   *final_rowmarks = NIL;
	int			save_rel_array_size = 0;
	RelOptInfo **save_rel_array = NULL;
	AppendRelInfo **save_append_rel_array = NULL;
	List	   *subpaths = NIL;
	List	   *subroots = NIL;
	List	   *resultRelations = NIL;
	List	   *withCheckOptionLists = NIL;
	List	   *returningLists = NIL;
	List	   *rowMarks;
	RelOptInfo *final_rel;
	ListCell   *lc;
	ListCell   *lc2;
	Index		rti;
	RangeTblEntry *parent_rte;
	Bitmapset  *parent_relids;
	Query	  **parent_parses;

	/* Should only get here for UPDATE or DELETE */
	Assert(parse->commandType == CMD_UPDATE ||
		   parse->commandType == CMD_DELETE);

	/*
	 * We generate a modified instance of the original Query for each target
	 * relation, plan that, and put all the plans into a list that will be
	 * controlled by a single ModifyTable node.  All the instances share the
	 * same rangetable, but each instance must have its own set of subquery
	 * RTEs within the finished rangetable because (1) they are likely to get
	 * scribbled on during planning, and (2) it's not inconceivable that
	 * subqueries could get planned differently in different cases.  We need
	 * not create duplicate copies of other RTE kinds, in particular not the
	 * target relations, because they don't have either of those issues.  Not
	 * having to duplicate the target relations is important because doing so
	 * (1) would result in a rangetable of length O(N^2) for N targets, with
	 * at least O(N^3) work expended here; and (2) would greatly complicate
	 * management of the rowMarks list.
	 *
	 * To begin with, generate a bitmapset of the relids of the subquery RTEs.
	 */
	subqueryRTindexes = NULL;
	rti = 1;
	foreach(lc, parse->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);

		if (rte->rtekind == RTE_SUBQUERY)
			subqueryRTindexes = bms_add_member(subqueryRTindexes, rti);
		rti++;
	}

	/*
	 * If the parent RTE is a partitioned table, we should use that as the
	 * nominal target relation, because the RTEs added for partitioned tables
	 * (including the root parent) as child members of the inheritance set do
	 * not appear anywhere else in the plan, so the confusion explained below
	 * for non-partitioning inheritance cases is not possible.
	 */
	parent_rte = rt_fetch(top_parentRTindex, parse->rtable);
	Assert(parent_rte->inh);
	if (parent_rte->relkind == RELKIND_PARTITIONED_TABLE)
	{
		nominalRelation = top_parentRTindex;
		rootRelation = top_parentRTindex;
	}

	/*
	 * Before generating the real per-child-relation plans, do a cycle of
	 * planning as though the query were a SELECT.  The objective here is to
	 * find out which child relations need to be processed, using the same
	 * expansion and pruning logic as for a SELECT.  We'll then pull out the
	 * RangeTblEntry-s generated for the child rels, and make use of the
	 * AppendRelInfo entries for them to guide the real planning.  (This is
	 * rather inefficient; we could perhaps stop short of making a full Path
	 * tree.  But this whole function is inefficient and slated for
	 * destruction, so let's not contort query_planner for that.)
	 */
	{
		PlannerInfo *subroot;

		/*
		 * Flat-copy the PlannerInfo to prevent modification of the original.
		 */
		subroot = makeNode(PlannerInfo);
		memcpy(subroot, root, sizeof(PlannerInfo));

		/*
		 * Make a deep copy of the parsetree for this planning cycle to mess
		 * around with, and change it to look like a SELECT.  (Hack alert: the
		 * target RTE still has updatedCols set if this is an UPDATE, so that
		 * expand_partitioned_rtentry will correctly update
		 * subroot->partColsUpdated.)
		 */
		subroot->parse = copyObject(root->parse);

		subroot->parse->commandType = CMD_SELECT;
		subroot->parse->resultRelation = 0;

		/*
		 * Ensure the subroot has its own copy of the original
		 * append_rel_list, since it'll be scribbled on.  (Note that at this
		 * point, the list only contains AppendRelInfos for flattened UNION
		 * ALL subqueries.)
		 */
		subroot->append_rel_list = copyObject(root->append_rel_list);

		/*
		 * Better make a private copy of the rowMarks, too.
		 */
		subroot->rowMarks = copyObject(root->rowMarks);

		/* There shouldn't be any OJ info to translate, as yet */
		Assert(subroot->join_info_list == NIL);
		/* and we haven't created PlaceHolderInfos, either */
		Assert(subroot->placeholder_list == NIL);

		/* Generate Path(s) for accessing this result relation */
		grouping_planner(subroot, true, 0.0 /* retrieve all tuples */ );

		/* Extract the info we need. */
		select_rtable = subroot->parse->rtable;
		select_appinfos = subroot->append_rel_list;

		/*
		 * We need to propagate partColsUpdated back, too.  (The later
		 * planning cycles will not set this because they won't run
		 * expand_partitioned_rtentry for the UPDATE target.)
		 */
		root->partColsUpdated = subroot->partColsUpdated;
	}

	/*----------
	 * Since only one rangetable can exist in the final plan, we need to make
	 * sure that it contains all the RTEs needed for any child plan.  This is
	 * complicated by the need to use separate subquery RTEs for each child.
	 * We arrange the final rtable as follows:
	 * 1. All original rtable entries (with their original RT indexes).
	 * 2. All the relation RTEs generated for children of the target table.
	 * 3. Subquery RTEs for children after the first.  We need N * (K - 1)
	 *    RT slots for this, if there are N subqueries and K child tables.
	 * 4. Additional RTEs generated during the child planning runs, such as
	 *    children of inheritable RTEs other than the target table.
	 * We assume that each child planning run will create an identical set
	 * of type-4 RTEs.
	 *
	 * So the next thing to do is append the type-2 RTEs (the target table's
	 * children) to the original rtable.  We look through select_appinfos
	 * to find them.
	 *
	 * To identify which AppendRelInfos are relevant as we thumb through
	 * select_appinfos, we need to look for both direct and indirect children
	 * of top_parentRTindex, so we use a bitmap of known parent relids.
	 * expand_inherited_rtentry() always processes a parent before any of that
	 * parent's children, so we should see an intermediate parent before its
	 * children.
	 *----------
	 */
	child_appinfos = NIL;
	old_child_rtis = NIL;
	new_child_rtis = NIL;
	parent_relids = bms_make_singleton(top_parentRTindex);
	foreach(lc, select_appinfos)
	{
		AppendRelInfo *appinfo = lfirst_node(AppendRelInfo, lc);
		RangeTblEntry *child_rte;

		/* append_rel_list contains all append rels; ignore others */
		if (!bms_is_member(appinfo->parent_relid, parent_relids))
			continue;

		/* remember relevant AppendRelInfos for use below */
		child_appinfos = lappend(child_appinfos, appinfo);

		/* extract RTE for this child rel */
		child_rte = rt_fetch(appinfo->child_relid, select_rtable);

		/* and append it to the original rtable */
		parse->rtable = lappend(parse->rtable, child_rte);

		/* remember child's index in the SELECT rtable */
		old_child_rtis = lappend_int(old_child_rtis, appinfo->child_relid);

		/* and its new index in the final rtable */
		new_child_rtis = lappend_int(new_child_rtis, list_length(parse->rtable));

		/* if child is itself partitioned, update parent_relids */
		if (child_rte->inh)
		{
			Assert(child_rte->relkind == RELKIND_PARTITIONED_TABLE);
			parent_relids = bms_add_member(parent_relids, appinfo->child_relid);
		}
	}

	/*
	 * It's possible that the RTIs we just assigned for the child rels in the
	 * final rtable are different from what they were in the SELECT query.
	 * Adjust the AppendRelInfos so that they will correctly map RT indexes to
	 * the final indexes.  We can do this left-to-right since no child rel's
	 * final RT index could be greater than what it had in the SELECT query.
	 */
	forboth(lc, old_child_rtis, lc2, new_child_rtis)
	{
		int			old_child_rti = lfirst_int(lc);
		int			new_child_rti = lfirst_int(lc2);

		if (old_child_rti == new_child_rti)
			continue;			/* nothing to do */

		Assert(old_child_rti > new_child_rti);

		ChangeVarNodes((Node *) child_appinfos,
					   old_child_rti, new_child_rti, 0);
	}

	/*
	 * Now set up rangetable entries for subqueries for additional children
	 * (the first child will just use the original ones).  These all have to
	 * look more or less real, or EXPLAIN will get unhappy; so we just make
	 * them all clones of the original subqueries.
	 */
	next_subquery_rti = list_length(parse->rtable) + 1;
	if (subqueryRTindexes != NULL)
	{
		int			n_children = list_length(child_appinfos);

		while (n_children-- > 1)
		{
			int			oldrti = -1;

			while ((oldrti = bms_next_member(subqueryRTindexes, oldrti)) >= 0)
			{
				RangeTblEntry *subqrte;

				subqrte = rt_fetch(oldrti, parse->rtable);
				parse->rtable = lappend(parse->rtable, copyObject(subqrte));
			}
		}
	}

	/*
	 * The query for each child is obtained by translating the query for its
	 * immediate parent, since the AppendRelInfo data we have shows deltas
	 * between parents and children.  We use the parent_parses array to
	 * remember the appropriate query trees.  This is indexed by parent relid.
	 * Since the maximum number of parents is limited by the number of RTEs in
	 * the SELECT query, we use that number to allocate the array.  An extra
	 * entry is needed since relids start from 1.
	 */
	parent_parses = (Query **) palloc0((list_length(select_rtable) + 1) *
									   sizeof(Query *));
	parent_parses[top_parentRTindex] = parse;

	/*
	 * And now we can get on with generating a plan for each child table.
	 */
	foreach(lc, child_appinfos)
	{
		AppendRelInfo *appinfo = lfirst_node(AppendRelInfo, lc);
		Index		this_subquery_rti = next_subquery_rti;
		Query	   *parent_parse;
		PlannerInfo *subroot;
		RangeTblEntry *child_rte;
		RelOptInfo *sub_final_rel;
		Path	   *subpath;

		/*
		 * expand_inherited_rtentry() always processes a parent before any of
		 * that parent's children, so the parent query for this relation
		 * should already be available.
		 */
		parent_parse = parent_parses[appinfo->parent_relid];
		Assert(parent_parse != NULL);

		/*
		 * We need a working copy of the PlannerInfo so that we can control
		 * propagation of information back to the main copy.
		 */
		subroot = makeNode(PlannerInfo);
		memcpy(subroot, root, sizeof(PlannerInfo));

		/*
		 * Generate modified query with this rel as target.  We first apply
		 * adjust_appendrel_attrs, which copies the Query and changes
		 * references to the parent RTE to refer to the current child RTE,
		 * then fool around with subquery RTEs.
		 */
		subroot->parse = (Query *)
			adjust_appendrel_attrs(subroot,
								   (Node *) parent_parse,
								   1, &appinfo);

		/*
		 * If there are securityQuals attached to the parent, move them to the
		 * child rel (they've already been transformed properly for that).
		 */
		parent_rte = rt_fetch(appinfo->parent_relid, subroot->parse->rtable);
		child_rte = rt_fetch(appinfo->child_relid, subroot->parse->rtable);
		child_rte->securityQuals = parent_rte->securityQuals;
		parent_rte->securityQuals = NIL;

		/*
		 * HACK: setting this to a value other than INHKIND_NONE signals to
		 * relation_excluded_by_constraints() to treat the result relation as
		 * being an appendrel member.
		 */
		subroot->inhTargetKind =
			(rootRelation != 0) ? INHKIND_PARTITIONED : INHKIND_INHERITED;

		/*
		 * If this child is further partitioned, remember it as a parent.
		 * Since a partitioned table does not have any data, we don't need to
		 * create a plan for it, and we can stop processing it here.  We do,
		 * however, need to remember its modified PlannerInfo for use when
		 * processing its children, since we'll update their varnos based on
		 * the delta from immediate parent to child, not from top to child.
		 *
		 * Note: a very non-obvious point is that we have not yet added
		 * duplicate subquery RTEs to the subroot's rtable.  We mustn't,
		 * because then its children would have two sets of duplicates,
		 * confusing matters.
		 */
		if (child_rte->inh)
		{
			Assert(child_rte->relkind == RELKIND_PARTITIONED_TABLE);
			parent_parses[appinfo->child_relid] = subroot->parse;
			continue;
		}

		/*
		 * Set the nominal target relation of the ModifyTable node if not
		 * already done.  If the target is a partitioned table, we already set
		 * nominalRelation to refer to the partition root, above.  For
		 * non-partitioned inheritance cases, we'll use the first child
		 * relation (even if it's excluded) as the nominal target relation.
		 * Because of the way expand_inherited_rtentry works, that should be
		 * the RTE representing the parent table in its role as a simple
		 * member of the inheritance set.
		 *
		 * It would be logically cleaner to *always* use the inheritance
		 * parent RTE as the nominal relation; but that RTE is not otherwise
		 * referenced in the plan in the non-partitioned inheritance case.
		 * Instead the duplicate child RTE created by expand_inherited_rtentry
		 * is used elsewhere in the plan, so using the original parent RTE
		 * would give rise to confusing use of multiple aliases in EXPLAIN
		 * output for what the user will think is the "same" table.  OTOH,
		 * it's not a problem in the partitioned inheritance case, because
		 * there is no duplicate RTE for the parent.
		 */
		if (nominalRelation < 0)
			nominalRelation = appinfo->child_relid;

		/*
		 * As above, each child plan run needs its own append_rel_list and
		 * rowmarks, which should start out as pristine copies of the
		 * originals.  There can't be any references to UPDATE/DELETE target
		 * rels in them; but there could be subquery references, which we'll
		 * fix up in a moment.
		 */
		subroot->append_rel_list = copyObject(root->append_rel_list);
		subroot->rowMarks = copyObject(root->rowMarks);

		/*
		 * If this isn't the first child Query, adjust Vars and jointree
		 * entries to reference the appropriate set of subquery RTEs.
		 */
		if (final_rtable != NIL && subqueryRTindexes != NULL)
		{
			int			oldrti = -1;

			while ((oldrti = bms_next_member(subqueryRTindexes, oldrti)) >= 0)
			{
				Index		newrti = next_subquery_rti++;

				ChangeVarNodes((Node *) subroot->parse, oldrti, newrti, 0);
				ChangeVarNodes((Node *) subroot->append_rel_list,
							   oldrti, newrti, 0);
				ChangeVarNodes((Node *) subroot->rowMarks, oldrti, newrti, 0);
			}
		}

		/* There shouldn't be any OJ info to translate, as yet */
		Assert(subroot->join_info_list == NIL);
		/* and we haven't created PlaceHolderInfos, either */
		Assert(subroot->placeholder_list == NIL);

		/* Generate Path(s) for accessing this result relation */
		grouping_planner(subroot, true, 0.0 /* retrieve all tuples */ );

		/*
		 * Select cheapest path in case there's more than one.  We always run
		 * modification queries to conclusion, so we care only for the
		 * cheapest-total path.
		 */
		sub_final_rel = fetch_upper_rel(subroot, UPPERREL_FINAL, NULL);
		set_cheapest(sub_final_rel);
		subpath = sub_final_rel->cheapest_total_path;

		/*
		 * If this child rel was excluded by constraint exclusion, exclude it
		 * from the result plan.
		 */
		if (IS_DUMMY_REL(sub_final_rel))
			continue;

		/*
		 * If this is the first non-excluded child, its post-planning rtable
		 * becomes the initial contents of final_rtable; otherwise, copy its
		 * modified subquery RTEs into final_rtable, to ensure we have sane
		 * copies of those.  Also save the first non-excluded child's version
		 * of the rowmarks list; we assume all children will end up with
		 * equivalent versions of that.
		 */
		if (final_rtable == NIL)
		{
			final_rtable = subroot->parse->rtable;
			final_rowmarks = subroot->rowMarks;
		}
		else
		{
			Assert(list_length(final_rtable) ==
				   list_length(subroot->parse->rtable));
			if (subqueryRTindexes != NULL)
			{
				int			oldrti = -1;

				while ((oldrti = bms_next_member(subqueryRTindexes, oldrti)) >= 0)
				{
					Index		newrti = this_subquery_rti++;
					RangeTblEntry *subqrte;
					ListCell   *newrticell;

					subqrte = rt_fetch(newrti, subroot->parse->rtable);
					newrticell = list_nth_cell(final_rtable, newrti - 1);
					lfirst(newrticell) = subqrte;
				}
			}
		}

		/*
		 * We need to collect all the RelOptInfos from all child plans into
		 * the main PlannerInfo, since setrefs.c will need them.  We use the
		 * last child's simple_rel_array, so we have to propagate forward the
		 * RelOptInfos that were already built in previous children.
		 */
		Assert(subroot->simple_rel_array_size >= save_rel_array_size);
		for (rti = 1; rti < save_rel_array_size; rti++)
		{
			RelOptInfo *brel = save_rel_array[rti];

			if (brel)
				subroot->simple_rel_array[rti] = brel;
		}
		save_rel_array_size = subroot->simple_rel_array_size;
		save_rel_array = subroot->simple_rel_array;
		save_append_rel_array = subroot->append_rel_array;

		/*
		 * Make sure any initplans from this rel get into the outer list. Note
		 * we're effectively assuming all children generate the same
		 * init_plans.
		 */
		root->init_plans = subroot->init_plans;

		/* Build list of sub-paths */
		subpaths = lappend(subpaths, subpath);

		/* Build list of modified subroots, too */
		subroots = lappend(subroots, subroot);

		/* Build list of target-relation RT indexes */
		resultRelations = lappend_int(resultRelations, appinfo->child_relid);

		/* Build lists of per-relation WCO and RETURNING targetlists */
		if (parse->withCheckOptions)
			withCheckOptionLists = lappend(withCheckOptionLists,
										   subroot->parse->withCheckOptions);
		if (parse->returningList)
			returningLists = lappend(returningLists,
									 subroot->parse->returningList);

		Assert(!parse->onConflict);
	}

	/* Result path must go into outer query's FINAL upperrel */
	final_rel = fetch_upper_rel(root, UPPERREL_FINAL, NULL);

	/*
	 * We don't currently worry about setting final_rel's consider_parallel
	 * flag in this case, nor about allowing FDWs or create_upper_paths_hook
	 * to get control here.
	 */

	if (subpaths == NIL)
	{
		/*
		 * We managed to exclude every child rel, so generate a dummy path
		 * representing the empty set.  Although it's clear that no data will
		 * be updated or deleted, we will still need to have a ModifyTable
		 * node so that any statement triggers are executed.  (This could be
		 * cleaner if we fixed nodeModifyTable.c to support zero child nodes,
		 * but that probably wouldn't be a net win.)
		 */
		Path	   *dummy_path;

		/* tlist processing never got done, either */
		root->processed_tlist = preprocess_targetlist(root);
		final_rel->reltarget = create_pathtarget(root, root->processed_tlist);

		/* Make a dummy path, cf set_dummy_rel_pathlist() */
		dummy_path = (Path *) create_append_path(NULL, final_rel, NIL, NIL,
												 NIL, NULL, 0, false,
												 NIL, -1);

		/* These lists must be nonempty to make a valid ModifyTable node */
		subpaths = list_make1(dummy_path);
		subroots = list_make1(root);
		resultRelations = list_make1_int(parse->resultRelation);
		if (parse->withCheckOptions)
			withCheckOptionLists = list_make1(parse->withCheckOptions);
		if (parse->returningList)
			returningLists = list_make1(parse->returningList);
		/* Disable tuple routing, too, just to be safe */
		root->partColsUpdated = false;
	}
	else
	{
		/*
		 * Put back the final adjusted rtable into the master copy of the
		 * Query.  (We mustn't do this if we found no non-excluded children,
		 * since we never saved an adjusted rtable at all.)
		 */
		parse->rtable = final_rtable;
		root->simple_rel_array_size = save_rel_array_size;
		root->simple_rel_array = save_rel_array;
		root->append_rel_array = save_append_rel_array;

		/* Must reconstruct master's simple_rte_array, too */
		root->simple_rte_array = (RangeTblEntry **)
			palloc0((list_length(final_rtable) + 1) * sizeof(RangeTblEntry *));
		rti = 1;
		foreach(lc, final_rtable)
		{
			RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);

			root->simple_rte_array[rti++] = rte;
		}

		/* Put back adjusted rowmarks, too */
		root->rowMarks = final_rowmarks;
	}

	/*
	 * If there was a FOR [KEY] UPDATE/SHARE clause, the LockRows node will
	 * have dealt with fetching non-locked marked rows, else we need to have
	 * ModifyTable do that.
	 */
	if (parse->rowMarks)
		rowMarks = NIL;
	else
		rowMarks = root->rowMarks;

	/* Create Path representing a ModifyTable to do the UPDATE/DELETE work */
	add_path(final_rel, (Path *)
			 create_modifytable_path(root, final_rel,
									 parse->commandType,
									 parse->canSetTag,
									 nominalRelation,
									 rootRelation,
									 root->partColsUpdated,
									 resultRelations,
									 subpaths,
									 subroots,
									 withCheckOptionLists,
									 returningLists,
									 rowMarks,
									 NULL,
									 assign_special_exec_param(root)));
}

/*--------------------
 * grouping_planner
 *	  执行与分组、聚合等相关的规划步骤。
 *
 * 该函数将所有必需的顶层处理添加到由 query_planner 生产的
 * 扫描/连接 Path(s) 上。
 *
 * 如果 inheritance_update 为 true，表示该函数由 inheritance_planner
 * 调用，不应在生成的 Path(s) 中包含 ModifyTable 步骤。
 * （inheritance_planner 将为所有目标表创建一个单一的 ModifyTable 节点。）
 *
 * tuple_fraction 是我们预期将被检索的元组的比例。
 * tuple_fraction 的解释如下：
 *	  0: 预计检索所有元组（正常情况）
 *	  0 < tuple_fraction < 1: 预计检索计划可用元组的给定比例
 *	  tuple_fraction >= 1: tuple_fraction 表示预计检索的元组的绝对数量（即 LIMIT）
 *
 * 函数不返回值；有用的输出在 *root 中我们附加到 (UPPERREL_FINAL, NULL)
 * upperrel 的 Paths 中。另外，root->processed_tlist 包含最终处理的目标列表。
 *
 * 注意：我们尚未对最终 rel 执行 set_cheapest()；将这一工作留给调用者更方便。
 *--------------------
 */
static void
grouping_planner(PlannerInfo *root, bool inheritance_update,
				 double tuple_fraction)
{
	Query	   *parse = root->parse;
	int64		offset_est = 0;
	int64		count_est = 0;
	double		limit_tuples = -1.0;
	bool		have_postponed_srfs = false;
	PathTarget *final_target;
	List	   *final_targets;
	List	   *final_targets_contain_srfs;
	bool		final_target_parallel_safe;
	RelOptInfo *current_rel;
	RelOptInfo *final_rel;
	FinalPathExtraData extra;
	ListCell   *lc;

	/*
	 * 处理查询中的 LIMIT 和 OFFSET 子句，
	 * 并据此调整优化器的“元组获取比例”（tuple_fraction），以便生成更优的执行计划。
	 * 如果存在 LIMIT/OFFSET, 调整调用者提供的 tuple_fraction
	 *
	 * tuple_fraction 是一个 0 到 1 之间的浮点数，表示用户期望获取结果集中多少比例的数据。
	 * -- 0.0 表示需要所有数据（例如 SELECT * FROM table）。
	 * -- > 0.0 表示只需要部分数据（例如游标或 LIMIT）。
	 * 逻辑：如果查询包含 LIMIT 或 OFFSET，优化器需要知道这一点，因为这会极大地影响计划的选择。
	 * 例如，如果只需要前 10 行，优化器可能会选择索引扫描而不是全表扫描。
	 */
	if (parse->limitCount || parse->limitOffset)
	{
		/*
		 * 估算 LIMIT 和 OFFSET 的具体数值（如果它们是常量表达式），并计算出一个新的 tuple_fraction。
		 * offset_est：估算的跳过行数。
		 * count_est：估算的返回行数。
		 */
		tuple_fraction = preprocess_limit(root, tuple_fraction,
										  &offset_est, &count_est);

		/*
		 * 如果我们有一个已知的 LIMIT，且没有未知的 OFFSET，则可估算
		 * 有界排序的效果。
		 */
		if (count_est > 0 && offset_est >= 0)
			limit_tuples = (double) count_est + (double) offset_est;
	}

	/* 使 tuple_fraction 对低层函数可见 */
	root->tuple_fraction = tuple_fraction;

	if (parse->setOperations)
	{
		/*
		 * 如果查询最外层有 ORDER BY 子句，优化器假设必须获取所有结果行才能完成排序。
		 * 将 tuple_fraction 设置为 0.0（表示需要 100% 的数据），
		 * 防止优化器错误地选择只优化前几行返回速度的计划（如使用索引扫描但总成本较高的计划），
		 * 因为无论如何都需要全量数据来排序。
		 */
		if (parse->sortClause)
			root->tuple_fraction = 0.0;

		/*
		 * 这是处理集合操作的关键函数。它会递归地规划整个集合操作树（例如 A UNION (B INTERSECT C)）。
		 * 返回一个 RelOptInfo 对象（current_rel），其中包含了执行该集合操作的各种可能路径（Paths）。
		 * 为集合操作构造 Paths。结果通常只需一个顶层排序和/或 LIMIT。
		 * 注意：递归 union 的特殊工作由 plan_set_operations 负责。
		 */
		current_rel = plan_set_operations(root);

		/*
		 * 我们不应需要调用 preprocess_targetlist，因为我们必须处于 SELECT
		 * 查询节点。使用 plan_set_operations 返回的 processed_tlist（复制）
		 * 并从原始 tlist 转移任何排序键信息。
		 */
		Assert(parse->commandType == CMD_SELECT);

		/*
		 * postprocess_setop_tlist 会将生成的列表与原始查询的 targetList 进行匹配，
		 * 确保输出列的名称、排序键信息等与用户查询一致。
		 * 为安全起见，复制 processed_tlist 而不是直接修改
		 */
		root->processed_tlist =
			postprocess_setop_tlist(copyObject(root->processed_tlist),
									parse->targetList);

		/* 也提取 setop 结果 tlist 的 PathTarget 形式 */
		final_target = current_rel->cheapest_total_path->pathtarget;

		/* 并检查其是否并行安全 */
		final_target_parallel_safe =
			is_parallel_safe(root, (Node *) final_target->exprs);

		/* setop 结果 tlist 不会包含任何 SRF */
		Assert(!parse->hasTargetSRFs);
		final_targets = final_targets_contain_srfs = NIL;

		/*
		 * 这里不能处理 FOR [KEY] UPDATE/SHARE（解析器应已检查，但再加以确认）。
		 */
		if (parse->rowMarks)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			/*------
			  translator: %s is a SQL row locking clause such as FOR UPDATE */
					 errmsg("%s is not allowed with UNION/INTERSECT/EXCEPT",
							LCS_asString(linitial_node(RowMarkClause,
													   parse->rowMarks)->strength))));

		/*
		 * 计算表示结果排序要求的 pathkeys
		 */
		Assert(parse->distinctClause == NIL);
		root->sort_pathkeys = make_pathkeys_for_sortclauses(root,
															parse->sortClause,
															root->processed_tlist);
	}
	else
	{
		/* 非集合操作，执行常规规划 */
		PathTarget *sort_input_target;
		List	   *sort_input_targets;
		List	   *sort_input_targets_contain_srfs;
		bool		sort_input_target_parallel_safe;
		PathTarget *grouping_target;
		List	   *grouping_targets;
		List	   *grouping_targets_contain_srfs;
		bool		grouping_target_parallel_safe;
		PathTarget *scanjoin_target;
		List	   *scanjoin_targets;
		List	   *scanjoin_targets_contain_srfs;
		bool		scanjoin_target_parallel_safe;
		bool		scanjoin_target_same_exprs;
		bool		have_grouping;
		AggClauseCosts agg_costs;
		WindowFuncLists *wflists = NULL;
		List	   *activeWindows = NIL;
		grouping_sets_data *gset_data = NULL;
		standard_qp_extra qp_extra;

		/* 递归查询应始终具有 setOperations */
		Assert(!root->hasRecursion);

		/* 预处理 grouping sets 和 GROUP BY 子句（如有） */
		if (parse->groupingSets)
		{
			gset_data = preprocess_grouping_sets(root);
		}
		else
		{
			/* 预处理常规 GROUP BY 子句（如有） */
			if (parse->groupClause)
				parse->groupClause = preprocess_groupclause(root, NIL);
		}

		/*
		 * 预处理 targetlist。虽然剩余多数规划工作使用 PathTarget 表示，
		 * 但仍需保留最终 tlist 的完整表示以便在生成的 Plan 顶层保留修饰
		 * （例如 resnames 等）。
		 */
		root->processed_tlist = preprocess_targetlist(root);

		/*
		 * 收集聚合的统计信息用于估算成本，并标记所有 Aggref 的 aggtranstype。
		 * 必须在将 tlist 拆分成各种 pathtarget 之前完成，否则某些 Aggref
		 * 的副本可能逃过正确标记。
		 */
		MemSet(&agg_costs, 0, sizeof(AggClauseCosts));
		if (parse->hasAggs)
		{
			get_agg_clause_costs(root, (Node *) root->processed_tlist,
								 AGGSPLIT_SIMPLE, &agg_costs);
			get_agg_clause_costs(root, parse->havingQual, AGGSPLIT_SIMPLE,
								 &agg_costs);
		}

		/*
		 * 在 tlist 中查找窗口函数（也会包含 ORDER BY 使用的表达式）。
		 * 注意它们可能已被常量折叠移除。
		 */
		if (parse->hasWindowFuncs)
		{
			wflists = find_window_functions((Node *) root->processed_tlist,
											list_length(parse->windowClause));
			if (wflists->numWindowFuncs > 0)
				activeWindows = select_active_windows(root, wflists);
			else
				parse->hasWindowFuncs = false;
		}

		/*
		 * 预处理 MIN/MAX 聚合（如有）。注意：在此与调用 query_planner()
		 * 之间不要添加需要在 planagg.c 中重复的逻辑。
		 */
		if (parse->hasAggs)
			preprocess_minmax_aggregates(root);

		/*
		 * 确定 query_planner 的结果子计划需要返回的行数上限。
		 * 即使我们总体上知道上限，如果查询包含任何分组/聚合操作或
		 * tlist 中的 SRF，则该上限不适用。
		 */
		if (parse->groupClause ||
			parse->groupingSets ||
			parse->distinctClause ||
			parse->hasAggs ||
			parse->hasWindowFuncs ||
			parse->hasTargetSRFs ||
			root->hasHavingQual)
			root->limit_tuples = -1.0;
		else
			root->limit_tuples = limit_tuples;

		/* 为 standard_qp_callback 设置所需数据 */
		qp_extra.activeWindows = activeWindows;
		qp_extra.groupClause = (gset_data
								? (gset_data->rollups ? linitial_node(RollupData, gset_data->rollups)->groupClause : NIL)
								: parse->groupClause);

		/*
		 * 为 FROM/WHERE 部分生成最佳的未排序和预排序路径（query_planner 会调用 standard_qp_callback
		 * 来生成 pathkeys 表示）。可能不存在任何预排序路径。
		 */
		current_rel = query_planner(root, standard_qp_callback, &qp_extra);

		/*
		 * 将查询的结果 tlist 转换为 PathTarget 形式。
		 *
		 * 注意：这不能在 query_planner() 之前完成，因为 appendrel 展开可能会在
		 * root->processed_tlist 中添加 resjunk 条目。等待到此刻也有利于使用
		 * query_planner() 中获得的每个 Var 的宽度估计。
		 */
		final_target = create_pathtarget(root, root->processed_tlist);
		final_target_parallel_safe =
			is_parallel_safe(root, (Node *) final_target->exprs);

		/*
		 * 如果给出了 ORDER BY，考虑是否应使用后置投影，并在需要时计算
		 * 前序步骤的调整后目标。
		 */
		if (parse->sortClause)
		{
			sort_input_target = make_sort_input_target(root,
													   final_target,
													   &have_postponed_srfs);
			sort_input_target_parallel_safe =
				is_parallel_safe(root, (Node *) sort_input_target->exprs);
		}
		else
		{
			sort_input_target = final_target;
			sort_input_target_parallel_safe = final_target_parallel_safe;
		}

		/*
		 * 如果有窗口函数，则任何分组步骤的输出应满足窗口函数的要求；
		 * 否则应为 sort_input_target。
		 */
		if (activeWindows)
		{
			grouping_target = make_window_input_target(root,
													   final_target,
													   activeWindows);
			grouping_target_parallel_safe =
				is_parallel_safe(root, (Node *) grouping_target->exprs);
		}
		else
		{
			grouping_target = sort_input_target;
			grouping_target_parallel_safe = sort_input_target_parallel_safe;
		}

		/*
		 * 如果需要分组或聚合，顶层扫描/连接计划节点必须产生 grouping_target；
		 * 否则应产生 grouping_target（对没有分组的情况两者相同）。
		 */
		have_grouping = (parse->groupClause || parse->groupingSets ||
						 parse->hasAggs || root->hasHavingQual);
		if (have_grouping)
		{
			scanjoin_target = make_group_input_target(root, final_target);
			scanjoin_target_parallel_safe =
				is_parallel_safe(root, (Node *) scanjoin_target->exprs);
		}
		else
		{
			scanjoin_target = grouping_target;
			scanjoin_target_parallel_safe = grouping_target_parallel_safe;
		}

		/*
		 * 如果 targetlist 中有 SRF，必须将每个 PathTarget 划分为 SRF 计算部分
		 * 与 SRF-free 部分。用不含 SRF 的版本替换每个命名目标，并记住
		 * 以后需要添加的投影步骤列表。
		 */
		if (parse->hasTargetSRFs)
		{
			/* final_target 不会重新计算 sort_input_target 中的任何 SRF */
			split_pathtarget_at_srfs(root, final_target, sort_input_target,
									 &final_targets,
									 &final_targets_contain_srfs);
			final_target = linitial_node(PathTarget, final_targets);
			Assert(!linitial_int(final_targets_contain_srfs));
			/* 同样处理 sort_input_target 与 grouping_target 之间的关系 */
			split_pathtarget_at_srfs(root, sort_input_target, grouping_target,
									 &sort_input_targets,
									 &sort_input_targets_contain_srfs);
			sort_input_target = linitial_node(PathTarget, sort_input_targets);
			Assert(!linitial_int(sort_input_targets_contain_srfs));
			/* 同样处理 grouping_target 与 scanjoin_target 之间的关系 */
			split_pathtarget_at_srfs(root, grouping_target, scanjoin_target,
									 &grouping_targets,
									 &grouping_targets_contain_srfs);
			grouping_target = linitial_node(PathTarget, grouping_targets);
			Assert(!linitial_int(grouping_targets_contain_srfs));
			/* scanjoin_target 不会有任何预计算的 SRF */
			split_pathtarget_at_srfs(root, scanjoin_target, NULL,
									 &scanjoin_targets,
									 &scanjoin_targets_contain_srfs);
			scanjoin_target = linitial_node(PathTarget, scanjoin_targets);
			Assert(!linitial_int(scanjoin_targets_contain_srfs));
		}
		else
		{
			/* 初始化列表；对多数情况，哑值是可以接受的 */
			final_targets = final_targets_contain_srfs = NIL;
			sort_input_targets = sort_input_targets_contain_srfs = NIL;
			grouping_targets = grouping_targets_contain_srfs = NIL;
			scanjoin_targets = list_make1(scanjoin_target);
			scanjoin_targets_contain_srfs = NIL;
		}

		/* 应用 scan/join 目标到路径上 */
		scanjoin_target_same_exprs = list_length(scanjoin_targets) == 1
			&& equal(scanjoin_target->exprs, current_rel->reltarget->exprs);
		apply_scanjoin_target_to_paths(root, current_rel, scanjoin_targets,
									   scanjoin_targets_contain_srfs,
									   scanjoin_target_parallel_safe,
									   scanjoin_target_same_exprs);

		/*
		 * 将刚计算的各个 upper-rel PathTargets 保存到 root->upper_targets[] 中。
		 * 核心代码不上层使用它，但扩展可以从这里获取信息。为一致性，
		 * 保存所有中间目标，即使某些对应的 upperrels 对于该查询可能不需要。
		 */
		root->upper_targets[UPPERREL_FINAL] = final_target;
		root->upper_targets[UPPERREL_ORDERED] = final_target;
		root->upper_targets[UPPERREL_DISTINCT] = sort_input_target;
		root->upper_targets[UPPERREL_WINDOW] = sort_input_target;
		root->upper_targets[UPPERREL_GROUP_AGG] = grouping_target;

		/*
		 * 如果需要分组和/或聚合，考虑实现方式。构建一个表示该阶段输出的 upperrel。
		 */
		if (have_grouping)
		{
			current_rel = create_grouping_paths(root,
												current_rel,
												grouping_target,
												grouping_target_parallel_safe,
												&agg_costs,
												gset_data);
			/* 如果 grouping_target 包含 SRF，修正路径 */
			if (parse->hasTargetSRFs)
				adjust_paths_for_srfs(root, current_rel,
									  grouping_targets,
									  grouping_targets_contain_srfs);
		}

		/*
		 * 如果有窗口函数，考虑如何实现这些函数。构建一个表示该阶段输出的 upperrel。
		 */
		if (activeWindows)
		{
			current_rel = create_window_paths(root,
											  current_rel,
											  grouping_target,
											  sort_input_target,
											  sort_input_target_parallel_safe,
											  wflists,
											  activeWindows);
			/* 如果 sort_input_target 包含 SRF，修正路径 */
			if (parse->hasTargetSRFs)
				adjust_paths_for_srfs(root, current_rel,
									  sort_input_targets,
									  sort_input_targets_contain_srfs);
		}

		/*
		 * 如果有 DISTINCT 子句，考虑如何实现。构建一个表示该阶段输出的 upperrel。
		 */
		if (parse->distinctClause)
		{
			current_rel = create_distinct_paths(root,
												current_rel);
		}
	}							/* end of if (setOperations) */

	/*
	 * 如果存在 ORDER BY，考虑如何实现该排序，并生成一个新的 upperrel，
	 * 其中只包含发出正确排序且投影到 final_target 的路径。
	 * 在排序成本估算中可以应用原始的 limit_tuples 上限，但仅当没有推迟的 SRF 时才可。
	 */
	if (parse->sortClause)
	{
		current_rel = create_ordered_paths(root,
										   current_rel,
										   final_target,
										   final_target_parallel_safe,
										   have_postponed_srfs ? -1.0 :
										   limit_tuples);
		/* 如果 final_target 包含 SRF，修正路径 */
		if (parse->hasTargetSRFs)
			adjust_paths_for_srfs(root, current_rel,
								  final_targets,
								  final_targets_contain_srfs);
	}

	/*
	 * 现在准备构建最终输出的 upperrel。
	 */
	final_rel = fetch_upper_rel(root, UPPERREL_FINAL, NULL);

	/*
	 * 如果输入 rel 标记为 consider_parallel 且 LIMIT 子句中没有不并行安全的内容，
	 * 则 final_rel 也可以标记为 consider_parallel。注意如果查询有 rowMarks 或不是 SELECT，
	 * 则查询中每个关系的 consider_parallel 都为 false。
	 */
	if (current_rel->consider_parallel &&
		is_parallel_safe(root, parse->limitOffset) &&
		is_parallel_safe(root, parse->limitCount))
		final_rel->consider_parallel = true;

	/*
	 * 如果 current_rel 属于单个 FDW，则 final_rel 也同样属于该 FDW。
	 */
	final_rel->serverid = current_rel->serverid;
	final_rel->userid = current_rel->userid;
	final_rel->useridiscurrent = current_rel->useridiscurrent;
	final_rel->fdwroutine = current_rel->fdwroutine;

	/*
	 * 为 final_rel 生成路径。插入所有保留下来的路径，并在需要时添加
	 * LockRows、Limit 和/或 ModifyTable 步骤。
	 */
	foreach(lc, current_rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);

		/*
		 * 如果存在 FOR [KEY] UPDATE/SHARE 子句，添加 LockRows 节点。
		 * （注意：我们有意测试 parse->rowMarks 而不是 root->rowMarks。）
		 */
		if (parse->rowMarks)
		{
			path = (Path *) create_lockrows_path(root, final_rel, path,
												 root->rowMarks,
												 assign_special_exec_param(root));
		}

		/*
		 * 如果存在 LIMIT/OFFSET 子句，添加 LIMIT 节点。
		 */
		if (limit_needed(parse))
		{
			path = (Path *) create_limit_path(root, final_rel, path,
											  parse->limitOffset,
											  parse->limitCount,
											  offset_est, count_est);
		}

		/*
		 * 如果这是 INSERT/UPDATE/DELETE，且我们不是从 inheritance_planner 调用，
		 * 则添加 ModifyTable 节点。
		 */
		if (parse->commandType != CMD_SELECT && !inheritance_update)
		{
			Index		rootRelation;
			List	   *withCheckOptionLists;
			List	   *returningLists;
			List	   *rowMarks;

			/*
			 * 如果目标是分区根表，需要相应地标记 ModifyTable 节点。
			 */
			if (rt_fetch(parse->resultRelation, parse->rtable)->relkind ==
				RELKIND_PARTITIONED_TABLE)
				rootRelation = parse->resultRelation;
			else
				rootRelation = 0;

			/*
			 * 设置 WITH CHECK OPTION 和 RETURNING 的列表（按关系分组），如需要。
			 */
			if (parse->withCheckOptions)
				withCheckOptionLists = list_make1(parse->withCheckOptions);
			else
				withCheckOptionLists = NIL;

			if (parse->returningList)
				returningLists = list_make1(parse->returningList);
			else
				returningLists = NIL;

			/*
			 * 如果有 FOR [KEY] UPDATE/SHARE，LockRows 节点已处理要提取的非锁定标记行，
			 * 否则由 ModifyTable 处理。
			 */
			if (parse->rowMarks)
				rowMarks = NIL;
			else
				rowMarks = root->rowMarks;

			path = (Path *)
				create_modifytable_path(root, final_rel,
										parse->commandType,
										parse->canSetTag,
										parse->resultRelation,
										rootRelation,
										false,
										list_make1_int(parse->resultRelation),
										list_make1(path),
										list_make1(root),
										withCheckOptionLists,
										returningLists,
										rowMarks,
										parse->onConflict,
										assign_special_exec_param(root));
		}

		/* 将其放入 final_rel */
		add_path(final_rel, path);
	}

	/*
	 * 如果最终 rel 可并行，并且外层查询级别可能使用它们，则也为 final_rel
	 * 生成 partial paths。
	 */
	if (final_rel->consider_parallel && root->query_level > 1 &&
		!limit_needed(parse))
	{
		Assert(!parse->rowMarks && parse->commandType == CMD_SELECT);
		foreach(lc, current_rel->partial_pathlist)
		{
			Path	   *partial_path = (Path *) lfirst(lc);

			add_partial_path(final_rel, partial_path);
		}
	}

	extra.limit_needed = limit_needed(parse);
	extra.limit_tuples = limit_tuples;
	extra.count_est = count_est;
	extra.offset_est = offset_est;

	/*
	 * 如果有 FDW 负责查询的所有基表，让其考虑添加 ForeignPaths。
	 */
	if (final_rel->fdwroutine &&
		final_rel->fdwroutine->GetForeignUpperPaths)
		final_rel->fdwroutine->GetForeignUpperPaths(root, UPPERREL_FINAL,
													current_rel, final_rel,
													&extra);

	/* 允许扩展可能添加更多路径 */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_FINAL,
									current_rel, final_rel, &extra);

	/* 注意：当前我们让调用者执行 set_cheapest() */
}

/*
 * Do preprocessing for groupingSets clause and related data.  This handles the
 * preliminary steps of expanding the grouping sets, organizing them into lists
 * of rollups, and preparing annotations which will later be filled in with
 * size estimates.
 */
static grouping_sets_data *
preprocess_grouping_sets(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	List	   *sets;
	int			maxref = 0;
	ListCell   *lc;
	ListCell   *lc_set;
	grouping_sets_data *gd = palloc0(sizeof(grouping_sets_data));

	parse->groupingSets = expand_grouping_sets(parse->groupingSets, -1);

	gd->any_hashable = false;
	gd->unhashable_refs = NULL;
	gd->unsortable_refs = NULL;
	gd->unsortable_sets = NIL;

	if (parse->groupClause)
	{
		ListCell   *lc;

		foreach(lc, parse->groupClause)
		{
			SortGroupClause *gc = lfirst_node(SortGroupClause, lc);
			Index		ref = gc->tleSortGroupRef;

			if (ref > maxref)
				maxref = ref;

			if (!gc->hashable)
				gd->unhashable_refs = bms_add_member(gd->unhashable_refs, ref);

			if (!OidIsValid(gc->sortop))
				gd->unsortable_refs = bms_add_member(gd->unsortable_refs, ref);
		}
	}

	/* Allocate workspace array for remapping */
	gd->tleref_to_colnum_map = (int *) palloc((maxref + 1) * sizeof(int));

	/*
	 * If we have any unsortable sets, we must extract them before trying to
	 * prepare rollups. Unsortable sets don't go through
	 * reorder_grouping_sets, so we must apply the GroupingSetData annotation
	 * here.
	 */
	if (!bms_is_empty(gd->unsortable_refs))
	{
		List	   *sortable_sets = NIL;

		foreach(lc, parse->groupingSets)
		{
			List	   *gset = (List *) lfirst(lc);

			if (bms_overlap_list(gd->unsortable_refs, gset))
			{
				GroupingSetData *gs = makeNode(GroupingSetData);

				gs->set = gset;
				gd->unsortable_sets = lappend(gd->unsortable_sets, gs);

				/*
				 * We must enforce here that an unsortable set is hashable;
				 * later code assumes this.  Parse analysis only checks that
				 * every individual column is either hashable or sortable.
				 *
				 * Note that passing this test doesn't guarantee we can
				 * generate a plan; there might be other showstoppers.
				 */
				if (bms_overlap_list(gd->unhashable_refs, gset))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("could not implement GROUP BY"),
							 errdetail("Some of the datatypes only support hashing, while others only support sorting.")));
			}
			else
				sortable_sets = lappend(sortable_sets, gset);
		}

		if (sortable_sets)
			sets = extract_rollup_sets(sortable_sets);
		else
			sets = NIL;
	}
	else
		sets = extract_rollup_sets(parse->groupingSets);

	foreach(lc_set, sets)
	{
		List	   *current_sets = (List *) lfirst(lc_set);
		RollupData *rollup = makeNode(RollupData);
		GroupingSetData *gs;

		/*
		 * Reorder the current list of grouping sets into correct prefix
		 * order.  If only one aggregation pass is needed, try to make the
		 * list match the ORDER BY clause; if more than one pass is needed, we
		 * don't bother with that.
		 *
		 * Note that this reorders the sets from smallest-member-first to
		 * largest-member-first, and applies the GroupingSetData annotations,
		 * though the data will be filled in later.
		 */
		current_sets = reorder_grouping_sets(current_sets,
											 (list_length(sets) == 1
											  ? parse->sortClause
											  : NIL));

		/*
		 * Get the initial (and therefore largest) grouping set.
		 */
		gs = linitial_node(GroupingSetData, current_sets);

		/*
		 * Order the groupClause appropriately.  If the first grouping set is
		 * empty, then the groupClause must also be empty; otherwise we have
		 * to force the groupClause to match that grouping set's order.
		 *
		 * (The first grouping set can be empty even though parse->groupClause
		 * is not empty only if all non-empty grouping sets are unsortable.
		 * The groupClauses for hashed grouping sets are built later on.)
		 */
		if (gs->set)
			rollup->groupClause = preprocess_groupclause(root, gs->set);
		else
			rollup->groupClause = NIL;

		/*
		 * Is it hashable? We pretend empty sets are hashable even though we
		 * actually force them not to be hashed later. But don't bother if
		 * there's nothing but empty sets (since in that case we can't hash
		 * anything).
		 */
		if (gs->set &&
			!bms_overlap_list(gd->unhashable_refs, gs->set))
		{
			rollup->hashable = true;
			gd->any_hashable = true;
		}

		/*
		 * Now that we've pinned down an order for the groupClause for this
		 * list of grouping sets, we need to remap the entries in the grouping
		 * sets from sortgrouprefs to plain indices (0-based) into the
		 * groupClause for this collection of grouping sets. We keep the
		 * original form for later use, though.
		 */
		rollup->gsets = remap_to_groupclause_idx(rollup->groupClause,
												 current_sets,
												 gd->tleref_to_colnum_map);
		rollup->gsets_data = current_sets;

		gd->rollups = lappend(gd->rollups, rollup);
	}

	if (gd->unsortable_sets)
	{
		/*
		 * We have not yet pinned down a groupclause for this, but we will
		 * need index-based lists for estimation purposes. Construct
		 * hash_sets_idx based on the entire original groupclause for now.
		 */
		gd->hash_sets_idx = remap_to_groupclause_idx(parse->groupClause,
													 gd->unsortable_sets,
													 gd->tleref_to_colnum_map);
		gd->any_hashable = true;
	}

	return gd;
}

/*
 * Given a groupclause and a list of GroupingSetData, return equivalent sets
 * (without annotation) mapped to indexes into the given groupclause.
 */
static List *
remap_to_groupclause_idx(List *groupClause,
						 List *gsets,
						 int *tleref_to_colnum_map)
{
	int			ref = 0;
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, groupClause)
	{
		SortGroupClause *gc = lfirst_node(SortGroupClause, lc);

		tleref_to_colnum_map[gc->tleSortGroupRef] = ref++;
	}

	foreach(lc, gsets)
	{
		List	   *set = NIL;
		ListCell   *lc2;
		GroupingSetData *gs = lfirst_node(GroupingSetData, lc);

		foreach(lc2, gs->set)
		{
			set = lappend_int(set, tleref_to_colnum_map[lfirst_int(lc2)]);
		}

		result = lappend(result, set);
	}

	return result;
}


/*
 * preprocess_rowmarks - set up PlanRowMarks if needed
 */
static void
preprocess_rowmarks(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	Bitmapset  *rels;
	List	   *prowmarks;
	ListCell   *l;
	int			i;

	if (parse->rowMarks)
	{
		/*
		 * We've got trouble if FOR [KEY] UPDATE/SHARE appears inside
		 * grouping, since grouping renders a reference to individual tuple
		 * CTIDs invalid.  This is also checked at parse time, but that's
		 * insufficient because of rule substitution, query pullup, etc.
		 */
		CheckSelectLocking(parse, linitial_node(RowMarkClause,
												parse->rowMarks)->strength);
	}
	else
	{
		/*
		 * We only need rowmarks for UPDATE, DELETE, or FOR [KEY]
		 * UPDATE/SHARE.
		 */
		if (parse->commandType != CMD_UPDATE &&
			parse->commandType != CMD_DELETE)
			return;
	}

	/*
	 * We need to have rowmarks for all base relations except the target. We
	 * make a bitmapset of all base rels and then remove the items we don't
	 * need or have FOR [KEY] UPDATE/SHARE marks for.
	 */
	rels = get_relids_in_jointree((Node *) parse->jointree, false);
	if (parse->resultRelation)
		rels = bms_del_member(rels, parse->resultRelation);

	/*
	 * Convert RowMarkClauses to PlanRowMark representation.
	 */
	prowmarks = NIL;
	foreach(l, parse->rowMarks)
	{
		RowMarkClause *rc = lfirst_node(RowMarkClause, l);
		RangeTblEntry *rte = rt_fetch(rc->rti, parse->rtable);
		PlanRowMark *newrc;

		/*
		 * Currently, it is syntactically impossible to have FOR UPDATE et al
		 * applied to an update/delete target rel.  If that ever becomes
		 * possible, we should drop the target from the PlanRowMark list.
		 */
		Assert(rc->rti != parse->resultRelation);

		/*
		 * Ignore RowMarkClauses for subqueries; they aren't real tables and
		 * can't support true locking.  Subqueries that got flattened into the
		 * main query should be ignored completely.  Any that didn't will get
		 * ROW_MARK_COPY items in the next loop.
		 */
		if (rte->rtekind != RTE_RELATION)
			continue;

		rels = bms_del_member(rels, rc->rti);

		newrc = makeNode(PlanRowMark);
		newrc->rti = newrc->prti = rc->rti;
		newrc->rowmarkId = ++(root->glob->lastRowMarkId);
		newrc->markType = select_rowmark_type(rte, rc->strength);
		newrc->allMarkTypes = (1 << newrc->markType);
		newrc->strength = rc->strength;
		newrc->waitPolicy = rc->waitPolicy;
		newrc->isParent = false;

		prowmarks = lappend(prowmarks, newrc);
	}

	/*
	 * Now, add rowmarks for any non-target, non-locked base relations.
	 */
	i = 0;
	foreach(l, parse->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, l);
		PlanRowMark *newrc;

		i++;
		if (!bms_is_member(i, rels))
			continue;

		newrc = makeNode(PlanRowMark);
		newrc->rti = newrc->prti = i;
		newrc->rowmarkId = ++(root->glob->lastRowMarkId);
		newrc->markType = select_rowmark_type(rte, LCS_NONE);
		newrc->allMarkTypes = (1 << newrc->markType);
		newrc->strength = LCS_NONE;
		newrc->waitPolicy = LockWaitBlock;	/* doesn't matter */
		newrc->isParent = false;

		prowmarks = lappend(prowmarks, newrc);
	}

	root->rowMarks = prowmarks;
}

/*
 * Select RowMarkType to use for a given table
 */
RowMarkType
select_rowmark_type(RangeTblEntry *rte, LockClauseStrength strength)
{
	if (rte->rtekind != RTE_RELATION)
	{
		/* If it's not a table at all, use ROW_MARK_COPY */
		return ROW_MARK_COPY;
	}
	else if (rte->relkind == RELKIND_FOREIGN_TABLE)
	{
		/* Let the FDW select the rowmark type, if it wants to */
		FdwRoutine *fdwroutine = GetFdwRoutineByRelId(rte->relid);

		if (fdwroutine->GetForeignRowMarkType != NULL)
			return fdwroutine->GetForeignRowMarkType(rte, strength);
		/* Otherwise, use ROW_MARK_COPY by default */
		return ROW_MARK_COPY;
	}
	else
	{
		/* Regular table, apply the appropriate lock type */
		switch (strength)
		{
			case LCS_NONE:

				/*
				 * We don't need a tuple lock, only the ability to re-fetch
				 * the row.
				 */
				return ROW_MARK_REFERENCE;
				break;
			case LCS_FORKEYSHARE:
				return ROW_MARK_KEYSHARE;
				break;
			case LCS_FORSHARE:
				return ROW_MARK_SHARE;
				break;
			case LCS_FORNOKEYUPDATE:
				return ROW_MARK_NOKEYEXCLUSIVE;
				break;
			case LCS_FORUPDATE:
				return ROW_MARK_EXCLUSIVE;
				break;
		}
		elog(ERROR, "unrecognized LockClauseStrength %d", (int) strength);
		return ROW_MARK_EXCLUSIVE;	/* keep compiler quiet */
	}
}

/*
 * preprocess_limit - 对LIMIT和/或OFFSET子句进行预估算
 *
 * 功能概述：
 *   此函数负责估算SQL查询中的LIMIT和OFFSET子句的值，并根据这些估算值调整元组分数，
 *   以帮助查询规划器做出更准确的执行计划选择。它处理常量值和表达式值，并针对不同情况
 *   采用不同的估算策略。
 *
 * 参数说明：
 *   root: PlannerInfo结构体指针，包含查询的所有规划信息
 *   tuple_fraction: 浮点数，表示调用者期望的元组分数（行数估计值）
 *                   - 当值 >= 1.0 时，表示期望的绝对行数
 *                   - 当值在 (0.0, 1.0) 之间时，表示期望的总行数比例
 *                   - 当值 <= 0.0 时，表示没有特定期望
 *   offset_est: 输出参数，int64指针，用于存储估算的OFFSET值
 *               - 0: 表示没有OFFSET子句
 *               - -1: 表示有OFFSET子句但无法估算其值
 *               - 其他正值: 表示估算的偏移量
 *   count_est: 输出参数，int64指针，用于存储估算的LIMIT值
 *              - 0: 表示没有LIMIT子句（或LIMIT ALL）
 *              - -1: 表示有LIMIT子句但无法估算其值
 *              - 其他正值: 表示估算的限制数量
 *
 * 返回值：
 *   调整后的元组分数，用于查询规划。该调整反映了规划器将采取的确定操作，
 *   而非对上下文的假设。
 */
static double
preprocess_limit(PlannerInfo *root, double tuple_fraction,
				 int64 *offset_est, int64 *count_est)
{
	Query	   *parse = root->parse;  /* 从规划器信息中获取查询解析树 */
	Node	   *est;                 /* 用于存储表达式估算结果的临时变量 */
	double		limit_fraction;       /* 存储计算得到的限制分数 */

	/* 断言：只有当存在LIMIT或OFFSET子句时才应该调用此函数 */
	Assert(parse->limitCount || parse->limitOffset);

	/*
	 * 尝试获取LIMIT子句的值。使用estimate_expression_value函数主要是因为它
	 * 有时能够处理Params类型的表达式。
	 */
	if (parse->limitCount)
	{
		/* 估算LIMIT表达式的值 */
		est = estimate_expression_value(root, parse->limitCount);
		
		/* 如果估算结果是非空的常量值 */
		if (est && IsA(est, Const))
		{
			/* 检查常量是否为NULL */
			if (((Const *) est)->constisnull)
			{
				/* NULL表示LIMIT ALL，即没有限制 */
				*count_est = 0; /* 当作不存在LIMIT处理 */
			}
			else
			{
				/* 从常量中提取LIMIT值并转换为int64类型 */
				*count_est = DatumGetInt64(((Const *) est)->constvalue);
				/* 确保LIMIT值至少为1（规划器的常规做法是不估算少于一行的结果） */
				if (*count_est <= 0)
					*count_est = 1;
			}
		}
		else
			*count_est = -1;  /* 无法估算LIMIT值 */
	}
	else
		*count_est = 0;     /* 不存在LIMIT子句 */

	/*
	 * 尝试获取OFFSET子句的值，处理逻辑与LIMIT类似
	 */
	if (parse->limitOffset)
	{
		/* 估算OFFSET表达式的值 */
		est = estimate_expression_value(root, parse->limitOffset);
		
		/* 如果估算结果是非空的常量值 */
		if (est && IsA(est, Const))
		{
			/* 检查常量是否为NULL */
			if (((Const *) est)->constisnull)
			{
				/* 将NULL视为没有偏移量；执行器也会这样处理 */
				*offset_est = 0;  /* 当作不存在OFFSET处理 */
			}
			else
			{
				/* 从常量中提取OFFSET值并转换为int64类型 */
				*offset_est = DatumGetInt64(((Const *) est)->constvalue);
				/* 确保OFFSET值不小于0 */
				if (*offset_est < 0)
					*offset_est = 0;
			}
		}
		else
			*offset_est = -1;  /* 无法估算OFFSET值 */
	}
	else
		*offset_est = 0;     /* 不存在OFFSET子句 */

	/*
	 * 处理存在LIMIT子句的情况
	 */
	if (*count_est != 0)
	{
		/*
		 * LIMIT子句限制了返回的元组绝对数量。但是，如果它不是常量LIMIT，我们就需要猜测；
		 * 由于没有更好的方法，我们假设需要获取计划结果的10%。
		 */
		if (*count_est < 0 || *offset_est < 0)
		{
			/* LIMIT或OFFSET是表达式，无法准确估算，使用10%的启发式值 */
			limit_fraction = 0.10;
		}
		else
		{
			/* LIMIT（加上OFFSET，如果有的话）是所需的最大元组数量 */
			limit_fraction = (double) *count_est + (double) *offset_est;
		}

		/*
		 * 如果调用者和LIMIT都提供了绝对限制，使用较小的值；如果两者都是分数值，同理。
		 * 如果一个是分数值而另一个是绝对值，我们很难确定哪个更小，但我们使用启发式
		 * 方法假设绝对值通常更小。
		 */
		if (tuple_fraction >= 1.0)
		{
			/* 调用者提供的是绝对值 */
			if (limit_fraction >= 1.0)
			{
				/* 两者都是绝对值，取较小值 */
				tuple_fraction = Min(tuple_fraction, limit_fraction);
			}
			else
			{
				/* 调用者提供绝对值，limit_fraction是分数值；保留调用者的值 */
			}
		}
		else if (tuple_fraction > 0.0)
		{
			/* 调用者提供的是分数值 */
			if (limit_fraction >= 1.0)
			{
				/* 调用者提供分数值，limit_fraction是绝对值；使用limit_fraction */
				tuple_fraction = limit_fraction;
			}
			else
			{
				/* 两者都是分数值，取较小值 */
				tuple_fraction = Min(tuple_fraction, limit_fraction);
			}
		}
		else
		{
			/* 调用者没有提供信息，直接使用limit_fraction */
			tuple_fraction = limit_fraction;
		}
	}
	/*
	 * 处理只有OFFSET没有LIMIT的情况
	 */
	else if (*offset_est != 0 && tuple_fraction > 0.0)
	{
		/*
		 * 只有OFFSET没有LIMIT的情况与LIMIT情况完全不同：在这里，我们需要增加而不是减少
		 * 调用者的tuple_fraction，因为OFFSET会导致获取更多的元组而不是更少。不过，这
		 * 只在我们获得的tuple_fraction > 0时才有意义。
		 *
		 * 与上面类似，如果OFFSET存在但无法估算，则使用10%。
		 */
		if (*offset_est < 0)
			limit_fraction = 0.10;  /* 无法估算OFFSET，使用10%的启发式值 */
		else
			limit_fraction = (double) *offset_est;  /* 使用估算的OFFSET值 */

		/*
		 * 如果调用者和OFFSET都提供了绝对计数，将它们相加；如果两者都是分数值，同理。
		 * 如果一个是分数值而另一个是绝对值，我们要取较大的值，并且我们启发式地假设
		 * 分数值更大。
		 */
		if (tuple_fraction >= 1.0)
		{
			/* 调用者提供的是绝对值 */
			if (limit_fraction >= 1.0)
			{
				/* 两者都是绝对值，将它们相加 */
				tuple_fraction += limit_fraction;
			}
			else
			{
				/* 调用者提供绝对值，limit_fraction是分数值；使用limit_fraction */
				tuple_fraction = limit_fraction;
			}
		}
		else
		{
			/* 调用者提供的是分数值 */
			if (limit_fraction >= 1.0)
			{
				/* 调用者提供分数值，limit_fraction是绝对值；保留调用者的值 */
			}
			else
			{
				/* 两者都是分数值，将它们相加 */
				tuple_fraction += limit_fraction;
				/* 如果总和大于等于1.0，则视为需要获取所有元组 */
				if (tuple_fraction >= 1.0)
					tuple_fraction = 0.0;  /* 假设获取全部元组 */
			}
		}
	}

	/* 返回调整后的元组分数，用于后续的查询规划 */
	return tuple_fraction;
}


/*
 * limit_needed - do we actually need a Limit plan node?
 *
 * If we have constant-zero OFFSET and constant-null LIMIT, we can skip adding
 * a Limit node.  This is worth checking for because "OFFSET 0" is a common
 * locution for an optimization fence.  (Because other places in the planner
 * merely check whether parse->limitOffset isn't NULL, it will still work as
 * an optimization fence --- we're just suppressing unnecessary run-time
 * overhead.)
 *
 * This might look like it could be merged into preprocess_limit, but there's
 * a key distinction: here we need hard constants in OFFSET/LIMIT, whereas
 * in preprocess_limit it's good enough to consider estimated values.
 */
bool
limit_needed(Query *parse)
{
	Node	   *node;

	node = parse->limitCount;
	if (node)
	{
		if (IsA(node, Const))
		{
			/* NULL indicates LIMIT ALL, ie, no limit */
			if (!((Const *) node)->constisnull)
				return true;	/* LIMIT with a constant value */
		}
		else
			return true;		/* non-constant LIMIT */
	}

	node = parse->limitOffset;
	if (node)
	{
		if (IsA(node, Const))
		{
			/* Treat NULL as no offset; the executor would too */
			if (!((Const *) node)->constisnull)
			{
				int64		offset = DatumGetInt64(((Const *) node)->constvalue);

				if (offset != 0)
					return true;	/* OFFSET with a nonzero value */
			}
		}
		else
			return true;		/* non-constant OFFSET */
	}

	return false;				/* don't need a Limit plan node */
}


/*
 * remove_useless_groupby_columns
 *		Remove any columns in the GROUP BY clause that are redundant due to
 *		being functionally dependent on other GROUP BY columns.
 *
 * Since some other DBMSes do not allow references to ungrouped columns, it's
 * not unusual to find all columns listed in GROUP BY even though listing the
 * primary-key columns would be sufficient.  Deleting such excess columns
 * avoids redundant sorting work, so it's worth doing.  When we do this, we
 * must mark the plan as dependent on the pkey constraint (compare the
 * parser's check_ungrouped_columns() and check_functional_grouping()).
 *
 * In principle, we could treat any NOT-NULL columns appearing in a UNIQUE
 * index as the determining columns.  But as with check_functional_grouping(),
 * there's currently no way to represent dependency on a NOT NULL constraint,
 * so we consider only the pkey for now.
 */
static void
remove_useless_groupby_columns(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	Bitmapset **groupbyattnos;
	Bitmapset **surplusvars;
	ListCell   *lc;
	int			relid;

	/* No chance to do anything if there are less than two GROUP BY items */
	if (list_length(parse->groupClause) < 2)
		return;

	/* Don't fiddle with the GROUP BY clause if the query has grouping sets */
	if (parse->groupingSets)
		return;

	/*
	 * Scan the GROUP BY clause to find GROUP BY items that are simple Vars.
	 * Fill groupbyattnos[k] with a bitmapset of the column attnos of RTE k
	 * that are GROUP BY items.
	 */
	groupbyattnos = (Bitmapset **) palloc0(sizeof(Bitmapset *) *
										   (list_length(parse->rtable) + 1));
	foreach(lc, parse->groupClause)
	{
		SortGroupClause *sgc = lfirst_node(SortGroupClause, lc);
		TargetEntry *tle = get_sortgroupclause_tle(sgc, parse->targetList);
		Var		   *var = (Var *) tle->expr;

		/*
		 * Ignore non-Vars and Vars from other query levels.
		 *
		 * XXX in principle, stable expressions containing Vars could also be
		 * removed, if all the Vars are functionally dependent on other GROUP
		 * BY items.  But it's not clear that such cases occur often enough to
		 * be worth troubling over.
		 */
		if (!IsA(var, Var) ||
			var->varlevelsup > 0)
			continue;

		/* OK, remember we have this Var */
		relid = var->varno;
		Assert(relid <= list_length(parse->rtable));
		groupbyattnos[relid] = bms_add_member(groupbyattnos[relid],
											  var->varattno - FirstLowInvalidHeapAttributeNumber);
	}

	/*
	 * Consider each relation and see if it is possible to remove some of its
	 * Vars from GROUP BY.  For simplicity and speed, we do the actual removal
	 * in a separate pass.  Here, we just fill surplusvars[k] with a bitmapset
	 * of the column attnos of RTE k that are removable GROUP BY items.
	 */
	surplusvars = NULL;			/* don't allocate array unless required */
	relid = 0;
	foreach(lc, parse->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);
		Bitmapset  *relattnos;
		Bitmapset  *pkattnos;
		Oid			constraintOid;

		relid++;

		/* Only plain relations could have primary-key constraints */
		if (rte->rtekind != RTE_RELATION)
			continue;

		/*
		 * We must skip inheritance parent tables as some of the child rels
		 * may cause duplicate rows.  This cannot happen with partitioned
		 * tables, however.
		 */
		if (rte->inh && rte->relkind != RELKIND_PARTITIONED_TABLE)
			continue;

		/* Nothing to do unless this rel has multiple Vars in GROUP BY */
		relattnos = groupbyattnos[relid];
		if (bms_membership(relattnos) != BMS_MULTIPLE)
			continue;

		/*
		 * Can't remove any columns for this rel if there is no suitable
		 * (i.e., nondeferrable) primary key constraint.
		 */
		pkattnos = get_primary_key_attnos(rte->relid, false, &constraintOid);
		if (pkattnos == NULL)
			continue;

		/*
		 * If the primary key is a proper subset of relattnos then we have
		 * some items in the GROUP BY that can be removed.
		 */
		if (bms_subset_compare(pkattnos, relattnos) == BMS_SUBSET1)
		{
			/*
			 * To easily remember whether we've found anything to do, we don't
			 * allocate the surplusvars[] array until we find something.
			 */
			if (surplusvars == NULL)
				surplusvars = (Bitmapset **) palloc0(sizeof(Bitmapset *) *
													 (list_length(parse->rtable) + 1));

			/* Remember the attnos of the removable columns */
			surplusvars[relid] = bms_difference(relattnos, pkattnos);

			/* Also, mark the resulting plan as dependent on this constraint */
			parse->constraintDeps = lappend_oid(parse->constraintDeps,
												constraintOid);
		}
	}

	/*
	 * If we found any surplus Vars, build a new GROUP BY clause without them.
	 * (Note: this may leave some TLEs with unreferenced ressortgroupref
	 * markings, but that's harmless.)
	 */
	if (surplusvars != NULL)
	{
		List	   *new_groupby = NIL;

		foreach(lc, parse->groupClause)
		{
			SortGroupClause *sgc = lfirst_node(SortGroupClause, lc);
			TargetEntry *tle = get_sortgroupclause_tle(sgc, parse->targetList);
			Var		   *var = (Var *) tle->expr;

			/*
			 * New list must include non-Vars, outer Vars, and anything not
			 * marked as surplus.
			 */
			if (!IsA(var, Var) ||
				var->varlevelsup > 0 ||
				!bms_is_member(var->varattno - FirstLowInvalidHeapAttributeNumber,
							   surplusvars[var->varno]))
				new_groupby = lappend(new_groupby, sgc);
		}

		parse->groupClause = new_groupby;
	}
}

/*
 * preprocess_groupclause - do preparatory work on GROUP BY clause
 *
 * The idea here is to adjust the ordering of the GROUP BY elements
 * (which in itself is semantically insignificant) to match ORDER BY,
 * thereby allowing a single sort operation to both implement the ORDER BY
 * requirement and set up for a Unique step that implements GROUP BY.
 *
 * In principle it might be interesting to consider other orderings of the
 * GROUP BY elements, which could match the sort ordering of other
 * possible plans (eg an indexscan) and thereby reduce cost.  We don't
 * bother with that, though.  Hashed grouping will frequently win anyway.
 *
 * Note: we need no comparable processing of the distinctClause because
 * the parser already enforced that that matches ORDER BY.
 *
 * For grouping sets, the order of items is instead forced to agree with that
 * of the grouping set (and items not in the grouping set are skipped). The
 * work of sorting the order of grouping set elements to match the ORDER BY if
 * possible is done elsewhere.
 */
static List *
preprocess_groupclause(PlannerInfo *root, List *force)
{
	Query	   *parse = root->parse;
	List	   *new_groupclause = NIL;
	bool		partial_match;
	ListCell   *sl;
	ListCell   *gl;

	/* For grouping sets, we need to force the ordering */
	if (force)
	{
		foreach(sl, force)
		{
			Index		ref = lfirst_int(sl);
			SortGroupClause *cl = get_sortgroupref_clause(ref, parse->groupClause);

			new_groupclause = lappend(new_groupclause, cl);
		}

		return new_groupclause;
	}

	/* If no ORDER BY, nothing useful to do here */
	if (parse->sortClause == NIL)
		return parse->groupClause;

	/*
	 * Scan the ORDER BY clause and construct a list of matching GROUP BY
	 * items, but only as far as we can make a matching prefix.
	 *
	 * This code assumes that the sortClause contains no duplicate items.
	 */
	foreach(sl, parse->sortClause)
	{
		SortGroupClause *sc = lfirst_node(SortGroupClause, sl);

		foreach(gl, parse->groupClause)
		{
			SortGroupClause *gc = lfirst_node(SortGroupClause, gl);

			if (equal(gc, sc))
			{
				new_groupclause = lappend(new_groupclause, gc);
				break;
			}
		}
		if (gl == NULL)
			break;				/* no match, so stop scanning */
	}

	/* Did we match all of the ORDER BY list, or just some of it? */
	partial_match = (sl != NULL);

	/* If no match at all, no point in reordering GROUP BY */
	if (new_groupclause == NIL)
		return parse->groupClause;

	/*
	 * Add any remaining GROUP BY items to the new list, but only if we were
	 * able to make a complete match.  In other words, we only rearrange the
	 * GROUP BY list if the result is that one list is a prefix of the other
	 * --- otherwise there's no possibility of a common sort.  Also, give up
	 * if there are any non-sortable GROUP BY items, since then there's no
	 * hope anyway.
	 */
	foreach(gl, parse->groupClause)
	{
		SortGroupClause *gc = lfirst_node(SortGroupClause, gl);

		if (list_member_ptr(new_groupclause, gc))
			continue;			/* it matched an ORDER BY item */
		if (partial_match)
			return parse->groupClause;	/* give up, no common sort possible */
		if (!OidIsValid(gc->sortop))
			return parse->groupClause;	/* give up, GROUP BY can't be sorted */
		new_groupclause = lappend(new_groupclause, gc);
	}

	/* Success --- install the rearranged GROUP BY list */
	Assert(list_length(parse->groupClause) == list_length(new_groupclause));
	return new_groupclause;
}

/*
 * Extract lists of grouping sets that can be implemented using a single
 * rollup-type aggregate pass each. Returns a list of lists of grouping sets.
 *
 * Input must be sorted with smallest sets first. Result has each sublist
 * sorted with smallest sets first.
 *
 * We want to produce the absolute minimum possible number of lists here to
 * avoid excess sorts. Fortunately, there is an algorithm for this; the problem
 * of finding the minimal partition of a partially-ordered set into chains
 * (which is what we need, taking the list of grouping sets as a poset ordered
 * by set inclusion) can be mapped to the problem of finding the maximum
 * cardinality matching on a bipartite graph, which is solvable in polynomial
 * time with a worst case of no worse than O(n^2.5) and usually much
 * better. Since our N is at most 4096, we don't need to consider fallbacks to
 * heuristic or approximate methods.  (Planning time for a 12-d cube is under
 * half a second on my modest system even with optimization off and assertions
 * on.)
 */
static List *
extract_rollup_sets(List *groupingSets)
{
	int			num_sets_raw = list_length(groupingSets);
	int			num_empty = 0;
	int			num_sets = 0;	/* distinct sets */
	int			num_chains = 0;
	List	   *result = NIL;
	List	  **results;
	List	  **orig_sets;
	Bitmapset **set_masks;
	int		   *chains;
	short	  **adjacency;
	short	   *adjacency_buf;
	BipartiteMatchState *state;
	int			i;
	int			j;
	int			j_size;
	ListCell   *lc1 = list_head(groupingSets);
	ListCell   *lc;

	/*
	 * Start by stripping out empty sets.  The algorithm doesn't require this,
	 * but the planner currently needs all empty sets to be returned in the
	 * first list, so we strip them here and add them back after.
	 */
	while (lc1 && lfirst(lc1) == NIL)
	{
		++num_empty;
		lc1 = lnext(lc1);
	}

	/* bail out now if it turns out that all we had were empty sets. */
	if (!lc1)
		return list_make1(groupingSets);

	/*----------
	 * We don't strictly need to remove duplicate sets here, but if we don't,
	 * they tend to become scattered through the result, which is a bit
	 * confusing (and irritating if we ever decide to optimize them out).
	 * So we remove them here and add them back after.
	 *
	 * For each non-duplicate set, we fill in the following:
	 *
	 * orig_sets[i] = list of the original set lists
	 * set_masks[i] = bitmapset for testing inclusion
	 * adjacency[i] = array [n, v1, v2, ... vn] of adjacency indices
	 *
	 * chains[i] will be the result group this set is assigned to.
	 *
	 * We index all of these from 1 rather than 0 because it is convenient
	 * to leave 0 free for the NIL node in the graph algorithm.
	 *----------
	 */
	orig_sets = palloc0((num_sets_raw + 1) * sizeof(List *));
	set_masks = palloc0((num_sets_raw + 1) * sizeof(Bitmapset *));
	adjacency = palloc0((num_sets_raw + 1) * sizeof(short *));
	adjacency_buf = palloc((num_sets_raw + 1) * sizeof(short));

	j_size = 0;
	j = 0;
	i = 1;

	for_each_cell(lc, lc1)
	{
		List	   *candidate = (List *) lfirst(lc);
		Bitmapset  *candidate_set = NULL;
		ListCell   *lc2;
		int			dup_of = 0;

		foreach(lc2, candidate)
		{
			candidate_set = bms_add_member(candidate_set, lfirst_int(lc2));
		}

		/* we can only be a dup if we're the same length as a previous set */
		if (j_size == list_length(candidate))
		{
			int			k;

			for (k = j; k < i; ++k)
			{
				if (bms_equal(set_masks[k], candidate_set))
				{
					dup_of = k;
					break;
				}
			}
		}
		else if (j_size < list_length(candidate))
		{
			j_size = list_length(candidate);
			j = i;
		}

		if (dup_of > 0)
		{
			orig_sets[dup_of] = lappend(orig_sets[dup_of], candidate);
			bms_free(candidate_set);
		}
		else
		{
			int			k;
			int			n_adj = 0;

			orig_sets[i] = list_make1(candidate);
			set_masks[i] = candidate_set;

			/* fill in adjacency list; no need to compare equal-size sets */

			for (k = j - 1; k > 0; --k)
			{
				if (bms_is_subset(set_masks[k], candidate_set))
					adjacency_buf[++n_adj] = k;
			}

			if (n_adj > 0)
			{
				adjacency_buf[0] = n_adj;
				adjacency[i] = palloc((n_adj + 1) * sizeof(short));
				memcpy(adjacency[i], adjacency_buf, (n_adj + 1) * sizeof(short));
			}
			else
				adjacency[i] = NULL;

			++i;
		}
	}

	num_sets = i - 1;

	/*
	 * Apply the graph matching algorithm to do the work.
	 */
	state = BipartiteMatch(num_sets, num_sets, adjacency);

	/*
	 * Now, the state->pair* fields have the info we need to assign sets to
	 * chains. Two sets (u,v) belong to the same chain if pair_uv[u] = v or
	 * pair_vu[v] = u (both will be true, but we check both so that we can do
	 * it in one pass)
	 */
	chains = palloc0((num_sets + 1) * sizeof(int));

	for (i = 1; i <= num_sets; ++i)
	{
		int			u = state->pair_vu[i];
		int			v = state->pair_uv[i];

		if (u > 0 && u < i)
			chains[i] = chains[u];
		else if (v > 0 && v < i)
			chains[i] = chains[v];
		else
			chains[i] = ++num_chains;
	}

	/* build result lists. */
	results = palloc0((num_chains + 1) * sizeof(List *));

	for (i = 1; i <= num_sets; ++i)
	{
		int			c = chains[i];

		Assert(c > 0);

		results[c] = list_concat(results[c], orig_sets[i]);
	}

	/* push any empty sets back on the first list. */
	while (num_empty-- > 0)
		results[1] = lcons(NIL, results[1]);

	/* make result list */
	for (i = 1; i <= num_chains; ++i)
		result = lappend(result, results[i]);

	/*
	 * Free all the things.
	 *
	 * (This is over-fussy for small sets but for large sets we could have
	 * tied up a nontrivial amount of memory.)
	 */
	BipartiteMatchFree(state);
	pfree(results);
	pfree(chains);
	for (i = 1; i <= num_sets; ++i)
		if (adjacency[i])
			pfree(adjacency[i]);
	pfree(adjacency);
	pfree(adjacency_buf);
	pfree(orig_sets);
	for (i = 1; i <= num_sets; ++i)
		bms_free(set_masks[i]);
	pfree(set_masks);

	return result;
}

/*
 * Reorder the elements of a list of grouping sets such that they have correct
 * prefix relationships. Also inserts the GroupingSetData annotations.
 *
 * The input must be ordered with smallest sets first; the result is returned
 * with largest sets first.  Note that the result shares no list substructure
 * with the input, so it's safe for the caller to modify it later.
 *
 * If we're passed in a sortclause, we follow its order of columns to the
 * extent possible, to minimize the chance that we add unnecessary sorts.
 * (We're trying here to ensure that GROUPING SETS ((a,b,c),(c)) ORDER BY c,b,a
 * gets implemented in one pass.)
 */
static List *
reorder_grouping_sets(List *groupingsets, List *sortclause)
{
	ListCell   *lc;
	List	   *previous = NIL;
	List	   *result = NIL;

	foreach(lc, groupingsets)
	{
		List	   *candidate = (List *) lfirst(lc);
		List	   *new_elems = list_difference_int(candidate, previous);
		GroupingSetData *gs = makeNode(GroupingSetData);

		while (list_length(sortclause) > list_length(previous) &&
			   list_length(new_elems) > 0)
		{
			SortGroupClause *sc = list_nth(sortclause, list_length(previous));
			int			ref = sc->tleSortGroupRef;

			if (list_member_int(new_elems, ref))
			{
				previous = lappend_int(previous, ref);
				new_elems = list_delete_int(new_elems, ref);
			}
			else
			{
				/* diverged from the sortclause; give up on it */
				sortclause = NIL;
				break;
			}
		}

		/*
		 * Safe to use list_concat (which shares cells of the second arg)
		 * because we know that new_elems does not share cells with anything.
		 */
		previous = list_concat(previous, new_elems);

		gs->set = list_copy(previous);
		result = lcons(gs, result);
	}

	list_free(previous);

	return result;
}

/*
 * Compute query_pathkeys and other pathkeys during plan generation
 */
static void
standard_qp_callback(PlannerInfo *root, void *extra)
{
	Query	   *parse = root->parse;
	standard_qp_extra *qp_extra = (standard_qp_extra *) extra;
	List	   *tlist = root->processed_tlist;
	List	   *activeWindows = qp_extra->activeWindows;

	/*
	 * Calculate pathkeys that represent grouping/ordering requirements.  The
	 * sortClause is certainly sort-able, but GROUP BY and DISTINCT might not
	 * be, in which case we just leave their pathkeys empty.
	 */
	if (qp_extra->groupClause &&
		grouping_is_sortable(qp_extra->groupClause))
		root->group_pathkeys =
			make_pathkeys_for_sortclauses(root,
										  qp_extra->groupClause,
										  tlist);
	else
		root->group_pathkeys = NIL;

	/* We consider only the first (bottom) window in pathkeys logic */
	if (activeWindows != NIL)
	{
		WindowClause *wc = linitial_node(WindowClause, activeWindows);

		root->window_pathkeys = make_pathkeys_for_window(root,
														 wc,
														 tlist);
	}
	else
		root->window_pathkeys = NIL;

	if (parse->distinctClause &&
		grouping_is_sortable(parse->distinctClause))
		root->distinct_pathkeys =
			make_pathkeys_for_sortclauses(root,
										  parse->distinctClause,
										  tlist);
	else
		root->distinct_pathkeys = NIL;

	root->sort_pathkeys =
		make_pathkeys_for_sortclauses(root,
									  parse->sortClause,
									  tlist);

	/*
	 * Figure out whether we want a sorted result from query_planner.
	 *
	 * If we have a sortable GROUP BY clause, then we want a result sorted
	 * properly for grouping.  Otherwise, if we have window functions to
	 * evaluate, we try to sort for the first window.  Otherwise, if there's a
	 * sortable DISTINCT clause that's more rigorous than the ORDER BY clause,
	 * we try to produce output that's sufficiently well sorted for the
	 * DISTINCT.  Otherwise, if there is an ORDER BY clause, we want to sort
	 * by the ORDER BY clause.
	 *
	 * Note: if we have both ORDER BY and GROUP BY, and ORDER BY is a superset
	 * of GROUP BY, it would be tempting to request sort by ORDER BY --- but
	 * that might just leave us failing to exploit an available sort order at
	 * all.  Needs more thought.  The choice for DISTINCT versus ORDER BY is
	 * much easier, since we know that the parser ensured that one is a
	 * superset of the other.
	 */
	if (root->group_pathkeys)
		root->query_pathkeys = root->group_pathkeys;
	else if (root->window_pathkeys)
		root->query_pathkeys = root->window_pathkeys;
	else if (list_length(root->distinct_pathkeys) >
			 list_length(root->sort_pathkeys))
		root->query_pathkeys = root->distinct_pathkeys;
	else if (root->sort_pathkeys)
		root->query_pathkeys = root->sort_pathkeys;
	else
		root->query_pathkeys = NIL;
}

/*
 * 估计分组子句产生的组数（如果不分组则为1）
 *
 * path_rows: 扫描/连接步骤的输出行数
 * gd: 分组集数据，包括分组集列表及其子句
 * target_list: 包含分组子句引用的目标列表
 *
 * 如果执行分组集操作，我们还会为每个分组集和每个单独的rollup列表注释
 * 估算值，以便稍后确定是否可以用哈希方式替代某些组合。
 */
static double
get_number_of_groups(PlannerInfo *root,  /* 规划器信息结构体指针 */
                     double path_rows,  /* 输入行数估计 */
                     grouping_sets_data *gd,  /* 分组集数据结构体指针 */
                     List *target_list)  /* 目标列表达式列表 */
{
    Query   *parse = root->parse;  /* 解析树指针 */
    double  dNumGroups;  /* 最终返回的组数估计值 */

    /* 检查是否有GROUP BY子句 */
    if (parse->groupClause)
    {
        List   *groupExprs;  /* 分组表达式列表 */

        /* 检查是否使用了分组集(GROUPING SETS) */
        if (parse->groupingSets)
        {
            /* 累加每个分组集的估计值 */
            ListCell   *lc;
            ListCell   *lc2;

            Assert(gd);  /* 确保分组集数据不为空，保持Coverity检查愉快 */

            dNumGroups = 0;  /* 初始化总组数计数器 */

            /* 遍历每个rollup结构 */
            foreach(lc, gd->rollups)
            {
                RollupData *rollup = lfirst_node(RollupData, lc);
                ListCell   *lc;

                /* 获取rollup分组子句对应的实际表达式 */
                groupExprs = get_sortgrouplist_exprs(rollup->groupClause,
                                                    target_list);

                rollup->numGroups = 0.0;  /* 初始化此rollup的组数计数器 */

                /* 同时遍历分组集和对应的分组集数据 */
                forboth(lc, rollup->gsets, lc2, rollup->gsets_data)
                {
                    List   *gset = (List *) lfirst(lc);  /* 当前分组集 */
                    GroupingSetData *gs = lfirst_node(GroupingSetData, lc2);  /* 分组集数据 */
                    /* 估计此分组集产生的组数 */
                    double  numGroups = estimate_num_groups(root,
                                                           groupExprs,
                                                           path_rows,
                                                           &gset);

                    gs->numGroups = numGroups;  /* 记录每个分组集的组数估计 */
                    rollup->numGroups += numGroups;  /* 累加到此rollup的总组数 */
                }

                dNumGroups += rollup->numGroups;  /* 将此rollup的组数累加到总数 */
            }

            /* 处理不可排序需要使用哈希的分组集 */
            if (gd->hash_sets_idx)
            {
                ListCell   *lc;

                gd->dNumHashGroups = 0;  /* 初始化哈希分组组数计数器 */

                /* 获取完整GROUP BY子句对应的表达式 */
                groupExprs = get_sortgrouplist_exprs(parse->groupClause,
                                                    target_list);

                /* 同时遍历哈希分组集索引和不可排序分组集数据 */
                forboth(lc, gd->hash_sets_idx, lc2, gd->unsortable_sets)
                {
                    List   *gset = (List *) lfirst(lc);  /* 当前哈希分组集 */
                    GroupingSetData *gs = lfirst_node(GroupingSetData, lc2);  /* 分组集数据 */
                    /* 估计此哈希分组集产生的组数 */
                    double  numGroups = estimate_num_groups(root,
                                                           groupExprs,
                                                           path_rows,
                                                           &gset);

                    gs->numGroups = numGroups;  /* 记录每个哈希分组集的组数估计 */
                    gd->dNumHashGroups += numGroups;  /* 累加哈希分组组数 */
                }

                dNumGroups += gd->dNumHashGroups;  /* 将哈希分组组数累加到总数 */
            }
        }
        else
        {
            /* 简单GROUP BY情况 */
            groupExprs = get_sortgrouplist_exprs(parse->groupClause,
                                                target_list);

            /* 估计简单GROUP BY产生的组数 */
            dNumGroups = estimate_num_groups(root, groupExprs, path_rows,
                                            NULL);
        }
    }
    else if (parse->groupingSets)
    {
        /* 空分组集情况...每个分组集产生一行结果 */
        dNumGroups = list_length(parse->groupingSets);
    }
    else if (parse->hasAggs || root->hasHavingQual)
    {
        /* 简单聚合，只有一行结果 */
        dNumGroups = 1;
    }
    else
    {
        /* 不分组，返回1 */
        dNumGroups = 1;
    }

    return dNumGroups;  /* 返回最终估计的组数 */
}


/*
 * create_grouping_paths
 *
 * Build a new upperrel containing Paths for grouping and/or aggregation.
 * Along the way, we also build an upperrel for Paths which are partially
 * grouped and/or aggregated.  A partially grouped and/or aggregated path
 * needs a FinalizeAggregate node to complete the aggregation.  Currently,
 * the only partially grouped paths we build are also partial paths; that
 * is, they need a Gather and then a FinalizeAggregate.
 *
 * input_rel: contains the source-data Paths
 * target: the pathtarget for the result Paths to compute
 * agg_costs: cost info about all aggregates in query (in AGGSPLIT_SIMPLE mode)
 * gd: grouping sets data including list of grouping sets and their clauses
 *
 * Note: all Paths in input_rel are expected to return the target computed
 * by make_group_input_target.
 */
static RelOptInfo *
create_grouping_paths(PlannerInfo *root,
					  RelOptInfo *input_rel,
					  PathTarget *target,
					  bool target_parallel_safe,
					  const AggClauseCosts *agg_costs,
					  grouping_sets_data *gd)
{
	Query	   *parse = root->parse;
	RelOptInfo *grouped_rel;
	RelOptInfo *partially_grouped_rel;

	/*
	 * Create grouping relation to hold fully aggregated grouping and/or
	 * aggregation paths.
	 */
	grouped_rel = make_grouping_rel(root, input_rel, target,
									target_parallel_safe, parse->havingQual);

	/*
	 * Create either paths for a degenerate grouping or paths for ordinary
	 * grouping, as appropriate.
	 */
	if (is_degenerate_grouping(root))
		create_degenerate_grouping_paths(root, input_rel, grouped_rel);
	else
	{
		int			flags = 0;
		GroupPathExtraData extra;

		/*
		 * Determine whether it's possible to perform sort-based
		 * implementations of grouping.  (Note that if groupClause is empty,
		 * grouping_is_sortable() is trivially true, and all the
		 * pathkeys_contained_in() tests will succeed too, so that we'll
		 * consider every surviving input path.)
		 *
		 * If we have grouping sets, we might be able to sort some but not all
		 * of them; in this case, we need can_sort to be true as long as we
		 * must consider any sorted-input plan.
		 */
		if ((gd && gd->rollups != NIL)
			|| grouping_is_sortable(parse->groupClause))
			flags |= GROUPING_CAN_USE_SORT;

		/*
		 * Determine whether we should consider hash-based implementations of
		 * grouping.
		 *
		 * Hashed aggregation only applies if we're grouping. If we have
		 * grouping sets, some groups might be hashable but others not; in
		 * this case we set can_hash true as long as there is nothing globally
		 * preventing us from hashing (and we should therefore consider plans
		 * with hashes).
		 *
		 * Executor doesn't support hashed aggregation with DISTINCT or ORDER
		 * BY aggregates.  (Doing so would imply storing *all* the input
		 * values in the hash table, and/or running many sorts in parallel,
		 * either of which seems like a certain loser.)  We similarly don't
		 * support ordered-set aggregates in hashed aggregation, but that case
		 * is also included in the numOrderedAggs count.
		 *
		 * Note: grouping_is_hashable() is much more expensive to check than
		 * the other gating conditions, so we want to do it last.
		 */
		if ((parse->groupClause != NIL &&
			 agg_costs->numOrderedAggs == 0 &&
			 (gd ? gd->any_hashable : grouping_is_hashable(parse->groupClause))))
			flags |= GROUPING_CAN_USE_HASH;

		/*
		 * Determine whether partial aggregation is possible.
		 */
		if (can_partial_agg(root, agg_costs))
			flags |= GROUPING_CAN_PARTIAL_AGG;

		extra.flags = flags;
		extra.target_parallel_safe = target_parallel_safe;
		extra.havingQual = parse->havingQual;
		extra.targetList = parse->targetList;
		extra.partial_costs_set = false;

		/*
		 * Determine whether partitionwise aggregation is in theory possible.
		 * It can be disabled by the user, and for now, we don't try to
		 * support grouping sets.  create_ordinary_grouping_paths() will check
		 * additional conditions, such as whether input_rel is partitioned.
		 */
		if (enable_partitionwise_aggregate && !parse->groupingSets)
			extra.patype = PARTITIONWISE_AGGREGATE_FULL;
		else
			extra.patype = PARTITIONWISE_AGGREGATE_NONE;

		create_ordinary_grouping_paths(root, input_rel, grouped_rel,
									   agg_costs, gd, &extra,
									   &partially_grouped_rel);
	}

	set_cheapest(grouped_rel);
	return grouped_rel;
}

/*
 * make_grouping_rel
 *
 * Create a new grouping rel and set basic properties.
 *
 * input_rel represents the underlying scan/join relation.
 * target is the output expected from the grouping relation.
 */
static RelOptInfo *
make_grouping_rel(PlannerInfo *root, RelOptInfo *input_rel,
				  PathTarget *target, bool target_parallel_safe,
				  Node *havingQual)
{
	RelOptInfo *grouped_rel;

	if (IS_OTHER_REL(input_rel))
	{
		grouped_rel = fetch_upper_rel(root, UPPERREL_GROUP_AGG,
									  input_rel->relids);
		grouped_rel->reloptkind = RELOPT_OTHER_UPPER_REL;
	}
	else
	{
		/*
		 * By tradition, the relids set for the main grouping relation is
		 * NULL.  (This could be changed, but might require adjustments
		 * elsewhere.)
		 */
		grouped_rel = fetch_upper_rel(root, UPPERREL_GROUP_AGG, NULL);
	}

	/* Set target. */
	grouped_rel->reltarget = target;

	/*
	 * If the input relation is not parallel-safe, then the grouped relation
	 * can't be parallel-safe, either.  Otherwise, it's parallel-safe if the
	 * target list and HAVING quals are parallel-safe.
	 */
	if (input_rel->consider_parallel && target_parallel_safe &&
		is_parallel_safe(root, (Node *) havingQual))
		grouped_rel->consider_parallel = true;

	/*
	 * If the input rel belongs to a single FDW, so does the grouped rel.
	 */
	grouped_rel->serverid = input_rel->serverid;
	grouped_rel->userid = input_rel->userid;
	grouped_rel->useridiscurrent = input_rel->useridiscurrent;
	grouped_rel->fdwroutine = input_rel->fdwroutine;

	return grouped_rel;
}

/*
 * is_degenerate_grouping
 *
 * A degenerate grouping is one in which the query has a HAVING qual and/or
 * grouping sets, but no aggregates and no GROUP BY (which implies that the
 * grouping sets are all empty).
 */
static bool
is_degenerate_grouping(PlannerInfo *root)
{
	Query	   *parse = root->parse;

	return (root->hasHavingQual || parse->groupingSets) &&
		!parse->hasAggs && parse->groupClause == NIL;
}

/*
 * create_degenerate_grouping_paths
 *
 * When the grouping is degenerate (see is_degenerate_grouping), we are
 * supposed to emit either zero or one row for each grouping set depending on
 * whether HAVING succeeds.  Furthermore, there cannot be any variables in
 * either HAVING or the targetlist, so we actually do not need the FROM table
 * at all! We can just throw away the plan-so-far and generate a Result node.
 * This is a sufficiently unusual corner case that it's not worth contorting
 * the structure of this module to avoid having to generate the earlier paths
 * in the first place.
 */
static void
create_degenerate_grouping_paths(PlannerInfo *root, RelOptInfo *input_rel,
								 RelOptInfo *grouped_rel)
{
	Query	   *parse = root->parse;
	int			nrows;
	Path	   *path;

	nrows = list_length(parse->groupingSets);
	if (nrows > 1)
	{
		/*
		 * Doesn't seem worthwhile writing code to cons up a generate_series
		 * or a values scan to emit multiple rows. Instead just make N clones
		 * and append them.  (With a volatile HAVING clause, this means you
		 * might get between 0 and N output rows. Offhand I think that's
		 * desired.)
		 */
		List	   *paths = NIL;

		while (--nrows >= 0)
		{
			path = (Path *)
				create_group_result_path(root, grouped_rel,
										 grouped_rel->reltarget,
										 (List *) parse->havingQual);
			paths = lappend(paths, path);
		}
		path = (Path *)
			create_append_path(root,
							   grouped_rel,
							   paths,
							   NIL,
							   NIL,
							   NULL,
							   0,
							   false,
							   NIL,
							   -1);
	}
	else
	{
		/* No grouping sets, or just one, so one output row */
		path = (Path *)
			create_group_result_path(root, grouped_rel,
									 grouped_rel->reltarget,
									 (List *) parse->havingQual);
	}

	add_path(grouped_rel, path);
}

/*
 * create_ordinary_grouping_paths
 *
 * Create grouping paths for the ordinary (that is, non-degenerate) case.
 *
 * We need to consider sorted and hashed aggregation in the same function,
 * because otherwise (1) it would be harder to throw an appropriate error
 * message if neither way works, and (2) we should not allow hashtable size
 * considerations to dissuade us from using hashing if sorting is not possible.
 *
 * *partially_grouped_rel_p will be set to the partially grouped rel which this
 * function creates, or to NULL if it doesn't create one.
 */
static void
create_ordinary_grouping_paths(PlannerInfo *root, RelOptInfo *input_rel,
							   RelOptInfo *grouped_rel,
							   const AggClauseCosts *agg_costs,
							   grouping_sets_data *gd,
							   GroupPathExtraData *extra,
							   RelOptInfo **partially_grouped_rel_p)
{
	Path	   *cheapest_path = input_rel->cheapest_total_path;
	RelOptInfo *partially_grouped_rel = NULL;
	double		dNumGroups;
	PartitionwiseAggregateType patype = PARTITIONWISE_AGGREGATE_NONE;

	/*
	 * If this is the topmost grouping relation or if the parent relation is
	 * doing some form of partitionwise aggregation, then we may be able to do
	 * it at this level also.  However, if the input relation is not
	 * partitioned, partitionwise aggregate is impossible.
	 */
	if (extra->patype != PARTITIONWISE_AGGREGATE_NONE &&
		IS_PARTITIONED_REL(input_rel))
	{
		/*
		 * If this is the topmost relation or if the parent relation is doing
		 * full partitionwise aggregation, then we can do full partitionwise
		 * aggregation provided that the GROUP BY clause contains all of the
		 * partitioning columns at this level and the collation used by GROUP
		 * BY matches the partitioning collation.  Otherwise, we can do at
		 * most partial partitionwise aggregation.  But if partial aggregation
		 * is not supported in general then we can't use it for partitionwise
		 * aggregation either.
		 */
		if (extra->patype == PARTITIONWISE_AGGREGATE_FULL &&
			group_by_has_partkey(input_rel, extra->targetList,
								 root->parse->groupClause))
			patype = PARTITIONWISE_AGGREGATE_FULL;
		else if ((extra->flags & GROUPING_CAN_PARTIAL_AGG) != 0)
			patype = PARTITIONWISE_AGGREGATE_PARTIAL;
		else
			patype = PARTITIONWISE_AGGREGATE_NONE;
	}

	/*
	 * Before generating paths for grouped_rel, we first generate any possible
	 * partially grouped paths; that way, later code can easily consider both
	 * parallel and non-parallel approaches to grouping.
	 */
	if ((extra->flags & GROUPING_CAN_PARTIAL_AGG) != 0)
	{
		bool		force_rel_creation;

		/*
		 * If we're doing partitionwise aggregation at this level, force
		 * creation of a partially_grouped_rel so we can add partitionwise
		 * paths to it.
		 */
		force_rel_creation = (patype == PARTITIONWISE_AGGREGATE_PARTIAL);

		partially_grouped_rel =
			create_partial_grouping_paths(root,
										  grouped_rel,
										  input_rel,
										  gd,
										  extra,
										  force_rel_creation);
	}

	/* Set out parameter. */
	*partially_grouped_rel_p = partially_grouped_rel;

	/* Apply partitionwise aggregation technique, if possible. */
	if (patype != PARTITIONWISE_AGGREGATE_NONE)
		create_partitionwise_grouping_paths(root, input_rel, grouped_rel,
											partially_grouped_rel, agg_costs,
											gd, patype, extra);

	/* If we are doing partial aggregation only, return. */
	if (extra->patype == PARTITIONWISE_AGGREGATE_PARTIAL)
	{
		Assert(partially_grouped_rel);

		if (partially_grouped_rel->pathlist)
			set_cheapest(partially_grouped_rel);

		return;
	}

	/* Gather any partially grouped partial paths. */
	if (partially_grouped_rel && partially_grouped_rel->partial_pathlist)
	{
		gather_grouping_paths(root, partially_grouped_rel);
		set_cheapest(partially_grouped_rel);
	}

	/*
	 * Estimate number of groups.
	 */
	dNumGroups = get_number_of_groups(root,
									  cheapest_path->rows,
									  gd,
									  extra->targetList);

	/* Build final grouping paths */
	add_paths_to_grouping_rel(root, input_rel, grouped_rel,
							  partially_grouped_rel, agg_costs, gd,
							  dNumGroups, extra);

	/* Give a helpful error if we failed to find any implementation */
	if (grouped_rel->pathlist == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not implement GROUP BY"),
				 errdetail("Some of the datatypes only support hashing, while others only support sorting.")));

	/*
	 * If there is an FDW that's responsible for all baserels of the query,
	 * let it consider adding ForeignPaths.
	 */
	if (grouped_rel->fdwroutine &&
		grouped_rel->fdwroutine->GetForeignUpperPaths)
		grouped_rel->fdwroutine->GetForeignUpperPaths(root, UPPERREL_GROUP_AGG,
													  input_rel, grouped_rel,
													  extra);

	/* Let extensions possibly add some more paths */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_GROUP_AGG,
									input_rel, grouped_rel,
									extra);
}

/*
 * consider_groupingsets_paths
 *
 * 对于给定的输入路径，考虑通过哈希和排序的组合方式来执行分组集操作。
 * 此函数可能被多次调用，因此重要的是它不会修改输入数据。
 * 不返回结果，但会将生成的路径添加到grouped_rel中。
 */
static void
consider_groupingsets_paths(PlannerInfo *root,  /* 规划器信息结构体指针 */
                           RelOptInfo *grouped_rel,  /* 分组关系（目标关系） */
                           Path *path,  /* 输入路径 */
                           bool is_sorted,  /* 输入是否已排序 */
                           bool can_hash,  /* 是否可以使用哈希 */
                           grouping_sets_data *gd,  /* 分组集数据 */
                           const AggClauseCosts *agg_costs,  /* 聚合函数成本信息 */
                           double dNumGroups)  /* 估计的组数 */
{
    Query   *parse = root->parse;  /* 查询解析树 */

    /*
     * 如果输入未排序，则只考虑可以完全通过哈希完成的计划。
     *
     * 如果看起来能放入work_mem，我们可以哈希所有内容。但如果输入实际上已排序，
     * 尽管未被标记为已排序，我们也会优先使用这一特性以节省内存。
     *
     * 如果没有分组集是可排序的，则忽略work_mem限制并生成路径，否则将无法执行。
     */
    if (!is_sorted)
    {
        List   *new_rollups = NIL;  /* 新的rollup列表 */
        RollupData *unhashed_rollup = NULL;  /* 未哈希的rollup */
        List   *sets_data;  /* 分组集数据列表 */
        List   *empty_sets_data = NIL;  /* 空分组集数据 */
        List   *empty_sets = NIL;  /* 空分组集 */
        ListCell   *lc;  /* 列表遍历单元格 */
        ListCell   *l_start = list_head(gd->rollups);  /* rollup列表的头部 */
        AggStrategy strat = AGG_HASHED;  /* 聚合策略，默认为哈希 */
        double  hashsize;  /* 哈希表大小估计 */
        double  exclude_groups = 0.0;  /* 要排除的组数 */

        Assert(can_hash);  /* 确保can_hash为true */

        /*
         * 如果输入恰好已经按有用的方式排序（即使is_sorted为false，因为这只表示
         * 调用者没有为我们设置排序），那么通过利用这一点来节省哈希表空间。
         * 但我们需要注意一些特殊情况：
         *
         * 1) 如果有空分组集，且所有非空分组集都是不可排序的，那么group_pathkeys可能为NIL。
         *    在这种情况下，将有一个只包含空分组的rollup，pathkeys_contained_in测试将
         *    自动为true；这是可以的。
         *
         * XXX: 上面依赖于group_pathkeys是从第一个rollup生成的事实。如果我们增加考虑
         * 分组输入的多种排序顺序的能力，这一假设可能会失败。
         *
         * 2) 如果没有空集且只有不可排序的集，那么rollups列表将为空（因此l_start == NULL），
         *    并且group_pathkeys将为NIL；我们必须确保自动为true的pathkeys_contain_in测试
         *    不会导致我们崩溃。
         */
        if (l_start != NULL &&
            pathkeys_contained_in(root->group_pathkeys, path->pathkeys))
        {
            unhashed_rollup = lfirst_node(RollupData, l_start);  /* 获取第一个rollup */
            exclude_groups = unhashed_rollup->numGroups;  /* 记录要排除的组数 */
            l_start = lnext(l_start);  /* 从下一个rollup开始处理 */
        }

        /* 估计哈希表大小，排除已排序的部分 */
        hashsize = estimate_hashagg_tablesize(path,
                                             agg_costs,
                                             dNumGroups - exclude_groups);

        /*
         * 如果只有不可排序的列，gd->rollups将为空。在这种情况下忽略work_mem；
         * 否则，我们将依赖排序输入情况来生成可用的混合路径。
         */
        if (hashsize > work_mem * 1024L && gd->rollups)
            return;  /* 不行，放不下 */

        /*
         * 我们需要将现有的rollups列表拆分为单独的分组集，并为每个集重新计算groupClause。
         */
        sets_data = list_copy(gd->unsortable_sets);  /* 复制不可排序的集 */

        /* 遍历剩余的rollup */
        for_each_cell(lc, l_start)
        {
            RollupData *rollup = lfirst_node(RollupData, lc);

            /*
             * 如果我们发现一个不可哈希的rollup，且未被上面的"实际已排序"检查跳过，
             * 我们无法处理；我们需要排序输入（具有不同的排序顺序），但在这里无法获得。
             * 因此放弃；我们将从is_sorted情况获得有效路径。
             *
             * 空分组集的存在本身不会使rollup不可哈希（见preprocess_grouping_sets），
             * 我们在下面特殊处理这些。
             */
            if (!rollup->hashable)
                return;
            else
                sets_data = list_concat(sets_data, list_copy(rollup->gsets_data));
        }
        
        /* 处理每个分组集数据 */
        foreach(lc, sets_data)
        {
            GroupingSetData *gs = lfirst_node(GroupingSetData, lc);
            List   *gset = gs->set;  /* 分组集定义 */
            RollupData *rollup;

            if (gset == NIL)
            {
                /* 空分组集不能哈希 */
                empty_sets_data = lappend(empty_sets_data, gs);
                empty_sets = lappend(empty_sets, NIL);
            }
            else
            {
                /* 创建新的rollup用于哈希处理 */
                rollup = makeNode(RollupData);

                rollup->groupClause = preprocess_groupclause(root, gset);  /* 预处理分组子句 */
                rollup->gsets_data = list_make1(gs);  /* 设置分组集数据 */
                rollup->gsets = remap_to_groupclause_idx(rollup->groupClause,  /* 重新映射索引 */
                                                        rollup->gsets_data,
                                                        gd->tleref_to_colnum_map);
                rollup->numGroups = gs->numGroups;  /* 设置估计组数 */
                rollup->hashable = true;  /* 标记为可哈希 */
                rollup->is_hashed = true;  /* 标记为将使用哈希 */
                new_rollups = lappend(new_rollups, rollup);  /* 添加到新rollup列表 */
            }
        }

        /*
         * 如果没有找到非空的可哈希项，则放弃。我们将从is_sorted情况生成路径。
         */
        if (new_rollups == NIL)
            return;

        /*
         * 如果有空分组集，它们应该在第一个rollup中。
         */
        Assert(!unhashed_rollup || !empty_sets);

        /* 处理未哈希的rollup和空分组集 */
        if (unhashed_rollup)
        {
            new_rollups = lappend(new_rollups, unhashed_rollup);
            strat = AGG_MIXED;  /* 混合策略 */
        }
        else if (empty_sets)
        {
            /* 为空分组集创建特殊rollup */
            RollupData *rollup = makeNode(RollupData);

            rollup->groupClause = NIL;  /* 无分组子句 */
            rollup->gsets_data = empty_sets_data;
            rollup->gsets = empty_sets;
            rollup->numGroups = list_length(empty_sets);  /* 空分组集数量 */
            rollup->hashable = false;  /* 不可哈希 */
            rollup->is_hashed = false;  /* 不使用哈希 */
            new_rollups = lappend(new_rollups, rollup);
            strat = AGG_MIXED;  /* 混合策略 */
        }

        /* 创建并添加分组集路径 */
        add_path(grouped_rel, (Path *)
                 create_groupingsets_path(root,
                                         grouped_rel,
                                         path,
                                         (List *) parse->havingQual,
                                         strat,
                                         new_rollups,
                                         agg_costs,
                                         dNumGroups));
        return;
    }

    /*
     * 如果我们有排序的输入但无法使用它，则放弃。
     */
    if (list_length(gd->rollups) == 0)
        return;

    /*
     * 给定排序的输入，我们尝试创建两种路径：一种是纯排序的，另一种是混合排序/哈希的。
     * （我们需要尝试两种，因为哈希聚合可能被禁用，或者某些列可能不可排序。）
     *
     * 如果其他地方有障碍（例如有序聚合）意味着我们不应该考虑哈希，则can_hash会被传入false。
     */
    if (can_hash && gd->any_hashable)
    {
        List   *rollups = NIL;  /* rollup列表 */
        List   *hash_sets = list_copy(gd->unsortable_sets);  /* 要哈希的不可排序集 */
        double  availspace = (work_mem * 1024.0);  /* 可用内存空间 */
        ListCell   *lc;

        /*
         * 首先计算完全不可排序的分组所需的空间。
         */
        availspace -= estimate_hashagg_tablesize(path,
                                                agg_costs,
                                                gd->dNumHashGroups);

        /* 如果有可用空间且rollup数量大于1，尝试优化内存使用 */
        if (availspace > 0 && list_length(gd->rollups) > 1)
        {
            double  scale;  /* 缩放因子 */
            int     num_rollups = list_length(gd->rollups);  /* rollup总数 */
            int     k_capacity;  /* 背包容量 */
            int     *k_weights = palloc(num_rollups * sizeof(int));  /* 项权重数组 */
            Bitmapset  *hash_items = NULL;  /* 要哈希的项集合 */
            int     i;

            /*
             * 我们将此视为背包问题：背包容量代表work_mem，项权重是实现单个rollup
             * 所需的哈希表估计内存使用量，而我们实际上应该使用成本节省作为项值；
             * 但是，当前分配给排序节点的成本不能很好地反映比较成本，因此我们将所有项
             * 视为具有相等的值（我们哈希而不是排序的每个rollup节省我们一次排序）。
             *
             * 要使用离散背包，我们需要将值缩放到合理小的有界范围。我们选择允许5%的错误
             * 边际；在最坏情况下，我们不超过4096个rollup，在5%的错误边际下将需要略多于
             * 42MB的工作空间。（任何想要规划如此复杂查询的人最好有足够的内存。在更合理
             * 的情况下，如果不超过几十个rollup，内存使用量将可以忽略不计。）
             *
             * k_capacity自然是有界的，但我们限制scale和weight的值（如下）以避免溢出或
             * 下溢（或无用的尝试使用小于1字节的缩放因子）。
             */
            scale = Max(availspace / (20.0 * num_rollups), 1.0);  /* 计算缩放因子 */
            k_capacity = (int) floor(availspace / scale);  /* 计算背包容量 */

            /*
             * 我们不考虑第一个rollup，因为它与输入排序顺序匹配。我们只为那些考虑哈希的
             * 条目分配索引"i"；下面的第二个循环必须使用相同的条件。
             */
            i = 0;
            for_each_cell(lc, lnext(list_head(gd->rollups)))
            {
                RollupData *rollup = lfirst_node(RollupData, lc);

                if (rollup->hashable)
                {
                    /* 估计此rollup的哈希表大小 */
                    double  sz = estimate_hashagg_tablesize(path,
                                                           agg_costs,
                                                           rollup->numGroups);

                    /*
                     * 如果sz非常大，但work_mem（因此scale）很小，避免此处整数溢出。
                     */
                    k_weights[i] = (int) Min(floor(sz / scale),
                                            k_capacity + 1.0);
                    ++i;
                }
            }

            /*
             * 应用背包算法；计算项目集，该项目集最大化存储的值（在这种情况下，节省的排序次数）
             * 同时保持总大小（近似）在容量内。
             */
            if (i > 0)
                hash_items = DiscreteKnapsack(k_capacity, i, k_weights, NULL);

            /* 如果找到要哈希的项目，构建rollup列表 */
            if (!bms_is_empty(hash_items))
            {
                rollups = list_make1(linitial(gd->rollups));  /* 保留第一个rollup用于排序 */

                i = 0;
                for_each_cell(lc, lnext(list_head(gd->rollups)))
                {
                    RollupData *rollup = lfirst_node(RollupData, lc);

                    if (rollup->hashable)
                    {
                        if (bms_is_member(i, hash_items))
                            /* 此rollup将通过哈希处理 */
                            hash_sets = list_concat(hash_sets,
                                                   list_copy(rollup->gsets_data));
                        else
                            /* 此rollup将通过排序处理 */
                            rollups = lappend(rollups, rollup);
                        ++i;
                    }
                    else
                        /* 不可哈希的rollup必须通过排序处理 */
                        rollups = lappend(rollups, rollup);
                }
            }
        }

        /* 如果没有rollup但有hash_sets，复制原始rollup列表 */
        if (!rollups && hash_sets)
            rollups = list_copy(gd->rollups);

        /* 为每个hash_set创建单独的rollup并添加到rollups列表的开头 */
        foreach(lc, hash_sets)
        {
            GroupingSetData *gs = lfirst_node(GroupingSetData, lc);
            RollupData *rollup = makeNode(RollupData);

            Assert(gs->set != NIL);  /* 确保不是空集 */

            rollup->groupClause = preprocess_groupclause(root, gs->set);
            rollup->gsets_data = list_make1(gs);
            rollup->gsets = remap_to_groupclause_idx(rollup->groupClause,
                                                    rollup->gsets_data,
                                                    gd->tleref_to_colnum_map);
            rollup->numGroups = gs->numGroups;
            rollup->hashable = true;
            rollup->is_hashed = true;
            rollups = lcons(rollup, rollups);  /* 添加到列表开头 */
        }

        /* 如果有rollup，创建并添加混合策略的分组集路径 */
        if (rollups)
        {
            add_path(grouped_rel, (Path *)
                     create_groupingsets_path(root,
                                             grouped_rel,
                                             path,
                                             (List *) parse->havingQual,
                                             AGG_MIXED,  /* 混合排序/哈希策略 */
                                             rollups,
                                             agg_costs,
                                             dNumGroups));
        }
    }

    /*
     * 现在尝试简单的排序情况。
     */
    if (!gd->unsortable_sets)  /* 如果没有不可排序的集 */
        add_path(grouped_rel, (Path *)
                 create_groupingsets_path(root,
                                         grouped_rel,
                                         path,
                                         (List *) parse->havingQual,
                                         AGG_SORTED,  /* 纯排序策略 */
                                         gd->rollups,
                                         agg_costs,
                                         dNumGroups));
}


/*
 * create_window_paths
 *    构建一个包含窗口函数评估路径的新上层关系(upperrel)
 *
 * 参数说明：
 * input_rel: 包含源数据路径的关系
 * input_target: 由make_window_input_target生成的输入目标
 * output_target: 最顶层WindowAggPath应该返回的目标
 * output_target_parallel_safe: 输出目标是否并行安全
 * wflists: 由find_window_functions生成的窗口函数列表
 * activeWindows: 由select_active_windows生成的活动窗口列表
 *
 * 注意：input_rel中的所有路径都应该返回input_target
 *
 * 返回值：
 * 包含窗口函数评估路径的上层关系
 */
static RelOptInfo *
create_window_paths(PlannerInfo *root,
                    RelOptInfo *input_rel,
                    PathTarget *input_target,
                    PathTarget *output_target,
                    bool output_target_parallel_safe,
                    WindowFuncLists *wflists,
                    List *activeWindows)
{
    RelOptInfo *window_rel;  // 新创建的窗口函数上层关系
    ListCell   *lc;          // 用于遍历路径列表的指针

    /* 目前，所有工作都在(WINDOW, NULL)上层关系中进行 */
    window_rel = fetch_upper_rel(root, UPPERREL_WINDOW, NULL);

    /*
     * 如果输入关系不支持并行执行，那么窗口关系也不能支持并行执行。
     * 否则，我们需要检查目标列表和活动窗口是否包含非并行安全的构造。
     */
    if (input_rel->consider_parallel && output_target_parallel_safe &&
        is_parallel_safe(root, (Node *) activeWindows))
        window_rel->consider_parallel = true;

    /*
     * 如果输入关系属于单个FDW(外部数据包装器)，那么窗口关系也属于该FDW
     */
    window_rel->serverid = input_rel->serverid;
    window_rel->userid = input_rel->userid;
    window_rel->useridiscurrent = input_rel->useridiscurrent;
    window_rel->fdwroutine = input_rel->fdwroutine;

    /*
     * 考虑从现有的总成本最低路径(可能需要排序)以及任何满足
     * root->window_pathkeys的现有路径(不需要排序)开始计算窗口函数
     */
    foreach(lc, input_rel->pathlist)
    {
        Path       *path = (Path *) lfirst(lc);

        // 选择总成本最低的路径或已满足窗口排序键要求的路径
        if (path == input_rel->cheapest_total_path ||
            pathkeys_contained_in(root->window_pathkeys, path->pathkeys))
            // 为选中的路径创建窗口函数路径
            create_one_window_path(root,
                                   window_rel,
                                   path,
                                   input_target,
                                   output_target,
                                   wflists,
                                   activeWindows);
    }

    /*
     * 如果存在一个负责查询所有基关系的FDW，让它考虑添加ForeignPaths
     */
    if (window_rel->fdwroutine &&
        window_rel->fdwroutine->GetForeignUpperPaths)
        window_rel->fdwroutine->GetForeignUpperPaths(root, UPPERREL_WINDOW,
                                                     input_rel, window_rel,
                                                     NULL);

    /* 允许扩展可能添加更多的路径 */
    if (create_upper_paths_hook)
        (*create_upper_paths_hook) (root, UPPERREL_WINDOW,
                                    input_rel, window_rel, NULL);

    /* 现在选择最佳路径 */
    set_cheapest(window_rel);

    return window_rel;
}


/*
 * create_one_window_path
 *    在给定路径上构建窗口函数实现步骤，并将结果添加到window_rel上层关系
 *
 * 参数说明：
 * window_rel: 包含结果的上层关系(upperrel)
 * path: 输入路径，必须返回input_target
 * input_target: 由make_window_input_target生成的输入目标
 * output_target: 最顶层WindowAggPath应该返回的目标
 * wflists: 由find_window_functions生成的窗口函数列表
 * activeWindows: 由select_active_windows生成的活动窗口列表
 */
static void
create_one_window_path(PlannerInfo *root,
                       RelOptInfo *window_rel,
                       Path *path,
                       PathTarget *input_target,
                       PathTarget *output_target,
                       WindowFuncLists *wflists,
                       List *activeWindows)
{
    PathTarget *window_target;  // 当前窗口操作的目标列表
    ListCell   *l;              // 用于遍历activeWindows列表的指针

    /*
     * 由于每个窗口子句可能需要不同的排序顺序，我们为每个子句堆叠一个WindowAgg节点，
     * 并在必要时在它们之间添加排序步骤。我们假设select_active_windows已经选择了
     * 一个良好的子句执行顺序。
     *
     * input_target应包含结果所需的所有变量(Vars)和聚合函数(Aggs)。
     * (在某些情况下，我们不需要将所有这些都一直传播到顶部，因为它们可能只需要作为
     * WindowFuncs的输入。但这种优化可能不值得。)它还必须包含所有窗口分区和排序表达式，
     * 以确保它们只在堆栈底部计算一次(这对易变函数至关重要)。随着我们向上爬堆栈，
     * 我们会为每个级别的WindowFuncs添加输出。
     */
    window_target = input_target;  // 初始化为输入目标

    // 遍历所有活动的窗口子句
    foreach(l, activeWindows)
    {
        WindowClause *wc = lfirst_node(WindowClause, l);  // 当前窗口子句
        List       *window_pathkeys;                      // 窗口操作所需的排序键

        // 为当前窗口子句生成所需的排序键
        window_pathkeys = make_pathkeys_for_window(root,
                                                   wc,
                                                   root->processed_tlist);

        /* 必要时执行排序操作 */
        if (!pathkeys_contained_in(window_pathkeys, path->pathkeys))
        {
            // 创建排序路径，将其插入到执行计划中
            path = (Path *) create_sort_path(root, window_rel,
                                             path,
                                             window_pathkeys,
                                             -1.0);  // -1.0表示使用默认的排序内存
        }

        if (lnext(l))
        {   // 如果不是最后一个窗口子句
            /*
             * 将当前窗口函数添加到这个中间WindowAggPath的输出目标中。
             * 我们必须复制window_target以避免修改前一个路径的目标。
             *
             * 注意：WindowFunc不会增加目标的评估成本；但我们需要考虑结果集宽度的增加。
             */
            ListCell   *lc2;  // 用于遍历窗口函数列表的指针

            window_target = copy_pathtarget(window_target);  // 复制目标以避免副作用
            foreach(lc2, wflists->windowFuncs[wc->winref])
            {   // 遍历当前窗口引用的所有窗口函数
                WindowFunc *wfunc = lfirst_node(WindowFunc, lc2);

                // 将窗口函数添加到输出目标中
                add_column_to_pathtarget(window_target, (Expr *) wfunc, 0);
                // 更新结果宽度估计
                window_target->width += get_typavgwidth(wfunc->wintype, -1);
            }
        }
        else
        {   // 最后一个窗口子句，使用最终的输出目标
            /* 在最顶层的WindowAgg中安装目标结果 */
            window_target = output_target;
        }

        // 创建窗口聚合路径节点
        path = (Path *)
            create_windowagg_path(root, window_rel, path, window_target,
                                  wflists->windowFuncs[wc->winref],
                                  wc);
    }

    // 将构建好的路径添加到上层关系中
    add_path(window_rel, path);
}

/*
 * create_distinct_paths
 *
 * 构建一个新的上层关系(upperrel)，包含用于SELECT DISTINCT评估的路径(Paths)。
 *
 * 参数:
 *   root - 规划器信息结构体指针
 *   input_rel - 包含源数据路径的关系
 *
 * 注意：输入路径应该已经计算出所需的pathtarget，因为Sort/Unique节点不会进行投影操作。
 */
static RelOptInfo *
create_distinct_paths(PlannerInfo *root,
                      RelOptInfo *input_rel)
{
    Query       *parse = root->parse;            /* 解析后的查询结构 */
    Path        *cheapest_input_path = input_rel->cheapest_total_path; /* 总成本最低的输入路径 */
    RelOptInfo  *distinct_rel;                   /* 存储DISTINCT结果的上层关系 */
    double      numDistinctRows;                 /* 估计的不同行数 */
    bool        allow_hash;                      /* 是否允许使用哈希实现 */
    Path        *path;                           /* 临时路径变量 */
    ListCell    *lc;                             /* 列表遍历指针 */

    /* 暂时在(DISTINCT, NULL)上层关系中完成所有工作 */
    distinct_rel = fetch_upper_rel(root, UPPERREL_DISTINCT, NULL);

    /*
     * 在这个级别我们不执行任何计算，所以如果输入关系是并行安全的，
     * distinct_rel也将是并行安全的。特别是，如果存在DISTINCT ON (...)子句，
     * 则input_rel的任何路径都将输出这些表达式，
     * 并且只有当这些表达式是并行安全的时，路径才是并行安全的。
     */
    distinct_rel->consider_parallel = input_rel->consider_parallel;

    /*
     * 如果输入关系属于单个外部数据包装器(FDW)，那么distinct_rel也属于同一FDW。
     */
    distinct_rel->serverid = input_rel->serverid;          /* 服务器ID */
    distinct_rel->userid = input_rel->userid;              /* 用户ID */
    distinct_rel->useridiscurrent = input_rel->useridiscurrent; /* 用户ID是否为当前用户 */
    distinct_rel->fdwroutine = input_rel->fdwroutine;      /* FDW处理例程 */

    /* 估计将会有多少不同的行数 */
    if (parse->groupClause || parse->groupingSets || parse->hasAggs ||
        root->hasHavingQual)
    {
        /*
         * 如果存在分组或聚合操作，使用输入行数作为估计的不同行数
         * （即假设输入已经大部分是唯一的）。
         */
        numDistinctRows = cheapest_input_path->rows;
    }
    else
    {
        /*
         * 否则，UNIQUE过滤器的效果类似于GROUP BY。
         */
        List       *distinctExprs;  /* DISTINCT表达式列表 */

        /* 获取DISTINCT子句对应的表达式列表 */
        distinctExprs = get_sortgrouplist_exprs(parse->distinctClause,
                                               parse->targetList);
        /* 估计不同组的数量 */
        numDistinctRows = estimate_num_groups(root, distinctExprs,
                                             cheapest_input_path->rows,
                                             NULL);
    }

    /*
     * 考虑基于排序的DISTINCT实现，如果可能的话。
     */
    if (grouping_is_sortable(parse->distinctClause))
    {
        /*
         * 首先，如果有任何已经适当排序的路径，只需在这些路径上添加一个Unique节点。
         * 然后考虑对最便宜的输入路径进行显式排序，然后应用Unique操作。
         *
         * 当有DISTINCT ON时，我们必须按照DISTINCT和ORDER BY中更严格的条件排序，
         * 否则它将不会有预期的行为。另外，如果我们必须进行显式排序，
         * 我们最好使用更严格的排序顺序，以避免稍后进行第二次排序。
         * （注意，解析器会确保一个子句是另一个子句的前缀。）
         */
        List       *needed_pathkeys;  /* 需要的排序键 */

        /* 确定需要使用的排序键 */
        if (parse->hasDistinctOn &&
            list_length(root->distinct_pathkeys) <
            list_length(root->sort_pathkeys))
            needed_pathkeys = root->sort_pathkeys;
        else
            needed_pathkeys = root->distinct_pathkeys;

        /* 检查并利用已经适当排序的路径 */
        foreach(lc, input_rel->pathlist)
        {
            Path       *path = (Path *) lfirst(lc);

            if (pathkeys_contained_in(needed_pathkeys, path->pathkeys))
            {
                add_path(distinct_rel, (Path *)
                         create_upper_unique_path(root, distinct_rel,
                                                 path,
                                                 list_length(root->distinct_pathkeys),
                                                 numDistinctRows));
            }
        }

        /* 对于显式排序的情况，总是使用更严格的子句 */
        if (list_length(root->distinct_pathkeys) <
            list_length(root->sort_pathkeys))
        {
            needed_pathkeys = root->sort_pathkeys;
            /* 断言确保解析器没有出错... */
            Assert(pathkeys_contained_in(root->distinct_pathkeys,
                                        needed_pathkeys));
        }
        else
            needed_pathkeys = root->distinct_pathkeys;

        /* 从最便宜的输入路径开始，如果需要则添加排序步骤 */
        path = cheapest_input_path;
        if (!pathkeys_contained_in(needed_pathkeys, path->pathkeys))
            path = (Path *) create_sort_path(root, distinct_rel,
                                            path,
                                            needed_pathkeys,
                                            -1.0);

        /* 添加带有Unique节点的路径 */
        add_path(distinct_rel, (Path *)
                 create_upper_unique_path(root, distinct_rel,
                                         path,
                                         list_length(root->distinct_pathkeys),
                                         numDistinctRows));
    }

    /*
     * 考虑基于哈希的DISTINCT实现，如果可能的话。
     *
     * 如果我们无法制作任何其他类型的路径，我们必须尝试哈希或者失败。
     * 如果我们确实有其他选择，有几件事应该阻止选择哈希：
     * 如果查询使用DISTINCT ON（因为如果我们哈希，它将不会有预期的行为），
     * 或者如果enable_hashagg关闭，
     * 或者如果看起来哈希表将超过work_mem。
     *
     * 注意：grouping_is_hashable()的检查比其他门控条件更昂贵，所以我们想最后做它。
     */
    if (distinct_rel->pathlist == NIL)
        allow_hash = true;      /* 我们没有其他选择 */
    else if (parse->hasDistinctOn || !enable_hashagg)
        allow_hash = false;     /* 基于策略决定不使用哈希 */
    else
    {
        Size        hashentrysize;  /* 每个哈希条目的大小估计 */

        /* 估计每个哈希条目的空间为元组宽度... */
        hashentrysize = MAXALIGN(cheapest_input_path->pathtarget->width) +
            MAXALIGN(SizeofMinimalTupleHeader);
        /* 加上每个哈希条目的开销 */
        hashentrysize += hash_agg_entry_size(0);

        /* 只有当哈希表预计适合work_mem时才允许哈希 */
        allow_hash = (hashentrysize * numDistinctRows <= work_mem * 1024L);
    }

    /* 如果允许哈希且DISTINCT子句支持哈希，则创建哈希路径 */
    if (allow_hash && grouping_is_hashable(parse->distinctClause))
    {
        /* 生成哈希聚合路径 --- 不需要排序 */
        add_path(distinct_rel, (Path *)
                 create_agg_path(root,
                                distinct_rel,
                                cheapest_input_path,
                                cheapest_input_path->pathtarget,
                                AGG_HASHED,      /* 使用哈希聚合 */
                                AGGSPLIT_SIMPLE, /* 简单聚合分割 */
                                parse->distinctClause,
                                NIL,             /* 无分组子句 */
                                NULL,            /* 无having子句 */
                                numDistinctRows));
    }

    /* 如果找不到任何实现，则给出有用的错误信息 */
    if (distinct_rel->pathlist == NIL)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("could not implement DISTINCT"),
                 errdetail("Some of the datatypes only support hashing, while others only support sorting.")));

    /*
     * 如果有一个负责查询中所有基础关系的FDW，让它考虑添加ForeignPaths。
     */
    if (distinct_rel->fdwroutine &&
        distinct_rel->fdwroutine->GetForeignUpperPaths)
        distinct_rel->fdwroutine->GetForeignUpperPaths(root, UPPERREL_DISTINCT,
                                                      input_rel, distinct_rel,
                                                      NULL);

    /* 让扩展可能添加更多路径 */
    if (create_upper_paths_hook)
        (*create_upper_paths_hook) (root, UPPERREL_DISTINCT,
                                  input_rel, distinct_rel, NULL);

    /* 现在选择最佳路径 */
    set_cheapest(distinct_rel);

    /* 返回构建的上层关系 */
    return distinct_rel;
}


/*
 * create_ordered_paths
 *
 * 构建一个新的上层关系(upperrel)，包含用于ORDER BY评估的路径(Paths)。
 *
 * 结果中的所有路径必须满足ORDER BY指定的排序顺序。
 * 我们需要考虑的唯一新路径是在总成本最低的现有路径上执行显式排序。
 *
 * 参数说明：
 *   root - 规划器信息结构体指针
 *   input_rel - 包含源数据路径的关系
 *   target - 结果路径必须输出的目标列表
 *   target_parallel_safe - 目标列表是否并行安全
 *   limit_tuples - 输出元组数量的估计上限，如果没有LIMIT或无法估计则为-1
 */
static RelOptInfo *
create_ordered_paths(PlannerInfo *root,
                     RelOptInfo *input_rel,
                     PathTarget *target,
                     bool target_parallel_safe,
                     double limit_tuples)
{
    Path        *cheapest_input_path = input_rel->cheapest_total_path; /* 总成本最低的输入路径 */
    RelOptInfo  *ordered_rel;                   /* 存储ORDER BY结果的上层关系 */
    ListCell    *lc;                            /* 列表遍历指针 */

    /* 暂时在(ORDERED, NULL)上层关系中完成所有工作 */
    ordered_rel = fetch_upper_rel(root, UPPERREL_ORDERED, NULL);

    /*
     * 如果输入关系不是并行安全的，那么有序关系也不可能是并行安全的。
     * 否则，只有当目标列表是并行安全的时，它才是并行安全的。
     */
    if (input_rel->consider_parallel && target_parallel_safe)
        ordered_rel->consider_parallel = true;

    /*
     * 如果输入关系属于单个外部数据包装器(FDW)，那么ordered_rel也属于同一FDW。
     */
    ordered_rel->serverid = input_rel->serverid;          /* 服务器ID */
    ordered_rel->userid = input_rel->userid;              /* 用户ID */
    ordered_rel->useridiscurrent = input_rel->useridiscurrent; /* 用户ID是否为当前用户 */
    ordered_rel->fdwroutine = input_rel->fdwroutine;      /* FDW处理例程 */

    /* 处理输入关系中的路径 */
    foreach(lc, input_rel->pathlist)
    {
        Path       *path = (Path *) lfirst(lc);
        bool        is_sorted;  /* 路径是否已经按需要排序 */

        /* 检查路径是否已经满足ORDER BY排序要求 */
        is_sorted = pathkeys_contained_in(root->sort_pathkeys,
                                         path->pathkeys);
        
        /* 考虑两种路径：总成本最低的路径和已经正确排序的路径 */
        if (path == cheapest_input_path || is_sorted)
        {
            /* 如果路径未按要求排序，则添加排序步骤 */
            if (!is_sorted)
            {
                /* 这里的显式排序可以利用LIMIT进行优化 */
                path = (Path *) create_sort_path(root,
                                                ordered_rel,
                                                path,
                                                root->sort_pathkeys,
                                                limit_tuples);
            }

            /* 如果需要，添加投影步骤以输出正确的目标列表 */
            if (path->pathtarget != target)
                path = apply_projection_to_path(root, ordered_rel,
                                               path, target);

            /* 将处理后的路径添加到有序关系中 */
            add_path(ordered_rel, path);
        }
    }

    /*
     * generate_gather_paths()已经为最佳并行路径生成了简单的Gather路径（如果有），
     * 上面的循环已经考虑了对其进行排序。类似地，generate_gather_paths()也生成了
     * 保持顺序的Gather Merge计划，如果它们恰好匹配sort_pathkeys，则无需排序即可使用，
     * 上面的循环也已经处理了这些情况。但是，还有一种可能性：
     * 可能有意义的是根据所需的输出顺序对最便宜的部分路径进行排序，然后使用Gather Merge。
     */
    if (ordered_rel->consider_parallel && root->sort_pathkeys != NIL &&
        input_rel->partial_pathlist != NIL)
    {
        Path       *cheapest_partial_path;  /* 成本最低的部分路径 */

        /* 获取成本最低的部分路径 */
        cheapest_partial_path = linitial(input_rel->partial_pathlist);

        /*
         * 如果最便宜的部分路径已经符合排序要求，则此操作是多余的
         */
        if (!pathkeys_contained_in(root->sort_pathkeys,
                                  cheapest_partial_path->pathkeys))
        {
            Path       *path;
            double      total_groups;  /* 估计的总行组数量 */

            /* 为部分路径添加排序步骤 */
            path = (Path *) create_sort_path(root,
                                            ordered_rel,
                                            cheapest_partial_path,
                                            root->sort_pathkeys,
                                            limit_tuples);

            /* 计算总组数 = 单部分行数 * 并行工作线程数 */
            total_groups = cheapest_partial_path->rows *
                cheapest_partial_path->parallel_workers;
            
            /* 创建Gather Merge路径，合并多个已排序的部分结果 */
            path = (Path *)
                create_gather_merge_path(root, ordered_rel,
                                        path,
                                        path->pathtarget,
                                        root->sort_pathkeys, NULL,
                                        &total_groups);

            /* 如果需要，添加投影步骤 */
            if (path->pathtarget != target)
                path = apply_projection_to_path(root, ordered_rel,
                                               path, target);

            /* 添加并行排序+Gather Merge路径 */
            add_path(ordered_rel, path);
        }
    }

    /*
     * 如果有一个负责查询中所有基础关系的FDW，让它考虑添加ForeignPaths。
     */
    if (ordered_rel->fdwroutine &&
        ordered_rel->fdwroutine->GetForeignUpperPaths)
        ordered_rel->fdwroutine->GetForeignUpperPaths(root, UPPERREL_ORDERED,
                                                     input_rel, ordered_rel,
                                                     NULL);

    /* 让扩展可能添加更多路径 */
    if (create_upper_paths_hook)
        (*create_upper_paths_hook) (root, UPPERREL_ORDERED,
                                  input_rel, ordered_rel, NULL);

    /*
     * 无需在此处调用set_cheapest；grouping_planner不需要我们这样做。
     */
    Assert(ordered_rel->pathlist != NIL);  /* 确保至少有一条路径生成 */

    /* 返回构建的有序关系 */
    return ordered_rel;
}



/*
 * make_group_input_target
 *	  Generate appropriate PathTarget for initial input to grouping nodes.
 *
 * If there is grouping or aggregation, the scan/join subplan cannot emit
 * the query's final targetlist; for example, it certainly can't emit any
 * aggregate function calls.  This routine generates the correct target
 * for the scan/join subplan.
 *
 * The query target list passed from the parser already contains entries
 * for all ORDER BY and GROUP BY expressions, but it will not have entries
 * for variables used only in HAVING clauses; so we need to add those
 * variables to the subplan target list.  Also, we flatten all expressions
 * except GROUP BY items into their component variables; other expressions
 * will be computed by the upper plan nodes rather than by the subplan.
 * For example, given a query like
 *		SELECT a+b,SUM(c+d) FROM table GROUP BY a+b;
 * we want to pass this targetlist to the subplan:
 *		a+b,c,d
 * where the a+b target will be used by the Sort/Group steps, and the
 * other targets will be used for computing the final results.
 *
 * 'final_target' is the query's final target list (in PathTarget form)
 *
 * The result is the PathTarget to be computed by the Paths returned from
 * query_planner().
 */
static PathTarget *
make_group_input_target(PlannerInfo *root, PathTarget *final_target)
{
	Query	   *parse = root->parse;
	PathTarget *input_target;
	List	   *non_group_cols;
	List	   *non_group_vars;
	int			i;
	ListCell   *lc;

	/*
	 * We must build a target containing all grouping columns, plus any other
	 * Vars mentioned in the query's targetlist and HAVING qual.
	 */
	input_target = create_empty_pathtarget();
	non_group_cols = NIL;

	i = 0;
	foreach(lc, final_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(final_target, i);

		if (sgref && parse->groupClause &&
			get_sortgroupref_clause_noerr(sgref, parse->groupClause) != NULL)
		{
			/*
			 * It's a grouping column, so add it to the input target as-is.
			 */
			add_column_to_pathtarget(input_target, expr, sgref);
		}
		else
		{
			/*
			 * Non-grouping column, so just remember the expression for later
			 * call to pull_var_clause.
			 */
			non_group_cols = lappend(non_group_cols, expr);
		}

		i++;
	}

	/*
	 * If there's a HAVING clause, we'll need the Vars it uses, too.
	 */
	if (parse->havingQual)
		non_group_cols = lappend(non_group_cols, parse->havingQual);

	/*
	 * Pull out all the Vars mentioned in non-group cols (plus HAVING), and
	 * add them to the input target if not already present.  (A Var used
	 * directly as a GROUP BY item will be present already.)  Note this
	 * includes Vars used in resjunk items, so we are covering the needs of
	 * ORDER BY and window specifications.  Vars used within Aggrefs and
	 * WindowFuncs will be pulled out here, too.
	 */
	non_group_vars = pull_var_clause((Node *) non_group_cols,
									 PVC_RECURSE_AGGREGATES |
									 PVC_RECURSE_WINDOWFUNCS |
									 PVC_INCLUDE_PLACEHOLDERS);
	add_new_columns_to_pathtarget(input_target, non_group_vars);

	/* clean up cruft */
	list_free(non_group_vars);
	list_free(non_group_cols);

	/* XXX this causes some redundant cost calculation ... */
	return set_pathtarget_cost_width(root, input_target);
}

/*
 * make_partial_grouping_target
 *	  Generate appropriate PathTarget for output of partial aggregate
 *	  (or partial grouping, if there are no aggregates) nodes.
 *
 * A partial aggregation node needs to emit all the same aggregates that
 * a regular aggregation node would, plus any aggregates used in HAVING;
 * except that the Aggref nodes should be marked as partial aggregates.
 *
 * In addition, we'd better emit any Vars and PlaceholderVars that are
 * used outside of Aggrefs in the aggregation tlist and HAVING.  (Presumably,
 * these would be Vars that are grouped by or used in grouping expressions.)
 *
 * grouping_target is the tlist to be emitted by the topmost aggregation step.
 * havingQual represents the HAVING clause.
 */
static PathTarget *
make_partial_grouping_target(PlannerInfo *root,
							 PathTarget *grouping_target,
							 Node *havingQual)
{
	Query	   *parse = root->parse;
	PathTarget *partial_target;
	List	   *non_group_cols;
	List	   *non_group_exprs;
	int			i;
	ListCell   *lc;

	partial_target = create_empty_pathtarget();
	non_group_cols = NIL;

	i = 0;
	foreach(lc, grouping_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(grouping_target, i);

		if (sgref && parse->groupClause &&
			get_sortgroupref_clause_noerr(sgref, parse->groupClause) != NULL)
		{
			/*
			 * It's a grouping column, so add it to the partial_target as-is.
			 * (This allows the upper agg step to repeat the grouping calcs.)
			 */
			add_column_to_pathtarget(partial_target, expr, sgref);
		}
		else
		{
			/*
			 * Non-grouping column, so just remember the expression for later
			 * call to pull_var_clause.
			 */
			non_group_cols = lappend(non_group_cols, expr);
		}

		i++;
	}

	/*
	 * If there's a HAVING clause, we'll need the Vars/Aggrefs it uses, too.
	 */
	if (havingQual)
		non_group_cols = lappend(non_group_cols, havingQual);

	/*
	 * Pull out all the Vars, PlaceHolderVars, and Aggrefs mentioned in
	 * non-group cols (plus HAVING), and add them to the partial_target if not
	 * already present.  (An expression used directly as a GROUP BY item will
	 * be present already.)  Note this includes Vars used in resjunk items, so
	 * we are covering the needs of ORDER BY and window specifications.
	 */
	non_group_exprs = pull_var_clause((Node *) non_group_cols,
									  PVC_INCLUDE_AGGREGATES |
									  PVC_RECURSE_WINDOWFUNCS |
									  PVC_INCLUDE_PLACEHOLDERS);

	add_new_columns_to_pathtarget(partial_target, non_group_exprs);

	/*
	 * Adjust Aggrefs to put them in partial mode.  At this point all Aggrefs
	 * are at the top level of the target list, so we can just scan the list
	 * rather than recursing through the expression trees.
	 */
	foreach(lc, partial_target->exprs)
	{
		Aggref	   *aggref = (Aggref *) lfirst(lc);

		if (IsA(aggref, Aggref))
		{
			Aggref	   *newaggref;

			/*
			 * We shouldn't need to copy the substructure of the Aggref node,
			 * but flat-copy the node itself to avoid damaging other trees.
			 */
			newaggref = makeNode(Aggref);
			memcpy(newaggref, aggref, sizeof(Aggref));

			/* For now, assume serialization is required */
			mark_partial_aggref(newaggref, AGGSPLIT_INITIAL_SERIAL);

			lfirst(lc) = newaggref;
		}
	}

	/* clean up cruft */
	list_free(non_group_exprs);
	list_free(non_group_cols);

	/* XXX this causes some redundant cost calculation ... */
	return set_pathtarget_cost_width(root, partial_target);
}

/*
 * mark_partial_aggref
 *	  Adjust an Aggref to make it represent a partial-aggregation step.
 *
 * The Aggref node is modified in-place; caller must do any copying required.
 */
void
mark_partial_aggref(Aggref *agg, AggSplit aggsplit)
{
	/* aggtranstype should be computed by this point */
	Assert(OidIsValid(agg->aggtranstype));
	/* ... but aggsplit should still be as the parser left it */
	Assert(agg->aggsplit == AGGSPLIT_SIMPLE);

	/* Mark the Aggref with the intended partial-aggregation mode */
	agg->aggsplit = aggsplit;

	/*
	 * Adjust result type if needed.  Normally, a partial aggregate returns
	 * the aggregate's transition type; but if that's INTERNAL and we're
	 * serializing, it returns BYTEA instead.
	 */
	if (DO_AGGSPLIT_SKIPFINAL(aggsplit))
	{
		if (agg->aggtranstype == INTERNALOID && DO_AGGSPLIT_SERIALIZE(aggsplit))
			agg->aggtype = BYTEAOID;
		else
			agg->aggtype = agg->aggtranstype;
	}
}

/*
 * postprocess_setop_tlist
 *	  Fix up targetlist returned by plan_set_operations().
 *
 * We need to transpose sort key info from the orig_tlist into new_tlist.
 * NOTE: this would not be good enough if we supported resjunk sort keys
 * for results of set operations --- then, we'd need to project a whole
 * new tlist to evaluate the resjunk columns.  For now, just ereport if we
 * find any resjunk columns in orig_tlist.
 */
static List *
postprocess_setop_tlist(List *new_tlist, List *orig_tlist)
{
	ListCell   *l;
	ListCell   *orig_tlist_item = list_head(orig_tlist);

	foreach(l, new_tlist)
	{
		TargetEntry *new_tle = lfirst_node(TargetEntry, l);
		TargetEntry *orig_tle;

		/* ignore resjunk columns in setop result */
		if (new_tle->resjunk)
			continue;

		Assert(orig_tlist_item != NULL);
		orig_tle = lfirst_node(TargetEntry, orig_tlist_item);
		orig_tlist_item = lnext(orig_tlist_item);
		if (orig_tle->resjunk)	/* should not happen */
			elog(ERROR, "resjunk output columns are not implemented");
		Assert(new_tle->resno == orig_tle->resno);
		new_tle->ressortgroupref = orig_tle->ressortgroupref;
	}
	if (orig_tlist_item != NULL)
		elog(ERROR, "resjunk output columns are not implemented");
	return new_tlist;
}

/*
 * select_active_windows
 *		Create a list of the "active" window clauses (ie, those referenced
 *		by non-deleted WindowFuncs) in the order they are to be executed.
 */
static List *
select_active_windows(PlannerInfo *root, WindowFuncLists *wflists)
{
	List	   *windowClause = root->parse->windowClause;
	List	   *result = NIL;
	ListCell   *lc;
	int			nActive = 0;
	WindowClauseSortData *actives = palloc(sizeof(WindowClauseSortData)
										   * list_length(windowClause));

	/* First, construct an array of the active windows */
	foreach(lc, windowClause)
	{
		WindowClause *wc = lfirst_node(WindowClause, lc);

		/* It's only active if wflists shows some related WindowFuncs */
		Assert(wc->winref <= wflists->maxWinRef);
		if (wflists->windowFuncs[wc->winref] == NIL)
			continue;

		actives[nActive].wc = wc;	/* original clause */

		/*
		 * For sorting, we want the list of partition keys followed by the
		 * list of sort keys. But pathkeys construction will remove duplicates
		 * between the two, so we can as well (even though we can't detect all
		 * of the duplicates, since some may come from ECs - that might mean
		 * we miss optimization chances here). We must, however, ensure that
		 * the order of entries is preserved with respect to the ones we do
		 * keep.
		 *
		 * partitionClause and orderClause had their own duplicates removed in
		 * parse analysis, so we're only concerned here with removing
		 * orderClause entries that also appear in partitionClause.
		 */
		actives[nActive].uniqueOrder =
			list_concat_unique(list_copy(wc->partitionClause),
							   wc->orderClause);
		nActive++;
	}

	/*
	 * Sort active windows by their partitioning/ordering clauses, ignoring
	 * any framing clauses, so that the windows that need the same sorting are
	 * adjacent in the list. When we come to generate paths, this will avoid
	 * inserting additional Sort nodes.
	 *
	 * This is how we implement a specific requirement from the SQL standard,
	 * which says that when two or more windows are order-equivalent (i.e.
	 * have matching partition and order clauses, even if their names or
	 * framing clauses differ), then all peer rows must be presented in the
	 * same order in all of them. If we allowed multiple sort nodes for such
	 * cases, we'd risk having the peer rows end up in different orders in
	 * equivalent windows due to sort instability. (See General Rule 4 of
	 * <window clause> in SQL2008 - SQL2016.)
	 *
	 * Additionally, if the entire list of clauses of one window is a prefix
	 * of another, put first the window with stronger sorting requirements.
	 * This way we will first sort for stronger window, and won't have to sort
	 * again for the weaker one.
	 */
	qsort(actives, nActive, sizeof(WindowClauseSortData), common_prefix_cmp);

	/* build ordered list of the original WindowClause nodes */
	for (int i = 0; i < nActive; i++)
		result = lappend(result, actives[i].wc);

	pfree(actives);

	return result;
}

/*
 * common_prefix_cmp
 *	  QSort comparison function for WindowClauseSortData
 *
 * Sort the windows by the required sorting clauses. First, compare the sort
 * clauses themselves. Second, if one window's clauses are a prefix of another
 * one's clauses, put the window with more sort clauses first.
 */
static int
common_prefix_cmp(const void *a, const void *b)
{
	const WindowClauseSortData *wcsa = a;
	const WindowClauseSortData *wcsb = b;
	ListCell   *item_a;
	ListCell   *item_b;

	forboth(item_a, wcsa->uniqueOrder, item_b, wcsb->uniqueOrder)
	{
		SortGroupClause *sca = lfirst_node(SortGroupClause, item_a);
		SortGroupClause *scb = lfirst_node(SortGroupClause, item_b);

		if (sca->tleSortGroupRef > scb->tleSortGroupRef)
			return -1;
		else if (sca->tleSortGroupRef < scb->tleSortGroupRef)
			return 1;
		else if (sca->sortop > scb->sortop)
			return -1;
		else if (sca->sortop < scb->sortop)
			return 1;
		else if (sca->nulls_first && !scb->nulls_first)
			return -1;
		else if (!sca->nulls_first && scb->nulls_first)
			return 1;
		/* no need to compare eqop, since it is fully determined by sortop */
	}

	if (list_length(wcsa->uniqueOrder) > list_length(wcsb->uniqueOrder))
		return -1;
	else if (list_length(wcsa->uniqueOrder) < list_length(wcsb->uniqueOrder))
		return 1;

	return 0;
}

/*
 * make_window_input_target
 *	  Generate appropriate PathTarget for initial input to WindowAgg nodes.
 *
 * When the query has window functions, this function computes the desired
 * target to be computed by the node just below the first WindowAgg.
 * This tlist must contain all values needed to evaluate the window functions,
 * compute the final target list, and perform any required final sort step.
 * If multiple WindowAggs are needed, each intermediate one adds its window
 * function results onto this base tlist; only the topmost WindowAgg computes
 * the actual desired target list.
 *
 * This function is much like make_group_input_target, though not quite enough
 * like it to share code.  As in that function, we flatten most expressions
 * into their component variables.  But we do not want to flatten window
 * PARTITION BY/ORDER BY clauses, since that might result in multiple
 * evaluations of them, which would be bad (possibly even resulting in
 * inconsistent answers, if they contain volatile functions).
 * Also, we must not flatten GROUP BY clauses that were left unflattened by
 * make_group_input_target, because we may no longer have access to the
 * individual Vars in them.
 *
 * Another key difference from make_group_input_target is that we don't
 * flatten Aggref expressions, since those are to be computed below the
 * window functions and just referenced like Vars above that.
 *
 * 'final_target' is the query's final target list (in PathTarget form)
 * 'activeWindows' is the list of active windows previously identified by
 *			select_active_windows.
 *
 * The result is the PathTarget to be computed by the plan node immediately
 * below the first WindowAgg node.
 */
static PathTarget *
make_window_input_target(PlannerInfo *root,
						 PathTarget *final_target,
						 List *activeWindows)
{
	Query	   *parse = root->parse;
	PathTarget *input_target;
	Bitmapset  *sgrefs;
	List	   *flattenable_cols;
	List	   *flattenable_vars;
	int			i;
	ListCell   *lc;

	Assert(parse->hasWindowFuncs);

	/*
	 * Collect the sortgroupref numbers of window PARTITION/ORDER BY clauses
	 * into a bitmapset for convenient reference below.
	 */
	sgrefs = NULL;
	foreach(lc, activeWindows)
	{
		WindowClause *wc = lfirst_node(WindowClause, lc);
		ListCell   *lc2;

		foreach(lc2, wc->partitionClause)
		{
			SortGroupClause *sortcl = lfirst_node(SortGroupClause, lc2);

			sgrefs = bms_add_member(sgrefs, sortcl->tleSortGroupRef);
		}
		foreach(lc2, wc->orderClause)
		{
			SortGroupClause *sortcl = lfirst_node(SortGroupClause, lc2);

			sgrefs = bms_add_member(sgrefs, sortcl->tleSortGroupRef);
		}
	}

	/* Add in sortgroupref numbers of GROUP BY clauses, too */
	foreach(lc, parse->groupClause)
	{
		SortGroupClause *grpcl = lfirst_node(SortGroupClause, lc);

		sgrefs = bms_add_member(sgrefs, grpcl->tleSortGroupRef);
	}

	/*
	 * Construct a target containing all the non-flattenable targetlist items,
	 * and save aside the others for a moment.
	 */
	input_target = create_empty_pathtarget();
	flattenable_cols = NIL;

	i = 0;
	foreach(lc, final_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(final_target, i);

		/*
		 * Don't want to deconstruct window clauses or GROUP BY items.  (Note
		 * that such items can't contain window functions, so it's okay to
		 * compute them below the WindowAgg nodes.)
		 */
		if (sgref != 0 && bms_is_member(sgref, sgrefs))
		{
			/*
			 * Don't want to deconstruct this value, so add it to the input
			 * target as-is.
			 */
			add_column_to_pathtarget(input_target, expr, sgref);
		}
		else
		{
			/*
			 * Column is to be flattened, so just remember the expression for
			 * later call to pull_var_clause.
			 */
			flattenable_cols = lappend(flattenable_cols, expr);
		}

		i++;
	}

	/*
	 * Pull out all the Vars and Aggrefs mentioned in flattenable columns, and
	 * add them to the input target if not already present.  (Some might be
	 * there already because they're used directly as window/group clauses.)
	 *
	 * Note: it's essential to use PVC_INCLUDE_AGGREGATES here, so that any
	 * Aggrefs are placed in the Agg node's tlist and not left to be computed
	 * at higher levels.  On the other hand, we should recurse into
	 * WindowFuncs to make sure their input expressions are available.
	 */
	flattenable_vars = pull_var_clause((Node *) flattenable_cols,
									   PVC_INCLUDE_AGGREGATES |
									   PVC_RECURSE_WINDOWFUNCS |
									   PVC_INCLUDE_PLACEHOLDERS);
	add_new_columns_to_pathtarget(input_target, flattenable_vars);

	/* clean up cruft */
	list_free(flattenable_vars);
	list_free(flattenable_cols);

	/* XXX this causes some redundant cost calculation ... */
	return set_pathtarget_cost_width(root, input_target);
}

/*
 * make_pathkeys_for_window
 *		Create a pathkeys list describing the required input ordering
 *		for the given WindowClause.
 *
 * The required ordering is first the PARTITION keys, then the ORDER keys.
 * In the future we might try to implement windowing using hashing, in which
 * case the ordering could be relaxed, but for now we always sort.
 */
static List *
make_pathkeys_for_window(PlannerInfo *root, WindowClause *wc,
						 List *tlist)
{
	List	   *window_pathkeys;
	List	   *window_sortclauses;

	/* Throw error if can't sort */
	if (!grouping_is_sortable(wc->partitionClause))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not implement window PARTITION BY"),
				 errdetail("Window partitioning columns must be of sortable datatypes.")));
	if (!grouping_is_sortable(wc->orderClause))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not implement window ORDER BY"),
				 errdetail("Window ordering columns must be of sortable datatypes.")));

	/* Okay, make the combined pathkeys */
	window_sortclauses = list_concat(list_copy(wc->partitionClause),
									 list_copy(wc->orderClause));
	window_pathkeys = make_pathkeys_for_sortclauses(root,
													window_sortclauses,
													tlist);
	list_free(window_sortclauses);
	return window_pathkeys;
}

/*
 * make_sort_input_target
 *	  生成适用于排序步骤初始输入的PathTarget
 *
 * 如果查询包含ORDER BY，此函数选择由排序（和DISTINCT，如果有的话，因为Unique无法执行投影）步骤
 * 下方的节点计算的目标列表。这可能与查询的最终输出目标列表相同或不同。
 *
 * 保持排序输入目标列表与最终目标列表相同的主要好处是避免单独的投影节点（如果它们不同，则需要投影节点，
 * 因为排序节点不能执行投影）。但是，将目标列表评估推迟到排序之后也有好处：它确保了目标列表中
 * 任何易变函数的一致评估顺序，并且如果查询包含LIMIT，我们可以在计算后续行的目标列表函数之前停止查询，
 * 这对易变函数和昂贵函数都有益。
 *
 * 我们当前的策略是无条件地将易变表达式推迟到排序之后（假设这是可能的，即它们位于普通目标列表列中，
 * 而不是ORDER BY/GROUP BY/DISTINCT列中）。我们也倾向于推迟集合返回表达式(SRFs)，因为提前运行它们
 * 会使排序数据集膨胀，并且如果排序不稳定，可能会导致意外的输出顺序。但有一个限制：目标列表中的所有SRFs
 * 应该在同一计划步骤中评估，以便它们可以在nodeProjectSet中同步运行。因此，如果任何排序列包含SRFs，
 * 我们就不能推迟任何SRFs。（注意，原则上这个策略可能也应该应用于分组/窗口输入目标列表，但历史上我们没有这样做。）
 * 最后，如果存在LIMIT，或者root->tuple_fraction表明查询可能部分评估（如果两者都不成立，我们预期无论如何都必须
 * 为每一行评估表达式），或者如果有任何易变或集合返回表达式（因为一旦我们设置了投影，推迟更多内容不会产生额外成本），
 * 则会推迟昂贵的表达式。
 *
 * 这里可能需要考虑的另一个问题是，评估目标列表表达式可能会产生比输入Vars更宽或更窄的数据，
 * 从而改变必须通过排序的数据量。但是，对于任何比Var更复杂的表达式，我们通常对其输出宽度了解甚少，
 * 所以目前基于此进行优化似乎风险太大。
 *
 * 请注意，如果我们确实生成了修改后的排序输入目标，而查询最终没有使用显式排序，则不会造成特别的危害：
 * 我们最初会为前面的路径节点使用修改后的目标，但随后会使用apply_projection_to_path将它们更改为最终目标。
 * 此外，在这种情况下，关于易变函数评估顺序的保证仍然有效，因为行已经排序了。
 *
 * 此函数与make_group_input_target和make_window_input_target有一些共同点，尽管具体的处理规则不同。
 * 我们从不展平/推迟任何分组或排序列；这些列在排序之前是必需的。如果我们确实展平了某个表达式，
 * 我们会保留Aggref和WindowFunc节点不变，因为它们之前已经计算过了。
 *
 * 'final_target'是查询的最终目标列表（以PathTarget形式）
 * 'have_postponed_srfs'是一个输出参数，见下文
 *
 * 结果是由排序步骤（如果有的话，还有Distinct步骤）正下方的计划节点计算的PathTarget。
 * 如果我们决定投影步骤没有帮助，这将完全等于final_target。
 *
 * 此外，如果我们选择将任何集合返回函数推迟到排序之后，则*have_postponed_srfs设置为true。
 */
static PathTarget *
make_sort_input_target(PlannerInfo *root,
					   PathTarget *final_target,
					   bool *have_postponed_srfs)
{
	Query	   *parse = root->parse;		/* 查询分析器的输出，包含排序子句等信息 */
	PathTarget *input_target;				/* 要返回的排序输入目标 */
	int			ncols;						/* 目标列表中的列数 */
	bool	   *col_is_srf;					/* 记录每列是否包含集合返回函数(SRF) */
	bool	   *postpone_col;				/* 记录哪些列应该被推迟到排序后计算 */
	bool		have_srf;					/* 是否有任何列包含SRF */
	bool		have_volatile;				/* 是否有任何列包含易变函数 */
	bool		have_expensive;				/* 是否有任何列包含昂贵函数 */
	bool		have_srf_sortcols;			/* 排序列中是否有SRF */
	bool		postpone_srfs;				/* 是否推迟SRF的计算 */
	List	   *postponable_cols;			/* 可以推迟计算的列列表 */
	List	   *postponable_vars;			/* 从推迟列中提取的变量列表 */
	int			i;							/* 列索引计数器 */
	ListCell   *lc;							/* 用于遍历列表的指针 */

	/* 除非查询有ORDER BY，否则不应该调用此函数 */
	Assert(parse->sortClause);

	*have_postponed_srfs = false;	/* 默认情况下没有推迟的SRF */

	/* 检查目标列表并收集每列的信息 */
	ncols = list_length(final_target->exprs);
	col_is_srf = (bool *) palloc0(ncols * sizeof(bool));		/* 为每列分配内存，初始化为false */
	postpone_col = (bool *) palloc0(ncols * sizeof(bool));	/* 为每列分配内存，初始化为false */
	have_srf = have_volatile = have_expensive = have_srf_sortcols = false;	/* 初始化标志位 */

	i = 0;
	foreach(lc, final_target->exprs)	/* 遍历目标列表中的每一列 */
	{
		Expr	   *expr = (Expr *) lfirst(lc);	/* 当前列的表达式 */

		/*
		 * 如果列有sortgroupref，假设它必须在排序前评估。通常，这些列是ORDER BY、GROUP BY等目标。
		 * 一个例外是被remove_useless_groupby_columns()从GROUP BY中移除的列...但这些列只会是Vars。
		 * 似乎没有任何情况值得我们为此进行双重检查。
		 */
		if (get_pathtarget_sortgroupref(final_target, i) == 0)
		{
			/*
			 * 检查SRF或易变函数。先检查SRF情况，因为我们必须知道是否有任何推迟的SRF。
			 */
			if (parse->hasTargetSRFs &&
				expression_returns_set((Node *) expr))
			{
				/* 稍后决定这些是否可以推迟 */
				col_is_srf[i] = true;
				have_srf = true;
			}
			else if (contain_volatile_functions((Node *) expr))
			{
				/* 无条件推迟易变函数 */
				postpone_col[i] = true;
				have_volatile = true;
			}
			else
			{
				/*
				 * 否则检查成本。XXX当set_pathtarget_cost_width()刚刚执行过时，这有点令人讨厌。
				 * 是否可以重构以允许共享工作？
				 */
				QualCost	cost;

				cost_qual_eval_node(&cost, (Node *) expr, root);

				/*
				 * 我们任意地将"昂贵"定义为"超过10倍cpu_operator_cost"。注意，这将包括任何具有默认成本的PL函数。
				 */
				if (cost.per_tuple > 10 * cpu_operator_cost)
				{
					postpone_col[i] = true;
					have_expensive = true;
				}
			}
		}
		else
		{
			/* 对于有sortgroupref的列，只需检查是否包含SRF */
			if (!have_srf_sortcols &&
				parse->hasTargetSRFs &&
				expression_returns_set((Node *) expr))
				have_srf_sortcols = true;
		}

		i++;
	}

	/*
	 * 如果有SRF但排序列中没有SRF，我们可以推迟SRF的计算。
	 */
	postpone_srfs = (have_srf && !have_srf_sortcols);

	/*
	 * 如果我们不需要排序后投影，直接返回final_target。
	 * 需要排序后投影的情况：
	 * 1. 需要推迟SRF
	 * 2. 有易变函数
	 * 3. 有昂贵函数且有LIMIT或可以部分评估查询
	 */
	if (!(postpone_srfs || have_volatile ||
		  (have_expensive &&
		   (parse->limitCount || root->tuple_fraction > 0))))
		return final_target;

	/*
	 * 报告排序后投影是否将包含集合返回函数。这很重要，因为它影响排序是否可以依赖查询的LIMIT（如果有）
	 * 来限制它需要返回的行数。
	 */
	*have_postponed_srfs = postpone_srfs;

	/*
	 * 构建排序输入目标，获取所有不可推迟的列，然后添加在可推迟列中找到的Vars、PlaceHolderVars、Aggrefs和WindowFuncs。
	 */
	input_target = create_empty_pathtarget();	/* 创建空的目标列表 */
	postponable_cols = NIL;						/* 初始化可推迟列列表为空 */

	i = 0;
	foreach(lc, final_target->exprs)	/* 再次遍历目标列表中的每一列 */
	{
		Expr	   *expr = (Expr *) lfirst(lc);

		/* 如果列应该被推迟或包含SRF且允许推迟SRF，则加入可推迟列列表 */
		if (postpone_col[i] || (postpone_srfs && col_is_srf[i]))
			postponable_cols = lappend(postponable_cols, expr);
		else
			/* 否则，直接添加到排序输入目标中 */
			add_column_to_pathtarget(input_target, expr,
									 get_pathtarget_sortgroupref(final_target, i));

		i++;
	}

	/*
	 * 提取可推迟列中提到的所有Vars、Aggrefs和WindowFuncs，并将它们添加到排序输入目标中（如果尚不存在）。
	 * （有些可能已经在那里了。）我们不能在这里解构Aggrefs或WindowFuncs，因为投影节点将无法重新计算它们。
	 */
	postponable_vars = pull_var_clause((Node *) postponable_cols,
									   PVC_INCLUDE_AGGREGATES |
									   PVC_INCLUDE_WINDOWFUNCS |
									   PVC_INCLUDE_PLACEHOLDERS);
	add_new_columns_to_pathtarget(input_target, postponable_vars);

	/* 清理临时变量 */
	list_free(postponable_vars);
	list_free(postponable_cols);

	/* XXX 这表示更多冗余的成本计算... */
	return set_pathtarget_cost_width(root, input_target);	/* 设置成本和宽度后返回 */
}


/*
 * get_cheapest_fractional_path
 *	  这个函数会根据 tuple_fraction 来挑选最划算的路径。这在优化 LIMIT 查询或 EXISTS 子查询时非常关键。
 *	  如果需要所有行，它通常会选总代价（Total Cost）最低的路径。
 *	  如果只需要前几行，它可能会选启动代价（Startup Cost）较低的路径（例如索引扫描），即使总代价较高。
 *
 * tuple_fraction 的解释方式与 grouping_planner 相同。
 *
 * 假定已对给定 rel 执行 set_cheapest()。
 */
Path *
get_cheapest_fractional_path(RelOptInfo *rel, double tuple_fraction)
{
	Path	   *best_path = rel->cheapest_total_path;
	ListCell   *l;


	/* If there is no cheapest_total_path, return NULL */
	if (best_path == NULL)
		return NULL;
	
	/*
	 * 如果 tuple_fraction <= 0.0（表示需要读取所有行），那么总代价最低的路径确实就是最好的，直接返回。
	 */
	if (tuple_fraction <= 0.0)
		return best_path;

	/*
	 * 如果 tuple_fraction 是绝对数量，则转换为比例；无需限制在 0..1 范围
	 * tuple_fraction 可以是比例（0.0 - 1.0），也可以是具体的行数（>= 1.0）。
	 * 如果是行数（例如 LIMIT 10），这里把它转换为比例（10 / 总行数），方便后续统一计算。
	 */
	if (tuple_fraction >= 1.0 && best_path->rows > 0)
		tuple_fraction /= best_path->rows;
	
	/*
	 * 遍历 rel 的所有路径，找出在给定 tuple_fraction 下成本最低的路径。
	 * compare_fractional_path_costs() 会根据 tuple_fraction 计算每条路径的实际成本。
	 */
	foreach(l, rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(l);

		if (path == rel->cheapest_total_path ||
			compare_fractional_path_costs(best_path, path, tuple_fraction) <= 0)
			continue;

		best_path = path;
	}

	return best_path;
}

/*
 * adjust_paths_for_srfs
 *      调整给定上层关系(upperrel)的路径，以正确处理集合返回函数(SRFs)。
 *
 * 执行器只能处理出现在ProjectSet计划节点目标列表顶层的集合返回函数。
 * 如果我们有任何不在顶层的SRFs，需要将评估拆分为多个计划级别，每个级别都满足这个约束。
 * 此函数修改上层关系中可能在其输出目标列表中计算SRFs的每个路径，插入适当的投影步骤。
 *
 * 给定的targets和targets_contain_srfs列表来自split_pathtarget_at_srfs()函数。
 * 我们假设现有的路径发出targets中的第一个目标。
 */
static void
adjust_paths_for_srfs(PlannerInfo *root, RelOptInfo *rel,
                      List *targets, List *targets_contain_srfs)
{
    ListCell   *lc;

    /* 验证targets和targets_contain_srfs列表长度相同 */
    Assert(list_length(targets) == list_length(targets_contain_srfs));
    /* 验证第一个目标不包含SRFs */
    Assert(!linitial_int(targets_contain_srfs));

    /* 如果在此计划级别没有SRFs出现，则无需处理 */
    if (list_length(targets) == 1)
        return;

    /*
     * 在关系的每个路径上堆叠SRF评估节点。
     *
     * 原则上，我们应该在这里重新运行set_cheapest()来识别最便宜的路径，
     * 但向所有路径添加相同的目标列表评估成本似乎不太可能改变这一点，因此我们不这样做。
     * 相反，我们假设cheapest-startup和cheapest-total路径保持不变。
     * （现在应该没有参数化路径了，所以我们不需要担心更新cheapest_parameterized_paths。）
     */
    foreach(lc, rel->pathlist)
    {
        Path       *subpath = (Path *) lfirst(lc);
        Path       *newpath = subpath;
        ListCell   *lc1,
                   *lc2;

        /* 确保没有参数化路径 */
        Assert(subpath->param_info == NULL);
        /* 同时遍历targets和targets_contain_srfs */
        forboth(lc1, targets, lc2, targets_contain_srfs)
        {
            PathTarget *thistarget = lfirst_node(PathTarget, lc1);
            bool        contains_srfs = (bool) lfirst_int(lc2);

            /* 如果此级别包含SRFs，则创建set投影；否则执行常规投影 */
            if (contains_srfs)
                newpath = (Path *) create_set_projection_path(root,
                                                             rel,
                                                             newpath,
                                                             thistarget);
            else
                newpath = (Path *) apply_projection_to_path(root,
                                                           rel,
                                                           newpath,
                                                           thistarget);
        }
        /* 更新路径列表中的路径 */
        lfirst(lc) = newpath;
        /* 如果原路径是最便宜的启动路径，更新最便宜的启动路径引用 */
        if (subpath == rel->cheapest_startup_path)
            rel->cheapest_startup_path = newpath;
        /* 如果原路径是最便宜的总成本路径，更新最便宜的总成本路径引用 */
        if (subpath == rel->cheapest_total_path)
            rel->cheapest_total_path = newpath;
    }

    /* 同样处理部分路径（如果有） */
    foreach(lc, rel->partial_pathlist)
    {
        Path       *subpath = (Path *) lfirst(lc);
        Path       *newpath = subpath;
        ListCell   *lc1,
                   *lc2;

        /* 确保没有参数化路径 */
        Assert(subpath->param_info == NULL);
        /* 同时遍历targets和targets_contain_srfs */
        forboth(lc1, targets, lc2, targets_contain_srfs)
        {
            PathTarget *thistarget = lfirst_node(PathTarget, lc1);
            bool        contains_srfs = (bool) lfirst_int(lc2);

            /* 如果此级别包含SRFs，则创建set投影；否则执行常规投影 */
            if (contains_srfs)
                newpath = (Path *) create_set_projection_path(root,
                                                             rel,
                                                             newpath,
                                                             thistarget);
            else
            {
                /* 避免使用apply_projection_to_path，以防多次引用 */
                newpath = (Path *) create_projection_path(root,
                                                         rel,
                                                         newpath,
                                                         thistarget);
            }
        }
        /* 更新部分路径列表中的路径 */
        lfirst(lc) = newpath;
    }
}


/*
 * expression_planner
 *		Perform planner's transformations on a standalone expression.
 *
 * Various utility commands need to evaluate expressions that are not part
 * of a plannable query.  They can do so using the executor's regular
 * expression-execution machinery, but first the expression has to be fed
 * through here to transform it from parser output to something executable.
 *
 * Currently, we disallow sublinks in standalone expressions, so there's no
 * real "planning" involved here.  (That might not always be true though.)
 * What we must do is run eval_const_expressions to ensure that any function
 * calls are converted to positional notation and function default arguments
 * get inserted.  The fact that constant subexpressions get simplified is a
 * side-effect that is useful when the expression will get evaluated more than
 * once.  Also, we must fix operator function IDs.
 *
 * This does not return any information about dependencies of the expression.
 * Hence callers should use the results only for the duration of the current
 * query.  Callers that would like to cache the results for longer should use
 * expression_planner_with_deps, probably via the plancache.
 *
 * Note: this must not make any damaging changes to the passed-in expression
 * tree.  (It would actually be okay to apply fix_opfuncids to it, but since
 * we first do an expression_tree_mutator-based walk, what is returned will
 * be a new node tree.)  The result is constructed in the current memory
 * context; beware that this can leak a lot of additional stuff there, too.
 */
Expr *
expression_planner(Expr *expr)
{
	Node	   *result;

	/*
	 * Convert named-argument function calls, insert default arguments and
	 * simplify constant subexprs
	 */
	result = eval_const_expressions(NULL, (Node *) expr);

	/* Fill in opfuncid values if missing */
	fix_opfuncids(result);

	return (Expr *) result;
}

/*
 * expression_planner_with_deps
 *		Perform planner's transformations on a standalone expression,
 *		returning expression dependency information along with the result.
 *
 * This is identical to expression_planner() except that it also returns
 * information about possible dependencies of the expression, ie identities of
 * objects whose definitions affect the result.  As in a PlannedStmt, these
 * are expressed as a list of relation Oids and a list of PlanInvalItems.
 */
Expr *
expression_planner_with_deps(Expr *expr,
							 List **relationOids,
							 List **invalItems)
{
	Node	   *result;
	PlannerGlobal glob;
	PlannerInfo root;

	/* Make up dummy planner state so we can use setrefs machinery */
	MemSet(&glob, 0, sizeof(glob));
	glob.type = T_PlannerGlobal;
	glob.relationOids = NIL;
	glob.invalItems = NIL;

	MemSet(&root, 0, sizeof(root));
	root.type = T_PlannerInfo;
	root.glob = &glob;

	/*
	 * Convert named-argument function calls, insert default arguments and
	 * simplify constant subexprs.  Collect identities of inlined functions
	 * and elided domains, too.
	 */
	result = eval_const_expressions(&root, (Node *) expr);

	/* Fill in opfuncid values if missing */
	fix_opfuncids(result);

	/*
	 * Now walk the finished expression to find anything else we ought to
	 * record as an expression dependency.
	 */
	(void) extract_query_dependencies_walker(result, &root);

	*relationOids = glob.relationOids;
	*invalItems = glob.invalItems;

	return (Expr *) result;
}


/*
 * plan_cluster_use_sort
 *		Use the planner to decide how CLUSTER should implement sorting
 *
 * tableOid is the OID of a table to be clustered on its index indexOid
 * (which is already known to be a btree index).  Decide whether it's
 * cheaper to do an indexscan or a seqscan-plus-sort to execute the CLUSTER.
 * Return true to use sorting, false to use an indexscan.
 *
 * Note: caller had better already hold some type of lock on the table.
 */
bool
plan_cluster_use_sort(Oid tableOid, Oid indexOid)
{
	PlannerInfo *root;
	Query	   *query;
	PlannerGlobal *glob;
	RangeTblEntry *rte;
	RelOptInfo *rel;
	IndexOptInfo *indexInfo;
	QualCost	indexExprCost;
	Cost		comparisonCost;
	Path	   *seqScanPath;
	Path		seqScanAndSortPath;
	IndexPath  *indexScanPath;
	ListCell   *lc;

	/* We can short-circuit the cost comparison if indexscans are disabled */
	if (!enable_indexscan)
		return true;			/* use sort */

	/* Set up mostly-dummy planner state */
	query = makeNode(Query);
	query->commandType = CMD_SELECT;

	glob = makeNode(PlannerGlobal);

	root = makeNode(PlannerInfo);
	root->parse = query;
	root->glob = glob;
	root->query_level = 1;
	root->planner_cxt = CurrentMemoryContext;
	root->wt_param_id = -1;

	/* Build a minimal RTE for the rel */
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = tableOid;
	rte->relkind = RELKIND_RELATION;	/* Don't be too picky. */
	rte->rellockmode = AccessShareLock;
	rte->lateral = false;
	rte->inh = false;
	rte->inFromCl = true;
	query->rtable = list_make1(rte);

	/* Set up RTE/RelOptInfo arrays */
	setup_simple_rel_arrays(root);

	/* Build RelOptInfo */
	rel = build_simple_rel(root, 1, NULL);

	/* Locate IndexOptInfo for the target index */
	indexInfo = NULL;
	foreach(lc, rel->indexlist)
	{
		indexInfo = lfirst_node(IndexOptInfo, lc);
		if (indexInfo->indexoid == indexOid)
			break;
	}

	/*
	 * It's possible that get_relation_info did not generate an IndexOptInfo
	 * for the desired index; this could happen if it's not yet reached its
	 * indcheckxmin usability horizon, or if it's a system index and we're
	 * ignoring system indexes.  In such cases we should tell CLUSTER to not
	 * trust the index contents but use seqscan-and-sort.
	 */
	if (lc == NULL)				/* not in the list? */
		return true;			/* use sort */

	/*
	 * Rather than doing all the pushups that would be needed to use
	 * set_baserel_size_estimates, just do a quick hack for rows and width.
	 */
	rel->rows = rel->tuples;
	rel->reltarget->width = get_relation_data_width(tableOid, NULL);

	root->total_table_pages = rel->pages;

	/*
	 * Determine eval cost of the index expressions, if any.  We need to
	 * charge twice that amount for each tuple comparison that happens during
	 * the sort, since tuplesort.c will have to re-evaluate the index
	 * expressions each time.  (XXX that's pretty inefficient...)
	 */
	cost_qual_eval(&indexExprCost, indexInfo->indexprs, root);
	comparisonCost = 2.0 * (indexExprCost.startup + indexExprCost.per_tuple);

	/* Estimate the cost of seq scan + sort */
	seqScanPath = create_seqscan_path(root, rel, NULL, 0);
	cost_sort(&seqScanAndSortPath, root, NIL,
			  seqScanPath->total_cost, rel->tuples, rel->reltarget->width,
			  comparisonCost, maintenance_work_mem, -1.0);

	/* Estimate the cost of index scan */
	indexScanPath = create_index_path(root, indexInfo,
									  NIL, NIL, NIL, NIL,
									  ForwardScanDirection, false,
									  NULL, 1.0, false);

	return (seqScanAndSortPath.total_cost < indexScanPath->path.total_cost);
}

/*
 * plan_create_index_workers
 *		Use the planner to decide how many parallel worker processes
 *		CREATE INDEX should request for use
 *
 * tableOid is the table on which the index is to be built.  indexOid is the
 * OID of an index to be created or reindexed (which must be a btree index).
 *
 * Return value is the number of parallel worker processes to request.  It
 * may be unsafe to proceed if this is 0.  Note that this does not include the
 * leader participating as a worker (value is always a number of parallel
 * worker processes).
 *
 * Note: caller had better already hold some type of lock on the table and
 * index.
 */
int
plan_create_index_workers(Oid tableOid, Oid indexOid)
{
	PlannerInfo *root;
	Query	   *query;
	PlannerGlobal *glob;
	RangeTblEntry *rte;
	Relation	heap;
	Relation	index;
	RelOptInfo *rel;
	int			parallel_workers;
	BlockNumber heap_blocks;
	double		reltuples;
	double		allvisfrac;

	/*
	 * We don't allow performing parallel operation in standalone backend or
	 * when parallelism is disabled.
	 */
	if (!IsUnderPostmaster || max_parallel_maintenance_workers == 0)
		return 0;

	/* Set up largely-dummy planner state */
	query = makeNode(Query);
	query->commandType = CMD_SELECT;

	glob = makeNode(PlannerGlobal);

	root = makeNode(PlannerInfo);
	root->parse = query;
	root->glob = glob;
	root->query_level = 1;
	root->planner_cxt = CurrentMemoryContext;
	root->wt_param_id = -1;

	/*
	 * Build a minimal RTE.
	 *
	 * Mark the RTE with inh = true.  This is a kludge to prevent
	 * get_relation_info() from fetching index info, which is necessary
	 * because it does not expect that any IndexOptInfo is currently
	 * undergoing REINDEX.
	 */
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = tableOid;
	rte->relkind = RELKIND_RELATION;	/* Don't be too picky. */
	rte->rellockmode = AccessShareLock;
	rte->lateral = false;
	rte->inh = true;
	rte->inFromCl = true;
	query->rtable = list_make1(rte);

	/* Set up RTE/RelOptInfo arrays */
	setup_simple_rel_arrays(root);

	/* Build RelOptInfo */
	rel = build_simple_rel(root, 1, NULL);

	/* Rels are assumed already locked by the caller */
	heap = table_open(tableOid, NoLock);
	index = index_open(indexOid, NoLock);

	/*
	 * Determine if it's safe to proceed.
	 *
	 * Currently, parallel workers can't access the leader's temporary tables.
	 * Furthermore, any index predicate or index expressions must be parallel
	 * safe.
	 */
	if (heap->rd_rel->relpersistence == RELPERSISTENCE_TEMP ||
		!is_parallel_safe(root, (Node *) RelationGetIndexExpressions(index)) ||
		!is_parallel_safe(root, (Node *) RelationGetIndexPredicate(index)))
	{
		parallel_workers = 0;
		goto done;
	}

	/*
	 * If parallel_workers storage parameter is set for the table, accept that
	 * as the number of parallel worker processes to launch (though still cap
	 * at max_parallel_maintenance_workers).  Note that we deliberately do not
	 * consider any other factor when parallel_workers is set. (e.g., memory
	 * use by workers.)
	 */
	if (rel->rel_parallel_workers != -1)
	{
		parallel_workers = Min(rel->rel_parallel_workers,
							   max_parallel_maintenance_workers);
		goto done;
	}

	/*
	 * Estimate heap relation size ourselves, since rel->pages cannot be
	 * trusted (heap RTE was marked as inheritance parent)
	 */
	estimate_rel_size(heap, NULL, &heap_blocks, &reltuples, &allvisfrac);

	/*
	 * Determine number of workers to scan the heap relation using generic
	 * model
	 */
	parallel_workers = compute_parallel_worker(rel, heap_blocks, -1,
											   max_parallel_maintenance_workers);

	/*
	 * Cap workers based on available maintenance_work_mem as needed.
	 *
	 * Note that each tuplesort participant receives an even share of the
	 * total maintenance_work_mem budget.  Aim to leave participants
	 * (including the leader as a participant) with no less than 32MB of
	 * memory.  This leaves cases where maintenance_work_mem is set to 64MB
	 * immediately past the threshold of being capable of launching a single
	 * parallel worker to sort.
	 */
	while (parallel_workers > 0 &&
		   maintenance_work_mem / (parallel_workers + 1) < 32768L)
		parallel_workers--;

done:
	index_close(index, NoLock);
	table_close(heap, NoLock);

	return parallel_workers;
}

/*
 * add_paths_to_grouping_rel
 *
 * 为分组关系添加非部分路径。
 * 此函数负责为分组操作（GROUP BY、GROUPING SETS或聚合）生成并添加各种可能的执行路径到分组关系中。
 */
static void
add_paths_to_grouping_rel(PlannerInfo *root,  /* 规划器信息结构体指针 */
                          RelOptInfo *input_rel,  /* 输入关系（未分组） */
                          RelOptInfo *grouped_rel,  /* 分组后关系（目标关系） */
                          RelOptInfo *partially_grouped_rel,  /* 部分分组关系（用于并行聚合） */
                          const AggClauseCosts *agg_costs,  /* 聚合函数成本信息 */
                          grouping_sets_data *gd,  /* 分组集数据（GROUPING SETS时使用） */
                          double dNumGroups,  /* 估计的组数 */
                          GroupPathExtraData *extra)  /* 额外的分组路径数据 */
{
    Query   *parse = root->parse;  /* 查询解析树 */
    Path    *cheapest_path = input_rel->cheapest_total_path;  /* 输入关系中总成本最低的路径 */
    ListCell *lc;  /* 列表遍历单元格 */
    /* 检查是否可以使用哈希聚合 */
    bool    can_hash = (extra->flags & GROUPING_CAN_USE_HASH) != 0;
    /* 检查是否可以使用排序聚合 */
    bool    can_sort = (extra->flags & GROUPING_CAN_USE_SORT) != 0;
    /* 获取HAVING子句条件 */
    List   *havingQual = (List *) extra->havingQual;
    /* 获取聚合函数最终成本信息 */
    AggClauseCosts *agg_final_costs = &extra->agg_final_costs;

    /* 处理可排序的情况 */
    if (can_sort)
    {
        /*
         * 使用任何已经按所需顺序排序的路径作为输入，并考虑对
         * 总成本最低的路径进行排序。
         */
        foreach(lc, input_rel->pathlist)
        {
            Path   *path = (Path *) lfirst(lc);  /* 当前考虑的路径 */
            bool    is_sorted;  /* 路径是否已按分组键排序 */

            /* 检查路径是否已按分组键排序 */
            is_sorted = pathkeys_contained_in(root->group_pathkeys,
                                             path->pathkeys);
            
            /* 只考虑成本最低的路径或已排序的路径 */
            if (path == cheapest_path || is_sorted)
            {
                /* 如果路径未排序，则对其进行排序 */
                if (!is_sorted)
                    path = (Path *) create_sort_path(root,
                                                    grouped_rel,
                                                    path,
                                                    root->group_pathkeys,
                                                    -1.0);

                /* 根据查询类型决定在排序路径上添加什么操作 */
                if (parse->groupingSets)
                {
                    /* 处理分组集情况 */
                    consider_groupingsets_paths(root, grouped_rel,
                                               path, true, can_hash,
                                               gd, agg_costs, dNumGroups);
                }
                else if (parse->hasAggs)
                {
                    /*
                     * 有聚合操作，可能带有简单GROUP BY。创建AggPath。
                     */
                    add_path(grouped_rel, (Path *)
                             create_agg_path(root,
                                            grouped_rel,
                                            path,
                                            grouped_rel->reltarget,
                                            parse->groupClause ? AGG_SORTED : AGG_PLAIN,
                                            AGGSPLIT_SIMPLE,
                                            parse->groupClause,
                                            havingQual,
                                            agg_costs,
                                            dNumGroups));
                }
                else if (parse->groupClause)
                {
                    /*
                     * 有GROUP BY但没有聚合或分组集。创建GroupPath。
                     */
                    add_path(grouped_rel, (Path *)
                             create_group_path(root,
                                              grouped_rel,
                                              path,
                                              parse->groupClause,
                                              havingQual,
                                              dNumGroups));
                }
                else
                {
                    /* 其他情况应该已经在上面对应条件中处理 */
                    Assert(false);
                }
            }
        }

        /*
         * 除了直接处理输入关系，我们还可以考虑完成部分聚合的路径。
         */
        if (partially_grouped_rel != NULL)
        {
            foreach(lc, partially_grouped_rel->pathlist)
            {
                Path   *path = (Path *) lfirst(lc);  /* 当前部分聚合路径 */

                /*
                 * 如果需要，插入排序节点。但只有对成本最低的路径排序才有意义。
                 */
                if (!pathkeys_contained_in(root->group_pathkeys, path->pathkeys))
                {
                    if (path != partially_grouped_rel->cheapest_total_path)
                        continue;
                    path = (Path *) create_sort_path(root,
                                                    grouped_rel,
                                                    path,
                                                    root->group_pathkeys,
                                                    -1.0);
                }

                /* 根据是否有聚合函数创建不同的路径 */
                if (parse->hasAggs)
                    add_path(grouped_rel, (Path *)
                             create_agg_path(root,
                                            grouped_rel,
                                            path,
                                            grouped_rel->reltarget,
                                            parse->groupClause ? AGG_SORTED : AGG_PLAIN,
                                            AGGSPLIT_FINAL_DESERIAL,  /* 使用最终反序列化模式 */
                                            parse->groupClause,
                                            havingQual,
                                            agg_final_costs,
                                            dNumGroups));
                else
                    add_path(grouped_rel, (Path *)
                             create_group_path(root,
                                              grouped_rel,
                                              path,
                                              parse->groupClause,
                                              havingQual,
                                              dNumGroups));
            }
        }
    }

    /* 处理可哈希的情况 */
    if (can_hash)
    {
        double  hashaggtablesize;  /* 哈希表大小估计 */

        if (parse->groupingSets)
        {
            /*
             * 尝试在未排序的输入上创建仅哈希的分组集路径。
             */
            consider_groupingsets_paths(root, grouped_rel,
                                       cheapest_path, false, true,
                                       gd, agg_costs, dNumGroups);
        }
        else
        {
            /* 估计哈希聚合表大小 */
            hashaggtablesize = estimate_hashagg_tablesize(cheapest_path,
                                                         agg_costs,
                                                         dNumGroups);

            /*
             * 只要估计的哈希表大小不超过work_mem，我们就会生成一个HashAgg路径，
             * 但如果上面无法排序，那么我们最好生成一个路径，至少保证有一个可用路径。
             */
            if (hashaggtablesize < work_mem * 1024L ||
                grouped_rel->pathlist == NIL)
            {
                /*
                 * 我们只需要在总成本最低的输入路径上添加Agg，因为输入顺序无关紧要。
                 */
                add_path(grouped_rel, (Path *)
                         create_agg_path(root, grouped_rel,
                                         cheapest_path,
                                         grouped_rel->reltarget,
                                         AGG_HASHED,  /* 使用哈希聚合 */
                                         AGGSPLIT_SIMPLE,
                                         parse->groupClause,
                                         havingQual,
                                         agg_costs,
                                         dNumGroups));
            }
        }

        /*
         * 在成本最低的部分分组路径上生成Finalize HashAgg路径，假设存在这样的路径。
         * 同样，只有当哈希表大小看起来不会超过work_mem时才这样做。
         */
        if (partially_grouped_rel && partially_grouped_rel->pathlist)
        {
            Path   *path = partially_grouped_rel->cheapest_total_path;

            /* 估计哈希表大小 */
            hashaggtablesize = estimate_hashagg_tablesize(path,
                                                         agg_final_costs,
                                                         dNumGroups);

            if (hashaggtablesize < work_mem * 1024L)
                add_path(grouped_rel, (Path *)
                         create_agg_path(root,
                                         grouped_rel,
                                         path,
                                         grouped_rel->reltarget,
                                         AGG_HASHED,  /* 使用哈希聚合 */
                                         AGGSPLIT_FINAL_DESERIAL,  /* 使用最终反序列化模式 */
                                         parse->groupClause,
                                         havingQual,
                                         agg_final_costs,
                                         dNumGroups));
        }
    }

    /*
     * 当使用分区聚合时，我们可能在部分路径列表中有完全聚合的路径，
     * 因为add_paths_to_append_rel()会考虑由每个子节点的非部分路径的
     * Parallel Append组成的grouped_rel路径。
     */
    if (grouped_rel->partial_pathlist != NIL)
        gather_grouping_paths(root, grouped_rel);  /* 收集分组路径 */
}


/*
 * create_partial_grouping_paths
 *
 * Create a new upper relation representing the result of partial aggregation
 * and populate it with appropriate paths.  Note that we don't finalize the
 * lists of paths here, so the caller can add additional partial or non-partial
 * paths and must afterward call gather_grouping_paths and set_cheapest on
 * the returned upper relation.
 *
 * All paths for this new upper relation -- both partial and non-partial --
 * have been partially aggregated but require a subsequent FinalizeAggregate
 * step.
 *
 * NB: This function is allowed to return NULL if it determines that there is
 * no real need to create a new RelOptInfo.
 */
static RelOptInfo *
create_partial_grouping_paths(PlannerInfo *root,
							  RelOptInfo *grouped_rel,
							  RelOptInfo *input_rel,
							  grouping_sets_data *gd,
							  GroupPathExtraData *extra,
							  bool force_rel_creation)
{
	Query	   *parse = root->parse;
	RelOptInfo *partially_grouped_rel;
	AggClauseCosts *agg_partial_costs = &extra->agg_partial_costs;
	AggClauseCosts *agg_final_costs = &extra->agg_final_costs;
	Path	   *cheapest_partial_path = NULL;
	Path	   *cheapest_total_path = NULL;
	double		dNumPartialGroups = 0;
	double		dNumPartialPartialGroups = 0;
	ListCell   *lc;
	bool		can_hash = (extra->flags & GROUPING_CAN_USE_HASH) != 0;
	bool		can_sort = (extra->flags & GROUPING_CAN_USE_SORT) != 0;

	/*
	 * Consider whether we should generate partially aggregated non-partial
	 * paths.  We can only do this if we have a non-partial path, and only if
	 * the parent of the input rel is performing partial partitionwise
	 * aggregation.  (Note that extra->patype is the type of partitionwise
	 * aggregation being used at the parent level, not this level.)
	 */
	if (input_rel->pathlist != NIL &&
		extra->patype == PARTITIONWISE_AGGREGATE_PARTIAL)
		cheapest_total_path = input_rel->cheapest_total_path;

	/*
	 * If parallelism is possible for grouped_rel, then we should consider
	 * generating partially-grouped partial paths.  However, if the input rel
	 * has no partial paths, then we can't.
	 */
	if (grouped_rel->consider_parallel && input_rel->partial_pathlist != NIL)
		cheapest_partial_path = linitial(input_rel->partial_pathlist);

	/*
	 * If we can't partially aggregate partial paths, and we can't partially
	 * aggregate non-partial paths, then don't bother creating the new
	 * RelOptInfo at all, unless the caller specified force_rel_creation.
	 */
	if (cheapest_total_path == NULL &&
		cheapest_partial_path == NULL &&
		!force_rel_creation)
		return NULL;

	/*
	 * Build a new upper relation to represent the result of partially
	 * aggregating the rows from the input relation.
	 */
	partially_grouped_rel = fetch_upper_rel(root,
											UPPERREL_PARTIAL_GROUP_AGG,
											grouped_rel->relids);
	partially_grouped_rel->consider_parallel =
		grouped_rel->consider_parallel;
	partially_grouped_rel->reloptkind = grouped_rel->reloptkind;
	partially_grouped_rel->serverid = grouped_rel->serverid;
	partially_grouped_rel->userid = grouped_rel->userid;
	partially_grouped_rel->useridiscurrent = grouped_rel->useridiscurrent;
	partially_grouped_rel->fdwroutine = grouped_rel->fdwroutine;

	/*
	 * Build target list for partial aggregate paths.  These paths cannot just
	 * emit the same tlist as regular aggregate paths, because (1) we must
	 * include Vars and Aggrefs needed in HAVING, which might not appear in
	 * the result tlist, and (2) the Aggrefs must be set in partial mode.
	 */
	partially_grouped_rel->reltarget =
		make_partial_grouping_target(root, grouped_rel->reltarget,
									 extra->havingQual);

	if (!extra->partial_costs_set)
	{
		/*
		 * Collect statistics about aggregates for estimating costs of
		 * performing aggregation in parallel.
		 */
		MemSet(agg_partial_costs, 0, sizeof(AggClauseCosts));
		MemSet(agg_final_costs, 0, sizeof(AggClauseCosts));
		if (parse->hasAggs)
		{
			List	   *partial_target_exprs;

			/* partial phase */
			partial_target_exprs = partially_grouped_rel->reltarget->exprs;
			get_agg_clause_costs(root, (Node *) partial_target_exprs,
								 AGGSPLIT_INITIAL_SERIAL,
								 agg_partial_costs);

			/* final phase */
			get_agg_clause_costs(root, (Node *) grouped_rel->reltarget->exprs,
								 AGGSPLIT_FINAL_DESERIAL,
								 agg_final_costs);
			get_agg_clause_costs(root, extra->havingQual,
								 AGGSPLIT_FINAL_DESERIAL,
								 agg_final_costs);
		}

		extra->partial_costs_set = true;
	}

	/* Estimate number of partial groups. */
	if (cheapest_total_path != NULL)
		dNumPartialGroups =
			get_number_of_groups(root,
								 cheapest_total_path->rows,
								 gd,
								 extra->targetList);
	if (cheapest_partial_path != NULL)
		dNumPartialPartialGroups =
			get_number_of_groups(root,
								 cheapest_partial_path->rows,
								 gd,
								 extra->targetList);

	if (can_sort && cheapest_total_path != NULL)
	{
		/* This should have been checked previously */
		Assert(parse->hasAggs || parse->groupClause);

		/*
		 * Use any available suitably-sorted path as input, and also consider
		 * sorting the cheapest partial path.
		 */
		foreach(lc, input_rel->pathlist)
		{
			Path	   *path = (Path *) lfirst(lc);
			bool		is_sorted;

			is_sorted = pathkeys_contained_in(root->group_pathkeys,
											  path->pathkeys);
			if (path == cheapest_total_path || is_sorted)
			{
				/* Sort the cheapest partial path, if it isn't already */
				if (!is_sorted)
					path = (Path *) create_sort_path(root,
													 partially_grouped_rel,
													 path,
													 root->group_pathkeys,
													 -1.0);

				if (parse->hasAggs)
					add_path(partially_grouped_rel, (Path *)
							 create_agg_path(root,
											 partially_grouped_rel,
											 path,
											 partially_grouped_rel->reltarget,
											 parse->groupClause ? AGG_SORTED : AGG_PLAIN,
											 AGGSPLIT_INITIAL_SERIAL,
											 parse->groupClause,
											 NIL,
											 agg_partial_costs,
											 dNumPartialGroups));
				else
					add_path(partially_grouped_rel, (Path *)
							 create_group_path(root,
											   partially_grouped_rel,
											   path,
											   parse->groupClause,
											   NIL,
											   dNumPartialGroups));
			}
		}
	}

	if (can_sort && cheapest_partial_path != NULL)
	{
		/* Similar to above logic, but for partial paths. */
		foreach(lc, input_rel->partial_pathlist)
		{
			Path	   *path = (Path *) lfirst(lc);
			bool		is_sorted;

			is_sorted = pathkeys_contained_in(root->group_pathkeys,
											  path->pathkeys);
			if (path == cheapest_partial_path || is_sorted)
			{
				/* Sort the cheapest partial path, if it isn't already */
				if (!is_sorted)
					path = (Path *) create_sort_path(root,
													 partially_grouped_rel,
													 path,
													 root->group_pathkeys,
													 -1.0);

				if (parse->hasAggs)
					add_partial_path(partially_grouped_rel, (Path *)
									 create_agg_path(root,
													 partially_grouped_rel,
													 path,
													 partially_grouped_rel->reltarget,
													 parse->groupClause ? AGG_SORTED : AGG_PLAIN,
													 AGGSPLIT_INITIAL_SERIAL,
													 parse->groupClause,
													 NIL,
													 agg_partial_costs,
													 dNumPartialPartialGroups));
				else
					add_partial_path(partially_grouped_rel, (Path *)
									 create_group_path(root,
													   partially_grouped_rel,
													   path,
													   parse->groupClause,
													   NIL,
													   dNumPartialPartialGroups));
			}
		}
	}

	if (can_hash && cheapest_total_path != NULL)
	{
		double		hashaggtablesize;

		/* Checked above */
		Assert(parse->hasAggs || parse->groupClause);

		hashaggtablesize =
			estimate_hashagg_tablesize(cheapest_total_path,
									   agg_partial_costs,
									   dNumPartialGroups);

		/*
		 * Tentatively produce a partial HashAgg Path, depending on if it
		 * looks as if the hash table will fit in work_mem.
		 */
		if (hashaggtablesize < work_mem * 1024L &&
			cheapest_total_path != NULL)
		{
			add_path(partially_grouped_rel, (Path *)
					 create_agg_path(root,
									 partially_grouped_rel,
									 cheapest_total_path,
									 partially_grouped_rel->reltarget,
									 AGG_HASHED,
									 AGGSPLIT_INITIAL_SERIAL,
									 parse->groupClause,
									 NIL,
									 agg_partial_costs,
									 dNumPartialGroups));
		}
	}

	if (can_hash && cheapest_partial_path != NULL)
	{
		double		hashaggtablesize;

		hashaggtablesize =
			estimate_hashagg_tablesize(cheapest_partial_path,
									   agg_partial_costs,
									   dNumPartialPartialGroups);

		/* Do the same for partial paths. */
		if (hashaggtablesize < work_mem * 1024L &&
			cheapest_partial_path != NULL)
		{
			add_partial_path(partially_grouped_rel, (Path *)
							 create_agg_path(root,
											 partially_grouped_rel,
											 cheapest_partial_path,
											 partially_grouped_rel->reltarget,
											 AGG_HASHED,
											 AGGSPLIT_INITIAL_SERIAL,
											 parse->groupClause,
											 NIL,
											 agg_partial_costs,
											 dNumPartialPartialGroups));
		}
	}

	/*
	 * If there is an FDW that's responsible for all baserels of the query,
	 * let it consider adding partially grouped ForeignPaths.
	 */
	if (partially_grouped_rel->fdwroutine &&
		partially_grouped_rel->fdwroutine->GetForeignUpperPaths)
	{
		FdwRoutine *fdwroutine = partially_grouped_rel->fdwroutine;

		fdwroutine->GetForeignUpperPaths(root,
										 UPPERREL_PARTIAL_GROUP_AGG,
										 input_rel, partially_grouped_rel,
										 extra);
	}

	return partially_grouped_rel;
}

/*
 * gather_grouping_paths
 *    为分组关系(grouping relation)或部分分组关系(partial grouping relation)生成
 *    Gather和Gather Merge并行执行路径。
 *
 *    此函数是PostgreSQL并行分组操作优化的核心组件，专门用于为GROUP BY、DISTINCT等分组操作
 *    提供高效的并行执行策略。它不仅利用基本的并行路径生成机制，还考虑了分组键排序的特殊优化。
 *
 *    主要工作流程：
 *    1. 首先调用generate_gather_paths生成基本的并行路径
 *    2. 然后考虑一个特殊优化情况：当成本最低的部分路径未按分组键排序时，添加一个
 *       显式排序+Gather Merge的路径，以支持需要保持分组键顺序的查询场景
 *
 *    注意事项：
 *    - 该函数仅适用于分组关系或部分分组关系，不适用于其他类型的关系
 *    - 函数依赖root->group_pathkeys的存在，这只在分组操作上下文中有意义
 *    - 向generate_gather_paths传递true作为override_rows参数，确保使用正确的行数估计
 */
static void
gather_grouping_paths(PlannerInfo *root, RelOptInfo *rel)
{
    /* 声明成本最低的部分路径指针 */
    Path       *cheapest_partial_path;

    /* 第一阶段：调用generate_gather_paths生成基本的并行路径
     * - 为无序路径生成Gather操作
     * - 为有序路径生成Gather Merge操作
     * - 传递true作为override_rows参数，表示需要覆盖行数估计
     */
    generate_gather_paths(root, rel, true);

    /* 第二阶段：考虑特殊优化情况
     * 尝试构建：成本最低的部分路径 + 显式排序 + Gather Merge的执行路径
     */
    /* 获取成本最低的部分路径（由于路径列表按成本排序，位于列表头部） */
    cheapest_partial_path = linitial(rel->partial_pathlist);
    
    /* 检查成本最低路径是否已经按照分组键排序
     * 如果未排序，我们需要添加一个显式排序操作以保持分组顺序
     */
    if (!pathkeys_contained_in(root->group_pathkeys,
                              cheapest_partial_path->pathkeys))
    {
        /* 声明变量 */
        Path       *path;               /* 构建的路径指针 */
        double      total_groups;       /* 总分组数量估计 */

        /* 计算总分组数量：单工作进程处理的分组数 × 并行工作进程数 */
        total_groups = cheapest_partial_path->rows * cheapest_partial_path->parallel_workers;
        
        /* 创建排序路径，按照分组键进行排序 */
        path = (Path *) create_sort_path(root, rel, cheapest_partial_path,
                                       root->group_pathkeys,  /* 排序键为分组键 */
                                       -1.0);                /* -1.0表示使用默认排序内存 */
        
        /* 在排序路径上创建Gather Merge路径，保持分组键的排序顺序 */
        path = (Path *)
            create_gather_merge_path(root,
                                    rel,           /* 目标关系 */
                                    path,          /* 排序后的路径作为子路径 */
                                    rel->reltarget,  /* 使用关系的目标列表 */
                                    root->group_pathkeys,  /* 保持分组键的排序顺序 */
                                    NULL,          /* 无参数化需求 */
                                    &total_groups);  /* 覆盖分组数量估计 */

        /* 将构建的优化路径添加到关系的路径列表中 */
        add_path(rel, path);
    }
}


/*
 * can_partial_agg
 *
 * Determines whether or not partial grouping and/or aggregation is possible.
 * Returns true when possible, false otherwise.
 */
static bool
can_partial_agg(PlannerInfo *root, const AggClauseCosts *agg_costs)
{
	Query	   *parse = root->parse;

	if (!parse->hasAggs && parse->groupClause == NIL)
	{
		/*
		 * We don't know how to do parallel aggregation unless we have either
		 * some aggregates or a grouping clause.
		 */
		return false;
	}
	else if (parse->groupingSets)
	{
		/* We don't know how to do grouping sets in parallel. */
		return false;
	}
	else if (agg_costs->hasNonPartial || agg_costs->hasNonSerial)
	{
		/* Insufficient support for partial mode. */
		return false;
	}

	/* Everything looks good. */
	return true;
}

/*
 * apply_scanjoin_target_to_paths
 *
 * Adjust the final scan/join relation, and recursively all of its children,
 * to generate the final scan/join target.  It would be more correct to model
 * this as a separate planning step with a new RelOptInfo at the toplevel and
 * for each child relation, but doing it this way is noticeably cheaper.
 * Maybe that problem can be solved at some point, but for now we do this.
 *
 * If tlist_same_exprs is true, then the scan/join target to be applied has
 * the same expressions as the existing reltarget, so we need only insert the
 * appropriate sortgroupref information.  By avoiding the creation of
 * projection paths we save effort both immediately and at plan creation time.
 */
static void
apply_scanjoin_target_to_paths(PlannerInfo *root,
							   RelOptInfo *rel,
							   List *scanjoin_targets,
							   List *scanjoin_targets_contain_srfs,
							   bool scanjoin_target_parallel_safe,
							   bool tlist_same_exprs)
{
	bool		rel_is_partitioned = IS_PARTITIONED_REL(rel);
	PathTarget *scanjoin_target;
	ListCell   *lc;

	/* This recurses, so be paranoid. */
	check_stack_depth();

	/*
	 * If the rel is partitioned, we want to drop its existing paths and
	 * generate new ones.  This function would still be correct if we kept the
	 * existing paths: we'd modify them to generate the correct target above
	 * the partitioning Append, and then they'd compete on cost with paths
	 * generating the target below the Append.  However, in our current cost
	 * model the latter way is always the same or cheaper cost, so modifying
	 * the existing paths would just be useless work.  Moreover, when the cost
	 * is the same, varying roundoff errors might sometimes allow an existing
	 * path to be picked, resulting in undesirable cross-platform plan
	 * variations.  So we drop old paths and thereby force the work to be done
	 * below the Append, except in the case of a non-parallel-safe target.
	 *
	 * Some care is needed, because we have to allow generate_gather_paths to
	 * see the old partial paths in the next stanza.  Hence, zap the main
	 * pathlist here, then allow generate_gather_paths to add path(s) to the
	 * main list, and finally zap the partial pathlist.
	 */
	if (rel_is_partitioned)
		rel->pathlist = NIL;

	/*
	 * If the scan/join target is not parallel-safe, partial paths cannot
	 * generate it.
	 */
	if (!scanjoin_target_parallel_safe)
	{
		/*
		 * Since we can't generate the final scan/join target in parallel
		 * workers, this is our last opportunity to use any partial paths that
		 * exist; so build Gather path(s) that use them and emit whatever the
		 * current reltarget is.  We don't do this in the case where the
		 * target is parallel-safe, since we will be able to generate superior
		 * paths by doing it after the final scan/join target has been
		 * applied.
		 */
		generate_gather_paths(root, rel, false);

		/* Can't use parallel query above this level. */
		rel->partial_pathlist = NIL;
		rel->consider_parallel = false;
	}

	/* Finish dropping old paths for a partitioned rel, per comment above */
	if (rel_is_partitioned)
		rel->partial_pathlist = NIL;

	/* Extract SRF-free scan/join target. */
	scanjoin_target = linitial_node(PathTarget, scanjoin_targets);

	/*
	 * Apply the SRF-free scan/join target to each existing path.
	 *
	 * If the tlist exprs are the same, we can just inject the sortgroupref
	 * information into the existing pathtargets.  Otherwise, replace each
	 * path with a projection path that generates the SRF-free scan/join
	 * target.  This can't change the ordering of paths within rel->pathlist,
	 * so we just modify the list in place.
	 */
	foreach(lc, rel->pathlist)
	{
		Path	   *subpath = (Path *) lfirst(lc);

		/* Shouldn't have any parameterized paths anymore */
		Assert(subpath->param_info == NULL);

		if (tlist_same_exprs)
			subpath->pathtarget->sortgrouprefs =
				scanjoin_target->sortgrouprefs;
		else
		{
			Path	   *newpath;

			newpath = (Path *) create_projection_path(root, rel, subpath,
													  scanjoin_target);
			lfirst(lc) = newpath;
		}
	}

	/* Likewise adjust the targets for any partial paths. */
	foreach(lc, rel->partial_pathlist)
	{
		Path	   *subpath = (Path *) lfirst(lc);

		/* Shouldn't have any parameterized paths anymore */
		Assert(subpath->param_info == NULL);

		if (tlist_same_exprs)
			subpath->pathtarget->sortgrouprefs =
				scanjoin_target->sortgrouprefs;
		else
		{
			Path	   *newpath;

			newpath = (Path *) create_projection_path(root, rel, subpath,
													  scanjoin_target);
			lfirst(lc) = newpath;
		}
	}

	/*
	 * Now, if final scan/join target contains SRFs, insert ProjectSetPath(s)
	 * atop each existing path.  (Note that this function doesn't look at the
	 * cheapest-path fields, which is a good thing because they're bogus right
	 * now.)
	 */
	if (root->parse->hasTargetSRFs)
		adjust_paths_for_srfs(root, rel,
							  scanjoin_targets,
							  scanjoin_targets_contain_srfs);

	/*
	 * Update the rel's target to be the final (with SRFs) scan/join target.
	 * This now matches the actual output of all the paths, and we might get
	 * confused in createplan.c if they don't agree.  We must do this now so
	 * that any append paths made in the next part will use the correct
	 * pathtarget (cf. create_append_path).
	 *
	 * Note that this is also necessary if GetForeignUpperPaths() gets called
	 * on the final scan/join relation or on any of its children, since the
	 * FDW might look at the rel's target to create ForeignPaths.
	 */
	rel->reltarget = llast_node(PathTarget, scanjoin_targets);

	/*
	 * If the relation is partitioned, recursively apply the scan/join target
	 * to all partitions, and generate brand-new Append paths in which the
	 * scan/join target is computed below the Append rather than above it.
	 * Since Append is not projection-capable, that might save a separate
	 * Result node, and it also is important for partitionwise aggregate.
	 */
	if (rel_is_partitioned)
	{
		List	   *live_children = NIL;
		int			partition_idx;

		/* Adjust each partition. */
		for (partition_idx = 0; partition_idx < rel->nparts; partition_idx++)
		{
			RelOptInfo *child_rel = rel->part_rels[partition_idx];
			AppendRelInfo **appinfos;
			int			nappinfos;
			List	   *child_scanjoin_targets = NIL;
			ListCell   *lc;

			/* Pruned or dummy children can be ignored. */
			if (child_rel == NULL || IS_DUMMY_REL(child_rel))
				continue;

			/* Translate scan/join targets for this child. */
			appinfos = find_appinfos_by_relids(root, child_rel->relids,
											   &nappinfos);
			foreach(lc, scanjoin_targets)
			{
				PathTarget *target = lfirst_node(PathTarget, lc);

				target = copy_pathtarget(target);
				target->exprs = (List *)
					adjust_appendrel_attrs(root,
										   (Node *) target->exprs,
										   nappinfos, appinfos);
				child_scanjoin_targets = lappend(child_scanjoin_targets,
												 target);
			}
			pfree(appinfos);

			/* Recursion does the real work. */
			apply_scanjoin_target_to_paths(root, child_rel,
										   child_scanjoin_targets,
										   scanjoin_targets_contain_srfs,
										   scanjoin_target_parallel_safe,
										   tlist_same_exprs);

			/* Save non-dummy children for Append paths. */
			if (!IS_DUMMY_REL(child_rel))
				live_children = lappend(live_children, child_rel);
		}

		/* Build new paths for this relation by appending child paths. */
		add_paths_to_append_rel(root, rel, live_children);
	}

	/*
	 * Consider generating Gather or Gather Merge paths.  We must only do this
	 * if the relation is parallel safe, and we don't do it for child rels to
	 * avoid creating multiple Gather nodes within the same plan. We must do
	 * this after all paths have been generated and before set_cheapest, since
	 * one of the generated paths may turn out to be the cheapest one.
	 */
	if (rel->consider_parallel && !IS_OTHER_REL(rel))
		generate_gather_paths(root, rel, false);

	/*
	 * Reassess which paths are the cheapest, now that we've potentially added
	 * new Gather (or Gather Merge) and/or Append (or MergeAppend) paths to
	 * this relation.
	 */
	set_cheapest(rel);
}

/*
 * create_partitionwise_grouping_paths
 *
 * 如果输入关系的分区键是 GROUP BY 子句的一部分，那么属于给定组的所有行都来自
 * 单个分区。这允许将分区关系上的聚合/分组分解为每个分区上的聚合/分组。
 * 这种方法应该不比普通方法差，而且通常更好。
 *
 * 但是，如果 GROUP BY 子句不包含所有分区键，则给定组的行可能分布在多个分区中。
 * 在这种情况下，我们对每个组执行部分聚合，追加结果，然后完成聚合。
 * 这种情况不如前一种情况确定能获胜。如果 PartialAggregate 阶段大大减少了
 * 组的数量，它可能会获胜，因为通过 Append 节点的行数会更少。
 * 如果我们有很多小组，它可能会失败。
 */
static void
create_partitionwise_grouping_paths(PlannerInfo *root,
									RelOptInfo *input_rel,
									RelOptInfo *grouped_rel,
									RelOptInfo *partially_grouped_rel,
									const AggClauseCosts *agg_costs,
									grouping_sets_data *gd,
									PartitionwiseAggregateType patype,
									GroupPathExtraData *extra)
{
	int			nparts = input_rel->nparts;
	int			cnt_parts;
	List	   *grouped_live_children = NIL;
	List	   *partially_grouped_live_children = NIL;
	PathTarget *target = grouped_rel->reltarget;
	bool		partial_grouping_valid = true;

	Assert(patype != PARTITIONWISE_AGGREGATE_NONE);
	Assert(patype != PARTITIONWISE_AGGREGATE_PARTIAL ||
		   partially_grouped_rel != NULL);

	/* 为分区聚合/分组添加路径。 */
	for (cnt_parts = 0; cnt_parts < nparts; cnt_parts++)
	{
		RelOptInfo *child_input_rel = input_rel->part_rels[cnt_parts];
		PathTarget *child_target = copy_pathtarget(target);
		AppendRelInfo **appinfos;
		int			nappinfos;
		GroupPathExtraData child_extra;
		RelOptInfo *child_grouped_rel;
		RelOptInfo *child_partially_grouped_rel;

		/* 可以忽略被修剪或虚拟的子关系。 */
		if (child_input_rel == NULL || IS_DUMMY_REL(child_input_rel))
			continue;

		/*
		 * 按原样复制给定的 "extra" 结构，然后覆盖特定于此子关系的成员。
		 */
		memcpy(&child_extra, extra, sizeof(child_extra));

		appinfos = find_appinfos_by_relids(root, child_input_rel->relids,
										   &nappinfos);

		child_target->exprs = (List *)
			adjust_appendrel_attrs(root,
								   (Node *) target->exprs,
								   nappinfos, appinfos);

		/* 转换 havingQual 和 targetList。 */
		child_extra.havingQual = (Node *)
			adjust_appendrel_attrs(root,
								   extra->havingQual,
								   nappinfos, appinfos);
		child_extra.targetList = (List *)
			adjust_appendrel_attrs(root,
								   (Node *) extra->targetList,
								   nappinfos, appinfos);

		/*
		 * extra->patype 是为我们的父关系计算的值；patype 是此关系的值。
		 * 对于子关系，我们的值是其父关系的值。
		 */
		child_extra.patype = patype;

		/*
		 * 创建分组关系以保存子关系的完全聚合分组和/或聚合路径。
		 */
		child_grouped_rel = make_grouping_rel(root, child_input_rel,
											  child_target,
											  extra->target_parallel_safe,
											  child_extra.havingQual);

		/* 为此子关系创建分组路径。 */
		create_ordinary_grouping_paths(root, child_input_rel,
									   child_grouped_rel,
									   agg_costs, gd, &child_extra,
									   &child_partially_grouped_rel);

		if (child_partially_grouped_rel)
		{
			partially_grouped_live_children =
				lappend(partially_grouped_live_children,
						child_partially_grouped_rel);
		}
		else
			partial_grouping_valid = false;

		if (patype == PARTITIONWISE_AGGREGATE_FULL)
		{
			set_cheapest(child_grouped_rel);
			grouped_live_children = lappend(grouped_live_children,
											child_grouped_rel);
		}

		pfree(appinfos);
	}

	/*
	 * 尝试为部分分组的子关系创建追加路径。对于完全分区聚合，如果并行聚合是可能的，
	 * 我们可能在 partial_pathlist 中有路径。对于部分分区聚合，我们可能在
	 * pathlist 和 partial_pathlist 中都有路径。
	 *
	 * 注意：我们必须为每个子关系都有一个部分分组路径，以便为此关系生成部分分组路径。
	 */
	if (partially_grouped_rel && partial_grouping_valid)
	{
		Assert(partially_grouped_live_children != NIL);

		add_paths_to_append_rel(root, partially_grouped_rel,
								partially_grouped_live_children);

		/*
		 * 我们需要调用 set_cheapest，因为最终化步骤将使用关系中最便宜的路径。
		 */
		if (partially_grouped_rel->pathlist)
			set_cheapest(partially_grouped_rel);
	}

	/* 如果可能，为完全分组的子关系创建追加路径。 */
	if (patype == PARTITIONWISE_AGGREGATE_FULL)
	{
		Assert(grouped_live_children != NIL);

		add_paths_to_append_rel(root, grouped_rel, grouped_live_children);
	}
}

/*
 * group_by_has_partkey
 *
 * Returns true if all the partition keys of the given relation are part of
 * the GROUP BY clauses, including having matching collation, false otherwise.
 */
static bool
group_by_has_partkey(RelOptInfo *input_rel,
					 List *targetList,
					 List *groupClause)
{
	List	   *groupexprs = get_sortgrouplist_exprs(groupClause, targetList);
	int			cnt = 0;
	int			partnatts;

	/* Input relation should be partitioned. */
	Assert(input_rel->part_scheme);

	/* Rule out early, if there are no partition keys present. */
	if (!input_rel->partexprs)
		return false;

	partnatts = input_rel->part_scheme->partnatts;

	for (cnt = 0; cnt < partnatts; cnt++)
	{
		List	   *partexprs = input_rel->partexprs[cnt];
		ListCell   *lc;
		bool		found = false;

		foreach(lc, partexprs)
		{
			ListCell   *lg;
			Expr	   *partexpr = lfirst(lc);
			Oid			partcoll = input_rel->part_scheme->partcollation[cnt];

			foreach(lg, groupexprs)
			{
				Expr	   *groupexpr = lfirst(lg);
				Oid			groupcoll = exprCollation((Node *) groupexpr);

				/*
				 * Note: we can assume there is at most one RelabelType node;
				 * eval_const_expressions() will have simplified if more than
				 * one.
				 */
				if (IsA(groupexpr, RelabelType))
					groupexpr = ((RelabelType *) groupexpr)->arg;

				if (equal(groupexpr, partexpr))
				{
					/*
					 * Reject a match if the grouping collation does not match
					 * the partitioning collation.
					 */
					if (OidIsValid(partcoll) && OidIsValid(groupcoll) &&
						partcoll != groupcoll)
						return false;

					found = true;
					break;
				}
			}

			if (found)
				break;
		}

		/*
		 * If none of the partition key expressions match with any of the
		 * GROUP BY expression, return false.
		 */
		if (!found)
			return false;
	}

	return true;
}
