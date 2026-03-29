# pg_outline 远程测试交付说明

## 概述

我已经为你创建了一套完整的 pg_outline 测试框架，可以在远程数据库上测试各种类型的SQL查询的 Outline 功能是否正确。

## 交付内容

### 1. 综合测试SQL文件
**文件**: `contrib/pg_outline/sql/comprehensive_outline_test.sql`

包含 **25个测试用例**，覆盖以下SQL类型：

#### 基础查询 (3个测试)
- Test 1: 简单 SELECT with WHERE
- Test 23: 带 DISTINCT 的聚合
- Test 24: CASE 表达式

#### JOIN 查询 (5个测试)
- Test 2: INNER JOIN
- Test 3: LEFT JOIN
- Test 4: 多表 JOIN (3个表)
- Test 21: Self JOIN (自连接)
- Test 22: CROSS JOIN (笛卡尔积)

#### 子查询 (7个测试)
- Test 5: SELECT 列表中的标量子查询
- Test 6: IN 子查询
- Test 7: EXISTS 子查询
- Test 8: NOT EXISTS 子查询
- Test 19: FROM 子句子查询
- Test 20: ANY/ALL 子查询
- Test 25: 关联子查询

#### CTE 公共表表达式 (3个测试)
- Test 9: 简单 CTE
- Test 10: 多个 CTE
- Test 11: 递归 CTE

#### 聚合和分组 (2个测试)
- Test 12: GROUP BY with HAVING
- Test 13: 窗口函数 (ROW_NUMBER, AVG OVER)

#### 集合操作 (4个测试)
- Test 14: UNION
- Test 15: UNION ALL
- Test 16: INTERSECT
- Test 17: EXCEPT

#### 复杂查询 (1个测试)
- Test 18: 多层嵌套 + JOIN + 子查询 + ORDER BY + LIMIT

### 2. 自动化测试脚本
**文件**: `contrib/pg_outline/run_outline_tests.sh`

功能特点：
- ✅ 自动连接数据库
- ✅ 检查 pg_outline 扩展
- ✅ 运行所有测试用例
- ✅ 彩色输出（成功/失败/警告）
- ✅ 生成测试报告
- ✅ 支持本地和远程数据库
- ✅ 可配置的输出目录
- ✅ 详细/安静模式切换

### 3. 详细文档
**文件**:
- `COMPREHENSIVE_TEST_CN.md` - 测试套件详细说明
- `TESTING.md` - 使用手册

包含：
- 每个测试的详细解释
- 预期结果说明
- 验证要点
- 故障排查指南
- 扩展测试方法

## 如何使用

### 方法1: 使用自动化脚本（推荐）

```bash
# 进入测试目录
cd /path/to/postgresql/contrib/pg_outline

# 本地测试
./run_outline_tests.sh

# 远程测试
./run_outline_tests.sh -H your.remote.server -p 5432 -d testdb -u testuser

# 详细输出模式
./run_outline_tests.sh -v

# 安静模式（只显示错误）
./run_outline_tests.sh -q
```

### 方法2: 直接使用 psql

```bash
# 本地数据库
psql -h localhost -d postgres -f sql/comprehensive_outline_test.sql

# 远程数据库
psql -h remote.server.com -p 5432 -d testdb -U testuser -f sql/comprehensive_outline_test.sql

# 或在 psql 交互模式中
psql -h remote.server.com -d testdb -U testuser
\i sql/comprehensive_outline_test.sql
```

### 方法3: 使用环境变量

```bash
export PGHOST=your.remote.server
export PGPORT=5432
export PGDATABASE=testdb
export PGUSER=testuser
export PGPASSWORD=yourpassword  # 可选

./run_outline_tests.sh
```

## 测试输出示例

成功运行后你会看到：

```
============================================================================
Testing Database Connection
============================================================================
  Host: remote.server.com
  Port: 5432
  Database: testdb
  User: testuser
✓ Database connection successful

============================================================================
Checking pg_outline Extension
============================================================================
✓ pg_outline extension is available

============================================================================
Running Comprehensive Tests
============================================================================
  Output will be saved to: ./test_results/test_output_20240329_150030.log
✓ All tests completed successfully
  Duration: 15 seconds

============================================================================
Test Summary
============================================================================
  Total outlines generated: 25
✓ No errors or warnings found

============================================================================
Test Report Generated
============================================================================
  Summary report: ./test_results/test_summary_20240329_150030.txt
```

## 验证重点

运行测试时，以下方面会被自动验证：

### 1. SubLink 标签正确性 ✅
- 主查询标记为 `[main]`
- 子查询标记为 `[sublink_0]`, `[sublink_1]` 等
- 示例：`[main] SeqScan(customers)` 和 `[sublink_0] SeqScan(orders)`

### 2. CTE 标签正确性 ✅
- CTE 标记为 `[cte_<name>]`
- 主查询和 CTE 分别生成 hints

### 3. JOIN 提示正确性 ✅
- 生成 HashJoin/NestLoop/MergeJoin hints
- 生成 Leading hints 指定连接顺序
- 表名正确识别

### 4. Fingerprint 一致性 ✅
- 相同查询产生相同的 fingerprint
- 不同空格格式不影响 fingerprint

### 5. 各种查询模式 ✅
- 简单查询
- 复杂嵌套查询
- 递归查询
- 聚合查询
- 窗口函数

## 测试数据

测试使用模拟电商场景的数据：
- **customers** 表：5个客户
- **orders** 表：5个订单
- **order_items** 表：6个订单项
- **products** 表：5个产品

数据量小巧但足以测试各种 SQL 模式。

## 查看测试结果

### 1. 查看测试输出文件

```bash
# 查看最新的测试日志
ls -lt test_results/test_output_*.log | head -1

# 读取日志
cat test_results/test_output_YYYYMMDD_HHMMSS.log
```

### 2. 查看生成的 Outlines

在测试数据库中运行：

```sql
-- 查看所有生成的 outlines
SELECT * FROM pg_outline_list();

-- 查看包含子查询的 outlines
SELECT outline_name, hint_string
FROM pg_outline_data
WHERE hint_string LIKE '%sublink%'
ORDER BY outline_name;

-- 查看 JOIN 相关的 outlines
SELECT outline_name, hint_string
FROM pg_outline_data
WHERE hint_string LIKE '%Join%'
ORDER BY outline_name;

-- 查看 CTE 相关的 outlines
SELECT outline_name, hint_string
FROM pg_outline_data
WHERE hint_string LIKE '%cte_%'
ORDER BY outline_name;
```

### 3. 验证 Fingerprint 一致性

```sql
-- 测试相同查询的 fingerprint
SELECT pg_outline_create('test1', 'SELECT * FROM customers WHERE city = ''New York''', '');
SELECT pg_outline_create('test2', '  SELECT * FROM customers WHERE city = ''New York''  ', '');

-- 应该看到相同的 fingerprint
SELECT fingerprint FROM pg_outline_data WHERE outline_name IN ('test1', 'test2');
```

## 常见问题

### Q1: 如何在远程服务器上测试？

A: 使用 `-H` 参数指定主机：
```bash
./run_outline_tests.sh -H your.server.com -u testuser -d testdb
```

### Q2: 测试需要什么权限？

A: 需要以下权限：
- CREATE EXTENSION
- CREATE TABLE
- INSERT/SELECT/DELETE
- 创建和删除 outline 的权限

建议使用 SUPERUSER 或数据库所有者运行测试。

### Q3: 测试会影响现有数据吗？

A: 不会。测试：
1. 创建独立的测试表（customers, orders 等）
2. 测试结束后自动清理所有测试表
3. 清理生成的 outline 数据

### Q4: 如何添加自己的测试？

A: 编辑 `sql/comprehensive_outline_test.sql`，添加新的测试块：
```sql
\echo 'Test 26: My Custom Test'
EXPLAIN SELECT ... your query ...;
```

### Q5: 测试失败怎么办？

A:
1. 检查日志文件：`test_results/test_output_*.log`
2. 使用详细模式重新运行：`./run_outline_tests.sh -v`
3. 确认 pg_outline 扩展已正确安装
4. 检查数据库连接权限

## 测试通过标准

✅ 所有25个测试用例执行成功
✅ 生成25个 outline（每个测试一个）
✅ 无错误信息
✅ SubLink 标签正确（`[sublink_N]`）
✅ CTE 标签正确（`[cte_name]`）
✅ JOIN hints 正确生成
✅ Fingerprint 一致性验证通过

## 后续建议

1. **性能测试**: 增加测试数据量（修改 generate_series 范围）
2. **压力测试**: 并发运行多个测试实例
3. **真实数据测试**: 使用生产环境的实际查询
4. **回归测试**: 在每次代码更改后运行测试
5. **CI/CD 集成**: 将测试加入自动化流程

## 文件清单

```
contrib/pg_outline/
├── sql/
│   └── comprehensive_outline_test.sql    # 主测试文件 (25个测试)
├── run_outline_tests.sh                  # 自动化测试脚本
├── COMPREHENSIVE_TEST_CN.md              # 详细测试说明
├── TESTING.md                            # 测试使用手册
└── test_results/                         # 测试结果目录（自动创建）
    ├── test_output_*.log                 # 测试输出日志
    └── test_summary_*.txt                # 测试摘要报告
```

## Git 提交

所有文件已提交到分支：
- Commit: 6a606ac29d
- 分支: claude/add-outline-functionality

可以直接拉取使用：
```bash
git checkout claude/add-outline-functionality
cd contrib/pg_outline
./run_outline_tests.sh
```

## 总结

这套测试框架提供了：
1. ✅ **全面的测试覆盖** - 25种不同的SQL模式
2. ✅ **自动化执行** - 一键运行所有测试
3. ✅ **远程测试支持** - 可以测试任何远程数据库
4. ✅ **详细的文档** - 完整的中文说明
5. ✅ **结果报告** - 自动生成测试报告
6. ✅ **易于扩展** - 可以轻松添加新测试

现在你可以方便地在远程服务器上测试 pg_outline 的各种SQL场景了！

如有任何问题或需要添加特定的测试用例，请随时告诉我。
