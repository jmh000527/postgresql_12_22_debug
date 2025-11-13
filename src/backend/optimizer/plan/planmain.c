/*-------------------------------------------------------------------------
 *
 * planmain.c
 *	  Routines to plan a single query
 *
 * What's in a name, anyway?  The top-level entry point of the planner/
 * optimizer is over in planner.c, not here as you might think from the
 * file name.  But this is the main code for planning a basic join operation,
 * shorn of features like subselects, inheritance, aggregates, grouping,
 * and so on.  (Those are the things planner.c deals with.)
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/planmain.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/inherit.h"
#include "optimizer/optimizer.h"
#include "optimizer/orclauses.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/placeholder.h"
#include "optimizer/planmain.h"


/*
 * query_planner
 *	  为一个基本查询生成一个路径（即简化的计划），该查询可能涉及联接但不包含任何更复杂的特性。
 *
 * 由于 query_planner 不处理顶层处理（分组、排序等），它无法自行选择最佳路径。
 * 因此它返回用于连接顶层的 RelOptInfo，由调用者（grouping_planner）在该 rel 的
 * 存活路径中进行选择。
 *
 * root 描述要规划的查询
 * qp_callback 是在可以安全计算 query_pathkeys 时用来计算它的函数
 * qp_extra 是可选的额外数据，传递给 qp_callback
 *
 * 注意：PlannerInfo 节点还包含一个 query_pathkeys 字段，它告诉 query_planner
 * 最终输出计划所期望的排序顺序。该值在调用时不可用，而是在我们完成合并查询的等价类后
 * 由 qp_callback 计算。（在这之前无法构造规范的 pathkeys。）
 */
RelOptInfo *
query_planner(PlannerInfo *root,
			  query_pathkeys_callback qp_callback, void *qp_extra)
{
	Query	   *parse = root->parse;
	List	   *joinlist;
	RelOptInfo *final_rel;

	/*
	 * 将 planner 列表初始化为空。
	 *
	 * 注意：append_rel_list 已由 subquery_planner 设置，因此这里不要修改。
	 */
	root->join_rel_list = NIL;
	root->join_rel_hash = NULL;
	root->join_rel_level = NULL;
	root->join_cur_level = 0;
	root->canon_pathkeys = NIL;
	root->left_join_clauses = NIL;
	root->right_join_clauses = NIL;
	root->full_join_clauses = NIL;
	root->join_info_list = NIL;
	root->placeholder_list = NIL;
	root->fkey_list = NIL;
	root->initial_rels = NIL;

	/*
	 * 为更快访问构建扁平化的 rangetable（这是安全的，因为 rangetable 不会再改变），并为索引基表
	 * 设置一个空数组。
	 */
	setup_simple_rel_arrays(root);

	/*
	 * 在 jointree 是单个 RTE_RESULT 关系的简化情况下，绕过本函数的其余部分，直接创建一个
	 * RelOptInfo 及其一条访问路径。这个优化值得做，因为它适用于像 "SELECT expression"
	 * 和 "INSERT ... VALUES()" 这样的常见情况。
	 */
	Assert(parse->jointree->fromlist != NIL);
	if (list_length(parse->jointree->fromlist) == 1)
	{
		Node	   *jtnode = (Node *) linitial(parse->jointree->fromlist);

		if (IsA(jtnode, RangeTblRef))
		{
			int			varno = ((RangeTblRef *) jtnode)->rtindex;
			RangeTblEntry *rte = root->simple_rte_array[varno];

			Assert(rte != NULL);
			if (rte->rtekind == RTE_RESULT)
			{
				/* 直接为其创建 RelOptInfo */
				final_rel = build_simple_rel(root, varno, NULL);

				/*
				 * 如果查询总体上允许并行性，检查 quals 是否受并行限制。
				 * （此处不需要检查 final_rel->reltarget，因为此时它为空。
				 * 查询 tlist 中任何受并行限制的内容稍后会处理。）通常这行为
				 * 很愚蠢，因为仅有 Result 的计划通常不适合并行化。但如果
				 * force_parallel_mode 被打开，我们希望尽可能在并行 worker 中
				 * 执行 Result，所以必须进行此检查。
				 */
				if (root->glob->parallelModeOK &&
					force_parallel_mode != FORCE_PARALLEL_OFF)
					final_rel->consider_parallel =
						is_parallel_safe(root, parse->jointree->quals);

				/*
				 * 它的唯一路径是一个简单的 Result path。这里我们稍微作弊，使用
				 * GroupResultPath，这样可以直接把 quals 塞进去而无需预处理。
				 * （但从某种角度看，没有 FROM 的 SELECT 是一种退化的分组情况，
				 * 所以这并不算太大的作弊。）
				 */
				add_path(final_rel, (Path *)
						 create_group_result_path(root, final_rel,
												  final_rel->reltarget,
												  (List *) parse->jointree->quals));

				/* 选择最便宜的路径（在此情形下很容易...） */
				set_cheapest(final_rel);

				/*
				 * 我们仍然需要调用 qp_callback，以防它像 "SELECT 2+2 ORDER BY 1" 之类。
				 */
				(*qp_callback) (root, qp_extra);

				return final_rel;
			}
		}
	}

	/*
	 * 用每个 AppendRelInfo 填充 append_rel_array，以便可以通过子 relid 进行直接查找。
	 */
	setup_append_rel_array(root);

	/*
	 * 为查询中使用的所有基表构建 RelOptInfo 节点。Appendrel 成员关系（“other rels”）
	 * 稍后再添加。
	 *
	 * 注意：我们通过搜索 jointree 来查找 baserels 而不是扫描 rangetable 的原因是 rangetable
	 * 可能包含不在查询中实际使用的 RTEs，例如视图。我们不希望为它们创建 RelOptInfos。
	 */
	add_base_rels_to_query(root, (Node *) parse->jointree);

	/*
	 * 检查 targetlist 和 join tree，为所有引用的 Vars 向 baserel targetlists 添加条目，
	 * 并为所有引用的 PlaceHolderVars 生成 PlaceHolderInfo 条目。限制和连接子句被添加到
	 * 提到的关系的适当列表中。我们还为可证明等价的表达式构建等价类（EquivalenceClasses）。
	 * SpecialJoinInfo 列表也被构建以保存有关连接顺序限制的信息。最后，形成一个目标
	 * joinlist，供 make_one_rel() 使用。
	 */
	build_base_rel_tlists(root, root->processed_tlist);

	find_placeholders_in_jointree(root);

	find_lateral_references(root);

	joinlist = deconstruct_jointree(root);

	/*
	 * 现在重新考虑任何被推迟的外连接条件，因为我们已经构建了等价类。
	 * （这可能会导致更多的等价类添加或合并。）
	 */
	reconsider_outer_join_clauses(root);

	/*
	 * 如果我们形成了任何等价类，按需生成额外的限制子句。（隐含的连接子句将在稍后按需生成。）
	 */
	generate_base_implied_equalities(root);

	/*
	 * 我们已完成等价集合的合并，因此现在可以生成规范形式的 pathkeys；
	 * 计算 PlannerInfo 中的 query_pathkeys 和其他 pathkeys 字段。
	 */
	(*qp_callback) (root, qp_extra);

	/*
	 * 检查在子查询上拔高期间生成的任何 “placeholder” 表达式。确保它们需要的 Vars
	 * 在相应的连接级别被标记为需要。这必须在 join removal 之前完成，因为它可能
	 * 导致 Vars 或 placeholders 在连接之上被需要，而先前未标记为需要。
	 */
	fix_placeholder_input_needed_levels(root);

	/*
	 * 删除任何无用的外连接。理想情况下这将在 jointree 预处理期间完成，但必要的信息
	 * 在构建 baserel 数据结构并分类 qual 子句之前不可用。
	 */
	joinlist = remove_useless_joins(root, joinlist);

	/*
	 * 同样，将具有唯一内表的 semijoins 简化为普通内连接。这同样直到现在才能完成，因为
	 * 之前缺乏必要信息。
	 */
	reduce_unique_semijoins(root);

	/*
	 * 现在将 "placeholders" 分配到需要的基表。由于连接移除可能改变一个 placeholder
	 * 是否可在基表上评估，因此必须在 join removal 之后进行。
	 */
	add_placeholders_to_base_rels(root);

	/*
	 * 在我们确定了 PlaceHolderVar 的评估级别后，构造 lateral reference 集合。
	 */
	create_lateral_join_info(root);

	/*
	 * 将外键与等价类和连接 quals 匹配。此操作必须在确定等价类之后进行，并且
	 * 等到 join removal 之后有利，因为我们可以跳过涉及已删除关系的外键处理。
	 */
	match_foreign_keys_to_quals(root);

	/*
	 * 查找可以从中提取单关系限制 OR 子句的联接 OR 子句。
	 */
	extract_restriction_or_clauses(root);

	/*
	 * 现在通过为 appendrels 添加其子关系的 "otherrels" 来展开 appendrels。
	 * 我们将此延迟到最后，以便在处理每个 baserel 时可用尽可能多的信息，
	 * 包括所有限制子句。这样我们可以修剪不满足限制子句的分区。
	 * 另外注意，有些信息例如 lateral_relids 会在此从 baserels 传播到 otherrels，
	 * 因此必须已经计算完成。
	 */
	add_other_rels_to_query(root);

	/*
	 * 准备开始主规划。
	 */
	final_rel = make_one_rel(root, joinlist);

	/* 检查是否至少构建了一个可用路径 */
	if (!final_rel || !final_rel->cheapest_total_path ||
		final_rel->cheapest_total_path->param_info != NULL)
		elog(ERROR, "failed to construct the join relation");

	return final_rel;
}
