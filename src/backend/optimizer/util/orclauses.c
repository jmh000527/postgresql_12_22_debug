/*-------------------------------------------------------------------------
 *
 * orclauses.c
 *	  Routines to extract restriction OR clauses from join OR clauses
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/orclauses.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/orclauses.h"
#include "optimizer/restrictinfo.h"


/* source-code-compatibility hacks for pull_varnos() API change */
#define make_restrictinfo(a,b,c,d,e,f,g,h,i) make_restrictinfo_new(a,b,c,d,e,f,g,h,i)

static bool is_safe_restriction_clause_for(RestrictInfo *rinfo, RelOptInfo *rel);
static Expr *extract_or_clause(RestrictInfo *or_rinfo, RelOptInfo *rel);
static void consider_new_or_clause(PlannerInfo *root, RelOptInfo *rel,
								   Expr *orclause, RestrictInfo *join_or_rinfo);


/*
 * extract_restriction_or_clauses
 *	  Examine join OR-of-AND clauses to see if any useful restriction OR
 *	  clauses can be extracted.  If so, add them to the query.
 *
 * Although a join clause must reference multiple relations overall,
 * an OR of ANDs clause might contain sub-clauses that reference just one
 * relation and can be used to build a restriction clause for that rel.
 * For example consider
 *		WHERE ((a.x = 42 AND b.y = 43) OR (a.x = 44 AND b.z = 45));
 * We can transform this into
 *		WHERE ((a.x = 42 AND b.y = 43) OR (a.x = 44 AND b.z = 45))
 *			AND (a.x = 42 OR a.x = 44)
 *			AND (b.y = 43 OR b.z = 45);
 * which allows the latter clauses to be applied during the scans of a and b,
 * perhaps as index qualifications, and in any case reducing the number of
 * rows arriving at the join.  In essence this is a partial transformation to
 * CNF (AND of ORs format).  It is not complete, however, because we do not
 * unravel the original OR --- doing so would usually bloat the qualification
 * expression to little gain.
 *
 * The added quals are partially redundant with the original OR, and therefore
 * would cause the size of the joinrel to be underestimated when it is finally
 * formed.  (This would be true of a full transformation to CNF as well; the
 * fault is not really in the transformation, but in clauselist_selectivity's
 * inability to recognize redundant conditions.)  We can compensate for this
 * redundancy by changing the cached selectivity of the original OR clause,
 * canceling out the (valid) reduction in the estimated sizes of the base
 * relations so that the estimated joinrel size remains the same.  This is
 * a MAJOR HACK: it depends on the fact that clause selectivities are cached
 * and on the fact that the same RestrictInfo node will appear in every
 * joininfo list that might be used when the joinrel is formed.
 * And it doesn't work in cases where the size estimation is nonlinear
 * (i.e., outer and IN joins).  But it beats not doing anything.
 *
 * We examine each base relation to see if join clauses associated with it
 * contain extractable restriction conditions.  If so, add those conditions
 * to the rel's baserestrictinfo and update the cached selectivities of the
 * join clauses.  Note that the same join clause will be examined afresh
 * from the point of view of each baserel that participates in it, so its
 * cached selectivity may get updated multiple times.
 */
void
extract_restriction_or_clauses(PlannerInfo *root)
{
	Index		rti;

	/* Examine each baserel for potential join OR clauses */
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *rel = root->simple_rel_array[rti];
		ListCell   *lc;

		/* there may be empty slots corresponding to non-baserel RTEs */
		if (rel == NULL)
			continue;

		Assert(rel->relid == rti);	/* sanity check on array */

		/* ignore RTEs that are "other rels" */
		if (rel->reloptkind != RELOPT_BASEREL)
			continue;

		/*
		 * Find potentially interesting OR joinclauses.  We can use any
		 * joinclause that is considered safe to move to this rel by the
		 * parameterized-path machinery, even though what we are going to do
		 * with it is not exactly a parameterized path.
		 *
		 * However, it seems best to ignore clauses that have been marked
		 * redundant (by setting norm_selec > 1).  That likely can't happen
		 * for OR clauses, but let's be safe.
		 */
		foreach(lc, rel->joininfo)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

			if (restriction_is_or_clause(rinfo) &&
				join_clause_is_movable_to(rinfo, rel) &&
				rinfo->norm_selec <= 1)
			{
				/* Try to extract a qual for this rel only */
				Expr	   *orclause = extract_or_clause(rinfo, rel);

				/*
				 * If successful, decide whether we want to use the clause,
				 * and insert it into the rel's restrictinfo list if so.
				 */
				if (orclause)
					consider_new_or_clause(root, rel, orclause, rinfo);
			}
		}
	}
}

/*
 * Is the given primitive (non-OR) RestrictInfo safe to move to the rel?
 */
static bool
is_safe_restriction_clause_for(RestrictInfo *rinfo, RelOptInfo *rel)
{
	/*
	 * We want clauses that mention the rel, and only the rel.  So in
	 * particular pseudoconstant clauses can be rejected quickly.  Then check
	 * the clause's Var membership.
	 */
	if (rinfo->pseudoconstant)
		return false;
	if (!bms_equal(rinfo->clause_relids, rel->relids))
		return false;

	/* We don't want extra evaluations of any volatile functions */
	if (contain_volatile_functions((Node *) rinfo->clause))
		return false;

	return true;
}

/*
 * Try to extract a restriction clause mentioning only "rel" from the given
 * join OR-clause.
 *
 * We must be able to extract at least one qual for this rel from each of
 * the arms of the OR, else we can't use it.
 *
 * Returns an OR clause (not a RestrictInfo!) pertaining to rel, or NULL
 * if no OR clause could be extracted.
 */
static Expr *
extract_or_clause(RestrictInfo *or_rinfo, RelOptInfo *rel)
{
	List	   *clauselist = NIL;
	ListCell   *lc;

	/*
	 * Scan each arm of the input OR clause.  Notice we descend into
	 * or_rinfo->orclause, which has RestrictInfo nodes embedded below the
	 * toplevel OR/AND structure.  This is useful because we can use the info
	 * in those nodes to make is_safe_restriction_clause_for()'s checks
	 * cheaper.  We'll strip those nodes from the returned tree, though,
	 * meaning that fresh ones will be built if the clause is accepted as a
	 * restriction clause.  This might seem wasteful --- couldn't we re-use
	 * the existing RestrictInfos?	But that'd require assuming that
	 * selectivity and other cached data is computed exactly the same way for
	 * a restriction clause as for a join clause, which seems undesirable.
	 */
	Assert(is_orclause(or_rinfo->orclause));
	foreach(lc, ((BoolExpr *) or_rinfo->orclause)->args)
	{
		Node	   *orarg = (Node *) lfirst(lc);
		List	   *subclauses = NIL;
		Node	   *subclause;

		/* OR arguments should be ANDs or sub-RestrictInfos */
		if (is_andclause(orarg))
		{
			List	   *andargs = ((BoolExpr *) orarg)->args;
			ListCell   *lc2;

			foreach(lc2, andargs)
			{
				RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);

				if (restriction_is_or_clause(rinfo))
				{
					/*
					 * Recurse to deal with nested OR.  Note we *must* recurse
					 * here, this isn't just overly-tense optimization: we
					 * have to descend far enough to find and strip all
					 * RestrictInfos in the expression.
					 */
					Expr	   *suborclause;

					suborclause = extract_or_clause(rinfo, rel);
					if (suborclause)
						subclauses = lappend(subclauses, suborclause);
				}
				else if (is_safe_restriction_clause_for(rinfo, rel))
					subclauses = lappend(subclauses, rinfo->clause);
			}
		}
		else
		{
			RestrictInfo *rinfo = castNode(RestrictInfo, orarg);

			Assert(!restriction_is_or_clause(rinfo));
			if (is_safe_restriction_clause_for(rinfo, rel))
				subclauses = lappend(subclauses, rinfo->clause);
		}

		/*
		 * If nothing could be extracted from this arm, we can't do anything
		 * with this OR clause.
		 */
		if (subclauses == NIL)
			return NULL;

		/*
		 * OK, add subclause(s) to the result OR.  If we found more than one,
		 * we need an AND node.  But if we found only one, and it is itself an
		 * OR node, add its subclauses to the result instead; this is needed
		 * to preserve AND/OR flatness (ie, no OR directly underneath OR).
		 */
		subclause = (Node *) make_ands_explicit(subclauses);
		if (is_orclause(subclause))
			clauselist = list_concat(clauselist,
									 list_copy(((BoolExpr *) subclause)->args));
		else
			clauselist = lappend(clauselist, subclause);
	}

	/*
	 * If we got a restriction clause from every arm, wrap them up in an OR
	 * node.  (In theory the OR node might be unnecessary, if there was only
	 * one arm --- but then the input OR node was also redundant.)
	 */
	if (clauselist != NIL)
		return make_orclause(clauselist);
	return NULL;
}

/*
 * 考虑一个成功提取的限制条件OR子句是否值得使用。如果值得，将其添加到
 * 规划器的数据结构中，并调整原始连接子句(join_or_rinfo)以进行补偿。
 * 此函数是PostgreSQL查询优化器中OR子句优化的关键部分，用于提取可下推
 * 到基础关系的OR条件，从而提高查询性能。
 */
static void
consider_new_or_clause(PlannerInfo *root,      // 规划器信息指针
                       RelOptInfo *rel,        // 关系优化信息指针（目标关系）
                       Expr *orclause,         // 提取出的OR子句表达式
                       RestrictInfo *join_or_rinfo)  // 原始连接OR子句的限制信息
{
	RestrictInfo *or_rinfo;      // 新创建的OR子句限制信息
	Selectivity or_selec,        // OR子句的选择性（影响行数的比例）
			  orig_selec;       // 原始连接子句的选择性

	/*
	 * 从新的OR子句构建RestrictInfo结构。我们可以假设它作为基础
	 * 限制子句是有效的，因为前面的代码已经验证了它只引用了单个关系。
	 * 
	 * 参数说明：
	 * - root: 规划器信息
	 * - orclause: 要处理的OR表达式
	 * - true: 标记为可被安全下推
	 * - false: 不是一个推入子查询的表达式
	 * - false: 不是一个被延迟评估的表达式
	 * - 安全级别: 继承自原始连接子句
	 * - 其余参数: NULL（不需要额外信息）
	 */
	or_rinfo = make_restrictinfo(root,
					     orclause,
					     true,
					     false,
					     false,
					     join_or_rinfo->security_level,
					     NULL,
					     NULL,
					     NULL);

	/*
	 * 估计OR子句的选择性。选择性表示子句会选择多少比例的行，
	 * 值越接近1表示选择越多的行。在RestrictInfo表示上进行计算可以
	 * 缓存结果，避免后续重复计算。
	 * 
	 * 参数说明：
	 * - root: 规划器信息
	 * - (Node *)or_rinfo: 转换为节点指针的限制信息
	 * - 0: 未使用的参数
	 * - JOIN_INNER: 内部连接语义
	 * - NULL: 无特殊连接信息
	 */
	or_selec = clause_selectivity(root, (Node *) or_rinfo,
					      0, JOIN_INNER, NULL);

	/*
	 * 只有当子句能有效过滤掉基础关系中的大量行时，才值得添加到查询中。
	 * 否则，它只会导致重复计算（因为在连接形成时我们仍需要检查原始OR子句）。
	 * 这里设定了一个阈值：选择性大于0.9（即过滤掉少于10%的行）时，
	 * 不添加该子句。这个阈值是经验性的。
	 */
	if (or_selec > 0.9)
		return;                    // 过滤效果不佳，放弃添加

	/*
	 * 将OR子句添加到关系的限制子句列表中。这使优化器可以考虑在
	 * 扫描基础关系时应用此过滤条件，减少需要处理的行数。
	 */
	rel->baserestrictinfo = lappend(rel->baserestrictinfo, or_rinfo);
	// 更新关系的最小安全级别，确保安全策略得到正确应用
	rel->baserestrict_min_security = Min(rel->baserestrict_min_security,
						    or_rinfo->security_level);

	/*
	 * 调整原始连接OR子句的缓存选择性，以补偿已添加的（冗余的）低级条件。
	 * 这确保连接关系获得与没有这些优化时大致相同的行数估计。
	 * 
	 * 注意事项：
	 * 1. 这依赖于选择性将保持缓存的假设
	 * 2. 我们只调整norm_selec（JOIN_INNER语义的缓存选择性），
	 *    即使连接子句可能是外部连接子句，因为：
	 *    - 难以在此处识别相关的SpecialJoinInfo
	 *    - 由于线性假设可能不成立，特别是当"rel"位于可空侧时
	 *    - 此时连接大小的计算与"rel"大小的关系非常非线性
	 */
	if (or_selec > 0)  // 避免除以零的情况
	{
		SpecialJoinInfo sjinfo;  // 特殊连接信息结构

		/*
		 * 为JOIN_INNER语义创建一个SpecialJoinInfo。
		 * 这里手动构建了连接信息，与costsize.c中的approx_tuple_count()函数类似。
		 */
		sjinfo.type = T_SpecialJoinInfo;  // 标记结构类型
		// 计算连接左侧关系ID（原始连接子句关系ID减去当前关系ID）
		sjinfo.min_lefthand = bms_difference(join_or_rinfo->clause_relids,
							    rel->relids);
		sjinfo.min_righthand = rel->relids;  // 右侧是当前关系
		// 同义词关系集与最小关系集相同（简化处理）
		sjinfo.syn_lefthand = sjinfo.min_lefthand;
		sjinfo.syn_righthand = sjinfo.min_righthand;
		sjinfo.jointype = JOIN_INNER;  // 设置为内部连接类型
		/* 以下字段未初始化，因为在此上下文中不需要它们 */
		sjinfo.lhs_strict = false;
		sjinfo.delay_upper_joins = false;
		sjinfo.semi_can_btree = false;
		sjinfo.semi_can_hash = false;
		sjinfo.semi_operators = NIL;
		sjinfo.semi_rhs_exprs = NIL;

		/* 计算原始连接子句在内部连接语义下的选择性 */
		orig_selec = clause_selectivity(root, (Node *) join_or_rinfo,
						 0, JOIN_INNER, &sjinfo);

		/* 调整缓存的选择性，使连接大小保持不变 */
		// 将原始选择性除以OR子句的选择性，补偿已应用的过滤
		join_or_rinfo->norm_selec = orig_selec / or_selec;
		/* 确保结果在合理范围内，特别是不能超过1（表示无过滤效果） */
		if (join_or_rinfo->norm_selec > 1)
			join_or_rinfo->norm_selec = 1;
		/* 如上所述，我们不修改outer_selec */
	}
}

