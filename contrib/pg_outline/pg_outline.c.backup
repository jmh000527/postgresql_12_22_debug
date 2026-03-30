/*-------------------------------------------------------------------------
 *
 * pg_outline.c
 *		Plan stabilization through execution plan outline hints
 *
 * This extension provides functionality similar to OceanBase's Outline feature.
 * It captures execution plans as hints and stores them, allowing plan
 * stabilization across query executions.
 *
 * The implementation uses PostgreSQL's planner and executor hooks to:
 * 1. Generate hints from execution plans (auto-generation)
 * 2. Store outline hints for specific queries
 * 3. Apply stored hints during query planning
 * 4. Display generated outline data after query execution
 *
 * Copyright (c) 2024, PostgreSQL Extension Development
 *
 * IDENTIFICATION
 *	  contrib/pg_outline/pg_outline.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "common/md5.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "optimizer/paths.h"
#include "optimizer/pathnode.h"
#include "parser/analyze.h"
#include "parser/parser.h"
#include "parser/parsetree.h"
#include "portability/instr_time.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/hashutils.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "common/md5.h"
#include "catalog/pg_class.h"

PG_MODULE_MAGIC;

/* GUC variables */
static bool pg_outline_enabled = true;
static bool pg_outline_display_hints = true;
static char *pg_outline_mode = "auto";  /* auto, manual, off */

/* Saved hook values */
static planner_hook_type prev_planner_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static ExplainOneQuery_hook_type prev_ExplainOneQuery_hook = NULL;
static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook = NULL;
static join_search_hook_type prev_join_search_hook = NULL;

/* Recursion protection flag */
static bool inside_outline_planner = false;

/* Active hints for current query - used during planning */
static List *active_outline_hints = NIL;

/* Data structures for outline hints */
typedef enum OutlineHintType
{
	HINT_SCAN_METHOD,
	HINT_JOIN_METHOD,
	HINT_JOIN_ORDER,
	HINT_PARALLEL
} OutlineHintType;

typedef struct OutlineHint
{
	OutlineHintType type;
	char *hint_string;
	List *rel_names;
	char *index_name;
	int join_method;  /* NESTLOOP, HASHJOIN, MERGEJOIN */
	int scan_method;  /* SEQSCAN, INDEXSCAN, INDEXONLYSCAN, BITMAPSCAN */
} OutlineHint;

typedef struct OutlineInfo
{
	char *query_string;
	Query *query;  /* Query tree for collecting hints with query names */
	List *hints;  /* List of OutlineHint */
	StringInfo outline_data;
	bool displayed;  /* Whether outline data has been displayed */
} OutlineInfo;

/* Structure to hold hint-to-Query position mapping */
typedef struct QueryHintPosition
{
	int			hint_location;	/* Location in source where this hint begins */
	int			hint_end;		/* Location where this hint ends */
	char	   *hint_text;		/* Hint text extracted from comment */
	bool		consumed;		/* true if this hint has been matched to a Query */
} QueryHintPosition;

/* Memory context for pg_outline data that persists across queries */
static MemoryContext OutlineContext = NULL;

/* Current outline being processed */
static OutlineInfo *current_outline = NULL;

/* Current PlannedStmt for relation name resolution */
static PlannedStmt *current_plannedstmt = NULL;

/*
 * Query jumbling infrastructure (adapted from pg_stat_statements)
 * Used to compute unique hashes for Query structures without modifying them.
 */
#define JUMBLE_SIZE				1024	/* query serialization buffer size */

/* Working state for computing a query jumble */
typedef struct OutlineJumbleState
{
	unsigned char *jumble;		/* Jumble of current query tree */
	Size		jumble_len;			/* Number of bytes used in jumble[] */
} OutlineJumbleState;

/*
 * Query metadata entry - stores the mapping from query hash to metadata
 * This replaces the invasive approach of adding fields to Query structure.
 */
typedef struct QueryMetadataEntry
{
	uint64		query_hash;			/* Hash computed by jumbleQuery (KEY) */
	int			stmt_location;		/* Query stmt_location for disambiguation */
	char	   *query_name;			/* Unique name (main, cte_orders, etc.) */
	char	   *query_hints;		/* Hint string for this Query */
	int			query_index;		/* Sequential index for ordering */
	struct QueryMetadataEntry *next;  /* For handling collisions */
} QueryMetadataEntry;

/* Simple hash table for Query metadata */
#define QUERY_METADATA_HASH_SIZE 128

typedef struct QueryMetadataHashTable
{
	QueryMetadataEntry *buckets[QUERY_METADATA_HASH_SIZE];
	MemoryContext	memory_context;	/* Context for allocations */
	int			entry_count;		/* Number of entries */
} QueryMetadataHashTable;

/* Current query metadata hash table - created per query execution */
static QueryMetadataHashTable *current_query_metadata = NULL;

/* Function declarations */
void		_PG_init(void);
void		_PG_fini(void);

static PlannedStmt *outline_planner(Query *parse, int cursorOptions,
									ParamListInfo boundParams);
static void outline_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void outline_ExecutorEnd(QueryDesc *queryDesc);
static void outline_ExplainOneQuery(Query *query, int cursorOptions,
									IntoClause *into, ExplainState *es,
									const char *queryString, ParamListInfo params,
									QueryEnvironment *queryEnv);

static void generate_outline_from_plan(PlannedStmt *plan, const char *query_string);
static void extract_hints_from_plan_tree(Plan *plan, List **hints, int level);
static char *get_scan_method_hint(Plan *plan);
static char *get_join_method_hint(Plan *plan);
static char *get_leading_hint(Plan *plan);
static char *get_leading_hint_from_join(Plan *plan);
static char *extract_relation_names_from_plan(Plan *plan);
static char *extract_relation_names_flat(Plan *plan);
static char *get_relation_name(Index relid, PlannedStmt *plan);
static void display_outline_data(void);
static StringInfo format_outline_data(List *hints);
static char *construct_sql_with_hints(const char *query_string, const char *hints_string);
static char *reconstruct_sql_with_positioned_hints(const char *query_string, const char *hints_string);

/* Query fingerprinting */
static char *normalize_query(const char *query_string);
static char *compute_query_fingerprint(const char *normalized_query);
static char *get_short_fingerprint(const char *fingerprint);

/* Hint storage and retrieval */
static void store_outline_hints(const char *outline_name, const char *query_pattern,
								const char *fingerprint, const char *hints);
static void store_outline_with_query_hints(const char *outline_name, const char *query_pattern,
										   const char *fingerprint, Query *query);
static char *retrieve_outline_hints(const char *fingerprint);
static bool outline_exists(const char *outline_name);

/* Inline hint extraction */
static List *extract_inline_hints_with_positions(const char *query_string);
static char *strip_hints_from_query(const char *query_string);
static void assign_hints_to_queries(Query *query, List *hint_positions, const char *source_text);
static char *find_hint_for_query_location(List *hint_positions, int stmt_location);

/* Hint parsing and application for stored outlines */
typedef struct ParsedHint
{
	char *query_name;  /* e.g., "main", "sublink_0" */
	char *hint_text;   /* e.g., "SeqScan(t1)", "IndexScan(t2)" */
} ParsedHint;

static List *parse_stored_hints(const char *hints_string);
static void apply_hints_to_query(Query *query, List *parsed_hints);
static void outline_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte);
static RelOptInfo *outline_join_search(PlannerInfo *root, int levels_needed, List *initial_rels);

/* Leading hint parsing structures and functions */
typedef enum LeadingHintNodeType
{
	LEADING_NODE_RELATION,   /* Leaf node: a single relation name */
	LEADING_NODE_JOIN        /* Internal node: represents a join of two subtrees */
} LeadingHintNodeType;

typedef struct LeadingHintNode
{
	LeadingHintNodeType type;
	union
	{
		char *relation_name;          /* For LEADING_NODE_RELATION */
		struct
		{
			struct LeadingHintNode *left;   /* For LEADING_NODE_JOIN */
			struct LeadingHintNode *right;
		} join;
	} u;
} LeadingHintNode;

static LeadingHintNode *parse_leading_hint(const char *hint_str);
static LeadingHintNode *parse_leading_hint_recursive(const char **str_ptr);
static void free_leading_hint_tree(LeadingHintNode *node);
static char *leading_hint_node_to_string(LeadingHintNode *node);
static void leading_hint_node_to_string_buf(LeadingHintNode *node, StringInfo buf);

/* Query naming support */
typedef struct QueryNamingContext
{
	int main_query_count;    /* Counter for main queries */
	int cte_count;           /* Counter for CTEs */
	int subquery_count;      /* Counter for subqueries */
	int sublink_count;       /* Counter for sublinks */
	int query_index;         /* Sequential index for all queries in this tree */
} QueryNamingContext;

static void assign_query_names(Query *query, QueryNamingContext *context, const char *parent_name, const char *cte_name);
static char *generate_query_name(QueryNamingContext *context, const char *type, const char *cte_name);

/* Query jumbling and hash table functions */
static void AppendJumble(OutlineJumbleState *jstate, const unsigned char *item, Size size);
static void JumbleQuery(OutlineJumbleState *jstate, Query *query);
static void JumbleRangeTable(OutlineJumbleState *jstate, List *rtable);
static void JumbleExpr(OutlineJumbleState *jstate, Node *node);
static uint64 compute_query_hash(Query *query);

static QueryMetadataHashTable *create_query_metadata_table(MemoryContext context);
static void destroy_query_metadata_table(QueryMetadataHashTable *table);
static void store_query_metadata(QueryMetadataHashTable *table, Query *query,
								 const char *query_name, const char *query_hints, int query_index);
static QueryMetadataEntry *lookup_query_metadata(QueryMetadataHashTable *table, Query *query);
static char *get_query_name(Query *query);
static char *get_query_hints(Query *query);

/* Query hint collection support */
typedef struct QueryHintCollector
{
	List	   *hints;		/* List of strings: each Query's hints */
	int			query_index; /* Current Query index */
} QueryHintCollector;

static void collect_query_hints_recursive(Query *query, QueryHintCollector *collector);
static void extract_hints_from_plan_with_query_names(PlannedStmt *plannedstmt, Query *query, List **hints);
static void log_query_names_and_hints(Query *query);

/* SubLink-to-relation mapping for handling pulled-up subqueries */
typedef struct RelationSubLinkMap RelationSubLinkMap;
typedef struct RelationSubLinkEntry RelationSubLinkEntry;
typedef struct RelSubLinkMapContext RelSubLinkMapContext;

static RelationSubLinkMap *build_relation_sublink_map(Query *query);
static int get_relation_sublink_index(RelationSubLinkMap *map, Oid relid);
static char *extract_relation_from_hint(const char *hint);
static Oid get_relation_oid_from_name(const char *relname, PlannedStmt *plannedstmt);
static int get_hint_sublink_index(const char *hint, RelationSubLinkMap *relmap, PlannedStmt *plannedstmt);
static bool build_relation_sublink_map_walker(Node *node, RelSubLinkMapContext *context);



/* SQL-callable functions */
PG_FUNCTION_INFO_V1(pg_outline_create);
PG_FUNCTION_INFO_V1(pg_outline_create_from_sql);
PG_FUNCTION_INFO_V1(pg_outline_update);
PG_FUNCTION_INFO_V1(pg_outline_drop);
PG_FUNCTION_INFO_V1(pg_outline_enable);
PG_FUNCTION_INFO_V1(pg_outline_disable);
PG_FUNCTION_INFO_V1(pg_outline_list);

/*
 * Module load callback
 */
void
_PG_init(void)
{
	/* Define custom GUC variables */
	DefineCustomBoolVariable("pg_outline.enabled",
							 "Enable pg_outline extension",
							 NULL,
							 &pg_outline_enabled,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_outline.display_hints",
							 "Display generated outline hints after query execution",
							 NULL,
							 &pg_outline_display_hints,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomStringVariable("pg_outline.mode",
							   "Outline generation mode: auto, manual, or off",
							   NULL,
							   &pg_outline_mode,
							   "auto",
							   PGC_USERSET,
							   0,
							   NULL,
							   NULL,
							   NULL);

	/* Install hooks */
	prev_planner_hook = planner_hook;
	planner_hook = outline_planner;

	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = outline_ExecutorStart;

	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = outline_ExecutorEnd;

	prev_ExplainOneQuery_hook = ExplainOneQuery_hook;
	ExplainOneQuery_hook = outline_ExplainOneQuery;

	prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = outline_set_rel_pathlist;

	prev_join_search_hook = join_search_hook;
	join_search_hook = outline_join_search;

	/* Create memory context for outline data */
	OutlineContext = AllocSetContextCreate(TopMemoryContext,
										   "OutlineContext",
										   ALLOCSET_DEFAULT_SIZES);

	elog(LOG, "pg_outline extension loaded");
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
	/* Restore hooks */
	planner_hook = prev_planner_hook;
	ExecutorStart_hook = prev_ExecutorStart;
	ExecutorEnd_hook = prev_ExecutorEnd;
	ExplainOneQuery_hook = prev_ExplainOneQuery_hook;
	set_rel_pathlist_hook = prev_set_rel_pathlist_hook;
	join_search_hook = prev_join_search_hook;

	elog(LOG, "pg_outline extension unloaded");
}

/*
 * Planner hook: generate outline hints from the plan and apply stored hints
 */
static PlannedStmt *
outline_planner(Query *parse, int cursorOptions, ParamListInfo boundParams)
{
	PlannedStmt *result;
	char	   *query_fingerprint = NULL;
	char	   *stored_hints = NULL;

	/* Prevent infinite recursion when SPI executes internal queries */
	if (inside_outline_planner)
	{
		if (prev_planner_hook)
			return prev_planner_hook(parse, cursorOptions, boundParams);
		else
			return standard_planner(parse, cursorOptions, boundParams);
	}

	inside_outline_planner = true;

	/* Always assign query names before planning if pg_outline is enabled */
	if (pg_outline_enabled && parse)
	{
		/* Create or reuse the hash table for Query metadata */
		if (current_query_metadata == NULL)
			current_query_metadata = create_query_metadata_table(TopMemoryContext);

		/* Assign names to all Query structures in the tree BEFORE optimization */
		{
			QueryNamingContext naming_context;
			memset(&naming_context, 0, sizeof(QueryNamingContext));
			assign_query_names(parse, &naming_context, NULL, NULL);
			elog(DEBUG1, "pg_outline: assigned query names before optimization");
		}
	}

	/* Try to retrieve stored hints if in manual mode */
	if (pg_outline_enabled && strcmp(pg_outline_mode, "manual") == 0 && debug_query_string)
	{
		char	   *normalized = normalize_query(debug_query_string);

		if (normalized)
		{
			query_fingerprint = compute_query_fingerprint(normalized);
			if (query_fingerprint)
			{
				elog(DEBUG1, "pg_outline: computed fingerprint: %s", query_fingerprint);
				stored_hints = retrieve_outline_hints(query_fingerprint);
				if (stored_hints)
					elog(NOTICE, "pg_outline: retrieved hints (len=%d): '%s'", (int)strlen(stored_hints), stored_hints);
				else
					elog(DEBUG1, "pg_outline: retrieved hints: (null)");
				if (stored_hints)
				{
					char *sql_with_hints;

					elog(DEBUG1, "pg_outline: found stored hints for query");

					/* Construct SQL with hints injected at original positions */
					sql_with_hints = reconstruct_sql_with_positioned_hints(debug_query_string, stored_hints);
					if (sql_with_hints)
					{
						elog(NOTICE, "Outline matched! SQL with hints inserted at original positions:\n%s", sql_with_hints);
						pfree(sql_with_hints);
					}

					/* Parse and apply hints before planning */
					active_outline_hints = parse_stored_hints(stored_hints);
					if (active_outline_hints != NIL)
					{
						elog(DEBUG1, "pg_outline: parsed %d hints from stored outline", list_length(active_outline_hints));
						apply_hints_to_query(parse, active_outline_hints);
					}
				}
				else
				{
					elog(DEBUG1, "pg_outline: no stored hints found for fingerprint %s", query_fingerprint);
				}
			}
			pfree(normalized);
		}
	}

	/* Call previous hook or standard planner */
	if (prev_planner_hook)
		result = prev_planner_hook(parse, cursorOptions, boundParams);
	else
		result = standard_planner(parse, cursorOptions, boundParams);

	/* Clean up active hints after planning */
	active_outline_hints = NIL;

	/* Save PlannedStmt for relation name resolution */
	current_plannedstmt = result;

	/* Note: In auto mode, outline generation is only done for EXPLAIN statements
	 * in the ExplainOneQuery hook. Normal queries don't generate outlines. */

	inside_outline_planner = false;
	return result;
}

/*
 * ExecutorStart hook: initialize outline tracking
 */
static void
outline_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	/* Initialize current outline if needed */
	if (pg_outline_enabled && current_outline == NULL)
	{
		current_outline = (OutlineInfo *) palloc0(sizeof(OutlineInfo));
		current_outline->hints = NIL;
		current_outline->outline_data = makeStringInfo();
	}

	/* Call previous hook or standard ExecutorStart */
	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

/*
 * ExecutorEnd hook: no cleanup needed
 */
static void
outline_ExecutorEnd(QueryDesc *queryDesc)
{
	/* Note: We do NOT clean up current_outline here.
	 * For EXPLAIN statements, the ExplainOneQuery hook will clean up after displaying.
	 * For normal queries, memory will be freed at transaction end since it's in TopMemoryContext.
	 * This avoids the issue where ExecutorEnd is called before ExplainOneQuery can display. */

	/* Call previous hook or standard ExecutorEnd */
	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/*
 * ExplainOneQuery hook: display and store outline data for EXPLAIN statements
 */
static void
outline_ExplainOneQuery(Query *query, int cursorOptions, IntoClause *into,
						ExplainState *es, const char *queryString,
						ParamListInfo params, QueryEnvironment *queryEnv)
{
	PlannedStmt *plan = NULL;

	/* Call the previous hook or default behavior first */
	if (prev_ExplainOneQuery_hook)
		(*prev_ExplainOneQuery_hook)(query, cursorOptions, into, es,
									 queryString, params, queryEnv);
	else
	{
		/* Default EXPLAIN behavior */
		instr_time	planstart,
					planduration;

		INSTR_TIME_SET_CURRENT(planstart);

		/* plan the query */
		plan = pg_plan_query(query, cursorOptions, params);

		INSTR_TIME_SET_CURRENT(planduration);
		INSTR_TIME_SUBTRACT(planduration, planstart);

		/* run it (if needed) and produce output */
		ExplainOnePlan(plan, into, es, queryString, params, queryEnv,
					   &planduration);
	}

	/* In auto mode, generate and display outline for EXPLAIN statements */
	if (pg_outline_enabled && strcmp(pg_outline_mode, "auto") == 0 && queryString)
	{
		MemoryContext oldcontext;
		List *hint_positions = NIL;

		/* Initialize current outline if needed */
		if (current_outline == NULL)
		{
			oldcontext = MemoryContextSwitchTo(OutlineContext);
			current_outline = (OutlineInfo *) palloc0(sizeof(OutlineInfo));
			current_outline->hints = NIL;
			MemoryContextSwitchTo(oldcontext);
		}

		/* Extract inline hints from the query string */
		hint_positions = extract_inline_hints_with_positions(queryString);

		/* Always assign query names if we have a query tree, so hints can be prefixed */
		if (query)
		{
			/* Create a hash table for Query metadata (names and hints) if needed */
			if (current_query_metadata == NULL)
				current_query_metadata = create_query_metadata_table(OutlineContext);

			/* Assign names to all Query structures in the tree if not already named */
			/* Check if the top-level query already has a name (from planner hook) */
			if (lookup_query_metadata(current_query_metadata, query) == NULL)
			{
				QueryNamingContext naming_context;
				memset(&naming_context, 0, sizeof(QueryNamingContext));
				assign_query_names(query, &naming_context, NULL, NULL);
			}

			/* If we have inline hints, assign them to Query structures based on their locations */
			if (list_length(hint_positions) > 0)
			{
				assign_hints_to_queries(query, hint_positions, queryString);

				/* Log Query names and their hints for debugging */
				elog(NOTICE, "=== Query Structure Analysis ===");
				log_query_names_and_hints(query);
				elog(NOTICE, "=== End of Query Structure Analysis ===");
			}

			/* Store the Query tree in current_outline for later use */
			oldcontext = MemoryContextSwitchTo(OutlineContext);
			current_outline->query = query;
			MemoryContextSwitchTo(oldcontext);
		}

		/* Get the plan if we don't have it yet */
		if (plan == NULL)
			plan = current_plannedstmt;

		if (plan)
		{

			/* Generate outline from the plan - must be in OutlineContext
			 * so hint strings survive across memory context resets */
			oldcontext = MemoryContextSwitchTo(OutlineContext);
			generate_outline_from_plan(plan, queryString);
			MemoryContextSwitchTo(oldcontext);

			if (current_outline && current_outline->hints && list_length(current_outline->hints) > 0)
			{
				/* Display outline data if enabled */
				if (pg_outline_display_hints)
				{
					char *normalized;
					char *fingerprint;
					StringInfoData outline_name;
					StringInfoData hints_buf;
					ListCell *lc;
					bool first = true;

					display_outline_data();

					/* Generate outline creation command for user to execute */
					normalized = normalize_query(queryString);
					fingerprint = compute_query_fingerprint(normalized);

					if (fingerprint)
					{
						char	   *short_fp = get_short_fingerprint(fingerprint);

						/* Generate outline name using short fingerprint */
						initStringInfo(&outline_name);
						appendStringInfo(&outline_name, "outline_%s", short_fp);
						pfree(short_fp);

						/* Format hints as a single string */
						initStringInfo(&hints_buf);
						foreach(lc, current_outline->hints)
						{
							char *hint = (char *) lfirst(lc);
							if (hint)
							{
								if (!first)
									appendStringInfoChar(&hints_buf, '\n');
								appendStringInfoString(&hints_buf, hint);
								first = false;
							}
						}

						/* Check if outline already exists */
						if (outline_exists(outline_name.data))
						{
							/* Outline already exists - notify user and show SQL with hints */
							char *sql_with_hints;
							StringInfo formatted_hints = format_outline_data(current_outline->hints);

							elog(NOTICE, "An outline named '%s' already exists for this query.\n"
								 "The existing outline will be used when the query is executed.",
								 outline_name.data);

							/* Show SQL with hints injected at original positions */
							if (formatted_hints && formatted_hints->data)
							{
								sql_with_hints = reconstruct_sql_with_positioned_hints(queryString, formatted_hints->data);
								if (sql_with_hints)
								{
									elog(NOTICE, "SQL with outline hints inserted at original positions:\n%s", sql_with_hints);
									pfree(sql_with_hints);
								}
								pfree(formatted_hints->data);
								pfree(formatted_hints);
							}
						}
						else
						{
							/* Display the command to store this outline */
							elog(NOTICE, "To store this outline, execute:\n"
								 "SELECT pg_outline_create('%s', %s, %s);",
								 outline_name.data,
								 quote_literal_cstr(normalized),
								 quote_literal_cstr(hints_buf.data));
						}

					/* Free the StringInfoData buffers */
					pfree(outline_name.data);
					pfree(hints_buf.data);
					}

					if (normalized)
						pfree(normalized);
					if (fingerprint)
						pfree(fingerprint);
				}
			}
		}
	}
	else if (pg_outline_display_hints && current_outline)
	{
		/* Not in auto mode but display is enabled - just display without storing */
		display_outline_data();
	}

	/* Reset outline context for next query - this frees all memory at once */
	if (OutlineContext)
	{
		MemoryContextReset(OutlineContext);
		current_outline = NULL;  /* Pointer is now invalid after reset */
	}

	/* Cleanup query metadata hash table if it was created */
	if (current_query_metadata)
	{
		destroy_query_metadata_table(current_query_metadata);
		current_query_metadata = NULL;
	}
}
/*
 * Generate outline hints from a planned statement
 */
static void
generate_outline_from_plan(PlannedStmt *plan, const char *query_string)
{
	List	   *hints = NIL;

	if (!plan || !plan->planTree)
		return;

	/* Check if we have a Query tree with query names assigned */
	if (current_outline && current_outline->query && current_query_metadata)
	{
		/* Collect hints with query names from the Query tree if hints were assigned */
		QueryHintCollector collector;

		collector.hints = NIL;
		collector.query_index = 0;

		/* Collect all hints from the Query tree with [query_name] prefixes */
		collect_query_hints_recursive(current_outline->query, &collector);

		/* If we got hints from the Query tree, use them */
		if (list_length(collector.hints) > 0)
		{
			hints = collector.hints;
		}
		else
		{
			/* No hints in Query tree, extract from plan tree with proper query names */
			extract_hints_from_plan_with_query_names(plan, current_outline->query, &hints);
		}
	}
	else
	{
		/* Fallback: Extract hints from the plan tree (without query names) */
		extract_hints_from_plan_tree(plan->planTree, &hints, 0);
	}

	/* Store hints in current outline */
	if (current_outline)
	{
		current_outline->query_string = pstrdup(query_string);
		current_outline->hints = hints;
		current_outline->outline_data = format_outline_data(hints);
	}
}

/*
 * Recursively extract hints from plan tree
 */
static void
extract_hints_from_plan_tree(Plan *plan, List **hints, int level)
{
	char	   *hint_str;

	if (plan == NULL)
		return;

	/* Extract scan method hints */
	hint_str = get_scan_method_hint(plan);
	if (hint_str)
		*hints = lappend(*hints, hint_str);

	/* Extract join method hints */
	hint_str = get_join_method_hint(plan);
	if (hint_str)
		*hints = lappend(*hints, hint_str);

	/* Extract leading (join order) hints - only at top level (level 0) */
	if (level == 0)
	{
		hint_str = get_leading_hint(plan);
		if (hint_str)
			*hints = lappend(*hints, hint_str);
	}

	/* Recurse into child plans */
	if (plan->lefttree)
		extract_hints_from_plan_tree(plan->lefttree, hints, level + 1);
	if (plan->righttree)
		extract_hints_from_plan_tree(plan->righttree, hints, level + 1);

	/* Handle subplans */
	if (plan->initPlan)
	{
		ListCell   *lc;

		foreach(lc, plan->initPlan)
		{
			/* Note: we'd need access to PlannedStmt to get actual subplan */
			(void) lc; /* unused in current implementation */
		}
	}
}

/*
 * Generate scan method hint for a plan node
 */
static char *
get_scan_method_hint(Plan *plan)
{
	StringInfoData hint;
	char	   *relname;

	initStringInfo(&hint);

	switch (nodeTag(plan))
	{
		case T_SeqScan:
			relname = get_relation_name(((Scan *) plan)->scanrelid, current_plannedstmt);
			appendStringInfo(&hint, "SeqScan(%s)", relname ? relname : "unknown");
			if (relname)
				pfree(relname);
			return hint.data;

		case T_IndexScan:
			relname = get_relation_name(((Scan *) plan)->scanrelid, current_plannedstmt);
			appendStringInfo(&hint, "IndexScan(%s)", relname ? relname : "unknown");
			if (relname)
				pfree(relname);
			return hint.data;

		case T_IndexOnlyScan:
			relname = get_relation_name(((Scan *) plan)->scanrelid, current_plannedstmt);
			appendStringInfo(&hint, "IndexOnlyScan(%s)", relname ? relname : "unknown");
			if (relname)
				pfree(relname);
			return hint.data;

		case T_BitmapHeapScan:
			relname = get_relation_name(((Scan *) plan)->scanrelid, current_plannedstmt);
			appendStringInfo(&hint, "BitmapScan(%s)", relname ? relname : "unknown");
			if (relname)
				pfree(relname);
			return hint.data;

		default:
			/* Not a scan node or not interesting */
			break;
	}

	return NULL;
}

/*
 * Generate join method hint for a plan node
 */
static char *
get_join_method_hint(Plan *plan)
{
	StringInfoData hint;
	char	   *relation_names;

	initStringInfo(&hint);

	switch (nodeTag(plan))
	{
		case T_NestLoop:
			/* Extract relation names from the join in flat format */
			relation_names = extract_relation_names_flat(plan);
			if (relation_names)
			{
				appendStringInfo(&hint, "NestLoop(%s)", relation_names);
				pfree(relation_names);
			}
			else
			{
				appendStringInfo(&hint, "NestLoop(...)");
			}
			return hint.data;

		case T_HashJoin:
			/* Extract relation names from the join in flat format */
			relation_names = extract_relation_names_flat(plan);
			if (relation_names)
			{
				appendStringInfo(&hint, "HashJoin(%s)", relation_names);
				pfree(relation_names);
			}
			else
			{
				appendStringInfo(&hint, "HashJoin(...)");
			}
			return hint.data;

		case T_MergeJoin:
			/* Extract relation names from the join in flat format */
			relation_names = extract_relation_names_flat(plan);
			if (relation_names)
			{
				appendStringInfo(&hint, "MergeJoin(%s)", relation_names);
				pfree(relation_names);
			}
			else
			{
				appendStringInfo(&hint, "MergeJoin(...)");
			}
			return hint.data;

		default:
			/* Not a join node */
			break;
	}

	return NULL;
}

/*
 * Extract relation names from a plan tree recursively
 * Returns a string representation suitable for Leading hint
 * For join nodes, returns nested format: (left right)
 * For scan nodes, returns the relation name
 */
static char *
extract_relation_names_from_plan(Plan *plan)
{
	StringInfoData result;
	char	   *left_str;
	char	   *right_str;
	char	   *relname;

	if (plan == NULL)
		return NULL;

	initStringInfo(&result);

	/* Check if this is a join node */
	switch (nodeTag(plan))
	{
		case T_NestLoop:
		case T_HashJoin:
		case T_MergeJoin:
			/* Recursively get relation names from left and right subtrees */
			left_str = extract_relation_names_from_plan(plan->lefttree);
			right_str = extract_relation_names_from_plan(plan->righttree);

			if (left_str && right_str)
			{
				/* Build nested format: (left right) */
				appendStringInfo(&result, "(%s %s)", left_str, right_str);
				pfree(left_str);
				pfree(right_str);
				return result.data;
			}
			else
			{
				/* Cleanup and return NULL if either side is missing */
				if (left_str)
					pfree(left_str);
				if (right_str)
					pfree(right_str);
				return NULL;
			}

		case T_SeqScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapHeapScan:
			/* Extract relation name from scan node */
			relname = get_relation_name(((Scan *) plan)->scanrelid, current_plannedstmt);
			if (relname)
			{
				/* Use just the base relation name (without AS alias part) */
				char	   *space_pos = strchr(relname, ' ');

				if (space_pos)
				{
					/* Extract just the base name before " AS " */
					size_t		len = space_pos - relname;
					char	   *base_name = palloc(len + 1);

					memcpy(base_name, relname, len);
					base_name[len] = '\0';
					pfree(relname);
					return base_name;
				}
				return relname;
			}
			return NULL;

		default:
			/* For other node types, try to recurse into child plans */
			if (plan->lefttree)
			{
				left_str = extract_relation_names_from_plan(plan->lefttree);
				if (left_str)
					return left_str;
			}
			if (plan->righttree)
			{
				right_str = extract_relation_names_from_plan(plan->righttree);
				if (right_str)
					return right_str;
			}
			return NULL;
	}
}

/*
 * Extract relation names from a plan tree in flat format (space-separated list)
 * Used for Join hints which should not have nested parentheses
 * For example: "t1 t2 t3" instead of "(t1 (t2 t3))"
 */
static char *
extract_relation_names_flat(Plan *plan)
{
	StringInfoData result;
	char	   *left_str;
	char	   *right_str;
	char	   *relname;

	if (plan == NULL)
		return NULL;

	initStringInfo(&result);

	/* Check if this is a join node */
	switch (nodeTag(plan))
	{
		case T_NestLoop:
		case T_HashJoin:
		case T_MergeJoin:
			/* Recursively get relation names from left and right subtrees */
			left_str = extract_relation_names_flat(plan->lefttree);
			right_str = extract_relation_names_flat(plan->righttree);

			if (left_str && right_str)
			{
				/* Build flat format: left right (space-separated) */
				appendStringInfo(&result, "%s %s", left_str, right_str);
				pfree(left_str);
				pfree(right_str);
				return result.data;
			}
			else if (left_str)
			{
				pfree(left_str);
				return NULL;
			}
			else if (right_str)
			{
				pfree(right_str);
				return NULL;
			}
			return NULL;

		case T_SeqScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapHeapScan:
			/* Extract relation name from scan node */
			relname = get_relation_name(((Scan *) plan)->scanrelid, current_plannedstmt);
			if (relname)
			{
				/* Use just the base relation name (without AS alias part) */
				char	   *space_pos = strchr(relname, ' ');

				if (space_pos)
				{
					/* Extract just the base name before " AS " */
					size_t		len = space_pos - relname;
					char	   *base_name = palloc(len + 1);

					memcpy(base_name, relname, len);
					base_name[len] = '\0';
					pfree(relname);
					return base_name;
				}
				return relname;
			}
			return NULL;

		default:
			/* For other node types, try to recurse into child plans */
			if (plan->lefttree)
			{
				left_str = extract_relation_names_flat(plan->lefttree);
				if (left_str)
					return left_str;
			}
			if (plan->righttree)
			{
				right_str = extract_relation_names_flat(plan->righttree);
				if (right_str)
					return right_str;
			}
			return NULL;
	}
}

/*
 * Generate Leading hint from a join plan tree
 * Returns nested parentheses format like: Leading((t1 t2) (t3 t4))
 */
static char *
get_leading_hint_from_join(Plan *plan)
{
	StringInfoData hint;
	char	   *relation_names;

	if (plan == NULL)
		return NULL;

	/* Only generate Leading hint for join nodes */
	switch (nodeTag(plan))
	{
		case T_NestLoop:
		case T_HashJoin:
		case T_MergeJoin:
			/* Extract nested relation names from the join tree */
			relation_names = extract_relation_names_from_plan(plan);
			if (relation_names)
			{
				initStringInfo(&hint);
				/* Always use nested format - wrap in outer parentheses */
				appendStringInfo(&hint, "Leading(%s)", relation_names);
				pfree(relation_names);
				return hint.data;
			}
			return NULL;

		default:
			return NULL;
	}
}

/*
 * Generate leading (join order) hint for a plan node
 */
static char *
get_leading_hint(Plan *plan)
{
	/* Generate Leading hint only for the top-level join */
	/* This will be called from extract_hints_from_plan_tree */
	return get_leading_hint_from_join(plan);
}

/* Structure to map relation OIDs to their originating SubLink index */
typedef struct RelationSubLinkMap
{
	HTAB *relid_to_sublink;  /* Hash table: Oid -> sublink_index */
	int   sublink_count;     /* Total number of SubLinks found */
} RelationSubLinkMap;

typedef struct RelationSubLinkEntry
{
	Oid relid;            /* Key: relation OID */
	int sublink_index;     /* Value: SubLink index (-1 for main query) */
} RelationSubLinkEntry;

/* Context for building the relation to SubLink mapping */
typedef struct RelSubLinkMapContext
{
	RelationSubLinkMap *map;
	int current_sublink_index;
	HASHCTL hash_ctl;
} RelSubLinkMapContext;

/* Walker to find SubLinks and build relation mapping */
static bool
build_relation_sublink_map_walker(Node *node, RelSubLinkMapContext *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, SubLink))
	{
		SubLink *sublink = (SubLink *) node;

		if (IsA(sublink->subselect, Query))
		{
			Query *subquery = (Query *) sublink->subselect;
			ListCell *lc;
			int sublink_idx = context->current_sublink_index++;
			bool found;

			/* Mark all relations in this subquery as belonging to this SubLink */
			foreach(lc, subquery->rtable)
			{
				RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

				if (rte->rtekind == RTE_RELATION)
				{
					RelationSubLinkEntry *entry;
					entry = (RelationSubLinkEntry *) hash_search(context->map->relid_to_sublink,
																  &rte->relid,
																  HASH_ENTER,
																  &found);
					if (!found)
					{
						entry->relid = rte->relid;
						entry->sublink_index = sublink_idx;
					}
				}
			}

			/* Recursively process nested subqueries */
			return query_tree_walker(subquery, build_relation_sublink_map_walker, context, 0);
		}
		return false;
	}

	/* For Query nodes, don't recurse - they're handled by explicit check above */
	if (IsA(node, Query))
		return false;

	return expression_tree_walker(node, build_relation_sublink_map_walker, context);
}

/* Build a mapping of relations to SubLinks */
static RelationSubLinkMap *
build_relation_sublink_map(Query *query)
{
	RelationSubLinkMap *map;
	RelSubLinkMapContext context;
	HASHCTL hash_ctl;

	map = (RelationSubLinkMap *) palloc0(sizeof(RelationSubLinkMap));

	/* Initialize hash table */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(Oid);
	hash_ctl.entrysize = sizeof(RelationSubLinkEntry);
	hash_ctl.hcxt = CurrentMemoryContext;

	map->relid_to_sublink = hash_create("Relation to SubLink Map",
										 32,  /* initial size */
										 &hash_ctl,
										 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	map->sublink_count = 0;

	/* Walk the query tree to find SubLinks */
	context.map = map;
	context.current_sublink_index = 0;

	/* Walk WHERE clause */
	if (query->jointree && query->jointree->quals)
		build_relation_sublink_map_walker(query->jointree->quals, &context);

	/* Walk targetlist */
	build_relation_sublink_map_walker((Node *) query->targetList, &context);

	/* Walk HAVING clause */
	if (query->havingQual)
		build_relation_sublink_map_walker(query->havingQual, &context);

	map->sublink_count = context.current_sublink_index;

	return map;
}

/* Get the SubLink index for a relation OID, or -1 if it's in the main query */
static int
get_relation_sublink_index(RelationSubLinkMap *map, Oid relid)
{
	RelationSubLinkEntry *entry;
	bool found;

	if (!map || !map->relid_to_sublink)
		return -1;

	entry = (RelationSubLinkEntry *) hash_search(map->relid_to_sublink,
												  &relid,
												  HASH_FIND,
												  &found);

	if (found)
		return entry->sublink_index;

	return -1;  /* Not in any SubLink, belongs to main query */
}

/*
 * Extract all relation names from a hint string
 * e.g., "HashJoin(t1 t2 t3)" -> list of ["t1", "t2", "t3"]
 */
static List *
extract_relations_from_join_hint(const char *hint)
{
	char	   *paren_start;
	char	   *paren_end;
	char	   *content;
	char	   *token;
	char	   *saveptr = NULL;
	List	   *relations = NIL;
	int			len;

	if (!hint)
		return NIL;

	paren_start = strchr(hint, '(');
	if (!paren_start)
		return NIL;

	paren_end = strchr(paren_start, ')');
	if (!paren_end)
		return NIL;

	/* Skip past the '(' */
	paren_start++;
	len = paren_end - paren_start;

	if (len <= 0)
		return NIL;

	/* Copy content between parentheses */
	content = palloc(len + 1);
	strncpy(content, paren_start, len);
	content[len] = '\0';

	/* Parse space-separated relation names */
	token = strtok_r(content, " \t", &saveptr);
	while (token != NULL)
	{
		char *relname = pstrdup(token);
		relations = lappend(relations, relname);
		token = strtok_r(NULL, " \t", &saveptr);
	}

	pfree(content);
	return relations;
}

/* Extract relation name from a hint string (e.g., "SeqScan(t2)" -> "t2") */
static char *
extract_relation_from_hint(const char *hint)
{
	char *paren_start;
	char *paren_end;
	char *space;
	int len;
	char *relname;

	if (!hint)
		return NULL;

	paren_start = strchr(hint, '(');
	if (!paren_start)
		return NULL;

	paren_end = strchr(paren_start, ')');
	if (!paren_end)
		return NULL;

	/* Skip past the '(' */
	paren_start++;
	len = paren_end - paren_start;

	if (len <= 0)
		return NULL;

	/* For join hints like "HashJoin(t1 t2)", get the first relation */
	space = strchr(paren_start, ' ');
	if (space && space < paren_end)
		len = space - paren_start;

	relname = palloc(len + 1);
	strncpy(relname, paren_start, len);
	relname[len] = '\0';

	return relname;
}

/* Get relation OID from relation name using PlannedStmt's rtable */
static Oid
get_relation_oid_from_name(const char *relname, PlannedStmt *plannedstmt)
{
	ListCell *lc;
	RangeTblEntry *rte;

	if (!relname || !plannedstmt || !plannedstmt->rtable)
		return InvalidOid;

	foreach(lc, plannedstmt->rtable)
	{
		rte = (RangeTblEntry *) lfirst(lc);

		if (rte->rtekind == RTE_RELATION)
		{
			char *rte_relname = get_rel_name(rte->relid);
			if (rte_relname && strcmp(rte_relname, relname) == 0)
			{
				Oid relid = rte->relid;
				pfree(rte_relname);
				return relid;
			}
			if (rte_relname)
				pfree(rte_relname);

			/* Also check alias */
			if (rte->alias && rte->alias->aliasname &&
				strcmp(rte->alias->aliasname, relname) == 0)
				return rte->relid;
		}
	}

	return InvalidOid;
}

/* Determine which SubLink (if any) a hint belongs to based on the relations it references */
static int
get_hint_sublink_index(const char *hint, RelationSubLinkMap *relmap, PlannedStmt *plannedstmt)
{
	char *relname;
	Oid relid;
	int sublink_idx;

	if (!hint || !relmap)
		return -1;

	/* Extract the first relation name from the hint */
	relname = extract_relation_from_hint(hint);
	if (!relname)
		return -1;

	/* Get the OID for this relation */
	relid = get_relation_oid_from_name(relname, plannedstmt);
	pfree(relname);

	if (relid == InvalidOid)
		return -1;

	/* Check if this relation belongs to a SubLink */
	sublink_idx = get_relation_sublink_index(relmap, relid);

	return sublink_idx;
}

/*
 * Extract hints from plan tree with proper query names
 * This function maps plan nodes to their corresponding Query structures
 * and prefixes hints with the appropriate query name ([main], [sublink_N], etc.)
 *
 * For pulled-up subqueries (converted to joins), we need to track which relations
 * originally came from SubLinks and label their hints accordingly.
 */
static void
extract_hints_from_plan_with_query_names(PlannedStmt *plannedstmt, Query *query, List **hints)
{
	List	   *main_hints = NIL;
	ListCell   *lc;
	char	   *query_name;
	int			sublink_index = 0;
	RelationSubLinkMap *relmap;
	List	   **sublink_hints_array;  /* Array of hint lists, one per SubLink */
	int			i;

	if (!plannedstmt || !plannedstmt->planTree || !query)
		return;

	/* Build a mapping of relations to their originating SubLinks */
	relmap = build_relation_sublink_map(query);

	/* Extract hints from main plan tree */
	extract_hints_from_plan_tree(plannedstmt->planTree, &main_hints, 0);

	/* Get the main query name from metadata */
	query_name = get_query_name(query);
	if (!query_name)
		query_name = "main";

	/* Initialize an array to hold hints for each SubLink */
	if (relmap->sublink_count > 0)
	{
		sublink_hints_array = (List **) palloc0(sizeof(List *) * relmap->sublink_count);
		for (i = 0; i < relmap->sublink_count; i++)
			sublink_hints_array[i] = NIL;
	}
	else
	{
		sublink_hints_array = NULL;
	}

	/*
	 * Categorize hints based on which SubLink (if any) they belong to.
	 * Check each hint to see if its relations belong to a SubLink.
	 */
	foreach(lc, main_hints)
	{
		char *hint = (char *) lfirst(lc);
		if (hint)
		{
			int hint_sublink_idx = get_hint_sublink_index(hint, relmap, plannedstmt);

			if (hint_sublink_idx >= 0 && hint_sublink_idx < relmap->sublink_count)
			{
				/* This hint belongs to a SubLink */
				char *sublink_name = psprintf("sublink_%d", hint_sublink_idx);
				char *prefixed_hint = psprintf("[%s] %s", sublink_name, hint);
				sublink_hints_array[hint_sublink_idx] = lappend(sublink_hints_array[hint_sublink_idx], prefixed_hint);
			}
			else
			{
				/* This hint belongs to the main query */
				char *prefixed_hint = psprintf("[%s] %s", query_name, hint);
				*hints = lappend(*hints, prefixed_hint);
			}
		}
	}

	/* Add SubLink hints to the main hints list */
	for (i = 0; i < relmap->sublink_count; i++)
	{
		ListCell *slc;
		foreach(slc, sublink_hints_array[i])
		{
			*hints = lappend(*hints, lfirst(slc));
		}
	}

	/* Process subplans (for SubLinks that weren't pulled up) */
	if (plannedstmt->subplans)
	{
		ListCell *subplan_lc;

		foreach(subplan_lc, plannedstmt->subplans)
		{
			Plan *subplan = (Plan *) lfirst(subplan_lc);
			List *subplan_hints = NIL;
			char *sublink_name;

			if (subplan == NULL)
				continue;

			/* Extract hints from this subplan */
			extract_hints_from_plan_tree(subplan, &subplan_hints, 0);

			/* Use sequential naming: sublink_0, sublink_1, etc. */
			sublink_name = psprintf("sublink_%d", sublink_index);
			sublink_index++;

			/* Prefix subplan hints with sublink name */
			foreach(lc, subplan_hints)
			{
				char *hint = (char *) lfirst(lc);
				if (hint)
				{
					char *prefixed_hint = psprintf("[%s] %s", sublink_name, hint);
					*hints = lappend(*hints, prefixed_hint);
				}
			}
		}
	}

	/* Clean up */
	if (sublink_hints_array)
		pfree(sublink_hints_array);
	if (relmap && relmap->relid_to_sublink)
		hash_destroy(relmap->relid_to_sublink);
}

/*
 * Get relation name from relid using PlannedStmt
 */
static char *
get_relation_name(Index relid, PlannedStmt *plan)
{
	RangeTblEntry *rte;
	char	   *relname = NULL;

	if (plan == NULL || relid == 0 || relid > list_length(plan->rtable))
		return NULL;

	rte = rt_fetch(relid, plan->rtable);

	if (rte->rtekind == RTE_RELATION)
	{
		Relation	rel = relation_open(rte->relid, NoLock);

		relname = pstrdup(RelationGetRelationName(rel));
		relation_close(rel, NoLock);

		/* Include alias if different from table name */
		if (rte->eref && rte->eref->aliasname &&
			strcmp(relname, rte->eref->aliasname) != 0)
		{
			char	   *result = psprintf("%s AS %s", relname, rte->eref->aliasname);

			pfree(relname);
			return result;
		}

		return relname;
	}
	else if (rte->rtekind == RTE_SUBQUERY && rte->eref)
	{
		/* For subqueries, use the alias */
		return pstrdup(rte->eref->aliasname);
	}

	return NULL;
}

/*
 * Format outline data for display
 */
static StringInfo
format_outline_data(List *hints)
{
	StringInfoData result;
	StringInfo ret;
	ListCell   *lc;
	bool		first = true;

	initStringInfo(&result);

	appendStringInfo(&result, "/*+\nBEGIN_OUTLINE_DATA\n");

	foreach(lc, hints)
	{
		char	   *hint = (char *) lfirst(lc);

		if (hint)
		{
			if (!first)
				appendStringInfo(&result, "\n");
			appendStringInfo(&result, "%s", hint);
			first = false;
		}
	}

	appendStringInfo(&result, "\nEND_OUTLINE_DATA\n*/");

	/* Allocate a StringInfo structure and copy the result */
	ret = makeStringInfo();
	appendStringInfoString(ret, result.data);
	pfree(result.data);  /* Free the local StringInfoData's buffer */
	return ret;
}

/*
 * Construct SQL with hints injected
 * This function injects the outline hints comment at the beginning of the SQL query
 */
static char *
construct_sql_with_hints(const char *query_string, const char *hints_string)
{
	StringInfoData result;
	char	   *ret;

	if (!query_string || !hints_string)
		return NULL;

	initStringInfo(&result);

	/* Add hints comment at the beginning */
	appendStringInfo(&result, "/*+\nBEGIN_OUTLINE_DATA\n%s\nEND_OUTLINE_DATA\n*/\n", hints_string);

	/* Add the original query */
	appendStringInfo(&result, "%s", query_string);

	ret = result.data;
	return ret;
}

/*
 * Reconstruct SQL with hints at their original positions
 * This is a simplified implementation that inserts the main query hint
 * at the beginning of the SQL, which is sufficient for most cases.
 *
 * For complex scenarios with positioned hints, this function parses
 * hints in "[query_name] hint" format and attempts to inject them at
 * appropriate positions in the SQL.
 */
static char *
reconstruct_sql_with_positioned_hints(const char *query_string, const char *hints_string)
{
	StringInfoData result;
	const char *p;
	char	   *main_hint = NULL;
	List	   *other_hints = NIL;

	if (!query_string || !hints_string)
		return NULL;

	initStringInfo(&result);

	/*
	 * Parse hints_string to separate [main] hint from others
	 * Format: "[main] hint1\n[sublink_0] hint2\n[cte_xxx] hint3"
	 */
	p = hints_string;
	while (*p)
	{
		/* Skip whitespace */
		while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
			p++;

		if (!*p)
			break;

		/* Look for [query_name] */
		if (*p == '[')
		{
			const char *name_start = p + 1;
			const char *name_end = strchr(name_start, ']');

			if (name_end)
			{
				size_t name_len = name_end - name_start;
				char *query_name = palloc(name_len + 1);
				memcpy(query_name, name_start, name_len);
				query_name[name_len] = '\0';

				/* Skip past ] and spaces */
				p = name_end + 1;
				while (*p && (*p == ' ' || *p == '\t'))
					p++;

				/* Extract hint text until newline */
				const char *hint_start = p;
				while (*p && *p != '\n' && *p != '\r')
					p++;

				size_t hint_len = p - hint_start;
				char *hint_text = palloc(hint_len + 1);
				memcpy(hint_text, hint_start, hint_len);
				hint_text[hint_len] = '\0';

				/* Store hint */
				if (strcmp(query_name, "main") == 0)
				{
					main_hint = hint_text;
				}
				else
				{
					/* Store non-main hints for potential future use */
					other_hints = lappend(other_hints, hint_text);
				}

				pfree(query_name);
			}
			else
			{
				/* Skip to next line if malformed */
				while (*p && *p != '\n')
					p++;
			}
		}
		else
		{
			/* Skip unrecognized content */
			while (*p && *p != '\n')
				p++;
		}
	}

	/* Insert [main] hint after SELECT keyword if found */
	if (main_hint)
	{
		const char *select_pos;
		const char *sql_ptr = query_string;
		bool found_select = false;

		/* Find SELECT keyword (case-insensitive) */
		while (*sql_ptr)
		{
			/* Skip whitespace and comments */
			while (*sql_ptr && (*sql_ptr == ' ' || *sql_ptr == '\t' || *sql_ptr == '\n' || *sql_ptr == '\r'))
				sql_ptr++;

			/* Check for SELECT keyword (case-insensitive) */
			if ((*sql_ptr == 'S' || *sql_ptr == 's') &&
				(strncasecmp(sql_ptr, "SELECT", 6) == 0 || strncasecmp(sql_ptr, "select", 6) == 0))
			{
				/* Make sure it's a complete word (not part of another identifier) */
				if (!isalnum((unsigned char)sql_ptr[6]) && sql_ptr[6] != '_')
				{
					select_pos = sql_ptr + 6;  /* Position after "SELECT" */
					found_select = true;
					break;
				}
			}

			/* Skip to next potential position */
			if (*sql_ptr)
				sql_ptr++;
		}

		if (found_select)
		{
			/* Copy everything up to and including SELECT */
			appendBinaryStringInfo(&result, query_string, select_pos - query_string);

			/* Insert hint comment after SELECT */
			appendStringInfo(&result, " /*+ %s */", main_hint);

			/* Copy the rest of the SQL */
			appendStringInfoString(&result, select_pos);
		}
		else
		{
			/* Fallback: if SELECT not found, put hint at beginning */
			appendStringInfo(&result, "/*+ %s */\n", main_hint);
			appendStringInfoString(&result, query_string);
		}
	}
	else
	{
		/* No hint, just return original SQL */
		appendStringInfoString(&result, query_string);
	}

	/* Add other hints as a comment block at the end for reference */
	if (list_length(other_hints) > 0)
	{
		ListCell *lc;
		appendStringInfoString(&result, "\n/*\nOther hints in outline:\n");
		foreach(lc, other_hints)
		{
			char *hint = (char *) lfirst(lc);
			appendStringInfo(&result, "  %s\n", hint);
		}
		appendStringInfoString(&result, "*/");
	}

	return result.data;
}

/*
 * Display outline data to client
 */
static void
display_outline_data(void)
{
	if (current_outline && current_outline->hints && list_length(current_outline->hints) > 0)
	{
		/* Only display if not already displayed */
		if (!current_outline->displayed)
		{
			StringInfoData outline;
			ListCell   *lc;

			initStringInfo(&outline);
			appendStringInfo(&outline, "\n/*+\nBEGIN_OUTLINE_DATA\n");

			foreach(lc, current_outline->hints)
			{
				char	   *hint = (char *) lfirst(lc);

				if (hint)
					appendStringInfo(&outline, "%s\n", hint);
			}

			appendStringInfo(&outline, "END_OUTLINE_DATA\n*/\n");

			elog(NOTICE, "Generated Outline Data:%s", outline.data);
			/* Note: outline.data will be freed when memory context is reset */
			current_outline->displayed = true;
		}
	}
}

/*
 * Normalize query by replacing literals with placeholders
 * This version uses improved string parsing to preserve all identifiers
 * while replacing only literal values
 */
static char *
normalize_query(const char *query_string)
{
	StringInfoData normalized;
	const char *p;
	bool		in_string = false;
	bool		in_identifier = false;
	bool		in_comment = false;
	char		quote_char = '\0';
	bool		last_was_space = true;  /* Start true to trim leading spaces */
	char	   *result;
	int			len;

	if (!query_string)
		return NULL;

	initStringInfo(&normalized);

	for (p = query_string; *p; p++)
	{
		/* Handle line comments */
		if (in_comment)
		{
			if (*p == '\n')
				in_comment = false;
			continue;
		}

		/* Handle string literals */
		if (in_string)
		{
			if (*p == quote_char)
			{
				/* Check for escaped quote */
				if (*(p + 1) == quote_char)
				{
					p++; /* Skip escaped quote */
					continue;
				}
				in_string = false;
				/* Add space before ? if needed */
				if (!last_was_space && normalized.len > 0)
					appendStringInfoChar(&normalized, ' ');
				appendStringInfoChar(&normalized, '?');
				last_was_space = false;
			}
			continue;
		}

		/* Handle quoted identifiers - preserve them */
		if (in_identifier)
		{
			appendStringInfoChar(&normalized, tolower(*p));
			last_was_space = false;
			if (*p == '"')
			{
				/* Check for escaped quote */
				if (*(p + 1) == '"')
				{
					p++;
					appendStringInfoChar(&normalized, '"');
					continue;
				}
				in_identifier = false;
			}
			continue;
		}

		/* Start of string literal */
		if (*p == '\'')
		{
			in_string = true;
			quote_char = '\'';
			continue;
		}

		/* Start of quoted identifier */
		if (*p == '"')
		{
			in_identifier = true;
			appendStringInfoChar(&normalized, '"');
			last_was_space = false;
			continue;
		}

		/* Start of line comment */
		if (*p == '-' && *(p + 1) == '-')
		{
			in_comment = true;
			continue;
		}

		/* Start of block comment */
		if (*p == '/' && *(p + 1) == '*')
		{
			/* Skip until end of block comment */
			p += 2;
			while (*p)
			{
				if (*p == '*' && *(p + 1) == '/')
				{
					p++;
					break;
				}
				p++;
			}
			continue;
		}

		/*
		 * Handle numeric literals - replace with placeholder
		 * A number is a standalone literal if it's NOT part of an identifier.
		 * An identifier can contain letters, digits, underscores, and dollar signs.
		 */
		if (isdigit(*p))
		{
			/* Check if this digit is part of an identifier */
			if (p > query_string && (isalnum(*(p - 1)) || *(p - 1) == '_' || *(p - 1) == '$'))
			{
				/* Part of an identifier - keep it */
				appendStringInfoChar(&normalized, *p);
				last_was_space = false;
				continue;
			}

			/* This is a standalone number - replace with placeholder */
			/* Also check ahead to ensure we're not breaking an identifier */
			const char *num_start = p;

			/* Skip the number (including decimal point and scientific notation) */
			while (*p && (isdigit(*p) || *p == '.' || *p == 'e' || *p == 'E' ||
						  (*p == '+' && p > num_start && (*(p - 1) == 'e' || *(p - 1) == 'E')) ||
						  (*p == '-' && p > num_start && (*(p - 1) == 'e' || *(p - 1) == 'E'))))
				p++;

			/* Check if the number is followed by identifier characters */
			if (*p && (isalpha(*p) || *p == '_' || *p == '$'))
			{
				/* This is actually part of an identifier like "123table" - keep it all */
				p = num_start;
				appendStringInfoChar(&normalized, *p);
				last_was_space = false;
				continue;
			}

			/* Move back one character since the loop will increment p */
			if (*p)
				p--;

			/* Add space before ? if needed */
			if (!last_was_space && normalized.len > 0)
				appendStringInfoChar(&normalized, ' ');
			appendStringInfoChar(&normalized, '?');
			last_was_space = false;
			continue;
		}

		/* Normalize whitespace */
		if (isspace(*p))
		{
			/* Skip consecutive whitespace - only add one space */
			if (!last_was_space && normalized.len > 0)
			{
				appendStringInfoChar(&normalized, ' ');
				last_was_space = true;
			}
			continue;
		}

		/* Keep everything else (keywords, identifiers, operators) as lowercase */
		appendStringInfoChar(&normalized, tolower(*p));
		last_was_space = false;
	}

	/* Trim trailing whitespace */
	result = normalized.data;
	len = normalized.len;
	while (len > 0 && isspace(result[len - 1]))
		len--;
	result[len] = '\0';

	return result;
}

/*
 * Compute MD5 fingerprint of normalized query
 * Returns the full 32-character MD5 hash
 */
static char *
compute_query_fingerprint(const char *normalized_query)
{
	char		hexsum[33];  /* MD5 hex string: 32 chars + null terminator */
	char		*result;

	if (!normalized_query)
		return NULL;

	/* Compute MD5 hash - pg_md5_hash outputs hexadecimal string directly */
	if (!pg_md5_hash(normalized_query, strlen(normalized_query), hexsum))
		return NULL;

	/* Return the full 32-character hash */
	result = pstrdup(hexsum);

	return result;
}

/*
 * Generate short outline name from fingerprint
 * Uses first 12 characters of the fingerprint
 */
static char *
get_short_fingerprint(const char *fingerprint)
{
	char	   *result;

	if (!fingerprint)
		return NULL;

	/* Use only first 12 characters for shorter outline names */
	result = pnstrdup(fingerprint, 12);

	return result;
}

/*
 * Store outline hints in database using SPI
 */
static void
store_outline_hints(const char *outline_name, const char *query_pattern,
					const char *fingerprint, const char *hints)
{
	int			ret;
	StringInfoData query;
	char	   *effective_name;
	char	   *short_fp;

	if (SPI_connect() != SPI_OK_CONNECT)
	{
		elog(ERROR, "pg_outline: SPI_connect failed");
		return;
	}

	/* If outline_name is NULL or empty, generate it from fingerprint */
	if (!outline_name || outline_name[0] == '\0')
	{
		short_fp = get_short_fingerprint(fingerprint);
		effective_name = psprintf("outline_%s", short_fp);
		pfree(short_fp);
	}
	else
	{
		effective_name = pstrdup(outline_name);
	}

	/* INSERT only - no ON CONFLICT to prevent accidental updates */
	initStringInfo(&query);
	appendStringInfo(&query,
					 "INSERT INTO pg_outline_data (outline_name, query_pattern, fingerprint, hint_string, enabled) "
					 "VALUES (%s, %s, %s, %s, true)",
					 quote_literal_cstr(effective_name),
					 quote_literal_cstr(query_pattern),
					 quote_literal_cstr(fingerprint),
					 quote_literal_cstr(hints));

	ret = SPI_execute(query.data, false, 0);

	if (ret != SPI_OK_INSERT && ret != SPI_OK_INSERT_RETURNING)
		elog(WARNING, "pg_outline: failed to store outline");

	pfree(effective_name);
	SPI_finish();
}

/*
 * Update existing outline hints in database using SPI
 */
static void
update_outline_hints(const char *outline_name, const char *query_pattern,
					const char *fingerprint, const char *hints)
{
	int			ret;
	StringInfoData query;
	char	   *effective_name;
	char	   *short_fp;

	if (SPI_connect() != SPI_OK_CONNECT)
	{
		elog(ERROR, "pg_outline: SPI_connect failed");
		return;
	}

	/* If outline_name is NULL or empty, generate it from fingerprint */
	if (!outline_name || outline_name[0] == '\0')
	{
		short_fp = get_short_fingerprint(fingerprint);
		effective_name = psprintf("outline_%s", short_fp);
		pfree(short_fp);
	}
	else
	{
		effective_name = pstrdup(outline_name);
	}

	/* UPDATE based on fingerprint */
	initStringInfo(&query);
	appendStringInfo(&query,
					 "UPDATE pg_outline_data SET "
					 "outline_name = %s, "
					 "query_pattern = %s, "
					 "hint_string = %s, "
					 "updated_at = CURRENT_TIMESTAMP "
					 "WHERE fingerprint = %s",
					 quote_literal_cstr(effective_name),
					 quote_literal_cstr(query_pattern),
					 quote_literal_cstr(hints),
					 quote_literal_cstr(fingerprint));

	ret = SPI_execute(query.data, false, 0);

	if (ret < 0)
	{
		elog(WARNING, "pg_outline: failed to update outline");
	}
	else if (SPI_processed == 0)
	{
		elog(WARNING, "pg_outline: outline with fingerprint '%s' not found", fingerprint);
	}

	pfree(effective_name);
	SPI_finish();
}

/*
 * Retrieve outline hints from database using SPI
 */
static char *
retrieve_outline_hints(const char *fingerprint)
{
	int			ret;
	char	   *hints = NULL;
	StringInfoData query;

	if (!fingerprint)
		return NULL;

	if (SPI_connect() != SPI_OK_CONNECT)
	{
		elog(WARNING, "pg_outline: SPI_connect failed");
		return NULL;
	}

	initStringInfo(&query);
	appendStringInfo(&query,
					 "SELECT hint_string FROM pg_outline_data "
					 "WHERE fingerprint = %s AND enabled = true "
					 "LIMIT 1",
					 quote_literal_cstr(fingerprint));

	ret = SPI_execute(query.data, true, 1);

	if (ret == SPI_OK_SELECT && SPI_processed > 0)
	{
		TupleDesc	tupdesc = SPI_tuptable->tupdesc;
		HeapTuple	tuple = SPI_tuptable->vals[0];
		bool		isnull;
		Datum		hint_datum;

		hint_datum = SPI_getbinval(tuple, tupdesc, 1, &isnull);
		if (!isnull)
			hints = pstrdup(TextDatumGetCString(hint_datum));
	}

	SPI_finish();

	return hints;
}

/*
 * Check if an outline with the given name exists in the database
 */
static bool
outline_exists(const char *outline_name)
{
	int			ret;
	bool		exists = false;
	StringInfoData query;

	if (!outline_name)
		return false;

	if (SPI_connect() != SPI_OK_CONNECT)
	{
		elog(WARNING, "pg_outline: SPI_connect failed");
		return false;
	}

	initStringInfo(&query);
	appendStringInfo(&query,
					 "SELECT 1 FROM pg_outline_data "
					 "WHERE outline_name = %s "
					 "LIMIT 1",
					 quote_literal_cstr(outline_name));

	ret = SPI_execute(query.data, true, 1);

	if (ret == SPI_OK_SELECT && SPI_processed > 0)
		exists = true;

	pfree(query.data);
	SPI_finish();

	return exists;
}

/*
 * Context structure for SubLink collector walker
 */
typedef struct SubLinkCollectorContext
{
	QueryHintCollector *collector;
} SubLinkCollectorContext;

/*
 * Walker function to collect hints from SubLink nodes in expressions
 */
static bool
collect_hints_sublink_walker(Node *node, SubLinkCollectorContext *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, SubLink))
	{
		SubLink    *sublink = (SubLink *) node;

		/* If the subselect is a Query, process it recursively */
		if (IsA(sublink->subselect, Query))
		{
			Query	   *subquery = (Query *) sublink->subselect;

			collect_query_hints_recursive(subquery, context->collector);
		}

		/* Continue walking the testexpr and other parts of the SubLink */
		return false;
	}

	/* For Query nodes, don't recurse here - they're handled separately */
	if (IsA(node, Query))
		return false;

	/* Continue walking the expression tree */
	return expression_tree_walker(node, collect_hints_sublink_walker, (void *) context);
}

/*
 * Process SubLinks in a Query's expression trees during hint collection
 */
static void
process_query_sublinks_collection(Query *query, QueryHintCollector *collector)
{
	SubLinkCollectorContext context;

	if (!query)
		return;

	context.collector = collector;

	/* Walk targetList (SELECT clause) */
	collect_hints_sublink_walker((Node *) query->targetList, &context);

	/* Walk jointree quals (WHERE clause) */
	if (query->jointree)
		collect_hints_sublink_walker((Node *) query->jointree->quals, &context);

	/* Walk havingQual (HAVING clause) */
	collect_hints_sublink_walker(query->havingQual, &context);

	/* Walk other expression fields that might contain SubLinks */
	collect_hints_sublink_walker(query->limitOffset, &context);
	collect_hints_sublink_walker(query->limitCount, &context);
}

/*
 * Recursively collect hints from Query tree
 * Collects hints with their query names and indices
 */
static void
collect_query_hints_recursive(Query *query, QueryHintCollector *collector)
{
	ListCell   *lc;
	char	   *query_name;
	char	   *query_hints;

	if (!query)
		return;

	/* Get Query metadata from hash table */
	query_name = get_query_name(query);
	query_hints = get_query_hints(query);

	/* Store this Query's hint with its name and index */
	if (query_hints && query_name)
	{
		/* Format: "[query_name] hint_content" */
		char	   *named_hint = psprintf("[%s] %s", query_name, query_hints);

		collector->hints = lappend(collector->hints, named_hint);
		elog(DEBUG1, "Collected hint for Query #%d '%s': %s",
			 collector->query_index, query_name, query_hints);
	}
	else if (query_hints)
	{
		/* Fallback if query_name is not set - use index only */
		char	   *indexed_hint = psprintf("[query_%d] %s", collector->query_index, query_hints);

		collector->hints = lappend(collector->hints, indexed_hint);
		elog(DEBUG1, "Collected hint for Query #%d (no name): %s",
			 collector->query_index, query_hints);
	}

	collector->query_index++;

	/* Process subqueries in RTEs */
	foreach(lc, query->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		if (rte->rtekind == RTE_SUBQUERY && rte->subquery)
		{
			collect_query_hints_recursive(rte->subquery, collector);
		}
	}

	/* Process CTEs */
	foreach(lc, query->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

		if (cte->ctequery)
		{
			Query	   *ctequery = castNode(Query, cte->ctequery);

			collect_query_hints_recursive(ctequery, collector);
		}
	}

	/* Process SubLinks in expressions (WHERE, SELECT, HAVING, etc.) */
	process_query_sublinks_collection(query, collector);
}

/*
 * Store outline with per-Query hints
 * This function stores hints separately for each Query in the Query tree
 */
static void
store_outline_with_query_hints(const char *outline_name, const char *query_pattern,
							   const char *fingerprint, Query *query)
{
	int			ret;
	StringInfoData sql;
	QueryHintCollector collector;
	ListCell   *lc;
	int			outline_id;
	char	   *effective_name;
	char	   *short_fp;

	/* Initialize collector */
	collector.hints = NIL;
	collector.query_index = 0;

	/* Collect all hints from the Query tree */
	collect_query_hints_recursive(query, &collector);

	if (list_length(collector.hints) == 0)
	{
		elog(WARNING, "pg_outline: no hints found in Query tree");
		return;
	}

	if (SPI_connect() != SPI_OK_CONNECT)
	{
		elog(ERROR, "pg_outline: SPI_connect failed");
		return;
	}

	/* If outline_name is NULL or empty, generate it from fingerprint */
	if (!outline_name || outline_name[0] == '\0')
	{
		short_fp = get_short_fingerprint(fingerprint);
		effective_name = psprintf("outline_%s", short_fp);
		pfree(short_fp);
	}
	else
	{
		effective_name = pstrdup(outline_name);
	}

	/* First, insert/update the outline record */
	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "INSERT INTO pg_outline_data (outline_name, query_pattern, fingerprint, hint_string, enabled) "
					 "VALUES (%s, %s, %s, '', true) "
					 "ON CONFLICT (fingerprint) DO UPDATE SET "
					 "outline_name = EXCLUDED.outline_name, "
					 "query_pattern = EXCLUDED.query_pattern, "
					 "updated_at = CURRENT_TIMESTAMP "
					 "RETURNING outline_id",
					 quote_literal_cstr(effective_name),
					 quote_literal_cstr(query_pattern),
					 quote_literal_cstr(fingerprint));

	ret = SPI_execute(sql.data, false, 0);

	if (ret != SPI_OK_INSERT_RETURNING && ret != SPI_OK_UPDATE_RETURNING)
	{
		elog(ERROR, "pg_outline: failed to store outline");
		pfree(effective_name);
		SPI_finish();
		return;
	}

	/* Get the outline_id */
	if (SPI_processed > 0)
	{
		TupleDesc	tupdesc = SPI_tuptable->tupdesc;
		HeapTuple	tuple = SPI_tuptable->vals[0];
		bool		isnull;
		Datum		id_datum;

		id_datum = SPI_getbinval(tuple, tupdesc, 1, &isnull);
		if (isnull)
		{
			elog(ERROR, "pg_outline: outline_id is NULL");
			pfree(effective_name);
			SPI_finish();
			return;
		}
		outline_id = DatumGetInt32(id_datum);
	}
	else
	{
		elog(ERROR, "pg_outline: no outline_id returned");
		pfree(effective_name);
		SPI_finish();
		return;
	}

	/* Delete existing per-Query hints for this outline */
	resetStringInfo(&sql);
	appendStringInfo(&sql,
					 "DELETE FROM pg_outline_query_hints WHERE outline_id = %d",
					 outline_id);
	SPI_execute(sql.data, false, 0);

	/* Insert per-Query hints */
	{
		int hint_index = 0;
		foreach(lc, collector.hints)
		{
			char	   *named_hint = (char *) lfirst(lc);
			char	   *query_name_buf;
			char	   *hint_text;
			char	   *bracket_start;
			char	   *bracket_end;

			/* Parse "[query_name] hint" format */
			bracket_start = strchr(named_hint, '[');
			bracket_end = strchr(named_hint, ']');

			if (!bracket_start || !bracket_end || bracket_end < bracket_start)
			{
				elog(WARNING, "pg_outline: malformed hint format (expected [query_name] hint): %s", named_hint);
				hint_index++;
				continue;
			}

			/* Extract query name */
			query_name_buf = palloc(bracket_end - bracket_start);
			memcpy(query_name_buf, bracket_start + 1, bracket_end - bracket_start - 1);
			query_name_buf[bracket_end - bracket_start - 1] = '\0';

			/* Extract hint text (skip the space after ]) */
			hint_text = bracket_end + 1;
			while (*hint_text == ' ' || *hint_text == '\t')
				hint_text++;

			/* Insert into database */
			resetStringInfo(&sql);
			appendStringInfo(&sql,
							 "INSERT INTO pg_outline_query_hints (outline_id, query_name, query_index, hint_string) "
							 "VALUES (%d, %s, %d, %s)",
							 outline_id,
							 quote_literal_cstr(query_name_buf),
							 hint_index,
							 quote_literal_cstr(hint_text));

			ret = SPI_execute(sql.data, false, 0);
			if (ret != SPI_OK_INSERT)
				elog(WARNING, "pg_outline: failed to store hint for Query '%s'", query_name_buf);

			pfree(query_name_buf);
			hint_index++;
		}
	}

	SPI_finish();

	elog(NOTICE, "pg_outline: stored %d hint(s) for outline '%s'",
		 list_length(collector.hints), effective_name);
	pfree(effective_name);
}

/*
 * Extract inline hints from query string with position information
 * Looks for slash-star-plus ... star-slash comments and records both the hint content and its location
 * Returns a list of QueryHintPosition structures
 */
static List *
extract_inline_hints_with_positions(const char *query_string)
{
	List	   *hint_positions = NIL;
	const char *p;
	const char *hint_start;
	bool		in_hint = false;
	int			hint_begin_loc = 0;

	if (!query_string)
		return NIL;

	for (p = query_string; *p; p++)
	{
		/* Check for start of hint comment: slash-star-plus */
		if (!in_hint && *p == '/' && *(p + 1) == '*' && *(p + 2) == '+')
		{
			in_hint = true;
			hint_begin_loc = p - query_string;
			hint_start = p + 3; /* Skip past the opening */
			p += 2;			/* Move past slash-star (the loop will move past plus) */
			continue;
		}

		/* Check for end of comment: star-slash */
		if (in_hint && *p == '*' && *(p + 1) == '/')
		{
			/* Extract hint content */
			size_t		hint_len = p - hint_start;
			char	   *hint_text = palloc(hint_len + 1);
			QueryHintPosition *pos = palloc(sizeof(QueryHintPosition));

			memcpy(hint_text, hint_start, hint_len);
			hint_text[hint_len] = '\0';

			pos->hint_location = hint_begin_loc;
			pos->hint_end = (p - query_string) + 2; /* Include closing */
			pos->hint_text = hint_text;
			pos->consumed = false;	/* Initially not matched to any Query */

			hint_positions = lappend(hint_positions, pos);

			in_hint = false;
			p++;				/* Skip past the slash in star-slash */
			continue;
		}
	}

	return hint_positions;
}

/*
 * Strip inline hints from query string
 * Returns a copy of the query with all hint comments removed
 */
static char *
strip_hints_from_query(const char *query_string)
{
	StringInfoData result;
	const char *p;
	bool		in_hint = false;

	if (!query_string)
		return NULL;

	initStringInfo(&result);

	for (p = query_string; *p; p++)
	{
		/* Check for start of hint comment: slash-star-plus */
		if (!in_hint && *p == '/' && *(p + 1) == '*' && *(p + 2) == '+')
		{
			in_hint = true;
			p += 2;			/* Skip past slash-star (the loop will move past plus) */
			continue;
		}

		/* Check for end of comment: star-slash */
		if (in_hint && *p == '*' && *(p + 1) == '/')
		{
			in_hint = false;
			p++;				/* Skip past the slash in star-slash */
			continue;
		}

		/* Copy character if not in hint */
		if (!in_hint)
			appendStringInfoChar(&result, *p);
	}

	return result.data;
}

/*
 * Find the hint that corresponds to a Query at a specific location
 * Looks for a hint comment that appears just before the SELECT keyword
 * at or near the Query's stmt_location
 *
 * Enhanced matching logic inspired by pg_hint_plan:
 * - A hint applies if it appears anywhere between the previous query's location and this query's location
 * - We search for the CLOSEST unconsumed hint before the query's stmt_location
 * - We allow hints up to 200 characters before a query (was 100, increased for robustness)
 * - Once a hint is matched, it's marked as consumed to prevent duplicate matching
 */
static char *
find_hint_for_query_location(List *hint_positions, int stmt_location)
{
	ListCell   *lc;
	QueryHintPosition *best_match = NULL;
	int			best_distance = INT_MAX;

	if (stmt_location < 0)
		return NULL;

	foreach(lc, hint_positions)
	{
		QueryHintPosition *pos = (QueryHintPosition *) lfirst(lc);

		/* Skip hints that have already been matched to another Query */
		if (pos->consumed)
			continue;

		/*
		 * A hint applies to a Query if it appears before the Query's location.
		 * We search for the closest unconsumed hint before the stmt_location.
		 * Allow up to 200 characters of whitespace/newlines between hint and query.
		 */
		if (pos->hint_end <= stmt_location)
		{
			int distance = stmt_location - pos->hint_end;

			/* Allow more generous distance (up to 200 chars for complex formatting) */
			if (distance < 200 && distance < best_distance)
			{
				best_match = pos;
				best_distance = distance;
			}
		}
	}

	if (best_match)
	{
		/* Mark this hint as consumed so it won't be matched again */
		best_match->consumed = true;

		elog(DEBUG1, "Matched hint '%s' (at %d-%d) to Query at location %d (distance=%d)",
			 best_match->hint_text, best_match->hint_location, best_match->hint_end,
			 stmt_location, best_distance);
		return best_match->hint_text;
	}

	return NULL;
}

/*
 * Context structure for SubLink walker
 */
typedef struct SubLinkWalkerContext
{
	List	   *hint_positions;
	const char *source_text;
} SubLinkWalkerContext;

/*
 * Walker function to process SubLink nodes in expressions
 * This is called by expression_tree_walker for each node in the expression tree
 */
static bool
assign_hints_sublink_walker(Node *node, SubLinkWalkerContext *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, SubLink))
	{
		SubLink    *sublink = (SubLink *) node;

		/* If the subselect is a Query, process it recursively */
		if (IsA(sublink->subselect, Query))
		{
			Query	   *subquery = (Query *) sublink->subselect;

			assign_hints_to_queries(subquery, context->hint_positions, context->source_text);
		}

		/* Continue walking the testexpr and other parts of the SubLink */
		return false;
	}

	/* For Query nodes, don't recurse here - they're handled separately */
	if (IsA(node, Query))
		return false;

	/* Continue walking the expression tree */
	return expression_tree_walker(node, assign_hints_sublink_walker, (void *) context);
}

/*
 * Process SubLinks in a Query's expression trees
 * This walks all expressions in the Query looking for SubLink nodes
 */
static void
process_query_sublinks(Query *query, List *hint_positions, const char *source_text)
{
	SubLinkWalkerContext context;

	if (!query)
		return;

	context.hint_positions = hint_positions;
	context.source_text = source_text;

	/* Walk targetList (SELECT clause) */
	assign_hints_sublink_walker((Node *) query->targetList, &context);

	/* Walk jointree quals (WHERE clause) */
	if (query->jointree)
		assign_hints_sublink_walker((Node *) query->jointree->quals, &context);

	/* Walk havingQual (HAVING clause) */
	assign_hints_sublink_walker(query->havingQual, &context);

	/* Walk other expression fields that might contain SubLinks */
	assign_hints_sublink_walker(query->limitOffset, &context);
	assign_hints_sublink_walker(query->limitCount, &context);
}

/*
 * ============================================================================
 * Query Jumbling and Hash Table Implementation
 * ============================================================================
 *
 * This section provides a non-invasive way to associate metadata (names and hints)
 * with Query structures without modifying the Query structure definition.
 *
 * It uses a jumble-based hashing approach (adapted from pg_stat_statements)
 * to compute unique identifiers for each Query, then maintains a hash table
 * mapping these identifiers to their associated metadata.
 */

/*
 * AppendJumble: Append data to the query jumble
 */
static void
AppendJumble(OutlineJumbleState *jstate, const unsigned char *item, Size size)
{
	unsigned char *jumble = jstate->jumble;
	Size		jumble_len = jstate->jumble_len;

	/*
	 * Whenever the jumble buffer is full, we hash the current contents and
	 * reset the buffer to contain just that hash value, thus relying on the
	 * hash to summarize everything so far.
	 */
	while (size > 0)
	{
		Size		part_size;

		if (jumble_len >= JUMBLE_SIZE)
		{
			uint64		start_hash;

			start_hash = DatumGetUInt64(hash_any_extended(jumble,
														  JUMBLE_SIZE, 0));
			memcpy(jumble, &start_hash, sizeof(start_hash));
			jumble_len = sizeof(start_hash);
		}
		part_size = Min(size, JUMBLE_SIZE - jumble_len);
		memcpy(jumble + jumble_len, item, part_size);
		jumble_len += part_size;
		item += part_size;
		size -= part_size;
	}
	jstate->jumble_len = jumble_len;
}

/* Macros for jumbling various data types */
#define APP_JUMB(item) \
	AppendJumble(jstate, (const unsigned char *) &(item), sizeof(item))
#define APP_JUMB_STRING(str) \
	AppendJumble(jstate, (const unsigned char *) (str), strlen(str) + 1)

/*
 * JumbleQuery: Selectively serialize the query tree
 */
static void
JumbleQuery(OutlineJumbleState *jstate, Query *query)
{
	Assert(IsA(query, Query));
	Assert(query->utilityStmt == NULL);

	APP_JUMB(query->commandType);
	/* Include stmt_location to distinguish between different Query nodes */
	APP_JUMB(query->stmt_location);

	JumbleExpr(jstate, (Node *) query->cteList);
	JumbleRangeTable(jstate, query->rtable);
	JumbleExpr(jstate, (Node *) query->jointree);
	JumbleExpr(jstate, (Node *) query->targetList);
	JumbleExpr(jstate, (Node *) query->onConflict);
	JumbleExpr(jstate, (Node *) query->returningList);
	JumbleExpr(jstate, (Node *) query->groupClause);
	JumbleExpr(jstate, (Node *) query->groupingSets);
	JumbleExpr(jstate, query->havingQual);
	JumbleExpr(jstate, (Node *) query->windowClause);
	JumbleExpr(jstate, (Node *) query->distinctClause);
	JumbleExpr(jstate, (Node *) query->sortClause);
	JumbleExpr(jstate, query->limitOffset);
	JumbleExpr(jstate, query->limitCount);
	JumbleExpr(jstate, query->setOperations);
}

/*
 * JumbleRangeTable: Jumble a range table
 */
static void
JumbleRangeTable(OutlineJumbleState *jstate, List *rtable)
{
	ListCell   *lc;

	foreach(lc, rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);

		APP_JUMB(rte->rtekind);
		switch (rte->rtekind)
		{
			case RTE_RELATION:
				APP_JUMB(rte->relid);
				JumbleExpr(jstate, (Node *) rte->tablesample);
				break;
			case RTE_SUBQUERY:
				JumbleQuery(jstate, rte->subquery);
				break;
			case RTE_JOIN:
				APP_JUMB(rte->jointype);
				break;
			case RTE_FUNCTION:
				JumbleExpr(jstate, (Node *) rte->functions);
				break;
			case RTE_TABLEFUNC:
				JumbleExpr(jstate, (Node *) rte->tablefunc);
				break;
			case RTE_VALUES:
				JumbleExpr(jstate, (Node *) rte->values_lists);
				break;
			case RTE_CTE:
				APP_JUMB_STRING(rte->ctename);
				APP_JUMB(rte->ctelevelsup);
				break;
			case RTE_NAMEDTUPLESTORE:
				APP_JUMB_STRING(rte->enrname);
				break;
			case RTE_RESULT:
				break;
			default:
				elog(ERROR, "unrecognized RTE kind: %d", (int) rte->rtekind);
				break;
		}
	}
}

/*
 * JumbleExpr: Jumble an expression tree
 * Simplified version that handles the most common expression types.
 */
static void
JumbleExpr(OutlineJumbleState *jstate, Node *node)
{
	ListCell   *temp;

	if (node == NULL)
		return;

	/* Guard against stack overflow */
	check_stack_depth();

	APP_JUMB(node->type);

	switch (nodeTag(node))
	{
		case T_Var:
			{
				Var		   *var = (Var *) node;
				APP_JUMB(var->varno);
				APP_JUMB(var->varattno);
				APP_JUMB(var->varlevelsup);
			}
			break;
		case T_Const:
			{
				Const	   *c = (Const *) node;
				APP_JUMB(c->consttype);
			}
			break;
		case T_Param:
			{
				Param	   *p = (Param *) node;
				APP_JUMB(p->paramkind);
				APP_JUMB(p->paramid);
				APP_JUMB(p->paramtype);
			}
			break;
		case T_Aggref:
			{
				Aggref	   *expr = (Aggref *) node;
				APP_JUMB(expr->aggfnoid);
				JumbleExpr(jstate, (Node *) expr->aggdirectargs);
				JumbleExpr(jstate, (Node *) expr->args);
				JumbleExpr(jstate, (Node *) expr->aggorder);
				JumbleExpr(jstate, (Node *) expr->aggdistinct);
				JumbleExpr(jstate, (Node *) expr->aggfilter);
			}
			break;
		case T_WindowFunc:
			{
				WindowFunc *expr = (WindowFunc *) node;
				APP_JUMB(expr->winfnoid);
				APP_JUMB(expr->winref);
				JumbleExpr(jstate, (Node *) expr->args);
				JumbleExpr(jstate, (Node *) expr->aggfilter);
			}
			break;
		case T_FuncExpr:
			{
				FuncExpr   *expr = (FuncExpr *) node;
				APP_JUMB(expr->funcid);
				JumbleExpr(jstate, (Node *) expr->args);
			}
			break;
		case T_OpExpr:
		case T_DistinctExpr:
		case T_NullIfExpr:
			{
				OpExpr	   *expr = (OpExpr *) node;
				APP_JUMB(expr->opno);
				JumbleExpr(jstate, (Node *) expr->args);
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) node;
				APP_JUMB(expr->opno);
				APP_JUMB(expr->useOr);
				JumbleExpr(jstate, (Node *) expr->args);
			}
			break;
		case T_BoolExpr:
			{
				BoolExpr   *expr = (BoolExpr *) node;
				APP_JUMB(expr->boolop);
				JumbleExpr(jstate, (Node *) expr->args);
			}
			break;
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) node;
				APP_JUMB(sublink->subLinkType);
				APP_JUMB(sublink->subLinkId);
				JumbleExpr(jstate, (Node *) sublink->testexpr);
				JumbleQuery(jstate, castNode(Query, sublink->subselect));
			}
			break;
		case T_List:
			foreach(temp, (List *) node)
			{
				JumbleExpr(jstate, (Node *) lfirst(temp));
			}
			break;
		case T_SortGroupClause:
			{
				SortGroupClause *sgc = (SortGroupClause *) node;
				APP_JUMB(sgc->tleSortGroupRef);
				APP_JUMB(sgc->eqop);
				APP_JUMB(sgc->sortop);
				APP_JUMB(sgc->nulls_first);
			}
			break;
		case T_GroupingSet:
			{
				GroupingSet *gsnode = (GroupingSet *) node;
				APP_JUMB(gsnode->kind);
				JumbleExpr(jstate, (Node *) gsnode->content);
			}
			break;
		case T_WindowClause:
			{
				WindowClause *wc = (WindowClause *) node;
				APP_JUMB(wc->winref);
				APP_JUMB(wc->frameOptions);
				JumbleExpr(jstate, (Node *) wc->partitionClause);
				JumbleExpr(jstate, (Node *) wc->orderClause);
				JumbleExpr(jstate, wc->startOffset);
				JumbleExpr(jstate, wc->endOffset);
			}
			break;
		case T_CommonTableExpr:
			{
				CommonTableExpr *cte = (CommonTableExpr *) node;
				APP_JUMB_STRING(cte->ctename);
				JumbleQuery(jstate, castNode(Query, cte->ctequery));
			}
			break;
		case T_SetOperationStmt:
			{
				SetOperationStmt *setop = (SetOperationStmt *) node;
				APP_JUMB(setop->op);
				APP_JUMB(setop->all);
				JumbleExpr(jstate, setop->larg);
				JumbleExpr(jstate, setop->rarg);
			}
			break;
		case T_RangeTblRef:
			{
				RangeTblRef *rtr = (RangeTblRef *) node;
				APP_JUMB(rtr->rtindex);
			}
			break;
		case T_JoinExpr:
			{
				JoinExpr   *join = (JoinExpr *) node;
				APP_JUMB(join->jointype);
				APP_JUMB(join->isNatural);
				APP_JUMB(join->rtindex);
				JumbleExpr(jstate, join->larg);
				JumbleExpr(jstate, join->rarg);
				JumbleExpr(jstate, (Node *) join->usingClause);
				JumbleExpr(jstate, join->quals);
			}
			break;
		case T_FromExpr:
			{
				FromExpr   *from = (FromExpr *) node;
				JumbleExpr(jstate, (Node *) from->fromlist);
				JumbleExpr(jstate, from->quals);
			}
			break;
		case T_OnConflictExpr:
			{
				OnConflictExpr *conf = (OnConflictExpr *) node;
				APP_JUMB(conf->action);
				JumbleExpr(jstate, (Node *) conf->arbiterElems);
				JumbleExpr(jstate, conf->arbiterWhere);
				JumbleExpr(jstate, (Node *) conf->onConflictSet);
				JumbleExpr(jstate, conf->onConflictWhere);
				APP_JUMB(conf->constraint);
				JumbleExpr(jstate, (Node *) conf->exclRelTlist);
			}
			break;
		case T_TargetEntry:
			{
				TargetEntry *tle = (TargetEntry *) node;
				JumbleExpr(jstate, (Node *) tle->expr);
				APP_JUMB(tle->ressortgroupref);
			}
			break;
		default:
			/* For unknown node types, just jumble the node type itself */
			break;
	}
}

/*
 * compute_query_hash: Compute a unique hash for a Query structure
 */
static uint64
compute_query_hash(Query *query)
{
	OutlineJumbleState jstate;
	uint64		hash;

	if (query == NULL)
		return 0;

	/* Set up workspace for query jumbling */
	jstate.jumble = (unsigned char *) palloc(JUMBLE_SIZE);
	jstate.jumble_len = 0;

	/* Compute the jumble */
	JumbleQuery(&jstate, query);

	/* Compute final hash */
	hash = DatumGetUInt64(hash_any_extended(jstate.jumble, jstate.jumble_len, 0));

	/* Avoid returning zero (reserved for special cases) */
	if (hash == UINT64CONST(0))
		hash = UINT64CONST(1);

	pfree(jstate.jumble);

	return hash;
}

/*
 * create_query_metadata_table: Create a new hash table for Query metadata
 */
static QueryMetadataHashTable *
create_query_metadata_table(MemoryContext context)
{
	QueryMetadataHashTable *table;
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(context);

	table = (QueryMetadataHashTable *) palloc0(sizeof(QueryMetadataHashTable));
	table->memory_context = context;
	table->entry_count = 0;

	MemoryContextSwitchTo(oldcontext);

	return table;
}

/*
 * destroy_query_metadata_table: Destroy a query metadata hash table
 */
static void
destroy_query_metadata_table(QueryMetadataHashTable *table)
{
	if (table == NULL)
		return;

	/* Entries are allocated in table->memory_context, so no need to free individually */
	pfree(table);
}

/*
 * store_query_metadata: Store metadata for a Query in the hash table
 */
static void
store_query_metadata(QueryMetadataHashTable *table, Query *query,
					 const char *query_name, const char *query_hints, int query_index)
{
	uint64		hash;
	int			bucket;
	QueryMetadataEntry *entry;
	MemoryContext oldcontext;

	if (table == NULL || query == NULL)
		return;

	hash = compute_query_hash(query);
	bucket = hash % QUERY_METADATA_HASH_SIZE;

	oldcontext = MemoryContextSwitchTo(table->memory_context);

	/* Create new entry */
	entry = (QueryMetadataEntry *) palloc(sizeof(QueryMetadataEntry));
	entry->query_hash = hash;
	entry->stmt_location = query->stmt_location;
	entry->query_name = query_name ? pstrdup(query_name) : NULL;
	entry->query_hints = query_hints ? pstrdup(query_hints) : NULL;
	entry->query_index = query_index;

	/* Insert at head of bucket list */
	entry->next = table->buckets[bucket];
	table->buckets[bucket] = entry;
	table->entry_count++;

	MemoryContextSwitchTo(oldcontext);

	elog(DEBUG1, "Stored query metadata: hash=%lu, location=%d, name=%s, index=%d",
		 hash, query->stmt_location, query_name ? query_name : "(null)", query_index);
}

/*
 * lookup_query_metadata: Look up metadata for a Query in the hash table
 */
static QueryMetadataEntry *
lookup_query_metadata(QueryMetadataHashTable *table, Query *query)
{
	uint64		hash;
	int			bucket;
	QueryMetadataEntry *entry;

	if (table == NULL || query == NULL)
		return NULL;

	hash = compute_query_hash(query);
	bucket = hash % QUERY_METADATA_HASH_SIZE;

	/* Search the bucket for a matching entry */
	for (entry = table->buckets[bucket]; entry != NULL; entry = entry->next)
	{
		if (entry->query_hash == hash && entry->stmt_location == query->stmt_location)
			return entry;
	}

	return NULL;
}

/*
 * get_query_name: Helper to get query name from current hash table
 */
static char *
get_query_name(Query *query)
{
	QueryMetadataEntry *entry;

	if (current_query_metadata == NULL)
		return NULL;

	entry = lookup_query_metadata(current_query_metadata, query);
	return entry ? entry->query_name : NULL;
}

/*
 * get_query_hints: Helper to get query hints from current hash table
 */
static char *
get_query_hints(Query *query)
{
	QueryMetadataEntry *entry;

	if (current_query_metadata == NULL)
		return NULL;

	entry = lookup_query_metadata(current_query_metadata, query);
	return entry ? entry->query_hints : NULL;
}

/*
 * ============================================================================
 * End of Query Jumbling and Hash Table Implementation
 * ============================================================================
 */

/*
 * Generate a unique name for a Query structure
 * Types: "main", "cte", "subquery", "sublink"
 */
static char *
generate_query_name(QueryNamingContext *context, const char *type, const char *cte_name)
{
	StringInfoData name;

	initStringInfo(&name);

	if (strcmp(type, "main") == 0)
	{
		if (context->main_query_count == 0)
			appendStringInfoString(&name, "main");
		else
			appendStringInfo(&name, "main_%d", context->main_query_count);
		context->main_query_count++;
	}
	else if (strcmp(type, "cte") == 0)
	{
		if (cte_name && cte_name[0] != '\0')
			appendStringInfo(&name, "cte_%s", cte_name);
		else
			appendStringInfo(&name, "cte_%d", context->cte_count);
		context->cte_count++;
	}
	else if (strcmp(type, "subquery") == 0)
	{
		appendStringInfo(&name, "subquery_%d", context->subquery_count);
		context->subquery_count++;
	}
	else if (strcmp(type, "sublink") == 0)
	{
		appendStringInfo(&name, "sublink_%d", context->sublink_count);
		context->sublink_count++;
	}

	return name.data;
}

/*
 * Walker context for assigning query names to SubLinks
 */
typedef struct QueryNamingSubLinkContext
{
	QueryNamingContext *naming_context;
	const char *parent_name;
} QueryNamingSubLinkContext;

/*
 * Walker function to assign names to SubLink subqueries
 */
static bool
assign_names_sublink_walker(Node *node, QueryNamingSubLinkContext *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, SubLink))
	{
		SubLink *sublink = (SubLink *) node;

		if (sublink->subselect && IsA(sublink->subselect, Query))
		{
			Query *subquery = (Query *) sublink->subselect;
			assign_query_names(subquery, context->naming_context, context->parent_name, NULL);
		}

		/* Continue walking other parts of the SubLink */
		return false;
	}

	/* For Query nodes, don't recurse here - they're handled separately */
	if (IsA(node, Query))
		return false;

	/* Continue walking the expression tree */
	return expression_tree_walker(node, assign_names_sublink_walker, (void *) context);
}

/*
 * Process SubLinks in a Query's expression trees for naming
 */
static void
process_query_sublinks_naming(Query *query, QueryNamingContext *context, const char *parent_name)
{
	QueryNamingSubLinkContext walker_context;

	if (!query)
		return;

	walker_context.naming_context = context;
	walker_context.parent_name = parent_name;

	/* Walk targetList (SELECT clause) */
	assign_names_sublink_walker((Node *) query->targetList, &walker_context);

	/* Walk jointree quals (WHERE clause) */
	if (query->jointree)
		assign_names_sublink_walker((Node *) query->jointree->quals, &walker_context);

	/* Walk havingQual (HAVING clause) */
	assign_names_sublink_walker(query->havingQual, &walker_context);

	/* Walk other expression fields that might contain SubLinks */
	assign_names_sublink_walker(query->limitOffset, &walker_context);
	assign_names_sublink_walker(query->limitCount, &walker_context);
}

/*
 * Recursively assign names to all Query structures in a Query tree
 * This supports CTEs, subqueries in FROM clause, and SubLinks in expressions
 *
 * Parameters:
 *   query - The Query structure to name
 *   context - Naming context with counters
 *   parent_name - Name of the parent query (NULL for top-level)
 *   cte_name - Name of the CTE if this is a CTE query (NULL otherwise)
 */
static void
assign_query_names(Query *query, QueryNamingContext *context, const char *parent_name, const char *cte_name)
{
	ListCell *lc;
	char *query_name;

	if (!query)
		return;

	/* Determine the type and generate a name for this Query */
	if (parent_name == NULL)
	{
		/* Top-level query */
		query_name = generate_query_name(context, "main", NULL);
		context->query_index = 0;  /* Reset index for new query tree */
	}
	else if (cte_name != NULL)
	{
		/* This is a CTE query */
		query_name = generate_query_name(context, "cte", cte_name);
	}
	else
	{
		/* This is a regular subquery in FROM clause */
		query_name = generate_query_name(context, "subquery", NULL);
	}

	/* Store the name in the hash table instead of modifying Query structure */
	if (current_query_metadata != NULL)
	{
		store_query_metadata(current_query_metadata, query, query_name, NULL, context->query_index++);
		elog(DEBUG1, "Assigned name '%s' to Query at location %d (parent: %s)",
			 query_name, query->stmt_location, parent_name ? parent_name : "none");
	}

	/* Process CTEs first - they're defined before use */
	foreach(lc, query->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

		if (cte->ctequery)
		{
			Query *ctequery = castNode(Query, cte->ctequery);
			/* Pass the CTE name so we can include it in the query name */
			assign_query_names(ctequery, context, query_name, cte->ctename);
		}
	}

	/* Process subqueries in FROM clause (RTEs) */
	foreach(lc, query->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		if (rte->rtekind == RTE_SUBQUERY && rte->subquery)
		{
			assign_query_names(rte->subquery, context, query_name, NULL);
		}
	}

	/* Process SubLinks in expressions (WHERE, SELECT, HAVING, etc.) */
	/* SubLinks get "sublink" type names */
	process_query_sublinks_naming(query, context, query_name);

	pfree(query_name);
}

/*
 * Recursively assign hints to Query structures based on their location
 * This walks the Query tree and associates each Query with its corresponding hint
 */
static void
assign_hints_to_queries(Query *query, List *hint_positions, const char *source_text)
{
	ListCell   *lc;
	char	   *hint;

	if (!query)
		return;

	/* Debug: Log the Query location and all hint positions */
	elog(NOTICE, "DEBUG: Processing Query at stmt_location=%d, commandType=%d",
		 query->stmt_location, query->commandType);

	foreach(lc, hint_positions)
	{
		QueryHintPosition *pos = (QueryHintPosition *) lfirst(lc);
		elog(NOTICE, "DEBUG:   Hint at location %d-%d: '%s'",
			 pos->hint_location, pos->hint_end, pos->hint_text);
	}

	/* Find and assign hint for this Query */
	hint = find_hint_for_query_location(hint_positions, query->stmt_location);
	if (hint && current_query_metadata != NULL)
	{
		/* Look up existing metadata entry and update the hints field */
		QueryMetadataEntry *entry = lookup_query_metadata(current_query_metadata, query);
		if (entry)
		{
			/* Update hints in existing entry */
			if (entry->query_hints)
				pfree(entry->query_hints);
			entry->query_hints = pstrdup(hint);
			elog(NOTICE, "DEBUG: Assigned hint '%s' to Query at location %d",
				 hint, query->stmt_location);
		}
		else
		{
			/* Entry doesn't exist yet, create one with just hints */
			store_query_metadata(current_query_metadata, query, NULL, hint, -1);
			elog(NOTICE, "DEBUG: Created metadata with hint '%s' for Query at location %d",
				 hint, query->stmt_location);
		}
	}
	else if (query->stmt_location >= 0)
	{
		elog(NOTICE, "DEBUG: No hint found for Query at location %d", query->stmt_location);
	}

	/* Process subqueries in RTEs */
	foreach(lc, query->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		if (rte->rtekind == RTE_SUBQUERY && rte->subquery)
		{
			assign_hints_to_queries(rte->subquery, hint_positions, source_text);
		}
	}

	/* Process CTEs */
	foreach(lc, query->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

		if (cte->ctequery)
		{
			Query *ctequery = castNode(Query, cte->ctequery);
			assign_hints_to_queries(ctequery, hint_positions, source_text);
		}
	}

	/* Process SubLinks in expressions (WHERE, SELECT, HAVING, etc.) */
	process_query_sublinks(query, hint_positions, source_text);
}

/*
 * Log Query names and their associated hints
 * This walks the Query tree and logs each Query's name and hint for debugging
 */
static void
log_query_names_and_hints(Query *query)
{
	ListCell   *lc;
	char	   *query_name;
	char	   *query_hints;

	if (!query || !current_query_metadata)
		return;

	/* Get Query metadata from hash table */
	query_name = get_query_name(query);
	query_hints = get_query_hints(query);

	/* Log this Query's name and hints */
	if (query_name)
	{
		if (query_hints)
		{
			elog(NOTICE, "Query '%s' has hints: %s", query_name, query_hints);
		}
		else
		{
			elog(NOTICE, "Query '%s' has no hints", query_name);
		}
	}

	/* Process subqueries in RTEs */
	foreach(lc, query->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		if (rte->rtekind == RTE_SUBQUERY && rte->subquery)
		{
			log_query_names_and_hints(rte->subquery);
		}
	}

	/* Process CTEs */
	foreach(lc, query->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

		if (cte->ctequery)
		{
			Query *ctequery = castNode(Query, cte->ctequery);
			log_query_names_and_hints(ctequery);
		}
	}

	/* Process SubLinks in expressions */
	/* We need a walker to traverse SubLinks */
	typedef struct SubLinkLogContext
	{
		/* Currently no extra context needed */
		int dummy;
	} SubLinkLogContext;

	bool log_sublink_walker(Node *node, SubLinkLogContext *context)
	{
		if (node == NULL)
			return false;

		if (IsA(node, SubLink))
		{
			SubLink *sublink = (SubLink *) node;

			if (IsA(sublink->subselect, Query))
			{
				Query *subquery = (Query *) sublink->subselect;
				log_query_names_and_hints(subquery);
			}

			return false;
		}

		if (IsA(node, Query))
			return false;

		return expression_tree_walker(node, log_sublink_walker, (void *) context);
	}

	SubLinkLogContext walker_context;
	walker_context.dummy = 0;

	/* Walk targetList (SELECT clause) */
	log_sublink_walker((Node *) query->targetList, &walker_context);

	/* Walk jointree quals (WHERE clause) */
	if (query->jointree)
		log_sublink_walker((Node *) query->jointree->quals, &walker_context);

	/* Walk havingQual (HAVING clause) */
	log_sublink_walker(query->havingQual, &walker_context);

	/* Walk other expression fields that might contain SubLinks */
	log_sublink_walker(query->limitOffset, &walker_context);
	log_sublink_walker(query->limitCount, &walker_context);
}

/*
 * SQL function: create an outline for a query
 */
Datum
pg_outline_create(PG_FUNCTION_ARGS)
{
	text	   *outline_name = PG_ARGISNULL(0) ? NULL : PG_GETARG_TEXT_PP(0);
	text	   *query_text = PG_GETARG_TEXT_PP(1);
	char	   *name_str;
	char	   *query_str;
	char	   *hints_str;
	char	   *normalized;
	char	   *fingerprint;

	/* Check for NULL query_text (required) */
	if (PG_ARGISNULL(1))
		PG_RETURN_NULL();

	/* Get outline_name string if provided, else pass NULL for auto-generation */
	name_str = outline_name ? text_to_cstring(outline_name) : NULL;
	query_str = text_to_cstring(query_text);

	/* Handle optional hints parameter */
	if (PG_ARGISNULL(2))
		hints_str = pstrdup("");  /* Use empty string for NULL hints */
	else
		hints_str = text_to_cstring(PG_GETARG_TEXT_PP(2));

	/* Normalize query and compute fingerprint */
	normalized = normalize_query(query_str);
	fingerprint = compute_query_fingerprint(normalized);

	/* Store the outline with normalized query as pattern */
	/* If name_str is NULL, store_outline_hints will auto-generate from fingerprint */
	store_outline_hints(name_str, normalized, fingerprint, hints_str);

	/* Get the effective name for the notice message */
	if (!name_str || name_str[0] == '\0')
	{
		char *short_fp = get_short_fingerprint(fingerprint);
		name_str = psprintf("outline_%s", short_fp);
		pfree(short_fp);
	}

	elog(NOTICE, "pg_outline_create: outline '%s' created with fingerprint %s",
		 name_str, fingerprint ? fingerprint : "none");

	if (normalized)
		pfree(normalized);
	if (fingerprint)
		pfree(fingerprint);
	pfree(hints_str);

	PG_RETURN_BOOL(true);
}

/*
 * SQL function: create an outline from SQL with inline hints
 * This is the simplified interface that accepts SQL with hint comments
 * The function will:
 * 1. Extract inline hints with their positions from the SQL
 * 2. Parse the query (with hints still in source for location tracking)
 * 3. Assign each hint to its corresponding Query structure
 * 4. Plan the query with hints attached to Query structures
 * 5. Extract hints from the resulting execution plan (per Query)
 * 6. Store the extracted hints separately for each Query
 */
Datum
pg_outline_create_from_sql(PG_FUNCTION_ARGS)
{
	text	   *outline_name = PG_ARGISNULL(0) ? NULL : PG_GETARG_TEXT_PP(0);
	text	   *query_with_hints = PG_GETARG_TEXT_PP(1);
	char	   *name_str;
	char	   *query_str = text_to_cstring(query_with_hints);
	char	   *query_without_hints;
	char	   *normalized;
	char	   *fingerprint;
	List	   *hint_positions;
	PlannedStmt *plan;
	Query	   *query;
	RawStmt	   *raw_stmt;
	List	   *raw_parsetree_list;
	List	   *hints = NIL;
	StringInfoData hint_str;
	ListCell   *lc;
	bool		first = true;

	/* Check for NULL query_text (required) */
	if (PG_ARGISNULL(1))
		PG_RETURN_NULL();

	/* Get outline_name string if provided, else will be auto-generated */
	name_str = outline_name ? text_to_cstring(outline_name) : NULL;

	/* Extract hint positions from the query */
	hint_positions = extract_inline_hints_with_positions(query_str);

	if (list_length(hint_positions) == 0)
	{
		elog(WARNING, "pg_outline_create_from_sql: no hints found in query");
		PG_RETURN_BOOL(false);
	}

	elog(NOTICE, "Found %d hint(s) in query", list_length(hint_positions));

	/* Parse the raw query to get RawStmt with location info */
	raw_parsetree_list = pg_parse_query(query_str);

	if (list_length(raw_parsetree_list) != 1)
	{
		elog(WARNING, "pg_outline_create_from_sql: query must contain exactly one statement");
		PG_RETURN_BOOL(false);
	}

	raw_stmt = linitial_node(RawStmt, raw_parsetree_list);

	/* Analyze the query to get Query tree */
	query = parse_analyze(raw_stmt, query_str, NULL, 0, NULL);

	/* Create a hash table for Query metadata (names and hints) */
	current_query_metadata = create_query_metadata_table(CurrentMemoryContext);

	PG_TRY();
	{
		/* First, assign names to all Query structures in the tree */
		{
			QueryNamingContext naming_context;
			memset(&naming_context, 0, sizeof(QueryNamingContext));
			assign_query_names(query, &naming_context, NULL, NULL);
		}

		/* Then assign hints to Query structures based on their locations */
		assign_hints_to_queries(query, hint_positions, query_str);

		/* Log Query names and their hints for debugging */
		elog(NOTICE, "=== Query Structure Analysis ===");
		log_query_names_and_hints(query);
		elog(NOTICE, "=== End of Query Structure Analysis ===");

		/*
		 * Count how many Query structures have hints assigned
		 */
		/* TODO: Walk the Query tree and count queries with hints */

		/*
		 * Plan the query - the hints are now stored in the hash table
		 * The planner can access hints via get_query_hints() for each Query
		 */
		plan = standard_planner(query, 0, NULL);

		/* Extract hints from the generated plan */
		if (plan && plan->planTree)
		{
			extract_hints_from_plan_tree(plan->planTree, &hints, 0);
		}

		if (list_length(hints) == 0)
		{
			elog(WARNING, "pg_outline_create_from_sql: no hints could be extracted from plan");
			/* Cleanup before returning */
			destroy_query_metadata_table(current_query_metadata);
			current_query_metadata = NULL;
			PG_RETURN_BOOL(false);
		}

		/* Format hints for storage */
		initStringInfo(&hint_str);
		foreach(lc, hints)
		{
			char	   *hint = (char *) lfirst(lc);

			if (hint)
			{
				if (!first)
					appendStringInfoChar(&hint_str, ' ');
				appendStringInfoString(&hint_str, hint);
				first = false;
			}
		}

		/* Strip hints and normalize query for fingerprint */
		query_without_hints = strip_hints_from_query(query_str);
		normalized = normalize_query(query_without_hints);
		fingerprint = compute_query_fingerprint(normalized);

		/*
		 * Store the outline with the collected hints
		 * Use normalized query as the pattern for consistent matching
		 * If name_str is NULL, store_outline_hints will auto-generate from fingerprint
		 */
		store_outline_hints(name_str, normalized, fingerprint, hint_str.data);

		/* Get the effective name for the notice message */
		if (!name_str || name_str[0] == '\0')
		{
			char *short_fp = get_short_fingerprint(fingerprint);
			name_str = psprintf("outline_%s", short_fp);
			pfree(short_fp);
		}

		elog(NOTICE, "pg_outline_create_from_sql: outline '%s' created with fingerprint %s",
			 name_str, fingerprint ? fingerprint : "none");

		if (normalized)
			pfree(normalized);
		if (fingerprint)
			pfree(fingerprint);

		/* Cleanup hash table */
		destroy_query_metadata_table(current_query_metadata);
		current_query_metadata = NULL;
	}
	PG_CATCH();
	{
		/* Cleanup hash table on error */
		if (current_query_metadata)
		{
			destroy_query_metadata_table(current_query_metadata);
			current_query_metadata = NULL;
		}
		PG_RE_THROW();
	}
	PG_END_TRY();

	PG_RETURN_BOOL(true);
}

/*
 * SQL function: update an existing outline
 * This function only updates, it will fail if the outline doesn't exist
 */
Datum
pg_outline_update(PG_FUNCTION_ARGS)
{
	text	   *outline_name = PG_ARGISNULL(0) ? NULL : PG_GETARG_TEXT_PP(0);
	text	   *query_text = PG_GETARG_TEXT_PP(1);
	char	   *name_str;
	char	   *query_str;
	char	   *hints_str;
	char	   *normalized;
	char	   *fingerprint;

	/* Check for NULL query_text (required) */
	if (PG_ARGISNULL(1))
		PG_RETURN_NULL();

	/* Get outline_name string if provided, else pass NULL for auto-generation */
	name_str = outline_name ? text_to_cstring(outline_name) : NULL;
	query_str = text_to_cstring(query_text);

	/* Handle optional hints parameter */
	if (PG_ARGISNULL(2))
		hints_str = pstrdup("");  /* Use empty string for NULL hints */
	else
		hints_str = text_to_cstring(PG_GETARG_TEXT_PP(2));

	/* Normalize query and compute fingerprint */
	normalized = normalize_query(query_str);
	fingerprint = compute_query_fingerprint(normalized);

	/* Update the outline - will fail if it doesn't exist */
	update_outline_hints(name_str, normalized, fingerprint, hints_str);

	/* Get the effective name for the notice message */
	if (!name_str || name_str[0] == '\0')
	{
		char *short_fp = get_short_fingerprint(fingerprint);
		name_str = psprintf("outline_%s", short_fp);
		pfree(short_fp);
	}

	elog(NOTICE, "pg_outline_update: outline '%s' updated with fingerprint %s",
		 name_str, fingerprint ? fingerprint : "none");

	if (normalized)
		pfree(normalized);
	if (fingerprint)
		pfree(fingerprint);
	pfree(hints_str);

	PG_RETURN_BOOL(true);
}

/*
 * SQL function: drop an outline
 */
Datum
pg_outline_drop(PG_FUNCTION_ARGS)
{
	text	   *outline_name = PG_GETARG_TEXT_PP(0);
	char	   *name_str = text_to_cstring(outline_name);
	int			ret;
	StringInfoData query;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "pg_outline: SPI_connect failed");

	initStringInfo(&query);
	appendStringInfo(&query,
					 "DELETE FROM pg_outline_data WHERE fingerprint = %s OR outline_name = %s",
					 quote_literal_cstr(name_str),
					 quote_literal_cstr(name_str));

	ret = SPI_execute(query.data, false, 0);

	if (ret < 0 || SPI_processed == 0)
	{
		SPI_finish();
		elog(WARNING, "pg_outline_drop: outline '%s' not found", name_str);
		PG_RETURN_BOOL(false);
	}

	SPI_finish();

	elog(NOTICE, "pg_outline_drop: outline '%s' dropped", name_str);

	PG_RETURN_BOOL(true);
}

/*
 * SQL function: enable an outline
 */
Datum
pg_outline_enable(PG_FUNCTION_ARGS)
{
	text	   *outline_name = PG_GETARG_TEXT_PP(0);
	char	   *name_str = text_to_cstring(outline_name);
	int			ret;
	StringInfoData query;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "pg_outline: SPI_connect failed");

	initStringInfo(&query);
	appendStringInfo(&query,
					 "UPDATE pg_outline_data SET enabled = true, updated_at = CURRENT_TIMESTAMP "
					 "WHERE fingerprint = %s OR outline_name = %s",
					 quote_literal_cstr(name_str),
					 quote_literal_cstr(name_str));

	ret = SPI_execute(query.data, false, 0);

	if (ret < 0 || SPI_processed == 0)
	{
		SPI_finish();
		elog(WARNING, "pg_outline_enable: outline '%s' not found", name_str);
		PG_RETURN_BOOL(false);
	}

	SPI_finish();

	elog(NOTICE, "pg_outline_enable: outline '%s' enabled", name_str);

	PG_RETURN_BOOL(true);
}

/*
 * SQL function: disable an outline
 */
Datum
pg_outline_disable(PG_FUNCTION_ARGS)
{
	text	   *outline_name = PG_GETARG_TEXT_PP(0);
	char	   *name_str = text_to_cstring(outline_name);
	int			ret;
	StringInfoData query;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "pg_outline: SPI_connect failed");

	initStringInfo(&query);
	appendStringInfo(&query,
					 "UPDATE pg_outline_data SET enabled = false, updated_at = CURRENT_TIMESTAMP "
					 "WHERE fingerprint = %s OR outline_name = %s",
					 quote_literal_cstr(name_str),
					 quote_literal_cstr(name_str));

	ret = SPI_execute(query.data, false, 0);

	if (ret < 0 || SPI_processed == 0)
	{
		SPI_finish();
		elog(WARNING, "pg_outline_disable: outline '%s' not found", name_str);
		PG_RETURN_BOOL(false);
	}

	SPI_finish();

	elog(NOTICE, "pg_outline_disable: outline '%s' disabled", name_str);

	PG_RETURN_BOOL(true);
}

/*
 * SQL function: list all outlines
 */
Datum
pg_outline_list(PG_FUNCTION_ARGS)
{
	/* This function is implemented in SQL in the --1.0.sql file */
	/* Just return void as it's a placeholder */
	elog(NOTICE, "pg_outline_list: use SELECT * FROM pg_outline_list() for listing");

	PG_RETURN_VOID();
}

/*
 * parse_stored_hints - Parse stored hint string into structured hints
 *
 * Input format: "[query_name] hint_text\n[query_name2] hint_text2"
 * Returns: List of ParsedHint structures
 */
static List *
parse_stored_hints(const char *hints_string)
{
	List	   *result = NIL;
	const char *p;
	ParsedHint *parsed_hint;

	if (!hints_string || hints_string[0] == '\0')
		return NIL;

	p = hints_string;
	while (*p)
	{
		/* Skip whitespace */
		while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
			p++;

		if (!*p)
			break;

		/* Look for [query_name] */
		if (*p == '[')
		{
			const char *name_start = p + 1;
			const char *name_end = strchr(name_start, ']');

			if (name_end)
			{
				size_t		name_len = name_end - name_start;
				char	   *query_name;
				const char *hint_start;
				const char *hint_end;
				char	   *hint_text;
				size_t		hint_len;

				query_name = (char *) palloc(name_len + 1);
				memcpy(query_name, name_start, name_len);
				query_name[name_len] = '\0';

				/* Skip past ] and spaces */
				p = name_end + 1;
				while (*p && (*p == ' ' || *p == '\t'))
					p++;

				/* Extract hint text until newline or end of string */
				hint_start = p;
				while (*p && *p != '\n' && *p != '\r')
					p++;

				hint_len = p - hint_start;
				hint_text = (char *) palloc(hint_len + 1);
				memcpy(hint_text, hint_start, hint_len);
				hint_text[hint_len] = '\0';

				/* Create ParsedHint and add to list */
				parsed_hint = (ParsedHint *) palloc(sizeof(ParsedHint));
				parsed_hint->query_name = query_name;
				parsed_hint->hint_text = hint_text;
				result = lappend(result, parsed_hint);

				elog(DEBUG2, "pg_outline: parsed hint [%s] %s", query_name, hint_text);
			}
			else
			{
				/* Skip to next line if malformed */
				while (*p && *p != '\n')
					p++;
			}
		}
		else
		{
			/* Skip unrecognized content */
			while (*p && *p != '\n')
				p++;
		}
	}

	return result;
}

/*
 * apply_hints_to_query - Apply parsed hints to Query tree via metadata
 *
 * Associates each hint with its corresponding Query node by looking up
 * the query name in the metadata hash table and storing the hint there.
 */
static void
apply_hints_to_query(Query *query, List *parsed_hints)
{
	ListCell   *lc;

	if (!query || !current_query_metadata || !parsed_hints)
		return;

	/* Iterate through all parsed hints */
	foreach(lc, parsed_hints)
	{
		ParsedHint *hint = (ParsedHint *) lfirst(lc);
		QueryMetadataEntry *entry;
		int			bucket;
		uint64		hash;
		bool		found = false;

		/* Try to find the Query with this name */
		for (bucket = 0; bucket < QUERY_METADATA_HASH_SIZE; bucket++)
		{
			for (entry = current_query_metadata->buckets[bucket]; entry != NULL; entry = entry->next)
			{
				if (entry->query_name && strcmp(entry->query_name, hint->query_name) == 0)
				{
					/* Found matching query - store hint */
					if (entry->query_hints)
					{
						/* Append to existing hints */
						char	   *old_hints = entry->query_hints;
						char	   *new_hints = psprintf("%s %s", old_hints, hint->hint_text);

						pfree(old_hints);
						entry->query_hints = new_hints;
					}
					else
					{
						entry->query_hints = pstrdup(hint->hint_text);
					}
					elog(DEBUG1, "pg_outline: applied hint to query '%s': %s",
						 hint->query_name, hint->hint_text);
					found = true;
					break;
				}
			}
			if (found)
				break;
		}

		if (!found)
		{
			elog(DEBUG1, "pg_outline: could not find query named '%s' for hint application",
				 hint->query_name);
		}
	}
}

/*
 * outline_set_rel_pathlist - Hook to enforce scan method hints
 *
 * This hook is called when the planner is generating paths for a relation.
 * We examine the active hints and filter out paths that don't match the hint.
 */
static void
outline_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	List	   *kept_paths = NIL;
	ListCell   *lc;
	char	   *rel_name;
	char	   *query_hints;
	bool		has_seqscan_hint = false;
	bool		has_indexscan_hint = false;
	bool		has_indexonlyscan_hint = false;

	/* Call previous hook first if it exists */
	if (prev_set_rel_pathlist_hook)
		prev_set_rel_pathlist_hook(root, rel, rti, rte);

	/* Only apply hints if we have active hints */
	if (active_outline_hints == NIL || !current_query_metadata)
		return;

	/* Only apply to base relations */
	if (rel->reloptkind != RELOPT_BASEREL)
		return;

	/* Get relation name */
	if (rte->rtekind == RTE_RELATION)
	{
		rel_name = get_rel_name(rte->relid);
		if (!rel_name)
			return;
	}
	else
	{
		return;  /* Not a regular table */
	}

	/* Get hints for the current query */
	query_hints = get_query_hints(root->parse);
	if (!query_hints)
	{
		pfree(rel_name);
		return;
	}

	/* Check if this relation is mentioned in any hint */
	if (strstr(query_hints, "SeqScan") && strstr(query_hints, rel_name))
		has_seqscan_hint = true;
	if (strstr(query_hints, "IndexScan") && strstr(query_hints, rel_name))
		has_indexscan_hint = true;
	if (strstr(query_hints, "IndexOnlyScan") && strstr(query_hints, rel_name))
		has_indexonlyscan_hint = true;

	/* If we have a specific scan hint for this relation, filter paths */
	if (has_seqscan_hint || has_indexscan_hint || has_indexonlyscan_hint)
	{
		elog(DEBUG1, "pg_outline: filtering paths for relation '%s' based on hints", rel_name);

		foreach(lc, rel->pathlist)
		{
			Path	   *path = (Path *) lfirst(lc);
			bool		keep = false;

			/* Keep paths that match the hint */
			if (has_seqscan_hint && IsA(path, Path) && path->pathtype == T_SeqScan)
				keep = true;
			if (has_indexscan_hint && IsA(path, IndexPath))
				keep = true;
			if (has_indexonlyscan_hint && IsA(path, IndexPath))
			{
				IndexPath  *ipath = (IndexPath *) path;

				/* Check if it's an index-only scan */
				if (ipath->indexinfo && ipath->indexinfo->canreturn)
					keep = true;
			}

			if (keep)
				kept_paths = lappend(kept_paths, path);
		}

		/* If we filtered to some paths, use only those */
		if (kept_paths != NIL)
		{
			elog(DEBUG1, "pg_outline: filtered from %d to %d paths for '%s'",
				 list_length(rel->pathlist), list_length(kept_paths), rel_name);
			rel->pathlist = kept_paths;
		}
	}

	pfree(rel_name);
}

/*
 * Parse Leading hint string and build a tree structure
 *
 * Supports the following formats:
 * 1. Simple flat format: "t1 t2 t3" -> left-to-right join order
 * 2. Nested format: "(t1 t2) t3" -> join t1 and t2 first, then join with t3
 * 3. Bushy format: "(t1 t2) (t3 t4)" -> two independent joins
 *
 * Returns a tree structure representing the join order, or NULL on parse error
 */
static LeadingHintNode *
parse_leading_hint(const char *hint_str)
{
	const char *str_ptr;
	LeadingHintNode *result;

	if (!hint_str || hint_str[0] == '\0')
		return NULL;

	str_ptr = hint_str;
	result = parse_leading_hint_recursive(&str_ptr);

	if (result)
		elog(DEBUG1, "pg_outline: Successfully parsed Leading hint: %s", hint_str);
	else
		elog(WARNING, "pg_outline: Failed to parse Leading hint: %s", hint_str);

	return result;
}

/*
 * Recursive parser for Leading hint syntax
 *
 * Grammar:
 *   element ::= relation_name | '(' element element ')'
 *
 * This function parses one element and advances *str_ptr
 */
static LeadingHintNode *
parse_leading_hint_recursive(const char **str_ptr)
{
	LeadingHintNode *node;
	const char *p;

	if (!str_ptr || !*str_ptr)
		return NULL;

	p = *str_ptr;

	/* Skip whitespace */
	while (*p && isspace((unsigned char) *p))
		p++;

	if (*p == '\0')
		return NULL;

	/* Check for opening parenthesis - nested join */
	if (*p == '(')
	{
		LeadingHintNode *left, *right;

		p++; /* Skip '(' */

		/* Parse left subtree */
		*str_ptr = p;
		left = parse_leading_hint_recursive(str_ptr);
		if (!left)
		{
			elog(DEBUG1, "pg_outline: Failed to parse left subtree in Leading hint");
			return NULL;
		}

		p = *str_ptr;

		/* Skip whitespace */
		while (*p && isspace((unsigned char) *p))
			p++;

		/* Parse right subtree */
		*str_ptr = p;
		right = parse_leading_hint_recursive(str_ptr);
		if (!right)
		{
			elog(DEBUG1, "pg_outline: Failed to parse right subtree in Leading hint");
			free_leading_hint_tree(left);
			return NULL;
		}

		p = *str_ptr;

		/* Skip whitespace */
		while (*p && isspace((unsigned char) *p))
			p++;

		/* Expect closing parenthesis */
		if (*p != ')')
		{
			elog(DEBUG1, "pg_outline: Expected ')' in Leading hint, found '%c'", *p ? *p : '\0');
			free_leading_hint_tree(left);
			free_leading_hint_tree(right);
			return NULL;
		}

		p++; /* Skip ')' */
		*str_ptr = p;

		/* Create join node */
		node = (LeadingHintNode *) palloc0(sizeof(LeadingHintNode));
		node->type = LEADING_NODE_JOIN;
		node->u.join.left = left;
		node->u.join.right = right;

		return node;
	}
	else if (isalpha((unsigned char) *p) || *p == '_')
	{
		/* Parse relation name */
		const char *name_start = p;
		char *relation_name;
		int name_len;

		/* Relation name can contain alphanumeric characters, underscores, and dollar signs */
		while (*p && (isalnum((unsigned char) *p) || *p == '_' || *p == '$'))
			p++;

		name_len = p - name_start;
		if (name_len == 0)
			return NULL;

		/* Allocate and copy relation name */
		relation_name = palloc(name_len + 1);
		strncpy(relation_name, name_start, name_len);
		relation_name[name_len] = '\0';

		*str_ptr = p;

		/* Create relation node */
		node = (LeadingHintNode *) palloc0(sizeof(LeadingHintNode));
		node->type = LEADING_NODE_RELATION;
		node->u.relation_name = relation_name;

		elog(DEBUG2, "pg_outline: Parsed relation name: %s", relation_name);

		return node;
	}
	else
	{
		/* Unexpected character */
		elog(DEBUG1, "pg_outline: Unexpected character '%c' in Leading hint", *p ? *p : '\0');
		return NULL;
	}
}

/*
 * Free a Leading hint tree structure
 */
static void
free_leading_hint_tree(LeadingHintNode *node)
{
	if (!node)
		return;

	if (node->type == LEADING_NODE_JOIN)
	{
		free_leading_hint_tree(node->u.join.left);
		free_leading_hint_tree(node->u.join.right);
	}
	else if (node->type == LEADING_NODE_RELATION)
	{
		if (node->u.relation_name)
			pfree(node->u.relation_name);
	}

	pfree(node);
}

/*
 * Convert Leading hint tree to string representation (for debugging)
 */
static char *
leading_hint_node_to_string(LeadingHintNode *node)
{
	StringInfoData buf;

	if (!node)
		return pstrdup("(null)");

	initStringInfo(&buf);
	leading_hint_node_to_string_buf(node, &buf);
	return buf.data;
}

/*
 * Helper function to recursively build string representation
 */
static void
leading_hint_node_to_string_buf(LeadingHintNode *node, StringInfo buf)
{
	if (!node)
		return;

	if (node->type == LEADING_NODE_RELATION)
	{
		appendStringInfoString(buf, node->u.relation_name);
	}
	else if (node->type == LEADING_NODE_JOIN)
	{
		appendStringInfoChar(buf, '(');
		leading_hint_node_to_string_buf(node->u.join.left, buf);
		appendStringInfoChar(buf, ' ');
		leading_hint_node_to_string_buf(node->u.join.right, buf);
		appendStringInfoChar(buf, ')');
	}
}

/*
 * outline_join_search - Hook to enforce Leading hints for join order
 *
 * This hook is called to determine the join order for a query.
 * We examine the active hints for Leading hints and enforce the specified join order.
 */
static RelOptInfo *
outline_join_search(PlannerInfo *root, int levels_needed, List *initial_rels)
{
	char	   *query_hints;
	char	   *leading_hint_start;
	char	   *leading_hint_end;
	char	   *leading_content;
	LeadingHintNode *leading_tree;
	char	   *parsed_tree_str;
	int			len;

	/* Call previous hook first if it exists */
	if (prev_join_search_hook)
		return prev_join_search_hook(root, levels_needed, initial_rels);

	/* Only apply hints if we have active hints */
	if (active_outline_hints == NIL || !current_query_metadata)
		return standard_join_search(root, levels_needed, initial_rels);

	/* Get hints for the current query */
	query_hints = get_query_hints(root->parse);
	if (!query_hints)
		return standard_join_search(root, levels_needed, initial_rels);

	/* Look for Leading hint in the format "Leading(t1 t2 t3)" or "Leading((t1 t2) t3)" */
	leading_hint_start = strstr(query_hints, "Leading(");
	if (!leading_hint_start)
		return standard_join_search(root, levels_needed, initial_rels);

	/* Find the matching closing parenthesis for Leading hint */
	leading_hint_start += strlen("Leading(");

	/* We need to find the matching closing parenthesis, accounting for nested parens */
	{
		const char *p = leading_hint_start;
		int paren_depth = 1;  /* We're already inside the first '(' */

		leading_hint_end = NULL;
		while (*p && paren_depth > 0)
		{
			if (*p == '(')
				paren_depth++;
			else if (*p == ')')
			{
				paren_depth--;
				if (paren_depth == 0)
				{
					/* Cast away const here since we're just storing a pointer for calculation */
					leading_hint_end = (char *) p;
					break;
				}
			}
			p++;
		}

		if (!leading_hint_end)
		{
			elog(DEBUG1, "pg_outline: Could not find matching ')' for Leading hint");
			return standard_join_search(root, levels_needed, initial_rels);
		}
	}

	len = leading_hint_end - leading_hint_start;
	if (len <= 0)
		return standard_join_search(root, levels_needed, initial_rels);

	/* Extract the Leading hint content */
	leading_content = palloc(len + 1);
	strncpy(leading_content, leading_hint_start, len);
	leading_content[len] = '\0';

	elog(NOTICE, "pg_outline: Found Leading hint: Leading(%s)", leading_content);

	/* Parse the Leading hint into a tree structure */
	leading_tree = parse_leading_hint(leading_content);

	if (leading_tree)
	{
		/* Convert parsed tree back to string for verification */
		parsed_tree_str = leading_hint_node_to_string(leading_tree);
		elog(NOTICE, "pg_outline: Parsed Leading hint tree structure: %s", parsed_tree_str);
		pfree(parsed_tree_str);

		/* Free the tree structure */
		free_leading_hint_tree(leading_tree);
	}
	else
	{
		elog(WARNING, "pg_outline: Failed to parse Leading hint: Leading(%s)", leading_content);
	}

	pfree(leading_content);

	/* Note: We only parse the hint here; actual join order enforcement is not implemented */
	elog(DEBUG1, "pg_outline: Leading hint parsed successfully, but join order enforcement is not implemented");

	return standard_join_search(root, levels_needed, initial_rels);
}

