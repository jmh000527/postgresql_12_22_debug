# PostgreSQL Outline Feature

## Overview

This implementation adds an **Outline** feature to PostgreSQL, similar to OceanBase's Outline interface and inspired by pg_hint_plan. The Outline feature allows DBAs to fix and stabilize execution plans by storing optimizer hints for specific SQL queries.

## Features

The Outline system provides:

1. **Plan Stabilization**: Fix execution plans for critical queries to prevent performance regressions
2. **Hint-based Control**: Use pg_hint_plan-compatible hint syntax for fine-grained control
3. **Catalog Storage**: Store outlines persistently in the `pg_outline` system catalog
4. **Dynamic Management**: Enable/disable outlines without modifying application code

## Architecture

### Components

#### 1. **Catalog Table (`pg_outline`)**
Located in: `src/include/catalog/pg_outline.h`

The `pg_outline` system catalog stores outline definitions with the following structure:
- `oid` - Object identifier
- `outlinename` - Name of the outline
- `outlinenamespace` - Namespace OID
- `outlineowner` - Owner OID
- `outlineenabled` - Whether the outline is currently enabled
- `outlinequery` - Normalized SQL query text (query signature)
- `outlinehints` - Hint string in pg_hint_plan format

#### 2. **Hint System**
Located in: `src/backend/optimizer/outline/`

**outline_hints.c** - Hint parsing
- Parses hint strings in pg_hint_plan format
- Supports scan method hints: `SeqScan()`, `IndexScan()`, `NoSeqScan()`, `NoIndexScan()`
- Supports join method hints: `NestLoop()`, `HashJoin()`, `MergeJoin()`

**outline_plan.c** - Plan-to-hint derivation
- Extracts hints from a `PlannedStmt` execution plan
- Generates hint strings that can reproduce the same plan
- Walks the plan tree to identify scan and join methods

**outline_apply.c** - Hint application
- Implements optimizer hooks (`set_rel_pathlist_hook`, `set_join_pathlist_hook`)
- Filters paths based on active hints
- Enforces scan and join method preferences

#### 3. **SQL Functions**
Located in: `src/backend/utils/adt/pg_outline_funcs.c`

Four SQL-callable functions for outline management:
- `pg_create_outline(name, query, hints)` - Create a new outline
- `pg_drop_outline(name)` - Drop an existing outline
- `pg_enable_outline(name)` - Enable an outline
- `pg_disable_outline(name)` - Disable an outline

## Supported Hint Types

### Scan Method Hints

```sql
SeqScan(table_name)          -- Force sequential scan
IndexScan(table_name index)  -- Force index scan with specific index
NoSeqScan(table_name)        -- Disable sequential scan
NoIndexScan(table_name)      -- Disable index scan
```

### Join Method Hints

```sql
NestLoop(table1 table2)      -- Force nested loop join
HashJoin(table1 table2)      -- Force hash join
MergeJoin(table1 table2)     -- Force merge join
```

## Usage Examples

### Example 1: Create an Outline for a Query

```sql
-- Create an outline that forces sequential scan on 'orders' table
SELECT pg_create_outline(
    'outline_orders_seq',
    'SELECT * FROM orders WHERE status = $1',
    'SeqScan(orders)'
);
```

### Example 2: Create an Outline with Multiple Hints

```sql
-- Force specific scan and join methods
SELECT pg_create_outline(
    'outline_complex_query',
    'SELECT * FROM customers c JOIN orders o ON c.id = o.customer_id WHERE c.region = $1',
    E'IndexScan(customers idx_customer_region)\nHashJoin(customers orders)'
);
```

### Example 3: Manage Outlines

```sql
-- Disable an outline temporarily
SELECT pg_disable_outline('outline_orders_seq');

-- Re-enable it later
SELECT pg_enable_outline('outline_orders_seq');

-- Drop an outline permanently
SELECT pg_drop_outline('outline_orders_seq');
```

### Example 4: View Existing Outlines

```sql
-- Query the pg_outline catalog
SELECT outlinename, outlineenabled, outlinehints
FROM pg_outline
WHERE outlinenamespace = (SELECT oid FROM pg_namespace WHERE nspname = 'public');
```

## Implementation Details

### System Caches

The implementation adds two syscache entries for fast outline lookups:
- `OUTLINENAMENSP` - Lookup by (name, namespace)
- `OUTLINEOID` - Lookup by OID

### Optimizer Integration

The outline system integrates with PostgreSQL's optimizer through hooks:

1. **set_rel_pathlist_hook**: Applied when generating paths for base relations
   - Filters scan paths based on scan method hints
   - Removes undesired scan types

2. **set_join_pathlist_hook**: Applied when generating join paths
   - Filters join paths based on join method hints
   - Enforces specific join algorithms

### Hint Matching

Currently, hints are matched by:
- **Relation name**: Must match the table name in the query
- **Hint type**: Scan or join method specification

## Limitations and Future Work

### Current Limitations

1. **Query Normalization**: The current implementation stores raw query text. A future enhancement would implement proper query normalization/fingerprinting for matching similar queries with different literal values.

2. **Join Hint Matching**: Join hints currently have limited matching logic. Enhancing this to track relation names through complex join trees is planned.

3. **Leading Hints**: The `LEADING` hint type for controlling join order is defined but not yet implemented.

4. **Subquery Support**: Hints for subqueries are not yet supported.

5. **Automatic Outline Creation**: Currently requires manual outline creation. A future feature could automatically capture plans from `EXPLAIN` output.

### Future Enhancements

1. **Query Fingerprinting**: Implement a query normalization algorithm similar to `pg_stat_statements` to match queries regardless of literal values.

2. **Automatic Capture**: Add a function like `pg_capture_outline(query_text)` that automatically executes a query and captures its plan as hints.

3. **Outline Import/Export**: Add functions to export outlines to SQL scripts for easy migration between environments.

4. **Statistics**: Add counters to track how often each outline is applied and its effect on query performance.

5. **Plan Comparison**: Add utilities to compare the plan with and without an outline to verify effectiveness.

## Files Modified/Created

### New Files Created
- `src/include/catalog/pg_outline.h` - Catalog definition
- `src/include/catalog/pg_outline.dat` - Catalog data
- `src/include/optimizer/outline_hints.h` - Hint structures and API
- `src/backend/optimizer/outline/Makefile` - Build configuration
- `src/backend/optimizer/outline/outline_hints.c` - Hint parsing
- `src/backend/optimizer/outline/outline_plan.c` - Plan-to-hint conversion
- `src/backend/optimizer/outline/outline_apply.c` - Hint application
- `src/backend/utils/adt/pg_outline_funcs.c` - SQL functions

### Modified Files
- `src/backend/optimizer/Makefile` - Added outline subdirectory
- `src/backend/utils/adt/Makefile` - Added pg_outline_funcs.o
- `src/backend/utils/cache/syscache.c` - Added outline syscaches
- `src/include/utils/syscache.h` - Added OUTLINENAMENSP and OUTLINEOID
- `src/include/catalog/pg_proc.dat` - Registered SQL functions
- `src/backend/catalog/Makefile` - Added pg_outline to catalog build

## References

- **OceanBase Outline**: https://en.oceanbase.com/docs/common-oceanbase-database-10000000000872110
- **pg_hint_plan**: https://pg-hint-plan.readthedocs.io/
- **PostgreSQL Optimizer Hooks**: `src/include/optimizer/paths.h`

## License

This implementation is part of PostgreSQL and follows the PostgreSQL License.
