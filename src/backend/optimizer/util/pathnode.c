/*-------------------------------------------------------------------------
 *
 * pathnode.c
 *	  Routines to manipulate pathlists and create path nodes
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/pathnode.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "miscadmin.h"
#include "foreign/fdwapi.h"
#include "nodes/extensible.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/placeholder.h"
#include "optimizer/planmain.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/selfuncs.h"


/* 路径成本比较结果的枚举类型 */
typedef enum
{
	COSTS_EQUAL,				/* 路径成本模糊相等 */
	COSTS_BETTER1,				/* 第一个路径成本优于第二个 */
	COSTS_BETTER2,				/* 第二个路径成本优于第一个 */
	COSTS_DIFFERENT				/* 两者在成本上没有支配关系 */
} PathCostComparison;

/*
 * STD_FUZZ_FACTOR 是 compare_path_costs_fuzzily 的标准模糊因子。
 * XXX 是否值得让用户可控？它在规划器运行时间和路径成本比较精度之间提供了权衡。
 */
#define STD_FUZZ_FACTOR 1.01

static List *translate_sub_tlist(List *tlist, int relid);
static int	append_total_cost_compare(const void *a, const void *b);
static int	append_startup_cost_compare(const void *a, const void *b);
static List *reparameterize_pathlist_by_child(PlannerInfo *root,
											  List *pathlist,
											  RelOptInfo *child_rel);
static bool contain_references_to(PlannerInfo *root, Node *clause,
								  Relids relids);
static bool ris_contain_references_to(PlannerInfo *root, List *rinfos,
									  Relids relids);


/*****************************************************************************
 *		MISC. PATH UTILITIES
 *****************************************************************************/

/*
 * compare_path_costs
 *	  Return -1, 0, or +1 according as path1 is cheaper, the same cost,
 *	  or more expensive than path2 for the specified criterion.
 */
int
compare_path_costs(Path *path1, Path *path2, CostSelector criterion)
{
	if (criterion == STARTUP_COST)
	{
		if (path1->startup_cost < path2->startup_cost)
			return -1;
		if (path1->startup_cost > path2->startup_cost)
			return +1;

		/*
		 * If paths have the same startup cost (not at all unlikely), order
		 * them by total cost.
		 */
		if (path1->total_cost < path2->total_cost)
			return -1;
		if (path1->total_cost > path2->total_cost)
			return +1;
	}
	else
	{
		if (path1->total_cost < path2->total_cost)
			return -1;
		if (path1->total_cost > path2->total_cost)
			return +1;

		/*
		 * If paths have the same total cost, order them by startup cost.
		 */
		if (path1->startup_cost < path2->startup_cost)
			return -1;
		if (path1->startup_cost > path2->startup_cost)
			return +1;
	}
	return 0;
}

/*
 * compare_path_fractional_costs
 *    根据获取指定比例元组的成本比较两条执行路径的开销
 *
 * 参数:
 *    path1 - 第一个执行路径对象指针
 *    path2 - 第二个执行路径对象指针
 *    fraction - 要获取的元组比例(0.0到1.0之间)
 *
 * 返回值:
 *    -1 - path1的部分成本低于path2
 *     0 - path1和path2的部分成本相同
 *    +1 - path1的部分成本高于path2
 *
 * 说明:
 *    此函数实现了PostgreSQL查询优化器中路径选择的关键逻辑，用于比较两条执行路径在只需要获取部分结果时的成本差异。
 *    特别适用于有LIMIT子句或只需要部分结果集的场景，可以帮助选择最佳执行路径。
 *
 *    当fraction <= 0或fraction >= 1时，将其解释为1，表示需要获取全部元组，此时调用compare_path_costs
 *    函数直接比较总执行成本。
 *
 *    对于有效的fraction值，计算方法为: 启动成本 + 比例 * (总成本 - 启动成本)
 *    这种计算方式假设总成本减去启动成本部分(即处理剩余元组的成本)是线性分布的。
 */
int
compare_fractional_path_costs(Path *path1, Path *path2, 
                            double fraction)
{
    Cost    cost1,   /* 第一条路径的部分成本 */
            cost2;   /* 第二条路径的部分成本 */

    /* 如果比例无效或要求全部结果，直接比较总执行成本 */
    if (fraction <= 0.0 || fraction >= 1.0)
        return compare_path_costs(path1, path2, TOTAL_COST);
    
    /* 计算两条路径在指定比例下的部分执行成本 */
    /* 公式: 启动成本 + 比例 * (总成本 - 启动成本) */
    cost1 = path1->startup_cost +
        fraction * (path1->total_cost - path1->startup_cost);
    cost2 = path2->startup_cost +
        fraction * (path2->total_cost - path2->startup_cost);
    
    /* 比较计算得出的部分成本并返回比较结果 */
    if (cost1 < cost2)
        return -1;  /* path1更便宜 */
    if (cost1 > cost2)
        return +1;  /* path2更便宜 */
    return 0;       /* 成本相同 */
}


/*
 * compare_path_costs_fuzzily
 *	  模糊比较两个路径的成本，判断是否有支配关系。
 *
 * 使用模糊比较，避免 add_path() 保留一对成本差异微小的路径。
 *
 * fuzz_factor 参数必须为 1.0 加上 delta，delta 是认为有显著差异的最小成本比例。
 * 例如 fuzz_factor = 1.01 表示 1% 的成本差异才算显著。
 *
 * 如果启动成本和总成本都模糊相同，则认为两路径成本“相等”。
 * 如果 path1 启动成本模糊更优且总成本不差，或总成本更优且启动成本不差，则 path1 支配 path2。
 * 反之亦然。如果一个路径启动成本更优但总成本更差，则认为“不同”，即没有支配关系。
 *
 * 此函数还强制一个策略：如果 parent->consider_startup 或 parent->consider_param_startup 为 false，
 * 则不能仅因启动成本好而存活，所以在总成本劣势时不会返回 COSTS_DIFFERENT。
 * （但如果总成本模糊相等，仍比较启动成本以期消除冗余路径。）
 */
static PathCostComparison
compare_path_costs_fuzzily(Path *path1, Path *path2, double fuzz_factor)
{
#define CONSIDER_PATH_STARTUP_COST(p)  \
	((p)->param_info == NULL ? (p)->parent->consider_startup : (p)->parent->consider_param_startup)

	/* 先比较总成本，因为多数路径启动成本为零，总成本更容易有差异 */
	if (path1->total_cost > path2->total_cost * fuzz_factor)
	{
		/* path1 总成本模糊更差 */
		if (CONSIDER_PATH_STARTUP_COST(path1) &&
			path2->startup_cost > path1->startup_cost * fuzz_factor)
		{
			/* 但 path2 启动成本也更差，则无支配关系 */
			return COSTS_DIFFERENT;
		}
		/* 否则 path2 支配 path1 */
		return COSTS_BETTER2;
	}
	if (path2->total_cost > path1->total_cost * fuzz_factor)
	{
		/* path2 总成本模糊更差 */
		if (CONSIDER_PATH_STARTUP_COST(path2) &&
			path1->startup_cost > path2->startup_cost * fuzz_factor)
		{
			/* 但 path1 启动成本也更差，则无支配关系 */
			return COSTS_DIFFERENT;
		}
		/* 否则 path1 支配 path2 */
		return COSTS_BETTER1;
	}
	/* 总成本模糊相同，继续比较启动成本 */
	if (path1->startup_cost > path2->startup_cost * fuzz_factor)
	{
		/* path1 启动成本更差，path2 支配 path1 */
		return COSTS_BETTER2;
	}
	if (path2->startup_cost > path1->startup_cost * fuzz_factor)
	{
		/* path2 启动成本更差，path1 支配 path2 */
		return COSTS_BETTER1;
	}
	/* 两者成本都模糊相同 */
	return COSTS_EQUAL;

#undef CONSIDER_PATH_STARTUP_COST
}
/*
 * set_cheapest
 *	  从一个关系的所有访问路径中找出最小成本的路径，并将它们保存在关系的最小成本路径字段中。
 *
 * cheapest_total_path 通常是总成本最低的非参数化路径；但如果没有非参数化路径，
 * 则会将其赋值为最佳（成本最低、参数化程度最低）的参数化路径。然而，只有非参数化路径
 * 会被考虑作为 cheapest_startup_path 的候选，因此如果没有非参数化路径，该值将为 NULL。
 *
 * cheapest_parameterized_paths 列表收集了所有在 add_path() 竞争中幸存下来的参数化路径。
 * （由于 add_path 在处理参数化路径时忽略路径键，这些路径将是针对其参数化的最佳成本或最佳行数的路径。
 * 在某些情况下，对于相同的参数化，我们可能同时有一个并行安全和非并行安全的路径，但这种情况应该相对罕见，
 * 因为最典型的是，同一关系的所有路径要么都是并行安全的，要么都不是。）
 *
 * 如果存在总成本最低的非参数化路径，cheapest_parameterized_paths 也会始终包含它；
 * 这个列表的用户发现这样包含更方便。
 *
 * 此函数通常仅在我们完成构建关系节点的路径列表后调用。
 */
void
set_cheapest(RelOptInfo *parent_rel)  /* 输入参数：关系优化信息结构体指针 */
{
	Path	   *cheapest_startup_path;  /* 启动成本最低的路径 */
	Path	   *cheapest_total_path;    /* 总成本最低的路径 */
	Path	   *best_param_path;        /* 最佳参数化路径 */
	List	   *parameterized_paths;    /* 参数化路径列表 */
	ListCell   *p;                      /* 遍历路径列表的指针 */

	/* 断言：确保传入的是有效的RelOptInfo类型 */
	Assert(IsA(parent_rel, RelOptInfo));

	/* 如果路径列表为空，报错 */
	if (parent_rel->pathlist == NIL)
		elog(ERROR, "could not devise a query plan for the given query");

	/* 初始化所有路径变量为NULL，参数化路径列表为空 */
	cheapest_startup_path = cheapest_total_path = best_param_path = NULL;
	parameterized_paths = NIL;

	/* 遍历关系的所有访问路径 */
	foreach(p, parent_rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(p);  /* 当前路径 */
		int			cmp;                          /* 路径比较结果 */

		if (path->param_info)  /* 如果是参数化路径 */
		{
			/* 参数化路径，将其添加到参数化路径列表中 */
			parameterized_paths = lappend(parameterized_paths, path);

			/*
			 * 如果我们已经找到一个非参数化的总成本最低路径，那么我们不再关心
			 * 寻找最佳参数化路径，所以继续下一个路径。
			 */
			if (cheapest_total_path)
				continue;

			/*
			 * 否则，跟踪最佳参数化路径，即那些在最小参数化中总成本最低的路径。
			 */
			if (best_param_path == NULL)
				best_param_path = path;
			else
			{
				/* 比较两个参数化路径的参数化需求（所需的外层关系） */
				switch (bms_subset_compare(PATH_REQ_OUTER(path),
							   PATH_REQ_OUTER(best_param_path)))
				{
					case BMS_EQUAL:  /* 参数化需求相同 */
						/* 保留成本更低的路径 */
						if (compare_path_costs(path, best_param_path,
							   TOTAL_COST) < 0)
							best_param_path = path;
						break;
					case BMS_SUBSET1:  /* 新路径参数化程度更低 */
						best_param_path = path;
						break;
					case BMS_SUBSET2:  /* 旧路径参数化程度更低，保留它 */
						break;
					case BMS_DIFFERENT:  /* 参数化需求既不是子集也不全等 */

						/*
						 * 这意味着两个路径都不具有关系的最小可能参数化。
						 * 我们将保留旧路径，直到出现更好的路径。
						 */
						break;
				}
			}
		}
		else  /* 如果是非参数化路径 */
		{
			/* 非参数化路径，考虑它是否是最低成本路径 */
			if (cheapest_total_path == NULL)
			{
				/* 第一个遇到的非参数化路径，初始化为所有最低成本路径 */
				cheapest_startup_path = cheapest_total_path = path;
				continue;
			}

			/*
			 * 如果我们找到两条成本相同的路径，尝试保留排序更好的路径。
			 * 这些路径可能有不相关的排序顺序，在这种情况下，我们只能猜测保留哪条可能更好，
			 * 但如果一条明显优于另一条，我们肯定应该保留那条更好的。
			 */
			/* 比较启动成本 */
			cmp = compare_path_costs(cheapest_startup_path, path, STARTUP_COST);
			if (cmp > 0 ||  /* 如果当前路径启动成本更低 */
				(cmp == 0 &&  /* 或者启动成本相同，但排序更好 */
				 compare_pathkeys(cheapest_startup_path->pathkeys,
							  path->pathkeys) == PATHKEYS_BETTER2))
				cheapest_startup_path = path;

			/* 比较总成本 */
			cmp = compare_path_costs(cheapest_total_path, path, TOTAL_COST);
			if (cmp > 0 ||  /* 如果当前路径总成本更低 */
				(cmp == 0 &&  /* 或者总成本相同，但排序更好 */
				 compare_pathkeys(cheapest_total_path->pathkeys,
							  path->pathkeys) == PATHKEYS_BETTER2))
				cheapest_total_path = path;
		}
	}

	/* 将最便宜的非参数化路径（如果有）添加到参数化路径列表中 */
	if (cheapest_total_path)
		parameterized_paths = lcons(cheapest_total_path, parameterized_paths);

	/*
	 * 如果没有非参数化路径，使用最佳参数化路径作为 cheapest_total_path
	 * （但不作为 cheapest_startup_path）。
	 */
	if (cheapest_total_path == NULL)
		cheapest_total_path = best_param_path;
	Assert(cheapest_total_path != NULL);  /* 确保至少有一条总成本路径 */

	/* 将找到的各种最低成本路径保存到关系结构中 */
	parent_rel->cheapest_startup_path = cheapest_startup_path;
	parent_rel->cheapest_total_path = cheapest_total_path;
	parent_rel->cheapest_unique_path = NULL;  /* 仅在需要时计算 */
	parent_rel->cheapest_parameterized_paths = parameterized_paths;
}


/*
 * add_path
 *	  考虑为指定的父关系（parent_rel）添加一个潜在的实现路径（new_path），
 *	  如果该路径具有更好的排序顺序（更优的 pathkeys）、更低的成本（任一维度），
 *	  或者生成更少的行数，或者比现有路径更具并行安全性，则将其添加到 rel 的 pathlist 中。
 *
 *	  同时，如果 new_path 优于旧路径（即成本更低、排序不差、行数不多、所需外部 rel 不多于旧路径，并且并行安全性不低于旧路径），
 *	  则从 pathlist 中移除被 new_path 支配的旧路径。
 *
 *	  在大多数情况下，参数化路径的参数集合越大，生成的行数越少（因为有更多的连接条件），
 *	  所以这两个衡量标准通常是相反的；这意味着不同参数化的路径很少会互相支配，但确实会出现这种情况，因此需要完整检查。
 *
 *	  本函数和其兄弟函数 add_path_precheck 中包含了两个策略决策。
 *	  第一，所有参数化路径都被视为没有 pathkeys，这样它们不会因为排序顺序而胜出，从而减少保留的参数化路径数量。
 *	  第二，只有当 parent_rel->consider_startup（无参数化路径）或 parent_rel->consider_param_startup（参数化路径）为真时，
 *	  才认为低启动成本有意义，这样可以更早地丢弃无用路径。
 *
 *	  pathlist 按 total_cost 升序排列，成本较低的路径在前。这不仅加快本函数的执行速度，
 *	  也让 add_path_precheck 能够更早退出。
 *
 *	  注意：被丢弃的 Path 对象会立即 pfree，以减少规划器内存消耗。不能释放 Path 的子结构，因为它们可能被其他 Path 或查询树共享；
 *	  但仅回收被丢弃的 Path 节点已经能显著节省内存。List 节点也可以回收。
 *
 *	  如 optimizer/README 所述，删除已接受的 Path 是安全的，因为此时该 rel 的 Path 不会被其他 rel（如更高层的连接）引用。
 *	  但某些情况下 Path 可能被同一个 rel 的其他 Path 引用，比如 IndexPath 可能被 BitmapHeapPath 引用，
 *	  因此拒绝时不能 pfree IndexPath。
 *
 * 'parent_rel' 是路径对应的关系条目。
 * 'new_path' 是 parent_rel 的一个潜在路径。
 *
 * 无返回值，但会修改 parent_rel->pathlist。
 */
void
add_path(RelOptInfo *parent_rel, Path *new_path)
{
	bool		accept_new = true;	/* 除非发现更优的旧路径，否则接受新路径 */
	ListCell   *insert_after = NULL;	/* 新路径插入的位置 */
	List	   *new_path_pathkeys;
	ListCell   *p1;
	ListCell   *p1_prev;
	ListCell   *p1_next;

	/*
	 * 这是检查查询取消的合适位置——规划器不会长时间不调用 add_path()。
	 */
	CHECK_FOR_INTERRUPTS();

	/* 按策略，参数化路径视为没有 pathkeys */
	new_path_pathkeys = new_path->param_info ? NIL : new_path->pathkeys;

	/*
	 * 循环检查新路径与旧路径的关系。注意 new_path 可能支配多个旧路径。
	 * 不能用 foreach，因为循环体可能会删除当前 list cell。
	 */
	p1_prev = NULL;
	for (p1 = list_head(parent_rel->pathlist); p1 != NULL; p1 = p1_next)
	{
		Path	   *old_path = (Path *) lfirst(p1);
		bool		remove_old = false; /* 除非新路径证明更优，否则不移除旧路径 */
		PathCostComparison costcmp;
		PathKeysComparison keyscmp;
		BMS_Comparison outercmp;

		p1_next = lnext(p1);

		/*
		 * 用标准模糊因子做成本比较。
		 */
		costcmp = compare_path_costs_fuzzily(new_path, old_path,
											 STD_FUZZ_FACTOR);

		/*
		 * 如果启动和总成本比较结果不同，则保留两条路径，可跳过 pathkeys 和 required_outer 的比较。
		 * 如果成本比较结果相同，则继续比较其他属性。行数最后比较（因为通常不会影响结果）。
		 */
		if (costcmp != COSTS_DIFFERENT)
		{
			/* 同样检查 pathkeys 是否有支配关系 */
			List	   *old_path_pathkeys;

			/* 参数化路径视为没有 pathkeys */
			old_path_pathkeys = old_path->param_info ? NIL : old_path->pathkeys;
			/* 检查 pathkeys 是否有支配关系 */
			keyscmp = compare_pathkeys(new_path_pathkeys,
									   old_path_pathkeys);

			/*
			 * 如果 pathkeys 不同，成本比较结果无关紧要。
			 * 只有当 pathkeys 有包含关系时，才继续比较 required_outer、rows 和 parallel_safe。
			 */
			if (keyscmp != PATHKEYS_DIFFERENT)
			{
				switch (costcmp)
				{
					case COSTS_EQUAL:
						outercmp = bms_subset_compare(PATH_REQ_OUTER(new_path),
													  PATH_REQ_OUTER(old_path));

						/*
						 * 如果新路径的 required_outer 是旧路径的子集，或者两者相等，
						 * 则新路径支配旧路径。反之亦然。
						 */
						if (keyscmp == PATHKEYS_BETTER1)
						{
							/* 新路径的 pathkeys 更好 */
							if ((outercmp == BMS_EQUAL ||
								 outercmp == BMS_SUBSET1) &&
								new_path->rows <= old_path->rows &&
								new_path->parallel_safe >= old_path->parallel_safe)
								remove_old = true;	/* 新路径支配旧路径 */
						}
						else if (keyscmp == PATHKEYS_BETTER2)
						{
							/* 旧路径的 pathkeys 更好 */
							if ((outercmp == BMS_EQUAL ||
								 outercmp == BMS_SUBSET2) &&
								new_path->rows >= old_path->rows &&
								new_path->parallel_safe <= old_path->parallel_safe)
								accept_new = false; /* 旧路径支配新路径 */
						}
						else	/* keyscmp == PATHKEYS_EQUAL */
						{
							/* pathkeys 相同 */
							if (outercmp == BMS_EQUAL)
							{
								/*
								 * 相同的 pathkeys 和 outer rels，且成本模糊相同，只保留一个；
								 * 优先比较并行安全性，再比较行数，最后用极小模糊因子再比较成本。
								 * 如果仍然相同，则只保留旧路径。
								 */
								if (new_path->parallel_safe > old_path->parallel_safe)
									remove_old = true;	/* 新路径支配旧路径 */
								else if (new_path->parallel_safe < old_path->parallel_safe)
									accept_new = false; /* 旧路径支配新路径 */
								else if (new_path->rows < old_path->rows)
									remove_old = true;	/* 新路径支配旧路径 */
								else if (new_path->rows > old_path->rows)
									accept_new = false; /* 旧路径支配新路径 */
								else if (compare_path_costs_fuzzily(new_path, old_path, 1.0000000001) == COSTS_BETTER1)
									remove_old = true;	/* 新路径支配旧路径 */
								else
									accept_new = false; /* 旧路径等于或支配新路径 */
							}
							else if (outercmp == BMS_SUBSET1 &&
									 new_path->rows <= old_path->rows &&
									 new_path->parallel_safe >= old_path->parallel_safe)
								/* 新路径的 required_outer 是旧路径的子集 */	 
								remove_old = true;	/* 新路径支配旧路径 */
							else if (outercmp == BMS_SUBSET2 &&
									 new_path->rows >= old_path->rows &&
									 new_path->parallel_safe <= old_path->parallel_safe)
								/* 旧路径的 required_outer 是新路径的子集 */	 
								accept_new = false; /* 旧路径支配新路径 */
							/* 否则参数化不同，两者都保留 */
						}
						break;
					case COSTS_BETTER1:
						/* 新路径成本更优 */
						if (keyscmp != PATHKEYS_BETTER2)
						{
							/* 新路径的 required_outer 是旧路径的子集
							 * 或者两者相等，
							 * 则新路径支配旧路径。反之亦然。
							 */
							outercmp = bms_subset_compare(PATH_REQ_OUTER(new_path),
														  PATH_REQ_OUTER(old_path));
							if ((outercmp == BMS_EQUAL ||
								 outercmp == BMS_SUBSET1) &&
								new_path->rows <= old_path->rows &&
								new_path->parallel_safe >= old_path->parallel_safe)
								remove_old = true;	/* 新路径支配旧路径 */
						}
						break;
					case COSTS_BETTER2:
						if (keyscmp != PATHKEYS_BETTER1)
						{
							outercmp = bms_subset_compare(PATH_REQ_OUTER(new_path),
														  PATH_REQ_OUTER(old_path));
							if ((outercmp == BMS_EQUAL ||
								 outercmp == BMS_SUBSET2) &&
								new_path->rows >= old_path->rows &&
								new_path->parallel_safe <= old_path->parallel_safe)
								accept_new = false; /* 旧路径支配新路径 */
						}
						break;
					case COSTS_DIFFERENT:

						/*
						 * 理论上不会到这里，仅为消除编译器警告保留
						 */
						break;
				}
			}
		}

		/*
		 * 如果新路径支配旧路径，则移除旧路径。
		 */
		if (remove_old)
		{
			parent_rel->pathlist = list_delete_cell(parent_rel->pathlist,
													p1, p1_prev);

			/*
			 * 如果可以，释放被删除的节点所指向的数据
			 * 但不释放 IndexPath，因为它是共享的
			 */
			if (!IsA(old_path, IndexPath))
				pfree(old_path);
			/* p1_prev 不前进 */
		}
		else
		{
			/* 如果新路径成本 >= 旧路径，则新路径插入在旧路径之后 */
			if (new_path->total_cost >= old_path->total_cost)
				insert_after = p1;
			/* p1_prev 前进 */
			p1_prev = p1;
		}

		/*
		 * 如果发现旧路径支配新路径，则可以提前退出循环，不再添加新路径，
		 * 并假定新路径不会支配 pathlist 中的其他元素。
		 */
		if (!accept_new)
			break;
	}

	if (accept_new)
	{
		/*
		 * 接受新路径：插入到 pathlist 的合适位置
		 */
		if (insert_after)
			lappend_cell(parent_rel->pathlist, insert_after, new_path);
		else
			parent_rel->pathlist = lcons(new_path, parent_rel->pathlist);
	}
	else
	{
		/*
		 * 拒绝并释放新路径
		 * 但不释放 IndexPath，因为它是共享的
		 */
		if (!IsA(new_path, IndexPath))
			pfree(new_path);
	}
}
/*
 * add_path_precheck
 *	  检查一个拟添加的新路径是否有可能被接受。
 *	  假定已准确知道路径的 pathkeys 和参数化信息，并有成本下界。
 *
 * 注意：此时无法获得路径的行数估算，因为在预检查前获取太昂贵。
 * 假定参数化集合更大的路径会生成更少的行数，因此不同参数化的路径不会互相支配，
 * 可以忽略参数化不同的已有路径。（极少数情况下该假设不成立，add_path 会进一步处理。）
 *
 * 调用时还未构造 Path 结构体，因此相关信息需分散传递。
 */
bool
add_path_precheck(RelOptInfo *parent_rel,
				  Cost startup_cost, Cost total_cost,
				  List *pathkeys, Relids required_outer)
{
	List	   *new_path_pathkeys;
	bool		consider_startup;
	ListCell   *p1;

	/* 按 add_path 策略，参数化路径视为无 pathkeys */
	new_path_pathkeys = required_outer ? NIL : pathkeys;

	/* 判断新路径的启动成本是否有意义 */
	consider_startup = required_outer ? parent_rel->consider_param_startup : parent_rel->consider_startup;

	foreach(p1, parent_rel->pathlist)
	{
		Path	   *old_path = (Path *) lfirst(p1);
		PathKeysComparison keyscmp;

		/*
		 * 寻找参数化相同（假定行数也相同）、在 pathkeys 和成本上都支配新路径的旧路径。
		 * 若找到，则可拒绝新路径。
		 * 成本比较应与 compare_path_costs_fuzzily 保持一致。
		 */
		if (total_cost > old_path->total_cost * STD_FUZZ_FACTOR)
		{
			/* 只有 consider_startup 时，启动成本才有机会胜出 */
			if (startup_cost > old_path->startup_cost * STD_FUZZ_FACTOR ||
				!consider_startup)
			{
				/* 新路径成本劣势，检查 pathkeys... */
				List	   *old_path_pathkeys;

				old_path_pathkeys = old_path->param_info ? NIL : old_path->pathkeys;
				keyscmp = compare_pathkeys(new_path_pathkeys,
										   old_path_pathkeys);
				if (keyscmp == PATHKEYS_EQUAL ||
					keyscmp == PATHKEYS_BETTER2)
				{
					/* 新路径在 pathkeys 上无优势... */
					if (bms_equal(required_outer, PATH_REQ_OUTER(old_path)))
					{
						/* 找到支配新路径的旧路径，拒绝新路径 */
						return false;
					}
				}
			}
		}
		else
		{
			/*
			 * 由于 pathlist 按 total_cost 升序排列，
			 * 一旦遇到 total_cost 大于新路径的旧路径即可停止。
			 */
			break;
		}
	}

	return true;
}

/*
 * add_partial_path
 *    与add_path类似，目标是评估一个路径是否值得保留，但这里的考虑因素有所不同。
 *    部分路径(partial path)是指可以在多个工作进程中并行执行的路径，每个工作进程会生成结果的一个子集。
 *
 *    与add_path一样，partial_pathlist保持排序状态，总成本最低的路径排在前面。
 *    多个地方依赖于这种排序，它们直接取列表第一个元素作为最便宜路径而无需搜索。
 *
 *    我们不生成参数化的部分路径，主要原因是它们不安全：无法确保在参数化部分的并行扫描在
 *    每个工作进程中同时使用相同的参数值。幸运的是，这种方案通常也不值得实施，因为让每个工作进程
 *    扫描整个外表和内表的子集通常是一个糟糕的计划。参数化的内表通常很小。虽然可能存在极少数情况
 *    这种方案会带来巨大优势（例如，连接顺序约束使一个只有1行的关系位于最顶层连接的外表，而内表
 *    使用参数化计划），但在有人构建能够处理这些情况的执行器基础设施之前，我们只能不处理这些情况。
 *
 *    由于我们不考虑参数化路径，因此也不需要将行数作为质量衡量标准：每个路径都会产生相同数量的行。
 *    我们也不需要考虑启动成本：并行处理只用于会运行到完成的计划。因此，此函数比add_path简单得多：
 *    它只需要考虑pathkeys和总成本。
 *
 *    与add_path一样，我们会释放被其他部分路径支配的路径；这要求这些路径目前没有其他引用。
 *    因此，在完成为某个关系创建所有部分路径之前，不能为该关系创建GatherPaths。
 *    与add_path不同，我们不对IndexPaths做特殊处理，因为部分索引路径不会被部分BitmapHeapPaths引用。
 */
void
add_partial_path(RelOptInfo *parent_rel, Path *new_path)
{
    bool        accept_new = true;    /* 除非找到更优的旧路径，否则接受新路径 */
    ListCell   *insert_after = NULL;  /* 新项插入位置的前一个节点 */
    ListCell   *p1;
    ListCell   *p1_prev;
    ListCell   *p1_next;

    /* 检查查询是否被取消 */
    CHECK_FOR_INTERRUPTS();

    /* 添加的路径必须是并行安全的 */
    Assert(new_path->parallel_safe);

    /* 关系也应该允许并行处理 */
    Assert(parent_rel->consider_parallel);

    /*
     * 与add_path类似，丢弃所有被新路径支配的旧路径，但如果有现有路径支配新路径，则丢弃新路径
     */
    p1_prev = NULL;
    for (p1 = list_head(parent_rel->partial_pathlist); p1 != NULL;
         p1 = p1_next)
    {
        Path       *old_path = (Path *) lfirst(p1);
        bool        remove_old = false; /* 除非新路径证明更优，否则不移除旧路径 */
        PathKeysComparison keyscmp;

        p1_next = lnext(p1);

        /* 比较两个路径的排序键 */
        keyscmp = compare_pathkeys(new_path->pathkeys, old_path->pathkeys);

        /* 除非排序键不可比较，否则只保留两个路径中的一个 */
        if (keyscmp != PATHKEYS_DIFFERENT)
        {
            if (new_path->total_cost > old_path->total_cost * STD_FUZZ_FACTOR)
            {
                /* 新路径成本更高；只有当它的排序键更好时才保留 */
                if (keyscmp != PATHKEYS_BETTER1)
                    accept_new = false;
            }
            else if (old_path->total_cost > new_path->total_cost
                     * STD_FUZZ_FACTOR)
            {
                /* 旧路径成本更高；只有当它的排序键更好时才保留 */
                if (keyscmp != PATHKEYS_BETTER2)
                    remove_old = true;
            }
            else if (keyscmp == PATHKEYS_BETTER1)
            {
                /* 成本大致相同，但新路径的排序键更好 */
                remove_old = true;
            }
            else if (keyscmp == PATHKEYS_BETTER2)
            {
                /* 成本大致相同，但旧路径的排序键更好 */
                accept_new = false;
            }
            else if (old_path->total_cost > new_path->total_cost * 1.0000000001)
            {
                /* 排序键相同，且旧路径成本更高 */
                remove_old = true;
            }
            else
            {
                /*
                 * 排序键相同，且新路径成本没有明显更低
                 */
                accept_new = false;
            }
        }

        /*
         * 如果旧路径被新路径支配，则从partial_pathlist中移除当前元素
         */
        if (remove_old)
        {
            parent_rel->partial_pathlist =
                list_delete_cell(parent_rel->partial_pathlist, p1, p1_prev);
            pfree(old_path);
            /* p1_prev不前进 */
        }
        else
        {
            /* 如果新路径成本大于等于旧路径，则新路径应插入在该旧路径之后 */
            if (new_path->total_cost >= old_path->total_cost)
                insert_after = p1;
            /* p1_prev前进 */
            p1_prev = p1;
        }

        /*
         * 如果找到一个支配新路径的旧路径，可以停止扫描partial_pathlist；
         * 我们不会添加新路径，并假设新路径也无法支配后面的任何路径
         */
        if (!accept_new)
            break;
    }

    if (accept_new)
    {
        /* 接受新路径：在适当位置插入 */
        if (insert_after)
            lappend_cell(parent_rel->partial_pathlist, insert_after, new_path);
        else
            parent_rel->partial_pathlist =
                lcons(new_path, parent_rel->partial_pathlist);
    }
    else
    {
        /* 拒绝并回收新路径 */
        pfree(new_path);
    }
}


/*
 * add_partial_path_precheck
 *	  Check whether a proposed new partial path could possibly get accepted.
 *
 * Unlike add_path_precheck, we can ignore startup cost and parameterization,
 * since they don't matter for partial paths (see add_partial_path).  But
 * we do want to make sure we don't add a partial path if there's already
 * a complete path that dominates it, since in that case the proposed path
 * is surely a loser.
 */
bool
add_partial_path_precheck(RelOptInfo *parent_rel, Cost total_cost,
						  List *pathkeys)
{
	ListCell   *p1;

	/*
	 * Our goal here is twofold.  First, we want to find out whether this path
	 * is clearly inferior to some existing partial path.  If so, we want to
	 * reject it immediately.  Second, we want to find out whether this path
	 * is clearly superior to some existing partial path -- at least, modulo
	 * final cost computations.  If so, we definitely want to consider it.
	 *
	 * Unlike add_path(), we always compare pathkeys here.  This is because we
	 * expect partial_pathlist to be very short, and getting a definitive
	 * answer at this stage avoids the need to call add_path_precheck.
	 */
	foreach(p1, parent_rel->partial_pathlist)
	{
		Path	   *old_path = (Path *) lfirst(p1);
		PathKeysComparison keyscmp;

		keyscmp = compare_pathkeys(pathkeys, old_path->pathkeys);
		if (keyscmp != PATHKEYS_DIFFERENT)
		{
			if (total_cost > old_path->total_cost * STD_FUZZ_FACTOR &&
				keyscmp != PATHKEYS_BETTER1)
				return false;
			if (old_path->total_cost > total_cost * STD_FUZZ_FACTOR &&
				keyscmp != PATHKEYS_BETTER2)
				return true;
		}
	}

	/*
	 * This path is neither clearly inferior to an existing partial path nor
	 * clearly good enough that it might replace one.  Compare it to
	 * non-parallel plans.  If it loses even before accounting for the cost of
	 * the Gather node, we should definitely reject it.
	 *
	 * Note that we pass the total_cost to add_path_precheck twice.  This is
	 * because it's never advantageous to consider the startup cost of a
	 * partial path; the resulting plans, if run in parallel, will be run to
	 * completion.
	 */
	if (!add_path_precheck(parent_rel, total_cost, total_cost, pathkeys,
						   NULL))
		return false;

	return true;
}


/*****************************************************************************
 *		路径节点创建例程
 *****************************************************************************/

/*
 * create_seqscan_path
 *	  创建一个顺序扫描（SeqScan）路径节点，并返回该节点。
 *
 * 参数说明：
 * root - 规划器信息结构体
 * rel - 关联的关系信息结构体
 * required_outer - 参数化路径所需的外部关系集合
 * parallel_workers - 并行工作者数量
 */
Path *
create_seqscan_path(PlannerInfo *root, RelOptInfo *rel,
					Relids required_outer, int parallel_workers)
{
	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_SeqScan;				/* 路径类型为顺序扫描 */
	pathnode->parent = rel;						/* 所属关系 */
	pathnode->pathtarget = rel->reltarget;		/* 目标输出 */
	pathnode->param_info = get_baserel_parampathinfo(root, rel,
													 required_outer); 	/* 参数化信息 */
	pathnode->parallel_aware = parallel_workers > 0 ? true : false;		/* 是否支持并行 */
	pathnode->parallel_safe = rel->consider_parallel;					/* 是否并行安全 */
	pathnode->parallel_workers = parallel_workers;						/* 并行工作者数量 */
	pathnode->pathkeys = NIL;		/* 顺序扫描结果无序 */

	cost_seqscan(pathnode, root, rel, pathnode->param_info);	/* 计算路径成本 */

	return pathnode;
}

/*
 * create_samplescan_path
 *	  Creates a path node for a sampled table scan.
 */
Path *
create_samplescan_path(PlannerInfo *root, RelOptInfo *rel, Relids required_outer)
{
	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_SampleScan;
	pathnode->parent = rel;
	pathnode->pathtarget = rel->reltarget;
	pathnode->param_info = get_baserel_parampathinfo(root, rel,
													 required_outer);
	pathnode->parallel_aware = false;
	pathnode->parallel_safe = rel->consider_parallel;
	pathnode->parallel_workers = 0;
	pathnode->pathkeys = NIL;	/* samplescan has unordered result */

	cost_samplescan(pathnode, root, rel, pathnode->param_info);

	return pathnode;
}

/*
 * create_index_path
 *	  Creates a path node for an index scan.
 *
 * 'index' is a usable index.
 * 'indexclauses' is a list of IndexClause nodes representing clauses
 *			to be enforced as qual conditions in the scan.
 * 'indexorderbys' is a list of bare expressions (no RestrictInfos)
 *			to be used as index ordering operators in the scan.
 * 'indexorderbycols' is an integer list of index column numbers (zero based)
 *			the ordering operators can be used with.
 * 'pathkeys' describes the ordering of the path.
 * 'indexscandir' is ForwardScanDirection or BackwardScanDirection
 *			for an ordered index, or NoMovementScanDirection for
 *			an unordered index.
 * 'indexonly' is true if an index-only scan is wanted.
 * 'required_outer' is the set of outer relids for a parameterized path.
 * 'loop_count' is the number of repetitions of the indexscan to factor into
 *		estimates of caching behavior.
 * 'partial_path' is true if constructing a parallel index scan path.
 *
 * Returns the new path node.
 */
IndexPath *
create_index_path(PlannerInfo *root,
				  IndexOptInfo *index,
				  List *indexclauses,
				  List *indexorderbys,
				  List *indexorderbycols,
				  List *pathkeys,
				  ScanDirection indexscandir,
				  bool indexonly,
				  Relids required_outer,
				  double loop_count,
				  bool partial_path)
{
	IndexPath  *pathnode = makeNode(IndexPath);
	RelOptInfo *rel = index->rel;

	pathnode->path.pathtype = indexonly ? T_IndexOnlyScan : T_IndexScan;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = rel->reltarget;
	pathnode->path.param_info = get_baserel_parampathinfo(root, rel,
														  required_outer);
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_workers = 0;
	pathnode->path.pathkeys = pathkeys;

	pathnode->indexinfo = index;
	pathnode->indexclauses = indexclauses;
	pathnode->indexorderbys = indexorderbys;
	pathnode->indexorderbycols = indexorderbycols;
	pathnode->indexscandir = indexscandir;

	cost_index(pathnode, root, loop_count, partial_path);

	return pathnode;
}

/*
 * create_bitmap_heap_path
 *    创建一个位图堆扫描(BitmapHeapScan)路径节点，并返回该节点
 *
 * 参数说明：
 *    root - 规划器信息结构体指针
 *    rel - 关联的关系信息结构体指针
 *    bitmapqual - 位图条件路径（可以是IndexPath、BitmapAndPath或BitmapOrPath的树结构）
 *    required_outer - 参数化路径所需的外部关系集合
 *    loop_count - 索引扫描重复次数，用于缓存行为的成本估算
 *    parallel_degree - 并行工作者数量
 *
 * 返回值：
 *    BitmapHeapPath* - 新创建的位图堆扫描路径节点
 */
BitmapHeapPath *
create_bitmap_heap_path(PlannerInfo *root,
						RelOptInfo *rel,
						Path *bitmapqual,
						Relids required_outer,
						double loop_count,
						int parallel_degree)
{
	BitmapHeapPath *pathnode = makeNode(BitmapHeapPath);  // 创建位图堆扫描路径节点

	pathnode->path.pathtype = T_BitmapHeapScan;           // 路径类型为位图堆扫描
	pathnode->path.parent = rel;                          // 设置所属关系
	pathnode->path.pathtarget = rel->reltarget;           // 设置目标输出
	pathnode->path.param_info = get_baserel_parampathinfo(root, rel, required_outer); // 参数化信息
	pathnode->path.parallel_aware = parallel_degree > 0 ? true : false; // 是否支持并行
	pathnode->path.parallel_safe = rel->consider_parallel;               // 是否并行安全
	pathnode->path.parallel_workers = parallel_degree;                   // 并行工作者数量
	pathnode->path.pathkeys = NIL;                                      // 位图堆扫描结果无序

	pathnode->bitmapqual = bitmapqual;                                  // 设置位图条件路径

	// 计算位图堆扫描的成本
	cost_bitmap_heap_scan(&pathnode->path, root, rel,
						 pathnode->path.param_info,
						 bitmapqual, loop_count);

	return pathnode;    // 返回创建的路径节点
}

/*
 * create_bitmap_and_path
 *	  Creates a path node representing a BitmapAnd.
 */
BitmapAndPath *
create_bitmap_and_path(PlannerInfo *root,
					   RelOptInfo *rel,
					   List *bitmapquals)
{
	BitmapAndPath *pathnode = makeNode(BitmapAndPath);
	Relids		required_outer = NULL;
	ListCell   *lc;

	pathnode->path.pathtype = T_BitmapAnd;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = rel->reltarget;

	/*
	 * Identify the required outer rels as the union of what the child paths
	 * depend on.  (Alternatively, we could insist that the caller pass this
	 * in, but it's more convenient and reliable to compute it here.)
	 */
	foreach(lc, bitmapquals)
	{
		Path	   *bitmapqual = (Path *) lfirst(lc);

		required_outer = bms_add_members(required_outer,
										 PATH_REQ_OUTER(bitmapqual));
	}
	pathnode->path.param_info = get_baserel_parampathinfo(root, rel,
														  required_outer);

	/*
	 * Currently, a BitmapHeapPath, BitmapAndPath, or BitmapOrPath will be
	 * parallel-safe if and only if rel->consider_parallel is set.  So, we can
	 * set the flag for this path based only on the relation-level flag,
	 * without actually iterating over the list of children.
	 */
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_workers = 0;

	pathnode->path.pathkeys = NIL;	/* always unordered */

	pathnode->bitmapquals = bitmapquals;

	/* this sets bitmapselectivity as well as the regular cost fields: */
	cost_bitmap_and_node(pathnode, root);

	return pathnode;
}

/*
 * create_bitmap_or_path
 *	  Creates a path node representing a BitmapOr.
 */
BitmapOrPath *
create_bitmap_or_path(PlannerInfo *root,
					  RelOptInfo *rel,
					  List *bitmapquals)
{
	BitmapOrPath *pathnode = makeNode(BitmapOrPath);
	Relids		required_outer = NULL;
	ListCell   *lc;

	pathnode->path.pathtype = T_BitmapOr;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = rel->reltarget;

	/*
	 * Identify the required outer rels as the union of what the child paths
	 * depend on.  (Alternatively, we could insist that the caller pass this
	 * in, but it's more convenient and reliable to compute it here.)
	 */
	foreach(lc, bitmapquals)
	{
		Path	   *bitmapqual = (Path *) lfirst(lc);

		required_outer = bms_add_members(required_outer,
										 PATH_REQ_OUTER(bitmapqual));
	}
	pathnode->path.param_info = get_baserel_parampathinfo(root, rel,
														  required_outer);

	/*
	 * Currently, a BitmapHeapPath, BitmapAndPath, or BitmapOrPath will be
	 * parallel-safe if and only if rel->consider_parallel is set.  So, we can
	 * set the flag for this path based only on the relation-level flag,
	 * without actually iterating over the list of children.
	 */
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_workers = 0;

	pathnode->path.pathkeys = NIL;	/* always unordered */

	pathnode->bitmapquals = bitmapquals;

	/* this sets bitmapselectivity as well as the regular cost fields: */
	cost_bitmap_or_node(pathnode, root);

	return pathnode;
}

/*
 * create_tidscan_path
 *	  Creates a path corresponding to a scan by TID, returning the pathnode.
 */
TidPath *
create_tidscan_path(PlannerInfo *root, RelOptInfo *rel, List *tidquals,
					Relids required_outer)
{
	TidPath    *pathnode = makeNode(TidPath);

	pathnode->path.pathtype = T_TidScan;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = rel->reltarget;
	pathnode->path.param_info = get_baserel_parampathinfo(root, rel,
														  required_outer);
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_workers = 0;
	pathnode->path.pathkeys = NIL;	/* always unordered */

	pathnode->tidquals = tidquals;

	cost_tidscan(&pathnode->path, root, rel, tidquals,
				 pathnode->path.param_info);

	return pathnode;
}

/*
 * create_append_path
 *	  Creates a path corresponding to an Append plan, returning the
 *	  pathnode.
 *
 * Note that we must handle subpaths = NIL, representing a dummy access path.
 * Also, there are callers that pass root = NULL.
 */
AppendPath *
create_append_path(PlannerInfo *root,
				   RelOptInfo *rel,
				   List *subpaths, List *partial_subpaths,
				   List *pathkeys, Relids required_outer,
				   int parallel_workers, bool parallel_aware,
				   List *partitioned_rels, double rows)
{
	AppendPath *pathnode = makeNode(AppendPath);
	ListCell   *l;

	Assert(!parallel_aware || parallel_workers > 0);

	pathnode->path.pathtype = T_Append;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = rel->reltarget;

	/*
	 * When generating an Append path for a partitioned table, there may be
	 * parameters that are useful so we can eliminate certain partitions
	 * during execution.  Here we'll go all the way and fully populate the
	 * parameter info data as we do for normal base relations.  However, we
	 * need only bother doing this for RELOPT_BASEREL rels, as
	 * RELOPT_OTHER_MEMBER_REL's Append paths are merged into the base rel's
	 * Append subpaths.  It would do no harm to do this, we just avoid it to
	 * save wasting effort.
	 */
	if (partitioned_rels != NIL && root && rel->reloptkind == RELOPT_BASEREL)
		pathnode->path.param_info = get_baserel_parampathinfo(root,
															  rel,
															  required_outer);
	else
		pathnode->path.param_info = get_appendrel_parampathinfo(rel,
																required_outer);

	pathnode->path.parallel_aware = parallel_aware;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_workers = parallel_workers;
	pathnode->path.pathkeys = pathkeys;
	pathnode->partitioned_rels = list_copy(partitioned_rels);

	/*
	 * For parallel append, non-partial paths are sorted by descending total
	 * costs. That way, the total time to finish all non-partial paths is
	 * minimized.  Also, the partial paths are sorted by descending startup
	 * costs.  There may be some paths that require to do startup work by a
	 * single worker.  In such case, it's better for workers to choose the
	 * expensive ones first, whereas the leader should choose the cheapest
	 * startup plan.
	 */
	if (pathnode->path.parallel_aware)
	{
		/*
		 * We mustn't fiddle with the order of subpaths when the Append has
		 * pathkeys.  The order they're listed in is critical to keeping the
		 * pathkeys valid.
		 */
		Assert(pathkeys == NIL);

		subpaths = list_qsort(subpaths, append_total_cost_compare);
		partial_subpaths = list_qsort(partial_subpaths,
									  append_startup_cost_compare);
	}
	pathnode->first_partial_path = list_length(subpaths);
	pathnode->subpaths = list_concat(subpaths, partial_subpaths);

	/*
	 * Apply query-wide LIMIT if known and path is for sole base relation.
	 * (Handling this at this low level is a bit klugy.)
	 */
	if (root != NULL && bms_equal(rel->relids, root->all_baserels))
		pathnode->limit_tuples = root->limit_tuples;
	else
		pathnode->limit_tuples = -1.0;

	foreach(l, pathnode->subpaths)
	{
		Path	   *subpath = (Path *) lfirst(l);

		pathnode->path.parallel_safe = pathnode->path.parallel_safe &&
			subpath->parallel_safe;

		/* All child paths must have same parameterization */
		Assert(bms_equal(PATH_REQ_OUTER(subpath), required_outer));
	}

	Assert(!parallel_aware || pathnode->path.parallel_safe);

	/*
	 * If there's exactly one child path, the Append is a no-op and will be
	 * discarded later (in setrefs.c); therefore, we can inherit the child's
	 * size and cost, as well as its pathkeys if any (overriding whatever the
	 * caller might've said).  Otherwise, we must do the normal costsize
	 * calculation.
	 */
	if (list_length(pathnode->subpaths) == 1)
	{
		Path	   *child = (Path *) linitial(pathnode->subpaths);

		pathnode->path.rows = child->rows;
		pathnode->path.startup_cost = child->startup_cost;
		pathnode->path.total_cost = child->total_cost;
		pathnode->path.pathkeys = child->pathkeys;
	}
	else
		cost_append(pathnode);

	/* If the caller provided a row estimate, override the computed value. */
	if (rows >= 0)
		pathnode->path.rows = rows;

	return pathnode;
}

/*
 * append_total_cost_compare
 *	  qsort comparator for sorting append child paths by total_cost descending
 *
 * For equal total costs, we fall back to comparing startup costs; if those
 * are equal too, break ties using bms_compare on the paths' relids.
 * (This is to avoid getting unpredictable results from qsort.)
 */
static int
append_total_cost_compare(const void *a, const void *b)
{
	Path	   *path1 = (Path *) lfirst(*(ListCell **) a);
	Path	   *path2 = (Path *) lfirst(*(ListCell **) b);
	int			cmp;

	cmp = compare_path_costs(path1, path2, TOTAL_COST);
	if (cmp != 0)
		return -cmp;
	return bms_compare(path1->parent->relids, path2->parent->relids);
}

/*
 * append_startup_cost_compare
 *	  qsort comparator for sorting append child paths by startup_cost descending
 *
 * For equal startup costs, we fall back to comparing total costs; if those
 * are equal too, break ties using bms_compare on the paths' relids.
 * (This is to avoid getting unpredictable results from qsort.)
 */
static int
append_startup_cost_compare(const void *a, const void *b)
{
	Path	   *path1 = (Path *) lfirst(*(ListCell **) a);
	Path	   *path2 = (Path *) lfirst(*(ListCell **) b);
	int			cmp;

	cmp = compare_path_costs(path1, path2, STARTUP_COST);
	if (cmp != 0)
		return -cmp;
	return bms_compare(path1->parent->relids, path2->parent->relids);
}

/*
 * create_merge_append_path
 *	  Creates a path corresponding to a MergeAppend plan, returning the
 *	  pathnode.
 */
MergeAppendPath *
create_merge_append_path(PlannerInfo *root,
						 RelOptInfo *rel,
						 List *subpaths,
						 List *pathkeys,
						 Relids required_outer,
						 List *partitioned_rels)
{
	MergeAppendPath *pathnode = makeNode(MergeAppendPath);
	Cost		input_startup_cost;
	Cost		input_total_cost;
	ListCell   *l;

	pathnode->path.pathtype = T_MergeAppend;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = rel->reltarget;
	pathnode->path.param_info = get_appendrel_parampathinfo(rel,
															required_outer);
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_workers = 0;
	pathnode->path.pathkeys = pathkeys;
	pathnode->partitioned_rels = list_copy(partitioned_rels);
	pathnode->subpaths = subpaths;

	/*
	 * Apply query-wide LIMIT if known and path is for sole base relation.
	 * (Handling this at this low level is a bit klugy.)
	 */
	if (bms_equal(rel->relids, root->all_baserels))
		pathnode->limit_tuples = root->limit_tuples;
	else
		pathnode->limit_tuples = -1.0;

	/*
	 * Add up the sizes and costs of the input paths.
	 */
	pathnode->path.rows = 0;
	input_startup_cost = 0;
	input_total_cost = 0;
	foreach(l, subpaths)
	{
		Path	   *subpath = (Path *) lfirst(l);

		pathnode->path.rows += subpath->rows;
		pathnode->path.parallel_safe = pathnode->path.parallel_safe &&
			subpath->parallel_safe;

		if (pathkeys_contained_in(pathkeys, subpath->pathkeys))
		{
			/* Subpath is adequately ordered, we won't need to sort it */
			input_startup_cost += subpath->startup_cost;
			input_total_cost += subpath->total_cost;
		}
		else
		{
			/* We'll need to insert a Sort node, so include cost for that */
			Path		sort_path;	/* dummy for result of cost_sort */

			cost_sort(&sort_path,
					  root,
					  pathkeys,
					  subpath->total_cost,
					  subpath->parent->tuples,
					  subpath->pathtarget->width,
					  0.0,
					  work_mem,
					  pathnode->limit_tuples);
			input_startup_cost += sort_path.startup_cost;
			input_total_cost += sort_path.total_cost;
		}

		/* All child paths must have same parameterization */
		Assert(bms_equal(PATH_REQ_OUTER(subpath), required_outer));
	}

	/*
	 * Now we can compute total costs of the MergeAppend.  If there's exactly
	 * one child path, the MergeAppend is a no-op and will be discarded later
	 * (in setrefs.c); otherwise we do the normal cost calculation.
	 */
	if (list_length(subpaths) == 1)
	{
		pathnode->path.startup_cost = input_startup_cost;
		pathnode->path.total_cost = input_total_cost;
	}
	else
		cost_merge_append(&pathnode->path, root,
						  pathkeys, list_length(subpaths),
						  input_startup_cost, input_total_cost,
						  pathnode->path.rows);

	return pathnode;
}

/*
 * create_group_result_path
 *	  Creates a path representing a Result-and-nothing-else plan.
 *
 * This is only used for degenerate grouping cases, in which we know we
 * need to produce one result row, possibly filtered by a HAVING qual.
 */
GroupResultPath *
create_group_result_path(PlannerInfo *root, RelOptInfo *rel,
						 PathTarget *target, List *havingqual)
{
	GroupResultPath *pathnode = makeNode(GroupResultPath);

	pathnode->path.pathtype = T_Result;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target;
	pathnode->path.param_info = NULL;	/* there are no other rels... */
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_workers = 0;
	pathnode->path.pathkeys = NIL;
	pathnode->quals = havingqual;

	/*
	 * We can't quite use cost_resultscan() because the quals we want to
	 * account for are not baserestrict quals of the rel.  Might as well just
	 * hack it here.
	 */
	pathnode->path.rows = 1;
	pathnode->path.startup_cost = target->cost.startup;
	pathnode->path.total_cost = target->cost.startup +
		cpu_tuple_cost + target->cost.per_tuple;

	/*
	 * Add cost of qual, if any --- but we ignore its selectivity, since our
	 * rowcount estimate should be 1 no matter what the qual is.
	 */
	if (havingqual)
	{
		QualCost	qual_cost;

		cost_qual_eval(&qual_cost, havingqual, root);
		/* havingqual is evaluated once at startup */
		pathnode->path.startup_cost += qual_cost.startup + qual_cost.per_tuple;
		pathnode->path.total_cost += qual_cost.startup + qual_cost.per_tuple;
	}

	return pathnode;
}

/*
 * create_material_path
 *	  Creates a path corresponding to a Material plan, returning the
 *	  pathnode.
 */
MaterialPath *
create_material_path(RelOptInfo *rel, Path *subpath)
{
	MaterialPath *pathnode = makeNode(MaterialPath);

	Assert(subpath->parent == rel);

	pathnode->path.pathtype = T_Material;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = rel->reltarget;
	pathnode->path.param_info = subpath->param_info;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_workers = subpath->parallel_workers;
	pathnode->path.pathkeys = subpath->pathkeys;

	pathnode->subpath = subpath;

	cost_material(&pathnode->path,
				  subpath->startup_cost,
				  subpath->total_cost,
				  subpath->rows,
				  subpath->pathtarget->width);

	return pathnode;
}

/*
 * create_unique_path
 *	  创建一个用于消除输入数据中重复行的 Unique 路径，放入 rel->cheapest_unique_path。
 *	  唯一性根据 sjinfo 所表示的半连接需求定义。
 *	  如果无法识别如何使数据唯一，则返回 NULL。
 *
 * 如果使用该函数，通常会在同一个 rel 上重复调用，并且输入 subpath 应始终相同（即 rel 的 cheapest_total 路径）。
 * 因此我们会缓存结果。
 */
UniquePath *
create_unique_path(PlannerInfo *root, RelOptInfo *rel, Path *subpath,
				   SpecialJoinInfo *sjinfo)
{
	UniquePath *pathnode;
	Path		sort_path;		/* 用于保存排序成本的临时变量 */
	Path		agg_path;		/* 用于保存聚合成本的临时变量 */
	MemoryContext oldcontext;
	int			numCols;

	/* 如果 subpath 不是 cheapest_total，则调用者有误 ... */
	Assert(subpath == rel->cheapest_total_path);
	Assert(subpath->parent == rel);
	/* ... 或者 SpecialJoinInfo 不正确 */
	Assert(sjinfo->jointype == JOIN_SEMI);
	Assert(bms_equal(rel->relids, sjinfo->syn_righthand));

	/* 如果结果已缓存，则直接返回 */
	if (rel->cheapest_unique_path)
		return (UniquePath *) rel->cheapest_unique_path;

	/* 判定连接约束语句中的操作符。如果无法使用 B-tree 或 Hash 方法唯一化，则返回 NULL */
	if (!(sjinfo->semi_can_btree || sjinfo->semi_can_hash))
		return NULL;

	/*
	 * 在 GEQO 连接规划期间，当前处于短生命周期的内存上下文。
	 * 必须确保为 baserel 创建的路径及其相关数据结构能在 GEQO 周期内存活，否则 baserel 会在后续 GEQO 周期被破坏。
	 * 另一方面，在 GEQO 期间为 joinrel 创建的结构不应污染主规划上下文。
	 * 最佳方案是显式地在 rel 所在的内存上下文中分配内存。
	 */
	oldcontext = MemoryContextSwitchTo(GetMemoryChunkContext(rel));

	pathnode = makeNode(UniquePath);

	pathnode->path.pathtype = T_Unique;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = rel->reltarget;
	pathnode->path.param_info = subpath->param_info;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_workers = subpath->parallel_workers;

	/*
	 * 假定输出是无序的，因为我们不一定有 pathkeys 来表示它。（下面可能会被覆盖。）
	 */
	pathnode->path.pathkeys = NIL;

	pathnode->subpath = subpath;

	/*
	 * 在 GEQO 下，sjinfo 可能是短生命周期的，所以需要复制从中提取的数据结构。
	 */
	pathnode->in_operators = copyObject(sjinfo->semi_operators);
	pathnode->uniq_exprs = copyObject(sjinfo->semi_rhs_exprs);

	/*
	 * 如果输入是一个关系，并且它有唯一索引能证明 semi_rhs_exprs 是唯一的，则无需做任何处理。
	 * 注意 relation_has_unique_index_for 会自动考虑该关系的限制条件。
	 */
	if (rel->rtekind == RTE_RELATION && sjinfo->semi_can_btree &&
		relation_has_unique_index_for(root, rel, NIL,
									  sjinfo->semi_rhs_exprs,
									  sjinfo->semi_operators))
	{
		pathnode->umethod = UNIQUE_PATH_NOOP;	/* 无需唯一化 */
		pathnode->path.rows = rel->rows;
		pathnode->path.startup_cost = subpath->startup_cost;
		pathnode->path.total_cost = subpath->total_cost;
		pathnode->path.pathkeys = subpath->pathkeys;

		rel->cheapest_unique_path = (Path *) pathnode;

		MemoryContextSwitchTo(oldcontext);

		return pathnode;
	}

	/*
	 * 如果输入是一个子查询且输出本身已经唯一，则无需做任何处理。
	 * 唯一性测试必须考虑具体提取了哪些列，例如 "SELECT DISTINCT x,y" 并不保证 x 唯一。
	 * 所以只有当 semi_rhs_exprs 仅包含引用子查询输出的简单 Var 时才能做此优化。
	 * （理论上可以处理表达式，但目前保持简单。）
	 */
	if (rel->rtekind == RTE_SUBQUERY)
	{
		RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);

		if (query_supports_distinctness(rte->subquery))
		{
			List	   *sub_tlist_colnos;

			sub_tlist_colnos = translate_sub_tlist(sjinfo->semi_rhs_exprs,
												   rel->relid);

			if (sub_tlist_colnos &&
				query_is_distinct_for(rte->subquery,
									  sub_tlist_colnos,
									  sjinfo->semi_operators))
			{
				pathnode->umethod = UNIQUE_PATH_NOOP;	/* 无需唯一化 */
				pathnode->path.rows = rel->rows;
				pathnode->path.startup_cost = subpath->startup_cost;
				pathnode->path.total_cost = subpath->total_cost;
				pathnode->path.pathkeys = subpath->pathkeys;

				rel->cheapest_unique_path = (Path *) pathnode;

				MemoryContextSwitchTo(oldcontext);

				return pathnode;
			}
		}
	}

	/* 估算输出行数 */
	pathnode->path.rows = estimate_num_groups(root,
											  sjinfo->semi_rhs_exprs,
											  rel->rows,
											  NULL);
	numCols = list_length(sjinfo->semi_rhs_exprs);

	/*
	 * 估算排序实现 Unique 的成本
	 */
	if (sjinfo->semi_can_btree)
	{
		/*
		 * 估算排序+唯一实现的成本
		 */
		cost_sort(&sort_path, root, NIL,
				  subpath->total_cost,
				  rel->rows,
				  subpath->pathtarget->width,
				  0.0,
				  work_mem,
				  -1.0);

		/*
		 * 每个输入元组每列比较一次，按 cpu_operator_cost 计费。
		 * 假定所有列都在大多数元组上被比较。（可能高估了。）
		 * 这应与 create_upper_unique_path 保持一致。
		 */
		sort_path.total_cost += cpu_operator_cost * rel->rows * numCols;
	}

	/*
	 * 估算哈希实现 Unique 的成本
	 */
	if (sjinfo->semi_can_hash)
	{
		/*
		 * 估算每个哈希表项的开销为 64 字节（与 planner.c 保持一致）。
		 */
		int			hashentrysize = subpath->pathtarget->width + 64;

		if (hashentrysize * pathnode->path.rows > work_mem * 1024L)
		{
			/*
			 * 不应尝试哈希。将此信息记录到 SpecialJoinInfo，以便下次调用时使用。
			 */
			sjinfo->semi_can_hash = false;
		}
		else
			cost_agg(&agg_path, root,
					 AGG_HASHED, NULL,
					 numCols, pathnode->path.rows,
					 NIL,
					 subpath->startup_cost,
					 subpath->total_cost,
					 rel->rows);
	}

	/*
	 * 选择成本更低的方法。
	 */
	if (sjinfo->semi_can_btree && sjinfo->semi_can_hash)
	{
		if (agg_path.total_cost < sort_path.total_cost)
			pathnode->umethod = UNIQUE_PATH_HASH;	/* 选择希方法实现 Unique */
		else
			pathnode->umethod = UNIQUE_PATH_SORT;	/* 选择排序方法实现 Unique */
	}
	else if (sjinfo->semi_can_btree)
		pathnode->umethod = UNIQUE_PATH_SORT;	/* 只能使用排序方法实现 Unique */
	else if (sjinfo->semi_can_hash)
		pathnode->umethod = UNIQUE_PATH_HASH;	/* 只能使用哈希方法实现 Unique */
	else
	{
		/* 只有在上面放弃哈希时才会到这里 */
		MemoryContextSwitchTo(oldcontext);
		return NULL;
	}

	/*
	 * 如果选择了哈希方法，则路径的成本就是聚合路径的成本；
	 * 否则就是排序路径的成本。
	 */
	if (pathnode->umethod == UNIQUE_PATH_HASH)
	{
		pathnode->path.startup_cost = agg_path.startup_cost;
		pathnode->path.total_cost = agg_path.total_cost;
	}
	else
	{
		pathnode->path.startup_cost = sort_path.startup_cost;
		pathnode->path.total_cost = sort_path.total_cost;
	}

	/* 将路径节点添加到 RelOptInfo 的 cheapest_unique_path 中 */
	rel->cheapest_unique_path = (Path*)pathnode;

	MemoryContextSwitchTo(oldcontext);

	return pathnode;
}

/*
 * create_gather_merge_path
 *
 *    创建一个对应于gather merge扫描的路径，返回路径节点。
 *    Gather Merge是PostgreSQL并行查询执行中的一种操作，用于合并来自多个工作进程的已排序结果。
 *    与普通Gather不同，Gather Merge要求子路径已经按指定顺序排序，以便工作进程返回的结果
 *    可以有效地合并成最终的有序结果集。
 */
GatherMergePath *
create_gather_merge_path(PlannerInfo *root, RelOptInfo *rel, Path *subpath,
                        PathTarget *target, List *pathkeys,
                        Relids required_outer, double *rows)
{
    /* 分配并初始化GatherMergePath节点 */
    GatherMergePath *pathnode = makeNode(GatherMergePath);
    /* 初始化输入成本变量 */
    Cost        input_startup_cost = 0;
    Cost        input_total_cost = 0;

    /* 断言检查：确保子路径支持并行执行 */
    Assert(subpath->parallel_safe);
    /* 断言检查：确保指定了排序键 */
    Assert(pathkeys);

    /* 设置路径节点的基本属性 */
    /* 设置路径类型为Gather Merge操作 */
    pathnode->path.pathtype = T_GatherMerge;
    /* 指定父关系 */
    pathnode->path.parent = rel;
    /* 获取并设置参数信息（关于外部关系的参数化需求） */
    pathnode->path.param_info = get_baserel_parampathinfo(root, rel,
                                                      required_outer);
    /* 设置并行感知标志为false，Gather Merge不需要特殊的并行协调 */
    pathnode->path.parallel_aware = false;

    /* 设置Gather Merge特有的属性 */
    /* 指定子路径 */
    pathnode->subpath = subpath;
    /* 从子路径继承并行工作进程数量 */
    pathnode->num_workers = subpath->parallel_workers;
    /* 设置排序键，这是Gather Merge操作的关键，决定如何合并来自不同工作进程的结果 */
    pathnode->path.pathkeys = pathkeys;
    /* 设置路径目标，如未指定则使用关系的默认目标 */
    pathnode->path.pathtarget = target ? target : rel->reltarget;
    /* 设置行数估计值 */
    pathnode->path.rows += subpath->rows;

    /* 判断子路径是否已经按所需顺序排序 */
    if (pathkeys_contained_in(pathkeys, subpath->pathkeys))
    {
        /* 子路径已满足排序要求，无需额外排序操作 */
        /* 直接使用子路径的成本 */
        input_startup_cost += subpath->startup_cost;
        input_total_cost += subpath->total_cost;
    }
    else
    {
        /* 子路径不满足排序要求，需要插入Sort节点 */
        /* 声明一个虚拟路径节点用于接收cost_sort的计算结果 */
        Path        sort_path;    /* dummy for result of cost_sort */

        /* 计算排序操作的成本 */
        cost_sort(&sort_path,
                root,
                pathkeys,
                subpath->total_cost,  /* 排序前的总成本作为排序的输入成本 */
                subpath->rows,        /* 排序的行数 */
                subpath->pathtarget->width,  /* 每行宽度 */
                0.0,                  /* 无额外CPU开销 */
                work_mem,             /* 可用工作内存 */
                -1);                  /* 无特殊排序类型 */
        /* 累加排序成本 */
        input_startup_cost += sort_path.startup_cost;
        input_total_cost += sort_path.total_cost;
    }

    /* 计算Gather Merge操作本身的成本，合并所有子路径的成本估计 */
    cost_gather_merge(pathnode, root, rel, pathnode->path.param_info,
                    input_startup_cost, input_total_cost, rows);

    /* 返回构建好的GatherMergePath节点 */
    return pathnode;
}


/*
 * translate_sub_tlist - 从目标列表中提取由tlist表示的子查询列号
 *
 * 参数：
 *   tlist - 目标列表，通常只包含引用指定relid的Var节点
 *   relid - 要查找引用的关系ID（RT索引）
 *
 * 返回值：
 *   如果目标列表仅包含简单的Var引用，则返回这些Var的varattno（即子查询的列号）组成的整数列表；
 *   如果任何目标列表项不是简单的Var，或者引用了不同的关系，则返回NIL
 *
 * 功能说明：
 *   该函数用于提取目标列表中引用特定子查询关系的列号。这在查询优化过程中，特别是在判断子查询结果
 *   是否具有唯一性时非常重要，因为只有当引用的列是简单的属性引用时，才能可靠地比较唯一性条件。
 */
static List *
translate_sub_tlist(List *tlist, int relid)
{
	List	   *result = NIL;  /* 存储提取的列号列表 */
	ListCell   *l;  /* 列表遍历指针 */

	/* 遍历目标列表中的每个元素 */
	foreach(l, tlist)
	{
		Var	   *var = (Var *) lfirst(l);  /* 假设当前元素是Var节点 */

		/*
		 * 验证当前元素是否为有效的Var节点，且引用了指定的关系ID：
		 * 1. var不能为NULL
		 * 2. 必须是Var类型的节点
		 * 3. 变量的关系编号必须与指定的relid匹配
		 * 如果不满足任何一个条件，则无法确定子查询的唯一性条件是否匹配，返回NIL
		 */
		if (!var || !IsA(var, Var) || var->varno != relid)
			return NIL;  /* 无法处理，放弃并返回空列表 */

		/* 将有效的列号添加到结果列表中 */
		result = lappend_int(result, var->varattno);
	}

	/* 返回收集到的列号列表 */
	return result;
}


/*
 * create_gather_path
 *	  Creates a path corresponding to a gather scan, returning the
 *	  pathnode.
 *
 * 'rows' may optionally be set to override row estimates from other sources.
 */
GatherPath *
create_gather_path(PlannerInfo *root, RelOptInfo *rel, Path *subpath,
				   PathTarget *target, Relids required_outer, double *rows)
{
	GatherPath *pathnode = makeNode(GatherPath);

	Assert(subpath->parallel_safe);

	pathnode->path.pathtype = T_Gather;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target;
	pathnode->path.param_info = get_baserel_parampathinfo(root, rel,
														  required_outer);
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = false;
	pathnode->path.parallel_workers = 0;
	pathnode->path.pathkeys = NIL;	/* Gather has unordered result */

	pathnode->subpath = subpath;
	pathnode->num_workers = subpath->parallel_workers;
	pathnode->single_copy = false;

	if (pathnode->num_workers == 0)
	{
		pathnode->path.pathkeys = subpath->pathkeys;
		pathnode->num_workers = 1;
		pathnode->single_copy = true;
	}

	cost_gather(pathnode, root, rel, pathnode->path.param_info, rows);

	return pathnode;
}

/*
 * create_subqueryscan_path
 *    创建对应于子查询扫描的路径，返回路径节点
 * 
 * 参数说明：
 *    root - 规划器全局信息结构体指针，包含查询优化过程中的所有上下文信息
 *    rel - 关系优化信息结构体指针，表示当前子查询对应的关系
 *    subpath - 子查询的执行路径指针，代表子查询本身的访问路径
 *    pathkeys - 路径键列表，表示该路径返回结果的排序顺序
 *    required_outer - 位图集合，表示路径所需的外部关系ID（用于参数化）
 * 
 * 返回值：
 *    SubqueryScanPath* - 返回创建的子查询扫描路径节点
 * 
 * 功能说明：
 *    此函数用于创建表示子查询扫描操作的路径节点。子查询扫描是PostgreSQL
 *    执行计划中处理子查询的一种方式，将子查询作为一个关系进行扫描。
 */
SubqueryScanPath *
create_subqueryscan_path(PlannerInfo *root, RelOptInfo *rel, Path *subpath,
                        List *pathkeys, Relids required_outer)
{
    SubqueryScanPath *pathnode = makeNode(SubqueryScanPath);  /* 创建子查询扫描路径节点 */

    /* 初始化路径基本属性 */
    pathnode->path.pathtype = T_SubqueryScan;  /* 设置路径类型为子查询扫描 */
    pathnode->path.parent = rel;  /* 设置父关系 */
    pathnode->path.pathtarget = rel->reltarget;  /* 设置路径的目标列表（输出字段） */
    
    /* 获取并设置参数化信息 */
    pathnode->path.param_info = get_baserel_parampathinfo(root, rel,
                                                          required_outer);
    
    /* 设置并行执行相关属性 */
    pathnode->path.parallel_aware = false;  /* 子查询扫描不支持并行感知 */
    pathnode->path.parallel_safe = rel->consider_parallel &&  /* 并行安全性取决于父关系和子路径 */
        subpath->parallel_safe;
    pathnode->path.parallel_workers = subpath->parallel_workers;  /* 继承子路径的并行工作线程数 */
    
    /* 设置路径键（排序信息） */
    pathnode->path.pathkeys = pathkeys;  /* 设置返回结果的排序顺序 */
    
    /* 设置子路径 */
    pathnode->subpath = subpath;  /* 保存子查询的执行路径 */

    /* 计算子查询扫描路径的成本 */
    cost_subqueryscan(pathnode, root, rel, pathnode->path.param_info);

    return pathnode;  /* 返回创建的路径节点 */
}


/*
 * create_functionscan_path
 *	  Creates a path corresponding to a sequential scan of a function,
 *	  returning the pathnode.
 */
Path *
create_functionscan_path(PlannerInfo *root, RelOptInfo *rel,
						 List *pathkeys, Relids required_outer)
{
	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_FunctionScan;
	pathnode->parent = rel;
	pathnode->pathtarget = rel->reltarget;
	pathnode->param_info = get_baserel_parampathinfo(root, rel,
													 required_outer);
	pathnode->parallel_aware = false;
	pathnode->parallel_safe = rel->consider_parallel;
	pathnode->parallel_workers = 0;
	pathnode->pathkeys = pathkeys;

	cost_functionscan(pathnode, root, rel, pathnode->param_info);

	return pathnode;
}

/*
 * create_tablefuncscan_path
 *	  Creates a path corresponding to a sequential scan of a table function,
 *	  returning the pathnode.
 */
Path *
create_tablefuncscan_path(PlannerInfo *root, RelOptInfo *rel,
						  Relids required_outer)
{
	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_TableFuncScan;
	pathnode->parent = rel;
	pathnode->pathtarget = rel->reltarget;
	pathnode->param_info = get_baserel_parampathinfo(root, rel,
													 required_outer);
	pathnode->parallel_aware = false;
	pathnode->parallel_safe = rel->consider_parallel;
	pathnode->parallel_workers = 0;
	pathnode->pathkeys = NIL;	/* result is always unordered */

	cost_tablefuncscan(pathnode, root, rel, pathnode->param_info);

	return pathnode;
}

/*
 * create_valuesscan_path
 *	  Creates a path corresponding to a scan of a VALUES list,
 *	  returning the pathnode.
 */
Path *
create_valuesscan_path(PlannerInfo *root, RelOptInfo *rel,
					   Relids required_outer)
{
	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_ValuesScan;
	pathnode->parent = rel;
	pathnode->pathtarget = rel->reltarget;
	pathnode->param_info = get_baserel_parampathinfo(root, rel,
													 required_outer);
	pathnode->parallel_aware = false;
	pathnode->parallel_safe = rel->consider_parallel;
	pathnode->parallel_workers = 0;
	pathnode->pathkeys = NIL;	/* result is always unordered */

	cost_valuesscan(pathnode, root, rel, pathnode->param_info);

	return pathnode;
}

/*
 * create_ctescan_path
 *      创建对应非自引用CTE（公共表表达式）扫描的访问路径，并返回路径节点
 *      
 * 参数说明：
 *      root - 规划器全局信息结构体指针，包含查询优化过程中的所有上下文信息
 *      rel - 关系优化信息结构体指针，表示当前需要构建访问路径的CTE关系
 *      required_outer - 必需的外部关系ID集合，表示此路径执行时需要先计算的外部关系
 *
 * 返回值：
 *      Path* - 指向创建好的CTE扫描路径节点的指针
 *
 * 功能说明：
 *      此函数负责创建表示CTE扫描操作的路径节点，设置路径的基本属性，并计算其执行成本。
 *      CTE扫描路径代表对物化的CTE结果集进行扫描的操作，是PostgreSQL查询优化器中
 *      处理公共表表达式的关键组件。
 */
Path *
create_ctescan_path(PlannerInfo *root, RelOptInfo *rel, Relids required_outer)
{
    /* 创建一个新的Path节点作为CTE扫描路径的基础结构 */
    Path	   *pathnode = makeNode(Path);

    /* 设置路径类型为CTE扫描 */
    pathnode->pathtype = T_CteScan;
    
    /* 设置此路径所属的关系 */
    pathnode->parent = rel;
    
    /* 设置路径的目标列表，直接使用关系的目标列表 */
    pathnode->pathtarget = rel->reltarget;
    
    /* 获取并设置参数化路径信息，处理外部参数依赖 */
    pathnode->param_info = get_baserel_parampathinfo(root, rel, required_outer);
    
    /* 设置并行执行相关属性 */
    pathnode->parallel_aware = false;  /* CTE扫描不是并行感知的 */
    
    /* CTE扫描是否并行安全取决于关系是否允许并行考虑 */
    pathnode->parallel_safe = rel->consider_parallel;
    
    /* CTE扫描目前不使用并行工作进程，设置为0 */
    pathnode->parallel_workers = 0;
    
    /* 
     * 设置路径键（排序顺序）为空列表NIL
     * 注释中的XXX表示这是一个临时限制，目前CTE扫描的结果总是无序的
     */
    pathnode->pathkeys = NIL;  /* XXX for now, result is always unordered */

    /* 计算CTE扫描路径的执行成本（包括启动成本和总执行成本） */
    cost_ctescan(pathnode, root, rel, pathnode->param_info);

    /* 返回创建好的CTE扫描路径节点 */
    return pathnode;
}


/*
 * create_namedtuplestorescan_path
 *	  Creates a path corresponding to a scan of a named tuplestore, returning
 *	  the pathnode.
 */
Path *
create_namedtuplestorescan_path(PlannerInfo *root, RelOptInfo *rel,
								Relids required_outer)
{
	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_NamedTuplestoreScan;
	pathnode->parent = rel;
	pathnode->pathtarget = rel->reltarget;
	pathnode->param_info = get_baserel_parampathinfo(root, rel,
													 required_outer);
	pathnode->parallel_aware = false;
	pathnode->parallel_safe = rel->consider_parallel;
	pathnode->parallel_workers = 0;
	pathnode->pathkeys = NIL;	/* result is always unordered */

	cost_namedtuplestorescan(pathnode, root, rel, pathnode->param_info);

	return pathnode;
}

/*
 * create_resultscan_path
 *	  Creates a path corresponding to a scan of an RTE_RESULT relation,
 *	  returning the pathnode.
 */
Path *
create_resultscan_path(PlannerInfo *root, RelOptInfo *rel,
					   Relids required_outer)
{
	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_Result;
	pathnode->parent = rel;
	pathnode->pathtarget = rel->reltarget;
	pathnode->param_info = get_baserel_parampathinfo(root, rel,
													 required_outer);
	pathnode->parallel_aware = false;
	pathnode->parallel_safe = rel->consider_parallel;
	pathnode->parallel_workers = 0;
	pathnode->pathkeys = NIL;	/* result is always unordered */

	cost_resultscan(pathnode, root, rel, pathnode->param_info);

	return pathnode;
}

/*
 * create_worktablescan_path
 *      创建对应于自引用CTE扫描的路径，返回路径节点
 *      此函数用于为递归CTE的工作表扫描构建路径节点
 */
Path *
create_worktablescan_path(PlannerInfo *root, RelOptInfo *rel,
                          Relids required_outer)
{
    Path       *pathnode = makeNode(Path);  	/* 创建新的路径节点 */

    pathnode->pathtype = T_WorkTableScan;    	/* 设置路径类型为工作表扫描 */
    pathnode->parent = rel;                   	/* 设置父关系 */
    pathnode->pathtarget = rel->reltarget;   	/* 设置路径目标为关系的目标列表 */
    pathnode->param_info = get_baserel_parampathinfo(root, rel,
                                                     required_outer);  /* 获取参数化路径信息 */
    pathnode->parallel_aware = false;         	/* 禁用并行感知 */
    pathnode->parallel_safe = rel->consider_parallel;  /* 并行安全性由关系决定 */
    pathnode->parallel_workers = 0;           	/* 设置并行工作进程数为0 */
    pathnode->pathkeys = NIL;                 	/* 结果总是无序的 */

    /* 成本计算与常规CTE扫描相同 */
    cost_ctescan(pathnode, root, rel, pathnode->param_info);

    return pathnode;  /* 返回构建的路径节点 */
}


/*
 * create_foreignscan_path
 *	  Creates a path corresponding to a scan of a foreign base table,
 *	  returning the pathnode.
 *
 * This function is never called from core Postgres; rather, it's expected
 * to be called by the GetForeignPaths function of a foreign data wrapper.
 * We make the FDW supply all fields of the path, since we do not have any way
 * to calculate them in core.  However, there is a usually-sane default for
 * the pathtarget (rel->reltarget), so we let a NULL for "target" select that.
 */
ForeignPath *
create_foreignscan_path(PlannerInfo *root, RelOptInfo *rel,
						PathTarget *target,
						double rows, Cost startup_cost, Cost total_cost,
						List *pathkeys,
						Relids required_outer,
						Path *fdw_outerpath,
						List *fdw_private)
{
	ForeignPath *pathnode = makeNode(ForeignPath);

	/* Historically some FDWs were confused about when to use this */
	Assert(IS_SIMPLE_REL(rel));

	pathnode->path.pathtype = T_ForeignScan;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target ? target : rel->reltarget;
	pathnode->path.param_info = get_baserel_parampathinfo(root, rel,
														  required_outer);
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_workers = 0;
	pathnode->path.rows = rows;
	pathnode->path.startup_cost = startup_cost;
	pathnode->path.total_cost = total_cost;
	pathnode->path.pathkeys = pathkeys;

	pathnode->fdw_outerpath = fdw_outerpath;
	pathnode->fdw_private = fdw_private;

	return pathnode;
}

/*
 * create_foreign_join_path
 *	  Creates a path corresponding to a scan of a foreign join,
 *	  returning the pathnode.
 *
 * This function is never called from core Postgres; rather, it's expected
 * to be called by the GetForeignJoinPaths function of a foreign data wrapper.
 * We make the FDW supply all fields of the path, since we do not have any way
 * to calculate them in core.  However, there is a usually-sane default for
 * the pathtarget (rel->reltarget), so we let a NULL for "target" select that.
 */
ForeignPath *
create_foreign_join_path(PlannerInfo *root, RelOptInfo *rel,
						 PathTarget *target,
						 double rows, Cost startup_cost, Cost total_cost,
						 List *pathkeys,
						 Relids required_outer,
						 Path *fdw_outerpath,
						 List *fdw_private)
{
	ForeignPath *pathnode = makeNode(ForeignPath);

	/*
	 * We should use get_joinrel_parampathinfo to handle parameterized paths,
	 * but the API of this function doesn't support it, and existing
	 * extensions aren't yet trying to build such paths anyway.  For the
	 * moment just throw an error if someone tries it; eventually we should
	 * revisit this.
	 */
	if (!bms_is_empty(required_outer) || !bms_is_empty(rel->lateral_relids))
		elog(ERROR, "parameterized foreign joins are not supported yet");

	pathnode->path.pathtype = T_ForeignScan;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target ? target : rel->reltarget;
	pathnode->path.param_info = NULL;	/* XXX see above */
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_workers = 0;
	pathnode->path.rows = rows;
	pathnode->path.startup_cost = startup_cost;
	pathnode->path.total_cost = total_cost;
	pathnode->path.pathkeys = pathkeys;

	pathnode->fdw_outerpath = fdw_outerpath;
	pathnode->fdw_private = fdw_private;

	return pathnode;
}

/*
 * create_foreign_upper_path
 *	  Creates a path corresponding to an upper relation that's computed
 *	  directly by an FDW, returning the pathnode.
 *
 * This function is never called from core Postgres; rather, it's expected to
 * be called by the GetForeignUpperPaths function of a foreign data wrapper.
 * We make the FDW supply all fields of the path, since we do not have any way
 * to calculate them in core.  However, there is a usually-sane default for
 * the pathtarget (rel->reltarget), so we let a NULL for "target" select that.
 */
ForeignPath *
create_foreign_upper_path(PlannerInfo *root, RelOptInfo *rel,
						  PathTarget *target,
						  double rows, Cost startup_cost, Cost total_cost,
						  List *pathkeys,
						  Path *fdw_outerpath,
						  List *fdw_private)
{
	ForeignPath *pathnode = makeNode(ForeignPath);

	/*
	 * Upper relations should never have any lateral references, since joining
	 * is complete.
	 */
	Assert(bms_is_empty(rel->lateral_relids));

	pathnode->path.pathtype = T_ForeignScan;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target ? target : rel->reltarget;
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel;
	pathnode->path.parallel_workers = 0;
	pathnode->path.rows = rows;
	pathnode->path.startup_cost = startup_cost;
	pathnode->path.total_cost = total_cost;
	pathnode->path.pathkeys = pathkeys;

	pathnode->fdw_outerpath = fdw_outerpath;
	pathnode->fdw_private = fdw_private;

	return pathnode;
}

/*
 * calc_nestloop_required_outer
 *	  Compute the required_outer set for a nestloop join path
 *
 * Note: result must not share storage with either input
 */
Relids
calc_nestloop_required_outer(Relids outerrelids,
							 Relids outer_paramrels,
							 Relids innerrelids,
							 Relids inner_paramrels)
{
	Relids		required_outer;

	/* inner_path can require rels from outer path, but not vice versa */
	Assert(!bms_overlap(outer_paramrels, innerrelids));
	/* easy case if inner path is not parameterized */
	if (!inner_paramrels)
		return bms_copy(outer_paramrels);
	/* else, form the union ... */
	required_outer = bms_union(outer_paramrels, inner_paramrels);
	/* ... and remove any mention of now-satisfied outer rels */
	required_outer = bms_del_members(required_outer,
									 outerrelids);
	/* maintain invariant that required_outer is exactly NULL if empty */
	if (bms_is_empty(required_outer))
	{
		bms_free(required_outer);
		required_outer = NULL;
	}
	return required_outer;
}

/*
 * calc_non_nestloop_required_outer
 *	  Compute the required_outer set for a merge or hash join path
 *
 * Note: result must not share storage with either input
 */
Relids
calc_non_nestloop_required_outer(Path *outer_path, Path *inner_path)
{
	Relids		outer_paramrels = PATH_REQ_OUTER(outer_path);
	Relids		inner_paramrels = PATH_REQ_OUTER(inner_path);
	Relids		required_outer;

	/* neither path can require rels from the other */
	Assert(!bms_overlap(outer_paramrels, inner_path->parent->relids));
	Assert(!bms_overlap(inner_paramrels, outer_path->parent->relids));
	/* form the union ... */
	required_outer = bms_union(outer_paramrels, inner_paramrels);
	/* we do not need an explicit test for empty; bms_union gets it right */
	return required_outer;
}

/*
 * create_nestloop_path
 *    创建一个对应于两个关系之间嵌套循环连接的路径节点
 *
 * 函数功能:
 * 该函数负责构建一个NestPath类型的路径节点，代表执行两个关系之间嵌套循环连接的方式。
 * 它初始化路径节点的各种属性，处理参数化情况，并调用最终成本计算函数。
 *
 * 参数说明:
 * - root: 查询规划器的全局信息结构
 * - joinrel: 要连接的关系的优化器信息
 * - jointype: 所需的连接类型(如内连接、外连接等)
 * - workspace: 来自initial_cost_nestloop的成本计算结果
 * - extra: 包含连接相关的各种信息的结构体
 * - outer_path: 外循环路径
 * - inner_path: 内循环路径
 * - restrict_clauses: 应用于连接的限制条件信息列表
 * - pathkeys: 新连接路径的路径键
 * - required_outer: 所需的外部关系集合
 *
 * 返回值:
 * NestPath* - 创建的嵌套循环连接路径节点
 */
NestPath *
create_nestloop_path(PlannerInfo *root,
                     RelOptInfo *joinrel,
                     JoinType jointype,
                     JoinCostWorkspace *workspace,
                     JoinPathExtraData *extra,
                     Path *outer_path,
                     Path *inner_path,
                     List *restrict_clauses,
                     List *pathkeys,
                     Relids required_outer)
{
    /* 创建并初始化嵌套循环路径节点 */
    NestPath   *pathnode = makeNode(NestPath);
    /* 获取内路径所需的外部关系 */
    Relids      inner_req_outer = PATH_REQ_OUTER(inner_path);

    /*
     * 如果内路径被外部参数化，我们必须删除任何将被移动到内路径的限制条件。
     * 我们必须现在就这样做，而不是推迟到创建计划时，因为限制条件列表会影响
     * 此路径的大小和成本估计。
     */
    if (bms_overlap(inner_req_outer, outer_path->parent->relids))
    {
        /* 计算内路径关系和它所需的外部关系的并集 */
        Relids      inner_and_outer = bms_union(inner_path->parent->relids,
                                               inner_req_outer);
        List       *jclauses = NIL;  /* 用于存储不能移动到内路径的连接条件 */
        ListCell   *lc;

        /* 遍历所有限制条件 */
        foreach(lc, restrict_clauses)
        {
            RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

            /*
             * 如果限制条件不能移动到内路径，则保留在连接级别的条件列表中
             */
            if (!join_clause_is_movable_into(rinfo,
                                             inner_path->parent->relids,
                                             inner_and_outer))
                jclauses = lappend(jclauses, rinfo);
        }
        /* 更新限制条件列表，只保留不能移动到内路径的条件 */
        restrict_clauses = jclauses;
    }

    /* 设置路径节点的基本属性 */
    pathnode->path.pathtype = T_NestLoop;     /* 路径类型为嵌套循环 */
    pathnode->path.parent = joinrel;          /* 设置父关系 */
    pathnode->path.pathtarget = joinrel->reltarget; /* 设置目标列表 */
    
    /* 获取参数化路径信息，并更新限制条件列表 */
    pathnode->path.param_info =
        get_joinrel_parampathinfo(root,
                                 joinrel,
                                 outer_path,
                                 inner_path,
                                 extra->sjinfo,
                                 required_outer,
                                 &restrict_clauses);
    
    /* 设置并行执行相关属性 */
    pathnode->path.parallel_aware = false;    /* 嵌套循环不支持并行感知 */
    /* 只有当连接关系考虑并行且内外路径都支持并行时，嵌套循环才支持并行 */
    pathnode->path.parallel_safe = joinrel->consider_parallel &&
        outer_path->parallel_safe && inner_path->parallel_safe;
    /* 简单地使用外路径的并行工作线程数(注释表明这是临时解决方案) */
    pathnode->path.parallel_workers = outer_path->parallel_workers;
    
    /* 设置路径键和连接属性 */
    pathnode->path.pathkeys = pathkeys;       /* 设置路径键 */
    pathnode->jointype = jointype;            /* 设置连接类型 */
    pathnode->inner_unique = extra->inner_unique; /* 是否内关系唯一 */
    pathnode->outerjoinpath = outer_path;     /* 设置外循环路径 */
    pathnode->innerjoinpath = inner_path;     /* 设置内循环路径 */
    pathnode->joinrestrictinfo = restrict_clauses; /* 设置连接限制条件 */

    /* 计算嵌套循环连接的最终成本 */
    final_cost_nestloop(root, pathnode, workspace, extra);

    /* 返回创建的嵌套循环路径节点 */
    return pathnode;
}


/*
 * create_mergejoin_path
 *	  Creates a pathnode corresponding to a mergejoin join between
 *	  two relations
 *
 * 'joinrel' is the join relation
 * 'jointype' is the type of join required
 * 'workspace' is the result from initial_cost_mergejoin
 * 'extra' contains various information about the join
 * 'outer_path' is the outer path
 * 'inner_path' is the inner path
 * 'restrict_clauses' are the RestrictInfo nodes to apply at the join
 * 'pathkeys' are the path keys of the new join path
 * 'required_outer' is the set of required outer rels
 * 'mergeclauses' are the RestrictInfo nodes to use as merge clauses
 *		(this should be a subset of the restrict_clauses list)
 * 'outersortkeys' are the sort varkeys for the outer relation
 * 'innersortkeys' are the sort varkeys for the inner relation
 */
MergePath *
create_mergejoin_path(PlannerInfo *root,
					  RelOptInfo *joinrel,
					  JoinType jointype,
					  JoinCostWorkspace *workspace,
					  JoinPathExtraData *extra,
					  Path *outer_path,
					  Path *inner_path,
					  List *restrict_clauses,
					  List *pathkeys,
					  Relids required_outer,
					  List *mergeclauses,
					  List *outersortkeys,
					  List *innersortkeys)
{
	MergePath  *pathnode = makeNode(MergePath);

	pathnode->jpath.path.pathtype = T_MergeJoin;
	pathnode->jpath.path.parent = joinrel;
	pathnode->jpath.path.pathtarget = joinrel->reltarget;
	pathnode->jpath.path.param_info =
		get_joinrel_parampathinfo(root,
								  joinrel,
								  outer_path,
								  inner_path,
								  extra->sjinfo,
								  required_outer,
								  &restrict_clauses);
	pathnode->jpath.path.parallel_aware = false;
	pathnode->jpath.path.parallel_safe = joinrel->consider_parallel &&
		outer_path->parallel_safe && inner_path->parallel_safe;
	/* This is a foolish way to estimate parallel_workers, but for now... */
	pathnode->jpath.path.parallel_workers = outer_path->parallel_workers;
	pathnode->jpath.path.pathkeys = pathkeys;
	pathnode->jpath.jointype = jointype;
	pathnode->jpath.inner_unique = extra->inner_unique;
	pathnode->jpath.outerjoinpath = outer_path;
	pathnode->jpath.innerjoinpath = inner_path;
	pathnode->jpath.joinrestrictinfo = restrict_clauses;
	pathnode->path_mergeclauses = mergeclauses;
	pathnode->outersortkeys = outersortkeys;
	pathnode->innersortkeys = innersortkeys;
	/* pathnode->skip_mark_restore will be set by final_cost_mergejoin */
	/* pathnode->materialize_inner will be set by final_cost_mergejoin */

	final_cost_mergejoin(root, pathnode, workspace, extra);

	return pathnode;
}

/*
 * create_hashjoin_path
 *	  Creates a pathnode corresponding to a hash join between two relations.
 *
 * 'joinrel' is the join relation
 * 'jointype' is the type of join required
 * 'workspace' is the result from initial_cost_hashjoin
 * 'extra' contains various information about the join
 * 'outer_path' is the cheapest outer path
 * 'inner_path' is the cheapest inner path
 * 'parallel_hash' to select Parallel Hash of inner path (shared hash table)
 * 'restrict_clauses' are the RestrictInfo nodes to apply at the join
 * 'required_outer' is the set of required outer rels
 * 'hashclauses' are the RestrictInfo nodes to use as hash clauses
 *		(this should be a subset of the restrict_clauses list)
 */
HashPath *
create_hashjoin_path(PlannerInfo *root,
					 RelOptInfo *joinrel,
					 JoinType jointype,
					 JoinCostWorkspace *workspace,
					 JoinPathExtraData *extra,
					 Path *outer_path,
					 Path *inner_path,
					 bool parallel_hash,
					 List *restrict_clauses,
					 Relids required_outer,
					 List *hashclauses)
{
	HashPath   *pathnode = makeNode(HashPath);

	pathnode->jpath.path.pathtype = T_HashJoin;
	pathnode->jpath.path.parent = joinrel;
	pathnode->jpath.path.pathtarget = joinrel->reltarget;
	pathnode->jpath.path.param_info =
		get_joinrel_parampathinfo(root,
								  joinrel,
								  outer_path,
								  inner_path,
								  extra->sjinfo,
								  required_outer,
								  &restrict_clauses);
	pathnode->jpath.path.parallel_aware =
		joinrel->consider_parallel && parallel_hash;
	pathnode->jpath.path.parallel_safe = joinrel->consider_parallel &&
		outer_path->parallel_safe && inner_path->parallel_safe;
	/* This is a foolish way to estimate parallel_workers, but for now... */
	pathnode->jpath.path.parallel_workers = outer_path->parallel_workers;

	/*
	 * A hashjoin never has pathkeys, since its output ordering is
	 * unpredictable due to possible batching.  XXX If the inner relation is
	 * small enough, we could instruct the executor that it must not batch,
	 * and then we could assume that the output inherits the outer relation's
	 * ordering, which might save a sort step.  However there is considerable
	 * downside if our estimate of the inner relation size is badly off. For
	 * the moment we don't risk it.  (Note also that if we wanted to take this
	 * seriously, joinpath.c would have to consider many more paths for the
	 * outer rel than it does now.)
	 */
	pathnode->jpath.path.pathkeys = NIL;
	pathnode->jpath.jointype = jointype;
	pathnode->jpath.inner_unique = extra->inner_unique;
	pathnode->jpath.outerjoinpath = outer_path;
	pathnode->jpath.innerjoinpath = inner_path;
	pathnode->jpath.joinrestrictinfo = restrict_clauses;
	pathnode->path_hashclauses = hashclauses;
	/* final_cost_hashjoin will fill in pathnode->num_batches */

	final_cost_hashjoin(root, pathnode, workspace, extra);

	return pathnode;
}

/*
 * create_projection_path
 *	  Creates a pathnode that represents performing a projection.
 *
 * 'rel' is the parent relation associated with the result
 * 'subpath' is the path representing the source of data
 * 'target' is the PathTarget to be computed
 */
ProjectionPath *
create_projection_path(PlannerInfo *root,
					   RelOptInfo *rel,
					   Path *subpath,
					   PathTarget *target)
{
	ProjectionPath *pathnode = makeNode(ProjectionPath);
	PathTarget *oldtarget;

	/*
	 * We mustn't put a ProjectionPath directly above another; it's useless
	 * and will confuse create_projection_plan.  Rather than making sure all
	 * callers handle that, let's implement it here, by stripping off any
	 * ProjectionPath in what we're given.  Given this rule, there won't be
	 * more than one.
	 */
	if (IsA(subpath, ProjectionPath))
	{
		ProjectionPath *subpp = (ProjectionPath *) subpath;

		Assert(subpp->path.parent == rel);
		subpath = subpp->subpath;
		Assert(!IsA(subpath, ProjectionPath));
	}

	pathnode->path.pathtype = T_Result;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target;
	/* For now, assume we are above any joins, so no parameterization */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe &&
		is_parallel_safe(root, (Node *) target->exprs);
	pathnode->path.parallel_workers = subpath->parallel_workers;
	/* Projection does not change the sort order */
	pathnode->path.pathkeys = subpath->pathkeys;

	pathnode->subpath = subpath;

	/*
	 * We might not need a separate Result node.  If the input plan node type
	 * can project, we can just tell it to project something else.  Or, if it
	 * can't project but the desired target has the same expression list as
	 * what the input will produce anyway, we can still give it the desired
	 * tlist (possibly changing its ressortgroupref labels, but nothing else).
	 * Note: in the latter case, create_projection_plan has to recheck our
	 * conclusion; see comments therein.
	 */
	oldtarget = subpath->pathtarget;
	if (is_projection_capable_path(subpath) ||
		equal(oldtarget->exprs, target->exprs))
	{
		/* No separate Result node needed */
		pathnode->dummypp = true;

		/*
		 * Set cost of plan as subpath's cost, adjusted for tlist replacement.
		 */
		pathnode->path.rows = subpath->rows;
		pathnode->path.startup_cost = subpath->startup_cost +
			(target->cost.startup - oldtarget->cost.startup);
		pathnode->path.total_cost = subpath->total_cost +
			(target->cost.startup - oldtarget->cost.startup) +
			(target->cost.per_tuple - oldtarget->cost.per_tuple) * subpath->rows;
	}
	else
	{
		/* We really do need the Result node */
		pathnode->dummypp = false;

		/*
		 * The Result node's cost is cpu_tuple_cost per row, plus the cost of
		 * evaluating the tlist.  There is no qual to worry about.
		 */
		pathnode->path.rows = subpath->rows;
		pathnode->path.startup_cost = subpath->startup_cost +
			target->cost.startup;
		pathnode->path.total_cost = subpath->total_cost +
			target->cost.startup +
			(cpu_tuple_cost + target->cost.per_tuple) * subpath->rows;
	}

	return pathnode;
}

/*
 * apply_projection_to_path
 *    向给定路径添加投影步骤，或直接将目标投影应用到路径上
 *
 * 此函数的效果与create_projection_path()相似，但区别在于：如果不需要单独的Result节点，
 * 我们会直接替换给定路径的pathtarget为所需的目标。此函数仅应在调用者确定该路径不会
 * 在其他地方被引用时使用，这样可以进行原地修改。
 *
 * 如果输入路径是GatherPath或GatherMergePath，我们还会尝试将新目标下推到其输入路径；
 * 这种更具侵入性的修改是create_projection_path()无法完成的。
 *
 * 注意：我们不能更改源路径的父链接；因此当它通过add_path()添加到"rel"时，状态会有些不一致。
 * 到目前为止这还没有引起任何问题。
 *
 * 参数：
 * 'root' - 规划器信息结构体指针
 * 'rel' - 与结果关联的父关系
 * 'path' - 表示数据源的路径
 * 'target' - 要计算的PathTarget
 *
 * 返回值：
 * 修改后的Path指针
 */
Path *
apply_projection_to_path(PlannerInfo *root,
                         RelOptInfo *rel,
                         Path *path,
                         PathTarget *target)
{
    QualCost    oldcost;  // 保存原始路径的成本信息，用于后续计算新成本

    /*
     * 如果给定路径不支持投影功能，我们可能需要创建一个Result节点，
     * 因此调用create_projection_path创建单独的ProjectionPath
     *
     * [场景 2：必须包装]
     * 例子：SELECT a + b FROM (SELECT * FROM t ORDER BY a) s;
     * 如果子查询使用 Sort 路径，通常无法执行计算（a+b）。
     * 我们必须用 Result 节点（ProjectionPath）包装它来处理计算。
     */
    if (!is_projection_capable_path(path))
        return (Path *) create_projection_path(root, rel, path, target);

    /*
     * 如果路径支持投影功能，我们可以直接将所需的目标列表替换到现有路径中，
     * 同时确保适当地更新其成本估算
     *
     * [场景 1：原地修改]
     * 例子：SELECT a + 100 FROM t_large_table;
     * 如果使用 Seq Scan，它可以在扫描时计算 a+100。
     * 我们直接更新目标列表并调整成本（计算的 CPU 成本）。
     */
    oldcost = path->pathtarget->cost;  // 保存原成本信息
    path->pathtarget = target;         // 替换目标投影

    // 更新启动成本：新的启动成本 = 原启动成本 + (新目标启动成本 - 旧目标启动成本)
    path->startup_cost += target->cost.startup - oldcost.startup;
    // 更新总成本：原总成本 + 启动成本变化 + 每行处理成本变化 × 行数
    path->total_cost += target->cost.startup - oldcost.startup +
        (target->cost.per_tuple - oldcost.per_tuple) * path->rows;

    /*
     * 如果路径是Gather或GatherMerge类型，我们希望安排其子路径返回所需的目标列表，
     * 以便工作进程可以协助执行投影操作。但前提是目标表达式中的所有内容都必须是并行安全的。
     *
     * [场景 3：并行查询下推]
     * 核心思想：将计算任务（投影）下推到 Worker 进程，减轻 Leader 负担。
     *
     * 例子：SELECT md5(log_content) FROM t_logs; (CPU 密集型)
     *
     * 1. 优化前（无下推）：
     *    Worker 只负责扫描并发送原始 log_content 给 Leader。
     *    Leader 独自计算 1亿次 md5()，成为单点瓶颈。
	 *    IPC 开销大（传输长字符串）。
	 *
	 * Result (计算 md5)  <-- 只有 Leader 在干活，累死
	 *  -> Gather
	 *     -> Parallel Seq Scan (只扫描，不计算)
	 *
     * 2. 优化后（有下推）：
     *    代码检测到 GatherPath 且 md5() 是并行安全的。
     *    将 Result 节点（计算 md5）下推到 subpath (Worker 执行路径)。
     *    Worker 扫描后立即计算 md5，发送短字符串给 Leader。
	 *    Leader 只负责收集，CPU 负载均衡，性能大幅提升。
	 *
	 * Gather (只负责收集)
	 *  -> Result (计算 md5)  <-- Worker 在干活，人多力量大
	 *     -> Parallel Seq Scan
	 */
    if ((IsA(path, GatherPath) || IsA(path, GatherMergePath)) &&
        is_parallel_safe(root, (Node *) target->exprs))
    {
        /*
         * 这里我们总是使用create_projection_path，即使子路径支持投影，
         * 这样做是为了避免原地修改子路径。虽然目前看起来不太可能有其他地方引用子路径，
         * 但保持安全比冒险更好。
         *
         * 注意：我们不更改并行路径的成本估算；理论上可能应该更新成本以反映大部分目标计算
         * 会在工作进程中完成这一事实，但目前未实现。
         */
        if (IsA(path, GatherPath))
        {
            GatherPath *gpath = (GatherPath *) path;

            // 为GatherPath的子路径创建投影路径
            gpath->subpath = (Path *)
                create_projection_path(root,
                                     gpath->subpath->parent,
                                     gpath->subpath,
                                     target);
        }
        else
        {
            GatherMergePath *gmpath = (GatherMergePath *) path;

            // 为GatherMergePath的子路径创建投影路径
            gmpath->subpath = (Path *)
                create_projection_path(root,
                                     gmpath->subpath->parent,
                                     gmpath->subpath,
                                     target);
        }
    }
    else if (path->parallel_safe &&
             !is_parallel_safe(root, (Node *) target->exprs))
    {
        /*
         * 如果我们向当前标记为并行安全的路径中插入了并行受限的目标列表，
         * 那么我们必须将路径标记为不再并行安全
         */
        path->parallel_safe = false;
    }

    return path;  // 返回修改后的路径
}


/*
 * create_set_projection_path
 *      创建一个表示执行包含集合返回函数(SRFs)的投影操作的路径节点。
 *
 * 'rel' 是与结果相关联的父关系
 * 'subpath' 是表示数据源的路径
 * 'target' 是要计算的PathTarget
 */
ProjectSetPath *
create_set_projection_path(PlannerInfo *root,
                          RelOptInfo *rel,
                          Path *subpath,
                          PathTarget *target)
{
    ProjectSetPath *pathnode = makeNode(ProjectSetPath);
    double      tlist_rows;
    ListCell   *lc;

    /* 设置基本路径属性 */
    pathnode->path.pathtype = T_ProjectSet;  /* 路径类型为ProjectSet */
    pathnode->path.parent = rel;             /* 设置父关系 */
    pathnode->path.pathtarget = target;      /* 设置目标投影 */
    /* 目前假设我们在所有连接之上，因此没有参数化 */
    pathnode->path.param_info = NULL;
    pathnode->path.parallel_aware = false;   /* 不感知并行 */
    /* 并行安全性取决于父关系、子路径和目标表达式 */
    pathnode->path.parallel_safe = rel->consider_parallel &&
        subpath->parallel_safe &&
        is_parallel_safe(root, (Node *) target->exprs);
    /* 继承子路径的并行工作线程数 */
    pathnode->path.parallel_workers = subpath->parallel_workers;
    /* 投影不会改变排序顺序 (XXX：这可能需要重新考虑？) */
    pathnode->path.pathkeys = subpath->pathkeys;

    /* 保存子路径引用 */
    pathnode->subpath = subpath;

    /*
     * 估计每个输入行通过SRFs产生的行数；如果在此节点中有多个SRF，使用最大值。
     */
    tlist_rows = 1;  /* 默认为每行输入产生一行输出 */
    foreach(lc, target->exprs)
    {
        Node       *node = (Node *) lfirst(lc);
        double      itemrows;

        /* 计算此表达式返回的行数 */
        itemrows = expression_returns_set_rows(root, node);
        /* 取最大行数值，因为SRF是并行展开的 */
        if (tlist_rows < itemrows)
            tlist_rows = itemrows;
    }

    /*
     * 除了评估目标列表的成本外，对每个输入行收取cpu_tuple_cost，
     * 对每个额外产生的输出行收取一半的cpu_tuple_cost。
     * 这可能有点奇怪，但这是9.6版本的实现方式；我们以后可能会重新审视这个估计。
     */
    /* 计算总行数：输入行数乘以每行产生的平均行数 */
    pathnode->path.rows = subpath->rows * tlist_rows;
    /* 启动成本：子路径启动成本加上目标表达式启动成本 */
    pathnode->path.startup_cost = subpath->startup_cost +
        target->cost.startup;
    /* 总成本计算：
     * 1. 子路径总成本
     * 2. 加上目标表达式启动成本
     * 3. 加上每行输入的处理成本（CPU元组成本+每行目标表达式成本）
     * 4. 加上额外产生的输出行的处理成本（每行收取一半的CPU元组成本）
     */
    pathnode->path.total_cost = subpath->total_cost +
        target->cost.startup +
        (cpu_tuple_cost + target->cost.per_tuple) * subpath->rows +
        (pathnode->path.rows - subpath->rows) * cpu_tuple_cost / 2;

    /* 返回创建的ProjectSet路径节点 */
    return pathnode;
}


/*
 * create_sort_path
 *	  Creates a pathnode that represents performing an explicit sort.
 *
 * 'rel' is the parent relation associated with the result
 * 'subpath' is the path representing the source of data
 * 'pathkeys' represents the desired sort order
 * 'limit_tuples' is the estimated bound on the number of output tuples,
 *		or -1 if no LIMIT or couldn't estimate
 */
SortPath *
create_sort_path(PlannerInfo *root,
				 RelOptInfo *rel,
				 Path *subpath,
				 List *pathkeys,
				 double limit_tuples)
{
	SortPath   *pathnode = makeNode(SortPath);

	pathnode->path.pathtype = T_Sort;
	pathnode->path.parent = rel;
	/* Sort doesn't project, so use source path's pathtarget */
	pathnode->path.pathtarget = subpath->pathtarget;
	/* For now, assume we are above any joins, so no parameterization */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_workers = subpath->parallel_workers;
	pathnode->path.pathkeys = pathkeys;

	pathnode->subpath = subpath;

	cost_sort(&pathnode->path, root, pathkeys,
			  subpath->total_cost,
			  subpath->rows,
			  subpath->pathtarget->width,
			  0.0,				/* XXX comparison_cost shouldn't be 0? */
			  work_mem, limit_tuples);

	return pathnode;
}

/*
 * create_group_path
 *	  Creates a pathnode that represents performing grouping of presorted input
 *
 * 'rel' is the parent relation associated with the result
 * 'subpath' is the path representing the source of data
 * 'target' is the PathTarget to be computed
 * 'groupClause' is a list of SortGroupClause's representing the grouping
 * 'qual' is the HAVING quals if any
 * 'numGroups' is the estimated number of groups
 */
GroupPath *
create_group_path(PlannerInfo *root,
				  RelOptInfo *rel,
				  Path *subpath,
				  List *groupClause,
				  List *qual,
				  double numGroups)
{
	GroupPath  *pathnode = makeNode(GroupPath);
	PathTarget *target = rel->reltarget;

	pathnode->path.pathtype = T_Group;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target;
	/* For now, assume we are above any joins, so no parameterization */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_workers = subpath->parallel_workers;
	/* Group doesn't change sort ordering */
	pathnode->path.pathkeys = subpath->pathkeys;

	pathnode->subpath = subpath;

	pathnode->groupClause = groupClause;
	pathnode->qual = qual;

	cost_group(&pathnode->path, root,
			   list_length(groupClause),
			   numGroups,
			   qual,
			   subpath->startup_cost, subpath->total_cost,
			   subpath->rows);

	/* add tlist eval cost for each output row */
	pathnode->path.startup_cost += target->cost.startup;
	pathnode->path.total_cost += target->cost.startup +
		target->cost.per_tuple * pathnode->path.rows;

	return pathnode;
}

/*
 * create_upper_unique_path
 *    创建一个路径节点，表示在已排序输入上执行显式Unique步骤。
 *
 * 该函数生成一个Unique计划节点，但其用例与create_unique_path非常不同，
 * 因此似乎不值得尝试合并这两个函数。
 *
 * 参数说明：
 *   root - 规划器信息结构体指针
 *   rel - 与结果关联的父关系
 *   subpath - 表示数据源的路径
 *   numCols - 分组列的数量
 *   numGroups - 估计的分组数量
 *
 * 输入路径必须按照分组列排序，可能还会有额外的列；因此前numCols个pathkeys是分组列
 */
UpperUniquePath *
create_upper_unique_path(PlannerInfo *root,
                         RelOptInfo *rel,
                         Path *subpath,
                         int numCols,
                         double numGroups)
{
    /* 创建一个新的UpperUniquePath节点 */
    UpperUniquePath *pathnode = makeNode(UpperUniquePath);

    /* 设置基本路径属性 */
    pathnode->path.pathtype = T_Unique;        /* 路径类型为Unique */
    pathnode->path.parent = rel;               /* 设置父关系 */
    /* Unique不进行投影操作，因此使用源路径的pathtarget */
    pathnode->path.pathtarget = subpath->pathtarget;
    /* 目前假设我们位于所有连接之上，因此不需要参数化 */
    pathnode->path.param_info = NULL;
    pathnode->path.parallel_aware = false;     /* 不感知并行执行 */
    /* 并行安全性取决于父关系的考虑和子路径的并行安全性 */
    pathnode->path.parallel_safe = rel->consider_parallel &&
        subpath->parallel_safe;
    pathnode->path.parallel_workers = subpath->parallel_workers; /* 继承并行工作线程数 */
    /* Unique不改变输入的排序顺序 */
    pathnode->path.pathkeys = subpath->pathkeys;

    /* 设置UpperUniquePath特有的属性 */
    pathnode->subpath = subpath;               /* 设置子路径（数据来源） */
    pathnode->numkeys = numCols;               /* 设置用于唯一性检查的键数量 */

    /*
     * 成本计算：每个比较操作按每个输入元组计费一个cpu_operator_cost。
     * 我们假设所有列在大多数元组上都会被比较。
     * （XXX这可能是一个高估。）
     */
    /* 启动成本与子路径相同（Unique在看到第一行后就可以开始输出） */
    pathnode->path.startup_cost = subpath->startup_cost;
    /* 总成本 = 子路径总成本 + 比较操作的CPU成本 */
    pathnode->path.total_cost = subpath->total_cost +
        cpu_operator_cost * subpath->rows * numCols;
    /* 行数估计为分组数量（去重后的行数） */
    pathnode->path.rows = numGroups;

    /* 返回创建的路径节点 */
    return pathnode;
}


/*
 * create_agg_path
 *	  Creates a pathnode that represents performing aggregation/grouping
 *
 * 'rel' is the parent relation associated with the result
 * 'subpath' is the path representing the source of data
 * 'target' is the PathTarget to be computed
 * 'aggstrategy' is the Agg node's basic implementation strategy
 * 'aggsplit' is the Agg node's aggregate-splitting mode
 * 'groupClause' is a list of SortGroupClause's representing the grouping
 * 'qual' is the HAVING quals if any
 * 'aggcosts' contains cost info about the aggregate functions to be computed
 * 'numGroups' is the estimated number of groups (1 if not grouping)
 */
AggPath *
create_agg_path(PlannerInfo *root,
				RelOptInfo *rel,
				Path *subpath,
				PathTarget *target,
				AggStrategy aggstrategy,
				AggSplit aggsplit,
				List *groupClause,
				List *qual,
				const AggClauseCosts *aggcosts,
				double numGroups)
{
	AggPath    *pathnode = makeNode(AggPath);

	pathnode->path.pathtype = T_Agg;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target;
	/* For now, assume we are above any joins, so no parameterization */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_workers = subpath->parallel_workers;
	if (aggstrategy == AGG_SORTED)
		pathnode->path.pathkeys = subpath->pathkeys;	/* preserves order */
	else
		pathnode->path.pathkeys = NIL;	/* output is unordered */
	pathnode->subpath = subpath;

	pathnode->aggstrategy = aggstrategy;
	pathnode->aggsplit = aggsplit;
	pathnode->numGroups = numGroups;
	pathnode->groupClause = groupClause;
	pathnode->qual = qual;

	cost_agg(&pathnode->path, root,
			 aggstrategy, aggcosts,
			 list_length(groupClause), numGroups,
			 qual,
			 subpath->startup_cost, subpath->total_cost,
			 subpath->rows);

	/* add tlist eval cost for each output row */
	pathnode->path.startup_cost += target->cost.startup;
	pathnode->path.total_cost += target->cost.startup +
		target->cost.per_tuple * pathnode->path.rows;

	return pathnode;
}

/*
 * create_groupingsets_path
 *	  Creates a pathnode that represents performing GROUPING SETS aggregation
 *
 * GroupingSetsPath represents sorted grouping with one or more grouping sets.
 * The input path's result must be sorted to match the last entry in
 * rollup_groupclauses.
 *
 * 'rel' is the parent relation associated with the result
 * 'subpath' is the path representing the source of data
 * 'target' is the PathTarget to be computed
 * 'having_qual' is the HAVING quals if any
 * 'rollups' is a list of RollupData nodes
 * 'agg_costs' contains cost info about the aggregate functions to be computed
 * 'numGroups' is the estimated total number of groups
 */
GroupingSetsPath *
create_groupingsets_path(PlannerInfo *root,
						 RelOptInfo *rel,
						 Path *subpath,
						 List *having_qual,
						 AggStrategy aggstrategy,
						 List *rollups,
						 const AggClauseCosts *agg_costs,
						 double numGroups)
{
	GroupingSetsPath *pathnode = makeNode(GroupingSetsPath);
	PathTarget *target = rel->reltarget;
	ListCell   *lc;
	bool		is_first = true;
	bool		is_first_sort = true;

	/* The topmost generated Plan node will be an Agg */
	pathnode->path.pathtype = T_Agg;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target;
	pathnode->path.param_info = subpath->param_info;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_workers = subpath->parallel_workers;
	pathnode->subpath = subpath;

	/*
	 * Simplify callers by downgrading AGG_SORTED to AGG_PLAIN, and AGG_MIXED
	 * to AGG_HASHED, here if possible.
	 */
	if (aggstrategy == AGG_SORTED &&
		list_length(rollups) == 1 &&
		((RollupData *) linitial(rollups))->groupClause == NIL)
		aggstrategy = AGG_PLAIN;

	if (aggstrategy == AGG_MIXED &&
		list_length(rollups) == 1)
		aggstrategy = AGG_HASHED;

	/*
	 * Output will be in sorted order by group_pathkeys if, and only if, there
	 * is a single rollup operation on a non-empty list of grouping
	 * expressions.
	 */
	if (aggstrategy == AGG_SORTED && list_length(rollups) == 1)
		pathnode->path.pathkeys = root->group_pathkeys;
	else
		pathnode->path.pathkeys = NIL;

	pathnode->aggstrategy = aggstrategy;
	pathnode->rollups = rollups;
	pathnode->qual = having_qual;

	Assert(rollups != NIL);
	Assert(aggstrategy != AGG_PLAIN || list_length(rollups) == 1);
	Assert(aggstrategy != AGG_MIXED || list_length(rollups) > 1);

	foreach(lc, rollups)
	{
		RollupData *rollup = lfirst(lc);
		List	   *gsets = rollup->gsets;
		int			numGroupCols = list_length(linitial(gsets));

		/*
		 * In AGG_SORTED or AGG_PLAIN mode, the first rollup takes the
		 * (already-sorted) input, and following ones do their own sort.
		 *
		 * In AGG_HASHED mode, there is one rollup for each grouping set.
		 *
		 * In AGG_MIXED mode, the first rollups are hashed, the first
		 * non-hashed one takes the (already-sorted) input, and following ones
		 * do their own sort.
		 */
		if (is_first)
		{
			cost_agg(&pathnode->path, root,
					 aggstrategy,
					 agg_costs,
					 numGroupCols,
					 rollup->numGroups,
					 having_qual,
					 subpath->startup_cost,
					 subpath->total_cost,
					 subpath->rows);
			is_first = false;
			if (!rollup->is_hashed)
				is_first_sort = false;
		}
		else
		{
			Path		sort_path;	/* dummy for result of cost_sort */
			Path		agg_path;	/* dummy for result of cost_agg */

			if (rollup->is_hashed || is_first_sort)
			{
				/*
				 * Account for cost of aggregation, but don't charge input
				 * cost again
				 */
				cost_agg(&agg_path, root,
						 rollup->is_hashed ? AGG_HASHED : AGG_SORTED,
						 agg_costs,
						 numGroupCols,
						 rollup->numGroups,
						 having_qual,
						 0.0, 0.0,
						 subpath->rows);
				if (!rollup->is_hashed)
					is_first_sort = false;
			}
			else
			{
				/* Account for cost of sort, but don't charge input cost again */
				cost_sort(&sort_path, root, NIL,
						  0.0,
						  subpath->rows,
						  subpath->pathtarget->width,
						  0.0,
						  work_mem,
						  -1.0);

				/* Account for cost of aggregation */

				cost_agg(&agg_path, root,
						 AGG_SORTED,
						 agg_costs,
						 numGroupCols,
						 rollup->numGroups,
						 having_qual,
						 sort_path.startup_cost,
						 sort_path.total_cost,
						 sort_path.rows);
			}

			pathnode->path.total_cost += agg_path.total_cost;
			pathnode->path.rows += agg_path.rows;
		}
	}

	/* add tlist eval cost for each output row */
	pathnode->path.startup_cost += target->cost.startup;
	pathnode->path.total_cost += target->cost.startup +
		target->cost.per_tuple * pathnode->path.rows;

	return pathnode;
}

/*
 * create_minmaxagg_path
 *	  Creates a pathnode that represents computation of MIN/MAX aggregates
 *
 * 'rel' is the parent relation associated with the result
 * 'target' is the PathTarget to be computed
 * 'mmaggregates' is a list of MinMaxAggInfo structs
 * 'quals' is the HAVING quals if any
 */
MinMaxAggPath *
create_minmaxagg_path(PlannerInfo *root,
					  RelOptInfo *rel,
					  PathTarget *target,
					  List *mmaggregates,
					  List *quals)
{
	MinMaxAggPath *pathnode = makeNode(MinMaxAggPath);
	Cost		initplan_cost;
	ListCell   *lc;

	/* The topmost generated Plan node will be a Result */
	pathnode->path.pathtype = T_Result;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target;
	/* For now, assume we are above any joins, so no parameterization */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	/* A MinMaxAggPath implies use of initplans, so cannot be parallel-safe */
	pathnode->path.parallel_safe = false;
	pathnode->path.parallel_workers = 0;
	/* Result is one unordered row */
	pathnode->path.rows = 1;
	pathnode->path.pathkeys = NIL;

	pathnode->mmaggregates = mmaggregates;
	pathnode->quals = quals;

	/* Calculate cost of all the initplans ... */
	initplan_cost = 0;
	foreach(lc, mmaggregates)
	{
		MinMaxAggInfo *mminfo = (MinMaxAggInfo *) lfirst(lc);

		initplan_cost += mminfo->pathcost;
	}

	/* add tlist eval cost for each output row, plus cpu_tuple_cost */
	pathnode->path.startup_cost = initplan_cost + target->cost.startup;
	pathnode->path.total_cost = initplan_cost + target->cost.startup +
		target->cost.per_tuple + cpu_tuple_cost;

	/*
	 * Add cost of qual, if any --- but we ignore its selectivity, since our
	 * rowcount estimate should be 1 no matter what the qual is.
	 */
	if (quals)
	{
		QualCost	qual_cost;

		cost_qual_eval(&qual_cost, quals, root);
		pathnode->path.startup_cost += qual_cost.startup;
		pathnode->path.total_cost += qual_cost.startup + qual_cost.per_tuple;
	}

	return pathnode;
}

/*
 * create_windowagg_path
 *    创建一个表示窗口函数计算的路径节点
 *
 * 参数说明：
 * 'rel' - 与结果关联的父关系
 * 'subpath' - 表示数据源的子路径
 * 'target' - 要计算的PathTarget（结果目标列表）
 * 'windowFuncs' - WindowFunc结构体的列表，包含要执行的窗口函数
 * 'winclause' - 适用于所有windowFuncs的WindowClause
 *
 * 注意：输入数据必须按照WindowClause的PARTITION BY键和ORDER BY键进行排序
 */
WindowAggPath *
create_windowagg_path(PlannerInfo *root,
                      RelOptInfo *rel,
                      Path *subpath,
                      PathTarget *target,
                      List *windowFuncs,
                      WindowClause *winclause)
{
    // 创建WindowAggPath节点
    WindowAggPath *pathnode = makeNode(WindowAggPath);

    // 设置基本路径属性
    pathnode->path.pathtype = T_WindowAgg;   // 设置路径类型为窗口聚合
    pathnode->path.parent = rel;             // 父关系设置
    pathnode->path.pathtarget = target;      // 设置目标结果列表
    /* 目前假设此节点位于所有连接操作之上，因此不需要参数化 */
    pathnode->path.param_info = NULL;
    pathnode->path.parallel_aware = false;   // 窗口聚合操作不是并行感知的
    // 窗口聚合操作的并行安全性取决于父关系是否考虑并行执行以及子路径是否并行安全
    pathnode->path.parallel_safe = rel->consider_parallel &&
        subpath->parallel_safe;
    // 并行工作进程数继承自子路径
    pathnode->path.parallel_workers = subpath->parallel_workers;
    /* 窗口聚合操作保留输入的排序顺序 */
    pathnode->path.pathkeys = subpath->pathkeys;

    // 设置窗口聚合特有的属性
    pathnode->subpath = subpath;             // 子路径（数据源）
    pathnode->winclause = winclause;         // 窗口子句信息

    /*
     * 出于成本计算的目的，假设不存在冗余的分区或排序列；
     * 处理这种特殊情况在当前实现中不值得额外开销。
     * 因此，我们直接将未修改的列表长度传递给cost_windowagg。
     */
    // 计算窗口聚合操作的成本
    cost_windowagg(&pathnode->path, root,
                   windowFuncs,              // 要执行的窗口函数列表
                   list_length(winclause->partitionClause),  // 分区列数量
                   list_length(winclause->orderClause),      // 排序列数量
                   subpath->startup_cost,    // 子路径启动成本
                   subpath->total_cost,      // 子路径总成本
                   subpath->rows);           // 子路径估计行数

    /* 添加目标列表评估成本到每行输出 */
    // 添加目标列表启动成本
    pathnode->path.startup_cost += target->cost.startup;
    // 添加目标列表启动成本和每行处理成本
    pathnode->path.total_cost += target->cost.startup +
        target->cost.per_tuple * pathnode->path.rows;

    // 返回创建的窗口聚合路径节点
    return pathnode;
}


/*
 * create_setop_path
 *	  Creates a pathnode that represents computation of INTERSECT or EXCEPT
 *
 * 'rel' is the parent relation associated with the result
 * 'subpath' is the path representing the source of data
 * 'cmd' is the specific semantics (INTERSECT or EXCEPT, with/without ALL)
 * 'strategy' is the implementation strategy (sorted or hashed)
 * 'distinctList' is a list of SortGroupClause's representing the grouping
 * 'flagColIdx' is the column number where the flag column will be, if any
 * 'firstFlag' is the flag value for the first input relation when hashing;
 *		or -1 when sorting
 * 'numGroups' is the estimated number of distinct groups
 * 'outputRows' is the estimated number of output rows
 */
SetOpPath *
create_setop_path(PlannerInfo *root,
				  RelOptInfo *rel,
				  Path *subpath,
				  SetOpCmd cmd,
				  SetOpStrategy strategy,
				  List *distinctList,
				  AttrNumber flagColIdx,
				  int firstFlag,
				  double numGroups,
				  double outputRows)
{
	SetOpPath  *pathnode = makeNode(SetOpPath);

	pathnode->path.pathtype = T_SetOp;
	pathnode->path.parent = rel;
	/* SetOp doesn't project, so use source path's pathtarget */
	pathnode->path.pathtarget = subpath->pathtarget;
	/* For now, assume we are above any joins, so no parameterization */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		subpath->parallel_safe;
	pathnode->path.parallel_workers = subpath->parallel_workers;
	/* SetOp preserves the input sort order if in sort mode */
	pathnode->path.pathkeys =
		(strategy == SETOP_SORTED) ? subpath->pathkeys : NIL;

	pathnode->subpath = subpath;
	pathnode->cmd = cmd;
	pathnode->strategy = strategy;
	pathnode->distinctList = distinctList;
	pathnode->flagColIdx = flagColIdx;
	pathnode->firstFlag = firstFlag;
	pathnode->numGroups = numGroups;

	/*
	 * Charge one cpu_operator_cost per comparison per input tuple. We assume
	 * all columns get compared at most of the tuples.
	 */
	pathnode->path.startup_cost = subpath->startup_cost;
	pathnode->path.total_cost = subpath->total_cost +
		cpu_operator_cost * subpath->rows * list_length(distinctList);
	pathnode->path.rows = outputRows;

	return pathnode;
}

/*
 * create_recursiveunion_path
 *	  Creates a pathnode that represents a recursive UNION node
 *
 * 'rel' is the parent relation associated with the result
 * 'leftpath' is the source of data for the non-recursive term
 * 'rightpath' is the source of data for the recursive term
 * 'target' is the PathTarget to be computed
 * 'distinctList' is a list of SortGroupClause's representing the grouping
 * 'wtParam' is the ID of Param representing work table
 * 'numGroups' is the estimated number of groups
 *
 * For recursive UNION ALL, distinctList is empty and numGroups is zero
 */
RecursiveUnionPath *
create_recursiveunion_path(PlannerInfo *root,
						   RelOptInfo *rel,
						   Path *leftpath,
						   Path *rightpath,
						   PathTarget *target,
						   List *distinctList,
						   int wtParam,
						   double numGroups)
{
	RecursiveUnionPath *pathnode = makeNode(RecursiveUnionPath);

	pathnode->path.pathtype = T_RecursiveUnion;
	pathnode->path.parent = rel;
	pathnode->path.pathtarget = target;
	/* For now, assume we are above any joins, so no parameterization */
	pathnode->path.param_info = NULL;
	pathnode->path.parallel_aware = false;
	pathnode->path.parallel_safe = rel->consider_parallel &&
		leftpath->parallel_safe && rightpath->parallel_safe;
	/* Foolish, but we'll do it like joins for now: */
	pathnode->path.parallel_workers = leftpath->parallel_workers;
	/* RecursiveUnion result is always unsorted */
	pathnode->path.pathkeys = NIL;

	pathnode->leftpath = leftpath;
	pathnode->rightpath = rightpath;
	pathnode->distinctList = distinctList;
	pathnode->wtParam = wtParam;
	pathnode->numGroups = numGroups;

	cost_recursive_union(&pathnode->path, leftpath, rightpath);

	return pathnode;
}

/*
 * create_lockrows_path
 *    创建表示获取行锁操作的路径节点
 *    
 * 这个函数在查询优化器中构建LockRowsPath节点，表示需要对查询结果行执行行锁定操作。
 * LockRowsPath通常用于实现FOR UPDATE/SHARE等锁定子句，确保在事务期间对所选行的隔离性。
 *
 * 参数说明：
 * 'root' - 规划器信息结构体指针，包含查询相关的全局信息
 * 'rel' - 与结果关联的父关系（RelOptInfo结构体指针）
 * 'subpath' - 表示数据源的路径节点，LockRows将在此基础上执行锁定操作
 * 'rowMarks' - PlanRowMark结构体列表，定义需要锁定的行和锁定类型
 * 'epqParam' - EvalPlanQual重新评估使用的参数ID，用于行可见性检查
 *
 * 返回值：
 * 返回新创建的LockRowsPath路径节点指针
 */
LockRowsPath *
create_lockrows_path(PlannerInfo *root, RelOptInfo *rel,
                     Path *subpath, List *rowMarks, int epqParam)
{
    // 分配并初始化LockRowsPath节点
    LockRowsPath *pathnode = makeNode(LockRowsPath);

    // 设置路径节点的基本属性
    pathnode->path.pathtype = T_LockRows;  // 标识这是LockRows类型的路径节点
    pathnode->path.parent = rel;           // 设置父关系
    /* LockRows不执行投影操作，所以直接使用源路径的目标列表 */
    pathnode->path.pathtarget = subpath->pathtarget;
    /* 目前假设LockRows位于所有连接操作之上，因此没有参数化信息 */
    pathnode->path.param_info = NULL;
    // 设置并行执行相关属性：LockRows不支持并行执行
    pathnode->path.parallel_aware = false;
    pathnode->path.parallel_safe = false;
    pathnode->path.parallel_workers = 0;
    // 行数估计继承自子路径
    pathnode->path.rows = subpath->rows;

    /*
     * 结果不能被假定为已排序，因为锁定操作可能导致排序键列被替换为新值。
     * 例如，在可重复读隔离级别下，行锁定可能触发EvalPlanQual重新评估，
     * 导致返回不同版本的行，从而破坏原有的排序顺序。
     */
    pathnode->path.pathkeys = NIL;

    // 设置LockRowsPath特有的属性
    pathnode->subpath = subpath;    // 存储子路径（数据源路径）
    pathnode->rowMarks = rowMarks;  // 存储行锁定标记列表
    pathnode->epqParam = epqParam;  // 存储EvalPlanQual参数ID

    /*
     * 行锁定和可能的重新获取需要额外成本，但具体数值难以精确估算。
     * 目前的实现中，每行额外增加cpu_tuple_cost的成本。
     * 这里使用cpu_tuple_cost作为行处理的CPU成本估算基础。
     */
    pathnode->path.startup_cost = subpath->startup_cost;  // 启动成本继承自子路径
    pathnode->path.total_cost = subpath->total_cost +    // 总成本 = 子路径成本 + 行锁定额外成本
        cpu_tuple_cost * subpath->rows;                  // 额外成本 = CPU元组成本 × 行数

    return pathnode;  // 返回创建的路径节点
}


/*
 * create_modifytable_path
 *    创建表示执行INSERT/UPDATE/DELETE修改操作的路径节点
 *    
 * 这个函数在查询优化器中构建ModifyTablePath节点，用于实现SQL中的数据修改操作（插入、更新、删除）。
 * ModifyTable是PostgreSQL执行引擎中的一个核心节点，负责协调数据修改的各个方面，包括约束检查、
 * RETURNING子句处理、ON CONFLICT处理等复杂功能。
 *
 * 参数说明：
 * 'root' - 规划器信息结构体指针，包含查询相关的全局信息
 * 'rel' - 与结果关联的父关系（RelOptInfo结构体指针）
 * 'operation' - 操作类型（CMD_INSERT/CMD_UPDATE/CMD_DELETE等）
 * 'canSetTag' - 是否设置命令标签/es_processed计数
 * 'nominalRelation' - 用于EXPLAIN显示的父关系RT索引
 * 'rootRelation' - 分区表根表的RT索引，如果不是分区表则为0
 * 'partColsUpdated' - 是否更新了任何分区列（目标关系或其子分区表）
 * 'resultRelations' - 实际目标关系的RT索引整数列表
 * 'subpaths' - 产生源数据的路径列表（每个关系一个）
 * 'subroots' - PlannerInfo结构体列表（每个关系一个）
 * 'withCheckOptionLists' - WITH CHECK OPTION列表（每个关系一个）
 * 'returningLists' - RETURNING目标列表（每个关系一个）
 * 'rowMarks' - PlanRowMark列表（仅限非锁定标记）
 * 'onconflict' - ON CONFLICT子句，如果没有则为NULL
 * 'epqParam' - EvalPlanQual重新评估使用的参数ID
 *
 * 返回值：
 * 返回新创建的ModifyTablePath路径节点指针
 */
ModifyTablePath *
create_modifytable_path(PlannerInfo *root, RelOptInfo *rel,
                      CmdType operation, bool canSetTag,
                      Index nominalRelation, Index rootRelation,
                      bool partColsUpdated,
                      List *resultRelations, List *subpaths,
                      List *subroots,
                      List *withCheckOptionLists, List *returningLists,
                      List *rowMarks, OnConflictExpr *onconflict,
                      int epqParam)
{
    // 分配并初始化ModifyTablePath节点
    ModifyTablePath *pathnode = makeNode(ModifyTablePath);
    double    total_size;  // 用于计算平均行大小的临时变量
    ListCell   *lc;        // 用于遍历子路径列表的迭代器

    // 验证参数的一致性：各个列表长度应该匹配
    Assert(list_length(resultRelations) == list_length(subpaths));
    Assert(list_length(resultRelations) == list_length(subroots));
    Assert(withCheckOptionLists == NIL ||
           list_length(resultRelations) == list_length(withCheckOptionLists));
    Assert(returningLists == NIL ||
           list_length(resultRelations) == list_length(returningLists));

    // 设置路径节点的基本属性
    pathnode->path.pathtype = T_ModifyTable;  // 标识这是ModifyTable类型的路径节点
    pathnode->path.parent = rel;              // 设置父关系
    /* pathtarget不是关键，只需设置为最小有效状态 */
    pathnode->path.pathtarget = rel->reltarget;
    /* 目前假设ModifyTable位于所有连接操作之上，因此没有参数化信息 */
    pathnode->path.param_info = NULL;
    // 设置并行执行相关属性：数据修改操作不支持并行执行
    pathnode->path.parallel_aware = false;
    pathnode->path.parallel_safe = false;
    pathnode->path.parallel_workers = 0;
    // ModifyTable节点的输出没有特定排序顺序
    pathnode->path.pathkeys = NIL;

    /*
     * 计算成本和行数为所有子路径成本和行数的总和。
     *
     * 目前，我们没有为实际的表修改工作、WITH CHECK OPTIONS或RETURNING表达式
     * 收取额外的成本。这只是表面工作，因为ModifyTable始终是顶层节点，
     * 成本不会影响任何更高层次的规划选择。但将来我们可能想要改进这一点。
     */
    pathnode->path.startup_cost = 0;
    pathnode->path.total_cost = 0;
    pathnode->path.rows = 0;
    total_size = 0;
    // 遍历所有子路径，累加成本和行数
    foreach(lc, subpaths)
    {
        Path   *subpath = (Path *) lfirst(lc);

        if (lc == list_head(subpaths))    /* 第一个节点？ */
            pathnode->path.startup_cost = subpath->startup_cost;
        pathnode->path.total_cost += subpath->total_cost;
        pathnode->path.rows += subpath->rows;
        total_size += subpath->pathtarget->width * subpath->rows;
    }

    /*
     * 将宽度设置为子路径输出的平均宽度。注意：这个处理方法不够准确：
     * 如果没有RETURNING子句，我们应该报告零宽度；如果有，应该报告RETURNING
     * 目标列表宽度的平均值。但这是历史上的处理方式，改进它是未来的任务。
     */
    if (pathnode->path.rows > 0)
        total_size /= pathnode->path.rows;
    pathnode->path.pathtarget->width = rint(total_size);

    // 设置ModifyTablePath特有的属性
    pathnode->operation = operation;                // 操作类型（INSERT/UPDATE/DELETE）
    pathnode->canSetTag = canSetTag;                // 是否设置命令标签
    pathnode->nominalRelation = nominalRelation;    // 用于EXPLAIN的关系索引
    pathnode->rootRelation = rootRelation;          // 分区表根表索引
    pathnode->partColsUpdated = partColsUpdated;    // 是否更新了分区列
    pathnode->resultRelations = resultRelations;    // 目标关系列表
    pathnode->subpaths = subpaths;                  // 数据源路径列表
    pathnode->subroots = subroots;                  // PlannerInfo结构体列表
    pathnode->withCheckOptionLists = withCheckOptionLists;  // WITH CHECK OPTION列表
    pathnode->returningLists = returningLists;      // RETURNING子句目标列表
    pathnode->rowMarks = rowMarks;                  // 行标记列表
    pathnode->onconflict = onconflict;              // ON CONFLICT子句
    pathnode->epqParam = epqParam;                  // EvalPlanQual参数ID

    return pathnode;  // 返回创建的路径节点
}


/*
 * create_limit_path
 *    创建表示执行LIMIT/OFFSET操作的路径节点
 *    
 * 这个函数在查询优化器中构建LimitPath节点，用于实现SQL查询中的LIMIT和OFFSET子句。
 * LIMIT/OFFSET操作允许查询只返回结果集的一部分，通常用于分页查询或限制大型结果集的输出。
 *
 * 参数说明：
 * 'root' - 规划器信息结构体指针，包含查询相关的全局信息
 * 'rel' - 与结果关联的父关系（RelOptInfo结构体指针）
 * 'subpath' - 表示数据源的路径节点，LIMIT/OFFSET将在此基础上执行
 * 'limitOffset' - 实际的OFFSET表达式，如果没有则为NULL
 * 'limitCount' - 实际的LIMIT表达式，如果没有则为NULL
 * 'offset_est' - OFFSET表达式的估计值，用于成本计算
 * 'count_est' - LIMIT表达式的估计值，用于成本计算
 *               注意：0表示该子句不存在，-1表示存在但无法估计值
 *
 * 返回值：
 * 返回新创建的LimitPath路径节点指针
 */
LimitPath *
create_limit_path(PlannerInfo *root, RelOptInfo *rel,
                 Path *subpath,
                 Node *limitOffset, Node *limitCount,
                 int64 offset_est, int64 count_est)
{
    // 分配并初始化LimitPath节点
    LimitPath  *pathnode = makeNode(LimitPath);

    // 设置路径节点的基本属性
    pathnode->path.pathtype = T_Limit;  // 标识这是Limit类型的路径节点
    pathnode->path.parent = rel;        // 设置父关系
    /* Limit不执行投影操作，所以直接使用源路径的目标列表 */
    pathnode->path.pathtarget = subpath->pathtarget;
    /* 目前假设Limit位于所有连接操作之上，因此没有参数化信息 */
    pathnode->path.param_info = NULL;
    // 设置并行执行相关属性
    pathnode->path.parallel_aware = false;  // Limit操作不是并行感知的
    // 并行安全标志由父关系的并行考虑和子路径的并行安全共同决定
    pathnode->path.parallel_safe = rel->consider_parallel &&
        subpath->parallel_safe;
    // 并行工作进程数继承自子路径
    pathnode->path.parallel_workers = subpath->parallel_workers;
    
    // 初始化行数和成本，稍后会根据LIMIT/OFFSET进行调整
    pathnode->path.rows = subpath->rows;
    pathnode->path.startup_cost = subpath->startup_cost;
    pathnode->path.total_cost = subpath->total_cost;
    // 排序键继承自子路径，因为LIMIT不会改变结果的排序顺序
    pathnode->path.pathkeys = subpath->pathkeys;
    
    // 设置LimitPath特有的属性
    pathnode->subpath = subpath;          // 存储子路径（数据源路径）
    pathnode->limitOffset = limitOffset;  // 存储OFFSET表达式
    pathnode->limitCount = limitCount;    // 存储LIMIT表达式

    /*
     * 根据OFFSET和LIMIT的估计值调整输出行数和成本。
     * 这个调整非常重要，因为LIMIT/OFFSET可能显著减少需要处理的行数，
     * 从而影响整体查询计划的成本估算和选择。
     */
    adjust_limit_rows_costs(&pathnode->path.rows,
                           &pathnode->path.startup_cost,
                           &pathnode->path.total_cost,
                           offset_est, count_est);

    return pathnode;  // 返回创建的路径节点
}


/*
 * adjust_limit_rows_costs
 *	  Adjust the size and cost estimates for a LimitPath node according to the
 *	  offset/limit.
 *
 * This is only a cosmetic issue if we are at top level, but if we are
 * building a subquery then it's important to report correct info to the outer
 * planner.
 *
 * When the offset or count couldn't be estimated, use 10% of the estimated
 * number of rows emitted from the subpath.
 *
 * XXX we don't bother to add eval costs of the offset/limit expressions
 * themselves to the path costs.  In theory we should, but in most cases those
 * expressions are trivial and it's just not worth the trouble.
 */
void
adjust_limit_rows_costs(double *rows,	/* in/out parameter */
						Cost *startup_cost, /* in/out parameter */
						Cost *total_cost,	/* in/out parameter */
						int64 offset_est,
						int64 count_est)
{
	double		input_rows = *rows;
	Cost		input_startup_cost = *startup_cost;
	Cost		input_total_cost = *total_cost;

	if (offset_est != 0)
	{
		double		offset_rows;

		if (offset_est > 0)
			offset_rows = (double) offset_est;
		else
			offset_rows = clamp_row_est(input_rows * 0.10);
		if (offset_rows > *rows)
			offset_rows = *rows;
		if (input_rows > 0)
			*startup_cost +=
				(input_total_cost - input_startup_cost)
				* offset_rows / input_rows;
		*rows -= offset_rows;
		if (*rows < 1)
			*rows = 1;
	}

	if (count_est != 0)
	{
		double		count_rows;

		if (count_est > 0)
			count_rows = (double) count_est;
		else
			count_rows = clamp_row_est(input_rows * 0.10);
		if (count_rows > *rows)
			count_rows = *rows;
		if (input_rows > 0)
			*total_cost = *startup_cost +
				(input_total_cost - input_startup_cost)
				* count_rows / input_rows;
		*rows = count_rows;
		if (*rows < 1)
			*rows = 1;
	}
}


/*
 * reparameterize_path
 *    尝试修改路径以增加其参数化程度
 * 
 * 参数说明：
 *    root - 规划器全局信息结构体指针，包含查询优化过程中的所有上下文信息
 *    path - 需要重新参数化的路径指针
 *    required_outer - 位图集合，表示路径所需的外部关系ID
 *    loop_count - 循环计数，用于成本估算的调整因子
 * 
 * 返回值：
 *    Path* - 返回重新参数化后的新路径，如果无法重新参数化则返回NULL
 * 
 * 功能说明：
 *    此函数用于尝试将路径的参数化程度增加到指定水平，主要用于确保append关系的所有子路径
 *    具有相同的参数化级别，从而保证它们都强制执行相同的连接条件集。
 *    目前仅支持少数路径类型，但可以根据需要添加更多支持。
 * 
 * 设计说明：
 *    我们有意不将创建的路径传递给add_path()；它可能会因为成本低于原始路径而尝试删除它们，
 *    但我们不希望这样。这里创建的路径不一定具有通用用途，但作为append路径的成员可能很有用。
 */
Path *
reparameterize_path(PlannerInfo *root, Path *path,
                    Relids required_outer,
                    double loop_count)
{
    RelOptInfo *rel = path->parent;  /* 获取路径所属的关系 */

    /* 只能增加路径的参数化程度，不能减少 */
    if (!bms_is_subset(PATH_REQ_OUTER(path), required_outer))
        return NULL;  /* 如果当前参数化不满足要求，返回NULL */

    switch (path->pathtype)  /* 根据路径类型分别处理 */
    {
        case T_SeqScan:  /* 顺序扫描路径 */
            return create_seqscan_path(root, rel, required_outer, 0);  /* 创建新的顺序扫描路径 */

        case T_SampleScan:  /* 采样扫描路径 */
            return (Path *) create_samplescan_path(root, rel, required_outer);  /* 创建新的采样扫描路径 */

        case T_IndexScan:  /* 索引扫描路径 */
        case T_IndexOnlyScan:  /* 仅索引扫描路径 */
            {
                IndexPath  *ipath = (IndexPath *) path;  /* 转换为索引路径 */
                IndexPath  *newpath = makeNode(IndexPath);  /* 创建新的索引路径节点 */

                /*
                 * 我们不能直接使用create_index_path，也不希望这样做，
                 * 因为它会重新计算索引条件，这是浪费精力的。
                 * 相反，我们稍微修改一下：平面复制路径节点，修改其param_info，并重新进行成本估算。
                 */
                memcpy(newpath, ipath, sizeof(IndexPath));  /* 复制原始路径的所有内容 */
                newpath->path.param_info =
                    get_baserel_parampathinfo(root, rel, required_outer);  /* 更新参数化信息 */
                cost_index(newpath, root, loop_count, false);  /* 重新计算索引路径成本 */
                return (Path *) newpath;  /* 返回新的索引路径 */
            }

        case T_BitmapHeapScan:  /* 位图堆扫描路径 */
            {
                BitmapHeapPath *bpath = (BitmapHeapPath *) path;  /* 转换为位图堆扫描路径 */

                return (Path *) create_bitmap_heap_path(root,  /* 创建新的位图堆扫描路径 */
                                                        rel,
                                                        bpath->bitmapqual,  /* 使用原始位图条件 */
                                                        required_outer,
                                                        loop_count, 0);
            }

        case T_SubqueryScan:  /* 子查询扫描路径 */
            {
                SubqueryScanPath *spath = (SubqueryScanPath *) path;  /* 转换为子查询扫描路径 */

                return (Path *) create_subqueryscan_path(root,  /* 创建新的子查询扫描路径 */
                                                         rel,
                                                         spath->subpath,  /* 使用原始子路径 */
                                                         spath->path.pathkeys,  /* 保持路径键 */
                                                         required_outer);
            }

        case T_Result:  /* 结果扫描路径 */
            /* 仅支持RTE_RESULT扫描路径 */
            if (IsA(path, Path))
                return create_resultscan_path(root, rel, required_outer);  /* 创建结果扫描路径 */
            break;

        case T_Append:  /* Append路径 */
            {
                AppendPath *apath = (AppendPath *) path;  /* 转换为Append路径 */
                List       *childpaths = NIL;  /* 常规子路径列表 */
                List       *partialpaths = NIL;  /* 部分子路径列表 */
                int         i;  /* 子路径索引计数器 */
                ListCell   *lc;  /* 列表遍历指针 */

                /* 重新参数化所有子路径 */
                i = 0;
                foreach(lc, apath->subpaths)  /* 遍历所有子路径 */
                {
                    Path       *spath = (Path *) lfirst(lc);  /* 获取当前子路径 */

                    spath = reparameterize_path(root, spath,  /* 递归重新参数化子路径 */
                                                required_outer,
                                                loop_count);
                    if (spath == NULL)
                        return NULL;  /* 如果子路径重新参数化失败，整个操作失败 */
                    
                    /* 我们必须重新分离常规路径和部分路径 */
                    if (i < apath->first_partial_path)
                        childpaths = lappend(childpaths, spath);  /* 添加到常规路径列表 */
                    else
                        partialpaths = lappend(partialpaths, spath);  /* 添加到部分路径列表 */
                    i++;
                }
                
                return (Path *)  /* 创建新的Append路径 */
                    create_append_path(root, rel, childpaths, partialpaths,
                                       apath->path.pathkeys, required_outer,
                                       apath->path.parallel_workers,
                                       apath->path.parallel_aware,
                                       apath->partitioned_rels,
                                       -1);
            }

        default:  /* 不支持的路径类型 */
            break;
    }
    
    return NULL;  /* 无法重新参数化该路径类型 */
}


/*
 * reparameterize_path_by_child
 *      将由给定子关系的父关系参数化的路径，转换为由给定子关系参数化的路径。
 *
 * 该函数创建与给定路径相同类型的新路径，但将其参数化调整为使用给定的子关系。
 * 原始路径的大多数字段可以简单地平面复制，但任何表达式都必须调整以引用正确的变量号，
 * 任何嵌套路径都必须递归地重新参数化。其他引用特定关系ID的字段也需要调整。
 *
 * 成本、行数、宽度和并行路径属性依赖于path->parent，这在转换过程中不会改变。
 * 因此，这些成员原样复制。
 *
 * 如果给定的路径无法重新参数化，函数返回NULL。
 */
Path *
reparameterize_path_by_child(PlannerInfo *root, Path *path,
                             RelOptInfo *child_rel)
{

/* 定义宏：平面复制路径节点 */
#define FLAT_COPY_PATH(newnode, node, nodetype)  \
	( (newnode) = makeNode(nodetype), \
	  memcpy((newnode), (node), sizeof(nodetype)) )

/* 定义宏：调整子关系属性引用 */
#define ADJUST_CHILD_ATTRS(node) \
	((node) = \
	 (List *) adjust_appendrel_attrs_multilevel(root, (Node *) (node), \
						child_rel->relids, \
						child_rel->top_parent_relids))

/* 定义宏：递归重新参数化子路径 */
#define REPARAMETERIZE_CHILD_PATH(path) \
do { \
	(path) = reparameterize_path_by_child(root, (path), child_rel); \
	if ((path) == NULL) \
		return NULL; \
} while(0)

/* 定义宏：递归重新参数化子路径列表 */
#define REPARAMETERIZE_CHILD_PATH_LIST(pathlist) \
do { \
	if ((pathlist) != NIL) \
	{ \
		(pathlist) = reparameterize_pathlist_by_child(root, (pathlist), \
							  child_rel); \
		if ((pathlist) == NIL) \
			return NULL; \
	} \
} while(0)

	Path       *new_path;        /* 新创建的重新参数化路径 */
	ParamPathInfo *new_ppi;      /* 新的参数路径信息 */
	ParamPathInfo *old_ppi;      /* 原始路径的参数信息 */
	Relids      required_outer;  /* 调整后的外部关系要求 */

	/*
	 * 如果路径不是由给定关系的父关系参数化的，则不需要重新参数化。
	 * 直接返回原始路径，避免不必要的处理。
	 */
	if (!path->param_info ||
		!bms_overlap(PATH_REQ_OUTER(path), child_rel->top_parent_relids))
		return path;

	/*
	 * 如果可能，重新参数化给定的路径，创建一个副本。
	 *
	 * 此函数当前仅应用于嵌套循环连接的内侧，该连接正被分区连接代码分区。
	 * 因此，我们只需要支持在此上下文中可能出现的路径类型。
	 * （特别是，支持排序路径类型是代码和周期的浪费：即使我们在这里翻译它们，
	 * 它们也会在后续的成本比较中失败。）如果我们确实看到不支持的路径类型，
	 * 这只意味着我们将无法使用该路径类型生成分区连接计划。
	 */
	switch (nodeTag(path))
	{
		case T_Path:

			/*
			 * 如果路径的限制子句包含对另一个关系的横向引用，我们无法重新参数化，
			 * 因为我们不能在此处更改RelOptInfo的内容。
			 * （如果我们最终使用非分区连接，这样做会破坏功能。）
			 */
			if (ris_contain_references_to(root,
							path->parent->baserestrictinfo,
							child_rel->top_parent_relids))
				return NULL;

			/*
			 * 如果是SampleScan且其表采样参数引用另一个关系，我们无法重新参数化，
			 * 因为我们不能在此处更改RTE的内容。
			 * （如果我们最终使用非分区连接，这样做会破坏功能。）
			 */
			if (path->pathtype == T_SampleScan)
			{
				Index      scan_relid = path->parent->relid;
				RangeTblEntry *rte;

				/* 应该是一个带有表采样子句的基础关系... */
				Assert(scan_relid > 0);
				rte = planner_rt_fetch(scan_relid, root);
				Assert(rte->rtekind == RTE_RELATION);
				Assert(rte->tablesample != NULL);

				if (contain_references_to(root, (Node *) rte->tablesample,
							child_rel->top_parent_relids))
					return NULL;
			}

			/* 创建Path节点的平面副本 */
			FLAT_COPY_PATH(new_path, path, Path);
			break;

		case T_IndexPath:
			{
				IndexPath  *ipath;

				/*
				 * 如果路径的限制子句包含对另一个关系的横向引用，我们无法重新参数化，
				 * 因为我们不能在此处更改IndexOptInfo的内容。
				 */
				if (ris_contain_references_to(root,
							path->parent->baserestrictinfo,
							child_rel->top_parent_relids))
					return NULL;

				/* 创建IndexPath节点的平面副本并调整索引子句的引用 */
				FLAT_COPY_PATH(ipath, path, IndexPath);
				ADJUST_CHILD_ATTRS(ipath->indexclauses);
				new_path = (Path *) ipath;
			}
			break;

		case T_BitmapHeapPath:
			{
				BitmapHeapPath *bhpath;

				/*
				 * 检查横向引用限制
				 */
				if (ris_contain_references_to(root,
							path->parent->baserestrictinfo,
							child_rel->top_parent_relids))
					return NULL;

				/* 创建BitmapHeapPath节点的平面副本并重新参数化位图条件 */
				FLAT_COPY_PATH(bhpath, path, BitmapHeapPath);
				REPARAMETERIZE_CHILD_PATH(bhpath->bitmapqual);
				new_path = (Path *) bhpath;
			}
			break;

		case T_BitmapAndPath:
			{
				BitmapAndPath *bapath;

				/* 创建BitmapAndPath节点的平面副本并重新参数化位图条件列表 */
				FLAT_COPY_PATH(bapath, path, BitmapAndPath);
				REPARAMETERIZE_CHILD_PATH_LIST(bapath->bitmapquals);
				new_path = (Path *) bapath;
			}
			break;

		case T_BitmapOrPath:
			{
				BitmapOrPath *bopath;

				/* 创建BitmapOrPath节点的平面副本并重新参数化位图条件列表 */
				FLAT_COPY_PATH(bopath, path, BitmapOrPath);
				REPARAMETERIZE_CHILD_PATH_LIST(bopath->bitmapquals);
				new_path = (Path *) bopath;
			}
			break;

		case T_ForeignPath:
			{
				ForeignPath *fpath;
				ReparameterizeForeignPathByChild_function rfpc_func;

				/* 检查横向引用限制 */
				if (ris_contain_references_to(root,
							path->parent->baserestrictinfo,
							child_rel->top_parent_relids))
					return NULL;

				/* 创建ForeignPath节点的平面副本 */
				FLAT_COPY_PATH(fpath, path, ForeignPath);
				/* 重新参数化外部路径（如果存在） */
				if (fpath->fdw_outerpath)
					REPARAMETERIZE_CHILD_PATH(fpath->fdw_outerpath);

				/* 如有必要，将重新参数化工作交给FDW（外部数据包装器） */
				rfpc_func =
					path->parent->fdwroutine->ReparameterizeForeignPathByChild;
				if (rfpc_func)
					fpath->fdw_private = rfpc_func(root, fpath->fdw_private,
									   child_rel);
				new_path = (Path *) fpath;
			}
			break;

		case T_CustomPath:
			{
				CustomPath *cpath;

				/* 检查横向引用限制 */
				if (ris_contain_references_to(root,
							path->parent->baserestrictinfo,
							child_rel->top_parent_relids))
					return NULL;

				/* 创建CustomPath节点的平面副本并重新参数化自定义路径列表 */
				FLAT_COPY_PATH(cpath, path, CustomPath);
				REPARAMETERIZE_CHILD_PATH_LIST(cpath->custom_paths);
				/* 如有必要，调用自定义路径方法的重新参数化函数 */
				if (cpath->methods &&
					cpath->methods->ReparameterizeCustomPathByChild)
					cpath->custom_private =
						cpath->methods->ReparameterizeCustomPathByChild(root,
												cpath->custom_private,
												child_rel);
				new_path = (Path *) cpath;
			}
			break;

		case T_NestPath:
			{
				JoinPath   *jpath;

				/* 创建NestPath节点的平面副本 */
				FLAT_COPY_PATH(jpath, path, NestPath);

				/* 递归重新参数化外部和内部连接路径 */
				REPARAMETERIZE_CHILD_PATH(jpath->outerjoinpath);
				REPARAMETERIZE_CHILD_PATH(jpath->innerjoinpath);
				/* 调整连接限制信息的引用 */
				ADJUST_CHILD_ATTRS(jpath->joinrestrictinfo);
				new_path = (Path *) jpath;
			}
			break;

		case T_MergePath:
			{
				JoinPath   *jpath;
				MergePath  *mpath;

				/* 创建MergePath节点的平面副本 */
				FLAT_COPY_PATH(mpath, path, MergePath);

				jpath = (JoinPath *) mpath;
				/* 递归重新参数化外部和内部连接路径 */
				REPARAMETERIZE_CHILD_PATH(jpath->outerjoinpath);
				REPARAMETERIZE_CHILD_PATH(jpath->innerjoinpath);
				/* 调整连接限制信息和合并条件的引用 */
				ADJUST_CHILD_ATTRS(jpath->joinrestrictinfo);
				ADJUST_CHILD_ATTRS(mpath->path_mergeclauses);
				new_path = (Path *) mpath;
			}
			break;

		case T_HashPath:
			{
				JoinPath   *jpath;
				HashPath   *hpath;

				/* 创建HashPath节点的平面副本 */
				FLAT_COPY_PATH(hpath, path, HashPath);

				jpath = (JoinPath *) hpath;
				/* 递归重新参数化外部和内部连接路径 */
				REPARAMETERIZE_CHILD_PATH(jpath->outerjoinpath);
				REPARAMETERIZE_CHILD_PATH(jpath->innerjoinpath);
				/* 调整连接限制信息和哈希条件的引用 */
				ADJUST_CHILD_ATTRS(jpath->joinrestrictinfo);
				ADJUST_CHILD_ATTRS(hpath->path_hashclauses);
				new_path = (Path *) hpath;
			}
			break;

		case T_AppendPath:
			{
				AppendPath *apath;

				/* 创建AppendPath节点的平面副本并重新参数化子路径列表 */
				FLAT_COPY_PATH(apath, path, AppendPath);
				REPARAMETERIZE_CHILD_PATH_LIST(apath->subpaths);
				new_path = (Path *) apath;
			}
			break;

		case T_GatherPath:
			{
				GatherPath *gpath;

				/* 创建GatherPath节点的平面副本并重新参数化子路径 */
				FLAT_COPY_PATH(gpath, path, GatherPath);
				REPARAMETERIZE_CHILD_PATH(gpath->subpath);
				new_path = (Path *) gpath;
			}
			break;

		default:

			/* 我们不知道如何重新参数化这种路径类型。 */
			return NULL;
	}

	/*
	 * 调整参数化信息，该信息引用最顶层的父级。
	 * 最顶层父级可能与给定子级有多个层级的距离，
	 * 因此使用多级表达式调整例程。
	 */
	old_ppi = new_path->param_info;
	required_outer =
		adjust_child_relids_multilevel(root, old_ppi->ppi_req_outer,
						   child_rel->relids,
						   child_rel->top_parent_relids);

	/* 如果我们已经有了这种参数化的PPI，就直接返回它 */
	new_ppi = find_param_path_info(new_path->parent, required_outer);

	/*
	 * 如果没有，则构建一个新的PPI并将其链接到PPIs列表。
	 * 出于与mark_dummy_rel()中解释的相同原因，在与给定RelOptInfo相同的上下文中分配新的PPI。
	 */
	if (new_ppi == NULL)
	{
		MemoryContext oldcontext;
		RelOptInfo *rel = path->parent;

		/* 切换到适当的内存上下文 */
		oldcontext = MemoryContextSwitchTo(GetMemoryChunkContext(rel));

		/* 创建新的参数路径信息节点 */
		new_ppi = makeNode(ParamPathInfo);
		new_ppi->ppi_req_outer = bms_copy(required_outer);
		new_ppi->ppi_rows = old_ppi->ppi_rows;
		new_ppi->ppi_clauses = old_ppi->ppi_clauses;
		/* 调整参数条件的引用 */
		ADJUST_CHILD_ATTRS(new_ppi->ppi_clauses);
		/* 将新PPI添加到关系的PPI列表 */
		rel->ppilist = lappend(rel->ppilist, new_ppi);

		/* 切换回原始内存上下文 */
		MemoryContextSwitchTo(oldcontext);
	}
	/* 释放临时使用的关系ID集合 */
	bms_free(required_outer);

	/* 更新新路径的参数信息 */
	new_path->param_info = new_ppi;

	/*
	 * 如果外部关系的父级在目标列表中被引用，则调整路径目标。
	 * 当只有外部关系的父级在此关系中被横向引用时，可能会发生这种情况。
	 */
	if (bms_overlap(path->parent->lateral_relids,
			child_rel->top_parent_relids))
	{
		/* 复制路径目标并调整其表达式引用 */
		new_path->pathtarget = copy_pathtarget(new_path->pathtarget);
		ADJUST_CHILD_ATTRS(new_path->pathtarget->exprs);
	}

	/* 返回重新参数化后的路径 */
	return new_path;
}


/*
 * reparameterize_pathlist_by_child
 * 		Helper function to reparameterize a list of paths by given child rel.
 */
static List *
reparameterize_pathlist_by_child(PlannerInfo *root,
								 List *pathlist,
								 RelOptInfo *child_rel)
{
	ListCell   *lc;
	List	   *result = NIL;

	foreach(lc, pathlist)
	{
		Path	   *path = reparameterize_path_by_child(root, lfirst(lc),
														child_rel);

		if (path == NULL)
		{
			list_free(result);
			return NIL;
		}

		result = lappend(result, path);
	}

	return result;
}

/*
 * contain_references_to
 *		Detect whether any Vars or PlaceHolderVars in the given clause contain
 *		lateral references to the given 'relids'.
 */
static bool
contain_references_to(PlannerInfo *root, Node *clause, Relids relids)
{
	bool		ret = false;
	List	   *vars;
	ListCell   *lc;

	/*
	 * Examine all Vars and PlaceHolderVars used in the clause.
	 *
	 * By omitting the relevant flags, this also gives us a cheap sanity check
	 * that no aggregates or window functions appear in the clause.  We don't
	 * expect any of those in scan-level restrictions or tablesamples.
	 */
	vars = pull_var_clause(clause, PVC_INCLUDE_PLACEHOLDERS);
	foreach(lc, vars)
	{
		Node	   *node = (Node *) lfirst(lc);

		if (IsA(node, Var))
		{
			Var		   *var = (Var *) node;

			if (bms_is_member(var->varno, relids))
			{
				ret = true;
				break;
			}
		}
		else if (IsA(node, PlaceHolderVar))
		{
			PlaceHolderVar *phv = (PlaceHolderVar *) node;
			PlaceHolderInfo *phinfo = find_placeholder_info(root, phv, false);

			/*
			 * We should check both ph_eval_at (in case the PHV is to be
			 * computed at the other relation and then laterally referenced
			 * here) and ph_lateral (in case the PHV is to be evaluated here
			 * but contains lateral references to the other relation).  The
			 * former case should not occur in baserestrictinfo clauses, but
			 * it can occur in tablesample clauses.
			 */
			if (bms_overlap(phinfo->ph_eval_at, relids) ||
				bms_overlap(phinfo->ph_lateral, relids))
			{
				ret = true;
				break;
			}
		}
		else
			Assert(false);
	}

	list_free(vars);

	return ret;
}

/*
 * ris_contain_references_to
 *		Apply contain_references_to() to a list of RestrictInfos.
 *
 * We need extra code for this because pull_var_clause() can't descend
 * through RestrictInfos.
 */
static bool
ris_contain_references_to(PlannerInfo *root, List *rinfos, Relids relids)
{
	ListCell   *lc;

	foreach(lc, rinfos)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		/* Pseudoconstant clauses can't contain any Vars or PHVs */
		if (rinfo->pseudoconstant)
			continue;
		if (contain_references_to(root, (Node *) rinfo->clause, relids))
			return true;
	}
	return false;
}
