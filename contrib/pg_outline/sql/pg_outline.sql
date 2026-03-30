-- Test pg_outline extension

-- Load the extension
CREATE EXTENSION pg_outline;

-- Check that the extension is loaded
SELECT extname, extversion FROM pg_extension WHERE extname = 'pg_outline';

-- Check GUC parameters
SHOW pg_outline.enabled;
SHOW pg_outline.display_hints;
SHOW pg_outline.mode;

-- Create test tables
CREATE TABLE test_table1 (
    id INTEGER PRIMARY KEY,
    name TEXT,
    value INTEGER
);

CREATE TABLE test_table2 (
    id INTEGER PRIMARY KEY,
    table1_id INTEGER REFERENCES test_table1(id),
    description TEXT
);

-- Insert test data
INSERT INTO test_table1 (id, name, value)
SELECT i, 'name_' || i, i * 10
FROM generate_series(1, 100) i;

INSERT INTO test_table2 (id, table1_id, description)
SELECT i, (i % 100) + 1, 'desc_' || i
FROM generate_series(1, 1000) i;

-- Run a test query to generate outline
SELECT t1.id, t1.name, t2.description
FROM test_table1 t1
JOIN test_table2 t2 ON t1.id = t2.table1_id
WHERE t1.value > 500
ORDER BY t1.id
LIMIT 10;

-- Test outline functions
SELECT pg_outline_create('test_outline_1',
                         'SELECT * FROM test_table1 WHERE value > 100',
                         'SeqScan(test_table1)');

-- List outlines
SELECT * FROM pg_outline_list();

-- Check outline data view
SELECT * FROM pg_outline_enabled;

-- Test disabling outline
SELECT pg_outline_disable('test_outline_1');

-- Verify it's disabled
SELECT * FROM pg_outline_enabled;

-- Test enabling outline
SELECT pg_outline_enable('test_outline_1');

-- Verify it's enabled
SELECT * FROM pg_outline_enabled;

-- Test dropping outline
SELECT pg_outline_drop('test_outline_1');

-- Verify it's dropped
SELECT * FROM pg_outline_list();

-- Clean up
DROP TABLE test_table2;
DROP TABLE test_table1;

-- Drop the extension
DROP EXTENSION pg_outline CASCADE;
