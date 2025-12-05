/*-------------------------------------------------------------------------
 *
 * planagg.c
 *	  Special planning for aggregate queries.
 *
 * This module tries to replace MIN/MAX aggregate functions by subqueries
 * of the form
 *		(SELECT col FROM tab
 *		 WHERE col IS NOT NULL AND existing-quals
 *		 ORDER BY col ASC/DESC
 *		 LIMIT 1)
 * Given a suitable index on tab.col, this can be much faster than the
 * generic scan-all-the-rows aggregation plan.  We can handle multiple
 * MIN/MAX aggregates by generating multiple subqueries, and their
 * orderings can be different.  However, if the query contains any
 * non-optimizable aggregates, there's no point since we'll have to
 * scan all the rows anyway.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/planagg.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "parser/parse_clause.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static bool find_minmax_aggs_walker(Node *node, List **context);
static bool build_minmax_path(PlannerInfo *root, MinMaxAggInfo *mminfo,
							  Oid eqop, Oid sortop, bool nulls_first);
static void minmax_qp_callback(PlannerInfo *root, void *extra);
static Oid	fetch_agg_sort_op(Oid aggfnoid);


/*
 * preprocess_minmax_aggregates - 预处理MIN/MAX聚合函数
 *
 * 功能说明:
 *    检查查询中是否包含可以通过索引扫描优化的MIN/MAX聚合函数。如果存在且所有聚合函数都可能被优化，
 *    则创建一个MinMaxAggPath并将其添加到(UPPERREL_GROUP_AGG, NULL)上层关系中。
 *
 * 调用时机:
 *    应该在grouping_planner()准备调用query_planner()之前调用，因为我们通过克隆规划器状态并在
 *    修改后的查询解析树上调用query_planner()来生成索引扫描路径。因此，query_planner()之前需要的所有预处理
 *    必须已经完成。
 *
 * 参数:
 *    root - 查询规划器的根节点指针，包含整个查询的规划信息
 *
 * 返回值:
 *    无返回值，但会向查询规划器添加可能的优化路径
 */
void
preprocess_minmax_aggregates(PlannerInfo *root)
{
    Query      *parse = root->parse;       /* 查询解析树 */
    FromExpr   *jtnode;                   /* 用于遍历FROM子句的节点 */
    RangeTblRef *rtr;                     /* 范围表引用 */
    RangeTblEntry *rte;                   /* 范围表条目 */
    List       *aggs_list;                /* MIN/MAX聚合函数列表 */
    RelOptInfo *grouped_rel;              /* 分组聚合上层关系 */
    ListCell   *lc;                       /* 用于遍历列表的迭代器 */

    /* 此时minmax_aggs列表应该为空 */
    Assert(root->minmax_aggs == NIL);

    /* 如果查询不包含聚合函数，则无需处理 */
    if (!parse->hasAggs)
        return;

    /* 断言确保不会在集合操作或带FOR UPDATE的查询中调用 */
    Assert(!parse->setOperations);    /* 如果是集合操作，不应该到这里 */
    Assert(parse->rowMarks == NIL);   /* 带FOR UPDATE的查询也不应该到这里 */

    /*
     * 拒绝不可优化的情况
     *
     * 我们不处理GROUP BY或窗口函数，因为当前分组实现无论如何都需要查看所有行，
     * 因此优化MIN/MAX意义不大
     */
    if (parse->groupClause || list_length(parse->groupingSets) > 1 ||
        parse->hasWindowFuncs)
        return;

    /*
     * 如果查询包含任何CTE，也拒绝优化；无法为CTE构建索引扫描，所以无法成功优化
     * (如果CTE未被引用，这个结论不成立，但检查这种情况似乎不值得消耗额外的周期)
     */
    if (parse->cteList)
        return;

    /*
     * 我们还限制查询只引用一个表，因为连接条件无法合理处理
     * (我们可能可以处理包含笛卡尔积连接的查询，但这样做似乎不值得)
     * 然而，这个单表可能因为子查询而嵌套在多层FromExpr中
     * 注意，这里的"单表"也可以是继承父表，包括已展平为appendrel的UNION ALL子查询
     */
    jtnode = parse->jointree;
    while (IsA(jtnode, FromExpr))
    {
        if (list_length(jtnode->fromlist) != 1)
            return;
        jtnode = linitial(jtnode->fromlist);
    }
    if (!IsA(jtnode, RangeTblRef))
        return;
    rtr = (RangeTblRef *) jtnode;
    rte = planner_rt_fetch(rtr->rtindex, root);
    if (rte->rtekind == RTE_RELATION)
         /* 普通关系，符合条件 */ ;
    else if (rte->rtekind == RTE_SUBQUERY && rte->inh)
         /* 已展平的UNION ALL子查询，符合条件 */ ;
    else
        return;

    /*
     * 扫描目标列表和HAVING条件，找出所有聚合函数并验证它们都是MIN/MAX聚合
     * 一旦发现不是MIN/MAX的聚合函数，立即停止处理
     */
    aggs_list = NIL;
    if (find_minmax_aggs_walker((Node *) root->processed_tlist, &aggs_list))
        return;
    if (find_minmax_aggs_walker(parse->havingQual, &aggs_list))
        return;

    /*
     * 好的，至少有可能执行优化
     * 为每个聚合函数构建访问路径
     * 如果任何聚合函数证明无法使用索引优化，则放弃整个优化；只优化部分聚合函数没有意义
     */
    foreach(lc, aggs_list)
    {
        MinMaxAggInfo *mminfo = (MinMaxAggInfo *) lfirst(lc);
        Oid         eqop;       /* 与排序操作符对应的相等操作符 */
        bool        reverse;    /* 是否为反向排序 */

        /*
         * 我们需要聚合函数排序操作符对应的相等操作符
         */
        eqop = get_equality_op_for_ordering_op(mminfo->aggsortop, &reverse);
        if (!OidIsValid(eqop))    /* 不应该发生 */
            elog(ERROR, "could not find equality operator for ordering operator %u",
                 mminfo->aggsortop);

        /*
         * 我们可以使用NULLS FIRST或NULLS LAST的排序方式
         * 而且它们之间的性能差异可能不大，所以如果第一种方式成功就没必要再尝试第二种
         * 如果操作符是反向排序操作符，NULLS FIRST更可能可用，所以如果reverse为true先尝试这种方式
         */
        if (build_minmax_path(root, mminfo, eqop, mminfo->aggsortop, reverse))
            continue;
        if (build_minmax_path(root, mminfo, eqop, mminfo->aggsortop, !reverse))
            continue;

        /* 此聚合函数没有可用的索引路径，因此失败 */
        return;
    }

    /*
     * 好的，我们可以以这种方式执行查询
     * 准备创建MinMaxAggPath节点
     *
     * 首先，为每个聚合函数创建输出Param节点
     * (如果最终没有使用MinMaxAggPath，我们将为每个聚合函数浪费一个PARAM_EXEC槽，但这不值得担心)
     * (不幸的是，我们不能等到create_plan时再决定是否创建Param)
     */
    foreach(lc, aggs_list)
    {
        MinMaxAggInfo *mminfo = (MinMaxAggInfo *) lfirst(lc);

        mminfo->param =
            SS_make_initplan_output_param(root,
                                         exprType((Node *) mminfo->target),
                                         -1,
                                         exprCollation((Node *) mminfo->target));
    }

    /*
     * 创建带有适当估算成本和其他所需数据的MinMaxAggPath节点，
     * 并将其添加到UPPERREL_GROUP_AGG上层关系中，在那里它将与标准聚合实现竞争
     * (它可能总是获胜，但我们不需要在此处假设这一点)
     *
     * 注意：grouping_planner还没有创建这个上层关系，但我们可以先创建它
     * 我们不会在其中插入正确的consider_parallel值，但MinMaxAggPath路径当前本来就不是并行安全的，所以这不重要
     * 同样，我们没有填写rel中的FDW相关字段也不重要
     * 此外，由于没有rowmarks，我们知道processed_tlist不需要再改变，所以现在创建pathtarget是安全的
     */
    grouped_rel = fetch_upper_rel(root, UPPERREL_GROUP_AGG, NULL);
    add_path(grouped_rel, (Path *)
             create_minmaxagg_path(root, grouped_rel,
                                 create_pathtarget(root,
                                                   root->processed_tlist),
                                 aggs_list,
                                 (List *) parse->havingQual));
}


/*
 * find_minmax_aggs_walker
 *		Recursively scan the Aggref nodes in an expression tree, and check
 *		that each one is a MIN/MAX aggregate.  If so, build a list of the
 *		distinct aggregate calls in the tree.
 *
 * Returns true if a non-MIN/MAX aggregate is found, false otherwise.
 * (This seemingly-backward definition is used because expression_tree_walker
 * aborts the scan on true return, which is what we want.)
 *
 * Found aggregates are added to the list at *context; it's up to the caller
 * to initialize the list to NIL.
 *
 * This does not descend into subqueries, and so should be used only after
 * reduction of sublinks to subplans.  There mustn't be outer-aggregate
 * references either.
 */
static bool
find_minmax_aggs_walker(Node *node, List **context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		Aggref	   *aggref = (Aggref *) node;
		Oid			aggsortop;
		TargetEntry *curTarget;
		MinMaxAggInfo *mminfo;
		ListCell   *l;

		Assert(aggref->agglevelsup == 0);
		if (list_length(aggref->args) != 1)
			return true;		/* it couldn't be MIN/MAX */

		/*
		 * ORDER BY is usually irrelevant for MIN/MAX, but it can change the
		 * outcome if the aggsortop's operator class recognizes non-identical
		 * values as equal.  For example, 4.0 and 4.00 are equal according to
		 * numeric_ops, yet distinguishable.  If MIN() receives more than one
		 * value equal to 4.0 and no value less than 4.0, it is unspecified
		 * which of those equal values MIN() returns.  An ORDER BY expression
		 * that differs for each of those equal values of the argument
		 * expression makes the result predictable once again.  This is a
		 * niche requirement, and we do not implement it with subquery paths.
		 * In any case, this test lets us reject ordered-set aggregates
		 * quickly.
		 */
		if (aggref->aggorder != NIL)
			return true;
		/* note: we do not care if DISTINCT is mentioned ... */

		/*
		 * We might implement the optimization when a FILTER clause is present
		 * by adding the filter to the quals of the generated subquery.  For
		 * now, just punt.
		 */
		if (aggref->aggfilter != NULL)
			return true;

		aggsortop = fetch_agg_sort_op(aggref->aggfnoid);
		if (!OidIsValid(aggsortop))
			return true;		/* not a MIN/MAX aggregate */

		curTarget = (TargetEntry *) linitial(aggref->args);

		if (contain_mutable_functions((Node *) curTarget->expr))
			return true;		/* not potentially indexable */

		if (type_is_rowtype(exprType((Node *) curTarget->expr)))
			return true;		/* IS NOT NULL would have weird semantics */

		/*
		 * Check whether it's already in the list, and add it if not.
		 */
		foreach(l, *context)
		{
			mminfo = (MinMaxAggInfo *) lfirst(l);
			if (mminfo->aggfnoid == aggref->aggfnoid &&
				equal(mminfo->target, curTarget->expr))
				return false;
		}

		mminfo = makeNode(MinMaxAggInfo);
		mminfo->aggfnoid = aggref->aggfnoid;
		mminfo->aggsortop = aggsortop;
		mminfo->target = curTarget->expr;
		mminfo->subroot = NULL; /* don't compute path yet */
		mminfo->path = NULL;
		mminfo->pathcost = 0;
		mminfo->param = NULL;

		*context = lappend(*context, mminfo);

		/*
		 * We need not recurse into the argument, since it can't contain any
		 * aggregates.
		 */
		return false;
	}
	Assert(!IsA(node, SubLink));
	return expression_tree_walker(node, find_minmax_aggs_walker,
								  (void *) context);
}

/*
 * build_minmax_path
 *      功能：尝试为MIN/MAX聚合函数构建一个基于索引扫描的优化路径
 *
 * 参数：
 *      root      - 父查询的规划器信息结构
 *      mminfo    - 存储MIN/MAX聚合信息的结构体，成功时会更新该结构体
 *      eqop      - 相等操作符的OID
 *      sortop    - 排序操作符的OID
 *      nulls_first - NULL值是否排在前面的标志
 *
 * 返回值：
 *      true   - 成功构建了优化路径，路径信息已保存到mminfo中
 *      false  - 无法构建优化路径
 *
 * 实现原理：
 *      该函数通过构造一个特殊的子查询来模拟"SELECT col FROM tab WHERE col IS NOT NULL ORDER BY col LIMIT 1"的执行，
 *      从而利用索引的有序性直接获取极值。这种方法可以将原本需要全表扫描的MIN/MAX查询转换为高效的索引访问。
 */
static bool
build_minmax_path(PlannerInfo *root, MinMaxAggInfo *mminfo,
                  Oid eqop, Oid sortop, bool nulls_first)
{
    /* 局部变量声明 */
    PlannerInfo     *subroot;     /* 子查询的规划器信息 */
    Query           *parse;       /* 子查询的查询树 */
    TargetEntry     *tle;         /* 目标列条目 */
    List            *tlist;       /* 目标列表 */
    NullTest        *ntest;       /* 非空测试表达式 */
    SortGroupClause *sortcl;      /* 排序/分组子句 */
    RelOptInfo      *final_rel;   /* 最终关系 */
    Path            *sorted_path; /* 排序后的路径 */
    Cost            path_cost;    /* 路径成本 */
    double          path_fraction; /* 路径比例因子 */

    /*
     * 构建子查询环境：复制当前查询级别状态并调整为子查询形式
     * 所有外层引用现在会比之前高一级，完成后不会有级别为1的Var，
     * 这使得该子查询可以成为一个initplan。
     */
    subroot = (PlannerInfo *) palloc(sizeof(PlannerInfo));
    memcpy(subroot, root, sizeof(PlannerInfo));
    subroot->query_level++;        /* 增加查询级别 */
    subroot->parent_root = root;   /* 设置父查询规划器 */
    /* 重置子计划相关信息 */
    subroot->plan_params = NIL;    /* 清空计划参数 */
    subroot->outer_params = NULL;  /* 清空外部参数 */
    subroot->init_plans = NIL;     /* 清空初始化计划 */

    /* 复制并调整查询树，增加变量子级 */
    subroot->parse = parse = copyObject(root->parse);
    IncrementVarSublevelsUp((Node *) parse, 1, 1);

    /* 复制并调整附加关系列表 */
    subroot->append_rel_list = copyObject(root->append_rel_list);
    IncrementVarSublevelsUp((Node *) subroot->append_rel_list, 1, 1);
    /* 以下断言确保我们处理的是简单情况 */
    /* 目前不应有连接信息需要翻译 */
    Assert(subroot->join_info_list == NIL);
    /* 尚未创建等价类 */
    Assert(subroot->eq_classes == NIL);
    /* 尚未创建占位符信息 */
    Assert(subroot->placeholder_list == NIL);

    /*----------
     * 生成修改后的查询，形式为：
     *      (SELECT col FROM tab
     *       WHERE col IS NOT NULL AND existing-quals
     *       ORDER BY col ASC/DESC
     *       LIMIT 1)
     *----------
     */
    /* 创建仅包含聚合目标列的目标列表 */
    tle = makeTargetEntry(copyObject(mminfo->target),
                          (AttrNumber) 1,
                          pstrdup("agg_target"),
                          false);
    tlist = list_make1(tle);
    subroot->processed_tlist = parse->targetList = tlist;

    /* 清除不需要的查询子句和标志 */
    parse->havingQual = NULL;      /* 无HAVING子句 */
    subroot->hasHavingQual = false;
    parse->distinctClause = NIL;   /* 无DISTINCT子句 */
    parse->hasDistinctOn = false;
    parse->hasAggs = false;        /* 不再有聚合函数 */

    /* 构建"target IS NOT NULL"表达式 */
    ntest = makeNode(NullTest);
    ntest->nulltesttype = IS_NOT_NULL;  /* 非空测试类型 */
    ntest->arg = copyObject(mminfo->target); /* 测试目标 */
    /* 我们在find_minmax_aggs_walker中已经确认这不是行类型 */
    ntest->argisrow = false;
    ntest->location = -1;

    /* 如果WHERE子句中还没有这个非空条件，就添加它 */
    if (!list_member((List *) parse->jointree->quals, ntest))
        parse->jointree->quals = (Node *)
            lcons(ntest, (List *) parse->jointree->quals);

    /* 构建合适的ORDER BY子句 */
    sortcl = makeNode(SortGroupClause);
    /* 分配排序组引用 */
    sortcl->tleSortGroupRef = assignSortGroupRef(tle, subroot->processed_tlist);
    sortcl->eqop = eqop;           /* 设置相等操作符 */
    sortcl->sortop = sortop;       /* 设置排序操作符 */
    sortcl->nulls_first = nulls_first; /* 设置NULL值排序策略 */
    sortcl->hashable = false;      /* 无需精确设置 */
    parse->sortClause = list_make1(sortcl); /* 排序子句仅包含这一项 */

    /* 设置LIMIT 1表达式 */
    parse->limitOffset = NULL;     /* 无偏移 */
    parse->limitCount = (Node *) makeConst(INT8OID, -1, InvalidOid,
                                           sizeof(int64),
                                           Int64GetDatum(1), false,
                                           FLOAT8PASSBYVAL);

    /*
     * 为查询生成最优路径，告知query_planner我们需要LIMIT 1
     */
    subroot->tuple_fraction = 1.0;
    subroot->limit_tuples = 1.0;

    /* 调用查询规划器生成计划 */
    final_rel = query_planner(subroot, minmax_qp_callback, NULL);

    /*
     * 由于我们没有通过subquery_planner()处理子查询，
     * 我们需要自己做一些subquery_planner会做的清理工作，
     * 特别是处理子查询中使用的参数和初始化计划。
     * (如果最终不使用这个子计划，这一步就无关紧要。)
     */
    SS_identify_outer_params(subroot); /* 识别外部参数 */
    SS_charge_for_initplans(subroot, final_rel); /* 为初始化计划计算成本 */

    /*
     * 获取最佳的预排序路径，即获取单行最廉价的路径。
     * 如果没有这样的路径，则失败。
     */
    if (final_rel->rows > 1.0)
        path_fraction = 1.0 / final_rel->rows;
    else
        path_fraction = 1.0;

    /* 获取符合指定路径键的最廉价部分路径 */
    sorted_path =
        get_cheapest_fractional_path_for_pathkeys(final_rel->pathlist,
                                                  subroot->query_pathkeys,
                                                  NULL,
                                                  path_fraction);
    if (!sorted_path)  /* 如果没有合适的路径，返回失败 */
        return false;

    /*
     * 路径可能不完全返回我们想要的内容，所以需要修复。
     * (我们假设这不会改变关于哪个路径最廉价的结论。)
     */
    sorted_path = apply_projection_to_path(subroot, final_rel, sorted_path,
                                          create_pathtarget(subroot,
                                                            subroot->processed_tlist));

    /*
     * 计算获取预排序路径的第一行的成本。
     *
     * 注意：这里的成本计算应与compare_fractional_path_costs()匹配。
     * 公式：启动成本 + 比例*(总成本-启动成本)
     */
    path_cost = sorted_path->startup_cost +
        path_fraction * (sorted_path->total_cost - sorted_path->startup_cost);

    /* 保存状态以供进一步处理 */
    mminfo->subroot = subroot;     /* 保存子查询规划器信息 */
    mminfo->path = sorted_path;    /* 保存优化路径 */
    mminfo->pathcost = path_cost;  /* 保存路径成本 */

    return true;  /* 成功构建了优化路径 */
}


/*
 * Compute query_pathkeys and other pathkeys during query_planner()
 */
static void
minmax_qp_callback(PlannerInfo *root, void *extra)
{
	root->group_pathkeys = NIL;
	root->window_pathkeys = NIL;
	root->distinct_pathkeys = NIL;

	root->sort_pathkeys =
		make_pathkeys_for_sortclauses(root,
									  root->parse->sortClause,
									  root->parse->targetList);

	root->query_pathkeys = root->sort_pathkeys;
}

/*
 * Get the OID of the sort operator, if any, associated with an aggregate.
 * Returns InvalidOid if there is no such operator.
 */
static Oid
fetch_agg_sort_op(Oid aggfnoid)
{
	HeapTuple	aggTuple;
	Form_pg_aggregate aggform;
	Oid			aggsortop;

	/* fetch aggregate entry from pg_aggregate */
	aggTuple = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(aggfnoid));
	if (!HeapTupleIsValid(aggTuple))
		return InvalidOid;
	aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);
	aggsortop = aggform->aggsortop;
	ReleaseSysCache(aggTuple);

	return aggsortop;
}
