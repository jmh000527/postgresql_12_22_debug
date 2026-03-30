/*-------------------------------------------------------------------------
 *
 * pg_outline.c
 *    PostgreSQL extension for plan hint management using outlines
 *
 * Based on OceanBase outline functionality, this extension provides:
 * - Persistent storage of query hints
 * - Automatic hint application during planning
 * - Inline hint support with comment syntax
 * - Auto-generation of outlines from execution plans
 * - Per-query hints for complex queries (CTEs, subqueries, SubLinks)
 *
 * Uses pg_hint_plan syntax for hints:
 * - Scan method hints: SeqScan, IndexScan, NoSeqScan, NoIndexScan, etc.
 * - Join method hints: NestLoop, HashJoin, MergeJoin, etc.
 * - Join order hints: Leading(t1 t2 t3)
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "optimizer/planner.h"
#include "parser/analyze.h"
#include "parser/parser.h"
#include "parser/scansup.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

PG_MODULE_MAGIC;

/*---- Module configuration ----*/
static bool outline_enabled = true;
static bool outline_auto_generate = false;
static bool outline_display_hints = false;
static int outline_log_level = NOTICE;
static char *outline_match_mode = "exact";

/*---- Planner hooks ----*/
static planner_hook_type prev_planner_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart_hook = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd_hook = NULL;
static ProcessUtility_hook_type prev_ProcessUtility_hook = NULL;

/*---- Forward declarations ----*/
static PlannedStmt *outline_planner(Query *parse, int cursorOptions, ParamListInfo boundParams);
static void outline_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void outline_ExecutorEnd(QueryDesc *queryDesc);
static void outline_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
                                   ProcessUtilityContext context, ParamListInfo params,
                                   QueryEnvironment *queryEnv, DestReceiver *dest,
                                   char *completionTag);

/*---- Helper functions ----*/
static char *extract_inline_hints(const char *query_text);
static char *normalize_query(const char *query_text);
static char *lookup_outline_hints(const char *query_text, const char *schema_name);
static void parse_and_apply_hints(Query *query, const char *hints);
static char *inject_hints_into_sql(const char *query_text, const char *hints);

/*
 * Module load callback
 */
void
_PG_init(void)
{
    /* Define custom GUC variables */
    DefineCustomBoolVariable("pg_outline.enabled",
                             "Enable outline functionality",
                             NULL,
                             &outline_enabled,
                             true,
                             PGC_USERSET,
                             0,
                             NULL,
                             NULL,
                             NULL);

    DefineCustomBoolVariable("pg_outline.auto_generate",
                             "Automatically generate outlines from execution plans",
                             NULL,
                             &outline_auto_generate,
                             false,
                             PGC_USERSET,
                             0,
                             NULL,
                             NULL,
                             NULL);

    DefineCustomBoolVariable("pg_outline.display_hints",
                             "Display applied hints in EXPLAIN output",
                             NULL,
                             &outline_display_hints,
                             false,
                             PGC_USERSET,
                             0,
                             NULL,
                             NULL,
                             NULL);

    DefineCustomEnumVariable("pg_outline.log_level",
                             "Logging level for outline operations",
                             NULL,
                             &outline_log_level,
                             NOTICE,
                             (const struct config_enum_entry[]) {
                                 {"debug", DEBUG1, false},
                                 {"notice", NOTICE, false},
                                 {"warning", WARNING, false},
                                 {"error", ERROR, false},
                                 {NULL, 0, false}
                             },
                             PGC_USERSET,
                             0,
                             NULL,
                             NULL,
                             NULL);

    DefineCustomStringVariable("pg_outline.match_mode",
                               "Query matching mode: exact, normalized, fingerprint",
                               NULL,
                               &outline_match_mode,
                               "exact",
                               PGC_USERSET,
                               0,
                               NULL,
                               NULL,
                               NULL);

    /* Install hooks */
    prev_planner_hook = planner_hook;
    planner_hook = outline_planner;

    prev_ExecutorStart_hook = ExecutorStart_hook;
    ExecutorStart_hook = outline_ExecutorStart;

    prev_ExecutorEnd_hook = ExecutorEnd_hook;
    ExecutorEnd_hook = outline_ExecutorEnd;

    prev_ProcessUtility_hook = ProcessUtility_hook;
    ProcessUtility_hook = outline_ProcessUtility;

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
    ExecutorStart_hook = prev_ExecutorStart_hook;
    ExecutorEnd_hook = prev_ExecutorEnd_hook;
    ProcessUtility_hook = prev_ProcessUtility_hook;

    elog(LOG, "pg_outline extension unloaded");
}

/*
 * Planner hook - main entry point for hint application
 */
static PlannedStmt *
outline_planner(Query *parse, int cursorOptions, ParamListInfo boundParams)
{
    PlannedStmt *result;
    char *hints = NULL;
    const char *query_text;
    const char *schema_name;

    /* Skip if outline is disabled */
    if (!outline_enabled)
    {
        if (prev_planner_hook)
            return (*prev_planner_hook)(parse, cursorOptions, boundParams);
        else
            return standard_planner(parse, cursorOptions, boundParams);
    }

    /* Get current query text from debug_query_string */
    query_text = debug_query_string;
    if (!query_text)
        query_text = "";

    /* Get current schema */
    schema_name = get_namespace_name(get_namespace_oid("public", true));

    /* Step 1: Extract inline hints from query text */
    hints = extract_inline_hints(query_text);

    /* Step 2: If no inline hints, lookup stored outline hints */
    if (!hints || strlen(hints) == 0)
    {
        hints = lookup_outline_hints(query_text, schema_name);
    }

    /* Step 3: Apply hints to the query if found */
    if (hints && strlen(hints) > 0)
    {
        ereport(outline_log_level,
                (errmsg("pg_outline: applying hints: %s", hints)));

        parse_and_apply_hints(parse, hints);
    }

    /* Call the next planner hook or standard planner */
    if (prev_planner_hook)
        result = (*prev_planner_hook)(parse, cursorOptions, boundParams);
    else
        result = standard_planner(parse, cursorOptions, boundParams);

    return result;
}

/*
 * Extract inline hints from query text
 * Supports hint comments in the format: comment-start plus hint1 hint2 ... comment-end
 */
static char *
extract_inline_hints(const char *query_text)
{
    const char *start, *end;
    StringInfoData hints_buf;

    if (!query_text)
        return NULL;

    /* Look for hint pattern: comment-start-plus ... comment-end */
    start = strstr(query_text, "/*+");
    if (!start)
        return NULL;

    start += 3; /* Skip the opening pattern */
    end = strstr(start, "*/");
    if (!end)
        return NULL;

    initStringInfo(&hints_buf);
    appendBinaryStringInfo(&hints_buf, start, end - start);

    return hints_buf.data;
}

/*
 * Normalize query text for matching
 * Removes whitespace, converts to lowercase
 */
static char *
normalize_query(const char *query_text)
{
    StringInfoData result;
    const char *ptr;
    bool in_whitespace = false;

    if (!query_text)
        return NULL;

    initStringInfo(&result);

    for (ptr = query_text; *ptr; ptr++)
    {
        if (isspace((unsigned char) *ptr))
        {
            if (!in_whitespace)
            {
                appendStringInfoChar(&result, ' ');
                in_whitespace = true;
            }
        }
        else
        {
            appendStringInfoChar(&result, tolower((unsigned char) *ptr));
            in_whitespace = false;
        }
    }

    return result.data;
}

/*
 * Lookup outline hints from the catalog table
 */
static char *
lookup_outline_hints(const char *query_text, const char *schema_name)
{
    StringInfoData sql;
    int ret;
    bool isnull;
    char *hints = NULL;
    char *normalized_query;

    if (!query_text || strlen(query_text) == 0)
        return NULL;

    /* Normalize query based on match mode */
    if (strcmp(outline_match_mode, "normalized") == 0)
    {
        normalized_query = normalize_query(query_text);
    }
    else
    {
        normalized_query = pstrdup(query_text);
    }

    /* Query the outlines table */
    initStringInfo(&sql);
    appendStringInfo(&sql,
        "SELECT hints FROM pg_outline.outlines "
        "WHERE enabled = true "
        "AND schema_name = %s "
        "AND query_text = %s "
        "LIMIT 1",
        quote_literal_cstr(schema_name ? schema_name : "public"),
        quote_literal_cstr(normalized_query));

    /* Execute query using SPI */
    if (SPI_connect() != SPI_OK_CONNECT)
        elog(ERROR, "pg_outline: SPI_connect failed");

    ret = SPI_execute(sql.data, true, 1);

    if (ret == SPI_OK_SELECT && SPI_processed > 0)
    {
        TupleDesc tupdesc = SPI_tuptable->tupdesc;
        HeapTuple tuple = SPI_tuptable->vals[0];
        Datum hints_datum;

        hints_datum = SPI_getbinval(tuple, tupdesc, 1, &isnull);
        if (!isnull)
        {
            hints = TextDatumGetCString(hints_datum);
        }
    }

    SPI_finish();

    pfree(normalized_query);
    pfree(sql.data);

    return hints;
}

/*
 * Parse and apply hints to query
 * This is a simplified implementation - full implementation would
 * integrate with pg_hint_plan's parser
 */
static void
parse_and_apply_hints(Query *query, const char *hints)
{
    /* For now, just log the hints
     * Full implementation would parse hints and set appropriate
     * planner parameters and hooks
     */
    ereport(outline_log_level,
            (errmsg("pg_outline: parsed hints for query"),
             errdetail("Hints: %s", hints)));

    /* TODO: Integrate with pg_hint_plan parser to actually apply hints */
}

/*
 * ExecutorStart hook
 */
static void
outline_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
    /* Call previous hook or standard function */
    if (prev_ExecutorStart_hook)
        (*prev_ExecutorStart_hook)(queryDesc, eflags);
    else
        standard_ExecutorStart(queryDesc, eflags);
}

/*
 * ExecutorEnd hook
 */
static void
outline_ExecutorEnd(QueryDesc *queryDesc)
{
    /* Auto-generate outline if enabled */
    if (outline_auto_generate && outline_enabled)
    {
        /* TODO: Generate outline from queryDesc->plannedstmt */
    }

    /* Call previous hook or standard function */
    if (prev_ExecutorEnd_hook)
        (*prev_ExecutorEnd_hook)(queryDesc);
    else
        standard_ExecutorEnd(queryDesc);
}

/*
 * ProcessUtility hook
 */
static void
outline_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
                       ProcessUtilityContext context, ParamListInfo params,
                       QueryEnvironment *queryEnv, DestReceiver *dest,
                       char *completionTag)
{
    /* Call previous hook or standard function */
    if (prev_ProcessUtility_hook)
        (*prev_ProcessUtility_hook)(pstmt, queryString, context, params,
                                    queryEnv, dest, completionTag);
    else
        standard_ProcessUtility(pstmt, queryString, context, params,
                                queryEnv, dest, completionTag);
}

/*
 * Helper: Extract hints for a specific query name from hint string
 */
static char *
extract_hints_for_query(const char *hints, const char *query_name)
{
    StringInfoData result;
    const char *ptr = hints;
    bool found_match = false;

    initStringInfo(&result);

    while (*ptr)
    {
        /* Skip whitespace */
        while (*ptr && isspace((unsigned char) *ptr))
            ptr++;

        if (*ptr == '\0')
            break;

        /* Check for [query_name] prefix */
        if (*ptr == '[')
        {
            const char *bracket_end = strchr(ptr, ']');
            if (bracket_end)
            {
                char *curr_query_name = pnstrdup(ptr + 1, bracket_end - ptr - 1);

                /* Check if this matches our target query name */
                if (strcmp(curr_query_name, query_name) == 0)
                {
                    /* Found matching query name, extract hints */
                    ptr = bracket_end + 1;

                    /* Copy hints until next bracket or end */
                    while (*ptr && *ptr != '[')
                    {
                        if (!isspace((unsigned char) *ptr) ||
                            (result.len > 0 && result.data[result.len - 1] != ' '))
                        {
                            appendStringInfoChar(&result, *ptr);
                        }
                        ptr++;
                    }
                    found_match = true;
                }
                else
                {
                    /* Not our query, skip this section */
                    ptr = bracket_end + 1;
                    while (*ptr && *ptr != '[')
                        ptr++;
                }

                pfree(curr_query_name);
            }
            else
            {
                ptr++;
            }
        }
        else
        {
            /* No prefix - if looking for "main", use these hints */
            if (strcmp(query_name, "main") == 0 && !found_match)
            {
                while (*ptr && *ptr != '[')
                {
                    if (!isspace((unsigned char) *ptr) ||
                        (result.len > 0 && result.data[result.len - 1] != ' '))
                    {
                        appendStringInfoChar(&result, *ptr);
                    }
                    ptr++;
                }
                found_match = true;
            }
            else
            {
                /* Skip unprefixed hints if not looking for main */
                while (*ptr && *ptr != '[')
                    ptr++;
            }
        }
    }

    /* Trim trailing whitespace */
    while (result.len > 0 && isspace((unsigned char) result.data[result.len - 1]))
        result.data[--result.len] = '\0';

    return result.data;
}

/*
 * Inject hints into SQL text
 * Supports multi-query hints with [query_name] prefixes
 *
 * Format of hints:
 * - Simple: "SeqScan(t1) HashJoin(t1 t2)"
 * - With prefixes: "[main]SeqScan(t1) [cte_data]IndexScan(t2)"
 *
 * This function parses the SQL and inserts hint comments at the appropriate locations.
 */
static char *
inject_hints_into_sql(const char *query_text, const char *hints)
{
    StringInfoData result;
    const char *ptr;
    char *main_hints;
    int cte_index = 0;
    int paren_depth = 0;

    if (!query_text || !hints)
        return pstrdup(query_text ? query_text : "");

    initStringInfo(&result);

    /* Extract hints for main query */
    main_hints = extract_hints_for_query(hints, "main");

    ptr = query_text;

    /* Skip leading whitespace and comments */
    while (*ptr && (isspace((unsigned char) *ptr) || *ptr == '-' || *ptr == '/'))
    {
        if (*ptr == '-' && *(ptr + 1) == '-')
        {
            /* Skip line comment */
            while (*ptr && *ptr != '\n')
            {
                appendStringInfoChar(&result, *ptr);
                ptr++;
            }
        }
        else if (*ptr == '/' && *(ptr + 1) == '*')
        {
            /* Skip block comment */
            appendStringInfoChar(&result, *ptr++);
            appendStringInfoChar(&result, *ptr++);
            while (*ptr && !(*ptr == '*' && *(ptr + 1) == '/'))
            {
                appendStringInfoChar(&result, *ptr);
                ptr++;
            }
            if (*ptr == '*')
            {
                appendStringInfoChar(&result, *ptr++);
                appendStringInfoChar(&result, *ptr++);
            }
        }
        else
        {
            appendStringInfoChar(&result, *ptr);
            ptr++;
        }
    }

    /* Check if this is a WITH clause (CTE) */
    if (strncasecmp(ptr, "WITH", 4) == 0 && isspace((unsigned char) ptr[4]))
    {
        /* Copy "WITH " */
        appendStringInfoString(&result, "WITH ");
        ptr += 4;
        while (*ptr && isspace((unsigned char) *ptr))
        {
            appendStringInfoChar(&result, *ptr);
            ptr++;
        }

        /* Parse CTEs and inject hints */
        while (*ptr)
        {
            /* Skip whitespace */
            while (*ptr && isspace((unsigned char) *ptr))
            {
                appendStringInfoChar(&result, *ptr);
                ptr++;
            }

            if (*ptr == '\0')
                break;

            /* Check if this starts a CTE name */
            if (isalpha((unsigned char) *ptr) || *ptr == '_')
            {
                /* Extract CTE name */
                const char *name_start = ptr;
                while (*ptr && (isalnum((unsigned char) *ptr) || *ptr == '_'))
                {
                    appendStringInfoChar(&result, *ptr);
                    ptr++;
                }

                /* Save CTE name for hint lookup */
                char *cte_name = pnstrdup(name_start, ptr - name_start);
                char *cte_query_name = psprintf("cte_%s", cte_name);
                char *cte_hints = extract_hints_for_query(hints, cte_query_name);

                /* Skip whitespace and expect AS */
                while (*ptr && isspace((unsigned char) *ptr))
                {
                    appendStringInfoChar(&result, *ptr);
                    ptr++;
                }

                if (strncasecmp(ptr, "AS", 2) == 0)
                {
                    appendStringInfoString(&result, "AS");
                    ptr += 2;

                    /* Skip whitespace */
                    while (*ptr && isspace((unsigned char) *ptr))
                    {
                        appendStringInfoChar(&result, *ptr);
                        ptr++;
                    }

                    /* Expect '(' */
                    if (*ptr == '(')
                    {
                        appendStringInfoChar(&result, *ptr);
                        ptr++;
                        paren_depth = 1;

                        /* Skip whitespace */
                        while (*ptr && isspace((unsigned char) *ptr))
                        {
                            appendStringInfoChar(&result, *ptr);
                            ptr++;
                        }

                        /* Look for SELECT to inject hints */
                        if (strncasecmp(ptr, "SELECT", 6) == 0)
                        {
                            if (cte_hints && strlen(cte_hints) > 0)
                            {
                                appendStringInfoString(&result, "SELECT /*+ ");
                                appendStringInfoString(&result, cte_hints);
                                appendStringInfoString(&result, " */ ");
                                ptr += 6;
                            }
                            else
                            {
                                appendStringInfoString(&result, "SELECT");
                                ptr += 6;
                            }
                        }

                        /* Copy rest of CTE until closing paren */
                        while (*ptr && paren_depth > 0)
                        {
                            if (*ptr == '(')
                                paren_depth++;
                            else if (*ptr == ')')
                                paren_depth--;

                            appendStringInfoChar(&result, *ptr);
                            ptr++;
                        }
                    }
                }

                pfree(cte_name);
                pfree(cte_query_name);
                pfree(cte_hints);
            }

            /* Skip whitespace */
            while (*ptr && isspace((unsigned char) *ptr))
            {
                appendStringInfoChar(&result, *ptr);
                ptr++;
            }

            /* Check for comma (more CTEs) or main query */
            if (*ptr == ',')
            {
                appendStringInfoChar(&result, *ptr);
                ptr++;
            }
            else
            {
                /* End of CTEs, main query follows */
                break;
            }
        }

        /* Now handle main SELECT after CTEs */
        while (*ptr && isspace((unsigned char) *ptr))
        {
            appendStringInfoChar(&result, *ptr);
            ptr++;
        }

        if (strncasecmp(ptr, "SELECT", 6) == 0)
        {
            if (main_hints && strlen(main_hints) > 0)
            {
                appendStringInfoString(&result, "SELECT /*+ ");
                appendStringInfoString(&result, main_hints);
                appendStringInfoString(&result, " */ ");
                ptr += 6;
            }
            else
            {
                appendStringInfoString(&result, "SELECT");
                ptr += 6;
            }
        }

        /* Copy rest of query */
        appendStringInfoString(&result, ptr);
    }
    else if (strncasecmp(ptr, "SELECT", 6) == 0)
    {
        /* Simple SELECT without WITH clause */
        if (main_hints && strlen(main_hints) > 0)
        {
            appendStringInfoString(&result, "SELECT /*+ ");
            appendStringInfoString(&result, main_hints);
            appendStringInfoString(&result, " */ ");
            ptr += 6;
        }
        else
        {
            appendStringInfoString(&result, "SELECT");
            ptr += 6;
        }

        /* Copy rest of query */
        appendStringInfoString(&result, ptr);
    }
    else
    {
        /* Not a query we can handle, just return original */
        appendStringInfoString(&result, ptr);
    }

    pfree(main_hints);

    return result.data;
}

/*
 * SQL-callable functions
 */

/* Create outline from SQL and hints */
PG_FUNCTION_INFO_V1(pg_outline_create);
Datum
pg_outline_create(PG_FUNCTION_ARGS)
{
    text *name_text = PG_GETARG_TEXT_PP(0);
    text *query_text = PG_GETARG_TEXT_PP(1);
    text *hints_text = PG_ARGISNULL(2) ? NULL : PG_GETARG_TEXT_PP(2);
    text *schema_text = PG_ARGISNULL(3) ? NULL : PG_GETARG_TEXT_PP(3);
    text *desc_text = PG_ARGISNULL(4) ? NULL : PG_GETARG_TEXT_PP(4);
    bool enabled = PG_ARGISNULL(5) ? true : PG_GETARG_BOOL(5);

    char *name = text_to_cstring(name_text);
    char *query = text_to_cstring(query_text);
    char *hints = hints_text ? text_to_cstring(hints_text) : NULL;
    char *schema = schema_text ? text_to_cstring(schema_text) : "public";
    char *description = desc_text ? text_to_cstring(desc_text) : NULL;

    StringInfoData sql;
    int ret;
    int outline_id = -1;

    /* Insert into outlines table */
    if (SPI_connect() != SPI_OK_CONNECT)
        elog(ERROR, "pg_outline_create: SPI_connect failed");

    initStringInfo(&sql);
    appendStringInfo(&sql,
        "INSERT INTO pg_outline.outlines (name, schema_name, query_text, hints, enabled, description) "
        "VALUES (%s, %s, %s, %s, %s, %s) "
        "RETURNING id",
        quote_literal_cstr(name),
        quote_literal_cstr(schema),
        quote_literal_cstr(query),
        hints ? quote_literal_cstr(hints) : "NULL",
        enabled ? "true" : "false",
        description ? quote_literal_cstr(description) : "NULL");

    ret = SPI_execute(sql.data, false, 0);

    if (ret == SPI_OK_INSERT_RETURNING && SPI_processed > 0)
    {
        bool isnull;
        Datum id_datum = SPI_getbinval(SPI_tuptable->vals[0],
                                       SPI_tuptable->tupdesc, 1, &isnull);
        if (!isnull)
            outline_id = DatumGetInt32(id_datum);
    }

    SPI_finish();
    pfree(sql.data);

    if (outline_id < 0)
        elog(ERROR, "pg_outline_create: failed to create outline");

    ereport(NOTICE,
            (errmsg("pg_outline: created outline '%s' with id %d", name, outline_id)));

    PG_RETURN_INT32(outline_id);
}

/* Drop outline by name */
PG_FUNCTION_INFO_V1(pg_outline_drop);
Datum
pg_outline_drop(PG_FUNCTION_ARGS)
{
    text *name_text = PG_GETARG_TEXT_PP(0);
    text *schema_text = PG_ARGISNULL(1) ? NULL : PG_GETARG_TEXT_PP(1);

    char *name = text_to_cstring(name_text);
    char *schema = schema_text ? text_to_cstring(schema_text) : "public";

    StringInfoData sql;
    int ret;

    if (SPI_connect() != SPI_OK_CONNECT)
        elog(ERROR, "pg_outline_drop: SPI_connect failed");

    initStringInfo(&sql);
    appendStringInfo(&sql,
        "DELETE FROM pg_outline.outlines "
        "WHERE name = %s AND schema_name = %s",
        quote_literal_cstr(name),
        quote_literal_cstr(schema));

    ret = SPI_execute(sql.data, false, 0);

    SPI_finish();
    pfree(sql.data);

    if (ret != SPI_OK_DELETE)
        elog(ERROR, "pg_outline_drop: failed to drop outline");

    PG_RETURN_BOOL(true);
}

/* Enable outline */
PG_FUNCTION_INFO_V1(pg_outline_enable);
Datum
pg_outline_enable(PG_FUNCTION_ARGS)
{
    text *name_text = PG_GETARG_TEXT_PP(0);
    text *schema_text = PG_ARGISNULL(1) ? NULL : PG_GETARG_TEXT_PP(1);

    char *name = text_to_cstring(name_text);
    char *schema = schema_text ? text_to_cstring(schema_text) : "public";

    StringInfoData sql;
    int ret;

    if (SPI_connect() != SPI_OK_CONNECT)
        elog(ERROR, "pg_outline_enable: SPI_connect failed");

    initStringInfo(&sql);
    appendStringInfo(&sql,
        "UPDATE pg_outline.outlines "
        "SET enabled = true, updated_at = CURRENT_TIMESTAMP "
        "WHERE name = %s AND schema_name = %s",
        quote_literal_cstr(name),
        quote_literal_cstr(schema));

    ret = SPI_execute(sql.data, false, 0);

    SPI_finish();
    pfree(sql.data);

    PG_RETURN_BOOL(ret == SPI_OK_UPDATE);
}

/* Disable outline */
PG_FUNCTION_INFO_V1(pg_outline_disable);
Datum
pg_outline_disable(PG_FUNCTION_ARGS)
{
    text *name_text = PG_GETARG_TEXT_PP(0);
    text *schema_text = PG_ARGISNULL(1) ? NULL : PG_GETARG_TEXT_PP(1);

    char *name = text_to_cstring(name_text);
    char *schema = schema_text ? text_to_cstring(schema_text) : "public";

    StringInfoData sql;
    int ret;

    if (SPI_connect() != SPI_OK_CONNECT)
        elog(ERROR, "pg_outline_disable: SPI_connect failed");

    initStringInfo(&sql);
    appendStringInfo(&sql,
        "UPDATE pg_outline.outlines "
        "SET enabled = false, updated_at = CURRENT_TIMESTAMP "
        "WHERE name = %s AND schema_name = %s",
        quote_literal_cstr(name),
        quote_literal_cstr(schema));

    ret = SPI_execute(sql.data, false, 0);

    SPI_finish();
    pfree(sql.data);

    PG_RETURN_BOOL(ret == SPI_OK_UPDATE);
}

/* Create outline from current plan - stub for now */
PG_FUNCTION_INFO_V1(pg_outline_create_from_plan);
Datum
pg_outline_create_from_plan(PG_FUNCTION_ARGS)
{
    /* TODO: Implement auto-generation from execution plan */
    elog(ERROR, "pg_outline_create_from_plan: not yet implemented");
    PG_RETURN_INT32(-1);
}

/* Match query and return hints - stub for now */
PG_FUNCTION_INFO_V1(pg_outline_match_query);
Datum
pg_outline_match_query(PG_FUNCTION_ARGS)
{
    /* TODO: Implement query matching */
    elog(ERROR, "pg_outline_match_query: not yet implemented");
    PG_RETURN_NULL();
}

/* Inject hints into SQL text and return the modified SQL */
PG_FUNCTION_INFO_V1(pg_outline_inject_hints);
Datum
pg_outline_inject_hints(PG_FUNCTION_ARGS)
{
    text *query_text = PG_GETARG_TEXT_PP(0);
    text *hints_text = PG_GETARG_TEXT_PP(1);

    char *query = text_to_cstring(query_text);
    char *hints = text_to_cstring(hints_text);
    char *result;

    /* Inject hints into SQL */
    result = inject_hints_into_sql(query, hints);

    PG_RETURN_TEXT_P(cstring_to_text(result));
}
