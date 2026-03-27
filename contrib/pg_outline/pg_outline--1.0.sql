/* contrib/pg_outline/pg_outline--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_outline" to load this file. \quit

-- Create outline catalog table
CREATE TABLE IF NOT EXISTS pg_outline_data (
    outline_id SERIAL PRIMARY KEY,
    outline_name TEXT NOT NULL,
    query_pattern TEXT NOT NULL,
    fingerprint TEXT NOT NULL UNIQUE,
    hint_string TEXT NOT NULL,
    enabled BOOLEAN DEFAULT true,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Create index on fingerprint for faster lookups
CREATE INDEX IF NOT EXISTS pg_outline_fingerprint_idx ON pg_outline_data(fingerprint);

-- Create index on outline name for lookups
CREATE INDEX IF NOT EXISTS pg_outline_name_idx ON pg_outline_data(outline_name);

-- Create index on enabled status
CREATE INDEX IF NOT EXISTS pg_outline_enabled_idx ON pg_outline_data(enabled) WHERE enabled = true;

-- Create table to store per-Query hints
-- Each outline can have multiple Query structures, each with its own hints
CREATE TABLE IF NOT EXISTS pg_outline_query_hints (
    hint_id SERIAL PRIMARY KEY,
    outline_id INTEGER NOT NULL REFERENCES pg_outline_data(outline_id) ON DELETE CASCADE,
    query_index INTEGER NOT NULL,  -- Index of Query in the parse tree (0=top-level)
    hint_string TEXT NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(outline_id, query_index)
);

-- Create index for faster lookups
CREATE INDEX IF NOT EXISTS pg_outline_query_hints_outline_idx ON pg_outline_query_hints(outline_id);

-- Function to create an outline
CREATE OR REPLACE FUNCTION pg_outline_create(
    outline_name TEXT,
    query_text TEXT,
    hints TEXT DEFAULT NULL
)
RETURNS BOOLEAN
AS 'MODULE_PATHNAME', 'pg_outline_create'
LANGUAGE C;

-- Simplified function to create an outline from SQL with inline hints
CREATE OR REPLACE FUNCTION pg_outline_create_from_sql(
    outline_name TEXT,
    query_with_hints TEXT
)
RETURNS BOOLEAN
AS 'MODULE_PATHNAME', 'pg_outline_create_from_sql'
LANGUAGE C STRICT;

-- Function to update an existing outline
CREATE OR REPLACE FUNCTION pg_outline_update(
    outline_name TEXT,
    query_text TEXT,
    hints TEXT DEFAULT NULL
)
RETURNS BOOLEAN
AS 'MODULE_PATHNAME', 'pg_outline_update'
LANGUAGE C;

-- Function to drop an outline
CREATE OR REPLACE FUNCTION pg_outline_drop(
    outline_name TEXT
)
RETURNS BOOLEAN
AS 'MODULE_PATHNAME', 'pg_outline_drop'
LANGUAGE C STRICT;

-- Function to enable an outline
CREATE OR REPLACE FUNCTION pg_outline_enable(
    outline_name TEXT
)
RETURNS BOOLEAN
AS 'MODULE_PATHNAME', 'pg_outline_enable'
LANGUAGE C STRICT;

-- Function to disable an outline
CREATE OR REPLACE FUNCTION pg_outline_disable(
    outline_name TEXT
)
RETURNS BOOLEAN
AS 'MODULE_PATHNAME', 'pg_outline_disable'
LANGUAGE C STRICT;

-- Function to list all outlines
CREATE OR REPLACE FUNCTION pg_outline_list()
RETURNS TABLE (
    outline_id INTEGER,
    outline_name TEXT,
    query_pattern TEXT,
    fingerprint TEXT,
    hint_string TEXT,
    enabled BOOLEAN,
    created_at TIMESTAMP,
    updated_at TIMESTAMP
)
AS $$
    SELECT outline_id, outline_name, query_pattern, fingerprint, hint_string,
           enabled, created_at, updated_at
    FROM pg_outline_data
    ORDER BY outline_id;
$$ LANGUAGE SQL;

-- Create a view for easy access to enabled outlines
CREATE OR REPLACE VIEW pg_outline_enabled AS
    SELECT outline_id, outline_name, query_pattern, fingerprint, hint_string, created_at, updated_at
    FROM pg_outline_data
    WHERE enabled = true;

GRANT SELECT ON pg_outline_enabled TO PUBLIC;
