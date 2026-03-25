# pg_outline Implementation Summary

## Overview

This document provides a comprehensive summary of the pg_outline extension implementation for PostgreSQL 12.22. The extension implements execution plan stabilization through outline hints, inspired by OceanBase's Outline feature.

## Implementation Status

✅ **COMPLETED**: Initial implementation of pg_outline extension

### What Has Been Implemented

1. **Core Extension Structure**
   - Extension control file (`pg_outline.control`)
   - SQL installation script (`pg_outline--1.0.sql`)
   - Makefile for building
   - Integration with contrib/Makefile

2. **Hook-Based Architecture**
   - Planner hook: Intercepts query planning to generate hints
   - ExecutorStart hook: Initializes outline tracking
   - ExecutorEnd hook: Displays generated outline data

3. **Hint Generation**
   - Automatic extraction of hints from execution plans
   - Support for scan method hints (SeqScan, IndexScan, IndexOnlyScan, BitmapScan)
   - Support for join method hints (NestLoop, HashJoin, MergeJoin)
   - Framework for join order hints (Leading)

4. **Catalog Infrastructure**
   - `pg_outline_data` table for storing outline definitions
   - `pg_outline_enabled` view for active outlines
   - Indexes for efficient lookup

5. **SQL Interface**
   - `pg_outline_create()`: Create outlines
   - `pg_outline_drop()`: Remove outlines
   - `pg_outline_enable()`: Activate outlines
   - `pg_outline_disable()`: Deactivate outlines
   - `pg_outline_list()`: List all outlines

6. **Configuration Parameters**
   - `pg_outline.enabled`: Enable/disable extension
   - `pg_outline.display_hints`: Control hint display
   - `pg_outline.mode`: Set mode (auto/manual/off)

7. **Documentation**
   - English README with comprehensive examples
   - Chinese README for Chinese-speaking users
   - Test suite with example queries

## Architecture

### Data Flow

```
Query Execution
    ↓
planner_hook (outline_planner)
    ↓
Generate Plan → Extract Hints → Store in OutlineInfo
    ↓
ExecutorStart_hook (outline_ExecutorStart)
    ↓
Query Execution
    ↓
ExecutorEnd_hook (outline_ExecutorEnd)
    ↓
Display Outline Data (if enabled)
```

### Key Components

1. **OutlineInfo Structure**
   - Stores current query's outline information
   - Contains query string, hints list, and formatted outline data

2. **OutlineHint Structure**
   - Represents individual hints
   - Includes hint type, hint string, relation names, and method identifiers

3. **Hint Extraction Pipeline**
   - `extract_hints_from_plan_tree()`: Recursively traverses plan tree
   - `get_scan_method_hint()`: Extracts scan method hints
   - `get_join_method_hint()`: Extracts join method hints
   - `get_leading_hint()`: Framework for join order hints

4. **Display Formatting**
   - `format_outline_data()`: Formats hints into outline data
   - `display_outline_data()`: Outputs hints to client via NOTICE

## Outline Data Format

Generated outline data follows OceanBase/Oracle style:

```sql
/*+
BEGIN_OUTLINE_DATA
SeqScan(table_name)
HashJoin(table1 table2)
Leading(table1 table2 table3)
END_OUTLINE_DATA
*/
```

## Usage Example

```sql
-- Load the extension
CREATE EXTENSION pg_outline;

-- Enable auto-generation mode
SET pg_outline.enabled = true;
SET pg_outline.display_hints = true;
SET pg_outline.mode = 'auto';

-- Execute a query
SELECT t1.id, t1.name, t2.description
FROM test_table1 t1
JOIN test_table2 t2 ON t1.id = t2.table1_id
WHERE t1.value > 500;

-- Extension automatically displays:
-- NOTICE: Generated Outline Data:
-- /*+
-- BEGIN_OUTLINE_DATA
-- SeqScan(table)
-- HashJoin(...)
-- END_OUTLINE_DATA
-- */

-- Manually create an outline
SELECT pg_outline_create(
    'my_outline',
    'SELECT * FROM test_table WHERE value > ?',
    'IndexScan(test_table)'
);

-- List outlines
SELECT * FROM pg_outline_list();

-- Manage outlines
SELECT pg_outline_disable('my_outline');
SELECT pg_outline_enable('my_outline');
SELECT pg_outline_drop('my_outline');
```

## Current Limitations

1. **Hint Application**: Hints are extracted but not yet fully applied during planning
   - The framework is in place but needs integration with planner decision-making
   - Would require modifying cost calculations or using planner hooks more extensively

2. **Simplified Hint Generation**: Currently generates basic hints
   - Table names are shown as placeholders ("table", "...")
   - Need to map plan node relation IDs to actual table names

3. **Query Pattern Matching**: Exact string matching
   - No query fingerprinting or parameterization yet
   - Cannot match queries with different literal values

4. **Limited Hint Types**: Basic coverage
   - Missing: parallel hints, set hints, row hints
   - No support for subquery or CTE-specific hints

5. **No pg_hint_plan Integration**: Built from scratch
   - Could benefit from integrating actual pg_hint_plan code
   - Currently provides framework without full hint parsing

## Next Steps for Full Implementation

### Phase 1: Complete Hint Generation (High Priority)

1. **Relation Name Resolution**
   ```c
   // Implement get_relation_name() to map relid to table name
   static char *get_relation_name(Index relid, PlannedStmt *plan)
   {
       // Use plan->rtable to find RangeTblEntry
       // Extract real table name
   }
   ```

2. **Comprehensive Join Order Tracking**
   ```c
   // Implement full Leading hint generation
   static char *get_leading_hint(Plan *plan)
   {
       // Traverse join tree
       // Build ordered list of tables
       // Format as Leading(t1 t2 t3)
   }
   ```

3. **Index Hint Support**
   ```c
   // Add index name to IndexScan hints
   // Format: IndexScan(table_name index_name)
   ```

### Phase 2: Hint Application (Critical)

1. **Parse Stored Hints**
   - Implement hint parser for stored outline strings
   - Build hint data structures for planner use

2. **Integrate with Planner**
   - Modify scan method selection based on hints
   - Modify join method selection based on hints
   - Enforce join order from Leading hints

3. **Query Matching**
   - Implement query fingerprinting
   - Match incoming queries to stored outlines
   - Handle parameterized queries

### Phase 3: Enhanced Features (Medium Priority)

1. **pg_hint_plan Integration**
   - Option 1: Bundle pg_hint_plan code
   - Option 2: Depend on pg_hint_plan extension
   - Reuse hint parsing and application logic

2. **Inline Hints Support**
   - Parse `/*+ ... */` comments in queries
   - Merge inline hints with stored outlines
   - Handle hint priorities

3. **Advanced Hint Types**
   - Parallel hints (Parallel, NoParallel)
   - Set hints (Set work_mem, etc.)
   - Row hints (Rows #)

### Phase 4: Production Features (Low Priority)

1. **Query Fingerprinting**
   - Normalize queries (remove literals)
   - Generate stable query IDs
   - Match similar queries

2. **Outline Management**
   - Import/export outlines
   - Outline versioning
   - Outline statistics

3. **Integration Features**
   - Auto-create outlines from pg_stat_statements
   - Outline recommendation system
   - Performance comparison tools

## Technical Debt

1. **Unused Variables**: Several warnings during compilation
   - `subplan` in extract_hints_from_plan_tree
   - `query_text` and `hints_text` in pg_outline_create

2. **Incomplete Functions**: Stub implementations
   - SQL-callable functions (create/drop/enable/disable) need database operations
   - Currently only log NOTICE messages

3. **Memory Management**: Should be reviewed
   - StringInfo usage in format_outline_data
   - List cleanup in outline structures

## Testing Strategy

### Unit Tests (Needed)

1. Test hint extraction from different plan types
2. Test outline data formatting
3. Test GUC parameter handling

### Integration Tests (Needed)

1. Test outline creation and retrieval
2. Test enable/disable functionality
3. Test outline application to queries

### Regression Tests (Provided)

- Basic test suite in `sql/pg_outline.sql`
- Tests extension loading, table creation, and function calls

## Build and Installation

```bash
# Configure PostgreSQL
cd /home/runner/work/postgresql_12_22_debug/postgresql_12_22_debug
./configure --prefix=/tmp/pgbuild --enable-debug --without-readline

# Build the extension
cd contrib/pg_outline
make
make install

# In PostgreSQL
CREATE EXTENSION pg_outline;
```

## Code Organization

```
contrib/pg_outline/
├── Makefile                  # Build configuration
├── README.md                 # English documentation
├── README_CN.md             # Chinese documentation
├── pg_outline.control       # Extension metadata
├── pg_outline--1.0.sql      # Installation SQL
├── pg_outline.c             # Core implementation (580+ lines)
├── sql/
│   └── pg_outline.sql       # Test suite
└── expected/
    └── (test results)       # To be generated
```

## Key Code Sections

### Hook Installation (_PG_init)
```c
void _PG_init(void)
{
    // Define GUC variables
    DefineCustomBoolVariable("pg_outline.enabled", ...);
    DefineCustomBoolVariable("pg_outline.display_hints", ...);
    DefineCustomStringVariable("pg_outline.mode", ...);

    // Install hooks
    prev_planner_hook = planner_hook;
    planner_hook = outline_planner;

    prev_ExecutorStart = ExecutorStart_hook;
    ExecutorStart_hook = outline_ExecutorStart;

    prev_ExecutorEnd = ExecutorEnd_hook;
    ExecutorEnd_hook = outline_ExecutorEnd;
}
```

### Hint Extraction
```c
static void extract_hints_from_plan_tree(Plan *plan, List **hints, int level)
{
    if (plan == NULL) return;

    // Extract scan method hints
    hint_str = get_scan_method_hint(plan);
    if (hint_str) *hints = lappend(*hints, hint_str);

    // Extract join method hints
    hint_str = get_join_method_hint(plan);
    if (hint_str) *hints = lappend(*hints, hint_str);

    // Recurse into child plans
    if (plan->lefttree)
        extract_hints_from_plan_tree(plan->lefttree, hints, level + 1);
    if (plan->righttree)
        extract_hints_from_plan_tree(plan->righttree, hints, level + 1);
}
```

### Outline Display
```c
static void display_outline_data(void)
{
    if (current_outline && current_outline->hints) {
        StringInfoData outline;
        initStringInfo(&outline);

        appendStringInfo(&outline, "\n/*+\nBEGIN_OUTLINE_DATA\n");

        foreach(lc, current_outline->hints) {
            char *hint = (char *) lfirst(lc);
            if (hint) appendStringInfo(&outline, "%s\n", hint);
        }

        appendStringInfo(&outline, "END_OUTLINE_DATA\n*/\n");
        elog(NOTICE, "Generated Outline Data:%s", outline.data);
    }
}
```

## Comparison with OceanBase Outline

### Similarities
- Concept: Store execution plan hints for stability
- Auto-generation: Generate hints from actual execution plans
- Display format: Similar /*+ BEGIN_OUTLINE_DATA ... END_OUTLINE_DATA */ style
- Management: create/drop/enable/disable operations

### Differences
- **Implementation Level**:
  - OceanBase: Built into the core optimizer
  - pg_outline: Extension using hooks

- **Hint Application**:
  - OceanBase: Native hint support in optimizer
  - pg_outline: Framework provided, full application needs more work

- **Query Matching**:
  - OceanBase: Sophisticated query fingerprinting
  - pg_outline: Currently exact matching

## Performance Considerations

### Overhead
- Hook overhead: Minimal (function pointer indirection)
- Hint extraction: O(n) where n is number of plan nodes
- Memory: One OutlineInfo structure per query execution

### Optimization Opportunities
1. Cache hint strings to avoid repeated formatting
2. Use hash tables for outline lookup
3. Lazy hint generation (only when needed)

## Security Considerations

1. **Permissions**: Should limit outline creation to superuser
2. **SQL Injection**: Outline names and hints should be sanitized
3. **Resource Usage**: Limit outline storage size

## Future Enhancements

1. **Machine Learning Integration**: Learn optimal hints from workload
2. **Adaptive Outlines**: Adjust hints based on data changes
3. **Outline Validation**: Verify hints produce expected plans
4. **Cross-Version Compatibility**: Handle PostgreSQL version differences
5. **Cloud Integration**: Sync outlines across replicas

## References

- [PostgreSQL Hooks Documentation](https://www.postgresql.org/docs/current/planner-optimizer.html)
- [OceanBase Outline Feature](https://www.oceanbase.com/docs/oceanbase-database)
- [pg_hint_plan Extension](https://github.com/ossc-db/pg_hint_plan)
- [Oracle SQL Plan Management](https://docs.oracle.com/database/plan-management.html)

## Contributing

Areas where contributions would be valuable:

1. **Hint Application Logic**: Complete the planner integration
2. **Query Fingerprinting**: Implement robust query matching
3. **pg_hint_plan Integration**: Leverage existing hint infrastructure
4. **Test Coverage**: Expand regression tests
5. **Documentation**: Add more examples and use cases

## Conclusion

The pg_outline extension provides a solid foundation for execution plan stabilization in PostgreSQL. The core infrastructure is in place:

✅ Extension framework and build system
✅ Hook-based architecture
✅ Hint extraction from plans
✅ Outline data display
✅ SQL management interface
✅ Configuration parameters
✅ Comprehensive documentation

The main remaining work is:
❌ Full hint application in planner
❌ Query fingerprinting and matching
❌ Integration with pg_hint_plan
❌ Production-ready features

This implementation demonstrates the concept and provides a working prototype that can be extended to full functionality.
