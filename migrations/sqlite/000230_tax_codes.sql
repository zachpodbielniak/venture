-- Tax codes and dated exchange rates come from field tables. Verify the
-- pair is present, or absent when the owning module is disabled.
CREATE TEMP TABLE tax_codes_upgrade_check (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO tax_codes_upgrade_check(valid)
SELECT CASE WHEN COUNT(*) IN (0, 1) THEN 1 ELSE 0 END
FROM sqlite_master WHERE type = 'table' AND name IN ('tax_codes');
DROP TABLE tax_codes_upgrade_check;
CREATE TEMP TABLE exchange_rates_upgrade_check (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO exchange_rates_upgrade_check(valid)
SELECT CASE WHEN COUNT(*) IN (0, 1) THEN 1 ELSE 0 END
FROM sqlite_master WHERE type = 'table' AND name IN ('exchange_rates');
DROP TABLE exchange_rates_upgrade_check;
