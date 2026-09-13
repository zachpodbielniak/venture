-- Keep the metadata backfill pending while pipelines is disabled. Startup
-- consumes this obligation atomically with default stages and deal history.
-- The queue exists independently of optional CRM tables.
CREATE TABLE venture_pipeline_upgrade (pending INTEGER NOT NULL);
INSERT INTO venture_pipeline_upgrade (pending) VALUES (1);
