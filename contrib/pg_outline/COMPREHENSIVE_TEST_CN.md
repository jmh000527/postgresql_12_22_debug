# pg_outline 综合测试套件说明

## 概述

这个测试套件 (`comprehensive_outline_test.sql`) 包含了25个不同类型的SQL查询测试，用于验证 pg_outline 扩展在各种SQL场景下的正确性。

## 测试覆盖的SQL类型

### 1. 基础查询
- **Test 1**: 简单 SELECT 带 WHERE 子句
- **Test 23**: 带 DISTINCT 的聚合查询

### 2. JOIN 查询
- **Test 2**: INNER JOIN
- **Test 3**: LEFT JOIN
- **Test 4**: 多表 JOIN（3个表）
- **Test 21**: Self JOIN（自连接）
- **Test 22**: CROSS JOIN（笛卡尔积）

### 3. 子查询
- **Test 5**: SELECT 列表中的标量子查询
- **Test 6**: IN 子句中的子查询
- **Test 7**: EXISTS 子查询
- **Test 8**: NOT EXISTS 子查询
- **Test 19**: FROM 子句中的子查询（派生表）
- **Test 20**: ANY/ALL 子查询
- **Test 25**: 关联子查询

### 4. CTE（公共表表达式）
- **Test 9**: 简单 CTE
- **Test 10**: 多个 CTE
- **Test 11**: 递归 CTE

### 5. 聚合和分组
- **Test 12**: GROUP BY 带 HAVING 子句
- **Test 13**: 窗口函数（ROW_NUMBER, AVG OVER）

### 6. 集合操作
- **Test 14**: UNION
- **Test 15**: UNION ALL
- **Test 16**: INTERSECT
- **Test 17**: EXCEPT

### 7. 复杂查询
- **Test 18**: 复杂嵌套查询（多层子查询 + JOIN + ORDER BY + LIMIT）
- **Test 24**: CASE 表达式带子查询

## 测试数据

测试使用以下表结构：

1. **customers** - 客户表
   - customer_id (主键)
   - customer_name, city, country
   - credit_limit

2. **orders** - 订单表
   - order_id (主键)
   - customer_id (外键)
   - order_date, total_amount, status

3. **order_items** - 订单明细表
   - item_id (主键)
   - order_id (外键)
   - product_name, quantity, unit_price

4. **products** - 产品表
   - product_id (主键)
   - product_name, category, price, stock_quantity

## 如何运行测试

### 方法1：使用 psql 直接运行

```bash
psql -h <host> -d <database> -U <user> -f contrib/pg_outline/sql/comprehensive_outline_test.sql
```

### 方法2：在 psql 交互模式中运行

```sql
\i contrib/pg_outline/sql/comprehensive_outline_test.sql
```

### 方法3：使用提供的测试脚本

```bash
./run_outline_tests.sh
```

## 预期结果

每个测试将：

1. 执行 EXPLAIN 命令生成查询计划
2. 在 auto 模式下自动生成 outline hints
3. 显示生成的 outline 数据，包括：
   - Query 名称（main, sublink_N, cte_name, subquery_N）
   - 生成的 hints（SeqScan, IndexScan, HashJoin, Leading等）

测试结束时会显示：
- 总共生成的 outline 数量
- 前10个 outline 的预览

## 验证要点

运行测试时需要关注：

### 1. SubLink 标签正确性
对于包含子查询的SQL（Test 5-8, 18, 24-25），验证：
- 主查询的 hints 标记为 `[main]`
- 子查询的 hints 标记为 `[sublink_0]`, `[sublink_1]` 等
- 不被优化器提升的子查询保持独立标签

### 2. CTE 标签正确性
对于CTE查询（Test 9-11），验证：
- 主查询标记为 `[main]`
- CTE 标记为 `[cte_<name>]`
- 递归CTE的两部分都正确标记

### 3. JOIN 提示正确性
对于JOIN查询（Test 2-4, 21-22），验证：
- 生成 HashJoin/NestLoop/MergeJoin hints
- 生成 Leading hints 指定 join 顺序
- 表名正确识别

### 4. 聚合查询正确性
对于GROUP BY/聚合查询（Test 12-13），验证：
- 扫描方法 hints 正确
- 窗口函数不影响 hint 生成

### 5. 集合操作正确性
对于UNION/INTERSECT/EXCEPT（Test 14-17），验证：
- 每个分支查询独立生成 hints
- 不同分支可能有不同的查询名称

### 6. Fingerprint 一致性
验证相同查询的 fingerprint 保持一致：
- 不同空格格式的相同查询应产生相同 fingerprint
- 可以正确匹配和应用存储的 outline

## 故障排查

如果测试失败，检查：

1. **扩展是否正确安装**
   ```sql
   SELECT * FROM pg_extension WHERE extname = 'pg_outline';
   ```

2. **GUC 参数是否正确设置**
   ```sql
   SHOW pg_outline.enabled;
   SHOW pg_outline.mode;
   ```

3. **查看详细日志**
   ```sql
   SET client_min_messages = DEBUG1;
   ```

4. **检查表是否有数据和统计信息**
   ```sql
   SELECT COUNT(*) FROM customers;
   SELECT COUNT(*) FROM pg_stats WHERE tablename = 'customers';
   ```

## 扩展测试

可以根据实际业务场景添加更多测试：

1. 分区表查询
2. 继承表查询
3. 物化视图
4. 外部表（Foreign Data Wrapper）
5. 并行查询
6. 准备语句（Prepared Statements）
7. 存储过程中的查询

## 性能考虑

- 所有测试使用小数据集，重点验证功能正确性
- 对于性能测试，需要使用更大的数据集
- 可以使用 pg_bench 或自定义负载测试工具

## 已知限制

根据当前实现，以下场景可能有特殊行为：

1. 极度复杂的嵌套查询（>5层）可能需要更多内存
2. 某些优化器转换可能导致 hint 应用位置变化
3. 并行查询的 hint 支持有限

## 测试结果报告

建议记录测试结果，包括：
- 测试时间和环境（PostgreSQL版本、OS等）
- 成功/失败的测试数量
- 生成的 outline 数量
- 任何错误消息或警告
- 性能数据（如果相关）
