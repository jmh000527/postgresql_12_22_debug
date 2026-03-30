-- ============================================================================
-- 测试多处 Hint 位置支持
-- Test Multiple Hint Positions Support (like OceanBase)
-- ============================================================================

\set ON_ERROR_STOP on

-- 清理并重新创建扩展
DROP EXTENSION IF EXISTS pg_outline CASCADE;
CREATE EXTENSION pg_outline;

-- 配置
SET pg_outline.enabled = true;
SET pg_outline.mode = 'auto';
SET client_min_messages = NOTICE;

-- ============================================================================
-- 准备测试数据
-- ============================================================================

CREATE TABLE t1 (
    id INTEGER PRIMARY KEY,
    name TEXT,
    value INTEGER
);

CREATE TABLE t2 (
    id INTEGER PRIMARY KEY,
    t1_id INTEGER REFERENCES t1(id),
    amount DECIMAL(10,2)
);

CREATE TABLE t3 (
    id INTEGER PRIMARY KEY,
    t2_id INTEGER REFERENCES t2(id),
    status TEXT
);

INSERT INTO t1 VALUES (1, 'Alice', 100), (2, 'Bob', 200), (3, 'Charlie', 300);
INSERT INTO t2 VALUES (1, 1, 50.00), (2, 1, 75.00), (3, 2, 100.00);
INSERT INTO t3 VALUES (1, 1, 'active'), (2, 2, 'pending'), (3, 3, 'completed');

ANALYZE t1;
ANALYZE t2;
ANALYZE t3;

\echo ''
\echo '============================================================================'
\echo 'Test 1: 单个查询中多个子查询，每个都有独立的 Hint'
\echo 'Single query with multiple subqueries, each with its own hint'
\echo '============================================================================'

EXPLAIN
/*+ SeqScan(t1) */
SELECT t1.name,
       /*+ IndexScan(t2) */ (SELECT COUNT(*) FROM t2 WHERE t2.t1_id = t1.id) as t2_count,
       /*+ SeqScan(t3) */ (SELECT COUNT(*) FROM t3 WHERE t3.t2_id IN (SELECT t2.id FROM t2 WHERE t2.t1_id = t1.id)) as t3_count
FROM t1
WHERE t1.value > 50;

\echo ''
\echo '============================================================================'
\echo 'Test 2: JOIN 查询 + 子查询，多处指定 Hint'
\echo 'JOIN query with subquery, multiple hint positions'
\echo '============================================================================'

EXPLAIN
/*+ HashJoin(t1 t2) Leading(t1 t2) */
SELECT t1.name, t2.amount
FROM t1
JOIN t2 ON t1.id = t2.t1_id
WHERE t2.amount > /*+ SeqScan(t2) */ (SELECT AVG(amount) FROM t2)
ORDER BY t2.amount DESC;

\echo ''
\echo '============================================================================'
\echo 'Test 3: CTE + 主查询都有 Hint'
\echo 'CTE and main query both with hints'
\echo '============================================================================'

EXPLAIN
/*+ SeqScan(t2) */
WITH high_value_t1 AS (
    /*+ SeqScan(t1) */
    SELECT id, name, value
    FROM t1
    WHERE value > 100
)
/*+ HashJoin(high_value_t1 t2) */
SELECT h.name, t2.amount
FROM high_value_t1 h
JOIN t2 ON h.id = t2.t1_id;

\echo ''
\echo '============================================================================'
\echo 'Test 4: 嵌套子查询，每层都有 Hint'
\echo 'Nested subqueries with hints at each level'
\echo '============================================================================'

EXPLAIN
/*+ SeqScan(t1) */
SELECT t1.name
FROM t1
WHERE t1.id IN (
    /*+ SeqScan(t2) */
    SELECT t2.t1_id
    FROM t2
    WHERE t2.amount > (
        /*+ SeqScan(t2) */
        SELECT AVG(amount)
        FROM t2
        WHERE t2.t1_id IN (
            /*+ SeqScan(t1) */
            SELECT id FROM t1 WHERE value > 100
        )
    )
);

\echo ''
\echo '============================================================================'
\echo 'Test 5: EXISTS 子查询 + IN 子查询，分别指定 Hint'
\echo 'EXISTS and IN subqueries with separate hints'
\echo '============================================================================'

EXPLAIN
/*+ SeqScan(t1) */
SELECT t1.name
FROM t1
WHERE EXISTS (
    /*+ SeqScan(t2) */
    SELECT 1 FROM t2 WHERE t2.t1_id = t1.id AND t2.amount > 60
)
AND t1.id IN (
    /*+ IndexScan(t2) */
    SELECT DISTINCT t1_id FROM t2 WHERE amount > 50
);

\echo ''
\echo '============================================================================'
\echo 'Test 6: UNION 查询的每个分支都有 Hint'
\echo 'UNION query with hints in each branch'
\echo '============================================================================'

EXPLAIN
/*+ SeqScan(t1) */
SELECT name, value FROM t1 WHERE value > 100
UNION ALL
/*+ SeqScan(t1) */
SELECT name, value FROM t1 WHERE value < 150;

\echo ''
\echo '============================================================================'
\echo 'Test 7: 复杂查询 - JOIN + 多个子查询 + 每处都有 Hint'
\echo 'Complex query - JOIN with multiple subqueries, hints everywhere'
\echo '============================================================================'

EXPLAIN
/*+ HashJoin(t1 t2) Leading(t1 t2) */
SELECT 
    t1.name,
    t2.amount,
    /*+ SeqScan(t3) */ (SELECT COUNT(*) FROM t3 WHERE t3.t2_id = t2.id) as t3_count,
    /*+ SeqScan(t2) */ (SELECT MAX(amount) FROM t2 WHERE t2.t1_id = t1.id) as max_amount
FROM t1
/*+ SeqScan(t1) */
JOIN t2 ON t1.id = t2.t1_id
WHERE t1.value > /*+ SeqScan(t1) */ (SELECT AVG(value) FROM t1)
  AND t2.amount > /*+ SeqScan(t2) */ (SELECT MIN(amount) FROM t2)
ORDER BY t2.amount DESC
LIMIT 10;

\echo ''
\echo '============================================================================'
\echo 'Test 8: 验证 Outline 是否存储了多处 Hint'
\echo 'Verify that outlines store multiple hints'
\echo '============================================================================'

-- 在 manual 模式下创建一个带多处 hint 的 outline
SET pg_outline.mode = 'manual';

SELECT pg_outline_create(
    'multi_hint_outline',
    'SELECT t1.name, (SELECT COUNT(*) FROM t2 WHERE t2.t1_id = t1.id) FROM t1 WHERE value > 50;',
    '[main] SeqScan(t1)
[sublink_0] IndexScan(t2)'
);

-- 查看存储的 outline
SELECT outline_name, query_pattern, hint_string 
FROM pg_outline_data 
WHERE outline_name = 'multi_hint_outline';

\echo ''
\echo '============================================================================'
\echo 'Test 9: 测试 Outline 匹配和应用'
\echo 'Test outline matching and application'
\echo '============================================================================'

-- 启用 outline
UPDATE pg_outline_data SET enabled = true WHERE outline_name = 'multi_hint_outline';

-- 执行相同的查询，应该使用 outline
EXPLAIN
SELECT t1.name, 
       (SELECT COUNT(*) FROM t2 WHERE t2.t1_id = t1.id) as t2_count
FROM t1 
WHERE value > 50;

\echo ''
\echo '============================================================================'
\echo '清理测试数据'
\echo 'Cleanup'
\echo '============================================================================'

DROP TABLE IF EXISTS t3 CASCADE;
DROP TABLE IF EXISTS t2 CASCADE;
DROP TABLE IF EXISTS t1 CASCADE;
DELETE FROM pg_outline_data;

\echo ''
\echo '============================================================================'
\echo '所有多 Hint 位置测试完成！'
\echo 'All multiple hint position tests completed!'
\echo '============================================================================'
