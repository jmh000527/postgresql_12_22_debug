/*-------------------------------------------------------------------------
 *
 * primnodes.h
 *	  Definitions for "primitive" node types, those that are used in more
 *	  than one of the parse/plan/execute stages of the query pipeline.
 *	  Currently, these are mostly nodes for executable expressions
 *	  and join trees.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/primnodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PRIMNODES_H
#define PRIMNODES_H

#include "access/attnum.h"
#include "nodes/bitmapset.h"
#include "nodes/pg_list.h"


 /* ----------------------------------------------------------------
  *						node definitions
  * ----------------------------------------------------------------
  */

  /*
   * Alias -
   *	  specifies an alias for a range variable; the alias might also
   *	  specify renaming of columns within the table.
   *
   * Note: colnames is a list of Value nodes (always strings).  In Alias structs
   * associated with RTEs, there may be entries corresponding to dropped
   * columns; these are normally empty strings ("").  See parsenodes.h for info.
   */
typedef struct Alias
{
	NodeTag		type;
	char* aliasname;		/* aliased rel name (never qualified) */
	List* colnames;		/* optional list of column aliases */
} Alias;

/* What to do at commit time for temporary relations */
typedef enum OnCommitAction
{
	ONCOMMIT_NOOP,				/* No ON COMMIT clause (do nothing) */
	ONCOMMIT_PRESERVE_ROWS,		/* ON COMMIT PRESERVE ROWS (do nothing) */
	ONCOMMIT_DELETE_ROWS,		/* ON COMMIT DELETE ROWS */
	ONCOMMIT_DROP				/* ON COMMIT DROP */
} OnCommitAction;

/*
 * RangeVar - range variable, used in FROM clauses
 *
 * Also used to represent table names in utility statements; there, the alias
 * field is not used, and inh tells whether to apply the operation
 * recursively to child tables.  In some contexts it is also useful to carry
 * a TEMP table indication here.
 */
typedef struct RangeVar
{
	NodeTag		type;
	char* catalogname;	/* the catalog (database) name, or NULL */
	char* schemaname;		/* the schema name, or NULL */
	char* relname;		/* the relation/sequence name */
	bool		inh;			/* expand rel by inheritance? recursively act
								 * on children? */
	char		relpersistence; /* see RELPERSISTENCE_* in pg_class.h */
	Alias* alias;			/* table alias & optional column aliases */
	int			location;		/* token location, or -1 if unknown */
} RangeVar;

/*
 * TableFunc - node for a table function, such as XMLTABLE.
 *
 * Entries in the ns_names list are either string Value nodes containing
 * literal namespace names, or NULL pointers to represent DEFAULT.
 */
typedef struct TableFunc
{
	NodeTag		type;
	List* ns_uris;		/* list of namespace URI expressions */
	List* ns_names;		/* list of namespace names or NULL */
	Node* docexpr;		/* input document expression */
	Node* rowexpr;		/* row filter expression */
	List* colnames;		/* column names (list of String) */
	List* coltypes;		/* OID list of column type OIDs */
	List* coltypmods;		/* integer list of column typmods */
	List* colcollations;	/* OID list of column collation OIDs */
	List* colexprs;		/* list of column filter expressions */
	List* coldefexprs;	/* list of column default expressions */
	Bitmapset* notnulls;		/* nullability flag for each output column */
	int			ordinalitycol;	/* counts from 0; -1 if none specified */
	int			location;		/* token location, or -1 if unknown */
} TableFunc;

/*
 * IntoClause - target information for SELECT INTO, CREATE TABLE AS, and
 * CREATE MATERIALIZED VIEW
 *
 * For CREATE MATERIALIZED VIEW, viewQuery is the parsed-but-not-rewritten
 * SELECT Query for the view; otherwise it's NULL.  (Although it's actually
 * Query*, we declare it as Node* to avoid a forward reference.)
 */
typedef struct IntoClause
{
	NodeTag		type;

	RangeVar* rel;			/* target relation name */
	List* colNames;		/* column names to assign, or NIL */
	char* accessMethod;	/* table access method */
	List* options;		/* options from WITH clause */
	OnCommitAction onCommit;	/* what do we do at COMMIT? */
	char* tableSpaceName; /* table space to use, or NULL */
	Node* viewQuery;		/* materialized view's SELECT query */
	bool		skipData;		/* true for WITH NO DATA */
} IntoClause;


/* ----------------------------------------------------------------
 *					node types for executable expressions
 * ----------------------------------------------------------------
 */

 /*
	* Expr - 可执行表达式节点的通用基类
	*
	* 所有在可执行表达式树中使用的节点类型都应该派生自 Expr
	*（即将 Expr 作为其第一个字段）。由于 Expr 仅包含 NodeTag，
	*这在技术上只是形式上的要求，但它是便于文档说明的约定。
	*另请参见 execnodes.h 中的 ExprState 节点类型。
	*/
typedef struct Expr
{
	NodeTag		type;
} Expr;

/*
 * Var - 表示变量（即表的列）的表达式节点
 *
 * 注意：在解析/规划阶段，varnoold/varoattno 始终只是 varno/varattno 的副本。
 * 在规划的最后阶段，出现在上层计划节点中的 Var 节点会被重新指派为指向
 * 其子计划的输出；例如，在连接节点中，varno 会变为 INNER_VAR 或 OUTER_VAR，
 * 而 varattno 则变为该子计划目标列表中相应元素的索引。类似地，INDEX_VAR
 * 用于标识引用索引列而不是堆列的 Vars。（在 ForeignScan 和 CustomScan
 * 计划节点中，INDEX_VAR 被用于表示对自定义扫描元组类型的列的引用。）
 * 在所有这些情况下，varnoold/varoattno 保存原始值。代码本身并不真正需要
 * varnoold/varoattno，但它们对调试和解释已完成的计划非常有用，所以我们保留它们。
 */
#define    INNER_VAR		65000	/* 引用内部子计划 */
#define    OUTER_VAR		65001	/* 引用外部子计划 */
#define    INDEX_VAR		65002	/* 引用索引列 */

#define IS_SPECIAL_VARNO(varno)		((varno) >= INNER_VAR)

 /* 规则中用于特殊 RTE 条目的索引符号 */
#define    PRS2_OLD_VARNO			1
#define    PRS2_NEW_VARNO			2

typedef struct Var
{
	Expr		xpr;
	Index		varno;			/* 此 var 在 rangetable 中所指关系的索引，
								 * 或者为 INNER_VAR/OUTER_VAR/INDEX_VAR
								 * 列属性所在的表的编号，源自 Query 中的 rtable 编号 rtindex*/
	AttrNumber	varattno;		/* 此 var 的属性编号，标识该列属性是表中的第几列；为 0 表示所有属性
								 * （“整行 Var”） */
	Oid			vartype;		/* 此 var （列属性）的数据类型的 pg_type OID */
	int32		vartypmod;		/* 列属性的精度（长度），pg_attribute 的 typmod 值 */
	Oid			varcollid;		/* 校对规则（collation）的 OID；若无则为 InvalidOid */
	Index		varlevelsup;	/* 对引用外层关系的子查询变量：普通 var 为 0，
								 * 大于 0 表示向上 N 层 */
	Index		varnoold;		/* varno 的原始值（用于调试） */
	AttrNumber	varoattno;		/* varattno 的原始值 */
	int			location;		/* 列属性出现在 SQL 中的词法标记位置；未知时为 -1 */
} Var;

/*
 * Const
 *
 * Note: for varlena data types, we make a rule that a Const node's value
 * must be in non-extended form (4-byte header, no compression or external
 * references).  This ensures that the Const node is self-contained and makes
 * it more likely that equal() will see logically identical values as equal.
 */
typedef struct Const
{
	Expr		xpr;
	Oid			consttype;		/* pg_type OID of the constant's datatype */
	int32		consttypmod;	/* typmod value, if any */
	Oid			constcollid;	/* OID of collation, or InvalidOid if none */
	int			constlen;		/* typlen of the constant's datatype */
	Datum		constvalue;		/* the constant's value */
	bool		constisnull;	/* whether the constant is null (if true,
								 * constvalue is undefined) */
	bool		constbyval;		/* whether this datatype is passed by value.
								 * If true, then all the information is stored
								 * in the Datum. If false, then the Datum
								 * contains a pointer to the information. */
	int			location;		/* token location, or -1 if unknown */
} Const;

/*
 * Param
 *
 *		paramkind specifies the kind of parameter. The possible values
 *		for this field are:
 *
 *		PARAM_EXTERN:  The parameter value is supplied from outside the plan.
 *				Such parameters are numbered from 1 to n.
 *
 *		PARAM_EXEC:  The parameter is an internal executor parameter, used
 *				for passing values into and out of sub-queries or from
 *				nestloop joins to their inner scans.
 *				For historical reasons, such parameters are numbered from 0.
 *				These numbers are independent of PARAM_EXTERN numbers.
 *
 *		PARAM_SUBLINK:	The parameter represents an output column of a SubLink
 *				node's sub-select.  The column number is contained in the
 *				`paramid' field.  (This type of Param is converted to
 *				PARAM_EXEC during planning.)
 *
 *		PARAM_MULTIEXPR:  Like PARAM_SUBLINK, the parameter represents an
 *				output column of a SubLink node's sub-select, but here, the
 *				SubLink is always a MULTIEXPR SubLink.  The high-order 16 bits
 *				of the `paramid' field contain the SubLink's subLinkId, and
 *				the low-order 16 bits contain the column number.  (This type
 *				of Param is also converted to PARAM_EXEC during planning.)
 */
typedef enum ParamKind
{
	PARAM_EXTERN,
	PARAM_EXEC,
	PARAM_SUBLINK,
	PARAM_MULTIEXPR
} ParamKind;

typedef struct Param
{
	Expr		xpr;
	ParamKind	paramkind;		/* kind of parameter. See above */
	int			paramid;		/* numeric ID for parameter */
	Oid			paramtype;		/* pg_type OID of parameter's datatype */
	int32		paramtypmod;	/* typmod value, if known */
	Oid			paramcollid;	/* OID of collation, or InvalidOid if none */
	int			location;		/* token location, or -1 if unknown */
} Param;

/*
 * Aggref
 *
 * The aggregate's args list is a targetlist, ie, a list of TargetEntry nodes.
 *
 * For a normal (non-ordered-set) aggregate, the non-resjunk TargetEntries
 * represent the aggregate's regular arguments (if any) and resjunk TLEs can
 * be added at the end to represent ORDER BY expressions that are not also
 * arguments.  As in a top-level Query, the TLEs can be marked with
 * ressortgroupref indexes to let them be referenced by SortGroupClause
 * entries in the aggorder and/or aggdistinct lists.  This represents ORDER BY
 * and DISTINCT operations to be applied to the aggregate input rows before
 * they are passed to the transition function.  The grammar only allows a
 * simple "DISTINCT" specifier for the arguments, but we use the full
 * query-level representation to allow more code sharing.
 *
 * For an ordered-set aggregate, the args list represents the WITHIN GROUP
 * (aggregated) arguments, all of which will be listed in the aggorder list.
 * DISTINCT is not supported in this case, so aggdistinct will be NIL.
 * The direct arguments appear in aggdirectargs (as a list of plain
 * expressions, not TargetEntry nodes).
 *
 * aggtranstype is the data type of the state transition values for this
 * aggregate (resolved to an actual type, if agg's transtype is polymorphic).
 * This is determined during planning and is InvalidOid before that.
 *
 * aggargtypes is an OID list of the data types of the direct and regular
 * arguments.  Normally it's redundant with the aggdirectargs and args lists,
 * but in a combining aggregate, it's not because the args list has been
 * replaced with a single argument representing the partial-aggregate
 * transition values.
 *
 * aggsplit indicates the expected partial-aggregation mode for the Aggref's
 * parent plan node.  It's always set to AGGSPLIT_SIMPLE in the parser, but
 * the planner might change it to something else.  We use this mainly as
 * a crosscheck that the Aggrefs match the plan; but note that when aggsplit
 * indicates a non-final mode, aggtype reflects the transition data type
 * not the SQL-level output type of the aggregate.
 */
typedef struct Aggref
{
	Expr		xpr;
	Oid			aggfnoid;		/* pg_proc Oid of the aggregate */
	Oid			aggtype;		/* type Oid of result of the aggregate */
	Oid			aggcollid;		/* OID of collation of result */
	Oid			inputcollid;	/* OID of collation that function should use */
	Oid			aggtranstype;	/* type Oid of aggregate's transition value */
	List* aggargtypes;	/* type Oids of direct and aggregated args */
	List* aggdirectargs;	/* direct arguments, if an ordered-set agg */
	List* args;			/* aggregated arguments and sort expressions */
	List* aggorder;		/* ORDER BY (list of SortGroupClause) */
	List* aggdistinct;	/* DISTINCT (list of SortGroupClause) */
	Expr* aggfilter;		/* FILTER expression, if any */
	bool		aggstar;		/* true if argument list was really '*' */
	bool		aggvariadic;	/* true if variadic arguments have been
								 * combined into an array last argument */
	char		aggkind;		/* aggregate kind (see pg_aggregate.h) */
	Index		agglevelsup;	/* > 0 if agg belongs to outer query */
	AggSplit	aggsplit;		/* expected agg-splitting mode of parent Agg */
	int			location;		/* token location, or -1 if unknown */
} Aggref;

/*
 * GroupingFunc
 *
 * A GroupingFunc is a GROUPING(...) expression, which behaves in many ways
 * like an aggregate function (e.g. it "belongs" to a specific query level,
 * which might not be the one immediately containing it), but also differs in
 * an important respect: it never evaluates its arguments, they merely
 * designate expressions from the GROUP BY clause of the query level to which
 * it belongs.
 *
 * The spec defines the evaluation of GROUPING() purely by syntactic
 * replacement, but we make it a real expression for optimization purposes so
 * that one Agg node can handle multiple grouping sets at once.  Evaluating the
 * result only needs the column positions to check against the grouping set
 * being projected.  However, for EXPLAIN to produce meaningful output, we have
 * to keep the original expressions around, since expression deparse does not
 * give us any feasible way to get at the GROUP BY clause.
 *
 * Also, we treat two GroupingFunc nodes as equal if they have equal arguments
 * lists and agglevelsup, without comparing the refs and cols annotations.
 *
 * In raw parse output we have only the args list; parse analysis fills in the
 * refs list, and the planner fills in the cols list.
 */
typedef struct GroupingFunc
{
	Expr		xpr;
	List* args;			/* arguments, not evaluated but kept for
								 * benefit of EXPLAIN etc. */
	List* refs;			/* ressortgrouprefs of arguments */
	List* cols;			/* actual column positions set by planner */
	Index		agglevelsup;	/* same as Aggref.agglevelsup */
	int			location;		/* token location */
} GroupingFunc;

/*
 * WindowFunc
 */
typedef struct WindowFunc
{
	Expr		xpr;
	Oid			winfnoid;		/* pg_proc Oid of the function */
	Oid			wintype;		/* type Oid of result of the window function */
	Oid			wincollid;		/* OID of collation of result */
	Oid			inputcollid;	/* OID of collation that function should use */
	List* args;			/* arguments to the window function */
	Expr* aggfilter;		/* FILTER expression, if any */
	Index		winref;			/* index of associated WindowClause */
	bool		winstar;		/* true if argument list was really '*' */
	bool		winagg;			/* is function a simple aggregate? */
	int			location;		/* token location, or -1 if unknown */
} WindowFunc;

/* ----------------
 *	SubscriptingRef: describes a subscripting operation over a container
 *			(array, etc).
 *
 * A SubscriptingRef can describe fetching a single element from a container,
 * fetching a part of container (e.g. array slice), storing a single element into
 * a container, or storing a slice.  The "store" cases work with an
 * initial container value and a source value that is inserted into the
 * appropriate part of the container; the result of the operation is an
 * entire new modified container value.
 *
 * If reflowerindexpr = NIL, then we are fetching or storing a single container
 * element at the subscripts given by refupperindexpr. Otherwise we are
 * fetching or storing a container slice, that is a rectangular subcontainer
 * with lower and upper bounds given by the index expressions.
 * reflowerindexpr must be the same length as refupperindexpr when it
 * is not NIL.
 *
 * In the slice case, individual expressions in the subscript lists can be
 * NULL, meaning "substitute the array's current lower or upper bound".
 *
 * Note: the result datatype is the element type when fetching a single
 * element; but it is the array type when doing subarray fetch or either
 * type of store.
 *
 * Note: for the cases where a container is returned, if refexpr yields a R/W
 * expanded container, then the implementation is allowed to modify that object
 * in-place and return the same object.)
 * ----------------
 */
typedef struct SubscriptingRef
{
	Expr		xpr;
	Oid			refcontainertype;	/* type of the container proper */
	Oid			refelemtype;	/* type of the container elements */
	int32		reftypmod;		/* typmod of the container (and elements too) */
	Oid			refcollid;		/* OID of collation, or InvalidOid if none */
	List* refupperindexpr;	/* expressions that evaluate to upper
									 * container indexes */
	List* reflowerindexpr;	/* expressions that evaluate to lower
									 * container indexes, or NIL for single
									 * container element */
	Expr* refexpr;		/* the expression that evaluates to a
								 * container value */

	Expr* refassgnexpr;	/* expression for the source value, or NULL if
								 * fetch */
} SubscriptingRef;

/*
 * CoercionContext - distinguishes the allowed set of type casts
 *
 * NB: ordering of the alternatives is significant; later (larger) values
 * allow more casts than earlier ones.
 */
typedef enum CoercionContext
{
	COERCION_IMPLICIT,			/* coercion in context of expression */
	COERCION_ASSIGNMENT,		/* coercion in context of assignment */
	COERCION_EXPLICIT			/* explicit cast operation */
} CoercionContext;

/*
 * CoercionForm - how to display a node that could have come from a cast
 *
 * NB: equal() ignores CoercionForm fields, therefore this *must* not carry
 * any semantically significant information.  We need that behavior so that
 * the planner will consider equivalent implicit and explicit casts to be
 * equivalent.  In cases where those actually behave differently, the coercion
 * function's arguments will be different.
 */
typedef enum CoercionForm
{
	COERCE_EXPLICIT_CALL,		/* display as a function call */
	COERCE_EXPLICIT_CAST,		/* display as an explicit cast */
	COERCE_IMPLICIT_CAST		/* implicit cast, so hide it */
} CoercionForm;

/*
 * FuncExpr - expression node for a function call
 */
typedef struct FuncExpr
{
	Expr		xpr;
	Oid			funcid;			/* PG_PROC OID of the function */
	Oid			funcresulttype; /* PG_TYPE OID of result value */
	bool		funcretset;		/* true if function returns set */
	bool		funcvariadic;	/* true if variadic arguments have been
								 * combined into an array last argument */
	CoercionForm funcformat;	/* how to display this function call */
	Oid			funccollid;		/* OID of collation of result */
	Oid			inputcollid;	/* OID of collation that function should use */
	List* args;			/* arguments to the function */
	int			location;		/* token location, or -1 if unknown */
} FuncExpr;

/*
 * NamedArgExpr - a named argument of a function
 *
 * This node type can only appear in the args list of a FuncCall or FuncExpr
 * node.  We support pure positional call notation (no named arguments),
 * named notation (all arguments are named), and mixed notation (unnamed
 * arguments followed by named ones).
 *
 * Parse analysis sets argnumber to the positional index of the argument,
 * but doesn't rearrange the argument list.
 *
 * The planner will convert argument lists to pure positional notation
 * during expression preprocessing, so execution never sees a NamedArgExpr.
 */
typedef struct NamedArgExpr
{
	Expr		xpr;
	Expr* arg;			/* the argument expression */
	char* name;			/* the name */
	int			argnumber;		/* argument's number in positional notation */
	int			location;		/* argument name location, or -1 if unknown */
} NamedArgExpr;

/*
 * OpExpr - expression node for an operator invocation
 *
 * Semantically, this is essentially the same as a function call.
 *
 * Note that opfuncid is not necessarily filled in immediately on creation
 * of the node.  The planner makes sure it is valid before passing the node
 * tree to the executor, but during parsing/planning opfuncid can be 0.
 */
typedef struct OpExpr
{
	Expr		xpr;
	Oid			opno;			/* PG_OPERATOR OID of the operator */
	Oid			opfuncid;		/* PG_PROC OID of underlying function */
	Oid			opresulttype;	/* PG_TYPE OID of result value */
	bool		opretset;		/* true if operator returns set */
	Oid			opcollid;		/* OID of collation of result */
	Oid			inputcollid;	/* OID of collation that operator should use */
	List* args;			/* arguments to the operator (1 or 2) */
	int			location;		/* token location, or -1 if unknown */
} OpExpr;

/*
 * DistinctExpr - expression node for "x IS DISTINCT FROM y"
 *
 * Except for the nodetag, this is represented identically to an OpExpr
 * referencing the "=" operator for x and y.
 * We use "=", not the more obvious "<>", because more datatypes have "="
 * than "<>".  This means the executor must invert the operator result.
 * Note that the operator function won't be called at all if either input
 * is NULL, since then the result can be determined directly.
 */
typedef OpExpr DistinctExpr;

/*
 * NullIfExpr - a NULLIF expression
 *
 * Like DistinctExpr, this is represented the same as an OpExpr referencing
 * the "=" operator for x and y.
 */
typedef OpExpr NullIfExpr;

/*
 * ScalarArrayOpExpr - expression node for "scalar op ANY/ALL (array)"
 *
 * The operator must yield boolean.  It is applied to the left operand
 * and each element of the righthand array, and the results are combined
 * with OR or AND (for ANY or ALL respectively).  The node representation
 * is almost the same as for the underlying operator, but we need a useOr
 * flag to remember whether it's ANY or ALL, and we don't have to store
 * the result type (or the collation) because it must be boolean.
 */
typedef struct ScalarArrayOpExpr
{
	Expr		xpr;
	Oid			opno;			/* PG_OPERATOR OID of the operator */
	Oid			opfuncid;		/* PG_PROC OID of underlying function */
	bool		useOr;			/* true for ANY, false for ALL */
	Oid			inputcollid;	/* OID of collation that operator should use */
	List* args;			/* the scalar and array operands */
	int			location;		/* token location, or -1 if unknown */
} ScalarArrayOpExpr;

/*
 * BoolExpr - expression node for the basic Boolean operators AND, OR, NOT
 *
 * Notice the arguments are given as a List.  For NOT, of course the list
 * must always have exactly one element.  For AND and OR, there can be two
 * or more arguments.
 */
typedef enum BoolExprType
{
	AND_EXPR, OR_EXPR, NOT_EXPR
} BoolExprType;

typedef struct BoolExpr
{
	Expr		xpr;
	BoolExprType boolop;
	List* args;			/* arguments to this expression */
	int			location;		/* token location, or -1 if unknown */
} BoolExpr;

/*
 * SubLink
 *
 * SubLink 表示表达式中出现的子查询（subselect），在某些情况下还包括其上方的组合操作符。
 * subLinkType 指示表达式的具体形式：
 *	EXISTS_SUBLINK		EXISTS(SELECT ...)
 *	ALL_SUBLINK			(lefthand) op ALL (SELECT ...)
 *	ANY_SUBLINK			(lefthand) op ANY (SELECT ...)
 *	ROWCOMPARE_SUBLINK	(lefthand) op (SELECT ...)
 *	EXPR_SUBLINK		(SELECT ...，仅有一个目标列)
 *	MULTIEXPR_SUBLINK	(SELECT ...，有多个目标列)
 *	ARRAY_SUBLINK		ARRAY(SELECT ...，仅有一个目标列)
 *	CTE_SUBLINK			WITH 查询（实际上不会出现在表达式中）
 *
 * 对于 ALL、ANY 和 ROWCOMPARE，lefthand 是与子查询目标列数量相同的表达式列表。
 * ROWCOMPARE 一定是多于一个目标列；若仅有一个目标列，则解析器会创建 EXPR_SUBLINK。
 * ROWCOMPARE、EXPR 和 MULTIEXPR 要求子查询最多返回一行（无行则结果为 NULL）。
 * ALL、ANY 和 ROWCOMPARE 要求组合操作符返回布尔值，ALL 用 AND 语义，ANY 用 OR 语义。
 * ARRAY 只要求一个目标列，结果为该类型的数组。
 *
 * SubLink 被归类为 Expr 节点，但实际不可执行，规划阶段需替换为 SubPlan 节点。
 *
 * 注意：在 gram.y 的原始输出中，testexpr 仅为 lefthand 的原始表达式，operName 为操作符名字符串，
 * subselect 为原始语法树。在解析分析阶段，testexpr 会被转换为完整的布尔表达式，比较 lefthand
 * 与 PARAM_SUBLINK 节点（代表子查询输出列），subselect 转为 Query 节点。
 *
 * 对于 EXISTS、EXPR、MULTIEXPR 和 ARRAY 类型，testexpr 和 operName 均未使用，始终为 NULL。
 *
 * subLinkId 仅用于 MULTIEXPR 类型，其他类型为 0。该编号用于标识 UPDATE 语句 SET 列表中的
 * 多重赋值子查询，仅在特定目标列表内唯一。MULTIEXPR 的输出列可被 tlist 中的 PARAM_MULTIEXPR 引用。
 *
 * CTE_SUBLINK 不会出现在实际 SubLink 节点，仅用于为 WITH 子查询生成的 SubPlan。
 */
typedef enum SubLinkType
{
	EXISTS_SUBLINK,		/* EXISTS(SELECT ...) */
	ALL_SUBLINK,		/* (lefthand) op ALL (SELECT ...) */
	ANY_SUBLINK,		/* (lefthand) op ANY (SELECT ...) */
	ROWCOMPARE_SUBLINK,	/* (lefthand) op (SELECT ...) */
	EXPR_SUBLINK,		/* (SELECT ...，单一目标列) */
	MULTIEXPR_SUBLINK,	/* (SELECT ...，多目标列) */
	ARRAY_SUBLINK,		/* ARRAY(SELECT ...，单一目标列) */
	CTE_SUBLINK			/* 仅用于 SubPlan，表示 WITH 查询 */
} SubLinkType;


/*
 * SubLink - 表示表达式中的子连接（subselect）
 *
 * subLinkType 指定子连接的类型（EXISTS、ANY、ALL、ROWCOMPARE 等）。
 * subLinkId 仅用于 MULTIEXPR 类型，其他类型为 0。
 * testexpr 是外层查询用于 ALL/ANY/ROWCOMPARE 的测试表达式。
 * operName 是原始指定的操作符名称列表（字符串列表）。
 * subselect 是子连接表达式（可以是 Query* 或原始语法树）。
 * location 表示该节点在 SQL 源码中的位置，未知时为 -1。
 */
typedef struct SubLink
{
	Expr		xpr;
	SubLinkType subLinkType;	/* 子连接类型，见上文说明 */
	int			subLinkId;		/* 子连接编号，仅 MULTIEXPR 类型使用，其他为 0 */
	Node*		testexpr;		/* 针对不同谓词的操作。外层查询用于 ALL/ANY/ROWCOMPARE 的测试表达式 */
	List*		operName;		/* 子连接的操作符，原始指定的操作符名称列表（字符串列表） */
	Node*		subselect;		/* 子连接表达式（可以是 Query* 或原始语法树） */
	int			location;		/* 在 SQL 语句中的位置，未知时为 -1 */
} SubLink;

/*
 * SubPlan - executable expression node for a subplan (sub-SELECT)
 *
 * The planner replaces SubLink nodes in expression trees with SubPlan
 * nodes after it has finished planning the subquery.  SubPlan references
 * a sub-plantree stored in the subplans list of the toplevel PlannedStmt.
 * (We avoid a direct link to make it easier to copy expression trees
 * without causing multiple processing of the subplan.)
 *
 * In an ordinary subplan, testexpr points to an executable expression
 * (OpExpr, an AND/OR tree of OpExprs, or RowCompareExpr) for the combining
 * operator(s); the left-hand arguments are the original lefthand expressions,
 * and the right-hand arguments are PARAM_EXEC Param nodes representing the
 * outputs of the sub-select.  (NOTE: runtime coercion functions may be
 * inserted as well.)  This is just the same expression tree as testexpr in
 * the original SubLink node, but the PARAM_SUBLINK nodes are replaced by
 * suitably numbered PARAM_EXEC nodes.
 *
 * If the sub-select becomes an initplan rather than a subplan, the executable
 * expression is part of the outer plan's expression tree (and the SubPlan
 * node itself is not, but rather is found in the outer plan's initPlan
 * list).  In this case testexpr is NULL to avoid duplication.
 *
 * The planner also derives lists of the values that need to be passed into
 * and out of the subplan.  Input values are represented as a list "args" of
 * expressions to be evaluated in the outer-query context (currently these
 * args are always just Vars, but in principle they could be any expression).
 * The values are assigned to the global PARAM_EXEC params indexed by parParam
 * (the parParam and args lists must have the same ordering).  setParam is a
 * list of the PARAM_EXEC params that are computed by the sub-select, if it
 * is an initplan or MULTIEXPR plan; they are listed in order by sub-select
 * output column position.  (parParam and setParam are integer Lists, not
 * Bitmapsets, because their ordering is significant.)
 *
 * Also, the planner computes startup and per-call costs for use of the
 * SubPlan.  Note that these include the cost of the subquery proper,
 * evaluation of the testexpr if any, and any hashtable management overhead.
 */
typedef struct SubPlan
{
	Expr		xpr;
	/* Fields copied from original SubLink: */
	SubLinkType subLinkType;	/* see above */
	/* The combining operators, transformed to an executable expression: */
	Node* testexpr;		/* OpExpr or RowCompareExpr expression tree */
	List* paramIds;		/* IDs of Params embedded in the above */
	/* Identification of the Plan tree to use: */
	int			plan_id;		/* Index (from 1) in PlannedStmt.subplans */
	/* Identification of the SubPlan for EXPLAIN and debugging purposes: */
	char* plan_name;		/* A name assigned during planning */
	/* Extra data useful for determining subplan's output type: */
	Oid			firstColType;	/* Type of first column of subplan result */
	int32		firstColTypmod; /* Typmod of first column of subplan result */
	Oid			firstColCollation;	/* Collation of first column of subplan
									 * result */
									 /* Information about execution strategy: */
	bool		useHashTable;	/* true to store subselect output in a hash
								 * table (implies we are doing "IN") */
	bool		unknownEqFalse; /* true if it's okay to return FALSE when the
								 * spec result is UNKNOWN; this allows much
								 * simpler handling of null values */
	bool		parallel_safe;	/* is the subplan parallel-safe? */
	/* Note: parallel_safe does not consider contents of testexpr or args */
	/* Information for passing params into and out of the subselect: */
	/* setParam and parParam are lists of integers (param IDs) */
	List* setParam;		/* initplan and MULTIEXPR subqueries have to
								 * set these Params for parent plan */
	List* parParam;		/* indices of input Params from parent plan */
	List* args;			/* exprs to pass as parParam values */
	/* Estimated execution costs: */
	Cost		startup_cost;	/* one-time setup cost */
	Cost		per_call_cost;	/* cost for each subplan evaluation */
	/* Copied from original SubLink, but placed at end for ABI stability */
	int			subLinkId;		/* ID (1..n); 0 if not MULTIEXPR */
} SubPlan;

/*
 * AlternativeSubPlan - expression node for a choice among SubPlans
 *
 * The subplans are given as a List so that the node definition need not
 * change if there's ever more than two alternatives.  For the moment,
 * though, there are always exactly two; and the first one is the fast-start
 * plan.
 */
typedef struct AlternativeSubPlan
{
	Expr		xpr;
	List* subplans;		/* SubPlan(s) with equivalent results */
} AlternativeSubPlan;

/* ----------------
 * FieldSelect
 *
 * FieldSelect represents the operation of extracting one field from a tuple
 * value.  At runtime, the input expression is expected to yield a rowtype
 * Datum.  The specified field number is extracted and returned as a Datum.
 * ----------------
 */

typedef struct FieldSelect
{
	Expr		xpr;
	Expr* arg;			/* input expression */
	AttrNumber	fieldnum;		/* attribute number of field to extract */
	Oid			resulttype;		/* type of the field (result type of this
								 * node) */
	int32		resulttypmod;	/* output typmod (usually -1) */
	Oid			resultcollid;	/* OID of collation of the field */
} FieldSelect;

/* ----------------
 * FieldStore
 *
 * FieldStore represents the operation of modifying one field in a tuple
 * value, yielding a new tuple value (the input is not touched!).  Like
 * the assign case of SubscriptingRef, this is used to implement UPDATE of a
 * portion of a column.
 *
 * resulttype is always a named composite type (not a domain).  To update
 * a composite domain value, apply CoerceToDomain to the FieldStore.
 *
 * A single FieldStore can actually represent updates of several different
 * fields.  The parser only generates FieldStores with single-element lists,
 * but the planner will collapse multiple updates of the same base column
 * into one FieldStore.
 * ----------------
 */

typedef struct FieldStore
{
	Expr		xpr;
	Expr* arg;			/* input tuple value */
	List* newvals;		/* new value(s) for field(s) */
	List* fieldnums;		/* integer list of field attnums */
	Oid			resulttype;		/* type of result (same as type of arg) */
	/* Like RowExpr, we deliberately omit a typmod and collation here */
} FieldStore;

/* ----------------
 * RelabelType
 *
 * RelabelType represents a "dummy" type coercion between two binary-
 * compatible datatypes, such as reinterpreting the result of an OID
 * expression as an int4.  It is a no-op at runtime; we only need it
 * to provide a place to store the correct type to be attributed to
 * the expression result during type resolution.  (We can't get away
 * with just overwriting the type field of the input expression node,
 * so we need a separate node to show the coercion's result type.)
 * ----------------
 */

typedef struct RelabelType
{
	Expr		xpr;
	Expr* arg;			/* input expression */
	Oid			resulttype;		/* output type of coercion expression */
	int32		resulttypmod;	/* output typmod (usually -1) */
	Oid			resultcollid;	/* OID of collation, or InvalidOid if none */
	CoercionForm relabelformat; /* how to display this node */
	int			location;		/* token location, or -1 if unknown */
} RelabelType;

/* ----------------
 * CoerceViaIO
 *
 * CoerceViaIO represents a type coercion between two types whose textual
 * representations are compatible, implemented by invoking the source type's
 * typoutput function then the destination type's typinput function.
 * ----------------
 */

typedef struct CoerceViaIO
{
	Expr		xpr;
	Expr* arg;			/* input expression */
	Oid			resulttype;		/* output type of coercion */
	/* output typmod is not stored, but is presumed -1 */
	Oid			resultcollid;	/* OID of collation, or InvalidOid if none */
	CoercionForm coerceformat;	/* how to display this node */
	int			location;		/* token location, or -1 if unknown */
} CoerceViaIO;

/* ----------------
 * ArrayCoerceExpr
 *
 * ArrayCoerceExpr represents a type coercion from one array type to another,
 * which is implemented by applying the per-element coercion expression
 * "elemexpr" to each element of the source array.  Within elemexpr, the
 * source element is represented by a CaseTestExpr node.  Note that even if
 * elemexpr is a no-op (that is, just CaseTestExpr + RelabelType), the
 * coercion still requires some effort: we have to fix the element type OID
 * stored in the array header.
 * ----------------
 */

typedef struct ArrayCoerceExpr
{
	Expr		xpr;
	Expr* arg;			/* input expression (yields an array) */
	Expr* elemexpr;		/* expression representing per-element work */
	Oid			resulttype;		/* output type of coercion (an array type) */
	int32		resulttypmod;	/* output typmod (also element typmod) */
	Oid			resultcollid;	/* OID of collation, or InvalidOid if none */
	CoercionForm coerceformat;	/* how to display this node */
	int			location;		/* token location, or -1 if unknown */
} ArrayCoerceExpr;

/* ----------------
 * ConvertRowtypeExpr
 *
 * ConvertRowtypeExpr represents a type coercion from one composite type
 * to another, where the source type is guaranteed to contain all the columns
 * needed for the destination type plus possibly others; the columns need not
 * be in the same positions, but are matched up by name.  This is primarily
 * used to convert a whole-row value of an inheritance child table into a
 * valid whole-row value of its parent table's rowtype.  Both resulttype
 * and the exposed type of "arg" must be named composite types (not domains).
 * ----------------
 */

typedef struct ConvertRowtypeExpr
{
	Expr		xpr;
	Expr* arg;			/* input expression */
	Oid			resulttype;		/* output type (always a composite type) */
	/* Like RowExpr, we deliberately omit a typmod and collation here */
	CoercionForm convertformat; /* how to display this node */
	int			location;		/* token location, or -1 if unknown */
} ConvertRowtypeExpr;

/*----------
 * CollateExpr - COLLATE
 *
 * The planner replaces CollateExpr with RelabelType during expression
 * preprocessing, so execution never sees a CollateExpr.
 *----------
 */
typedef struct CollateExpr
{
	Expr		xpr;
	Expr* arg;			/* input expression */
	Oid			collOid;		/* collation's OID */
	int			location;		/* token location, or -1 if unknown */
} CollateExpr;

/*----------
 * CaseExpr - a CASE expression
 *
 * We support two distinct forms of CASE expression:
 *		CASE WHEN boolexpr THEN expr [ WHEN boolexpr THEN expr ... ]
 *		CASE testexpr WHEN compexpr THEN expr [ WHEN compexpr THEN expr ... ]
 * These are distinguishable by the "arg" field being NULL in the first case
 * and the testexpr in the second case.
 *
 * In the raw grammar output for the second form, the condition expressions
 * of the WHEN clauses are just the comparison values.  Parse analysis
 * converts these to valid boolean expressions of the form
 *		CaseTestExpr '=' compexpr
 * where the CaseTestExpr node is a placeholder that emits the correct
 * value at runtime.  This structure is used so that the testexpr need be
 * evaluated only once.  Note that after parse analysis, the condition
 * expressions always yield boolean.
 *
 * Note: we can test whether a CaseExpr has been through parse analysis
 * yet by checking whether casetype is InvalidOid or not.
 *----------
 */
typedef struct CaseExpr
{
	Expr		xpr;
	Oid			casetype;		/* type of expression result */
	Oid			casecollid;		/* OID of collation, or InvalidOid if none */
	Expr* arg;			/* implicit equality comparison argument */
	List* args;			/* the arguments (list of WHEN clauses) */
	Expr* defresult;		/* the default result (ELSE clause) */
	int			location;		/* token location, or -1 if unknown */
} CaseExpr;

/*
 * CaseWhen - one arm of a CASE expression
 */
typedef struct CaseWhen
{
	Expr		xpr;
	Expr* expr;			/* condition expression */
	Expr* result;			/* substitution result */
	int			location;		/* token location, or -1 if unknown */
} CaseWhen;

/*
 * Placeholder node for the test value to be processed by a CASE expression.
 * This is effectively like a Param, but can be implemented more simply
 * since we need only one replacement value at a time.
 *
 * We also abuse this node type for some other purposes, including:
 *	* Placeholder for the current array element value in ArrayCoerceExpr;
 *	  see build_coercion_expression().
 *	* Nested FieldStore/SubscriptingRef assignment expressions in INSERT/UPDATE;
 *	  see transformAssignmentIndirection().
 *
 * The uses in CaseExpr and ArrayCoerceExpr are safe only to the extent that
 * there is not any other CaseExpr or ArrayCoerceExpr between the value source
 * node and its child CaseTestExpr(s).  This is true in the parse analysis
 * output, but the planner's function-inlining logic has to be careful not to
 * break it.
 *
 * The nested-assignment-expression case is safe because the only node types
 * that can be above such CaseTestExprs are FieldStore and SubscriptingRef.
 */
typedef struct CaseTestExpr
{
	Expr		xpr;
	Oid			typeId;			/* type for substituted value */
	int32		typeMod;		/* typemod for substituted value */
	Oid			collation;		/* collation for the substituted value */
} CaseTestExpr;

/*
 * ArrayExpr - an ARRAY[] expression
 *
 * Note: if multidims is false, the constituent expressions all yield the
 * scalar type identified by element_typeid.  If multidims is true, the
 * constituent expressions all yield arrays of element_typeid (ie, the same
 * type as array_typeid); at runtime we must check for compatible subscripts.
 */
typedef struct ArrayExpr
{
	Expr		xpr;
	Oid			array_typeid;	/* type of expression result */
	Oid			array_collid;	/* OID of collation, or InvalidOid if none */
	Oid			element_typeid; /* common type of array elements */
	List* elements;		/* the array elements or sub-arrays */
	bool		multidims;		/* true if elements are sub-arrays */
	int			location;		/* token location, or -1 if unknown */
} ArrayExpr;

/*
 * RowExpr - a ROW() expression
 *
 * Note: the list of fields must have a one-for-one correspondence with
 * physical fields of the associated rowtype, although it is okay for it
 * to be shorter than the rowtype.  That is, the N'th list element must
 * match up with the N'th physical field.  When the N'th physical field
 * is a dropped column (attisdropped) then the N'th list element can just
 * be a NULL constant.  (This case can only occur for named composite types,
 * not RECORD types, since those are built from the RowExpr itself rather
 * than vice versa.)  It is important not to assume that length(args) is
 * the same as the number of columns logically present in the rowtype.
 *
 * colnames provides field names in cases where the names can't easily be
 * obtained otherwise.  Names *must* be provided if row_typeid is RECORDOID.
 * If row_typeid identifies a known composite type, colnames can be NIL to
 * indicate the type's cataloged field names apply.  Note that colnames can
 * be non-NIL even for a composite type, and typically is when the RowExpr
 * was created by expanding a whole-row Var.  This is so that we can retain
 * the column alias names of the RTE that the Var referenced (which would
 * otherwise be very difficult to extract from the parsetree).  Like the
 * args list, colnames is one-for-one with physical fields of the rowtype.
 */
typedef struct RowExpr
{
	Expr		xpr;
	List* args;			/* the fields */
	Oid			row_typeid;		/* RECORDOID or a composite type's ID */

	/*
	 * row_typeid cannot be a domain over composite, only plain composite.  To
	 * create a composite domain value, apply CoerceToDomain to the RowExpr.
	 *
	 * Note: we deliberately do NOT store a typmod.  Although a typmod will be
	 * associated with specific RECORD types at runtime, it will differ for
	 * different backends, and so cannot safely be stored in stored
	 * parsetrees.  We must assume typmod -1 for a RowExpr node.
	 *
	 * We don't need to store a collation either.  The result type is
	 * necessarily composite, and composite types never have a collation.
	 */
	CoercionForm row_format;	/* how to display this node */
	List* colnames;		/* list of String, or NIL */
	int			location;		/* token location, or -1 if unknown */
} RowExpr;

/*
 * RowCompareExpr - row-wise comparison, such as (a, b) <= (1, 2)
 *
 * We support row comparison for any operator that can be determined to
 * act like =, <>, <, <=, >, or >= (we determine this by looking for the
 * operator in btree opfamilies).  Note that the same operator name might
 * map to a different operator for each pair of row elements, since the
 * element datatypes can vary.
 *
 * A RowCompareExpr node is only generated for the < <= > >= cases;
 * the = and <> cases are translated to simple AND or OR combinations
 * of the pairwise comparisons.  However, we include = and <> in the
 * RowCompareType enum for the convenience of parser logic.
 */
typedef enum RowCompareType
{
	/* Values of this enum are chosen to match btree strategy numbers */
	ROWCOMPARE_LT = 1,			/* BTLessStrategyNumber */
	ROWCOMPARE_LE = 2,			/* BTLessEqualStrategyNumber */
	ROWCOMPARE_EQ = 3,			/* BTEqualStrategyNumber */
	ROWCOMPARE_GE = 4,			/* BTGreaterEqualStrategyNumber */
	ROWCOMPARE_GT = 5,			/* BTGreaterStrategyNumber */
	ROWCOMPARE_NE = 6			/* no such btree strategy */
} RowCompareType;

typedef struct RowCompareExpr
{
	Expr		xpr;
	RowCompareType rctype;		/* LT LE GE or GT, never EQ or NE */
	List* opnos;			/* OID list of pairwise comparison ops */
	List* opfamilies;		/* OID list of containing operator families */
	List* inputcollids;	/* OID list of collations for comparisons */
	List* largs;			/* the left-hand input arguments */
	List* rargs;			/* the right-hand input arguments */
} RowCompareExpr;

/*
 * CoalesceExpr - a COALESCE expression
 */
typedef struct CoalesceExpr
{
	Expr		xpr;
	Oid			coalescetype;	/* type of expression result */
	Oid			coalescecollid; /* OID of collation, or InvalidOid if none */
	List* args;			/* the arguments */
	int			location;		/* token location, or -1 if unknown */
} CoalesceExpr;

/*
 * MinMaxExpr - a GREATEST or LEAST function
 */
typedef enum MinMaxOp
{
	IS_GREATEST,
	IS_LEAST
} MinMaxOp;

typedef struct MinMaxExpr
{
	Expr		xpr;
	Oid			minmaxtype;		/* common type of arguments and result */
	Oid			minmaxcollid;	/* OID of collation of result */
	Oid			inputcollid;	/* OID of collation that function should use */
	MinMaxOp	op;				/* function to execute */
	List* args;			/* the arguments */
	int			location;		/* token location, or -1 if unknown */
} MinMaxExpr;

/*
 * SQLValueFunction - parameterless functions with special grammar productions
 *
 * The SQL standard categorizes some of these as <datetime value function>
 * and others as <general value specification>.  We call 'em SQLValueFunctions
 * for lack of a better term.  We store type and typmod of the result so that
 * some code doesn't need to know each function individually, and because
 * we would need to store typmod anyway for some of the datetime functions.
 * Note that currently, all variants return non-collating datatypes, so we do
 * not need a collation field; also, all these functions are stable.
 */
typedef enum SQLValueFunctionOp
{
	SVFOP_CURRENT_DATE,
	SVFOP_CURRENT_TIME,
	SVFOP_CURRENT_TIME_N,
	SVFOP_CURRENT_TIMESTAMP,
	SVFOP_CURRENT_TIMESTAMP_N,
	SVFOP_LOCALTIME,
	SVFOP_LOCALTIME_N,
	SVFOP_LOCALTIMESTAMP,
	SVFOP_LOCALTIMESTAMP_N,
	SVFOP_CURRENT_ROLE,
	SVFOP_CURRENT_USER,
	SVFOP_USER,
	SVFOP_SESSION_USER,
	SVFOP_CURRENT_CATALOG,
	SVFOP_CURRENT_SCHEMA
} SQLValueFunctionOp;

typedef struct SQLValueFunction
{
	Expr		xpr;
	SQLValueFunctionOp op;		/* which function this is */
	Oid			type;			/* result type/typmod */
	int32		typmod;
	int			location;		/* token location, or -1 if unknown */
} SQLValueFunction;

/*
 * XmlExpr - various SQL/XML functions requiring special grammar productions
 *
 * 'name' carries the "NAME foo" argument (already XML-escaped).
 * 'named_args' and 'arg_names' represent an xml_attribute list.
 * 'args' carries all other arguments.
 *
 * Note: result type/typmod/collation are not stored, but can be deduced
 * from the XmlExprOp.  The type/typmod fields are just used for display
 * purposes, and are NOT necessarily the true result type of the node.
 */
typedef enum XmlExprOp
{
	IS_XMLCONCAT,				/* XMLCONCAT(args) */
	IS_XMLELEMENT,				/* XMLELEMENT(name, xml_attributes, args) */
	IS_XMLFOREST,				/* XMLFOREST(xml_attributes) */
	IS_XMLPARSE,				/* XMLPARSE(text, is_doc, preserve_ws) */
	IS_XMLPI,					/* XMLPI(name [, args]) */
	IS_XMLROOT,					/* XMLROOT(xml, version, standalone) */
	IS_XMLSERIALIZE,			/* XMLSERIALIZE(is_document, xmlval) */
	IS_DOCUMENT					/* xmlval IS DOCUMENT */
} XmlExprOp;

typedef enum
{
	XMLOPTION_DOCUMENT,
	XMLOPTION_CONTENT
} XmlOptionType;

typedef struct XmlExpr
{
	Expr		xpr;
	XmlExprOp	op;				/* xml function ID */
	char* name;			/* name in xml(NAME foo ...) syntaxes */
	List* named_args;		/* non-XML expressions for xml_attributes */
	List* arg_names;		/* parallel list of Value strings */
	List* args;			/* list of expressions */
	XmlOptionType xmloption;	/* DOCUMENT or CONTENT */
	Oid			type;			/* target type/typmod for XMLSERIALIZE */
	int32		typmod;
	int			location;		/* token location, or -1 if unknown */
} XmlExpr;

/* ----------------
 * NullTest
 *
 * NullTest represents the operation of testing a value for NULLness.
 * The appropriate test is performed and returned as a boolean Datum.
 *
 * When argisrow is false, this simply represents a test for the null value.
 *
 * When argisrow is true, the input expression must yield a rowtype, and
 * the node implements "row IS [NOT] NULL" per the SQL standard.  This
 * includes checking individual fields for NULLness when the row datum
 * itself isn't NULL.
 *
 * NOTE: the combination of a rowtype input and argisrow==false does NOT
 * correspond to the SQL notation "row IS [NOT] NULL"; instead, this case
 * represents the SQL notation "row IS [NOT] DISTINCT FROM NULL".
 * ----------------
 */

typedef enum NullTestType
{
	IS_NULL, IS_NOT_NULL
} NullTestType;

typedef struct NullTest
{
	Expr		xpr;
	Expr* arg;			/* input expression */
	NullTestType nulltesttype;	/* IS NULL, IS NOT NULL */
	bool		argisrow;		/* T to perform field-by-field null checks */
	int			location;		/* token location, or -1 if unknown */
} NullTest;

/*
 * BooleanTest
 *
 * BooleanTest represents the operation of determining whether a boolean
 * is TRUE, FALSE, or UNKNOWN (ie, NULL).  All six meaningful combinations
 * are supported.  Note that a NULL input does *not* cause a NULL result.
 * The appropriate test is performed and returned as a boolean Datum.
 */

typedef enum BoolTestType
{
	IS_TRUE, IS_NOT_TRUE, IS_FALSE, IS_NOT_FALSE, IS_UNKNOWN, IS_NOT_UNKNOWN
} BoolTestType;

typedef struct BooleanTest
{
	Expr		xpr;
	Expr* arg;			/* input expression */
	BoolTestType booltesttype;	/* test type */
	int			location;		/* token location, or -1 if unknown */
} BooleanTest;

/*
 * CoerceToDomain
 *
 * CoerceToDomain represents the operation of coercing a value to a domain
 * type.  At runtime (and not before) the precise set of constraints to be
 * checked will be determined.  If the value passes, it is returned as the
 * result; if not, an error is raised.  Note that this is equivalent to
 * RelabelType in the scenario where no constraints are applied.
 */
typedef struct CoerceToDomain
{
	Expr		xpr;
	Expr* arg;			/* input expression */
	Oid			resulttype;		/* domain type ID (result type) */
	int32		resulttypmod;	/* output typmod (currently always -1) */
	Oid			resultcollid;	/* OID of collation, or InvalidOid if none */
	CoercionForm coercionformat;	/* how to display this node */
	int			location;		/* token location, or -1 if unknown */
} CoerceToDomain;

/*
 * Placeholder node for the value to be processed by a domain's check
 * constraint.  This is effectively like a Param, but can be implemented more
 * simply since we need only one replacement value at a time.
 *
 * Note: the typeId/typeMod/collation will be set from the domain's base type,
 * not the domain itself.  This is because we shouldn't consider the value
 * to be a member of the domain if we haven't yet checked its constraints.
 */
typedef struct CoerceToDomainValue
{
	Expr		xpr;
	Oid			typeId;			/* type for substituted value */
	int32		typeMod;		/* typemod for substituted value */
	Oid			collation;		/* collation for the substituted value */
	int			location;		/* token location, or -1 if unknown */
} CoerceToDomainValue;

/*
 * Placeholder node for a DEFAULT marker in an INSERT or UPDATE command.
 *
 * This is not an executable expression: it must be replaced by the actual
 * column default expression during rewriting.  But it is convenient to
 * treat it as an expression node during parsing and rewriting.
 */
typedef struct SetToDefault
{
	Expr		xpr;
	Oid			typeId;			/* type for substituted value */
	int32		typeMod;		/* typemod for substituted value */
	Oid			collation;		/* collation for the substituted value */
	int			location;		/* token location, or -1 if unknown */
} SetToDefault;

/*
 * Node representing [WHERE] CURRENT OF cursor_name
 *
 * CURRENT OF is a bit like a Var, in that it carries the rangetable index
 * of the target relation being constrained; this aids placing the expression
 * correctly during planning.  We can assume however that its "levelsup" is
 * always zero, due to the syntactic constraints on where it can appear.
 *
 * The referenced cursor can be represented either as a hardwired string
 * or as a reference to a run-time parameter of type REFCURSOR.  The latter
 * case is for the convenience of plpgsql.
 */
typedef struct CurrentOfExpr
{
	Expr		xpr;
	Index		cvarno;			/* RT index of target relation */
	char* cursor_name;	/* name of referenced cursor, or NULL */
	int			cursor_param;	/* refcursor parameter number, or 0 */
} CurrentOfExpr;

/*
 * NextValueExpr - get next value from sequence
 *
 * This has the same effect as calling the nextval() function, but it does not
 * check permissions on the sequence.  This is used for identity columns,
 * where the sequence is an implicit dependency without its own permissions.
 */
typedef struct NextValueExpr
{
	Expr		xpr;
	Oid			seqid;
	Oid			typeId;
} NextValueExpr;

/*
 * InferenceElem - an element of a unique index inference specification
 *
 * This mostly matches the structure of IndexElems, but having a dedicated
 * primnode allows for a clean separation between the use of index parameters
 * by utility commands, and this node.
 */
typedef struct InferenceElem
{
	Expr		xpr;
	Node* expr;			/* expression to infer from, or NULL */
	Oid			infercollid;	/* OID of collation, or InvalidOid */
	Oid			inferopclass;	/* OID of att opclass, or InvalidOid */
} InferenceElem;

/*--------------------
 * TargetEntry -
 *	   目标条目（用于查询的目标列表）
 *
 * 严格来说，TargetEntry 不是一个表达式节点（因为它不能被 ExecEvalExpr 评估）。
 * 但我们仍将其作为表达式处理，因为在很多场合将整个查询目标列表作为一棵
 * 表达式树来处理是很方便的。
 *
 * 在 SELECT 的 targetlist 中，resno 通常应等于该项的序号（从 1 开始）。
 * 但是在 INSERT 或 UPDATE 的 targetlist 中，resno 表示目标列的属性号；
 * 因此可能存在缺失或乱序的 resno。甚至允许重复的 resno；例如：
 *		UPDATE table SET arraycol[1] = ..., arraycol[2] = ..., ...
 * 这两种含义在执行器中会合在一起，因为规划器会将 INSERT/UPDATE 的 tlist
 * 归一化为针对目标表每一列恰好一个条目的形式。在那之前，不应假设 resno == position。
 * 通常应使用 get_tle_by_resno() 来按 resno 获取 tlist 条目，而不是直接用 list_nth()，
 * 仅在 SELECT 的情况下才可假定 resno 是唯一标识。
 *
 * 对于顶层 SELECT 的非 resjunk 条目，resname 被要求表示正确的列名，
 * 因为它会作为发送到前端的列标题。在大多数其他场合它只是调试辅助信息，
 * 可能为 NULL 或不准确。（例如，存储规则的 tlist 中，如果被引用列在之后
 * 被 ALTER TABLE 重命名，则 resname 可能不正确。规划器在非顶层计划节点中
 * 往往也会存储 NULL 而不是查找有效名称。）对于 resjunk 条目，resname
 * 应该是系统生成的特定名称（例如 "ctid"）或 NULL；否则可能会干扰 ExecGetJunkAttribute！
 *
 * ressortgroupref 用于表示 ORDER BY、GROUP BY 和 DISTINCT 项。
 * ressortgroupref=0 的条目不是排序/分组项。若 ressortgroupref>0，
 * 则该条目是一个 ORDER BY、GROUP BY 或/和 DISTINCT 的目标值。
 * 在同一个 targetlist 中，不得有两个条目具有相同的非零 ressortgroupref，
 * 但非零值本身没有别的语义（例如不要假定较小的 ressortgroupref 表示更重要的排序键）。
 * 相关的 SortGroupClause 列表的顺序决定了语义。
 *
 * resorigtbl/resorigcol 标识列的来源（如果它是对基表或视图某列的简单引用）。
 * 若不是简单引用，这些字段为零。
 *
 * 若 resjunk 为真，则该列是用于中间处理的工作列（例如排序键），应从查询最终输出中移除。
 * Resjunk 列必须具有不会与任何常规列重复的 resno。此外，有些地方假定 resjunk 列出现在非 junk 列之后。
 *--------------------
 */
typedef struct TargetEntry
{
	Expr		xpr;
	Expr* 		expr;				/* 要计算的表达式 */
	AttrNumber	resno;				/* 属性号（见上文说明） */
	char* 		resname;			/* 列名（可以为 NULL） */
	Index		ressortgroupref;	/* 若被排序/分组引用则为非零值 */
	Oid			resorigtbl;			/* 列来源表的 OID */
	AttrNumber	resorigcol;			/* 列在源表中的列号 */
	bool		resjunk;			/* 为 true 则在最终目标列表中移除该属性 */
} TargetEntry;


/* ----------------------------------------------------------------
 *					连接树的节点类型
 *
 * 连接树结构的叶子节点是 RangeTblRef 节点。在它们之上可以出现
 * JoinExpr 节点，用于表示某种特定类型的连接或带条件的连接。
 * 同样，FromExpr 节点可以表示普通的笛卡尔积连接（"FROM foo, bar, baz WHERE ..."）。
 * FromExpr 类似于 jointype 为 JOIN_INNER 的 JoinExpr，但它可以有任意数量的子节点，
 * 而不仅仅是两个。
 *
 * 注意：Query 的 jointree 的顶层总是一个 FromExpr。即使 jointree 不包含任何关系，
 * 也会有一个 FromExpr。
 *
 * 注意：JoinExpr 节点中存在的限定条件（qualification expressions）
 * 是在查询主 WHERE 子句之外的附加条件，主查询的 WHERE 作为顶层 FromExpr 的 qual。
 * 将 quals 与连接树中特定节点关联的原因是当存在外连接时，qual 的位置很关键。
 * （如果对 qual 的应用时机过早或过晚，可能导致外连接产生错误的带 NULL 扩展的行集。）
 * 如果所有连接都是内连接，则所有 qual 的位置在语义上是可互换的。
 *
 * 注意：在 gram.y 的原始输出中，连接树包含 RangeVar、RangeSubselect 和 RangeFunction 节点，
 * 这些节点在解析分析阶段都被替换为 RangeTblRef 节点。此外，顶层的 FromExpr 也是在
 * 解析分析阶段加入的；语法上将 FROM 和 WHERE 视为分离的部分。
 * ----------------------------------------------------------------
 */

 /*
  * RangeTblRef - 引用查询 rangetable 中的一个条目，在 Query 的 jointree 中使用
  *
  * 我们本可以使用指向 RT 条目的直接指针而省去这些节点，但在 querytree 中对同一节点的多重指针
  * 会带来很多麻烦，所以更好的做法是存储一个到 RT 的索引。
  */
typedef struct RangeTblRef
{
	NodeTag		type;
	int			rtindex;
} RangeTblRef;

/*----------
 * JoinExpr - 用于 SQL JOIN 表达式
 *
 * isNatural、usingClause 和 quals 彼此关联。用户只能写入
 * NATURAL、USING() 或 ON() 中的一种（语法层面强制如此）。
 * 如果用户写入 NATURAL，则解析分析会生成等效的 USING() 列表，
 * 并据此填充 "quals" 为相应的相等比较表达式。
 * 如果用户写入 USING()，则 "quals" 会被填充为相等比较表达式。
 * 如果用户写入 ON()，则仅设置 "quals"。注意 NATURAL/USING
 * 与 ON() 并不完全等价，因为前者还会影响输出列列表。
 *
 * alias 是表示附加到连接表达式的 AS 别名子句的 Alias 节点，
 * 如果不存在则为 NULL。注意：别名的存在与否对语义有重要影响，
 * 因为带别名的连接会限制其内部表/列的可见性。
 *
 * 在解析分析阶段，会为 Join 创建一个 RTE，并将其索引填入 rtindex。
 * 该 RTE 存在主要是为了让 Var 能够引用连接的输出。规划器有时会
 * 在内部生成 JoinExpr；这些 JoinExpr 的 rtindex 可能为 0，表示
 * 没有为该连接分配别名的 RT 变量。
 *----------
 */
typedef struct JoinExpr
{
	NodeTag		type;
	JoinType	jointype;		/* 连接类型 */
	bool		isNatural;		/* 是否为 NATURAL JOIN？需要调整输出列 */
	Node*		larg;			/* 左子树 */
	Node*		rarg;			/* 右子树 */
	List*		usingClause;	/* USING 子句（若有），字符串列表 */
	Node*		quals;			/* 该连接的限定条件（若有） */
	Alias*		alias;			/* 连接操作的投影列用户书写的别名子句（若有） */
	int			rtindex;		/* 这个 JoinExpr 对应的 RangeTblRef->rtindex，为该连接分配的 RT 索引，或 0 */
} JoinExpr;

/*----------
 * FromExpr - 表示 FROM ... WHERE ... 结构
 *
 * 该节点比 JoinExpr 更灵活（它可以有任意数量的子节点，包括零个），
 * 但也更简单——我们无需处理别名等。输出列集合隐式地是子节点输出的并集。
 *----------
 */
typedef struct FromExpr
{
	NodeTag		type;
	List*		fromlist;		/* FromExpr 中包含的表，连接子树列表（List of join subtrees） */
	Node*		quals;			/* fromlist 中表连接上的限定条件（若有） */
} FromExpr;

/*----------
 * OnConflictExpr - represents an ON CONFLICT DO ... expression
 *
 * The optimizer requires a list of inference elements, and optionally a WHERE
 * clause to infer a unique index.  The unique index (or, occasionally,
 * indexes) inferred are used to arbitrate whether or not the alternative ON
 * CONFLICT path is taken.
 *----------
 */
typedef struct OnConflictExpr
{
	NodeTag		type;
	OnConflictAction action;	/* DO NOTHING or UPDATE? */

	/* Arbiter */
	List* arbiterElems;	/* unique index arbiter list (of
								 * InferenceElem's) */
	Node* arbiterWhere;	/* unique index arbiter WHERE clause */
	Oid			constraint;		/* pg_constraint OID for arbiter */

	/* ON CONFLICT UPDATE */
	List* onConflictSet;	/* List of ON CONFLICT SET TargetEntrys */
	Node* onConflictWhere;	/* qualifiers to restrict UPDATE to */
	int			exclRelIndex;	/* RT index of 'excluded' relation */
	List* exclRelTlist;	/* tlist of the EXCLUDED pseudo relation */
} OnConflictExpr;

#endif							/* PRIMNODES_H */
