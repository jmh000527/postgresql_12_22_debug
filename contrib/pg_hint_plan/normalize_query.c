#define PG_HINT_PLAN_NORMALIZE_API
#include "postgres.h"

#include "mb/pg_wchar.h"
#include "lib/stringinfo.h"
#include "nodes/nodeFuncs.h"
#include "parser/scansup.h"

#include "normalize_query.h"

/*
 * A very small and permissive "normalizer": collapse whitespace and strip
 * trailing semicolons. It keeps signatures stable enough for matching while
 * avoiding the full pg_stat_statements jumble dependency.
 */
char *
generate_normalized_query(pgssJumbleState *jstate, const char *query,
						  int query_loc, int *query_len_p, int encoding)
{
	StringInfoData buf;
	const char *p = query;
	bool		in_space = false;

	(void) jstate;		/* unused in this lightweight version */
	(void) query_loc;
	(void) encoding;

	initStringInfo(&buf);

	while (*p)
	{
		if (isspace((unsigned char) *p))
		{
			if (!in_space)
				appendStringInfoChar(&buf, ' ');
			in_space = true;
		}
		else
		{
			in_space = false;
			appendStringInfoChar(&buf, *p);
		}
		p++;
	}

	/* trim possible trailing semicolon and spaces */
	while (buf.len > 0 &&
		   (buf.data[buf.len - 1] == ';' || isspace((unsigned char) buf.data[buf.len - 1])))
		buf.data[--buf.len] = '\0';

	*query_len_p = buf.len + 1;

	return buf.data;
}

/*
 * Dummy implementation. We don't jumble in this simplified normalizer, but
 * keep the signature to satisfy callers.
 */
void
JumbleQuery(pgssJumbleState *jstate, Query *query)
{
	(void) jstate;
	(void) query;
}
