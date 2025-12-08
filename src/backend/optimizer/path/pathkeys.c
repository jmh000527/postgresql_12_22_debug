/*-------------------------------------------------------------------------
 *
 * pathkeys.c
 *	  Utilities for matching and building path keys
 *
 * See src/backend/optimizer/README for a great deal of information about
 * the nature and use of path keys.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/pathkeys.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "catalog/pg_opfamily.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/plannodes.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "partitioning/partbounds.h"
#include "utils/lsyscache.h"


static bool pathkey_is_redundant(PathKey *new_pathkey, List *pathkeys);
static bool matches_boolean_partition_clause(RestrictInfo *rinfo,
											 RelOptInfo *partrel,
											 int partkeycol);
static Var *find_var_for_subquery_tle(RelOptInfo *rel, TargetEntry *tle);
static bool right_merge_direction(PlannerInfo *root, PathKey *pathkey);


/****************************************************************************
 *		PATHKEY CONSTRUCTION AND REDUNDANCY TESTING
 ****************************************************************************/

/*
 * make_canonical_pathkey
 *	  根据给定参数查找或创建规范化（canonical）的 PathKey。
 *
 * 功能说明：
 * - 如果查询的规范化 PathKey 列表（root->canon_pathkeys）中已存在完全匹配的 PathKey，则直接返回该 PathKey。
 * - 如果没有，则新建一个 PathKey，并加入到规范化列表中。
 *
 * 参数说明：
 * - root: 查询优化器上下文
 * - eclass: 等价类（EquivalenceClass），可能不是规范化的，需要追溯到顶层
 * - opfamily: 排序操作符族
 * - strategy: 排序策略（升序/降序）
 * - nulls_first: NULL 是否排在前面
 *
 * 注意事项：
 * - 只有在所有 EquivalenceClass 合并完成后才能调用本函数，否则会导致规范化 PathKey 列表不一致。
 * - 新建的 PathKey 必须分配在主规划内存上下文中（planner_cxt），以保证生命周期正确。
 */
PathKey *
make_canonical_pathkey(PlannerInfo *root,
					   EquivalenceClass *eclass, Oid opfamily,
					   int strategy, bool nulls_first)
{
	PathKey    *pk;
	ListCell   *lc;
	MemoryContext oldcontext;

	/* 
	 * 追溯到规范化的等价类（顶层 EC）
	 * 
	 * 详细解释：
	 * 在查询规划过程中，等价类（EquivalenceClass, EC）可能会发生合并。
	 * 例如，如果有 WHERE a = b AND b = c，最初可能有 {a, b} 和 {b, c} 两个 EC。
	 * 后来优化器发现它们其实是同一个集合 {a, b, c}，于是会将其中一个 EC 合并到另一个 EC 中。
	 * 被合并的 EC 会设置 ec_merged 指针指向合并后的主 EC。
	 * 
	 * PathKey 必须始终引用合并后的主 EC（Canonical EC），以确保唯一性和一致性。
	 * 这个 while 循环就是为了找到那个最终的、未被合并的主 EC。
	 */
	while (eclass->ec_merged)
		eclass = eclass->ec_merged;

	/* 
	 * 查找是否已存在完全匹配的 PathKey
	 * 
	 * 详细解释：
	 * root->canon_pathkeys 是一个全局缓存列表，存储了当前查询中所有已知的“规范化 PathKey”。
	 * 
	 * 为什么要缓存？
	 * 1. 节省内存：避免为相同的排序需求重复创建 PathKey 对象。
	 * 2. 快速比较：如果两个 PathKey 指针相同（地址相同），我们就知道它们代表完全相同的排序顺序，
	 *    而不需要去比较内部复杂的字段。这在比较两条路径的排序顺序是否一致时非常高效。
	 * 
	 * 匹配条件：
	 * - eclass: 排序的列属于同一个等价类。
	 * - opfamily: 使用相同的 B-Tree 操作符族（例如 integer_ops vs integer_ops）。
	 * - strategy: 排序方向相同（BTLessStrategyNumber 表示 ASC，BTGreaterStrategyNumber 表示 DESC）。
	 * - nulls_first: NULL 值的排序位置相同（NULLS FIRST vs NULLS LAST）。
	 */
	foreach(lc, root->canon_pathkeys)
	{
		pk = (PathKey *) lfirst(lc);
		if (eclass == pk->pk_eclass &&
			opfamily == pk->pk_opfamily &&
			strategy == pk->pk_strategy &&
			nulls_first == pk->pk_nulls_first)
			return pk;
	}

	/*
	 * 确保新建的 PathKey 分配在主规划内存上下文（planner_cxt）中。
	 * 在 GEQO（遗传优化）等特殊场景下尤为重要。
	 * 
	 * 详细解释：
	 * PathKey 对象需要在整个查询规划期间存活。
	 * 如果当前处于某个临时的内存上下文中（例如在处理某个子查询或临时计算），
	 * 直接分配内存可能会导致 PathKey 在后续被意外释放。
	 * 因此，必须切换到 root->planner_cxt，这是整个规划器的顶级内存上下文。
	 */
	oldcontext = MemoryContextSwitchTo(root->planner_cxt);

	pk = makeNode(PathKey);
	pk->pk_eclass = eclass;
	pk->pk_opfamily = opfamily;
	pk->pk_strategy = strategy;
	pk->pk_nulls_first = nulls_first;

	root->canon_pathkeys = lappend(root->canon_pathkeys, pk);

	MemoryContextSwitchTo(oldcontext);

	return pk;
}

/*
 * pathkey_is_redundant - 检查路径键是否在给定列表中冗余
 *
 * 该函数用于判断一个新的路径键是否与路径键列表中已有的路径键冗余。
 * 如果冗余，则可以在排序或索引匹配时忽略该路径键，从而优化查询执行计划。
 *
 * 参数说明:
 * - new_pathkey: 要检查的新路径键
 * - pathkeys: 现有的路径键列表
 *
 * 返回值:
 * - bool: 如果路径键冗余则返回true，否则返回false
 *
 * 冗余检测的两种情况:
 *
 * 1. 如果新路径键的等价类包含常量且不在外连接之下，则可以忽略它作为排序键。
 *    示例: SELECT ... WHERE x = 42 ORDER BY x, y;
 *    我们可以直接按y排序即可。由于操作符族匹配，这在语义上是正确的：
 *    我们知道等式约束实际上将变量绑定到单个值，对于与等价类相关的任何排序操作符都是如此。
 *    这个规则不仅让我们简化（甚至跳过）显式排序，还允许在存在无关紧要的索引列时，
 *    将索引排序顺序与查询匹配。
 *
 * 2. 如果新路径键的等价类与路径键列表中任何现有成员的等价类相同，则它是冗余的。
 *    示例:
 *      SELECT ... ORDER BY x, x;           -- 第二个x无法区分第一个x认为相等的值
 *      SELECT ... ORDER BY x, x DESC;      -- 同样无法提供更多区分信息
 *      SELECT ... WHERE x = y ORDER BY x, y; -- x和y相等，第二个排序键无意义
 *    特别注意的是，我们不需要比较操作符族（等价类的所有操作符族都有相同的相等概念）
 *    也不需要比较排序方向。
 *
 * 要求:
 * 给定的路径键和列表成员都必须是规范化的(canonical)，这样才能正常工作。
 * 但由于我们现在不再构造任何非规范化的路径键，所以这不是问题。
 * (注意：路径键列表被认为是规范化的，还包括没有冗余条目的额外要求，
 * 这正是我们在这里检查的内容。)
 *
 * 由于equivclass.c机制为每个查询只形成一个EC副本，
 * 指针比较就足够判断规范化的EC是否相同。
 */
static bool
pathkey_is_redundant(PathKey *new_pathkey, List *pathkeys)
{
	EquivalenceClass *new_ec = new_pathkey->pk_eclass;  /* 新路径键的等价类 */
	ListCell   *lc;  /* 列表遍历指针 */

	/* 
	 * 检查等价类是否包含常量 --- 无条件冗余 
	 * 
	 * 详细解释：
	 * 如果一个等价类（EC）包含常量（例如 WHERE x = 5），那么在这个 EC 中的所有列的值都必须等于该常量。
	 * 这意味着对于结果集中的每一行，这一列的值都是一样的。
	 * 既然值都一样，那么按照这一列进行排序是没有任何意义的（顺序不会改变）。
	 * 因此，这种 PathKey 是“冗余”的，可以被安全地忽略。
	 * 
	 * EC_MUST_BE_REDUNDANT 宏通常检查 ec_has_const 标志，但也需要排除外连接（Outer Join）下的情况
	 * （因为在外连接下，常量可能变成 NULL，见前文关于 ec_below_outer_join 的讨论）。
	 */
	if (EC_MUST_BE_REDUNDANT(new_ec))
		return true;

	/* 
	 * 如果列表中已使用相同的等价类，则冗余 
	 * 
	 * 详细解释：
	 * 假设我们有一个 PathKey 列表（pathkeys），代表当前的排序键序列，例如 (A, B)。
	 * 现在我们要检查是否需要添加一个新的 PathKey C。
	 * 
	 * 如果 C 所属的等价类（new_ec）已经出现在列表 (A, B) 中，例如 A 和 C 属于同一个 EC（即 A = C），
	 * 那么按照 A 排序之后，C 的值在每一组 A 相同的数据中也是相同的（因为 A=C）。
	 * 或者更准确地说，既然 A 和 C 等价，那么按 A 排序就已经隐含了按 C 排序。
	 * 
	 * 举例：SELECT * FROM t WHERE a = c ORDER BY a, c;
	 * 这里的 ORDER BY a, c 实际上等同于 ORDER BY a。
	 * 因为当 a 确定时，c 也就确定了（因为 a=c）。
	 * 所以第二个排序键 c 是冗余的。
	 * 
	 * 注意：这里只比较 pk_eclass 指针。因为 PathKey 已经被规范化（canonicalized），
	 * 相同的 EC 指针意味着相同的等价类。
	 */
	foreach(lc, pathkeys)
	{
		PathKey    *old_pathkey = (PathKey *) lfirst(lc);

		/* 通过指针比较等价类是否相同 */
		if (new_ec == old_pathkey->pk_eclass)
			return true;
	}

	/* 不冗余 */
	return false;
}


/*
 * make_pathkey_from_sortinfo
 *	  根据表达式和排序信息创建一个 PathKey。
 *	  返回的总是“规范化”的 PathKey，但可能是冗余的。
 *
 * expr: 排序表达式
 * nullable_relids: 该表达式下方可能为 NULL 的基表 relids 集合
 * opfamily: 排序操作符族
 * opcintype: 操作符输入类型
 * collation: 排序规则
 * reverse_sort: 是否为降序（true 表示降序，false 表示升序）
 * nulls_first: NULL 是否排在前面
 * sortref: 如果由 SortGroupClause 生成，则为其 SortGroupRef，否则为 0
 * rel: 指定具体关系的 relids，用于允许该关系的子成员参与等价类匹配，否则为 NULL
 * create_it: 如果为 true，则必要时创建缺失的等价类；否则如果找不到等价类则返回 NULL
 */
static PathKey *
make_pathkey_from_sortinfo(PlannerInfo *root,
						   Expr *expr,
						   Relids nullable_relids,
						   Oid opfamily,
						   Oid opcintype,
						   Oid collation,
						   bool reverse_sort,
						   bool nulls_first,
						   Index sortref,
						   Relids rel,
						   bool create_it)
{
	int16		strategy;
	Oid			equality_op;
	List	   *opfamilies;
	EquivalenceClass *eclass;

	/*
	 * 根据排序方向选择策略号（升序/降序）。
	 * BTLess (1): <，对应升序（ASC）。
	 * BTGreater (5): >，对应降序（DESC）。
	 * 如果 reverse_sort 为真，说明我们要降序，所以用 > 策略；否则用 < 策略。
	 */
	strategy = reverse_sort ? BTGreaterStrategyNumber : BTLessStrategyNumber;

	/*
	 * 等价类需要包含基于等值操作符族的 opfamily 列表，
	 * 因为等值操作符可能属于多个操作符族。
	 * 所以需要查找排序操作符族对应的等值操作符，并获取其所有 opfamily。
	 *
	 * 目的：为了找到这个排序操作符族（OpFamily）背后的“等值”概念。
	 * 为什么需要？
	 * PathKey 是基于等价类（EquivalenceClass, EC）构建的。EC 的核心定义是“相等”。
	 * 即使我们是在做排序（比如 ORDER BY a），优化器也需要知道 a 属于哪个等价类。
	 * 而等价类的定义依赖于等值操作符。
	 */
	equality_op = get_opfamily_member(opfamily,
									  opcintype,
									  opcintype,
									  BTEqualStrategyNumber);

	/* 如果没有找到等值操作符，则报错。理论上不应发生 */
	if (!OidIsValid(equality_op))
		elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
			 BTEqualStrategyNumber, opcintype, opcintype, opfamily);

	/*
	 * 获取合并连接操作符族列表。
	 * 目的：找到所有与这个等值操作符兼容的操作符族。
	 * 原因：一个操作符可能属于多个族。等价类（EC）需要知道所有兼容的族，
	 * 以便在不同表之间推导等价关系（比如 t1.a = t2.b）。
	 */
	opfamilies = get_mergejoin_opfamilies(equality_op);
	if (!opfamilies)			/* 一定要能找到 */
		elog(ERROR, "could not find opfamilies for equality operator %u",
			 equality_op);

	/*
	 * 查找或（可选）创建匹配的等价类。
	 * 核心步骤：
	 * 优化器检查现有的等价类列表中，是否已经有一个 EC 包含了当前的表达式 expr（比如 t1.a），
	 * 且兼容我们刚才找到的 opfamilies。
	 * - 如果找到了：直接复用这个 EC。
	 * - 如果没找到：
	 *   - 如果 create_it 为真：创建一个新的 EC，把 t1.a 放进去。
	 *   - 如果 create_it 为假：返回 NULL（放弃）。
	 */
	eclass = get_eclass_for_sort_expr(root, expr, nullable_relids,
									  opfamilies, opcintype, collation,
									  sortref, rel, create_it);

	/* 如果找不到等价类且不允许创建，则返回 NULL */
	if (!eclass)
		return NULL;

	/*
	 * 最后查找或创建规范化 PathKey 节点。
	 * 把所有信息打包成一个 PathKey 对象。
	 * 规范化（Canonical）：
	 * 优化器会维护一个全局的 PathKey 列表。如果已经有一个完全一样的 PathKey
	 * （同一个 EC，同一个 OpFamily，同一个方向），就直接返回那个现成的指针。
	 */
	return make_canonical_pathkey(root, eclass, opfamily,
								  strategy, nulls_first);
}

/*
 * make_pathkey_from_sortop
 *	  Like make_pathkey_from_sortinfo, but work from a sort operator.
 *
 * This should eventually go away, but we need to restructure SortGroupClause
 * first.
 */
static PathKey *
make_pathkey_from_sortop(PlannerInfo *root,
						 Expr *expr,
						 Relids nullable_relids,
						 Oid ordering_op,
						 bool nulls_first,
						 Index sortref,
						 bool create_it)
{
	Oid			opfamily,
				opcintype,
				collation;
	int16		strategy;

	/* Find the operator in pg_amop --- failure shouldn't happen */
	if (!get_ordering_op_properties(ordering_op,
									&opfamily, &opcintype, &strategy))
		elog(ERROR, "operator %u is not a valid ordering operator",
			 ordering_op);

	/* Because SortGroupClause doesn't carry collation, consult the expr */
	collation = exprCollation((Node *) expr);

	return make_pathkey_from_sortinfo(root,
									  expr,
									  nullable_relids,
									  opfamily,
									  opcintype,
									  collation,
									  (strategy == BTGreaterStrategyNumber),
									  nulls_first,
									  sortref,
									  NULL,
									  create_it);
}


/****************************************************************************
 *		PATHKEY COMPARISONS
 ****************************************************************************/

/*
 * compare_pathkeys
 *	  比较两个 pathkeys 列表，判断它们是否等价，如果不等价则判断哪一个“更好”。
 *
 *	  假设 pathkeys 都是规范化的，因此可以通过指针比较来判断是否相等。
 */
PathKeysComparison
compare_pathkeys(List *keys1, List *keys2)
{
	ListCell   *key1,
			   *key2;

	/*
	 * 如果传入的是同一个列表，直接返回相等。主要用于处理两个都是 NIL 的情况，这种情况很常见。
	 */
	if (keys1 == keys2)
		return PATHKEYS_EQUAL;

	/*
	 * 遍历两个列表，比较每个 PathKey 是否相等。
	 * 如果发现有不同的 PathKey，直接返回不同。
	 */
	forboth(key1, keys1, key2, keys2)
	{
		PathKey    *pathkey1 = (PathKey *) lfirst(key1);
		PathKey    *pathkey2 = (PathKey *) lfirst(key2);

		if (pathkey1 != pathkey2)
			return PATHKEYS_DIFFERENT;	/* 不需要继续比较，直接返回不同 */
	}

	/*
	 * 如果只到达了其中一个列表的末尾，说明另一个更长，因此不是子集关系。
	 * 返回相应的“更好”结果。
	 */
	if (key1 != NULL)
		return PATHKEYS_BETTER1;	/* key1 更长 */
	if (key2 != NULL)
		return PATHKEYS_BETTER2;	/* key2 更长 */

	/*
	 * 如果两个列表都遍历完了，说明它们是等价的。
	 */
	return PATHKEYS_EQUAL;
}

/*
 * pathkeys_contained_in
 *	  Common special case of compare_pathkeys: we just want to know
 *	  if keys2 are at least as well sorted as keys1.
 */
bool
pathkeys_contained_in(List *keys1, List *keys2)
{
	switch (compare_pathkeys(keys1, keys2))
	{
		case PATHKEYS_EQUAL:
		case PATHKEYS_BETTER2:
			return true;
		default:
			break;
	}
	return false;
}

/*
 * get_cheapest_path_for_pathkeys
 *	  Find the cheapest path (according to the specified criterion) that
 *	  satisfies the given pathkeys and parameterization.
 *	  Return NULL if no such path.
 *
 * 'paths' is a list of possible paths that all generate the same relation
 * 'pathkeys' represents a required ordering (in canonical form!)
 * 'required_outer' denotes allowable outer relations for parameterized paths
 * 'cost_criterion' is STARTUP_COST or TOTAL_COST
 * 'require_parallel_safe' causes us to consider only parallel-safe paths
 */
Path *
get_cheapest_path_for_pathkeys(List *paths, List *pathkeys,
							   Relids required_outer,
							   CostSelector cost_criterion,
							   bool require_parallel_safe)
{
	Path	   *matched_path = NULL;
	ListCell   *l;

	foreach(l, paths)
	{
		Path	   *path = (Path *) lfirst(l);

		/*
		 * Since cost comparison is a lot cheaper than pathkey comparison, do
		 * that first.  (XXX is that still true?)
		 */
		if (matched_path != NULL &&
			compare_path_costs(matched_path, path, cost_criterion) <= 0)
			continue;

		if (require_parallel_safe && !path->parallel_safe)
			continue;

		if (pathkeys_contained_in(pathkeys, path->pathkeys) &&
			bms_is_subset(PATH_REQ_OUTER(path), required_outer))
			matched_path = path;
	}
	return matched_path;
}

/*
 * get_cheapest_fractional_path_for_pathkeys
 *	  Find the cheapest path (for retrieving a specified fraction of all
 *	  the tuples) that satisfies the given pathkeys and parameterization.
 *	  Return NULL if no such path.
 *
 * See compare_fractional_path_costs() for the interpretation of the fraction
 * parameter.
 *
 * 'paths' is a list of possible paths that all generate the same relation
 * 'pathkeys' represents a required ordering (in canonical form!)
 * 'required_outer' denotes allowable outer relations for parameterized paths
 * 'fraction' is the fraction of the total tuples expected to be retrieved
 */
Path *
get_cheapest_fractional_path_for_pathkeys(List *paths,
										  List *pathkeys,
										  Relids required_outer,
										  double fraction)
{
	Path	   *matched_path = NULL;
	ListCell   *l;

	foreach(l, paths)
	{
		Path	   *path = (Path *) lfirst(l);

		/*
		 * Since cost comparison is a lot cheaper than pathkey comparison, do
		 * that first.  (XXX is that still true?)
		 */
		if (matched_path != NULL &&
			compare_fractional_path_costs(matched_path, path, fraction) <= 0)
			continue;

		if (pathkeys_contained_in(pathkeys, path->pathkeys) &&
			bms_is_subset(PATH_REQ_OUTER(path), required_outer))
			matched_path = path;
	}
	return matched_path;
}


/*
 * get_cheapest_parallel_safe_total_inner - 查找最便宜的并行安全总内部路径
 *
 * 该函数用于在路径列表中查找未参数化的并行安全路径中总成本最低的路径。
 * 这在并行查询优化中非常重要，因为只有并行安全的路径才能在并行执行中使用。
 *
 * 参数说明:
 * - paths: 路径列表，包含多个可能的执行路径
 *
 * 返回值:
 * - Path*: 返回找到的最便宜的并行安全内部路径，如果未找到则返回NULL
 *
 * 工作原理:
 * 1. 遍历路径列表中的每个路径
 * 2. 检查路径是否满足两个条件：
 *    a) parallel_safe: 路径是否支持并行执行
 *    b) 未参数化：PATH_REQ_OUTER返回空，表示不依赖外部参数
 * 3. 返回第一个满足条件的路径（由于路径列表通常按成本排序，第一个就是最便宜的）
 *
 * 注意事项:
 * - 该函数假设输入的路径列表已经按成本排序
 * - 只返回完全独立的路径（无参数依赖），确保可以在并行工作进程中安全执行
 * - 如果没有找到满足条件的路径，返回NULL
 */
Path *
get_cheapest_parallel_safe_total_inner(List *paths)
{
	ListCell   *l;  /* 列表遍历指针 */

	/* 遍历路径列表中的每个路径 */
	foreach(l, paths)
	{
		Path	   *innerpath = (Path *) lfirst(l);

		/* 
		 * 检查路径是否满足并行安全和未参数化条件：
		 * 1. parallel_safe: 路径必须标记为并行安全
		 * 2. bms_is_empty(PATH_REQ_OUTER(innerpath)): 路径不能有外部参数依赖
		 */
		if (innerpath->parallel_safe &&
			bms_is_empty(PATH_REQ_OUTER(innerpath)))
			return innerpath;  /* 返回第一个满足条件的路径 */
	}

	/* 如果没有找到满足条件的路径，返回NULL */
	return NULL;
}


/****************************************************************************
 *		NEW PATHKEY FORMATION
 ****************************************************************************/

/*
 * build_index_pathkeys
 *	  构建一个 pathkeys 列表，描述使用指定索引扫描时所产生的排序顺序。
 *	  （注意：无序索引不会产生任何排序，因此直接返回 NIL。）
 *
 * 如果 'scandir' 为 BackwardScanDirection，则构建代表索引反向扫描的 pathkeys。
 *
 * 只遍历覆盖索引的 key 列，因为非 key 列不会影响索引的排序顺序。
 * 结果是规范化的（canonical），即会去除冗余的 pathkey，因此返回的 pathkey 数量
 * 可能少于索引的 key 列数。
 *
 * 另一个提前终止的原因是：如果我们能判断某个索引列的排序对本查询无意义，
 * 就可以提前停止。但这种判断仅基于等价类（EquivalenceClass）的存在，
 * 并不考虑 pathkey 列表中的具体位置，因此并不完全。调用者应再调用
 * truncate_useless_pathkeys() 以进一步去除无用的 pathkey。
 */
List *
build_index_pathkeys(PlannerInfo *root,
					 IndexOptInfo *index,
					 ScanDirection scandir)
{
	List	   *retval = NIL;
	ListCell   *lc;
	int			i;

	/*
     * sortopfamily 为 NULL 时，索引不支持排序，直接返回 NIL
	 */
	if (index->sortopfamily == NULL)
		return NIL;

	i = 0;
	foreach(lc, index->indextlist)
	{
		TargetEntry *indextle = (TargetEntry *) lfirst(lc);
		Expr	   *indexkey;
		bool		reverse_sort;
		bool		nulls_first;
		PathKey    *cpathkey;

		/*
		 * INCLUDE 列在索引中是无序存储的，不支持有序索引扫描。
		 * 例如：CREATE INDEX idx ON t(a) INCLUDE (b)
		 * 这里 nkeycolumns 是 1。b 是 INCLUDE 列，它只是作为负载存储，不参与排序。
		 * 因此，一旦遍历到 INCLUDE 列，我们就停止构建 PathKeys。
		 */
		if (i >= index->nkeycolumns)
			break;

		/* 直接取 tlist 项，无需拷贝 */
		indexkey = indextle->expr;

		/*
		 * 根据扫描方向调整排序和 NULLS 位置。
		 * B-Tree 索引支持双向扫描。
		 *
		 * 正向扫描 (ForwardScanDirection):
		 * 直接使用索引定义中的排序属性。如果索引定义是 ASC，扫描出来就是 ASC。
		 *
		 * 反向扫描 (BackwardScanDirection):
		 * 属性取反！
		 * 如果索引定义是 ASC（升序），反向扫描出来的结果就是 DESC（降序）。
		 * 如果索引定义是 NULLS FIRST，反向扫描出来的结果就是 NULLS LAST。
		 * 
		 * 这使得优化器可以利用同一个索引来满足 ORDER BY a DESC 的需求（即使索引是 a ASC 建的）。
		 */
		if (ScanDirectionIsBackward(scandir))
		{
			/* 反向扫描，属性取反 */
			reverse_sort = !index->reverse_sort[i];
			nulls_first = !index->nulls_first[i];
		}
		else
		{
			reverse_sort = index->reverse_sort[i];
			nulls_first = index->nulls_first[i];
		}

		/*
		 * 尝试为该排序键构建规范化 pathkey。注意此处在任何外连接之下，
		 * 所以 nullable_relids 传 NULL。
		 */
		cpathkey = make_pathkey_from_sortinfo(root,
											  indexkey,
											  NULL,
											  index->sortopfamily[i],
											  index->opcintype[i],
											  index->indexcollations[i],
											  reverse_sort,
											  nulls_first,
											  0,
											  index->rel->relids,
											  false);

		if (cpathkey)
		{
			/*
			 * 找到了等价类中的排序键，说明对本查询有意义。
			 * 如果不冗余，则加入结果列表。
			 */
			if (!pathkey_is_redundant(cpathkey, retval))
				retval = lappend(retval, cpathkey);
		}
		else
		{
			/*
			 * 布尔类型的索引键即使不在等价类中，也可能是冗余的，
			 * 参见 indexcol_is_bool_constant_for_query() 的注释。
			 * 如果是这种情况，可以继续处理低阶索引列；
			 * 否则，说明该排序键对本查询无意义，后续索引列也不会有用，直接停止。
			 *
			 * 详细解释：
			 * 通常情况下，如果索引的某一列（例如第 i 列）没有对应的 PathKey（即 cpathkey 为 NULL），
			 * 这意味着查询并没有要求按这一列排序，也没有 WHERE 条件约束这一列等于某个常量。
			 * 在这种情况下，索引的排序顺序在这一列之后就“断掉”了。
			 * 例如：索引是 (a, b)，查询是 ORDER BY a。
			 * 当处理到列 b 时，发现没有 PathKey，通常我们会停止，因为 b 的顺序对查询没用。
			 *
			 * 但是，对于布尔类型的列，有一个特殊情况：
			 * 如果查询中有类似 "WHERE bool_col" 或 "WHERE NOT bool_col" 这样的条件，
			 * 虽然这看起来不像 "bool_col = true" 这种标准的等值约束（可能不会生成包含常量的 EC），
			 * 但实际上它限制了 bool_col 必须为 true（或 false）。
			 *
			 * indexcol_is_bool_constant_for_query() 就是用来检测这种情况的。
			 * 如果它返回 true，说明这一列实际上被约束为了常量。
			 * 既然是常量，它就是冗余的（就像 pathkey_is_redundant 处理的那样）。
			 * 我们可以跳过这一列，继续查看索引的下一列是否能提供有用的排序。
			 *
			 * 举例：索引 (bool_col, x)，查询 SELECT * FROM t WHERE bool_col ORDER BY x;
			 * 1. 处理 bool_col：没有显式的 ORDER BY bool_col，也没有 bool_col = const 的 EC。
			 *    但 WHERE bool_col 隐含了 bool_col = true。
			 *    indexcol_is_bool_constant_for_query 返回 true。
			 *    我们跳过 bool_col，继续处理下一列。
			 * 2. 处理 x：发现匹配 ORDER BY x。
			 * 3. 结果：我们可以利用这个索引来满足 ORDER BY x。
			 */
			if (!indexcol_is_bool_constant_for_query(root, index, i))
				break;
		}

		i++;
	}

	return retval;
}

/*
 * partkey_is_bool_constant_for_query
 *
 * If a partition key column is constrained to have a constant value by the
 * query's WHERE conditions, then it's irrelevant for sort-order
 * considerations.  Usually that means we have a restriction clause
 * WHERE partkeycol = constant, which gets turned into an EquivalenceClass
 * containing a constant, which is recognized as redundant by
 * build_partition_pathkeys().  But if the partition key column is a
 * boolean variable (or expression), then we are not going to see such a
 * WHERE clause, because expression preprocessing will have simplified it
 * to "WHERE partkeycol" or "WHERE NOT partkeycol".  So we are not going
 * to have a matching EquivalenceClass (unless the query also contains
 * "ORDER BY partkeycol").  To allow such cases to work the same as they would
 * for non-boolean values, this function is provided to detect whether the
 * specified partition key column matches a boolean restriction clause.
 */
static bool
partkey_is_bool_constant_for_query(RelOptInfo *partrel, int partkeycol)
{
	PartitionScheme partscheme = partrel->part_scheme;
	ListCell   *lc;

	/* If the partkey isn't boolean, we can't possibly get a match */
	if (!IsBooleanOpfamily(partscheme->partopfamily[partkeycol]))
		return false;

	/* Check each restriction clause for the partitioned rel */
	foreach(lc, partrel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		/* Ignore pseudoconstant quals, they won't match */
		if (rinfo->pseudoconstant)
			continue;

		/* See if we can match the clause's expression to the partkey column */
		if (matches_boolean_partition_clause(rinfo, partrel, partkeycol))
			return true;
	}

	return false;
}

/*
 * matches_boolean_partition_clause
 *		Determine if the boolean clause described by rinfo matches
 *		partrel's partkeycol-th partition key column.
 *
 * "Matches" can be either an exact match (equivalent to partkey = true),
 * or a NOT above an exact match (equivalent to partkey = false).
 */
static bool
matches_boolean_partition_clause(RestrictInfo *rinfo,
								 RelOptInfo *partrel, int partkeycol)
{
	Node	   *clause = (Node *) rinfo->clause;
	Node	   *partexpr = (Node *) linitial(partrel->partexprs[partkeycol]);

	/* Direct match? */
	if (equal(partexpr, clause))
		return true;
	/* NOT clause? */
	else if (is_notclause(clause))
	{
		Node	   *arg = (Node *) get_notclausearg((Expr *) clause);

		if (equal(partexpr, arg))
			return true;
	}

	return false;
}

/*
 * build_partition_pathkeys
 *	  Build a pathkeys list that describes the ordering induced by the
 *	  partitions of partrel, under either forward or backward scan
 *	  as per scandir.
 *
 * Caller must have checked that the partitions are properly ordered,
 * as detected by partitions_are_ordered().
 *
 * Sets *partialkeys to true if pathkeys were only built for a prefix of the
 * partition key, or false if the pathkeys include all columns of the
 * partition key.
 */
List *
build_partition_pathkeys(PlannerInfo *root, RelOptInfo *partrel,
						 ScanDirection scandir, bool *partialkeys)
{
	List	   *retval = NIL;
	PartitionScheme partscheme = partrel->part_scheme;
	int			i;

	Assert(partscheme != NULL);
	Assert(partitions_are_ordered(partrel->boundinfo, partrel->nparts));
	/* For now, we can only cope with baserels */
	Assert(IS_SIMPLE_REL(partrel));

	for (i = 0; i < partscheme->partnatts; i++)
	{
		PathKey    *cpathkey;
		Expr	   *keyCol = (Expr *) linitial(partrel->partexprs[i]);

		/*
		 * Try to make a canonical pathkey for this partkey.
		 *
		 * We're considering a baserel scan, so nullable_relids should be
		 * NULL.  Also, we assume the PartitionDesc lists any NULL partition
		 * last, so we treat the scan like a NULLS LAST index: we have
		 * nulls_first for backwards scan only.
		 */
		cpathkey = make_pathkey_from_sortinfo(root,
											  keyCol,
											  NULL,
											  partscheme->partopfamily[i],
											  partscheme->partopcintype[i],
											  partscheme->partcollation[i],
											  ScanDirectionIsBackward(scandir),
											  ScanDirectionIsBackward(scandir),
											  0,
											  partrel->relids,
											  false);


		if (cpathkey)
		{
			/*
			 * We found the sort key in an EquivalenceClass, so it's relevant
			 * for this query.  Add it to list, unless it's redundant.
			 */
			if (!pathkey_is_redundant(cpathkey, retval))
				retval = lappend(retval, cpathkey);
		}
		else
		{
			/*
			 * Boolean partition keys might be redundant even if they do not
			 * appear in an EquivalenceClass, because of our special treatment
			 * of boolean equality conditions --- see the comment for
			 * partkey_is_bool_constant_for_query().  If that applies, we can
			 * continue to examine lower-order partition keys.  Otherwise, the
			 * sort key is not an interesting sort order for this query, so we
			 * should stop considering partition columns; any lower-order sort
			 * keys won't be useful either.
			 */
			if (!partkey_is_bool_constant_for_query(partrel, i))
			{
				*partialkeys = true;
				return retval;
			}
		}
	}

	*partialkeys = false;
	return retval;
}

/*
 * build_expression_pathkey
 *	  Build a pathkeys list that describes an ordering by a single expression
 *	  using the given sort operator.
 *
 * expr, nullable_relids, and rel are as for make_pathkey_from_sortinfo.
 * We induce the other arguments assuming default sort order for the operator.
 *
 * Similarly to make_pathkey_from_sortinfo, the result is NIL if create_it
 * is false and the expression isn't already in some EquivalenceClass.
 */
List *
build_expression_pathkey(PlannerInfo *root,
						 Expr *expr,
						 Relids nullable_relids,
						 Oid opno,
						 Relids rel,
						 bool create_it)
{
	List	   *pathkeys;
	Oid			opfamily,
				opcintype;
	int16		strategy;
	PathKey    *cpathkey;

	/* Find the operator in pg_amop --- failure shouldn't happen */
	if (!get_ordering_op_properties(opno,
									&opfamily, &opcintype, &strategy))
		elog(ERROR, "operator %u is not a valid ordering operator",
			 opno);

	cpathkey = make_pathkey_from_sortinfo(root,
										  expr,
										  nullable_relids,
										  opfamily,
										  opcintype,
										  exprCollation((Node *) expr),
										  (strategy == BTGreaterStrategyNumber),
										  (strategy == BTGreaterStrategyNumber),
										  0,
										  rel,
										  create_it);

	if (cpathkey)
		pathkeys = list_make1(cpathkey);
	else
		pathkeys = NIL;

	return pathkeys;
}

/*
 * convert_subquery_pathkeys
 *	  Build a pathkeys list that describes the ordering of a subquery's
 *	  result, in the terms of the outer query.  This is essentially a
 *	  task of conversion.
 *
 * 'rel': outer query's RelOptInfo for the subquery relation.
 * 'subquery_pathkeys': the subquery's output pathkeys, in its terms.
 * 'subquery_tlist': the subquery's output targetlist, in its terms.
 *
 * We intentionally don't do truncate_useless_pathkeys() here, because there
 * are situations where seeing the raw ordering of the subquery is helpful.
 * For example, if it returns ORDER BY x DESC, that may prompt us to
 * construct a mergejoin using DESC order rather than ASC order; but the
 * right_merge_direction heuristic would have us throw the knowledge away.
 */
List *
convert_subquery_pathkeys(PlannerInfo *root, RelOptInfo *rel,
						  List *subquery_pathkeys,
						  List *subquery_tlist)
{
	List	   *retval = NIL;
	int			retvallen = 0;
	int			outer_query_keys = list_length(root->query_pathkeys);
	ListCell   *i;

	foreach(i, subquery_pathkeys)
	{
		PathKey    *sub_pathkey = (PathKey *) lfirst(i);
		EquivalenceClass *sub_eclass = sub_pathkey->pk_eclass;
		PathKey    *best_pathkey = NULL;

		if (sub_eclass->ec_has_volatile)
		{
			/*
			 * If the sub_pathkey's EquivalenceClass is volatile, then it must
			 * have come from an ORDER BY clause, and we have to match it to
			 * that same targetlist entry.
			 */
			TargetEntry *tle;
			Var		   *outer_var;

			if (sub_eclass->ec_sortref == 0)	/* can't happen */
				elog(ERROR, "volatile EquivalenceClass has no sortref");
			tle = get_sortgroupref_tle(sub_eclass->ec_sortref, subquery_tlist);
			Assert(tle);
			/* Is TLE actually available to the outer query? */
			outer_var = find_var_for_subquery_tle(rel, tle);
			if (outer_var)
			{
				/* We can represent this sub_pathkey */
				EquivalenceMember *sub_member;
				EquivalenceClass *outer_ec;

				Assert(list_length(sub_eclass->ec_members) == 1);
				sub_member = (EquivalenceMember *) linitial(sub_eclass->ec_members);

				/*
				 * Note: it might look funny to be setting sortref = 0 for a
				 * reference to a volatile sub_eclass.  However, the
				 * expression is *not* volatile in the outer query: it's just
				 * a Var referencing whatever the subquery emitted. (IOW, the
				 * outer query isn't going to re-execute the volatile
				 * expression itself.)	So this is okay.  Likewise, it's
				 * correct to pass nullable_relids = NULL, because we're
				 * underneath any outer joins appearing in the outer query.
				 */
				outer_ec =
					get_eclass_for_sort_expr(root,
											 (Expr *) outer_var,
											 NULL,
											 sub_eclass->ec_opfamilies,
											 sub_member->em_datatype,
											 sub_eclass->ec_collation,
											 0,
											 rel->relids,
											 false);

				/*
				 * If we don't find a matching EC, sub-pathkey isn't
				 * interesting to the outer query
				 */
				if (outer_ec)
					best_pathkey =
						make_canonical_pathkey(root,
											   outer_ec,
											   sub_pathkey->pk_opfamily,
											   sub_pathkey->pk_strategy,
											   sub_pathkey->pk_nulls_first);
			}
		}
		else
		{
			/*
			 * Otherwise, the sub_pathkey's EquivalenceClass could contain
			 * multiple elements (representing knowledge that multiple items
			 * are effectively equal).  Each element might match none, one, or
			 * more of the output columns that are visible to the outer query.
			 * This means we may have multiple possible representations of the
			 * sub_pathkey in the context of the outer query.  Ideally we
			 * would generate them all and put them all into an EC of the
			 * outer query, thereby propagating equality knowledge up to the
			 * outer query.  Right now we cannot do so, because the outer
			 * query's EquivalenceClasses are already frozen when this is
			 * called. Instead we prefer the one that has the highest "score"
			 * (number of EC peers, plus one if it matches the outer
			 * query_pathkeys). This is the most likely to be useful in the
			 * outer query.
			 */
			int			best_score = -1;
			ListCell   *j;

			foreach(j, sub_eclass->ec_members)
			{
				EquivalenceMember *sub_member = (EquivalenceMember *) lfirst(j);
				Expr	   *sub_expr = sub_member->em_expr;
				Oid			sub_expr_type = sub_member->em_datatype;
				Oid			sub_expr_coll = sub_eclass->ec_collation;
				ListCell   *k;

				if (sub_member->em_is_child)
					continue;	/* ignore children here */

				foreach(k, subquery_tlist)
				{
					TargetEntry *tle = (TargetEntry *) lfirst(k);
					Var		   *outer_var;
					Expr	   *tle_expr;
					EquivalenceClass *outer_ec;
					PathKey    *outer_pk;
					int			score;

					/* Is TLE actually available to the outer query? */
					outer_var = find_var_for_subquery_tle(rel, tle);
					if (!outer_var)
						continue;

					/*
					 * The targetlist entry is considered to match if it
					 * matches after sort-key canonicalization.  That is
					 * needed since the sub_expr has been through the same
					 * process.
					 */
					tle_expr = canonicalize_ec_expression(tle->expr,
														  sub_expr_type,
														  sub_expr_coll);
					if (!equal(tle_expr, sub_expr))
						continue;

					/* See if we have a matching EC for the TLE */
					outer_ec = get_eclass_for_sort_expr(root,
														(Expr *) outer_var,
														NULL,
														sub_eclass->ec_opfamilies,
														sub_expr_type,
														sub_expr_coll,
														0,
														rel->relids,
														false);

					/*
					 * If we don't find a matching EC, this sub-pathkey isn't
					 * interesting to the outer query
					 */
					if (!outer_ec)
						continue;

					outer_pk = make_canonical_pathkey(root,
													  outer_ec,
													  sub_pathkey->pk_opfamily,
													  sub_pathkey->pk_strategy,
													  sub_pathkey->pk_nulls_first);
					/* score = # of equivalence peers */
					score = list_length(outer_ec->ec_members) - 1;
					/* +1 if it matches the proper query_pathkeys item */
					if (retvallen < outer_query_keys &&
						list_nth(root->query_pathkeys, retvallen) == outer_pk)
						score++;
					if (score > best_score)
					{
						best_pathkey = outer_pk;
						best_score = score;
					}
				}
			}
		}

		/*
		 * If we couldn't find a representation of this sub_pathkey, we're
		 * done (we can't use the ones to its right, either).
		 */
		if (!best_pathkey)
			break;

		/*
		 * Eliminate redundant ordering info; could happen if outer query
		 * equivalences subquery keys...
		 */
		if (!pathkey_is_redundant(best_pathkey, retval))
		{
			retval = lappend(retval, best_pathkey);
			retvallen++;
		}
	}

	return retval;
}

/*
 * find_var_for_subquery_tle
 *
 * If the given subquery tlist entry is due to be emitted by the subquery's
 * scan node, return a Var for it, else return NULL.
 *
 * We need this to ensure that we don't return pathkeys describing values
 * that are unavailable above the level of the subquery scan.
 */
static Var *
find_var_for_subquery_tle(RelOptInfo *rel, TargetEntry *tle)
{
	ListCell   *lc;

	/* If the TLE is resjunk, it's certainly not visible to the outer query */
	if (tle->resjunk)
		return NULL;

	/* Search the rel's targetlist to see what it will return */
	foreach(lc, rel->reltarget->exprs)
	{
		Var		   *var = (Var *) lfirst(lc);

		/* Ignore placeholders */
		if (!IsA(var, Var))
			continue;
		Assert(var->varno == rel->relid);

		/* If we find a Var referencing this TLE, we're good */
		if (var->varattno == tle->resno)
			return copyObject(var); /* Make a copy for safety */
	}
	return NULL;
}

/*
 * build_join_pathkeys
 *	  Build the path keys for a join relation constructed by mergejoin or
 *	  nestloop join.  This is normally the same as the outer path's keys.
 *
 *	  EXCEPTION: in a FULL or RIGHT join, we cannot treat the result as
 *	  having the outer path's path keys, because null lefthand rows may be
 *	  inserted at random points.  It must be treated as unsorted.
 *
 *	  We truncate away any pathkeys that are uninteresting for higher joins.
 *
 * 'joinrel' is the join relation that paths are being formed for
 * 'jointype' is the join type (inner, left, full, etc)
 * 'outer_pathkeys' is the list of the current outer path's path keys
 *
 * Returns the list of new path keys.
 */
List *
build_join_pathkeys(PlannerInfo *root,
					RelOptInfo *joinrel,
					JoinType jointype,
					List *outer_pathkeys)
{
	if (jointype == JOIN_FULL || jointype == JOIN_RIGHT)
		return NIL;

	/*
	 * This used to be quite a complex bit of code, but now that all pathkey
	 * sublists start out life canonicalized, we don't have to do a darn thing
	 * here!
	 *
	 * We do, however, need to truncate the pathkeys list, since it may
	 * contain pathkeys that were useful for forming this joinrel but are
	 * uninteresting to higher levels.
	 */
	return truncate_useless_pathkeys(root, joinrel, outer_pathkeys);
}

/****************************************************************************
 *		PATHKEYS AND SORT CLAUSES
 ****************************************************************************/

/*
 * make_pathkeys_for_sortclauses
 *		Generate a pathkeys list that represents the sort order specified
 *		by a list of SortGroupClauses
 *
 * The resulting PathKeys are always in canonical form.  (Actually, there
 * is no longer any code anywhere that creates non-canonical PathKeys.)
 *
 * We assume that root->nullable_baserels is the set of base relids that could
 * have gone to NULL below the SortGroupClause expressions.  This is okay if
 * the expressions came from the query's top level (ORDER BY, DISTINCT, etc)
 * and if this function is only invoked after deconstruct_jointree.  In the
 * future we might have to make callers pass in the appropriate
 * nullable-relids set, but for now it seems unnecessary.
 *
 * 'sortclauses' is a list of SortGroupClause nodes
 * 'tlist' is the targetlist to find the referenced tlist entries in
 */
List *
make_pathkeys_for_sortclauses(PlannerInfo *root,
							  List *sortclauses,
							  List *tlist)
{
	List	   *pathkeys = NIL;
	ListCell   *l;

	foreach(l, sortclauses)
	{
		SortGroupClause *sortcl = (SortGroupClause *) lfirst(l);
		Expr	   *sortkey;
		PathKey    *pathkey;

		sortkey = (Expr *) get_sortgroupclause_expr(sortcl, tlist);
		Assert(OidIsValid(sortcl->sortop));
		pathkey = make_pathkey_from_sortop(root,
										   sortkey,
										   root->nullable_baserels,
										   sortcl->sortop,
										   sortcl->nulls_first,
										   sortcl->tleSortGroupRef,
										   true);

		/* Canonical form eliminates redundant ordering keys */
		if (!pathkey_is_redundant(pathkey, pathkeys))
			pathkeys = lappend(pathkeys, pathkey);
	}
	return pathkeys;
}

/****************************************************************************
 *		PATHKEYS AND MERGECLAUSES
 ****************************************************************************/

/*
 * initialize_mergeclause_eclasses
 *		Set the EquivalenceClass links in a mergeclause restrictinfo.
 *
 * RestrictInfo contains fields in which we may cache pointers to
 * EquivalenceClasses for the left and right inputs of the mergeclause.
 * (If the mergeclause is a true equivalence clause these will be the
 * same EquivalenceClass, otherwise not.)  If the mergeclause is either
 * used to generate an EquivalenceClass, or derived from an EquivalenceClass,
 * then it's easy to set up the left_ec and right_ec members --- otherwise,
 * this function should be called to set them up.  We will generate new
 * EquivalenceClauses if necessary to represent the mergeclause's left and
 * right sides.
 *
 * Note this is called before EC merging is complete, so the links won't
 * necessarily point to canonical ECs.  Before they are actually used for
 * anything, update_mergeclause_eclasses must be called to ensure that
 * they've been updated to point to canonical ECs.
 */
void
initialize_mergeclause_eclasses(PlannerInfo *root, RestrictInfo *restrictinfo)
{
	Expr	   *clause = restrictinfo->clause;
	Oid			lefttype,
				righttype;

	/* Should be a mergeclause ... */
	Assert(restrictinfo->mergeopfamilies != NIL);
	/* ... with links not yet set */
	Assert(restrictinfo->left_ec == NULL);
	Assert(restrictinfo->right_ec == NULL);

	/* Need the declared input types of the operator */
	op_input_types(((OpExpr *) clause)->opno, &lefttype, &righttype);

	/* Find or create a matching EquivalenceClass for each side */
	restrictinfo->left_ec =
		get_eclass_for_sort_expr(root,
								 (Expr *) get_leftop(clause),
								 restrictinfo->nullable_relids,
								 restrictinfo->mergeopfamilies,
								 lefttype,
								 ((OpExpr *) clause)->inputcollid,
								 0,
								 NULL,
								 true);
	restrictinfo->right_ec =
		get_eclass_for_sort_expr(root,
								 (Expr *) get_rightop(clause),
								 restrictinfo->nullable_relids,
								 restrictinfo->mergeopfamilies,
								 righttype,
								 ((OpExpr *) clause)->inputcollid,
								 0,
								 NULL,
								 true);
}

/*
 * update_mergeclause_eclasses
 *		Make the cached EquivalenceClass links valid in a mergeclause
 *		restrictinfo.
 *
 * These pointers should have been set by process_equivalence or
 * initialize_mergeclause_eclasses, but they might have been set to
 * non-canonical ECs that got merged later.  Chase up to the canonical
 * merged parent if so.
 */
void
update_mergeclause_eclasses(PlannerInfo *root, RestrictInfo *restrictinfo)
{
	/* Should be a merge clause ... */
	Assert(restrictinfo->mergeopfamilies != NIL);
	/* ... with pointers already set */
	Assert(restrictinfo->left_ec != NULL);
	Assert(restrictinfo->right_ec != NULL);

	/* Chase up to the top as needed */
	while (restrictinfo->left_ec->ec_merged)
		restrictinfo->left_ec = restrictinfo->left_ec->ec_merged;
	while (restrictinfo->right_ec->ec_merged)
		restrictinfo->right_ec = restrictinfo->right_ec->ec_merged;
}

/*
 * find_mergeclauses_for_outer_pathkeys
 *	  This routine attempts to find a list of mergeclauses that can be
 *	  used with a specified ordering for the join's outer relation.
 *	  If successful, it returns a list of mergeclauses.
 *
 * 'pathkeys' is a pathkeys list showing the ordering of an outer-rel path.
 * 'restrictinfos' is a list of mergejoinable restriction clauses for the
 *			join relation being formed, in no particular order.
 *
 * The restrictinfos must be marked (via outer_is_left) to show which side
 * of each clause is associated with the current outer path.  (See
 * select_mergejoin_clauses())
 *
 * The result is NIL if no merge can be done, else a maximal list of
 * usable mergeclauses (represented as a list of their restrictinfo nodes).
 * The list is ordered to match the pathkeys, as required for execution.
 */
List *
find_mergeclauses_for_outer_pathkeys(PlannerInfo *root,
									 List *pathkeys,
									 List *restrictinfos)
{
	List	   *mergeclauses = NIL;
	ListCell   *i;

	/* make sure we have eclasses cached in the clauses */
	foreach(i, restrictinfos)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(i);

		update_mergeclause_eclasses(root, rinfo);
	}

	foreach(i, pathkeys)
	{
		PathKey    *pathkey = (PathKey *) lfirst(i);
		EquivalenceClass *pathkey_ec = pathkey->pk_eclass;
		List	   *matched_restrictinfos = NIL;
		ListCell   *j;

		/*----------
		 * A mergejoin clause matches a pathkey if it has the same EC.
		 * If there are multiple matching clauses, take them all.  In plain
		 * inner-join scenarios we expect only one match, because
		 * equivalence-class processing will have removed any redundant
		 * mergeclauses.  However, in outer-join scenarios there might be
		 * multiple matches.  An example is
		 *
		 *	select * from a full join b
		 *		on a.v1 = b.v1 and a.v2 = b.v2 and a.v1 = b.v2;
		 *
		 * Given the pathkeys ({a.v1}, {a.v2}) it is okay to return all three
		 * clauses (in the order a.v1=b.v1, a.v1=b.v2, a.v2=b.v2) and indeed
		 * we *must* do so or we will be unable to form a valid plan.
		 *
		 * We expect that the given pathkeys list is canonical, which means
		 * no two members have the same EC, so it's not possible for this
		 * code to enter the same mergeclause into the result list twice.
		 *
		 * It's possible that multiple matching clauses might have different
		 * ECs on the other side, in which case the order we put them into our
		 * result makes a difference in the pathkeys required for the inner
		 * input rel.  However this routine hasn't got any info about which
		 * order would be best, so we don't worry about that.
		 *
		 * It's also possible that the selected mergejoin clauses produce
		 * a noncanonical ordering of pathkeys for the inner side, ie, we
		 * might select clauses that reference b.v1, b.v2, b.v1 in that
		 * order.  This is not harmful in itself, though it suggests that
		 * the clauses are partially redundant.  Since the alternative is
		 * to omit mergejoin clauses and thereby possibly fail to generate a
		 * plan altogether, we live with it.  make_inner_pathkeys_for_merge()
		 * has to delete duplicates when it constructs the inner pathkeys
		 * list, and we also have to deal with such cases specially in
		 * create_mergejoin_plan().
		 *----------
		 */
		foreach(j, restrictinfos)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(j);
			EquivalenceClass *clause_ec;

			clause_ec = rinfo->outer_is_left ?
				rinfo->left_ec : rinfo->right_ec;
			if (clause_ec == pathkey_ec)
				matched_restrictinfos = lappend(matched_restrictinfos, rinfo);
		}

		/*
		 * If we didn't find a mergeclause, we're done --- any additional
		 * sort-key positions in the pathkeys are useless.  (But we can still
		 * mergejoin if we found at least one mergeclause.)
		 */
		if (matched_restrictinfos == NIL)
			break;

		/*
		 * If we did find usable mergeclause(s) for this sort-key position,
		 * add them to result list.
		 */
		mergeclauses = list_concat(mergeclauses, matched_restrictinfos);
	}

	return mergeclauses;
}
/*
 * select_outer_pathkeys_for_merge
 *	  构建一个 pathkey 列表，表示可以用于给定 mergeclauses 的外部排序顺序。
 *
 * 'mergeclauses' 是用于 merge join 的 RestrictInfo 列表。
 * 'joinrel' 是我们尝试构建的连接关系。
 *
 * RestrictInfo 必须通过 outer_is_left 标记，指示每个子句的外部路径是哪一侧。
 * （参见 select_mergejoin_clauses()）
 *
 * 返回可应用于外部关系的 pathkeys 列表。
 *
 * 由于这里假设需要排序，因此无需匹配外部关系已有的排序顺序（joinpath.c 有专门处理无需排序的 mergejoin 的代码路径）。
 * 更有意义的是尝试匹配 query_pathkeys，这样可以避免二次排序输出；如果无法匹配，则优先列出“更受欢迎”的键
 * （即未匹配的 EquivalenceClass 成员最多的键），以期使结果排序对更多高层 mergejoin 有用。
 */
List *
select_outer_pathkeys_for_merge(PlannerInfo *root,
								List *mergeclauses,
								RelOptInfo *joinrel)
{
	List	   *pathkeys = NIL;
	int			nClauses = list_length(mergeclauses);
	EquivalenceClass **ecs;
	int		   *scores;
	int			necs;
	ListCell   *lc;
	int			j;

	/* 可能没有 mergeclauses */
	if (nClauses == 0)
		return NIL;

	/*
	 * 构建 mergeclauses 使用的 EC 数组（去重）及其“受欢迎度”分数。
	 */
	ecs = (EquivalenceClass **) palloc(nClauses * sizeof(EquivalenceClass *));
	scores = (int *) palloc(nClauses * sizeof(int));
	necs = 0;

	foreach(lc, mergeclauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		EquivalenceClass *oeclass;
		int			score;
		ListCell   *lc2;

		/* 获取外部 eclass */
		update_mergeclause_eclasses(root, rinfo);

		if (rinfo->outer_is_left)
			oeclass = rinfo->left_ec;
		else
			oeclass = rinfo->right_ec;

		/* 去重 */
		for (j = 0; j < necs; j++)
		{
			if (ecs[j] == oeclass)
				break;
		}
		if (j < necs)
			continue;

		/* 计算分数：未与 joinrel 连接的成员数量 */
		score = 0;
		foreach(lc2, oeclass->ec_members)
		{
			EquivalenceMember *em = (EquivalenceMember *) lfirst(lc2);

			/* 未来可能的连接伙伴？ */
			if (!em->em_is_const && !em->em_is_child &&
				!bms_overlap(em->em_relids, joinrel->relids))
				score++;
		}

		ecs[necs] = oeclass;
		scores[necs] = score;
		necs++;
	}

	/*
	 * 检查 query_pathkeys 是否全部包含在 ECs 中；如果是，则可以生成对最终输出有用的排序顺序。
	 * 部分匹配没有意义，必须全部包含。
	 */
	if (root->query_pathkeys)
	{
		foreach(lc, root->query_pathkeys)
		{
			PathKey    *query_pathkey = (PathKey *) lfirst(lc);
			EquivalenceClass *query_ec = query_pathkey->pk_eclass;

			for (j = 0; j < necs; j++)
			{
				if (ecs[j] == query_ec)
					break;		/* 找到匹配 */
			}
			if (j >= necs)
				break;			/* 未找到匹配，退出 */
		}
		/* 如果遍历完了，说明全部匹配 */
		if (lc == NULL)
		{
			/* 复制 query_pathkeys 作为输出的起点 */
			pathkeys = list_copy(root->query_pathkeys);
			/* 标记这些 EC 已经输出 */
			foreach(lc, root->query_pathkeys)
			{
				PathKey    *query_pathkey = (PathKey *) lfirst(lc);
				EquivalenceClass *query_ec = query_pathkey->pk_eclass;

				for (j = 0; j < necs; j++)
				{
					if (ecs[j] == query_ec)
					{
						scores[j] = -1;
						break;
					}
				}
			}
		}
	}

	/*
	 * 按受欢迎度顺序将剩余 EC 加入列表，使用默认排序方式。
	 * （可以用 qsort，但通常列表很短，不值得。）
	 */
	for (;;)
	{
		int			best_j;
		int			best_score;
		EquivalenceClass *ec;
		PathKey    *pathkey;

		best_j = 0;
		best_score = scores[0];
		for (j = 1; j < necs; j++)
		{
			if (scores[j] > best_score)
			{
				best_j = j;
				best_score = scores[j];
			}
		}
		if (best_score < 0)
			break;				/* 全部处理完毕 */
		ec = ecs[best_j];
		scores[best_j] = -1;
		pathkey = make_canonical_pathkey(root,
										 ec,
										 linitial_oid(ec->ec_opfamilies),
										 BTLessStrategyNumber,
										 false);
		/* 不可能重复，因为没有重复的 EC */
		Assert(!pathkey_is_redundant(pathkey, pathkeys));
		pathkeys = lappend(pathkeys, pathkey);
	}

	pfree(ecs);
	pfree(scores);

	return pathkeys;
}

/*
 * make_inner_pathkeys_for_merge
 *	  Builds a pathkey list representing the explicit sort order that
 *	  must be applied to an inner path to make it usable with the
 *	  given mergeclauses.
 *
 * 'mergeclauses' is a list of RestrictInfos for the mergejoin clauses
 *			that will be used in a merge join, in order.
 * 'outer_pathkeys' are the already-known canonical pathkeys for the outer
 *			side of the join.
 *
 * The restrictinfos must be marked (via outer_is_left) to show which side
 * of each clause is associated with the current outer path.  (See
 * select_mergejoin_clauses())
 *
 * Returns a pathkeys list that can be applied to the inner relation.
 *
 * Note that it is not this routine's job to decide whether sorting is
 * actually needed for a particular input path.  Assume a sort is necessary;
 * just make the keys, eh?
 */
List *
make_inner_pathkeys_for_merge(PlannerInfo *root,
							  List *mergeclauses,
							  List *outer_pathkeys)
{
	List	   *pathkeys = NIL;
	EquivalenceClass *lastoeclass;
	PathKey    *opathkey;
	ListCell   *lc;
	ListCell   *lop;

	lastoeclass = NULL;
	opathkey = NULL;
	lop = list_head(outer_pathkeys);

	foreach(lc, mergeclauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		EquivalenceClass *oeclass;
		EquivalenceClass *ieclass;
		PathKey    *pathkey;

		update_mergeclause_eclasses(root, rinfo);

		if (rinfo->outer_is_left)
		{
			oeclass = rinfo->left_ec;
			ieclass = rinfo->right_ec;
		}
		else
		{
			oeclass = rinfo->right_ec;
			ieclass = rinfo->left_ec;
		}

		/* outer eclass should match current or next pathkeys */
		/* we check this carefully for debugging reasons */
		if (oeclass != lastoeclass)
		{
			if (!lop)
				elog(ERROR, "too few pathkeys for mergeclauses");
			opathkey = (PathKey *) lfirst(lop);
			lop = lnext(lop);
			lastoeclass = opathkey->pk_eclass;
			if (oeclass != lastoeclass)
				elog(ERROR, "outer pathkeys do not match mergeclause");
		}

		/*
		 * Often, we'll have same EC on both sides, in which case the outer
		 * pathkey is also canonical for the inner side, and we can skip a
		 * useless search.
		 */
		if (ieclass == oeclass)
			pathkey = opathkey;
		else
			pathkey = make_canonical_pathkey(root,
											 ieclass,
											 opathkey->pk_opfamily,
											 opathkey->pk_strategy,
											 opathkey->pk_nulls_first);

		/*
		 * Don't generate redundant pathkeys (which can happen if multiple
		 * mergeclauses refer to the same EC).  Because we do this, the output
		 * pathkey list isn't necessarily ordered like the mergeclauses, which
		 * complicates life for create_mergejoin_plan().  But if we didn't,
		 * we'd have a noncanonical sort key list, which would be bad; for one
		 * reason, it certainly wouldn't match any available sort order for
		 * the input relation.
		 */
		if (!pathkey_is_redundant(pathkey, pathkeys))
			pathkeys = lappend(pathkeys, pathkey);
	}

	return pathkeys;
}

/*
 * trim_mergeclauses_for_inner_pathkeys
 *	  This routine trims a list of mergeclauses to include just those that
 *	  work with a specified ordering for the join's inner relation.
 *
 * 'mergeclauses' is a list of RestrictInfos for mergejoin clauses for the
 *			join relation being formed, in an order known to work for the
 *			currently-considered sort ordering of the join's outer rel.
 * 'pathkeys' is a pathkeys list showing the ordering of an inner-rel path;
 *			it should be equal to, or a truncation of, the result of
 *			make_inner_pathkeys_for_merge for these mergeclauses.
 *
 * What we return will be a prefix of the given mergeclauses list.
 *
 * We need this logic because make_inner_pathkeys_for_merge's result isn't
 * necessarily in the same order as the mergeclauses.  That means that if we
 * consider an inner-rel pathkey list that is a truncation of that result,
 * we might need to drop mergeclauses even though they match a surviving inner
 * pathkey.  This happens when they are to the right of a mergeclause that
 * matches a removed inner pathkey.
 *
 * The mergeclauses must be marked (via outer_is_left) to show which side
 * of each clause is associated with the current outer path.  (See
 * select_mergejoin_clauses())
 */
List *
trim_mergeclauses_for_inner_pathkeys(PlannerInfo *root,
									 List *mergeclauses,
									 List *pathkeys)
{
	List	   *new_mergeclauses = NIL;
	PathKey    *pathkey;
	EquivalenceClass *pathkey_ec;
	bool		matched_pathkey;
	ListCell   *lip;
	ListCell   *i;

	/* No pathkeys => no mergeclauses (though we don't expect this case) */
	if (pathkeys == NIL)
		return NIL;
	/* Initialize to consider first pathkey */
	lip = list_head(pathkeys);
	pathkey = (PathKey *) lfirst(lip);
	pathkey_ec = pathkey->pk_eclass;
	lip = lnext(lip);
	matched_pathkey = false;

	/* Scan mergeclauses to see how many we can use */
	foreach(i, mergeclauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(i);
		EquivalenceClass *clause_ec;

		/* Assume we needn't do update_mergeclause_eclasses again here */

		/* Check clause's inner-rel EC against current pathkey */
		clause_ec = rinfo->outer_is_left ?
			rinfo->right_ec : rinfo->left_ec;

		/* If we don't have a match, attempt to advance to next pathkey */
		if (clause_ec != pathkey_ec)
		{
			/* If we had no clauses matching this inner pathkey, must stop */
			if (!matched_pathkey)
				break;

			/* Advance to next inner pathkey, if any */
			if (lip == NULL)
				break;
			pathkey = (PathKey *) lfirst(lip);
			pathkey_ec = pathkey->pk_eclass;
			lip = lnext(lip);
			matched_pathkey = false;
		}

		/* If mergeclause matches current inner pathkey, we can use it */
		if (clause_ec == pathkey_ec)
		{
			new_mergeclauses = lappend(new_mergeclauses, rinfo);
			matched_pathkey = true;
		}
		else
		{
			/* Else, no hope of adding any more mergeclauses */
			break;
		}
	}

	return new_mergeclauses;
}


/****************************************************************************
 *		PATHKEY USEFULNESS CHECKS
 *
 * We only want to remember as many of the pathkeys of a path as have some
 * potential use, either for subsequent mergejoins or for meeting the query's
 * requested output ordering.  This ensures that add_path() won't consider
 * a path to have a usefully different ordering unless it really is useful.
 * These routines check for usefulness of given pathkeys.
 ****************************************************************************/

/*
 * pathkeys_useful_for_merging
 *		Count the number of pathkeys that may be useful for mergejoins
 *		above the given relation.
 *
 * We consider a pathkey potentially useful if it corresponds to the merge
 * ordering of either side of any joinclause for the rel.  This might be
 * overoptimistic, since joinclauses that require different other relations
 * might never be usable at the same time, but trying to be exact is likely
 * to be more trouble than it's worth.
 *
 * To avoid doubling the number of mergejoin paths considered, we would like
 * to consider only one of the two scan directions (ASC or DESC) as useful
 * for merging for any given target column.  The choice is arbitrary unless
 * one of the directions happens to match an ORDER BY key, in which case
 * that direction should be preferred, in hopes of avoiding a final sort step.
 * right_merge_direction() implements this heuristic.
 */
static int
pathkeys_useful_for_merging(PlannerInfo *root, RelOptInfo *rel, List *pathkeys)
{
	int			useful = 0;
	ListCell   *i;

	foreach(i, pathkeys)
	{
		PathKey    *pathkey = (PathKey *) lfirst(i);
		bool		matched = false;
		ListCell   *j;

		/* If "wrong" direction, not useful for merging */
		if (!right_merge_direction(root, pathkey))
			break;

		/*
		 * First look into the EquivalenceClass of the pathkey, to see if
		 * there are any members not yet joined to the rel.  If so, it's
		 * surely possible to generate a mergejoin clause using them.
		 */
		if (rel->has_eclass_joins &&
			eclass_useful_for_merging(root, pathkey->pk_eclass, rel))
			matched = true;
		else
		{
			/*
			 * Otherwise search the rel's joininfo list, which contains
			 * non-EquivalenceClass-derivable join clauses that might
			 * nonetheless be mergejoinable.
			 */
			foreach(j, rel->joininfo)
			{
				RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(j);

				if (restrictinfo->mergeopfamilies == NIL)
					continue;
				update_mergeclause_eclasses(root, restrictinfo);

				if (pathkey->pk_eclass == restrictinfo->left_ec ||
					pathkey->pk_eclass == restrictinfo->right_ec)
				{
					matched = true;
					break;
				}
			}
		}

		/*
		 * If we didn't find a mergeclause, we're done --- any additional
		 * sort-key positions in the pathkeys are useless.  (But we can still
		 * mergejoin if we found at least one mergeclause.)
		 */
		if (matched)
			useful++;
		else
			break;
	}

	return useful;
}

/*
 * right_merge_direction
 *		Check whether the pathkey embodies the preferred sort direction
 *		for merging its target column.
 */
static bool
right_merge_direction(PlannerInfo *root, PathKey *pathkey)
{
	ListCell   *l;

	foreach(l, root->query_pathkeys)
	{
		PathKey    *query_pathkey = (PathKey *) lfirst(l);

		if (pathkey->pk_eclass == query_pathkey->pk_eclass &&
			pathkey->pk_opfamily == query_pathkey->pk_opfamily)
		{
			/*
			 * Found a matching query sort column.  Prefer this pathkey's
			 * direction iff it matches.  Note that we ignore pk_nulls_first,
			 * which means that a sort might be needed anyway ... but we still
			 * want to prefer only one of the two possible directions, and we
			 * might as well use this one.
			 */
			return (pathkey->pk_strategy == query_pathkey->pk_strategy);
		}
	}

	/* If no matching ORDER BY request, prefer the ASC direction */
	return (pathkey->pk_strategy == BTLessStrategyNumber);
}

/*
 * pathkeys_useful_for_ordering
 *		Count the number of pathkeys that are useful for meeting the
 *		query's requested output ordering.
 *
 * Unlike merge pathkeys, this is an all-or-nothing affair: it does us
 * no good to order by just the first key(s) of the requested ordering.
 * So the result is always either 0 or list_length(root->query_pathkeys).
 */
static int
pathkeys_useful_for_ordering(PlannerInfo *root, List *pathkeys)
{
	if (root->query_pathkeys == NIL)
		return 0;				/* no special ordering requested */

	if (pathkeys == NIL)
		return 0;				/* unordered path */

	if (pathkeys_contained_in(root->query_pathkeys, pathkeys))
	{
		/* It's useful ... or at least the first N keys are */
		return list_length(root->query_pathkeys);
	}

	return 0;					/* path ordering not useful */
}

/*
 * truncate_useless_pathkeys
 *		将给定的 pathkey 列表截断为仅包含“有用”的 pathkeys。
 *
 * 作用：
 * - 只保留对后续 merge join 或满足查询排序要求有用的 pathkeys，去除无用部分。
 *
 * 参数说明：
 * - root: 查询优化器上下文
 * - rel: 当前关系
 * - pathkeys: 原始 pathkey 列表
 *
 * 返回值：
 * - List*: 截断后的 pathkey 列表（只包含有用部分），如果没有有用的则返回 NIL。
 *
 * 实现思路：
 * 1. 计算对 merge join 有用的 pathkey 数量（nuseful）。
 * 2. 计算对输出排序有用的 pathkey 数量（nuseful2）。
 * 3. 取两者最大值作为最终有用的 pathkey 数量。
 * 4. 如果没有有用的 pathkey，返回 NIL。
 *    如果全部都有效，直接返回原列表。
 *    否则，复制并截断列表，只保留有用部分。
 */
List *
truncate_useless_pathkeys(PlannerInfo *root,
						  RelOptInfo *rel,
						  List *pathkeys)
{
	int			nuseful;
	int			nuseful2;

	/* 计算对 merge join 有用的 pathkey 数量 */
	nuseful = pathkeys_useful_for_merging(root, rel, pathkeys);

	/* 计算对输出排序有用的 pathkey 数量 */
	nuseful2 = pathkeys_useful_for_ordering(root, pathkeys);

	/* 取最大值，确保不会漏掉任何有用的 pathkey */
	if (nuseful2 > nuseful)
		nuseful = nuseful2;

	/*
	 * 注意：不能直接修改输入列表，但如果不需要截断则可直接返回原列表。
	 */
	if (nuseful == 0)
		return NIL;
	else if (nuseful == list_length(pathkeys))
		return pathkeys;
	else
		return list_truncate(list_copy(pathkeys), nuseful);
}

/*
 * has_useful_pathkeys
 *		判断指定的 rel 是否可能拥有对 truncate_useless_pathkeys() 有用的 pathkeys。
 *
 * 这是一个廉价的测试，用于在非常简单的查询中跳过 pathkeys 的构建。
 * 如果返回 true 但实际上没有可用的 pathkeys 也没关系，但如果漏掉了有用的 pathkeys 就不好了——
 * 所以要和上面的相关逻辑保持一致！
 *
 * 我们可以让测试更复杂，比如检查 joinclauses 是否真的可用于 mergejoin，
 * 但这样做带来的收益通常不大。没有 join 也没有 sort 的查询还是比较常见的，
 * 所以做这么多判断还是值得的。
 */
bool
has_useful_pathkeys(PlannerInfo *root, RelOptInfo *rel)
{
	/*
	 * 检查当前关系是否参与了连接（Join）。
	 * 如果表要参与连接，那么它的排序属性可能非常有价值，因为 Merge Join（归并连接）
	 * 要求输入数据是有序的。如果扫描路径天然有序，就可以直接做 Merge Join，
	 * 省去昂贵的 Sort 操作。
	 *
	 * rel->joininfo: 存储了涉及该表的连接条件。
	 * rel->has_eclass_joins: 标记该表是否参与了基于等价类（EquivalenceClass）的连接。
	 */
	if (rel->joininfo != NIL || rel->has_eclass_joins)
		return true;			/* 可能可以用于 mergejoin 的 pathkeys */

	/*
	 * 检查整个查询是否有全局的排序要求（对应 SQL 中的 ORDER BY、GROUP BY 或 DISTINCT）。
	 * 如果用户要求 ORDER BY a，而我们生成的路径正好按 a 排序，那就可以直接把这个路径
	 * 作为最终结果，省去最后的 Sort 节点。
	 */
	if (root->query_pathkeys != NIL)
		return true;			/* 可能可以用于排序输出的 pathkeys */

	/*
	 * 既不参与连接，用户也没要求排序。
	 * 例如：SELECT * FROM t WHERE a > 10; （没有 ORDER BY，单表查询）。
	 * 在这种情况下，索引扫描出来的顺序无关紧要，我们只关心能不能快速把数据找出来。
	 */
	return false;				/* 肯定没有用处 */
}
