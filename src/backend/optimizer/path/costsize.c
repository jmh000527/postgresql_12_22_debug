/*-------------------------------------------------------------------------
 *
 * costsize.c
 *	  Routines to compute (and set) relation sizes and path costs
 *
 * Path costs are measured in arbitrary units established by these basic
 * parameters:
 *
 *	seq_page_cost		Cost of a sequential page fetch
 *	random_page_cost	Cost of a non-sequential page fetch
 *	cpu_tuple_cost		Cost of typical CPU time to process a tuple
 *	cpu_index_tuple_cost  Cost of typical CPU time to process an index tuple
 *	cpu_operator_cost	Cost of CPU time to execute an operator or function
 *	parallel_tuple_cost Cost of CPU time to pass a tuple from worker to master backend
 *	parallel_setup_cost Cost of setting up shared memory for parallelism
 *
 * We expect that the kernel will typically do some amount of read-ahead
 * optimization; this in conjunction with seek costs means that seq_page_cost
 * is normally considerably less than random_page_cost.  (However, if the
 * database is fully cached in RAM, it is reasonable to set them equal.)
 *
 * We also use a rough estimate "effective_cache_size" of the number of
 * disk pages in Postgres + OS-level disk cache.  (We can't simply use
 * NBuffers for this purpose because that would ignore the effects of
 * the kernel's disk cache.)
 *
 * Obviously, taking constants for these values is an oversimplification,
 * but it's tough enough to get any useful estimates even at this level of
 * detail.  Note that all of these parameters are user-settable, in case
 * the default values are drastically off for a particular platform.
 *
 * seq_page_cost and random_page_cost can also be overridden for an individual
 * tablespace, in case some data is on a fast disk and other data is on a slow
 * disk.  Per-tablespace overrides never apply to temporary work files such as
 * an external sort or a materialize node that overflows work_mem.
 *
 * We compute two separate costs for each path:
 *		total_cost: total estimated cost to fetch all tuples
 *		startup_cost: cost that is expended before first tuple is fetched
 * In some scenarios, such as when there is a LIMIT or we are implementing
 * an EXISTS(...) sub-select, it is not necessary to fetch all tuples of the
 * path's result.  A caller can estimate the cost of fetching a partial
 * result by interpolating between startup_cost and total_cost.  In detail:
 *		actual_cost = startup_cost +
 *			(total_cost - startup_cost) * tuples_to_fetch / path->rows;
 * Note that a base relation's rows count (and, by extension, plan_rows for
 * plan nodes below the LIMIT node) are set without regard to any LIMIT, so
 * that this equation works properly.  (Note: while path->rows is never zero
 * for ordinary relations, it is zero for paths for provably-empty relations,
 * so beware of division-by-zero.)	The LIMIT is applied as a top-level
 * plan node.
 *
 * For largely historical reasons, most of the routines in this module use
 * the passed result Path only to store their results (rows, startup_cost and
 * total_cost) into.  All the input data they need is passed as separate
 * parameters, even though much of it could be extracted from the Path.
 * An exception is made for the cost_XXXjoin() routines, which expect all
 * the other fields of the passed XXXPath to be filled in, and similarly
 * cost_index() assumes the passed IndexPath is valid except for its output
 * values.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/costsize.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <math.h>

#include "access/amapi.h"
#include "access/htup_details.h"
#include "access/tsmapi.h"
#include "executor/executor.h"
#include "executor/nodeHash.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/placeholder.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/spccache.h"
#include "utils/tuplesort.h"

/*
 * 计算以2为底的对数的宏定义
 * 使用自然对数计算：log2(x) = ln(x) / ln(2)，其中0.693147180559945是ln(2)的近似值
 * 该宏用于查询优化器中的各种计算，如估算基数时的统计信息处理
 */
#define LOG2(x)  (log(x) / 0.693147180559945)

/*
 * Append和MergeAppend节点的成本估算系数
 * 这些节点的处理成本低于其他使用cpu_tuple_cost的操作
 * 为避免添加单独的GUC参数，将每个元组的处理成本估算为cpu_tuple_cost乘以该系数
 */
#define APPEND_CPU_COST_MULTIPLIER 0.5


/*
 * 顺序扫描一个数据页的成本估算值
 * 从配置参数seq_page_cost获取，可由用户在postgresql.conf中调整
 * 反映了数据库系统对顺序I/O操作的相对成本评估
 */
double		seq_page_cost = DEFAULT_SEQ_PAGE_COST;

/*
 * 随机扫描一个数据页的成本估算值
 * 从配置参数random_page_cost获取，通常设置为比seq_page_cost高的值
 * 反映了随机I/O与顺序I/O相比的额外开销，如磁盘寻道时间
 */
double		random_page_cost = DEFAULT_RANDOM_PAGE_COST;

/*
 * 处理一个数据元组的CPU成本估算值
 * 用于计算扫描操作中处理每个元组的CPU开销
 */
double		cpu_tuple_cost = DEFAULT_CPU_TUPLE_COST;

/*
 * 处理一个索引元组的CPU成本估算值
 * 用于计算索引扫描中处理每个索引项的CPU开销
 */
double		cpu_index_tuple_cost = DEFAULT_CPU_INDEX_TUPLE_COST;

/*
 * 执行一个操作符的CPU成本估算值
 * 用于计算查询中执行各种操作符(如比较、数学运算)的CPU开销
 */
double		cpu_operator_cost = DEFAULT_CPU_OPERATOR_COST;

/*
 * 并行查询中，将一个元组从工作进程传递到主进程的成本估算值
 * 影响并行查询计划的成本计算
 */
double		parallel_tuple_cost = DEFAULT_PARALLEL_TUPLE_COST;

/*
 * 并行查询中，启动并行工作进程的设置成本估算值
 * 用于计算并行查询的初始开销
 */
double		parallel_setup_cost = DEFAULT_PARALLEL_SETUP_COST;

/*
 * 有效的缓存大小估算值(以页为单位)
 * 由配置参数effective_cache_size设置，指示系统估计可用缓存空间
 * 影响查询优化器对索引使用效率的判断
 */
int		effective_cache_size = DEFAULT_EFFECTIVE_CACHE_SIZE;

/*
 * 禁用某项操作的成本值(一个非常大的值)
 * 用于在某些情况下明确禁用特定的查询路径
 */
Cost		disable_cost = 1.0e10;

/*
 * 每个Gather节点允许的最大并行工作进程数
 * 控制并行查询的并行度
 */
int		max_parallel_workers_per_gather = 2;

/*
 * 以下布尔变量控制是否启用各种查询计划操作
 */

/* 是否启用顺序扫描 */
bool		enable_seqscan = true;

/* 是否启用索引扫描 */
bool		enable_indexscan = true;

/* 是否启用仅索引扫描(不访问表数据页) */
bool		enable_indexonlyscan = true;

/* 是否启用位图扫描 */
bool		enable_bitmapscan = true;

/* 是否启用TID扫描(通过元组ID直接访问元组) */
bool		enable_tidscan = true;

/* 是否启用排序操作 */
bool		enable_sort = true;

/* 是否启用哈希聚合 */
bool		enable_hashagg = true;

/* 是否启用嵌套循环连接 */
bool		enable_nestloop = true;

/* 是否启用物化操作(缓存中间结果) */
bool		enable_material = true;

/* 是否启用归并连接 */
bool		enable_mergejoin = true;

/* 是否启用哈希连接 */
bool		enable_hashjoin = true;

/* 是否启用GatherMerge操作(用于并行查询中的排序合并) */
bool		enable_gathermerge = true;

/* 是否启用分区表间的分区级连接 */
bool		enable_partitionwise_join = false;

/* 是否启用分区表间的分区级聚合 */
bool		enable_partitionwise_aggregate = false;

/* 是否启用并行Append操作 */
bool		enable_parallel_append = true;

/* 是否启用并行哈希连接 */
bool		enable_parallel_hash = true;

/* 是否启用分区裁剪(排除不需要扫描的分区) */
bool		enable_partition_pruning = true;


typedef struct
{
	PlannerInfo *root;
	QualCost	total;
} cost_qual_eval_context;

static List *extract_nonindex_conditions(List *qual_clauses, List *indexclauses);
static MergeScanSelCache *cached_scansel(PlannerInfo *root,
										 RestrictInfo *rinfo,
										 PathKey *pathkey);
static void cost_rescan(PlannerInfo *root, Path *path,
						Cost *rescan_startup_cost, Cost *rescan_total_cost);
static bool cost_qual_eval_walker(Node *node, cost_qual_eval_context *context);
static void get_restriction_qual_cost(PlannerInfo *root, RelOptInfo *baserel,
									  ParamPathInfo *param_info,
									  QualCost *qpqual_cost);
static bool has_indexed_join_quals(NestPath *joinpath);
static double approx_tuple_count(PlannerInfo *root, JoinPath *path,
								 List *quals);
static double calc_joinrel_size_estimate(PlannerInfo *root,
										 RelOptInfo *joinrel,
										 RelOptInfo *outer_rel,
										 RelOptInfo *inner_rel,
										 double outer_rows,
										 double inner_rows,
										 SpecialJoinInfo *sjinfo,
										 List *restrictlist);
static Selectivity get_foreign_key_join_selectivity(PlannerInfo *root,
													Relids outer_relids,
													Relids inner_relids,
													SpecialJoinInfo *sjinfo,
													List **restrictlist);
static Cost append_nonpartial_cost(List *subpaths, int numpaths,
								   int parallel_workers);
static void set_rel_width(PlannerInfo *root, RelOptInfo *rel);
static double relation_byte_size(double tuples, int width);
static double page_size(double tuples, int width);
static double get_parallel_divisor(Path *path);


/*
 * clamp_row_est
 *		Force a row-count estimate to a sane value.
 */
double
clamp_row_est(double nrows)
{
	/*
	 * Force estimate to be at least one row, to make explain output look
	 * better and to avoid possible divide-by-zero when interpolating costs.
	 * Make it an integer, too.
	 */
	if (nrows <= 1.0)
		nrows = 1.0;
	else
		nrows = rint(nrows);

	return nrows;
}


/*
 * cost_seqscan
 *	  计算并返回顺序扫描关系的成本。
 *
 * 'baserel' 是要扫描的关系
 * 'param_info' 如果这是参数化路径则为 ParamPathInfo，否则为 NULL
 */
void
cost_seqscan(Path *path, PlannerInfo *root,
			 RelOptInfo *baserel, ParamPathInfo *param_info)
{
	Cost		startup_cost = 0;
	Cost		cpu_run_cost;
	Cost		disk_run_cost;
	double		spc_seq_page_cost;
	QualCost	qpqual_cost;
	Cost		cpu_per_tuple;

	/* 只应用于基本关系 */
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_RELATION);

	/* 设置正确的行数估算值 */
	if (param_info)
		/* 如果是参数化路径，使用参数化路径的行数估算值 */
		path->rows = param_info->ppi_rows;
	else
		path->rows = baserel->rows;

	/* 如果顺序扫描被禁用，增加一个高成本惩罚 */
	if (!enable_seqscan)
		startup_cost += disable_cost;

	/* 获取表空间的顺序页成本估算值 spc_seq_page_cost */
	get_tablespace_page_costs(baserel->reltablespace,
							  NULL,
							  &spc_seq_page_cost);

	/*
	 * 磁盘成本
	 */
	disk_run_cost = spc_seq_page_cost * baserel->pages;

	/* CPU 成本 */
	get_restriction_qual_cost(root, baserel, param_info, &qpqual_cost);

	startup_cost += qpqual_cost.startup;
	cpu_per_tuple = cpu_tuple_cost + qpqual_cost.per_tuple;
	cpu_run_cost = cpu_per_tuple * baserel->tuples;
	/* tlist 评估成本按输出行支付，而不是扫描的元组 */
	startup_cost += path->pathtarget->cost.startup;
	cpu_run_cost += path->pathtarget->cost.per_tuple * path->rows;

	/* 如果使用并行，调整成本 */
	if (path->parallel_workers > 0)
	{
		double		parallel_divisor = get_parallel_divisor(path);

		/* CPU 成本在所有工作进程间分摊 */
		cpu_run_cost /= parallel_divisor;

		/*
		 * 磁盘成本可能可以摊销一部分，但通常不多，因为大多数操作系统已经做了积极的预取。
		 * 目前假设磁盘运行成本无法摊销。
		 */

		/*
		 * 对于并行计划，行数需要表示每个工作进程处理的元组数。
		 */
		path->rows = clamp_row_est(path->rows / parallel_divisor);
	}

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + cpu_run_cost + disk_run_cost;
}

/*
 * cost_samplescan
 *	  Determines and returns the cost of scanning a relation using sampling.
 *
 * 'baserel' is the relation to be scanned
 * 'param_info' is the ParamPathInfo if this is a parameterized path, else NULL
 */
void
cost_samplescan(Path *path, PlannerInfo *root,
				RelOptInfo *baserel, ParamPathInfo *param_info)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	RangeTblEntry *rte;
	TableSampleClause *tsc;
	TsmRoutine *tsm;
	double		spc_seq_page_cost,
				spc_random_page_cost,
				spc_page_cost;
	QualCost	qpqual_cost;
	Cost		cpu_per_tuple;

	/* Should only be applied to base relations with tablesample clauses */
	Assert(baserel->relid > 0);
	rte = planner_rt_fetch(baserel->relid, root);
	Assert(rte->rtekind == RTE_RELATION);
	tsc = rte->tablesample;
	Assert(tsc != NULL);
	tsm = GetTsmRoutine(tsc->tsmhandler);

	/* Mark the path with the correct row estimate */
	if (param_info)
		path->rows = param_info->ppi_rows;
	else
		path->rows = baserel->rows;

	/* fetch estimated page cost for tablespace containing table */
	get_tablespace_page_costs(baserel->reltablespace,
							  &spc_random_page_cost,
							  &spc_seq_page_cost);

	/* if NextSampleBlock is used, assume random access, else sequential */
	spc_page_cost = (tsm->NextSampleBlock != NULL) ?
		spc_random_page_cost : spc_seq_page_cost;

	/*
	 * disk costs (recall that baserel->pages has already been set to the
	 * number of pages the sampling method will visit)
	 */
	run_cost += spc_page_cost * baserel->pages;

	/*
	 * CPU costs (recall that baserel->tuples has already been set to the
	 * number of tuples the sampling method will select).  Note that we ignore
	 * execution cost of the TABLESAMPLE parameter expressions; they will be
	 * evaluated only once per scan, and in most usages they'll likely be
	 * simple constants anyway.  We also don't charge anything for the
	 * calculations the sampling method might do internally.
	 */
	get_restriction_qual_cost(root, baserel, param_info, &qpqual_cost);

	startup_cost += qpqual_cost.startup;
	cpu_per_tuple = cpu_tuple_cost + qpqual_cost.per_tuple;
	run_cost += cpu_per_tuple * baserel->tuples;
	/* tlist eval costs are paid per output row, not per tuple scanned */
	startup_cost += path->pathtarget->cost.startup;
	run_cost += path->pathtarget->cost.per_tuple * path->rows;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_gather
 *	  Determines and returns the cost of gather path.
 *
 * 'rel' is the relation to be operated upon
 * 'param_info' is the ParamPathInfo if this is a parameterized path, else NULL
 * 'rows' may be used to point to a row estimate; if non-NULL, it overrides
 * both 'rel' and 'param_info'.  This is useful when the path doesn't exactly
 * correspond to any particular RelOptInfo.
 */
void
cost_gather(GatherPath *path, PlannerInfo *root,
			RelOptInfo *rel, ParamPathInfo *param_info,
			double *rows)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;

	/* Mark the path with the correct row estimate */
	if (rows)
		path->path.rows = *rows;
	else if (param_info)
		path->path.rows = param_info->ppi_rows;
	else
		path->path.rows = rel->rows;

	startup_cost = path->subpath->startup_cost;

	run_cost = path->subpath->total_cost - path->subpath->startup_cost;

	/* Parallel setup and communication cost. */
	startup_cost += parallel_setup_cost;
	run_cost += parallel_tuple_cost * path->path.rows;

	path->path.startup_cost = startup_cost;
	path->path.total_cost = (startup_cost + run_cost);
}

/*
 * cost_gather_merge
 *	  Determines and returns the cost of gather merge path.
 *
 * GatherMerge merges several pre-sorted input streams, using a heap that at
 * any given instant holds the next tuple from each stream. If there are N
 * streams, we need about N*log2(N) tuple comparisons to construct the heap at
 * startup, and then for each output tuple, about log2(N) comparisons to
 * replace the top heap entry with the next tuple from the same stream.
 */
void
cost_gather_merge(GatherMergePath *path, PlannerInfo *root,
				  RelOptInfo *rel, ParamPathInfo *param_info,
				  Cost input_startup_cost, Cost input_total_cost,
				  double *rows)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	Cost		comparison_cost;
	double		N;
	double		logN;

	/* Mark the path with the correct row estimate */
	if (rows)
		path->path.rows = *rows;
	else if (param_info)
		path->path.rows = param_info->ppi_rows;
	else
		path->path.rows = rel->rows;

	if (!enable_gathermerge)
		startup_cost += disable_cost;

	/*
	 * Add one to the number of workers to account for the leader.  This might
	 * be overgenerous since the leader will do less work than other workers
	 * in typical cases, but we'll go with it for now.
	 */
	Assert(path->num_workers > 0);
	N = (double) path->num_workers + 1;
	logN = LOG2(N);

	/* Assumed cost per tuple comparison */
	comparison_cost = 2.0 * cpu_operator_cost;

	/* Heap creation cost */
	startup_cost += comparison_cost * N * logN;

	/* Per-tuple heap maintenance cost */
	run_cost += path->path.rows * comparison_cost * logN;

	/* small cost for heap management, like cost_merge_append */
	run_cost += cpu_operator_cost * path->path.rows;

	/*
	 * Parallel setup and communication cost.  Since Gather Merge, unlike
	 * Gather, requires us to block until a tuple is available from every
	 * worker, we bump the IPC cost up a little bit as compared with Gather.
	 * For lack of a better idea, charge an extra 5%.
	 */
	startup_cost += parallel_setup_cost;
	run_cost += parallel_tuple_cost * path->path.rows * 1.05;

	path->path.startup_cost = startup_cost + input_startup_cost;
	path->path.total_cost = (startup_cost + run_cost + input_total_cost);
}

/*
 * cost_index
 *	  Determines and returns the cost of scanning a relation using an index.
 *
 * 'path' describes the indexscan under consideration, and is complete
 *		except for the fields to be set by this routine
 * 'loop_count' is the number of repetitions of the indexscan to factor into
 *		estimates of caching behavior
 *
 * In addition to rows, startup_cost and total_cost, cost_index() sets the
 * path's indextotalcost and indexselectivity fields.  These values will be
 * needed if the IndexPath is used in a BitmapIndexScan.
 *
 * NOTE: path->indexquals must contain only clauses usable as index
 * restrictions.  Any additional quals evaluated as qpquals may reduce the
 * number of returned tuples, but they won't reduce the number of tuples
 * we have to fetch from the table, so they don't reduce the scan cost.
 */
void
cost_index(IndexPath *path, PlannerInfo *root, double loop_count,
		   bool partial_path)
{
	IndexOptInfo *index = path->indexinfo;
	RelOptInfo *baserel = index->rel;
	bool		indexonly = (path->path.pathtype == T_IndexOnlyScan);
	amcostestimate_function amcostestimate;
	List	   *qpquals;
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	Cost		cpu_run_cost = 0;
	Cost		indexStartupCost;
	Cost		indexTotalCost;
	Selectivity indexSelectivity;
	double		indexCorrelation,
				csquared;
	double		spc_seq_page_cost,
				spc_random_page_cost;
	Cost		min_IO_cost,
				max_IO_cost;
	QualCost	qpqual_cost;
	Cost		cpu_per_tuple;
	double		tuples_fetched;
	double		pages_fetched;
	double		rand_heap_pages;
	double		index_pages;

	/* Should only be applied to base relations */
	Assert(IsA(baserel, RelOptInfo) &&
		   IsA(index, IndexOptInfo));
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_RELATION);

	/*
	 * Mark the path with the correct row estimate, and identify which quals
	 * will need to be enforced as qpquals.  We need not check any quals that
	 * are implied by the index's predicate, so we can use indrestrictinfo not
	 * baserestrictinfo as the list of relevant restriction clauses for the
	 * rel.
	 */
	if (path->path.param_info)
	{
		path->path.rows = path->path.param_info->ppi_rows;
		/* qpquals come from the rel's restriction clauses and ppi_clauses */
		qpquals = list_concat(extract_nonindex_conditions(path->indexinfo->indrestrictinfo,
														  path->indexclauses),
							  extract_nonindex_conditions(path->path.param_info->ppi_clauses,
														  path->indexclauses));
	}
	else
	{
		path->path.rows = baserel->rows;
		/* qpquals come from just the rel's restriction clauses */
		qpquals = extract_nonindex_conditions(path->indexinfo->indrestrictinfo,
											  path->indexclauses);
	}

	if (!enable_indexscan)
		startup_cost += disable_cost;
	/* we don't need to check enable_indexonlyscan; indxpath.c does that */

	/*
	 * Call index-access-method-specific code to estimate the processing cost
	 * for scanning the index, as well as the selectivity of the index (ie,
	 * the fraction of main-table tuples we will have to retrieve) and its
	 * correlation to the main-table tuple order.  We need a cast here because
	 * pathnodes.h uses a weak function type to avoid including amapi.h.
	 */
	amcostestimate = (amcostestimate_function) index->amcostestimate;
	amcostestimate(root, path, loop_count,
				   &indexStartupCost, &indexTotalCost,
				   &indexSelectivity, &indexCorrelation,
				   &index_pages);

	/*
	 * Save amcostestimate's results for possible use in bitmap scan planning.
	 * We don't bother to save indexStartupCost or indexCorrelation, because a
	 * bitmap scan doesn't care about either.
	 */
	path->indextotalcost = indexTotalCost;
	path->indexselectivity = indexSelectivity;

	/* all costs for touching index itself included here */
	startup_cost += indexStartupCost;
	run_cost += indexTotalCost - indexStartupCost;

	/* estimate number of main-table tuples fetched */
	tuples_fetched = clamp_row_est(indexSelectivity * baserel->tuples);

	/* fetch estimated page costs for tablespace containing table */
	get_tablespace_page_costs(baserel->reltablespace,
							  &spc_random_page_cost,
							  &spc_seq_page_cost);

	/*----------
	 * Estimate number of main-table pages fetched, and compute I/O cost.
	 *
	 * When the index ordering is uncorrelated with the table ordering,
	 * we use an approximation proposed by Mackert and Lohman (see
	 * index_pages_fetched() for details) to compute the number of pages
	 * fetched, and then charge spc_random_page_cost per page fetched.
	 *
	 * When the index ordering is exactly correlated with the table ordering
	 * (just after a CLUSTER, for example), the number of pages fetched should
	 * be exactly selectivity * table_size.  What's more, all but the first
	 * will be sequential fetches, not the random fetches that occur in the
	 * uncorrelated case.  So if the number of pages is more than 1, we
	 * ought to charge
	 *		spc_random_page_cost + (pages_fetched - 1) * spc_seq_page_cost
	 * For partially-correlated indexes, we ought to charge somewhere between
	 * these two estimates.  We currently interpolate linearly between the
	 * estimates based on the correlation squared (XXX is that appropriate?).
	 *
	 * If it's an index-only scan, then we will not need to fetch any heap
	 * pages for which the visibility map shows all tuples are visible.
	 * Hence, reduce the estimated number of heap fetches accordingly.
	 * We use the measured fraction of the entire heap that is all-visible,
	 * which might not be particularly relevant to the subset of the heap
	 * that this query will fetch; but it's not clear how to do better.
	 *----------
	 */
	if (loop_count > 1)
	{
		/*
		 * For repeated indexscans, the appropriate estimate for the
		 * uncorrelated case is to scale up the number of tuples fetched in
		 * the Mackert and Lohman formula by the number of scans, so that we
		 * estimate the number of pages fetched by all the scans; then
		 * pro-rate the costs for one scan.  In this case we assume all the
		 * fetches are random accesses.
		 */
		pages_fetched = index_pages_fetched(tuples_fetched * loop_count,
											baserel->pages,
											(double) index->pages,
											root);

		if (indexonly)
			pages_fetched = ceil(pages_fetched * (1.0 - baserel->allvisfrac));

		rand_heap_pages = pages_fetched;

		max_IO_cost = (pages_fetched * spc_random_page_cost) / loop_count;

		/*
		 * In the perfectly correlated case, the number of pages touched by
		 * each scan is selectivity * table_size, and we can use the Mackert
		 * and Lohman formula at the page level to estimate how much work is
		 * saved by caching across scans.  We still assume all the fetches are
		 * random, though, which is an overestimate that's hard to correct for
		 * without double-counting the cache effects.  (But in most cases
		 * where such a plan is actually interesting, only one page would get
		 * fetched per scan anyway, so it shouldn't matter much.)
		 */
		pages_fetched = ceil(indexSelectivity * (double) baserel->pages);

		pages_fetched = index_pages_fetched(pages_fetched * loop_count,
											baserel->pages,
											(double) index->pages,
											root);

		if (indexonly)
			pages_fetched = ceil(pages_fetched * (1.0 - baserel->allvisfrac));

		min_IO_cost = (pages_fetched * spc_random_page_cost) / loop_count;
	}
	else
	{
		/*
		 * Normal case: apply the Mackert and Lohman formula, and then
		 * interpolate between that and the correlation-derived result.
		 */
		pages_fetched = index_pages_fetched(tuples_fetched,
											baserel->pages,
											(double) index->pages,
											root);

		if (indexonly)
			pages_fetched = ceil(pages_fetched * (1.0 - baserel->allvisfrac));

		rand_heap_pages = pages_fetched;

		/* max_IO_cost is for the perfectly uncorrelated case (csquared=0) */
		max_IO_cost = pages_fetched * spc_random_page_cost;

		/* min_IO_cost is for the perfectly correlated case (csquared=1) */
		pages_fetched = ceil(indexSelectivity * (double) baserel->pages);

		if (indexonly)
			pages_fetched = ceil(pages_fetched * (1.0 - baserel->allvisfrac));

		if (pages_fetched > 0)
		{
			min_IO_cost = spc_random_page_cost;
			if (pages_fetched > 1)
				min_IO_cost += (pages_fetched - 1) * spc_seq_page_cost;
		}
		else
			min_IO_cost = 0;
	}

	if (partial_path)
	{
		/*
		 * For index only scans compute workers based on number of index pages
		 * fetched; the number of heap pages we fetch might be so small as to
		 * effectively rule out parallelism, which we don't want to do.
		 */
		if (indexonly)
			rand_heap_pages = -1;

		/*
		 * Estimate the number of parallel workers required to scan index. Use
		 * the number of heap pages computed considering heap fetches won't be
		 * sequential as for parallel scans the pages are accessed in random
		 * order.
		 */
		path->path.parallel_workers = compute_parallel_worker(baserel,
															  rand_heap_pages,
															  index_pages,
															  max_parallel_workers_per_gather);

		/*
		 * Fall out if workers can't be assigned for parallel scan, because in
		 * such a case this path will be rejected.  So there is no benefit in
		 * doing extra computation.
		 */
		if (path->path.parallel_workers <= 0)
			return;

		path->path.parallel_aware = true;
	}

	/*
	 * Now interpolate based on estimated index order correlation to get total
	 * disk I/O cost for main table accesses.
	 */
	csquared = indexCorrelation * indexCorrelation;

	run_cost += max_IO_cost + csquared * (min_IO_cost - max_IO_cost);

	/*
	 * Estimate CPU costs per tuple.
	 *
	 * What we want here is cpu_tuple_cost plus the evaluation costs of any
	 * qual clauses that we have to evaluate as qpquals.
	 */
	cost_qual_eval(&qpqual_cost, qpquals, root);

	startup_cost += qpqual_cost.startup;
	cpu_per_tuple = cpu_tuple_cost + qpqual_cost.per_tuple;

	cpu_run_cost += cpu_per_tuple * tuples_fetched;

	/* tlist eval costs are paid per output row, not per tuple scanned */
	startup_cost += path->path.pathtarget->cost.startup;
	cpu_run_cost += path->path.pathtarget->cost.per_tuple * path->path.rows;

	/* Adjust costing for parallelism, if used. */
	if (path->path.parallel_workers > 0)
	{
		double		parallel_divisor = get_parallel_divisor(&path->path);

		path->path.rows = clamp_row_est(path->path.rows / parallel_divisor);

		/* The CPU cost is divided among all the workers. */
		cpu_run_cost /= parallel_divisor;
	}

	run_cost += cpu_run_cost;

	path->path.startup_cost = startup_cost;
	path->path.total_cost = startup_cost + run_cost;
}

/*
 * extract_nonindex_conditions
 *
 * Given a list of quals to be enforced in an indexscan, extract the ones that
 * will have to be applied as qpquals (ie, the index machinery won't handle
 * them).  Here we detect only whether a qual clause is directly redundant
 * with some indexclause.  If the index path is chosen for use, createplan.c
 * will try a bit harder to get rid of redundant qual conditions; specifically
 * it will see if quals can be proven to be implied by the indexquals.  But
 * it does not seem worth the cycles to try to factor that in at this stage,
 * since we're only trying to estimate qual eval costs.  Otherwise this must
 * match the logic in create_indexscan_plan().
 *
 * qual_clauses, and the result, are lists of RestrictInfos.
 * indexclauses is a list of IndexClauses.
 */
static List *
extract_nonindex_conditions(List *qual_clauses, List *indexclauses)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, qual_clauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		if (rinfo->pseudoconstant)
			continue;			/* we may drop pseudoconstants here */
		if (is_redundant_with_indexclauses(rinfo, indexclauses))
			continue;			/* dup or derived from same EquivalenceClass */
		/* ... skip the predicate proof attempt createplan.c will try ... */
		result = lappend(result, rinfo);
	}
	return result;
}

/*
 * index_pages_fetched
 *	  Estimate the number of pages actually fetched after accounting for
 *	  cache effects.
 *
 * We use an approximation proposed by Mackert and Lohman, "Index Scans
 * Using a Finite LRU Buffer: A Validated I/O Model", ACM Transactions
 * on Database Systems, Vol. 14, No. 3, September 1989, Pages 401-424.
 * The Mackert and Lohman approximation is that the number of pages
 * fetched is
 *	PF =
 *		min(2TNs/(2T+Ns), T)			when T <= b
 *		2TNs/(2T+Ns)					when T > b and Ns <= 2Tb/(2T-b)
 *		b + (Ns - 2Tb/(2T-b))*(T-b)/T	when T > b and Ns > 2Tb/(2T-b)
 * where
 *		T = # pages in table
 *		N = # tuples in table
 *		s = selectivity = fraction of table to be scanned
 *		b = # buffer pages available (we include kernel space here)
 *
 * We assume that effective_cache_size is the total number of buffer pages
 * available for the whole query, and pro-rate that space across all the
 * tables in the query and the index currently under consideration.  (This
 * ignores space needed for other indexes used by the query, but since we
 * don't know which indexes will get used, we can't estimate that very well;
 * and in any case counting all the tables may well be an overestimate, since
 * depending on the join plan not all the tables may be scanned concurrently.)
 *
 * The product Ns is the number of tuples fetched; we pass in that
 * product rather than calculating it here.  "pages" is the number of pages
 * in the object under consideration (either an index or a table).
 * "index_pages" is the amount to add to the total table space, which was
 * computed for us by make_one_rel.
 *
 * Caller is expected to have ensured that tuples_fetched is greater than zero
 * and rounded to integer (see clamp_row_est).  The result will likewise be
 * greater than zero and integral.
 */
double
index_pages_fetched(double tuples_fetched, BlockNumber pages,
					double index_pages, PlannerInfo *root)
{
	double		pages_fetched;
	double		total_pages;
	double		T,
				b;

	/* T is # pages in table, but don't allow it to be zero */
	T = (pages > 1) ? (double) pages : 1.0;

	/* Compute number of pages assumed to be competing for cache space */
	total_pages = root->total_table_pages + index_pages;
	total_pages = Max(total_pages, 1.0);
	Assert(T <= total_pages);

	/* b is pro-rated share of effective_cache_size */
	b = (double) effective_cache_size * T / total_pages;

	/* force it positive and integral */
	if (b <= 1.0)
		b = 1.0;
	else
		b = ceil(b);

	/* This part is the Mackert and Lohman formula */
	if (T <= b)
	{
		pages_fetched =
			(2.0 * T * tuples_fetched) / (2.0 * T + tuples_fetched);
		if (pages_fetched >= T)
			pages_fetched = T;
		else
			pages_fetched = ceil(pages_fetched);
	}
	else
	{
		double		lim;

		lim = (2.0 * T * b) / (2.0 * T - b);
		if (tuples_fetched <= lim)
		{
			pages_fetched =
				(2.0 * T * tuples_fetched) / (2.0 * T + tuples_fetched);
		}
		else
		{
			pages_fetched =
				b + (tuples_fetched - lim) * (T - b) / T;
		}
		pages_fetched = ceil(pages_fetched);
	}
	return pages_fetched;
}

/*
 * get_indexpath_pages
 *		Determine the total size of the indexes used in a bitmap index path.
 *
 * Note: if the same index is used more than once in a bitmap tree, we will
 * count it multiple times, which perhaps is the wrong thing ... but it's
 * not completely clear, and detecting duplicates is difficult, so ignore it
 * for now.
 */
static double
get_indexpath_pages(Path *bitmapqual)
{
	double		result = 0;
	ListCell   *l;

	if (IsA(bitmapqual, BitmapAndPath))
	{
		BitmapAndPath *apath = (BitmapAndPath *) bitmapqual;

		foreach(l, apath->bitmapquals)
		{
			result += get_indexpath_pages((Path *) lfirst(l));
		}
	}
	else if (IsA(bitmapqual, BitmapOrPath))
	{
		BitmapOrPath *opath = (BitmapOrPath *) bitmapqual;

		foreach(l, opath->bitmapquals)
		{
			result += get_indexpath_pages((Path *) lfirst(l));
		}
	}
	else if (IsA(bitmapqual, IndexPath))
	{
		IndexPath  *ipath = (IndexPath *) bitmapqual;

		result = (double) ipath->indexinfo->pages;
	}
	else
		elog(ERROR, "unrecognized node type: %d", nodeTag(bitmapqual));

	return result;
}

/*
 * cost_bitmap_heap_scan
 *	  Determines and returns the cost of scanning a relation using a bitmap
 *	  index-then-heap plan.
 *
 * 'baserel' is the relation to be scanned
 * 'param_info' is the ParamPathInfo if this is a parameterized path, else NULL
 * 'bitmapqual' is a tree of IndexPaths, BitmapAndPaths, and BitmapOrPaths
 * 'loop_count' is the number of repetitions of the indexscan to factor into
 *		estimates of caching behavior
 *
 * Note: the component IndexPaths in bitmapqual should have been costed
 * using the same loop_count.
 */
void
cost_bitmap_heap_scan(Path *path, PlannerInfo *root, RelOptInfo *baserel,
					  ParamPathInfo *param_info,
					  Path *bitmapqual, double loop_count)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	Cost		indexTotalCost;
	QualCost	qpqual_cost;
	Cost		cpu_per_tuple;
	Cost		cost_per_page;
	Cost		cpu_run_cost;
	double		tuples_fetched;
	double		pages_fetched;
	double		spc_seq_page_cost,
				spc_random_page_cost;
	double		T;

	/* Should only be applied to base relations */
	Assert(IsA(baserel, RelOptInfo));
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_RELATION);

	/* Mark the path with the correct row estimate */
	if (param_info)
		path->rows = param_info->ppi_rows;
	else
		path->rows = baserel->rows;

	if (!enable_bitmapscan)
		startup_cost += disable_cost;

	pages_fetched = compute_bitmap_pages(root, baserel, bitmapqual,
										 loop_count, &indexTotalCost,
										 &tuples_fetched);

	startup_cost += indexTotalCost;
	T = (baserel->pages > 1) ? (double) baserel->pages : 1.0;

	/* Fetch estimated page costs for tablespace containing table. */
	get_tablespace_page_costs(baserel->reltablespace,
							  &spc_random_page_cost,
							  &spc_seq_page_cost);

	/*
	 * For small numbers of pages we should charge spc_random_page_cost
	 * apiece, while if nearly all the table's pages are being read, it's more
	 * appropriate to charge spc_seq_page_cost apiece.  The effect is
	 * nonlinear, too. For lack of a better idea, interpolate like this to
	 * determine the cost per page.
	 */
	if (pages_fetched >= 2.0)
		cost_per_page = spc_random_page_cost -
			(spc_random_page_cost - spc_seq_page_cost)
			* sqrt(pages_fetched / T);
	else
		cost_per_page = spc_random_page_cost;

	run_cost += pages_fetched * cost_per_page;

	/*
	 * Estimate CPU costs per tuple.
	 *
	 * Often the indexquals don't need to be rechecked at each tuple ... but
	 * not always, especially not if there are enough tuples involved that the
	 * bitmaps become lossy.  For the moment, just assume they will be
	 * rechecked always.  This means we charge the full freight for all the
	 * scan clauses.
	 */
	get_restriction_qual_cost(root, baserel, param_info, &qpqual_cost);

	startup_cost += qpqual_cost.startup;
	cpu_per_tuple = cpu_tuple_cost + qpqual_cost.per_tuple;
	cpu_run_cost = cpu_per_tuple * tuples_fetched;

	/* Adjust costing for parallelism, if used. */
	if (path->parallel_workers > 0)
	{
		double		parallel_divisor = get_parallel_divisor(path);

		/* The CPU cost is divided among all the workers. */
		cpu_run_cost /= parallel_divisor;

		path->rows = clamp_row_est(path->rows / parallel_divisor);
	}


	run_cost += cpu_run_cost;

	/* tlist eval costs are paid per output row, not per tuple scanned */
	startup_cost += path->pathtarget->cost.startup;
	run_cost += path->pathtarget->cost.per_tuple * path->rows;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_bitmap_tree_node
 *		Extract cost and selectivity from a bitmap tree node (index/and/or)
 */
void
cost_bitmap_tree_node(Path *path, Cost *cost, Selectivity *selec)
{
	if (IsA(path, IndexPath))
	{
		*cost = ((IndexPath *) path)->indextotalcost;
		*selec = ((IndexPath *) path)->indexselectivity;

		/*
		 * Charge a small amount per retrieved tuple to reflect the costs of
		 * manipulating the bitmap.  This is mostly to make sure that a bitmap
		 * scan doesn't look to be the same cost as an indexscan to retrieve a
		 * single tuple.
		 */
		*cost += 0.1 * cpu_operator_cost * path->rows;
	}
	else if (IsA(path, BitmapAndPath))
	{
		*cost = path->total_cost;
		*selec = ((BitmapAndPath *) path)->bitmapselectivity;
	}
	else if (IsA(path, BitmapOrPath))
	{
		*cost = path->total_cost;
		*selec = ((BitmapOrPath *) path)->bitmapselectivity;
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d", nodeTag(path));
		*cost = *selec = 0;		/* keep compiler quiet */
	}
}

/*
 * cost_bitmap_and_node
 *		Estimate the cost of a BitmapAnd node
 *
 * Note that this considers only the costs of index scanning and bitmap
 * creation, not the eventual heap access.  In that sense the object isn't
 * truly a Path, but it has enough path-like properties (costs in particular)
 * to warrant treating it as one.  We don't bother to set the path rows field,
 * however.
 */
void
cost_bitmap_and_node(BitmapAndPath *path, PlannerInfo *root)
{
	Cost		totalCost;
	Selectivity selec;
	ListCell   *l;

	/*
	 * We estimate AND selectivity on the assumption that the inputs are
	 * independent.  This is probably often wrong, but we don't have the info
	 * to do better.
	 *
	 * The runtime cost of the BitmapAnd itself is estimated at 100x
	 * cpu_operator_cost for each tbm_intersect needed.  Probably too small,
	 * definitely too simplistic?
	 */
	totalCost = 0.0;
	selec = 1.0;
	foreach(l, path->bitmapquals)
	{
		Path	   *subpath = (Path *) lfirst(l);
		Cost		subCost;
		Selectivity subselec;

		cost_bitmap_tree_node(subpath, &subCost, &subselec);

		selec *= subselec;

		totalCost += subCost;
		if (l != list_head(path->bitmapquals))
			totalCost += 100.0 * cpu_operator_cost;
	}
	path->bitmapselectivity = selec;
	path->path.rows = 0;		/* per above, not used */
	path->path.startup_cost = totalCost;
	path->path.total_cost = totalCost;
}

/*
 * cost_bitmap_or_node
 *		Estimate the cost of a BitmapOr node
 *
 * See comments for cost_bitmap_and_node.
 */
void
cost_bitmap_or_node(BitmapOrPath *path, PlannerInfo *root)
{
	Cost		totalCost;
	Selectivity selec;
	ListCell   *l;

	/*
	 * We estimate OR selectivity on the assumption that the inputs are
	 * non-overlapping, since that's often the case in "x IN (list)" type
	 * situations.  Of course, we clamp to 1.0 at the end.
	 *
	 * The runtime cost of the BitmapOr itself is estimated at 100x
	 * cpu_operator_cost for each tbm_union needed.  Probably too small,
	 * definitely too simplistic?  We are aware that the tbm_unions are
	 * optimized out when the inputs are BitmapIndexScans.
	 */
	totalCost = 0.0;
	selec = 0.0;
	foreach(l, path->bitmapquals)
	{
		Path	   *subpath = (Path *) lfirst(l);
		Cost		subCost;
		Selectivity subselec;

		cost_bitmap_tree_node(subpath, &subCost, &subselec);

		selec += subselec;

		totalCost += subCost;
		if (l != list_head(path->bitmapquals) &&
			!IsA(subpath, IndexPath))
			totalCost += 100.0 * cpu_operator_cost;
	}
	path->bitmapselectivity = Min(selec, 1.0);
	path->path.rows = 0;		/* per above, not used */
	path->path.startup_cost = totalCost;
	path->path.total_cost = totalCost;
}

/*
 * cost_tidscan
 *	  Determines and returns the cost of scanning a relation using TIDs.
 *
 * 'baserel' is the relation to be scanned
 * 'tidquals' is the list of TID-checkable quals
 * 'param_info' is the ParamPathInfo if this is a parameterized path, else NULL
 */
void
cost_tidscan(Path *path, PlannerInfo *root,
			 RelOptInfo *baserel, List *tidquals, ParamPathInfo *param_info)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	bool		isCurrentOf = false;
	QualCost	qpqual_cost;
	Cost		cpu_per_tuple;
	QualCost	tid_qual_cost;
	int			ntuples;
	ListCell   *l;
	double		spc_random_page_cost;

	/* Should only be applied to base relations */
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_RELATION);

	/* Mark the path with the correct row estimate */
	if (param_info)
		path->rows = param_info->ppi_rows;
	else
		path->rows = baserel->rows;

	/* Count how many tuples we expect to retrieve */
	ntuples = 0;
	foreach(l, tidquals)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);
		Expr	   *qual = rinfo->clause;

		if (IsA(qual, ScalarArrayOpExpr))
		{
			/* Each element of the array yields 1 tuple */
			ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) qual;
			Node	   *arraynode = (Node *) lsecond(saop->args);

			ntuples += estimate_array_length(arraynode);
		}
		else if (IsA(qual, CurrentOfExpr))
		{
			/* CURRENT OF yields 1 tuple */
			isCurrentOf = true;
			ntuples++;
		}
		else
		{
			/* It's just CTID = something, count 1 tuple */
			ntuples++;
		}
	}

	/*
	 * We must force TID scan for WHERE CURRENT OF, because only nodeTidscan.c
	 * understands how to do it correctly.  Therefore, honor enable_tidscan
	 * only when CURRENT OF isn't present.  Also note that cost_qual_eval
	 * counts a CurrentOfExpr as having startup cost disable_cost, which we
	 * subtract off here; that's to prevent other plan types such as seqscan
	 * from winning.
	 */
	if (isCurrentOf)
	{
		Assert(baserel->baserestrictcost.startup >= disable_cost);
		startup_cost -= disable_cost;
	}
	else if (!enable_tidscan)
		startup_cost += disable_cost;

	/*
	 * The TID qual expressions will be computed once, any other baserestrict
	 * quals once per retrieved tuple.
	 */
	cost_qual_eval(&tid_qual_cost, tidquals, root);

	/* fetch estimated page cost for tablespace containing table */
	get_tablespace_page_costs(baserel->reltablespace,
							  &spc_random_page_cost,
							  NULL);

	/* disk costs --- assume each tuple on a different page */
	run_cost += spc_random_page_cost * ntuples;

	/* Add scanning CPU costs */
	get_restriction_qual_cost(root, baserel, param_info, &qpqual_cost);

	/* XXX currently we assume TID quals are a subset of qpquals */
	startup_cost += qpqual_cost.startup + tid_qual_cost.per_tuple;
	cpu_per_tuple = cpu_tuple_cost + qpqual_cost.per_tuple -
		tid_qual_cost.per_tuple;
	run_cost += cpu_per_tuple * ntuples;

	/* tlist eval costs are paid per output row, not per tuple scanned */
	startup_cost += path->pathtarget->cost.startup;
	run_cost += path->pathtarget->cost.per_tuple * path->rows;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_subqueryscan
 *	  Determines and returns the cost of scanning a subquery RTE.
 *
 * 'baserel' is the relation to be scanned
 * 'param_info' is the ParamPathInfo if this is a parameterized path, else NULL
 */
void
cost_subqueryscan(SubqueryScanPath *path, PlannerInfo *root,
				  RelOptInfo *baserel, ParamPathInfo *param_info)
{
	Cost		startup_cost;
	Cost		run_cost;
	QualCost	qpqual_cost;
	Cost		cpu_per_tuple;

	/* Should only be applied to base relations that are subqueries */
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_SUBQUERY);

	/* Mark the path with the correct row estimate */
	if (param_info)
		path->path.rows = param_info->ppi_rows;
	else
		path->path.rows = baserel->rows;

	/*
	 * Cost of path is cost of evaluating the subplan, plus cost of evaluating
	 * any restriction clauses and tlist that will be attached to the
	 * SubqueryScan node, plus cpu_tuple_cost to account for selection and
	 * projection overhead.
	 */
	path->path.startup_cost = path->subpath->startup_cost;
	path->path.total_cost = path->subpath->total_cost;

	get_restriction_qual_cost(root, baserel, param_info, &qpqual_cost);

	startup_cost = qpqual_cost.startup;
	cpu_per_tuple = cpu_tuple_cost + qpqual_cost.per_tuple;
	run_cost = cpu_per_tuple * baserel->tuples;

	/* tlist eval costs are paid per output row, not per tuple scanned */
	startup_cost += path->path.pathtarget->cost.startup;
	run_cost += path->path.pathtarget->cost.per_tuple * path->path.rows;

	path->path.startup_cost += startup_cost;
	path->path.total_cost += startup_cost + run_cost;
}

/*
 * cost_functionscan
 *	  Determines and returns the cost of scanning a function RTE.
 *
 * 'baserel' is the relation to be scanned
 * 'param_info' is the ParamPathInfo if this is a parameterized path, else NULL
 */
void
cost_functionscan(Path *path, PlannerInfo *root,
				  RelOptInfo *baserel, ParamPathInfo *param_info)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	QualCost	qpqual_cost;
	Cost		cpu_per_tuple;
	RangeTblEntry *rte;
	QualCost	exprcost;

	/* Should only be applied to base relations that are functions */
	Assert(baserel->relid > 0);
	rte = planner_rt_fetch(baserel->relid, root);
	Assert(rte->rtekind == RTE_FUNCTION);

	/* Mark the path with the correct row estimate */
	if (param_info)
		path->rows = param_info->ppi_rows;
	else
		path->rows = baserel->rows;

	/*
	 * Estimate costs of executing the function expression(s).
	 *
	 * Currently, nodeFunctionscan.c always executes the functions to
	 * completion before returning any rows, and caches the results in a
	 * tuplestore.  So the function eval cost is all startup cost, and per-row
	 * costs are minimal.
	 *
	 * XXX in principle we ought to charge tuplestore spill costs if the
	 * number of rows is large.  However, given how phony our rowcount
	 * estimates for functions tend to be, there's not a lot of point in that
	 * refinement right now.
	 */
	cost_qual_eval_node(&exprcost, (Node *) rte->functions, root);

	startup_cost += exprcost.startup + exprcost.per_tuple;

	/* Add scanning CPU costs */
	get_restriction_qual_cost(root, baserel, param_info, &qpqual_cost);

	startup_cost += qpqual_cost.startup;
	cpu_per_tuple = cpu_tuple_cost + qpqual_cost.per_tuple;
	run_cost += cpu_per_tuple * baserel->tuples;

	/* tlist eval costs are paid per output row, not per tuple scanned */
	startup_cost += path->pathtarget->cost.startup;
	run_cost += path->pathtarget->cost.per_tuple * path->rows;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_tablefuncscan
 *	  Determines and returns the cost of scanning a table function.
 *
 * 'baserel' is the relation to be scanned
 * 'param_info' is the ParamPathInfo if this is a parameterized path, else NULL
 */
void
cost_tablefuncscan(Path *path, PlannerInfo *root,
				   RelOptInfo *baserel, ParamPathInfo *param_info)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	QualCost	qpqual_cost;
	Cost		cpu_per_tuple;
	RangeTblEntry *rte;
	QualCost	exprcost;

	/* Should only be applied to base relations that are functions */
	Assert(baserel->relid > 0);
	rte = planner_rt_fetch(baserel->relid, root);
	Assert(rte->rtekind == RTE_TABLEFUNC);

	/* Mark the path with the correct row estimate */
	if (param_info)
		path->rows = param_info->ppi_rows;
	else
		path->rows = baserel->rows;

	/*
	 * Estimate costs of executing the table func expression(s).
	 *
	 * XXX in principle we ought to charge tuplestore spill costs if the
	 * number of rows is large.  However, given how phony our rowcount
	 * estimates for tablefuncs tend to be, there's not a lot of point in that
	 * refinement right now.
	 */
	cost_qual_eval_node(&exprcost, (Node *) rte->tablefunc, root);

	startup_cost += exprcost.startup + exprcost.per_tuple;

	/* Add scanning CPU costs */
	get_restriction_qual_cost(root, baserel, param_info, &qpqual_cost);

	startup_cost += qpqual_cost.startup;
	cpu_per_tuple = cpu_tuple_cost + qpqual_cost.per_tuple;
	run_cost += cpu_per_tuple * baserel->tuples;

	/* tlist eval costs are paid per output row, not per tuple scanned */
	startup_cost += path->pathtarget->cost.startup;
	run_cost += path->pathtarget->cost.per_tuple * path->rows;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_valuesscan
 *	  Determines and returns the cost of scanning a VALUES RTE.
 *
 * 'baserel' is the relation to be scanned
 * 'param_info' is the ParamPathInfo if this is a parameterized path, else NULL
 */
void
cost_valuesscan(Path *path, PlannerInfo *root,
				RelOptInfo *baserel, ParamPathInfo *param_info)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	QualCost	qpqual_cost;
	Cost		cpu_per_tuple;

	/* Should only be applied to base relations that are values lists */
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_VALUES);

	/* Mark the path with the correct row estimate */
	if (param_info)
		path->rows = param_info->ppi_rows;
	else
		path->rows = baserel->rows;

	/*
	 * For now, estimate list evaluation cost at one operator eval per list
	 * (probably pretty bogus, but is it worth being smarter?)
	 */
	cpu_per_tuple = cpu_operator_cost;

	/* Add scanning CPU costs */
	get_restriction_qual_cost(root, baserel, param_info, &qpqual_cost);

	startup_cost += qpqual_cost.startup;
	cpu_per_tuple += cpu_tuple_cost + qpqual_cost.per_tuple;
	run_cost += cpu_per_tuple * baserel->tuples;

	/* tlist eval costs are paid per output row, not per tuple scanned */
	startup_cost += path->pathtarget->cost.startup;
	run_cost += path->pathtarget->cost.per_tuple * path->rows;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_ctescan
 *	  Determines and returns the cost of scanning a CTE RTE.
 *
 * Note: this is used for both self-reference and regular CTEs; the
 * possible cost differences are below the threshold of what we could
 * estimate accurately anyway.  Note that the costs of evaluating the
 * referenced CTE query are added into the final plan as initplan costs,
 * and should NOT be counted here.
 */
void
cost_ctescan(Path *path, PlannerInfo *root,
			 RelOptInfo *baserel, ParamPathInfo *param_info)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	QualCost	qpqual_cost;
	Cost		cpu_per_tuple;

	/* Should only be applied to base relations that are CTEs */
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_CTE);

	/* Mark the path with the correct row estimate */
	if (param_info)
		path->rows = param_info->ppi_rows;
	else
		path->rows = baserel->rows;

	/* Charge one CPU tuple cost per row for tuplestore manipulation */
	cpu_per_tuple = cpu_tuple_cost;

	/* Add scanning CPU costs */
	get_restriction_qual_cost(root, baserel, param_info, &qpqual_cost);

	startup_cost += qpqual_cost.startup;
	cpu_per_tuple += cpu_tuple_cost + qpqual_cost.per_tuple;
	run_cost += cpu_per_tuple * baserel->tuples;

	/* tlist eval costs are paid per output row, not per tuple scanned */
	startup_cost += path->pathtarget->cost.startup;
	run_cost += path->pathtarget->cost.per_tuple * path->rows;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_namedtuplestorescan
 *	  Determines and returns the cost of scanning a named tuplestore.
 */
void
cost_namedtuplestorescan(Path *path, PlannerInfo *root,
						 RelOptInfo *baserel, ParamPathInfo *param_info)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	QualCost	qpqual_cost;
	Cost		cpu_per_tuple;

	/* Should only be applied to base relations that are Tuplestores */
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_NAMEDTUPLESTORE);

	/* Mark the path with the correct row estimate */
	if (param_info)
		path->rows = param_info->ppi_rows;
	else
		path->rows = baserel->rows;

	/* Charge one CPU tuple cost per row for tuplestore manipulation */
	cpu_per_tuple = cpu_tuple_cost;

	/* Add scanning CPU costs */
	get_restriction_qual_cost(root, baserel, param_info, &qpqual_cost);

	startup_cost += qpqual_cost.startup;
	cpu_per_tuple += cpu_tuple_cost + qpqual_cost.per_tuple;
	run_cost += cpu_per_tuple * baserel->tuples;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_resultscan
 *	  Determines and returns the cost of scanning an RTE_RESULT relation.
 */
void
cost_resultscan(Path *path, PlannerInfo *root,
				RelOptInfo *baserel, ParamPathInfo *param_info)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	QualCost	qpqual_cost;
	Cost		cpu_per_tuple;

	/* Should only be applied to RTE_RESULT base relations */
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_RESULT);

	/* Mark the path with the correct row estimate */
	if (param_info)
		path->rows = param_info->ppi_rows;
	else
		path->rows = baserel->rows;

	/* We charge qual cost plus cpu_tuple_cost */
	get_restriction_qual_cost(root, baserel, param_info, &qpqual_cost);

	startup_cost += qpqual_cost.startup;
	cpu_per_tuple = cpu_tuple_cost + qpqual_cost.per_tuple;
	run_cost += cpu_per_tuple * baserel->tuples;

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_recursive_union
 *	  Determines and returns the cost of performing a recursive union,
 *	  and also the estimated output size.
 *
 * We are given Paths for the nonrecursive and recursive terms.
 */
void
cost_recursive_union(Path *runion, Path *nrterm, Path *rterm)
{
	Cost		startup_cost;
	Cost		total_cost;
	double		total_rows;

	/* We probably have decent estimates for the non-recursive term */
	startup_cost = nrterm->startup_cost;
	total_cost = nrterm->total_cost;
	total_rows = nrterm->rows;

	/*
	 * We arbitrarily assume that about 10 recursive iterations will be
	 * needed, and that we've managed to get a good fix on the cost and output
	 * size of each one of them.  These are mighty shaky assumptions but it's
	 * hard to see how to do better.
	 */
	total_cost += 10 * rterm->total_cost;
	total_rows += 10 * rterm->rows;

	/*
	 * Also charge cpu_tuple_cost per row to account for the costs of
	 * manipulating the tuplestores.  (We don't worry about possible
	 * spill-to-disk costs.)
	 */
	total_cost += cpu_tuple_cost * total_rows;

	runion->startup_cost = startup_cost;
	runion->total_cost = total_cost;
	runion->rows = total_rows;
	runion->pathtarget->width = Max(nrterm->pathtarget->width,
									rterm->pathtarget->width);
}

/*
 * cost_sort - 计算排序一个关系的成本，包括读取输入数据的成本
 *
 * 参数说明:
 * - path: 路径节点，用于存储成本估算结果
 * - root: 查询规划器信息
 * - pathkeys: 排序键列表
 * - input_cost: 读取输入数据的总成本
 * - tuples: 关系中的元组数量
 * - width: 平均元组宽度（字节）
 * - comparison_cost: 每次比较的额外成本（可为0）
 * - sort_mem: 分配给排序的内存大小（KB）
 * - limit_tuples: 输出元组的数量限制（-1表示无限制）
 *
 * 算法说明:
 * 1. 如果待排序数据总量小于sort_mem，将执行内存排序，不需要I/O操作，
 *    大约需要 t*log2(t) 次元组比较（t为元组数）。
 *
 * 2. 如果数据量超过sort_mem，切换到磁带式归并算法。仍然需要大约 t*log2(t) 
 *    次元组比较，但每个元组在每次归并过程中都需要写入和读取一次。
 *    预期归并趟数约为 ceil(logM(r))，其中r是初始有序段数量，M是归并度。
 *
 * 3. 如果排序是有界的（只需要前k个结果元组）且k个元组可以放入sort_mem，
 *    使用堆排序方法，仅在内存中维护k个元组，需要约 t*log2(k) 次比较。
 *
 * 4. 磁盘访问假设为3/4顺序访问和1/4随机访问。
 *
 * 5. 默认情况下，每次元组比较计为两次操作符求值，在大多数情况下这个估计合理。
 *    调用者可以通过指定非零comparison_cost来调整这个值。
 */
void
cost_sort(Path *path, PlannerInfo *root,
		  List *pathkeys, Cost input_cost, double tuples, int width,
		  Cost comparison_cost, int sort_mem,
		  double limit_tuples)
{
	/* 初始化成本变量 */
	Cost		startup_cost = input_cost;  /* 启动成本初始化为输入成本 */
	Cost		run_cost = 0;               /* 运行成本 */
	
	/* 计算输入数据的字节数 */
	double		input_bytes = relation_byte_size(tuples, width);
	double		output_bytes;               /* 输出数据字节数 */
	double		output_tuples;              /* 输出元组数 */
	
	/* 将sort_mem从KB转换为字节 */
	long		sort_mem_bytes = sort_mem * 1024L;

	/* 如果排序被禁用，增加禁用成本 */
	if (!enable_sort)
		startup_cost += disable_cost;

	/* 设置路径的行数 */
	path->rows = tuples;

	/*
	 * 确保排序成本永远不会估计为零，即使是传入的元组数为零。
	 * 此外，不能计算log(0)...
	 */
	if (tuples < 2.0)
		tuples = 2.0;

	/* 包含默认的每次比较成本（2倍操作符成本） */
	comparison_cost += 2.0 * cpu_operator_cost;

	/* 检查是否有有用的LIMIT限制 */
	if (limit_tuples > 0 && limit_tuples < tuples)
	{
		/* 有限制的情况下，输出元组数等于限制数 */
		output_tuples = limit_tuples;
		/* 计算输出数据的字节数 */
		output_bytes = relation_byte_size(output_tuples, width);
	}
	else
	{
		/* 无限制或限制大于总元组数，输出等于输入 */
		output_tuples = tuples;
		output_bytes = input_bytes;
	}

	/* 根据数据量和内存大小选择不同的排序策略 */
	if (output_bytes > sort_mem_bytes)
	{
		/*
		 * 数据量超过内存限制，必须使用基于磁盘的排序
		 */
		double		npages = ceil(input_bytes / BLCKSZ);      /* 输入数据占用的页面数 */
		double		nruns = input_bytes / sort_mem_bytes;     /* 初始有序段数量 */
		double		mergeorder = tuplesort_merge_order(sort_mem_bytes);  /* 归并度 */
		double		log_runs;                                 /* 归并趟数 */
		double		npageaccesses;                            /* 页面访问次数 */

		/*
		 * CPU成本计算
		 *
		 * 假设大约需要 N log2 N 次比较
		 */
		startup_cost += comparison_cost * tuples * LOG2(tuples);

		/* 磁盘成本计算 */

		/* 计算logM(r) = log(r) / log(M) */
		if (nruns > mergeorder)
			log_runs = ceil(log(nruns) / log(mergeorder));
		else
			log_runs = 1.0;
		
		/* 计算总的页面访问次数：每趟归并需要2次读写 */
		npageaccesses = 2.0 * npages * log_runs;
		
		/* 假设3/4的访问是顺序的，1/4是随机的 */
		startup_cost += npageaccesses *
			(seq_page_cost * 0.75 + random_page_cost * 0.25);
	}
	else if (tuples > 2 * output_tuples || input_bytes > sort_mem_bytes)
	{
		/*
		 * 使用有界堆排序，仅在内存中保持K个元组，
		 * 总比较次数为 N log2 K；但常数因子比快速排序稍高。
		 * 调整使其在交叉点处成本曲线连续。
		 */
		startup_cost += comparison_cost * tuples * LOG2(2.0 * output_tuples);
	}
	else
	{
		/* 使用普通的快速排序处理所有输入元组 */
		startup_cost += comparison_cost * tuples * LOG2(tuples);
	}

	/*
	 * 为每个提取的元组收取少量费用（任意设置为等于操作符成本）。
	 * 我们不收取cpu_tuple_cost，因为Sort节点不进行条件检查或投影操作，
	 * 所以它的开销比大多数计划节点要小。
	 * 注意这里正确地使用tuples而不是output_tuples ---
	 * 上层的LIMIT会对运行成本进行比例分配，否则我们会重复计算LIMIT。
	 */
	run_cost += cpu_operator_cost * tuples;

	/* 设置路径的启动成本和总成本 */
	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}


/*
 * append_nonpartial_cost
 *	  Estimate the cost of the non-partial paths in a Parallel Append.
 *	  The non-partial paths are assumed to be the first "numpaths" paths
 *	  from the subpaths list, and to be in order of decreasing cost.
 */
static Cost
append_nonpartial_cost(List *subpaths, int numpaths, int parallel_workers)
{
	Cost	   *costarr;
	int			arrlen;
	ListCell   *l;
	ListCell   *cell;
	int			i;
	int			path_index;
	int			min_index;
	int			max_index;

	if (numpaths == 0)
		return 0;

	/*
	 * Array length is number of workers or number of relevants paths,
	 * whichever is less.
	 */
	arrlen = Min(parallel_workers, numpaths);
	costarr = (Cost *) palloc(sizeof(Cost) * arrlen);

	/* The first few paths will each be claimed by a different worker. */
	path_index = 0;
	foreach(cell, subpaths)
	{
		Path	   *subpath = (Path *) lfirst(cell);

		if (path_index == arrlen)
			break;
		costarr[path_index++] = subpath->total_cost;
	}

	/*
	 * Since subpaths are sorted by decreasing cost, the last one will have
	 * the minimum cost.
	 */
	min_index = arrlen - 1;

	/*
	 * For each of the remaining subpaths, add its cost to the array element
	 * with minimum cost.
	 */
	for_each_cell(l, cell)
	{
		Path	   *subpath = (Path *) lfirst(l);
		int			i;

		/* Consider only the non-partial paths */
		if (path_index++ == numpaths)
			break;

		costarr[min_index] += subpath->total_cost;

		/* Update the new min cost array index */
		for (min_index = i = 0; i < arrlen; i++)
		{
			if (costarr[i] < costarr[min_index])
				min_index = i;
		}
	}

	/* Return the highest cost from the array */
	for (max_index = i = 0; i < arrlen; i++)
	{
		if (costarr[i] > costarr[max_index])
			max_index = i;
	}

	return costarr[max_index];
}

/*
 * cost_append
 *	  Determines and returns the cost of an Append node.
 */
void
cost_append(AppendPath *apath)
{
	ListCell   *l;

	apath->path.startup_cost = 0;
	apath->path.total_cost = 0;
	apath->path.rows = 0;

	if (apath->subpaths == NIL)
		return;

	if (!apath->path.parallel_aware)
	{
		List	   *pathkeys = apath->path.pathkeys;

		if (pathkeys == NIL)
		{
			Path	   *subpath = (Path *) linitial(apath->subpaths);

			/*
			 * For an unordered, non-parallel-aware Append we take the startup
			 * cost as the startup cost of the first subpath.
			 */
			apath->path.startup_cost = subpath->startup_cost;

			/* Compute rows and costs as sums of subplan rows and costs. */
			foreach(l, apath->subpaths)
			{
				Path	   *subpath = (Path *) lfirst(l);

				apath->path.rows += subpath->rows;
				apath->path.total_cost += subpath->total_cost;
			}
		}
		else
		{
			/*
			 * For an ordered, non-parallel-aware Append we take the startup
			 * cost as the sum of the subpath startup costs.  This ensures
			 * that we don't underestimate the startup cost when a query's
			 * LIMIT is such that several of the children have to be run to
			 * satisfy it.  This might be overkill --- another plausible hack
			 * would be to take the Append's startup cost as the maximum of
			 * the child startup costs.  But we don't want to risk believing
			 * that an ORDER BY LIMIT query can be satisfied at small cost
			 * when the first child has small startup cost but later ones
			 * don't.  (If we had the ability to deal with nonlinear cost
			 * interpolation for partial retrievals, we would not need to be
			 * so conservative about this.)
			 *
			 * This case is also different from the above in that we have to
			 * account for possibly injecting sorts into subpaths that aren't
			 * natively ordered.
			 */
			foreach(l, apath->subpaths)
			{
				Path	   *subpath = (Path *) lfirst(l);
				Path		sort_path;	/* dummy for result of cost_sort */

				if (!pathkeys_contained_in(pathkeys, subpath->pathkeys))
				{
					/*
					 * We'll need to insert a Sort node, so include costs for
					 * that.  We can use the parent's LIMIT if any, since we
					 * certainly won't pull more than that many tuples from
					 * any child.
					 */
					cost_sort(&sort_path,
							  NULL, /* doesn't currently need root */
							  pathkeys,
							  subpath->total_cost,
							  subpath->rows,
							  subpath->pathtarget->width,
							  0.0,
							  work_mem,
							  apath->limit_tuples);
					subpath = &sort_path;
				}

				apath->path.rows += subpath->rows;
				apath->path.startup_cost += subpath->startup_cost;
				apath->path.total_cost += subpath->total_cost;
			}
		}
	}
	else						/* parallel-aware */
	{
		int			i = 0;
		double		parallel_divisor = get_parallel_divisor(&apath->path);

		/* Parallel-aware Append never produces ordered output. */
		Assert(apath->path.pathkeys == NIL);

		/* Calculate startup cost. */
		foreach(l, apath->subpaths)
		{
			Path	   *subpath = (Path *) lfirst(l);

			/*
			 * Append will start returning tuples when the child node having
			 * lowest startup cost is done setting up. We consider only the
			 * first few subplans that immediately get a worker assigned.
			 */
			if (i == 0)
				apath->path.startup_cost = subpath->startup_cost;
			else if (i < apath->path.parallel_workers)
				apath->path.startup_cost = Min(apath->path.startup_cost,
											   subpath->startup_cost);

			/*
			 * Apply parallel divisor to subpaths.  Scale the number of rows
			 * for each partial subpath based on the ratio of the parallel
			 * divisor originally used for the subpath to the one we adopted.
			 * Also add the cost of partial paths to the total cost, but
			 * ignore non-partial paths for now.
			 */
			if (i < apath->first_partial_path)
				apath->path.rows += subpath->rows / parallel_divisor;
			else
			{
				double		subpath_parallel_divisor;

				subpath_parallel_divisor = get_parallel_divisor(subpath);
				apath->path.rows += subpath->rows * (subpath_parallel_divisor /
													 parallel_divisor);
				apath->path.total_cost += subpath->total_cost;
			}

			apath->path.rows = clamp_row_est(apath->path.rows);

			i++;
		}

		/* Add cost for non-partial subpaths. */
		apath->path.total_cost +=
			append_nonpartial_cost(apath->subpaths,
								   apath->first_partial_path,
								   apath->path.parallel_workers);
	}

	/*
	 * Although Append does not do any selection or projection, it's not free;
	 * add a small per-tuple overhead.
	 */
	apath->path.total_cost +=
		cpu_tuple_cost * APPEND_CPU_COST_MULTIPLIER * apath->path.rows;
}

/*
 * cost_merge_append
 *	  Determines and returns the cost of a MergeAppend node.
 *
 * MergeAppend merges several pre-sorted input streams, using a heap that
 * at any given instant holds the next tuple from each stream.  If there
 * are N streams, we need about N*log2(N) tuple comparisons to construct
 * the heap at startup, and then for each output tuple, about log2(N)
 * comparisons to replace the top entry.
 *
 * (The effective value of N will drop once some of the input streams are
 * exhausted, but it seems unlikely to be worth trying to account for that.)
 *
 * The heap is never spilled to disk, since we assume N is not very large.
 * So this is much simpler than cost_sort.
 *
 * As in cost_sort, we charge two operator evals per tuple comparison.
 *
 * 'pathkeys' is a list of sort keys
 * 'n_streams' is the number of input streams
 * 'input_startup_cost' is the sum of the input streams' startup costs
 * 'input_total_cost' is the sum of the input streams' total costs
 * 'tuples' is the number of tuples in all the streams
 */
void
cost_merge_append(Path *path, PlannerInfo *root,
				  List *pathkeys, int n_streams,
				  Cost input_startup_cost, Cost input_total_cost,
				  double tuples)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	Cost		comparison_cost;
	double		N;
	double		logN;

	/*
	 * Avoid log(0)...
	 */
	N = (n_streams < 2) ? 2.0 : (double) n_streams;
	logN = LOG2(N);

	/* Assumed cost per tuple comparison */
	comparison_cost = 2.0 * cpu_operator_cost;

	/* Heap creation cost */
	startup_cost += comparison_cost * N * logN;

	/* Per-tuple heap maintenance cost */
	run_cost += tuples * comparison_cost * logN;

	/*
	 * Although MergeAppend does not do any selection or projection, it's not
	 * free; add a small per-tuple overhead.
	 */
	run_cost += cpu_tuple_cost * APPEND_CPU_COST_MULTIPLIER * tuples;

	path->startup_cost = startup_cost + input_startup_cost;
	path->total_cost = startup_cost + run_cost + input_total_cost;
}

/*
 * cost_material
 *	  Determines and returns the cost of materializing a relation, including
 *	  the cost of reading the input data.
 *
 * If the total volume of data to materialize exceeds work_mem, we will need
 * to write it to disk, so the cost is much higher in that case.
 *
 * Note that here we are estimating the costs for the first scan of the
 * relation, so the materialization is all overhead --- any savings will
 * occur only on rescan, which is estimated in cost_rescan.
 */
void
cost_material(Path *path,
			  Cost input_startup_cost, Cost input_total_cost,
			  double tuples, int width)
{
	Cost		startup_cost = input_startup_cost;
	Cost		run_cost = input_total_cost - input_startup_cost;
	double		nbytes = relation_byte_size(tuples, width);
	long		work_mem_bytes = work_mem * 1024L;

	path->rows = tuples;

	/*
	 * Whether spilling or not, charge 2x cpu_operator_cost per tuple to
	 * reflect bookkeeping overhead.  (This rate must be more than what
	 * cost_rescan charges for materialize, ie, cpu_operator_cost per tuple;
	 * if it is exactly the same then there will be a cost tie between
	 * nestloop with A outer, materialized B inner and nestloop with B outer,
	 * materialized A inner.  The extra cost ensures we'll prefer
	 * materializing the smaller rel.)	Note that this is normally a good deal
	 * less than cpu_tuple_cost; which is OK because a Material plan node
	 * doesn't do qual-checking or projection, so it's got less overhead than
	 * most plan nodes.
	 */
	run_cost += 2 * cpu_operator_cost * tuples;

	/*
	 * If we will spill to disk, charge at the rate of seq_page_cost per page.
	 * This cost is assumed to be evenly spread through the plan run phase,
	 * which isn't exactly accurate but our cost model doesn't allow for
	 * nonuniform costs within the run phase.
	 */
	if (nbytes > work_mem_bytes)
	{
		double		npages = ceil(nbytes / BLCKSZ);

		run_cost += seq_page_cost * npages;
	}

	path->startup_cost = startup_cost;
	path->total_cost = startup_cost + run_cost;
}

/*
 * cost_agg - 计算执行Agg计划节点的成本，包括其输入的成本
 *
 * 参数说明:
 * - path: 路径节点，用于存储成本估算结果
 * - root: 查询规划器信息
 * - aggstrategy: 聚合策略（AGG_PLAIN, AGG_SORTED, AGG_HASHED, AGG_MIXED）
 * - aggcosts: 聚合子句成本信息（如果没有实际聚合函数可以为NULL）
 * - numGroupCols: 分组列的数量
 * - numGroups: 分组的数量
 * - quals: HAVING子句条件
 * - input_startup_cost: 输入的启动成本
 * - input_total_cost: 输入的总成本
 * - input_tuples: 输入元组数量
 *
 * 特殊说明:
 * - 当没有实际聚合函数时（仅使用哈希聚合节点进行分组），aggcosts可以为NULL
 * - 当aggstrategy == AGG_SORTED时，调用者必须确保输入成本是针对适当排序的输入
 */
void
cost_agg(Path *path, PlannerInfo *root,
		 AggStrategy aggstrategy, const AggClauseCosts *aggcosts,
		 int numGroupCols, double numGroups,
		 List *quals,
		 Cost input_startup_cost, Cost input_total_cost,
		 double input_tuples)
{
	double		output_tuples;     /* 输出元组数 */
	Cost		startup_cost;      /* 启动成本 */
	Cost		total_cost;        /* 总成本 */
	AggClauseCosts dummy_aggcosts; /* 空的聚合成本结构 */

	/* 如果传入的aggcosts为NULL，使用全零的聚合成本 */
	if (aggcosts == NULL)
	{
		Assert(aggstrategy == AGG_HASHED);  /* 断言只能是哈希聚合 */
		MemSet(&dummy_aggcosts, 0, sizeof(AggClauseCosts));  /* 清零结构体 */
		aggcosts = &dummy_aggcosts;  /* 指向空结构体 */
	}

	/*
	 * 成本计算模型说明:
	 * 1. aggcosts中的transCost.per_tuple组件应该按每个输入元组收费一次，
	 *    对应于计算聚合转换函数及其输入表达式的成本。
	 * 2. finalCost.per_tuple组件按每个输出元组收费一次，
	 *    对应于计算最终函数的成本。
	 * 3. 启动成本当然只收取一次。
	 * 4. 如果进行分组，我们为每个分组列按每个输入元组额外收取cpu_operator_cost，
	 *    用于分组比较。
	 * 5. 如果不分组，我们将产生单个输出元组；否则每个分组产生一个元组。
	 *    我们为每个输出元组收取cpu_tuple_cost。
	 *
	 * 注意：在此成本模型中，AGG_SORTED和AGG_HASHED具有完全相同的总CPU成本，
	 * 但AGG_SORTED的启动成本更低。如果输入路径已经适当排序，应优先选择AGG_SORTED
	 * （因为它没有内存溢出的风险）。只要计算的总成本确实完全相等就会发生这种情况 ---
	 * 但如果存在舍入误差，我们可能会做出错误的选择。因此要确保下面的计算以相同的顺序
	 * 形成相同的中间值。
	 */
	 
	/* 处理普通聚合（无分组） */
	if (aggstrategy == AGG_PLAIN)
	{
		/* 启动成本等于输入总成本加上各种聚合成本 */
		startup_cost = input_total_cost;
		startup_cost += aggcosts->transCost.startup;                 /* 转换函数启动成本 */
		startup_cost += aggcosts->transCost.per_tuple * input_tuples; /* 转换函数每元组成本 */
		startup_cost += aggcosts->finalCost.startup;                 /* 最终函数启动成本 */
		startup_cost += aggcosts->finalCost.per_tuple;               /* 最终函数每元组成本 */
		/* 不进行分组，只产生一个输出元组 */
		total_cost = startup_cost + cpu_tuple_cost;  /* 加上输出元组成本 */
		output_tuples = 1;  /* 输出元组数为1 */
	}
	/* 处理排序聚合或混合聚合 */
	else if (aggstrategy == AGG_SORTED || aggstrategy == AGG_MIXED)
	{
		/* 这里我们可以即时交付输出 */
		startup_cost = input_startup_cost;  /* 启动成本为输入启动成本 */
		total_cost = input_total_cost;      /* 总成本为输入总成本 */
		
		/* 如果是混合聚合且哈希聚合被禁用，增加禁用成本 */
		if (aggstrategy == AGG_MIXED && !enable_hashagg)
		{
			startup_cost += disable_cost;
			total_cost += disable_cost;
		}
		
		/* 计算方式这样表述是为了与哈希聚合情况匹配，参见上面的注释 */
		total_cost += aggcosts->transCost.startup;                   /* 转换函数启动成本 */
		total_cost += aggcosts->transCost.per_tuple * input_tuples;  /* 转换函数每元组成本 */
		total_cost += (cpu_operator_cost * numGroupCols) * input_tuples; /* 分组比较成本 */
		total_cost += aggcosts->finalCost.startup;                   /* 最终函数启动成本 */
		total_cost += aggcosts->finalCost.per_tuple * numGroups;     /* 最终函数每组成本 */
		total_cost += cpu_tuple_cost * numGroups;                    /* 输出元组成本 */
		output_tuples = numGroups;  /* 输出元组数等于分组数 */
	}
	/* 处理哈希聚合 */
	else
	{
		/* 必须是AGG_HASHED */
		startup_cost = input_total_cost;  /* 启动成本为输入总成本 */
		
		/* 如果哈希聚合被禁用，增加禁用成本 */
		if (!enable_hashagg)
			startup_cost += disable_cost;
			
		startup_cost += aggcosts->transCost.startup;                  /* 转换函数启动成本 */
		startup_cost += aggcosts->transCost.per_tuple * input_tuples; /* 转换函数每元组成本 */
		startup_cost += (cpu_operator_cost * numGroupCols) * input_tuples; /* 分组比较成本 */
		startup_cost += aggcosts->finalCost.startup;                  /* 最终函数启动成本 */
		
		total_cost = startup_cost;                                    /* 总成本初始为启动成本 */
		total_cost += aggcosts->finalCost.per_tuple * numGroups;      /* 最终函数每组成本 */
		total_cost += cpu_tuple_cost * numGroups;                     /* 输出元组成本 */
		output_tuples = numGroups;  /* 输出元组数等于分组数 */
	}

	/*
	 * 如果存在条件（HAVING子句），计算其成本和选择性
	 */
	if (quals)
	{
		QualCost	qual_cost;  /* 条件成本结构 */

		/* 计算HAVING条件的成本 */
		cost_qual_eval(&qual_cost, quals, root);
		startup_cost += qual_cost.startup;  /* 增加条件启动成本 */
		total_cost += qual_cost.startup + output_tuples * qual_cost.per_tuple;  /* 增加条件总成本 */

		/* 根据条件选择性调整输出元组数 */
		output_tuples = clamp_row_est(output_tuples *
									  clauselist_selectivity(root,
															 quals,
															 0,
															 JOIN_INNER,
															 NULL));
	}

	/* 设置路径的各项成本和行数 */
	path->rows = output_tuples;        /* 设置输出行数 */
	path->startup_cost = startup_cost; /* 设置启动成本 */
	path->total_cost = total_cost;     /* 设置总成本 */
}


/*
 * cost_windowagg
 *		Determines and returns the cost of performing a WindowAgg plan node,
 *		including the cost of its input.
 *
 * Input is assumed already properly sorted.
 */
void
cost_windowagg(Path *path, PlannerInfo *root,
			   List *windowFuncs, int numPartCols, int numOrderCols,
			   Cost input_startup_cost, Cost input_total_cost,
			   double input_tuples)
{
	Cost		startup_cost;
	Cost		total_cost;
	ListCell   *lc;

	startup_cost = input_startup_cost;
	total_cost = input_total_cost;

	/*
	 * Window functions are assumed to cost their stated execution cost, plus
	 * the cost of evaluating their input expressions, per tuple.  Since they
	 * may in fact evaluate their inputs at multiple rows during each cycle,
	 * this could be a drastic underestimate; but without a way to know how
	 * many rows the window function will fetch, it's hard to do better.  In
	 * any case, it's a good estimate for all the built-in window functions,
	 * so we'll just do this for now.
	 */
	foreach(lc, windowFuncs)
	{
		WindowFunc *wfunc = lfirst_node(WindowFunc, lc);
		Cost		wfunccost;
		QualCost	argcosts;

		argcosts.startup = argcosts.per_tuple = 0;
		add_function_cost(root, wfunc->winfnoid, (Node *) wfunc,
						  &argcosts);
		startup_cost += argcosts.startup;
		wfunccost = argcosts.per_tuple;

		/* also add the input expressions' cost to per-input-row costs */
		cost_qual_eval_node(&argcosts, (Node *) wfunc->args, root);
		startup_cost += argcosts.startup;
		wfunccost += argcosts.per_tuple;

		/*
		 * Add the filter's cost to per-input-row costs.  XXX We should reduce
		 * input expression costs according to filter selectivity.
		 */
		cost_qual_eval_node(&argcosts, (Node *) wfunc->aggfilter, root);
		startup_cost += argcosts.startup;
		wfunccost += argcosts.per_tuple;

		total_cost += wfunccost * input_tuples;
	}

	/*
	 * We also charge cpu_operator_cost per grouping column per tuple for
	 * grouping comparisons, plus cpu_tuple_cost per tuple for general
	 * overhead.
	 *
	 * XXX this neglects costs of spooling the data to disk when it overflows
	 * work_mem.  Sooner or later that should get accounted for.
	 */
	total_cost += cpu_operator_cost * (numPartCols + numOrderCols) * input_tuples;
	total_cost += cpu_tuple_cost * input_tuples;

	path->rows = input_tuples;
	path->startup_cost = startup_cost;
	path->total_cost = total_cost;
}

/*
 * cost_group
 *		Determines and returns the cost of performing a Group plan node,
 *		including the cost of its input.
 *
 * Note: caller must ensure that input costs are for appropriately-sorted
 * input.
 */
void
cost_group(Path *path, PlannerInfo *root,
		   int numGroupCols, double numGroups,
		   List *quals,
		   Cost input_startup_cost, Cost input_total_cost,
		   double input_tuples)
{
	double		output_tuples;
	Cost		startup_cost;
	Cost		total_cost;

	output_tuples = numGroups;
	startup_cost = input_startup_cost;
	total_cost = input_total_cost;

	/*
	 * Charge one cpu_operator_cost per comparison per input tuple. We assume
	 * all columns get compared at most of the tuples.
	 */
	total_cost += cpu_operator_cost * input_tuples * numGroupCols;

	/*
	 * If there are quals (HAVING quals), account for their cost and
	 * selectivity.
	 */
	if (quals)
	{
		QualCost	qual_cost;

		cost_qual_eval(&qual_cost, quals, root);
		startup_cost += qual_cost.startup;
		total_cost += qual_cost.startup + output_tuples * qual_cost.per_tuple;

		output_tuples = clamp_row_est(output_tuples *
									  clauselist_selectivity(root,
															 quals,
															 0,
															 JOIN_INNER,
															 NULL));
	}

	path->rows = output_tuples;
	path->startup_cost = startup_cost;
	path->total_cost = total_cost;
}
/*
 * initial_cost_nestloop
 *	  初步估算嵌套循环连接路径的成本。
 *
 * 该函数需要快速给出路径的启动和总成本的下界估算。如果无法通过下界排除该路径，
 * 则会调用 final_cost_nestloop 进行最终估算。
 *
 * 该函数与 final_cost_nestloop 的具体分工是私有的，权衡了初步估算的速度和下界的紧密程度。
 * 这里选择不分析连接条件（join quals），因为这是计算中最耗时的部分。因此，CPU成本的考虑
 * 留到第二阶段；对于 SEMI/ANTI 连接，也推迟了内层路径运行成本的计算。
 *
 * 'workspace' 用于填充启动成本、总成本，以及可能用于 final_cost_nestloop 的其他数据
 * 'jointype' 是连接类型
 * 'outer_path' 是外层输入路径
 * 'inner_path' 是内层输入路径
 * 'extra' 包含连接的其他信息
 */
void
initial_cost_nestloop(PlannerInfo *root, JoinCostWorkspace *workspace,
					  JoinType jointype,
					  Path *outer_path, Path *inner_path,
					  JoinPathExtraData *extra)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	double		outer_path_rows = outer_path->rows;
	Cost		inner_rescan_start_cost;
	Cost		inner_rescan_total_cost;
	Cost		inner_run_cost;
	Cost		inner_rescan_run_cost;

	/* 估算重新扫描内层关系的成本 */
	cost_rescan(root, inner_path,
				&inner_rescan_start_cost,
				&inner_rescan_total_cost);

	/* 源数据成本 */

	/*
	 * 注意：显然，在开始返回元组之前，必须支付外层和内层路径的 startup_cost，
	 * 所以连接的启动成本是它们的和。内层路径的重新扫描启动成本会多次支付。
	 */
	startup_cost += outer_path->startup_cost + inner_path->startup_cost;
	run_cost += outer_path->total_cost - outer_path->startup_cost;
	if (outer_path_rows > 1)
		run_cost += (outer_path_rows - 1) * inner_rescan_start_cost;

	inner_run_cost = inner_path->total_cost - inner_path->startup_cost;
	inner_rescan_run_cost = inner_rescan_total_cost - inner_rescan_start_cost;

	if (jointype == JOIN_SEMI || jointype == JOIN_ANTI ||
		extra->inner_unique)
	{
		/*
		 * 对于 SEMI 或 ANTI 连接，或者内层关系唯一时，执行器在找到第一个匹配后会停止。
		 *
		 * 要获得合理的估算需要检查连接条件，这里选择推迟到 final_cost_nestloop 处理。
		 */

		/* 保存供 final_cost_nestloop 使用的私有数据 */
		workspace->inner_run_cost = inner_run_cost;
		workspace->inner_rescan_run_cost = inner_rescan_run_cost;
	}
	else
	{
		/* 普通情况：每个外层行都扫描整个内层关系 */
		run_cost += inner_run_cost;
		if (outer_path_rows > 1)
			run_cost += (outer_path_rows - 1) * inner_rescan_run_cost;
	}

	/* CPU 成本留到后续处理 */

	/* 公共结果字段 */
	workspace->startup_cost = startup_cost;
	workspace->total_cost = startup_cost + run_cost;
	/* 保存供 final_cost_nestloop 使用的私有数据 */
	workspace->run_cost = run_cost;
}


/*
 * final_cost_nestloop
 *    对嵌套循环连接路径的成本和结果大小进行最终估算
 *
 * 'path' 已经填充完成，但缺少rows和cost字段的值
 * 'workspace' 是initial_cost_nestloop函数的计算结果
 * 'extra' 包含关于连接的杂项信息
 *
 * 此函数完成嵌套循环连接路径的成本计算，主要考虑以下因素：
 * 1. 行数估计（包括并行执行情况下的缩放）
 * 2. 内关系扫描成本（尤其是早期终止情况下）
 * 3. 连接限定条件评估成本
 * 4. 目标列表评估成本
 *
 * 对于SEMI/ANTI连接和内关系唯一的情况有特殊处理，因为执行器会在找到第一个匹配项后停止
 */
void
final_cost_nestloop(PlannerInfo *root, NestPath *path,
                    JoinCostWorkspace *workspace,
                    JoinPathExtraData *extra)
{
    // 获取外层和内层路径信息
    Path   *outer_path = path->outerjoinpath;
    Path   *inner_path = path->innerjoinpath;
    
    // 提取外层和内层路径的行数估计
    double  outer_path_rows = outer_path->rows;
    double  inner_path_rows = inner_path->rows;
    
    // 从workspace获取初步计算的启动成本和运行成本
    Cost    startup_cost = workspace->startup_cost;
    Cost    run_cost = workspace->run_cost;
    
    // 局部变量声明
    Cost    cpu_per_tuple;       // 处理每个元组的CPU成本
    QualCost restrict_qual_cost; // 连接限定条件的评估成本
    double  ntuples;            // 处理的元组总数（非输出行数）

    /* 确保下面的计算中不会出现零或NaN行计数 */
    if (outer_path_rows <= 0 || isnan(outer_path_rows))
        outer_path_rows = 1;
    if (inner_path_rows <= 0 || isnan(inner_path_rows))
        inner_path_rows = 1;

    /* 为路径设置正确的行数估计 */
    // 如果路径是参数化的，使用参数信息中的行数
    if (path->path.param_info)
        path->path.rows = path->path.param_info->ppi_rows;
    else
        // 否则使用父关系中的行数估计
        path->path.rows = path->path.parent->rows;

    /* 对于部分并行路径，根据并行度调整行数估计 */
    if (path->path.parallel_workers > 0)
    {
        // 获取并行除数
        double  parallel_divisor = get_parallel_divisor(&path->path);
        
        // 调整行数估计，确保不会低于最小估计值
        path->path.rows = clamp_row_est(path->path.rows / parallel_divisor);
    }

    /*
     * 如果禁用嵌套循环连接，则增加禁用成本
     * 注意：不在初步估计中包含disable_cost，因为优化不应该针对禁用的情况
     */
    if (!enable_nestloop)
        startup_cost += disable_cost;

    /* 计算内关系源数据的成本（外层关系的成本已在初步估计中处理） */
    
    // 特殊处理：SEMI连接、ANTI连接或内关系唯一的情况
    // 这些情况下，执行器在找到第一个匹配项后就会停止内关系扫描
    if (path->jointype == JOIN_SEMI || path->jointype == JOIN_ANTI ||
        extra->inner_unique)
    {
        /*
         * SEMI或ANTI连接，或内关系已知唯一时，执行器会在找到第一个匹配项后停止
         */
        Cost    inner_run_cost = workspace->inner_run_cost;        // 内关系首次扫描成本
        Cost    inner_rescan_run_cost = workspace->inner_rescan_run_cost; // 内关系重复扫描成本
        double  outer_matched_rows;     // 在外层关系中有匹配的行数
        double  outer_unmatched_rows;   // 在外层关系中无匹配的行数
        Selectivity inner_scan_frac;    // 内关系扫描比例（早期终止时）

        /*
         * 对于有至少一个匹配项的外层行，如果匹配均匀分布，内扫描将在检查1/(match_count+1)比例
         * 的内关系行后停止。由于实际匹配可能不均匀，我们使用2.0的模糊因子调整这个比例。
         * 由于match_count至少为1，所以这里不需要将inner_scan_frac限制在1.0以内
         */
        // 计算匹配和未匹配的外层行数
        outer_matched_rows = rint(outer_path_rows * extra->semifactors.outer_match_frac);
        outer_unmatched_rows = outer_path_rows - outer_matched_rows;
        // 计算内关系扫描比例
        inner_scan_frac = 2.0 / (extra->semifactors.match_count + 1.0);

        /*
         * 计算处理的元组数量（非输出行数）。首先，计算成功匹配的外层行处理的元组数。
         */
        ntuples = outer_matched_rows * inner_path_rows * inner_scan_frac;

        /*
         * 现在需要估算扫描内关系的实际成本，由于早期停止，实际成本可能远低于N倍的inner_run_cost
         * 我们考虑两种情况：
         * 1. 如果内路径是索引扫描，且所有连接条件都用作索引条件，那么未匹配的外层行将导致
         *    一个返回零行的索引扫描，这可能非常便宜
         * 2. 否则，执行器将不得不扫描整个内关系以确定无匹配，成本较高
         */
        if (has_indexed_join_quals(path))
        {
            /*
             * 有索引支持的连接条件情况：
             * 成功匹配的外层行只需要扫描inner_scan_frac比例的内关系
             * 在此情况下，即使inner_run_cost大于inner_rescan_run_cost，我们也不需要
             * 收取完整的inner_run_cost，因为可以假设没有内扫描会扫描整个内关系
             */
            // 首次匹配项的内扫描成本
            run_cost += inner_run_cost * inner_scan_frac;
            // 额外匹配项的内扫描成本（使用重新扫描成本）
            if (outer_matched_rows > 1)
                run_cost += (outer_matched_rows - 1) * inner_rescan_run_cost * inner_scan_frac;

            /*
             * 添加未匹配外层行的内扫描执行成本
             * 估算为返回非空扫描第一个元组的成本
             * 由于已经使用了一次inner_run_cost，所以这些都被视为重新扫描
             */
            run_cost += outer_unmatched_rows *
                inner_rescan_run_cost / inner_path_rows;

            /*
             * 对于未匹配的行，我们不会评估任何限定条件，因此不将它们添加到ntuples中
             */
        }
        else
        {
            /*
             * 无索引支持的连接条件情况：
             * 这里的复杂因素是重新扫描可能比首次扫描便宜
             * 如果我们从不扫描到内关系的末尾，可能不需要支付完整的首次扫描运行成本
             * 但很难估计是否会发生这种情况（如果有任何未匹配的外层行，就必然会有完整扫描！）
             * 因此采取保守态度，始终收取一次完整的首次扫描成本
             * 我们认为这个收费对应于第一个未匹配的外层行，如果估计中没有，则对应第一个匹配的行
             */

            /* 首先，计算所有未匹配连接元组被处理的数量 */
            ntuples += outer_unmatched_rows * inner_path_rows;

            /* 添加强制完整扫描的成本，并减少相应计数 */
            run_cost += inner_run_cost;
            if (outer_unmatched_rows >= 1)
                outer_unmatched_rows -= 1;  // 归因于未匹配行
            else
                outer_matched_rows -= 1;    // 归因于匹配行

            /* 添加有匹配的额外外层元组的内扫描运行成本 */
            if (outer_matched_rows > 0)
                run_cost += outer_matched_rows * inner_rescan_run_cost * inner_scan_frac;

            /* 添加无匹配的额外外层元组的内扫描运行成本 */
            if (outer_unmatched_rows > 0)
                run_cost += outer_unmatched_rows * inner_rescan_run_cost;
        }
    }
    else
    {
        /* 普通情况：源成本已包含在初步估计中 */

        /* 计算处理的元组数量（非输出行数） */
        ntuples = outer_path_rows * inner_path_rows;
    }

    /* 计算CPU成本 */
    // 估算连接限定条件的评估成本
    cost_qual_eval(&restrict_qual_cost, path->joinrestrictinfo, root);
    // 添加限定条件的启动成本
    startup_cost += restrict_qual_cost.startup;
    // 计算每个元组的CPU处理成本
    cpu_per_tuple = cpu_tuple_cost + restrict_qual_cost.per_tuple;
    // 添加所有处理元组的CPU运行成本
    run_cost += cpu_per_tuple * ntuples;

    /* 目标列表评估成本按输出行数计算，而非扫描的元组数 */
    startup_cost += path->path.pathtarget->cost.startup;
    run_cost += path->path.pathtarget->cost.per_tuple * path->path.rows;

    // 设置路径的最终成本信息
    path->path.startup_cost = startup_cost;      // 启动成本
    path->path.total_cost = startup_cost + run_cost;  // 总成本
}


/*
 * initial_cost_mergejoin
 *	  Preliminary estimate of the cost of a mergejoin path.
 *
 * This must quickly produce lower-bound estimates of the path's startup and
 * total costs.  If we are unable to eliminate the proposed path from
 * consideration using the lower bounds, final_cost_mergejoin will be called
 * to obtain the final estimates.
 *
 * The exact division of labor between this function and final_cost_mergejoin
 * is private to them, and represents a tradeoff between speed of the initial
 * estimate and getting a tight lower bound.  We choose to not examine the
 * join quals here, except for obtaining the scan selectivity estimate which
 * is really essential (but fortunately, use of caching keeps the cost of
 * getting that down to something reasonable).
 * We also assume that cost_sort is cheap enough to use here.
 *
 * 'workspace' is to be filled with startup_cost, total_cost, and perhaps
 *		other data to be used by final_cost_mergejoin
 * 'jointype' is the type of join to be performed
 * 'mergeclauses' is the list of joinclauses to be used as merge clauses
 * 'outer_path' is the outer input to the join
 * 'inner_path' is the inner input to the join
 * 'outersortkeys' is the list of sort keys for the outer path
 * 'innersortkeys' is the list of sort keys for the inner path
 * 'extra' contains miscellaneous information about the join
 *
 * Note: outersortkeys and innersortkeys should be NIL if no explicit
 * sort is needed because the respective source path is already ordered.
 */
void
initial_cost_mergejoin(PlannerInfo *root, JoinCostWorkspace *workspace,
					   JoinType jointype,
					   List *mergeclauses,
					   Path *outer_path, Path *inner_path,
					   List *outersortkeys, List *innersortkeys,
					   JoinPathExtraData *extra)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	double		outer_path_rows = outer_path->rows;
	double		inner_path_rows = inner_path->rows;
	Cost		inner_run_cost;
	double		outer_rows,
				inner_rows,
				outer_skip_rows,
				inner_skip_rows;
	Selectivity outerstartsel,
				outerendsel,
				innerstartsel,
				innerendsel;
	Path		sort_path;		/* dummy for result of cost_sort */

	/* Protect some assumptions below that rowcounts aren't zero or NaN */
	if (outer_path_rows <= 0 || isnan(outer_path_rows))
		outer_path_rows = 1;
	if (inner_path_rows <= 0 || isnan(inner_path_rows))
		inner_path_rows = 1;

	/*
	 * A merge join will stop as soon as it exhausts either input stream
	 * (unless it's an outer join, in which case the outer side has to be
	 * scanned all the way anyway).  Estimate fraction of the left and right
	 * inputs that will actually need to be scanned.  Likewise, we can
	 * estimate the number of rows that will be skipped before the first join
	 * pair is found, which should be factored into startup cost. We use only
	 * the first (most significant) merge clause for this purpose. Since
	 * mergejoinscansel() is a fairly expensive computation, we cache the
	 * results in the merge clause RestrictInfo.
	 */
	if (mergeclauses && jointype != JOIN_FULL)
	{
		RestrictInfo *firstclause = (RestrictInfo *) linitial(mergeclauses);
		List	   *opathkeys;
		List	   *ipathkeys;
		PathKey    *opathkey;
		PathKey    *ipathkey;
		MergeScanSelCache *cache;

		/* Get the input pathkeys to determine the sort-order details */
		opathkeys = outersortkeys ? outersortkeys : outer_path->pathkeys;
		ipathkeys = innersortkeys ? innersortkeys : inner_path->pathkeys;
		Assert(opathkeys);
		Assert(ipathkeys);
		opathkey = (PathKey *) linitial(opathkeys);
		ipathkey = (PathKey *) linitial(ipathkeys);
		/* debugging check */
		if (opathkey->pk_opfamily != ipathkey->pk_opfamily ||
			opathkey->pk_eclass->ec_collation != ipathkey->pk_eclass->ec_collation ||
			opathkey->pk_strategy != ipathkey->pk_strategy ||
			opathkey->pk_nulls_first != ipathkey->pk_nulls_first)
			elog(ERROR, "left and right pathkeys do not match in mergejoin");

		/* Get the selectivity with caching */
		cache = cached_scansel(root, firstclause, opathkey);

		if (bms_is_subset(firstclause->left_relids,
						  outer_path->parent->relids))
		{
			/* left side of clause is outer */
			outerstartsel = cache->leftstartsel;
			outerendsel = cache->leftendsel;
			innerstartsel = cache->rightstartsel;
			innerendsel = cache->rightendsel;
		}
		else
		{
			/* left side of clause is inner */
			outerstartsel = cache->rightstartsel;
			outerendsel = cache->rightendsel;
			innerstartsel = cache->leftstartsel;
			innerendsel = cache->leftendsel;
		}
		if (jointype == JOIN_LEFT ||
			jointype == JOIN_ANTI)
		{
			outerstartsel = 0.0;
			outerendsel = 1.0;
		}
		else if (jointype == JOIN_RIGHT)
		{
			innerstartsel = 0.0;
			innerendsel = 1.0;
		}
	}
	else
	{
		/* cope with clauseless or full mergejoin */
		outerstartsel = innerstartsel = 0.0;
		outerendsel = innerendsel = 1.0;
	}

	/*
	 * Convert selectivities to row counts.  We force outer_rows and
	 * inner_rows to be at least 1, but the skip_rows estimates can be zero.
	 */
	outer_skip_rows = rint(outer_path_rows * outerstartsel);
	inner_skip_rows = rint(inner_path_rows * innerstartsel);
	outer_rows = clamp_row_est(outer_path_rows * outerendsel);
	inner_rows = clamp_row_est(inner_path_rows * innerendsel);

	/* skip rows can become NaN when path rows has become infinite */
	Assert(outer_skip_rows <= outer_rows || isnan(outer_skip_rows));
	Assert(inner_skip_rows <= inner_rows || isnan(inner_skip_rows));

	/*
	 * Readjust scan selectivities to account for above rounding.  This is
	 * normally an insignificant effect, but when there are only a few rows in
	 * the inputs, failing to do this makes for a large percentage error.
	 */
	outerstartsel = outer_skip_rows / outer_path_rows;
	innerstartsel = inner_skip_rows / inner_path_rows;
	outerendsel = outer_rows / outer_path_rows;
	innerendsel = inner_rows / inner_path_rows;

	/* start sel can become NaN when path rows has become infinite */
	Assert(outerstartsel <= outerendsel || isnan(outerstartsel));
	Assert(innerstartsel <= innerendsel || isnan(innerstartsel));

	/* cost of source data */

	if (outersortkeys)			/* do we need to sort outer? */
	{
		cost_sort(&sort_path,
				  root,
				  outersortkeys,
				  outer_path->total_cost,
				  outer_path_rows,
				  outer_path->pathtarget->width,
				  0.0,
				  work_mem,
				  -1.0);
		startup_cost += sort_path.startup_cost;
		startup_cost += (sort_path.total_cost - sort_path.startup_cost)
			* outerstartsel;
		run_cost += (sort_path.total_cost - sort_path.startup_cost)
			* (outerendsel - outerstartsel);
	}
	else
	{
		startup_cost += outer_path->startup_cost;
		startup_cost += (outer_path->total_cost - outer_path->startup_cost)
			* outerstartsel;
		run_cost += (outer_path->total_cost - outer_path->startup_cost)
			* (outerendsel - outerstartsel);
	}

	if (innersortkeys)			/* do we need to sort inner? */
	{
		cost_sort(&sort_path,
				  root,
				  innersortkeys,
				  inner_path->total_cost,
				  inner_path_rows,
				  inner_path->pathtarget->width,
				  0.0,
				  work_mem,
				  -1.0);
		startup_cost += sort_path.startup_cost;
		startup_cost += (sort_path.total_cost - sort_path.startup_cost)
			* innerstartsel;
		inner_run_cost = (sort_path.total_cost - sort_path.startup_cost)
			* (innerendsel - innerstartsel);
	}
	else
	{
		startup_cost += inner_path->startup_cost;
		startup_cost += (inner_path->total_cost - inner_path->startup_cost)
			* innerstartsel;
		inner_run_cost = (inner_path->total_cost - inner_path->startup_cost)
			* (innerendsel - innerstartsel);
	}

	/*
	 * We can't yet determine whether rescanning occurs, or whether
	 * materialization of the inner input should be done.  The minimum
	 * possible inner input cost, regardless of rescan and materialization
	 * considerations, is inner_run_cost.  We include that in
	 * workspace->total_cost, but not yet in run_cost.
	 */

	/* CPU costs left for later */

	/* Public result fields */
	workspace->startup_cost = startup_cost;
	workspace->total_cost = startup_cost + run_cost + inner_run_cost;
	/* Save private data for final_cost_mergejoin */
	workspace->run_cost = run_cost;
	workspace->inner_run_cost = inner_run_cost;
	workspace->outer_rows = outer_rows;
	workspace->inner_rows = inner_rows;
	workspace->outer_skip_rows = outer_skip_rows;
	workspace->inner_skip_rows = inner_skip_rows;
}

/*
 * final_cost_mergejoin
 *    MergeJoin 路径的最终成本和结果大小估算
 *
 * 函数功能:
 * 与其他成本计算函数不同，该函数会做出两个重要决策：
 * 1. 执行器是否需要执行标记/恢复(mark/restore)操作
 * 2. 是否应该物化(materialize)内路径
 *
 * 设计考虑:
 * 虽然在逻辑上更清晰的做法是构建测试这些替代方案的独立路径，但这需要重复大部分成本计算，
 * 而这些计算并不便宜。由于选择不会影响输出路径键或启动成本，只影响总成本，且不可能需要保留多个路径，
 * 因此在此处做决策并将结果记录在路径的skip_mark_restore和materialize_inner字段中是最佳选择。
 *
 * 标记/恢复优化:
 * 通常需要标记/恢复开销，但如果知道执行器只需要为每个外元组找到一个匹配项，且合并子句足以标识匹配项，
 * 则可以跳过标记/恢复开销。
 *
 * 内路径物化:
 * 如果需要标记/恢复且内路径不支持标记/恢复，或者使用中间物化节点处理标记/恢复更便宜时，会物化内路径。
 *
 * 参数说明:
 * - root: 查询规划器信息结构
 * - path: 合并连接路径结构(已填充除行数和成本字段以及skip_mark_restore和materialize_inner外的所有信息)
 * - workspace: 来自initial_cost_mergejoin的结果
 * - extra: 关于连接的杂项信息
 *
 * 返回值:
 * 无返回值，通过修改path参数中的字段来返回结果
 */
void
final_cost_mergejoin(PlannerInfo *root, MergePath *path,
                     JoinCostWorkspace *workspace,
                     JoinPathExtraData *extra)
{
    /* 提取路径相关信息 */
    Path       *outer_path = path->jpath.outerjoinpath;     /* 外连接路径 */
    Path       *inner_path = path->jpath.innerjoinpath;     /* 内连接路径 */
    double      inner_path_rows = inner_path->rows;         /* 内路径行数 */
    List       *mergeclauses = path->path_mergeclauses;     /* 合并连接子句 */
    List       *innersortkeys = path->innersortkeys;        /* 内路径排序键 */
    
    /* 从工作空间提取成本和行数估计 */
    Cost        startup_cost = workspace->startup_cost;     /* 启动成本 */
    Cost        run_cost = workspace->run_cost;             /* 运行成本 */
    Cost        inner_run_cost = workspace->inner_run_cost; /* 内路径运行成本 */
    double      outer_rows = workspace->outer_rows;         /* 外路径行数 */
    double      inner_rows = workspace->inner_rows;         /* 内路径行数 */
    double      outer_skip_rows = workspace->outer_skip_rows;   /* 外路径跳过行数 */
    double      inner_skip_rows = workspace->inner_skip_rows;   /* 内路径跳过行数 */
    
    /* 成本计算相关变量 */
    Cost        cpu_per_tuple,              /* 每元组CPU成本 */
                bare_inner_cost,            /* 无物化的内路径成本 */
                mat_inner_cost;             /* 物化的内路径成本 */
    QualCost    merge_qual_cost;            /* 合并条件成本 */
    QualCost    qp_qual_cost;               /* 其他限制条件成本 */
    double      mergejointuples,            /* 通过合并条件的元组数 */
                rescannedtuples;            /* 重新扫描的元组数 */
    double      rescanratio;                /* 重新扫描比率 */

    /* 保护下面关于行数不为零或NaN的假设 */
    if (inner_path_rows <= 0 || isnan(inner_path_rows))
        inner_path_rows = 1;

    /* 标记路径的正确行数估计 */
    if (path->jpath.path.param_info)
        path->jpath.path.rows = path->jpath.path.param_info->ppi_rows;
    else
        path->jpath.path.rows = path->jpath.path.parent->rows;

    /* 对于并行路径，缩放行数估计 */
    if (path->jpath.path.parallel_workers > 0)
    {
        double      parallel_divisor = get_parallel_divisor(&path->jpath.path);

        path->jpath.path.rows =
            clamp_row_est(path->jpath.path.rows / parallel_divisor);
    }

    /*
     * 我们可以在初步估计中包含disable_cost，但这相当于针对连接方法被禁用的情况进行优化，
     * 这似乎不是正确的做法。
     */
    if (!enable_mergejoin)
        startup_cost += disable_cost;

    /*
     * 分别计算合并条件(mergequals)和其他限制条件(qpquals)的成本
     */
    cost_qual_eval(&merge_qual_cost, mergeclauses, root);
    cost_qual_eval(&qp_qual_cost, path->jpath.joinrestrictinfo, root);
    /* 调整qp_qual_cost，减去已在merge_qual_cost中计算的部分 */
    qp_qual_cost.startup -= merge_qual_cost.startup;
    qp_qual_cost.per_tuple -= merge_qual_cost.per_tuple;

    /*
     * 对于SEMI或ANTI连接，或者如果内关系已知唯一，执行器在找到第一个匹配后就会停止扫描。
     * 当所有连接子句都是合并子句时，这意味着我们永远不需要回退合并，因此可以跳过标记/恢复开销。
     */
    if ((path->jpath.jointype == JOIN_SEMI ||
         path->jpath.jointype == JOIN_ANTI ||
         extra->inner_unique) &&
        (list_length(path->jpath.joinrestrictinfo) ==
         list_length(path->path_mergeclauses)))
        path->skip_mark_restore = true;  /* 跳过标记/恢复 */
    else
        path->skip_mark_restore = false; /* 需要标记/恢复 */

    /*
     * 获取通过合并条件的大致元组数。这里使用approx_tuple_count是因为我们需要用JOIN_INNER语义完成的估计。
     */
    mergejointuples = approx_tuple_count(root, &path->jpath, mergeclauses);

    /*
     * 当外关系中有相等的合并键时，合并连接必须重新扫描内关系中的任何匹配元组。
     * 这意味着重新获取内元组；我们必须估计这种情况发生的频率。
     *
     * 对于常规的内连接和外连接，重新获取次数可以大致估计为合并连接输出大小减去内关系大小。
     * 假设不同的键值为1, 2, ...，并将在外关系中每个键的值的数量表示为m1, m2, ...；
     * 在内关系中表示为n1, n2, ...。那么我们有：
     *
     * 连接大小 = m1 * n1 + m2 * n2 + ...
     *
     * 重新扫描的元组数 = (m1 - 1) * n1 + (m2 - 1) * n2 + ... = m1 * n1 + m2 * n2 + ... - (n1 + n2 + ...)
     *                  = 连接大小 - 内关系大小
     *
     * 这个公式对外元组没有内匹配(nk = 0)的情况是正确的，但对内元组没有外匹配(mk = 0)的情况不正确；
     * 我们实际上是从重新扫描的元组数中减去了这些，但我们不应该这样做。
     * 在不进行昂贵的选择性计算的情况下，我们能做得更好吗？
     *
     * 如果我们使用的是唯一化的外输入，或者我们知道根本不需要标记/恢复，那么整个问题就不存在了。
     */
    if (IsA(outer_path, UniquePath) || path->skip_mark_restore)
        rescannedtuples = 0;  /* 无需重新扫描 */
    else
    {
        rescannedtuples = mergejointuples - inner_path_rows;
        /* 必须限制，因为可能存在低估 */
        if (rescannedtuples < 0)
            rescannedtuples = 0;
    }

    /*
     * 我们将按此比例增加各种成本以考虑重新扫描。注意，这需要乘以内行数或与我们将扫描的内关系部分相关的其他数字。
     */
    rescanratio = 1.0 + (rescannedtuples / inner_rows);

    /*
     * 决定是否要物化内输入以保护其免受标记/恢复和执行重新获取的影响。
     * 我们对常规重新获取的成本模型是，重新获取的成本与原始获取相同，这可能是一个高估；
     * 但另一方面我们忽略了标记/恢复的簿记成本。不清楚是否值得开发更精细的模型。
     * 因此我们只需要按rescanratio增加内运行成本。
     */
    bare_inner_cost = inner_run_cost * rescanratio;

    /*
     * 当我们在中间插入Material节点时，假定重新获取成本仅为每个元组cpu_operator_cost，
     * 与底层计划的成本无关；我们还会对每次原始获取额外收取cpu_operator_cost。
     * 注意，我们假设物化节点永远不会溢出到磁盘，因为它只需要记住回到最后一个标记的元组。
     * （如果有大量重复项，我们的其他成本因素会使路径变得非常昂贵，以至于可能根本不会被选择。）
     * 因此我们在这里不使用cost_rescan。
     *
     * 注意：保持此估计与create_mergejoin_plan对标记生成的Material节点的标签同步。
     */
    mat_inner_cost = inner_run_cost +
        cpu_operator_cost * inner_rows * rescanratio;

    /*
     * 如果我们根本不需要标记/恢复，我们就不需要物化。
     */
    if (path->skip_mark_restore)
        path->materialize_inner = false;  /* 不需要物化 */

    /*
     * 如果物化看起来更便宜，则优先选择物化，除非用户要求禁止物化。
     */
    else if (enable_material && mat_inner_cost < bare_inner_cost)
        path->materialize_inner = true;   /* 选择物化 */

    /*
     * 即使物化看起来不便宜，如果我们直接使用内路径（不排序）且它不支持标记/恢复，我们也必须这样做。
     *
     * 由于内侧必须有序，而只有排序和索引扫描才能首先创建顺序，而且它们都支持标记/恢复，
     * 你可能会认为没有问题——但你就错了。嵌套循环和合并连接可以保留其输入的顺序，
     * 因此它们可以被选为合并连接的输入，而它们目前不支持标记/恢复。
     *
     * 我们在这里不测试enable_material的值，因为在这种情况下物化对于正确性是必需的，
     * 关闭物化并不能让我们提供无效的计划。
     */
    else if (innersortkeys == NIL &&
             !ExecSupportsMarkRestore(inner_path))
        path->materialize_inner = true;   /* 必须物化 */

    /*
     * 此外，如果内路径需要排序且预计排序会溢出到磁盘，则强制物化。
     * 这是因为如果不支持标记/恢复，最终的合并过程可以在线完成。
     * 不过，我们不会尝试为此考虑调整成本估计。
     *
     * 由于在这种情况下物化是一种性能优化而不是正确性所必需的，如果enable_material关闭则跳过它。
     */
    else if (enable_material && innersortkeys != NIL &&
             relation_byte_size(inner_path_rows,
                                inner_path->pathtarget->width) >
             (work_mem * 1024L))
        path->materialize_inner = true;   /* 强制物化 */
    else
        path->materialize_inner = false;  /* 不物化 */

    /* 根据选择的情况收取相应的增量成本 */
    if (path->materialize_inner)
        run_cost += mat_inner_cost;       /* 加上物化成本 */
    else
        run_cost += bare_inner_cost;      /* 加上无物化成本 */

    /* CPU成本计算 */

    /*
     * 所需的元组比较数量大约是外行数加上内行数再加上重新扫描的元组数（我们可以改进这个估计吗？）。
     * 在每一个比较中，我们需要评估合并连接条件。
     */
    startup_cost += merge_qual_cost.startup;
    startup_cost += merge_qual_cost.per_tuple *
        (outer_skip_rows + inner_skip_rows * rescanratio);
    run_cost += merge_qual_cost.per_tuple *
        ((outer_rows - outer_skip_rows) +
         (inner_rows - inner_skip_rows) * rescanratio);

    /*
     * 对于通过合并连接本身的每个元组，我们收取cpu_tuple_cost加上在连接处应用的附加限制子句的评估成本。
     * （这是悲观的，因为并非所有的条件都可能在每个元组上都被评估。）
     *
     * 注意：我们可以在此处调整SEMI/ANTI连接跳过某些条件评估的情况，但这可能不值得麻烦。
     */
    startup_cost += qp_qual_cost.startup;
    cpu_per_tuple = cpu_tuple_cost + qp_qual_cost.per_tuple;
    run_cost += cpu_per_tuple * mergejointuples;

    /* 目标列表评估成本按输出行支付，而不是按扫描的元组数支付 */
    startup_cost += path->jpath.path.pathtarget->cost.startup;
    run_cost += path->jpath.path.pathtarget->cost.per_tuple * path->jpath.path.rows;

    /* 设置最终的启动成本和总成本 */
    path->jpath.path.startup_cost = startup_cost;
    path->jpath.path.total_cost = startup_cost + run_cost;
}


/*
 * run mergejoinscansel() with caching
 */
static MergeScanSelCache *
cached_scansel(PlannerInfo *root, RestrictInfo *rinfo, PathKey *pathkey)
{
	MergeScanSelCache *cache;
	ListCell   *lc;
	Selectivity leftstartsel,
				leftendsel,
				rightstartsel,
				rightendsel;
	MemoryContext oldcontext;

	/* Do we have this result already? */
	foreach(lc, rinfo->scansel_cache)
	{
		cache = (MergeScanSelCache *) lfirst(lc);
		if (cache->opfamily == pathkey->pk_opfamily &&
			cache->collation == pathkey->pk_eclass->ec_collation &&
			cache->strategy == pathkey->pk_strategy &&
			cache->nulls_first == pathkey->pk_nulls_first)
			return cache;
	}

	/* Nope, do the computation */
	mergejoinscansel(root,
					 (Node *) rinfo->clause,
					 pathkey->pk_opfamily,
					 pathkey->pk_strategy,
					 pathkey->pk_nulls_first,
					 &leftstartsel,
					 &leftendsel,
					 &rightstartsel,
					 &rightendsel);

	/* Cache the result in suitably long-lived workspace */
	oldcontext = MemoryContextSwitchTo(root->planner_cxt);

	cache = (MergeScanSelCache *) palloc(sizeof(MergeScanSelCache));
	cache->opfamily = pathkey->pk_opfamily;
	cache->collation = pathkey->pk_eclass->ec_collation;
	cache->strategy = pathkey->pk_strategy;
	cache->nulls_first = pathkey->pk_nulls_first;
	cache->leftstartsel = leftstartsel;
	cache->leftendsel = leftendsel;
	cache->rightstartsel = rightstartsel;
	cache->rightendsel = rightendsel;

	rinfo->scansel_cache = lappend(rinfo->scansel_cache, cache);

	MemoryContextSwitchTo(oldcontext);

	return cache;
}

/*
 * initial_cost_hashjoin
 *    对哈希连接路径进行初步成本估算。
 * 
 * 此函数必须快速生成路径的启动成本和总成本的下界估计。如果我们无法使用这些下界
 * 排除提议的路径，那么final_cost_hashjoin将被调用来获取最终估计值。
 * 
 * 此函数与final_cost_hashjoin之间的具体分工是它们私有的，代表了初始估计速度
 * 和获得紧密下界之间的权衡。我们选择在这里不检查连接条件（除了计算哈希子句的数量），
 * 因此在CPU成本方面不能做太多工作。我们假设ExecChooseHashTableSize在这里使用
 * 是足够便宜的。
 * 
 * 参数说明：
 *    root - 规划器的全局信息结构指针
 *    workspace - 将被填充启动成本、总成本和可能的其他数据，供final_cost_hashjoin使用
 *    jointype - 要执行的连接类型
 *    hashclauses - 用作哈希子句的连接条件列表
 *    outer_path - 连接的外层输入路径
 *    inner_path - 连接的内层输入路径
 *    extra - 包含有关连接的杂项信息
 *    parallel_hash - 指示inner_path是部分路径，并且将并行构建共享哈希表
 */
void
initial_cost_hashjoin(PlannerInfo *root, JoinCostWorkspace *workspace,
                     JoinType jointype,
                     List *hashclauses,
                     Path *outer_path, Path *inner_path,
                     JoinPathExtraData *extra,
                     bool parallel_hash)
{
    Cost        startup_cost = 0;            /* 启动成本初始化 */
    Cost        run_cost = 0;                /* 运行成本初始化 */
    double      outer_path_rows = outer_path->rows;  /* 外层路径返回的行数 */
    double      inner_path_rows = inner_path->rows;  /* 内层路径返回的行数 */
    double      inner_path_rows_total = inner_path_rows; /* 内层路径总行数（考虑并行情况） */
    int         num_hashclauses = list_length(hashclauses); /* 哈希子句数量 */
    int         numbuckets;                  /* 哈希表桶数量 */
    int         numbatches;                  /* 批处理数量 */
    int         num_skew_mcvs;               /* 倾斜优化MCV(最常见值)数量 */
    size_t      space_allowed;               /* 允许的空间（未使用） */

    /* 源数据的成本计算 */
    startup_cost += outer_path->startup_cost;  /* 外层路径启动成本 */
    run_cost += outer_path->total_cost - outer_path->startup_cost; /* 外层路径运行成本 */
    startup_cost += inner_path->total_cost;  /* 内层路径总成本（作为哈希表构建成本，属于启动成本） */

    /*
     * 计算哈希函数的成本：必须为每个输入元组执行一次。
     * 我们为每个列的哈希函数收取一个cpu_operator_cost。
     * 此外，为每个内关系行增加一个cpu_tuple_cost，以模拟将行插入哈希表的成本。
     * 
     * 注意：当哈希子句比单个操作符更复杂时，我们应该为左侧或右侧的额外评估
     * 成本收费，但目前认为这不值得为此付出额外的工作。
     */
    startup_cost += (cpu_operator_cost * num_hashclauses + cpu_tuple_cost)
        * inner_path_rows;  /* 内关系哈希计算和插入哈希表的成本 */
    run_cost += cpu_operator_cost * num_hashclauses * outer_path_rows; /* 外关系哈希计算成本 */

    /*
     * 如果这是并行哈希构建，则我们当前的inner_rows_total值仅指每个参与者返回的行数。
     * 对于共享哈希表大小估计，我们需要总数，因此需要撤销除法操作。
     */
    if (parallel_hash)
        inner_path_rows_total *= get_parallel_divisor(inner_path);

    /*
     * 获取执行器将为内关系使用的哈希表大小。
     * 
     * 注意：目前，我们总是假设将执行倾斜优化。只要SKEW_WORK_MEM_PERCENT很小，
     * 就不值得确定这一点。
     * 
     * 注意：将来可能有兴趣尝试在成本估计中考虑倾斜优化，但目前我们没有这样做。
     */
    ExecChooseHashTableSize(inner_path_rows_total,
                           inner_path->pathtarget->width,  /* 内关系元组宽度 */
                           true,    /* useskew - 使用倾斜优化 */
                           parallel_hash, /* try_combined_work_mem - 尝试组合工作内存 */
                           outer_path->parallel_workers,  /* 并行工作线程数 */
                           &space_allowed,
                           &numbuckets,  /* 输出参数：哈希表桶数量 */
                           &numbatches,  /* 输出参数：批处理数量 */
                           &num_skew_mcvs);  /* 输出参数：倾斜优化MCV数量 */

    /*
     * 如果内关系太大，那么我们需要对连接进行"批处理"，这意味着需要额外的
     * 将大部分元组写入磁盘并重新读取一次。
     * 每页收取seq_page_cost，因为I/O应该是良好且顺序的。
     * 写入内关系计为启动成本，其余的计为运行成本。
     */
    if (numbatches > 1)
    {
        double      outerpages = page_size(outer_path_rows,  /* 计算外关系页数 */
                                          outer_path->pathtarget->width);
        double      innerpages = page_size(inner_path_rows,  /* 计算内关系页数 */
                                          inner_path->pathtarget->width);

        startup_cost += seq_page_cost * innerpages;  /* 内关系批处理写入成本 */
        run_cost += seq_page_cost * (innerpages + 2 * outerpages);  /* 内关系读取和外关系读写成本 */
    }

    /* 剩余的CPU成本留待后面处理 */

    /* 公共结果字段设置 */
    workspace->startup_cost = startup_cost;  /* 设置启动成本 */
    workspace->total_cost = startup_cost + run_cost;  /* 设置总成本 */
    /* 保存私有数据供final_cost_hashjoin使用 */
    workspace->run_cost = run_cost;  /* 保存运行成本 */
    workspace->numbuckets = numbuckets;  /* 保存哈希表桶数量 */
    workspace->numbatches = numbatches;  /* 保存批处理数量 */
    workspace->inner_rows_total = inner_path_rows_total;  /* 保存内层总行数 */
}


/*
 * final_cost_hashjoin
 *	  Final estimate of the cost and result size of a hashjoin path.
 *
 * Note: the numbatches estimate is also saved into 'path' for use later
 *
 * 'path' is already filled in except for the rows and cost fields and
 *		num_batches
 * 'workspace' is the result from initial_cost_hashjoin
 * 'extra' contains miscellaneous information about the join
 */
void
final_cost_hashjoin(PlannerInfo *root, HashPath *path,
					JoinCostWorkspace *workspace,
					JoinPathExtraData *extra)
{
	Path	   *outer_path = path->jpath.outerjoinpath;
	Path	   *inner_path = path->jpath.innerjoinpath;
	double		outer_path_rows = outer_path->rows;
	double		inner_path_rows = inner_path->rows;
	double		inner_path_rows_total = workspace->inner_rows_total;
	List	   *hashclauses = path->path_hashclauses;
	Cost		startup_cost = workspace->startup_cost;
	Cost		run_cost = workspace->run_cost;
	int			numbuckets = workspace->numbuckets;
	int			numbatches = workspace->numbatches;
	Cost		cpu_per_tuple;
	QualCost	hash_qual_cost;
	QualCost	qp_qual_cost;
	double		hashjointuples;
	double		virtualbuckets;
	Selectivity innerbucketsize;
	Selectivity innermcvfreq;
	ListCell   *hcl;

	/* Mark the path with the correct row estimate */
	if (path->jpath.path.param_info)
		path->jpath.path.rows = path->jpath.path.param_info->ppi_rows;
	else
		path->jpath.path.rows = path->jpath.path.parent->rows;

	/* For partial paths, scale row estimate. */
	if (path->jpath.path.parallel_workers > 0)
	{
		double		parallel_divisor = get_parallel_divisor(&path->jpath.path);

		path->jpath.path.rows =
			clamp_row_est(path->jpath.path.rows / parallel_divisor);
	}

	/*
	 * We could include disable_cost in the preliminary estimate, but that
	 * would amount to optimizing for the case where the join method is
	 * disabled, which doesn't seem like the way to bet.
	 */
	if (!enable_hashjoin)
		startup_cost += disable_cost;

	/* mark the path with estimated # of batches */
	path->num_batches = numbatches;

	/* store the total number of tuples (sum of partial row estimates) */
	path->inner_rows_total = inner_path_rows_total;

	/* and compute the number of "virtual" buckets in the whole join */
	virtualbuckets = (double) numbuckets * (double) numbatches;

	/*
	 * Determine bucketsize fraction and MCV frequency for the inner relation.
	 * We use the smallest bucketsize or MCV frequency estimated for any
	 * individual hashclause; this is undoubtedly conservative.
	 *
	 * BUT: if inner relation has been unique-ified, we can assume it's good
	 * for hashing.  This is important both because it's the right answer, and
	 * because we avoid contaminating the cache with a value that's wrong for
	 * non-unique-ified paths.
	 */
	if (IsA(inner_path, UniquePath))
	{
		innerbucketsize = 1.0 / virtualbuckets;
		innermcvfreq = 0.0;
	}
	else
	{
		innerbucketsize = 1.0;
		innermcvfreq = 1.0;
		foreach(hcl, hashclauses)
		{
			RestrictInfo *restrictinfo = lfirst_node(RestrictInfo, hcl);
			Selectivity thisbucketsize;
			Selectivity thismcvfreq;

			/*
			 * First we have to figure out which side of the hashjoin clause
			 * is the inner side.
			 *
			 * Since we tend to visit the same clauses over and over when
			 * planning a large query, we cache the bucket stats estimates in
			 * the RestrictInfo node to avoid repeated lookups of statistics.
			 */
			if (bms_is_subset(restrictinfo->right_relids,
							  inner_path->parent->relids))
			{
				/* righthand side is inner */
				thisbucketsize = restrictinfo->right_bucketsize;
				if (thisbucketsize < 0)
				{
					/* not cached yet */
					estimate_hash_bucket_stats(root,
											   get_rightop(restrictinfo->clause),
											   virtualbuckets,
											   &restrictinfo->right_mcvfreq,
											   &restrictinfo->right_bucketsize);
					thisbucketsize = restrictinfo->right_bucketsize;
				}
				thismcvfreq = restrictinfo->right_mcvfreq;
			}
			else
			{
				Assert(bms_is_subset(restrictinfo->left_relids,
									 inner_path->parent->relids));
				/* lefthand side is inner */
				thisbucketsize = restrictinfo->left_bucketsize;
				if (thisbucketsize < 0)
				{
					/* not cached yet */
					estimate_hash_bucket_stats(root,
											   get_leftop(restrictinfo->clause),
											   virtualbuckets,
											   &restrictinfo->left_mcvfreq,
											   &restrictinfo->left_bucketsize);
					thisbucketsize = restrictinfo->left_bucketsize;
				}
				thismcvfreq = restrictinfo->left_mcvfreq;
			}

			if (innerbucketsize > thisbucketsize)
				innerbucketsize = thisbucketsize;
			if (innermcvfreq > thismcvfreq)
				innermcvfreq = thismcvfreq;
		}
	}

	/*
	 * If the bucket holding the inner MCV would exceed work_mem, we don't
	 * want to hash unless there is really no other alternative, so apply
	 * disable_cost.  (The executor normally copes with excessive memory usage
	 * by splitting batches, but obviously it cannot separate equal values
	 * that way, so it will be unable to drive the batch size below work_mem
	 * when this is true.)
	 */
	if (relation_byte_size(clamp_row_est(inner_path_rows * innermcvfreq),
						   inner_path->pathtarget->width) >
		(work_mem * 1024L))
		startup_cost += disable_cost;

	/*
	 * Compute cost of the hashquals and qpquals (other restriction clauses)
	 * separately.
	 */
	cost_qual_eval(&hash_qual_cost, hashclauses, root);
	cost_qual_eval(&qp_qual_cost, path->jpath.joinrestrictinfo, root);
	qp_qual_cost.startup -= hash_qual_cost.startup;
	qp_qual_cost.per_tuple -= hash_qual_cost.per_tuple;

	/* CPU costs */

	if (path->jpath.jointype == JOIN_SEMI ||
		path->jpath.jointype == JOIN_ANTI ||
		extra->inner_unique)
	{
		double		outer_matched_rows;
		Selectivity inner_scan_frac;

		/*
		 * With a SEMI or ANTI join, or if the innerrel is known unique, the
		 * executor will stop after the first match.
		 *
		 * For an outer-rel row that has at least one match, we can expect the
		 * bucket scan to stop after a fraction 1/(match_count+1) of the
		 * bucket's rows, if the matches are evenly distributed.  Since they
		 * probably aren't quite evenly distributed, we apply a fuzz factor of
		 * 2.0 to that fraction.  (If we used a larger fuzz factor, we'd have
		 * to clamp inner_scan_frac to at most 1.0; but since match_count is
		 * at least 1, no such clamp is needed now.)
		 */
		outer_matched_rows = rint(outer_path_rows * extra->semifactors.outer_match_frac);
		inner_scan_frac = 2.0 / (extra->semifactors.match_count + 1.0);

		startup_cost += hash_qual_cost.startup;
		run_cost += hash_qual_cost.per_tuple * outer_matched_rows *
			clamp_row_est(inner_path_rows * innerbucketsize * inner_scan_frac) * 0.5;

		/*
		 * For unmatched outer-rel rows, the picture is quite a lot different.
		 * In the first place, there is no reason to assume that these rows
		 * preferentially hit heavily-populated buckets; instead assume they
		 * are uncorrelated with the inner distribution and so they see an
		 * average bucket size of inner_path_rows / virtualbuckets.  In the
		 * second place, it seems likely that they will have few if any exact
		 * hash-code matches and so very few of the tuples in the bucket will
		 * actually require eval of the hash quals.  We don't have any good
		 * way to estimate how many will, but for the moment assume that the
		 * effective cost per bucket entry is one-tenth what it is for
		 * matchable tuples.
		 */
		run_cost += hash_qual_cost.per_tuple *
			(outer_path_rows - outer_matched_rows) *
			clamp_row_est(inner_path_rows / virtualbuckets) * 0.05;

		/* Get # of tuples that will pass the basic join */
		if (path->jpath.jointype == JOIN_ANTI)
			hashjointuples = outer_path_rows - outer_matched_rows;
		else
			hashjointuples = outer_matched_rows;
	}
	else
	{
		/*
		 * The number of tuple comparisons needed is the number of outer
		 * tuples times the typical number of tuples in a hash bucket, which
		 * is the inner relation size times its bucketsize fraction.  At each
		 * one, we need to evaluate the hashjoin quals.  But actually,
		 * charging the full qual eval cost at each tuple is pessimistic,
		 * since we don't evaluate the quals unless the hash values match
		 * exactly.  For lack of a better idea, halve the cost estimate to
		 * allow for that.
		 */
		startup_cost += hash_qual_cost.startup;
		run_cost += hash_qual_cost.per_tuple * outer_path_rows *
			clamp_row_est(inner_path_rows * innerbucketsize) * 0.5;

		/*
		 * Get approx # tuples passing the hashquals.  We use
		 * approx_tuple_count here because we need an estimate done with
		 * JOIN_INNER semantics.
		 */
		hashjointuples = approx_tuple_count(root, &path->jpath, hashclauses);
	}

	/*
	 * For each tuple that gets through the hashjoin proper, we charge
	 * cpu_tuple_cost plus the cost of evaluating additional restriction
	 * clauses that are to be applied at the join.  (This is pessimistic since
	 * not all of the quals may get evaluated at each tuple.)
	 */
	startup_cost += qp_qual_cost.startup;
	cpu_per_tuple = cpu_tuple_cost + qp_qual_cost.per_tuple;
	run_cost += cpu_per_tuple * hashjointuples;

	/* tlist eval costs are paid per output row, not per tuple scanned */
	startup_cost += path->jpath.path.pathtarget->cost.startup;
	run_cost += path->jpath.path.pathtarget->cost.per_tuple * path->jpath.path.rows;

	path->jpath.path.startup_cost = startup_cost;
	path->jpath.path.total_cost = startup_cost + run_cost;
}


/*
 * cost_subplan
 *		Figure the costs for a SubPlan (or initplan).
 *
 * Note: we could dig the subplan's Plan out of the root list, but in practice
 * all callers have it handy already, so we make them pass it.
 */
void
cost_subplan(PlannerInfo *root, SubPlan *subplan, Plan *plan)
{
	QualCost	sp_cost;

	/* Figure any cost for evaluating the testexpr */
	cost_qual_eval(&sp_cost,
				   make_ands_implicit((Expr *) subplan->testexpr),
				   root);

	if (subplan->useHashTable)
	{
		/*
		 * If we are using a hash table for the subquery outputs, then the
		 * cost of evaluating the query is a one-time cost.  We charge one
		 * cpu_operator_cost per tuple for the work of loading the hashtable,
		 * too.
		 */
		sp_cost.startup += plan->total_cost +
			cpu_operator_cost * plan->plan_rows;

		/*
		 * The per-tuple costs include the cost of evaluating the lefthand
		 * expressions, plus the cost of probing the hashtable.  We already
		 * accounted for the lefthand expressions as part of the testexpr, and
		 * will also have counted one cpu_operator_cost for each comparison
		 * operator.  That is probably too low for the probing cost, but it's
		 * hard to make a better estimate, so live with it for now.
		 */
	}
	else
	{
		/*
		 * Otherwise we will be rescanning the subplan output on each
		 * evaluation.  We need to estimate how much of the output we will
		 * actually need to scan.  NOTE: this logic should agree with the
		 * tuple_fraction estimates used by make_subplan() in
		 * plan/subselect.c.
		 */
		Cost		plan_run_cost = plan->total_cost - plan->startup_cost;

		if (subplan->subLinkType == EXISTS_SUBLINK)
		{
			/* we only need to fetch 1 tuple; clamp to avoid zero divide */
			sp_cost.per_tuple += plan_run_cost / clamp_row_est(plan->plan_rows);
		}
		else if (subplan->subLinkType == ALL_SUBLINK ||
				 subplan->subLinkType == ANY_SUBLINK)
		{
			/* assume we need 50% of the tuples */
			sp_cost.per_tuple += 0.50 * plan_run_cost;
			/* also charge a cpu_operator_cost per row examined */
			sp_cost.per_tuple += 0.50 * plan->plan_rows * cpu_operator_cost;
		}
		else
		{
			/* assume we need all tuples */
			sp_cost.per_tuple += plan_run_cost;
		}

		/*
		 * Also account for subplan's startup cost. If the subplan is
		 * uncorrelated or undirect correlated, AND its topmost node is one
		 * that materializes its output, assume that we'll only need to pay
		 * its startup cost once; otherwise assume we pay the startup cost
		 * every time.
		 */
		if (subplan->parParam == NIL &&
			ExecMaterializesOutput(nodeTag(plan)))
			sp_cost.startup += plan->startup_cost;
		else
			sp_cost.per_tuple += plan->startup_cost;
	}

	subplan->startup_cost = sp_cost.startup;
	subplan->per_call_cost = sp_cost.per_tuple;
}


/*
 * cost_rescan
 *		Given a finished Path, estimate the costs of rescanning it after
 *		having done so the first time.  For some Path types a rescan is
 *		cheaper than an original scan (if no parameters change), and this
 *		function embodies knowledge about that.  The default is to return
 *		the same costs stored in the Path.  (Note that the cost estimates
 *		actually stored in Paths are always for first scans.)
 *
 * This function is not currently intended to model effects such as rescans
 * being cheaper due to disk block caching; what we are concerned with is
 * plan types wherein the executor caches results explicitly, or doesn't
 * redo startup calculations, etc.
 */
static void
cost_rescan(PlannerInfo *root, Path *path,
			Cost *rescan_startup_cost,	/* output parameters */
			Cost *rescan_total_cost)
{
	switch (path->pathtype)
	{
		case T_FunctionScan:

			/*
			 * Currently, nodeFunctionscan.c always executes the function to
			 * completion before returning any rows, and caches the results in
			 * a tuplestore.  So the function eval cost is all startup cost
			 * and isn't paid over again on rescans. However, all run costs
			 * will be paid over again.
			 */
			*rescan_startup_cost = 0;
			*rescan_total_cost = path->total_cost - path->startup_cost;
			break;
		case T_HashJoin:

			/*
			 * If it's a single-batch join, we don't need to rebuild the hash
			 * table during a rescan.
			 */
			if (((HashPath *) path)->num_batches == 1)
			{
				/* Startup cost is exactly the cost of hash table building */
				*rescan_startup_cost = 0;
				*rescan_total_cost = path->total_cost - path->startup_cost;
			}
			else
			{
				/* Otherwise, no special treatment */
				*rescan_startup_cost = path->startup_cost;
				*rescan_total_cost = path->total_cost;
			}
			break;
		case T_CteScan:
		case T_WorkTableScan:
			{
				/*
				 * These plan types materialize their final result in a
				 * tuplestore or tuplesort object.  So the rescan cost is only
				 * cpu_tuple_cost per tuple, unless the result is large enough
				 * to spill to disk.
				 */
				Cost		run_cost = cpu_tuple_cost * path->rows;
				double		nbytes = relation_byte_size(path->rows,
														path->pathtarget->width);
				long		work_mem_bytes = work_mem * 1024L;

				if (nbytes > work_mem_bytes)
				{
					/* It will spill, so account for re-read cost */
					double		npages = ceil(nbytes / BLCKSZ);

					run_cost += seq_page_cost * npages;
				}
				*rescan_startup_cost = 0;
				*rescan_total_cost = run_cost;
			}
			break;
		case T_Material:
		case T_Sort:
			{
				/*
				 * These plan types not only materialize their results, but do
				 * not implement qual filtering or projection.  So they are
				 * even cheaper to rescan than the ones above.  We charge only
				 * cpu_operator_cost per tuple.  (Note: keep that in sync with
				 * the run_cost charge in cost_sort, and also see comments in
				 * cost_material before you change it.)
				 */
				Cost		run_cost = cpu_operator_cost * path->rows;
				double		nbytes = relation_byte_size(path->rows,
														path->pathtarget->width);
				long		work_mem_bytes = work_mem * 1024L;

				if (nbytes > work_mem_bytes)
				{
					/* It will spill, so account for re-read cost */
					double		npages = ceil(nbytes / BLCKSZ);

					run_cost += seq_page_cost * npages;
				}
				*rescan_startup_cost = 0;
				*rescan_total_cost = run_cost;
			}
			break;
		default:
			*rescan_startup_cost = path->startup_cost;
			*rescan_total_cost = path->total_cost;
			break;
	}
}


/*
 * cost_qual_eval
 *		Estimate the CPU costs of evaluating a WHERE clause.
 *		The input can be either an implicitly-ANDed list of boolean
 *		expressions, or a list of RestrictInfo nodes.  (The latter is
 *		preferred since it allows caching of the results.)
 *		The result includes both a one-time (startup) component,
 *		and a per-evaluation component.
 */
void
cost_qual_eval(QualCost *cost, List *quals, PlannerInfo *root)
{
	cost_qual_eval_context context;
	ListCell   *l;

	context.root = root;
	context.total.startup = 0;
	context.total.per_tuple = 0;

	/* We don't charge any cost for the implicit ANDing at top level ... */

	foreach(l, quals)
	{
		Node	   *qual = (Node *) lfirst(l);

		cost_qual_eval_walker(qual, &context);
	}

	*cost = context.total;
}

/*
 * cost_qual_eval_node
 *		As above, for a single RestrictInfo or expression.
 */
void
cost_qual_eval_node(QualCost *cost, Node *qual, PlannerInfo *root)
{
	cost_qual_eval_context context;

	context.root = root;
	context.total.startup = 0;
	context.total.per_tuple = 0;

	cost_qual_eval_walker(qual, &context);

	*cost = context.total;
}

static bool
cost_qual_eval_walker(Node *node, cost_qual_eval_context *context)
{
	if (node == NULL)
		return false;

	/*
	 * RestrictInfo nodes contain an eval_cost field reserved for this
	 * routine's use, so that it's not necessary to evaluate the qual clause's
	 * cost more than once.  If the clause's cost hasn't been computed yet,
	 * the field's startup value will contain -1.
	 */
	if (IsA(node, RestrictInfo))
	{
		RestrictInfo *rinfo = (RestrictInfo *) node;

		if (rinfo->eval_cost.startup < 0)
		{
			cost_qual_eval_context locContext;

			locContext.root = context->root;
			locContext.total.startup = 0;
			locContext.total.per_tuple = 0;

			/*
			 * For an OR clause, recurse into the marked-up tree so that we
			 * set the eval_cost for contained RestrictInfos too.
			 */
			if (rinfo->orclause)
				cost_qual_eval_walker((Node *) rinfo->orclause, &locContext);
			else
				cost_qual_eval_walker((Node *) rinfo->clause, &locContext);

			/*
			 * If the RestrictInfo is marked pseudoconstant, it will be tested
			 * only once, so treat its cost as all startup cost.
			 */
			if (rinfo->pseudoconstant)
			{
				/* count one execution during startup */
				locContext.total.startup += locContext.total.per_tuple;
				locContext.total.per_tuple = 0;
			}
			rinfo->eval_cost = locContext.total;
		}
		context->total.startup += rinfo->eval_cost.startup;
		context->total.per_tuple += rinfo->eval_cost.per_tuple;
		/* do NOT recurse into children */
		return false;
	}

	/*
	 * For each operator or function node in the given tree, we charge the
	 * estimated execution cost given by pg_proc.procost (remember to multiply
	 * this by cpu_operator_cost).
	 *
	 * Vars and Consts are charged zero, and so are boolean operators (AND,
	 * OR, NOT). Simplistic, but a lot better than no model at all.
	 *
	 * Should we try to account for the possibility of short-circuit
	 * evaluation of AND/OR?  Probably *not*, because that would make the
	 * results depend on the clause ordering, and we are not in any position
	 * to expect that the current ordering of the clauses is the one that's
	 * going to end up being used.  The above per-RestrictInfo caching would
	 * not mix well with trying to re-order clauses anyway.
	 *
	 * Another issue that is entirely ignored here is that if a set-returning
	 * function is below top level in the tree, the functions/operators above
	 * it will need to be evaluated multiple times.  In practical use, such
	 * cases arise so seldom as to not be worth the added complexity needed;
	 * moreover, since our rowcount estimates for functions tend to be pretty
	 * phony, the results would also be pretty phony.
	 */
	if (IsA(node, FuncExpr))
	{
		add_function_cost(context->root, ((FuncExpr *) node)->funcid, node,
						  &context->total);
	}
	else if (IsA(node, OpExpr) ||
			 IsA(node, DistinctExpr) ||
			 IsA(node, NullIfExpr))
	{
		/* rely on struct equivalence to treat these all alike */
		set_opfuncid((OpExpr *) node);
		add_function_cost(context->root, ((OpExpr *) node)->opfuncid, node,
						  &context->total);
	}
	else if (IsA(node, ScalarArrayOpExpr))
	{
		/*
		 * Estimate that the operator will be applied to about half of the
		 * array elements before the answer is determined.
		 */
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) node;
		Node	   *arraynode = (Node *) lsecond(saop->args);
		QualCost	sacosts;

		set_sa_opfuncid(saop);
		sacosts.startup = sacosts.per_tuple = 0;
		add_function_cost(context->root, saop->opfuncid, NULL,
						  &sacosts);
		context->total.startup += sacosts.startup;
		context->total.per_tuple += sacosts.per_tuple *
			estimate_array_length(arraynode) * 0.5;
	}
	else if (IsA(node, Aggref) ||
			 IsA(node, WindowFunc))
	{
		/*
		 * Aggref and WindowFunc nodes are (and should be) treated like Vars,
		 * ie, zero execution cost in the current model, because they behave
		 * essentially like Vars at execution.  We disregard the costs of
		 * their input expressions for the same reason.  The actual execution
		 * costs of the aggregate/window functions and their arguments have to
		 * be factored into plan-node-specific costing of the Agg or WindowAgg
		 * plan node.
		 */
		return false;			/* don't recurse into children */
	}
	else if (IsA(node, GroupingFunc))
	{
		/* Treat this as having cost 1 */
		context->total.per_tuple += cpu_operator_cost;
		return false;			/* don't recurse into children */
	}
	else if (IsA(node, CoerceViaIO))
	{
		CoerceViaIO *iocoerce = (CoerceViaIO *) node;
		Oid			iofunc;
		Oid			typioparam;
		bool		typisvarlena;

		/* check the result type's input function */
		getTypeInputInfo(iocoerce->resulttype,
						 &iofunc, &typioparam);
		add_function_cost(context->root, iofunc, NULL,
						  &context->total);
		/* check the input type's output function */
		getTypeOutputInfo(exprType((Node *) iocoerce->arg),
						  &iofunc, &typisvarlena);
		add_function_cost(context->root, iofunc, NULL,
						  &context->total);
	}
	else if (IsA(node, ArrayCoerceExpr))
	{
		ArrayCoerceExpr *acoerce = (ArrayCoerceExpr *) node;
		QualCost	perelemcost;

		cost_qual_eval_node(&perelemcost, (Node *) acoerce->elemexpr,
							context->root);
		context->total.startup += perelemcost.startup;
		if (perelemcost.per_tuple > 0)
			context->total.per_tuple += perelemcost.per_tuple *
				estimate_array_length((Node *) acoerce->arg);
	}
	else if (IsA(node, RowCompareExpr))
	{
		/* Conservatively assume we will check all the columns */
		RowCompareExpr *rcexpr = (RowCompareExpr *) node;
		ListCell   *lc;

		foreach(lc, rcexpr->opnos)
		{
			Oid			opid = lfirst_oid(lc);

			add_function_cost(context->root, get_opcode(opid), NULL,
							  &context->total);
		}
	}
	else if (IsA(node, MinMaxExpr) ||
			 IsA(node, SQLValueFunction) ||
			 IsA(node, XmlExpr) ||
			 IsA(node, CoerceToDomain) ||
			 IsA(node, NextValueExpr))
	{
		/* Treat all these as having cost 1 */
		context->total.per_tuple += cpu_operator_cost;
	}
	else if (IsA(node, CurrentOfExpr))
	{
		/* Report high cost to prevent selection of anything but TID scan */
		context->total.startup += disable_cost;
	}
	else if (IsA(node, SubLink))
	{
		/* This routine should not be applied to un-planned expressions */
		elog(ERROR, "cannot handle unplanned sub-select");
	}
	else if (IsA(node, SubPlan))
	{
		/*
		 * A subplan node in an expression typically indicates that the
		 * subplan will be executed on each evaluation, so charge accordingly.
		 * (Sub-selects that can be executed as InitPlans have already been
		 * removed from the expression.)
		 */
		SubPlan    *subplan = (SubPlan *) node;

		context->total.startup += subplan->startup_cost;
		context->total.per_tuple += subplan->per_call_cost;

		/*
		 * We don't want to recurse into the testexpr, because it was already
		 * counted in the SubPlan node's costs.  So we're done.
		 */
		return false;
	}
	else if (IsA(node, AlternativeSubPlan))
	{
		/*
		 * Arbitrarily use the first alternative plan for costing.  (We should
		 * certainly only include one alternative, and we don't yet have
		 * enough information to know which one the executor is most likely to
		 * use.)
		 */
		AlternativeSubPlan *asplan = (AlternativeSubPlan *) node;

		return cost_qual_eval_walker((Node *) linitial(asplan->subplans),
									 context);
	}
	else if (IsA(node, PlaceHolderVar))
	{
		/*
		 * A PlaceHolderVar should be given cost zero when considering general
		 * expression evaluation costs.  The expense of doing the contained
		 * expression is charged as part of the tlist eval costs of the scan
		 * or join where the PHV is first computed (see set_rel_width and
		 * add_placeholders_to_joinrel).  If we charged it again here, we'd be
		 * double-counting the cost for each level of plan that the PHV
		 * bubbles up through.  Hence, return without recursing into the
		 * phexpr.
		 */
		return false;
	}

	/* recurse into children */
	return expression_tree_walker(node, cost_qual_eval_walker,
								  (void *) context);
}

/*
 * get_restriction_qual_cost
 *	  Compute evaluation costs of a baserel's restriction quals, plus any
 *	  movable join quals that have been pushed down to the scan.
 *	  Results are returned into *qpqual_cost.
 *
 * This is a convenience subroutine that works for seqscans and other cases
 * where all the given quals will be evaluated the hard way.  It's not useful
 * for cost_index(), for example, where the index machinery takes care of
 * some of the quals.  We assume baserestrictcost was previously set by
 * set_baserel_size_estimates().
 */
static void
get_restriction_qual_cost(PlannerInfo *root, RelOptInfo *baserel,
						  ParamPathInfo *param_info,
						  QualCost *qpqual_cost)
{
	if (param_info)
	{
		/* Include costs of pushed-down clauses */
		cost_qual_eval(qpqual_cost, param_info->ppi_clauses, root);

		qpqual_cost->startup += baserel->baserestrictcost.startup;
		qpqual_cost->per_tuple += baserel->baserestrictcost.per_tuple;
	}
	else
		*qpqual_cost = baserel->baserestrictcost;
}


/*
 * compute_semi_anti_join_factors
 *	  估算 SEMI、ANTI 或 inner_unique 连接中内层输入被扫描的比例。
 *
 * 在 hash 或 nestloop 的 SEMI/ANTI 连接中，执行器在找到当前外层行的第一个匹配后会停止扫描内层行。
 * 如果检测到内层关系唯一，也会有同样的行为。
 * 因此需要调整部分成本组件以反映这一效果。本函数计算这些调整所需的估算值。
 * 这些估算值与具体的外层和内层路径无关，只需计算一次并传递给所有连接成本估算函数。
 *
 * 输入参数:
 *	joinrel: 当前考虑的连接关系
 *	outerrel: 外层关系
 *	innerrel: 内层关系
 *	jointype: 如果不是 JOIN_SEMI 或 JOIN_ANTI，则假定为 inner_unique
 *	sjinfo: 连接相关的 SpecialJoinInfo
 *	restrictlist: 连接条件
 * 输出参数:
 *	*semifactors 被填充（字段定义见 pathnodes.h）
 */
void
compute_semi_anti_join_factors(PlannerInfo *root,
							   RelOptInfo *joinrel,
							   RelOptInfo *outerrel,
							   RelOptInfo *innerrel,
							   JoinType jointype,
							   SpecialJoinInfo *sjinfo,
							   List *restrictlist,
							   SemiAntiJoinFactors *semifactors)
{
	Selectivity jselec;
	Selectivity nselec;
	Selectivity avgmatch;
	SpecialJoinInfo norm_sjinfo;
	List	   *joinquals;
	ListCell   *l;

	/*
	 * 对于 ANTI 连接，必须忽略 "pushed down" 的条件，因为这些不会影响匹配逻辑。
	 * 对于 SEMI 连接，不区分 joinquals 和 "pushed down" 条件，直接使用整个 restrictinfo 列表。
	 * 对于其他外连接类型，只考虑非 "pushed down" 条件。
	 */
	if (IS_OUTER_JOIN(jointype))
	{
		joinquals = NIL;
		foreach(l, restrictlist)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

			if (!RINFO_IS_PUSHED_DOWN(rinfo, joinrel->relids))
				joinquals = lappend(joinquals, rinfo);
		}
	}
	else
		joinquals = restrictlist;

	/*
	 * 获取连接条件在 JOIN_SEMI 或 JOIN_ANTI 下的选择率。
	 */
	jselec = clauselist_selectivity(root,
									joinquals,
									0,
									(jointype == JOIN_ANTI) ? JOIN_ANTI : JOIN_SEMI,
									sjinfo);

	/*
	 * 同时获取连接条件在普通内连接下的选择率。
	 */
	norm_sjinfo.type = T_SpecialJoinInfo;
	norm_sjinfo.min_lefthand = outerrel->relids;
	norm_sjinfo.min_righthand = innerrel->relids;
	norm_sjinfo.syn_lefthand = outerrel->relids;
	norm_sjinfo.syn_righthand = innerrel->relids;
	norm_sjinfo.jointype = JOIN_INNER;
	/* 其它字段无需设置 */
	norm_sjinfo.lhs_strict = false;
	norm_sjinfo.delay_upper_joins = false;
	norm_sjinfo.semi_can_btree = false;
	norm_sjinfo.semi_can_hash = false;
	norm_sjinfo.semi_operators = NIL;
	norm_sjinfo.semi_rhs_exprs = NIL;

	nselec = clauselist_selectivity(root,
									joinquals,
									0,
									JOIN_INNER,
									&norm_sjinfo);

	/* 释放 joinquals 列表 */
	if (IS_OUTER_JOIN(jointype))
		list_free(joinquals);

	/*
	 * jselec 可理解为有匹配的外层行比例（SEMI/ANTI 都如此）。
	 * nselec 是笛卡尔积中匹配的比例。
	 * 所以每个有匹配的外层行平均匹配的内层行数为 nselec * inner_rows / jselec。
	 *
	 * 注意：这里用的是内层关系的 "rows" 数，即使后续考虑的是参数化路径也没问题，
	 * 因为选择率估算已包含所有连接条件。
	 */
	if (jselec > 0)				/* 防止除零 */
	{
		avgmatch = nselec * innerrel->rows / jselec;
		/* 限定在合理范围 */
		avgmatch = Max(1.0, avgmatch);
	}
	else
		avgmatch = 1.0;

	semifactors->outer_match_frac = jselec;
	semifactors->match_count = avgmatch;
}

/*
 * has_indexed_join_quals
 *	  Check whether all the joinquals of a nestloop join are used as
 *	  inner index quals.
 *
 * If the inner path of a SEMI/ANTI join is an indexscan (including bitmap
 * indexscan) that uses all the joinquals as indexquals, we can assume that an
 * unmatched outer tuple is cheap to process, whereas otherwise it's probably
 * expensive.
 */
static bool
has_indexed_join_quals(NestPath *joinpath)
{
	Relids		joinrelids = joinpath->path.parent->relids;
	Path	   *innerpath = joinpath->innerjoinpath;
	List	   *indexclauses;
	bool		found_one;
	ListCell   *lc;

	/* If join still has quals to evaluate, it's not fast */
	if (joinpath->joinrestrictinfo != NIL)
		return false;
	/* Nor if the inner path isn't parameterized at all */
	if (innerpath->param_info == NULL)
		return false;

	/* Find the indexclauses list for the inner scan */
	switch (innerpath->pathtype)
	{
		case T_IndexScan:
		case T_IndexOnlyScan:
			indexclauses = ((IndexPath *) innerpath)->indexclauses;
			break;
		case T_BitmapHeapScan:
			{
				/* Accept only a simple bitmap scan, not AND/OR cases */
				Path	   *bmqual = ((BitmapHeapPath *) innerpath)->bitmapqual;

				if (IsA(bmqual, IndexPath))
					indexclauses = ((IndexPath *) bmqual)->indexclauses;
				else
					return false;
				break;
			}
		default:

			/*
			 * If it's not a simple indexscan, it probably doesn't run quickly
			 * for zero rows out, even if it's a parameterized path using all
			 * the joinquals.
			 */
			return false;
	}

	/*
	 * Examine the inner path's param clauses.  Any that are from the outer
	 * path must be found in the indexclauses list, either exactly or in an
	 * equivalent form generated by equivclass.c.  Also, we must find at least
	 * one such clause, else it's a clauseless join which isn't fast.
	 */
	found_one = false;
	foreach(lc, innerpath->param_info->ppi_clauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		if (join_clause_is_movable_into(rinfo,
										innerpath->parent->relids,
										joinrelids))
		{
			if (!is_redundant_with_indexclauses(rinfo, indexclauses))
				return false;
			found_one = true;
		}
	}
	return found_one;
}


/*
 * approx_tuple_count
 *		Quick-and-dirty estimation of the number of join rows passing
 *		a set of qual conditions.
 *
 * The quals can be either an implicitly-ANDed list of boolean expressions,
 * or a list of RestrictInfo nodes (typically the latter).
 *
 * We intentionally compute the selectivity under JOIN_INNER rules, even
 * if it's some type of outer join.  This is appropriate because we are
 * trying to figure out how many tuples pass the initial merge or hash
 * join step.
 *
 * This is quick-and-dirty because we bypass clauselist_selectivity, and
 * simply multiply the independent clause selectivities together.  Now
 * clauselist_selectivity often can't do any better than that anyhow, but
 * for some situations (such as range constraints) it is smarter.  However,
 * we can't effectively cache the results of clauselist_selectivity, whereas
 * the individual clause selectivities can be and are cached.
 *
 * Since we are only using the results to estimate how many potential
 * output tuples are generated and passed through qpqual checking, it
 * seems OK to live with the approximation.
 */
static double
approx_tuple_count(PlannerInfo *root, JoinPath *path, List *quals)
{
	double		tuples;
	double		outer_tuples = path->outerjoinpath->rows;
	double		inner_tuples = path->innerjoinpath->rows;
	SpecialJoinInfo sjinfo;
	Selectivity selec = 1.0;
	ListCell   *l;

	/*
	 * Make up a SpecialJoinInfo for JOIN_INNER semantics.
	 */
	sjinfo.type = T_SpecialJoinInfo;
	sjinfo.min_lefthand = path->outerjoinpath->parent->relids;
	sjinfo.min_righthand = path->innerjoinpath->parent->relids;
	sjinfo.syn_lefthand = path->outerjoinpath->parent->relids;
	sjinfo.syn_righthand = path->innerjoinpath->parent->relids;
	sjinfo.jointype = JOIN_INNER;
	/* we don't bother trying to make the remaining fields valid */
	sjinfo.lhs_strict = false;
	sjinfo.delay_upper_joins = false;
	sjinfo.semi_can_btree = false;
	sjinfo.semi_can_hash = false;
	sjinfo.semi_operators = NIL;
	sjinfo.semi_rhs_exprs = NIL;

	/* Get the approximate selectivity */
	foreach(l, quals)
	{
		Node	   *qual = (Node *) lfirst(l);

		/* Note that clause_selectivity will be able to cache its result */
		selec *= clause_selectivity(root, qual, 0, JOIN_INNER, &sjinfo);
	}

	/* Apply it to the input relation sizes */
	tuples = selec * outer_tuples * inner_tuples;

	return clamp_row_est(tuples);
}


/*-------------------------------------------------------------------------*
 * set_baserel_size_estimates
 *		设置给定基本关系的大小估计值
 *
 * 前置条件：
 *	- 关系的targetlist（目标列列表）和restrictinfo列表（限制条件信息列表）必须已构建完成
 *	- rel->tuples（表中元组总数估计）必须已设置
 *
 * 此函数会设置关系节点的以下字段：
 *	rows: 应用限制条件后的预计输出元组数量
 *	width: 预计的平均输出元组宽度（以字节为单位）
 *	baserestrictcost: 评估baserestrictinfo子句的估计成本
 *
 * 该函数是PostgreSQL查询优化器中成本估算阶段的核心组件，为后续的执行计划生成
 * 提供必要的统计信息基础。
 *-------------------------------------------------------------------------*/
void
set_baserel_size_estimates(PlannerInfo *root, RelOptInfo *rel)
{
	double		nrows;  /* 用于临时存储计算后的行数估计值 */

	/* 断言：此函数只应应用于基本关系（基表），而非连接关系或其他类型 */
	Assert(rel->relid > 0);  /* 基本关系的relid大于0，连接关系为负数 */

	/*
	 * 计算过滤后的行数：原始行数乘以限制条件的选择性
	 *
	 * clauselist_selectivity函数计算一组限制条件对元组的过滤比例
	 * 参数说明：
	 * - root: 规划器全局信息
	 * - rel->baserestrictinfo: 该关系的所有限制条件信息列表
	 * - 0: 通常为0，表示不考虑特定变量的相关性
	 * - JOIN_INNER: 表示内连接语义，适用于基本关系的过滤
	 * - NULL: 不需要传递特定的附加数据
	 */
	nrows = rel->tuples *
		clauselist_selectivity(root,
					   rel->baserestrictinfo,
					   0,
					   JOIN_INNER,
					   NULL);

	/*
	 * 将计算得到的行数限制在合理范围内，防止出现极小或极大的估计值
	 * clamp_row_est函数确保估计值在合理的边界内，避免优化器做出极端决策
	 */
	rel->rows = clamp_row_est(nrows);

	/*
	 * 计算评估所有限制条件的成本
	 * cost_qual_eval函数分析限制条件的复杂度和执行代价，更新baserestrictcost结构体
	 */
	cost_qual_eval(&rel->baserestrictcost, rel->baserestrictinfo, root);

	/*
	 * 设置关系的平均行宽
	 * set_rel_width函数根据关系的目标列计算出平均元组宽度，这对I/O成本估算至关重要
	 */
	set_rel_width(root, rel);
}


/*
 * get_parameterized_baserel_size
 *		Make a size estimate for a parameterized scan of a base relation.
 *
 * 'param_clauses' lists the additional join clauses to be used.
 *
 * set_baserel_size_estimates must have been applied already.
 */
double
get_parameterized_baserel_size(PlannerInfo *root, RelOptInfo *rel,
							   List *param_clauses)
{
	List	   *allclauses;
	double		nrows;

	/*
	 * Estimate the number of rows returned by the parameterized scan, knowing
	 * that it will apply all the extra join clauses as well as the rel's own
	 * restriction clauses.  Note that we force the clauses to be treated as
	 * non-join clauses during selectivity estimation.
	 */
	allclauses = list_concat(list_copy(param_clauses),
							 rel->baserestrictinfo);
	nrows = rel->tuples *
		clauselist_selectivity(root,
							   allclauses,
							   rel->relid,	/* do not use 0! */
							   JOIN_INNER,
							   NULL);
	nrows = clamp_row_est(nrows);
	/* For safety, make sure result is not more than the base estimate */
	if (nrows > rel->rows)
		nrows = rel->rows;
	return nrows;
}

/*
 * set_joinrel_size_estimates
 *		Set the size estimates for the given join relation.
 *
 * The rel's targetlist must have been constructed already, and a
 * restriction clause list that matches the given component rels must
 * be provided.
 *
 * Since there is more than one way to make a joinrel for more than two
 * base relations, the results we get here could depend on which component
 * rel pair is provided.  In theory we should get the same answers no matter
 * which pair is provided; in practice, since the selectivity estimation
 * routines don't handle all cases equally well, we might not.  But there's
 * not much to be done about it.  (Would it make sense to repeat the
 * calculations for each pair of input rels that's encountered, and somehow
 * average the results?  Probably way more trouble than it's worth, and
 * anyway we must keep the rowcount estimate the same for all paths for the
 * joinrel.)
 *
 * We set only the rows field here.  The reltarget field was already set by
 * build_joinrel_tlist, and baserestrictcost is not used for join rels.
 */
void
set_joinrel_size_estimates(PlannerInfo *root, RelOptInfo *rel,
						   RelOptInfo *outer_rel,
						   RelOptInfo *inner_rel,
						   SpecialJoinInfo *sjinfo,
						   List *restrictlist)
{
	rel->rows = calc_joinrel_size_estimate(root,
										   rel,
										   outer_rel,
										   inner_rel,
										   outer_rel->rows,
										   inner_rel->rows,
										   sjinfo,
										   restrictlist);
}

/*
 * get_parameterized_joinrel_size
 *		Make a size estimate for a parameterized scan of a join relation.
 *
 * 'rel' is the joinrel under consideration.
 * 'outer_path', 'inner_path' are (probably also parameterized) Paths that
 *		produce the relations being joined.
 * 'sjinfo' is any SpecialJoinInfo relevant to this join.
 * 'restrict_clauses' lists the join clauses that need to be applied at the
 * join node (including any movable clauses that were moved down to this join,
 * and not including any movable clauses that were pushed down into the
 * child paths).
 *
 * set_joinrel_size_estimates must have been applied already.
 */
double
get_parameterized_joinrel_size(PlannerInfo *root, RelOptInfo *rel,
							   Path *outer_path,
							   Path *inner_path,
							   SpecialJoinInfo *sjinfo,
							   List *restrict_clauses)
{
	double		nrows;

	/*
	 * Estimate the number of rows returned by the parameterized join as the
	 * sizes of the input paths times the selectivity of the clauses that have
	 * ended up at this join node.
	 *
	 * As with set_joinrel_size_estimates, the rowcount estimate could depend
	 * on the pair of input paths provided, though ideally we'd get the same
	 * estimate for any pair with the same parameterization.
	 */
	nrows = calc_joinrel_size_estimate(root,
									   rel,
									   outer_path->parent,
									   inner_path->parent,
									   outer_path->rows,
									   inner_path->rows,
									   sjinfo,
									   restrict_clauses);
	/* For safety, make sure result is not more than the base estimate */
	if (nrows > rel->rows)
		nrows = rel->rows;
	return nrows;
}

/*
 * calc_joinrel_size_estimate
 *		Workhorse for set_joinrel_size_estimates and
 *		get_parameterized_joinrel_size.
 *
 * outer_rel/inner_rel are the relations being joined, but they should be
 * assumed to have sizes outer_rows/inner_rows; those numbers might be less
 * than what rel->rows says, when we are considering parameterized paths.
 */
static double
calc_joinrel_size_estimate(PlannerInfo *root,
						   RelOptInfo *joinrel,
						   RelOptInfo *outer_rel,
						   RelOptInfo *inner_rel,
						   double outer_rows,
						   double inner_rows,
						   SpecialJoinInfo *sjinfo,
						   List *restrictlist_in)
{
	/* This apparently-useless variable dodges a compiler bug in VS2013: */
	List	   *restrictlist = restrictlist_in;
	JoinType	jointype = sjinfo->jointype;
	Selectivity fkselec;
	Selectivity jselec;
	Selectivity pselec;
	double		nrows;

	/*
	 * Compute joinclause selectivity.  Note that we are only considering
	 * clauses that become restriction clauses at this join level; we are not
	 * double-counting them because they were not considered in estimating the
	 * sizes of the component rels.
	 *
	 * First, see whether any of the joinclauses can be matched to known FK
	 * constraints.  If so, drop those clauses from the restrictlist, and
	 * instead estimate their selectivity using FK semantics.  (We do this
	 * without regard to whether said clauses are local or "pushed down".
	 * Probably, an FK-matching clause could never be seen as pushed down at
	 * an outer join, since it would be strict and hence would be grounds for
	 * join strength reduction.)  fkselec gets the net selectivity for
	 * FK-matching clauses, or 1.0 if there are none.
	 */
	fkselec = get_foreign_key_join_selectivity(root,
											   outer_rel->relids,
											   inner_rel->relids,
											   sjinfo,
											   &restrictlist);

	/*
	 * For an outer join, we have to distinguish the selectivity of the join's
	 * own clauses (JOIN/ON conditions) from any clauses that were "pushed
	 * down".  For inner joins we just count them all as joinclauses.
	 */
	if (IS_OUTER_JOIN(jointype))
	{
		List	   *joinquals = NIL;
		List	   *pushedquals = NIL;
		ListCell   *l;

		/* Grovel through the clauses to separate into two lists */
		foreach(l, restrictlist)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

			if (RINFO_IS_PUSHED_DOWN(rinfo, joinrel->relids))
				pushedquals = lappend(pushedquals, rinfo);
			else
				joinquals = lappend(joinquals, rinfo);
		}

		/* Get the separate selectivities */
		jselec = clauselist_selectivity(root,
										joinquals,
										0,
										jointype,
										sjinfo);
		pselec = clauselist_selectivity(root,
										pushedquals,
										0,
										jointype,
										sjinfo);

		/* Avoid leaking a lot of ListCells */
		list_free(joinquals);
		list_free(pushedquals);
	}
	else
	{
		jselec = clauselist_selectivity(root,
										restrictlist,
										0,
										jointype,
										sjinfo);
		pselec = 0.0;			/* not used, keep compiler quiet */
	}

	/*
	 * Basically, we multiply size of Cartesian product by selectivity.
	 *
	 * If we are doing an outer join, take that into account: the joinqual
	 * selectivity has to be clamped using the knowledge that the output must
	 * be at least as large as the non-nullable input.  However, any
	 * pushed-down quals are applied after the outer join, so their
	 * selectivity applies fully.
	 *
	 * For JOIN_SEMI and JOIN_ANTI, the selectivity is defined as the fraction
	 * of LHS rows that have matches, and we apply that straightforwardly.
	 */
	switch (jointype)
	{
		case JOIN_INNER:
			nrows = outer_rows * inner_rows * fkselec * jselec;
			/* pselec not used */
			break;
		case JOIN_LEFT:
			nrows = outer_rows * inner_rows * fkselec * jselec;
			if (nrows < outer_rows)
				nrows = outer_rows;
			nrows *= pselec;
			break;
		case JOIN_FULL:
			nrows = outer_rows * inner_rows * fkselec * jselec;
			if (nrows < outer_rows)
				nrows = outer_rows;
			if (nrows < inner_rows)
				nrows = inner_rows;
			nrows *= pselec;
			break;
		case JOIN_SEMI:
			nrows = outer_rows * fkselec * jselec;
			/* pselec not used */
			break;
		case JOIN_ANTI:
			nrows = outer_rows * (1.0 - fkselec * jselec);
			nrows *= pselec;
			break;
		default:
			/* other values not expected here */
			elog(ERROR, "unrecognized join type: %d", (int) jointype);
			nrows = 0;			/* keep compiler quiet */
			break;
	}

	return clamp_row_est(nrows);
}

/*
 * get_foreign_key_join_selectivity
 *		Estimate join selectivity for foreign-key-related clauses.
 *
 * Remove any clauses that can be matched to FK constraints from *restrictlist,
 * and return a substitute estimate of their selectivity.  1.0 is returned
 * when there are no such clauses.
 *
 * The reason for treating such clauses specially is that we can get better
 * estimates this way than by relying on clauselist_selectivity(), especially
 * for multi-column FKs where that function's assumption that the clauses are
 * independent falls down badly.  But even with single-column FKs, we may be
 * able to get a better answer when the pg_statistic stats are missing or out
 * of date.
 */
static Selectivity
get_foreign_key_join_selectivity(PlannerInfo *root,
								 Relids outer_relids,
								 Relids inner_relids,
								 SpecialJoinInfo *sjinfo,
								 List **restrictlist)
{
	Selectivity fkselec = 1.0;
	JoinType	jointype = sjinfo->jointype;
	List	   *worklist = *restrictlist;
	ListCell   *lc;

	/* Consider each FK constraint that is known to match the query */
	foreach(lc, root->fkey_list)
	{
		ForeignKeyOptInfo *fkinfo = (ForeignKeyOptInfo *) lfirst(lc);
		bool		ref_is_outer;
		List	   *removedlist;
		ListCell   *cell;
		ListCell   *prev;
		ListCell   *next;

		/*
		 * This FK is not relevant unless it connects a baserel on one side of
		 * this join to a baserel on the other side.
		 */
		if (bms_is_member(fkinfo->con_relid, outer_relids) &&
			bms_is_member(fkinfo->ref_relid, inner_relids))
			ref_is_outer = false;
		else if (bms_is_member(fkinfo->ref_relid, outer_relids) &&
				 bms_is_member(fkinfo->con_relid, inner_relids))
			ref_is_outer = true;
		else
			continue;

		/*
		 * If we're dealing with a semi/anti join, and the FK's referenced
		 * relation is on the outside, then knowledge of the FK doesn't help
		 * us figure out what we need to know (which is the fraction of outer
		 * rows that have matches).  On the other hand, if the referenced rel
		 * is on the inside, then all outer rows must have matches in the
		 * referenced table (ignoring nulls).  But any restriction or join
		 * clauses that filter that table will reduce the fraction of matches.
		 * We can account for restriction clauses, but it's too hard to guess
		 * how many table rows would get through a join that's inside the RHS.
		 * Hence, if either case applies, punt and ignore the FK.
		 */
		if ((jointype == JOIN_SEMI || jointype == JOIN_ANTI) &&
			(ref_is_outer || bms_membership(inner_relids) != BMS_SINGLETON))
			continue;

		/*
		 * Modify the restrictlist by removing clauses that match the FK (and
		 * putting them into removedlist instead).  It seems unsafe to modify
		 * the originally-passed List structure, so we make a shallow copy the
		 * first time through.
		 */
		if (worklist == *restrictlist)
			worklist = list_copy(worklist);

		removedlist = NIL;
		prev = NULL;
		for (cell = list_head(worklist); cell; cell = next)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(cell);
			bool		remove_it = false;
			int			i;

			next = lnext(cell);
			/* Drop this clause if it matches any column of the FK */
			for (i = 0; i < fkinfo->nkeys; i++)
			{
				if (rinfo->parent_ec)
				{
					/*
					 * EC-derived clauses can only match by EC.  It is okay to
					 * consider any clause derived from the same EC as
					 * matching the FK: even if equivclass.c chose to generate
					 * a clause equating some other pair of Vars, it could
					 * have generated one equating the FK's Vars.  So for
					 * purposes of estimation, we can act as though it did so.
					 *
					 * Note: checking parent_ec is a bit of a cheat because
					 * there are EC-derived clauses that don't have parent_ec
					 * set; but such clauses must compare expressions that
					 * aren't just Vars, so they cannot match the FK anyway.
					 */
					if (fkinfo->eclass[i] == rinfo->parent_ec)
					{
						remove_it = true;
						break;
					}
				}
				else
				{
					/*
					 * Otherwise, see if rinfo was previously matched to FK as
					 * a "loose" clause.
					 */
					if (list_member_ptr(fkinfo->rinfos[i], rinfo))
					{
						remove_it = true;
						break;
					}
				}
			}
			if (remove_it)
			{
				worklist = list_delete_cell(worklist, cell, prev);
				removedlist = lappend(removedlist, rinfo);
			}
			else
				prev = cell;
		}

		/*
		 * If we failed to remove all the matching clauses we expected to
		 * find, chicken out and ignore this FK; applying its selectivity
		 * might result in double-counting.  Put any clauses we did manage to
		 * remove back into the worklist.
		 *
		 * Since the matching clauses are known not outerjoin-delayed, they
		 * should certainly have appeared in the initial joinclause list.  If
		 * we didn't find them, they must have been matched to, and removed
		 * by, some other FK in a previous iteration of this loop.  (A likely
		 * case is that two FKs are matched to the same EC; there will be only
		 * one EC-derived clause in the initial list, so the first FK will
		 * consume it.)  Applying both FKs' selectivity independently risks
		 * underestimating the join size; in particular, this would undo one
		 * of the main things that ECs were invented for, namely to avoid
		 * double-counting the selectivity of redundant equality conditions.
		 * Later we might think of a reasonable way to combine the estimates,
		 * but for now, just punt, since this is a fairly uncommon situation.
		 */
		if (list_length(removedlist) !=
			(fkinfo->nmatched_ec + fkinfo->nmatched_ri))
		{
			worklist = list_concat(worklist, removedlist);
			continue;
		}

		/*
		 * Finally we get to the payoff: estimate selectivity using the
		 * knowledge that each referencing row will match exactly one row in
		 * the referenced table.
		 *
		 * XXX that's not true in the presence of nulls in the referencing
		 * column(s), so in principle we should derate the estimate for those.
		 * However (1) if there are any strict restriction clauses for the
		 * referencing column(s) elsewhere in the query, derating here would
		 * be double-counting the null fraction, and (2) it's not very clear
		 * how to combine null fractions for multiple referencing columns. So
		 * we do nothing for now about correcting for nulls.
		 *
		 * XXX another point here is that if either side of an FK constraint
		 * is an inheritance parent, we estimate as though the constraint
		 * covers all its children as well.  This is not an unreasonable
		 * assumption for a referencing table, ie the user probably applied
		 * identical constraints to all child tables (though perhaps we ought
		 * to check that).  But it's not possible to have done that for a
		 * referenced table.  Fortunately, precisely because that doesn't
		 * work, it is uncommon in practice to have an FK referencing a parent
		 * table.  So, at least for now, disregard inheritance here.
		 */
		if (jointype == JOIN_SEMI || jointype == JOIN_ANTI)
		{
			/*
			 * For JOIN_SEMI and JOIN_ANTI, we only get here when the FK's
			 * referenced table is exactly the inside of the join.  The join
			 * selectivity is defined as the fraction of LHS rows that have
			 * matches.  The FK implies that every LHS row has a match *in the
			 * referenced table*; but any restriction clauses on it will
			 * reduce the number of matches.  Hence we take the join
			 * selectivity as equal to the selectivity of the table's
			 * restriction clauses, which is rows / tuples; but we must guard
			 * against tuples == 0.
			 */
			RelOptInfo *ref_rel = find_base_rel(root, fkinfo->ref_relid);
			double		ref_tuples = Max(ref_rel->tuples, 1.0);

			fkselec *= ref_rel->rows / ref_tuples;
		}
		else
		{
			/*
			 * Otherwise, selectivity is exactly 1/referenced-table-size; but
			 * guard against tuples == 0.  Note we should use the raw table
			 * tuple count, not any estimate of its filtered or joined size.
			 */
			RelOptInfo *ref_rel = find_base_rel(root, fkinfo->ref_relid);
			double		ref_tuples = Max(ref_rel->tuples, 1.0);

			fkselec *= 1.0 / ref_tuples;
		}
	}

	*restrictlist = worklist;
	return fkselec;
}

/*
 * set_subquery_size_estimates
 *		Set the size estimates for a base relation that is a subquery.
 *
 * The rel's targetlist and restrictinfo list must have been constructed
 * already, and the Paths for the subquery must have been completed.
 * We look at the subquery's PlannerInfo to extract data.
 *
 * We set the same fields as set_baserel_size_estimates.
 */
void
set_subquery_size_estimates(PlannerInfo *root, RelOptInfo *rel)
{
	PlannerInfo *subroot = rel->subroot;
	RelOptInfo *sub_final_rel;
	ListCell   *lc;

	/* Should only be applied to base relations that are subqueries */
	Assert(rel->relid > 0);
	Assert(planner_rt_fetch(rel->relid, root)->rtekind == RTE_SUBQUERY);

	/*
	 * Copy raw number of output rows from subquery.  All of its paths should
	 * have the same output rowcount, so just look at cheapest-total.
	 */
	sub_final_rel = fetch_upper_rel(subroot, UPPERREL_FINAL, NULL);
	rel->tuples = sub_final_rel->cheapest_total_path->rows;

	/*
	 * Compute per-output-column width estimates by examining the subquery's
	 * targetlist.  For any output that is a plain Var, get the width estimate
	 * that was made while planning the subquery.  Otherwise, we leave it to
	 * set_rel_width to fill in a datatype-based default estimate.
	 */
	foreach(lc, subroot->parse->targetList)
	{
		TargetEntry *te = lfirst_node(TargetEntry, lc);
		Node	   *texpr = (Node *) te->expr;
		int32		item_width = 0;

		/* junk columns aren't visible to upper query */
		if (te->resjunk)
			continue;

		/*
		 * The subquery could be an expansion of a view that's had columns
		 * added to it since the current query was parsed, so that there are
		 * non-junk tlist columns in it that don't correspond to any column
		 * visible at our query level.  Ignore such columns.
		 */
		if (te->resno < rel->min_attr || te->resno > rel->max_attr)
			continue;

		/*
		 * XXX This currently doesn't work for subqueries containing set
		 * operations, because the Vars in their tlists are bogus references
		 * to the first leaf subquery, which wouldn't give the right answer
		 * even if we could still get to its PlannerInfo.
		 *
		 * Also, the subquery could be an appendrel for which all branches are
		 * known empty due to constraint exclusion, in which case
		 * set_append_rel_pathlist will have left the attr_widths set to zero.
		 *
		 * In either case, we just leave the width estimate zero until
		 * set_rel_width fixes it.
		 */
		if (IsA(texpr, Var) &&
			subroot->parse->setOperations == NULL)
		{
			Var		   *var = (Var *) texpr;
			RelOptInfo *subrel = find_base_rel(subroot, var->varno);

			item_width = subrel->attr_widths[var->varattno - subrel->min_attr];
		}
		rel->attr_widths[te->resno - rel->min_attr] = item_width;
	}

	/* Now estimate number of output rows, etc */
	set_baserel_size_estimates(root, rel);
}

/*
 * set_function_size_estimates
 *		Set the size estimates for a base relation that is a function call.
 *
 * The rel's targetlist and restrictinfo list must have been constructed
 * already.
 *
 * We set the same fields as set_baserel_size_estimates.
 */
void
set_function_size_estimates(PlannerInfo *root, RelOptInfo *rel)
{
	RangeTblEntry *rte;
	ListCell   *lc;

	/* Should only be applied to base relations that are functions */
	Assert(rel->relid > 0);
	rte = planner_rt_fetch(rel->relid, root);
	Assert(rte->rtekind == RTE_FUNCTION);

	/*
	 * Estimate number of rows the functions will return. The rowcount of the
	 * node is that of the largest function result.
	 */
	rel->tuples = 0;
	foreach(lc, rte->functions)
	{
		RangeTblFunction *rtfunc = (RangeTblFunction *) lfirst(lc);
		double		ntup = expression_returns_set_rows(root, rtfunc->funcexpr);

		if (ntup > rel->tuples)
			rel->tuples = ntup;
	}

	/* Now estimate number of output rows, etc */
	set_baserel_size_estimates(root, rel);
}

/*
 * set_function_size_estimates
 *		Set the size estimates for a base relation that is a function call.
 *
 * The rel's targetlist and restrictinfo list must have been constructed
 * already.
 *
 * We set the same fields as set_tablefunc_size_estimates.
 */
void
set_tablefunc_size_estimates(PlannerInfo *root, RelOptInfo *rel)
{
	/* Should only be applied to base relations that are functions */
	Assert(rel->relid > 0);
	Assert(planner_rt_fetch(rel->relid, root)->rtekind == RTE_TABLEFUNC);

	rel->tuples = 100;

	/* Now estimate number of output rows, etc */
	set_baserel_size_estimates(root, rel);
}

/*
 * set_values_size_estimates
 *		Set the size estimates for a base relation that is a values list.
 *
 * The rel's targetlist and restrictinfo list must have been constructed
 * already.
 *
 * We set the same fields as set_baserel_size_estimates.
 */
void
set_values_size_estimates(PlannerInfo *root, RelOptInfo *rel)
{
	RangeTblEntry *rte;

	/* Should only be applied to base relations that are values lists */
	Assert(rel->relid > 0);
	rte = planner_rt_fetch(rel->relid, root);
	Assert(rte->rtekind == RTE_VALUES);

	/*
	 * Estimate number of rows the values list will return. We know this
	 * precisely based on the list length (well, barring set-returning
	 * functions in list items, but that's a refinement not catered for
	 * anywhere else either).
	 */
	rel->tuples = list_length(rte->values_lists);

	/* Now estimate number of output rows, etc */
	set_baserel_size_estimates(root, rel);
}

/*
 * set_cte_size_estimates
 *		Set the size estimates for a base relation that is a CTE reference.
 *
 * The rel's targetlist and restrictinfo list must have been constructed
 * already, and we need an estimate of the number of rows returned by the CTE
 * (if a regular CTE) or the non-recursive term (if a self-reference).
 *
 * We set the same fields as set_baserel_size_estimates.
 */
void
set_cte_size_estimates(PlannerInfo *root, RelOptInfo *rel, double cte_rows)
{
	RangeTblEntry *rte;

	/* Should only be applied to base relations that are CTE references */
	Assert(rel->relid > 0);
	rte = planner_rt_fetch(rel->relid, root);
	Assert(rte->rtekind == RTE_CTE);

	if (rte->self_reference)
	{
		/*
		 * In a self-reference, arbitrarily assume the average worktable size
		 * is about 10 times the nonrecursive term's size.
		 */
		rel->tuples = 10 * cte_rows;
	}
	else
	{
		/* Otherwise just believe the CTE's rowcount estimate */
		rel->tuples = cte_rows;
	}

	/* Now estimate number of output rows, etc */
	set_baserel_size_estimates(root, rel);
}

/*
 * set_namedtuplestore_size_estimates
 *		Set the size estimates for a base relation that is a tuplestore reference.
 *
 * The rel's targetlist and restrictinfo list must have been constructed
 * already.
 *
 * We set the same fields as set_baserel_size_estimates.
 */
void
set_namedtuplestore_size_estimates(PlannerInfo *root, RelOptInfo *rel)
{
	RangeTblEntry *rte;

	/* Should only be applied to base relations that are tuplestore references */
	Assert(rel->relid > 0);
	rte = planner_rt_fetch(rel->relid, root);
	Assert(rte->rtekind == RTE_NAMEDTUPLESTORE);

	/*
	 * Use the estimate provided by the code which is generating the named
	 * tuplestore.  In some cases, the actual number might be available; in
	 * others the same plan will be re-used, so a "typical" value might be
	 * estimated and used.
	 */
	rel->tuples = rte->enrtuples;
	if (rel->tuples < 0)
		rel->tuples = 1000;

	/* Now estimate number of output rows, etc */
	set_baserel_size_estimates(root, rel);
}

/*
 * set_result_size_estimates
 *		Set the size estimates for an RTE_RESULT base relation
 *
 * The rel's targetlist and restrictinfo list must have been constructed
 * already.
 *
 * We set the same fields as set_baserel_size_estimates.
 */
void
set_result_size_estimates(PlannerInfo *root, RelOptInfo *rel)
{
	/* Should only be applied to RTE_RESULT base relations */
	Assert(rel->relid > 0);
	Assert(planner_rt_fetch(rel->relid, root)->rtekind == RTE_RESULT);

	/* RTE_RESULT always generates a single row, natively */
	rel->tuples = 1;

	/* Now estimate number of output rows, etc */
	set_baserel_size_estimates(root, rel);
}

/*
 * set_foreign_size_estimates
 *		Set the size estimates for a base relation that is a foreign table.
 *
 * There is not a whole lot that we can do here; the foreign-data wrapper
 * is responsible for producing useful estimates.  We can do a decent job
 * of estimating baserestrictcost, so we set that, and we also set up width
 * using what will be purely datatype-driven estimates from the targetlist.
 * There is no way to do anything sane with the rows value, so we just put
 * a default estimate and hope that the wrapper can improve on it.  The
 * wrapper's GetForeignRelSize function will be called momentarily.
 *
 * The rel's targetlist and restrictinfo list must have been constructed
 * already.
 */
void
set_foreign_size_estimates(PlannerInfo *root, RelOptInfo *rel)
{
	/* Should only be applied to base relations */
	Assert(rel->relid > 0);

	rel->rows = 1000;			/* entirely bogus default estimate */

	cost_qual_eval(&rel->baserestrictcost, rel->baserestrictinfo, root);

	set_rel_width(root, rel);
}


/*
 * set_rel_width
 *		Set the estimated output width of a base relation.
 *
 * The estimated output width is the sum of the per-attribute width estimates
 * for the actually-referenced columns, plus any PHVs or other expressions
 * that have to be calculated at this relation.  This is the amount of data
 * we'd need to pass upwards in case of a sort, hash, etc.
 *
 * This function also sets reltarget->cost, so it's a bit misnamed now.
 *
 * NB: this works best on plain relations because it prefers to look at
 * real Vars.  For subqueries, set_subquery_size_estimates will already have
 * copied up whatever per-column estimates were made within the subquery,
 * and for other types of rels there isn't much we can do anyway.  We fall
 * back on (fairly stupid) datatype-based width estimates if we can't get
 * any better number.
 *
 * The per-attribute width estimates are cached for possible re-use while
 * building join relations or post-scan/join pathtargets.
 */
static void
set_rel_width(PlannerInfo *root, RelOptInfo *rel)
{
	Oid			reloid = planner_rt_fetch(rel->relid, root)->relid;
	int32		tuple_width = 0;
	bool		have_wholerow_var = false;
	ListCell   *lc;

	/* Vars are assumed to have cost zero, but other exprs do not */
	rel->reltarget->cost.startup = 0;
	rel->reltarget->cost.per_tuple = 0;

	foreach(lc, rel->reltarget->exprs)
	{
		Node	   *node = (Node *) lfirst(lc);

		/*
		 * Ordinarily, a Var in a rel's targetlist must belong to that rel;
		 * but there are corner cases involving LATERAL references where that
		 * isn't so.  If the Var has the wrong varno, fall through to the
		 * generic case (it doesn't seem worth the trouble to be any smarter).
		 */
		if (IsA(node, Var) &&
			((Var *) node)->varno == rel->relid)
		{
			Var		   *var = (Var *) node;
			int			ndx;
			int32		item_width;

			Assert(var->varattno >= rel->min_attr);
			Assert(var->varattno <= rel->max_attr);

			ndx = var->varattno - rel->min_attr;

			/*
			 * If it's a whole-row Var, we'll deal with it below after we have
			 * already cached as many attr widths as possible.
			 */
			if (var->varattno == 0)
			{
				have_wholerow_var = true;
				continue;
			}

			/*
			 * The width may have been cached already (especially if it's a
			 * subquery), so don't duplicate effort.
			 */
			if (rel->attr_widths[ndx] > 0)
			{
				tuple_width += rel->attr_widths[ndx];
				continue;
			}

			/* Try to get column width from statistics */
			if (reloid != InvalidOid && var->varattno > 0)
			{
				item_width = get_attavgwidth(reloid, var->varattno);
				if (item_width > 0)
				{
					rel->attr_widths[ndx] = item_width;
					tuple_width += item_width;
					continue;
				}
			}

			/*
			 * Not a plain relation, or can't find statistics for it. Estimate
			 * using just the type info.
			 */
			item_width = get_typavgwidth(var->vartype, var->vartypmod);
			Assert(item_width > 0);
			rel->attr_widths[ndx] = item_width;
			tuple_width += item_width;
		}
		else if (IsA(node, PlaceHolderVar))
		{
			/*
			 * We will need to evaluate the PHV's contained expression while
			 * scanning this rel, so be sure to include it in reltarget->cost.
			 */
			PlaceHolderVar *phv = (PlaceHolderVar *) node;
			PlaceHolderInfo *phinfo = find_placeholder_info(root, phv, false);
			QualCost	cost;

			tuple_width += phinfo->ph_width;
			cost_qual_eval_node(&cost, (Node *) phv->phexpr, root);
			rel->reltarget->cost.startup += cost.startup;
			rel->reltarget->cost.per_tuple += cost.per_tuple;
		}
		else
		{
			/*
			 * We could be looking at an expression pulled up from a subquery,
			 * or a ROW() representing a whole-row child Var, etc.  Do what we
			 * can using the expression type information.
			 */
			int32		item_width;
			QualCost	cost;

			item_width = get_typavgwidth(exprType(node), exprTypmod(node));
			Assert(item_width > 0);
			tuple_width += item_width;
			/* Not entirely clear if we need to account for cost, but do so */
			cost_qual_eval_node(&cost, node, root);
			rel->reltarget->cost.startup += cost.startup;
			rel->reltarget->cost.per_tuple += cost.per_tuple;
		}
	}

	/*
	 * If we have a whole-row reference, estimate its width as the sum of
	 * per-column widths plus heap tuple header overhead.
	 */
	if (have_wholerow_var)
	{
		int32		wholerow_width = MAXALIGN(SizeofHeapTupleHeader);

		if (reloid != InvalidOid)
		{
			/* Real relation, so estimate true tuple width */
			wholerow_width += get_relation_data_width(reloid,
													  rel->attr_widths - rel->min_attr);
		}
		else
		{
			/* Do what we can with info for a phony rel */
			AttrNumber	i;

			for (i = 1; i <= rel->max_attr; i++)
				wholerow_width += rel->attr_widths[i - rel->min_attr];
		}

		rel->attr_widths[0 - rel->min_attr] = wholerow_width;

		/*
		 * Include the whole-row Var as part of the output tuple.  Yes, that
		 * really is what happens at runtime.
		 */
		tuple_width += wholerow_width;
	}

	Assert(tuple_width >= 0);
	rel->reltarget->width = tuple_width;
}

/*
 * set_pathtarget_cost_width
 *		Set the estimated eval cost and output width of a PathTarget tlist.
 *
 * As a notational convenience, returns the same PathTarget pointer passed in.
 *
 * Most, though not quite all, uses of this function occur after we've run
 * set_rel_width() for base relations; so we can usually obtain cached width
 * estimates for Vars.  If we can't, fall back on datatype-based width
 * estimates.  Present early-planning uses of PathTargets don't need accurate
 * widths badly enough to justify going to the catalogs for better data.
 */
PathTarget *
set_pathtarget_cost_width(PlannerInfo *root, PathTarget *target)
{
	int32		tuple_width = 0;
	ListCell   *lc;

	/* Vars are assumed to have cost zero, but other exprs do not */
	target->cost.startup = 0;
	target->cost.per_tuple = 0;

	foreach(lc, target->exprs)
	{
		Node	   *node = (Node *) lfirst(lc);

		if (IsA(node, Var))
		{
			Var		   *var = (Var *) node;
			int32		item_width;

			/* We should not see any upper-level Vars here */
			Assert(var->varlevelsup == 0);

			/* Try to get data from RelOptInfo cache */
			if (var->varno < root->simple_rel_array_size)
			{
				RelOptInfo *rel = root->simple_rel_array[var->varno];

				if (rel != NULL &&
					var->varattno >= rel->min_attr &&
					var->varattno <= rel->max_attr)
				{
					int			ndx = var->varattno - rel->min_attr;

					if (rel->attr_widths[ndx] > 0)
					{
						tuple_width += rel->attr_widths[ndx];
						continue;
					}
				}
			}

			/*
			 * No cached data available, so estimate using just the type info.
			 */
			item_width = get_typavgwidth(var->vartype, var->vartypmod);
			Assert(item_width > 0);
			tuple_width += item_width;
		}
		else
		{
			/*
			 * Handle general expressions using type info.
			 */
			int32		item_width;
			QualCost	cost;

			item_width = get_typavgwidth(exprType(node), exprTypmod(node));
			Assert(item_width > 0);
			tuple_width += item_width;

			/* Account for cost, too */
			cost_qual_eval_node(&cost, node, root);
			target->cost.startup += cost.startup;
			target->cost.per_tuple += cost.per_tuple;
		}
	}

	Assert(tuple_width >= 0);
	target->width = tuple_width;

	return target;
}

/*
 * relation_byte_size
 *	  Estimate the storage space in bytes for a given number of tuples
 *	  of a given width (size in bytes).
 */
static double
relation_byte_size(double tuples, int width)
{
	return tuples * (MAXALIGN(width) + MAXALIGN(SizeofHeapTupleHeader));
}

/*
 * page_size
 *    计算给定数量和宽度的元组所占用的数据页面数量的估计值。
 * 
 * 此函数是PostgreSQL查询优化器中用于成本估算的辅助函数，主要用于计算
 * 关系数据在磁盘上的物理存储需求，这对于估算I/O成本非常重要。
 * 
 * 参数说明：
 *    tuples - 元组的数量（行数）
 *    width - 每个元组的宽度（以字节为单位）
 * 
 * 返回值：
 *    double - 返回向上取整后的页面数量
 * 
 * 实现说明：
 *    1. 首先调用relation_byte_size计算所有元组的总字节大小
 *    2. 然后将总字节数除以BLCKSZ（PostgreSQL块大小，通常为8KB）
 *    3. 使用ceil函数向上取整，确保即使有部分填充的页面也被计入
 */
static double
page_size(double tuples, int width)
{
    return ceil(relation_byte_size(tuples, width) / BLCKSZ);
}

/*
 * Estimate the fraction of the work that each worker will do given the
 * number of workers budgeted for the path.
 */
static double
get_parallel_divisor(Path *path)
{
	double		parallel_divisor = path->parallel_workers;

	/*
	 * Early experience with parallel query suggests that when there is only
	 * one worker, the leader often makes a very substantial contribution to
	 * executing the parallel portion of the plan, but as more workers are
	 * added, it does less and less, because it's busy reading tuples from the
	 * workers and doing whatever non-parallel post-processing is needed.  By
	 * the time we reach 4 workers, the leader no longer makes a meaningful
	 * contribution.  Thus, for now, estimate that the leader spends 30% of
	 * its time servicing each worker, and the remainder executing the
	 * parallel plan.
	 */
	if (parallel_leader_participation)
	{
		double		leader_contribution;

		leader_contribution = 1.0 - (0.3 * path->parallel_workers);
		if (leader_contribution > 0)
			parallel_divisor += leader_contribution;
	}

	return parallel_divisor;
}

/*
 * compute_bitmap_pages
 *
 * compute number of pages fetched from heap in bitmap heap scan.
 */
double
compute_bitmap_pages(PlannerInfo *root, RelOptInfo *baserel, Path *bitmapqual,
					 int loop_count, Cost *cost, double *tuple)
{
	Cost		indexTotalCost;
	Selectivity indexSelectivity;
	double		T;
	double		pages_fetched;
	double		tuples_fetched;
	double		heap_pages;
	long		maxentries;

	/*
	 * Fetch total cost of obtaining the bitmap, as well as its total
	 * selectivity.
	 */
	cost_bitmap_tree_node(bitmapqual, &indexTotalCost, &indexSelectivity);

	/*
	 * Estimate number of main-table pages fetched.
	 */
	tuples_fetched = clamp_row_est(indexSelectivity * baserel->tuples);

	T = (baserel->pages > 1) ? (double) baserel->pages : 1.0;

	/*
	 * For a single scan, the number of heap pages that need to be fetched is
	 * the same as the Mackert and Lohman formula for the case T <= b (ie, no
	 * re-reads needed).
	 */
	pages_fetched = (2.0 * T * tuples_fetched) / (2.0 * T + tuples_fetched);

	/*
	 * Calculate the number of pages fetched from the heap.  Then based on
	 * current work_mem estimate get the estimated maxentries in the bitmap.
	 * (Note that we always do this calculation based on the number of pages
	 * that would be fetched in a single iteration, even if loop_count > 1.
	 * That's correct, because only that number of entries will be stored in
	 * the bitmap at one time.)
	 */
	heap_pages = Min(pages_fetched, baserel->pages);
	maxentries = tbm_calculate_entries(work_mem * 1024L);

	if (loop_count > 1)
	{
		/*
		 * For repeated bitmap scans, scale up the number of tuples fetched in
		 * the Mackert and Lohman formula by the number of scans, so that we
		 * estimate the number of pages fetched by all the scans. Then
		 * pro-rate for one scan.
		 */
		pages_fetched = index_pages_fetched(tuples_fetched * loop_count,
											baserel->pages,
											get_indexpath_pages(bitmapqual),
											root);
		pages_fetched /= loop_count;
	}

	if (pages_fetched >= T)
		pages_fetched = T;
	else
		pages_fetched = ceil(pages_fetched);

	if (maxentries < heap_pages)
	{
		double		exact_pages;
		double		lossy_pages;

		/*
		 * Crude approximation of the number of lossy pages.  Because of the
		 * way tbm_lossify() is coded, the number of lossy pages increases
		 * very sharply as soon as we run short of memory; this formula has
		 * that property and seems to perform adequately in testing, but it's
		 * possible we could do better somehow.
		 */
		lossy_pages = Max(0, heap_pages - maxentries / 2);
		exact_pages = heap_pages - lossy_pages;

		/*
		 * If there are lossy pages then recompute the  number of tuples
		 * processed by the bitmap heap node.  We assume here that the chance
		 * of a given tuple coming from an exact page is the same as the
		 * chance that a given page is exact.  This might not be true, but
		 * it's not clear how we can do any better.
		 */
		if (lossy_pages > 0)
			tuples_fetched =
				clamp_row_est(indexSelectivity *
							  (exact_pages / heap_pages) * baserel->tuples +
							  (lossy_pages / heap_pages) * baserel->tuples);
	}

	if (cost)
		*cost = indexTotalCost;
	if (tuple)
		*tuple = tuples_fetched;

	return pages_fetched;
}
