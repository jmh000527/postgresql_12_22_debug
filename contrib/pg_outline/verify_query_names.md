# Query Naming Verification Test Results

## Purpose
This document describes how to verify that Query names are correctly prefixed to outline hints when hints are specified in CTEs, subqueries, and sublinks.

## Testing Instructions

### Setup
1. Ensure PostgreSQL is compiled with pg_outline extension
2. Install the extension: `CREATE EXTENSION pg_outline;`
3. Enable outline features:
   ```sql
   SET pg_outline.enabled = true;
   SET pg_outline.display_hints = true;
   SET pg_outline.mode = 'auto';
   ```

### Run the Test
```bash
psql -d your_database -f test_query_naming.sql
```

## Expected Query Naming Patterns

### Pattern 1: Main Query Only
```sql
SELECT /*+ SeqScan(t1) */ * FROM t1;
```
**Expected Output:**
- `[main] SeqScan(t1)`

### Pattern 2: CTE (Common Table Expression)
```sql
WITH orders AS (
    SELECT /*+ SeqScan(orders_table) */ * FROM orders_table
)
SELECT /*+ HashJoin(customers orders) */ *
FROM customers JOIN orders ON customers.id = orders.customer_id;
```
**Expected Output:**
- `[main] HashJoin(customers orders)`
- `[cte_orders] SeqScan(orders_table)`

### Pattern 3: Subquery in FROM Clause
```sql
SELECT /*+ HashJoin(t1 sub) */ t1.id, sub.data
FROM t1
JOIN (
    SELECT /*+ IndexScan(t2) */ id, data FROM t2
) AS sub ON t1.id = sub.id;
```
**Expected Output:**
- `[main] HashJoin(t1 sub)`
- `[subquery_0] IndexScan(t2)`

### Pattern 4: SubLink in WHERE (IN Clause)
```sql
SELECT /*+ SeqScan(t1) */ *
FROM t1
WHERE id IN (
    SELECT /*+ IndexScan(t2) */ t1_id FROM t2
);
```
**Expected Output:**
- `[main] SeqScan(t1)`
- `[sublink_0] IndexScan(t2)`

### Pattern 5: SubLink in WHERE (EXISTS Clause)
```sql
SELECT /*+ SeqScan(t1) */ *
FROM t1
WHERE EXISTS (
    SELECT /*+ SeqScan(t2) */ 1 FROM t2 WHERE t2.t1_id = t1.id
);
```
**Expected Output:**
- `[main] SeqScan(t1)`
- `[sublink_0] SeqScan(t2)`

### Pattern 6: Multiple CTEs
```sql
WITH cte1 AS (
    SELECT /*+ SeqScan(t1) */ * FROM t1
),
cte2 AS (
    SELECT /*+ SeqScan(t2) */ * FROM t2
)
SELECT /*+ HashJoin(cte1 cte2) */ cte1.id, cte2.data
FROM cte1 JOIN cte2 ON cte1.id = cte2.t1_id;
```
**Expected Output:**
- `[main] HashJoin(cte1 cte2)`
- `[cte_cte1] SeqScan(t1)`
- `[cte_cte2] SeqScan(t2)`

### Pattern 7: Complex - CTE + Subquery + SubLink
```sql
WITH cte1 AS (
    SELECT /*+ SeqScan(t2) */ * FROM t2
)
SELECT /*+ HashJoin(t1 sub) */ t1.id, sub.name
FROM t1
JOIN (
    SELECT /*+ NestLoop(t3 cte1) */ t3.id, cte1.data as name
    FROM t3
    JOIN cte1 ON t3.id = cte1.id
    WHERE t3.id IN (
        SELECT /*+ SeqScan(t4) */ t4_id FROM t4
    )
) AS sub ON t1.id = sub.id;
```
**Expected Output:**
- `[main] HashJoin(t1 sub)`
- `[cte_cte1] SeqScan(t2)`
- `[subquery_0] NestLoop(t3 cte1)`
- `[sublink_0] SeqScan(t4)`

### Pattern 8: Nested SubLinks
```sql
SELECT /*+ SeqScan(t1) */ *
FROM t1
WHERE id IN (
    SELECT /*+ SeqScan(t2) */ t1_id
    FROM t2
    WHERE id IN (
        SELECT /*+ SeqScan(t3) */ id FROM t3
    )
);
```
**Expected Output:**
- `[main] SeqScan(t1)`
- `[sublink_0] SeqScan(t2)` (first level IN)
- `[sublink_1] SeqScan(t3)` (nested IN)

### Pattern 9: Multiple SubLinks in Different Clauses
```sql
SELECT /*+ SeqScan(t1) */ *
FROM t1
WHERE id IN (
    SELECT /*+ SeqScan(t2) */ t1_id FROM t2
)
AND value > (
    SELECT /*+ SeqScan(t3) */ AVG(id) FROM t3
);
```
**Expected Output:**
- `[main] SeqScan(t1)`
- `[sublink_0] SeqScan(t2)` (IN subquery)
- `[sublink_1] SeqScan(t3)` (scalar subquery)

### Pattern 10: Scalar Subquery in SELECT List
```sql
SELECT /*+ SeqScan(t1) */
    t1.id,
    (SELECT /*+ SeqScan(t2) */ COUNT(*) FROM t2 WHERE t2.t1_id = t1.id) as count
FROM t1;
```
**Expected Output:**
- `[main] SeqScan(t1)`
- `[sublink_0] SeqScan(t2)`

## Verification Checklist

When running the test, verify the following:

- [ ] Main query hints are prefixed with `[main]`
- [ ] CTE hints are prefixed with `[cte_<cte_name>]` where `<cte_name>` is the actual CTE name
- [ ] Subquery hints (FROM clause) are prefixed with `[subquery_0]`, `[subquery_1]`, etc. in order of appearance
- [ ] SubLink hints (WHERE/SELECT/HAVING) are prefixed with `[sublink_0]`, `[sublink_1]`, etc. in order of appearance
- [ ] Multiple CTEs get unique names: `[cte_cte1]`, `[cte_cte2]`, etc.
- [ ] Nested sublinks get sequential numbers
- [ ] Complex queries with all types show all correct prefixes
- [ ] Query names in stored outlines (via `pg_outline_create_from_sql`) match the expected pattern

## Common Issues to Watch For

1. **Missing Prefixes**: Hints without `[query_name]` prefix indicate query naming is not working
2. **Wrong Names**: Incorrect prefixes (e.g., `[main]` for a CTE) indicate naming logic error
3. **Duplicate Numbers**: Same subquery/sublink number used twice indicates counter issue
4. **Mixed Order**: Queries named out of order indicate traversal problem

## Success Criteria

The test passes if:
1. All hints in EXPLAIN output have proper `[query_name]` prefixes
2. Query names follow the documented naming convention
3. Sequential numbering for subqueries and sublinks is correct
4. CTE names match their actual names in the SQL
5. No duplicate or missing query names
6. Stored outlines preserve the query name prefixes

## Technical Implementation Notes

Based on the code analysis:
- Query naming happens in `assign_query_names()` function (pg_outline.c:2708-2770)
- Names are generated by `generate_query_name()` (pg_outline.c:2593-2627)
- Query metadata is stored in hash table using `store_query_metadata()` (pg_outline.c:2482-2515)
- Naming now uses `QueryNamingContext` structure with non-static `query_index` field
- Query naming occurs in `outline_planner()` hook BEFORE optimizer runs (pg_outline.c:348-362)
