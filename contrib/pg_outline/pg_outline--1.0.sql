/* contrib/pg_outline/pg_outline--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_outline" to load this file. \quit

-- Create schema
CREATE SCHEMA pg_outline;

-- Set search path for the extension
SET search_path = pg_outline, pg_catalog;

-- Outline catalog table
-- Stores outline definitions with their associated SQL and hints
CREATE TABLE pg_outline.outlines (
    id          SERIAL PRIMARY KEY,
    name        TEXT NOT NULL UNIQUE,
    schema_name TEXT NOT NULL DEFAULT 'public',
    query_text  TEXT NOT NULL,
    hints       TEXT,
    enabled     BOOLEAN NOT NULL DEFAULT true,
    description TEXT,
    created_at  TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at  TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE (name, schema_name)
);

-- Index for fast lookup by name and schema
CREATE INDEX idx_outlines_name_schema ON pg_outline.outlines(name, schema_name);
CREATE INDEX idx_outlines_enabled ON pg_outline.outlines(enabled) WHERE enabled = true;

-- Query hint mappings table
-- For per-query hints in complex queries (CTEs, subqueries)
CREATE TABLE pg_outline.query_hints (
    outline_id  INTEGER NOT NULL REFERENCES pg_outline.outlines(id) ON DELETE CASCADE,
    query_name  TEXT NOT NULL,
    hints       TEXT NOT NULL,
    PRIMARY KEY (outline_id, query_name)
);

-- Configuration parameters table
CREATE TABLE pg_outline.config (
    param_name  TEXT PRIMARY KEY,
    param_value TEXT NOT NULL,
    description TEXT
);

-- Insert default configuration
INSERT INTO pg_outline.config (param_name, param_value, description) VALUES
    ('enabled', 'true', 'Enable or disable outline functionality globally'),
    ('auto_generate', 'false', 'Automatically generate outlines from execution plans'),
    ('display_hints', 'false', 'Display applied hints in EXPLAIN output'),
    ('log_level', 'notice', 'Logging level: debug, notice, warning, error'),
    ('match_mode', 'exact', 'Query matching mode: exact, normalized, fingerprint');

-- Function: Create outline from SQL and hints
CREATE FUNCTION pg_outline.create_outline(
    p_name TEXT,
    p_query_text TEXT,
    p_hints TEXT DEFAULT NULL,
    p_schema_name TEXT DEFAULT 'public',
    p_description TEXT DEFAULT NULL,
    p_enabled BOOLEAN DEFAULT true
)
RETURNS INTEGER
AS 'MODULE_PATHNAME', 'pg_outline_create'
LANGUAGE C STRICT;

-- Function: Drop outline by name
CREATE FUNCTION pg_outline.drop_outline(
    p_name TEXT,
    p_schema_name TEXT DEFAULT 'public'
)
RETURNS BOOLEAN
AS 'MODULE_PATHNAME', 'pg_outline_drop'
LANGUAGE C STRICT;

-- Function: Enable outline
CREATE FUNCTION pg_outline.enable_outline(
    p_name TEXT,
    p_schema_name TEXT DEFAULT 'public'
)
RETURNS BOOLEAN
AS 'MODULE_PATHNAME', 'pg_outline_enable'
LANGUAGE C STRICT;

-- Function: Disable outline
CREATE FUNCTION pg_outline.disable_outline(
    p_name TEXT,
    p_schema_name TEXT DEFAULT 'public'
)
RETURNS BOOLEAN
AS 'MODULE_PATHNAME', 'pg_outline_disable'
LANGUAGE C STRICT;

-- Function: Create outline from current plan
CREATE FUNCTION pg_outline.create_outline_from_plan(
    p_name TEXT,
    p_query_text TEXT,
    p_schema_name TEXT DEFAULT 'public',
    p_description TEXT DEFAULT NULL
)
RETURNS INTEGER
AS 'MODULE_PATHNAME', 'pg_outline_create_from_plan'
LANGUAGE C STRICT;

-- Function: Match and apply hints for a query
CREATE FUNCTION pg_outline.match_query(
    p_query_text TEXT,
    p_schema_name TEXT DEFAULT 'public'
)
RETURNS TABLE(outline_name TEXT, hints TEXT)
AS 'MODULE_PATHNAME', 'pg_outline_match_query'
LANGUAGE C STRICT;

-- Function: Inject hints into SQL text
-- Takes query text and hint string, returns SQL with hints inserted as /*+ ... */ comments
-- Supports multi-query hints with [query_name] prefix format
CREATE FUNCTION pg_outline.inject_hints(
    p_query_text TEXT,
    p_hints TEXT
)
RETURNS TEXT
AS 'MODULE_PATHNAME', 'pg_outline_inject_hints'
LANGUAGE C STRICT;

-- Function: Get outline statistics
CREATE FUNCTION pg_outline.get_statistics()
RETURNS TABLE(
    total_outlines BIGINT,
    enabled_outlines BIGINT,
    disabled_outlines BIGINT
)
AS $$
SELECT
    COUNT(*)::BIGINT AS total_outlines,
    COUNT(*) FILTER (WHERE enabled = true)::BIGINT AS enabled_outlines,
    COUNT(*) FILTER (WHERE enabled = false)::BIGINT AS disabled_outlines
FROM pg_outline.outlines;
$$ LANGUAGE SQL;

-- View: Active outlines
CREATE VIEW pg_outline.active_outlines AS
SELECT
    id,
    name,
    schema_name,
    query_text,
    hints,
    description,
    created_at,
    updated_at
FROM pg_outline.outlines
WHERE enabled = true;

-- Grant permissions
GRANT USAGE ON SCHEMA pg_outline TO PUBLIC;
GRANT SELECT ON ALL TABLES IN SCHEMA pg_outline TO PUBLIC;
GRANT EXECUTE ON ALL FUNCTIONS IN SCHEMA pg_outline TO PUBLIC;
