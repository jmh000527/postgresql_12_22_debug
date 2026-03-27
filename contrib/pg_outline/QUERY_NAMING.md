# Query Naming for Outline Hints

## Overview

The pg_outline extension now assigns unique names to each Query structure in the parse tree and prefixes hints with `[query_name]` to clearly identify which Query each hint belongs to.

## Motivation

SQL statements can contain multiple Query structures:
- Main query (top-level SELECT)
- Subqueries in FROM clause
- Common Table Expressions (CTEs / WITH clauses)
- SubLinks in expressions (WHERE IN, EXISTS, scalar subqueries, etc.)

Previously, hints were identified only by numeric index, which was not intuitive. The new naming system provides human-readable names that reflect the structure of the SQL.

## Query Naming Scheme

### Naming Rules

1. **Main Query**: `main` (or `main_1`, `main_2` for subsequent statements)
2. **CTEs**: `cte_<cte_name>` (e.g., `cte_orders`, `cte_products`)
3. **Subqueries (FROM clause)**: `subquery_0`, `subquery_1`, `subquery_2`, ...
4. **SubLinks (expressions)**: `sublink_0`, `sublink_1`, `sublink_2`, ...

### Examples

#### Example 1: Simple Query
```sql
SELECT /*+ SeqScan(t1) */ * FROM t1
```

Query naming:
- `main`: The top-level SELECT

Stored hints:
- `[main] SeqScan(t1)`

#### Example 2: Subquery in FROM
```sql
SELECT /*+ HashJoin(t1 sub) */ t1.id, sub.data
FROM t1
JOIN (
    SELECT /*+ IndexScan(t2) */ id, data FROM t2
) AS sub ON t1.id = sub.id
```

Query naming:
- `main`: The outer SELECT
- `subquery_0`: The subquery in FROM clause

Stored hints:
- `[main] HashJoin(t1 sub)`
- `[subquery_0] IndexScan(t2)`

#### Example 3: CTE (Common Table Expression)
```sql
WITH orders AS (
    SELECT /*+ SeqScan(orders_table) */ * FROM orders_table
)
SELECT /*+ HashJoin(customers orders) */ *
FROM customers
JOIN orders ON customers.id = orders.customer_id
```

Query naming:
- `main`: The main SELECT after WITH
- `cte_orders`: The CTE named "orders"

Stored hints:
- `[main] HashJoin(customers orders)`
- `[cte_orders] SeqScan(orders_table)`

#### Example 4: SubLink in WHERE
```sql
SELECT /*+ SeqScan(t1) */ *
FROM t1
WHERE id IN (
    SELECT /*+ IndexScan(t2) */ id FROM t2
)
```

Query naming:
- `main`: The outer SELECT
- `sublink_0`: The subquery in WHERE IN clause

Stored hints:
- `[main] SeqScan(t1)`
- `[sublink_0] IndexScan(t2)`

#### Example 5: Complex Query with All Types
```sql
WITH cte1 AS (
    SELECT /*+ SeqScan(t2) */ * FROM t2
)
SELECT /*+ HashJoin(t1 sub) */ t1.id, sub.name
FROM t1
JOIN (
    SELECT /*+ NestLoop(t3 cte1) */ t3.id, cte1.name
    FROM t3
    JOIN cte1 ON t3.id = cte1.id
    WHERE t3.id IN (
        SELECT /*+ SeqScan(t4) */ t4_id FROM t4
    )
) AS sub ON t1.id = sub.id
```

Query naming:
- `main`: The main SELECT
- `cte_t2`: The CTE
- `subquery_0`: The subquery in FROM
- `sublink_0`: The SubLink in WHERE IN

Stored hints:
- `[main] HashJoin(t1 sub)`
- `[cte_t2] SeqScan(t2)`
- `[subquery_0] NestLoop(t3 cte1)`
- `[sublink_0] SeqScan(t4)`

## Implementation Details

### Query Structure Extension

The `Query` structure in `src/include/nodes/parsenodes.h` now has a `query_name` field:

```c
typedef struct Query
{
    /* ... existing fields ... */

    /* Outline hint support */
    char *query_hints;    /* hint string for this specific Query, or NULL */
    char *query_name;     /* unique name for this Query in the tree, or NULL */
} Query;
```

### Query Naming Function

The `assign_query_names()` function recursively walks the Query tree and assigns names:

```c
static void assign_query_names(Query *query, QueryNamingContext *context,
                               const char *parent_name, const char *cte_name);
```

**Process:**
1. Top-level query gets name "main"
2. CTEs are processed first and get names like "cte_<cte_name>"
3. Subqueries in FROM clause get names like "subquery_0", "subquery_1", ...
4. SubLinks in expressions (WHERE, SELECT, HAVING) get names like "sublink_0", "sublink_1", ...

### Database Schema

The `pg_outline_query_hints` table stores per-Query hints:

```sql
CREATE TABLE pg_outline_query_hints (
    hint_id SERIAL PRIMARY KEY,
    outline_id INTEGER NOT NULL REFERENCES pg_outline_data(outline_id) ON DELETE CASCADE,
    query_name TEXT NOT NULL,      -- Name of Query (e.g., "main", "cte_orders", "subquery_0")
    query_index INTEGER NOT NULL,   -- Index for backward compatibility
    hint_string TEXT NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(outline_id, query_name)
);
```

### Hint Format

Hints are stored with the format: `[query_name] hint_content`

Example entries in `pg_outline_query_hints`:
```
outline_id | query_name   | query_index | hint_string
-----------+--------------+-------------+-------------------------
1          | main         | 0           | HashJoin(t1 t2)
1          | cte_orders   | 1           | SeqScan(orders_table)
1          | subquery_0   | 2           | IndexScan(t3)
1          | sublink_0    | 3           | SeqScan(t4)
```

## Usage

### Creating Outlines with Named Queries

Use `pg_outline_create_from_sql()` to create outlines from SQL with inline hints:

```sql
-- The function will automatically assign names to all queries and store hints
SELECT pg_outline_create_from_sql(
    'my_outline',
    'WITH orders AS (
         SELECT /*+ SeqScan(orders_table) */ * FROM orders_table
     )
     SELECT /*+ HashJoin(customers orders) */ *
     FROM customers
     JOIN orders ON customers.id = orders.customer_id'
);
```

### Viewing Stored Hints

Query the `pg_outline_query_hints` table to see named hints:

```sql
SELECT
    d.outline_name,
    h.query_name,
    h.query_index,
    h.hint_string
FROM pg_outline_data d
JOIN pg_outline_query_hints h ON d.outline_id = h.outline_id
WHERE d.outline_name = 'my_outline'
ORDER BY h.query_index;
```

## Benefits

1. **Human-Readable**: Query names like `cte_orders` are more intuitive than numeric indices
2. **Self-Documenting**: The naming scheme reflects the SQL structure
3. **Stability**: CTE names remain constant even if the query structure changes slightly
4. **Debugging**: Easier to identify which hint belongs to which part of the query
5. **Complete Coverage**: Supports all types of subqueries (FROM, CTEs, SubLinks)

## Backward Compatibility

- The `query_index` field is retained for backward compatibility
- Old outlines without query names will still work
- The system gracefully handles queries without assigned names

## Future Enhancements

Possible future improvements:
1. Alias-based naming for subqueries (e.g., use the AS alias)
2. More descriptive names for SubLinks (e.g., `sublink_where_in`, `sublink_select_scalar`)
3. Hierarchical names showing parent-child relationships (e.g., `main.subquery_0.sublink_0`)
4. User-configurable naming schemes

## Technical Notes

### SubLink Processing

SubLinks are subqueries in expressions (not FROM clauses). The implementation uses expression tree walkers to find and name these:

```c
static bool assign_names_sublink_walker(Node *node, QueryNamingSubLinkContext *context);
static void process_query_sublinks_naming(Query *query, QueryNamingContext *context,
                                          const char *parent_name);
```

### Memory Management

- Query names are allocated with `pstrdup()` in the current memory context
- Names persist for the lifetime of the Query structure
- Proper cleanup occurs when the Query tree is freed

### Naming Context

The `QueryNamingContext` structure tracks counters for generating unique names:

```c
typedef struct QueryNamingContext
{
    int main_query_count;    /* Counter for main queries */
    int cte_count;           /* Counter for CTEs */
    int subquery_count;      /* Counter for subqueries */
    int sublink_count;       /* Counter for sublinks */
} QueryNamingContext;
```

## See Also

- [PER_QUERY_HINTS.md](PER_QUERY_HINTS.md) - Overall per-query hints design
- [OUTLINE_FEATURE.md](OUTLINE_FEATURE.md) - Main outline feature documentation
- PostgreSQL documentation on Query structures and parse trees
