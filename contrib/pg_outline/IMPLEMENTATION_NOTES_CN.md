# 子链接Hint应用实现 - 技术说明

## 问题与需求

**用户需求**: 当新的SQL命中outline时,需要在进入优化器**之前**将outline hints插入到SQL中,此时子链接(sublinks)还没有被上拉(pull up)。

## 实现方案

我采用了**方案A**(在SUBLINK_REWRITE_SOLUTION.md中描述的推荐方案):在Query树中直接应用hints到SubLinks,而不是重新解析SQL文本。

### 架构设计

```
SQL字符串 → raw_parser() → 原始解析树 → parse_analyze() → Query结构
                                                   ↑
                                     post_parse_analyze_hook
                                           (新增加的)
                                                   ↓
                               检测outline匹配 → 应用hints到Query树
                                                   ↓
                               遍历Query树找到所有SubLinks
                                                   ↓
                              给每个SubLink的Query应用hints
                                                   ↓
                                         进入 planner_hook
                                                   ↓
                                           标准优化器处理
```

### 关键代码变更

#### 1. 新增 `post_parse_analyze_hook`

**文件**: `pg_outline.c`

**函数**: `outline_post_parse_analyze(ParseState *pstate, Query *query)`
- **位置**: Lines 421-504
- **功能**:
  - 计算查询指纹(query fingerprint)
  - 检索匹配的存储outline
  - 解析stored hints
  - 调用 `apply_hints_to_sublinks()` 应用hints
  - 将hints存储在全局变量供planner使用

#### 2. 实现SubLink遍历和hint应用

**新函数**:

1. **`apply_hints_to_sublinks(Query *query, List *parsed_hints)`**
   - 位置: Lines 4504-4529
   - 功能:
     - 确保Query名称已分配(如: "main", "sublink_0", "sublink_1")
     - 应用hints到主查询
     - 使用 `query_tree_walker` 遍历整个Query树
     - 找到所有SubLink并应用对应的hints

2. **`apply_sublink_hints_walker(Node *node, List **parsed_hints_ptr)`**
   - 位置: Lines 4534-4571
   - 功能:
     - Walker函数,递归遍历Query树
     - 检测 `SubLink` 节点
     - 提取SubLink中的Query
     - 调用 `apply_hints_to_query()` 应用hints
     - 递归处理嵌套的subqueries

### 工作原理

1. **解析阶段**:
   ```sql
   SELECT * FROM t1 WHERE c1 IN (SELECT c2 FROM t2 WHERE c2 < 5)
   ```
   解析后产生包含SubLink的Query树,此时SubLink结构完整保留。

2. **post_parse_analyze_hook 拦截**:
   - 计算query fingerprint
   - 检测到匹配的stored outline: `[main] ... [sublink_0] ...`
   - 解析hints得到结构化数据

3. **应用hints到Query树**:
   - 遍历Query树,找到:
     - 主查询 (名称: "main")
     - 子查询 (名称: "sublink_0", "sublink_1", ...)
   - 将对应的hints存储在Query的metadata中

4. **优化阶段** (在 `planner_hook`):
   - 优化器看到的Query已经带有hint metadata
   - 在SubLink pull-up之前,hints已经关联到对应的Query节点
   - 优化器根据hints调整执行计划

### 与原有代码的区别

**之前的实现**:
- 只在 `planner_hook` 中工作
- 此时SubLinks可能已经被pull-up成joins
- 无法区分哪些表来自sublinks

**新实现**:
- 在 `post_parse_analyze_hook` 中拦截
- SubLinks结构完整保留
- 可以精确地将hints应用到每个subquery
- hints在优化前就已经关联到Query节点

## 使用示例

### 生成outline (auto mode)

```sql
SET pg_outline.mode = 'auto';
SET client_min_messages = 'DEBUG1';

EXPLAIN SELECT * FROM t1 WHERE c1 IN (SELECT c2 FROM t2 WHERE c2 < 10);
-- 自动生成outline,包含 [main] 和 [sublink_0] hints
```

### 应用outline (manual mode)

```sql
SET pg_outline.mode = 'manual';
SET client_min_messages = 'DEBUG1';

EXPLAIN SELECT * FROM t1 WHERE c1 IN (SELECT c2 FROM t2 WHERE c2 < 5);
-- 输出:
-- DEBUG1: pg_outline: found stored outline, applying hints to query tree
-- DEBUG1: pg_outline: traversing query tree to apply hints to sublinks
-- DEBUG1: pg_outline: applied hint to query 'main': ...
-- DEBUG1: pg_outline: found SubLink, applying hints to subquery
-- DEBUG1: pg_outline: applied hint to query 'sublink_0': ...
-- NOTICE: pg_outline: Outline matched! Hints will be applied. Equivalent SQL:
--   SELECT /*+ main hints */ * FROM t1 WHERE c1 IN (SELECT /*+ sublink_0 hints */ c2 FROM t2 ...)
```

## 技术限制与注意事项

### 当前实现的限制

1. **SubLink Pull-up仍会发生**:
   - Hints在SubLink pull-up之前关联到Query
   - 但PostgreSQL优化器仍可能决定pull-up sublinks
   - 如果sublink被pull-up成semi-join,hints需要能够应用到join后的结构

2. **Hint语义**:
   - `[sublink_0] SeqScan(t2)` 意味着"对t2表使用SeqScan"
   - 如果sublink被pull-up,t2仍然会应用SeqScan hint
   - 这是通过 `outline_set_rel_pathlist_hook` 实现的

3. **Query Name分配**:
   - 依赖于 `assign_query_names()` 函数
   - 必须在hints应用之前调用
   - 保证Query名称的一致性("main", "sublink_0", ...)

### 优点

✅ **在extension层面实现**,无需修改PostgreSQL核心
✅ **SubLinks在hints应用时保持完整**
✅ **可以精确控制每个subquery的执行计划**
✅ **向后兼容**,不影响现有功能

### 下一步改进

如果需要进一步改进,可以考虑:

1. **防止SubLink Pull-up** (高级功能):
   - 在planner早期阶段,检查是否有sublink hints
   - 如果有,设置标志阻止pull_up_sublinks()
   - 这需要更深入的planner hook集成

2. **更细粒度的Hint控制**:
   - 支持 `[sublink_0] NoPullup` hint来明确禁止pull-up
   - 支持 `[sublink_0] ForceSubPlan` 强制使用SubPlan

3. **性能优化**:
   - 缓存Query树遍历结果
   - 只在必要时重新分配Query名称

## 测试

编译和测试:

```bash
cd /home/runner/work/postgresql_12_22_debug/postgresql_12_22_debug/contrib/pg_outline
make clean && make && make install

# 运行测试
psql -f test_post_parse_hook.sql
```

预期看到:
- ✅ 编译成功(仅有warnings)
- ✅ DEBUG消息显示hints被应用到main和sublink查询
- ✅ NOTICE显示重构后的SQL with positioned hints
- ✅ 执行计划反映应用的hints

## 结论

这个实现提供了一个实用的解决方案来在优化前应用sublink hints,虽然不能完全阻止SubLink pull-up,但能确保hints在SubLink结构完整时被正确关联,并在后续优化中生效。

这是在不修改PostgreSQL核心的情况下能达到的最佳效果。
