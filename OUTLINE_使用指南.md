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

## 快速入门：构建、安装与测试

本节提供完整的步骤指导，让初学者能够从零开始构建、安装PostgreSQL Outline功能，并进行完整的功能测试。

### 前置要求

在开始之前，请确保系统已安装以下软件：

```bash
# Ubuntu/Debian系统
sudo apt-get update
sudo apt-get install -y build-essential libreadline-dev zlib1g-dev flex bison

# CentOS/RHEL系统
sudo yum install -y gcc make readline-devel zlib-devel flex bison

# macOS系统
brew install readline
```

### 步骤1：获取源代码

```bash
# 克隆仓库
git clone https://github.com/jmh000527/postgresql_12_22_debug.git
cd postgresql_12_22_debug
```

### 步骤2：配置编译选项

```bash
# 配置PostgreSQL编译选项
# --prefix 指定安装目录
# --enable-debug 启用调试信息
# --enable-cassert 启用断言检查
./configure --prefix=/usr/local/pgsql --enable-debug --enable-cassert
```

**输出示例：**
```
checking build system type... x86_64-pc-linux-gnu
checking host system type... x86_64-pc-linux-gnu
...
configure: creating ./config.status
config.status: creating GNUmakefile
config.status: creating src/Makefile.global
...
PostgreSQL configured successfully.
```

### 步骤3：编译源代码

```bash
# 使用多核编译（-j4表示使用4个CPU核心，可根据实际情况调整）
make -j4

# 如果编译成功，最后会显示：
# All of PostgreSQL successfully made. Ready to install.
```

**编译时间：** 通常需要5-15分钟，取决于机器性能。

### 步骤4：安装PostgreSQL

```bash
# 安装到指定目录
sudo make install

# 输出示例：
# PostgreSQL installation complete.
```

### 步骤5：初始化数据库集群

```bash
# 创建postgres用户（如果不存在）
sudo useradd -m postgres

# 创建数据目录
sudo mkdir -p /usr/local/pgsql/data
sudo chown postgres:postgres /usr/local/pgsql/data

# 切换到postgres用户
sudo -u postgres bash

# 初始化数据库集群
/usr/local/pgsql/bin/initdb -D /usr/local/pgsql/data

# 输出示例：
# Success. You can now start the database server using:
#     /usr/local/pgsql/bin/pg_ctl -D /usr/local/pgsql/data -l logfile start
```

### 步骤6：启动PostgreSQL服务

```bash
# 作为postgres用户启动数据库
/usr/local/pgsql/bin/pg_ctl -D /usr/local/pgsql/data -l /usr/local/pgsql/logfile start

# 输出示例：
# waiting for server to start.... done
# server started
```

### 步骤7：连接数据库并验证安装

```bash
# 连接到PostgreSQL
/usr/local/pgsql/bin/psql -d postgres

# 在psql提示符下执行：
postgres=# SELECT version();
# 应该显示PostgreSQL 12.22版本信息

# 检查Outline功能是否可用
postgres=# \d pg_outline
# 应该显示pg_outline系统表的结构
```

### 步骤8：创建测试数据

现在让我们创建完整的测试案例来验证Outline功能：

```sql
-- 创建测试表
CREATE TABLE customers (
    id INT PRIMARY KEY,
    region VARCHAR(100),
    name VARCHAR(100)
);

CREATE TABLE orders (
    order_id INT PRIMARY KEY,
    customer_id INT,
    amount DECIMAL,
    status VARCHAR(50)
);

-- 创建索引
CREATE INDEX idx_customer_region ON customers(region);
CREATE INDEX idx_orders_customer ON orders(customer_id);
CREATE INDEX idx_orders_status ON orders(status);

-- 插入测试数据
INSERT INTO customers
SELECT i, 'region_' || (i % 10), 'customer_' || i
FROM generate_series(1, 1000) i;

INSERT INTO orders
SELECT i, (i % 1000) + 1, random() * 1000,
       CASE WHEN random() < 0.5 THEN 'pending' ELSE 'completed' END
FROM generate_series(1, 5000) i;

-- 分析表以更新统计信息
ANALYZE customers;
ANALYZE orders;
```

### 步骤9：测试基本Outline功能

#### 测试1：手动创建Outline

```sql
-- 1. 创建一个简单的Outline
SELECT pg_create_outline(
    'outline_customer_by_region',
    'SELECT * FROM customers WHERE region = $1',
    'IndexScan(customers idx_customer_region)'
);

-- 预期输出：
--  pg_create_outline
-- -------------------
--              16384
-- (1 row)

-- 2. 查看创建的Outline
SELECT outlinename, outlinequery, outlinehints
FROM pg_outline;

-- 预期输出：
--        outlinename        |              outlinequery               |               outlinehints
-- --------------------------+-----------------------------------------+------------------------------------------
--  outline_customer_by_region | select * from customers where region = $1 | IndexScan(customers idx_customer_region)

-- 3. 执行匹配Outline的查询
EXPLAIN SELECT * FROM customers WHERE region = 'region_5';

-- 预期输出应包含：
-- Index Scan using idx_customer_region on customers
```

#### 测试2：自动显示Outline Data

```sql
-- 1. 启用自动显示功能
SET outline.display_hints = on;

-- 2. 执行查询
SELECT * FROM orders WHERE status = 'pending' LIMIT 5;

-- 预期输出包含NOTICE消息：
-- NOTICE:  Outline Data:
-- /*+
-- BEGIN_OUTLINE_DATA
-- IndexScan(orders idx_orders_status)
-- END_OUTLINE_DATA
-- */

-- 3. 对于连接查询
SELECT c.name, o.amount
FROM customers c
JOIN orders o ON c.id = o.customer_id
WHERE c.region = 'region_1'
LIMIT 10;

-- 预期输出包含多个Hint的NOTICE消息
```

#### 测试3：录制模式（sr_plan式）

```sql
-- 1. 启用录制模式
SET outline.recording_mode = on;
SET outline.display_hints = on;

-- 2. 执行要录制的查询
SELECT * FROM customers WHERE region = 'region_3';

-- 预期输出：
-- NOTICE:  Outline Data:
-- /*+
-- BEGIN_OUTLINE_DATA
-- IndexScan(customers idx_customer_region)
-- END_OUTLINE_DATA
-- */
-- NOTICE:  Created outline "auto_outline_12345_1" for query

-- 3. 关闭录制模式
SET outline.recording_mode = off;
SET outline.display_hints = off;

-- 4. 验证自动创建的Outline
SELECT outlinename, outlinequery, outlinehints
FROM pg_outline
WHERE outlinename LIKE 'auto_outline%';

-- 5. 测试回放（再次执行相同查询）
SELECT * FROM customers WHERE region = 'region_3' LIMIT 5;

-- 查询应自动使用之前录制的Outline
```

#### 测试4：Outline管理操作

```sql
-- 1. 禁用Outline
SELECT pg_disable_outline('outline_customer_by_region');

-- 2. 验证已禁用
SELECT outlinename, outlineenabled FROM pg_outline;

-- 3. 重新启用
SELECT pg_enable_outline('outline_customer_by_region');

-- 4. 删除Outline
SELECT pg_drop_outline('outline_customer_by_region');

-- 5. 确认已删除
SELECT count(*) FROM pg_outline WHERE outlinename = 'outline_customer_by_region';
-- 预期输出：0
```

### 步骤10：高级测试案例

#### 测试5：多Hint的Outline

```sql
-- 创建包含多个Hint的Outline（使用OceanBase格式）
SELECT pg_create_outline(
    'outline_complex_join',
    'SELECT c.name, o.amount FROM customers c JOIN orders o ON c.id = o.customer_id WHERE c.region = $1',
    '/*+
    BEGIN_OUTLINE_DATA
    IndexScan(customers idx_customer_region)
    IndexScan(orders idx_orders_customer)
    HashJoin(customers orders)
    END_OUTLINE_DATA
    */'
);

-- 验证Outline
EXPLAIN SELECT c.name, o.amount
FROM customers c
JOIN orders o ON c.id = o.customer_id
WHERE c.region = 'region_2';

-- 预期输出应显示：
-- Hash Join
--   -> Index Scan using idx_customer_region on customers c
--   -> Index Scan using idx_orders_customer on orders o
```

#### 测试6：强制顺序扫描

```sql
-- 创建强制SeqScan的Outline
SELECT pg_create_outline(
    'outline_force_seqscan',
    'SELECT * FROM orders WHERE status = $1',
    'SeqScan(orders)'
);

-- 执行查询并验证
EXPLAIN SELECT * FROM orders WHERE status = 'pending';

-- 预期输出应显示：
-- Seq Scan on orders
-- 而不是 Index Scan
```

#### 测试7：测试Leading Hint - 连接顺序控制

Leading hint允许完全控制连接顺序，支持简单语法和嵌套语法：

```sql
-- 创建测试表用于连接顺序测试
CREATE TABLE t1 (id int PRIMARY KEY, data text);
CREATE TABLE t2 (id int PRIMARY KEY, t1_id int, data text);
CREATE TABLE t3 (id int PRIMARY KEY, t2_id int, data text);
CREATE TABLE t4 (id int PRIMARY KEY, t3_id int, data text);

INSERT INTO t1 SELECT i, 'data_' || i FROM generate_series(1, 100) i;
INSERT INTO t2 SELECT i, (i % 100) + 1, 'data_' || i FROM generate_series(1, 500) i;
INSERT INTO t3 SELECT i, (i % 500) + 1, 'data_' || i FROM generate_series(1, 200) i;
INSERT INTO t4 SELECT i, (i % 200) + 1, 'data_' || i FROM generate_series(1, 300) i;

ANALYZE t1, t2, t3, t4;

-- 测试7a：简单的左到右连接顺序
SELECT pg_create_outline(
    'test_leading_simple',
    'SELECT * FROM t1 JOIN t2 ON t1.id = t2.t1_id JOIN t3 ON t2.id = t3.t2_id',
    'Leading(t1 t2 t3)'
);

-- 验证连接顺序：t1 -> t2 -> t3
EXPLAIN SELECT * FROM t1
JOIN t2 ON t1.id = t2.t1_id
JOIN t3 ON t2.id = t3.t2_id;

-- 测试7b：嵌套语法用于bushy连接树
SELECT pg_create_outline(
    'test_leading_nested',
    'SELECT * FROM t1 JOIN t2 ON t1.id = t2.t1_id JOIN t3 ON t1.id = t3.t2_id JOIN t4 ON t2.id = t4.t3_id',
    'Leading(((t1 t2) (t3 t4)))'
);

-- 验证bushy连接：(t1 JOIN t2)与(t3 JOIN t4)并行，然后连接结果
EXPLAIN SELECT * FROM t1
JOIN t2 ON t1.id = t2.t1_id
JOIN t3 ON t1.id = t3.t2_id
JOIN t4 ON t2.id = t4.t3_id;

-- 测试7c：复杂的多层嵌套
SELECT pg_create_outline(
    'test_leading_complex',
    'SELECT * FROM t1 JOIN t2 ON t1.id = t2.t1_id JOIN t3 ON t2.id = t3.t2_id JOIN t4 ON t3.id = t4.t3_id',
    'Leading((((t1 t2) t3) t4))'
);

-- 验证复杂嵌套：((t1 JOIN t2) JOIN t3) JOIN t4
EXPLAIN SELECT * FROM t1
JOIN t2 ON t1.id = t2.t1_id
JOIN t3 ON t2.id = t3.t2_id
JOIN t4 ON t3.id = t4.t3_id;
```

**预期行为：**
- 简单语法创建左深连接树：t1 → t2 → t3
- 嵌套语法创建bushy树：并行连接然后组合
- 复杂嵌套完全控制连接树结构
- PostgreSQL自动验证外连接约束
- 如果提示的连接顺序无效，系统会回退到标准连接搜索

#### 测试8：测试内联Leading Hint

```sql
-- 内联hint不需要创建outline
SELECT /*+ Leading(t2 t1 t3) */
    t1.id, t2.data, t3.data
FROM t1
JOIN t2 ON t1.id = t2.t1_id
JOIN t3 ON t2.id = t3.t2_id
WHERE t1.id < 50;

-- 内联嵌套语法
SELECT /*+ Leading((t1 t2) t3) */
    t1.id, t2.data, t3.data
FROM t1
JOIN t2 ON t1.id = t2.t1_id
JOIN t3 ON t2.id = t3.t2_id
WHERE t1.id < 50;

-- 清理测试表
DROP TABLE t4, t3, t2, t1;
```

### 故障排查

如果遇到问题，请检查以下几点：

1. **编译失败**
   ```bash
   # 检查是否安装了必需的开发工具
   gcc --version
   make --version
   ```

2. **服务无法启动**
   ```bash
   # 查看日志文件
   cat /usr/local/pgsql/logfile

   # 检查端口是否被占用
   netstat -an | grep 5432
   ```

3. **pg_outline表不存在**
   ```sql
   -- 检查是否在正确的数据库中
   SELECT current_database();

   -- 检查是否有权限
   SELECT current_user;
   ```

4. **Outline不生效**
   ```sql
   -- 检查Outline是否启用
   SELECT outlinename, outlineenabled FROM pg_outline;

   -- 检查查询是否匹配（规范化）
   SET client_min_messages = DEBUG1;
   -- 然后执行查询，查看日志中是否有"Applied outline"消息
   ```

### 清理测试环境

测试完成后，可以清理测试数据：

```sql
-- 删除所有自动创建的Outline
SELECT pg_drop_outline(outlinename)
FROM pg_outline
WHERE outlinename LIKE 'auto_outline%';

-- 删除测试表
DROP TABLE IF EXISTS orders;
DROP TABLE IF EXISTS customers;

-- 或者停止并删除整个数据库集群
-- 退出psql，然后执行：
# /usr/local/pgsql/bin/pg_ctl -D /usr/local/pgsql/data stop
# sudo rm -rf /usr/local/pgsql/data
```

### 下一步

恭喜！您已经成功构建、安装并测试了PostgreSQL Outline功能。接下来可以：

1. 阅读下面的详细功能说明，了解更多高级特性
2. 在实际应用场景中使用Outline功能
3. 探索录制模式（sr_plan式）的更多用法
4. 学习如何监控和优化Outline的效果
5. 尝试内联Hint功能，详见 `INLINE_HINTS_TEST.md`

## 内联Hint功能

### 概述

内联Hint功能允许您直接在SQL查询中使用特殊注释语法指定Hint。这在以下场景特别有用：
- 查询包含多个SELECT关键字（子查询、CTE）
- 需要为复杂查询的不同部分指定不同的Hint
- 希望将Hint与查询保持在一起而不是单独存储

### 语法

内联Hint使用格式：`/*+ hint1 hint2 ... */`

注释必须以 `/*+`（斜杠-星号-加号）开始才会被识别为Hint注释。普通的 `/*` 注释（不带加号）会被忽略。

### 基本示例

#### 单表查询

```sql
-- 强制顺序扫描
SELECT /*+ SeqScan(customers) */ * FROM customers WHERE region = 'region_5';

-- 强制索引扫描
SELECT /*+ IndexScan(customers idx_customer_region) */ * FROM customers WHERE region = 'region_5';

-- 禁用顺序扫描（强制使用索引）
SELECT /*+ NoSeqScan(customers) */ * FROM customers WHERE id > 500;
```

#### 连接查询

```sql
-- 强制Hash Join
SELECT /*+ HashJoin(customers orders) */ c.name, o.amount
FROM customers c
JOIN orders o ON c.id = o.customer_id
WHERE c.region = 'region_1';

-- 强制Nested Loop Join
SELECT /*+ NestLoop(customers orders) */ c.name, o.amount
FROM customers c
JOIN orders o ON c.id = o.customer_id
WHERE c.region = 'region_1';
```

### 多个SELECT语句

内联Hint的核心特性是支持在多个SELECT关键字处指定不同的Hint：

#### 子查询示例

```sql
-- 主查询使用IndexScan，子查询使用SeqScan
SELECT /*+ IndexScan(customers idx_customer_region) */ *
FROM customers c
WHERE c.region = 'region_5'
  AND EXISTS (
      SELECT /*+ SeqScan(orders) */ 1
      FROM orders o
      WHERE o.customer_id = c.id AND o.amount > 50
  );
```

在这个例子中：
- 主查询的customers表将使用索引扫描
- 子查询的orders表将使用顺序扫描

#### CTE（公共表表达式）示例

```sql
WITH high_value_orders AS (
    SELECT /*+ SeqScan(orders) */ customer_id, SUM(amount) as total
    FROM orders
    WHERE amount > 50
    GROUP BY customer_id
)
SELECT /*+ HashJoin(customers high_value_orders) */ c.name, h.total
FROM customers c
JOIN high_value_orders h ON c.id = h.customer_id;
```

### OceanBase兼容格式

您也可以使用OceanBase风格的格式，使用BEGIN_OUTLINE_DATA标记：

```sql
SELECT /*+
BEGIN_OUTLINE_DATA
IndexScan(customers idx_customer_region)
HashJoin(customers orders)
END_OUTLINE_DATA
*/ c.name, o.amount
FROM customers c
JOIN orders o ON c.id = o.customer_id
WHERE c.region = 'region_3';
```

### Hint提取和合并

当查询包含多个内联Hint注释时，它们会被全部提取并合并。例如：

```sql
SELECT /*+ IndexScan(customers idx_customer_region) */ c.name
FROM customers c
WHERE c.id IN (
    SELECT /*+ SeqScan(orders) */ customer_id
    FROM orders
    WHERE amount > 75
);
```

系统提取：`IndexScan(customers idx_customer_region) SeqScan(orders)`

两个Hint都会在查询规划期间应用。

### 优先级和优先权

1. **内联Hint具有最高优先级**：如果查询包含内联Hint，它们优先于`pg_outline`目录中存储的Outline。
2. **存储的Outline作为后备**：如果不存在内联Hint，系统会检查`pg_outline`中匹配的Outline。

示例：

```sql
-- 创建存储的Outline
SELECT pg_create_outline(
    'outline1',
    'SELECT * FROM customers WHERE region = $1',
    'SeqScan(customers)'
);

-- 此查询使用存储的Outline（SeqScan）
SELECT * FROM customers WHERE region = 'region_5';

-- 此查询使用内联Hint覆盖（IndexScan）
SELECT /*+ IndexScan(customers idx_customer_region) */ *
FROM customers WHERE region = 'region_5';
```

### 支持的Hint类型

Outline系统支持的所有Hint类型都可用于内联Hint：

**扫描方法Hint：**
- `SeqScan(table)` - 强制顺序扫描
- `IndexScan(table index)` - 强制使用特定索引进行索引扫描
- `NoSeqScan(table)` - 禁用顺序扫描
- `NoIndexScan(table)` - 禁用索引扫描

**连接方法Hint：**
- `NestLoop(table1 table2)` - 强制嵌套循环连接
- `HashJoin(table1 table2)` - 强制哈希连接
- `MergeJoin(table1 table2)` - 强制归并连接

**行数估算Hint：**
- `Rows(table row_count)` - 覆盖行数估算
- `Rows(table1 table2 row_count)` - 覆盖连接结果估算

**并行化控制Hint：**
- `Parallel(table num_workers)` - 强制并行执行
- `NoParallel(table)` - 禁用并行执行

**GUC参数覆盖Hint：**
- `Set(parameter value)` - 临时覆盖GUC参数

**示例：**
```sql
-- 行数覆盖
SELECT /*+ Rows(customers 10000) */ * FROM customers;

-- 并行化控制
SELECT /*+ Parallel(large_table 8) */ * FROM large_table;

-- GUC参数覆盖
SELECT /*+ Set(random_page_cost 1.1) */ * FROM orders;

-- 组合多种Hint类型
SELECT /*+ Rows(customers 5000) Parallel(orders 4) HashJoin(customers orders) */
  * FROM customers JOIN orders ON customers.id = orders.customer_id;
```

### 调试内联Hint

要查看何时提取和应用内联Hint：

```sql
SET client_min_messages = DEBUG1;

SELECT /*+ SeqScan(customers) */ * FROM customers WHERE region = 'region_5';
```

您将看到调试消息：
```
DEBUG:  Extracted inline hints: SeqScan(customers)
DEBUG:  Applying inline hints from query
```

### 完整测试套件

有关全面的示例和测试用例，请参阅 `INLINE_HINTS_TEST.md`，其中包括：
- 基本内联Hint测试
- 多个SELECT语句测试
- 子查询和CTE示例
- 优先级和优先权测试
- 性能比较

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

### 行数估算Hint

```sql
Rows(表名 行数)                            -- 覆盖单个表的行数估算
Rows(表1 表2 行数)                         -- 覆盖连接结果的行数估算
Rows(表1 表2 表3 行数)                     -- 覆盖多表连接的行数估算
```

**用途**：当统计信息不准确或过时时，覆盖优化器的行数估算。

**使用场景**：
- 纠正`ANALYZE`统计信息过时导致的估算错误
- 通过操纵基数估算来强制特定的连接顺序
- 处理优化器无法检测到的数据倾斜

**示例**：
```sql
-- 覆盖customers表的估算
SELECT pg_create_outline(
    'fix_customers_estimate',
    'SELECT * FROM customers WHERE region = $1',
    'Rows(customers 5000)'
);
```

### 并行化控制Hint

```sql
Parallel(表名 工作进程数)                   -- 强制并行扫描并指定工作进程数
NoParallel(表名)                            -- 禁用并行执行
```

**用途**：控制表扫描的并行度。

**使用场景**：
- 为大表扫描强制启用并行执行
- 为OLTP工作负载禁用并行化以减少开销
- 微调工作进程数以获得最佳性能

**示例**：
```sql
-- 为大表扫描强制使用8个并行工作进程
SELECT pg_create_outline(
    'parallelize_huge_table',
    'SELECT * FROM huge_table WHERE status = $1',
    'Parallel(huge_table 8)'
);

-- 为小型OLTP查询禁用并行化
SELECT pg_create_outline(
    'no_parallel_oltp',
    'SELECT * FROM users WHERE id = $1',
    'NoParallel(users)'
);
```

### GUC参数覆盖Hint

```sql
Set(参数名 值)                              -- 临时覆盖GUC参数
```

**用途**：在查询规划期间临时调整成本参数和其他GUC设置。

**常用参数**：
- `random_page_cost` - 随机磁盘页访问成本（默认：4.0）
- `seq_page_cost` - 顺序磁盘页访问成本（默认：1.0）
- `cpu_tuple_cost` - 处理一个元组的成本（默认：0.01）
- `cpu_operator_cost` - 处理一个操作符的成本（默认：0.0025）
- `work_mem` - 排序/哈希操作的内存
- `enable_seqscan`、`enable_indexscan`等 - 启用/禁用特定计划类型

**示例**：
```sql
-- 针对SSD优化，降低随机页访问成本
SELECT pg_create_outline(
    'ssd_optimized',
    'SELECT * FROM orders WHERE customer_id = $1',
    'Set(random_page_cost 1.1) IndexScan(orders idx_customer)'
);

-- 为复杂查询增加工作内存
SELECT pg_create_outline(
    'large_sort',
    'SELECT * FROM large_table ORDER BY created_at',
    'Set(work_mem 256MB)'
);

-- 组合多个Set hint与其他hint
SELECT pg_create_outline(
    'complex_optimization',
    'SELECT * FROM t1 JOIN t2 ON t1.id = t2.fk',
    E'Set(random_page_cost 1.1)\nSet(work_mem 128MB)\nHashJoin(t1 t2)'
);
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

3. **Leading Hint**：用于控制连接顺序的`LEADING` Hint类型已完全实现，支持简单语法和嵌套语法，可完全控制连接顺序，包括bushy连接树的构建。

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
