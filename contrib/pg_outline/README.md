# pg_outline - PostgreSQL Execution Plan Outline Extension

## Overview

`pg_outline` is a PostgreSQL extension that provides plan stabilization through execution plan outline hints. It is inspired by OceanBase's Outline feature and allows you to capture and fix execution plans for SQL queries.

## Features

- **Auto-generation of Hints**: Automatically generates hints from execution plans
- **Plan Stabilization**: Store and reuse execution plans to ensure consistent query performance
- **Outline Display**: Shows generated outline data after query execution
- **Easy Management**: SQL functions to create, drop, enable, and disable outlines
- **Hint Support**: Based on common hint types (scan methods, join methods, join order)

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
SeqScan(table)
HashJoin(...)
END_OUTLINE_DATA
*/
```

### Manual Outline Management

You can manually create, manage, and apply outlines:

```sql
-- Create an outline
SELECT pg_outline_create(
    'my_outline',  -- outline name
    'SELECT * FROM test_table1 WHERE value > ?',  -- query pattern
    'SeqScan(test_table1)'  -- hints (optional)
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

- `Leading(table1 table2 table3)` - Specify join order

## Outline Data Format

The generated outline data follows a format similar to OceanBase and Oracle:

```
/*+
BEGIN_OUTLINE_DATA
<hint1>
<hint2>
...
END_OUTLINE_DATA
*/
```

Example:

```
/*+
BEGIN_OUTLINE_DATA
SeqScan(test_table1)
HashJoin(test_table1 test_table2)
Leading(test_table1 test_table2)
END_OUTLINE_DATA
*/
```

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

1. **Planner Hook**: Intercepts query planning to generate hints from the chosen plan
2. **Executor Hooks**: Track query execution and display outline data
3. **Plan Analysis**: Recursively analyzes the plan tree to extract relevant hints
4. **Hint Storage**: Stores hints in catalog tables for later reuse

## Comparison with Similar Tools

### vs. pg_hint_plan

- `pg_hint_plan`: Focuses on applying manually specified hints
- `pg_outline`: Automatically generates hints from execution plans and manages them as outlines

### vs. OceanBase Outline

- Similar concept and API design
- Adapted for PostgreSQL's planner and executor architecture
- Compatible with PostgreSQL's standard query optimization

## Limitations

Current version limitations:

1. Simplified hint generation (basic scan and join methods)
2. Query pattern matching is exact (no parameterization yet)
3. Limited to single-query outlines (no support for complex CTEs yet)
4. Hint application not yet fully implemented

## Future Enhancements

Planned improvements:

- Full hint application using planner hooks
- Query fingerprinting for better pattern matching
- Support for parallel query hints
- Extended hint types (e.g., SET, ROWS hints)
- Integration with pg_stat_statements for automatic outline creation
- Outline import/export functionality

## Examples

### Example 1: Stabilize a Complex Query

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
    'HashJoin(large_table1 large_table2)'
);

-- Production environment: use different plan
SELECT pg_outline_create(
    'report_query_prod',
    'SELECT * FROM large_table1 t1 JOIN large_table2 t2 ON ...',
    'MergeJoin(large_table1 large_table2) IndexScan(large_table1)'
);

-- Switch between environments
SELECT pg_outline_enable('report_query_prod');
SELECT pg_outline_disable('report_query_dev');
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
