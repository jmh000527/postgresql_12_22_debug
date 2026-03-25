/*-------------------------------------------------------------------------
 *
 * allpaths.c
 *	  Routines to find possible search paths for processing a query
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/allpaths.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>
#include <math.h>

#include "access/sysattr.h"
#include "access/tsmapi.h"
#include "catalog/pg_class.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#ifdef OPTIMIZER_DEBUG
#include "nodes/print.h"
#endif
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/geqo.h"
#include "optimizer/inherit.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/planner.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parse_clause.h"
#include "parser/parsetree.h"
#include "partitioning/partbounds.h"
#include "partitioning/partprune.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"


/* results of subquery_is_pushdown_safe */
typedef struct pushdown_safety_info
{
	bool	   *unsafeColumns;	/* which output columns are unsafe to use */
	bool		unsafeVolatile; /* don't push down volatile quals */
	bool		unsafeLeaky;	/* don't push down leaky quals */
} pushdown_safety_info;

/* These parameters are set by GUC */
bool		enable_geqo = false;	/* just in case GUC doesn't set it */
int			geqo_threshold;
int			min_parallel_table_scan_size;
int			min_parallel_index_scan_size;

/* Hook for plugins to get control in set_rel_pathlist() */
set_rel_pathlist_hook_type set_rel_pathlist_hook = NULL;

/* Hook for plugins to replace standard_join_search() */
join_search_hook_type join_search_hook = NULL;


static void set_base_rel_consider_startup(PlannerInfo *root);
static void set_base_rel_sizes(PlannerInfo *root);
static void set_base_rel_pathlists(PlannerInfo *root);
static void set_rel_size(PlannerInfo *root, RelOptInfo *rel,
						 Index rti, RangeTblEntry *rte);
static void set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
							 Index rti, RangeTblEntry *rte);
static void set_plain_rel_size(PlannerInfo *root, RelOptInfo *rel,
							   RangeTblEntry *rte);
static void create_plain_partial_paths(PlannerInfo *root, RelOptInfo *rel);
static void set_rel_consider_parallel(PlannerInfo *root, RelOptInfo *rel,
									  RangeTblEntry *rte);
static void set_plain_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
								   RangeTblEntry *rte);
static void set_tablesample_rel_size(PlannerInfo *root, RelOptInfo *rel,
									 RangeTblEntry *rte);
static void set_tablesample_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
										 RangeTblEntry *rte);
static void set_foreign_size(PlannerInfo *root, RelOptInfo *rel,
							 RangeTblEntry *rte);
static void set_foreign_pathlist(PlannerInfo *root, RelOptInfo *rel,
								 RangeTblEntry *rte);
static void set_append_rel_size(PlannerInfo *root, RelOptInfo *rel,
								Index rti, RangeTblEntry *rte);
static void set_append_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
									Index rti, RangeTblEntry *rte);
static void generate_orderedappend_paths(PlannerInfo *root, RelOptInfo *rel,
										 List *live_childrels,
										 List *all_child_pathkeys,
										 List *partitioned_rels);
static Path *get_cheapest_parameterized_child_path(PlannerInfo *root,
												   RelOptInfo *rel,
												   Relids required_outer);
static void accumulate_append_subpath(Path *path,
									  List **subpaths, List **special_subpaths);
static Path *get_singleton_append_subpath(Path *path);
static void set_dummy_rel_pathlist(RelOptInfo *rel);
static void set_subquery_pathlist(PlannerInfo *root, RelOptInfo *rel,
								  Index rti, RangeTblEntry *rte);
static void set_function_pathlist(PlannerInfo *root, RelOptInfo *rel,
								  RangeTblEntry *rte);
static void set_values_pathlist(PlannerInfo *root, RelOptInfo *rel,
								RangeTblEntry *rte);
static void set_tablefunc_pathlist(PlannerInfo *root, RelOptInfo *rel,
								   RangeTblEntry *rte);
static void set_cte_pathlist(PlannerInfo *root, RelOptInfo *rel,
							 RangeTblEntry *rte);
static void set_namedtuplestore_pathlist(PlannerInfo *root, RelOptInfo *rel,
										 RangeTblEntry *rte);
static void set_result_pathlist(PlannerInfo *root, RelOptInfo *rel,
								RangeTblEntry *rte);
static void set_worktable_pathlist(PlannerInfo *root, RelOptInfo *rel,
								   RangeTblEntry *rte);
static RelOptInfo *make_rel_from_joinlist(PlannerInfo *root, List *joinlist);
static bool subquery_is_pushdown_safe(Query *subquery, Query *topquery,
									  pushdown_safety_info *safetyInfo);
static bool recurse_pushdown_safe(Node *setOp, Query *topquery,
								  pushdown_safety_info *safetyInfo);
static void check_output_expressions(Query *subquery,
									 pushdown_safety_info *safetyInfo);
static void compare_tlist_datatypes(List *tlist, List *colTypes,
									pushdown_safety_info *safetyInfo);
static bool targetIsInAllPartitionLists(TargetEntry *tle, Query *query);
static bool qual_is_pushdown_safe(Query *subquery, Index rti, Node *qual,
								  pushdown_safety_info *safetyInfo);
static void subquery_push_qual(Query *subquery,
							   RangeTblEntry *rte, Index rti, Node *qual);
static void recurse_push_qual(Node *setOp, Query *topquery,
							  RangeTblEntry *rte, Index rti, Node *qual);
static void remove_unused_subquery_outputs(Query *subquery, RelOptInfo *rel);


/*
 * make_one_rel
 *	  为执行查询查找所有可能的访问路径，返回表示查询中所有基表连接的单个 rel。
 *    向 RelOptInfo 结构添加所有可行的路径。
 */
RelOptInfo *
make_one_rel(PlannerInfo *root, List *joinlist)
{
	RelOptInfo *rel;
	Index		rti;
	double		total_pages;

	/*
	 * 构建 all_baserels Relids 集合（表示查询中所有的基表变元集）。
	 * 遍历 simple_rel_array 数组，收集所有基表 relid
	 * simple_rel_array 数组按 RT 索引存储 RelOptInfo 指针
	 * 跳过 NULL 槽和非基表 reloptkind
	 */
	root->all_baserels = NULL;
	
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *brel = root->simple_rel_array[rti];

		/* 可能有对应非基表 RTE 的空槽，跳过 */
		if (brel == NULL)
			continue;

		Assert(brel->relid == rti); /* 数组一致性断言 */

		/* 忽略被标记为 "other rels" 的 RTE */
		if (brel->reloptkind != RELOPT_BASEREL)
			continue;

		/* 将基表加入集合 */
		root->all_baserels = bms_add_member(root->all_baserels, brel->relid);
	}

	/* 标记基表是否需要考虑 fast-start（快速启动）计划 */
	set_base_rel_consider_startup(root);

	/*
	 * 为每个基表计算大小估计（行数、页数、宽度等）并设置 consider_parallel 标志
	 * 这些信息随后会用于生成参数化路径和并行路径的判断。
	 */
	set_base_rel_sizes(root);

	/*
	 * 此时我们应该对查询中涉及到的每个实际表都有了大小估计，并且知道哪些表
	 * 被连接消除、分区剪枝或约束排除删除了。因此可以计算 total_table_pages。
	 *
	 * 注意：对于 appendrel（继承/分区的父表），父表的 pages 保持为 0，避免重复计数。
	 *
	 * XXX: 如果表被自连接，这里会按出现次数重复计数，是否合适尚不明确，
	 *      且在此处检测自连接比较困难，故暂不处理。
	 */
	total_pages = 0;
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *brel = root->simple_rel_array[rti];

		if (brel == NULL)
			continue;

		Assert(brel->relid == rti); /* 数组一致性断言 */

		/* 跳过已被证明为空的关系（dummy rel） */
		if (IS_DUMMY_REL(brel))
			continue;

		/* 只统计简单基表（非 join/append 等）页数 */
		if (IS_SIMPLE_REL(brel))
			total_pages += (double) brel->pages;
	}
	root->total_table_pages = total_pages;

	/*
	 * 生成扫描路径的阶段
	 *
	 * 为每个基表生成访问路径（顺序扫描、索引扫描、TID 扫描、外部表等）。
	 * 这些路径会被添加到每个 RelOptInfo 的 pathlist / partial_pathlist 中。
	 */
	set_base_rel_pathlists(root);

	/*
	 * 生成连接路径的阶段
	 *
	 * 针对整个连接树生成访问路径（即对 joinlist 中描述的连接项进行组合搜索）。
	 * 返回的 rel 表示将所有基表连接起来的最终 joinrel。
	 */
	rel = make_rel_from_joinlist(root, joinlist);

	/*
	 * 结果 rel 的 relids 应该正好等于查询的 all_baserels（所有基表集合）。
	 */
	Assert(bms_equal(rel->relids, root->all_baserels));

	return rel;
}

/*
 * set_base_rel_consider_startup
 *	  Set the consider_[param_]startup flags for each base-relation entry.
 *
 * For the moment, we only deal with consider_param_startup here; because the
 * logic for consider_startup is pretty trivial and is the same for every base
 * relation, we just let build_simple_rel() initialize that flag correctly to
 * start with.  If that logic ever gets more complicated it would probably
 * be better to move it here.
 */
static void
set_base_rel_consider_startup(PlannerInfo *root)
{
	/*
	 * Since parameterized paths can only be used on the inside of a nestloop
	 * join plan, there is usually little value in considering fast-start
	 * plans for them.  However, for relations that are on the RHS of a SEMI
	 * or ANTI join, a fast-start plan can be useful because we're only going
	 * to care about fetching one tuple anyway.
	 *
	 * To minimize growth of planning time, we currently restrict this to
	 * cases where the RHS is a single base relation, not a join; there is no
	 * provision for consider_param_startup to get set at all on joinrels.
	 * Also we don't worry about appendrels.  costsize.c's costing rules for
	 * nestloop semi/antijoins don't consider such cases either.
	 */
	ListCell   *lc;

	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);
		int			varno;

		if ((sjinfo->jointype == JOIN_SEMI || sjinfo->jointype == JOIN_ANTI) &&
			bms_get_singleton_member(sjinfo->syn_righthand, &varno))
		{
			RelOptInfo *rel = find_base_rel(root, varno);

			rel->consider_param_startup = true;
		}
	}
}

/*
 * set_base_rel_sizes
 *	  设置每个基础关系条目的大小估计值（行数和宽度）。
 *	  同时确定是否为基础关系考虑并行路径。
 *
 * 我们在单独的遍历中执行此操作，以便行数估计值可用于参数化路径生成，
 * 并且在开始生成路径之前，每个关系的consider_parallel标志被正确设置。
 * 功能：为所有基础关系设置大小估计并确定并行执行的可行性
 * 参数：root - 规划器信息结构，包含查询的整体规划状态
 */
static void
set_base_rel_sizes(PlannerInfo *root)
{
	/* 关系表索引，用于遍历所有关系 */
	Index		rti;

	/* 遍历所有可能的关系表索引（从1开始，因为索引0通常不使用） */
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		/* 获取当前索引对应的关系优化信息 */
		RelOptInfo *rel = root->simple_rel_array[rti];
		/* 范围表条目指针，稍后用于获取表的元数据 */
		RangeTblEntry *rte;

		/* 存在可能为空的槽位，对应非基础关系的RTE */
		/* 说明：跳过数组中为空的关系槽位 */
		if (rel == NULL)
			continue;

		/* 对数组进行一致性检查，确保关系ID与数组索引匹配 */
		Assert(rel->relid == rti);

		/* 忽略那些属于"其他关系"类型的RTE */
		/* 说明：只处理基础关系类型的条目 */
		if (rel->reloptkind != RELOPT_BASEREL)
			continue;

		/* 获取当前关系对应的范围表条目 */
		rte = root->simple_rte_array[rti];

		/*
		 * 如果查询总体上允许并行执行，检查特定于此关系是否允许并行执行。
		 * 我们必须在set_rel_size()之前执行此操作，因为：
		 * (a) 如果该关系是继承父表，set_append_rel_size()将使用并可能更改关系的
		 *     consider_parallel标志；
		 * (b) 对于某些RTE类型，set_rel_size()会立即生成路径。
		 */
		/* 说明：确定当前表是否适合并行执行，这会影响后续的路径生成策略 */
		if (root->glob->parallelModeOK)
			set_rel_consider_parallel(root, rel, rte);

		/* 设置关系的大小估计（行数和宽度） */
		/* 说明：根据表的统计信息和约束条件估算关系的大小 */
		set_rel_size(root, rel, rti, rte);
	}
}


/*
 * set_base_rel_pathlists
 *    为每个基表生成所有可用的扫描路径（顺序扫描、索引扫描等）。
 *    每个可用路径都会被添加到对应关系的 pathlist 字段中。
 *
 * 这个函数是PostgreSQL查询优化器路径生成阶段的起点，负责初始化所有基表
 * 的访问路径。优化器后续会基于这些路径进行连接路径的构建和选择。
 *
 * 遍历查询中涉及的所有“基表”（Base Relations），并为每一个基表生成所有可能的访问路径（Access Paths）。
 */
static void
set_base_rel_pathlists(PlannerInfo *root) /* 规划器全局信息结构，包含查询的所有优化信息 */
{
    Index       rti; 	/* 关系表索引（Range Table Index），用于遍历关系表 */

    /*
     * 遍历 simple_rel_array 数组，为每个基表生成访问路径。
     * 跳过空槽和非基表类型的关系。
     * 注意：数组从索引1开始遍历，因为PostgreSQL中关系索引从1开始计数
     */
    for (rti = 1; rti < root->simple_rel_array_size; rti++)
    {
        /* 获取当前索引对应的关系优化信息结构体 */
        RelOptInfo *rel = root->simple_rel_array[rti];

        /*
         * 可能有对应非基表 RTE 的空槽，跳过。
         * 例如：SELECT * FROM t1 JOIN t2 ON t1.id = t2.id;
         * 解析器会生成3个RTE：t1(RTI 1), t2(RTI 2), JOIN(RTI 3)。
         * 其中RTI 3是RTE_JOIN类型，不是基表，在simple_rel_array中对应位置为NULL。
         */
        if (rel == NULL)
            continue;

        /* 数组一致性断言：确保关系ID与数组索引一致 */
        Assert(rel->relid == rti);

        /*
         * 忽略被标记为 "other rels" 的 RTE，只处理基表。
         * 这种情况主要出现在继承或分区表中。
         * 例如：SELECT * FROM parent_tb; (child_tb INHERITS parent_tb)
         * RTI 1 (parent_tb) 是 RELOPT_BASEREL，会被处理。
         * RTI 2 (child_tb) 是 RELOPT_OTHER_MEMBER_REL，会被跳过。
         * 子表的路径生成由父表在处理 Append 路径时触发，不在此处独立进行。
         */
        if (rel->reloptkind != RELOPT_BASEREL)
            continue;

        /*
         * 为基表生成访问路径并添加到 pathlist
         * 调用 set_rel_pathlist 进行实际的路径生成工作
         * 参数包括：规划器信息、关系优化信息、关系索引和关系表条目
         */
        set_rel_pathlist(root, rel, rti, root->simple_rte_array[rti]);
    }
}


/*
 * set_rel_size
 *	  为基础关系设置大小估计值
 * 功能：根据关系类型设置相应的大小估计（行数和宽度），并针对不同类型的关系采取不同的处理策略
 * 参数：
 *    root - 规划器信息结构，包含查询的整体规划状态
 *    rel - 关系优化信息，代表要设置大小的关系
 *    rti - 关系表索引
 *    rte - 范围表条目，包含表的元数据信息
 */
static void
set_rel_size(PlannerInfo *root, RelOptInfo *rel,
		 Index rti, RangeTblEntry *rte)
{
	/* 首先检查是否可以通过约束排除来跳过此关系 */
	if (rel->reloptkind == RELOPT_BASEREL &&
		relation_excluded_by_constraints(root, rel, rte))
	{
		/*
		 * 我们通过约束排除证明不需要扫描该关系，因此为其设置单个dummy路径。
		 * 这里我们只检查常规基础关系；如果是otherrel，CE已经在set_append_rel_size()中检查过。
		 *
		 * 在这种情况下，我们立即设置关系的路径，而不是留给set_rel_pathlist去做。
		 * 这是因为除了通过为其分配dummy路径外，我们没有其他方式标记关系为dummy。
		 */
		/* 说明：设置空关系的路径列表，表明此关系不需要实际扫描 */
		set_dummy_rel_pathlist(rel);
	}
	/* 处理继承关系（如分区表的父表） */
	else if (rte->inh)
	{
		/* 这是一个"append relation"，相应地处理 */
		/* 说明：处理继承表或分区表的大小估计 */
		set_append_rel_size(root, rel, rti, rte);
	}
	/* 处理所有其他类型的关系 */
	else
	{
		/* 根据关系的类型进行不同的处理 */
		switch (rel->rtekind)
		{
			case RTE_RELATION:
				/* 根据具体的关系类型进一步区分处理 */
				if (rte->relkind == RELKIND_FOREIGN_TABLE)
				{
					/* 外部表 */
					/* 说明：处理外部数据源表的大小估计 */
					set_foreign_size(root, rel, rte);
				}
				else if (rte->relkind == RELKIND_PARTITIONED_TABLE)
				{
					/*
					 * 如果使用ONLY关键字请求扫描分区表，则不应扫描任何分区，
					 * 因此将其标记为dummy关系。
					 */
					/* 说明：对于使用ONLY关键字的分区表，将其视为空关系 */
					set_dummy_rel_pathlist(rel);
				}
				else if (rte->tablesample != NULL)
				{
					/* 采样关系 */
					/* 说明：处理使用TABLESAMPLE子句的表的大小估计 */
					set_tablesample_rel_size(root, rel, rte);
				}
				else
				{
					/* 普通关系 */
					/* 说明：处理普通基础表的大小估计 */
					set_plain_rel_size(root, rel, rte);
				}
				break;
			case RTE_SUBQUERY:

					/*
					 * 子查询不支持在参数化和非参数化路径之间进行选择，
					 * 所以直接立即构建它们的路径。
					 */
					/* 说明：为子查询构建访问路径 */
					set_subquery_pathlist(root, rel, rti, rte);
				break;
			case RTE_FUNCTION:
				/* 说明：为函数关系设置大小估计 */
				set_function_size_estimates(root, rel);
				break;
			case RTE_TABLEFUNC:
				/* 说明：为表函数（如unnest）设置大小估计 */
				set_tablefunc_size_estimates(root, rel);
				break;
			case RTE_VALUES:
				/* 说明：为VALUES列表设置大小估计 */
				set_values_size_estimates(root, rel);
				break;
			case RTE_CTE:

					/*
					 * CTE不支持在参数化和非参数化路径之间进行选择，
					 * 所以直接立即构建它们的路径。
					 */
					/* 说明：区分自引用CTE和普通CTE进行处理 */
					if (rte->self_reference)
						set_worktable_pathlist(root, rel, rte);
					else
						set_cte_pathlist(root, rel, rte);
				break;
			case RTE_NAMEDTUPLESTORE:
				/* 说明：为命名元组存储设置路径（通常是临时结果集） */
				set_namedtuplestore_pathlist(root, rel, rte);
				break;
			case RTE_RESULT:
				/* 说明：为结果节点设置路径（通常是常量表达式） */
				set_result_pathlist(root, rel, rte);
				break;
			default:
				/* 错误处理：遇到未知的关系类型 */
				elog(ERROR, "unexpected rtekind: %d", (int) rel->rtekind);
				break;
		}
	}

	/*
	 * Note: In some planning scenarios (especially with constraint exclusion,
	 * security barriers, or complex subqueries), relations may temporarily have
	 * rows=0 at this point before paths are generated. The path generation phase
	 * in set_rel_pathlist() will properly mark these as dummy relations.
	 * Therefore, we don't assert here and let the planner handle it naturally.
	 */
	/* 说明：在某些规划场景（特别是约束排除、安全屏障或复杂子查询）中，关系在此时可能
	 * 暂时rows=0。路径生成阶段会正确地将这些关系标记为dummy。因此我们不在此断言。 */
}

/*
 * set_rel_pathlist
 *    为基表构建访问路径
 *
 * 该函数是PostgreSQL查询优化器中的核心函数之一，根据关系类型为各种关系
 * （表、子查询、函数等）生成可能的访问路径。它是连接查询优化中路径生成
 * 过程的基础部分，为不同类型的数据源提供了统一的路径生成入口。
 */
static void
set_rel_pathlist(PlannerInfo *root,     /* 规划器全局信息结构 */
                 RelOptInfo *rel,      /* 要生成路径的关系优化信息 */
                 Index rti,            /* 关系表索引 */
                 RangeTblEntry *rte)   /* 关系表条目，包含表的元数据 */
{
    /*
     * 根据关系类型选择不同的路径生成方式。
     * - 如果是 dummy rel（已被证明为空），无需处理。
     * - 如果是继承/分区表（append rel），调用 set_append_rel_pathlist。
     * - 否则根据 rtekind 分别处理普通表、外部表、采样表、函数、VALUES、CTE 等。
     *   部分类型（如子查询、CTE、tuplestore、Result）在 set_rel_size 阶段已处理，这里跳过。
     */
    if (IS_DUMMY_REL(rel))
    {
        /* 已经证明该关系为空，无需进一步处理 */
    }
    else if (rte->inh) /* 检查是否为继承表或分区表 */
    {
        /* 继承/分区表，需要特殊处理 - 递归处理子表并生成Append路径 */
        set_append_rel_pathlist(root, rel, rti, rte);
    }
    else
    {
        /* 根据关系类型（rtekind）选择相应的路径生成函数 */
        switch (rel->rtekind)
        {
            case RTE_RELATION: /* 普通关系（表） */
                if (rte->relkind == RELKIND_FOREIGN_TABLE)
                {
                    /* 外部表 - 调用外部表专用路径生成函数 */
                    set_foreign_pathlist(root, rel, rte);
                }
                else if (rte->tablesample != NULL)
                {
                    /* 采样表 - 需要处理表采样子句 */
                    set_tablesample_rel_pathlist(root, rel, rte);
                }
                else
                {
                    /* 普通表 - 生成顺序扫描、索引扫描等路径 */
                    set_plain_rel_pathlist(root, rel, rte);
                }
                break;
            case RTE_SUBQUERY: /* 子查询 */
                /* 子查询，已在 set_rel_size 阶段处理，此处跳过 */
                break;
            case RTE_FUNCTION: /* FROM 子句中的函数 */
                /* 处理返回记录集的函数 */
                set_function_pathlist(root, rel, rte);
                break;
            case RTE_TABLEFUNC: /* 表函数（如unnest等） */
                /* 处理表函数 */
                set_tablefunc_pathlist(root, rel, rte);
                break;
            case RTE_VALUES: /* VALUES 列表（如 VALUES (1,2), (3,4)） */
                /* 为VALUES列表生成访问路径 */
                set_values_pathlist(root, rel, rte);
                break;
            case RTE_CTE: /* CTE引用（WITH子句） */
                /* CTE引用，已在 set_rel_size 阶段处理，此处跳过 */
                break;
            case RTE_NAMEDTUPLESTORE: /* 命名元组存储引用 */
                /* tuplestore引用，已在 set_rel_size 阶段处理，此处跳过 */
                break;
            case RTE_RESULT: /* Result类型 */
                /* Result RTE，已在 set_rel_size 阶段处理，此处跳过 */
                break;
            default:
                /* 未预期的关系类型，报错 */
                elog(ERROR, "unexpected rtekind: %d", (int) rel->rtekind);
                break;
        }
    }

    /*
     * 允许插件对该基表的路径进行编辑，可以通过 add_path/add_partial_path 添加自定义路径，
     * 也可以删除或修改核心代码添加的路径。
     * 这是PostgreSQL优化器的扩展点之一，允许第三方扩展自定义优化策略。
     */
    if (set_rel_pathlist_hook)
        (*set_rel_pathlist_hook) (root, rel, rti, rte);

    /*
     * 如果是 baserel，且不是唯一基表（即存在连接），则考虑将 partial path 封装为 Gather 路径。
     * 需要在 set_rel_pathlist_hook 之后调用，以便插件可以添加 partial path。
     * 如果是继承子表则跳过，避免生成过多 Gather 节点，统一在父 appendrel 上处理。
     * 如果是唯一基表（无连接），则推迟到最终 targetlist 可用时再处理（见 grouping_planner）。
     *
     * 这部分代码处理并行查询执行路径的生成，将部分路径（partial path）包装为Gather路径。
     */
    if (rel->reloptkind == RELOPT_BASEREL && /* 确保是基表 */
        bms_membership(root->all_baserels) != BMS_SINGLETON) /* 确保不是唯一基表 */
        generate_gather_paths(root, rel, false); /* 生成并行执行路径 */

    /*
     * 选择该关系的最优路径
     * 调用set_cheapest函数从所有生成的路径中选择成本最低的路径，
     * 分别针对总代价、启动代价和排序后的路径进行选择。
     */
    set_cheapest(rel);

#ifdef OPTIMIZER_DEBUG
    /* 调试模式下打印关系和路径信息 */
    debug_print_rel(root, rel);
#endif
}


/*
 * set_plain_rel_size
 *    设置普通关系（无派生表，无继承）的大小估计值
 *
 * 参数说明：
 *    root - 规划器的全局信息结构，包含查询的所有规划信息
 *    rel - 关系的优化信息结构，用于存储关系的路径和统计信息
 *    rte - 范围表条目，表示查询中引用的关系
 *
 * 函数功能：
 *    该函数负责计算并设置普通基表的大小估计值，包括行数、行宽等统计信息。
 *    它是PostgreSQL查询优化器中路径生成阶段的重要组成部分。
 */
static void
set_plain_rel_size(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
    /*
     * 测试该关系的所有部分索引的适用性。
     * 我们必须首先执行此操作，因为部分唯一索引可能会影响大小估计结果。
     * 例如，部分索引可以确保满足特定条件的行的唯一性，这会影响最终结果集的大小估计。
     */
    check_index_predicates(root, rel);

    /*
     * 使用基于统计信息的方法估计普通基表的输出行数、宽度等信息。
     * 这一步将填充rel结构中的rows（估计行数）、width（平均行宽）
     * 以及其他与关系大小相关的统计信息，这些信息对后续的路径生成和成本计算至关重要。
     */
    set_baserel_size_estimates(root, rel);
}


/*
 * If this relation could possibly be scanned from within a worker, then set
 * its consider_parallel flag.
 */
static void
set_rel_consider_parallel(PlannerInfo *root, RelOptInfo *rel,
						  RangeTblEntry *rte)
{
	/*
	 * The flag has previously been initialized to false, so we can just
	 * return if it becomes clear that we can't safely set it.
	 */
	Assert(!rel->consider_parallel);

	/* Don't call this if parallelism is disallowed for the entire query. */
	Assert(root->glob->parallelModeOK);

	/* This should only be called for baserels and appendrel children. */
	Assert(IS_SIMPLE_REL(rel));

	/* Assorted checks based on rtekind. */
	switch (rte->rtekind)
	{
		case RTE_RELATION:

			/*
			 * Currently, parallel workers can't access the leader's temporary
			 * tables.  We could possibly relax this if we wrote all of its
			 * local buffers at the start of the query and made no changes
			 * thereafter (maybe we could allow hint bit changes), and if we
			 * taught the workers to read them.  Writing a large number of
			 * temporary buffers could be expensive, though, and we don't have
			 * the rest of the necessary infrastructure right now anyway.  So
			 * for now, bail out if we see a temporary table.
			 */
			if (get_rel_persistence(rte->relid) == RELPERSISTENCE_TEMP)
				return;

			/*
			 * Table sampling can be pushed down to workers if the sample
			 * function and its arguments are safe.
			 */
			if (rte->tablesample != NULL)
			{
				char		proparallel = func_parallel(rte->tablesample->tsmhandler);

				if (proparallel != PROPARALLEL_SAFE)
					return;
				if (!is_parallel_safe(root, (Node *) rte->tablesample->args))
					return;
			}

			/*
			 * Ask FDWs whether they can support performing a ForeignScan
			 * within a worker.  Most often, the answer will be no.  For
			 * example, if the nature of the FDW is such that it opens a TCP
			 * connection with a remote server, each parallel worker would end
			 * up with a separate connection, and these connections might not
			 * be appropriately coordinated between workers and the leader.
			 */
			if (rte->relkind == RELKIND_FOREIGN_TABLE)
			{
				Assert(rel->fdwroutine);
				if (!rel->fdwroutine->IsForeignScanParallelSafe)
					return;
				if (!rel->fdwroutine->IsForeignScanParallelSafe(root, rel, rte))
					return;
			}

			/*
			 * There are additional considerations for appendrels, which we'll
			 * deal with in set_append_rel_size and set_append_rel_pathlist.
			 * For now, just set consider_parallel based on the rel's own
			 * quals and targetlist.
			 */
			break;

		case RTE_SUBQUERY:

			/*
			 * There's no intrinsic problem with scanning a subquery-in-FROM
			 * (as distinct from a SubPlan or InitPlan) in a parallel worker.
			 * If the subquery doesn't happen to have any parallel-safe paths,
			 * then flagging it as consider_parallel won't change anything,
			 * but that's true for plain tables, too.  We must set
			 * consider_parallel based on the rel's own quals and targetlist,
			 * so that if a subquery path is parallel-safe but the quals and
			 * projection we're sticking onto it are not, we correctly mark
			 * the SubqueryScanPath as not parallel-safe.  (Note that
			 * set_subquery_pathlist() might push some of these quals down
			 * into the subquery itself, but that doesn't change anything.)
			 *
			 * We can't push sub-select containing LIMIT/OFFSET to workers as
			 * there is no guarantee that the row order will be fully
			 * deterministic, and applying LIMIT/OFFSET will lead to
			 * inconsistent results at the top-level.  (In some cases, where
			 * the result is ordered, we could relax this restriction.  But it
			 * doesn't currently seem worth expending extra effort to do so.)
			 */
			{
				Query	   *subquery = castNode(Query, rte->subquery);

				if (limit_needed(subquery))
					return;
			}
			break;

		case RTE_JOIN:
			/* Shouldn't happen; we're only considering baserels here. */
			Assert(false);
			return;

		case RTE_FUNCTION:
			/* Check for parallel-restricted functions. */
			if (!is_parallel_safe(root, (Node *) rte->functions))
				return;
			break;

		case RTE_TABLEFUNC:
			/* not parallel safe */
			return;

		case RTE_VALUES:
			/* Check for parallel-restricted functions. */
			if (!is_parallel_safe(root, (Node *) rte->values_lists))
				return;
			break;

		case RTE_CTE:

			/*
			 * CTE tuplestores aren't shared among parallel workers, so we
			 * force all CTE scans to happen in the leader.  Also, populating
			 * the CTE would require executing a subplan that's not available
			 * in the worker, might be parallel-restricted, and must get
			 * executed only once.
			 */
			return;

		case RTE_NAMEDTUPLESTORE:

			/*
			 * tuplestore cannot be shared, at least without more
			 * infrastructure to support that.
			 */
			return;

		case RTE_RESULT:
			/* RESULT RTEs, in themselves, are no problem. */
			break;
	}

	/*
	 * If there's anything in baserestrictinfo that's parallel-restricted, we
	 * give up on parallelizing access to this relation.  We could consider
	 * instead postponing application of the restricted quals until we're
	 * above all the parallelism in the plan tree, but it's not clear that
	 * that would be a win in very many cases, and it might be tricky to make
	 * outer join clauses work correctly.  It would likely break equivalence
	 * classes, too.
	 */
	if (!is_parallel_safe(root, (Node *) rel->baserestrictinfo))
		return;

	/*
	 * Likewise, if the relation's outputs are not parallel-safe, give up.
	 * (Usually, they're just Vars, but sometimes they're not.)
	 */
	if (!is_parallel_safe(root, (Node *) rel->reltarget->exprs))
		return;

	/* We have a winner. */
	rel->consider_parallel = true;
}

/*
 * set_plain_rel_pathlist
 *	  为普通表（无子查询、无继承）生成访问路径
 * 功能：为普通基础表构建各种可能的扫描路径，包括顺序扫描、并行扫描、索引扫描和TID扫描
 * 参数：
 *    root - 规划器信息结构，包含查询的整体规划状态
 *    rel - 关系优化信息，代表要为其生成路径的表
 *    rte - 范围表条目，包含表的元数据信息
 * 说明：此函数负责生成普通表的所有可能访问路径，是查询优化过程中路径生成阶段的关键组件
 */
static void
set_plain_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	/* 存储需要作为参数提供的外部关系ID集合 */
	Relids		required_outer;

	/*
	 * 顺序扫描不支持将连接条件下推到 quals，但如果 tlist 中有 LATERAL 引用，
	 * 仍然可能需要参数化路径。
	 * 说明：确定当前表是否依赖于其他关系（LATERAL连接中），如果依赖则需要参数化路径
	 */
	required_outer = rel->lateral_relids;

	/* 添加顺序扫描路径 */
	/* 说明：创建并添加顺序扫描路径，这是最基础的表访问方式，扫描表的所有行 */
	add_path(rel, create_seqscan_path(root, rel, required_outer, 0));

	/* 如果允许并行且无参数化，考虑并行顺序扫描路径 */
	/* 说明：当表允许并行扫描且不需要外部参数时，创建并行执行的顺序扫描路径以提高性能 */
	if (rel->consider_parallel && required_outer == NULL)
		create_plain_partial_paths(root, rel);

	/* 添加索引扫描路径 */
	/* 说明：为表上的所有可用索引创建相应的索引扫描路径，利用索引加速数据访问 */
	create_index_paths(root, rel);

	/* 添加 TID 扫描路径 */
	/* 说明：创建基于元组标识符(TID)的扫描路径，适用于直接通过ctid访问特定行的情况 */
	create_tidscan_paths(root, rel);
}


/*
 * create_plain_partial_paths
 *    为普通关系创建并行扫描的部分访问路径
 *
 * 功能说明：
 *    此函数为普通表关系（非分区表、非外部表等简单关系）生成并行顺序扫描的部分路径。
 *    部分路径(partial paths)是指可以并行执行的访问路径段，这些路径可以被Gather或
 *    Gather Merge节点收集和汇总，从而实现查询的并行执行。
 *
 * 参数：
 *    root - 规划器信息结构，包含查询的整体上下文和状态
 *    rel - 关系优化信息结构，表示要为其创建并行路径的关系
 *
 * 返回值：
 *    无返回值，通过修改rel结构中的partial_pathlist来添加生成的路径
 */
static void
create_plain_partial_paths(PlannerInfo *root, RelOptInfo *rel)
{
    /* 用于存储计算得到的并行工作线程数 */
    int         parallel_workers;

    /*
     * 计算需要的并行工作线程数
     * 参数说明：
     *   rel - 当前关系结构
     *   rel->pages - 关系包含的数据页数，用于估算并行度
     *   -1 - 表示使用默认的并行阈值
     *   max_parallel_workers_per_gather - 每个Gather节点允许的最大工作线程数
     */
    parallel_workers = compute_parallel_worker(rel, rel->pages, -1,
                                               max_parallel_workers_per_gather);

    /*
     * 如果并行工作线程数小于等于0，表示用户配置不允许并行扫描，
     * 或者根据表大小/系统资源计算后不适合并行扫描。
     * 在这种情况下，直接返回，不创建并行路径。
     */
    if (parallel_workers <= 0)
        return;

    /*
     * 添加一个基于并行顺序扫描的无序部分路径
     * 参数说明：
     *   root - 规划器信息
     *   rel - 目标关系
     *   NULL - 表示不指定特定的排序路径键（无序扫描）
     *   parallel_workers - 执行扫描的并行工作线程数
     *
     * 注意：create_seqscan_path函数会根据parallel_workers参数自动区分是普通顺序扫描
     * 还是并行顺序扫描。当parallel_workers>0时，创建的是并行顺序扫描路径。
     */
    add_partial_path(rel, create_seqscan_path(root, rel, NULL, parallel_workers));
}


/*
 * set_tablesample_rel_size
 *	  Set size estimates for a sampled relation
 */
static void
set_tablesample_rel_size(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	TableSampleClause *tsc = rte->tablesample;
	TsmRoutine *tsm;
	BlockNumber pages;
	double		tuples;

	/*
	 * Test any partial indexes of rel for applicability.  We must do this
	 * first since partial unique indexes can affect size estimates.
	 */
	check_index_predicates(root, rel);

	/*
	 * Call the sampling method's estimation function to estimate the number
	 * of pages it will read and the number of tuples it will return.  (Note:
	 * we assume the function returns sane values.)
	 */
	tsm = GetTsmRoutine(tsc->tsmhandler);
	tsm->SampleScanGetSampleSize(root, rel, tsc->args,
								 &pages, &tuples);

	/*
	 * For the moment, because we will only consider a SampleScan path for the
	 * rel, it's okay to just overwrite the pages and tuples estimates for the
	 * whole relation.  If we ever consider multiple path types for sampled
	 * rels, we'll need more complication.
	 */
	rel->pages = pages;
	rel->tuples = tuples;

	/* Mark rel with estimated output rows, width, etc */
	set_baserel_size_estimates(root, rel);
}

/*
 * set_tablesample_rel_pathlist
 *    为采样关系构建访问路径
 * 
 * 参数说明：
 *    root - 规划器全局信息结构体指针，包含查询优化过程中的所有上下文信息
 *    rel - 关系优化信息结构体指针，表示当前需要构建访问路径的采样关系
 *    rte - 范围表条目指针，包含采样关系的元数据信息
 * 
 * 返回值：
 *    void - 无返回值，直接修改传入的rel结构体的路径列表
 * 
 * 功能说明：
 *    此函数专门为使用TABLESAMPLE子句的关系构建访问路径。它处理采样扫描的特殊需求，
 *    包括LATERAL引用支持和不可重复采样方法的处理。
 */
static void
set_tablesample_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
    Relids      required_outer;  /* 存储LATERAL引用所需的外部关系ID集合 */
    Path       *path;            /* 当前构建的采样扫描路径 */

    /*
     * 我们不支持将连接条件推入到采样扫描的quals中，
     * 但由于其目标列表或TABLESAMPLE参数中的LATERAL引用，
     * 采样扫描仍然可能需要参数化处理。
     */
    required_outer = rel->lateral_relids;  /* 获取LATERAL引用所需的外部关系ID */

    /* 考虑采样扫描路径 */
    path = create_samplescan_path(root, rel, required_outer);  /* 创建采样扫描路径 */

    /*
     * 如果采样方法不支持可重复扫描，我们必须避免可能多次扫描该关系的计划。
     * 理想情况下，我们只需避免将该关系放在嵌套循环连接的内部；但为了支持
     * 次优采样方法的不常见用法，在规划器中添加这样的考虑似乎过于复杂。
     * 相反，如果查询可能执行不安全的连接，只需将SampleScan包装在Materialize节点中。
     * 我们可以通过计算all_baserels的成员数来检查连接（注意这正确地将继承树计为单个关系）。
     * 如果我们在子查询内部，无法轻松检查外部查询是否可能发生连接，因此假设可能发生连接。
     *
     * GetTsmRoutine相对于这里的其他测试来说比较昂贵，所以最后检查repeatable_across_scans，
     * 尽管这有点奇怪。
     */
    if ((root->query_level > 1 ||  /* 在子查询内部，假设可能发生连接 */
         bms_membership(root->all_baserels) != BMS_SINGLETON) &&  /* 存在多个基础关系，可能发生连接 */
        !(GetTsmRoutine(rte->tablesample->tsmhandler)->repeatable_across_scans))  /* 采样方法不支持跨扫描重复 */
    {
        path = (Path *) create_material_path(rel, path);  /* 创建物化路径包装采样扫描 */
    }

    add_path(rel, path);  /* 将路径添加到关系的路径列表中 */

    /* 目前，至少没有其他路径需要考虑 */
}


/*
 * set_foreign_size
 *      为外部表范围表条目(RTE)设置大小估计值
 *      
 * 参数说明：
 *      root - 规划器全局信息结构体指针，包含查询优化过程中的所有上下文信息
 *      rel - 关系优化信息结构体指针，表示当前需要设置大小估计的外部表关系
 *      rte - 范围表条目指针，包含外部表的元数据信息
 *
 * 返回值：
 *      void - 无返回值，直接修改传入的rel结构体
 *
 * 功能说明：
 *      此函数负责初始化和调整外部表的统计信息估计，为查询优化器提供必要的大小信息，
 *      包括行数、宽度等估计值。这些估计值对于外部表查询计划的生成和成本计算至关重要。
 */
static void
set_foreign_size(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
    /* 
     * 首先调用set_foreign_size_estimates设置外部表的基本大小估计信息，
     * 包括估计的输出行数、元组宽度等统计数据
     */
    set_foreign_size_estimates(root, rel);

    /* 
     * 允许外部数据包装器(FDW)通过GetForeignRelSize钩子函数调整大小估计值，
     * 这为特定的FDW提供了机会来提供更准确的表大小估计，利用FDW可能拥有的特定数据源信息
     */
    rel->fdwroutine->GetForeignRelSize(root, rel, rte->relid);

    /* 
     * 对行数估计值进行边界检查，确保行数不为零，避免后续计算中出现除零错误
     * clamp_row_est函数会确保返回一个最小的合理行数估计值
     */
    rel->rows = clamp_row_est(rel->rows);

    /* 
     * 确保元组总数估计值(rel->tuples)不会小于行数估计值(rel->rows)，
     * 因为元组总数通常表示表的完整大小，而行数表示经过过滤后预计返回的行数
     * 这一约束保证了统计信息的逻辑一致性
     */
    rel->tuples = Max(rel->tuples, rel->rows);
}


/*
 * set_foreign_pathlist
 *		Build access paths for a foreign table RTE
 */
static void
set_foreign_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	/* Call the FDW's GetForeignPaths function to generate path(s) */
	rel->fdwroutine->GetForeignPaths(root, rel, rte->relid);
}

/*
 * set_append_rel_size
 *      为简单的"append关系"（如分区表或继承表）设置大小估计值
 *      
 * 说明：
 *      传入的rel和RTE表示整个append关系。该关系的内容是通过将各个成员关系的输出
 *      附加（append）在一起计算的。注意，在非分区的继承情况下，第一个成员关系
 *      实际上与父RTE中提到的表相同，但它有不同的RTE和RelOptInfo。这是好事，
 *      因为它们的输出大小不同。
 *      
 * 参数说明：
 *      root - 规划器全局信息结构体指针，包含查询优化过程中的所有上下文信息
 *      rel - 关系优化信息结构体指针，表示需要设置大小估计的append关系
 *      rti - 关系表索引（Range Table Index），标识当前处理的关系
 *      rte - 范围表条目指针，包含关系的元数据信息
 *
 * 返回值：
 *      void - 无返回值，直接修改传入的rel结构体
 *
 * 功能说明：
 *      此函数负责计算并设置append关系的大小估计信息，通过合并所有子关系的统计信息
 *      来获得整体估计值。它处理约束排除、并行执行设置、分区级连接等高级优化特性，
 *      为后续的访问路径生成提供必要的统计数据基础。
 */
static void
set_append_rel_size(PlannerInfo *root, RelOptInfo *rel,
                   Index rti, RangeTblEntry *rte)
{
    int			parentRTindex = rti;       /* 父关系的范围表索引 */
    bool		has_live_children;        /* 是否存在至少一个有效（非排除）的子关系 */
    double		parent_rows;             /* 父关系的总行数估计值 */
    double		parent_size;             /* 父关系的总大小估计值 */
    double	   *parent_attrsizes;       /* 存储每个属性的总大小估计值 */
    int			nattrs;                   /* 关系中的属性数量 */
    ListCell   *l;                     /* 列表遍历指针 */

    /* 防止由于过深的继承树导致栈溢出 */
    check_stack_depth();

    Assert(IS_SIMPLE_REL(rel));        /* 确保处理的是简单关系 */

    /*
     * 初始化partitioned_child_rels，包含当前RT索引
     *
     * 注意：在set_append_rel_pathlist()阶段，我们会将树中出现的分区关系
     * 的索引向上冒泡，这样当我们为所有子关系创建路径后，根分区表的列表
     * 将包含所有这些索引。
     */
    if (rte->relkind == RELKIND_PARTITIONED_TABLE)
        rel->partitioned_child_rels = list_make1_int(rti);

    /*
     * 如果这是一个分区基本关系，设置consider_partitionwise_join标志
     * 当前，只有当目标列表不包含整行Var时，才考虑与基本关系进行分区级连接
     */
    if (enable_partitionwise_join &&
        rel->reloptkind == RELOPT_BASEREL &&
        rte->relkind == RELKIND_PARTITIONED_TABLE &&
        rel->attr_needed[InvalidAttrNumber - rel->min_attr] == NULL)
        rel->consider_partitionwise_join = true;

    /*
     * 初始化以计算整个append关系的大小估计值
     *
     * 我们通过按子关系行数比例加权不同子关系的宽度来处理宽度估计。这是合理的，
     * 因为宽度估计主要用于计算如果我们必须排序或哈希关系时的总关系"占用空间"。
     * 为此，我们对总等效大小求和（使用"double"算术），然后除以总行数估计值。
     * 这分别对总关系宽度和每个属性进行计算。
     *
     * 注意：如果考虑更改此逻辑，请注意子关系可能有零行和/或宽度，如果它们被约束排除。
     */
    has_live_children = false;
    parent_rows = 0;
    parent_size = 0;
    nattrs = rel->max_attr - rel->min_attr + 1;
    parent_attrsizes = (double *) palloc0(nattrs * sizeof(double));

    /* 遍历所有append关系信息，查找属于当前父关系的子关系 */
    foreach(l, root->append_rel_list)
    {
        AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(l);
        int			childRTindex;
        RangeTblEntry *childRTE;
        RelOptInfo *childrel;
        ListCell   *parentvars;
        ListCell   *childvars;

        /* append_rel_list包含所有append关系；忽略其他关系 */
        if (appinfo->parent_relid != parentRTindex)
            continue;

        childRTindex = appinfo->child_relid;
        childRTE = root->simple_rte_array[childRTindex];

        /* 子关系的RelOptInfo已经在add_other_rels_to_query期间创建 */
        childrel = find_base_rel(root, childRTindex);
        Assert(childrel->reloptkind == RELOPT_OTHER_MEMBER_REL);

        /* 我们可能已经证明该子关系是dummy（空）的 */
        if (IS_DUMMY_REL(childrel))
            continue;

        /*
         * 我们必须将父关系的目标列表和条件复制到子关系，进行适当的变量替换。
         * 但是，baserestrictinfo条件已经在构建子RelOptInfo时被复制/替换了。
         * 因此，在应用约束排除之前，我们不需要任何额外的设置。
         */
        if (relation_excluded_by_constraints(root, childrel, childRTE))
        {
            /* 此子关系不需要扫描，因此可以将其从appendrel中省略 */
            set_dummy_rel_pathlist(childrel);
            continue;
        }

        /*
         * 约束排除失败，因此将父关系的连接条件和目标列表复制到子关系，
         * 进行适当的变量替换。
         *
         * 注意：生成的childrel->reltarget->exprs可能包含任意表达式，
         * 否则不会出现在关系的目标列表中。可能查看appendrel子项的代码必须处理这种情况。
         * （通常，关系的目标列表只包含Var和PlaceHolderVars。）
         * 我们不费心更新childrel->reltarget的成本或宽度字段；尚不清楚是否有用。
         */
        childrel->joininfo = (List *)
            adjust_appendrel_attrs(root,
                                 (Node *) rel->joininfo,
                                 1, &appinfo);
        childrel->reltarget->exprs = (List *)
            adjust_appendrel_attrs(root,
                                 (Node *) rel->reltarget->exprs,
                                 1, &appinfo);

        /*
         * 我们还必须在EquivalenceClass数据结构中创建子条目。这是必要的，
         * 要么是因为父关系参与一些eclass连接（因为我们会考虑对各个子关系
         * 进行内索引扫描连接），要么是因为父关系有有用的路径键（因为我们应该
         * 尝试构建产生这些排序顺序的MergeAppend路径）。
         */
        if (rel->has_eclass_joins || has_useful_pathkeys(root, rel))
            add_child_rel_equivalences(root, appinfo, rel, childrel);
        childrel->has_eclass_joins = rel->has_eclass_joins;

        /*
         * 注意：我们可以为子关系的变量计算适当的attr_needed数据，
         * 通过translated_vars映射转换父关系的attr_needed。但是，目前不需要，
         * 因为attr_needed仅针对基本关系而非其他关系进行检查。因此，我们
         * 只是将子关系的attr_needed留空。
         */

        /*
         * 如果我们考虑与父关系进行分区级连接，则对分区子关系也做同样的处理。
         *
         * 注意：我们在这里滥用consider_partitionwise_join标志，将其设置为
         * 适用于本身未分区的子关系。我们这样做是为了告诉try_partitionwise_join()
         * 该子关系足够有效，可以用作每个分区的输入，即使它后来被证明是dummy。
         * （在我们设置好reltarget和EC条目之前，它是不可用的，我们刚刚完成了这些设置。）
         */
        if (rel->consider_partitionwise_join)
            childrel->consider_partitionwise_join = true;

        /*
         * 如果查询一般允许并行性，则查看是否特别允许此childrel。但是，如果我们已经
         * 确定整个appendrel不是并行安全的，则考虑此子关系的并行性没有意义。
         * 为了保持一致性，请在调用set_rel_size()之前执行此操作。
         */
        if (root->glob->parallelModeOK && rel->consider_parallel)
            set_rel_consider_parallel(root, childrel, childRTE);

        /* 计算子关系的大小 */
        set_rel_size(root, childrel, childRTindex, childRTE);

        /*
         * 即使我们上面没有证明，约束排除也可能检测到子查询中的矛盾。如果是这样，
         * 我们可以跳过这个子关系。
         */
        if (IS_DUMMY_REL(childrel))
            continue;

        /* 我们有至少一个有效的子关系 */
        has_live_children = true;

        /*
         * 如果任何有效子关系不是并行安全的，则将整个appendrel视为非并行安全的。
         * 将来，我们可能能够生成这样的计划：一些子节点分配给工作进程，而其他子节点
         * 则不分配；但我们今天没有这种能力，因此除非所有部分都安全，否则考虑appendrel
         * 中任何地方的部分路径都是浪费的。
         * （在此之前访问的子关系将在set_append_rel_pathlist()中取消标记。）
         */
        if (!childrel->consider_parallel)
            rel->consider_parallel = false;

        /* 从每个有效子关系累加大小信息 */
        Assert(childrel->rows > 0);

        parent_rows += childrel->rows;                           /* 累加行数 */
        parent_size += childrel->reltarget->width * childrel->rows;  /* 累加总大小 */

        /*
         * 还累加每列的估计值。我们不需要为父列表中的PlaceHolderVars做任何事情。
         * 如果子表达式不是Var，或者我们没有为其记录宽度估计，我们必须依靠基于数据类型的估计。
         *
         * 根据构造，子关系的目标列表与父关系的目标列表是一对一对应的。
         */
        forboth(parentvars, rel->reltarget->exprs,
                childvars, childrel->reltarget->exprs)
        {
            Var	   *parentvar = (Var *) lfirst(parentvars);
            Node	   *childvar = (Node *) lfirst(childvars);

            if (IsA(parentvar, Var))
            {
                int		pndx = parentvar->varattno - rel->min_attr;
                int32	child_width = 0;

                if (IsA(childvar, Var) &&
                    ((Var *) childvar)->varno == childrel->relid)
                {
                    int		cndx = ((Var *) childvar)->varattno - childrel->min_attr;

                    child_width = childrel->attr_widths[cndx];
                }
                if (child_width <= 0)
                    child_width = get_typavgwidth(exprType(childvar),
                                                  exprTypmod(childvar));
                Assert(child_width > 0);
                parent_attrsizes[pndx] += child_width * childrel->rows; /* 累加属性宽度 */
            }
        }
    }

    if (has_live_children)
    {
        /* 保存完成的大小估计值 */
        int		i;

        Assert(parent_rows > 0);
        rel->rows = parent_rows;                                  /* 设置总行数估计 */
        rel->reltarget->width = rint(parent_size / parent_rows);   /* 计算平均行宽度 */
        for (i = 0; i < nattrs; i++)
            rel->attr_widths[i] = rint(parent_attrsizes[i] / parent_rows); /* 计算每个属性的平均宽度 */

        /*
         * 为appendrel设置"原始元组"计数等于"行数"；这是必要的，因为有些地方假设
         * rel->tuples对于任何基本关系都是有效的。
         */
        rel->tuples = parent_rows;

        /*
         * 注意，我们将rel->pages保留为零；这对于避免在total_table_pages中重复计算
         * appendrel树非常重要。
         */
    }
    else
    {
        /*
         * 所有子关系都被约束排除，因此将整个appendrel标记为dummy。我们必须在此阶段执行此操作，
         * 以便在我们为其他关系生成路径时，该关系的dummy状态是可见的。
         */
        set_dummy_rel_pathlist(rel);
    }

    /* 释放分配的内存 */
    pfree(parent_attrsizes);
}


/*
 * set_append_rel_pathlist
 *	  为“追加关系”（append relation）构建访问路径
 */
static void
set_append_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
						Index rti, RangeTblEntry *rte)
{
	/* 假设有如下查询
     *
	 * CREATE TABLE sales (
	 * 	id int,
	 * 	sale_date date,
	 * 	amount int
	 * ) PARTITION BY RANGE (sale_date);
	 * 
	 * CREATE TABLE sales_2023 PARTITION OF sales
	 * 	FOR VALUES FROM ('2023-01-01') TO ('2024-01-01');
 	 * 
	 * CREATE TABLE sales_2024 PARTITION OF sales
	 * 	FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
 	 * 
	 * 查询父表 -> 触发 set_append_rel_pathlist
	 * SELECT * FROM sales WHERE sale_date >= '2023-06-01';
	 */

	int			parentRTindex = rti;	/* 父关系的范围表索引 */
	List	   *live_childrels = NIL; 	/* 存储非 dummy（非空）的子关系 */
	ListCell   *l;

	/*
	 * 为每个成员关系生成访问路径，并记住非 dummy（非空）的子关系。
	 * root->append_rel_list 存储了父子关系的映射信息。循环会遍历所有追加关系。
	 */
	foreach(l, root->append_rel_list)
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(l);
		int			childRTindex;
		RangeTblEntry *childRTE;
		RelOptInfo *childrel;

		/*
		 * append_rel_list 包含所有追加关系；忽略其他的
		 * 只有当 appinfo->parent_relid 等于当前处理的父表的范围表索引时，才继续处理。
		 */
		if (appinfo->parent_relid != parentRTindex)
			continue;

		/*
		 * 获取子表的范围表索引和 RTE、RelOptInfo
		 * 重新定位子 RTE 和 RelOptInfo
		 */
		childRTindex = appinfo->child_relid;
		childRTE = root->simple_rte_array[childRTindex];
		childrel = root->simple_rel_array[childRTindex];

		/*
		 * 并行安全性传播
		 * 如果 set_append_rel_size() 在访问此子关系后的某个时刻判定父 appendrel
		 * 是并行不安全的，我们需要将这种不安全性标记向下传播给子关系，
		 * 以便我们不会为其生成无用的部分路径（partial paths）。
		 */
		if (!rel->consider_parallel)
			childrel->consider_parallel = false;

		/*
		 * 计算子关系的访问路径。
		 * 这将设置 childrel->pathlist 和 childrel->partial_pathlist。
		 * 这将为子关系生成 SeqScan、IndexScan 等路径。
		 */
		set_rel_pathlist(root, childrel, childRTindex, childRTE);

		/*
		 * 如果子关系是 dummy（空的），忽略它。
		 */
		if (IS_DUMMY_REL(childrel))
			continue;

		/* 向上冒泡 childrel 的分区子关系。 */
		if (rel->part_scheme)
			rel->partitioned_child_rels =
				list_concat(rel->partitioned_child_rels,
							list_copy(childrel->partitioned_child_rels));

		/*
		 * 子关系是活跃的（live），因此将其添加到 live_childrels 列表中以供下面使用。
		 */
		live_childrels = lappend(live_childrels, childrel);
	}

	/*
	 * 循环结束后，live_childrels 列表里有了 [sales_2023, sales_2024]。
	 * 向追加关系添加路径。
	 * 这个函数会创建一个 Append Path（或者 MergeAppend Path），
	 * 把这两个子表的最佳路径“缝合”在一起，作为父表 sales 的访问路径。
	 */
	add_paths_to_append_rel(root, rel, live_childrels);
}


/*
 * add_paths_to_append_rel
 *		为给定的 append 关系（追加关系）生成路径，基于非 dummy（非空）的子关系集合。
 *
 * 该函数收集非 dummy 子关系支持的所有参数化和排序。对于每一种这样的参数化或排序，
 * 它创建一个 append 路径，该路径从每个非 dummy 子关系中收集一个具有给定参数化或排序的路径。
 * 同样，它从非 dummy 子关系中收集部分路径（partial paths）以创建部分 append 路径。
 */
void
add_paths_to_append_rel(PlannerInfo *root, RelOptInfo *rel,
						List *live_childrels)
{
	List	   *subpaths = NIL;
	bool		subpaths_valid = true;
	List	   *partial_subpaths = NIL;
	List	   *pa_partial_subpaths = NIL;
	List	   *pa_nonpartial_subpaths = NIL;
	bool		partial_subpaths_valid = true;
	bool		pa_subpaths_valid;
	List	   *all_child_pathkeys = NIL;
	List	   *all_child_outers = NIL;
	ListCell   *l;
	List	   *partitioned_rels = NIL;
	double		partial_rows = -1;

	/*
	 * 场景设定：
	 * 父表：orders（按年份分区）。
	 *
	 * 子表1：orders_2023（历史数据，较小，无并行扫描路径）。
	 * 路径 A：SeqScan（代价 100，无序）。
	 * 路径 B：IndexScan on order_date（代价 150，有序）
	 *
	 * 子表2：orders_2024（热点数据，巨大，支持并行扫描）。
	 * 路径 C：SeqScan（代价 500，无序）。
	 * 路径 D：Parallel SeqScan（代价 300，无序，部分路径）。
	 * 路径 E：IndexScan on order_date（代价 600，有序）。
	 *
	 * 查询：SELECT * FROM orders WHERE amount > 100 ORDER BY order_date;
	 */

	/*
	 * 如果合适，考虑并行 append
	 * 检查开关 enable_parallel_append 是否开启，且 orders 表本身是否允许并行（例如没有调用非并行安全的函数）。
	 * 如果都为真，我们就有机会生成一个“并行 Append”计划，让多个 Worker 进程同时处理不同的分区。
	 */
	pa_subpaths_valid = enable_parallel_append && rel->consider_parallel;

	/*
	 * 收集分区信息 (Partition Info)
	 * 为分区表生成的 AppendPath 必须记录作为此 Append 关系直接或间接子级的
	 * 分区表的 RT 索引。
	 *
	 * AppendPath 可能是针对子查询 RTE（UNION ALL），在这种情况下，'rel' 本身
	 * 并不代表一个分区关系，但子查询可能包含对分区关系的引用。下面的循环将查找
	 * 此类子级并将它们收集到一个列表中，以便传递给路径创建函数。（这假设我们
	 * 不需要查看多层子查询 RTE；如果我们确实需要这样做，我们可以考虑将我们在此处
	 * 生成的列表填充到子查询 RTE 的 RelOptInfo 中，就像我们对分区关系所做的那样，
	 * 这将在填充父关系的路径时使用。目前看来，这似乎是不必要的。）
	 */
	if (rel->part_scheme != NULL)
	{
		if (IS_SIMPLE_REL(rel))
			partitioned_rels = list_make1(rel->partitioned_child_rels);
		else if (IS_JOIN_REL(rel))
		{
			int			relid = -1;
			List	   *partrels = NIL;

			/*
			 * 对于分区连接关系（partitioned joinrel），连接组件关系的
			 * partitioned_child_rels 列表。
			 */
			while ((relid = bms_next_member(rel->relids, relid)) >= 0)
			{
				RelOptInfo *component;

				Assert(relid >= 1 && relid < root->simple_rel_array_size);
				component = root->simple_rel_array[relid];
				Assert(component->part_scheme != NULL);
				Assert(list_length(component->partitioned_child_rels) >= 1);
				partrels =
					list_concat(partrels,
								list_copy(component->partitioned_child_rels));
			}

			partitioned_rels = list_make1(partrels);
		}

		Assert(list_length(partitioned_rels) >= 1);
	}

	/*
	 * 核心循环：遍历子表；现在开始遍历 orders_2023 和 orders_2024。
	 * 对于每个非 dummy 子关系，记住最便宜的路径。此外，识别非 dummy 成员关系
	 * 可用的所有 pathkeys（排序）和参数化（required_outer 集合）。
	 */
	foreach(l, live_childrels)
	{
		RelOptInfo *childrel = lfirst(l);
		ListCell   *lcp;
		Path	   *cheapest_partial_path = NULL;

		/*
		 * 对于具有非空 partitioned_child_rels 的 UNION ALL，累积子关系列表。
		 */
		if (rel->rtekind == RTE_SUBQUERY && childrel->partitioned_child_rels != NIL)
			partitioned_rels = lappend(partitioned_rels,
									   childrel->partitioned_child_rels);

		/*
		 * 如果子关系有一个无参数化的最便宜总成本路径（cheapest-total path），
		 * 将其添加到我们正在为父关系构建的无参数化 Append 路径中。
		 * 如果没有，则不存在可行的无参数化路径。
		 *
		 * 对于分区聚合（partitionwise aggregates），子关系的 pathlist 可能为空，
		 * 所以不要假设这里存在路径。
		 *
		 * 场景代入：
		 * 对于 orders_2023，最便宜的是 路径 A (SeqScan, 100)。加入 subpaths。
		 * 对于 orders_2024，最便宜的完整路径是 路径 C (SeqScan, 500)。加入 subpaths。
		 * 结果：subpaths 列表准备好用于构建最基础的 Append 节点（总代价 100+500=600）。
		 */
		if (childrel->pathlist != NIL &&
			childrel->cheapest_total_path->param_info == NULL)
			accumulate_append_subpath(childrel->cheapest_total_path,
									  &subpaths, NULL);
		else
			subpaths_valid = false;

		/*
		 * 同样的想法，但是针对部分计划（partial plan）。
		 *
		 * 场景代入：
		 * orders_2023 太小，没有部分路径。partial_subpaths_valid 变为 false。
		 * orders_2024 有 路径 D (Parallel SeqScan, 300)。
		 * 结果：因为有一个子表不支持并行，纯粹的“部分路径 Append”在这里可能无法构建
		 * （取决于具体逻辑，通常要求所有子表都有部分路径才能构建完美的 Parallel Append，或者退化为混合模式）。
		 */
		if (childrel->partial_pathlist != NIL)
		{
			cheapest_partial_path = linitial(childrel->partial_pathlist);
			accumulate_append_subpath(cheapest_partial_path,
									  &partial_subpaths, NULL);
		}
		else
			partial_subpaths_valid = false;

		/*
		 * 同样的想法，但是针对混合了部分路径和非部分路径的并行 append。
		 *
		 * 场景代入：这是一个很聪明的逻辑。
		 * 对于 orders_2023，只有非并行路径 A。它被放入 pa_nonpartial_subpaths。
		 * 对于 orders_2024，并行路径 D (300) 比非并行路径 C (500) 便宜。路径 D 被放入 pa_partial_subpaths。
		 *
		 * 结果：我们收集到了一个混合方案——Worker 进程可以去扫 orders_2024，而 Leader 进程（或空闲 Worker）去扫 orders_2023。
		 */
		if (pa_subpaths_valid)
		{
			Path	   *nppath = NULL;

			nppath =
				get_cheapest_parallel_safe_total_inner(childrel->pathlist);

			if (cheapest_partial_path == NULL && nppath == NULL)
			{
				/* 既不是部分路径也不是并行安全路径？算了吧。 */
				pa_subpaths_valid = false;
			}
			else if (nppath == NULL ||
					 (cheapest_partial_path != NULL &&
					  cheapest_partial_path->total_cost < nppath->total_cost))
			{
				/* 部分路径更便宜或者是唯一的选择。 */
				Assert(cheapest_partial_path != NULL);
				accumulate_append_subpath(cheapest_partial_path,
										  &pa_partial_subpaths,
										  &pa_nonpartial_subpaths);

			}
			else
			{
				/*
				 * 要么我们只有一个非部分路径，要么我们认为单个后端执行最佳非部分路径的速度
				 * 比所有并行后端协同工作执行最佳部分路径的速度更快。
				 *
				 * 在这里更激进一点可能是有意义的。即使最佳非部分路径比最佳部分路径更昂贵，
				 * 如果有多个这样的路径可以分配给不同的工作进程，选择非部分路径可能仍然更好。
				 * 目前，我们不尝试弄清楚这一点。
				 */
				accumulate_append_subpath(nppath,
										  &pa_nonpartial_subpaths,
										  NULL);
			}
		}

		/*
		 * 收集排序和参数化信息 (Pathkeys & Outer)
		 * 收集所有子关系可用的所有路径排序和参数化的列表。我们使用这些作为启发式方法，
		 * 来指示我们应该为哪些排序顺序和参数化构建 Append 和 MergeAppend 路径。
		 *
		 * 场景代入：
		 * orders_2023 有 路径 B (IndexScan)，提供 order_date 排序。
		 * orders_2024 有 路径 E (IndexScan)，提供 order_date 排序。
		 * 结果：all_child_pathkeys 列表中记录下：我们有机会利用 order_date 的排序！
		 * 这为后续生成 MergeAppend 埋下伏笔。
		 */
		foreach(lcp, childrel->pathlist)
		{
			Path	   *childpath = (Path *) lfirst(lcp);
			List	   *childkeys = childpath->pathkeys;
			Relids		childouter = PATH_REQ_OUTER(childpath);

			/* 未排序的路径不贡献 pathkey 列表 */
			if (childkeys != NIL)
			{
				ListCell   *lpk;
				bool		found = false;

				/* 我们已经见过这种排序了吗？ */
				foreach(lpk, all_child_pathkeys)
				{
					List	   *existing_pathkeys = (List *) lfirst(lpk);

					if (compare_pathkeys(existing_pathkeys,
										 childkeys) == PATHKEYS_EQUAL)
					{
						found = true;
						break;
					}
				}
				if (!found)
				{
					/* 没有，所以将其添加到 all_child_pathkeys */
					all_child_pathkeys = lappend(all_child_pathkeys,
												 childkeys);
				}
			}

			/* 无参数化的路径不贡献 param-set 列表 */
			if (childouter)
			{
				ListCell   *lco;
				bool		found = false;

				/* 我们已经见过这个参数集了吗？ */
				foreach(lco, all_child_outers)
				{
					Relids		existing_outers = (Relids) lfirst(lco);

					if (bms_equal(existing_outers, childouter))
					{
						found = true;
						break;
					}
				}
				if (!found)
				{
					/* 没有，所以将其添加到 all_child_outers */
					all_child_outers = lappend(all_child_outers,
											   childouter);
				}
			}
		}
	}

	/*
	 * 生成标准 Append 路径
	 * 如果我们为所有子关系找到了无参数化路径，则为该关系构建一个无序、无参数化的
	 * Append 路径。（注意：即使由于约束排除导致我们有零个或一个有效子路径，这也是正确的。）
	 *
	 * 场景代入：
	 * orders_2023 有 路径 A (SeqScan)。
	 * orders_2024 有 路径 C (SeqScan)。
	 * 动作：创建一个包含 [路径 A, 路径 C] 的 Append 节点。
	 * 特点：无序，总代价 600。
	 */
	if (subpaths_valid)
		add_path(rel, (Path *) create_append_path(root, rel, subpaths, NIL,
												  NIL, NULL, 0, false,
												  partitioned_rels, -1));

	/*
	 * 生成并行 Append 路径
	 * 考虑一个无序、无参数化部分路径的 append。如果可能，使其具有并行感知能力（parallel-aware）。
	 *
	 * 动作：如果所有子表都有部分路径，这里会生成一个完全并行的 Append。
	 * 在我们的例子中，因为 orders_2023 没有部分路径，这里可能跳过。
	 */
	if (partial_subpaths_valid && partial_subpaths != NIL)
	{
		AppendPath *appendpath;
		ListCell   *lc;
		int			parallel_workers = 0;

		/* 找出任何子路径请求的最大工作进程数。 */
		foreach(lc, partial_subpaths)
		{
			Path	   *path = lfirst(lc);

			parallel_workers = Max(parallel_workers, path->parallel_workers);
		}
		Assert(parallel_workers > 0);

		/*
		 * 如果允许使用并行 append，则始终请求至少 log2(子关系数量) 个工作进程。
		 * 我们假设在这种情况下拥有额外的工作进程是有用的，因为它们将分散在子关系中。
		 * 精确的公式只是一个猜测，但我们不希望对于具有 N 个分区的表与具有相同数据的
		 * 未分区表得出截然不同的答案，因此在这里使用某种对数缩放似乎是有意义的。
		 */
		if (enable_parallel_append)
		{
			parallel_workers = Max(parallel_workers,
								   fls(list_length(live_childrels)));
			parallel_workers = Min(parallel_workers,
								   max_parallel_workers_per_gather);
		}
		Assert(parallel_workers > 0);

		/* 生成部分 append 路径。 */
		appendpath = create_append_path(root, rel, NIL, partial_subpaths,
										NIL, NULL, parallel_workers,
										enable_parallel_append,
										partitioned_rels, -1);

		/*
		 * 确保任何后续的部分路径使用相同的行数估计。
		 */
		partial_rows = appendpath->path.rows;

		/* 添加路径。 */
		add_partial_path(rel, (Path *) appendpath);
	}

	/*
	 * 生成混合并行 Append 路径
	 * 考虑使用混合了部分路径和非部分路径的并行感知 append。（这只有在至少有一个子关系
	 * 拥有比任何部分路径都便宜得多的非部分路径时才有意义；否则，我们应该使用在上一步中
	 * 添加的 append 路径。）
	 *
	 * 动作：创建一个 Parallel Append 节点，包含 [路径 A (非并行), 路径 D (并行)]。
	 * 特点：这是 PostgreSQL 强大的地方。它允许并行查询中包含非并行安全的子计划。
	 */
	if (pa_subpaths_valid && pa_nonpartial_subpaths != NIL)
	{
		AppendPath *appendpath;
		ListCell   *lc;
		int			parallel_workers = 0;

		/*
		 * 找出任何部分子路径请求的最大工作进程数。
		 */
		foreach(lc, pa_partial_subpaths)
		{
			Path	   *path = lfirst(lc);

			parallel_workers = Max(parallel_workers, path->parallel_workers);
		}

		/*
		 * 这里使用与上面相同的公式。在这个实例中更为重要，因为非部分路径不会对
		 * 计划的并行工作进程数量做出任何贡献。
		 */
		parallel_workers = Max(parallel_workers,
							   fls(list_length(live_childrels)));
		parallel_workers = Min(parallel_workers,
							   max_parallel_workers_per_gather);
		Assert(parallel_workers > 0);

		appendpath = create_append_path(root, rel, pa_nonpartial_subpaths,
										pa_partial_subpaths,
										NIL, NULL, parallel_workers, true,
										partitioned_rels, partial_rows);
		add_partial_path(rel, (Path *) appendpath);
	}

	/*
	 * 生成有序 Append / MergeAppend 路径
	 * 此外，根据收集到的子 pathkeys 列表构建无参数化的有序 append 路径。
	 *
	 * 场景代入：
	 * 函数发现 all_child_pathkeys 中包含 order_date。
	 * 它会去 orders_2023 找 order_date 的路径 -> 找到 路径 B (150)。
	 * 它会去 orders_2024 找 order_date 的路径 -> 找到 路径 E (600)。
	 * 它创建一个 MergeAppend 路径，包含 [路径 B, 路径 E]。
	 *
	 * 结果：生成了一个保留 order_date 顺序的路径。虽然总代价 (150+600=750) 比标准 Append (600) 高，
	 * 但因为它满足了查询的 ORDER BY，省去了顶层的 Sort 操作，最终可能会被选中！
	 */
	if (subpaths_valid)
		generate_orderedappend_paths(root, rel, live_childrels,
									 all_child_pathkeys,
									 partitioned_rels);

	/*
	 * 生成参数化路径 (Parameterized Paths)
	 * 为子关系中出现的每种参数化构建 Append 路径。
	 * （这看起来可能相当昂贵，但在大多数实际感兴趣的情况下，子关系将主要暴露
	 * 相同的参数化，因此实际上并没有那么多情况在这里被考虑。）
	 *
	 * Append 节点本身不能强制执行 quals（条件），因此所有 qual 检查必须在
	 * 子路径中完成。这意味着要拥有一个参数化的 Append 路径，我们必须为每个
	 * 子路径拥有完全相同的参数化；否则，某些子路径可能无法检查下推的 quals。
	 * 为了使它们匹配，我们可以尝试增加较少参数化路径的参数化。
	 *
	 * 场景代入：如果查询是 SELECT * FROM orders t1 JOIN customers t2 ON t1.customer_id = t2.id。
	 * 动作：这里会尝试为 orders 生成一个接受 t2.id 作为参数的路径（通常是 IndexScan）。
	 * 如果所有子表都能支持这种参数化扫描，
	 * 就会生成一个参数化的 Append 路径，用于 Nested Loop Join 的内表。
	 */
	foreach(l, all_child_outers)
	{
		Relids		required_outer = (Relids) lfirst(l);
		ListCell   *lcr;

		/* 为具有此参数化的 Append 选择子路径 */
		subpaths = NIL;
		subpaths_valid = true;
		foreach(lcr, live_childrels)
		{
			RelOptInfo *childrel = (RelOptInfo *) lfirst(lcr);
			Path	   *subpath;

			if (childrel->pathlist == NIL)
			{
				/* 未能为此子关系生成合适的路径 */
				subpaths_valid = false;
				break;
			}

			subpath = get_cheapest_parameterized_child_path(root,
															childrel,
															required_outer);
			if (subpath == NULL)
			{
				/* 未能为此子关系生成合适的路径 */
				subpaths_valid = false;
				break;
			}
			accumulate_append_subpath(subpath, &subpaths, NULL);
		}

		if (subpaths_valid)
			add_path(rel, (Path *)
					 create_append_path(root, rel, subpaths, NIL,
										NIL, required_outer, 0, false,
										partitioned_rels, -1));
	}

	/*
	 * 当只有一个子关系时，Append 路径可以继承子关系路径可用的任何排序，
	 * 因此考虑有序的部分路径是有用的。上面我们只考虑了每个子关系的最便宜的
	 * 部分路径，但让我们也使用任何具有 pathkeys 的部分路径来制作路径。
	 */
	if (list_length(live_childrels) == 1)
	{
		RelOptInfo *childrel = (RelOptInfo *) linitial(live_childrels);

		foreach(l, childrel->partial_pathlist)
		{
			Path	   *path = (Path *) lfirst(l);
			AppendPath *appendpath;

			/*
			 * 跳过没有 pathkeys 的路径。也跳过最便宜的部分路径，因为我们上面已经使用过它了。
			 */
			if (path->pathkeys == NIL ||
				path == linitial(childrel->partial_pathlist))
				continue;

			appendpath = create_append_path(root, rel, NIL, list_make1(path),
											NIL, NULL,
											path->parallel_workers, true,
											partitioned_rels, partial_rows);
			add_partial_path(rel, (Path *) appendpath);
		}
	}
}

/*
 * generate_orderedappend_paths
 *		为 append 关系生成有序的 append 路径
 *
 * 通常我们在这里生成 MergeAppend 路径，但在某些特殊情况下，我们可以生成简单的
 * Append 路径，因为子路径已经可以按所需的顺序提供元组。
 *
 * 我们为出现在 all_child_pathkeys 中的每种排序（pathkey 列表）生成一个路径。
 *
 * 我们同时考虑最便宜启动成本（cheapest-startup）和最便宜总成本（cheapest-total）的情况，
 * 即对于每种感兴趣的排序，收集所有最便宜启动成本的子路径和所有最便宜总成本的路径，
 * 并为每种情况构建合适的路径。
 *
 * 我们目前不在这里生成任何参数化的有序路径。虽然这样做不需要增加太多代码，
 * 但很不清楚是否值得花费规划周期来研究这些路径：在嵌套循环（nestloop）内部使用
 * 有序路径几乎没有什么用处。事实上，当前的 add_path 编码很可能会直接拒绝这些路径，
 * 因为 add_path 不会对参数化路径的排序顺序给予任何信用，而且参数化的 MergeAppend
 * 将比相应的参数化 Append 路径更昂贵。如果我们以后努力支持参数化的 mergejoin 计划，
 * 那么在这里添加对参数化路径的支持以供给此类连接可能是值得的。（不过，请参阅
 * optimizer/README 中的注释，了解为什么这可能永远不会发生。）
 */
static void
generate_orderedappend_paths(PlannerInfo *root, RelOptInfo *rel,
							 List *live_childrels,
							 List *all_child_pathkeys,
							 List *partitioned_rels)
{
	ListCell   *lcp;
	List	   *partition_pathkeys = NIL;
	List	   *partition_pathkeys_desc = NIL;
	bool		partition_pathkeys_partial = true;
	bool		partition_pathkeys_desc_partial = true;

	/*
	 * 场景设定：
	 * 父表：logs（按月份分区）。
	 *
	 * CREATE TABLE logs (
	 * 	log_time timestamp,
	 * 	severity int,
	 * 	message text
	 * ) PARTITION BY RANGE (log_time);
 	 * 
	 * -- 分区 1：一月数据
	 * CREATE TABLE logs_jan PARTITION OF logs
	 * FOR VALUES FROM ('2023-01-01') TO ('2023-02-01');
 	 * 
	 * -- 分区 2：二月数据
	 * CREATE TABLE logs_feb PARTITION OF logs
	 * FOR VALUES FROM ('2023-02-01') TO ('2023-03-01');
	 *
	 * 并且我们在每个分区上都有索引：
 	 * logs_jan 上有 (log_time) 索引和 (severity) 索引。
	 * logs_feb 上有 (log_time) 索引和 (severity) 索引。
	 */

	/*
	 * 某些分区表设置可能允许我们使用 Append 节点而不是 MergeAppend。
	 * 这在诸如 RANGE 分区表的情况下是可能的，因为它可以保证较早的分区必须包含
	 * 在排序顺序中较早出现的行。为了检测这是否相关，我们需要构建分区排序的
	 * pathkey 描述，包括正向和反向扫描。
	 */
	if (rel->part_scheme != NULL && IS_SIMPLE_REL(rel) &&
		partitions_are_ordered(rel->boundinfo, rel->nparts))
	{
		partition_pathkeys = build_partition_pathkeys(root, rel,
													  ForwardScanDirection,
													  &partition_pathkeys_partial);

		partition_pathkeys_desc = build_partition_pathkeys(root, rel,
														   BackwardScanDirection,
														   &partition_pathkeys_desc_partial);

		/*
		 * 你可能认为我们应该在这里 truncate_useless_pathkeys，但是允许作为查询
		 * pathkeys 子集的分区键通常是有用的。例如，考虑一个按 RANGE (a, b) 分区的表，
		 * 以及一个带有 ORDER BY a, b, c 的查询。如果我们有可以产生 a, b, c 排序的
		 * 子路径（也许通过 (a, b, c) 上的索引），那么将 appendrel 输出视为按 a, b, c
		 * 排序是可行的。
		 */
	}

	/* 现在考虑每种有趣的排序顺序 */
	foreach(lcp, all_child_pathkeys)
	{
		List	   *pathkeys = (List *) lfirst(lcp);
		List	   *startup_subpaths = NIL;
		List	   *total_subpaths = NIL;
		bool		startup_neq_total = false;
		ListCell   *lcr;
		bool		match_partition_order;
		bool		match_partition_order_desc;

		/*
		 * 检查是否匹配分区顺序（正向或反向）
		 * 确定此排序顺序是否与我们拥有的任何分区 pathkeys 匹配（包括升序和降序分区顺序）。
		 * 如果分区 pathkeys 恰好包含在 pathkeys 中，那么它仍然有效，如上所述，
		 * 前提是分区 pathkeys 是完整的，而不仅仅是分区键的前缀。（在这种情况下，
		 * 我们将依赖子路径对所需 pathkeys 的低阶列进行了排序。）
		 *
		 * 逻辑：如果 match_partition_order 和 match_partition_order_desc 都为 false，
		 * 		说明我们无法简单地通过按顺序扫描分区来获得有序结果。
		 * 结论：必须使用 MergeAppend，利用二叉堆（Heap）算法来动态合并来自各个子表的有序数据流。
		 */
		match_partition_order =
			pathkeys_contained_in(pathkeys, partition_pathkeys) ||
			(!partition_pathkeys_partial &&
			 pathkeys_contained_in(partition_pathkeys, pathkeys));

		match_partition_order_desc = !match_partition_order &&
			(pathkeys_contained_in(pathkeys, partition_pathkeys_desc) ||
			 (!partition_pathkeys_desc_partial &&
			  pathkeys_contained_in(partition_pathkeys_desc, pathkeys)));

		/*
		 * 为每个子表挑选“最佳”路径
		 * 我们将为每个子表选择最便宜的路径（cheapest-startup 和 cheapest-total），
		 * 并根据是否匹配分区顺序（match_partition_order 或 match_partition_order_desc）
		 * 来决定是否生成 Append 或 MergeAppend 路径。
		 */
		foreach(lcr, live_childrels)
		{
			RelOptInfo *childrel = (RelOptInfo *) lfirst(lcr);
			Path	   *cheapest_startup,
					   *cheapest_total;

			/*
			 * 找到合适的路径（如果可用）。
			 *
			 * 尝试找到该子表中，符合 pathkeys 排序要求的路径。
			 * 
			 * get_cheapest_path_for_pathkeys 的智能之处：
			 * 1. 如果子表有索引扫描（IndexScan）能提供该排序，它会返回该路径。
			 * 2. 如果子表没有索引，它会返回最便宜的扫描路径（如 SeqScan），
			 *    后续 create_merge_append_path 会自动识别并加上 Sort 节点的代价。
			 */
			cheapest_startup =
				get_cheapest_path_for_pathkeys(childrel->pathlist,
											   pathkeys,
											   NULL,
											   STARTUP_COST,
											   false);
			cheapest_total =
				get_cheapest_path_for_pathkeys(childrel->pathlist,
											   pathkeys,
											   NULL,
											   TOTAL_COST,
											   false);

			/*
			 * 如果我们找不到任何具有正确顺序的路径，就使用最便宜总成本（cheapest-total）路径；
			 * 我们稍后必须对其进行排序。
			 */
			if (cheapest_startup == NULL || cheapest_total == NULL)
			{
				/*
				 * 没有符合排序要求的路径，只能使用最便宜的总成本路径。
				 * 这通常是 SeqScan，但也可能是其他扫描类型（如 IndexScan）。
				 */
				cheapest_startup = cheapest_total = childrel->cheapest_total_path;
				/* 确保这个路径不是参数化路径（MergeAppend 目前不支持参数化路径） */
				Assert(cheapest_total->param_info == NULL);
			}

			/*
			 * 注意我们是否实际上针对 "cheapest" 和 "total" 情况有不同的路径；
			 * 通常没有必要进行两次 create_merge_append_path() 调用。
			 *
			 * 如果两者相同（大多数情况）：说明无论为了启动快还是总量快，最佳选择都是同一个路径。
			 * 那么 startup_neq_total 保持为 false。后续我们只需要构建一个 MergeAppend 路径。
			 *
			 * 如果两者不同：说明我们有两个候选方案。
			 * 方案 A：由所有子表的 cheapest_startup 路径组成的 MergeAppend（启动极快）。
			 * 方案 B：由所有子表的 cheapest_total 路径组成的 MergeAppend（总量极快）。
			 *
			 * 此时将 startup_neq_total 设为 true，告诉后续代码：“嘿，记得构建两个不同的 MergeAppend 路径供上层挑选。”
			 *
			 * 这是一个去重逻辑。
			 * 如果为了启动快和为了总量快选出的子路径是一样的，那就没必要浪费内存和 CPU 去构建两个一模一样的 MergeAppend 节点了。
			 */
			if (cheapest_startup != cheapest_total)
				startup_neq_total = true;

			/*
			 * 收集适当的子路径。所需的逻辑因 Append 和 MergeAppend 情况而异。
			 * 这段代码负责将选中的子路径（cheapest_startup 和 cheapest_total）收集到列表中，为后续构建父路径做准备。
			 * 根据是否利用了分区键的天然顺序，收集方式有三种分支。
			 *
			 * 分支 1：匹配正向分区顺序 (match_partition_order)
			 * 场景：ORDER BY sale_date，且分区是按 sale_date 递增排列的（Jan, Feb, Mar）。
			 *
			 * 分支 2：匹配反向分区顺序 (match_partition_order_desc)
			 * 场景：ORDER BY sale_date DESC，分区依然是按 sale_date 递增排列的（Jan, Feb, Mar）。
			 *
			 * 分支 3：不匹配分区顺序 (else) -> MergeAppend
			 * 场景：ORDER BY amount，与分区键无关。
			 */
			if (match_partition_order)
			{
				/*
				 * 我们将生成一个普通的 Append 路径。我们不需要 accumulate_append_subpath
				 * 做的大部分工作，但我们确实希望剔除那些只有一个子路径（因此没有做任何有用事情）
				 * 的子 Append 或 MergeAppend。
				 */

				/*
				 * 如果子路径本身就是一个只包含单个子节点的 Append，直接取其内核。
				 * 避免出现 Append -> Append -> Scan 这种多余的嵌套。
				 */
				cheapest_startup = get_singleton_append_subpath(cheapest_startup);
				cheapest_total = get_singleton_append_subpath(cheapest_total);

				/*
				 * 将子路径添加到列表中。
				 * 顺序追加 (lappend)：将子路径加到列表末尾。
				 * 结果列表顺序：[Jan_Path, Feb_Path, Mar_Path]
				 */
				startup_subpaths = lappend(startup_subpaths, cheapest_startup);
				total_subpaths = lappend(total_subpaths, cheapest_total);
			}
			else if (match_partition_order_desc)
			{
				/*
				 * 如上所述，但我们需要反转子路径的顺序，因为 nodeAppend.c 对反向排序
				 * 一无所知，并将按呈现的顺序扫描子路径。
				 */

				/*
				 * 同样地，剔除多余的 Append 层。
				 */
				cheapest_startup = get_singleton_append_subpath(cheapest_startup);
				cheapest_total = get_singleton_append_subpath(cheapest_total);

				/*
				 * 反向追加 (lcons)：将子路径加到列表前端。
				 * 结果列表顺序：[Mar_Path, Feb_Path, Jan_Path]
				 *
				 * 这确保了执行器按列表顺序执行，即先扫 Mar，再扫 Feb，最后 Jan，天然满足反向排序。
				 * 这里不仅列表顺序反了，子路径本身也必须是支持反向扫描的（例如 Index Backward Scan）。
				 */
				startup_subpaths = lcons(cheapest_startup, startup_subpaths);
				total_subpaths = lcons(cheapest_total, total_subpaths);
			}
			else
			{
				/*
				 * 否则，依靠 accumulate_append_subpath 为 MergeAppend 收集子路径。
				 *
				 * 使用 accumulate_append_subpath 进行收集。
				 * 这个函数比简单的 lappend 更智能，它会处理一些特殊情况（如拉平子 Append）。
				 * 对于 MergeAppend 来说，子路径在列表中的顺序并不重要，因为堆排序会重新排列数据流。
				 */
				accumulate_append_subpath(cheapest_startup,
										  &startup_subpaths, NULL);
				accumulate_append_subpath(cheapest_total,
										  &total_subpaths, NULL);
			}
		}

		/*
		 * 这段代码是 generate_orderedappend_paths 函数的收尾阶段，负责根据前面收集的信息，正式创建并注册路径节点。
		 * 根据之前的逻辑，我们已经为每个子关系选择了合适的路径，并将它们收集到了 startup_subpaths 和 total_subpaths 列表中。
		 * 现在，我们需要根据是否匹配分区顺序，决定是创建 Append 路径还是 MergeAppend 路径。
		 *
		 * 分支 1： 匹配分区顺序 (match_partition_order || match_partition_order_desc)
		 * 场景：ORDER BY sale_date，且分区是按 sale_date 递增排列的（Jan, Feb, Mar）。
		 * 动作：创建一个 Append 路径，子路径顺序为 Jan, Feb, Mar。
		 *
		 * 分支 2：不匹配分区顺序 (else) -> MergeAppend
		 * 场景：ORDER BY amount，与分区键无关。
		 * 动作：创建一个 MergeAppend 路径，子路径顺序任意。
		 */
		if (match_partition_order || match_partition_order_desc)
		{
			/* 我们只需要 Append */
			add_path(rel, (Path *) create_append_path(root,
													  rel,
													  startup_subpaths,
													  NIL,
													  pathkeys,
													  NULL,
													  0,
													  false,
													  partitioned_rels,
													  -1));
			if (startup_neq_total)
				add_path(rel, (Path *) create_append_path(root,
														  rel,
														  total_subpaths,
														  NIL,
														  pathkeys,
														  NULL,
														  0,
														  false,
														  partitioned_rels,
														  -1));
		}
		else
		{
			/* 我们需要 MergeAppend */
			add_path(rel, (Path *) create_merge_append_path(root,
															rel,
															startup_subpaths,
															pathkeys,
															NULL,
															partitioned_rels));
			if (startup_neq_total)
				add_path(rel, (Path *) create_merge_append_path(root,
																rel,
																total_subpaths,
																pathkeys,
																NULL,
																partitioned_rels));
		}
	}
}

/*
 * get_cheapest_parameterized_child_path
 *    获取具有指定参数化的最便宜子路径
 * 
 * 参数说明：
 *    root - 规划器全局信息结构体指针，包含查询优化过程中的所有上下文信息
 *    rel - 关系优化信息结构体指针，表示当前需要获取路径的关系
 *    required_outer - 位图集合，表示路径所需的外部关系ID
 * 
 * 返回值：
 *    Path* - 返回满足指定参数化要求的最便宜路径，如果无法创建这样的路径则返回NULL
 * 
 * 功能说明：
 *    此函数在查询优化过程中用于为特定关系查找或创建具有精确参数化要求的最便宜路径。
 *    它首先尝试查找现有的匹配路径，如果不存在则通过重新参数化现有路径来满足要求。
 */
static Path *
get_cheapest_parameterized_child_path(PlannerInfo *root, RelOptInfo *rel,
                                      Relids required_outer)
{
    Path       *cheapest;  /* 当前找到的最便宜路径 */
    ListCell   *lc;        /* 路径列表遍历指针 */

    /*
     * 查找具有不超过所需参数化的现有最便宜路径。
     * 如果它恰好具有所需的参数化，我们就完成了。
     */
    cheapest = get_cheapest_path_for_pathkeys(rel->pathlist,  /* 关系路径列表 */
                                              NIL,            /* 空路径键列表 */
                                              required_outer, /* 所需外部关系 */
                                              TOTAL_COST,     /* 总成本比较 */
                                              false);         /* 不要求精确匹配 */
    Assert(cheapest != NULL);  /* 确保至少找到一个路径 */
    if (bms_equal(PATH_REQ_OUTER(cheapest), required_outer))  /* 检查参数化是否精确匹配 */
        return cheapest;  /* 如果匹配，直接返回该路径 */

    /*
     * 否则，我们可以"重新参数化"现有路径以匹配给定的参数化，
     * 这实际上意味着将额外的连接条件推入到路径的扫描中进行检查。
     * 然而，一些现有路径可能已经检查了可用的连接条件，而其他路径可能没有；
     * 因此，不清楚重新参数化后哪个现有路径将是最便宜的。
     * 我们必须遍历所有路径来找出答案。
     */
    cheapest = NULL;  /* 重置最便宜路径指针 */
    foreach(lc, rel->pathlist)  /* 遍历关系的所有路径 */
    {
        Path       *path = (Path *) lfirst(lc);  /* 获取当前路径 */

        /* 如果路径需要比请求更多的参数化，则不能使用它 */
        if (!bms_is_subset(PATH_REQ_OUTER(path), required_outer))
            continue;  /* 跳过不满足条件的路径 */

        /*
         * 重新参数化只能增加路径的成本，所以如果它已经比当前最便宜的路径更昂贵，就忽略它。
         */
        if (cheapest != NULL &&
            compare_path_costs(cheapest, path, TOTAL_COST) <= 0)
            continue;  /* 跳过成本更高的路径 */

        /* 如果需要，重新参数化路径，然后重新检查成本 */
        if (!bms_equal(PATH_REQ_OUTER(path), required_outer))  /* 检查是否需要重新参数化 */
        {
            path = reparameterize_path(root, path, required_outer, 1.0);  /* 重新参数化路径 */
            if (path == NULL)
                continue;        /* 重新参数化失败，跳过此路径 */
            Assert(bms_equal(PATH_REQ_OUTER(path), required_outer));  /* 验证参数化结果 */

            /* 重新参数化后再次检查成本 */
            if (cheapest != NULL &&
                compare_path_costs(cheapest, path, TOTAL_COST) <= 0)
                continue;  /* 重新参数化后成本仍然更高，跳过 */
        }

        /* 我们找到了一个新的最佳路径 */
        cheapest = path;  /* 更新最便宜路径 */
    }

    /* 返回最佳路径，如果没有找到合适的候选路径则返回NULL */
    return cheapest;
}


/*
 * accumulate_append_subpath
 *		Add a subpath to the list being built for an Append or MergeAppend.
 *
 * It's possible that the child is itself an Append or MergeAppend path, in
 * which case we can "cut out the middleman" and just add its child paths to
 * our own list.  (We don't try to do this earlier because we need to apply
 * both levels of transformation to the quals.)
 *
 * Note that if we omit a child MergeAppend in this way, we are effectively
 * omitting a sort step, which seems fine: if the parent is to be an Append,
 * its result would be unsorted anyway, while if the parent is to be a
 * MergeAppend, there's no point in a separate sort on a child.
 *
 * Normally, either path is a partial path and subpaths is a list of partial
 * paths, or else path is a non-partial plan and subpaths is a list of those.
 * However, if path is a parallel-aware Append, then we add its partial path
 * children to subpaths and the rest to special_subpaths.  If the latter is
 * NULL, we don't flatten the path at all (unless it contains only partial
 * paths).
 */
static void
accumulate_append_subpath(Path *path, List **subpaths, List **special_subpaths)
{
	if (IsA(path, AppendPath))
	{
		AppendPath *apath = (AppendPath *) path;

		if (!apath->path.parallel_aware || apath->first_partial_path == 0)
		{
			/* list_copy is important here to avoid sharing list substructure */
			*subpaths = list_concat(*subpaths, list_copy(apath->subpaths));
			return;
		}
		else if (special_subpaths != NULL)
		{
			List	   *new_special_subpaths;

			/* Split Parallel Append into partial and non-partial subpaths */
			*subpaths = list_concat(*subpaths,
									list_copy_tail(apath->subpaths,
												   apath->first_partial_path));
			new_special_subpaths =
				list_truncate(list_copy(apath->subpaths),
							  apath->first_partial_path);
			*special_subpaths = list_concat(*special_subpaths,
											new_special_subpaths);
			return;
		}
	}
	else if (IsA(path, MergeAppendPath))
	{
		MergeAppendPath *mpath = (MergeAppendPath *) path;

		/* list_copy is important here to avoid sharing list substructure */
		*subpaths = list_concat(*subpaths, list_copy(mpath->subpaths));
		return;
	}

	*subpaths = lappend(*subpaths, path);
}

/*
 * get_singleton_append_subpath
 *		返回 Append/MergeAppend 的单个子路径，如果它不是包含单个子路径的
 *		Append/MergeAppend，则直接返回 'path'。
 *
 * 注意：'path' 必须不是并行感知的路径。
 */
static Path *
get_singleton_append_subpath(Path *path)
{
	Assert(!path->parallel_aware);

	if (IsA(path, AppendPath))
	{
		AppendPath *apath = (AppendPath *) path;

		if (list_length(apath->subpaths) == 1)
			return (Path *) linitial(apath->subpaths);
	}
	else if (IsA(path, MergeAppendPath))
	{
		MergeAppendPath *mpath = (MergeAppendPath *) path;

		if (list_length(mpath->subpaths) == 1)
			return (Path *) linitial(mpath->subpaths);
	}

	return path;
}

/*
 * set_dummy_rel_pathlist
 *      为被约束排除的关系构建一个虚拟路径
 *
 * 设计思路：
 * - 不创建特殊的"虚拟"路径类型，而是使用没有成员的AppendPath来表示这种状态
 *   （参见IS_DUMMY_APPEND/IS_DUMMY_REL宏）
 * - 当一个关系被约束排除（例如分区裁剪后确定某分区不包含所需数据）时，
 *   该函数为其设置特殊的虚拟路径表示
 *
 * 相关函数对比：
 * - 与mark_dummy_rel功能相似，但mark_dummy_rel通常用于在已经生成路径后
 *   将关系更改为虚拟状态
 * - 此函数主要在初始路径生成阶段使用
 */
static void
set_dummy_rel_pathlist(RelOptInfo *rel)  /* 要设置为虚拟状态的关系优化信息结构 */
{
    /* 设置虚拟大小估计值 - 属性宽度数组保持为0 */
    rel->rows = 0;                 /* 将行数设置为0，表明关系没有数据 */
    rel->reltarget->width = 0;     /* 将行宽设置为0，表明无需存储任何值 */

    /* 丢弃任何预先存在的路径；不再需要它们 */
    rel->pathlist = NIL;           /* 清空常规路径列表 */
    rel->partial_pathlist = NIL;   /* 清空部分路径列表 */

    /* 设置虚拟路径 - 创建一个没有子路径的AppendPath */
    add_path(rel, (Path *) create_append_path(NULL, rel, NIL, NIL,
                                              NIL, rel->lateral_relids,
                                              0, false, NIL, -1));
    /* 参数说明：
     * - NULL: 没有子路径的Append路径
     * - rel: 目标关系
     * - NIL: 没有子路径列表
     * - NIL: 没有分区边界信息
     * - NIL: 没有分区表信息
     * - rel->lateral_relids: 保留原始的lateral引用信息
     * - 0: 成本为0（无需实际扫描）
     * - false: 不是并行安全的（虚拟路径不参与实际执行）
     * - NIL: 没有分区键表达式
     * - -1: 无效的分区OID
     */

    /*
     * 立即设置最便宜路径字段，以防它们之前指向已丢弃的路径。
     * 当从set_rel_size()调用时，这是冗余的，但从其他地方调用时不是，
     * 并且这样做两次也无害。
     * 这确保了后续优化器阶段能正确处理这个虚拟关系。
     */
    set_cheapest(rel);
}


/* quick-and-dirty test to see if any joining is needed */
static bool
has_multiple_baserels(PlannerInfo *root)
{
	int			num_base_rels = 0;
	Index		rti;

	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *brel = root->simple_rel_array[rti];

		if (brel == NULL)
			continue;

		/* ignore RTEs that are "other rels" */
		if (brel->reloptkind == RELOPT_BASEREL)
			if (++num_base_rels > 1)
				return true;
	}
	return false;
}

/*
 * set_subquery_pathlist
 *		Generate SubqueryScan access paths for a subquery RTE
 *
 * We don't currently support generating parameterized paths for subqueries
 * by pushing join clauses down into them; it seems too expensive to re-plan
 * the subquery multiple times to consider different alternatives.
 * (XXX that could stand to be reconsidered, now that we use Paths.)
 * So the paths made here will be parameterized if the subquery contains
 * LATERAL references, otherwise not.  As long as that's true, there's no need
 * for a separate set_subquery_size phase: just make the paths right away.
 */
static void
set_subquery_pathlist(PlannerInfo *root, RelOptInfo *rel,
					  Index rti, RangeTblEntry *rte)
{
	Query	   *parse = root->parse;
	Query	   *subquery = rte->subquery;
	Relids		required_outer;
	pushdown_safety_info safetyInfo;
	double		tuple_fraction;
	RelOptInfo *sub_final_rel;
	ListCell   *lc;

	/*
	 * Must copy the Query so that planning doesn't mess up the RTE contents
	 * (really really need to fix the planner to not scribble on its input,
	 * someday ... but see remove_unused_subquery_outputs to start with).
	 */
	subquery = copyObject(subquery);

	/*
	 * If it's a LATERAL subquery, it might contain some Vars of the current
	 * query level, requiring it to be treated as parameterized, even though
	 * we don't support pushing down join quals into subqueries.
	 */
	required_outer = rel->lateral_relids;

	/*
	 * Zero out result area for subquery_is_pushdown_safe, so that it can set
	 * flags as needed while recursing.  In particular, we need a workspace
	 * for keeping track of unsafe-to-reference columns.  unsafeColumns[i]
	 * will be set true if we find that output column i of the subquery is
	 * unsafe to use in a pushed-down qual.
	 */
	memset(&safetyInfo, 0, sizeof(safetyInfo));
	safetyInfo.unsafeColumns = (bool *)
		palloc0((list_length(subquery->targetList) + 1) * sizeof(bool));

	/*
	 * If the subquery has the "security_barrier" flag, it means the subquery
	 * originated from a view that must enforce row level security.  Then we
	 * must not push down quals that contain leaky functions.  (Ideally this
	 * would be checked inside subquery_is_pushdown_safe, but since we don't
	 * currently pass the RTE to that function, we must do it here.)
	 */
	safetyInfo.unsafeLeaky = rte->security_barrier;

	/*
	 * If there are any restriction clauses that have been attached to the
	 * subquery relation, consider pushing them down to become WHERE or HAVING
	 * quals of the subquery itself.  This transformation is useful because it
	 * may allow us to generate a better plan for the subquery than evaluating
	 * all the subquery output rows and then filtering them.
	 *
	 * There are several cases where we cannot push down clauses. Restrictions
	 * involving the subquery are checked by subquery_is_pushdown_safe().
	 * Restrictions on individual clauses are checked by
	 * qual_is_pushdown_safe().  Also, we don't want to push down
	 * pseudoconstant clauses; better to have the gating node above the
	 * subquery.
	 *
	 * Non-pushed-down clauses will get evaluated as qpquals of the
	 * SubqueryScan node.
	 *
	 * XXX Are there any cases where we want to make a policy decision not to
	 * push down a pushable qual, because it'd result in a worse plan?
	 */
	if (rel->baserestrictinfo != NIL &&
		subquery_is_pushdown_safe(subquery, subquery, &safetyInfo))
	{
		/* OK to consider pushing down individual quals */
		List	   *upperrestrictlist = NIL;
		ListCell   *l;

		foreach(l, rel->baserestrictinfo)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);
			Node	   *clause = (Node *) rinfo->clause;

			if (!rinfo->pseudoconstant &&
				qual_is_pushdown_safe(subquery, rti, clause, &safetyInfo))
			{
				/* Push it down */
				subquery_push_qual(subquery, rte, rti, clause);
			}
			else
			{
				/* Keep it in the upper query */
				upperrestrictlist = lappend(upperrestrictlist, rinfo);
			}
		}
		rel->baserestrictinfo = upperrestrictlist;
		/* We don't bother recomputing baserestrict_min_security */
	}

	pfree(safetyInfo.unsafeColumns);

	/*
	 * The upper query might not use all the subquery's output columns; if
	 * not, we can simplify.
	 */
	remove_unused_subquery_outputs(subquery, rel);

	/*
	 * We can safely pass the outer tuple_fraction down to the subquery if the
	 * outer level has no joining, aggregation, or sorting to do. Otherwise
	 * we'd better tell the subquery to plan for full retrieval. (XXX This
	 * could probably be made more intelligent ...)
	 */
	if (parse->hasAggs ||
		parse->groupClause ||
		parse->groupingSets ||
		parse->havingQual ||
		parse->distinctClause ||
		parse->sortClause ||
		has_multiple_baserels(root))
		tuple_fraction = 0.0;	/* default case */
	else
		tuple_fraction = root->tuple_fraction;

	/* plan_params should not be in use in current query level */
	Assert(root->plan_params == NIL);

	/* Generate a subroot and Paths for the subquery */
	rel->subroot = subquery_planner(root->glob, subquery,
									root,
									false, tuple_fraction);

	/* Isolate the params needed by this specific subplan */
	rel->subplan_params = root->plan_params;
	root->plan_params = NIL;

	/*
	 * It's possible that constraint exclusion proved the subquery empty. If
	 * so, it's desirable to produce an unadorned dummy path so that we will
	 * recognize appropriate optimizations at this query level.
	 */
	sub_final_rel = fetch_upper_rel(rel->subroot, UPPERREL_FINAL, NULL);

	if (IS_DUMMY_REL(sub_final_rel))
	{
		 (rel);
		return;
	}

	/*
	 * Mark rel with estimated output rows, width, etc.  Note that we have to
	 * do this before generating outer-query paths, else cost_subqueryscan is
	 * not happy.
	 */
	set_subquery_size_estimates(root, rel);

	/*
	 * For each Path that subquery_planner produced, make a SubqueryScanPath
	 * in the outer query.
	 */
	foreach(lc, sub_final_rel->pathlist)
	{
		Path	   *subpath = (Path *) lfirst(lc);
		List	   *pathkeys;

		/* Convert subpath's pathkeys to outer representation */
		pathkeys = convert_subquery_pathkeys(root,
											 rel,
											 subpath->pathkeys,
											 make_tlist_from_pathtarget(subpath->pathtarget));

		/* Generate outer path using this subpath */
		add_path(rel, (Path *)
				 create_subqueryscan_path(root, rel, subpath,
										  pathkeys, required_outer));
	}

	/* If outer rel allows parallelism, do same for partial paths. */
	if (rel->consider_parallel && bms_is_empty(required_outer))
	{
		/* If consider_parallel is false, there should be no partial paths. */
		Assert(sub_final_rel->consider_parallel ||
			   sub_final_rel->partial_pathlist == NIL);

		/* Same for partial paths. */
		foreach(lc, sub_final_rel->partial_pathlist)
		{
			Path	   *subpath = (Path *) lfirst(lc);
			List	   *pathkeys;

			/* Convert subpath's pathkeys to outer representation */
			pathkeys = convert_subquery_pathkeys(root,
												 rel,
												 subpath->pathkeys,
												 make_tlist_from_pathtarget(subpath->pathtarget));

			/* Generate outer path using this subpath */
			add_partial_path(rel, (Path *)
							 create_subqueryscan_path(root, rel, subpath,
													  pathkeys,
													  required_outer));
		}
	}
}

/*
 * set_function_pathlist
 *      为函数RTE（范围表条目）构建访问路径
 *
 * 参数说明：
 * - root: 规划器信息结构体指针，包含查询规划的全局上下文
 * - rel: 关系优化信息结构体指针，代表要处理的关系
 * - rte: 范围表条目，包含函数相关信息
 *
 * 函数功能：
 * 为函数调用（如返回行集的函数）生成单一的访问路径，并将该路径添加到关系的路径列表中。
 * 特别处理了带有ORDINALITY修饰符的函数，确保正确处理其排序特性。
 */
static void
set_function_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
    Relids      required_outer;   /* 需要的外部关系ID集合 */
    List       *pathkeys = NIL;   /* 排序键列表，默认为空（无序） */

    /*
     * 函数扫描不支持将连接条件下推到其条件中，
     * 但由于函数表达式中可能存在LATERAL引用，因此仍然可能需要参数化。
     * LATERAL关键字允许函数引用FROM子句中前面表的列值。
     */
    required_outer = rel->lateral_relids;   /* 从关系中获取所需的外部关系ID */

    /*
     * 函数结果默认被视为无序，除非使用了ORDINALITY修饰符。
     * 使用ORDINALITY时，结果集按序号列（最后一列）排序。
     * 通过检查该Var是否存在于等价类中来确定是否需要关注这个排序。
     */
    if (rte->funcordinality)   /* 检查函数是否使用了ORDINALITY修饰符 */
    {
        AttrNumber  ordattno = rel->max_attr;   /* 序号列的属性号（总是最后一列） */
        Var         *var = NULL;                /* 指向序号列Var节点的指针 */
        ListCell   *lc;

        /*
         * 检查关系的目标列表中是否包含该序号列的Var引用。
         * 如果不存在，则表明查询未引用该序号列，或至少未以排序相关的方式引用。
         */
        foreach(lc, rel->reltarget->exprs)
        {
            Var         *node = (Var *) lfirst(lc);

            /* 检查节点类型和属性，确认是否是我们要找的序号列Var */
            /* varno/varlevelsup的检查是为了额外的安全验证 */
            if (IsA(node, Var) &&
                node->varattno == ordattno &&
                node->varno == rel->relid &&
                node->varlevelsup == 0)
            {
                var = node;   /* 找到了序号列的Var引用 */
                break;
            }
        }

        /*
         * 尝试使用int8类型的排序操作符为该Var构建pathkeys。
         * 我们告知build_expression_pathkey不要构建新的等价类；
         * 如果Var未在任何等价类中提及，则表明没有任何地方关心这个排序。
         */
        if (var)   /* 如果找到了序号列Var */
            pathkeys = build_expression_pathkey(root,
                                                (Expr *) var,         /* 要构建排序键的表达式 */
                                                NULL,                 /* 外层连接下方 */
                                                Int8LessOperator,     /* 使用int8的小于操作符进行排序 */
                                                rel->relids,          /* 表达式涉及的关系ID */
                                                false);               /* 不创建新的等价类 */
    }

    /* 生成适当的访问路径 */
    add_path(rel, create_functionscan_path(root, rel,
                                           pathkeys, required_outer));
    /*
     * create_functionscan_path创建函数扫描路径节点，传入排序键和所需外部关系
     * add_path将创建的路径添加到关系的路径列表中
     */
}


/*
 * set_values_pathlist
 *    为VALUES表达式构建单一访问路径
 *    
 * 这个函数为SQL查询中的VALUES表达式（如SELECT * FROM (VALUES (1,2), (3,4)) AS t(a,b)）
 * 构建对应的访问路径。VALUES表达式作为一种特殊的关系类型，在执行计划中需要
 * 被表示为一个具体的扫描路径节点。
 *
 * 参数说明：
 * 'root' - 规划器信息结构体指针，包含查询相关的全局信息
 * 'rel' - 要处理的关系（RelOptInfo结构体指针），表示当前VALUES表达式对应的关系
 * 'rte' - 范围表项（RangeTblEntry结构体指针），包含VALUES表达式的原始信息
 *
 * 功能说明：
 * 此函数为VALUES表达式生成唯一的访问路径（ValuesScanPath），并将其添加到关系的路径列表中。
 * 与表扫描不同，VALUES表达式只有一种访问方式，因此只生成一个路径。
 */
static void
set_values_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
    Relids    required_outer;  // 存储VALUES表达式所需的外部关系ID集合

    /*
     * 我们不支持将连接条件下推到VALUES扫描的条件中，
     * 但由于VALUES表达式中可能包含LATERAL引用（引用外侧查询的列），
     * 因此它仍然可能需要参数化。
     * lateral_relids包含了此VALUES表达式依赖的外部关系ID
     */
    required_outer = rel->lateral_relids;

    /* 生成适当的访问路径并添加到关系的路径列表中 */
    add_path(rel, create_valuesscan_path(root, rel, required_outer));
}


/*
 * set_tablefunc_pathlist
 *      为表函数RTE（范围表条目）构建访问路径
 *
 * 参数说明：
 * - root: 规划器信息结构体指针，包含查询规划的全局上下文
 * - rel: 关系优化信息结构体指针，代表要处理的关系
 * - rte: 范围表条目，包含表函数相关信息
 *
 * 函数功能：
 * 为表函数（如unnest()、generate_series()等返回结果集的函数）生成单一的访问路径
 * 并将该路径添加到关系的路径列表中。表函数在查询计划中被视为特殊类型的关系。
 */
static void
set_tablefunc_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
    Relids      required_outer;   /* 需要的外部关系ID集合 */

    /*
     * 表函数扫描不支持将连接条件下推到其条件中，
     * 但由于函数表达式中可能存在LATERAL引用，因此仍然可能需要参数化。
     * LATERAL关键字允许函数引用FROM子句中前面表的列值。
     */
    required_outer = rel->lateral_relids;   /* 从关系中获取所需的外部关系ID */

    /* 生成适当的访问路径 */
    add_path(rel, create_tablefuncscan_path(root, rel,
                                            required_outer));
    /*
     * create_tablefuncscan_path创建表函数扫描路径节点
     * add_path将创建的路径添加到关系的路径列表中
     */
}



/*
 * set_cte_pathlist
 *      为非自引用的CTE（公共表表达式）范围表条目(RTE)构建单一的访问路径
 *      
 * 说明：
 *      对于CTE，不需要单独的set_cte_size阶段，因为PostgreSQL不支持对CTE使用
 *      连接条件参数化的路径。
 *      
 * 参数说明：
 *      root - 规划器全局信息结构体指针，包含查询优化过程中的所有上下文信息
 *      rel - 关系优化信息结构体指针，表示当前需要设置访问路径的CTE关系
 *      rte - 范围表条目指针，包含CTE的元数据信息
 *
 * 返回值：
 *      void - 无返回值，直接修改传入的rel结构体
 *
 * 功能说明：
 *      此函数负责为CTE构建访问路径，主要通过定位已预先规划好的CTE执行计划，
 *      设置必要的大小估计信息，并创建CTE扫描路径。在PostgreSQL查询优化器中，
 *      CTE被视为物化的中间结果，所以只需要构建单一的扫描路径。
 */
static void
set_cte_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
    Plan	   		*cteplan;           /* CTE的执行计划 */
    PlannerInfo 	*cteroot;           /* CTE所属的规划器信息结构体 */
    Index			levelsup;          	/* CTE在嵌套层级中的深度 */
    int				ndx;               	/* CTE在列表中的索引 */
    ListCell   		*lc;                /* 列表遍历指针 */ 
    int				plan_id;          	/* CTE计划的ID */
    Relids			required_outer;    	/* 必需的外部关系ID集合 */

    /*
     * 查找被引用的CTE，并定位之前为其生成的执行计划
     */
    levelsup = rte->ctelevelsup;    /* 获取CTE嵌套的层级深度 */
    cteroot = root;                 /* 从当前规划器信息开始 */
    
    /* 沿着父规划器链向上查找，直到找到CTE定义所在的层级 */
    while (levelsup-- > 0)
    {
        cteroot = cteroot->parent_root;
        if (!cteroot)               /* 这种情况理论上不应该发生 */
            elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
    }

    /*
     * 注意：当我们仍在处理CTE规划时（例如，这是来自另一个CTE的引用），
     * cte_plan_ids列表可能比cteList短。因此，我们不能使用forboth来遍历这两个列表。
     */
    ndx = 0;
    
    /* 遍历CTE列表，查找与当前RTE名称匹配的CTE */
    foreach(lc, cteroot->parse->cteList)
    {
        CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

        if (strcmp(cte->ctename, rte->ctename) == 0)
            break;                  /* 找到匹配的CTE */
        ndx++;                      /* 递增索引，继续查找 */
    }
    
    /* 进行各种错误检查，确保CTE定义和计划存在 */
    if (lc == NULL)                 /* 找不到CTE定义（不应该发生） */
        elog(ERROR, "could not find CTE \"%s\"", rte->ctename);
    
    if (ndx >= list_length(cteroot->cte_plan_ids))  /* 找不到CTE计划ID（不应该发生） */
        elog(ERROR, "could not find plan for CTE \"%s\"", rte->ctename);
    
    /* 获取CTE计划ID */
    plan_id = list_nth_int(cteroot->cte_plan_ids, ndx);
    
    if (plan_id <= 0)               /* 计划ID无效（不应该发生） */
        elog(ERROR, "no plan was made for CTE \"%s\"", rte->ctename);
    
    /* 根据计划ID从全局子计划列表中获取CTE的执行计划 */
    cteplan = (Plan *) list_nth(root->glob->subplans, plan_id - 1);

    /*
     * 设置关系的估计输出行数、宽度等统计信息
     * 使用CTE计划中已计算好的行数估计值
     */
    set_cte_size_estimates(root, rel, cteplan->plan_rows);

    /*
     * 虽然PostgreSQL不支持将连接条件下推到CTE扫描的条件中，
     * 但由于CTE的目标列表中可能包含对外部关系的LATERAL引用，
     * 因此CTE扫描仍可能需要参数化
     */
    required_outer = rel->lateral_relids;    /* 获取必需的外部关系ID集合 */

    /* 生成适当的访问路径并添加到关系的路径列表中 */
    add_path(rel, create_ctescan_path(root, rel, required_outer));
}


/*
 * set_namedtuplestore_pathlist
 *      为命名元组存储(named tuplestore)类型的范围表条目构建访问路径
 *
 * 说明：
 * 不需要单独的set_namedtuplestore_size阶段，因为我们不支持为元组存储设置连接条件参数化的路径。
 *
 * 参数说明：
 * - root: 规划器信息结构体指针，包含查询规划的全局上下文
 * - rel: 关系优化信息结构体指针，代表要处理的关系
 * - rte: 范围表条目，具体是命名元组存储类型（如WITH子句中的临时结果集）
 *
 * 函数功能：
 * 为命名元组存储（如WITH子句中定义的公共表表达式CTE）生成单一的访问路径，
 * 设置估计的输出行数和宽度，并将路径添加到关系的路径列表中。
 */
static void
set_namedtuplestore_pathlist(PlannerInfo *root, RelOptInfo *rel,
                             RangeTblEntry *rte)
{
    Relids      required_outer;   /* 需要的外部关系ID集合 */

    /* 设置关系的估计输出行数、宽度等统计信息 */
    set_namedtuplestore_size_estimates(root, rel);

    /*
     * 元组存储扫描不支持将连接条件下推到其条件中，
     * 但由于其目标列表中可能存在LATERAL引用，因此仍然可能需要参数化。
     */
    required_outer = rel->lateral_relids;   /* 从关系中获取所需的外部关系ID */

    /* 生成适当的访问路径 */
    add_path(rel, create_namedtuplestorescan_path(root, rel, required_outer));
    /*
     * create_namedtuplestorescan_path创建命名元组存储扫描路径节点
     * add_path将创建的路径添加到关系的路径列表中
     */

    /* 选择最便宜的路径（在这种情况下很简单，因为只有一条路径） */
    set_cheapest(rel);
}


/*
 * set_result_pathlist
 *      为RTE_RESULT类型的范围表条目构建访问路径
 *
 * 说明：
 * 不需要单独的set_result_size阶段，因为我们不支持为这些RTE设置连接条件参数化的路径。
 *
 * 参数说明：
 * - root: 规划器信息结构体指针，包含查询规划的全局上下文
 * - rel: 关系优化信息结构体指针，代表要处理的关系
 * - rte: 范围表条目，具体是RTE_RESULT类型（表示如VALUES子句等结果集）
 *
 * 函数功能：
 * 为结果集关系（如VALUES子句、没有FROM子句的SELECT查询等）生成单一的访问路径，
 * 设置估计的输出行数和宽度，并将路径添加到关系的路径列表中。
 */
static void
set_result_pathlist(PlannerInfo *root, RelOptInfo *rel,
                    RangeTblEntry *rte)
{
    Relids      required_outer;   /* 需要的外部关系ID集合 */

    /* 设置关系的估计输出行数、宽度等统计信息 */
    set_result_size_estimates(root, rel);

    /*
     * 结果扫描不支持将连接条件下推到其条件中，
     * 但由于其目标列表中可能存在LATERAL引用，因此仍然可能需要参数化。
     */
    required_outer = rel->lateral_relids;   /* 从关系中获取所需的外部关系ID */

    /* 生成适当的访问路径 */
    add_path(rel, create_resultscan_path(root, rel, required_outer));
    /*
     * create_resultscan_path创建结果扫描路径节点
     * add_path将创建的路径添加到关系的路径列表中
     */

    /* 选择最便宜的路径（在这种情况下很简单，因为只有一条路径） */
    set_cheapest(rel);
}


/*
 * set_worktable_pathlist
 *      为自引用CTE（Common Table Expression）范围表项构建访问路径
 *      此函数构建单个访问路径，用于处理自引用的CTE
 *
 * 由于不支持对CTE使用基于连接条件的参数化路径，因此不需要单独的set_worktable_size阶段。
 */
static void
set_worktable_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
    Path       *ctepath;          	/* CTE的非递归部分路径 */
    PlannerInfo *cteroot;         	/* CTE所在层次的规划器信息 */
    Index       levelsup;          	/* CTE层次深度 */
    Relids      required_outer;    	/* 所需的外部关系ID集合 */

    /*
     * 需要找到非递归项的路径，它位于处理递归UNION的计划层次中，
     * 这个层次比CTE来源的层次低一级。
     */
    levelsup = rte->ctelevelsup;  /* 获取CTE相对于当前查询的层次深度 */
    if (levelsup == 0)            /* 不应该发生的情况 */
        elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
    levelsup--;                   /* 减1以指向递归UNION的层次 */
    cteroot = root;
    while (levelsup-- > 0)        /* 遍历找到对应的父规划器层次 */
    {
        cteroot = cteroot->parent_root;
        if (!cteroot)              /* 不应该发生的情况 */
            elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
    }
    ctepath = cteroot->non_recursive_path;  /* 获取非递归路径 */
    if (!ctepath)                 /* 不应该发生的情况 */
        elog(ERROR, "could not find path for CTE \"%s\"", rte->ctename);

    /* 为关系设置估计的输出行数、宽度等统计信息 */
    set_cte_size_estimates(root, rel, ctepath->rows);

    /*
     * 不支持将连接条件推入工作表扫描的条件中，但由于目标列表中的LATERAL引用，
     * 它仍然可能需要参数化。
     * （考虑到递归引用的限制，我不确定这是否实际可行，但支持起来很简单。）
     */
    required_outer = rel->lateral_relids;  /* 设置所需的外部关系 */

    /* 生成适当的访问路径并添加到关系的路径列表中 */
    add_path(rel, create_worktablescan_path(root, rel, required_outer));
}


/*
 * generate_gather_paths
 *    为关系生成并行访问路径，通过在部分路径(partial path)上添加Gather或Gather Merge操作。
 *    该函数是PostgreSQL并行查询优化的核心组件，负责将并行工作进程生成的中间结果
 *    整合为最终的查询结果。
 *
 * 注意事项：
 *    - 必须在为指定关系创建完所有部分路径(partial paths)之后调用此函数
 *    - 否则，add_partial_path可能会删除被GatherPath或GatherMergePath引用的路径
 *
 * 行数估计处理逻辑：
 *    - 当为扫描或连接关系生成路径时，override_rows为false，直接使用关系的大小估计
 *    - 当为部分分组路径(partially-grouped path)调用时，需要覆盖行数估计值
 *    - 当前使用的特定值可能不是最佳选择，但底层关系没有估计值，必须提供一个合理值
 */
void
generate_gather_paths(PlannerInfo *root, RelOptInfo *rel, bool override_rows)
{
    /* 声明所需变量 */
    Path       *cheapest_partial_path;  /* 成本最低的部分路径 */
    Path       *simple_gather_path;     /* 普通Gather路径 */
    ListCell   *lc;                     /* 遍历列表的指针 */
    double      rows;                   /* 行数估计值 */
    double     *rowsp = NULL;           /* 指向行数估计值的指针，用于覆盖估计 */

    /* 如果没有部分路径，则无需处理 */
    if (rel->partial_pathlist == NIL)
        return;

    /* 确定是否需要覆盖行数估计 */
    if (override_rows)
        rowsp = &rows;

    /*
     * Gather操作的输出总是未排序的，因此只需要考虑一个部分路径：成本最低的那个。
     * 由于add_partial_path的工作方式，成本最低的路径位于partial_pathlist的头部。
     */
    cheapest_partial_path = linitial(rel->partial_pathlist);
    /* 计算总估计行数：工作进程数乘以单个工作进程处理的行数 */
    rows = cheapest_partial_path->rows * cheapest_partial_path->parallel_workers;
    /* 创建普通Gather路径 */
    simple_gather_path = (Path *)
        create_gather_path(root, rel, cheapest_partial_path, rel->reltarget,
                          NULL, rowsp);
    /* 将创建的路径添加到关系的路径列表中 */
    add_path(rel, simple_gather_path);

    /*
     * 对于每个有用的排序顺序，我们可以考虑使用保持顺序的Gather Merge操作。
     * Gather Merge允许合并来自多个工作进程的已排序结果，保持整体有序性。
     */
    foreach(lc, rel->partial_pathlist)
    {
        Path       *subpath = (Path *) lfirst(lc);  /* 当前遍历的部分路径 */
        GatherMergePath *path;                      /* Gather Merge路径 */

        /* 跳过没有排序键的路径，因为它们不适合Gather Merge */
        if (subpath->pathkeys == NIL)
            continue;

        /* 计算总估计行数 */
        rows = subpath->rows * subpath->parallel_workers;
        /* 创建Gather Merge路径，保持子路径的排序顺序 */
        path = create_gather_merge_path(root, rel, subpath, rel->reltarget,
                                      subpath->pathkeys, NULL, rowsp);
        /* 将Gather Merge路径添加到关系的路径列表中 */
        add_path(rel, &path->path);
    }
}


/*
 * make_rel_from_joinlist
 *	  使用 "joinlist" 指导连接路径搜索，构建访问路径。
 *    'joinlist' 中可能存在RangeTblRef节点（表示基表）或嵌套的 joinlist 节点，
 *	  不一定是完全拉平的。
 *
 * 参见 deconstruct_jointree() 的注释，了解 joinlist 数据结构的定义。
 */
static RelOptInfo *
make_rel_from_joinlist(PlannerInfo *root, List *joinlist)
{
	int			levels_needed;
	List* 		initial_rels;	/* joinlist 中每个节点对应的 RelOptInfo 列表 */
	ListCell* 	jl;				/* 用于遍历 joinlist 的辅助指针 */

	/*
	 * 统计 joinlist 子节点的数量。这是动态规划算法需要的深度，
	 * 用于考虑所有可能的连接方式。
	 * 基表的数量等于 joinlist 中 RangeTblRef 节点的数量，
	 * levels_needed 则是 joinlist 中节点的总数量（包括基表和子 joinlist）。
	 */
	levels_needed = list_length(joinlist);

	if (levels_needed <= 0)
		return NULL;			/* 没有要处理的内容？ */

	/*
	 * 构造与 joinlist 子节点对应的 rels 列表。
	 * 其中可能包含基表 rel 和根据子 joinlist 构造的 rel。
	 * 递归处理未拉平的子 joinlist 节点。
	 */
	initial_rels = NIL;
	foreach(jl, joinlist)
	{
		Node	   *jlnode = (Node *) lfirst(jl);	/* joinlist 中的当前节点 */
		RelOptInfo *thisrel;						/* 当前节点对应的 RelOptInfo */

		/*
		 * 处理连接列表（joinlist）中的节点，根据节点类型执行不同的操作：
		 * - 如果节点类型为 RangeTblRef，则通过 rtindex 查找对应的基本关系（base rel）对应的 RelOptInfo。
		 * - 如果节点类型为 List，则递归处理子问题，生成对应的关系信息。
		 * - 如果节点类型无法识别，则报错并返回 NULL（防止编译器警告）。
		 */
		if (IsA(jlnode, RangeTblRef))
		{
			int			varno = ((RangeTblRef *) jlnode)->rtindex;

			thisrel = find_base_rel(root, varno);
		}
		else if (IsA(jlnode, List))
		{
			/* 递归处理子问题 */
			thisrel = make_rel_from_joinlist(root, (List *) jlnode);
		}
		else
		{
			elog(ERROR, "unrecognized joinlist node type: %d",
				 (int) nodeTag(jlnode));
			thisrel = NULL;		/* 防止编译器警告 */
		}

		/* 将当前 joinlist 节点对应的 RelOptInfo 添加到初始关系列表中 */
		initial_rels = lappend(initial_rels, thisrel);
	}

	/*
	 * 如果 joinlist 长度，说明只有一个基表或子 joinlist，直接返回对应的 RelOptInfo。
	 * 否则，使用连接搜索算法（插件、GEQO 或标准动态规划方法）来考虑不同的连接顺序。
	 */
	if (levels_needed == 1)
	{
		/*
		 * 只有一个 joinlist 节点，直接返回。
		 */
		return (RelOptInfo *) linitial(initial_rels);
	}
	else
	{
		/*
		 * 多个 joinlist 节点，使用连接搜索算法考虑不同的连接顺序。
		 *
		 * 使用插件、GEQO 或常规连接搜索代码，考虑不同的连接顺序。
		 *
		 * 将 initial_rels 列表存入 PlannerInfo 字段，因为
		 * has_legal_joinclause() 需要访问它（有点丑陋 :-()。
		 */

		/* 将 initial_rels 列表存入 PlannerInfo 字段，该字段存储基表的链表 */
		root->initial_rels = initial_rels;

		if (join_search_hook)
			return (*join_search_hook) (root, levels_needed, initial_rels);
		else if (enable_geqo && levels_needed >= geqo_threshold)
			return geqo(root, levels_needed, initial_rels);
		else
			return standard_join_search(root, levels_needed, initial_rels);
	}
}

/*
 * standard_join_search
 *	  通过逐步将组件关系连接成连接关系，为查询查找可能的连接路径。
 *
 * 'levels_needed' 是所需的迭代次数，即查询中独立 jointree 项的数量。该值 > 1。
 *
 * 'initial_rels' 是每个独立 jointree 项对应的基表的 RelOptInfo 节点列表。这些是需要连接的组件。
 *		注意 levels_needed == list_length(initial_rels)。
 *
 * 返回最终级别的连接关系，即所有原始关系连接后的结果关系。
 * 必须为该关系及所有需要的子关系提供至少一种实现路径。
 *
 * 为了支持通过更改连接搜索算法来修改规划器行为的可加载插件，我们提供了一个钩子变量，
 * 允许插件替换或补充此函数。任何这样的钩子必须返回与标准代码相同的最终连接关系，
 * 但其附加的实现路径集合可能不同，并且只需实例化这些路径所需的子连接关系。
 *
 * 给插件作者的说明：standard_join_search() 调用的函数会修改 root->join_rel_list 和 root->join_rel_hash。
 * 如果你想进行多次连接顺序搜索，可能需要保存和恢复这些数据结构的原始状态。可参考 geqo_eval() 的实现。
 */
RelOptInfo *
standard_join_search(PlannerInfo *root, int levels_needed, List *initial_rels)
{
	int			lev; /* 当前处理的连接层级，辅助遍历 */
	RelOptInfo *rel;

	/*
	 * 此函数在同一个规划问题中不能递归调用，因此 join_rel_level[] 不应已被使用。
	 */
	Assert(root->join_rel_level == NULL);

	/*
	 * 采用简单的“动态规划”算法：首先找到所有两项连接的方式，然后找到三项连接的所有方式
	 * （由两项连接和单项组成），然后是四项连接，依此类推，直到考虑所有将所有项连接成一个关系的方法。
	 *
	 * root->join_rel_level[j] 是所有 j 项连接关系的列表。最初我们将 root->join_rel_level[1]
	 * 设置为所有单 jointree 项的关系。
	 *
	 * 创建一个链表的链表，多创建一个位置
	 * 将基表的链表放到下表为1的位置，root->join_rel_level[0] 永远不会使用
	 */
	root->join_rel_level = (List **) palloc0((levels_needed + 1) * sizeof(List *));
	root->join_rel_level[1] = initial_rels;

	// 第一层已经初始化好，从第二层开始
	for (lev = 2; lev <= levels_needed; lev++)
	{
		ListCell   *lc;

		/*
		 * 确定本级别所有可能的关系对，并为每个可用的低级别关系对构建连接路径。
		 * 生成对应 lev 层的所有对应 RelOptInfo
		 */
		join_search_one_level(root, lev);

		/*
		 * 对刚处理过的每个 joinrel 运行 generate_partitionwise_join_paths() 和 generate_gather_paths()。
		 * 之前不能做这些，因为常规路径和部分路径可能在 join_search_one_level 内多次添加到某个 joinrel。
		 *
		 * 此后，joinrel 的路径创建完成，因此运行 set_cheapest()。
		 */
		foreach(lc, root->join_rel_level[lev])
		{
			rel = (RelOptInfo *) lfirst(lc);

			/* 为分区连接创建路径。 */
			generate_partitionwise_join_paths(root, rel);

			/*
			 * 除了最顶层的扫描/连接关系外，考虑收集部分路径。对于最顶层的扫描/连接关系，
			 * 会在确定最终目标列后再做（见 grouping_planner）。
			 */
			if (lev < levels_needed)
				generate_gather_paths(root, rel, false);

			/* 查找并保存该关系的最优路径 */
			set_cheapest(rel);

#ifdef OPTIMIZER_DEBUG
			debug_print_rel(root, rel);
#endif
		}
	}

	/*
	 * 最终级别应只有一个关系。
	 */
	if (root->join_rel_level[levels_needed] == NIL)
		elog(ERROR, "failed to build any %d-way joins", levels_needed);
	Assert(list_length(root->join_rel_level[levels_needed]) == 1);

	rel = (RelOptInfo *) linitial(root->join_rel_level[levels_needed]);

	root->join_rel_level = NULL;

	return rel;
}

/*****************************************************************************
 *			PUSHING QUALS DOWN INTO SUBQUERIES
 *****************************************************************************/

/*
 * subquery_is_pushdown_safe - is a subquery safe for pushing down quals?
 *
 * subquery is the particular component query being checked.  topquery
 * is the top component of a set-operations tree (the same Query if no
 * set-op is involved).
 *
 * Conditions checked here:
 *
 * 1. If the subquery has a LIMIT clause, we must not push down any quals,
 * since that could change the set of rows returned.
 *
 * 2. If the subquery contains EXCEPT or EXCEPT ALL set ops we cannot push
 * quals into it, because that could change the results.
 *
 * 3. If the subquery uses DISTINCT, we cannot push volatile quals into it.
 * This is because upper-level quals should semantically be evaluated only
 * once per distinct row, not once per original row, and if the qual is
 * volatile then extra evaluations could change the results.  (This issue
 * does not apply to other forms of aggregation such as GROUP BY, because
 * when those are present we push into HAVING not WHERE, so that the quals
 * are still applied after aggregation.)
 *
 * 4. If the subquery contains window functions, we cannot push volatile quals
 * into it.  The issue here is a bit different from DISTINCT: a volatile qual
 * might succeed for some rows of a window partition and fail for others,
 * thereby changing the partition contents and thus the window functions'
 * results for rows that remain.
 *
 * 5. If the subquery contains any set-returning functions in its targetlist,
 * we cannot push volatile quals into it.  That would push them below the SRFs
 * and thereby change the number of times they are evaluated.  Also, a
 * volatile qual could succeed for some SRF output rows and fail for others,
 * a behavior that cannot occur if it's evaluated before SRF expansion.
 *
 * 6. If the subquery has nonempty grouping sets, we cannot push down any
 * quals.  The concern here is that a qual referencing a "constant" grouping
 * column could get constant-folded, which would be improper because the value
 * is potentially nullable by grouping-set expansion.  This restriction could
 * be removed if we had a parsetree representation that shows that such
 * grouping columns are not really constant.  (There are other ideas that
 * could be used to relax this restriction, but that's the approach most
 * likely to get taken in the future.  Note that there's not much to be gained
 * so long as subquery_planner can't move HAVING clauses to WHERE within such
 * a subquery.)
 *
 * In addition, we make several checks on the subquery's output columns to see
 * if it is safe to reference them in pushed-down quals.  If output column k
 * is found to be unsafe to reference, we set safetyInfo->unsafeColumns[k]
 * to true, but we don't reject the subquery overall since column k might not
 * be referenced by some/all quals.  The unsafeColumns[] array will be
 * consulted later by qual_is_pushdown_safe().  It's better to do it this way
 * than to make the checks directly in qual_is_pushdown_safe(), because when
 * the subquery involves set operations we have to check the output
 * expressions in each arm of the set op.
 *
 * Note: pushing quals into a DISTINCT subquery is theoretically dubious:
 * we're effectively assuming that the quals cannot distinguish values that
 * the DISTINCT's equality operator sees as equal, yet there are many
 * counterexamples to that assumption.  However use of such a qual with a
 * DISTINCT subquery would be unsafe anyway, since there's no guarantee which
 * "equal" value will be chosen as the output value by the DISTINCT operation.
 * So we don't worry too much about that.  Another objection is that if the
 * qual is expensive to evaluate, running it for each original row might cost
 * more than we save by eliminating rows before the DISTINCT step.  But it
 * would be very hard to estimate that at this stage, and in practice pushdown
 * seldom seems to make things worse, so we ignore that problem too.
 *
 * Note: likewise, pushing quals into a subquery with window functions is a
 * bit dubious: the quals might remove some rows of a window partition while
 * leaving others, causing changes in the window functions' results for the
 * surviving rows.  We insist that such a qual reference only partitioning
 * columns, but again that only protects us if the qual does not distinguish
 * values that the partitioning equality operator sees as equal.  The risks
 * here are perhaps larger than for DISTINCT, since no de-duplication of rows
 * occurs and thus there is no theoretical problem with such a qual.  But
 * we'll do this anyway because the potential performance benefits are very
 * large, and we've seen no field complaints about the longstanding comparable
 * behavior with DISTINCT.
 */
static bool
subquery_is_pushdown_safe(Query *subquery, Query *topquery,
						  pushdown_safety_info *safetyInfo)
{
	SetOperationStmt *topop;

	/* Check point 1 */
	if (subquery->limitOffset != NULL || subquery->limitCount != NULL)
		return false;

	/* Check point 6 */
	if (subquery->groupClause && subquery->groupingSets)
		return false;

	/* Check points 3, 4, and 5 */
	if (subquery->distinctClause ||
		subquery->hasWindowFuncs ||
		subquery->hasTargetSRFs)
		safetyInfo->unsafeVolatile = true;

	/*
	 * If we're at a leaf query, check for unsafe expressions in its target
	 * list, and mark any unsafe ones in unsafeColumns[].  (Non-leaf nodes in
	 * setop trees have only simple Vars in their tlists, so no need to check
	 * them.)
	 */
	if (subquery->setOperations == NULL)
		check_output_expressions(subquery, safetyInfo);

	/* Are we at top level, or looking at a setop component? */
	if (subquery == topquery)
	{
		/* Top level, so check any component queries */
		if (subquery->setOperations != NULL)
			if (!recurse_pushdown_safe(subquery->setOperations, topquery,
									   safetyInfo))
				return false;
	}
	else
	{
		/* Setop component must not have more components (too weird) */
		if (subquery->setOperations != NULL)
			return false;
		/* Check whether setop component output types match top level */
		topop = castNode(SetOperationStmt, topquery->setOperations);
		Assert(topop);
		compare_tlist_datatypes(subquery->targetList,
								topop->colTypes,
								safetyInfo);
	}
	return true;
}

/*
 * Helper routine to recurse through setOperations tree
 */
static bool
recurse_pushdown_safe(Node *setOp, Query *topquery,
					  pushdown_safety_info *safetyInfo)
{
	if (IsA(setOp, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) setOp;
		RangeTblEntry *rte = rt_fetch(rtr->rtindex, topquery->rtable);
		Query	   *subquery = rte->subquery;

		Assert(subquery != NULL);
		return subquery_is_pushdown_safe(subquery, topquery, safetyInfo);
	}
	else if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;

		/* EXCEPT is no good (point 2 for subquery_is_pushdown_safe) */
		if (op->op == SETOP_EXCEPT)
			return false;
		/* Else recurse */
		if (!recurse_pushdown_safe(op->larg, topquery, safetyInfo))
			return false;
		if (!recurse_pushdown_safe(op->rarg, topquery, safetyInfo))
			return false;
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(setOp));
	}
	return true;
}

/*
 * check_output_expressions - check subquery's output expressions for safety
 *
 * There are several cases in which it's unsafe to push down an upper-level
 * qual if it references a particular output column of a subquery.  We check
 * each output column of the subquery and set unsafeColumns[k] to true if
 * that column is unsafe for a pushed-down qual to reference.  The conditions
 * checked here are:
 *
 * 1. We must not push down any quals that refer to subselect outputs that
 * return sets, else we'd introduce functions-returning-sets into the
 * subquery's WHERE/HAVING quals.
 *
 * 2. We must not push down any quals that refer to subselect outputs that
 * contain volatile functions, for fear of introducing strange results due
 * to multiple evaluation of a volatile function.
 *
 * 3. If the subquery uses DISTINCT ON, we must not push down any quals that
 * refer to non-DISTINCT output columns, because that could change the set
 * of rows returned.  (This condition is vacuous for DISTINCT, because then
 * there are no non-DISTINCT output columns, so we needn't check.  Note that
 * subquery_is_pushdown_safe already reported that we can't use volatile
 * quals if there's DISTINCT or DISTINCT ON.)
 *
 * 4. If the subquery has any window functions, we must not push down quals
 * that reference any output columns that are not listed in all the subquery's
 * window PARTITION BY clauses.  We can push down quals that use only
 * partitioning columns because they should succeed or fail identically for
 * every row of any one window partition, and totally excluding some
 * partitions will not change a window function's results for remaining
 * partitions.  (Again, this also requires nonvolatile quals, but
 * subquery_is_pushdown_safe handles that.)
 */
static void
check_output_expressions(Query *subquery, pushdown_safety_info *safetyInfo)
{
	ListCell   *lc;

	foreach(lc, subquery->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		if (tle->resjunk)
			continue;			/* ignore resjunk columns */

		/* We need not check further if output col is already known unsafe */
		if (safetyInfo->unsafeColumns[tle->resno])
			continue;

		/* Functions returning sets are unsafe (point 1) */
		if (subquery->hasTargetSRFs &&
			expression_returns_set((Node *) tle->expr))
		{
			safetyInfo->unsafeColumns[tle->resno] = true;
			continue;
		}

		/* Volatile functions are unsafe (point 2) */
		if (contain_volatile_functions((Node *) tle->expr))
		{
			safetyInfo->unsafeColumns[tle->resno] = true;
			continue;
		}

		/* If subquery uses DISTINCT ON, check point 3 */
		if (subquery->hasDistinctOn &&
			!targetIsInSortList(tle, InvalidOid, subquery->distinctClause))
		{
			/* non-DISTINCT column, so mark it unsafe */
			safetyInfo->unsafeColumns[tle->resno] = true;
			continue;
		}

		/* If subquery uses window functions, check point 4 */
		if (subquery->hasWindowFuncs &&
			!targetIsInAllPartitionLists(tle, subquery))
		{
			/* not present in all PARTITION BY clauses, so mark it unsafe */
			safetyInfo->unsafeColumns[tle->resno] = true;
			continue;
		}
	}
}

/*
 * For subqueries using UNION/UNION ALL/INTERSECT/INTERSECT ALL, we can
 * push quals into each component query, but the quals can only reference
 * subquery columns that suffer no type coercions in the set operation.
 * Otherwise there are possible semantic gotchas.  So, we check the
 * component queries to see if any of them have output types different from
 * the top-level setop outputs.  unsafeColumns[k] is set true if column k
 * has different type in any component.
 *
 * We don't have to care about typmods here: the only allowed difference
 * between set-op input and output typmods is input is a specific typmod
 * and output is -1, and that does not require a coercion.
 *
 * tlist is a subquery tlist.
 * colTypes is an OID list of the top-level setop's output column types.
 * safetyInfo->unsafeColumns[] is the result array.
 */
static void
compare_tlist_datatypes(List *tlist, List *colTypes,
						pushdown_safety_info *safetyInfo)
{
	ListCell   *l;
	ListCell   *colType = list_head(colTypes);

	foreach(l, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->resjunk)
			continue;			/* ignore resjunk columns */
		if (colType == NULL)
			elog(ERROR, "wrong number of tlist entries");
		if (exprType((Node *) tle->expr) != lfirst_oid(colType))
			safetyInfo->unsafeColumns[tle->resno] = true;
		colType = lnext(colType);
	}
	if (colType != NULL)
		elog(ERROR, "wrong number of tlist entries");
}

/*
 * targetIsInAllPartitionLists
 *		True if the TargetEntry is listed in the PARTITION BY clause
 *		of every window defined in the query.
 *
 * It would be safe to ignore windows not actually used by any window
 * function, but it's not easy to get that info at this stage; and it's
 * unlikely to be useful to spend any extra cycles getting it, since
 * unreferenced window definitions are probably infrequent in practice.
 */
static bool
targetIsInAllPartitionLists(TargetEntry *tle, Query *query)
{
	ListCell   *lc;

	foreach(lc, query->windowClause)
	{
		WindowClause *wc = (WindowClause *) lfirst(lc);

		if (!targetIsInSortList(tle, InvalidOid, wc->partitionClause))
			return false;
	}
	return true;
}

/*
 * qual_is_pushdown_safe - is a particular qual safe to push down?
 *
 * qual is a restriction clause applying to the given subquery (whose RTE
 * has index rti in the parent query).
 *
 * Conditions checked here:
 *
 * 1. The qual must not contain any SubPlans (mainly because I'm not sure
 * it will work correctly: SubLinks will already have been transformed into
 * SubPlans in the qual, but not in the subquery).  Note that SubLinks that
 * transform to initplans are safe, and will be accepted here because what
 * we'll see in the qual is just a Param referencing the initplan output.
 *
 * 2. If unsafeVolatile is set, the qual must not contain any volatile
 * functions.
 *
 * 3. If unsafeLeaky is set, the qual must not contain any leaky functions
 * that are passed Var nodes, and therefore might reveal values from the
 * subquery as side effects.
 *
 * 4. The qual must not refer to the whole-row output of the subquery
 * (since there is no easy way to name that within the subquery itself).
 *
 * 5. The qual must not refer to any subquery output columns that were
 * found to be unsafe to reference by subquery_is_pushdown_safe().
 */
static bool
qual_is_pushdown_safe(Query *subquery, Index rti, Node *qual,
					  pushdown_safety_info *safetyInfo)
{
	bool		safe = true;
	List	   *vars;
	ListCell   *vl;

	/* Refuse subselects (point 1) */
	if (contain_subplans(qual))
		return false;

	/* Refuse volatile quals if we found they'd be unsafe (point 2) */
	if (safetyInfo->unsafeVolatile &&
		contain_volatile_functions(qual))
		return false;

	/* Refuse leaky quals if told to (point 3) */
	if (safetyInfo->unsafeLeaky &&
		contain_leaked_vars(qual))
		return false;

	/*
	 * It would be unsafe to push down window function calls, but at least for
	 * the moment we could never see any in a qual anyhow.  (The same applies
	 * to aggregates, which we check for in pull_var_clause below.)
	 */
	Assert(!contain_window_function(qual));

	/*
	 * Examine all Vars used in clause.  Since it's a restriction clause, all
	 * such Vars must refer to subselect output columns ... unless this is
	 * part of a LATERAL subquery, in which case there could be lateral
	 * references.
	 */
	vars = pull_var_clause(qual, PVC_INCLUDE_PLACEHOLDERS);
	foreach(vl, vars)
	{
		Var		   *var = (Var *) lfirst(vl);

		/*
		 * XXX Punt if we find any PlaceHolderVars in the restriction clause.
		 * It's not clear whether a PHV could safely be pushed down, and even
		 * less clear whether such a situation could arise in any cases of
		 * practical interest anyway.  So for the moment, just refuse to push
		 * down.
		 */
		if (!IsA(var, Var))
		{
			safe = false;
			break;
		}

		/*
		 * Punt if we find any lateral references.  It would be safe to push
		 * these down, but we'd have to convert them into outer references,
		 * which subquery_push_qual lacks the infrastructure to do.  The case
		 * arises so seldom that it doesn't seem worth working hard on.
		 */
		if (var->varno != rti)
		{
			safe = false;
			break;
		}

		/* Subqueries have no system columns */
		Assert(var->varattno >= 0);

		/* Check point 4 */
		if (var->varattno == 0)
		{
			safe = false;
			break;
		}

		/* Check point 5 */
		if (safetyInfo->unsafeColumns[var->varattno])
		{
			safe = false;
			break;
		}
	}

	list_free(vars);

	return safe;
}

/*
 * subquery_push_qual - push down a qual that we have determined is safe
 */
static void
subquery_push_qual(Query *subquery, RangeTblEntry *rte, Index rti, Node *qual)
{
	if (subquery->setOperations != NULL)
	{
		/* Recurse to push it separately to each component query */
		recurse_push_qual(subquery->setOperations, subquery,
						  rte, rti, qual);
	}
	else
	{
		/*
		 * We need to replace Vars in the qual (which must refer to outputs of
		 * the subquery) with copies of the subquery's targetlist expressions.
		 * Note that at this point, any uplevel Vars in the qual should have
		 * been replaced with Params, so they need no work.
		 *
		 * This step also ensures that when we are pushing into a setop tree,
		 * each component query gets its own copy of the qual.
		 */
		qual = ReplaceVarsFromTargetList(qual, rti, 0, rte,
										 subquery->targetList,
										 REPLACEVARS_REPORT_ERROR, 0,
										 &subquery->hasSubLinks);

		/*
		 * Now attach the qual to the proper place: normally WHERE, but if the
		 * subquery uses grouping or aggregation, put it in HAVING (since the
		 * qual really refers to the group-result rows).
		 */
		if (subquery->hasAggs || subquery->groupClause || subquery->groupingSets || subquery->havingQual)
			subquery->havingQual = make_and_qual(subquery->havingQual, qual);
		else
			subquery->jointree->quals =
				make_and_qual(subquery->jointree->quals, qual);

		/*
		 * We need not change the subquery's hasAggs or hasSubLinks flags,
		 * since we can't be pushing down any aggregates that weren't there
		 * before, and we don't push down subselects at all.
		 */
	}
}

/*
 * Helper routine to recurse through setOperations tree
 */
static void
recurse_push_qual(Node *setOp, Query *topquery,
				  RangeTblEntry *rte, Index rti, Node *qual)
{
	if (IsA(setOp, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) setOp;
		RangeTblEntry *subrte = rt_fetch(rtr->rtindex, topquery->rtable);
		Query	   *subquery = subrte->subquery;

		Assert(subquery != NULL);
		subquery_push_qual(subquery, rte, rti, qual);
	}
	else if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;

		recurse_push_qual(op->larg, topquery, rte, rti, qual);
		recurse_push_qual(op->rarg, topquery, rte, rti, qual);
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(setOp));
	}
}

/*****************************************************************************
 *			SIMPLIFYING SUBQUERY TARGETLISTS
 *****************************************************************************/

/*
 * remove_unused_subquery_outputs
 *		Remove subquery targetlist items we don't need
 *
 * It's possible, even likely, that the upper query does not read all the
 * output columns of the subquery.  We can remove any such outputs that are
 * not needed by the subquery itself (e.g., as sort/group columns) and do not
 * affect semantics otherwise (e.g., volatile functions can't be removed).
 * This is useful not only because we might be able to remove expensive-to-
 * compute expressions, but because deletion of output columns might allow
 * optimizations such as join removal to occur within the subquery.
 *
 * To avoid affecting column numbering in the targetlist, we don't physically
 * remove unused tlist entries, but rather replace their expressions with NULL
 * constants.  This is implemented by modifying subquery->targetList.
 */
static void
remove_unused_subquery_outputs(Query *subquery, RelOptInfo *rel)
{
	Bitmapset  *attrs_used = NULL;
	ListCell   *lc;

	/*
	 * Do nothing if subquery has UNION/INTERSECT/EXCEPT: in principle we
	 * could update all the child SELECTs' tlists, but it seems not worth the
	 * trouble presently.
	 */
	if (subquery->setOperations)
		return;

	/*
	 * If subquery has regular DISTINCT (not DISTINCT ON), we're wasting our
	 * time: all its output columns must be used in the distinctClause.
	 */
	if (subquery->distinctClause && !subquery->hasDistinctOn)
		return;

	/*
	 * Collect a bitmap of all the output column numbers used by the upper
	 * query.
	 *
	 * Add all the attributes needed for joins or final output.  Note: we must
	 * look at rel's targetlist, not the attr_needed data, because attr_needed
	 * isn't computed for inheritance child rels, cf set_append_rel_size().
	 * (XXX might be worth changing that sometime.)
	 */
	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &attrs_used);

	/* Add all the attributes used by un-pushed-down restriction clauses. */
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		pull_varattnos((Node *) rinfo->clause, rel->relid, &attrs_used);
	}

	/*
	 * If there's a whole-row reference to the subquery, we can't remove
	 * anything.
	 */
	if (bms_is_member(0 - FirstLowInvalidHeapAttributeNumber, attrs_used))
		return;

	/*
	 * Run through the tlist and zap entries we don't need.  It's okay to
	 * modify the tlist items in-place because set_subquery_pathlist made a
	 * copy of the subquery.
	 */
	foreach(lc, subquery->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		Node	   *texpr = (Node *) tle->expr;

		/*
		 * If it has a sortgroupref number, it's used in some sort/group
		 * clause so we'd better not remove it.  Also, don't remove any
		 * resjunk columns, since their reason for being has nothing to do
		 * with anybody reading the subquery's output.  (It's likely that
		 * resjunk columns in a sub-SELECT would always have ressortgroupref
		 * set, but even if they don't, it seems imprudent to remove them.)
		 */
		if (tle->ressortgroupref || tle->resjunk)
			continue;

		/*
		 * If it's used by the upper query, we can't remove it.
		 */
		if (bms_is_member(tle->resno - FirstLowInvalidHeapAttributeNumber,
						  attrs_used))
			continue;

		/*
		 * If it contains a set-returning function, we can't remove it since
		 * that could change the number of rows returned by the subquery.
		 */
		if (subquery->hasTargetSRFs &&
			expression_returns_set(texpr))
			continue;

		/*
		 * If it contains volatile functions, we daren't remove it for fear
		 * that the user is expecting their side-effects to happen.
		 */
		if (contain_volatile_functions(texpr))
			continue;

		/*
		 * OK, we don't need it.  Replace the expression with a NULL constant.
		 * Preserve the exposed type of the expression, in case something
		 * looks at the rowtype of the subquery's result.
		 */
		tle->expr = (Expr *) makeNullConst(exprType(texpr),
										   exprTypmod(texpr),
										   exprCollation(texpr));
	}
}

/*
 * create_partial_bitmap_paths
 *	  Build partial bitmap heap path for the relation
 */
void
create_partial_bitmap_paths(PlannerInfo *root, RelOptInfo *rel,
							Path *bitmapqual)
{
	int			parallel_workers;
	double		pages_fetched;

	/* Compute heap pages for bitmap heap scan */
	pages_fetched = compute_bitmap_pages(root, rel, bitmapqual, 1.0,
										 NULL, NULL);

	parallel_workers = compute_parallel_worker(rel, pages_fetched, -1,
											   max_parallel_workers_per_gather);

	if (parallel_workers <= 0)
		return;

	add_partial_path(rel, (Path *) create_bitmap_heap_path(root, rel,
														   bitmapqual, rel->lateral_relids, 1.0, parallel_workers));
}

/*
 * Compute the number of parallel workers that should be used to scan a
 * relation.  We compute the parallel workers based on the size of the heap to
 * be scanned and the size of the index to be scanned, then choose a minimum
 * of those.
 *
 * "heap_pages" is the number of pages from the table that we expect to scan, or
 * -1 if we don't expect to scan any.
 *
 * "index_pages" is the number of pages from the index that we expect to scan, or
 * -1 if we don't expect to scan any.
 *
 * "max_workers" is caller's limit on the number of workers.  This typically
 * comes from a GUC.
 */
int
compute_parallel_worker(RelOptInfo *rel, double heap_pages, double index_pages,
						int max_workers)
{
	int			parallel_workers = 0;

	/*
	 * If the user has set the parallel_workers reloption, use that; otherwise
	 * select a default number of workers.
	 */
	if (rel->rel_parallel_workers != -1)
		parallel_workers = rel->rel_parallel_workers;
	else
	{
		/*
		 * If the number of pages being scanned is insufficient to justify a
		 * parallel scan, just return zero ... unless it's an inheritance
		 * child. In that case, we want to generate a parallel path here
		 * anyway.  It might not be worthwhile just for this relation, but
		 * when combined with all of its inheritance siblings it may well pay
		 * off.
		 */
		if (rel->reloptkind == RELOPT_BASEREL &&
			((heap_pages >= 0 && heap_pages < min_parallel_table_scan_size) ||
			 (index_pages >= 0 && index_pages < min_parallel_index_scan_size)))
			return 0;

		if (heap_pages >= 0)
		{
			int			heap_parallel_threshold;
			int			heap_parallel_workers = 1;

			/*
			 * Select the number of workers based on the log of the size of
			 * the relation.  This probably needs to be a good deal more
			 * sophisticated, but we need something here for now.  Note that
			 * the upper limit of the min_parallel_table_scan_size GUC is
			 * chosen to prevent overflow here.
			 */
			heap_parallel_threshold = Max(min_parallel_table_scan_size, 1);
			while (heap_pages >= (BlockNumber) (heap_parallel_threshold * 3))
			{
				heap_parallel_workers++;
				heap_parallel_threshold *= 3;
				if (heap_parallel_threshold > INT_MAX / 3)
					break;		/* avoid overflow */
			}

			parallel_workers = heap_parallel_workers;
		}

		if (index_pages >= 0)
		{
			int			index_parallel_workers = 1;
			int			index_parallel_threshold;

			/* same calculation as for heap_pages above */
			index_parallel_threshold = Max(min_parallel_index_scan_size, 1);
			while (index_pages >= (BlockNumber) (index_parallel_threshold * 3))
			{
				index_parallel_workers++;
				index_parallel_threshold *= 3;
				if (index_parallel_threshold > INT_MAX / 3)
					break;		/* avoid overflow */
			}

			if (parallel_workers > 0)
				parallel_workers = Min(parallel_workers, index_parallel_workers);
			else
				parallel_workers = index_parallel_workers;
		}
	}

	/* In no case use more than caller supplied maximum number of workers */
	parallel_workers = Min(parallel_workers, max_workers);

	return parallel_workers;
}

/*
 * generate_partitionwise_join_paths
 * 		为给定的分区连接关系创建表示分区级连接（partitionwise join）的路径。
 *
 * 必须在完成为所有子连接添加路径之后才能调用此函数。否则，add_path 可能会删除
 * 此处生成的某个路径所引用的路径。
 */
void
generate_partitionwise_join_paths(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *live_children = NIL;
	int			cnt_parts;
	int			num_parts;
	RelOptInfo **part_rels;

	/* 这里只处理连接关系。 */
	if (!IS_JOIN_REL(rel))
		return;

	/* 如果关系未分区，则无需执行任何操作。 */
	if (!IS_PARTITIONED_REL(rel))
		return;

	/* 该关系应该已设置 consider_partitionwise_join 标志。 */
	Assert(rel->consider_partitionwise_join);

	/* 防止由于分区层次过深导致的栈溢出。 */
	check_stack_depth();

	num_parts = rel->nparts;
	part_rels = rel->part_rels;

	/* 收集非 dummy（非空）的子连接。 */
	for (cnt_parts = 0; cnt_parts < num_parts; cnt_parts++)
	{
		RelOptInfo *child_rel = part_rels[cnt_parts];

		/* 如果它已被完全剪枝，那它肯定是 dummy 的。 */
		if (child_rel == NULL)
			continue;

		/* 为此分区子连接生成分区级连接路径。 */
		generate_partitionwise_join_paths(root, child_rel);

		/* 如果我们未能为此子连接生成任何路径，则必须放弃。 */
		if (child_rel->pathlist == NIL)
		{
			/*
			 * 将父连接关系标记为未分区，以便后续函数能正确处理它。
			 */
			rel->nparts = 0;
			return;
		}

		/* 否则，确定它的最便宜路径。 */
		set_cheapest(child_rel);

		/* Dummy 子连接不需要扫描，因此忽略它们。 */
		if (IS_DUMMY_REL(child_rel))
			continue;

#ifdef OPTIMIZER_DEBUG
		debug_print_rel(root, child_rel);
#endif

		live_children = lappend(live_children, child_rel);
	}

	/* 如果所有子连接都是 dummy 的，则父连接也是 dummy 的。 */
	if (!live_children)
	{
		mark_dummy_rel(rel);
		return;
	}

	/* 基于子连接路径为此关系构建额外的路径。 */
	add_paths_to_append_rel(root, rel, live_children);
	list_free(live_children);
}


/*****************************************************************************
 *			DEBUG SUPPORT
 *****************************************************************************/

#ifdef OPTIMIZER_DEBUG

static void
print_relids(PlannerInfo *root, Relids relids)
{
	int			x;
	bool		first = true;

	x = -1;
	while ((x = bms_next_member(relids, x)) >= 0)
	{
		if (!first)
			printf(" ");
		if (x < root->simple_rel_array_size &&
			root->simple_rte_array[x])
			printf("%s", root->simple_rte_array[x]->eref->aliasname);
		else
			printf("%d", x);
		first = false;
	}
}

static void
print_restrictclauses(PlannerInfo *root, List *clauses)
{
	ListCell   *l;

	foreach(l, clauses)
	{
		RestrictInfo *c = lfirst(l);

		print_expr((Node *) c->clause, root->parse->rtable);
		if (lnext(l))
			printf(", ");
	}
}

static void
print_path(PlannerInfo *root, Path *path, int indent)
{
	const char *ptype;
	bool		join = false;
	Path	   *subpath = NULL;
	int			i;

	switch (nodeTag(path))
	{
		case T_Path:
			switch (path->pathtype)
			{
				case T_SeqScan:
					ptype = "SeqScan";
					break;
				case T_SampleScan:
					ptype = "SampleScan";
					break;
				case T_FunctionScan:
					ptype = "FunctionScan";
					break;
				case T_TableFuncScan:
					ptype = "TableFuncScan";
					break;
				case T_ValuesScan:
					ptype = "ValuesScan";
					break;
				case T_CteScan:
					ptype = "CteScan";
					break;
				case T_NamedTuplestoreScan:
					ptype = "NamedTuplestoreScan";
					break;
				case T_Result:
					ptype = "Result";
					break;
				case T_WorkTableScan:
					ptype = "WorkTableScan";
					break;
				default:
					ptype = "???Path";
					break;
			}
			break;
		case T_IndexPath:
			ptype = "IdxScan";
			break;
		case T_BitmapHeapPath:
			ptype = "BitmapHeapScan";
			break;
		case T_BitmapAndPath:
			ptype = "BitmapAndPath";
			break;
		case T_BitmapOrPath:
			ptype = "BitmapOrPath";
			break;
		case T_TidPath:
			ptype = "TidScan";
			break;
		case T_SubqueryScanPath:
			ptype = "SubqueryScan";
			break;
		case T_ForeignPath:
			ptype = "ForeignScan";
			break;
		case T_CustomPath:
			ptype = "CustomScan";
			break;
		case T_NestPath:
			ptype = "NestLoop";
			join = true;
			break;
		case T_MergePath:
			ptype = "MergeJoin";
			join = true;
			break;
		case T_HashPath:
			ptype = "HashJoin";
			join = true;
			break;
		case T_AppendPath:
			ptype = "Append";
			break;
		case T_MergeAppendPath:
			ptype = "MergeAppend";
			break;
		case T_GroupResultPath:
			ptype = "GroupResult";
			break;
		case T_MaterialPath:
			ptype = "Material";
			subpath = ((MaterialPath *) path)->subpath;
			break;
		case T_UniquePath:
			ptype = "Unique";
			subpath = ((UniquePath *) path)->subpath;
			break;
		case T_GatherPath:
			ptype = "Gather";
			subpath = ((GatherPath *) path)->subpath;
			break;
		case T_GatherMergePath:
			ptype = "GatherMerge";
			subpath = ((GatherMergePath *) path)->subpath;
			break;
		case T_ProjectionPath:
			ptype = "Projection";
			subpath = ((ProjectionPath *) path)->subpath;
			break;
		case T_ProjectSetPath:
			ptype = "ProjectSet";
			subpath = ((ProjectSetPath *) path)->subpath;
			break;
		case T_SortPath:
			ptype = "Sort";
			subpath = ((SortPath *) path)->subpath;
			break;
		case T_GroupPath:
			ptype = "Group";
			subpath = ((GroupPath *) path)->subpath;
			break;
		case T_UpperUniquePath:
			ptype = "UpperUnique";
			subpath = ((UpperUniquePath *) path)->subpath;
			break;
		case T_AggPath:
			ptype = "Agg";
			subpath = ((AggPath *) path)->subpath;
			break;
		case T_GroupingSetsPath:
			ptype = "GroupingSets";
			subpath = ((GroupingSetsPath *) path)->subpath;
			break;
		case T_MinMaxAggPath:
			ptype = "MinMaxAgg";
			break;
		case T_WindowAggPath:
			ptype = "WindowAgg";
			subpath = ((WindowAggPath *) path)->subpath;
			break;
		case T_SetOpPath:
			ptype = "SetOp";
			subpath = ((SetOpPath *) path)->subpath;
			break;
		case T_RecursiveUnionPath:
			ptype = "RecursiveUnion";
			break;
		case T_LockRowsPath:
			ptype = "LockRows";
			subpath = ((LockRowsPath *) path)->subpath;
			break;
		case T_ModifyTablePath:
			ptype = "ModifyTable";
			break;
		case T_LimitPath:
			ptype = "Limit";
			subpath = ((LimitPath *) path)->subpath;
			break;
		default:
			ptype = "???Path";
			break;
	}

	for (i = 0; i < indent; i++)
		printf("\t");
	printf("%s", ptype);

	if (path->parent)
	{
		printf("(");
		print_relids(root, path->parent->relids);
		printf(")");
	}
	if (path->param_info)
	{
		printf(" required_outer (");
		print_relids(root, path->param_info->ppi_req_outer);
		printf(")");
	}
	printf(" rows=%.0f cost=%.2f..%.2f\n",
		   path->rows, path->startup_cost, path->total_cost);

	if (path->pathkeys)
	{
		for (i = 0; i < indent; i++)
			printf("\t");
		printf("  pathkeys: ");
		print_pathkeys(path->pathkeys, root->parse->rtable);
	}

	if (join)
	{
		JoinPath   *jp = (JoinPath *) path;

		for (i = 0; i < indent; i++)
			printf("\t");
		printf("  clauses: ");
		print_restrictclauses(root, jp->joinrestrictinfo);
		printf("\n");

		if (IsA(path, MergePath))
		{
			MergePath  *mp = (MergePath *) path;

			for (i = 0; i < indent; i++)
				printf("\t");
			printf("  sortouter=%d sortinner=%d materializeinner=%d\n",
				   ((mp->outersortkeys) ? 1 : 0),
				   ((mp->innersortkeys) ? 1 : 0),
				   ((mp->materialize_inner) ? 1 : 0));
		}

		print_path(root, jp->outerjoinpath, indent + 1);
		print_path(root, jp->innerjoinpath, indent + 1);
	}

	if (subpath)
		print_path(root, subpath, indent + 1);
}

void
debug_print_rel(PlannerInfo *root, RelOptInfo *rel)
{
	ListCell   *l;

	printf("RELOPTINFO (");
	print_relids(root, rel->relids);
	printf("): rows=%.0f width=%d\n", rel->rows, rel->reltarget->width);

	if (rel->baserestrictinfo)
	{
		printf("\tbaserestrictinfo: ");
		print_restrictclauses(root, rel->baserestrictinfo);
		printf("\n");
	}

	if (rel->joininfo)
	{
		printf("\tjoininfo: ");
		print_restrictclauses(root, rel->joininfo);
		printf("\n");
	}

	printf("\tpath list:\n");
	foreach(l, rel->pathlist)
		print_path(root, lfirst(l), 1);
	if (rel->cheapest_parameterized_paths)
	{
		printf("\n\tcheapest parameterized paths:\n");
		foreach(l, rel->cheapest_parameterized_paths)
			print_path(root, lfirst(l), 1);
	}
	if (rel->cheapest_startup_path)
	{
		printf("\n\tcheapest startup path:\n");
		print_path(root, rel->cheapest_startup_path, 1);
	}
	if (rel->cheapest_total_path)
	{
		printf("\n\tcheapest total path:\n");
		print_path(root, rel->cheapest_total_path, 1);
	}
	printf("\n");
	fflush(stdout);
}

#endif							/* OPTIMIZER_DEBUG */
