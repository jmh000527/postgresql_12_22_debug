-- Test sublink hint placement with EXISTS subquery
-- EXISTS subqueries are more likely to preserve sublink structure

\echo '========================================='
\echo 'Test: Sublink Hint Placement with EXISTS'
\echo '========================================='
\echo ''

-- First create a stored outline with sublink hints
SELECT pg_outline_create(
    'test_exists_outline',
    'SELECT * FROM t1 WHERE EXISTS (SELECT ? FROM t2 WHERE t2.c2 = t1.c1);',
    E'[main] SeqScan(t1)\n[sublink_0] SeqScan(t2)'
);

\echo ''
\echo '--- Execute query to trigger outline ---'
\echo ''

SET pg_outline.mode = 'auto';
SET client_min_messages = 'NOTICE';

-- This query should match the stored outline
EXPLAIN SELECT * FROM t1 WHERE EXISTS (SELECT 1 FROM t2 WHERE t2.c2 = t1.c1);

\echo ''
\echo '========================================='
\echo 'Expected: NOTICE showing reconstructed SQL'
\echo 'with hints placed after SELECT in both'
\echo 'main query and subquery'
\echo '========================================='
