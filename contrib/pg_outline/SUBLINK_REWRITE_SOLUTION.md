# 子链接提示插入方案 (Sublink Hint Injection Solution)

## 问题描述 (Problem Description)

当新SQL命中存储的outline时,需要在SQL进入优化器**之前**将outline hints插入到SQL文本中,这样子链接(sublinks)还没有被上拉(pull up),可以保持原有的查询结构。

When a new SQL matches a stored outline, we need to insert the outline hints into the SQL text BEFORE it enters the optimizer, so that sublinks haven't been pulled up yet and the original query structure is preserved.

## 当前实现 (Current Implementation)

我已经添加了 `post_parse_analyze_hook` 的支持:

### 关键变更 (Key Changes)

1. **新增钩子** (Added new hook):
   - `outline_post_parse_analyze()` - 在解析后、优化前拦截查询

2. **功能** (Functionality):
   - 计算查询指纹 (Compute query fingerprint)
   - 检索匹配的存储outline (Retrieve matching stored outline)
   - 重构带提示的SQL (Reconstruct SQL with positioned hints)
   - 显示重构后的SQL (Display reconstructed SQL)

### 代码位置 (Code Location)

- **文件**: `pg_outline.c`
- **函数**: `outline_post_parse_analyze()` (lines 421-508)
- **钩子安装**: `_PG_init()` (lines 370-371)

## 架构限制 (Architectural Limitations)

`post_parse_analyze_hook` 在以下时机被调用:

```
SQL字符串 → raw_parser() → 原始解析树 → parse_analyze() → Query结构
                                                  ↑
                                    post_parse_analyze_hook在这里
```

**问题**: 此时我们收到的是已经解析完成的 `Query *` 结构体,而不是SQL字符串。

### 为什么不能简单地重新解析? (Why Can't We Simply Re-parse?)

在 `post_parse_analyze_hook` 中:
- ✅ 我们有 `debug_query_string` (原始SQL)
- ✅ 我们可以重构带提示的SQL
- ✅ 我们可以调用 `raw_parser()` 和 `parse_analyze()` 重新解析
- ❌ **但是**:我们不能替换已经传入的 `Query *` 结构,因为:
  - 钩子签名是 `void hook(ParseState *pstate, Query *query)`
  - Query 已经分配并部分初始化
  - 调用者期望在同一个 Query 对象上继续工作
  - 替换整个 Query 结构会导致内存管理问题

## 可能的解决方案 (Possible Solutions)

### 方案 1: ExecutorStart Hook + SPI (当前最可行)

在 `ExecutorStart_hook` 中:

```c
static void outline_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
    // 1. 检查是否匹配存储的outline
    // 2. 如果匹配,使用 SPI_execute() 执行重构后的SQL
    // 3. 替换 queryDesc 的结果
}
```

**优点**:
- 可以完全控制SQL执行
- Sublinks在重新解析时保持完整

**缺点**:
- 需要处理参数绑定
- 需要处理事务状态
- 可能影响性能(需要重新执行解析)

### 方案 2: ProcessUtility Hook (针对EXPLAIN)

对于 EXPLAIN 语句特殊处理:

```c
// 拦截 "EXPLAIN SELECT ..."
// 重写为 "EXPLAIN SELECT /*+ hints */ ..."
// 重新提交执行
```

**优点**:
- 对常见的outline测试场景有效
- 实现相对简单

**缺点**:
- 只能处理EXPLAIN,不能处理实际查询

### 方案 3: 修改PostgreSQL核心 (最彻底,最复杂)

添加一个新的钩子,在raw_parser之前:

```c
typedef char *(*rewrite_query_string_hook_type)(const char *query_string);
```

**优点**:
- 完美解决问题
- 对所有查询类型有效

**缺点**:
- 需要修改PostgreSQL源代码
- 不能作为extension实现
- 维护成本高

### 方案 4: 扩展解析器支持(推荐!)

利用PostgreSQL内置的hint支持机制。虽然PostgreSQL核心不解析hints,但我们可以:

1. 在 `post_parse_analyze_hook` 中检测到匹配的outline
2. 将hints信息存储在全局变量中
3. 在 `planner_hook` 中(当前的 `outline_planner`),使用这些hints
4. 关键是在Query tree中保留SubLink信息:
   - 在 `post_parse_analyze_hook` 中标记Query,指示"不要上拉sublinks"
   - 或者在 `planner_hook` 早期阶段,在sublink上拉之前应用hints

## 当前状态 (Current Status)

✅ 已实现:
- `post_parse_analyze_hook` 基础框架
- 检测匹配的outline
- 重构带提示的SQL
- 显示给用户

❌ 未实现:
- 实际使用重构后的SQL进行执行
- 保证sublinks不被上拉

## 下一步建议 (Next Steps)

### 选项A: 简单实用的解决方案

继续使用当前的 `planner_hook` 方法,但改进sublink hints的应用:

1. 在 `outline_post_parse_analyze()` 中,当检测到匹配的outline时:
   - 解析stored hints
   - 遍历Query树,找到所有SubLinks
   - 直接给SubLink的Query节点应用对应的hints
   - 在Query metadata中标记这些hints

2. 在 `outline_planner()` 中:
   - 检查是否有预先应用的hints
   - 在sublink pull-up之前,尝试影响优化器决策

**代码示例**:
```c
static void
outline_post_parse_analyze(ParseState *pstate, Query *query)
{
    // ... 现有代码 ...

    if (stored_hints)
    {
        // 解析hints
        List *parsed_hints = parse_stored_hints(stored_hints);

        // 遍历Query树应用hints到sublinks
        apply_hints_to_sublinks(query, parsed_hints);

        // 存储在全局变量供planner使用
        active_outline_hints = parsed_hints;
    }
}

static void
apply_hints_to_sublinks(Query *query, List *hints)
{
    // 使用 query_tree_walker 遍历所有SubLinks
    // 对每个SubLink,应用对应的 [sublink_N] hints
}
```

### 选项B: 完整的SQL重写方案

如果必须在优化前插入inline hints到SQL文本:

1. 在 `ExecutorStart_hook` 层实现
2. 检测outline匹配
3. 使用 SPI_execute() 执行重写后的SQL
4. 返回新的结果

这需要大量额外工作来处理边缘情况。

## 测试 (Testing)

编译成功,可以使用以下命令测试:

```bash
cd /home/runner/work/postgresql_12_22_debug/postgresql_12_22_debug/contrib/pg_outline
make && make install
```

```sql
-- 测试post_parse_analyze_hook是否工作
SET client_min_messages = 'DEBUG1';
SET pg_outline.mode = 'manual';

-- 先创建一个outline
-- (使用auto mode)

-- 然后执行匹配的查询
-- 应该能看到 "pg_outline: found stored outline, reconstructing SQL with hints"
```

## 结论 (Conclusion)

**当前实现**已经建立了基础框架,可以:
1. ✅ 检测outline匹配
2. ✅ 重构带positioned hints的SQL
3. ✅ 显示给用户

**仍需工作**:实现实际的hint应用机制,确保sublinks在优化前接收hints。

推荐使用**选项A**(在Query树中直接应用hints到SubLinks),这是在不修改PostgreSQL核心的情况下最实用的方案。
