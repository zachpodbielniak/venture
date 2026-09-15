-- Bank feed connections and commerce imports. Complete family or none when
-- the optional modules are disabled; no financial history is inferred.
CREATE TEMP TABLE connectors_upgrade_check (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO connectors_upgrade_check(valid)
SELECT CASE WHEN COUNT(*) IN (0, 1) THEN 1 ELSE 0 END
FROM sqlite_master WHERE type = 'table' AND name IN ('bank_connections');
DROP TABLE connectors_upgrade_check;
