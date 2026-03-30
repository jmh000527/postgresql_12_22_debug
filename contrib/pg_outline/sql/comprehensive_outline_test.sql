-- ============================================================================
-- Comprehensive Outline Test Suite
-- Tests pg_outline functionality with various SQL query types
-- ============================================================================

-- Setup
\set ECHO all
\set ON_ERROR_STOP on

-- Drop and recreate extension for clean test
DROP EXTENSION IF EXISTS pg_outline CASCADE;
CREATE EXTENSION pg_outline;

-- Configure pg_outline
SET pg_outline.enabled = true;
SET pg_outline.mode = 'auto';
SET client_min_messages = NOTICE;

-- ============================================================================
-- Test Data Setup
-- ============================================================================

-- Create test tables
CREATE TABLE customers (
    customer_id INTEGER PRIMARY KEY,
    customer_name TEXT NOT NULL,
    city TEXT,
    country TEXT,
    credit_limit DECIMAL(10,2)
);

CREATE TABLE orders (
    order_id INTEGER PRIMARY KEY,
    customer_id INTEGER REFERENCES customers(customer_id),
    order_date DATE,
    total_amount DECIMAL(10,2),
    status TEXT
);

CREATE TABLE order_items (
    item_id INTEGER PRIMARY KEY,
    order_id INTEGER REFERENCES orders(order_id),
    product_name TEXT,
    quantity INTEGER,
    unit_price DECIMAL(10,2)
);

CREATE TABLE products (
    product_id INTEGER PRIMARY KEY,
    product_name TEXT NOT NULL,
    category TEXT,
    price DECIMAL(10,2),
    stock_quantity INTEGER
);

-- Insert test data
INSERT INTO customers VALUES
    (1, 'Alice', 'New York', 'USA', 10000.00),
    (2, 'Bob', 'London', 'UK', 15000.00),
    (3, 'Charlie', 'Paris', 'France', 20000.00),
    (4, 'David', 'Tokyo', 'Japan', 25000.00),
    (5, 'Eve', 'Sydney', 'Australia', 12000.00);

INSERT INTO orders VALUES
    (101, 1, '2024-01-15', 1500.00, 'completed'),
    (102, 2, '2024-01-20', 2500.00, 'completed'),
    (103, 1, '2024-02-10', 3000.00, 'pending'),
    (104, 3, '2024-02-15', 1800.00, 'completed'),
    (105, 4, '2024-03-01', 4500.00, 'pending');

INSERT INTO order_items VALUES
    (1, 101, 'Laptop', 1, 1200.00),
    (2, 101, 'Mouse', 2, 150.00),
    (3, 102, 'Keyboard', 3, 800.00),
    (4, 103, 'Monitor', 2, 1500.00),
    (5, 104, 'Tablet', 1, 1800.00),
    (6, 105, 'Phone', 3, 1500.00);

INSERT INTO products VALUES
    (1, 'Laptop', 'Electronics', 1200.00, 50),
    (2, 'Mouse', 'Accessories', 75.00, 200),
    (3, 'Keyboard', 'Accessories', 150.00, 150),
    (4, 'Monitor', 'Electronics', 750.00, 80),
    (5, 'Tablet', 'Electronics', 1800.00, 30);

ANALYZE customers;
ANALYZE orders;
ANALYZE order_items;
ANALYZE products;

\echo '============================================================================'
\echo 'Test 1: Simple SELECT with WHERE clause'
\echo '============================================================================'

EXPLAIN SELECT * FROM customers WHERE city = 'New York';

\echo '============================================================================'
\echo 'Test 2: INNER JOIN'
\echo '============================================================================'

EXPLAIN SELECT c.customer_name, o.order_id, o.total_amount
FROM customers c
INNER JOIN orders o ON c.customer_id = o.customer_id
WHERE o.status = 'completed';

\echo '============================================================================'
\echo 'Test 3: LEFT JOIN'
\echo '============================================================================'

EXPLAIN SELECT c.customer_name, o.order_id
FROM customers c
LEFT JOIN orders o ON c.customer_id = o.customer_id
WHERE c.country = 'USA';

\echo '============================================================================'
\echo 'Test 4: Multiple JOINs'
\echo '============================================================================'

EXPLAIN SELECT c.customer_name, o.order_id, oi.product_name, oi.quantity
FROM customers c
JOIN orders o ON c.customer_id = o.customer_id
JOIN order_items oi ON o.order_id = oi.order_id
WHERE o.order_date >= '2024-02-01';

\echo '============================================================================'
\echo 'Test 5: Scalar Subquery in SELECT'
\echo '============================================================================'

EXPLAIN SELECT c.customer_name,
       (SELECT COUNT(*) FROM orders o WHERE o.customer_id = c.customer_id) as order_count
FROM customers c
LIMIT 10;

\echo '============================================================================'
\echo 'Test 6: Subquery with IN clause'
\echo '============================================================================'

EXPLAIN SELECT * FROM customers
WHERE customer_id IN (SELECT customer_id FROM orders WHERE total_amount > 2000);

\echo '============================================================================'
\echo 'Test 7: Subquery with EXISTS'
\echo '============================================================================'

EXPLAIN SELECT c.* FROM customers c
WHERE EXISTS (
    SELECT 1 FROM orders o
    WHERE o.customer_id = c.customer_id
    AND o.status = 'pending'
);

\echo '============================================================================'
\echo 'Test 8: Subquery with NOT EXISTS'
\echo '============================================================================'

EXPLAIN SELECT c.* FROM customers c
WHERE NOT EXISTS (
    SELECT 1 FROM orders o
    WHERE o.customer_id = c.customer_id
);

\echo '============================================================================'
\echo 'Test 9: Simple CTE (Common Table Expression)'
\echo '============================================================================'

EXPLAIN WITH customer_totals AS (
    SELECT customer_id, SUM(total_amount) as total
    FROM orders
    GROUP BY customer_id
)
SELECT c.customer_name, ct.total
FROM customers c
JOIN customer_totals ct ON c.customer_id = ct.customer_id;

\echo '============================================================================'
\echo 'Test 10: Multiple CTEs'
\echo '============================================================================'

EXPLAIN WITH
high_value_customers AS (
    SELECT customer_id FROM customers WHERE credit_limit > 15000
),
recent_orders AS (
    SELECT customer_id, order_id FROM orders WHERE order_date >= '2024-02-01'
)
SELECT c.customer_name, ro.order_id
FROM customers c
JOIN high_value_customers hvc ON c.customer_id = hvc.customer_id
JOIN recent_orders ro ON c.customer_id = ro.customer_id;

\echo '============================================================================'
\echo 'Test 11: Recursive CTE'
\echo '============================================================================'

EXPLAIN WITH RECURSIVE numbers AS (
    SELECT 1 as n
    UNION ALL
    SELECT n + 1 FROM numbers WHERE n < 10
)
SELECT * FROM numbers;

\echo '============================================================================'
\echo 'Test 12: GROUP BY with HAVING'
\echo '============================================================================'

EXPLAIN SELECT customer_id, COUNT(*) as order_count, SUM(total_amount) as total
FROM orders
GROUP BY customer_id
HAVING COUNT(*) > 1;

\echo '============================================================================'
\echo 'Test 13: Window Functions'
\echo '============================================================================'

EXPLAIN SELECT
    customer_name,
    credit_limit,
    ROW_NUMBER() OVER (ORDER BY credit_limit DESC) as rank,
    AVG(credit_limit) OVER () as avg_credit
FROM customers;

\echo '============================================================================'
\echo 'Test 14: UNION'
\echo '============================================================================'

EXPLAIN SELECT customer_name as name FROM customers WHERE country = 'USA'
UNION
SELECT customer_name as name FROM customers WHERE city = 'London';

\echo '============================================================================'
\echo 'Test 15: UNION ALL'
\echo '============================================================================'

EXPLAIN SELECT customer_id, 'customer' as type FROM customers
UNION ALL
SELECT order_id, 'order' as type FROM orders;

\echo '============================================================================'
\echo 'Test 16: INTERSECT'
\echo '============================================================================'

EXPLAIN SELECT customer_id FROM customers WHERE credit_limit > 10000
INTERSECT
SELECT customer_id FROM orders WHERE total_amount > 2000;

\echo '============================================================================'
\echo 'Test 17: EXCEPT'
\echo '============================================================================'

EXPLAIN SELECT customer_id FROM customers
EXCEPT
SELECT customer_id FROM orders WHERE status = 'cancelled';

\echo '============================================================================'
\echo 'Test 18: Complex Nested Query'
\echo '============================================================================'

EXPLAIN SELECT c.customer_name,
       o.order_id,
       (SELECT SUM(quantity * unit_price)
        FROM order_items oi
        WHERE oi.order_id = o.order_id) as order_total
FROM customers c
JOIN orders o ON c.customer_id = o.customer_id
WHERE c.customer_id IN (
    SELECT customer_id
    FROM orders
    GROUP BY customer_id
    HAVING COUNT(*) > 1
)
ORDER BY o.order_date DESC
LIMIT 5;

\echo '============================================================================'
\echo 'Test 19: Subquery in FROM clause'
\echo '============================================================================'

EXPLAIN SELECT avg_amount, COUNT(*)
FROM (
    SELECT customer_id, AVG(total_amount) as avg_amount
    FROM orders
    GROUP BY customer_id
) subq
GROUP BY avg_amount;

\echo '============================================================================'
\echo 'Test 20: ANY/ALL subquery'
\echo '============================================================================'

EXPLAIN SELECT * FROM customers
WHERE credit_limit > ALL (SELECT total_amount FROM orders WHERE status = 'completed');

\echo '============================================================================'
\echo 'Test 21: Self JOIN'
\echo '============================================================================'

EXPLAIN SELECT c1.customer_name as customer1, c2.customer_name as customer2
FROM customers c1
JOIN customers c2 ON c1.city = c2.city AND c1.customer_id < c2.customer_id;

\echo '============================================================================'
\echo 'Test 22: CROSS JOIN'
\echo '============================================================================'

EXPLAIN SELECT c.customer_name, p.product_name
FROM customers c
CROSS JOIN products p
WHERE c.country = 'USA' AND p.category = 'Electronics'
LIMIT 10;

\echo '============================================================================'
\echo 'Test 23: Aggregate with DISTINCT'
\echo '============================================================================'

EXPLAIN SELECT COUNT(DISTINCT country) as country_count,
       COUNT(DISTINCT city) as city_count
FROM customers;

\echo '============================================================================'
\echo 'Test 24: CASE expression with subquery'
\echo '============================================================================'

EXPLAIN SELECT customer_name,
       CASE
           WHEN credit_limit > (SELECT AVG(credit_limit) FROM customers)
           THEN 'High'
           ELSE 'Normal'
       END as credit_category
FROM customers;

\echo '============================================================================'
\echo 'Test 25: Correlated subquery'
\echo '============================================================================'

EXPLAIN SELECT c.customer_name,
       (SELECT MAX(o.total_amount)
        FROM orders o
        WHERE o.customer_id = c.customer_id) as max_order
FROM customers c;

\echo '============================================================================'
\echo 'Test Results Summary'
\echo '============================================================================'

-- List all generated outlines
SELECT COUNT(*) as total_outlines FROM pg_outline_data;

-- Show sample of generated outlines
SELECT outline_name,
       LEFT(query_pattern, 50) as query_preview,
       LEFT(hint_string, 100) as hints_preview
FROM pg_outline_data
LIMIT 10;

\echo '============================================================================'
\echo 'Cleanup'
\echo '============================================================================'

-- Clean up test data
DROP TABLE IF EXISTS order_items CASCADE;
DROP TABLE IF EXISTS orders CASCADE;
DROP TABLE IF EXISTS customers CASCADE;
DROP TABLE IF EXISTS products CASCADE;

-- Clean up outlines
DELETE FROM pg_outline_data;

\echo '============================================================================'
\echo 'All tests completed successfully!'
\echo '============================================================================'
