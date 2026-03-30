# pg_outline 使用指南 (User Guide in Chinese)

## 概述

`pg_outline` 是一个 PostgreSQL 扩展，实现了类似 OceanBase 的 outline 功能，提供持久化的计划提示管理以优化查询。它允许 DBA 存储并自动应用查询提示来控制 PostgreSQL 规划器的行为，而无需修改应用程序代码。

## 核心特性

- **持久化提示存储**：在目录表中存储查询提示，跨会话持久化
- **自动提示应用**：当匹配的查询执行时自动应用存储的提示
- **内联提示**：支持使用 `/*+ hint1 hint2 ... */` 语法的内联提示
- **多种匹配模式**：支持精确匹配、标准化匹配和指纹匹配
- **每个查询的提示**：支持包含 CTE、子查询和 SubLink 的复杂查询
- **自动生成**：从实际执行计划自动生成 outline（计划功能）
- **pg_hint_plan 兼容**：使用 pg_hint_plan 语法的提示

## 架构设计

### OceanBase Outline 原理对比

OceanBase 的 outline 功能通过以下方式实现：

1. **SQL 签名匹配**：对 SQL 进行标准化处理，生成唯一签名
2. **Hint 存储**：将 hint 与 SQL 签名关联存储在系统表中
3. **执行期匹配**：SQL 执行时根据签名查找并应用对应的 hint
4. **优先级控制**：内联 hint > outline hint > 默认优化器行为

### pg_outline 实现原理

本实现参考 OceanBase 设计，结合 PostgreSQL 特性：

1. **Planner Hook 机制**：通过 planner_hook 在优化阶段介入
2. **SPI 查询目录表**：使用 SPI 接口查询 pg_outline.outlines 表
3. **Hint 解析和应用**：借用 pg_hint_plan 的 hint 语法和解析逻辑
4. **查询匹配策略**：支持精确匹配、标准化匹配和指纹匹配

### 组件说明

1. **目录表**：
   - `pg_outline.outlines`：存储 outline 定义的主表
   - `pg_outline.query_hints`：复杂查询的每个查询提示
   - `pg_outline.config`：配置参数表

2. **Planner Hooks**：
   - `outline_planner`：主规划器钩子，用于提示提取和应用
   - `outline_ExecutorStart`：执行器启动钩子
   - `outline_ExecutorEnd`：执行器结束钩子，用于自动生成

3. **SQL 函数**：
   - `pg_outline.create_outline()`：创建新 outline
   - `pg_outline.drop_outline()`：删除 outline
   - `pg_outline.enable_outline()`：启用 outline
   - `pg_outline.disable_outline()`：禁用 outline
   - `pg_outline.create_outline_from_plan()`：从执行计划生成 outline
   - `pg_outline.match_query()`：匹配查询并返回适用的提示

## 安装配置

### 从源码构建

```bash
cd contrib/pg_outline
make
make install
```

### PostgreSQL 配置

在 `postgresql.conf` 中添加：

```ini
shared_preload_libraries = 'pg_outline'
```

重启 PostgreSQL，然后创建扩展：

```sql
CREATE EXTENSION pg_outline;
```

## 配置参数

通过 GUC 参数配置：

```sql
-- 全局启用/禁用 outline 功能
SET pg_outline.enabled = true;

-- 启用自动生成 outline
SET pg_outline.auto_generate = false;

-- 在 EXPLAIN 输出中显示应用的提示
SET pg_outline.display_hints = false;

-- 设置日志级别：debug, notice, warning, error
SET pg_outline.log_level = 'notice';

-- 查询匹配模式：exact, normalized, fingerprint
SET pg_outline.match_mode = 'exact';
```

## 使用示例

### 基本 Outline 创建

```sql
-- 创建带扫描提示的 outline
SELECT pg_outline.create_outline(
    'myoutline1',                           -- outline 名称
    'SELECT * FROM users WHERE id = 123',   -- 查询文本
    'SeqScan(users)',                       -- 提示
    'public',                               -- 模式
    '强制对 users 表进行顺序扫描'             -- 描述
);

-- 创建带连接提示的 outline
SELECT pg_outline.create_outline(
    'myoutline2',
    'SELECT * FROM orders o JOIN customers c ON o.customer_id = c.id',
    'HashJoin(o c) IndexScan(orders)',
    'public',
    '使用哈希连接和索引扫描'
);
```

### 内联提示

```sql
-- 在查询中直接使用内联提示
SELECT /*+ SeqScan(users) */ * FROM users WHERE age > 25;

SELECT /*+ NestLoop(o c) IndexScan(orders order_idx) */
    o.*, c.name
FROM orders o
JOIN customers c ON o.customer_id = c.id;
```

### 管理 Outline

```sql
-- 列出所有 outline
SELECT * FROM pg_outline.outlines;

-- 仅列出启用的 outline
SELECT * FROM pg_outline.active_outlines;

-- 禁用 outline
SELECT pg_outline.disable_outline('myoutline1', 'public');

-- 启用 outline
SELECT pg_outline.enable_outline('myoutline1', 'public');

-- 删除 outline
SELECT pg_outline.drop_outline('myoutline1', 'public');

-- 获取统计信息
SELECT * FROM pg_outline.get_statistics();
```

### 匹配模式

```sql
-- 精确匹配（默认）- 查询必须逐字符匹配
SET pg_outline.match_mode = 'exact';

-- 标准化匹配 - 忽略空白和大小写差异
SET pg_outline.match_mode = 'normalized';

-- 指纹匹配 - 匹配查询结构（计划功能）
SET pg_outline.match_mode = 'fingerprint';
```

## 支持的 Hint 类型

`pg_outline` 使用 pg_hint_plan 兼容语法：

### 扫描方法提示

- `SeqScan(table)`：强制顺序扫描
- `IndexScan(table [index])`：强制索引扫描
- `IndexOnlyScan(table [index])`：强制仅索引扫描
- `BitmapScan(table [index])`：强制位图扫描
- `TidScan(table)`：强制 TID 扫描
- `NoSeqScan(table)`：禁止顺序扫描
- `NoIndexScan(table)`：禁止索引扫描

### 连接方法提示

- `NestLoop(table1 table2 ...)`：强制嵌套循环连接
- `HashJoin(table1 table2 ...)`：强制哈希连接
- `MergeJoin(table1 table2 ...)`：强制归并连接
- `NoNestLoop(table1 table2 ...)`：禁止嵌套循环连接
- `NoHashJoin(table1 table2 ...)`：禁止哈希连接
- `NoMergeJoin(table1 table2 ...)`：禁止归并连接

### 连接顺序提示

- `Leading(table1 table2 table3 ...)`：指定连接顺序
- `Leading((table1 table2) table3)`：使用分组指定连接顺序

## 实际应用场景

### 场景 1：稳定查询计划

某些查询由于数据分布变化，执行计划可能不稳定。使用 outline 固定计划：

```sql
-- 创建 outline 固定执行计划
SELECT pg_outline.create_outline(
    'stable_plan_query1',
    'SELECT * FROM large_table WHERE status = ''active''',
    'IndexScan(large_table status_idx) Rows(large_table #1000)',
    'public',
    '固定该查询的执行计划'
);
```

### 场景 2：优化慢查询

发现某个查询执行慢，通过添加 hint 优化：

```sql
-- 原查询使用了错误的连接方式
-- 通过 outline 强制使用更好的连接方式
SELECT pg_outline.create_outline(
    'optimize_slow_query',
    'SELECT o.*, p.* FROM orders o JOIN products p ON o.product_id = p.id WHERE o.order_date > ''2025-01-01''',
    'HashJoin(o p) IndexScan(orders order_date_idx)',
    'public',
    '优化订单产品连接查询'
);
```

### 场景 3：应用无法修改的情况

当应用代码无法修改但需要调整执行计划时：

```sql
-- 应用固定的 SQL，但需要改变执行计划
SELECT pg_outline.create_outline(
    'app_query_optimization',
    'SELECT user_id, COUNT(*) FROM orders GROUP BY user_id',
    'HashAggregate Parallel(orders 4 hard)',
    'public',
    '为应用程序查询添加并行处理'
);
```

## 最佳实践

### 1. 使用描述字段

创建 outline 时添加详细描述，说明为什么需要这个 outline：

```sql
SELECT pg_outline.create_outline(
    'important_outline',
    'SELECT ...',
    'IndexScan(...)',
    'public',
    '原因：统计信息过时导致选择了错误的计划；影响：该查询每秒执行 1000 次；解决方案：强制使用索引扫描'
);
```

### 2. 定期审查 Outline

定期检查并清理不再需要的 outline：

```sql
-- 查看所有 outline 及其创建时间
SELECT name, created_at, description
FROM pg_outline.outlines
ORDER BY created_at DESC;

-- 删除过时的 outline
SELECT pg_outline.drop_outline('old_outline', 'public');
```

### 3. 测试验证

创建 outline 后，验证它确实改善了查询性能：

```sql
-- 禁用 outline 测试
SELECT pg_outline.disable_outline('test_outline', 'public');
EXPLAIN ANALYZE SELECT ...;

-- 启用 outline 测试
SELECT pg_outline.enable_outline('test_outline', 'public');
EXPLAIN ANALYZE SELECT ...;
```

### 4. 监控日志

启用日志查看 outline 应用情况：

```sql
SET pg_outline.log_level = 'notice';
SET pg_outline.display_hints = true;
```

## 故障排查

### Outline 未生效

1. 检查 outline 功能是否启用：
   ```sql
   SHOW pg_outline.enabled;
   ```

2. 检查 outline 是否存在且已启用：
   ```sql
   SELECT * FROM pg_outline.outlines WHERE name = 'myoutline';
   ```

3. 检查查询文本是否完全匹配（如果使用精确匹配模式）

4. 启用调试日志：
   ```sql
   SET pg_outline.log_level = 'debug';
   ```

### 构建错误

- 确保安装了 PostgreSQL 开发头文件
- 检查 PostgreSQL 版本兼容性（需要 12+）
- 验证 `pg_config` 在 PATH 中

### 性能问题

如果 outline 查找影响性能：

1. 使用 `normalized` 匹配模式而不是 `exact`
2. 为常用查询创建索引
3. 减少启用的 outline 数量
4. 考虑使用内联提示而不是存储的 outline

## 与 OceanBase Outline 的对比

### 相似之处
- 持久化的 hint 存储在目录表中
- 查询执行期间自动应用 hint
- 支持启用/禁用 outline
- 支持多种 hint 类型

### 差异之处
- PostgreSQL 使用 planner hook 而不是 OceanBase 的优化器框架
- 使用 pg_hint_plan 语法而不是 OceanBase 特定语法
- 不同的查询匹配机制
- PostgreSQL 支持更灵活的扩展机制

## 实现状态

### 已完成功能
- ✅ 基本 outline 目录模式
- ✅ Planner hook 基础设施
- ✅ SQL 管理函数
- ✅ 内联提示提取
- ✅ GUC 配置参数
- ✅ 查询标准化匹配

### 计划功能
- ⏳ 完整的 pg_hint_plan hint 解析器集成
- ⏳ 从执行计划自动生成
- ⏳ CTE 和子查询的每个查询提示
- ⏳ 查询指纹匹配
- ⏳ 通过 SQL ID 绑定 outline
- ⏳ Outline 演化跟踪
- ⏳ 性能统计收集

## 技术细节

### Hint 应用流程

```
查询执行
    ↓
outline_planner hook
    ↓
1. 从查询文本提取内联 hint (/*+ ... */)
    ↓
2. 如果没有内联 hint，查找存储的 outline
    ↓
3. 将 hint 应用到 Query 结构
    ↓
standard_planner / prev_planner_hook
    ↓
优化后的计划
```

### Hint 优先级

```
1. 内联 Hint (/*+ ... */)
    ↓
2. 存储的 Outline (pg_outline.outlines)
    ↓
3. 默认规划器行为
```

## 参考资料

- [OceanBase Outline 文档](https://www.oceanbase.com/docs/outline)
- [pg_hint_plan 扩展](https://github.com/ossc-db/pg_hint_plan)
- [PostgreSQL Planner Hooks](https://www.postgresql.org/docs/current/planner.html)

## 版本历史

### 1.0（初始版本）
- 基本 outline 功能
- 内联提示支持
- SQL 管理函数
- 多种匹配模式
- GUC 配置

## 许可证

本扩展使用 PostgreSQL 许可证，与 PostgreSQL 本身相同的许可证。

---

更多信息和更新，请访问项目仓库。
