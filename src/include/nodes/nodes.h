/*-------------------------------------------------------------------------
 *
 * nodes.h
 *	  Definitions for tagged nodes.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/nodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODES_H
#define NODES_H

 /*
	* The first field of every node is NodeTag. Each node created (with makeNode)
	* will have one of the following tags as the value of its first field.
	*
	* Note that inserting or deleting node types changes the numbers of other
	* node types later in the list.  This is no problem during development, since
	* the node numbers are never stored on disk.  But don't do it in a released
	* branch, because that would represent an ABI break for extensions.
	*
	* 中文注释：
	* 每个节点的第一个字段都是 NodeTag。通过 makeNode 创建的节点其第一个字段
	* 的值就是下面枚举中的某个标签。不要在已发布分支中随意插入或删除枚举项，
	* 否则会造成二进制兼容性（ABI）中断。
	*/
typedef enum NodeTag
{
	T_Invalid = 0,				/* 无效标签 */

	/*
	 * TAGS FOR EXECUTOR NODES (execnodes.h)
	 * 执行器相关节点标签
	 */
	T_IndexInfo,
	T_ExprContext,
	T_ProjectionInfo,
	T_JunkFilter,
	T_OnConflictSetState,
	T_ResultRelInfo,
	T_EState,
	T_TupleTableSlot,

	/*
	 * TAGS FOR PLAN NODES (plannodes.h)
	 * 计划节点标签（Plan 及其子类）
	 */
	T_Plan,
	T_Result,
	T_ProjectSet,
	T_ModifyTable,
	T_Append,
	T_MergeAppend,
	T_RecursiveUnion,
	T_BitmapAnd,
	T_BitmapOr,
	T_Scan,
	T_SeqScan,
	T_SampleScan,
	T_IndexScan,
	T_IndexOnlyScan,
	T_BitmapIndexScan,
	T_BitmapHeapScan,
	T_TidScan,
	T_SubqueryScan,
	T_FunctionScan,
	T_ValuesScan,
	T_TableFuncScan,
	T_CteScan,
	T_NamedTuplestoreScan,
	T_WorkTableScan,
	T_ForeignScan,
	T_CustomScan,
	T_Join,
	T_NestLoop,
	T_MergeJoin,
	T_HashJoin,
	T_Material,
	T_Sort,
	T_Group,
	T_Agg,
	T_WindowAgg,
	T_Unique,
	T_Gather,
	T_GatherMerge,
	T_Hash,
	T_SetOp,
	T_LockRows,
	T_Limit,
	/* these aren't subclasses of Plan: */
	T_NestLoopParam,
	T_PlanRowMark,
	T_PartitionPruneInfo,
	T_PartitionedRelPruneInfo,
	T_PartitionPruneStepOp,
	T_PartitionPruneStepCombine,
	T_PlanInvalItem,

	/*
	 * TAGS FOR PLAN STATE NODES (execnodes.h)
	 *
	 * These should correspond one-to-one with Plan node types.
	 * 计划状态节点，通常与 Plan 节点一一对应
	 */
	T_PlanState,
	T_ResultState,
	T_ProjectSetState,
	T_ModifyTableState,
	T_AppendState,
	T_MergeAppendState,
	T_RecursiveUnionState,
	T_BitmapAndState,
	T_BitmapOrState,
	T_ScanState,
	T_SeqScanState,
	T_SampleScanState,
	T_IndexScanState,
	T_IndexOnlyScanState,
	T_BitmapIndexScanState,
	T_BitmapHeapScanState,
	T_TidScanState,
	T_SubqueryScanState,
	T_FunctionScanState,
	T_TableFuncScanState,
	T_ValuesScanState,
	T_CteScanState,
	T_NamedTuplestoreScanState,
	T_WorkTableScanState,
	T_ForeignScanState,
	T_CustomScanState,
	T_JoinState,
	T_NestLoopState,
	T_MergeJoinState,
	T_HashJoinState,
	T_MaterialState,
	T_SortState,
	T_GroupState,
	T_AggState,
	T_WindowAggState,
	T_UniqueState,
	T_GatherState,
	T_GatherMergeState,
	T_HashState,
	T_SetOpState,
	T_LockRowsState,
	T_LimitState,

	/*
	 * TAGS FOR PRIMITIVE NODES (primnodes.h)
	 * 基本表达式与语法节点
	 */
	T_Alias,
	T_RangeVar,
	T_TableFunc,
	T_Expr,
	T_Var,
	T_Const,
	T_Param,
	T_Aggref,
	T_GroupingFunc,
	T_WindowFunc,
	T_SubscriptingRef,
	T_FuncExpr,
	T_NamedArgExpr,
	T_OpExpr,
	T_DistinctExpr,
	T_NullIfExpr,
	T_ScalarArrayOpExpr,
	T_BoolExpr,
	T_SubLink,
	T_SubPlan,
	T_AlternativeSubPlan,
	T_FieldSelect,
	T_FieldStore,
	T_RelabelType,
	T_CoerceViaIO,
	T_ArrayCoerceExpr,
	T_ConvertRowtypeExpr,
	T_CollateExpr,
	T_CaseExpr,
	T_CaseWhen,
	T_CaseTestExpr,
	T_ArrayExpr,
	T_RowExpr,
	T_RowCompareExpr,
	T_CoalesceExpr,
	T_MinMaxExpr,
	T_SQLValueFunction,
	T_XmlExpr,
	T_NullTest,
	T_BooleanTest,
	T_CoerceToDomain,
	T_CoerceToDomainValue,
	T_SetToDefault,
	T_CurrentOfExpr,
	T_NextValueExpr,
	T_InferenceElem,
	T_TargetEntry,
	T_RangeTblRef,
	T_JoinExpr,
	T_FromExpr,
	T_OnConflictExpr,
	T_IntoClause,

	/*
	 * TAGS FOR EXPRESSION STATE NODES (execnodes.h)
	 *
	 * 表示表达式求值时的运行时状态节点
	 */
	T_ExprState,
	T_AggrefExprState,
	T_WindowFuncExprState,
	T_SetExprState,
	T_SubPlanState,
	T_AlternativeSubPlanState,
	T_DomainConstraintState,

	/*
	 * TAGS FOR PLANNER NODES (pathnodes.h)
	 * 规划器相关节点（路径、代价估算等）
	 */
	T_PlannerInfo,
	T_PlannerGlobal,
	T_RelOptInfo,
	T_IndexOptInfo,
	T_ForeignKeyOptInfo,
	T_ParamPathInfo,
	T_Path,
	T_IndexPath,
	T_BitmapHeapPath,
	T_BitmapAndPath,
	T_BitmapOrPath,
	T_TidPath,
	T_SubqueryScanPath,
	T_ForeignPath,
	T_CustomPath,
	T_NestPath,
	T_MergePath,
	T_HashPath,
	T_AppendPath,
	T_MergeAppendPath,
	T_GroupResultPath,
	T_MaterialPath,
	T_UniquePath,
	T_GatherPath,
	T_GatherMergePath,
	T_ProjectionPath,
	T_ProjectSetPath,
	T_SortPath,
	T_GroupPath,
	T_UpperUniquePath,
	T_AggPath,
	T_GroupingSetsPath,
	T_MinMaxAggPath,
	T_WindowAggPath,
	T_SetOpPath,
	T_RecursiveUnionPath,
	T_LockRowsPath,
	T_ModifyTablePath,
	T_LimitPath,
	/* these aren't subclasses of Path: */
	T_EquivalenceClass,
	T_EquivalenceMember,
	T_PathKey,
	T_PathTarget,
	T_RestrictInfo,
	T_IndexClause,
	T_PlaceHolderVar,
	T_SpecialJoinInfo,
	T_AppendRelInfo,
	T_PlaceHolderInfo,
	T_MinMaxAggInfo,
	T_PlannerParamItem,
	T_RollupData,
	T_GroupingSetData,
	T_StatisticExtInfo,

	/*
	 * TAGS FOR MEMORY NODES (memnodes.h)
	 * 内存上下文相关节点
	 */
	T_MemoryContext,
	T_AllocSetContext,
	T_SlabContext,
	T_GenerationContext,

	/*
	 * TAGS FOR VALUE NODES (value.h)
	 * 值节点（字面量等）
	 */
	T_Value,
	T_Integer,
	T_Float,
	T_String,
	T_BitString,
	T_Null,

	/*
	 * TAGS FOR LIST NODES (pg_list.h)
	 * 列表节点类型
	 */
	T_List,
	T_IntList,
	T_OidList,

	/*
	 * TAGS FOR EXTENSIBLE NODES (extensible.h)
	 * 可扩展节点
	 */
	T_ExtensibleNode,

	/*
	 * TAGS FOR STATEMENT NODES (mostly in parsenodes.h)
	 * 语句节点（大多数在 parsenodes.h）
	 */
	T_RawStmt,
	T_Query,
	T_PlannedStmt,
	T_InsertStmt,
	T_DeleteStmt,
	T_UpdateStmt,
	T_SelectStmt,
	T_AlterTableStmt,
	T_AlterTableCmd,
	T_AlterDomainStmt,
	T_SetOperationStmt,
	T_GrantStmt,
	T_GrantRoleStmt,
	T_AlterDefaultPrivilegesStmt,
	T_ClosePortalStmt,
	T_ClusterStmt,
	T_CopyStmt,
	T_CreateStmt,
	T_DefineStmt,
	T_DropStmt,
	T_TruncateStmt,
	T_CommentStmt,
	T_FetchStmt,
	T_IndexStmt,
	T_CreateFunctionStmt,
	T_AlterFunctionStmt,
	T_DoStmt,
	T_RenameStmt,
	T_RuleStmt,
	T_NotifyStmt,
	T_ListenStmt,
	T_UnlistenStmt,
	T_TransactionStmt,
	T_ViewStmt,
	T_LoadStmt,
	T_CreateDomainStmt,
	T_CreatedbStmt,
	T_DropdbStmt,
	T_VacuumStmt,
	T_ExplainStmt,
	T_CreateTableAsStmt,
	T_CreateSeqStmt,
	T_AlterSeqStmt,
	T_VariableSetStmt,
	T_VariableShowStmt,
	T_DiscardStmt,
	T_CreateTrigStmt,
	T_CreatePLangStmt,
	T_CreateRoleStmt,
	T_AlterRoleStmt,
	T_DropRoleStmt,
	T_LockStmt,
	T_ConstraintsSetStmt,
	T_ReindexStmt,
	T_CheckPointStmt,
	T_CreateSchemaStmt,
	T_AlterDatabaseStmt,
	T_AlterDatabaseSetStmt,
	T_AlterRoleSetStmt,
	T_CreateConversionStmt,
	T_CreateCastStmt,
	T_CreateOpClassStmt,
	T_CreateOpFamilyStmt,
	T_AlterOpFamilyStmt,
	T_PrepareStmt,
	T_ExecuteStmt,
	T_DeallocateStmt,
	T_DeclareCursorStmt,
	T_CreateTableSpaceStmt,
	T_DropTableSpaceStmt,
	T_AlterObjectDependsStmt,
	T_AlterObjectSchemaStmt,
	T_AlterOwnerStmt,
	T_AlterOperatorStmt,
	T_DropOwnedStmt,
	T_ReassignOwnedStmt,
	T_CompositeTypeStmt,
	T_CreateEnumStmt,
	T_CreateRangeStmt,
	T_AlterEnumStmt,
	T_AlterTSDictionaryStmt,
	T_AlterTSConfigurationStmt,
	T_CreateFdwStmt,
	T_AlterFdwStmt,
	T_CreateForeignServerStmt,
	T_AlterForeignServerStmt,
	T_CreateUserMappingStmt,
	T_AlterUserMappingStmt,
	T_DropUserMappingStmt,
	T_AlterTableSpaceOptionsStmt,
	T_AlterTableMoveAllStmt,
	T_SecLabelStmt,
	T_CreateForeignTableStmt,
	T_ImportForeignSchemaStmt,
	T_CreateExtensionStmt,
	T_AlterExtensionStmt,
	T_AlterExtensionContentsStmt,
	T_CreateEventTrigStmt,
	T_AlterEventTrigStmt,
	T_RefreshMatViewStmt,
	T_ReplicaIdentityStmt,
	T_AlterSystemStmt,
	T_CreatePolicyStmt,
	T_AlterPolicyStmt,
	T_CreateTransformStmt,
	T_CreateAmStmt,
	T_CreatePublicationStmt,
	T_AlterPublicationStmt,
	T_CreateSubscriptionStmt,
	T_AlterSubscriptionStmt,
	T_DropSubscriptionStmt,
	T_CreateStatsStmt,
	T_AlterCollationStmt,
	T_CallStmt,

	/*
	 * TAGS FOR PARSE TREE NODES (parsenodes.h)
	 * 解析树节点
	 */
	T_A_Expr,
	T_ColumnRef,
	T_ParamRef,
	T_A_Const,
	T_FuncCall,
	T_A_Star,
	T_A_Indices,
	T_A_Indirection,
	T_A_ArrayExpr,
	T_ResTarget,
	T_MultiAssignRef,
	T_TypeCast,
	T_CollateClause,
	T_SortBy,
	T_WindowDef,
	T_RangeSubselect,
	T_RangeFunction,
	T_RangeTableSample,
	T_RangeTableFunc,
	T_RangeTableFuncCol,
	T_TypeName,
	T_ColumnDef,
	T_IndexElem,
	T_Constraint,
	T_DefElem,
	T_RangeTblEntry,
	T_RangeTblFunction,
	T_TableSampleClause,
	T_WithCheckOption,
	T_SortGroupClause,
	T_GroupingSet,
	T_WindowClause,
	T_ObjectWithArgs,
	T_AccessPriv,
	T_CreateOpClassItem,
	T_TableLikeClause,
	T_FunctionParameter,
	T_LockingClause,
	T_RowMarkClause,
	T_XmlSerialize,
	T_WithClause,
	T_InferClause,
	T_OnConflictClause,
	T_CommonTableExpr,
	T_RoleSpec,
	T_TriggerTransition,
	T_PartitionElem,
	T_PartitionSpec,
	T_PartitionBoundSpec,
	T_PartitionRangeDatum,
	T_PartitionCmd,
	T_VacuumRelation,

	/*
	 * TAGS FOR REPLICATION GRAMMAR PARSE NODES (replnodes.h)
	 * 复制相关的解析节点
	 */
	T_IdentifySystemCmd,
	T_BaseBackupCmd,
	T_CreateReplicationSlotCmd,
	T_DropReplicationSlotCmd,
	T_StartReplicationCmd,
	T_TimeLineHistoryCmd,
	T_SQLCmd,

	/*
	 * TAGS FOR RANDOM OTHER STUFF
	 *
	 * These are objects that aren't part of parse/plan/execute node tree
	 * structures, but we give them NodeTags anyway for identification
	 * purposes (usually because they are involved in APIs where we want to
	 * pass multiple object types through the same pointer).
	 *
	 * 其他杂项对象，也使用 NodeTag 标识以便统一 API 传递
	 */
	T_TriggerData,				/* in commands/trigger.h */
	T_EventTriggerData,			/* in commands/event_trigger.h */
	T_ReturnSetInfo,			/* in nodes/execnodes.h */
	T_WindowObjectData,			/* private in nodeWindowAgg.c */
	T_TIDBitmap,				/* in nodes/tidbitmap.h */
	T_InlineCodeBlock,			/* in nodes/parsenodes.h */
	T_FdwRoutine,				/* in foreign/fdwapi.h */
	T_IndexAmRoutine,			/* in access/amapi.h */
	T_TableAmRoutine,			/* in access/tableam.h */
	T_TsmRoutine,				/* in access/tsmapi.h */
	T_ForeignKeyCacheInfo,		/* in utils/rel.h */
	T_CallContext,				/* in nodes/parsenodes.h */
	T_SupportRequestSimplify,	/* in nodes/supportnodes.h */
	T_SupportRequestSelectivity,	/* in nodes/supportnodes.h */
	T_SupportRequestCost,		/* in nodes/supportnodes.h */
	T_SupportRequestRows,		/* in nodes/supportnodes.h */
	T_SupportRequestIndexCondition	/* in nodes/supportnodes.h */
} NodeTag;

/*
 * 首字段保证为 NodeTag。将任意节点强制转换为 Node 即可获取其类型。
 * 将变量声明为 Node *（而不是 void *）有助于调试。
 */
typedef struct Node
{
	NodeTag		type;		/* node type tag / 节点类型标识 */
} Node;

#define nodeTag(nodeptr)		(((const Node*)(nodeptr))->type)

/*
 * newNode -
 *	  create a new node of the specified size and tag the node with the
 *	  specified tag.
 *
 * !WARNING!: Avoid using newNode directly. You should be using the
 *	  macro makeNode.  eg. to create a Query node, use makeNode(Query)
 *
 * Note: the size argument should always be a compile-time constant, so the
 * apparent risk of multiple evaluation doesn't matter in practice.
 */
#ifdef __GNUC__

 /* With GCC, we can use a compound statement within an expression */
#define newNode(size, tag) \
({	Node   *_result; \
	AssertMacro((size) >= sizeof(Node));		/* need the tag, at least */ \
	_result = (Node *) palloc0fast(size); \
	_result->type = (tag); \
	_result; \
})
#else

 /*
  *	There is no way to dereference the palloc'ed pointer to assign the
  *	tag, and also return the pointer itself, so we need a holder variable.
  *	Fortunately, this macro isn't recursive so we just define
  *	a global variable for this purpose.
  */
extern PGDLLIMPORT Node* newNodeMacroHolder;

#define newNode(size, tag) \
( \
	AssertMacro((size) >= sizeof(Node)),		/* need the tag, at least */ \
	newNodeMacroHolder = (Node *) palloc0fast(size), \
	newNodeMacroHolder->type = (tag), \
	newNodeMacroHolder \
)
#endif							/* __GNUC__ */


/*
 * makeNode(_type_)
 *   宏，用于分配并初始化指定类型的新节点。
 *   使用 newNode() 分配内存并设置节点类型标签。
 *
 * NodeSetTag(nodeptr, t)
 *   宏，将节点的类型标签设置为指定值。
 *
 * IsA(nodeptr, _type_)
 *   宏，判断节点是否为指定类型。
 *   如果节点类型标签等于 T__type_，则返回 true。
 */
#define makeNode(_type_)		((_type_ *) newNode(sizeof(_type_),T_##_type_))
#define NodeSetTag(nodeptr,t)	(((Node*)(nodeptr))->type = (t))

#define IsA(nodeptr,_type_)		(nodeTag(nodeptr) == T_##_type_)

/*
 * castNode(type, ptr) 将ptr强制转换为"type *"，并且在启用断言的情况下，
 * 验证节点具有适当的类型（使用其nodeTag()方法）。
 *
 * 在启用断言时使用内联函数，以避免ptr参数的多次求值（例如，ptr可能是一个函数调用）。
 */
#ifdef USE_ASSERT_CHECKING

/* 
 * castNodeImpl - 类型转换的内联实现函数
 * @type: 目标节点类型标签
 * @ptr: 要转换的节点指针
 * @return: 转换后的节点指针
 * 
 * 这个内联函数在断言启用时使用，确保类型安全的节点转换
 */
static inline Node*
castNodeImpl(NodeTag type, void* ptr)
{
    /* 
     * 断言检查：如果指针不为NULL，则验证其节点标签是否与目标类型匹配
     * 这确保了在开发环境中进行严格的类型检查
     */
    Assert(ptr == NULL || nodeTag(ptr) == type);
    /* 执行实际的类型转换并返回 */
    return (Node*)ptr;
}

/* 
 * castNode 宏定义 - 在断言启用情况下
 * @_type_: 目标节点类型名称
 * @nodeptr: 要转换的节点指针
 * 
 * 使用内联函数castNodeImpl执行带类型检查的转换
 * T_##_type_ 是一个宏展开，生成对应类型的枚举常量
 */
#define castNode(_type_, nodeptr) ((_type_ *) castNodeImpl(T_##_type_, nodeptr))

#else

/* 
 * castNode 宏定义 - 在断言禁用情况下
 * @_type_: 目标节点类型名称
 * @nodeptr: 要转换的节点指针
 * 
 * 在生产环境中，直接执行简单的指针转换，不进行类型检查
 * 这提高了执行效率，但牺牲了运行时类型安全性
 */
#define castNode(_type_, nodeptr) ((_type_ *) (nodeptr))

#endif							/* USE_ASSERT_CHECKING */



 /* ----------------------------------------------------------------
  *					  extern declarations follow
  * ----------------------------------------------------------------
  */

  /*
   * nodes/{outfuncs.c,print.c}
   */
struct Bitmapset;				/* not to include bitmapset.h here */
struct StringInfoData;			/* not to include stringinfo.h here */

extern void outNode(struct StringInfoData* str, const void* obj);
extern void outToken(struct StringInfoData* str, const char* s);
extern void outBitmapset(struct StringInfoData* str,
	const struct Bitmapset* bms);
extern void outDatum(struct StringInfoData* str, uintptr_t value,
	int typlen, bool typbyval);
extern char* nodeToString(const void* obj);
extern char* bmsToString(const struct Bitmapset* bms);

/*
 * nodes/{readfuncs.c,read.c}
 */
extern void* stringToNode(const char* str);
#ifdef WRITE_READ_PARSE_PLAN_TREES
extern void* stringToNodeWithLocations(const char* str);
#endif
extern struct Bitmapset* readBitmapset(void);
extern uintptr_t readDatum(bool typbyval);
extern bool* readBoolCols(int numCols);
extern int* readIntCols(int numCols);
extern Oid* readOidCols(int numCols);
extern int16* readAttrNumberCols(int numCols);

/*
 * nodes/copyfuncs.c
 */
extern void* copyObjectImpl(const void* obj);

/* 
 * copyObject 宏定义
 * 
 * 此宏提供了一个类型安全的对象深拷贝机制，是PostgreSQL内部对象系统的核心函数之一。
 * 它会调用copyObjectImpl函数进行实际的对象复制操作，并根据编译器支持情况进行适当的类型转换。
 * 
 * 实现原理：
 * 1. 对于支持typeof运算符的编译器（如GCC、Clang等），使用typeof获取参数类型
 *    并将复制结果显式转换回原始类型，提供完整的类型安全性。
 * 2. 对于不支持typeof的编译器，直接返回copyObjectImpl的结果，可能需要调用者进行手动类型转换。
 * 
 * copyObjectImpl是实际执行对象复制的函数，通常会根据对象的节点类型标签(node tag)执行相应的复制逻辑。
 * 这种设计使得PostgreSQL能够实现一个统一的复制接口，同时处理各种不同类型的节点对象。
 */
/* 如果编译器支持，将结果转换回参数类型 */
#ifdef HAVE_TYPEOF
/* 当编译器支持typeof时，使用typeof获取参数类型并进行类型安全的转换 */
#define copyObject(obj) ((typeof(obj)) copyObjectImpl(obj))
#else
/* 当编译器不支持typeof时，直接返回复制结果，可能需要调用者手动进行类型转换 */
#define copyObject(obj) copyObjectImpl(obj)
#endif


/*
 * nodes/equalfuncs.c
 */
extern bool equal(const void* a, const void* b);


/*
 * Typedefs for identifying qualifier selectivities and plan costs as such.
 * These are just plain "double"s, but declaring a variable as Selectivity
 * or Cost makes the intent more obvious.
 *
 * These could have gone into plannodes.h or some such, but many files
 * depend on them...
 */
typedef double Selectivity;		/* fraction of tuples a qualifier will pass */
typedef double Cost;			/* execution cost (in page-access units) */


/*
 * CmdType -
 *	  enums for type of operation represented by a Query or PlannedStmt
 *
 * This is needed in both parsenodes.h and plannodes.h, so put it here...
 */
typedef enum CmdType
{
	CMD_UNKNOWN,
	CMD_SELECT,					/* select stmt */
	CMD_UPDATE,					/* update stmt */
	CMD_INSERT,					/* insert stmt */
	CMD_DELETE,
	CMD_UTILITY,				/* cmds like create, destroy, copy, vacuum,
								 * etc. */
	CMD_NOTHING					/* dummy command for instead nothing rules
								 * with qual */
} CmdType;


/*
 * JoinType -
 *	  关系连接类型的枚举，用于 JoinExpr、JoinPath、Join 节点
 *
 * JoinType 决定使用匹配谓词连接两个关系时的精确语义。例如，它决定对于在另一侧
 * 没有匹配的元组应如何处理。
 *
 * 这个枚举在 parsenodes.h 和 plannodes.h 中都需要，所以放在此处...
 */
typedef enum JoinType
{
	/*
	 * 根据 SQL JOIN 语法的规范连接类型。只有这些代码可以出现在解析器输出
	 *（例如 JoinExpr 节点）中。
	 */
	JOIN_INNER,					/* 仅包含匹配的元组对 */
	JOIN_LEFT,					/* 包含匹配对 + 左侧未匹配元组 */
	JOIN_FULL,					/* 包含匹配对 + 左侧未匹配 + 右侧未匹配 */
	JOIN_RIGHT,					/* 包含匹配对 + 右侧未匹配元组 */

	/*
	 * 半连接（semijoin）和反半连接（antijoin）在关系理论中有明确定义，但不
	 * 出现在 SQL JOIN 语法中。不过可以通过常见语法习惯（例如 EXISTS）来表示它们。
	 * 规划器会识别这些情况并将其转换为连接。因此规划器和执行器必须支持这些代码。
	 *
	 * 注意：对于 JOIN_SEMI，哪个匹配的 RHS 行与之连接是不确定的（只保证有一份
	 * 左侧行被输出）。对于 JOIN_ANTI，输出的行保证右侧部分为 NULL 扩展。
	 */
	JOIN_SEMI,					/* 对于有匹配的每个左侧行，返回一份 */
	JOIN_ANTI,					/* 对于没有匹配的每个左侧行，返回一份（右侧扩展为 NULL） */

	/*
	 * 这些代码在规划器内部使用，但执行器（以及大多数规划器代码）并不支持它们。
	 */
	JOIN_UNIQUE_OUTER,			/* 需要使左侧路径唯一 */
	JOIN_UNIQUE_INNER			/* 需要使右侧路径唯一 */

	/*
	 * 将来可能需要额外的连接类型。
	 */
} JoinType;

/*
 * IS_OUTER_JOIN - 判断连接类型是否为外连接
 *
 * 外连接是指那些下推的限定条件必须与连接自身限定条件表现不同的连接类型。
 * 实际上除了内连接(INNER)和半连接(SEMI)之外的所有连接都是外连接。
 * 但是，这个宏还必须排除JOIN_UNIQUE符号，因为它们只是最终将成为内连接的临时代理。
 *
 * 注意：半连接是一个混合情况，但我们选择将其视为非外连接。
 * 这主要是可以接受的，因为SQL语法使得不可能存在引用半连接内关系的下推限定条件；
 * 因此没有必要强烈区分连接限定条件和下推限定条件。
 * 这很方便，因为几乎所有情况下，附加到半连接的限定条件都可以像内连接限定条件一样处理。
 *
 * 参数说明:
 * - jointype: 连接类型枚举值(JOIN_LEFT, JOIN_FULL, JOIN_RIGHT, JOIN_ANTI等)
 *
 * 返回值:
 * - 布尔值：如果是外连接则返回true，否则返回false
 *
 * 工作原理:
 * 使用位运算检查jointype是否属于外连接类型集合。
 * 通过将jointype左移一位生成位掩码，然后与外连接类型的位掩码进行按位与操作，
 * 如果结果不为0则说明是外连接类型。
 *
 * 外连接类型包括:
 * - JOIN_LEFT: 左外连接
 * - JOIN_FULL: 全外连接
 * - JOIN_RIGHT: 右外连接
 * - JOIN_ANTI: 反连接
 *
 * 非外连接类型:
 * - JOIN_INNER: 内连接
 * - JOIN_SEMI: 半连接
 * - JOIN_UNIQUE_*: 唯一化连接(临时代理)
 */
#define IS_OUTER_JOIN(jointype) \
	(((1 << (jointype)) & \
	  ((1 << JOIN_LEFT) | \
	   (1 << JOIN_FULL) | \
	   (1 << JOIN_RIGHT) | \
	   (1 << JOIN_ANTI))) != 0)


/*
 * AggStrategy -
 *	  聚合节点的整体执行策略枚举
 *
 * 该枚举用于 Agg 计划节点，表示不同的聚合执行方式。
 * 在 pathnodes.h 和 plannodes.h 中都需要，所以放在此处。
 */
typedef enum AggStrategy
{
	AGG_PLAIN,		/* 简单聚合，针对所有输入行进行汇总 */
	AGG_SORTED,		/* 分组聚合，输入必须已排序 */
	AGG_HASHED,		/* 分组聚合，使用内部哈希表 */
	AGG_MIXED		/* 分组聚合，同时使用哈希和排序 */
} AggStrategy;

/*
 * AggSplit -
 *	  Agg 计划节点的拆分（部分聚合）模式
 *
 * 该枚举在 pathnodes.h 和 plannodes.h 中都需要，所以放在此处...
 */

/* nodeAgg.c 支持的基本选项: */
#define AGGSPLITOP_COMBINE		0x01	/* 用 combinefn 替换 transfn */
#define AGGSPLITOP_SKIPFINAL	0x02	/* 跳过 finalfn，直接返回状态 */
#define AGGSPLITOP_SERIALIZE	0x04	/* 对输出应用 serializefn */
#define AGGSPLITOP_DESERIALIZE	0x08	/* 对输入应用 deserializefn */

/* 支持的操作模式（即这些选项的有用组合）: */
typedef enum AggSplit
{
	/* 基本的非拆分聚合: */
	AGGSPLIT_SIMPLE = 0,
	/* 部分聚合的初始阶段，带序列化: */
	AGGSPLIT_INITIAL_SERIAL = AGGSPLITOP_SKIPFINAL | AGGSPLITOP_SERIALIZE,
	/* 部分聚合的最终阶段，带反序列化: */
	AGGSPLIT_FINAL_DESERIAL = AGGSPLITOP_COMBINE | AGGSPLITOP_DESERIALIZE
} AggSplit;

/* Test whether an AggSplit value selects each primitive option: */
/* 
 * 检查给定的 AggSplit 值是否包含 COMBINE 操作选项
 * 参数：
 *   as - AggSplit 枚举值
 * 返回：
 *   布尔值，表示是否使用 combinefn 替换 transfn（即合并聚合状态）
 * 实现原理：
 *   通过按位与操作检查 AGGSPLITOP_COMBINE 位标志是否设置
 */
#define DO_AGGSPLIT_COMBINE(as)		(((as) & AGGSPLITOP_COMBINE) != 0)

/* 
 * 检查给定的 AggSplit 值是否包含 SKIPFINAL 操作选项
 * 参数：
 *   as - AggSplit 枚举值
 * 返回：
 *   布尔值，表示是否跳过 finalfn，直接返回聚合状态
 * 实现原理：
 *   通过按位与操作检查 AGGSPLITOP_SKIPFINAL 位标志是否设置
 */
#define DO_AGGSPLIT_SKIPFINAL(as)	(((as) & AGGSPLITOP_SKIPFINAL) != 0)

/* 
 * 检查给定的 AggSplit 值是否包含 SERIALIZE 操作选项
 * 参数：
 *   as - AggSplit 枚举值
 * 返回：
 *   布尔值，表示是否对聚合输出应用 serializefn 进行序列化
 * 实现原理：
 *   通过按位与操作检查 AGGSPLITOP_SERIALIZE 位标志是否设置
 */
#define DO_AGGSPLIT_SERIALIZE(as)	(((as) & AGGSPLITOP_SERIALIZE) != 0)

/* 
 * 检查给定的 AggSplit 值是否包含 DESERIALIZE 操作选项
 * 参数：
 *   as - AggSplit 枚举值
 * 返回：
 *   布尔值，表示是否对聚合输入应用 deserializefn 进行反序列化
 * 实现原理：
 *   通过按位与操作检查 AGGSPLITOP_DESERIALIZE 位标志是否设置
 */
#define DO_AGGSPLIT_DESERIALIZE(as) (((as) & AGGSPLITOP_DESERIALIZE) != 0)


/*
 * SetOpCmd and SetOpStrategy -
 *	  overall semantics and execution strategies for SetOp plan nodes
 *
 * This is needed in both pathnodes.h and plannodes.h, so put it here...
 */
typedef enum SetOpCmd
{
	SETOPCMD_INTERSECT,
	SETOPCMD_INTERSECT_ALL,
	SETOPCMD_EXCEPT,
	SETOPCMD_EXCEPT_ALL
} SetOpCmd;

typedef enum SetOpStrategy
{
	SETOP_SORTED,				/* input must be sorted */
	SETOP_HASHED				/* use internal hashtable */
} SetOpStrategy;

/*
 * OnConflictAction -
 *	  "ON CONFLICT" clause type of query
 *
 * This is needed in both parsenodes.h and plannodes.h, so put it here...
 */
typedef enum OnConflictAction
{
	ONCONFLICT_NONE,			/* No "ON CONFLICT" clause */
	ONCONFLICT_NOTHING,			/* ON CONFLICT ... DO NOTHING */
	ONCONFLICT_UPDATE			/* ON CONFLICT ... DO UPDATE */
} OnConflictAction;

#endif							/* NODES_H */
