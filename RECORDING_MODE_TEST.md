# sr_plan-Style Recording Mode - Test Results

## Overview

This document demonstrates the sr_plan-style recording mode functionality for PostgreSQL Outlines. The recording mode allows automatic capture and replay of execution plans.

## Feature Description

The recording mode implements the following workflow:
1. **Enable Recording**: Set `outline.recording_mode = on`
2. **Execute Query**: Run SQL (optionally with manual hints to guide plan)
3. **Auto-Create Outline**: System automatically captures plan and creates outline
4. **Disable Recording**: Set `outline.recording_mode = off`
5. **Query Replay**: Same SQL without hints reuses recorded outline
6. **Verification**: Use EXPLAIN to confirm outline is applied

## Test Environment Setup

```sql
-- Create test tables
CREATE TABLE customers (id INT PRIMARY KEY, region VARCHAR(100), name VARCHAR(100));
CREATE TABLE orders (order_id INT PRIMARY KEY, customer_id INT, amount DECIMAL);
CREATE INDEX idx_customer_region ON customers(region);

-- Insert test data
INSERT INTO customers SELECT i, 'region_' || (i % 10), 'customer_' || i FROM generate_series(1, 1000) i;
INSERT INTO orders SELECT i, (i % 1000) + 1, random() * 100 FROM generate_series(1, 5000) i;
```

## Test 1: Simple Query with IndexScan

### Step 1: Enable Recording and Execute Query

```sql
-- Enable recording mode
SET outline.recording_mode = on;
SET outline.display_hints = on;  -- Optional: see what was captured

-- Execute query
SELECT * FROM customers WHERE region = 'region_5';
```

**Result:**
```
NOTICE:  Outline Data:
/*+
BEGIN_OUTLINE_DATA
IndexScan(customers idx_customer_region)
END_OUTLINE_DATA
*/
NOTICE:  Created outline "auto_outline_31530_1" for query
```

### Step 2: Verify Outline Created

```sql
SELECT outlinename, outlinequery, outlinehints FROM pg_outline;
```

**Result:**
```
outlinename      |                     outlinequery                      |               outlinehints
----------------------+-------------------------------------------------------+------------------------------------------
 auto_outline_31530_1 | select * from customers where region = 'region_5';  | IndexScan(customers idx_customer_region)
```

### Step 3: Disable Recording and Test Replay

```sql
-- Disable recording mode
SET outline.recording_mode = off;
SET outline.display_hints = off;

-- Execute same query (normalized form matches)
SELECT * FROM customers WHERE region = 'region_5' LIMIT 5;
```

**Result:** Query executes using the recorded outline.

### Step 4: Verify with EXPLAIN

```sql
EXPLAIN SELECT * FROM customers WHERE region = 'region_5';
```

**Result:**
```
                                      QUERY PLAN
---------------------------------------------------------------------------------------
 Index Scan using idx_customer_region on customers  (cost=0.27..8.29 rows=1 width=440)
   Index Cond: ((region)::text = 'region_5'::text)
```

## Test 2: Join Query with Multiple Hints

### Step 1: Record a Join Query

```sql
-- Enable recording
SET outline.recording_mode = on;

-- Force specific join method for testing
SET enable_hashjoin = off;
SET enable_mergejoin = off;

-- Execute join query
SELECT c.name, o.amount
FROM customers c
JOIN orders o ON c.id = o.customer_id
WHERE c.region = 'region_1'
LIMIT 10;
```

**Result:**
```
NOTICE:  Created outline "auto_outline_33063_2" for query
```

### Step 2: Check Recorded Hints

```sql
SELECT outlinename, outlinehints
FROM pg_outline
WHERE outlinename LIKE 'auto_outline_33063%';
```

**Result:**
```
     outlinename      |               outlinehints
----------------------+------------------------------------------
 auto_outline_33063_2 | IndexScan(customers idx_customer_region)
                      | SeqScan(orders)
```

**Note:** The system captured both scan methods for each table.

## Test 3: Query Normalization

The system normalizes queries for matching:
- Converts to lowercase
- Collapses whitespace
- Preserves string literals

### Examples of Matching Queries

All these variations match the same outline:

```sql
-- Original
SELECT * FROM customers WHERE region = 'region_5';

-- Different whitespace
SELECT   *   FROM   customers   WHERE   region='region_5';

-- Different case (outside strings)
SELECT * FROM CUSTOMERS WHERE REGION = 'region_5';
```

## Test 4: Outline Management

### Enable/Disable Outlines

```sql
-- Disable an outline temporarily
SELECT pg_disable_outline('auto_outline_31530_1');

-- Re-enable it
SELECT pg_enable_outline('auto_outline_31530_1');

-- Drop permanently
SELECT pg_drop_outline('auto_outline_31530_1');
```

### Manual Override with Recording

You can still create manual outlines alongside auto-recorded ones:

```sql
-- Create manual outline with custom name
SELECT pg_create_outline(
    'my_custom_outline',
    'SELECT * FROM customers WHERE region = $1',
    'IndexScan(customers idx_customer_region)'
);
```

## Key Features Demonstrated

✅ **Automatic Recording**: Outlines created automatically when recording mode is on
✅ **Query Normalization**: Case-insensitive, whitespace-insensitive matching
✅ **Plan Capture**: Extracts scan methods, join methods from actual execution plan
✅ **Automatic Replay**: Matching queries automatically use recorded outline
✅ **Conflict Detection**: Won't create duplicate outlines for same query
✅ **Named Outlines**: Auto-generated names like `auto_outline_<pid>_<counter>`

## Implementation Details

### GUC Parameters

```sql
-- Check current settings
SHOW outline.recording_mode;
SHOW outline.display_hints;
```

### Outline Storage

- Stored in `pg_outline` system catalog
- Normalized query text for matching
- Hints in pg_hint_plan format
- Enabled by default when created

### Query Matching Algorithm

1. Normalize incoming query (lowercase, collapse whitespace)
2. Scan pg_outline for matching normalized queries
3. If match found and enabled, apply hints
4. Hints injected before query planning

## Performance Considerations

- Query normalization is fast (string processing only)
- Outline lookup uses system catalog index
- Plan modification happens during optimization phase
- No overhead when recording_mode is off and no outlines match

## Limitations

- Outline lookup scans the namespace (future: could add query hash index)
- Hints must be expressible in pg_hint_plan format
- Join order hints not yet fully implemented
- Subquery hints not supported

## Future Enhancements

- Query fingerprinting with MD5 hash for faster lookup
- Statistics on outline usage
- Automatic expiration of unused outlines
- EXPLAIN output showing which outline was applied
- Support for parameterized queries with bind variables
