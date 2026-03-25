# pg_outline - PostgreSQL 执行计划固定扩展

## 概述

`pg_outline` 是一个 PostgreSQL 扩展,通过执行计划的 Outline Hints 提供计划固定功能。该扩展参考了 OceanBase 的 Outline 功能,允许您捕获和固定 SQL 查询的执行计划。

## 功能特性

- **自动生成 Hints**: 从执行计划自动生成提示
- **计划固定**: 存储并重用执行计划,确保查询性能的一致性
- **Outline 显示**: 在查询执行后显示生成的 outline 数据
- **便捷管理**: 提供 SQL 函数来创建、删除、启用和禁用 outlines
- **Hint 支持**: 基于常见的 hint 类型(扫描方法、连接方法、连接顺序)

## 安装

### 编译扩展

```bash
cd contrib/pg_outline
make
make install
```

### 加载扩展

```sql
CREATE EXTENSION pg_outline;
```

## 配置参数

扩展提供了几个 GUC 参数:

- `pg_outline.enabled` (boolean, 默认: true)
  - 启用或禁用 pg_outline 扩展

- `pg_outline.display_hints` (boolean, 默认: true)
  - 控制是否在查询执行后显示生成的 outline hints

- `pg_outline.mode` (string, 默认: 'auto')
  - 设置 outline 生成模式
  - 可选值: 'auto', 'manual', 'off'
  - 'auto': 自动为所有查询生成 outlines
  - 'manual': 仅使用手动创建的 outlines
  - 'off': 禁用 outline 生成

### 配置示例

```sql
-- 启用 pg_outline
SET pg_outline.enabled = true;

-- 在查询执行后显示 hints
SET pg_outline.display_hints = true;

-- 使用自动模式
SET pg_outline.mode = 'auto';
```

## 使用方法

### 自动 Outline 生成

当 `pg_outline.mode` 设置为 'auto' 时,扩展会自动为每个查询生成 outline 数据:

```sql
-- 启用自动模式
SET pg_outline.mode = 'auto';
SET pg_outline.display_hints = true;

-- 执行查询
SELECT t1.id, t1.name, t2.description
FROM test_table1 t1
JOIN test_table2 t2 ON t1.id = t2.table1_id
WHERE t1.value > 500
ORDER BY t1.id
LIMIT 10;

-- 扩展会显示生成的 outline 数据:
/*+
BEGIN_OUTLINE_DATA
SeqScan(table)
HashJoin(...)
END_OUTLINE_DATA
*/
```

### 手动 Outline 管理

您可以手动创建、管理和应用 outlines:

```sql
-- 创建 outline
SELECT pg_outline_create(
    'my_outline',  -- outline 名称
    'SELECT * FROM test_table1 WHERE value > ?',  -- 查询模式
    'SeqScan(test_table1)'  -- hints (可选)
);

-- 列出所有 outlines
SELECT * FROM pg_outline_list();

-- 查看启用的 outlines
SELECT * FROM pg_outline_enabled;

-- 禁用 outline
SELECT pg_outline_disable('my_outline');

-- 启用 outline
SELECT pg_outline_enable('my_outline');

-- 删除 outline
SELECT pg_outline_drop('my_outline');
```

## Hint 类型

扩展支持类似 pg_hint_plan 的各种 hint 类型:

### 扫描方法 Hints

- `SeqScan(table)` - 强制顺序扫描
- `IndexScan(table)` - 强制索引扫描
- `IndexOnlyScan(table)` - 强制仅索引扫描
- `BitmapScan(table)` - 强制位图扫描

### 连接方法 Hints

- `NestLoop(tables)` - 强制嵌套循环连接
- `HashJoin(tables)` - 强制哈希连接
- `MergeJoin(tables)` - 强制归并连接

### 连接顺序 Hints

- `Leading(table1 table2 table3)` - 指定连接顺序

## Outline 数据格式

生成的 outline 数据遵循类似 OceanBase 和 Oracle 的格式:

```
/*+
BEGIN_OUTLINE_DATA
<hint1>
<hint2>
...
END_OUTLINE_DATA
*/
```

示例:

```
/*+
BEGIN_OUTLINE_DATA
SeqScan(test_table1)
HashJoin(test_table1 test_table2)
Leading(test_table1 test_table2)
END_OUTLINE_DATA
*/
```

## 系统表

扩展创建以下目录结构:

- `pg_outline_data` - 存储 outline 定义
  - `outline_id` - 唯一标识符
  - `outline_name` - outline 名称
  - `query_pattern` - SQL 查询模式
  - `hint_string` - 要应用的 hint 字符串
  - `enabled` - outline 是否激活
  - `created_at` - 创建时间戳
  - `updated_at` - 最后更新时间戳

- `pg_outline_enabled` - 已启用 outline 的视图

## 架构设计

扩展使用 PostgreSQL hooks 来拦截查询规划和执行:

1. **Planner Hook**: 拦截查询规划,从选择的计划中生成 hints
2. **Executor Hooks**: 跟踪查询执行并显示 outline 数据
3. **计划分析**: 递归分析计划树以提取相关 hints
4. **Hint 存储**: 将 hints 存储在系统表中以供后续重用

## 与类似工具的比较

### vs. pg_hint_plan

- `pg_hint_plan`: 专注于应用手动指定的 hints
- `pg_outline`: 自动从执行计划生成 hints 并作为 outlines 管理

### vs. OceanBase Outline

- 类似的概念和 API 设计
- 适配 PostgreSQL 的规划器和执行器架构
- 兼容 PostgreSQL 的标准查询优化

## 限制

当前版本的限制:

1. 简化的 hint 生成(基本的扫描和连接方法)
2. 查询模式匹配是精确的(尚未参数化)
3. 仅限于单查询 outlines(尚不支持复杂的 CTE)
4. Hint 应用尚未完全实现

## 未来改进

计划中的改进:

- 使用 planner hooks 完整实现 hint 应用
- 查询指纹识别以实现更好的模式匹配
- 支持并行查询 hints
- 扩展的 hint 类型(例如 SET、ROWS hints)
- 与 pg_stat_statements 集成以自动创建 outline
- Outline 导入/导出功能

## 示例

### 示例 1: 固定复杂查询

```sql
-- 创建测试表
CREATE TABLE orders (
    order_id INTEGER PRIMARY KEY,
    customer_id INTEGER,
    order_date DATE,
    total_amount DECIMAL(10,2)
);

CREATE TABLE order_items (
    item_id INTEGER PRIMARY KEY,
    order_id INTEGER REFERENCES orders(order_id),
    product_id INTEGER,
    quantity INTEGER,
    price DECIMAL(10,2)
);

CREATE INDEX idx_orders_customer ON orders(customer_id);
CREATE INDEX idx_items_order ON order_items(order_id);

-- 使用自动 outline 生成运行查询
SET pg_outline.mode = 'auto';
SET pg_outline.display_hints = true;

SELECT o.order_id, o.order_date, SUM(oi.quantity * oi.price) as total
FROM orders o
JOIN order_items oi ON o.order_id = oi.order_id
WHERE o.customer_id = 12345
GROUP BY o.order_id, o.order_date
ORDER BY o.order_date DESC;

-- 扩展会显示生成的 outline
-- 然后您可以基于此创建永久 outline
```

### 示例 2: 管理不同环境的 Outlines

```sql
-- 开发环境: 使用 hash joins
SELECT pg_outline_create(
    'report_query_dev',
    'SELECT * FROM large_table1 t1 JOIN large_table2 t2 ON ...',
    'HashJoin(large_table1 large_table2)'
);

-- 生产环境: 使用不同的计划
SELECT pg_outline_create(
    'report_query_prod',
    'SELECT * FROM large_table1 t1 JOIN large_table2 t2 ON ...',
    'MergeJoin(large_table1 large_table2) IndexScan(large_table1)'
);

-- 在不同环境间切换
SELECT pg_outline_enable('report_query_prod');
SELECT pg_outline_disable('report_query_dev');
```

## 故障排除

### 扩展未加载

```sql
-- 检查扩展是否已安装
SELECT * FROM pg_available_extensions WHERE name = 'pg_outline';

-- 检查 PostgreSQL 日志中的错误
-- 查找 "pg_outline extension loaded" 消息
```

### Hints 未显示

```sql
-- 验证配置
SHOW pg_outline.enabled;
SHOW pg_outline.display_hints;
SHOW pg_outline.mode;

-- 确保设置正确
SET pg_outline.enabled = true;
SET pg_outline.display_hints = true;
SET pg_outline.mode = 'auto';
```

### Outline 未应用

```sql
-- 检查 outline 是否已启用
SELECT * FROM pg_outline_enabled WHERE outline_name = 'your_outline';

-- 验证查询模式是否匹配
-- 注意: 当前版本需要精确匹配
```

## 贡献

欢迎贡献! 可贡献的领域:

- 额外的 hint 类型
- 改进的查询模式匹配
- 性能优化
- 文档改进
- 测试覆盖率

## 许可证

该扩展在 PostgreSQL 许可证下发布。

## 作者

PostgreSQL Extension Development Team

## 参考资料

- [PostgreSQL Planner Hooks](https://www.postgresql.org/docs/current/planner-optimizer.html)
- [pg_hint_plan](https://github.com/ossc-db/pg_hint_plan)
- [OceanBase Outline](https://www.oceanbase.com/docs/oceanbase-database/oceanbase-database/V3.1.0/outline-management)
- [Oracle SQL Plan Management](https://docs.oracle.com/en/database/oracle/oracle-database/19/tgsql/overview-of-sql-plan-management.html)
