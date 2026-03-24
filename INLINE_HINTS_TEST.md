# Inline Hints Test Documentation

## Overview

This document provides comprehensive test cases for the inline hints feature, which allows hints to be specified directly in SQL queries using `/*+ ... */` comments.

## Background

The inline hints feature addresses the requirement that "Hints are not necessarily all specified after the SELECT statement. A SQL can have multiple SELECT keywords, and hints can be specified after each of these keywords."

### Key Features

1. **Inline hint syntax**: `/*+ hint1 hint2 ... */`
2. **OceanBase-compatible format**: `/*+ BEGIN_OUTLINE_DATA ... END_OUTLINE_DATA */`
3. **Multiple hints per query**: Hints from all inline comments are merged
4. **Priority**: Inline hints take precedence over stored outlines in `pg_outline`

## Test Setup

```sql
-- Create test database
CREATE DATABASE inline_hints_test;
\c inline_hints_test

-- Create test tables
CREATE TABLE customers (
    id INT PRIMARY KEY,
    region VARCHAR(100),
    name VARCHAR(100)
);

CREATE TABLE orders (
    order_id INT PRIMARY KEY,
    customer_id INT,
    amount DECIMAL
);

-- Create indexes
CREATE INDEX idx_customer_region ON customers(region);
CREATE INDEX idx_order_customer ON orders(customer_id);

-- Insert test data
INSERT INTO customers
SELECT i, 'region_' || (i % 10), 'customer_' || i
FROM generate_series(1, 1000) i;

INSERT INTO orders
SELECT i, (i % 1000) + 1, random() * 100
FROM generate_series(1, 5000) i;

-- Analyze tables for better statistics
ANALYZE customers;
ANALYZE orders;
```

## Test Cases

### Test 1: Basic Inline Hint - Single SELECT

Test that a simple inline hint works with a single SELECT statement.

```sql
-- Query with inline SeqScan hint
EXPLAIN /*+ SeqScan(customers) */
SELECT * FROM customers WHERE region = 'region_5';
```

**Expected Result**: The plan should show a Sequential Scan on customers.

```
                                QUERY PLAN
--------------------------------------------------------------------------
 Seq Scan on customers  (cost=0.00..20.00 rows=100 width=440)
   Filter: ((region)::text = 'region_5'::text)
```

### Test 2: Inline Hint with IndexScan

Test that IndexScan hints work inline.

```sql
-- Query with inline IndexScan hint
EXPLAIN /*+ IndexScan(customers idx_customer_region) */
SELECT * FROM customers WHERE region = 'region_5';
```

**Expected Result**: The plan should show an Index Scan using idx_customer_region.

```
                                         QUERY PLAN
--------------------------------------------------------------------------------------------
 Index Scan using idx_customer_region on customers  (cost=0.27..8.29 rows=100 width=440)
   Index Cond: ((region)::text = 'region_5'::text)
```

### Test 3: Multiple Hints in Same Comment

Test that multiple hints in a single comment are all applied.

```sql
-- Query with multiple hints in one comment
EXPLAIN /*+ SeqScan(customers) SeqScan(orders) */
SELECT c.name, o.amount
FROM customers c
JOIN orders o ON c.id = o.customer_id
WHERE c.region = 'region_1';
```

**Expected Result**: Both tables should use Sequential Scan.

### Test 4: Hints in Subquery

Test that hints in subqueries are properly extracted and applied.

```sql
-- Query with hint in subquery
EXPLAIN SELECT * FROM customers c
WHERE c.id IN (
    /*+ SeqScan(orders) */
    SELECT customer_id FROM orders WHERE amount > 50
);
```

**Expected Result**: The subquery should use Sequential Scan for the orders table.

### Test 5: Hints in CTE (Common Table Expression)

Test that hints in CTEs are properly handled.

```sql
-- Query with hint in CTE
EXPLAIN WITH high_value_orders AS (
    /*+ SeqScan(orders) */
    SELECT customer_id, SUM(amount) as total
    FROM orders
    WHERE amount > 50
    GROUP BY customer_id
)
SELECT c.name, h.total
FROM customers c
JOIN high_value_orders h ON c.id = h.customer_id;
```

**Expected Result**: The orders table in the CTE should use Sequential Scan.

### Test 6: Multiple SELECTs with Different Hints

Test the core requirement: multiple SELECT statements with different hints.

```sql
-- Main query with IndexScan, subquery with SeqScan
EXPLAIN /*+ IndexScan(customers idx_customer_region) */
SELECT * FROM customers c
WHERE c.region = 'region_5'
  AND EXISTS (
      /*+ SeqScan(orders) */
      SELECT 1 FROM orders o
      WHERE o.customer_id = c.id
        AND o.amount > 50
  );
```

**Expected Result**:
- Main query should use Index Scan on customers
- Subquery should use Sequential Scan on orders

### Test 7: OceanBase-Style Inline Hints

Test the OceanBase-compatible format.

```sql
-- Query with OceanBase-style inline hints
EXPLAIN /*+
BEGIN_OUTLINE_DATA
IndexScan(customers idx_customer_region)
HashJoin(customers orders)
END_OUTLINE_DATA
*/
SELECT * FROM customers c
JOIN orders o ON c.id = o.customer_id
WHERE c.region = 'region_3';
```

**Expected Result**:
- Index Scan on customers using idx_customer_region
- Hash Join between customers and orders

### Test 8: Inline Hints Take Priority Over Stored Outlines

Test that inline hints have higher priority than stored outlines.

```sql
-- First create a stored outline that forces SeqScan
SELECT pg_create_outline(
    'test_stored_outline',
    'SELECT * FROM customers WHERE region = $1',
    'SeqScan(customers)'
);

-- Verify the stored outline works
EXPLAIN SELECT * FROM customers WHERE region = 'region_5';
-- Should show SeqScan

-- Now use inline hint to override with IndexScan
EXPLAIN /*+ IndexScan(customers idx_customer_region) */
SELECT * FROM customers WHERE region = 'region_5';
-- Should show IndexScan (inline hint takes precedence)

-- Clean up
SELECT pg_drop_outline('test_stored_outline');
```

**Expected Result**: The inline hint should override the stored outline.

### Test 9: Join Method Hints

Test inline hints for join methods.

```sql
-- Force NestLoop join
EXPLAIN /*+ NestLoop(customers orders) */
SELECT * FROM customers c
JOIN orders o ON c.id = o.customer_id
WHERE c.region = 'region_2';
```

**Expected Result**: Plan should show Nested Loop join.

```sql
-- Force HashJoin
EXPLAIN /*+ HashJoin(customers orders) */
SELECT * FROM customers c
JOIN orders o ON c.id = o.customer_id
WHERE c.region = 'region_2';
```

**Expected Result**: Plan should show Hash Join.

### Test 10: Negative Hints (NoSeqScan)

Test that negative hints work inline.

```sql
-- Disable SeqScan to force index usage
EXPLAIN /*+ NoSeqScan(customers) */
SELECT * FROM customers WHERE id > 500;
```

**Expected Result**: Should use Index Scan instead of Sequential Scan.

### Test 11: Complex Query with Multiple Inline Hints

Test a complex query with hints at multiple levels.

```sql
EXPLAIN /*+ IndexScan(customers idx_customer_region) */
WITH regional_customers AS (
    SELECT id, name FROM customers
    WHERE region = 'region_5'
),
high_orders AS (
    /*+ SeqScan(orders) */
    SELECT customer_id, amount FROM orders
    WHERE amount > 75
)
SELECT rc.name, ho.amount
FROM regional_customers rc
/*+ HashJoin(rc ho) */
JOIN high_orders ho ON rc.id = ho.customer_id;
```

**Expected Result**:
- Main customers query: Index Scan
- Orders in CTE: Sequential Scan
- Final join: Hash Join

### Test 12: Verify Inline Hints Don't Break Normal Queries

Test that queries without hints continue to work normally.

```sql
-- Query without any hints should work as before
EXPLAIN SELECT * FROM customers WHERE region = 'region_5';

-- Query with regular comment (not a hint) should be ignored
EXPLAIN /* This is a regular comment */
SELECT * FROM customers WHERE region = 'region_5';

-- Query with malformed hint should be ignored gracefully
EXPLAIN /*+ InvalidHintName(customers) */
SELECT * FROM customers WHERE region = 'region_5';
```

**Expected Result**: All queries should execute without error, using default planning.

## Debug Testing

Enable debug logging to see when inline hints are applied:

```sql
-- Enable debug logging
SET client_min_messages = DEBUG1;

-- Run a query with inline hints
/*+ SeqScan(customers) */
SELECT * FROM customers WHERE region = 'region_5' LIMIT 5;
```

**Expected Output**: Should see debug messages like:
```
DEBUG:  Extracted inline hints: SeqScan(customers)
DEBUG:  Applying inline hints from query
```

## Performance Comparison

Compare performance with and without hints:

```sql
-- Without hint (might use SeqScan for small result set)
EXPLAIN ANALYZE SELECT * FROM customers WHERE region = 'region_5';

-- With IndexScan hint
EXPLAIN ANALYZE /*+ IndexScan(customers idx_customer_region) */
SELECT * FROM customers WHERE region = 'region_5';
```

Compare the execution times and costs.

## Cleanup

```sql
-- Drop test tables
DROP TABLE IF EXISTS orders;
DROP TABLE IF EXISTS customers;

-- Drop test database (run from another database)
-- \c postgres
-- DROP DATABASE inline_hints_test;
```

## Conclusion

The inline hints feature successfully addresses the requirement for hints to be specified at multiple locations in SQL queries with multiple SELECT keywords. It supports:

1. ✅ Hints after each SELECT keyword
2. ✅ Hints in subqueries
3. ✅ Hints in CTEs
4. ✅ Multiple hint comments per query
5. ✅ OceanBase-compatible format
6. ✅ Priority over stored outlines
7. ✅ All existing hint types (scan methods, join methods, negative hints)
