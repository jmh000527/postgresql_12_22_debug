/*-------------------------------------------------------------------------
 *
 * indxpath.c
 *	  Routines to determine which indexes are usable for scanning a
 *	  given relation, and create Paths accordingly.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/indxpath.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/stratnum.h"
#include "access/sysattr.h"
#include "catalog/pg_am.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"


/* source-code-compatibility hacks for pull_varnos() API change */
#define pull_varnos(a,b) pull_varnos_new(a,b)
#undef make_simple_restrictinfo
#define make_simple_restrictinfo(root, clause)  \
	make_restrictinfo_new(root, clause, true, false, false, 0, NULL, NULL, NULL)

/* XXX 参见 PartCollMatchesExprColl
 * 判断索引的排序规则（idxcollation）与表达式的排序规则（exprcollation）是否匹配
 * 如果索引未指定排序规则（InvalidOid），则认为总是匹配；
 * 否则要求两者完全相等
 */
#define IndexCollMatchesExprColl(idxcollation, exprcollation) \
	((idxcollation) == InvalidOid || (idxcollation) == (exprcollation))


/* 我们是在寻找普通索引扫描、位图扫描，还是两者皆可 */
typedef enum
{
	ST_INDEXSCAN,				/* 必须支持 amgettuple */
	ST_BITMAPSCAN,				/* 必须支持 amgetbitmap */
	ST_ANYSCAN					/* 两者皆可 */
} ScanTypeControl;

/* 用于收集与索引匹配的条件子句的数据结构 */
typedef struct
{
	bool		nonempty;		/* 如果存在任何非空列表则为 true */

	/* 每个索引列对应一个 IndexClause 节点列表 */
	List	   *indexclauses[INDEX_MAX_KEYS];
} IndexClauseSet;

/* Per-path data used within choose_bitmap_and() */
typedef struct
{
	Path	   *path;			/* IndexPath, BitmapAndPath, or BitmapOrPath */
	List	   *quals;			/* the WHERE clauses it uses */
	List	   *preds;			/* predicates of its partial index(es) */
	Bitmapset  *clauseids;		/* quals+preds represented as a bitmapset */
	bool		unclassifiable; /* has too many quals+preds to process? */
} PathClauseUsage;

/* Callback argument for ec_member_matches_indexcol */
typedef struct
{
	IndexOptInfo *index;		/* index we're considering */
	int			indexcol;		/* index column we want to match to */
} ec_member_matches_arg;


static void consider_index_join_clauses(PlannerInfo *root, RelOptInfo *rel,
										IndexOptInfo *index,
										IndexClauseSet *rclauseset,
										IndexClauseSet *jclauseset,
										IndexClauseSet *eclauseset,
										List **bitindexpaths);
static void consider_index_join_outer_rels(PlannerInfo *root, RelOptInfo *rel,
										   IndexOptInfo *index,
										   IndexClauseSet *rclauseset,
										   IndexClauseSet *jclauseset,
										   IndexClauseSet *eclauseset,
										   List **bitindexpaths,
										   List *indexjoinclauses,
										   int considered_clauses,
										   List **considered_relids);
static void get_join_index_paths(PlannerInfo *root, RelOptInfo *rel,
								 IndexOptInfo *index,
								 IndexClauseSet *rclauseset,
								 IndexClauseSet *jclauseset,
								 IndexClauseSet *eclauseset,
								 List **bitindexpaths,
								 Relids relids,
								 List **considered_relids);
static bool eclass_already_used(EquivalenceClass *parent_ec, Relids oldrelids,
								List *indexjoinclauses);
static bool bms_equal_any(Relids relids, List *relids_list);
static void get_index_paths(PlannerInfo *root, RelOptInfo *rel,
							IndexOptInfo *index, IndexClauseSet *clauses,
							List **bitindexpaths);
static List *build_index_paths(PlannerInfo *root, RelOptInfo *rel,
							   IndexOptInfo *index, IndexClauseSet *clauses,
							   bool useful_predicate,
							   ScanTypeControl scantype,
							   bool *skip_nonnative_saop,
							   bool *skip_lower_saop);
static List *build_paths_for_OR(PlannerInfo *root, RelOptInfo *rel,
								List *clauses, List *other_clauses);
static List *generate_bitmap_or_paths(PlannerInfo *root, RelOptInfo *rel,
									  List *clauses, List *other_clauses);
static Path *choose_bitmap_and(PlannerInfo *root, RelOptInfo *rel,
							   List *paths);
static int	path_usage_comparator(const void *a, const void *b);
static Cost bitmap_scan_cost_est(PlannerInfo *root, RelOptInfo *rel,
								 Path *ipath);
static Cost bitmap_and_cost_est(PlannerInfo *root, RelOptInfo *rel,
								List *paths);
static PathClauseUsage *classify_index_clause_usage(Path *path,
													List **clauselist);
static void find_indexpath_quals(Path *bitmapqual, List **quals, List **preds);
static int	find_list_position(Node *node, List **nodelist);
static bool check_index_only(RelOptInfo *rel, IndexOptInfo *index);
static double get_loop_count(PlannerInfo *root, Index cur_relid, Relids outer_relids);
static double adjust_rowcount_for_semijoins(PlannerInfo *root,
											Index cur_relid,
											Index outer_relid,
											double rowcount);
static double approximate_joinrel_size(PlannerInfo *root, Relids relids);
static void match_restriction_clauses_to_index(PlannerInfo *root,
											   IndexOptInfo *index,
											   IndexClauseSet *clauseset);
static void match_join_clauses_to_index(PlannerInfo *root,
										RelOptInfo *rel, IndexOptInfo *index,
										IndexClauseSet *clauseset,
										List **joinorclauses);
static void match_eclass_clauses_to_index(PlannerInfo *root,
										  IndexOptInfo *index,
										  IndexClauseSet *clauseset);
static void match_clauses_to_index(PlannerInfo *root,
								   List *clauses,
								   IndexOptInfo *index,
								   IndexClauseSet *clauseset);
static void match_clause_to_index(PlannerInfo *root,
								  RestrictInfo *rinfo,
								  IndexOptInfo *index,
								  IndexClauseSet *clauseset);
static IndexClause *match_clause_to_indexcol(PlannerInfo *root,
											 RestrictInfo *rinfo,
											 int indexcol,
											 IndexOptInfo *index);
static IndexClause *match_boolean_index_clause(PlannerInfo *root,
											   RestrictInfo *rinfo,
											   int indexcol, IndexOptInfo *index);
static IndexClause *match_opclause_to_indexcol(PlannerInfo *root,
											   RestrictInfo *rinfo,
											   int indexcol,
											   IndexOptInfo *index);
static IndexClause *match_funcclause_to_indexcol(PlannerInfo *root,
												 RestrictInfo *rinfo,
												 int indexcol,
												 IndexOptInfo *index);
static IndexClause *get_index_clause_from_support(PlannerInfo *root,
												  RestrictInfo *rinfo,
												  Oid funcid,
												  int indexarg,
												  int indexcol,
												  IndexOptInfo *index);
static IndexClause *match_saopclause_to_indexcol(PlannerInfo *root,
												 RestrictInfo *rinfo,
												 int indexcol,
												 IndexOptInfo *index);
static IndexClause *match_rowcompare_to_indexcol(PlannerInfo *root,
												 RestrictInfo *rinfo,
												 int indexcol,
												 IndexOptInfo *index);
static IndexClause *expand_indexqual_rowcompare(PlannerInfo *root,
												RestrictInfo *rinfo,
												int indexcol,
												IndexOptInfo *index,
												Oid expr_op,
												bool var_on_left);
static void match_pathkeys_to_index(IndexOptInfo *index, List *pathkeys,
									List **orderby_clauses_p,
									List **clause_columns_p);
static Expr *match_clause_to_ordering_op(IndexOptInfo *index,
										 int indexcol, Expr *clause, Oid pk_opfamily);
static bool ec_member_matches_indexcol(PlannerInfo *root, RelOptInfo *rel,
									   EquivalenceClass *ec, EquivalenceMember *em,
									   void *arg);


/*
 * create_index_paths()
 *	  为给定关系生成所有有意义的索引路径。
 *	  候选路径会通过 add_path 添加到 rel 的 pathlist 中。
 *
 * 要考虑索引扫描，索引必须匹配查询条件中的一个或多个限制条件或连接条件，
 * 或者匹配查询的 ORDER BY 条件，或者其谓词能被查询条件满足。
 *
 * 索引扫描有两种基本类型。"普通"索引扫描只使用限制条件（可以没有），
 * 所以可以在任何上下文中应用。"参数化"索引扫描会使用连接条件（以及可用的限制条件），
 * 这种扫描只能作为嵌套循环连接的内表，不能作为外表，也不能用于合并或哈希连接。
 * 在这种情况下，其他关系的属性值在每次索引路径扫描时都是可用且固定的。
 *
 * 对于每个普通或参数化索引扫描，本函数都会生成一个 IndexPath 并提交给 add_path()。
 *
 * 'rel' 是我们要为其生成索引路径的关系
 *
 * 注意：必须先对该关系运行过 check_index_predicates()。
 *
 * 注意：如果关系的 tlist 中涉及 LATERAL 引用，rel->lateral_relids 可能非空。
 * 当前我们会把 lateral_relids 加入每个路径的参数化集合，但除此之外不做特殊处理。
 * 任何这些关系必须作为参数源，这可能应该影响我们对索引条件的选择，但目前没有处理。
 * 下面关于“非参数化”路径的注释应理解为“就索引条件而言是非参数化”。
 */
void
create_index_paths(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *indexpaths; 	/* 普通索引路径列表 */
	List	   *bitindexpaths;	/* 位图索引路径列表 */
	List	   *bitjoinpaths;	/* 位图连接路径列表 */
	List	   *joinorclauses;	/* 连接OR条件列表 */
	IndexClauseSet rclauseset;  /* 限制条件集合 */
	IndexClauseSet jclauseset;  /* 连接条件集合 */
	IndexClauseSet eclauseset;  /* 等价类条件集合 */
	ListCell   *lc;

	/* 如果关系没有索引，直接返回 */
	if (rel->indexlist == NIL)
		return;

	/* 初始化位图路径和连接OR条件列表 */
	bitindexpaths = bitjoinpaths = joinorclauses = NIL;

	/* 遍历关系的每个索引，为每个索引生成可能的路径 */
	foreach(lc, rel->indexlist)
	{
		/* 从链表中获取当前索引 */
		IndexOptInfo* index = (IndexOptInfo*)lfirst(lc);

		/* 确保索引键数量不超过系统定义的最大限制 */
		Assert(index->nkeycolumns <= INDEX_MAX_KEYS);

		/*
		 * 跳过不满足查询条件的部分索引，确保查询优化器不会错误地使用”部分索引“
		 * 部分索引只在其谓词条件被查询条件满足时才可用
		 * （见 check_index_predicates()  ）
		 *
		 *	如果index->indpred不为NIL，表示这是一个部分索引
		 *	如果 predOK 为 true，表示查询的 WHERE 条件蕴含（implies）了索引的 WHERE 条件。
		 *	也就是说，查询所需的数据完全落在索引覆盖的范围内。
		 *  因此，只有在 predOK 为 true 时，部分索引才可用。
		 */
		if (index->indpred != NIL && !index->predOK)
			continue;

		/*
		 * 阶段1：匹配和处理限制条件
		 * 清空限制条件集合并查找能与当前索引匹配的限制条件
		 *
		 * 在 rclauseset 结构体中：
		 * 键（Key） 是数组 indexclauses 的下标索引，它是一个整数，代表索引定义中的列序号（从 0 开始计数）。
		 * 例如，0 代表索引的第一列，1 代表索引的第二列，以此类推。
		 * 值（Value） 是该下标位置存储的一个链表（List）。
		 * 这个链表里包含了一个或多个 IndexClause 对象。每一个 IndexClause 对象都代表一个具体的 SQL 查询条件。
		 */
		MemSet(&rclauseset, 0, sizeof(rclauseset)); /* 重置条件集合 */
		/* 查找能与当前索引匹配的限制条件，将它们添加到rclauseset中 */
		match_restriction_clauses_to_index(root, index, &rclauseset);

		/*
		 * 使用匹配的限制条件生成索引路径
		 * - 普通索引路径直接添加到rel->pathlist
		 * - 位图索引路径暂时收集到bitindexpaths，后续统一处理
		 */
		get_index_paths(root, rel, index, &rclauseset,
						&bitindexpaths);

		/*
		 * 阶段2：匹配和处理连接条件
		 * 查找未合并到等价类中的松散连接条件
		 * 同时收集连接OR条件供后续处理
		 */
		MemSet(&jclauseset, 0, sizeof(jclauseset)); /* 重置连接条件集合 */
		/* 查找未合并到等价类中的松散连接条件，将它们添加到jclauseset中 */
		match_join_clauses_to_index(root, rel, index,
							&jclauseset, &joinorclauses);

		/*
		 * 阶段3：匹配和处理等价类条件
		 * 从等价类中提取能与索引匹配的条件
		 */
		MemSet(&eclauseset, 0, sizeof(eclauseset)); /* 重置等价类条件集合 */
		/* 从等价类中提取能与索引匹配的条件，将它们添加到eclauseset中 */
		match_eclass_clauses_to_index(root, index,
							&eclauseset);

		/*
		 * 如果找到连接条件或等价类条件，则生成参数化索引路径
		 * 参数化路径主要用于嵌套循环连接的内表
		 */
		if (jclauseset.nonempty || eclauseset.nonempty)
			consider_index_join_clauses(root, rel, index,
								&rclauseset,
								&jclauseset,
								&eclauseset,
								&bitjoinpaths);
	}

	/*
	 * 处理OR条件：为限制条件中的OR条件生成BitmapOrPath
	 * 这允许多个条件通过位图操作高效组合
	 */
	indexpaths = generate_bitmap_or_paths(root, rel,
							rel->baserestrictinfo, NIL);
	bitindexpaths = list_concat(bitindexpaths, indexpaths);

	/*
	 * 同理，为连接OR条件生成BitmapOrPath
	 */
	indexpaths = generate_bitmap_or_paths(root, rel,
							joinorclauses, rel->baserestrictinfo);
	bitjoinpaths = list_concat(bitjoinpaths, indexpaths);

	/*
	 * 处理位图索引路径：为所有位图索引路径生成一个最优的BitmapHeapPath
	 * 即使有多个索引，也只生成一个路径，因为最终会根据总成本选择最优组合
	 */
	if (bitindexpaths != NIL)
	{
		Path	   *bitmapqual; /* 位图条件的最优组合 */
		BitmapHeapPath *bpath;  /* 生成的位图堆路径 */

		/* 选择位图索引路径的最优AND组合 */
		bitmapqual = choose_bitmap_and(root, rel, bitindexpaths);
		/* 创建位图堆路径，使用关系的lateral_relids作为参数化要求 */
		bpath = create_bitmap_heap_path(root, rel, bitmapqual,
								rel->lateral_relids, 1.0, 0);
		/* 将生成的路径添加到关系的路径列表中 */
		add_path(rel, (Path *) bpath);

		/*
		 * 如果支持并行查询且无lateral引用，创建并行位图路径
		 * 并行查询可以利用多个CPU核心加速数据扫描
		 */
		if (rel->consider_parallel && rel->lateral_relids == NULL)
			create_partial_bitmap_paths(root, rel, bitmapqual);
	}

	/*
	 * 处理位图连接路径：为每种不同的参数化方式生成对应的BitmapHeapPath
	 * 这允许在不同的连接上下文中使用位图索引
	 */
	if (bitjoinpaths != NIL)
	{
		List	   *all_path_outers; /* 所有不同的参数化集合 */
		ListCell   *lc;

		/* 步骤1：收集所有不同的参数化集合 */
		all_path_outers = NIL;
		foreach(lc, bitjoinpaths)
		{
			Path	   *path = (Path *) lfirst(lc);
			Relids		required_outer = PATH_REQ_OUTER(path);

			/* 仅添加新的参数化集合（避免重复） */
			if (!bms_equal_any(required_outer, all_path_outers))
				all_path_outers = lappend(all_path_outers, required_outer);
		}

		/* 步骤2：为每种参数化集合生成对应的位图堆路径 */
		foreach(lc, all_path_outers)
		{
			Relids		max_outers = (Relids) lfirst(lc);
			List	   *this_path_set; /* 特定参数化集合的路径集合 */
			Path	   *bitmapqual; /* 位图条件的最优组合 */
			Relids		required_outer; /* 最终路径需要的外部关系 */
			double		loop_count; /* 循环迭代次数估计 */
			BitmapHeapPath *bpath; /* 生成的位图堆路径 */
			ListCell   *lcp;

			/* 收集所有与当前参数化集合兼容的位图连接路径 */
			this_path_set = NIL;
			foreach(lcp, bitjoinpaths)
			{
				Path	   *path = (Path *) lfirst(lcp);

				/* 只选择参数化需求是当前集合子集的路径 */
				if (bms_is_subset(PATH_REQ_OUTER(path), max_outers))
					this_path_set = lappend(this_path_set, path);
			}

			/*
			 * 添加限制条件的位图路径，因为它们不依赖特定连接上下文
			 * 可以与任何连接条件组合使用
			 */
			this_path_set = list_concat(this_path_set, bitindexpaths);

			/* 选择该参数化集合下的最优位图组合 */
			bitmapqual = choose_bitmap_and(root, rel, this_path_set);

			/* 获取最终路径所需的外部关系集合 */
			required_outer = PATH_REQ_OUTER(bitmapqual);
			/* 估计嵌套循环的迭代次数 */
			loop_count = get_loop_count(root, rel->relid, required_outer);
			/* 创建位图堆路径，包含参数化信息和循环次数估计 */
			bpath = create_bitmap_heap_path(root, rel, bitmapqual,
									required_outer, loop_count, 0);
			/* 添加生成的路径到关系的路径列表 */
			add_path(rel, (Path *) bpath);
		}
	}
}


/*
 * consider_index_join_clauses
 *	  Given sets of join clauses for an index, decide which parameterized
 *	  index paths to build.
 *
 * Plain indexpaths are sent directly to add_path, while potential
 * bitmap indexpaths are added to *bitindexpaths for later processing.
 *
 * 'rel' is the index's heap relation
 * 'index' is the index for which we want to generate paths
 * 'rclauseset' is the collection of indexable restriction clauses
 * 'jclauseset' is the collection of indexable simple join clauses
 * 'eclauseset' is the collection of indexable clauses from EquivalenceClasses
 * '*bitindexpaths' is the list to add bitmap paths to
 */
static void
consider_index_join_clauses(PlannerInfo *root, RelOptInfo *rel,
							IndexOptInfo *index,
							IndexClauseSet *rclauseset,
							IndexClauseSet *jclauseset,
							IndexClauseSet *eclauseset,
							List **bitindexpaths)
{
	int			considered_clauses = 0;
	List	   *considered_relids = NIL;
	int			indexcol;

	/*
	 * The strategy here is to identify every potentially useful set of outer
	 * rels that can provide indexable join clauses.  For each such set,
	 * select all the join clauses available from those outer rels, add on all
	 * the indexable restriction clauses, and generate plain and/or bitmap
	 * index paths for that set of clauses.  This is based on the assumption
	 * that it's always better to apply a clause as an indexqual than as a
	 * filter (qpqual); which is where an available clause would end up being
	 * applied if we omit it from the indexquals.
	 *
	 * This looks expensive, but in most practical cases there won't be very
	 * many distinct sets of outer rels to consider.  As a safety valve when
	 * that's not true, we use a heuristic: limit the number of outer rel sets
	 * considered to a multiple of the number of clauses considered.  (We'll
	 * always consider using each individual join clause, though.)
	 *
	 * For simplicity in selecting relevant clauses, we represent each set of
	 * outer rels as a maximum set of clause_relids --- that is, the indexed
	 * relation itself is also included in the relids set.  considered_relids
	 * lists all relids sets we've already tried.
	 */
	for (indexcol = 0; indexcol < index->nkeycolumns; indexcol++)
	{
		/* Consider each applicable simple join clause */
		considered_clauses += list_length(jclauseset->indexclauses[indexcol]);
		consider_index_join_outer_rels(root, rel, index,
									   rclauseset, jclauseset, eclauseset,
									   bitindexpaths,
									   jclauseset->indexclauses[indexcol],
									   considered_clauses,
									   &considered_relids);
		/* Consider each applicable eclass join clause */
		considered_clauses += list_length(eclauseset->indexclauses[indexcol]);
		consider_index_join_outer_rels(root, rel, index,
									   rclauseset, jclauseset, eclauseset,
									   bitindexpaths,
									   eclauseset->indexclauses[indexcol],
									   considered_clauses,
									   &considered_relids);
	}
}

/*
 * consider_index_join_outer_rels
 *	  Generate parameterized paths based on clause relids in the clause list.
 *
 * Workhorse for consider_index_join_clauses; see notes therein for rationale.
 *
 * 'rel', 'index', 'rclauseset', 'jclauseset', 'eclauseset', and
 *		'bitindexpaths' as above
 * 'indexjoinclauses' is a list of IndexClauses for join clauses
 * 'considered_clauses' is the total number of clauses considered (so far)
 * '*considered_relids' is a list of all relids sets already considered
 */
static void
consider_index_join_outer_rels(PlannerInfo *root, RelOptInfo *rel,
							   IndexOptInfo *index,
							   IndexClauseSet *rclauseset,
							   IndexClauseSet *jclauseset,
							   IndexClauseSet *eclauseset,
							   List **bitindexpaths,
							   List *indexjoinclauses,
							   int considered_clauses,
							   List **considered_relids)
{
	ListCell   *lc;

	/* Examine relids of each joinclause in the given list */
	foreach(lc, indexjoinclauses)
	{
		IndexClause *iclause = (IndexClause *) lfirst(lc);
		Relids		clause_relids = iclause->rinfo->clause_relids;
		EquivalenceClass *parent_ec = iclause->rinfo->parent_ec;
		ListCell   *lc2;

		/* If we already tried its relids set, no need to do so again */
		if (bms_equal_any(clause_relids, *considered_relids))
			continue;

		/*
		 * Generate the union of this clause's relids set with each
		 * previously-tried set.  This ensures we try this clause along with
		 * every interesting subset of previous clauses.  However, to avoid
		 * exponential growth of planning time when there are many clauses,
		 * limit the number of relid sets accepted to 10 * considered_clauses.
		 *
		 * Note: get_join_index_paths adds entries to *considered_relids, but
		 * it prepends them to the list, so that we won't visit new entries
		 * during the inner foreach loop.  No real harm would be done if we
		 * did, since the subset check would reject them; but it would waste
		 * some cycles.
		 */
		foreach(lc2, *considered_relids)
		{
			Relids		oldrelids = (Relids) lfirst(lc2);

			/*
			 * If either is a subset of the other, no new set is possible.
			 * This isn't a complete test for redundancy, but it's easy and
			 * cheap.  get_join_index_paths will check more carefully if we
			 * already generated the same relids set.
			 */
			if (bms_subset_compare(clause_relids, oldrelids) != BMS_DIFFERENT)
				continue;

			/*
			 * If this clause was derived from an equivalence class, the
			 * clause list may contain other clauses derived from the same
			 * eclass.  We should not consider that combining this clause with
			 * one of those clauses generates a usefully different
			 * parameterization; so skip if any clause derived from the same
			 * eclass would already have been included when using oldrelids.
			 */
			if (parent_ec &&
				eclass_already_used(parent_ec, oldrelids,
									indexjoinclauses))
				continue;

			/*
			 * If the number of relid sets considered exceeds our heuristic
			 * limit, stop considering combinations of clauses.  We'll still
			 * consider the current clause alone, though (below this loop).
			 */
			if (list_length(*considered_relids) >= 10 * considered_clauses)
				break;

			/* OK, try the union set */
			get_join_index_paths(root, rel, index,
								 rclauseset, jclauseset, eclauseset,
								 bitindexpaths,
								 bms_union(clause_relids, oldrelids),
								 considered_relids);
		}

		/* Also try this set of relids by itself */
		get_join_index_paths(root, rel, index,
							 rclauseset, jclauseset, eclauseset,
							 bitindexpaths,
							 clause_relids,
							 considered_relids);
	}
}

/*
 * get_join_index_paths
 *	  Generate index paths using clauses from the specified outer relations.
 *	  In addition to generating paths, relids is added to *considered_relids
 *	  if not already present.
 *
 * Workhorse for consider_index_join_clauses; see notes therein for rationale.
 *
 * 'rel', 'index', 'rclauseset', 'jclauseset', 'eclauseset',
 *		'bitindexpaths', 'considered_relids' as above
 * 'relids' is the current set of relids to consider (the target rel plus
 *		one or more outer rels)
 */
static void
get_join_index_paths(PlannerInfo *root, RelOptInfo *rel,
					 IndexOptInfo *index,
					 IndexClauseSet *rclauseset,
					 IndexClauseSet *jclauseset,
					 IndexClauseSet *eclauseset,
					 List **bitindexpaths,
					 Relids relids,
					 List **considered_relids)
{
	IndexClauseSet clauseset;
	int			indexcol;

	/* If we already considered this relids set, don't repeat the work */
	if (bms_equal_any(relids, *considered_relids))
		return;

	/* Identify indexclauses usable with this relids set */
	MemSet(&clauseset, 0, sizeof(clauseset));

	for (indexcol = 0; indexcol < index->nkeycolumns; indexcol++)
	{
		ListCell   *lc;

		/* First find applicable simple join clauses */
		foreach(lc, jclauseset->indexclauses[indexcol])
		{
			IndexClause *iclause = (IndexClause *) lfirst(lc);

			if (bms_is_subset(iclause->rinfo->clause_relids, relids))
				clauseset.indexclauses[indexcol] =
					lappend(clauseset.indexclauses[indexcol], iclause);
		}

		/*
		 * Add applicable eclass join clauses.  The clauses generated for each
		 * column are redundant (cf generate_implied_equalities_for_column),
		 * so we need at most one.  This is the only exception to the general
		 * rule of using all available index clauses.
		 */
		foreach(lc, eclauseset->indexclauses[indexcol])
		{
			IndexClause *iclause = (IndexClause *) lfirst(lc);

			if (bms_is_subset(iclause->rinfo->clause_relids, relids))
			{
				clauseset.indexclauses[indexcol] =
					lappend(clauseset.indexclauses[indexcol], iclause);
				break;
			}
		}

		/* Add restriction clauses (this is nondestructive to rclauseset) */
		clauseset.indexclauses[indexcol] =
			list_concat(clauseset.indexclauses[indexcol],
						rclauseset->indexclauses[indexcol]);

		if (clauseset.indexclauses[indexcol] != NIL)
			clauseset.nonempty = true;
	}

	/* We should have found something, else caller passed silly relids */
	Assert(clauseset.nonempty);

	/* Build index path(s) using the collected set of clauses */
	get_index_paths(root, rel, index, &clauseset, bitindexpaths);

	/*
	 * Remember we considered paths for this set of relids.  We use lcons not
	 * lappend to avoid confusing the loop in consider_index_join_outer_rels.
	 */
	*considered_relids = lcons(relids, *considered_relids);
}

/*
 * eclass_already_used
 *		True if any join clause usable with oldrelids was generated from
 *		the specified equivalence class.
 */
static bool
eclass_already_used(EquivalenceClass *parent_ec, Relids oldrelids,
					List *indexjoinclauses)
{
	ListCell   *lc;

	foreach(lc, indexjoinclauses)
	{
		IndexClause *iclause = (IndexClause *) lfirst(lc);
		RestrictInfo *rinfo = iclause->rinfo;

		if (rinfo->parent_ec == parent_ec &&
			bms_is_subset(rinfo->clause_relids, oldrelids))
			return true;
	}
	return false;
}

/*
 * bms_equal_any
 *		True if relids is bms_equal to any member of relids_list
 *
 * Perhaps this should be in bitmapset.c someday.
 */
static bool
bms_equal_any(Relids relids, List *relids_list)
{
	ListCell   *lc;

	foreach(lc, relids_list)
	{
		if (bms_equal(relids, (Relids) lfirst(lc)))
			return true;
	}
	return false;
}

/*
 * get_index_paths
 *	  给定一个索引和一组索引条件，为其构造 IndexPath 路径。
 *
 * 普通索引路径直接通过 add_path 添加到 rel 的 pathlist，
 * 位图索引路径则加入 *bitindexpaths，后续统一处理。
 *
 * 该函数主要用于正确处理 ScalarArrayOpExpr 条件。
 * 如果索引访问方法（AM）原生支持 ScalarArrayOpExpr，则直接包含在普通索引路径中；
 * 否则，构造普通索引路径时要排除这些条件，并单独尝试将其用于位图路径。
 * 此外，还要考虑排除非首列的 ScalarArrayOpExpr，以便生成有序路径。
 */
static void
get_index_paths(PlannerInfo *root, RelOptInfo *rel,
				IndexOptInfo *index, IndexClauseSet *clauses,
				List **bitindexpaths)
{
	List	   *indexpaths;
	bool		skip_nonnative_saop = false;
	bool		skip_lower_saop = false;
	ListCell   *lc;

	/*
	 * 首先用条件构造普通索引路径。
	 * 只有索引 AM 原生支持 ScalarArrayOpExpr，
	 * 且该条件在首列时才允许包含，否则跳过以便生成有序路径。
	 *
	 * clauses：索引条件集合，之前 match_restriction_clauses_to_index 函数的产出。
	 * skip_nonnative_saop：是否跳过非首列的 ScalarArrayOpExpr。
	 * skip_lower_saop：是否跳过非首列的 ScalarArrayOpExpr（索引 AM 支持原生处理）。
	 * predOK：索引谓词是否被查询条件满足。
	 */
	indexpaths = build_index_paths(root, rel,
								   index, clauses,
								   index->predOK,
								   ST_ANYSCAN,
								   &skip_nonnative_saop,
								   &skip_lower_saop);

	/*
	 * 如果跳过了非首列的 ScalarArrayOpExpr（且索引 AM 支持），
	 * 则再尝试一次，把这些条件也包含进来（但会失去排序）。
	 */
	if (skip_lower_saop)
	{
		indexpaths = list_concat(indexpaths,
								 build_index_paths(root, rel,
												   index, clauses,
												   index->predOK,
												   ST_ANYSCAN,
												   &skip_nonnative_saop,
												   NULL));
	}

	/*
	 * 把能用于普通 IndexScan 的路径提交给 add_path。
	 * （普通 IndexPath 既可用于普通索引扫描，也可用于索引仅扫描，
	 * 这里不区分。部分索引只支持位图扫描的不能提交。）
	 *
	 * 同时，挑选出能用于位图扫描的路径。只考虑支持位图扫描的索引，
	 * 且只要路径有选择性（即不是仅用于排序的路径）。
	 */
	foreach(lc, indexpaths)
	{
		IndexPath  *ipath = (IndexPath *) lfirst(lc);

		if (index->amhasgettuple)
			add_path(rel, (Path *) ipath);

		if (index->amhasgetbitmap &&
			(ipath->path.pathkeys == NIL ||
			 ipath->indexselectivity < 1.0))
			*bitindexpaths = lappend(*bitindexpaths, ipath);
	}

	/*
	 * 如果有 ScalarArrayOpExpr 条件但索引不支持原生处理，
	 * 则生成依赖于执行器处理 ScalarArrayOpExpr 的位图扫描路径。
	 */
	if (skip_nonnative_saop)
	{
		indexpaths = build_index_paths(root, rel,
									   index, clauses,
									   false,
									   ST_BITMAPSCAN,
									   NULL,
									   NULL);
		*bitindexpaths = list_concat(*bitindexpaths, indexpaths);
	}
}

/*
 * build_index_paths
 *    根据给定索引和索引条件集合，构建零个或多个IndexPath，同时构建零个或多个部分并行索引路径(partial IndexPaths)
 *
 * 函数返回路径列表的原因：(1) 该函数检查某些不应生成任何IndexPath的情况；(2) 在某些情况下
 * 我们需要同时考虑正向和反向扫描，以获取两种排序顺序。注意：这些路径只是返回给调用者，
 * 不会立即通过add_path()添加。
 *
 * 在顶层调用时，useful_predicate参数应精确对应索引的predOK标志（即如果索引有一个已被限制
 * 条件证明的谓词，则为true）。当处理OR子句的一个分支时，如果该谓词要求证明当前OR列表，则
 * useful_predicate应为true。注意：如果索引具有不可证明的谓词，则根本不应调用此函数。
 *
 * scantype指示我们是否要创建普通索引扫描、位图索引扫描或两者都创建。当它为ST_BITMAPSCAN时，
 * 我们在决定是否生成路径时不会考虑索引顺序。
 *
 * 如果skip_nonnative_saop不为NULL，除非索引AM直接支持，否则我们将忽略ScalarArrayOpExpr条件，
 * 并且如果我们发现任何此类条件，我们将*skip_nonnative_saop设置为true（调用者必须将变量初始化为false）。
 * 如果它为NULL，我们不会忽略ScalarArrayOpExpr条件。
 *
 * 如果skip_lower_saop不为NULL，我们将忽略非第一索引列的ScalarArrayOpExpr条件，如果我们发现
 * 任何此类条件，我们将*skip_lower_saop设置为true（调用者必须将变量初始化为false）。如果它为NULL，
 * 我们不会忽略非第一列的ScalarArrayOpExpr条件，但它们会导致扫描输出被视为无序。
 *
 * 参数：
 *   root - 查询规划器的根结构，包含查询的所有规划信息
 *   rel - 索引对应的堆表关系
 *   index - 我们要为其生成路径的索引
 *   clauses - 可用于索引的条件集合(IndexClause节点)
 *   useful_predicate - 指示索引是否有有用的谓词
 *   scantype - 指示我们需要普通还是位图扫描支持
 *   skip_nonnative_saop - 指示如果索引AM不支持，是否接受SAOP
 *   skip_lower_saop - 指示是否接受非首列的SAOP
 *
 * 返回值：
 *   生成的IndexPath路径列表，可能为空
 */
static List *
build_index_paths(PlannerInfo *root, RelOptInfo *rel,
                  IndexOptInfo *index, IndexClauseSet *clauses,
                  bool useful_predicate,
                  ScanTypeControl scantype,
                  bool *skip_nonnative_saop,
                  bool *skip_lower_saop)
{
    // 局部变量声明
    List       *result = NIL;           // 存储生成的索引路径结果列表
    IndexPath  *ipath;                  // 当前生成的索引路径
    List       *index_clauses;          // 收集可用于索引的条件
    Relids      outer_relids;           // 外部关系ID集合
    double      loop_count;             // 循环计数，用于成本估算
    List       *orderbyclauses;         // ORDER BY子句
    List       *orderbyclausecols;      // ORDER BY子句对应的列
    List       *index_pathkeys;         // 索引的排序键
    List       *useful_pathkeys;        // 对当前查询有用的排序键
    bool        found_lower_saop_clause; // 标记是否发现非首列的SAOP条件
    bool        pathkeys_possibly_useful; // 标记排序键是否可能有用
    bool        index_is_ordered;       // 标记索引是否有序
    bool        index_only_scan;        // 标记是否可以进行仅索引扫描
    int         indexcol;               // 当前处理的索引列

    /*
     * 检查索引是否支持所需的扫描类型
     * 根据scantype参数验证索引的访问方法是否支持对应扫描
     */
    switch (scantype)
    {
        case ST_INDEXSCAN:  // 普通索引扫描
            // 检查索引访问方法是否支持gettuple操作
            if (!index->amhasgettuple)
                return NIL;  // 不支持则返回空列表
            break;
        case ST_BITMAPSCAN: // 位图索引扫描
            // 检查索引访问方法是否支持getbitmap操作
            if (!index->amhasgetbitmap)
                return NIL;  // 不支持则返回空列表
            break;
        case ST_ANYSCAN:    // 任意类型扫描（普通或位图）
            /* 两种类型都可以，无需检查 */
            break;
    }

    /*
	 * 第一步：把分散在各个列上的查询条件，收集并整理成一个有序的大列表，为后续生成索引扫描路径做准备。
     *
     * 结果列表中的条件按索引键排序，使得列号形成非递减序列。
     * （btree和可能的其他地方依赖于此顺序）。如果索引AM允许，
     * 该列表可以为空。
     *
     * found_lower_saop_clause在我们接受非第一索引列的ScalarArrayOpExpr
     * 索引条件时设置为true。这会阻止我们假设扫描结果是有序的。
     * （实际上，如果所有前面的列都有相等约束，结果仍然是有序的，但
     * 让这段代码了解这种改进似乎太昂贵且不符合模块化原则）。
     *
     * 我们还构建一个Relids集，显示所选条件所需的外部关系。
     * lateral_relids包含在其中，但不单独考虑。
     */
	index_clauses = NIL;                // 准备一个空列表，用来装最终结果
    found_lower_saop_clause = false;    // 初始化为未发现非首列SAOP条件
	/*
	 * 复制关系的lateral_relids作为外部关系的初始集合
	 * 如果这些条件里引用了其他表（比如 a = t2.x），
	 * 我们需要记录下来，因为这意味着这个索引扫描是参数化的（必须先算出 t2.x 才能扫 t1）。
	 */
	outer_relids = bms_copy(rel->lateral_relids);
    // 遍历索引的所有键列
    for (indexcol = 0; indexcol < index->nkeycolumns; indexcol++)
    {
        ListCell   *lc;

        // 遍历当前列的所有索引条件
        foreach(lc, clauses->indexclauses[indexcol])
        {
            IndexClause *iclause = (IndexClause *) lfirst(lc);
            RestrictInfo *rinfo = iclause->rinfo;

			/* 处理ScalarArrayOpExpr类型的条件（例如 IN (...) 或 ANY (...)） */
			if (IsA(rinfo->clause, ScalarArrayOpExpr))
			{
				/*
				 * 检查索引是否原生支持数组搜索操作（如 B-Tree 支持，但某些索引可能不支持）。
				 * 如果不支持，我们通常不能在普通索引扫描中使用它。
				 */
				if (!index->amsearcharray)
				{
					if (skip_nonnative_saop)
					{
						/*
						 * 如果调用者要求跳过非原生支持的数组操作（通常是为了构建普通索引路径），
						 * 则标记 skip_nonnative_saop 为 true 并跳过此条件。
						 * 这种条件只能在位图扫描（Bitmap Scan）中使用。
						 */
						*skip_nonnative_saop = true;
						continue;
					}
					/* 如果没跳过，那必须是位图扫描，因为位图扫描可以通过多次查找来模拟数组搜索 */
					Assert(scantype == ST_BITMAPSCAN);
				}

				/*
				 * 处理非首列（indexcol > 0）的数组操作条件。
				 * 在 B-Tree 索引中，如果在非首列使用 IN/ANY，通常会导致扫描结果不再有序。
				 *
				 * 解释：
				 * B-Tree 索引的有序性是基于“字典序”的。如果在前面的列上使用了多个值（IN 或 ANY），
				 * 那么对于后面的列来说，它们在整个结果集中就不再是连续且有序的了。
				 *
				 * 例如：索引 (a, b)，查询 WHERE a IN (1, 2) ORDER BY b
				 * 扫描顺序可能是：
				 * a=1: (1, 10), (1, 20)
				 * a=2: (2, 5), (2, 15)
				 * 最终结果序列 b: 10, 20, 5, 15 -> 乱序。
				 *
				 * 同理，如果索引是 (a, b, c)，查询 WHERE a=1 AND b IN (2, 3)，
				 * 那么 c 的有序性也会被破坏。
				 */
				if (indexcol > 0)
				{
					if (skip_lower_saop)
					{
						/*
						 * 如果调用者想要保留索引的排序能力（例如为了 ORDER BY），
						 * 我们必须跳过这些会破坏排序的非首列数组条件。
						 * 标记 skip_lower_saop 为 true 并跳过。
						 */
						*skip_lower_saop = true;
						continue;
					}
					/*
					 * 如果不跳过，说明我们优先考虑过滤性能而非排序。
					 * 记录 found_lower_saop_clause = true，表示结果可能是乱序的。
					 */
					found_lower_saop_clause = true;
				}
			}            /* 条件可用，将其添加到索引条件列表 */
            index_clauses = lappend(index_clauses, iclause);
            /*
             * 将条件涉及的外部关系添加到 outer_relids 集合。
             * outer_relids 记录了当前索引扫描路径所依赖的所有外部表（Outer Relations）的 ID 集合。
             * 如果 outer_relids 不为空，意味着这是一个参数化路径（Parameterized Path），
             * 必须在获取了这些外部表的值之后才能执行。
             */
            outer_relids = bms_add_members(outer_relids,
                                           rinfo->clause_relids);
        }

		/*
		 * 早期退出（Early Exit）检查。
		 * 如果这种索引类型必须要有查询条件才能工作（比如 Hash 索引），
		 * 但我们连第一个列的查询条件都没找到，那就别试了，这个索引没法用。
		 *
		 * index_clauses == NIL 表示到目前为止没有任何可用的索引条件。
		 * !index->amoptionalkey 表示该索引类型不允许没有任何键条件的扫描。
		 *		比如 Hash 索引就要求至少有一个键条件才能使用。
		 *		而 B-Tree 索引则允许没有键条件的全表扫描。
		 *
		 * 如果两者都成立，说明当前索引无法满足查询条件的要求，
		 * 因此直接返回空列表，表示无法生成任何索引路径。
		 */
        if (index_clauses == NIL && !index->amoptionalkey)
            return NIL;  // 不满足条件，返回空列表
    }

	/*
	 * 从outer_relids中移除索引自己的关系ID
	 * 清理和规范化 outer_relids 集合，确保它只包含真正的“外部”依赖。
	 */
	outer_relids = bms_del_member(outer_relids, rel->relid);
	/*
	 * 将空的 Bitmapset 指针规范化为 NULL
	 * 虽然空的 Bitmapset 在逻辑上也是空集，但为了后续代码判断方便（直接检查 if (outer_relids)），
	 * 这里统一将其置为 NULL。
	 */
	if (bms_is_empty(outer_relids))
        outer_relids = NULL;

    /* 计算用于成本估算的循环计数 */
    loop_count = get_loop_count(root, rel->relid, outer_relids);

	/*
	 * 第二步：确定这个索引扫描路径是否能提供有用的排序（PathKeys）。
	 * 优化器想知道：“如果我用这个索引扫描，出来的结果是不是天然有序的？
	 * 如果是，这个顺序对当前的查询（比如 ORDER BY 或 Merge Join）有用吗？”
	 * 这与我们只尝试构建位图索引扫描的情况无关，也与我们必须假设扫描无序的情况无关
     */
	/*
	 * 只有同时满足以下三个条件，我们才关心排序：
	 * 1、不是位图扫描 (scantype != ST_BITMAPSCAN)：位图扫描出来的结果是按物理块号排序的，
	 * 索引本身的逻辑顺序会丢失，所以位图扫描永远不提供逻辑排序。
	 * 2、没有乱序因素 (!found_lower_saop_clause)：之前提到过，如果在非首列用了 IN，B-Tree 的排序就乱了。如果乱了，那也就没法提供排序了。
	 * 3、查询确实需要排序 (has_useful_pathkeys)：如果用户查询既没有 ORDER BY，也没有 GROUP BY，也不需要 Merge Join，那就算索引有序也没啥用，不用费劲去算了。
	 */
	pathkeys_possibly_useful = (scantype != ST_BITMAPSCAN &&
                               !found_lower_saop_clause &&
                               has_useful_pathkeys(root, rel));
    // 检查索引是否有序（通过sortopfamily是否存在判断）
    index_is_ordered = (index->sortopfamily != NULL);
    if (index_is_ordered && pathkeys_possibly_useful)
    {
        // 构建正向扫描方向的索引排序键
        index_pathkeys = build_index_pathkeys(root, index,
                                              ForwardScanDirection);
        // 截断无用的排序键
        useful_pathkeys = truncate_useless_pathkeys(root, rel,
                                                   index_pathkeys);
        orderbyclauses = NIL;
        orderbyclausecols = NIL;
    }
    else if (index->amcanorderbyop && pathkeys_possibly_useful)
    {
        /* 查看我们是否可以为query_pathkeys生成排序操作符 */
        match_pathkeys_to_index(index, root->query_pathkeys,
                               &orderbyclauses,
                               &orderbyclausecols);
        if (orderbyclauses)
            useful_pathkeys = root->query_pathkeys;
        else
            useful_pathkeys = NIL;
    }
    else
    {
        // 排序键无用的情况
        useful_pathkeys = NIL;
        orderbyclauses = NIL;
        orderbyclausecols = NIL;
    }

    /*
     * 第三步：检查是否可以进行仅索引扫描
     * 如果我们不构建普通索引扫描，则这无关紧要，因为位图扫描无论如何都不支持索引数据检索
     */
    index_only_scan = (scantype != ST_BITMAPSCAN &&
                      check_index_only(rel, index));

    /*
     * 第四步：如果当前条件中有相关的限制条件，或者索引排序可能对后续合并或最终输出排序有用，
     * 或者索引有有用的谓词，或者可以进行仅索引扫描，则生成索引扫描路径
     */
    if (index_clauses != NIL || useful_pathkeys != NIL || useful_predicate ||
        index_only_scan)
    {
        // 创建索引路径（非并行）
        ipath = create_index_path(root, index,
                                 index_clauses,
                                 orderbyclauses,
                                 orderbyclausecols,
                                 useful_pathkeys,
                                 index_is_ordered ?
                                 ForwardScanDirection :
                                 NoMovementScanDirection,
                                 index_only_scan,
                                 outer_relids,
                                 loop_count,
                                 false);
        // 将路径添加到结果列表
        result = lappend(result, ipath);

        /*
         * 如果适合，考虑并行索引扫描
         * 位图索引扫描不允许并行索引扫描
         */
        if (index->amcanparallel &&          // 索引支持并行操作
            rel->consider_parallel &&        // 关系允许并行
            outer_relids == NULL &&          // 没有外部关系依赖
            scantype != ST_BITMAPSCAN)       // 不是位图扫描
        {
            // 创建并行索引路径
            ipath = create_index_path(root, index,
                                     index_clauses,
                                     orderbyclauses,
                                     orderbyclausecols,
                                     useful_pathkeys,
                                     index_is_ordered ?
                                     ForwardScanDirection :
                                     NoMovementScanDirection,
                                     index_only_scan,
                                     outer_relids,
                                     loop_count,
                                     true); // 启用并行

            /*
             * 成本计算后，如果发现使用并行工作进程不值得，就释放它
             * 否则将其添加为部分路径
             */
            if (ipath->path.parallel_workers > 0)
                add_partial_path(rel, (Path *) ipath);
            else
                pfree(ipath);
        }
    }

    /*
     * 第五步：如果索引是有序的，反向扫描可能也很有用
     * 对于需要反向排序的查询，反向索引扫描可能更有效
     */
    if (index_is_ordered && pathkeys_possibly_useful)
    {
        // 构建反向扫描方向的索引排序键
        index_pathkeys = build_index_pathkeys(root, index,
                                              BackwardScanDirection);
        // 截断无用的排序键
        useful_pathkeys = truncate_useless_pathkeys(root, rel,
                                                   index_pathkeys);
        // 只有当反向排序键有用时，才生成反向扫描路径
        if (useful_pathkeys != NIL)
        {
            // 创建反向索引路径（非并行）
            ipath = create_index_path(root, index,
                                     index_clauses,
                                     NIL,
                                     NIL,
                                     useful_pathkeys,
                                     BackwardScanDirection,
                                     index_only_scan,
                                     outer_relids,
                                     loop_count,
                                     false);
            // 将路径添加到结果列表
            result = lappend(result, ipath);

            /* 如果适合，考虑并行反向索引扫描 */
            if (index->amcanparallel &&          // 索引支持并行操作
                rel->consider_parallel &&        // 关系允许并行
                outer_relids == NULL &&          // 没有外部关系依赖
                scantype != ST_BITMAPSCAN)       // 不是位图扫描
            {
                // 创建并行反向索引路径
                ipath = create_index_path(root, index,
                                         index_clauses,
                                         NIL,
                                         NIL,
                                         useful_pathkeys,
                                         BackwardScanDirection,
                                         index_only_scan,
                                         outer_relids,
                                         loop_count,
                                         true); // 启用并行

                /*
                 * 成本计算后，如果发现使用并行工作进程不值得，就释放它
                 * 否则将其添加为部分路径
                 */
                if (ipath->path.parallel_workers > 0)
                    add_partial_path(rel, (Path *) ipath);
                else
                    pfree(ipath);
            }
        }
    }

    /* 返回生成的所有索引路径列表 */
    return result;
}


/*
 * build_paths_for_OR
 *	  Given a list of restriction clauses from one arm of an OR clause,
 *	  construct all matching IndexPaths for the relation.
 *
 * Here we must scan all indexes of the relation, since a bitmap OR tree
 * can use multiple indexes.
 *
 * The caller actually supplies two lists of restriction clauses: some
 * "current" ones and some "other" ones.  Both lists can be used freely
 * to match keys of the index, but an index must use at least one of the
 * "current" clauses to be considered usable.  The motivation for this is
 * examples like
 *		WHERE (x = 42) AND (... OR (y = 52 AND z = 77) OR ....)
 * While we are considering the y/z subclause of the OR, we can use "x = 42"
 * as one of the available index conditions; but we shouldn't match the
 * subclause to any index on x alone, because such a Path would already have
 * been generated at the upper level.  So we could use an index on x,y,z
 * or an index on x,y for the OR subclause, but not an index on just x.
 * When dealing with a partial index, a match of the index predicate to
 * one of the "current" clauses also makes the index usable.
 *
 * 'rel' is the relation for which we want to generate index paths
 * 'clauses' is the current list of clauses (RestrictInfo nodes)
 * 'other_clauses' is the list of additional upper-level clauses
 */
static List *
build_paths_for_OR(PlannerInfo *root, RelOptInfo *rel,
				   List *clauses, List *other_clauses)
{
	List	   *result = NIL;
	List	   *all_clauses = NIL;	/* not computed till needed */
	ListCell   *lc;

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(lc);
		IndexClauseSet clauseset;
		List	   *indexpaths;
		bool		useful_predicate;

		/* Ignore index if it doesn't support bitmap scans */
		if (!index->amhasgetbitmap)
			continue;

		/*
		 * Ignore partial indexes that do not match the query.  If a partial
		 * index is marked predOK then we know it's OK.  Otherwise, we have to
		 * test whether the added clauses are sufficient to imply the
		 * predicate. If so, we can use the index in the current context.
		 *
		 * We set useful_predicate to true iff the predicate was proven using
		 * the current set of clauses.  This is needed to prevent matching a
		 * predOK index to an arm of an OR, which would be a legal but
		 * pointlessly inefficient plan.  (A better plan will be generated by
		 * just scanning the predOK index alone, no OR.)
		 */
		useful_predicate = false;
		if (index->indpred != NIL)
		{
			if (index->predOK)
			{
				/* Usable, but don't set useful_predicate */
			}
			else
			{
				/* Form all_clauses if not done already */
				if (all_clauses == NIL)
					all_clauses = list_concat(list_copy(clauses),
											  other_clauses);

				if (!predicate_implied_by(index->indpred, all_clauses, false))
					continue;	/* can't use it at all */

				if (!predicate_implied_by(index->indpred, other_clauses, false))
					useful_predicate = true;
			}
		}

		/*
		 * Identify the restriction clauses that can match the index.
		 */
		MemSet(&clauseset, 0, sizeof(clauseset));
		match_clauses_to_index(root, clauses, index, &clauseset);

		/*
		 * If no matches so far, and the index predicate isn't useful, we
		 * don't want it.
		 */
		if (!clauseset.nonempty && !useful_predicate)
			continue;

		/*
		 * Add "other" restriction clauses to the clauseset.
		 */
		match_clauses_to_index(root, other_clauses, index, &clauseset);

		/*
		 * Construct paths if possible.
		 */
		indexpaths = build_index_paths(root, rel,
									   index, &clauseset,
									   useful_predicate,
									   ST_BITMAPSCAN,
									   NULL,
									   NULL);
		result = list_concat(result, indexpaths);
	}

	return result;
}
/*
 * generate_bitmap_or_paths
 *    遍历条件列表寻找OR子句，并为每个可以处理的OR子句生成一个BitmapOrPath
 *    返回生成的BitmapOrPaths列表
 *
 * other_clauses是一个额外条件列表，在生成indexquals时可以假设这些条件为真，
 * 但不会在其中搜索OR子句。（详见build_paths_for_OR()的动机说明）
 *
 * 参数：
 *   root - 查询规划器的根结构，包含查询的规划信息
 *   rel - 要生成路径的关系
 *   clauses - 要检查OR子句的条件列表
 *   other_clauses - 额外的上下文条件列表
 *
 * 返回值：
 *   生成的BitmapOrPath路径列表
 */
static List *
generate_bitmap_or_paths(PlannerInfo *root, RelOptInfo *rel,
                         List *clauses, List *other_clauses)
{
    List       *result = NIL;          // 存储生成的BitmapOrPath结果列表
    List       *all_clauses;           // 所有条件（clauses和other_clauses的合并）
    ListCell   *lc;                    // 用于遍历clauses列表的迭代器

    /*
     * 我们可以将当前条件和其他条件都用作build_paths_for_OR的上下文；
     * 无需从列表中移除OR子句
     */
    // 复制clauses列表并与other_clauses合并
    all_clauses = list_concat(list_copy(clauses), other_clauses);

    // 遍历每个条件，寻找OR子句
    foreach(lc, clauses)
    {
        RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);  // 当前条件
        List       *pathlist;          // 存储每个OR分支匹配的最佳位图路径
        Path       *bitmapqual;        // 位图条件路径
        ListCell   *j;                 // 用于遍历OR子句参数的迭代器

        /* 忽略不是OR子句的RestrictInfo */
        if (!restriction_is_or_clause(rinfo))
            continue;

        /*
         * 我们必须能够为OR的每个分支匹配至少一个索引，否则无法使用它
         */
        pathlist = NIL;  // 初始化OR分支路径列表
        // 遍历OR子句的每个参数
        foreach(j, ((BoolExpr *) rinfo->orclause)->args)
        {
            Node       *orarg = (Node *) lfirst(j);  // OR子句的一个分支
            List       *indlist;                     // 该分支匹配的索引路径列表

            /* OR参数应该是AND子句或子RestrictInfo */
            if (is_andclause(orarg))
            {
                // OR分支是AND子句
                List       *andargs = ((BoolExpr *) orarg)->args;

                // 为AND子句构建索引路径
                indlist = build_paths_for_OR(root, rel,
                                            andargs,
                                            all_clauses);

                /* 递归处理可能存在的子OR子句 */
                indlist = list_concat(indlist,
                                    generate_bitmap_or_paths(root, rel,
                                                            andargs,
                                                            all_clauses));
            }
            else
            {
                // OR分支是单个条件
                RestrictInfo *or_rinfo = castNode(RestrictInfo, orarg);
                List       *orargs;

                // 断言：单个OR分支不应该是OR子句
                Assert(!restriction_is_or_clause(or_rinfo));
                // 创建只有一个元素的列表
                orargs = list_make1(or_rinfo);

                // 为单个条件构建索引路径
                indlist = build_paths_for_OR(root, rel,
                                            orargs,
                                            all_clauses);
            }

            /*
             * 如果这个分支没有匹配到任何索引路径，
             * 我们就无法处理这个OR子句
             */
            if (indlist == NIL)
            {
                pathlist = NIL;  // 标记整个OR子句无法处理
                break;          // 跳出循环，不再处理其他分支
            }

            /*
             * 从匹配的索引路径中选择最有希望的AND组合，
             * 并将其添加到pathlist
             */
            bitmapqual = choose_bitmap_and(root, rel, indlist);
            pathlist = lappend(pathlist, bitmapqual);
        }

        /*
         * 如果我们为OR的每个分支都找到了匹配的路径，
         * 那么将它们转换为BitmapOrPath，并添加到结果列表
         */
        if (pathlist != NIL)
        {
            // 创建位图OR路径
            bitmapqual = (Path *) create_bitmap_or_path(root, rel, pathlist);
            // 将路径添加到结果列表
            result = lappend(result, bitmapqual);
        }
    }

    // 返回生成的所有BitmapOrPath列表
    return result;
}


/*
 * choose_bitmap_and
 *		Given a nonempty list of bitmap paths, AND them into one path.
 *
 * This is a nontrivial decision since we can legally use any subset of the
 * given path set.  We want to choose a good tradeoff between selectivity
 * and cost of computing the bitmap.
 *
 * The result is either a single one of the inputs, or a BitmapAndPath
 * combining multiple inputs.
 */
static Path *
choose_bitmap_and(PlannerInfo *root, RelOptInfo *rel, List *paths)
{
	int			npaths = list_length(paths);
	PathClauseUsage **pathinfoarray;
	PathClauseUsage *pathinfo;
	List	   *clauselist;
	List	   *bestpaths = NIL;
	Cost		bestcost = 0;
	int			i,
				j;
	ListCell   *l;

	Assert(npaths > 0);			/* else caller error */
	if (npaths == 1)
		return (Path *) linitial(paths);	/* easy case */

	/*
	 * In theory we should consider every nonempty subset of the given paths.
	 * In practice that seems like overkill, given the crude nature of the
	 * estimates, not to mention the possible effects of higher-level AND and
	 * OR clauses.  Moreover, it's completely impractical if there are a large
	 * number of paths, since the work would grow as O(2^N).
	 *
	 * As a heuristic, we first check for paths using exactly the same sets of
	 * WHERE clauses + index predicate conditions, and reject all but the
	 * cheapest-to-scan in any such group.  This primarily gets rid of indexes
	 * that include the interesting columns but also irrelevant columns.  (In
	 * situations where the DBA has gone overboard on creating variant
	 * indexes, this can make for a very large reduction in the number of
	 * paths considered further.)
	 *
	 * We then sort the surviving paths with the cheapest-to-scan first, and
	 * for each path, consider using that path alone as the basis for a bitmap
	 * scan.  Then we consider bitmap AND scans formed from that path plus
	 * each subsequent (higher-cost) path, adding on a subsequent path if it
	 * results in a reduction in the estimated total scan cost. This means we
	 * consider about O(N^2) rather than O(2^N) path combinations, which is
	 * quite tolerable, especially given than N is usually reasonably small
	 * because of the prefiltering step.  The cheapest of these is returned.
	 *
	 * We will only consider AND combinations in which no two indexes use the
	 * same WHERE clause.  This is a bit of a kluge: it's needed because
	 * costsize.c and clausesel.c aren't very smart about redundant clauses.
	 * They will usually double-count the redundant clauses, producing a
	 * too-small selectivity that makes a redundant AND step look like it
	 * reduces the total cost.  Perhaps someday that code will be smarter and
	 * we can remove this limitation.  (But note that this also defends
	 * against flat-out duplicate input paths, which can happen because
	 * match_join_clauses_to_index will find the same OR join clauses that
	 * extract_restriction_or_clauses has pulled OR restriction clauses out
	 * of.)
	 *
	 * For the same reason, we reject AND combinations in which an index
	 * predicate clause duplicates another clause.  Here we find it necessary
	 * to be even stricter: we'll reject a partial index if any of its
	 * predicate clauses are implied by the set of WHERE clauses and predicate
	 * clauses used so far.  This covers cases such as a condition "x = 42"
	 * used with a plain index, followed by a clauseless scan of a partial
	 * index "WHERE x >= 40 AND x < 50".  The partial index has been accepted
	 * only because "x = 42" was present, and so allowing it would partially
	 * double-count selectivity.  (We could use predicate_implied_by on
	 * regular qual clauses too, to have a more intelligent, but much more
	 * expensive, check for redundancy --- but in most cases simple equality
	 * seems to suffice.)
	 */

	/*
	 * Extract clause usage info and detect any paths that use exactly the
	 * same set of clauses; keep only the cheapest-to-scan of any such groups.
	 * The surviving paths are put into an array for qsort'ing.
	 */
	pathinfoarray = (PathClauseUsage **)
		palloc(npaths * sizeof(PathClauseUsage *));
	clauselist = NIL;
	npaths = 0;
	foreach(l, paths)
	{
		Path	   *ipath = (Path *) lfirst(l);

		pathinfo = classify_index_clause_usage(ipath, &clauselist);

		/* If it's unclassifiable, treat it as distinct from all others */
		if (pathinfo->unclassifiable)
		{
			pathinfoarray[npaths++] = pathinfo;
			continue;
		}

		for (i = 0; i < npaths; i++)
		{
			if (!pathinfoarray[i]->unclassifiable &&
				bms_equal(pathinfo->clauseids, pathinfoarray[i]->clauseids))
				break;
		}
		if (i < npaths)
		{
			/* duplicate clauseids, keep the cheaper one */
			Cost		ncost;
			Cost		ocost;
			Selectivity nselec;
			Selectivity oselec;

			cost_bitmap_tree_node(pathinfo->path, &ncost, &nselec);
			cost_bitmap_tree_node(pathinfoarray[i]->path, &ocost, &oselec);
			if (ncost < ocost)
				pathinfoarray[i] = pathinfo;
		}
		else
		{
			/* not duplicate clauseids, add to array */
			pathinfoarray[npaths++] = pathinfo;
		}
	}

	/* If only one surviving path, we're done */
	if (npaths == 1)
		return pathinfoarray[0]->path;

	/* Sort the surviving paths by index access cost */
	qsort(pathinfoarray, npaths, sizeof(PathClauseUsage *),
		  path_usage_comparator);

	/*
	 * For each surviving index, consider it as an "AND group leader", and see
	 * whether adding on any of the later indexes results in an AND path with
	 * cheaper total cost than before.  Then take the cheapest AND group.
	 *
	 * Note: paths that are either clauseless or unclassifiable will have
	 * empty clauseids, so that they will not be rejected by the clauseids
	 * filter here, nor will they cause later paths to be rejected by it.
	 */
	for (i = 0; i < npaths; i++)
	{
		Cost		costsofar;
		List	   *qualsofar;
		Bitmapset  *clauseidsofar;
		ListCell   *lastcell;

		pathinfo = pathinfoarray[i];
		paths = list_make1(pathinfo->path);
		costsofar = bitmap_scan_cost_est(root, rel, pathinfo->path);
		qualsofar = list_concat(list_copy(pathinfo->quals),
								list_copy(pathinfo->preds));
		clauseidsofar = bms_copy(pathinfo->clauseids);
		lastcell = list_head(paths);	/* for quick deletions */

		for (j = i + 1; j < npaths; j++)
		{
			Cost		newcost;

			pathinfo = pathinfoarray[j];
			/* Check for redundancy */
			if (bms_overlap(pathinfo->clauseids, clauseidsofar))
				continue;		/* consider it redundant */
			if (pathinfo->preds)
			{
				bool		redundant = false;

				/* we check each predicate clause separately */
				foreach(l, pathinfo->preds)
				{
					Node	   *np = (Node *) lfirst(l);

					if (predicate_implied_by(list_make1(np), qualsofar, false))
					{
						redundant = true;
						break;	/* out of inner foreach loop */
					}
				}
				if (redundant)
					continue;
			}
			/* tentatively add new path to paths, so we can estimate cost */
			paths = lappend(paths, pathinfo->path);
			newcost = bitmap_and_cost_est(root, rel, paths);
			if (newcost < costsofar)
			{
				/* keep new path in paths, update subsidiary variables */
				costsofar = newcost;
				qualsofar = list_concat(qualsofar,
										list_copy(pathinfo->quals));
				qualsofar = list_concat(qualsofar,
										list_copy(pathinfo->preds));
				clauseidsofar = bms_add_members(clauseidsofar,
												pathinfo->clauseids);
				lastcell = lnext(lastcell);
			}
			else
			{
				/* reject new path, remove it from paths list */
				paths = list_delete_cell(paths, lnext(lastcell), lastcell);
			}
			Assert(lnext(lastcell) == NULL);
		}

		/* Keep the cheapest AND-group (or singleton) */
		if (i == 0 || costsofar < bestcost)
		{
			bestpaths = paths;
			bestcost = costsofar;
		}

		/* some easy cleanup (we don't try real hard though) */
		list_free(qualsofar);
	}

	if (list_length(bestpaths) == 1)
		return (Path *) linitial(bestpaths);	/* no need for AND */
	return (Path *) create_bitmap_and_path(root, rel, bestpaths);
}

/* qsort comparator to sort in increasing index access cost order */
static int
path_usage_comparator(const void *a, const void *b)
{
	PathClauseUsage *pa = *(PathClauseUsage *const *) a;
	PathClauseUsage *pb = *(PathClauseUsage *const *) b;
	Cost		acost;
	Cost		bcost;
	Selectivity aselec;
	Selectivity bselec;

	cost_bitmap_tree_node(pa->path, &acost, &aselec);
	cost_bitmap_tree_node(pb->path, &bcost, &bselec);

	/*
	 * If costs are the same, sort by selectivity.
	 */
	if (acost < bcost)
		return -1;
	if (acost > bcost)
		return 1;

	if (aselec < bselec)
		return -1;
	if (aselec > bselec)
		return 1;

	return 0;
}

/*
 * Estimate the cost of actually executing a bitmap scan with a single
 * index path (which could be a BitmapAnd or BitmapOr node).
 */
static Cost
bitmap_scan_cost_est(PlannerInfo *root, RelOptInfo *rel, Path *ipath)
{
	BitmapHeapPath bpath;

	/* Set up a dummy BitmapHeapPath */
	bpath.path.type = T_BitmapHeapPath;
	bpath.path.pathtype = T_BitmapHeapScan;
	bpath.path.parent = rel;
	bpath.path.pathtarget = rel->reltarget;
	bpath.path.param_info = ipath->param_info;
	bpath.path.pathkeys = NIL;
	bpath.bitmapqual = ipath;

	/*
	 * Check the cost of temporary path without considering parallelism.
	 * Parallel bitmap heap path will be considered at later stage.
	 */
	bpath.path.parallel_workers = 0;

	/* Now we can do cost_bitmap_heap_scan */
	cost_bitmap_heap_scan(&bpath.path, root, rel,
						  bpath.path.param_info,
						  ipath,
						  get_loop_count(root, rel->relid,
										 PATH_REQ_OUTER(ipath)));

	return bpath.path.total_cost;
}

/*
 * Estimate the cost of actually executing a BitmapAnd scan with the given
 * inputs.
 */
static Cost
bitmap_and_cost_est(PlannerInfo *root, RelOptInfo *rel, List *paths)
{
	BitmapAndPath *apath;

	/*
	 * Might as well build a real BitmapAndPath here, as the work is slightly
	 * too complicated to be worth repeating just to save one palloc.
	 */
	apath = create_bitmap_and_path(root, rel, paths);

	return bitmap_scan_cost_est(root, rel, (Path *) apath);
}


/*
 * classify_index_clause_usage
 *		Construct a PathClauseUsage struct describing the WHERE clauses and
 *		index predicate clauses used by the given indexscan path.
 *		We consider two clauses the same if they are equal().
 *
 * At some point we might want to migrate this info into the Path data
 * structure proper, but for the moment it's only needed within
 * choose_bitmap_and().
 *
 * *clauselist is used and expanded as needed to identify all the distinct
 * clauses seen across successive calls.  Caller must initialize it to NIL
 * before first call of a set.
 */
static PathClauseUsage *
classify_index_clause_usage(Path *path, List **clauselist)
{
	PathClauseUsage *result;
	Bitmapset  *clauseids;
	ListCell   *lc;

	result = (PathClauseUsage *) palloc(sizeof(PathClauseUsage));
	result->path = path;

	/* Recursively find the quals and preds used by the path */
	result->quals = NIL;
	result->preds = NIL;
	find_indexpath_quals(path, &result->quals, &result->preds);

	/*
	 * Some machine-generated queries have outlandish numbers of qual clauses.
	 * To avoid getting into O(N^2) behavior even in this preliminary
	 * classification step, we want to limit the number of entries we can
	 * accumulate in *clauselist.  Treat any path with more than 100 quals +
	 * preds as unclassifiable, which will cause calling code to consider it
	 * distinct from all other paths.
	 */
	if (list_length(result->quals) + list_length(result->preds) > 100)
	{
		result->clauseids = NULL;
		result->unclassifiable = true;
		return result;
	}

	/* Build up a bitmapset representing the quals and preds */
	clauseids = NULL;
	foreach(lc, result->quals)
	{
		Node	   *node = (Node *) lfirst(lc);

		clauseids = bms_add_member(clauseids,
								   find_list_position(node, clauselist));
	}
	foreach(lc, result->preds)
	{
		Node	   *node = (Node *) lfirst(lc);

		clauseids = bms_add_member(clauseids,
								   find_list_position(node, clauselist));
	}
	result->clauseids = clauseids;
	result->unclassifiable = false;

	return result;
}


/*
 * find_indexpath_quals
 *
 * Given the Path structure for a plain or bitmap indexscan, extract lists
 * of all the index clauses and index predicate conditions used in the Path.
 * These are appended to the initial contents of *quals and *preds (hence
 * caller should initialize those to NIL).
 *
 * Note we are not trying to produce an accurate representation of the AND/OR
 * semantics of the Path, but just find out all the base conditions used.
 *
 * The result lists contain pointers to the expressions used in the Path,
 * but all the list cells are freshly built, so it's safe to destructively
 * modify the lists (eg, by concat'ing with other lists).
 */
static void
find_indexpath_quals(Path *bitmapqual, List **quals, List **preds)
{
	if (IsA(bitmapqual, BitmapAndPath))
	{
		BitmapAndPath *apath = (BitmapAndPath *) bitmapqual;
		ListCell   *l;

		foreach(l, apath->bitmapquals)
		{
			find_indexpath_quals((Path *) lfirst(l), quals, preds);
		}
	}
	else if (IsA(bitmapqual, BitmapOrPath))
	{
		BitmapOrPath *opath = (BitmapOrPath *) bitmapqual;
		ListCell   *l;

		foreach(l, opath->bitmapquals)
		{
			find_indexpath_quals((Path *) lfirst(l), quals, preds);
		}
	}
	else if (IsA(bitmapqual, IndexPath))
	{
		IndexPath  *ipath = (IndexPath *) bitmapqual;
		ListCell   *l;

		foreach(l, ipath->indexclauses)
		{
			IndexClause *iclause = (IndexClause *) lfirst(l);

			*quals = lappend(*quals, iclause->rinfo->clause);
		}
		*preds = list_concat(*preds, list_copy(ipath->indexinfo->indpred));
	}
	else
		elog(ERROR, "unrecognized node type: %d", nodeTag(bitmapqual));
}


/*
 * find_list_position
 *		Return the given node's position (counting from 0) in the given
 *		list of nodes.  If it's not equal() to any existing list member,
 *		add it at the end, and return that position.
 */
static int
find_list_position(Node *node, List **nodelist)
{
	int			i;
	ListCell   *lc;

	i = 0;
	foreach(lc, *nodelist)
	{
		Node	   *oldnode = (Node *) lfirst(lc);

		if (equal(node, oldnode))
			return i;
		i++;
	}

	*nodelist = lappend(*nodelist, node);

	return i;
}


/*
 * check_index_only
 *		Determine whether an index-only scan is possible for this index.
 */
static bool
check_index_only(RelOptInfo *rel, IndexOptInfo *index)
{
	bool		result;
	Bitmapset  *attrs_used = NULL;
	Bitmapset  *index_canreturn_attrs = NULL;
	Bitmapset  *index_cannotreturn_attrs = NULL;
	ListCell   *lc;
	int			i;

	/* Index-only scans must be enabled */
	if (!enable_indexonlyscan)
		return false;

	/*
	 * Check that all needed attributes of the relation are available from the
	 * index.
	 */

	/*
	 * First, identify all the attributes needed for joins or final output.
	 * Note: we must look at rel's targetlist, not the attr_needed data,
	 * because attr_needed isn't computed for inheritance child rels.
	 */
	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &attrs_used);

	/*
	 * Add all the attributes used by restriction clauses; but consider only
	 * those clauses not implied by the index predicate, since ones that are
	 * so implied don't need to be checked explicitly in the plan.
	 *
	 * Note: attributes used only in index quals would not be needed at
	 * runtime either, if we are certain that the index is not lossy.  However
	 * it'd be complicated to account for that accurately, and it doesn't
	 * matter in most cases, since we'd conclude that such attributes are
	 * available from the index anyway.
	 */
	foreach(lc, index->indrestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		pull_varattnos((Node *) rinfo->clause, rel->relid, &attrs_used);
	}

	/*
	 * Construct a bitmapset of columns that the index can return back in an
	 * index-only scan.  If there are multiple index columns containing the
	 * same attribute, all of them must be capable of returning the value,
	 * since we might recheck operators on any of them.  (Potentially we could
	 * be smarter about that, but it's such a weird situation that it doesn't
	 * seem worth spending a lot of sweat on.)
	 */
	for (i = 0; i < index->ncolumns; i++)
	{
		int			attno = index->indexkeys[i];

		/*
		 * For the moment, we just ignore index expressions.  It might be nice
		 * to do something with them, later.
		 */
		if (attno == 0)
			continue;

		if (index->canreturn[i])
			index_canreturn_attrs =
				bms_add_member(index_canreturn_attrs,
							   attno - FirstLowInvalidHeapAttributeNumber);
		else
			index_cannotreturn_attrs =
				bms_add_member(index_cannotreturn_attrs,
							   attno - FirstLowInvalidHeapAttributeNumber);
	}

	index_canreturn_attrs = bms_del_members(index_canreturn_attrs,
											index_cannotreturn_attrs);

	/* Do we have all the necessary attributes? */
	result = bms_is_subset(attrs_used, index_canreturn_attrs);

	bms_free(attrs_used);
	bms_free(index_canreturn_attrs);
	bms_free(index_cannotreturn_attrs);

	return result;
}

/*
 * get_loop_count
 *		为带有给定外部关系ID集合的参数化路径选择用于成本估算的循环次数。
 *
 * 由于我们在生成连接关系之前就会生成参数化路径，因此无法准确预测参数化路径会被迭代多少次；
 * 我们不知道嵌套循环外层的关系有多大。不过，我们应该在路径成本估算时以某种方式考虑多次迭代的影响。
 * 这里采用的启发式方法是：使用路径所需的所有外部基本关系中，行数最小的那个的行数作为循环次数。
 * （也可以考虑用最大的那个，但那样太乐观了。）对于只有一个外部关系的情况，这当然是正确的；
 * 对于多表连接，这也是一种合理的零阶近似。
 *
 * 此外，我们还会检查每个连接条件的另一侧是否处于某个半连接（semijoin）的内表，
 * 而当前关系处于外表。如果是这样，参数化路径只有在半连接右表已经去重（unique-ified）时才能使用，
 * 因此我们应该用右表唯一行数而不是原始行数。
 *
 * 注意：为了让本函数工作，allpaths.c 必须在开始计算路径之前（或至少在调用 create_index_paths() 之前）
 * 就为所有基表关系建立好行数估算。
 */
static double
get_loop_count(PlannerInfo *root, Index cur_relid, Relids outer_relids)
{
	double		result;
	int			outer_relid;

	/* 非参数化路径，直接返回 1.0 */
	if (outer_relids == NULL)
		return 1.0;

	result = 0.0;
	outer_relid = -1;
	while ((outer_relid = bms_next_member(outer_relids, outer_relid)) >= 0)
	{
		RelOptInfo *outer_rel;
		double		rowcount;

		/* 健壮性检查：忽略非法的 relid 下标 */
		if (outer_relid >= root->simple_rel_array_size)
			continue;
		outer_rel = root->simple_rel_array[outer_relid];
		if (outer_rel == NULL)
			continue;
		Assert(outer_rel->relid == outer_relid);	/* 数组一致性检查 */

		/* 其他关系如果被证明为空，则忽略 */
		if (IS_DUMMY_REL(outer_rel))
			continue;

		/* 否则，该关系的行数估算应该已经有效 */
		Assert(outer_rel->rows > 0);

		/* 检查该关系是否处于某个半连接的内表 */
		rowcount = adjust_rowcount_for_semijoins(root,
												 cur_relid,
												 outer_relid,
												 outer_rel->rows);

		/* 记录所有外部关系中最小的行数估算 */
		if (result == 0.0 || result > rowcount)
			result = rowcount;
	}
	/* 如果没有找到有效关系，则返回 1.0（理论上不应发生） */
	return (result > 0.0) ? result : 1.0;
}

/*
 * Check to see if outer_relid is on the inside of any semijoin that cur_relid
 * is on the outside of.  If so, replace rowcount with the estimated number of
 * unique rows from the semijoin RHS (assuming that's smaller, which it might
 * not be).  The estimate is crude but it's the best we can do at this stage
 * of the proceedings.
 */
static double
adjust_rowcount_for_semijoins(PlannerInfo *root,
							  Index cur_relid,
							  Index outer_relid,
							  double rowcount)
{
	ListCell   *lc;

	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);

		if (sjinfo->jointype == JOIN_SEMI &&
			bms_is_member(cur_relid, sjinfo->syn_lefthand) &&
			bms_is_member(outer_relid, sjinfo->syn_righthand))
		{
			/* Estimate number of unique-ified rows */
			double		nraw;
			double		nunique;

			nraw = approximate_joinrel_size(root, sjinfo->syn_righthand);
			nunique = estimate_num_groups(root,
										  sjinfo->semi_rhs_exprs,
										  nraw,
										  NULL);
			if (rowcount > nunique)
				rowcount = nunique;
		}
	}
	return rowcount;
}

/*
 * Make an approximate estimate of the size of a joinrel.
 *
 * We don't have enough info at this point to get a good estimate, so we
 * just multiply the base relation sizes together.  Fortunately, this is
 * the right answer anyway for the most common case with a single relation
 * on the RHS of a semijoin.  Also, estimate_num_groups() has only a weak
 * dependency on its input_rows argument (it basically uses it as a clamp).
 * So we might be able to get a fairly decent end result even with a severe
 * overestimate of the RHS's raw size.
 */
static double
approximate_joinrel_size(PlannerInfo *root, Relids relids)
{
	double		rowcount = 1.0;
	int			relid;

	relid = -1;
	while ((relid = bms_next_member(relids, relid)) >= 0)
	{
		RelOptInfo *rel;

		/* Paranoia: ignore bogus relid indexes */
		if (relid >= root->simple_rel_array_size)
			continue;
		rel = root->simple_rel_array[relid];
		if (rel == NULL)
			continue;
		Assert(rel->relid == relid);	/* sanity check on array */

		/* Relation could be proven empty, if so ignore */
		if (IS_DUMMY_REL(rel))
			continue;

		/* Otherwise, rel's rows estimate should be valid by now */
		Assert(rel->rows > 0);

		/* Accumulate product */
		rowcount *= rel->rows;
	}
	return rowcount;
}


/****************************************************************************
 *				----  ROUTINES TO CHECK QUERY CLAUSES  ----
 ****************************************************************************/

/*
 * match_restriction_clauses_to_index
 *	  找出与索引匹配的限制条件，并将匹配的条件加入 *clauseset。
 */
static void
match_restriction_clauses_to_index(PlannerInfo *root,
								   IndexOptInfo *index,
								   IndexClauseSet *clauseset)
{
	/* 可以忽略被索引谓词隐含的条件 */
	match_clauses_to_index(root, index->indrestrictinfo, index, clauseset);
}

/*
 * match_join_clauses_to_index
 *    识别关系中与索引匹配的连接条件子句
 *    将匹配的子句添加到*clauseset中
 *    同时，将任何潜在可用的连接OR子句添加到*joinorclauses中
 *
 * 参数说明：
 * - root: 规划器信息根结构，包含查询的整体规划上下文
 * - rel: 正在考虑的关系优化信息结构
 * - index: 要匹配的索引优化信息结构
 * - clauseset: 输出参数，用于存储匹配索引的子句集合
 * - joinorclauses: 输出参数，用于存储潜在可用的连接OR子句列表
 *
 * 功能：此函数在查询优化过程中，寻找能够利用指定索引的连接条件，
 *      为后续生成高效的连接执行计划提供支持
 */
static void
match_join_clauses_to_index(PlannerInfo *root,
                          RelOptInfo *rel, IndexOptInfo *index,
                          IndexClauseSet *clauseset,
                          List **joinorclauses)
{
    ListCell   *lc; /* 用于遍历列表的迭代器 */

    /* 扫描关系的所有连接条件子句 */
    foreach(lc, rel->joininfo)
    {
        /* 获取当前连接条件的RestrictInfo结构 */
        RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

        /* 检查该连接条件是否可以移动到当前关系上执行
		 * 这是谓词下推(predicate pushdown)优化的关键步骤，确保条件尽可能早地执行
		 * 如果约束条件不满足谓词下推的要求，则它没有产生参数化路径的可能性
		 * 因此，我们跳过
		 */
        if (!join_clause_is_movable_to(rinfo, rel))
            continue; /* 如果不可移动，则跳过该子句 */

        /* 子句可能可用，检查它是否是OR子句或是可匹配索引的子句 */
        if (restriction_is_or_clause(rinfo))
            /* 如果是OR子句，添加到joinorclauses列表中供后续处理 */
            *joinorclauses = lappend(*joinorclauses, rinfo);
        else
			/* 否则，尝试将该子句与索引进行匹配 */
			match_clause_to_index(root, rinfo, index, clauseset);
    }
}


/*
 * match_eclass_clauses_to_index
 *    识别与索引匹配的等价类(EquivalenceClass)连接子句
 *    将匹配的子句添加到*clauseset中
 *
 * 参数说明：
 * - root: 规划器信息根结构，包含查询的整体规划上下文
 * - index: 要匹配的索引优化信息结构
 * - clauseset: 输出参数，用于存储匹配索引的子句集合
 *
 * 功能：此函数在查询优化过程中，利用PostgreSQL的等价类(EC)机制，
 *      寻找能够用于索引访问的隐含连接条件，即使这些条件并未在原始查询中明确写出
 */
static void
match_eclass_clauses_to_index(PlannerInfo *root, IndexOptInfo *index,
                           IndexClauseSet *clauseset)
{
    int         indexcol; /* 用于遍历索引列的循环变量 */

    /* 如果关系不在任何等价类连接中，则无需处理，直接返回 */
    if (!index->rel->has_eclass_joins)
        return;

    /* 遍历索引的每个键列 */
    for (indexcol = 0; indexcol < index->nkeycolumns; indexcol++)
    {
        ec_member_matches_arg arg; /* 用于传递给回调函数的参数结构体 */
        List       *clauses;       /* 存储生成的隐含相等性子句 */

        /* 设置回调函数参数，指定当前处理的索引和列 */
        arg.index = index;
        arg.indexcol = indexcol;
        
        /* 为索引列生成隐含的相等性条件
         * 该函数会查找与指定列相关的等价类，并生成所有可能的等式条件
         * ec_member_matches_indexcol作为回调函数，用于过滤出与索引列兼容的等价成员
         * 最后一个参数排除连接到LATERAL引用表的条件，因为这些条件不能下推
         */
        clauses = generate_implied_equalities_for_column(root,
                                                       index->rel,
                                                       ec_member_matches_indexcol,
                                                       (void *) &arg,
                                                       index->rel->lateral_referencers);

        /*
         * 对于非B树索引，需要进一步验证生成的条件是否真正匹配索引
         * 因为等价类中的相等性操作符可能不在索引操作符类中
         * 例如，某些特殊索引类型可能有自己特定的相等性语义
         */
        match_clauses_to_index(root, clauses, index, clauseset);
    }
}

/*
 * match_clauses_to_index
 *	  尝试将条件列表中的每个条件与索引进行匹配。
 *
 * 对于每个匹配的条件，将其作为 IndexClause 加入到 *clauseset 的对应列表中。
 * （*clauseset 必须在首次调用前初始化为零。）
 */
static void
match_clauses_to_index(PlannerInfo *root,
                       List *clauses,
                       IndexOptInfo *index,
                       IndexClauseSet *clauseset)
{
    ListCell   *lc;	/* 遍历条件列表的指针 */

    /*
     * 对列表中的每个条件执行 match_clause_to_index()
     * 匹配的条件会被加入到 *clauseset 中。
     */
    foreach(lc, clauses)
    {
        // 1. 获取当前节点
        // lfirst_node 是一个宏，用于从链表节点 lc 中提取数据，并将其强制转换为 RestrictInfo* 类型。
        RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

        // 2. 尝试匹配
        // 调用核心逻辑函数 match_clause_to_index（注意这是单数形式），
        // 判断单个条件 rinfo 是否能被 index 使用。
        match_clause_to_index(root, rinfo, index, clauseset);
    }
}

/*
 * match_clause_to_index
 *	  测试一个条件是否可以用于索引。
 *
 * 如果条件可用，则将其作为 IndexClause 加入到 *clauseset 的对应列表中。
 * （*clauseset 必须在首次调用前初始化为零。）
 *
 * 注意：某些情况下同一个 RestrictInfo 可能来自多个地方。为避免重复输出，
 * 如果已经添加过该条件（指针相等即可），则拒绝再次添加。
 *
 * 注意：如果索引定义不合理，可能会有多个匹配的列。我们总是选择第一个匹配，
 * 这样可以避免同一个条件被多次用于不同索引列，导致选择性估算过高。
 */
static void
match_clause_to_index(PlannerInfo *root,
					  RestrictInfo *rinfo,
					  IndexOptInfo *index,
					  IndexClauseSet *clauseset)
{
	int			indexcol;

	/*
	 * 永远不要把伪常量条件用于索引。
	 * （通常伪常量不会包含 Var，因此不会匹配，但如果有人在常量上建表达式索引呢？
	 * 对于部分索引，这么做也不是完全没有道理。）
	 *
	 * 伪常量：这个条件不包含当前查询层级的任何变量（Vars），但在当前层级下其值是固定的（真或假）。
	 * 通常有两种情况：
	 * 真正的常量表达式：例如 WHERE 1 = 1 或 WHERE 1 = 2。
	 * 参数化值：例如在嵌套循环连接（Nested Loop Join）中，
	 * 内层表的查询条件引用了外层表的值（如 WHERE inner.x = outer.y）。对
	 * 于内层表的单次扫描来说，outer.y 是一个定值，但在整个查询计划构建阶段，它可能被标记为伪常量或参数。
	 *
	 * 不过，这里的 pseudoconstant 更多指第一种情况，即与当前扫描的每一行都无关的条件。
	 */
	if (rinfo->pseudoconstant)
		return;

	/*
	 * 如果该条件不能作为索引条件（因为必须等到更低安全级别的限制条件之后才能使用），则拒绝。
	 */
	if (!restriction_is_securely_promotable(rinfo, index->rel))
		return;

	/*
	 * 检查每个索引键列是否匹配
	 * nkeycolumns：真正的索引键列数
	 */
	for (indexcol = 0; indexcol < index->nkeycolumns; indexcol++)
	{
		IndexClause *iclause;
		ListCell   *lc;

		/* 忽略重复项
		 * 注意：某些情况下同一个 RestrictInfo 可能来自多个地方。为避免重复输出，
		 * 如果已经添加过该条件（指针相等即可），则拒绝再次添加。
		 * 这是一个防御性编程，确保无论上层逻辑多么复杂，
		 * 无论同一个条件通过什么路径（基表条件、连接条件、推导条件）被传递进来，
		 * 只要它是同一个内存对象，我们在最终的索引扫描条件列表中只保留一份。
		 */
		foreach(lc, clauseset->indexclauses[indexcol])
		{
			/* 取出一个已存在的 IndexClause */
			IndexClause* iclause = (IndexClause*)lfirst(lc);

			/*
			 * 比较指针地址：
			 * iclause->rinfo 是已保存条件的指针。
			 * rinfo 是当前正在尝试添加的新条件的指针。
			 */
			if (iclause->rinfo == rinfo)
				return; /* 如果指针相同，说明完全是同一个对象，直接返回，不做任何操作 */
		}

		/*
		 * 尝试将条件与索引列匹配
		 * 判断“这个条件”是否适用于“这个索引的这一列”。
		 * 如果匹配成功，返回一个指向 IndexClause 结构体的指针。这个结构体描述了如何使用这个条件来扫描索引。
		 */
		iclause = match_clause_to_indexcol(root,
										   rinfo,
										   indexcol,
										   index);

		/* 如果匹配成功，记录下来 */
		if (iclause)
		{
			clauseset->indexclauses[indexcol] =
				lappend(clauseset->indexclauses[indexcol], iclause);
			clauseset->nonempty = true;
			return;
		}
	}
}

/*
 * match_clause_to_indexcol()
 *	  判断一个限制条件是否可以用于某个索引列，如果可以则构造一个 IndexClause 节点。
 *
 *	  通常，操作符条件要满足以下要求才能用于索引：
 *	  (1) 必须是 (indexkey op const) 或 (const op indexkey) 形式；
 *	  (2) 操作符必须属于该索引列的操作符族；
 *	  (3) 如果相关，条件的排序规则必须与索引一致。
 *
 *	  这里对“const”的定义非常宽松：只要不包含索引表的 Var 或易变函数即可。
 *	  允许引用其他表的 Var，因为这种条件可以用于参数化索引扫描。
 *	  更高层代码负责区分限制条件和连接条件。
 *
 *	  注意：需要检查“const”侧是否包含索引表的 Var，否则像 (a.f1 OP (b.f2 OP a.f3))
 *	  这样的条件无法用于参数化索引扫描。
 *
 *	  目前执行器只能处理索引键在左侧的 indexqual，因此如果索引键在右侧，
 *	  必须能交换操作符并生成等价的 indexqual。
 *
 *	  如果索引有排序规则，条件也必须一致。对于无排序规则的索引，假定无所谓。
 *
 *	  还可以匹配 RowCompareExpr（目前仅支持 btree 索引）、ScalarArrayOpExpr（ANY 形式），
 *	  布尔索引可以直接匹配布尔表达式或 NOT 表达式。
 *
 *	  某些操作符和函数可以通过 planner 支持函数生成（通常是有损的）索引条件，
 *	  如果 OpExpr 或 FuncExpr 的某个参数匹配索引键且有支持函数，则调用支持函数尝试生成索引条件。
 *
 * 'rinfo'：待测试的条件（RestrictInfo 节点）
 * 'indexcol'：索引的列号（从 0 开始）
 * 'index'：目标索引
 *
 * 如果条件可用于该索引列，则返回 IndexClause，否则返回 NULL。
 *
 * 注意：如果条件是 OR 或 AND，直接返回 NULL，由更高层处理。
 */
static IndexClause *
match_clause_to_indexcol(PlannerInfo *root,
						 RestrictInfo *rinfo,
						 int indexcol,
						 IndexOptInfo *index)
{
	IndexClause *iclause;
	Expr	   *clause = rinfo->clause;
	Oid			opfamily;

	Assert(indexcol < index->nkeycolumns);

	/* 兼容历史，允许 NULL 条件，但直接返回 NULL */
	if (clause == NULL)
		return NULL;

	/*
	 * 优先处理布尔索引的特殊情况
	 * 如果当前索引列的操作符族（OpFamily）是布尔类型（BOOL_BTREE_FAM_OID 或 BOOL_HASH_FAM_OID），
	 * 那么优化器会尝试一种特殊的匹配方式：将单独的布尔列作为索引条件。
	 * 为什么需要特殊处理？
	 * 在 SQL 中，布尔类型的列可以直接作为 WHERE 条件，而不需要显式地写成 col = true。
	 *
	 * 对于布尔列 is_active，用户可以这样写：
	 * WHERE is_active = true （标准二元表达式）
	 * WHERE is_active （简写，隐式表示 is_active = true）
	 * WHERE NOT is_active （简写，隐式表示 is_active = false）
	 *
	 * 标准的 match_clause_to_index 逻辑主要处理第 1 种情况（二元操作符）。
	 * 而这段代码专门用来处理第 2 和第 3 种情况。
	 *
	 * 举例说明：
	 * 查询 1: SELECT * FROM users WHERE is_active = true;
	 * 这是一个标准的 OpExpr，可以被普通逻辑匹配。
	 * 因此不会经过这段特殊代码，而是直接调用 match_opclause 来处理。
	 *
	 * 查询 2: SELECT * FROM users WHERE active;
     * 这是一个裸的 Var 节点，不是 OpExpr。普通逻辑无法匹配（因为它找的是操作符）。
	 * 这段特殊代码介入：发现 active 是布尔索引列，且条件是单独的 active。于是它生成一个内部表示，相当于 active = true，从而利用索引。
	 *
	 * 查询 3: SELECT * FROM users WHERE NOT active;
     * 这是一个 BoolExpr (NOT)。普通逻辑无法匹配。
     * 这段特殊代码介入：发现是 NOT active，生成相当于 active = false 的索引条件。
	 */
	opfamily = index->opfamily[indexcol];
	if (IsBooleanOpfamily(opfamily))
	{
		iclause = match_boolean_index_clause(root, rinfo, indexcol, index);
		if (iclause)
			return iclause;
	}

	/*
	 * 尝试将给定的 WHERE 子句（clause）与指定的索引列（indexcol）进行匹配，
	 * 以判断该索引是否可以用于优化查询。
	 *
	 * 代码逻辑根据子句的表达式类型分别处理：
	 *
	 * 1. OpExpr (操作符表达式):
	 *    对应形如 "col = 1", "col < 10" 的普通操作符比较。
	 *    调用 match_opclause_to_indexcol 进行处理。
	 *
	 * 2. FuncExpr (函数表达式):
	 *    对应函数调用，通常用于处理函数索引或支持特定函数的索引操作符。
	 *    例如：对于函数索引 index(func(col))，查询条件为 "func(col) = 'val'"。
	 *    调用 match_funcclause_to_indexcol 进行处理。
	 *
	 * 3. ScalarArrayOpExpr (标量数组操作符表达式):
	 *    对应 "col = ANY(array)" (即 IN 查询) 或 "col = ALL(array)"。
	 *    例如："id IN (1, 2, 3)"。
	 *    调用 match_saopclause_to_indexcol 进行处理。
	 *
	 * 4. RowCompareExpr (行比较表达式):
	 *    对应行构造器的比较，如 "(a, b) > (1, 2)"。
	 *    这通常用于多列索引的匹配。
	 *    调用 match_rowcompare_to_indexcol 进行处理。
	 *
	 * 5. NullTest (空值测试):
	 *    对应 "col IS NULL" 或 "col IS NOT NULL"。
	 *    前提是索引访问方法（AM）必须支持空值搜索 (index->amsearchnulls 为真)。
	 *    如果匹配成功，直接构造并返回一个 IndexClause 节点，表示该索引可用于此空值测试。
	 *
	 * 只处理 OpExpr、FuncExpr、ScalarArrayOpExpr、RowCompareExpr，
	 * 或者索引支持 IS NULL/NOT NULL 时处理 NullTest。
	 */
	if (IsA(clause, OpExpr))
	{
		return match_opclause_to_indexcol(root, rinfo, indexcol, index);
	}
	else if (IsA(clause, FuncExpr))
	{
		return match_funcclause_to_indexcol(root, rinfo, indexcol, index);
	}
	else if (IsA(clause, ScalarArrayOpExpr))
	{
		return match_saopclause_to_indexcol(root, rinfo, indexcol, index);
	}
	else if (IsA(clause, RowCompareExpr))
	{
		return match_rowcompare_to_indexcol(root, rinfo, indexcol, index);
	}
	else if (index->amsearchnulls && IsA(clause, NullTest))
	{
		NullTest   *nt = (NullTest *) clause;

		if (!nt->argisrow &&
			match_index_to_operand((Node *) nt->arg, indexcol, index))
		{
			iclause = makeNode(IndexClause);
			iclause->rinfo = rinfo;
			iclause->indexquals = list_make1(rinfo);
			iclause->lossy = false;
			iclause->indexcol = indexcol;
			iclause->indexcols = NIL;
			return iclause;
		}
	}

	return NULL;
}

/*
 * match_boolean_index_clause
 *	  识别可以匹配布尔索引的限制子句。
 *
 * 这里的思路是，对于支持 BooleanEqualOperator 的布尔列索引，
 * 我们可以将对索引键的直接引用（例如 "WHERE col"）转换为 "indexkey = true"，
 * 或者将 "NOT indexkey"（例如 "WHERE NOT col"）转换为 "indexkey = false" 等，
 * 从而使表达式可以使用索引的 "=" 操作符进行索引扫描。
 *
 * 自 Postgres 8.1 起，必须这样做，因为常量简化（constant simplification）
 * 会执行反向转换（即把 "col = true" 简化为 "col"）；
 * 如果没有这段代码，根本无法使用此类索引。
 *
 * 此函数仅在 IsBooleanOpfamily() 识别出索引的操作符族时才应被调用。
 * 我们检查子句是否匹配索引的键，如果匹配，则构建一个合适的 IndexClause。
 */
static IndexClause *
match_boolean_index_clause(PlannerInfo *root,
						   RestrictInfo *rinfo,
						   int indexcol,
						   IndexOptInfo *index)
{
	Node	   *clause = (Node *) rinfo->clause;
	Expr	   *op = NULL;

	/*
	 * 直接匹配？
	 * 这里的 clause 本身就是一个 Var 节点（或者其他匹配索引键的表达式）。
	 *
	 * 代码处理形如 WHERE column_name 的查询条件（其中 column_name 是布尔类型），
	 * 将其换为 WHERE column_name = true 的形式，以便能够利用索引。
	 */
	if (match_index_to_operand(clause, indexcol, index))
	{
		/* 转换为 indexkey = TRUE */
		op = make_opclause(BooleanEqualOperator, BOOLOID, false,
						   (Expr *) clause,
						   (Expr *) makeBoolConst(true, false),
						   InvalidOid, InvalidOid);
	}
	/*
	 * NOT 子句？
	 * 处理形如 "WHERE NOT indexkey" 的情况。
	 */
	else if (is_notclause(clause))
	{
		Node	   *arg = (Node *) get_notclausearg((Expr *) clause);

		/* 检查 NOT 的参数是否匹配索引键 */
		if (match_index_to_operand(arg, indexcol, index))
		{
			/* 转换为 indexkey = FALSE */
			op = make_opclause(BooleanEqualOperator, BOOLOID, false,
							   (Expr *) arg,
							   (Expr *) makeBoolConst(false, false),
							   InvalidOid, InvalidOid);
		}
	}

	/*
	 * 处理 BooleanTest 节点（IS TRUE / IS FALSE）。
	 *
	 * 它将 col IS TRUE 转换为 col = TRUE，将 col IS FALSE 转换为 col = FALSE，以便利用索引。
	 * SQL 标准中，IS TRUE 和 = TRUE 在处理 NULL 值时是有区别的
	 * col = TRUE:
	 * 如果 col 是 TRUE -> 结果 TRUE
	 * 如果 col 是 FALSE -> 结果 FALSE
	 * 如果 col 是 NULL -> 结果 NULL (未知) 
	 * col IS TRUE:
	 * 如果 col 是 TRUE -> 结果 TRUE
	 * 如果 col 是 FALSE -> 结果 FALSE
	 * 如果 col 是 NULL -> 结果 FALSE (明确的假)
	 *
	 * 又有，在 WHERE 子句中，任何结果为 NULL 的行都会被过滤掉，
	 * 在 WHERE 子句中，只有当条件结果为 TRUE 时，行才会被返回。
	 * 如果结果是 FALSE，行被丢弃。
	 * 如果结果是 NULL，行也被丢弃。
	 */
	else if (clause && IsA(clause, BooleanTest))
	{
		BooleanTest *btest = (BooleanTest *) clause;
		Node	   *arg = (Node *) btest->arg;

		/* 处理 IS TRUE */
		if (btest->booltesttype == IS_TRUE &&
			match_index_to_operand(arg, indexcol, index))
		{
			/* 转换为 indexkey = TRUE */
			op = make_opclause(BooleanEqualOperator, BOOLOID, false,
							   (Expr *) arg,
							   (Expr *) makeBoolConst(true, false),
							   InvalidOid, InvalidOid);
		}
		/* 处理 IS FALSE */
		else if (btest->booltesttype == IS_FALSE &&
				 match_index_to_operand(arg, indexcol, index))
		{
			/* 转换为 indexkey = FALSE */
			op = make_opclause(BooleanEqualOperator, BOOLOID, false,
							   (Expr *) arg,
							   (Expr *) makeBoolConst(false, false),
							   InvalidOid, InvalidOid);
		}
	}

	/*
	 * 如果我们成功地从给定的限定条件（qual）生成了一个操作符子句，
	 * 我们必须将其包装在一个 IndexClause 中。
	 * 这种转换是精确的，不是有损（lossy）的。
	 */
	if (op)
	{
		IndexClause *iclause = makeNode(IndexClause);

		iclause->rinfo = rinfo;
		/*
		 * 注意：我们需要为新生成的 op 创建一个新的 RestrictInfo。
		 * make_simple_restrictinfo 会处理这个过程。
		 * 这里保存的是转换后的、用于实际索引扫描的条件。
		 */
		iclause->indexquals = list_make1(make_simple_restrictinfo(root, op));
		/*
		 * 标记这个转换是精确的（Exact）。
		 * 意思是：索引扫描返回的行，肯定百分之百满足查询条件，不需要再回表（Heap Fetch）进行二次检查（Recheck）。
		 * 有些索引（如 GIN 的某些操作符）是“有损”的（Lossy），索引说“可能有”，还需要回表确认。
		 * 但这里的布尔转换是精确的。
		 */
		iclause->lossy = false;
		iclause->indexcol = indexcol;
		/*
		 * 布尔索引条件总是单列的，因此 indexcols 设为 NIL。
		 * 这里处理行比较（RowCompareExpr）时会用到多列。
		 * 例如，(a, b) < (5, 10) 这样的条件会涉及多列索引。
		 * 在这种情况下，indexcols 会保存所有相关的索引列号。
		 */
		iclause->indexcols = NIL;
		return iclause;
	}

	return NULL;
}

/*
 * match_opclause_to_indexcol()
 *	  处理 match_clause_to_indexcol() 的 OpExpr 情况。
 *	  判断一个二元操作符表达式是否可以用于某个索引列，并构造 IndexClause。
 */
static IndexClause *
match_opclause_to_indexcol(PlannerInfo *root,
						   RestrictInfo *rinfo,
						   int indexcol,
						   IndexOptInfo *index)
{
	IndexClause *iclause;
	OpExpr	   	*clause = (OpExpr *) rinfo->clause;
	Node	   	*leftop, *rightop;
	Oid			expr_op;
	Oid			expr_coll;
	Index		index_relid;
	Oid			opfamily;
	Oid			idxcollation;

	/*
	 * 只处理二元操作符（如 a = b），一元操作符不考虑。
	 */
	if (list_length(clause->args) != 2)
		return NULL;

	/*
	 * 提取操作符的左右操作数。
	 */
	leftop = (Node*)linitial(clause->args);
	rightop = (Node*)lsecond(clause->args);
	/*
	 * 提取操作符的 OID 和输入排序规则。
	 */
	expr_op = clause->opno;
	expr_coll = clause->inputcollid;

	/* 提取索引的相关信息 */
	index_relid = index->rel->relid;
	opfamily = index->opfamily[indexcol];
	idxcollation = index->indexcollations[indexcol];

	/*
	 * 检查是否为 (indexkey operator 常量) 或 (常量 operator indexkey) 形式。
	 * 参见 match_clause_to_indexcol 的注释。
	 * 注意：只有一侧是索引列，另一侧不能包含本表的变量且不能有易变函数。
	 * 
	 * !bms_is_member(index_relid, rinfo->right_relids)
	 * 		index_relid: 当前索引所属表的 ID（例如表 t1 的 ID）。
	 * 		rinfo->right_relids: 查询条件右操作数（rightop）中引用的所有表的 ID 集合。
	 * 		bms_is_member: 检查 index_relid 是否在集合 right_relids 中。
	 * 		! (非): 确保它不在其中。
	 * 		翻译成人话：“确保条件的右边（比如 a = b 中的 b）没有引用当前这张表里的任何列。”
	 * 如果条件的右边也引用了同一张表，例如：SELECT * FROM t1 WHERE t1.a = t1.b;
	 * 为了能使用索引扫描（Index Scan），查询条件必须是形如：
	 * IndexKey OP Constant （索引列 操作符 常量/参数），所以这里拒绝。
	 *
	 * !contain_volatile_functions(rightop)
	 * 		contain_volatile_functions: 检查表达式中是否包含易变函数（Volatile Functions）。
	 * 		易变函数：每次调用可能返回不同结果的函数，例如 random() 或 timeofday()。
	 *
	 * PostgreSQL 将函数稳定性分为三类：
	 * 		Immutable（不可变）: 输入相同，输出永远相同。如 2 + 2。
	 * 		Stable（稳定）: 在同一个事务/查询内，输入相同，输出相同。如 now()。索引扫描允许使用 Stable 函数。
	 * 		Volatile（易变）: 每次调用结果都可能不同，或者有副作用。如 random()、nextval()。
	 * 索引扫描通常禁止使用 Volatile 函数作为键值。
	 */
	if (match_index_to_operand(leftop, indexcol, index) &&
		!bms_is_member(index_relid, rinfo->right_relids) &&
		!contain_volatile_functions(rightop))
	{
		/*
		 * 检查排序规则和操作符族是否匹配
		 *
		 * IndexCollMatchesExprColl(idxcollation, expr_coll)：
		 * 		确保查询要求的排序规则与索引存储的排序规则一致。
		 * 		如果索引是按 C 规则建立的，而查询是按 zh_CN 规则比较的，那么索引就不能用（因为顺序不一样）。
		 *
		 * op_in_opfamily(expr_op, opfamily):
		 * 		检查操作符 expr_op 是否属于操作符族 opfamily。
		 * 		如果操作符不在该族中，索引就不能用。
		 */
		if (IndexCollMatchesExprColl(idxcollation, expr_coll) &&
			op_in_opfamily(expr_op, opfamily))
		{
			iclause = makeNode(IndexClause);
			iclause->rinfo = rinfo;
			iclause->indexquals = list_make1(rinfo);
			iclause->lossy = false;
			iclause->indexcol = indexcol;
			iclause->indexcols = NIL;
			return iclause;
		}

		/*
		 * 如果操作符不在索引的操作符族中，尝试调用 planner 支持函数。
		 * 当标准的操作符匹配逻辑（直接查 pg_amop 系统表）失败，或者需要更复杂的转换时，
		 * PostgreSQL 12+ 引入了一种新的机制：Planner Support Function。
		 * 这段代码尝试调用与该操作符关联的支持函数，看看它能不能“变魔术”，把当前的查询条件转化为一个有效的索引扫描条件。
		 */
		set_opfuncid(clause);	/* 确保 opfuncid 被正确填充，用于后续的索引扫描条件构造 */
		return get_index_clause_from_support(root,
											 rinfo,
											 clause->opfuncid,
											 0, /* 索引列在左侧 */
											 indexcol,
											 index);
	}

	/* 试试交换操作数的情况 */
	if (match_index_to_operand(rightop, indexcol, index) &&
		!bms_is_member(index_relid, rinfo->left_relids) &&
		!contain_volatile_functions(leftop))
	{
		if (IndexCollMatchesExprColl(idxcollation, expr_coll))
		{
			Oid comm_op = get_commutator(expr_op);

			if (OidIsValid(comm_op) &&
				op_in_opfamily(comm_op, opfamily))
			{
				RestrictInfo *commrinfo;

				/* 构造交换后的 OpExpr 和 RestrictInfo */
				commrinfo = commute_restrictinfo(rinfo, comm_op);

				/* 构造 IndexClause，标记为派生条件 */
				iclause = makeNode(IndexClause);
				iclause->rinfo = rinfo;
				iclause->indexquals = list_make1(commrinfo);
				iclause->lossy = false;
				iclause->indexcol = indexcol;
				iclause->indexcols = NIL;
				return iclause;
			}
		}

		/*
		 * 如果操作符不在索引的操作符族中，尝试调用 planner 支持函数。
		 */
		set_opfuncid(clause);	/* 确保有 opfuncid */
		return get_index_clause_from_support(root,
											 rinfo,
											 clause->opfuncid,
											 1, /* 索引列在右侧 */
											 indexcol,
											 index);
	}

	return NULL;
}

/*
 * match_funcclause_to_indexcol()
 *	  Handles the FuncExpr case for match_clause_to_indexcol(),
 *	  which see for comments.
 */
static IndexClause *
match_funcclause_to_indexcol(PlannerInfo *root,
							 RestrictInfo *rinfo,
							 int indexcol,
							 IndexOptInfo *index)
{
	FuncExpr   *clause = (FuncExpr *) rinfo->clause;
	int			indexarg;
	ListCell   *lc;

	/*
	 * We have no built-in intelligence about function clauses, but if there's
	 * a planner support function, it might be able to do something.  But, to
	 * cut down on wasted planning cycles, only call the support function if
	 * at least one argument matches the target index column.
	 *
	 * Note that we don't insist on the other arguments being pseudoconstants;
	 * the support function has to check that.  This is to allow cases where
	 * only some of the other arguments need to be included in the indexqual.
	 */
	indexarg = 0;
	foreach(lc, clause->args)
	{
		Node	   *op = (Node *) lfirst(lc);

		if (match_index_to_operand(op, indexcol, index))
		{
			return get_index_clause_from_support(root,
												 rinfo,
												 clause->funcid,
												 indexarg,
												 indexcol,
												 index);
		}

		indexarg++;
	}

	return NULL;
}

/*
 * get_index_clause_from_support()
 *		If the function has a planner support function, try to construct
 *		an IndexClause using indexquals created by the support function.
 */
static IndexClause *
get_index_clause_from_support(PlannerInfo *root,
							  RestrictInfo *rinfo,
							  Oid funcid,
							  int indexarg,
							  int indexcol,
							  IndexOptInfo *index)
{
	Oid			prosupport = get_func_support(funcid);
	SupportRequestIndexCondition req;
	List	   *sresult;

	if (!OidIsValid(prosupport))
		return NULL;

	req.type = T_SupportRequestIndexCondition;
	req.root = root;
	req.funcid = funcid;
	req.node = (Node *) rinfo->clause;
	req.indexarg = indexarg;
	req.index = index;
	req.indexcol = indexcol;
	req.opfamily = index->opfamily[indexcol];
	req.indexcollation = index->indexcollations[indexcol];

	req.lossy = true;			/* default assumption */

	sresult = (List *)
		DatumGetPointer(OidFunctionCall1(prosupport,
										 PointerGetDatum(&req)));

	if (sresult != NIL)
	{
		IndexClause *iclause = makeNode(IndexClause);
		List	   *indexquals = NIL;
		ListCell   *lc;

		/*
		 * The support function API says it should just give back bare
		 * clauses, so here we must wrap each one in a RestrictInfo.
		 */
		foreach(lc, sresult)
		{
			Expr	   *clause = (Expr *) lfirst(lc);

			indexquals = lappend(indexquals,
								 make_simple_restrictinfo(root, clause));
		}

		iclause->rinfo = rinfo;
		iclause->indexquals = indexquals;
		iclause->lossy = req.lossy;
		iclause->indexcol = indexcol;
		iclause->indexcols = NIL;

		return iclause;
	}

	return NULL;
}

/*
 * match_saopclause_to_indexcol()
 *	  Handles the ScalarArrayOpExpr case for match_clause_to_indexcol(),
 *	  which see for comments.
 */
static IndexClause *
match_saopclause_to_indexcol(PlannerInfo *root,
							 RestrictInfo *rinfo,
							 int indexcol,
							 IndexOptInfo *index)
{
	ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) rinfo->clause;
	Node	   *leftop,
			   *rightop;
	Relids		right_relids;
	Oid			expr_op;
	Oid			expr_coll;
	Index		index_relid;
	Oid			opfamily;
	Oid			idxcollation;

	/* We only accept ANY clauses, not ALL */
	if (!saop->useOr)
		return NULL;
	leftop = (Node *) linitial(saop->args);
	rightop = (Node *) lsecond(saop->args);
	right_relids = pull_varnos(root, rightop);
	expr_op = saop->opno;
	expr_coll = saop->inputcollid;

	index_relid = index->rel->relid;
	opfamily = index->opfamily[indexcol];
	idxcollation = index->indexcollations[indexcol];

	/*
	 * We must have indexkey on the left and a pseudo-constant array argument.
	 */
	if (match_index_to_operand(leftop, indexcol, index) &&
		!bms_is_member(index_relid, right_relids) &&
		!contain_volatile_functions(rightop))
	{
		if (IndexCollMatchesExprColl(idxcollation, expr_coll) &&
			op_in_opfamily(expr_op, opfamily))
		{
			IndexClause *iclause = makeNode(IndexClause);

			iclause->rinfo = rinfo;
			iclause->indexquals = list_make1(rinfo);
			iclause->lossy = false;
			iclause->indexcol = indexcol;
			iclause->indexcols = NIL;
			return iclause;
		}

		/*
		 * We do not currently ask support functions about ScalarArrayOpExprs,
		 * though in principle we could.
		 */
	}

	return NULL;
}

/*
 * match_rowcompare_to_indexcol()
 *	  Handles the RowCompareExpr case for match_clause_to_indexcol(),
 *	  which see for comments.
 *
 * In this routine we check whether the first column of the row comparison
 * matches the target index column.  This is sufficient to guarantee that some
 * index condition can be constructed from the RowCompareExpr --- the rest
 * is handled by expand_indexqual_rowcompare().
 */
static IndexClause *
match_rowcompare_to_indexcol(PlannerInfo *root,
							 RestrictInfo *rinfo,
							 int indexcol,
							 IndexOptInfo *index)
{
	RowCompareExpr *clause = (RowCompareExpr *) rinfo->clause;
	Index		index_relid;
	Oid			opfamily;
	Oid			idxcollation;
	Node	   *leftop,
			   *rightop;
	bool		var_on_left;
	Oid			expr_op;
	Oid			expr_coll;

	/* Forget it if we're not dealing with a btree index */
	if (index->relam != BTREE_AM_OID)
		return NULL;

	index_relid = index->rel->relid;
	opfamily = index->opfamily[indexcol];
	idxcollation = index->indexcollations[indexcol];

	/*
	 * We could do the matching on the basis of insisting that the opfamily
	 * shown in the RowCompareExpr be the same as the index column's opfamily,
	 * but that could fail in the presence of reverse-sort opfamilies: it'd be
	 * a matter of chance whether RowCompareExpr had picked the forward or
	 * reverse-sort family.  So look only at the operator, and match if it is
	 * a member of the index's opfamily (after commutation, if the indexkey is
	 * on the right).  We'll worry later about whether any additional
	 * operators are matchable to the index.
	 */
	leftop = (Node *) linitial(clause->largs);
	rightop = (Node *) linitial(clause->rargs);
	expr_op = linitial_oid(clause->opnos);
	expr_coll = linitial_oid(clause->inputcollids);

	/* Collations must match, if relevant */
	if (!IndexCollMatchesExprColl(idxcollation, expr_coll))
		return NULL;

	/*
	 * These syntactic tests are the same as in match_opclause_to_indexcol()
	 */
	if (match_index_to_operand(leftop, indexcol, index) &&
		!bms_is_member(index_relid, pull_varnos(root, rightop)) &&
		!contain_volatile_functions(rightop))
	{
		/* OK, indexkey is on left */
		var_on_left = true;
	}
	else if (match_index_to_operand(rightop, indexcol, index) &&
			 !bms_is_member(index_relid, pull_varnos(root, leftop)) &&
			 !contain_volatile_functions(leftop))
	{
		/* indexkey is on right, so commute the operator */
		expr_op = get_commutator(expr_op);
		if (expr_op == InvalidOid)
			return NULL;
		var_on_left = false;
	}
	else
		return NULL;

	/* We're good if the operator is the right type of opfamily member */
	switch (get_op_opfamily_strategy(expr_op, opfamily))
	{
		case BTLessStrategyNumber:
		case BTLessEqualStrategyNumber:
		case BTGreaterEqualStrategyNumber:
		case BTGreaterStrategyNumber:
			return expand_indexqual_rowcompare(root,
											   rinfo,
											   indexcol,
											   index,
											   expr_op,
											   var_on_left);
	}

	return NULL;
}

/*
 * expand_indexqual_rowcompare --- expand a single indexqual condition
 *		that is a RowCompareExpr
 *
 * It's already known that the first column of the row comparison matches
 * the specified column of the index.  We can use additional columns of the
 * row comparison as index qualifications, so long as they match the index
 * in the "same direction", ie, the indexkeys are all on the same side of the
 * clause and the operators are all the same-type members of the opfamilies.
 *
 * If all the columns of the RowCompareExpr match in this way, we just use it
 * as-is, except for possibly commuting it to put the indexkeys on the left.
 *
 * Otherwise, we build a shortened RowCompareExpr (if more than one
 * column matches) or a simple OpExpr (if the first-column match is all
 * there is).  In these cases the modified clause is always "<=" or ">="
 * even when the original was "<" or ">" --- this is necessary to match all
 * the rows that could match the original.  (We are building a lossy version
 * of the row comparison when we do this, so we set lossy = true.)
 *
 * Note: this is really just the last half of match_rowcompare_to_indexcol,
 * but we split it out for comprehensibility.
 */
static IndexClause *
expand_indexqual_rowcompare(PlannerInfo *root,
							RestrictInfo *rinfo,
							int indexcol,
							IndexOptInfo *index,
							Oid expr_op,
							bool var_on_left)
{
	IndexClause *iclause = makeNode(IndexClause);
	RowCompareExpr *clause = (RowCompareExpr *) rinfo->clause;
	int			op_strategy;
	Oid			op_lefttype;
	Oid			op_righttype;
	int			matching_cols;
	List	   *expr_ops;
	List	   *opfamilies;
	List	   *lefttypes;
	List	   *righttypes;
	List	   *new_ops;
	List	   *var_args;
	List	   *non_var_args;
	ListCell   *vargs_cell;
	ListCell   *nargs_cell;
	ListCell   *opnos_cell;
	ListCell   *collids_cell;

	iclause->rinfo = rinfo;
	iclause->indexcol = indexcol;

	if (var_on_left)
	{
		var_args = clause->largs;
		non_var_args = clause->rargs;
	}
	else
	{
		var_args = clause->rargs;
		non_var_args = clause->largs;
	}

	get_op_opfamily_properties(expr_op, index->opfamily[indexcol], false,
							   &op_strategy,
							   &op_lefttype,
							   &op_righttype);

	/* Initialize returned list of which index columns are used */
	iclause->indexcols = list_make1_int(indexcol);

	/* Build lists of ops, opfamilies and operator datatypes in case needed */
	expr_ops = list_make1_oid(expr_op);
	opfamilies = list_make1_oid(index->opfamily[indexcol]);
	lefttypes = list_make1_oid(op_lefttype);
	righttypes = list_make1_oid(op_righttype);

	/*
	 * See how many of the remaining columns match some index column in the
	 * same way.  As in match_clause_to_indexcol(), the "other" side of any
	 * potential index condition is OK as long as it doesn't use Vars from the
	 * indexed relation.
	 */
	matching_cols = 1;
	vargs_cell = lnext(list_head(var_args));
	nargs_cell = lnext(list_head(non_var_args));
	opnos_cell = lnext(list_head(clause->opnos));
	collids_cell = lnext(list_head(clause->inputcollids));

	while (vargs_cell != NULL)
	{
		Node	   *varop = (Node *) lfirst(vargs_cell);
		Node	   *constop = (Node *) lfirst(nargs_cell);
		int			i;

		expr_op = lfirst_oid(opnos_cell);
		if (!var_on_left)
		{
			/* indexkey is on right, so commute the operator */
			expr_op = get_commutator(expr_op);
			if (expr_op == InvalidOid)
				break;			/* operator is not usable */
		}
		if (bms_is_member(index->rel->relid, pull_varnos(root, constop)))
			break;				/* no good, Var on wrong side */
		if (contain_volatile_functions(constop))
			break;				/* no good, volatile comparison value */

		/*
		 * The Var side can match any key column of the index.
		 */
		for (i = 0; i < index->nkeycolumns; i++)
		{
			if (match_index_to_operand(varop, i, index) &&
				get_op_opfamily_strategy(expr_op,
										 index->opfamily[i]) == op_strategy &&
				IndexCollMatchesExprColl(index->indexcollations[i],
										 lfirst_oid(collids_cell)))
				break;
		}
		if (i >= index->nkeycolumns)
			break;				/* no match found */

		/* Add column number to returned list */
		iclause->indexcols = lappend_int(iclause->indexcols, i);

		/* Add operator info to lists */
		get_op_opfamily_properties(expr_op, index->opfamily[i], false,
								   &op_strategy,
								   &op_lefttype,
								   &op_righttype);
		expr_ops = lappend_oid(expr_ops, expr_op);
		opfamilies = lappend_oid(opfamilies, index->opfamily[i]);
		lefttypes = lappend_oid(lefttypes, op_lefttype);
		righttypes = lappend_oid(righttypes, op_righttype);

		/* This column matches, keep scanning */
		matching_cols++;
		vargs_cell = lnext(vargs_cell);
		nargs_cell = lnext(nargs_cell);
		opnos_cell = lnext(opnos_cell);
		collids_cell = lnext(collids_cell);
	}

	/* Result is non-lossy if all columns are usable as index quals */
	iclause->lossy = (matching_cols != list_length(clause->opnos));

	/*
	 * We can use rinfo->clause as-is if we have var on left and it's all
	 * usable as index quals.
	 */
	if (var_on_left && !iclause->lossy)
		iclause->indexquals = list_make1(rinfo);
	else
	{
		/*
		 * We have to generate a modified rowcompare (possibly just one
		 * OpExpr).  The painful part of this is changing < to <= or > to >=,
		 * so deal with that first.
		 */
		if (!iclause->lossy)
		{
			/* very easy, just use the commuted operators */
			new_ops = expr_ops;
		}
		else if (op_strategy == BTLessEqualStrategyNumber ||
				 op_strategy == BTGreaterEqualStrategyNumber)
		{
			/* easy, just use the same (possibly commuted) operators */
			new_ops = list_truncate(expr_ops, matching_cols);
		}
		else
		{
			ListCell   *opfamilies_cell;
			ListCell   *lefttypes_cell;
			ListCell   *righttypes_cell;

			if (op_strategy == BTLessStrategyNumber)
				op_strategy = BTLessEqualStrategyNumber;
			else if (op_strategy == BTGreaterStrategyNumber)
				op_strategy = BTGreaterEqualStrategyNumber;
			else
				elog(ERROR, "unexpected strategy number %d", op_strategy);
			new_ops = NIL;
			forthree(opfamilies_cell, opfamilies,
					 lefttypes_cell, lefttypes,
					 righttypes_cell, righttypes)
			{
				Oid			opfam = lfirst_oid(opfamilies_cell);
				Oid			lefttype = lfirst_oid(lefttypes_cell);
				Oid			righttype = lfirst_oid(righttypes_cell);

				expr_op = get_opfamily_member(opfam, lefttype, righttype,
											  op_strategy);
				if (!OidIsValid(expr_op))	/* should not happen */
					elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
						 op_strategy, lefttype, righttype, opfam);
				new_ops = lappend_oid(new_ops, expr_op);
			}
		}

		/* If we have more than one matching col, create a subset rowcompare */
		if (matching_cols > 1)
		{
			RowCompareExpr *rc = makeNode(RowCompareExpr);

			rc->rctype = (RowCompareType) op_strategy;
			rc->opnos = new_ops;
			rc->opfamilies = list_truncate(list_copy(clause->opfamilies),
										   matching_cols);
			rc->inputcollids = list_truncate(list_copy(clause->inputcollids),
											 matching_cols);
			rc->largs = list_truncate(copyObject(var_args),
									  matching_cols);
			rc->rargs = list_truncate(copyObject(non_var_args),
									  matching_cols);
			iclause->indexquals = list_make1(make_simple_restrictinfo(root,
																	  (Expr *) rc));
		}
		else
		{
			Expr	   *op;

			/* We don't report an index column list in this case */
			iclause->indexcols = NIL;

			op = make_opclause(linitial_oid(new_ops), BOOLOID, false,
							   copyObject(linitial(var_args)),
							   copyObject(linitial(non_var_args)),
							   InvalidOid,
							   linitial_oid(clause->inputcollids));
			iclause->indexquals = list_make1(make_simple_restrictinfo(root, op));
		}
	}

	return iclause;
}


/****************************************************************************
 *				----  ROUTINES TO CHECK ORDERING OPERATORS	----
 ****************************************************************************/

/*
 * match_pathkeys_to_index
 *		Test whether an index can produce output ordered according to the
 *		given pathkeys using "ordering operators".
 *
 * If it can, return a list of suitable ORDER BY expressions, each of the form
 * "indexedcol operator pseudoconstant", along with an integer list of the
 * index column numbers (zero based) that each clause would be used with.
 * NIL lists are returned if the ordering is not achievable this way.
 *
 * On success, the result list is ordered by pathkeys, and in fact is
 * one-to-one with the requested pathkeys.
 */
static void
match_pathkeys_to_index(IndexOptInfo *index, List *pathkeys,
						List **orderby_clauses_p,
						List **clause_columns_p)
{
	List	   *orderby_clauses = NIL;
	List	   *clause_columns = NIL;
	ListCell   *lc1;

	*orderby_clauses_p = NIL;	/* set default results */
	*clause_columns_p = NIL;

	/* Only indexes with the amcanorderbyop property are interesting here */
	if (!index->amcanorderbyop)
		return;

	foreach(lc1, pathkeys)
	{
		PathKey    *pathkey = (PathKey *) lfirst(lc1);
		bool		found = false;
		ListCell   *lc2;

		/*
		 * Note: for any failure to match, we just return NIL immediately.
		 * There is no value in matching just some of the pathkeys.
		 */

		/* Pathkey must request default sort order for the target opfamily */
		if (pathkey->pk_strategy != BTLessStrategyNumber ||
			pathkey->pk_nulls_first)
			return;

		/* If eclass is volatile, no hope of using an indexscan */
		if (pathkey->pk_eclass->ec_has_volatile)
			return;

		/*
		 * Try to match eclass member expression(s) to index.  Note that child
		 * EC members are considered, but only when they belong to the target
		 * relation.  (Unlike regular members, the same expression could be a
		 * child member of more than one EC.  Therefore, the same index could
		 * be considered to match more than one pathkey list, which is OK
		 * here.  See also get_eclass_for_sort_expr.)
		 */
		foreach(lc2, pathkey->pk_eclass->ec_members)
		{
			EquivalenceMember *member = (EquivalenceMember *) lfirst(lc2);
			int			indexcol;

			/* No possibility of match if it references other relations */
			if (!bms_equal(member->em_relids, index->rel->relids))
				continue;

			/*
			 * We allow any column of the index to match each pathkey; they
			 * don't have to match left-to-right as you might expect.  This is
			 * correct for GiST, and it doesn't matter for SP-GiST because
			 * that doesn't handle multiple columns anyway, and no other
			 * existing AMs support amcanorderbyop.  We might need different
			 * logic in future for other implementations.
			 */
			for (indexcol = 0; indexcol < index->nkeycolumns; indexcol++)
			{
				Expr	   *expr;

				expr = match_clause_to_ordering_op(index,
												   indexcol,
												   member->em_expr,
												   pathkey->pk_opfamily);
				if (expr)
				{
					orderby_clauses = lappend(orderby_clauses, expr);
					clause_columns = lappend_int(clause_columns, indexcol);
					found = true;
					break;
				}
			}

			if (found)			/* don't want to look at remaining members */
				break;
		}

		if (!found)				/* fail if no match for this pathkey */
			return;
	}

	*orderby_clauses_p = orderby_clauses;	/* success! */
	*clause_columns_p = clause_columns;
}

/*
 * match_clause_to_ordering_op
 *	  Determines whether an ordering operator expression matches an
 *	  index column.
 *
 *	  This is similar to, but simpler than, match_clause_to_indexcol.
 *	  We only care about simple OpExpr cases.  The input is a bare
 *	  expression that is being ordered by, which must be of the form
 *	  (indexkey op const) or (const op indexkey) where op is an ordering
 *	  operator for the column's opfamily.
 *
 * 'index' is the index of interest.
 * 'indexcol' is a column number of 'index' (counting from 0).
 * 'clause' is the ordering expression to be tested.
 * 'pk_opfamily' is the btree opfamily describing the required sort order.
 *
 * Note that we currently do not consider the collation of the ordering
 * operator's result.  In practical cases the result type will be numeric
 * and thus have no collation, and it's not very clear what to match to
 * if it did have a collation.  The index's collation should match the
 * ordering operator's input collation, not its result.
 *
 * If successful, return 'clause' as-is if the indexkey is on the left,
 * otherwise a commuted copy of 'clause'.  If no match, return NULL.
 */
static Expr *
match_clause_to_ordering_op(IndexOptInfo *index,
							int indexcol,
							Expr *clause,
							Oid pk_opfamily)
{
	Oid			opfamily;
	Oid			idxcollation;
	Node	   *leftop,
			   *rightop;
	Oid			expr_op;
	Oid			expr_coll;
	Oid			sortfamily;
	bool		commuted;

	Assert(indexcol < index->nkeycolumns);

	opfamily = index->opfamily[indexcol];
	idxcollation = index->indexcollations[indexcol];

	/*
	 * Clause must be a binary opclause.
	 */
	if (!is_opclause(clause))
		return NULL;
	leftop = get_leftop(clause);
	rightop = get_rightop(clause);
	if (!leftop || !rightop)
		return NULL;
	expr_op = ((OpExpr *) clause)->opno;
	expr_coll = ((OpExpr *) clause)->inputcollid;

	/*
	 * We can forget the whole thing right away if wrong collation.
	 */
	if (!IndexCollMatchesExprColl(idxcollation, expr_coll))
		return NULL;

	/*
	 * Check for clauses of the form: (indexkey operator constant) or
	 * (constant operator indexkey).
	 */
	if (match_index_to_operand(leftop, indexcol, index) &&
		!contain_var_clause(rightop) &&
		!contain_volatile_functions(rightop))
	{
		commuted = false;
	}
	else if (match_index_to_operand(rightop, indexcol, index) &&
			 !contain_var_clause(leftop) &&
			 !contain_volatile_functions(leftop))
	{
		/* Might match, but we need a commuted operator */
		expr_op = get_commutator(expr_op);
		if (expr_op == InvalidOid)
			return NULL;
		commuted = true;
	}
	else
		return NULL;

	/*
	 * Is the (commuted) operator an ordering operator for the opfamily? And
	 * if so, does it yield the right sorting semantics?
	 */
	sortfamily = get_op_opfamily_sortfamily(expr_op, opfamily);
	if (sortfamily != pk_opfamily)
		return NULL;

	/* We have a match.  Return clause or a commuted version thereof. */
	if (commuted)
	{
		OpExpr	   *newclause = makeNode(OpExpr);

		/* flat-copy all the fields of clause */
		memcpy(newclause, clause, sizeof(OpExpr));

		/* commute it */
		newclause->opno = expr_op;
		newclause->opfuncid = InvalidOid;
		newclause->args = list_make2(rightop, leftop);

		clause = (Expr *) newclause;
	}

	return clause;
}


/****************************************************************************
 *				----  ROUTINES TO DO PARTIAL INDEX PREDICATE TESTS	----
 ****************************************************************************/

/*
 * check_index_predicates
 *    为指定关系的每个索引设置由谓词派生的IndexOptInfo字段
 *
 * 函数功能：
 *    该函数检查表上的每个索引，特别是部分索引（partial index），确定其谓词条件是否被当前查询满足。
 *    它设置两个关键字段：
 *    - predOK：如果索引是部分索引且其谓词条件被查询的WHERE子句隐含，则设为true
 *    - indrestrictinfo：关系的baserestrictinfo列表减去那些被索引谓词隐含的条件
 *
 * 参数说明：
 *    root - 规划器的全局信息结构，包含查询的所有规划信息
 *    rel - 要检查索引的关系优化信息结构
 */
void
check_index_predicates(PlannerInfo *root, RelOptInfo *rel)
{
    List	   *clauselist;        /* 用于证明索引可用性的条件列表 */
    bool		have_partial;      /* 标记是否存在部分索引 */
    bool		is_target_rel;     /* 标记关系是否为更新目标 */
    Relids		otherrels;         /* 除当前关系外的其他关系ID集合 */
    ListCell   *lc;               /* 循环列表的指针 */

    /* 断言：索引仅适用于基本关系或"其他"成员关系 */
    Assert(IS_SIMPLE_REL(rel));

    /*
     * 初始化阶段：
     * 1. 将每个索引的indrestrictinfo初始化为与baserestrictinfo相同
     * 2. 检查是否存在任何部分索引
     * 3. 如果没有部分索引，直接返回（无需进一步处理）
     */
    have_partial = false;
    foreach(lc, rel->indexlist)
    {
        IndexOptInfo *index = (IndexOptInfo *) lfirst(lc);

        /* 默认情况下，索引限制条件与关系的基本限制条件相同 */
        index->indrestrictinfo = rel->baserestrictinfo;
        /* 检测是否存在部分索引（indpred不为空） */
        if (index->indpred)
            have_partial = true;
    }
    /* 如果没有部分索引，不需要进一步处理 */
    if (!have_partial)
        return;

    /*
     * 构建可用条件列表：
     * 1. 首先复制关系的基本限制条件
     * 2. 添加可以"移动到"当前关系的连接条件
     * 3. 添加任何可以通过等价类推导出来的连接条件
     */
    clauselist = list_copy(rel->baserestrictinfo);

    /* 扫描关系的连接条件，添加可移动的条件 */
    foreach(lc, rel->joininfo)
    {
        RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

        /* 检查连接条件是否可以移动到当前关系 */
        if (!join_clause_is_movable_to(rinfo, rel))
            continue;

        /* 添加可移动的连接条件到条件列表 */
        clauselist = lappend(clauselist, rinfo);
    }

    /*
     * 添加通过等价类推导出来的连接条件
     * 计算正确的relid集合需要考虑特殊情况：当前关系可能是子关系而非真正的基本关系
     * 这种情况下需要从all_baserels中移除其父关系的relid
     */
    if (rel->reloptkind == RELOPT_OTHER_MEMBER_REL)
        otherrels = bms_difference(root->all_baserels,
                                   find_childrel_parents(root, rel));
    else
        otherrels = bms_difference(root->all_baserels, rel->relids);

    /* 如果存在其他关系，生成并添加推导的连接条件 */
    if (!bms_is_empty(otherrels))
        clauselist =
            list_concat(clauselist,
                        generate_join_implied_equalities(root,
                                                         bms_union(rel->relids,
                                                                   otherrels),
                                                         otherrels,
                                                         rel));

    /*
     * 确定关系是否为更新目标（UPDATE/DELETE/SELECT FOR UPDATE）
     * 对于更新目标关系，不能从indrestrictinfo中移除由索引谓词隐含的条件，
     * 因为这些条件需要在EvalPlanQual测试中被重新检查
     */
    is_target_rel = (rel->relid == root->parse->resultRelation ||
                     get_plan_rowmark(root->rowMarks, rel->relid) != NULL);

    /*
     * 处理阶段：
     * 1. 尝试证明每个部分索引的谓词为真
     * 2. 为部分索引计算indrestrictinfo列表
     * 注意：即使对于非predOK的索引，我们也要计算indrestrictinfo，
     * 因为这些索引可能在OR子句中使用（参见generate_bitmap_or_paths）
     */
    foreach(lc, rel->indexlist)
    {
        IndexOptInfo *index = (IndexOptInfo *) lfirst(lc);
        ListCell   *lcr;

        /* 跳过非部分索引（没有indpred的索引） */
        if (index->indpred == NIL)
            continue;

        /* 如果尚未证明索引谓词适用，则进行检查 */
        if (!index->predOK)
            index->predOK = predicate_implied_by(index->indpred, clauselist,
                                                 false);

        /* 如果关系是更新目标，保持indrestrictinfo不变 */
        if (is_target_rel)
            continue;

        /* 否则，计算indrestrictinfo为不被索引谓词隐含的条件 */
        index->indrestrictinfo = NIL;
        foreach(lcr, rel->baserestrictinfo)
        {
            RestrictInfo *rinfo = (RestrictInfo *) lfirst(lcr);

            /*
             * predicate_implied_by()假设第一个参数是不可变的
             * 因此，如果条件中包含可变函数，或者条件不被索引谓词隐含，
             * 则保留该条件在indrestrictinfo中
             */
            if (contain_mutable_functions((Node *) rinfo->clause) ||
                !predicate_implied_by(list_make1(rinfo->clause),
                                      index->indpred, false))
                index->indrestrictinfo = lappend(index->indrestrictinfo, rinfo);
        }
    }
}


/****************************************************************************
 *				----  ROUTINES TO CHECK EXTERNALLY-VISIBLE CONDITIONS  ----
 ****************************************************************************/

/*
 * ec_member_matches_indexcol
 *	  Test whether an EquivalenceClass member matches an index column.
 *
 * This is a callback for use by generate_implied_equalities_for_column.
 */
static bool
ec_member_matches_indexcol(PlannerInfo *root, RelOptInfo *rel,
						   EquivalenceClass *ec, EquivalenceMember *em,
						   void *arg)
{
	IndexOptInfo *index = ((ec_member_matches_arg *) arg)->index;
	int			indexcol = ((ec_member_matches_arg *) arg)->indexcol;
	Oid			curFamily;
	Oid			curCollation;

	Assert(indexcol < index->nkeycolumns);

	curFamily = index->opfamily[indexcol];
	curCollation = index->indexcollations[indexcol];

	/*
	 * If it's a btree index, we can reject it if its opfamily isn't
	 * compatible with the EC, since no clause generated from the EC could be
	 * used with the index.  For non-btree indexes, we can't easily tell
	 * whether clauses generated from the EC could be used with the index, so
	 * don't check the opfamily.  This might mean we return "true" for a
	 * useless EC, so we have to recheck the results of
	 * generate_implied_equalities_for_column; see
	 * match_eclass_clauses_to_index.
	 */
	if (index->relam == BTREE_AM_OID &&
		!list_member_oid(ec->ec_opfamilies, curFamily))
		return false;

	/* We insist on collation match for all index types, though */
	if (!IndexCollMatchesExprColl(curCollation, ec->ec_collation))
		return false;

	return match_index_to_operand((Node *) em->em_expr, indexcol, index);
}

/*
 * relation_has_unique_index_for
 *	  判断给定关系是否可以保证满足一组相等条件的记录最多只有一条，
 *	  这种保证源于这些条件约束了某个唯一索引的所有列。
 *
 * 参数：
 *   root - 查询规划器的全局信息结构
 *   rel - 要检查的关系（表或视图）
 *   restrictlist - RestrictInfo节点列表，表示连接条件中的等式约束
 *   exprlist - 当前关系中的表达式列表（用于索引键匹配）
 *   oprlist - 对应exprlist的相等操作符OID列表
 *
 * 返回值：
 *   如果存在唯一索引覆盖了所有条件对应的列，则返回true；否则返回false
 *
 * 条件表示方式：
 *   1. RestrictInfo节点列表：每个节点是一个mergejoinable的等式，一边是当前关系的表达式，
 *      另一边是不涉及当前关系的表达式。outer_is_left标志用于标识我们应该查看哪一边。
 *   2. 表达式列表和操作符列表：每个表达式对应当前关系中的一个列，操作符必须表示相等关系。
 *
 * 注意：
 *   - 函数会自动将关系的baserestrictinfo子句中可用的var = const条件添加到restrictlist
 *   - 传递的restrictlist会被函数以破坏性方式修改
 */
bool
relation_has_unique_index_for(PlannerInfo *root, RelOptInfo *rel,
				  List *restrictlist,
				  List *exprlist, List *oprlist)
{
	ListCell   *ic;  /* 循环遍历指针 */

	/* 断言：表达式列表和操作符列表长度必须相等 */
	Assert(list_length(exprlist) == list_length(oprlist));

	/* 快速路径：如果关系没有索引，直接返回false */
	if (rel->indexlist == NIL)
		return false;

	/*
	 * 检查关系的baserestrictinfo子句，找出可以添加到restrictlist的var = const子句
	 * 这一步会将基础限制条件（非连接条件的约束）合并到检查中
	 */
	foreach(ic, rel->baserestrictinfo)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(ic);

		/*
		 * 注意：对于限制子句，can_join不会被设置，但如果它有mergejoinable操作符
		 * 并且不包含volatile函数，mergeopfamilies会被设置
		 */
		if (restrictinfo->mergeopfamilies == NIL)
			continue;  /* 不是mergejoinable的等式，跳过 */

		/*
		 * 该子句肯定只引用了给定的关系。如果任何一边是伪常量，我们就可以使用它。
		 * 伪常量指的是不依赖于当前查询中任何表的表达式（如常量或参数）
		 */
		if (bms_is_empty(restrictinfo->left_relids))
		{
			/* 右手边是内部的（属于当前关系） */
			restrictinfo->outer_is_left = true;
		}
		else if (bms_is_empty(restrictinfo->right_relids))
		{
			/* 左手边是内部的（属于当前关系） */
			restrictinfo->outer_is_left = false;
		}
		else
			continue;  /* 两边都不是伪常量，跳过 */

		/* OK，添加到列表中 */
		restrictlist = lappend(restrictlist, restrictinfo);
	}

	/* 快速路径：如果没有任何限制条件，直接返回false */
	if (restrictlist == NIL && exprlist == NIL)
		return false;

	/* 检查关系的每个索引... */
	foreach(ic, rel->indexlist)
	{
		IndexOptInfo *ind = (IndexOptInfo *) lfirst(ic);
		int		c;  /* 索引列计数器 */

		/*
		 * 如果索引不是唯一的，或者不是立即强制的，或者是部分索引，那么它在这里没用。
		 * 我们无法使用predOK的部分唯一索引，因为check_index_predicates()也会使用连接谓词
		 * 来确定部分索引是否可用。在这里我们需要的是在任何连接评估之前就成立的证明。
		 */
		if (!ind->unique || !ind->immediate || ind->indpred != NIL)
			continue;

		/*
		 * 尝试在条件列表中找到每个索引列的匹配。这是O(N^2)或更差的复杂度，
		 * 但我们期望所有列表都很短，所以性能不是问题。
		 */
		for (c = 0; c < ind->nkeycolumns; c++)
		{
			bool		matched = false;  /* 当前索引列是否找到匹配的条件 */
			ListCell   *lc;
			ListCell   *lc2;

			/* 首先尝试在restrictlist中查找匹配 */
			foreach(lc, restrictlist)
			{
				RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
				Node	   *rexpr;  /* 关系侧的表达式 */

				/*
				 * 条件的相等操作符必须是索引操作符族的成员，否则它不能保证与索引相同的相等语义。
				 * 我们首先检查这一点，因为它可能比match_index_to_operand()更便宜。
				 */
				if (!list_member_oid(rinfo->mergeopfamilies, ind->opfamily[c]))
					continue;

				/*
				 * XXX 将来可能需要在这里检查排序规则。目前我们假设所有排序规则都归结为相同的相等概念。
				 */

				/* OK，检查条件操作数是否与索引键匹配 */
				if (rinfo->outer_is_left)
					rexpr = get_rightop(rinfo->clause);  /* 获取关系侧表达式 */
				else
					rexpr = get_leftop(rinfo->clause);

				if (match_index_to_operand(rexpr, c, ind))
				{
					matched = true;  /* 列已被唯一约束 */
					break;
				}
			}

			if (matched)
				continue;  /* 继续检查下一个索引列 */

			/* 如果在restrictlist中没找到匹配，则在exprlist和oprlist中查找 */
			forboth(lc, exprlist, lc2, oprlist)
			{
				Node	   *expr = (Node *) lfirst(lc);  /* 关系中的表达式 */
				Oid		opr = lfirst_oid(lc2);  /* 操作符OID */

				/* 检查表达式是否与索引键匹配 */
				if (!match_index_to_operand(expr, c, ind))
					continue;

				/*
				 * 相等操作符必须是索引操作符族的成员，否则它不能保证与索引相同的相等语义。
				 * 我们假设调用者已经确定它是一个相等操作符，所以不需要更严格的检查。
				 */
				if (!op_in_opfamily(opr, ind->opfamily[c]))
					continue;

				/*
				 * XXX 将来可能需要在这里检查排序规则。目前我们假设所有排序规则都归结为相同的相等概念。
				 */

				matched = true;  /* 列已被唯一约束 */
				break;
			}

			if (!matched)
				break;  /* 未找到匹配；此索引对我们没有帮助 */
		}

		/* 是否匹配了此索引的所有键列？ */
		if (c == ind->nkeycolumns)
			return true;  /* 找到满足条件的唯一索引 */
	}

	/* 没有找到满足所有条件的唯一索引 */
	return false;
}

/*
 * indexcol_is_bool_constant_for_query
 *
 * If an index column is constrained to have a constant value by the query's
 * WHERE conditions, then it's irrelevant for sort-order considerations.
 * Usually that means we have a restriction clause WHERE indexcol = constant,
 * which gets turned into an EquivalenceClass containing a constant, which
 * is recognized as redundant by build_index_pathkeys().  But if the index
 * column is a boolean variable (or expression), then we are not going to
 * see WHERE indexcol = constant, because expression preprocessing will have
 * simplified that to "WHERE indexcol" or "WHERE NOT indexcol".  So we are not
 * going to have a matching EquivalenceClass (unless the query also contains
 * "ORDER BY indexcol").  To allow such cases to work the same as they would
 * for non-boolean values, this function is provided to detect whether the
 * specified index column matches a boolean restriction clause.
 */
bool
indexcol_is_bool_constant_for_query(PlannerInfo *root,
									IndexOptInfo *index,
									int indexcol)
{
	ListCell   *lc;

	/* If the index isn't boolean, we can't possibly get a match */
	if (!IsBooleanOpfamily(index->opfamily[indexcol]))
		return false;

	/* Check each restriction clause for the index's rel */
	foreach(lc, index->rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		/*
		 * As in match_clause_to_indexcol, never match pseudoconstants to
		 * indexes.  (It might be semantically okay to do so here, but the
		 * odds of getting a match are negligible, so don't waste the cycles.)
		 */
		if (rinfo->pseudoconstant)
			continue;

		/* See if we can match the clause's expression to the index column */
		if (match_boolean_index_clause(root, rinfo, indexcol, index))
			return true;
	}

	return false;
}


/****************************************************************************
 *				----  ROUTINES TO CHECK OPERANDS  ----
 ****************************************************************************/

/*
 * match_index_to_operand()
 *	  它的作用是判断：给定的表达式（operand）是否就是索引的第 indexcol 列？
 *
 * operand: 要与索引进行比较的节点树
 * indexcol: 索引的列号（从0开始计数）
 * index: 感兴趣的索引
 *
 * 注意，这里我们不关心排序规则（collation）；如果涉及对排序规则敏感的操作符，
 * 调用者必须自行检查排序规则是否匹配。
 *
 * 此函数导出供 selfuncs.c 使用。
 */
bool
match_index_to_operand(Node *operand,
					   int indexcol,
					   IndexOptInfo *index)
{
	int			indkey;

	/*
	 * 预处理：剥离 RelabelType 节点
	 * 这是为了在二进制兼容操作符的情况下能够应用索引扫描所必需的。
	 * 注意：我们可以假设最多只有一个 RelabelType 节点；
	 * 如果有多个，eval_const_expressions() 应该已经进行了简化。
	 *
	 * 它是 PostgreSQL 内部的一种节点，表示“二进制兼容的类型转换”。
	 * 比如 varchar 到 text 的转换，底层存储是一样的，不需要重新计算，只需要改个标签
	 * 如果用户写 WHERE my_varchar_col::text = 'abc'，
	 * 优化器看到的 operand 是一个 RelabelType 节点。但索引是建立在 my_varchar_col 上的。
	 * 为了匹配成功，我们需要透过这层“马甲”，看到里面的本质——即那个 Var 节点。
	 */
	if (operand && IsA(operand, RelabelType))
		operand = (Node *) ((RelabelType *) operand)->arg;

	/*
	 * 索引键（indexkeys）数组记录了索引的每一列对应的列号（attnum）。
	 * 如果索引列是表达式列（indkey == 0），则数组中对应位置的值为0。
	 */
	indkey = index->indexkeys[indexcol];
	if (indkey != 0)
	{
		/*
		 * 检查 operand 是不是一个变量 (Var)。
		 * 检查这个变量是不是属于当前表 (varno 匹配)。
		 * 检查这个变量的列号 (varattno) 是不是等于索引定义的列号 (indkey)。
		 * 如果都对，说明这个操作数就是索引列。
		 */
		if (operand && IsA(operand, Var) &&
			index->rel->relid == ((Var *) operand)->varno &&
			indkey == ((Var *) operand)->varattno)
			return true;
	}
	else
	{
		/*
		 * 索引表达式；找到正确的表达式。
		 * （可以通过让调用者传递表达式来避免这种搜索，但这会使所有调用者的逻辑变复杂；
		 * 似乎不值得这样做。）
		 */
		ListCell   *indexpr_item;
		int			i;
		Node	   *indexkey;

		/*
		 * 找到对应的表达式列。
		 * indexkeys 数组中存 0 表示这一列是表达式。真正的表达式存在 index->indexprs 列表中。
		 * 在 index->indexprs 列表中找到第 indexcol 列对应的那个表达式。
		 * 这意味着 indexprs 的长度通常小于或等于 indexkeys 的长度。
		 * 它里面的元素顺序，对应着 indexkeys 中出现 0 的顺序。
		 *
		 * 示例：CREATE INDEX idx ON t1 (col1, lower(col2), col3, col4 + 1);
		 * indexkeys = [1, 0, 3, 0]
		 * indexprs = [lower(col2), col4 + 1]
		 */
		indexpr_item = list_head(index->indexprs);
		for (i = 0; i < indexcol; i++)
		{
			if (index->indexkeys[i] == 0)
			{
				if (indexpr_item == NULL)
					elog(ERROR, "wrong number of index expressions");
				indexpr_item = lnext(indexpr_item);
			}
		}
		if (indexpr_item == NULL)
			elog(ERROR, "wrong number of index expressions");

		/*
		 * 从列表中取出当前表达式列。
		 */
		indexkey = (Node*)lfirst(indexpr_item);

		/*
		 * 它与操作数匹配吗？同样，剥离任何 RelabelType。
		 * 这样可以确保即使表达式上有类型转换标签，也能正确匹配。
		 */
		if (indexkey && IsA(indexkey, RelabelType))
			indexkey = (Node *) ((RelabelType *) indexkey)->arg;

		/*
		 * 深度比较两个节点树是否完全相同。
		 * 如果相同，说明 operand 就是索引表达式列。
		 * 例如建立表达式索引：CREATE INDEX idx ON t1 ( (col1 + 1) );
		 * 如果 operand 是 (col1 + 1)，那么就匹配成功。
		 */
		if (equal(indexkey, operand))
			return true;
	}

	return false;
}

/*
 * is_pseudo_constant_for_index()
 *	  Test whether the given expression can be used as an indexscan
 *	  comparison value.
 *
 * An indexscan comparison value must not contain any volatile functions,
 * and it can't contain any Vars of the index's own table.  Vars of
 * other tables are okay, though; in that case we'd be producing an
 * indexqual usable in a parameterized indexscan.  This is, therefore,
 * a weaker condition than is_pseudo_constant_clause().
 *
 * This function is exported for use by planner support functions,
 * which will have available the IndexOptInfo, but not any RestrictInfo
 * infrastructure.  It is making the same test made by functions above
 * such as match_opclause_to_indexcol(), but those rely where possible
 * on RestrictInfo information about variable membership.
 *
 * expr: the nodetree to be checked
 * index: the index of interest
 */
bool
is_pseudo_constant_for_index(Node *expr, IndexOptInfo *index)
{
	return is_pseudo_constant_for_index_new(NULL, expr, index);
}

bool
is_pseudo_constant_for_index_new(PlannerInfo *root, Node *expr, IndexOptInfo *index)
{
	/* pull_varnos is cheaper than volatility check, so do that first */
	if (bms_is_member(index->rel->relid, pull_varnos(root, expr)))
		return false;			/* no good, contains Var of table */
	if (contain_volatile_functions(expr))
		return false;			/* no good, volatile comparison value */
	return true;
}
