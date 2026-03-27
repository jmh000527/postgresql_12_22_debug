#!/bin/bash
# Quick Test Script for Query Naming Verification
# This script provides a fast way to verify Query naming is working correctly

echo "=================================="
echo "Query Naming Quick Verification"
echo "=================================="
echo ""

# Check if psql is available
if ! command -v psql &> /dev/null; then
    echo "Error: psql not found. Please ensure PostgreSQL is installed."
    exit 1
fi

# Set test database name
TEST_DB="test_query_naming_$$"

echo "1. Creating test database: $TEST_DB"
createdb "$TEST_DB" 2>/dev/null || {
    echo "   Using existing database: $TEST_DB"
}

echo "2. Running query naming tests..."
echo ""

# Run the comprehensive test
psql -d "$TEST_DB" <<'EOF'
-- Setup
CREATE EXTENSION IF NOT EXISTS pg_outline;
SET pg_outline.enabled = true;
SET pg_outline.display_hints = true;
SET pg_outline.mode = 'auto';

-- Create minimal test tables
CREATE TEMP TABLE t1 (id int, val int);
CREATE TEMP TABLE t2 (id int, t1_id int);
CREATE TEMP TABLE t3 (id int);
INSERT INTO t1 SELECT i, i*10 FROM generate_series(1,10) i;
INSERT INTO t2 SELECT i, i FROM generate_series(1,10) i;
INSERT INTO t3 SELECT i FROM generate_series(1,10) i;

\echo '=== Test 1: Main Query ==='
EXPLAIN (COSTS OFF) SELECT /*+ SeqScan(t1) */ * FROM t1 WHERE id < 5;

\echo ''
\echo '=== Test 2: CTE ==='
EXPLAIN (COSTS OFF)
WITH my_cte AS (SELECT /*+ SeqScan(t1) */ * FROM t1)
SELECT /*+ HashJoin(t2 my_cte) */ * FROM t2 JOIN my_cte ON t2.t1_id = my_cte.id;

\echo ''
\echo '=== Test 3: Subquery in FROM ==='
EXPLAIN (COSTS OFF)
SELECT /*+ SeqScan(t1) */ * FROM t1
JOIN (SELECT /*+ SeqScan(t2) */ * FROM t2) sub ON t1.id = sub.t1_id;

\echo ''
\echo '=== Test 4: SubLink in WHERE ==='
EXPLAIN (COSTS OFF)
SELECT /*+ SeqScan(t1) */ * FROM t1
WHERE id IN (SELECT /*+ SeqScan(t2) */ t1_id FROM t2);

\echo ''
\echo '=== Test 5: Complex - All Types ==='
EXPLAIN (COSTS OFF)
WITH cte1 AS (SELECT /*+ SeqScan(t2) */ * FROM t2)
SELECT /*+ HashJoin(t1 sub) */ * FROM t1
JOIN (
    SELECT /*+ NestLoop(t3 cte1) */ t3.id
    FROM t3 JOIN cte1 ON t3.id = cte1.id
    WHERE t3.id IN (SELECT /*+ SeqScan(t2) */ id FROM t2)
) sub ON t1.id = sub.id;

\echo ''
\echo '=================================='
\echo 'Expected Patterns:'
\echo '[main] - for main queries'
\echo '[cte_<name>] - for CTEs with their names'
\echo '[subquery_N] - for subqueries in FROM'
\echo '[sublink_N] - for subqueries in WHERE/SELECT/HAVING'
\echo '=================================='
EOF

echo ""
echo "3. Test completed!"
echo ""
echo "To review the full test suite, run:"
echo "   psql -d $TEST_DB -f contrib/pg_outline/test_query_naming.sql"
echo ""
echo "To cleanup:"
echo "   dropdb $TEST_DB"
echo ""
