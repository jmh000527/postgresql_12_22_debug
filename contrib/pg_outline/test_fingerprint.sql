-- Test fingerprint matching

\echo 'Testing fingerprint calculation...'
\echo ''

-- Test 1: Check what fingerprint is generated from our pattern
\echo 'Pattern fingerprint:'
SELECT pg_outline_fingerprint('select * from t1 where c1 in (select c2 from t2 where c2 < ?)');

-- Test 2: Check what fingerprint is generated from our actual query
\echo 'Actual query fingerprint:'
SELECT pg_outline_fingerprint('select * from t1 where c1 in (select c2 from t2 where c2 < 5)');

-- Test 3: Check what the normalized version looks like
\echo 'Normalized query:'
SELECT pg_outline_normalize('select * from t1 where c1 in (select c2 from t2 where c2 < 5)');

\echo ''
\echo 'Now lets check stored outlines:'
SELECT outline_name, fingerprint, query_pattern FROM pg_outline_data;
