/*-------------------------------------------------------------------------
 *
 * restrictinfo.c
 *	  RestrictInfo node manipulation routines.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/restrictinfo.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/restrictinfo.h"


/* source-code-compatibility hacks for pull_varnos() API change */
#define pull_varnos(a,b) pull_varnos_new(a,b)

static RestrictInfo *make_restrictinfo_internal(PlannerInfo *root,
												Expr *clause,
												Expr *orclause,
												bool is_pushed_down,
												bool outerjoin_delayed,
												bool pseudoconstant,
												Index security_level,
												Relids required_relids,
												Relids outer_relids,
												Relids nullable_relids);
static Expr *make_sub_restrictinfos(PlannerInfo *root,
									Expr *clause,
									bool is_pushed_down,
									bool outerjoin_delayed,
									bool pseudoconstant,
									Index security_level,
									Relids required_relids,
									Relids outer_relids,
									Relids nullable_relids);


/*
 * make_restrictinfo
 *
 * Build a RestrictInfo node containing the given subexpression.
 *
 * The is_pushed_down, outerjoin_delayed, and pseudoconstant flags for the
 * RestrictInfo must be supplied by the caller, as well as the correct values
 * for security_level, outer_relids, and nullable_relids.
 * required_relids can be NULL, in which case it defaults to the actual clause
 * contents (i.e., clause_relids).
 *
 * We initialize fields that depend only on the given subexpression, leaving
 * others that depend on context (or may never be needed at all) to be filled
 * later.
 */
RestrictInfo *
make_restrictinfo(Expr *clause,
				  bool is_pushed_down,
				  bool outerjoin_delayed,
				  bool pseudoconstant,
				  Index security_level,
				  Relids required_relids,
				  Relids outer_relids,
				  Relids nullable_relids)
{
	return make_restrictinfo_new(NULL,
								 clause,
								 is_pushed_down,
								 outerjoin_delayed,
								 pseudoconstant,
								 security_level,
								 required_relids,
								 outer_relids,
								 nullable_relids);
}

RestrictInfo *
make_restrictinfo_new(PlannerInfo *root,
					  Expr *clause,
					  bool is_pushed_down,
					  bool outerjoin_delayed,
					  bool pseudoconstant,
					  Index security_level,
					  Relids required_relids,
					  Relids outer_relids,
					  Relids nullable_relids)
{
	/*
	 * If it's an OR clause, build a modified copy with RestrictInfos inserted
	 * above each subclause of the top-level AND/OR structure.
	 */
	if (is_orclause(clause))
		return (RestrictInfo *) make_sub_restrictinfos(root,
													   clause,
													   is_pushed_down,
													   outerjoin_delayed,
													   pseudoconstant,
													   security_level,
													   required_relids,
													   outer_relids,
													   nullable_relids);

	/* Shouldn't be an AND clause, else AND/OR flattening messed up */
	Assert(!is_andclause(clause));

	return make_restrictinfo_internal(root,
									  clause,
									  NULL,
									  is_pushed_down,
									  outerjoin_delayed,
									  pseudoconstant,
									  security_level,
									  required_relids,
									  outer_relids,
									  nullable_relids);
}

/*
 * make_restrictinfo_internal
 *
 * Common code for the main entry points and the recursive cases.
 */
static RestrictInfo *
make_restrictinfo_internal(PlannerInfo *root,
						   Expr *clause,
						   Expr *orclause,
						   bool is_pushed_down,
						   bool outerjoin_delayed,
						   bool pseudoconstant,
						   Index security_level,
						   Relids required_relids,
						   Relids outer_relids,
						   Relids nullable_relids)
{
	RestrictInfo *restrictinfo = makeNode(RestrictInfo);

	restrictinfo->clause = clause;
	restrictinfo->orclause = orclause;
	restrictinfo->is_pushed_down = is_pushed_down;
	restrictinfo->outerjoin_delayed = outerjoin_delayed;
	restrictinfo->pseudoconstant = pseudoconstant;
	restrictinfo->can_join = false; /* may get set below */
	restrictinfo->security_level = security_level;
	restrictinfo->outer_relids = outer_relids;
	restrictinfo->nullable_relids = nullable_relids;

	/*
	 * If it's potentially delayable by lower-level security quals, figure out
	 * whether it's leakproof.  We can skip testing this for level-zero quals,
	 * since they would never get delayed on security grounds anyway.
	 */
	if (security_level > 0)
		restrictinfo->leakproof = !contain_leaked_vars((Node *) clause);
	else
		restrictinfo->leakproof = false;	/* really, "don't know" */

	/*
	 * If it's a binary opclause, set up left/right relids info. In any case
	 * set up the total clause relids info.
	 */
	if (is_opclause(clause) && list_length(((OpExpr *) clause)->args) == 2)
	{
		restrictinfo->left_relids = pull_varnos(root, get_leftop(clause));
		restrictinfo->right_relids = pull_varnos(root, get_rightop(clause));

		restrictinfo->clause_relids = bms_union(restrictinfo->left_relids,
												restrictinfo->right_relids);

		/*
		 * Does it look like a normal join clause, i.e., a binary operator
		 * relating expressions that come from distinct relations? If so we
		 * might be able to use it in a join algorithm.  Note that this is a
		 * purely syntactic test that is made regardless of context.
		 */
		if (!bms_is_empty(restrictinfo->left_relids) &&
			!bms_is_empty(restrictinfo->right_relids) &&
			!bms_overlap(restrictinfo->left_relids,
						 restrictinfo->right_relids))
		{
			restrictinfo->can_join = true;
			/* pseudoconstant should certainly not be true */
			Assert(!restrictinfo->pseudoconstant);
		}
	}
	else
	{
		/* Not a binary opclause, so mark left/right relid sets as empty */
		restrictinfo->left_relids = NULL;
		restrictinfo->right_relids = NULL;
		/* and get the total relid set the hard way */
		restrictinfo->clause_relids = pull_varnos(root, (Node *) clause);
	}

/* required_relids defaults to clause_relids */
	if (required_relids != NULL)
		restrictinfo->required_relids = required_relids;
	else
		restrictinfo->required_relids = restrictinfo->clause_relids;

	/*
	 * Fill in all the cacheable fields with "not yet set" markers. None of
	 * these will be computed until/unless needed.  Note in particular that we
	 * don't mark a binary opclause as mergejoinable or hashjoinable here;
	 * that happens only if it appears in the right context (top level of a
	 * joinclause list).
	 */
	restrictinfo->parent_ec = NULL;

	restrictinfo->eval_cost.startup = -1;
	restrictinfo->norm_selec = -1;
	restrictinfo->outer_selec = -1;

	restrictinfo->mergeopfamilies = NIL;

	restrictinfo->left_ec = NULL;
	restrictinfo->right_ec = NULL;
	restrictinfo->left_em = NULL;
	restrictinfo->right_em = NULL;
	restrictinfo->scansel_cache = NIL;

	restrictinfo->outer_is_left = false;

	restrictinfo->hashjoinoperator = InvalidOid;

	restrictinfo->left_bucketsize = -1;
	restrictinfo->right_bucketsize = -1;
	restrictinfo->left_mcvfreq = -1;
	restrictinfo->right_mcvfreq = -1;

	return restrictinfo;
}

/*
 * Recursively insert sub-RestrictInfo nodes into a boolean expression.
 *
 * We put RestrictInfos above simple (non-AND/OR) clauses and above
 * sub-OR clauses, but not above sub-AND clauses, because there's no need.
 * This may seem odd but it is closely related to the fact that we use
 * implicit-AND lists at top level of RestrictInfo lists.  Only ORs and
 * simple clauses are valid RestrictInfos.
 *
 * The same is_pushed_down, outerjoin_delayed, and pseudoconstant flag
 * values can be applied to all RestrictInfo nodes in the result.  Likewise
 * for security_level, outer_relids, and nullable_relids.
 *
 * The given required_relids are attached to our top-level output,
 * but any OR-clause constituents are allowed to default to just the
 * contained rels.
 */
static Expr *
make_sub_restrictinfos(PlannerInfo *root,
					   Expr *clause,
					   bool is_pushed_down,
					   bool outerjoin_delayed,
					   bool pseudoconstant,
					   Index security_level,
					   Relids required_relids,
					   Relids outer_relids,
					   Relids nullable_relids)
{
	if (is_orclause(clause))
	{
		List	   *orlist = NIL;
		ListCell   *temp;

		foreach(temp, ((BoolExpr *) clause)->args)
			orlist = lappend(orlist,
							 make_sub_restrictinfos(root,
													lfirst(temp),
													is_pushed_down,
													outerjoin_delayed,
													pseudoconstant,
													security_level,
													NULL,
													outer_relids,
													nullable_relids));
		return (Expr *) make_restrictinfo_internal(root,
												   clause,
												   make_orclause(orlist),
												   is_pushed_down,
												   outerjoin_delayed,
												   pseudoconstant,
												   security_level,
												   required_relids,
												   outer_relids,
												   nullable_relids);
	}
	else if (is_andclause(clause))
	{
		List	   *andlist = NIL;
		ListCell   *temp;

		foreach(temp, ((BoolExpr *) clause)->args)
			andlist = lappend(andlist,
							  make_sub_restrictinfos(root,
													 lfirst(temp),
													 is_pushed_down,
													 outerjoin_delayed,
													 pseudoconstant,
													 security_level,
													 required_relids,
													 outer_relids,
													 nullable_relids));
		return make_andclause(andlist);
	}
	else
		return (Expr *) make_restrictinfo_internal(root,
												   clause,
												   NULL,
												   is_pushed_down,
												   outerjoin_delayed,
												   pseudoconstant,
												   security_level,
												   required_relids,
												   outer_relids,
												   nullable_relids);
}

/*
 * commute_restrictinfo
 *
 * Given a RestrictInfo containing a binary opclause, produce a RestrictInfo
 * representing the commutation of that clause.  The caller must pass the
 * OID of the commutator operator (which it's presumably looked up, else
 * it would not know this is valid).
 *
 * Beware that the result shares sub-structure with the given RestrictInfo.
 * That's okay for the intended usage with derived index quals, but might
 * be hazardous if the source is subject to change.  Also notice that we
 * assume without checking that the commutator op is a member of the same
 * btree and hash opclasses as the original op.
 */
RestrictInfo *
commute_restrictinfo(RestrictInfo *rinfo, Oid comm_op)
{
	RestrictInfo *result;
	OpExpr	   *newclause;
	OpExpr	   *clause = castNode(OpExpr, rinfo->clause);

	Assert(list_length(clause->args) == 2);

	/* flat-copy all the fields of clause ... */
	newclause = makeNode(OpExpr);
	memcpy(newclause, clause, sizeof(OpExpr));

	/* ... and adjust those we need to change to commute it */
	newclause->opno = comm_op;
	newclause->opfuncid = InvalidOid;
	newclause->args = list_make2(lsecond(clause->args),
								 linitial(clause->args));

	/* likewise, flat-copy all the fields of rinfo ... */
	result = makeNode(RestrictInfo);
	memcpy(result, rinfo, sizeof(RestrictInfo));

	/*
	 * ... and adjust those we need to change.  Note in particular that we can
	 * preserve any cached selectivity or cost estimates, since those ought to
	 * be the same for the new clause.  Likewise we can keep the source's
	 * parent_ec.
	 */
	result->clause = (Expr *) newclause;
	result->left_relids = rinfo->right_relids;
	result->right_relids = rinfo->left_relids;
	Assert(result->orclause == NULL);
	result->left_ec = rinfo->right_ec;
	result->right_ec = rinfo->left_ec;
	result->left_em = rinfo->right_em;
	result->right_em = rinfo->left_em;
	result->scansel_cache = NIL;	/* not worth updating this */
	if (rinfo->hashjoinoperator == clause->opno)
		result->hashjoinoperator = comm_op;
	else
		result->hashjoinoperator = InvalidOid;
	result->left_bucketsize = rinfo->right_bucketsize;
	result->right_bucketsize = rinfo->left_bucketsize;
	result->left_mcvfreq = rinfo->right_mcvfreq;
	result->right_mcvfreq = rinfo->left_mcvfreq;

	return result;
}

/*
 * restriction_is_or_clause
 *
 * 检查RestrictInfo节点是否包含OR子句条件
 * 当restrictinfo节点表示一个OR条件时返回true，否则返回false
 *
 * 参数说明：
 * - restrictinfo: RestrictInfo类型的指针，表示一个查询条件的优化信息结构
 *
 * 返回值：
 * - bool类型：如果条件是OR子句返回true，否则返回false
 *
 * 功能说明：
 * 该函数是查询优化器中用于识别条件类型的辅助函数，通过检查RestrictInfo结构中的orclause字段
 * 是否为非空来判断条件是否是OR子句。这对于查询优化器决定如何处理特定条件至关重要，
 * 因为OR条件通常需要特殊的优化策略（如位图索引扫描）
 */
bool
restriction_is_or_clause(RestrictInfo *restrictinfo)
{
    /* 检查restrictinfo结构的orclause字段是否非空
     * orclause字段存储了原始的OR表达式，当条件是OR子句时该字段被设置
     */
    if (restrictinfo->orclause != NULL)
        return true;  /* 是OR子句，返回true */
    else
        return false; /* 不是OR子句，返回false */
}

/*
 * restriction_is_securely_promotable
 *
 * 判断该限制条件是否可以“提前”执行，即在指定关系上的其他限制条件之前执行。
 *
 * 这段代码涉及 PostgreSQL 的行级安全（Row-Level Security, RLS）和安全屏障视图（Security Barrier Views）机制。
 * 它的核心目的是防止侧信道攻击（Side-channel attacks），
 * 即防止用户通过执行报错的函数或观察执行时间，推断出他们本不该看到的行的数据。
 *
 * 简单总结
 * 这个函数判断：能不能把这个过滤条件（restrictinfo）拿到更早的阶段去执行？
 * 如果返回 true：可以提前执行。说明这个条件是“安全”的，或者它本身就属于当前必须执行的安全检查层级。
 * 如果返回 false：不能提前执行。说明这个条件可能包含用户自定义的不安全函数，
 * 必须等更底层的安全检查（如 RLS 策略）先过滤完数据后，才能执行这个条件。
 */
bool
restriction_is_securely_promotable(RestrictInfo *restrictinfo,
                                   RelOptInfo *rel)
{
    /*
     * 条件 1: restrictinfo->security_level <= rel->baserestrict_min_security
     * 
     * 含义：如果当前条件的层级 <= 表上所有剩余限制条件的最小层级。
     * 解释：这意味着当前条件本身就是目前优先级最高（或同级）的安全检查之一，
     *       或者它比剩下的所有安全检查都更"外层"。
     *       在这种情况下，按顺序执行它没有问题，因为它没有试图跳过任何必须先执行的检查。
     */
    
    /*
     * 条件 2: restrictinfo->leakproof
     * 
     * 含义：如果这个条件是"防泄漏"的。
     * 解释：即使这个条件原本应该在 RLS 检查之后执行（比如它是用户写的 WHERE 子句），
     *       但因为它被证明是无害的（不会抛出带数据的错误），优化器可以为了性能
     *       把它"提升"（Promote）到 RLS 检查之前执行。
     *       这通常能利用索引快速过滤掉大量数据，从而提升性能。
     */

    if (restrictinfo->security_level <= rel->baserestrict_min_security ||
        restrictinfo->leakproof)
        return true;  // 可以安全地提前执行
    else
        return false; // 必须等待更底层的安全检查先执行
}

/*
 * get_actual_clauses
 *
 * Returns a list containing the bare clauses from 'restrictinfo_list'.
 *
 * This is only to be used in cases where none of the RestrictInfos can
 * be pseudoconstant clauses (for instance, it's OK on indexqual lists).
 */
List *
get_actual_clauses(List *restrictinfo_list)
{
	List	   *result = NIL;
	ListCell   *l;

	foreach(l, restrictinfo_list)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

		Assert(!rinfo->pseudoconstant);

		result = lappend(result, rinfo->clause);
	}
	return result;
}

/*
 * extract_actual_clauses
 *
 * 从 'restrictinfo_list' 中提取原始子句，根据 'pseudoconstant' 参数返回普通子句或伪常量子句。
 * 若 pseudoconstant = false，则返回普通子句；若为 true，则返回伪常量子句。
 *
 * 参数:
 * - restrictinfo_list: 要处理的RestrictInfo节点列表
 * - pseudoconstant: 布尔标志，指示要提取的子句类型
 *   - true: 提取伪常量子句
 *   - false: 提取普通子句
 *
 * 返回值:
 * - 返回符合条件的Clause节点列表
 */
List *
extract_actual_clauses(List *restrictinfo_list,
						   bool pseudoconstant)
{
	List	   *result = NIL;  // 初始化结果列表为空
	ListCell   *l;              // 用于遍历restrictinfo_list的列表指针

	// 遍历每个RestrictInfo节点
	foreach(l, restrictinfo_list)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);  // 获取当前节点

		// 根据pseudoconstant参数筛选子句
		if (rinfo->pseudoconstant == pseudoconstant)
			result = lappend(result, rinfo->clause);  // 将符合条件的子句添加到结果列表
	}
	return result;  // 返回筛选后的子句列表
}


/*
 * extract_actual_join_clauses
 *
 * 从'restrictinfo_list'中提取原始子句，将那些语义上匹配连接级别的子句与被下推的子句分开。
 * 伪常量子句会被排除在结果之外。
 *
 * 此函数仅用于外连接，因为对于普通连接我们不关心子句是否被下推。
 *
 * 参数:
 * - restrictinfo_list: 要处理的RestrictInfo节点列表
 * - joinrelids: 当前连接级别的关系ID集合
 * - joinquals: 输出参数，用于存储语义上属于当前连接级别的子句
 * - otherquals: 输出参数，用于存储被下推到当前连接级别的子句
 */
void
extract_actual_join_clauses(List *restrictinfo_list,
							Relids joinrelids,
							List **joinquals,
							List **otherquals)
{
	ListCell   *l;  // 用于遍历restrictinfo_list的列表指针

	// 初始化输出参数为空列表
	*joinquals = NIL;
	*otherquals = NIL;

	// 遍历每个RestrictInfo节点
	foreach(l, restrictinfo_list)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

		// 检查当前子句是否被下推到当前连接级别
		if (RINFO_IS_PUSHED_DOWN(rinfo, joinrelids))
		{
			// 如果不是伪常量子句，则将其添加到otherquals列表
			if (!rinfo->pseudoconstant)
				*otherquals = lappend(*otherquals, rinfo->clause);
		}
		else
		{
			// 对于属于当前连接级别的子句，确保它们不是伪常量
			/* joinquals shouldn't have been marked pseudoconstant */
			Assert(!rinfo->pseudoconstant);
			// 将子句添加到joinquals列表
			*joinquals = lappend(*joinquals, rinfo->clause);
		}
	}
}

/*
 * has_pseudoconstant_clauses
 *
 * Returns true if 'restrictinfo_list' includes pseudoconstant clauses.
 *
 * This is used when we determine whether to allow extensions to consider
 * pushing down joins in add_paths_to_joinrel().
 */
bool
has_pseudoconstant_clauses(PlannerInfo *root,
						   List *restrictinfo_list)
{
	ListCell   *l;

	/* No need to look if we know there are no pseudoconstants */
	if (!root->hasPseudoConstantQuals)
		return false;

	/* See if there are pseudoconstants in the RestrictInfo list */
	foreach(l, restrictinfo_list)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

		if (rinfo->pseudoconstant)
			return true;
	}
	return false;
}


/*
 * join_clause_is_movable_to
 *		Test whether a join clause is a safe candidate for parameterization
 *		of a scan on the specified base relation.
 *
 * A movable join clause is one that can safely be evaluated at a rel below
 * its normal semantic level (ie, its required_relids), if the values of
 * variables that it would need from other rels are provided.
 *
 * We insist that the clause actually reference the target relation; this
 * prevents undesirable movement of degenerate join clauses, and ensures
 * that there is a unique place that a clause can be moved down to.
 *
 * We cannot move an outer-join clause into the non-nullable side of its
 * outer join, as that would change the results (rows would be suppressed
 * rather than being null-extended).
 *
 * Also there must not be an outer join below the clause that would null the
 * Vars coming from the target relation.  Otherwise the clause might give
 * results different from what it would give at its normal semantic level.
 *
 * Also, the join clause must not use any relations that have LATERAL
 * references to the target relation, since we could not put such rels on
 * the outer side of a nestloop with the target relation.
 */
bool
join_clause_is_movable_to(RestrictInfo *rinfo, RelOptInfo *baserel)
{
	/* Clause must physically reference target rel */
	if (!bms_is_member(baserel->relid, rinfo->clause_relids))
		return false;

	/* Cannot move an outer-join clause into the join's outer side */
	if (bms_is_member(baserel->relid, rinfo->outer_relids))
		return false;

	/* Target rel must not be nullable below the clause */
	if (bms_is_member(baserel->relid, rinfo->nullable_relids))
		return false;

	/* Clause must not use any rels with LATERAL references to this rel */
	if (bms_overlap(baserel->lateral_referencers, rinfo->clause_relids))
		return false;

	return true;
}

/*
 * join_clause_is_movable_into
 *		Test whether a join clause is movable and can be evaluated within
 *		the current join context.
 *
 * currentrelids: the relids of the proposed evaluation location
 * current_and_outer: the union of currentrelids and the required_outer
 *		relids (parameterization's outer relations)
 *
 * The API would be a bit clearer if we passed the current relids and the
 * outer relids separately and did bms_union internally; but since most
 * callers need to apply this function to multiple clauses, we make the
 * caller perform the union.
 *
 * Obviously, the clause must only refer to Vars available from the current
 * relation plus the outer rels.  We also check that it does reference at
 * least one current Var, ensuring that the clause will be pushed down to
 * a unique place in a parameterized join tree.  And we check that we're
 * not pushing the clause into its outer-join outer side, nor down into
 * a lower outer join's inner side.
 *
 * The check about pushing a clause down into a lower outer join's inner side
 * is only approximate; it sometimes returns "false" when actually it would
 * be safe to use the clause here because we're still above the outer join
 * in question.  This is okay as long as the answers at different join levels
 * are consistent: it just means we might sometimes fail to push a clause as
 * far down as it could safely be pushed.  It's unclear whether it would be
 * worthwhile to do this more precisely.  (But if it's ever fixed to be
 * exactly accurate, there's an Assert in get_joinrel_parampathinfo() that
 * should be re-enabled.)
 *
 * There's no check here equivalent to join_clause_is_movable_to's test on
 * lateral_referencers.  We assume the caller wouldn't be inquiring unless
 * it'd verified that the proposed outer rels don't have lateral references
 * to the current rel(s).  (If we are considering join paths with the outer
 * rels on the outside and the current rels on the inside, then this should
 * have been checked at the outset of such consideration; see join_is_legal
 * and the path parameterization checks in joinpath.c.)  On the other hand,
 * in join_clause_is_movable_to we are asking whether the clause could be
 * moved for some valid set of outer rels, so we don't have the benefit of
 * relying on prior checks for lateral-reference validity.
 *
 * Note: if this returns true, it means that the clause could be moved to
 * this join relation, but that doesn't mean that this is the lowest join
 * it could be moved to.  Caller may need to make additional calls to verify
 * that this doesn't succeed on either of the inputs of a proposed join.
 *
 * Note: get_joinrel_parampathinfo depends on the fact that if
 * current_and_outer is NULL, this function will always return false
 * (since one or the other of the first two tests must fail).
 */
bool
join_clause_is_movable_into(RestrictInfo *rinfo,
							Relids currentrelids,
							Relids current_and_outer)
{
	/* Clause must be evaluable given available context */
	if (!bms_is_subset(rinfo->clause_relids, current_and_outer))
		return false;

	/* Clause must physically reference at least one target rel */
	if (!bms_overlap(currentrelids, rinfo->clause_relids))
		return false;

	/* Cannot move an outer-join clause into the join's outer side */
	if (bms_overlap(currentrelids, rinfo->outer_relids))
		return false;

	/*
	 * Target rel(s) must not be nullable below the clause.  This is
	 * approximate, in the safe direction, because the current join might be
	 * above the join where the nulling would happen, in which case the clause
	 * would work correctly here.  But we don't have enough info to be sure.
	 */
	if (bms_overlap(currentrelids, rinfo->nullable_relids))
		return false;

	return true;
}