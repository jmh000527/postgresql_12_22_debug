# Query Naming Test Summary (测试总结)

## Test Purpose (测试目的)
验证在CTE、子查询、子链接等多处指定Hint的情况下，outline输出的Hint前的Query名字是否正确。

Test whether the Query names prefixed to outline hints are correct when hints are specified in CTEs, subqueries, and sublinks.

## Test Files Created (创建的测试文件)

### 1. test_query_naming.sql
Comprehensive SQL test script with 12 test cases covering:
- Simple queries with main query only
- Queries with CTEs (Common Table Expressions)
- Queries with subqueries in FROM clause
- Queries with SubLinks in WHERE clause (IN, EXISTS)
- Complex queries combining all types
- Nested subqueries and multiple SubLinks
- Scalar subqueries in SELECT list

### 2. verify_query_names.md
Verification guide with:
- Expected query naming patterns for each test case
- Verification checklist
- Common issues to watch for
- Success criteria

## How to Run the Test (如何运行测试)

### Prerequisites (前置条件)
```bash
# 1. Configure and build PostgreSQL with pg_outline
cd /home/runner/work/postgresql_12_22_debug/postgresql_12_22_debug
./configure
make
make install

# 2. Build pg_outline extension
cd contrib/pg_outline
make
make install

# 3. Initialize database (if needed)
initdb -D /path/to/data
pg_ctl -D /path/to/data start
```

### Run Test (运行测试)
```bash
# Create test database
createdb test_outline

# Run the test script
psql -d test_outline -f contrib/pg_outline/test_query_naming.sql > test_results.txt 2>&1

# Review results
less test_results.txt
```

## Expected Query Naming Convention (预期的Query命名规范)

| Query Type | Naming Pattern | Example |
|------------|----------------|---------|
| Main Query | `main` | `[main] SeqScan(t1)` |
| CTE | `cte_<name>` | `[cte_orders] SeqScan(orders_table)` |
| Subquery (FROM) | `subquery_N` | `[subquery_0] IndexScan(t2)` |
| SubLink (WHERE/SELECT/HAVING) | `sublink_N` | `[sublink_0] SeqScan(t3)` |

Where N is a sequential counter starting from 0.

## Key Test Cases (关键测试用例)

### Test Case 1: CTE Naming
```sql
WITH orders AS (
    SELECT /*+ SeqScan(orders_table) */ * FROM orders_table
)
SELECT /*+ HashJoin(customers orders) */ *
FROM customers JOIN orders ON customers.id = orders.customer_id;
```
**Expected:**
- `[main] HashJoin(customers orders)`
- `[cte_orders] SeqScan(orders_table)`

✓ CTE should be named `cte_orders` (using actual CTE name)

### Test Case 2: Subquery in FROM
```sql
SELECT /*+ HashJoin(t1 sub) */ t1.id, sub.data
FROM t1
JOIN (
    SELECT /*+ IndexScan(t2) */ id, data FROM t2
) AS sub ON t1.id = sub.id;
```
**Expected:**
- `[main] HashJoin(t1 sub)`
- `[subquery_0] IndexScan(t2)`

✓ First subquery in FROM should be numbered `subquery_0`

### Test Case 3: SubLink in WHERE
```sql
SELECT /*+ SeqScan(t1) */ *
FROM t1
WHERE id IN (
    SELECT /*+ IndexScan(t2) */ t1_id FROM t2
);
```
**Expected:**
- `[main] SeqScan(t1)`
- `[sublink_0] IndexScan(t2)`

✓ First SubLink should be numbered `sublink_0`

### Test Case 4: Complex Mixed
```sql
WITH cte1 AS (
    SELECT /*+ SeqScan(t2) */ * FROM t2
)
SELECT /*+ HashJoin(t1 sub) */ t1.id, sub.name
FROM t1
JOIN (
    SELECT /*+ NestLoop(t3 cte1) */ t3.id, cte1.data as name
    FROM t3
    JOIN cte1 ON t3.id = cte1.id
    WHERE t3.id IN (
        SELECT /*+ SeqScan(t4) */ t4_id FROM t4
    )
) AS sub ON t1.id = sub.id;
```
**Expected:**
- `[main] HashJoin(t1 sub)`
- `[cte_cte1] SeqScan(t2)` - CTE with its name
- `[subquery_0] NestLoop(t3 cte1)` - First subquery
- `[sublink_0] SeqScan(t4)` - First SubLink (within subquery)

✓ All four query types should have correct, unique names

## Verification Points (验证要点)

When reviewing test output, check:

1. **Prefix Format (前缀格式)**: All hints should have `[query_name]` prefix
2. **Main Query (主查询)**: Should always be `[main]`
3. **CTE Names (CTE名称)**: Should use actual CTE name, e.g., `[cte_orders]`, not `[cte_0]`
4. **Sequential Numbers (顺序编号)**:
   - Subqueries: `subquery_0`, `subquery_1`, `subquery_2`, ...
   - SubLinks: `sublink_0`, `sublink_1`, `sublink_2`, ...
5. **No Duplicates (无重复)**: Each Query should have unique name/number
6. **Complete Coverage (完整覆盖)**: All hints should be prefixed, none missing

## Success Criteria (成功标准)

The test PASSES if (测试通过条件):

✓ All EXPLAIN outputs show hints with `[query_name]` prefixes
✓ Main queries are labeled `[main]`
✓ CTEs use their actual names: `[cte_<cte_name>]`
✓ Subqueries are numbered sequentially: `[subquery_0]`, `[subquery_1]`, ...
✓ SubLinks are numbered sequentially: `[sublink_0]`, `[sublink_1]`, ...
✓ No missing or duplicate query names
✓ Complex queries with all types show all correct prefixes
✓ Stored outlines preserve query name prefixes

## Potential Issues (可能的问题)

### Issue 1: Missing Prefixes
**Symptom**: Hints appear without `[query_name]` prefix
**Cause**: Query naming not executed or metadata table not created
**Check**: Verify `assign_query_names()` is called in `outline_planner()`

### Issue 2: Wrong Query Type Names
**Symptom**: CTE shows as `[subquery_0]` or SubLink shows as `[main]`
**Cause**: Query type detection logic error
**Check**: Review `assign_query_names()` parent_name and cte_name parameters

### Issue 3: Duplicate Numbers
**Symptom**: Multiple queries have same number (e.g., two `[sublink_0]`)
**Cause**: Static variable persistence (should be fixed now)
**Check**: Verify `query_index` is in `QueryNamingContext`, not static

### Issue 4: Names Not Persistent
**Symptom**: Query names change between EXPLAIN and execution
**Cause**: Memory context issue or hash table destroyed too early
**Check**: Verify metadata table allocated in appropriate memory context

## Code References (代码参考)

Key implementation files:
- `contrib/pg_outline/pg_outline.c:2708-2770` - `assign_query_names()` function
- `contrib/pg_outline/pg_outline.c:2593-2627` - `generate_query_name()` function
- `contrib/pg_outline/pg_outline.c:348-362` - Query naming in `outline_planner()` hook
- `contrib/pg_outline/pg_outline.c:215-222` - `QueryNamingContext` structure
- `contrib/pg_outline/QUERY_NAMING.md` - Full documentation

## Recent Fixes (最近的修复)

As of the latest commit:
1. ✓ Fixed static `query_index` variable issue - now part of `QueryNamingContext`
2. ✓ Moved query naming to `outline_planner()` hook before optimizer runs
3. ✓ Added duplicate detection in `outline_ExplainOneQuery()` hook
4. ✓ Added proper cleanup of metadata hash table

These fixes ensure:
- No cross-call index persistence
- Query naming happens before optimization
- No duplicate naming
- No memory leaks
