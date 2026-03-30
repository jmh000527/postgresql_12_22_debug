-- pg_outline extension

CREATE TABLE pg_catalog.pg_outline_query_hints
(
    outline_name      text    NOT NULL,
    normalized_query  text    NOT NULL,
    query_name        text    NOT NULL,
    hints             text    NOT NULL,
    created_at        timestamptz DEFAULT now()
);

CREATE UNIQUE INDEX pg_outline_query_hints_name_idx
    ON pg_catalog.pg_outline_query_hints(outline_name, query_name);

CREATE FUNCTION pg_catalog.pg_outline_create(outline_name text,
                                             sql text,
                                             query_name text,
                                             hints text)
RETURNS void
LANGUAGE C
AS 'MODULE_PATHNAME', 'pg_outline_create';

CREATE FUNCTION pg_catalog.pg_outline_delete(outline_name text)
RETURNS void
LANGUAGE C
AS 'MODULE_PATHNAME', 'pg_outline_delete';

CREATE FUNCTION pg_catalog.pg_outline_list()
RETURNS TABLE(outline_name text, query_name text, normalized_query text, hints text, created_at timestamptz)
LANGUAGE C
AS 'MODULE_PATHNAME', 'pg_outline_list';
