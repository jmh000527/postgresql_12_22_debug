/*-------------------------------------------------------------------------
 *
 * outline_hints.c
 *	  Hint parsing and management for outline system
 *
 * This file implements parsing of hints in pg_hint_plan format for the
 * outline system.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/outline/outline_hints.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/outline_hints.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "lib/stringinfo.h"
#include "catalog/pg_outline.h"
#include "utils/syscache.h"
#include "access/htup_details.h"
#include "parser/scansup.h"

/*
 * Parse a hint string and return a HintState structure
 *
 * Hint format (similar to pg_hint_plan):
 *   SeqScan(table)
 *   IndexScan(table index)
 *   NoSeqScan(table)
 *   NoIndexScan(table)
 *   NestLoop(table1 table2)
 *   HashJoin(table1 table2)
 *   MergeJoin(table1 table2)
 */
HintState *
parse_hints(const char *hint_str)
{
	HintState  *hstate;
	char	   *str;
	char	   *p;
	char	   *start;

	if (hint_str == NULL || *hint_str == '\0')
		return NULL;

	hstate = (HintState *) palloc0(sizeof(HintState));
	hstate->hints = NIL;
	hstate->enabled = true;
	hstate->outline_oid = InvalidOid;

	/* Make a working copy of the hint string */
	str = pstrdup(hint_str);
	p = str;

	/* Parse each hint separated by whitespace or newlines */
	while (*p != '\0')
	{
		Hint	   *hint;
		char	   *hint_name;
		char	   *args;
		char	   *lparen;
		char	   *rparen;

		/* Skip whitespace */
		while (*p && isspace((unsigned char) *p))
			p++;

		if (*p == '\0')
			break;

		/* Find hint name and arguments */
		start = p;
		lparen = strchr(p, '(');
		if (lparen == NULL)
			break;			/* No more hints */

		/* Extract hint name */
		*lparen = '\0';
		hint_name = pstrdup(start);
		*lparen = '(';

		/* Find matching rparen */
		rparen = strchr(lparen + 1, ')');
		if (rparen == NULL)
			break;			/* Malformed hint */

		/* Extract arguments */
		*rparen = '\0';
		args = pstrdup(lparen + 1);
		*rparen = ')';

		/* Create appropriate hint structure */
		hint = (Hint *) palloc0(sizeof(Hint));

		/* Parse scan method hints */
		if (pg_strcasecmp(hint_name, "SeqScan") == 0)
		{
			hint->type = HINT_TYPE_SCAN_METHOD;
			hint->hint.scan.type = HINT_TYPE_SCAN_METHOD;
			hint->hint.scan.relname = pstrdup(args);
			hint->hint.scan.method = SCAN_HINT_SEQSCAN;
			hint->hint.scan.indexname = NULL;
			hstate->hints = lappend(hstate->hints, hint);
		}
		else if (pg_strcasecmp(hint_name, "IndexScan") == 0)
		{
			char	   *relname;
			char	   *indexname = NULL;
			char	   *space;

			relname = pstrdup(args);
			space = strchr(relname, ' ');
			if (space != NULL)
			{
				*space = '\0';
				indexname = pstrdup(space + 1);
			}

			hint->type = HINT_TYPE_SCAN_METHOD;
			hint->hint.scan.type = HINT_TYPE_SCAN_METHOD;
			hint->hint.scan.relname = relname;
			hint->hint.scan.indexname = indexname;
			hint->hint.scan.method = SCAN_HINT_INDEXSCAN;
			hstate->hints = lappend(hstate->hints, hint);
		}
		else if (pg_strcasecmp(hint_name, "NoSeqScan") == 0)
		{
			hint->type = HINT_TYPE_SCAN_METHOD;
			hint->hint.scan.type = HINT_TYPE_SCAN_METHOD;
			hint->hint.scan.relname = pstrdup(args);
			hint->hint.scan.method = SCAN_HINT_NOSEQSCAN;
			hint->hint.scan.indexname = NULL;
			hstate->hints = lappend(hstate->hints, hint);
		}
		else if (pg_strcasecmp(hint_name, "NoIndexScan") == 0)
		{
			hint->type = HINT_TYPE_SCAN_METHOD;
			hint->hint.scan.type = HINT_TYPE_SCAN_METHOD;
			hint->hint.scan.relname = pstrdup(args);
			hint->hint.scan.method = SCAN_HINT_NOINDEXSCAN;
			hint->hint.scan.indexname = NULL;
			hstate->hints = lappend(hstate->hints, hint);
		}
		/* Parse join method hints */
		else if (pg_strcasecmp(hint_name, "NestLoop") == 0)
		{
			char	   *rel1;
			char	   *rel2;
			char	   *space;

			rel1 = pstrdup(args);
			space = strchr(rel1, ' ');
			if (space != NULL)
			{
				*space = '\0';
				rel2 = pstrdup(space + 1);

				hint->type = HINT_TYPE_JOIN_METHOD;
				hint->hint.join.type = HINT_TYPE_JOIN_METHOD;
				hint->hint.join.relname1 = rel1;
				hint->hint.join.relname2 = rel2;
				hint->hint.join.method = JOIN_HINT_NESTLOOP;
				hstate->hints = lappend(hstate->hints, hint);
			}
			else
				pfree(hint);
		}
		else if (pg_strcasecmp(hint_name, "HashJoin") == 0)
		{
			char	   *rel1;
			char	   *rel2;
			char	   *space;

			rel1 = pstrdup(args);
			space = strchr(rel1, ' ');
			if (space != NULL)
			{
				*space = '\0';
				rel2 = pstrdup(space + 1);

				hint->type = HINT_TYPE_JOIN_METHOD;
				hint->hint.join.type = HINT_TYPE_JOIN_METHOD;
				hint->hint.join.relname1 = rel1;
				hint->hint.join.relname2 = rel2;
				hint->hint.join.method = JOIN_HINT_HASHJOIN;
				hstate->hints = lappend(hstate->hints, hint);
			}
			else
				pfree(hint);
		}
		else if (pg_strcasecmp(hint_name, "MergeJoin") == 0)
		{
			char	   *rel1;
			char	   *rel2;
			char	   *space;

			rel1 = pstrdup(args);
			space = strchr(rel1, ' ');
			if (space != NULL)
			{
				*space = '\0';
				rel2 = pstrdup(space + 1);

				hint->type = HINT_TYPE_JOIN_METHOD;
				hint->hint.join.type = HINT_TYPE_JOIN_METHOD;
				hint->hint.join.relname1 = rel1;
				hint->hint.join.relname2 = rel2;
				hint->hint.join.method = JOIN_HINT_MERGEJOIN;
				hstate->hints = lappend(hstate->hints, hint);
			}
			else
				pfree(hint);
		}
		else
		{
			/* Unknown hint, skip it */
			pfree(hint);
		}

		pfree(hint_name);
		pfree(args);

		/* Move to next hint */
		p = rparen + 1;
	}

	pfree(str);
	return hstate;
}

/*
 * Free a HintState structure
 */
void
free_hint_state(HintState *hstate)
{
	if (hstate == NULL)
		return;

	if (hstate->hints != NIL)
		list_free_deep(hstate->hints);

	pfree(hstate);
}

/*
 * Get hints for a query from the outline catalog
 *
 * This function looks up the pg_outline catalog for a matching query
 * and returns the parsed hints if found.
 */
HintState *
get_hints_for_query(const char *query_string)
{
	/* TODO: Implement query normalization and lookup in pg_outline */
	/* For now, return NULL (no hints) */
	return NULL;
}
