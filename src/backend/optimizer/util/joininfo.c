/*-------------------------------------------------------------------------
 *
 * joininfo.c
 *	  joininfo list manipulation routines
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/joininfo.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/joininfo.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"


/*
 * have_relevant_joinclause
 *      检测是否存在涉及给定两个关系的连接子句。
 *      
 * 函数作用：在查询优化阶段判断两个关系之间是否存在有意义的连接条件，为连接顺序和连接类型决策提供依据。
 * 
 * 参数说明：
 * - root: 查询规划器的全局信息结构，包含查询上下文和优化状态
 * - rel1: 第一个要检查的关系节点（表或已连接的关系集合）
 * - rel2: 第二个要检查的关系节点（表或已连接的关系集合）
 * 
 * 返回值：
 * - bool: 如果存在相关的连接子句，返回true；否则返回false
 * 
 * 注意：连接子句不一定只能用这两个关系来计算。这是有意为之。例如考虑：
 *      SELECT * FROM a, b, c WHERE a.x = (b.y + c.z)
 * 如果 a 比其他表大很多，可能值得先对 b 和 c 做笛卡尔积，然后对 a.x 做索引扫描。
 * 因此，即使该连接子句不能在这一步连接时应用，我们也应该认为它是连接 b 和 c 的理由。
 */
bool
have_relevant_joinclause(PlannerInfo *root,
                         RelOptInfo *rel1, RelOptInfo *rel2)
{
    bool        result = false;  /* 初始化结果为false，表示默认不存在相关连接子句 */
    List       *joininfo;       /* 要扫描的连接信息列表 */
    Relids      other_relids;   /* 另一个关系的关系ID集合 */
    ListCell   *l;              /* 列表遍历指针 */

    /*
     * 可以扫描任一关系的 joininfo 列表；选用较短的那个以提高效率。
     * 这是一种性能优化策略，通过扫描较短的列表来减少遍历次数。
     */
    if (list_length(rel1->joininfo) <= list_length(rel2->joininfo))
    {
        joininfo = rel1->joininfo;    /* 选择rel1的joininfo列表 */
        other_relids = rel2->relids;  /* 存储rel2的关系ID集合 */
    }
    else
    {
        joininfo = rel2->joininfo;    /* 选择rel2的joininfo列表 */
        other_relids = rel1->relids;  /* 存储rel1的关系ID集合 */
    }

    /*
     * 遍历选择的joininfo列表，检查每个RestrictInfo是否与另一个关系相关
     * joininfo列表中存储了关系可能参与的所有连接条件信息
     */
    foreach(l, joininfo)
    {
        RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);  /* 获取当前连接条件信息 */

        /*
         * 使用bms_overlap检查另一个关系的ID集合是否与连接条件所需的关系ID有重叠
         * 这表示这两个关系之间存在直接的连接条件
         */
        if (bms_overlap(other_relids, rinfo->required_relids))
        {
            result = true;  /* 找到相关连接条件，设置结果为true */
            break;          /* 提前退出循环，避免不必要的检查 */
        }
    }

	/*
	 * 如果在joininfo中没有找到相关连接条件，还需要检查EquivalenceClass数据结构
     * 等价类中可能包含未被显式放入joininfo列表的隐含连接关系
     * 只有当两个关系都有等价类连接（has_eclass_joins标记为true）时才需要检查
     */
    if (!result && rel1->has_eclass_joins && rel2->has_eclass_joins)
        result = have_relevant_eclass_joinclause(root, rel1, rel2);

    return result;  /* 返回最终检测结果 */
}



/*
 * add_join_clause_to_rels
 *	  Add 'restrictinfo' to the joininfo list of each relation it requires.
 *
 * Note that the same copy of the restrictinfo node is linked to by all the
 * lists it is in.  This allows us to exploit caching of information about
 * the restriction clause (but we must be careful that the information does
 * not depend on context).
 *
 * 'restrictinfo' describes the join clause
 * 'join_relids' is the list of relations participating in the join clause
 *				 (there must be more than one)
 */
void
add_join_clause_to_rels(PlannerInfo *root,
						RestrictInfo *restrictinfo,
						Relids join_relids)
{
	int			cur_relid;

	cur_relid = -1;
	while ((cur_relid = bms_next_member(join_relids, cur_relid)) >= 0)
	{
		RelOptInfo *rel = find_base_rel(root, cur_relid);

		rel->joininfo = lappend(rel->joininfo, restrictinfo);
	}
}

/*
 * remove_join_clause_from_rels
 *	  Delete 'restrictinfo' from all the joininfo lists it is in
 *
 * This reverses the effect of add_join_clause_to_rels.  It's used when we
 * discover that a relation need not be joined at all.
 *
 * 'restrictinfo' describes the join clause
 * 'join_relids' is the list of relations participating in the join clause
 *				 (there must be more than one)
 */
void
remove_join_clause_from_rels(PlannerInfo *root,
							 RestrictInfo *restrictinfo,
							 Relids join_relids)
{
	int			cur_relid;

	cur_relid = -1;
	while ((cur_relid = bms_next_member(join_relids, cur_relid)) >= 0)
	{
		RelOptInfo *rel = find_base_rel(root, cur_relid);

		/*
		 * Remove the restrictinfo from the list.  Pointer comparison is
		 * sufficient.
		 */
		Assert(list_member_ptr(rel->joininfo, restrictinfo));
		rel->joininfo = list_delete_ptr(rel->joininfo, restrictinfo);
	}
}
