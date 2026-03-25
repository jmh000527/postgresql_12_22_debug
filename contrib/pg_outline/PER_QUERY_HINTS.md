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
- Query #1 (first subquery): hint = "IndexScan(t3)"
- Query #2 (second subquery): hint = "SeqScan(t4)"

**Stored as:**
```
pg_outline_data:
  outline_id=1, outline_name='my_outline', query_pattern='SELECT ...', hint_string=''

pg_outline_query_hints:
  (outline_id=1, query_index=0, hint_string='HashJoin(t1 t2)')
  (outline_id=1, query_index=1, hint_string='IndexScan(t3)')
  (outline_id=1, query_index=2, hint_string='SeqScan(t4)')
```

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

## Benefits

1. **Correct Semantics**: Each Query structure maintains its own hints, matching how PostgreSQL's optimizer processes queries
2. **Multiple Subqueries**: Can have different hints for each subquery, CTE, or nested SELECT
3. **Future-Proof**: Can extend to store more metadata per Query (e.g., plan costs, row estimates)
4. **Clear Separation**: The hint for the main query doesn't interfere with subquery hints

## Next Steps

1. **Hint Application**: Modify the planner hook to read `Query.query_hints` and apply them during planning
2. **Hint Retrieval**: Update the retrieval logic to load per-Query hints from `pg_outline_query_hints` and restore them to the Query tree
3. **Testing**: Comprehensive testing with complex queries containing multiple subqueries and CTEs

## Compatibility Note

The old `hint_string` column in `pg_outline_data` is kept empty for outlines created with the new method. This preserves backward compatibility with the `pg_outline_create()` function which still uses the single-string approach.
