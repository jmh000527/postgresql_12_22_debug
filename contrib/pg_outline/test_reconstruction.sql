-- Test to verify sublink hint placement in reconstructed SQL
\echo '========================================='
\echo 'Sublink Hint Placement Verification'
\echo '========================================='
\echo ''

-- First, create the outline manually with both main and sublink hints
SELECT pg_outline_create(
    'test_sublink_outline',
    'SELECT * FROM t1 WHERE c1 IN (SELECT c2 FROM t2 WHERE c2 < ?);',
    E'[main] SeqScan(t1) HashJoin(t1 t2) Leading((t1 t2))\n[sublink_0] SeqScan(t2)'
);

\echo ''
\echo '--- Now execute the same query ---'
\echo 'This should show the reconstructed SQL with hints in correct positions'
\echo ''

SET pg_outline.mode = 'auto';
SET client_min_messages = 'NOTICE';

-- Execute the query again - should trigger the "outline already exists" notice
-- with reconstructed SQL showing hints in correct positions
EXPLAIN SELECT * FROM t1 WHERE c1 IN (SELECT c2 FROM t2 WHERE c2 < 5);

\echo ''
\echo '========================================='
\echo 'Check the NOTICE message above'
\echo 'It should show:'
\echo '  SELECT /*+ main_hints */ * FROM t1'
\echo '    WHERE c1 IN (SELECT /*+ sublink_hints */ c2 FROM t2 ...)'
\echo '========================================='
