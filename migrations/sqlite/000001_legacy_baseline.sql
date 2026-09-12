-- Preserve the legacy schema marker while adopting checksummed migrations.
CREATE TABLE IF NOT EXISTS venture_schema_version (version INTEGER NOT NULL);
INSERT INTO venture_schema_version (version)
SELECT 1 WHERE NOT EXISTS (SELECT 1 FROM venture_schema_version);
