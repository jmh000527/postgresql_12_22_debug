-- Smoke test: create and list outline
SELECT pg_outline_create('smoke', 'SELECT 1', 'main', 'SeqScan(t)'); -- query_name ignored here
SELECT outline_name, query_name, hints FROM pg_outline_list() ORDER BY outline_name, query_name;
SELECT pg_outline_delete('smoke');
