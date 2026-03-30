-- Quick test for sublink hint placement fix
-- This test verifies that hints are correctly placed in subqueries

\echo '========================================='
\echo 'Testing Sublink Hint Placement Fix'
\echo '========================================='
\echo ''

-- Setup test environment
DROP EXTENSION IF EXISTS pg_outline CASCADE;
CREATE EXTENSION pg_outline;

-- Create test tables
DROP TABLE IF EXISTS t1, t2 CASCADE;
CREATE TABLE t1 (c1 INTEGER PRIMARY KEY, value TEXT);
CREATE TABLE t2 (c2 INTEGER PRIMARY KEY, info TEXT);

-- Insert sample data
INSERT INTO t1 SELECT i, 'value_' || i FROM generate_series(1, 10) i;
INSERT INTO t2 SELECT i, 'info_' || i FROM generate_series(1, 10) i;
ANALYZE t1, t2;

\echo ''
\echo '--- Test Case 1: Simple IN subquery ---'
\echo 'Input: SELECT * FROM t1 WHERE c1 IN (SELECT c2 FROM t2 WHERE c2 < 5)'
\echo ''

-- Enable auto mode to generate outline
SET pg_outline.mode = 'auto';
SET pg_outline.display_hints = true;
SET client_min_messages = 'NOTICE';

-- Execute query - this will generate outline with hints
EXPLAIN SELECT * FROM t1 WHERE c1 IN (SELECT c2 FROM t2 WHERE c2 < 5);

\echo ''
\echo '--- Expected Output ---'
\echo 'The NOTICE message should show SQL with hints placed correctly:'
\echo '  - Main query hints after main SELECT'
\echo '  - Subquery hints after subquery SELECT (in the IN clause)'
\echo ''
\echo 'Example correct format:'
\echo '  SELECT /*+ main_hints */ * FROM t1'
\echo '    WHERE c1 IN (SELECT /*+ sublink_hints */ c2 FROM t2 ...)'
\echo ''

\echo ''
\echo '--- Test Case 2: Multiple sublinks ---'
\echo 'Input: Query with two subqueries (IN and NOT IN)'
\echo ''

EXPLAIN SELECT * FROM t1
WHERE c1 IN (SELECT c2 FROM t2 WHERE c2 < 3)
  AND c1 NOT IN (SELECT c2 FROM t2 WHERE c2 > 8);

\echo ''
\echo '--- Expected Output ---'
\echo 'Should show hints placed in both subqueries:'
\echo '  - First subquery: (SELECT /*+ sublink_0_hints */ c2 FROM t2 WHERE c2 < 3)'
\echo '  - Second subquery: (SELECT /*+ sublink_1_hints */ c2 FROM t2 WHERE c2 > 8)'
\echo ''

\echo ''
\echo '========================================='
\echo 'Test Complete'
\echo '========================================='
