--
-- OUTLINE_HINTS
-- Test the outline hint system for query plan control
--

-- Create test tables
CREATE TABLE outline_test1 (
    id int PRIMARY KEY,
    name text,
    value int
);

CREATE TABLE outline_test2 (
    id int PRIMARY KEY,
    ref_id int,
    data text
);

CREATE TABLE outline_test3 (
    id int PRIMARY KEY,
    category text,
    score int
);

-- Create indexes
CREATE INDEX idx_test1_value ON outline_test1(value);
CREATE INDEX idx_test2_ref ON outline_test2(ref_id);
CREATE INDEX idx_test3_category ON outline_test3(category);

-- Insert test data
INSERT INTO outline_test1 SELECT i, 'name_' || i, i * 10 FROM generate_series(1, 1000) i;
INSERT INTO outline_test2 SELECT i, (i % 100) + 1, 'data_' || i FROM generate_series(1, 5000) i;
INSERT INTO outline_test3 SELECT i, 'cat_' || (i % 10), i * 5 FROM generate_series(1, 500) i;

-- Analyze tables for proper statistics
ANALYZE outline_test1;
ANALYZE outline_test2;
ANALYZE outline_test3;

--
-- Test 1: SeqScan hint
--
SELECT pg_create_outline(
    'test_seqscan',
    'SELECT * FROM outline_test1 WHERE value < $1',
    'SeqScan(outline_test1)'
);

-- Verify the outline was created
SELECT outlinename, outlineenabled FROM pg_outline WHERE outlinename = 'test_seqscan';

--
-- Test 2: IndexScan hint
--
SELECT pg_create_outline(
    'test_indexscan',
    'SELECT * FROM outline_test1 WHERE value < $1',
    'IndexScan(outline_test1 idx_test1_value)'
);

SELECT outlinename, outlineenabled FROM pg_outline WHERE outlinename = 'test_indexscan';

--
-- Test 3: NoSeqScan hint
--
SELECT pg_create_outline(
    'test_noseqscan',
    'SELECT * FROM outline_test1 WHERE value = $1',
    'NoSeqScan(outline_test1)'
);

SELECT outlinename, outlineenabled FROM pg_outline WHERE outlinename = 'test_noseqscan';

--
-- Test 4: Join method hints
--
SELECT pg_create_outline(
    'test_hashjoin',
    'SELECT * FROM outline_test1 t1 JOIN outline_test2 t2 ON t1.id = t2.ref_id WHERE t1.value > $1',
    'HashJoin(outline_test1 outline_test2)'
);

SELECT pg_create_outline(
    'test_nestloop',
    'SELECT * FROM outline_test1 t1 JOIN outline_test2 t2 ON t1.id = t2.ref_id WHERE t1.id < $1',
    'NestLoop(outline_test1 outline_test2)'
);

SELECT pg_create_outline(
    'test_mergejoin',
    'SELECT * FROM outline_test1 t1 JOIN outline_test2 t2 ON t1.id = t2.id WHERE t1.value > $1',
    'MergeJoin(outline_test1 outline_test2)'
);

--
-- Test 5: Rows hint - Override row estimates
--
SELECT pg_create_outline(
    'test_rows_single',
    'SELECT * FROM outline_test1 WHERE value < $1',
    'Rows(outline_test1 100)'
);

SELECT pg_create_outline(
    'test_rows_join',
    'SELECT * FROM outline_test1 t1 JOIN outline_test2 t2 ON t1.id = t2.ref_id',
    'Rows(outline_test1 outline_test2 5000)'
);

--
-- Test 6: Parallel hint
--
SELECT pg_create_outline(
    'test_parallel',
    'SELECT COUNT(*) FROM outline_test2 WHERE ref_id > $1',
    'Parallel(outline_test2 4)'
);

--
-- Test 7: NoParallel hint
--
SELECT pg_create_outline(
    'test_noparallel',
    'SELECT COUNT(*) FROM outline_test2 WHERE ref_id < $1',
    'NoParallel(outline_test2)'
);

--
-- Test 8: Set hint - GUC parameter override
--
SELECT pg_create_outline(
    'test_set_workmen',
    'SELECT * FROM outline_test1 ORDER BY value',
    'Set(work_mem 128MB)'
);

SELECT pg_create_outline(
    'test_set_random_page_cost',
    'SELECT * FROM outline_test1 WHERE value < $1',
    'Set(random_page_cost 1.5)'
);

--
-- Test 9: Multiple hints combined
--
SELECT pg_create_outline(
    'test_combined_hints',
    'SELECT * FROM outline_test1 t1 JOIN outline_test2 t2 ON t1.id = t2.ref_id WHERE t1.value > $1',
    'HashJoin(outline_test1 outline_test2)
Rows(outline_test1 50)
Rows(outline_test2 1000)
Set(random_page_cost 2.0)'
);

--
-- Test 10: Leading hint (simple syntax)
--
SELECT pg_create_outline(
    'test_leading_simple',
    'SELECT * FROM outline_test1 t1 JOIN outline_test2 t2 ON t1.id = t2.ref_id JOIN outline_test3 t3 ON t2.id = t3.id',
    'Leading(outline_test1 outline_test2 outline_test3)'
);

--
-- Test 11: Leading hint (nested syntax)
--
SELECT pg_create_outline(
    'test_leading_nested',
    'SELECT * FROM outline_test1 t1 JOIN outline_test2 t2 ON t1.id = t2.ref_id JOIN outline_test3 t3 ON t1.id = t3.id',
    'Leading((outline_test1 outline_test2) outline_test3)'
);

--
-- Test 12: Inline hints
--
-- Test that inline hints are recognized
SELECT /*+ SeqScan(outline_test1) */ * FROM outline_test1 WHERE value < 100;

SELECT /*+ IndexScan(outline_test1 idx_test1_value) */ * FROM outline_test1 WHERE value < 50;

SELECT /*+ HashJoin(outline_test1 outline_test2) Rows(outline_test1 100) */
    t1.*, t2.*
FROM outline_test1 t1
JOIN outline_test2 t2 ON t1.id = t2.ref_id
WHERE t1.value > 500;

--
-- Test 13: OceanBase-style outline data format
--
SELECT /*+
BEGIN_OUTLINE_DATA
IndexScan(outline_test1 idx_test1_value)
Rows(outline_test1 50)
END_OUTLINE_DATA
*/ * FROM outline_test1 WHERE value < 200;

--
-- Test 14: Outline management functions
--
-- List all outlines
SELECT outlinename, outlineenabled FROM pg_outline ORDER BY outlinename;

-- Disable an outline
SELECT pg_enable_outline('test_seqscan', false);
SELECT outlinename, outlineenabled FROM pg_outline WHERE outlinename = 'test_seqscan';

-- Re-enable an outline
SELECT pg_enable_outline('test_seqscan', true);
SELECT outlinename, outlineenabled FROM pg_outline WHERE outlinename = 'test_seqscan';

-- Update an outline's hints
SELECT pg_update_outline('test_seqscan', 'NoSeqScan(outline_test1)');

-- Drop an outline
SELECT pg_drop_outline('test_seqscan');
SELECT COUNT(*) FROM pg_outline WHERE outlinename = 'test_seqscan';

--
-- Test 15: Error cases
--
-- Try to create outline with invalid hint syntax
SELECT pg_create_outline(
    'test_invalid_hint',
    'SELECT * FROM outline_test1',
    'InvalidHint(outline_test1)'
);

-- Try to create duplicate outline name
SELECT pg_create_outline(
    'test_indexscan',
    'SELECT * FROM outline_test1 WHERE id = $1',
    'IndexScan(outline_test1)'
);

-- Try to drop non-existent outline
SELECT pg_drop_outline('nonexistent_outline');

-- Try to enable non-existent outline
SELECT pg_enable_outline('nonexistent_outline', true);

--
-- Test 16: Auto-generated outlines
--
-- Enable outline recording mode
SET outline.recording_mode = on;

-- Execute queries to auto-generate outlines
SELECT * FROM outline_test1 WHERE value < 500;
SELECT COUNT(*) FROM outline_test2 WHERE ref_id > 50;

-- Disable recording mode
SET outline.recording_mode = off;

-- Check that outlines were auto-generated
SELECT COUNT(*) FROM pg_outline WHERE outlinename LIKE 'auto_outline_%';

--
-- Test 17: Outline display setting
--
-- Enable hint display to see which hints are being applied
SET outline.display_hints = on;

-- Execute a query with an outline
SELECT * FROM outline_test1 WHERE value < 100;

-- Disable hint display
SET outline.display_hints = off;

--
-- Cleanup
--
-- Drop all test outlines
SELECT pg_drop_outline(outlinename) FROM pg_outline WHERE outlinename LIKE 'test_%' OR outlinename LIKE 'auto_outline_%';

-- Drop test tables
DROP TABLE outline_test3;
DROP TABLE outline_test2;
DROP TABLE outline_test1;
