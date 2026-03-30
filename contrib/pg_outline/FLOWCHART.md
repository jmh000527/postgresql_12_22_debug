# SubLink Hint 应用流程图

## 完整流程

```
用户执行SQL
    |
    v
SQL字符串: "SELECT * FROM t1 WHERE c1 IN (SELECT c2 FROM t2)"
    |
    v
raw_parser() - PostgreSQL解析器
    |
    v
RawStmt (原始语法树)
    |
    v
parse_analyze() - 语义分析
    |
    v
Query树生成:
    Query (main)
      └─ SubLink
           └─ Query (subquery for t2)
    |
    v
╔═══════════════════════════════════════════════════╗
║ post_parse_analyze_hook                            ║
║ (我们的新代码 - outline_post_parse_analyze)        ║
║                                                    ║
║ 1. 计算query fingerprint                          ║
║ 2. 检索存储的outline hints                        ║
║    └─ 如果找到: [main] SeqScan(t1)                ║
║                 [sublink_0] SeqScan(t2)           ║
║ 3. 解析hints成结构化数据                          ║
║ 4. 调用 assign_query_names():                    ║
║    └─ 给Query树中每个Query分配名称:               ║
║       - main query → "main"                        ║
║       - first sublink → "sublink_0"                ║
║       - second sublink → "sublink_1" ...           ║
║ 5. 调用 apply_hints_to_sublinks():                ║
║    └─ 遍历整个Query树                             ║
║    └─ 找到所有SubLink节点                         ║
║    └─ 对每个Query节点:                            ║
║       a. 在metadata hash表中查找Query             ║
║       b. 将对应hints关联到Query                   ║
║       c. 存储: entry->query_hints = "SeqScan(t2)" ║
║ 6. 存储全局 active_outline_hints                  ║
║                                                    ║
║ 输出 NOTICE: 显示重构后的SQL带positioned hints     ║
╚═══════════════════════════════════════════════════╝
    |
    v
Query树 (已关联hints metadata):
    Query "main" → hints: "SeqScan(t1)"
      └─ SubLink
           └─ Query "sublink_0" → hints: "SeqScan(t2)"
    |
    v
╔═══════════════════════════════════════════════════╗
║ planner_hook                                       ║
║ (outline_planner)                                  ║
║                                                    ║
║ - 接收带hints的Query树                             ║
║ - 调用 standard_planner()                          ║
╚═══════════════════════════════════════════════════╝
    |
    v
标准优化器处理:
    |
    v
╔═══════════════════════════════════════════════════╗
║ 优化阶段                                           ║
║                                                    ║
║ 1. 可能发生 SubLink Pull-up:                      ║
║    IN (SELECT ...) → Semi-Join                    ║
║                                                    ║
║ 2. 生成路径(Paths)时:                             ║
║    └─ set_rel_pathlist_hook                        ║
║       (outline_set_rel_pathlist)                   ║
║       ├─ 检查当前relation的hints                   ║
║       ├─ 如果hint是"SeqScan(t2)"                   ║
║       └─ 移除IndexScan/BitmapScan等其他paths       ║
║                                                    ║
║ 3. Join顺序决策:                                   ║
║    └─ join_search_hook                             ║
║       (outline_join_search)                        ║
║       └─ 根据Leading hints调整join顺序             ║
╚═══════════════════════════════════════════════════╝
    |
    v
PlannedStmt (执行计划)
    |
    v
执行器执行
```

## 关键时间点

```
Timeline:

T0: SQL字符串
    "SELECT * FROM t1 WHERE c1 IN (SELECT c2 FROM t2)"

T1: 解析完成,SubLink完整
    Query { SubLink { Query } }

T2: post_parse_analyze_hook ← 我们在这里拦截!
    ✓ SubLinks还在
    ✓ 可以识别每个subquery
    ✓ 应用hints到每个Query节点

T3: 进入优化器
    SubLinks可能被pull-up成joins
    但hints已经关联到对应的relations

T4: 路径生成
    根据hints过滤paths

T5: 执行计划生成
    反映应用的hints
```

## 数据结构

### Query Metadata Hash Table

```
current_query_metadata
  |
  ├─ bucket[0] → entry → entry → NULL
  ├─ bucket[1] → entry → NULL
  |       ↓
  |  QueryMetadataEntry {
  |    query: Query*         ← 指向Query节点
  |    query_name: "main"    ← 名称
  |    query_hints: "SeqScan(t1) HashJoin(t1 t2)"  ← hints
  |  }
  |
  ├─ bucket[2] → entry → NULL
  |       ↓
  |  QueryMetadataEntry {
  |    query: Query*         ← 指向SubLink中的Query
  |    query_name: "sublink_0"
  |    query_hints: "SeqScan(t2)"
  |  }
  |
  └─ bucket[...] → ...
```

### Hint应用过程

```
Stored Outline Hints:
"[main] SeqScan(t1) HashJoin(t1 t2) Leading((t1 t2))
 [sublink_0] SeqScan(t2)"

    ↓ parse_stored_hints()

List of ParsedHint:
  ├─ { query_name: "main", hint_text: "SeqScan(t1)" }
  ├─ { query_name: "main", hint_text: "HashJoin(t1 t2)" }
  ├─ { query_name: "main", hint_text: "Leading((t1 t2))" }
  └─ { query_name: "sublink_0", hint_text: "SeqScan(t2)" }

    ↓ apply_hints_to_query()

每个ParsedHint:
  1. 在hash table中查找对应query_name的Query
  2. 将hint_text追加到Query的metadata
  3. 完成hint关联
```

## 代码调用链

```
PostgreSQL核心
  |
  └─ parse_analyze()
       └─ post_parse_analyze_hook
            |
            v
         outline_post_parse_analyze()      ← pg_outline.c:421
            |
            ├─ normalize_query()
            ├─ compute_query_fingerprint()
            ├─ retrieve_outline_hints()
            |
            ├─ reconstruct_sql_with_positioned_hints()
            |     └─ 显示NOTICE消息
            |
            └─ apply_hints_to_sublinks()   ← pg_outline.c:4504
                  |
                  ├─ assign_query_names()
                  |     └─ 递归给Query树分配名称
                  |
                  ├─ apply_hints_to_query()  ← 主查询
                  |
                  └─ query_tree_walker()
                        └─ apply_sublink_hints_walker()  ← pg_outline.c:4534
                              |
                              └─ 对每个SubLink:
                                   └─ apply_hints_to_query()  ← subquery
```

## 对比:之前 vs. 现在

### 之前的实现

```
SQL → 解析 → Query树 → planner_hook
                           |
                           v
                      应用hints (太晚!)
                           |
                           v
                      优化器已经pull-up了SubLinks
                      无法区分哪些是sublinks
```

### 现在的实现

```
SQL → 解析 → Query树 → post_parse_analyze_hook ← 新增!
                           |
                           v
                    应用hints (刚好!)
                    SubLinks完整保留
                           |
                           v
                      planner_hook
                           |
                           v
                      优化器处理
                      hints已经关联好
```

## 示例执行

### 输入SQL
```sql
SELECT * FROM t1 WHERE c1 IN (SELECT c2 FROM t2 WHERE c2 < 5)
```

### Stored Outline
```
[main] SeqScan(t1) HashJoin(t1 t2)
[sublink_0] SeqScan(t2)
```

### 处理过程

1. **解析后**:
   ```
   Query (main) {
     targetList: [t1.*]
     jointree: {
       fromlist: [RangeTblRef(t1)]
       quals: SubLink {
         subLinkType: ANY_SUBLINK
         subselect: Query (subquery) {
           targetList: [c2]
           jointree: { fromlist: [RangeTblRef(t2)] }
         }
       }
     }
   }
   ```

2. **post_parse_analyze_hook应用hints后**:
   ```
   Query (main) → metadata.hints = "SeqScan(t1) HashJoin(t1 t2)"
     └─ SubLink
          └─ Query (sublink_0) → metadata.hints = "SeqScan(t2)"
   ```

3. **输出给用户**:
   ```
   NOTICE: Outline matched! Equivalent SQL with positioned hints:
   SELECT /*+ SeqScan(t1) HashJoin(t1 t2) */ * FROM t1
   WHERE c1 IN (SELECT /*+ SeqScan(t2) */ c2 FROM t2 WHERE c2 < 5);
   ```

4. **优化器看到**:
   - Query "main" 带hints
   - Query "sublink_0" 带hints
   - 可以正确应用到对应的relations
