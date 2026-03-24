/*-------------------------------------------------------------------------
 *
 * prepjointree.c
 *	  Planner preprocessing for subqueries and join tree manipulation.
 *
 * NOTE: the intended sequence for invoking these operations is
 *		replace_empty_jointree
 *		pull_up_sublinks
 *		inline_set_returning_functions
 *		pull_up_subqueries
 *		flatten_simple_union_all
 *		do expression preprocessing (including flattening JOIN alias vars)
 *		reduce_outer_joins
 *		remove_useless_result_rtes
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/prep/prepjointree.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/placeholder.h"
#include "optimizer/prep.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"


/* source-code-compatibility hacks for pull_varnos() API change */
#define pull_varnos(a,b) pull_varnos_new(a,b)
#define pull_varnos_of_level(a,b,c) pull_varnos_of_level_new(a,b,c)

typedef struct pullup_replace_vars_context
{
	PlannerInfo *root;
	List	   *targetlist;		/* tlist of subquery being pulled up */
	RangeTblEntry *target_rte;	/* RTE of subquery */
	Relids		relids;			/* relids within subquery, as numbered after
								 * pullup (set only if target_rte->lateral) */
	bool	   *outer_hasSubLinks;	/* -> outer query's hasSubLinks */
	int			varno;			/* varno of subquery */
	bool		need_phvs;		/* do we need PlaceHolderVars? */
	bool		wrap_non_vars;	/* do we need 'em on *all* non-Vars? */
	Node	  **rv_cache;		/* cache for results with PHVs */
} pullup_replace_vars_context;

/*
 * reduce_outer_joins_state 结构体
 * 用于在外连接简化(reduce_outer_joins)的第一遍遍历中收集每个连接子树的信息。
 *
 * - relids: 当前子树包含的所有基表的 RT index 集合（Relids 类型为位图集合）。
 * - contains_outer: 当前子树是否包含外连接（true 表示包含至少一个外连接）。
 * - sub_states: 子树各个分支的状态列表（每个元素为 reduce_outer_joins_state*）。
 */
typedef struct reduce_outer_joins_state
{
	Relids		relids;			/* 当前子树包含的基表 RT index 集合 */
	bool		contains_outer; /* 当前子树是否包含外连接 */
	List	   *sub_states;		/* 子树各分支的状态列表 */
} reduce_outer_joins_state;

static Node *pull_up_sublinks_jointree_recurse(PlannerInfo *root, Node *jtnode,
											   Relids *relids);
static Node *pull_up_sublinks_qual_recurse(PlannerInfo *root, Node *node,
										   Node **jtlink1, Relids available_rels1,
										   Node **jtlink2, Relids available_rels2);
static Node *pull_up_subqueries_recurse(PlannerInfo *root, Node *jtnode,
										JoinExpr *lowest_outer_join,
										JoinExpr *lowest_nulling_outer_join,
										AppendRelInfo *containing_appendrel);
static Node *pull_up_simple_subquery(PlannerInfo *root, Node *jtnode,
									 RangeTblEntry *rte,
									 JoinExpr *lowest_outer_join,
									 JoinExpr *lowest_nulling_outer_join,
									 AppendRelInfo *containing_appendrel);
static Node *pull_up_simple_union_all(PlannerInfo *root, Node *jtnode,
									  RangeTblEntry *rte);
static void pull_up_union_leaf_queries(Node *setOp, PlannerInfo *root,
									   int parentRTindex, Query *setOpQuery,
									   int childRToffset);
static void make_setop_translation_list(Query *query, Index newvarno,
										List **translated_vars);
static bool is_simple_subquery(PlannerInfo *root, Query *subquery,
							   RangeTblEntry *rte,
							   JoinExpr *lowest_outer_join);
static Node *pull_up_simple_values(PlannerInfo *root, Node *jtnode,
								   RangeTblEntry *rte);
static bool is_simple_values(PlannerInfo *root, RangeTblEntry *rte);
static bool is_simple_union_all(Query *subquery);
static bool is_simple_union_all_recurse(Node *setOp, Query *setOpQuery,
										List *colTypes);
static bool is_safe_append_member(Query *subquery);
static bool jointree_contains_lateral_outer_refs(PlannerInfo *root,
												 Node *jtnode, bool restricted,
												 Relids safe_upper_varnos);
static void replace_vars_in_jointree(Node *jtnode,
									 pullup_replace_vars_context *context,
									 JoinExpr *lowest_nulling_outer_join);
static Node *pullup_replace_vars(Node *expr,
								 pullup_replace_vars_context *context);
static Node *pullup_replace_vars_callback(Var *var,
										  replace_rte_variables_context *context);
static Query *pullup_replace_vars_subquery(Query *query,
										   pullup_replace_vars_context *context);
static reduce_outer_joins_state *reduce_outer_joins_pass1(Node *jtnode);
static void reduce_outer_joins_pass2(Node *jtnode,
									 reduce_outer_joins_state *state,
									 PlannerInfo *root,
									 Relids nonnullable_rels,
									 List *nonnullable_vars,
									 List *forced_null_vars);
static Node *remove_useless_results_recurse(PlannerInfo *root, Node *jtnode);
static int	get_result_relid(PlannerInfo *root, Node *jtnode);
static void remove_result_refs(PlannerInfo *root, int varno, Node *newjtloc);
static bool find_dependent_phvs(PlannerInfo *root, int varno);
static bool find_dependent_phvs_in_jointree(PlannerInfo *root,
											Node *node, int varno);
static void substitute_phv_relids(Node *node,
								  int varno, Relids subrelids);
static void fix_append_rel_relids(List *append_rel_list, int varno,
								  Relids subrelids);
static Node *find_jointree_node_for_rel(Node *jtnode, int relid);


/*
 * replace_empty_jointree
 *		If the Query's jointree is empty, replace it with a dummy RTE_RESULT
 *		relation.
 *
 * By doing this, we can avoid a bunch of corner cases that formerly existed
 * for SELECTs with omitted FROM clauses.  An example is that a subquery
 * with empty jointree previously could not be pulled up, because that would
 * have resulted in an empty relid set, making the subquery not uniquely
 * identifiable for join or PlaceHolderVar processing.
 *
 * Unlike most other functions in this file, this function doesn't recurse;
 * we rely on other processing to invoke it on sub-queries at suitable times.
 */
void
replace_empty_jointree(Query *parse)
{
	RangeTblEntry *rte;
	Index		rti;
	RangeTblRef *rtr;

	/* Nothing to do if jointree is already nonempty */
	if (parse->jointree->fromlist != NIL)
		return;

	/* We mustn't change it in the top level of a setop tree, either */
	if (parse->setOperations)
		return;

	/* Create suitable RTE */
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RESULT;
	rte->eref = makeAlias("*RESULT*", NIL);

	/* Add it to rangetable */
	parse->rtable = lappend(parse->rtable, rte);
	rti = list_length(parse->rtable);

	/* And jam a reference into the jointree */
	rtr = makeNode(RangeTblRef);
	rtr->rtindex = rti;
	parse->jointree->fromlist = list_make1(rtr);
}

/*
 * pull_up_sublinks
 *		尝试将 ANY 和 EXISTS 类型的 SubLink 上拉为半连接或反半连接处理。
 *
 * 例如 "foo op ANY (sub-SELECT)" 这种子查询，可以通过将 sub-SELECT 上拉为
 * 一个 rangetable 条目，并将比较条件作为半连接的条件来处理。
 * 但这种优化仅在 WHERE 或 JOIN/ON 子句的顶层有效，因为在涉及 NULL 输入的情况下，
 * 无法区分 ANY 应返回 FALSE 还是 NULL。
 * 并且在外连接的 ON 子句中，只有当子查询是退化的（即只引用连接的可空侧）时才允许，
 * 这种情况下可以将半连接下推到连接的可空侧。如果子查询引用了不可空侧的变量，
 * 则必须在外连接中进行评估，这会变得非常复杂。
 *
 * 类似地，EXISTS 和 NOT EXISTS 子句也可以在类似条件下通过上拉子查询，
 * 创建半连接或反半连接来处理。
 *
 * 本函数会搜索这些子句，并在发现时进行必要的语法树转换。
 *
 * 本函数必须在 preprocess_expression() 之前运行，因此 quals 子句尚未被
 * 转换为隐式 AND 格式，也不保证是 AND/OR 平展的。因此需要递归搜索显式 AND 子句，
 * 一旦遇到非 AND 项则停止。
 */
void
pull_up_sublinks(PlannerInfo *root)
{
	Node	   *jtnode;
	Relids		relids;

	/*
	 * 递归遍历连接树以提升子链接（Sublinks）。
	 *
	 * 参数:
	 *   root    - 优化器的 PlannerInfo 结构体，包含查询的上下文信息。
	 *   (Node *) root->parse->jointree - 查询的连接树（jointree），作为递归的起始节点。可能分析为 FromExpr、JoinExpr 或 RangeTblRef。
	 *   &relids - 输出参数。指向 relids 的指针，用于收集涉及的 relid 集合。
	 */
	/* 开始递归遍历连接树 */
	jtnode = pull_up_sublinks_jointree_recurse(root,
											   (Node *) root->parse->jointree,
											   &relids);

	/*
	 * root->parse->jointree 必须始终为 FromExpr，
	 * 如果递归结果是单独的 RangeTblRef 或 JoinExpr，则插入一个虚拟的 FromExpr。
	 */
	if (IsA(jtnode, FromExpr))
		root->parse->jointree = (FromExpr *) jtnode;
	else
		root->parse->jointree = makeFromExpr(list_make1(jtnode), NULL);
}

/*
 * pull_up_sublinks_jointree_recurse
 *		递归遍历连接树节点以提升子链接（Sublinks）。
 *
 * 参数:
 *	  root: 规划器信息。
 *	  jtnode: 当前连接树节点。
 *	  relids: [输出参数] 返回该子树包含的所有基表 relids。
 *
 * 返回值:
 *	  可能被修改后的连接树节点（例如，如果在该节点下方提升了子查询，
 *	  该节点可能被包装在新的 JoinExpr 中）。
 *
 * 核心逻辑:
 *	  1. 递归深入连接树的所有分支。
 *	  2. 计算每个子树包含的 relids。
 *	  3. 调用 pull_up_sublinks_qual_recurse 处理当前节点的 quals（WHERE 或 ON 条件）。
 *		 - 如果在 quals 中发现了可提升的 SubLink（如 EXISTS/ANY），
 *		   会构建新的 Semi/Anti Join 节点，并将其插入到连接树的合适位置。
 *		 - "合适位置"由传入 pull_up_sublinks_qual_recurse 的 jtlink 指针决定。
 */
static Node *
pull_up_sublinks_jointree_recurse(PlannerInfo *root, Node *jtnode,
								  Relids *relids)
{
	/* 由于本函数递归调用，可能导致栈溢出，需检查栈深度 */
	check_stack_depth();

	if (jtnode == NULL)
	{
		*relids = NULL;
	}
	else if (IsA(jtnode, RangeTblRef))	/* 叶子节点：基表引用 */
	{
		int	varno = ((RangeTblRef *) jtnode)->rtindex;

		*relids = bms_make_singleton(varno);
		/* 叶子节点无法包含 quals，所以我们只需返回它 */
	}
	else if (IsA(jtnode, FromExpr))
	{
		/* 
		 * 处理 FromExpr 节点（对应 SQL 中的 FROM ... WHERE ...）。
		 * FromExpr 的 quals 就是 WHERE 子句。
		 */
		FromExpr   *f = (FromExpr *) jtnode;
		List	   *newfromlist = NIL;	/* 重构后的 FROM 列表 */
		Relids		frelids = NULL;		/* 当前节点下所有表的 relids 集合 */
		FromExpr   *newf;
		Node	   *jtlink;				/* 指向当前树顶部的指针（可能会被插入的 Join 改变） */
		ListCell   *l;

		/* 
		 * 自底向上处理：先递归处理所有子节点。
		 * 必须先做这个，因为子节点可能会变成 JoinExpr 堆栈。
		 */
		foreach(l, f->fromlist)
		{
			Node	   *newchild;
			Relids		childrelids;

			newchild = pull_up_sublinks_jointree_recurse(root,
														 lfirst(l),
														 &childrelids);
			newfromlist = lappend(newfromlist, newchild);
			frelids = bms_join(frelids, childrelids);
		}

		/* 构造新的 FromExpr 节点，包含处理后的子节点列表 */
		newf = makeFromExpr(newfromlist, NULL);
		
		/* 
		 * 初始化 jtlink 指向这个新的 FromExpr。
		 * 如果在处理 quals 时发现了 SubLink，pull_up_sublinks_qual_recurse 
		 * 会修改 jtlink，让它指向新生成的 JoinExpr（而原来的 FromExpr 会变成其子节点）。
		 */
		jtlink = (Node *) newf;

		/* 
		 * 处理 WHERE 子句 (quals)。
		 * 在 WHERE 子句中的 SubLink 可以与 FromExpr 下的任何表进行连接（Semi/Anti Join），
		 * 因此我们将 frelids (所有子表的 relids) 作为 available_rels 传递。
		 */
		newf->quals = pull_up_sublinks_qual_recurse(root, f->quals,
													&jtlink, frelids,
													NULL, NULL);

		/*
		 * 更新 relids 输出。
		 * 注意：我们不需要包含新生成的 Semi-Join 里的子查询表的 relids，
		 * 因为上层的 quals 不应该引用它们（SubLink 上拉后对上层是透明的）。
		 */
		*relids = frelids;
		jtnode = jtlink;
	}
	else if (IsA(jtnode, JoinExpr))
	{
		/* 处理 JoinExpr 节点（显式的 JOIN ... ON ...） */
		JoinExpr   *j;
		Relids		leftrelids;
		Relids		rightrelids;
		Node	   *jtlink;

		/* 复制 JoinExpr 节点以便修改 */
		j = (JoinExpr *) palloc(sizeof(JoinExpr));
		memcpy(j, jtnode, sizeof(JoinExpr));
		jtlink = (Node *) j;

		/* 递归处理左右子树 */
		j->larg = pull_up_sublinks_jointree_recurse(root, j->larg,
													&leftrelids);
		j->rarg = pull_up_sublinks_jointree_recurse(root, j->rarg,
													&rightrelids);

		/*
		 * 处理 JOIN 的 ON 子句 (quals)。
		 * 根据连接类型，SubLink 上拉的位置和可用关系集有很大不同。
		 */
		switch (j->jointype)
		{
			case JOIN_INNER:
				/* 
				 * INNER JOIN: 等同于 FromExpr 的情况。
				 * SubLink 可以放在这个 Join 节点之上，并且可以引用左右两侧的表。
				 * 新生成的 SemiJoin 将会包裹当前的 join 节点。
				 */
				j->quals = pull_up_sublinks_qual_recurse(root, j->quals,
														 &jtlink,
														 bms_union(leftrelids,
																   rightrelids),
														 NULL, NULL);
				break;
			case JOIN_LEFT:
				/* 
				 * LEFT JOIN: 
				 * ON 子句中的 SubLink 只能引用右侧表的列（或者是常量/左侧列，但必须视为在右侧上下文中）。
				 * 生成的 SemiJoin 必须插入到左连接的右侧子树 (j->rarg) 上方，
				 * 而不能在整个左连接上方（否则会破坏左连接的 NULL 生成语义）。
				 * 因此传递 &j->rarg 作为插入点，rightrelids 作为可用关系。
				 */
				j->quals = pull_up_sublinks_qual_recurse(root, j->quals,
														 &j->rarg,
														 rightrelids,
														 NULL, NULL);
				break;
			case JOIN_FULL:
				/* 
				 * FULL JOIN: 
				 * 两侧都可能产生 NULL，非常复杂。目前不支持在这里提升 SubLink。
				 * 它们将被保留为 SubPlan（执行时的过滤器）。
				 */
				break;
			case JOIN_RIGHT:
				/* 
				 * RIGHT JOIN: 与 LEFT JOIN 对称。
				 * 必须插入到左侧子树 (j->larg) 上方。
				 */
				j->quals = pull_up_sublinks_qual_recurse(root, j->quals,
														 &j->larg,
														 leftrelids,
														 NULL, NULL);
				break;
			default:
				elog(ERROR, "unrecognized join type: %d",
					 (int) j->jointype);
				break;
		}

		/*
		 * 计算当前子树的 relids。
		 * 如果 JoinExpr 有别名 (rtindex)，必须包含它，防止上层错误判断引用可行性。
		 */
		*relids = bms_join(leftrelids, rightrelids);
		if (j->rtindex)
			*relids = bms_add_member(*relids, j->rtindex);
		jtnode = jtlink;
	}
	else
		elog(ERROR, "unrecognized join type: %d",
			 (int) nodeTag(jtnode));
	return jtnode;
}

/*
 * pull_up_sublinks_qual_recurse
 *		递归遍历顶层 qual 节点以实现 pull_up_sublinks() 的逻辑。
 *
 * 核心逻辑：
 * 在 WHERE 或 JOIN/ON 子句中扫描 SubLink（如 IN, EXISTS），如果条件允许，
 * 将其转化为 Semi Join (半连接) 或 Anti Join (反半连接)，并以此替换原来的 SubLink 表达式。
 *
 * 这种转换通常比针对每行扫描执行子查询要高效得多。
 *
 * 参数:
 *	  root, node: 规划器信息和当前表达式节点。
 *	  jtlink1/available_rels1: 指向连接树的一侧（通常是当前正在处理的关系层级）。
 *	  jtlink2/available_rels2: 指向连接树的另一侧（如果是双侧 Join）。
 *
 * --- 下面是几种关键的子链接类型转换示例 ---
 *
 * 1. ANY_SUBLINK (IN 子查询) -> Semi Join
 *    原始 SQL:
 *        SELECT * FROM t1 WHERE t1.x IN (SELECT t2.y FROM t2);
 *    内部 SubLink:
 *        (x = ANY (SELECT y FROM t2))
 *    转换后:
 *        SELECT * FROM t1 Semi Join t2 ON t1.x = t2.y;
 *
 * 2. EXISTS_SUBLINK (EXISTS 子查询) -> Semi Join
 *    原始 SQL:
 *        SELECT * FROM t1 WHERE EXISTS (SELECT 1 FROM t2 WHERE t1.x = t2.y);
 *    内部 SubLink:
 *        EXISTS(SELECT 1 FROM t2 WHERE t1.x = t2.y)
 *    转换后:
 *        SELECT * FROM t1 Semi Join t2 ON t1.x = t2.y;
 *
 * 3. NOT EXISTS (反 EXISTS) -> Anti Join
 *    原始 SQL:
 *        SELECT * FROM t1 WHERE NOT EXISTS (SELECT 1 FROM t2 WHERE t1.x = t2.y);
 *    内部 SubLink:
 *        NOT(EXISTS(...))
 *    转换后:
 *        SELECT * FROM t1 Anti Join t2 ON t1.x = t2.y;
 *
 * 返回值：
 *	  返回处理后的表达式树。如果 SubLink 被完全转化为 Join，
 *    则原位置的过滤条件被移除（返回 NULL），因为它已经变成了 Join 的连接条件。
 */
static Node *
pull_up_sublinks_qual_recurse(PlannerInfo *root, Node *node,
							  Node **jtlink1, Relids available_rels1,
							  Node **jtlink2, Relids available_rels2)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, SubLink))
	{
		SubLink    *sublink = (SubLink *) node;
		JoinExpr   *j;
		Relids		child_rels;

		/*
		 * 情况 1: 处理 ANY 或 EXISTS 类型的子链接
		 * 目标：尝试转换为 Semi Join (半连接)
		 */
		if (sublink->subLinkType == ANY_SUBLINK)
		{
			/*
			 * 尝试针对 link1 侧的关系进行转换
			 * 场景：WHERE t1.x IN (SELECT ...) 且 t1 在 available_rels1 中
			 */
			if ((j = convert_ANY_sublink_to_join(root, sublink,
												 available_rels1)) != NULL)
			{
				/* 转换成功! 构建新的 Join 节点并插入到 jtlink1 指向的位置 */
				j->larg = *jtlink1;
				*jtlink1 = (Node *) j;
				
				/* 递归处理刚刚提升上来的子查询部分 (j->rarg) */
				j->rarg = pull_up_sublinks_jointree_recurse(root,
															j->rarg,
															&child_rels);

				/*
				 * 递归处理新的 Join 节点的过滤条件 (quals)。
				 * 为什么要递归？因为子查询里可能还有嵌套的子查询。
				 * 新 Join 的条件可以引用左右两侧 (j->larg 和 j->rarg)。
				 */
				j->quals = pull_up_sublinks_qual_recurse(root,
														 j->quals,
														 &j->larg,
														 available_rels1,
														 &j->rarg,
														 child_rels);
				
				/* 返回 NULL，意味着原来的 IN (...) 条件不再作为 Filter 存在，而是变成了 Join */
				return NULL;
			}
			/* 尝试针对 link2 侧的关系进行转换（为了处理复杂的 Join 树结构） */
			if (available_rels2 != NULL &&
				(j = convert_ANY_sublink_to_join(root, sublink,
												 available_rels2)) != NULL)
			{
				j->larg = *jtlink2;
				*jtlink2 = (Node *) j;
				j->rarg = pull_up_sublinks_jointree_recurse(root,
															j->rarg,
															&child_rels);

				j->quals = pull_up_sublinks_qual_recurse(root,
														 j->quals,
														 &j->larg,
														 available_rels2,
														 &j->rarg,
														 child_rels);
				return NULL;
			}
		}
		else if (sublink->subLinkType == EXISTS_SUBLINK)
		{
			/*
			 * EXISTS 类型的逻辑几乎与 ANY 相同，只是使用 convert_EXISTS_sublink_to_join
			 * 这里的 false 参数表示它是正向的 EXISTS（不是 NOT EXISTS）
			 */
			if ((j = convert_EXISTS_sublink_to_join(root, sublink, false,
													available_rels1)) != NULL)
			{
				j->larg = *jtlink1;
				*jtlink1 = (Node *) j;
				j->rarg = pull_up_sublinks_jointree_recurse(root,
															j->rarg,
															&child_rels);

				j->quals = pull_up_sublinks_qual_recurse(root,
														 j->quals,
														 &j->larg,
														 available_rels1,
														 &j->rarg,
														 child_rels);
				return NULL;
			}
			if (available_rels2 != NULL &&
				(j = convert_EXISTS_sublink_to_join(root, sublink, false,
													available_rels2)) != NULL)
			{
				j->larg = *jtlink2;
				*jtlink2 = (Node *) j;
				j->rarg = pull_up_sublinks_jointree_recurse(root,
															j->rarg,
															&child_rels);

				j->quals = pull_up_sublinks_qual_recurse(root,
														 j->quals,
														 &j->larg,
														 available_rels2,
														 &j->rarg,
														 child_rels);
				return NULL;
			}
		}
		/* 如果是其他类型的 SubLink (如标量子查询) 或无法转换，则保持原样 */
		return node;
	}
	if (is_notclause(node))
	{
		/*
		 * 情况 2: 处理 NOT EXISTS 类型的子链接
		 * 目标：尝试转换为 Anti Join (反半连接)
		 * node 是一个 NOT 表达式。
		 */
		SubLink    *sublink = (SubLink *) get_notclausearg((Expr *) node);
		JoinExpr   *j;
		Relids		child_rels;

		if (sublink && IsA(sublink, SubLink))
		{
			if (sublink->subLinkType == EXISTS_SUBLINK)
			{
				/* 
				 * 调用 convert_EXISTS_sublink_to_join，参数 true 表示 under_not。
				 * 这将生成 Anti Join。
				 */
				if ((j = convert_EXISTS_sublink_to_join(root, sublink, true,
														available_rels1)) != NULL)
				{
					j->larg = *jtlink1;
					*jtlink1 = (Node *) j;
					j->rarg = pull_up_sublinks_jointree_recurse(root,
																j->rarg,
																&child_rels);

					/*
					 * 关键点：对于 Anti Join (NOT EXISTS)，
					 * 我们不能将引用左侧关系的 SubLink 上拉到这个 Join 的 quals 中。
					 * 因为在 Anti Join 中，ON 条件过滤的是右表匹配行，
					 * 而不是左表保留行。
					 * 所以这里 jtlink1 传了 NULL，表示不允许引用左侧。
					 */
					j->quals = pull_up_sublinks_qual_recurse(root,
															 j->quals,
															 &j->rarg,
															 child_rels,
															 NULL, NULL);
					/* 返回 NULL (NOT EXISTS 条件已由 Anti Join 实现) */
					return NULL;
				}
				if (available_rels2 != NULL &&
					(j = convert_EXISTS_sublink_to_join(root, sublink, true,
														available_rels2)) != NULL)
				{
					j->larg = *jtlink2;
					*jtlink2 = (Node *) j;
					j->rarg = pull_up_sublinks_jointree_recurse(root,
																j->rarg,
																&child_rels);

					j->quals = pull_up_sublinks_qual_recurse(root,
															 j->quals,
															 &j->larg,
															 available_rels2,
															 &j->rarg,
															 child_rels);
					return NULL;
				}
			}
		}
		/* 否则原样返回 */
		return node;
	}
	if (is_andclause(node))
	{
		/*
		 * 情况 3: 处理 AND 连接的多个条件
		 * 递归处理 AND 列表中的每一个条件。
		 */
		List	   *newclauses = NIL;
		ListCell   *l;

		foreach(l, ((BoolExpr *) node)->args)
		{
			Node	   *oldclause = (Node *) lfirst(l);
			Node	   *newclause;

			newclause = pull_up_sublinks_qual_recurse(root,
													  oldclause,
													  jtlink1,
													  available_rels1,
													  jtlink2,
													  available_rels2);
			if (newclause)
				newclauses = lappend(newclauses, newclause);
		}
		/* 如果所有子句都被提升了（变为 Join），则返回 NULL；否则构造新的 AND */
		if (newclauses == NIL)
			return NULL;
		else if (list_length(newclauses) == 1)
			return (Node *) linitial(newclauses);
		else
			return (Node *) make_andclause(newclauses);
	}
	/* 如果不是 AND，停止递归 */
	return node;
}

/*
 * inline_set_returning_functions
 *		Attempt to "inline" set-returning functions in the FROM clause.
 *
 * If an RTE_FUNCTION rtable entry invokes a set-returning function that
 * contains just a simple SELECT, we can convert the rtable entry to an
 * RTE_SUBQUERY entry exposing the SELECT directly.  This is especially
 * useful if the subquery can then be "pulled up" for further optimization,
 * but we do it even if not, to reduce executor overhead.
 *
 * This has to be done before we have started to do any optimization of
 * subqueries, else any such steps wouldn't get applied to subqueries
 * obtained via inlining.  However, we do it after pull_up_sublinks
 * so that we can inline any functions used in SubLink subselects.
 *
 * Like most of the planner, this feels free to scribble on its input data
 * structure.
 */
void
inline_set_returning_functions(PlannerInfo *root)
{
	ListCell   *rt;

	foreach(rt, root->parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);

		if (rte->rtekind == RTE_FUNCTION)
		{
			Query	   *funcquery;

			/* Check safety of expansion, and expand if possible */
			funcquery = inline_set_returning_function(root, rte);
			if (funcquery)
			{
				/* Successful expansion, convert the RTE to a subquery */
				rte->rtekind = RTE_SUBQUERY;
				rte->subquery = funcquery;
				rte->security_barrier = false;
				/* Clear fields that should not be set in a subquery RTE */
				rte->functions = NIL;
				rte->funcordinality = false;
			}
		}
	}
}

/*
 * pull_up_subqueries
 *		在范围表中查找可以提升到父查询中的子查询。
 *		如果子查询没有分组/聚合等特殊功能，我们可以将其合并到父查询的连接树中。
 *		此外，简单的 UNION ALL 结构的子查询可以转换为“追加关系”（append relations）。
 *
 *		此优化的目的是减少查询层级，给予优化器更多自由度来优化连接顺序。
 *
 *		例子 1：简单子查询合并 (Flattening)
 *			原始查询：SELECT * FROM (SELECT * FROM t1 WHERE a=1) AS s JOIN t2 ON s.b=t2.b;
 *			优化效果：逻辑上变为 SELECT * FROM t1 JOIN t2 ON t1.b=t2.b WHERE t1.a=1;
 *			益处：消除子查询边界，允许优化器在 t1 和 t2 之间自由选择连接顺序。
 *
 *		例子 2：UNION ALL 展开 (Append Relations)
 *			原始查询：SELECT * FROM (SELECT * FROM t1 UNION ALL SELECT * FROM t2) AS s WHERE s.a > 10;
 *			优化效果：转换为 Append 计划，直接扫描 t1 和 t2，并将 s.a > 10 下推。
 *			益处：避免物化中间结果，利用索引加速对基表的扫描。
 */
void
pull_up_subqueries(PlannerInfo *root)
{
	/* 连接树的顶层必须始终是一个 FromExpr (通常对应 SELECT ... FROM ... WHERE ...) */
	Assert(IsA(root->parse->jointree, FromExpr));

	/* 
	 * 递归遍历连接树，尝试提升子查询。
	 * 初始状态下：
	 * - lowest_outer_join_alias 为 NULL (不在外连接的右侧，可以自由提升)
	 * - joinrelids 为 NULL (尚未进入连接关系)
	 * - containing_appendrel 为 NULL (尚未进入 Append 关系)
	 */
	root->parse->jointree = (FromExpr *)
		pull_up_subqueries_recurse(root, (Node *) root->parse->jointree,
								   NULL, NULL, NULL);
	/* 优化及重写后，顶层结构应该仍然是一个 FromExpr */
	Assert(IsA(root->parse->jointree, FromExpr));
}

/*
 * pull_up_subqueries_recurse
 *		pull_up_subqueries 的递归核心部分。
 *
 * 此函数递归处理连接树并返回修改后的连接树。
 *
 * 参数说明:
 *	 root: 规划器信息结构体。
 *	 jtnode: 当前处理的连接树节点。
 *	 lowest_outer_join: 如果此节点位于某个外部连接（Outer Join）的范围内（即在其 ON 子句或输出列表中），
 *		则指向最低层级的那个 JoinExpr；否则为 NULL。这用于防止 LATERAL 子查询引用不该引用的变量。
 *	 lowest_nulling_outer_join: 如果此节点位于可能为 NULL 的外部连接的一侧（例如 LEFT JOIN 的右侧），
 *		则指向最低层级的那個 JoinExpr；否则为 NULL。这对于正确生成 PlaceHolderVar 至关重要，
 *		确保在非 NULL 上下文中引用的变量被正确处理。
 *	 containing_appendrel: 如果我们正在处理一个 Append Relation（由 UNION ALL 生成）的成员子查询，
 *		这会指向描述该关系的结构体；否则为 NULL。
 *
 * 处理逻辑:
 *	 1. 简单子查询 (RTE_SUBQUERY): 尝试将其“扁平化”到当前查询层级。
 *	 2. UNION ALL 子查询: 尝试将其转换为 Append Relation。
 *	 3. VALUES 子句: 在某些条件下尝试展开。
 *	 4. Still FromExpr/JoinExpr: 递归深入处理其子节点。
 *
 * 变量替换 (Variable Replacement):
 *	 当我们提升子查询时，必须在整个父查询中查找引用该子查询输出列的 Var 节点，
 *	 并将它们替换为子查询内部对应的表达式。这项工作主要由 pull_up_simple_subquery 等辅助函数完成。
 *	 此函数负责正确的递归遍历顺序，确保在修改其子节点之前，上层的结构是稳定的。
 */
static Node *
pull_up_subqueries_recurse(PlannerInfo *root, Node *jtnode,
						   JoinExpr *lowest_outer_join,
						   JoinExpr *lowest_nulling_outer_join,
						   AppendRelInfo *containing_appendrel)
{
	/* 由于此函数递归调用，可能会导致堆栈溢出，检查堆栈深度 */
	check_stack_depth();
	/* 此外，由于它稍微有点昂贵，检查是否有查询取消信号 */
	CHECK_FOR_INTERRUPTS();

	Assert(jtnode != NULL);
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;
		RangeTblEntry *rte = rt_fetch(varno, root->parse->rtable);

		/*
		 * 情况 1: 这是一个子查询 RTE 吗？
		 * 如果是，检查它是否足够简单以进行提升（Flattening）。
		 *
		 * 这里的 "简单 "通常意味着没有聚合、GROUP BY、ORDER BY ... LIMIT 等。
		 * 
		 * 如果我们正在处理追加关系成员 (UNION ALL 的一部分)，我们需要额外的检查 (is_safe_append_member)，
		 * 以防止产生不正确的 PlaceHolderVar 行为。
		 */
		if (rte->rtekind == RTE_SUBQUERY &&
			is_simple_subquery(root, rte->subquery, rte, lowest_outer_join) &&
			(containing_appendrel == NULL ||
			 is_safe_append_member(rte->subquery)))
			return pull_up_simple_subquery(root, jtnode, rte,
										   lowest_outer_join,
										   lowest_nulling_outer_join,
										   containing_appendrel);

		/*
		 * 情况 2: 它是一个简单的 UNION ALL 子查询吗？
		 * 如果是，我们不进行扁平化，而是将其展开为“追加关系” (Append Relation)。
		 *
		 * Append Relation 允许优化器将每个 UNION 分支视为独立的路径进行规划，
		 * 然后将结果合并。这比将整个 UNION ALL 结果视为一个黑盒要高效得多。
		 */
		if (rte->rtekind == RTE_SUBQUERY &&
			is_simple_union_all(rte->subquery))
			return pull_up_simple_union_all(root, jtnode, rte);

		/*
		 * 情况 3: 它是一个简单的 VALUES RTE 吗？
		 *
		 * 我们可以将简单的 VALUES 子句（例如 `FROM (VALUES (1),(2)) v(x)`）提升，
		 * 就像提升 constant SELECT 一样，这有助于后续的常量折叠等优化。
		 * 注意：我们不允许将 VALUES 提升到外部连接的可空侧下方，或者在 Append Relation 中。
		 */
		if (rte->rtekind == RTE_VALUES &&
			lowest_outer_join == NULL &&
			containing_appendrel == NULL &&
			is_simple_values(root, rte))
			return pull_up_simple_values(root, jtnode, rte);

		/* 否则，无法提升，保持节点不变。 */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		/* FromExpr 不应该出现在 AppendRelation 内部 */
		Assert(containing_appendrel == NULL);
		/* 递归处理 FROM 列表中的所有子项 */
		foreach(l, f->fromlist)
		{
			lfirst(l) = pull_up_subqueries_recurse(root, lfirst(l),
												   lowest_outer_join,
												   lowest_nulling_outer_join,
												   NULL);
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		Assert(containing_appendrel == NULL);
		/* 
		 * 递归处理 JOIN 的左右两侧。
		 * 根据 JOIN 类型，我们需要传递正确的 lowest_outer_join 和 lowest_nulling_outer_join。
		 * 这些参数告知下层节点它们处于何种外连接上下文中。
		 */
		switch (j->jointype)
		{
			case JOIN_INNER:
				/* 内连接不改变 Nullable 上下文 */
				j->larg = pull_up_subqueries_recurse(root, j->larg,
													 lowest_outer_join,
													 lowest_nulling_outer_join,
													 NULL);
				j->rarg = pull_up_subqueries_recurse(root, j->rarg,
													 lowest_outer_join,
													 lowest_nulling_outer_join,
													 NULL);
				break;
			case JOIN_LEFT:
			case JOIN_SEMI:
			case JOIN_ANTI:
				/* 
				 * 对于左连接 (及其变体)，左侧上下文不变。
				 * 右侧现在处于此连接 (j) 的可空侧，因此 lowest_nulling_outer_join 变为 j。
				 * lowest_outer_join 也变为 j，因为右侧在 LATERAL 引用上也受到此连接的限制。
				 */
				j->larg = pull_up_subqueries_recurse(root, j->larg,
													 j,
													 lowest_nulling_outer_join,
													 NULL);
				j->rarg = pull_up_subqueries_recurse(root, j->rarg,
													 j,
													 j,
													 NULL);
				break;
			case JOIN_FULL:
				/* 全外连接两侧都变为可空侧 */
				j->larg = pull_up_subqueries_recurse(root, j->larg,
													 j,
													 j,
													 NULL);
				j->rarg = pull_up_subqueries_recurse(root, j->rarg,
													 j,
													 j,
													 NULL);
				break;
			case JOIN_RIGHT:
				/* 右连接与左连接相反 */
				j->larg = pull_up_subqueries_recurse(root, j->larg,
													 j,
													 j,
													 NULL);
				j->rarg = pull_up_subqueries_recurse(root, j->rarg,
													 j,
													 lowest_nulling_outer_join,
													 NULL);
				break;
			default:
				elog(ERROR, "unrecognized join type: %d",
					 (int) j->jointype);
				break;
		}
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return jtnode;
}

/*
 * pull_up_simple_subquery
 *		尝试提升单个简单子查询。
 *
 * jtnode 是已经被 pull_up_subqueries 初步识别为简单子查询的 RangeTblRef。
 * 我们返回替换后的连接树节点；如果最终确定该子查询无法提升，则返回 jtnode 本身。
 *
 * rte 是 jtnode 引用的 RangeTblEntry。
 * 其余参数与 pull_up_subqueries_recurse 相同。
 */
static Node *
pull_up_simple_subquery(PlannerInfo *root, Node *jtnode, RangeTblEntry *rte,
						JoinExpr *lowest_outer_join,
						JoinExpr *lowest_nulling_outer_join,
						AppendRelInfo *containing_appendrel)
{
	Query	   *parse = root->parse;
	int			varno = ((RangeTblRef *) jtnode)->rtindex;
	Query	   *subquery;
	PlannerInfo *subroot;
	int			rtoffset;
	pullup_replace_vars_context rvcontext;
	ListCell   *lc;

	/*
	 * 需要子查询的一个可修改副本以进行处理。
	 * 即使我们在下面决定不提升，我们也必须这样做，以避免如果同一个子查询被多个连接树项引用时出现问题
	 * （这在正常情况下不会发生，但在规则重写后可能会发生）。
	 */
	subquery = copyObject(rte->subquery);

	/*
	 * 为此子查询创建一个 PlannerInfo 数据结构。
	 *
	 * 注意：接下来的几个步骤应与 subquery_planner() 中的初始处理相匹配。
	 * 我们是否可以重构以避免代码重复，或者那只会让事情变得更糟？
	 */
	subroot = makeNode(PlannerInfo);
	subroot->parse = subquery;
	subroot->glob = root->glob;
	subroot->query_level = root->query_level;
	subroot->parent_root = root->parent_root;
	subroot->plan_params = NIL;
	subroot->outer_params = NULL;
	subroot->planner_cxt = CurrentMemoryContext;
	subroot->init_plans = NIL;
	subroot->cte_plan_ids = NIL;
	subroot->multiexpr_params = NIL;
	subroot->eq_classes = NIL;
	subroot->append_rel_list = NIL;
	subroot->rowMarks = NIL;
	memset(subroot->upper_rels, 0, sizeof(subroot->upper_rels));
	memset(subroot->upper_targets, 0, sizeof(subroot->upper_targets));
	subroot->processed_tlist = NIL;
	subroot->grouping_map = NULL;
	subroot->minmax_aggs = NIL;
	subroot->qual_security_level = 0;
	subroot->inhTargetKind = INHKIND_NONE;
	subroot->hasRecursion = false;
	subroot->wt_param_id = -1;
	subroot->non_recursive_path = NULL;

	/* 无需担心 CTE */
	Assert(subquery->cteList == NIL);

	/*
	 * 如果 FROM 子句为空，将其替换为虚拟的 RTE_RESULT RTE，
	 * 这样我们就不需要处理那么多特殊情况了。
	 */
	replace_empty_jointree(subquery);

	/*
	 * 提升子查询 quals 中的任何 SubLink，以免留下未优化的 SubLink。
	 */
	if (subquery->hasSubLinks)
		pull_up_sublinks(subroot);

	/*
	 * 同样，内联其范围表中的任何集合返回函数。
	 */
	inline_set_returning_functions(subroot);

	/*
	 * 递归提升子查询的子查询，以便 pull_up_subqueries 对其连接树和范围表的处理完成。
	 *
	 * 注意：即使我们在上层查询的外部连接中，子查询的递归以 containing-join info 为 NULL 开始也是可以的；
	 * 下层查询以外部连接语义的空白开始。同样，我们不需要向下传递 appendrel 状态。
	 */
	pull_up_subqueries(subroot);

	/*
	 * 现在我们必须重新检查子查询是否仍然足够简单以进行提升。如果不是，放弃处理它。
	 *
	 * 我们实际上不需要重新检查所有涉及的条件，但为了保持这个“if”与
	 * pull_up_subqueries_recurse 中的看起来一样，这样做更容易。
	 */
	if (is_simple_subquery(root, subquery, rte, lowest_outer_join) &&
		(containing_appendrel == NULL || is_safe_append_member(subquery)))
	{
		/* 可以继续 */
	}
	else
	{
		/*
		 * 放弃，返回未修改的 RangeTblRef。
		 *
		 * 注意：当子查询单独进行规划时，我们刚刚所做的工作将重做。
		 * 也许我们可以通过将修改后的子查询存回范围表来避免这种情况，但我现在不想冒这个险。
		 */
		return jtnode;
	}

	/*
	 * 我们必须扁平化子查询目标列表中的任何连接别名 Vars，
	 * 因为提升子查询的子查询可能已将其扩展为任意表达式，这可能会影响
	 * pullup_replace_vars 关于是否需要 PlaceHolderVar 包装器的决策。
	 * （可能最好在早期阶段，甚至在重写器中对整个查询树进行 flatten_join_alias_vars；
	 * 但现在让我们就在这里修复这种情况。）
	 */
	subquery->targetList = (List *)
		flatten_join_alias_vars(subroot->parse, (Node *) subquery->targetList);

	/*
	 * 调整子查询中的 level-0 varnos，以便我们可以将其范围表追加到上层查询的范围表。
	 * 我们还必须修复子查询的 append_rel_list。
	 */
	rtoffset = list_length(parse->rtable);
	OffsetVarNodes((Node *) subquery, rtoffset, 0);
	OffsetVarNodes((Node *) subroot->append_rel_list, rtoffset, 0);

	/*
	 * 子查询中的上层变量现在比以前更接近其父级一层。
	 */
	IncrementVarSublevelsUp((Node *) subquery, -1, 1);
	IncrementVarSublevelsUp((Node *) subroot->append_rel_list, -1, 1);

	/*
	 * 子查询的目标列表项现在处于适合插入到顶层查询的形式，
	 * 除了我们可能需要将它们包装在 PlaceHolderVars 中。
	 * 为 pullup_replace_vars 设置所需的上下文数据。
	 */
	rvcontext.root = root;
	rvcontext.targetlist = subquery->targetList;
	rvcontext.target_rte = rte;
	if (rte->lateral)
		rvcontext.relids = get_relids_in_jointree((Node *) subquery->jointree,
												  true);
	else						/* 不需要 relids */
		rvcontext.relids = NULL;
	rvcontext.outer_hasSubLinks = &parse->hasSubLinks;
	rvcontext.varno = varno;
	/* 这些标志将在下面根据需要设置 */
	rvcontext.need_phvs = false;
	rvcontext.wrap_non_vars = false;
	/* initialize cache array with indexes 0 .. length(tlist) */
	rvcontext.rv_cache = palloc0((list_length(subquery->targetList) + 1) *
								 sizeof(Node *));

	/*
	 * 如果我们在外部连接下，则非可空项和 lateral 引用可能必须转换为 PlaceHolderVars。
	 */
	if (lowest_nulling_outer_join != NULL)
		rvcontext.need_phvs = true;

	/*
	 * 如果我们正在处理 appendrel 成员，那么任何不是简单 Var 的东西都必须转换为 PlaceHolderVar。
	 * 我们强制这样做是为了确保我们提升的内容不会在以后的处理中合并到周围的表达式中，
	 * 从而无法匹配从 appendrel 实际可用的表达式。
	 */
	if (containing_appendrel != NULL)
	{
		rvcontext.need_phvs = true;
		rvcontext.wrap_non_vars = true;
	}

	/*
	 * 如果父查询使用分组集，我们需要为任何不是简单 Var 的东西使用 PlaceHolderVar。
	 * 同样，这确保表达式保持其独立身份，以便在适当时匹配分组集列。
	 * （仅包装分组集列中使用的值，并且仅在 tlist 和 havingQual 的非聚合部分中这样做就足够了，
	 * 但这需要 pullup_replace_vars 目前尚未具备的大量基础设施。）
	 */
	if (parse->groupingSets)
	{
		rvcontext.need_phvs = true;
		rvcontext.wrap_non_vars = true;
	}

	/*
	 * 将顶层查询中对子查询输出的所有引用替换为调整后的子列表项的副本，
	 * 小心不要替换任何连接树结构。（如果我们可以使用 query_tree_mutator，这会更干净。）
	 * 我们必须在 targetList、returningList 和 havingQual 中使用 PHV，
	 * 因为这些肯定在任何外部连接之上。
	 * replace_vars_in_jointree 跟踪其在连接树中的位置，并适当地使用 PHV 或不使用。
	 */
	parse->targetList = (List *)
		pullup_replace_vars((Node *) parse->targetList, &rvcontext);
	parse->returningList = (List *)
		pullup_replace_vars((Node *) parse->returningList, &rvcontext);
	if (parse->onConflict)
	{
		parse->onConflict->onConflictSet = (List *)
			pullup_replace_vars((Node *) parse->onConflict->onConflictSet,
								&rvcontext);
		parse->onConflict->onConflictWhere =
			pullup_replace_vars(parse->onConflict->onConflictWhere,
								&rvcontext);

		/*
		 * 我们假设 ON CONFLICT 的 arbiterElems、arbiterWhere、exclRelTlist
		 * 不包含任何对子查询的引用
		 */
	}
	replace_vars_in_jointree((Node *) parse->jointree, &rvcontext,
							 lowest_nulling_outer_join);
	Assert(parse->setOperations == NULL);
	parse->havingQual = pullup_replace_vars(parse->havingQual, &rvcontext);

	/*
	 * 替换 appendrels 的 translated_vars 列表中的引用。
	 * 当提升 appendrel 成员时，我们不需要父 appendrel 列表中的 PHV —— 它们之间没有任何外部连接。
	 * 在其他地方，使用 PHV 为了安全。（这个分析可以做得更紧密，但似乎不值得那么多麻烦。）
	 */
	foreach(lc, root->append_rel_list)
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(lc);
		bool		save_need_phvs = rvcontext.need_phvs;

		if (appinfo == containing_appendrel)
			rvcontext.need_phvs = false;
		appinfo->translated_vars = (List *)
			pullup_replace_vars((Node *) appinfo->translated_vars, &rvcontext);
		rvcontext.need_phvs = save_need_phvs;
	}

	/*
	 * 替换连接 RTE 的 joinaliasvars 列表中的引用。
	 *
	 * 你可能认为我们可以避免对 lowest_nulling_outer_join 下方的连接别名变量使用 PHV，
	 * 但这是行不通的，因为别名变量可能会在该连接上方被引用；
	 * 在别名变量被展平后，我们需要 PHV 出现在此类引用中。（也许有一天值得尝试在这里更聪明一点。）
	 */
	foreach(lc, parse->rtable)
	{
		RangeTblEntry *otherrte = (RangeTblEntry *) lfirst(lc);

		if (otherrte->rtekind == RTE_JOIN)
			otherrte->joinaliasvars = (List *)
				pullup_replace_vars((Node *) otherrte->joinaliasvars,
									&rvcontext);
	}

	/*
	 * 如果子查询有 LATERAL 标记，将其传播到其任何可能现在包含 lateral 交叉引用的子 RTE。
	 * 子 RTE 可能包含也可能不包含任何实际的 lateral 交叉引用，
	 * 但我们必须标记提升后的子 RTE，以便以后的规划阶段检查这些引用。
	 */
	if (rte->lateral)
	{
		foreach(lc, subquery->rtable)
		{
			RangeTblEntry *child_rte = (RangeTblEntry *) lfirst(lc);

			switch (child_rte->rtekind)
			{
				case RTE_RELATION:
					if (child_rte->tablesample)
						child_rte->lateral = true;
					break;
				case RTE_SUBQUERY:
				case RTE_FUNCTION:
				case RTE_VALUES:
				case RTE_TABLEFUNC:
					child_rte->lateral = true;
					break;
				case RTE_JOIN:
				case RTE_CTE:
				case RTE_NAMEDTUPLESTORE:
				case RTE_RESULT:
					/* 这些不可能包含任何 lateral 引用 */
					break;
			}
		}
	}

	/*
	 * 现在将调整后的范围表条目追加到上层查询。（我们在修复上层范围表条目后再做，
	 * 没有必要在子查询的条目上也运行该代码。）
	 */
	parse->rtable = list_concat(parse->rtable, subquery->rtable);

	/*
	 * 也提升任何 FOR UPDATE/SHARE 标记。（OffsetVarNodes 已经调整了标记 rtindexes，
	 * 所以只需连接列表。）
	 */
	parse->rowMarks = list_concat(parse->rowMarks, subquery->rowMarks);

	/*
	 * 我们还必须修复父查询中任何 PlaceHolderVar 节点的 relid 集。
	 * （这也许可以通过 pullup_replace_vars() 完成，但使用两遍扫描似乎更清晰。）
	 * 特别注意，pullup_replace_vars() 刚刚创建的任何 PlaceHolderVar 节点都会被调整，
	 * 所以用子查询的 varno 创建它们是正确的。
	 *
	 * 同样，出现在 AppendRelInfo 节点中的 relids 也必须修复。
	 * 我们之前已经检查过，这不会需要在单槽 AppendRelInfo 结构中引入多个 subrelids。
	 */
	if (parse->hasSubLinks || root->glob->lastPHId != 0 ||
		root->append_rel_list)
	{
		Relids		subrelids;

		subrelids = get_relids_in_jointree((Node *) subquery->jointree, false);
		substitute_phv_relids((Node *) parse, varno, subrelids);
		fix_append_rel_relids(root->append_rel_list, varno, subrelids);
	}

	/*
	 * 现在将子查询的 AppendRelInfos 添加到我们的列表中。
	 */
	root->append_rel_list = list_concat(root->append_rel_list,
										subroot->append_rel_list);

	/*
	 * 我们不需要为 outer-join info 做类似的簿记，因为它还没有建立。
	 * placeholder_list 也是如此。
	 */
	Assert(root->join_info_list == NIL);
	Assert(subroot->join_info_list == NIL);
	Assert(root->placeholder_list == NIL);
	Assert(subroot->placeholder_list == NIL);

	/*
	 * 杂项清理工作。
	 *
	 * 虽然如果从子查询的目标列表中复制了任何 SubLink，replace_rte_variables() 会忠实地更新 parse->hasSubLinks，
	 * 但我们仍然可能会因为从子查询复制上来的 FUNCTION 和 VALUES RTE 的表达式中增加了 SubLinks。
	 * 所以无论如何都有必要复制 subquery->hasSubLinks。也许这一天可以改进。
	 */
	parse->hasSubLinks |= subquery->hasSubLinks;

	/* 如果子查询有任何 RLS 条件，现在主查询也有 */
	parse->hasRowSecurity |= subquery->hasRowSecurity;

	/*
	 * 如果有 hasAggs, hasWindowFuncs 或 hasTargetSRFs，子查询将不会被提升，
	 * 所以不需要在这些标志上做任何工作
	 */

	 /*
	 * 返回调整后的子查询连接树以替换父连接树中的 RangeTblRef 条目；
	 * 或者，如果 FromExpr 只有一个成员，只返回它的单个成员。
	 */
	Assert(IsA(subquery->jointree, FromExpr));
	Assert(subquery->jointree->fromlist != NIL);
	if (subquery->jointree->quals == NULL &&
		list_length(subquery->jointree->fromlist) == 1)
		return (Node *) linitial(subquery->jointree->fromlist);

	return (Node *) subquery->jointree;
}

/*
 * pull_up_simple_union_all
 *		Pull up a single simple UNION ALL subquery.
 *
 * jtnode is a RangeTblRef that has been identified as a simple UNION ALL
 * subquery by pull_up_subqueries.  We pull up the leaf subqueries and
 * build an "append relation" for the union set.  The result value is just
 * jtnode, since we don't actually need to change the query jointree.
 */
static Node *
pull_up_simple_union_all(PlannerInfo *root, Node *jtnode, RangeTblEntry *rte)
{
	int			varno = ((RangeTblRef *) jtnode)->rtindex;
	Query	   *subquery = rte->subquery;
	int			rtoffset = list_length(root->parse->rtable);
	List	   *rtable;

	/*
	 * Make a modifiable copy of the subquery's rtable, so we can adjust
	 * upper-level Vars in it.  There are no such Vars in the setOperations
	 * tree proper, so fixing the rtable should be sufficient.
	 */
	rtable = copyObject(subquery->rtable);

	/*
	 * Upper-level vars in subquery are now one level closer to their parent
	 * than before.  We don't have to worry about offsetting varnos, though,
	 * because the UNION leaf queries can't cross-reference each other.
	 */
	IncrementVarSublevelsUp_rtable(rtable, -1, 1);

	/*
	 * If the UNION ALL subquery had a LATERAL marker, propagate that to all
	 * its children.  The individual children might or might not contain any
	 * actual lateral cross-references, but we have to mark the pulled-up
	 * child RTEs so that later planner stages will check for such.
	 */
	if (rte->lateral)
	{
		ListCell   *rt;

		foreach(rt, rtable)
		{
			RangeTblEntry *child_rte = (RangeTblEntry *) lfirst(rt);

			Assert(child_rte->rtekind == RTE_SUBQUERY);
			child_rte->lateral = true;
		}
	}

	/*
	 * Append child RTEs to parent rtable.
	 */
	root->parse->rtable = list_concat(root->parse->rtable, rtable);

	/*
	 * Recursively scan the subquery's setOperations tree and add
	 * AppendRelInfo nodes for leaf subqueries to the parent's
	 * append_rel_list.  Also apply pull_up_subqueries to the leaf subqueries.
	 */
	Assert(subquery->setOperations);
	pull_up_union_leaf_queries(subquery->setOperations, root, varno, subquery,
							   rtoffset);

	/*
	 * Mark the parent as an append relation.
	 */
	rte->inh = true;

	return jtnode;
}

/*
 * pull_up_union_leaf_queries -- recursive guts of pull_up_simple_union_all
 *
 * Build an AppendRelInfo for each leaf query in the setop tree, and then
 * apply pull_up_subqueries to the leaf query.
 *
 * Note that setOpQuery is the Query containing the setOp node, whose tlist
 * contains references to all the setop output columns.  When called from
 * pull_up_simple_union_all, this is *not* the same as root->parse, which is
 * the parent Query we are pulling up into.
 *
 * parentRTindex is the appendrel parent's index in root->parse->rtable.
 *
 * The child RTEs have already been copied to the parent.  childRToffset
 * tells us where in the parent's range table they were copied.  When called
 * from flatten_simple_union_all, childRToffset is 0 since the child RTEs
 * were already in root->parse->rtable and no RT index adjustment is needed.
 */
static void
pull_up_union_leaf_queries(Node *setOp, PlannerInfo *root, int parentRTindex,
						   Query *setOpQuery, int childRToffset)
{
	if (IsA(setOp, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) setOp;
		int			childRTindex;
		AppendRelInfo *appinfo;

		/*
		 * Calculate the index in the parent's range table
		 */
		childRTindex = childRToffset + rtr->rtindex;

		/*
		 * Build a suitable AppendRelInfo, and attach to parent's list.
		 */
		appinfo = makeNode(AppendRelInfo);
		appinfo->parent_relid = parentRTindex;
		appinfo->child_relid = childRTindex;
		appinfo->parent_reltype = InvalidOid;
		appinfo->child_reltype = InvalidOid;
		make_setop_translation_list(setOpQuery, childRTindex,
									&appinfo->translated_vars);
		appinfo->parent_reloid = InvalidOid;
		root->append_rel_list = lappend(root->append_rel_list, appinfo);

		/*
		 * Recursively apply pull_up_subqueries to the new child RTE.  (We
		 * must build the AppendRelInfo first, because this will modify it.)
		 * Note that we can pass NULL for containing-join info even if we're
		 * actually under an outer join, because the child's expressions
		 * aren't going to propagate up to the join.  Also, we ignore the
		 * possibility that pull_up_subqueries_recurse() returns a different
		 * jointree node than what we pass it; if it does, the important thing
		 * is that it replaced the child relid in the AppendRelInfo node.
		 */
		rtr = makeNode(RangeTblRef);
		rtr->rtindex = childRTindex;
		(void) pull_up_subqueries_recurse(root, (Node *) rtr,
										  NULL, NULL, appinfo);
	}
	else if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;

		/* Recurse to reach leaf queries */
		pull_up_union_leaf_queries(op->larg, root, parentRTindex, setOpQuery,
								   childRToffset);
		pull_up_union_leaf_queries(op->rarg, root, parentRTindex, setOpQuery,
								   childRToffset);
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(setOp));
	}
}

/*
 * make_setop_translation_list
 *	  Build the list of translations from parent Vars to child Vars for
 *	  a UNION ALL member.  (At this point it's just a simple list of
 *	  referencing Vars, but if we succeed in pulling up the member
 *	  subquery, the Vars will get replaced by pulled-up expressions.)
 */
static void
make_setop_translation_list(Query *query, Index newvarno,
							List **translated_vars)
{
	List	   *vars = NIL;
	ListCell   *l;

	foreach(l, query->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->resjunk)
			continue;

		vars = lappend(vars, makeVarFromTargetEntry(newvarno, tle));
	}

	*translated_vars = vars;
}

/*
 * is_simple_subquery
 *	  检查范围表（RangeTable）中的子查询是否足够简单，能够被提升（pull up）到父查询中。
 *
 *	  所谓的“提升”，指的是将子查询的结构（FROM, WHERE 等）合并到父查询中，
 *	  从而消除子查询的边界。这通常能让优化器有更多的连接顺序选择。
 *
 * 参数:
 *	  rte: 包含子查询的 RTE_SUBQUERY 类型的 RangeTblEntry。
 *	  subquery: 具体的查询树 (Query 结构体)。注意它不一定等于 rte->subquery；
 *				可能是它的一个经过处理的副本。
 *	  lowest_outer_join: 子查询之上的最低层外部连接（Outer Join），如果没有则为 NULL。
 *                       用于检查 LATERAL 引用的安全性。
 */
static bool
is_simple_subquery(PlannerInfo *root, Query *subquery, RangeTblEntry *rte,
				   JoinExpr *lowest_outer_join)
{
	/*
	 * 首先确保它是一个有效的 SELECT 子查询 ...
	 */
	if (!IsA(subquery, Query) ||
		subquery->commandType != CMD_SELECT)
		elog(ERROR, "subquery is bogus");

	/*
	 * 目前无法提升包含集合操作（Set Operations，如 UNION, INTERSECT, EXCEPT）的查询。
	 * （除非它是简单的 UNION ALL，那种情况由不同的代码路径处理，见 pull_up_simple_union_all）。
	 * 也许在未来的查询树重构后可以支持更多情况...
	 */
	if (subquery->setOperations)
		return false;

	/*
	 * 无法提升涉及分组、聚合、窗口函数、集合返回函数 (SRF)、
	 * 排序、限制 (LIMIT) 或 CTE (WITH) 的子查询。
	 * (XXX WITH 将来可能会被允许)
	 *
	 * 如果子查询有显式的 FOR UPDATE/SHARE 子句，我们也无法提升，
	 * 因为提升会导致锁定操作发生在语义上比原本应该发生的层级更高的地方。
	 * 隐式的 FOR UPDATE/SHARE 是可以的，因为这种情况下锁定原本就在上层查询中声明了。
	 *
	 * 简而言之，这些特性通常意味着子查询定义了一个特定的数据集边界或计算顺序，
	 * 打破这个边界会改变查询的语义。
	 */
	if (subquery->hasAggs ||
		subquery->hasWindowFuncs ||
		subquery->hasTargetSRFs ||
		subquery->groupClause ||
		subquery->groupingSets ||
		subquery->havingQual ||
		subquery->sortClause ||
		subquery->distinctClause ||
		subquery->limitOffset ||
		subquery->limitCount ||
		subquery->hasForUpdate ||
		subquery->cteList)
		return false;

	/*
	 * 如果 RTE 代表一个安全屏障（security-barrier）视图（例如使用了 security_barrier 选项的视图），
	 * 则不要提升。
	 * 
	 * 原因：如果提升了视图，视图内部的 WHERE 条件和上层查询的 WHERE 条件混杂在一起，
	 * 优化器可能会重排执行顺序，导致上层用户定义的（可能是恶意的）函数在视图过滤条件之前执行，
	 * 从而泄露本应被视图隐藏的数据。
	 */
	if (rte->security_barrier)
		return false;

	/*
	 * 如果子查询是 LATERAL 的（即它可以引用同层级之前的表），检查基于此的提升限制。
	 */
	if (rte->lateral)
	{
		bool		restricted;
		Relids		safe_upper_varnos;

		/*
		 * 子查询的 WHERE 和 JOIN/ON 条件绝不能包含任何指向“高于某个外部连接”的外部表的 LATERAL 引用。
		 * （即使该外部连接位于子查询本身内部，也算作这种情况，不过这里主要关注 lowest_outer_join）。
		 *
		 * 如果存在这种情况，提升子查询会导致我们需要将位于外部连接下方的条件推迟到外部连接上方执行，
		 * 这在逻辑上可能是完全错误的（例如，破坏了外连接的语义），
		 * 无论如何这是一个复杂的问题，目前不值得为了解决它而增加复杂性。
		 */
		if (lowest_outer_join != NULL)
		{
			restricted = true;
			safe_upper_varnos = get_relids_in_jointree((Node *) lowest_outer_join,
													   true);
		}
		else
		{
			restricted = false;
			safe_upper_varnos = NULL;	/* 并不重要 */
		}

		if (jointree_contains_lateral_outer_refs(root,
												 (Node *) subquery->jointree,
												 restricted, safe_upper_varnos))
			return false;

		/*
		 * 如果 LATERAL 子查询上方存在外部连接，并且子查询的目标列表（TargetList）
		 * 包含任何指向该外部连接之外的表的引用，也禁止提升。
		 *
		 * 因为这些引用可能会被拉取到子查询上方的条件中（但在外部连接内部或下方），
		 * 从而导致类似于上述检查的“条件推迟”问题。
		 *
		 * （如果在外部查询的条件中没有出现此类引用，我们其实不需要阻止提升，
		 * 但我们在这里没有足够的信息来检查这一点。
		 * 另外，如果我们强制将此类引用包装在 PlaceHolderVars 中，即使它们位于最近的外部连接下方，
		 * 也许可以移除此限制？但这用法相当奇怪，不确定是否值得通过努力来支持。）
		 */
		if (lowest_outer_join != NULL)
		{
			Relids		lvarnos = pull_varnos_of_level(root,
													   (Node *) subquery->targetList,
													   1);

			if (!bms_is_subset(lvarnos, safe_upper_varnos))
				return false;
		}
	}

	/*
	 * 不要提升任何在其目标列表中包含易变（volatile）函数的子查询。
	 *
	 * 原因：易变函数（如 random(), timeofday()）每次调用结果可能不同。
	 * 提升后，原本在子查询中只计算一次的表达式，可能会被复制到上层查询的多个位置，
	 * 导致多次评估，从而产生令人惊讶的结果（例如，WHERE random() < 0.5 AND random() > 0.5）。
	 *
	 * （注意：PlaceHolderVar 机制并不能完全保证单次评估；
	 * 否则我们本可以提升它并通过 PlaceHolderVars 包装这些项目...）
	 */
	if (contain_volatile_functions((Node *) subquery->targetList))
		return false;

	return true;
}

/*
 * pull_up_simple_values
 *		Pull up a single simple VALUES RTE.
 *
 * jtnode is a RangeTblRef that has been identified as a simple VALUES RTE
 * by pull_up_subqueries.  We always return a RangeTblRef representing a
 * RESULT RTE to replace it (all failure cases should have been detected by
 * is_simple_values()).  Actually, what we return is just jtnode, because
 * we replace the VALUES RTE in the rangetable with the RESULT RTE.
 *
 * rte is the RangeTblEntry referenced by jtnode.  Because of the limited
 * possible usage of VALUES RTEs, we do not need the remaining parameters
 * of pull_up_subqueries_recurse.
 */
static Node *
pull_up_simple_values(PlannerInfo *root, Node *jtnode, RangeTblEntry *rte)
{
	Query	   *parse = root->parse;
	int			varno = ((RangeTblRef *) jtnode)->rtindex;
	List	   *values_list;
	List	   *tlist;
	AttrNumber	attrno;
	pullup_replace_vars_context rvcontext;
	ListCell   *lc;

	Assert(rte->rtekind == RTE_VALUES);
	Assert(list_length(rte->values_lists) == 1);

	/*
	 * Need a modifiable copy of the VALUES list to hack on, just in case it's
	 * multiply referenced.
	 */
	values_list = copyObject(linitial(rte->values_lists));

	/*
	 * The VALUES RTE can't contain any Vars of level zero, let alone any that
	 * are join aliases, so no need to flatten join alias Vars.
	 */
	Assert(!contain_vars_of_level((Node *) values_list, 0));

	/*
	 * Set up required context data for pullup_replace_vars.  In particular,
	 * we have to make the VALUES list look like a subquery targetlist.
	 */
	tlist = NIL;
	attrno = 1;
	foreach(lc, values_list)
	{
		tlist = lappend(tlist,
						makeTargetEntry((Expr *) lfirst(lc),
										attrno,
										NULL,
										false));
		attrno++;
	}
	rvcontext.root = root;
	rvcontext.targetlist = tlist;
	rvcontext.target_rte = rte;
	rvcontext.relids = NULL;
	rvcontext.outer_hasSubLinks = &parse->hasSubLinks;
	rvcontext.varno = varno;
	rvcontext.need_phvs = false;
	rvcontext.wrap_non_vars = false;
	/* initialize cache array with indexes 0 .. length(tlist) */
	rvcontext.rv_cache = palloc0((list_length(tlist) + 1) *
								 sizeof(Node *));

	/*
	 * Replace all of the top query's references to the RTE's outputs with
	 * copies of the adjusted VALUES expressions, being careful not to replace
	 * any of the jointree structure. (This'd be a lot cleaner if we could use
	 * query_tree_mutator.)  Much of this should be no-ops in the dummy Query
	 * that surrounds a VALUES RTE, but it's not enough code to be worth
	 * removing.
	 */
	parse->targetList = (List *)
		pullup_replace_vars((Node *) parse->targetList, &rvcontext);
	parse->returningList = (List *)
		pullup_replace_vars((Node *) parse->returningList, &rvcontext);
	if (parse->onConflict)
	{
		parse->onConflict->onConflictSet = (List *)
			pullup_replace_vars((Node *) parse->onConflict->onConflictSet,
								&rvcontext);
		parse->onConflict->onConflictWhere =
			pullup_replace_vars(parse->onConflict->onConflictWhere,
								&rvcontext);

		/*
		 * We assume ON CONFLICT's arbiterElems, arbiterWhere, exclRelTlist
		 * can't contain any references to a subquery
		 */
	}
	replace_vars_in_jointree((Node *) parse->jointree, &rvcontext, NULL);
	Assert(parse->setOperations == NULL);
	parse->havingQual = pullup_replace_vars(parse->havingQual, &rvcontext);

	/*
	 * There should be no appendrels to fix, nor any join alias Vars, nor any
	 * outer joins and hence no PlaceHolderVars.
	 */
	Assert(root->append_rel_list == NIL);
	Assert(list_length(parse->rtable) == 1);
	Assert(root->join_info_list == NIL);
	Assert(root->placeholder_list == NIL);

	/*
	 * Replace the VALUES RTE with a RESULT RTE.  The VALUES RTE is the only
	 * rtable entry in the current query level, so this is easy.
	 */
	Assert(list_length(parse->rtable) == 1);

	/* Create suitable RTE */
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RESULT;
	rte->eref = makeAlias("*RESULT*", NIL);

	/* Replace rangetable */
	parse->rtable = list_make1(rte);

	/* We could manufacture a new RangeTblRef, but the one we have is fine */
	Assert(varno == 1);

	return jtnode;
}

/*
 * is_simple_values
 *	  Check a VALUES RTE in the range table to see if it's simple enough
 *	  to pull up into the parent query.
 *
 * rte is the RTE_VALUES RangeTblEntry to check.
 */
static bool
is_simple_values(PlannerInfo *root, RangeTblEntry *rte)
{
	Assert(rte->rtekind == RTE_VALUES);

	/*
	 * There must be exactly one VALUES list, else it's not semantically
	 * correct to replace the VALUES RTE with a RESULT RTE, nor would we have
	 * a unique set of expressions to substitute into the parent query.
	 */
	if (list_length(rte->values_lists) != 1)
		return false;

	/*
	 * Because VALUES can't appear under an outer join (or at least, we won't
	 * try to pull it up if it does), we need not worry about LATERAL, nor
	 * about validity of PHVs for the VALUES' outputs.
	 */

	/*
	 * Don't pull up a VALUES that contains any set-returning or volatile
	 * functions.  The considerations here are basically identical to the
	 * restrictions on a pull-able subquery's targetlist.
	 */
	if (expression_returns_set((Node *) rte->values_lists) ||
		contain_volatile_functions((Node *) rte->values_lists))
		return false;

	/*
	 * Do not pull up a VALUES that's not the only RTE in its parent query.
	 * This is actually the only case that the parser will generate at the
	 * moment, and assuming this is true greatly simplifies
	 * pull_up_simple_values().
	 */
	if (list_length(root->parse->rtable) != 1 ||
		rte != (RangeTblEntry *) linitial(root->parse->rtable))
		return false;

	return true;
}

/*
 * is_simple_union_all
 *	  Check a subquery to see if it's a simple UNION ALL.
 *
 * We require all the setops to be UNION ALL (no mixing) and there can't be
 * any datatype coercions involved, ie, all the leaf queries must emit the
 * same datatypes.
 */
static bool
is_simple_union_all(Query *subquery)
{
	SetOperationStmt *topop;

	/* Let's just make sure it's a valid subselect ... */
	if (!IsA(subquery, Query) ||
		subquery->commandType != CMD_SELECT)
		elog(ERROR, "subquery is bogus");

	/* Is it a set-operation query at all? */
	topop = castNode(SetOperationStmt, subquery->setOperations);
	if (!topop)
		return false;

	/* Can't handle ORDER BY, LIMIT/OFFSET, locking, or WITH */
	if (subquery->sortClause ||
		subquery->limitOffset ||
		subquery->limitCount ||
		subquery->rowMarks ||
		subquery->cteList)
		return false;

	/* Recursively check the tree of set operations */
	return is_simple_union_all_recurse((Node *) topop, subquery,
									   topop->colTypes);
}

static bool
is_simple_union_all_recurse(Node *setOp, Query *setOpQuery, List *colTypes)
{
	/* Since this function recurses, it could be driven to stack overflow. */
	check_stack_depth();

	if (IsA(setOp, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) setOp;
		RangeTblEntry *rte = rt_fetch(rtr->rtindex, setOpQuery->rtable);
		Query	   *subquery = rte->subquery;

		Assert(subquery != NULL);

		/* Leaf nodes are OK if they match the toplevel column types */
		/* We don't have to compare typmods or collations here */
		return tlist_same_datatypes(subquery->targetList, colTypes, true);
	}
	else if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;

		/* Must be UNION ALL */
		if (op->op != SETOP_UNION || !op->all)
			return false;

		/* Recurse to check inputs */
		return is_simple_union_all_recurse(op->larg, setOpQuery, colTypes) &&
			is_simple_union_all_recurse(op->rarg, setOpQuery, colTypes);
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(setOp));
		return false;			/* keep compiler quiet */
	}
}

/*
 * is_safe_append_member
 *	  Check a subquery that is a leaf of a UNION ALL appendrel to see if it's
 *	  safe to pull up.
 */
static bool
is_safe_append_member(Query *subquery)
{
	FromExpr   *jtnode;

	/*
	 * It's only safe to pull up the child if its jointree contains exactly
	 * one RTE, else the AppendRelInfo data structure breaks. The one base RTE
	 * could be buried in several levels of FromExpr, however.  Also, if the
	 * child's jointree is completely empty, we can pull up because
	 * pull_up_simple_subquery will insert a single RTE_RESULT RTE instead.
	 *
	 * Also, the child can't have any WHERE quals because there's no place to
	 * put them in an appendrel.  (This is a bit annoying...) If we didn't
	 * need to check this, we'd just test whether get_relids_in_jointree()
	 * yields a singleton set, to be more consistent with the coding of
	 * fix_append_rel_relids().
	 */
	jtnode = subquery->jointree;
	Assert(IsA(jtnode, FromExpr));
	/* Check the completely-empty case */
	if (jtnode->fromlist == NIL && jtnode->quals == NULL)
		return true;
	/* Check the more general case */
	while (IsA(jtnode, FromExpr))
	{
		if (jtnode->quals != NULL)
			return false;
		if (list_length(jtnode->fromlist) != 1)
			return false;
		jtnode = linitial(jtnode->fromlist);
	}
	if (!IsA(jtnode, RangeTblRef))
		return false;

	return true;
}

/*
 * jointree_contains_lateral_outer_refs
 *		Check for disallowed lateral references in a jointree's quals
 *
 * If restricted is false, all level-1 Vars are allowed (but we still must
 * search the jointree, since it might contain outer joins below which there
 * will be restrictions).  If restricted is true, return true when any qual
 * in the jointree contains level-1 Vars coming from outside the rels listed
 * in safe_upper_varnos.
 */
static bool
jointree_contains_lateral_outer_refs(PlannerInfo *root, Node *jtnode,
									 bool restricted,
									 Relids safe_upper_varnos)
{
	if (jtnode == NULL)
		return false;
	if (IsA(jtnode, RangeTblRef))
		return false;
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		/* First, recurse to check child joins */
		foreach(l, f->fromlist)
		{
			if (jointree_contains_lateral_outer_refs(root,
													 lfirst(l),
													 restricted,
													 safe_upper_varnos))
				return true;
		}

		/* Then check the top-level quals */
		if (restricted &&
			!bms_is_subset(pull_varnos_of_level(root, f->quals, 1),
						   safe_upper_varnos))
			return true;
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		/*
		 * If this is an outer join, we mustn't allow any upper lateral
		 * references in or below it.
		 */
		if (j->jointype != JOIN_INNER)
		{
			restricted = true;
			safe_upper_varnos = NULL;
		}

		/* Check the child joins */
		if (jointree_contains_lateral_outer_refs(root,
												 j->larg,
												 restricted,
												 safe_upper_varnos))
			return true;
		if (jointree_contains_lateral_outer_refs(root,
												 j->rarg,
												 restricted,
												 safe_upper_varnos))
			return true;

		/* Check the JOIN's qual clauses */
		if (restricted &&
			!bms_is_subset(pull_varnos_of_level(root, j->quals, 1),
						   safe_upper_varnos))
			return true;
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return false;
}

/*
 * Helper routine for pull_up_subqueries: do pullup_replace_vars on every
 * expression in the jointree, without changing the jointree structure itself.
 * Ugly, but there's no other way...
 *
 * If we are at or below lowest_nulling_outer_join, we can suppress use of
 * PlaceHolderVars wrapped around the replacement expressions.
 */
static void
replace_vars_in_jointree(Node *jtnode,
						 pullup_replace_vars_context *context,
						 JoinExpr *lowest_nulling_outer_join)
{
	if (jtnode == NULL)
		return;
	if (IsA(jtnode, RangeTblRef))
	{
		/*
		 * If the RangeTblRef refers to a LATERAL subquery (that isn't the
		 * same subquery we're pulling up), it might contain references to the
		 * target subquery, which we must replace.  We drive this from the
		 * jointree scan, rather than a scan of the rtable, for a couple of
		 * reasons: we can avoid processing no-longer-referenced RTEs, and we
		 * can use the appropriate setting of need_phvs depending on whether
		 * the RTE is above possibly-nulling outer joins or not.
		 */
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		if (varno != context->varno)	/* ignore target subquery itself */
		{
			RangeTblEntry *rte = rt_fetch(varno, context->root->parse->rtable);

			Assert(rte != context->target_rte);
			if (rte->lateral)
			{
				switch (rte->rtekind)
				{
					case RTE_RELATION:
						/* shouldn't be marked LATERAL unless tablesample */
						Assert(rte->tablesample);
						rte->tablesample = (TableSampleClause *)
							pullup_replace_vars((Node *) rte->tablesample,
												context);
						break;
					case RTE_SUBQUERY:
						rte->subquery =
							pullup_replace_vars_subquery(rte->subquery,
														 context);
						break;
					case RTE_FUNCTION:
						rte->functions = (List *)
							pullup_replace_vars((Node *) rte->functions,
												context);
						break;
					case RTE_TABLEFUNC:
						rte->tablefunc = (TableFunc *)
							pullup_replace_vars((Node *) rte->tablefunc,
												context);
						break;
					case RTE_VALUES:
						rte->values_lists = (List *)
							pullup_replace_vars((Node *) rte->values_lists,
												context);
						break;
					case RTE_JOIN:
					case RTE_CTE:
					case RTE_NAMEDTUPLESTORE:
					case RTE_RESULT:
						/* these shouldn't be marked LATERAL */
						Assert(false);
						break;
				}
			}
		}
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
			replace_vars_in_jointree(lfirst(l), context,
									 lowest_nulling_outer_join);
		f->quals = pullup_replace_vars(f->quals, context);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		bool		save_need_phvs = context->need_phvs;

		if (j == lowest_nulling_outer_join)
		{
			/* no more PHVs in or below this join */
			context->need_phvs = false;
			lowest_nulling_outer_join = NULL;
		}
		replace_vars_in_jointree(j->larg, context, lowest_nulling_outer_join);
		replace_vars_in_jointree(j->rarg, context, lowest_nulling_outer_join);

		/*
		 * Use PHVs within the join quals of a full join, even when it's the
		 * lowest nulling outer join.  Otherwise, we cannot identify which
		 * side of the join a pulled-up var-free expression came from, which
		 * can lead to failure to make a plan at all because none of the quals
		 * appear to be mergeable or hashable conditions.  For this purpose we
		 * don't care about the state of wrap_non_vars, so leave it alone.
		 */
		if (j->jointype == JOIN_FULL)
			context->need_phvs = true;

		j->quals = pullup_replace_vars(j->quals, context);

		/*
		 * We don't bother to update the colvars list, since it won't be used
		 * again ...
		 */
		context->need_phvs = save_need_phvs;
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}

/*
 * Apply pullup variable replacement throughout an expression tree
 *
 * Returns a modified copy of the tree, so this can't be used where we
 * need to do in-place replacement.
 */
static Node *
pullup_replace_vars(Node *expr, pullup_replace_vars_context *context)
{
	return replace_rte_variables(expr,
								 context->varno, 0,
								 pullup_replace_vars_callback,
								 (void *) context,
								 context->outer_hasSubLinks);
}

static Node *
pullup_replace_vars_callback(Var *var,
							 replace_rte_variables_context *context)
{
	pullup_replace_vars_context *rcon = (pullup_replace_vars_context *) context->callback_arg;
	int			varattno = var->varattno;
	Node	   *newnode;

	/*
	 * If PlaceHolderVars are needed, we cache the modified expressions in
	 * rcon->rv_cache[].  This is not in hopes of any material speed gain
	 * within this function, but to avoid generating identical PHVs with
	 * different IDs.  That would result in duplicate evaluations at runtime,
	 * and possibly prevent optimizations that rely on recognizing different
	 * references to the same subquery output as being equal().  So it's worth
	 * a bit of extra effort to avoid it.
	 */
	if (rcon->need_phvs &&
		varattno >= InvalidAttrNumber &&
		varattno <= list_length(rcon->targetlist) &&
		rcon->rv_cache[varattno] != NULL)
	{
		/* Just copy the entry and fall through to adjust its varlevelsup */
		newnode = copyObject(rcon->rv_cache[varattno]);
	}
	else if (varattno == InvalidAttrNumber)
	{
		/* Must expand whole-tuple reference into RowExpr */
		RowExpr    *rowexpr;
		List	   *colnames;
		List	   *fields;
		bool		save_need_phvs = rcon->need_phvs;
		int			save_sublevelsup = context->sublevels_up;

		/*
		 * If generating an expansion for a var of a named rowtype (ie, this
		 * is a plain relation RTE), then we must include dummy items for
		 * dropped columns.  If the var is RECORD (ie, this is a JOIN), then
		 * omit dropped columns. Either way, attach column names to the
		 * RowExpr for use of ruleutils.c.
		 *
		 * In order to be able to cache the results, we always generate the
		 * expansion with varlevelsup = 0, and then adjust if needed.
		 */
		expandRTE(rcon->target_rte,
				  var->varno, 0 /* not varlevelsup */ , var->location,
				  (var->vartype != RECORDOID),
				  &colnames, &fields);
		/* Adjust the generated per-field Vars, but don't insert PHVs */
		rcon->need_phvs = false;
		context->sublevels_up = 0;	/* to match the expandRTE output */
		fields = (List *) replace_rte_variables_mutator((Node *) fields,
														context);
		rcon->need_phvs = save_need_phvs;
		context->sublevels_up = save_sublevelsup;

		rowexpr = makeNode(RowExpr);
		rowexpr->args = fields;
		rowexpr->row_typeid = var->vartype;
		rowexpr->row_format = COERCE_IMPLICIT_CAST;
		rowexpr->colnames = colnames;
		rowexpr->location = var->location;
		newnode = (Node *) rowexpr;

		/*
		 * Insert PlaceHolderVar if needed.  Notice that we are wrapping one
		 * PlaceHolderVar around the whole RowExpr, rather than putting one
		 * around each element of the row.  This is because we need the
		 * expression to yield NULL, not ROW(NULL,NULL,...) when it is forced
		 * to null by an outer join.
		 */
		if (rcon->need_phvs)
		{
			/* RowExpr is certainly not strict, so always need PHV */
			newnode = (Node *)
				make_placeholder_expr(rcon->root,
									  (Expr *) newnode,
									  bms_make_singleton(rcon->varno));
			/* cache it with the PHV, and with varlevelsup still zero */
			rcon->rv_cache[InvalidAttrNumber] = copyObject(newnode);
		}
	}
	else
	{
		/* Normal case referencing one targetlist element */
		TargetEntry *tle = get_tle_by_resno(rcon->targetlist, varattno);

		if (tle == NULL)		/* shouldn't happen */
			elog(ERROR, "could not find attribute %d in subquery targetlist",
				 varattno);

		/* Make a copy of the tlist item to return */
		newnode = (Node *) copyObject(tle->expr);

		/* Insert PlaceHolderVar if needed */
		if (rcon->need_phvs)
		{
			bool		wrap;

			if (newnode && IsA(newnode, Var) &&
				((Var *) newnode)->varlevelsup == 0)
			{
				/*
				 * Simple Vars always escape being wrapped, unless they are
				 * lateral references to something outside the subquery being
				 * pulled up.  (Even then, we could omit the PlaceHolderVar if
				 * the referenced rel is under the same lowest outer join, but
				 * it doesn't seem worth the trouble to check that.)
				 */
				if (rcon->target_rte->lateral &&
					!bms_is_member(((Var *) newnode)->varno, rcon->relids))
					wrap = true;
				else
					wrap = false;
			}
			else if (newnode && IsA(newnode, PlaceHolderVar) &&
					 ((PlaceHolderVar *) newnode)->phlevelsup == 0)
			{
				/* The same rules apply for a PlaceHolderVar */
				if (rcon->target_rte->lateral &&
					!bms_is_subset(((PlaceHolderVar *) newnode)->phrels,
								   rcon->relids))
					wrap = true;
				else
					wrap = false;
			}
			else if (rcon->wrap_non_vars)
			{
				/* Wrap all non-Vars in a PlaceHolderVar */
				wrap = true;
			}
			else
			{
				/*
				 * If it contains a Var of the subquery being pulled up, and
				 * does not contain any non-strict constructs, then it's
				 * certainly nullable so we don't need to insert a
				 * PlaceHolderVar.
				 *
				 * This analysis could be tighter: in particular, a non-strict
				 * construct hidden within a lower-level PlaceHolderVar is not
				 * reason to add another PHV.  But for now it doesn't seem
				 * worth the code to be more exact.
				 *
				 * Note: in future maybe we should insert a PlaceHolderVar
				 * anyway, if the tlist item is expensive to evaluate?
				 *
				 * For a LATERAL subquery, we have to check the actual var
				 * membership of the node, but if it's non-lateral then any
				 * level-zero var must belong to the subquery.
				 */
				if ((rcon->target_rte->lateral ?
					 bms_overlap(pull_varnos(rcon->root, (Node *) newnode),
								 rcon->relids) :
					 contain_vars_of_level((Node *) newnode, 0)) &&
					!contain_nonstrict_functions((Node *) newnode))
				{
					/* No wrap needed */
					wrap = false;
				}
				else
				{
					/* Else wrap it in a PlaceHolderVar */
					wrap = true;
				}
			}

			if (wrap)
				newnode = (Node *)
					make_placeholder_expr(rcon->root,
										  (Expr *) newnode,
										  bms_make_singleton(rcon->varno));

			/*
			 * Cache it if possible (ie, if the attno is in range, which it
			 * probably always should be).  We can cache the value even if we
			 * decided we didn't need a PHV, since this result will be
			 * suitable for any request that has need_phvs.
			 */
			if (varattno > InvalidAttrNumber &&
				varattno <= list_length(rcon->targetlist))
				rcon->rv_cache[varattno] = copyObject(newnode);
		}
	}

	/* Must adjust varlevelsup if tlist item is from higher query */
	if (var->varlevelsup > 0)
		IncrementVarSublevelsUp(newnode, var->varlevelsup, 0);

	return newnode;
}

/*
 * Apply pullup variable replacement to a subquery
 *
 * This needs to be different from pullup_replace_vars() because
 * replace_rte_variables will think that it shouldn't increment sublevels_up
 * before entering the Query; so we need to call it with sublevels_up == 1.
 */
static Query *
pullup_replace_vars_subquery(Query *query,
							 pullup_replace_vars_context *context)
{
	Assert(IsA(query, Query));
	return (Query *) replace_rte_variables((Node *) query,
										   context->varno, 1,
										   pullup_replace_vars_callback,
										   (void *) context,
										   NULL);
}


/*
 * flatten_simple_union_all
 *		尝试将顶层 UNION ALL 结构优化为 appendrel（追加关系）
 *
 * 如果一个查询的 setOperations 树完全由简单的 UNION ALL 操作组成，
 * 则将其扁平化为 append relation，这样我们可以比一般集合操作更智能地处理它。
 * 否则，什么也不做。
 *
 * 在大多数情况下，这只能对顶层查询成功，因为对于 FROM 中的子查询，
 * 父查询调用 pull_up_subqueries 时，通常已经通过 pull_up_simple_union_all 扁平化了 UNION。
 * 但这里我们支持一些那个代码路径不支持的情况，例如当子查询还包含 ORDER BY 时。
 *
 * 例如：
 * SELECT * FROM t1 UNION ALL SELECT * FROM t2 ORDER BY a;
 * 这里的 UNION ALL 可以被转换为一个 append relation，允许我们对 t1 和 t2 分别进行扫描，
 * 然后合并结果，而不是先计算 UNION ALL 的结果集再排序。
 */
void
flatten_simple_union_all(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	SetOperationStmt *topop;
	Node	   *leftmostjtnode;
	int			leftmostRTI;
	RangeTblEntry *leftmostRTE;
	int			childRTI;
	RangeTblEntry *childRTE;
	RangeTblRef *rtr;

	/* 除非查询有 setops，否则不应该调用 */
	topop = castNode(SetOperationStmt, parse->setOperations);
	Assert(topop);

	/* 无法优化递归 UNION (WITH RECURSIVE) */
	if (root->hasRecursion)
		return;

	/*
	 * 递归检查集合操作树。如果不是所有列类型相同的 UNION ALL，则放弃。
	 */
	if (!is_simple_union_all_recurse((Node *) topop, parse, topop->colTypes))
		return;

	/*
	 * 定位 setops 树中最左边的叶子查询。上层查询的 Vars 都引用此 RTE (参见 transformSetOperationStmt)。
	 *
	 * 在 UNION ALL 树中，最左侧的叶子节点具有特殊的地位，因为它的 RangeTblEntry (RTE)
	 * 最初被用作整个 UNION ALL 结果的每一个列的“代表”。
	 */
	leftmostjtnode = topop->larg;
	while (leftmostjtnode && IsA(leftmostjtnode, SetOperationStmt))
		leftmostjtnode = ((SetOperationStmt *) leftmostjtnode)->larg;
	Assert(leftmostjtnode && IsA(leftmostjtnode, RangeTblRef));
	leftmostRTI = ((RangeTblRef *) leftmostjtnode)->rtindex;
	leftmostRTE = rt_fetch(leftmostRTI, parse->rtable);
	Assert(leftmostRTE->rtekind == RTE_SUBQUERY);

	/*
	 * 制作最左边 RTE 的副本并将其添加到 rtable。
	 * 此副本将代表最左边的叶子查询作为 appendrel 的成员。
	 * 原始 RTE 将代表整个 appendrel。
	 * (我们必须这样做，因为上层查询的 Vars 必须被视为引用整个 appendrel。)
	 *
	 * 举例:
	 * 原始: SELECT * FROM t1 UNION ALL SELECT * FROM t2;
	 * 转换期间:
	 * 我们创建一个新的 RTE 来专门代表 t1 (作为子节点)，而原来的 RTE 现在变成了
	 * 代表 (t1 UNION ALL t2) 这个整体结构的“父”节点。
	 */
	childRTE = copyObject(leftmostRTE);
	parse->rtable = lappend(parse->rtable, childRTE);
	childRTI = list_length(parse->rtable);

	/* 修改 setops 树以引用子副本 */
	((RangeTblRef *) leftmostjtnode)->rtindex = childRTI;

	/* 修改原来最左边的 RTE 以将其标记为 appendrel 父级 */
	leftmostRTE->inh = true;

	/*
	 * 为 appendrel 形成一个 RangeTblRef，并将其插入到 FROM 中。
	 * setops 树的顶层 Query 最初应该有一个空的 FromClause。
	 *
	 * 这一步有效地将 "SELECT ... FROM (t1 UNION ALL t2)" 中的集合操作结构
	 * 转换为了 "SELECT ... FROM append_rel_parent" 的形式。
	 */
	rtr = makeNode(RangeTblRef);
	rtr->rtindex = leftmostRTI;
	Assert(parse->jointree->fromlist == NIL);
	parse->jointree->fromlist = list_make1(rtr);

	/*
	 * 现在假装查询没有 setops。我们必须在尝试进行子查询提升之前这样做，
	 * 因为 pull_up_simple_subquery 中有 Assert。
	 *
	 * 通过将 setOperations 置为 NULL，我们告诉规划器的后续部分：
	 * "这不是一个集合操作查询了，这是一个普通的扫描查询（扫描一个追加关系）"。
	 */
	parse->setOperations = NULL;

	/*
	 * 构建 AppendRelInfo 信息，并将 pull_up_subqueries 应用于 UNION ALL 的叶子查询。
	 * (我们必须现在做，因为它们以前没有被 jointree 引用，因此被 pull_up_subqueries 的主调用错过了。)
	 *
	 * 这一步是核心：它遍历 UNION ALL 树的每个分支，为每个分支创建一个 AppendRelInfo，
	 * 告诉优化器："这个分支是 appendrel 父级的一个成员"。
	 */
	pull_up_union_leaf_queries((Node *) topop, root, leftmostRTI, parse, 0);
}


/*
 * reduce_outer_joins
 *		尝试将外连接简化为普通内连接。
 *
 * 这里的思想是，对于如下查询：
 *		SELECT ... FROM a LEFT JOIN b ON (...) WHERE b.y = 42;
 * 如果 WHERE 子句中的 "=" 操作符是严格的（strict），那么当 LEFT JOIN 为 b 的列填充 NULL 时，
 * 严格操作符总会返回 NULL，导致外层 WHERE 失败。因此，连接无需产生 null 扩展的行，
 * 这就等价于普通的内连接（plain join）而不是外连接。
 * （这种场景在手写 SQL 时可能不常见，但在将 quals 下推到复杂视图时很常见。）
 *
 * 更一般地说，如果在 qual 树中有严格的条件约束了外连接可空侧的 Var 非空，
 * 那么外连接可以被弱化（reduced）为更强的连接类型（如内连接）。
 * （对于 FULL JOIN，这一规则分别适用于两侧。）
 *
 * 此外，我们还识别如下场景：
 *		SELECT ... FROM a LEFT JOIN b ON (a.x = b.y) WHERE b.y IS NULL;
 * 如果连接条件对 b.y 是严格的，则只有 null 扩展的行才能通过外层 WHERE，
 * 这实际上等价于反半连接（anti-semijoin）。我们将连接类型从 JOIN_LEFT 改为 JOIN_ANTI。
 * IS NULL 条件变得多余，必须移除以避免错误的选择率估算，但实际移除工作交由 distribute_qual_to_rels 完成。
 *
 * 同时，我们会将 JOIN_RIGHT 反转为 JOIN_LEFT。这样可以简化本函数和后续规划器代码，
 * 主要原因是避免引入 JOIN_REVERSE_ANTI 这种新的连接类型。
 *
 * 为了便于识别严格的 qual 子句，要求本函数在表达式预处理（如 qual 规范化和 JOIN 别名变量展开）之后运行。
 */
void
reduce_outer_joins(PlannerInfo *root)
{
	reduce_outer_joins_state *state;

	/*
	 * 为了避免对过多的 quals 进行严格性检查，我们希望一旦当前 jointree 下没有外连接就停止递归。
	 * 这需要两遍处理：第一遍收集每个连接子树下有哪些基表、是否包含外连接等信息；
	 * 第二遍再结合 quals 递归处理 jointree 并修改连接类型。
	 *
	 * 举例说明:
	 * 1. 外连接消除 (Outer Join Reduction):
	 *    查询: SELECT * FROM a LEFT JOIN b ON a.x = b.y WHERE b.z = 42;
	 *    由于 WHERE 子句对 b.z 是严格的(strict)，即 b.z 为 NULL 时 WHERE 为假，
	 *    因此 LEFT JOIN 生成的 NULL 补全行会被过滤掉。
	 *    优化: 将 LEFT JOIN 转换为 INNER JOIN。
	 *
	 * 2. 转换为反连接 (Anti Join Conversion):
	 *    查询: SELECT * FROM a LEFT JOIN b ON a.x = b.y WHERE b.y IS NULL;
	 *    如果连接条件对 b.y 是严格的，那么只有当 b 中没有匹配行(生成 NULL)时，
	 *    WHERE 子句才为真。
	 *    优化: 将 LEFT JOIN 转换为 ANTI JOIN (JOIN_ANTI)。
	 *
	 * 关于“严格性”(Strictness)的定义:
	 * 如果一个表达式(通常是 WHERE 子句)引用的某个变量为 NULL 时，该表达式的计算结果
	 * 必然为 FALSE 或 NULL (即不可能为 TRUE)，则称该表达式对该变量是“严格的”。
	 * - 严格的例子: "x = 5", "x > 10"。如果 x 为 NULL，结果为 NULL (假)。
	 * - 不严格的例子: "x IS NULL"。如果 x 为 NULL，结果为 TRUE。
	 * 正是利用这一属性，如果上层 WHERE 对右表列是严格的，我们就能确定 LEFT JOIN
	 * 产生的 NULL 补全行会被丢弃，从而安全地将其转换为 INNER JOIN。
	 */
	state = reduce_outer_joins_pass1((Node *) root->parse->jointree);

	/* 如果没有外连接，planner.c 不应该调用本函数 */
	if (state == NULL || !state->contains_outer)
		elog(ERROR, "so where are the outer joins?");

	reduce_outer_joins_pass2((Node *) root->parse->jointree,
							 state, root, NULL, NIL, NIL);
}

/*
 * reduce_outer_joins_pass1 - 第一阶段数据收集
 *
 * 返回一个描述给定 jointree 节点的 state 节点。
 * 这个函数的主要目的是自底向上遍历连接树，收集每个节点包含的基表集合(relids)
 * 以及该子树中是否包含外连接(contains_outer)。
 */
static reduce_outer_joins_state *
reduce_outer_joins_pass1(Node *jtnode)
{
	reduce_outer_joins_state *result;

	/* 分配并初始化 state 结构 */
	result = (reduce_outer_joins_state*)palloc(sizeof(reduce_outer_joins_state));
	result->relids = NULL;
	result->contains_outer = false;
	result->sub_states = NIL;

	/* 空节点直接返回空状态 */
	if (jtnode == NULL)
		return result;

	/* 处理 RangeTblRef (基表引用) */
	if (IsA(jtnode, RangeTblRef))
	{
		/*
		 * 如果是基表引用(RangeTblRef)，则记录该表的 RT index。
		 * 基表本身不包含外连接。
		 */
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		result->relids = bms_make_singleton(varno);
	}
	else if (IsA(jtnode, FromExpr))
	{
		/*
		 * 如果是 FromExpr (通常对应 SQL 中的 FROM 子句列表)，
		 * 则递归处理 fromlist 中的每个子节点。
		 * 结果是所有子节点的并集。
		 */
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
		{
			reduce_outer_joins_state *sub_state;

			sub_state = reduce_outer_joins_pass1(lfirst(l));
			result->relids = bms_add_members(result->relids,
											 sub_state->relids);
			result->contains_outer |= sub_state->contains_outer;
			result->sub_states = lappend(result->sub_states, sub_state);
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		/*
		 * 如果是 JoinExpr (连接表达式)，递归处理左右子树。
		 */
		JoinExpr   *j = (JoinExpr *) jtnode;
		reduce_outer_joins_state *sub_state;

		/* 
		 * join 自己的 RT index 不需要包含在 result->relids 中，
		 * 因为我们只关心基表。
		 * 
		 * 检查当前连接本身是否是外连接。
		 */
		if (IS_OUTER_JOIN(j->jointype))
			result->contains_outer = true;

		/* 处理左子树 */
		sub_state = reduce_outer_joins_pass1(j->larg);
		result->relids = bms_add_members(result->relids,
										 sub_state->relids);
		result->contains_outer |= sub_state->contains_outer;
		result->sub_states = lappend(result->sub_states, sub_state);

		/* 处理右子树 */
		sub_state = reduce_outer_joins_pass1(j->rarg);
		result->relids = bms_add_members(result->relids,
										 sub_state->relids);
		result->contains_outer |= sub_state->contains_outer;
		result->sub_states = lappend(result->sub_states, sub_state);
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return result;
}

/*
 * reduce_outer_joins_pass2 - 第二阶段处理
 *
 *	jtnode: 当前连接树节点
 *	state: 第一阶段收集的状态数据
 *	root: 顶层 PlannerInfo
 *	nonnullable_rels: 由上层 quals 强制非空的基表 relids 集合
 *	nonnullable_vars: 由上层 quals 强制非空的 Vars 列表
 *	forced_null_vars: 由上层 quals 强制为 NULL 的 Vars 列表
 */
static void
reduce_outer_joins_pass2(Node *jtnode,
						 reduce_outer_joins_state *state,
						 PlannerInfo *root,
						 Relids nonnullable_rels,
						 List *nonnullable_vars,
						 List *forced_null_vars)
{
	/*
	 * 第二阶段不应递归到空节点或基表节点，只处理包含外连接的子树
	 */
	if (jtnode == NULL)
		elog(ERROR, "reached empty jointree");
	if (IsA(jtnode, RangeTblRef))
		elog(ERROR, "reached base rel");
	else if (IsA(jtnode, FromExpr))
	{
		/* 处理 FromExpr 节点 */
		FromExpr* f = (FromExpr*)jtnode;
		ListCell   *l;
		ListCell   *s;
		Relids		pass_nonnullable_rels;
		List	   *pass_nonnullable_vars;
		List	   *pass_forced_null_vars;

		/*
		 * 扫描 quals，收集本层可加的约束
		 * 收集当前层级（FromExpr）的 WHERE 条件（quals）所带来的约束信息，
		 * 并将这些信息与上层传递下来的约束合并，以便传递给子节点继续处理。
		 */
		/* 
		 * 合并非空表 relids
		 * find_nonnullable_rels(f->quals): 分析当前层的 WHERE 条件。
		 * 如果条件是 tableA.col = 5，那么 tableA 就被标记为“非空表”。
		 * 这意味着如果下层有 LEFT JOIN 生成了 tableA 的全 NULL 行，这些行会被杀掉。
		 *
		 * 作用：
		 * 用于将 LEFT JOIN 简化为 INNER JOIN。
		 * 如果一个 LEFT JOIN 的右表出现在这个集合里，说明该 LEFT JOIN 可以被消除。
		 */
		pass_nonnullable_rels = find_nonnullable_rels(f->quals);
		pass_nonnullable_rels = bms_add_members(pass_nonnullable_rels,
												nonnullable_rels);
		/*
		 * 合并非空变量列表
		 * 哪些具体的列（变量）被当前的 WHERE 条件严格约束了。
		 * find_nonnullable_vars: 找出那些一旦为 NULL 就会导致条件为 False 的变量。
		 * 例如 WHERE a.x > 10，那么 a.x 就是非空变量。
		 *
		 * 作用: 这是更细粒度的约束信息，主要用于辅助判断。
		 * 在某些复杂的嵌套连接场景下，仅靠表级信息可能不够精确，需要知道具体是哪个列被约束了。
		 */
		pass_nonnullable_vars = find_nonnullable_vars(f->quals);
		pass_nonnullable_vars = list_concat(pass_nonnullable_vars,
											nonnullable_vars);
		/*
		 * 合并强制为 NULL 的变量列表
		 * 哪些变量被 WHERE 条件强制要求必须是 NULL。
		 * find_forced_null_vars: 寻找形如 WHERE col IS NULL 的条件。如果存在，col 就被加入这个列表。
		 *
		 * 作用：
		 * 这是反连接（Anti Join）转换的核心依据。
		 * 如果下层有一个 LEFT JOIN，且其连接条件对某个变量是严格的。
		 * 同时，这个变量出现在 pass_forced_null_vars 列表中（即上层要求它必须是 NULL）。
		 * 那么优化器就会触发 Anti Join 转换。
		 */
		pass_forced_null_vars = find_forced_null_vars(f->quals);
		pass_forced_null_vars = list_concat(pass_forced_null_vars,
											forced_null_vars);

		/* 递归处理有外连接的子树 */
		Assert(list_length(f->fromlist) == list_length(state->sub_states));
		forboth(l, f->fromlist, s, state->sub_states)
		{
			reduce_outer_joins_state *sub_state = lfirst(s);

			if (sub_state->contains_outer)
				reduce_outer_joins_pass2(lfirst(l), sub_state, root,
										 pass_nonnullable_rels,
										 pass_nonnullable_vars,
										 pass_forced_null_vars);
		}
		bms_free(pass_nonnullable_rels);
		/* var 列表无法方便地释放内存 */
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		int			rtindex = j->rtindex;
		JoinType	jointype = j->jointype;

		/* 获取左右子树的状态 */
		reduce_outer_joins_state* left_state = linitial(state->sub_states);
		reduce_outer_joins_state *right_state = lsecond(state->sub_states);

		/* 
		 * 收集当前连接的非空变量列表
		 * 分析当前连接的 quals，找出哪些变量一旦为 NULL 就会导致条件为 False。
		 * 这些变量就是当前连接的“非空变量”。
		 *
		 * 作用：
		 * 用于判断当前连接是否可以被简化为内连接（INNER JOIN）。
		 * 如果一个 LEFT JOIN 的右表出现在这个集合里，说明该 LEFT JOIN 可以被消除。
		 */
		List		*local_nonnullable_vars = NIL;
		bool		computed_local_nonnullable_vars = false;

		/* 
		 * 1. 尝试简化连接类型 (Outer Join Reduction)
		 * 检查上层传递下来的非空表集合(nonnullable_rels)是否与当前连接的左右子树有重叠。
		 * 如果有重叠，说明上层 WHERE 条件要求该子树必须非空，因此可以将外连接简化为内连接。
		 */
		switch (jointype)
		{
			case JOIN_INNER:
				break;
			case JOIN_LEFT:
				/* 
				 * 如果右表被上层约束为非空，LEFT JOIN -> INNER JOIN 
				 * 
				 * "上层"(Upper Level)指的是在查询树中位于当前 JOIN 节点之上的操作，
				 * 通常对应 SQL 中的 WHERE 子句或更高层的 JOIN 条件。
				 * 
				 * 逻辑:
				 * 1. LEFT JOIN 会生成包含 NULL 的行(当右表不匹配时)。
				 * 2. "上层约束为非空"意味着 WHERE 子句会过滤掉右表为 NULL 的行。
				 * 3. 既然这些 NULL 行注定要被上层杀掉，不如直接用 INNER JOIN，
				 *    根本不生成这些行。
				 */
				if (bms_overlap(nonnullable_rels, right_state->relids))
					jointype = JOIN_INNER;
				break;
			case JOIN_RIGHT:
				/* 如果左表被上层约束为非空，RIGHT JOIN -> INNER JOIN */
				if (bms_overlap(nonnullable_rels, left_state->relids))
					jointype = JOIN_INNER;
				break;
			case JOIN_FULL:
				/* 
				 * FULL JOIN 两边都可能产生 NULL，需分别检查 
				 * 
				 * FULL JOIN = (A left join B) UNION (A right join B)
				 * 它可能生成：
				 * 1. 左边为 NULL 的行 (右表有，左表无)
				 * 2. 右边为 NULL 的行 (左表有，右表无)
				 * 
				 * 举例: A={1,2}, B={1,3}
				 * SQL: SELECT * FROM A FULL JOIN B ON A.id = B.id
				 * FULL JOIN 结果: (1,1), (2,NULL), (NULL,3)
				 */
				if (bms_overlap(nonnullable_rels, left_state->relids))
				{
					/* 
					 * 上层约束要求左表必须非空 (例如 WHERE A.id IS NOT NULL)。
					 * 这意味着所有"左边为 NULL"的行((NULL,3))都会被过滤掉。
					 * 剩下的行是 (1,1) 和 (2,NULL)。
					 * 这正是 LEFT JOIN 的行为。
					 * 
					 * 优化后 SQL: SELECT * FROM A LEFT JOIN B ON A.id = B.id WHERE A.id IS NOT NULL
					 */
					if (bms_overlap(nonnullable_rels, right_state->relids))
					{
						/* 
						 * 上层同时要求右表也必须非空 (例如 WHERE B.id IS NOT NULL)。
						 * 那么"右边为 NULL"的行((2,NULL))也会被过滤掉。
						 * 剩下的行只有 (1,1)。
						 * 既不能左边空，也不能右边空 -> 只能是 INNER JOIN。
						 * 
						 * 优化后 SQL: SELECT * FROM A INNER JOIN B ON A.id = B.id WHERE ...
						 */
						jointype = JOIN_INNER; /* 两边都非空 -> INNER */
					}
					else
					{
						/* 只有左边被约束非空 -> 消除左NULL行 -> 降级为 LEFT JOIN */
						jointype = JOIN_LEFT;  /* 只有左边非空 -> LEFT */
					}
				}
				else
				{
					if (bms_overlap(nonnullable_rels, right_state->relids))
					{
						/* 
						 * 上层只要求右表必须非空 (例如 WHERE B.id IS NOT NULL)。
						 * 消除"右边为 NULL"的行((2,NULL))。
						 * 剩下的行是 (1,1) 和 (NULL,3)。
						 * 这正是 RIGHT JOIN 的行为。
						 * 
						 * 优化后 SQL: SELECT * FROM A RIGHT JOIN B ON A.id = B.id WHERE B.id IS NOT NULL
						 */
						jointype = JOIN_RIGHT; /* 只有右边非空 -> RIGHT */
					}
				}
				break;
			case JOIN_SEMI:
			case JOIN_ANTI:
				/* 
				 * 这些类型通常由子查询提升(pull_up_sublinks)引入。
				 * 上层 quals 不可能引用 RHS 的变量(因为它们对上层不可见)，
				 * 所以无法通过上层约束来简化这些连接。
				 * 因为 SEMI/ANTI JOIN 的结果集里只有左表（LHS）的列，右表（RHS）的列在连接完成后就被丢弃了。
				 * 所以，上层的 WHERE 子句根本无法引用右表的列。
				 *
				 * 为什么说“由 pull_up_sublinks 引入”？
				 * 在 PostgreSQL 的查询优化流程中，
				 * JOIN_SEMI 和 JOIN_ANTI 通常不是用户直接写在 SQL 里的（SQL 标准里没有 SEMI JOIN 关键字），
				 * 而是优化器在处理 EXISTS 或 NOT EXISTS 子查询时，
				 * 通过 pull_up_sublinks 步骤将子查询“提升”上来转换而成的。
				 */
				break;
			default:
				elog(ERROR, "unrecognized join type: %d",
					 (int) jointype);
				break;
		}

		/*
		 * 2. 规范化: 将 JOIN_RIGHT 转换为 JOIN_LEFT
		 * 这样后续处理只需要考虑 LEFT JOIN，减少代码复杂度。
		 * 同时交换左右子树和对应的状态。
		 */
		if (jointype == JOIN_RIGHT)
		{
			Node	   *tmparg;

			tmparg = j->larg;
			j->larg = j->rarg;
			j->rarg = tmparg;

			jointype = JOIN_LEFT;
			right_state = linitial(state->sub_states);
			left_state = lsecond(state->sub_states);
		}

		/*
		 * 3. 尝试转换为反连接 (Anti Join Conversion)
		 * 仅针对 JOIN_LEFT。
		 * 逻辑: 如果连接条件(j->quals)对某些变量是严格的(local_nonnullable_vars)，
		 * 且这些变量在上层被强制要求为 NULL (forced_null_vars)，
		 * 并且这些变量确实来自右表(RHS)，
		 * 那么可以将 LEFT JOIN 转换为 ANTI JOIN。
		 * 
		 * 举例:
		 * SQL: SELECT * FROM A LEFT JOIN B ON A.id = B.id WHERE B.id IS NULL
		 * 
		 * 推导过程:
		 * 1. 连接条件 "A.id = B.id" 对 B.id 是严格的。这意味着如果 B.id 为 NULL (数据本身为 NULL)，
		 *    连接条件不成立，该行不会匹配成功。
		 * 2. 因此，LEFT JOIN 结果集中 B.id 为 NULL 的行，只能是"匹配失败"后由系统自动补全的 NULL。
		 * 3. 上层 WHERE 子句 "B.id IS NULL" 专门筛选这些行。
		 * 4. 结论: 查询意图是"找出 A 中在 B 里没有匹配的行"。这正是 ANTI JOIN 的定义。
		 */
		if (jointype == JOIN_LEFT)
		{
			List	   *overlap;

			/*
			 * j->quals 是 ON 子句的条件
			 * find_nonnullable_vars 找出那些“如果为 NULL 则条件必不成立”的变量。
			 */
			local_nonnullable_vars = find_nonnullable_vars(j->quals);
			computed_local_nonnullable_vars = true;

			/* 
			 * 检查连接条件的严格变量与上层强制 NULL 变量的交集。
			 * 必须确认交集中的变量属于右表(right_state->relids)。
			 * forced_null_vars 来自上层的 WHERE 子句，包含形如 WHERE col IS NULL 的变量。
			 * 代码检查：连接条件里的严格变量，是否出现在了上层的“强制 NULL”列表中。
			 */
			overlap = list_intersection(local_nonnullable_vars,
										forced_null_vars);

			/*
			 * 确认这个既严格又被要求为 NULL 的变量，确实是来自 LEFT JOIN 的右侧表。
			 * 举例：
			 * SELECT *
			 * FROM A LEFT JOIN B ON A.id = B.id
			 * WHERE B.id IS NULL;
			 *
			 * 连接条件: A.id = B.id。对 B.id 是严格的。
			 * 上层约束: WHERE B.id IS NULL。B.id 在 forced_null_vars 中
			 * 交集: B.id 既是严格变量，又被要求为 NULL。
			 * 归属: B.id 属于右表 B。
			 * 转换: LEFT JOIN -> ANTI JOIN。
			 *
			 * 数据库不再执行完整的 LEFT JOIN（生成所有行再过滤），
			 * 而是直接使用高效的 Anti Join 算法（如 Hash Anti Join），
			 * 一旦找到匹配就丢弃，只保留没匹配的，效率大幅提升。
			 */
			if (overlap != NIL &&
				bms_overlap(pull_varnos(root, (Node *) overlap),
							right_state->relids))
				jointype = JOIN_ANTI;
		}

		/* 
		 * 4. 应用连接类型变更
		 * 如果 jointype 发生了变化，需要更新 JoinExpr 节点和对应的 RTE。
		 */
		if (rtindex && jointype != j->jointype)
		{
			RangeTblEntry *rte = rt_fetch(rtindex, root->parse->rtable);

			Assert(rte->rtekind == RTE_JOIN);
			Assert(rte->jointype == j->jointype);
			rte->jointype = jointype;
		}
		j->jointype = jointype;

		/* 
		 * 5. 递归处理子节点
		 * 只有当子树中包含外连接时才需要递归(通过 contains_outer 快速判断)。
		 * 这个分支（递归处理子节点）只有在连接树（Join Tree）是多层嵌套结构时才会进入。
		 * 简单来说，就是你的 SQL 里有多个 JOIN，或者 JOIN 里面套了子查询。
		 * 例如：
		 * SELECT *
		 * FROM t1
		 * LEFT JOIN (t2 LEFT JOIN t3 ON t2.id = t3.id)
		 * ON t1.id = t2.id
		 * WHERE t3.val IS NOT NULL;
		 */
		if (left_state->contains_outer || right_state->contains_outer)
		{
			Relids		local_nonnullable_rels;
			List	   *local_forced_null_vars;
			Relids		pass_nonnullable_rels;
			List	   *pass_nonnullable_vars;
			List	   *pass_forced_null_vars;

			/*
			 * 准备向下传递的约束信息。
			 * 
			 * 规则:
			 * - INNER/SEMI: 连接条件是严格的，可以将本层的约束(j->quals)与上层约束合并传递。
			 * - LEFT/ANTI: 连接条件对左表不严格(左表总是保留)，所以不能将本层约束传给左表，
			 *              也不能将上层约束传给右表(因为右表可能补 NULL)。
			 *              但本层的约束可以传给右表(因为如果右表不满足连接条件，就会补 NULL)。
			 * - FULL: 不产生任何有用的约束传递，所以直接跳过。
			 */
			if (jointype != JOIN_FULL)
			{
				local_nonnullable_rels = find_nonnullable_rels(j->quals);

				if (!computed_local_nonnullable_vars)
					local_nonnullable_vars = find_nonnullable_vars(j->quals);
				local_forced_null_vars = find_forced_null_vars(j->quals);

				if (jointype == JOIN_INNER || jointype == JOIN_SEMI)
				{
					local_nonnullable_rels = bms_add_members(local_nonnullable_rels,
															 nonnullable_rels);
					local_nonnullable_vars = list_concat(local_nonnullable_vars,
														 nonnullable_vars);
					local_forced_null_vars = list_concat(local_forced_null_vars,
														 forced_null_vars);
				}
			}
			else
			{
				local_nonnullable_rels = NULL;
				local_forced_null_vars = NIL;
			}

			/* 递归处理左子树 */
			if (left_state->contains_outer)
			{
				if (jointype == JOIN_INNER || jointype == JOIN_SEMI)
				{
					/* INNER/SEMI: 传递合并后的所有约束 */
					pass_nonnullable_rels = local_nonnullable_rels;
					pass_nonnullable_vars = local_nonnullable_vars;
					pass_forced_null_vars = local_forced_null_vars;
				}
				else if (jointype != JOIN_FULL) /* LEFT/ANTI */
				{
					/* 
					 * LEFT/ANTI: 左子树是保留侧(preserved side)。
					 * 本层的连接条件(j->quals)不能约束左表(因为即使不满足也会保留行)。
					 * 但上层的约束(nonnullable_rels)依然有效。
					 */
					pass_nonnullable_rels = nonnullable_rels;
					pass_nonnullable_vars = nonnullable_vars;
					pass_forced_null_vars = forced_null_vars;
				}
				else
				{
					/* FULL: 无法传递任何约束 */
					pass_nonnullable_rels = NULL;
					pass_nonnullable_vars = NIL;
					pass_forced_null_vars = NIL;
				}
				reduce_outer_joins_pass2(j->larg, left_state, root,
										 pass_nonnullable_rels,
										 pass_nonnullable_vars,
										 pass_forced_null_vars);
			}

			/* 递归处理右子树 */
			if (right_state->contains_outer)
			{
				if (jointype != JOIN_FULL)	/* INNER/LEFT/SEMI/ANTI */
				{
					/*
					 * 对于 INNER/SEMI，传递合并后的约束。
					 * 对于 LEFT/ANTI，右子树是空值生成侧(nullable side)。
					 * 本层的连接条件(j->quals)对右表是有效的约束(不满足则补 NULL)。
					 * 所以这里传递的是 local_... (包含了本层约束)。
					 * 注意: 对于 LEFT/ANTI，local_... 并没有包含上层约束(见上方 if 逻辑)。
					 */
					pass_nonnullable_rels = local_nonnullable_rels;
					pass_nonnullable_vars = local_nonnullable_vars;
					pass_forced_null_vars = local_forced_null_vars;
				}
				else
				{
					pass_nonnullable_rels = NULL;
					pass_nonnullable_vars = NIL;
					pass_forced_null_vars = NIL;
				}
				reduce_outer_joins_pass2(j->rarg, right_state, root,
										 pass_nonnullable_rels,
										 pass_nonnullable_vars,
										 pass_forced_null_vars);
			}
			bms_free(local_nonnullable_rels);
		}
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}


/*
 * remove_useless_result_rtes
 *		尝试从连接树中移除 RTE_RESULT 类型的 RTE。
 *
 * 我们可以利用 RTE_RESULT 总是返回一行且没有输出列的特性，
 * 在连接树中将其删除。如果它与其他表进行内连接，则可以直接移除。
 * 对于某些外连接场景，也可以进行优化，具体见下文。
 *
 * 这些优化依赖于识别空（恒为真）的 quals，因此建议在表达式预处理后
 * 再进行本优化，这样可以识别更多可优化的场景。通常 RTE_RESULT 的出现
 * 是因为上拉了子查询或 VALUES 子句，这可能会将 Vars 替换为常量，
 * 使 quals 更容易被化简为恒真。同时，由于部分优化依赖于外连接类型，
 * 建议先执行 reduce_outer_joins()。
 *
 * 如果有 PlaceHolderVar 引用 RTE_RESULT，则移除时需将该 relid
 * 从 PHV 的 phrels 集合中删除，但不能删到空集。如果删到空集且该
 * RTE_RESULT 是外连接的直接子节点，则必须放弃移除，因为没有其他地方
 * 可以计算该 PlaceHolderVar（此时 RTE_RESULT 实际有输出列）。
 * 如果是内连接，则通常可以将 PlaceHolderVar 的 phrels 改为在内连接处
 * 计算，这样是安全的，因为我们只关心 PHV 是否在正确的外连接上下方计算。
 * 但不能将 PHV 的计算推迟到其被使用之后，因此还需检查其他连接输入是否
 * 有对 PHV 的引用。
 *
 * 过去曾尝试在 pull_up_subqueries() 阶段做这项工作，但单独处理更简单，
 * 效果也更好。
 */
void
remove_useless_result_rtes(PlannerInfo *root)
{
	ListCell   *cell;
	ListCell   *prev;
	ListCell   *next;

	/* 顶层连接树必须是 FromExpr */
	Assert(IsA(root->parse->jointree, FromExpr));
	/* 递归处理连接树 */
	root->parse->jointree = (FromExpr *)
		remove_useless_results_recurse(root, (Node *) root->parse->jointree);
	/* 处理后仍应为 FromExpr */
	Assert(IsA(root->parse->jointree, FromExpr));

	/*
	 * 移除所有引用 RTE_RESULT 的 PlanRowMark。
	 * 对于刚刚被移除的 RTE_RESULT 必须移除 PlanRowMark；
	 * 即使未被移除的 RTE_RESULT，其 PlanRowMark 也可以删除：
	 * 因为该 RTE 只有一行输出，EPQ 无需标记和恢复该行。
	 *
	 * 对于存活的 RTE_RESULT，移除 PlanRowMark 是必须的，
	 * 否则会生成 whole-row Var，执行器无法支持。
	 */
	prev = NULL;
	for (cell = list_head(root->rowMarks); cell; cell = next)
	{
		PlanRowMark *rc = (PlanRowMark *) lfirst(cell);

		next = lnext(cell);
		if (rt_fetch(rc->rti, root->parse->rtable)->rtekind == RTE_RESULT)
			root->rowMarks = list_delete_cell(root->rowMarks, cell, prev);
		else
			prev = cell;
	}
}

/*
 * remove_useless_results_recurse
 *		Recursive guts of remove_useless_result_rtes.
 *
 * This recursively processes the jointree and returns a modified jointree.
 */
static Node *
remove_useless_results_recurse(PlannerInfo *root, Node *jtnode)
{
	Assert(jtnode != NULL);
	if (IsA(jtnode, RangeTblRef))
	{
		/* Can't immediately do anything with a RangeTblRef */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		Relids		result_relids = NULL;
		ListCell   *cell;
		ListCell   *prev;
		ListCell   *next;

		/*
		 * We can drop RTE_RESULT rels from the fromlist so long as at least
		 * one child remains, since joining to a one-row table changes
		 * nothing.  (But we can't drop a RTE_RESULT that computes PHV(s) that
		 * are needed by some sibling.  The cleanup transformation below would
		 * reassign the PHVs to be computed at the join, which is too late for
		 * the sibling's use.)  The easiest way to mechanize this rule is to
		 * modify the list in-place, using list_delete_cell.
		 */
		prev = NULL;
		for (cell = list_head(f->fromlist); cell; cell = next)
		{
			Node	   *child = (Node *) lfirst(cell);
			int			varno;

			/* Recursively transform child ... */
			child = remove_useless_results_recurse(root, child);
			/* ... and stick it back into the tree */
			lfirst(cell) = child;
			next = lnext(cell);

			/*
			 * If it's an RTE_RESULT with at least one sibling, and no sibling
			 * references dependent PHVs, we can drop it.  We don't yet know
			 * what the inner join's final relid set will be, so postpone
			 * cleanup of PHVs etc till after this loop.
			 */
			if (list_length(f->fromlist) > 1 &&
				(varno = get_result_relid(root, child)) != 0 &&
				!find_dependent_phvs_in_jointree(root, (Node *) f, varno))
			{
				f->fromlist = list_delete_cell(f->fromlist, cell, prev);
				result_relids = bms_add_member(result_relids, varno);
			}
			else
				prev = cell;
		}

		/*
		 * Clean up if we dropped any RTE_RESULT RTEs.  This is a bit
		 * inefficient if there's more than one, but it seems better to
		 * optimize the support code for the single-relid case.
		 */
		if (result_relids)
		{
			int			varno = -1;

			while ((varno = bms_next_member(result_relids, varno)) >= 0)
				remove_result_refs(root, varno, (Node *) f);
		}

		/*
		 * If we're not at the top of the jointree, it's valid to simplify a
		 * degenerate FromExpr into its single child.  (At the top, we must
		 * keep the FromExpr since Query.jointree is required to point to a
		 * FromExpr.)
		 */
		if (f != root->parse->jointree &&
			f->quals == NULL &&
			list_length(f->fromlist) == 1)
			return (Node *) linitial(f->fromlist);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		int			varno;

		/* First, recurse */
		j->larg = remove_useless_results_recurse(root, j->larg);
		j->rarg = remove_useless_results_recurse(root, j->rarg);

		/* Apply join-type-specific optimization rules */
		switch (j->jointype)
		{
			case JOIN_INNER:

				/*
				 * An inner join is equivalent to a FromExpr, so if either
				 * side was simplified to an RTE_RESULT rel, we can replace
				 * the join with a FromExpr with just the other side; and if
				 * the qual is empty (JOIN ON TRUE) then we can omit the
				 * FromExpr as well.
				 *
				 * Just as in the FromExpr case, we can't simplify if the
				 * other input rel references any PHVs that are marked as to
				 * be evaluated at the RTE_RESULT rel, because we can't
				 * postpone their evaluation in that case.  But we only have
				 * to check this in cases where it's syntactically legal for
				 * the other input to have a LATERAL reference to the
				 * RTE_RESULT rel.  Only RHSes of inner and left joins are
				 * allowed to have such refs.
				 */
				if ((varno = get_result_relid(root, j->larg)) != 0 &&
					!find_dependent_phvs_in_jointree(root, j->rarg, varno))
				{
					remove_result_refs(root, varno, j->rarg);
					if (j->quals)
						jtnode = (Node *)
							makeFromExpr(list_make1(j->rarg), j->quals);
					else
						jtnode = j->rarg;
				}
				else if ((varno = get_result_relid(root, j->rarg)) != 0)
				{
					remove_result_refs(root, varno, j->larg);
					if (j->quals)
						jtnode = (Node *)
							makeFromExpr(list_make1(j->larg), j->quals);
					else
						jtnode = j->larg;
				}
				break;
			case JOIN_LEFT:

				/*
				 * We can simplify this case if the RHS is an RTE_RESULT, with
				 * two different possibilities:
				 *
				 * If the qual is empty (JOIN ON TRUE), then the join can be
				 * strength-reduced to a plain inner join, since each LHS row
				 * necessarily has exactly one join partner.  So we can always
				 * discard the RHS, much as in the JOIN_INNER case above.
				 * (Again, the LHS could not contain a lateral reference to
				 * the RHS.)
				 *
				 * Otherwise, it's still true that each LHS row should be
				 * returned exactly once, and since the RHS returns no columns
				 * (unless there are PHVs that have to be evaluated there), we
				 * don't much care if it's null-extended or not.  So in this
				 * case also, we can just ignore the qual and discard the left
				 * join.
				 */
				if ((varno = get_result_relid(root, j->rarg)) != 0 &&
					(j->quals == NULL ||
					 !find_dependent_phvs(root, varno)))
				{
					remove_result_refs(root, varno, j->larg);
					jtnode = j->larg;
				}
				break;
			case JOIN_SEMI:

				/*
				 * We may simplify this case if the RHS is an RTE_RESULT; the
				 * join qual becomes effectively just a filter qual for the
				 * LHS, since we should either return the LHS row or not.  For
				 * simplicity we inject the filter qual into a new FromExpr.
				 *
				 * There is a fine point about PHVs that are supposed to be
				 * evaluated at the RHS.  Such PHVs could only appear in the
				 * semijoin's qual, since the rest of the query cannot
				 * reference any outputs of the semijoin's RHS.  Therefore,
				 * they can't actually go to null before being examined, and
				 * it'd be OK to just remove the PHV wrapping.  We don't have
				 * infrastructure for that, but remove_result_refs() will
				 * relabel them as to be evaluated at the LHS, which is fine.
				 */
				if ((varno = get_result_relid(root, j->rarg)) != 0)
				{
					remove_result_refs(root, varno, j->larg);
					if (j->quals)
						jtnode = (Node *)
							makeFromExpr(list_make1(j->larg), j->quals);
					else
						jtnode = j->larg;
				}
				break;
			case JOIN_FULL:
			case JOIN_ANTI:
				/* We have no special smarts for these cases */
				break;
			default:
				/* Note: JOIN_RIGHT should be gone at this point */
				elog(ERROR, "unrecognized join type: %d",
					 (int) j->jointype);
				break;
		}
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return jtnode;
}

/*
 * get_result_relid
 *		If jtnode is a RangeTblRef for an RTE_RESULT RTE, return its relid;
 *		otherwise return 0.
 */
static int
get_result_relid(PlannerInfo *root, Node *jtnode)
{
	int			varno;

	if (!IsA(jtnode, RangeTblRef))
		return 0;
	varno = ((RangeTblRef *) jtnode)->rtindex;
	if (rt_fetch(varno, root->parse->rtable)->rtekind != RTE_RESULT)
		return 0;
	return varno;
}

/*
 * remove_result_refs
 *		Helper routine for dropping an unneeded RTE_RESULT RTE.
 *
 * This doesn't physically remove the RTE from the jointree, because that's
 * more easily handled in remove_useless_results_recurse.  What it does do
 * is the necessary cleanup in the rest of the tree: we must adjust any PHVs
 * that may reference the RTE.  Be sure to call this at a point where the
 * jointree is valid (no disconnected nodes).
 *
 * Note that we don't need to process the append_rel_list, since RTEs
 * referenced directly in the jointree won't be appendrel members.
 *
 * varno is the RTE_RESULT's relid.
 * newjtloc is the jointree location at which any PHVs referencing the
 * RTE_RESULT should be evaluated instead.
 */
static void
remove_result_refs(PlannerInfo *root, int varno, Node *newjtloc)
{
	/* Fix up PlaceHolderVars as needed */
	/* If there are no PHVs anywhere, we can skip this bit */
	if (root->glob->lastPHId != 0)
	{
		Relids		subrelids;

		subrelids = get_relids_in_jointree(newjtloc, false);
		Assert(!bms_is_empty(subrelids));
		substitute_phv_relids((Node *) root->parse, varno, subrelids);
		fix_append_rel_relids(root->append_rel_list, varno, subrelids);
	}

	/*
	 * We also need to remove any PlanRowMark referencing the RTE, but we
	 * postpone that work until we return to remove_useless_result_rtes.
	 */
}


/*
 * find_dependent_phvs - are there any PlaceHolderVars whose relids are
 * exactly the given varno?
 *
 * find_dependent_phvs should be used when we want to see if there are
 * any such PHVs anywhere in the Query.  Another use-case is to see if
 * a subtree of the join tree contains such PHVs; but for that, we have
 * to look not only at the join tree nodes themselves but at the
 * referenced RTEs.  For that, use find_dependent_phvs_in_jointree.
 */

typedef struct
{
	Relids		relids;
	int			sublevels_up;
} find_dependent_phvs_context;

static bool
find_dependent_phvs_walker(Node *node,
						   find_dependent_phvs_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		if (phv->phlevelsup == context->sublevels_up &&
			bms_equal(context->relids, phv->phrels))
			return true;
		/* fall through to examine children */
	}
	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node,
								   find_dependent_phvs_walker,
								   (void *) context, 0);
		context->sublevels_up--;
		return result;
	}
	/* Shouldn't need to handle planner auxiliary nodes here */
	Assert(!IsA(node, SpecialJoinInfo));
	Assert(!IsA(node, AppendRelInfo));
	Assert(!IsA(node, PlaceHolderInfo));
	Assert(!IsA(node, MinMaxAggInfo));

	return expression_tree_walker(node, find_dependent_phvs_walker,
								  (void *) context);
}

static bool
find_dependent_phvs(PlannerInfo *root, int varno)
{
	find_dependent_phvs_context context;

	/* If there are no PHVs anywhere, we needn't work hard */
	if (root->glob->lastPHId == 0)
		return false;

	context.relids = bms_make_singleton(varno);
	context.sublevels_up = 0;

	return query_tree_walker(root->parse,
							 find_dependent_phvs_walker,
							 (void *) &context,
							 0);
}

static bool
find_dependent_phvs_in_jointree(PlannerInfo *root, Node *node, int varno)
{
	find_dependent_phvs_context context;
	Relids		subrelids;
	int			relid;

	/* If there are no PHVs anywhere, we needn't work hard */
	if (root->glob->lastPHId == 0)
		return false;

	context.relids = bms_make_singleton(varno);
	context.sublevels_up = 0;

	/*
	 * See if the jointree fragment itself contains references (in join quals)
	 */
	if (find_dependent_phvs_walker(node, &context))
		return true;

	/*
	 * Otherwise, identify the set of referenced RTEs (we can ignore joins,
	 * since they should be flattened already, so their join alias lists no
	 * longer matter), and tediously check each RTE.  We can ignore RTEs that
	 * are not marked LATERAL, though, since they couldn't possibly contain
	 * any cross-references to other RTEs.
	 */
	subrelids = get_relids_in_jointree(node, false);
	relid = -1;
	while ((relid = bms_next_member(subrelids, relid)) >= 0)
	{
		RangeTblEntry *rte = rt_fetch(relid, root->parse->rtable);

		if (rte->lateral &&
			range_table_entry_walker(rte,
									 find_dependent_phvs_walker,
									 (void *) &context,
									 0))
			return true;
	}

	return false;
}

/*
 * substitute_phv_relids - adjust PlaceHolderVar relid sets after pulling up
 * a subquery or removing an RTE_RESULT jointree item
 *
 * Find any PlaceHolderVar nodes in the given tree that reference the
 * pulled-up relid, and change them to reference the replacement relid(s).
 *
 * NOTE: although this has the form of a walker, we cheat and modify the
 * nodes in-place.  This should be OK since the tree was copied by
 * pullup_replace_vars earlier.  Avoid scribbling on the original values of
 * the bitmapsets, though, because expression_tree_mutator doesn't copy those.
 */

typedef struct
{
	int			varno;
	int			sublevels_up;
	Relids		subrelids;
} substitute_phv_relids_context;

static bool
substitute_phv_relids_walker(Node *node,
							 substitute_phv_relids_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		if (phv->phlevelsup == context->sublevels_up &&
			bms_is_member(context->varno, phv->phrels))
		{
			phv->phrels = bms_union(phv->phrels,
									context->subrelids);
			phv->phrels = bms_del_member(phv->phrels,
										 context->varno);
			/* Assert we haven't broken the PHV */
			Assert(!bms_is_empty(phv->phrels));
		}
		/* fall through to examine children */
	}
	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node,
								   substitute_phv_relids_walker,
								   (void *) context, 0);
		context->sublevels_up--;
		return result;
	}
	/* Shouldn't need to handle planner auxiliary nodes here */
	Assert(!IsA(node, SpecialJoinInfo));
	Assert(!IsA(node, AppendRelInfo));
	Assert(!IsA(node, PlaceHolderInfo));
	Assert(!IsA(node, MinMaxAggInfo));

	return expression_tree_walker(node, substitute_phv_relids_walker,
								  (void *) context);
}

static void
substitute_phv_relids(Node *node, int varno, Relids subrelids)
{
	substitute_phv_relids_context context;

	context.varno = varno;
	context.sublevels_up = 0;
	context.subrelids = subrelids;

	/*
	 * Must be prepared to start with a Query or a bare expression tree.
	 */
	query_or_expression_tree_walker(node,
									substitute_phv_relids_walker,
									(void *) &context,
									0);
}

/*
 * fix_append_rel_relids: update RT-index fields of AppendRelInfo nodes
 *
 * When we pull up a subquery, any AppendRelInfo references to the subquery's
 * RT index have to be replaced by the substituted relid (and there had better
 * be only one).  We also need to apply substitute_phv_relids to their
 * translated_vars lists, since those might contain PlaceHolderVars.
 *
 * We assume we may modify the AppendRelInfo nodes in-place.
 */
static void
fix_append_rel_relids(List *append_rel_list, int varno, Relids subrelids)
{
	ListCell   *l;
	int			subvarno = -1;

	/*
	 * We only want to extract the member relid once, but we mustn't fail
	 * immediately if there are multiple members; it could be that none of the
	 * AppendRelInfo nodes refer to it.  So compute it on first use. Note that
	 * bms_singleton_member will complain if set is not singleton.
	 */
	foreach(l, append_rel_list)
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(l);

		/* The parent_relid shouldn't ever be a pullup target */
		Assert(appinfo->parent_relid != varno);

		if (appinfo->child_relid == varno)
		{
			if (subvarno < 0)
				subvarno = bms_singleton_member(subrelids);
			appinfo->child_relid = subvarno;
		}

		/* Also fix up any PHVs in its translated vars */
		substitute_phv_relids((Node *) appinfo->translated_vars,
							  varno, subrelids);
	}
}

/*
 * get_relids_in_jointree: get set of RT indexes present in a jointree
 *
 * If include_joins is true, join RT indexes are included; if false,
 * only base rels are included.
 */
Relids
get_relids_in_jointree(Node *jtnode, bool include_joins)
{
	Relids		result = NULL;

	if (jtnode == NULL)
		return result;
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		result = bms_make_singleton(varno);
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
		{
			result = bms_join(result,
							  get_relids_in_jointree(lfirst(l),
													 include_joins));
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		result = get_relids_in_jointree(j->larg, include_joins);
		result = bms_join(result,
						  get_relids_in_jointree(j->rarg, include_joins));
		if (include_joins && j->rtindex)
			result = bms_add_member(result, j->rtindex);
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return result;
}

/*
 * get_relids_for_join: get set of base RT indexes making up a join
 */
Relids
get_relids_for_join(Query *query, int joinrelid)
{
	Node	   *jtnode;

	jtnode = find_jointree_node_for_rel((Node *) query->jointree,
										joinrelid);
	if (!jtnode)
		elog(ERROR, "could not find join node %d", joinrelid);
	return get_relids_in_jointree(jtnode, false);
}

/*
 * find_jointree_node_for_rel: locate jointree node for a base or join RT index
 *
 * Returns NULL if not found
 */
static Node *
find_jointree_node_for_rel(Node *jtnode, int relid)
{
	if (jtnode == NULL)
		return NULL;
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		if (relid == varno)
			return jtnode;
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
		{
			jtnode = find_jointree_node_for_rel(lfirst(l), relid);
			if (jtnode)
				return jtnode;
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		if (relid == j->rtindex)
			return jtnode;
		jtnode = find_jointree_node_for_rel(j->larg, relid);
		if (jtnode)
			return jtnode;
		jtnode = find_jointree_node_for_rel(j->rarg, relid);
		if (jtnode)
			return jtnode;
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return NULL;
}
