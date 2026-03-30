/*-------------------------------------------------------------------------
 *
 * normalize_query.h
 *		Lightweight query normalization helpers (very small subset of
 *		pg_stat_statements).
 *
 * Portions Copyright (c) 2008-2020, PostgreSQL Global Development Group
 */
#ifndef NORMALIZE_QUERY_H
#define NORMALIZE_QUERY_H

#include "nodes/parsenodes.h"

typedef struct pgssLocationLen
{
	int			location;
	int			length;
} pgssLocationLen;

typedef struct pgssJumbleState
{
	unsigned char *jumble;
	Size		jumble_len;
	pgssLocationLen *clocations;
	int			clocations_buf_size;
	int			clocations_count;
	int			highest_extern_param_id;
} pgssJumbleState;

#define JUMBLE_SIZE		1024

extern char *generate_normalized_query(pgssJumbleState *jstate,
									   const char *query,
									   int query_loc, int *query_len_p,
									   int encoding);
extern void JumbleQuery(pgssJumbleState *jstate, Query *query);

#endif	/* NORMALIZE_QUERY_H */
