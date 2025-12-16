/*-------------------------------------------------------------------------
 *
 * relnode.c
 *	  Relation-node lookup/construction routines
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/relnode.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "miscadmin.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/inherit.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/placeholder.h"
#include "optimizer/plancat.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "partitioning/partbounds.h"
#include "utils/hsearch.h"


typedef struct JoinHashEntry
{
	Relids		join_relids;	/* hash key --- MUST BE FIRST */
	RelOptInfo *join_rel;
} JoinHashEntry;

static void build_joinrel_tlist(PlannerInfo *root, RelOptInfo *joinrel,
								RelOptInfo *input_rel);
static List *build_joinrel_restrictlist(PlannerInfo *root,
										RelOptInfo *joinrel,
										RelOptInfo *outer_rel,
										RelOptInfo *inner_rel);
static void build_joinrel_joinlist(RelOptInfo *joinrel,
								   RelOptInfo *outer_rel,
								   RelOptInfo *inner_rel);
static List *subbuild_joinrel_restrictlist(RelOptInfo *joinrel,
										   List *joininfo_list,
										   List *new_restrictlist);
static List *subbuild_joinrel_joinlist(RelOptInfo *joinrel,
									   List *joininfo_list,
									   List *new_joininfo);
static void set_foreign_rel_properties(RelOptInfo *joinrel,
									   RelOptInfo *outer_rel, RelOptInfo *inner_rel);
static void add_join_rel(PlannerInfo *root, RelOptInfo *joinrel);
static void build_joinrel_partition_info(RelOptInfo *joinrel,
										 RelOptInfo *outer_rel, RelOptInfo *inner_rel,
										 List *restrictlist, JoinType jointype);
static void build_child_join_reltarget(PlannerInfo *root,
									   RelOptInfo *parentrel,
									   RelOptInfo *childrel,
									   int nappinfos,
									   AppendRelInfo **appinfos);


/*
 * setup_simple_rel_arrays
 *    准备用于快速访问基表的数组
 *
 * 函数功能：
 *    为查询优化器创建两个重要的数组，用于通过RT索引（Range Table索引）快速访问
 *    关系表的RelOptInfo结构和RangeTblEntry结构，提高查询优化过程中的数据访问效率
 *
 * 参数：
 *    root - PlannerInfo指针，包含查询优化器的所有上下文信息
 */
void
setup_simple_rel_arrays(PlannerInfo *root)
{
    Index      rti;          /* 关系表索引变量，用于数组填充 */
    ListCell  *lc;           /* 用于遍历rtable列表的列表单元格指针 */

    /* 
     * 数组使用RT索引访问（1..N）
	 * 设置数组大小为range table长度+1，+1是因为RT索引从1开始计数而不是从0开始
	 * root->parse->rtable: 这是解析树中的“范围表”（Range Table）。
	 * 它是一个链表，包含了查询中引用的所有表、子查询、函数等。
	 */
    root->simple_rel_array_size = list_length(root->parse->rtable) + 1;

    /* 
     * 分配simple_rel_array数组并初始化为全NULL
     * simple_rel_array用于存储每个关系表的优化信息（RelOptInfo结构）
     * 使用palloc0确保所有元素初始化为NULL
     */
    root->simple_rel_array = (RelOptInfo **)
        palloc0(root->simple_rel_array_size * sizeof(RelOptInfo *));

    /* 
     * 分配simple_rte_array数组并初始化为全NULL
     * simple_rte_array是rtable列表的数组等价物，用于快速访问RangeTblEntry
     */
    root->simple_rte_array = (RangeTblEntry **)
        palloc0(root->simple_rel_array_size * sizeof(RangeTblEntry *));
    
    /* 
     * 初始化RT索引计数器，从1开始（PostgreSQL的RT索引惯例）
     */
    rti = 1;
    
    /* 
     * 遍历range table列表，将每个RangeTblEntry复制到simple_rte_array数组中
     * 遍历使用PostgreSQL的标准foreach宏，lc是当前列表项的指针
     */
    foreach(lc, root->parse->rtable)
    {
        /* 获取当前列表项中的RangeTblEntry指针 */
        RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

        /* 将RangeTblEntry存储到对应的数组位置，并递增索引 */
        root->simple_rte_array[rti++] = rte;
    }
}


/*
 * setup_append_rel_array
 *    填充append_rel_array数组，以允许通过子关系ID直接查找AppendRelInfo结构。
 *
 * 如果没有AppendRelInfo结构，数组将保持未分配状态。
 * 它将一个链表（append_rel_list）转换成一个数组（append_rel_array），
 * 以便可以通过子表的 ID（child_relid）直接 O(1) 找到对应的父子映射信息（AppendRelInfo）。
 *
 * 在 PostgreSQL 优化器中，
 * 继承表（分区表） 和 被展平的 UNION ALL 在内部都被统一视为 “追加关系”（Append Relation）。
 *
 * 例如：
 * SELECT * FROM
 *		(SELECT a FROM t1
 * 		UNION ALL
 * 		SELECT b FROM t2) AS sub;
 *
 * 解析器会生成一个范围表（Range Table, rtable），大概长这样：
 *  - RTE 1: sub (子查询，逻辑上的“父表”)
 *  - RTE 2: t1 (UNION 的第一个分支)
 *  - RTE 3: t2 (UNION 的第二个分支)
 * 2. 优化阶段（展平）：
 * 优化器发现这是一个简单的 UNION ALL，于是决定将其展平。它会创建 AppendRelInfo 节点来记录映射关系：
 *  - 映射 1: 父表 sub (ID=1) <--- 子表 t1 (ID=2)。列 a 映射到 sub 的输出列。
 *  - 映射 2: 父表 sub (ID=1) <--- 子表 t2 (ID=3)。列 b 映射到 sub 的输出列。
 * 这两个映射节点会被放入 root->append_rel_list 链表中。
 * 
 * 3. setup_append_rel_array 的工作：
 * 这个函数会将上述链表转换为数组 root->append_rel_array：
 *  - Index 1 (sub): NULL (因为它不是别人的子表，它是父表)。
 *  - Index 2 (t1): 指向 映射 1 的指针。
 *  - Index 3 (t2): 指向 映射 2 的指针。
 * 为什么要这么做？
 * 当优化器后续处理 t1 (RTE 2) 时，它需要知道：“我生成的列 a 最终要对应到父查询的哪一列？”
 * 它只需要查 root->append_rel_array[2]，就能立刻找到映射信息，而不需要去遍历整个 UNION ALL 结构。
 */
void
setup_append_rel_array(PlannerInfo *root)
{
    ListCell   *lc;             /* 用于遍历append_rel_list的列表单元格指针 */
    int         size = list_length(root->parse->rtable) + 1; /* 数组大小，+1因为RT索引从1开始 */

    /* 
     * 检查是否存在任何AppendRelInfo结构
     * 如果append_rel_list为空，则设置数组为NULL并直接返回
     */
    if (root->append_rel_list == NIL)
    {
        root->append_rel_array = NULL;
        return;
    }

    /* 
     * 分配append_rel_array数组并初始化为全NULL
     * 数组用于存储AppendRelInfo指针，索引为子关系ID
     */
    root->append_rel_array = (AppendRelInfo **)
        palloc0(size * sizeof(AppendRelInfo *));

    /* 
     * 遍历append_rel_list中的所有AppendRelInfo结构
     * 将每个结构按照其子关系ID放入对应的数组位置
     */
    foreach(lc, root->append_rel_list)
    {
        /* 从列表单元格中提取AppendRelInfo节点 */
        AppendRelInfo *appinfo = lfirst_node(AppendRelInfo, lc);
        /* 获取子关系ID，用作数组索引 */
        int         child_relid = appinfo->child_relid;

        /* 一致性检查：确保子关系ID在数组范围内 */
        Assert(child_relid < size);

        /* 错误检查：确保不会覆盖现有的子关系映射 */
        if (root->append_rel_array[child_relid])
            elog(ERROR, "child relation already exists");

        /* 将AppendRelInfo存储到以子关系ID为索引的数组位置 */
        root->append_rel_array[child_relid] = appinfo;
    }
}


/*
 * expand_planner_arrays
 *		Expand the PlannerInfo's per-RTE arrays by add_size members
 *		and initialize the newly added entries to NULLs
 */
void
expand_planner_arrays(PlannerInfo *root, int add_size)
{
	int			new_size;

	Assert(add_size > 0);

	new_size = root->simple_rel_array_size + add_size;

	root->simple_rte_array = (RangeTblEntry **)
		repalloc(root->simple_rte_array,
				 sizeof(RangeTblEntry *) * new_size);
	MemSet(root->simple_rte_array + root->simple_rel_array_size,
		   0, sizeof(RangeTblEntry *) * add_size);

	root->simple_rel_array = (RelOptInfo **)
		repalloc(root->simple_rel_array,
				 sizeof(RelOptInfo *) * new_size);
	MemSet(root->simple_rel_array + root->simple_rel_array_size,
		   0, sizeof(RelOptInfo *) * add_size);

	if (root->append_rel_array)
	{
		root->append_rel_array = (AppendRelInfo **)
			repalloc(root->append_rel_array,
					 sizeof(AppendRelInfo *) * new_size);
		MemSet(root->append_rel_array + root->simple_rel_array_size,
			   0, sizeof(AppendRelInfo *) * add_size);
	}
	else
	{
		root->append_rel_array = (AppendRelInfo **)
			palloc0(sizeof(AppendRelInfo *) * new_size);
	}

	root->simple_rel_array_size = new_size;
}

/*
 * build_simple_rel
 *	  为基本关系或“其他”关系构造一个新的 RelOptInfo。
 */
RelOptInfo *
build_simple_rel(PlannerInfo* root, int relid, RelOptInfo* parent)
{
	RelOptInfo* rel;
	RangeTblEntry* rte;

	/* 关系不应已存在 */
	Assert(relid > 0 && relid < root->simple_rel_array_size);
	if (root->simple_rel_array[relid] != NULL)
		elog(ERROR, "rel %d already exists", relid);

	/* 获取关系的 RTE */
	rte = root->simple_rte_array[relid];
	Assert(rte != NULL);

	rel = makeNode(RelOptInfo);
	rel->reloptkind = parent ? RELOPT_OTHER_MEMBER_REL : RELOPT_BASEREL;
	rel->relids = bms_make_singleton(relid);
	rel->rows = 0;
	/* 仅当不需要检索所有元组时，廉价的启动成本才有趣 */
	rel->consider_startup = (root->tuple_fraction > 0);
	rel->consider_param_startup = false;	/* 稍后可能会更改 */
	rel->consider_parallel = false; /* 稍后可能会更改 */
	rel->reltarget = create_empty_pathtarget();
	rel->pathlist = NIL;
	rel->ppilist = NIL;
	rel->partial_pathlist = NIL;
	rel->cheapest_startup_path = NULL;
	rel->cheapest_total_path = NULL;
	rel->cheapest_unique_path = NULL;
	rel->cheapest_parameterized_paths = NIL;
	rel->relid = relid;
	rel->rtekind = rte->rtekind;
	/* min_attr, max_attr, attr_needed, attr_widths 在下面设置 */
	rel->lateral_vars = NIL;
	rel->indexlist = NIL;
	rel->statlist = NIL;
	rel->pages = 0;
	rel->tuples = 0;
	rel->allvisfrac = 0;
	rel->subroot = NULL;
	rel->subplan_params = NIL;
	rel->rel_parallel_workers = -1; /* 在 get_relation_info 中设置 */
	rel->serverid = InvalidOid;
	rel->userid = rte->checkAsUser;
	rel->useridiscurrent = false;
	rel->fdwroutine = NULL;
	rel->fdw_private = NULL;
	rel->unique_for_rels = NIL;
	rel->non_unique_for_rels = NIL;
	rel->baserestrictinfo = NIL;
	rel->baserestrictcost.startup = 0;
	rel->baserestrictcost.per_tuple = 0;
	rel->baserestrict_min_security = UINT_MAX;
	rel->joininfo = NIL;
	rel->has_eclass_joins = false;
	rel->consider_partitionwise_join = false;	/* 稍后可能会更改 */
	rel->part_scheme = NULL;
	rel->nparts = 0;
	rel->boundinfo = NULL;
	rel->partition_qual = NIL;
	rel->part_rels = NULL;
	rel->partexprs = NULL;
	rel->nullable_partexprs = NULL;
	rel->partitioned_child_rels = NIL;

	/*
	 * 将各种信息向下传递到继承层次结构。
	 */
	if (parent) {
		/*
		 * 每个直接或间接子级都想知道其最顶层父级的 relids。
		 */
		if (parent->top_parent_relids)
			rel->top_parent_relids = parent->top_parent_relids;
		else
			rel->top_parent_relids = bms_copy(parent->relids);

		/*
		 * 还要将横向引用信息从 appendrel 父级关系传播到其子级关系。
		 * 我们有意为每个子级关系提供相同的最小参数化，即使某些子级可能
		 * 不引用所有横向关系也是如此。这是因为父级的任何追加路径无论如何
		 * 都必须对每个子级具有相同的参数化，并且强制额外的 reparameterize_path()
		 * 调用没有任何价值。同样，对父级的横向引用会阻止对每个子级使用
		 * 否则可移动的连接关系。
		 *
		 * 子级关系可能有自己的子级，在这种情况下，最顶层父级的横向信息
		 * 会一直向下传播。
		 */
		rel->direct_lateral_relids = parent->direct_lateral_relids;
		rel->lateral_relids = parent->lateral_relids;
		rel->lateral_referencers = parent->lateral_referencers;
	}
	else {
		rel->top_parent_relids = NULL;
		rel->direct_lateral_relids = NULL;
		rel->lateral_relids = NULL;
		rel->lateral_referencers = NULL;
	}

	/* 检查 rtable 条目的类型 */
	switch (rte->rtekind) {
	case RTE_RELATION:
		/* 表 --- 从系统目录检索统计信息 */
		get_relation_info(root, rte->relid, rte->inh, rel);
		break;
	case RTE_SUBQUERY:
	case RTE_FUNCTION:
	case RTE_TABLEFUNC:
	case RTE_VALUES:
	case RTE_CTE:
	case RTE_NAMEDTUPLESTORE:

		/*
		 * 子查询、函数、tablefunc、值列表、CTE 或 ENR --- 设置
		 * 属性范围和数组
		 *
		 * 注意：0 包含在范围内以支持整行 Vars
		 */
		rel->min_attr = 0;
		rel->max_attr = list_length(rte->eref->colnames);
		rel->attr_needed = (Relids*)
			palloc0((rel->max_attr - rel->min_attr + 1) * sizeof(Relids));
		rel->attr_widths = (int32*)
			palloc0((rel->max_attr - rel->min_attr + 1) * sizeof(int32));
		break;
	case RTE_RESULT:
		/* RTE_RESULT 没有列，也不可能有整行 Var */
		rel->min_attr = 0;
		rel->max_attr = -1;
		rel->attr_needed = NULL;
		rel->attr_widths = NULL;
		break;
	default:
		elog(ERROR, "unrecognized RTE kind: %d",
			(int)rte->rtekind);
		break;
	}

	/*
	 * 将父级的 quals 复制到子级，并适当替换变量。
	 * 如果出现任何常量 false 或 NULL 子句，我们可以立即将子级标记为 dummy。
	 * （我们必须立即这样做，以便在 expand_partitioned_rtentry 中递归时修剪正常工作。）
	 */
	if (parent) {
		AppendRelInfo* appinfo = root->append_rel_array[relid];

		Assert(appinfo != NULL);
		if (!apply_child_basequals(root, parent, rel, rte, appinfo)) {
			/*
			 * 某些限制子句在替换后简化为常量 FALSE 或 NULL，
			 * 因此无需扫描此子级。
			 */
			mark_dummy_rel(rel);
		}
	}

	/* 将完成的结构保存在查询的 simple_rel_array 中 */
	root->simple_rel_array[relid] = rel;

	return rel;
}

/*
 * find_base_rel
 *      根据 Relid（rtindex） 从 simple_rel_array 中查找已经存在的 RelOptInfo。
 *
 * 此函数用于从查询规划器的数据结构中快速检索指定ID的关系信息。
 * 函数假定要查找的关系必须已经存在，如果找不到则报错。
 */
RelOptInfo *
find_base_rel(PlannerInfo *root, /* 查询规划器信息 */
             int relid)         /* 要查找的关系ID */
{
    RelOptInfo *rel;  /* 存储找到的关系信息 */

    /* 断言：确保关系ID为正数 */
    Assert(relid > 0);

    /* 检查关系ID是否在simple_rel_array数组的有效范围内 */
    if (relid < root->simple_rel_array_size)
    {
        /* 直接从数组中获取关系信息 */
        rel = root->simple_rel_array[relid];
        /* 如果找到有效关系，直接返回 */
        if (rel)
            return rel;
    }

    /* 如果找不到关系，报错并终止执行 */
    elog(ERROR, "no relation entry for relid %d", relid);

    /* 返回NULL只是为了避免编译器警告，实际上不会执行到这里 */
    return NULL;
}


/*
 * build_join_rel_hash
 *	  Construct the auxiliary hash table for join relations.
 */
static void
build_join_rel_hash(PlannerInfo *root)
{
	HTAB	   *hashtab;
	HASHCTL		hash_ctl;
	ListCell   *l;

	/* Create the hash table */
	MemSet(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(Relids);
	hash_ctl.entrysize = sizeof(JoinHashEntry);
	hash_ctl.hash = bitmap_hash;
	hash_ctl.match = bitmap_match;
	hash_ctl.hcxt = CurrentMemoryContext;
	hashtab = hash_create("JoinRelHashTable",
						  256L,
						  &hash_ctl,
						  HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);

	/* Insert all the already-existing joinrels */
	foreach(l, root->join_rel_list)
	{
		RelOptInfo *rel = (RelOptInfo *) lfirst(l);
		JoinHashEntry *hentry;
		bool		found;

		hentry = (JoinHashEntry *) hash_search(hashtab,
											   &(rel->relids),
											   HASH_ENTER,
											   &found);
		Assert(!found);
		hentry->join_rel = rel;
	}

	root->join_rel_hash = hashtab;
}

/*
 * find_join_rel
 *	  返回与 'relids'（一组 RT 索引）对应的关系条目（RelOptInfo），如果不存在则返回 NULL。仅用于连接关系。
 */
RelOptInfo *
find_join_rel(PlannerInfo *root, Relids relids)
{
	/*
	 * 当列表变得“太长”时，切换为哈希查找。阈值是任意的，仅在此处已知。
	 */
	if (!root->join_rel_hash && list_length(root->join_rel_list) > 32)
		build_join_rel_hash(root);

	/*
	 * 根据情况使用哈希表查找或线性搜索。
	 *
	 * 注意：看似多余的 hashkey 变量用于避免直接取 relids 的地址；除非编译器非常智能，否则这样做会导致 relids 被移出寄存器，从而可能降低列表搜索的速度。
	 */
	if (root->join_rel_hash)
	{
		Relids		hashkey = relids;
		JoinHashEntry *hentry;

		hentry = (JoinHashEntry *) hash_search(root->join_rel_hash,
											   &hashkey,
											   HASH_FIND,
											   NULL);
		if (hentry)
			return hentry->join_rel;
	}
	else
	{
		ListCell   *l;

		foreach(l, root->join_rel_list)
		{
			RelOptInfo *rel = (RelOptInfo *) lfirst(l);

			if (bms_equal(rel->relids, relids))
				return rel;
		}
	}

	return NULL;
}

/*
 * set_foreign_rel_properties
 *		Set up foreign-join fields if outer and inner relation are foreign
 *		tables (or joins) belonging to the same server and assigned to the same
 *		user to check access permissions as.
 *
 * In addition to an exact match of userid, we allow the case where one side
 * has zero userid (implying current user) and the other side has explicit
 * userid that happens to equal the current user; but in that case, pushdown of
 * the join is only valid for the current user.  The useridiscurrent field
 * records whether we had to make such an assumption for this join or any
 * sub-join.
 *
 * Otherwise these fields are left invalid, so GetForeignJoinPaths will not be
 * called for the join relation.
 *
 */
static void
set_foreign_rel_properties(RelOptInfo *joinrel, RelOptInfo *outer_rel,
						   RelOptInfo *inner_rel)
{
	if (OidIsValid(outer_rel->serverid) &&
		inner_rel->serverid == outer_rel->serverid)
	{
		if (inner_rel->userid == outer_rel->userid)
		{
			joinrel->serverid = outer_rel->serverid;
			joinrel->userid = outer_rel->userid;
			joinrel->useridiscurrent = outer_rel->useridiscurrent || inner_rel->useridiscurrent;
			joinrel->fdwroutine = outer_rel->fdwroutine;
		}
		else if (!OidIsValid(inner_rel->userid) &&
				 outer_rel->userid == GetUserId())
		{
			joinrel->serverid = outer_rel->serverid;
			joinrel->userid = outer_rel->userid;
			joinrel->useridiscurrent = true;
			joinrel->fdwroutine = outer_rel->fdwroutine;
		}
		else if (!OidIsValid(outer_rel->userid) &&
				 inner_rel->userid == GetUserId())
		{
			joinrel->serverid = outer_rel->serverid;
			joinrel->userid = inner_rel->userid;
			joinrel->useridiscurrent = true;
			joinrel->fdwroutine = outer_rel->fdwroutine;
		}
	}
}

/*
 * add_join_rel - 将指定的连接关系添加到PlannerInfo的连接关系列表中
 *
 * 该函数负责将新的连接关系加入查询优化器的全局信息结构中，以便后续优化步骤可以访问。
 * 同时，如果存在辅助哈希表，也会将该连接关系添加到哈希表中以提高查找效率。
 *
 * 参数说明:
 * - root: 查询优化器的全局信息结构，包含所有已知的关系和连接信息
 * - joinrel: 要添加的连接关系优化信息结构
 *
 * 返回值:
 * - void: 无返回值
 *
 * 工作原理:
 * 1. 将新的连接关系追加到join_rel_list列表的末尾（GEQO遗传算法要求）
 * 2. 如果存在辅助哈希表join_rel_hash，则将该连接关系也插入到哈希表中
 * 3. 使用位图集合relids作为哈希键，确保每个连接关系只被添加一次
 * 4. 通过Assert断言确保该连接关系之前未被添加过
 */
static void
add_join_rel(PlannerInfo *root, RelOptInfo *joinrel)
{
	/* 
	 * GEQO遗传查询优化器要求我们将新的连接关系追加到列表末尾！
	 * 这是为了保持特定的搜索顺序，不能使用prepend或其他插入方式。
	 */
	root->join_rel_list = lappend(root->join_rel_list, joinrel);

	/* 
	 * 如果存在辅助哈希表，则也将该连接关系存储到哈希表中以提高查找效率。
	 * 哈希表主要用于快速查找已存在的连接关系，避免重复创建。
	 */
	if (root->join_rel_hash)
	{
		JoinHashEntry *hentry;  /* 哈希表条目指针 */
		bool		found;      /* 标记是否在哈希表中找到了该项 */

		/* 
		 * 在哈希表中搜索并插入新的连接关系条目。
		 * 使用HASH_ENTER模式表示如果不存在则创建新条目。
		 * &(joinrel->relids)作为搜索键，即连接涉及的关系ID集合的位图。
		 */
		hentry = (JoinHashEntry *) hash_search(root->join_rel_hash,
											   &(joinrel->relids),
											   HASH_ENTER,
											   &found);
		
		/* 
		 * 断言确保该连接关系之前不存在于哈希表中。
		 * 因为我们是在创建新的连接关系，所以不应该已经存在于哈希表里。
		 */
		Assert(!found);
		
		/* 将连接关系指针存储到哈希表条目中 */
		hentry->join_rel = joinrel;
	}
}

/*
 * build_join_rel
 *	  返回由两个给定关系的并集对应的关系条目，如果不存在则创建一个新的关系条目。
 *
 * 'joinrelids' 是唯一标识该连接的 Relids 集合
 * 'outer_rel' 和 'inner_rel' 是要连接的关系节点
 * 'sjinfo': 连接上下文信息（处理特殊连接类型）
 * 'restrictlist_ptr': 结果变量。如果不为 NULL，*restrictlist_ptr
 *		将接收适用于该对可连接关系的 RestrictInfo 节点列表。
 *
 * restrictlist_ptr 让该函数的 API 有些不优雅，但它避免了重复计算 restrictlist...
 *
 * 参数：
 *   root - 规划器信息结构，包含查询相关的全局数据
 *   joinrelids - 连接后关系的ID集合（两个输入关系ID的并集）
 *   outer_rel - 连接操作的外关系（左表）
 *   inner_rel - 连接操作的内关系（右表）
 *   sjinfo - 特殊连接信息（如外连接、半连接等信息）
 *   restrictlist_ptr - 用于返回连接限制条件列表的输出参数
 *
 * 返回值：
 *   表示两个关系连接结果的RelOptInfo结构指针
 */
RelOptInfo *
build_join_rel(PlannerInfo *root,
		   Relids joinrelids,
		   RelOptInfo *outer_rel,
		   RelOptInfo *inner_rel,
		   SpecialJoinInfo *sjinfo,
		   List **restrictlist_ptr)
{
	RelOptInfo *joinrel;  /* 连接关系结构 */
	List	   *restrictlist; /* 连接限制条件列表 */

	/* 断言：确保两个输入关系都是常规关系类型，不是特殊关系 */
	Assert(!IS_OTHER_REL(outer_rel) && !IS_OTHER_REL(inner_rel));

	/*
	 * 检查是否已经有该组基表的 joinrel，从 PlannerInfo->join_rel_list 或 PlannerInfo->join_rel_hash 中查找
	 * 这是为了避免重复创建表示相同表集合的连接关系
	 */
	joinrel = find_join_rel(root, joinrelids);

	if (joinrel)
	{
		/*
		 * 关系已存在，只需为该对组件关系计算 restrictlist，对约束条件进行筛选
		 * 注意：此时不需要重新创建整个关系结构
		 */
		if (restrictlist_ptr)
			*restrictlist_ptr = build_joinrel_restrictlist(root,
							   joinrel,
							   outer_rel,
							   inner_rel);
		return joinrel;  /* 返回已存在的关系 */
	}

	/*
	 * 如果目标 RelOptInfo 在 PlannerInfo->join_rel_list 或 PlannerInfo->join_rel_hash 中不存在，则创建一个新的
	 * 这是函数的主要工作：从头构建一个新的连接关系结构
	 */
	joinrel = makeNode(RelOptInfo);  /* 创建新的关系节点 */
	joinrel->reloptkind = RELOPT_JOINREL;  /* 标记为连接关系 */
	joinrel->relids = bms_copy(joinrelids);  /* 复制连接关系ID集合 */
	joinrel->rows = 0;  /* 行数初始化为0，稍后会计算 */
	/* 仅当不是检索所有元组时，才关注低启动成本 */
	joinrel->consider_startup = (root->tuple_fraction > 0);
	joinrel->consider_param_startup = false;  /* 初始不考虑参数化启动 */
	joinrel->consider_parallel = false;  /* 初始不考虑并行执行 */
	joinrel->reltarget = create_empty_pathtarget();  /* 创建空的路径目标 */
	joinrel->pathlist = NIL;  /* 路径列表初始化为空 */
	joinrel->ppilist = NIL;  /* 参数化路径信息列表初始化为空 */
	joinrel->partial_pathlist = NIL;  /* 部分路径列表初始化为空 */
	joinrel->cheapest_startup_path = NULL;  /* 启动成本最低路径 */
	joinrel->cheapest_total_path = NULL;  /* 总成本最低路径 */
	joinrel->cheapest_unique_path = NULL;  /* 唯一值成本最低路径 */
	joinrel->cheapest_parameterized_paths = NIL;  /* 参数化路径列表 */
	/* 从子节点初始化 direct_lateral_relids，稍后完善 */
	joinrel->direct_lateral_relids =
		bms_union(outer_rel->direct_lateral_relids,
			  inner_rel->direct_lateral_relids);
	joinrel->lateral_relids = min_join_parameterization(root, joinrel->relids,
						outer_rel, inner_rel);  /* 计算最小参数化 */
	joinrel->relid = 0;  /* 0 表示不是基本关系（baserel） */
	joinrel->rtekind = RTE_JOIN;  /* 关系表条目类型为连接 */
	joinrel->min_attr = 0;  /* 属性范围最小值 */
	joinrel->max_attr = 0;  /* 属性范围最大值 */
	joinrel->attr_needed = NULL;  /* 需要的属性位图 */
	joinrel->attr_widths = NULL;  /* 属性宽度数组 */
	joinrel->lateral_vars = NIL;  /* LATERAL引用的变量列表 */
	joinrel->lateral_referencers = NULL;  /* 引用该关系的LATERAL关系 */
	joinrel->indexlist = NIL;  /* 索引列表（连接关系没有索引） */
	joinrel->statlist = NIL;  /* 统计信息列表 */
	joinrel->pages = 0;  /* 页数估计 */
	joinrel->tuples = 0;  /* 元组数量估计 */
	joinrel->allvisfrac = 0;  /* 全可见分数 */
	joinrel->subroot = NULL;  /* 子查询根节点 */
	joinrel->subplan_params = NIL;  /* 子计划参数 */
	joinrel->rel_parallel_workers = -1;  /* 并行工作线程数 */
	joinrel->serverid = InvalidOid;  /* 服务器ID */
	joinrel->userid = InvalidOid;  /* 用户ID */
	joinrel->useridiscurrent = false;  /* 用户ID是否为当前用户 */
	joinrel->fdwroutine = NULL;  /* 外部数据包装器例程 */
	joinrel->fdw_private = NULL;  /* 外部数据包装器私有数据 */
	joinrel->unique_for_rels = NIL;  /* 对哪些关系集是唯一的 */
	joinrel->non_unique_for_rels = NIL;  /* 对哪些关系集不唯一 */
	joinrel->baserestrictinfo = NIL;  /* 基本关系限制条件 */
	joinrel->baserestrictcost.startup = 0;  /* 基本限制启动成本 */
	joinrel->baserestrictcost.per_tuple = 0;  /* 基本限制每行成本 */
	joinrel->baserestrict_min_security = UINT_MAX;  /* 基本限制最低安全级别 */
	joinrel->joininfo = NIL;  /* 连接信息列表 */
	joinrel->has_eclass_joins = false;  /* 是否有等价类连接 */
	joinrel->consider_partitionwise_join = false;  /* 是否考虑分区级连接 */
	joinrel->top_parent_relids = NULL;  /* 顶级父关系ID */
	joinrel->part_scheme = NULL;  /* 分区方案 */
	joinrel->nparts = 0;  /* 分区数量 */
	joinrel->boundinfo = NULL;  /* 分区边界信息 */
	joinrel->partition_qual = NIL;  /* 分区限定条件 */
	joinrel->part_rels = NULL;  /* 分区关系 */
	joinrel->partexprs = NULL;  /* 分区表达式 */
	joinrel->nullable_partexprs = NULL;  /* 可为空的分区表达式 */
	joinrel->partitioned_child_rels = NIL;  /* 分区子关系列表 */

	/* 计算与外部表相关的信息 */
	set_foreign_rel_properties(joinrel, outer_rel, inner_rel);

	/*
	 * 创建一个新的 tlist（目标列表），仅包含需要从该连接输出的 vars（即用于更高层连接条件或最终输出）
	 *
	 * 注意：连接关系的 tlist 顺序取决于首次尝试构建它的外部和内部关系对。但内容应始终一致。
	 */
	build_joinrel_tlist(root, joinrel, outer_rel);  /* 从外关系构建目标列表 */
	build_joinrel_tlist(root, joinrel, inner_rel);  /* 从内关系构建目标列表 */
	add_placeholders_to_joinrel(root, joinrel, outer_rel, inner_rel);  /* 添加占位变量 */

	/*
	 * add_placeholders_to_joinrel 也会处理在此处计算的 PlaceHolderVars 的 ph_lateral 集，
	 * 因此现在可以完善 direct_lateral_relids。类似于 min_join_parameterization 中
	 * 闭包 lateral_relids 的计算，但这里必须考虑新增的 PHV。
	 */
	/* 移除已包含在关系自身中的lateral引用 */
	joinrel->direct_lateral_relids =
		bms_del_members(joinrel->direct_lateral_relids, joinrel->relids);
	/* 如果为空，则设置为NULL以节省空间 */
	if (bms_is_empty(joinrel->direct_lateral_relids))
		joinrel->direct_lateral_relids = NULL;

	/*
	 * 为新 joinrel 构建 restrict 和 join 条件列表。（调用者可能需要 restrictlist，
	 * 但函数自身也需要它用于 set_joinrel_size_estimates。）
	 */
	restrictlist = build_joinrel_restrictlist(root, joinrel,
			  outer_rel, inner_rel);  /* 构建限制条件列表 */
	if (restrictlist_ptr)
		*restrictlist_ptr = restrictlist;  /* 如果调用者需要，返回限制条件列表 */
	build_joinrel_joinlist(joinrel, outer_rel, inner_rel);  /* 构建连接条件列表 */

	/*
	 * 此处也应检查 joinrel 是否有待处理的 EquivalenceClass 连接
	 * 等价类连接是通过等价关系隐式存在的连接条件
	 */
	joinrel->has_eclass_joins = has_relevant_eclass_joinclause(root, joinrel);

	/* 存储分区信息，处理表分区相关的优化 */
	build_joinrel_partition_info(joinrel, outer_rel, inner_rel, restrictlist,
			 sjinfo->jointype);

	/*
	 * 设置 joinrel 的大小估算（行数、页数等）
	 * 这是优化器选择最佳执行计划的重要依据
	 */
	set_joinrel_size_estimates(root, joinrel, outer_rel, inner_rel,
			 sjinfo, restrictlist);

	/*
	 * 如果该 joinrel 可以在并行 worker 中扫描，则设置 consider_parallel 标志
	 * 条件：
	 * 1. 内、外关系都允许并行
	 * 2. 限制条件可以并行安全执行
	 * 3. 目标列表表达式可以并行安全执行
	 *
	 * 注意：如果该关系中有超过两个表，它们可能以任意方式分布在 inner_rel 和 outer_rel 中
	 * 但无论如何构建到该 joinrel，决策应一致
	 */
	if (inner_rel->consider_parallel && outer_rel->consider_parallel &&
		is_parallel_safe(root, (Node *) restrictlist) &&
		is_parallel_safe(root, (Node *) joinrel->reltarget->exprs))
		joinrel->consider_parallel = true;

	/* 将 joinrel 添加到 PlannerInfo，使其可被查询优化器的其他部分访问 */
	add_join_rel(root, joinrel);

	/*
	 * 如果启用动态规划连接搜索，则将新 joinrel 添加到相应的子列表
	 * 这是连接顺序优化的关键部分
	 * 注意：你可能认为成员数量应该相等，但某些 level 1 的关系可能已经是 joinrel，
	 * 所以只能断言 <=
	 */
	if (root->join_rel_level)
	{
		Assert(root->join_cur_level > 0);
		Assert(root->join_cur_level <= bms_num_members(joinrel->relids));
		root->join_rel_level[root->join_cur_level] =
			lappend(root->join_rel_level[root->join_cur_level], joinrel);
	}

	/* 返回构建好的连接关系 */
	return joinrel;
}


/*
 * build_child_join_rel
 *	  Builds RelOptInfo representing join between given two child relations.
 *
 * 'outer_rel' and 'inner_rel' are the RelOptInfos of child relations being
 *		joined
 * 'parent_joinrel' is the RelOptInfo representing the join between parent
 *		relations. Some of the members of new RelOptInfo are produced by
 *		translating corresponding members of this RelOptInfo
 * 'sjinfo': child-join context info
 * 'restrictlist': list of RestrictInfo nodes that apply to this particular
 *		pair of joinable relations
 * 'jointype' is the join type (inner, left, full, etc)
 */
RelOptInfo *
build_child_join_rel(PlannerInfo *root, RelOptInfo *outer_rel,
					 RelOptInfo *inner_rel, RelOptInfo *parent_joinrel,
					 List *restrictlist, SpecialJoinInfo *sjinfo,
					 JoinType jointype)
{
	RelOptInfo *joinrel = makeNode(RelOptInfo);
	AppendRelInfo **appinfos;
	int			nappinfos;

	/* Only joins between "other" relations land here. */
	Assert(IS_OTHER_REL(outer_rel) && IS_OTHER_REL(inner_rel));

	/* The parent joinrel should have consider_partitionwise_join set. */
	Assert(parent_joinrel->consider_partitionwise_join);

	joinrel->reloptkind = RELOPT_OTHER_JOINREL;
	joinrel->relids = bms_union(outer_rel->relids, inner_rel->relids);
	joinrel->rows = 0;
	/* cheap startup cost is interesting iff not all tuples to be retrieved */
	joinrel->consider_startup = (root->tuple_fraction > 0);
	joinrel->consider_param_startup = false;
	joinrel->consider_parallel = false;
	joinrel->reltarget = create_empty_pathtarget();
	joinrel->pathlist = NIL;
	joinrel->ppilist = NIL;
	joinrel->partial_pathlist = NIL;
	joinrel->cheapest_startup_path = NULL;
	joinrel->cheapest_total_path = NULL;
	joinrel->cheapest_unique_path = NULL;
	joinrel->cheapest_parameterized_paths = NIL;
	joinrel->direct_lateral_relids = NULL;
	joinrel->lateral_relids = NULL;
	joinrel->relid = 0;			/* indicates not a baserel */
	joinrel->rtekind = RTE_JOIN;
	joinrel->min_attr = 0;
	joinrel->max_attr = 0;
	joinrel->attr_needed = NULL;
	joinrel->attr_widths = NULL;
	joinrel->lateral_vars = NIL;
	joinrel->lateral_referencers = NULL;
	joinrel->indexlist = NIL;
	joinrel->pages = 0;
	joinrel->tuples = 0;
	joinrel->allvisfrac = 0;
	joinrel->subroot = NULL;
	joinrel->subplan_params = NIL;
	joinrel->serverid = InvalidOid;
	joinrel->userid = InvalidOid;
	joinrel->useridiscurrent = false;
	joinrel->fdwroutine = NULL;
	joinrel->fdw_private = NULL;
	joinrel->baserestrictinfo = NIL;
	joinrel->baserestrictcost.startup = 0;
	joinrel->baserestrictcost.per_tuple = 0;
	joinrel->joininfo = NIL;
	joinrel->has_eclass_joins = false;
	joinrel->consider_partitionwise_join = false;	/* might get changed later */
	joinrel->top_parent_relids = NULL;
	joinrel->part_scheme = NULL;
	joinrel->nparts = 0;
	joinrel->boundinfo = NULL;
	joinrel->partition_qual = NIL;
	joinrel->part_rels = NULL;
	joinrel->partexprs = NULL;
	joinrel->nullable_partexprs = NULL;
	joinrel->partitioned_child_rels = NIL;

	joinrel->top_parent_relids = bms_union(outer_rel->top_parent_relids,
										   inner_rel->top_parent_relids);

	/* Compute information relevant to foreign relations. */
	set_foreign_rel_properties(joinrel, outer_rel, inner_rel);

	/* Compute information needed for mapping Vars to the child rel */
	appinfos = find_appinfos_by_relids(root, joinrel->relids, &nappinfos);

	/* Set up reltarget struct */
	build_child_join_reltarget(root, parent_joinrel, joinrel,
							   nappinfos, appinfos);

	/* Construct joininfo list. */
	joinrel->joininfo = (List *) adjust_appendrel_attrs(root,
														(Node *) parent_joinrel->joininfo,
														nappinfos,
														appinfos);

	/*
	 * Lateral relids referred in child join will be same as that referred in
	 * the parent relation. Throw any partial result computed while building
	 * the targetlist.
	 */
	bms_free(joinrel->direct_lateral_relids);
	bms_free(joinrel->lateral_relids);
	joinrel->direct_lateral_relids = (Relids) bms_copy(parent_joinrel->direct_lateral_relids);
	joinrel->lateral_relids = (Relids) bms_copy(parent_joinrel->lateral_relids);

	/*
	 * If the parent joinrel has pending equivalence classes, so does the
	 * child.
	 */
	joinrel->has_eclass_joins = parent_joinrel->has_eclass_joins;

	/* Is the join between partitions itself partitioned? */
	build_joinrel_partition_info(joinrel, outer_rel, inner_rel, restrictlist,
								 jointype);

	/* Child joinrel is parallel safe if parent is parallel safe. */
	joinrel->consider_parallel = parent_joinrel->consider_parallel;

	/* Set estimates of the child-joinrel's size. */
	set_joinrel_size_estimates(root, joinrel, outer_rel, inner_rel,
							   sjinfo, restrictlist);

	/* We build the join only once. */
	Assert(!find_join_rel(root, joinrel->relids));

	/* Add the relation to the PlannerInfo. */
	add_join_rel(root, joinrel);

	/*
	 * We might need EquivalenceClass members corresponding to the child join,
	 * so that we can represent sort pathkeys for it.  As with children of
	 * baserels, we shouldn't need this unless there are relevant eclass joins
	 * (implying that a merge join might be possible) or pathkeys to sort by.
	 */
	if (joinrel->has_eclass_joins || has_useful_pathkeys(root, parent_joinrel))
		add_child_join_rel_equivalences(root,
										nappinfos, appinfos,
										parent_joinrel, joinrel);

	pfree(appinfos);

	return joinrel;
}

/*
 * min_join_parameterization
 *
 * Determine the minimum possible parameterization of a joinrel, that is, the
 * set of other rels it contains LATERAL references to.  We save this value in
 * the join's RelOptInfo.  This function is split out of build_join_rel()
 * because join_is_legal() needs the value to check a prospective join.
 */
Relids
min_join_parameterization(PlannerInfo *root,
						  Relids joinrelids,
						  RelOptInfo *outer_rel,
						  RelOptInfo *inner_rel)
{
	Relids		result;

	/*
	 * Basically we just need the union of the inputs' lateral_relids, less
	 * whatever is already in the join.
	 *
	 * It's not immediately obvious that this is a valid way to compute the
	 * result, because it might seem that we're ignoring possible lateral refs
	 * of PlaceHolderVars that are due to be computed at the join but not in
	 * either input.  However, because create_lateral_join_info() already
	 * charged all such PHV refs to each member baserel of the join, they'll
	 * be accounted for already in the inputs' lateral_relids.  Likewise, we
	 * do not need to worry about doing transitive closure here, because that
	 * was already accounted for in the original baserel lateral_relids.
	 */
	result = bms_union(outer_rel->lateral_relids, inner_rel->lateral_relids);
	result = bms_del_members(result, joinrelids);

	/* Maintain invariant that result is exactly NULL if empty */
	if (bms_is_empty(result))
		result = NULL;

	return result;
}

/*
 * build_joinrel_tlist
 *	  Builds a join relation's target list from an input relation.
 *	  (This is invoked twice to handle the two input relations.)
 *
 * The join's targetlist includes all Vars of its member relations that
 * will still be needed above the join.  This subroutine adds all such
 * Vars from the specified input rel's tlist to the join rel's tlist.
 *
 * We also compute the expected width of the join's output, making use
 * of data that was cached at the baserel level by set_rel_width().
 */
static void
build_joinrel_tlist(PlannerInfo *root, RelOptInfo *joinrel,
					RelOptInfo *input_rel)
{
	Relids		relids = joinrel->relids;
	ListCell   *vars;

	foreach(vars, input_rel->reltarget->exprs)
	{
		Var		   *var = (Var *) lfirst(vars);
		RelOptInfo *baserel;
		int			ndx;

		/*
		 * Ignore PlaceHolderVars in the input tlists; we'll make our own
		 * decisions about whether to copy them.
		 */
		if (IsA(var, PlaceHolderVar))
			continue;

		/*
		 * Otherwise, anything in a baserel or joinrel targetlist ought to be
		 * a Var.  (More general cases can only appear in appendrel child
		 * rels, which will never be seen here.)
		 */
		if (!IsA(var, Var))
			elog(ERROR, "unexpected node type in rel targetlist: %d",
				 (int) nodeTag(var));

		/* Get the Var's original base rel */
		baserel = find_base_rel(root, var->varno);

		/* Is it still needed above this joinrel? */
		ndx = var->varattno - baserel->min_attr;
		if (bms_nonempty_difference(baserel->attr_needed[ndx], relids))
		{
			/* Yup, add it to the output */
			joinrel->reltarget->exprs = lappend(joinrel->reltarget->exprs, var);
			/* Vars have cost zero, so no need to adjust reltarget->cost */
			joinrel->reltarget->width += baserel->attr_widths[ndx];
		}
	}
}

/*
 * build_joinrel_restrictlist
 * build_joinrel_joinlist
 *	  这些例程根据被连接关系的 joininfo 列表，为连接关系构建限制和连接子句列表。
 *
 *	  这两个例程是分开的，因为限制列表必须针对每一对输入子关系重新构建，
 *	  而连接列表只需为每个连接关系 RelOptInfo 计算一次。
 *	  连接列表完全由组成 joinrel 的关系集合决定，因此无论选择哪一对候选子关系，
 *	  都应该得到相同的结果（顺序可能不同）。但限制列表取决于哪些子关系被考虑，
 *	  因为它包含未在子关系中处理的部分。
 *
 *	  如果输入关系的连接子句引用了在 joinrel 中尚未出现的基表，
 *	  那么它仍然是 joinrel 的连接子句；我们将其放入 joinrel 的 joininfo 列表。
 *	  否则，该子句现在成为连接关系的限制子句，我们将其返回给 build_joinrel_restrictlist() 的调用者，
 *	  以存储在由这对子关系组成的连接路径中。（它不需要在连接树的更高层继续考虑。）
 *
 *	  在许多情况下，我们会在两个输入关系的 joinlists 中发现相同的 RestrictInfo，
 *	  因此要注意去除重复项。指针相等性应足以判断重复，因为所有不同的 joinlist 条目最终都引用了
 *	  distribute_restrictinfo_to_rels() 推送进去的 RestrictInfo。
 *
 * 'joinrel' 是连接关系节点
 * 'outer_rel' 和 'inner_rel' 是可以连接形成 joinrel 的一对关系。
 *
 * build_joinrel_restrictlist() 返回相关的 restrictinfo 列表，
 * 而 build_joinrel_joinlist() 将结果存储在 joinrel 的 joininfo 列表中。
 * 每个子句必须被其中一个接受！
 *
 * 注意：以前我们会对每个输入 RestrictInfo 进行深度复制以传递到连接关系。
 * 我认为现在不再需要这样做，因为 RestrictInfo 节点不再依赖于上下文。
 * 现在只需将原始节点包含在为连接关系构建的列表中即可。
 */
static List *
build_joinrel_restrictlist(PlannerInfo *root,
						   RelOptInfo *joinrel,
						   RelOptInfo *outer_rel,
						   RelOptInfo *inner_rel)
{
	List	   *result;

	/*
	 * 收集所有在语法上属于该层级的子句，并去除重复项（很重要，因为我们会在两个输入关系中看到许多相同的子句）。
	 */
	result = subbuild_joinrel_restrictlist(joinrel, outer_rel->joininfo, NIL);
	result = subbuild_joinrel_restrictlist(joinrel, inner_rel->joininfo, result);

	/*
	 * 添加由等价类（EquivalenceClasses）推导出的子句。这些子句不会与 joininfo 列表中的子句重复，因此无需检查。
	 */
	result = list_concat(result,
						 generate_join_implied_equalities(root,
														  joinrel->relids,
														  outer_rel->relids,
														  inner_rel));

	return result;
}

/*
 * build_joinrel_joinlist
 *	  为连接关系构建 joininfo 列表。
 *
 *	  收集所有在语法上属于该层级的连接子句，并去除重复项（很重要，因为我们会在两个输入关系中看到许多相同的子句）。
 *	  只保留那些在当前 joinrel 层级仍然是连接子句的 RestrictInfo。
 */
static void
build_joinrel_joinlist(RelOptInfo *joinrel,
					   RelOptInfo *outer_rel,
					   RelOptInfo *inner_rel)
{
	List	   *result;

	/*
	 * 收集所有在语法上属于该层级的连接子句，并去除重复项（很重要，因为我们会在两个输入关系中看到许多相同的子句）。
	 */
	result = subbuild_joinrel_joinlist(joinrel, outer_rel->joininfo, NIL);
	result = subbuild_joinrel_joinlist(joinrel, inner_rel->joininfo, result);

	/* 将结果存储到 joinrel 的 joininfo 列表中 */
	joinrel->joininfo = result;
}

/*
 * subbuild_joinrel_restrictlist - 构建连接关系的限制条件列表的辅助函数
 *
 * 该函数用于从连接信息列表中筛选出适用于指定连接关系的限制条件。
 *
 * 参数说明:
 * - joinrel: 目标连接关系的优化信息结构
 * - joininfo_list: 连接信息列表，包含可能适用于此连接关系的限制条件
 * - new_restrictlist: 已有的限制条件列表，新筛选出的条件将添加到此列表中
 *
 * 返回值:
 * - List*: 更新后的限制条件列表，包含所有适用于该连接关系的限制条件
 *
 * 工作原理:
 * 遍历joininfo_list中的每个限制条件，检查其所需的relids是否是当前joinrel的relids的子集。
 * 如果是，则说明该限制条件完全适用于当前连接关系，可作为其限制条件；
 * 否则，该条件仍是一个连接条件，不适用于当前层级，被忽略。
 */
static List *
subbuild_joinrel_restrictlist(RelOptInfo *joinrel,
							  List *joininfo_list,
							  List *new_restrictlist)
{
	ListCell   *l;  /* 列表遍历指针 */

	/* 遍历连接信息列表中的每个元素 */
	foreach(l, joininfo_list)
	{
		/* 获取当前的限制信息 */
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		/* 检查该限制条件所需的relids是否是当前连接关系relids的子集 */
		if (bms_is_subset(rinfo->required_relids, joinrel->relids))
		{
			/*
			 * 该条件成为连接关系的限制条件，因为它不引用外部关系。
			 * 将其添加到列表中，注意消除重复项。
			 * (由于不同连接列表中的RestrictInfo节点是多重链接而不是复制的，
			 *  指针相等性应该是 sufficient 测试。)
			 */
			new_restrictlist = list_append_unique_ptr(new_restrictlist, rinfo);
		}
		else
		{
			/*
			 * 该条件在此层级仍然是一个连接条件，因此在此例程中忽略它。
			 */
		}
	}

	/* 返回更新后的限制条件列表 */
	return new_restrictlist;
}


static List *
subbuild_joinrel_joinlist(RelOptInfo *joinrel,
						  List *joininfo_list,
						  List *new_joininfo)
{
	ListCell   *l;

	/* Expected to be called only for join between parent relations. */
	Assert(joinrel->reloptkind == RELOPT_JOINREL);

	foreach(l, joininfo_list)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		if (bms_is_subset(rinfo->required_relids, joinrel->relids))
		{
			/*
			 * This clause becomes a restriction clause for the joinrel, since
			 * it refers to no outside rels.  So we can ignore it in this
			 * routine.
			 */
		}
		else
		{
			/*
			 * This clause is still a join clause at this level, so add it to
			 * the new joininfo list, being careful to eliminate duplicates.
			 * (Since RestrictInfo nodes in different joinlists will have been
			 * multiply-linked rather than copied, pointer equality should be
			 * a sufficient test.)
			 */
			new_joininfo = list_append_unique_ptr(new_joininfo, rinfo);
		}
	}

	return new_joininfo;
}


/*
 * fetch_upper_rel
 *		构建一个描述扫描/连接之后的查询处理的 RelOptInfo，
 *		如果已经构建则直接返回已有的 RelOptInfo。
 *
 * 一个“upper”关系由 UpperRelationKind 和 Relids 集合标识。
 * Relids 集合的含义在此不做规定，并且对于不同的关系类型可能会有不同的解释。
 *
 * upper-level RelOptInfo 的大多数字段不会被使用，也不会在此设置
 * （不过 makeNode 会确保它们被初始化为零）。我们基本只关心 add_path() 和 set_cheapest() 相关的字段。
 */
RelOptInfo *
fetch_upper_rel(PlannerInfo *root, UpperRelationKind kind, Relids relids)
{
	RelOptInfo *upperrel;
	ListCell   *lc;

	/*
	 * 当前，我们的索引数据结构只是每种关系类型一个 List。
	 * 如果某种类型的数量太多导致效率低，可以优化这里。
	 * 除本函数外，其他代码不应假定如何查找某个 upperrel。
	 */

	/* 如果已经为该查询构建了 upperrel，则直接返回 */
	foreach(lc, root->upper_rels[kind])
	{
		upperrel = (RelOptInfo *) lfirst(lc);

		if (bms_equal(upperrel->relids, relids))
			return upperrel;
	}

	/* 否则新建一个 upperrel */
	upperrel = makeNode(RelOptInfo);
	upperrel->reloptkind = RELOPT_UPPER_REL;
	upperrel->relids = bms_copy(relids);

	/* 仅当不是检索所有元组时，才关注低启动成本 */
	upperrel->consider_startup = (root->tuple_fraction > 0);
	upperrel->consider_param_startup = false;
	upperrel->consider_parallel = false;	/* 可能稍后更改 */
	upperrel->reltarget = create_empty_pathtarget();
	upperrel->pathlist = NIL;
	upperrel->cheapest_startup_path = NULL;
	upperrel->cheapest_total_path = NULL;
	upperrel->cheapest_unique_path = NULL;
	upperrel->cheapest_parameterized_paths = NIL;

	/* 将新建的 upperrel 添加到对应类型的列表中 */
	root->upper_rels[kind] = lappend(root->upper_rels[kind], upperrel);

	return upperrel;
}


/*
 * find_childrel_parents
 *      计算appendrel子关系的父关系ID集合。
 *
 * 由于appendrel可以嵌套，一个子关系可能有多个级别的appendrel祖先。
 * 此函数计算所有父关系ID的Relids集合。
 */
Relids
find_childrel_parents(PlannerInfo *root, /* 查询规划器信息 */
                     RelOptInfo *rel)   /* 子关系节点 */
{
    Relids      result = NULL;  /* 存储父关系ID集合的结果 */

    /* 断言：确保传入的关系是其他成员关系类型(RELOPT_OTHER_MEMBER_REL) */
    Assert(rel->reloptkind == RELOPT_OTHER_MEMBER_REL);
    /* 断言：确保关系ID有效且在simple_rel_array数组范围内 */
    Assert(rel->relid > 0 && rel->relid < root->simple_rel_array_size);

    /* 循环向上遍历父关系链 */
    do
    {
        /* 获取当前关系的appendrel信息 */
        AppendRelInfo *appinfo = root->append_rel_array[rel->relid];
        /* 获取父关系的ID */
        Index       prelid = appinfo->parent_relid;

        /* 将父关系ID添加到结果集合中 */
        result = bms_add_member(result, prelid);

        /* 向上遍历到父关系，如果父关系也是子关系则继续循环 */
        rel = find_base_rel(root, prelid);
    } while (rel->reloptkind == RELOPT_OTHER_MEMBER_REL);

    /* 断言：遍历结束时应该到达基础关系类型(RELOPT_BASEREL) */
    Assert(rel->reloptkind == RELOPT_BASEREL);

    /* 返回收集到的所有父关系ID集合 */
    return result;
}


/*
 * get_baserel_parampathinfo
 *    获取基本关系参数化路径的ParamPathInfo信息，如果不存在则构造一个新的
 *
 * 函数功能：
 *    该函数是PostgreSQL查询优化器中处理参数化路径的核心组件，它集中管理参数化路径的
 *    行数估计并确保相同参数化的所有路径使用一致的行数估计值。同时，它还负责确定哪些
 *    可移动的连接条件应该由参数化路径来评估。
 *
 * 参数说明：
 *    root - 规划器的全局信息结构，包含查询的所有规划信息
 *    baserel - 要获取参数化路径信息的基本关系
 *    required_outer - 参数化路径所需的外部关系ID集合
 *
 * 返回值：
 *    返回指向ParamPathInfo结构的指针，包含参数化路径所需的所有信息
 */
ParamPathInfo *
get_baserel_parampathinfo(PlannerInfo *root, RelOptInfo *baserel,
						  Relids required_outer)
{
    ParamPathInfo *ppi;       /* 参数化路径信息结构指针 */
    Relids		joinrelids;  /* 基本关系与外部关系的联合ID集合 */
    List	   *pclauses;    /* 参数化路径需要评估的条件列表 */
    List	   *eqclauses;   /* 由等价类生成的连接条件列表 */
    double		rows;        /* 参数化扫描返回的估计行数 */
    ListCell   *lc;           /* 循环列表的指针 */

    /* 断言：如果关系有LATERAL引用，那么所有路径都应该考虑这些引用 */
    Assert(bms_is_subset(baserel->lateral_relids, required_outer));

    /* 非参数化路径不需要ParamPathInfo结构 */
    if (bms_is_empty(required_outer))
        return NULL;

    /* 断言：基本关系的ID不应与外部关系的ID重叠 */
    Assert(!bms_overlap(baserel->relids, required_outer));

    /* 如果已经存在针对此参数化的PPI，直接返回现有的 */
    if ((ppi = find_param_path_info(baserel, required_outer)))
        return ppi;

    /*
     * 识别所有在给定参数化条件下可移动到该基本关系的连接条件
     * 这些条件将由参数化路径负责评估，而不是在后续的连接处理中
     */
    joinrelids = bms_union(baserel->relids, required_outer);
    pclauses = NIL;
    foreach(lc, baserel->joininfo)
    {
        RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

        /* 检查连接条件是否可以移动到当前基本关系 */
        if (join_clause_is_movable_into(rinfo,
                                        baserel->relids,
                                        joinrelids))
            pclauses = lappend(pclauses, rinfo);
    }

    /*
     * 还需要添加由等价类(EquivalenceClasses)生成的连接条件
     * 理论上这些条件应该都满足join_clause_is_movable_into，但在外部连接下，
     * 这些条件可能包含应该在连接之上评估的变量，因此仍需检查
     */
    eqclauses = generate_join_implied_equalities(root,
                                                 joinrelids,
                                                 required_outer,
                                                 baserel);
    foreach(lc, eqclauses)
    {
        RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

        /* 再次检查等价类生成的条件是否可以移动 */
        if (join_clause_is_movable_into(rinfo,
                                        baserel->relids,
                                        joinrelids))
            pclauses = lappend(pclauses, rinfo);
    }

    /* 估计参数化扫描返回的行数 */
    rows = get_parameterized_baserel_size(root, baserel, pclauses);

    /* 构建ParamPathInfo结构 */
    ppi = makeNode(ParamPathInfo);
    ppi->ppi_req_outer = required_outer; /* 所需的外部关系 */
    ppi->ppi_rows = rows;               /* 估计的行数 */
    ppi->ppi_clauses = pclauses;        /* 参数化路径负责评估的条件 */
    /* 将新创建的PPI添加到基本关系的ppilist中，以便后续重用 */
    baserel->ppilist = lappend(baserel->ppilist, ppi);

    return ppi;
}


/*
 * get_joinrel_parampathinfo
 *		Get the ParamPathInfo for a parameterized path for a join relation,
 *		constructing one if we don't have one already.
 *
 * This centralizes estimating the rowcounts for parameterized paths.
 * We need to cache those to be sure we use the same rowcount for all paths
 * of the same parameterization for a given rel.  This is also a convenient
 * place to determine which movable join clauses the parameterized path will
 * be responsible for evaluating.
 *
 * outer_path and inner_path are a pair of input paths that can be used to
 * construct the join, and restrict_clauses is the list of regular join
 * clauses (including clauses derived from EquivalenceClasses) that must be
 * applied at the join node when using these inputs.
 *
 * Unlike the situation for base rels, the set of movable join clauses to be
 * enforced at a join varies with the selected pair of input paths, so we
 * must calculate that and pass it back, even if we already have a matching
 * ParamPathInfo.  We handle this by adding any clauses moved down to this
 * join to *restrict_clauses, which is an in/out parameter.  (The addition
 * is done in such a way as to not modify the passed-in List structure.)
 *
 * Note: when considering a nestloop join, the caller must have removed from
 * restrict_clauses any movable clauses that are themselves scheduled to be
 * pushed into the right-hand path.  We do not do that here since it's
 * unnecessary for other join types.
 */
ParamPathInfo *
get_joinrel_parampathinfo(PlannerInfo *root, RelOptInfo *joinrel,
						  Path *outer_path,
						  Path *inner_path,
						  SpecialJoinInfo *sjinfo,
						  Relids required_outer,
						  List **restrict_clauses)
{
	ParamPathInfo *ppi;
	Relids		join_and_req;
	Relids		outer_and_req;
	Relids		inner_and_req;
	List	   *pclauses;
	List	   *eclauses;
	List	   *dropped_ecs;
	double		rows;
	ListCell   *lc;

	/* If rel has LATERAL refs, every path for it should account for them */
	Assert(bms_is_subset(joinrel->lateral_relids, required_outer));

	/* Unparameterized paths have no ParamPathInfo or extra join clauses */
	if (bms_is_empty(required_outer))
		return NULL;

	Assert(!bms_overlap(joinrel->relids, required_outer));

	/*
	 * Identify all joinclauses that are movable to this join rel given this
	 * parameterization.  These are the clauses that are movable into this
	 * join, but not movable into either input path.  Treat an unparameterized
	 * input path as not accepting parameterized clauses (because it won't,
	 * per the shortcut exit above), even though the joinclause movement rules
	 * might allow the same clauses to be moved into a parameterized path for
	 * that rel.
	 */
	join_and_req = bms_union(joinrel->relids, required_outer);
	if (outer_path->param_info)
		outer_and_req = bms_union(outer_path->parent->relids,
								  PATH_REQ_OUTER(outer_path));
	else
		outer_and_req = NULL;	/* outer path does not accept parameters */
	if (inner_path->param_info)
		inner_and_req = bms_union(inner_path->parent->relids,
								  PATH_REQ_OUTER(inner_path));
	else
		inner_and_req = NULL;	/* inner path does not accept parameters */

	pclauses = NIL;
	foreach(lc, joinrel->joininfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		if (join_clause_is_movable_into(rinfo,
										joinrel->relids,
										join_and_req) &&
			!join_clause_is_movable_into(rinfo,
										 outer_path->parent->relids,
										 outer_and_req) &&
			!join_clause_is_movable_into(rinfo,
										 inner_path->parent->relids,
										 inner_and_req))
			pclauses = lappend(pclauses, rinfo);
	}

	/* Consider joinclauses generated by EquivalenceClasses, too */
	eclauses = generate_join_implied_equalities(root,
												join_and_req,
												required_outer,
												joinrel);
	/* We only want ones that aren't movable to lower levels */
	dropped_ecs = NIL;
	foreach(lc, eclauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		/*
		 * In principle, join_clause_is_movable_into() should accept anything
		 * returned by generate_join_implied_equalities(); but because its
		 * analysis is only approximate, sometimes it doesn't.  So we
		 * currently cannot use this Assert; instead just assume it's okay to
		 * apply the joinclause at this level.
		 */
#ifdef NOT_USED
		Assert(join_clause_is_movable_into(rinfo,
										   joinrel->relids,
										   join_and_req));
#endif
		if (join_clause_is_movable_into(rinfo,
										outer_path->parent->relids,
										outer_and_req))
			continue;			/* drop if movable into LHS */
		if (join_clause_is_movable_into(rinfo,
										inner_path->parent->relids,
										inner_and_req))
		{
			/* drop if movable into RHS, but remember EC for use below */
			Assert(rinfo->left_ec == rinfo->right_ec);
			dropped_ecs = lappend(dropped_ecs, rinfo->left_ec);
			continue;
		}
		pclauses = lappend(pclauses, rinfo);
	}

	/*
	 * EquivalenceClasses are harder to deal with than we could wish, because
	 * of the fact that a given EC can generate different clauses depending on
	 * context.  Suppose we have an EC {X.X, Y.Y, Z.Z} where X and Y are the
	 * LHS and RHS of the current join and Z is in required_outer, and further
	 * suppose that the inner_path is parameterized by both X and Z.  The code
	 * above will have produced either Z.Z = X.X or Z.Z = Y.Y from that EC,
	 * and in the latter case will have discarded it as being movable into the
	 * RHS.  However, the EC machinery might have produced either Y.Y = X.X or
	 * Y.Y = Z.Z as the EC enforcement clause within the inner_path; it will
	 * not have produced both, and we can't readily tell from here which one
	 * it did pick.  If we add no clause to this join, we'll end up with
	 * insufficient enforcement of the EC; either Z.Z or X.X will fail to be
	 * constrained to be equal to the other members of the EC.  (When we come
	 * to join Z to this X/Y path, we will certainly drop whichever EC clause
	 * is generated at that join, so this omission won't get fixed later.)
	 *
	 * To handle this, for each EC we discarded such a clause from, try to
	 * generate a clause connecting the required_outer rels to the join's LHS
	 * ("Z.Z = X.X" in the terms of the above example).  If successful, and if
	 * the clause can't be moved to the LHS, add it to the current join's
	 * restriction clauses.  (If an EC cannot generate such a clause then it
	 * has nothing that needs to be enforced here, while if the clause can be
	 * moved into the LHS then it should have been enforced within that path.)
	 *
	 * Note that we don't need similar processing for ECs whose clause was
	 * considered to be movable into the LHS, because the LHS can't refer to
	 * the RHS so there is no comparable ambiguity about what it might
	 * actually be enforcing internally.
	 */
	if (dropped_ecs)
	{
		Relids		real_outer_and_req;

		real_outer_and_req = bms_union(outer_path->parent->relids,
									   required_outer);
		eclauses =
			generate_join_implied_equalities_for_ecs(root,
													 dropped_ecs,
													 real_outer_and_req,
													 required_outer,
													 outer_path->parent);
		foreach(lc, eclauses)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

			/* As above, can't quite assert this here */
#ifdef NOT_USED
			Assert(join_clause_is_movable_into(rinfo,
											   outer_path->parent->relids,
											   real_outer_and_req));
#endif
			if (!join_clause_is_movable_into(rinfo,
											 outer_path->parent->relids,
											 outer_and_req))
				pclauses = lappend(pclauses, rinfo);
		}
	}

	/*
	 * Now, attach the identified moved-down clauses to the caller's
	 * restrict_clauses list.  By using list_concat in this order, we leave
	 * the original list structure of restrict_clauses undamaged.
	 */
	*restrict_clauses = list_concat(pclauses, *restrict_clauses);

	/* If we already have a PPI for this parameterization, just return it */
	if ((ppi = find_param_path_info(joinrel, required_outer)))
		return ppi;

	/* Estimate the number of rows returned by the parameterized join */
	rows = get_parameterized_joinrel_size(root, joinrel,
										  outer_path,
										  inner_path,
										  sjinfo,
										  *restrict_clauses);

	/*
	 * And now we can build the ParamPathInfo.  No point in saving the
	 * input-pair-dependent clause list, though.
	 *
	 * Note: in GEQO mode, we'll be called in a temporary memory context, but
	 * the joinrel structure is there too, so no problem.
	 */
	ppi = makeNode(ParamPathInfo);
	ppi->ppi_req_outer = required_outer;
	ppi->ppi_rows = rows;
	ppi->ppi_clauses = NIL;
	joinrel->ppilist = lappend(joinrel->ppilist, ppi);

	return ppi;
}

/*
 * get_appendrel_parampathinfo
 *		Get the ParamPathInfo for a parameterized path for an append relation.
 *
 * For an append relation, the rowcount estimate will just be the sum of
 * the estimates for its children.  However, we still need a ParamPathInfo
 * to flag the fact that the path requires parameters.  So this just creates
 * a suitable struct with zero ppi_rows (and no ppi_clauses either, since
 * the Append node isn't responsible for checking quals).
 */
ParamPathInfo *
get_appendrel_parampathinfo(RelOptInfo *appendrel, Relids required_outer)
{
	ParamPathInfo *ppi;

	/* If rel has LATERAL refs, every path for it should account for them */
	Assert(bms_is_subset(appendrel->lateral_relids, required_outer));

	/* Unparameterized paths have no ParamPathInfo */
	if (bms_is_empty(required_outer))
		return NULL;

	Assert(!bms_overlap(appendrel->relids, required_outer));

	/* If we already have a PPI for this parameterization, just return it */
	if ((ppi = find_param_path_info(appendrel, required_outer)))
		return ppi;

	/* Else build the ParamPathInfo */
	ppi = makeNode(ParamPathInfo);
	ppi->ppi_req_outer = required_outer;
	ppi->ppi_rows = 0;
	ppi->ppi_clauses = NIL;
	appendrel->ppilist = lappend(appendrel->ppilist, ppi);

	return ppi;
}

/*
 * find_param_path_info
 *    在指定关系中查找与给定外部关系参数化匹配的ParamPathInfo结构
 *
 * 函数功能：
 *    该函数在关系的ppilist（参数化路径信息列表）中搜索，查找与指定required_outer参数化
 *    完全匹配的ParamPathInfo结构。如果找到匹配项则返回该结构指针，否则返回NULL。
 *    这是PostgreSQL查询优化器中参数化路径缓存机制的关键组件。
 *
 * 参数说明：
 *    rel - 要查找参数化路径信息的关系优化结构
 *    required_outer - 要匹配的外部关系ID集合，表示参数化所需的外部关系
 *
 * 返回值：
 *    成功：返回匹配的ParamPathInfo结构指针
 *    失败：返回NULL，表示该关系中不存在与给定参数化匹配的路径信息
 */
ParamPathInfo *
find_param_path_info(RelOptInfo *rel, Relids required_outer)
{
    ListCell   *lc;  /* 用于遍历ppilist的循环指针 */

    /* 遍历关系的参数化路径信息列表 */
    foreach(lc, rel->ppilist)
    {
        /* 获取当前的参数化路径信息 */
        ParamPathInfo *ppi = (ParamPathInfo *) lfirst(lc);

        /* 检查当前PPI的外部关系需求是否与请求的完全匹配 */
        /* bms_equal用于比较两个位图集是否完全相同 */
        if (bms_equal(ppi->ppi_req_outer, required_outer))
            return ppi;  /* 找到匹配项，立即返回 */
    }

    /* 遍历结束仍未找到匹配的PPI，返回NULL */
    return NULL;
}


/*
 * build_joinrel_partition_info
 *		If the two relations have same partitioning scheme, their join may be
 *		partitioned and will follow the same partitioning scheme as the joining
 *		relations. Set the partition scheme and partition key expressions in
 *		the join relation.
 */
static void
build_joinrel_partition_info(RelOptInfo *joinrel, RelOptInfo *outer_rel,
							 RelOptInfo *inner_rel, List *restrictlist,
							 JoinType jointype)
{
	int			partnatts;
	int			cnt;
	PartitionScheme part_scheme;

	/* Nothing to do if partitionwise join technique is disabled. */
	if (!enable_partitionwise_join)
	{
		Assert(!IS_PARTITIONED_REL(joinrel));
		return;
	}

	/*
	 * We can only consider this join as an input to further partitionwise
	 * joins if (a) the input relations are partitioned and have
	 * consider_partitionwise_join=true, (b) the partition schemes match, and
	 * (c) we can identify an equi-join between the partition keys.  Note that
	 * if it were possible for have_partkey_equi_join to return different
	 * answers for the same joinrel depending on which join ordering we try
	 * first, this logic would break.  That shouldn't happen, though, because
	 * of the way the query planner deduces implied equalities and reorders
	 * the joins.  Please see optimizer/README for details.
	 */
	if (!IS_PARTITIONED_REL(outer_rel) || !IS_PARTITIONED_REL(inner_rel) ||
		!outer_rel->consider_partitionwise_join ||
		!inner_rel->consider_partitionwise_join ||
		outer_rel->part_scheme != inner_rel->part_scheme ||
		!have_partkey_equi_join(joinrel, outer_rel, inner_rel,
								jointype, restrictlist))
	{
		Assert(!IS_PARTITIONED_REL(joinrel));
		return;
	}

	part_scheme = outer_rel->part_scheme;

	Assert(REL_HAS_ALL_PART_PROPS(outer_rel) &&
		   REL_HAS_ALL_PART_PROPS(inner_rel));

	/*
	 * For now, our partition matching algorithm can match partitions only
	 * when the partition bounds of the joining relations are exactly same.
	 * So, bail out otherwise.
	 */
	if (outer_rel->nparts != inner_rel->nparts ||
		!partition_bounds_equal(part_scheme->partnatts,
								part_scheme->parttyplen,
								part_scheme->parttypbyval,
								outer_rel->boundinfo, inner_rel->boundinfo))
	{
		Assert(!IS_PARTITIONED_REL(joinrel));
		return;
	}

	/*
	 * This function will be called only once for each joinrel, hence it
	 * should not have partition scheme, partition bounds, partition key
	 * expressions and array for storing child relations set.
	 */
	Assert(!joinrel->part_scheme && !joinrel->partexprs &&
		   !joinrel->nullable_partexprs && !joinrel->part_rels &&
		   !joinrel->boundinfo);

	/*
	 * Join relation is partitioned using the same partitioning scheme as the
	 * joining relations and has same bounds.
	 */
	joinrel->part_scheme = part_scheme;
	joinrel->boundinfo = outer_rel->boundinfo;
	partnatts = joinrel->part_scheme->partnatts;
	joinrel->partexprs = (List **) palloc0(sizeof(List *) * partnatts);
	joinrel->nullable_partexprs =
		(List **) palloc0(sizeof(List *) * partnatts);
	joinrel->nparts = outer_rel->nparts;
	joinrel->part_rels =
		(RelOptInfo **) palloc0(sizeof(RelOptInfo *) * joinrel->nparts);

	/*
	 * Set the consider_partitionwise_join flag.
	 */
	Assert(outer_rel->consider_partitionwise_join);
	Assert(inner_rel->consider_partitionwise_join);
	joinrel->consider_partitionwise_join = true;

	/*
	 * Construct partition keys for the join.
	 *
	 * An INNER join between two partitioned relations can be regarded as
	 * partitioned by either key expression.  For example, A INNER JOIN B ON
	 * A.a = B.b can be regarded as partitioned on A.a or on B.b; they are
	 * equivalent.
	 *
	 * For a SEMI or ANTI join, the result can only be regarded as being
	 * partitioned in the same manner as the outer side, since the inner
	 * columns are not retained.
	 *
	 * An OUTER join like (A LEFT JOIN B ON A.a = B.b) may produce rows with
	 * B.b NULL. These rows may not fit the partitioning conditions imposed on
	 * B.b. Hence, strictly speaking, the join is not partitioned by B.b and
	 * thus partition keys of an OUTER join should include partition key
	 * expressions from the OUTER side only.  However, because all
	 * commonly-used comparison operators are strict, the presence of nulls on
	 * the outer side doesn't cause any problem; they can't match anything at
	 * future join levels anyway.  Therefore, we track two sets of
	 * expressions: those that authentically partition the relation
	 * (partexprs) and those that partition the relation with the exception
	 * that extra nulls may be present (nullable_partexprs).  When the
	 * comparison operator is strict, the latter is just as good as the
	 * former.
	 */
	for (cnt = 0; cnt < partnatts; cnt++)
	{
		List	   *outer_expr;
		List	   *outer_null_expr;
		List	   *inner_expr;
		List	   *inner_null_expr;
		List	   *partexpr = NIL;
		List	   *nullable_partexpr = NIL;

		outer_expr = list_copy(outer_rel->partexprs[cnt]);
		outer_null_expr = list_copy(outer_rel->nullable_partexprs[cnt]);
		inner_expr = list_copy(inner_rel->partexprs[cnt]);
		inner_null_expr = list_copy(inner_rel->nullable_partexprs[cnt]);

		switch (jointype)
		{
			case JOIN_INNER:
				partexpr = list_concat(outer_expr, inner_expr);
				nullable_partexpr = list_concat(outer_null_expr,
												inner_null_expr);
				break;

			case JOIN_SEMI:
			case JOIN_ANTI:
				partexpr = outer_expr;
				nullable_partexpr = outer_null_expr;
				break;

			case JOIN_LEFT:
				partexpr = outer_expr;
				nullable_partexpr = list_concat(inner_expr,
												outer_null_expr);
				nullable_partexpr = list_concat(nullable_partexpr,
												inner_null_expr);
				break;

			case JOIN_FULL:
				nullable_partexpr = list_concat(outer_expr,
												inner_expr);
				nullable_partexpr = list_concat(nullable_partexpr,
												outer_null_expr);
				nullable_partexpr = list_concat(nullable_partexpr,
												inner_null_expr);
				break;

			default:
				elog(ERROR, "unrecognized join type: %d", (int) jointype);

		}

		joinrel->partexprs[cnt] = partexpr;
		joinrel->nullable_partexprs[cnt] = nullable_partexpr;
	}
}

/*
 * build_child_join_reltarget
 *	  Set up a child-join relation's reltarget from a parent-join relation.
 */
static void
build_child_join_reltarget(PlannerInfo *root,
						   RelOptInfo *parentrel,
						   RelOptInfo *childrel,
						   int nappinfos,
						   AppendRelInfo **appinfos)
{
	/* Build the targetlist */
	childrel->reltarget->exprs = (List *)
		adjust_appendrel_attrs(root,
							   (Node *) parentrel->reltarget->exprs,
							   nappinfos, appinfos);

	/* Set the cost and width fields */
	childrel->reltarget->cost.startup = parentrel->reltarget->cost.startup;
	childrel->reltarget->cost.per_tuple = parentrel->reltarget->cost.per_tuple;
	childrel->reltarget->width = parentrel->reltarget->width;
}
