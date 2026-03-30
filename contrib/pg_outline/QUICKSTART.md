# pg_outline Quick Start Guide

## What is pg_outline?

`pg_outline` is a PostgreSQL extension that implements OceanBase-style outline functionality, allowing you to persistently store and automatically apply query hints to control the query planner's behavior without modifying application code.

## Installation

### 1. Build and Install

```bash
cd contrib/pg_outline
make
sudo make install
```

### 2. Configure PostgreSQL

Edit `postgresql.conf`:

```ini
shared_preload_libraries = 'pg_outline'
```

Restart PostgreSQL:

```bash
sudo systemctl restart postgresql
```

### 3. Create Extension

```sql
CREATE EXTENSION pg_outline;
```

## Quick Examples

### Example 1: Force Sequential Scan

```sql
-- Create an outline to force sequential scan on a table
SELECT pg_outline.create_outline(
    'force_seqscan_users',
    'SELECT * FROM users WHERE age > 25',
    'SeqScan(users)',
    'public',
    'Force seq scan for this specific query'
);

-- Now when you run the query, the hint will be automatically applied
SELECT * FROM users WHERE age > 25;
```

### Example 2: Control Join Method

```sql
-- Force hash join between two tables
SELECT pg_outline.create_outline(
    'orders_customers_hashjoin',
    'SELECT o.*, c.name FROM orders o JOIN customers c ON o.customer_id = c.id',
    'HashJoin(o c)',
    'public',
    'Use hash join for orders-customers'
);

-- Query will use hash join automatically
SELECT o.*, c.name
FROM orders o
JOIN customers c ON o.customer_id = c.id;
```

### Example 3: Inline Hints (No Outline Needed)

```sql
-- Use inline hints directly in queries
SELECT /*+ SeqScan(users) */ * FROM users WHERE age > 25;

SELECT /*+ HashJoin(o c) IndexScan(orders) */
    o.*, c.name
FROM orders o
JOIN customers c ON o.customer_id = c.id;
```

### Example 4: Control Join Order

```sql
-- Force specific join order
SELECT pg_outline.create_outline(
    'three_way_join_order',
    'SELECT * FROM t1 JOIN t2 ON t1.id = t2.id JOIN t3 ON t2.id = t3.id',
    'Leading(t1 t2 t3) HashJoin(t1 t2) NestLoop(t1_t2 t3)',
    'public',
    'Control join order and methods'
);
```

## Managing Outlines

### List All Outlines

```sql
-- View all outlines
SELECT * FROM pg_outline.outlines;

-- View only enabled outlines
SELECT * FROM pg_outline.active_outlines;
```

### Enable/Disable Outlines

```sql
-- Disable an outline temporarily
SELECT pg_outline.disable_outline('force_seqscan_users', 'public');

-- Re-enable it
SELECT pg_outline.enable_outline('force_seqscan_users', 'public');
```

### Delete Outlines

```sql
-- Delete an outline
SELECT pg_outline.drop_outline('force_seqscan_users', 'public');
```

### Get Statistics

```sql
-- See outline statistics
SELECT * FROM pg_outline.get_statistics();
```

## Configuration Options

```sql
-- Enable/disable outline functionality globally
SET pg_outline.enabled = true;

-- Control query matching mode
SET pg_outline.match_mode = 'exact';      -- Exact text match (default)
SET pg_outline.match_mode = 'normalized';  -- Ignore whitespace/case

-- Enable logging
SET pg_outline.log_level = 'notice';  -- See when hints are applied
```

## Common Use Cases

### 1. Stabilize Query Plans

When statistics change and cause plan regression:

```sql
SELECT pg_outline.create_outline(
    'stable_plan_critical_query',
    'SELECT ...',  -- your critical query
    'IndexScan(table1 idx1) HashJoin(table1 table2)',
    'public',
    'Stabilize plan for critical business query'
);
```

### 2. Optimize Without Changing Code

When you can't modify application code:

```sql
-- Application runs: SELECT * FROM orders WHERE status = 'pending'
-- Add outline to optimize it
SELECT pg_outline.create_outline(
    'optimize_pending_orders',
    'SELECT * FROM orders WHERE status = ''pending''',
    'IndexScan(orders status_idx)',
    'public',
    'Optimize query from legacy application'
);
```

### 3. Fix Poor Join Choices

When PostgreSQL chooses the wrong join method:

```sql
SELECT pg_outline.create_outline(
    'fix_join_method',
    'SELECT * FROM large_table l JOIN small_table s ON l.s_id = s.id',
    'HashJoin(l s)',
    'public',
    'Hash join is better than nested loop here'
);
```

## Supported Hint Types

### Scan Methods
- `SeqScan(table)` - Force sequential scan
- `IndexScan(table [index])` - Force index scan
- `IndexOnlyScan(table [index])` - Force index-only scan
- `BitmapScan(table [index])` - Force bitmap scan
- `NoSeqScan(table)` - Prevent sequential scan
- `NoIndexScan(table)` - Prevent index scan

### Join Methods
- `NestLoop(t1 t2 ...)` - Force nested loop join
- `HashJoin(t1 t2 ...)` - Force hash join
- `MergeJoin(t1 t2 ...)` - Force merge join
- `NoNestLoop(t1 t2 ...)` - Prevent nested loop
- `NoHashJoin(t1 t2 ...)` - Prevent hash join
- `NoMergeJoin(t1 t2 ...)` - Prevent merge join

### Join Order
- `Leading(t1 t2 t3 ...)` - Specify join order

## Troubleshooting

### Outline Not Applied?

```sql
-- 1. Check if outline system is enabled
SHOW pg_outline.enabled;

-- 2. Check if specific outline exists and is enabled
SELECT * FROM pg_outline.outlines WHERE name = 'your_outline_name';

-- 3. Enable debug logging
SET pg_outline.log_level = 'debug';
-- Then run your query to see what's happening
```

### Query Text Doesn't Match?

```sql
-- Use normalized matching to ignore whitespace/case differences
SET pg_outline.match_mode = 'normalized';
```

## Best Practices

1. **Always Add Descriptions**: Explain why you created the outline
2. **Test Before and After**: Use EXPLAIN to verify the hint works
3. **Monitor Performance**: Check if the outline actually improves performance
4. **Regular Reviews**: Periodically check if outlines are still needed
5. **Use Inline Hints for Testing**: Test hints inline before creating permanent outlines

## Performance Impact

- Outline lookup adds ~1-2ms per query (negligible)
- No shared memory overhead
- Hint application is very fast
- Properly chains with other planner hooks

## Getting Help

Check the full documentation:
- `README.md` - Complete feature documentation
- `OUTLINE_使用指南.md` - Chinese user guide

For issues or questions, check the PostgreSQL logs with:
```sql
SET pg_outline.log_level = 'debug';
```

## Example Workflow

```sql
-- 1. Identify a slow query
EXPLAIN ANALYZE SELECT ...;

-- 2. Test with inline hints
EXPLAIN ANALYZE SELECT /*+ IndexScan(table) */ ...;

-- 3. If it helps, create permanent outline
SELECT pg_outline.create_outline(
    'my_optimization',
    'SELECT ...',
    'IndexScan(table)',
    'public',
    'Fixes slow query - improves from 5s to 0.5s'
);

-- 4. Verify it's working
SELECT * FROM pg_outline.outlines WHERE name = 'my_optimization';

-- 5. Monitor and adjust as needed
SELECT pg_outline.disable_outline('my_optimization', 'public');  -- if issues
```

---

**Note**: This extension is compatible with PostgreSQL 12.22 and uses pg_hint_plan-compatible hint syntax.
