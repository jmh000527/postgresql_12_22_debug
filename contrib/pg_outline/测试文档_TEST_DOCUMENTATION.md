# Query Naming Test Documentation - 完整测试文档

## 概述 (Overview)

根据问题陈述："测试一下在CTE，子查询，子链接等多处指定Hint的情况，outline输出的Hint前的Query名字是否正确"

本文档提供了完整的测试套件来验证 pg_outline 扩展在以下场景中的 Query 命名是否正确：
- CTE (Common Table Expressions / WITH 子句)
- 子查询 (Subqueries in FROM clause)
- 子链接 (SubLinks in WHERE/SELECT/HAVING)

This document provides a complete test suite to verify that pg_outline extension correctly names queries with hints in:
- CTEs (Common Table Expressions / WITH clauses)
- Subqueries (in FROM clause)
- SubLinks (in WHERE/SELECT/HAVING clauses)

---

## 测试文件清单 (Test Files)

### 1. 主测试文件 (Main Test File)
**文件**: `test_query_naming.sql`
**描述**: 包含 12 个综合测试用例的完整 SQL 测试脚本
- 简单查询（仅主查询）
- 带 CTE 的查询
- 带 FROM 子查询的查询
- 带 WHERE SubLink 的查询（IN、EXISTS）
- 复杂的组合查询
- 嵌套子查询和多个 SubLinks
- SELECT 列表中的标量子查询

### 2. 快速测试脚本 (Quick Test Script)
**文件**: `quick_test.sh`
**描述**: 可执行的 Bash 脚本，快速验证基本功能
**使用方法**:
```bash
cd contrib/pg_outline
./quick_test.sh
```

### 3. 测试总结 (Test Summary)
**文件**: `TEST_SUMMARY.md`
**描述**: 完整的测试说明和成功标准

### 4. 预期输出示例 (Expected Output Examples)
**文件**: `EXPECTED_OUTPUT.md`
**描述**: 详细的预期输出格式和验证模式

### 5. 验证指南 (Verification Guide)
**文件**: `verify_query_names.md`
**描述**: 验证清单和常见问题排查

---

## Query 命名规范 (Naming Convention)

### 命名规则 (Naming Rules)

| Query 类型 | 命名模式 | 示例 |
|-----------|---------|------|
| 主查询 | `main` | `[main] SeqScan(t1)` |
| CTE | `cte_<名称>` | `[cte_orders] IndexScan(orders)` |
| FROM 子查询 | `subquery_N` | `[subquery_0] HashJoin(t2 t3)` |
| SubLink | `sublink_N` | `[sublink_0] SeqScan(t4)` |

其中 N 从 0 开始的顺序计数器。

### 输出格式 (Output Format)

所有 hint 都应该使用以下格式：
```
[query_name] hint_content
```

例如：
```
[main] SeqScan(t1)
[cte_orders] IndexScan(orders_table)
[subquery_0] HashJoin(t2 t3)
[sublink_0] NestLoop(t4 t5)
```

---

## 如何运行测试 (How to Run Tests)

### 方法 1: 使用快速测试脚本 (Quick Test)

```bash
cd /home/runner/work/postgresql_12_22_debug/postgresql_12_22_debug/contrib/pg_outline
./quick_test.sh
```

这将创建一个临时数据库并运行基本测试用例。

### 方法 2: 使用完整测试套件 (Full Test Suite)

```bash
# 创建测试数据库
createdb test_query_naming

# 运行完整测试
psql -d test_query_naming -f test_query_naming.sql > test_results.txt 2>&1

# 查看结果
less test_results.txt

# 清理
dropdb test_query_naming
```

### 方法 3: 手动测试单个用例 (Manual Single Case)

```sql
-- 连接到数据库
psql -d your_database

-- 创建扩展
CREATE EXTENSION pg_outline;

-- 配置参数
SET pg_outline.enabled = true;
SET pg_outline.display_hints = true;
SET pg_outline.mode = 'auto';

-- 创建测试表
CREATE TABLE t1 (id int, val int);
CREATE TABLE t2 (id int, t1_id int);
INSERT INTO t1 SELECT i, i*10 FROM generate_series(1,10) i;
INSERT INTO t2 SELECT i, i FROM generate_series(1,10) i;

-- 测试 CTE 命名
EXPLAIN (COSTS OFF)
WITH my_cte AS (
    SELECT /*+ SeqScan(t1) */ * FROM t1
)
SELECT /*+ HashJoin(t2 my_cte) */ *
FROM t2 JOIN my_cte ON t2.t1_id = my_cte.id;
```

**预期输出应该包含**:
```
[main] HashJoin(t2 my_cte)
[cte_my_cte] SeqScan(t1)
```

---

## 关键测试用例 (Key Test Cases)

### 测试用例 1: CTE 命名
**SQL**:
```sql
WITH orders AS (
    SELECT /*+ SeqScan(orders_table) */ * FROM orders_table
)
SELECT /*+ HashJoin(customers orders) */ *
FROM customers JOIN orders ON customers.id = orders.customer_id;
```

**预期输出**:
```
[main] HashJoin(customers orders)
[cte_orders] SeqScan(orders_table)
```

**验证点**:
- ✓ CTE 使用实际名称 `orders`
- ✓ 主查询标记为 `main`

---

### 测试用例 2: FROM 子查询命名
**SQL**:
```sql
SELECT /*+ HashJoin(t1 sub) */ t1.id, sub.data
FROM t1
JOIN (
    SELECT /*+ IndexScan(t2) */ id, data FROM t2
) AS sub ON t1.id = sub.id;
```

**预期输出**:
```
[main] HashJoin(t1 sub)
[subquery_0] IndexScan(t2)
```

**验证点**:
- ✓ 子查询编号从 0 开始
- ✓ 不使用别名 `sub`，而是使用 `subquery_0`

---

### 测试用例 3: WHERE SubLink 命名
**SQL**:
```sql
SELECT /*+ SeqScan(t1) */ *
FROM t1
WHERE id IN (
    SELECT /*+ IndexScan(t2) */ t1_id FROM t2
);
```

**预期输出**:
```
[main] SeqScan(t1)
[sublink_0] IndexScan(t2)
```

**验证点**:
- ✓ SubLink 编号从 0 开始
- ✓ IN 子查询识别为 SubLink

---

### 测试用例 4: 复杂混合 (All Types)
**SQL**:
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

**预期输出**:
```
[main] HashJoin(t1 sub)
[cte_cte1] SeqScan(t2)
[subquery_0] NestLoop(t3 cte1)
[sublink_0] SeqScan(t4)
```

**验证点**:
- ✓ 所有四种 Query 类型都有正确的前缀
- ✓ CTE 使用名称 `cte1`
- ✓ 子查询和 SubLink 各自独立编号

---

## 验证清单 (Verification Checklist)

检查测试输出时，验证以下内容：

### 1. 格式检查 (Format Checks)
- [ ] 所有 hint 都有 `[query_name] hint_content` 格式
- [ ] Query 名称用方括号括起来
- [ ] Query 名称和 hint 内容之间有空格

### 2. 主查询检查 (Main Query Checks)
- [ ] 顶层查询始终命名为 `[main]`
- [ ] 第一个查询不是 `[main_0]`（除非有多个语句）

### 3. CTE 检查 (CTE Checks)
- [ ] 使用 `cte_` 前缀
- [ ] 使用 SQL 中的实际 CTE 名称
- [ ] 示例：`[cte_orders]`、`[cte_products]`

### 4. 子查询检查 (Subquery Checks)
- [ ] 使用 `subquery_` 前缀
- [ ] 从 0 开始编号
- [ ] 顺序：`[subquery_0]`、`[subquery_1]` 等

### 5. SubLink 检查 (SubLink Checks)
- [ ] 使用 `sublink_` 前缀
- [ ] 从 0 开始编号
- [ ] 顺序：`[sublink_0]`、`[sublink_1]` 等

### 6. 完整性检查 (Completeness Checks)
- [ ] 所有内联 hint 都出现在输出中
- [ ] 没有缺失的 hint
- [ ] 没有重复的 Query 名称/编号

---

## 成功标准 (Success Criteria)

### 测试通过条件 (Test Passes If):

✅ **所有以下条件都满足**:
1. 所有 hint 都有 `[query_name]` 前缀
2. 主查询显示为 `[main]`
3. CTE 显示为 `[cte_实际名称]`
4. 子查询顺序编号：`[subquery_0]`, `[subquery_1]`, ...
5. SubLink 顺序编号：`[sublink_0]`, `[sublink_1]`, ...
6. 没有缺失的 hint
7. 没有重复的名称/编号
8. 复杂查询显示所有类型的正确前缀
9. 存储的 outline 保留 Query 名称前缀

### 测试失败条件 (Test Fails If):

❌ **出现以下任何情况**:
1. Hint 没有前缀
2. Query 类型名称错误
3. 编号有间隙
4. 编号重复
5. Hint 缺失
6. 使用通用名称如 `[query_0]` 而不是正确名称

---

## 常见问题 (Common Issues)

### 问题 1: 缺失 `[query_name]` 前缀
**症状**: Hint 没有前缀
**原因**: Query 命名未执行或元数据表未创建
**检查**: 验证 `outline_planner()` 在优化前调用 `assign_query_names()`

### 问题 2: Query 类型名称错误
**症状**: CTE 显示为 `[subquery_0]`
**原因**: Query 类型检测逻辑错误
**检查**: 检查 `assign_query_names()` 的参数传递

### 问题 3: 编号重复
**症状**: 多个查询有相同的编号
**原因**: 静态变量持久化（应该已修复）
**检查**: 验证 `query_index` 在 `QueryNamingContext` 中，不是静态的

---

## 技术实现细节 (Technical Details)

### 代码位置 (Code Locations)
- Query 命名: `pg_outline.c:2708-2770` (`assign_query_names()`)
- 名称生成: `pg_outline.c:2593-2627` (`generate_query_name()`)
- 优化前命名: `pg_outline.c:348-362` (`outline_planner()` hook)
- Hint 收集: `pg_outline.c:1662-1723` (`collect_query_hints_recursive()`)
- 前缀格式化: `pg_outline.c:1679` (格式: `"[%s] %s"`)

### 最近的修复 (Recent Fixes)
1. ✓ 修复了静态 `query_index` 变量问题 - 现在是 `QueryNamingContext` 的一部分
2. ✓ 将 Query 命名移到 `outline_planner()` hook，在优化器运行之前
3. ✓ 在 `outline_ExplainOneQuery()` hook 中添加了重复检测
4. ✓ 添加了元数据哈希表的适当清理

---

## 文件位置 (File Locations)

所有测试文件位于:
```
/home/runner/work/postgresql_12_22_debug/postgresql_12_22_debug/contrib/pg_outline/
```

- `test_query_naming.sql` - 完整测试套件
- `quick_test.sh` - 快速测试脚本
- `TEST_SUMMARY.md` - 测试总结
- `EXPECTED_OUTPUT.md` - 预期输出示例
- `verify_query_names.md` - 验证指南
- `QUERY_NAMING.md` - Query 命名文档（已存在）

---

## 总结 (Summary)

本测试套件全面验证了 pg_outline 扩展的 Query 命名功能，涵盖：
- ✅ CTE (Common Table Expressions)
- ✅ 子查询 (Subqueries in FROM)
- ✅ SubLink (IN, EXISTS, scalar subqueries)
- ✅ 复杂的嵌套和混合场景
- ✅ 所有 Query 类型的正确命名和编号

通过运行这些测试，可以确保在所有场景下 outline 输出的 Hint 前的 Query 名字都是正确的。

This test suite comprehensively verifies pg_outline's Query naming functionality, covering all scenarios with CTEs, subqueries, and SubLinks, ensuring correct Query name prefixes in outline hints output.
