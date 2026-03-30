-- Test for correct sublink hint placement
-- Verifies that hints for subqueries are placed after SELECT in subqueries

\echo '========================================='
\echo 'Test: Sublink Hint Placement'
\echo '========================================='
\echo ''

-- Setup
DROP EXTENSION IF EXISTS pg_outline CASCADE;
CREATE EXTENSION pg_outline;

-- Create test tables
DROP TABLE IF EXISTS t1, t2 CASCADE;
CREATE TABLE t1 (c1 INTEGER PRIMARY KEY, value TEXT);
CREATE TABLE t2 (c2 INTEGER PRIMARY KEY, info TEXT);

-- Insert test data
INSERT INTO t1 SELECT i, 'value_' || i FROM generate_series(1, 100) i;
INSERT INTO t2 SELECT i, 'info_' || i FROM generate_series(1, 100) i;
ANALYZE t1, t2;

\echo ''
\echo '--- Test 1: Query with IN subquery ---'
\echo ''

SET pg_outline.mode = 'auto';
SET pg_outline.display_hints = true;
SET client_min_messages = 'NOTICE';

-- Execute query to generate outline
EXPLAIN SELECT * FROM t1 WHERE c1 IN (SELECT c2 FROM t2 WHERE c2 < 10);

\echo ''
\echo '--- The generated SQL with positioned hints should show: ---'
\echo '--- Main hints after main SELECT: SELECT /*+ hints */ * FROM t1 ---'
\echo '--- Sublink hints after subquery SELECT: (SELECT /*+ hints */ c2 FROM t2) ---'
\echo ''

\echo ''
\echo '--- Test 2: Query with multiple sublinks ---'
\echo ''

EXPLAIN SELECT * FROM t1
WHERE c1 IN (SELECT c2 FROM t2 WHERE c2 < 20)
  AND c1 NOT IN (SELECT c2 FROM t2 WHERE c2 > 80);

\echo ''
\echo '--- Each subquery should have its hint after its own SELECT keyword ---'
\echo ''

\echo ''
\echo '==========================================='
\echo 'Expected Output Format'
\echo '==========================================='
\echo ''
\echo 'For query: select * from t1 where c1 in (select c2 from t2)'
\echo ''
\echo 'Correct placement:'
\echo '  explain select /*+ main hints */ * from t1 where c1 in (select /*+ sublink_0 hints */ c2 from t2);'
\echo ''
\echo 'Incorrect (old behavior):'
\echo '  explain select /*+ all hints including sublink */ * from t1 where c1 in (select c2 from t2);'
\echo ''
