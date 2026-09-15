-- Bank feed connections and commerce imports. Complete family or none when
-- the optional modules are disabled; no financial history is inferred.
CREATE TEMP TABLE connectors_upgrade_check (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO connectors_upgrade_check(valid)
SELECT CASE WHEN COUNT(*) IN (0, 1) THEN 1 ELSE 0 END
FROM information_schema.tables
 WHERE table_schema = current_schema() AND table_type = 'BASE TABLE'
 AND table_name IN ('bank_connections');
DROP TABLE connectors_upgrade_check;
