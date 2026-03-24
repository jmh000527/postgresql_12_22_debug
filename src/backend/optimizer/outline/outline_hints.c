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
#include "catalog/namespace.h"
#include "utils/syscache.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_type.h"
#include "parser/scansup.h"
#include "miscadmin.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include <ctype.h>

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
 * Normalize a query string for outline matching
 *
 * This function normalizes a query by:
 * - Trimming leading/trailing whitespace
 * - Collapsing multiple spaces into single spaces
 * - Converting to lowercase (for case-insensitive matching)
 *
 * Returns a palloc'd normalized string.
 */
char *
normalize_query_string(const char *query_string)
{
	StringInfoData	buf;
	const char	   *p;
	bool			in_space = false;
	bool			in_quote = false;
	char			quote_char = '\0';

	if (query_string == NULL)
		return NULL;

	initStringInfo(&buf);

	/* Skip leading whitespace */
	p = query_string;
	while (*p && isspace((unsigned char) *p))
		p++;

	/* Process the query string */
	while (*p)
	{
		char c = *p;

		/* Handle string literals - preserve them as-is */
		if (!in_quote && (c == '\'' || c == '"'))
		{
			in_quote = true;
			quote_char = c;
			appendStringInfoChar(&buf, c);
			p++;
			continue;
		}
		else if (in_quote)
		{
			appendStringInfoChar(&buf, c);
			if (c == quote_char)
			{
				/* Check for escaped quote */
				if (*(p + 1) == quote_char)
				{
					p++;
					appendStringInfoChar(&buf, quote_char);
				}
				else
				{
					in_quote = false;
				}
			}
			p++;
			continue;
		}

		/* Collapse whitespace */
		if (isspace((unsigned char) c))
		{
			if (!in_space && buf.len > 0)
			{
				appendStringInfoChar(&buf, ' ');
				in_space = true;
			}
		}
		else
		{
			/* Convert to lowercase for case-insensitive matching */
			appendStringInfoChar(&buf, tolower((unsigned char) c));
			in_space = false;
		}

		p++;
	}

	/* Remove trailing whitespace */
	while (buf.len > 0 && buf.data[buf.len - 1] == ' ')
		buf.len--;
	buf.data[buf.len] = '\0';

	return buf.data;
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
	char		   *normalized_query;
	HeapTuple		tuple;
	Relation		rel;
	SysScanDesc		scan;
	ScanKeyData		scankey;
	HintState	   *hstate = NULL;
	Oid				nspid;

	if (query_string == NULL)
		return NULL;

	/* Normalize the query for matching */
	normalized_query = normalize_query_string(query_string);
	if (normalized_query == NULL)
		return NULL;

	/* Get the current namespace (public by default) */
	nspid = get_namespace_oid("public", true);
	if (!OidIsValid(nspid))
	{
		pfree(normalized_query);
		return NULL;
	}

	/* Open the pg_outline relation */
	rel = table_open(OutlineRelationId, AccessShareLock);

	/* Scan for matching outlines */
	ScanKeyInit(&scankey,
				Anum_pg_outline_outlinenamespace,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(nspid));

	scan = systable_beginscan(rel, InvalidOid, false, NULL, 1, &scankey);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_outline	outlineForm;
		Datum			queryDatum;
		Datum			hintsDatum;
		bool			queryNull, hintsNull;
		char		   *stored_query;
		char		   *normalized_stored;
		char		   *hints_str;

		outlineForm = (Form_pg_outline) GETSTRUCT(tuple);

		/* Skip disabled outlines */
		if (!outlineForm->outlineenabled)
			continue;

		/* Get the query text */
		queryDatum = heap_getattr(tuple, Anum_pg_outline_outlinequery,
								 RelationGetDescr(rel), &queryNull);
		if (queryNull)
			continue;

		stored_query = TextDatumGetCString(queryDatum);
		normalized_stored = normalize_query_string(stored_query);

		/* Check if queries match */
		if (strcmp(normalized_query, normalized_stored) == 0)
		{
			/* Found a match! Get the hints */
			hintsDatum = heap_getattr(tuple, Anum_pg_outline_outlinehints,
									 RelationGetDescr(rel), &hintsNull);
			if (!hintsNull)
			{
				hints_str = TextDatumGetCString(hintsDatum);
				hstate = parse_hints(hints_str);
				if (hstate != NULL)
				{
					hstate->outline_oid = outlineForm->oid;
					ereport(DEBUG1,
							(errmsg("Applied outline \"%s\" to query",
									NameStr(outlineForm->outlinename))));
				}
				pfree(hints_str);
			}

			pfree(normalized_stored);
			pfree(stored_query);
			break;  /* Found a match, stop searching */
		}

		pfree(normalized_stored);
		pfree(stored_query);
	}

	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	pfree(normalized_query);

	return hstate;
}

/*
 * Record an outline for a query during recording mode
 *
 * This function automatically creates or updates an outline for the given query.
 * The outline name is auto-generated based on a counter.
 */
void
record_outline_for_query(const char *query_string, const char *hints)
{
	static int outline_counter = 0;
	char	   *outline_name;
	char	   *normalized_query;
	HeapTuple	tuple;
	Relation	rel;
	Datum		values[Natts_pg_outline];
	bool		nulls[Natts_pg_outline];
	Oid			outline_oid;
	Oid			nspid;
	SysScanDesc	scan;
	ScanKeyData	scankey;
	bool		found_existing = false;

	if (query_string == NULL || hints == NULL)
		return;

	/* Skip if not in a transaction */
	if (!IsTransactionState())
		return;

	/* Normalize the query */
	normalized_query = normalize_query_string(query_string);
	if (normalized_query == NULL)
		return;

	/* Get current namespace */
	nspid = get_namespace_oid("public", false);

	/* Open the relation */
	rel = table_open(OutlineRelationId, RowExclusiveLock);

	/* Check if an outline already exists for this query */
	ScanKeyInit(&scankey,
				Anum_pg_outline_outlinenamespace,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(nspid));

	scan = systable_beginscan(rel, InvalidOid, false, NULL, 1, &scankey);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_outline	outlineForm;
		Datum			queryDatum;
		bool			queryNull;
		char		   *stored_query;
		char		   *normalized_stored;

		outlineForm = (Form_pg_outline) GETSTRUCT(tuple);

		queryDatum = heap_getattr(tuple, Anum_pg_outline_outlinequery,
								 RelationGetDescr(rel), &queryNull);
		if (queryNull)
			continue;

		stored_query = TextDatumGetCString(queryDatum);
		normalized_stored = normalize_query_string(stored_query);

		if (strcmp(normalized_query, normalized_stored) == 0)
		{
			/* Found existing outline for this query */
			found_existing = true;
			ereport(NOTICE,
					(errmsg("Outline \"%s\" already exists for this query, skipping recording",
							NameStr(outlineForm->outlinename))));
			pfree(normalized_stored);
			pfree(stored_query);
			break;
		}

		pfree(normalized_stored);
		pfree(stored_query);
	}

	systable_endscan(scan);

	if (!found_existing)
	{
		/* Generate a unique outline name */
		outline_counter++;
		outline_name = psprintf("auto_outline_%d_%d",
								(int) MyProcPid, outline_counter);

		/* Prepare values */
		MemSet(nulls, false, sizeof(nulls));

		outline_oid = GetNewOidWithIndex(rel, OutlineOidIndexId,
										Anum_pg_outline_oid);
		values[Anum_pg_outline_oid - 1] = ObjectIdGetDatum(outline_oid);
		values[Anum_pg_outline_outlinename - 1] = DirectFunctionCall1(namein,
																	  CStringGetDatum(outline_name));
		values[Anum_pg_outline_outlinenamespace - 1] = ObjectIdGetDatum(nspid);
		values[Anum_pg_outline_outlineowner - 1] = ObjectIdGetDatum(GetUserId());
		values[Anum_pg_outline_outlineenabled - 1] = BoolGetDatum(true);
		values[Anum_pg_outline_outlinequery - 1] = CStringGetTextDatum(normalized_query);
		values[Anum_pg_outline_outlinehints - 1] = CStringGetTextDatum(hints);

		/* Insert the new outline */
		tuple = heap_form_tuple(RelationGetDescr(rel), values, nulls);
		CatalogTupleInsert(rel, tuple);

		heap_freetuple(tuple);

		ereport(NOTICE,
				(errmsg("Created outline \"%s\" for query", outline_name)));

		pfree(outline_name);
	}

	table_close(rel, RowExclusiveLock);
	pfree(normalized_query);
}
