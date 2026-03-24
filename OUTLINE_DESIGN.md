# PostgreSQL Outline特性概要设计文档

**版本**: 1.0
**日期**: 2026-03-24
**作者**: PostgreSQL Outline开发团队
**基于**: PostgreSQL 12.22

---

## 1. 概述

### 1.1 功能简介

Outline特性是PostgreSQL的查询优化器扩展功能，通过提示(Hint)机制实现执行计划的稳定性控制。该特性允许DBA和开发人员：

- 通过Hint指定扫描方法和连接方法
- 持久化存储查询的优化提示（存储式Outline）
- 在SQL语句中直接嵌入提示（内联式Hint）
- 自动捕获和生成查询的执行计划提示
- 重用已经过验证的执行计划，避免性能回退

### 1.2 设计目标

1. **执行计划稳定性**: 防止统计信息变化导致的计划回退
2. **非侵入式集成**: 利用PostgreSQL标准Hook机制，无需修改核心优化器代码
3. **易用性**: 提供SQL函数接口和自动化工具
4. **兼容性**: 支持OceanBase/Oracle风格的Outline格式
5. **可扩展性**: 模块化设计，便于功能扩展

### 1.3 核心特性

- **三级提示优先级**: 内联Hint > 存储Outline > 默认优化器
- **自动提示生成**: 从执行计划反向生成Hint字符串
- **自动录制模式**: sr_plan风格的自动计划捕获
- **查询归一化**: 支持不同格式的查询匹配同一Outline
- **系统目录存储**: 基于pg_outline系统表的持久化存储

---

## 2. 系统架构

### 2.1 整体架构图

```
┌─────────────────────────────────────────────────────────────────┐
│                         用户查询 (SQL Query)                      │
└────────────────────────────┬────────────────────────────────────┘
                             │
                             ▼
        ┌────────────────────────────────────────┐
        │     查询解析器 (Query Parser)           │
        └──────────────┬─────────────────────────┘
                       │
                       ▼
        ┌──────────────────────────────────────────────┐
        │          提示提取模块 (Hint Extraction)       │
        │  1. 检查内联Hint (/*+ ... */)                │
        │  2. 查询存储Outline (pg_outline目录)         │
        └──────────────┬───────────────────────────────┘
                       │
                       ▼
        ┌──────────────────────────────────────────────┐
        │       提示解析器 (Hint Parser)                │
        │  - 解析Hint语法                               │
        │  - 生成HintState结构                         │
        └──────────────┬───────────────────────────────┘
                       │
                       ▼
        ┌──────────────────────────────────────────────┐
        │      查询优化器 (Query Planner)               │
        │  + Hook: set_rel_pathlist_hook               │
        │  + Hook: set_join_pathlist_hook              │
        │  → 根据Hint过滤路径                           │
        └──────────────┬───────────────────────────────┘
                       │
                       ▼
        ┌──────────────────────────────────────────────┐
        │         执行计划 (Execution Plan)             │
        └──────────────┬───────────────────────────────┘
                       │
                       ├─────────────────────────────────┐
                       │                                 │
                       ▼                                 ▼
        ┌──────────────────────────┐    ┌──────────────────────────┐
        │ 自动提示显示              │    │ 自动录制模式              │
        │ (outline.display_hints)  │    │ (outline.recording_mode) │
        │ → 生成Hint并发送NOTICE   │    │ → 自动创建Outline        │
        └──────────────────────────┘    └──────────────────────────┘
```

### 2.2 核心组件

#### 2.2.1 系统目录模块

**pg_outline系统表 (OID: 9900)**

| 列名 | 类型 | 说明 |
|------|------|------|
| oid | Oid | Outline对象标识符 |
| outlinename | NameData | Outline名称 |
| outlinenamespace | Oid | 命名空间OID（默认public） |
| outlineowner | Oid | 所有者OID |
| outlineenabled | bool | 是否启用（默认true） |
| outlinequery | text | 归一化后的查询文本 |
| outlinehints | text | Hint字符串 |

**系统缓存**

1. **OUTLINENAMENSP**: 按(名称, 命名空间)查找
   - 索引: OutlineNameNspIndexId
   - 缓存大小: 32条目

2. **OUTLINEOID**: 按OID查找
   - 索引: OutlineOidIndexId

#### 2.2.2 配置参数模块

**GUC参数**

1. **outline.display_hints** (boolean, 默认: off)
   - 上下文: PGC_USERSET
   - 功能: 自动显示查询执行计划的Hint
   - 输出格式: OceanBase/Oracle风格的Outline Data

2. **outline.recording_mode** (boolean, 默认: off)
   - 上下文: PGC_USERSET
   - 功能: 自动捕获并创建Outline
   - 命名规则: auto_outline_<进程ID>_<计数器>

#### 2.2.3 提示解析模块

**数据结构**

```c
// Hint类型枚举
typedef enum HintType {
    HINT_TYPE_SCAN_METHOD,    // 扫描方法提示
    HINT_TYPE_JOIN_METHOD,    // 连接方法提示
    HINT_TYPE_LEADING         // 连接顺序提示（未完全实现）
} HintType;

// 扫描方法提示
typedef struct ScanHint {
    HintType type;
    char *relname;            // 表名
    char *indexname;          // 索引名（可选）
    ScanMethodHint method;    // 具体扫描方法
} ScanHint;

// 连接方法提示
typedef struct JoinHint {
    HintType type;
    char *relname1;           // 第一个表
    char *relname2;           // 第二个表
    JoinMethodHint method;    // 具体连接方法
} JoinHint;

// 通用Hint结构
typedef struct Hint {
    HintType type;
    union {
        ScanHint scan;
        JoinHint join;
        LeadingHint leading;
    } hint;
} Hint;

// Hint状态
typedef struct HintState {
    List *hints;              // Hint列表
    bool enabled;             // 是否激活
    Oid outline_oid;          // 关联的Outline OID
} HintState;
```

**支持的Hint类型**

扫描方法Hint:
- `SeqScan(table)` - 强制顺序扫描
- `IndexScan(table index)` - 强制索引扫描
- `IndexOnlyScan(table index)` - 强制仅索引扫描
- `BitmapScan(table)` - 强制位图扫描
- `TidScan(table)` - 强制TID扫描
- `NoSeqScan(table)` - 禁用顺序扫描
- `NoIndexScan(table)` - 禁用索引扫描
- 等等...

连接方法Hint:
- `NestLoop(table1 table2)` - 强制嵌套循环连接
- `HashJoin(table1 table2)` - 强制哈希连接
- `MergeJoin(table1 table2)` - 强制归并连接
- `NoNestLoop(table1 table2)` - 禁用嵌套循环
- 等等...

#### 2.2.4 提示应用模块

**优化器Hook**

1. **set_rel_pathlist_hook**
   - 时机: 为基础关系生成路径后
   - 函数: `outline_set_rel_pathlist()`
   - 作用: 根据扫描Hint过滤路径列表

2. **set_join_pathlist_hook**
   - 时机: 为连接生成路径后
   - 函数: `outline_set_join_pathlist()`
   - 作用: 根据连接Hint过滤路径列表

**路径过滤机制**

```
原始路径列表 (rel->pathlist)
    │
    ▼
检查是否存在匹配的Hint
    │
    ├─无Hint────→ 保留所有路径
    │
    └─有Hint────→ 过滤路径
                    │
                    ├─扫描Hint: 移除不匹配的扫描路径
                    └─连接Hint: 移除不匹配的连接路径
```

---

## 3. 详细设计

### 3.1 查询归一化算法

**目的**: 使不同格式的查询能匹配同一Outline

**归一化规则**:
1. 关键字和标识符转小写
2. 多个连续空格合并为单个空格
3. 保留字符串字面量的大小写
4. 移除前导和尾随空格
5. 处理字符串中的转义引号

**实现**: `normalize_query_string(const char *query_string)`

**示例**:
```sql
原始: SELECT * FROM Customers WHERE region = 'Asia'
变体: SELECT   *   FROM   CUSTOMERS   WHERE   region='Asia'
大小写: select * from customers where region = 'Asia'

全部归一化为: select * from customers where region = 'asia'
```

### 3.2 内联Hint提取流程

**语法**: `/*+ hint1 hint2 ... */`

**提取算法** (`extract_inline_hints()`):

```
1. 扫描查询字符串，查找 "/*+" 模式
2. 处理OceanBase格式（BEGIN_OUTLINE_DATA...END_OUTLINE_DATA）
3. 跳过字符串字面量和引用标识符
4. 累积所有Hint到缓冲区
5. 合并多个Hint注释
6. 调用parse_hints()解析
7. 返回HintState或NULL
```

**优先级**: 内联Hint > 存储Outline

**示例**:
```sql
SELECT /*+ SeqScan(t1) */ *
FROM t1
WHERE EXISTS (
    SELECT /*+ IndexScan(t2 idx) */ 1 FROM t2
);

提取结果: "SeqScan(t1) IndexScan(t2 idx)"
```

### 3.3 存储Outline查找流程

**函数**: `get_hints_for_query(const char *query_string)`

**算法**:
```
1. 归一化输入查询
2. 获取"public"命名空间的OID
3. 以AccessShareLock打开pg_outline关系
4. 使用syscache按命名空间过滤
5. 遍历每个Outline:
   a. 获取存储的查询文本
   b. 归一化存储的查询
   c. 与输入查询比较
6. 若匹配:
   a. 获取Hint文本
   b. 解析Hint
   c. 设置outline_oid
   d. 记录DEBUG1日志
   e. 返回HintState
7. 释放锁，返回NULL（无匹配）
```

**性能优化**: 利用syscache进行快速重复查找

### 3.4 Hint解析算法

**函数**: `parse_hints(const char *hint_str)`

**解析步骤**:
```
1. 逐字符遍历Hint字符串
2. 识别Hint名称（如SeqScan、IndexScan）
3. 提取括号内的参数
4. 根据Hint类型创建相应结构
   - 扫描Hint: 创建ScanHint
   - 连接Hint: 创建JoinHint
5. 添加到HintState.hints列表
6. 返回完整的HintState
```

**错误处理**: 解析失败时记录WARNING，跳过该Hint

### 3.5 执行计划到Hint的转换

**函数**: `plan_to_hints(PlannedStmt *pstmt, const char *query_string)`

**转换算法**:
```
1. 递归遍历计划树
2. 识别扫描操作:
   - SeqScan → SeqScan(table)
   - IndexScan → IndexScan(table index)
   - IndexOnlyScan → IndexOnlyScan(table index)
   - BitmapHeapScan → BitmapScan(table)
   - TidScan → TidScan(table)
3. 识别连接操作:
   - NestLoop → NestLoop(rel1 rel2)
   - HashJoin → HashJoin(rel1 rel2)
   - MergeJoin → MergeJoin(rel1 rel2)
4. 收集所有Hint到字符串
5. 返回Hint字符串
```

**格式化输出** (`format_outline_data()`):
```
/*+
BEGIN_OUTLINE_DATA
SeqScan(table1)
IndexScan(table2 idx)
NestLoop(table1 table2)
END_OUTLINE_DATA
*/
```

### 3.6 自动录制模式实现

**触发条件**: `outline.recording_mode = on`

**函数**: `record_outline_for_query(const char *query_string, const char *hints)`

**录制流程**:
```
1. 归一化查询字符串
2. 扫描pg_outline查找匹配的查询
3. 若找到匹配: 跳过录制（避免重复）
4. 若无匹配:
   a. 生成唯一名称: auto_outline_<进程ID>_<计数器>
   b. 插入元组到pg_outline:
      - outline_name: 自动生成的名称
      - outline_query: 归一化查询
      - outline_hints: 捕获的Hint
      - outline_enabled: true
   c. 发送NOTICE消息
```

**去重机制**: 基于归一化查询文本的完全匹配

---

## 4. 数据流设计

### 4.1 Outline应用流程

```
查询文本
    │
    ▼
extract_inline_hints() ──→ 找到Hint? ──是──→ 解析并应用
    │                           │
    否                          否
    │                           │
    ▼                           ▼
归一化查询           get_hints_for_query()
    │                           │
    ▼                           ▼
搜索pg_outline         查询目录
    │                           │
    ▼                           ▼
找到? ──否──→ 默认优化器 ──→ 执行计划
    │
    是
    │
    ▼
解析Hint
    │
    ▼
设置HintState
    │
    ▼
优化器Hook应用（过滤路径）
    │
    ▼
最终执行计划
```

### 4.2 自动录制流程

```
执行的查询
    │
    ▼
生成执行计划
    │
    ▼
outline.recording_mode ON? ──否──→ 继续
    │
    是
    │
    ▼
plan_to_hints() ──→ 从实际计划提取
    │
    ▼
record_outline_for_query()
    │
    ▼
检查重复 ──重复──→ 跳过
    │
    否
    │
    ▼
生成名称: auto_outline_<pid>_<计数>
    │
    ▼
插入pg_outline
    │
    ▼
NOTICE: 已创建Outline "..."
```

### 4.3 查询执行集成点

**文件**: `src/backend/tcop/postgres.c`

**阶段1: Hint准备**
```c
exec_simple_query(const char *query_string) {
    // 解析后

    // 优先尝试内联Hint
    HintState *hstate = extract_inline_hints(query_string);
    if (hstate == NULL) {
        // 回退到存储Outline
        hstate = get_hints_for_query(query_string);
    }
    if (hstate != NULL) {
        outline_set_hint_state(hstate);  // 设置模块级状态
    }
}
```

**阶段2: 计划生成（应用Hint）**
```c
planner(Query *parse, ...) {
    // PostgreSQL在计划期间调用优化器Hook
    // 我们的Hook使用current_hint_state过滤路径

    // 为每个关系调用set_rel_pathlist_hook
    // 为每个连接调用set_join_pathlist_hook
}
```

**阶段3: 自动显示（可选）**
```c
if (outline_display_hints && plantree_list != NIL) {
    // 从实际计划生成Hint
    for each PlannedStmt {
        hints = plan_to_hints(pstmt, query_string);
        formatted = format_outline_data(hints);
        ereport(NOTICE, ...);  // 显示给客户端
    }
}
```

**阶段4: 自动录制（可选）**
```c
if (outline_recording_mode && plantree_list != NIL) {
    for each PlannedStmt {
        hints = plan_to_hints(pstmt, query_string);
        record_outline_for_query(query_string, hints);  // 自动创建Outline
    }
}
```

**阶段5: 清理**
```c
outline_clear_hint_state();  // 执行后清除Hint
```

---

## 5. 接口设计

### 5.1 SQL函数接口

#### pg_create_outline()

**签名**:
```sql
pg_create_outline(
    outline_name text,    -- Outline名称
    query_text text,      -- 查询文本
    hints_text text       -- Hint文本（支持OceanBase格式）
) RETURNS oid
```

**权限**: 仅超级用户

**功能**:
1. 提取Hint（支持OceanBase格式转换）
2. 检查名称重复
3. 生成新OID
4. 插入pg_outline
5. 返回生成的OID

**示例**:
```sql
SELECT pg_create_outline(
    'opt_customer_query',
    'SELECT * FROM customers WHERE region = ''Asia''',
    'SeqScan(customers)'
);
```

#### pg_drop_outline()

**签名**:
```sql
pg_drop_outline(outline_name text) RETURNS void
```

**权限**: 仅超级用户

**功能**: 按名称删除Outline

**示例**:
```sql
SELECT pg_drop_outline('opt_customer_query');
```

#### pg_enable_outline()

**签名**:
```sql
pg_enable_outline(outline_name text) RETURNS void
```

**权限**: 仅超级用户

**功能**: 启用指定Outline

**示例**:
```sql
SELECT pg_enable_outline('opt_customer_query');
```

#### pg_disable_outline()

**签名**:
```sql
pg_disable_outline(outline_name text) RETURNS void
```

**权限**: 仅超级用户

**功能**: 禁用指定Outline（不删除）

**示例**:
```sql
SELECT pg_disable_outline('opt_customer_query');
```

### 5.2 系统目录查询

```sql
-- 查看所有Outline
SELECT oid, outlinename, outlineenabled, outlinequery, outlinehints
FROM pg_outline;

-- 查看已启用的Outline
SELECT * FROM pg_outline WHERE outlineenabled = true;

-- 查看特定查询的Outline
SELECT * FROM pg_outline
WHERE outlinequery LIKE '%customers%';
```

### 5.3 GUC参数配置

```sql
-- 启用自动提示显示
SET outline.display_hints = on;

-- 启用自动录制模式
SET outline.recording_mode = on;

-- 查看当前配置
SHOW outline.display_hints;
SHOW outline.recording_mode;
```

---

## 6. 模块划分与文件组织

### 6.1 源代码结构

```
src/
├── include/
│   ├── catalog/
│   │   ├── pg_outline.h          # Outline目录表定义
│   │   └── pg_outline.dat        # 目录数据初始化
│   └── optimizer/
│       └── outline_hints.h       # 公共头文件：类型定义和函数声明
│
├── backend/
│   ├── catalog/
│   │   └── pg_outline.c          # 目录基础操作
│   │
│   ├── optimizer/outline/
│   │   ├── outline_hints.c       # Hint解析、归一化、目录查找、内联提取
│   │   ├── outline_apply.c       # Hook函数：路径过滤
│   │   ├── outline_plan.c        # 计划到Hint转换
│   │   └── outline_guc.c         # GUC参数注册
│   │
│   ├── utils/adt/
│   │   └── pg_outline_funcs.c    # SQL可调用函数
│   │
│   └── tcop/
│       └── postgres.c            # 集成点（修改）
│
└── test/
    └── regress/sql/
        └── outline.sql           # 回归测试
```

### 6.2 模块依赖关系

```
postgres.c (查询执行)
    │
    ├──→ outline_hints.c (Hint提取和解析)
    │       │
    │       └──→ outline_apply.c (Hint应用)
    │
    ├──→ outline_plan.c (计划分析)
    │
    ├──→ outline_funcs.c (SQL函数)
    │       │
    │       └──→ pg_outline.c (目录操作)
    │
    └──→ outline_guc.c (配置参数)
```

### 6.3 关键数据结构位置

| 数据结构 | 定义位置 | 用途 |
|---------|---------|------|
| `HintState` | outline_hints.h | 存储解析后的Hint状态 |
| `Hint` | outline_hints.h | 通用Hint结构 |
| `ScanHint` | outline_hints.h | 扫描方法Hint |
| `JoinHint` | outline_hints.h | 连接方法Hint |
| `FormData_pg_outline` | pg_outline.h | 目录表行结构 |

---

## 7. 核心算法

### 7.1 路径过滤算法

**函数**: `outline_set_rel_pathlist()`

```c
void outline_set_rel_pathlist(
    PlannerInfo *root,
    RelOptInfo *rel,
    Index rti,
    RangeTblEntry *rte)
{
    HintState *hstate = outline_get_hint_state();
    if (!hstate || !hstate->enabled)
        return;  // 无Hint或未启用

    // 查找匹配当前关系的扫描Hint
    ScanHint *scan_hint = find_scan_hint_for_rel(hstate, rte->relname);
    if (!scan_hint)
        return;  // 无匹配Hint

    // 根据Hint类型过滤路径
    ListCell *lc;
    List *filtered_paths = NIL;

    foreach(lc, rel->pathlist) {
        Path *path = (Path *) lfirst(lc);

        // 检查路径类型是否与Hint匹配
        if (path_matches_hint(path, scan_hint)) {
            filtered_paths = lappend(filtered_paths, path);
        }
    }

    // 替换路径列表
    if (filtered_paths != NIL) {
        rel->pathlist = filtered_paths;
    }
}
```

**函数**: `outline_set_join_pathlist()`

```c
void outline_set_join_pathlist(
    PlannerInfo *root,
    RelOptInfo *joinrel,
    RelOptInfo *outerrel,
    RelOptInfo *innerrel,
    JoinType jointype,
    JoinPathExtraData *extra)
{
    HintState *hstate = outline_get_hint_state();
    if (!hstate || !hstate->enabled)
        return;

    // 查找匹配的连接Hint
    JoinHint *join_hint = find_join_hint(hstate, outerrel, innerrel);
    if (!join_hint)
        return;

    // 过滤连接路径
    List *filtered_paths = NIL;
    ListCell *lc;

    foreach(lc, joinrel->pathlist) {
        Path *path = (Path *) lfirst(lc);

        if (join_path_matches_hint(path, join_hint)) {
            filtered_paths = lappend(filtered_paths, path);
        }
    }

    if (filtered_paths != NIL) {
        joinrel->pathlist = filtered_paths;
    }
}
```

### 7.2 计划遍历算法

**函数**: `plan_to_hints()`

```c
char *plan_to_hints(PlannedStmt *pstmt, const char *query_string)
{
    StringInfo hint_buf = makeStringInfo();

    // 递归遍历计划树
    extract_hints_from_plan(pstmt->planTree, hint_buf);

    return hint_buf->data;
}

static void extract_hints_from_plan(Plan *plan, StringInfo buf)
{
    if (plan == NULL)
        return;

    // 处理扫描节点
    switch (nodeTag(plan)) {
        case T_SeqScan:
            appendStringInfo(buf, "SeqScan(%s) ",
                get_rel_name(((Scan *)plan)->scanrelid));
            break;

        case T_IndexScan:
            {
                IndexScan *iscan = (IndexScan *)plan;
                appendStringInfo(buf, "IndexScan(%s %s) ",
                    get_rel_name(iscan->scan.scanrelid),
                    get_index_name(iscan->indexid));
            }
            break;

        // ... 其他扫描类型
    }

    // 处理连接节点
    switch (nodeTag(plan)) {
        case T_NestLoop:
            appendStringInfo(buf, "NestLoop(%s %s) ",
                get_rel_names_from_plan(plan->lefttree),
                get_rel_names_from_plan(plan->righttree));
            break;

        case T_HashJoin:
            appendStringInfo(buf, "HashJoin(%s %s) ",
                get_rel_names_from_plan(plan->lefttree),
                get_rel_names_from_plan(plan->righttree));
            break;

        // ... 其他连接类型
    }

    // 递归处理子计划
    extract_hints_from_plan(plan->lefttree, buf);
    extract_hints_from_plan(plan->righttree, buf);
}
```

---

## 8. 性能考虑

### 8.1 性能优化措施

1. **系统缓存利用**
   - 使用syscache加速pg_outline查找
   - 缓存最近使用的Outline
   - 避免重复扫描目录表

2. **查询归一化缓存**
   - 缓存归一化结果
   - 减少重复字符串处理

3. **早期退出**
   - 无Hint时立即返回
   - 禁用的Outline不进行解析

4. **增量解析**
   - 只解析需要的Hint类型
   - 延迟解析复杂Hint

### 8.2 内存管理

1. **内存上下文**
   - 使用短生命周期的内存上下文
   - 查询结束时自动释放Hint内存

2. **字符串管理**
   - 使用StringInfo进行高效字符串拼接
   - 避免频繁的palloc/pfree

### 8.3 锁策略

1. **读锁优化**
   - 使用AccessShareLock读取pg_outline
   - 避免长时间持有锁

2. **并发控制**
   - 支持多会话同时读取Outline
   - 写操作使用合适的锁级别

---

## 9. 安全性设计

### 9.1 权限控制

1. **超级用户权限**
   - 创建/删除Outline需要超级用户权限
   - 启用/禁用Outline需要超级用户权限

2. **命名空间隔离**
   - 每个Outline关联特定命名空间
   - 支持未来扩展为schema级别隔离

### 9.2 SQL注入防护

1. **参数化查询**
   - SQL函数使用参数化接口
   - 避免字符串拼接

2. **输入验证**
   - 验证Outline名称格式
   - 检查Hint语法有效性

### 9.3 资源限制

1. **Hint数量限制**
   - 单个查询最大Hint数量
   - 防止内存耗尽

2. **Outline总数限制**
   - 可配置的Outline总数上限
   - 防止目录表过大

---

## 10. 可扩展性设计

### 10.1 新Hint类型扩展

**添加新Hint的步骤**:

1. 在`HintType`枚举中添加新类型
2. 定义新的Hint结构（如`RowHint`）
3. 在`parse_hints()`中添加解析逻辑
4. 在应用Hook中添加处理逻辑
5. 在`plan_to_hints()`中添加生成逻辑

**示例**: 添加行数Hint

```c
// 1. 添加类型
typedef enum HintType {
    // ... 现有类型
    HINT_TYPE_ROWS
} HintType;

// 2. 定义结构
typedef struct RowsHint {
    HintType type;
    char *relname;
    int estimated_rows;
} RowsHint;

// 3. 扩展Hint联合
typedef struct Hint {
    HintType type;
    union {
        ScanHint scan;
        JoinHint join;
        RowsHint rows;  // 新增
    } hint;
} Hint;

// 4. 在parse_hints()中添加解析
// 5. 在Hook中添加应用逻辑
```

### 10.2 新优化器Hook扩展

PostgreSQL提供多个优化器Hook点，可以扩展支持：

1. **join_search_hook**: 控制连接顺序搜索
2. **create_upper_paths_hook**: 处理聚合和排序
3. **get_relation_info_hook**: 修改关系信息

### 10.3 插件化设计

**模块接口标准化**:

```c
// 定义标准Hint处理器接口
typedef struct HintHandler {
    const char *hint_name;
    HintType hint_type;
    bool (*parse_func)(const char *hint_str, Hint *hint);
    void (*apply_func)(PlannerInfo *root, Hint *hint);
    char *(*generate_func)(Plan *plan);
} HintHandler;

// 注册机制
void register_hint_handler(HintHandler *handler);
```

---

## 11. 错误处理与日志

### 11.1 错误级别

| 级别 | 场景 | 示例 |
|------|------|------|
| ERROR | 致命错误 | Outline名称重复 |
| WARNING | 可恢复错误 | Hint解析失败 |
| NOTICE | 信息通知 | 自动创建Outline |
| DEBUG1 | 调试信息 | Outline匹配成功 |
| DEBUG2 | 详细调试 | Hint应用细节 |

### 11.2 日志记录点

1. **Outline创建/删除**
   ```c
   elog(NOTICE, "Created outline \"%s\" for query", outline_name);
   elog(NOTICE, "Dropped outline \"%s\"", outline_name);
   ```

2. **Hint匹配**
   ```c
   elog(DEBUG1, "Found matching outline \"%s\" for query", outline_name);
   elog(DEBUG1, "No matching outline found for query");
   ```

3. **Hint应用**
   ```c
   elog(DEBUG2, "Applied scan hint: %s", hint_to_string(hint));
   elog(DEBUG2, "Filtered %d paths to %d paths", orig_count, new_count);
   ```

4. **错误情况**
   ```c
   elog(WARNING, "Failed to parse hint: %s", hint_str);
   elog(WARNING, "Invalid hint syntax at position %d", pos);
   ```

### 11.3 错误恢复策略

1. **Hint解析失败**: 跳过该Hint，继续处理其余Hint
2. **Outline查找失败**: 回退到默认优化器
3. **内存分配失败**: 抛出ERROR，中止当前查询

---

## 12. 测试策略

### 12.1 单元测试

**测试模块**:
1. 查询归一化测试
2. Hint解析测试
3. 路径过滤测试
4. 计划转换测试

**示例测试用例**:
```c
// 测试归一化
test_normalize_query() {
    char *input = "SELECT * FROM  Users WHERE id=1";
    char *expected = "select * from users where id=1";
    char *result = normalize_query_string(input);
    assert(strcmp(result, expected) == 0);
}

// 测试Hint解析
test_parse_scan_hint() {
    char *hint_str = "SeqScan(customers)";
    HintState *hstate = parse_hints(hint_str);
    assert(hstate != NULL);
    assert(list_length(hstate->hints) == 1);
    Hint *hint = (Hint *)linitial(hstate->hints);
    assert(hint->type == HINT_TYPE_SCAN_METHOD);
}
```

### 12.2 集成测试

**测试场景**:
1. 内联Hint功能测试
2. 存储Outline功能测试
3. 自动显示功能测试
4. 自动录制功能测试
5. Hint优先级测试

**测试SQL示例**:
```sql
-- 测试内联Hint
EXPLAIN SELECT /*+ SeqScan(customers) */ * FROM customers;

-- 测试存储Outline
SELECT pg_create_outline('test1',
    'SELECT * FROM orders WHERE status = ''pending''',
    'IndexScan(orders status_idx)');
EXPLAIN SELECT * FROM orders WHERE status = 'pending';

-- 测试自动显示
SET outline.display_hints = on;
SELECT * FROM products LIMIT 10;

-- 测试自动录制
SET outline.recording_mode = on;
SELECT * FROM inventory WHERE qty < 100;
SELECT * FROM pg_outline WHERE outlinename LIKE 'auto_outline%';
```

### 12.3 性能测试

**基准测试**:
1. 无Hint的查询性能（基准）
2. 有Hint的查询性能（应接近基准）
3. Outline查找开销
4. Hint解析开销

**压力测试**:
1. 大量Outline场景（10000+条）
2. 复杂Hint场景（50+个Hint）
3. 高并发查询场景

---

## 13. 部署与维护

### 13.1 安装步骤

1. **编译安装**
   ```bash
   cd postgresql_source
   ./configure
   make
   make install
   ```

2. **初始化数据库**
   ```bash
   initdb -D /path/to/data
   ```

3. **启动服务器**
   ```bash
   pg_ctl -D /path/to/data start
   ```

4. **验证功能**
   ```sql
   -- 检查pg_outline表是否存在
   \d pg_outline

   -- 检查GUC参数
   SHOW outline.display_hints;
   SHOW outline.recording_mode;
   ```

### 13.2 配置建议

**postgresql.conf**:
```conf
# Outline功能配置（可选）
# outline.display_hints = off
# outline.recording_mode = off

# 日志级别（用于调试）
# log_min_messages = debug1
```

**使用建议**:
1. 生产环境默认关闭自动功能
2. 开发环境可启用display_hints辅助调优
3. 测试环境可使用recording_mode快速捕获计划

### 13.3 监控与诊断

**监控指标**:
1. pg_outline表大小
2. Outline匹配率（通过日志分析）
3. Hint应用成功率
4. 查询性能变化

**诊断工具**:
```sql
-- 查看所有Outline统计
SELECT
    outlinename,
    outlineenabled,
    length(outlinequery) as query_len,
    length(outlinehints) as hints_len
FROM pg_outline
ORDER BY oid;

-- 查找禁用的Outline
SELECT outlinename FROM pg_outline
WHERE outlineenabled = false;

-- 查找长时间未使用的Outline（需要扩展功能）
-- 未来可添加last_used字段
```

### 13.4 备份与恢复

**备份Outline**:
```bash
# 导出Outline定义
pg_dump -t pg_outline database_name > outlines_backup.sql
```

**恢复Outline**:
```bash
# 恢复Outline定义
psql database_name < outlines_backup.sql
```

**迁移Outline**:
```sql
-- 从一个数据库迁移到另一个
COPY (SELECT * FROM pg_outline) TO '/tmp/outlines.csv' CSV HEADER;
-- 在目标数据库
COPY pg_outline FROM '/tmp/outlines.csv' CSV HEADER;
```

---

## 14. 限制与未来改进

### 14.1 当前限制

1. **查询匹配**
   - 使用字面量查询匹配，不支持参数化查询
   - 不同绑定变量值的查询被视为不同查询

2. **Hint功能**
   - Leading hint（连接顺序）未完全实现
   - 不支持子查询的独立Hint控制（部分通过内联Hint缓解）

3. **统计信息**
   - 缺少Outline使用统计
   - 无法评估Outline的效果

4. **命名空间**
   - 当前所有Outline在同一命名空间
   - 不支持按schema隔离

### 14.2 改进方向

#### 14.2.1 短期改进

1. **查询指纹**
   - 实现类似pg_stat_statements的查询指纹
   - 支持参数化查询匹配
   - 示例:
     ```sql
     SELECT * FROM users WHERE id = 1
     SELECT * FROM users WHERE id = 100
     -- 归一化为: SELECT * FROM users WHERE id = $1
     ```

2. **使用统计**
   - 添加字段: last_used, use_count, success_count
   - 提供视图: pg_outline_stats
   - 便于识别无效Outline

3. **计划比较**
   - 工具函数: compare_plan_with_outline()
   - 验证Outline是否按预期工作

#### 14.2.2 中期改进

1. **完整Leading hint**
   - 完全控制连接顺序
   - 语法: `Leading((t1 t2) t3)`

2. **Schema级隔离**
   - 每个schema独立的Outline空间
   - 避免命名冲突

3. **导入导出工具**
   - pg_dump/pg_restore集成
   - 批量管理工具

4. **图形化管理界面**
   - Web UI或pgAdmin插件
   - 可视化Outline管理

#### 14.2.3 长期改进

1. **机器学习集成**
   - 自动推荐Outline
   - 基于历史性能数据

2. **动态Hint调整**
   - 根据实时统计信息调整
   - 自适应优化

3. **分布式支持**
   - 跨节点Outline同步
   - 集群级别管理

4. **更丰富的Hint类型**
   - 并行度控制: `Parallel(table 4)`
   - 内存控制: `WorkMem(operation 256MB)`
   - 成本参数: `Set(random_page_cost 1.1)`

---

## 15. 最佳实践

### 15.1 Outline创建原则

1. **明确目标**
   - 只为性能关键查询创建Outline
   - 避免过度使用

2. **充分测试**
   - 先用display_hints观察当前计划
   - 验证Hint是否产生预期计划
   - 比较有无Outline的性能差异

3. **命名规范**
   - 使用有意义的名称，如: `opt_customer_report_monthly`
   - 包含创建日期: `opt_orders_20260324`
   - 标记用途: `temp_outline_debug_issue_123`

4. **文档化**
   - 记录创建Outline的原因
   - 记录预期性能改进
   - 注明审查周期

### 15.2 使用场景

#### 场景1: 统计信息不准确

```sql
-- 问题: 统计信息过时导致错误的计划
-- 解决: 创建Outline强制使用正确计划

SELECT pg_create_outline(
    'opt_large_table_join',
    'SELECT * FROM orders JOIN customers ON orders.customer_id = customers.id',
    'HashJoin(orders customers) IndexScan(customers customers_pkey)'
);
```

#### 场景2: 优化器选择不稳定

```sql
-- 问题: 查询计划随数据分布波动
-- 解决: 固定已验证的最优计划

SET outline.display_hints = on;
-- 执行查询，观察好的计划
SELECT * FROM products WHERE category = 'Electronics';
-- 获取Hint后创建Outline

SELECT pg_create_outline(
    'opt_products_by_category',
    'SELECT * FROM products WHERE category = ''Electronics''',
    'IndexScan(products idx_category)'
);
```

#### 场景3: 紧急性能问题

```sql
-- 问题: 生产环境突发性能问题
-- 解决: 快速应用内联Hint，后续创建Outline

-- 临时解决
SELECT /*+ SeqScan(large_table) HashJoin(large_table small_table) */
    * FROM large_table JOIN small_table ...;

-- 永久方案
SELECT pg_create_outline(...);
```

#### 场景4: 批量计划捕获

```sql
-- 场景: 新系统上线，需要快速固定计划
-- 方法: 使用recording_mode

SET outline.recording_mode = on;
-- 运行典型业务查询
\i business_queries.sql
-- 查看自动创建的Outline
SELECT outlinename, outlinequery FROM pg_outline
WHERE outlinename LIKE 'auto_outline%';
```

### 15.3 维护策略

1. **定期审查**
   - 每季度审查所有Outline
   - 删除不再使用的Outline
   - 更新过时的Outline

2. **性能验证**
   - 定期禁用Outline对比性能
   - 验证Outline仍然有效

3. **版本管理**
   - PostgreSQL升级后重新验证
   - 记录Outline变更历史

4. **监控告警**
   - 监控查询性能退化
   - Outline失效时告警

### 15.4 故障排查

**问题1: Outline不生效**

检查清单:
```sql
-- 1. 检查Outline是否存在
SELECT * FROM pg_outline WHERE outlinename = 'my_outline';

-- 2. 检查是否已启用
SELECT outlineenabled FROM pg_outline WHERE outlinename = 'my_outline';

-- 3. 检查查询归一化是否匹配
SELECT normalize_query_string('your query');

-- 4. 检查日志
-- 设置 log_min_messages = debug1
-- 查看是否有"Found matching outline"消息
```

**问题2: 计划未按预期改变**

诊断步骤:
```sql
-- 1. 验证Hint语法
-- 临时使用内联Hint测试
EXPLAIN SELECT /*+ your_hint */ ...;

-- 2. 检查表名和索引名是否正确
\d table_name

-- 3. 查看实际应用的Hint
SET client_min_messages = debug2;
-- 执行查询，查看日志
```

**问题3: 性能反而下降**

解决方案:
```sql
-- 1. 禁用Outline对比
SELECT pg_disable_outline('problematic_outline');
-- 运行查询对比性能

-- 2. 重新分析表
ANALYZE table_name;

-- 3. 更新或删除Outline
SELECT pg_drop_outline('problematic_outline');
```

---

## 16. 附录

### 16.1 完整Hint参考

#### 扫描方法Hint

| Hint | 语法 | 说明 |
|------|------|------|
| SeqScan | `SeqScan(table)` | 强制顺序扫描 |
| IndexScan | `IndexScan(table index)` | 强制索引扫描 |
| IndexOnlyScan | `IndexOnlyScan(table index)` | 强制仅索引扫描 |
| BitmapScan | `BitmapScan(table)` | 强制位图扫描 |
| TidScan | `TidScan(table)` | 强制TID扫描 |
| NoSeqScan | `NoSeqScan(table)` | 禁用顺序扫描 |
| NoIndexScan | `NoIndexScan(table)` | 禁用索引扫描 |
| NoIndexOnlyScan | `NoIndexOnlyScan(table)` | 禁用仅索引扫描 |
| NoBitmapScan | `NoBitmapScan(table)` | 禁用位图扫描 |
| NoTidScan | `NoTidScan(table)` | 禁用TID扫描 |

#### 连接方法Hint

| Hint | 语法 | 说明 |
|------|------|------|
| NestLoop | `NestLoop(t1 t2 ...)` | 强制嵌套循环连接 |
| HashJoin | `HashJoin(t1 t2 ...)` | 强制哈希连接 |
| MergeJoin | `MergeJoin(t1 t2 ...)` | 强制归并连接 |
| NoNestLoop | `NoNestLoop(t1 t2 ...)` | 禁用嵌套循环 |
| NoHashJoin | `NoHashJoin(t1 t2 ...)` | 禁用哈希连接 |
| NoMergeJoin | `NoMergeJoin(t1 t2 ...)` | 禁用归并连接 |

#### 连接顺序Hint（部分实现）

| Hint | 语法 | 说明 |
|------|------|------|
| Leading | `Leading((t1 t2) t3)` | 指定连接顺序 |

### 16.2 系统视图参考

```sql
-- pg_outline系统表
CREATE TABLE pg_outline (
    oid oid NOT NULL,                      -- 对象标识符
    outlinename name NOT NULL,             -- Outline名称
    outlinenamespace oid DEFAULT 2200,     -- 命名空间OID
    outlineowner oid DEFAULT 10,           -- 所有者OID
    outlineenabled bool DEFAULT true,      -- 是否启用
    outlinequery text NOT NULL,            -- 归一化查询
    outlinehints text NOT NULL,            -- Hint字符串
    PRIMARY KEY (oid)
);

-- 系统缓存
OUTLINENAMENSP(outlinename, outlinenamespace)
OUTLINEOID(oid)
```

### 16.3 配置参数完整列表

| 参数名 | 类型 | 默认值 | 上下文 | 说明 |
|--------|------|--------|--------|------|
| outline.display_hints | boolean | off | PGC_USERSET | 自动显示查询Hint |
| outline.recording_mode | boolean | off | PGC_USERSET | 自动录制执行计划 |

### 16.4 错误代码

| 错误码 | 说明 |
|--------|------|
| ERRCODE_DUPLICATE_OBJECT | Outline名称重复 |
| ERRCODE_UNDEFINED_OBJECT | Outline不存在 |
| ERRCODE_INSUFFICIENT_PRIVILEGE | 权限不足 |
| ERRCODE_SYNTAX_ERROR | Hint语法错误 |

### 16.5 相关文档链接

- PostgreSQL官方文档: https://www.postgresql.org/docs/
- pg_hint_plan项目: https://github.com/ossc-db/pg_hint_plan
- OceanBase Outline文档: [待补充]
- sr_plan扩展: [待补充]

---

## 17. 术语表

| 术语 | 定义 |
|------|------|
| Outline | 存储在系统目录中的查询优化提示定义 |
| Hint | 指导查询优化器选择执行计划的提示 |
| 内联Hint | 直接嵌入在SQL语句中的Hint，格式为`/*+ ... */` |
| 存储Outline | 持久化存储在pg_outline表中的Hint |
| 查询归一化 | 将不同格式的查询转换为标准格式的过程 |
| HintState | 内存中表示解析后的Hint状态的数据结构 |
| Hook | PostgreSQL提供的扩展点，允许插件修改优化器行为 |
| 路径过滤 | 根据Hint移除不符合条件的执行路径 |
| 自动显示 | outline.display_hints功能，自动生成并显示Hint |
| 自动录制 | outline.recording_mode功能，自动捕获并创建Outline |
| 计划稳定性 | 保持查询执行计划不因统计信息变化而改变 |

---

## 18. 版本历史

| 版本 | 日期 | 作者 | 主要变更 |
|------|------|------|----------|
| 1.0 | 2026-03-24 | Outline开发团队 | 初始版本，完整设计文档 |

---

## 19. 贡献者

- PostgreSQL社区
- pg_hint_plan项目团队
- OceanBase团队（Outline概念启发）
- [其他贡献者]

---

## 20. 许可证

本文档遵循PostgreSQL许可证。

---

**文档结束**
