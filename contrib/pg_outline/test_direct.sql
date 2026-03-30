-- Direct test of hint reconstruction function
\echo '========================================='
\echo 'Direct Test: Reconstruct SQL with Sublink Hints'
\echo '========================================='

-- Create a simple query with IN subquery
\echo ''
\echo '--- Generate outline for query with subquery ---'

SET pg_outline.mode = 'auto';
SET client_min_messages = 'NOTICE';

-- This query structure will generate outline data
EXPLAIN SELECT * FROM t1 WHERE c1 > 5 AND c1 IN (SELECT c2 FROM t2 WHERE c2 > 3);

\echo ''
\echo '--- Check the outline data above ---'
\echo 'Look for [sublink_N] entries in the outline'
