/*-------------------------------------------------------------------------
 *
 * prepunion.c
 *	  Routines to plan set-operation queries.  The filename is a leftover
 *	  from a time when only UNIONs were implemented.
 *
 * There are two code paths in the planner for set-operation queries.
 * If a subquery consists entirely of simple UNION ALL operations, it
 * is converted into an "append relation".  Otherwise, it is handled
 * by the general code in this module (plan_set_operations and its
 * subroutines).  There is some support code here for the append-relation
 * case, but most of the heavy lifting for that is done elsewhere,
 * notably in prepjointree.c and allpaths.c.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/prep/prepunion.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/partition.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/tlist.h"
#include "parser/parse_coerce.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"


static RelOptInfo *recurse_set_operations(Node *setOp, PlannerInfo *root,
										  List *colTypes, List *colCollations,
										  bool junkOK,
										  int flag, List *refnames_tlist,
										  List **pTargetList,
										  double *pNumGroups);
static RelOptInfo *generate_recursion_path(SetOperationStmt *setOp,
										   PlannerInfo *root,
										   List *refnames_tlist,
										   List **pTargetList);
static RelOptInfo *generate_union_paths(SetOperationStmt *op, PlannerInfo *root,
										List *refnames_tlist,
										List **pTargetList);
static RelOptInfo *generate_nonunion_paths(SetOperationStmt *op, PlannerInfo *root,
										   List *refnames_tlist,
										   List **pTargetList);
static List *plan_union_children(PlannerInfo *root,
								 SetOperationStmt *top_union,
								 List *refnames_tlist,
								 List **tlist_list);
static Path *make_union_unique(SetOperationStmt *op, Path *path, List *tlist,
							   PlannerInfo *root);
static void postprocess_setop_rel(PlannerInfo *root, RelOptInfo *rel);
static bool choose_hashed_setop(PlannerInfo *root, List *groupClauses,
								Path *input_path,
								double dNumGroups, double dNumOutputRows,
								const char *construct);
static List *generate_setop_tlist(List *colTypes, List *colCollations,
								  int flag,
								  Index varno,
								  bool hack_constants,
								  List *input_tlist,
								  List *refnames_tlist);
static List *generate_append_tlist(List *colTypes, List *colCollations,
								   bool flag,
								   List *input_tlists,
								   List *refnames_tlist);
static List *generate_setop_grouplist(SetOperationStmt *op, List *targetlist);


/*
 * plan_set_operations - 为集合操作树(UNION/INTERSECT/EXCEPT)生成查询计划
 *
 * 功能概述：
 *   此函数专门负责处理SQL查询中的集合操作树，包括UNION、INTERSECT和EXCEPT操作。
 *   它递归地处理集合操作树的各个节点，并生成相应的执行路径。
 *
 * 处理范围：
 *   - 仅处理查询中的setOperations树部分
 *   - 不处理顶级ORDER BY子句（由grouping_planner处理）
 *   - 不处理LIMIT子句（由grouping_planner处理）
 *
 * 参数说明：
 *   root: PlannerInfo结构体指针，包含查询的所有规划信息
 *
 * 返回值：
 *   返回一个"upperrel"类型的RelOptInfo结构体，其中包含至少一个实现集合操作树的Path
 *   同时，root->processed_tlist将被设置为表示最顶层setop节点输出的目标列表
 */
RelOptInfo *
plan_set_operations(PlannerInfo *root)
{
	Query		   		*parse = root->parse;               /* 从规划器信息中获取查询解析树 */
	SetOperationStmt 	*topop = castNode(SetOperationStmt, parse->setOperations); /* 顶层集合操作节点 */
	Node		   		*node;                             	/* 用于遍历集合操作树的临时节点指针 */
	RangeTblEntry 		*leftmostRTE;                      	/* 最左侧子查询的范围表条目 */
	Query		   		*leftmostQuery;                     /* 最左侧子查询的查询结构 */
	RelOptInfo	 		*setop_rel;                        	/* 用于存储生成的集合操作关系结构 */
	List		   		*top_tlist;                         /* 顶层目标列表 */

	/* 断言：确保顶层集合操作节点存在 */
	Assert(topop);

	/* 检查是否存在不支持的查询结构 */
	Assert(parse->jointree->fromlist == NIL);       /* 确保没有FROM子句 */
	Assert(parse->jointree->quals == NULL);         /* 确保没有WHERE子句 */
	Assert(parse->groupClause == NIL);              /* 确保没有GROUP BY子句 */
	Assert(parse->havingQual == NULL);              /* 确保没有HAVING子句 */
	Assert(parse->windowClause == NIL);             /* 确保没有窗口函数子句 */
	Assert(parse->distinctClause == NIL);           /* 确保没有DISTINCT子句 */

	/*
	 * Find the leftmost component query.  We need to use its column names
	 * for all the targetlists (else SELECT INTO won't work right).
	 *
	 * We do this before setup_simple_rel_arrays so that we can propagate
	 * hints before any planning happens.
	 */
	node = topop->larg;                             /* 从顶层集合操作的左侧开始 */
	while (node && IsA(node, SetOperationStmt))     /* 如果当前节点仍然是集合操作，则继续向左遍历 */
		node = ((SetOperationStmt *) node)->larg;
	Assert(node && IsA(node, RangeTblRef));         /* 断言找到的节点是RangeTblRef类型 */

	leftmostRTE = rt_fetch(((RangeTblRef*)node)->rtindex, parse->rtable); /* 获取最左侧RTE */
	leftmostQuery = leftmostRTE->subquery;          /* 获取最左侧子查询 */
	Assert(leftmostQuery != NULL);                  /* 断言子查询存在 */

	/*
	 * 为每个叶级子查询（它们是此查询中的RTE_SUBQUERY类型的范围表条目）
	 * 构建RelOptInfos结构。为此，我们需要准备索引数组。
	 */
	setup_simple_rel_arrays(root);

	/*
	 * 填充append_rel_array数组，存储每个AppendRelInfo，以便通过子关系ID直接查找
	 */
	setup_append_rel_array(root);

	/*
	 * 如果顶层节点是递归UNION，需要特殊处理
	 */
	if (root->hasRecursion)
	{
		/* 生成递归路径 */
		setop_rel = generate_recursion_path(topop, root,
								leftmostQuery->targetList,
								&top_tlist);
	}
	else
	{
		/*
		 * 递归处理集合操作树，为所有集合操作生成路径。
		 * 最终输出路径应仅包含顶层节点输出的列类型，
		 * 以及可能的resjunk工作列（我们可以依赖上层节点处理这些）。
		 */
		setop_rel = recurse_set_operations((Node *) topop, root,
								topop->colTypes, topop->colCollations,
								true, -1,
								leftmostQuery->targetList,
								&top_tlist,
								NULL);
	}

	/* 必须将构建的目标列表返回给root->processed_tlist */
	root->processed_tlist = top_tlist;

	/* 返回生成的集合操作关系结构 */
	return setop_rel;
}


/*
 * recurse_set_operations
 *    递归处理集合操作树中的一个步骤
 *
 * 参数说明：
 *   setOp: 当前处理的集合操作节点（可能是RangeTblRef或SetOperationStmt）
 *   root: 包含查询优化相关信息的PlannerInfo结构体
 *   colTypes: 集合操作结果列的数据类型OID列表
 *   colCollations: 集合操作结果列的排序规则OID列表
 *   junkOK: 如果为true，允许结果中保留resjunk类型的列
 *   flag: 如果>=0，添加一个resjunk类型的输出列来标记flag值
 *   refnames_tlist: 用于获取列名的目标列表
 *   pTargetList: 输出参数，接收子树顶层计划的完整目标列表
 *   pNumGroups: 如果非NULL，估计结果中不同组的数量并存储
 *
 * 返回值：
 *   返回子树对应的RelOptInfo结构体，包含生成的执行路径信息
 */
static RelOptInfo *
recurse_set_operations(Node *setOp, PlannerInfo *root,
			   List *colTypes, List *colCollations,
			   bool junkOK, int flag,
			   List *refnames_tlist,
			   List **pTargetList,
			   double *pNumGroups)
{
	RelOptInfo *rel = NULL;  /* 初始化返回值，避免编译器警告 */

	/* 防止由于过于复杂的集合操作嵌套导致栈溢出 */
	check_stack_depth();

	/* 处理叶子节点：RangeTblRef，表示集合操作树中的子查询引用 */
	if (IsA(setOp, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) setOp;
		RangeTblEntry *rte = root->simple_rte_array[rtr->rtindex];
		Query	   *subquery = rte->subquery;
		PlannerInfo *subroot;
		RelOptInfo *final_rel;
		Path	   *subpath;
		Path	   *path;
		List	   *tlist;

		Assert(subquery != NULL); /* 确保是子查询引用 */

		/* 为这个叶子子查询构建RelOptInfo */
		rel = build_simple_rel(root, rtr->rtindex, NULL);

		/* 确保当前查询级别中plan_params未被使用 */
		Assert(root->plan_params == NIL);

		/* 为子查询调用优化器，生成该子查询的执行计划 */
		subroot = rel->subroot = subquery_planner(root->glob, subquery,
						  root,
						  false,
						  root->tuple_fraction);

		/*
		 * 检查子查询是否有对集合操作树中其他原始查询的跨引用
		 * 这应该是不可能的，如果存在则报错
		 *
		 * 在 PostgreSQL 优化器中，plan_params 列表用于收集当前查询层级中引用的外部参数（Outer References）。
		 * 例如：SELECT * FROM t1 WHERE t1.id = (SELECT t2.id FROM t2 WHERE t2.x = t1.x);
		 * 在这个例子中，子查询引用了外部查询的 t1.x，这会导致 plan_params 非空。
		 * 但是，在集合操作的子查询中，不允许这种跨引用，因为集合操作的语义要求各个子查询独立。
		 */
		if (root->plan_params)
			elog(ERROR, "unexpected outer reference in set operation subquery");

		/*
		 * 标准化子查询的输出列，使其符合集合操作的要求
		 * 为子查询确定适当的目标列表
		 *
		 * 输入参数:
		 * colTypes, colCollations: 集合操作要求的列类型和排序规则（由最左侧查询决定）。
		 * subroot->processed_tlist: 当前子查询原本计划输出的列。
		 *
		 * 功能:
		 * 类型转换: 如果子查询输出 int，但集合操作要求 bigint，这里会插入一个类型转换表达式。
		 * 列对齐: 确保第 N 列对应第 N 列。
		 * 添加标识列: 如果 flag 参数有效（通常用于 INTERSECT/EXCEPT），它会添加一个隐藏的“标识列”（flag column），
		 * 用于在执行阶段区分这一行数据来自哪个分支（左边还是右边）。
		 */
		tlist = generate_setop_tlist(colTypes, colCollations,
						 flag,
						 rtr->rtindex,
						 true,
						 subroot->processed_tlist,
						 refnames_tlist);
		rel->reltarget = create_pathtarget(root, tlist);

		/* 将完整的目标列表返回给调用者 */
		*pTargetList = tlist;

		/*
		 * 为rel标记估计的输出行数、宽度等信息
		 * 注意：必须在生成外层查询路径前执行此操作，否则cost_subqueryscan会出错
		 */
		set_subquery_size_estimates(root, rel);

		/*
		 * 由于可能需要为该关系添加部分路径，必须正确设置consider_parallel标志
		 * 将子查询的并行执行能力“传递”给上层的集合操作节点。
		 * 这是为了支持 并行集合操作，比如 Parallel Append。
		 */
		final_rel = fetch_upper_rel(subroot, UPPERREL_FINAL, NULL);
		rel->consider_parallel = final_rel->consider_parallel;

		/*
		 * 从子查询中选择一条“最优”路径，作为集合操作（如 UNION）的一个输入分支。
		 * 暂时只为子查询考虑单个路径
		 * 这部分未来可能会更改（使其更像set_subquery_pathlist）
		 */
		subpath = get_cheapest_fractional_path(final_rel,
						   root->tuple_fraction);

		/*
		 * 在子路径上添加SubqueryScanPath
		 *
		 * 由于子查询的输出排序在集合操作结果中不会被保留，
		 * 所以简单地将SubqueryScanPath标记为nil pathkeys
		 * 通常集合操作（如 UNION）会打乱顺序，或者上层并不关心子查询的原始顺序。
		 * （XXX这部分将来也可能更改）
		 *
		 * Append  (对应 UNION ALL)
  		 *   ->  Subquery Scan on "*SELECT* 1"  <-- 这就是 create_subqueryscan_path 创建的节点
		 *   		->  Seq Scan on t_users_a   <-- 这就是 subpath
  		 *   ->  Subquery Scan on "*SELECT* 2"
		 *         ->  Seq Scan on t_users_b    <-- 这就是 subpath
		 */
		path = (Path *) create_subqueryscan_path(root, rel, subpath,
						 NIL, NULL);

		add_path(rel, path);

		/*
		 * 为集合操作（如 UNION）构建并行的子查询扫描路径。
		 * 如果子关系有部分路径，可以用它来构建该关系的部分路径
		 * 但只考虑最便宜的路径
		 *
		 * 条件：
		 * - rel->consider_parallel：当前关系（集合操作节点）被标记为“可以考虑并行”
		 * - bms_is_empty(rel->lateral_relids)：当前关系没有LATERAL 依赖
		 * - final_rel->partial_pathlist != NIL：子查询的最终关系有部分路径
		 */
		if (rel->consider_parallel && bms_is_empty(rel->lateral_relids) &&
			final_rel->partial_pathlist != NIL)
		{
			Path	   *partial_subpath;
			Path	   *partial_path;

			partial_subpath = linitial(final_rel->partial_pathlist);
			partial_path = (Path *)
				create_subqueryscan_path(root, rel, partial_subpath,
							 NIL, NULL);
			add_partial_path(rel, partial_path);
		}

		/*
		 * 如果调用者需要，估计组的数量
		 * 如果子查询使用了分组或聚合，其输出可能已经大部分是唯一的；
		 * 否则进行统计估计
		 */
		if (pNumGroups)
		{
			if (subquery->groupClause || subquery->groupingSets ||
				subquery->distinctClause ||
				subroot->hasHavingQual || subquery->hasAggs)
				*pNumGroups = subpath->rows;
			else
				*pNumGroups = estimate_num_groups(subroot,
							  get_tlist_exprs(subquery->targetList, false),
							  subpath->rows,
							  NULL);
		}
	}
	/* 处理内部节点：SetOperationStmt，表示集合操作 */
	else if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;

		/*
		 * UNION与INTERSECT/EXCEPT的处理方式有很大不同
		 * 例如：
		 * -- 简单的 UNION 操作
		 * 	SELECT user_id, username FROM active_users
		 * 	UNION
		 * 	SELECT user_id, username FROM archived_users;
		 * 优化器会调用 generate_union_paths。
		 * 通常这会生成一个 Append 路径（如果是 UNION ALL）或者 Append 后跟 Unique / Aggregate 路径（如果是 UNION 去重）。
		 *
		 * 当 SQL 中使用 INTERSECT (交集) 或 EXCEPT (差集) 时，会进入 else 分支。
		 * 例如：
		 * -- EXCEPT 操作（找出在 active_users 但不在 vip_users 中的人）
		 * 	SELECT user_id FROM active_users
		 * 	EXCEPT
		 * 	SELECT user_id FROM vip_users;
		 * 优化器会调用 generate_nonunion_paths。
		 * 这通常比 UNION 复杂，可能需要对两个数据集进行排序（Sort）然后合并，或者使用哈希（Hash）算法来计算差集或交集。
		 */
		if (op->op == SETOP_UNION)
			rel = generate_union_paths(op, root,
						   refnames_tlist,
						   pTargetList);
		else
			rel = generate_nonunion_paths(op, root,
						  refnames_tlist,
						  pTargetList);
		if (pNumGroups)
			*pNumGroups = rel->rows;

		/*
		 * 如有必要，添加Result节点来投影调用者请求的输出列
		 * 用于处理类型不匹配或需要类型转换的情况。
		 * 例如：
		 * CREATE TABLE t_int (a int);
		 * CREATE TABLE t_float (b float);
		 *
		 * -- int 和 float 进行 UNION，结果类型会统一为 float
		 * SELECT a FROM t_int
		 * UNION
		 * SELECT b FROM t_float;
		 *
		 * 
		 */
		if (flag >= 0 ||
			!tlist_same_datatypes(*pTargetList, colTypes, junkOK) ||
			!tlist_same_collations(*pTargetList, colCollations, junkOK))
		{
			PathTarget *target;
			ListCell   *lc;

			/*
			 * 生成新的目标列表，使用varno 0
			 * 当发现子路径输出的数据类型、排序规则与上层要求不一致时，
			 * 需要准备一个“模具”（即 TargetList），用来生成一个 Result 节点（投影节点）进行格式转换。
			 */
			*pTargetList = generate_setop_tlist(colTypes, colCollations,
						 flag,
						 0,
						 false,
						 *pTargetList,
						 refnames_tlist);
			target = create_pathtarget(root, *pTargetList);

			/*
			 * 对每个路径应用投影
			 * 于是执行 apply_projection_to_path，在路径上方添加一个 Result 节点，负责将 int 转换为 float 类型
			 */
			foreach(lc, rel->pathlist)
			{
				Path	   *subpath = (Path *) lfirst(lc);
				Path	   *path;

				Assert(subpath->param_info == NULL);
				path = apply_projection_to_path(root, subpath->parent,
							subpath, target);
				/* 如果添加了Result，则path与subpath不同 */
				if (path != subpath)
					lfirst(lc) = path;
			}

			/*
			 * 对每个部分路径应用投影
			 * Gather  (收集节点)
  			 * Output: bigint  <-- 主进程接收到的是 bigint
  			 * Workers Planned: 2
  			 * ->  Result  (投影节点，对应 create_projection_path)
			 * 		 Output: bigint  <-- 在这里将 int 转为 bigint
			 * 		 ->  Parallel Seq Scan on t_huge_int
			 * 		 	  Output: int  <-- 原始扫描输出 int
			 */
			foreach(lc, rel->partial_pathlist)
			{
				Path	   *subpath = (Path *) lfirst(lc);
				Path	   *path;

				Assert(subpath->param_info == NULL);

				/* 避免使用apply_projection_to_path，以防多次引用 */
				path = (Path *) create_projection_path(root, subpath->parent,
							   subpath, target);
				lfirst(lc) = path;
			}
		}
	}
	/* 处理未识别的节点类型 */
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(setOp));
		*pTargetList = NIL;
	}

	/* 对集合操作关系进行后处理 */
	postprocess_setop_rel(root, rel);

	return rel;
}


/*
 * 为递归 UNION 节点生成查询路径
 *
 * 参数说明：
 *   setOp: 集合操作语句节点，包含 UNION 操作的所有信息
 *   root: 包含查询优化相关信息的 PlannerInfo 结构体
 *   refnames_tlist: 引用名称的目标列表
 *   pTargetList: 输出参数，用于存储生成的目标列表
 *
 * 返回值：
 *   返回包含递归 UNION 路径的 RelOptInfo 结构体
 */
static RelOptInfo *
generate_recursion_path(SetOperationStmt *setOp, PlannerInfo *root,
					List *refnames_tlist,
					List **pTargetList)
{
	/* 局部变量声明 */
	RelOptInfo *result_rel;  /* 结果关系表信息 */
	Path	   *path;        /* 生成的执行路径 */
	RelOptInfo *lrel,        /* 左输入的关系表信息 */
		       *rrel;        /* 右输入的关系表信息 */
	Path	   *lpath;       /* 左输入的最优路径 */
	Path	   *rpath;       /* 右输入的最优路径 */
	List	   *lpath_tlist; /* 左输入路径的目标列表 */
	List	   *rpath_tlist; /* 右输入路径的目标列表 */
	List	   *tlist;       /* 结果路径的目标列表 */
	List	   *groupList;   /* 用于去重的分组列表 */
	double	   dNumGroups;   /* 预估的不同组数量 */

	/* 解析器应该已经拒绝了其他类型的递归操作 */
	if (setOp->op != SETOP_UNION)
		elog(ERROR, "only UNION queries can be recursive");
	/* 递归查询必须有工作表ID */
	Assert(root->wt_param_id >= 0);

	/*
	 * 与普通 UNION 不同，递归 UNION 需要分别处理左右输入，
	 * 而不是将它们合并到一个 Append 节点中
	 */
	/* 处理非递归部分（左输入） */
	lrel = recurse_set_operations(setOp->larg, root,
					  setOp->colTypes, setOp->colCollations,
					  false, -1,
					  refnames_tlist,
					  &lpath_tlist,
					  NULL);
	lpath = lrel->cheapest_total_path;
	/* 保存非递归路径，供递归部分参考 */
	root->non_recursive_path = lpath;
	/* 处理递归部分（右输入） */
	rrel = recurse_set_operations(setOp->rarg, root,
					  setOp->colTypes, setOp->colCollations,
					  false, -1,
					  refnames_tlist,
					  &rpath_tlist,
					  NULL);
	rpath = rrel->cheapest_total_path;
	/* 清理非递归路径引用 */
	root->non_recursive_path = NULL;

	/*
	 * 为 RecursiveUnion 路径节点生成目标列表
	 * 使用与 Append 节点相同的方法
	 */
	tlist = generate_append_tlist(setOp->colTypes, setOp->colCollations, false,
					  list_make2(lpath_tlist, rpath_tlist),
					  refnames_tlist);

	/* 输出目标列表 */
	*pTargetList = tlist;

	/* 构建结果关系表 */
	result_rel = fetch_upper_rel(root, UPPERREL_SETOP,
					 bms_union(lrel->relids, rrel->relids));
	result_rel->reltarget = create_pathtarget(root, tlist);

	/*
	 * 如果是 UNION（非 ALL 变体），需要标识分组操作符
	 */
	if (setOp->all) /* UNION ALL 不需要去重 */
	{
		groupList = NIL;  /* 无需分组 */
		dNumGroups = 0;   /* 组数量为0 */
	}
	else /* UNION 需要去重 */
	{
		/* 标识分组语义，用于去重操作 */
		groupList = generate_setop_grouplist(setOp, tlist);

		/* 递归 UNION 仅支持基于哈希的去重 */
		if (!grouping_is_hashable(groupList))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("could not implement recursive UNION"),
					 errdetail("All column datatypes must be hashable.")));

		/*
		 * 暂时估计不同组的数量等于总输入大小，即最坏情况
		 * 递归部分的行数乘以10作为保守估计，因为递归查询可能产生大量重复
		 */
		dNumGroups = lpath->rows + rpath->rows * 10;
	}

	/*
	 * 创建递归 UNION 路径节点
	 */
	path = (Path *) create_recursiveunion_path(root,
					   result_rel,
					   lpath,
					   rpath,
					   result_rel->reltarget,
					   groupList,
					   root->wt_param_id,
					   dNumGroups);

	/* 将生成的路径添加到结果关系表中 */
	add_path(result_rel, path);
	/* 对集合操作关系表进行后处理 */
	postprocess_setop_rel(root, result_rel);
	return result_rel;
}

/*
 * 为 UNION 或 UNION ALL 节点生成查询执行路径
 *
 * 参数说明：
 *   op: SetOperationStmt 结构体，包含 UNION 操作的详细信息
 *   root: 包含查询优化相关信息的 PlannerInfo 结构体
 *   refnames_tlist: 用于获取列名的目标列表
 *   pTargetList: 输出参数，接收生成的完整目标列表
 *
 * 返回值：
 *   返回包含 UNION 执行路径的 RelOptInfo 结构体
 */
static RelOptInfo *
generate_union_paths(SetOperationStmt *op, PlannerInfo *root,
				 List *refnames_tlist,
				 List **pTargetList)
{
	Relids		relids = NULL;           /* 参与操作的关系表ID集合 */
	RelOptInfo *result_rel;            /* 结果关系表信息 */
	double		save_fraction = root->tuple_fraction;  /* 保存原始元组分数 */
	ListCell   *lc;                    /* 列表遍历指针 */
	List	   *pathlist = NIL;         /* 普通路径列表 */
	List	   *partial_pathlist = NIL; /* 并行部分路径列表 */
	bool		partial_paths_valid = true; /* 并行路径是否有效 */
	bool		consider_parallel = true;   /* 是否考虑并行执行 */
	List	   *rellist;                /* 子关系表列表 */
	List	   *tlist_list;             /* 子目标列表的列表 */
	List	   *tlist;                  /* 结果目标列表 */
	Path	   *path;                   /* 生成的执行路径 */

	/*
	 * 对于普通 UNION（非 ALL 变体），设置子查询获取所有元组
	 *
	 * 注意：在 UNION ALL 情况下，我们将顶层的 tuple_fraction 原样传递给每个子查询
	 * 虽然可以考虑为后面的子查询减少元组分数（根据前面子查询结果的预期大小进行折扣），
	 * 但似乎不值得这样做。tuple_fraction 不为零的正常情况是顶层有 LIMIT，
	 * 原样传递它通常足以获得优先选择快速启动计划的预期结果。
	 */
	if (!op->all)
		root->tuple_fraction = 0.0;

	/*
	 * 如果任何子节点是具有相同操作类型、all标志和列类型的UNION节点，
	 * 则可以将它们合并到此节点中，以便我们只为所有这些节点生成一个Append和去重操作。
	 * 递归查找此类节点并计算其子节点的路径。
	 */
	rellist = plan_union_children(root, op, refnames_tlist, &tlist_list);

	/*
	 * 为Append计划节点生成目标列表
	 *
	 * 对Append计划本身而言，目标列表并不重要，但为了上一级计划的需要，必须使其看起来真实
	 */
	tlist = generate_append_tlist(op->colTypes, op->colCollations, false,
					  tlist_list, refnames_tlist);

	/* 输出目标列表 */
	*pTargetList = tlist;

	/* 构建路径列表和关系ID集合 */
	foreach(lc, rellist)
	{
		RelOptInfo *rel = lfirst(lc);

		/* 添加最便宜的完整路径 */
		pathlist = lappend(pathlist, rel->cheapest_total_path);

		/* 收集并行执行相关信息 */
		if (consider_parallel)
		{
			if (!rel->consider_parallel)
			{
				consider_parallel = false;
				partial_paths_valid = false;
			}
			else if (rel->partial_pathlist == NIL)
				partial_paths_valid = false;
			else
				partial_pathlist = lappend(partial_pathlist,
							   linitial(rel->partial_pathlist));
		}

		/* 合并关系ID */
		relids = bms_union(relids, rel->relids);
	}

	/* 构建结果关系表 */
	result_rel = fetch_upper_rel(root, UPPERREL_SETOP, relids);
	result_rel->reltarget = create_pathtarget(root, tlist);
	result_rel->consider_parallel = consider_parallel;

	/*
	 * 创建Append节点将子结果合并
	 */
	path = (Path *) create_append_path(root, result_rel, pathlist, NIL,
					   NIL, NULL, 0, false, NIL, -1);

	/*
	 * 对于UNION ALL，只需要Append路径即可
	 * 对于普通UNION，需要添加去重节点
	 */
	if (!op->all)
		path = make_union_unique(op, path, tlist, root);

	/* 将生成的路径添加到结果关系表中 */
	add_path(result_rel, path);

	/*
	 * 估计组数
	 * 目前我们假设输出是唯一的——对于UNION情况这肯定是正确的，
	 * 而且无论如何我们都希望最坏情况的估计
	 */
	result_rel->rows = path->rows;

	/*
	 * 现在考虑使用并行路径（partial paths）、Append和Gather的组合方式
	 */
	if (partial_paths_valid)
	{
		Path	   *ppath;           /* 并行执行路径 */
		int		parallel_workers = 0; /* 并行工作线程数 */

		/* 找出所有子路径中请求的最大工作线程数 */
		foreach(lc, partial_pathlist)
		{
			Path	   *path = lfirst(lc);

			parallel_workers = Max(parallel_workers, path->parallel_workers);
		}
		Assert(parallel_workers > 0);

		/*
		 * 如果允许使用并行Append，总是请求至少log2(子节点数量)个工作线程
		 * 我们假设在这种情况下有额外的工作线程是有用的，因为它们会分散在多个子节点上
		 * 精确的公式只是一个猜测；详见add_paths_to_append_rel
		 */
		if (enable_parallel_append)
		{
			parallel_workers = Max(parallel_workers,
						   fls(list_length(partial_pathlist)));
			parallel_workers = Min(parallel_workers,
						   max_parallel_workers_per_gather);
		}
		Assert(parallel_workers > 0);

		/* 创建并行Append路径 */
		ppath = (Path *)
			create_append_path(root, result_rel, NIL, partial_pathlist,
					   NIL, NULL,
					   parallel_workers, enable_parallel_append,
					   NIL, -1);
		/* 在Append路径上添加Gather节点 */
		ppath = (Path *)
			create_gather_path(root, result_rel, ppath,
					   result_rel->reltarget, NULL, NULL);
		/* 对于普通UNION，添加去重节点 */
		if (!op->all)
			ppath = make_union_unique(op, ppath, tlist, root);
		add_path(result_rel, ppath);
	}

	/* 恢复原始的元组分数设置 */
	root->tuple_fraction = save_fraction;

	return result_rel;
}


/*
 * Generate paths for an INTERSECT, INTERSECT ALL, EXCEPT, or EXCEPT ALL node
 * 为INTERSECT、INTERSECT ALL、EXCEPT或EXCEPT ALL节点生成执行路径
 * 
 * 参数说明：
 * - op: 集合操作语句节点，包含操作类型、左右参数、类型信息等
 * - root: 规划器信息，包含整个查询的优化上下文
 * - refnames_tlist: 引用名称的目标列表，用于生成列引用
 * - pTargetList: 输出参数，用于存储生成的目标列表
 * 
 * 返回值：
 * - 代表集合操作结果的关系优化信息结构
 */
static RelOptInfo *
generate_nonunion_paths(SetOperationStmt *op, PlannerInfo *root,
				List *refnames_tlist,
				List **pTargetList)
{
	RelOptInfo *result_rel;       /* 最终结果关系 */
	RelOptInfo *lrel, *rrel;      /* 左右子操作的关系 */
	double save_fraction = root->tuple_fraction; /* 保存当前的元组分数 */
	Path *lpath, *rpath, *path;   /* 路径节点 */
	List *lpath_tlist,            /* 左路径的目标列表 */
	     *rpath_tlist,            /* 右路径的目标列表 */
	     *tlist_list,             /* 目标列表的列表 */
	     *tlist,                  /* 最终目标列表 */
	     *groupList,              /* 分组列表，用于去重 */
	     *pathlist;               /* 路径列表 */
	double dLeftGroups,           /* 左输入估计的不同组数量 */
	       dRightGroups,          /* 右输入估计的不同组数量 */
	       dNumGroups,            /* 估计需要哈希表条目的组数 */
	       dNumOutputRows;        /* 估计的输出行数 */
	bool use_hash;                /* 是否使用哈希方法 */
	SetOpCmd cmd;                 /* 集合操作命令类型 */
	int firstFlag;                /* 第一个输入的标志，用于区分左右操作数 */

	/*
	 * Tell children to fetch all tuples.
	 * 告诉子操作获取所有元组，因为集合操作需要处理完整输入
	 */
	root->tuple_fraction = 0.0;

	/* Recurse on children, ensuring their outputs are marked */
	/* 递归处理左子操作，获取其关系信息、目标列表和组数量 */
	lrel = recurse_set_operations(op->larg, root,
					  op->colTypes, op->colCollations,
					  false, 0,
					  refnames_tlist,
					  &lpath_tlist,
					  &dLeftGroups);
	lpath = lrel->cheapest_total_path; /* 获取左子操作的最便宜总成本路径 */
	
	/* 递归处理右子操作，获取其关系信息、目标列表和组数量 */
	rrel = recurse_set_operations(op->rarg, root,
					  op->colTypes, op->colCollations,
					  false, 1,
					  refnames_tlist,
					  &rpath_tlist,
					  &dRightGroups);
	rpath = rrel->cheapest_total_path; /* 获取右子操作的最便宜总成本路径 */

	/* Undo effects of forcing tuple_fraction to 0 */
	/* 恢复原始的元组分数设置 */
	root->tuple_fraction = save_fraction;

	/*
	 * For EXCEPT, we must put the left input first.  For INTERSECT,
	 * either order should give the same results, and we prefer to put the
	 * smaller input first in order to minimize the size of the hash table
	 * in the hashing case.  "Smaller" means the one with the fewer groups.
	 * 
	 * 对于EXCEPT操作，必须保持左输入在前
	 * 对于INTERSECT操作，输入顺序不影响结果，因此选择较小的输入在前
	 * 以最小化哈希表的大小（"较小"指组数量较少）
	 */
	if (op->op == SETOP_EXCEPT || dLeftGroups <= dRightGroups)
	{
		pathlist = list_make2(lpath, rpath);      /* 左输入在前 */
		tlist_list = list_make2(lpath_tlist, rpath_tlist);
		firstFlag = 0;  /* 第一个是左输入 */
	}
	else
	{
		pathlist = list_make2(rpath, lpath);      /* 右输入在前 */
		tlist_list = list_make2(rpath_tlist, lpath_tlist);
		firstFlag = 1;  /* 第一个是右输入 */
	}

	/*
	 * Generate tlist for Append plan node.
	 * 
	 * 为Append计划节点生成目标列表
	 * Append计划的目标列表对Append本身不重要，但为了上层计划的正确性必须构建真实的结构
	 * 特别是必须将标志列表示为变量而非常量，否则setrefs.c会混淆
	 */
	tlist = generate_append_tlist(op->colTypes, op->colCollations, true,
					  tlist_list, refnames_tlist);

	*pTargetList = tlist;  /* 通过输出参数返回目标列表 */

	/* Build result relation. */
	/* 构建表示结果的关系结构 */
	result_rel = fetch_upper_rel(root, UPPERREL_SETOP,
					   bms_union(lrel->relids, rrel->relids));
	result_rel->reltarget = create_pathtarget(root, tlist); /* 设置关系的目标 */

	/*
	 * Append the child results together.
	 * 创建Append路径，将左右子操作的结果合并
	 */
	path = (Path *) create_append_path(root, result_rel, pathlist, NIL,
				   NIL, NULL, 0, false, NIL, -1);

	/* Identify the grouping semantics */
	/* 生成用于集合操作分组/去重的排序列表 */
	groupList = generate_setop_grouplist(op, tlist);

	/*
	 * Estimate number of distinct groups that we'll need hashtable entries
	 * for; this is the size of the left-hand input for EXCEPT, or the smaller
	 * input for INTERSECT.  Also estimate the number of eventual output rows.
	 * In non-ALL cases, we estimate each group produces one output row; in
	 * ALL cases use the relevant relation size.  These are worst-case
	 * estimates, of course, but we need to be conservative.
	 * 
	 * 估计需要哈希表条目的不同组数量：
	 * - 对于EXCEPT操作，这等于左输入的组数量
	 * - 对于INTERSECT操作，这等于较小输入的组数量
	 * 同时估计最终输出的行数：
	 * - 非ALL情况下，每个组产生一行输出
	 * - ALL情况下，使用相关输入的大小
	 * 这些是最坏情况的估计，但我们需要保持保守
	 */
	if (op->op == SETOP_EXCEPT)
	{
		dNumGroups = dLeftGroups;
		dNumOutputRows = op->all ? lpath->rows : dNumGroups;
	}
	else
	{
		dNumGroups = Min(dLeftGroups, dRightGroups);
		dNumOutputRows = op->all ? Min(lpath->rows, rpath->rows) : dNumGroups;
	}

	/*
	 * Decide whether to hash or sort, and add a sort node if needed.
	 * 决定使用哈希还是排序方法，并在需要时添加排序节点
	 */
	use_hash = choose_hashed_setop(root, groupList, path,
				   dNumGroups, dNumOutputRows,
				   (op->op == SETOP_INTERSECT) ? "INTERSECT" : "EXCEPT");

	if (groupList && !use_hash)  /* 如果有分组列表且不使用哈希，则添加排序节点 */
		path = (Path *) create_sort_path(root,
					 result_rel,
					 path,
					 make_pathkeys_for_sortclauses(root,
									   groupList,
									   tlist),
					 -1.0);

	/*
	 * Finally, add a SetOp path node to generate the correct output.
	 * 最后，添加SetOp路径节点以生成正确的集合操作输出
	 */
	switch (op->op)
	{
		case SETOP_INTERSECT:
			cmd = op->all ? SETOPCMD_INTERSECT_ALL : SETOPCMD_INTERSECT;
			break;
		case SETOP_EXCEPT:
			cmd = op->all ? SETOPCMD_EXCEPT_ALL : SETOPCMD_EXCEPT;
			break;
		default:
			elog(ERROR, "unrecognized set op: %d", (int) op->op);
			cmd = SETOPCMD_INTERSECT; /* 保持编译器安静 */
			break;
	}
	/* 创建SetOp路径节点 */
	path = (Path *) create_setop_path(root,
				  result_rel,
				  path,
				  cmd,
				  use_hash ? SETOP_HASHED : SETOP_SORTED,
				  groupList,
				  list_length(op->colTypes) + 1,
				  use_hash ? firstFlag : -1,
				  dNumGroups,
				  dNumOutputRows);

	result_rel->rows = path->rows; /* 设置结果关系的行数估计 */
	add_path(result_rel, path);    /* 将生成的路径添加到结果关系中 */
	return result_rel;             /* 返回结果关系 */
}

/*
 * 拉升具有相同属性的 UNION 节点的子节点。
 *
 * 注意：我们也可以将 UNION ALL 拉升到 UNION 中，因为无论如何最终都会丢弃重复的输出行。
 *
 * 注意：目前在判断子节点属性是否一致时，我们忽略了排序规则（collations）。
 * 这在语义上是合理的，只要所有排序规则对等价的定义一致。
 * 从实现角度看也是有效的，因为我们不关心 UNION 子节点结果的排序：
 * UNION ALL 的结果总是无序的，而 generate_union_paths 会在顶层为 UNION 强制重新排序。
 */
static List *
plan_union_children(PlannerInfo *root,
                   SetOperationStmt *top_union,
                   List *refnames_tlist,
                   List **tlist_list)
{
    // 创建待处理节点列表，初始包含顶层UNION节点
    List       *pending_rels = list_make1(top_union);
    // 初始化结果列表为空
    List       *result = NIL;
    // 用于存储单个子节点目标列表的临时变量
    List       *child_tlist;

    // 初始化输出参数tlist_list为空列表
    *tlist_list = NIL;

    // 迭代处理待处理节点列表，直到所有节点都被处理
    while (pending_rels != NIL)
    {
        // 获取列表中的第一个节点
        Node       *setOp = linitial(pending_rels);
        
        // 从待处理列表中移除已取出的节点
        pending_rels = list_delete_first(pending_rels);

        // 检查当前节点是否为SetOperationStmt类型（即集合操作节点）
        if (IsA(setOp, SetOperationStmt))
        {
            // 类型转换
            SetOperationStmt *op = (SetOperationStmt *) setOp;

            // 检查节点兼容性：操作类型相同，且(
            // - 要么ALL标志一致，
            // - 要么子节点是UNION ALL且父节点是UNION)
            // 同时确保列类型也一致
            if (op->op == top_union->op &&
                (op->all == top_union->all || op->all) &&
                equal(op->colTypes, top_union->colTypes))
            {
                /* 节点兼容，将其子节点提升到当前层级处理 */
                // 先将右侧子节点加入待处理列表开头（LIFO顺序）
                pending_rels = lcons(op->rarg, pending_rels);
                // 再将左侧子节点加入待处理列表开头（确保左子节点先被处理）
                pending_rels = lcons(op->larg, pending_rels);
                // 跳过当前节点的后续处理
                continue;
            }
        }

        /*
         * 当前节点不兼容或不是集合操作节点，需要单独规划
         *
         * 注意：这里不允许子节点结果中包含resjunk列。这是因为实现UNION的Append节点
         * 不会执行投影操作，如果部分输出元组包含junk列而其他不包含，上层处理会出错。
         * 这种情况主要发生在子节点是EXCEPT或INTERSECT时，其他情况下不会有resjunk列。
         */
        // 递归处理当前节点，生成其执行路径
        result = lappend(result, recurse_set_operations(setOp, root,
                                                      top_union->colTypes,
                                                      top_union->colCollations,
                                                      false, -1,
                                                      refnames_tlist,
                                                      &child_tlist,
                                                      NULL));
        // 保存当前节点的目标列表到输出参数中
        *tlist_list = lappend(*tlist_list, child_tlist);
    }

    // 返回优化后的关系节点列表
    return result;
}


/*
 * make_union_unique
 *   向给定的路径树中添加节点，以实现UNION操作的结果集去重
 *
 * 参数:
 *   op - 包含UNION操作信息的集合操作语句节点
 *   path - 指向要处理的路径的指针，代表UNION的初步执行计划
 *   tlist - 目标列表达式列表，定义结果集的列
 *   root - 优化器的根节点，包含查询优化所需的所有信息
 *
 * 返回值:
 *   返回一个新的路径指针，包含了实现去重逻辑的节点
 */
static Path *
make_union_unique(SetOperationStmt *op, Path *path, List *tlist,
				  PlannerInfo *root)
{
	RelOptInfo *result_rel; /* 结果关系的优化器信息结构 */
	List	   *groupList;  /* 用于分组/去重的表达式列表 */
	double		dNumGroups; /* 预估的不同分组数量 */

	/* 获取或创建用于SETOP操作的上层关系结构 */
	result_rel = fetch_upper_rel(root, UPPERREL_SETOP, NULL);

	/* 识别用于去重的分组语义，生成必要的分组表达式列表 */
	groupList = generate_setop_grouplist(op, tlist);

	/*
	 * XXX 当前实现中，我们保守地将不同分组的数量估计为输入的总行数，
	 * 即假设最坏情况下没有重复行。这种估计过于保守，但有助于避免哈希表
	 * 占用过多内存。此外，目前尚不清楚如何准确估计实际的去重后行数。
	 * 还需注意，初学者常倾向于使用UNION而非UNION ALL，即使他们不期望
	 * 结果中有重复行...
	 */
	dNumGroups = path->rows;

	/* 决定使用哈希聚合还是排序+唯一操作来实现去重 */
	if (choose_hashed_setop(root, groupList, path,
							dNumGroups, dNumGroups,
							"UNION"))
	{
		/* 使用哈希聚合计划实现去重 - 不需要排序 */
		path = (Path *) create_agg_path(root,
										result_rel,		/* 目标关系 */
										path,			/* 输入路径 */
										create_pathtarget(root, tlist), /* 目标列 */
										AGG_HASHED,		/* 使用哈希聚合方式 */
										AGGSPLIT_SIMPLE, /* 简单聚合拆分策略 */
										groupList,		/* 分组表达式列表 */
										NIL,			/* 聚合表达式列表(NIL表示仅去重) */
										NULL,			/* 聚合过滤器(NULL表示无过滤) */
										dNumGroups);	/* 预估的分组数量 */
	}
	else
	{
		/* 使用排序+唯一操作实现去重 */
		/* 如果有分组表达式，先创建排序路径 */
		if (groupList)
			path = (Path *)
				create_sort_path(root,
								 result_rel,	/* 目标关系 */
								 path,			/* 输入路径 */
								 make_pathkeys_for_sortclauses(root, /* 根据分组表达式创建排序键 */
															   groupList,
															   tlist),
								 -1.0);			/* 不指定排序成本 */
		/* 创建唯一操作路径，实现最终去重 */
		path = (Path *) create_upper_unique_path(root,
												 result_rel,	/* 目标关系 */
												 path,			/* 输入路径(已排序) */
												 list_length(path->pathkeys), /* 排序键数量 */
												 dNumGroups);	/* 预估的去重后行数 */
	}

	/* 返回包含去重逻辑的新路径 */
	return path;
}


/*
 * postprocess_setop_rel - 执行添加路径后所需的后处理步骤
 *
 * 参数:
 *    root - 查询规划器的根节点指针，包含整个查询的规划信息
 *    rel - 表示集合操作关系的RelOptInfo指针，已添加了执行路径
 *
 * 功能说明:
 *    此函数在PostgreSQL查询优化器中负责对集合操作（如UNION、INTERSECT、EXCEPT）的关系进行后处理。
 *    它在为集合操作生成所有可能的执行路径后被调用，主要执行两个关键任务：
 *    1. 允许扩展模块通过钩子函数添加额外的执行路径
 *    2. 从所有可用的执行路径中选择成本最低的路径
 *
 * 执行流程:
 *    - 检查是否设置了create_upper_paths_hook钩子函数，如果有，则调用它以允许扩展模块为集合操作关系添加自定义路径
 *    - 调用set_cheapest函数从所有添加的路径中选择总成本最低、启动成本最低和总成本最低的路径
 *
 * 注意事项:
 *    - 函数注释中明确指出当前PostgreSQL不考虑允许外部数据包装器(FDW)为此类关系贡献路径
 *    - 但通过钩子机制保留了扩展性，允许第三方扩展实现自定义功能
 */
static void
postprocess_setop_rel(PlannerInfo *root, RelOptInfo *rel)
{
    /*
     * 检查是否设置了create_upper_paths_hook钩子函数
     * 当前PostgreSQL核心代码不考虑允许外部数据包装器(FDW)为此类关系贡献路径，
     * 但通过钩子机制提供了扩展性，允许第三方扩展模块添加自定义路径
     */
    if (create_upper_paths_hook)
        (*create_upper_paths_hook) (root, UPPERREL_SETOP,  /* 集合操作类型的上关系 */
                                   NULL, rel, NULL);      /* 将集合操作关系传递给钩子函数 */

    /*
     * 从所有可用的执行路径中选择成本最低的路径
     * set_cheapest函数会更新rel->cheapest_total_path、rel->cheapest_startup_path等字段
     */
    set_cheapest(rel);
}


/*
 * choose_hashed_setop
 *   决定是否应该使用哈希方法来执行集合操作(如UNION、INTERSECT、EXCEPT)
 *
 * 参数:
 *   root - 优化器的根节点，包含查询上下文和参数
 *   groupClauses - 分组子句列表，定义了用于去重或分组的表达式
 *   input_path - 输入路径，代表集合操作前的执行计划
 *   dNumGroups - 估计的不同分组数量
 *   dNumOutputRows - 估计的输出行数
 *   construct - 集合操作类型名称(如"UNION"、"INTERSECT"或"EXCEPT")
 *
 * 返回值:
 *   如果应该使用哈希方法，返回true；否则返回false
 */
static bool
choose_hashed_setop(PlannerInfo *root, List *groupClauses,
				    Path *input_path,
				    double dNumGroups, double dNumOutputRows,
				    const char *construct)
{
	int			numGroupCols;   /* 分组列的数量 */
	bool		can_sort;      	/* 是否可以进行排序操作 */
	bool		can_hash;      	/* 是否可以进行哈希操作 */
	Size		hashentrysize; 	/* 哈希表条目的大小 */
	Path		hashed_p;      	/* 哈希方法的路径成本估算 */
	Path		sorted_p;      	/* 排序方法的路径成本估算 */
	double		tuple_fraction; /* 输出元组的分数比例(LIMIT相关) */

	/* 计算分组列的数量 */
	numGroupCols = list_length(groupClauses);

	/* 检查操作符是否支持排序或哈希 */
	can_sort = grouping_is_sortable(groupClauses);
	can_hash = grouping_is_hashable(groupClauses);
	
	/* 根据支持情况决定初步策略 */
	if (can_hash && can_sort)
	{
		/* 同时支持排序和哈希，需要进一步比较成本 */
	}
	else if (can_hash)
		/* 只支持哈希，直接返回true */
		return true;
	else if (can_sort)
		/* 只支持排序，直接返回false */
		return false;
	else
		/* 既不支持排序也不支持哈希，报错 */
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/* translator: %s is UNION, INTERSECT, or EXCEPT */
				 errmsg("could not implement %s", construct),
				 errdetail("Some of the datatypes only support hashing, while others only support sorting.")));

	/* 如果哈希聚合功能被禁用，则优先选择排序 */
	if (!enable_hashagg)
		return false;

	/*
	 * 如果哈希表大小可能超过work_mem内存限制，则不使用哈希方法
	 */
	/* 计算哈希表条目的大小：对齐的元组宽度 + 对齐的最小元组头大小 */
	hashentrysize = MAXALIGN(input_path->pathtarget->width) + MAXALIGN(SizeofMinimalTupleHeader);

	/* 如果总哈希表大小超过work_mem(转换为字节)，则不使用哈希方法 */
	if (hashentrysize * dNumGroups > work_mem * 1024L)
		return false;

	/*
	 * 比较哈希方法和排序方法的成本估算
	 *
	 * 我们需要比较：input_plan + hashagg 与 input_plan + sort + group
	 * 注意：实际执行计划可能使用SetOp或Unique节点，而不是Agg或Group，但
	 * 对于成本估算目的，Agg和Group的成本估计足够接近
	 *
	 * 这些路径变量只是用于保存成本字段的占位符，我们不会为这些步骤创建实际的Path对象
	 */
	/* 计算哈希聚合的成本 */
	cost_agg(&hashed_p, root, AGG_HASHED, NULL,
			 numGroupCols, dNumGroups,
			 NIL,
			 input_path->startup_cost, input_path->total_cost,
			 input_path->rows);

	/*
	 * 计算排序方法的成本
	 * 注意：输入总是未排序的，因为它是通过将不相关的子关系追加在一起而形成的
	 */
	/* 初始化排序路径的成本为输入路径的成本 */
	sorted_p.startup_cost = input_path->startup_cost;
	sorted_p.total_cost = input_path->total_cost;
	/* 计算排序成本，由于cost_sort实际上不查看pathkeys，所以传递NIL */
	cost_sort(&sorted_p, root, NIL, sorted_p.total_cost,
			 input_path->rows, input_path->pathtarget->width,
			 0.0, work_mem, -1.0);
	/* 计算分组成本 */
	cost_group(&sorted_p, root, numGroupCols, dNumGroups,
			  NIL,
			  sorted_p.startup_cost, sorted_p.total_cost,
			  input_path->rows);

	/*
	 * 使用顶层元组分数来做最终决定。首先需要将绝对数量(LIMIT)转换为分数形式
	 */
	tuple_fraction = root->tuple_fraction;
	if (tuple_fraction >= 1.0)
		tuple_fraction /= dNumOutputRows;

	/* 比较两种方法的分数成本 */
	if (compare_fractional_path_costs(&hashed_p, &sorted_p,
						  tuple_fraction) < 0)
	{
		/* 哈希方法成本更低，使用哈希 */
		return true;
	}
	/* 否则使用排序方法 */
	return false;
}


/*
 * generate_setop_tlist
 * 	为集合操作（UNION/INTERSECT/EXCEPT）的某个分支生成标准化的输出列表（TargetList）。
 *	因为集合操作要求所有分支的列数相同、对应列的数据类型兼容，所以必须对每个分支的原始输出进行“整形”。
 *
 * 参数说明：
 * - colTypes: 集合操作结果列的数据类型 OID 列表
 * - colCollations: 集合操作结果列的排序规则 OID 列表
 * - flag: 如果为 -1 表示不需要标志列，0 或 1 时生成常量标志列
 * - varno: 生成 Var 时使用的 varno
 * - hack_constants: 若为 true，则直接上移常量（见代码注释）
 * - input_tlist: 本节点输入的目标列表
 * - refnames_tlist: 用于获取列名的目标列表
 *
 * 返回值：
 *   返回生成的目标列表（List 结构）
 */
static List *
generate_setop_tlist(List *colTypes, List *colCollations,
					 int flag,
					 Index varno,
					 bool hack_constants,
					 List *input_tlist,
					 List *refnames_tlist)
{
	List	   *tlist = NIL;	/* 最终目标列表 */
	int			resno = 1;		/* 当前列序号 */
	ListCell   *ctlc,
			   *cclc,
			   *itlc,
			   *rtlc;
	TargetEntry *tle;
	Node	   *expr;

	/*
	 * 遍历每一列，生成对应的目标列表条目
	 *
	 * 这是一个四路并行遍历宏。它同时遍历：
	 * colTypes: 集合操作最终决定的列类型列表（标准）。
	 * colCollations: 集合操作最终决定的排序规则列表（标准）。
	 * input_tlist: 当前分支子查询的原始输出列表（输入）。
	 * refnames_tlist: 用于提供列名的参考列表（通常来自最左侧查询）。
	 */
	forfour(ctlc, colTypes, cclc, colCollations,
			itlc, input_tlist, rtlc, refnames_tlist)
	{
		Oid			colType = lfirst_oid(ctlc);
		Oid			colColl = lfirst_oid(cclc);
		TargetEntry *inputtle = (TargetEntry *) lfirst(itlc);
		TargetEntry *reftle = (TargetEntry *) lfirst(rtlc);

		Assert(inputtle->resno == resno);
		Assert(reftle->resno == resno);
		Assert(!inputtle->resjunk);
		Assert(!reftle->resjunk);

		/*
		 * 生成引用输入列的表达式，并保证数据类型和列名正确。
		 * 如有必要，插入类型转换。
		 *
		 * HACK: 如果输入目标列表中的表达式是常量且 hack_constants 为真，
		 * 则直接复制常量上来，而不是引用子查询输出。
		 * 这样做主要是为了处理 UNKNOWN 类型常量的类型转换。
		 * 但仅在最底层的 subquery-scan 计划中这样做，避免上层出现伪常量。
		 */
		if (hack_constants && inputtle->expr && IsA(inputtle->expr, Const))
			/* 如果子查询输出的是一个常量（比如 SELECT 'a' ...），且允许 hack，则直接把常量拿上来，而不是生成 Var。*/
			expr = (Node*)inputtle->expr;
		else
			/* 创建一个 Var 节点，引用子查询的第 N 列。*/
			expr = (Node*)makeVar(varno,
									inputtle->resno,
									exprType((Node *) inputtle->expr),
									exprTypmod((Node *) inputtle->expr),
									exprCollation((Node *) inputtle->expr),
									0);

		/*
		 * 如有必要，进行类型转换
		 * 当前分支的列类型 (exprType) 是否与集合操作要求的标准类型 (colType) 一致？
		 * 如果不一致，插入类型转换节点。
		 * 例如: 分支输出 int，标准要求 bigint。这里会包裹一层转换，变成 CAST(subquery.colN AS bigint)。
		 */
		if (exprType(expr) != colType)
		{
			/*
			 * 注意：这里直接调用 coerce_to_common_type 进行类型转换。
			 * assign_expr_collations 不会被调用，但我们会在下方强制设置正确的排序规则。
			 */
			expr = coerce_to_common_type(NULL,	/* 这里不会有 UNKNOWN 类型 */
										 expr,
										 colType,
										 "UNION/INTERSECT/EXCEPT");
		}

		/*
		 * 当前表达式的排序规则是否符合要求？
		 * 由于 plan_set_operations() 只通过 SortGroupClause 传递排序信息，
		 * 必须保证目标列表中的排序规则正确，否则上层查询可能出错。
		 * 这里用 RelabelType 而不是 CollateExpr，因为表达式会直接传递到执行器。
		 *
		 * 如果不符合，使用 RelabelType 强制指定排序规则。
		 * 这对于字符串比较非常重要（例如区分大小写 vs 不区分大小写）。
		 */
		if (exprCollation(expr) != colColl)
			expr = applyRelabelType(expr,
									exprType(expr), exprTypmod(expr), colColl,
									COERCE_IMPLICIT_CAST, -1, false);

		/* 创建一个新的 TargetEntry，包含处理好的表达式。*/
		tle = makeTargetEntry((Expr *) expr,
							  (AttrNumber) resno++,
							  pstrdup(reftle->resname),
							  false);

		/*
		 * 约定：集合操作树中所有非 resjunk 列的 ressortgroupref 等于其 resno。
		 * 有些情况下不需要该属性，但统一设置更规范。
		 */
		tle->ressortgroupref = tle->resno;

		tlist = lappend(tlist, tle);
	}

	/* 如需要，添加 resjunk 标志列 */
	if (flag >= 0)
	{
		/* 添加一个常量标志列，值为 flag */
		expr = (Node *) makeConst(INT4OID,
								  -1,
								  InvalidOid,
								  sizeof(int32),
								  Int32GetDatum(flag),
								  false,
								  true);
		tle = makeTargetEntry((Expr *) expr,
							  (AttrNumber) resno++,
							  pstrdup("flag"),
							  true);
		tlist = lappend(tlist, tle);
	}

	return tlist;
}

/*
 * 为集合操作的 Append 节点生成目标列表
 *
 * 参数说明：
 * - colTypes: 集合操作结果列的数据类型 OID 列表
 * - colCollations: 集合操作结果列的排序规则 OID 列表
 * - flag: 如果为 true，则添加一个从子计划复制上来的标志列
 * - input_tlists: 所有子计划的目标列表链表
 * - refnames_tlist: 用于获取列名的目标列表
 *
 * Append 节点的目标列表中的条目应始终为简单的 Var，
 * 只需确保它们具有正确的数据类型、typmod 和排序规则即可。
 * 这里生成的 Var 的 varno 总是 0。
 *
 * XXX：由于 varno 为 0，set_pathtarget_cost_width 无法计算出真实的宽度，
 * 但本函数本应直接生成 PathTarget，后续可优化。
 */
static List *
generate_append_tlist(List *colTypes, List *colCollations,
					  bool flag,
					  List *input_tlists,
					  List *refnames_tlist)
{
	List	   *tlist = NIL;			/* 最终目标列表 */
	int			resno = 1;				/* 当前列序号 */
	ListCell   *curColType;
	ListCell   *curColCollation;
	ListCell   *ref_tl_item;
	int			colindex;
	TargetEntry *tle;
	Node	   *expr;
	ListCell   *tlistl;
	int32	   *colTypmods;

	/*
	 * 首先提取每一列应使用的 typmod。
	 * 如果所有输入子计划在某一列的类型和 typmod 都一致，则使用该 typmod，否则用 -1。
	 */
	colTypmods = (int32 *) palloc(list_length(colTypes) * sizeof(int32));

	foreach(tlistl, input_tlists)
	{
		List	   *subtlist = (List *) lfirst(tlistl);
		ListCell   *subtlistl;

		curColType = list_head(colTypes);
		colindex = 0;
		foreach(subtlistl, subtlist)
		{
			TargetEntry *subtle = (TargetEntry *) lfirst(subtlistl);

			if (subtle->resjunk)
				continue;
			Assert(curColType != NULL);
			if (exprType((Node *) subtle->expr) == lfirst_oid(curColType))
			{
				/* 如果是第一个子计划，直接记录 typmod；否则比较是否一致 */
				int32		subtypmod = exprTypmod((Node *) subtle->expr);

				if (tlistl == list_head(input_tlists))
					colTypmods[colindex] = subtypmod;
				else if (subtypmod != colTypmods[colindex])
					colTypmods[colindex] = -1;
			}
			else
			{
				/* 类型不一致，强制 typmod 为 -1 */
				colTypmods[colindex] = -1;
			}
			curColType = lnext(curColType);
			colindex++;
		}
		Assert(curColType == NULL);
	}

	/*
	 * 现在可以为 Append 节点构建目标列表。
	 */
	colindex = 0;
	forthree(curColType, colTypes, curColCollation, colCollations,
			 ref_tl_item, refnames_tlist)
	{
		Oid			colType = lfirst_oid(curColType);
		int32		colTypmod = colTypmods[colindex++];
		Oid			colColl = lfirst_oid(curColCollation);
		TargetEntry *reftle = (TargetEntry *) lfirst(ref_tl_item);

		Assert(reftle->resno == resno);
		Assert(!reftle->resjunk);
		expr = (Node *) makeVar(0,
								resno,
								colType,
								colTypmod,
								colColl,
								0);
		tle = makeTargetEntry((Expr *) expr,
							  (AttrNumber) resno++,
							  pstrdup(reftle->resname),
							  false);

		/*
		 * 约定：集合操作树中所有非 resjunk 列的 ressortgroupref 等于其 resno。
		 * 有些情况下不需要该属性，但统一设置更规范。
		 */
		tle->ressortgroupref = tle->resno;

		tlist = lappend(tlist, tle);
	}

	if (flag)
	{
		/* 添加一个 resjunk 标志列，值从子计划复制上来 */
		expr = (Node *) makeVar(0,
								resno,
								INT4OID,
								-1,
								InvalidOid,
								0);
		tle = makeTargetEntry((Expr *) expr,
							  (AttrNumber) resno++,
							  pstrdup("flag"),
							  true);
		tlist = lappend(tlist, tle);
	}

	pfree(colTypmods);

	return tlist;
}

/*
 * generate_setop_grouplist
 *   构建一个SortGroupClause列表，定义集合操作(setop)输出列的排序/分组属性
 *
 * 解析分析阶段已经确定了这些属性并构建了适当的列表，但由于解析器输出表示
 * 不包含每个集合操作的目标列列表(tlist)，因此这些条目没有设置sortgrouprefs。
 * 所以，我们需要复制该列表并在其中安装正确的sortgrouprefs（从目标列列表复制）。
 *
 * 参数:
 *   op - 包含集合操作信息的语句节点，其中的groupClauses保存了初步的分组/排序信息
 *   targetlist - 目标列表达式列表，包含了已设置好的sortgrouprefs信息
 *
 * 返回值:
 *   返回一个新的SortGroupClause列表，其中的每个条目都已正确设置了tleSortGroupRef
 */
static List *
generate_setop_grouplist(SetOperationStmt *op, List *targetlist)
{
	List	   *grouplist; /* 要返回的分组/排序子句列表 */
	ListCell   *lg;       /* grouplist的遍历指针 */
	ListCell   *lt;       /* targetlist的遍历指针 */

	/* 深拷贝原始的groupClauses列表，避免修改原列表 */
	grouplist = copyObject(op->groupClauses);
	
	/* 初始化grouplist的遍历指针，指向列表头部 */
	lg = list_head(grouplist);
	
	/* 遍历目标列列表中的每个条目 */
	foreach(lt, targetlist)
	{
		TargetEntry *tle;      /* 当前目标列条目 */
		SortGroupClause *sgc;  /* 当前分组/排序子句 */

		/* 获取当前目标列条目 */
		tle = (TargetEntry *) lfirst(lt);
		
		/* 如果是resjunk列（临时处理列，不输出给用户），跳过 */
		if (tle->resjunk)
		{
			/* resjunk列不应该有sortgrouprefs引用 */
			Assert(tle->ressortgroupref == 0);
			continue; /* 跳过resjunk列的处理 */
		}

		/* 非resjunk列应该有sortgroupref等于其resno */
		Assert(tle->ressortgroupref == tle->resno);

		/* 非resjunk列应该有对应的分组子句 */
		Assert(lg != NULL);
		/* 获取对应的分组/排序子句 */
		sgc = (SortGroupClause *) lfirst(lg);
		/* 移动到下一个分组/排序子句 */
		lg = lnext(lg);
		/* 确保原始的tleSortGroupRef尚未设置（应为0） */
		Assert(sgc->tleSortGroupRef == 0);

		/* 从目标列条目复制sortgroupref到分组/排序子句 */
		sgc->tleSortGroupRef = tle->ressortgroupref;
	}
	
	/* 确保所有分组子句都已处理 */
	Assert(lg == NULL);
	
	/* 返回设置好的分组/排序子句列表 */
	return grouplist;
}

