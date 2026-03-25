# Inline Hints and Simplified Outline Creation

## Overview

The pg_outline extension now supports inline hints and simplified outline creation. This feature allows you to:

1. **Write SQL with hints** using `/*+ ... */` syntax
2. **Create outlines automatically** from SQL with hints
3. **Apply stored plans** to the same SQL without hints

## Key Design Principle

> **Important**: The hints stored in an outline are **derived from the execution plan**, not from the input hints directly.

The workflow is:
1. Input SQL with hints → Controls how PostgreSQL generates the plan
2. PostgreSQL creates execution plan → Based on the hints provided
3. Extension extracts hints from plan → Reverse-engineers what actually happened
4. Stored hints used for matching → Applied to future queries without hints

## Inline Hint Syntax

Hints are specified using `/*+ ... */` comments:

```sql
-- Single hint after SELECT
SELECT /*+ SeqScan(t1) */ * FROM t1 WHERE id > 100;

-- Multiple hints
SELECT /*+ SeqScan(t1) IndexScan(t2) HashJoin(t1 t2) */
       t1.id, t2.name
FROM t1 JOIN t2 ON t1.id = t2.t1_id;

-- Hints in subqueries
SELECT * FROM (
    SELECT /*+ SeqScan(t1) */ * FROM t1
) AS sub
JOIN /*+ NestLoop(sub t2) */ t2 ON sub.id = t2.id;

-- Hints in CTEs
WITH cte AS (
    SELECT /*+ IndexScan(t1) */ * FROM t1
)
SELECT /*+ HashJoin(cte t2) */ * FROM cte JOIN t2 ON cte.id = t2.id;
```

## Simplified Outline Creation

### New Function: `pg_outline_create_from_sql()`

```sql
-- Create extension
CREATE EXTENSION pg_outline;

-- Create an outline from SQL with hints
SELECT pg_outline_create_from_sql(
    'my_outline',  -- outline name
    'SELECT /*+ SeqScan(t1) */ * FROM t1 WHERE value > 100'
);
```

### What Happens Internally:

1. **Extract Hints**: Parse `/*+ SeqScan(t1) */` from the SQL
2. **Strip Hints**: Create clean query: `SELECT * FROM t1 WHERE value > 100`
3. **Plan with Hints**: Execute planner with the original hints
4. **Extract from Plan**: Get actual hints from the resulting execution plan
5. **Store Outline**: Save extracted hints with fingerprint of clean query

### Matching Future Queries:

```sql
-- Enable manual mode to use stored outlines
SET pg_outline.mode = 'manual';

-- This query will match the outline and use the stored plan
SELECT * FROM t1 WHERE value > 500;
-- Query fingerprint matches → Stored hints applied → Fixed plan used
```

## Multiple SELECT Support

The implementation supports multiple SELECT statements with hints:

```sql
-- Main query with hint
SELECT /*+ HashJoin(t1 t2) */ t1.id, t2.name
FROM t1
-- Subquery with different hint
JOIN (
    SELECT /*+ IndexScan(t3) */ id, name FROM t3
) AS t2 ON t1.id = t2.id
-- Another subquery with hint
WHERE t1.id IN (
    SELECT /*+ SeqScan(t4) */ t4_id FROM t4 WHERE enabled = true
);
```

All hints are extracted and combined for the outline.

## Comparison: Old vs New Method

### Old Method (Manual):

```sql
-- 1. Create outline manually with pre-defined hints
SELECT pg_outline_create(
    'my_outline',
    'SELECT * FROM t1 WHERE value > ?',
    'SeqScan(t1)'  -- You specify the hints
);
```

### New Method (Simplified):

```sql
-- 1. Create outline from SQL with hints
SELECT pg_outline_create_from_sql(
    'my_outline',
    'SELECT /*+ SeqScan(t1) */ * FROM t1 WHERE value > 100'
);
-- Hints are automatically extracted from the resulting plan
```

## Complete Workflow Example

```sql
-- ===== Step 1: Setup =====
CREATE EXTENSION pg_outline;

-- Create sample table
CREATE TABLE users (
    id INT PRIMARY KEY,
    name TEXT,
    age INT,
    created_at TIMESTAMP
);

CREATE INDEX idx_users_age ON users(age);

-- ===== Step 2: Create Outline with Hints =====
SELECT pg_outline_create_from_sql(
    'users_by_age',
    'SELECT /*+ IndexScan(users) */ id, name, age
     FROM users
     WHERE age > 25'
);

-- Output:
-- NOTICE: pg_outline_create_from_sql: outline 'users_by_age' created with fingerprint abc123...
-- NOTICE: Extracted hints: IndexScan(users)

-- ===== Step 3: Enable Manual Mode =====
SET pg_outline.mode = 'manual';

-- ===== Step 4: Execute Query Without Hints =====
-- This will match the outline and use the fixed plan
SELECT id, name, age FROM users WHERE age > 30;
-- ✓ Uses IndexScan because of stored outline

-- ===== Step 5: View with EXPLAIN =====
EXPLAIN SELECT id, name, age FROM users WHERE age > 30;
-- Shows: Index Scan using idx_users_age on users
-- Plan is fixed by the outline

-- ===== Step 6: Disable Outline =====
SELECT pg_outline_disable('users_by_age');

-- Now the same query will use the planner's default choice
EXPLAIN SELECT id, name, age FROM users WHERE age > 30;
-- Might show: Seq Scan (if planner prefers it)
```

## Supported Hint Types

### Scan Method Hints:
- `SeqScan(table)` - Force sequential scan
- `IndexScan(table)` - Force index scan
- `IndexOnlyScan(table)` - Force index-only scan
- `BitmapScan(table)` - Force bitmap heap scan

### Join Method Hints:
- `NestLoop(t1 t2)` - Force nested loop join
- `HashJoin(t1 t2)` - Force hash join
- `MergeJoin(t1 t2)` - Force merge join

### Join Order Hints:
- `Leading(t1 t2 t3)` - Specify join order

## Management Functions

```sql
-- List all outlines
SELECT * FROM pg_outline_list();

-- View enabled outlines
SELECT * FROM pg_outline_enabled;

-- Enable an outline
SELECT pg_outline_enable('outline_name');

-- Disable an outline
SELECT pg_outline_disable('outline_name');

-- Drop an outline
SELECT pg_outline_drop('outline_name');
```

## Configuration Parameters

```sql
-- Enable/disable the extension
SET pg_outline.enabled = true;

-- Mode: 'auto', 'manual', or 'off'
-- auto: Generate hints from plans automatically
-- manual: Use stored outlines
-- off: Disable outline generation
SET pg_outline.mode = 'manual';

-- Display generated hints after query execution
SET pg_outline.display_hints = true;
```

## Advanced Example: Complex Query with Multiple Hints

```sql
-- Create outline for complex query with multiple hints
SELECT pg_outline_create_from_sql(
    'complex_report',
    '
    WITH active_users AS (
        SELECT /*+ IndexScan(users) */ id, name, created_at
        FROM users
        WHERE status = ''active''
    ),
    recent_orders AS (
        SELECT /*+ SeqScan(orders) */ user_id, COUNT(*) as order_count
        FROM orders
        WHERE created_at > CURRENT_DATE - INTERVAL ''30 days''
        GROUP BY user_id
    )
    SELECT /*+ HashJoin(active_users recent_orders) MergeJoin(u profiles) */
           u.id, u.name, o.order_count, p.preferences
    FROM active_users u
    LEFT JOIN recent_orders o ON u.id = o.user_id
    LEFT JOIN profiles p ON u.id = p.user_id
    ORDER BY o.order_count DESC
    LIMIT 100
    '
);

-- Now the same query structure without hints will use the fixed plan
SET pg_outline.mode = 'manual';

-- This matches the outline
WITH active_users AS (
    SELECT id, name, created_at
    FROM users
    WHERE status = 'active'
),
recent_orders AS (
    SELECT user_id, COUNT(*) as order_count
    FROM orders
    WHERE created_at > CURRENT_DATE - INTERVAL '30 days'
    GROUP BY user_id
)
SELECT u.id, u.name, o.order_count, p.preferences
FROM active_users u
LEFT JOIN recent_orders o ON u.id = o.user_id
LEFT JOIN profiles p ON u.id = p.user_id
ORDER BY o.order_count DESC
LIMIT 100;

-- Verify the plan with EXPLAIN
EXPLAIN (VERBOSE, COSTS) [...same query...];
```

## Limitations and Notes

1. **Plan Extraction**: The stored hints reflect what PostgreSQL actually did, which may differ from input hints if:
   - Hints are invalid
   - Required indexes don't exist
   - Table statistics suggest a different approach

2. **Query Matching**: Queries must match the fingerprint (normalized form):
   - Different literal values → Same fingerprint ✓
   - Different table aliases → Different fingerprint ✗
   - Different whitespace → Same fingerprint ✓

3. **Hint Application**: Currently extracts hints from plans. Full hint application (modifying planner decisions) is in development.

4. **EXPLAIN Support**: EXPLAIN shows the plan that will be used, including any applied outline hints.

## Troubleshooting

### No Hints Extracted
```sql
-- If you see: "no hints could be extracted from plan"
-- Check if the query produces a valid plan:
EXPLAIN SELECT /*+ SeqScan(t1) */ * FROM t1 WHERE id > 100;
```

### Outline Not Matching
```sql
-- Check fingerprint of your query:
SELECT compute_query_fingerprint(normalize_query('SELECT * FROM t1 WHERE id > 100'));

-- List outlines and their patterns:
SELECT outline_name, query_pattern FROM pg_outline_list();
```

### Hints Not Applied
```sql
-- Make sure you're in manual mode:
SHOW pg_outline.mode;  -- Should be 'manual'

-- Check if outline is enabled:
SELECT * FROM pg_outline_enabled WHERE outline_name = 'my_outline';
```

## Next Steps

For more information:
- See `README.md` for general extension documentation
- See `IMPLEMENTATION_SUMMARY.md` for technical details
- See `ENHANCEMENTS.md` for recent improvements
