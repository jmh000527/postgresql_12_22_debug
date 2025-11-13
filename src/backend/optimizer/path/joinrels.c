/*-------------------------------------------------------------------------
 *
 * joinrels.c
 *	  Routines to determine which relations should be joined
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/joinrels.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "optimizer/appendinfo.h"
#include "optimizer/joininfo.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "partitioning/partbounds.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"


static void make_rels_by_clause_joins(PlannerInfo *root,
									  RelOptInfo *old_rel,
									  ListCell *other_rels);
static void make_rels_by_clauseless_joins(PlannerInfo *root,
										  RelOptInfo *old_rel,
										  ListCell *other_rels);
static bool has_join_restriction(PlannerInfo *root, RelOptInfo *rel);
static bool has_legal_joinclause(PlannerInfo *root, RelOptInfo *rel);
static bool restriction_is_constant_false(List *restrictlist,
										  RelOptInfo *joinrel,
										  bool only_pushed_down);
static void populate_joinrel_with_paths(PlannerInfo *root, RelOptInfo *rel1,
										RelOptInfo *rel2, RelOptInfo *joinrel,
										SpecialJoinInfo *sjinfo, List *restrictlist);
static void try_partitionwise_join(PlannerInfo *root, RelOptInfo *rel1,
								   RelOptInfo *rel2, RelOptInfo *joinrel,
								   SpecialJoinInfo *parent_sjinfo,
								   List *parent_restrictlist);
static SpecialJoinInfo *build_child_join_sjinfo(PlannerInfo *root,
												SpecialJoinInfo *parent_sjinfo,
												Relids left_relids, Relids right_relids);
static int	match_expr_to_partition_keys(Expr *expr, RelOptInfo *rel,
										 bool strict_op);


/*
 * join_search_one_level
 *	  考虑生成包含恰好 'level' 个 jointree 项的连接关系的方法。
 *	  （这是标准动态规划方法 standard_join_search 的一步。）
 *	  为每个可行的低层关系组合创建并返回 join rel 节点列表，同时也为每个 joinrel 创建实现路径。
 *
 * level: 本次要生成的关系层级
 * root->join_rel_level[j], 1 <= j < level, 是包含 j 个项的关系列表
 *
 * 结果返回在 root->join_rel_level[level] 中。
 */
void
join_search_one_level(PlannerInfo *root, int level)
{
	List	  **joinrels = root->join_rel_level; /* 所有基表的链表 */
	ListCell   *r;
	int			k;

	Assert(joinrels[level] == NIL);

	/* 设置 join_cur_level，使新 joinrel 加入正确的列表 */
	root->join_cur_level = level;

	/*
	 * 首先，考虑左连接和右连接方案，即将恰好包含 level-1 个成员关系的 rel 与初始关系连接。
	 * 优先使用连接条件进行连接，但如果 level-1 的 rel 没有连接条件，则会与所有未包含的初始关系做笛卡尔积连接。
	 *
	 * 对当前层的上一层进行遍历，也就是说如果要生成 level 层的 RelOptInfo，需要遍历 level -1 层的 RelOptInfo 和第一层的基表尝试连接
	 */
	foreach(r, joinrels[level - 1])
	{
		RelOptInfo *old_rel = (RelOptInfo *) lfirst(r);

		if (old_rel->joininfo != NIL || old_rel->has_eclass_joins ||
			has_join_restriction(root, old_rel))
		{
			/*
			 * 该关系有连接条件或连接顺序限制，优先给这两个 RelOptInfo 生成连接。
			 *
			 * 在 level 2 时该条件是对称的，无需考虑列表中该关系之前的初始关系；
			 * 这些连接在之前的层级已考虑（镜像连接由 make_join_rel 自动处理）。
			 * 在更高层级（level > 2）时，将前一层级的关系与所有未包含但有连接条件或限制的初始关系连接。
			 *
			 * 要生成第 N 层的 RelOptInfo，就需要第 N - 1 层的 RelOptInfo 和第一层的基表集合进行连接
			 * 如果要生成第二层的连接树子集，那么就变成第一层的基表集和第一层的基表集合进行连接
			 * 需要对第二层进行单独处理，防止自己和自己连接
			 */
			ListCell   *other_rels;

			if (level == 2)		/* 只考虑剩余初始关系 */
				other_rels = lnext(r);
			else				/* 考虑所有初始关系 */
				other_rels = list_head(joinrels[1]);

			make_rels_by_clause_joins(root,
									  old_rel,
									  other_rels);
		}
		else
		{
			/*
			 * 没有与其他关系直接或通过连接顺序限制连接的关系，只能做笛卡尔积。
			 *
			 * 与每个未包含的初始关系做笛卡尔积，无论其是否有其他连接条件。
			 * 在 level 2 时，若有两个以上无条件初始关系，会重复考虑它们的连接顺序；
			 * 但这种情况不常见，无需为避免重复增加复杂度。
			 */
			make_rels_by_clauseless_joins(root,
										  old_rel,
										  list_head(joinrels[1]));
		}
	}

	/*
	 * 接下来，考虑“浓密树计划”，即将 k 个初始关系与 level-k 个初始关系连接，2 <= k <= level-2。
	 *
	 * 仅对有合适连接条件（或连接顺序限制）的关系对考虑灌木型连接，以避免规划时间过长。
	 */
	for (k = 2;; k++)
	{
		int			other_level = level - k;

		/*
		 * 由于 make_join_rel(x, y) 会处理 x,y 和 y,x 两种情况，只需遍历到一半即可。
		 */
		if (k > other_level)
			break;

		foreach(r, joinrels[k])
		{
			RelOptInfo *old_rel = (RelOptInfo *) lfirst(r);
			ListCell   *other_rels;
			ListCell   *r2;

			/*
			 * 没有连接条件的关系可忽略，除非参与了连接顺序限制——此时可能需要强制灌木型连接。
			 */
			if (old_rel->joininfo == NIL && !old_rel->has_eclass_joins &&
				!has_join_restriction(root, old_rel))
				continue;

			if (k == other_level)
				other_rels = lnext(r);	/* 只考虑剩余关系 */
			else
				other_rels = list_head(joinrels[other_level]);

			for_each_cell(r2, other_rels)
			{
				RelOptInfo *new_rel = (RelOptInfo *) lfirst(r2);

				if (!bms_overlap(old_rel->relids, new_rel->relids))
				{
					/*
					 * 可以用该关系对构建目标层级的关系。
					 * 若有相关连接条件或连接顺序限制，则进行连接。
					 */
					if (have_relevant_joinclause(root, old_rel, new_rel) ||
						have_join_order_restriction(root, old_rel, new_rel))
					{
						(void) make_join_rel(root, old_rel, new_rel);
					}
				}
			}
		}
	}

	/*----------
	 * 最后尝试：如果之前未找到可用连接，则强制生成一组笛卡尔积连接。
	 * 处理所有可用关系都有连接条件但暂时无法使用的特殊情况。
	 * 这种情况只会在处理连接子问题（子连接列表）且所有子问题关系仅与外部关系有连接条件时发生。
	 * 例如：
	 *
	 *		SELECT ... FROM a INNER JOIN b ON TRUE, c, d, ...
	 *		WHERE a.w = c.x and b.y = d.z;
	 *
	 * 若 "a INNER JOIN b" 子问题未被上层合并，必须允许 a 和 b 做笛卡尔连接；
	 * 但上述代码不会这样做，因为认为 a 和 b 都有连接条件。
	 * 此时只考虑左深树和右深树（不考虑浓密树）。
	 *----------
	 */
	if (joinrels[level] == NIL)
	{
		/*
		 * 此循环与第一个类似，只是始终调用 make_rels_by_clauseless_joins()。
		 */
		foreach(r, joinrels[level - 1])
		{
			RelOptInfo *old_rel = (RelOptInfo *) lfirst(r);

			make_rels_by_clauseless_joins(root,
										  old_rel,
										  list_head(joinrels[1]));
		}

		/*----------
		 * 若涉及特殊连接，某些 N 路连接可能无法合法生成。例如：
		 *
		 * SELECT ... FROM t1 WHERE
		 *	 x IN (SELECT ... FROM t2,t3 WHERE ...) AND
		 *	 y IN (SELECT ... FROM t4,t5 WHERE ...)
		 *
		 * 会被展开为 5 路连接，但没有任何 4 路连接是合法的。
		 * 必须允许在 level 4 失败，继续在 level 5 寻找可行灌木型计划。
		 *
		 * 但若无特殊连接且无 lateral 引用，则 join_is_legal() 不应失败，
		 * 因此下面的健壮性检查是有意义的。
		 *----------
		 */
		if (joinrels[level] == NIL &&
			root->join_info_list == NIL &&
			!root->hasLateralRTEs)
			elog(ERROR, "failed to build any %d-way joins", level);
	}
}

/*
 * make_rels_by_clause_joins
 *	  构建给定关系 'old_rel' 与其他参与连接条件的关系之间的连接，
 *	  这些其他关系要么与 'old_rel' 参与相同的连接条件，要么与其有连接顺序限制。
 *	  生成的连接关系会被加入到 root->join_rel_level[join_cur_level]。
 *
 * 注意：在 level > 2 时，会以多种方式生成相同的连接关系——例如 (a join b) join c
 * 与 (b join c) join a 是同一个 RelOptInfo，但第二种方式会为其添加不同的路径集合。
 * 这也是使用 join_rel_level 机制的原因，它能确保每个新 joinrel 只被加入一次。
 *
 * 'old_rel'：要参与连接的关系条目
 * 'other_rels'：链表中的第一个节点，包含要考虑连接的其他关系
 *
 * 当前仅用于与初始关系进行连接，但也可用于与 joinrels 连接。
 */
static void
make_rels_by_clause_joins(PlannerInfo *root,
						  RelOptInfo *old_rel,
						  ListCell *other_rels)
{
	/*
	 * 遍历列表 'other_rels'，尝试将 'old_rel' 与每个 'other_rel' 进行连接。
	 * 对于每个 'other_rel'，检查其 relids 是否与 'old_rel' 不重叠，并且两者之间是否存在
	 * 相关的连接条件或连接顺序限制。如果满足条件，则调用 make_join_rel() 尝试创建连接关系。
	 */
	ListCell   *l;

	for_each_cell(l, other_rels)
	{
		RelOptInfo *other_rel = (RelOptInfo *) lfirst(l);

		if (!bms_overlap(old_rel->relids, other_rel->relids) &&
			(have_relevant_joinclause(root, old_rel, other_rel) ||
			 have_join_order_restriction(root, old_rel, other_rel)))
		{
			(void) make_join_rel(root, old_rel, other_rel);
		}
	}
}

/*
 * make_rels_by_clauseless_joins
 *	  给定关系 'old_rel' 和其他关系列表 'other_rels'，
 *	  为 'old_rel' 与 'other_rels' 中尚未包含在 'old_rel' 的每个成员创建连接关系。
 *	  生成的连接关系会被加入到 root->join_rel_level[join_cur_level]。
 *
 * 'old_rel'：要参与连接的关系条目
 * 'other_rels'：链表中的第一个节点，包含要考虑连接的其他关系
 *
 * 当前仅用于与初始关系进行连接，但也可用于与 joinrels 连接。
 */
static void
make_rels_by_clauseless_joins(PlannerInfo *root,
							  RelOptInfo *old_rel,
							  ListCell *other_rels)
{
	ListCell   *l;

	for_each_cell(l, other_rels)
	{
		RelOptInfo *other_rel = (RelOptInfo *) lfirst(l);

		if (!bms_overlap(other_rel->relids, old_rel->relids))
		{
			(void) make_join_rel(root, old_rel, other_rel);
		}
	}
}


/*
 * join_is_legal
 *	   判断一个建议的连接在查询的连接顺序约束下是否合法；如果合法，则确定连接类型。
 *
 * 调用者必须提供两个关系以及它们 relids 的并集。
 * （我们可以在本地计算 joinrelids 来简化 API，但在 make_join_rel 的正常路径下这样做会重复工作。）
 *
 * 成功时，*sjinfo_p 被设置为 NULL 表示普通内连接，否则指向相关的 SpecialJoinInfo 节点。
 * 同时，*reversed_p 被设置为 true 表示需要交换两个关系以匹配 SpecialJoinInfo 节点。
 */
static bool
join_is_legal(PlannerInfo *root, RelOptInfo *rel1, RelOptInfo *rel2,
			  Relids joinrelids,
			  SpecialJoinInfo **sjinfo_p, bool *reversed_p)
{
	SpecialJoinInfo *match_sjinfo;
	bool		reversed;
	bool		unique_ified;
	bool		must_be_leftjoin;
	ListCell   *l;

	/*
	 * 确保失败返回时输出参数已设置。这样做只是为了让过于严格的编译器不警告未初始化变量。
	 */
	*sjinfo_p = NULL;
	*reversed_p = false;

	/*
	 * 如果有特殊连接，建议的连接可能不合法；无论如何都要确定连接类型。
	 * 扫描连接信息列表以查找匹配项和冲突项。
	 */
	match_sjinfo = NULL;
	reversed = false;
	unique_ified = false;
	must_be_leftjoin = false;

	foreach(l, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(l);

		/*
		 * 如果该特殊连接的 RHS 与建议连接无重叠，则不相关。
		 * （优先检查此项以快速跳过大多数无关的 SJ。）
		 */
		if (!bms_overlap(sjinfo->min_righthand, joinrelids))
			continue;

		/*
		 * 如果建议连接完全包含在 RHS 内（即我们还在构建 RHS），则也不相关。
		 */
		if (bms_is_subset(joinrelids, sjinfo->min_righthand))
			continue;

		/*
		 * 如果 SJ 已经在任一输入中完成，则也不相关。
		 */
		if (bms_is_subset(sjinfo->min_lefthand, rel1->relids) &&
			bms_is_subset(sjinfo->min_righthand, rel1->relids))
			continue;
		if (bms_is_subset(sjinfo->min_lefthand, rel2->relids) &&
			bms_is_subset(sjinfo->min_righthand, rel2->relids))
			continue;

		/*
		 * 如果是半连接且 RHS 已在任一输入中与其他关系连接，则此时必须已唯一化 RHS，
		 * 因此该半连接在此连接路径中不再相关。
		 */
		if (sjinfo->jointype == JOIN_SEMI)
		{
			if (bms_is_subset(sjinfo->syn_righthand, rel1->relids) &&
				!bms_equal(sjinfo->syn_righthand, rel1->relids))
				continue;
			if (bms_is_subset(sjinfo->syn_righthand, rel2->relids) &&
				!bms_equal(sjinfo->syn_righthand, rel2->relids))
				continue;
		}

		/*
		 * 如果一个输入包含 min_lefthand，另一个包含 min_righthand，则可以在此连接执行 SJ。
		 *
		 * 如果匹配到多个 SJ，则拒绝，因为这意味着正在考虑不真正有效的连接。
		 */
		if (bms_is_subset(sjinfo->min_lefthand, rel1->relids) &&
			bms_is_subset(sjinfo->min_righthand, rel2->relids))
		{
			if (match_sjinfo)
				return false;	/* 非法连接路径 */
			match_sjinfo = sjinfo;
			reversed = false;
		}
		else if (bms_is_subset(sjinfo->min_lefthand, rel2->relids) &&
				 bms_is_subset(sjinfo->min_righthand, rel1->relids))
		{
			if (match_sjinfo)
				return false;	/* 非法连接路径 */
			match_sjinfo = sjinfo;
			reversed = true;
		}
		else if (sjinfo->jointype == JOIN_SEMI &&
				 bms_equal(sjinfo->syn_righthand, rel2->relids) &&
				 create_unique_path(root, rel2, rel2->cheapest_total_path,
									sjinfo) != NULL)
		{
			/*----------
			 * 对于半连接，可以通过唯一化 RHS（如果 RHS 可唯一化）将 RHS 与其他任何关系连接。
			 * 只有当我们拥有完整 RHS 但 LHS 少于 min_lefthand 时才会到这里。
			 *
			 * 这样做的原因举例：
			 *	SELECT ... FROM a,b WHERE (a.x,b.y) IN (SELECT c1,c2 FROM c)
			 * 如果坚持做半连接，必须先形成 A*B 的笛卡尔积。但如果唯一化 C，则半连接变为普通内连接，
			 * 可以任意顺序连接，例如先 C 与 A，再与 B。当 C 远小于 A 和 B 时，这样做效率极高。
			 * 所以允许 C 只与 A 或只与 B 连接，make_join_rel 需正确处理此情况。
			 *
			 * 实际上也允许唯一化后的 C 与其他关系 D 连接，这也是合法的，虽然通常不太合理，
			 * 此函数只关心合法性，不关心连接策略是否优良。
			 *----------
			 */
			if (match_sjinfo)
				return false;	/* 非法连接路径 */
			match_sjinfo = sjinfo;
			reversed = false;
			unique_ified = true;
		}
		else if (sjinfo->jointype == JOIN_SEMI &&
				 bms_equal(sjinfo->syn_righthand, rel1->relids) &&
				 create_unique_path(root, rel1, rel1->cheapest_total_path,
									sjinfo) != NULL)
		{
			/* 反向半连接情况 */
			if (match_sjinfo)
				return false;	/* 非法连接路径 */
			match_sjinfo = sjinfo;
			reversed = true;
			unique_ified = true;
		}
		else
		{
			/*
			 * 否则，建议连接与 RHS 重叠但不是该 SJ 的有效实现。
			 * 但不要急于拒绝：RHS 违规可能已在一个或两个输入关系中发生，
			 * 此时必须已允许某些 SJ 与该 SJ 交换顺序。如果需要执行此连接以完成 RHS 构建，
			 * 拒绝可能导致无法找到任何计划。（因为本文件其他启发式方法会推迟无条件连接，
			 * 可能直到执行其他有效交换的 SJ 后才考虑在 RHS 内做无条件连接。）
			 * 这归结为：如果两个输入都与 RHS 重叠，则允许连接——它们要么完全在 RHS 内，
			 * 要么表示之前允许与 RHS 外关系连接。
			 */
			if (bms_overlap(rel1->relids, sjinfo->min_righthand) &&
				bms_overlap(rel2->relids, sjinfo->min_righthand))
				continue;		/* 假定之前已有效违规 RHS */

			/*
			 * 建议连接仍可能合法，但仅当允许将其关联到该 SJ 的 RHS 时。
			 * 这意味着 SJ 必须是 LEFT 连接（不是 SEMI/ANTI，更不是 FULL），
			 * 且建议连接不能与 LHS 重叠。
			 */
			if (sjinfo->jointype != JOIN_LEFT ||
				bms_overlap(joinrelids, sjinfo->min_lefthand))
				return false;	/* 非法连接路径 */

			/*
			 * 要合法，建议连接必须是 LEFT 连接；否则无法关联到该 SJ 的 RHS。
			 * 但可能还未找到与建议连接匹配的 SpecialJoinInfo，因此暂时记下此要求。
			 */
			must_be_leftjoin = true;
		}
	}

	/*
	 * 如果违反了某个 SJ 的 RHS 且未匹配到 LEFT SJ，则建议连接无法关联到 SJ 的 RHS，失败。
	 *
	 * 同时，如果建议连接的谓词不是严格的，也要失败；本质上是在检查能否应用外连接恒等式 3，这是必要条件。
	 * （此检查可能与 make_outerjoininfo 的检查重复，但成本很低，故仍保留。）
	 */
	if (must_be_leftjoin &&
		(match_sjinfo == NULL ||
		 match_sjinfo->jointype != JOIN_LEFT ||
		 !match_sjinfo->lhs_strict))
		return false;			/* 非法连接路径 */

	/*
	 * 还需检查 LATERAL 引用带来的约束。
	 */
	if (root->hasLateralRTEs)
	{
		bool		lateral_fwd;
		bool		lateral_rev;
		Relids		join_lateral_rels;

		/*
		 * 建议的两个关系可能各自包含对另一方的 lateral 引用，此时连接不可能。
		 * 如果只有一个方向有 lateral 引用，则必须用 nestloop 且引用方为内表。
		 * 如果连接匹配到不能用 nestloop 实现的 SJ，则连接不可能。
		 *
		 * 如果 lateral 引用只是间接的，也应拒绝连接；引用链涉及的关系必须先连接。
		 *
		 * 还有一种可能导致无法构建有效计划的情况，即 have_dangerous_phv() 所述的实现限制。
		 */
		lateral_fwd = bms_overlap(rel1->relids, rel2->lateral_relids);
		lateral_rev = bms_overlap(rel2->relids, rel1->lateral_relids);
		if (lateral_fwd && lateral_rev)
			return false;		/* 两方向都有 lateral 引用 */
		if (lateral_fwd)
		{
			/* 必须用 rel1 为左表的 nestloop 实现 */
			if (match_sjinfo &&
				(reversed ||
				 unique_ified ||
				 match_sjinfo->jointype == JOIN_FULL))
				return false;	/* 不能用 nestloop 实现 */
			/* 检查 rel2 是否对 rel1 有直接引用 */
			if (!bms_overlap(rel1->relids, rel2->direct_lateral_relids))
				return false;	/* 只有间接引用，拒绝 */
			/* 检查是否有危险的 PHV */
			if (have_dangerous_phv(root, rel1->relids, rel2->lateral_relids))
				return false;	/* 可能无法处理所需 PHV */
		}
		else if (lateral_rev)
		{
			/* 必须用 rel2 为左表的 nestloop 实现 */
			if (match_sjinfo &&
				(!reversed ||
				 unique_ified ||
				 match_sjinfo->jointype == JOIN_FULL))
				return false;	/* 不能用 nestloop 实现 */
			/* 检查 rel1 是否对 rel2 有直接引用 */
			if (!bms_overlap(rel2->relids, rel1->direct_lateral_relids))
				return false;	/* 只有间接引用，拒绝 */
			/* 检查是否有危险的 PHV */
			if (have_dangerous_phv(root, rel2->relids, rel1->lateral_relids))
				return false;	/* 可能无法处理所需 PHV */
		}

		/*
		 * LATERAL 引用还可能在后续阶段带来问题：如果连接的最小参数化包含必须作为外连接内表的关系，
		 * 则永远无法用该连接构建完整查询。应拒绝此连接，不仅因为能节省工作量，
		 * 还因为如果不拒绝，启发式方法可能认为该连接合法，导致某些连接关系未被构建，最终无法找到任何计划。
		 * 不仅要考虑直接作为外连接内表的关系，还要考虑间接的，因此需搜索所有此类关系。
		 */
		join_lateral_rels = min_join_parameterization(root, joinrelids,
													  rel1, rel2);
		if (join_lateral_rels)
		{
			Relids		join_plus_rhs = bms_copy(joinrelids);
			bool		more;

			do
			{
				more = false;
				foreach(l, root->join_info_list)
				{
					SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(l);

					/* 忽略全连接——它们的顺序已预定 */
					if (sjinfo->jointype == JOIN_FULL)
						continue;

					if (bms_overlap(sjinfo->min_lefthand, join_plus_rhs) &&
						!bms_is_subset(sjinfo->min_righthand, join_plus_rhs))
					{
						join_plus_rhs = bms_add_members(join_plus_rhs,
														sjinfo->min_righthand);
						more = true;
					}
				}
			} while (more);
			if (bms_overlap(join_plus_rhs, join_lateral_rels))
				return false;	/* 无法与某些 RHS 关系连接 */
		}
	}

	/* 否则，连接合法 */
	*sjinfo_p = match_sjinfo;
	*reversed_p = reversed;
	return true;
}


/*
 * make_join_rel
 *	   查找或创建一个表示 rel1 和 rel2 连接的 RelOptInfo，并为以 rel1 和 rel2
 *	   为外表和内表创建的路径添加路径信息。
 *	   （该 join rel 可能已经包含由其他 rel 组合生成的路径，这些组合包含相同的基表集合。）
 *
 * 注意：如果尝试的连接不合法则返回 NULL。这在处理外连接或已转换为连接的 IN/EXISTS 子句时可能发生。
 */
RelOptInfo *
make_join_rel(PlannerInfo *root, RelOptInfo *rel1, RelOptInfo *rel2)
{
	Relids				joinrelids;
	SpecialJoinInfo*	sjinfo;
	bool				reversed;
	SpecialJoinInfo		sjinfo_data;
	RelOptInfo*			joinrel;
	List*				restrictlist;

	/* 不应尝试连接两个有重叠 relids 的关系集合。 */
	Assert(!bms_overlap(rel1->relids, rel2->relids));

	/* 构造标识 joinrel 的 Relids 集合。 */
	joinrelids = bms_union(rel1->relids, rel2->relids);

	/* 检查连接合法性并确定连接类型。 */
	if (!join_is_legal(root, rel1, rel2, joinrelids,
					   &sjinfo, &reversed))
	{
		/* 非法连接路径 */
		bms_free(joinrelids);
		return NULL;
	}

	/* 如有需要，交换 rel1 和 rel2 以匹配连接信息。 */
	if (reversed)
	{
		RelOptInfo *trel = rel1;

		rel1 = rel2;
		rel2 = trel;
	}

	/*
	 * 如果是普通的内连接，则在 join_info_list 中不会找到任何信息。
	 * 构造一个 SpecialJoinInfo，以便选择性估算函数能知道连接的内容。
	 */
	if (sjinfo == NULL)
	{
		sjinfo = &sjinfo_data;
		sjinfo->type = T_SpecialJoinInfo;
		sjinfo->min_lefthand = rel1->relids;
		sjinfo->min_righthand = rel2->relids;
		sjinfo->syn_lefthand = rel1->relids;
		sjinfo->syn_righthand = rel2->relids;
		sjinfo->jointype = JOIN_INNER;
		/* 其余字段无需设置有效值 */
		sjinfo->lhs_strict = false;
		sjinfo->delay_upper_joins = false;
		sjinfo->semi_can_btree = false;
		sjinfo->semi_can_hash = false;
		sjinfo->semi_operators = NIL;
		sjinfo->semi_rhs_exprs = NIL;
	}

	/*
	 * 查找或构建 join RelOptInfo，并计算与本次连接相关的 restrictlist。
	 */
	joinrel = build_join_rel(root, joinrelids, rel1, rel2, sjinfo,
							 &restrictlist);

	/*
	 * 如果已经证明该连接结果为空，则无需再为其考虑路径。
	 */
	if (is_dummy_rel(joinrel))
	{
		bms_free(joinrelids);
		return joinrel;
	}

	/* 为连接关系添加路径。 */
	populate_joinrel_with_paths(root, rel1, rel2, joinrel, sjinfo,
								restrictlist);

	bms_free(joinrelids);

	return joinrel;
}

/*
 * populate_joinrel_with_paths
 *	  为给定的连接关系 joinrel 添加路径，针对给定的一对连接关系 rel1 和 rel2。
 *	  SpecialJoinInfo 提供连接的详细信息，restrictlist 包含连接条件和适用于该对连接关系的其他条件。
 */
static void
populate_joinrel_with_paths(PlannerInfo *root, RelOptInfo *rel1,
							RelOptInfo *rel2, RelOptInfo *joinrel,
							SpecialJoinInfo *sjinfo, List *restrictlist)
{
	/*
	 * 针对每个连接类型，分别考虑以 rel1 和 rel2 为外表和内表的路径。
	 * 根据连接类型，如果外表或内表已被证明为空，则整个连接结果也为空，此时丢弃之前计算的路径并标记为 dummy。
	 * （这样做是因为多表连接只有某些构造路径才会显现 dummy 性。）
	 *
	 * 另外，如果连接条件恒为 FALSE，通常意味着可以跳过对某一侧或两侧的计算。
	 * 对于外连接，如果恒 FALSE 的条件是下推的，则整个连接结果为空；如果不是下推的，则内表无行可连接，可以将内表标记为 dummy。
	 *
	 * 这里只需考虑 join_info_list 中出现的连接类型，以及 JOIN_INNER。
	 */
	switch (sjinfo->jointype)
	{
		case JOIN_INNER:
			/* 内连接：任一输入为空或连接条件恒 FALSE，则结果为空 */
			if (is_dummy_rel(rel1) || is_dummy_rel(rel2) ||
				restriction_is_constant_false(restrictlist, joinrel, false))
			{
				mark_dummy_rel(joinrel);
				break;
			}
			/* 添加 rel1 为外表、rel2 为内表的路径 */
			add_paths_to_joinrel(root, joinrel, rel1, rel2,
								 JOIN_INNER, sjinfo,
								 restrictlist);
			/* 添加 rel2 为外表、rel1 为内表的路径 */
			add_paths_to_joinrel(root, joinrel, rel2, rel1,
								 JOIN_INNER, sjinfo,
								 restrictlist);
			break;
		case JOIN_LEFT:
			/* 左连接：外表为空或下推条件恒 FALSE，则结果为空 */
			if (is_dummy_rel(rel1) ||
				restriction_is_constant_false(restrictlist, joinrel, true))
			{
				mark_dummy_rel(joinrel);
				break;
			}
			/* 非下推条件恒 FALSE 且内表为右表，则内表可标记为 dummy */
			if (restriction_is_constant_false(restrictlist, joinrel, false) &&
				bms_is_subset(rel2->relids, sjinfo->syn_righthand))
				mark_dummy_rel(rel2);
			add_paths_to_joinrel(root, joinrel, rel1, rel2,
								 JOIN_LEFT, sjinfo,
								 restrictlist);
			add_paths_to_joinrel(root, joinrel, rel2, rel1,
								 JOIN_RIGHT, sjinfo,
								 restrictlist);
			break;
		case JOIN_FULL:
			/* 全连接：两侧都为空或下推条件恒 FALSE，则结果为空 */
			if ((is_dummy_rel(rel1) && is_dummy_rel(rel2)) ||
				restriction_is_constant_false(restrictlist, joinrel, true))
			{
				mark_dummy_rel(joinrel);
				break;
			}
			add_paths_to_joinrel(root, joinrel, rel1, rel2,
								 JOIN_FULL, sjinfo,
								 restrictlist);
			add_paths_to_joinrel(root, joinrel, rel2, rel1,
								 JOIN_FULL, sjinfo,
								 restrictlist);

			/*
			 * 如果连接条件既不可排序也不可哈希，则无法生成有效计划，报错。
			 * （全连接没有规划灵活性，无法通过其他输入关系成功。）
			 */
			if (joinrel->pathlist == NIL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("FULL JOIN 仅支持可排序或可哈希的连接条件")));
			break;
		case JOIN_SEMI:
			/*
			 * 可能是普通半连接，也可能 RHS 可唯一化后做普通连接（见 join_is_legal 注释）。
			 * 后者不能用 JOIN_SEMI 方式。
			 */
			if (bms_is_subset(sjinfo->min_lefthand, rel1->relids) &&
				bms_is_subset(sjinfo->min_righthand, rel2->relids))
			{
				if (is_dummy_rel(rel1) || is_dummy_rel(rel2) ||
					restriction_is_constant_false(restrictlist, joinrel, false))
				{
					mark_dummy_rel(joinrel);
					break;
				}
				add_paths_to_joinrel(root, joinrel, rel1, rel2,
									 JOIN_SEMI, sjinfo,
									 restrictlist);
			}

			/*
			 * 如果 RHS 可唯一化且输入关系正好是 RHS，则可以唯一化后做普通连接。
			 * （create_unique_path 检查可能与 join_is_legal 重复，但有缓存，故仍检查。）
			 */
			if (bms_equal(sjinfo->syn_righthand, rel2->relids) &&
				create_unique_path(root, rel2, rel2->cheapest_total_path,
								   sjinfo) != NULL)
			{
				if (is_dummy_rel(rel1) || is_dummy_rel(rel2) ||
					restriction_is_constant_false(restrictlist, joinrel, false))
				{
					mark_dummy_rel(joinrel);
					break;
				}
				add_paths_to_joinrel(root, joinrel, rel1, rel2,
									 JOIN_UNIQUE_INNER, sjinfo,
									 restrictlist);
				add_paths_to_joinrel(root, joinrel, rel2, rel1,
									 JOIN_UNIQUE_OUTER, sjinfo,
									 restrictlist);
			}
			break;
		case JOIN_ANTI:
			/* 反连接：外表为空或下推条件恒 FALSE，则结果为空 */
			if (is_dummy_rel(rel1) ||
				restriction_is_constant_false(restrictlist, joinrel, true))
			{
				mark_dummy_rel(joinrel);
				break;
			}
			/* 非下推条件恒 FALSE 且内表为右表，则内表可标记为 dummy */
			if (restriction_is_constant_false(restrictlist, joinrel, false) &&
				bms_is_subset(rel2->relids, sjinfo->syn_righthand))
				mark_dummy_rel(rel2);
			add_paths_to_joinrel(root, joinrel, rel1, rel2,
								 JOIN_ANTI, sjinfo,
								 restrictlist);
			break;
		default:
			/* 其它类型不应出现，报错 */
			elog(ERROR, "未识别的连接类型: %d", (int) sjinfo->jointype);
			break;
	}

	/* 如果可以，尝试分区连接优化。 */
	try_partitionwise_join(root, rel1, rel2, joinrel, sjinfo, restrictlist);
}


/*
 * have_join_order_restriction
 *		检测两个关系是否需要连接以满足由特殊连接或 LATERAL 连接引起的连接顺序限制。
 *
 * 实际上，这个函数总是与 have_relevant_joinclause() 一起使用，因此可以合并，
 * 但分开处理更清晰。我们需要这个测试，因为存在一些退化情况，必须执行无条件连接以满足连接顺序限制。
 * 另外，如果一方有对另一方的 lateral 引用，或者两者都用于计算某个 PHV，也应该考虑连接它们，即使连接没有条件。
 *
 * 注意：只有当退化外连接的一侧包含多个关系，或者 IN/EXISTS 的 RHS 需要无条件连接时才会有这个问题；
 * 否则我们会通过 join_search_one_level() 的“最后一搏”分支找到连接路径。
 * 如果愿意在“最后一搏”分支尝试灌木型计划，则可以省略此测试，但效率较低。
 */
bool
have_join_order_restriction(PlannerInfo *root,
							RelOptInfo *rel1, RelOptInfo *rel2)
{
	bool		result = false;
	ListCell   *l;

	/*
	 * 如果任一方有对另一方的直接 lateral 引用，则无论外连接情况如何都尝试连接。
	 */
	if (bms_overlap(rel1->relids, rel2->direct_lateral_relids) ||
		bms_overlap(rel2->relids, rel1->direct_lateral_relids))
		return true;

	/*
	 * 同样，如果两个关系都用于计算某个 PlaceHolderVar，则无论外连接情况如何都尝试连接。
	 * （这不是很理想，因为 PHV 的 eval_at 集合很大时会导致很多无用连接被考虑，
	 * 但不这样做可能导致无法构造任何计划。）
	 */
	foreach(l, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(l);

		if (bms_is_subset(rel1->relids, phinfo->ph_eval_at) &&
			bms_is_subset(rel2->relids, phinfo->ph_eval_at))
			return true;
	}

	/*
	 * 关系可能对应于退化外连接的左右两侧，即没有连接条件涉及非可为空一侧，此时应强制连接。
	 *
	 * 另外，这两个关系可能表示必须完成的无条件连接，以构建外连接的 LHS 或 RHS。
	 */
	foreach(l, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(l);

		/* 忽略全连接——其它机制处理它们的顺序 */
		if (sjinfo->jointype == JOIN_FULL)
			continue;

		/* 这两个关系能否执行该特殊连接？ */
		if (bms_is_subset(sjinfo->min_lefthand, rel1->relids) &&
			bms_is_subset(sjinfo->min_righthand, rel2->relids))
		{
			result = true;
			break;
		}
		if (bms_is_subset(sjinfo->min_lefthand, rel2->relids) &&
			bms_is_subset(sjinfo->min_righthand, rel1->relids))
		{
			result = true;
			break;
		}

		/*
		 * 是否需要连接这两个关系以完成 RHS？必须用“重叠”测试，因为任一关系可能包含已被证明可交换的下层特殊连接。
		 */
		if (bms_overlap(sjinfo->min_righthand, rel1->relids) &&
			bms_overlap(sjinfo->min_righthand, rel2->relids))
		{
			result = true;
			break;
		}

		/* LHS 同理。 */
		if (bms_overlap(sjinfo->min_lefthand, rel1->relids) &&
			bms_overlap(sjinfo->min_lefthand, rel2->relids))
		{
			result = true;
			break;
		}
	}

	/*
	 * 如果任一输入关系可以通过连接条件合法地与其他关系连接，则不强制连接。
	 * 这意味着无条件浓密树连接会尽量延后。原因是当连接顺序限制在连接树较高处（即 LHS 或 RHS 内有很多关系）时，
	 * 否则会花费大量时间考虑非常愚蠢的连接组合。
	 */
	if (result)
	{
		if (has_legal_joinclause(root, rel1) ||
			has_legal_joinclause(root, rel2))
			result = false;
	}

	return result;
}


/*
 * has_join_restriction
 *		Detect whether the specified relation has join-order restrictions,
 *		due to being inside an outer join or an IN (sub-SELECT),
 *		or participating in any LATERAL references or multi-rel PHVs.
 *
 * Essentially, this tests whether have_join_order_restriction() could
 * succeed with this rel and some other one.  It's OK if we sometimes
 * say "true" incorrectly.  (Therefore, we don't bother with the relatively
 * expensive has_legal_joinclause test.)
 */
static bool
has_join_restriction(PlannerInfo *root, RelOptInfo *rel)
{
	ListCell   *l;

	if (rel->lateral_relids != NULL || rel->lateral_referencers != NULL)
		return true;

	foreach(l, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(l);

		if (bms_is_subset(rel->relids, phinfo->ph_eval_at) &&
			!bms_equal(rel->relids, phinfo->ph_eval_at))
			return true;
	}

	foreach(l, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(l);

		/* ignore full joins --- other mechanisms preserve their ordering */
		if (sjinfo->jointype == JOIN_FULL)
			continue;

		/* ignore if SJ is already contained in rel */
		if (bms_is_subset(sjinfo->min_lefthand, rel->relids) &&
			bms_is_subset(sjinfo->min_righthand, rel->relids))
			continue;

		/* restricted if it overlaps LHS or RHS, but doesn't contain SJ */
		if (bms_overlap(sjinfo->min_lefthand, rel->relids) ||
			bms_overlap(sjinfo->min_righthand, rel->relids))
			return true;
	}

	return false;
}


/*
 * has_legal_joinclause
 *		Detect whether the specified relation can legally be joined
 *		to any other rels using join clauses.
 *
 * We consider only joins to single other relations in the current
 * initial_rels list.  This is sufficient to get a "true" result in most real
 * queries, and an occasional erroneous "false" will only cost a bit more
 * planning time.  The reason for this limitation is that considering joins to
 * other joins would require proving that the other join rel can legally be
 * formed, which seems like too much trouble for something that's only a
 * heuristic to save planning time.  (Note: we must look at initial_rels
 * and not all of the query, since when we are planning a sub-joinlist we
 * may be forced to make clauseless joins within initial_rels even though
 * there are join clauses linking to other parts of the query.)
 */
static bool
has_legal_joinclause(PlannerInfo *root, RelOptInfo *rel)
{
	ListCell   *lc;

	foreach(lc, root->initial_rels)
	{
		RelOptInfo *rel2 = (RelOptInfo *) lfirst(lc);

		/* ignore rels that are already in "rel" */
		if (bms_overlap(rel->relids, rel2->relids))
			continue;

		if (have_relevant_joinclause(root, rel, rel2))
		{
			Relids		joinrelids;
			SpecialJoinInfo *sjinfo;
			bool		reversed;

			/* join_is_legal needs relids of the union */
			joinrelids = bms_union(rel->relids, rel2->relids);

			if (join_is_legal(root, rel, rel2, joinrelids,
							  &sjinfo, &reversed))
			{
				/* Yes, this will work */
				bms_free(joinrelids);
				return true;
			}

			bms_free(joinrelids);
		}
	}

	return false;
}


/*
 * There's a pitfall for creating parameterized nestloops: suppose the inner
 * rel (call it A) has a parameter that is a PlaceHolderVar, and that PHV's
 * minimum eval_at set includes the outer rel (B) and some third rel (C).
 * We might think we could create a B/A nestloop join that's parameterized by
 * C.  But we would end up with a plan in which the PHV's expression has to be
 * evaluated as a nestloop parameter at the B/A join; and the executor is only
 * set up to handle simple Vars as NestLoopParams.  Rather than add complexity
 * and overhead to the executor for such corner cases, it seems better to
 * forbid the join.  (Note that we can still make use of A's parameterized
 * path with pre-joined B+C as the outer rel.  have_join_order_restriction()
 * ensures that we will consider making such a join even if there are not
 * other reasons to do so.)
 *
 * So we check whether any PHVs used in the query could pose such a hazard.
 * We don't have any simple way of checking whether a risky PHV would actually
 * be used in the inner plan, and the case is so unusual that it doesn't seem
 * worth working very hard on it.
 *
 * This needs to be checked in two places.  If the inner rel's minimum
 * parameterization would trigger the restriction, then join_is_legal() should
 * reject the join altogether, because there will be no workable paths for it.
 * But joinpath.c has to check again for every proposed nestloop path, because
 * the inner path might have more than the minimum parameterization, causing
 * some PHV to be dangerous for it that otherwise wouldn't be.
 */
bool
have_dangerous_phv(PlannerInfo *root,
				   Relids outer_relids, Relids inner_params)
{
	ListCell   *lc;

	foreach(lc, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(lc);

		if (!bms_is_subset(phinfo->ph_eval_at, inner_params))
			continue;			/* ignore, could not be a nestloop param */
		if (!bms_overlap(phinfo->ph_eval_at, outer_relids))
			continue;			/* ignore, not relevant to this join */
		if (bms_is_subset(phinfo->ph_eval_at, outer_relids))
			continue;			/* safe, it can be eval'd within outerrel */
		/* Otherwise, it's potentially unsafe, so reject the join */
		return true;
	}

	/* OK to perform the join */
	return false;
}


/*
 * is_dummy_rel --- 该关系是否已被证明为空？
 */
bool
is_dummy_rel(RelOptInfo *rel)
{
	Path	   *path;

	/*
	 * 已知为空的关系通常只有一个没有子路径的 Append 路径。
	 * （即使有多个路径，没有子路径的 Append 路径代价为零，因此应在 pathlist 的最前面。）
	 */
	if (rel->pathlist == NIL)
		return false;
	path = (Path *) linitial(rel->pathlist);

	/*
	 * 最初，dummy 路径只是一个没有子路径的 Append。
	 * 但在后续规划阶段，可能会在其上加 ProjectSetPath 和/或 ProjectionPath，
	 * 因为 Append 不能做投影。与其假设可能出现哪些组合，不如直接递归向下查找。
	 */
	for (;;)
	{
		if (IsA(path, ProjectionPath))
			path = ((ProjectionPath *) path)->subpath;
		else if (IsA(path, ProjectSetPath))
			path = ((ProjectSetPath *) path)->subpath;
		else
			break;
	}
	if (IS_DUMMY_APPEND(path))
		return true;
	return false;
}

/*
 * Mark a relation as proven empty.
 *
 * During GEQO planning, this can get invoked more than once on the same
 * baserel struct, so it's worth checking to see if the rel is already marked
 * dummy.
 *
 * Also, when called during GEQO join planning, we are in a short-lived
 * memory context.  We must make sure that the dummy path attached to a
 * baserel survives the GEQO cycle, else the baserel is trashed for future
 * GEQO cycles.  On the other hand, when we are marking a joinrel during GEQO,
 * we don't want the dummy path to clutter the main planning context.  Upshot
 * is that the best solution is to explicitly make the dummy path in the same
 * context the given RelOptInfo is in.
 */
void
mark_dummy_rel(RelOptInfo *rel)
{
	MemoryContext oldcontext;

	/* Already marked? */
	if (is_dummy_rel(rel))
		return;

	/* No, so choose correct context to make the dummy path in */
	oldcontext = MemoryContextSwitchTo(GetMemoryChunkContext(rel));

	/* Set dummy size estimate */
	rel->rows = 0;

	/* Evict any previously chosen paths */
	rel->pathlist = NIL;
	rel->partial_pathlist = NIL;

	/* Set up the dummy path */
	add_path(rel, (Path *) create_append_path(NULL, rel, NIL, NIL,
											  NIL, rel->lateral_relids,
											  0, false, NIL, -1));

	/* Set or update cheapest_total_path and related fields */
	set_cheapest(rel);

	MemoryContextSwitchTo(oldcontext);
}


/*
 * restriction_is_constant_false --- is a restrictlist just FALSE?
 *
 * In cases where a qual is provably constant FALSE, eval_const_expressions
 * will generally have thrown away anything that's ANDed with it.  In outer
 * join situations this will leave us computing cartesian products only to
 * decide there's no match for an outer row, which is pretty stupid.  So,
 * we need to detect the case.
 *
 * If only_pushed_down is true, then consider only quals that are pushed-down
 * from the point of view of the joinrel.
 */
static bool
restriction_is_constant_false(List *restrictlist,
							  RelOptInfo *joinrel,
							  bool only_pushed_down)
{
	ListCell   *lc;

	/*
	 * Despite the above comment, the restriction list we see here might
	 * possibly have other members besides the FALSE constant, since other
	 * quals could get "pushed down" to the outer join level.  So we check
	 * each member of the list.
	 */
	foreach(lc, restrictlist)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		if (only_pushed_down && !RINFO_IS_PUSHED_DOWN(rinfo, joinrel->relids))
			continue;

		if (rinfo->clause && IsA(rinfo->clause, Const))
		{
			Const	   *con = (Const *) rinfo->clause;

			/* constant NULL is as good as constant FALSE for our purposes */
			if (con->constisnull)
				return true;
			if (!DatumGetBool(con->constvalue))
				return true;
		}
	}
	return false;
}

/*
 * Assess whether join between given two partitioned relations can be broken
 * down into joins between matching partitions; a technique called
 * "partitionwise join"
 *
 * Partitionwise join is possible when a. Joining relations have same
 * partitioning scheme b. There exists an equi-join between the partition keys
 * of the two relations.
 *
 * Partitionwise join is planned as follows (details: optimizer/README.)
 *
 * 1. Create the RelOptInfos for joins between matching partitions i.e
 * child-joins and add paths to them.
 *
 * 2. Construct Append or MergeAppend paths across the set of child joins.
 * This second phase is implemented by generate_partitionwise_join_paths().
 *
 * The RelOptInfo, SpecialJoinInfo and restrictlist for each child join are
 * obtained by translating the respective parent join structures.
 */
static void
try_partitionwise_join(PlannerInfo *root, RelOptInfo *rel1, RelOptInfo *rel2,
					   RelOptInfo *joinrel, SpecialJoinInfo *parent_sjinfo,
					   List *parent_restrictlist)
{
	bool		rel1_is_simple = IS_SIMPLE_REL(rel1);
	bool		rel2_is_simple = IS_SIMPLE_REL(rel2);
	int			nparts;
	int			cnt_parts;

	/* Guard against stack overflow due to overly deep partition hierarchy. */
	check_stack_depth();

	/* Nothing to do, if the join relation is not partitioned. */
	if (!IS_PARTITIONED_REL(joinrel))
		return;

	/* The join relation should have consider_partitionwise_join set. */
	Assert(joinrel->consider_partitionwise_join);

	/*
	 * Since this join relation is partitioned, all the base relations
	 * participating in this join must be partitioned and so are all the
	 * intermediate join relations.
	 */
	Assert(IS_PARTITIONED_REL(rel1) && IS_PARTITIONED_REL(rel2));
	Assert(REL_HAS_ALL_PART_PROPS(rel1) && REL_HAS_ALL_PART_PROPS(rel2));

	/* The joining relations should have consider_partitionwise_join set. */
	Assert(rel1->consider_partitionwise_join &&
		   rel2->consider_partitionwise_join);

	/*
	 * The partition scheme of the join relation should match that of the
	 * joining relations.
	 */
	Assert(joinrel->part_scheme == rel1->part_scheme &&
		   joinrel->part_scheme == rel2->part_scheme);

	/*
	 * Since we allow partitionwise join only when the partition bounds of the
	 * joining relations exactly match, the partition bounds of the join
	 * should match those of the joining relations.
	 */
	Assert(partition_bounds_equal(joinrel->part_scheme->partnatts,
								  joinrel->part_scheme->parttyplen,
								  joinrel->part_scheme->parttypbyval,
								  joinrel->boundinfo, rel1->boundinfo));
	Assert(partition_bounds_equal(joinrel->part_scheme->partnatts,
								  joinrel->part_scheme->parttyplen,
								  joinrel->part_scheme->parttypbyval,
								  joinrel->boundinfo, rel2->boundinfo));

	nparts = joinrel->nparts;

	/*
	 * Create child-join relations for this partitioned join, if those don't
	 * exist. Add paths to child-joins for a pair of child relations
	 * corresponding to the given pair of parent relations.
	 */
	for (cnt_parts = 0; cnt_parts < nparts; cnt_parts++)
	{
		RelOptInfo *child_rel1 = rel1->part_rels[cnt_parts];
		RelOptInfo *child_rel2 = rel2->part_rels[cnt_parts];
		bool		rel1_empty = (child_rel1 == NULL ||
								  IS_DUMMY_REL(child_rel1));
		bool		rel2_empty = (child_rel2 == NULL ||
								  IS_DUMMY_REL(child_rel2));
		SpecialJoinInfo *child_sjinfo;
		List	   *child_restrictlist;
		RelOptInfo *child_joinrel;
		Relids		child_joinrelids;
		AppendRelInfo **appinfos;
		int			nappinfos;

		/*
		 * Check for cases where we can prove that this segment of the join
		 * returns no rows, due to one or both inputs being empty (including
		 * inputs that have been pruned away entirely).  If so just ignore it.
		 * These rules are equivalent to populate_joinrel_with_paths's rules
		 * for dummy input relations.
		 */
		switch (parent_sjinfo->jointype)
		{
			case JOIN_INNER:
			case JOIN_SEMI:
				if (rel1_empty || rel2_empty)
					continue;	/* ignore this join segment */
				break;
			case JOIN_LEFT:
			case JOIN_ANTI:
				if (rel1_empty)
					continue;	/* ignore this join segment */
				break;
			case JOIN_FULL:
				if (rel1_empty && rel2_empty)
					continue;	/* ignore this join segment */
				break;
			default:
				/* other values not expected here */
				elog(ERROR, "unrecognized join type: %d",
					 (int) parent_sjinfo->jointype);
				break;
		}

		/*
		 * If a child has been pruned entirely then we can't generate paths
		 * for it, so we have to reject partitionwise joining unless we were
		 * able to eliminate this partition above.
		 */
		if (child_rel1 == NULL || child_rel2 == NULL)
		{
			/*
			 * Mark the joinrel as unpartitioned so that later functions treat
			 * it correctly.
			 */
			joinrel->nparts = 0;
			return;
		}

		/*
		 * If a leaf relation has consider_partitionwise_join=false, it means
		 * that it's a dummy relation for which we skipped setting up tlist
		 * expressions and adding EC members in set_append_rel_size(), so
		 * again we have to fail here.
		 */
		if (rel1_is_simple && !child_rel1->consider_partitionwise_join)
		{
			Assert(child_rel1->reloptkind == RELOPT_OTHER_MEMBER_REL);
			Assert(IS_DUMMY_REL(child_rel1));
			joinrel->nparts = 0;
			return;
		}
		if (rel2_is_simple && !child_rel2->consider_partitionwise_join)
		{
			Assert(child_rel2->reloptkind == RELOPT_OTHER_MEMBER_REL);
			Assert(IS_DUMMY_REL(child_rel2));
			joinrel->nparts = 0;
			return;
		}

		/* We should never try to join two overlapping sets of rels. */
		Assert(!bms_overlap(child_rel1->relids, child_rel2->relids));
		child_joinrelids = bms_union(child_rel1->relids, child_rel2->relids);
		appinfos = find_appinfos_by_relids(root, child_joinrelids, &nappinfos);

		/*
		 * Construct SpecialJoinInfo from parent join relations's
		 * SpecialJoinInfo.
		 */
		child_sjinfo = build_child_join_sjinfo(root, parent_sjinfo,
											   child_rel1->relids,
											   child_rel2->relids);

		/*
		 * Construct restrictions applicable to the child join from those
		 * applicable to the parent join.
		 */
		child_restrictlist =
			(List *) adjust_appendrel_attrs(root,
											(Node *) parent_restrictlist,
											nappinfos, appinfos);
		pfree(appinfos);

		child_joinrel = joinrel->part_rels[cnt_parts];
		if (!child_joinrel)
		{
			child_joinrel = build_child_join_rel(root, child_rel1, child_rel2,
												 joinrel, child_restrictlist,
												 child_sjinfo,
												 child_sjinfo->jointype);
			joinrel->part_rels[cnt_parts] = child_joinrel;
		}

		Assert(bms_equal(child_joinrel->relids, child_joinrelids));

		populate_joinrel_with_paths(root, child_rel1, child_rel2,
									child_joinrel, child_sjinfo,
									child_restrictlist);
	}
}

/*
 * Construct the SpecialJoinInfo for a child-join by translating
 * SpecialJoinInfo for the join between parents. left_relids and right_relids
 * are the relids of left and right side of the join respectively.
 */
static SpecialJoinInfo *
build_child_join_sjinfo(PlannerInfo *root, SpecialJoinInfo *parent_sjinfo,
						Relids left_relids, Relids right_relids)
{
	SpecialJoinInfo *sjinfo = makeNode(SpecialJoinInfo);
	AppendRelInfo **left_appinfos;
	int			left_nappinfos;
	AppendRelInfo **right_appinfos;
	int			right_nappinfos;

	memcpy(sjinfo, parent_sjinfo, sizeof(SpecialJoinInfo));
	left_appinfos = find_appinfos_by_relids(root, left_relids,
											&left_nappinfos);
	right_appinfos = find_appinfos_by_relids(root, right_relids,
											 &right_nappinfos);

	sjinfo->min_lefthand = adjust_child_relids(sjinfo->min_lefthand,
											   left_nappinfos, left_appinfos);
	sjinfo->min_righthand = adjust_child_relids(sjinfo->min_righthand,
												right_nappinfos,
												right_appinfos);
	sjinfo->syn_lefthand = adjust_child_relids(sjinfo->syn_lefthand,
											   left_nappinfos, left_appinfos);
	sjinfo->syn_righthand = adjust_child_relids(sjinfo->syn_righthand,
												right_nappinfos,
												right_appinfos);
	sjinfo->semi_rhs_exprs = (List *) adjust_appendrel_attrs(root,
															 (Node *) sjinfo->semi_rhs_exprs,
															 right_nappinfos,
															 right_appinfos);

	pfree(left_appinfos);
	pfree(right_appinfos);

	return sjinfo;
}

/*
 * Returns true if there exists an equi-join condition for each pair of
 * partition keys from given relations being joined.
 */
bool
have_partkey_equi_join(RelOptInfo *joinrel,
					   RelOptInfo *rel1, RelOptInfo *rel2,
					   JoinType jointype, List *restrictlist)
{
	PartitionScheme part_scheme = rel1->part_scheme;
	ListCell   *lc;
	int			cnt_pks;
	bool		pk_has_clause[PARTITION_MAX_KEYS];
	bool		strict_op;

	/*
	 * This function should be called when the joining relations have same
	 * partitioning scheme.
	 */
	Assert(rel1->part_scheme == rel2->part_scheme);
	Assert(part_scheme);

	memset(pk_has_clause, 0, sizeof(pk_has_clause));
	foreach(lc, restrictlist)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		OpExpr	   *opexpr;
		Expr	   *expr1;
		Expr	   *expr2;
		int			ipk1;
		int			ipk2;

		/* If processing an outer join, only use its own join clauses. */
		if (IS_OUTER_JOIN(jointype) &&
			RINFO_IS_PUSHED_DOWN(rinfo, joinrel->relids))
			continue;

		/* Skip clauses which can not be used for a join. */
		if (!rinfo->can_join)
			continue;

		/* Skip clauses which are not equality conditions. */
		if (!rinfo->mergeopfamilies && !OidIsValid(rinfo->hashjoinoperator))
			continue;

		opexpr = castNode(OpExpr, rinfo->clause);

		/*
		 * The equi-join between partition keys is strict if equi-join between
		 * at least one partition key is using a strict operator. See
		 * explanation about outer join reordering identity 3 in
		 * optimizer/README
		 */
		strict_op = op_strict(opexpr->opno);

		/* Match the operands to the relation. */
		if (bms_is_subset(rinfo->left_relids, rel1->relids) &&
			bms_is_subset(rinfo->right_relids, rel2->relids))
		{
			expr1 = linitial(opexpr->args);
			expr2 = lsecond(opexpr->args);
		}
		else if (bms_is_subset(rinfo->left_relids, rel2->relids) &&
				 bms_is_subset(rinfo->right_relids, rel1->relids))
		{
			expr1 = lsecond(opexpr->args);
			expr2 = linitial(opexpr->args);
		}
		else
			continue;

		/*
		 * Only clauses referencing the partition keys are useful for
		 * partitionwise join.
		 */
		ipk1 = match_expr_to_partition_keys(expr1, rel1, strict_op);
		if (ipk1 < 0)
			continue;
		ipk2 = match_expr_to_partition_keys(expr2, rel2, strict_op);
		if (ipk2 < 0)
			continue;

		/*
		 * If the clause refers to keys at different ordinal positions, it can
		 * not be used for partitionwise join.
		 */
		if (ipk1 != ipk2)
			continue;

		/* Reject if the partition key collation differs from the clause's. */
		if (rel1->part_scheme->partcollation[ipk1] != opexpr->inputcollid)
			return false;

		/*
		 * The clause allows partitionwise join if only it uses the same
		 * operator family as that specified by the partition key.
		 */
		if (rel1->part_scheme->strategy == PARTITION_STRATEGY_HASH)
		{
			if (!op_in_opfamily(rinfo->hashjoinoperator,
								part_scheme->partopfamily[ipk1]))
				continue;
		}
		else if (!list_member_oid(rinfo->mergeopfamilies,
								  part_scheme->partopfamily[ipk1]))
			continue;

		/* Mark the partition key as having an equi-join clause. */
		pk_has_clause[ipk1] = true;
	}

	/* Check whether every partition key has an equi-join condition. */
	for (cnt_pks = 0; cnt_pks < part_scheme->partnatts; cnt_pks++)
	{
		if (!pk_has_clause[cnt_pks])
			return false;
	}

	return true;
}

/*
 * Find the partition key from the given relation matching the given
 * expression. If found, return the index of the partition key, else return -1.
 */
static int
match_expr_to_partition_keys(Expr *expr, RelOptInfo *rel, bool strict_op)
{
	int			cnt;

	/* This function should be called only for partitioned relations. */
	Assert(rel->part_scheme);

	/* Remove any relabel decorations. */
	while (IsA(expr, RelabelType))
		expr = (Expr *) (castNode(RelabelType, expr))->arg;

	for (cnt = 0; cnt < rel->part_scheme->partnatts; cnt++)
	{
		ListCell   *lc;

		Assert(rel->partexprs);
		foreach(lc, rel->partexprs[cnt])
		{
			if (equal(lfirst(lc), expr))
				return cnt;
		}

		if (!strict_op)
			continue;

		/*
		 * If it's a strict equi-join a NULL partition key on one side will
		 * not join a NULL partition key on the other side. So, rows with NULL
		 * partition key from a partition on one side can not join with those
		 * from a non-matching partition on the other side. So, search the
		 * nullable partition keys as well.
		 */
		Assert(rel->nullable_partexprs);
		foreach(lc, rel->nullable_partexprs[cnt])
		{
			if (equal(lfirst(lc), expr))
				return cnt;
		}
	}

	return -1;
}
