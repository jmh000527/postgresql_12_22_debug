-- Test post_parse_analyze_hook for sublink hint application
-- This test verifies that hints are applied to sublinks BEFORE optimization

\echo '========================================='
\echo 'Testing Post-Parse Hook for Sublink Hints'
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
\echo '--- Step 1: Generate outline in auto mode ---'
\echo ''

SET pg_outline.mode = 'auto';
SET pg_outline.display_hints = true;
SET client_min_messages = 'DEBUG1';

-- Execute query to generate outline with sublink
EXPLAIN SELECT * FROM t1 WHERE c1 IN (SELECT c2 FROM t2 WHERE c2 < 10);

\echo ''
\echo '--- Step 2: Switch to manual mode and test outline matching ---'
\echo ''

SET pg_outline.mode = 'manual';

-- Execute the same query pattern - should match stored outline
-- The post_parse_analyze_hook should:
-- 1. Detect the matching outline
-- 2. Display the reconstructed SQL with positioned hints
-- 3. Apply hints to the Query tree (including sublinks) BEFORE optimization
EXPLAIN SELECT * FROM t1 WHERE c1 IN (SELECT c2 FROM t2 WHERE c2 < 5);

\echo ''
\echo '--- Expected Output ---'
\echo 'You should see:'
\echo '1. DEBUG1 message: "pg_outline: found stored outline, applying hints to query tree"'
\echo '2. DEBUG1 message: "pg_outline: traversing query tree to apply hints to sublinks"'
\echo '3. DEBUG1 messages showing hints being applied to "main" and "sublink_0" queries'
\echo '4. NOTICE showing the reconstructed SQL with hints at correct positions'
\echo ''

\echo ''
\echo '--- Step 3: Test with multiple sublinks ---'
\echo ''

-- First generate outline with multiple sublinks
SET pg_outline.mode = 'auto';
EXPLAIN SELECT * FROM t1
WHERE c1 IN (SELECT c2 FROM t2 WHERE c2 < 20)
  AND c1 NOT IN (SELECT c2 FROM t2 WHERE c2 > 80);

-- Now test matching
SET pg_outline.mode = 'manual';
EXPLAIN SELECT * FROM t1
WHERE c1 IN (SELECT c2 FROM t2 WHERE c2 < 15)
  AND c1 NOT IN (SELECT c2 FROM t2 WHERE c2 > 85);

\echo ''
\echo '========================================='
\echo 'Test Complete'
\echo '========================================='
