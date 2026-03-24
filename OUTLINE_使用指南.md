# PostgreSQL Outline 功能使用指南

## 概述

Outline（执行计划固定）功能是PostgreSQL的一项增强特性，参考了OceanBase的Outline接口设计，并基于pg_hint_plan的Hint实现。该功能允许数据库管理员（DBA）通过存储优化器提示（Hints）来固定和稳定特定SQL查询的执行计划，防止因统计信息变化、数据量增长等因素导致的性能退化。

## 核心功能

Outline系统提供以下核心能力：

1. **执行计划固定**：为关键查询固定执行计划，防止性能回退
2. **基于Hint的控制**：使用pg_hint_plan兼容的Hint语法进行细粒度控制
3. **持久化存储**：将Outline持久化存储在`pg_outline`系统表中
4. **动态管理**：无需修改应用代码即可启用/禁用Outline
5. **自动生成Hint**：自动从执行计划生成Hint，无需手动编写（新功能）

## 自动Outline生成功能（新增）

### 功能说明

系统现在支持自动从执行计划生成Outline Data（Hint数据），并在每次SQL执行后自动显示在终端。这个功能参考了OceanBase和Oracle的设计，使用类似的格式输出。

### 配置参数

系统提供以下GUC配置参数：

#### outline.display_hints

**类型**: `boolean`
**默认值**: `off`
**上下文**: `PGC_USERSET` (可以在会话级别设置)
**描述**: 控制是否在每次查询执行后自动生成并显示Outline Data

当启用此参数时，PostgreSQL会：
1. 在查询执行完成后自动分析执行计划
2. 从执行计划中提取扫描和连接方法Hint
3. 将Hint格式化为OceanBase/Oracle风格的Outline Data
4. 通过NOTICE消息将结果显示到客户端

**使用方法**：
```sql
-- 为当前会话启用
SET outline.display_hints = on;

-- 为当前会话禁用
SET outline.display_hints = off;

-- 为整个数据库设置默认值（需要超级用户权限）
ALTER DATABASE mydb SET outline.display_hints = on;
```

**注意事项**：
- 此功能仅影响查询结果的显示，不影响查询执行性能
- 对于复杂查询，生成Hint可能需要少量额外开销
- 建议仅在需要分析执行计划时启用

### 使用示例

```sql
-- 1. 启用自动显示
SET outline.display_hints = on;

-- 2. 执行任意SQL查询
SELECT * FROM orders WHERE customer_id = 123;

-- 3. 系统会自动显示生成的Outline Data
NOTICE:  Outline Data:
/*+
BEGIN_OUTLINE_DATA
IndexScan(orders idx_orders_customer)
END_OUTLINE_DATA
*/

-- 4. 对于包含连接的查询
SELECT o.*, c.name
FROM orders o
JOIN customers c ON o.customer_id = c.id
WHERE c.region = 'Asia';

NOTICE:  Outline Data:
/*+
BEGIN_OUTLINE_DATA
IndexScan(customers idx_customers_region)
IndexScan(orders idx_orders_customer)
HashJoin(customers orders)
END_OUTLINE_DATA
*/
```

### 输出格式

生成的Outline Data采用标准的SQL注释格式，包含：

- 开始标记：`/*+ BEGIN_OUTLINE_DATA`
- Hint列表：每个Hint占一行
- 结束标记：`END_OUTLINE_DATA */`

这种格式与OceanBase和Oracle的Outline格式兼容，便于理解和手动使用。

### 工作流程

1. **执行查询**：PostgreSQL执行SQL查询并生成执行计划
2. **提取Hint**：系统自动遍历执行计划树，识别扫描方法和连接方法
3. **格式化输出**：将提取的Hint格式化为OceanBase/Oracle风格
4. **显示结果**：通过NOTICE消息将Outline Data发送到客户端终端

### 应用场景

1. **学习优化器行为**：查看PostgreSQL为特定查询选择了哪些执行策略
2. **快速创建Outline**：获取自动生成的Hint后，可以直接用于创建持久化Outline
3. **性能分析**：了解执行计划的细节，便于优化调整
4. **文档记录**：保存关键查询的执行计划Hint作为文档

### 从自动生成到手动创建

可以将自动生成的Hint直接用于创建永久Outline：

```sql
-- 步骤1：启用自动显示并执行查询
SET outline.display_hints = on;
SELECT * FROM orders WHERE status = 'pending';

-- 步骤2：系统显示Outline Data
NOTICE:  Outline Data:
/*+
BEGIN_OUTLINE_DATA
SeqScan(orders)
END_OUTLINE_DATA
*/

-- 步骤3：复制Hint内容，创建永久Outline
SET outline.display_hints = off;  -- 可选，关闭自动显示

SELECT pg_create_outline(
    'outline_orders_pending',
    'SELECT * FROM orders WHERE status = $1',
    'SeqScan(orders)'  -- 使用自动生成的Hint
);
```

## sr_plan式录制模式（新增）

### 功能说明

录制模式（Recording Mode）是一个强大的新功能，参考了sr_plan的设计理念，允许自动录制和回放SQL执行计划。与手动创建Outline不同，录制模式可以自动捕获执行计划并创建Outline，大大简化了Outline的管理流程。

### 配置参数

#### outline.recording_mode

**类型**: `boolean`
**默认值**: `off`
**上下文**: `PGC_USERSET` (可以在会话级别设置)
**描述**: 启用自动Outline录制模式

当启用录制模式时：
1. 每个执行的查询都会被自动分析
2. 从实际执行计划中提取Hint
3. 自动创建Outline并存储到`pg_outline`表
4. 为Outline生成唯一名称（格式：`auto_outline_<进程ID>_<计数器>`）

**使用方法**：
```sql
-- 启用录制模式
SET outline.recording_mode = on;

-- 执行查询（计划将被自动录制）
SELECT * FROM customers WHERE region = 'Asia';

-- 禁用录制模式
SET outline.recording_mode = off;
```

### 使用流程

#### 步骤1：启用录制模式

```sql
-- 开启录制
SET outline.recording_mode = on;
```

#### 步骤2：执行SQL（可选择性添加手动Hint）

```sql
-- 方式1：直接执行SQL，记录默认执行计划
SELECT * FROM customers WHERE region = 'Asia';

-- 方式2：使用手动Hint强制特定计划，然后录制
-- （如果需要固定特定的执行策略）
SET enable_seqscan = off;  -- 强制使用索引
SELECT * FROM customers WHERE region = 'Asia';
SET enable_seqscan = on;   -- 恢复默认设置

-- 系统自动提示：
-- NOTICE:  Created outline "auto_outline_12345_1" for query
```

#### 步骤3：关闭录制模式

```sql
SET outline.recording_mode = off;
```

#### 步骤4：验证Outline已创建

```sql
-- 查看自动创建的Outline
SELECT outlinename, outlinequery, outlinehints
FROM pg_outline
WHERE outlinename LIKE 'auto_outline%';
```

#### 步骤5：测试回放

现在，当你再次执行相同的查询时，系统会自动应用录制的Outline：

```sql
-- 执行相同查询（查询会被规范化匹配）
SELECT * FROM customers WHERE region = 'Asia';

-- 系统会自动使用之前录制的Outline
-- 可以用EXPLAIN验证：
EXPLAIN SELECT * FROM customers WHERE region = 'Asia';
```

### 查询规范化与匹配

录制模式使用智能查询规范化来匹配查询：

**规范化规则**：
- 转换为小写（字符串字面量除外）
- 折叠空白字符（多个空格合并为一个）
- 去除首尾空白
- 保留字符串字面量原样

**匹配示例**：

以下查询都会匹配同一个Outline：

```sql
-- 原始查询
SELECT * FROM customers WHERE region = 'Asia';

-- 不同大小写（SQL关键字外）
SELECT * FROM CUSTOMERS WHERE REGION = 'Asia';

-- 不同空白
SELECT   *   FROM   customers   WHERE   region='Asia';

-- 所有这些都会使用相同的录制Outline！
```

### 完整示例

#### 示例1：简单查询录制与回放

```sql
-- 1. 启用录制
SET outline.recording_mode = on;
SET outline.display_hints = on;  -- 可选：查看录制的内容

-- 2. 执行查询
SELECT * FROM customers WHERE region = 'Asia';

-- 输出：
-- NOTICE:  Outline Data:
-- /*+
-- BEGIN_OUTLINE_DATA
-- IndexScan(customers idx_customer_region)
-- END_OUTLINE_DATA
-- */
-- NOTICE:  Created outline "auto_outline_56789_1" for query

-- 3. 关闭录制
SET outline.recording_mode = off;
SET outline.display_hints = off;

-- 4. 验证Outline
SELECT outlinename, outlinehints FROM pg_outline;
--      outlinename      |               outlinehints
-- ----------------------+------------------------------------------
--  auto_outline_56789_1 | IndexScan(customers idx_customer_region)

-- 5. 测试回放（相同查询会自动使用Outline）
EXPLAIN SELECT * FROM customers WHERE region = 'Asia';
-- 应该显示Index Scan using idx_customer_region
```

#### 示例2：连接查询录制

```sql
-- 1. 启用录制
SET outline.recording_mode = on;

-- 2. 强制特定连接方式（可选）
SET enable_hashjoin = off;  -- 禁用HashJoin
SET enable_mergejoin = off; -- 禁用MergeJoin

-- 3. 执行连接查询
SELECT c.name, o.amount
FROM customers c
JOIN orders o ON c.id = o.customer_id
WHERE c.region = 'Asia';

-- NOTICE:  Created outline "auto_outline_56789_2" for query

-- 4. 恢复设置并关闭录制
SET enable_hashjoin = on;
SET enable_mergejoin = on;
SET outline.recording_mode = off;

-- 5. 查看录制的Hint
SELECT outlinename, outlinehints FROM pg_outline
WHERE outlinename = 'auto_outline_56789_2';
--      outlinename      |               outlinehints
-- ----------------------+------------------------------------------
--  auto_outline_56789_2 | IndexScan(customers idx_customer_region)
--                       | SeqScan(orders)
--                       | NestLoop(customers orders)

-- 6. 后续执行会自动使用NestLoop连接
SELECT c.name, o.amount
FROM customers c
JOIN orders o ON c.id = o.customer_id
WHERE c.region = 'Asia';
```

### 与手动Outline的对比

| 特性 | 手动Outline (`pg_create_outline`) | 录制模式 (`outline.recording_mode`) |
|------|----------------------------------|-------------------------------------|
| 创建方式 | 手动调用函数 | 自动创建 |
| Outline名称 | 用户指定 | 自动生成 |
| 适用场景 | 需要明确控制的固定Outline | 快速捕获当前执行计划 |
| 学习成本 | 需要了解Hint语法 | 无需了解Hint语法 |
| 灵活性 | 高（可精确控制） | 中（基于实际执行计划） |
| 重复处理 | 允许覆盖 | 自动跳过重复 |

### 最佳实践

1. **录制前准备**
   - 确保统计信息是最新的：`ANALYZE tables;`
   - 测试环境与生产环境数据分布相似
   - 对于关键查询，可能需要手动Hint引导计划

2. **录制时机选择**
   - 在业务低峰期录制，减少干扰
   - 确保录制的查询是性能稳定的版本
   - 如需特定计划，先用GUC参数调整（如`enable_*`系列）

3. **录制后验证**
   - 使用EXPLAIN检查录制的计划
   - 在测试环境验证回放效果
   - 监控查询性能是否符合预期

4. **Outline管理**
   - 定期检查自动创建的Outline
   - 重命名重要的auto_outline为有意义的名称
   - 删除不再需要的Outline

### 故障排查

**问题1：Outline没有被创建**
- 检查`outline.recording_mode`是否为`on`
- 确认查询成功执行（没有错误）
- 查看日志是否有错误信息

**问题2：查询没有使用Outline**
- 检查Outline是否启用：`SELECT * FROM pg_outline WHERE outlineenabled = false;`
- 验证查询规范化后是否匹配：使用`EXPLAIN`查看
- 确认Outline的Hint格式正确

**问题3：重复录制相同查询**
- 系统会自动检测并跳过已存在的Outline
- 如果想更新，先删除旧Outline：`SELECT pg_drop_outline('outline_name');`

### 技术实现细节

录制模式的核心实现包括：

1. **查询规范化**：`normalize_query_string()`函数
   - 处理大小写
   - 标准化空白
   - 保留字符串字面量

2. **Outline查找**：`get_hints_for_query()`函数
   - 规范化查询字符串
   - 在`pg_outline`中查找匹配项
   - 解析并应用Hint

3. **自动录制**：`record_outline_for_query()`函数
   - 检测重复Outline
   - 生成唯一名称
   - 插入到系统目录

4. **优化器集成**
   - 在查询规划前注入Hint
   - 在查询执行后捕获计划
   - 通过PostgreSQL optimizer hooks实现

### 与sr_plan的区别

虽然参考了sr_plan的设计理念，但实现方式有本质不同：

| 特性 | sr_plan | 本实现（基于Outline） |
|------|---------|---------------------|
| 计划固定方式 | 直接序列化执行计划 | 通过注入Hint影响优化器 |
| 灵活性 | 固定整个计划树 | 可以部分约束（仅Hint覆盖的部分） |
| 适应性 | 统计信息变化可能失效 | Hint引导，优化器仍可优化细节 |
| 实现复杂度 | 需要计划树序列化/反序列化 | 基于现有Hint机制 |
| 维护成本 | 高（计划格式变化需要适配） | 低（Hint语法相对稳定） |

## 系统架构

### 主要组件

#### 1. 系统目录表 (`pg_outline`)

位置：`src/include/catalog/pg_outline.h`

`pg_outline`系统目录表存储Outline定义，包含以下字段：
- `oid` - 对象标识符
- `outlinename` - Outline名称
- `outlinenamespace` - 命名空间OID
- `outlineowner` - 所有者OID
- `outlineenabled` - Outline是否启用
- `outlinequery` - 规范化的SQL查询文本（查询签名）
- `outlinehints` - pg_hint_plan格式的Hint字符串

#### 2. Hint系统

位置：`src/backend/optimizer/outline/`

**outline_hints.c** - Hint解析
- 解析pg_hint_plan格式的Hint字符串
- 支持扫描方法提示：`SeqScan()`、`IndexScan()`、`NoSeqScan()`、`NoIndexScan()`
- 支持连接方法提示：`NestLoop()`、`HashJoin()`、`MergeJoin()`

**outline_plan.c** - 执行计划到Hint的转换
- 从`PlannedStmt`执行计划中提取Hint
- 生成能够重现相同执行计划的Hint字符串
- 遍历计划树识别扫描和连接方法

**outline_apply.c** - Hint应用
- 实现优化器钩子（`set_rel_pathlist_hook`、`set_join_pathlist_hook`）
- 根据活动的Hint过滤路径
- 强制执行扫描和连接方法偏好

#### 3. SQL函数

位置：`src/backend/utils/adt/pg_outline_funcs.c`

提供四个SQL可调用函数用于Outline管理：
- `pg_create_outline(name, query, hints)` - 创建新的Outline
  - **name**: Outline名称
  - **query**: 规范化的SQL查询文本（支持参数占位符$1, $2等）
  - **hints**: Hint字符串，支持两种格式：
    1. OceanBase/Oracle格式：`/*+ BEGIN_OUTLINE_DATA ... END_OUTLINE_DATA */`
    2. 传统格式：`E'hint1\nhint2\nhint3'` 或 `'hint1'`（单个hint）
- `pg_drop_outline(name)` - 删除现有Outline
- `pg_enable_outline(name)` - 启用Outline
- `pg_disable_outline(name)` - 禁用Outline

**Hint格式转换**：当使用OceanBase/Oracle格式时，系统会自动提取`BEGIN_OUTLINE_DATA`和`END_OUTLINE_DATA`之间的内容，去除包裹的注释标记，使Outline Data可以直接用于创建Outline。

## 支持的Hint类型

### 扫描方法Hint

```sql
SeqScan(表名)                    -- 强制使用顺序扫描
IndexScan(表名 索引名)           -- 强制使用指定索引的索引扫描
NoSeqScan(表名)                  -- 禁用顺序扫描
NoIndexScan(表名)                -- 禁用索引扫描
```

### 连接方法Hint

```sql
NestLoop(表1 表2)                -- 强制使用嵌套循环连接
HashJoin(表1 表2)                -- 强制使用哈希连接
MergeJoin(表1 表2)               -- 强制使用归并连接
```

## 使用示例

### 示例1：为查询创建Outline

```sql
-- 创建一个强制在'orders'表上使用顺序扫描的Outline
SELECT pg_create_outline(
    'outline_orders_seq',                      -- Outline名称
    'SELECT * FROM orders WHERE status = $1',  -- 查询文本
    'SeqScan(orders)'                          -- Hint字符串
);
```

**说明**：此Outline将强制查询使用顺序扫描而不是索引扫描，适用于需要扫描大量数据的场景。

### 示例2：创建包含多个Hint的Outline

支持两种格式来指定多个Hint：

**方式1：使用OceanBase/Oracle风格（推荐）**

```sql
-- 使用OceanBase Outline Data格式
SELECT pg_create_outline(
    'outline_complex_query',
    'SELECT * FROM customers c JOIN orders o ON c.id = o.customer_id WHERE c.region = $1',
    '/*+
    BEGIN_OUTLINE_DATA
    IndexScan(customers idx_customer_region)
    HashJoin(customers orders)
    END_OUTLINE_DATA
    */'
);
```

**方式2：使用传统的E字符串格式**

```sql
-- 使用PostgreSQL的E字符串语法
SELECT pg_create_outline(
    'outline_complex_query_2',
    'SELECT * FROM customers c JOIN orders o ON c.id = o.customer_id WHERE c.region = $1',
    E'IndexScan(customers idx_customer_region)\nHashJoin(customers orders)'
);
```

**说明**：
- 第一个Hint：在customers表上使用idx_customer_region索引
- 第二个Hint：在customers和orders之间使用哈希连接
- **推荐使用OceanBase格式**：更清晰易读，可以直接从`outline.display_hints`的输出复制粘贴
- 传统E字符串格式仍然兼容支持

### 示例3：管理Outline

```sql
-- 临时禁用Outline
SELECT pg_disable_outline('outline_orders_seq');

-- 稍后重新启用
SELECT pg_enable_outline('outline_orders_seq');

-- 永久删除Outline
SELECT pg_drop_outline('outline_orders_seq');
```

**使用场景**：
- **禁用**：在测试新的执行计划时临时禁用
- **启用**：恢复已验证的稳定执行计划
- **删除**：不再需要该Outline时清理

### 示例4：查看现有Outline

```sql
-- 查询pg_outline系统表
SELECT outlinename, outlineenabled, outlinehints
FROM pg_outline
WHERE outlinenamespace = (SELECT oid FROM pg_namespace WHERE nspname = 'public');
```

**输出示例**：
```
     outlinename      | outlineenabled |           outlinehints
----------------------+----------------+----------------------------------
 outline_orders_seq   | t              | SeqScan(orders)
 outline_complex_query| t              | IndexScan(customers idx_customer_region)
                      |                | HashJoin(customers orders)
```

## 实际应用场景

### 场景1：防止执行计划变化

**问题**：生产环境中某个关键查询因统计信息更新后执行计划发生变化，性能下降。

**解决方案**：
```sql
-- 步骤1：使用EXPLAIN查看当前良好的执行计划
EXPLAIN SELECT * FROM orders WHERE customer_id = 123;

-- 步骤2：根据执行计划创建Outline
-- 假设当前使用的是索引扫描，我们要固定它
SELECT pg_create_outline(
    'outline_orders_by_customer',
    'SELECT * FROM orders WHERE customer_id = $1',
    'IndexScan(orders idx_orders_customer_id)'
);
```

### 场景2：优化复杂连接查询

**问题**：多表连接查询的执行计划不稳定，有时选择低效的连接顺序。

**解决方案**：
```sql
-- 创建固定连接方法的Outline
SELECT pg_create_outline(
    'outline_sales_report',
    'SELECT c.name, o.total, p.product_name
     FROM customers c
     JOIN orders o ON c.id = o.customer_id
     JOIN products p ON o.product_id = p.id
     WHERE c.region = $1',
    E'IndexScan(customers idx_region)\nHashJoin(customers orders)\nHashJoin(orders products)'
);
```

### 场景3：应对数据量变化

**问题**：表数据量增长后，优化器选择了不合适的执行计划。

**解决方案**：
```sql
-- 对于小表强制使用索引，对于大表强制使用顺序扫描
SELECT pg_create_outline(
    'outline_large_table_scan',
    'SELECT * FROM large_table WHERE status IN ($1, $2, $3)',
    'SeqScan(large_table)'  -- 大范围查询使用顺序扫描更高效
);
```

## 最佳实践

### 1. 命名规范

建议使用清晰的命名规范：
- 使用前缀`outline_`
- 包含表名或业务功能描述
- 示例：`outline_orders_by_status`、`outline_user_login_query`

### 2. 监控和验证

在创建Outline后应进行验证：

```sql
-- 步骤1：查看Outline是否生效
EXPLAIN (ANALYZE, VERBOSE)
SELECT * FROM orders WHERE customer_id = 123;

-- 步骤2：对比有无Outline的执行时间
-- 禁用Outline测试
SELECT pg_disable_outline('outline_orders_by_customer');
-- 运行查询并记录时间
-- 启用Outline测试
SELECT pg_enable_outline('outline_orders_by_customer');
-- 运行查询并记录时间
```

### 3. 文档记录

为每个Outline创建文档记录：
- 创建原因
- 预期性能提升
- 相关的业务场景
- 创建日期和创建人

### 4. 定期审查

建议定期审查Outline的有效性：
- 每季度检查Outline是否仍然必要
- 评估数据量和查询模式的变化
- 删除过时的Outline

## 注意事项和限制

### 当前限制

1. **查询规范化**：当前实现存储原始查询文本。未来版本将实现查询规范化/指纹识别，以匹配具有不同字面值的相似查询。

2. **连接Hint匹配**：连接Hint目前具有有限的匹配逻辑。未来将增强对复杂连接树中关系名称的跟踪。

3. **Leading Hint**：用于控制连接顺序的`LEADING` Hint类型已定义但尚未实现。

4. **子查询支持**：尚不支持子查询的Hint。

**注意**：自动Outline生成功能已实现（通过`outline.display_hints`参数），但完全自动化的Outline创建和管理功能仍在开发中。

### 使用建议

1. **权限管理**：只有超级用户可以创建、修改和删除Outline，确保生产环境的安全性。

2. **性能测试**：在生产环境应用Outline前，务必在测试环境充分验证。

3. **版本兼容性**：升级PostgreSQL版本后，需要重新验证Outline的有效性。

4. **避免过度使用**：不要为所有查询都创建Outline，只针对关键查询和问题查询。

## 故障排查

### 问题1：Outline未生效

**检查步骤**：
```sql
-- 1. 确认Outline已启用
SELECT outlinename, outlineenabled
FROM pg_outline
WHERE outlinename = 'your_outline_name';

-- 2. 确认查询文本匹配
-- 查询文本必须完全匹配（包括空格和大小写）
```

### 问题2：性能未改善

**可能原因**：
- Hint选择不当
- 统计信息过时
- 硬件资源限制

**解决方法**：
```sql
-- 更新统计信息
ANALYZE table_name;

-- 尝试不同的Hint组合
SELECT pg_drop_outline('old_outline');
SELECT pg_create_outline('new_outline', 'query', 'different_hints');
```

### 问题3：查询报错

**常见原因**：
- 引用的索引不存在
- 表名拼写错误
- Hint语法错误

**解决方法**：
```sql
-- 检查索引是否存在
\d table_name

-- 删除有问题的Outline
SELECT pg_drop_outline('problematic_outline');
```

## 技术实现细节

### 系统缓存

实现添加了两个syscache条目用于快速Outline查找：
- `OUTLINENAMENSP` - 按（名称，命名空间）查找，使用索引OutlineNameNspIndexId (OID: 6201)
- `OUTLINEOID` - 按OID查找，使用索引OutlineOidIndexId (OID: 6200)

### 目录对象标识符（OID）

pg_outline系统使用以下OID范围：
- **目录表**: pg_outline (OID: 9900)
- **TOAST表**: pg_outline_toast (OID: 4187)
- **TOAST索引**: pg_outline_toast_index (OID: 4188)
- **OID索引**: pg_outline_oid_index (OID: 6200)
- **名称命名空间索引**: pg_outline_name_nsp_index (OID: 6201)

### 优化器集成

Outline系统通过钩子与PostgreSQL优化器集成：

1. **set_rel_pathlist_hook**：在为基础关系生成路径时应用
   - 根据扫描方法Hint过滤扫描路径
   - 移除不需要的扫描类型

2. **set_join_pathlist_hook**：在生成连接路径时应用
   - 根据连接方法Hint过滤连接路径
   - 强制执行特定的连接算法

### Hint匹配机制

当前Hint匹配依据：
- **关系名称**：必须匹配查询中的表名
- **Hint类型**：扫描或连接方法规范

### 自动Hint生成实现

自动Hint生成功能的技术实现：

1. **执行计划遍历**（`outline_plan.c`）：
   - `plan_to_hints()` - 主入口函数，将PlannedStmt转换为Hint字符串
   - `extract_hints_from_plan()` - 递归遍历计划树，提取扫描和连接Hint
   - `extract_scan_hints()` - 识别SeqScan、IndexScan等扫描节点
   - `extract_join_hints()` - 识别NestLoop、HashJoin、MergeJoin等连接节点

2. **格式化输出**（`outline_plan.c`）：
   - `format_outline_data()` - 将Hint列表格式化为OceanBase/Oracle风格
   - 使用`/*+ BEGIN_OUTLINE_DATA ... END_OUTLINE_DATA */`格式
   - 每个Hint占一行，便于阅读和复制

3. **查询执行集成**（`postgres.c`）：
   - 在查询执行完成后（`PortalRun`之后）检查`outline_display_hints`参数
   - 如果启用，调用`plan_to_hints()`生成Hint
   - 通过`ereport(NOTICE, ...)`将格式化的Outline Data发送到客户端

4. **GUC参数管理**（`outline_guc.c`）：
   - 定义`outline.display_hints`布尔参数
   - 在PostgresMain初始化时注册GUC参数
   - 用户可以通过SET命令动态控制功能开关

## 未来增强计划

1. **查询指纹识别**：实现类似`pg_stat_statements`的查询规范化算法，以匹配具有不同字面值的查询。

2. **增强自动捕获**：在现有的自动Hint生成（`outline.display_hints`）基础上，添加类似`pg_capture_outline(query_text)`的函数，可以一步完成查询执行、计划捕获和Outline创建。

3. **导入/导出**：添加函数将Outline导出到SQL脚本，便于在不同环境之间迁移。

4. **统计信息**：添加计数器跟踪每个Outline的应用频率及其对查询性能的影响。

5. **计划比较**：添加工具比较有无Outline的执行计划，验证有效性。

## 相关文件

### 新增文件
- `src/include/catalog/pg_outline.h` - 目录定义
- `src/include/catalog/pg_outline.dat` - 目录数据
- `src/include/optimizer/outline_hints.h` - Hint结构和API
- `src/backend/optimizer/outline/outline_hints.c` - Hint解析
- `src/backend/optimizer/outline/outline_plan.c` - 计划到Hint转换
- `src/backend/optimizer/outline/outline_apply.c` - Hint应用
- `src/backend/optimizer/outline/outline_guc.c` - GUC参数管理
- `src/backend/utils/adt/pg_outline_funcs.c` - SQL函数

### 修改文件
- `src/backend/optimizer/Makefile` - 添加outline子目录
- `src/backend/optimizer/outline/Makefile` - 添加outline_guc.o
- `src/backend/utils/adt/Makefile` - 添加pg_outline_funcs.o
- `src/backend/utils/cache/syscache.c` - 添加outline系统缓存
- `src/backend/tcop/postgres.c` - 集成自动Outline生成和显示
- `src/include/utils/syscache.h` - 添加OUTLINENAMENSP和OUTLINEOID
- `src/include/catalog/pg_proc.dat` - 注册SQL函数
- `src/include/catalog/indexing.h` - 添加pg_outline索引定义
- `src/include/catalog/toasting.h` - 添加pg_outline TOAST表定义
- `src/backend/catalog/Makefile` - 添加pg_outline到目录构建

## 参考资料

- **OceanBase Outline文档**: https://en.oceanbase.com/docs/common-oceanbase-database-10000000000872110
- **pg_hint_plan文档**: https://pg-hint-plan.readthedocs.io/
- **PostgreSQL优化器钩子**: `src/include/optimizer/paths.h`

## 技术支持

如遇到问题或需要帮助，请：
1. 查看PostgreSQL日志文件
2. 使用`EXPLAIN (ANALYZE, VERBOSE)`分析查询
3. 检查系统表`pg_outline`中的Outline定义
4. 参考本文档的故障排查部分

## 许可证

本实现是PostgreSQL的一部分，遵循PostgreSQL许可证。
