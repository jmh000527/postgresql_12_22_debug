#ifndef PG_OUTLINE_H
#define PG_OUTLINE_H

#include "nodes/parsenodes.h"
#include "parser/parse_node.h"

/* Return a hint string for the given Query if an outline exists. */
extern const char *pg_outline_get_hint(Query *query);

/* Expose query-name lookup so callers can log/debug if needed. */
extern const char *pg_outline_get_query_name(Query *query);

extern void pg_outline_post_parse(ParseState *pstate, Query *query);
extern void pg_outline_init_hooks(void);
extern void pg_outline_fini_hooks(void);

#endif /* PG_OUTLINE_H */
