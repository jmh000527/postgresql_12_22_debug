/*-------------------------------------------------------------------------
 *
 * plannodes.h
 *	  definitions for query plan nodes
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/plannodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANNODES_H
#define PLANNODES_H

#include "access/sdir.h"
#include "access/stratnum.h"
#include "lib/stringinfo.h"
#include "nodes/bitmapset.h"
#include "nodes/lockoptions.h"
#include "nodes/primnodes.h"


/* ----------------------------------------------------------------
 *						node definitions
 * ----------------------------------------------------------------
 */

/* ----------------
 *		PlannedStmt node
 *
 * The output of the planner is a Plan tree headed by a PlannedStmt node.
 * PlannedStmt holds the "one time" information needed by the executor.
 *
 * For simplicity in APIs, we also wrap utility statements in PlannedStmt
 * nodes; in such cases, commandType == CMD_UTILITY, the statement itself
 * is in the utilityStmt field, and the rest of the struct is mostly dummy.
 * (We do use canSetTag, stmt_location, stmt_len, and possibly queryId.)
 * ----------------
 */
typedef struct PlannedStmt
{
	NodeTag		type;

	CmdType		commandType;	/* select|insert|update|delete|utility */

	uint64		queryId;		/* query identifier (copied from Query) */

	bool		hasReturning;	/* is it insert|update|delete RETURNING? */

	bool		hasModifyingCTE;	/* has insert|update|delete in WITH? */

	bool		canSetTag;		/* do I set the command result tag? */

	bool		transientPlan;	/* redo plan when TransactionXmin changes? */

	bool		dependsOnRole;	/* is plan specific to current role? */

	bool		parallelModeNeeded; /* parallel mode required to execute? */

	int			jitFlags;		/* which forms of JIT should be performed */

	struct Plan *planTree;		/* tree of Plan nodes */

	List	   *rtable;			/* list of RangeTblEntry nodes */

	/* rtable indexes of target relations for INSERT/UPDATE/DELETE */
	List	   *resultRelations;	/* integer list of RT indexes, or NIL */

	/*
	 * rtable indexes of partitioned table roots that are UPDATE/DELETE
	 * targets; needed for trigger firing.
	 */
	List	   *rootResultRelations;

	List	   *subplans;		/* Plan trees for SubPlan expressions; note
								 * that some could be NULL */

	Bitmapset  *rewindPlanIDs;	/* indices of subplans that require REWIND */

	List	   *rowMarks;		/* a list of PlanRowMark's */

	List	   *relationOids;	/* OIDs of relations the plan depends on */

	List	   *invalItems;		/* other dependencies, as PlanInvalItems */

	List	   *paramExecTypes; /* type OIDs for PARAM_EXEC Params */

	Node	   *utilityStmt;	/* non-null if this is utility stmt */

	/* statement location in source string (copied from Query) */
	int			stmt_location;	/* start location, or -1 if unknown */
	int			stmt_len;		/* length in bytes; 0 means "rest of string" */
} PlannedStmt;

/* macro for fetching the Plan associated with a SubPlan node */
#define exec_subplan_get_plan(plannedstmt, subplan) \
	((Plan *) list_nth((plannedstmt)->subplans, (subplan)->plan_id - 1))


/* ----------------
 *		Plan node
 *
 * All plan nodes "derive" from the Plan structure by having the
 * Plan structure as the first field.  This ensures that everything works
 * when nodes are cast to Plan's.  (node pointers are frequently cast to Plan*
 * when passed around generically in the executor)
 *
 * We never actually instantiate any Plan nodes; this is just the common
 * abstract superclass for all Plan-type nodes.
 * ----------------
 */
typedef struct Plan
{
	NodeTag		type;

	/*
	 * estimated execution costs for plan (see costsize.c for more info)
	 */
	Cost		startup_cost;	/* cost expended before fetching any tuples */
	Cost		total_cost;		/* total cost (assuming all tuples fetched) */

	/*
	 * planner's estimate of result size of this plan step
	 */
	double		plan_rows;		/* number of rows plan is expected to emit */
	int			plan_width;		/* average row width in bytes */

	/*
	 * information needed for parallel query
	 */
	bool		parallel_aware; /* engage parallel-aware logic? */
	bool		parallel_safe;	/* OK to use as part of parallel plan? */

	/*
	 * Common structural data for all Plan types.
	 */
	int			plan_node_id;	/* unique across entire final plan tree */
	List	   *targetlist;		/* target list to be computed at this node */
	List	   *qual;			/* implicitly-ANDed qual conditions */
	struct Plan *lefttree;		/* input plan tree(s) */
	struct Plan *righttree;
	List	   *initPlan;		/* Init Plan nodes (un-correlated expr
								 * subselects) */

	/*
	 * Information for management of parameter-change-driven rescanning
	 *
	 * extParam includes the paramIDs of all external PARAM_EXEC params
	 * affecting this plan node or its children.  setParam params from the
	 * node's initPlans are not included, but their extParams are.
	 *
	 * allParam includes all the extParam paramIDs, plus the IDs of local
	 * params that affect the node (i.e., the setParams of its initplans).
	 * These are _all_ the PARAM_EXEC params that affect this node.
	 */
	Bitmapset  *extParam;
	Bitmapset  *allParam;
} Plan;

/* ----------------
 *	these are defined to avoid confusion problems with "left"
 *	and "right" and "inner" and "outer".  The convention is that
 *	the "left" plan is the "outer" plan and the "right" plan is
 *	the inner plan, but these make the code more readable.
 * ----------------
 */
#define innerPlan(node)			(((Plan *)(node))->righttree)
#define outerPlan(node)			(((Plan *)(node))->lefttree)


/* ----------------
 *	 Result node -
 *		If no outer plan, evaluate a variable-free targetlist.
 *		If outer plan, return tuples from outer plan (after a level of
 *		projection as shown by targetlist).
 *
 * If resconstantqual isn't NULL, it represents a one-time qualification
 * test (i.e., one that doesn't depend on any variables from the outer plan,
 * so needs to be evaluated only once).
 * ----------------
 */
typedef struct Result
{
	Plan		plan;
	Node	   *resconstantqual;
} Result;

/* ----------------
 *	 ProjectSet node -
 *		Apply a projection that includes set-returning functions to the
 *		output tuples of the outer plan.
 * ----------------
 */
typedef struct ProjectSet
{
	Plan		plan;
} ProjectSet;

/* ----------------
 *	 ModifyTable node -
 *		Apply rows produced by subplan(s) to result table(s),
 *		by inserting, updating, or deleting.
 *
 * If the originally named target table is a partitioned table, both
 * nominalRelation and rootRelation contain the RT index of the partition
 * root, which is not otherwise mentioned in the plan.  Otherwise rootRelation
 * is zero.  However, nominalRelation will always be set, as it's the rel that
 * EXPLAIN should claim is the INSERT/UPDATE/DELETE target.
 *
 * Note that rowMarks and epqParam are presumed to be valid for all the
 * subplan(s); they can't contain any info that varies across subplans.
 * ----------------
 */
typedef struct ModifyTable
{
	Plan		plan;
	CmdType		operation;		/* INSERT, UPDATE, or DELETE */
	bool		canSetTag;		/* do we set the command tag/es_processed? */
	Index		nominalRelation;	/* Parent RT index for use of EXPLAIN */
	Index		rootRelation;	/* Root RT index, if target is partitioned */
	bool		partColsUpdated;	/* some part key in hierarchy updated */
	List	   *resultRelations;	/* integer list of RT indexes */
	int			resultRelIndex; /* index of first resultRel in plan's list */
	int			rootResultRelIndex; /* index of the partitioned table root */
	List	   *plans;			/* plan(s) producing source data */
	List	   *withCheckOptionLists;	/* per-target-table WCO lists */
	List	   *returningLists; /* per-target-table RETURNING tlists */
	List	   *fdwPrivLists;	/* per-target-table FDW private data lists */
	Bitmapset  *fdwDirectModifyPlans;	/* indices of FDW DM plans */
	List	   *rowMarks;		/* PlanRowMarks (non-locking only) */
	int			epqParam;		/* ID of Param for EvalPlanQual re-eval */
	OnConflictAction onConflictAction;	/* ON CONFLICT action */
	List	   *arbiterIndexes; /* List of ON CONFLICT arbiter index OIDs  */
	List	   *onConflictSet;	/* SET for INSERT ON CONFLICT DO UPDATE */
	Node	   *onConflictWhere;	/* WHERE for ON CONFLICT UPDATE */
	Index		exclRelRTI;		/* RTI of the EXCLUDED pseudo relation */
	List	   *exclRelTlist;	/* tlist of the EXCLUDED pseudo relation */
} ModifyTable;

struct PartitionPruneInfo;		/* forward reference to struct below */

/* ----------------
 *	 Append节点 -
 *		生成子计划结果的串联。
 * ----------------
 */
typedef struct Append
{
	Plan		plan;
	List	   *appendplans;		/* 子计划列表 */

	/*
	 * 在此索引之前的所有'appendplans'都是非部分计划。从此索引开始的
	 * 所有'appendplans'都是部分计划。
	 */
	int			first_partial_plan;

	/* 运行时子计划剪枝信息；如果不进行剪枝则为NULL */
	struct PartitionPruneInfo *part_prune_info;
} Append;

/* ----------------
 *	 MergeAppend节点 -
 *		合并预排序的子计划结果以保持排序。
 * ----------------
 */
typedef struct MergeAppend
{
	Plan		plan;
	List	   *mergeplans;
	/* 以下字段与Sort结构体中的排序键信息一致： */
	int			numCols;		/* 排序键列的数量 */
	AttrNumber *sortColIdx;		/* 在目标列表中的索引 */
	Oid		   *sortOperators;	/* 用于排序的操作符OID */
	Oid		   *collations;		/* 排序规则的OID */
	bool	   *nullsFirst;		/* NULL值是否排在前面 */
	/* 运行时子计划剪枝信息；如果不进行剪枝则为NULL */
	struct PartitionPruneInfo *part_prune_info;
} MergeAppend;

/* ----------------
 *	RecursiveUnion node -
 *		Generate a recursive union of two subplans.
 *
 * The "outer" subplan is always the non-recursive term, and the "inner"
 * subplan is the recursive term.
 * ----------------
 */
typedef struct RecursiveUnion
{
	Plan		plan;
	int			wtParam;		/* ID of Param representing work table */
	/* Remaining fields are zero/null in UNION ALL case */
	int			numCols;		/* number of columns to check for
								 * duplicate-ness */
	AttrNumber *dupColIdx;		/* their indexes in the target list */
	Oid		   *dupOperators;	/* equality operators to compare with */
	Oid		   *dupCollations;
	long		numGroups;		/* estimated number of groups in input */
} RecursiveUnion;

/* ----------------
 *	 BitmapAnd node -
 *		Generate the intersection of the results of sub-plans.
 *
 * The subplans must be of types that yield tuple bitmaps.  The targetlist
 * and qual fields of the plan are unused and are always NIL.
 * ----------------
 */
typedef struct BitmapAnd
{
	Plan		plan;
	List	   *bitmapplans;
} BitmapAnd;

/* ----------------
 *	 BitmapOr node -
 *		Generate the union of the results of sub-plans.
 *
 * The subplans must be of types that yield tuple bitmaps.  The targetlist
 * and qual fields of the plan are unused and are always NIL.
 * ----------------
 */
typedef struct BitmapOr
{
	Plan		plan;
	bool		isshared;
	List	   *bitmapplans;
} BitmapOr;

/*
 * ==========
 * Scan节点
 * ==========
 */
typedef struct Scan
{
	Plan		plan;
	Index		scanrelid;		/* relid是范围表中的索引 */
} Scan;

/* ----------------
 *		顺序扫描节点
 * ----------------
 */
typedef Scan SeqScan;

/* ----------------
 *		表采样扫描节点
 * ----------------
 */
typedef struct SampleScan
{
	Scan		scan;
	/* 使用结构体指针避免包含parsenodes.h */
	struct TableSampleClause *tablesample;
} SampleScan;

/* ----------------
 *		索引扫描节点
 *
 * indexqualorig是隐式AND的索引条件表达式列表，每个表达式与查询WHERE条件中的形式一致。
 * 每个表达式应为(indexkey OP comparisonval)或(comparisonval OP indexkey)形式。
 * indexkey是引用索引基表列的Var或表达式，comparisonval可以是任何表达式，但不会使用基表列。
 * 表达式按索引列顺序排列（但引用同一索引列的项顺序可任意）。indexqualorig仅在需要重新检查
 * lossy索引条件时在运行时使用。
 *
 * indexqual形式相同，但表达式已交换使indexkey在左侧，并且indexkey被替换为标识索引列的Var节点
 * （varno为INDEX_VAR，varattno为索引列号）。
 *
 * indexorderbyorig是由索引实现的ORDER BY表达式的原始形式，indexorderby则修改为左侧为索引列Var。
 * 多个表达式必须严格按ORDER BY顺序排列，不一定是索引列顺序。只提供表达式，不包含ORDER BY
 * SortGroupClauses的辅助排序信息；假定排序顺序可由顶层操作符完全确定。indexorderbyorig用于
 * 运行时重新检查排序（如果索引无法准确计算排序），也用于EXPLAIN。
 *
 * indexorderbyops是用于ORDER BY表达式排序的操作符OID列表。与indexorderbyorig一起用于运行时
 * 重新检查排序。（注意indexorderby、indexorderbyorig和indexorderbyops用于amcanorderbyop情况，
 * 而不是amcanorder。）
 *
 * indexorderdir指定扫描顺序，仅用于支持amcanorder的索引（其他索引则为“无关紧要”）。
 * ----------------
 */
typedef struct IndexScan
{
	Scan		scan;
	Oid			indexid;			/* 要扫描的索引OID */
	List	   *indexqual;			/* 索引条件列表（通常为OpExprs） */
	List	   *indexqualorig;		/* 原始形式的索引条件列表 */
	List	   *indexorderby;		/* 索引ORDER BY表达式列表 */
	List	   *indexorderbyorig;	/* 原始形式的ORDER BY表达式列表 */
	List	   *indexorderbyops;	/* ORDER BY表达式的排序操作符OID列表 */
	ScanDirection indexorderdir;	/* 扫描方向：前向/后向/无关紧要 */
} IndexScan;

/* ----------------
 *		索引仅扫描节点
 *
 * IndexOnlyScan与IndexScan非常类似，但指定为索引仅扫描，数据来自索引而非堆表。
 * 因此，计划节点的targetlist、qual和索引表达式中的所有Var都引用索引列且varno=INDEX_VAR。
 *
 * 理论上可以直接用indexqual对索引输出元组进行重新检查，但对于不可检索的索引列上的条件不行。
 * 因此需要recheckqual用于重新检查：它表达与indexqual相同的条件，但只使用可检索的索引列。
 * （如果不可行则不会生成索引仅扫描。例如，索引有表列"x"在可检索索引列"ind1"，还有表达式f(x)
 * 在不可检索列"ind2"，对f(x)的可索引查询会用"ind2"在indexqual，用f(ind1)在recheckqual。
 * 没有"ind1"则不允许索引仅扫描。）
 *
 * 当前不需要indexorderby的可重新检查版本，因为不支持ORDER BY中的lossy操作符。
 *
 * 为方便EXPLAIN解释索引Var，提供indextlist，表示索引内容的目标列表，每个索引列一个TLE。
 * 此列表中的Var引用基表，这是计划节点唯一可能包含此类Var的字段。且为方便setrefs.c，
 * indextlist中的TLE若对应索引AM无法重建的列则标记为resjunk。
 * ----------------
 */
typedef struct IndexOnlyScan
{
	Scan		scan;
	Oid			indexid;			/* 要扫描的索引OID */
	List	   *indexqual;			/* 索引条件列表（通常为OpExprs） */
	List	   *indexorderby;		/* 索引ORDER BY表达式列表 */
	List	   *indextlist;			/* 描述索引列的TargetEntry列表 */
	ScanDirection indexorderdir;	/* 扫描方向：前向/后向/无关紧要 */
	List	   *recheckqual;		/* 可重新检查形式的索引条件 */
} IndexOnlyScan;

/* ----------------
 *		位图索引扫描节点
 *
 * BitmapIndexScan 产生潜在元组位置的位图；
 * 它本身不会访问堆表。该位图会被上层的
 * BitmapHeapScan 节点使用，可能在经过中间的
 * BitmapAnd 和/或 BitmapOr 节点后，与其他
 * BitmapIndexScan 的结果合并。
 *
 * 字段含义与 IndexScan 相同，但不包含方向标志，
 * 因为方向在这里不重要。
 *
 * 在 BitmapIndexScan 计划节点中，targetlist 和 qual 字段
 * 未使用且总为 NIL。indexqualorig 字段在运行时也未使用，
 * 仅为 EXPLAIN 保留。
 * ----------------
 */
typedef struct BitmapIndexScan
{
	Scan		scan;
	Oid			indexid;		/* 要扫描的索引 OID */
	bool		isshared;		/* 若设置则创建共享位图 */
	List	   *indexqual;		/* 索引条件列表（OpExprs） */
	List	   *indexqualorig;	/* 原始形式的索引条件列表 */
} BitmapIndexScan;

/* ----------------
 *		位图顺序扫描节点
 *
 * 该节点需要保存输入索引扫描使用的条件表达式副本，
 * 因为在某些情况下需要重新检查这些条件；
 * 例如，当位图对页面上满足索引条件的具体行不精确时。
 * ----------------
 */
typedef struct BitmapHeapScan
{
	Scan		scan;
	List	   *bitmapqualorig; /* 索引条件，标准表达式形式 */
} BitmapHeapScan;

/* ----------------
 *		tid 扫描节点
 *
 * tidquals 是一个隐式 OR 的条件表达式列表，
 * 形式为 "CTID = 伪常量"，或 "CTID = ANY(伪常量数组)"，
 * 或关系的 CurrentOfExpr。
 * ----------------
 */
typedef struct TidScan
{
	Scan		scan;
	List	   *tidquals;		/* 包含 CTID = 某值 的条件列表 */
} TidScan;

/* ----------------
 *		subquery scan node
 *
 * SubqueryScan is for scanning the output of a sub-query in the range table.
 * We often need an extra plan node above the sub-query's plan to perform
 * expression evaluations (which we can't push into the sub-query without
 * risking changing its semantics).  Although we are not scanning a physical
 * relation, we make this a descendant of Scan anyway for code-sharing
 * purposes.
 *
 * Note: we store the sub-plan in the type-specific subplan field, not in
 * the generic lefttree field as you might expect.  This is because we do
 * not want plan-tree-traversal routines to recurse into the subplan without
 * knowing that they are changing Query contexts.
 * ----------------
 */
typedef struct SubqueryScan
{
	Scan		scan;
	Plan	   *subplan;
} SubqueryScan;

/* ----------------
 *		FunctionScan node
 * ----------------
 */
typedef struct FunctionScan
{
	Scan		scan;
	List	   *functions;		/* list of RangeTblFunction nodes */
	bool		funcordinality; /* WITH ORDINALITY */
} FunctionScan;

/* ----------------
 *		ValuesScan node
 * ----------------
 */
typedef struct ValuesScan
{
	Scan		scan;
	List	   *values_lists;	/* list of expression lists */
} ValuesScan;

/* ----------------
 *		TableFunc scan node
 * ----------------
 */
typedef struct TableFuncScan
{
	Scan		scan;
	TableFunc  *tablefunc;		/* table function node */
} TableFuncScan;

/* ----------------
 *		CteScan node
 * ----------------
 */
typedef struct CteScan
{
	Scan		scan;
	int			ctePlanId;		/* ID of init SubPlan for CTE */
	int			cteParam;		/* ID of Param representing CTE output */
} CteScan;

/* ----------------
 *		NamedTuplestoreScan node
 * ----------------
 */
typedef struct NamedTuplestoreScan
{
	Scan		scan;
	char	   *enrname;		/* Name given to Ephemeral Named Relation */
} NamedTuplestoreScan;

/* ----------------
 *		WorkTableScan node
 * ----------------
 */
typedef struct WorkTableScan
{
	Scan		scan;
	int			wtParam;		/* ID of Param representing work table */
} WorkTableScan;

/* ----------------
 *		ForeignScan node
 *
 * fdw_exprs and fdw_private are both under the control of the foreign-data
 * wrapper, but fdw_exprs is presumed to contain expression trees and will
 * be post-processed accordingly by the planner; fdw_private won't be.
 * Note that everything in both lists must be copiable by copyObject().
 * One way to store an arbitrary blob of bytes is to represent it as a bytea
 * Const.  Usually, though, you'll be better off choosing a representation
 * that can be dumped usefully by nodeToString().
 *
 * fdw_scan_tlist is a targetlist describing the contents of the scan tuple
 * returned by the FDW; it can be NIL if the scan tuple matches the declared
 * rowtype of the foreign table, which is the normal case for a simple foreign
 * table scan.  (If the plan node represents a foreign join, fdw_scan_tlist
 * is required since there is no rowtype available from the system catalogs.)
 * When fdw_scan_tlist is provided, Vars in the node's tlist and quals must
 * have varno INDEX_VAR, and their varattnos correspond to resnos in the
 * fdw_scan_tlist (which are also column numbers in the actual scan tuple).
 * fdw_scan_tlist is never actually executed; it just holds expression trees
 * describing what is in the scan tuple's columns.
 *
 * fdw_recheck_quals should contain any quals which the core system passed to
 * the FDW but which were not added to scan.plan.qual; that is, it should
 * contain the quals being checked remotely.  This is needed for correct
 * behavior during EvalPlanQual rechecks.
 *
 * When the plan node represents a foreign join, scan.scanrelid is zero and
 * fs_relids must be consulted to identify the join relation.  (fs_relids
 * is valid for simple scans as well, but will always match scan.scanrelid.)
 * ----------------
 */
typedef struct ForeignScan
{
	Scan		scan;
	CmdType		operation;		/* SELECT/INSERT/UPDATE/DELETE */
	Oid			fs_server;		/* OID of foreign server */
	List	   *fdw_exprs;		/* expressions that FDW may evaluate */
	List	   *fdw_private;	/* private data for FDW */
	List	   *fdw_scan_tlist; /* optional tlist describing scan tuple */
	List	   *fdw_recheck_quals;	/* original quals not in scan.plan.qual */
	Bitmapset  *fs_relids;		/* RTIs generated by this scan */
	bool		fsSystemCol;	/* true if any "system column" is needed */
} ForeignScan;

/* ----------------
 *	   CustomScan node
 *
 * The comments for ForeignScan's fdw_exprs, fdw_private, fdw_scan_tlist,
 * and fs_relids fields apply equally to CustomScan's custom_exprs,
 * custom_private, custom_scan_tlist, and custom_relids fields.  The
 * convention of setting scan.scanrelid to zero for joins applies as well.
 *
 * Note that since Plan trees can be copied, custom scan providers *must*
 * fit all plan data they need into those fields; embedding CustomScan in
 * a larger struct will not work.
 * ----------------
 */
struct CustomScanMethods;

typedef struct CustomScan
{
	Scan		scan;
	uint32		flags;			/* mask of CUSTOMPATH_* flags, see
								 * nodes/extensible.h */
	List	   *custom_plans;	/* list of Plan nodes, if any */
	List	   *custom_exprs;	/* expressions that custom code may evaluate */
	List	   *custom_private; /* private data for custom code */
	List	   *custom_scan_tlist;	/* optional tlist describing scan tuple */
	Bitmapset  *custom_relids;	/* RTIs generated by this scan */
	const struct CustomScanMethods *methods;
} CustomScan;

/*
 * ==========
 * Join节点
 * ==========
 */

/* ----------------
 *		Join节点
 *
 * jointype:	连接左右子树元组的规则
 * inner_unique: 每个外部元组最多只能匹配一个内部元组
 * joinqual:	来自JOIN/ON或JOIN/USING的连接条件
 *				(plan.qual包含来自WHERE的条件)
 *
 * 当jointype为INNER时，joinqual和plan.qual在语义上是可互换的。
 * 对于OUTER连接类型，两者不可互换；只有joinqual用于判断是否找到匹配，
 * 用于决定是否生成带NULL扩展的元组。（但plan.qual在实际返回元组前仍会应用）
 * 对于外连接，只有joinqual允许作为merge或hash连接的条件。
 *
 * inner_unique在连接条件保证每个外部元组最多只能匹配一个内部元组时设置为true，
 * 这样执行器可以跳过查找其他匹配项。（此属性必须仅由joinqual推导，而忽略plan.qual，
 * 因为执行器测试的位置不同）
 * ----------------
 */
typedef struct Join
{
	Plan		plan;
	JoinType	jointype;		/* 连接类型 */
	bool		inner_unique;	/* 是否每个外部元组最多匹配一个内部元组 */
	List	   *joinqual;		/* JOIN条件（除了plan.qual之外） */
} Join;

/* ----------------
 *		嵌套循环连接节点
 *
 * nestParams 列表标识需要传递给内层子计划的执行器参数（Params），
 * 这些参数值来自外层子计划当前行。当前这些值仅支持简单的 Var，
 * 但未来可能会放宽限制。（注意：在计划创建期间，paramval 可能是
 * PlaceHolderVar 表达式；但到达执行器时必须是 varno=OUTER_VAR 的 Var。）
 * ----------------
 */
typedef struct NestLoop
{
	Join		join;
	List	   *nestParams;		/* NestLoopParam 节点列表 */
} NestLoop;

typedef struct NestLoopParam
{
	NodeTag		type;
	int			paramno;		/* 要设置的 PARAM_EXEC 参数编号 */
	Var		   *paramval;		/* 要赋值给参数的外表 Var */
} NestLoopParam;

/* ----------------
 *		归并连接节点
 *
 * 每个可归并列的期望排序由 btree 操作符族 OID、排序规则 OID、
 * 排序方向（BTLessStrategyNumber 或 BTGreaterStrategyNumber）和
 * NULL 是否在前标志描述。注意归并子句两边的数据类型可以不同，
 * 但根据共同的操作符族和排序规则排序方式一致。每个归并子句的操作符
 * 必须是指定操作符族的等值操作符。
 * ----------------
 */
typedef struct MergeJoin
{
	Join		join;
	bool		skip_mark_restore;	/* 是否可以跳过 mark/restore 调用 */
	List	   *mergeclauses;	/* 归并子句表达式树列表 */
	/* 以下数组长度与 mergeclauses 列表一致： */
	Oid		   *mergeFamilies;	/* 每个子句的 btree 操作符族 OID */
	Oid		   *mergeCollations;	/* 每个子句的排序规则 OID */
	int		   *mergeStrategies;	/* 每个子句的排序方向（ASC/DESC） */
	bool	   *mergeNullsFirst;	/* 每个子句的 NULL 是否在前 */
} MergeJoin;

/* ----------------
 *		哈希连接节点
 * ----------------
 */
typedef struct HashJoin
{
	Join		join;
	List	   *hashclauses;		/* 哈希连接条件列表 */
	List	   *hashoperators;		/* 哈希操作符列表 */
	List	   *hashcollations;		/* 哈希排序规则列表 */

	/*
	 * 外表元组需要进行哈希的表达式列表，用于在内表哈希表中查找。
	 */
	List	   *hashkeys;
} HashJoin;

/* ----------------
 *		物化节点
 * ----------------
 */
typedef struct Material
{
	Plan		plan;
} Material;

/* ----------------
 *		排序节点
 * ----------------
 */
typedef struct Sort
{
	Plan		plan;
	int			numCols;		/* 排序键列的数量 */
	AttrNumber *sortColIdx;		/* 在目标列表中的索引 */
	Oid		   *sortOperators;	/* 用于排序的操作符OID */
	Oid		   *collations;		/* 排序规则的OID */
	bool	   *nullsFirst;		/* NULL值是否排在前面 */
} Sort;

/* ---------------
 *	 分组节点 -
 *		用于指定了 GROUP BY（但没有聚合函数）的查询。
 *		输入必须按照分组列预排序。
 * ---------------
 */
typedef struct Group
{
	Plan		plan;
	int			numCols;		/* 分组列的数量 */
	AttrNumber *grpColIdx;		/* 在目标列表中的索引 */
	Oid		   *grpOperators;	/* 用于比较的等值操作符 */
	Oid		   *grpCollations;
} Group;

/* ---------------
 *		aggregate node
 *
 * Agg节点实现了普通或分组聚合。对于分组聚合，可以处理预排序输入或无序输入；
 * 后者策略使用内部哈希表。
 *
 * 注意没有直接关于要计算的聚合函数的信息。它们会在执行器启动时通过扫描节点的
 * tlist和quals来找到。（有可能没有聚合函数；如果它们被常量折叠优化掉，或者
 * 我们用Agg节点实现基于哈希的分组时会发生这种情况。）
 * ---------------
 */
typedef struct Agg
{
	Plan		plan;
	AggStrategy aggstrategy;	/* 基本策略，见nodes.h */
	AggSplit	aggsplit;		/* 聚合拆分模式，见nodes.h */
	int			numCols;		/* 分组列的数量 */
	AttrNumber *grpColIdx;		/* 在目标列表中的索引 */
	Oid		   *grpOperators;	/* 用于比较的等值操作符 */
	Oid		   *grpCollations;
	long		numGroups;		/* 输入中估算的分组数 */
	Bitmapset  *aggParams;		/* Aggref输入中使用的Param的ID */
	/* 注意：planner只在HASHED/MIXED情况下提供numGroups和aggParams */
	List	   *groupingSets;	/* 要使用的分组集合 */
	List	   *chain;			/* 链式的Agg/Sort节点 */
} Agg;

/* ----------------
 *		window aggregate node
 * ----------------
 * WindowAgg节点实现窗口聚合。
 */
typedef struct WindowAgg
{
	Plan		plan;
	Index		winref;			/* 被窗口函数引用的ID */
	int			partNumCols;	/* 分区子句中的列数 */
	AttrNumber *partColIdx;		/* 在目标列表中的索引 */
	Oid		   *partOperators;	/* 分区列的等值操作符 */
	Oid		   *partCollations; /* 分区列的排序规则 */
	int			ordNumCols;		/* 排序子句中的列数 */
	AttrNumber *ordColIdx;		/* 在目标列表中的索引 */
	Oid		   *ordOperators;	/* 排序列的等值操作符 */
	Oid		   *ordCollations;	/* 排序列的排序规则 */
	int			frameOptions;	/* frame_clause选项，见WindowDef */
	Node	   *startOffset;	/* 起始边界的表达式（如有） */
	Node	   *endOffset;		/* 结束边界的表达式（如有） */
	/* 以下字段用于RANGE offset的PRECEDING/FOLLOWING： */
	Oid			startInRangeFunc;	/* 用于startOffset的in_range函数 */
	Oid			endInRangeFunc; /* 用于endOffset的in_range函数 */
	Oid			inRangeColl;	/* in_range测试的排序规则 */
	bool		inRangeAsc;		/* in_range测试是否使用升序排序？ */
	bool		inRangeNullsFirst;	/* in_range测试null是否排在前面？ */
} WindowAgg;

/* ----------------
 *		Unique 节点
 * ----------------
 */
typedef struct Unique
{
	Plan		plan;
	int			numCols;		/* 需要检查唯一性的列数 */
	AttrNumber *uniqColIdx;		/* 在目标列表中的索引 */
	Oid		   *uniqOperators;	/* 用于比较的等值操作符 */
	Oid		   *uniqCollations; /* 用于等值比较的排序规则 */
} Unique;

/* ------------
 *		Gather 节点
 *
 * 注意：rescan_param 是一个 PARAM_EXEC 参数槽的编号。该槽实际上不会存储值，
 * 但每次重新扫描 Gather 节点时必须标记该参数已变化。子节点中的并行感知扫描节点
 * 会依赖该参数，因此重新扫描机制能感知其输出可能变化。有时不需要 rescan Param，
 * 此时 rescan_param 设为 -1。
 * ------------
 */
typedef struct Gather
{
	Plan		plan;
	int			num_workers;	/* 计划使用的工作进程数 */
	int			rescan_param;	/* 标识重新扫描的 Param 编号，或 -1 */
	bool		single_copy;	/* 是否只执行一次计划 */
	bool		invisible;		/* 是否在 EXPLAIN 中隐藏（用于测试） */
	Bitmapset  *initParam;		/* 在 gather 或其子节点中引用的 initplan 参数编号集合 */
} Gather;

/* ------------
 *		GatherMerge 节点
 * ------------
 */
typedef struct GatherMerge
{
	Plan		plan;
	int			num_workers;	/* 计划使用的工作进程数 */
	int			rescan_param;	/* 标识重新扫描的 Param 编号，或 -1 */
	/* 以下字段与 Sort 结构体中的排序键信息一致： */
	int			numCols;		/* 排序键列的数量 */
	AttrNumber *sortColIdx;		/* 在目标列表中的索引 */
	Oid		   *sortOperators;	/* 用于排序的操作符OID */
	Oid		   *collations;		/* 排序规则的OID */
	bool	   *nullsFirst;		/* NULL值是否排在前面 */
	Bitmapset  *initParam;		/* 在 gather merge 或其子节点中引用的 initplan 参数编号集合 */
} GatherMerge;

/* ----------------
 *		hash build 节点
 *
 * 如果执行器需要尝试应用倾斜连接优化，则 skewTable/skewColumn/skewInherit
 * 标识外表连接键的列，可用于获取相关的MCV统计信息。
 * ----------------
 */
typedef struct Hash
{
	Plan		plan;

	/*
	 * 用于哈希连接条件的哈希键表达式列表，
	 * 用于将外表元组放入哈希表。
	 */
	List	   *hashkeys;		/* 哈希连接条件的哈希键 */
	Oid			skewTable;		/* 外连接键的表OID，或InvalidOid */
	AttrNumber	skewColumn;		/* 外连接键的列号，或0 */
	bool		skewInherit;	/* 外连接表是否为继承树 */
	/* 其他信息在父HashJoin节点中 */
	double		rows_total;		/* 如果并行感知，估算总行数 */
} Hash;

/* ----------------
 *		集合操作节点
 * ----------------
 */
typedef struct SetOp
{
	Plan		plan;
	SetOpCmd	cmd;			/* 操作类型，见nodes.h */
	SetOpStrategy strategy;		/* 执行策略，见nodes.h */
	int			numCols;		/* 检查重复性的列数 */
	AttrNumber *dupColIdx;		/* 在目标列表中的索引 */
	Oid		   *dupOperators;	/* 用于比较的等值操作符 */
	Oid		   *dupCollations;
	AttrNumber	flagColIdx;		/* 标志列的位置（如有） */
	int			firstFlag;		/* 第一个输入关系的标志值 */
	long		numGroups;		/* 输入中估算的分组数 */
} SetOp;

/* ----------------
 *		LockRows 节点
 *
 * rowMarks 标识该节点需要锁定的关系，应为顶层 PlannedStmt 中 rowMarks 的子集。
 * epqParam 是所有下层扫描节点必须依赖的 Param，用于在 EvalPlanQual 期间强制重新评估计划。
 * ----------------
 */
typedef struct LockRows
{
	Plan		plan;
	List	   *rowMarks;		/* PlanRowMark 节点列表 */
	int			epqParam;		/* EvalPlanQual 重新评估用的 Param 编号 */
} LockRows;

/* ----------------
 *		Limit 节点
 *
 * 注意：从 Postgres 8.2 起，offset 和 count 表达式应返回 int8 类型，而不是之前的 int4。
 * ----------------
 */
typedef struct Limit
{
	Plan		plan;
	Node	   *limitOffset;	/* OFFSET 参数，无则为 NULL */
	Node	   *limitCount;		/* COUNT 参数，无则为 NULL */
} Limit;


/*
 * RowMarkType -
 *	  行标记操作类型枚举
 *
 * 前四个值表示根据 SELECT FOR [KEY] UPDATE/SHARE 请求可对元组加锁的不同强度。
 * 这些操作支持普通表和支持延迟加锁的外部表。对于其他外部表，任何锁定都必须在初始行获取时完成，
 * 因此语义与本地表略有不同，可能会锁定更多行，但通常性能影响不大。
 *
 * 在 UPDATE、DELETE 或 SELECT FOR UPDATE/SHARE 时，必须唯一标识所有源行，
 * 以便在需要时执行 EvalPlanQual 重新检查。对于普通表可直接获取 TID（ROW_MARK_REFERENCE），
 * 对于 VALUES 或 FUNCTION 扫描则需复制整行（ROW_MARK_COPY），效率较低但通常影响不大。
 * 默认外部表使用 ROW_MARK_COPY，但如果 FDW 支持 rowid 可用 ROW_MARK_REFERENCE。
 */
typedef enum RowMarkType
{
	ROW_MARK_EXCLUSIVE,			/* 获取排他元组锁 */
	ROW_MARK_NOKEYEXCLUSIVE,	/* 获取无键排他元组锁 */
	ROW_MARK_SHARE,				/* 获取共享元组锁 */
	ROW_MARK_KEYSHARE,			/* 获取键共享元组锁 */
	ROW_MARK_REFERENCE,			/* 仅获取 TID，不加锁 */
	ROW_MARK_COPY				/* 物理复制整行数据 */
} RowMarkType;

#define RowMarkRequiresRowShareLock(marktype)  ((marktype) <= ROW_MARK_KEYSHARE)

/*
 * PlanRowMark -
 *	   计划时 FOR [KEY] UPDATE/SHARE 子句的表示
 *
 * 在 UPDATE、DELETE 或 SELECT FOR UPDATE/SHARE 时，为每个非目标关系创建一个 PlanRowMark 节点。
 * 未指定 FOR UPDATE/SHARE 的关系标记为 ROW_MARK_REFERENCE（普通表或支持的外部表）或 ROW_MARK_COPY（其他）。
 *
 * 初始所有 PlanRowMark 的 rti == prti 且 isParent == false。
 * 若关系为继承树根，则设置 isParent 为 true，并为每个子关系（包括目标关系自身）添加 PlanRowMark。
 * 子节点 rti == 子表 RT 索引，prti == 父表 RT 索引，可通过 prti != rti 识别为子节点。
 * 父节点的 allMarkTypes 字段为所有子节点 markType 的 OR。
 *
 * 计划器还会为计划添加 resjunk 输出列，用于标识锁定或获取的行。
 * markType != ROW_MARK_COPY 时，列名为 tableoid%u（表OID）、ctid%u（行TID），仅继承层次有 tableoid。
 * markType == ROW_MARK_COPY 时，列名为 wholerow%u（整行值）。
 * %u 为 rowmarkId，计划树内唯一，子节点复制父节点 rowmarkId。
 * 继承 UPDATE/DELETE 时，各子计划的列物理编号可能不同。
 */
typedef struct PlanRowMark
{
	NodeTag		type;
	Index		rti;			/* 可标记关系的范围表索引 */
	Index		prti;			/* 父关系的范围表索引 */
	Index		rowmarkId;		/* resjunk 列的唯一标识符 */
	RowMarkType markType;		/* 行标记类型，见上面枚举 */
	int			allMarkTypes;	/* 所有子节点 markType 的 OR */
	LockClauseStrength strength;	/* LockingClause 的强度，或 LCS_NONE */
	LockWaitPolicy waitPolicy;	/* NOWAIT 和 SKIP LOCKED 选项 */
	bool		isParent;		/* 是否为“虚拟”父节点 */
} PlanRowMark;


/*
 * Node types to represent partition pruning information.
 */

/*
 * PartitionPruneInfo - Details required to allow the executor to prune
 * partitions.
 *
 * Here we store mapping details to allow translation of a partitioned table's
 * index as returned by the partition pruning code into subplan indexes for
 * plan types which support arbitrary numbers of subplans, such as Append.
 * We also store various details to tell the executor when it should be
 * performing partition pruning.
 *
 * Each PartitionedRelPruneInfo describes the partitioning rules for a single
 * partitioned table (a/k/a level of partitioning).  Since a partitioning
 * hierarchy could contain multiple levels, we represent it by a List of
 * PartitionedRelPruneInfos, where the first entry represents the topmost
 * partitioned table and additional entries represent non-leaf child
 * partitions, ordered such that parents appear before their children.
 * Then, since an Append-type node could have multiple partitioning
 * hierarchies among its children, we have an unordered List of those Lists.
 *
 * prune_infos			List of Lists containing PartitionedRelPruneInfo nodes,
 *						one sublist per run-time-prunable partition hierarchy
 *						appearing in the parent plan node's subplans.
 * other_subplans		Indexes of any subplans that are not accounted for
 *						by any of the PartitionedRelPruneInfo nodes in
 *						"prune_infos".  These subplans must not be pruned.
 */
typedef struct PartitionPruneInfo
{
	NodeTag		type;
	List	   *prune_infos;
	Bitmapset  *other_subplans;
} PartitionPruneInfo;

/*
 * PartitionedRelPruneInfo - Details required to allow the executor to prune
 * partitions for a single partitioned table.
 *
 * subplan_map[] and subpart_map[] are indexed by partition index of the
 * partitioned table referenced by 'rtindex', the partition index being the
 * order that the partitions are defined in the table's PartitionDesc.  For a
 * leaf partition p, subplan_map[p] contains the zero-based index of the
 * partition's subplan in the parent plan's subplan list; it is -1 if the
 * partition is non-leaf or has been pruned.  For a non-leaf partition p,
 * subpart_map[p] contains the zero-based index of that sub-partition's
 * PartitionedRelPruneInfo in the hierarchy's PartitionedRelPruneInfo list;
 * it is -1 if the partition is a leaf or has been pruned.  Note that subplan
 * indexes, as stored in 'subplan_map', are global across the parent plan
 * node, but partition indexes are valid only within a particular hierarchy.
 * relid_map[p] contains the partition's OID, or 0 if the partition was pruned.
 */
typedef struct PartitionedRelPruneInfo
{
	NodeTag		type;
	Index		rtindex;		/* RT index of partition rel for this level */
	Bitmapset  *present_parts;	/* Indexes of all partitions which subplans or
								 * subparts are present for */
	int			nparts;			/* Length of the following arrays: */
	int		   *subplan_map;	/* subplan index by partition index, or -1 */
	int		   *subpart_map;	/* subpart index by partition index, or -1 */
	Oid		   *relid_map;		/* relation OID by partition index, or 0 */

	/*
	 * initial_pruning_steps shows how to prune during executor startup (i.e.,
	 * without use of any PARAM_EXEC Params); it is NIL if no startup pruning
	 * is required.  exec_pruning_steps shows how to prune with PARAM_EXEC
	 * Params; it is NIL if no per-scan pruning is required.
	 */
	List	   *initial_pruning_steps;	/* List of PartitionPruneStep */
	List	   *exec_pruning_steps; /* List of PartitionPruneStep */
	Bitmapset  *execparamids;	/* All PARAM_EXEC Param IDs in
								 * exec_pruning_steps */
} PartitionedRelPruneInfo;

/*
 * Abstract Node type for partition pruning steps (there are no concrete
 * Nodes of this type).
 *
 * step_id is the global identifier of the step within its pruning context.
 */
typedef struct PartitionPruneStep
{
	NodeTag		type;
	int			step_id;
} PartitionPruneStep;

/*
 * PartitionPruneStepOp - Information to prune using a set of mutually AND'd
 *							OpExpr clauses
 *
 * This contains information extracted from up to partnatts OpExpr clauses,
 * where partnatts is the number of partition key columns.  'opstrategy' is the
 * strategy of the operator in the clause matched to the last partition key.
 * 'exprs' contains expressions which comprise the lookup key to be passed to
 * the partition bound search function.  'cmpfns' contains the OIDs of
 * comparison functions used to compare aforementioned expressions with
 * partition bounds.  Both 'exprs' and 'cmpfns' contain the same number of
 * items, up to partnatts items.
 *
 * Once we find the offset of a partition bound using the lookup key, we
 * determine which partitions to include in the result based on the value of
 * 'opstrategy'.  For example, if it were equality, we'd return just the
 * partition that would contain that key or a set of partitions if the key
 * didn't consist of all partitioning columns.  For non-equality strategies,
 * we'd need to include other partitions as appropriate.
 *
 * 'nullkeys' is the set containing the offset of the partition keys (0 to
 * partnatts - 1) that were matched to an IS NULL clause.  This is only
 * considered for hash partitioning as we need to pass which keys are null
 * to the hash partition bound search function.  It is never possible to
 * have an expression be present in 'exprs' for a given partition key and
 * the corresponding bit set in 'nullkeys'.
 */
typedef struct PartitionPruneStepOp
{
	PartitionPruneStep step;

	StrategyNumber opstrategy;
	List	   *exprs;
	List	   *cmpfns;
	Bitmapset  *nullkeys;
} PartitionPruneStepOp;

/*
 * PartitionPruneStepCombine - Information to prune using a BoolExpr clause
 *
 * For BoolExpr clauses, we combine the set of partitions determined for each
 * of the argument clauses.
 */
typedef enum PartitionPruneCombineOp
{
	PARTPRUNE_COMBINE_UNION,
	PARTPRUNE_COMBINE_INTERSECT
} PartitionPruneCombineOp;

typedef struct PartitionPruneStepCombine
{
	PartitionPruneStep step;

	PartitionPruneCombineOp combineOp;
	List	   *source_stepids;
} PartitionPruneStepCombine;


/*
 * Plan invalidation info
 *
 * We track the objects on which a PlannedStmt depends in two ways:
 * relations are recorded as a simple list of OIDs, and everything else
 * is represented as a list of PlanInvalItems.  A PlanInvalItem is designed
 * to be used with the syscache invalidation mechanism, so it identifies a
 * system catalog entry by cache ID and hash value.
 */
typedef struct PlanInvalItem
{
	NodeTag		type;
	int			cacheId;		/* a syscache ID, see utils/syscache.h */
	uint32		hashValue;		/* hash value of object's cache lookup key */
} PlanInvalItem;

#endif							/* PLANNODES_H */
