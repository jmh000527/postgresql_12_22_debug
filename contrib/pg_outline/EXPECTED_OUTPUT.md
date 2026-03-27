# Query Naming Test - Expected Output Examples (预期输出示例)

## Understanding the Output Format

When pg_outline displays hints with query names, the format is:
```
[query_name] hint_content
```

For example:
- `[main] SeqScan(t1)`
- `[cte_orders] IndexScan(orders_table)`
- `[subquery_0] HashJoin(t2 t3)`
- `[sublink_0] NestLoop(t4 t5)`

## Test Case Output Examples

### Example 1: Simple Query
**SQL:**
```sql
SELECT /*+ SeqScan(t1) */ * FROM t1 WHERE id < 10;
```

**Expected in EXPLAIN output or NOTICE:**
```
[main] SeqScan(t1)
```

**Verification:**
- ✓ Hint has `[main]` prefix
- ✓ No other query names present (only one query)

---

### Example 2: Query with CTE
**SQL:**
```sql
WITH orders AS (
    SELECT /*+ SeqScan(orders_table) */ * FROM orders_table WHERE amount > 500
)
SELECT /*+ HashJoin(customers orders) */ *
FROM customers
JOIN orders ON customers.id = orders.customer_id;
```

**Expected Output:**
```
[main] HashJoin(customers orders)
[cte_orders] SeqScan(orders_table)
```

**Verification:**
- ✓ Main query has `[main]` prefix
- ✓ CTE uses actual CTE name: `[cte_orders]` (not `[cte_0]`)
- ✓ Both hints are present

---

### Example 3: Query with Subquery in FROM
**SQL:**
```sql
SELECT /*+ HashJoin(t1 sub) */ t1.id, sub.data
FROM t1
JOIN (
    SELECT /*+ IndexScan(t2) */ id, data FROM t2 WHERE id < 50
) AS sub ON t1.id = sub.id;
```

**Expected Output:**
```
[main] HashJoin(t1 sub)
[subquery_0] IndexScan(t2)
```

**Verification:**
- ✓ Main query: `[main]`
- ✓ First subquery: `[subquery_0]` (numbered, not named after alias)
- ✓ Both hints present

---

### Example 4: Query with SubLink in WHERE (IN)
**SQL:**
```sql
SELECT /*+ SeqScan(t1) */ *
FROM t1
WHERE id IN (
    SELECT /*+ IndexScan(t2) */ t1_id FROM t2 WHERE data LIKE 'data_1%'
);
```

**Expected Output:**
```
[main] SeqScan(t1)
[sublink_0] IndexScan(t2)
```

**Verification:**
- ✓ Outer query: `[main]`
- ✓ SubLink query: `[sublink_0]` (first SubLink)
- ✓ Both hints present

---

### Example 5: Multiple CTEs
**SQL:**
```sql
WITH cte1 AS (
    SELECT /*+ SeqScan(t1) */ * FROM t1 WHERE value > 100
),
cte2 AS (
    SELECT /*+ SeqScan(t2) */ * FROM t2 WHERE id < 50
)
SELECT /*+ HashJoin(cte1 cte2) */ cte1.id, cte2.data
FROM cte1
JOIN cte2 ON cte1.id = cte2.t1_id;
```

**Expected Output:**
```
[main] HashJoin(cte1 cte2)
[cte_cte1] SeqScan(t1)
[cte_cte2] SeqScan(t2)
```

**Verification:**
- ✓ Main query: `[main]`
- ✓ First CTE: `[cte_cte1]` (uses CTE name)
- ✓ Second CTE: `[cte_cte2]` (uses CTE name)
- ✓ All three hints present

---

### Example 6: Nested SubLinks
**SQL:**
```sql
SELECT /*+ SeqScan(t1) */ *
FROM t1
WHERE id IN (
    SELECT /*+ SeqScan(t2) */ t1_id
    FROM t2
    WHERE id IN (
        SELECT /*+ SeqScan(t3) */ id FROM t3 WHERE id < 20
    )
);
```

**Expected Output:**
```
[main] SeqScan(t1)
[sublink_0] SeqScan(t2)
[sublink_1] SeqScan(t3)
```

**Verification:**
- ✓ Outer query: `[main]`
- ✓ First level SubLink: `[sublink_0]`
- ✓ Nested SubLink: `[sublink_1]` (continues numbering)
- ✓ All three hints present
- ✓ Numbers are sequential, no gaps

---

### Example 7: Multiple Subqueries in FROM
**SQL:**
```sql
SELECT /*+ HashJoin(sub1 sub2) */ *
FROM (
    SELECT /*+ SeqScan(t1) */ * FROM t1
) AS sub1
JOIN (
    SELECT /*+ SeqScan(t2) */ * FROM t2
) AS sub2 ON sub1.id = sub2.t1_id;
```

**Expected Output:**
```
[main] HashJoin(sub1 sub2)
[subquery_0] SeqScan(t1)
[subquery_1] SeqScan(t2)
```

**Verification:**
- ✓ Main query: `[main]`
- ✓ First subquery: `[subquery_0]`
- ✓ Second subquery: `[subquery_1]` (sequential numbering)
- ✓ All three hints present

---

### Example 8: Mixed SubLinks (IN + Scalar)
**SQL:**
```sql
SELECT /*+ SeqScan(t1) */ *
FROM t1
WHERE id IN (
    SELECT /*+ SeqScan(t2) */ t1_id FROM t2
)
AND value > (
    SELECT /*+ SeqScan(t3) */ AVG(id) FROM t3
);
```

**Expected Output:**
```
[main] SeqScan(t1)
[sublink_0] SeqScan(t2)
[sublink_1] SeqScan(t3)
```

**Verification:**
- ✓ Main query: `[main]`
- ✓ IN subquery: `[sublink_0]`
- ✓ Scalar subquery: `[sublink_1]` (different clause, same numbering series)
- ✓ All three hints present

---

### Example 9: Complex - All Types Combined
**SQL:**
```sql
WITH cte1 AS (
    SELECT /*+ SeqScan(t2) */ * FROM t2
)
SELECT /*+ HashJoin(t1 sub) */ t1.id, sub.name
FROM t1
JOIN (
    SELECT /*+ NestLoop(t3 cte1) */ t3.id, cte1.data as name
    FROM t3
    JOIN cte1 ON t3.id = cte1.id
    WHERE t3.id IN (
        SELECT /*+ SeqScan(t4) */ t4_id FROM t4
    )
) AS sub ON t1.id = sub.id;
```

**Expected Output:**
```
[main] HashJoin(t1 sub)
[cte_cte1] SeqScan(t2)
[subquery_0] NestLoop(t3 cte1)
[sublink_0] SeqScan(t4)
```

**Verification:**
- ✓ Main query: `[main]`
- ✓ CTE: `[cte_cte1]` (uses CTE name)
- ✓ Subquery: `[subquery_0]` (first and only subquery)
- ✓ SubLink (inside subquery): `[sublink_0]` (first SubLink overall)
- ✓ All four hints present
- ✓ All four query types represented

---

### Example 10: Scalar Subquery in SELECT
**SQL:**
```sql
SELECT /*+ SeqScan(t1) */
    t1.id,
    t1.name,
    (SELECT /*+ SeqScan(t2) */ COUNT(*) FROM t2 WHERE t2.t1_id = t1.id) as count
FROM t1
WHERE id < 10;
```

**Expected Output:**
```
[main] SeqScan(t1)
[sublink_0] SeqScan(t2)
```

**Verification:**
- ✓ Main query: `[main]`
- ✓ Scalar subquery in SELECT: `[sublink_0]` (counts as SubLink)
- ✓ Both hints present

---

## Common Verification Patterns

### Pattern 1: Check for Brackets
All query names must be enclosed in square brackets:
```
✓ [main] SeqScan(t1)
✗ main SeqScan(t1)
✗ SeqScan(t1) [main]
```

### Pattern 2: Check CTE Names
CTEs must use their actual names:
```sql
WITH my_orders AS (SELECT /*+ ... */ ...)
```
```
✓ [cte_my_orders] ...
✗ [cte_0] ...
✗ [subquery_0] ...
```

### Pattern 3: Check Sequential Numbering
Numbers must be sequential without gaps:
```
✓ [subquery_0], [subquery_1], [subquery_2]
✗ [subquery_0], [subquery_2], [subquery_3]  (missing 1)
✗ [subquery_1], [subquery_2]  (doesn't start from 0)
```

### Pattern 4: Check Separate Counters
Subqueries and SubLinks have separate counters:
```
✓ [subquery_0], [sublink_0]  (both start at 0 independently)
✗ [subquery_0], [sublink_1]  (should be sublink_0)
```

### Pattern 5: Check All Hints Present
Every inline hint should appear in output:
```sql
-- 3 hints in query
SELECT /*+ hint1 */ ... WHERE ... IN (SELECT /*+ hint2 */ ...)
AND ... IN (SELECT /*+ hint3 */ ...)
```
```
✓ Three hints in output: [main] hint1, [sublink_0] hint2, [sublink_1] hint3
✗ Only two hints in output (one missing)
```

---

## Quick Checklist

When reviewing test output, verify:

1. **Format**
   - [ ] All hints have `[query_name] hint_content` format
   - [ ] Query names are enclosed in brackets `[...]`
   - [ ] Space between query name and hint content

2. **Main Query**
   - [ ] Top-level query always named `[main]`
   - [ ] Never `[main_0]` for first query (unless multiple statements)

3. **CTEs**
   - [ ] Named with `cte_` prefix
   - [ ] Use actual CTE name from SQL
   - [ ] Examples: `[cte_orders]`, `[cte_products]`, `[cte_cte1]`

4. **Subqueries**
   - [ ] Named with `subquery_` prefix
   - [ ] Numbered starting from 0
   - [ ] Sequential: `[subquery_0]`, `[subquery_1]`, etc.

5. **SubLinks**
   - [ ] Named with `sublink_` prefix
   - [ ] Numbered starting from 0
   - [ ] Sequential: `[sublink_0]`, `[sublink_1]`, etc.

6. **Completeness**
   - [ ] All inline hints appear in output
   - [ ] No missing hints
   - [ ] No duplicate query names/numbers

---

## Debugging Common Issues

### Issue: Missing `[query_name]` prefix
**Symptom:**
```
SeqScan(t1)  ← Missing prefix
HashJoin(t2 t3)  ← Missing prefix
```

**Possible Causes:**
1. Query naming not executed (`assign_query_names()` not called)
2. Metadata hash table not created
3. Using old code path that doesn't add prefixes

**Fix:** Verify `outline_planner()` calls `assign_query_names()` before optimization

---

### Issue: Wrong query type (e.g., CTE shown as subquery)
**Symptom:**
```sql
WITH my_cte AS (SELECT /*+ ... */ ...)
```
```
[subquery_0] ...  ← Should be [cte_my_cte]
```

**Possible Causes:**
1. CTE detection logic failing
2. Wrong parameters passed to `assign_query_names()`

**Fix:** Check `cte_name` parameter is passed correctly

---

### Issue: Duplicate numbers
**Symptom:**
```
[sublink_0] SeqScan(t2)
[sublink_0] SeqScan(t3)  ← Duplicate!
```

**Possible Causes:**
1. Static variable persistence (should be fixed now)
2. Counter not incrementing properly

**Fix:** Verify `query_index` is in `QueryNamingContext`, not static

---

### Issue: Numbers not starting from 0
**Symptom:**
```
[subquery_2] ...  ← Should start at 0
[subquery_3] ...
```

**Possible Causes:**
1. Counter not reset between queries
2. Using persistent counter from previous query

**Fix:** Verify `context->query_index = 0` in `assign_query_names()` for top-level query

---

## Success Indicators

✅ **Test passes if you see:**
- All hints have `[query_name]` prefix
- Main queries show `[main]`
- CTEs show `[cte_actual_name]`
- Subqueries numbered sequentially: `[subquery_0]`, `[subquery_1]`, ...
- SubLinks numbered sequentially: `[sublink_0]`, `[sublink_1]`, ...
- No missing hints
- No duplicate names/numbers

❌ **Test fails if you see:**
- Hints without prefixes
- Wrong query type names
- Gaps in numbering
- Duplicate numbers
- Missing hints
- Generic names like `[query_0]` instead of proper names
