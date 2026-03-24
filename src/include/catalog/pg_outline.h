/*-------------------------------------------------------------------------
 *
 * pg_outline.h
 *	  definition of the "outline" system catalog (pg_outline)
 *
 * An outline is a stored set of optimizer hints that can be applied to
 * SQL queries to stabilize execution plans. This is similar to Oracle's
 * SQL Outline feature and OceanBase's outline feature.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_outline.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_OUTLINE_H
#define PG_OUTLINE_H

#include "catalog/genbki.h"
#include "catalog/pg_outline_d.h"

/* ----------------
 *		pg_outline definition.  cpp turns this into
 *		typedef struct FormData_pg_outline
 * ----------------
 */
CATALOG(pg_outline,9900,OutlineRelationId)
{
	Oid			oid;			/* oid */

	/* outline name */
	NameData	outlinename;

	/* OID of namespace containing this outline */
	Oid			outlinenamespace BKI_DEFAULT(PGNSP);

	/* outline owner */
	Oid			outlineowner BKI_DEFAULT(PGUID);

	/* is outline enabled? */
	bool		outlineenabled BKI_DEFAULT(t);

#ifdef CATALOG_VARLEN
	/* normalized SQL query text (query signature) */
	text		outlinequery BKI_FORCE_NOT_NULL;

	/* hint string (pg_hint_plan format) */
	text		outlinehints BKI_FORCE_NOT_NULL;
#endif
} FormData_pg_outline;

/* ----------------
 *		Form_pg_outline corresponds to a pointer to a tuple with
 *		the format of pg_outline relation.
 * ----------------
 */
typedef FormData_pg_outline *Form_pg_outline;

#endif							/* PG_OUTLINE_H */
