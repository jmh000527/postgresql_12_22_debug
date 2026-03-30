# SQL Hint Injection Feature

## Overview

This document describes the SQL hint injection functionality implemented for the `pg_outline` extension. This feature allows stored outline hints to be correctly inserted back into SQL queries as inline `/*+ ... */` comments.

## Implementation Date

- **Completed**: 2026-03-30
- **Branch**: claude/implement-outline-functionality
- **Commits**: d645959c7b, 21f1561ce2

## Purpose

The hint injection feature serves several important purposes:

1. **Visualization**: Show users which hints would be applied to their queries
2. **Debugging**: Help debug hint application by seeing the final SQL
3. **Export**: Generate SQL with hints for external tools or documentation
4. **Verification**: Verify that hints are correctly parsed and targeted

## Core Functionality

### Function: `pg_outline.inject_hints(query_text, hints)`

**Parameters:**
- `query_text` (TEXT): The SQL query to inject hints into
- `hints` (TEXT): The hints string with optional `[query_name]` prefixes

**Returns:** TEXT - The modified SQL with hints inserted as `/*+ ... */` comments

### Multi-Query Hint Syntax

The implementation supports OceanBase-style multi-query hints with `[query_name]` prefixes:

```
[main]Hint1(args) [cte_name]Hint2(args) [subquery_1]Hint3(args)
```

**Query Name Format:**
- `[main]` - Main/outer query
- `[cte_<name>]` - Named CTE (e.g., `[cte_active_users]`)
- `[subquery_N]` - Numbered subqueries (future enhancement)
- No prefix - Defaults to main query

## Implementation Details

### Code Structure

The implementation consists of two main functions in `contrib/pg_outline/pg_outline.c`:

#### 1. `extract_hints_for_query(hints, query_name)`

Extracts hints for a specific query part from the full hints string.

**Logic:**
- Parses the hints string looking for `[query_name]` prefixes
- Extracts hints between `[query_name]` and the next `[` or end of string
- Handles unprefixed hints (treats them as `[main]` hints)
- Returns trimmed hint string for the specified query

**Example:**
```c
extract_hints_for_query("[main]SeqScan(t1) [cte_data]IndexScan(t2)", "main")
// Returns: "SeqScan(t1)"

extract_hints_for_query("[main]SeqScan(t1) [cte_data]IndexScan(t2)", "cte_data")
// Returns: "IndexScan(t2)"
```

#### 2. `inject_hints_into_sql(query_text, hints)`

Main function that parses SQL and inserts hints at appropriate locations.

**Parsing Strategy:**

1. **Skip Leading Comments/Whitespace**
   - Handles `--` line comments
   - Handles `/* */` block comments
   - Preserves comment text in output

2. **Detect Query Type**
   - Checks for `WITH` clause (CTEs)
   - Checks for `SELECT` statement
   - Falls back to returning original text if neither

3. **Handle CTEs (WITH clause)**
   - Parses `WITH cte_name AS (SELECT ...)` syntax
   - Extracts CTE name and looks up `[cte_<name>]` hints
   - Injects hints after `SELECT` within CTE definition
   - Handles multiple CTEs separated by commas
   - Tracks parenthesis depth to correctly parse nested structures

4. **Handle Main Query**
   - After CTEs (if any), finds main `SELECT`
   - Extracts `[main]` hints or unprefixed hints
   - Injects as `SELECT /*+ hints */ ...`

### Example Transformations

#### Simple SELECT

**Input:**
```sql
Query: SELECT * FROM users WHERE id = 1
Hints: SeqScan(users)
```

**Output:**
```sql
SELECT /*+ SeqScan(users) */ * FROM users WHERE id = 1
```

#### WITH Clause (Single CTE)

**Input:**
```sql
Query: WITH active_users AS (SELECT * FROM users WHERE active = true)
       SELECT * FROM active_users
Hints: [cte_active_users]IndexScan(users active_idx) [main]SeqScan(active_users)
```

**Output:**
```sql
WITH active_users AS (SELECT /*+ IndexScan(users active_idx) */ * FROM users WHERE active = true)
SELECT /*+ SeqScan(active_users) */ * FROM active_users
```

#### Multiple CTEs

**Input:**
```sql
Query: WITH cte1 AS (SELECT * FROM t1),
            cte2 AS (SELECT * FROM t2)
       SELECT * FROM cte1 JOIN cte2 ON cte1.id = cte2.id
Hints: [cte_cte1]IndexScan(t1) [cte_cte2]SeqScan(t2) [main]HashJoin(cte1 cte2)
```

**Output:**
```sql
WITH cte1 AS (SELECT /*+ IndexScan(t1) */ * FROM t1),
     cte2 AS (SELECT /*+ SeqScan(t2) */ * FROM t2)
SELECT /*+ HashJoin(cte1 cte2) */ * FROM cte1 JOIN cte2 ON cte1.id = cte2.id
```

## SQL Interface

### Function Definition

```sql
CREATE FUNCTION pg_outline.inject_hints(
    p_query_text TEXT,
    p_hints TEXT
)
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pg_outline_inject_hints'
LANGUAGE C STRICT;
```

### Usage Examples

```sql
-- Basic usage
SELECT pg_outline.inject_hints(
    'SELECT * FROM users WHERE id = 1',
    'SeqScan(users)'
);

-- With multi-query hints
SELECT pg_outline.inject_hints(
    'WITH data AS (SELECT * FROM users) SELECT * FROM data',
    '[cte_data]SeqScan(users) [main]HashJoin(data)'
);

-- Combine with outline lookup
SELECT pg_outline.inject_hints(
    o.query_text,
    o.hints
)
FROM pg_outline.outlines o
WHERE o.name = 'my_outline';
```

## Testing

Comprehensive tests added to `contrib/pg_outline/sql/pg_outline.sql`:

1. Simple SELECT with simple hints
2. Simple SELECT with `[main]` prefix
3. WITH clause (CTE) with multi-query hints
4. Multiple CTEs with different hints
5. Mixed prefix and non-prefix hints
6. Only CTE hints, no main hints
7. Empty hints
8. Complex query with existing comments

## Limitations

### Current Limitations

1. **Subquery Support**: Limited support for subqueries in FROM clause or WHERE clause
   - Named CTEs work well
   - Inline subqueries would need additional parsing

2. **Comment Preservation**: Existing `/*+ */` hints in the original query are not detected
   - May result in duplicate hint comments

3. **String Parsing**: Uses simple character-by-character parsing
   - Doesn't use full SQL parser
   - May have edge cases with complex SQL syntax

4. **Parenthesis Tracking**: Uses depth counter for CTE parsing
   - Works for standard cases
   - May have issues with extremely nested structures

### Known Edge Cases

- **Quoted Identifiers**: CTE names with quotes may not parse correctly
- **Reserved Keywords**: CTE names that are SQL keywords may cause issues
- **Nested CTEs**: CTEs within CTEs not tested
- **Multi-line Comments**: Complex multi-line comment structures may affect parsing

## Future Enhancements

### Priority 1 (Next Steps)

1. **Full Subquery Support**
   - Parse subqueries in FROM clause
   - Parse subqueries in WHERE/HAVING (SubLinks)
   - Support `[subquery_N]` and `[sublink_N]` prefixes

2. **Duplicate Hint Detection**
   - Check for existing `/*+ ... */` in query
   - Skip injection if hints already present
   - Or merge/replace existing hints

### Priority 2 (Future Work)

3. **Enhanced Parsing**
   - Use PostgreSQL's built-in parser for more robust SQL parsing
   - Handle all SQL statement types (INSERT, UPDATE, DELETE)
   - Support UNION queries

4. **Advanced Features**
   - Support for hint inheritance (parent to child queries)
   - Hint validation before injection
   - Pretty-printing of injected SQL

## Performance Considerations

- **String Operations**: Uses StringInfo for efficient string building
- **Memory Management**: All allocations use PostgreSQL's memory contexts
- **Linear Parsing**: Single-pass parsing, O(n) complexity
- **No Caching**: Each call re-parses the query (acceptable for utility function)

## Security Considerations

- **Input Validation**: No SQL injection risk (function returns text, doesn't execute)
- **Memory Safety**: Uses PostgreSQL's memory management (pfree/pstrdup)
- **No Privilege Escalation**: Function is STRICT and doesn't access system catalogs

## Compatibility

- **PostgreSQL Version**: 12.22+
- **pg_hint_plan Syntax**: Compatible with pg_hint_plan hint syntax
- **OceanBase Style**: Follows OceanBase's `[query_name]` prefix convention

## References

- **OceanBase Outline Documentation**: Multi-query hint syntax inspiration
- **pg_hint_plan**: Hint syntax format (SeqScan, HashJoin, Leading, etc.)
- **PostgreSQL StringInfo**: Efficient string building API

## Contributing

To extend this functionality:

1. Add new query type handling in `inject_hints_into_sql()`
2. Add corresponding hint prefix format (e.g., `[sublink_N]`)
3. Add tests in `sql/pg_outline.sql`
4. Update this documentation

## License

Same as pg_outline extension (PostgreSQL License)

---

**Document Version**: 1.0
**Last Updated**: 2026-03-30
**Status**: Implementation Complete - Core Features Operational
