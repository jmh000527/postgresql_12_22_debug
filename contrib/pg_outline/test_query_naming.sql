-- ============================================================================
-- Test Query Naming for Outline Hints
-- ============================================================================
-- This test verifies that Query names are correctly prefixed to hints when
-- hints are specified in CTEs, subqueries, and sublinks.
--
-- Expected Query Naming:
-- - main: Top-level query
-- - cte_<name>: Common Table Expressions
-- - subquery_N: Subqueries in FROM clause
-- - sublink_N: Subqueries in expressions (WHERE, SELECT, HAVING)
-- ============================================================================

-- Load the extension
CREATE EXTENSION IF NOT EXISTS pg_outline;

-- Enable outline display
SET pg_outline.enabled = true;
SET pg_outline.display_hints = true;
SET pg_outline.mode = 'auto';

-- Create test tables
DROP TABLE IF EXISTS t1, t2, t3, t4, orders_table, customers CASCADE;

CREATE TABLE t1 (
    id INTEGER PRIMARY KEY,
    name TEXT,
    value INTEGER
);

CREATE TABLE t2 (
    id INTEGER PRIMARY KEY,
    t1_id INTEGER,
    data TEXT
);

CREATE TABLE t3 (
    id INTEGER PRIMARY KEY,
    name TEXT
);

CREATE TABLE t4 (
    t4_id INTEGER PRIMARY KEY,
    info TEXT
);

CREATE TABLE orders_table (
    id INTEGER PRIMARY KEY,
    customer_id INTEGER,
    amount NUMERIC
);

CREATE TABLE customers (
    id INTEGER PRIMARY KEY,
    name TEXT
);

-- Insert test data
INSERT INTO t1 SELECT i, 'name_' || i, i * 10 FROM generate_series(1, 100) i;
INSERT INTO t2 SELECT i, (i % 100) + 1, 'data_' || i FROM generate_series(1, 100) i;
INSERT INTO t3 SELECT i, 'name_' || i FROM generate_series(1, 50) i;
INSERT INTO t4 SELECT i, 'info_' || i FROM generate_series(1, 50) i;
INSERT INTO orders_table SELECT i, (i % 100) + 1, i * 100.0 FROM generate_series(1, 100) i;
INSERT INTO customers SELECT i, 'customer_' || i FROM generate_series(1, 100) i;

-- Analyze tables for better plans
ANALYZE t1, t2, t3, t4, orders_table, customers;

\echo '============================================================================'
\echo 'Test 1: Simple Query (main only)'
\echo '============================================================================'

EXPLAIN (COSTS OFF)
SELECT /*+ SeqScan(t1) */ * FROM t1 WHERE id < 10;

\echo ''
\echo '============================================================================'
\echo 'Test 2: Query with CTE'
\echo '============================================================================'

EXPLAIN (COSTS OFF)
WITH orders AS (
    SELECT /*+ SeqScan(orders_table) */ * FROM orders_table WHERE amount > 500
)
SELECT /*+ HashJoin(customers orders) */ *
FROM customers
JOIN orders ON customers.id = orders.customer_id;

\echo ''
\echo '============================================================================'
\echo 'Test 3: Query with Subquery in FROM'
\echo '============================================================================'

EXPLAIN (COSTS OFF)
SELECT /*+ HashJoin(t1 sub) */ t1.id, sub.data
FROM t1
JOIN (
    SELECT /*+ IndexScan(t2) */ id, data FROM t2 WHERE id < 50
) AS sub ON t1.id = sub.id;

\echo ''
\echo '============================================================================'
\echo 'Test 4: Query with SubLink in WHERE (IN clause)'
\echo '============================================================================'

EXPLAIN (COSTS OFF)
SELECT /*+ SeqScan(t1) */ *
FROM t1
WHERE id IN (
    SELECT /*+ IndexScan(t2) */ t1_id FROM t2 WHERE data LIKE 'data_1%'
);

\echo ''
\echo '============================================================================'
\echo 'Test 5: Query with SubLink in WHERE (EXISTS clause)'
\echo '============================================================================'

EXPLAIN (COSTS OFF)
SELECT /*+ SeqScan(t1) */ *
FROM t1
WHERE EXISTS (
    SELECT /*+ SeqScan(t2) */ 1 FROM t2 WHERE t2.t1_id = t1.id AND t2.data = 'data_10'
);

\echo ''
\echo '============================================================================'
\echo 'Test 6: Complex Query with Multiple CTEs'
\echo '============================================================================'

EXPLAIN (COSTS OFF)
WITH cte1 AS (
    SELECT /*+ SeqScan(t1) */ * FROM t1 WHERE value > 100
),
cte2 AS (
    SELECT /*+ SeqScan(t2) */ * FROM t2 WHERE id < 50
)
SELECT /*+ HashJoin(cte1 cte2) */ cte1.id, cte2.data
FROM cte1
JOIN cte2 ON cte1.id = cte2.t1_id;

\echo ''
\echo '============================================================================'
\echo 'Test 7: Complex Query with CTE, Subquery, and SubLink'
\echo '============================================================================'

EXPLAIN (COSTS OFF)
WITH cte1 AS (
    SELECT /*+ SeqScan(t2) */ * FROM t2 WHERE id < 50
)
SELECT /*+ HashJoin(t1 sub) */ t1.id, sub.name
FROM t1
JOIN (
    SELECT /*+ NestLoop(t3 cte1) */ t3.id, cte1.data as name
    FROM t3
    JOIN cte1 ON t3.id = cte1.id
    WHERE t3.id IN (
        SELECT /*+ SeqScan(t4) */ t4_id FROM t4 WHERE info LIKE 'info_1%'
    )
) AS sub ON t1.id = sub.id;

\echo ''
\echo '============================================================================'
\echo 'Test 8: Nested Subqueries'
\echo '============================================================================'

EXPLAIN (COSTS OFF)
SELECT /*+ SeqScan(t1) */ *
FROM t1
WHERE id IN (
    SELECT /*+ SeqScan(t2) */ t1_id
    FROM t2
    WHERE id IN (
        SELECT /*+ SeqScan(t3) */ id FROM t3 WHERE id < 20
    )
);

\echo ''
\echo '============================================================================'
\echo 'Test 9: Multiple SubLinks in Different Clauses'
\echo '============================================================================'

EXPLAIN (COSTS OFF)
SELECT /*+ SeqScan(t1) */ *
FROM t1
WHERE id IN (
    SELECT /*+ SeqScan(t2) */ t1_id FROM t2
)
AND value > (
    SELECT /*+ SeqScan(t3) */ AVG(id) FROM t3
);

\echo ''
\echo '============================================================================'
\echo 'Test 10: Scalar Subquery in SELECT List'
\echo '============================================================================'

EXPLAIN (COSTS OFF)
SELECT /*+ SeqScan(t1) */
    t1.id,
    t1.name,
    (SELECT /*+ SeqScan(t2) */ COUNT(*) FROM t2 WHERE t2.t1_id = t1.id) as count
FROM t1
WHERE id < 10;

\echo ''
\echo '============================================================================'
\echo 'Test 11: Very Complex Query - All Types Combined'
\echo '============================================================================'

EXPLAIN (COSTS OFF)
WITH main_cte AS (
    SELECT /*+ SeqScan(t1) */ * FROM t1 WHERE value > 200
),
secondary_cte AS (
    SELECT /*+ IndexScan(t2) */ * FROM t2 WHERE id < 30
)
SELECT /*+ HashJoin(main_cte sub) */
    main_cte.id,
    sub.combined_name,
    (SELECT /*+ SeqScan(orders_table) */ COUNT(*)
     FROM orders_table
     WHERE customer_id = main_cte.id) as order_count
FROM main_cte
JOIN (
    SELECT /*+ NestLoop(t3 secondary_cte) */
        t3.id,
        t3.name || '-' || secondary_cte.data as combined_name
    FROM t3
    JOIN secondary_cte ON t3.id = secondary_cte.id
    WHERE t3.id IN (
        SELECT /*+ SeqScan(t4) */ t4_id FROM t4 WHERE info LIKE 'info_2%'
    )
    AND EXISTS (
        SELECT /*+ SeqScan(customers) */ 1
        FROM customers
        WHERE customers.id = t3.id
    )
) AS sub ON main_cte.id = sub.id
WHERE main_cte.value > (
    SELECT /*+ SeqScan(t1) */ AVG(value) FROM t1
);

\echo ''
\echo '============================================================================'
\echo 'Test 12: Using pg_outline_create_from_sql to verify stored hints'
\echo '============================================================================'

-- Create an outline from a complex query
SELECT pg_outline_create_from_sql(
    'test_complex_query',
    'WITH cte1 AS (
        SELECT /*+ SeqScan(t1) */ * FROM t1 WHERE value > 100
     )
     SELECT /*+ HashJoin(t1 cte1) */ t1.id, cte1.name
     FROM t1
     JOIN cte1 ON t1.id = cte1.id
     WHERE t1.id IN (
         SELECT /*+ IndexScan(t2) */ t1_id FROM t2
     )'
);

-- Check the stored outline hints
SELECT outline_name, query_pattern, hints
FROM pg_outline_list()
WHERE outline_name = 'test_complex_query';

\echo ''
\echo '============================================================================'
\echo 'Cleanup'
\echo '============================================================================'

-- Drop the test outline
SELECT pg_outline_drop('test_complex_query');

-- Clean up tables
DROP TABLE IF EXISTS t1, t2, t3, t4, orders_table, customers CASCADE;

-- Drop extension
DROP EXTENSION pg_outline CASCADE;

\echo ''
\echo '============================================================================'
\echo 'Test Complete'
\echo '============================================================================'
\echo 'Expected Results:'
\echo '- All EXPLAIN outputs should show hints with proper [query_name] prefixes'
\echo '- Main query hints: [main]'
\echo '- CTE hints: [cte_<cte_name>]'
\echo '- Subquery hints: [subquery_N]'
\echo '- SubLink hints: [sublink_N]'
\echo '============================================================================'
