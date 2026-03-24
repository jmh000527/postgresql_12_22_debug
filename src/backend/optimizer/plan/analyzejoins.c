/*-------------------------------------------------------------------------
 *
 * analyzejoins.c
 *	  Routines for simplifying joins after initial query analysis
 *
 * While we do a great deal of join simplification in prep/prepjointree.c,
 * certain optimizations cannot be performed at that stage for lack of
 * detailed information about the query.  The routines here are invoked
 * after initsplan.c has done its work, and can do additional join removal
 * and simplification steps based on the information extracted.  The penalty
 * is that we have to work harder to clean up after ourselves when we modify
 * the query, since the derived data structures have to be updated too.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/analyzejoins.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/joininfo.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/tlist.h"
#include "utils/lsyscache.h"

/* source-code-compatibility hacks for pull_varnos() API change */
#define pull_varnos(a,b) pull_varnos_new(a,b)

/* local functions */
static bool join_is_removable(PlannerInfo *root, SpecialJoinInfo *sjinfo);
static void remove_rel_from_query(PlannerInfo *root, int relid,
								  Relids joinrelids);
static List *remove_rel_from_joinlist(List *joinlist, int relid, int *nremoved);
static bool rel_supports_distinctness(PlannerInfo *root, RelOptInfo *rel);
static bool rel_is_distinct_for(PlannerInfo *root, RelOptInfo *rel,
								List *clause_list);
static Oid	distinct_col_search(int colno, List *colnos, List *opids);
static bool is_innerrel_unique_for(PlannerInfo *root,
								   Relids joinrelids,
								   Relids outerrelids,
								   RelOptInfo *innerrel,
								   JoinType jointype,
								   List *restrictlist);


/*
 * remove_useless_joins
 *		Check for relations that don't actually need to be joined at all,
 *		and remove them from the query.
 *
 * We are passed the current joinlist and return the updated list.  Other
 * data structures that have to be updated are accessible via "root".
 */
List *
remove_useless_joins(PlannerInfo *root, List *joinlist)
{
	ListCell   *lc;

	/*
	 * We are only interested in relations that are left-joined to, so we can
	 * scan the join_info_list to find them easily.
	 */
restart:
	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);
		int			innerrelid;
		int			nremoved;

		/* Skip if not removable */
		if (!join_is_removable(root, sjinfo))
			continue;

		/*
		 * Currently, join_is_removable can only succeed when the sjinfo's
		 * righthand is a single baserel.  Remove that rel from the query and
		 * joinlist.
		 */
		innerrelid = bms_singleton_member(sjinfo->min_righthand);

		remove_rel_from_query(root, innerrelid,
							  bms_union(sjinfo->min_lefthand,
										sjinfo->min_righthand));

		/* We verify that exactly one reference gets removed from joinlist */
		nremoved = 0;
		joinlist = remove_rel_from_joinlist(joinlist, innerrelid, &nremoved);
		if (nremoved != 1)
			elog(ERROR, "failed to find relation %d in joinlist", innerrelid);

		/*
		 * We can delete this SpecialJoinInfo from the list too, since it's no
		 * longer of interest.
		 */
		root->join_info_list = list_delete_ptr(root->join_info_list, sjinfo);

		/*
		 * Restart the scan.  This is necessary to ensure we find all
		 * removable joins independently of ordering of the join_info_list
		 * (note that removal of attr_needed bits may make a join appear
		 * removable that did not before).  Also, since we just deleted the
		 * current list cell, we'd have to have some kluge to continue the
		 * list scan anyway.
		 */
		goto restart;
	}

	return joinlist;
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
clause_sides_match_join(RestrictInfo *rinfo, Relids outerrelids,
						Relids innerrelids)
{
	if (bms_is_subset(rinfo->left_relids, outerrelids) &&
		bms_is_subset(rinfo->right_relids, innerrelids))
	{
		/* lefthand side is outer */
		rinfo->outer_is_left = true;
		return true;
	}
	else if (bms_is_subset(rinfo->left_relids, innerrelids) &&
			 bms_is_subset(rinfo->right_relids, outerrelids))
	{
		/* righthand side is outer */
		rinfo->outer_is_left = false;
		return true;
	}
	return false;				/* no good for these input relations */
}

/*
 * join_is_removable
 *	  Check whether we need not perform this special join at all, because
 *	  it will just duplicate its left input.
 *
 * This is true for a left join for which the join condition cannot match
 * more than one inner-side row.  (There are other possibly interesting
 * cases, but we don't have the infrastructure to prove them.)  We also
 * have to check that the inner side doesn't generate any variables needed
 * above the join.
 */
static bool
join_is_removable(PlannerInfo *root, SpecialJoinInfo *sjinfo)
{
	int			innerrelid;
	RelOptInfo *innerrel;
	Relids		joinrelids;
	List	   *clause_list = NIL;
	ListCell   *l;
	int			attroff;

	/*
	 * Must be a non-delaying left join to a single baserel, else we aren't
	 * going to be able to do anything with it.
	 */
	if (sjinfo->jointype != JOIN_LEFT ||
		sjinfo->delay_upper_joins)
		return false;

	if (!bms_get_singleton_member(sjinfo->min_righthand, &innerrelid))
		return false;

	innerrel = find_base_rel(root, innerrelid);

	/*
	 * Before we go to the effort of checking whether any innerrel variables
	 * are needed above the join, make a quick check to eliminate cases in
	 * which we will surely be unable to prove uniqueness of the innerrel.
	 */
	if (!rel_supports_distinctness(root, innerrel))
		return false;

	/* Compute the relid set for the join we are considering */
	joinrelids = bms_union(sjinfo->min_lefthand, sjinfo->min_righthand);

	/*
	 * We can't remove the join if any inner-rel attributes are used above the
	 * join.
	 *
	 * Note that this test only detects use of inner-rel attributes in higher
	 * join conditions and the target list.  There might be such attributes in
	 * pushed-down conditions at this join, too.  We check that case below.
	 *
	 * As a micro-optimization, it seems better to start with max_attr and
	 * count down rather than starting with min_attr and counting up, on the
	 * theory that the system attributes are somewhat less likely to be wanted
	 * and should be tested last.
	 */
	for (attroff = innerrel->max_attr - innerrel->min_attr;
		 attroff >= 0;
		 attroff--)
	{
		if (!bms_is_subset(innerrel->attr_needed[attroff], joinrelids))
			return false;
	}

	/*
	 * Similarly check that the inner rel isn't needed by any PlaceHolderVars
	 * that will be used above the join.  We only need to fail if such a PHV
	 * actually references some inner-rel attributes; but the correct check
	 * for that is relatively expensive, so we first check against ph_eval_at,
	 * which must mention the inner rel if the PHV uses any inner-rel attrs as
	 * non-lateral references.  Note that if the PHV's syntactic scope is just
	 * the inner rel, we can't drop the rel even if the PHV is variable-free.
	 */
	foreach(l, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(l);

		if (bms_overlap(phinfo->ph_lateral, innerrel->relids))
			return false;		/* it references innerrel laterally */
		if (bms_is_subset(phinfo->ph_needed, joinrelids))
			continue;			/* PHV is not used above the join */
		if (!bms_overlap(phinfo->ph_eval_at, innerrel->relids))
			continue;			/* it definitely doesn't reference innerrel */
		if (bms_is_subset(phinfo->ph_eval_at, innerrel->relids))
			return false;		/* there isn't any other place to eval PHV */
		if (bms_overlap(pull_varnos(root, (Node *) phinfo->ph_var->phexpr),
						innerrel->relids))
			return false;		/* it does reference innerrel */
	}

	/*
	 * Search for mergejoinable clauses that constrain the inner rel against
	 * either the outer rel or a pseudoconstant.  If an operator is
	 * mergejoinable then it behaves like equality for some btree opclass, so
	 * it's what we want.  The mergejoinability test also eliminates clauses
	 * containing volatile functions, which we couldn't depend on.
	 */
	foreach(l, innerrel->joininfo)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(l);

		/*
		 * If it's not a join clause for this outer join, we can't use it.
		 * Note that if the clause is pushed-down, then it is logically from
		 * above the outer join, even if it references no other rels (it might
		 * be from WHERE, for example).
		 */
		if (RINFO_IS_PUSHED_DOWN(restrictinfo, joinrelids))
		{
			/*
			 * If such a clause actually references the inner rel then join
			 * removal has to be disallowed.  We have to check this despite
			 * the previous attr_needed checks because of the possibility of
			 * pushed-down clauses referencing the rel.
			 */
			if (bms_is_member(innerrelid, restrictinfo->clause_relids))
				return false;
			continue;			/* else, ignore; not useful here */
		}

		/* Ignore if it's not a mergejoinable clause */
		if (!restrictinfo->can_join ||
			restrictinfo->mergeopfamilies == NIL)
			continue;			/* not mergejoinable */

		/*
		 * Check if clause has the form "outer op inner" or "inner op outer",
		 * and if so mark which side is inner.
		 */
		if (!clause_sides_match_join(restrictinfo, sjinfo->min_lefthand,
									 innerrel->relids))
			continue;			/* no good for these input relations */

		/* OK, add to list */
		clause_list = lappend(clause_list, restrictinfo);
	}

	/*
	 * Now that we have the relevant equality join clauses, try to prove the
	 * innerrel distinct.
	 */
	if (rel_is_distinct_for(root, innerrel, clause_list))
		return true;

	/*
	 * Some day it would be nice to check for other methods of establishing
	 * distinctness.
	 */
	return false;
}


/*
 * Remove the target relid from the planner's data structures, having
 * determined that there is no need to include it in the query.
 *
 * We are not terribly thorough here.  We must make sure that the rel is
 * no longer treated as a baserel, and that attributes of other baserels
 * are no longer marked as being needed at joins involving this rel.
 * Also, join quals involving the rel have to be removed from the joininfo
 * lists, but only if they belong to the outer join identified by joinrelids.
 */
static void
remove_rel_from_query(PlannerInfo *root, int relid, Relids joinrelids)
{
	RelOptInfo *rel = find_base_rel(root, relid);
	List	   *joininfos;
	Index		rti;
	ListCell   *l;
	ListCell   *nextl;

	/*
	 * Mark the rel as "dead" to show it is no longer part of the join tree.
	 * (Removing it from the baserel array altogether seems too risky.)
	 */
	rel->reloptkind = RELOPT_DEADREL;

	/*
	 * Remove references to the rel from other baserels' attr_needed arrays.
	 */
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *otherrel = root->simple_rel_array[rti];
		int			attroff;

		/* there may be empty slots corresponding to non-baserel RTEs */
		if (otherrel == NULL)
			continue;

		Assert(otherrel->relid == rti); /* sanity check on array */

		/* no point in processing target rel itself */
		if (otherrel == rel)
			continue;

		for (attroff = otherrel->max_attr - otherrel->min_attr;
			 attroff >= 0;
			 attroff--)
		{
			otherrel->attr_needed[attroff] =
				bms_del_member(otherrel->attr_needed[attroff], relid);
		}
	}

	/*
	 * Likewise remove references from SpecialJoinInfo data structures.
	 *
	 * This is relevant in case the outer join we're deleting is nested inside
	 * other outer joins: the upper joins' relid sets have to be adjusted. The
	 * RHS of the target outer join will be made empty here, but that's OK
	 * since caller will delete that SpecialJoinInfo entirely.
	 */
	foreach(l, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(l);

		sjinfo->min_lefthand = bms_del_member(sjinfo->min_lefthand, relid);
		sjinfo->min_righthand = bms_del_member(sjinfo->min_righthand, relid);
		sjinfo->syn_lefthand = bms_del_member(sjinfo->syn_lefthand, relid);
		sjinfo->syn_righthand = bms_del_member(sjinfo->syn_righthand, relid);
	}

	/*
	 * Likewise remove references from PlaceHolderVar data structures,
	 * removing any no-longer-needed placeholders entirely.
	 *
	 * Removal is a bit tricker than it might seem: we can remove PHVs that
	 * are used at the target rel and/or in the join qual, but not those that
	 * are used at join partner rels or above the join.  It's not that easy to
	 * distinguish PHVs used at partner rels from those used in the join qual,
	 * since they will both have ph_needed sets that are subsets of
	 * joinrelids.  However, a PHV used at a partner rel could not have the
	 * target rel in ph_eval_at, so we check that while deciding whether to
	 * remove or just update the PHV.  There is no corresponding test in
	 * join_is_removable because it doesn't need to distinguish those cases.
	 */
	for (l = list_head(root->placeholder_list); l != NULL; l = nextl)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(l);

		nextl = lnext(l);
		Assert(!bms_is_member(relid, phinfo->ph_lateral));
		if (bms_is_subset(phinfo->ph_needed, joinrelids) &&
			bms_is_member(relid, phinfo->ph_eval_at))
			root->placeholder_list = list_delete_ptr(root->placeholder_list,
													 phinfo);
		else
		{
			phinfo->ph_eval_at = bms_del_member(phinfo->ph_eval_at, relid);
			Assert(!bms_is_empty(phinfo->ph_eval_at));
			phinfo->ph_needed = bms_del_member(phinfo->ph_needed, relid);
		}
	}

	/*
	 * Remove any joinquals referencing the rel from the joininfo lists.
	 *
	 * In some cases, a joinqual has to be put back after deleting its
	 * reference to the target rel.  This can occur for pseudoconstant and
	 * outerjoin-delayed quals, which can get marked as requiring the rel in
	 * order to force them to be evaluated at or above the join.  We can't
	 * just discard them, though.  Only quals that logically belonged to the
	 * outer join being discarded should be removed from the query.
	 *
	 * We must make a copy of the rel's old joininfo list before starting the
	 * loop, because otherwise remove_join_clause_from_rels would destroy the
	 * list while we're scanning it.
	 */
	joininfos = list_copy(rel->joininfo);
	foreach(l, joininfos)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		remove_join_clause_from_rels(root, rinfo, rinfo->required_relids);

		if (RINFO_IS_PUSHED_DOWN(rinfo, joinrelids))
		{
			/* Recheck that qual doesn't actually reference the target rel */
			Assert(!bms_is_member(relid, rinfo->clause_relids));

			/*
			 * The required_relids probably aren't shared with anything else,
			 * but let's copy them just to be sure.
			 */
			rinfo->required_relids = bms_copy(rinfo->required_relids);
			rinfo->required_relids = bms_del_member(rinfo->required_relids,
													relid);
			distribute_restrictinfo_to_rels(root, rinfo);
		}
	}

	/*
	 * There may be references to the rel in root->fkey_list, but if so,
	 * match_foreign_keys_to_quals() will get rid of them.
	 */
}

/*
 * Remove any occurrences of the target relid from a joinlist structure.
 *
 * It's easiest to build a whole new list structure, so we handle it that
 * way.  Efficiency is not a big deal here.
 *
 * *nremoved is incremented by the number of occurrences removed (there
 * should be exactly one, but the caller checks that).
 */
static List *
remove_rel_from_joinlist(List *joinlist, int relid, int *nremoved)
{
	List	   *result = NIL;
	ListCell   *jl;

	foreach(jl, joinlist)
	{
		Node	   *jlnode = (Node *) lfirst(jl);

		if (IsA(jlnode, RangeTblRef))
		{
			int			varno = ((RangeTblRef *) jlnode)->rtindex;

			if (varno == relid)
				(*nremoved)++;
			else
				result = lappend(result, jlnode);
		}
		else if (IsA(jlnode, List))
		{
			/* Recurse to handle subproblem */
			List	   *sublist;

			sublist = remove_rel_from_joinlist((List *) jlnode,
											   relid, nremoved);
			/* Avoid including empty sub-lists in the result */
			if (sublist)
				result = lappend(result, sublist);
		}
		else
		{
			elog(ERROR, "unrecognized joinlist node type: %d",
				 (int) nodeTag(jlnode));
		}
	}

	return result;
}


/*
 * reduce_unique_semijoins
 *		尝试将半连接（Semi Join）转换为普通的内连接（Inner Join）。
 *		这通常发生在其内关系在连接条件上可被证明是唯一的情况下。
 *
 * 理想情况下，这一步应该在 reduce_outer_joins 期间进行，但那时我们还没有
 * 足够的信息。
 *
 * 当适用这种强度削减（strength reduction）时，我们只需要从 root->join_info_list
 * 中删除该半连接的 SpecialJoinInfo。
 * （我们无需费心去修改查询连接树中归属于它的连接类型，因为后续不会再查询该信息。）
 *
 * 如果我们可以证明在连接条件下，右表（内表）对于左表的任意一行最多只有一个匹配项（即右表在连接键上是唯一的），那么：
 * 1、右表没有重复匹配，内连接不会导致左表行膨胀。
 * 2、半连接的“去重”特性在这里自然满足。
 * 在这种情况下，半连接在逻辑上完全等价于内连接。优化器将其转换为内连接后，可以利用更多常规的连接优化策略。
 */
void
reduce_unique_semijoins(PlannerInfo *root)
{
	ListCell   *lc;
	ListCell   *next;

	/*
	 * 遍历连接列表，查找半连接（Semi Join）。
	 * 扫描 join_info_list 以查找 semi-joins。我们不能使用 foreach，
	 * 因为我们可能会删除当前的单元格。
	 */
	for (lc = list_head(root->join_info_list); lc != NULL; lc = next)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);
		int			innerrelid;
		RelOptInfo *innerrel;
		Relids		joinrelids;
		List	   *restrictlist;

		next = lnext(lc);

		/*
		 * 必须是指向单个基础关系（baserel）的非延迟半连接，否则我们无法对其进行
		 * 任何操作。（对于半连接来说，delay_upper_joins 大概是不可能被设置的，
		 * 但我们不妨检查一下。）
		 */
		if (sjinfo->jointype != JOIN_SEMI ||
			sjinfo->delay_upper_joins)
			continue;

		/* sjinfo->min_righthand 必须是一个单一的基础表（Baserel）。
		 * 目前的逻辑不支持右侧是复杂子查询或连接的情况。
		 */
		if (!bms_get_singleton_member(sjinfo->min_righthand, &innerrelid))
			continue;

		/* 查找内关系的 RelOptInfo 结构体 */
		innerrel = find_base_rel(root, innerrelid);

		/*
		 * 在我们费力运行 generate_join_implied_equalities 之前，先做一个快速检查，
		 * 以排除那些我们肯定无法证明内关系唯一性的情况。
		 *
		 * 检查右表是否有支持唯一性的性质（例如是否有唯一索引、主键等）。
		 * 如果这个表连一个唯一索引都没有，那就没必要进行后面复杂的检查了，直接跳过。
		 */
		if (!rel_supports_distinctness(root, innerrel))
			continue;

		/* 计算我们正在考虑的连接的关系ID集合 */
		joinrelids = bms_union(sjinfo->min_lefthand, sjinfo->min_righthand);

		/*
		 * restrictlist 收集了所有连接条件，
		 * 包括从等价类（Equivalence Class）推导出的隐含相等条件，以及直接写在 ON 子句中的条件。
		 *
		 * 由于我们只考虑右侧是单个关系的情况，它所拥有的任何连接子句必然是
		 * 将其链接到半连接的 min_lefthand 的子句。我们还可以考虑由等价类（EC）
		 * 派生的连接子句。
		 */
		restrictlist =
			list_concat(generate_join_implied_equalities(root,
														 joinrelids,
														 sjinfo->min_lefthand,
														 innerrel), innerrel->joininfo);

		/*
		 * 测试内关系是否对于这些子句是唯一的
		 * 它会检查连接条件是否使用了右表的所有唯一索引列（或主键列）。
		 *
		 * 例子：如果 Semi Join 是 ON left.id = right.pk，且 pk 是 right 表的主键，
		 * 那么对于任意 left.id，最多只能找到一个 right 行。此时函数返回 true。
		 */
		if (!innerrel_is_unique(root,
								joinrelids, sjinfo->min_lefthand, innerrel,
								JOIN_SEMI, restrictlist, true))
			continue;

		/*
		 * 好的，从列表中移除该 SpecialJoinInfo
		 * 这是优化生效的一步。
		 * PostgreSQL 的规划器通过 join_info_list 来追踪外连接和半连接。
		 * 移除 sjinfo 后，规划器在后续步骤中就不再把这个连接视为 Semi Join，而是当作普通的 Inner Join 来规划。
		 */
		root->join_info_list = list_delete_ptr(root->join_info_list, sjinfo);
	}
}


/*
 * rel_supports_distinctness
 *		判断该关系是否可能在某些列集合上被证明是唯一的。
 *
 * 这是一个 rel_is_distinct_for() 的预检查函数。
 * 如果 rel_is_distinct_for() 可能对此关系返回 true，则此函数必须返回 true，
 * 但它不应消耗大量 CPU 周期。
 * 其目的是让调用者在调用不可能成功的情况下，避免执行可能耗时的处理来计算
 * rel_is_distinct_for() 的参数列表。
 */
static bool
rel_supports_distinctness(PlannerInfo *root, RelOptInfo *rel)
{
	/* 我们只了解基础关系（baserels）... */
	if (rel->reloptkind != RELOPT_BASEREL)
		return false;
	if (rel->rtekind == RTE_RELATION)
	{
		/*
		 * 对于普通关系，我们只知道如何通过引用唯一索引来证明唯一性。
		 * 确保至少存在一个合适的唯一索引。它必须是立即强制执行的（immediate），
		 * 且不是部分索引。（请保持这些条件与 relation_has_unique_index_for 同步！）
		 */
		ListCell   *lc;

		foreach(lc, rel->indexlist)
		{
			IndexOptInfo *ind = (IndexOptInfo *) lfirst(lc);

			if (ind->unique && ind->immediate && ind->indpred == NIL)
				return true;
		}
	}
	else if (rel->rtekind == RTE_SUBQUERY)
	{
		Query	   *subquery = root->simple_rte_array[rel->relid]->subquery;

		/* 检查子查询是否具有任何支持唯一性的性质 */
		if (query_supports_distinctness(subquery))
			return true;
	}
	/* 对于任何其他 rtekind，我们没有证明规则。 */
	return false;
}

/*
 * rel_is_distinct_for
 *		Does the relation return only distinct rows according to clause_list?
 *
 * clause_list is a list of join restriction clauses involving this rel and
 * some other one.  Return true if no two rows emitted by this rel could
 * possibly join to the same row of the other rel.
 *
 * The caller must have already determined that each condition is a
 * mergejoinable equality with an expression in this relation on one side, and
 * an expression not involving this relation on the other.  The transient
 * outer_is_left flag is used to identify which side references this relation:
 * left side if outer_is_left is false, right side if it is true.
 *
 * Note that the passed-in clause_list may be destructively modified!  This
 * is OK for current uses, because the clause_list is built by the caller for
 * the sole purpose of passing to this function.
 */
static bool
rel_is_distinct_for(PlannerInfo *root, RelOptInfo *rel, List *clause_list)
{
	/*
	 * We could skip a couple of tests here if we assume all callers checked
	 * rel_supports_distinctness first, but it doesn't seem worth taking any
	 * risk for.
	 */
	if (rel->reloptkind != RELOPT_BASEREL)
		return false;
	if (rel->rtekind == RTE_RELATION)
	{
		/*
		 * Examine the indexes to see if we have a matching unique index.
		 * relation_has_unique_index_for automatically adds any usable
		 * restriction clauses for the rel, so we needn't do that here.
		 */
		if (relation_has_unique_index_for(root, rel, clause_list, NIL, NIL))
			return true;
	}
	else if (rel->rtekind == RTE_SUBQUERY)
	{
		Index		relid = rel->relid;
		Query	   *subquery = root->simple_rte_array[relid]->subquery;
		List	   *colnos = NIL;
		List	   *opids = NIL;
		ListCell   *l;

		/*
		 * Build the argument lists for query_is_distinct_for: a list of
		 * output column numbers that the query needs to be distinct over, and
		 * a list of equality operators that the output columns need to be
		 * distinct according to.
		 *
		 * (XXX we are not considering restriction clauses attached to the
		 * subquery; is that worth doing?)
		 */
		foreach(l, clause_list)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);
			Oid			op;
			Var		   *var;

			/*
			 * Get the equality operator we need uniqueness according to.
			 * (This might be a cross-type operator and thus not exactly the
			 * same operator the subquery would consider; that's all right
			 * since query_is_distinct_for can resolve such cases.)  The
			 * caller's mergejoinability test should have selected only
			 * OpExprs.
			 */
			op = castNode(OpExpr, rinfo->clause)->opno;

			/* caller identified the inner side for us */
			if (rinfo->outer_is_left)
				var = (Var *) get_rightop(rinfo->clause);
			else
				var = (Var *) get_leftop(rinfo->clause);

			/*
			 * We may ignore any RelabelType node above the operand.  (There
			 * won't be more than one, since eval_const_expressions() has been
			 * applied already.)
			 */
			if (var && IsA(var, RelabelType))
				var = (Var *) ((RelabelType *) var)->arg;

			/*
			 * If inner side isn't a Var referencing a subquery output column,
			 * this clause doesn't help us.
			 */
			if (!var || !IsA(var, Var) ||
				var->varno != relid || var->varlevelsup != 0)
				continue;

			colnos = lappend_int(colnos, var->varattno);
			opids = lappend_oid(opids, op);
		}

		if (query_is_distinct_for(subquery, colnos, opids))
			return true;
	}
	return false;
}


/*
 * query_supports_distinctness
 *	  判断查询是否可能在某些输出列上被证明具有唯一性
 *
 * 参数：
 *   query - 要检查的查询树结构
 *
 * 返回值：
 *   如果查询可能在某些列上具有唯一性，则返回true；否则返回false
 *
 * 功能说明：
 *   此函数是query_is_distinct_for()的预检查函数。它必须在query_is_distinct_for()
 *   可能返回true时返回true，但不应消耗大量计算资源。设计思路是让调用者可以避免
 *   执行可能昂贵的处理来计算query_is_distinct_for()的参数列表，如果调用不可能成功的话。
 *
 * 应用场景：
 *   在查询优化过程中，特别是在处理半连接（semi-joins）和子查询优化时，
 *   需要确定查询结果是否具有某些列上的唯一性，以应用更高效的执行策略。
 */
bool
query_supports_distinctness(Query *query)
{
	/*
	 * 集合返回函数(SRFs)会破坏唯一性，除非使用了DISTINCT子句
	 * 原因：SRF可能为每行输入生成多行输出，导致即使在主键列上也可能出现重复
	 */
	if (query->hasTargetSRFs && query->distinctClause == NIL)
		return false;

	/*
	 * 检查查询是否包含可以证明唯一性的特性：
	 *   1. DISTINCT子句 - 显式去重
	 *   2. GROUP BY子句 - 按分组列聚合，每组只返回一行
	 *   3. GROUPING SETS - 多维度分组，结果具有分组键的唯一性
	 *   4. 聚合函数 - 通常返回汇总结果，具有分组键的唯一性
	 *   5. HAVING子句 - 与GROUP BY配合使用，结果具有分组键的唯一性
	 *   6. 集合操作(UNION/INTERSECT等) - 某些集合操作结果具有自然唯一性
	 */
	if (query->distinctClause != NIL ||       	/* 存在DISTINCT子句 */
		query->groupClause != NIL ||         	/* 存在GROUP BY子句 */
		query->groupingSets != NIL ||        	/* 存在GROUPING SETS */
		query->hasAggs ||                    	/* 包含聚合函数 */
		query->havingQual ||                 	/* 存在HAVING条件 */
		query->setOperations)                	/* 存在集合操作 */
		return true;

	/* 如果没有上述特性，则查询结果默认不保证任何列的唯一性 */
	return false;
}

/*
 * query_is_distinct_for - 判断查询在指定的列上是否保证不会返回重复行
 *
 * 参数:
 * - query: 尚未进行规划的子查询（当前用法中，它总是来自子查询RTE，规划器不会修改它）
 * - colnos: 输出列编号(resno)的整数列表
 * - opids: 对应的上层相等运算符OID列表，用于定义"唯一性"判断标准
 *
 * 返回值:
 * - bool: 如果指定列集保证唯一返回true，否则返回false
 *
 * 注："唯一性"根据opids中列出的上层相等运算符来定义。这些运算符可能是跨类型的，
 * 与子查询自身使用的相等运算符可能不完全相同。函数使用equality_ops_are_compatible()
 * 来检查兼容性，该函数会检查btree或hash操作符族的成员资格，因此对于此处需要处理的
 * 所有运算符都能给出可信的答案。
 */
bool
query_is_distinct_for(Query *query, List *colnos, List *opids)
{
	ListCell   *l;
	Oid		opid;

	/* 验证colnos和opids列表长度必须相同 */
	Assert(list_length(colnos) == list_length(opids));

	/*
	 * 检查DISTINCT（包括DISTINCT ON）情况：
	 * 如果DISTINCT子句中的所有列都包含在colnos中，且运算符语义匹配，
	 * 则保证唯一性。即使DISTINCT列或目标列表中存在集合返回函数(SRF)也成立。
	 */
	if (query->distinctClause)
	{
		foreach(l, query->distinctClause)
		{
			/* 获取排序分组子句对应的目标列表项 */
			SortGroupClause *sgc = (SortGroupClause *) lfirst(l);
			TargetEntry *tle = get_sortgroupclause_tle(sgc,
									   query->targetList);

			/* 搜索对应列并验证运算符兼容性 */
			opid = distinct_col_search(tle->resno, colnos, opids);
			if (!OidIsValid(opid) ||
				!equality_ops_are_compatible(opid, sgc->eqop))
				break;            /* 无匹配则提前退出 */
		}
		if (l == NULL)           /* 所有DISTINCT列都匹配成功？ */
			return true;
	}

	/*
	 * 检查目标列表中的集合返回函数(SRF)：
	 * 即使在tlist计算前进行了分组，SRF也可能导致返回重复行。
	 * （注意：如果所有tlist SRF都在GROUP BY列中，实际上也是安全的，因为它们会在分组前展开，
	 * 但目前认为不值得为此进行专门检查。）
	 */
	if (query->hasTargetSRFs)
		return false;

	/*
	 * 检查GROUP BY（无GROUPING SETS）情况：
	 * 类似DISTINCT，如果GROUP BY子句中的所有列都包含在colnos中且运算符语义匹配，
	 * 则保证唯一性。
	 */
	if (query->groupClause && !query->groupingSets)
	{
		foreach(l, query->groupClause)
		{
			SortGroupClause *sgc = (SortGroupClause *) lfirst(l);
			TargetEntry *tle = get_sortgroupclause_tle(sgc,
									   query->targetList);

			opid = distinct_col_search(tle->resno, colnos, opids);
			if (!OidIsValid(opid) ||
				!equality_ops_are_compatible(opid, sgc->eqop))
				break;            /* 无匹配则提前退出 */
		}
		if (l == NULL)           /* 所有GROUP BY列都匹配成功？ */
			return true;
	}
	/* 检查GROUPING SETS情况 */
	else if (query->groupingSets)
	{
		/*
		 * 如果有带表达式的分组集，我们可能不具有唯一性，且分析起来很困难，
		 * 因此直接返回false
		 */
		if (query->groupClause)
			return false;

		/*
		 * 如果没有groupClause（即没有分组表达式），我们可能有一个或多个空分组集。
		 * 如果只有一个空分组集，则只返回一行，肯定是唯一的。否则肯定不唯一。
		 */
		if (list_length(query->groupingSets) == 1 &&
			((GroupingSet *) linitial(query->groupingSets))->kind == GROUPING_SET_EMPTY)
			return true;
		else
			return false;
	}
	else
	{
		/*
		 * 如果没有GROUP BY，但有聚合函数或HAVING子句，
		 * 那么结果最多只有一行，对任何运算符来说都是唯一的
		 */
		if (query->hasAggs || query->havingQual)
			return true;
	}

	/*
	 * 检查集合操作（UNION、INTERSECT、EXCEPT）情况：
	 * 这些操作保证整个输出行的唯一性，除非使用了ALL关键字
	 */
	if (query->setOperations)
	{
		SetOperationStmt *topop = castNode(SetOperationStmt, query->setOperations);

		Assert(topop->op != SETOP_NONE);

		if (!topop->all)  /* 不是UNION ALL、INTERSECT ALL或EXCEPT ALL */
		{
			ListCell   *lg;

			/* 我们需要检查所有非junk输出列是否都在colnos中 */
			lg = list_head(topop->groupClauses);
			foreach(l, query->targetList)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(l);
				SortGroupClause *sgc;

				if (tle->resjunk)
					continue;    /* 忽略resjunk列 */

				/* 非resjunk列应该有对应的分组子句 */
				Assert(lg != NULL);
				sgc = (SortGroupClause *) lfirst(lg);
				lg = lnext(lg);

				opid = distinct_col_search(tle->resno, colnos, opids);
				if (!OidIsValid(opid) ||
					!equality_ops_are_compatible(opid, sgc->eqop))
					break;    /* 无匹配则提前退出 */
			}
			if (l == NULL)    /* 所有非junk列都匹配成功？ */
				return true;
		}
	}

	/*
	 * XXX 还有其他容易判断结果必须唯一的情况吗？
	 *
	 * 如果要为这个函数添加更多智能判断，请确保同时更新query_supports_distinctness()函数
	 * 以保持一致性。
	 */

	/* 所有唯一性检查都失败，返回false */
	return false;
}


/*
 * distinct_col_search - subroutine for query_is_distinct_for
 *
 * If colno is in colnos, return the corresponding element of opids,
 * else return InvalidOid.  (Ordinarily colnos would not contain duplicates,
 * but if it does, we arbitrarily select the first match.)
 */
static Oid
distinct_col_search(int colno, List *colnos, List *opids)
{
	ListCell   *lc1,
			   *lc2;

	forboth(lc1, colnos, lc2, opids)
	{
		if (colno == lfirst_int(lc1))
			return lfirst_oid(lc2);
	}
	return InvalidOid;
}


/*
 * innerrel_is_unique - 检查内关系是否对于外关系具有唯一性
 *
 * 该函数用于判断在给定连接条件下，内关系(innerrel)对于外关系(outerrel)中的任意元组，
 * 最多只会有一个匹配的元组。这是查询优化中半连接优化和去重优化的关键判断函数。
 *
 * 参数说明:
 * - root: 查询优化器的全局信息结构
 * - joinrelids: 连接关系的ID集合（用于缓存优化）
 * - outerrelids: 外关系的ID集合
 * - innerrel: 内关系的优化信息结构
 * - jointype: 连接类型（如INNER JOIN, LEFT JOIN等）
 * - restrictlist: 连接限制条件列表
 * - force_cache: 是否强制缓存结果（用于非标准调用场景）
 *
 * 返回值:
 * - bool: 如果能证明内关系对于外关系具有唯一性则返回true，否则返回false
 *
 * 工作原理:
 * 1. 首先进行快速否定检查：如果没有连接条件或内关系不支持唯一性，则直接返回false
 * 2. 查询缓存：检查是否已有相关的结果缓存，避免重复计算
 * 3. 实际证明：调用is_innerrel_unique_for函数进行具体的唯一性证明
 * 4. 结果缓存：将证明结果缓存起来供后续查询使用
 *
 * 缓存机制:
 * - unique_for_rels: 存储已证明具有唯一性的外关系集合
 * - non_unique_for_rels: 存储已证明不具有唯一性的外关系集合
 *
 * 注意事项:
 * - 对于外连接，只考虑真正的连接条件(joinquals)，忽略下推的其他条件(otherquals)
 * - 缓存机制在GEQO模式和连接搜索插件中有重要作用
 */
bool
innerrel_is_unique(PlannerInfo *root,
				   Relids joinrelids,
				   Relids outerrelids,
				   RelOptInfo *innerrel,
				   JoinType jointype,
				   List *restrictlist,
				   bool force_cache)
{
	MemoryContext old_context;  /* 用于内存上下文切换 */
	ListCell   *lc;             /* 列表遍历指针 */

	/* 当没有连接条件时，肯定无法证明唯一性 */
	if (restrictlist == NIL)
		return false;

	/*
	 * 快速检查：排除明显无法证明内关系唯一性的情况。
	 * 如果内关系本身不支持唯一性（例如包含集合返回函数），则直接返回false。
	 */
	if (!rel_supports_distinctness(root, innerrel))
		return false;

	/*
	 * 查询缓存：检查是否已证明内关系对于当前外关系的某个子集具有唯一性。
	 * 由于额外的外关系不会降低内关系的唯一性，所以只需要检查是否有子集已满足条件。
	 */
	foreach(lc, innerrel->unique_for_rels)
	{
		Relids		unique_for_rels = (Relids) lfirst(lc);

		/* 如果缓存中的关系集合是当前外关系的子集，则说明已满足唯一性条件 */
		if (bms_is_subset(unique_for_rels, outerrelids))
			return true;		/* 成功证明唯一性！ */
	}

	/*
	 * 反向检查：检查是否已确定当前外关系或其超集无法证明内关系的唯一性。
	 * 如果外关系是某个已知无法证明唯一性的关系的子集，则当前也无法证明。
	 */
	foreach(lc, innerrel->non_unique_for_rels)
	{
		Relids		unique_for_rels = (Relids) lfirst(lc);

		/* 如果当前外关系是缓存中关系的子集，则说明无法证明唯一性 */
		if (bms_is_subset(outerrelids, unique_for_rels))
			return false;
	}

	/* 没有缓存信息，需要实际进行唯一性证明 */
	if (is_innerrel_unique_for(root, joinrelids, outerrelids, innerrel,
							   jointype, restrictlist))
	{
		/*
		 * 缓存正面结果供未来查询使用，确保将其保存在planner_cxt中，
		 * 即使当前在GEQO环境中工作也是如此。
		 *
		 * 注意：理论上可以尝试找出证明内关系唯一的最小外关系子集，
		 * 但这不值得额外开销，因为规划器是增量构建连接关系的，
		 * 所以会在任何超集之前先看到最小充分的外关系。
		 */
		old_context = MemoryContextSwitchTo(root->planner_cxt);
		innerrel->unique_for_rels = lappend(innerrel->unique_for_rels,
											bms_copy(outerrelids));
		MemoryContextSwitchTo(old_context);

		return true;			/* 成功证明唯一性！ */
	}
	else
	{
		/*
		 * 外关系的连接条件都无法证明内关系的唯一性，
		 * 因此可以在未来的检查中安全地拒绝此外关系或其任何子集。
		 *
		 * 然而，在正常规划模式下，缓存这些知识完全没有意义；
		 * 因为我们会从小到大逐步构建连接关系，不会再查询相同的内容。
		 * 但在GEQO模式下有用，因为这些知识可以在连续的规划尝试间传递；
		 * 在使用连接搜索插件时也可能有用。因此当join_search_private非空时进行缓存。
		 * （是的，这是个hack，但看起来合理。）
		 *
		 * 此外，允许调用者覆盖该启发式规则并强制缓存；
		 * 这对reduce_unique_semijoins很有用，它在正常的连接搜索开始前就调用此函数。
		 */
		if (force_cache || root->join_search_private)
		{
			old_context = MemoryContextSwitchTo(root->planner_cxt);
			innerrel->non_unique_for_rels =
				lappend(innerrel->non_unique_for_rels,
						bms_copy(outerrelids));
			MemoryContextSwitchTo(old_context);
		}

		return false;
	}
}


/*
 * is_innerrel_unique_for
 *	  Check if the innerrel provably contains at most one tuple matching any
 *	  tuple from the outerrel, based on join clauses in the 'restrictlist'.
 */
static bool
is_innerrel_unique_for(PlannerInfo *root,
					   Relids joinrelids,
					   Relids outerrelids,
					   RelOptInfo *innerrel,
					   JoinType jointype,
					   List *restrictlist)
{
	List	   *clause_list = NIL;
	ListCell   *lc;

	/*
	 * Search for mergejoinable clauses that constrain the inner rel against
	 * the outer rel.  If an operator is mergejoinable then it behaves like
	 * equality for some btree opclass, so it's what we want.  The
	 * mergejoinability test also eliminates clauses containing volatile
	 * functions, which we couldn't depend on.
	 */
	foreach(lc, restrictlist)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(lc);

		/*
		 * As noted above, if it's a pushed-down clause and we're at an outer
		 * join, we can't use it.
		 */
		if (IS_OUTER_JOIN(jointype) &&
			RINFO_IS_PUSHED_DOWN(restrictinfo, joinrelids))
			continue;

		/* Ignore if it's not a mergejoinable clause */
		if (!restrictinfo->can_join ||
			restrictinfo->mergeopfamilies == NIL)
			continue;			/* not mergejoinable */

		/*
		 * Check if clause has the form "outer op inner" or "inner op outer",
		 * and if so mark which side is inner.
		 */
		if (!clause_sides_match_join(restrictinfo, outerrelids,
									 innerrel->relids))
			continue;			/* no good for these input relations */

		/* OK, add to list */
		clause_list = lappend(clause_list, restrictinfo);
	}

	/* Let rel_is_distinct_for() do the hard work */
	return rel_is_distinct_for(root, innerrel, clause_list);
}
