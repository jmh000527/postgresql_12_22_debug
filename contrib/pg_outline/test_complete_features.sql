-- Comprehensive test for pg_outline complete features
-- Tests: 1) Multi-relation join hints, 2) Leading hints, 3) Query parameterization

\echo '========================================='
\echo 'pg_outline Complete Features Test'
\echo '========================================='
\echo ''

-- Setup
DROP EXTENSION IF EXISTS pg_outline CASCADE;
CREATE EXTENSION pg_outline;

-- Create test tables
DROP TABLE IF EXISTS t1, t2, t3 CASCADE;
CREATE TABLE t1 (id INTEGER PRIMARY KEY, value INTEGER, name TEXT);
CREATE TABLE t2 (id INTEGER PRIMARY KEY, t1_id INTEGER, description TEXT);
CREATE TABLE t3 (id INTEGER PRIMARY KEY, t2_id INTEGER, status TEXT);

-- Create indexes
CREATE INDEX idx_t1_value ON t1(value);
CREATE INDEX idx_t2_t1_id ON t2(t1_id);
CREATE INDEX idx_t3_t2_id ON t3(t2_id);

-- Insert test data
INSERT INTO t1 SELECT i, i * 10, 'name_' || i FROM generate_series(1, 1000) i;
INSERT INTO t2 SELECT i, (i % 1000) + 1, 'desc_' || i FROM generate_series(1, 5000) i;
INSERT INTO t3 SELECT i, (i % 5000) + 1, 'status_' || i FROM generate_series(1, 10000) i;

ANALYZE t1, t2, t3;

\echo ''
\echo '========================================='
\echo 'Test 1: Query Parameterization'
\echo '========================================='
\echo ''

-- Enable auto mode to capture plan
SET pg_outline.enabled = true;
SET pg_outline.mode = 'auto';
SET pg_outline.display_hints = true;
SET client_min_messages = 'NOTICE';

\echo '--- Run query with literal value 100 ---'
EXPLAIN SELECT * FROM t1 WHERE id < 100;

\echo ''
\echo '--- Manually create outline with parameterized pattern ---'
SELECT pg_outline_create(
    'test_parameterization',
    'select * from t1 where id < ?',  -- Parameterized pattern
    '[main] SeqScan(t1)'
);

\echo ''
\echo '--- Switch to manual mode ---'
SET pg_outline.mode = 'manual';
SET client_min_messages = 'DEBUG1';

\echo ''
\echo '--- Run query with different literal value (200) - should match same outline ---'
EXPLAIN SELECT * FROM t1 WHERE id < 200;

\echo ''
\echo '--- Run query with different literal value (500) - should also match ---'
EXPLAIN SELECT * FROM t1 WHERE id < 500;

\echo ''
\echo '========================================='
\echo 'Test 2: Multi-relation Join Hints'
\echo '========================================='
\echo ''

SET client_min_messages = 'NOTICE';
SET pg_outline.mode = 'auto';

\echo '--- Run 3-way join query ---'
EXPLAIN SELECT *
FROM t1
JOIN t2 ON t1.id = t2.t1_id
JOIN t3 ON t2.id = t3.t2_id
WHERE t1.value > 50;

\echo ''
\echo '--- Create outline with multi-relation join hint ---'
SELECT pg_outline_create(
    'test_multijoin',
    'select * from t1 join t2 on t1.id = t2.t1_id join t3 on t2.id = t3.t2_id where t1.value > ?',
    '[main] HashJoin(t1 t2 t3) [main] SeqScan(t1)'
);

SET pg_outline.mode = 'manual';
SET client_min_messages = 'DEBUG1';

\echo ''
\echo '--- Verify multi-relation hint is parsed ---'
EXPLAIN SELECT *
FROM t1
JOIN t2 ON t1.id = t2.t1_id
JOIN t3 ON t2.id = t3.t2_id
WHERE t1.value > 100;

\echo ''
\echo '========================================='
\echo 'Test 3: Leading Hints (Join Order)'
\echo '========================================='
\echo ''

SET client_min_messages = 'NOTICE';
SET pg_outline.mode = 'auto';

\echo '--- Run 3-way join to see natural join order ---'
EXPLAIN SELECT *
FROM t1
JOIN t2 ON t1.id = t2.t1_id
JOIN t3 ON t2.id = t3.t2_id
WHERE t1.id < 10;

\echo ''
\echo '--- Create outline with Leading hint ---'
SELECT pg_outline_create(
    'test_leading',
    'select * from t1 join t2 on t1.id = t2.t1_id join t3 on t2.id = t3.t2_id where t1.id < ?',
    '[main] Leading((t1 t2) t3)'
);

SET pg_outline.mode = 'manual';
SET client_min_messages = 'DEBUG1';

\echo ''
\echo '--- Apply outline with Leading hint ---'
EXPLAIN SELECT *
FROM t1
JOIN t2 ON t1.id = t2.t1_id
JOIN t3 ON t2.id = t3.t2_id
WHERE t1.id < 20;

\echo ''
\echo '========================================='
\echo 'Test 4: Combined Features'
\echo '========================================='
\echo ''

\echo '--- Create outline with all hint types ---'
SELECT pg_outline_create(
    'test_combined',
    'select * from t1 join t2 on t1.id = t2.t1_id join t3 on t2.id = t3.t2_id where t1.value > ? and t1.id < ?',
    '[main] SeqScan(t1) [main] IndexScan(t2) [main] HashJoin(t1 t2 t3) [main] Leading((t1 t2) t3)'
);

\echo ''
\echo '--- Apply combined outline with different parameters ---'
EXPLAIN SELECT *
FROM t1
JOIN t2 ON t1.id = t2.t1_id
JOIN t3 ON t2.id = t3.t2_id
WHERE t1.value > 200 AND t1.id < 500;

\echo ''
\echo '========================================='
\echo 'Summary: List All Outlines'
\echo '========================================='
\echo ''

SET client_min_messages = 'NOTICE';
SELECT outline_name, enabled,
       substring(hint_string, 1, 50) || '...' as hints,
       created_at
FROM pg_outline_data
ORDER BY created_at;

\echo ''
\echo '========================================='
\echo 'Test Complete'
\echo '========================================='
