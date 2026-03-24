/*-------------------------------------------------------------------------
 *
 * pg_outline_funcs.c
 *	  SQL-callable functions for outline management
 *
 * This file provides SQL functions for creating, dropping, enabling,
 * and disabling outlines.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/pg_outline_funcs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_outline.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "optimizer/outline_hints.h"
#include "parser/analyze.h"
#include "parser/parser.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * extract_hints_from_outline_data - Extract hints from OceanBase/Oracle style outline data
 *
 * This function extracts hints from the format:
 * / *+
 * BEGIN_OUTLINE_DATA
 * hint1
 * hint2
 * END_OUTLINE_DATA
 * * /
 *
 * Returns the extracted hints as a plain string with hints separated by newlines.
 * If the input doesn't match the outline data format, returns the input unchanged.
 */
static char *
extract_hints_from_outline_data(const char *hints_text)
{
	const char *begin_marker = "BEGIN_OUTLINE_DATA";
	const char *end_marker = "END_OUTLINE_DATA";
	const char *p;
	const char *begin_pos = NULL;
	const char *end_pos = NULL;
	StringInfoData result;

	if (hints_text == NULL)
		return NULL;

	if (*hints_text == '\0')
		return pstrdup("");

	/* Look for BEGIN_OUTLINE_DATA marker */
	p = strstr(hints_text, begin_marker);
	if (p != NULL)
	{
		begin_pos = p + strlen(begin_marker);

		/* Look for END_OUTLINE_DATA marker */
		end_pos = strstr(begin_pos, end_marker);

		if (end_pos != NULL)
		{
			/* Found both markers, extract the content between them */
			initStringInfo(&result);

			/* Skip leading whitespace after BEGIN_OUTLINE_DATA */
			while (begin_pos < end_pos && isspace((unsigned char) *begin_pos))
				begin_pos++;

			/* Copy content, trimming trailing whitespace before END_OUTLINE_DATA */
			while (begin_pos < end_pos && isspace((unsigned char) *(end_pos - 1)))
				end_pos--;

			/* Copy the hints */
			if (begin_pos < end_pos)
				appendBinaryStringInfo(&result, begin_pos, end_pos - begin_pos);

			/* Return the StringInfo data (empty or not) */
			return result.data;
		}
	}

	/* No outline data markers found, return input as-is */
	return pstrdup(hints_text);
}

/*
 * pg_create_outline - Create a new outline
 *
 * Parameters:
 *   outline_name: Name of the outline
 *   query_text: SQL query text
 *   hints_text: Hint string in pg_hint_plan format or OceanBase/Oracle style outline data
 *
 * The hints_text can be in either format:
 * 1. Plain format: "SeqScan(table1) IndexScan(table2 idx)"
 * 2. OceanBase format: "/ *+ BEGIN_OUTLINE_DATA\nSeqScan(table1)\nIndexScan(table2 idx)\nEND_OUTLINE_DATA * /"
 */
Datum
pg_create_outline(PG_FUNCTION_ARGS)
{
	char	   *outline_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *query_text = text_to_cstring(PG_GETARG_TEXT_PP(1));
	char	   *hints_text_raw = text_to_cstring(PG_GETARG_TEXT_PP(2));
	char	   *hints_text;
	Oid			outline_oid;
	Relation	rel;
	HeapTuple	tuple;
	Datum		values[Natts_pg_outline];
	bool		nulls[Natts_pg_outline];
	Oid			nspid;

	/* Check permissions */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create outlines")));

	/* Extract hints from OceanBase/Oracle format if present */
	hints_text = extract_hints_from_outline_data(hints_text_raw);

	/* Get current namespace */
	nspid = get_namespace_oid("public", false);

	/* Open the relation */
	rel = table_open(OutlineRelationId, RowExclusiveLock);

	/* Check if outline already exists */
	tuple = SearchSysCache2(OUTLINENAMENSP,
							PointerGetDatum(outline_name),
							ObjectIdGetDatum(nspid));
	if (HeapTupleIsValid(tuple))
	{
		ReleaseSysCache(tuple);
		table_close(rel, RowExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("outline \"%s\" already exists", outline_name)));
	}

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
	values[Anum_pg_outline_outlinequery - 1] = CStringGetTextDatum(query_text);
	values[Anum_pg_outline_outlinehints - 1] = CStringGetTextDatum(hints_text);

	/* Insert tuple */
	tuple = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tuple);

	/* Close relation */
	table_close(rel, RowExclusiveLock);

	/* Return the OID */
	PG_RETURN_OID(outline_oid);
}

/*
 * pg_drop_outline - Drop an existing outline
 *
 * Parameters:
 *   outline_name: Name of the outline to drop
 */
Datum
pg_drop_outline(PG_FUNCTION_ARGS)
{
	char	   *outline_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Relation	rel;
	HeapTuple	tuple;
	Oid			nspid;
	Form_pg_outline outlineForm;

	/* Check permissions */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to drop outlines")));

	/* Get current namespace */
	nspid = get_namespace_oid("public", false);

	/* Find the outline */
	tuple = SearchSysCache2(OUTLINENAMENSP,
							PointerGetDatum(outline_name),
							ObjectIdGetDatum(nspid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("outline \"%s\" does not exist", outline_name)));

	outlineForm = (Form_pg_outline) GETSTRUCT(tuple);

	/* Open the relation */
	rel = table_open(OutlineRelationId, RowExclusiveLock);

	/* Delete the tuple */
	CatalogTupleDelete(rel, &tuple->t_self);

	/* Release syscache and close relation */
	ReleaseSysCache(tuple);
	table_close(rel, RowExclusiveLock);

	PG_RETURN_VOID();
}

/*
 * pg_enable_outline - Enable an outline
 *
 * Parameters:
 *   outline_name: Name of the outline to enable
 */
Datum
pg_enable_outline(PG_FUNCTION_ARGS)
{
	char	   *outline_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Relation	rel;
	HeapTuple	oldtuple;
	HeapTuple	newtuple;
	Datum		values[Natts_pg_outline];
	bool		nulls[Natts_pg_outline];
	bool		replaces[Natts_pg_outline];
	Oid			nspid;

	/* Check permissions */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to enable outlines")));

	/* Get current namespace */
	nspid = get_namespace_oid("public", false);

	/* Find the outline */
	oldtuple = SearchSysCache2(OUTLINENAMENSP,
							  PointerGetDatum(outline_name),
							  ObjectIdGetDatum(nspid));
	if (!HeapTupleIsValid(oldtuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("outline \"%s\" does not exist", outline_name)));

	/* Open the relation */
	rel = table_open(OutlineRelationId, RowExclusiveLock);

	/* Update the enabled flag */
	MemSet(nulls, false, sizeof(nulls));
	MemSet(replaces, false, sizeof(replaces));

	values[Anum_pg_outline_outlineenabled - 1] = BoolGetDatum(true);
	replaces[Anum_pg_outline_outlineenabled - 1] = true;

	newtuple = heap_modify_tuple(oldtuple, RelationGetDescr(rel),
								 values, nulls, replaces);
	CatalogTupleUpdate(rel, &newtuple->t_self, newtuple);

	/* Release syscache and close relation */
	ReleaseSysCache(oldtuple);
	table_close(rel, RowExclusiveLock);

	PG_RETURN_VOID();
}

/*
 * pg_disable_outline - Disable an outline
 *
 * Parameters:
 *   outline_name: Name of the outline to disable
 */
Datum
pg_disable_outline(PG_FUNCTION_ARGS)
{
	char	   *outline_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Relation	rel;
	HeapTuple	oldtuple;
	HeapTuple	newtuple;
	Datum		values[Natts_pg_outline];
	bool		nulls[Natts_pg_outline];
	bool		replaces[Natts_pg_outline];
	Oid			nspid;

	/* Check permissions */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to disable outlines")));

	/* Get current namespace */
	nspid = get_namespace_oid("public", false);

	/* Find the outline */
	oldtuple = SearchSysCache2(OUTLINENAMENSP,
							  PointerGetDatum(outline_name),
							  ObjectIdGetDatum(nspid));
	if (!HeapTupleIsValid(oldtuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("outline \"%s\" does not exist", outline_name)));

	/* Open the relation */
	rel = table_open(OutlineRelationId, RowExclusiveLock);

	/* Update the enabled flag */
	MemSet(nulls, false, sizeof(nulls));
	MemSet(replaces, false, sizeof(replaces));

	values[Anum_pg_outline_outlineenabled - 1] = BoolGetDatum(false);
	replaces[Anum_pg_outline_outlineenabled - 1] = true;

	newtuple = heap_modify_tuple(oldtuple, RelationGetDescr(rel),
								 values, nulls, replaces);
	CatalogTupleUpdate(rel, &newtuple->t_self, newtuple);

	/* Release syscache and close relation */
	ReleaseSysCache(oldtuple);
	table_close(rel, RowExclusiveLock);

	PG_RETURN_VOID();
}
