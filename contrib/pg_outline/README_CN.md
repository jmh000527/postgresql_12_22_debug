# pg_outline - PostgreSQL 执行计划固定扩展

## 概述

`pg_outline` 是一个 PostgreSQL 扩展,通过执行计划的 Outline Hints 提供计划固定功能。该扩展参考了 OceanBase 的 Outline 功能,允许您捕获和固定 SQL 查询的执行计划。

## 功能特性

- **自动生成 Hints**: 从执行计划自动生成提示，支持带位置信息的 hint 重构
- **Hint 解析与应用**: 完整的 hint 解析和应用机制，实现真正的计划固定
- **计划固定**: 存储并重用执行计划，确保查询性能的一致性
- **Outline 显示**: 在查询执行后显示生成的 outline 数据
- **便捷管理**: 提供 SQL 函数来创建、删除、启用和禁用 outlines
- **Hint 支持**: 支持扫描方法、连接方法、连接顺序等常见 hint 类型
- **路径过滤**: 通过 set_rel_pathlist_hook 强制执行扫描方法 hints

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

当 `pg_outline.mode` 设置为 'auto' 时，扩展会自动为每个查询生成 outline 数据:

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
[main] SeqScan(test_table1)
[main] IndexScan(test_table2)
[main] HashJoin(test_table1 test_table2)
[main] Leading((test_table1 test_table2))
END_OUTLINE_DATA
*/
```

**注意**: 生成的 hints 现在包含 `[query_name]` 前缀，用于在应用时精确匹配到对应的查询节点。

### 手动 Outline 管理

您可以手动创建、管理和应用 outlines:

```sql
-- 创建 outline（使用带 query_name 前缀的格式）
SELECT pg_outline_create(
    'my_outline',  -- outline 名称
    'SELECT * FROM test_table1 WHERE value > ?',  -- 查询模式
    '[main] SeqScan(test_table1)'  -- hints (必须包含 [query_name] 前缀)
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

### Outline 应用与计划固定

当相同模式的 SQL 再次执行时，pg_outline 会自动应用存储的 hints，重现原始执行计划:

```sql
-- 1. 首先，使用 auto 模式捕获计划
SET pg_outline.mode = 'auto';
EXPLAIN SELECT * FROM t1 WHERE id < 100;
-- 记录生成的 hints

-- 2. 手动创建 outline（或使用 pg_outline_create_from_sql）
SELECT pg_outline_create(
    'test_outline',
    'SELECT * FROM t1 WHERE id < ?;',
    '[main] SeqScan(t1)'
);

-- 3. 切换到 manual 模式并启用详细日志
SET pg_outline.mode = 'manual';
SET client_min_messages = 'DEBUG1';

-- 4. 执行相同模式的查询，hints 将自动应用
SELECT * FROM t1 WHERE id < 100;
-- 输出会显示:
-- DEBUG: pg_outline: computed fingerprint: ...
-- DEBUG: pg_outline: retrieved hints from stored outline
-- DEBUG: pg_outline: parsed 1 hints from stored outline
-- DEBUG: pg_outline: applied hint to query 'main': SeqScan(t1)
-- DEBUG: pg_outline: filtering paths for relation 't1' based on hints

-- 5. 验证计划是否匹配
EXPLAIN SELECT * FROM t1 WHERE id < 100;
-- 应该显示 Seq Scan，即使索引扫描可能更优
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

- `Leading(...)` - 指定连接顺序，使用嵌套括号格式控制连接树结构
  - 简单顺序: `Leading(t1 t2 t3)` - 从左到右连接
  - 嵌套格式: `Leading((t1 t2) (t3 t4))` - 先连接 t1 和 t2，再连接 t3 和 t4，最后连接两个结果
  - 复杂嵌套: `Leading(((t1 t2) t3) t4)` - 精确控制每一步的连接顺序

**注意**: pg_outline 自动生成的 Leading hint 采用嵌套括号格式，以准确表示计划树的连接顺序。这种格式与 pg_hint_plan 的语法一致。

## Outline 数据格式

生成的 outline 数据遵循类似 OceanBase 和 Oracle 的格式，并包含 query_name 前缀:

```
/*+
BEGIN_OUTLINE_DATA
[query_name] <hint1>
[query_name] <hint2>
...
END_OUTLINE_DATA
*/
```

示例:

```
/*+
BEGIN_OUTLINE_DATA
[main] SeqScan(test_table1)
[main] IndexScan(test_table2)
[main] HashJoin(test_table1 test_table2)
[main] Leading((test_table1 test_table2))
END_OUTLINE_DATA
*/
```

**Hint 格式说明**:
- `[query_name]`: 查询名称前缀，用于标识 hint 应用到哪个查询节点
  - `[main]`: 主查询
  - `[cte_<name>]`: CTE 查询
  - `[subquery_N]`: 子查询
  - `[sublink_N]`: SubLink 子查询
- `hint_text`: 具体的 hint 内容（扫描方法、连接方法等）

对于复杂的多表连接:

```sql
SELECT * FROM t1
  JOIN t2 ON t1.id = t2.t1_id
  JOIN t3 ON t2.id = t3.t2_id
  JOIN t4 ON t3.id = t4.t3_id;
```

生成的 Leading hint 可能类似:

```
Leading(((t1 t2) t3) t4)
```

这表示: 先连接 t1 和 t2，然后将结果与 t3 连接，最后与 t4 连接。

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

1. **Planner Hook**: 拦截查询规划，从选择的计划中生成 hints，并在规划前应用存储的 hints
2. **Set Rel Pathlist Hook**: 在生成路径阶段过滤扫描路径，强制执行扫描方法 hints
3. **Executor Hooks**: 跟踪查询执行并显示 outline 数据
4. **计划分析**: 递归分析计划树以提取相关 hints，支持带位置信息的 hint 重构
5. **Hint 存储**: 将 hints 存储在系统表中以供后续重用
6. **Hint 应用**: 完整的 hint 解析和应用流程，包括：
   - 解析存储的 hint 字符串（`[query_name] hint_text` 格式）
   - 通过 metadata hash table 将 hints 关联到对应的 Query 节点
   - 在规划阶段过滤路径，只保留 hint 指定的扫描方法

## 与类似工具的比较

### vs. pg_hint_plan

- `pg_hint_plan`: 专注于应用手动指定的 hints，通过 SQL 注释语法
- `pg_outline`: 自动从执行计划生成 hints 并作为 outlines 管理，支持计划捕获和重现
- 相似点: 都使用类似的 hint 语法（SeqScan、IndexScan、Leading 等）
- 不同点: pg_outline 增加了 `[query_name]` 前缀以支持复杂查询的精确 hint 应用

### vs. OceanBase Outline

- 类似的概念和 API 设计
- 适配 PostgreSQL 的规划器和执行器架构
- 兼容 PostgreSQL 的标准查询优化
- 实现了完整的 outline 生成、存储和应用流程

## 限制

当前版本的限制:

1. Leading hint 强制执行是部分的 - hints 可以被检测和记录，但完整的连接顺序强制执行需要复杂的嵌套语法解析
2. 当前使用简单的字符串匹配检查 hints，未来可以改进为完整的 hint 解析器
3. 如果 hint 无法应用（如表名不匹配），系统会回退到正常规划

## 未来改进

计划中的改进:

- 完整的 Leading hint 强制执行（需要复杂的嵌套语法解析和自定义连接树构建）
- 支持并行查询 hints
- 扩展的 hint 类型(例如 SET、ROWS hints)
- 与 pg_stat_statements 集成以自动创建 outline
- Outline 导入/导出功能
- 更复杂的 hint 语法解析器
- Cost 调整作为路径过滤的替代方案
- Hint 冲突检测和警告

## 已完成功能 (Version 1.0)

以下功能已经完全实现:

1. **查询参数化** ✓
   - 自动替换字面值（数字和字符串）为 `?` 占位符
   - 允许不同字面值的查询匹配同一个 outline
   - 示例: `SELECT * FROM t1 WHERE id < 100` 和 `SELECT * FROM t1 WHERE id < 500` 匹配同一个模式 `select * from t1 where id < ?`

2. **多表连接 Hints** ✓
   - 支持包含多个关系的连接 hints: `HashJoin(t1 t2 t3)`
   - `extract_relations_from_join_hint()` 函数解析空格分隔的表名
   - 实现对复杂多路连接的连接方法控制

3. **Leading Hint 钩子集成** ✓
   - 注册了 `join_search_hook` 以拦截连接顺序规划
   - `outline_join_search()` 函数检测 `Leading((t1 t2) t3)` 格式的 Leading hints
   - 为未来完整连接顺序强制执行奠定基础
   - 当前记录检测到的 hints 用于调试

## 技术实现细节

### Hint 解析流程

pg_outline 实现了完整的 hint 解析和应用机制:

1. **normalize_query()**: 参数化查询以实现模式匹配
   - 将所有字符串字面值替换为 `?` 占位符
   - 将所有数字字面值替换为 `?` 占位符
   - 保留标识符、关键字和结构
   - 允许不同字面值的查询匹配同一个模式
   - 示例: `SELECT * FROM t1 WHERE id < 100` → `select * from t1 where id < ?`

2. **extract_relations_from_join_hint()**: 解析多表连接 hints
   - 输入: `HashJoin(t1 t2 t3)` 或 `NestLoop(orders items)`
   - 输出: 关系名列表: ["t1", "t2", "t3"]
   - 用于验证和应用连接方法 hints

3. **parse_stored_hints()**: 解析存储的 hint 字符串
   - 输入格式: `[query_name] hint_text\n[query_name2] hint_text2`
   - 输出: ParsedHint 结构列表，每个包含 query_name 和 hint_text

4. **apply_hints_to_query()**: 将解析的 hints 关联到 Query 节点
   - 通过 metadata hash table 查找匹配的 query_name
   - 将 hint_text 存储到对应的 QueryMetadataEntry
   - 支持多个 hints 合并到同一个查询

5. **outline_set_rel_pathlist()**: 在路径生成阶段过滤路径
   - 实现 set_rel_pathlist_hook
   - 获取当前关系的查询 hints
   - 检查此表的扫描方法 hints
   - 过滤 rel->pathlist 只保留匹配的路径类型
   - 支持 SeqScan、IndexScan、IndexOnlyScan hints

6. **outline_join_search()**: 检测连接顺序的 Leading hints
   - 实现 join_search_hook
   - 解析 Leading hint 格式: `Leading((t1 t2) t3)`
   - 记录检测到的 hints 用于调试
   - 为未来完整连接顺序强制执行奠定基础

### 工作流程

**生成阶段** (auto 模式):
1. 用户执行查询
2. outline_planner 钩子拦截规划过程
3. 系统生成执行计划
4. 从计划中提取 hints（带位置信息）
5. 以 `[query_name] hint_text` 格式生成 hint 字符串
6. 如果启用了 display_hints，显示生成的 outline

**应用阶段** (manual 模式):
1. 用户执行相同模式的 SQL
2. outline_planner 使用 normalize_query() 标准化查询（参数化字面值）
3. 从标准化的查询计算查询指纹（fingerprint）
4. 从 pg_outline_data 表检索匹配的 stored hints
5. 调用 parse_stored_hints() 解析 hint 字符串为 ParsedHint 列表
6. 调用 apply_hints_to_query() 将 hints 关联到 Query 节点
7. 设置 active_outline_hints 全局变量
8. 调用 standard_planner() 开始规划
9. 规划期间，outline_set_rel_pathlist() 钩子被调用处理扫描 hints
10. 规划期间，outline_join_search() 钩子被调用处理连接顺序 hints
11. 钩子过滤路径，只保留 hint 指定的方法
12. 规划器从过滤后的路径中选择（被限制为 hint 指定的方法）
13. 规划完成后，清理 active_outline_hints
14. 返回 PlannedStmt（应该匹配原始计划）

### 关键数据结构

```c
typedef struct ParsedHint
{
    char *query_name;  /* e.g., "main", "sublink_0" */
    char *hint_text;   /* e.g., "SeqScan(t1)", "IndexScan(t2)" */
} ParsedHint;
```

全局变量:
- `active_outline_hints`: 当前查询的活动 hints 列表
- `prev_set_rel_pathlist_hook`: 保存的前一个 set_rel_pathlist_hook 指针
- `prev_join_search_hook`: 保存的前一个 join_search_hook 指针

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
    '[main] HashJoin(large_table1 large_table2)'
);

-- 生产环境: 使用不同的计划
SELECT pg_outline_create(
    'report_query_prod',
    'SELECT * FROM large_table1 t1 JOIN large_table2 t2 ON ...',
    '[main] MergeJoin(large_table1 large_table2) [main] IndexScan(large_table1)'
);

-- 在不同环境间切换
SELECT pg_outline_enable('report_query_prod');
SELECT pg_outline_disable('report_query_dev');
```

### 示例 3: 调试 Hint 应用

```sql
-- 启用详细日志以查看 hint 应用过程
SET client_min_messages = 'DEBUG1';
SET pg_outline.mode = 'manual';

-- 执行查询
SELECT * FROM t1 WHERE id < 100;

-- 日志输出示例:
-- DEBUG: pg_outline: computed fingerprint: 1234567890
-- DEBUG: pg_outline: retrieved hints from stored outline: [main] SeqScan(t1)
-- DEBUG: pg_outline: parsed 1 hints from stored outline
-- DEBUG: pg_outline: applied hint to query 'main': SeqScan(t1)
-- DEBUG: pg_outline: filtering paths for relation 't1' based on hints
-- DEBUG: pg_outline: filtered from 3 to 1 paths for 't1'
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

-- 启用详细日志以查看应用过程
SET client_min_messages = 'DEBUG1';

-- 检查 hint 格式是否正确（必须包含 [query_name] 前缀）
SELECT outline_name, hint_string FROM pg_outline_data
WHERE outline_name = 'your_outline';

-- 确保 hint 格式为: [main] SeqScan(table) 而不是 SeqScan(table)
```

### Hints 未生效

如果 hints 已应用但计划没有改变:

1. **检查表名是否正确**: Hint 中的表名必须与查询中的表名完全匹配
2. **检查 hint 类型**: 确保 hint 类型（SeqScan、IndexScan 等）适用于该表
3. **查看日志**: DEBUG1 级别的日志会显示路径过滤过程
4. **验证路径存在**: 如果 hint 指定的路径不存在（如没有索引时使用 IndexScan），系统会回退到正常规划

```sql
-- 查看可用的路径
SET enable_seqscan = off;  -- 禁用顺序扫描
EXPLAIN SELECT * FROM t1 WHERE id < 100;  -- 查看是否有索引扫描路径
SET enable_seqscan = on;   -- 恢复设置
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
