# PostgreSQL Outline Feature

## Overview

This implementation adds an **Outline** feature to PostgreSQL, similar to OceanBase's Outline interface and inspired by pg_hint_plan. The Outline feature allows DBAs to fix and stabilize execution plans by storing optimizer hints for specific SQL queries.

## Features

The Outline system provides:

1. **Plan Stabilization**: Fix execution plans for critical queries to prevent performance regressions
2. **Hint-based Control**: Use pg_hint_plan-compatible hint syntax for fine-grained control
3. **Catalog Storage**: Store outlines persistently in the `pg_outline` system catalog
4. **Dynamic Management**: Enable/disable outlines without modifying application code

## Quick Start: Building, Installation, and Testing

This section provides complete step-by-step instructions for beginners to build PostgreSQL with the Outline feature from source, install it, and run comprehensive test cases.

### Prerequisites

Before starting, ensure you have the necessary build tools and dependencies installed:

**Ubuntu/Debian Systems:**
```bash
# Update package list
sudo apt-get update

# Install build dependencies
sudo apt-get install -y build-essential libreadline-dev zlib1g-dev flex bison libssl-dev
```

**CentOS/RHEL Systems:**
```bash
# Install build dependencies
sudo yum install -y gcc gcc-c++ readline-devel zlib-devel flex bison openssl-devel
```

**macOS Systems:**
```bash
# Install Xcode Command Line Tools
xcode-select --install

# Install dependencies via Homebrew
brew install readline openssl
```

### Step 1: Get the Source Code

Clone the repository from GitHub:

```bash
# Clone the repository
git clone https://github.com/jmh000527/postgresql_12_22_debug.git

# Enter the source directory
cd postgresql_12_22_debug
```

**Expected Output:**
```
Cloning into 'postgresql_12_22_debug'...
remote: Enumerating objects: xxxxx, done.
remote: Counting objects: 100% (xxxx/xxxx), done.
remote: Compressing objects: 100% (xxxx/xxxx), done.
Receiving objects: 100% (xxxxx/xxxxx), xx.xx MiB | xx.xx MiB/s, done.
```

### Step 2: Configure Build Options

Configure the build with debugging enabled:

```bash
# Configure with debug options
./configure --prefix=/usr/local/pgsql --enable-debug --enable-cassert

# Note: Adjust --prefix to your preferred installation directory
```

**Configuration Explanation:**
- `--prefix=/usr/local/pgsql`: Installation directory (modify as needed)
- `--enable-debug`: Enable debugging symbols for development
- `--enable-cassert`: Enable assertion checks

**Expected Output:**
```
checking build system type... x86_64-pc-linux-gnu
checking host system type... x86_64-pc-linux-gnu
...
configure: creating ./config.status
config.status: creating GNUmakefile
config.status: creating src/Makefile.global
config.status: creating src/include/pg_config.h
```

**Common Issues:**
- If you see "configure: error: readline library not found", install libreadline-dev
- If you see "configure: error: zlib library not found", install zlib1g-dev

### Step 3: Compile the Source Code

Build PostgreSQL using multiple cores for faster compilation:

```bash
# Compile using 4 parallel jobs (adjust -j based on your CPU cores)
make -j4

# This process may take 5-15 minutes depending on your system
```

**Expected Output:**
```
make -C src all
make[1]: Entering directory '/path/to/postgresql_12_22_debug/src'
...
All of PostgreSQL successfully made. Ready to install.
```

**Success Indicator:** You should see the message "All of PostgreSQL successfully made."

### Step 4: Install PostgreSQL

Install the compiled binaries:

```bash
# Install to the directory specified in --prefix
sudo make install

# Optionally, install documentation
sudo make install-docs
```

**Expected Output:**
```
/bin/mkdir -p '/usr/local/pgsql/bin'
/usr/bin/install -c  postgres '/usr/local/pgsql/bin/postgres'
...
PostgreSQL installation complete.
```

### Step 5: Initialize the Database Cluster

Create a new PostgreSQL database cluster:

```bash
# Add PostgreSQL bin directory to PATH (add to ~/.bashrc for persistence)
export PATH=/usr/local/pgsql/bin:$PATH

# Create a data directory
mkdir -p ~/pgdata

# Initialize the database cluster
/usr/local/pgsql/bin/initdb -D ~/pgdata
```

**Expected Output:**
```
The files belonging to this database system will be owned by user "username".
This user must also own the server process.

The database cluster will be initialized with locale "en_US.UTF-8".
The default database encoding has accordingly been set to "UTF8".
...
Success. You can now start the database server using:

    pg_ctl -D ~/pgdata -l logfile start
```

### Step 6: Start PostgreSQL Service

Start the database server:

```bash
# Start PostgreSQL
/usr/local/pgsql/bin/pg_ctl -D ~/pgdata -l ~/pgdata/logfile start

# Wait a few seconds for startup
sleep 3

# Verify the server is running
/usr/local/pgsql/bin/pg_ctl -D ~/pgdata status
```

**Expected Output:**
```
waiting for server to start.... done
server started
```

### Step 7: Connect to Database and Verify Installation

Create a test database and verify the Outline feature is available:

```bash
# Create a test database
/usr/local/pgsql/bin/createdb testdb

# Connect to the database
/usr/local/pgsql/bin/psql testdb
```

**In psql, verify the Outline feature:**

```sql
-- Check if pg_outline catalog exists
SELECT * FROM pg_catalog.pg_class WHERE relname = 'pg_outline';

-- Verify Outline functions are available
\df pg_create_outline
\df pg_drop_outline
\df pg_enable_outline
\df pg_disable_outline

-- Check Outline GUC parameters
SHOW outline.display_hints;
SHOW outline.recording_mode;
```

**Expected Output:**
```sql
-- pg_outline catalog should exist
 relname    | relnamespace | reltype | relowner | ...
------------+--------------+---------+----------+-----
 pg_outline |           11 |   xxxxx |       10 | ...

-- Functions should be listed
 Schema |       Name        | Result data type | Argument data types | Type
--------+-------------------+------------------+---------------------+------
 public | pg_create_outline | oid              | name, text, text    | func
 ...

-- GUC parameters should show default values
 outline.display_hints
-----------------------
 off

 outline.recording_mode
------------------------
 off
```

### Step 8: Create Test Data

Create tables and insert sample data for testing:

```sql
-- Create customers table
CREATE TABLE customers (
    id INT PRIMARY KEY,
    region VARCHAR(100),
    name VARCHAR(100)
);

-- Create orders table
CREATE TABLE orders (
    order_id INT PRIMARY KEY,
    customer_id INT,
    amount DECIMAL
);

-- Create indexes for testing different scan methods
CREATE INDEX idx_customer_region ON customers(region);
CREATE INDEX idx_order_customer ON orders(customer_id);

-- Insert test data (1000 customers)
INSERT INTO customers
SELECT i, 'region_' || (i % 10), 'customer_' || i
FROM generate_series(1, 1000) i;

-- Insert test data (5000 orders)
INSERT INTO orders
SELECT i, (i % 1000) + 1, random() * 100
FROM generate_series(1, 5000) i;

-- Verify data insertion
SELECT COUNT(*) FROM customers;  -- Should return 1000
SELECT COUNT(*) FROM orders;     -- Should return 5000
```

**Expected Output:**
```
CREATE TABLE
CREATE TABLE
CREATE INDEX
CREATE INDEX
INSERT 0 1000
INSERT 0 5000
 count
-------
  1000
(1 row)

 count
-------
  5000
(1 row)
```

### Step 9: Test Basic Outline Functionality

Now let's run comprehensive tests of the Outline feature:

#### Test 1: Create Manual Outline with IndexScan Hint

```sql
-- First, see the default plan (might use SeqScan for small tables)
EXPLAIN SELECT * FROM customers WHERE region = 'region_5';

-- Create an outline that forces IndexScan
SELECT pg_create_outline(
    'test_outline_1',
    'SELECT * FROM customers WHERE region = $1',
    'IndexScan(customers idx_customer_region)'
);

-- Check the outline was created
SELECT outlinename, outlineenabled, outlinehints
FROM pg_outline
WHERE outlinename = 'test_outline_1';

-- Execute the query and verify IndexScan is used
EXPLAIN SELECT * FROM customers WHERE region = 'region_5';
```

**Expected Output:**
```sql
-- Default plan (before outline)
                                QUERY PLAN
--------------------------------------------------------------------------
 Seq Scan on customers  (cost=0.00..20.00 rows=100 width=440)
   Filter: ((region)::text = 'region_5'::text)

-- Outline creation
 pg_create_outline
-------------------
             xxxxx
(1 row)

-- Outline details
  outlinename   | outlineenabled |               outlinehints
----------------+----------------+------------------------------------------
 test_outline_1 | t              | IndexScan(customers idx_customer_region)

-- Plan after outline (IndexScan is now forced)
                                         QUERY PLAN
--------------------------------------------------------------------------------------------
 Index Scan using idx_customer_region on customers  (cost=0.27..8.29 rows=100 width=440)
   Index Cond: ((region)::text = 'region_5'::text)
```

#### Test 2: Test outline.display_hints Feature

```sql
-- Enable hint display
SET outline.display_hints = on;

-- Execute a query and see the generated hints
SELECT * FROM customers WHERE region = 'region_5' LIMIT 5;
```

**Expected Output:**
```
NOTICE:  Outline Data:
/*+
BEGIN_OUTLINE_DATA
IndexScan(customers idx_customer_region)
END_OUTLINE_DATA
*/

(5 rows showing customer data)
```

#### Test 3: Test Recording Mode (Auto-Capture)

```sql
-- Enable recording mode
SET outline.recording_mode = on;
SET outline.display_hints = on;

-- Execute a new query (this will auto-create an outline)
SELECT c.name, o.amount
FROM customers c
JOIN orders o ON c.id = o.customer_id
WHERE c.region = 'region_1'
LIMIT 5;

-- Check auto-generated outline
SELECT outlinename, outlineenabled, outlinehints
FROM pg_outline
WHERE outlinename LIKE 'auto_outline_%'
ORDER BY oid DESC
LIMIT 1;
```

**Expected Output:**
```
NOTICE:  Outline Data:
/*+
BEGIN_OUTLINE_DATA
IndexScan(customers idx_customer_region)
SeqScan(orders)
END_OUTLINE_DATA
*/
NOTICE:  Created outline "auto_outline_xxxxx_1" for query

-- Auto-generated outline details
      outlinename      | outlineenabled |               outlinehints
-----------------------+----------------+------------------------------------------
 auto_outline_12345_1  | t              | IndexScan(customers idx_customer_region)
                       |                | SeqScan(orders)
```

#### Test 4: Test Outline Management Operations

```sql
-- Disable recording mode
SET outline.recording_mode = off;
SET outline.display_hints = off;

-- Test disabling an outline
SELECT pg_disable_outline('test_outline_1');

-- Verify it's disabled
SELECT outlinename, outlineenabled FROM pg_outline WHERE outlinename = 'test_outline_1';

-- Test re-enabling
SELECT pg_enable_outline('test_outline_1');

-- Verify it's enabled again
SELECT outlinename, outlineenabled FROM pg_outline WHERE outlinename = 'test_outline_1';
```

**Expected Output:**
```
 pg_disable_outline
--------------------

(1 row)

  outlinename   | outlineenabled
----------------+----------------
 test_outline_1 | f

 pg_enable_outline
-------------------

(1 row)

  outlinename   | outlineenabled
----------------+----------------
 test_outline_1 | t
```

#### Test 5: Test Multi-Hint Outline (OceanBase Format)

```sql
-- Create outline with multiple hints using OceanBase format
SELECT pg_create_outline(
    'test_outline_join',
    'SELECT * FROM customers c JOIN orders o ON c.id = o.customer_id WHERE c.region = $1',
    '/*+
    BEGIN_OUTLINE_DATA
    IndexScan(customers idx_customer_region)
    HashJoin(customers orders)
    END_OUTLINE_DATA
    */'
);

-- Check the outline
SELECT outlinename, outlinehints
FROM pg_outline
WHERE outlinename = 'test_outline_join';

-- Test with EXPLAIN
EXPLAIN SELECT * FROM customers c
JOIN orders o ON c.id = o.customer_id
WHERE c.region = 'region_3';
```

**Expected Output:**
```
 pg_create_outline
-------------------
             xxxxx

      outlinename      |               outlinehints
-----------------------+------------------------------------------
 test_outline_join     | IndexScan(customers idx_customer_region)
                       | HashJoin(customers orders)

                                         QUERY PLAN
--------------------------------------------------------------------------------------------
 Hash Join  (cost=...
   Hash Cond: (o.customer_id = c.id)
   ->  Seq Scan on orders o  (cost=...
   ->  Hash  (cost=...
         ->  Index Scan using idx_customer_region on customers c  (cost=...
               Index Cond: ((region)::text = 'region_3'::text)
```

#### Test 6: Test Negative Hints (NoSeqScan)

```sql
-- Force index usage by disabling SeqScan
SELECT pg_create_outline(
    'test_outline_no_seq',
    'SELECT * FROM customers WHERE id > $1',
    'NoSeqScan(customers)'
);

-- Verify the outline forces index scan
EXPLAIN SELECT * FROM customers WHERE id > 500;
```

**Expected Output:**
```
                                         QUERY PLAN
--------------------------------------------------------------------------------------------
 Index Scan using customers_pkey on customers  (cost=0.28..28.55 rows=500 width=440)
   Index Cond: (id > 500)
```

### Step 10: Advanced Testing - Query Normalization

Test that the Outline system properly matches queries regardless of whitespace and case:

```sql
-- Original query
EXPLAIN SELECT * FROM customers WHERE region = 'region_5';

-- Same query with different whitespace (should use same outline)
EXPLAIN SELECT   *   FROM   customers   WHERE   region='region_5';

-- Same query with different case (should use same outline)
EXPLAIN SELECT * FROM CUSTOMERS WHERE REGION = 'region_5';

-- All three should show the same Index Scan plan from test_outline_1
```

### Troubleshooting

#### Issue 1: "relation 'pg_outline' does not exist"

**Cause:** The Outline feature is not properly compiled or installed.

**Solution:**
1. Check that you compiled from the correct source with Outline support
2. Verify the catalog was created: `SELECT * FROM pg_catalog.pg_class WHERE relname = 'pg_outline';`
3. If missing, rebuild and reinstall from source

#### Issue 2: Outline functions not found

**Cause:** Functions were not registered during installation.

**Solution:**
```sql
-- Check pg_proc for the functions
SELECT proname FROM pg_proc WHERE proname LIKE 'pg_%outline%';

-- If missing, rebuild and reinstall
```

#### Issue 3: Outline not being applied

**Cause:** Multiple possible reasons:
1. Outline is disabled
2. Query text doesn't match exactly
3. Tables/indexes referenced in hints don't exist

**Solution:**
```sql
-- Check outline status
SELECT outlinename, outlineenabled FROM pg_outline;

-- Enable if disabled
SELECT pg_enable_outline('your_outline_name');

-- Check query normalization
-- The system normalizes: lowercase, whitespace collapse
```

#### Issue 4: Permission denied errors

**Cause:** Insufficient privileges to create outlines.

**Solution:**
```sql
-- Outlines are owned by the creating user
-- Ensure you have CREATE privilege in the schema
GRANT CREATE ON SCHEMA public TO your_user;
```

### Cleanup Test Data

After testing, you can clean up the test environment:

```sql
-- Drop all test outlines
SELECT pg_drop_outline('test_outline_1');
SELECT pg_drop_outline('test_outline_join');
SELECT pg_drop_outline('test_outline_no_seq');

-- Drop auto-generated outlines
DO $$
DECLARE
    outline_rec RECORD;
BEGIN
    FOR outline_rec IN
        SELECT outlinename FROM pg_outline WHERE outlinename LIKE 'auto_outline_%'
    LOOP
        EXECUTE 'SELECT pg_drop_outline(' || quote_literal(outline_rec.outlinename) || ')';
    END LOOP;
END $$;

-- Drop test tables
DROP TABLE IF EXISTS orders;
DROP TABLE IF EXISTS customers;

-- Verify cleanup
SELECT COUNT(*) FROM pg_outline;  -- Should return 0
```

### Next Steps

After completing these tests, you can:

1. **Read the detailed documentation**: See sections below for architecture details and advanced features
2. **Explore recording mode**: Check `RECORDING_MODE_TEST.md` for advanced recording mode examples
3. **Try real workloads**: Apply Outlines to your actual queries
4. **Monitor performance**: Use EXPLAIN ANALYZE to measure the impact of Outlines

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
  - **name**: Outline name
  - **query**: Normalized SQL query text (supports parameter placeholders $1, $2, etc.)
  - **hints**: Hint string, supports two formats:
    1. OceanBase/Oracle format: `/*+ BEGIN_OUTLINE_DATA ... END_OUTLINE_DATA */`
    2. Traditional format: `E'hint1\nhint2\nhint3'` or `'hint1'` (single hint)
- `pg_drop_outline(name)` - Drop an existing outline
- `pg_enable_outline(name)` - Enable an outline
- `pg_disable_outline(name)` - Disable an outline

**Hint Format Conversion**: When using OceanBase/Oracle format, the system automatically extracts content between `BEGIN_OUTLINE_DATA` and `END_OUTLINE_DATA`, removing the comment wrapper, allowing Outline Data to be used directly for outline creation.

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

Two formats are supported for specifying multiple hints:

**Method 1: OceanBase/Oracle Style (Recommended)**

```sql
-- Use OceanBase Outline Data format
SELECT pg_create_outline(
    'outline_complex_query',
    'SELECT * FROM customers c JOIN orders o ON c.id = o.customer_id WHERE c.region = $1',
    '/*+
    BEGIN_OUTLINE_DATA
    IndexScan(customers idx_customer_region)
    HashJoin(customers orders)
    END_OUTLINE_DATA
    */'
);
```

**Method 2: Traditional E-String Format**

```sql
-- Use PostgreSQL's E-string syntax
SELECT pg_create_outline(
    'outline_complex_query_2',
    'SELECT * FROM customers c JOIN orders o ON c.id = o.customer_id WHERE c.region = $1',
    E'IndexScan(customers idx_customer_region)\nHashJoin(customers orders)'
);
```

**Explanation**:
- First hint: Use index `idx_customer_region` on the `customers` table
- Second hint: Use hash join between `customers` and `orders`
- **OceanBase format is recommended**: More readable and can be directly copied from `outline.display_hints` output
- Traditional E-string format is still supported for backward compatibility

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
