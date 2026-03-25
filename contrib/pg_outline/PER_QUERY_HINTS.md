# Per-Query Hint Storage Design

## Overview

The pg_outline extension has been redesigned to store hints separately for each Query structure in a SQL statement, as requested. This document explains the implementation.

## Key Changes

### 1. Query Structure Modification

Added a new field to the `Query` structure in `src/include/nodes/parsenodes.h`:

```c
typedef struct Query
{
    // ... existing fields ...

    /* Outline hint support: hints extracted from inline comments for this Query */
    char       *query_hints;    /* hint string for this specific Query, or NULL */
} Query;
```

This field stores the hint text that applies to each individual Query structure.

### 2. Database Schema Changes

Added a new table `pg_outline_query_hints` to store per-Query hints separately:

```sql
CREATE TABLE pg_outline_query_hints (
    hint_id SERIAL PRIMARY KEY,
    outline_id INTEGER NOT NULL REFERENCES pg_outline_data(outline_id) ON DELETE CASCADE,
    query_index INTEGER NOT NULL,  -- Index of Query in the parse tree (0=top-level)
    hint_string TEXT NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(outline_id, query_index)
);
```

**Key Points:**
- One SQL statement → One outline in `pg_outline_data`
- That outline can have multiple Query structures → Multiple rows in `pg_outline_query_hints`
- Each Query has a unique index (0 for top-level, 1+ for subqueries/CTEs)

### 3. Hint Extraction with Position Tracking

**Old Approach (Incorrect):**
```c
// Merged all hints into a single string
char *extract_inline_hints(const char *query_string);
```

**New Approach (Correct):**
```c
// Extracts hints with their source positions
List *extract_inline_hints_with_positions(const char *query_string);

// Returns a list of QueryHintPosition structures:
typedef struct QueryHintPosition
{
    int hint_location;   // Where the hint starts in source
    int hint_end;        // Where the hint ends
    char *hint_text;     // The actual hint text
} QueryHintPosition;
```

### 4. Hint-to-Query Association

The function `assign_hints_to_queries()` recursively walks the Query tree and assigns each hint to the corresponding Query based on source location:

```c
static void assign_hints_to_queries(Query *query, List *hint_positions, const char *source_text);
```

**How it works:**
1. Each Query structure has a `stmt_location` field indicating where it appears in the source
2. Each hint has a `hint_location` and `hint_end` indicating its position
3. A hint is assigned to a Query if it appears just before that Query's location (within 100 characters)

### 5. Storage Workflow

**Old Workflow:**
```
SQL with hints → Extract all hints → Merge into one string → Store in hint_string column
```

**New Workflow:**
```
SQL with hints
  ↓
Extract hints with positions
  ↓
Parse SQL → Query tree
  ↓
Assign each hint to its Query (based on location)
  ↓
Walk Query tree collecting (query_index, hint) pairs
  ↓
Store in pg_outline_query_hints (one row per Query)
```

### 6. Example

```sql
-- Input SQL with multiple hints
SELECT /*+ HashJoin(t1 t2) */ t1.id, t2.name
FROM t1
JOIN (
    SELECT /*+ IndexScan(t3) */ id, name FROM t3
) AS t2 ON t1.id = t2.id
WHERE t1.id IN (
    SELECT /*+ SeqScan(t4) */ t4_id FROM t4
);
```

**Results in:**
- Query #0 (top-level): hint = "HashJoin(t1 t2)"
- Query #1 (first subquery in FROM): hint = "IndexScan(t3)"
- Query #2 (SubLink in WHERE): hint = "SeqScan(t4)"

**Stored as:**
```
pg_outline_data:
  outline_id=1, outline_name='my_outline', query_pattern='SELECT ...', hint_string=''

pg_outline_query_hints:
  (outline_id=1, query_index=0, hint_string='HashJoin(t1 t2)')
  (outline_id=1, query_index=1, hint_string='IndexScan(t3)')
  (outline_id=1, query_index=2, hint_string='SeqScan(t4)')
```

### 7. SubLink Support

**SubLinks** are subqueries that appear in expressions rather than in FROM clauses. The implementation now fully supports hints in SubLinks:

#### Types of SubLinks Supported:

1. **WHERE Clause Subqueries**:
   ```sql
   SELECT * FROM t1
   WHERE id IN (SELECT /*+ IndexScan(t2) */ id FROM t2);

   SELECT * FROM t1
   WHERE EXISTS (SELECT /*+ HashJoin(t3 t4) */ 1 FROM t3, t4 WHERE ...);

   SELECT * FROM t1
   WHERE value > ANY (SELECT /*+ SeqScan(t5) */ value FROM t5);
   ```

2. **SELECT List Scalar Subqueries**:
   ```sql
   SELECT id,
          (SELECT /*+ IndexScan(t2) */ max(value) FROM t2 WHERE t2.id = t1.id) AS max_val
   FROM t1;
   ```

3. **HAVING Clause Subqueries**:
   ```sql
   SELECT dept, COUNT(*)
   FROM employees
   GROUP BY dept
   HAVING COUNT(*) > (SELECT /*+ SeqScan(departments) */ avg_size FROM departments);
   ```

4. **Expression Subqueries**:
   ```sql
   SELECT * FROM t1
   ORDER BY (SELECT /*+ IndexOnlyScan(t2) */ rank FROM t2 WHERE t2.id = t1.id);
   ```

#### Implementation Details:

The implementation uses expression tree walkers to traverse all expression nodes in a Query:

```c
// Walker function that processes SubLink nodes
static bool assign_hints_sublink_walker(Node *node, SubLinkWalkerContext *context);

// Process all expression fields that might contain SubLinks
static void process_query_sublinks(Query *query, List *hint_positions, const char *source_text);
```

The walker examines:
- `targetList` (SELECT clause)
- `jointree->quals` (WHERE clause)
- `havingQual` (HAVING clause)
- `limitOffset` and `limitCount` (LIMIT/OFFSET clauses)

When a SubLink is found, if its `subselect` field contains a Query node, that Query is processed recursively with `assign_hints_to_queries()`.

#### Query Index Assignment:

Query indices are assigned in depth-first order:
1. Top-level Query gets index 0
2. Process RTE subqueries (FROM clause)
3. Process CTEs (WITH clause)
4. Process SubLinks in expressions (WHERE, SELECT, HAVING, etc.)

This ensures consistent indexing regardless of where subqueries appear in the SQL.

## Implementation Functions

### Hint Extraction
- `extract_inline_hints_with_positions()`: Extract all `/*+ ... */` comments with their locations
- `strip_hints_from_query()`: Remove hints from SQL for normalization

### Hint Assignment
- `find_hint_for_query_location()`: Match a hint to a Query based on location
- `assign_hints_to_queries()`: Recursively assign hints to all Query structures

### Storage
- `collect_query_hints_recursive()`: Walk Query tree and collect all hints with their indices
- `store_outline_with_query_hints()`: Store outline and per-Query hints in separate tables

### SubLink Processing
- `assign_hints_sublink_walker()`: Walker function to process SubLink nodes in expressions
- `process_query_sublinks()`: Walk expression trees looking for SubLinks
- `collect_hints_sublink_walker()`: Walker for collecting hints from SubLinks
- `process_query_sublinks_collection()`: Collect hints from SubLinks during collection phase

## Benefits

1. **Correct Semantics**: Each Query structure maintains its own hints, matching how PostgreSQL's optimizer processes queries
2. **Multiple Subqueries**: Can have different hints for each subquery, CTE, or nested SELECT
3. **Complete Coverage**: Supports hints in all subquery locations (FROM clause, WHERE clause, SELECT list, HAVING, etc.)
4. **SubLink Support**: Full support for SubLinks in expressions (IN, EXISTS, ANY, scalar subqueries)
5. **Future-Proof**: Can extend to store more metadata per Query (e.g., plan costs, row estimates)
6. **Clear Separation**: The hint for the main query doesn't interfere with subquery hints

## PostgreSQL Query Rewriting

Note that PostgreSQL's optimizer may rewrite certain SubLinks:

- **IN/EXISTS subqueries** may be converted to semi-joins by `pull_up_sublinks()`
- After conversion, these become RTE_SUBQUERY entries (already supported)
- Unconverted SubLinks (e.g., correlated subqueries) are handled by the SubLink walker

This means the implementation handles both:
1. Subqueries that remain as SubLinks
2. Subqueries that get converted to joins (and become RTEs)

## Next Steps

1. **Hint Application**: Modify the planner hook to read `Query.query_hints` and apply them during planning
2. **Hint Retrieval**: Update the retrieval logic to load per-Query hints from `pg_outline_query_hints` and restore them to the Query tree
3. **Testing**: Comprehensive testing with complex queries containing:
   - Multiple SubLinks in WHERE clauses
   - Scalar subqueries in SELECT lists
   - Subqueries in HAVING clauses
   - Nested subqueries (SubLinks within SubLinks)
   - Mixed RTE_SUBQUERY and SubLink subqueries

## Compatibility Note

The old `hint_string` column in `pg_outline_data` is kept empty for outlines created with the new method. This preserves backward compatibility with the `pg_outline_create()` function which still uses the single-string approach.
