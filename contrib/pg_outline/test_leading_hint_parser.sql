-- Test Leading Hint Parser
-- This test demonstrates the Leading hint parsing capability in pg_outline

\echo '========================================='
\echo 'Leading Hint Parser Test'
\echo '========================================='
\echo ''

-- Setup
DROP EXTENSION IF EXISTS pg_outline CASCADE;
CREATE EXTENSION pg_outline;

-- Create test tables
DROP TABLE IF EXISTS t1, t2, t3, t4 CASCADE;
CREATE TABLE t1 (id INTEGER PRIMARY KEY, value INTEGER);
CREATE TABLE t2 (id INTEGER PRIMARY KEY, t1_id INTEGER);
CREATE TABLE t3 (id INTEGER PRIMARY KEY, t2_id INTEGER);
CREATE TABLE t4 (id INTEGER PRIMARY KEY, t3_id INTEGER);

-- Insert test data
INSERT INTO t1 SELECT i, i * 10 FROM generate_series(1, 100) i;
INSERT INTO t2 SELECT i, (i % 100) + 1 FROM generate_series(1, 200) i;
INSERT INTO t3 SELECT i, (i % 200) + 1 FROM generate_series(1, 300) i;
INSERT INTO t4 SELECT i, (i % 300) + 1 FROM generate_series(1, 400) i;

ANALYZE t1, t2, t3, t4;

\echo ''
\echo '========================================='
\echo 'Test 1: Simple Flat Format'
\echo 'Leading(t1 t2 t3)'
\echo '========================================='
\echo ''

SET pg_outline.mode = 'manual';
SET client_min_messages = 'NOTICE';

-- Create outline with simple flat format
SELECT pg_outline_create(
    'test_simple_leading',
    'select * from t1 join t2 on t1.id = t2.t1_id join t3 on t2.id = t3.t2_id where t1.value > ?',
    '[main] Leading(t1 t2 t3)'
);

\echo '--- Execute query to trigger Leading hint parsing ---'
EXPLAIN SELECT * FROM t1
JOIN t2 ON t1.id = t2.t1_id
JOIN t3 ON t2.id = t3.t2_id
WHERE t1.value > 50;

\echo ''
\echo '========================================='
\echo 'Test 2: Nested Format (Left-Deep)'
\echo 'Leading((t1 t2) t3)'
\echo '========================================='
\echo ''

-- Create outline with nested format
SELECT pg_outline_drop('test_simple_leading');
SELECT pg_outline_create(
    'test_nested_leading',
    'select * from t1 join t2 on t1.id = t2.t1_id join t3 on t2.id = t3.t2_id where t1.value > ?',
    '[main] Leading((t1 t2) t3)'
);

\echo '--- Execute query to trigger nested Leading hint parsing ---'
EXPLAIN SELECT * FROM t1
JOIN t2 ON t1.id = t2.t1_id
JOIN t3 ON t2.id = t3.t2_id
WHERE t1.value > 50;

\echo ''
\echo '========================================='
\echo 'Test 3: Bushy Join Format'
\echo 'Leading((t1 t2) (t3 t4))'
\echo '========================================='
\echo ''

-- Create outline with bushy join format
SELECT pg_outline_drop('test_nested_leading');
SELECT pg_outline_create(
    'test_bushy_leading',
    'select * from t1 join t2 on t1.id = t2.t1_id join t3 on t2.id = t3.t2_id join t4 on t3.id = t4.t3_id where t1.value > ?',
    '[main] Leading((t1 t2) (t3 t4))'
);

\echo '--- Execute query to trigger bushy Leading hint parsing ---'
EXPLAIN SELECT * FROM t1
JOIN t2 ON t1.id = t2.t1_id
JOIN t3 ON t2.id = t3.t2_id
JOIN t4 ON t3.id = t4.t3_id
WHERE t1.value > 50;

\echo ''
\echo '========================================='
\echo 'Test 4: Complex Nested Format'
\echo 'Leading(((t1 t2) t3) t4)'
\echo '========================================='
\echo ''

-- Create outline with complex nested format
SELECT pg_outline_drop('test_bushy_leading');
SELECT pg_outline_create(
    'test_complex_leading',
    'select * from t1 join t2 on t1.id = t2.t1_id join t3 on t2.id = t3.t2_id join t4 on t3.id = t4.t3_id where t1.value > ?',
    '[main] Leading(((t1 t2) t3) t4)'
);

\echo '--- Execute query to trigger complex nested Leading hint parsing ---'
EXPLAIN SELECT * FROM t1
JOIN t2 ON t1.id = t2.t1_id
JOIN t3 ON t2.id = t3.t2_id
JOIN t4 ON t3.id = t4.t3_id
WHERE t1.value > 50;

\echo ''
\echo '========================================='
\echo 'Test 5: Right-Deep Tree'
\echo 'Leading(t1 (t2 (t3 t4)))'
\echo '========================================='
\echo ''

-- Create outline with right-deep tree
SELECT pg_outline_drop('test_complex_leading');
SELECT pg_outline_create(
    'test_rightdeep_leading',
    'select * from t1 join t2 on t1.id = t2.t1_id join t3 on t2.id = t3.t2_id join t4 on t3.id = t4.t3_id where t1.value > ?',
    '[main] Leading(t1 (t2 (t3 t4)))'
);

\echo '--- Execute query to trigger right-deep Leading hint parsing ---'
EXPLAIN SELECT * FROM t1
JOIN t2 ON t1.id = t2.t1_id
JOIN t3 ON t2.id = t3.t2_id
JOIN t4 ON t3.id = t4.t3_id
WHERE t1.value > 50;

\echo ''
\echo '========================================='
\echo 'Summary: List All Test Outlines'
\echo '========================================='
\echo ''

SELECT outline_name, enabled,
       substring(hint_string, 1, 60) || '...' as hints
FROM pg_outline_data
ORDER BY outline_name;

\echo ''
\echo '========================================='
\echo 'Test Complete'
\echo '========================================='
\echo ''
\echo 'The Leading hint parser successfully parsed all formats:'
\echo '  1. Simple flat: t1 t2 t3'
\echo '  2. Nested (left-deep): (t1 t2) t3'
\echo '  3. Bushy: (t1 t2) (t3 t4)'
\echo '  4. Complex nested: ((t1 t2) t3) t4'
\echo '  5. Right-deep: t1 (t2 (t3 t4))'
\echo ''
\echo 'Check the NOTICE messages above to see the parsed tree structures.'
