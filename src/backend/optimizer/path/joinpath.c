/*-------------------------------------------------------------------------
 *
 * joinpath.c
 *	  Routines to find all possible paths for processing a set of joins
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/joinpath.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "executor/executor.h"
#include "foreign/fdwapi.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"

/* Hook for plugins to get control in add_paths_to_joinrel() */
set_join_pathlist_hook_type set_join_pathlist_hook = NULL;

/*
 * 如果路径被父关系参数化，则可以认为它也被其任何子关系参数化。
 */
#define PATH_PARAM_BY_PARENT(path, rel)	\
	((path)->param_info && bms_overlap(PATH_REQ_OUTER(path),	\
									   (rel)->top_parent_relids))

/*
 * 判断路径是否被指定的关系自身参数化。
 */
#define PATH_PARAM_BY_REL_SELF(path, rel)  \
	((path)->param_info && bms_overlap(PATH_REQ_OUTER(path), (rel)->relids))

/*
 * 判断路径是否被指定的关系（自身或父关系）参数化。
 */
#define PATH_PARAM_BY_REL(path, rel)	\
	(PATH_PARAM_BY_REL_SELF(path, rel) || PATH_PARAM_BY_PARENT(path, rel))

static void try_partial_mergejoin_path(PlannerInfo *root,
									   RelOptInfo *joinrel,
									   Path *outer_path,
									   Path *inner_path,
									   List *pathkeys,
									   List *mergeclauses,
									   List *outersortkeys,
									   List *innersortkeys,
									   JoinType jointype,
									   JoinPathExtraData *extra);
static void sort_inner_and_outer(PlannerInfo *root, RelOptInfo *joinrel,
								 RelOptInfo *outerrel, RelOptInfo *innerrel,
								 JoinType jointype, JoinPathExtraData *extra);
static void match_unsorted_outer(PlannerInfo *root, RelOptInfo *joinrel,
								 RelOptInfo *outerrel, RelOptInfo *innerrel,
								 JoinType jointype, JoinPathExtraData *extra);
static void consider_parallel_nestloop(PlannerInfo *root,
									   RelOptInfo *joinrel,
									   RelOptInfo *outerrel,
									   RelOptInfo *innerrel,
									   JoinType jointype,
									   JoinPathExtraData *extra);
static void consider_parallel_mergejoin(PlannerInfo *root,
										RelOptInfo *joinrel,
										RelOptInfo *outerrel,
										RelOptInfo *innerrel,
										JoinType jointype,
										JoinPathExtraData *extra,
										Path *inner_cheapest_total);
static void hash_inner_and_outer(PlannerInfo *root, RelOptInfo *joinrel,
								 RelOptInfo *outerrel, RelOptInfo *innerrel,
								 JoinType jointype, JoinPathExtraData *extra);
static List *select_mergejoin_clauses(PlannerInfo *root,
									  RelOptInfo *joinrel,
									  RelOptInfo *outerrel,
									  RelOptInfo *innerrel,
									  List *restrictlist,
									  JoinType jointype,
									  bool *mergejoin_allowed);
static void generate_mergejoin_paths(PlannerInfo *root,
									 RelOptInfo *joinrel,
									 RelOptInfo *innerrel,
									 Path *outerpath,
									 JoinType jointype,
									 JoinPathExtraData *extra,
									 bool useallclauses,
									 Path *inner_cheapest_total,
									 List *merge_pathkeys,
									 bool is_partial);

/*
 * add_paths_to_joinrel
 *  给定一个连接关系和两个组成关系（可用于构建该连接关系），
 *  考虑所有可能的路径，这些路径使用两个组成关系分别作为外部和内部关系。
 *  如果这些路径在与其他路径比较后存活，则将这些路径添加到连接关系的路径列表中
 *  （并移除任何被这些路径支配的现有路径）。
 *
 * 修改 joinrel 节点的 pathlist 字段，使其包含目前为止找到的最佳路径。
 *
 * jointype 不一定与 sjinfo->jointype 相同；如果我们考虑以与 sjinfo 指示的方向相反的方式连接关系，
 * 它可能会"翻转"。
 *
 * 此外，本例程和本模块中的其他例程接受特殊的 JoinTypes
 * JOIN_UNIQUE_OUTER 和 JOIN_UNIQUE_INNER，表示我们应该
 * 对外部或内部关系进行唯一化，然后应用常规的内部连接。
 * 但是，这些值不允许在本模块之外传播。
 * 路径成本估算代码可能需要识别它正在处理这种情况——
 * 名义 jointype 为 INNER 且 sjinfo->jointype == JOIN_SEMI 的组合表示这种情况。
 */
void
add_paths_to_joinrel(PlannerInfo *root,        /* 规划器全局信息结构 */
                     RelOptInfo *joinrel,     /* 目标连接关系 */
                     RelOptInfo *outerrel,    /* 外部（左）关系 */
                     RelOptInfo *innerrel,    /* 内部（右）关系 */
                     JoinType jointype,       /* 连接类型 */
                     SpecialJoinInfo *sjinfo, /* 特殊连接信息（半连接等） */
                     List *restrictlist)      /* 连接条件列表 */
{
    /* 额外的连接路径信息结构体，用于存储路径生成过程中需要的各种参数 */
    JoinPathExtraData extra;
    /* 是否允许合并连接 */
    bool    mergejoin_allowed = true;
    /* 是否考虑连接下推到外部数据源 */
    bool    consider_join_pushdown = false;
    /* 用于遍历链表的单元格指针 */
    ListCell   *lc;
    /* 连接关系的关系ID集合 */
    Relids    joinrelids;

    /*
     * PlannerInfo 不包含为子关系之间的连接创建的 SpecialJoinInfos，
     * 即使对于顶层父关系之间的连接有 SpecialJoinInfo 节点。
     * 因此，在计算表示限制的 Relids 集时，要考虑分区顶层父的 relids。
     */
    if (joinrel->reloptkind == RELOPT_OTHER_JOINREL) /* 如果是其他类型的连接关系 */
        joinrelids = joinrel->top_parent_relids; /* 使用顶层父关系ID */
    else
        joinrelids = joinrel->relids; /* 使用普通关系ID */

    /* 初始化额外信息结构体 */
    extra.restrictlist = restrictlist;          /* 设置限制条件列表 */
    extra.mergeclause_list = NIL;               /* 初始化合并子句列表为空 */
    extra.sjinfo = sjinfo;                      /* 设置特殊连接信息 */
    extra.param_source_rels = NULL;             /* 初始化参数源关系为空集 */

    /*
     * 查看对于此外部关系，内部关系是否被证明是唯一的。
     *
     * 有一些特殊情况：对于 JOIN_SEMI 和 JOIN_ANTI，没关系，
     * 因为执行器可以做等效的优化；我们不需要在证明上花费规划器的周期。
     * 对于 JOIN_UNIQUE_INNER，我们必须考虑一个内侧不是唯一的半连接
     * （否则 reduce_unique_semijoins 会简化它），因此调用 innerrel_is_unique 没有意义。
     * 但是，如果 LHS 覆盖了所有的 semijoin 的 min_lefthand，
     * 那么设置 inner_unique 是合适的，因为 create_unique_path 产生的路径
     * 对于 LHS 是唯一的。（如果我们的 LHS 只是 min_lefthand 的一部分，则不成立。）
     * 对于 JOIN_UNIQUE_OUTER，传递 JOIN_INNER 以避免该值泄露到本模块之外。
     */
    switch (jointype)
    {
        case JOIN_SEMI:  /* 半连接 */
        case JOIN_ANTI:  /* 反连接 */
            extra.inner_unique = false; /* 未证明唯一 */
            break;
        case JOIN_UNIQUE_INNER:  /* 内部关系去重的连接 */
            /* 检查左半部分是否是半连接最小左操作数的子集 */
            extra.inner_unique = bms_is_subset(sjinfo->min_lefthand,
                                              outerrel->relids);
            break;
        case JOIN_UNIQUE_OUTER:  /* 外部关系去重的连接 */
            /* 检查内部关系是否唯一，使用JOIN_INNER避免特殊值泄露 */
            extra.inner_unique = innerrel_is_unique(root,
                                                  joinrel->relids,
                                                  outerrel->relids,
                                                  innerrel,
                                                  JOIN_INNER,
                                                  restrictlist,
                                                  false);
            break;
        default:  /* 其他连接类型 */
            /* 正常检查内部关系是否唯一 */
            extra.inner_unique = innerrel_is_unique(root,
                                                  joinrel->relids,
                                                  outerrel->relids,
                                                  innerrel,
                                                  jointype,
                                                  restrictlist,
                                                  false);
            break;
    }

    /*
     * 对于 JOIN_FULL 连接，我们必须考虑合并连接（mergejoin）子句，
     * 因为全外连接只能通过 MergeJoin 实现。
     * 查找潜在的合并连接（mergejoin）子句。如果我们不打算做合并连接，可以跳过这一步。
     * 但是，合并连接可能是实现全外连接的唯一方式，因此如果是全连接则覆盖 enable_mergejoin。
     */
    if (enable_mergejoin || jointype == JOIN_FULL)
        /* 选择适合合并连接的子句 */
        extra.mergeclause_list = select_mergejoin_clauses(root,
                                                        joinrel,
                                                        outerrel,
                                                        innerrel,
                                                        restrictlist,
                                                        jointype,
                                                        &mergejoin_allowed);

    /*
     * 对于 JOIN_SEMI、JOIN_ANTI 或 inner_unique 连接，
     * 我们需要为成本估算计算修正因子。
     * 这些因子对于所有路径都是一样的。
     */
    if (jointype == JOIN_SEMI || jointype == JOIN_ANTI || extra.inner_unique)
        /* 计算半连接/反连接的成本修正因子 */
        compute_semi_anti_join_factors(root, joinrel, outerrel, innerrel,
                                     jointype, sjinfo, restrictlist,
                                     &extra.semifactors);

    /*
     * 决定是否有必要为此连接关系生成参数化路径，如果需要，哪些关系应作为参数源。
     * 通常，除非有连接顺序限制，阻止将我们的输入关系之一直接连接到参数源关系，
     * 而不是连接到另一个输入关系，否则没有必要创建参数化结果路径。
     * （但见 allow_star_schema_join()。）
     * 这种限制减少了我们在更高连接层级需要处理的参数化路径数量，
     * 而不会影响结果计划的质量。
     * 我们用一个 Relids 集来表达这种限制，任何建议的连接路径的参数化都必须与该集有重叠。
     */
    foreach(lc, root->join_info_list) /* 遍历所有特殊连接信息 */
    {
        SpecialJoinInfo *sjinfo2 = (SpecialJoinInfo *) lfirst(lc);

        /*
         * 如果我们有它 RHS 的某部分（可能不是全部），并且还没有连接到它的 LHS，
         * 则 SJ 与此连接相关。（这个测试很简单，但考虑到连接已经被证明是合法的，应该足够了。）
         * 如果 SJ 相关，则它对连接到 RHS 之外的任何东西提出约束。
         */
        if (bms_overlap(joinrelids, sjinfo2->min_righthand) && /* 检查是否与右操作数重叠 */
            !bms_overlap(joinrelids, sjinfo2->min_lefthand)) /* 且与左操作数不重叠 */
            /* 添加除右操作数外的所有基础关系作为参数源 */
            extra.param_source_rels = bms_join(extra.param_source_rels,
                                             bms_difference(root->all_baserels,
                                                          sjinfo2->min_righthand));

        /* 全连接对两边都对称约束 */
        if (sjinfo2->jointype == JOIN_FULL && /* 如果是全外连接 */
            bms_overlap(joinrelids, sjinfo2->min_lefthand) && /* 检查是否与左操作数重叠 */
            !bms_overlap(joinrelids, sjinfo2->min_righthand)) /* 且与右操作数不重叠 */
            /* 添加除左操作数外的所有基础关系作为参数源 */
            extra.param_source_rels = bms_join(extra.param_source_rels,
                                             bms_difference(root->all_baserels,
                                                          sjinfo2->min_lefthand));
    }

    /*
     * 然而，当涉及 LATERAL 子查询时，除非其参数化在连接关系内被解决，
     * 否则 joinrel 不会有不被参数化的路径。
     * 因此我们不妨允许 joinrel 的剩余 lateral 依赖作为额外的参数源。
     */
    /* 添加剩余的LATERAL依赖关系作为参数源 */
    extra.param_source_rels = bms_add_members(extra.param_source_rels,
                                            joinrel->lateral_relids);

    /*
     * 1. 考虑需要对两个关系都显式排序的合并连接路径。如果不能合并连接则跳过。
     */
    if (mergejoin_allowed)
        /* 处理需要对内外关系都排序的合并连接路径 */
        sort_inner_and_outer(root, joinrel, outerrel, innerrel,
                           jointype, &extra);

    /*
     * 2. 考虑外部关系不需要显式排序的路径。这包括外部路径已经有序的 NestLoop 和 MergeJoin。
     * 同样，如果不能 MergeJoin 则跳过。（这是可以的，因为我们知道嵌套循环无法处理右/全连接，
     * 所以在禁止的情况下也不会工作。）
     */
    if (mergejoin_allowed)
        /* 处理外部关系已经有序的连接路径 */
        match_unsorted_outer(root, joinrel, outerrel, innerrel,
                           jointype, &extra);

#ifdef NOT_USED

    /*
     * 3. 考虑内部关系不需要显式排序的路径。这只包括合并连接（嵌套循环已在 match_unsorted_outer 中构建）。
     *
     * 2000-02-13 tgl：认为是冗余的而移除。合并连接的内外两侧没有本质区别，
     * 所以当以相反顺序调用 add_paths_to_joinrel() 时，match_unsorted_inner 不会创建新的路径。
     */
    if (mergejoin_allowed)
        match_unsorted_inner(root, joinrel, outerrel, innerrel,
                           jointype, &extra);
#endif

    /*
     * 4. 考虑在连接前需要对外部和内部关系都进行哈希的路径。同上，对于全连接忽略 enable_hashjoin，
     * 因为可能没有其他选择。
     */
    if (enable_hashjoin || jointype == JOIN_FULL)
        /* 处理哈希连接路径 */
        hash_inner_and_outer(root, joinrel, outerrel, innerrel,
                           jointype, &extra);

    /*
     * createplan.c 当前不支持处理由扩展推送下来的连接分配的伪常量子句；
     * 检查 restrictlist 是否有这样的子句，如果没有，则允许考虑推送下来的连接。
     */
    if ((joinrel->fdwroutine &&
         joinrel->fdwroutine->GetForeignJoinPaths) || /* 检查是否有外部数据包装器连接路径函数 */
        set_join_pathlist_hook) /* 或者设置了连接路径列表钩子 */
        /* 如果没有伪常量子句，则考虑连接下推 */
        consider_join_pushdown = !has_pseudoconstant_clauses(root,
                                                          restrictlist);

    /*
     * 5. 如果内外关系都是属于同一服务器且分配给同一用户检查访问权限的外部表（或连接），
     * 则让 FDW 有机会推送下连接。
     */
    if (joinrel->fdwroutine &&
        joinrel->fdwroutine->GetForeignJoinPaths &&
        consider_join_pushdown)
        /* 调用外部数据包装器的连接路径生成函数 */
        joinrel->fdwroutine->GetForeignJoinPaths(root, joinrel,
                                              outerrel, innerrel,
                                              jointype, &extra);

    /*
     * 6. 最后，给扩展一个操作路径列表的机会。
     * 它们可以通过调用 add_path() 或 add_partial_path()（如果支持并行）添加新路径（如 CustomPaths）。
     * 也可以删除或修改核心代码添加的路径。
     */
    if (set_join_pathlist_hook &&
        consider_join_pushdown)
        /* 调用连接路径列表钩子 */
        set_join_pathlist_hook(root, joinrel, outerrel, innerrel,
                            jointype, &extra);
}


/*
 * We override the param_source_rels heuristic to accept nestloop paths in
 * which the outer rel satisfies some but not all of the inner path's
 * parameterization.  This is necessary to get good plans for star-schema
 * scenarios, in which a parameterized path for a large table may require
 * parameters from multiple small tables that will not get joined directly to
 * each other.  We can handle that by stacking nestloops that have the small
 * tables on the outside; but this breaks the rule the param_source_rels
 * heuristic is based on, namely that parameters should not be passed down
 * across joins unless there's a join-order-constraint-based reason to do so.
 * So we ignore the param_source_rels restriction when this case applies.
 *
 * allow_star_schema_join() returns true if the param_source_rels restriction
 * should be overridden, ie, it's okay to perform this join.
 */
static inline bool
allow_star_schema_join(PlannerInfo *root,
					   Relids outerrelids,
					   Relids inner_paramrels)
{
	/*
	 * It's a star-schema case if the outer rel provides some but not all of
	 * the inner rel's parameterization.
	 */
	return (bms_overlap(inner_paramrels, outerrelids) &&
			bms_nonempty_difference(inner_paramrels, outerrelids));
}


/*
 * try_nestloop_path
 *	  考虑一个嵌套循环连接路径；如果看起来有用，则通过 add_path() 将其加入 joinrel 的 pathlist。
 */
static void
try_nestloop_path(PlannerInfo *root,
				  RelOptInfo *joinrel,
				  Path *outer_path,
				  Path *inner_path,
				  List *pathkeys,
				  JoinType jointype,
				  JoinPathExtraData *extra)
{
	Relids		required_outer;	/* 嵌套循环所需的额外参数化 */
	JoinCostWorkspace workspace;
	RelOptInfo *innerrel = inner_path->parent;
	RelOptInfo *outerrel = outer_path->parent;
	Relids		innerrelids;
	Relids		outerrelids;
	Relids		inner_paramrels = PATH_REQ_OUTER(inner_path);
	Relids		outer_paramrels = PATH_REQ_OUTER(outer_path);

	/*
	 * 路径是由顶层父关系参数化的，因此在父 relids 上进行参数化测试。
	 */
	if (innerrel->top_parent_relids)
		innerrelids = innerrel->top_parent_relids;
	else
		innerrelids = innerrel->relids;

	if (outerrel->top_parent_relids)
		outerrelids = outerrel->top_parent_relids;
	else
		outerrelids = outerrel->relids;

	/*
	 * 检查建议的路径是否仍然被参数化，如果参数化不合理则拒绝——
	 * 除非 allow_star_schema_join 允许。还必须拒绝 have_dangerous_phv 不喜欢的情况，
	 * 这只会在嵌套循环仍被参数化时发生。
	 */
	required_outer = calc_nestloop_required_outer(outerrelids, outer_paramrels,
												  innerrelids, inner_paramrels);
	if (required_outer &&
		((!bms_overlap(required_outer, extra->param_source_rels) &&
		  !allow_star_schema_join(root, outerrelids, inner_paramrels)) ||
		 have_dangerous_phv(root, outerrelids, inner_paramrels)))
	{
		/* 拒绝路径时释放内存 */
		bms_free(required_outer);
		return;
	}

	/*
	 * 预检查以快速排除明显劣势的路径。我们计算路径成本的一个廉价下界，
	 * 然后用 add_path_precheck() 判断该路径是否会被 joinrel 的某个已有路径支配。
	 * 如果不会，则继续创建完整的路径结构并提交给 add_path()。
	 * 后两步开销较大，因此采用两阶段方法。
	 */
	initial_cost_nestloop(root, &workspace, jointype,
						  outer_path, inner_path, extra);

	if (add_path_precheck(joinrel,
						  workspace.startup_cost, workspace.total_cost,
						  pathkeys, required_outer))
	{
		/*
		 * 如果 inner_path 被参数化，则它是由 outerrel 的顶层父关系参数化的，而不是 outerrel 本身。
		 * 需要修正这一点。
		 */
		if (PATH_PARAM_BY_PARENT(inner_path, outer_path->parent))
		{
			inner_path = reparameterize_path_by_child(root, inner_path,
													  outer_path->parent);

			/*
			 * 如果无法转换路径，则无法创建嵌套循环路径。
			 */
			if (!inner_path)
			{
				bms_free(required_outer);
				return;
			}
		}

		add_path(joinrel, (Path *)
				 create_nestloop_path(root,
									  joinrel,
									  jointype,
									  &workspace,
									  extra,
									  outer_path,
									  inner_path,
									  extra->restrictlist,
									  pathkeys,
									  required_outer));
	}
	else
	{
		/* 拒绝路径时释放内存 */
		bms_free(required_outer);
	}
}

/*
 * try_partial_nestloop_path
 *	  Consider a partial nestloop join path; if it appears useful, push it into
 *	  the joinrel's partial_pathlist via add_partial_path().
 */
static void
try_partial_nestloop_path(PlannerInfo *root,
						  RelOptInfo *joinrel,
						  Path *outer_path,
						  Path *inner_path,
						  List *pathkeys,
						  JoinType jointype,
						  JoinPathExtraData *extra)
{
	JoinCostWorkspace workspace;

	/*
	 * If the inner path is parameterized, the parameterization must be fully
	 * satisfied by the proposed outer path.  Parameterized partial paths are
	 * not supported.  The caller should already have verified that no
	 * extra_lateral_rels are required here.
	 */
	Assert(bms_is_empty(joinrel->lateral_relids));
	if (inner_path->param_info != NULL)
	{
		Relids		inner_paramrels = inner_path->param_info->ppi_req_outer;
		RelOptInfo *outerrel = outer_path->parent;
		Relids		outerrelids;

		/*
		 * The inner and outer paths are parameterized, if at all, by the top
		 * level parents, not the child relations, so we must use those relids
		 * for our parameterization tests.
		 */
		if (outerrel->top_parent_relids)
			outerrelids = outerrel->top_parent_relids;
		else
			outerrelids = outerrel->relids;

		if (!bms_is_subset(inner_paramrels, outerrelids))
			return;
	}

	/*
	 * Before creating a path, get a quick lower bound on what it is likely to
	 * cost.  Bail out right away if it looks terrible.
	 */
	initial_cost_nestloop(root, &workspace, jointype,
						  outer_path, inner_path, extra);
	if (!add_partial_path_precheck(joinrel, workspace.total_cost, pathkeys))
		return;

	/*
	 * If the inner path is parameterized, it is parameterized by the topmost
	 * parent of the outer rel, not the outer rel itself.  Fix that.
	 */
	if (PATH_PARAM_BY_PARENT(inner_path, outer_path->parent))
	{
		inner_path = reparameterize_path_by_child(root, inner_path,
												  outer_path->parent);

		/*
		 * If we could not translate the path, we can't create nest loop path.
		 */
		if (!inner_path)
			return;
	}

	/* Might be good enough to be worth trying, so let's try it. */
	add_partial_path(joinrel, (Path *)
					 create_nestloop_path(root,
										  joinrel,
										  jointype,
										  &workspace,
										  extra,
										  outer_path,
										  inner_path,
										  extra->restrictlist,
										  pathkeys,
										  NULL));
}

/*
 * try_mergejoin_path
 *	  考虑一个合并连接（merge join）路径；如果看起来有用，则通过 add_path() 将其加入 joinrel 的 pathlist。
 */
static void
try_mergejoin_path(PlannerInfo *root,
				   RelOptInfo *joinrel,
				   Path *outer_path,
				   Path *inner_path,
				   List *pathkeys,
				   List *mergeclauses,
				   List *outersortkeys,
				   List *innersortkeys,
				   JoinType jointype,
				   JoinPathExtraData *extra,
				   bool is_partial)
{
	Relids		required_outer;
	JoinCostWorkspace workspace;

	/* 如果是部分路径，则直接调用 try_partial_mergejoin_path */
	if (is_partial)
	{
		try_partial_mergejoin_path(root,
								   joinrel,
								   outer_path,
								   inner_path,
								   pathkeys,
								   mergeclauses,
								   outersortkeys,
								   innersortkeys,
								   jointype,
								   extra);
		return;
	}

	/*
	 * 检查建议的路径是否仍然被参数化，如果参数化不合理则拒绝。
	 */
	required_outer = calc_non_nestloop_required_outer(outer_path,
													  inner_path);
	if (required_outer &&
		!bms_overlap(required_outer, extra->param_source_rels))
	{
		/* 拒绝路径时释放内存 */
		bms_free(required_outer);
		return;
	}

	/*
	 * 如果输入路径已经有足够的排序，则可以跳过显式排序。
	 */
	if (outersortkeys &&
		pathkeys_contained_in(outersortkeys, outer_path->pathkeys))
		outersortkeys = NIL;
	if (innersortkeys &&
		pathkeys_contained_in(innersortkeys, inner_path->pathkeys))
		innersortkeys = NIL;

	/*
	 * 预检查以快速排除明显劣势的路径。计算路径成本的下界，
	 * 用 add_path_precheck 判断该路径是否会被 joinrel 的某个已有路径支配。
	 * 如果不会，则继续创建完整的路径结构并提交给 add_path()。
	 */
	initial_cost_mergejoin(root, &workspace, jointype, mergeclauses,
						   outer_path, inner_path,
						   outersortkeys, innersortkeys,
						   extra);

	if (add_path_precheck(joinrel,
						  workspace.startup_cost, workspace.total_cost,
						  pathkeys, required_outer))
	{
		add_path(joinrel, (Path *)
				 create_mergejoin_path(root,
									   joinrel,
									   jointype,
									   &workspace,
									   extra,
									   outer_path,
									   inner_path,
									   extra->restrictlist,
									   pathkeys,
									   required_outer,
									   mergeclauses,
									   outersortkeys,
									   innersortkeys));
	}
	else
	{
		/* 拒绝路径时释放内存 */
		bms_free(required_outer);
	}
}

/*
 * try_partial_mergejoin_path - 尝试创建部分归并连接路径
 *
 * 该函数用于考虑创建一个部分归并连接路径，如果该路径看起来有用，
 * 就通过add_partial_path()将其添加到连接关系的路径列表中。
 *
 * 归并连接是一种高效的连接算法，要求两个输入路径都按照连接键排序。
 * 部分归并连接用于并行查询处理，其中每个工作进程处理数据的一部分。
 *
 * 参数说明:
 * - root: 查询优化器的全局信息结构
 * - joinrel: 连接关系的优化信息结构
 * - outer_path: 外部路径
 * - inner_path: 内部路径
 * - pathkeys: 路径键列表，定义结果的排序顺序
 * - mergeclauses: 归并连接子句列表
 * - outersortkeys: 外部路径需要的排序键
 * - innersortkeys: 内部路径需要的排序键
 * - jointype: 连接类型（如INNER JOIN, LEFT JOIN等）
 * - extra: 连接路径的额外数据结构
 *
 * 返回值:
 * - void: 无返回值
 *
 * 工作原理:
 * 1. 检查连接关系是否包含横向依赖（lateral_relids应为空）
 * 2. 检查内部路径是否包含参数信息，如果有则要求参数关系为空
 * 3. 检查输入路径是否已经满足排序要求，避免不必要的显式排序
 * 4. 计算归并连接的初始成本
 * 5. 使用预检查确定该路径是否值得进一步考虑
 * 6. 如果值得考虑，则创建归并连接路径并添加到部分路径列表中
 */
static void
try_partial_mergejoin_path(PlannerInfo *root,
						   RelOptInfo *joinrel,
						   Path *outer_path,
						   Path *inner_path,
						   List *pathkeys,
						   List *mergeclauses,
						   List *outersortkeys,
						   List *innersortkeys,
						   JoinType jointype,
						   JoinPathExtraData *extra)
{
	JoinCostWorkspace workspace;  /* 用于存储成本计算的工作空间 */

	/*
	 * 参见try_partial_hashjoin_path()中的注释。
	 * 断言检查连接关系不应包含横向依赖关系。
	 */
	Assert(bms_is_empty(joinrel->lateral_relids));
	
	/* 
	 * 如果内部路径包含参数信息，则获取其所需的外部关系集合。
	 * 如果该集合非空，则直接返回，不创建部分归并连接路径。
	 */
	if (inner_path->param_info != NULL)
	{
		Relids		inner_paramrels = inner_path->param_info->ppi_req_outer;

		if (!bms_is_empty(inner_paramrels))
			return;
	}

	/*
	 * 如果给定的路径已经足够有序，我们可以跳过显式的排序操作。
	 * 这是一个优化，避免不必要的排序开销。
	 */
	if (outersortkeys &&
		pathkeys_contained_in(outersortkeys, outer_path->pathkeys))
		outersortkeys = NIL;  /* 外部路径已满足排序要求，无需额外排序 */
	if (innersortkeys &&
		pathkeys_contained_in(innersortkeys, inner_path->pathkeys))
		innersortkeys = NIL;  /* 内部路径已满足排序要求，无需额外排序 */

	/*
	 * 参见try_partial_nestloop_path()中的注释。
	 * 计算归并连接的初始成本，包括I/O成本和CPU成本。
	 */
	initial_cost_mergejoin(root, &workspace, jointype, mergeclauses,
						   outer_path, inner_path,
						   outersortkeys, innersortkeys,
						   extra);

	/*
	 * 使用预检查确定该部分路径是否值得进一步考虑。
	 * 如果成本过高或不符合其他条件，则直接返回。
	 */
	if (!add_partial_path_precheck(joinrel, workspace.total_cost, pathkeys))
		return;

	/* 
	 * 路径可能足够好，值得尝试，所以让我们创建并添加它。
	 * 创建归并连接路径并将其作为部分路径添加到连接关系中。
	 */
	add_partial_path(joinrel, (Path *)
					 create_mergejoin_path(root,
										   joinrel,
										   jointype,
										   &workspace,
										   extra,
										   outer_path,
										   inner_path,
										   extra->restrictlist,
										   pathkeys,
										   NULL,
										   mergeclauses,
										   outersortkeys,
										   innersortkeys));
}


/*
 * try_hashjoin_path
 *    考虑一个哈希连接路径；如果它看起来有用，则通过add_path()将其添加到joinrel的路径列表中。
 * 
 * 参数说明：
 *    root - 规划器的全局信息结构指针
 *    joinrel - 要添加路径的连接关系
 *    outer_path - 外层关系的路径
 *    inner_path - 内层关系的路径
 *    hashclauses - 可用于哈希连接的连接条件子句列表
 *    jointype - 连接类型（如内连接、左外连接等）
 *    extra - 额外的连接路径信息
 *
 * 返回值：
 *    void - 无返回值，成功时将路径添加到joinrel的路径列表
 */
static void
try_hashjoin_path(PlannerInfo *root,
                  RelOptInfo *joinrel,
                  Path *outer_path,
                  Path *inner_path,
                  List *hashclauses,
                  JoinType jointype,
                  JoinPathExtraData *extra)
{
    Relids      required_outer;  /* 参数化路径所需要的外层关系ID集合 */
    JoinCostWorkspace workspace;  /* 连接成本计算的工作区结构 */

    /*
     * 检查提议的路径是否仍为参数化路径，如果参数化不合理则拒绝该路径。
     * calc_non_nestloop_required_outer计算非嵌套循环连接所需的外层关系。
     */
    required_outer = calc_non_nestloop_required_outer(outer_path,
                                                     inner_path);
    if (required_outer &&
        !bms_overlap(required_outer, extra->param_source_rels))
    {
        /* 当我们在此拒绝路径时不要浪费内存 */
        bms_free(required_outer);
        return;  /* 如果参数化关系不在有效源关系中，拒绝该路径 */
    }

    /*
     * 请参见try_nestloop_path()中的注释。还要注意，哈希连接路径永远没有 output pathkeys
     * 这一点在create_hashjoin_path中有说明。
     * 初始化哈希连接成本计算，填充workspace结构体
     */
    initial_cost_hashjoin(root, &workspace, jointype, hashclauses,
                         outer_path, inner_path, extra, false);

    /*
     * 使用add_path_precheck预检查该路径是否可能有用，基于其启动成本、总成本、
     * 路径键(NIL表示哈希连接无路径键)和required_outer参数
     */
    if (add_path_precheck(joinrel,
                         workspace.startup_cost, workspace.total_cost,
                         NIL, required_outer))
    {
        /*
         * 如果预检查通过，创建哈希连接路径并将其添加到joinrel的路径列表中
         * parallel_hash设置为false，表示不使用并行哈希连接
         */
        add_path(joinrel, (Path *)
                create_hashjoin_path(root,
                                    joinrel,
                                    jointype,
                                    &workspace,
                                    extra,
                                    outer_path,
                                    inner_path,
                                    false,    /* parallel_hash */
                                    extra->restrictlist,
                                    required_outer,
                                    hashclauses));
    }
    else
    {
        /* 当我们在此拒绝路径时不要浪费内存 */
        bms_free(required_outer);
    }
}


/*
 * try_partial_hashjoin_path
 *	  Consider a partial hashjoin join path; if it appears useful, push it into
 *	  the joinrel's partial_pathlist via add_partial_path().
 *	  The outer side is partial.  If parallel_hash is true, then the inner path
 *	  must be partial and will be run in parallel to create one or more shared
 *	  hash tables; otherwise the inner path must be complete and a copy of it
 *	  is run in every process to create separate identical private hash tables.
 */
static void
try_partial_hashjoin_path(PlannerInfo *root,
						  RelOptInfo *joinrel,
						  Path *outer_path,
						  Path *inner_path,
						  List *hashclauses,
						  JoinType jointype,
						  JoinPathExtraData *extra,
						  bool parallel_hash)
{
	JoinCostWorkspace workspace;

	/*
	 * If the inner path is parameterized, the parameterization must be fully
	 * satisfied by the proposed outer path.  Parameterized partial paths are
	 * not supported.  The caller should already have verified that no
	 * extra_lateral_rels are required here.
	 */
	Assert(bms_is_empty(joinrel->lateral_relids));
	if (inner_path->param_info != NULL)
	{
		Relids		inner_paramrels = inner_path->param_info->ppi_req_outer;

		if (!bms_is_empty(inner_paramrels))
			return;
	}

	/*
	 * Before creating a path, get a quick lower bound on what it is likely to
	 * cost.  Bail out right away if it looks terrible.
	 */
	initial_cost_hashjoin(root, &workspace, jointype, hashclauses,
						  outer_path, inner_path, extra, parallel_hash);
	if (!add_partial_path_precheck(joinrel, workspace.total_cost, NIL))
		return;

	/* Might be good enough to be worth trying, so let's try it. */
	add_partial_path(joinrel, (Path *)
					 create_hashjoin_path(root,
										  joinrel,
										  jointype,
										  &workspace,
										  extra,
										  outer_path,
										  inner_path,
										  parallel_hash,
										  extra->restrictlist,
										  NULL,
										  hashclauses));
}

/*
 * clause_sides_match_join
 *	  Determine whether a join clause is of the right form to use in this join.
 *
 * We already know that the clause is a binary opclause referencing only the
 * rels in the current join.  The point here is to check whether it has the
 * form "outerrel_expr op innerrel_expr" or "innerrel_expr op outerrel_expr",
 * rather than mixing outer and inner vars on either side.  If it matches,
 * we set the transient flag outer_is_left to identify which side is which.
 */
static inline bool
clause_sides_match_join(RestrictInfo *rinfo, RelOptInfo *outerrel,
						RelOptInfo *innerrel)
{
	if (bms_is_subset(rinfo->left_relids, outerrel->relids) &&
		bms_is_subset(rinfo->right_relids, innerrel->relids))
	{
		/* lefthand side is outer */
		rinfo->outer_is_left = true;
		return true;
	}
	else if (bms_is_subset(rinfo->left_relids, innerrel->relids) &&
			 bms_is_subset(rinfo->right_relids, outerrel->relids))
	{
		/* righthand side is outer */
		rinfo->outer_is_left = false;
		return true;
	}
	return false;				/* no good for these input relations */
}

/*
 * sort_inner_and_outer
 *	  通过对外部和内部连接关系进行显式排序，创建合并连接（mergejoin）路径。
 *
 * 参数说明：
 * 'joinrel' 连接关系
 * 'outerrel' 外部连接关系
 * 'innerrel' 内部连接关系
 * 'jointype' 连接类型
 * 'extra' 额外输入参数
 */
static void
sort_inner_and_outer(PlannerInfo *root,
					 RelOptInfo *joinrel,
					 RelOptInfo *outerrel,
					 RelOptInfo *innerrel,
					 JoinType jointype,
					 JoinPathExtraData *extra)
{
	JoinType	save_jointype = jointype;
	Path	   *outer_path;
	Path	   *inner_path;
	Path	   *cheapest_partial_outer = NULL;
	Path	   *cheapest_safe_inner = NULL;
	List	   *all_pathkeys;
	ListCell   *l;

	/*
	 * 这里只考虑最便宜总成本的输入路径，因为假定需要排序。
	 * 后续会考虑最便宜启动成本的输入路径，但仅当它们不需要排序时。
	 *
	 * 本函数有意不考虑参数化输入路径，除非最便宜总成本路径本身被参数化。
	 * 如果考虑，会导致合并连接路径数量爆炸，且价值有限。
	 * 这与其他地方对参数化输入的合并连接的处理有关，详见 src/backend/optimizer/README。
	 */
	outer_path = outerrel->cheapest_total_path;
	inner_path = innerrel->cheapest_total_path;

	/*
	 * 如果任一最便宜总成本路径被另一侧关系参数化，则不能使用合并连接。
	 * （无需寻找其他输入路径，因为这些已经是可用的最少参数化路径。）
	 */
	if (PATH_PARAM_BY_REL(outer_path, innerrel) ||
		PATH_PARAM_BY_REL(inner_path, outerrel))
		return;

	/*
	 * 当连接类型为 JOIN_UNIQUE_OUTER 或 JOIN_UNIQUE_INNER 时，需要对输入外表或内表进行唯一化处理。
	 * 如果需要唯一化，则先唯一化再按普通内连接处理。
	 */
	if (jointype == JOIN_UNIQUE_OUTER)
	{
		outer_path = (Path *) create_unique_path(root, outerrel,
												 outer_path, extra->sjinfo);
		Assert(outer_path);
		jointype = JOIN_INNER;
	}
	else if (jointype == JOIN_UNIQUE_INNER)
	{
		inner_path = (Path *) create_unique_path(root, innerrel,
												 inner_path, extra->sjinfo);
		Assert(inner_path);
		jointype = JOIN_INNER;
	}

	/*
	 * 如果 joinrel 支持并行，则可以考虑部分合并连接。
	 * 但不能处理 JOIN_UNIQUE_OUTER，因为外部路径为 partial，无法保证唯一性。
	 * 也不能处理 JOIN_FULL 和 JOIN_RIGHT，因为可能产生错误的 null 扩展行。
	 * 结果路径也不能被参数化。
	 */
	if (joinrel->consider_parallel &&
		save_jointype != JOIN_UNIQUE_OUTER &&
		save_jointype != JOIN_FULL &&
		save_jointype != JOIN_RIGHT &&
		outerrel->partial_pathlist != NIL &&
		bms_is_empty(joinrel->lateral_relids))
	{
		cheapest_partial_outer = (Path *) linitial(outerrel->partial_pathlist);

		if (inner_path->parallel_safe)
			cheapest_safe_inner = inner_path;
		else if (save_jointype != JOIN_UNIQUE_INNER)
			cheapest_safe_inner =
				get_cheapest_parallel_safe_total_inner(innerrel->pathlist);
	}

	/*
	 * 每种可用的合并连接子句排序都会生成不同排序的结果路径，且成本基本相同。
	 * 虽然无法在此层级选择最佳排序，但某些排序对更高层级的合并连接更有用，因此值得考虑多种排序。
	 *
	 * 实际上，并非每种合并子句排序都会生成不同的路径顺序，因为有些子句可能部分冗余（引用相同的等价类）。
	 * 因此，先将合并子句列表转换为规范的 pathkey 列表，再考虑不同的 pathkey 排序。
	 *
	 * 并不为 pathkey 的所有排列生成路径，规划时间代价太高。
	 * 当前策略是为每个 pathkey 生成一个路径，将该 pathkey 放在首位，其余随机排列。
	 * 这样至少能保证有一种排序无需重新排序即可进行合并连接。
	 * 如果次要 key 排序不理想，可能导致部分子句作为普通连接条件处理而非合并子句。
	 * （实际中合并子句数量很少，通常两三个，故无需过度优化。）
	 *
	 * select_outer_pathkeys_for_merge() 返回的 pathkey 顺序有一定启发式依据，需原样尝试，也可做变体。
	 */
	all_pathkeys = select_outer_pathkeys_for_merge(root,
												   extra->mergeclause_list,
												   joinrel);

	/*
	 * 为每个 pathkey 生成一个路径
	 * 每个 pathkey 都对应一个路径，路径的排序方式为当前 pathkey 放在首位，
	 * 其余 pathkey 随机排列。
	 */
	foreach(l, all_pathkeys)
	{
		PathKey	   *front_pathkey = (PathKey *) lfirst(l);
		List	   *cur_mergeclauses;
		List	   *outerkeys;
		List	   *innerkeys;
		List	   *merge_pathkeys;

		/* 构造以当前 pathkey 为首的 pathkey 列表 */
		if (l != list_head(all_pathkeys))
			outerkeys = lcons(front_pathkey,
							  list_delete_ptr(list_copy(all_pathkeys),
											  front_pathkey));
		else
			outerkeys = all_pathkeys;	/* 第一个无需处理 */

		/*
		 * 按当前排序整理合并子句
		 * 将mergeclause_list中的合并子句按outerkeys的顺序重新排列，得到cur_mergeclauses。
		 */
		cur_mergeclauses =
			find_mergeclauses_for_outer_pathkeys(root,
												 outerkeys,
												 extra->mergeclause_list);

		/* 应该全部用上... */
		Assert(list_length(cur_mergeclauses) == list_length(extra->mergeclause_list));

		/* 构造内表侧的排序 pathkey */
		innerkeys = make_inner_pathkeys_for_merge(root,
												  cur_mergeclauses,
												  outerkeys);

		/* 构造输出排序的 pathkey */
		merge_pathkeys = build_join_pathkeys(root, joinrel, jointype,
											 outerkeys);

		/*
		 * 现在可以生成路径了。
		 *
		 * 注意：最便宜路径可能已正确排序，try_mergejoin_path 会检测并避免显式排序。
		 */
		try_mergejoin_path(root,
						   joinrel,
						   outer_path,
						   inner_path,
						   merge_pathkeys,
						   cur_mergeclauses,
						   outerkeys,
						   innerkeys,
						   jointype,
						   extra,
						   false);

		/*
		 * 如果有 partial 外部路径和并行安全的内部路径，则尝试部分合并连接路径。
		 */
		if (cheapest_partial_outer && cheapest_safe_inner)
			try_partial_mergejoin_path(root,
									   joinrel,
									   cheapest_partial_outer,
									   cheapest_safe_inner,
									   merge_pathkeys,
									   cur_mergeclauses,
									   outerkeys,
									   innerkeys,
									   jointype,
									   extra);
	}
}
/*
 * generate_mergejoin_paths
 *	为输入的 outerpath 创建可能的合并连接（mergejoin）路径。
 *
 * 如果有可用的合并连接子句，则生成合并连接路径。生成内部路径有两种方式：
 * 一是对最便宜的内部路径进行排序，二是使用已经按要求排序的内部路径。
 * 如果有多个合并连接子句，可能没有内部路径（或只有非常昂贵的路径）能满足所有合并子句的排序要求，
 * 但如果截断合并子句列表（减少排序要求），则可能有更好的路径。因此会考虑合并子句列表的截断情况。
 * （理想情况下应考虑所有子集，但代价太高。）
 */
static void
generate_mergejoin_paths(PlannerInfo *root,
						 RelOptInfo *joinrel,
						 RelOptInfo *innerrel,
						 Path *outerpath,
						 JoinType jointype,
						 JoinPathExtraData *extra,
						 bool useallclauses,
						 Path *inner_cheapest_total,
						 List *merge_pathkeys,
						 bool is_partial)
{
	List	   *mergeclauses;
	List	   *innersortkeys;
	List	   *trialsortkeys;
	Path	   *cheapest_startup_inner;
	Path	   *cheapest_total_inner;
	JoinType	save_jointype = jointype;
	int			num_sortkeys;
	int			sortkeycnt;

	/* 如果是唯一化连接，转换为普通内连接处理 */
	if (jointype == JOIN_UNIQUE_OUTER || jointype == JOIN_UNIQUE_INNER)
		jointype = JOIN_INNER;

	/* 查找可用的合并连接子句 */
	mergeclauses =
		find_mergeclauses_for_outer_pathkeys(root,
											 outerpath->pathkeys,
											 extra->mergeclause_list);

	/*
	 * 如果没有可用的合并连接子句，则本 outerpath 不考虑合并连接。
	 * 特殊情况：如 "x FULL JOIN y ON true"，没有连接子句，但 FULL JOIN 只能用合并连接实现，
	 * 所以需要生成无连接条件的合并连接路径。
	 */
	if (mergeclauses == NIL)
	{
		if (jointype == JOIN_FULL)
			 /* FULL JOIN 可以尝试合并连接 */ ;
		else
			return;
	}
	/* 如果要求必须使用所有合并子句，但当前不满足，则不生成路径 */
	if (useallclauses &&
		list_length(mergeclauses) != list_length(extra->mergeclause_list))
		return;

	/* 计算内部路径需要的排序 key */
	innersortkeys = make_inner_pathkeys_for_merge(root,
												  mergeclauses,
												  outerpath->pathkeys);

	/*
	 * 首先尝试对最便宜的内部路径排序后进行合并连接。
	 * 因为需要排序，所以只考虑总成本最便宜的路径。
	 * （如果 inner_cheapest_total 已经排序正确，try_mergejoin_path 会自动跳过排序。）
	 */
	try_mergejoin_path(root,
					   joinrel,
					   outerpath,
					   inner_cheapest_total,
					   merge_pathkeys,
					   mergeclauses,
					   NIL,
					   innersortkeys,
					   jointype,
					   extra,
					   is_partial);

	/* 如果内部路径需要唯一化，则不能做进一步处理 */
	if (save_jointype == JOIN_UNIQUE_INNER)
		return;

	/*
	 * 查找已经按 innersortkeys 排序的内部路径，或者其截断版本（如果允许只用部分合并子句）。
	 * 这里同时考虑启动成本和总成本最便宜的路径。
	 *
	 * 当前不考虑参数化的内部路径，这与其他地方的决策有关，详见 optimizer/README。
	 *
	 * 截断排序 key 时，只考虑比之前找到的路径更便宜的路径，避免重复和无意义的计划。
	 * 如果 inner_cheapest_total 已经排序正确，则初始化最便宜路径变量为它，避免重复生成路径。
	 * 注意：如果 inner_cheapest_total 只满足更短的排序 key，则仍然可以生成不同的计划。
	 */
	if (pathkeys_contained_in(innersortkeys,
							  inner_cheapest_total->pathkeys))
	{
		/* inner_cheapest_total 已经排序，无需再排序 */
		cheapest_startup_inner = inner_cheapest_total;
		cheapest_total_inner = inner_cheapest_total;
	}
	else
	{
		/* 需要排序，至少对于全部排序 key */
		cheapest_startup_inner = NULL;
		cheapest_total_inner = NULL;
	}
	num_sortkeys = list_length(innersortkeys);
	if (num_sortkeys > 1 && !useallclauses)
		trialsortkeys = list_copy(innersortkeys);	/* 需要可修改副本 */
	else
		trialsortkeys = innersortkeys;	/* 不会截断 */

	for (sortkeycnt = num_sortkeys; sortkeycnt > 0; sortkeycnt--)
	{
		Path	   *innerpath;
		List	   *newclauses = NIL;

		/*
		 * 查找满足前 sortkeycnt 个 innersortkeys 的内部路径。
		 * 注意：trialsortkeys 会被破坏性修改，所以需要副本。
		 */
		trialsortkeys = list_truncate(trialsortkeys, sortkeycnt);
		innerpath = get_cheapest_path_for_pathkeys(innerrel->pathlist,
												   trialsortkeys,
												   NULL,
												   TOTAL_COST,
												   is_partial);
		if (innerpath != NULL &&
			(cheapest_total_inner == NULL ||
			 compare_path_costs(innerpath, cheapest_total_inner,
								TOTAL_COST) < 0))
		{
			/* 找到更便宜的已排序路径 */
			/* 如果排序 key 被截断，则需要重新选择合并子句 */
			if (sortkeycnt < num_sortkeys)
			{
				newclauses =
					trim_mergeclauses_for_inner_pathkeys(root,
														 mergeclauses,
														 trialsortkeys);
				Assert(newclauses != NIL);
			}
			else
				newclauses = mergeclauses;
			try_mergejoin_path(root,
							   joinrel,
							   outerpath,
							   innerpath,
							   merge_pathkeys,
							   newclauses,
							   NIL,
							   NIL,
							   jointype,
							   extra,
							   is_partial);
			cheapest_total_inner = innerpath;
		}
		/* 同样查找启动成本最便宜的路径 */
		innerpath = get_cheapest_path_for_pathkeys(innerrel->pathlist,
												   trialsortkeys,
												   NULL,
												   STARTUP_COST,
												   is_partial);
		if (innerpath != NULL &&
			(cheapest_startup_inner == NULL ||
			 compare_path_costs(innerpath, cheapest_startup_inner,
								STARTUP_COST) < 0))
		{
			/* 找到更便宜的已排序路径 */
			if (innerpath != cheapest_total_inner)
			{
				/*
				 * 如果已经构造过合并子句列表，则复用，节省内存。
				 */
				if (newclauses == NIL)
				{
					if (sortkeycnt < num_sortkeys)
					{
						newclauses =
							trim_mergeclauses_for_inner_pathkeys(root,
																 mergeclauses,
																 trialsortkeys);
						Assert(newclauses != NIL);
					}
					else
						newclauses = mergeclauses;
				}
				try_mergejoin_path(root,
								   joinrel,
								   outerpath,
								   innerpath,
								   merge_pathkeys,
								   newclauses,
								   NIL,
								   NIL,
								   jointype,
								   extra,
								   is_partial);
			}
			cheapest_startup_inner = innerpath;
		}

		/*
		 * 如果要求必须使用所有合并子句，则不考虑截断排序 key。
		 */
		if (useallclauses)
			break;
	}
}
/*
 * match_unsorted_outer
 *	  为处理单个连接关系 'joinrel' 创建可能的连接路径，
 *	  通过迭代替换或在每个可能的外部路径上进行合并连接（仅考虑已经有序到足以进行合并的外部路径）。
 *
 * 我们总是为每个可用的外部路径生成一个嵌套循环路径。
 * 实际上我们可能会生成多达五个：一个使用最便宜总成本的内部路径，
 * 一个对同一路径进行物化，一个使用最便宜启动成本的内部路径（如果不同），
 * 一个使用最便宜总成本的内部索引扫描路径（如果有），
 * 以及一个使用最便宜启动成本的内部索引扫描路径（如果不同）。
 *
 * 如果有可用的合并连接子句，也会考虑合并连接。详细注释见 generate_mergejoin_paths。
 *
 * 参数说明：
 * 'joinrel' 连接关系
 * 'outerrel' 外部连接关系
 * 'innerrel' 内部连接关系
 * 'jointype' 连接类型
 * 'extra' 额外输入参数
 */
static void
match_unsorted_outer(PlannerInfo *root,
					 RelOptInfo *joinrel,
					 RelOptInfo *outerrel,
					 RelOptInfo *innerrel,
					 JoinType jointype,
					 JoinPathExtraData *extra)
{
	JoinType	save_jointype = jointype;
	bool		nestjoinOK;		/* 是否支持 NestLoop 连接 */
	bool		useallclauses;	/* 是否使用所有约束语句 */
	Path	   *inner_cheapest_total = innerrel->cheapest_total_path;
	Path	   *matpath = NULL;
	ListCell   *lc1;

	/*
	 * 嵌套循环仅支持 inner、left、semi 和 anti 连接。
	 * 如果是 right 或 full 合并连接，必须使用所有合并子句作为连接子句，否则计划无效。
	 * （虽然这两个标志目前是反向的，但为了清晰和未来可能的更改，保持分离。）
	 * 对于 unique_outer 和 unique_inner，我们将其视为 inner 连接进行处理。
	 * 对于未识别的连接类型，发出错误消息。
	 */
	switch (jointype)
	{
		case JOIN_INNER:
		case JOIN_LEFT:
		case JOIN_SEMI:
		case JOIN_ANTI:
			nestjoinOK = true;
			useallclauses = false;
			break;
		case JOIN_RIGHT:
		case JOIN_FULL:
			nestjoinOK = false;
			useallclauses = true;
			break;
		case JOIN_UNIQUE_OUTER:
		case JOIN_UNIQUE_INNER:
			jointype = JOIN_INNER;
			nestjoinOK = true;
			useallclauses = false;
			break;
		default:
			elog(ERROR, "unrecognized join type: %d",
				 (int) jointype);
			nestjoinOK = false; /* 保持编译器安静 */
			useallclauses = false;
			break;
	}

	/*
	 * 如果 inner_cheapest_total 被外部关系参数化，则忽略它；
	 * 我们会在后面作为 cheapest_parameterized_paths 的成员考虑它，
	 * 但本例程考虑的其他可能性不可用。
	 */
	if (PATH_PARAM_BY_REL(inner_cheapest_total, outerrel))
		inner_cheapest_total = NULL;

	/*
	 * 当前连接类型为 JOIN_UNIQUE_INNER 时，
	 * 需要对内连接路径进行唯一化处理。
	 * 如果需要对内部路径进行唯一化，则只考虑最便宜总成本的内部路径。
	 * 否则，考虑对最便宜的内部路径进行物化，除非该路径本身已物化输出。
	 */
	if (save_jointype == JOIN_UNIQUE_INNER)
	{
		/* 无法用被外部关系参数化的内部路径实现 */
		if (inner_cheapest_total == NULL)
			return;

		/*
		 * 对最便宜总成本的内部路径进行唯一化处理。
		 */
		inner_cheapest_total = (Path*)
			create_unique_path(root, innerrel, inner_cheapest_total, extra->sjinfo);
		Assert(inner_cheapest_total);
	}
	else if (nestjoinOK)
	{
		/*
		 * 考虑对最便宜的内部路径进行物化，除非 enable_material 关闭或该路径本身已物化输出。
		 */
		if (enable_material && inner_cheapest_total != NULL &&
			!ExecMaterializesOutput(inner_cheapest_total->pathtype))
			matpath = (Path *)create_material_path(innerrel, inner_cheapest_total);
	}

	/*
	 * 为每个可用的外连接路径生成连接路径。
	 * 对每个外连接路径，考虑以下情况：
	 * 1.  cheapest_total_path
	 * 2.  cheapest_parameterized_paths
	 * 3.  物化路径（如果 enable_material 为 true 且路径未物化输出）
	 * 对于每种情况，生成 NestLoop 连接路径（如果允许）和 MergeJoin 连接路径（如果有合并子句）。
	 * 注意：对于 MergeJoin 连接路径，只有 cheapest_total_path 可用，因为参数化路径无法保证排序要求。
	 * （对于 NestLoop 连接路径，参数化路径是有用的，因为它们可以避免对内部关系进行不必要的扫描。）
	 */
	foreach(lc1, outerrel->pathlist)
	{
		Path	   *outerpath = (Path *) lfirst(lc1);
		List	   *merge_pathkeys;

		/*
		 * 不能使用被内部关系参数化的外部路径。
		 */
		if (PATH_PARAM_BY_REL(outerpath, innerrel))
			continue;

		/*
		 * 如果需要对外部路径进行唯一化，则只考虑最便宜的外部路径。
		 * （XXX 唯一化场景下未考虑参数化外部和内部路径，是否需要？）
		 */
		if (save_jointype == JOIN_UNIQUE_OUTER)
		{
			if (outerpath != outerrel->cheapest_total_path)
				continue;
			outerpath = (Path *) create_unique_path(root, outerrel,
													outerpath, extra->sjinfo);
			Assert(outerpath);
		}

		/*
		 * 计算任何我们创建的路径将具有的有用排序 PathKey。
		 * 结果将具有此排序（即使实现为 NestLoop，且部分 MergeJoin 子句作为普通连接条件处理）：
		 */
		merge_pathkeys = build_join_pathkeys(root, joinrel, jointype,
											 outerpath->pathkeys);

		if (save_jointype == JOIN_UNIQUE_INNER)
		{
			/*
			 * 只用唯一化后的最便宜内部路径考虑 NestLoop 连接
			 */
			try_nestloop_path(root,
							  joinrel,
							  outerpath,
							  inner_cheapest_total,
							  merge_pathkeys,
							  jointype,
							  extra);
		}
		else if (nestjoinOK)
		{
			/*
			 * 用此外部路径和内部关系的各种可用路径考虑 NestLoop 连接。
			 * 对每种参数化方式的内部关系的最便宜总成本路径进行考虑，包括无参数化情况。
			 */
			ListCell   *lc2;

			/* 对于参数化路径，只能使用 NestLoop 连接 */
			foreach(lc2, innerrel->cheapest_parameterized_paths)
			{
				Path	   *innerpath = (Path *) lfirst(lc2);

				try_nestloop_path(root,
								  joinrel,
								  outerpath,
								  innerpath,
								  merge_pathkeys,
								  jointype,
								  extra);
			}

			/*
			 * 当存在物化路径时，尝试使用该路径进行连接
			 * 也考虑最便宜内部路径的物化形式
			 */
			if (matpath != NULL)
				try_nestloop_path(root,
								  joinrel,
								  outerpath,
								  matpath,
								  merge_pathkeys,
								  jointype,
								  extra);
		}

		/* 如果外部路径需要唯一化，则不能做其他操作 */
		if (save_jointype == JOIN_UNIQUE_OUTER)
			continue;

		/* 如果内部关系被外部参数化，则不能做其他操作 */
		if (inner_cheapest_total == NULL)
			continue;

		/* 生成合并连接路径 */
		generate_mergejoin_paths(root, joinrel, innerrel, outerpath,
								 save_jointype, extra, useallclauses,
								 inner_cheapest_total, merge_pathkeys,
								 false);
	}

	/*
	 * 如果 outerrel 有 partial path 且 joinrel 是并行安全的，则考虑部分嵌套循环和合并连接计划。
	 * 但不能处理 JOIN_UNIQUE_OUTER，因为外部路径将是 partial，无法保证唯一性。
	 * 也不能处理 extra_lateral_rels，因为 partial 路径不能参数化。
	 * 同样不能处理 JOIN_FULL 和 JOIN_RIGHT，因为可能产生错误的 null 扩展行。
	 */
	if (joinrel->consider_parallel &&
		save_jointype != JOIN_UNIQUE_OUTER &&
		save_jointype != JOIN_FULL &&
		save_jointype != JOIN_RIGHT &&
		outerrel->partial_pathlist != NIL &&
		bms_is_empty(joinrel->lateral_relids))
	{
		if (nestjoinOK)
			consider_parallel_nestloop(root, joinrel, outerrel, innerrel,
									   save_jointype, extra);

		/*
		 * 如果 inner_cheapest_total 为 NULL 或不支持并行，则查找最便宜的并行安全路径。
		 * 如果是 JOIN_UNIQUE_INNER，则不能使用其他内部路径。
		 */
		if (inner_cheapest_total == NULL ||
			!inner_cheapest_total->parallel_safe)
		{
			if (save_jointype == JOIN_UNIQUE_INNER)
				return;

			inner_cheapest_total = get_cheapest_parallel_safe_total_inner(
																		  innerrel->pathlist);
		}

		if (inner_cheapest_total)
			consider_parallel_mergejoin(root, joinrel, outerrel, innerrel,
										save_jointype, extra,
										inner_cheapest_total);
	}
}

/*
 * consider_parallel_mergejoin
 *	  Try to build partial paths for a joinrel by joining a partial path
 *	  for the outer relation to a complete path for the inner relation.
 *
 * 'joinrel' is the join relation
 * 'outerrel' is the outer join relation
 * 'innerrel' is the inner join relation
 * 'jointype' is the type of join to do
 * 'extra' contains additional input values
 * 'inner_cheapest_total' cheapest total path for innerrel
 */
static void
consider_parallel_mergejoin(PlannerInfo *root,
							RelOptInfo *joinrel,
							RelOptInfo *outerrel,
							RelOptInfo *innerrel,
							JoinType jointype,
							JoinPathExtraData *extra,
							Path *inner_cheapest_total)
{
	ListCell   *lc1;

	/* generate merge join path for each partial outer path */
	foreach(lc1, outerrel->partial_pathlist)
	{
		Path	   *outerpath = (Path *) lfirst(lc1);
		List	   *merge_pathkeys;

		/*
		 * Figure out what useful ordering any paths we create will have.
		 */
		merge_pathkeys = build_join_pathkeys(root, joinrel, jointype,
											 outerpath->pathkeys);

		generate_mergejoin_paths(root, joinrel, innerrel, outerpath, jointype,
								 extra, false, inner_cheapest_total,
								 merge_pathkeys, true);
	}
}

/*
 * consider_parallel_nestloop
 *	  Try to build partial paths for a joinrel by joining a partial path for the
 *	  outer relation to a complete path for the inner relation.
 *
 * 'joinrel' is the join relation
 * 'outerrel' is the outer join relation
 * 'innerrel' is the inner join relation
 * 'jointype' is the type of join to do
 * 'extra' contains additional input values
 */
static void
consider_parallel_nestloop(PlannerInfo *root,
						   RelOptInfo *joinrel,
						   RelOptInfo *outerrel,
						   RelOptInfo *innerrel,
						   JoinType jointype,
						   JoinPathExtraData *extra)
{
	JoinType	save_jointype = jointype;
	ListCell   *lc1;

	if (jointype == JOIN_UNIQUE_INNER)
		jointype = JOIN_INNER;

	foreach(lc1, outerrel->partial_pathlist)
	{
		Path	   *outerpath = (Path *) lfirst(lc1);
		List	   *pathkeys;
		ListCell   *lc2;

		/* Figure out what useful ordering any paths we create will have. */
		pathkeys = build_join_pathkeys(root, joinrel, jointype,
									   outerpath->pathkeys);

		/*
		 * Try the cheapest parameterized paths; only those which will produce
		 * an unparameterized path when joined to this outerrel will survive
		 * try_partial_nestloop_path.  The cheapest unparameterized path is
		 * also in this list.
		 */
		foreach(lc2, innerrel->cheapest_parameterized_paths)
		{
			Path	   *innerpath = (Path *) lfirst(lc2);

			/* Can't join to an inner path that is not parallel-safe */
			if (!innerpath->parallel_safe)
				continue;

			/*
			 * If we're doing JOIN_UNIQUE_INNER, we can only use the inner's
			 * cheapest_total_path, and we have to unique-ify it.  (We might
			 * be able to relax this to allow other safe, unparameterized
			 * inner paths, but right now create_unique_path is not on board
			 * with that.)
			 */
			if (save_jointype == JOIN_UNIQUE_INNER)
			{
				if (innerpath != innerrel->cheapest_total_path)
					continue;
				innerpath = (Path *) create_unique_path(root, innerrel,
														innerpath,
														extra->sjinfo);
				Assert(innerpath);
			}

			try_partial_nestloop_path(root, joinrel, outerpath, innerpath,
									  pathkeys, jointype, extra);
		}
	}
}

/*
 * hash_inner_and_outer
 *    通过显式地对每个可用哈希子句的外层键和内层键进行哈希处理来创建哈希连接路径
 *
 * 'joinrel' 是要构建的连接关系
 * 'outerrel' 是连接的外层关系
 * 'innerrel' 是连接的内层关系
 * 'jointype' 是要执行的连接类型（如内连接、左连接等）
 * 'extra' 包含额外的输入值，如连接限定条件列表
 *
 * 此函数是PostgreSQL查询优化器中哈希连接路径生成的核心组件，主要完成以下工作：
 * 1. 从连接限定条件中筛选出可用于哈希连接的条件（哈希子句）
 * 2. 为不同类型的连接创建相应的哈希连接路径
 * 3. 处理特殊连接类型（如JOIN_UNIQUE_OUTER/JOIN_UNIQUE_INNER）
 * 4. 在条件允许的情况下，生成并行哈希连接路径
 */
static void
hash_inner_and_outer(PlannerInfo* root,
					 RelOptInfo* joinrel,
					 RelOptInfo* outerrel,
					 RelOptInfo* innerrel,
					 JoinType jointype,
					 JoinPathExtraData* extra)
{
	JoinType    save_jointype = jointype;  				// 保存原始连接类型，用于并行处理判断
	bool        isouterjoin = IS_OUTER_JOIN(jointype);  // 判断是否为外连接
	List* 		hashclauses;  							// 存储可用于哈希连接的子句列表
	ListCell* 	l;  									// 列表遍历指针

	/*
	 * 对于给定的外层和内层关系对，我们只需要构建一个hashclauses列表
	 * 所有可哈希的子句都将作为键使用
	 *
	 * 扫描连接的restrictinfo列表，找出可用于哈希连接且适用于这对子关系的子句
	 */
	hashclauses = NIL;  // 初始化哈希子句列表为空
	foreach(l, extra->restrictlist)  // 遍历所有限定条件
	{
		RestrictInfo* restrictinfo = (RestrictInfo*)lfirst(l);

		/*
		 * 处理外连接时，只使用其自身的连接子句进行哈希
		 * 对于内连接，我们不需要这么严格的限制
		 * is_pushed_down 是区分一个约束条件是连接条件还是过滤条件的关键
		 * is_pushed_down = true, 代表这是一个过滤条件，不适合用于哈希连接
		 * is_pushed_down = false, 代表这是一个连接条件
		 */
		if (isouterjoin && RINFO_IS_PUSHED_DOWN(restrictinfo, joinrel->relids))
			continue;  // 跳过被下推的条件

		// 检查是否可用于哈希连接
		if (!restrictinfo->can_join ||
			restrictinfo->hashjoinoperator == InvalidOid)
			continue;  // 不可用于哈希连接

		/*
		 * 检查子句是否具有"outer op inner"或"inner op outer"的形式
		 * 确保子句的两边分别对应外层和内层关系
		 */
		if (!clause_sides_match_join(restrictinfo, outerrel, innerrel))
			continue;  // 子句形式不适合这些输入关系

		// 添加到哈希子句列表
		hashclauses = lappend(hashclauses, restrictinfo);
	}

	/* 如果找到任何可用的哈希子句，则生成连接路径 */
	if (hashclauses) {
		/*
		 * 我们考虑两种外层路径：总成本最低和启动成本最低的路径
		 * 然而，只需要考虑总成本最低的内层路径
		 */
		Path* cheapest_startup_outer = outerrel->cheapest_startup_path;  // 外层启动成本最低路径
		Path* cheapest_total_outer = outerrel->cheapest_total_path;      // 外层总成本最低路径
		Path* cheapest_total_inner = innerrel->cheapest_total_path;      // 内层总成本最低路径

		/*
		 * 如果任一总成本最低的路径被另一关系参数化，则无法使用哈希连接
		 * （无需寻找替代输入路径，因为这些应该已经是可用的参数化程度最低的路径）
		 */
		if (PATH_PARAM_BY_REL(cheapest_total_outer, innerrel) ||
			PATH_PARAM_BY_REL(cheapest_total_inner, outerrel))
			return;  // 无法使用哈希连接

		/* 处理需要去重的情况；我们忽略参数化的可能性 */
		if (jointype == JOIN_UNIQUE_OUTER)  // 处理外层唯一连接
		{
			// 为外层路径创建唯一路径
			cheapest_total_outer = (Path*)
				create_unique_path(root, outerrel,
					cheapest_total_outer, extra->sjinfo);
			Assert(cheapest_total_outer);  // 确保创建成功

			jointype = JOIN_INNER;  // 转换为内连接类型
			// 尝试创建哈希连接路径
			try_hashjoin_path(root,
				joinrel,
				cheapest_total_outer,
				cheapest_total_inner,
				hashclauses,
				jointype,
				extra);
			/* 这里不可能有低启动成本的路径，因为需要去重 */
		}
		else if (jointype == JOIN_UNIQUE_INNER)  // 处理内层唯一连接
		{
			// 为内层路径创建唯一路径
			cheapest_total_inner = (Path*)
				create_unique_path(root, innerrel,
					cheapest_total_inner, extra->sjinfo);
			Assert(cheapest_total_inner);  // 确保创建成功

			jointype = JOIN_INNER;  // 转换为内连接类型
			// 尝试创建哈希连接路径
			try_hashjoin_path(root,
				joinrel,
				cheapest_total_outer,
				cheapest_total_inner,
				hashclauses,
				jointype,
				extra);
			// 如果外层有单独的启动成本最低路径，也尝试使用它
			if (cheapest_startup_outer != NULL &&
				cheapest_startup_outer != cheapest_total_outer)
				try_hashjoin_path(root,
					joinrel,
					cheapest_startup_outer,
					cheapest_total_inner,
					hashclauses,
					jointype,
					extra);
		}
		else  // 处理其他连接类型
		{
			/*
			 * 对于其他连接类型，我们考虑启动成本最低的外层与总成本最低的内层的组合
			 * 然后考虑总成本最低的路径配对，包括参数化的路径
			 * 无需基于可能的低启动成本生成参数化路径，所以这种方式就足够了
			 */
			ListCell* lc1;  // 外层参数化路径遍历指针
			ListCell* lc2;  // 内层参数化路径遍历指针

			// 尝试启动成本最低的外层与总成本最低的内层组合
			if (cheapest_startup_outer != NULL)
				try_hashjoin_path(root,
					joinrel,
					cheapest_startup_outer,
					cheapest_total_inner,
					hashclauses,
					jointype,
					extra);

			// 遍历外层所有参数化的低成本路径
			foreach(lc1, outerrel->cheapest_parameterized_paths)
			{
				Path* outerpath = (Path*)lfirst(lc1);

				/*
				 * 我们不能使用被内层关系参数化的外层路径
				 * （避免循环依赖）
				 */
				if (PATH_PARAM_BY_REL(outerpath, innerrel))
					continue;

				// 遍历内层所有参数化的低成本路径
				foreach(lc2, innerrel->cheapest_parameterized_paths)
				{
					Path* innerpath = (Path*)lfirst(lc2);

					/*
					 * 我们也不能使用被外层关系参数化的内层路径
					 * （避免循环依赖）
					 */
					if (PATH_PARAM_BY_REL(innerpath, outerrel))
						continue;

					// 跳过已经尝试过的组合
					if (outerpath == cheapest_startup_outer &&
						innerpath == cheapest_total_inner)
						continue;  /* 已经尝试过这个组合 */

					// 尝试创建哈希连接路径
					try_hashjoin_path(root,
						joinrel,
						outerpath,
						innerpath,
						hashclauses,
						jointype,
						extra);
				}
			}
		}

		/*
		 * 如果连接关系支持并行执行，我们可能可以考虑部分哈希连接
		 * 但是，我们不能处理JOIN_UNIQUE_OUTER，因为外层路径将是部分的，
		 * 因此我们无法正确保证唯一性。类似地，我们不能处理JOIN_FULL和JOIN_RIGHT，
		 * 因为它们可能产生错误的空值扩展行。此外，结果路径不能是参数化的。
		 * 对于Parallel Hash，我们可以支持JOIN_FULL和JOIN_RIGHT，因为在这种情况下，
		 * 我们回到每个批次使用单个哈希表和单个匹配位集的情况，但这需要找出一种
		 * 无死锁的方式来等待探测完成。
		 */
		if (joinrel->consider_parallel &&  // 连接关系支持并行
			save_jointype != JOIN_UNIQUE_OUTER &&  // 非外层唯一连接
			save_jointype != JOIN_FULL &&  // 非全连接
			save_jointype != JOIN_RIGHT &&  // 非右连接
			outerrel->partial_pathlist != NIL &&  // 外层有部分路径
			bms_is_empty(joinrel->lateral_relids))  // 无横向依赖
		{
			Path* cheapest_partial_outer;  // 外层成本最低的部分路径
			Path* cheapest_partial_inner = NULL;  // 内层成本最低的部分路径
			Path* cheapest_safe_inner = NULL;  // 并行安全的内层路径

			// 获取外层成本最低的部分路径
			cheapest_partial_outer = (Path*)linitial(outerrel->partial_pathlist);

			/*
			 * 我们是否也可以使用部分内层计划，以便并行构建共享哈希表？
			 * 我们不能处理JOIN_UNIQUE_INNER，因为无法保证唯一性
			 */
			if (innerrel->partial_pathlist != NIL &&  // 内层有部分路径
				save_jointype != JOIN_UNIQUE_INNER &&  // 非内层唯一连接
				enable_parallel_hash)  // 启用了并行哈希
			{
				// 获取内层成本最低的部分路径
				cheapest_partial_inner = (Path*)linitial(innerrel->partial_pathlist);
				// 尝试创建并行哈希连接路径
				try_partial_hashjoin_path(root, joinrel,
					cheapest_partial_outer,
					cheapest_partial_inner,
					hashclauses, jointype, extra,
					true /* parallel_hash */);  // 启用并行哈希
			}

			/*
			 * 通常，考虑到连接关系是并行安全的，总成本最低的内层路径也应该是并行安全的，
			 * 但如果不是，我们将需要搜索成本最低的安全、未参数化的内层路径
			 * 如果是JOIN_UNIQUE_INNER，我们不能使用任何替代的内层路径
			 */
			if (cheapest_total_inner->parallel_safe)  // 如果内层总成本最低路径已经并行安全
				cheapest_safe_inner = cheapest_total_inner;
			else if (save_jointype != JOIN_UNIQUE_INNER)  // 否则寻找替代方案
				cheapest_safe_inner = get_cheapest_parallel_safe_total_inner(innerrel->pathlist);

			// 如果找到了并行安全的内层路径，创建部分哈希连接路径
			if (cheapest_safe_inner != NULL)
				try_partial_hashjoin_path(root, joinrel,
					cheapest_partial_outer,
					cheapest_safe_inner,
					hashclauses, jointype, extra,
					false /* parallel_hash */);  // 不启用并行哈希
		}
	}
}

/*
 * select_mergejoin_clauses
 *	  测试给定的基表以及约束语句是否可以构成 MergeJoin 子句。
 *	  选择可用于特定连接的合并连接子句，返回这些子句的 RestrictInfo 节点列表。
 *
 * *mergejoin_allowed 通常设置为 true，但如果是右连接或全连接且存在不可合并连接的连接子句，则设置为 false。
 * 执行器的合并连接机制无法处理这种情况，因此必须避免生成合并连接计划。
 * （注意该标志并不考虑是否实际有可合并连接的子句，这是正确的，因为某些情况下需要生成无连接条件的合并连接。）
 *
 * 同时会标记每个选中的 RestrictInfo，表示当前哪个关系作为外部关系。这些标记仅在本次 add_paths_to_joinrel() 调用期间有效。
 *
 * 检查每个 restrictinfo 子句，判断其是否可用于合并连接且涉及当前关注的两个子关系的变量。
 */
static List *
select_mergejoin_clauses(PlannerInfo *root,
						 RelOptInfo *joinrel,
						 RelOptInfo *outerrel,
						 RelOptInfo *innerrel,
						 List *restrictlist,
						 JoinType jointype,
						 bool *mergejoin_allowed)
{
	List	   *result_list = NIL;
	bool		isouterjoin = IS_OUTER_JOIN(jointype);
	bool		have_nonmergeable_joinclause = false;
	ListCell   *l;

	foreach(l, restrictlist)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(l);

		/*
		 * 如果处理的是外连接，只能使用自身的连接子句进行合并连接。
		 * 对于内连接，可以使用下推的子句。（注意：下推子句不会设置 have_nonmergeable_joinclause，因为它们会作为其他条件处理。）
		 * is_pushed_down 是区分过滤条件和连接条件的标志。
		 */
		if (isouterjoin && RINFO_IS_PUSHED_DOWN(restrictinfo, joinrel->relids))
			continue;

		/*
		 * 检查子句是否为可合并的操作符子句
		 * can_join 是在 distribute_qual_to_rels() -> make_restrictinfo -> make_restrictinfo_internal 中设置的，
		 * 表示该子句是否可用于连接。
		 */
		if (!restrictinfo->can_join ||
			restrictinfo->mergeopfamilies == NIL)
		{
			/*
			 * 执行器在做右连接或全连接时，只能处理常量类型的额外连接条件（如 FULL JOIN ON FALSE），其他类型无法处理。
			 * 不设置 have_nonmergeable_joinclause 为 true。
			 */
			if (!restrictinfo->clause || !IsA(restrictinfo->clause, Const))
				have_nonmergeable_joinclause = true;
			continue;			/* 不可用于合并连接 */
		}

		/*
		 * 检查子句是否为“inner op outer”或“outer op inner”形式。
		 */
		if (!clause_sides_match_join(restrictinfo, outerrel, innerrel))
		{
			have_nonmergeable_joinclause = true;
			continue;			/* 不适用于当前输入关系 */
		}

		/*
		 * 要求每一侧都有非冗余的等价类（eclass）。
		 * 这是因为规划器的其他部分期望每个合并连接子句都能关联到规范的 pathkey 列表中的某个 pathkey，
		 * 而冗余的 eclass 不会出现在规范的排序顺序中。
		 */
		update_mergeclause_eclasses(root, restrictinfo);

		if (EC_MUST_BE_REDUNDANT(restrictinfo->left_ec) ||
			EC_MUST_BE_REDUNDANT(restrictinfo->right_ec))
		{
			have_nonmergeable_joinclause = true;
			continue;			/* 无法处理冗余的等价类 */
		}

		result_list = lappend(result_list, restrictinfo);
	}

	/*
	 * 返回是否允许合并连接（见函数顶部注释）。
	 */
	switch (jointype)
	{
		case JOIN_RIGHT:
		case JOIN_FULL:
			*mergejoin_allowed = !have_nonmergeable_joinclause;
			break;
		default:
			*mergejoin_allowed = true;
			break;
	}

	return result_list;
}
