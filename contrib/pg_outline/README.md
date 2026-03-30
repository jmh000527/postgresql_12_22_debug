# pg_outline - PostgreSQL Execution Plan Outline Extension

## Overview

`pg_outline` is a PostgreSQL extension that provides plan stabilization through execution plan outline hints. It is inspired by OceanBase's Outline feature and allows you to capture and fix execution plans for SQL queries.

## Features

- **Auto-generation of Hints**: Automatically generates hints from execution plans with positioned hint reconstruction
- **Hint Parsing and Application**: Complete hint parsing and application mechanism for true plan stabilization
- **Plan Stabilization**: Store and reuse execution plans to ensure consistent query performance
- **Outline Display**: Shows generated outline data after query execution
- **Easy Management**: SQL functions to create, drop, enable, and disable outlines
- **Hint Support**: Supports common hint types (scan methods, join methods, join order)
- **Path Filtering**: Enforces scan method hints through set_rel_pathlist_hook

## Installation

### Building the Extension

```bash
cd contrib/pg_outline
make
make install
```

### Loading the Extension

```sql
CREATE EXTENSION pg_outline;
```

## Configuration Parameters

The extension provides several GUC parameters:

- `pg_outline.enabled` (boolean, default: true)
  - Enables or disables the pg_outline extension

- `pg_outline.display_hints` (boolean, default: true)
  - Controls whether generated outline hints are displayed after query execution

- `pg_outline.mode` (string, default: 'auto')
  - Sets the outline generation mode
  - Values: 'auto', 'manual', 'off'
  - 'auto': Automatically generates outlines for all queries
  - 'manual': Only use manually created outlines
  - 'off': Disable outline generation

### Example Configuration

```sql
-- Enable pg_outline
SET pg_outline.enabled = true;

-- Show hints after query execution
SET pg_outline.display_hints = true;

-- Use auto mode
SET pg_outline.mode = 'auto';
```

## Usage

### Automatic Outline Generation

When `pg_outline.mode` is set to 'auto', the extension automatically generates outline data for each query:

```sql
-- Enable auto mode
SET pg_outline.mode = 'auto';
SET pg_outline.display_hints = true;

-- Run a query
SELECT t1.id, t1.name, t2.description
FROM test_table1 t1
JOIN test_table2 t2 ON t1.id = t2.table1_id
WHERE t1.value > 500
ORDER BY t1.id
LIMIT 10;

-- The extension will display generated outline data:
/*+
BEGIN_OUTLINE_DATA
[main] SeqScan(test_table1)
[main] IndexScan(test_table2)
[main] HashJoin(test_table1 test_table2)
[main] Leading((test_table1 test_table2))
END_OUTLINE_DATA
*/
```

**Note**: Generated hints now include `[query_name]` prefix for precise matching when applying hints to corresponding query nodes.

### Manual Outline Management

You can manually create, manage, and apply outlines:

```sql
-- Create an outline (with query_name prefix format)
SELECT pg_outline_create(
    'my_outline',  -- outline name
    'SELECT * FROM test_table1 WHERE value > ?',  -- query pattern
    '[main] SeqScan(test_table1)'  -- hints (must include [query_name] prefix)
);

-- List all outlines
SELECT * FROM pg_outline_list();

-- View enabled outlines
SELECT * FROM pg_outline_enabled;

-- Disable an outline
SELECT pg_outline_disable('my_outline');

-- Enable an outline
SELECT pg_outline_enable('my_outline');

-- Drop an outline
SELECT pg_outline_drop('my_outline');
```

### Outline Application and Plan Stabilization

When SQL with the same pattern is executed again, pg_outline automatically applies stored hints to reproduce the original execution plan:

```sql
-- 1. First, capture the plan using auto mode
SET pg_outline.mode = 'auto';
EXPLAIN SELECT * FROM t1 WHERE id < 100;
-- Record the generated hints

-- 2. Manually create outline (or use pg_outline_create_from_sql)
SELECT pg_outline_create(
    'test_outline',
    'SELECT * FROM t1 WHERE id < ?;',
    '[main] SeqScan(t1)'
);

-- 3. Switch to manual mode and enable detailed logging
SET pg_outline.mode = 'manual';
SET client_min_messages = 'DEBUG1';

-- 4. Execute query with same pattern, hints will be automatically applied
SELECT * FROM t1 WHERE id < 100;
-- Output will show:
-- DEBUG: pg_outline: computed fingerprint: ...
-- DEBUG: pg_outline: retrieved hints from stored outline
-- DEBUG: pg_outline: parsed 1 hints from stored outline
-- DEBUG: pg_outline: applied hint to query 'main': SeqScan(t1)
-- DEBUG: pg_outline: filtering paths for relation 't1' based on hints

-- 5. Verify the plan matches
EXPLAIN SELECT * FROM t1 WHERE id < 100;
-- Should show Seq Scan even if index scan might be better
```

## Hint Types

The extension supports various hint types similar to pg_hint_plan:

### Scan Method Hints

- `SeqScan(table)` - Force sequential scan
- `IndexScan(table)` - Force index scan
- `IndexOnlyScan(table)` - Force index-only scan
- `BitmapScan(table)` - Force bitmap scan

### Join Method Hints

- `NestLoop(tables)` - Force nested loop join
- `HashJoin(tables)` - Force hash join
- `MergeJoin(tables)` - Force merge join

### Join Order Hints

- `Leading(...)` - Specify join order using nested parentheses format to control join tree structure
  - Simple order: `Leading(t1 t2 t3)` - join left to right
  - Nested format: `Leading((t1 t2) (t3 t4))` - join t1 and t2 first, then t3 and t4, finally join the results
  - Complex nested: `Leading(((t1 t2) t3) t4)` - precisely control each step of join order

**Note**: pg_outline automatically generates Leading hints in nested parentheses format to accurately represent the join order in the plan tree. This format is consistent with pg_hint_plan syntax.

## Outline Data Format

The generated outline data follows a format similar to OceanBase and Oracle, with query_name prefix:

```
/*+
BEGIN_OUTLINE_DATA
[query_name] <hint1>
[query_name] <hint2>
...
END_OUTLINE_DATA
*/
```

Example:

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

**Hint Format Explanation**:
- `[query_name]`: Query name prefix to identify which query node the hint applies to
  - `[main]`: Main query
  - `[cte_<name>]`: CTE query
  - `[subquery_N]`: Subquery
  - `[sublink_N]`: SubLink subquery
- `hint_text`: Specific hint content (scan methods, join methods, etc.)

For complex multi-table joins:

```sql
SELECT * FROM t1
  JOIN t2 ON t1.id = t2.t1_id
  JOIN t3 ON t2.id = t3.t2_id
  JOIN t4 ON t3.id = t4.t3_id;
```

The generated Leading hint might look like:

```
Leading(((t1 t2) t3) t4)
```

This indicates: join t1 and t2 first, then join the result with t3, and finally with t4.

## Catalog Tables

The extension creates the following catalog structures:

- `pg_outline_data` - Stores outline definitions
  - `outline_id` - Unique identifier
  - `outline_name` - Name of the outline
  - `query_pattern` - SQL query pattern
  - `hint_string` - Hint string to apply
  - `enabled` - Whether the outline is active
  - `created_at` - Creation timestamp
  - `updated_at` - Last update timestamp

- `pg_outline_enabled` - View of enabled outlines

## Architecture

The extension uses PostgreSQL hooks to intercept query planning and execution:

1. **Planner Hook**: Intercepts query planning to generate hints from the chosen plan and apply stored hints before planning
2. **Set Rel Pathlist Hook**: Filters scan paths during path generation to enforce scan method hints
3. **Executor Hooks**: Track query execution and display outline data
4. **Plan Analysis**: Recursively analyzes the plan tree to extract relevant hints with positioned hint reconstruction
5. **Hint Storage**: Stores hints in catalog tables for later reuse
6. **Hint Application**: Complete hint parsing and application flow, including:
   - Parsing stored hint strings (`[query_name] hint_text` format)
   - Associating hints with corresponding Query nodes via metadata hash table
   - Filtering paths during planning to keep only hint-specified scan methods

## Comparison with Similar Tools

### vs. pg_hint_plan

- `pg_hint_plan`: Focuses on applying manually specified hints through SQL comment syntax
- `pg_outline`: Automatically generates hints from execution plans and manages them as outlines, supporting plan capture and reproduction
- Similarities: Both use similar hint syntax (SeqScan, IndexScan, Leading, etc.)
- Differences: pg_outline adds `[query_name]` prefix to support precise hint application for complex queries

### vs. OceanBase Outline

- Similar concept and API design
- Adapted for PostgreSQL's planner and executor architecture
- Compatible with PostgreSQL's standard query optimization
- Implements complete outline generation, storage, and application workflow

## Limitations

Current version limitations:

1. Leading hint parsing is complete, but join order enforcement is not implemented - the parser correctly identifies join order structure but does not override planner decisions
2. Currently uses simple string matching to check hints, can be improved with a full hint parser
3. If hints cannot be applied (e.g., table name mismatch), system falls back to normal planning

## Future Enhancements

Planned improvements:

- Full Leading hint enforcement (requires custom RelOptInfo join tree building to override planner's join order)
- Support for parallel query hints
- Extended hint types (e.g., SET, ROWS hints)
- Integration with pg_stat_statements for automatic outline creation
- Outline import/export functionality
- More sophisticated hint syntax parser
- Cost adjustment as an alternative to path filtering
- Hint conflict detection and warnings

## Completed Features (Version 1.0)

The following features have been fully implemented:

1. **Query Parameterization** ✓
   - Automatic replacement of literal values (numbers and strings) with `?` placeholders
   - Allows queries with different literal values to match the same outline
   - Example: `SELECT * FROM t1 WHERE id < 100` and `SELECT * FROM t1 WHERE id < 500` match the same pattern `select * from t1 where id < ?`

2. **Multi-relation Join Hints** ✓
   - Support for join hints with multiple relations: `HashJoin(t1 t2 t3)`
   - `extract_relations_from_join_hint()` function parses space-separated table names
   - Enables control of join methods for complex multi-way joins

3. **Leading Hint Parsing** ✓
   - Complete parser for nested Leading hint syntax
   - Supports simple flat format: `Leading(t1 t2 t3)`
   - Supports nested format: `Leading((t1 t2) t3)`
   - Supports bushy join format: `Leading((t1 t2) (t3 t4))`
   - Supports complex nested: `Leading(((t1 t2) t3) t4)`
   - Recursive descent parser builds tree structure from hint string
   - Tree-to-string conversion for verification
   - Proper memory management with palloc/pfree
   - **Note**: Parser is complete; join order enforcement is not yet implemented

## Technical Implementation Details

### Hint Parsing Flow

pg_outline implements a complete hint parsing and application mechanism:

1. **normalize_query()**: Parameterizes queries for pattern matching
   - Replaces all string literals with `?` placeholder
   - Replaces all numeric literals with `?` placeholder
   - Preserves identifiers, keywords, and structure
   - Enables queries with different literals to match same pattern
   - Example: `SELECT * FROM t1 WHERE id < 100` → `select * from t1 where id < ?`

2. **extract_relations_from_join_hint()**: Parses multi-relation join hints
   - Input: `HashJoin(t1 t2 t3)` or `NestLoop(orders items)`
   - Output: List of relation names: ["t1", "t2", "t3"]
   - Used for validating and applying join method hints

3. **parse_stored_hints()**: Parses stored hint strings
   - Input format: `[query_name] hint_text\n[query_name2] hint_text2`
   - Output: List of ParsedHint structures, each containing query_name and hint_text

4. **apply_hints_to_query()**: Associates parsed hints with Query nodes
   - Looks up matching query_name in metadata hash table
   - Stores hint_text in corresponding QueryMetadataEntry
   - Supports merging multiple hints for the same query

5. **outline_set_rel_pathlist()**: Filters paths during path generation
   - Implements set_rel_pathlist_hook
   - Gets query hints for current relation
   - Checks for scan method hints for this table
   - Filters rel->pathlist to keep only matching path types
   - Supports SeqScan, IndexScan, IndexOnlyScan hints

6. **parse_leading_hint()**: Parses Leading hint into tree structure
   - Recursive descent parser for nested parentheses syntax
   - Builds LeadingHintNode tree representing join order
   - Supports all Leading hint formats (flat, nested, bushy)
   - Grammar: `element ::= relation_name | '(' element element ')'`
   - Returns tree structure or NULL on parse error

7. **outline_join_search()**: Detects and parses Leading hints
   - Implements join_search_hook
   - Extracts Leading hint string with proper parenthesis matching
   - Calls parse_leading_hint() to build tree structure
   - Logs parsed tree structure for verification (NOTICE level)
   - Parser complete; join order enforcement not yet implemented

### Workflow

**Generation Phase** (auto mode):
1. User executes a query
2. outline_planner hook intercepts planning process
3. System generates execution plan
4. Extracts hints from plan (with position information)
5. Generates hint string in `[query_name] hint_text` format
6. If display_hints is enabled, shows generated outline

**Application Phase** (manual mode):
1. User executes SQL with same pattern
2. outline_planner normalizes query using normalize_query() (parameterizes literals)
3. Computes query fingerprint from normalized query
4. Retrieves matching stored hints from pg_outline_data table
5. Calls parse_stored_hints() to parse hint string into ParsedHint list
6. Calls apply_hints_to_query() to associate hints with Query nodes
7. Sets active_outline_hints global variable
8. Calls standard_planner() to begin planning
9. During planning, outline_set_rel_pathlist() hook is called for scan hints
10. During planning, outline_join_search() hook is called for join order hints
11. Hooks filter paths to keep only hint-specified methods
12. Planner chooses from filtered paths (restricted to hint-specified methods)
13. After planning completes, clears active_outline_hints
14. Returns PlannedStmt (should match original plan)

### Key Data Structures

```c
typedef struct ParsedHint
{
    char *query_name;  /* e.g., "main", "sublink_0" */
    char *hint_text;   /* e.g., "SeqScan(t1)", "IndexScan(t2)" */
} ParsedHint;

typedef struct LeadingHintNode
{
    LeadingHintNodeType type;  /* LEADING_NODE_RELATION or LEADING_NODE_JOIN */
    union
    {
        char *relation_name;           /* For leaf nodes (relations) */
        struct
        {
            struct LeadingHintNode *left;   /* For join nodes */
            struct LeadingHintNode *right;
        } join;
    } u;
} LeadingHintNode;
```

**LeadingHintNode** represents the parsed Leading hint as a tree:
- Leaf nodes (LEADING_NODE_RELATION) contain relation names
- Internal nodes (LEADING_NODE_JOIN) represent join operations with left and right children
- Example: `Leading((t1 t2) t3)` creates tree: JOIN(JOIN(t1, t2), t3)

Global variables:
- `active_outline_hints`: List of active hints for current query
- `prev_set_rel_pathlist_hook`: Saved previous set_rel_pathlist_hook pointer
- `prev_join_search_hook`: Saved previous join_search_hook pointer

## Examples

### Example 1: Query Parameterization - Same Outline for Different Literals

```sql
-- Enable auto mode to capture plan
SET pg_outline.mode = 'auto';
SET pg_outline.display_hints = true;

-- Run query with literal value 100
EXPLAIN SELECT * FROM t1 WHERE id < 100;
-- System generates outline with parameterized pattern

-- Manually create outline (or system auto-generates it)
SELECT pg_outline_create(
    'my_query_outline',
    'select * from t1 where id < ?',  -- Parameterized pattern
    '[main] SeqScan(t1)'
);

-- Switch to manual mode
SET pg_outline.mode = 'manual';

-- Run query with different literal value (500)
-- This will match the same outline!
EXPLAIN SELECT * FROM t1 WHERE id < 500;
-- Uses the stored outline because both queries normalize to "select * from t1 where id < ?"

-- Run query with another different value (1000)
EXPLAIN SELECT * FROM t1 WHERE id < 1000;
-- Also matches the same outline!
```

### Example 2: Multi-relation Join Hints

```sql
-- Create outline with 3-way join hint
SELECT pg_outline_create(
    'three_way_join',
    'select * from t1 join t2 on t1.id = t2.t1_id join t3 on t2.id = t3.t2_id where t1.value > ?',
    '[main] HashJoin(t1 t2 t3) [main] SeqScan(t1) [main] IndexScan(t2)'
);

-- The HashJoin(t1 t2 t3) hint tells pg_outline that all three tables
-- should be joined using hash join method
```

### Example 3: Stabilize a Complex Query

```sql
-- Create test tables
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

-- Run query with auto outline generation
SET pg_outline.mode = 'auto';
SET pg_outline.display_hints = true;

SELECT o.order_id, o.order_date, SUM(oi.quantity * oi.price) as total
FROM orders o
JOIN order_items oi ON o.order_id = oi.order_id
WHERE o.customer_id = 12345
GROUP BY o.order_id, o.order_date
ORDER BY o.order_date DESC;

-- The extension displays the generated outline
-- You can then create a permanent outline based on this
```

### Example 2: Manage Outlines for Different Environments

```sql
-- Development environment: use hash joins
SELECT pg_outline_create(
    'report_query_dev',
    'SELECT * FROM large_table1 t1 JOIN large_table2 t2 ON ...',
    '[main] HashJoin(large_table1 large_table2)'
);

-- Production environment: use different plan
SELECT pg_outline_create(
    'report_query_prod',
    'SELECT * FROM large_table1 t1 JOIN large_table2 t2 ON ...',
    '[main] MergeJoin(large_table1 large_table2) [main] IndexScan(large_table1)'
);

-- Switch between environments
SELECT pg_outline_enable('report_query_prod');
SELECT pg_outline_disable('report_query_dev');
```

### Example 3: Debugging Hint Application

```sql
-- Enable detailed logging to see hint application process
SET client_min_messages = 'DEBUG1';
SET pg_outline.mode = 'manual';

-- Execute query
SELECT * FROM t1 WHERE id < 100;

-- Log output example:
-- DEBUG: pg_outline: computed fingerprint: 1234567890
-- DEBUG: pg_outline: retrieved hints from stored outline: [main] SeqScan(t1)
-- DEBUG: pg_outline: parsed 1 hints from stored outline
-- DEBUG: pg_outline: applied hint to query 'main': SeqScan(t1)
-- DEBUG: pg_outline: filtering paths for relation 't1' based on hints
-- DEBUG: pg_outline: filtered from 3 to 1 paths for 't1'
```

## Troubleshooting

### Extension Not Loading

```sql
-- Check if extension is installed
SELECT * FROM pg_available_extensions WHERE name = 'pg_outline';

-- Check for errors in PostgreSQL log
-- Look for "pg_outline extension loaded" message
```

### Hints Not Displaying

```sql
-- Verify configuration
SHOW pg_outline.enabled;
SHOW pg_outline.display_hints;
SHOW pg_outline.mode;

-- Make sure settings are correct
SET pg_outline.enabled = true;
SET pg_outline.display_hints = true;
SET pg_outline.mode = 'auto';
```

### Outline Not Applied

```sql
-- Check if outline is enabled
SELECT * FROM pg_outline_enabled WHERE outline_name = 'your_outline';

-- Verify query pattern matches
-- Note: Current version requires exact match

-- Enable detailed logging to see application process
SET client_min_messages = 'DEBUG1';

-- Check hint format is correct (must include [query_name] prefix)
SELECT outline_name, hint_string FROM pg_outline_data
WHERE outline_name = 'your_outline';

-- Ensure hint format is: [main] SeqScan(table) not SeqScan(table)
```

### Hints Not Taking Effect

If hints are applied but plan doesn't change:

1. **Check table name**: Table name in hint must exactly match table name in query
2. **Check hint type**: Ensure hint type (SeqScan, IndexScan, etc.) is applicable for the table
3. **Review logs**: DEBUG1 level logs show path filtering process
4. **Verify path exists**: If hint-specified path doesn't exist (e.g., IndexScan with no index), system falls back to normal planning

```sql
-- View available paths
SET enable_seqscan = off;  -- Disable sequential scan
EXPLAIN SELECT * FROM t1 WHERE id < 100;  -- Check if index scan path exists
SET enable_seqscan = on;   -- Restore setting
```

## Contributing

Contributions are welcome! Areas for contribution:

- Additional hint types
- Improved query pattern matching
- Performance optimizations
- Documentation improvements
- Test coverage

## License

This extension is released under the PostgreSQL License.

## Authors

PostgreSQL Extension Development Team

## References

- [PostgreSQL Planner Hooks](https://www.postgresql.org/docs/current/planner-optimizer.html)
- [pg_hint_plan](https://github.com/ossc-db/pg_hint_plan)
- [OceanBase Outline](https://www.oceanbase.com/docs/oceanbase-database/oceanbase-database/V3.1.0/outline-management)
- [Oracle SQL Plan Management](https://docs.oracle.com/en/database/oracle/oracle-database/19/tgsql/overview-of-sql-plan-management.html)
