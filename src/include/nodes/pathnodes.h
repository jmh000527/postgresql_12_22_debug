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
 * 在寻找“最优路径”时，此枚举指定我们关注的是启动成本还是总成本。
 */
typedef enum CostSelector
{
	STARTUP_COST,	/* 启动成本 */
	TOTAL_COST		/* 总成本 */
} CostSelector;

/*
 * cost_qual_eval() 产生的成本估算包括一次性（启动）成本和每元组成本。
 */
typedef struct QualCost
{
	Cost		startup;		/* 一次性启动成本 */
	Cost		per_tuple;		/* 每元组评估成本 */
} QualCost;

/*
 * 聚合函数执行的成本估算需要以下关于待执行聚合的信息。
 * 注意，成本包括聚合参数表达式的执行成本以及聚合函数本身的成本。
 * 所有字段必须定义为可用 memset 初始化为零。
 */
typedef struct AggClauseCosts
{
	int			numAggs;			/* 聚合函数总数 */
	int			numOrderedAggs; 	/* 包含 DISTINCT/ORDER BY/WITHIN GROUP 的聚合数 */
	bool		hasNonPartial;		/* 是否存在不支持部分聚合模式的聚合 */
	bool		hasNonSerial;		/* 是否存在部分聚合不可序列化 */
	QualCost	transCost;			/* 每输入行的总执行成本 */
	QualCost	finalCost;			/* 每聚合行的总执行成本 */
	Size		transitionSpace;	/* 按引用传递的中间数据空间大小 */
} AggClauseCosts;

/*
 * UpperRelationKind 枚举用于标识在规划过程中可能处理的不同类型的“上层”（扫描/连接之后）关系。
 */
typedef enum UpperRelationKind
{
	UPPERREL_SETOP,				/* UNION/INTERSECT/EXCEPT 操作的结果（如有） */
	UPPERREL_PARTIAL_GROUP_AGG, /* 部分分组/聚合的结果（如有） */
	UPPERREL_GROUP_AGG,			/* 分组/聚合的结果（如有） */
	UPPERREL_WINDOW,			/* 窗口函数的结果（如有） */
	UPPERREL_DISTINCT,			/* SELECT DISTINCT 的结果（如有） */
	UPPERREL_ORDERED,			/* ORDER BY 的结果（如有） */
	UPPERREL_FINAL				/* 任何剩余顶层操作的结果 */
	/* 注意：UPPERREL_FINAL 必须是最后一个枚举项，用于确定相关数组的大小 */
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
 *		规划/优化的全局信息
 *
 * PlannerGlobal 保存一次 planner 调用的全局状态；
 * 该状态在整个命令的所有子查询层级间共享。
 *----------
 */
typedef struct PlannerGlobal
{
	NodeTag		type;

	ParamListInfo boundParams;				/* planner() 提供的参数值 */

	List	   *subplans;					/* SubPlan 节点的计划列表 */

	List	   *subroots;					/* SubPlan 节点的 PlannerInfo 列表 */

	Bitmapset  *rewindPlanIDs;				/* 需要 REWIND 的 subplan 索引集合 */

	List	   *finalrtable;				/* executor 用的“扁平化” rangetable */

	List	   *finalrowmarks;				/* “扁平化”的 PlanRowMarks 列表 */

	List	   *resultRelations;			/* “扁平化”的结果关系 RT 索引列表 */

	List	   *rootResultRelations;		/* “扁平化”的根结果关系 RT 索引列表 */

	List	   *relationOids;				/* 计划依赖的关系 OID 列表 */

	List	   *invalItems;					/* 其他依赖项，PlanInvalItems 列表 */

	List	   *paramExecTypes; 			/* PARAM_EXEC 类型的 OID 列表 */

	Index		lastPHId;					/* 已分配的最大 PlaceHolderVar ID */

	Index		lastRowMarkId;				/* 已分配的最大 PlanRowMark ID */

	int			lastPlanNodeId; 			/* 已分配的最大计划节点 ID */

	bool		transientPlan;				/* TransactionXmin 变化时是否重做计划？ */

	bool		dependsOnRole;				/* 计划是否依赖当前角色？ */

	bool		parallelModeOK; 			/* 是否可能使用并行模式？ */

	bool		parallelModeNeeded; 		/* 是否实际需要并行模式？ */

	char		maxParallelHazard;			/* 最严重的 PROPARALLEL hazard 等级 */

	PartitionDirectory partition_directory; /* 分区描述符目录 */
} PlannerGlobal;

/* 获取 SubPlan 节点关联的 Plan 的宏
 * 通过 subplan 的 plan_id 从 root->glob->subplans 列表中获取对应的 Plan 指针
 * 注意 plan_id 从 1 开始，因此需要减 1 作为索引
 */
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
/*
 * IndexOptInfo
 *		规划/优化阶段每个索引的信息
 *
 *		indexkeys[], indexcollations[] 数组长度为 ncolumns。
 *		opfamily[], opcintype[] 数组长度为 nkeycolumns。它们不包含包含列（included attributes）的信息。
 *
 *		sortopfamily[], reverse_sort[], nulls_first[] 数组长度为 nkeycolumns（若索引有序）；若无序则为 NULL。
 *
 *		indexkeys[] 数组中的 0 表示该索引列为表达式；每个此类列在 indexprs 中有一个元素。
 *
 *		对于有序索引，reverse_sort[] 和 nulls_first[] 描述正向索引扫描的排序方式；反向扫描则顺序相反。
 *
 *		indexprs 和 indpred 表达式已通过 prepqual.c 和 eval_const_expressions() 处理，便于与 WHERE 子句匹配。indpred 为隐式 AND 形式。
 *
 *		indextlist 是 TargetEntry 列表，表示索引列。对于简单列，提供等价的基表 Var；对于表达式列，链接到 indexprs 中的对应元素。
 *
 *		大部分字段在创建 IndexOptInfo 时填充（由 plancat.c 完成），indrestrictinfo 和 predOK 在 check_index_predicates() 中设置。
 */
struct IndexOptInfo
{
	NodeTag		type;

	Oid			indexoid;				/* 索引关系的 OID */
	Oid			reltablespace;			/* 索引所在表空间（不是表的表空间） */
	RelOptInfo *rel;					/* 回链到索引所属的表 */

	/* 索引大小统计信息（来自 pg_class 等） */
	BlockNumber pages;					/* 索引的磁盘页数 */
	double		tuples;					/* 索引中的元组数 */
	int			tree_height;			/* 索引树高度，未知时为 -1 */

	/* 索引描述信息 */	
	int			ncolumns;				/* 索引列总数，包括索引键列和 INCLUDE 列 */
	int			nkeycolumns;			/* 索引键列数 */
	int		   *indexkeys;				/* 索引属性的列号（包括键列和包含列），表达式列为 0 */
	Oid		   *indexcollations;		/* 索引列的排序规则 OID */
	Oid		   *opfamily;				/* 索引列的操作符族 OID */
	Oid		   *opcintype;				/* 索引列的操作符类声明输入类型 OID */
	Oid		   *sortopfamily;			/* 若可排序，则为 btree 操作符族 OID，否则为 NULL */
	bool	   *reverse_sort;			/* 是否降序排序？ */
	bool	   *nulls_first;			/* NULL 是否排在前面？ */
	bool	   *canreturn;				/* 哪些索引列可用于索引仅扫描？ */
	Oid			relam;					/* 访问方法的 OID（pg_am） */

	List	   *indexprs;				/* 非简单索引列的表达式列表 */
	List	   *indpred;				/* 若为部分索引则为谓词，否则为 NIL */

	List	   *indextlist;				/* 表示索引列的 targetlist */

	List	   *indrestrictinfo;		/* 父关系的 baserestrictinfo 列表，去除被索引谓词隐含的条件（除非是目标关系，详见 check_index_predicates() 注释） */

	bool		predOK;					/* 索引谓词是否与查询匹配？ */
	bool		unique;					/* 是否唯一索引？ */
	bool		immediate;				/* 唯一性是否立即强制？ */
	bool		hypothetical;			/* 是否为假想索引（实际不存在）？ */

	/* 以下字段来自索引访问方法的 API 结构体： */
	bool		amcanorderbyop;			/* 访问方法是否支持 order by 操作符结果？ */
	bool		amoptionalkey;			/* 查询是否可省略首列键？ */
	bool		amsearcharray;			/* 访问方法是否支持 ScalarArrayOpExpr 条件？ */
	bool		amsearchnulls;			/* 访问方法是否支持搜索 NULL/NOT NULL？ */
	bool		amhasgettuple;			/* 是否有 amgettuple 接口？ */
	bool		amhasgetbitmap; 		/* 是否有 amgetbitmap 接口？ */
	bool		amcanparallel;			/* 是否支持并行扫描？ */
	bool		amcanmarkpos;			/* 是否支持 mark/restore？ */

	/* 为避免包含 amapi.h，这里直接声明 amcostestimate */
	void		(*amcostestimate) ();	/* 访问方法的成本估算函数 */
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
 * EC_MUST_BE_REDUNDANT - 判断等价类是否必然冗余
 *
 * 宏功能:
 * 该宏用于判断一个等价类(EC)是否必然冗余。如果等价类包含常量且不在外连接之下，
 * 那么依赖于它的任何路径键(PathKey)都必须是冗余的，因为该键只有一个可能的值。
 *
 * 参数说明:
 * eclass: 指向EquivalenceClass结构的指针，表示要检查的等价类
 *
 * 返回值:
 * 布尔值，如果等价类必然冗余则返回true，否则返回false
 *
 * 判断逻辑:
 * 1. (eclass)->ec_has_const: 检查等价类是否包含常量值
 * 2. !(eclass)->ec_below_outer_join: 检查等价类是否不在外连接之下
 * 3. 当两个条件都满足时，等价类必然冗余
 *
 * 应用场景:
 * 在查询优化过程中，当构建路径键列表时，可以通过此宏快速识别并跳过那些
 * 不会提供额外排序区分度的路径键，从而优化排序和索引匹配操作。
 *
 * 示例:
 * SELECT * FROM table WHERE x = 42 ORDER BY x, y;
 * 在这个例子中，x列由于等于常量42，因此ORDER BY子句中的x是冗余的，
 * 可以直接按y排序即可。
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
 * 路径的排序顺序由PathKey节点列表表示。
 * 空列表表示无已知排序。否则，列表的第一个元素表示主排序键，第二个表示第一次要排序键，
 * 依此类推。排序的值通过链接到包含该值的EquivalenceClass来表示，其中EquivalenceClass包含pk_opfamily
 * 在其ec_opfamilies中。EquivalenceClass还指定了要使用的排序规则。
 * 这是一种便捷的方法，因为它可以轻松检测等价和密切相关的排序方式。（更多信息请参阅optimizer/README）
 *
 * 注意：pk_strategy要么是BTLessStrategyNumber（表示ASC），要么是BTGreaterStrategyNumber（表示DESC）。
 * 我们假设所有支持排序的索引类型都将使用与btree兼容的策略编号。
 */

/*
 * PathKey结构体：表示查询执行路径中的单个排序键
 * 在PostgreSQL查询优化器中，PathKey用于描述查询结果集的排序状态
 * 多个PathKey组成的列表定义了完整的排序顺序
 */
typedef struct PathKey
{
	NodeTag		type;          /* 节点类型标记，用于运行时类型识别 */

	/*
	 * pk_eclass：指向等价类的指针，等价类包含被排序的值
	 * 等价类的使用使优化器能够识别不同表达式之间的等价关系，
	 * 从而在逻辑上等价的排序方式之间进行匹配
	 */
	EquivalenceClass *pk_eclass; /* 被排序的值 */
	
	/*
	 * pk_opfamily：定义排序的btree操作符族OID
	 * 操作符族决定了如何比较排序键的值，特别是对于非标准数据类型
	 * 不同的操作符族可能有不同的比较规则
	 */
	Oid			pk_opfamily;   /* 定义排序的btree操作符族 */
	
	/*
	 * pk_strategy：排序方向（升序或降序）
	 * 取值为BTLessStrategyNumber（表示ASC）或BTGreaterStrategyNumber（表示DESC）
	 * 策略号对应btree索引接口中定义的操作符策略
	 */
	int			pk_strategy;   /* 排序方向（ASC或DESC） */
	
	/*
	 * pk_nulls_first：NULL值排序行为标志
	 * true表示NULL值排在普通值之前，false表示NULL值排在普通值之后
	 * 控制SQL标准中的NULL排序规则行为
	 */
	bool		pk_nulls_first; /* NULL值是否排在普通值之前？ */
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

	NodeTag		pathtype;			/* 标识可构建的扫描/连接方法的 tag */

	RelOptInfo *parent;				/* 该 path 能构建的关系 */
	PathTarget *pathtarget;			/* 要计算的 Vars/Expr 列表、成本、宽度 */

	ParamPathInfo *param_info;		/* 参数化信息，若无则为 NULL */

	bool		parallel_aware; 	/* 是否启用并行感知逻辑？ */
	bool		parallel_safe;		/* 是否可安全用于并行计划？ */
	int			parallel_workers;	/* 期望的 worker 数；0 表示不并行 */

	/* 路径的估计大小/代价（详见 costsize.c） */
	double		rows;				/* 估计的结果元组数 */
	Cost		startup_cost;		/* 在获取任何元组前产生的代价 */
	Cost		total_cost;			/* 总代价（假定获取所有元组） */

	List	   *pathkeys;			/* path 输出的排序顺序 */
	/* pathkeys 是 PathKey 节点的 List；详见上文 */
} Path;

/* 获取路径的参数化外部关系 relids 的宏；注意避免重复求值
 * 如果 path->param_info 非空，则返回其 ppi_req_outer 字段（参数化所需的外部 relids）
 * 否则返回 NULL
 */
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
 * 每个 IndexClause 都引用查询的 WHERE 或 JOIN 条件中的一个 RestrictInfo 节点，
 * 并展示该限制如何应用于特定索引。我们支持既能被索引机制直接使用的 indexclauses，
 * 通常形式为 "indexcol OP pseudoconstant"，也支持可以从中推导出可索引条件的子句。
 * 最简单的转换是将 "pseudoconstant OP indexcol" 形式的子句交换左右，
 * 以生成可索引的条件（索引机制总是期望索引列在左侧）。
 * 另一个例子是可以从 LIKE 条件中提取可索引的范围条件，
 * 如 "x LIKE 'foo%bar'" 可转化为 "x >= 'foo' AND x < 'fop'"。
 * 这种有损条件的推导由附加在 indexclause 顶层函数或操作符上的 planner 支持函数完成。
 *
 * indexquals 是与该 IndexClause 相关的可直接用于索引的条件（RestrictInfo 列表）。
 * 最简单情况下，它是一个只包含 iclause->rinfo 的单元素列表。
 * 否则，它包含一个或多个从给定子句中提取的可直接用于索引的条件。
 * 'lossy' 标志表示 indexquals 是否与原始子句语义等价，或仅表示更弱的条件。
 *
 * 通常情况下，indexcol 是该子句作用的单个索引列的索引，indexcols 为 NIL。
 * 但如果子句是 RowCompareExpr，则 indexcol 是首列索引，indexcols 是所有受影响列的列表。
 * （注意，indexcols 与 indexquals 中实际可索引的 RowCompareExpr 的列对应，
 * 可能与 rinfo 中的原始表达式不同。）
 *
 * 一个 IndexPath 的 IndexClause 列表要求按索引列顺序排列，即 indexcol 值必须是非递减序列。
 * （同一索引列的多个子句顺序未指定。）
 */
typedef struct IndexClause
{
	NodeTag				type;
	struct RestrictInfo *rinfo; 		/* 原始限制或连接子句 */
	List	   			*indexquals;	/* 从其推导出的 indexqual 条件 */
	bool				lossy;			/* indexquals 是否为子句的有损版本？ */
	AttrNumber			indexcol;		/* 子句使用的索引列（从零开始） */	
	List	   			*indexcols;		/* 若为 RowCompare，则为多个索引列 */
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
 * UniquePath表示从其子路径的输出中消除重复行的操作。
 * 
 * 这个结构可以代表几种截然不同的执行计划：
 * 1. 基于哈希的实现
 * 2. 基于排序的实现
 * 3. 无操作（如果输入路径已经被证明是唯一的）
 * 
 * 由于这些决策是相对局部化的，因此不值得为每种情况创建单独的Path节点类型。
 * 注意：在无操作的情况下，我们可以完全消除UniquePath节点，直接返回子路径；
 * 但是在路径树中保留UniquePath节点可以向上层例程表明输入已知是唯一的，这很方便。
 */

/*
 * UniquePathMethod枚举定义了PostgreSQL查询优化器中去重操作的三种实现方法。
 * 这个枚举被用于UniquePath节点中，指定具体的去重策略。
 */
typedef enum
{
	UNIQUE_PATH_NOOP,  /* 无操作：输入数据已经保证唯一，无需额外处理。
                        * 这种情况通常发生在输入是主键扫描或唯一索引扫描时，
                        * 优化器可以直接推断出结果集的唯一性。
                        */
	UNIQUE_PATH_HASH,  /* 使用哈希算法：通过构建哈希表来去重。
                        * 这种方法的优点是在输入数据量适中时效率高，通常为O(n)复杂度。
                        * 适用于内存充足且不需要维护结果顺序的场景。
                        */
	UNIQUE_PATH_SORT   /* 使用排序方法：先对数据排序，然后扫描去除相邻重复项。
                        * 虽然排序本身需要O(n log n)时间，但如果查询中已经包含排序操作，
                        * 或者需要保持特定的输出顺序，则排序去重可能是更优选择。
                        */
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
 * JoinPath结构体
 * 表示查询优化器中的连接操作路径
 * 所有连接类型路径共享的字段，用于实现继承机制
 * 作为所有特定连接类型路径（如NestLoopPath、MergePath、HashPath等）的基类
 */
typedef struct JoinPath
{
	Path		path;   		/* 继承自Path结构体，包含所有路径共有的字段如成本、行数、宽度等 */

	JoinType	jointype;	/* 连接类型，如JOIN_INNER、JOIN_LEFT、JOIN_FULL、JOIN_RIGHT等 */

	bool		inner_unique;	/* 标记外部表的每个元组最多只匹配内部表的一个元组
						 * 此信息用于优化连接操作，如将某些连接类型降级以提高性能 */

	Path	   *outerjoinpath;  /* 连接操作外側（驱动表）的访问路径 */
	Path	   *innerjoinpath;  /* 连接操作内侧（被驱动表）的访问路径 */

	List	   *joinrestrictinfo; /* 应用于连接操作的限制条件列表，每个元素为RestrictInfo结构体
						 * 包含连接条件的表达式和相关优化信息 */

	/*
	 * 参考RelOptInfo和ParamPathInfo的注释，了解为什么joinrestrictinfo需要存在于JoinPath中，
	 * 而不能合并到父级RelOptInfo中。这主要是因为连接条件可能包含参数化信息，
	 * 需要与特定的连接路径相关联，而不是与一般的关系表示相关联。
	 */
} JoinPath;


/*
 * A nested-loop path needs no special fields.
 * 嵌套循环路径不需要特殊字段
 *
 * 注释说明：嵌套循环连接(Nested Loop Join)路径的表示不需要在JoinPath基类之外添加任何特殊字段，
 * 所有必要的信息（如连接类型、内外侧路径、连接条件等）都已经包含在基类JoinPath中。
 */

/*
 * NestPath - 嵌套循环连接路径类型
 * 将JoinPath类型定义为NestPath的别名
 *
 * 由于嵌套循环连接的执行逻辑相对简单，不需要存储额外的优化信息，因此直接复用了JoinPath结构体。
 * 相比之下，其他连接类型（如哈希连接、归并连接）需要额外的字段来存储特定的执行参数和优化信息。
 */
typedef JoinPath NestPath;


/*
 * MergePath结构体定义了合并连接(Merge Join)路径节点
 *
 * 与其他路径类型不同，一个MergePath节点不仅代表单个运行时计划节点，它最多可以代表四个节点：
 * 1. MergeJoin节点本身
 * 2. 用于外部输入的Sort节点（如需要）
 * 3. 用于内部输入的Sort节点（如需要）
 * 4. 用于内部输入的Material节点（如需要）
 *
 * 虽然我们可以用单独的路径节点来表示这些节点，但考虑到复杂连接问题中会考察大量不同的合并路径，
 * 将这些信息合并到一个结构体中可以避免不必要的内存分配开销。
 *
 * path_mergeclauses列出了将用于合并的连接条件（以RestrictInfo形式）。
 * 注意，这些合并条件是父关系限制条件列表的一个子集。任何不可用于合并连接的条件
 * 仅出现在父关系的限制列表中，必须在执行时由qpqual进行检查。
 *
 * outersortkeys（或innersortkeys）为NIL表示外部路径（或内部路径）已经具有适合合并连接的顺序。
 * 如果不为NIL，则表示需要由显式Sort节点创建的排序顺序，以PathKeys列表形式描述。
 *
 * skip_mark_restore为true表示执行器不需要执行mark/restore操作。
 * 通常情况下需要mark/restore开销，但如果我们知道对于每个外部元组只需要找到一个匹配项，
 * 并且合并条件足以识别匹配项，那么可以跳过这些操作。在这种情况下，执行器可以在处理完一个匹配项后
 * 立即前进到下一个外部关系元组，因此永远不需要回退内部关系。
 *
 * materialize_inner为true表示应该在内部输入之上放置一个Material节点。
 * 这可以与内部Sort步骤一起出现，也可以不一起出现。
 */

/*
 * MergePath结构体：描述合并连接(Merge Join)查询路径的节点结构
 * 继承自JoinPath，添加了合并连接特有的属性
 */
typedef struct MergePath
{
	JoinPath	jpath;              /* 继承自JoinPath，包含基本的连接路径信息 */
	List	   *path_mergeclauses;    /* 用于合并连接的连接条件列表，每个元素为RestrictInfo */
	List	   *outersortkeys;        /* 外部输入需要的显式排序键，如果不需要排序则为NIL */
	List	   *innersortkeys;        /* 内部输入需要的显式排序键，如果不需要排序则为NIL */
	bool		skip_mark_restore;      /* 执行器是否可以跳过mark/restore操作的标志 */
	bool		materialize_inner;     /* 是否需要为内部输入添加Materialize节点的标志 */
} MergePath;

/*
 * A hashjoin path has these fields.
 * 哈希连接路径具有以下字段
 *
 * The remarks above for mergeclauses apply for hashclauses as well.
 * 上面关于mergeclauses的说明同样适用于hashclauses
 *
 * Hashjoin does not care what order its inputs appear in, so we have
 * no need for sortkeys.
 * 哈希连接不关心输入的顺序，因此我们不需要排序键
 *
 * 注释说明：哈希连接是一种高效的连接算法，它通过将一个关系（通常是较小的内部表）构建成哈希表，
 * 然后扫描另一个关系（外部表）并查找匹配的哈希桶来执行连接操作。与归并连接不同，哈希连接不需要
 * 输入关系预先排序，这是它的一个重要特性。
 */

/*
 * HashPath - 哈希连接路径结构体
 * 表示PostgreSQL查询优化器中哈希连接操作的执行路径
 * 继承自JoinPath基类，添加了哈希连接特有的字段
 */
typedef struct HashPath
{
	JoinPath	jpath;               /* 继承自JoinPath的基本连接信息 */
	List	   *path_hashclauses;     /* 用于哈希的连接条件列表
						 * 每个元素为RestrictInfo结构体，表示将用于构建哈希表和查找匹配的条件 */
	int			num_batches;          /* 预期的批次数
						 * 当内部表太大无法一次性放入内存时，需要分批构建哈希表
						 * 此值决定了哈希连接将使用多少批次来处理数据 */
	double		inner_rows_total;    /* 预期的内部表总行数
						 * 用于计算哈希表大小和批次数的估计值
						 * 这对于内存管理和执行计划成本估算非常重要 */
} HashPath;


/*
 * ProjectionPath结构体：表示查询计划中的投影操作（目标列表计算）
 *
 * 投影操作本质上是将查询执行过程中获取的数据转换为最终结果集所需的形式，
 * 包括列重命名、表达式计算、函数应用等操作。
 *
 * 在名义上，该路径节点表示使用Result计划节点执行投影步骤。然而，PostgreSQL
 * 采用了一种优化策略：如果输入的计划节点本身支持投影（如通过is_projection_capable_path判断），
 * 则可以直接修改该节点的输出目标列表，无需额外的Result节点。
 *
 * 规划器在不同场景下采用不同策略：
 * 1. 在某些情况下，可以直接将目标PathTarget嵌入到输入路径节点中（同时调整成本），
 *    此时不需要创建ProjectionPath节点。
 * 2. 而在其他情况下（如需要保持输入路径节点不变），则需要创建独立的ProjectionPath节点，
 *    通过设置dummypp标记来指示实际工作将由输入计划节点执行，而非创建单独的Result节点。
 *
 * ProjectionPath节点的估计成本会根据是否需要创建单独的Result节点而有所不同，
 * 这种设计使得查询优化器能够灵活地选择最有效的投影执行方式。
 */
typedef struct ProjectionPath
{
	Path		path;           /* 继承自Path结构体的公共字段，包括成本、行估计等信息 */
	Path	   *subpath;        /* 表示输入数据源的路径节点，投影操作将应用于该路径的结果 */
	bool		dummypp;        /* 标记是否需要单独的Result节点：
                               		* true表示投影工作将由subpath处理，无需单独Result节点
                               		* false表示需要创建单独的Result节点执行投影 */
} ProjectionPath;


/*
 * ProjectSetPath结构体：表示包含集合返回函数(SRFs)的目标列表的计算路径
 *
 * 集合返回函数(Set-Returning Functions)是一种特殊函数，它可以为每行输入返回多行结果集，
 * 例如generate_series()、unnest()等函数。这类函数在查询执行过程中需要特殊处理，
 * 因此需要通过ProjectSetPath来表示使用ProjectSet计划节点执行的路径。
 *
 * ProjectSet节点的主要作用是正确处理集合返回函数的执行和结果展开，确保函数调用
 * 与行处理正确对应，特别是当函数结果集大小与输入行数不同时。
 */
typedef struct ProjectSetPath
{
	Path		path;           /* 继承自Path结构体的公共字段，包括成本、行估计等信息 */
	Path	   *subpath;        /* 表示输入数据源的路径节点，集合返回函数将应用于该路径的结果 */
} ProjectSetPath;

/*
 * SortPath结构体：表示显式排序步骤的执行路径
 *
 * 排序操作是查询执行计划中的重要组件，用于实现ORDER BY子句或支持某些连接操作
 *（如归并连接）。SortPath节点表示使用Sort计划节点执行排序操作的路径。
 *
 * 排序键信息存储在继承自Path结构体的pathkeys字段中，通过定义可以确保排序键与pathkeys一致。
 *
 * 重要限制：Sort计划节点本身不支持投影操作，因此SortPath的pathtarget必须与输入路径的
 * pathtarget完全相同，即排序操作不会改变结果的数据结构，只改变元组的顺序。
 */
typedef struct SortPath
{
	Path		path;           /* 继承自Path结构体的公共字段，包括成本、行估计和排序键(pathkeys)等信息 */
	Path	   *subpath;        /* 表示输入数据源的路径节点，排序操作将应用于该路径的结果 */
} SortPath;


/*
 * GroupPath结构体：表示对已排序输入进行分组操作的执行路径
 *
 * 此路径节点用于实现SQL查询中的GROUP BY功能，特别是在输入数据已经按照分组键排序的情况下。
 * 通过利用已排序的输入，可以高效地实现分组操作，而无需额外的排序或哈希操作。
 *
 * groupClause字段定义了用于分组的列；输入路径必须至少按照这些分组列进行了排序。
 * 此外，GroupPath还支持对分组后的行应用过滤条件，这对应于SQL中的HAVING子句。
 */
typedef struct GroupPath
{
	Path		path;           /* 继承自Path结构体的公共字段，包括成本、行估计等信息 */
	Path	   *subpath;        /* 表示输入数据源的路径节点，该路径必须已按分组键排序 */
	List	   *groupClause;    /* 分组子句列表，包含SortGroupClause结构，定义分组的列和排序规则 */
	List	   *qual;           /* 分组后的过滤条件（HAVING条件），如果有的话 */
} GroupPath;

/*
 * UpperUniquePath结构体：表示在已排序输入中去除相邻重复值的执行路径
 *
 * 此路径节点对应于SQL中的DISTINCT操作或需要去除重复行的场景，特别是当输入已经排序时。
 * 通过利用输入已排序的特性，可以高效地检测和去除相邻的重复行，而无需构建哈希表或进行完整的排序。
 *
 * 用于比较重复的列是路径pathkeys中的前numkeys个列，输入数据被假定已经按照这些列排序。
 */
typedef struct UpperUniquePath
{
	Path		path;           /* 继承自Path结构体的公共字段，包括成本、行估计和排序键等信息 */
	Path	   *subpath;        /* 表示输入数据源的路径节点，该路径必须已按指定的键排序 */
	int			numkeys;        /* 用于比较重复值的pathkey列数量，表示前numkeys个排序键列用于判断重复 */
} UpperUniquePath;

/*
 * AggPath结构体：表示聚合函数通用计算的执行路径
 *
 * 此路径节点用于实现SQL中的聚合函数计算（如SUM、AVG、COUNT等），可以支持简单分组（非分组集）
 * 的聚合操作，使用排序或哈希分组策略。对于排序分组(AGG_SORTED)的情况，输入必须已经适当排序。
 *
 * AggPath是PostgreSQL中实现聚合操作的核心路径类型，它支持多种聚合策略和优化模式，
 * 并包含了执行聚合所需的所有关键信息。
 */
typedef struct AggPath
{
	Path		path;           /* 继承自Path结构体的公共字段，包括成本、行估计等信息 */
	Path	   *subpath;        /* 表示输入数据源的路径节点，对于排序聚合，该路径必须已排序 */
	AggStrategy aggstrategy;   /* 基本聚合策略，定义在nodes.h中，如AGG_HASHED(哈希聚合)、AGG_SORTED(排序聚合)等 */
	AggSplit	aggsplit;       /* 聚合拆分模式，定义在nodes.h中，表示聚合计算的拆分策略 */
	double		numGroups;      /* 输入中估计的组数，用于成本计算和资源分配 */
	List	   *groupClause;    /* 分组子句列表，包含SortGroupClause结构，定义分组的列和排序规则 */
	List	   *qual;           /* 分组后的过滤条件（HAVING条件），如果有的话 */
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
 * ModifyTablePath 表示执行 INSERT/UPDATE/DELETE 数据修改操作的路径节点
 * 
 * 这个结构体几乎直接表示了最终 ModifyTable 计划节点中的所有内容，
 * 唯一的区别是它包含的是子路径(Path)而不是子计划(Plan)。
 * 注意：OnConflictExpr 的分析会推迟到 createplan.c 中进行，
 * FDW(外部数据包装器)数据的收集也是如此。
 */
typedef struct ModifyTablePath
{
	Path		path;           /* 继承自基础Path结构，包含路径的通用信息（如成本、行数等） */
	CmdType		operation;      /* 操作类型：INSERT、UPDATE 或 DELETE */
	bool		canSetTag;       /* 是否设置命令标签和es_processed计数 */
	Index		nominalRelation; /* 用于EXPLAIN显示的父关系RT索引 */
	Index		rootRelation;    /* 如果目标是分区表，则为根表RT索引 */
	bool		partColsUpdated; /* 是否更新了分区层次结构中的某些分区键 */
	List	   *resultRelations; /* RT索引的整数列表，表示要修改的表 */
	List	   *subpaths;       /* 生成源数据的子路径(可以有多个，如CTE或VALUES子句) */
	List	   *subroots;       /* 每个目标表对应的PlannerInfo结构 */
	List	   *withCheckOptionLists; /* 每个目标表的WITH CHECK OPTION条件列表 */
	List	   *returningLists; /* 每个目标表的RETURNING子句选择列表 */
	List	   *rowMarks;       /* PlanRowMarks列表（仅非锁定的），用于行级并发控制 */
	OnConflictExpr *onconflict; /* ON CONFLICT子句表达式，用于INSERT ... ON CONFLICT，为NULL时表示无此子句 */
	int		epqParam;          /* 用于EvalPlanQual重新评估的Param ID，处理并发更新冲突 */
} ModifyTablePath;

/*
 * LimitPath 表示应用 LIMIT/OFFSET 限制的路径节点
 * 
 * 这个结构体用于表示SQL查询中的LIMIT和OFFSET子句，
 * 它控制最终结果集返回的行数和起始位置。
 */
typedef struct LimitPath
{
	Path		path;           /* 继承自基础Path结构，包含路径的通用信息 */
	Path	   *subpath;        /* 表示输入数据源的子路径 */
	Node	   *limitOffset;    /* OFFSET参数表达式，为NULL表示无偏移 */
	Node	   *limitCount;     /* COUNT参数表达式，为NULL表示无限制 */
} LimitPath;


/*
 * 限制子句信息（RestrictInfo）。
 *
 * 对于每个限制条件（WHERE 或 JOIN/ON 子句）中的 AND 子句，我们都会创建一个 RestrictInfo 结构体。
 * 由于这些限制子句是逻辑上的 AND 关系，可以单独或组合使用它们过滤元组，无需全部评估。
 * RestrictInfo 节点用于优化器选择最佳查询计划时存储相关数据。
 *
 * 如果限制子句只引用一个基表，则会出现在该基表的 RelOptInfo 的 baserestrictinfo 列表中。
 * 如果引用多个基表，则会出现在每个严格子集的 RelOptInfo 的 joininfo 列表中，用于驱动连接树的构建。
 * 只有当我们构建了包含所有被引用基表的连接关系时，才能实际应用该子句。
 *
 * 多关系限制子句在不同连接顺序下可能到达连接树的不同高度，因此不能直接与连接 RelOptInfo 关联，
 * 而是需要在每个连接路径上单独跟踪。
 *
 * 等价条件（如可用于合并连接的等值条件）会先附加到 EquivalenceClass，
 * 在构建扫描或连接路径时，再从 EquivalenceClass 生成实际需要检查的 RestrictInfo。
 *
 * 外连接处理时需特别注意子句的下推与上推。外连接的 JOIN/ON 条件必须在该连接节点处评估，
 * 除非是“退化”条件（只引用可空侧的变量）。WHERE 或更高层 JOIN 的条件不能下推到外连接之下，
 * 如果引用了可空变量。RestrictInfo 包含 is_pushed_down 标志，指示该子句是否被下推到比其语法位置更低的层级。
 * 若外连接阻止子句下推到其“自然”语义层级，则 required_relids 会包含比实际引用更多的基表，
 * 这样可防止其被评估在外连接之下。
 *
 * outerjoin_delayed 标志表示该子句因下层外连接而必须延迟应用。
 * outer_relids 字段仅对外连接子句有效，表示外连接的外侧基表集合。
 * nullable_relids 表示该子句引用的、可能被外连接强制为 NULL 的基表集合。
 *
 * security_level 字段用于安全屏障条件，值越高表示来源越不可信。只有泄漏安全（leakproof）的子句才能在更低安全级别的子句之前评估。
 *
 * can_join 标志表示该子句可能可用于合并连接或哈希连接（即二元操作符且左右引用的基表集合不重叠）。
 * pseudoconstant 标志表示该子句不引用本查询级别的 Vars 且无易变函数，可作为一次性条件在 gating Result 节点中评估。
 *
 * parent_ec 字段指向生成该子句的 EquivalenceClass，若多个子句在同一连接中有相同 parent_ec，则它们是冗余的。
 *
 * 其它字段用于缓存成本、选择性、合并连接/哈希连接相关信息等。
 */
typedef struct RestrictInfo
{
	NodeTag		type;

	Expr	   *clause;			/* 实际的 WHERE 或 JOIN 子句表达式 */

	bool		is_pushed_down; /* 是否被下推到更低层级 */

	bool		outerjoin_delayed;	/* 是否因下层外连接而延迟应用 */

	bool		can_join;		/* 是否可能用于合并/哈希连接 */

	bool		pseudoconstant; /* 是否为伪常量子句 */

	bool		leakproof;		/* 是否为泄漏安全子句 */

	Index		security_level; /* 安全级别 */

	Relids		clause_relids;	/* 实际引用的基表集合 */

	Relids		required_relids;/* 评估该子句所需的基表集合 */

	Relids		outer_relids;	/* 外连接子句的外侧基表集合，否则为 NULL */

	Relids		nullable_relids;/* 可能被外连接强制为 NULL 的基表集合 */

	Relids		left_relids;	/* 子句左侧引用的基表集合（二元操作符时） */
	Relids		right_relids;	/* 子句右侧引用的基表集合（二元操作符时） */

	Expr	   *orclause;		/* 若为 OR 子句，则为带 RestrictInfo 的修改版 */

	EquivalenceClass *parent_ec;	/* 生成该子句的 EquivalenceClass，冗余检测用 */

	QualCost		eval_cost;		/* 评估该子句的成本，未设置时为 -1 */
	Selectivity 	norm_selec;		/* “普通”连接语义下的选择性，未设置时为 -1，>1 表示冗余 */
	Selectivity 	outer_selec;	/* 外连接语义下的选择性，未设置时为 -1 */

	List	   		*mergeopfamilies;	/* 若可合并连接，则为操作符族列表，否则为 NIL */

	EquivalenceClass 	*left_ec;		/* 合并连接左侧的 EquivalenceClass */
	EquivalenceClass 	*right_ec; 		/* 合并连接右侧的 EquivalenceClass */
	EquivalenceMember 	*left_em; 		/* 合并连接左侧的 EquivalenceMember */
	EquivalenceMember 	*right_em;		/* 合并连接右侧的 EquivalenceMember */
	List	   			*scansel_cache;	/* 合并连接选择性缓存（MergeScanSelCache 列表） */

	bool		outer_is_left; 		/* 临时工作区：外连接时外表在左侧？ */

	Oid			hashjoinoperator;	/* 若可哈希连接，则为操作符 OID，否则为 InvalidOid */

	Selectivity left_bucketsize;	/* 哈希连接左侧平均桶大小，未设置为 -1 */
	Selectivity right_bucketsize;	/* 哈希连接右侧平均桶大小，未设置为 -1 */
	Selectivity left_mcvfreq; 		/* 左侧最常见值的频率 */
	Selectivity right_mcvfreq; 		/* 右侧最常见值的频率 */
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
 * 当我们将一个可继承表或 UNION-ALL 子查询展开为“追加关系”（append relation，
 * 本质上是一组子 RTE）时，我们会为每个子 RTE 构建一个 AppendRelInfo。
 * AppendRelInfo 列表指示在展开父级时必须包含哪些子 RTE，并且每个节点都携带
 * 将引用父级的 Vars 转换为引用该子级的 Vars 所需的信息。
 *
 * 这些结构保存在 PlannerInfo 节点的 append_rel_list 中。
 * 注意，我们将所有结构都放入一个列表中，并在需要展开任何一个父级时扫描整个列表。
 * 我们本可以使用更复杂的数据结构（例如，每个父级一个列表），但这在执行诸如
 * 提升子查询之类的操作时会更难更新，而且扫描起来也不见得更容易。
 * 考虑到典型的查询不会有很多不同的追加父级，让事情变得复杂似乎不值得。
 *
 * 注意：在规划器准备阶段完成后，任何给定的 RTE 只有在其“inh”标志被设置时，
 * 才是 append_rel_list 中有条目的追加父级。
 * 对于结果证明没有继承子级的普通表，我们会清除“inh”，而对于结果证明是
 * 可展平的 UNION ALL 查询的子查询 RTE，我们会设置“inh”（这是对该标志原始含义的滥用）。
 * 这使我们可以避免对 append_rel_list 进行无用的搜索。
 *
 * 注意：该数据结构假设追加关系成员是单个基本关系（baserels）。
 * 这对于继承是可以的，但它阻止了我们提升包含连接的 UNION ALL 成员子查询。
 * 虽然这可以通过更复杂的数据结构来解决，但目前没有太大意义，因为这不会带来计划上的改进。
 */

typedef struct AppendRelInfo
{
	NodeTag		type;

	/*
	 * 这些字段唯一标识此追加关系。
	 * 对于同一个 parent_relid，可以有（实际上总是应该有）多个 AppendRelInfo，
	 * 但对于每个 child_relid 绝不会超过一个，因为给定的 RTE 不能是多个追加父级的子级。
	 */
	Index		parent_relid;	/* 追加父级关系的 RT 索引 */
	Index		child_relid;	/* 追加子级关系的 RT 索引 */

	/*
	 * 对于继承追加关系，父级和子级都是普通关系，我们在此存储它们的行类型 OID，
	 * 以用于转换整行 Vars。对于 UNION-ALL 追加关系，父级和子级都是没有命名行类型的子查询，
	 * 我们在此存储 InvalidOid。
	 */
	Oid			parent_reltype; /* 父级复合类型的 OID */
	Oid			child_reltype;	/* 子级复合类型的 OID */

	/*
	 * 此列表的第 N 个元素是表示对应于父级第 N 列的子级列的 Var 或表达式。
	 * 这用于将引用父级关系的 Vars 转换为对子级的引用。
	 * 如果列表元素对应于父级的已删除列，则该元素为 NULL（这仅可能发生在继承情况下，
	 * 而不是 UNION ALL）。对于继承情况，列表元素始终是简单的 Vars，
	 * 但在 UNION ALL 情况下可以是任意表达式。
	 *
	 * 注意，我们只存储用户列（attno > 0）的条目。整行 Vars 是特殊情况，
	 * 系统列（attno < 0）不需要特殊转换，因为它们的 attnos 对于所有表都是相同的。
	 *
	 * 注意：Vars 的 varlevelsup = 0。在复制到子查询中时，请务必根据需要进行调整。
	 */
	List	   *translated_vars;	/* 子级 Vars 中的表达式 */

	/*
	 * 对于继承，我们在此存储父表的 OID，对于 UNION ALL 则存储 InvalidOid。
	 * 这仅用于在尝试引用已删除的父列时帮助生成错误消息。
	 */
	Oid			parent_reloid;	/* 父关系的 OID */
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
 * 分组实现方式相关的标志位定义
 *
 * GROUPING_CAN_USE_SORT：如果可以使用排序方式实现分组，则设置该标志。
 *   当使用分组集（grouping sets）时，只要有任意一个分组集可以用排序实现，也会设置此标志。
 *
 * GROUPING_CAN_USE_HASH：如果可以使用哈希方式实现分组，则设置该标志。
 *
 * GROUPING_CAN_PARTIAL_AGG：如果该聚合类型支持部分聚合（partial aggregation），则设置该标志。
 *   例如普通分组支持部分聚合，但分组集不支持。该标志不表示并行安全性或是否有合适的路径。
 */
#define GROUPING_CAN_USE_SORT       0x0001   /* 可使用排序分组 */
#define GROUPING_CAN_USE_HASH       0x0002   /* 可使用哈希分组 */
#define GROUPING_CAN_PARTIAL_AGG    0x0004   /* 支持部分聚合 */

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
 * 传递给 create_grouping_paths 子程序的额外信息结构体
 *
 * flags 指示可能的聚合方式（HASHED, SORTED, PARTIAL 等）。
 * 如果 agg_partial_costs 和 agg_final_costs 已被初始化，则 partial_costs_set 为 true。
 * agg_partial_costs 给出执行部分聚合（Partial Aggregation）的成本。
 * agg_final_costs 给出执行最终聚合（Finalization）的成本。
 * 如果目标列表（TargetList）是并行安全的，则 target_parallel_safe 为 true。
 * havingQual 给出聚合后要应用的过滤条件（HAVING 子句）。
 * targetList 给出要投影的列列表。
 * patype 是正在执行的分区聚合类型。
 */
typedef struct
{
	/* 设置后保持不变的数据。 */
	int			flags;
	bool		partial_costs_set;
	AggClauseCosts agg_partial_costs;
	AggClauseCosts agg_final_costs;

	/* 可能因分区而异的数据。 */
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
