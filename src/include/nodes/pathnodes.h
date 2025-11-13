/*-------------------------------------------------------------------------
 *
 * pathnodes.h
 *	  Definitions for planner's internal data structures, especially Paths.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/pathnodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PATHNODES_H
#define PATHNODES_H

#include "access/sdir.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "nodes/params.h"
#include "nodes/parsenodes.h"
#include "storage/block.h"


/*
 * Relids
 *		关系标识符集合（rangetable 索引的集合，使用 Bitmapset 表示）。
 */
typedef Bitmapset *Relids;

/*
 * When looking for a "cheapest path", this enum specifies whether we want
 * cheapest startup cost or cheapest total cost.
 */
typedef enum CostSelector
{
	STARTUP_COST, TOTAL_COST
} CostSelector;

/*
 * The cost estimate produced by cost_qual_eval() includes both a one-time
 * (startup) cost, and a per-tuple cost.
 */
typedef struct QualCost
{
	Cost		startup;		/* one-time cost */
	Cost		per_tuple;		/* per-evaluation cost */
} QualCost;

/*
 * Costing aggregate function execution requires these statistics about
 * the aggregates to be executed by a given Agg node.  Note that the costs
 * include the execution costs of the aggregates' argument expressions as
 * well as the aggregate functions themselves.  Also, the fields must be
 * defined so that initializing the struct to zeroes with memset is correct.
 */
typedef struct AggClauseCosts
{
	int			numAggs;		/* total number of aggregate functions */
	int			numOrderedAggs; /* number w/ DISTINCT/ORDER BY/WITHIN GROUP */
	bool		hasNonPartial;	/* does any agg not support partial mode? */
	bool		hasNonSerial;	/* is any partial agg non-serializable? */
	QualCost	transCost;		/* total per-input-row execution costs */
	QualCost	finalCost;		/* total per-aggregated-row costs */
	Size		transitionSpace;	/* space for pass-by-ref transition data */
} AggClauseCosts;

/*
 * This enum identifies the different types of "upper" (post-scan/join)
 * relations that we might deal with during planning.
 */
typedef enum UpperRelationKind
{
	UPPERREL_SETOP,				/* result of UNION/INTERSECT/EXCEPT, if any */
	UPPERREL_PARTIAL_GROUP_AGG, /* result of partial grouping/aggregation, if
								 * any */
	UPPERREL_GROUP_AGG,			/* result of grouping/aggregation, if any */
	UPPERREL_WINDOW,			/* result of window functions, if any */
	UPPERREL_DISTINCT,			/* result of "SELECT DISTINCT", if any */
	UPPERREL_ORDERED,			/* result of ORDER BY, if any */
	UPPERREL_FINAL				/* result of any remaining top-level actions */
	/* NB: UPPERREL_FINAL must be last enum entry; it's used to size arrays */
} UpperRelationKind;

/*
 * This enum identifies which type of relation is being planned through the
 * inheritance planner.  INHKIND_NONE indicates the inheritance planner
 * was not used.
 */
typedef enum InheritanceKind
{
	INHKIND_NONE,
	INHKIND_INHERITED,
	INHKIND_PARTITIONED
} InheritanceKind;

/*----------
 * PlannerGlobal
 *		Global information for planning/optimization
 *
 * PlannerGlobal holds state for an entire planner invocation; this state
 * is shared across all levels of sub-Queries that exist in the command being
 * planned.
 *----------
 */
typedef struct PlannerGlobal
{
	NodeTag		type;

	ParamListInfo boundParams;	/* Param values provided to planner() */

	List	   *subplans;		/* Plans for SubPlan nodes */

	List	   *subroots;		/* PlannerInfos for SubPlan nodes */

	Bitmapset  *rewindPlanIDs;	/* indices of subplans that require REWIND */

	List	   *finalrtable;	/* "flat" rangetable for executor */

	List	   *finalrowmarks;	/* "flat" list of PlanRowMarks */

	List	   *resultRelations;	/* "flat" list of integer RT indexes */

	List	   *rootResultRelations;	/* "flat" list of integer RT indexes */

	List	   *relationOids;	/* OIDs of relations the plan depends on */

	List	   *invalItems;		/* other dependencies, as PlanInvalItems */

	List	   *paramExecTypes; /* type OIDs for PARAM_EXEC Params */

	Index		lastPHId;		/* highest PlaceHolderVar ID assigned */

	Index		lastRowMarkId;	/* highest PlanRowMark ID assigned */

	int			lastPlanNodeId; /* highest plan node ID assigned */

	bool		transientPlan;	/* redo plan when TransactionXmin changes? */

	bool		dependsOnRole;	/* is plan specific to current role? */

	bool		parallelModeOK; /* parallel mode potentially OK? */

	bool		parallelModeNeeded; /* parallel mode actually required? */

	char		maxParallelHazard;	/* worst PROPARALLEL hazard level */

	PartitionDirectory partition_directory; /* partition descriptors */
} PlannerGlobal;

/* macro for fetching the Plan associated with a SubPlan node */
#define planner_subplan_get_plan(root, subplan) \
	((Plan *) list_nth((root)->glob->subplans, (subplan)->plan_id - 1))


/*----------
 * PlannerInfo
 *		用于每个查询的规划/优化信息
 *
 * 在所有 planner 例程中该结构通常被称为 "root"。它保存了规划器的所有工作状态的链接，
 * 以及原始的 Query。注意，目前 planner 会大量修改传入的 Query 数据结构；将来可能会停止这样做。
 *
 * 出于 optimizer/optimizer.h 中解释的原因，我们在该头文件或此处（哪个先被包含）定义 typedef。
 *----------
 */
#ifndef HAVE_PLANNERINFO_TYPEDEF
typedef struct PlannerInfo PlannerInfo;
#define HAVE_PLANNERINFO_TYPEDEF 1
#endif

struct PlannerInfo
{
	NodeTag		type;

	Query	   *parse;			/* 正在规划的 Query */

	PlannerGlobal *glob;		/* 当前 planner 运行的全局信息 */

	Index		query_level;	/* 查询层次，1标识最高层 */

	PlannerInfo* parent_root;	/* 如为子计划，则这里存储父计划器指针，NULL标识最高层 */

	/*
	 * plan_params 包含本查询级别需要对正在规划的下级查询提供的表达式。
	 * outer_params 包含外层查询级别将提供给本级别的 PARAM_EXEC 参数的 paramId。
	 */
	List	   *plan_params;	/* PlannerParamItems 列表，见下文 */
	Bitmapset  *outer_params;

	/*
	 * simple_rel_array 保存指向“基表关系”（base rels）和“其他关系”（other rels）
	 * （详见 RelOptInfo 注释）的指针。它以 rangetable 索引为索引（因此条目 0 总是未使用）。
	 * 当一个 RTE 不对应基表（例如 join RTE 或未引用的 view RTE），或尚未创建 RelOptInfo 时，
	 * 相应条目可以为 NULL。
	 */
	struct RelOptInfo **simple_rel_array;	/* 所有单表 RelOptInfos 的数组 */
	int			simple_rel_array_size;	/* 数组已分配大小 */

	/*
	 * simple_rte_array 与 simple_rel_array 长度相同，保存对应的 rangetable 条目指针。
	 * 这样可以避免调用 rt_fetch()，在展开大型继承集合时会稍微慢一些。
	 */
	RangeTblEntry **simple_rte_array;	/* 以数组形式的 rangetable */

	/*
	 * append_rel_array 与上述数组长度相同，按 child_relid 索引保存对应的 AppendRelInfo 指针，
	 * 若没有则为 NULL。如果 append_rel_list 为空，则该数组本身不分配。
	 */
	struct AppendRelInfo **append_rel_array;

	/*
	 * all_baserels 是所有基表 relid 的 Relids 集合（不含 “other” rels）；
	 * 也就是我们需要形成的最终连接的 Relids 标识符。该集合在 make_one_rel 中计算，
	 * 就在开始生成 Paths 之前。
	 */
	Relids		all_baserels;

	/*
	 * nullable_baserels 是在连接树中某些外连接可能使之可空的基表 relids 集合；
	 * 这些是在 WHERE、SELECT targetlist 等之下可能为 NULL 的关系。在 deconstruct_jointree 中计算。
	 */
	Relids		nullable_baserels;

	/*
	 * join_rel_list 是在本次规划运行中我们考虑过的所有 join-relation RelOptInfo 的列表。
	 * 对于小问题我们直接扫描该列表以做查找，但当 join relation 很多时我们会建立哈希表以便快速查找。
	 * 当 join_rel_hash 非 NULL 时哈希表存在且有效。即便使用哈希表进行查找，我们仍保留列表；这简化了 GEQO 的处理。
	 */
	List	   *join_rel_list;	/* join-relation RelOptInfos 列表 */
	struct HTAB *join_rel_hash; /* 可选的 join relation 哈希表 */

	/*
	 * 在做动态规划型的连接搜索时，join_rel_level[k] 是级别 k 的所有 join-relation RelOptInfos 的列表，
	 * join_cur_level 是当前级别。新的 join-relation RelOptInfos 会被自动添加到 join_rel_level[join_cur_level] 列表中。
	 * 当未使用时 join_rel_level 为 NULL。
	 */
	List	  **join_rel_level; /* join-relation RelOptInfos 的分级列表 */
	int			join_cur_level; /* 正在扩展的 join_rel_level 列表的索引 */

	List	   *init_plans;		/* 查询的 init SubPlans */

	List	   *cte_plan_ids;	/* 每个 CTE 项对应的子计划 ID 列表（若未为该 CTE 生成子计划则为 -1） */

	List	   *multiexpr_params;	/* 用于 MULTIEXPR 子查询输出的 Params 列表的列表 */

	List	   *eq_classes;		/* 活跃的 EquivalenceClasses 列表 */

	List	   *canon_pathkeys; /* “规范化” PathKeys 列表 */

	List	   *left_join_clauses;	/* 针对 mergejoin 可用且左侧非空的外连接 RestrictInfos 列表 */

	List	   *right_join_clauses; /* 针对 mergejoin 可用且右侧非空的外连接 RestrictInfos 列表 */

	List	   *full_join_clauses;	/* 针对 mergejoin 可用的 full join RestrictInfos 列表 */

	List	   *join_info_list; /* SpecialJoinInfos 列表 */

	/*
	 * 注意：对于描述分区表各分区的 AppendRelInfos，我们保证在 PartitionDesc 中较早出现的分区
	 * 在 append_rel_list 中也较早出现。
	 */
	List	   *append_rel_list;	/* AppendRelInfos 列表 */

	List	   *rowMarks;		/* PlanRowMarks 列表 */

	List	   *placeholder_list;	/* PlaceHolderInfos 列表 */

	List	   *fkey_list;		/* ForeignKeyOptInfos 列表 */

	List	   *query_pathkeys; /* 传入 query_planner() 的期望 pathkeys */

	List	   *group_pathkeys; /* groupClause 的 pathkeys（如存在） */
	List	   *window_pathkeys;	/* 底层 window 的 pathkeys（如存在） */
	List	   *distinct_pathkeys;	/* distinctClause 的 pathkeys（如存在） */
	List	   *sort_pathkeys;	/* sortClause 的 pathkeys（如存在） */

	List	   *part_schemes;	/* 查询中使用的规范化分区方案列表 */

	List	   *initial_rels;	/* 我们当前正在尝试连接的 RelOptInfos 列表 */

	/* 使用 fetch_upper_rel() 获取任何特定的 upper rel */
	List	   *upper_rels[UPPERREL_FINAL + 1]; /* upper-rel 的 RelOptInfos */

	/* grouping_planner 为上层处理选择的结果 tlist */
	struct PathTarget *upper_targets[UPPERREL_FINAL + 1];

	/*
	 * 完全处理后的 targetlist 存放在这里。它不同于 parse->targetList，
	 * 因为（对于 INSERT 和 UPDATE）它已经按目标表重排并填充了默认值。
	 * 此外，可能存在额外的 resjunk targets。preprocess_targetlist() 大部分完成这项工作，
	 * 但注意在 appendrel 展开期间可能会添加更多的 resjunk targets。
	 * 因此，在那之前不应设置 upper_targets。
	 */
	List	   *processed_tlist;

	/* 在 create_plan() 中填写，供 setrefs.c 使用 */
	AttrNumber *grouping_map;	/* 用于 GroupingFunc 的修正 */
	List	   *minmax_aggs;	/* MinMaxAggInfos 列表 */

	MemoryContext planner_cxt;	/* 保存 PlannerInfo 的内存上下文 */

	double		total_table_pages;	/* 查询中所有非 dummy 表的页面总数 */

	double		tuple_fraction; /* 传入 query_planner 的 tuple_fraction */
	double		limit_tuples;	/* 传入 query_planner 的 limit_tuples */

	Index		qual_security_level;	/* quals 的最小 security_level */
	/* 注意：如果没有 securityQuals，则 qual_security_level 为 0 */

	InheritanceKind inhTargetKind;	/* 指示目标关系是继承子表、分区子表还是分区表本身 */
	bool		hasJoinRTEs;	/* 若有任何 RTE 为 RTE_JOIN 则为真 */
	bool		hasLateralRTEs; /* 若有任何 RTE 标记为 LATERAL 则为真 */
	bool		hasHavingQual;	/* 若 havingQual 非空则为真 */
	bool		hasPseudoConstantQuals; /* 若有任何 RestrictInfo 的 pseudoconstant = true 则为真 */
	bool		hasRecursion;	/* 若正在规划递归 WITH 项则为真 */

	/* 当 hasRecursion 为真时使用的字段： */
	int			wt_param_id;	/* work table 的 PARAM_EXEC ID */
	struct Path *non_recursive_path;	/* 非递归项的路径 */

	/* createplan.c 使用的工作区字段 */
	Relids		curOuterRels;	/* 当前节点之上可见的外部 rels */
	List	   *curOuterParams; /* 尚未分配的 NestLoopParams */

	/* join_search_hook 的可选私有数据，例如 GEQO */
	void	   *join_search_private;

	/* 本查询是否修改了任何分区键列？ */
	bool		partColsUpdated;
};


/*
 * 在那些已知 simple_rte_array[] 已经被准备好的位置，我们直接通过索引
 * 从该数组获取 RTE。对于可能在进入或离开 query_planner() 之前/之后执行
 * 的代码，应使用此宏以保证正确获取 RTE（会在必要时回退到 rt_fetch）。
 */
#define planner_rt_fetch(rti, root) \
	((root)->simple_rte_array ? (root)->simple_rte_array[rti] : \
	 rt_fetch(rti, (root)->parse->rtable))

/*
 * If multiple relations are partitioned the same way, all such partitions
 * will have a pointer to the same PartitionScheme.  A list of PartitionScheme
 * objects is attached to the PlannerInfo.  By design, the partition scheme
 * incorporates only the general properties of the partition method (LIST vs.
 * RANGE, number of partitioning columns and the type information for each)
 * and not the specific bounds.
 *
 * We store the opclass-declared input data types instead of the partition key
 * datatypes since the former rather than the latter are used to compare
 * partition bounds. Since partition key data types and the opclass declared
 * input data types are expected to be binary compatible (per ResolveOpClass),
 * both of those should have same byval and length properties.
 */
typedef struct PartitionSchemeData
{
	char		strategy;		/* partition strategy */
	int16		partnatts;		/* number of partition attributes */
	Oid		   *partopfamily;	/* OIDs of operator families */
	Oid		   *partopcintype;	/* OIDs of opclass declared input data types */
	Oid		   *partcollation;	/* OIDs of partitioning collations */

	/* Cached information about partition key data types. */
	int16	   *parttyplen;
	bool	   *parttypbyval;

	/* Cached information about partition comparison functions. */
	FmgrInfo   *partsupfunc;
}			PartitionSchemeData;

typedef struct PartitionSchemeData *PartitionScheme;

/*----------
 * RelOptInfo
 *		用于规划/优化的每个关系信息
 *
 * 在规划阶段，“基表（base rel）”是指出现在 rangetable 中的普通表（table）
 * 或出现在 FROM 子句中的子 SELECT 或函数的输出。在任一情况下，它由一个
 * RT 索引唯一标识。“joinrel”是两个或多个基表的连接。joinrel 由其组成基表
 * 的 RT 索引集合标识。我们为每个 baserel 和 joinrel 创建 RelOptInfo 节点，
 * 并分别将它们存储在 PlannerInfo 的 simple_rel_array 和 join_rel_list 中。
 *
 * 注意：对于任意给定的一组组成基表，不管我们以何种顺序将它们组装，都只有
 * 一个 joinrel；因此用无序集合作为标识类型是合适的。
 *
 * 我们还有“other rels”，它们类似于基表，因为它们引用单个 RT 索引；但它们
 * 不在连接树中，因此用不同的 RelOptKind 来标识。目前唯一的 otherrels 是为
 * “append relation”的成员关系制作的，即继承集或 UNION ALL 子查询的成员。
 * 一个 append relation 有一个作为基表的父 RTE，表示整个 append relation。
 * 成员 RTE 是 otherrels。成员 RTE 和 otherrels 用于规划 append 集合中各个表
 * 或子查询的扫描；然后父 baserel 会被赋予由各成员的最佳路径组成的 Append
 * 和/或 MergeAppend 路径。（关于 AppendRelInfo 的更多信息见相应注释。）
 *
 * 曾经我们还为表示连接 RTE 的情况创建 otherrels，用于处理连接别名 Vars。
 * 目前不需要这样做，因为所有连接别名 Vars 在 preprocess_expression 阶段
 * 已经被展开为非别名形式。
 *
 * 我们还为不同分区表的子关系之间的连接创建关系。这些关系不会被加入到
 * join_rel_level 列表，因为它们不会被动态规划算法直接连接。
 *
 * 还有一种 RelOptKind 用于“upper”关系，即描述扫描/连接之后处理步骤的
 * RelOptInfos，例如聚合（aggregation）。这些 RelOptInfo 的许多字段没有意义，
 * 但它们的 Path 字段总是保存执行该处理步骤的路径。
 *
 * 最后，还有一种 RelOptKind 用于“dead”关系，即那些我们已经证明不需要再
 * 参与连接的基表。
 *
 * 该数据结构的部分字段针对各种扫描和连接机制是专用的，因此并不值得为它们
 * 新建节点类型。
 *
 *		relids - 包含的基表标识集合；若只有一个则为基表，若多于一个则为 joinrel
 *		rows - 在应用限制性子句（restriction clauses）后估计的元组数
 *		consider_startup - 是否有必要因为低启动成本而保留普通路径
 *		consider_param_startup - 针对参数化路径的同样判断
 *		reltarget - 此关系扫描的默认 Path 输出目标列表；通常包含需要从该关系
 *					输出的 Var 和 PlaceHolderVar 节点。
 *					该列表无特定顺序，但 appendrel 集合中的所有关系必须使用
 *					对应的顺序。
 *					注意：在 appendrel 子关系中，可能包含从子查询提升的任意表达式！
 *		pathlist - Path 节点列表，每个表示生成该关系的一种可能方法
 *		ppilist - 与 pathlist 中的参数化路径对应的 ParamPathInfo 列表（若存在）
 *		cheapest_startup_path - 在未参数化路径中启动成本最低者（不考虑排序）；
 *			若不存在未参数化路径则为 NULL
 *		cheapest_total_path - 在未参数化路径中总成本最低者（不考虑排序）；
 *			如果不存在未参数化路径，则在具有最小参数化的路径中选择总成本最低者
 *		cheapest_unique_path - 缓存生成唯一（无重复）输出的最低成本路径；若尚
 *			未请求则为 NULL
 *		cheapest_parameterized_paths - 针对不同参数化情况的最佳路径列表；
 *			该列表总是包含 cheapest_total_path，即使它是未参数化的
 *		direct_lateral_relids - 该关系直接横向（LATERAL）引用的 rels
 *		lateral_relids - 横向参数化所需的最小外部 rels（Relids 集合，包含直接和间接引用）
 *
 * 若该关系为基表，将设置以下字段：
 *
 *		relid - RTE 索引（与 relids 字段冗余，但为了访问方便而提供）
 *		rtekind - RTE 的 rtekind 字段副本
 *		min_attr, max_attr - 该关系的有效 AttrNumber 范围
 *		attr_needed - 按属性索引的 bitmapset 数组，指示每个属性在最高的 joinrel
 *					  中是否需要；如果 bit 0 被设置则表示该属性在最终 targetlist 中需要
 *		attr_widths - 每属性宽度估计的缓存；为 0 表示尚未计算
 *		lateral_vars - 该关系引用的横向变量（Vars 和 PlaceHolderVars）列表
 *		lateral_referencers - 横向引用此关系的 rels 的 relids（包含直接和间接引用）
 *		indexlist - 该关系的 IndexOptInfo 列表（若不是表则始终为 NIL）
 *		pages - 关系的磁盘页面数估计（若不是表则为 0）
 *		tuples - 表中元组数估计（不考虑过滤）
 *		allvisfrac - 全可见页面比例估计
 *		subroot - 若为子查询，则为其 PlannerInfo；否则为 NULL
 *		subplan_params - 若为子查询，要传入子查询的 PlannerParamItems 列表
 *
 *		注意：对于子查询，tuples 和 subroot 并不会在创建 RelOptInfo 时立即设置；
 *		它们会在 set_subquery_pathlist 处理该对象时填充。
 *
 *		对于作为 appendrel 成员的 otherrels，这些字段与 baserel 类似地被填充，
 *		只是我们不为其维护 lateral_vars。
 *
 * 如果该关系是外部表或所有参与的外部连接都属于相同外部服务器且为同一用户
 * 分配了访问权限（参见 checkAsUser），则会设置以下字段：
 *
 *		serverid - 外部服务器的 OID（若为外部表，否则为 InvalidOid）
 *		userid - 用于检查权限的用户 OID（InvalidOid 表示当前用户）
 *		useridiscurrent - 标识 userid 是否等于当前用户
 *		fdwroutine - FDW 的函数钩子（若为外部表，否则为 NULL）
 *		fdw_private - FDW 的私有状态（若为外部表，否则为 NULL）
 *
 * 有两个字段用于缓存连接搜索过程中获得的关于当该 rel 与给定其他关系连接时
 * 是否可断言其是唯一（即对于任一给定的其他关系行，至多有一行匹配）的知识。
 * 目前我们仅尝试对基表进行此类证明，因此只会为基表填充这些字段；未来也可能
 * 用于 join rel：
 *
 *		unique_for_rels - Relid 集合列表，每个集合表示对于这些其他 rels 已证明
 *						  当前 rel 是唯一的
 *		non_unique_for_rels - Relid 集合列表，每个集合表示对于这些集合我们已尝试但
 *							  未能证明该 rel 是唯一的
 *
 * 下列字段的存在取决于该关系所参与的限制和连接：
 *
 *		baserestrictinfo - RestrictInfo 节点列表，包含该关系参与的每个非连接限制子句
 *						  的信息（仅用于基表）
 *		baserestrictcost - 在单个元组上评估 baserestrictinfo 子句的估计成本（仅用于基表）
 *		baserestrict_min_security - 在 baserestrictinfo 中发现的最小 security_level
 *		joininfo  - RestrictInfo 节点列表，包含该关系参与的每个连接子句的信息
 *		has_eclass_joins - 标志，若存在 EquivalenceClass 连接则为真（意味着 joininfo 可能不完整）
 *
 * 注意：在 RelOptInfo 中保存 restrictinfo 列表仅对基表有用，因为对于 join rel，
 * 被视为 restrict clauses 的子句集合会随我们选择连接的子关系而变化。
 * 因此在 JoinPath（字段 joinrestrictinfo）中保存连接级别的 restrictinfo 列表更合适。
 * 但将 joininfo 列表保留在 RelOptInfo 中是可以的，因为对于给定的 rel，无论我们如何形成它，
 * 该列表都是相同的。
 *
 * 我们在 RelOptInfo 中存储 baserestrictcost（仅对于基表）是因为我们知道至少需要一次
 * （用于估价顺序扫描），并且可能多次需要它来估价索引扫描。
 *
 * 如果该关系是分区表，则会设置以下字段：
 *
 *		part_scheme - 分区方案
 *		nparts - 分区数
 *		boundinfo - 分区边界信息
 *		partition_qual - 若不是根分区则为分区约束
 *		part_rels - 每个分区的 RelOptInfo（按边界顺序）
 *		partexprs, nullable_partexprs - 分区键表达式
 *		partitioned_child_rels - 分区表中未被裁剪的分区的 RT 索引列表，
 *								 按层次顺序
 *
 * 注意：一个基表总是只有一组分区键，但一个 join relation 可以有多组分区键，
 * 数量等于被连接的关系数。partexprs 和 nullable_partexprs 是包含
 * part_scheme->partnatts 元素的数组。每个元素本身是一个分区键表达式列表。
 * 对于基表，partexprs 中每个列表只包含一个表达式，nullable_partexprs 不被填充。
 * 对于 join relation，partexprs 和 nullable_partexprs 分别包含来自不可空和可空
 * 关系的分区键表达式。任一位置处这些数组中的列表一起包含的元素个数等于加入
 * 的关系数。
 *----------
 */
typedef enum RelOptKind
{
	RELOPT_BASEREL,			/* 基表关系（单一基表或子查询/函数结果） */
	RELOPT_JOINREL,			/* 连接关系（由多个基表组成的 join rel，连接操作的中间结果） */
	RELOPT_OTHER_MEMBER_REL,/* append 成员关系（other member rel，用于继承/UNION ALL 成员） */
	RELOPT_OTHER_JOINREL,	/* append 成员的 joinrel（other join rel） */
	RELOPT_UPPER_REL,		/* 上层关系（扫描/连接之后的处理，如聚合、窗口等） */
	RELOPT_OTHER_UPPER_REL,	/* 上层关系的成员（other upper rel，append/partition 情况） */
	RELOPT_DEADREL			/* 已证明为空的关系（dummy/死关系） */
} RelOptKind;

/*
 * 判断给定的 RelOptInfo 是否为简单关系（即基表或“其他”成员关系）
 */
#define IS_SIMPLE_REL(rel) \
	((rel)->reloptkind == RELOPT_BASEREL || \
	 (rel)->reloptkind == RELOPT_OTHER_MEMBER_REL)

/* 判断给定的关系是否为连接关系 */
#define IS_JOIN_REL(rel)	\
	((rel)->reloptkind == RELOPT_JOINREL || \
	 (rel)->reloptkind == RELOPT_OTHER_JOINREL)

/* 判断给定的关系是否为上层（upper）关系 */
#define IS_UPPER_REL(rel)	\
	((rel)->reloptkind == RELOPT_UPPER_REL || \
	 (rel)->reloptkind == RELOPT_OTHER_UPPER_REL)

/* 判断给定的关系是否为“其他”类型关系（other rel） */
#define IS_OTHER_REL(rel) \
	((rel)->reloptkind == RELOPT_OTHER_MEMBER_REL || \
	 (rel)->reloptkind == RELOPT_OTHER_JOINREL || \
	 (rel)->reloptkind == RELOPT_OTHER_UPPER_REL)

typedef struct RelOptInfo
{
	NodeTag		type;

	RelOptKind	reloptkind;

	/* 此 RelOptInfo 包含的所有基表 relids 集合（rangetable 索引）
	 * 对于基表类型的 reloptkind，该集合仅包含一个元素，relids 存储的是单个基表的 rtindex 索引
	 * 对于连接类型的 reloptkind，该集合包含多个元素，relids 存储的是组成该连接的各个基表的 rtindex 索引
	 */
	Relids		relids;

	/* planner 生成的大小估计 */
	double		rows;			/* 估计的结果元组数 */

	/* 每个关系的规划控制标志
	 * 对查询的物理路径做预检（add_path_precheck）的时候，是否考虑预检启动代价（startup_cost）
	 */
	bool		consider_startup;			/* 针对非参数化路径，是否保留低启动成本的路径？ */
	bool		consider_param_startup; 	/* 同上，针对参数化路径，在 Semi Join 和 Anti Join 中是否考虑启动成本？参照 set_base_rel_consider_startup 函数 */
	bool		consider_parallel;			/* 是否考虑并行路径？ */

	/* RelOptInfo 的查询结果对应的投影列 */
	struct PathTarget *reltarget;			/* 要计算的 Vars/Expr 列表、成本、宽度 */

	/* 物化/路径信息 */
	List	   *pathlist;						/* Path 结构列表，记录所有可行的路径 */
	List	   *ppilist;						/* pathlist 中使用的 ParamPathInfo 列表，参数化路径的参数 */
	List* partial_pathlist;				/* 部分（partial）Path 列表 */
	
	struct Path* 	cheapest_startup_path; 			/* 启动成本最低的 Path */
	struct Path* 	cheapest_total_path;   			/* 总成本最低的 Path */
	struct Path* 	cheapest_unique_path;  			/* 产生唯一输出的最低成本 Path，和 Semi Join 有关*/
	List* 			cheapest_parameterized_paths; 	/* 包含所有的参数化路径，另外 cheapest_total_path 也在此列表中，即使它不是参数化路径 */

	/* 基表和连接表均需的参数化信息（参见 lateral_vars 和 lateral_referencers） */
	Relids		direct_lateral_relids;	/* 语句中直接指出的 LATERAL rels */
	Relids		lateral_relids; 		/* 最小的横向参数化 rels，由 direct_lateral_relids 推导而来的表之间的依赖关系 */

	/* 关于基表（RELOPT_BASEREL）类型的 RelOptInfo 的信息（对于连接表（RELOPT_JOINREL）不设置） */
	Index		relid;					/* RTE 索引，基表的 rtindex */
	Oid			reltablespace;			/* 所在表空间 */
	RTEKind		rtekind;				/* RTE 的类型：RELATION、SUBQUERY、FUNCTION 等 */

	AttrNumber	min_attr;				/* 关系的最小属性号（常可 < 0），即表中的首列编号。对 RTE_RELATION 类型的 RTE，min_attr 应该是 FirstLowInvalidHeapAttributeNumber+1，否则应该是 0 */
	AttrNumber	max_attr;				/* 关系的最大属性号，即表中的末列编号。对于 RTE_SUBQUERY、RTE_FUNCTION 类型的 RTE，max_attr 应该是这个 RelOptInfo 结果集投影列的个数 */
	Relids	   *attr_needed;			/* 按属性索引的 bitmapset 数组，表示每属性在何处需要，长度为 max_attr - min_attr + 1 */
	int32	   *attr_widths;			/* 按属性索引的宽度估计缓存；为 0 表示尚未计算，每个数组元素代表了该列的宽度 */
	
	List	   *lateral_vars;			/* 该关系引用的 LATERAL Vars 和 PlaceHolderVar 列表 */
	Relids		lateral_referencers;	/* 引用该关系的 rels（横向参考） */
	
	List	   *indexlist;				/* 该关系的 IndexOptInfo 列表，即该表的所有索引 */
	List	   *statlist;				/* StatisticExtInfo 列表，扩展多列统计信息 */
	BlockNumber pages;					/* 从 pg_class 等得出的页面数估计 */
	double		tuples;					/* 表中元组数估计（不考虑过滤） */
	double		allvisfrac;				/* 全可见页面的比例估计 */

	PlannerInfo* 	subroot;				/* 若为子查询，则为其 PlannerInfo，子查询生成的子执行计划 */
	List	   		*subplan_params; 		/* 若为子查询，传入子查询的 PlannerParamItems 列表，子查询的参数 */
	int				rel_parallel_workers;	/* 希望的并行 worker 数 */

	/* 关于外部表和外部连接的信息 */
	Oid			serverid;			/* 外部表或连接所在的 server OID */
	Oid			userid;				/* 用于检查权限的用户 OID */
	bool		useridiscurrent;	/* 标识 userid 是否等于当前用户 */
	/* 使用 "struct FdwRoutine" 避免在此包含 fdwapi.h */
	struct FdwRoutine *fdwroutine;
	void	   *fdw_private;

	/* 用于缓存我们是否已经证明该关系在作为内表情况下具有唯一性 */
	List	   *unique_for_rels;		/* 已知对这些其他 relid 集合是唯一的 */
	List	   *non_unique_for_rels;	/* 已知对这些集合不是唯一的 */

	/* 被各种扫描和连接使用： */
	List	   *baserestrictinfo;			/* RestrictInfo 结构列表（若为基表），基表上的过滤条件 */
	QualCost	baserestrictcost;			/* 评估上述约束的成本 */
	Index		baserestrict_min_security;	/* baserestrictinfo 中发现的最小 security_level */
	List	   *joininfo;					/* 涉及该关系的连接条件的 RestrictInfo 列表，连接条件 */
	bool		has_eclass_joins;			/* 为 true 表示存在 EquivalenceClass 等值连接条件（即 joininfo 可能不完整） */

	/* 用于分区感知的连接： */
	bool		consider_partitionwise_join;	/* 是否考虑分区感知连接路径？（若为分区关系） */
	Relids		top_parent_relids;				/* 顶层父关系的 Relids（若为 "other" rel） */

	/* 用于分区关系 */
	PartitionScheme part_scheme;				/* 分区方案 */
	int			nparts;							/* 分区数量 */
	struct PartitionBoundInfoData *boundinfo;	/* 分区边界信息 */
	List	   *partition_qual;					/* 分区约束 */
	struct RelOptInfo **part_rels;				/* 分区的 RelOptInfo 数组，按边界顺序存放 */
	List	  **partexprs;						/* 非可空的分区键表达式数组（每个分区键一个 list） */
	List	  **nullable_partexprs;				/* 可空的分区键表达式数组 */
	List	   *partitioned_child_rels;			/* 分区子表的 RT 索引列表 */
} RelOptInfo;

/*
 * Is given relation partitioned?
 *
 * It's not enough to test whether rel->part_scheme is set, because it might
 * be that the basic partitioning properties of the input relations matched
 * but the partition bounds did not.  Also, if we are able to prove a rel
 * dummy (empty), we should henceforth treat it as unpartitioned.
 */
#define IS_PARTITIONED_REL(rel) \
	((rel)->part_scheme && (rel)->boundinfo && (rel)->nparts > 0 && \
	 (rel)->part_rels && !IS_DUMMY_REL(rel))

/*
 * Convenience macro to make sure that a partitioned relation has all the
 * required members set.
 */
#define REL_HAS_ALL_PART_PROPS(rel)	\
	((rel)->part_scheme && (rel)->boundinfo && (rel)->nparts > 0 && \
	 (rel)->part_rels && (rel)->partexprs && (rel)->nullable_partexprs)

/*
 * IndexOptInfo
 *		Per-index information for planning/optimization
 *
 *		indexkeys[], indexcollations[] each have ncolumns entries.
 *		opfamily[], and opcintype[]	each have nkeycolumns entries. They do
 *		not contain any information about included attributes.
 *
 *		sortopfamily[], reverse_sort[], and nulls_first[] have
 *		nkeycolumns entries, if the index is ordered; but if it is unordered,
 *		those pointers are NULL.
 *
 *		Zeroes in the indexkeys[] array indicate index columns that are
 *		expressions; there is one element in indexprs for each such column.
 *
 *		For an ordered index, reverse_sort[] and nulls_first[] describe the
 *		sort ordering of a forward indexscan; we can also consider a backward
 *		indexscan, which will generate the reverse ordering.
 *
 *		The indexprs and indpred expressions have been run through
 *		prepqual.c and eval_const_expressions() for ease of matching to
 *		WHERE clauses. indpred is in implicit-AND form.
 *
 *		indextlist is a TargetEntry list representing the index columns.
 *		It provides an equivalent base-relation Var for each simple column,
 *		and links to the matching indexprs element for each expression column.
 *
 *		While most of these fields are filled when the IndexOptInfo is created
 *		(by plancat.c), indrestrictinfo and predOK are set later, in
 *		check_index_predicates().
 */
#ifndef HAVE_INDEXOPTINFO_TYPEDEF
typedef struct IndexOptInfo IndexOptInfo;
#define HAVE_INDEXOPTINFO_TYPEDEF 1
#endif

struct IndexOptInfo
{
	NodeTag		type;

	Oid			indexoid;		/* OID of the index relation */
	Oid			reltablespace;	/* tablespace of index (not table) */
	RelOptInfo *rel;			/* back-link to index's table */

	/* index-size statistics (from pg_class and elsewhere) */
	BlockNumber pages;			/* number of disk pages in index */
	double		tuples;			/* number of index tuples in index */
	int			tree_height;	/* index tree height, or -1 if unknown */

	/* index descriptor information */
	int			ncolumns;		/* number of columns in index */
	int			nkeycolumns;	/* number of key columns in index */
	int		   *indexkeys;		/* column numbers of index's attributes both
								 * key and included columns, or 0 */
	Oid		   *indexcollations;	/* OIDs of collations of index columns */
	Oid		   *opfamily;		/* OIDs of operator families for columns */
	Oid		   *opcintype;		/* OIDs of opclass declared input data types */
	Oid		   *sortopfamily;	/* OIDs of btree opfamilies, if orderable */
	bool	   *reverse_sort;	/* is sort order descending? */
	bool	   *nulls_first;	/* do NULLs come first in the sort order? */
	bool	   *canreturn;		/* which index cols can be returned in an
								 * index-only scan? */
	Oid			relam;			/* OID of the access method (in pg_am) */

	List	   *indexprs;		/* expressions for non-simple index columns */
	List	   *indpred;		/* predicate if a partial index, else NIL */

	List	   *indextlist;		/* targetlist representing index columns */

	List	   *indrestrictinfo;	/* parent relation's baserestrictinfo
									 * list, less any conditions implied by
									 * the index's predicate (unless it's a
									 * target rel, see comments in
									 * check_index_predicates()) */

	bool		predOK;			/* true if index predicate matches query */
	bool		unique;			/* true if a unique index */
	bool		immediate;		/* is uniqueness enforced immediately? */
	bool		hypothetical;	/* true if index doesn't really exist */

	/* Remaining fields are copied from the index AM's API struct: */
	bool		amcanorderbyop; /* does AM support order by operator result? */
	bool		amoptionalkey;	/* can query omit key for the first column? */
	bool		amsearcharray;	/* can AM handle ScalarArrayOpExpr quals? */
	bool		amsearchnulls;	/* can AM search for NULL/NOT NULL entries? */
	bool		amhasgettuple;	/* does AM have amgettuple interface? */
	bool		amhasgetbitmap; /* does AM have amgetbitmap interface? */
	bool		amcanparallel;	/* does AM support parallel scan? */
	bool		amcanmarkpos;	/* does AM support mark/restore? */
	/* Rather than include amapi.h here, we declare amcostestimate like this */
	void		(*amcostestimate) ();	/* AM's cost estimator */
};

/*
 * ForeignKeyOptInfo
 *		Per-foreign-key information for planning/optimization
 *
 * The per-FK-column arrays can be fixed-size because we allow at most
 * INDEX_MAX_KEYS columns in a foreign key constraint.  Each array has
 * nkeys valid entries.
 */
typedef struct ForeignKeyOptInfo
{
	NodeTag		type;

	/* Basic data about the foreign key (fetched from catalogs): */
	Index		con_relid;		/* RT index of the referencing table */
	Index		ref_relid;		/* RT index of the referenced table */
	int			nkeys;			/* number of columns in the foreign key */
	AttrNumber	conkey[INDEX_MAX_KEYS]; /* cols in referencing table */
	AttrNumber	confkey[INDEX_MAX_KEYS];	/* cols in referenced table */
	Oid			conpfeqop[INDEX_MAX_KEYS];	/* PK = FK operator OIDs */

	/* Derived info about whether FK's equality conditions match the query: */
	int			nmatched_ec;	/* # of FK cols matched by ECs */
	int			nmatched_rcols; /* # of FK cols matched by non-EC rinfos */
	int			nmatched_ri;	/* total # of non-EC rinfos matched to FK */
	/* Pointer to eclass matching each column's condition, if there is one */
	struct EquivalenceClass *eclass[INDEX_MAX_KEYS];
	/* List of non-EC RestrictInfos matching each column's condition */
	List	   *rinfos[INDEX_MAX_KEYS];
} ForeignKeyOptInfo;

/*
 * StatisticExtInfo
 *		Information about extended statistics for planning/optimization
 *
 * Each pg_statistic_ext row is represented by one or more nodes of this
 * type, or even zero if ANALYZE has not computed them.
 */
typedef struct StatisticExtInfo
{
	NodeTag		type;

	Oid			statOid;		/* OID of the statistics row */
	RelOptInfo *rel;			/* back-link to statistic's table */
	char		kind;			/* statistics kind of this entry */
	Bitmapset  *keys;			/* attnums of the columns covered */
} StatisticExtInfo;

/*
 * EquivalenceClasses
 *
 * Whenever we can determine that a mergejoinable equality clause A = B is
 * not delayed by any outer join, we create an EquivalenceClass containing
 * the expressions A and B to record this knowledge.  If we later find another
 * equivalence B = C, we add C to the existing EquivalenceClass; this may
 * require merging two existing EquivalenceClasses.  At the end of the qual
 * distribution process, we have sets of values that are known all transitively
 * equal to each other, where "equal" is according to the rules of the btree
 * operator family(s) shown in ec_opfamilies, as well as the collation shown
 * by ec_collation.  (We restrict an EC to contain only equalities whose
 * operators belong to the same set of opfamilies.  This could probably be
 * relaxed, but for now it's not worth the trouble, since nearly all equality
 * operators belong to only one btree opclass anyway.  Similarly, we suppose
 * that all or none of the input datatypes are collatable, so that a single
 * collation value is sufficient.)
 *
 * We also use EquivalenceClasses as the base structure for PathKeys, letting
 * us represent knowledge about different sort orderings being equivalent.
 * Since every PathKey must reference an EquivalenceClass, we will end up
 * with single-member EquivalenceClasses whenever a sort key expression has
 * not been equivalenced to anything else.  It is also possible that such an
 * EquivalenceClass will contain a volatile expression ("ORDER BY random()"),
 * which is a case that can't arise otherwise since clauses containing
 * volatile functions are never considered mergejoinable.  We mark such
 * EquivalenceClasses specially to prevent them from being merged with
 * ordinary EquivalenceClasses.  Also, for volatile expressions we have
 * to be careful to match the EquivalenceClass to the correct targetlist
 * entry: consider SELECT random() AS a, random() AS b ... ORDER BY b,a.
 * So we record the SortGroupRef of the originating sort clause.
 *
 * We allow equality clauses appearing below the nullable side of an outer join
 * to form EquivalenceClasses, but these have a slightly different meaning:
 * the included values might be all NULL rather than all the same non-null
 * values.  See src/backend/optimizer/README for more on that point.
 *
 * NB: if ec_merged isn't NULL, this class has been merged into another, and
 * should be ignored in favor of using the pointed-to class.
 */
typedef struct EquivalenceClass
{
	NodeTag		type;

	List	   *ec_opfamilies;	/* btree operator family OIDs */
	Oid			ec_collation;	/* collation, if datatypes are collatable */
	List	   *ec_members;		/* list of EquivalenceMembers */
	List	   *ec_sources;		/* list of generating RestrictInfos */
	List	   *ec_derives;		/* list of derived RestrictInfos */
	Relids		ec_relids;		/* all relids appearing in ec_members, except
								 * for child members (see below) */
	bool		ec_has_const;	/* any pseudoconstants in ec_members? */
	bool		ec_has_volatile;	/* the (sole) member is a volatile expr */
	bool		ec_below_outer_join;	/* equivalence applies below an OJ */
	bool		ec_broken;		/* failed to generate needed clauses? */
	Index		ec_sortref;		/* originating sortclause label, or 0 */
	Index		ec_min_security;	/* minimum security_level in ec_sources */
	Index		ec_max_security;	/* maximum security_level in ec_sources */
	struct EquivalenceClass *ec_merged; /* set if merged into another EC */
} EquivalenceClass;

/*
 * If an EC contains a const and isn't below-outer-join, any PathKey depending
 * on it must be redundant, since there's only one possible value of the key.
 */
#define EC_MUST_BE_REDUNDANT(eclass)  \
	((eclass)->ec_has_const && !(eclass)->ec_below_outer_join)

/*
 * EquivalenceMember - one member expression of an EquivalenceClass
 *
 * em_is_child signifies that this element was built by transposing a member
 * for an appendrel parent relation to represent the corresponding expression
 * for an appendrel child.  These members are used for determining the
 * pathkeys of scans on the child relation and for explicitly sorting the
 * child when necessary to build a MergeAppend path for the whole appendrel
 * tree.  An em_is_child member has no impact on the properties of the EC as a
 * whole; in particular the EC's ec_relids field does NOT include the child
 * relation.  An em_is_child member should never be marked em_is_const nor
 * cause ec_has_const or ec_has_volatile to be set, either.  Thus, em_is_child
 * members are not really full-fledged members of the EC, but just reflections
 * or doppelgangers of real members.  Most operations on EquivalenceClasses
 * should ignore em_is_child members, and those that don't should test
 * em_relids to make sure they only consider relevant members.
 *
 * em_datatype is usually the same as exprType(em_expr), but can be
 * different when dealing with a binary-compatible opfamily; in particular
 * anyarray_ops would never work without this.  Use em_datatype when
 * looking up a specific btree operator to work with this expression.
 */
typedef struct EquivalenceMember
{
	NodeTag		type;

	Expr	   *em_expr;		/* the expression represented */
	Relids		em_relids;		/* all relids appearing in em_expr */
	Relids		em_nullable_relids; /* nullable by lower outer joins */
	bool		em_is_const;	/* expression is pseudoconstant? */
	bool		em_is_child;	/* derived version for a child relation? */
	Oid			em_datatype;	/* the "nominal type" used by the opfamily */
} EquivalenceMember;

/*
 * PathKeys
 *
 * The sort ordering of a path is represented by a list of PathKey nodes.
 * An empty list implies no known ordering.  Otherwise the first item
 * represents the primary sort key, the second the first secondary sort key,
 * etc.  The value being sorted is represented by linking to an
 * EquivalenceClass containing that value and including pk_opfamily among its
 * ec_opfamilies.  The EquivalenceClass tells which collation to use, too.
 * This is a convenient method because it makes it trivial to detect
 * equivalent and closely-related orderings. (See optimizer/README for more
 * information.)
 *
 * Note: pk_strategy is either BTLessStrategyNumber (for ASC) or
 * BTGreaterStrategyNumber (for DESC).  We assume that all ordering-capable
 * index types will use btree-compatible strategy numbers.
 */
typedef struct PathKey
{
	NodeTag		type;

	EquivalenceClass *pk_eclass;	/* the value that is ordered */
	Oid			pk_opfamily;	/* btree opfamily defining the ordering */
	int			pk_strategy;	/* sort direction (ASC or DESC) */
	bool		pk_nulls_first; /* do NULLs come before normal values? */
} PathKey;


/*
 * PathTarget
 *
 * This struct contains what we need to know during planning about the
 * targetlist (output columns) that a Path will compute.  Each RelOptInfo
 * includes a default PathTarget, which its individual Paths may simply
 * reference.  However, in some cases a Path may compute outputs different
 * from other Paths, and in that case we make a custom PathTarget for it.
 * For example, an indexscan might return index expressions that would
 * otherwise need to be explicitly calculated.  (Note also that "upper"
 * relations generally don't have useful default PathTargets.)
 *
 * exprs contains bare expressions; they do not have TargetEntry nodes on top,
 * though those will appear in finished Plans.
 *
 * sortgrouprefs[] is an array of the same length as exprs, containing the
 * corresponding sort/group refnos, or zeroes for expressions not referenced
 * by sort/group clauses.  If sortgrouprefs is NULL (which it generally is in
 * RelOptInfo.reltarget targets; only upper-level Paths contain this info),
 * we have not identified sort/group columns in this tlist.  This allows us to
 * deal with sort/group refnos when needed with less expense than including
 * TargetEntry nodes in the exprs list.
 */
typedef struct PathTarget
{
	NodeTag		type;
	List	   *exprs;			/* list of expressions to be computed */
	Index	   *sortgrouprefs;	/* corresponding sort/group refnos, or 0 */
	QualCost	cost;			/* cost of evaluating the expressions */
	int			width;			/* estimated avg width of result tuples */
} PathTarget;

/* Convenience macro to get a sort/group refno from a PathTarget */
#define get_pathtarget_sortgroupref(target, colno) \
	((target)->sortgrouprefs ? (target)->sortgrouprefs[colno] : (Index) 0)


/*
 * ParamPathInfo
 *
 * All parameterized paths for a given relation with given required outer rels
 * link to a single ParamPathInfo, which stores common information such as
 * the estimated rowcount for this parameterization.  We do this partly to
 * avoid recalculations, but mostly to ensure that the estimated rowcount
 * is in fact the same for every such path.
 *
 * Note: ppi_clauses is only used in ParamPathInfos for base relation paths;
 * in join cases it's NIL because the set of relevant clauses varies depending
 * on how the join is formed.  The relevant clauses will appear in each
 * parameterized join path's joinrestrictinfo list, instead.
 */
typedef struct ParamPathInfo
{
	NodeTag		type;

	Relids		ppi_req_outer;	/* rels supplying parameters used by path */
	double		ppi_rows;		/* estimated number of result tuples */
	List	   *ppi_clauses;	/* join clauses available from outer rels */
} ParamPathInfo;


/*
 * Path 类型直接用于顺序扫描路径，以及其它一些不需要在 path 中
 * 保存额外信息的简单计划类型。
 *
 * "pathtype" 指明了可由此 Path 构建的 Plan 节点的 NodeTag。
 * 这与 Path 本身的 NodeTag 部分重叠，但允许在不同 Plan 类型
 * 之间复用同一种 Path 类型，在路径处理期间无需区分具体的 Plan 类型。
 *
 * "parent" 标识该 Path 所扫描的关系，"pathtarget" 描述该 Path
 * 将计算出的精确输出列集合。在简单情况下，一个关系的所有 Path
 * 共享同一目标列表，此时 path->pathtarget 等于 parent->reltarget。
 *
 * 如果 param_info 非 NULL，则它指向一个 ParamPathInfo，说明每次对
 * 该 path 的扫描会使用来自某些外部关系的参数值。也就是说，
 * 该 path 只能通过嵌套循环（nestloop）将其作为内侧连接到这些外部关系。
 * 同时应注意，参数化路径负责测试所有涉及该关系与指定外部关系的
 * “可移动”连接子句。
 *
 * "rows" 在简单路径中与 parent->rows 相同，但在参数化路径和 UniquePath
 * 中它可能小于 parent->rows，以反映已经由额外连接条件过滤或去重后
 * 的估计行数。
 *
 * "pathkeys" 是一个 PathKey 节点的 List（见上文），描述了该 path 输出行的排序顺序。
 */
typedef struct Path
{
	NodeTag		type;

	NodeTag		pathtype;		/* 标识可构建的扫描/连接方法的 tag */

	RelOptInfo *parent;			/* 该 path 能构建的关系 */
	PathTarget *pathtarget;		/* 要计算的 Vars/Expr 列表、成本、宽度 */

	ParamPathInfo *param_info;	/* 参数化信息，若无则为 NULL */

	bool		parallel_aware; /* 是否启用并行感知逻辑？ */
	bool		parallel_safe;	/* 是否可安全用于并行计划？ */
	int			parallel_workers;	/* 期望的 worker 数；0 表示不并行 */

	/* 路径的估计大小/代价（详见 costsize.c） */
	double		rows;			/* 估计的结果元组数 */
	Cost		startup_cost;	/* 在获取任何元组前产生的代价 */
	Cost		total_cost;		/* 总代价（假定获取所有元组） */

	List	   *pathkeys;		/* path 输出的排序顺序 */
	/* pathkeys 是 PathKey 节点的 List；详见上文 */
} Path;

/* Macro for extracting a path's parameterization relids; beware double eval */
#define PATH_REQ_OUTER(path)  \
	((path)->param_info ? (path)->param_info->ppi_req_outer : (Relids) NULL)

/*----------
 * IndexPath represents an index scan over a single index.
 *
 * This struct is used for both regular indexscans and index-only scans;
 * path.pathtype is T_IndexScan or T_IndexOnlyScan to show which is meant.
 *
 * 'indexinfo' is the index to be scanned.
 *
 * 'indexclauses' is a list of IndexClause nodes, each representing one
 * index-checkable restriction, with implicit AND semantics across the list.
 * An empty list implies a full index scan.
 *
 * 'indexorderbys', if not NIL, is a list of ORDER BY expressions that have
 * been found to be usable as ordering operators for an amcanorderbyop index.
 * The list must match the path's pathkeys, ie, one expression per pathkey
 * in the same order.  These are not RestrictInfos, just bare expressions,
 * since they generally won't yield booleans.  It's guaranteed that each
 * expression has the index key on the left side of the operator.
 *
 * 'indexorderbycols' is an integer list of index column numbers (zero-based)
 * of the same length as 'indexorderbys', showing which index column each
 * ORDER BY expression is meant to be used with.  (There is no restriction
 * on which index column each ORDER BY can be used with.)
 *
 * 'indexscandir' is one of:
 *		ForwardScanDirection: forward scan of an ordered index
 *		BackwardScanDirection: backward scan of an ordered index
 *		NoMovementScanDirection: scan of an unordered index, or don't care
 * (The executor doesn't care whether it gets ForwardScanDirection or
 * NoMovementScanDirection for an indexscan, but the planner wants to
 * distinguish ordered from unordered indexes for building pathkeys.)
 *
 * 'indextotalcost' and 'indexselectivity' are saved in the IndexPath so that
 * we need not recompute them when considering using the same index in a
 * bitmap index/heap scan (see BitmapHeapPath).  The costs of the IndexPath
 * itself represent the costs of an IndexScan or IndexOnlyScan plan type.
 *----------
 */
typedef struct IndexPath
{
	Path		path;
	IndexOptInfo *indexinfo;
	List	   *indexclauses;
	List	   *indexorderbys;
	List	   *indexorderbycols;
	ScanDirection indexscandir;
	Cost		indextotalcost;
	Selectivity indexselectivity;
} IndexPath;

/*
 * Each IndexClause references a RestrictInfo node from the query's WHERE
 * or JOIN conditions, and shows how that restriction can be applied to
 * the particular index.  We support both indexclauses that are directly
 * usable by the index machinery, which are typically of the form
 * "indexcol OP pseudoconstant", and those from which an indexable qual
 * can be derived.  The simplest such transformation is that a clause
 * of the form "pseudoconstant OP indexcol" can be commuted to produce an
 * indexable qual (the index machinery expects the indexcol to be on the
 * left always).  Another example is that we might be able to extract an
 * indexable range condition from a LIKE condition, as in "x LIKE 'foo%bar'"
 * giving rise to "x >= 'foo' AND x < 'fop'".  Derivation of such lossy
 * conditions is done by a planner support function attached to the
 * indexclause's top-level function or operator.
 *
 * indexquals is a list of RestrictInfos for the directly-usable index
 * conditions associated with this IndexClause.  In the simplest case
 * it's a one-element list whose member is iclause->rinfo.  Otherwise,
 * it contains one or more directly-usable indexqual conditions extracted
 * from the given clause.  The 'lossy' flag indicates whether the
 * indexquals are semantically equivalent to the original clause, or
 * represent a weaker condition.
 *
 * Normally, indexcol is the index of the single index column the clause
 * works on, and indexcols is NIL.  But if the clause is a RowCompareExpr,
 * indexcol is the index of the leading column, and indexcols is a list of
 * all the affected columns.  (Note that indexcols matches up with the
 * columns of the actual indexable RowCompareExpr in indexquals, which
 * might be different from the original in rinfo.)
 *
 * An IndexPath's IndexClause list is required to be ordered by index
 * column, i.e. the indexcol values must form a nondecreasing sequence.
 * (The order of multiple clauses for the same index column is unspecified.)
 */
typedef struct IndexClause
{
	NodeTag		type;
	struct RestrictInfo *rinfo; /* original restriction or join clause */
	List	   *indexquals;		/* indexqual(s) derived from it */
	bool		lossy;			/* are indexquals a lossy version of clause? */
	AttrNumber	indexcol;		/* index column the clause uses (zero-based) */
	List	   *indexcols;		/* multiple index columns, if RowCompare */
} IndexClause;

/*
 * BitmapHeapPath represents one or more indexscans that generate TID bitmaps
 * instead of directly accessing the heap, followed by AND/OR combinations
 * to produce a single bitmap, followed by a heap scan that uses the bitmap.
 * Note that the output is always considered unordered, since it will come
 * out in physical heap order no matter what the underlying indexes did.
 *
 * The individual indexscans are represented by IndexPath nodes, and any
 * logic on top of them is represented by a tree of BitmapAndPath and
 * BitmapOrPath nodes.  Notice that we can use the same IndexPath node both
 * to represent a regular (or index-only) index scan plan, and as the child
 * of a BitmapHeapPath that represents scanning the same index using a
 * BitmapIndexScan.  The startup_cost and total_cost figures of an IndexPath
 * always represent the costs to use it as a regular (or index-only)
 * IndexScan.  The costs of a BitmapIndexScan can be computed using the
 * IndexPath's indextotalcost and indexselectivity.
 */
typedef struct BitmapHeapPath
{
	Path		path;
	Path	   *bitmapqual;		/* IndexPath, BitmapAndPath, BitmapOrPath */
} BitmapHeapPath;

/*
 * BitmapAndPath represents a BitmapAnd plan node; it can only appear as
 * part of the substructure of a BitmapHeapPath.  The Path structure is
 * a bit more heavyweight than we really need for this, but for simplicity
 * we make it a derivative of Path anyway.
 */
typedef struct BitmapAndPath
{
	Path		path;
	List	   *bitmapquals;	/* IndexPaths and BitmapOrPaths */
	Selectivity bitmapselectivity;
} BitmapAndPath;

/*
 * BitmapOrPath represents a BitmapOr plan node; it can only appear as
 * part of the substructure of a BitmapHeapPath.  The Path structure is
 * a bit more heavyweight than we really need for this, but for simplicity
 * we make it a derivative of Path anyway.
 */
typedef struct BitmapOrPath
{
	Path		path;
	List	   *bitmapquals;	/* IndexPaths and BitmapAndPaths */
	Selectivity bitmapselectivity;
} BitmapOrPath;

/*
 * TidPath represents a scan by TID
 *
 * tidquals is an implicitly OR'ed list of qual expressions of the form
 * "CTID = pseudoconstant", or "CTID = ANY(pseudoconstant_array)",
 * or a CurrentOfExpr for the relation.
 */
typedef struct TidPath
{
	Path		path;
	List	   *tidquals;		/* qual(s) involving CTID = something */
} TidPath;

/*
 * SubqueryScanPath represents a scan of an unflattened subquery-in-FROM
 *
 * Note that the subpath comes from a different planning domain; for example
 * RTE indexes within it mean something different from those known to the
 * SubqueryScanPath.  path.parent->subroot is the planning context needed to
 * interpret the subpath.
 */
typedef struct SubqueryScanPath
{
	Path		path;
	Path	   *subpath;		/* path representing subquery execution */
} SubqueryScanPath;

/*
 * ForeignPath represents a potential scan of a foreign table, foreign join
 * or foreign upper-relation.
 *
 * fdw_private stores FDW private data about the scan.  While fdw_private is
 * not actually touched by the core code during normal operations, it's
 * generally a good idea to use a representation that can be dumped by
 * nodeToString(), so that you can examine the structure during debugging
 * with tools like pprint().
 */
typedef struct ForeignPath
{
	Path		path;
	Path	   *fdw_outerpath;
	List	   *fdw_private;
} ForeignPath;

/*
 * CustomPath represents a table scan or a table join done by some out-of-core
 * extension.
 *
 * We provide a set of hooks here - which the provider must take care to set
 * up correctly - to allow extensions to supply their own methods of scanning
 * a relation or joing relations.  For example, a provider might provide GPU
 * acceleration, a cache-based scan, or some other kind of logic we haven't
 * dreamed up yet.
 *
 * CustomPaths can be injected into the planning process for a base or join
 * relation by set_rel_pathlist_hook or set_join_pathlist_hook functions,
 * respectively.
 *
 * Core code must avoid assuming that the CustomPath is only as large as
 * the structure declared here; providers are allowed to make it the first
 * element in a larger structure.  (Since the planner never copies Paths,
 * this doesn't add any complication.)  However, for consistency with the
 * FDW case, we provide a "custom_private" field in CustomPath; providers
 * may prefer to use that rather than define another struct type.
 */

struct CustomPathMethods;

typedef struct CustomPath
{
	Path		path;
	uint32		flags;			/* mask of CUSTOMPATH_* flags, see
								 * nodes/extensible.h */
	List	   *custom_paths;	/* list of child Path nodes, if any */
	List	   *custom_private;
	const struct CustomPathMethods *methods;
} CustomPath;

/*
 * AppendPath 表示一个 Append 计划节点，即依次执行多个成员计划。
 *
 * 对于 partial Append，'subpaths' 包含非 partial 的 subpath，后面跟着 partial 的 subpath。
 *
 * 注意："subpaths" 可能只包含一个，甚至没有元素。这些情况会在 create_append_plan 中进行优化。
 * 特别地，一个没有 subpaths 的 AppendPath 是一个“dummy”路径，用于表示关系被证明为空的情况。
 * （这样表示很方便，因为当我们构建一个 appendrel 并发现所有子节点都被排除时，无需额外操作即可识别该关系为 dummy。）
 */
typedef struct AppendPath
{
	Path		path;
	/* 分区树中非叶子表的 RT 索引列表 */
	List	   *partitioned_rels;
	List	   *subpaths;		/* 组件 Path 的列表 */
	/* subpaths 中第一个 partial path 的索引；若没有则为 list_length(subpaths) */
	int			first_partial_path;
	double		limit_tuples;	/* 输出元组的硬限制，或为 -1 表示无限制 */
} AppendPath;

/* 判断是否为 dummy AppendPath（即 subpaths 为空） */
#define IS_DUMMY_APPEND(p) \
	(IsA((p), AppendPath) && ((AppendPath *) (p))->subpaths == NIL)

/*
 * A relation that's been proven empty will have one path that is dummy
 * (but might have projection paths on top).  For historical reasons,
 * this is provided as a macro that wraps is_dummy_rel().
 */
#define IS_DUMMY_REL(r) is_dummy_rel(r)
extern bool is_dummy_rel(RelOptInfo *rel);

/*
 * MergeAppendPath represents a MergeAppend plan, ie, the merging of sorted
 * results from several member plans to produce similarly-sorted output.
 */
typedef struct MergeAppendPath
{
	Path		path;
	/* RT indexes of non-leaf tables in a partition tree */
	List	   *partitioned_rels;
	List	   *subpaths;		/* list of component Paths */
	double		limit_tuples;	/* hard limit on output tuples, or -1 */
} MergeAppendPath;

/*
 * GroupResultPath represents use of a Result plan node to compute the
 * output of a degenerate GROUP BY case, wherein we know we should produce
 * exactly one row, which might then be filtered by a HAVING qual.
 *
 * Note that quals is a list of bare clauses, not RestrictInfos.
 */
typedef struct GroupResultPath
{
	Path		path;
	List	   *quals;
} GroupResultPath;

/*
 * MaterialPath represents use of a Material plan node, i.e., caching of
 * the output of its subpath.  This is used when the subpath is expensive
 * and needs to be scanned repeatedly, or when we need mark/restore ability
 * and the subpath doesn't have it.
 */
typedef struct MaterialPath
{
	Path		path;
	Path	   *subpath;
} MaterialPath;

/*
 * UniquePath represents elimination of distinct rows from the output of
 * its subpath.
 *
 * This can represent significantly different plans: either hash-based or
 * sort-based implementation, or a no-op if the input path can be proven
 * distinct already.  The decision is sufficiently localized that it's not
 * worth having separate Path node types.  (Note: in the no-op case, we could
 * eliminate the UniquePath node entirely and just return the subpath; but
 * it's convenient to have a UniquePath in the path tree to signal upper-level
 * routines that the input is known distinct.)
 */
typedef enum
{
	UNIQUE_PATH_NOOP,			/* input is known unique already */
	UNIQUE_PATH_HASH,			/* use hashing */
	UNIQUE_PATH_SORT			/* use sorting */
} UniquePathMethod;

typedef struct UniquePath
{
	Path		path;
	Path	   *subpath;
	UniquePathMethod umethod;
	List	   *in_operators;	/* equality operators of the IN clause */
	List	   *uniq_exprs;		/* expressions to be made unique */
} UniquePath;

/*
 * GatherPath runs several copies of a plan in parallel and collects the
 * results.  The parallel leader may also execute the plan, unless the
 * single_copy flag is set.
 */
typedef struct GatherPath
{
	Path		path;
	Path	   *subpath;		/* path for each worker */
	bool		single_copy;	/* don't execute path more than once */
	int			num_workers;	/* number of workers sought to help */
} GatherPath;

/*
 * GatherMergePath runs several copies of a plan in parallel and collects
 * the results, preserving their common sort order.
 */
typedef struct GatherMergePath
{
	Path		path;
	Path	   *subpath;		/* path for each worker */
	int			num_workers;	/* number of workers sought to help */
} GatherMergePath;


/*
 * All join-type paths share these fields.
 */

typedef struct JoinPath
{
	Path		path;

	JoinType	jointype;

	bool		inner_unique;	/* each outer tuple provably matches no more
								 * than one inner tuple */

	Path	   *outerjoinpath;	/* path for the outer side of the join */
	Path	   *innerjoinpath;	/* path for the inner side of the join */

	List	   *joinrestrictinfo;	/* RestrictInfos to apply to join */

	/*
	 * See the notes for RelOptInfo and ParamPathInfo to understand why
	 * joinrestrictinfo is needed in JoinPath, and can't be merged into the
	 * parent RelOptInfo.
	 */
} JoinPath;

/*
 * A nested-loop path needs no special fields.
 */

typedef JoinPath NestPath;

/*
 * A mergejoin path has these fields.
 *
 * Unlike other path types, a MergePath node doesn't represent just a single
 * run-time plan node: it can represent up to four.  Aside from the MergeJoin
 * node itself, there can be a Sort node for the outer input, a Sort node
 * for the inner input, and/or a Material node for the inner input.  We could
 * represent these nodes by separate path nodes, but considering how many
 * different merge paths are investigated during a complex join problem,
 * it seems better to avoid unnecessary palloc overhead.
 *
 * path_mergeclauses lists the clauses (in the form of RestrictInfos)
 * that will be used in the merge.
 *
 * Note that the mergeclauses are a subset of the parent relation's
 * restriction-clause list.  Any join clauses that are not mergejoinable
 * appear only in the parent's restrict list, and must be checked by a
 * qpqual at execution time.
 *
 * outersortkeys (resp. innersortkeys) is NIL if the outer path
 * (resp. inner path) is already ordered appropriately for the
 * mergejoin.  If it is not NIL then it is a PathKeys list describing
 * the ordering that must be created by an explicit Sort node.
 *
 * skip_mark_restore is true if the executor need not do mark/restore calls.
 * Mark/restore overhead is usually required, but can be skipped if we know
 * that the executor need find only one match per outer tuple, and that the
 * mergeclauses are sufficient to identify a match.  In such cases the
 * executor can immediately advance the outer relation after processing a
 * match, and therefore it need never back up the inner relation.
 *
 * materialize_inner is true if a Material node should be placed atop the
 * inner input.  This may appear with or without an inner Sort step.
 */

typedef struct MergePath
{
	JoinPath	jpath;
	List	   *path_mergeclauses;	/* join clauses to be used for merge */
	List	   *outersortkeys;	/* keys for explicit sort, if any */
	List	   *innersortkeys;	/* keys for explicit sort, if any */
	bool		skip_mark_restore;	/* can executor skip mark/restore? */
	bool		materialize_inner;	/* add Materialize to inner? */
} MergePath;

/*
 * A hashjoin path has these fields.
 *
 * The remarks above for mergeclauses apply for hashclauses as well.
 *
 * Hashjoin does not care what order its inputs appear in, so we have
 * no need for sortkeys.
 */

typedef struct HashPath
{
	JoinPath	jpath;
	List	   *path_hashclauses;	/* join clauses used for hashing */
	int			num_batches;	/* number of batches expected */
	double		inner_rows_total;	/* total inner rows expected */
} HashPath;

/*
 * ProjectionPath represents a projection (that is, targetlist computation)
 *
 * Nominally, this path node represents using a Result plan node to do a
 * projection step.  However, if the input plan node supports projection,
 * we can just modify its output targetlist to do the required calculations
 * directly, and not need a Result.  In some places in the planner we can just
 * jam the desired PathTarget into the input path node (and adjust its cost
 * accordingly), so we don't need a ProjectionPath.  But in other places
 * it's necessary to not modify the input path node, so we need a separate
 * ProjectionPath node, which is marked dummy to indicate that we intend to
 * assign the work to the input plan node.  The estimated cost for the
 * ProjectionPath node will account for whether a Result will be used or not.
 */
typedef struct ProjectionPath
{
	Path		path;
	Path	   *subpath;		/* path representing input source */
	bool		dummypp;		/* true if no separate Result is needed */
} ProjectionPath;

/*
 * ProjectSetPath represents evaluation of a targetlist that includes
 * set-returning function(s), which will need to be implemented by a
 * ProjectSet plan node.
 */
typedef struct ProjectSetPath
{
	Path		path;
	Path	   *subpath;		/* path representing input source */
} ProjectSetPath;

/*
 * SortPath represents an explicit sort step
 *
 * The sort keys are, by definition, the same as path.pathkeys.
 *
 * Note: the Sort plan node cannot project, so path.pathtarget must be the
 * same as the input's pathtarget.
 */
typedef struct SortPath
{
	Path		path;
	Path	   *subpath;		/* path representing input source */
} SortPath;

/*
 * GroupPath represents grouping (of presorted input)
 *
 * groupClause represents the columns to be grouped on; the input path
 * must be at least that well sorted.
 *
 * We can also apply a qual to the grouped rows (equivalent of HAVING)
 */
typedef struct GroupPath
{
	Path		path;
	Path	   *subpath;		/* path representing input source */
	List	   *groupClause;	/* a list of SortGroupClause's */
	List	   *qual;			/* quals (HAVING quals), if any */
} GroupPath;

/*
 * UpperUniquePath represents adjacent-duplicate removal (in presorted input)
 *
 * The columns to be compared are the first numkeys columns of the path's
 * pathkeys.  The input is presumed already sorted that way.
 */
typedef struct UpperUniquePath
{
	Path		path;
	Path	   *subpath;		/* path representing input source */
	int			numkeys;		/* number of pathkey columns to compare */
} UpperUniquePath;

/*
 * AggPath represents generic computation of aggregate functions
 *
 * This may involve plain grouping (but not grouping sets), using either
 * sorted or hashed grouping; for the AGG_SORTED case, the input must be
 * appropriately presorted.
 */
typedef struct AggPath
{
	Path		path;
	Path	   *subpath;		/* path representing input source */
	AggStrategy aggstrategy;	/* basic strategy, see nodes.h */
	AggSplit	aggsplit;		/* agg-splitting mode, see nodes.h */
	double		numGroups;		/* estimated number of groups in input */
	List	   *groupClause;	/* a list of SortGroupClause's */
	List	   *qual;			/* quals (HAVING quals), if any */
} AggPath;

/*
 * Various annotations used for grouping sets in the planner.
 */

typedef struct GroupingSetData
{
	NodeTag		type;
	List	   *set;			/* grouping set as list of sortgrouprefs */
	double		numGroups;		/* est. number of result groups */
} GroupingSetData;

typedef struct RollupData
{
	NodeTag		type;
	List	   *groupClause;	/* applicable subset of parse->groupClause */
	List	   *gsets;			/* lists of integer indexes into groupClause */
	List	   *gsets_data;		/* list of GroupingSetData */
	double		numGroups;		/* est. number of result groups */
	bool		hashable;		/* can be hashed */
	bool		is_hashed;		/* to be implemented as a hashagg */
} RollupData;

/*
 * GroupingSetsPath represents a GROUPING SETS aggregation
 */

typedef struct GroupingSetsPath
{
	Path		path;
	Path	   *subpath;		/* path representing input source */
	AggStrategy aggstrategy;	/* basic strategy */
	List	   *rollups;		/* list of RollupData */
	List	   *qual;			/* quals (HAVING quals), if any */
} GroupingSetsPath;

/*
 * MinMaxAggPath represents computation of MIN/MAX aggregates from indexes
 */
typedef struct MinMaxAggPath
{
	Path		path;
	List	   *mmaggregates;	/* list of MinMaxAggInfo */
	List	   *quals;			/* HAVING quals, if any */
} MinMaxAggPath;

/*
 * WindowAggPath represents generic computation of window functions
 */
typedef struct WindowAggPath
{
	Path		path;
	Path	   *subpath;		/* path representing input source */
	WindowClause *winclause;	/* WindowClause we'll be using */
} WindowAggPath;

/*
 * SetOpPath represents a set-operation, that is INTERSECT or EXCEPT
 */
typedef struct SetOpPath
{
	Path		path;
	Path	   *subpath;		/* path representing input source */
	SetOpCmd	cmd;			/* what to do, see nodes.h */
	SetOpStrategy strategy;		/* how to do it, see nodes.h */
	List	   *distinctList;	/* SortGroupClauses identifying target cols */
	AttrNumber	flagColIdx;		/* where is the flag column, if any */
	int			firstFlag;		/* flag value for first input relation */
	double		numGroups;		/* estimated number of groups in input */
} SetOpPath;

/*
 * RecursiveUnionPath represents a recursive UNION node
 */
typedef struct RecursiveUnionPath
{
	Path		path;
	Path	   *leftpath;		/* paths representing input sources */
	Path	   *rightpath;
	List	   *distinctList;	/* SortGroupClauses identifying target cols */
	int			wtParam;		/* ID of Param representing work table */
	double		numGroups;		/* estimated number of groups in input */
} RecursiveUnionPath;

/*
 * LockRowsPath represents acquiring row locks for SELECT FOR UPDATE/SHARE
 */
typedef struct LockRowsPath
{
	Path		path;
	Path	   *subpath;		/* path representing input source */
	List	   *rowMarks;		/* a list of PlanRowMark's */
	int			epqParam;		/* ID of Param for EvalPlanQual re-eval */
} LockRowsPath;

/*
 * ModifyTablePath represents performing INSERT/UPDATE/DELETE modifications
 *
 * We represent most things that will be in the ModifyTable plan node
 * literally, except we have child Path(s) not Plan(s).  But analysis of the
 * OnConflictExpr is deferred to createplan.c, as is collection of FDW data.
 */
typedef struct ModifyTablePath
{
	Path		path;
	CmdType		operation;		/* INSERT, UPDATE, or DELETE */
	bool		canSetTag;		/* do we set the command tag/es_processed? */
	Index		nominalRelation;	/* Parent RT index for use of EXPLAIN */
	Index		rootRelation;	/* Root RT index, if target is partitioned */
	bool		partColsUpdated;	/* some part key in hierarchy updated */
	List	   *resultRelations;	/* integer list of RT indexes */
	List	   *subpaths;		/* Path(s) producing source data */
	List	   *subroots;		/* per-target-table PlannerInfos */
	List	   *withCheckOptionLists;	/* per-target-table WCO lists */
	List	   *returningLists; /* per-target-table RETURNING tlists */
	List	   *rowMarks;		/* PlanRowMarks (non-locking only) */
	OnConflictExpr *onconflict; /* ON CONFLICT clause, or NULL */
	int			epqParam;		/* ID of Param for EvalPlanQual re-eval */
} ModifyTablePath;

/*
 * LimitPath represents applying LIMIT/OFFSET restrictions
 */
typedef struct LimitPath
{
	Path		path;
	Path	   *subpath;		/* path representing input source */
	Node	   *limitOffset;	/* OFFSET parameter, or NULL if none */
	Node	   *limitCount;		/* COUNT parameter, or NULL if none */
} LimitPath;


/*
 * Restriction clause info.
 *
 * We create one of these for each AND sub-clause of a restriction condition
 * (WHERE or JOIN/ON clause).  Since the restriction clauses are logically
 * ANDed, we can use any one of them or any subset of them to filter out
 * tuples, without having to evaluate the rest.  The RestrictInfo node itself
 * stores data used by the optimizer while choosing the best query plan.
 *
 * If a restriction clause references a single base relation, it will appear
 * in the baserestrictinfo list of the RelOptInfo for that base rel.
 *
 * If a restriction clause references more than one base rel, it will
 * appear in the joininfo list of every RelOptInfo that describes a strict
 * subset of the base rels mentioned in the clause.  The joininfo lists are
 * used to drive join tree building by selecting plausible join candidates.
 * The clause cannot actually be applied until we have built a join rel
 * containing all the base rels it references, however.
 *
 * When we construct a join rel that includes all the base rels referenced
 * in a multi-relation restriction clause, we place that clause into the
 * joinrestrictinfo lists of paths for the join rel, if neither left nor
 * right sub-path includes all base rels referenced in the clause.  The clause
 * will be applied at that join level, and will not propagate any further up
 * the join tree.  (Note: the "predicate migration" code was once intended to
 * push restriction clauses up and down the plan tree based on evaluation
 * costs, but it's dead code and is unlikely to be resurrected in the
 * foreseeable future.)
 *
 * Note that in the presence of more than two rels, a multi-rel restriction
 * might reach different heights in the join tree depending on the join
 * sequence we use.  So, these clauses cannot be associated directly with
 * the join RelOptInfo, but must be kept track of on a per-join-path basis.
 *
 * RestrictInfos that represent equivalence conditions (i.e., mergejoinable
 * equalities that are not outerjoin-delayed) are handled a bit differently.
 * Initially we attach them to the EquivalenceClasses that are derived from
 * them.  When we construct a scan or join path, we look through all the
 * EquivalenceClasses and generate derived RestrictInfos representing the
 * minimal set of conditions that need to be checked for this particular scan
 * or join to enforce that all members of each EquivalenceClass are in fact
 * equal in all rows emitted by the scan or join.
 *
 * When dealing with outer joins we have to be very careful about pushing qual
 * clauses up and down the tree.  An outer join's own JOIN/ON conditions must
 * be evaluated exactly at that join node, unless they are "degenerate"
 * conditions that reference only Vars from the nullable side of the join.
 * Quals appearing in WHERE or in a JOIN above the outer join cannot be pushed
 * down below the outer join, if they reference any nullable Vars.
 * RestrictInfo nodes contain a flag to indicate whether a qual has been
 * pushed down to a lower level than its original syntactic placement in the
 * join tree would suggest.  If an outer join prevents us from pushing a qual
 * down to its "natural" semantic level (the level associated with just the
 * base rels used in the qual) then we mark the qual with a "required_relids"
 * value including more than just the base rels it actually uses.  By
 * pretending that the qual references all the rels required to form the outer
 * join, we prevent it from being evaluated below the outer join's joinrel.
 * When we do form the outer join's joinrel, we still need to distinguish
 * those quals that are actually in that join's JOIN/ON condition from those
 * that appeared elsewhere in the tree and were pushed down to the join rel
 * because they used no other rels.  That's what the is_pushed_down flag is
 * for; it tells us that a qual is not an OUTER JOIN qual for the set of base
 * rels listed in required_relids.  A clause that originally came from WHERE
 * or an INNER JOIN condition will *always* have its is_pushed_down flag set.
 * It's possible for an OUTER JOIN clause to be marked is_pushed_down too,
 * if we decide that it can be pushed down into the nullable side of the join.
 * In that case it acts as a plain filter qual for wherever it gets evaluated.
 * (In short, is_pushed_down is only false for non-degenerate outer join
 * conditions.  Possibly we should rename it to reflect that meaning?  But
 * see also the comments for RINFO_IS_PUSHED_DOWN, below.)
 *
 * RestrictInfo nodes also contain an outerjoin_delayed flag, which is true
 * if the clause's applicability must be delayed due to any outer joins
 * appearing below it (ie, it has to be postponed to some join level higher
 * than the set of relations it actually references).
 *
 * There is also an outer_relids field, which is NULL except for outer join
 * clauses; for those, it is the set of relids on the outer side of the
 * clause's outer join.  (These are rels that the clause cannot be applied to
 * in parameterized scans, since pushing it into the join's outer side would
 * lead to wrong answers.)
 *
 * There is also a nullable_relids field, which is the set of rels the clause
 * references that can be forced null by some outer join below the clause.
 *
 * outerjoin_delayed = true is subtly different from nullable_relids != NULL:
 * a clause might reference some nullable rels and yet not be
 * outerjoin_delayed because it also references all the other rels of the
 * outer join(s). A clause that is not outerjoin_delayed can be enforced
 * anywhere it is computable.
 *
 * To handle security-barrier conditions efficiently, we mark RestrictInfo
 * nodes with a security_level field, in which higher values identify clauses
 * coming from less-trusted sources.  The exact semantics are that a clause
 * cannot be evaluated before another clause with a lower security_level value
 * unless the first clause is leakproof.  As with outer-join clauses, this
 * creates a reason for clauses to sometimes need to be evaluated higher in
 * the join tree than their contents would suggest; and even at a single plan
 * node, this rule constrains the order of application of clauses.
 *
 * In general, the referenced clause might be arbitrarily complex.  The
 * kinds of clauses we can handle as indexscan quals, mergejoin clauses,
 * or hashjoin clauses are limited (e.g., no volatile functions).  The code
 * for each kind of path is responsible for identifying the restrict clauses
 * it can use and ignoring the rest.  Clauses not implemented by an indexscan,
 * mergejoin, or hashjoin will be placed in the plan qual or joinqual field
 * of the finished Plan node, where they will be enforced by general-purpose
 * qual-expression-evaluation code.  (But we are still entitled to count
 * their selectivity when estimating the result tuple count, if we
 * can guess what it is...)
 *
 * When the referenced clause is an OR clause, we generate a modified copy
 * in which additional RestrictInfo nodes are inserted below the top-level
 * OR/AND structure.  This is a convenience for OR indexscan processing:
 * indexquals taken from either the top level or an OR subclause will have
 * associated RestrictInfo nodes.
 *
 * The can_join flag is set true if the clause looks potentially useful as
 * a merge or hash join clause, that is if it is a binary opclause with
 * nonoverlapping sets of relids referenced in the left and right sides.
 * (Whether the operator is actually merge or hash joinable isn't checked,
 * however.)
 *
 * The pseudoconstant flag is set true if the clause contains no Vars of
 * the current query level and no volatile functions.  Such a clause can be
 * pulled out and used as a one-time qual in a gating Result node.  We keep
 * pseudoconstant clauses in the same lists as other RestrictInfos so that
 * the regular clause-pushing machinery can assign them to the correct join
 * level, but they need to be treated specially for cost and selectivity
 * estimates.  Note that a pseudoconstant clause can never be an indexqual
 * or merge or hash join clause, so it's of no interest to large parts of
 * the planner.
 *
 * When join clauses are generated from EquivalenceClasses, there may be
 * several equally valid ways to enforce join equivalence, of which we need
 * apply only one.  We mark clauses of this kind by setting parent_ec to
 * point to the generating EquivalenceClass.  Multiple clauses with the same
 * parent_ec in the same join are redundant.
 */

typedef struct RestrictInfo
{
	NodeTag		type;

	Expr	   *clause;			/* the represented clause of WHERE or JOIN */

	bool		is_pushed_down; /* true if clause was pushed down in level */

	bool		outerjoin_delayed;	/* true if delayed by lower outer join */

	bool		can_join;		/* see comment above */

	bool		pseudoconstant; /* see comment above */

	bool		leakproof;		/* true if known to contain no leaked Vars */

	Index		security_level; /* see comment above */

	/* The set of relids (varnos) actually referenced in the clause: */
	Relids		clause_relids;

	/* The set of relids required to evaluate the clause: */
	Relids		required_relids;

	/* If an outer-join clause, the outer-side relations, else NULL: */
	Relids		outer_relids;

	/* The relids used in the clause that are nullable by lower outer joins: */
	Relids		nullable_relids;

	/* These fields are set for any binary opclause: */
	Relids		left_relids;	/* relids in left side of clause */
	Relids		right_relids;	/* relids in right side of clause */

	/* This field is NULL unless clause is an OR clause: */
	Expr	   *orclause;		/* modified clause with RestrictInfos */

	/* This field is NULL unless clause is potentially redundant: */
	EquivalenceClass *parent_ec;	/* generating EquivalenceClass */

	/* cache space for cost and selectivity */
	QualCost	eval_cost;		/* eval cost of clause; -1 if not yet set */
	Selectivity norm_selec;		/* selectivity for "normal" (JOIN_INNER)
								 * semantics; -1 if not yet set; >1 means a
								 * redundant clause */
	Selectivity outer_selec;	/* selectivity for outer join semantics; -1 if
								 * not yet set */

	/* valid if clause is mergejoinable, else NIL */
	List	   *mergeopfamilies;	/* opfamilies containing clause operator */

	/* cache space for mergeclause processing; NULL if not yet set */
	EquivalenceClass *left_ec;	/* EquivalenceClass containing lefthand */
	EquivalenceClass *right_ec; /* EquivalenceClass containing righthand */
	EquivalenceMember *left_em; /* EquivalenceMember for lefthand */
	EquivalenceMember *right_em;	/* EquivalenceMember for righthand */
	List	   *scansel_cache;	/* list of MergeScanSelCache structs */

	/* transient workspace for use while considering a specific join path */
	bool		outer_is_left;	/* T = outer var on left, F = on right */

	/* valid if clause is hashjoinable, else InvalidOid: */
	Oid			hashjoinoperator;	/* copy of clause operator */

	/* cache space for hashclause processing; -1 if not yet set */
	Selectivity left_bucketsize;	/* avg bucketsize of left side */
	Selectivity right_bucketsize;	/* avg bucketsize of right side */
	Selectivity left_mcvfreq;	/* left side's most common val's freq */
	Selectivity right_mcvfreq;	/* right side's most common val's freq */
} RestrictInfo;

/*
 * This macro embodies the correct way to test whether a RestrictInfo is
 * "pushed down" to a given outer join, that is, should be treated as a filter
 * clause rather than a join clause at that outer join.  This is certainly so
 * if is_pushed_down is true; but examining that is not sufficient anymore,
 * because outer-join clauses will get pushed down to lower outer joins when
 * we generate a path for the lower outer join that is parameterized by the
 * LHS of the upper one.  We can detect such a clause by noting that its
 * required_relids exceed the scope of the join.
 */
#define RINFO_IS_PUSHED_DOWN(rinfo, joinrelids) \
	((rinfo)->is_pushed_down || \
	 !bms_is_subset((rinfo)->required_relids, joinrelids))

/*
 * Since mergejoinscansel() is a relatively expensive function, and would
 * otherwise be invoked many times while planning a large join tree,
 * we go out of our way to cache its results.  Each mergejoinable
 * RestrictInfo carries a list of the specific sort orderings that have
 * been considered for use with it, and the resulting selectivities.
 */
typedef struct MergeScanSelCache
{
	/* Ordering details (cache lookup key) */
	Oid			opfamily;		/* btree opfamily defining the ordering */
	Oid			collation;		/* collation for the ordering */
	int			strategy;		/* sort direction (ASC or DESC) */
	bool		nulls_first;	/* do NULLs come before normal values? */
	/* Results */
	Selectivity leftstartsel;	/* first-join fraction for clause left side */
	Selectivity leftendsel;		/* last-join fraction for clause left side */
	Selectivity rightstartsel;	/* first-join fraction for clause right side */
	Selectivity rightendsel;	/* last-join fraction for clause right side */
} MergeScanSelCache;

/*
 * Placeholder node for an expression to be evaluated below the top level
 * of a plan tree.  This is used during planning to represent the contained
 * expression.  At the end of the planning process it is replaced by either
 * the contained expression or a Var referring to a lower-level evaluation of
 * the contained expression.  Typically the evaluation occurs below an outer
 * join, and Var references above the outer join might thereby yield NULL
 * instead of the expression value.
 *
 * Although the planner treats this as an expression node type, it is not
 * recognized by the parser or executor, so we declare it here rather than
 * in primnodes.h.
 */

typedef struct PlaceHolderVar
{
	Expr		xpr;
	Expr	   *phexpr;			/* the represented expression */
	Relids		phrels;			/* base relids syntactically within expr src */
	Index		phid;			/* ID for PHV (unique within planner run) */
	Index		phlevelsup;		/* > 0 if PHV belongs to outer query */
} PlaceHolderVar;

/*
 * "特殊连接"信息。
 *
 * 单边外连接（one-sided outer joins）部分但不完全约束连接顺序。
 * 我们将这些连接展平成 planner 的顶层关系列表进行连接，但会在
 * SpecialJoinInfo 结构体中记录每个外连接的信息。这些结构体存储在
 * PlannerInfo 节点的 join_info_list 中。
 *
 * 类似地，通过展平 IN (subselect) 和 EXISTS(subselect) 子句创建的
 * 半连接（semijoin）和反连接（antijoin）也会对连接顺序产生部分约束。
 * 这些约束同样记录在 SpecialJoinInfo 结构体中。
 *
 * 即使 FULL JOIN 没有灵活的规划方式，我们也会为其创建 SpecialJoinInfo，
 * 因为这简化了 make_join_rel() 的 API。
 *
 * min_lefthand 和 min_righthand 分别是执行特殊连接时每一侧必须可用的
 * 基表 relids 集合。lhs_strict 为真表示特殊连接条件在 LHS 变量全为 NULL 时
 * 不可能成立（这意味着外连接即使出现在上层 RHS，也可以与上层外连接交换）。
 * 但对于 FULL JOIN，我们不会设置 lhs_strict。
 *
 * min_lefthand 和 min_righthand 都不能为空集，否则会破坏强制连接顺序的逻辑。
 *
 * syn_lefthand 和 syn_righthand 是在该特殊连接之下语法层面的基表 relids 集合。
 * （这些用于帮助计算更高层连接的 min_lefthand 和 min_righthand。）
 *
 * delay_upper_joins 若为真，表示检测到一个下推子句必须在该连接形成后才能评估
 * （因为它引用了 RHS）。任何有此类子句且该连接在其 RHS 的外连接都不能与该连接交换，
 * 否则会导致无法检查下推子句。（对于 FULL JOIN，我们也不跟踪此项。）
 *
 * 对于半连接（semijoin），我们还会提取连接操作符及其 RHS 参数，并设置
 * semi_operators、semi_rhs_exprs、semi_can_btree 和 semi_can_hash。
 * 这是为了支持对 RHS 唯一化（unique-ifying），所以只有当 semi_can_btree 或
 * semi_can_hash 至少有一个为真时才会设置这些信息。（你可能认为这些信息应在连接规划时
 * 计算，但在参数化表扫描规划期间能用到这些信息，因此我们将其存储在 SpecialJoinInfo 中。）
 *
 * jointype 永远不会是 JOIN_RIGHT；RIGHT JOIN 会通过交换输入变为 LEFT JOIN。
 * 因此 join_info_list 成员中 jointype 只允许为 LEFT、FULL、SEMI 或 ANTI。
 *
 * 为了连接选择性估算，我们会为常规内连接创建临时的 SpecialJoinInfo 结构体；
 * 所以 jointype == JOIN_INNER 在这种结构体中是可能的，尽管在 join_info_list 中不允许。
 * 我们也会为外连接创建 jointype == JOIN_INNER 的临时 SpecialJoinInfo，因为在成本估算时
 * 有时需要知道纯内连接语义下的连接大小。注意 lhs_strict、delay_upper_joins 以及
 * semi_xxx 字段在这种结构体中没有实际意义。
 */
#ifndef HAVE_SPECIALJOININFO_TYPEDEF
typedef struct SpecialJoinInfo SpecialJoinInfo;
#define HAVE_SPECIALJOININFO_TYPEDEF 1
#endif

struct SpecialJoinInfo
{
	NodeTag		type;
	Relids		min_lefthand;	/* 连接时左侧必须包含的基表 relids */
	Relids		min_righthand;	/* 连接时右侧必须包含的基表 relids */
	Relids		syn_lefthand;	/* 语法层面左侧包含的基表 relids */
	Relids		syn_righthand;	/* 语法层面右侧包含的基表 relids */
	JoinType	jointype;		/* 仅为 INNER、LEFT、FULL、SEMI 或 ANTI */
	bool		lhs_strict;		/* 连接条件对某些 LHS rel 是严格的 */
	bool		delay_upper_joins;	/* 不能与上层 RHS 交换连接顺序 */

	/* 以下字段仅对 JOIN_SEMI 类型设置： */
	bool		semi_can_btree; /* 若 semi_operators 全为 btree 操作符则为真 */
	bool		semi_can_hash;	/* 若 semi_operators 全为 hash 操作符则为真 */
	List	   *semi_operators; /* 等值连接操作符的 OID 列表 */
	List	   *semi_rhs_exprs; /* 这些操作符的右侧表达式列表 */
};

/*
 * Append-relation info.
 *
 * When we expand an inheritable table or a UNION-ALL subselect into an
 * "append relation" (essentially, a list of child RTEs), we build an
 * AppendRelInfo for each child RTE.  The list of AppendRelInfos indicates
 * which child RTEs must be included when expanding the parent, and each node
 * carries information needed to translate Vars referencing the parent into
 * Vars referencing that child.
 *
 * These structs are kept in the PlannerInfo node's append_rel_list.
 * Note that we just throw all the structs into one list, and scan the
 * whole list when desiring to expand any one parent.  We could have used
 * a more complex data structure (eg, one list per parent), but this would
 * be harder to update during operations such as pulling up subqueries,
 * and not really any easier to scan.  Considering that typical queries
 * will not have many different append parents, it doesn't seem worthwhile
 * to complicate things.
 *
 * Note: after completion of the planner prep phase, any given RTE is an
 * append parent having entries in append_rel_list if and only if its
 * "inh" flag is set.  We clear "inh" for plain tables that turn out not
 * to have inheritance children, and (in an abuse of the original meaning
 * of the flag) we set "inh" for subquery RTEs that turn out to be
 * flattenable UNION ALL queries.  This lets us avoid useless searches
 * of append_rel_list.
 *
 * Note: the data structure assumes that append-rel members are single
 * baserels.  This is OK for inheritance, but it prevents us from pulling
 * up a UNION ALL member subquery if it contains a join.  While that could
 * be fixed with a more complex data structure, at present there's not much
 * point because no improvement in the plan could result.
 */

typedef struct AppendRelInfo
{
	NodeTag		type;

	/*
	 * These fields uniquely identify this append relationship.  There can be
	 * (in fact, always should be) multiple AppendRelInfos for the same
	 * parent_relid, but never more than one per child_relid, since a given
	 * RTE cannot be a child of more than one append parent.
	 */
	Index		parent_relid;	/* RT index of append parent rel */
	Index		child_relid;	/* RT index of append child rel */

	/*
	 * For an inheritance appendrel, the parent and child are both regular
	 * relations, and we store their rowtype OIDs here for use in translating
	 * whole-row Vars.  For a UNION-ALL appendrel, the parent and child are
	 * both subqueries with no named rowtype, and we store InvalidOid here.
	 */
	Oid			parent_reltype; /* OID of parent's composite type */
	Oid			child_reltype;	/* OID of child's composite type */

	/*
	 * The N'th element of this list is a Var or expression representing the
	 * child column corresponding to the N'th column of the parent. This is
	 * used to translate Vars referencing the parent rel into references to
	 * the child.  A list element is NULL if it corresponds to a dropped
	 * column of the parent (this is only possible for inheritance cases, not
	 * UNION ALL).  The list elements are always simple Vars for inheritance
	 * cases, but can be arbitrary expressions in UNION ALL cases.
	 *
	 * Notice we only store entries for user columns (attno > 0).  Whole-row
	 * Vars are special-cased, and system columns (attno < 0) need no special
	 * translation since their attnos are the same for all tables.
	 *
	 * Caution: the Vars have varlevelsup = 0.  Be careful to adjust as needed
	 * when copying into a subquery.
	 */
	List	   *translated_vars;	/* Expressions in the child's Vars */

	/*
	 * We store the parent table's OID here for inheritance, or InvalidOid for
	 * UNION ALL.  This is only needed to help in generating error messages if
	 * an attempt is made to reference a dropped parent column.
	 */
	Oid			parent_reloid;	/* OID of parent relation */
} AppendRelInfo;

/*
 * For each distinct placeholder expression generated during planning, we
 * store a PlaceHolderInfo node in the PlannerInfo node's placeholder_list.
 * This stores info that is needed centrally rather than in each copy of the
 * PlaceHolderVar.  The phid fields identify which PlaceHolderInfo goes with
 * each PlaceHolderVar.  Note that phid is unique throughout a planner run,
 * not just within a query level --- this is so that we need not reassign ID's
 * when pulling a subquery into its parent.
 *
 * The idea is to evaluate the expression at (only) the ph_eval_at join level,
 * then allow it to bubble up like a Var until the ph_needed join level.
 * ph_needed has the same definition as attr_needed for a regular Var.
 *
 * The PlaceHolderVar's expression might contain LATERAL references to vars
 * coming from outside its syntactic scope.  If so, those rels are *not*
 * included in ph_eval_at, but they are recorded in ph_lateral.
 *
 * Notice that when ph_eval_at is a join rather than a single baserel, the
 * PlaceHolderInfo may create constraints on join order: the ph_eval_at join
 * has to be formed below any outer joins that should null the PlaceHolderVar.
 *
 * We create a PlaceHolderInfo only after determining that the PlaceHolderVar
 * is actually referenced in the plan tree, so that unreferenced placeholders
 * don't result in unnecessary constraints on join order.
 */

typedef struct PlaceHolderInfo
{
	NodeTag		type;

	Index		phid;			/* ID for PH (unique within planner run) */
	PlaceHolderVar *ph_var;		/* copy of PlaceHolderVar tree */
	Relids		ph_eval_at;		/* lowest level we can evaluate value at */
	Relids		ph_lateral;		/* relids of contained lateral refs, if any */
	Relids		ph_needed;		/* highest level the value is needed at */
	int32		ph_width;		/* estimated attribute width */
} PlaceHolderInfo;

/*
 * This struct describes one potentially index-optimizable MIN/MAX aggregate
 * function.  MinMaxAggPath contains a list of these, and if we accept that
 * path, the list is stored into root->minmax_aggs for use during setrefs.c.
 */
typedef struct MinMaxAggInfo
{
	NodeTag		type;

	Oid			aggfnoid;		/* pg_proc Oid of the aggregate */
	Oid			aggsortop;		/* Oid of its sort operator */
	Expr	   *target;			/* expression we are aggregating on */
	PlannerInfo *subroot;		/* modified "root" for planning the subquery */
	Path	   *path;			/* access path for subquery */
	Cost		pathcost;		/* estimated cost to fetch first row */
	Param	   *param;			/* param for subplan's output */
} MinMaxAggInfo;

/*
 * At runtime, PARAM_EXEC slots are used to pass values around from one plan
 * node to another.  They can be used to pass values down into subqueries (for
 * outer references in subqueries), or up out of subqueries (for the results
 * of a subplan), or from a NestLoop plan node into its inner relation (when
 * the inner scan is parameterized with values from the outer relation).
 * The planner is responsible for assigning nonconflicting PARAM_EXEC IDs to
 * the PARAM_EXEC Params it generates.
 *
 * Outer references are managed via root->plan_params, which is a list of
 * PlannerParamItems.  While planning a subquery, each parent query level's
 * plan_params contains the values required from it by the current subquery.
 * During create_plan(), we use plan_params to track values that must be
 * passed from outer to inner sides of NestLoop plan nodes.
 *
 * The item a PlannerParamItem represents can be one of three kinds:
 *
 * A Var: the slot represents a variable of this level that must be passed
 * down because subqueries have outer references to it, or must be passed
 * from a NestLoop node to its inner scan.  The varlevelsup value in the Var
 * will always be zero.
 *
 * A PlaceHolderVar: this works much like the Var case, except that the
 * entry is a PlaceHolderVar node with a contained expression.  The PHV
 * will have phlevelsup = 0, and the contained expression is adjusted
 * to match in level.
 *
 * An Aggref (with an expression tree representing its argument): the slot
 * represents an aggregate expression that is an outer reference for some
 * subquery.  The Aggref itself has agglevelsup = 0, and its argument tree
 * is adjusted to match in level.
 *
 * Note: we detect duplicate Var and PlaceHolderVar parameters and coalesce
 * them into one slot, but we do not bother to do that for Aggrefs.
 * The scope of duplicate-elimination only extends across the set of
 * parameters passed from one query level into a single subquery, or for
 * nestloop parameters across the set of nestloop parameters used in a single
 * query level.  So there is no possibility of a PARAM_EXEC slot being used
 * for conflicting purposes.
 *
 * In addition, PARAM_EXEC slots are assigned for Params representing outputs
 * from subplans (values that are setParam items for those subplans).  These
 * IDs need not be tracked via PlannerParamItems, since we do not need any
 * duplicate-elimination nor later processing of the represented expressions.
 * Instead, we just record the assignment of the slot number by appending to
 * root->glob->paramExecTypes.
 */
typedef struct PlannerParamItem
{
	NodeTag		type;

	Node	   *item;			/* the Var, PlaceHolderVar, or Aggref */
	int			paramId;		/* its assigned PARAM_EXEC slot number */
} PlannerParamItem;

/*
 * When making cost estimates for a SEMI/ANTI/inner_unique join, there are
 * some correction factors that are needed in both nestloop and hash joins
 * to account for the fact that the executor can stop scanning inner rows
 * as soon as it finds a match to the current outer row.  These numbers
 * depend only on the selected outer and inner join relations, not on the
 * particular paths used for them, so it's worthwhile to calculate them
 * just once per relation pair not once per considered path.  This struct
 * is filled by compute_semi_anti_join_factors and must be passed along
 * to the join cost estimation functions.
 *
 * outer_match_frac is the fraction of the outer tuples that are
 *		expected to have at least one match.
 * match_count is the average number of matches expected for
 *		outer tuples that have at least one match.
 */
typedef struct SemiAntiJoinFactors
{
	Selectivity outer_match_frac;
	Selectivity match_count;
} SemiAntiJoinFactors;

/*
 * Struct for extra information passed to subroutines of add_paths_to_joinrel
 *
 * restrictlist contains all of the RestrictInfo nodes for restriction
 *		clauses that apply to this join
 * mergeclause_list is a list of RestrictInfo nodes for available
 *		mergejoin clauses in this join
 * inner_unique is true if each outer tuple provably matches no more
 *		than one inner tuple
 * sjinfo is extra info about special joins for selectivity estimation
 * semifactors is as shown above (only valid for SEMI/ANTI/inner_unique joins)
 * param_source_rels are OK targets for parameterization of result paths
 */
typedef struct JoinPathExtraData
{
	List	   *restrictlist;
	List	   *mergeclause_list;
	bool		inner_unique;
	SpecialJoinInfo *sjinfo;
	SemiAntiJoinFactors semifactors;
	Relids		param_source_rels;
} JoinPathExtraData;

/*
 * Various flags indicating what kinds of grouping are possible.
 *
 * GROUPING_CAN_USE_SORT should be set if it's possible to perform
 * sort-based implementations of grouping.  When grouping sets are in use,
 * this will be true if sorting is potentially usable for any of the grouping
 * sets, even if it's not usable for all of them.
 *
 * GROUPING_CAN_USE_HASH should be set if it's possible to perform
 * hash-based implementations of grouping.
 *
 * GROUPING_CAN_PARTIAL_AGG should be set if the aggregation is of a type
 * for which we support partial aggregation (not, for example, grouping sets).
 * It says nothing about parallel-safety or the availability of suitable paths.
 */
#define GROUPING_CAN_USE_SORT       0x0001
#define GROUPING_CAN_USE_HASH       0x0002
#define GROUPING_CAN_PARTIAL_AGG	0x0004

/*
 * What kind of partitionwise aggregation is in use?
 *
 * PARTITIONWISE_AGGREGATE_NONE: Not used.
 *
 * PARTITIONWISE_AGGREGATE_FULL: Aggregate each partition separately, and
 * append the results.
 *
 * PARTITIONWISE_AGGREGATE_PARTIAL: Partially aggregate each partition
 * separately, append the results, and then finalize aggregation.
 */
typedef enum
{
	PARTITIONWISE_AGGREGATE_NONE,
	PARTITIONWISE_AGGREGATE_FULL,
	PARTITIONWISE_AGGREGATE_PARTIAL
} PartitionwiseAggregateType;

/*
 * Struct for extra information passed to subroutines of create_grouping_paths
 *
 * flags indicating what kinds of grouping are possible.
 * partial_costs_set is true if the agg_partial_costs and agg_final_costs
 * 		have been initialized.
 * agg_partial_costs gives partial aggregation costs.
 * agg_final_costs gives finalization costs.
 * target_parallel_safe is true if target is parallel safe.
 * havingQual gives list of quals to be applied after aggregation.
 * targetList gives list of columns to be projected.
 * patype is the type of partitionwise aggregation that is being performed.
 */
typedef struct
{
	/* Data which remains constant once set. */
	int			flags;
	bool		partial_costs_set;
	AggClauseCosts agg_partial_costs;
	AggClauseCosts agg_final_costs;

	/* Data which may differ across partitions. */
	bool		target_parallel_safe;
	Node	   *havingQual;
	List	   *targetList;
	PartitionwiseAggregateType patype;
} GroupPathExtraData;

/*
 * Struct for extra information passed to subroutines of grouping_planner
 *
 * limit_needed is true if we actually need a Limit plan node.
 * limit_tuples is an estimated bound on the number of output tuples,
 *		or -1 if no LIMIT or couldn't estimate.
 * count_est and offset_est are the estimated values of the LIMIT and OFFSET
 * 		expressions computed by preprocess_limit() (see comments for
 * 		preprocess_limit() for more information).
 */
typedef struct
{
	bool		limit_needed;
	double		limit_tuples;
	int64		count_est;
	int64		offset_est;
} FinalPathExtraData;

/*
 * For speed reasons, cost estimation for join paths is performed in two
 * phases: the first phase tries to quickly derive a lower bound for the
 * join cost, and then we check if that's sufficient to reject the path.
 * If not, we come back for a more refined cost estimate.  The first phase
 * fills a JoinCostWorkspace struct with its preliminary cost estimates
 * and possibly additional intermediate values.  The second phase takes
 * these values as inputs to avoid repeating work.
 *
 * (Ideally we'd declare this in cost.h, but it's also needed in pathnode.h,
 * so seems best to put it here.)
 */
typedef struct JoinCostWorkspace
{
	/* Preliminary cost estimates --- must not be larger than final ones! */
	Cost		startup_cost;	/* cost expended before fetching any tuples */
	Cost		total_cost;		/* total cost (assuming all tuples fetched) */

	/* Fields below here should be treated as private to costsize.c */
	Cost		run_cost;		/* non-startup cost components */

	/* private for cost_nestloop code */
	Cost		inner_run_cost; /* also used by cost_mergejoin code */
	Cost		inner_rescan_run_cost;

	/* private for cost_mergejoin code */
	double		outer_rows;
	double		inner_rows;
	double		outer_skip_rows;
	double		inner_skip_rows;

	/* private for cost_hashjoin code */
	int			numbuckets;
	int			numbatches;
	double		inner_rows_total;
} JoinCostWorkspace;

#endif							/* PATHNODES_H */
