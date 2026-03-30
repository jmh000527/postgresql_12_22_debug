-- contrib/pg_outline/sql/pg_outline.sql
--
-- Test cases for pg_outline extension

-- Create extension
CREATE EXTENSION pg_outline;

-- Verify schema and tables exist
SELECT nspname FROM pg_namespace WHERE nspname = 'pg_outline';
SELECT tablename FROM pg_tables WHERE schemaname = 'pg_outline' ORDER BY tablename;

-- Test configuration
SELECT * FROM pg_outline.config ORDER BY param_name;

-- Test basic outline creation
SELECT pg_outline.create_outline(
    'test_outline1',
    'SELECT * FROM pg_class WHERE relname = ''pg_outline''',
    'SeqScan(pg_class)',
    'pg_catalog',
    'Test outline 1'
);

-- Verify outline was created
SELECT id, name, schema_name, enabled FROM pg_outline.outlines WHERE name = 'test_outline1';

-- Test outline with join hints
SELECT pg_outline.create_outline(
    'test_outline2',
    'SELECT * FROM pg_class c JOIN pg_namespace n ON c.relnamespace = n.oid',
    'HashJoin(c n) IndexScan(pg_class)',
    'pg_catalog',
    'Test outline with join hints'
);

-- Test outline enable/disable
SELECT pg_outline.disable_outline('test_outline1', 'pg_catalog');
SELECT enabled FROM pg_outline.outlines WHERE name = 'test_outline1';

SELECT pg_outline.enable_outline('test_outline1', 'pg_catalog');
SELECT enabled FROM pg_outline.outlines WHERE name = 'test_outline1';

-- Test active outlines view
SELECT name, schema_name FROM pg_outline.active_outlines ORDER BY name;

-- Test statistics
SELECT * FROM pg_outline.get_statistics();

-- Test duplicate outline creation (should fail)
SELECT pg_outline.create_outline(
    'test_outline1',
    'SELECT 1',
    'SeqScan(t)',
    'pg_catalog',
    'Duplicate'
);

-- Test outline drop
SELECT pg_outline.drop_outline('test_outline1', 'pg_catalog');
SELECT COUNT(*) FROM pg_outline.outlines WHERE name = 'test_outline1';

SELECT pg_outline.drop_outline('test_outline2', 'pg_catalog');

-- Test with NULL hints
SELECT pg_outline.create_outline(
    'test_outline3',
    'SELECT * FROM pg_type',
    NULL,
    'pg_catalog',
    'Outline with no hints'
);

SELECT name, hints FROM pg_outline.outlines WHERE name = 'test_outline3';
SELECT pg_outline.drop_outline('test_outline3', 'pg_catalog');

-- Test GUC parameters
SHOW pg_outline.enabled;
SHOW pg_outline.auto_generate;
SHOW pg_outline.display_hints;
SHOW pg_outline.log_level;
SHOW pg_outline.match_mode;

-- Test setting GUC parameters
SET pg_outline.enabled = false;
SHOW pg_outline.enabled;
SET pg_outline.enabled = true;

SET pg_outline.match_mode = 'normalized';
SHOW pg_outline.match_mode;

-- Test inline hints (just verify syntax doesn't break)
SELECT /*+ SeqScan(pg_class) */ relname FROM pg_class WHERE relname = 'pg_type';

-- Test hint injection function
-- Test 1: Simple SELECT with simple hints
SELECT pg_outline.inject_hints(
    'SELECT * FROM users WHERE id = 1',
    'SeqScan(users)'
);

-- Test 2: Simple SELECT with [main] prefix
SELECT pg_outline.inject_hints(
    'SELECT * FROM users WHERE id = 1',
    '[main]IndexScan(users user_idx)'
);

-- Test 3: WITH clause (CTE) with multi-query hints
SELECT pg_outline.inject_hints(
    'WITH data AS (SELECT * FROM users) SELECT * FROM data',
    '[cte_data]SeqScan(users) [main]HashJoin(data)'
);

-- Test 4: Multiple CTEs with different hints
SELECT pg_outline.inject_hints(
    'WITH cte1 AS (SELECT * FROM t1), cte2 AS (SELECT * FROM t2) SELECT * FROM cte1 JOIN cte2 ON cte1.id = cte2.id',
    '[cte_cte1]IndexScan(t1) [cte_cte2]SeqScan(t2) [main]HashJoin(cte1 cte2)'
);

-- Test 5: Mixed prefix and non-prefix hints
SELECT pg_outline.inject_hints(
    'SELECT * FROM orders',
    'SeqScan(orders) Leading(orders customers)'
);

-- Test 6: Only CTE hints, no main hints
SELECT pg_outline.inject_hints(
    'WITH active_users AS (SELECT * FROM users WHERE active = true) SELECT * FROM active_users',
    '[cte_active_users]IndexScan(users active_idx)'
);

-- Test 7: Empty hints
SELECT pg_outline.inject_hints(
    'SELECT * FROM users',
    ''
);

-- Test 8: Complex query with existing comments
SELECT pg_outline.inject_hints(
    '-- This is a comment
SELECT * FROM users',
    'SeqScan(users)'
);

-- Cleanup
DROP EXTENSION pg_outline CASCADE;

-- Verify cleanup
SELECT COUNT(*) FROM pg_namespace WHERE nspname = 'pg_outline';
