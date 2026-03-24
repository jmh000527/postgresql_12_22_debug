/*-------------------------------------------------------------------------
 *
 * postgres_fdw.h
 *		  Foreign-data wrapper for remote PostgreSQL servers
 *
 * Portions Copyright (c) 2012-2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/postgres_fdw/postgres_fdw.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGRES_FDW_H
#define POSTGRES_FDW_H

#include "foreign/foreign.h"
#include "lib/stringinfo.h"
#include "nodes/pathnodes.h"
#include "utils/relcache.h"

#include "libpq-fe.h"

/*
 * PgFdwRelationInfo
 *
 * 这是 postgres_fdw 在优化阶段为每个关系（RelOptInfo）维护的私有数据，
 * 存储在 RelOptInfo.fdw_private 字段中。
 *
 * 对于基表（baserel），此结构由 postgresGetForeignRelSize 创建，
 * 尽管部分字段会稍后填充。
 * 对于连接关系（joinrel），由 postgresGetForeignJoinPaths 创建。
 * 对于上层关系（upperrel，如聚合/分组），由 postgresGetForeignUpperPaths 创建。
 */
typedef struct PgFdwRelationInfo
{
	/*
	 * 指示该关系是否可以安全地下推到远程服务器执行。
	 * 对于简单的单表外部扫描（foreign scan），此值总为 true。
	 * 对于连接或聚合，若存在不支持的语法或函数，则为 false。
	 */
	bool		pushdown_safe;

	/*
	 * 过滤条件（Restriction clauses）列表。
	 * 分为两部分：
	 * remote_conds: 可以安全下推并在远程服务器执行的条件。
	 * local_conds: 不能下推，必须将数据拉回本地后过滤的条件。
	 *
	 * 列表中的每一项都应包装在 RestrictInfo 结构中，
	 * 这有助于提高选择率（selectivity）和成本（cost）估算的效率。
	 */
	List	   *remote_conds;
	List	   *local_conds;

	/*
	 * 用于实际生成 SQL 的远程过滤条件列表（不包含 RestrictInfo 包装器）。
	 * 这些是从 remote_conds 提取出来的纯表达式。
	 */
	List	   *final_remote_exprs;

	/*
	 * 位图，记录了我们需要从远程服务器获取哪些列（属性）。
	 * 对应于 baserel 的属性号。
	 */
	Bitmapset  *attrs_used;

	/*
	 * 指示查询的路径键（pathkeys，即排序顺序）是否可以安全地下推。
	 * 如果为 true，我们可以请求远程服务器对结果进行排序。
	 */
	bool		qp_is_pushdown_safe;

	/* 本地过滤条件（local_conds）的计算成本和选择率 */
	QualCost	local_conds_cost;
	Selectivity local_conds_sel;

	/*
	 * 连接条件（join clauses）的选择率。
	 * 仅当此关系为 joinrel 时有效。
	 */
	Selectivity joinclause_sel;

	/*
	 * 估算结果：
	 * rows: 扫描、连接或分组/聚合后的预计行数。
	 * width: 预计行的平均宽度（字节）。
	 * startup_cost: 启动成本（获取第一行所需的成本）。
	 * total_cost: 总成本（获取所有行所需的成本）。
	 *
	 * 注意：这些成本包括了网络传输和本地处理的开销。
	 */
	double		rows;
	int			width;
	Cost		startup_cost;
	Cost		total_cost;

	/*
	 * 仅用于 estimate_path_cost_size() 内部估算的中间值：
	 * retrieved_rows: 从远程服务器实际获取的行数估算值。
	 * rel_startup_cost: 在远程服务器上的启动成本（不含传输）。
	 * rel_total_cost: 在远程服务器上的总执行成本（不含传输）。
	 */
	double		retrieved_rows;
	Cost		rel_startup_cost;
	Cost		rel_total_cost;

	/* 从系统目录（System Catalogs）中提取的 FDW 选项 */
	bool		use_remote_estimate;	/* 是否使用 EXPLAIN 从远程获取估算值 */
	Cost		fdw_startup_cost;		/* FDW 设置的启动成本因子 */
	Cost		fdw_tuple_cost;			/* FDW 设置的每行处理成本因子 */
	List	   *shippable_extensions;	/* 允许下推的扩展 OID 白名单 */

	/* 缓存的系统目录信息，避免重复查表 */
	ForeignTable *table;
	ForeignServer *server;
	UserMapping *user;			/* 仅在 use_remote_estimate 模式下设置 */

	int			fetch_size;		/* 此远程表的每次网络获取行数（fetch size） */

	/*
	 * 关系名称，用于构建 EXPLAIN 输出中的 Description。
	 * 也会用于 joinrel，此时名称指示了正在连接哪些表以及连接类型。
	 */
	StringInfo	relation_name;

	/*
	 * 连接信息（仅当此 struct 为 joinrel 时有效）
	 */
	RelOptInfo *outerrel;		/* 连接的外侧关系 */
	RelOptInfo *innerrel;		/* 连接的内侧关系 */
	JoinType	jointype;		/* 连接类型（Inner, Left, etc.） */

	/*
	 * 仅包含 JOIN/ON 条件的列表（主要用于外连接）。
	 * 对于内连接，条件通常在 remote_conds 中，这里可能为空。
	 */
	List	   *joinclauses;	/* List of RestrictInfo */

	/*
	 * 上层关系信息（仅当此 struct 为 upperrel 时有效）
	 */
	UpperRelationKind stage;	/* 上层处理阶段（如 Group, Sort） */

	/*
	 * 分组信息（仅当做聚合/分组下推时有效）
	 */
	List	   *grouped_tlist;	/* 分组目标列表 */

	/*
	 * 子查询构造信息。
	 * 在生成远程 SQL 时，是否需要将 outerrel 或 innerrel 包装在括号子查询中？
	 */
	bool		make_outerrel_subquery; /* 是否将 outerrel 解析为子查询? */
	bool		make_innerrel_subquery; /* 是否将 innerrel 解析为子查询? */

	/* 所有出现在下层子查询中的关系 ID 集合 */
	Relids		lower_subquery_rels;

	/*
	 * 关系的索引（虚构的）。
	 * 用于在生成 SQL 时为代表该关系的子查询创建一个别名（例如 "s1", "s2"）。
	 */
	int			relation_index;
} PgFdwRelationInfo;

/* in postgres_fdw.c */
extern int	set_transmission_modes(void);
extern void reset_transmission_modes(int nestlevel);

/* in connection.c */
extern PGconn *GetConnection(UserMapping *user, bool will_prep_stmt);
extern void ReleaseConnection(PGconn *conn);
extern unsigned int GetCursorNumber(PGconn *conn);
extern unsigned int GetPrepStmtNumber(PGconn *conn);
extern PGresult *pgfdw_get_result(PGconn *conn, const char *query);
extern PGresult *pgfdw_exec_query(PGconn *conn, const char *query);
extern void pgfdw_report_error(int elevel, PGresult *res, PGconn *conn,
							   bool clear, const char *sql);

/* in option.c */
extern int	ExtractConnectionOptions(List *defelems,
									 const char **keywords,
									 const char **values);
extern List *ExtractExtensionList(const char *extensionsString,
								  bool warnOnMissing);

/* in deparse.c */
extern void classifyConditions(PlannerInfo *root,
							   RelOptInfo *baserel,
							   List *input_conds,
							   List **remote_conds,
							   List **local_conds);
extern bool is_foreign_expr(PlannerInfo *root,
							RelOptInfo *baserel,
							Expr *expr);
extern bool is_foreign_param(PlannerInfo *root,
							 RelOptInfo *baserel,
							 Expr *expr);
extern bool is_foreign_pathkey(PlannerInfo *root,
							   RelOptInfo *baserel,
							   PathKey *pathkey);
extern void deparseInsertSql(StringInfo buf, RangeTblEntry *rte,
							 Index rtindex, Relation rel,
							 List *targetAttrs, bool doNothing,
							 List *withCheckOptionList, List *returningList,
							 List **retrieved_attrs);
extern void deparseUpdateSql(StringInfo buf, RangeTblEntry *rte,
							 Index rtindex, Relation rel,
							 List *targetAttrs,
							 List *withCheckOptionList, List *returningList,
							 List **retrieved_attrs);
extern void deparseDirectUpdateSql(StringInfo buf, PlannerInfo *root,
								   Index rtindex, Relation rel,
								   RelOptInfo *foreignrel,
								   List *targetlist,
								   List *targetAttrs,
								   List *remote_conds,
								   List **params_list,
								   List *returningList,
								   List **retrieved_attrs);
extern void deparseDeleteSql(StringInfo buf, RangeTblEntry *rte,
							 Index rtindex, Relation rel,
							 List *returningList,
							 List **retrieved_attrs);
extern void deparseDirectDeleteSql(StringInfo buf, PlannerInfo *root,
								   Index rtindex, Relation rel,
								   RelOptInfo *foreignrel,
								   List *remote_conds,
								   List **params_list,
								   List *returningList,
								   List **retrieved_attrs);
extern void deparseAnalyzeSizeSql(StringInfo buf, Relation rel);
extern void deparseAnalyzeSql(StringInfo buf, Relation rel,
							  List **retrieved_attrs);
extern void deparseStringLiteral(StringInfo buf, const char *val);
extern EquivalenceMember *find_em_for_rel(PlannerInfo *root,
										  EquivalenceClass *ec,
										  RelOptInfo *rel);
extern EquivalenceMember *find_em_for_rel_target(PlannerInfo *root,
												 EquivalenceClass *ec,
												 RelOptInfo *rel);
extern List *build_tlist_to_deparse(RelOptInfo *foreignrel);
extern void deparseSelectStmtForRel(StringInfo buf, PlannerInfo *root,
									RelOptInfo *foreignrel, List *tlist,
									List *remote_conds, List *pathkeys,
									bool has_final_sort, bool has_limit,
									bool is_subquery,
									List **retrieved_attrs, List **params_list);
extern const char *get_jointype_name(JoinType jointype);

/* in shippable.c */
extern bool is_builtin(Oid objectId);
extern bool is_shippable(Oid objectId, Oid classId, PgFdwRelationInfo *fpinfo);

#endif							/* POSTGRES_FDW_H */
