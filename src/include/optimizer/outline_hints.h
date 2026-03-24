/*-------------------------------------------------------------------------
 *
 * outline_hints.h
 *	  Definitions for outline hint system
 *
 * This implements a hint system similar to pg_hint_plan for controlling
 * query execution plans through stored outlines.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/outline_hints.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef OUTLINE_HINTS_H
#define OUTLINE_HINTS_H

#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"

/* Hint types */
typedef enum HintType
{
	HINT_TYPE_SCAN_METHOD,		/* Scan method hints (SeqScan, IndexScan, etc.) */
	HINT_TYPE_JOIN_METHOD,		/* Join method hints (NestLoop, HashJoin, MergeJoin) */
	HINT_TYPE_LEADING			/* Join order hints */
} HintType;

/* Scan method hints */
typedef enum ScanMethodHint
{
	SCAN_HINT_SEQSCAN,			/* Force sequential scan */
	SCAN_HINT_INDEXSCAN,		/* Force index scan */
	SCAN_HINT_INDEXONLYSCAN,	/* Force index-only scan */
	SCAN_HINT_BITMAPSCAN,		/* Force bitmap scan */
	SCAN_HINT_TIDSCAN,			/* Force TID scan */
	SCAN_HINT_NOSEQSCAN,		/* Disable sequential scan */
	SCAN_HINT_NOINDEXSCAN,		/* Disable index scan */
	SCAN_HINT_NOINDEXONLYSCAN,	/* Disable index-only scan */
	SCAN_HINT_NOBITMAPSCAN,		/* Disable bitmap scan */
	SCAN_HINT_NOTIDSCAN			/* Disable TID scan */
} ScanMethodHint;

/* Join method hints */
typedef enum JoinMethodHint
{
	JOIN_HINT_NESTLOOP,			/* Force nested loop join */
	JOIN_HINT_HASHJOIN,			/* Force hash join */
	JOIN_HINT_MERGEJOIN,		/* Force merge join */
	JOIN_HINT_NONESTLOOP,		/* Disable nested loop join */
	JOIN_HINT_NOHASHJOIN,		/* Disable hash join */
	JOIN_HINT_NOMERGEJOIN		/* Disable merge join */
} JoinMethodHint;

/* Hint structure for scan methods */
typedef struct ScanHint
{
	HintType	type;			/* HINT_TYPE_SCAN_METHOD */
	char	   *relname;		/* Relation name */
	char	   *indexname;		/* Index name (optional, for IndexScan hints) */
	ScanMethodHint method;		/* Scan method to use or avoid */
} ScanHint;

/* Hint structure for join methods */
typedef struct JoinHint
{
	HintType	type;			/* HINT_TYPE_JOIN_METHOD */
	char	   *relname1;		/* First relation name */
	char	   *relname2;		/* Second relation name */
	JoinMethodHint method;		/* Join method to use or avoid */
} JoinHint;

/* Hint structure for join order */
typedef struct LeadingHint
{
	HintType	type;			/* HINT_TYPE_LEADING */
	List	   *relnames;		/* List of relation names in join order */
} LeadingHint;

/* Generic hint structure */
typedef struct Hint
{
	HintType	type;
	union
	{
		ScanHint	scan;
		JoinHint	join;
		LeadingHint	leading;
	} hint;
} Hint;

/* Hint state for a query */
typedef struct HintState
{
	List	   *hints;			/* List of Hint structures */
	bool		enabled;		/* Whether hints are enabled */
	Oid			outline_oid;	/* OID of the outline these hints came from */
} HintState;

/* Function declarations */
extern HintState *parse_hints(const char *hint_str);
extern void free_hint_state(HintState *hstate);
extern char *plan_to_hints(PlannedStmt *plan, const char *query_string);
extern char *format_outline_data(const char *hints);
extern HintState *get_hints_for_query(const char *query_string);

/* Hint state management */
extern void outline_hints_init(void);
extern void outline_set_hint_state(HintState *hstate);
extern HintState *outline_get_hint_state(void);
extern void outline_clear_hint_state(void);

/* Hook functions for applying hints */
extern void outline_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
									Index rti, RangeTblEntry *rte);
extern void outline_set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel,
									 RelOptInfo *outerrel, RelOptInfo *innerrel,
									 JoinType jointype, JoinPathExtraData *extra);

/* GUC parameters and initialization */
extern bool outline_display_hints;
extern bool outline_recording_mode;
extern void outline_init_guc(void);

/* Query normalization and outline lookup */
extern char *normalize_query_string(const char *query_string);
extern void record_outline_for_query(const char *query_string, const char *hints);

#endif							/* OUTLINE_HINTS_H */
