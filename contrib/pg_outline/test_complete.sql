-- Complete test for post_parse_analyze_hook sublink hint application

\echo '========================================='
\echo 'Complete Sublink Hint Application Test'
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
\echo '--- Step 1: Generate and store outline ---'
\echo ''

SET pg_outline.mode = 'auto';
SET pg_outline.display_hints = true;
SET client_min_messages = 'NOTICE';

-- Execute query to generate outline
EXPLAIN SELECT * FROM t1 WHERE c1 IN (SELECT c2 FROM t2 WHERE c2 < 10);

-- Extract the fingerprint from the notice message and create the outline manually
SELECT pg_outline_create(
    'test_sublink_outline',
    'SELECT * FROM t1 WHERE c1 IN (SELECT c2 FROM t2 WHERE c2 < ?)',
    '[main] HashJoin(t1 t2)
[main] Leading((t1 t2))
[main] SeqScan(t1)
[main] SeqScan(t2)'
);

\echo ''
\echo '--- Step 2: Verify outline was stored ---'
\echo ''

SELECT outline_name, query_pattern FROM pg_outline_data WHERE outline_name = 'test_sublink_outline';

\echo ''
\echo '--- Step 3: Switch to manual mode and test matching ---'
\echo ''

SET pg_outline.mode = 'manual';
SET client_min_messages = 'DEBUG1';

-- This query should match the stored outline
-- Expected: hints will be applied via post_parse_analyze_hook
\echo 'Executing query that should match outline...'
EXPLAIN SELECT * FROM t1 WHERE c1 IN (SELECT c2 FROM t2 WHERE c2 < 5);

\echo ''
\echo '--- Step 4: Test with EXISTS subquery ---'
\echo ''

SET pg_outline.mode = 'auto';
SET client_min_messages = 'NOTICE';

EXPLAIN SELECT * FROM t1 WHERE EXISTS (SELECT 1 FROM t2 WHERE t2.c2 = t1.c1 AND c2 < 20);

-- Store this outline
SELECT pg_outline_create(
    'test_exists_outline',
    'SELECT * FROM t1 WHERE EXISTS (SELECT 1 FROM t2 WHERE t2.c2 = t1.c1 AND c2 < ?)',
    '[main] HashJoin(t1 t2)
[main] Leading((t1 t2))
[main] SeqScan(t1)
[main] SeqScan(t2)'
);

SET pg_outline.mode = 'manual';
SET client_min_messages = 'DEBUG1';

\echo 'Executing EXISTS query...'
EXPLAIN SELECT * FROM t1 WHERE EXISTS (SELECT 1 FROM t2 WHERE t2.c2 = t1.c1 AND c2 < 15);

\echo ''
\echo '========================================='
\echo 'Test Summary'
\echo '========================================='
\echo 'If working correctly, you should see:'
\echo '1. Outlines created successfully'
\echo '2. DEBUG messages showing post_parse_analyze_hook executing'
\echo '3. Messages about applying hints to query tree'
\echo '4. NOTICE showing reconstructed SQL with positioned hints'
\echo '========================================='
