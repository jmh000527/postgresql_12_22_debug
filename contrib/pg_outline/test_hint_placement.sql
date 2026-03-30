-- Test for correct hint placement after SELECT keyword

\echo '========================================='
\echo 'Test: Hint Placement After SELECT'
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
\echo '--- Test 1: Simple SELECT with WHERE clause ---'
\echo ''

SET pg_outline.mode = 'auto';
SET pg_outline.display_hints = true;

-- Execute query to generate outline
EXPLAIN SELECT * FROM t1 WHERE c1 = 50;

\echo ''
\echo '--- Test 2: SELECT with IN subquery ---'
\echo ''

EXPLAIN SELECT * FROM t1 WHERE c1 IN (SELECT c2 FROM t2 WHERE c2 < 10);

\echo ''
\echo '--- Test 3: Check the reconstructed SQL format ---'
\echo 'The hint should be placed after SELECT keyword like:'
\echo 'SELECT /*+ hints */ * FROM ...'
\echo 'NOT before SELECT like: /*+ hints */ SELECT ...'
\echo ''

-- The NOTICE message should show:
-- "SQL with outline hints inserted at original positions:"
-- followed by SQL in the format: SELECT /*+ ... */ * FROM ...

\echo ''
\echo '========================================='
\echo 'Expected Output Format'
\echo '========================================='
\echo ''
\echo 'Correct:   SELECT /*+ HashJoin(t1 t2) Leading((t1 t2)) */ * FROM ...'
\echo 'Incorrect: /*+ HashJoin(t1 t2) Leading((t1 t2)) */ SELECT * FROM ...'
\echo ''
